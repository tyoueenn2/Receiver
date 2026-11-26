#include "receiver/color.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace receiver {
void ColorMatcher::update_thresholds(const ColorSettings& s) {
    const std::array hue_thresholds{s.min_h, s.max_h};
    if (hue_thresholds != hue_thresholds_) {
        auto matches = [&](float hue) {
            return (hue == 0 && s.max_h == 360) ||
                   (s.min_h <= s.max_h ? hue >= s.min_h && hue <= s.max_h : hue >= s.min_h || hue <= s.max_h);
        };
        for (int channel = 0; channel < 3; ++channel) {
            for (int delta = 0; delta < 256; ++delta) {
                auto& bits = hue_bits_[channel][delta];
                bits.fill(0);
                for (int n = -delta; n <= delta; ++n) {
                    float hue = delta ? float(n) / float(delta) + float(channel * 2) : 0;
                    hue *= 60;
                    if (hue < 0)
                        hue += 360;
                    if (matches(hue)) {
                        const int index = n + 255;
                        bits[index / 64] |= uint64_t(1) << (index % 64);
                    }
                }
            }
        }
        hue_thresholds_ = hue_thresholds;
    }
    const std::array saturation_value_thresholds{s.min_s, s.max_s, s.min_v, s.max_v};
    if (saturation_value_thresholds == saturation_value_thresholds_)
        return;
    for (int hi = 0; hi < 256; ++hi) {
        auto& range = saturation_ranges_[hi];
        range = {256, -1};
        const float value = float(hi) * (100.f / 255.f);
        if (value < s.min_v || value > s.max_v)
            continue;
        for (int delta = 0; delta <= hi; ++delta) {
            const float saturation = hi ? float(100 * delta) / float(hi) : 0;
            if (saturation < s.min_s || saturation > s.max_s)
                continue;
            range.first = std::min(range.first, delta);
            range.second = delta;
        }
    }
    saturation_value_thresholds_ = saturation_value_thresholds;
}

