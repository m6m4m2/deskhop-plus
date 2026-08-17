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

## Written but not verified

**`firmware/rp2040/`** — none of it has been compiled or run. There is no Pico
SDK in the environment it was written in, so it has not seen a compiler at all,
let alone hardware. Treat it as a considered starting point, not as working
firmware. In particular:

- The TinyUSB dual-role setup (device on the native controller, host on
  PIO-USB) is the arrangement upstream DeskHop uses and that the Pico-PIO-USB
  examples document, but the specific initialisation order here is unverified.
  There is a [known TinyUSB issue](https://github.com/hathach/tinyusb/discussions/3477)
  with descriptor fetches inside `tuh_mount_cb` in dual-role configurations;
  this code avoids fetching descriptors there, but that is reasoning, not
  evidence.
- The UART interrupt handlers, the flash key storage and the debounce logic
  are all plausible and all untested.
- `board_led()` is a stub. The WS2812 PIO program is not written.
- `third_party/Pico-PIO-USB` and `third_party/monocypher` are referenced by
  the build but not vendored into the tree.

**Descriptors** — `DHP_PID` is a placeholder. It must be registered with
[pid.codes](https://pid.codes) before anything is distributed.

## Not started

- **Coordinator** (`coordinator/`). The chain reaches level 2 when something on
  it advertises `DISPLAY` and `CONFIG`; the daemon that does so on a Pi Zero
  2 W, drives the SSD1306, and serves configuration is not written. The
  protocol side is done and tested — the simulation stands in a coordinator and
  exercises the handover in both directions — but no Pi-side code exists.
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
