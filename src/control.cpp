#include "receiver/control.hpp"
#include "receiver/color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
    auto difference = [&](int64_t a, int64_t b) {
        if (t.motion_counters_64)
            return static_cast<long double>(a) - static_cast<long double>(b);
        uint32_t delta = uint32_t(a) - uint32_t(b);
        return static_cast<long double>(delta <= 0x7fffffffu ? int64_t(delta)
                                                             : int64_t(delta) - 0x100000000ll);
    };
    double dt = double(t.sample_ns - previous_.sample_ns) / 1e9;
    double x = double(difference(t.total_x, previous_.total_x) / dt);
    double y = double(difference(t.total_y, previous_.total_y) / dt);
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
    if (count <= 0 || nc <= 0 || out.size() != size_t(count) * (size_t(nc) + 4))
        throw std::runtime_error("YOLO output must be [1,4+classes,candidates]");
    auto box = letterbox(width, height, input);
    std::vector<int> selected;
    if (!s.classes.empty()) {
        auto add = [&](int cls) {
            if (cls >= 0 && cls < nc)
                selected.push_back(cls);
        };
        for (int cls : s.classes)
            add(cls);
        add(s.class_preference.class_id);
        if (selected.empty())
            return {};
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    }
    struct Candidate {
        Detection detection;
        int index;
    };
    std::vector<Candidate> candidates;
    constexpr int block_size = 256;
    constexpr int nms_limit = 1000;
    candidates.reserve(size_t(std::min(count, nms_limit)));
    struct ClassScore {
        float score;
        int cls;
    };
    std::array<ClassScore, block_size> best;
    // Use a strict comparison to keep the first class on ties while admitting
    // scores exactly at the confidence threshold.
    const float minimum = std::max(-1.f, std::nextafter(s.confidence, -std::numeric_limits<float>::infinity()));
    for (int offset = 0; offset < count;) {
        const int length = std::min(block_size, count - offset);
        std::fill_n(best.begin(), length, ClassScore{minimum, -1});
        // Each class row is contiguous in the raw tensor. Process a tile of
        // candidates at a time so their best scores stay hot in cache.
        auto scan = [&](int cls) {
            const float* row = out.data() + (size_t(cls) + 4) * count + offset;
            for (int i = 0; i < length; ++i) {
                const float score = row[i];
                if (score > best[i].score && std::isfinite(score))
                    best[i] = {score, cls};
            }
        };
        if (s.classes.empty()) {
            for (int cls = 0; cls < nc; ++cls)
                scan(cls);
        } else {
            for (int cls : selected)
                scan(cls);
        }
        for (int j = 0; j < length; ++j) {
            auto [score, cls] = best[j];
            if (cls < 0 || score > 1)
                continue;
            const size_t i = size_t(offset + j);
            const float cx = out[i], cy = out[count + i], w = out[2ull * count + i], h = out[3ull * count + i];
            if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h) ||
                w <= 0 || h <= 0)
                continue;
            const float x1 = std::clamp((cx - w * .5f - box.left) / box.scale, 0.f, float(width));
            const float y1 = std::clamp((cy - h * .5f - box.top) / box.scale, 0.f, float(height));
            const float x2 = std::clamp((cx + w * .5f - box.left) / box.scale, 0.f, float(width));
            const float y2 = std::clamp((cy + h * .5f - box.top) / box.scale, 0.f, float(height));
            if (x2 > x1 && y2 > y1)
                candidates.push_back({{x1, y1, x2 - x1, y2 - y1, score, cls}, int(i)});
        }
        offset += length;
    }
    // Preserve model order for equal scores in both the cap and NMS.
    auto better = [](const Candidate& a, const Candidate& b) {
        if (a.detection.score != b.detection.score)
            return a.detection.score > b.detection.score;
        return a.index < b.index;
    };
    if (candidates.size() > nms_limit) {
        std::nth_element(candidates.begin(), candidates.begin() + nms_limit, candidates.end(), better);
        candidates.resize(nms_limit);
    }
    std::sort(candidates.begin(), candidates.end(), better);
    std::vector<Detection> kept;
    constexpr size_t detection_limit = 300;
    kept.reserve(std::min(candidates.size(), detection_limit));
    for (const auto& candidate : candidates) {
        const auto& d = candidate.detection;
        bool suppressed = false;
        for (auto& k : kept)
            if (k.cls == d.cls && iou(k, d) > s.nms_iou) {
                suppressed = true;
                break;
            }
        if (!suppressed)
            kept.push_back(d);
        if (kept.size() == detection_limit)
            break;
    }
    return kept;
}
void Controller::reset() {
    previous_.reset();
    previous_base_.reset();
    residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    last_ = 0;
    predictor_.reset();
    acquired_ = jitter_at_ = noise_at_ = 0;
    jitter_x_ = jitter_y_ = noise_from_ = noise_to_ = 0;
}
Correction Controller::update(const std::vector<Detection>& ds, int width, int height, const Settings& s,
                              const Telemetry& limits, int64_t now, MotionEstimate motion, int64_t capture_ns,
                              const ColorMask* colors) {
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
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(d.w) && std::isfinite(d.h) &&
               d.w > 0 && d.h > 0 && d.score >= s.confidence && d.score <= 1 &&
               dx * dx + dy * dy <= radius * radius;
    };
    auto normal = [&](const Detection& d) {
        return eligible(d) && (s.classes.empty() ||
                              std::find(s.classes.begin(), s.classes.end(), d.cls) != s.classes.end());
    };
    auto priority = [&](const Detection& d) {
        auto [x, y] = point(d);
        return s.highest_confidence ? d.score : -((x - rx) * (x - rx) + (y - ry) * (y - ry));
    };
    auto select = [&](auto accepts, const std::optional<Detection>& previous, auto value) {
        const Detection* chosen = nullptr;
        float rank = -1e30f;
        if (s.persistence && previous)
            for (const auto& d : ds)
                if (accepts(d) && matches(d, *previous)) {
                    float score = -std::hypot(d.x + d.w * .5f - previous->x - previous->w * .5f,
                                              d.y + d.h * .5f - previous->y - previous->h * .5f);
                    if (score >= rank) {
                        chosen = &d;
                        rank = score;
                    }
                }
        if (chosen)
            return chosen;
        rank = -1e30f;
        for (const auto& d : ds)
            if (accepts(d)) {
                const float score = value(d);
                if (score > rank) {
                    rank = score;
                    chosen = &d;
                }
            }
        return chosen;
    };
    const auto& preference = s.class_preference;
    const bool prefer = preference.class_id >= 0;
    const bool nearby = s.color.mode != 1 && prefer && preference.mode == 1;
    const Detection* best = nullptr;
    const Detection* base = nullptr;
    Detection color_target;
    std::optional<ColorAim> color_aim;
    const bool have_colors = colors && colors->width == width && colors->height == height;
    if (s.color.mode == 1) {
        if (have_colors)
            color_aim = colors->highest({0, 0, float(width), float(height), 1, 0}, rx, ry, radius, s.color);
        if (color_aim) {
            color_target = {color_aim->x - .5f, color_aim->y - .5f, 1, 1, 1, 0};
            best = &color_target;
        }
    } else if (nearby) {
        base = select([&](const Detection& d) { return d.cls != preference.class_id && normal(d); },
                      previous_base_, priority);
        if (base) {
            auto associated = [&](const Detection& d) {
                if (d.cls != preference.class_id || !eligible(d))
                    return false;
                const float x = d.x + d.w * .5f, y = d.y + d.h * .5f;
                const float dx = x - std::clamp(x, base->x, base->x + base->w);
                const float dy = y - std::clamp(y, base->y, base->y + base->h);
                return dx * dx + dy * dy <= preference.max_distance * preference.max_distance;
            };
            // Rank nearby parts relative to this target, not the crosshair or another player.
            auto distance = [&](const Detection& d) {
                auto [x, y] = point(*base);
                return -std::hypot(d.x + d.w * .5f - x, d.y + d.h * .5f - y);
            };
            best = select(associated, previous_, distance);
            if (!best)
                best = base;
        }
    } else {
        if (prefer)
            best = select([&](const Detection& d) { return d.cls == preference.class_id && eligible(d); },
                          previous_, priority);
        if (!best)
            best = select(normal, previous_, priority);
    }
    if (!best) {
        reset();
        return {};
    }
    if (s.color.mode == 2 && have_colors)
        color_aim = colors->highest(*best, rx, ry, radius, s.color);
    bool same = previous_ && matches(*previous_, *best) &&
                (!nearby || (previous_base_ && base && matches(*previous_base_, *base)));
    if (!same) {
        reset();
    }
    previous_base_ = base ? std::optional<Detection>(*base) : std::nullopt;
    if (!acquired_)
        acquired_ = now;
    auto [x, y] = point(*best);
    if (color_aim) {
        x = color_aim->x;
        y = color_aim->y;
    }
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
    if (h.enabled && !h.uses_abcurves() && distance > s.deadzone) {
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
    double strength = h.uses_abcurves_trajectory() ? 1 : direction_multiplier(ex, ey, motion, s.direction);
    vx *= strength;
    vy *= strength;
    if (strength == 0) {
        residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    }
    previous_ = *best;
    last_ = now;
    if (h.uses_abcurves()) {
        Correction correction{0, 0, *best, x, y, strength};
        const double aim_dx = x - rx, aim_dy = y - ry;
        if (!h.uses_abcurves_trajectory() && h.abcurves_inner_px > 0 &&
            std::hypot(aim_dx, aim_dy) <= h.abcurves_inner_px) {
            correction.curve_target = CurveTarget{aim_dx * s.gain_x, aim_dy * s.gain_y, !same, true};
            correction.strength = 1;
        } else if (vx != 0 || vy != 0) {
            correction.curve_target = CurveTarget{vx, vy, !same};
        }
        return correction;
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
    return {dx, dy, *best, x, y, strength};
}
} // namespace receiver
