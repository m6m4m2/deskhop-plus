# Hardware

Everything below is buildable from parts already in your inventory. Nothing
here needs a custom PCB — the first chain can be built on breadboards, and the
firmware does not care whether it is running on a breadboard or a finished
board.

## The mapping

| Role | Part | Why this one |
|---|---|---|
| Per-machine board | Raspberry Pi Pico (RP2040) | Native USB device to the PC *and* a PIO-USB host port for the keyboard, on one chip. Two hardware UARTs, which is exactly the up-port/down-port the chain needs. |
| Chain isolation | ISO7721DR or ADuM1201BRZ | One IC per link. Both are 1-forward/1-reverse dual channel, which is precisely a bidirectional UART. |
| Coordinator | Raspberry Pi Zero 2 W | Boots in 15–20 s, which is the number the whole staged-startup design is built around. Drives the display and holds configuration. |
| Status display | SSD1306 128×64 I²C | Level 2 only. |
| Status LED | WS2812B | Per-board focus/role indication. |
| Buttons | 6×6 tactile | One to switch machines, one to pair. Three is the minimum for a two-board chain -- see below. |

You have 4 Picos, so a 4-machine chain is buildable today, with 10 of each
isolator — far more than the 3 links a 4-board chain needs.

## Why each board needs both USB roles

The RP2040's native USB controller is used in **device** mode, presenting a
keyboard and mouse to the computer it is plugged into. That is what makes the
system work at BIOS and at a login screen: to the machine, this is simply a
USB HID keyboard, and there is nothing to install.

The **host** side — the port the real keyboard and mouse plug into — is
bit-banged over PIO using [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB),
which is how upstream DeskHop does it too. TinyUSB runs the device stack on
core 0 and the PIO host stack on core 1.

Only the board with the keyboard physically attached needs the USB-A socket
populated. Every board runs the same firmware image regardless, and discovers
at runtime whether anything is plugged into its host port — that discovery is
what sets the `HID_IN` capability, which is what raises its election priority.

## Pin assignment

One image, one pinout, every board identical.

| Function | GPIO | Notes |
|---|---|---|
| PIO-USB host D+ | GP0 | D− must be GP1: the PIO program requires consecutive pins |
| PIO-USB host D− | GP1 | |
| UART1 TX → down-port | GP4 | toward the tail of the chain |
| UART1 RX ← down-port | GP5 | |
| UART0 TX → up-port | GP12 | toward the head of the chain |
| UART0 RX ← up-port | GP13 | |
| Pair button | GP14 | to GND, internal pull-up |
| Switch button | GP15 | to GND, internal pull-up |
| WS2812B data | GP16 | |
| I²C1 SDA (optional OLED) | GP26 | |
| I²C1 SCL (optional OLED) | GP27 | |

GP0/GP1 are deliberately given to PIO-USB, which is why UART0 is on GP12/GP13
rather than its default GP0/GP1.

## Wiring one link

Each link between adjacent boards gets one isolator. The two halves are
powered from *different* boards, which is the entire point — the two computers'
grounds must never meet.

```
        Board A (upstream)                     Board B (downstream)
                                ISO7721DR
      3V3 ──────┬──── VDD1 [1]  ┌───────┐  [8] VDD2 ────┬────── 3V3
                │               │       │               │
      GND ──────┴──── GND1 [2]  │       │  [7] GND2 ────┴────── GND
                               │       │
   GP4 (U1 TX) ─────── INA [3]  │       │  [6] OUTA ─────────── GP13 (U0 RX)
                               │       │
   GP5 (U1 RX) ─────── OUTB [4] │       │  [5] INB  ─────────── GP12 (U0 TX)
                                └───────┘

   100 nF from VDD1 to GND1, and 100 nF from VDD2 to GND2, both as close to
   the package as you can get them. These are not optional.
```

