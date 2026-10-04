# Receiver network protocols

All integers are in network byte order. Signed integers use two's-complement representation. One message occupies one UDP datagram; native C/C++ structs are never serialized directly. These are unauthenticated LAN protocols, and Receiver accepts packets only from its configured peers.

## UVF1 raw frames and clock synchronization

UVF1 uses the existing 48-byte header followed by raw RGB24 or BGRA32 data:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UVF1` |
| 4 | 1 | Version 1 |
| 5 | 1 | Format: 1 RGB24, 2 BGRA32 |
| 6 | 2 | Header length 48 |
| 8 | 8 | Nonzero sender session |
| 16 | 4 | Frame sequence modulo 2^32 |
| 20 | 8 | Sender monotonic capture time, ns |
| 28 | 2 | Width |
| 30 | 2 | Height |
| 32 | 4 | Complete raw frame length |
| 36 | 2 | Fragment index |
| 38 | 2 | Fragment count |
| 40 | 4 | Payload byte offset |
| 44 | 2 | Fragment stride |
| 46 | 2 | Reserved, zero |
| 48 | variable | Raw image bytes |

Rows are tightly packed. Width and height are 1–1024. The raw length must match the dimensions and channel count. Fragment stride is 1024–1352, metadata must agree across a frame, incomplete frames expire after 20 ms, and frame/session sequence comparisons use the unsigned half-range rule.

The existing auxiliary datagrams are unchanged:

- `UVH1 | reserved:u32=0 | sender_session:u64` (16 bytes), sent every 50 ms.
- `UVC1 | reserved:u32=0 | sender_session:u64 | t0:u64` (24 bytes).
- `UVS1 | reserved:u32=0 | sender_session:u64 | t0:u64 | t1:u64 | t2:u64` (40 bytes).

The receiver bounds sender clock offset using `[t2-t3, t1-t0]`, rejects malformed or over-50-ms exchanges, and expires synchronization after two seconds. Fresh-frame, sender ownership, activation, and stale-frame rules are unchanged.

## UPX1 movement and persistent synthetic buttons

UPX1 is exactly 16 bytes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPX1` |
| 4 | 4 | Sequence modulo 2^32 |
| 8 | 2 | Relative X, signed |
| 10 | 2 | Relative Y, signed |
| 12 | 1 | Wheel, signed |
| 13 | 1 | Horizontal pan, signed |
| 14 | 1 | Complete persistent injected-button mask |
| 15 | 1 | Reserved, zero |

Button usages 1–8 map to bits `1 << (usage - 1)`. The mask is Receiver's entire desired persistent synthetic state, never a delta. Every ordinary movement takes a fresh snapshot and repeats it. Physical UPT buttons are never copied or ORed into the mask; the Pi independently merges physical, persistent, and scheduled-click state.

Every datagram receives a fresh sequence number, including button transitions, zero-motion releases, held-button heartbeats, and release snapshots. Zero is valid after `0xffffffff`. X/Y and wheel/pan are clamped only to their wire/HID limits and the existing movement policy.

Button transitions are sent with zero movement when no correction is ready. A nonzero persistent mask is refreshed by a zero-motion heartbeat every 75 ms from the Pi-output worker. A zero mask with no transition or release pending has no heartbeat.

## UPC1 request version 2

