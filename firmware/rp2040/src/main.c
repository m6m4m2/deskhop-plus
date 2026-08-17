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

#include "bsp/board_api.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "board.h"
#include "dhp/auth.h"
#include "dhp/level.h"
#include "dhp/link.h"
#include "dhp/router.h"

#include "crypto_backend.h"
#include "hid_bridge.h"

static dhp_link_t   g_link;
static dhp_router_t g_router;
static dhp_pair_t   g_pair;
static bool         g_paired;

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

static void on_focus(void *ctx, dhp_addr_t focus, bool is_self)
{
    (void)ctx;
    (void)focus;
    board_led(is_self ? LED_FOCUSED
                      : (dhp_router_is_active(&g_router) ? LED_ACTIVE
                                                         : LED_OFF));
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
        board_led(LED_ERROR);
    }

    if (ev.completed) {
        const uint8_t *key = dhp_pair_chain_key(&g_pair);
        if (key) {
            board_key_save(key);
            dhp_link_set_key(&g_link, key);
            g_paired = true;
            board_led(LED_OFF);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Main loop
 * ------------------------------------------------------------------ */

static void core1_usb_host(void)
{
    hid_bridge_host_init();
    for (;;) {
        tuh_task();
        hid_bridge_host_poll();
    }
}

int main(void)
{
    board_init();
    crypto_backend_init();

    const dhp_addr_t addr = board_addr();
    const dhp_uid_t uid = board_uid();

    dhp_link_init(&g_link, addr, on_tx, NULL);

    uint8_t key[16];
    g_paired = board_key_load(key);
    if (g_paired) {
        dhp_link_set_key(&g_link, key);
    } else {
        /* Not an error. An unpaired board is still a keyboard and mouse to its
         * own machine -- level 0 -- and simply cannot join a chain yet. */
        board_led(LED_UNPAIRED);
    }
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
        .ctx = NULL,
    };
    dhp_router_init(&g_router, &cfg, &g_link, &hooks);

    tusb_init();
    multicore_launch_core1(core1_usb_host);

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
            dhp_router_button(&g_router, now);
        }

        if (board_pair_held() && g_pair.state != DHP_PAIR_WAITING) {
            board_led(LED_PAIRING);
            dhp_pair_begin(&g_pair, now, 30000);
        }

        /* Plugging a keyboard in raises this board's priority; unplugging it
         * lowers it again. */
        const uint16_t caps = current_caps();
        if (caps != last_caps) {
            dhp_router_set_caps(&g_router, caps);
            last_caps = caps;
        }

        dhp_pair_tick(&g_pair, now);
        pairing_drain(now);

        if (g_paired) {
            dhp_router_tick(&g_router, now);
        }
    }
}
