#pragma once
#include "receiver/mouse_history_protocol.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace receiver {
inline constexpr size_t abcurves_max_plan_age_ms = 16;
inline constexpr size_t abcurves_trajectory_ms = 320;
inline constexpr uint64_t abcurves_trajectory_replan_ms = 32;
inline constexpr uint64_t abcurves_trajectory_max_step_ms = 10;

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

inline bool abcurves_targetward(const std::array<int, 2>& aim, const std::array<int, 2>& tangent) {
    return int64_t(aim[0]) * tangent[0] + int64_t(aim[1]) * tangent[1] > 0;
}

inline std::array<double, 2> abcurves_position(std::span<const std::array<double, 2>> path, size_t ms) {
    return ms ? path[ms - 1] : std::array<double, 2>{};
}

inline size_t abcurves_trajectory_start(std::span<const std::array<double, 2>> path, size_t elapsed,
                                       double x, double y) {
    // The observed hand can start by going away. Start playing where the forecast turns toward the target.
    for (size_t i = elapsed; i < path.size(); ++i) {
        const auto from = abcurves_position(path, i);
        const auto to = path[i];
        if (x * (to[0] - from[0]) + y * (to[1] - from[1]) > 0)
            return i;
    }
    return path.size();
}
} // namespace receiver
