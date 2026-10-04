# Getting started

## Try the Windows interface

After building the app or unpacking a test build, install Python 3.12 or newer and double-click **Start Demo.cmd**. This opens the app with example pictures and a simulated mouse device already connected. Close the app to stop the demo helpers.

- **Setup** explains how to connect devices or try practice mode.
- **Models** switches models during a session, chooses image size automatically,
  remembers each model's settings and measures real-model processing speed.
- **Detection** controls which object is selected and where to point within its box.
- **Mouse** controls the named button to hold and output limits.
- **Humanization** contains sensitivity, smoothing, movement styles, jitter, and configurable direction-based strength. See [control details](technical/HUMANIZATION.md).
- **Saved settings** searches, opens and exports profiles. Accepted settings
  restore automatically next launch, with movement off.

See [model switching, automatic sizes and preferences](technical/MODEL_WORKFLOWS.md)
for supported metadata, rollback behavior, library refresh and performance tuning.

Use **Show preview** to see the picture. **Enable practice movement** sends commands only to the simulator; it never moves your actual mouse. The simulated device holds the right button for you. **Turn movement off** or **Delete** disables movement. Practice uses a fixed example detection, not YOLO inference. Technical settings and performance numbers are in expandable sections.

From a terminal, the same demo is `py tools/ui_demo.py`; use `--exe path/to/receiver.exe` for a custom build location.

## Local mouse and automated checks

The Mouse page offers a **Local Windows mouse** connection for manual testing. It starts disarmed and requires a held physical activation button. Humanization includes three prediction methods, lead controls, sticky distance, dynamic FOV, and EMA response. See [Humanization](technical/HUMANIZATION.md).

Open **Test Hub** to run individual or selected checks, inspect results, and save logs automatically. The demo launcher fills in Python automatically. See [Test Hub requirements](technical/TEST_HUB.md).

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
