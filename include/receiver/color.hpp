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
    // Require matches left, right and above the reference pixel in bounded horizontal/vertical strips.
    bool surrounds(float reference_x, float reference_y, int radius, int tolerance) const;
};
class ColorMatcher {
    std::array<std::shared_ptr<ColorMask>, 4> pool_{};
    std::optional<std::array<float, 2>> hue_thresholds_;
    std::optional<std::array<float, 4>> saturation_value_thresholds_;
    std::array<std::pair<int, int>, 256> saturation_ranges_{};
    // Indexed by maximum channel, RGB delta, then signed hue numerator plus 255.
    std::array<std::array<std::array<uint64_t, 8>, 256>, 3> hue_bits_{};

    void update_thresholds(const ColorSettings& settings);

  public:
    std::shared_ptr<const ColorMask> build(const Frame& frame, const ColorSettings& settings);
};
} // namespace receiver
