#include "receiver/color.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace receiver {
std::shared_ptr<const ColorMask> ColorMatcher::build(const Frame& frame, const ColorSettings& s) {
    const auto& h = frame.header;
    const size_t channels = h.format == 1 ? 3 : 4;
    const size_t pixels = size_t(h.width) * h.height;
    if (!h.width || !h.height || h.width > 1024 || h.height > 1024 || (h.format != 1 && h.format != 2) ||
        h.bytes != pixels * channels || frame.pixels.size() < h.bytes)
        throw std::runtime_error("HSV targeting requires a complete RGB or BGRA frame");
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
    mask->bits.assign(stride * h.height, 0);
    for (size_t i = 0; i < pixels; ++i) {
        const auto* p = frame.pixels.data() + i * channels;
        const float r = p[h.format == 1 ? 0 : 2], g = p[1], b = p[h.format == 1 ? 2 : 0];
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), delta = hi - lo;
        const float value = hi * (100.f / 255.f), saturation = hi ? 100 * delta / hi : 0;
        if (value < s.min_v || value > s.max_v || saturation < s.min_s || saturation > s.max_s)
            continue;
        float hue = 0;
        if (delta) {
            hue = hi == r ? (g - b) / delta : hi == g ? 2 + (b - r) / delta : 4 + (r - g) / delta;
            hue *= 60;
            if (hue < 0)
                hue += 360;
        }
        const bool matches =
            (hue == 0 && s.max_h == 360) ||
            (s.min_h <= s.max_h ? hue >= s.min_h && hue <= s.max_h : hue >= s.min_h || hue <= s.max_h);
        if (matches) {
            const auto x = i % h.width, y = i / h.width;
            mask->bits[y * stride + x / 64] |= uint64_t(1) << (x % 64);
            ++mask->count;
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
} // namespace receiver
