/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Trust establishment.
 *
 * Every frame carries a MAC under a chain key, so a board that does not hold
 * the key cannot advertise a priority, cannot claim the coordinator role, and
 * cannot inject a keystroke. This module is how a board comes to hold that key.
 *
 * The threat being addressed
 * -------------------------
 * The chain is a physical bus running between several computers. Anything that
 * can be plugged into it can, without authentication, claim to be the most
 * capable board, win the election, and thereby become the thing that decides
 * where every keystroke goes. That is the attack worth preventing, and it is
 * why role advertisements are authenticated rather than just the input frames.
 *
 * Pairing
 * -------
 * Boards trust each other only after a deliberate physical action on both --
 * a button held on each within the same window. That gives us a channel the
 * attacker does not have: presence at the desk.
 *
 * The exchange itself is an ephemeral Diffie-Hellman, which is unauthenticated
 * on its own and therefore open to a machine-in-the-middle. The defence is not
 * to try to authenticate it cryptographically -- there is no prior secret to
 * do that with -- but to make a machine-in-the-middle visible: a MITM must
 * answer both sides, so both sides see more offers than the one they expect.
 * Hence the rule that if a third device joins the exchange, everyone aborts
 * rather than anyone deciding which of the two to trust. Refusing to pair is
 * always available and always safe; guessing is not.
 *
 * As a second, independent check, the exchange derives a short authentication
 * string from the transcript. Both boards can show it, and if a user compares
 * them a MITM cannot match both. That check is optional and belongs to the
 * display, so it lives at level 2; the abort-on-third-party rule works with no
 * display at all.
 *
 * Crypto backend
 * --------------
 * The primitives are supplied by the caller through dhp_crypto_t rather than
 * implemented here. Hand-rolling X25519 is a good way to ship a subtly broken
 * curve implementation, so the intended backend is Monocypher (public domain,
 * single file, and small enough for an RP2040); the tests substitute a mock so
 * that the state machine is exercised independently of the mathematics.
 */
#ifndef DHP_AUTH_H
#define DHP_AUTH_H

#include "dhp/msg.h"
#include "dhp/siphash.h"
#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHP_DH_PUBLIC_BYTES 32
#define DHP_DH_SECRET_BYTES 32
#define DHP_DH_SHARED_BYTES 32
#define DHP_NONCE_BYTES      8
#define DHP_CONFIRM_BYTES   16
#define DHP_SAS_BYTES        3  /* six hex digits: enough for a human check */

typedef struct {
    /* Cryptographically secure randomness. On an RP2040 this must come from
     * the ring-oscillator entropy source, not from a PRNG seeded at boot --
     * boards are identical and would otherwise agree on their "random"
     * values. */
    void (*random)(void *ctx, uint8_t *out, size_t len);

    void (*dh_keypair)(void *ctx, uint8_t secret[DHP_DH_SECRET_BYTES],
                       uint8_t public_key[DHP_DH_PUBLIC_BYTES]);

    /* Must return false for a degenerate peer key (all-zero shared secret). */
    bool (*dh_shared)(void *ctx, uint8_t out[DHP_DH_SHARED_BYTES],
                      const uint8_t secret[DHP_DH_SECRET_BYTES],
                      const uint8_t peer_public[DHP_DH_PUBLIC_BYTES]);

    /* A PRF over (key material, context string). Any of HKDF, BLAKE2b keyed,
     * or SHA-256 based construction is fine. */
    void (*kdf)(void *ctx, uint8_t *out, size_t out_len,
                const uint8_t *ikm, size_t ikm_len,
                const uint8_t *info, size_t info_len);

    void *ctx;
} dhp_crypto_t;

/* PAIR payload subtypes. */
typedef enum {
    DHP_PAIR_OFFER   = 0,
    DHP_PAIR_CONFIRM = 1,
    DHP_PAIR_ABORT   = 2,
} dhp_pair_sub_t;

