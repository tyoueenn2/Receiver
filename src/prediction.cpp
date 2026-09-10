#include "receiver/control.hpp"
#include <algorithm>
#include <cmath>
namespace receiver {
void TargetPredictor::Axis::update(double observed, double dt) {
    position += velocity * dt;
    double dt2 = dt * dt;
    double a = p00 + 2 * dt * p01 + dt2 * p11 + 2500 * dt2 * dt2 / 4;
    double b = p01 + dt * p11 + 2500 * dt2 * dt / 2;
    double c = p11 + 2500 * dt2;
    double innovation = observed - position, denom = a + 4;
    position += a / denom * innovation;
    velocity = std::clamp(velocity + b / denom * innovation, -5000., 5000.);
    p00 = a * 4 / denom;
    p01 = b * 4 / denom;
    p11 = std::max(0., c - b * b / denom);
}
void TargetPredictor::reset() {
    *this = {};
}
std::pair<float, float> TargetPredictor::update(float x, float y, int64_t at, const TrackingSettings& s) {
    if (!at_ || at <= at_ || at - at_ > 100'000'000) {
        reset();
        at_ = at;
        previous_x_ = x;
        previous_y_ = y;
        x_.position = x;
        y_.position = y;
        return {x, y};
    }
    double dt = double(at - at_) / 1e9;
    double raw_x = std::clamp((x - previous_x_) / dt, -5000., 5000.);
    double raw_y = std::clamp((y - previous_y_) / dt, -5000., 5000.);
    x_.update(x, dt);
    y_.update(y, dt);
    double alpha = -std::expm1(-dt * 1000 / s.velocity_smoothing_ms);
    vx_ += alpha * (raw_x - vx_);
    vy_ += alpha * (raw_y - vy_);
    history_[count_++ % history_.size()] = {raw_x, raw_y};
    double vx = x_.velocity, vy = y_.velocity;
    if (s.prediction_method == 1) {
        vx = vx_;
        vy = vy_;
    }
    if (s.prediction_method == 2) {
        vx = vy = 0;
        for (size_t i = 0; i < std::min(count_, history_.size()); ++i) {
            vx += history_[i].first;
            vy += history_[i].second;
        }
        vx /= std::min(count_, history_.size());
        vy /= std::min(count_, history_.size());
    }
    double dx = vx * s.lead_ms / 1000 * s.lead_multiplier;
    double dy = vy * s.lead_ms / 1000 * s.lead_multiplier;
    double length = std::hypot(dx, dy);
    if (length > s.max_lead) {
        dx *= s.max_lead / length;
        dy *= s.max_lead / length;
    }
    at_ = at;
    previous_x_ = x;
    previous_y_ = y;
    return {float(x + dx), float(y + dy)};
}
} // namespace receiver
