# Link framing

HDLC-style flag delimiting with byte stuffing.

## Why not a length prefix

A length-prefixed format requires the receiver to already be in sync to know
where a frame ends. A receiver that starts listening to a running link has no
way to find the boundary except by guessing and hoping.

Flag delimiting resynchronises with **no state at all**: scan for the next
`0x7E` and you are aligned. That is the property that lets a board be plugged
into a chain that is already running, mid-frame, and find its place
immediately — which is the whole modularity story. It is worth the ~1.5%
average stuffing overhead.

## Wire format

```
0x7E   <stuffed body>   0x7E
```

A flag both closes the frame in progress and opens the next one, so
back-to-back frames share a flag and an idle line can be filled with flags for
free.

The body, before stuffing:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ver:4 \| type:4` |
| 1 | 1 | `ttl` — hop limit, decremented on forward |
| 2 | 2 | `src` (LE) |
| 4 | 2 | `dst` (LE), `0xFFFF` = broadcast |
| 6 | 2 | `seq` (LE) |
| 8 | 1 | `len` — payload length, 0…64 |
| 9 | `len` | payload |
| … | 4 | `mac` (LE) — SipHash-2-4, truncated to 32 bits |
| … | 2 | `crc16` (LE) — CRC-16/CCITT-FALSE |

Overhead is 15 bytes plus stuffing. Version is 1.

### Stuffing

`0x7E` and `0x7D` are escaped as `0x7D` followed by the byte XOR `0x20`.
Nothing inside a frame can therefore be mistaken for a delimiter.

## Two integrity fields, deliberately

**`crc16`** covers the whole body *including* `ttl`. It is the cheap
line-noise check, evaluated first so that garbage costs almost nothing to
discard — noise on the wire never gets as far as a SipHash.

**`mac`** covers the body *excluding* `ttl`. It is the authenticity check.

### Why the MAC excludes the TTL

Every hop decrements the hop limit. If the MAC covered it, a forwarded frame's
tag would fail at the second board and the chain could not relay anything at
all.

So the originator computes the MAC once, and every board along the path
verifies that same tag unchanged. Only the CRC is recomputed per hop, which is
cheap. This is the same reasoning that makes IPsec AH exclude the IP TTL.

`dhp_frame_forward_prepare()` is the whole of the forwarding path: decrement,
repair the CRC, leave the MAC alone.

## Message types

The type field is 4 bits, so there are sixteen and no more. That is a
deliberate constraint — it keeps the protocol small enough to reason about, and
anything richer is layered inside `CFG` or `DATA` rather than spending a type.

| # | Type | Purpose |
|---|---|---|
| 0 | `HELLO` | UHRP advertisement |
| 1 | `RESIGN` | Graceful handover of the active role |
| 2 | `KBD` | Keyboard report toward a machine |
| 3 | `MOUSE` | Mouse report toward a machine |
| 4 | `FOCUS` | "Machine N now has the input devices" |
| 5 | `RELEASE` | Release everything held on a board |
| 6 | `TOPO` | Chain position |
| 7 | `CAP` | Capability / level advertisement |
| 8 | `PAIR` | Trust establishment |
| 9 | `CFG` | Configuration (level 2) |
| 10 | `DATA` | Level 3 bulk: clipboard, files, images |
| 11 | `PING` | |
| 12 | `PONG` | |
| 13 | `INPUT` | Raw input forwarded to the active speaker |
| 14–15 | — | Reserved |

## Replay window

Each board keeps a high-water sequence number per source. A frame whose
sequence has already been seen is neither delivered nor relayed, which
suppresses the duplicate a broadcast produces when it reaches a board from both
directions at once.

Two details:

- The first frame from a source is always accepted. Without this, a board's
  very first frame — sequence zero, against a high-water mark initialised to
  zero — would be mistaken for a replay and silently dropped.
- A source that appears to have jumped far backwards has rebooted, so its
  counter is resynchronised rather than ignored forever. The MAC has already
  established that it holds the chain key.

## Sizes

| | |
|---|---|
| Max payload | 64 bytes |
| Max body | 79 bytes |
| Max on the wire | 160 bytes (worst case: every byte stuffed) |
| Typical mouse frame | 23 bytes body → ~92 µs per hop at 2 Mbps |

## Implementation

`core/include/dhp/frame.h`, `core/src/frame.c` — framing.
`core/include/dhp/link.h`, `core/src/link.c` — ports, relaying, replay window.

Tested in `tests/test_frame.c` and `tests/test_link.c`, including
resynchronisation from mid-frame, 5000 bytes of random noise followed by a
valid frame, and MAC survival across two forwarding hops.