Receiver emits only the public 40-byte version-2 request. The obsolete private 32-byte request is neither emitted nor accepted.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPC1` |
| 4 | 1 | Version 2 |
| 5 | 1 | Operation: 1 Schedule, 2 ReleaseAll |
| 6 | 1 | HID usage 1–8; zero for ReleaseAll |
| 7 | 1 | Reserved, zero |
| 8 | 8 | Nonzero random client session |
| 16 | 8 | Nonzero monotonically increasing command ID |
| 24 | 4 | Click count |
| 28 | 4 | Press duration, microseconds |
| 32 | 4 | Release-completion-to-next-press gap, microseconds |
| 36 | 4 | Reserved, zero |

Schedule validation is usage 1–8, count 1–10,000, positive press duration no greater than five seconds, press duration not below the UPT3 endpoint poll interval, and interval zero through 60 seconds. ReleaseAll uses zero for usage, count, and both durations. Version-2 scheduling is rejected locally unless valid, ready UPT3 endpoint timing is known.

Timing is writer-relative: press duration begins only after the Pi USB writer successfully completes the press report, and the interval begins after successful release completion. The Pi maintains scheduled buttons independently of UPX1 persistent state, so movement and hold heartbeats cannot cancel a schedule. A click on a persistently held same button is rejected; different-button holds and schedules may coexist.

## UPA1 response version 2

UPA1 is exactly 48 bytes. The obsolete 32-byte response is rejected as malformed.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPA1` |
| 4 | 1 | Version 2 |
| 5 | 1 | Status |
| 6 | 1 | Button usage |
| 7 | 1 | Reserved, zero |
| 8 | 8 | Client session |
| 16 | 8 | Command ID |
| 24 | 4 | Accepted clicks for this command |
| 28 | 4 | Completed clicks for this command |
| 32 | 8 | Nonzero Pi server epoch |
| 40 | 2 | Scheduler queue depth |
| 42 | 2 | Reserved, zero |
| 44 | 4 | Reserved, zero |

Statuses are 1 Accepted, 2 Duplicate, 3 Completed, 4 Busy, 5 QueueFull, 6 UnsupportedButton, 7 Invalid, 8 Cancelled, 9 ButtonActive, 10 NotReady, and 11 StaleCommand.

Receiver constructs immutable request bytes once. Before acceptance, no-response retries use the same bytes and ID after 50 ms. QueueFull retains the same command and backs off 25 ms, 50 ms, then 75 ms capped. After Accepted or Duplicate, Receiver continues same-ID status/lease requests at least every 75 ms until terminal. Command N+1 is not transmitted before N is known Accepted, Duplicate, or Completed. Duplicate/reordered responses cannot double-count transitions, and aggregate telemetry counters are never used to infer command completion.

Responses are accepted only from the configured Pi endpoint, for version 2, the current client session, a known already-sent ID, and the telemetry-confirmed server epoch. Completed means the final release completed successfully in the Pi USB writer. A Schedule `ButtonActive` with zero accepted/completed counts is rejection before acceptance. `ButtonActive` with `accepted == requested` and `completed < accepted` is a legitimate post-acceptance termination caused by physical-button interference. `Cancelled` can likewise retain accepted and partial-completion counts. Receiver preserves the greatest observed progress, counts acceptance once even when the terminal response is the first ACK received, and records post-acceptance `ButtonActive` separately from pre-acceptance rejection. Partial accepted counts, completed greater than accepted, and a failed terminal status claiming every click completed are impossible and fail closed.

Busy, QueueFull, UnsupportedButton, NotReady, Invalid, and StaleCommand are pre-acceptance statuses and require zero counts. Invalid, StaleCommand, malformed responses, and impossible per-command counts trigger Receiver's fail-closed Pi protocol path.

Status labels have exact scopes:

- **Submitted locally**: placed in Receiver's bounded local queue.
- **Accepted by Pi**: confirmed stored idempotently by the Pi.
- **Completed by USB writer**: final release reported successfully completed by the Pi writer.
- None alone proves that the physical USB link delivered the report or that the destination application consumed it.

## Release fence

Manual ReleaseAll, GUI disarm, shutdown, sender loss/restart, stale or invalid Pi telemetry, Pi socket/protocol error, GPU/inference failure, restart-required configuration change, server-epoch change, and existing fail-closed transitions use the same manager routine. It immediately clears manual/scoped desired ownership, invalidates guards, cancels local clicks, records the reason, wakes the worker, and ensures one version-2 ReleaseAll is fenced. Repeated release triggers coalesce onto the existing obligation and at most one three-snapshot batch; they do not grow an unbounded release queue.

The worker sends ReleaseAll before normal work. No-response timeouts and nonterminal Busy/QueueFull/NotReady responses retry the exact immutable bytes and ID. The audited Pi can cache a terminal `Cancelled` for a release that lost endpoint readiness. Retrying that ID can never succeed, so Receiver atomically replaces it under the still-closed fence with a same-session, strictly higher command ID. Only creation of that replacement supersedes the old obligation. Delayed replies for superseded IDs are ignored and cannot open the fence; only valid Completed for the current replacement can do so. A confirmed server-epoch change instead retires all old-session work and creates a new-session release fence.

