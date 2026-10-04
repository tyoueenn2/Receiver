# Native YOLO-Omni models

Choose a trained `.pt` model in Setup and leave **Detection backend** on **Auto**.
Auto routes `.pt` files to the persistent PyTorch worker and `.onnx`/`.engine` files to
TensorRT. See [direct engine loading](ENGINE_FILES.md) for serialized engines.
The worker identifies YOLO-Omni from the model's actual modules, so
renaming the checkpoint does not affect detection. Compatible ordinary YOLO
detection checkpoints can use the same PyTorch worker. Existing profiles default
to Auto when the new setting is absent.

## Install the model runtime

Use a separate Python environment with a CUDA-enabled PyTorch build and a
checkout of the [YOLO-Omni fork](https://github.com/z637826/yolo-omni).
Install PyTorch for your CUDA/driver combination
using the [official PyTorch selector](https://pytorch.org/get-started/locally/).
Then install the fork's remaining dependencies with that environment's Python:
`python -m pip install -r tools/requirements-omni.txt`. Set **YOLO-Omni source folder**
to the checkout root; this selects its custom modules ahead of stock Ultralytics.
The receiver does not install packages or download checkpoints automatically.
Native inference uses FP32, preserving compatibility with the fork's custom
modules. TensorRT exports use the existing FP16 path.

In **Advanced connection settings**, set:

- **Python executable**: the environment's actual interpreter, such as
  `C:/Models/omni-env/Scripts/python.exe` on Windows or
  `/home/user/omni-env/bin/python` on Linux. This is one executable path, not a
  shell command; use the interpreter directly instead of `py -3.12`.
- **YOLO-Omni source folder**: the repository root containing `ultralytics/`.
  Leave empty if the fork is already installed in that environment. Stock
  Ultralytics does not contain the fork's custom modules.
- **PyTorch device**: NVIDIA GPU by default. CPU must be selected explicitly for
  testing; there is no automatic CPU fallback.

The included `profiles/yolo-omni.json` is an example. Supply your own trained
weights and update its interpreter path and device addresses. No Omni checkpoint
is bundled. Both GUI and headless builds support the Python backend even when
compiled without TensorRT.

For headless operation, from the receiver directory:

```powershell
.\receiver_headless.exe --profile profiles/yolo-omni.json --model C:/Models/custom.pt --omni-python C:/Models/omni-env/Scripts/python.exe --omni-source C:/Models/yolo-omni
```

Linux uses the same options with its executable and interpreter paths. Use
`--backend yolo_omni` or `--backend tensorrt` to override Auto. A conflicting file
format is rejected with an explanation. The resolved runtime appears in the GUI
and headless status. Class names are read from the native checkpoint after loading;
`.pt` models do not need an ONNX manifest. Stop before changing models, runtimes,
source folders or devices. `omni_worker` defaults to `tools/yolo_omni_worker.py`
relative to the working directory and can be set to an absolute path in a profile.

## Contract and timing

The child is launched once per session without a shell or visible console. It
connects to an authenticated ephemeral loopback TCP socket. Each request carries
one packed RGB24/BGRA frame; each response returns float32 raw
`[1,4+classes,candidates]` predictions. The existing receiver continues to apply
its current confidence, class filters, NMS, letterbox reversal, freshness checks
and mouse controls. There is no extra queue of inference requests.

The worker uses normalized RGB, half-pixel bilinear resize and 114-valued
letterbox padding. It validates task, device, warmup output and every frame.
Pose, segmentation, OBB, classification, state-dict-only and embedded-NMS models
are not supported. Loading is bounded to 120 seconds; a frame exchange to 10
seconds. Stop interrupts worker waits and reaps the process. Dependency, model
and protocol failures fail the session; they never silently change backends.
Logs are written to `cache/omni-worker-*.log` and errors include the log path.

For PyTorch, **Inference** measures synchronized model execution. **Copies/preprocess**
also includes preprocessing, local transport, output transfer and worker overhead.
These numbers are not the CUDA-event measurements used by TensorRT. Compare
receiver-to-submission p95/p99 as well as detection quality on representative
captures. CPU mode is for functional checks and may exceed freshness limits.

For single-frame verification, `receiver_verify` is now built with either backend:

```powershell
.\receiver_verify.exe profiles/yolo-omni.json sample.rgb 320 320 omni-result.json
```

## Optional ONNX export

`tools/export_model.py --weights custom.pt --omni-source C:/Models/yolo-omni --size 320`
uses the fork and writes an architecture-aware companion manifest. Compatible
Omni ONNX graphs can run through TensorRT with Auto. Custom operators still need
to be supported by the installed ONNX exporter and TensorRT parser. Export errors
are reported; selecting native `.pt` inference avoids that export requirement.
TensorRT verifies the manifest hash and raw I/O layout as it does for YOLO11.

## Local validation

The automated checks cover Auto/manual selection, legacy profile loading,
settings round trips, class inspection, protocol bounds, repeated RGB/BGRA
frames, detection coordinate decoding, worker startup failures, invalid output,
invalid class selection, and cancellation. The process tests use a deterministic
worker fixture. A separate actual-model CPU smoke test also passed using an
untrained Game2Real nano checkpoint from upstream commit
`0b8610dbfc4d5eb3496ac8037002ea3c81a24fbd`: its `[1,84,2100]` output matched direct
PyTorch inference, and RGB/BGRA preprocessing matched an independent bilinear
reference. This does not establish trained-model accuracy or RTX 3060 Ti
performance. Validate your checkpoint and deployment Python environment using
the preview or single-frame verifier. To repeat the optional check in an
environment with the dependencies installed:

```powershell
python tests/omni_native_test.py --verify-exe build/omni/receiver_verify.exe --source C:/Models/yolo-omni
```

Add `--weights custom.pt` to check an existing Omni checkpoint instead of making
an untrained fixture.
