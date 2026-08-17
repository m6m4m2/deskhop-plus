/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/router.h"

#include <string.h>

/* ---------------------------------------------------------------------- *
 * Chain table
 * ---------------------------------------------------------------------- */

static dhp_chain_entry_t *chain_find(dhp_router_t *r, dhp_addr_t addr)
{
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (r->chain[i].used && r->chain[i].addr == addr) {
            return &r->chain[i];
        }
    }
    return NULL;
}

static dhp_chain_entry_t *chain_get(dhp_router_t *r, dhp_addr_t addr)
{
    dhp_chain_entry_t *e = chain_find(r, addr);
    if (e) {
        return e;
    }
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (!r->chain[i].used) {
            memset(&r->chain[i], 0, sizeof(r->chain[i]));
            r->chain[i].used = true;
            r->chain[i].addr = addr;
            return &r->chain[i];
        }
    }
    return NULL;
}

/* The hid tracker is indexed by chain slot, so the slot doubles as the
 * identity of "the machine attached to that board". */
static uint8_t slot_of(dhp_router_t *r, dhp_addr_t addr)
{
    if (addr == r->cfg.self) {
        return r->self_slot;
    }
    const dhp_chain_entry_t *e = chain_find(r, addr);
    if (!e) {
        return r->self_slot;
    }
    return (uint8_t)(e - r->chain);
}

/* Position relative to us, derived from arrival port and hop-limit decay.
 * See the header for why this needs no numbering protocol. */
static int8_t rel_pos_from_frame(const dhp_frame_t *f, dhp_port_t in_port)
{
    int distance = (int)DHP_TTL_DEFAULT - (int)f->ttl + 1;
    if (distance < 1) {
        distance = 1;
    }
    if (distance > 127) {
        distance = 127;
    }
    return (int8_t)(in_port == DHP_PORT_UP ? -distance : distance);
}

uint8_t dhp_router_chain_size(const dhp_router_t *r)
{
    uint8_t n = 0;
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (r->chain[i].used) {
            n++;
        }
    }
    return n;
}

/* Nearest live board one step from `from` in the given direction.
 *
 * Crossing is relative to the machine the user is currently on, NOT to this
 * board. Those are routinely different: once the coordinator takes the active
 * role it owns the routing decision while sitting at the end of the chain,
 * and measuring from itself would send the pointer to its own neighbour
 * instead of the focused machine's. */
