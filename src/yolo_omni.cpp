#include "receiver/backend.hpp"
#include "python_worker.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace receiver {
namespace {
class OmniBackend final : public Backend {
    std::unique_ptr<PythonWorker> worker_;
    std::vector<float> output_;
    std::vector<std::string> names_;
    std::string description_;
    size_t free_memory_ = 0, total_memory_ = 0;
    int size_ = 0, classes_ = 0;

    void initialize(const Settings& s, const BackendProgress& progress, const BackendCancel& cancelled) {
        size_ = s.input_size;
        if (!std::filesystem::is_regular_file(s.model))
            throw std::runtime_error("YOLO-Omni weights not found: " + s.model);
        if (progress)
            progress("YOLO-Omni: starting Python runtime");
        const auto deadline = now_ns() + 120'000'000'000ll;
        worker_ = std::make_unique<PythonWorker>(s.omni_python, s.omni_worker, "YOLO-Omni", "omni",
                                                 deadline, cancelled);
        worker_->send_json({{"version", 1},
                            {"model", std::filesystem::absolute(s.model).string()},
                            {"source", s.omni_source.empty() ? "" : std::filesystem::absolute(s.omni_source).string()},
                            {"input_size", size_},
                            {"auto_size", s.auto_size},
                            {"device", s.omni_device}},
                           deadline);
        if (progress)
            progress("YOLO-Omni: loading weights and warming model");
        auto reply = worker_->receive_json(deadline);
        if (reply.value("status", "") != "ready" || reply.value("version", 0) != 1)
            throw worker_->error("unsupported worker protocol");
        size_ = reply.value("input_size", size_);
        if (size_ < 32 || size_ > 1024 || size_ % 32)
            throw worker_->error("invalid worker input size");
        names_ = reply.at("names").get<std::vector<std::string>>();
        classes_ = int(names_.size());
        if (classes_ < 1 || classes_ > 10000 ||
            std::any_of(names_.begin(), names_.end(), [](const auto& name) { return name.empty(); }))
            throw worker_->error("invalid class count");
        for (int c : s.classes)
            if (c >= classes_)
                throw worker_->error("selected class is absent from model");
        description_ = reply.at("description").get<std::string>();
        free_memory_ = reply.value("free_memory", size_t(0));
        total_memory_ = reply.value("total_memory", size_t(0));
    }

  public:
    OmniBackend(const Settings& s, const BackendProgress& progress, const BackendCancel& cancelled) {
        initialize(s, progress, cancelled);
    }
    std::string description() const override {
        return description_;
    }
    std::vector<std::string> class_names() const override {
        return names_;
    }
    int input_size() const override {
        return size_;
    }
    std::pair<size_t, size_t> device_memory() const override {
        return {free_memory_, total_memory_};
    }
    Inference run(const Frame& frame) override {
        const auto& h = frame.header;
        size_t expected = size_t(h.width) * h.height * (h.format == 1 ? 3 : 4);
        if (!h.width || !h.height || h.width > 1024 || h.height > 1024 || (h.format != 1 && h.format != 2) ||
            h.bytes != expected || expected > frame.pixels.size())
            throw worker_->error("invalid input frame");
        const int64_t begin = now_ns();
        const int64_t deadline = begin + 10'000'000'000;
        worker_->send_json({{"width", h.width}, {"height", h.height}, {"format", h.format}, {"bytes", h.bytes}},
                           deadline);
        worker_->transfer(const_cast<uint8_t*>(frame.pixels.data()), expected, true, deadline);
        auto reply = worker_->receive_json(deadline);
        int count = reply.at("candidates").get<int>();
        if (reply.value("status", "") != "result" || reply.at("classes") != classes_ || count < 1 ||
            count > 100000 || size_t(count) * (classes_ + 4) > 16 * 1024 * 1024)
            throw worker_->error("unsupported raw detection output shape");
        double inference = reply.at("inference_ms").get<double>();
        if (!std::isfinite(inference) || inference < 0)
            throw worker_->error("invalid inference timing");
        output_.resize(size_t(count) * (classes_ + 4));
        worker_->transfer(output_.data(), output_.size() * sizeof(float), false, deadline);
        static_assert(std::endian::native == std::endian::little, "Worker output is little-endian float32");
        double elapsed = double(now_ns() - begin) / 1e6;
        return {output_, count, classes_, std::max(0., elapsed - inference), inference};
    }
};
} // namespace
std::unique_ptr<Backend> make_omni_backend(const Settings& s, const BackendProgress& progress,
                                          const BackendCancel& cancelled) {
    return std::make_unique<OmniBackend>(s, progress, cancelled);
}
} // namespace receiver
