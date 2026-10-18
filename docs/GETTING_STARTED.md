# Getting started

## Try the Windows interface

After building the app or unpacking a test build, install Python 3.12 or newer and double-click **Start Demo.cmd**. This opens the app with example pictures and a simulated mouse device already connected. Close the app to stop the demo helpers.

The sidebar groups settings by what you want to change. **Find a setting** accepts
words such as `prediction`, `mouse sensitivity`, `spray`, or `dataset` and opens
the matching category. Q/E moves between categories when you are not typing.

| Category | What belongs here |
| --- | --- |
| Connections | Practice/live mode, mouse output device, sender/Pi addresses and network ports |
| Model | Model library, input resolution, inference FPS limit, runtime and performance benchmark |
| Activation | Held-button or continuous mode, activation buttons, cursor pause and maximum frame age |
| Targeting | Confidence, classes, target priority/lock, search radius, aim point and movement prediction |
| Movement | Sensitivity, smoothing, correction limits, movement path, jitter and directional strength |
| Auto click | Enable automatic clicks, once/repeat behavior, click button and timing |
| Preview | Live video, detection boxes, search-area circle, labels, target lines and overlay appearance |
| Data capture | Save images, output folder, save interval and optional YOLO labels |
| Profiles | Save/load Receiver profiles and import Aimmy configs |
| App preferences | Window behavior, theme color and keyboard shortcuts |
| Diagnostics | Setup checks and live connection/performance details |

Choose **Connections**, select a **Model** for live detection, and start the
session. Use **Preview** to inspect the video before arming assistance. The
global **Arm assistance** control permits output; **Activation** determines when
that output runs. **Disarm** or **Delete** stops it. Accepted settings restore
automatically next launch, with assistance disarmed.

See [model switching, automatic sizes and preferences](technical/MODEL_WORKFLOWS.md)
for supported metadata, rollback behavior, library refresh and performance tuning.

In practice mode, **Arm practice assistance** sends commands only to the
simulator; it never moves your actual mouse with the default simulated Pi output.
The simulated device holds the right button for you. Practice uses a fixed example
detection, not YOLO inference. Each category keeps advanced tuning beside the
setting it affects. Movement reset affects only Movement, preserving Targeting.

From a terminal, the same demo is `py tools/ui_demo.py`; use `--exe path/to/receiver.exe` for a custom build location.

## Local mouse and automated checks

For manual cursor testing, end the session and choose **Connections > Mouse
output > This computer's Windows mouse**. Set buttons or continuous mode in
**Activation**, then start and arm the session. This output moves your real mouse.
See [movement and tracking details](technical/HUMANIZATION.md).

Open **Diagnostics > Setup checks** to run individual or selected checks, inspect
results, and save logs automatically. The demo launcher fills in Python
automatically. **Live status and timing** contains telemetry and performance
reports. See [check requirements](technical/TEST_HUB.md).

## Use your own model or connect devices

- [Native YOLO-Omni runtime](technical/YOLO_OMNI.md)
- [Model switching and remembered settings](technical/MODEL_WORKFLOWS.md)
- [Connect the sender and Raspberry Pi](technical/SETUP.md)
- [Build Receiver, including TensorRT](technical/BUILD.md)
- [Run without the GUI](technical/HEADLESS.md)

The downloadable CI builds support practice mode and native `.pt` models after
installing the Python model runtime. ONNX and TensorRT engines require a
TensorRT build. The supplied sender produces test frames; real desktop capture
requires a sender that implements the documented protocol.