static dhp_addr_t neighbour_of(dhp_router_t *r, dhp_addr_t from, int direction)
{
    int from_pos = 0;
    if (from != r->cfg.self) {
        const dhp_chain_entry_t *e = chain_find(r, from);
        if (!e) {
            return DHP_ADDR_NONE;
        }
        from_pos = e->rel_pos;
    }

    dhp_addr_t best = DHP_ADDR_NONE;
    int best_pos = 0;

    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        const dhp_chain_entry_t *e = &r->chain[i];
        if (!e->used || e->addr == from) {
            continue;
        }
        /* Only boards that present as a keyboard to a machine are somewhere
         * the pointer can go; a coordinator is on the chain but is not a
         * screen. */
        if (!(e->caps & DHP_CAP_HID_OUT) && e->addr != r->cfg.self) {
            continue;
        }
        const int pos = (e->addr == r->cfg.self) ? 0 : e->rel_pos;

        if (direction > 0 && pos > from_pos &&
            (best == DHP_ADDR_NONE || pos < best_pos)) {
            best = e->addr;
            best_pos = pos;
        } else if (direction < 0 && pos < from_pos &&
                   (best == DHP_ADDR_NONE || pos > best_pos)) {
            best = e->addr;
            best_pos = pos;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------- *
 * Sending input onward
 * ---------------------------------------------------------------------- */

static void send_kbd_to(dhp_router_t *r, dhp_addr_t dst,
                        const dhp_kbd_report_t *rep, dhp_time_t now)
{
    dhp_hid_note_kbd(&r->hid, slot_of(r, dst), rep);

    if (dst == r->cfg.self) {
        if (r->hooks.deliver_kbd) {
            r->hooks.deliver_kbd(r->hooks.ctx, rep);
        }
        return;
    }
    uint8_t buf[DHP_KBD_BYTES];
    dhp_kbd_pack(rep, buf);
    dhp_link_send(r->link, DHP_MSG_KBD, dst, buf, sizeof(buf), now);
}

static void send_mouse_to(dhp_router_t *r, dhp_addr_t dst,
                          const dhp_mouse_report_t *rep, dhp_time_t now)
{
    dhp_hid_note_mouse(&r->hid, slot_of(r, dst), rep->buttons);

    if (dst == r->cfg.self) {
        if (r->hooks.deliver_mouse) {
            r->hooks.deliver_mouse(r->hooks.ctx, rep);
        }
        return;
    }
    uint8_t buf[DHP_MOUSE_BYTES];
    dhp_mouse_pack(rep, buf);
    dhp_link_send(r->link, DHP_MSG_MOUSE, dst, buf, sizeof(buf), now);
}

/* Undo everything outstanding on a machine that is losing focus.
 *
 * This is the difference between a switch and an abandonment: without it, a
 * machine that had Ctrl down when focus moved keeps Ctrl down forever, and
 * there is no keyboard attached to it to lift the key. */
static void release_machine(dhp_router_t *r, dhp_addr_t addr, dhp_time_t now)
{
    dhp_kbd_report_t k;
    dhp_mouse_report_t m;

    if (!dhp_hid_build_release(&r->hid, slot_of(r, addr), &k, &m)) {
        return; /* nothing was held: do not spend frames saying so */
    }

    r->n_releases_sent++;

    if (addr == r->cfg.self) {
        if (r->hooks.deliver_kbd)   r->hooks.deliver_kbd(r->hooks.ctx, &k);
        if (r->hooks.deliver_mouse) r->hooks.deliver_mouse(r->hooks.ctx, &m);
        return;
    }

    uint8_t kb[DHP_KBD_BYTES];
    dhp_kbd_pack(&k, kb);
    dhp_link_send(r->link, DHP_MSG_KBD, addr, kb, sizeof(kb), now);

    uint8_t mb[DHP_MOUSE_BYTES];
    dhp_mouse_pack(&m, mb);
    dhp_link_send(r->link, DHP_MSG_MOUSE, addr, mb, sizeof(mb), now);
}

/* ---------------------------------------------------------------------- *
 * Focus
 * ---------------------------------------------------------------------- */

static void apply_focus(dhp_router_t *r, dhp_addr_t target, uint8_t reason,
                        dhp_edge_t edge, dhp_time_t now, bool announce)
{
    if (target == DHP_ADDR_NONE || target == r->focus) {
        return;
    }

    /* Order matters: release the outgoing machine before anything else, so
     * that even if the link dies during the switch the abandoned machine has
     * already been told to let go. */
    release_machine(r, r->focus, now);

    r->focus = target;
    r->n_switches++;

    /* Re-seat the pointer gesture for the machine we have arrived on. */
    dhp_pointer_arrive(&r->pointer, edge, now);

    if (announce) {
        const dhp_focus_t fo = {
            .target = target, .reason = reason, .edge = (uint8_t)edge,
        };
        uint8_t buf[DHP_FOCUS_BYTES];
        dhp_focus_pack(&fo, buf);
        dhp_link_send(r->link, DHP_MSG_FOCUS, DHP_ADDR_BROADCAST, buf,
                      sizeof(buf), now);
    }

    if (r->hooks.focus_changed) {
        r->hooks.focus_changed(r->hooks.ctx, target, target == r->cfg.self);
    }
}

void dhp_router_set_focus(dhp_router_t *r, dhp_addr_t target, uint8_t reason,
                          dhp_time_t now)
{
    apply_focus(r, target, reason, DHP_EDGE_NONE, now, true);
}

void dhp_router_button(dhp_router_t *r, dhp_time_t now)
{
    /* Step one place down-chain, wrapping to the far up-chain end. Only the
     * active speaker owns the focus decision. */
    if (!dhp_router_is_active(r)) {
        return;
    }

    dhp_addr_t next = DHP_ADDR_NONE;
    const dhp_chain_entry_t *cur = chain_find(r, r->focus);
    const int cur_pos = (r->focus == r->cfg.self) ? 0 : (cur ? cur->rel_pos : 0);

    int best_pos = 0;
    int wrap_pos = 0;
    dhp_addr_t wrap = DHP_ADDR_NONE;

    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        const dhp_chain_entry_t *e = &r->chain[i];
        if (!e->used || !(e->caps & DHP_CAP_HID_OUT)) {
            continue; /* a coordinator is on the chain but is not a screen */
        }
        const int pos = (e->addr == r->cfg.self) ? 0 : e->rel_pos;

        if (pos > cur_pos && (next == DHP_ADDR_NONE || pos < best_pos)) {
            next = e->addr;
            best_pos = pos;
        }
        if (wrap == DHP_ADDR_NONE || pos < wrap_pos) {
            wrap = e->addr;
            wrap_pos = pos;
        }
    }

    if (next == DHP_ADDR_NONE) {
        next = wrap;
    }
    apply_focus(r, next, DHP_FOCUS_R_BUTTON, DHP_EDGE_NONE, now, true);
}

/* ---------------------------------------------------------------------- *
 * Level
 * ---------------------------------------------------------------------- */

static void recompute_level(dhp_router_t *r)
{
    const uint16_t caps = dhp_uhrp_reachable_caps(&r->uhrp);

    /* An election has settled once somebody -- us or a peer -- holds the
     * active role. Until then the chain cannot route anything. */
    const bool elected =
        dhp_router_is_active(r) || r->uhrp.active != DHP_ADDR_NONE;

    const dhp_level_t l = dhp_level_of(caps, elected);
    if (l != r->level) {
        r->level = l;
        if (r->hooks.level_changed) {
            r->hooks.level_changed(r->hooks.ctx, l);
        }
    }
}

/* ---------------------------------------------------------------------- *
 * UHRP plumbing
 * ---------------------------------------------------------------------- */

static void broadcast_release_all(dhp_router_t *r, dhp_time_t now)
{
    /* Rule 2. The previous active speaker vanished without warning, so any
     * machine it was typing on may be holding keys with nobody left to lift
     * them. Tell every board to let go of everything, unconditionally --
     * this board's tracker cannot know what the dead one had sent. */
    dhp_link_send(r->link, DHP_MSG_RELEASE, DHP_ADDR_BROADCAST, NULL, 0, now);

    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (r->chain[i].used) {
            dhp_hid_forget(&r->hid, (uint8_t)i);
        }
    }

    /* And our own machine, which the broadcast will not come back to. */
    dhp_kbd_report_t k;
    memset(&k, 0, sizeof(k));
    dhp_mouse_report_t m;
    memset(&m, 0, sizeof(m));
    if (r->hooks.deliver_kbd)   r->hooks.deliver_kbd(r->hooks.ctx, &k);
    if (r->hooks.deliver_mouse) r->hooks.deliver_mouse(r->hooks.ctx, &m);
}

