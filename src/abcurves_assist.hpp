#pragma once
#include "receiver/mouse_history_protocol.hpp"
#include <algorithm>
#include <cmath>

namespace receiver {
inline constexpr size_t abcurves_horizon_ms = 16;

inline std::array<int, 2> current_hand_direction(const mouse_history::Trace& trace) {
    std::array<int, 2> recent{}, newest{};
    for (size_t i = trace.size() - 4; i < trace.size(); ++i) {
        const auto delta = trace[i];
        recent[0] += delta[0];
        recent[1] += delta[1];
        if (i >= trace.size() - 2 && (delta[0] || delta[1]))
            newest = delta;
    }
    // Give a stop or reversal priority over the averaged direction.
    if (int64_t(recent[0]) * newest[0] + int64_t(recent[1]) * newest[1] <= 0)
        return {};
    return recent;
}

inline bool abcurves_aligned(const mouse_history::Trace& trace, double x, double y, double angle_deg) {
    const auto [dx, dy] = current_hand_direction(trace);
    if ((!dx && !dy) || (!x && !y))
        return false;
    constexpr double radians_per_degree = 3.14159265358979323846 / 180;
    return std::abs(std::atan2(x * dy - y * dx, x * dx + y * dy)) <= angle_deg * radians_per_degree;
}

inline std::array<int, 2> abcurves_wire_direction(double x, double y) {
    const auto length = std::hypot(x, y);
    if (!std::isfinite(length) || length == 0)
        return {};
    return {int(std::round(x / length * 10000)), int(std::round(y / length * 10000))};
}
} // namespace receiver
