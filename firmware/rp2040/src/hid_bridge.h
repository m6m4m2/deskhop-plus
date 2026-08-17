/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The two USB roles, and the boundary between them.
 *
 * Device side (core 0, native USB): this board presenting as a keyboard and
 * mouse to the computer it is plugged into. That is what makes it work at BIOS
 * and at a login screen -- the machine sees an ordinary HID keyboard and there
 * is nothing to install.
 *
 * Host side (core 1, PIO-USB): the real keyboard and mouse, if they happen to
 * be plugged into this particular board.
 *
 * Reports captured on core 1 are queued here and consumed by the main loop on
 * core 0. They are never acted on from the host stack's callback: PIO-USB is
 * timing critical to the microsecond and must not be made to wait for protocol
 * work.
 */
#ifndef DHP_HID_BRIDGE_H
#define DHP_HID_BRIDGE_H

#include "dhp/msg.h"

/* --- host side, core 1 --- */
void hid_bridge_host_init(void);
void hid_bridge_host_poll(void);

/* --- consumed on core 0 --- */
bool hid_bridge_pop_kbd(dhp_kbd_report_t *out);
bool hid_bridge_pop_mouse(dhp_mouse_report_t *out);

/* True while a keyboard or mouse is mounted on the host port. Drives
 * DHP_CAP_HID_IN, and therefore this board's election priority. */
bool hid_bridge_has_input_device(void);

/* --- device side, core 0 --- */
void hid_bridge_send_kbd(const dhp_kbd_report_t *r);
void hid_bridge_send_mouse(const dhp_mouse_report_t *r);

#endif /* DHP_HID_BRIDGE_H */