/* A sensible target when nothing better is known.
 *
 * The active speaker is not necessarily attached to a machine -- a coordinator
 * has a display and configuration but no host to be a keyboard to -- so it
 * cannot simply focus itself. Prefer our own machine when we have one, then
 * the nearest board that does. */
static dhp_addr_t pick_default_focus(dhp_router_t *r)
{
    if (r->cfg.caps & DHP_CAP_HID_OUT) {
        return r->cfg.self;
    }

    dhp_addr_t best = DHP_ADDR_NONE;
    int best_dist = 0;
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        const dhp_chain_entry_t *e = &r->chain[i];
        if (!e->used || e->addr == r->cfg.self ||
            !(e->caps & DHP_CAP_HID_OUT)) {
            continue;
        }
        const int dist = e->rel_pos < 0 ? -e->rel_pos : e->rel_pos;
        if (best == DHP_ADDR_NONE || dist < best_dist) {
            best = e->addr;
            best_dist = dist;
        }
    }
    return best;
}

static void drain_uhrp(dhp_router_t *r, dhp_time_t now)
{
    const dhp_uhrp_events_t ev = dhp_uhrp_take_events(&r->uhrp);

    if (ev.broadcast_release) {
        broadcast_release_all(r, now);
    }

    if (ev.send_resign) {
        dhp_link_send(r->link, DHP_MSG_RESIGN, DHP_ADDR_BROADCAST, NULL, 0, now);
    }

    if (ev.send_hello) {
        dhp_hello_t h;
        dhp_uhrp_build_hello(&r->uhrp, &h);
        uint8_t buf[DHP_HELLO_BYTES];
        dhp_hello_pack(&h, buf);
        dhp_link_send(r->link, DHP_MSG_HELLO, DHP_ADDR_BROADCAST, buf,
                      sizeof(buf), now);
    }

    if (ev.state_changed) {
        if (dhp_router_is_active(r)) {
            /* Adopt whatever focus we already know about. Every board tracks
             * the FOCUS broadcasts even while it is only standing by, so a
             * planned handover inherits the user's current machine rather
             * than yanking them somewhere else mid-sentence. */
            dhp_addr_t f = r->focus;
            if (f == DHP_ADDR_NONE || !chain_find(r, f)) {
                f = pick_default_focus(r);
            }
            if (f != DHP_ADDR_NONE) {
                /* Announce unconditionally, even if unchanged: the chain has
                 * a new authority and every board should hear it say so. */
                const dhp_focus_t fo = {.target = f,
                                        .reason = DHP_FOCUS_R_FAILOVER,
                                        .edge = DHP_EDGE_NONE};
                uint8_t buf[DHP_FOCUS_BYTES];
                dhp_focus_pack(&fo, buf);
                dhp_link_send(r->link, DHP_MSG_FOCUS, DHP_ADDR_BROADCAST, buf,
                              sizeof(buf), now);

                if (f != r->focus) {
                    r->focus = f;
                    if (r->hooks.focus_changed) {
                        r->hooks.focus_changed(r->hooks.ctx, f,
                                               f == r->cfg.self);
                    }
                }
            }
        }
        recompute_level(r);
    }
}

