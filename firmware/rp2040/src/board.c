/* SPDX-License-Identifier: GPL-2.0-only
 *
 * RP2040 hardware glue: the two link UARTs, buttons, indication, and the
 * persistent chain key.
 */
#include "board.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "ws2812.pio.h"

/* ------------------------------------------------------------------ *
 * Identity
 * ------------------------------------------------------------------ */

static dhp_uid_t g_uid;

void board_uid_bytes(uint8_t out[8])
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(out, id.id, 8);
}

dhp_uid_t board_uid(void)
{
    return g_uid;
}

dhp_addr_t board_addr(void)
{
    /* Fold the 64-bit uid down to the 16-bit wire address. Zero and the
     * broadcast address are both reserved, so they are mapped away. */
    uint16_t a = (uint16_t)(g_uid ^ (g_uid >> 16) ^ (g_uid >> 32) ^ (g_uid >> 48));
    if (a == DHP_ADDR_NONE || a == DHP_ADDR_BROADCAST) {
        a = 1;
    }
    return a;
}

/* ------------------------------------------------------------------ *
 * Link ports
 *
 * Interrupt-driven ring buffers rather than blocking writes. A blocking UART
 * write in the HID path would add jitter to every keystroke, and at 2 Mbps a
 * full frame takes ~90 us -- long enough to matter at a 1 kHz report rate.
 * ------------------------------------------------------------------ */

#define RING_BITS 10
#define RING_SIZE (1u << RING_BITS)
#define RING_MASK (RING_SIZE - 1u)

typedef struct {
    uint8_t buf[RING_SIZE];
    volatile uint16_t head, tail;
} ring_t;

static bool ring_push(ring_t *r, uint8_t b)
{
    const uint16_t next = (uint16_t)((r->head + 1) & RING_MASK);
    if (next == r->tail) {
        return false; /* full: drop, and let the CRC/replay layers cope */
    }
    r->buf[r->head] = b;
    r->head = next;
    return true;
}

static bool ring_pop(ring_t *r, uint8_t *out)
{
    if (r->head == r->tail) {
        return false;
    }
    *out = r->buf[r->tail];
    r->tail = (uint16_t)((r->tail + 1) & RING_MASK);
    return true;
}

static ring_t g_rx[DHP_PORT_COUNT];
static ring_t g_tx[DHP_PORT_COUNT];

/* DHP_PORT_UP is uart0, DHP_PORT_DOWN is uart1. See docs/hardware.md. */
static uart_inst_t *port_uart(dhp_port_t p)
{
    return p == DHP_PORT_UP ? uart0 : uart1;
}

static void port_isr(dhp_port_t p)
{
    uart_inst_t *u = port_uart(p);

    while (uart_is_readable(u)) {
        ring_push(&g_rx[p], (uint8_t)uart_get_hw(u)->dr);
    }
    while (uart_is_writable(u)) {
        uint8_t b;
        if (!ring_pop(&g_tx[p], &b)) {
            /* Nothing left: stop asking to be told the FIFO has room, or the
             * interrupt will fire continuously on an idle link. */
            hw_clear_bits(&uart_get_hw(u)->imsc, UART_UARTIMSC_TXIM_BITS);
            break;
        }
        uart_get_hw(u)->dr = b;
    }
}

static void isr_uart0(void) { port_isr(DHP_PORT_UP); }
static void isr_uart1(void) { port_isr(DHP_PORT_DOWN); }

void board_uart_write(dhp_port_t port, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ring_push(&g_tx[port], data[i]);
    }
    /* Re-arm the transmit interrupt; the ISR disarms it when the queue drains. */
    hw_set_bits(&uart_get_hw(port_uart(port))->imsc, UART_UARTIMSC_TXIM_BITS);
}

bool board_uart_read(dhp_port_t port, uint8_t *out)
{
    return ring_pop(&g_rx[port], out);
}

static void link_init(void)
{
    uart_init(uart0, DHP_LINK_BAUD);
    gpio_set_function(PIN_UART0_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART0_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, true);

    uart_init(uart1, DHP_LINK_BAUD);
    gpio_set_function(PIN_UART1_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART1_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(uart1, false, false);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart1, true);

    irq_set_exclusive_handler(UART0_IRQ, isr_uart0);
    irq_set_exclusive_handler(UART1_IRQ, isr_uart1);
    irq_set_enabled(UART0_IRQ, true);
    irq_set_enabled(UART1_IRQ, true);

    /* Receive interrupts always; transmit only while there is something to
     * send. */
    uart_set_irq_enables(uart0, true, false);
    uart_set_irq_enables(uart1, true, false);
}

/* ------------------------------------------------------------------ *
 * Buttons
 * ------------------------------------------------------------------ */

#define DEBOUNCE_MS 25
#define PAIR_HOLD_MS 3000
/* Long enough that nobody reaches it by accident, short enough to be usable
 * when a pairing has gone wrong. That case is otherwise unrecoverable without
 * reflashing, and it fails silently -- two boards holding different keys just
 * refuse each other's frames. */
