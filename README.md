# DeskHop+

One keyboard and mouse serving several computers, where the machines behave
like one desk rather than one machine at a time.

The device sits between them and is a keyboard and mouse *to each* — so it
works before any operating system is running, with nothing installed anywhere.

It is modular: not one box with a fixed port count, but a chain of small
boards, one per computer, that you extend by adding a board. Nothing to
configure when you do. The system notices and reconfigures itself.

---

## It works in levels, and comes up in stages

This is the part that matters most in practice. The coordinator takes fifteen
to twenty seconds to boot. **The device does not wait for it.**

| | | |
|---|---|---|
| **Level 1** | milliseconds after power-on | The boards elect one of themselves as coordinator and the basics work: keyboard and mouse shared across every machine, switching between them, pointer crossing. You can start typing while the rest of the system is still coming up. |
| **Level 2** | once the coordinator has booted | It takes over the role — a planned handover, not a restart — and brings the status display and configuration. Nothing is interrupted; not a keystroke is dropped in the transition. |
| **Level 3** | with a client on a machine | Copy and paste, text, files, folders, images, SMB share. Only on the machines that want it; the others carry on as before. |

Levels degrade the same way they arrive. Unplug the coordinator and the system
drops back to level 1 in about 150 ms rather than stopping. Capability follows
the hardware actually present, so the system only ever advertises what it can
currently deliver.

These are measured, not asserted. From the test suite:

```
== chain sim ==
- test_level1_comes_up_immediately
    level 1 usable after 150 ms
- test_coordinator_takes_over_cleanly
- test_coordinator_loss_degrades_to_level1
    dropped back to level 1 in 151 ms
- test_abrupt_loss_releases_everything
```

## Features

- Keyboard and mouse across every machine, working at BIOS and at a login
  screen
- Switch computers with a button press
- Pointer crosses by pushing into the screen edge — no screen sizes to
  configure, because the device measures mouse movement rather than pixels, so
  it behaves the same on any monitor arrangement
- Automatic failover, with held keys released on the way out so nothing is left
  stuck on a machine nobody is watching
- Coordinator adds display and configuration; clients add clipboard, text,
  files and images

## Protocols

| | |
|---|---|
| **[UHRP](docs/protocols/uhrp.md)** | The role election, modelled on Cisco HSRP. Boards advertise a priority; the most capable wins, ties break on a unique per-board id, and a backup is pre-elected so failover is a promotion rather than an election. Two rules HSRP doesn't need: never hand over while a key is held down, and release everything if the coordinator vanished without warning. |
| **[Link framing](docs/protocols/link-framing.md)** | HDLC-style, with a checksum and a hop limit. Chosen because it resynchronises with no state, so a board plugged into a running chain finds its place immediately. |
| **[Authentication](docs/protocols/auth.md)** | Every frame is tagged, so a board attached by someone else cannot claim the coordinator role. Boards trust each other only after a deliberate physical action on both, and if a third device joins that exchange, everyone aborts rather than trusting anyone. |
| **[Pointer](docs/protocols/pointer.md)** | Relative dead-reckoning, where crossing is "keep pushing at the edge". That's why it needs no layout configuration. |
| **[Data](docs/protocols/data.md)** | Level 3 segmentation and reassembly, go-back-N so a receiver can stream to disk without a reordering buffer. Anything large moves over the share instead, because a keyboard cable is the wrong place for a gigabyte. |

## Build and test

The protocol core is freestanding C11 and runs on a development machine
against a simulated clock, so no hardware is needed to exercise it:

```sh
make test        # 962 checks
make sanitize    # the same, under ASan + UBSan
make freestanding
```

An entire chain of boards is instantiated in one process, wired through
simulated UARTs, and run against a virtual clock. The code under test is
bit-for-bit the code that runs on the boards — the simulation replaces only
the wires and the passage of time. That is what makes the election, the
failover timing and the level transitions testable at all.

## Repository layout

```
core/          Freestanding C11. No allocation, no I/O, no clock access.
firmware/      RP2040: USB, UART, PIO, buttons, LED. Glue only.
coordinator/   Pi Zero 2 W: display, configuration. Runs the same core/.
client/        Level 3, per machine. Also runs the same core/.
tests/         Host-run. Real protocol code, simulated wires and clock.
docs/          Architecture and the four protocol specifications.
```

Start with **[docs/architecture.md](docs/architecture.md)**.

## Hardware

A chain of Raspberry Pi Picos, one per computer, each galvanically isolated
from its neighbours, with a Pi Zero 2 W as the optional coordinator.

**[docs/hardware.md](docs/hardware.md)** has the pin assignment, the isolator
wiring, and a bring-up order that lets you test one thing at a time.
**[docs/coordinator.md](docs/coordinator.md)** covers the level 2 daemon and
**[docs/client.md](docs/client.md)** the level 3 client.

The final product is a modular chain you extend by adding boards, all running
one firmware image with one identity — deliberately identifiable and
allow-listable rather than covert. Buy the boards for the machines you have;
add the coordinator when you want the display and the data features; add
clients only where you want them.

## Relationship to DeskHop

This builds on [hrvach/deskhop](https://github.com/hrvach/deskhop), which is
two Picos, a UART, and an isolator — a good design that this borrows the core
hardware insight from. The differences are set out at the end of
[docs/architecture.md](docs/architecture.md); the largest are the chain instead
of a fixed pair, the relative pointer instead of absolute coordinates, the
election instead of fixed roles, and the authenticated link.

## Status

The protocol core, the election, the pointer, the link layer and pairing are
implemented and tested. The coordinator daemon is implemented and tested by
being run — including end to end against the real binary over a pseudo-terminal.
The level 3 client is implemented and tested end to end, two real client
processes at a time. The RP2040 firmware compiles and produces a `.uf2` but has
never been run on hardware, and does not yet expose the vendor HID interface
the client needs to attach to.

[docs/status.md](docs/status.md) is specific about which is which, and about
the design's real limitations.

## Licence

GPL-2.0-only, matching upstream DeskHop.
