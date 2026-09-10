# Test Hub

Open **Test Hub** and press **Run selected checks**, **Run all checks**, or **Run** beside one check. Starting tests disarms and stops the current receiver session. The interface stays responsive; Cancel stops the active test and its child processes. Closing Receiver also cancels testing. Tests never enable the local cursor-output backend.

| Check | What it does | Requirements |
|---|---|---|
| Core checks | Frame validation/reassembly, timing, detection, profiles | Included executable |
| Humanization checks | Movement styles, direction ratios, caps, resets | Included executable |
| Tracking and input checks | Prediction, EMA, sticky distance, dynamic FOV, injected-input exclusion | Included executable |
| Local mouse startup | Opens the real Windows reader without sending movement | Interactive Windows desktop |
| Network pipeline | UDP sender and simulated Pi, stale input, restarts, release | Python |
| Direction pipeline | Direction ratios, missing telemetry, recovery | Python |
| Interface pages | Renders all six pages in a hidden app; checks startup is disarmed | Python, DirectX 11 |
| Model reference comparison | YOLO11n ONNX/reference detection comparison | Python ML packages, model, image |
| CUDA kernel compilation | Compiles production preprocessing source | Python, NVRTC DLL |
| CUDA preprocessing comparison | Compares GPU preprocessing with CPU reference | Python/NumPy, NVRTC, NVIDIA GPU |
| TensorRT reference comparison | Compares FP16 output with FP32 ONNX Runtime | GPU build, Python ML packages, saved profile, raw RGB image |
| Physical Pi / USB validation | Lists the external check to perform | Pi/Linux and physical USB hardware; unavailable from Windows |

The first seven checks are selected by default. Optional prerequisites are entered under **Test settings and optional GPU checks**. The demo launcher supplies its Python path; otherwise select a Python executable. See the main setup guide for model and CUDA dependencies. GPU profile model paths are relative to the package/repository root. The preprocessing execution check compiles compute_75 PTX, usable on the MX450 and RTX 3060 Ti with a compatible CUDA driver.

Each check shows Passed, Failed, Unavailable, Cancelled, or Not run. Unavailable is not a pass. Missing optional files are explained; runtime dependency errors appear in the test log. Each process has a three-minute time limit. Network tests use temporary loopback ports and isolated settings. Reports and individual logs are saved under `test-results/<run>/`, with the exact report location shown in the page. GUI test screenshots are rendered from the test app's own framebuffer, not captured from the desktop.

For command-line validation, `receiver_test_hub_runner.exe <python.exe>` runs the same first seven checks and fails if any does not pass. Automatic results do not establish real cursor delivery, Pi USB behavior, TensorRT detection quality, or deployment latency.
