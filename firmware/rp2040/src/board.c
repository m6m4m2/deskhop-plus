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

static bool       g_switch_last;
static dhp_time_t g_switch_change;
static dhp_time_t g_pair_down_since;
static bool       g_pair_reported;

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

bool board_pair_held(void)
{
    const bool down = !gpio_get(PIN_BTN_PAIR);
    const dhp_time_t now = board_now_ms();

    if (!down) {
        g_pair_down_since = 0;
        g_pair_reported = false;
        return false;
    }
    if (g_pair_down_since == 0) {
        g_pair_down_since = now;
        return false;
    }

    /* A hold rather than a click: pairing should not be reachable by brushing
     * against the board, since it is the one action that grants trust. */
    if (!g_pair_reported && dhp_time_after(now, g_pair_down_since + PAIR_HOLD_MS)) {
        g_pair_reported = true;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Indication
 *
 * Left as a stub: WS2812 output belongs on PIO, and the protocol does not
 * depend on it. Colours are chosen so the chain's state is readable at a
 * glance across a desk.
 * ------------------------------------------------------------------ */

void board_led(led_state_t s)
{
    (void)s;
    /* TODO: drive PIN_WS2812 from a PIO program.
     *   FOCUSED  white   this machine has the user
     *   ACTIVE   blue    this board holds the routing role
     *   STANDBY  dim blue
     *   PAIRING  amber, pulsing
     *   UNPAIRED amber, steady
     *   ERROR    red
     */
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
