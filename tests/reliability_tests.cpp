#include "test_support.hpp"
#include "receiver/app.hpp"
#include "receiver/engine.hpp"
#include "receiver/performance.hpp"
#include "receiver/preferences.hpp"
#include <atomic>
#include <iostream>
#include <random>
#include <thread>
using namespace receiver;
using test::rejects;
namespace {
void sample_snapshots() {
    // Windows Debug snapshots must fit the ordinary 1 MiB application stack.
    static_assert(sizeof(Stats) < 16 * 1024);
    Samples ring;
    for (int i = 0; i < 4096; ++i)
        ring.add(i);
    CHECK(ring.count == 4096 && ring.percentile(0) == 2048 && ring.percentile(1) == 4095);
    const auto snapshot = ring;
    for (int i = 0; i < 2048; ++i)
        ring.add(7);
    CHECK(ring.percentile(.5) == 7);
    CHECK(snapshot.count == 4096 && snapshot.percentile(0) == 2048 && snapshot.percentile(1) == 4095);
    auto copied = snapshot;
    copied.add(-5);
    CHECK(copied.percentile(0) == -5 && snapshot.percentile(0) == 2048);
}
void metadata_boundaries() {
    for (int size : {32, 64, 160, 256, 320, 416, 640, 1024})
        CHECK(inspect_onnx(test::onnx({1, 3, size, size})).fixed_size == size);
    for (const auto& data :
         {test::onnx({2, 3, 320, 320}), test::onnx({1, 4, 320, 320}), test::onnx({1, 3, 320, 640}),
          test::onnx({1, 3, 0, 33}), test::onnx({1, 3, 320, 320}, {1, 10, 6}),
          test::onnx({1, 3, 320, 320}, {1, 6, 10}, 10)})
        rejects([&] { inspect_onnx(data); });
    auto original = test::onnx();
    for (size_t n = 0; n < original.size(); ++n) {
        std::optional<ModelInfo> parsed;
        try {
            parsed = inspect_onnx(Bytes(original.data(), n));
        } catch (const std::exception&) {
            ++test::checks;
        }
        if (parsed)
            CHECK(parsed->fixed_size == 640);
    }
    auto duplicate = original;
    test::metadata(duplicate, "names", "[]");
    rejects([&] { inspect_onnx(duplicate); });
    auto large = original;
    test::metadata(large, "other", std::string(1024 * 1024 + 1, 'x'));
    rejects([&] { inspect_onnx(large); });
    test::Buffer graph, initializer;
    test::message(graph, 11, test::value({1, 3, 320, 320}));
    test::message(graph, 12, test::value({1, 6, 10}));
    test::string(initializer, 13, "outside.bin");
    test::message(graph, 5, initializer);
    test::Buffer external;
    test::message(external, 7, graph);
    rejects([&] { inspect_onnx(external); });
    std::mt19937 random(0x512320);
    for (int iteration = 0; iteration < 10000; ++iteration) {
        auto mutated = original;
        for (unsigned changes = 1 + random() % 4; changes; --changes)
            mutated[random() % mutated.size()] ^= uint8_t(1u << (random() % 8));
        std::optional<ModelInfo> parsed;
        try {
            parsed = inspect_onnx(mutated);
        } catch (const std::exception&) {
            ++test::checks;
        }
        if (parsed)
            CHECK(parsed->fixed_size == 0 ||
                  (parsed->fixed_size >= 32 && parsed->fixed_size <= 1024 && parsed->fixed_size % 32 == 0));
        test::Buffer packet(random() % 256);
        for (auto& byte : packet)
            byte = uint8_t(random());
        CHECK(!parse_frame(packet));
        CHECK(!parse_telemetry(packet));
    }
}
void preferences_failures() {
    test::TempDirectory temp;
    auto root = temp.path / "settings";
    Preferences prefs(root);
    Settings good;
    good.model = (temp.path / "good.pt").string();
    good.confidence = .7f;
    prefs.save(good, "good.json");
    const auto baseline = settings_json(prefs.current);
    auto file = root / "preferences.json";
    std::ifstream input(file);
    nlohmann::json saved;
    input >> saved;
    input.close();
    auto corrupted = saved;
    corrupted["settings"]["confidence"] = .2f;
    corrupted["model_directories"] = {42};
    write_json_atomic(corrupted, file);
    rejects([&] { prefs.load(); });
    CHECK(settings_json(prefs.current) == baseline && prefs.profile == "good.json");
    corrupted = saved;
    corrupted["models"] = nlohmann::json::array();
    write_json_atomic(corrupted, file);
    rejects([&] { prefs.load(); });
    CHECK(settings_json(prefs.current) == baseline);
    corrupted = saved;
    corrupted["version"] = 99;
    write_json_atomic(corrupted, file);
    rejects([&] { prefs.load(); });
    CHECK(settings_json(prefs.current) == baseline);
    write_json_atomic(saved, file);
    prefs.load();
    CHECK(settings_json(prefs.current) == baseline);
#ifdef _WIN32
    // An unrelated reader that denies replacement must not lose the last save.
    std::ifstream locked(file);
    CHECK(locked.is_open());
    auto blocked = good;
    blocked.confidence = .3f;
    rejects([&] { prefs.save(blocked, "blocked.json"); });
    CHECK(settings_json(prefs.current) == baseline && prefs.profile == "good.json");
    locked.close();
    CHECK(read_json_file(file) == saved);
#endif
    // A replacement blocked by a directory must preserve both disk and in-memory choices.
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    std::ofstream(file / "keep") << "preserve me";
    auto changed = good;
    changed.model = (temp.path / "new.pt").string();
    changed.confidence = .1f;
    rejects([&] { prefs.save(changed, "new.json"); });
    CHECK(settings_json(prefs.current) == baseline && prefs.profile == "good.json");
    CHECK(prefs.select_model(good, changed.model).confidence == good.confidence);
    CHECK(std::filesystem::is_regular_file(file / "keep"));
    for (const auto& entry : std::filesystem::directory_iterator(root))
        CHECK(!entry.path().filename().string().ends_with(".tmp"));
    auto profile = temp.path / "profile.json";
    save_settings(good, profile.string());
    std::atomic<bool> done{false}, broken{false};
    std::atomic<int> reads{0};
    std::string read_error;
    std::thread reader([&] {
        while (!done) {
            try {
                const auto s = load_settings(profile.string());
                if (s.model != good.model || s.confidence < .1f || s.confidence > .9f)
                    broken = true;
                ++reads;
            } catch (const std::exception& e) {
                read_error = e.what();
                broken = true;
            }
        }
    });
    try {
        sample_snapshots();
        for (int i = 0; i < 100; ++i) {
            auto s = good;
            s.confidence = i % 2 ? .2f : .8f;
            save_settings(s, profile.string());
        }
    } catch (...) {
        done = true;
        reader.join();
        throw;
    }
    done = true;
    reader.join();
    if (broken)
        throw std::runtime_error("Concurrent settings read failed: " + read_error);
    CHECK(reads > 0);
    for (const auto& entry : std::filesystem::directory_iterator(temp.path))
        CHECK(!entry.path().filename().string().ends_with(".tmp"));
    auto oversized = temp.path / "oversized.json";
    std::ofstream(oversized) << std::string(8 * 1024 * 1024 + 1, ' ');
    rejects([&] { read_json_file(oversized); });
    auto truncated = temp.path / "truncated.json";
    std::ofstream(truncated) << "{\"confidence\":";
    rejects([&] { load_settings(truncated.string()); });
    auto folder = temp.path / "library";
    std::filesystem::create_directory(folder);
    Preferences library(temp.path / "library-settings");
    library.add_directory(folder, true);
    library.add_directory(folder / ".", true);
    for (auto name : {"one.ONNX", "two.PT", "three.engine", "skip.txt", "skip.json"})
        std::ofstream(folder / name) << "fixture";
    std::filesystem::create_directory(folder / "nested");
    std::ofstream(folder / "nested" / "ignored.pt") << "fixture";
    const auto entries = library.library();
    CHECK(std::count_if(entries.begin(), entries.end(),
                        [&](const auto& e) { return e.path.parent_path() == folder; }) == 3);
    std::filesystem::remove(folder / "two.PT");
    const auto refreshed = library.library();
    CHECK(std::none_of(refreshed.begin(), refreshed.end(),
                       [&](const auto& e) { return e.path == folder / "two.PT"; }));
    CHECK(std::count_if(refreshed.begin(), refreshed.end(),
                        [&](const auto& e) { return e.path.parent_path() == folder; }) == 2);
}
void benchmark_failures() {
    BenchmarkReport report;
    report.samples = {{160, 0, 0, 0, 0, "bad model"}};
    rejects([&] { recommend_performance(report, PerformanceGoal::balanced, 30); });
    report.samples = {
        {160, 10, 200, 2, 4, ""}, {320, 10, 160, 4, 20, ""}, {640, 0, 0, 0, 0, "out of memory"}};
    recommend_performance(report, PerformanceGoal::balanced, 30);
    CHECK(report.recommended_size == 160 && report.recommended_fps == 160);
    recommend_performance(report, PerformanceGoal::efficient, 30);
    CHECK(report.recommended_size == 160 && report.recommended_fps == 60);
}
} // namespace
int main() {
    try {
        metadata_boundaries();
        preferences_failures();
        benchmark_failures();
        std::cout << test::checks << " reliability checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
