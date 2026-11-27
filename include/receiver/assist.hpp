#pragma once
#include "control.hpp"
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
namespace receiver {
// Called only for new, eligible inference results. The caller owns output gating.
class AutoTrigger {
    std::optional<Detection> previous_;
    int64_t acquired_ = 0, fired_at_ = 0, observed_at_ = 0;
    bool fired_ = false;

  public:
    void reset();
    void suspend();
    bool update(const Correction& correction, int width, int height, const Settings& settings, int64_t now,
                bool can_fire = true, const ColorMask* colors = nullptr);
};
bool activation_held(uint8_t physical, const Settings& settings);
// Writes received images and optional normalized YOLO labels, never desktop screenshots.
void collect_sample(const Frame& frame, const std::vector<Detection>& detections,
                    const CollectionSettings& settings, const std::string& name);
class SampleCollector {
    struct Sample {
        Frame frame;
        std::vector<Detection> labels;
        CollectionSettings settings;
        std::string name;
    };
    std::function<void(const std::string&)> completed_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Sample> sample_;
    bool busy_ = false, stopping_ = false;
    std::thread writer_;
    void run();

  public:
    explicit SampleCollector(std::function<void(const std::string&)> completed);
    ~SampleCollector();
    bool enqueue(const Frame&, const std::vector<Detection>&, const CollectionSettings&, std::string name);
};
struct AimmyImport {
    Settings settings;
    std::vector<std::string> notes;
};
AimmyImport import_aimmy(const std::filesystem::path& path, const Settings& base);
} // namespace receiver