/* ---------------------------------------------------------------------- *
 * Public
 * ---------------------------------------------------------------------- */

void dhp_router_init(dhp_router_t *r, const dhp_router_cfg_t *cfg,
                     dhp_link_t *link, const dhp_router_hooks_t *hooks)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
    r->link = link;
    if (hooks) {
        r->hooks = *hooks;
    }

    const dhp_uhrp_config_t uc = {
        .uid = cfg->uid, .addr = cfg->self, .priority = 0, .caps = cfg->caps,
        .preempt = cfg->preempt, .timing = cfg->timing,
    };
    dhp_uhrp_init(&r->uhrp, &uc);
    dhp_pointer_init(&r->pointer, &cfg->pointer);
    dhp_hid_tracker_init(&r->hid);

    /* Reserve a chain slot for ourselves so slot_of() always resolves. */
    dhp_chain_entry_t *self = chain_get(r, cfg->self);
    self->uid = cfg->uid;
    self->caps = cfg->caps;
    self->rel_pos = 0;
    r->self_slot = (uint8_t)(self - r->chain);

    r->focus = DHP_ADDR_NONE;
    r->level = DHP_LEVEL_ISOLATED;
}

void dhp_router_start(dhp_router_t *r, dhp_time_t now)
{
    dhp_uhrp_start(&r->uhrp, now);
    dhp_pointer_arrive(&r->pointer, DHP_EDGE_NONE, now);
    drain_uhrp(r, now);
}

