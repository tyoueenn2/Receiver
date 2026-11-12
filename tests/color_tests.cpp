#include "receiver/color.hpp"
#include "receiver/assist.hpp"
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
    CHECK(aim.target->w == 1 && aim.target->h == 1); // Color trigger checks the mask, not this aim dot.
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
void trigger_geometry() {
    ColorMatcher matcher;
    ColorSettings s;
    // Exercise a word boundary, partial final word, odd/even sizes and RGB/BGRA layouts.
    for (int format : {1, 2})
        for (int width : {128, 129, 130}) {
            auto f = image(width, 25, format);
            const float rx = width * .5f, ry = 12.5f;
            const int cx = int(std::floor(rx)), cy = 12;
            pixel(f, cx - 4, cy, 255, 0, 0);
            pixel(f, cx + 4, cy, 255, 0, 0);
            pixel(f, cx, cy - 4, 255, 0, 0);
            auto mask = matcher.build(f, s);
            CHECK(mask->surrounds(rx, ry, 4, 0)); // Inclusive distance, no center pixel needed.
            CHECK(!mask->surrounds(rx, ry, 3, 0));
            for (auto [x, y] : {std::pair{cx - 4, cy}, {cx + 4, cy}, {cx, cy - 4}}) {
                pixel(f, x, y, 0, 255, 0);
                CHECK(!matcher.build(f, s)->surrounds(rx, ry, 8, 2));
                pixel(f, x, y, 255, 0, 0);
            }
            pixel(f, cx - 4, cy, 0, 0, 0);
            pixel(f, cx - 4, cy + 2, 255, 0, 0);
            mask = matcher.build(f, s);
            CHECK(!mask->surrounds(rx, ry, 4, 1));
            CHECK(mask->surrounds(rx, ry, 4, 2)); // Inclusive tolerance.
            pixel(f, cx, cy - 4, 0, 0, 0);
            pixel(f, cx, cy + 4, 255, 0, 0);
            CHECK(!matcher.build(f, s)->surrounds(rx, ry, 4, 2)); // Below is not above.
        }
    auto f = image(130, 25);
    pixel(f, 129, 12, 255, 0, 0);
    pixel(f, 127, 12, 255, 0, 0);
    pixel(f, 128, 11, 255, 0, 0);
    auto mask = matcher.build(f, s);
    CHECK(mask->surrounds(128.5f, 12.5f, 1, 0));
    CHECK(!mask->surrounds(129.5f, 12.5f, 1024, 2)); // No right-hand pixel at the edge.
    CHECK(!mask->surrounds(128.5f, 0, 1024, 2));     // No above pixel at the edge.
    CHECK(!mask->surrounds(-1, 12, 4, 0));
    CHECK(!mask->surrounds(130, 12, 4, 0));
    CHECK(!mask->surrounds(std::numeric_limits<float>::quiet_NaN(), 12, 4, 0));
    CHECK(!mask->surrounds(128, 12, 0, 0));
    CHECK(!mask->surrounds(128, 12, 4, 5));
    auto blank = image(130, 25);
    pixel(blank, 65, 12, 255, 0, 0);
    CHECK(!matcher.build(blank, s)->surrounds(65, 12, 32, 2)); // Center alone cannot trigger.
}
void color_trigger() {
    Settings s;
    s.color.mode = 1;
    s.color.offset_x = 7;
    s.color.offset_y = 10;
    s.trigger.enabled = true;
    s.trigger.delay_ms = 30;
    s.trigger.color_radius = 8;
    s.trigger.color_tolerance = 0;
    auto f = image(130, 24);
    pixel(f, 61, 12, 255, 0, 0);
    pixel(f, 69, 12, 255, 0, 0);
    pixel(f, 65, 8, 255, 0, 0);
    ColorMatcher matcher;
    auto mask = matcher.build(f, s.color);
    Controller controller(42);
    Telemetry limits;
    auto c = controller.update({}, 130, 24, s, limits, 1'000'000'000, {}, 0, mask.get());
    CHECK(c.target && c.aim_x == 72.5f && c.aim_y == 18.5f);
    AutoTrigger trigger;
    int64_t now = 1'000'000'000;
    auto update = [&](const ColorMask* colors, bool can_fire = true) {
        now += 10'000'000;
        return trigger.update(c, 130, 24, s, now, can_fire, colors);
    };
    CHECK(!update(nullptr)); // No fallback to the aim dot or stale mask.
    CHECK(!update(mask.get()));
    c.target->x += 2; // Topmost pixel jitter must not restart dwell or re-arm single mode.
    CHECK(!update(mask.get()));
    c.target->y += 2;
    CHECK(!update(mask.get()));
    CHECK(update(mask.get())); // Dwell completes despite changing aim and nonzero offsets.
    c.target->x += 2;
    CHECK(!update(mask.get()));
    trigger.suspend();
    for (int i = 0; i < 5; ++i)
        CHECK(!update(mask.get())); // Recovery retains the single-click latch.
    pixel(f, 65, 8, 0, 0, 0);
    auto missing = matcher.build(f, s.color);
    CHECK(!update(missing.get())); // A missing direction breaks continuous occupancy.
    CHECK(!update(mask.get()));
    CHECK(!update(mask.get()));
    CHECK(!update(mask.get()));
    CHECK(!update(mask.get(), false)); // Pending/physical click blocks output, not observation.
    CHECK(update(mask.get()));
    s.trigger.spray = true;
    for (int i = 0; i < 9; ++i)
        CHECK(!update(mask.get()));
    CHECK(update(mask.get()));
    now += 100'000'000;
    CHECK(!update(mask.get())); // Stale observations restart dwell.
    CHECK(!update(mask.get()));
    CHECK(!update(mask.get()));
    CHECK(update(mask.get()));
    auto wrong_dimensions = *mask;
    --wrong_dimensions.width;
    CHECK(!update(&wrong_dimensions));
    s.trigger.delay_ms = 0;
    c.target.reset();
    CHECK(!update(mask.get())); // No eligible aim target, even when geometry matches.
    c.target = Detection{140, 140, 40, 40, .9f, 0};
    s.color.mode = 0;
    CHECK(trigger.update(c, 320, 320, s, now)); // Model keeps box-based triggering without a mask.
    trigger.reset();
    s.color.mode = 2;
    CHECK(trigger.update(c, 320, 320, s, now)); // Hybrid keeps its model-box trigger.
    trigger.reset();
    s.color.mode = 1;
    s.reference_x = 65;
    s.reference_y = 12;
    CHECK(trigger.update(c, 130, 24, s, now, true, mask.get())); // Explicit crosshair reference.
    s.reference_x = 100;
    CHECK(!trigger.update(c, 130, 24, s, now, true, mask.get()));
}
void settings() {
    Settings s;
    s.color = {2, 300, 20, 40, 80, 20, 90, -4, 7, false};
    auto loaded = settings_from_json(settings_json(s));
    CHECK(loaded.color.mode == 2 && loaded.color.min_h == 300 && loaded.color.offset_x == -4 &&
          loaded.color.max_s == 80 && !loaded.color.preview);
    CHECK(settings_from_json(nlohmann::json{{"version", 1}}).color.mode == 0);
    s.trigger.color_radius = 16;
    s.trigger.color_tolerance = 3;
    loaded = settings_from_json(settings_json(s));
    CHECK(loaded.trigger.color_radius == 16 && loaded.trigger.color_tolerance == 3);
    auto legacy = settings_from_json(nlohmann::json{{"trigger", {{"enabled", true}}}});
    CHECK(legacy.trigger.color_radius == 32 && legacy.trigger.color_tolerance == 2);
    for (auto [radius, tolerance] : {std::pair{0, 0}, {1025, 2}, {32, -1}, {2, 3}}) {
        auto bad = s;
        bad.trigger.color_radius = radius;
        bad.trigger.color_tolerance = tolerance;
        rejects([&] { validate(bad); });
    }
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
        trigger_geometry();
        color_trigger();
        settings();
        std::cout << checks << " HSV, RGB/BGRA, region, offset, mode and profile checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
