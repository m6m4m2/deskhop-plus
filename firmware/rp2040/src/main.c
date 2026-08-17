/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Chain board firmware: the integration point between core/ and the hardware.
 *
 * Core allocation on the RP2040:
 *
 *   core 0  TinyUSB *device* stack on the native USB controller -- this board
 *           presenting as a keyboard and mouse to its computer -- plus the
 *           link ports, the protocol tick, buttons and indication.
 *   core 1  TinyUSB *host* stack on PIO-USB -- the real keyboard and mouse
 *           plugged into this board, if any.
 *
 * The split is not arbitrary. PIO-USB bit-bangs USB in software and is timing
 * critical to the microsecond; sharing a core with the device stack's
 * interrupt latency causes it to drop packets. Reports captured on core 1 are
 * handed to core 0 through the inter-core FIFO rather than being acted on
 * there.
 */
#include <string.h>

/* Deliberately not TinyUSB's bsp/board_api.h: it declares its own board_init()
 * and this project has its own hardware layer in board.c. */
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "board.h"
#include "dhp/auth.h"
#include "dhp/level.h"
#include "dhp/link.h"
#include "dhp/router.h"

#include "clientlink.h"
#include "crypto_backend.h"
#include "hid_bridge.h"

static dhp_link_t   g_link;
static dhp_router_t g_router;
static dhp_pair_t   g_pair;
static clientlink_t g_client;
static bool         g_paired;
static dhp_addr_t   g_self;
static dhp_time_t   g_error_until;
static bool         g_error_active;

/* ------------------------------------------------------------------ *
 * Link plumbing
 * ------------------------------------------------------------------ */

static void on_tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    board_uart_write(port, data, len);
}

/* ------------------------------------------------------------------ *
 * Router hooks: what happens to input destined for THIS board's machine
 * ------------------------------------------------------------------ */

static void on_kbd(void *ctx, const dhp_kbd_report_t *r)
{
    (void)ctx;
    hid_bridge_send_kbd(r);
}

static void on_mouse(void *ctx, const dhp_mouse_report_t *r)
{
    (void)ctx;
    hid_bridge_send_mouse(r);
}

/* Indication is derived in one place from the whole of the board's state.
 * Setting it from each callback separately does not work: a callback only
 * knows about the thing it was told, so the focus hook would happily
 * overwrite the routing role and vice versa. */
static void refresh_led(dhp_time_t now)
{
    /* Guarded by a flag rather than by comparing g_error_until against zero.
     * dhp_time_after() is a wrap-safe *ordering* test between two real
     * timestamps; against a sentinel it starts returning true once the
     * millisecond counter passes the halfway mark, which would latch the LED
     * to ERROR after about 25 days of uptime. */
    if (g_error_active) {
        if (!dhp_time_after(now, g_error_until)) {
            board_led(LED_ERROR);
            return;
        }
        g_error_active = false;
    }
    if (g_pair.state == DHP_PAIR_WAITING ||
        g_pair.state == DHP_PAIR_CONFIRM_WAIT) {
        board_led(LED_PAIRING);
        return;
    }
    if (!g_paired) {
        board_led(LED_UNPAIRED);
        return;
    }

    const bool focused = (dhp_router_focus(&g_router) == g_self);
    const bool active = dhp_router_is_active(&g_router);

    if (active && focused) {
        board_led(LED_ACTIVE_FOCUSED);
    } else if (active) {
        board_led(LED_ACTIVE);
    } else if (focused) {
        board_led(LED_FOCUSED);
    } else if (g_router.uhrp.state == DHP_UHRP_STANDBY) {
        board_led(LED_STANDBY);
    } else {
        board_led(LED_OFF);
    }
}

static void on_focus(void *ctx, dhp_addr_t focus, bool is_self)
{
    (void)ctx;
    (void)focus;
    (void)is_self;
    refresh_led(board_now_ms());
}

/* A level 3 frame for this board's machine. Handing it to the client is the
 * only thing to do with it: the board has nowhere to reassemble a transfer and
 * no reason to. */
static void on_data(void *ctx, const dhp_frame_t *f)
{
    (void)ctx;
    clientlink_deliver(&g_client, f);
}

static void on_level(void *ctx, dhp_level_t level)
{
    (void)ctx;
    (void)level;
    /* Level 2 brings the display; there is nothing for a chain board to do
     * here beyond making the change visible over the debug UART. */
}

/* ------------------------------------------------------------------ *
 * Capability, recomputed from what is actually attached
 * ------------------------------------------------------------------ */

static uint16_t current_caps(void)
{
    /* Always a keyboard and mouse to our own machine. */
    uint16_t caps = DHP_CAP_HID_OUT;

    /* ...and an input source only while something is actually plugged into
     * the host port. This is what makes the board holding the keyboard win
     * the election, and what makes it stop winning when unplugged. */
    if (hid_bridge_has_input_device()) {
        caps |= DHP_CAP_HID_IN;
    }

    /* ...and whatever a level 3 client on this machine contributes. Advertised
     * only while a client is actually attached and answering, so the chain's
     * level never promises a clipboard on a machine that cannot paste. */
    caps |= clientlink_caps(&g_client);

    return caps;
}