#define UNPAIR_HOLD_MS 10000

static bool       g_switch_last;
static dhp_time_t g_switch_change;
static dhp_time_t g_pair_down_since;
static bool       g_pair_reported;
static bool       g_unpair_reported;

bool board_switch_pressed(void)
{
    const bool down = !gpio_get(PIN_BTN_SWITCH); /* active low */
    const dhp_time_t now = board_now_ms();

    if (down != g_switch_last) {
        if (dhp_time_after(now, g_switch_change + DEBOUNCE_MS)) {
            g_switch_last = down;
            g_switch_change = now;
            return down; /* edge, on press only */
        }
    } else {
        g_switch_change = now;
    }
    return false;
}

board_pair_btn_t board_pair_button(void)
{
    const bool down = !gpio_get(PIN_BTN_PAIR);
    const dhp_time_t now = board_now_ms();

    if (!down) {
        g_pair_down_since = 0;
        g_pair_reported = false;
        g_unpair_reported = false;
        return BOARD_PAIR_BTN_NONE;
    }
    if (g_pair_down_since == 0) {
        g_pair_down_since = now;
        return BOARD_PAIR_BTN_NONE;
    }

    /* Longer hold checked first, so continuing to hold past the pairing
     * threshold reaches it rather than being swallowed by the shorter one. */
    if (!g_unpair_reported &&
        dhp_time_after(now, g_pair_down_since + UNPAIR_HOLD_MS)) {
        g_unpair_reported = true;
        return BOARD_PAIR_BTN_UNPAIR;
    }
    if (!g_pair_reported &&
        dhp_time_after(now, g_pair_down_since + PAIR_HOLD_MS)) {
        g_pair_reported = true;
        return BOARD_PAIR_BTN_PAIR;
    }
    return BOARD_PAIR_BTN_NONE;
}

/* ------------------------------------------------------------------ *
 * Indication
 *
 * A single WS2812 driven from PIO. Colours are chosen to be distinguishable
 * across a desk at a glance, and to encode focus and routing role separately
 * so a bench session can see both at once.
 * ------------------------------------------------------------------ */

#define LED_REFRESH_MS 16 /* ~60 Hz: smooth enough for the breathing state */

static PIO      g_led_pio;
static uint     g_led_sm;
static bool     g_led_ready;
static bool     g_led_unavailable;

static volatile led_state_t g_led_state = LED_OFF;
static dhp_time_t g_led_next;
static uint32_t   g_led_last_grb = 0xFFFFFFFFu; /* impossible: forces first write */

void board_led_hw_init(void)
{
    if (g_led_ready || g_led_unavailable) {
        return;
    }

    uint offset;
    if (!pio_claim_free_sm_and_add_program(&ws2812_program, &g_led_pio,
                                           &g_led_sm, &offset)) {
        /* PIO-USB got there first and there is no room left. Run without an
         * LED rather than fighting it for a state machine. */
        g_led_unavailable = true;
        return;
    }

    pio_gpio_init(g_led_pio, PIN_WS2812);
    pio_sm_set_consecutive_pindirs(g_led_pio, g_led_sm, PIN_WS2812, 1, true);

    pio_sm_config c = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PIN_WS2812);
    /* Shift left, autopull at 24 bits: one GRB pixel per FIFO word, with the
     * colour left-justified so the MSB goes out first as WS2812 expects. */
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    const int cycles_per_bit = ws2812_T1 + ws2812_T2 + ws2812_T3;
    const float div = (float)clock_get_hz(clk_sys) / (800000.0f * cycles_per_bit);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(g_led_pio, g_led_sm, offset, &c);
    pio_sm_set_enabled(g_led_pio, g_led_sm, true);

    g_led_ready = true;
}

void board_led(led_state_t s)
{
    /* Deliberately does no hardware work: this is called from router hooks,
     * which run in the middle of the HID path. */
    g_led_state = s;
}

static uint32_t grb(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 wants green first, and the state machine is configured to shift
     * out of the top of the 32-bit word. */
    return ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
}

static uint8_t scale(uint8_t v, uint8_t pct)
{
    return (uint8_t)(((uint32_t)v * pct) / 100u);
}

/* A triangle wave, so "breathing" needs no sine table. */
static uint8_t breathe(dhp_time_t now, uint32_t period_ms, uint8_t lo, uint8_t hi)
{
    const uint32_t phase = now % period_ms;
    const uint32_t half = period_ms / 2;
    const uint32_t up = phase < half ? phase : (period_ms - phase);
    return (uint8_t)(lo + ((uint32_t)(hi - lo) * up) / half);
}

