#pragma once
#include "protocol.hpp"
#include "move_protocol.hpp"
#include <string>
#include <vector>

namespace receiver {
struct CalibrationRecord {
    int version = 1, context = 0; // 0 desktop pixels, 1 local rotating-view pixels.
    bool verified = false;
    std::string method = "manual", label;
    double xx = 1, xy = 0, yx = 0, yy = 1, error_px = 0, return_error_px = 0, tolerance_px = 1;
    int max_counts = 256, frame_width = 0, frame_height = 0, display_width = 0, display_height = 0,
        display_left = 0, display_top = 0;
    uint64_t layout = 0, display = 0;
    uint32_t poll_us = 0;
};
struct ReturnActionSettings {
    bool enabled = false, verified_target = false;
    int button = 1, settle_ms = 2, press_ms = 10, return_delay_ms = 0;
};
struct CalibrationObservation {
    double counts_x = 0, counts_y = 0, pixels_x = 0, pixels_y = 0;
};
struct ViewDisplacement {
    double x = 0, y = 0, confidence = 0;
    int patches = 0;
};
struct CalibrationStatus {
    bool running = false, supported = false, desktop_feedback = false, operation_active = false;
    int completed = 0, total = 0;
    uint64_t revision = 0;
    std::string message = "Not calibrated", error, operation;
    move_protocol::Ack action;
};
void validate_calibration(const CalibrationRecord& record);
CalibrationRecord fit_calibration(const std::vector<CalibrationObservation>& samples, double tolerance);
std::pair<int, int> calibrated_counts(const CalibrationRecord& record, double dx, double dy);
ViewDisplacement register_view(const Frame& before, const Frame& after, int search = 32);
} // namespace receiver