/* ------------------------------------------------------------------ *
 * Pairing
 * ------------------------------------------------------------------ */

static void pairing_drain(dhp_time_t now)
{
    const dhp_pair_events_t ev = dhp_pair_take_events(&g_pair);
    uint8_t buf[64];
    uint8_t n;

    /* Offer before confirm: a confirmation is meaningless to a peer that does
     * not yet hold the matching offer. */
    if (ev.send_offer) {
        n = dhp_pair_build_offer(&g_pair, buf, sizeof(buf));
        if (n) {
            dhp_link_send(&g_link, DHP_MSG_PAIR, DHP_ADDR_BROADCAST, buf, n, now);
        }
    }
    if (ev.send_confirm) {
        n = dhp_pair_build_confirm(&g_pair, buf, sizeof(buf));
        if (n) {
            dhp_link_send(&g_link, DHP_MSG_PAIR, DHP_ADDR_BROADCAST, buf, n, now);
        }
    }
    if (ev.send_abort) {
        n = dhp_pair_build_abort(&g_pair, buf, sizeof(buf));
        if (n) {
            dhp_link_send(&g_link, DHP_MSG_PAIR, DHP_ADDR_BROADCAST, buf, n, now);
        }
        /* Hold the error colour long enough to be seen, then fall back to
         * whatever the board's real state is. */
        g_error_until = now + 4000;
        g_error_active = true;
    }

    if (ev.completed) {
        const uint8_t *key = dhp_pair_chain_key(&g_pair);
        if (key) {
            board_key_save(key);
            dhp_link_set_key(&g_link, key);
            g_paired = true;
            g_error_active = false;
        }
    }
}

/* Called from the TinyUSB vendor-interface OUT callback in hid_bridge.c. */
void hid_bridge_on_vendor_out(const uint8_t *data, uint16_t len)
{
    clientlink_rx_report(&g_client, data, len, board_now_ms());
}

/* ------------------------------------------------------------------ *
 * Main loop
 * ------------------------------------------------------------------ */

#ifndef DHP_BRINGUP
static void core1_usb_host(void)
{
    /* Must happen before anything else on this core: core 0 writes flash when
     * pairing completes, and core 1 is executing from flash. */
    board_flash_lockout_ready();

    hid_bridge_host_init();

    /* Only now. In host mode PIO-USB claims state machines in both PIO blocks,
     * so the LED must take what is left rather than compete for it. */
    board_led_hw_init();
    for (;;) {
        tuh_task();
        hid_bridge_host_poll();
    }
}
#endif

