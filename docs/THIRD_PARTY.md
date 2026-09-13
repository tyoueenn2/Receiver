# Third-party sources

- Dear ImGui 1.92.1: https://github.com/ocornut/imgui — MIT license, copied in `licenses/imgui.txt`.
- nlohmann/json 3.12.0: https://github.com/nlohmann/json — MIT license, copied in `licenses/json.txt`.
- LLVM-MinGW runtime DLLs in the portable test package: https://github.com/mstorsjo/llvm-mingw — bundled toolchain license in `licenses/llvm-mingw.txt`. Production MSVC builds use their own runtime deployment requirements.
- NVIDIA CUDA and TensorRT are separately installed SDK dependencies. They are not bundled in the test package.
- YOLO11n sample weights, ONNX export, and optional Python exporter use Ultralytics: https://github.com/ultralytics/ultralytics and https://github.com/ultralytics/assets. The included sample originates from assets release v8.3.0 and was exported with Ultralytics 8.3.199. Its upstream AGPL-3.0 license is included in `licenses/ultralytics.txt`.
- The separate public proxy project at https://github.com/tyoueenn2/usb-proxy-udp is authoritative for Pi, Raw Gadget, scheduler, writer, and watchdog behavior and retains its Apache-2.0 license. Receiver does not vendor or modify that source. The local fake proxy is an independent deterministic protocol fixture with no USB access.
- Aimmy and Aimmy-CUDA were inspected as behavioral references for settings and selection. Receiver code is independently implemented; their code, assets, and models are not bundled.

Model weights and third-party libraries retain their own licenses.
