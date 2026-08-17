/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/hid.h"

#include <string.h>

void dhp_hid_tracker_init(dhp_hid_tracker_t *t)
{
    memset(t, 0, sizeof(*t));
}

void dhp_hid_note_kbd(dhp_hid_tracker_t *t, uint8_t slot,
                      const dhp_kbd_report_t *r)
{
    if (slot >= DHP_MAX_BOARDS) {
        return;
    }
    dhp_hid_sent_t *s = &t->target[slot];
    s->used = true;
    s->modifiers = r->modifiers;
    memcpy(s->keys, r->keys, sizeof(s->keys));
}

void dhp_hid_note_mouse(dhp_hid_tracker_t *t, uint8_t slot, uint8_t buttons)
{
    if (slot >= DHP_MAX_BOARDS) {
        return;
    }
    t->target[slot].used = true;
    t->target[slot].buttons = buttons;
}

static bool slot_held(const dhp_hid_sent_t *s)
{
    if (!s->used) {
        return false;
    }
    if (s->modifiers || s->buttons) {
        return true;
    }
    for (int k = 0; k < 6; k++) {
        if (s->keys[k]) {
            return true;
        }
    }
    return false;
}

bool dhp_hid_slot_held(const dhp_hid_tracker_t *t, uint8_t slot)
{
    if (slot >= DHP_MAX_BOARDS) {
        return false;
    }
    return slot_held(&t->target[slot]);
}

bool dhp_hid_any_held(const dhp_hid_tracker_t *t)
{
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (slot_held(&t->target[i])) {
            return true;
        }
    }
    return false;
}

uint8_t dhp_hid_key_count(const dhp_hid_tracker_t *t, uint8_t slot)
{
    if (slot >= DHP_MAX_BOARDS) {
        return 0;
    }
    uint8_t n = 0;
    for (int k = 0; k < 6; k++) {
        if (t->target[slot].keys[k]) {
            n++;
        }
    }
    return n;
}

bool dhp_hid_build_release(dhp_hid_tracker_t *t, uint8_t slot,
                           dhp_kbd_report_t *kbd_out,
                           dhp_mouse_report_t *mouse_out)
{
    if (slot >= DHP_MAX_BOARDS || !slot_held(&t->target[slot])) {
        return false;
    }

    /* An all-zero keyboard report is "no modifiers, no keys", and an all-zero
     * button field is "nothing pressed" -- which is precisely the undo. */
    memset(kbd_out, 0, sizeof(*kbd_out));
    memset(mouse_out, 0, sizeof(*mouse_out));

    dhp_hid_sent_t *s = &t->target[slot];
    s->modifiers = 0;
    s->buttons = 0;
    memset(s->keys, 0, sizeof(s->keys));

    return true;
}

void dhp_hid_forget(dhp_hid_tracker_t *t, uint8_t slot)
{
    if (slot >= DHP_MAX_BOARDS) {
        return;
    }
    memset(&t->target[slot], 0, sizeof(t->target[slot]));
}
