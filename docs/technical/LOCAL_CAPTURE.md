# Local desktop capture

On Windows 10/11, choose **Connections > Video source > Picture source**:

- **UDP sender** uses the existing sender IP, frame port, ownership and clock-sync protocol.
- **Local desktop capture** reads an attached display on this PC. Select **Capture
  display** and use **Refresh displays** after changing display connections.

End the session before changing the source or display. Both choices are saved in
profiles and app preferences, and retained when switching models. Existing
profiles use UDP. Mouse output is independent: either source can be used with the
Pi output or the Windows test mouse.

## Capture path

Local capture uses the documented DXGI Desktop Duplication API on the display's
D3D11 adapter. It does not inject code, install hooks or drivers, or change another
process. Windows capture exclusions and protected-content masking remain in effect.

The GPU copies only an exact centered square matching the model input size into
a reusable CPU-readable staging texture. Row-pitch padding is removed into pooled
BGRA frame buffers; the existing inference backends consume BGRA directly. There
is no resizing, image compression, CPU color conversion, frame packetization,
UDP frame socket, or additional capture thread. A CPU readback is still required
by the current `Frame` and Python worker interfaces; this is not a zero-copy GPU
pipeline.

Capture runs on the inference worker when it is ready, after any configured FPS
limit. It acquires the latest desktop rather than queuing older images. The crop
follows model input-size changes automatically. Lower input sizes reduce readback
and inference work. Preview and data collection work with either source.

DXGI presentation timestamps are mapped into Receiver's monotonic clock. Local
frames do not require sender clock synchronization, and the existing maximum-age
checks apply through output submission. Timeouts and pointer-only updates do not
create duplicate images or refresh old detection deadlines. A static desktop can
therefore pause assistance until a new desktop image arrives.

## Requirements and troubleshooting

- Select an attached, unrotated display with Desktop Duplication support. The crop
  must fit inside it; model input sizes are at most 1024 pixels.
- Keep Receiver's own window outside the captured center region. This captures
  the visible desktop, so other windows covering the target also appear.
- Secure desktops, unavailable displays, mode switches and GPU/device loss can
  invalidate duplication. Capture errors stop the session and release controls;
  restore the desktop, select the display again if necessary, and restart.
- Linux supports UDP input. Selecting local capture there fails before workers start.

## Validation

`receiver_capture` checks source handling on Linux and, on an accessible Windows
desktop, paints a known color to verify exact center-crop BGRA pixels, timestamps,
preview/inference, model-size changes, bounded shutdown and bypass of an occupied
UDP frame port. The Windows test skips when Desktop Duplication is unavailable.
Actual FPS and latency depend on the display, driver, GPU and model and require
measurement on the target Windows hardware.
