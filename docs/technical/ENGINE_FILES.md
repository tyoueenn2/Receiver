# Load a TensorRT engine directly

In a **TensorRT-enabled build**, choose a `.engine` file and leave **Detection
backend** on **Auto**. The receiver loads the selected engine directly, skipping
ONNX parsing and engine building. This works for compatible YOLO11 and YOLO-Omni
raw-detection engines. `.pt` continues to use PyTorch; `.onnx` builds or reuses a
TensorRT engine.

The portable Windows package built without TensorRT cannot execute `.engine` or
`.onnx` files. Build with `RECEIVER_TENSORRT=ON`, using the existing
`windows-cuda` or `linux-cuda` preset and TensorRT 10.x/CUDA dependencies described
in the README. TensorRT, its plugins and CUDA are not bundled in the package.

## Class metadata

Ultralytics `.engine` exports contain a length-prefixed JSON header with the
model task and class names. The receiver reads this header and passes only the
serialized plan to TensorRT. No companion file is needed. The model's class
names appear after loading.

Raw TensorRT plans, including `trtexec` outputs, need `<model>.engine.json`, or an
explicit **Model metadata file** in Advanced settings. Prepare that file from
the receiver manifest for the ONNX model used to build the engine:

```powershell
python tools/prepare_engine.py --engine models/custom.engine --manifest models/custom.onnx.json
```

This records the actual engine SHA-256, detection layout and class names. Use
the manifest for the matching source model. Do not rename an ONNX manifest to
an engine manifest: its original hash is for a different file. An explicitly
selected companion file overrides embedded metadata and must describe the same
engine. Newly built receiver cache engines now include their companion metadata
file automatically. Older cache engines can use the preparation tool.

## Supported engines

- One batch-one RGB NCHW image input: `[1,3,size,size]`.
- One raw detection output: `[1,4+classes,candidates]`.
- Linear, unvectorized device tensors with FP32 or FP16 input/output. FP16
  conversion runs on the GPU; the existing decoder receives FP32 predictions.
  The engine's internal precision and tactics are preserved.
- The selected processing size must match a static engine's dimensions or fit
  its first optimization profile. Input/output shapes are checked after selection.
- Class metadata must describe a detection task and agree with the output
  channel count. A supplied hash must match the full engine file.

Embedded NMS, transposed outputs, multiple outputs, pose, segmentation, OBB,
classification and INT8 I/O are rejected. An engine with INT8 internal layers
can still use supported FP32/FP16 I/O. Missing plugins or an incompatible plan
produce the TensorRT deserialization error; the receiver does not change runtime
or rebuild a selected engine automatically.

Use an engine compatible with the target operating system, TensorRT runtime and
GPU, or rebuild it there. See NVIDIA's
[engine compatibility documentation](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/engine-compatibility.html).

## Validation

Auto/manual selection, the Ultralytics header format, raw plan handling, class
metadata, wrong hashes, NMS/pose rejection, fixed-size mismatches, transposed
outputs and memory bounds are covered by the backend tests. All eight CTest
suites pass. The TensorRT loader passes syntax checking against TensorRT 10.13
and CUDA 12.9 headers. The actual FP32/FP16 conversion kernels were compiled
with NVRTC and executed on the local CUDA GPU: 257 values matched the IEEE half
reference, including rounding ties, subnormals, signed zero and nonfinite values.

A complete linked TensorRT build and engine inference have not been tested on
this host. Validate your actual engine on the deployment PC with the preview or:

```powershell
.\receiver_verify.exe profiles/custom-engine.json sample.rgb 320 320 engine-result.json
```

Use the model's actual processing size and a profile pointing to your engine.
Compare representative captures before measuring latency. Conversion tests do
not establish end-to-end model accuracy or RTX 3060 Ti performance.
