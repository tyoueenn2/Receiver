# Building Receiver

Build from a source checkout and run commands from the repository root.

## Without GPU dependencies

Install CMake 3.24+, Git and Python 3.12+. Windows also needs Visual Studio 2022
with the Desktop development with C++ workload and a Windows SDK. Linux needs a
C++20 compiler and Ninja. These presets support simulation and native `.pt`
models when their [Python model runtime](YOLO_OMNI.md) is installed.

### Windows

Run in an x64 Developer PowerShell:

```powershell
cmake --preset windows-tests -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build --preset windows-tests
ctest --preset windows-tests
.\build\tests\Release\receiver.exe
```

### Linux

```bash
cmake --preset linux-tests -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build --preset linux-tests
ctest --preset linux-tests
```

Integration checks use software peers and do not require a device. See
[headless operation](HEADLESS.md) to run the Linux app, and [CI coverage](CI.md)
for the full test matrix.

## With CUDA and TensorRT

### Windows

Use Windows 11 x64, Visual Studio 2022 with the Desktop development with C++ workload and a Windows SDK, CMake 3.24+, Git, **CUDA Toolkit 12.9**, and the **TensorRT 10.13 Windows x64 CUDA 12 SDK**. Use an NVIDIA driver supported by that CUDA version. This backend deliberately targets TensorRT **10.x**; TensorRT 11's precision/export API is different and is rejected at compile time.

Official SDK references: [TensorRT Windows installation](https://docs.nvidia.com/deeplearning/tensorrt/latest/installing-tensorrt/install-zip.html), [TensorRT compatibility matrix](https://docs.nvidia.com/deeplearning/tensorrt/latest/getting-started/support-matrix.html). Select the 10.13 release, rather than substituting the latest major version.

In an x64 Developer PowerShell, from this directory:

```powershell
$env:TENSORRT_ROOT = 'C:\SDKs\TensorRT-10.13.0.35'
$env:PATH = "$env:TENSORRT_ROOT\lib;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin;$env:PATH"
cmake --preset windows-cuda
cmake --build --preset windows-cuda
ctest --preset windows-cuda
.\build\cuda\Release\receiver.exe
```

Set `TENSORRT_ROOT` to your actual extracted SDK directory containing `include` and `lib`. CUDA kernels target SM 8.6, the RTX 3060 Ti architecture. CMake fetches pinned Dear ImGui 1.92.1 and nlohmann/json 3.12.0 sources; subsequent builds can run offline.

### Linux headless

Install a C++20 compiler, CMake 3.24+, Ninja, OpenSSL development headers, an NVIDIA driver, CUDA Toolkit, and TensorRT 10.x with its ONNX parser. TensorRT may be installed system-wide or extracted beneath a custom `TENSORRT_ROOT`. Then run:

```bash
export TENSORRT_ROOT=/path/to/TensorRT-10.x
cmake --preset linux-cuda
cmake --build --preset linux-cuda
ctest --preset linux-cuda
./build/linux-cuda/receiver_headless --profile profiles/yolo11n.json --arm --seconds 0
```

The default CUDA architecture is SM 8.6 for the planned RTX 3060 Ti. Override it at configure time for another supported NVIDIA GPU, for example `cmake --preset linux-cuda -DRECEIVER_CUDA_ARCHITECTURES=89`. The Linux executable uses the Raspberry Pi UDP backend; the DirectX GUI and local Windows mouse backend are not built.