Note the crossover: A's **down**-port transmit lands on B's **up**-port
receive. Get this wrong and the chain will look dead in a way the framing
layer cannot diagnose for you, because it will never see a valid flag byte.

ADuM1201BRZ has the same 1-forward/1-reverse arrangement; check the pinout
against its datasheet before substituting, as the pin numbering differs.

Both parts are SOP-8. You have 20 SOP8→DIP8 adapter boards, which is what
makes this breadboard-able.

## Buttons, and why three is the minimum

Pairing requires a deliberate physical action on **both** boards within one
window. That is the only channel an attacker on the wire does not have, so it
cannot be reduced to a single button somewhere. A two-board chain therefore
needs two pair buttons at minimum.

The switch button only does anything on the board that currently holds the
routing role, so one is enough to drive a bench session:

| Board | Buttons |
|---|---|
| the one with the keyboard | pair (GP14) **and** switch (GP15) |
| the other | pair (GP14) |

### The pair button does two things

| Hold | |
|---|---|
| ~3 s | begin pairing |
| ~10 s | **forget the chain key** and return to level 0 |

The second matters more than it looks. A mispaired board is otherwise
unrecoverable without reflashing, and the symptom gives nothing away: two
boards holding different keys simply discard each other's frames, so the chain
looks dead rather than misconfigured. Watch the `AUTH` counter on the
coordinator's display -- climbing means a key mismatch, and a ten-second hold
on both boards followed by a fresh pairing is the fix.

## Status LED

One WS2812B on GP16, driven from a PIO state machine so it costs the CPU
nothing — bit-banging it would mean holding timing to a few hundred nanoseconds
with interrupts off for 30 µs per pixel, which would disturb the USB device
stack.

| Colour | Meaning |
|---|---|
| white | this machine has the user |
| blue | this board holds the routing role, user is elsewhere |
| cyan | both: routing *and* has the user |
| dim blue | pre-elected standby |
| amber, breathing | pairing window open |
| amber, dim steady | unpaired — no chain key |
| red, fast blink | pairing failed (4 s, then back to real state) |

Focus and role get separate colours deliberately: they are independent, and
watching which board routes while the user's focus moves is most of what makes
a bench session legible.

### The 3.3 V data problem

WS2812B is a 5 V part, and its data threshold is about 0.7 × VDD — so at a 5 V
supply it wants 3.5 V to register a one, and the RP2040 puts out 3.3 V. It
often works anyway, and it is exactly the kind of marginal that behaves on the
bench and fails in the finished build.

The cheap fix uses parts you already have: feed the LED through a **1N4148 in
series with its 5 V supply**. The ~0.7 V drop puts it at roughly 4.3 V, which
moves the threshold down to about 3.0 V and gives the 3.3 V data line real
margin.

```
   VBUS (5V) ──|>|── LED VDD        1N4148, band toward the LED
                        │
                     100 nF to GND, at the LED
   GP16 ───────────── LED DIN
```

If you would rather not, powering the LED from 3V3 also works — it is dimmer
and the colours shift, but the threshold problem disappears entirely.

### If the LED never lights

There is one case where this is expected rather than broken. In host mode
PIO-USB claims state machines in **both** PIO blocks, and the LED deliberately
takes only what is left over — if there is no room, it does without rather than
competing for a resource USB needs. Being a keyboard matters more than
indication. `board_led_hw_init()` sets a flag and returns quietly in that case.

### Ground rule

Do **not** tie the grounds of two boards together anywhere. If you power two
Picos from the same USB hub for bench testing you have already defeated the
isolation, which is fine for a desk test and not fine once real machines are
attached — a ground loop between two computers' USB ports is exactly the fault
the isolator exists to prevent.

## Link speed

Default **2 Mbps**, chain-wide.

This is set by the coordinator rather than by the boards. An RP2040 UART will
comfortably run 4 Mbps and the isolators are rated far beyond that (ISO7721 to
100 Mbps, ADuM1201BRZ to 25 Mbps), but the Pi Zero's PL011 is the awkward one,
and every board on a chain must agree.

