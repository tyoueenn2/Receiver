#include "receiver/control.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace receiver {
void MotionTracker::reset() {
    previous_ = {};
    velocity_ = {};
    received_ = 0;
    have_ = false;
}
void MotionTracker::observe(const Telemetry& t, int64_t now, float window_ms) {
    if (!t.ready || !t.has_motion) {
        reset();
        return;
    }
    if (!have_ || t.server != previous_.server || t.motion_generation != previous_.motion_generation ||
        now < received_ || now - received_ > 50'000'000 || t.sample_ns <= previous_.sample_ns ||
        t.sample_ns - previous_.sample_ns > 100'000'000) {
        velocity_ = {};
        previous_ = t;
        received_ = now;
        have_ = true;
        return;
    }
    auto difference = [](uint32_t a, uint32_t b) {
        uint32_t delta = a - b;
        return delta <= 0x7fffffffu ? int64_t(delta) : int64_t(delta) - 0x100000000ll;
    };
    double dt = double(t.sample_ns - previous_.sample_ns) / 1e9;
    double x = difference(t.total_x, previous_.total_x) / dt;
    double y = difference(t.total_y, previous_.total_y) / dt;
    // Discontinuities are not physical intent. Re-baseline without producing assistance.
    if (std::abs(x) > 10'000'000 || std::abs(y) > 10'000'000) {
        velocity_ = {};
        previous_ = t;
        received_ = now;
        return;
    }
    double a = -std::expm1(-dt * 1000 / window_ms);
    velocity_.x += a * (x - velocity_.x);
    velocity_.y += a * (y - velocity_.y);
    if (t.motion_age_us >= window_ms * 1000)
        velocity_.x = velocity_.y = 0;
    velocity_.available = true;
    previous_ = t;
    received_ = now;
}
MotionEstimate MotionTracker::estimate(int64_t now) const {
    if (!have_ || now < received_ || now - received_ > 50'000'000)
        return {};
    return velocity_;
}
double direction_multiplier(double ex, double ey, MotionEstimate m, const DirectionSettings& s) {
    if (!s.enabled)
        return 1;
    if (!m.available || !std::isfinite(m.x) || !std::isfinite(m.y))
        return 0;
    double speed = std::hypot(m.x, m.y), distance = std::hypot(ex, ey);
    if (distance < 1e-9)
        return 0;
    double cosine = speed > 1e-9 ? std::clamp((ex * m.x + ey * m.y) / (distance * speed), -1., 1.) : 0;
    double moving = s.mode == 0 ? 1 - s.strength * cosine : 1 + s.strength * cosine;
    if (s.mode == 2) {
        double right_weight = (ex / distance + 1) * .5;
        double side = s.left_strength + (s.right_strength - s.left_strength) * right_weight;
        moving = 1 + s.strength * std::max(0., cosine) * (side - 1);
    }
    double blend = std::clamp(speed / s.slow_speed, 0., 1.);
    return std::clamp(s.slow_strength + blend * (moving - s.slow_strength), 0., 2.);
}
Letterbox letterbox(int w, int h, int n) {
    if (w <= 0 || h <= 0 || n <= 0)
        throw std::invalid_argument("Invalid image dimensions");
    float scale = std::min(float(n) / w, float(n) / h);
    int rw = std::max(1, int(std::round(w * scale))), rh = std::max(1, int(std::round(h * scale)));
    return {scale, rw, rh, (n - rw) / 2, (n - rh) / 2, n};
}
float iou(const Detection& a, const Detection& b) {
    float area = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x)) *
                 std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    float denom = a.w * a.h + b.w * b.h - area;
    return denom > 0 ? area / denom : 0;
}
std::vector<Detection> decode_yolo(std::span<const float> out, int count, int nc, int width, int height,
                                   int input, const Settings& s) {
    if (count <= 0 || nc <= 0 || out.size() != size_t(count) * size_t(nc + 4))
        throw std::runtime_error("YOLO output must be [1,4+classes,candidates]");
    auto box = letterbox(width, height, input);
    std::vector<Detection> candidates;
    candidates.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        float score = -1;
        int cls = -1;
        for (int c = 0; c < nc; ++c) {
            if (!s.classes.empty() && std::find(s.classes.begin(), s.classes.end(), c) == s.classes.end())
                continue;
            float v = out[size_t(4 + c) * count + i];
            if (std::isfinite(v) && v > score) {
                score = v;
                cls = c;
            }
        }
        if (score < s.confidence || score > 1 || cls < 0)
            continue;
        float cx = out[i], cy = out[count + i], w = out[2 * count + i], h = out[3 * count + i];
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 ||
            h <= 0)
            continue;
        float x1 = std::clamp((cx - w * .5f - box.left) / box.scale, 0.f, float(width)),
              y1 = std::clamp((cy - h * .5f - box.top) / box.scale, 0.f, float(height));
        float x2 = std::clamp((cx + w * .5f - box.left) / box.scale, 0.f, float(width)),
              y2 = std::clamp((cy + h * .5f - box.top) / box.scale, 0.f, float(height));
        if (x2 > x1 && y2 > y1)
            candidates.push_back({x1, y1, x2 - x1, y2 - y1, score, cls});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](auto& a, auto& b) { return a.score > b.score; });
    // Bound worst-case NMS time on dense/noisy model output.
    if (candidates.size() > 1000)
        candidates.resize(1000);
    std::vector<Detection> kept;
    kept.reserve(300);
    for (auto& d : candidates) {
        bool suppressed = false;
        for (auto& k : kept)
            if (k.cls == d.cls && iou(k, d) > s.nms_iou) {
                suppressed = true;
                break;
            }
        if (!suppressed)
            kept.push_back(d);
        if (kept.size() == 300)
            break;
    }
    return kept;
}
void Controller::reset() {
    previous_.reset();
    residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    last_ = 0;
    predictor_.reset();
    acquired_ = jitter_at_ = noise_at_ = 0;
    jitter_x_ = jitter_y_ = noise_from_ = noise_to_ = 0;
}
Correction Controller::update(const std::vector<Detection>& ds, int width, int height, const Settings& s,
                              const Telemetry& limits, int64_t now, MotionEstimate motion,
                              int64_t capture_ns) {
    const float rx = s.reference_x < 0 ? width * .5f : s.reference_x,
                ry = s.reference_y < 0 ? height * .5f : s.reference_y;
    auto point = [&](const Detection& d) {
        return std::pair{d.x + d.w * s.aim_x + s.offset_x, d.y + d.h * s.aim_y + s.offset_y};
    };
    const float radius = s.tracking.dynamic_fov && (limits.physical & (1u << (s.tracking.fov_button - 1)))
                             ? s.tracking.held_radius
                             : s.fov_radius;
    auto matches = [&](const Detection& a, const Detection& b) {
        if (a.cls != b.cls)
            return false;
        return iou(a, b) >= s.persistence_iou ||
               (s.tracking.sticky_distance > 0 &&
                std::hypot(a.x + a.w * .5f - b.x - b.w * .5f, a.y + a.h * .5f - b.y - b.h * .5f) <=
                    s.tracking.sticky_distance);
    };
    auto eligible = [&](const Detection& d) {
        auto [x, y] = point(d);
        float dx = x - rx, dy = y - ry;
        return std::isfinite(x) && std::isfinite(y) && d.score >= s.confidence &&
               (s.classes.empty() ||
                std::find(s.classes.begin(), s.classes.end(), d.cls) != s.classes.end()) &&
               dx * dx + dy * dy <= radius * radius;
    };
    const Detection* best = nullptr;
    float rank = -1e30f, best_match = -1e30f;
    if (s.persistence && previous_)
        for (auto& d : ds)
            if (eligible(d) && matches(d, *previous_)) {
                float match = -std::hypot(d.x + d.w * .5f - previous_->x - previous_->w * .5f,
                                          d.y + d.h * .5f - previous_->y - previous_->h * .5f);
                if (match >= best_match) {
                    best = &d;
                    best_match = match;
                }
            }
    if (!best)
        for (auto& d : ds)
            if (eligible(d)) {
                auto [x, y] = point(d);
                float value = s.highest_confidence ? d.score : -((x - rx) * (x - rx) + (y - ry) * (y - ry));
                if (value > rank) {
                    rank = value;
                    best = &d;
                }
            }
    if (!best) {
        reset();
        return {};
    }
    bool same = previous_ && matches(*previous_, *best);
    if (!same) {
        reset();
    }
    if (!acquired_)
        acquired_ = now;
    auto [x, y] = point(*best);
    if (s.tracking.prediction) {
        auto predicted = predictor_.update(x, y, capture_ns ? capture_ns : now, s.tracking);
        x = std::clamp(predicted.first, 0.f, float(width));
        y = std::clamp(predicted.second, 0.f, float(height));
        double distance = std::hypot(x - rx, y - ry);
        if (distance > radius) {
            x = float(rx + (x - rx) * radius / distance);
            y = float(ry + (y - ry) * radius / distance);
        }
    }
    double ex = x - rx, ey = y - ry;
    if (std::hypot(ex, ey) <= s.deadzone) {
        ex = ey = 0;
        residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    }
    double vx = ex * s.gain_x, vy = ey * s.gain_y;
    const auto& h = s.humanization;
    double distance = std::hypot(ex, ey);
    if (h.enabled && distance > s.deadzone) {
        double progress = std::clamp(double(now - acquired_) / (h.duration_ms * 1e6), 0., 1.);
        double length = std::hypot(vx, vy);
        double px = length ? -vy / length : 0, py = length ? vx / length : 0;
        if (h.path == 1) {
            // A cubic Bezier with equal perpendicular control offsets; bend fades at both ends.
            double bend = 3 * (1 - progress) * progress * h.curve * length;
            vx += px * bend;
            vy += py * bend;
        } else if (h.path == 2) {
            double ease = std::pow(progress, h.exponent);
            vx *= ease;
            vy *= ease;
        } else if (h.path == 3) {
            double adaptive = std::clamp(distance / h.adaptive_distance, .1, 1.);
            vx *= adaptive;
            vy *= adaptive;
        } else if (h.path == 4) {
            if (!noise_at_ || now - noise_at_ >= h.noise_period_ms * 1e6) {
                noise_from_ = noise_to_;
                noise_to_ = std::uniform_real_distribution<double>(-1, 1)(random_);
                noise_at_ = now;
            }
            double t = std::clamp(double(now - noise_at_) / (h.noise_period_ms * 1e6), 0., 1.);
            double fade = t * t * (3 - 2 * t);
            double amount =
                (noise_from_ + fade * (noise_to_ - noise_from_)) * h.noise * std::min(1., distance / 20);
            vx += px * amount;
            vy += py * amount;
        }
        if (h.jitter > 0 && length > 0) {
            if (!jitter_at_ || now - jitter_at_ >= h.jitter_interval_ms * 1e6) {
                jitter_x_ = std::uniform_real_distribution<double>(-1, 1)(random_);
                jitter_y_ = std::uniform_real_distribution<double>(-1, 1)(random_);
                jitter_at_ = now;
            }
            double amount = h.jitter * std::min(1., distance / 20);
            vx += jitter_x_ * amount;
            vy += jitter_y_ * amount;
        }
    }
    if (s.smoothing_ms > 0 || h.ema_enabled) {
        double dt = last_ ? std::clamp(double(now - last_) / 1e6, 0.0, 100.0) : 1000.0 / 120;
        double a = h.ema_enabled ? h.ema_alpha : -std::expm1(-dt / s.smoothing_ms);
        smooth_x_ += a * (vx - smooth_x_);
        smooth_y_ += a * (vy - smooth_y_);
        vx = smooth_x_;
        vy = smooth_y_;
    }
    double strength = direction_multiplier(ex, ey, motion, s.direction);
    vx *= strength;
    vy *= strength;
    if (strength == 0) {
        residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    }
    const int minx = std::max(-s.max_step, limits.xmin), maxx = std::min(s.max_step, limits.xmax),
              miny = std::max(-s.max_step, limits.ymin), maxy = std::min(s.max_step, limits.ymax);
    vx = std::clamp(vx, double(minx), double(maxx));
    vy = std::clamp(vy, double(miny), double(maxy));
    double ax = std::clamp(vx + residual_x_, double(minx), double(maxx)),
           ay = std::clamp(vy + residual_y_, double(miny), double(maxy));
    int dx = int(std::trunc(ax)), dy = int(std::trunc(ay));
    residual_x_ = ax - dx;
    residual_y_ = ay - dy;
    previous_ = *best;
    last_ = now;
    return {dx, dy, *best, x, y, strength};
}
} // namespace receiver
