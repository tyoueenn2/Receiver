#include "receiver/color.hpp"
#include "receiver/preferences.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace receiver;
namespace {
int checks = 0;
#define CHECK(value)                                                                                         \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(value))                                                                                        \
            throw std::runtime_error("Failed: " #value);                                                     \
    } while (false)
template <class F> void rejects(F action) {
    try {
        action();
    } catch (const std::exception&) {
        ++checks;
        return;
    }
    throw std::runtime_error("Expected rejection");
}
Frame image(int w = 130, int h = 24, int format = 1) {
    Frame f;
    f.header.width = uint16_t(w);
    f.header.height = uint16_t(h);
    f.header.format = uint8_t(format);
    f.header.bytes = uint32_t(w * h * (format == 1 ? 3 : 4));
    return f;
}
void pixel(Frame& f, int x, int y, int r, int g, int b) {
    const bool rgb = f.header.format == 1;
    auto* p = f.pixels.data() + (size_t(y) * f.header.width + x) * (rgb ? 3 : 4);
    p[rgb ? 0 : 2] = uint8_t(r);
    p[1] = uint8_t(g);
    p[rgb ? 2 : 0] = uint8_t(b);
    if (!rgb)
        p[3] = 0; // Alpha is irrelevant, including transparent capture pixels.
}
Detection full(const Frame& f) {
    return {0, 0, float(f.header.width), float(f.header.height), 1, 0};
}
void matching() {
    ColorMatcher matcher;
    ColorSettings s;
    for (int format : {1, 2}) {
        auto f = image(130, 24, format);
        pixel(f, 1, 1, 0, 255, 0);
        pixel(f, 1, 2, 0, 0, 255);
        pixel(f, 1, 3, 255, 255, 255);
        pixel(f, 1, 4, 50, 0, 0);      // Below minimum value.
        pixel(f, 1, 5, 255, 120, 120); // Below minimum saturation.
        pixel(f, 64, 6, 255, 0, 20);   // Negative RGB hue wraps through red.
        pixel(f, 65, 6, 255, 20, 0);
        pixel(f, 129, 8, 255, 0, 0); // Last partial mask word.
        auto mask = matcher.build(f, s);
        CHECK(mask->count == 3);
        auto aim = mask->highest(full(f), 65.5f, 12, 200, s);
        CHECK(aim && aim->pixel_x == 65.5f && aim->pixel_y == 6.5f);
        aim = mask->highest(full(f), 65, 12, 200, s);
        CHECK(aim && aim->pixel_x == 64.5f); // Equal horizontal distance: left pixel.
        auto green = s;
        green.min_h = green.max_h = 120;
        auto second = matcher.build(f, green);
        CHECK(second->count == 1);
        CHECK(mask->count == 3); // Subsequent builds cannot mutate published snapshots.
        auto all = s;
        all.min_h = 0;
        all.max_h = 360;
        all.min_s = all.min_v = 0;
        CHECK(matcher.build(f, all)->count == size_t(f.header.width) * f.header.height);
        auto red = s;
        red.min_h = red.max_h = 0;
        CHECK(matcher.build(f, red)->count == 1);
        red.min_h = red.max_h = 360;
        CHECK(matcher.build(f, red)->count == 1); // 360 and 0 are the same hue.
        auto gray = s;
        gray.min_h = 0;
        gray.max_h = 360;
        gray.min_s = gray.max_s = 0;
        gray.min_v = gray.max_v = 100;
        CHECK(matcher.build(f, gray)->count == 1);
    }
    auto invalid = image();
    invalid.header.format = 3;
    rejects([&] { matcher.build(invalid, s); });
    invalid = image();
    --invalid.header.bytes;
    rejects([&] { matcher.build(invalid, s); });
    invalid = image();
    invalid.pixels.resize(1);
    rejects([&] { matcher.build(invalid, s); });
}
void regions_and_offsets() {
    auto f = image();
    pixel(f, 10, 1, 255, 0, 0); // Highest, but outside a small FOV.
    pixel(f, 65, 8, 255, 0, 0);
    pixel(f, 66, 9, 255, 0, 0);
    ColorSettings s;
    ColorMatcher matcher;
    auto mask = matcher.build(f, s);
    auto aim = mask->highest(full(f), 65.5f, 12, 5, s);
    CHECK(aim && aim->pixel_y == 8.5f);
    CHECK(!mask->highest(full(f), 65.5f, 12, 1, s));
    Detection box{65.6f, 8, 1.4f, 3, 1, 0};
    aim = mask->highest(box, 65.5f, 12, 200, s);
    CHECK(aim && aim->pixel_x == 66.5f && aim->pixel_y == 9.5f);
    s.offset_x = 100;
    s.offset_y = -100;
    aim = mask->highest(box, 65.5f, 12, 200, s);
    CHECK(aim && aim->x == 67 && aim->y == 8); // Offset stays within the selected box.
    aim = mask->highest(full(f), 65.5f, 12, 200, s);
    CHECK(aim && aim->x == 110.5f && aim->y == 0);
    CHECK(!mask->highest({-5, -5, 2, 2, 1, 0}, 0, 0, 200, s));
    CHECK(!mask->highest({130, 0, 20, 20, 1, 0}, 0, 0, 200, s));
    CHECK(!mask->highest({65.6f, 8, .1f, 1, 1, 0}, 65, 12, 200, s));
    s = {};
    // A pixel center on the box's right edge is outside; its left edge is inclusive.
    CHECK(!mask->highest({64, 8, 1.5f, 1, 1, 0}, 65, 12, 200, s));
    CHECK(mask->highest({65.5f, 8.5f, 1, 1, 1, 0}, 65, 12, 200, s));
}
void controller_modes() {
    auto f = image(320, 320);
    pixel(f, 200, 60, 255, 0, 0); // Pure-color highest; outside the normal target box.
    pixel(f, 150, 130, 255, 0, 0);
    pixel(f, 160, 140, 255, 0, 0);
    ColorMatcher matcher;
    Settings s;
    s.color.mode = 1;
    s.classes = {999};
    s.confidence = 1; // Model filters never suppress a color dot.
    s.class_preference.class_id = 99;
    s.class_preference.mode = 1;
    s.color.offset_x = 2;
    s.color.offset_y = 3;
    auto mask = matcher.build(f, s.color);
    Telemetry limits;
    limits.xmin = limits.ymin = -127;
    limits.xmax = limits.ymax = 127;
    Controller control(42);
    auto update = [&](const std::vector<Detection>& ds, const ColorMask* colors) {
        return control.update(ds, 320, 320, s, limits, now_ns(), {}, 0, colors);
    };
    auto aim = update({}, mask.get());
    CHECK(aim.target && aim.aim_x == 202.5f && aim.aim_y == 63.5f);
    CHECK(aim.target->w == 1 && aim.target->h == 1); // Auto trigger sees a dot-sized target.
    CHECK(aim.target->x + .5f == aim.aim_x && aim.target->y + .5f == aim.aim_y);
    CHECK(!update({{140, 120, 40, 100, 1, 999}}, nullptr).target);
    s.tracking.dynamic_fov = true;
    s.tracking.held_radius = 35;
    limits.physical = 1;
    aim = update({}, mask.get());
    CHECK(aim.target && aim.aim_x == 152.5f && aim.aim_y == 133.5f);
    s.tracking.dynamic_fov = false;
    s.classes = {0};
    s.class_preference.class_id = -1;
    s.confidence = .45f;
    s.color.mode = 2;
    const Detection player{140, 120, 40, 100, .9f, 0}, other{190, 50, 30, 30, .99f, 0};
    aim = update({other, player}, mask.get());
    CHECK(aim.target && aim.target->x == player.x);
    CHECK(aim.aim_x == 152.5f && aim.aim_y == 133.5f); // Colors refine only the normal target.
    s.color.offset_y = -1024;
    aim = update({player}, mask.get());
    CHECK(aim.target && aim.aim_y == player.y);
    s.color.offset_y = 3;
    s.fov_radius = 15;
    aim = update({player}, mask.get());
    CHECK(aim.target && aim.aim_x == 160 && aim.aim_y == 170); // Color outside FOV falls back.
    s.fov_radius = 160;
    aim = update({player}, nullptr);
    CHECK(aim.target && aim.aim_x == 160 && aim.aim_y == 170); // Requested fallback.
    auto blank = matcher.build(image(320, 320), s.color);
    aim = update({player}, blank.get());
    CHECK(aim.target && aim.aim_x == 160 && aim.aim_y == 170);
    CHECK(!update({}, mask.get()).target); // Hybrid requires a model detection.
    s.color.mode = 0;
    aim = update({player}, mask.get());
    CHECK(aim.target && aim.aim_x == 160 && aim.aim_y == 170);
}
void settings() {
    Settings s;
    s.color = {2, 300, 20, 40, 80, 20, 90, -4, 7, false};
    auto loaded = settings_from_json(settings_json(s));
    CHECK(loaded.color.mode == 2 && loaded.color.min_h == 300 && loaded.color.offset_x == -4 &&
          loaded.color.max_s == 80 && !loaded.color.preview);
    CHECK(settings_from_json(nlohmann::json{{"version", 1}}).color.mode == 0);
    for (int mode : {-1, 3}) {
        auto bad = s;
        bad.color.mode = mode;
        rejects([&] { validate(bad); });
    }
    for (float value : {-1.f, 361.f, std::numeric_limits<float>::quiet_NaN()}) {
        auto bad = s;
        bad.color.min_h = value;
        rejects([&] { validate(bad); });
    }
    for (float value : {-1.f, 101.f, std::numeric_limits<float>::infinity()}) {
        auto bad = s;
        bad.color.max_v = value;
        rejects([&] { validate(bad); });
    }
    auto bad = s;
    bad.color.min_s = 90;
    rejects([&] { validate(bad); });
    bad = s;
    bad.color.offset_y = 1025;
    rejects([&] { validate(bad); });
}
} // namespace
int main() {
    try {
        matching();
        regions_and_offsets();
        controller_modes();
        settings();
        std::cout << checks << " HSV, RGB/BGRA, region, offset, mode and profile checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
