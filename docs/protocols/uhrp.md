# UHRP — the role election

Modelled on Cisco HSRP, with two rules HSRP does not need because HSRP routes
packets and this routes keystrokes.

## Shape

Boards advertise a priority. The most capable wins, ties break on the unique
per-board id, and a **backup is pre-elected** so that failover is a promotion
rather than an election.

That last property is what buys the recovery time. When the active speaker
vanishes there is nothing to decide — only a timer to expire.

## Priority

Derived from capability rather than configured, so "the most capable wins" is a
property of the hardware present and not of a config file somebody has to keep
in sync:

| Capability | Contribution |
|---|---|
| base | 50 |
| `COORD` | +100 |
| `HID_IN` (holds the real keyboard) | +40 |
| `DISPLAY` | +10 |
| `CONFIG` | +5 |

So a coordinator outranks any chain board, and among chain boards the one with
the keyboard attached wins. Ties break on the 64-bit uid from flash, which is
globally unique — so the ordering is total and the election always converges on
exactly one winner rather than oscillating.

## States

`INIT` → `LEARN` → `LISTEN` / `SPEAK` / `STANDBY` / `ACTIVE`

`LEARN` is listen-before-speak: a board plugged into a running chain joins it
rather than fighting it. The window is one hold time, and since every live board
advertises every hello interval, the field is fully known by the time it
closes — so the winner goes straight to `ACTIVE` without spending a further
hold time in `SPEAK`.

`SPEAK` is retained for the case where no pre-elected backup was available (for
instance the standby died at the same instant as the active), where contending
for one hold time is the correct fallback.

Rather than a literal HSRP transition ladder, every entry point funnels into one
function that derives the correct state from current facts. A state machine
that recomputes from evidence cannot drift into an inconsistent state the way an
incrementally-updated one can.

## Timing

| | Default | |
|---|---|---|
| hello interval | 50 ms | |
| hold time | 150 ms | three missed hellos |
| learn window | 150 ms | |
| handover defer deadline | 2000 ms | rule 1 safety valve |

Three strikes rather than one keeps a single dropped frame from causing a
spurious role change. Measured results from the test suite:

```
level 1 reached in 150 ms
failover in 150 ms
dropped back to level 1 in 151 ms
```

## Rule 1 — never hand over while a key is held

A planned handover in the middle of a chord would leave the outgoing board's
key-down unmatched by any key-up.

The deferral lives on the **active** side, because that is the board that knows
what it has sent: a board that has been out-prioritised resigns only once its
held-key count reaches zero. The challenger simply waits.

A genuinely stuck key must not wedge the handover forever, so the deferral is
bounded. Past `handover_defer_max_ms` the active board releases everything and
then resigns — a key stuck permanently on a machine nobody is watching is worse
than an interrupted chord.

## Rule 2 — release everything if the active speaker vanished without warning

A graceful resignation is clean: the outgoing board has already lifted its keys,
and the successor must **not** broadcast a release.

A timeout is not clean: the machine being typed on may be holding a modifier
with nobody left to lift it. So a board promoted by *timeout* rather than by
`RESIGN` broadcasts a release before it does anything else, and forgets its
tracked state — it cannot know what the dead board had sent.

Distinguishing the two paths is the entire reason `dhp_uhrp_events_t` reports
`promoted_by_timeout` separately from an ordinary state change.

## HELLO payload

16 bytes, sent every hello interval by every participating board.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | state |
| 1 | 1 | priority |
| 2 | 1 | flags |
| 3 | 1 | hold time, units of 10 ms |
| 4 | 8 | uid (LE) |
| 12 | 2 | capability bitmap (LE) |
| 14 | 2 | reserved |

Flags: `PREEMPT` (1), `KEYS_HELD` (2), `HAS_INPUT` (4), `COORD` (8).

## Split brain

Cut the chain in half and each fragment elects its own active speaker — which
is correct, because they are genuinely two separate systems at that point, and
each half stays usable. Reconnect them and the weaker active stands down, so
they merge back to one authority. Both directions are tested in
`tests/test_sim.c`.

## Implementation

`core/include/dhp/uhrp.h`, `core/src/uhrp.c`.

The module is pure: it never reads a clock, never sends a frame, and never
allocates. Time comes in as a parameter and actions come out as events, so a
whole chain can be simulated deterministically. Tested in `tests/test_uhrp.c`
(election, tiebreak, both rules, cascading failover, convergence stability) and
end to end in `tests/test_sim.c`.
