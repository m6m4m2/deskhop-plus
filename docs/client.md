# The level 3 client

Runs on a machine, talks to the board plugged into that machine, and gives that
one machine the data features: clipboard, text, images, files, share handoff.

Machines without a client carry on exactly as before. That is why level 3 is
per-machine and not a property of the chain — the system can be at level 3 for
one board and level 2 for its neighbour at the same instant.

## The client is not a chain member

The obvious design is to hand the client the chain key and let it speak the
chain protocol directly. That is wrong, and the reason is the most important
thing on this page.

**The chain key authenticates role advertisements.** A process holding it could
claim to be the most capable board, win the election, and become the thing that
decides where every keystroke goes. Any unprivileged program on any one machine
could then take over input for all of them — and levels 1 and 2 would be only
as trustworthy as the least trustworthy desktop on the desk.

So the board stays the chain endpoint and **proxies** for its client. It accepts
local frames over USB and re-emits onto the chain only `DHP_MSG_DATA`.

A compromised client can therefore move clipboard content around — which it
could do anyway, being on the machine — and **cannot** advertise a priority,
claim a role, or inject a keystroke. The end-to-end test asserts exactly this:
it counts every frame the client emits and fails if any of them is not `DATA`.

### The local key is not a secret

`dhp_link` wants a key and this link is a USB cable inside a single machine. A
secret there would protect nothing: anything that can open the device can
already read the clipboard it is carrying, and the OS is the trust boundary.
`DHP_LOCAL_KEY` is therefore a fixed, published value, present so the framing
layer is unchanged. Do not mistake it for one.

## Running it

```sh
cmake -B build -S client && cmake --build build
./build/deskhop-client --verbose
```

| Option | |
|---|---|
| `-d, --device PATH` | board hidraw node (default `/dev/deskhop0`) |
| `-s, --socket PATH` | a Unix socket instead — development, no hardware |
| `-S, --self ADDR` | this board's address (the board supplies it on real hardware) |
| `-p, --peer ADDR` | where to send |
| `-o, --outdir DIR` | where received files land |
| `--accept-files` | write received files to disk (off by default) |
| `--copy-cmd`, `--paste-cmd` | override the clipboard helpers |

### Files are opt-in

Text and images arrive because the user copied something, and land on a
clipboard. A file lands on **disk**, so it requires `--accept-files`. Without
it, incoming file offers are refused and the sender is told.

### udev

`hidraw` nodes are root-only by default. Give the vendor interface a stable
name and let the user open it:

```
# /etc/udev/rules.d/70-deskhop.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", \
  MODE="0660", TAG+="uaccess", SYMLINK+="deskhop%n"
```

`TAG+="uaccess"` grants the locally logged-in user access, which is the right
grain: the clipboard is the user's, not the system's.

## Clipboard

Via helper commands rather than by linking a display server library. Linking
libX11 or libwayland would tie one binary to one display stack, and a desktop
can be X11, Wayland, both at once through XWayland, or neither. Shelling out to
`wl-copy` or `xclip` means the same binary works everywhere the user's own
scripts already work — and it makes the whole thing testable, because the tests
point the helpers at files.

Wayland is preferred when `WAYLAND_DISPLAY` is set, then X11. With neither
installed the client says so once and continues: file and share transfers still
work.

Polling at 2 Hz, because neither X11 nor Wayland offers a portable way to be
notified without owning a window. Imperceptible for copy-and-paste.

**Echo suppression matters more than it sounds.** When a client receives text it
records it as its own last-seen clipboard. Without that, the receiving client
sees a new clipboard, concludes the user copied something, and sends it back —
and with two clients that is an endless loop.

## Control socket

`~/.deskhop-client.sock`, mode 0600.

```sh
deskhop-send status
deskhop-send peer 2
deskhop-send text "hello"
deskhop-send file ~/notes.txt
deskhop-send share smb://deskhop/share/holiday.mp4
```

## Anything large goes over the share

The chain moves roughly 100 KB/s. A 1 GB video would take about three hours.

```sh
cp holiday.mp4 /mnt/deskhop-share/
deskhop-send share smb://deskhop/share/holiday.mp4
```

The chain carries thirty bytes; the file goes over the network. The receiving
client prints the location, and the coordinator is the natural host for the
share since it is the one machine on the desk that is always on and already
trusted.

See [protocols/data.md](protocols/data.md) for why this split exists rather
than a "fast mode".

## Tests

```sh
cmake -B build -S client && cmake --build build
ctest --test-dir build --output-on-failure
```

`client_end_to_end` starts **two real `deskhop-client` processes**, each on a
Unix socket standing in for its board, and plays the pair of boards between
them — relaying `DATA` and nothing else, exactly as a real board would.
Clipboard helpers point at files, so the whole path is observable:

```
ok  both clients connected to their boards
ok  clipboard text reached the other machine
ok  20 KB clipboard survived segmentation and reassembly
ok  file arrived on the other machine and was written to disk
ok  DATA frames were relayed
ok  the client emitted nothing but DATA (it cannot claim a role)
```

## The board side

`firmware/rp2040/src/clientlink.c` is the proxy. It exposes a third HID
interface -- vendor usage, 64 bytes each way, 1 ms polling -- and gates what
crosses between the client and the chain.

HID rather than CDC or a bulk vendor interface because it needs no driver
anywhere: Linux exposes it as hidraw, macOS and Windows both bind HID natively.
A device that sits between a keyboard and every machine you own should not also
ask each of them to install something.

Two messages never leave the board: the client's `ATTACH` (announcing its
capabilities, so the board can advertise level 3 on its behalf) and the board's
`STATUS` (which tells the client its chain address, the current focus and the
level). The address matters: without it a client would have to be told what to
call itself, and two clients that guessed the same value would each mistake the
other's frames for their own echo.

The proxy is deliberately free of hardware headers, so its rule is tested on a
development machine rather than only reasoned about — `tests/test_clientlink.c`
compiles the firmware source unchanged and stubs the two USB entry points:

```
ok  DATA is proxied onto the chain
ok  only DATA reaches the chain (HELLO, KBD, FOCUS, RESIGN all refused)
ok  the source address cannot be spoofed
ok  capability follows the client
ok  a client cannot claim COORD, HID_IN or DISPLAY
ok  a silent client is forgotten
ok  chain DATA reaches the client with its source intact
```

### Bandwidth

A full-speed interrupt endpoint moves 64 bytes per millisecond, so this link
tops out near 64 KB/s of wire and roughly 45 KB/s of content — **slower than
the chain it feeds**. The client link, not the chain, is the level 3
bottleneck, and it is another reason anything large should travel by
`DHP_DATA_SHARE`.

Outbound bytes are queued and each poll drains a full report rather than one
report per frame; a short frame would otherwise waste most of a millisecond.

## Not done
- **Folder transfer.** `DHP_DATA_LIST` has a kind number and no implementation.
- **The SMB share itself.** The handoff message works and is tested; nothing
  sets up a Samba share on the coordinator.
- **Images** are wired through the same path as text and share its helpers, but
  have not been exercised against a real desktop clipboard.
- **Follow-the-focus clipboard.** `--peer` is explicit; the board reports the
  current focus in `STATUS` and nothing uses it yet.
