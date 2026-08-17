/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/auth.h"

#include <string.h>

/* Context strings, so that keys derived for one purpose can never be used for
 * another even though they come from the same shared secret. */
static const char INFO_KEY[]     = "dhp/v1 chain-key";
static const char INFO_CONFIRM[] = "dhp/v1 confirm";
static const char INFO_SAS[]     = "dhp/v1 sas";

const char *dhp_pair_state_name(dhp_pair_state_t s)
{
    switch (s) {
    case DHP_PAIR_IDLE:         return "idle";
    case DHP_PAIR_WAITING:      return "waiting";
    case DHP_PAIR_CONFIRM_WAIT: return "confirming";
    case DHP_PAIR_DONE:         return "done";
    case DHP_PAIR_ABORTED:      return "aborted";
    default:                    return "?";
    }
}

const char *dhp_pair_abort_name(dhp_pair_abort_t a)
{
    switch (a) {
    case DHP_ABORT_NONE:        return "none";
    case DHP_ABORT_THIRD_PARTY: return "third party joined";
    case DHP_ABORT_TIMEOUT:     return "timed out";
    case DHP_ABORT_BAD_CONFIRM: return "confirmation mismatch";
    case DHP_ABORT_BAD_KEY:     return "bad public key";
    case DHP_ABORT_PEER:        return "peer aborted";
    default:                    return "?";
    }
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (8 * i);
    }
    return v;
}

static void abort_with(dhp_pair_t *p, dhp_pair_abort_t why)
{
    /* A completed pairing can still be torn down. Reaching DONE is not a
     * guarantee that the exchange was between two parties -- a machine in the
     * middle answers quickly and a third offer may arrive just after -- so
     * "done" must remain revocable for as long as the window is open. Only an
     * already-aborted exchange is inert. */
    if (p->state == DHP_PAIR_ABORTED) {
        return;
    }
    p->state = DHP_PAIR_ABORTED;
    p->abort_reason = why;
    p->ev.send_abort = true;
    p->ev.aborted = true;
    p->ev.abort_reason = (uint8_t)why;

    /* Leave nothing derived behind: a failed pairing must not leave a
     * half-usable key in memory. */
    memset(p->chain_key, 0, sizeof(p->chain_key));
    memset(p->secret, 0, sizeof(p->secret));
}

void dhp_pair_init(dhp_pair_t *p, const dhp_crypto_t *crypto, dhp_uid_t uid)
{
    memset(p, 0, sizeof(*p));
    p->crypto = *crypto;
    p->self_uid = uid;
    p->state = DHP_PAIR_IDLE;
}

void dhp_pair_begin(dhp_pair_t *p, dhp_time_t now, uint16_t window_ms)
{
    const dhp_crypto_t crypto = p->crypto;
    const dhp_uid_t uid = p->self_uid;

    memset(p, 0, sizeof(*p));
    p->crypto = crypto;
    p->self_uid = uid;

    p->state = DHP_PAIR_WAITING;
    p->t_started = now;
    p->window_ms = window_ms ? window_ms : 30000;

    p->crypto.dh_keypair(p->crypto.ctx, p->secret, p->self_public);
    p->crypto.random(p->crypto.ctx, p->self_nonce, DHP_NONCE_BYTES);

    p->ev.send_offer = true;
}

/* Both boards must derive identical keys, so the transcript has to be built in
 * an order both agree on independently. Ordering by uid does that without an
 * extra round trip to decide who is "first". */
#define DHP_TRANSCRIPT_BYTES 96 /* 2 * (uid 8 + public 32 + nonce 8) */

static void build_transcript(const dhp_pair_t *p,
                             uint8_t out[DHP_TRANSCRIPT_BYTES])
{
    const bool self_first = p->self_uid < p->peer_uid;

    const uint8_t *pub_a = self_first ? p->self_public : p->peer_public;
    const uint8_t *non_a = self_first ? p->self_nonce : p->peer_nonce;
    const uint8_t *pub_b = self_first ? p->peer_public : p->self_public;
    const uint8_t *non_b = self_first ? p->peer_nonce : p->self_nonce;
    const dhp_uid_t uid_a = self_first ? p->self_uid : p->peer_uid;
    const dhp_uid_t uid_b = self_first ? p->peer_uid : p->self_uid;

    size_t o = 0;
    put64(&out[o], uid_a);                          o += 8;
    memcpy(&out[o], pub_a, DHP_DH_PUBLIC_BYTES);    o += DHP_DH_PUBLIC_BYTES;
    memcpy(&out[o], non_a, DHP_NONCE_BYTES);        o += DHP_NONCE_BYTES;
    put64(&out[o], uid_b);                          o += 8;
    memcpy(&out[o], pub_b, DHP_DH_PUBLIC_BYTES);    o += DHP_DH_PUBLIC_BYTES;
    memcpy(&out[o], non_b, DHP_NONCE_BYTES);        o += DHP_NONCE_BYTES;
}

