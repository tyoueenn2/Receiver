#include "receiver/control.hpp"
#include "receiver/preferences.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace receiver;
namespace {
int checks = 0;
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(condition))                                                                                    \
            throw std::runtime_error("Failed: " #condition);                                                 \
    } while (false)
template <class F> void rejects(F action) {
    try {
        action();
    } catch (const std::exception&) {
        ++checks;
        return;
    }
    throw std::runtime_error("Expected invalid preference to be rejected");
}
Telemetry limits() {
    Telemetry result;
    result.xmin = result.ymin = -127;
    result.xmax = result.ymax = 127;
    return result;
}
struct Selection {
    Controller controller{42};
    int64_t at = 1'000'000;
    std::optional<Detection> operator()(const std::vector<Detection>& detections, const Settings& settings) {
        at += 10'000'000;
        return controller.update(detections, 320, 320, settings, limits(), at).target;
    }
};
const Detection player{140, 140, 40, 100, .9f, 0};
const Detection head{150, 120, 20, 20, .8f, 1}; // Center is 10 pixels above player's box.
const Detection other_player{240, 100, 40, 100, .95f, 0};
const Detection other_head{250, 80, 20, 20, .99f, 1};

void global_preference() {
    Settings s;
    s.classes = {0};
    s.persistence = true;
    Selection select;
    CHECK(select({player}, s)->cls == 0);
    s.class_preference.class_id = 1;
    CHECK(select({player, other_head}, s)->cls == 1); // Overrides an existing body lock and class filter.
    CHECK(select({player}, s)->cls == 0);             // Missing preference falls back immediately.
    CHECK(select({player, head}, s)->cls == 1);
    s.class_preference.class_id = -1;
    CHECK(select({player, head}, s)->cls == 0);
    s.persistence = false;
    s.class_preference.class_id = 1;
    CHECK(select({other_head, head}, s)->x == head.x); // Normal priority among preferred detections.
    s.highest_confidence = true;
    CHECK(select({head, other_head}, s)->x == other_head.x);
    auto weak = head;
    weak.score = .1f;
    CHECK(select({player, weak}, s)->cls == 0);
    s.fov_radius = 45;
    CHECK(select({player, other_head}, s)->cls == 0);
    CHECK(!select({other_head}, s));
    s.fov_radius = 160;
    weak.score = std::numeric_limits<float>::quiet_NaN();
    CHECK(select({player, weak}, s)->cls == 0);
}

void proximity_preference() {
    Settings s;
    s.classes = {0};
    s.class_preference = {1, "head", 1, 10};
    Selection select;
    CHECK(select({player, other_player, head, other_head}, s)->x == head.x);
    CHECK(select({player, other_player, other_head}, s)->cls == 0);
    CHECK(select({player, other_player, other_head}, s)->x == player.x);
    CHECK(!select({head}, s)); // A preferred class cannot anchor itself.
    s.classes = {1};
    CHECK(!select({player, head}, s)); // An excluded player cannot qualify a head.
    s.classes.clear();
    CHECK(select({player, head}, s)->cls == 1); // All-classes mode still excludes heads from anchors.
    s.classes = {0};
    s.class_preference.max_distance = 9;
    CHECK(select({player, head}, s)->cls == 0);
    s.class_preference.max_distance = 10;
    CHECK(select({player, head}, s)->cls == 1); // Inclusive distance boundary.
    s.class_preference.max_distance = 0;
    auto contained = head;
    contained.y = player.y;
    CHECK(select({player, contained}, s)->cls == 1); // Inside the body needs no extra margin.
    CHECK(select({player, head}, s)->cls == 0);
    auto diagonal = head;
    diagonal.x = 180; // Center 10 beyond both the top and right edges: sqrt(200) > 10.
    s.class_preference.max_distance = 10;
    CHECK(select({player, diagonal}, s)->cls == 0);
    s.class_preference.max_distance = 15;
    CHECK(select({player, diagonal}, s)->cls == 1);
    s.class_preference.max_distance = 10;
    s.highest_confidence = true;
    CHECK(select({player, other_player, head, other_head}, s)->x == other_head.x);
    auto nearby_part = head;
    nearby_part.y = 150;
    nearby_part.score = .5f;
    s.highest_confidence = false;
    CHECK(select({player, head, nearby_part}, s)->y == nearby_part.y); // Closest part of this anchor.
    s.fov_radius = 40;
    CHECK(select({player, other_player, other_head}, s)->x == player.x);
    auto outside = head;
    outside.y = 100;
    s.class_preference.max_distance = 40;
    CHECK(select({player, outside}, s)->cls == 0); // Head outside FOV does not qualify.
    auto weak_player = player;
    weak_player.score = .1f;
    CHECK(!select({weak_player, head}, s));
}

