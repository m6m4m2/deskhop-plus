/* SPDX-License-Identifier: GPL-2.0-only */
#include "sim.h"

#include <string.h>

/* ---- wires ----------------------------------------------------------- */

static void wire_put(sim_wire_t *w, const uint8_t *d, size_t n)
{
    if (w->cut) {
        w->dropped += (uint32_t)n;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const int next = (w->tail + 1) % SIM_WIRE_CAP;
        if (next == w->head) {
            w->dropped++;
            return;
        }
        w->buf[w->tail] = d[i];
        w->tail = next;
    }
}

static bool wire_get(sim_wire_t *w, uint8_t *out)
{
    if (w->head == w->tail) {
        return false;
    }
    *out = w->buf[w->head];
    w->head = (w->head + 1) % SIM_WIRE_CAP;
    return true;
}

static void wire_clear(sim_wire_t *w)
{
    w->head = w->tail = 0;
}

/* ---- node callbacks -------------------------------------------------- */

static void on_tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    sim_node_t *n = ctx;
    sim_t *s = n->owner;

    if (port == DHP_PORT_DOWN) {
        if (n->idx + 1 < s->n) {
            wire_put(&s->down[n->idx], data, len);
        }
    } else {
        if (n->idx > 0) {
            wire_put(&s->up[n->idx], data, len);
        }
    }
}

static bool is_zero_kbd(const dhp_kbd_report_t *r)
{
    if (r->modifiers) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (r->keys[i]) {
            return false;
        }
    }
    return true;
}

static void on_kbd(void *ctx, const dhp_kbd_report_t *r)
{
    sim_node_t *n = ctx;
    n->last_kbd = *r;
    n->kbd_count++;
    if (is_zero_kbd(r)) {
        n->kbd_release_count++;
    }
    /* Model what the attached machine believes: exactly the last report it
     * was given. This is how the tests detect a key left stuck. */
    n->held_mods = r->modifiers;
    memcpy(n->held_keys, r->keys, 6);
}

static void on_mouse(void *ctx, const dhp_mouse_report_t *r)
{
    sim_node_t *n = ctx;
    n->last_mouse = *r;
    n->mouse_count++;
    n->held_buttons = r->buttons;
}

static void on_focus(void *ctx, dhp_addr_t focus, bool is_self)
{
    (void)is_self;
    sim_node_t *n = ctx;
    n->focus = focus;
    n->focus_changes++;
}

static void on_level(void *ctx, dhp_level_t level)
{
    sim_node_t *n = ctx;
    n->level = level;
    n->level_changes++;
}

/* ---- construction ---------------------------------------------------- */

void sim_init(sim_t *s, dhp_time_t start)
{
    memset(s, 0, sizeof(*s));
    s->now = start;
    for (int i = 0; i < DHP_SIPHASH_KEY_BYTES; i++) {
        s->key[i] = (uint8_t)(0xA0 + i);
    }
}

int sim_add(sim_t *s, uint16_t caps, bool preempt, dhp_uid_t uid)
{
    const int i = s->n++;
    sim_node_t *n = &s->node[i];
    memset(n, 0, sizeof(*n));
    n->owner = s;
    n->idx = i;
    n->focus = DHP_ADDR_NONE;
    n->level = DHP_LEVEL_ISOLATED;

    const dhp_addr_t addr = (dhp_addr_t)(i + 1);
    dhp_link_init(&n->link, addr, on_tx, n);
    dhp_link_set_key(&n->link, s->key);

    const dhp_router_cfg_t cfg = {
        .self = addr, .uid = uid, .caps = caps, .preempt = preempt,
        .timing = DHP_UHRP_TIMING_DEFAULT,
        .pointer = DHP_POINTER_CFG_DEFAULT,
    };
    const dhp_router_hooks_t hooks = {
        .deliver_kbd = on_kbd, .deliver_mouse = on_mouse,
        .focus_changed = on_focus, .level_changed = on_level, .ctx = n,
    };
    dhp_router_init(&n->router, &cfg, &n->link, &hooks);
    return i;
}

void sim_power(sim_t *s, int idx, bool on)
{
    sim_node_t *n = &s->node[idx];
    if (on && !n->powered) {
        n->powered = true;
        dhp_router_start(&n->router, s->now);
    } else if (!on && n->powered) {
        n->powered = false;
        /* Whatever was in flight from it is lost, exactly as when a board is
         * pulled out mid-frame. */
        if (idx > 0)         wire_clear(&s->up[idx]);
        if (idx + 1 < s->n)  wire_clear(&s->down[idx]);
    }
}

