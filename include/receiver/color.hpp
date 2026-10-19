#pragma once
#include "control.hpp"
#include <memory>

namespace receiver {
struct ColorAim {
    float pixel_x = 0, pixel_y = 0, x = 0, y = 0;
};
// One bit per pixel, constructed on the image worker and shared read-only with control/preview.
struct ColorMask {
    int width = 0, height = 0;
    size_t count = 0;
    std::vector<uint64_t> bits;
    std::optional<ColorAim> highest(const Detection& region, float reference_x, float reference_y,
                                    float radius, const ColorSettings& settings) const;
};
class ColorMatcher {
    std::array<std::shared_ptr<ColorMask>, 4> pool_{};

  public:
    std::shared_ptr<const ColorMask> build(const Frame& frame, const ColorSettings& settings);
};
} // namespace receiver
