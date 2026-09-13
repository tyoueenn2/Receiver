# Validation record — 2026-09-13

## Synthetic holds and scheduled clicks — 2026-09-13

Receiver was updated from public `main` HEAD `173b99f0e66391e79fd515a3ce2cdecdfe7cbfa1`. The deterministic synthetic-input suite covers the exact 16-byte UPX1, 40-byte UPC1 v2, 48-byte UPA1 v2, and 128-byte UPT3 layouts; strict physical/persistent/scheduled separation; persistent movement masks; zero-motion transitions; 75 ms heartbeats; manual/scoped ownership and generation invalidation; all centralized release reasons and three-snapshot batches; modulo-2^32 sequence wrap; prompt click submission; same-ID request/ACK loss recovery; ordered submission; duplicate/reordered ACK idempotency; every status; QueueFull backpressure; impossible/malformed ACK rejection; server-epoch session rotation; release barriers; queue bounds; and completion/timeout/rejection metrics.

The real C++ headless loopback uses an independent deterministic fake Pi/proxy. It checks UPS3/UPT3 negotiation, a zero-motion synthetic press, corrections retaining only Receiver's mask, independent physical/persistent/scheduled masks, a hold heartbeat, scheduled two-click acceptance and completion, explicit ReleaseAll, shutdown snapshots, and a single stable UDP source port across subscriptions, movement, clicks, retries, releases, telemetry, and ACKs. The report separately exposes commands submitted locally, accepted by the Pi fixture, and completed by its simulated writer. The fixture has no Raw Gadget or USB access and is not evidence of physical USB reliability.

Receiver does not vendor or patch the proxy implementation. The separate public proxy project is authoritative for Pi scheduling, Raw Gadget, USB-writer, and watchdog behavior. Receiver intentionally sends only UPC1/UPA1 version 2 and requires valid UPT3 endpoint timing before accepting a scheduled click. “Completed by USB writer” in deterministic testing means that a valid UPA1 `Completed` was received from the fake writer; it does not prove transport over a physical link or destination-application handling.

Final regression on 2026-09-13:

- The native Windows build completed for the core library, headless runner, GUI, Test Hub, decoder, and all test executables.
- CTest passed 4/4 suites: core, humanization, tracking, and injection.
- The executables reported 76,864 core checks and 208 synthetic-button/UPC1/UPA1-v2 checks; the humanization/motion-telemetry and tracking/input suites also passed.
- The command-line Test Hub passed all eight default checks. Its local mouse check started the real Windows reader and sent no output. The network pipeline passed sender stalls, stale telemetry, old capture rejection, restart/BGRA recovery, readiness failure, default-disarmed operation, synthetic hold/heartbeat/release, click completion, shutdown release, and stable-source-port assertions. Both direction modes passed missing/stale data, generation restart, and physical release. All six hidden GUI framebuffer pages passed preview, synchronization, telemetry, and disarmed-startup checks.
- `git diff --check` reported no whitespace errors; it emitted only the repository's existing LF-to-CRLF conversion notices.

The optional model-reference check was not run because no representative input-image fixture is present. CUDA compilation/execution was not run because the NVRTC toolkit files are unavailable, and the non-TensorRT build does not contain `receiver_verify.exe`. Physical Pi/Raw Gadget/USB/destination validation requires the separate hardware setup. Unavailable or unrun checks are not counted as passes.

## Interface update — 2026-09-10

Humanization additions: the new controller/protocol tests pass for all five movement styles, directional symmetry and mode reversal, custom left/right ratios, stationary/slow-input blending, counter wrap, baseline resets, malformed UPT2, missing motion, movement caps, zero-strength output, dead-zone behavior, and old/new profile loading. The real headless receiver passed additional loopback tests: toward/away mean corrections were approximately 6.4/19.2 counts with a 50% effect, reversed by the mode switch; stationary output was approximately 12.8. These are deterministic synthetic-input output checks, not GPU latency measurements. Missing/stale motion telemetry, a generation restart, and physical release behaved as expected. The existing core and pipeline integration tests still pass. The Humanization page also passes the framebuffer smoke check and was visually reviewed.

The Pi codec tests pass locally for unchanged UPT1, public 80-byte UPT2, legacy cumulative 88-byte UPT2, and cumulative UPT3 framing. Direction tracking uses only physical cumulative counters from legacy UPT2 or UPT3; no physical Pi deployment has been performed.

