# Move click and return

Receiver converts a pixel target into calibrated mouse counts and asks the Pi to execute one ordered move, click, and return operation. The Pi returns the synthetic offset it successfully wrote, preserving the physical mouse's own input. The target computer still receives the movement; display refresh and application input processing determine whether it looks stationary and whether the click is consumed at the intended position.

## Install compatible components

Build the updated USB proxy, Receiver, and FrameSender together. Existing movement and click packets remain compatible, but this operation requires the new proxy capability response. Desktop automatic calibration also requires updated FrameSender cursor feedback. A Receiver-side simulator does not update a deployed Pi.

Use Pi UDP as the output device. The local Windows and MAKCU outputs do not expose this transaction. Local Windows capture can provide calibration images or cursor observations when the Pi also controls that same computer.

## Calibrate

1. Configure Receiver's Pi and capture connections. Open **Movement > Calibrate...**. Calibration can start its own capture session without loading a detection model.
2. Choose **Desktop cursor** for a normal pointer, or **Fixed crosshair / rotating view** for an application that turns the camera. Choose **Automatic** and start calibration.
3. Release the mouse buttons during the countdown and leave the mouse still. For desktop use, place the cursor near the display center. For a view, choose a stationary, textured scene with little HUD or moving foreground.
4. Wait for the measured landing error, return error, and movement range. Receiver applies a record only after independent verification; a failed automatic run preserves the previous record. Use **Cancel calibration** to skip any pending click and request a bounded return. Calibration itself never clicks.

Desktop calibration uses cursor positions from the target computer, including when the desktop image is static. It measures both axes and directions, gradually expands probes within display/device limits, and rejects an inconsistent linear response. Pointer acceleration, boundary clipping, or changes in sensitivity can prevent a valid result.

View calibration matches several background patches before movement, at the intermediate position, and after return. It rejects low texture, inconsistent scene motion, stale images, and geometry changes. Its mapping is valid only in the measured local range. It does not infer a global FOV model or automatically discover zoom settings; use a separate saved profile and verify it for each view/zoom/sensitivity configuration.

For **Manual**, enter signed pixels per count and any measured cross-axis response. Enter the measured count range and allowed pixel error, then choose **Verify calibration**. Verification uses movement-only probes on both axes and independent diagonals. Manual values still require the selected context's feedback for verification and pixel targeting. Unverified manual coefficients cannot enable a pixel action. Standard profiles save the calibration and operation settings.

Changes to endpoint layout/timing or display/crop geometry require verification. Settings inside another application that Receiver cannot observe require verification after you change them. **Clear calibration** removes the current record. Calibration pauses normal assistance; when calibration started its own session, end that session and start a normal session before arming tracking.

## Test and use the action

In **Movement > Move, click, and return**, choose the mouse button and settle, press, and return-delay timings. Enter a target in physical desktop coordinates, or uncheck **Test uses desktop coordinates** to use unscaled capture pixels. **Use image center** fills the capture center. Try **Test movement only** before **Test click and return**.

**Verify desktop position before clicking** makes the Pi wait while Receiver checks fresh target-computer cursor feedback. If landing is outside tolerance, Receiver skips the click and requests the return. This mode adds network/feedback latency. Final desktop feedback separately reports whether the return was observed. A successful USB report does not establish what an application did with the click.

Enable **Use move, click, and return** to use the selected detection/color aim point during normal armed assistance. It submits one frozen target and disarms after submission. Re-arm to permit another action. Ordinary smoothing, shaping, tracking corrections, and auto-trigger do not run during the operation. A held activation button can coexist with a different synthetic click button; the requested click button must be released. Physical motion or a button change during the transaction interrupts it.

Targets outside the calibrated count radius or beyond achievable count quantization are rejected. Do not increase the saved range beyond what was measured. For a rotating view, settle/press timings must allow the application to process the intermediate view; making every stage shorter does not guarantee a correctly located action.

**Disarm**, Delete, ReleaseAll, and shutdown stop further synthetic movement and request button release. They do not issue a new return movement after the stop. A normal calibration cancellation can return the known offset while the same endpoint remains usable. Disconnects, writer failures, or endpoint changes can leave an incomplete or unverified return, which is reported rather than replayed against a new endpoint.

## API and verification

`App::start_calibration(settings, context, verify)` starts calibration asynchronously (`0` desktop, `1` view). `calibration_status()` returns progress/error information. `move_click_return(x, y, desktop_coordinates, click)` submits a target using the stored verified mapping. It uses the existing Pi socket and shared command ID allocator.

The proxy's Python `MouseProxy.move_action_return()` submits relative HID counts directly; pixel conversion belongs to Receiver. See the proxy's `USAGE.md` for wire layouts and Python examples.

Automated checks cover response fitting, signed/cross-axis mappings, quantization, profile persistence, image registration, protocol validation, partial writes, duplicate requests, cancellation, and real Receiver orchestration against loopback peers. The integration scenarios include automatic desktop/view calibration, manual verification, missing cursor feedback, nonlinear response rejection, and verification that skips a misplaced click. Windows cross-compilation/syntax checks do not measure a real desktop, Raw Gadget endpoint, or application. Precision and timing on actual Pi/Windows hardware remain deployment checks.
