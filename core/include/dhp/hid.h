/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Sent-state tracking, so that nothing is ever left held down on a machine
 * that no longer has the user's attention.
 *
 * A board presenting as a USB keyboard is a state machine to its host: the
 * host believes a key is down until it is told otherwise. If focus moves to
 * another machine while Ctrl is down, the abandoned machine holds Ctrl forever
 * -- every subsequent click on it is a ctrl-click, and there is no keyboard
 * attached to that machine to fix it. The same applies if the active speaker
 * dies mid-chord.
 *
 * So the invariant is: this module remembers exactly what each machine has
 * been told, and any transition that removes a machine's focus first sends it
 * the report that undoes everything outstanding.
 *
 * It is also the source of truth for UHRP rule 1 -- dhp_hid_any_held() is what
 * defers a planned handover until the chord finishes.
 */
#ifndef DHP_HID_H
#define DHP_HID_H

#include "dhp/msg.h"
#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one machine currently believes is pressed. */
typedef struct {
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t buttons;
    bool    used;
} dhp_hid_sent_t;

typedef struct {
    dhp_hid_sent_t target[DHP_MAX_BOARDS];
} dhp_hid_tracker_t;

void dhp_hid_tracker_init(dhp_hid_tracker_t *t);

/* Record a keyboard report as delivered to `slot`. */
void dhp_hid_note_kbd(dhp_hid_tracker_t *t, uint8_t slot,
                      const dhp_kbd_report_t *r);

/* Record mouse button state as delivered to `slot`. Motion is stateless and
 * needs no tracking; buttons are exactly as sticky as keys. */
void dhp_hid_note_mouse(dhp_hid_tracker_t *t, uint8_t slot, uint8_t buttons);

/* Is anything held anywhere? Drives UHRP rule 1. */
bool dhp_hid_any_held(const dhp_hid_tracker_t *t);

/* Is anything held on this one machine? */
bool dhp_hid_slot_held(const dhp_hid_tracker_t *t, uint8_t slot);

/* Number of physical keys down on a slot, excluding modifiers. */
uint8_t dhp_hid_key_count(const dhp_hid_tracker_t *t, uint8_t slot);

/* Build the reports that undo everything outstanding on `slot`, and clear the
 * tracked state. Returns false if nothing was held, in which case no report
 * needs sending and the caller should not spend a frame on it.
 *
 * Both outputs are always written when the function returns true: a machine
 * can be holding a modifier and a mouse button at once, and releasing only
 * one of them is the bug this function exists to prevent. */
bool dhp_hid_build_release(dhp_hid_tracker_t *t, uint8_t slot,
                           dhp_kbd_report_t *kbd_out,
                           dhp_mouse_report_t *mouse_out);

/* Clear tracked state for a slot without generating reports -- for when the
 * machine is known to have gone away entirely (board unplugged), so there is
 * nobody left to tell. */
void dhp_hid_forget(dhp_hid_tracker_t *t, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* DHP_HID_H */
