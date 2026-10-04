# Checks without a device

Every push, pull request and manual workflow run executes `.github/workflows/receiver.yml`.
No job needs a Raspberry Pi, USB mouse, GPU or trained custom weights.

| Check | What it verifies |
| --- | --- |
| Linux GCC Release and Clang Debug | Native compilation, core tests, independent UDP peers, worker processes and model workflows on real Linux runners |
| Windows MSVC Release and Debug | The same checks plus rendering all seven DirectX GUI pages and starting the read-only mouse reader |
| Linux Clang ASan + UBSan | Memory errors, leaks and undefined behavior in the full native/process/network suite |
| Repeated workflows | 20 runs per native build and 10 sanitized runs of reload, rollback and settings tests |
| CPU model inference | A checkpoint generated from the pinned YOLO-Omni fork runs through the real worker; its output matches direct PyTorch inference |
| Preprocessing reference | RGB/BGRA, rectangular/odd images and 1-pixel dimensions match independent NumPy bilinear letterboxing |
| ONNX schema oracle | 19 models generated with the official ONNX schema verify fixed/dynamic image sizes, names and rejected formats |
| ONNX detection reference | The included YOLO11 sample runs in ONNX Runtime; native class filtering, NMS and coordinate mapping match torchvision |
| CUDA compilation | Both production kernels compile to PTX for compute_75 and compute_86 through NVIDIA NVRTC |
| TensorRT API check | Production host code compiles against pinned official TensorRT 10.13, ONNX parser and CUDA 12.9 headers |
| Extracted packages | Installed Windows ZIP/Linux tar.gz contents are checked, started disarmed and rerun worker fault tests outside the checkout |

The new reliability suite runs 10,000 seeded metadata mutations, randomized protocol
inputs, truncated/oversized metadata, blocked settings replacements, concurrent
profile reads/writes and library refresh cases. Timing ring snapshots also verify
wraparound, independent copies and a bounded stack footprint for Windows Debug.
Failed preference loads/saves must
preserve the previous in-memory settings and clean up incomplete temporary files.

The independent fault worker exercises partial TCP replies, slow loads, reordered
class names, failed replacement rollback, 12 same-path reloads, benchmark cancellation,
malformed handshakes, worker crashes, truncated inference data, bad dimensions and
timings, and shutdown during a stalled load. It verifies continued picture reception,
an unchanged Pi connection during reloads, cleared synthetic holds, disarmed output
and termination of every launched worker. All network peers use loopback, and the
scenario runner rejects local cursor injection and non-loopback settings.

## Results and packages

Each native job uploads JUnit results, CTest failure logs, model fault reports and
Windows GUI screenshots, including when a test fails. Sanitizer results are retained
separately. The CUDA job uploads compiled PTX. Full command output is also available
in the Actions job log.

Release configurations produce `Receiver-ubuntu-24.04` and `Receiver-windows-2022`
artifacts containing a tested archive and SHA-256 checksum. They include the worker
tools, profiles, documentation, sample model, licenses and Windows C++ runtime DLLs.
Python/model dependencies and CUDA/TensorRT runtimes remain separately installed.

Pushing a `v*` version tag publishes those packages to GitHub Releases only after
all native, sanitizer, CPU-model and CUDA/API jobs succeed. Ordinary branch pushes
and pull requests upload artifacts without creating a release. Release publishing
has write permission only in its gated job; other jobs use read-only repository
permissions. These packages are built with `RECEIVER_TENSORRT=OFF`.

## Run locally

Python 3.12+, CMake 3.24+ and a C++20 compiler are required. For Windows, use the
existing `windows-tests` preset; on Linux, use `linux-tests`:

```sh
cmake --preset linux-tests -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build --preset linux-tests
ctest --preset linux-tests --output-junit junit.xml
```

On Windows replace the preset name with `windows-tests`. Integration tests are
opt-in locally and always enabled in the native CI jobs. For Linux memory checks:

```sh
CC=clang CXX=clang++ cmake -S . -B build/sanitized \
  -DCMAKE_BUILD_TYPE=Debug -DRECEIVER_GUI=OFF \
  -DRECEIVER_SANITIZERS=ON -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build build/sanitized --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/sanitized --output-on-failure
```

The CPU-model and CUDA jobs show exact dependency versions and pinned upstream
commits in the workflow. Their dependencies are optional for the ordinary suite.

## What remains hardware-dependent

These checks establish functional behavior with independent software peers. They
do not prove physical USB delivery, real Pi scheduling/backpressure, desktop capture,
GPU driver compatibility, linked TensorRT/NVCC execution, model accuracy or deployment
latency. The generated Omni checkpoint is untrained, so its test proves inference
and transport consistency rather than detection quality. The read-only mouse test
may report a CTest skip when Windows has no interactive desktop; all other checks
must pass. See [deployment validation](VALIDATION.md) for the remaining device checks.