void persistent_anchor() {
    Settings s;
    s.classes = {0};
    s.class_preference = {1, "head", 1, 10};
    s.persistence = true;
    Selection select;
    CHECK(select({player}, s)->cls == 0);
    auto closer_player = other_player;
    closer_player.x = 160;
    closer_player.y = 110; // Its center is now closer to the crosshair than the locked player.
    auto closer_head = other_head;
    closer_head.x = 170;
    closer_head.y = 90;
    CHECK(select({player, closer_player, head, closer_head}, s)->x == head.x);
    CHECK(select({player, closer_player, closer_head}, s)->x == player.x);
    CHECK(select({player, closer_player, head, closer_head}, s)->x == head.x);
    CHECK(select({closer_player, closer_head}, s)->x == closer_head.x); // Missing anchor can be replaced.
    CHECK(!select({}, s));
    CHECK(select({player, head}, s)->x == head.x); // Reset does not retain the old anchor.
}

void decode_and_settings() {
    Settings s;
    s.classes = {0};
    // Two predictions with two classes; the preferred class is outside the normal filter.
    const std::vector<float> output{160, 160, 190, 130, 40, 20, 100, 20, .9f, .1f, .1f, .8f};
    CHECK(decode_yolo(output, 2, 2, 320, 320, 320, s).size() == 1);
    s.class_preference = {1, "head", 1, 10};
    const auto detections = decode_yolo(output, 2, 2, 320, 320, 320, s);
    CHECK(detections.size() == 2 && detections[1].cls == 1);
    Selection select;
    CHECK(select(detections, s)->cls == 1);
    auto loaded = settings_from_json(settings_json(s));
    CHECK(loaded.class_preference.class_id == 1 && loaded.class_preference.class_name == "head" &&
          loaded.class_preference.mode == 1 && loaded.class_preference.max_distance == 10);
    CHECK(settings_from_json(nlohmann::json{{"version", 1}}).class_preference.class_id == -1);
    reconcile_classes(loaded, {"head", "player"});
    CHECK(loaded.class_preference.class_id == 0 && loaded.class_preference.class_name == "head");
    reconcile_classes(loaded, {"player"});
    CHECK(loaded.class_preference.class_id == -1 && loaded.class_preference.class_name.empty());
    loaded.class_preference = {0, "", 0, 32};
    reconcile_classes(loaded, {"player", "head"});
    CHECK(loaded.class_preference.class_name == "player");
    loaded.class_preference = {500, "", 0, 32};
    reconcile_classes(loaded, {"player", "head"});
    CHECK(loaded.class_preference.class_id == -1);
    for (int id : {-2, 10000}) {
        loaded = s;
        loaded.class_preference.class_id = id;
        rejects([&] { validate(loaded); });
    }
    for (int mode : {-1, 2}) {
        loaded = s;
        loaded.class_preference.mode = mode;
        rejects([&] { validate(loaded); });
    }
    for (float distance :
         {-1.f, 1025.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        loaded = s;
        loaded.class_preference.max_distance = distance;
        rejects([&] { validate(loaded); });
    }
    loaded = s;
    loaded.class_preference.class_name.assign(1025, 'x');
    rejects([&] { validate(loaded); });
}
} // namespace
int main() {
    try {
        global_preference();
        proximity_preference();
        persistent_anchor();
        decode_and_settings();
        std::cout << checks
                  << " class preference, proximity, filtering, persistence and profile checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
