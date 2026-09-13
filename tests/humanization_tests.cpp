#include "receiver/control.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <limits>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (0)
using namespace receiver;
bool close(double a, double b) {
    return std::abs(a - b) < 1e-5;
}
int main() {
    try {
        DirectionSettings d;
        CHECK(direction_multiplier(30, 0, {}, d) == 1);
        d.enabled = true;
        CHECK(direction_multiplier(30, 0, {}, d) == 0);
        CHECK(close(direction_multiplier(30, 0, {true, 100, 0}, d), .5));
        CHECK(close(direction_multiplier(30, 0, {true, -100, 0}, d), 1.5));
        CHECK(close(direction_multiplier(-30, 0, {true, -100, 0}, d), .5));
        CHECK(close(direction_multiplier(30, 0, {true, 0, 100}, d), 1));
        CHECK(close(direction_multiplier(30, 0, {true, 0, 0}, d), 1));
        CHECK(close(direction_multiplier(30, 0, {true, 20, 0}, d), .75));
        d.slow_strength = 1.5f;
        CHECK(close(direction_multiplier(30, 0, {true, 0, 0}, d), 1.5));
        d.mode = 1;
        CHECK(close(direction_multiplier(30, 0, {true, 100, 0}, d), 1.5));
        CHECK(close(direction_multiplier(30, 0, {true, -100, 0}, d), .5));
        d.mode = 2;
        d.strength = 1;
        CHECK(close(direction_multiplier(30, 0, {true, 100, 0}, d), .5));
        CHECK(close(direction_multiplier(-30, 0, {true, -100, 0}, d), 1.5));
        CHECK(close(direction_multiplier(-30, 0, {true, 100, 0}, d), 1));
        CHECK(direction_multiplier(30, 0, {true, std::numeric_limits<double>::quiet_NaN(), 0}, d) == 0);
        MotionTracker tracker;
        Telemetry t;
        t.ready = t.has_motion = true;
        t.server = 1;
        t.motion_generation = 1;
        t.sample_ns = 1'000'000'000;
        t.total_x = 0xfffffffe;
        tracker.observe(t, 1'000'000'000, 5);
        CHECK(!tracker.estimate(1'000'000'000).available);
        t.sample_ns += 10'000'000;
        t.total_x = 8;
        tracker.observe(t, 1'010'000'000, 5);
        CHECK(tracker.estimate(1'010'000'000).available && tracker.estimate(1'010'000'000).x > 800);
        t.sample_ns += 10'000'000;
        t.motion_age_us = 10000;
        tracker.observe(t, 1'020'000'000, 5);
        CHECK(tracker.estimate(1'020'000'000).x == 0);
        CHECK(!tracker.estimate(1'071'000'000).available);
        ++t.motion_generation;
        t.sample_ns += 10'000'000;
        tracker.observe(t, 1'030'000'000, 5);
        CHECK(!tracker.estimate(1'030'000'000).available);
        t.has_motion = false;
        tracker.observe(t, 1'040'000'000, 5);
        CHECK(!tracker.estimate(1'040'000'000).available);
        auto request = subscribe(1, 2, true);
        CHECK(request[3] == '2');
        std::array<uint8_t, 88> packet{};
        std::memcpy(packet.data(), "UPT2", 4);
        packet[4] = 1;
        put64(packet.data() + 8, 1);
        put64(packet.data() + 16, 2);
        put64(packet.data() + 24, 3);
        put32(packet.data() + 36, uint32_t(-127));
        put32(packet.data() + 40, 127);
        put32(packet.data() + 44, uint32_t(-127));
        put32(packet.data() + 48, 127);
        put64(packet.data() + 56, 7);
        put64(packet.data() + 64, 123);
        put32(packet.data() + 72, 0xffffffff);
        auto parsed = parse_telemetry(packet);
        CHECK(parsed && parsed->has_motion && parsed->total_x == 0xffffffff);
        packet[84] = 1;
        CHECK(!parse_telemetry(packet));
        packet[84] = 0;
        std::array<uint8_t, 80> public_v2{};
        std::copy_n(packet.begin(), 72, public_v2.begin());
        put16(public_v2.data() + 72, uint16_t(-12));
        put16(public_v2.data() + 74, 34);
        put32(public_v2.data() + 76, 500);
        parsed = parse_telemetry(public_v2);
        CHECK(parsed && parsed->kind == Telemetry::Kind::upt2_public && !parsed->has_motion);
        CHECK(parsed->last_physical_dx == -12 && parsed->last_physical_dy == 34);
        std::array<uint8_t, 128> packet3{};
        std::memcpy(packet3.data(), "UPT3", 4);
        packet3[4] = 1;
        packet3[5] = 0xa4; // physical only
        packet3[6] = 0x02; // applied persistent only
        packet3[7] = 0x08; // scheduled only
        put64(packet3.data() + 8, 1);
        put64(packet3.data() + 16, 2);
        put64(packet3.data() + 24, 3);
        put32(packet3.data() + 32, 4);
        put32(packet3.data() + 36, uint32_t(-127));
        put32(packet3.data() + 40, 127);
        put32(packet3.data() + 44, uint32_t(-127));
        put32(packet3.data() + 48, 127);
        put32(packet3.data() + 52, 1000);
        put64(packet3.data() + 56, 8);
        put64(packet3.data() + 64, 1'000'000);
        put64(packet3.data() + 72, uint64_t(-9));
        put64(packet3.data() + 80, 12);
        put32(packet3.data() + 88, 100);
        put32(packet3.data() + 92, 20);
        put32(packet3.data() + 96, 19);
        put16(packet3.data() + 100, 1);
        put16(packet3.data() + 102, 2);
        put32(packet3.data() + 104, 30);
        put32(packet3.data() + 108, 29);
        put16(packet3.data() + 112, 3);
        put16(packet3.data() + 114, 4);
        put32(packet3.data() + 116, 5);
        put32(packet3.data() + 120, 6);
        parsed = parse_telemetry(packet3);
        CHECK(parsed && parsed->kind == Telemetry::Kind::upt3 && parsed->has_motion &&
              parsed->motion_counters_64);
        CHECK(parsed->physical == 0xa4 && parsed->applied_persistent == 2 && parsed->scheduled == 8);
        CHECK(parsed->total_x == -9 && parsed->total_y == 12 && parsed->endpoint_poll_us == 1000);
        CHECK(parsed->accepted_click_total == 20 && parsed->completed_click_total == 19 &&
              parsed->writer_failures == 6);
        packet3[127] = 1;
        CHECK(!parse_telemetry(packet3));
        packet3[127] = 0;
        put32(packet3.data() + 96, 21);
        CHECK(!parse_telemetry(packet3));
        CHECK(subscribe(1, 2, SubscriptionVersion::v3)[3] == '3');
        Telemetry limits;
        limits.xmin = limits.ymin = -127;
        limits.xmax = limits.ymax = 127;
        std::vector<Detection> targets{{180, 150, 20, 20, .9f, 0}};
        Settings s;
        s.direction.enabled = true;
        s.direction.strength = 1;
        Controller c(123);
        auto r = c.update(targets, 320, 320, s, limits, 1'000'000, {true, 100, 0});
        CHECK(r.dx == 0 && r.dy == 0);
        r = c.update(targets, 320, 320, s, limits, 11'000'000, {true, -100, 0});
        CHECK(r.dx == 12);
        r = c.update(targets, 320, 320, s, limits, 21'000'000, {});
        CHECK(r.dx == 0 && r.dy == 0);
        CHECK(!c.update({}, 320, 320, s, limits, 31'000'000, {true, -100, 0}).target);
        s.direction.enabled = false;
        s.humanization.enabled = true;
        s.humanization.jitter = 20;
        s.max_step = 8;
        for (int path = 0; path <= 4; ++path) {
            s.humanization.path = path;
            c.reset();
            for (int i = 0; i < 200; ++i) {
                r = c.update(targets, 320, 320, s, limits, 1'000'000 + i * 10'000'000ll);
                CHECK(std::abs(r.dx) <= 8 && std::abs(r.dy) <= 8);
            }
            auto centered = targets;
            centered[0].x = 150;
            r = c.update(centered, 320, 320, s, limits, 3'000'000'000);
            CHECK(r.dx == 0 && r.dy == 0);
        }
        s.humanization.jitter = 0;
        s.humanization.path = 2;
        s.max_step = 32;
        c.reset();
        CHECK(c.update(targets, 320, 320, s, limits, 1'000'000).dx == 0);
        CHECK(c.update(targets, 320, 320, s, limits, 201'000'000).dx == 6);
        c.reset();
        CHECK(c.update(targets, 320, 320, s, limits, 301'000'000).dx == 0);
        auto file = std::filesystem::temp_directory_path() / "receiver-humanization-test.json";
        s.direction = d;
        s.humanization.path = 4;
        save_settings(s, file.string());
        auto loaded = load_settings(file.string());
        CHECK(loaded.direction.mode == 2 && loaded.humanization.path == 4);
        {
            std::ofstream f(file);
            f << "{\"version\":1,\"gain_x\":0.3}";
        }
        loaded = load_settings(file.string());
        CHECK(!loaded.direction.enabled && !loaded.humanization.enabled && loaded.gain_x == .3f);
        std::filesystem::remove(file);
        s.direction.slow_speed = 0;
        bool invalid = false;
        try {
            validate(s);
        } catch (...) {
            invalid = true;
        }
        CHECK(invalid);
        std::cout << "Humanization, motion telemetry, direction ratios, resets, caps and profile "
                     "compatibility passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
