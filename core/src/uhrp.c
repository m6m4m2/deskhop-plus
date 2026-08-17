/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/uhrp.h"

#include <string.h>

#include "dhp/level.h"

/* ---------------------------------------------------------------------- *
 * Ordering
 *
 * "The most capable wins, ties break on the unique per-board id." Higher
 * priority wins; equal priority is broken by the larger uid. The uid comes
 * from flash and is globally unique, so this total order never has ties --
 * which is what guarantees the election always converges on exactly one
 * winner rather than oscillating.
 * ---------------------------------------------------------------------- */
static bool better(uint8_t pa, dhp_uid_t ua, uint8_t pb, dhp_uid_t ub)
{
    if (pa != pb) {
        return pa > pb;
    }
    return ua > ub;
}

uint8_t dhp_uhrp_priority_from_caps(uint16_t caps)
{
    unsigned p = 50;
    if (caps & DHP_CAP_COORD)   p += 100; /* a coordinator outranks any board */
    if (caps & DHP_CAP_HID_IN)  p += 40;  /* holding the real keyboard matters */
    if (caps & DHP_CAP_DISPLAY) p += 10;
    if (caps & DHP_CAP_CONFIG)  p += 5;
    return (uint8_t)(p > 255 ? 255 : p);
}

const char *dhp_uhrp_state_name(dhp_uhrp_state_t s)
{
    switch (s) {
    case DHP_UHRP_INIT:    return "init";
    case DHP_UHRP_LEARN:   return "learn";
    case DHP_UHRP_LISTEN:  return "listen";
    case DHP_UHRP_SPEAK:   return "speak";
    case DHP_UHRP_STANDBY: return "standby";
    case DHP_UHRP_ACTIVE:  return "active";
    default:               return "?";
    }
}

/* ---------------------------------------------------------------------- *
 * Peer table
 * ---------------------------------------------------------------------- */

static dhp_uhrp_peer_t *peer_find(dhp_uhrp_t *u, dhp_addr_t addr)
{
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (u->peers[i].used && u->peers[i].addr == addr) {
            return &u->peers[i];
        }
    }
    return NULL;
}

static dhp_uhrp_peer_t *peer_get(dhp_uhrp_t *u, dhp_addr_t addr)
{
    dhp_uhrp_peer_t *p = peer_find(u, addr);
    if (p) {
        return p;
    }
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (!u->peers[i].used) {
            memset(&u->peers[i], 0, sizeof(u->peers[i]));
            u->peers[i].used = true;
            u->peers[i].addr = addr;
            return &u->peers[i];
        }
    }
    return NULL; /* chain larger than DHP_MAX_BOARDS; ignore the newcomer */
}

/* Drop peers we have not heard from within the hold time. This is the only
 * place a peer disappears, so failover detection has exactly one source. */
static void peer_age(dhp_uhrp_t *u, dhp_time_t now)
{
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        dhp_uhrp_peer_t *p = &u->peers[i];
        if (p->used && dhp_time_after(now, p->last_seen + u->cfg.timing.hold_ms)) {
            p->used = false;
        }
    }
}

