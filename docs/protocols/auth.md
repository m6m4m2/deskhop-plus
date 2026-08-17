# Authentication

Every frame is tagged, so a board attached by someone else cannot claim the
coordinator role.

## The threat

The chain is a physical bus running between several computers. Anything that
can be plugged into it can, without authentication, claim to be the most
capable board, win the election, and thereby become the thing that decides
where every keystroke goes.

That is the attack worth preventing, and it is why **role advertisements** are
authenticated and not merely the input frames. An attacker who can only replay
mouse movements is a nuisance; one who can become the router is not.

## Frame tagging

Every frame carries a 32-bit truncated **SipHash-2-4** MAC under the chain key,
covering the header and payload but not the hop limit (see
[link-framing.md](link-framing.md) for why).

SipHash rather than HMAC-SHA256 because frames are tiny and frequent: a 1 kHz
mouse generates 1000 MACs per second on every board in the chain, and SipHash
was designed precisely for short authenticated messages on machines with no
crypto acceleration. The RP2040 has none.

The implementation is verified against the published reference vectors and
cross-checked against an independent implementation
(`tests/test_siphash.c`). MAC comparison is constant-time — `memcmp` would leak
the position of the first differing byte, which is enough to forge one byte at
a time.

A board with no chain key can take part in pairing and **nothing else**: it
cannot originate a frame, and frames arriving at it are neither acted on nor
relayed, so it cannot be used as a bridge into somebody else's chain.

## Pairing

Boards trust each other only after a **deliberate physical action on both** — a
button held on each within the same window. That gives a channel the attacker
does not have: presence at the desk.

```
A                                          B
│  button held                             │  button held
│                                          │
├──── OFFER  {uid, ephemeral public, nonce} ──────────►
◄──────────── OFFER {uid, ephemeral public, nonce} ────┤
│                                          │
│  derive shared secret, chain key, SAS    │  (same)
│                                          │
├──── CONFIRM {uid, tag} ──────────────────────────────►
◄─────────────────────────── CONFIRM {uid, tag} ───────┤
│                                          │
│  both verify → chain key committed       │
```

### Derivation

The transcript is ordered by uid so both sides build it identically without an
extra round trip to decide who is first:

```
transcript = uid_lo ‖ public_lo ‖ nonce_lo ‖ uid_hi ‖ public_hi ‖ nonce_hi
```

Three separate derivations from the same shared secret, each with its own
context string so a key derived for one purpose can never be used for another:

| | Context | |
|---|---|---|
| chain key | `dhp/v1 chain-key` | 16 bytes, the frame MAC key |
| confirmation | `dhp/v1 confirm` ‖ transcript ‖ **sender uid** | 16 bytes |
| SAS | `dhp/v1 sas` | 3 bytes → six hex digits |

The confirmation is bound to the *sender's* uid, so it cannot be reflected back
at the sender as its own confirmation.

## Machine-in-the-middle

The exchange is an ephemeral Diffie–Hellman and is unauthenticated on its own —
there is no prior secret with which to authenticate it. So the defence is not
to try, but to make a MITM **visible**.

A MITM must answer both sides. Both sides therefore see more offers than the
one they expect.

> **If a third device joins that exchange, everyone aborts rather than trusting
> anyone.**

Refusing to pair is always available and always safe; guessing which of two
devices to trust is not. Pairing again costs the user five seconds, whereas
trusting the wrong device costs them every keystroke.

A completed pairing stays revocable while the window is open, because a MITM
answers quickly and the third offer may arrive just afterwards. Once the window
closes the pairing is settled, and a later offer is simply somebody else
starting their own exchange — which is how a board is added to the chain
afterwards, and must not revoke anything.

### Second, independent check

The exchange derives a short authentication string from the transcript. Both
boards can display it, and a MITM cannot match both. This check is optional and
belongs to the display, so it lives at level 2 — the abort-on-third-party rule
works with no display at all.

## Other abort conditions

| Reason | Cause |
|---|---|
| `THIRD_PARTY` | more than one peer answered |
| `BAD_CONFIRM` | tag mismatch — a different shared secret, which is what a MITM produces |
| `BAD_KEY` | degenerate peer public key |
| `TIMEOUT` | the window closed with only one button pressed |
| `PEER` | the other side aborted |

Any abort wipes the derived key and the ephemeral secret: a failed pairing must
not leave a half-usable key in memory.

## Crypto backend

The primitives are supplied by the caller through `dhp_crypto_t` rather than
implemented in `core/`. Hand-rolling X25519 is a good way to ship a subtly
broken curve implementation, so the intended backend is
[Monocypher](https://monocypher.org) — public domain, single file, small enough
for an RP2040.

Randomness must come from the RP2040's ring-oscillator entropy source, not from
a PRNG seeded at boot: boards run identical firmware and would otherwise agree
on their "random" values.

The tests substitute a mock backend, so the state machine is exercised
independently of the mathematics.

## Implementation

`core/include/dhp/auth.h`, `core/src/auth.c`. Tested in `tests/test_auth.c`,
including third-party abort, late third-party abort, tampered confirmation,
timeout, and that the derived key actually authenticates real frames end to
end.
