#include "receiver/backend.hpp"
#include "receiver/model.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace receiver {
std::string select_backend(const Settings& s) {
    std::string extension = std::filesystem::path(s.model).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    auto selected = s.inference_backend;
    if (selected == "auto") {
        if (extension == ".onnx" || extension == ".engine")
            selected = "tensorrt";
        else if (extension == ".pt")
            selected = "yolo_omni";
        else
            throw std::runtime_error("Choose an .onnx export, .engine file, or trained .pt detection model");
    }
    if (selected != "tensorrt" && selected != "yolo_omni")
        throw std::runtime_error("Unknown detection backend: " + selected);
    if ((selected == "tensorrt" && extension != ".onnx" && extension != ".engine") ||
        (selected == "yolo_omni" && extension != ".pt"))
        throw std::runtime_error(
            "TensorRT requires .onnx or .engine; YOLO-Omni (PyTorch) requires .pt weights");
    return selected;
}
std::unique_ptr<Backend> make_backend(const Settings& original, const BackendProgress& progress,
                                      const BackendCancel& cancelled) {
    auto selected = select_backend(original);
    auto s = prepare_model_settings(original);
    if (selected == "yolo_omni")
        return make_omni_backend(s, progress, cancelled);
    return make_tensorrt_backend(s, progress);
}
} // namespace receiver