The worker also sends three best-effort UPX1 zero-motion, zero-wheel, zero-pan, zero-mask snapshots with fresh sequence numbers. Later presses remain behind the fence. Shutdown gives the worker up to 100 ms to deliver and retry this work. A UPA1 Completed response is the only evidence used to label ReleaseAll completed by the writer; the three UPX1 snapshots are not scheduled-click completion proof. If the socket is unavailable, desired local state remains zero and the Pi's 250 ms watchdog is the final fallback.

## UPS1, UPS2, and UPS3 subscriptions

All subscriptions are 24 bytes:

`magic[4] | reserved:u32=0 | receiver_session:u64 | token:u64`

Both IDs are nonzero. Receiver renews the selected subscription every 20 ms. The Pi subscription lifetime remains 100 ms, while Receiver requires both receipt and an echoed token issued for that exact protocol version within 50 ms. A UPS1 token cannot validate UPT3, and a UPS3 token cannot validate UPT1. Packets must also come from the configured Pi endpoint, echo the current Receiver client session, and satisfy the server-epoch and sequence rules below.

At startup or after confirmed peer loss/server-epoch change, Receiver probes UPS3 for 100 ms and then selects UPS1 if UPT3 is unavailable. While UPS1 is healthy, Receiver keeps renewing it and sends one-shot UPS3 upgrade probes in the background. A probe has a distinct token and a 50 ms response window; failed probes use bounded exponential backoff beginning at 250 ms and capped at 4 seconds. A probe does not reset the UPT1 gate, physical-button state, output flow, or release fence. When UPT1 first recovers after an outage, the next UPS3 attempt is accelerated. Specific unknown/unsupported replies are expected during probing and do not disable UPT1.

Only a valid UPT3 response for the currently outstanding UPS3 token can upgrade a healthy UPT1 session. Delayed responses for older probes, wrong-version token echoes, unexpected peer/client packets, and unsolicited UPT3 packets are rejected. A confirmed upgrade switches renewals to UPS3, resets the physical-motion baseline before consuming the new cumulative sample, and refreshes UPT3 endpoint timing used by scheduled clicks. A server-epoch change still invokes the normal fail-closed release/session-rotation path before telemetry is re-established.

Receiver deliberately does not auto-negotiate UPT2 because deployed 80-byte encoders use conflicting layouts with no wire discriminator. Full scheduling and direction assistance therefore require UPT3; UPT1 is the explicit reduced-capability fallback.

## UPT1 compatibility

