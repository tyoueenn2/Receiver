#pragma once
#include "protocol.hpp"
#include <optional>
#include <string>
#include <vector>
#include <random>
namespace receiver {
struct HumanizationSettings {
    bool enabled = false;
    int path = 0;
    float curve = .2f, duration_ms = 150, exponent = 2, adaptive_distance = 100;
    float jitter = 0, jitter_interval_ms = 30, noise = 1, noise_period_ms = 100;
    bool ema_enabled = false;
    float ema_alpha = .5f;
};
struct TrackingSettings {
    bool prediction = false, dynamic_fov = false;
    int prediction_method = 0, fov_button = 1;
    float lead_ms = 100, lead_multiplier = 1, velocity_smoothing_ms = 40, max_lead = 100;
    float sticky_distance = 0, held_radius = 100;
};
class TargetPredictor {
    struct Axis {
        double position = 0, velocity = 0, p00 = 10, p01 = 0, p11 = 10000;
        void update(double observed, double dt);
    };
    Axis x_, y_;
    int64_t at_ = 0;
    double previous_x_ = 0, previous_y_ = 0, vx_ = 0, vy_ = 0;
    std::array<std::pair<double, double>, 5> history_{};
    size_t count_ = 0;

  public:
    void reset();
    std::pair<float, float> update(float x, float y, int64_t stamp, const TrackingSettings& settings);
};
struct DirectionSettings {
    bool enabled = false;
    int mode = 0; // 0: more help away, 1: more help toward, 2: left/right balance
    float strength = .5f, slow_strength = 1, slow_speed = 40, window_ms = 30;
    float left_strength = 1.5f, right_strength = .5f;
};
struct MotionEstimate {
    bool available = false;
    double x = 0, y = 0; // Physical HID counts per second; never injected movement.
};
class MotionTracker {
    Telemetry previous_{};
    MotionEstimate velocity_{};
    int64_t received_ = 0;
    bool have_ = false;

  public:
    void reset();
    void observe(const Telemetry& sample, int64_t now, float window_ms);
    MotionEstimate estimate(int64_t now) const;
};
double direction_multiplier(double error_x, double error_y, MotionEstimate motion,
                            const DirectionSettings& settings);
struct Detection {
    float x = 0, y = 0, w = 0, h = 0, score = 0;
    int cls = 0;
};
struct Settings {
    int version = 1;
    std::string bind_ip = "0.0.0.0", sender_ip = "127.0.0.1", pi_ip = "127.0.0.1", model = "", metadata = "";
    int frame_port = 5000, pi_port = 12345, input_size = 320;
    float confidence = .45f, nms_iou = .45f, fov_radius = 160, reference_x = -1, reference_y = -1;
    std::vector<int> classes;
    bool highest_confidence = false, persistence = false, preview = false;
    float persistence_iou = .2f, aim_x = .5f, aim_y = .5f, offset_x = 0, offset_y = 0;
    float gain_x = .2f, gain_y = .2f, smoothing_ms = 0, deadzone = 1;
    int max_step = 32, activation_button = 2, max_age_ms = 50;
    HumanizationSettings humanization;
    DirectionSettings direction;
    TrackingSettings tracking;
    int mouse_backend = 0; // 0: Pi UDP, 1: local Windows test mouse
    int secondary_button = 0;
};
void validate(const Settings& s);
Settings load_settings(const std::string& path);
void save_settings(const Settings& s, const std::string& path);
struct Letterbox {
    float scale = 1;
    int resized_w = 0, resized_h = 0, left = 0, top = 0, size = 0;
};
Letterbox letterbox(int width, int height, int input);
std::vector<Detection> decode_yolo(std::span<const float> output, int candidates, int classes, int width,
                                   int height, int input, const Settings& settings);
float iou(const Detection& a, const Detection& b);
struct Correction {
    int dx = 0, dy = 0;
    std::optional<Detection> target;
    float aim_x = 0, aim_y = 0;
    double strength = 0;
};
class Controller {
    std::optional<Detection> previous_;
    double residual_x_ = 0, residual_y_ = 0, smooth_x_ = 0, smooth_y_ = 0;
    int64_t last_ = 0;
    int64_t acquired_ = 0, jitter_at_ = 0, noise_at_ = 0;
    double jitter_x_ = 0, jitter_y_ = 0, noise_from_ = 0, noise_to_ = 0;
    std::mt19937 random_;
    TargetPredictor predictor_;

  public:
    explicit Controller(uint32_t seed = std::random_device{}()) : random_(seed) {}
    void reset();
    Correction update(const std::vector<Detection>& detections, int width, int height, const Settings& s,
                      const Telemetry& limits, int64_t now, MotionEstimate motion = {},
                      int64_t capture_ns = 0);
};
} // namespace receiver
