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

### Linux desktop interface

Install the SDL2 and OpenGL development packages in addition to the command-line
build dependencies. On Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake ninja-build libsdl2-dev libgl-dev
cmake --preset linux-gui -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build --preset linux-gui
./build/linux-gui/receiver
```

The `linux-gui` preset enables `RECEIVER_GUI=ON` and builds `receiver` alongside
`receiver_headless`. The existing `linux-tests` preset stays headless. A manual
Linux configuration can also enable the interface with `-DRECEIVER_GUI=ON`.
For CUDA with the interface, use `cmake --preset linux-cuda -DRECEIVER_GUI=ON`.

Linux shares the Windows interface's settings pages and preview overlays, using
SDL2/OpenGL 3.0 for native windows, keyboard input and rendering. It provides an
in-app file picker for models, profiles and Python executables. It supports UDP
video, Pi output and MAKCU serial output. Local Windows mouse output, Desktop
Duplication capture, cursor-visibility checks and capture exclusion remain
Windows-only. Delete disarms while Receiver is focused on Linux; it is not a
global Linux shortcut.

Run `python3 tools/ui_demo.py` for a complete simulated session; use `--exe` to
choose a different build. The launcher shuts down its software peers when the
interface closes.

GUI integration tests need a display. Install `xvfb xauth` for headless testing;
CMake uses `xvfb-run -a` automatically when it is available. Then run
`ctest --preset linux-gui`. The Linux preview lifecycle test uses X11; rendering
can use Mesa software OpenGL (`LIBGL_ALWAYS_SOFTWARE=1`). Tests cover every
settings page, search, color controls, video uploads, preview resize/close/reopen,
and disarmed startup. Native Wayland execution is provided by SDL2 but is not
covered by the Xvfb tests.

## With CUDA and TensorRT

CUDA builds default to CMake's `all-major` setting. The installed toolkit
generates kernels for its supported major architectures and PTX for its highest
major architecture. Use the TensorRT support matrix to check your GPU and driver.
[TensorRT support matrix](https://docs.nvidia.com/deeplearning/tensorrt/10.x.x/getting-started/support-matrix.html)

For a smaller build that targets compute capability 8.6, which includes the RTX
3060 Ti, add `-DRECEIVER_CUDA_ARCHITECTURES=86` to the configure command. Set
`-DRECEIVER_CUDA_ARCHITECTURES=all-major` to select the portable build in an
existing build directory.

### Windows

Use Windows 11 x64, Visual Studio 2022 with the Desktop development with C++ workload and a Windows SDK, CMake 3.24+, Git, **CUDA Toolkit 12.9**, and the **TensorRT 10.13 Windows x64 CUDA 12 SDK**. Use an NVIDIA driver supported by that CUDA version. This backend deliberately targets TensorRT **10.x**; TensorRT 11's precision/export API is different and is rejected at compile time.

Follow the Windows SDK installation guide with the 10.13 release archive.
[TensorRT Windows installation](https://docs.nvidia.com/deeplearning/tensorrt/10.x.x/installing-tensorrt/install-zip.html)

In an x64 Developer PowerShell, from this directory:

```powershell
$env:TENSORRT_ROOT = 'C:\SDKs\TensorRT-10.13.0.35'
$env:PATH = "$env:TENSORRT_ROOT\lib;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin;$env:PATH"
cmake --preset windows-cuda
cmake --build --preset windows-cuda
ctest --preset windows-cuda
.\build\cuda\Release\receiver.exe
```

Set `TENSORRT_ROOT` to your actual extracted SDK directory containing `include` and `lib`. CMake fetches pinned Dear ImGui 1.92.1 and nlohmann/json 3.12.0 sources. Subsequent builds can run offline.

### Linux headless

Install a C++20 compiler, CMake 3.24+, Ninja, OpenSSL development headers, an NVIDIA driver, CUDA Toolkit, and TensorRT 10.x with its ONNX parser. TensorRT may be installed system-wide or extracted beneath a custom `TENSORRT_ROOT`. Then run:

```bash
export TENSORRT_ROOT=/path/to/TensorRT-10.x
cmake --preset linux-cuda
cmake --build --preset linux-cuda
ctest --preset linux-cuda
./build/linux-cuda/receiver_headless --profile profiles/yolo11n.json --arm --seconds 0
```

The Linux headless executable supports Raspberry Pi UDP and MAKCU output.
