# Model export and validation

Run commands from the repository root or extracted app folder unless noted.

The included `models/yolo11n.onnx` and its manifest use the standard COCO
detection model. They are a sample rather than weights trained for a particular
application.

The sample is already exported. For other YOLO11 detection weights, create an isolated Python environment:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools/requirements-export.txt
.\.venv\Scripts\python.exe tools/export_model.py --weights path/to/custom.pt --size 320
```

The exporter writes an ONNX file and `<model>.onnx.json` containing class names, the raw detection contract, and a SHA-256 digest. Default exports have dynamic spatial dimensions, allowing separate optimized engines at 160 and 320. `--fixed` exports require the GUI input size to match exactly. Batch size is always one; model input dimensions must be multiples of 32, from 32 to 1024. Capture sizes need not be multiples of 32.

The ONNX path validates float32 NCHW input and raw `[1, 4 + classes, candidates]` output. Internal TensorRT layers use FP16 where supported; its I/O remains float32. Direct `.engine` loading also supports FP16 I/O and requires embedded or companion class metadata; see [engine requirements](ENGINE_FILES.md). Segmentation, pose, classification, embedded-NMS and external-data ONNX are not supported. Cache keys include the model hash, input size, GPU model, runtime/driver versions, and precision mode. Cache files live under `cache/`; deleting a cache file forces a rebuild when loading ONNX.

For a GPU/reference comparison on a tightly packed RGB24 file:

```powershell
.\.venv\Scripts\python.exe tools/validate_gpu.py --verify-exe build/cuda/Release/receiver_verify.exe --profile profiles/yolo11n.json --raw sample.rgb --width 320 --height 320
```

This compares preprocessing plus inference against ONNX Runtime CPU FP32. Review representative images as well as numeric differences. Reduced input resolution and FP16 may affect detection accuracy. ONNX Runtime CUDA is a possible future backend; it is not a hidden CPU fallback in this application.