/* Forward locally-captured input to the active speaker.
 *
 * Returns true if the input was handed off and the caller should stop. Only a
 * board that actually holds the input devices may do this, so a board with no
 * keyboard cannot inject anything into the chain. */
static bool forward_input(dhp_router_t *r, uint8_t kind, const uint8_t *report,
                          uint8_t report_len, dhp_time_t now)
{
    if (dhp_router_is_active(r)) {
        return false; /* we are the authority: handle it here */
    }
    if (!(r->cfg.caps & DHP_CAP_HID_IN)) {
        return true; /* not ours to send; drop */
    }
    if (r->uhrp.active == DHP_ADDR_NONE) {
        return true; /* no authority yet; nowhere to send it */
    }

    uint8_t buf[1 + DHP_MOUSE_BYTES];
    buf[0] = kind;
    memcpy(&buf[1], report, report_len);
    dhp_link_send(r->link, DHP_MSG_INPUT, r->uhrp.active, buf,
                  (uint8_t)(1 + report_len), now);
    return true;
}

void dhp_router_local_kbd(dhp_router_t *r, const dhp_kbd_report_t *rep,
                          dhp_time_t now)
{
    uint8_t packed[DHP_KBD_BYTES];
    dhp_kbd_pack(rep, packed);
    if (forward_input(r, DHP_INPUT_KIND_KBD, packed, sizeof(packed), now)) {
        return;
    }

    send_kbd_to(r, r->focus, rep, now);

    /* Feeds UHRP rule 1: a planned handover waits for this to go false. */
    dhp_uhrp_set_keys_held(&r->uhrp, dhp_hid_any_held(&r->hid));
    drain_uhrp(r, now);
}

void dhp_router_local_mouse(dhp_router_t *r, const dhp_mouse_report_t *rep,
                            dhp_time_t now)
{
    uint8_t packed[DHP_MOUSE_BYTES];
    dhp_mouse_pack(rep, packed);
    if (forward_input(r, DHP_INPUT_KIND_MOUSE, packed, sizeof(packed), now)) {
        return;
    }

    const dhp_edge_t edge = dhp_pointer_feed(&r->pointer, rep->dx, rep->dy, now);

    if (edge != DHP_EDGE_NONE) {
        /* Left edge means "go up-chain", right edge means "go down-chain". */
        const int dir = (edge == DHP_EDGE_LEFT || edge == DHP_EDGE_UP) ? -1 : +1;
        const dhp_addr_t target = neighbour_of(r, r->focus, dir);
        if (target != DHP_ADDR_NONE) {
            apply_focus(r, target, DHP_FOCUS_R_EDGE, edge, now, true);
            /* Deliberately drop this report: it is the tail of the shove, not
             * motion the user wants applied on the machine just arrived at. */
            return;
        }
        /* No board that way -- we are at the end of the chain. The gesture
         * simply does nothing, which is the right behaviour at the last
         * screen. */
    }

    send_mouse_to(r, r->focus, rep, now);
    dhp_uhrp_set_keys_held(&r->uhrp, dhp_hid_any_held(&r->hid));
    drain_uhrp(r, now);
}

