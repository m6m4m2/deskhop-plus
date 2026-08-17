# Level 3 data transport

Clipboard, text, images, file handoff. Segmentation and reassembly over
`DHP_MSG_DATA`.

## The problem

A link frame carries 64 payload bytes. Anything larger than a short clipboard
string needs segmentation, reassembly, ordering, flow control and a way to fail
cleanly — none of which the frame layer provides, because the frame layer
exists to move keystrokes and would be worse at that job if it also had to move
megabytes.

## Go-back-N, and why

The receiver accepts only the next expected chunk and discards anything out of
order; the sender retransmits from the last acknowledged point.

That is less efficient than selective acknowledgement, and it is chosen
deliberately:

- **It needs no receive-side reordering buffer.** `core/` allocates nothing,
  and a reassembly buffer would either bound transfers to whatever fits in a
  fixed array or force an allocator into the protocol layer. Go-back-N lets a
  receiver stream chunks straight to disk as they arrive — which is exactly
  what `on_chunk(offset, data, len)` is shaped for.
- **The link underneath is a short point-to-point UART** with a CRC and a
  replay window. Loss is rare and almost always a burst, which is the case
  where selective acknowledgement wins least.

### The duplicate-ACK trap

Naive go-back-N thrashes. After one lost chunk the whole rest of the window
arrives out of order; if the receiver answers each with a resync ack and the
sender rewinds on each, one dropped chunk causes the window to be resent once
per duplicate and the transfer never converges.

Two guards, both necessary:

- The receiver rate-limits out-of-order acks to one per 20 ms.
- The sender rewinds **once per loss event**, tracked by `rewound_to`, and
  ignores further acks at the same position until it has made progress.

Measured on a link mangling roughly one frame in nine: **49 retransmissions**
for a 12 KB transfer. Without the guards the same test produced **76,850** and
still completed only by luck.

## What this is not for

Large files.

The chain runs at 2 Mbps. After framing overhead a transfer moves roughly
100 KB/s — fine for a clipboard, a screenshot or a document, and about three
hours for a 1 GB video. Pushing bulk file data through a keyboard cable is the
wrong shape.

So `DHP_DATA_SHARE` exists. Instead of the bytes, the sender offers a location
on the coordinator's SMB share and the receiving machine fetches it over the
network at network speed. The chain carries the handoff — which is small, and
needs the trust and the machine-to-machine addressing the chain already has —
and not the payload.

A 4 GB video costs the chain about thirty bytes.

## Wire format

Every `DHP_MSG_DATA` payload begins with an opcode and a stream id.

| Op | | Body |
|---|---|---|
| 0 | `OFFER` | `kind:1 total:4 crc:4 name_len:1 name[]` |
| 1 | `ACCEPT` | `window:1` |
| 2 | `REJECT` | `reason:1` |
| 3 | `CHUNK` | `seq:2 data[]` — 60 bytes of content |
| 4 | `ACK` | `next_seq:2 window:1` |
| 5 | `DONE` | `result:1` — the receiver's verdict |
| 6 | `ABORT` | `reason:1` |

Stream ids are chosen by the sender and are unique per peer per direction, so
inbound and outbound slots are kept in separate halves of the table — a peer
flooding offers cannot starve this machine's ability to send.

### Nothing moves until it is accepted

An `OFFER` is answered by the application, not by the protocol. No content is
transmitted until it is, so one machine cannot push a megabyte at another
unasked. A receiver too busy, too full, or simply unwilling sends `REJECT` and
the sender is told why.

The offer phase waits longer than the chunk phase and is timed separately: a
missing chunk ack means a dropped frame on a working wire, so retrying quickly
is right, but an unanswered offer can mean the far machine's client is still
starting or a human is being asked. Neither resolves in 250 ms.

| | Timeout | Retries |
|---|---|---|
| offer | 1000 ms | 8 |
| chunk ack | 250 ms | 6 |
| idle sweep | 15 s | — |

The idle sweep is deliberately longer than the offer budget, so a slow
acceptance is decided by the offer path rather than cut short.

## End-to-end integrity

Every frame already carries a per-hop CRC-16 and a MAC. Those catch a corrupted
wire. The end-to-end check catches something different: a transfer *reassembled*
wrongly — a chunk duplicated, dropped or transposed — which is the failure this
layer could itself cause and the one nothing below it would notice.

**Adler-32**, because it must be computed *incrementally*. The receiver streams
chunks straight out to the application and never holds the reassembled content,
so any check needing a second pass over a complete buffer is unavailable to it
by construction. Adler-32 folds in one byte at a time and costs no table.

The receiver passes the verdict, because only it has seen every byte — and
reports that verdict back in `DONE`, so a corrupt transfer fails on **both**
sides rather than looking successful to whoever sent it.

## Names are untrusted

The `name` in an offer comes from another machine. It is advisory. A receiver
must never build a path from it unchecked; the reference client replaces
everything outside `[A-Za-z0-9._-]`, rejects `.` and `..`, and opens with
`O_EXCL` so it can neither follow a symlink nor clobber an existing file.

## Implementation

`core/include/dhp/data.h`, `core/src/data.c`.

Tested in `tests/test_data.c` with two endpoints wired through real `dhp_link`
instances, so every transfer is genuinely segmented, framed, MAC'd, relayed and
reassembled rather than handed across in memory: sub-chunk and multi-chunk
transfers, a transfer across a four-board chain, rejection, a lossy link,
a vanishing peer, concurrent streams, stream exhaustion, oversized offers,
cancellation, the share handoff, and an over-long name.