std::shared_ptr<const ColorMask> ColorMatcher::build(const Frame& frame, const ColorSettings& s) {
    const auto& h = frame.header;
    const size_t channels = h.format == 1 ? 3 : 4;
    const size_t pixels = size_t(h.width) * h.height;
    if (!h.width || !h.height || h.width > 1024 || h.height > 1024 || (h.format != 1 && h.format != 2) ||
        h.bytes != pixels * channels || frame.pixels.size() < h.bytes)
        throw std::runtime_error("HSV targeting requires a complete RGB or BGRA frame");
    update_thresholds(s);
    std::shared_ptr<ColorMask> mask;
    for (auto& slot : pool_)
        if (!slot || slot.use_count() == 1) {
            if (!slot)
                slot = std::make_shared<ColorMask>();
            mask = slot;
            break;
        }
    if (!mask)
        mask = std::make_shared<ColorMask>();
    mask->width = h.width;
    mask->height = h.height;
    mask->count = 0;
    const size_t stride = (h.width + 63) / 64;
    mask->bits.resize(stride * h.height);
    const int red = h.format == 1 ? 0 : 2, blue = 2 - red;
    const auto* p = frame.pixels.data();
    auto out = mask->bits.begin();
    for (int y = 0; y < h.height; ++y) {
        for (int x = 0; x < h.width; x += 64) {
            uint64_t bits = 0;
            const int extent = std::min(64, h.width - x);
            for (int bit = 0; bit < extent; ++bit, p += channels) {
                const int r = p[red], g = p[1], b = p[blue];
                const int hi = std::max({r, g, b}), delta = hi - std::min({r, g, b});
                const auto [min_delta, max_delta] = saturation_ranges_[hi];
                if (delta < min_delta || delta > max_delta)
                    continue;
                const int channel = hi == r ? 0 : hi == g ? 1 : 2;
                const int index = 255 + (hi == r ? g - b : hi == g ? b - r : r - g);
                const auto& hue = hue_bits_[channel][delta];
                bits |= ((hue[index / 64] >> (index % 64)) & 1) << bit;
            }
            *out++ = bits;
            mask->count += std::popcount(bits);
        }
    }
    return mask;
}
std::optional<ColorAim> ColorMask::highest(const Detection& region, float rx, float ry, float radius,
                                           const ColorSettings& s) const {
    if (!count || !std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.w) ||
        !std::isfinite(region.h) || region.w <= 0 || region.h <= 0)
        return {};
    const float left = std::clamp(region.x, 0.f, float(width)),
                top = std::clamp(region.y, 0.f, float(height)),
                right = std::clamp(region.x + region.w, 0.f, float(width)),
                bottom = std::clamp(region.y + region.h, 0.f, float(height));
    if (right <= left || bottom <= top)
        return {};
    const int x0 = int(std::ceil(left - .5f)), x1 = int(std::ceil(right - .5f));
    const int y0 = int(std::ceil(top - .5f)), y1 = int(std::ceil(bottom - .5f));
    if (x0 >= x1 || y0 >= y1)
        return {};
    const size_t stride = (width + 63) / 64;
    const float min_x = std::clamp(x0 + .5f + s.offset_x, left, right),
                max_x = std::clamp(x1 - .5f + s.offset_x, left, right);
    const float nearest_dx = std::clamp(rx, min_x, max_x) - rx;
    for (int y = y0; y < y1; ++y) {
        const float aim_y = std::clamp(y + .5f + s.offset_y, top, bottom), dy = aim_y - ry;
        const float horizontal_budget = radius * radius - dy * dy;
        if (nearest_dx * nearest_dx > horizontal_budget)
            continue; // Reject whole rows before walking the set bits.
        std::optional<ColorAim> best;
        float distance = std::numeric_limits<float>::infinity();
        for (int word = x0 / 64; word < (x1 + 63) / 64; ++word) {
            uint64_t row = bits[size_t(y) * stride + word];
            while (row) {
                const int x = word * 64 + std::countr_zero(row);
                row &= row - 1;
                if (x < x0 || x >= x1)
                    continue;
                ColorAim aim{x + .5f, y + .5f, std::clamp(x + .5f + s.offset_x, left, right), aim_y};
                if ((aim.x - rx) * (aim.x - rx) > horizontal_budget)
                    continue;
                const float dx = std::abs(aim.pixel_x - rx);
                if (dx < distance) {
                    best = aim;
                    distance = dx;
                }
            }
        }
        if (best)
            return best;
    }
    return {};
}
bool ColorMask::surrounds(float rx, float ry, int radius, int tolerance) const {
    if (!count || !std::isfinite(rx) || !std::isfinite(ry) || rx < 0 || ry < 0 || rx >= width ||
        ry >= height || radius < 1 || radius > 1024 || tolerance < 0 || tolerance > radius)
        return false;
    const int cx = int(std::floor(rx)), cy = int(std::floor(ry));
    const size_t stride = (width + 63) / 64;
    auto any = [&](int x0, int y0, int x1, int y1) {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, width - 1);
        y1 = std::min(y1, height - 1);
        if (x0 > x1 || y0 > y1)
            return false;
        for (int y = y0; y <= y1; ++y)
            for (int word = x0 / 64; word <= x1 / 64; ++word) {
                const int lo = std::max(x0 - word * 64, 0), hi = std::min(x1 - word * 64, 63);
                const auto range = (~uint64_t(0) << lo) & (~uint64_t(0) >> (63 - hi));
                if (bits[size_t(y) * stride + word] & range)
                    return true;
            }
        return false;
    };
    return any(cx - radius, cy - tolerance, cx - 1, cy + tolerance) &&
           any(cx + 1, cy - tolerance, cx + radius, cy + tolerance) &&
           any(cx - tolerance, cy - radius, cx + tolerance, cy - 1);
}
} // namespace receiver
