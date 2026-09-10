#include "receiver/control.hpp"
#include "receiver/local_mouse.hpp"
#include <iostream>
#include <cmath>
#include <filesystem>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (0)
using namespace receiver;
int main() {
    try {
        LocalMouseState local;
        local.pointer(100, 100, false, 1);
        local.pointer(104, 98, false, 2);
        CHECK(local.total_x == 4 && local.total_y == uint32_t(-2));
        local.pointer(150, 120, true, 3);
        CHECK(local.total_x == 4 && local.total_y == uint32_t(-2));
        local.pointer(151, 121, false, 4);
        CHECK(local.total_x == 5 && local.total_y == uint32_t(-1));
        local.button(2, true, true);
        CHECK(local.buttons == 0);
        local.button(2, true, false);
        local.button(2, false, true);
        CHECK(local.buttons == 2);
        local.button(2, false, false);
        CHECK(local.buttons == 0);
        TrackingSettings tracking;
        tracking.prediction = true;
        tracking.max_lead = 25;
        for (int method = 0; method < 3; ++method) {
            tracking.prediction_method = method;
            TargetPredictor predictor;
            auto first = predictor.update(100, 100, 1'000'000, tracking);
            CHECK(first.first == 100);
            std::pair<float, float> predicted;
            for (int i = 1; i <= 20; ++i) {
                float position = 100 + float(i);
                predicted = predictor.update(position, 100, 1'000'000 + i * 10'000'000ll, tracking);
                CHECK(predicted.first >= position && predicted.first - position <= 25.001f);
                CHECK(std::abs(predicted.second - 100) < .001f);
            }
            CHECK(predicted.first > 121);
            auto restarted = predictor.update(500, 50, 2'000'000'000, tracking);
            CHECK(restarted.first == 500);
            predictor.reset();
            CHECK(predictor.update(50, 50, 3'000'000'000, tracking).first == 50);
        }
        Settings s;
        Telemetry limits;
        limits.xmin = limits.ymin = -127;
        limits.xmax = limits.ymax = 127;
        Controller controller(42);
        std::vector<Detection> objects{{210, 150, 20, 20, .9f, 0}};
        s.tracking.dynamic_fov = true;
        s.tracking.held_radius = 20;
        CHECK(controller.update(objects, 320, 320, s, limits, 1'000'000).target.has_value());
        limits.physical = 1;
        CHECK(!controller.update(objects, 320, 320, s, limits, 11'000'000).target);
        limits.physical = 0;
        s.tracking.dynamic_fov = false;
        s.persistence = true;
        s.tracking.sticky_distance = 30;
        controller.reset();
        controller.update({{180, 150, 10, 10, .9f, 0}}, 320, 320, s, limits, 1'000'000);
        auto kept = controller.update({{200, 150, 10, 10, .9f, 0}, {160, 150, 10, 10, .9f, 1}}, 320, 320, s,
                                      limits, 11'000'000);
        CHECK(kept.target && kept.target->cls == 0 && kept.target->x == 200);
        s.humanization.ema_enabled = true;
        s.humanization.ema_alpha = .5f;
        s.smoothing_ms = 0;
        controller.reset();
        auto ema = controller.update(objects, 320, 320, s, limits, 1'000'000);
        CHECK(ema.dx == 6);
        s.tracking.prediction = true;
        s.secondary_button = 3;
        s.mouse_backend = 1;
        auto file = std::filesystem::temp_directory_path() / "receiver-tracking-test.json";
        save_settings(s, file.string());
        auto loaded = load_settings(file.string());
        std::filesystem::remove(file);
        CHECK(loaded.mouse_backend == 1 && loaded.secondary_button == 3 && loaded.tracking.prediction &&
              loaded.humanization.ema_enabled);
        s.mouse_backend = 5;
        bool bad = false;
        try {
            validate(s);
        } catch (...) {
            bad = true;
        }
        CHECK(bad);
        std::cout
            << "Prediction, EMA, dynamic FOV, sticky distance, local input filtering and profiles passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