The latency this costs is small enough not to matter. A mouse frame is 23 bytes
before stuffing, so at 2 Mbps one store-and-forward hop is about 92 µs, and the
worst case across a four-board chain is under 400 µs — comfortably inside the
1 ms budget of a 1 kHz mouse.

If you build a chain with no Pi coordinator, 4 Mbps works and halves that.

On the Pi, use the PL011 (`ttyAMA0`), not the mini-UART, whose baud rate is
tied to the VPU clock and will drift when the core clock scales:

```
# /boot/firmware/config.txt
dtoverlay=disable-bt        # frees PL011 for the GPIO header
enable_uart=1
```

## Coordinator wiring

The Pi Zero 2 W attaches at either end of the chain, through an isolator
exactly like any other link:

| Pi | Function |
|---|---|
| GPIO14 (pin 8) | UART TX → isolator → board's up- or down-port RX |
| GPIO15 (pin 10) | UART RX ← isolator ← board's TX |
| GPIO2 (pin 3) | I²C SDA → SSD1306 |
| GPIO3 (pin 5) | I²C SCL → SSD1306 |

Power the Pi from its own supply, not from a board.

The coordinator is genuinely optional. Without it the chain sits at level 1 —
keyboard, mouse, switching and pointer crossing all working — and gains the
display and configuration whenever you plug it in. Unplugging it drops back to
level 1 in about 150 ms rather than stopping. That is verified end to end in
`tests/test_sim.c`.

## Building the firmware

```sh
git submodule update --init --recursive     # Pico-PIO-USB, Monocypher
export PICO_SDK_PATH=/path/to/pico-sdk      # tested against SDK 2.1.1
cmake -B build -S firmware/rp2040
cmake --build build
```

Produces `build/deskhop_plus.uf2`. One image for every board in the chain —
boards differ only by their flash unique id, which they read at runtime.

Flash it by holding BOOTSEL while plugging the Pico in, then copying the `.uf2`
to the mass-storage device that appears.

The SDK needs its own TinyUSB submodule:

```sh
cd $PICO_SDK_PATH && git submodule update --init lib/tinyusb
```

## Bring-up order

Build and test incrementally; a chain that is wrong in two places at once is
very hard to read.

1. **One board, no chain.** Flash it, plug it into a PC. It should enumerate
   as a keyboard and mouse and pass through a keyboard plugged into its host
   port. This is level 0 and needs no link at all.
2. **Two boards, one link.** Confirm the election settles with exactly one
   active speaker (the LED shows the role), then confirm switching works.
3. **Add the third and fourth.** Nothing to configure — chain position is
   derived from which port traffic arrives on and how far the hop limit has
   been decremented.
4. **Add the coordinator.** Watch it preempt and take the role, and confirm
   the display comes up without anything being interrupted.

## Parts you do not need yet

The ESP32-DevKitC boards, the Pi 3, the Arduinos and the Teensy have no role in
the chain as designed. The ESP32s are the obvious basis for a later wireless
coordinator or a level-3 bridge for a machine you would rather not run a client
on, but neither is needed to get to level 3 on a wired chain.

The 300× 27 Ω resistors are the wrong value for USB series termination (22 Ω or
33 Ω is usual) but are close enough to 33 Ω to work on the PIO-USB lines if you
want them; the Pico-PIO-USB reference designs commonly run without series
resistors at these lengths anyway.

## Identity

The USB descriptors are deliberately fixed and distinctive rather than cloning
some other vendor's ids. This is a device that sits between a keyboard and
every machine you own; it should be trivially identifiable in `lsusb`, and
trivially allow-listable by any endpoint-security policy that cares. Covert
would be the wrong design.

See `firmware/rp2040/src/usb_descriptors.c` for the current values and the note
on registering a PID with [pid.codes](https://pid.codes).
