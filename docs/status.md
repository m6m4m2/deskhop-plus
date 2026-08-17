# Status

What is actually done, what is written but unverified, and what does not exist
yet. Read this before assuming any part of the tree works.

## Verified

Everything in `core/`, exercised by 962 checks that run on a host machine
against a simulated clock and simulated UARTs. Clean under ASan and UBSan, and
compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, plus a
`make freestanding` check that keeps `core/` compilable for a target with no
libc beyond `string.h`.

| Module | Covered by |
|---|---|
| SipHash-2-4 | Published reference vectors, plus a cross-check against an independently written implementation |
| Framing | Round trip, byte stuffing, CRC rejection, wrong-key rejection, mid-stream resync, 5000 bytes of noise then a valid frame, MAC survival across two forwarding hops, hop-limit exhaustion |
| Link | Relaying along a chain, broadcast delivery, replay suppression, neighbour detection and timeout, unpaired boards being inert |
| UHRP | Election, capability-derived priority, uid tiebreak, pre-elected standby, cascading failover, convergence stability over 5 s, late joiners, and both special rules |
| Pointer | 40 traverses with no spurious crossing, crossing in both directions, decay defeating an instalment shove, reversal cancelling a gesture, re-entry guard |
| HID tracking | Held-key and held-button release, slot independence |
| Levels | Derivation from capability, cumulative ordering, symmetric degradation |
| Router | Chain position derivation, input authority, focus tracking, end-of-chain behaviour |
| Chain simulation | Level 1 timing, typing to the focused machine, pointer switching, release on switch, coordinator handover, coordinator loss, release on abrupt loss, adding a board, button cycling, chain cut and merge |
| Pairing | Successful pairing, derived key authenticating real frames, third-party abort, late third-party abort, tampered confirmation, timeout, inertness when not pairing |

Measured timings, printed by the suite rather than asserted in prose:

```
level 1 usable after 150 ms
failover in 150 ms
dropped back to level 1 in 151 ms
```

## Verified: the coordinator daemon

`coordinator/` builds and **runs**, and unlike the firmware it is tested by
being executed rather than only compiled. Two suites, both in CI:

**`display`** renders every status screen into a framebuffer and checks it —
title, chain row, focus inversion, the pairing screen, oversized counters, a
full 16-board chain, an empty chain, lowercase folding, and clipping at the
right edge. No panel or I2C bus involved.

**`end_to_end`** is the one place a daemon is actually run. A pseudo-terminal
stands in for the UART, a real chain board built from the real `core/` sits on
one end, and the actual `deskhop-coord` binary is exec'd on the other — so the
termios setup, the serial read/write path, frame assembly across arbitrary read
boundaries and the whole event loop are all exercised:

```
ok   board alone takes the routing role (level 1, no coordinator)
ok   coordinator preempted and took the routing role   (20 ms)
ok   board sees level 2 once the coordinator is present
ok   focus survives the handover
ok   board retook the role after the coordinator was killed  (140 ms)
ok   degraded back to level 1 rather than stopping
```

What that does not cover: a real PL011 at 2 Mbps (a pty ignores baud rate
entirely), a real SSD1306 over I2C, and the Pi's own boot timing.

## Compiles, but never run on hardware

**`firmware/rp2040/`** builds clean for the Cortex-M0+ with the Pico SDK 2.1.1
and produces a real `deskhop_plus.uf2`:

```
   text    data     bss     dec
 158708       0   20216  178924
```

158 KB of flash and 20 KB of RAM, against the Pico's 2 MB and 264 KB — plenty
of headroom. No warnings from any file in `core/` or `firmware/`.

Getting it to compile turned up five genuine defects that would each have cost
bench time:

1. `PICO_DEFAULT_UART=-1` pastes into a `uart-1` token and does not compile.
   Freeing both UARTs is done with `pico_enable_stdio_uart(... 0)`.
2. `SYS_CLK_KHZ=120000` at compile time demands the PLL VCO and post-dividers
   by hand. Setting the clock at runtime in `board_init()` is both simpler and
   more correct, because `uart_init()` then computes its baud divisor from the
   final clock rather than the 125 MHz default.
3. The SDK's `tinyusb_host` target does not include the PIO-USB host
   controller driver, so every `hcd_*` entry point was undefined at link time.
   `hcd_pio_usb.c` has to be listed explicitly.
4. `set_sys_clock_khz` was being called against an implicit declaration.
5. **The flash/dual-core hazard.** Writing flash stalls the XIP cache, so core
   1 — sitting in a tight `tuh_task()` loop executing straight out of flash —
   faults or hangs. Disabling interrupts on core 0 does nothing for it. Core 1
   now registers as a `multicore_lockout` victim and is parked for the
   duration. This would have fired exactly once: on the first successful
   pairing, which is the first thing anyone tests.

What compiling does **not** establish, and what a breadboard still has to:

- The TinyUSB dual-role arrangement (device on the native controller, host on
  PIO-USB) links, but has never enumerated anything. This is the arrangement
  upstream DeskHop uses and that the Pico-PIO-USB examples document, and there
  is a [known TinyUSB issue](https://github.com/hathach/tinyusb/discussions/3477)
  with descriptor fetches inside `tuh_mount_cb` in dual-role builds; this code
  avoids fetching descriptors there, but that is reasoning, not evidence.
- The UART interrupt handlers and ring buffers are single-producer /
  single-consumer by construction and should be race-free, but have never
  moved a byte.
- Debounce and pairing-hold timing are guesses at reasonable values.
- 2 Mbps across a real isolator has not been demonstrated.
- The WS2812 driver assembles to the expected four PIO instructions and its
  clock divider lands on exactly 15.0 at 120 MHz, so the bit timing is exact
  by construction — but no LED has been lit.

## Not started

- **Level 3 clients** (`client/`). `DHP_MSG_DATA` has a type number and nothing
  behind it. Clipboard, files, images and the SMB share are unimplemented.
- **Chunking for `DATA`.** The 64-byte payload limit means anything larger than
  a short clipboard string needs segmentation and reassembly, which is not
  designed yet.
- **Configuration persistence** beyond the chain key.
- **Hotkey switching.** `DHP_FOCUS_R_HOTKEY` is defined; nothing detects a
  hotkey, which needs a keystroke-interception policy that has not been decided.

## Known limitations of the design

These are properties, not bugs, but they are real and worth stating.

**Pointer drift.** The integrator is deliberately decoupled from the real
pointer position — the OS applies its own acceleration, the real screen edge
clamps the pointer while the integrator keeps counting, and applications warp
the cursor at will. This is the price of needing no screen configuration, and
the gesture-based crossing test is what makes it harmless. But it does mean
crossing costs a deliberate shove rather than a flick.

**Split brain is by design.** Cut the chain and each half elects its own active
speaker, because each half is genuinely a separate system and each stays
usable. If a cut is intermittent the role will move around; the hold time is
what bounds how fast.

**The chain key is stored in plaintext.** The RP2040 has no secure element and
no way to keep a secret from someone holding the board. Pairing protects
against a device plugged into the chain, not against someone with physical
possession of a board.

**Store-and-forward latency scales with chain length.** About 92 µs per hop at
2 Mbps for a mouse frame, so under 400 µs across four boards — inside the 1 ms
budget of a 1 kHz mouse, but a very long chain would eventually be felt. Cut-
through forwarding would fix it and is not implemented.

**`DHP_MAX_BOARDS` is 16.** Every table in `core/` is a fixed array so that
nothing allocates; a longer chain needs the constant raised and the RAM to
match.