int main(void)
{
    board_init();
    crypto_backend_init();

#ifdef DHP_BRINGUP
    /* Bring-up build: the LED comes up first and PIO-USB is left out entirely.
     *
     * Both UARTs belong to the chain, so there is no console on this board and
     * the LED is the only diagnostic. In the normal build it is initialised on
     * core 1 *after* PIO-USB, because in host mode PIO-USB claims state
     * machines in both PIO blocks and indication must not compete with USB for
     * them. The consequence is that a PIO-USB failure would present as a dark
     * LED and total silence, with no way to tell which of the two broke.
     *
     * So stage one tests one thing: does this board boot, clock correctly, and
     * enumerate as a keyboard and mouse. If the LED lights here and goes dark
     * in the full build, PIO-USB took the resources -- which is a diagnosis
     * rather than a mystery. */
    board_led_hw_init();
    board_led(LED_UNPAIRED);
#endif

    const dhp_addr_t addr = board_addr();
    const dhp_uid_t uid = board_uid();
    g_self = addr;

    dhp_link_init(&g_link, addr, on_tx, NULL);

    uint8_t key[16];
    g_paired = board_key_load(key);
    if (g_paired) {
        dhp_link_set_key(&g_link, key);
    }
    /* An unpaired board is not an error: it is still a keyboard and mouse to
     * its own machine -- level 0 -- and simply cannot join a chain yet.
     * refresh_led() shows that state below. */
    memset(key, 0, sizeof(key));

    dhp_pair_init(&g_pair, crypto_backend(), uid);

    const dhp_router_cfg_t cfg = {
        .self = addr,
        .uid = uid,
        .caps = current_caps(),
        /* A chain board does not preempt: a board joining a running chain must
         * join it rather than fight it. The coordinator is the one part that
         * does preempt, because taking over is its whole purpose. */
        .preempt = false,
        .timing = DHP_UHRP_TIMING_DEFAULT,
        .pointer = DHP_POINTER_CFG_DEFAULT,
    };
    const dhp_router_hooks_t hooks = {
        .deliver_kbd = on_kbd,
        .deliver_mouse = on_mouse,
        .focus_changed = on_focus,
        .level_changed = on_level,
        .deliver_data = on_data,
        .ctx = NULL,
    };
    dhp_router_init(&g_router, &cfg, &g_link, &hooks);
    clientlink_init(&g_client, &g_link, &g_router);

    /* Device stack on core 0's native controller, then the host stack on
     * core 1. Queues first, since core 0 begins draining them immediately. */
    tud_init(0);
    hid_bridge_queues_init();
#ifndef DHP_BRINGUP
    multicore_launch_core1(core1_usb_host);
#endif

    dhp_router_start(&g_router, board_now_ms());

    uint16_t last_caps = cfg.caps;

    for (;;) {
        const dhp_time_t now = board_now_ms();

        tud_task();

        /* Drain both link ports. A frame addressed elsewhere is relayed inside
         * dhp_link_rx_byte and never surfaces here. */
        for (int p = 0; p < DHP_PORT_COUNT; p++) {
            uint8_t b;
            while (board_uart_read((dhp_port_t)p, &b)) {
                dhp_frame_t f;
                if (dhp_link_rx_byte(&g_link, (dhp_port_t)p, b, now, &f) !=
                    DHP_OK) {
                    continue;
                }
                if (f.type == DHP_MSG_PAIR) {
                    dhp_pair_rx(&g_pair, f.payload, f.len, now);
                } else if (g_paired) {
                    dhp_router_rx(&g_router, &f, (dhp_port_t)p, now);
                }
            }
        }

        /* Input captured on core 1. On the board that is the active speaker
         * this routes locally; otherwise the router forwards it to whoever
         * holds the role. */
        dhp_kbd_report_t kbd;
        while (hid_bridge_pop_kbd(&kbd)) {
            dhp_router_local_kbd(&g_router, &kbd, now);
        }
        dhp_mouse_report_t mouse;
        while (hid_bridge_pop_mouse(&mouse)) {
            dhp_router_local_mouse(&g_router, &mouse, now);
        }

        if (board_switch_pressed()) {
#ifdef DHP_BRINGUP
            /* Nudge the pointer instead of switching machines. There is no
             * chain to switch to in stage one, and a cursor that jumps when
             * the button is pressed proves the whole device-side path at once:
             * clock, USB enumeration, the HID descriptors, the report
             * endpoint, the button and its debounce. Enumeration alone would
             * only prove the descriptors. */
            dhp_mouse_report_t nudge;
            memset(&nudge, 0, sizeof(nudge));
            nudge.dx = 30;
            hid_bridge_send_mouse(&nudge);
            board_led(LED_ACTIVE_FOCUSED);
#else
            dhp_router_button(&g_router, now);
#endif
        }

        switch (board_pair_button()) {
        case BOARD_PAIR_BTN_PAIR:
            if (g_pair.state != DHP_PAIR_WAITING) {
                dhp_pair_begin(&g_pair, now, 30000);
            }
            break;

        case BOARD_PAIR_BTN_UNPAIR:
            /* Forget the chain key and go back to level 0.
             *
             * Without this a mispaired board is unrecoverable short of
             * reflashing, and the symptom gives nothing away: two boards
             * holding different keys simply discard each other's frames, so
             * the chain looks dead rather than misconfigured. */
            board_key_erase();
            dhp_link_clear_key(&g_link);
            g_paired = false;
            dhp_pair_init(&g_pair, crypto_backend(), uid);
            /* Flash so the hold is visibly acknowledged; refresh_led() then
             * settles on UNPAIRED, which is now the truth. */
            g_error_until = now + 1500;
            g_error_active = true;
            break;

        case BOARD_PAIR_BTN_NONE:
        default:
            break;
        }

        /* Plugging a keyboard in raises this board's priority; unplugging it
         * lowers it again. */
        const uint16_t caps = current_caps();
        if (caps != last_caps) {
            dhp_router_set_caps(&g_router, caps);
            last_caps = caps;
        }

#ifndef DHP_BRINGUP
        clientlink_task(&g_client, now);
#endif

        dhp_pair_tick(&g_pair, now);
        pairing_drain(now);

        refresh_led(now);
        board_led_task();

        /* Ticked whether or not this board is paired.
         *
         * An unpaired board is still a keyboard and mouse to its own machine
         * -- that is what level 0 means, and it is the state every board is in
         * before it has ever been paired. Gating the tick on g_paired stopped
         * the election from ever settling, so the board never became the
         * active speaker, so dhp_router_local_kbd() had nowhere to route to
         * and silently dropped every keystroke.
         *
         * Nothing reaches the wire while unpaired regardless: dhp_link_send()
         * refuses without a chain key, so the hellos this generates go
         * nowhere, which is correct. */
        dhp_router_tick(&g_router, now);
    }
}
