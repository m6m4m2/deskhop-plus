/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The two USB roles. See hid_bridge.h for why they live on different cores.
 */
#include "hid_bridge.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "pio_usb.h"
#include "tusb.h"

#include "board.h"

enum {
    ITF_KEYBOARD = 0,
    ITF_MOUSE,
};

/* Cross-core queues. Reports are captured in the host stack's callback on
 * core 1 and consumed by the protocol loop on core 0; nothing is acted on
 * from the callback itself, because PIO-USB is timing critical and must not
 * be made to wait for a link write. */
static queue_t g_kbd_q;
static queue_t g_mouse_q;
static volatile bool g_have_kbd;
static volatile bool g_have_mouse;

/* ------------------------------------------------------------------ *
 * Host side -- core 1
 * ------------------------------------------------------------------ */

/* Queues are created on core 0 before core 1 is launched. Creating them inside
 * the core 1 entry point would race: core 0 starts popping from them as soon
 * as its main loop runs, which can be before core 1 has initialised them. */
void hid_bridge_queues_init(void)
{
    queue_init(&g_kbd_q, sizeof(dhp_kbd_report_t), 32);
    queue_init(&g_mouse_q, sizeof(dhp_mouse_report_t), 64);
}

void hid_bridge_host_init(void)
{
    /* rhport 1 is PIO-USB. The pin pair must be handed to TinyUSB before
     * tuh_init(), and D- is implicitly pin_dp + 1 -- the PIO program requires
     * the two to be consecutive. */
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_PIO_USB_DP;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    tuh_init(1);
}

void hid_bridge_host_poll(void)
{
    /* tuh_task() does the work; this exists so the main loop has a place to
     * hang periodic host-side maintenance. */
}

bool hid_bridge_has_input_device(void)
{
    return g_have_kbd || g_have_mouse;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;

    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
        g_have_kbd = true;
    } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
        g_have_mouse = true;
    }

    /* Boot protocol deliberately: it is a fixed, known report layout, which
     * avoids parsing an arbitrary report descriptor on a microcontroller and
     * matches the shape this device presents downstream. */
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
        g_have_kbd = false;
    } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
        g_have_mouse = false;
    }
    (void)dev_addr;
    (void)instance;

    /* Losing the keyboard drops DHP_CAP_HID_IN, which lowers this board's
     * election priority; the main loop notices on its next pass. */
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len)
{
    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

    if (proto == HID_ITF_PROTOCOL_KEYBOARD && len >= 8) {
        /* Boot keyboard: modifiers, reserved, then six keycodes. */
        dhp_kbd_report_t r;
        r.modifiers = report[0];
        memcpy(r.keys, &report[2], 6);
        queue_try_add(&g_kbd_q, &r);

    } else if (proto == HID_ITF_PROTOCOL_MOUSE && len >= 3) {
        /* Boot mouse: buttons, dx, dy, with wheel as an optional fourth. */
        dhp_mouse_report_t r;
        memset(&r, 0, sizeof(r));
        r.buttons = report[0];
        r.dx = (int8_t)report[1];
        r.dy = (int8_t)report[2];
        if (len >= 4) {
            r.wheel = (int8_t)report[3];
        }
        queue_try_add(&g_mouse_q, &r);
    }

    /* Ask for the next one immediately: a missed request stalls the device. */
    tuh_hid_receive_report(dev_addr, instance);
}

/* ------------------------------------------------------------------ *
 * Consumed on core 0
 * ------------------------------------------------------------------ */

bool hid_bridge_pop_kbd(dhp_kbd_report_t *out)
{
    return queue_try_remove(&g_kbd_q, out);
}

bool hid_bridge_pop_mouse(dhp_mouse_report_t *out)
{
    return queue_try_remove(&g_mouse_q, out);
}

/* ------------------------------------------------------------------ *
 * Device side -- core 0
 * ------------------------------------------------------------------ */

void hid_bridge_send_kbd(const dhp_kbd_report_t *r)
{
    if (!tud_hid_n_ready(ITF_KEYBOARD)) {
        /* The host is not collecting. Dropping is correct here: a stale
         * keystroke delivered late is worse than one not delivered, and the
         * release path in core/ guarantees nothing is left held regardless. */
        return;
    }
    tud_hid_n_keyboard_report(ITF_KEYBOARD, 0, r->modifiers, (uint8_t *)r->keys);
}

void hid_bridge_send_mouse(const dhp_mouse_report_t *r)
{
    if (!tud_hid_n_ready(ITF_MOUSE)) {
        return;
    }
    /* Deltas are clamped to int8 for the boot-mouse report. A single report
     * larger than this only happens on a very fast flick, and the pointer
     * integrator in core/ sees the unclamped value, so crossing behaviour is
     * unaffected by the clamp. */
    const int8_t dx = (int8_t)(r->dx > 127 ? 127 : (r->dx < -127 ? -127 : r->dx));
    const int8_t dy = (int8_t)(r->dy > 127 ? 127 : (r->dy < -127 ? -127 : r->dy));

    tud_hid_n_mouse_report(ITF_MOUSE, 0, r->buttons, dx, dy, r->wheel, r->pan);
}

/* Required by TinyUSB; this device has nothing to report on request and
 * ignores output reports such as the keyboard LED state. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer;
    (void)bufsize;
}
