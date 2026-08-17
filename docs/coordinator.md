# The coordinator

Level 2: the status display and configuration. A Pi Zero 2 W joins the chain
through an isolator like any other link, takes the routing role by a planned
handover, and adds what a chain of bare boards cannot provide.

It is genuinely optional, and that is enforced rather than promised — see
"What it deliberately is not", below.

## It runs the same protocol code as the boards

`coordinator/` compiles the same `core/` the firmware does: the same framing,
the same election, the same pointer logic, from the same files. There is no
second implementation.

That was the main reason `core/` is freestanding C with no clock access and no
allocation. A reimplementation would drift, and the first place it would drift
is the handover — the one transition the whole design is built around, and the
hardest one to notice going subtly wrong.

The only thing that differs is the platform layer: `coordinator/src/platform.h`
is the mirror of `firmware/rp2040/src/board.h`. Both hand the same `core/` the
same bytes and the same millisecond counter.

## What it deliberately is not

**It never claims `DHP_CAP_HID_OUT`.** It is not a keyboard to any machine, so
it is on the chain but is not a screen. The router already skips it when the
pointer crosses and when the button cycles, so the user never lands on it.

**It holds no state the chain needs.** Kill it and the boards drop to level 1
in about 150 ms and carry on. That is only true because nothing in the daemon
is load-bearing for level 1 — every configurable value has a default the boards
already use, so the boards never wait for configuration they might not get.

**It stands down cleanly on shutdown.** `SIGTERM` sends a `RESIGN` before
exiting, so the successor does *not* broadcast a release and whatever the user
is typing is undisturbed. Only an abrupt death takes the release path.

## Running it

```sh
cmake -B build -S coordinator
cmake --build build
sudo ./build/deskhop-coord --config /etc/deskhop/coordinator.conf
```

It also builds and runs on an ordinary Linux machine with a USB serial adapter,
which is a much faster way to develop against a chain than reflashing a Pi.

Install as a service:

```sh
sudo cmake --install build
sudo systemctl enable --now deskhop-coord
```

The unit sets `Restart=always` with a one second delay and no restart limit,
deliberately: the chain does not depend on the daemon, so restarting promptly
is worth more than any give-up policy.

## Configuration

`/etc/deskhop/coordinator.conf`, `key = value`, `#` comments.

| Key | Default | |
|---|---|---|
| `serial_dev` | `/dev/serial0` | the symlink to whichever UART is on the header |
| `link_baud` | `2000000` | must match every board |
| `at_head` | `false` | true if plugged into the head rather than the tail |
| `i2c_dev` | `/dev/i2c-1` | |
| `i2c_addr` | `0x3C` | some SSD1306 modules are `0x3D` |
| `headless` | `false` | run without a panel |
| `hello_ms` | `50` | election advertisement interval |
| `hold_ms` | `150` | three missed hellos is a role change |
| `handover_defer_max_ms` | `2000` | rule 1 safety deadline |
| `pointer_span` | `8000` | counts; see [protocols/pointer.md](protocols/pointer.md) |
| `pointer_push` | `600` | shove needed to cross |
| `pointer_decay` | `4` | the speed floor, in counts/ms |
| `pointer_guard` | `1500` | re-entry guard |
| `pointer_vertical` | `false` | enable for a stacked layout |

An unknown or invalid key is reported and ignored rather than refusing to
start. A coordinator that will not start takes the display and the
configuration down with it, which is worse than one stale line.

`at_head` only affects which way round the display draws the chain: positions
are derived from arrival port and hop count, so getting it wrong shows the
chain mirrored and breaks nothing.

## Control socket

A Unix datagram socket at `/run/deskhop-coord.sock`. Unix rather than TCP
because the commands can move focus and start a pairing — reachability should
be a filesystem permission, not a firewall rule.

```sh
deskhop-ctl status
deskhop-ctl pair
deskhop-ctl focus 2
deskhop-ctl set pointer_push 800
deskhop-ctl save
```

Pointer and election settings take effect immediately; link and panel settings
need a restart, and the reply says so.

`focus` is refused unless the coordinator currently holds the routing role,
because focus has exactly one owner. Saying so beats sending a request that
would be silently ignored.

## The display

128×64, four fields:

```
DESKHOP+          L2
--------------------
1-2*-3-C
ROUTING: THIS
LINK 2.0M
CRC 0 AUTH 0
--------------------
UP 3H07M
```

- The chain in **physical order**, machines numbered as you count them along
  the desk. The coordinator shows as `C` and does not consume a number.
- The **focused** board is drawn inverted — the one thing you most often want
  to know, legible without reading anything.
- `*` marks the board the real keyboard is plugged into, which is otherwise
  invisible physical state.
- `CRC` and `AUTH` counts are the fastest way to tell a bad cable (CRC climbing)
  from a key mismatch (AUTH climbing).

While pairing, the middle of the screen is replaced by the six-digit
authentication string to compare against the other board.

If the panel is missing the daemon logs it once and continues headless. Losing
the display costs the `DISPLAY` capability and nothing else; `CONFIG` is
advertised separately, so the chain still reaches level 2.

## Tests

```sh
cmake -B build -S coordinator && cmake --build build
ctest --test-dir build --output-on-failure
```

Two suites:

**`display`** renders every screen into a framebuffer and checks it — no panel,
no I2C. Run `./build/test_display --dump` to see them as ASCII art, which is
how the font and layout were checked in the first place.

**`end_to_end`** is the one place the daemon is actually run. A pseudo-terminal
stands in for the UART, a real chain board built from the real `core/` sits on
one end, and the actual `deskhop-coord` binary is exec'd on the other. It
exercises the termios setup, the serial path, frame assembly across arbitrary
read boundaries, and the whole event loop, against the four claims that matter:

```
ok   board alone takes the routing role (level 1, no coordinator)
ok   level 1 reached without the coordinator
ok   coordinator preempted and took the routing role
     takeover in 20 ms
ok   board sees level 2 once the coordinator is present
ok   board tracks the coordinator as the active speaker
ok   focus survives the handover
ok   board retook the role after the coordinator was killed
     recovery in 140 ms
ok   recovery within a hold time
ok   degraded back to level 1 rather than stopping
```

## Pi setup

The PL011, not the mini-UART, whose baud rate follows the VPU clock and drifts
when the core clock scales:

```
# /boot/firmware/config.txt
dtoverlay=disable-bt
enable_uart=1
dtparam=i2c_arm=on
```

Then `sudo raspi-config` → Interface Options → disable the serial *console*
while leaving the serial *port* enabled. A login prompt on `/dev/serial0` will
happily eat the chain traffic and answer it with `login:`.
