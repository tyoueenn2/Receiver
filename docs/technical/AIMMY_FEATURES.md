# Interface and Aimmy feature coverage

Receiver uses its native C++/ImGui interface with a dark blue theme, a searchable
task-based sidebar, two-column settings, a dedicated live preview, connection status and a
persistent movement-off button. Settings stack into one column at narrow widths.
The minimum window is 1060 x 740. Q/E cycles categories when the window has focus and
you are not typing. Mouse control starts disarmed.

The comparison uses [Aimmy's V2 settings](https://github.com/Babyhamsta/Aimmy/blob/Aimmy-V2/Aimmy2/Class/Dictionary.cs).
The implementations here are written for Receiver; no Aimmy implementation or model
assets were copied.

## Added features

| Feature | Location and behavior |
| --- | --- |
| Continuous assistance (constant tracking) | **Activation > Run assistance > Continuously while armed**. Selects targets without requiring a held button. Changing this mode disarms. Delete still stops output. |
| Automatic clicking (auto trigger) | **Auto click > Enable automatic clicks**. A selected box must contain the configured crosshair for the full delay. Once-per-target mode fires once until a fresh result loses the target, it leaves the crosshair, or the user releases activation/disarms. Temporary stale input keeps the single-shot latch. |
| Repeated clicking (spray) | **Auto click > Click behavior > Repeat while target stays centered**. Repeats with a configurable interval, one pending click at a time. Press time and button are configurable. Changing click settings disarms and cancels pending output. |
| Cursor check | **Activation**, local Windows mouse only. Pauses assistance while the Windows cursor is visible. Receiver cannot inspect the other computer's cursor. |
| Preview window | **Preview → Enable preview window** opens a separate resizable Windows window. This is the only video preview; it continues while browsing settings or minimizing the main window. Closing it disables the saved preview toggle; enable it again to reopen. **Bring preview to front** restores a minimized preview. The window follows the app's always-on-top and capture-exclusion settings. |
| Preview overlays | **Preview**. Independently show boxes, confidence and FOV; optional tracers from the top, center or bottom. Colors, opacity, box corner radius, thickness and confidence font size are saved. Geometry is clipped to the received image. HSV markers use the same settings as targeting. |
| Theme and window behavior | **App preferences**. Accent color, always on top and Windows capture exclusion for the Receiver window. Capture exclusion depends on Windows and the capturing application's support. |
| Model-switch shortcut | **App preferences**. Rebindable key, default backslash/pipe. Cycles the discovered model library while Receiver is focused; switching disarms. Q/E and Delete are reserved. |
| Aimmy config import | **Profiles**. Reads flat JSON `.cfg` files and imports supported numeric/toggle values into the current Receiver setup. Shows conversion notes. Connections and model paths stay as selected. Save the result as a Receiver JSON profile. |
| Frame collection and auto labels | **Data capture**. Opt-in, interval-limited writes to the selected dataset directory. Lossless RGB PPM files go in `images/`; optional normalized YOLO text labels go in `labels/`. Uses received frames, including simulated frames in practice. Can run while disarmed. Review generated labels and class IDs before training. |

Auto trigger requires the Pi UDP connection with current UPT3 telemetry, valid USB
endpoint timing and the existing UPC1 v2 click protocol. It does not send local
Windows clicks. The old 32-byte click extension is incompatible; use the current
proxy protocol described in [PROTOCOL.md](PROTOCOL.md). A physically held or
persistently injected trigger button prevents another scheduled click. The press
time is raised to the USB endpoint polling interval when needed.

Arming, fresh capture, synchronized clocks, fresh ready telemetry and current
inference results remain required for automatic output. Target loss, stale
results, deactivation and configuration transitions cancel outstanding automatic
clicks through the existing release fence. No real hardware was needed for the
automated checks; USB behavior still needs validation on the user's devices.

Collection writes on the inference worker and can reduce throughput. Start with
the default one-second interval. Output failures appear in Data capture and do not
terminate the session. Images and labels use unique session/frame names; generated
datasets are ignored by Git. These labels contain model predictions, not ground
truth. The class IDs follow the currently selected model.

## Features already present

Receiver already supports primary and alternate mouse activation, emergency stop,
confidence and class selection, input sizing and FPS limits, custom crosshair
coordinates, pixel and percentage offsets, sticky targeting, dynamic FOV, three
prediction filters, EMA smoothing, jitter, five movement paths, per-model settings,
model hot swaps, profile libraries and a performance helper. These remain available
through Activation, Targeting, Movement, Model, Profiles and Diagnostics.

Aimmy's third-person alignment is covered by Receiver's custom crosshair coordinates
and box aim-position controls in Targeting. Aimmy's FOV sizes are diameters; config
import halves them to produce Receiver radii. Trigger delays and prediction lead
times convert from seconds to milliseconds. Sensitivity stays unchanged because
the two apps use different movement scales; the importer explicitly reports this.
Unsupported fields are not silently treated as implemented controls.

## Architecture-specific differences

Receiver receives pictures from a separate sender and sends mouse commands to a Pi
or its local test backend. Aimmy's DirectX/BitBlt capture switching, desktop-wide
ESP/FOV windows, driver-specific mouse libraries, capture masking, remote keyboard
bindings and online model marketplace are not included. Preview overlays describe
the received picture and do not overlay the other computer's desktop. Capture
exclusion applies only to Receiver's own window.

## Validation

`receiver_assist` checks activation, dwell/repeat timing, one-shot behavior during
pending output, single-shot latching through temporary input loss, stale dwell resets, legacy JSON defaults, invalid settings,
Aimmy import, RGB conversion and normalized labels. `receiver_assist_pipeline`
uses the production App with independent loopback sender, Pi and worker fixtures
to verify disarmed collection, single trigger, spray, held/continuous activation
and sender-loss behavior, plus cancellation of a long press on telemetry loss.
GUI smoke checks render all eleven categories and test keyword routing at the
normal or minimum window size:

```powershell
ctest --test-dir build/ci -C Release --output-on-failure
python tests/gui_smoke.py build/ci/Release/receiver.exe --all-pages --compact --search-checks
```

Validated on 2026-10-18 with a Windows Clang 23 Release build, no TensorRT:
all 18 CTest checks and the installed-package
smoke test passed. The package checks run the new unit and loopback tests and
the existing model reload/fault scenarios from a separate working directory.
Always-on-top, keyboard rebinding and capture exclusion are native Windows paths;
their interactive behavior and real Pi/USB output were not exercised by the
hidden-framebuffer checks.

The settings reorganization adds eleven dedicated categories and seven search cases.
The sidebar keeps every category visible at the minimum window size. JSON profile
keys and the processing behavior are unchanged; older page launch names remain aliases.