void dhp_router_rx(dhp_router_t *r, const dhp_frame_t *f, dhp_port_t in_port,
                   dhp_time_t now)
{
    /* Every authenticated frame is evidence of where its sender sits. */
    if (f->src != r->cfg.self && in_port < DHP_PORT_COUNT) {
        dhp_chain_entry_t *e = chain_get(r, f->src);
        if (e) {
            e->rel_pos = rel_pos_from_frame(f, in_port);
            e->last_seen = now;
        }
    }

    switch (f->type) {
    case DHP_MSG_HELLO: {
        dhp_hello_t h;
        if (dhp_hello_unpack(f->payload, f->len, &h)) {
            dhp_chain_entry_t *e = chain_get(r, f->src);
            if (e) {
                e->uid = h.uid;
                e->caps = h.caps;
            }
            dhp_uhrp_rx_hello(&r->uhrp, f->src, &h, now);
            drain_uhrp(r, now);
            recompute_level(r);
        }
        break;
    }

    case DHP_MSG_RESIGN:
        dhp_uhrp_rx_resign(&r->uhrp, f->src, now);
        drain_uhrp(r, now);
        recompute_level(r);
        break;

    case DHP_MSG_KBD: {
        dhp_kbd_report_t rep;
        if (dhp_kbd_unpack(f->payload, f->len, &rep) && r->hooks.deliver_kbd) {
            r->hooks.deliver_kbd(r->hooks.ctx, &rep);
        }
        break;
    }

    case DHP_MSG_MOUSE: {
        dhp_mouse_report_t rep;
        if (dhp_mouse_unpack(f->payload, f->len, &rep) &&
            r->hooks.deliver_mouse) {
            r->hooks.deliver_mouse(r->hooks.ctx, &rep);
        }
        break;
    }

    case DHP_MSG_FOCUS: {
        dhp_focus_t fo;
        if (dhp_focus_unpack(f->payload, f->len, &fo)) {
            /* Informational on non-active boards: it tells the display and
             * the LEDs who currently has the user. */
            if (!dhp_router_is_active(r)) {
                const bool was_self = (r->focus == r->cfg.self);
                r->focus = fo.target;
                if (r->hooks.focus_changed) {
                    r->hooks.focus_changed(r->hooks.ctx, fo.target,
                                           fo.target == r->cfg.self);
                }
                /* If we just lost focus, let go of anything we were holding
                 * even though the active board should also have told us. Two
                 * independent paths to "not stuck" is the point. */
                if (was_self && fo.target != r->cfg.self) {
                    release_machine(r, r->cfg.self, now);
                }
            }
        }
        break;
    }

    case DHP_MSG_INPUT: {
        /* Raw input captured elsewhere and handed to us because we hold the
         * active role. Only accept it from a board that actually has input
         * devices attached, so that no other board can inject keystrokes. */
        if (!dhp_router_is_active(r) || f->len < 1) {
            break;
        }
        const dhp_chain_entry_t *e = chain_find(r, f->src);
        if (!e || !(e->caps & DHP_CAP_HID_IN)) {
            break;
        }

        if (f->payload[0] == DHP_INPUT_KIND_KBD) {
            dhp_kbd_report_t rep;
            if (dhp_kbd_unpack(f->payload + 1, (uint8_t)(f->len - 1), &rep)) {
                dhp_router_local_kbd(r, &rep, now);
            }
        } else if (f->payload[0] == DHP_INPUT_KIND_MOUSE) {
            dhp_mouse_report_t rep;
            if (dhp_mouse_unpack(f->payload + 1, (uint8_t)(f->len - 1), &rep)) {
                dhp_router_local_mouse(r, &rep, now);
            }
        }
        break;
    }

    case DHP_MSG_RELEASE: {
        dhp_kbd_report_t k;
        memset(&k, 0, sizeof(k));
        dhp_mouse_report_t m;
        memset(&m, 0, sizeof(m));
        dhp_hid_forget(&r->hid, r->self_slot);
        if (r->hooks.deliver_kbd)   r->hooks.deliver_kbd(r->hooks.ctx, &k);
        if (r->hooks.deliver_mouse) r->hooks.deliver_mouse(r->hooks.ctx, &m);
        break;
    }

    default:
        break;
    }
}

void dhp_router_tick(dhp_router_t *r, dhp_time_t now)
{
    dhp_link_tick(r->link, now);
    dhp_uhrp_tick(&r->uhrp, now);
    drain_uhrp(r, now);
    recompute_level(r);
}
