# Getting started

## Try the Windows interface

After building the app or unpacking a test build, install Python 3.12 or newer and double-click **Start Demo.cmd**. This opens the app with example pictures and a simulated mouse device already connected. Close the app to stop the demo helpers.

The sidebar groups settings by what you want to change. **Find a setting** accepts
words such as `prediction`, `mouse sensitivity`, `spray`, or `dataset` and opens
the matching category. Q/E moves between categories when you are not typing.

| Category | What belongs here |
| --- | --- |
| Connections | Practice/live mode, UDP/local video source, capture display, mouse output and network ports |
| Model | Model library, input resolution, inference FPS limit, runtime and performance benchmark |
| Activation | Held-button or continuous mode, activation buttons, cursor pause and maximum frame age |
| Targeting | Confidence, classes, class preference/type, target priority/lock, search radius, aim point and prediction |
| Movement | Sensitivity, smoothing, correction limits, movement path, jitter and directional strength |
| Auto click | Enable automatic clicks, once/repeat behavior, click button and timing |
| Preview | Enable the separate video window and adjust detection boxes, search-area circle, labels, target lines and overlay appearance |
| Data capture | Save images, output folder, save interval and optional YOLO labels |
| Profiles | Save/load Receiver profiles and import Aimmy configs |
| App preferences | Window behavior, theme color and keyboard shortcuts |
| Diagnostics | Setup checks and live connection/performance details |

Choose **Connections**, select a **Model** for live detection, and start the
session. Turn on **Preview → Enable preview window** to inspect the video before arming assistance. The
global **Arm assistance** control permits output; **Activation** determines when
that output runs. **Disarm** or **Delete** stops it. Accepted settings restore
automatically next launch, with assistance disarmed.

See [model switching, automatic sizes and preferences](technical/MODEL_WORKFLOWS.md)
for supported metadata, rollback behavior, library refresh and performance tuning.

In **Targeting > Class preference**, choose a preferred class and a preference
type. **Whenever available** prioritizes that class anywhere inside the search
area. **Near the normal target** first selects the normal target, then prefers a
nearby detection of the preferred class. For example, set **Target class** to
`player`, **Preferred class** to `head`, and adjust **Maximum association distance**.
If no head qualifies near that player, the player remains the target.
See [class preference details](technical/CLASS_PREFERENCE.md).

In practice mode, **Arm practice assistance** sends commands only to the
simulator; it never moves your actual mouse with the default simulated Pi output.
The simulated device holds the right button for you. Practice uses a fixed example
detection, not YOLO inference. Each category keeps advanced tuning beside the
setting it affects. Movement reset affects only Movement, preserving Targeting.

From a terminal, the same demo is `py tools/ui_demo.py`; use `--exe path/to/receiver.exe` for a custom build location.

## Local mouse and automated checks

For video from another PC, select the UDP picture source in **Connections**, enter
that PC's sender IP address, and start the session. Run
`frame_sender.exe --host RECEIVER_IPV4 --output 0` on the sending PC. FrameSender
automatically follows the loaded model's input size, including live model changes.
See [sender setup and compatibility](technical/SENDER.md).

For desktop capture on this PC, end the session and choose **Connections > Video
source > Picture source > Local desktop capture**, then select a display. Receiver
captures a centered square matching the current model input size. Keep the Receiver
window outside that area, choose your model, and start with assistance disarmed.
No sender program is needed. Mouse output remains a separate choice.
See [local capture](technical/LOCAL_CAPTURE.md) for requirements and performance details.

For manual cursor testing, end the session and choose **Connections > Mouse
output > This computer's Windows mouse**. Set buttons or continuous mode in
**Activation**, then start and arm the session. This output moves your real mouse.
See [movement and tracking details](technical/HUMANIZATION.md).

For a MAKCU device, end the session and choose **Connections > Mouse output > MAKCU (USB serial)**. Enter its control port, start the session, arm assistance, and hold an activation button on the mouse connected through MAKCU. See [MAKCU setup and tests](technical/MAKCU.md).

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
TensorRT build. The supplied sender produces test frames. Use local capture for
this Windows desktop, or a protocol-compatible sender for video from another PC.
