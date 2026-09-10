# Validation record — 2026-09-08

## Interface update — 2026-09-10

Humanization additions: the new controller/protocol tests pass for all five movement styles, directional symmetry and mode reversal, custom left/right ratios, stationary/slow-input blending, counter wrap, baseline resets, malformed UPT2, missing motion, movement caps, zero-strength output, dead-zone behavior, and old/new profile loading. The real headless receiver passed additional loopback tests: toward/away mean corrections were approximately 6.4/19.2 counts with a 50% effect, reversed by the mode switch; stationary output was approximately 12.8. These are deterministic synthetic-input output checks, not GPU latency measurements. Missing/stale motion telemetry, a generation restart, and physical release behaved as expected. The existing core and pipeline integration tests still pass. The Humanization page also passes the framebuffer smoke check and was visually reviewed.

The Pi codec tests pass locally for unchanged UPT1 and new UPT2 framing. Both the complete telemetry patch and the upgrade from the earlier button-only patch are checked for clean application. The Linux UDP/USB test for excluding injected motion from physical counters is implemented but still requires Linux/Pi execution; no physical Pi deployment has been performed.

The Windows GUI was rebuilt locally with guided Setup, Detection, Mouse, and Saved settings pages. Each page passed the existing framebuffer smoke check with live synthetic pictures, synchronized timing, a simulated Pi, and no commands sent while disarmed. Rendered screenshots were reviewed for readable labels and layout. A new demo launcher starts both local test peers and opens practice mode with the preview enabled. Native Windows model/settings file dialogs are included; exhaustive manual interaction and deployment GPU validation remain outstanding.

## Implemented and locally verified

Development host: Windows, Intel Core i7-1165G7, NVIDIA GeForce MX450, NVIDIA driver 592.82. This is not the planned RTX 3060 Ti deployment system.

- Native Windows test build: C++20 receiver core, headless executable, and Dear ImGui/DirectX 11 GUI compile and link with LLVM-MinGW 20260826 (Clang 23.1.0), CMake 4.4.3, Dear ImGui 1.92.1, and nlohmann/json 3.12.0.
- C++ core tests pass: binary framing, malformed dimensions/lengths, duplicates and conflicting fragments, reversed fragment order, assembly expiry/eviction, sender session changes, held-buffer lifetime/pool exhaustion, 10,000 malformed fuzz datagrams, clock offset bounds, telemetry token/sequence checks, wraparound, NMS/class filtering, letterbox reversal, selection/persistence, movement clamping, fractional accumulation, smoothing, and profile validation.
- Actual C++ receiver loopback integration passes using Python UDP peers: physical activation/release, capture stalls, telemetry loss, old capture timestamps, positive/negative sender clock offsets, sender restart, RGB/BGRA, Pi-not-ready handling, and default-disarmed startup. No real USB/mouse input is generated during these tests.
- GUI smoke test passes using the application's own hidden DirectX framebuffer: live preview, clock synchronization, telemetry, rendering, and disarmed startup. A rendered image is included separately in the delivery. This is a render/startup check, not exhaustive manual interaction testing.
- Pi telemetry wire codec compiles/runs on Windows. The proxy's Linux integration test has been extended to exercise subscriptions, physical-button updates, and expiry.
- Sample `yolo11n.onnx` was exported from official `yolo11n.pt` with Ultralytics 8.3.199 / PyTorch 2.8.0 CPU, ONNX opset 17, dynamic dimensions, raw detection output, and a SHA-256 manifest. ONNX checker passes.
- ONNX Runtime 1.22.1 CPU executes the actual sample at input sizes 160 and 320. C++ decoding matches independent torchvision NMS and coordinate mapping on 13 detections across a real-image fixture, two capture/input combinations, and class filtering.
- TensorRT host code passes C++ syntax checking against official TensorRT 10.13 and CUDA 12.9 headers. This caught and resolved ownership of TensorRT optimization profiles. This check does not link the TensorRT backend.
- The production CUDA preprocessing kernel compiles with NVIDIA NVRTC 12.9 for compute_86 and compute_75. It executes on the local MX450 through the CUDA driver API and matches the CPU reference across RGB/BGRA, square/non-square frames, odd dimensions, and a 1×1 edge case. Maximum absolute tensor error observed: approximately **1.2e-7**.