UPT1 remains exactly 56 bytes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPT1` |
| 4 | 1 | Ready flag |
| 5 | 1 | Physical button mask only |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Receiver session |
| 16 | 8 | Pi server epoch |
| 24 | 8 | Echoed token |
| 32 | 4 | Telemetry sequence modulo 2^32 |
| 36 | 4 | Native X minimum, signed |
| 40 | 4 | Native X maximum, signed |
| 44 | 4 | Native Y minimum, signed |
| 48 | 4 | Native Y maximum, signed |
| 52 | 4 | Reserved, zero |

UPT1 is sufficient for button activation when direction assistance is disabled. Receiver does not fall back to uncorrelated `+state` polling.

## UPT2 compatibility

The canonical UPT2 is 80 bytes. Bytes 0–55 use the UPT1 layout with magic `UPT2`; offsets 6–7 and 52–55 are reserved and zero. Its extension is:

| Offset | Bytes | Field |
|---:|---:|---|
| 56 | 8 | Mouse endpoint/layout generation |
| 64 | 8 | Pi monotonic sample time, ns |
| 72 | 2 | Last physical relative X, signed |
| 74 | 2 | Last physical relative Y, signed |
| 76 | 4 | Physical motion age, microseconds; `0xffffffff` if none |

The last-delta fields support diagnostics but are not loss/reorder-safe velocity. The canonical decoder remains covered for explicit packet compatibility, but Receiver never auto-negotiates or uses this 80-byte format as reliable direction input.

`usb-proxy-udp` revision `ef4abe246a614bd22cc21116fd14ecc885edbb60` emits a different 80-byte packet: persistent/scheduled masks at 6–7, physical deltas at 52/56, click counters at 60/64, active/queued counts at 68/70, and generation at 72. That layout is unsupported and representative golden packets are rejected. Because a quiet packet can overlap valid canonical values, packet contents cannot safely identify which same-size layout was intended. Receiver therefore does not guess or request UPT2 from this proxy; it uses that proxy's UPT3 implementation.

Receiver also accepts its legacy 88-byte cumulative UPT2. Bytes 0–55 again match UPT1 except for the magic:

| Offset | Bytes | Field |
|---:|---:|---|
| 56 | 8 | Mouse endpoint/layout generation |
| 64 | 8 | Pi monotonic sample time, ns |
| 72 | 4 | Cumulative physical X modulo 2^32 |
| 76 | 4 | Cumulative physical Y modulo 2^32 |
| 80 | 4 | Physical motion age, microseconds; `0xffffffff` if none |
| 84 | 4 | Reserved, zero |

Packet length distinguishes the two decoder formats. The 88-byte cumulative counters remain covered for legacy/offline compatibility and contain physical movement only using modulo-2^32 differences. They are not auto-negotiated; production direction assistance requires UPT3.

## UPT3 full telemetry

UPT3 is exactly 128 bytes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPT3` |
| 4 | 1 | Ready flag |
| 5 | 1 | Physical button mask only |
| 6 | 1 | Applied persistent injected mask |
| 7 | 1 | Applied scheduled-click mask |
| 8 | 8 | Receiver session |
| 16 | 8 | Pi server epoch |
| 24 | 8 | Echoed token |
| 32 | 4 | Telemetry sequence modulo 2^32 |
| 36 | 4 | Native X minimum, signed |
| 40 | 4 | Native X maximum, signed |
| 44 | 4 | Native Y minimum, signed |
| 48 | 4 | Native Y maximum, signed |
| 52 | 4 | Endpoint poll interval, microseconds |
| 56 | 8 | Mouse endpoint/layout generation |
| 64 | 8 | Pi monotonic sample time, ns |
| 72 | 8 | Cumulative physical X, signed |
| 80 | 8 | Cumulative physical Y, signed |
| 88 | 4 | Physical motion age, microseconds; `0xffffffff` if none |
| 92 | 4 | Epoch accepted-click total |
| 96 | 4 | Epoch completed-click total |
| 100 | 2 | Active sequence count |
| 102 | 2 | Queued sequence count |
| 104 | 4 | Physical reports received |
| 108 | 4 | Physical reports submitted |
| 112 | 2 | Endpoint/output queue depth |
| 114 | 2 | Pending synthetic-item depth |
| 116 | 4 | Superseded synthetic movement count |
| 120 | 4 | USB writer failure count |
| 124 | 4 | Reserved, zero |

Receiver validates exact size, reserved bytes, session/token/epoch, readiness-dependent endpoint and generation values, axis ranges, monotonic sequence/sample time, and consistent counters before use. Duplicate/reordered telemetry is rejected by the modulo-2^32 half-range rule.

Direction tracking uses differences between UPT3 cumulative signed 64-bit physical counters. Server-epoch or layout-generation changes reset the baseline; non-increasing sample time, reception gaps, excessive sample gaps, discontinuities, and stale motion age invalidate or pause assistance. Two accepted samples are required after reset. Injected movement is never mixed into physical direction calculations.

A telemetry-confirmed server-epoch change discards old pending state, increments the reset metric, clears persistent desired state, treats old ReleaseAll work as fenced by the restart, rotates to a new nonzero random click-client session with command numbering reset, starts a new ReleaseAll fence, and reprobes UPS3. Old click commands are never resubmitted, and delayed old-session/old-epoch responses are ignored.

## Socket ownership and test scope

One long-lived nonblocking UDP socket carries UPS subscriptions, UPT telemetry, UPX1, UPC1, ReleaseAll, and UPA1. This preserves the source IP/port used by the Pi ownership lease. Inference, control, GUI, and API callers only validate/enqueue and wake the worker; they do not sleep or perform socket I/O.

The deterministic tests use an independent fake Pi/proxy and virtual-time state-machine checks. They validate Receiver framing, ordering, retry, freshness, state separation, and status accounting. They do not establish Raspberry Pi, Raw Gadget, real-mouse, physical USB, GPU, or destination-application reliability.
