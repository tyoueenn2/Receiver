#include "receiver/assist.hpp"
#include "receiver/preferences.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (0)
using namespace receiver;
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("receiver-assist-" + std::to_string(now_ns()));
    try {
        std::filesystem::create_directories(root);
        Settings s;
        CHECK(!activation_held(0, s) && activation_held(2, s));
        s.secondary_button = 4;
        CHECK(activation_held(8, s));
        s.constant_tracking = true;
        CHECK(activation_held(0, s));
        AutoTrigger t;
        Correction c;
        c.target = Detection{140, 140, 40, 40, .95f, 0};
        CHECK(!t.update(c, 320, 320, s, 1'000'000'000));
        s.trigger.enabled = true;
        int64_t at = 1'000'000'000;
        CHECK(!t.update(c, 320, 320, s, at));
        for (int i = 1; i < 10; ++i)
            CHECK(!t.update(c, 320, 320, s, at + i * 10'000'000));
        CHECK(t.update(c, 320, 320, s, at + 100'000'000));
        // A pending click must not re-arm single mode, even over a long response wait.
        for (int i = 11; i < 40; ++i)
            CHECK(!t.update(c, 320, 320, s, at + i * 10'000'000, false));
        CHECK(!t.update(c, 320, 320, s, at + 400'000'000));
        t.suspend();
        CHECK(!t.update(c, 320, 320, s, at + 405'000'000));
        CHECK(!t.update(c, 320, 320, s, at + 409'000'000));
        CHECK(!t.update({}, 320, 320, s, at + 410'000'000));
        s.trigger.delay_ms = 0;
        CHECK(t.update(c, 320, 320, s, at + 420'000'000));
        s.trigger.spray = true;
        CHECK(!t.update(c, 320, 320, s, at + 450'000'000));
        CHECK(!t.update(c, 320, 320, s, at + 490'000'000));
        CHECK(t.update(c, 320, 320, s, at + 520'000'000));
        c.target->x = 200;
        CHECK(!t.update(c, 320, 320, s, at + 530'000'000));
        c.target->x = 140;
        s.trigger.delay_ms = 100;
        CHECK(!t.update(c, 320, 320, s, at + 540'000'000));
        CHECK(!t.update(c, 320, 320, s, at + 700'000'000)); // stale observation resets dwell
        t.reset();
        s.trigger.delay_ms = 0;
        s.trigger.spray = false;
        CHECK(t.update(c, 320, 320, s, at + 1'000'000'000));
        t.suspend();
        CHECK(!t.update(c, 320, 320, s, at + 2'000'000'000)); // interrupted single mode stays latched
        s.trigger.spray = true;
        s.visual.tracers = true;
        s.ui.always_on_top = true;
        auto loaded = settings_from_json(settings_json(s));
        CHECK(loaded.constant_tracking && loaded.trigger.spray && loaded.visual.tracers &&
              loaded.ui.always_on_top);
        auto legacy = settings_from_json(nlohmann::json::object());
        CHECK(!legacy.constant_tracking && !legacy.trigger.enabled && !legacy.collection.enabled);
        const auto config = root / "example.cfg";
        write_json_atomic({{"FOV Size", 200},
                           {"AI Minimum Confidence", 65},
                           {"Auto Trigger Delay", .2},
                           {"Mouse Sensitivity (+/-)", .8},
                           {"Y Offset (%)", 25}},
                          config);
        auto imported = import_aimmy(config, s);
        CHECK(imported.settings.fov_radius == 100 && imported.settings.trigger.delay_ms == 200 &&
              imported.settings.aim_y == .25f && imported.settings.pi_ip == s.pi_ip &&
              !imported.notes.empty());
        CHECK(std::abs(imported.settings.confidence - .65f) < .001f);
        write_json_atomic({{"AI Minimum Confidence", -1}}, config);
        bool bad = false;
        try {
            import_aimmy(config, s);
        } catch (...) {
            bad = true;
        }
        CHECK(bad);
        Frame frame;
        frame.header.width = frame.header.height = 2;
        frame.header.format = 2;
        frame.pixels = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
        CollectionSettings collection;
        collection.directory = root.string();
        collect_sample(frame, {}, collection, "disabled");
        CHECK(!std::filesystem::exists(root / "images"));
        collection.enabled = collection.auto_label = true;
        collect_sample(frame, {{0, 0, 2, 2, .9f, 3}}, collection, "test");
        std::ifstream image(root / "images/test.ppm", std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(image)), {});
        CHECK(bytes.substr(0, 11) == "P6\n2 2\n255\n" && bytes.size() == 23);
        CHECK(uint8_t(bytes[11]) == 30 && uint8_t(bytes[13]) == 10);
        image.close();
        std::ifstream labels(root / "labels/test.txt");
        std::string label;
        std::getline(labels, label);
        CHECK(label == "3 0.500000 0.500000 1.000000 1.000000");
        labels.close();
        // Invalid timing/color values must not reach control or drawing code.
        for (const auto& patch : {nlohmann::json{{"trigger", {{"interval_ms", 0}}}},
                                  nlohmann::json{{"visual", {{"box_color", {1, 2, 3}}}}}}) {
            auto j = settings_json(s);
            j.merge_patch(patch);
            bad = false;
            try {
                settings_from_json(j);
            } catch (...) {
                bad = true;
            }
            CHECK(bad);
        }
        std::filesystem::remove_all(root);
        std::cout
            << "Activation, trigger dwell/repeat, legacy profiles, Aimmy import and dataset output passed\n";
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
