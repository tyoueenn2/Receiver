# Move action return and calibration implementation plan

Status: implemented in the current Receiver, FrameSender, and Raspberry Pi USB proxy sources. Automated and Windows compilation checks are recorded in [the user guide](MOVE_ACTION_RETURN.md); actual Pi/Windows precision and timing measurements remain deployment acceptance work. This document retains the design and acceptance targets.

Implement one Pi operation that moves by a calibrated offset, performs one mouse click, and returns by the synthetic offset actually written. Add **Calibrate...** to Receiver's **Movement** page, with automatic desktop calibration, automatic calibration from a stationary view where measurement is reliable, and manual calibration. Receiver accepts pixel targets and converts them to relative HID counts; the Pi remains responsible for ordered USB execution.

The operation can make a short excursion appear stationary, but display timing and application input processing determine what is visible and whether the action occurs at the intermediate position. USB completion, verified cursor position, and application action success are separate results.

## 1. Existing integration points

| Component | Current behavior | Required extension |
| --- | --- | --- |
| [Receiver Movement page](../../src/gui.cpp) | Sensitivity, smoothing, movement limits, and shaping | Calibration controls, progress, results, and operation settings |
| [Receiver application](../../include/receiver/app.hpp) and [control loop](../../src/app.cpp) | One persistent Pi socket, telemetry gates, automatic clicking, and release handling | Exclusive calibration/action state, command submission, cancellation, and progress snapshots |
| [Receiver profiles](../../src/config.cpp) | Validated JSON with defaults for existing profiles | Versioned calibration records and action settings |
| [FrameSender control thread](https://github.com/tyoueenn2/Sender/blob/main/src/main.cpp) | Heartbeats, clock replies, and crop requests independent of capture | Cursor feedback, display geometry, and feedback capability negotiation |
| [Proxy UDP server](https://github.com/tyoueenn2/usb-proxy-udp/blob/main/udp_server.cpp) | Relative movement and idempotent scheduled clicks | An ordered movement/action transaction and capability response |
| [Proxy mixer](https://github.com/tyoueenn2/usb-proxy-udp/blob/main/mouse_mixer.cpp) | HID splitting and writer feedback for click/release edges | Successful synthetic displacement accounting and movement-stage completion |

`UPX1` uses replacement of pending movement corrections. It is unsuitable for submitting all stages of this operation. `UPC1` completion proves writer completion of click edges, and does not acknowledge preceding movement as an application-visible position. UPT3 cumulative motion counters contain physical input only; they cannot measure the cursor response to a calibration probe.

## 2. User behavior and calibration button

Add a **Calibration** section immediately after Sensitivity on the Movement page:

- **Calibrate...** opens a small wizard with **Desktop cursor** and **Fixed crosshair / rotating view** contexts.
- Offer **Automatic** when the required feedback is available and **Manual** otherwise. Show a specific unavailable reason, such as an older Sender without cursor feedback.
- Show the current profile's calibration status: **Not calibrated**, **Ready**, **Needs verification**, or **Failed**, plus measured error and the conditions under which it was validated.
- Provide **Verify**, **Clear calibration**, and **Cancel**. While measuring, show the current axis, trial count, progress, and outcome.
- Explain that starting calibration makes small mouse movements. Calibration performs no clicks. Start with a brief countdown so the button click has been released and the user can leave the mouse still.

Automatic calibration selects probe sizes, measures the response, rejects bad samples, fits a mapping, and verifies it without the user entering a sensitivity value. The user still chooses the context and starts calibration. For a view, ask the user to hold a stationary textured scene; for desktop use, require room away from cursor boundaries.

Run calibration asynchronously. Continue rendering the UI, receiving frames, renewing telemetry, and servicing the existing Pi lease. Calibration obtains exclusive synthetic output, pauses ordinary tracking and automatic clicking, and leaves ordinary assistance disarmed when finished or cancelled. A calibration request authorizes its probes through a separate calibration state; it must not require arming normal assistance or a detection model.

Add a **Move, click, and return** setting with button, settle time before the click, press duration, and delay before returning. Include **Test movement only** first, followed by an explicitly invoked **Test click and return**. Label the settings in time units; derive report pacing from the endpoint rather than exposing an invented speed multiplier.

## 3. Coordinate and calibration model

Add proposed `include/receiver/calibration.hpp` and `src/calibration.cpp` containing typed observations, fit results, validation, and pixel-to-count conversion. Keep physical calibration separate from `gain_x` / `gain_y`, which currently control correction strength rather than encode a measured device mapping.

Define these coordinate spaces explicitly:

1. The frame's unscaled crop pixels, before model letterboxing.
2. Physical desktop pixels, including signed virtual-desktop origins.
3. Relative HID counts on the selected Pi endpoint.
4. For a rotating view, displacement relative to the reference/crosshair in the observed scene.

For desktop use, convert a crop point to desktop pixels using the geometry belonging to that frame, then subtract a fresh cursor position. Initially fit `pixel_delta = J * count_delta`, where `J` is a signed 2 by 2 response matrix. Retain inversion and meaningful cross-axis effects; reject a singular or unstable fit. Use rounding with measured error bounds and independent verification. A cursor position plus a calibrated mapping is required for absolute desktop targets.

For a rotating view, use a measured local mapping or monotonic response table within the tested displacement range. Image displacement changes with field of view and position in the image. A calibration near the center must not be extrapolated across the display. If an angular model is added, use an explicitly supplied/independently validated FOV and sensitivity for that view; otherwise reject targets outside the measured range. Keep zoom modes as separate calibration records.

Persist the method, model/schema version, signed coefficients or response samples, tested range, repeatability and validation errors, endpoint identity/layout/ranges/poll interval, display geometry, crop dimensions, context, and user-entered sensitivity/FOV/zoom identifiers. Treat Pi process epochs as live command state, not permanent device identities. A restart requires renewed capabilities and endpoint verification; a genuine layout or timing change requires calibration verification. Display, crop, context, or known sensitivity changes invalidate the applicable record. Mouse DPI and in-application sensitivity changes that cannot be observed require user-triggered verification.

Default new profile fields to disabled/unset so existing profiles load. Validate finite values, units, matrix conditioning, limits, and record version before accepting saved calibration.

## 4. Automatic desktop calibration

Extend FrameSender to provide cursor observations from the target Windows computer using documented cursor APIs. Receiver's local cursor is valid feedback only when capture and Pi output refer to that same computer. Do not use cursor position as feedback for a view that locks or recenters the cursor.

Introduce a separately versioned cursor-feedback request/reply on Sender's existing owned UDP endpoint. Preserve UVF1, UVH1, UVN1, and clock packet layouts. Bind requests to the current Sender session and an echoed request token; return sample sequence/time, cursor validity/visibility and position, selected output bounds, crop origin/dimensions, and a geometry generation. Associate crop geometry with a specific frame sequence, or an explicit sequence boundary when geometry changes, so reordered feedback cannot map a frame with the wrong crop origin. Publish requested cursor observations even when the desktop image is unchanged, without requiring model initialization. Set consistent DPI awareness so cursor and DXGI geometry use physical pixels. Link any required Windows cursor/DPI libraries in Sender's CMake target.

Calibration sequence:

1. Confirm Pi readiness, supported probe operations, fresh cursor observations, stable geometry, and no active synthetic/physical buttons. Drain previous synthetic work through an acknowledged barrier.
2. Measure a stable baseline and check that the cursor has space for bounded initial probes. Begin with small count values; expand only after observing the response.
3. Probe positive and negative X and Y at several amplitudes using the same HID splitting, per-report movement, and pacing as the eventual action command. Use a bounded midpoint wait/continue so Receiver can observe the intermediate position before returning, without clicking. A timed-out observation skips measurement and triggers the declared return policy.
4. Restore the starting location between trials using fresh feedback and a bounded number of corrections. Pause/retry if physical motion occurs. Reject clipping, missing samples, axis sign changes, stale geometry, or inconsistent returns.
5. Fit the response matrix robustly. Compare signs, amplitudes, and pacing patterns to detect acceleration or other nonlinear behavior. For this first implementation, fail a materially nonlinear fit with a clear instruction to use fixed pointer settings or manual calibration; do not silently apply a linear model.
6. Verify on probe sizes/directions excluded from fitting, including a diagonal. Verify outward landing and return error separately. Commit a new record only after both pass; retain the previous record on cancellation/failure.

Starting desktop acceptance target: held-out landing and return error no greater than one physical pixel for at least 95% of trials, with maximum error and the tested range also shown. This is a release target to validate on hardware. Where count granularity prevents it, report the achieved error and leave exact-pixel operation unavailable rather than claiming precision the device cannot produce.

Normal fast operation can use the measured mapping without a network round trip between stages. A slower verified mode may wait for fresh cursor feedback before allowing the click. It requires a bounded prepare/continue transaction: the Pi waits at the target, Receiver checks the observation and continues the same command ID, and timeout skips the click and attempts the declared return policy. Final cursor feedback verifies return; it cannot prove what the target application did with the click.

## 5. Automatic calibration from a rotating view

Provide **Automatic from view** as a conditional mode for both UDP video and local capture. It measures a stationary background using controlled motion; a detector's moving target box is not an adequate calibration reference.

Implement a bounded grayscale patch-registration module in proposed `src/calibration_view.cpp`. Select several textured patches across the usable image, mask a configured crosshair/HUD region and detected foreground objects when available, and match before/after patches using coarse-to-fine normalized correlation. Require consistent patch motion, sufficient confidence, and a stable return image. Use forward/backward matching to reject ambiguous correspondences. Establish feasibility on recorded fixtures before connecting live probes.

Use small positive/negative probes on each axis at multiple amplitudes, with an independently observed midpoint and return. Calibrate the local counts-to-view response and its valid range from scene correspondences. Verify on held-out probes. Reject moving/animated scenes, low texture, a menu opening, cursor recentering mistaken for camera response, dropped/stale frames, or insufficient overlap. Automatically reduce probe size if features would leave the crop.

Correlate observations causally: record an observed pre-probe frame, wait for probe completion, discard queued older captures using synchronized frame times and their uncertainty, then require stable post-probe samples before measuring. Do not compare Pi and Sender monotonic timestamps directly. Record presentation delay separately from geometric response. The view mode passes only when measured forward and return errors satisfy the configured tolerance across its declared range. Otherwise offer manual calibration and preserve the previous record. Automatic recovery of an arbitrary game's global FOV or behavior is outside this first implementation.

## 6. Manual calibration

Use the same wizard and probe path. For a desktop, let the user supply observed pixel displacements for known count probes or enter measured counts per pixel. For a view, allow selecting the same stationary landmark in before/after frames, or supplying FOV and a measured angular sensitivity. Verify the resulting mapping with independent movement-only probes and mark an unverified manual entry accordingly. Manual input does not bypass clipping, stale observations, or device limits.

## 7. Pi command and execution contract

Introduce a separately versioned transaction protocol; working names are `UPM1` for requests and `UPR1` for replies. Add a token-bound capability query/reply without changing existing UPT3 or click packet sizes. Define exact byte offsets and independent golden vectors before implementation.

The request contract includes session/monotonic command IDs, expected server epoch and endpoint generation, operation type, signed count offsets, button, settle/press/return timings, maximum execution budget, and interruption policy. Support movement-only probing with a measurable midpoint, move/click/return, bounded prepare/continue for verification, status query, and cancellation. Keep calibration probes and action execution on the same movement path. The pixel-facing API lives in Receiver; a raw Pi client supplies calibrated counts.

Replies include accepted/active/terminal state, current stage, applied outward and return counts, click press/release completion, remaining synthetic offset, endpoint identity/generation, and failure reason. Cache terminal results so loss of a final acknowledgment does not repeat the action. A repeated immutable request replays state; changed payload for an existing ID is invalid. Prepare/continue/cancel messages must use defined legal transitions on the original transaction and must never initiate a second click.

Share the existing controller lease, bounded session tracking, and command ID ordering with click/release operations. Receiver needs a common command ID allocator for old and new operations. Cross-protocol ID reuse must be rejected, and ReleaseAll must fence earlier transaction commands so delayed requests cannot restart cancelled work. Do not silently translate an unsupported transaction into several UPX1 packets.

Pi execution states:

```text
Validate -> Move out -> Settle -> Press -> Hold -> Release -> Return -> Complete
                         |
                         +-> Wait for verified continue, when requested
```

Movement-only probes skip the button stages. Extend mixer/writer metadata to report each successful synthetic movement segment and whole-stage completion. Add each completed segment to an applied-count ledger; return the inverse of the ledger. A partially written outward stage must never cause a full requested-offset return. Use validated wide arithmetic for totals and negation, including asymmetric HID axis limits. Split movement to the actual descriptor's ranges and reject work that exceeds the segment/time budgets before admission.

Transaction stages must not be coalesced, expired as ordinary latest corrections, or interleaved with other synthetic movement/click work. Reserve bounded capacity for release and return when admitting an operation. Reject conflicting persistent holds and queued clicks rather than silently delaying the transaction. No network calls or sleeps belong in the USB writer. Schedule stage transitions from writer events and monotonic deadlines; verify whether the server's current 2 ms poll wait needs a deadline-driven wakeup for the advertised polling rate.

Physical reports retain their order and priority. Default to physical motion/button changes interrupting calibration or the transaction: skip a click not yet pressed, promptly release a synthetic press already active, and report interruption. Never invert physical motion. Use the existing live-mouse behavior outside a transaction. Admission and interruption must also work in standalone replay, which has no physical reports.

Define cancellation explicitly. Normal cancellation may perform a bounded return of the known synthetic offset when the same endpoint/layout is still usable. Manual Disarm/ReleaseAll stops further movement and releases synthetic buttons; report any residual offset. On writer failure, disconnect, generation change, process restart, or an uncertain write, stop the action, release when possible, and report an incomplete/unknown return. Do not replay old displacement into a new endpoint. Bound preparation, hold, cleanup, and lease renewal so a lost controller cannot leave an indefinite transaction.

## 8. Receiver orchestration and action API

Add proposed `App::start_calibration`, `App::cancel_calibration`, `App::verify_calibration`, `App::calibration_status`, and `App::move_click_return` entry points. Add a transaction manager beside the injected-button manager, sharing session/ID allocation and release fencing. Keep all Pi sends and replies in the existing `pi_loop()` socket; a second socket would compete for ownership. Route Sender feedback through `receive_loop()` and a bounded observation buffer. Use a worker for fitting/registration so network/control loops remain responsive.

Add a calibration-only session lifecycle because `App::start()` currently starts inference and `receive_loop()` gates UVN1 on model readiness. In this mode, start the required capture/Pi services without loading a detector; request a configured calibration crop for view measurements, and allow cursor-only feedback for desktop measurements. Local capture currently belongs to the inference loop and must also become available without inference. Freeze crop/model changes during measurement and restore ordinary capture negotiation afterward. An ordinary in-progress model load or benchmark must finish or be cancelled before calibration starts.

Choose one synthetic output owner: ordinary tracking, calibration, or a pending action. Suppress normal corrections, prediction, smoothing, movement shaping, and auto-trigger while calibration/action owns output; use the measured transaction mapping directly. Continue the telemetry and retry traffic required by the 250 ms controller lease. Reset stale controller/trigger state when leaving exclusive output, and require ordinary assistance to be armed explicitly afterward.

Accept either a pixel in a specific fresh capture frame or an explicit desktop coordinate with valid geometry. For a view, calculate the displacement from the configured reference point. Check calibration compatibility, measured range, frame age, fresh feedback where required, endpoint capability, and button state immediately before submission. Freeze the submitted target and immutable command payload. Expose the command API and a direct test action first; automatic target selection should invoke this same API once per activation/target event, with rearming after completion so one target cannot cause repeated transactions accidentally.

Initial Pi operation supports one click. Preserve existing continuous tracking and scheduled-click behavior when this feature is disabled. Local Windows and MAKCU backends show the new Pi transaction feature as unavailable; they must not pretend to support equivalent timing or acknowledgments. Local capture can still supply view observations for a supported Pi output.

## 9. Delivery order

| Step | Deliverable | Completion gate |
| --- | --- | --- |
| 1 | Coordinate types, calibration records, capability and transaction wire specifications | Golden vectors agree across proxy, Receiver, and Python client; old packet layouts remain valid |
| 2 | Proxy movement ledger and transaction scheduler, plus Python command helper | Deterministic writer tests demonstrate ordered stages, exact inverse accounting, retries, and interruption |
| 3 | Sender cursor/geometry feedback and Receiver observation parsing | Cursor samples work on a static desktop; geometry/frame/session matching and DPI tests pass |
| 4 | Calibration engine, Movement button/wizard, manual mode, automatic desktop mode | Independent desktop probes meet the selected tolerance and saved profiles round-trip |
| 5 | Stationary-view registration and automatic view mode | Synthetic and recorded fixtures verify response fitting, useful-range limits, and rejection cases |
| 6 | Pixel-facing action API, settings, and test controls | End-to-end simulated peer verifies exclusivity, application gating, and cancellation |
| 7 | Pi/Windows hardware validation and user documentation | Measured landing/return error, input consumption, timing, and supported hardware are recorded |

Update Receiver's CMake targets, FrameSender's portable protocol tests/Windows link libraries, the proxy Makefile, protocol documentation, Python client, and device simulators as each implementation lands. Keep changes in their authoritative repositories; Receiver's Pi integration fixtures cannot substitute for the actual proxy changes.

## 10. Validation and acceptance

Use meaningful tests for state transitions, measurement, and failure handling:

- **Wire compatibility:** malformed/truncated/oversized packets, reserved fields, capability fallback, wrong sessions/epochs/generations, duplicate/changed requests, shared ID ordering, and delayed traffic after ReleaseAll.
- **Proxy execution:** split and asymmetric axes; partial outward success; return accounting; bounded capacity; expired deadlines; physical motion and holds before/during a command; lost ACKs; interrupted writes; watchdog cleanup; and live versus replay mode. Preserve the existing physical/synthetic mask rules.
- **Calibration math:** known signed/scaled/cross-axis mappings, count quantization, noise/outliers, nonlinear acceleration, clipping, insufficient samples, invalid coefficients, and independent validation trials.
- **Feedback and view measurement:** old Sender compatibility, static desktop cursor movement, negative display origins, mixed DPI, crop/display changes, packet reordering, stale samples, synthetic rotations/translations, moving foreground objects, blank scenes, and ambiguous patches.
- **Receiver behavior:** one output owner; calibration without a detector or arming ordinary tracking; calibration-only crop negotiation and static cursor feedback; Cancel/Disarm/Delete/shutdown; sender/Pi restart; profile switching; no auto-trigger during probes; and no automatic repeat after uncertain completion. Add simulator scenarios and GUI checks to the existing test infrastructure.

On actual hardware, verify an instrumented desktop test surface records a click at the intended position and the observed cursor returns within the accepted tolerance. Measure the full cycle at supported endpoint polling rates, HID ranges, display refresh rates, and settle/hold timings. Record landing/return error, p50/p95/p99 completion time, interruption behavior, and physical-input delay. For a rotating view, validate scene return and application action timing separately. Report writer completion, observed return, and application-observed action distinctly. No fixed millisecond or invisible-movement guarantee should be published before these measurements.
