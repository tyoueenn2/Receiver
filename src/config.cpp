#include "receiver/control.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace receiver {
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HumanizationSettings, enabled, path, curve, duration_ms,
                                                exponent, adaptive_distance, jitter, jitter_interval_ms,
                                                noise, noise_period_ms, ema_enabled, ema_alpha)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TrackingSettings, prediction, dynamic_fov, prediction_method,
                                                fov_button, lead_ms, lead_multiplier, velocity_smoothing_ms,
                                                max_lead, sticky_distance, held_radius)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DirectionSettings, enabled, mode, strength, slow_strength,
                                                slow_speed, window_ms, left_strength, right_strength)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, version, bind_ip, sender_ip, pi_ip, model, metadata,
                                                frame_port, pi_port, input_size, confidence, nms_iou,
                                                fov_radius, reference_x, reference_y, classes,
                                                highest_confidence, persistence, preview, persistence_iou,
                                                aim_x, aim_y, offset_x, offset_y, gain_x, gain_y,
                                                smoothing_ms, deadzone, max_step, activation_button,
                                                max_age_ms, humanization, direction, tracking, mouse_backend,
                                                secondary_button)
void validate(const Settings& s) {
    auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (s.version != 1)
        throw std::runtime_error("Unsupported profile version");
    if (s.frame_port < 1 || s.frame_port > 65535 || s.pi_port < 1 || s.pi_port > 65535)
        throw std::runtime_error("Ports must be 1..65535");
    if (s.input_size < 32 || s.input_size > 1024 || s.input_size % 32)
        throw std::runtime_error("Model input must be 32..1024 in multiples of 32");
    if (!range(s.confidence, 0, 1) || !range(s.nms_iou, 0, 1) || !range(s.persistence_iou, .01f, 1) ||
        !range(s.fov_radius, 0, 2048) || !range(s.reference_x, -1, 1024) || !range(s.reference_y, -1, 1024) ||
        !range(s.aim_x, 0, 1) || !range(s.aim_y, 0, 1) || !range(s.offset_x, -1024, 1024) ||
        !range(s.offset_y, -1024, 1024) || !range(s.gain_x, 0, 10) || !range(s.gain_y, 0, 10) ||
        !range(s.smoothing_ms, 0, 1000) || !range(s.deadzone, 0, 1024) || s.max_step < 1 ||
        s.max_step > 32767 || s.activation_button < 1 || s.activation_button > 8 || s.max_age_ms < 1 ||
        s.max_age_ms > 250)
        throw std::runtime_error("Profile contains invalid control settings");
    for (int c : s.classes)
        if (c < 0 || c > 9999)
            throw std::runtime_error("Invalid class ID");
    const auto& h = s.humanization;
    const auto& d = s.direction;
    const auto& t = s.tracking;
    if (s.mouse_backend < 0 || s.mouse_backend > 1 || s.secondary_button < 0 || s.secondary_button > 8 ||
        t.prediction_method < 0 || t.prediction_method > 2 || t.fov_button < 1 || t.fov_button > 8 ||
        !range(t.lead_ms, 0, 300) || !range(t.lead_multiplier, 0, 5) ||
        !range(t.velocity_smoothing_ms, 1, 200) || !range(t.max_lead, 0, 500) ||
        !range(t.sticky_distance, 0, 500) || !range(t.held_radius, 1, 2048) || !range(h.ema_alpha, .01f, 1))
        throw std::runtime_error("Tracking or mouse-backend settings are outside their allowed range");
    if (h.path < 0 || h.path > 4 || !range(h.curve, 0, 1) || !range(h.duration_ms, 10, 1000) ||
        !range(h.exponent, 1, 5) || !range(h.adaptive_distance, 1, 1024) || !range(h.jitter, 0, 20) ||
        !range(h.jitter_interval_ms, 8, 250) || !range(h.noise, 0, 20) || !range(h.noise_period_ms, 10, 1000))
        throw std::runtime_error("Humanization settings are outside their allowed range");
    if (d.mode < 0 || d.mode > 2 || !range(d.strength, 0, 1) || !range(d.slow_strength, 0, 2) ||
        !range(d.slow_speed, 1, 2000) || !range(d.window_ms, 5, 100) || !range(d.left_strength, 0, 2) ||
        !range(d.right_strength, 0, 2))
        throw std::runtime_error("Direction assistance settings are outside their allowed range");
}
Settings load_settings(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open profile: " + path);
    json j;
    f >> j;
    auto s = j.get<Settings>();
    validate(s);
    return s;
}
void save_settings(const Settings& s, const std::string& path) {
    validate(s);
    auto temp = path + ".tmp";
    {
        std::ofstream f(temp);
        if (!f)
            throw std::runtime_error("Cannot write profile");
        f << json(s).dump(2) << '\n';
        f.flush();
        if (!f)
            throw std::runtime_error("Failed writing profile");
    }
    // The GUI never serializes its runtime armed flag.
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temp, path, error);
        if (error)
            throw std::runtime_error("Cannot replace profile: " + error.message());
    }
}
} // namespace receiver
