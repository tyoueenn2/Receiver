# Future capture sender contract

The production sender is intentionally not implemented in this repository. A future sender must satisfy every rule below; `tools/test_sender.py` is a synthetic/raw-file reference, not a desktop-capture implementation or performance benchmark.

## Endpoint ownership and timing

Create one UDP socket, bind it once, and use that same source IP and port for UVH1 heartbeats, every UVF1 fragment, receiving UVC1 clock requests, and sending UVS1 replies. Receiver identifies the sender by the full UDP endpoint. Frames alone do not establish ownership.

Generate a new unpredictable nonzero 64-bit sender session whenever the sender process or capture session restarts. Send a 16-byte UVH1 heartbeat from the bound socket about every 50 ms:

`UVH1 | reserved:u32be=0 | sender_session:u64be`

Receiver declares the sender lost after 100 ms without a valid heartbeat. Heartbeats and clock replies must run independently of capture and frame transmission so a slow capture or large frame cannot starve them. A new session resets reassembly and clock state; recently retired sessions are rejected to prevent delayed packets from an old sender becoming current.

Use one monotonic nanosecond clock for UVF1 capture timestamps and UVS1 `t1`/`t2`. On a valid 24-byte `UVC1 | reserved:u32be=0 | session:u64be | t0:u64be`, record `t1` immediately after receipt and `t2` immediately before sending this 40-byte reply from the same socket:

`UVS1 | reserved:u32be=0 | session:u64be | echoed_t0:u64be | t1:u64be | t2:u64be`

Do not substitute wall-clock/Unix time. Echo the session and `t0` exactly, require `t2 >= t1`, and service clock requests promptly. Receiver rejects exchanges longer than 50 ms and expires synchronization after two seconds. Output correction requires both fresh heartbeats and valid clock synchronization.

## Capture and pixels

Capture/crop before packetization. Stamp `capture_ns` at the moment the captured pixels represent—not after conversion or network transmission. Dimensions are independently 1–1024, and complete decoded data must not exceed 4 MiB.

Supported tightly packed row layouts are:

- Format 1, RGB24: exactly `width * height * 3` bytes, row order top-to-bottom, pixels `R,G,B`, no row padding.
- Format 2, BGRA32: exactly `width * height * 4` bytes, row order top-to-bottom, pixels `B,G,R,A`, no row padding. Alpha is carried but not used by inference.

Do not send a full-resolution desktop frame if it violates these bounds. Start deployment testing with a centered 320×320 RGB24 crop; crop size and position must match the Receiver profile's reference/FOV assumptions.

## UVF1 fragmentation

Every fragment is one UDP datagram with the 48-byte UVF1 header documented in [PROTOCOL.md](PROTOCOL.md), followed immediately by raw bytes. Choose one fragment stride from 1024 through 1352 bytes and keep it, dimensions, format, session, sequence, capture timestamp, and total raw length identical for all fragments of a frame.

For raw length `N` and stride `S`:

- `fragment_count = ceil(N / S)` and must not exceed 4096.
- `fragment_index` is zero-based and less than `fragment_count`.
- `payload_offset = fragment_index * S`.
- Payload length is `min(S, N - payload_offset)`; the UDP datagram is exactly `48 + payload_length` bytes and never exceeds 1400 bytes.
- Header version is 1, header length is 48, and the reserved field at 46 is zero.

Fragments may arrive out of order. Identical duplicates are tolerated, but a duplicate index containing different bytes invalidates that assembly. There is no retransmission or FEC; one lost fragment discards the frame when its 20 ms assembly lifetime expires. Receiver keeps only three incomplete assemblies and publishes only the newest complete frame, so the sender must favor fresh captures rather than retransmitting obsolete frames.

## Sequence rules

Frame sequence is an unsigned 32-bit value scoped to the sender session. Zero is valid. Increment once per captured frame and wrap from `0xffffffff` to zero. Receiver compares sequences with the unsigned half-range rule: a candidate is newer only when `(candidate - previous) mod 2^32` is in `1..0x7fffffff`. Never reuse a sequence for different pixel data within a session, and never jump by half the sequence space or more. A fresh nonzero session permits sequence numbering to restart.

## Sender acceptance checklist

Before calling a sender production-ready, test exact RGB24 and BGRA32 vectors, odd dimensions, last-fragment sizing, shuffled/duplicated/lost fragments, sequence wrap, process restart with a new session, heartbeat starvation, positive and negative monotonic clock offsets, delayed clock replies, and capture timestamps old enough to trigger Receiver's freshness rejection. Measure sustained packet rate and loss on the real LAN; `tools/test_sender.py` timings do not establish capture performance.