void sim_cut(sim_t *s, int idx, bool cut)
{
    s->down[idx].cut = cut;
    s->up[idx + 1].cut = cut;
    if (cut) {
        wire_clear(&s->down[idx]);
        wire_clear(&s->up[idx + 1]);
    }
}

/* ---- time ------------------------------------------------------------ */

static void deliver(sim_t *s)
{
    for (int i = 0; i < s->n; i++) {
        uint8_t b;

        /* down[i] : node i -> node i+1, arriving on that node's up port. */
        if (i + 1 < s->n) {
            while (wire_get(&s->down[i], &b)) {
                sim_node_t *dst = &s->node[i + 1];
                if (!dst->powered) {
                    continue;
                }
                dhp_frame_t f;
                if (dhp_link_rx_byte(&dst->link, DHP_PORT_UP, b, s->now, &f) ==
                    DHP_OK) {
                    dhp_router_rx(&dst->router, &f, DHP_PORT_UP, s->now);
                }
            }
        }

        /* up[i] : node i -> node i-1, arriving on that node's down port. */
        if (i > 0) {
            while (wire_get(&s->up[i], &b)) {
                sim_node_t *dst = &s->node[i - 1];
                if (!dst->powered) {
                    continue;
                }
                dhp_frame_t f;
                if (dhp_link_rx_byte(&dst->link, DHP_PORT_DOWN, b, s->now,
                                     &f) == DHP_OK) {
                    dhp_router_rx(&dst->router, &f, DHP_PORT_DOWN, s->now);
                }
            }
        }
    }
}

void sim_step(sim_t *s)
{
    s->now++;
    deliver(s);
    for (int i = 0; i < s->n; i++) {
        if (s->node[i].powered) {
            dhp_router_tick(&s->node[i].router, s->now);
        }
    }
    deliver(s); /* let this millisecond's output land in this millisecond */
}

void sim_run(sim_t *s, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++) {
        sim_step(s);
    }
}

/* ---- input injection -------------------------------------------------- */

void sim_kbd(sim_t *s, int idx, uint8_t mods, uint8_t key1)
{
    dhp_kbd_report_t r;
    memset(&r, 0, sizeof(r));
    r.modifiers = mods;
    r.keys[0] = key1;
    dhp_router_local_kbd(&s->node[idx].router, &r, s->now);
}

void sim_mouse_move(sim_t *s, int idx, int16_t dx, int16_t dy)
{
    dhp_mouse_report_t r;
    memset(&r, 0, sizeof(r));
    r.dx = dx;
    r.dy = dy;
    dhp_router_local_mouse(&s->node[idx].router, &r, s->now);
}

void sim_mouse_buttons(sim_t *s, int idx, uint8_t buttons)
{
    dhp_mouse_report_t r;
    memset(&r, 0, sizeof(r));
    r.buttons = buttons;
    dhp_router_local_mouse(&s->node[idx].router, &r, s->now);
}

bool sim_push_edge(sim_t *s, int idx, int direction, uint32_t max_ms)
{
    const int before = sim_focus_idx(s);
    const int16_t d = (int16_t)(direction < 0 ? -20 : 20);

    for (uint32_t t = 0; t < max_ms; t++) {
        sim_mouse_move(s, idx, d, 0);
        sim_step(s);
        if (sim_focus_idx(s) != before) {
            return true;
        }
    }
    return false;
}

/* ---- queries ---------------------------------------------------------- */

int sim_active(const sim_t *s)
{
    for (int i = 0; i < s->n; i++) {
        if (s->node[i].powered && dhp_router_is_active(&s->node[i].router)) {
            return i;
        }
    }
    return -1;
}

int sim_active_count(const sim_t *s)
{
    int c = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->node[i].powered && dhp_router_is_active(&s->node[i].router)) {
            c++;
        }
    }
    return c;
}

int sim_focus_idx(const sim_t *s)
{
    const int a = sim_active(s);
    if (a < 0) {
        return -1;
    }
    const dhp_addr_t f = dhp_router_focus(&s->node[a].router);
    if (f == DHP_ADDR_NONE) {
        return -1;
    }
    return (int)f - 1;
}