typedef enum {
    DHP_PAIR_IDLE    = 0,
    DHP_PAIR_WAITING = 1, /* button held, offer broadcast, listening */
    DHP_PAIR_CONFIRM_WAIT = 2, /* one offer seen, confirm sent */
    DHP_PAIR_DONE    = 3,
    DHP_PAIR_ABORTED = 4,
} dhp_pair_state_t;

typedef enum {
    DHP_ABORT_NONE = 0,
    DHP_ABORT_THIRD_PARTY = 1, /* more than one peer answered */
    DHP_ABORT_TIMEOUT = 2,
    DHP_ABORT_BAD_CONFIRM = 3,
    DHP_ABORT_BAD_KEY = 4,
    DHP_ABORT_PEER = 5,        /* the other side aborted */
} dhp_pair_abort_t;

typedef struct {
    /* One-shot outputs, cleared by dhp_pair_take_events(). */
    bool send_offer;
    bool send_confirm;
    bool send_abort;
    bool completed;   /* chain_key is now valid */
    bool aborted;
    uint8_t abort_reason;
} dhp_pair_events_t;

typedef struct {
    dhp_crypto_t crypto;
    dhp_uid_t    self_uid;

    dhp_pair_state_t state;
    dhp_time_t   t_started;
    uint16_t     window_ms;

    uint8_t  secret[DHP_DH_SECRET_BYTES];
    uint8_t  self_public[DHP_DH_PUBLIC_BYTES];
    uint8_t  self_nonce[DHP_NONCE_BYTES];

    dhp_uid_t peer_uid;
    uint8_t   peer_public[DHP_DH_PUBLIC_BYTES];
    uint8_t   peer_nonce[DHP_NONCE_BYTES];
    bool      have_peer;

    /* Count of DISTINCT peers that have offered. More than one means somebody
     * is in the middle, or a third board joined; either way we refuse. */
    uint8_t   offers_seen;

    uint8_t   chain_key[DHP_SIPHASH_KEY_BYTES];
    uint8_t   expect_confirm[DHP_CONFIRM_BYTES];
    uint8_t   sas[DHP_SAS_BYTES];
    bool      peer_confirmed;

    dhp_pair_abort_t abort_reason;
    dhp_pair_events_t ev;
} dhp_pair_t;

void dhp_pair_init(dhp_pair_t *p, const dhp_crypto_t *crypto, dhp_uid_t uid);

/* The deliberate physical action: the user has held the pairing button.
 * `window_ms` is how long this board will stay receptive (30 s is sane). */
void dhp_pair_begin(dhp_pair_t *p, dhp_time_t now, uint16_t window_ms);

/* Handle a received DHP_MSG_PAIR payload. */
void dhp_pair_rx(dhp_pair_t *p, const uint8_t *payload, uint8_t len,
                 dhp_time_t now);

void dhp_pair_tick(dhp_pair_t *p, dhp_time_t now);

dhp_pair_events_t dhp_pair_take_events(dhp_pair_t *p);

/* Build the payload the events asked to be sent. Returns the length, or 0. */
uint8_t dhp_pair_build_offer(const dhp_pair_t *p, uint8_t *out, size_t cap);
uint8_t dhp_pair_build_confirm(const dhp_pair_t *p, uint8_t *out, size_t cap);
uint8_t dhp_pair_build_abort(const dhp_pair_t *p, uint8_t *out, size_t cap);

/* Valid once events reported `completed`. */
const uint8_t *dhp_pair_chain_key(const dhp_pair_t *p);

/* Six hex digits for the user to compare across the two boards. Only
 * meaningful once the exchange has produced a shared secret. */
void dhp_pair_sas_string(const dhp_pair_t *p, char out[7]);

const char *dhp_pair_state_name(dhp_pair_state_t s);
const char *dhp_pair_abort_name(dhp_pair_abort_t a);

#ifdef __cplusplus
}
#endif

#endif /* DHP_AUTH_H */