## Still required on deployment hardware

- A complete MSVC + CUDA 12.9 + TensorRT 10.13 production build, including the NVCC host launch wrapper and SDK linking. MSVC and a full CUDA toolkit are not installed on this development host. The portable test executables do not contain TensorRT.
- TensorRT engine build, warmup, FP16 inference, numerical comparison, GPU memory measurement, and latency measurements on the RTX 3060 Ti.
- Linux `make test` for the modified Pi proxy. WSL/Linux is not installed on this development host. The existing raw-gadget/libusb implementation cannot be executed as a Windows application.
- Physical mouse/USB enumeration, Pi report merging and telemetry behavior under actual USB backpressure, button release, and endpoint disconnect/reconnect.
- LAN performance at 160/320 captures and 120/240 FPS, preview overhead, and true capture-to-USB timing.

## Delivery boundaries

Real target-PC screen capture is not implemented; a documented protocol and synthetic/raw-file sender are included. No code has been deployed to the Pi and no system CUDA/Visual Studio installation has been performed. The Pi changes are based on commit `c3c09153feabe4be2eb3a711fe3c3e957f6eb794` and are supplied as `integrations/pi/telemetry.patch` for the separate proxy repository.

All zero-millisecond GPU entries in simulation CSV files mean the inference backend was bypassed. They are not performance claims. The lack of UPX1 success ACK means no software-only receiver test can establish USB delivery latency.

## 2026-09-10: additional controls, local mouse and Test Hub

- Release Windows build without TensorRT succeeded; all three CTest suites passed (including 76,864 core checks).
- All seven default Test Hub entries passed through the same background process runner used by the GUI. The six GUI pages rendered their own hidden framebuffer; the Test Hub screenshot was visually reviewed.
- Prediction methods, prediction resets/limits, sticky distance, dynamic FOV, EMA response, local injected-event exclusion, and profile round-tripping passed deterministic checks.
- The actual Windows mouse reader started successfully without sending movement. A separate local-backend application session received synthetic frames without a Pi, remained disarmed, and recorded zero submitted corrections.
- Local cursor injection/feel was deliberately left for explicit manual activation. RTX 3060 Ti TensorRT performance, physical Pi/USB operation, and deployment behavior remain unvalidated by these checks. Optional model/CUDA entries do not count as passing unless their prerequisites are supplied and the checks actually run.

### Steady demo and local pacing update

The demo now defaults to stationary simulated motion; four-second direction cycling requires --cycle-motion. A crosshair/bullseye image matches the fixed detection. Movement status and submitted-correction count are visible in practice mode. The local worker now waits on a high-resolution timer or pending Windows mouse messages instead of sleeping between polls. All seven Test Hub checks passed. A 200-sample read-only wait/poll measurement on this PC was p50 1.671 ms, p95 1.865 ms, maximum 2.215 ms. This measures reader cadence only; no actual cursor movement or complete pipeline continuity was measured. Stale-frame gating remains unchanged.

### Event-driven control optimization

Replaced Windows control-thread timed polling with notifications for new detection results, telemetry, and local mouse messages. Added inference_to_control timing to CSV and GUI. All seven Test Hub checks passed after this change. Final 120-setting loopback receiver-to-submission p95 was 2.5916 ms versus 14.5460 ms before; handoff p95 was 0.0431 ms. See BENCHMARK.md for sample counts, throughput limits and scope. No deployment-GPU or actual mouse-delivery claim follows from these simulated measurements.