/* Largest info buffer any derivation needs. */
#define DHP_INFO_MAX (sizeof(INFO_CONFIRM) + DHP_TRANSCRIPT_BYTES + 8)

/* Derive everything the exchange produces, once both offers are in hand. */
static bool derive(dhp_pair_t *p)
{
    uint8_t shared[DHP_DH_SHARED_BYTES];
    if (!p->crypto.dh_shared(p->crypto.ctx, shared, p->secret,
                             p->peer_public)) {
        return false;
    }

    uint8_t transcript[DHP_TRANSCRIPT_BYTES];
    build_transcript(p, transcript);

    uint8_t info[DHP_INFO_MAX];

    /* Chain key. */
    memcpy(info, INFO_KEY, sizeof(INFO_KEY));
    memcpy(info + sizeof(INFO_KEY), transcript, sizeof(transcript));
    p->crypto.kdf(p->crypto.ctx, p->chain_key, sizeof(p->chain_key), shared,
                  sizeof(shared), info, sizeof(INFO_KEY) + sizeof(transcript));

    /* Short authentication string, for the optional human check. */
    memcpy(info, INFO_SAS, sizeof(INFO_SAS));
    memcpy(info + sizeof(INFO_SAS), transcript, sizeof(transcript));
    p->crypto.kdf(p->crypto.ctx, p->sas, sizeof(p->sas), shared, sizeof(shared),
                  info, sizeof(INFO_SAS) + sizeof(transcript));

    /* The confirmation we expect from the peer is bound to the peer's uid, so
     * it cannot be replayed back at the sender as its own confirmation. */
    memcpy(info, INFO_CONFIRM, sizeof(INFO_CONFIRM));
    memcpy(info + sizeof(INFO_CONFIRM), transcript, sizeof(transcript));
    put64(info + sizeof(INFO_CONFIRM) + sizeof(transcript), p->peer_uid);
    p->crypto.kdf(p->crypto.ctx, p->expect_confirm, sizeof(p->expect_confirm),
                  shared, sizeof(shared), info,
                  sizeof(INFO_CONFIRM) + sizeof(transcript) + 8);

    memset(shared, 0, sizeof(shared));
    return true;
}

/* Our own confirmation, bound to our uid. Recomputed on demand so the value
 * never has to be stored. */
static bool build_own_confirm(const dhp_pair_t *p, uint8_t out[DHP_CONFIRM_BYTES])
{
    uint8_t shared[DHP_DH_SHARED_BYTES];
    if (!p->crypto.dh_shared(p->crypto.ctx, shared, p->secret,
                             p->peer_public)) {
        return false;
    }

    uint8_t transcript[DHP_TRANSCRIPT_BYTES];
    build_transcript(p, transcript);

    uint8_t info[DHP_INFO_MAX];
    memcpy(info, INFO_CONFIRM, sizeof(INFO_CONFIRM));
    memcpy(info + sizeof(INFO_CONFIRM), transcript, sizeof(transcript));
    put64(info + sizeof(INFO_CONFIRM) + sizeof(transcript), p->self_uid);

    p->crypto.kdf(p->crypto.ctx, out, DHP_CONFIRM_BYTES, shared, sizeof(shared),
                  info, sizeof(INFO_CONFIRM) + sizeof(transcript) + 8);
    memset(shared, 0, sizeof(shared));
    return true;
}

static void rx_offer(dhp_pair_t *p, const uint8_t *b, uint8_t len,
                     dhp_time_t now)
{
    (void)now;
    if (len < 1 + 8 + DHP_DH_PUBLIC_BYTES + DHP_NONCE_BYTES) {
        return;
    }

    const dhp_uid_t uid = get64(&b[1]);
    if (uid == p->self_uid) {
        return; /* our own offer echoed back */
    }

    if (p->have_peer && uid == p->peer_uid) {
        /* A retransmission from the peer we already have. Re-send our
         * confirmation rather than ignoring it: the peer only repeats its
         * offer because it has not finished, and our first confirmation may
         * have arrived before it had our offer -- in which case it had no
         * shared secret yet and could only discard it. Without this the
         * exchange deadlocks purely on message ordering. */
        if (p->state == DHP_PAIR_CONFIRM_WAIT) {
            p->ev.send_confirm = true;
        }
        return;
    }

    if (p->have_peer) {
        /* A second, different device has answered. One of them may be
         * relaying for the other, and there is no way to tell which. Refusing
         * is the only safe move: pairing again costs the user five seconds,
         * whereas trusting the wrong device costs them every keystroke. */
        p->offers_seen++;
        abort_with(p, DHP_ABORT_THIRD_PARTY);
        return;
    }

    p->peer_uid = uid;
    memcpy(p->peer_public, &b[9], DHP_DH_PUBLIC_BYTES);
    memcpy(p->peer_nonce, &b[9 + DHP_DH_PUBLIC_BYTES], DHP_NONCE_BYTES);
    p->have_peer = true;
    p->offers_seen = 1;

    if (!derive(p)) {
        abort_with(p, DHP_ABORT_BAD_KEY);
        return;
    }

    p->state = DHP_PAIR_CONFIRM_WAIT;
    p->ev.send_confirm = true;
    /* Re-offer as well: the peer may have begun before us and missed ours. */
    p->ev.send_offer = true;
}

