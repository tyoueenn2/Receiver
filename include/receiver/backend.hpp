#pragma once
#include "control.hpp"
#include <functional>
#include <memory>
namespace receiver {
struct Inference {
    std::span<const float> output;
    int candidates = 0, classes = 0;
    double upload_ms = 0, inference_ms = 0;
};
class Backend {
  public:
    virtual ~Backend() = default;
    virtual Inference run(const Frame& frame) = 0;
    virtual std::string description() const = 0;
    virtual std::vector<std::string> class_names() const {
        return {};
    }
    virtual int input_size() const {
        return 0;
    }
    virtual std::vector<int> supported_sizes() const {
        return {};
    }
    virtual std::pair<size_t, size_t> device_memory() const {
        return {};
    }
};
using BackendProgress = std::function<void(const std::string&)>;
using BackendCancel = std::function<bool()>;
using BackendFactory =
    std::function<std::unique_ptr<Backend>(const Settings&, const BackendProgress&, const BackendCancel&)>;
std::string select_backend(const Settings& settings);
std::unique_ptr<Backend> make_backend(const Settings& settings, const BackendProgress& progress = {},
                                      const BackendCancel& cancelled = {});
std::unique_ptr<Backend> make_tensorrt_backend(const Settings&, const BackendProgress&);
std::unique_ptr<Backend> make_omni_backend(const Settings&, const BackendProgress&, const BackendCancel&);
} // namespace receiver
