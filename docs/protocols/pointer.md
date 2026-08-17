# Pointer crossing

Relative dead-reckoning. Crossing is "keep pushing at the edge", which is why
it needs no layout configuration.

## What upstream does, and why this does something else

Upstream DeskHop rewrites the mouse HID descriptor to report **absolute**
coordinates, tracks the cursor in pixel space, and switches when it reaches a
boundary. That works, and it obliges you to tell it how big each screen is and
how the screens are arranged — and it re-breaks whenever a monitor changes, a
resolution changes, or a machine reorders its displays.

This design stays relative and never models the screen at all.

## The reasoning

A device on the USB wire cannot see where the pointer is. With relative reports
it can only integrate what the user does. So consider what physically happens
when someone pushes off the side of a screen:

1. they move the mouse right; the pointer moves right;
2. the pointer reaches the real screen edge and stops;
3. they keep moving the mouse right, and the pointer stays pinned.

The device sees continued rightward motion in both step 3 and the middle of a
very wide screen. **The only thing that separates them is how far the user has
already travelled in that direction.**

So cumulative travel, saturated at a span wider than any plausible screen, is
the discriminator. Nothing here needs to know pixels.

## Mechanism

| | |
|---|---|
| `travel` | integrates `dx`, clamped to ±`span`. Reaching the clamp means "you have moved further this way than any screen is wide, so the pointer is certainly against the edge by now". |
| `push` | accumulates **only** while `travel` is saturated, and decays with time. |

The decay is what turns the rule into "keep pushing" rather than "happen to end
up at the edge". Resting the mouse against the edge never crosses; a deliberate
shove does.

Movement away from the edge zeroes `push` immediately, so a shove interrupted
by a correction does not silently resume where it left off.

## Defaults

| | Default | Meaning |
|---|---|---|
| `span` | 8000 counts | 8 inches at 1000 dpi — no single screen needs this in one traverse |
| `push_threshold` | 600 counts | |
| `push_decay_per_ms` | 4 | |
| `reentry_guard` | 1500 counts | |
| `vertical_enabled` | false | |

The decay rate matters more as a **speed floor** than as a decay. A mouse moving
slower than 4 counts/ms — 4 inches per second at 1000 dpi — can never
accumulate anything, however long it is held against the edge. So a slow drift
or a hand resting on the mouse cannot switch machines. A deliberate shove at an
ordinary 20 counts/ms nets 16/ms and crosses in under 40 ms.

## After a crossing

The integrator is reset to the far side, less a guard band. Landing saturated
against the opposite edge would make the reverse crossing available instantly
and let a crossing bounce straight back; landing in the centre would make "I
overshot, go back" cost a full traverse.

The guard band is the middle: going straight back is a short deliberate
movement, which is a common and legitimate correction, but it still costs more
than zero.

## The cost of needing no configuration

The integrator is deliberately decoupled from the real pointer position, and it
must be. The OS applies its own acceleration curve, the real edge clamps the
pointer while the integrator keeps counting, and applications warp the cursor
whenever they like.

Any attempt to keep the two in agreement would be exactly the fragile screen
model this design is avoiding. Because the crossing test is a *gesture* and not
a coordinate, that drift is harmless — it is the reason the approach is robust
rather than a defect in it.

The honest trade: crossing costs a deliberate shove rather than a flick, and
back-to-back crossings across a long chain cost one traverse each. In exchange
there is nothing to configure, ever, on any monitor arrangement.

## Vertical

Both axes are implemented; vertical is off by default so that a chain laid out
left-to-right cannot be switched by vertical mouse movement. Enable it for a
stacked arrangement.

## Direction and chain order

A left-edge crossing moves up-chain and a right-edge crossing moves down-chain,
relative to **the machine the user is currently on** — not relative to the board
making the decision. Those are routinely different: once the coordinator takes
the active role it owns routing while sitting at the end of the chain, and
measuring from itself would send the pointer to its own neighbour instead of
the focused machine's.

Boards that are not attached to a machine — a coordinator — are skipped, since
they are on the chain but are not a screen.

Pushing past the last machine does nothing, which is the right behaviour at the
end of a desk.

## Implementation

`core/include/dhp/pointer.h`, `core/src/pointer.c`.

Tested in `tests/test_pointer.c`: 40 traverses of ordinary movement with no
spurious crossing, shove-crosses in both directions, decay defeating a shove
delivered in instalments, reversal cancelling a nearly-complete gesture, and the
re-entry guard. Integrated behaviour in `tests/test_sim.c`.
