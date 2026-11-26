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

// An assist can extend the user's existing tangent. Crediting their measured
// movement over the exact forecast interval avoids replaying that motion.
inline std::array<int, 2> abcurves_assist(const mouse_history::Trace& trace, size_t elapsed,
                                        const std::array<double, 2>& forecast, double target_x,
                                        double target_y, int max_step) {
    if (!elapsed || elapsed > abcurves_horizon_ms)
        return {};
    int dx = 0, dy = 0;
    for (size_t i = trace.size() - elapsed; i < trace.size(); ++i) {
        dx += trace[i][0];
        dy += trace[i][1];
    }
    const auto [vx, vy] = current_hand_direction(trace);
    const double speed = std::hypot(vx, vy);
    if (speed == 0)
        return {};
    const double ux = vx / speed, uy = vy / speed;
    const double observed = ux * dx + uy * dy;
    const double remaining = ux * (target_x - dx) + uy * (target_y - dy);
    if (observed <= 0 || remaining <= 0)
        return {};
    const double predicted = ux * forecast[0] + uy * forecast[1];
    if (!std::isfinite(predicted))
        return {};
    const double extra = std::clamp(predicted - observed, 0.,
                                    std::min({remaining, observed * .5, double(max_step)}));
    return {int(ux * extra), int(uy * extra)};
}
} // namespace receiver
