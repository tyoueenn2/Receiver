#include "receiver/backend.hpp"
#include <stdexcept>
namespace receiver {
std::unique_ptr<Backend> make_tensorrt_backend(const Settings&, const BackendProgress&) {
    throw std::runtime_error(
        "This build has no TensorRT backend. Use a TensorRT build for .onnx or .engine, or select a .pt model with the YOLO-Omni Python runtime installed.");
}
} // namespace receiver
