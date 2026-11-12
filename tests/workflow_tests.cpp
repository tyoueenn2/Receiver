#include "receiver/app.hpp"
#include "receiver/preferences.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace receiver;
namespace {
int checks = 0;
std::atomic<bool> replacement_entered{false}, replacement_allowed{false};
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x))                                                                                            \
            throw std::runtime_error("Failed: " #x);                                                         \
    } while (false)
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception&) {
        ++checks;
        return;
    }
    throw std::runtime_error("Expected rejection");
}
template <class F> void until(F predicate, int ms = 2500) {
    auto end = now_ns() + int64_t(ms) * 1'000'000;
    while (!predicate()) {
        if (now_ns() > end)
            throw std::runtime_error("Timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
using Buffer = std::vector<uint8_t>;
void varint(Buffer& b, uint64_t n) {
    while (n > 127) {
        b.push_back(uint8_t(n) | 128);
        n >>= 7;
    }
    b.push_back(uint8_t(n));
}
void integer(Buffer& b, unsigned id, uint64_t n) {
    varint(b, id << 3);
    varint(b, n);
}
void message(Buffer& b, unsigned id, Bytes data) {
    varint(b, (id << 3) | 2);
    varint(b, data.size());
    b.insert(b.end(), data.begin(), data.end());
}
void string(Buffer& b, unsigned id, const std::string& s) {
    message(b, id, Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}
Buffer value(std::initializer_list<int> dims) {
    Buffer shape;
    for (int n : dims) {
        Buffer d;
        if (n < 0)
            string(d, 2, "dynamic");
        else
            integer(d, 1, uint64_t(n));
        message(shape, 1, d);
    }
    Buffer tensor;
    integer(tensor, 1, 1);
    message(tensor, 2, shape);
    Buffer type;
    message(type, 1, tensor);
    Buffer info;
    message(info, 2, type);
    return info;
}
void metadata(Buffer& model, const std::string& key, const std::string& val) {
    Buffer entry;
    string(entry, 1, key);
    string(entry, 2, val);
    message(model, 14, entry);
}
Buffer onnx(int size = 640, const std::string& names = R"({"0":"first","1":"second"})") {
    Buffer graph;
    message(graph, 11, value({1, 3, size, size}));
    message(graph, 12, value({1, 6, 10}));
    Buffer model;
    message(model, 7, graph);
    metadata(model, "task", "detect");
    metadata(model, "names", names);
    return model;
}
void write(const std::filesystem::path& p, Bytes data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}
class FakeBackend : public Backend {
    int size_;
    std::string model_;
    std::array<float, 6> output_{160, 160, 40, 40, .9f, .1f};

  public:
    explicit FakeBackend(const Settings& s)
        : size_(s.model == "second.pt" ? 640 : s.input_size), model_(s.model) {}
    Inference run(const Frame&) override {
        return {output_, 1, 2, 0, .1};
    }
    std::string description() const override {
        return model_;
    }
    std::vector<std::string> class_names() const override {
        return model_ == "second.pt" ? std::vector<std::string>{"second", "first"}
                                     : std::vector<std::string>{"first", "second"};
    }
    int input_size() const override {
        return size_;
    }
    std::vector<int> supported_sizes() const override {
        return model_.ends_with(".engine") ? std::vector<int>{1024} : std::vector<int>{};
    }
};
std::unique_ptr<Backend> fake_factory(const Settings& s, const BackendProgress&,
                                      const BackendCancel& cancel) {
    if (s.model == "second.pt" || s.model == "fail.pt" || s.model == "stall.pt") {
        if (s.model == "second.pt")
            replacement_entered = true;
        auto end = now_ns() + 100'000'000;
        while ((s.model == "second.pt" && !replacement_allowed) ||
               (s.model != "second.pt" && now_ns() < end) || s.model == "stall.pt") {
            if (cancel && cancel())
                throw std::runtime_error("Cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (s.model == "fail.pt")
        throw std::runtime_error("Bad checkpoint");
    return std::make_unique<FakeBackend>(s);
}
void send_picture(UdpSocket& sender, const Address& to, uint32_t sequence) {
    std::array<uint8_t, 16> hello{};
    std::memcpy(hello.data(), "UVH1", 4);
    put64(hello.data() + 8, 9182);
    sender.send(hello, to);
    FrameHeader h;
    h.session = 9182;
    h.sequence = sequence;
    h.capture_ns = now_ns();
    h.width = h.height = 16;
    h.bytes = 16 * 16 * 3;
    h.count = 1;
    h.stride = 1200;
    auto head = encode_frame(h);
    Buffer packet(head.begin(), head.end());
    packet.resize(48 + h.bytes, 114);
    sender.send(packet, to);
}
void setup_workflows(const std::filesystem::path& root) {
    Preferences fresh(root / "fresh");
    fresh.load();
    auto defaults = read_json_file(fresh.setup_path());
    CHECK(defaults.at("frame_port") == 5000 && defaults.at("omni_python") == "python");
    CHECK(!defaults.contains("confidence") && !defaults.contains("model"));

    // Existing installations migrate the latest machine setup, not a model's older snapshot.
    auto directory = root / "migrated";
    Preferences old(directory);
    Settings s;
    s.model = "first.pt";
    s.sender_ip = "192.168.1.20";
    s.pi_ip = "192.168.1.30";
    s.frame_port = 5100;
    s.pi_port = 12400;
    s.makcu_port = "COM7";
    s.omni_python = "C:/Python/python.exe";
    s.omni_source = "C:/Models/omni";
    s.omni_worker = "C:/Receiver/worker.py";
    s.omni_device = "cpu";
    s.confidence = .7f;
    old.save(s, "");
    CHECK(!std::filesystem::exists(old.setup_path()));
    auto latest = s;
    latest.model = "second.pt";
    latest.sender_ip = "192.168.1.21";
    latest.omni_python = "C:/New Python/python.exe";
    old.save(latest, "");
    Preferences migrated(directory);
    migrated.load();
    auto setup = read_json_file(migrated.setup_path());
    CHECK(setup.at("sender_ip") == latest.sender_ip);
    CHECK(setup.at("omni_python") == latest.omni_python);
    auto restored = migrated.select_model(latest, "first.pt");
    CHECK(restored.sender_ip == latest.sender_ip && restored.omni_python == latest.omni_python);
    CHECK(restored.confidence == s.confidence && restored.makcu_port == "COM7");

    // Setup wins over imported profiles and model snapshots, while tuning still restores.
    auto profile = root / "imported.json";
    s.confidence = .3f;
    save_settings(s, profile.string());
    auto imported = migrated.load_profile(profile.string());
    CHECK(imported.sender_ip == latest.sender_ip && imported.omni_python == latest.omni_python);
    CHECK(imported.confidence == .3f && imported.model == "first.pt");
    CHECK(load_settings(profile.string()).sender_ip == s.sender_ip); // CLI profiles are unchanged.

    // Autosave must never overwrite edits made in a text editor, including during practice.
    setup["pi_port"] = 12500;
    setup["omni_device"] = "cuda:0";
    write_json_atomic(setup, migrated.setup_path());
    auto practice = latest;
    practice.sender_ip = practice.pi_ip = "127.0.0.1";
    migrated.save(practice, "");
    CHECK(read_json_file(migrated.setup_path()) == setup);
    CHECK(migrated.current.sender_ip == latest.sender_ip);
    auto reloaded = migrated.reload_setup(latest);
    CHECK(reloaded.pi_port == 12500 && reloaded.omni_device == "cuda:0");
    Preferences restarted(directory);
    restarted.load();
    CHECK(restarted.current.pi_port == 12500);
    CHECK(restarted.select_model(latest, "first.pt").omni_device == "cuda:0");

    // Bad files produce an error, retain the last good setup, and are never repaired silently.
    for (auto bad : {nlohmann::json{{"frame_port", 0}}, nlohmann::json{{"pi_port", 65536}},
                     nlohmann::json{{"omni_device", "invalid"}}, nlohmann::json{{"omni_python", ""}},
                     nlohmann::json{{"sender_ipv4", "192.168.1.2"}}, nlohmann::json::array()}) {
        write_json_atomic(bad, migrated.setup_path());
        rejects([&] { migrated.reload_setup(latest); });
        CHECK(migrated.apply_setup(latest).pi_port == 12500);
        rejects([&] { Preferences(directory).load(); });
        CHECK(read_json_file(migrated.setup_path()) == bad);
    }
    {
        std::ofstream file(migrated.setup_path());
        file << "{invalid json";
    }
    rejects([&] { migrated.reload_setup(latest); });
    CHECK(migrated.apply_setup(latest).pi_port == 12500);

    // A small hand-written setup is enough; omitted values use documented defaults.
    write_json_atomic({{"sender_ip", "192.168.1.22"}}, fresh.setup_path());
    fresh.load();
    CHECK(fresh.current.sender_ip == "192.168.1.22" && fresh.current.pi_port == 12345);
}
void app_workflows() {
    Settings s;
    s.model = "first.pt";
    s.bind_ip = s.sender_ip = s.pi_ip = "127.0.0.1";
    s.preview = true;
    s.frame_port = 30000 + int(random_id() % 20000);
    s.pi_port = s.frame_port + 1;
    s.max_age_ms = 250;
    s.classes = {1};
    s.selected_class_names = {"second"};
    s.class_preference = {0, "first", 1, 24};
    App app(fake_factory);
    app.start(s);
    until([&] { return app.stats().model_revision == 1 || !app.stats().error.empty(); });
    CHECK(app.stats().error.empty());
    CHECK(!app.stats().armed);
    CHECK(app.settings().input_size == 320);
    UdpSocket sender("127.0.0.1", 0);
    auto to = address("127.0.0.1", s.frame_port);
    uint32_t sequence = 1;
    until([&] {
        send_picture(sender, to, sequence++);
        return app.stats().network.completed > 0;
    });
    until([&] { return bool(app.preview().frame); });
    {
        auto view = app.settings();
        CHECK(app.preview().frame);
        const auto before = app.preview().frame->header.sequence;
        until([&] {
            send_picture(sender, to, sequence++);
            const auto picture = app.preview();
            return picture.frame && picture.frame->header.sequence > before;
        });
        CHECK(app.settings().preview);
        CHECK(app.stats().model_revision == 1 && !app.stats().armed);
        view.preview = false;
        app.configure(view);
        CHECK(!app.preview().frame);
        const auto inferred_before = app.stats().inferred;
        until([&] {
            send_picture(sender, to, sequence++);
            return app.stats().inferred > inferred_before + 3;
        });
        CHECK(!app.preview().frame); // Closing preview stops snapshots without stopping inference.
        view.preview = true;
        app.configure(view);
        until([&] {
            send_picture(sender, to, sequence++);
            return bool(app.preview().frame);
        });
    }
    auto completed = app.stats().network.completed;
    app.arm(true);
    CHECK(app.stats().armed);
    auto selected = app.settings();
    selected.model = "second.pt";
    app.configure(selected);
    until([&] { return replacement_entered.load(); });
    CHECK(app.stats().model_loading && !app.stats().armed);
    app.arm(true);
    CHECK(!app.stats().armed);
    until([&] {
        send_picture(sender, to, sequence++);
        return app.stats().network.completed > completed + 3;
    });
    CHECK(app.stats().model_loading); // Networking receives pictures while the replacement warms up.
    replacement_allowed = true;
    until([&] { return app.stats().model_revision == 2; });
    CHECK(app.stats().running && app.stats().error.empty() && !app.stats().armed);
    CHECK(app.settings().model == "second.pt" && app.settings().input_size == 640);
    CHECK(app.settings().classes == std::vector<int>{0}); // Class labels survive reordered IDs.
    CHECK(app.settings().class_preference.class_id == 1 &&
          app.settings().class_preference.class_name == "first");
    auto good = settings_json(app.settings());
    selected = app.settings();
    selected.model = "fail.pt";
    selected.confidence = .8f;
    app.configure(selected);
    until([&] { return app.stats().model_revision == 3; });
    CHECK(app.stats().running && app.stats().error.empty() && !app.stats().model_error.empty());
    CHECK(settings_json(app.settings()) == good);
    CHECK(app.stats().backend == "second.pt");
    selected = app.settings();
    selected.model = "first.pt";
    selected.input_size = 256;
    selected.auto_size = false;
    app.configure(selected);
    until([&] { return app.stats().model_revision == 4; });
    CHECK(app.settings().input_size == 256 && app.stats().model_error.empty());
    app.configure(app.settings(), true); // Reload replaced weights at the same path.
    until([&] { return app.stats().model_revision == 5; });
    CHECK(app.stats().running && app.settings().model == "first.pt");
    selected = app.settings();
    selected.inference_fps = 20;
    app.configure(selected);
    auto inferred = app.stats().inferred;
    auto end = now_ns() + 200'000'000;
    while (now_ns() < end) {
        send_picture(sender, to, sequence++);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(app.stats().inferred - inferred <= 6);
    CHECK(app.stats().network.completed > completed + 30);
    until([&] { return bool(app.preview().frame); });
    app.arm(true);
    app.benchmark(PerformanceGoal::balanced);
    CHECK(app.stats().benchmarking && !app.stats().armed);
    app.arm(true);
    CHECK(!app.stats().armed);
    app.cancel_benchmark();
    until([&] { return !app.stats().benchmarking; });
    CHECK(app.stats().running && app.settings().model == "first.pt" && app.stats().benchmark.samples.empty());
    selected = app.settings();
    selected.frame_source = "local";
    rejects([&] { app.configure(selected); });
    CHECK(app.settings().frame_source == "udp" && !app.stats().armed);
    selected = app.settings();
    selected.sender_ip = "127.0.0.2";
    rejects([&] { app.configure(selected); });
    CHECK(app.settings().sender_ip == "127.0.0.1");
    selected = app.settings();
    selected.model = "stall.pt";
    app.configure(selected);
    auto before = now_ns();
    app.stop();
    CHECK(now_ns() - before < 1'000'000'000);
    CHECK(!app.stats().model_loading && !app.stats().benchmarking && !app.stats().running);
}
} // namespace
int main() {
    auto root = std::filesystem::temp_directory_path() / ("receiver workflow " + std::to_string(now_ns()));
    try {
        std::filesystem::create_directories(root / "models");
        auto data = onnx();
        auto info = inspect_onnx(data);
        CHECK(info.fixed_size == 640 && !info.dynamic && info.names.size() == 2);
        auto python_names = onnx(320, R"({0: 'o\'bj', 1: "second"})");
        CHECK(inspect_onnx(python_names).names[0] == "o'bj");
        auto dynamic = onnx(-1);
        metadata(dynamic, "imgsz", "[512, 512]");
        CHECK(inspect_onnx(dynamic).dynamic);
        auto nms = data;
        metadata(nms, "args", "{'nms': True}");
        rejects([&] { inspect_onnx(nms); });
        nms = data;
        metadata(nms, "args", "{'nms': False}");
        CHECK(inspect_onnx(nms).names.size() == 2);
        auto raw = data;
        metadata(raw, "end2end", "False");
        CHECK(inspect_onnx(raw).metadata.at("end2end") == false);
        auto end2end = dynamic;
        metadata(end2end, "end2end", "True");
        rejects([&] { inspect_onnx(end2end); });
        rejects([&] { inspect_onnx(onnx(640, R"({"1":"missing zero"})")); });
        rejects([&] { inspect_onnx(onnx(160, R"({0: __import__('os'), 1: 'second'})")); });
        rejects([&] { inspect_onnx(onnx(33)); });
        data.pop_back();
        rejects([&] { inspect_onnx(data); });
        rejects([&] { inspect_onnx(Buffer{0x80}); });
        auto fixed_path = root / "models" / "fixed.onnx";
        write(fixed_path, onnx());
        Settings s;
        s.model = fixed_path.string();
        s.input_size = 160;
        s.classes = {999, 1};
        auto prepared = prepare_model_settings(s);
        CHECK(prepared.input_size == 640 && prepared.classes == std::vector<int>{1});
        CHECK(prepared.selected_class_names == std::vector<std::string>{"second"});
        auto dynamic_path = root / "models" / "dynamic.onnx";
        write(dynamic_path, dynamic);
        s.model = dynamic_path.string();
        s.classes.clear();
        s.selected_class_names.clear();
        CHECK(prepare_model_settings(s).input_size == 512);
        s.auto_size = false;
        CHECK(prepare_model_settings(s).input_size == 160);
        Preferences prefs(root / "preferences");
        s.model = fixed_path.string();
        s.input_size = 640;
        s.confidence = .75f;
        s.inference_fps = 80;
        s.classes = {1};
        s.class_preference = {1, "second", 1, 24};
        prefs.save(s, "", {"first", "second"});
        auto another = prefs.select_model(s, dynamic_path.string());
        CHECK(another.auto_size && another.classes.empty() && another.inference_fps == 0);
        CHECK(another.class_preference.class_id == -1);
        another.confidence = .2f;
        another.class_preference = {0, "first", 0, 70};
        another.sender_ip = "127.0.0.5";
        another.frame_source = "local";
        another.capture_output = 2;
        another.color.mode = 2;
        another.color.min_h = 100;
        another.preview = true;
        prefs.save(another, "");
        auto restored = prefs.select_model(another, fixed_path.string());
        CHECK(restored.confidence == .75f && restored.input_size == 640 && restored.inference_fps == 80);
        CHECK(restored.sender_ip == "127.0.0.5" &&
              restored.selected_class_names == std::vector<std::string>{"second"});
        CHECK(restored.frame_source == "local" && restored.capture_output == 2);
        CHECK(restored.color.mode == 2 && restored.color.min_h == 100);
        CHECK(restored.preview);
        CHECK(restored.class_preference.class_id == 1 && restored.class_preference.mode == 1 &&
              restored.class_preference.max_distance == 24);
        reconcile_classes(restored, {"second", "first"});
        CHECK(restored.classes == std::vector<int>{0});
        CHECK(restored.class_preference.class_id == 0 && restored.class_preference.class_name == "second");
        Preferences reloaded(root / "preferences");
        reloaded.load();
        CHECK(settings_json(reloaded.current) == settings_json(prefs.current));
        CHECK(reloaded.current.frame_source == "local" && reloaded.current.capture_output == 2);
        CHECK(reloaded.select_model(another, fixed_path.string()).confidence == .75f);
        auto library = reloaded.library();
        CHECK(
            std::any_of(library.begin(), library.end(), [&](const auto& e) { return e.path == fixed_path; }));
        std::filesystem::remove(fixed_path);
        library = reloaded.library();
        CHECK(std::none_of(library.begin(), library.end(),
                           [&](const auto& e) { return e.path == fixed_path; }));
        auto profile = root / "settings.json";
        save_settings(s, profile.string());
        auto invalid = s;
        invalid.inference_fps = -1;
        rejects([&] { save_settings(invalid, profile.string()); });
        CHECK(load_settings(profile.string()).inference_fps == 80);
        CHECK(settings_from_json(nlohmann::json{{"version", 1}}).auto_size);
        CHECK(settings_from_json(nlohmann::json{{"version", 1}}).frame_source == "udp");
        CHECK(settings_from_json(nlohmann::json{{"frame_source", "local"}, {"capture_output", 3}})
                  .capture_output == 3);
        rejects([&] { settings_from_json(nlohmann::json{{"frame_source", "invalid"}}); });
        rejects([&] { settings_from_json(nlohmann::json{{"capture_output", -1}}); });
        rejects([&] { settings_from_json(nlohmann::json{{"capture_output", 1025}}); });
        CHECK(!settings_from_json(nlohmann::json{{"version", 1}, {"input_size", 256}}).auto_size);
        BenchmarkReport report;
        report.samples = {{160, 10, 400, 2, 3, ""}, {320, 10, 350, 3, 4, ""}, {512, 10, 200, 5, 6, ""}};
        recommend_performance(report, PerformanceGoal::fastest, 30);
        CHECK(report.recommended_size == 160);
        recommend_performance(report, PerformanceGoal::balanced, 30);
        CHECK(report.recommended_size == 320);
        recommend_performance(report, PerformanceGoal::efficient, 30);
        CHECK(report.recommended_size == 160 && report.recommended_fps == 60);
        Frame frame;
        frame.header.width = frame.header.height = 16;
        frame.header.bytes = 768;
        s = {};
        s.model = "first.pt";
        auto measured = benchmark_model(s, frame, fake_factory, PerformanceGoal::balanced, {}, {}, {0, 2});
        CHECK(measured.samples.size() == 6);
        CHECK(std::all_of(measured.samples.begin(), measured.samples.end(),
                          [](const auto& x) { return x.frames && x.fps > 0 && x.error.empty(); }));
        rejects([&] {
            benchmark_model(s, frame, fake_factory, PerformanceGoal::fastest, {}, [] { return true; },
                            {0, 2});
        });
        auto engine = root / "test.engine";
        auto engine_meta =
            nlohmann::json{{"task", "detect"}, {"names", {"first", "second"}}, {"imgsz", {512, 512}}}.dump();
        Buffer wrapped(4 + engine_meta.size() + 8);
        for (int i = 0; i < 4; ++i)
            wrapped[i] = uint8_t(engine_meta.size() >> (8 * i));
        std::copy(engine_meta.begin(), engine_meta.end(), wrapped.begin() + 4);
        write(engine, wrapped);
        s.model = engine.string();
        auto engine_report =
            benchmark_model(s, frame, fake_factory, PerformanceGoal::fastest, {}, {}, {0, 2});
        CHECK(engine_report.samples.size() == 1 && engine_report.recommended_size == 1024);
        setup_workflows(root / "setup");
        app_workflows();
        std::filesystem::remove_all(root);
        std::cout << checks << " workflow checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
