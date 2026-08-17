# Architecture

## The shape of the problem

One keyboard and mouse, several computers, and the machines should behave like
one desk rather than one machine at a time.

The device sits on the USB wire and is a keyboard and mouse *to each machine*.
That single decision determines almost everything else:

- It works before any operating system is running, because a USB HID keyboard
  is something a BIOS already understands.
- Nothing is installed anywhere, because there is nothing to install.
- It cannot see the screen, the pointer position, or anything else about the
  machine's state. Every capability has to be built out of what a keyboard and
  mouse can observe and emit.

That last constraint is the interesting one, and it is why the pointer design
looks the way it does.

## Modularity

Not one box with a fixed port count, but a chain of small boards — one per
computer. You extend it by adding a board.

The chain is a **line**, not a bus or a ring. Each board has an up port toward
the head and a down port toward the tail, which is exactly the two UARTs an
RP2040 provides. A line has no cycles, so a frame is relayed out the opposite
port from the one it arrived on and can never come back; the hop limit is a
backstop against miswiring rather than the primary loop defence.

Nothing needs configuring when you extend it. Each board derives every other
board's position relative to itself from two facts it already has for free:

- **which port** a frame arrived on gives the sign (up-chain or down-chain);
- **how far the hop limit has been decremented** gives the distance.

So a frame arriving on the up port having been relayed twice came from the
board three places to the left. No board needs an absolute index, because every
routing decision is relative anyway. Positions are recomputed from arriving
traffic, so plugging in a board reconfigures the system by the ordinary
operation of the protocol rather than by a discovery pass that has to be
triggered.

## Levels

The system comes up in stages and degrades the same way. The coordinator takes
fifteen to twenty seconds to boot; the device does not wait for it.

| Level | When | What you get |
|---|---|---|
| **0** Isolated | One board, no chain | Still a keyboard and mouse to its own machine — just nowhere to switch to |
| **1** Elected | Milliseconds after power-on | Keyboard and mouse shared across every machine, switching, pointer crossing |
| **2** Coordinated | Once the coordinator has booted | Status display and configuration |
| **3** Rich | With a client on a machine | Clipboard, text, files, folders, images, SMB share |

Two properties make this more than a boot sequence:

**Levels are derived, not configured.** A level is recomputed from the
capabilities actually reachable right now — see `dhp_level_of()`. The system
never advertises what it cannot currently deliver, and it never waits for
something it does not have.

**Level 3 is per-machine.** The chain can be at level 3 for one board and level
2 for its neighbour at the same instant, because only the machines running a
client get the data features. The others carry on exactly as before.

Degradation is the same mechanism in reverse. Unplug the coordinator and the
capability set loses `DISPLAY` and `CONFIG`, so the level drops to 1 — in about
150 ms, and without stopping. Measured in `tests/test_sim.c`.

## Handover, not restart

Level 1 → level 2 is the transition that matters in practice, because it
happens on every cold boot: a chain board wins the initial election in ~150 ms
and the coordinator takes over ~15 s later when it finishes booting.

That handover is planned. The coordinator advertises a higher priority and sets
the preempt flag; the incumbent resigns; the coordinator takes the role.
Because it is a resignation and not a failure, the outgoing board has already
lifted its keys, and the successor does *not* broadcast a release. The user's
focus is inherited rather than reset, because every board tracks the focus
broadcasts even while it is only standing by.

Not a keystroke is dropped, and nothing is interrupted.

## Separating input capture from focus authority

Once the coordinator holds the active role, it owns routing — and it has no
keyboard of its own. The keyboard is plugged into some particular board.

Rather than let two boards both believe they own routing, the board holding the
input devices forwards its reports to the active speaker (`DHP_MSG_INPUT`), and
the active speaker remains the single authority. Only a board that actually has
input devices attached may do this, so no other board can inject keystrokes.

This also gives the pointer integrator exactly one owner, which it needs: a
crossing decision made in two places at once is a crossing decision made
wrongly.

## The four protocols

| | Purpose | Detail |
|---|---|---|
| **UHRP** | Role election | [protocols/uhrp.md](protocols/uhrp.md) |
| **Link framing** | Getting bytes between boards | [protocols/link-framing.md](protocols/link-framing.md) |
| **Authentication** | Who is allowed to be on the chain | [protocols/auth.md](protocols/auth.md) |
| **Pointer** | Crossing between machines | [protocols/pointer.md](protocols/pointer.md) |

## Code layout

```
core/          Freestanding C11. No allocation, no I/O, no clock access.
               Time is passed in; actions come out as events.
firmware/      RP2040: USB, UART, PIO, buttons, LED. Glue only.
coordinator/   Pi Zero 2 W: display, configuration.
client/        Level 3, per machine.
tests/         Host-run. Real protocol code, simulated wires and clock.
```

The constraint on `core/` is deliberate and load-bearing. Because it never
reads a clock and never touches hardware, an entire chain of boards can be
instantiated in one process, wired through simulated UARTs, and run against a
virtual clock — so the election, the failover timing and the level transitions
are all testable without any hardware at all. The code under test is
bit-for-bit the code that runs on the boards; the simulation replaces only the
wires and the passage of time.

That is why claims like "failover in about 150 ms" are measured in the test
output rather than asserted in a comment.

## What this changes versus upstream DeskHop

[DeskHop](https://github.com/hrvach/deskhop) is two Picos, a UART between them,
and an isolator — and it is a good design. The differences here:

- **Two machines → a chain of any length.** One board per computer, extended by
  adding a board.
- **Absolute pointer coordinates → relative dead-reckoning.** Upstream rewrites
  the mouse descriptor to report absolute positions and tracks the cursor in
  pixel space, which means telling it how big the screens are. This design
  never models the screen at all. See [protocols/pointer.md](protocols/pointer.md).
- **Fixed roles → an election.** With a pre-elected backup, so failover is a
  promotion rather than an election.
- **Unauthenticated link → MAC-tagged frames.** A physical bus between several
  computers where anything plugged in could otherwise claim the routing role.
- **One-shot startup → levels.** Capability follows the hardware present.