The Windows GUI was rebuilt locally with guided Setup, Detection, Mouse, and Saved settings pages. Each page passed the existing framebuffer smoke check with live synthetic pictures, synchronized timing, a simulated Pi, and no commands sent while disarmed. Rendered screenshots were reviewed for readable labels and layout. A new demo launcher starts both local test peers and opens practice mode with the preview enabled. Native Windows model/settings file dialogs are included; exhaustive manual interaction and deployment GPU validation remain outstanding.

## Implemented and locally verified

Development host: Windows, Intel Core i7-1165G7, NVIDIA GeForce MX450, NVIDIA driver 592.82. This is not the planned RTX 3060 Ti deployment system.

- Native Windows test build: C++20 receiver core, headless executable, and Dear ImGui/DirectX 11 GUI compile and link with LLVM-MinGW 20260826 (Clang 23.1.0), CMake 4.4.3, Dear ImGui 1.92.1, and nlohmann/json 3.12.0.
- C++ core and synthetic-input tests pass: binary framing, malformed dimensions/lengths, duplicates and conflicting fragments, reversed fragment order, assembly expiry/eviction, sender session changes, held-buffer lifetime/pool exhaustion, 10,000 malformed fuzz datagrams, clock offset bounds, telemetry token/sequence checks, wraparound, NMS/class filtering, letterbox reversal, selection/persistence, movement clamping, fractional accumulation, smoothing, profile validation, persistent holds/releases/heartbeats, and scheduled-click ACK faults.
- Actual C++ receiver loopback integration passes using Python UDP peers: physical activation/release, capture stalls, telemetry loss, old capture timestamps, positive/negative sender clock offsets, sender restart, RGB/BGRA, Pi-not-ready handling, default-disarmed startup, synthetic hold preservation/heartbeat/release, and scheduled-click status. No real USB/mouse input is generated during these tests.
- GUI smoke test passes using the application's own hidden DirectX framebuffer: live preview, clock synchronization, telemetry, rendering, and disarmed startup. A rendered image is included separately in the delivery. This is a render/startup check, not exhaustive manual interaction testing.
- Pi telemetry wire codec compiles/runs on Windows. The proxy's Linux integration test has been extended to exercise subscriptions, physical-button updates, and expiry.
- Sample `yolo11n.onnx` was exported from official `yolo11n.pt` with Ultralytics 8.3.199 / PyTorch 2.8.0 CPU, ONNX opset 17, dynamic dimensions, raw detection output, and a SHA-256 manifest. ONNX checker passes.
- ONNX Runtime 1.22.1 CPU executes the actual sample at input sizes 160 and 320. C++ decoding matches independent torchvision NMS and coordinate mapping on 13 detections across a real-image fixture, two capture/input combinations, and class filtering.
- TensorRT host code passes C++ syntax checking against official TensorRT 10.13 and CUDA 12.9 headers. This caught and resolved ownership of TensorRT optimization profiles. This check does not link the TensorRT backend.
- The production CUDA preprocessing kernel compiles with NVIDIA NVRTC 12.9 for compute_86 and compute_75. It executes on the local MX450 through the CUDA driver API and matches the CPU reference across RGB/BGRA, square/non-square frames, odd dimensions, and a 1×1 edge case. Maximum absolute tensor error observed: approximately **1.2e-7**.

## Still required on deployment hardware

- A complete MSVC + CUDA 12.9 + TensorRT 10.13 production build, including the NVCC host launch wrapper and SDK linking. MSVC and a full CUDA toolkit are not installed on this development host. The portable test executables do not contain TensorRT.
- TensorRT engine build, warmup, FP16 inference, numerical comparison, GPU memory measurement, and latency measurements on the RTX 3060 Ti.
- The authoritative public proxy's own Linux test suite. WSL/Linux is not installed on this development host, and Raw Gadget/libusb cannot be exercised as a Windows application.
- Physical mouse/USB enumeration, Pi report merging and telemetry behavior under actual USB backpressure, button release, and endpoint disconnect/reconnect.
- LAN performance at 160/320 captures and 120/240 FPS, preview overhead, and true capture-to-USB timing.

## Delivery boundaries

Real target-PC screen capture is not implemented; a documented protocol and synthetic/raw-file sender are included. No code has been deployed to the Pi and no system CUDA/Visual Studio installation has been performed. Receiver's fake Pi/proxy is only a byte-level and state-machine fixture; this repository does not supply an implementation patch for the authoritative proxy project.

All zero-millisecond GPU entries in simulation CSV files mean the inference backend was bypassed. They are not performance claims. The lack of UPX1 success ACK means no software-only receiver test can establish USB delivery latency. UPC1 accepted/completed status separates Pi scheduling from USB-writer submission but still cannot establish physical USB delivery.

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
