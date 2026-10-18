#pragma once
#include "control.hpp"
#include <filesystem>
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
                bool can_fire = true);
};
bool activation_held(uint8_t physical, const Settings& settings);
// Writes received images and optional normalized YOLO labels, never desktop screenshots.
void collect_sample(const Frame& frame, const std::vector<Detection>& detections,
                    const CollectionSettings& settings, const std::string& name);
struct AimmyImport {
    Settings settings;
    std::vector<std::string> notes;
};
AimmyImport import_aimmy(const std::filesystem::path& path, const Settings& base);
} // namespace receiver