static dhp_uhrp_peer_t *find_active_peer(dhp_uhrp_t *u)
{
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (u->peers[i].used && u->peers[i].state == DHP_UHRP_ACTIVE) {
            return &u->peers[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- *
 * State entry
 * ---------------------------------------------------------------------- */

static void enter_state(dhp_uhrp_t *u, dhp_uhrp_state_t s, dhp_time_t now)
{
    if (u->state == s) {
        return;
    }
    u->ev.prev_state = u->state;
    u->ev.state_changed = true;
    u->state = s;
    u->t_state_entered = now;
}

/* Become the active speaker.
 *
 * `clean` distinguishes the two arrival paths, and it is the whole of rule 2:
 * a graceful RESIGN means the outgoing board already lifted its keys, whereas
 * a hold-timer expiry means it may have died mid-chord and left a modifier
 * stuck on a machine nobody is watching. */
static void become_active(dhp_uhrp_t *u, dhp_time_t now, bool clean)
{
    const bool succeeded_someone = u->active_valid;

    enter_state(u, DHP_UHRP_ACTIVE, now);
    u->active = u->cfg.addr;
    u->active_uid = u->cfg.uid;
    u->active_priority = u->cfg.priority;
    u->active_valid = true;
    u->resign_pending = false;

    if (succeeded_someone && !clean) {
        u->ev.promoted_by_timeout = true;
        u->ev.broadcast_release = true;
    }

    /* Announce immediately rather than waiting for the next hello slot: the
     * sooner the chain knows, the shorter the window in which two boards
     * could both think they are active. */
    u->ev.send_hello = true;
    u->t_next_hello = now + u->cfg.timing.hello_ms;
}

/* ---------------------------------------------------------------------- *
 * Rule 1: a board that has been out-prioritised stands down only once its
 * held-key count reaches zero -- bounded by a deadline so that a stuck key
 * cannot wedge the handover permanently.
 * ---------------------------------------------------------------------- */

static void schedule_resign(dhp_uhrp_t *u, dhp_time_t now)
{
    if (u->resign_pending) {
        return;
    }
    u->resign_pending = true;
    u->t_resign_deadline = now + u->cfg.timing.handover_defer_max_ms;
}

static void service_resign(dhp_uhrp_t *u, dhp_time_t now)
{
    if (!u->resign_pending) {
        return;
    }

    const bool deadline_passed = dhp_time_after(now, u->t_resign_deadline);

    if (u->keys_held && !deadline_passed) {
        return; /* rule 1: wait for the chord to finish */
    }

    if (u->keys_held) {
        /* Deadline reached with a key still down. Releasing is the lesser
         * evil: a key stuck forever on a machine nobody is watching is worse
         * than an interrupted chord. */
        u->ev.broadcast_release = true;
    }

    u->ev.send_resign = true;
    u->resign_pending = false;
    u->active = DHP_ADDR_NONE;
    enter_state(u, DHP_UHRP_LISTEN, now);
}

/* ---------------------------------------------------------------------- *
 * Central evaluation
 *
 * Rather than a literal HSRP transition ladder, every entry point funnels
 * into this one function, which derives the correct state from the current
 * facts. A state machine that recomputes from evidence cannot drift into an
 * inconsistent state the way an incrementally-updated one can.
 * ---------------------------------------------------------------------- */
static void reevaluate(dhp_uhrp_t *u, dhp_time_t now)
{
    if (!u->started) {
        return;
    }

    peer_age(u, now);

    dhp_uhrp_peer_t *act = find_active_peer(u);

    /* Best candidate overall, and best excluding whoever is active. */
    uint8_t best_p = u->cfg.priority;
    dhp_uid_t best_u = u->cfg.uid;
    bool best_is_self = true;

    uint8_t backup_p = 0;
    dhp_uid_t backup_u = 0;
    dhp_addr_t backup_addr = DHP_ADDR_NONE;
    bool backup_is_self = false;

    if (u->state != DHP_UHRP_ACTIVE) {
        backup_p = u->cfg.priority;
        backup_u = u->cfg.uid;
        backup_addr = u->cfg.addr;
        backup_is_self = true;
    }

    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        const dhp_uhrp_peer_t *p = &u->peers[i];
        if (!p->used) {
            continue;
        }
        if (better(p->priority, p->uid, best_p, best_u)) {
            best_p = p->priority;
            best_u = p->uid;
            best_is_self = false;
        }
        if (p->state != DHP_UHRP_ACTIVE &&
            better(p->priority, p->uid, backup_p, backup_u)) {
            backup_p = p->priority;
            backup_u = p->uid;
            backup_addr = p->addr;
            backup_is_self = false;
        }
    }
    u->standby = backup_addr;

    /* ---- LEARN: listen before speaking, so a board plugged into a running
     * chain joins it rather than fighting it. ---- */
    if (u->state == DHP_UHRP_LEARN) {
        if (act) {
            u->active = act->addr;
            u->active_uid = act->uid;
            u->active_priority = act->priority;
            u->active_valid = true;
            enter_state(u, backup_is_self ? DHP_UHRP_STANDBY : DHP_UHRP_LISTEN,
                        now);
            return;
        }
        if (!dhp_time_after(now, u->t_state_entered + u->cfg.timing.learn_ms)) {
            return; /* still learning the field */
        }
        /* The learn window has closed and every live board has advertised at
         * least twice, so the field is known and there is nothing left to
         * discover by spending another hold time in SPEAK. */
        if (best_is_self) {
            become_active(u, now, true); /* initial election, nothing to clean */
        } else {
            enter_state(u, backup_is_self ? DHP_UHRP_STANDBY : DHP_UHRP_LISTEN,
                        now);
        }
        return;
    }

    /* ---- We hold the role ---- */
    if (u->state == DHP_UHRP_ACTIVE) {
        if (act && better(act->priority, act->uid, u->cfg.priority, u->cfg.uid)) {
            /* Split brain: two boards believe they are active. The weaker one
             * stands down. */
            schedule_resign(u, now);
        } else if (!best_is_self) {
            /* A more capable board has appeared -- typically the coordinator
             * finishing its boot. Hand over, but only on its terms: it must
             * actually want the role. */
            for (int i = 0; i < DHP_MAX_BOARDS; i++) {
                const dhp_uhrp_peer_t *p = &u->peers[i];
                if (p->used && p->priority == best_p && p->uid == best_u &&
                    (p->flags & DHP_HELLO_F_PREEMPT)) {
                    schedule_resign(u, now);
                    break;
                }
            }
        }
        service_resign(u, now);
        return;
    }

    /* ---- Somebody else holds the role ---- */
    if (act) {
        u->active = act->addr;
        u->active_uid = act->uid;
        u->active_priority = act->priority;
        u->active_valid = true;
        u->t_active_expiry = act->last_seen + u->cfg.timing.hold_ms;
        enter_state(u, backup_is_self ? DHP_UHRP_STANDBY : DHP_UHRP_LISTEN, now);
        return;
    }

    /* ---- Nobody holds the role ---- */
    u->active = DHP_ADDR_NONE;

    if (!best_is_self) {
        enter_state(u, backup_is_self ? DHP_UHRP_STANDBY : DHP_UHRP_LISTEN, now);
        return;
    }

    if (u->state == DHP_UHRP_STANDBY) {
        /* The pre-election pays off here: the backup was already chosen, so
         * this is a promotion and not a fresh election. No contention window,
         * which is what keeps recovery inside one hold time. */
        become_active(u, now, u->vacancy_clean);
        return;
    }

    /* No pre-elected backup was available (for instance the standby died at
     * the same instant). Fall back to contending for one hold time. */
    if (u->state != DHP_UHRP_SPEAK) {
        enter_state(u, DHP_UHRP_SPEAK, now);
        return;
    }
    if (dhp_time_after(now, u->t_state_entered + u->cfg.timing.hold_ms)) {
        become_active(u, now, u->vacancy_clean);
    }
}

/* ---------------------------------------------------------------------- *
 * Public entry points
 * ---------------------------------------------------------------------- */

void dhp_uhrp_init(dhp_uhrp_t *u, const dhp_uhrp_config_t *cfg)
{
    memset(u, 0, sizeof(*u));
    u->cfg = *cfg;
    if (u->cfg.priority == 0) {
        u->cfg.priority = dhp_uhrp_priority_from_caps(cfg->caps);
    }
    if (u->cfg.timing.hello_ms == 0) {
        u->cfg.timing = DHP_UHRP_TIMING_DEFAULT;
    }
    u->state = DHP_UHRP_INIT;
    u->active = DHP_ADDR_NONE;
    u->standby = DHP_ADDR_NONE;
}

void dhp_uhrp_start(dhp_uhrp_t *u, dhp_time_t now)
{
    u->started = true;
    u->vacancy_clean = true; /* nothing has died yet, so nothing to release */
    enter_state(u, DHP_UHRP_LEARN, now);
    u->t_state_entered = now;
    u->t_next_hello = now; /* advertise at once so peers learn us immediately */
}

void dhp_uhrp_rx_hello(dhp_uhrp_t *u, dhp_addr_t src, const dhp_hello_t *h,
                       dhp_time_t now)
{
    if (!u->started || src == u->cfg.addr) {
        return;
    }

    dhp_uhrp_peer_t *p = peer_get(u, src);
    if (!p) {
        return;
    }

    /* A peer announcing itself active means any earlier vacancy is over. */
    if (h->state == DHP_UHRP_ACTIVE && u->active != src) {
        u->vacancy_clean = true;
    }

    p->uid = h->uid;
    p->priority = h->priority;
    p->state = h->state;
    p->flags = h->flags;
    p->caps = h->caps;
    p->last_seen = now;

    reevaluate(u, now);
}

void dhp_uhrp_rx_resign(dhp_uhrp_t *u, dhp_addr_t src, dhp_time_t now)
{
    if (!u->started) {
        return;
    }

    dhp_uhrp_peer_t *p = peer_find(u, src);
    if (p) {
        /* Demote it in our table; it is no longer a candidate for active. */
        p->state = DHP_UHRP_LISTEN;
        p->last_seen = now;
    }

    if (src == u->active) {
        /* Graceful departure: it lifted its own keys on the way out, so the
         * board that takes over must NOT broadcast a release. That is the
         * difference between a planned handover and a failover. */
        u->vacancy_clean = true;
        u->active = DHP_ADDR_NONE;
    }

    reevaluate(u, now);
}

void dhp_uhrp_set_keys_held(dhp_uhrp_t *u, bool held)
{
    u->keys_held = held;
}

void dhp_uhrp_tick(dhp_uhrp_t *u, dhp_time_t now)
{
    if (!u->started) {
        return;
    }

    /* A live active peer that stops advertising is the failover trigger. Note
     * the vacancy is *not* clean: it vanished without warning. */
    if (u->state != DHP_UHRP_ACTIVE && u->active != DHP_ADDR_NONE) {
        const dhp_uhrp_peer_t *p = peer_find(u, u->active);
        if (!p || dhp_time_after(now, p->last_seen + u->cfg.timing.hold_ms)) {
            u->vacancy_clean = false;
        }
    }

    reevaluate(u, now);

    if (dhp_time_after(now, u->t_next_hello)) {
        u->ev.send_hello = true;
        u->t_next_hello = now + u->cfg.timing.hello_ms;
    }
}

dhp_uhrp_events_t dhp_uhrp_take_events(dhp_uhrp_t *u)
{
    const dhp_uhrp_events_t out = u->ev;
    memset(&u->ev, 0, sizeof(u->ev));
    return out;
}

void dhp_uhrp_build_hello(const dhp_uhrp_t *u, dhp_hello_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = (uint8_t)u->state;
    out->priority = u->cfg.priority;
    out->uid = u->cfg.uid;
    out->caps = u->cfg.caps;
    out->hold_cs = (uint8_t)(u->cfg.timing.hold_ms / 10);

    uint8_t f = 0;
    if (u->cfg.preempt)              f |= DHP_HELLO_F_PREEMPT;
    if (u->keys_held)                f |= DHP_HELLO_F_KEYS_HELD;
    if (u->cfg.caps & DHP_CAP_HID_IN) f |= DHP_HELLO_F_HAS_INPUT;
    if (u->cfg.caps & DHP_CAP_COORD)  f |= DHP_HELLO_F_COORD;
    out->flags = f;
}

uint16_t dhp_uhrp_reachable_caps(const dhp_uhrp_t *u)
{
    uint16_t caps = u->cfg.caps;
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (u->peers[i].used) {
            caps |= u->peers[i].caps;
        }
    }
    return caps;
}
