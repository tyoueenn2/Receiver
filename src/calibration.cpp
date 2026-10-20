#include "receiver/calibration.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace receiver {
void validate_calibration(const CalibrationRecord& r) {
    const double values[] = {r.xx, r.xy, r.yx, r.yy, r.error_px, r.return_error_px, r.tolerance_px};
    for (double v : values)
        if (!std::isfinite(v) || std::abs(v) > 100000)
            throw std::runtime_error("Invalid calibration value");
    const double det = r.xx * r.yy - r.xy * r.yx;
    const double norm = r.xx * r.xx + r.xy * r.xy + r.yx * r.yx + r.yy * r.yy;
    if (r.version != 1 || r.context < 0 || r.context > 1 || r.max_counts < 1 || r.max_counts > 32767 ||
        r.error_px < 0 || r.return_error_px < 0 || r.tolerance_px < .1 || r.tolerance_px > 20 ||
        std::abs(det) < 1e-8 || norm / std::abs(det) > 100 || r.label.size() > 256 || r.frame_width < 0 ||
        r.frame_width > 1024 || r.frame_height < 0 || r.frame_height > 1024 || r.display_width < 0 ||
        r.display_width > 65536 || r.display_height < 0 || r.display_height > 65536)
        throw std::runtime_error("Calibration is singular or outside its supported range");
    if (r.verified &&
        (!r.layout || !r.poll_us || r.error_px > r.tolerance_px || r.return_error_px > r.tolerance_px))
        throw std::runtime_error("Verified calibration needs compatible hardware and measured accuracy");
}
CalibrationRecord fit_calibration(const std::vector<CalibrationObservation>& samples, double tolerance) {
    if (samples.size() < 8)
        throw std::runtime_error("Calibration needs both axes, directions and repeated probes");
    double a = 0, b = 0, c = 0, px = 0, py = 0, qx = 0, qy = 0;
    for (const auto& s : samples) {
        for (double value : {s.counts_x, s.counts_y, s.pixels_x, s.pixels_y})
            if (!std::isfinite(value) || std::abs(value) > 100000)
                throw std::runtime_error("Invalid calibration sample");
        a += s.counts_x * s.counts_x;
        b += s.counts_x * s.counts_y;
        c += s.counts_y * s.counts_y;
        px += s.counts_x * s.pixels_x;
        py += s.counts_y * s.pixels_x;
        qx += s.counts_x * s.pixels_y;
        qy += s.counts_y * s.pixels_y;
    }
    const double det = a * c - b * b;
    if (det < 1e-6)
        throw std::runtime_error("Calibration did not measure independent axes");
    CalibrationRecord r;
    r.tolerance_px = tolerance;
    r.method = "automatic";
    r.xx = (px * c - py * b) / det;
    r.xy = (py * a - px * b) / det;
    r.yx = (qx * c - qy * b) / det;
    r.yy = (qy * a - qx * b) / det;
    // Small controlled trials should all agree. Do not hide clipping or acceleration as outliers.
    for (const auto& s : samples)
        r.error_px = std::max(r.error_px, std::hypot(s.pixels_x - r.xx * s.counts_x - r.xy * s.counts_y,
                                                     s.pixels_y - r.yx * s.counts_x - r.yy * s.counts_y));
    validate_calibration(r);
    if (r.error_px > tolerance)
        throw std::runtime_error("Inconsistent response: pointer acceleration, clipping, or scene movement. "
                                 "Use fixed settings and retry.");
    return r;
}
std::pair<int, int> calibrated_counts(const CalibrationRecord& r, double dx, double dy) {
    validate_calibration(r);
    if (!std::isfinite(dx) || !std::isfinite(dy))
        throw std::runtime_error("Pixel target is invalid");
    const double det = r.xx * r.yy - r.xy * r.yx;
    double x = (dx * r.yy - dy * r.xy) / det, y = (dy * r.xx - dx * r.yx) / det;
    if (!std::isfinite(x) || !std::isfinite(y) || std::hypot(x, y) > r.max_counts + .001)
        throw std::runtime_error("Target is outside the calibrated movement range");
    const int ix = int(std::lround(x)), iy = int(std::lround(y));
    if (std::hypot(ix, iy) > r.max_counts + .001)
        throw std::runtime_error("Rounded target is outside the calibrated movement range");
    if (std::hypot(dx - r.xx * ix - r.xy * iy, dy - r.yx * ix - r.yy * iy) > r.tolerance_px)
        throw std::runtime_error("This pixel cannot be reached within the calibrated tolerance");
    return {ix, iy};
}
} // namespace receiver