static uint32_t colour_for(led_state_t s, dhp_time_t now)
{
    switch (s) {
    case LED_FOCUSED:
        return grb(120, 120, 120); /* white */
    case LED_ACTIVE:
        return grb(0, 0, 150); /* blue */
    case LED_ACTIVE_FOCUSED:
        return grb(0, 140, 140); /* cyan */
    case LED_STANDBY:
        return grb(0, 0, 20); /* dim blue */
    case LED_PAIRING: {
        /* Breathing, because pairing is a state the user is waiting inside
         * and a steady light gives no sign the window is still open. */
        const uint8_t v = breathe(now, 1600, 20, 255);
        return grb(v, scale(v, 55), 0); /* amber */
    }
    case LED_UNPAIRED:
        return grb(40, 22, 0); /* dim amber */
    case LED_ERROR:
        return (now % 300) < 150 ? grb(200, 0, 0) : 0; /* fast red blink */
    case LED_OFF:
    default:
        return 0;
    }
}

void board_led_task(void)
{
    if (!g_led_ready) {
        return;
    }

    const dhp_time_t now = board_now_ms();
    if (!dhp_time_after(now, g_led_next)) {
        return;
    }
    g_led_next = now + LED_REFRESH_MS;

    const uint32_t c = colour_for(g_led_state, now);

    /* Only push when something changed, so a static state costs nothing on
     * the wire and the FIFO is always free when it does change. */
    if (c == g_led_last_grb) {
        return;
    }
    if (pio_sm_is_tx_fifo_full(g_led_pio, g_led_sm)) {
        return; /* try again next tick rather than blocking the HID path */
    }

    pio_sm_put(g_led_pio, g_led_sm, c);
    g_led_last_grb = c;
}

/* ------------------------------------------------------------------ *
 * Persistent chain key
 *
 * The last flash sector. Note that the key is stored in plaintext: an RP2040
 * has no secure element and no way to keep a secret from someone holding the
 * board, so this protects against a device plugged into the chain, not
 * against someone with the board in their hand. docs/protocols/auth.md is
 * explicit about which threat is in scope.
 * ------------------------------------------------------------------ */

/* Writing flash stalls the XIP cache, so any core executing from flash at that
 * moment faults or hangs. Core 1 sits in a tight tuh_task() loop running
 * straight out of flash, so it must be parked for the duration -- interrupts
 * being disabled on core 0 does nothing for it. This is the classic RP2040
 * dual-core flash hazard, and it would fire exactly once: on the first
 * successful pairing, which is the first thing anyone tests. */
static volatile bool g_lockout_ready;

void board_flash_lockout_ready(void)
{
    /* Called ON core 1, once, before it starts its loop. */
    multicore_lockout_victim_init();
    g_lockout_ready = true;
}

static void flash_begin(void)
{
    if (g_lockout_ready) {
        multicore_lockout_start_blocking();
    }
}

static void flash_end(void)
{
    if (g_lockout_ready) {
        multicore_lockout_end_blocking();
    }
}

#define KEY_MAGIC 0x4448504bu /* "DHPK" */
#define KEY_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint8_t  key[16];
    uint32_t check; /* magic ^ key words, so a half-written record is rejected */
} key_record_t;

static uint32_t key_check(const uint8_t key[16])
{
    uint32_t c = KEY_MAGIC;
    for (int i = 0; i < 4; i++) {
        uint32_t w;
        memcpy(&w, key + i * 4, 4);
        c ^= w;
    }
    return c;
}

bool board_key_load(uint8_t key[16])
{
    const key_record_t *rec =
        (const key_record_t *)(XIP_BASE + KEY_OFFSET);

    if (rec->magic != KEY_MAGIC || rec->check != key_check(rec->key)) {
        return false;
    }
    memcpy(key, rec->key, 16);
    return true;
}

void board_key_save(const uint8_t key[16])
{
    key_record_t rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.magic = KEY_MAGIC;
    memcpy(rec.key, key, 16);
    rec.check = key_check(key);

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &rec, sizeof(rec));

    flash_begin();
    const uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KEY_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(KEY_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    flash_end();
}

void board_key_erase(void)
{
    flash_begin();
    const uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KEY_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    flash_end();
}

/* ------------------------------------------------------------------ *
 * Init and time
 * ------------------------------------------------------------------ */

dhp_time_t board_now_ms(void)
{
    return (dhp_time_t)(time_us_64() / 1000u);
}

void board_init(void)
{
    /* Before anything else. PIO-USB bit-bangs USB in software and its PIO
     * programs are written against a 120 MHz system clock; and doing it here
     * means uart_init() below computes its baud divisor from the final clock
     * rather than from the 125 MHz default. */
    set_sys_clock_khz(120000, true);

    uint8_t id[8];
    board_uid_bytes(id);
    g_uid = 0;
    for (int i = 0; i < 8; i++) {
        g_uid |= ((dhp_uid_t)id[i]) << (8 * i);
    }

    gpio_init(PIN_BTN_SWITCH);
    gpio_set_dir(PIN_BTN_SWITCH, GPIO_IN);
    gpio_pull_up(PIN_BTN_SWITCH);

    gpio_init(PIN_BTN_PAIR);
    gpio_set_dir(PIN_BTN_PAIR, GPIO_IN);
    gpio_pull_up(PIN_BTN_PAIR);

    link_init();
}
