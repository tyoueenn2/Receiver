# Documentation

Start with [Getting started](GETTING_STARTED.md) to try the Windows interface and
find the instructions for your model or device setup. Run commands in the guides
from the repository root or extracted app folder unless stated otherwise.

Detailed technical documents live in `technical/`. Third-party license texts live
in `licenses/`.

## Setup and operation

| Guide | Contents |
| --- | --- |
| [Connect devices](technical/SETUP.md) | Sender, network and Raspberry Pi setup |
| [Headless operation](technical/HEADLESS.md) | Command-line use, simulation and logging |
| [Model workflows](technical/MODEL_WORKFLOWS.md) | Hot swaps, automatic sizing, saved settings and libraries |
| [YOLO-Omni](technical/YOLO_OMNI.md) | Python runtime, native checkpoints and worker behavior |
| [TensorRT engines](technical/ENGINE_FILES.md) | Engine loading, metadata and precision |
| [Movement and tracking](technical/HUMANIZATION.md) | Humanization, prediction and local mouse controls |

## Build, test and performance

| Guide | Contents |
| --- | --- |
| [Build Receiver](technical/BUILD.md) | Windows/Linux builds with or without TensorRT |
| [Export and validate models](technical/MODEL_EXPORT.md) | ONNX export, supported formats and reference comparisons |
| [CI and packages](technical/CI.md) | Checks without hardware, artifacts and release gates |
| [Test Hub](technical/TEST_HUB.md) | Run checks from the desktop interface |
| [Performance and troubleshooting](technical/PERFORMANCE.md) | Metrics, buffering, retries and failure handling |
| [Benchmark procedure](technical/BENCHMARK.md) | Repeatable measurements and recorded results |
| [Validation record](technical/VALIDATION.md) | Completed checks and remaining hardware validation |

## Reference and notices

| Document | Contents |
| --- | --- |
| [Network protocols](technical/PROTOCOL.md) | Frame, telemetry and mouse-command wire formats |
| [Sender contract](technical/SENDER.md) | Requirements for a capture/sender implementation |
| [Pi compatibility](technical/PI_COMPATIBILITY.md) | Supported proxy protocols and integration boundaries |
| [Third-party notices](technical/THIRD_PARTY.md) | Source attribution and dependency licenses |