static void rx_confirm(dhp_pair_t *p, const uint8_t *b, uint8_t len)
{
    if (len < 1 + 8 + DHP_CONFIRM_BYTES || !p->have_peer) {
        return;
    }
    if (get64(&b[1]) != p->peer_uid) {
        return;
    }

    if (!dhp_ct_equal(&b[9], p->expect_confirm, DHP_CONFIRM_BYTES)) {
        /* Either a different shared secret -- which is what a machine in the
         * middle produces -- or corruption. Neither is worth continuing. */
        abort_with(p, DHP_ABORT_BAD_CONFIRM);
        return;
    }

    p->peer_confirmed = true;
    if (p->state == DHP_PAIR_CONFIRM_WAIT) {
        p->state = DHP_PAIR_DONE;
        p->ev.completed = true;
    }
}

void dhp_pair_rx(dhp_pair_t *p, const uint8_t *payload, uint8_t len,
                 dhp_time_t now)
{
    if (len < 1 || p->state == DHP_PAIR_IDLE || p->state == DHP_PAIR_ABORTED) {
        return;
    }

    switch (payload[0]) {
    case DHP_PAIR_OFFER:
        /* An offer arriving after we are done means a third party turned up
         * late; the same rule applies. */
        if (p->state == DHP_PAIR_DONE) {
            /* Only while the window is still open. Once it has closed the
             * pairing is settled, and a later offer is simply somebody else
             * starting their own exchange -- which is how a board gets added
             * to the chain afterwards, and must not revoke anything. */
            const bool window_open =
                !dhp_time_after(now, p->t_started + p->window_ms);
            if (window_open && len >= 9 &&
                get64(&payload[1]) != p->peer_uid &&
                get64(&payload[1]) != p->self_uid) {
                abort_with(p, DHP_ABORT_THIRD_PARTY);
            }
            return;
        }
        rx_offer(p, payload, len, now);
        break;

    case DHP_PAIR_CONFIRM:
        rx_confirm(p, payload, len);
        break;

    case DHP_PAIR_ABORT:
        abort_with(p, DHP_ABORT_PEER);
        break;

    default:
        break;
    }
}

void dhp_pair_tick(dhp_pair_t *p, dhp_time_t now)
{
    if (p->state != DHP_PAIR_WAITING && p->state != DHP_PAIR_CONFIRM_WAIT) {
        return;
    }
    if (dhp_time_after(now, p->t_started + p->window_ms)) {
        abort_with(p, DHP_ABORT_TIMEOUT);
    }
}

dhp_pair_events_t dhp_pair_take_events(dhp_pair_t *p)
{
    const dhp_pair_events_t out = p->ev;
    memset(&p->ev, 0, sizeof(p->ev));
    return out;
}

uint8_t dhp_pair_build_offer(const dhp_pair_t *p, uint8_t *out, size_t cap)
{
    const size_t need = 1 + 8 + DHP_DH_PUBLIC_BYTES + DHP_NONCE_BYTES;
    if (cap < need) {
        return 0;
    }
    out[0] = DHP_PAIR_OFFER;
    put64(&out[1], p->self_uid);
    memcpy(&out[9], p->self_public, DHP_DH_PUBLIC_BYTES);
    memcpy(&out[9 + DHP_DH_PUBLIC_BYTES], p->self_nonce, DHP_NONCE_BYTES);
    return (uint8_t)need;
}

uint8_t dhp_pair_build_confirm(const dhp_pair_t *p, uint8_t *out, size_t cap)
{
    const size_t need = 1 + 8 + DHP_CONFIRM_BYTES;
    if (cap < need || !p->have_peer) {
        return 0;
    }
    uint8_t tag[DHP_CONFIRM_BYTES];
    if (!build_own_confirm(p, tag)) {
        return 0;
    }
    out[0] = DHP_PAIR_CONFIRM;
    put64(&out[1], p->self_uid);
    memcpy(&out[9], tag, DHP_CONFIRM_BYTES);
    return (uint8_t)need;
}

uint8_t dhp_pair_build_abort(const dhp_pair_t *p, uint8_t *out, size_t cap)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = DHP_PAIR_ABORT;
    out[1] = (uint8_t)p->abort_reason;
    return 2;
}

const uint8_t *dhp_pair_chain_key(const dhp_pair_t *p)
{
    return p->state == DHP_PAIR_DONE ? p->chain_key : NULL;
}

void dhp_pair_sas_string(const dhp_pair_t *p, char out[7])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < DHP_SAS_BYTES; i++) {
        out[i * 2]     = hex[(p->sas[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[p->sas[i] & 0xF];
    }
    out[6] = '\0';
}
