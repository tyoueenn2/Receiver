#include "receiver/assist.hpp"
#include "receiver/preferences.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
namespace receiver {
bool activation_held(uint8_t physical, const Settings& s) {
    return s.constant_tracking || (physical & (1u << (s.activation_button - 1))) ||
           (s.secondary_button && (physical & (1u << (s.secondary_button - 1))));
}
void AutoTrigger::reset() {
    previous_.reset();
    acquired_ = fired_at_ = observed_at_ = 0;
    fired_ = false;
}
void AutoTrigger::suspend() {
    acquired_ = observed_at_ = 0;
}
bool AutoTrigger::update(const Correction& c, int width, int height, const Settings& s, int64_t now,
                         bool can_fire) {
    const float x = s.reference_x < 0 ? width * .5f : s.reference_x;
    const float y = s.reference_y < 0 ? height * .5f : s.reference_y;
    if (!s.trigger.enabled || !c.target || x < c.target->x || x > c.target->x + c.target->w ||
        y < c.target->y || y > c.target->y + c.target->h) {
        reset();
        return false;
    }
    const bool same =
        previous_ && previous_->cls == c.target->cls && iou(*previous_, *c.target) >= s.persistence_iou;
    if (!same) {
        reset();
        acquired_ = now;
    } else if (!observed_at_ || now < observed_at_ || now - observed_at_ > int64_t(s.max_age_ms) * 1'000'000)
        acquired_ = now;
    previous_ = c.target;
    observed_at_ = now;
    if (!can_fire || now - acquired_ < int64_t(s.trigger.delay_ms) * 1'000'000 ||
        (fired_ &&
         (!s.trigger.spray ||
          now - fired_at_ < int64_t(std::max(s.trigger.interval_ms, s.trigger.press_ms)) * 1'000'000)))
        return false;
    fired_ = true;
    fired_at_ = now;
    return true;
}
void collect_sample(const Frame& f, const std::vector<Detection>& detections, const CollectionSettings& s,
                    const std::string& name) {
    if (!s.enabled)
        return;
    const int channels = f.header.format == 1 ? 3 : 4;
    const auto pixels = size_t(f.header.width) * f.header.height;
    if (!pixels || pixels * channels > f.pixels.size())
        throw std::runtime_error("Invalid collection frame");
    const auto root = std::filesystem::path(s.directory);
    std::filesystem::create_directories(root / "images");
    // Binary PPM is lossless and readable by common labeling/training tools.
    std::ofstream image(root / "images" / (name + ".ppm"), std::ios::binary);
    image << "P6\n" << f.header.width << ' ' << f.header.height << "\n255\n";
    std::vector<char> rgb(pixels * 3);
    for (size_t i = 0; i < pixels; ++i) {
        rgb[i * 3] = char(f.pixels[i * channels + (channels == 4 ? 2 : 0)]);
        rgb[i * 3 + 1] = char(f.pixels[i * channels + 1]);
        rgb[i * 3 + 2] = char(f.pixels[i * channels + (channels == 4 ? 0 : 2)]);
    }
    image.write(rgb.data(), std::streamsize(rgb.size()));
    image.close();
    if (!image)
        throw std::runtime_error("Cannot save collected image");
    if (!s.auto_label)
        return;
    std::filesystem::create_directories(root / "labels");
    std::ofstream labels(root / "labels" / (name + ".txt"));
    labels << std::fixed << std::setprecision(6);
    for (const auto& d : detections) {
        if (d.cls < 0 || !std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.w) ||
            !std::isfinite(d.h))
            continue;
        float x1 = std::clamp(d.x, 0.f, float(f.header.width));
        float y1 = std::clamp(d.y, 0.f, float(f.header.height));
        float x2 = std::clamp(d.x + d.w, 0.f, float(f.header.width));
        float y2 = std::clamp(d.y + d.h, 0.f, float(f.header.height));
        if (x2 > x1 && y2 > y1)
            labels << d.cls << ' ' << (x1 + x2) / (2 * f.header.width) << ' '
                   << (y1 + y2) / (2 * f.header.height) << ' ' << (x2 - x1) / f.header.width << ' '
                   << (y2 - y1) / f.header.height << '\n';
    }
    labels.close();
    if (!labels)
        throw std::runtime_error("Cannot save collected labels");
}
AimmyImport import_aimmy(const std::filesystem::path& path, const Settings& base) {
    auto j = read_json_file(path);
    if (!j.is_object())
        throw std::runtime_error("Aimmy config must be a JSON object");
    AimmyImport result{base, {}};
    auto& s = result.settings;
    size_t count = 0;
    auto number = [&](const char* key, float& target, float lo, float hi, float scale = 1) {
        if (!j.contains(key))
            return;
        if (!j[key].is_number())
            throw std::runtime_error(std::string("Expected a number for ") + key);
        const auto value = j[key].get<float>() * scale;
        if (!std::isfinite(value) || value < lo || value > hi)
            throw std::runtime_error(std::string("Out of range: ") + key);
        target = value;
        ++count;
    };
    auto integer = [&](const char* key, int& target, float lo, float hi, float scale = 1) {
        float value = float(target);
        number(key, value, lo, hi, scale);
        target = int(std::round(value));
    };
    auto toggle = [&](const char* key, bool& target) {
        if (j.contains(key)) {
            target = j.at(key).get<bool>();
            ++count;
        }
    };
    number("FOV Size", s.fov_radius, 0, 2048, .5f);
    number("Dynamic FOV Size", s.tracking.held_radius, 1, 2048, .5f);
    number("AI Minimum Confidence", s.confidence, 0, 1, .01f);
    number("X Offset (Left/Right)", s.offset_x, -1024, 1024);
    number("Y Offset (Up/Down)", s.offset_y, -1024, 1024);
    number("X Offset (%)", s.aim_x, 0, 1, .01f);
    number("Y Offset (%)", s.aim_y, 0, 1, .01f);
    number("Sticky Aim Threshold", s.tracking.sticky_distance, 0, 500);
    number("Kalman Lead Time", s.tracking.lead_ms, 0, 300, 1000);
    number("EMA Smoothening", s.humanization.ema_alpha, .01f, 1);
    number("Mouse Jitter", s.humanization.jitter, 0, 20);
    integer("Auto Trigger Delay", s.trigger.delay_ms, 0, 5000, 1000);
    integer("AI FPS Limit", s.inference_fps, 0, 1000);
    number("Border Thickness", s.visual.thickness, 1, 6);
    number("Corner Radius", s.visual.corner_radius, 0, 30);
    number("Opacity", s.visual.opacity, .1f, 1);
    number("AI Confidence Font Size", s.visual.font_size, 10, 32);
    toggle("Constant AI Tracking", s.constant_tracking);
    toggle("Predictions", s.tracking.prediction);
    toggle("Sticky Aim", s.persistence);
    toggle("Dynamic FOV", s.tracking.dynamic_fov);
    toggle("Auto Trigger", s.trigger.enabled);
    toggle("Spray Mode", s.trigger.spray);
    toggle("FOV", s.visual.fov);
    toggle("Show Detected Player", s.visual.boxes);
    toggle("Show AI Confidence", s.visual.confidence);
    toggle("Show Tracers", s.visual.tracers);
    toggle("UI TopMost", s.ui.always_on_top);
    if (!count)
        throw std::runtime_error("No supported Aimmy settings found");
    if (s.humanization.jitter > 0)
        s.humanization.enabled = true;
    result.notes.push_back("Imported " + std::to_string(count) +
                           " settings. Save as a Receiver JSON profile.");
    result.notes.push_back(
        "Aimmy FOV diameters were converted to radii. Connections and model paths are preserved.");
    if (j.contains("Mouse Sensitivity (+/-)"))
        result.notes.push_back(
            "Sensitivity needs manual tuning: Aimmy and Receiver use different movement scales.");
    result.notes.push_back(
        "Aimmy drivers, screen capture, keyboard bindings and unsupported fields were not imported.");
    validate(s);
    return result;
}
} // namespace receiver
