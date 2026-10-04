#include "receiver/backend.hpp"
#include "receiver/engine.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace receiver;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error("Check failed: " #x); } while (false)
template<class F> void rejects(F f, const std::string& expected) {
    try { f(); } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find(expected) != std::string::npos);
        return;
    }
    throw std::runtime_error("Expected rejection: " + expected);
}
int main(int argc, char** argv) {
    std::filesystem::path temp;
    try {
        Settings s;
        s.model = "renamed model.ONNX";
        CHECK(select_backend(s) == "tensorrt");
        s.model = "no family in name.PT";
        CHECK(select_backend(s) == "yolo_omni");
        s.inference_backend = "tensorrt";
        rejects([&] { select_backend(s); }, "requires");
        s.inference_backend = "yolo_omni";
        CHECK(select_backend(s) == "yolo_omni");
        s.model = "omni.onnx";
        rejects([&] { select_backend(s); }, "requires");
        s.inference_backend = "auto";
        s.model = "model.engine";
        CHECK(select_backend(s) == "tensorrt");
        s.model = "RENAMED.ENGINE";
        CHECK(select_backend(s) == "tensorrt");
        s.inference_backend = "yolo_omni";
        rejects([&] { select_backend(s); }, "requires");
        s.inference_backend = "auto";
        s.model = "model.bin";
        rejects([&] { select_backend(s); }, "Choose");
        s.inference_backend = "bogus";
        rejects([&] { validate(s); }, "backend");
        s = {};
        nlohmann::json meta{{"task", "detect"}, {"names", {{"0", "person"}, {"1", "object"}}},
                            {"args", {{"nms", false}}}, {"version", "8.3.199"}};
        auto text = meta.dump();
        std::vector<uint8_t> wrapped(4 + text.size() + 8);
        for (int i = 0; i < 4; ++i) wrapped[i] = uint8_t(text.size() >> (8 * i));
        std::copy(text.begin(), text.end(), wrapped.begin() + 4);
        auto payload = inspect_engine_payload(wrapped);
        CHECK(payload.offset == text.size() + 4 && payload.metadata == meta);
        CHECK(engine_class_names(payload.metadata, "hash") == std::vector<std::string>({"person", "object"}));
        auto raw = std::array<uint8_t, 8>{0x66, 0x74, 0x72, 0x74, 0, 0, 0, 0};
        CHECK(inspect_engine_payload(raw).offset == 0);
        rejects([&] { engine_class_names(nlohmann::json{}, "hash"); }, "class metadata");
        wrapped.resize(8);
        rejects([&] { inspect_engine_payload(wrapped); }, "truncated");
        meta["args"]["nms"] = true;
        rejects([&] { engine_class_names(meta, "hash"); }, "NMS");
        meta["args"]["nms"] = false;
        meta["task"] = "pose";
        rejects([&] { engine_class_names(meta, "hash"); }, "detection");
        meta["task"] = "detect";
        meta["sha256"] = "other";
        rejects([&] { engine_class_names(meta, "hash"); }, "SHA-256");
        meta["sha256"] = "hash";
        CHECK(engine_class_names(meta, "hash").size() == 2);
        std::array<int64_t, 4> in{1, 3, 320, 320};
        std::array<int64_t, 3> out{1, 84, 2100};
        CHECK(validate_engine_shape(in, out, 320, 80) == 2100);
        rejects([&] { validate_engine_shape(in, out, 160, 80); }, "processing size");
        out = {1, 300, 6};
        rejects([&] { validate_engine_shape(in, out, 320, 80); }, "raw");
        out = {1, 2100, 84};
        rejects([&] { validate_engine_shape(in, out, 320, 80); }, "transposed");
        out = {1, 10004, 100000};
        rejects([&] { validate_engine_shape(in, out, 320, 10000); }, "raw");
        temp = std::filesystem::temp_directory_path() / ("receiver omni tests " + std::to_string(now_ns()));
        std::filesystem::create_directories(temp);
        auto profile = temp / "legacy.json";
        { std::ofstream f(profile); f << "{\"version\":1}"; }
        CHECK(load_settings(profile.string()).inference_backend == "auto");
        s.omni_python = "python path with spaces";
        s.omni_source = "source path";
        s.omni_device = "cpu";
        s.inference_backend = "yolo_omni";
        save_settings(s, profile.string());
        auto loaded = load_settings(profile.string());
        CHECK(loaded.omni_python == s.omni_python && loaded.omni_source == s.omni_source &&
              loaded.omni_device == "cpu" && loaded.inference_backend == "yolo_omni");
        if (argc == 3) {
            s = {};
            s.model = (temp / "custom weights.pt").string();
            s.omni_python = argv[1];
            s.omni_worker = argv[2];
            s.omni_device = "cpu";
            auto scenario = [&](const char* value) { std::ofstream(s.model) << value; };
            scenario("ok");
            auto backend = make_backend(s);
            CHECK(backend->description() == "YOLO-Omni test fixture");
            CHECK(backend->class_names() == std::vector<std::string>({"object", "other"}));
            Frame frame;
            frame.header.width = 320;
            frame.header.height = 160;
            frame.header.bytes = 320 * 160 * 3;
            frame.pixels[0] = 12; frame.pixels[1] = 34; frame.pixels[2] = 56;
            auto result = backend->run(frame);
            CHECK(result.candidates == 1 && result.classes == 2 && result.output.size() == 6);
            auto detections = decode_yolo(result.output, result.candidates, result.classes, 320, 160, 320, s);
            CHECK(detections.size() == 1 && detections[0].cls == 0);
            CHECK(std::abs(detections[0].x - 140) < .01f && std::abs(detections[0].y - 40) < .01f);
            frame.header.format = 2;
            frame.header.bytes = 320 * 160 * 4;
            frame.pixels[0] = 56; frame.pixels[1] = 34; frame.pixels[2] = 12;
            CHECK(backend->run(frame).candidates == 1); // Same persistent process; BGRA transport.
            backend.reset();
            scenario("load_error");
            rejects([&] { make_backend(s); }, "checkpoint load failed");
            scenario("bad_shape");
            backend = make_backend(s);
            rejects([&] { backend->run(frame); }, "output shape");
            backend.reset();
            scenario("ok");
            s.classes = {2};
            rejects([&] { make_backend(s); }, "absent from model");
            s.classes.clear();
            scenario("stall");
            auto start = now_ns();
            rejects([&] { make_backend(s, {}, [&] { return now_ns() - start > 200'000'000; }); }, "cancelled");
            CHECK(now_ns() - start < 2'000'000'000);
        }
        std::filesystem::remove_all(temp);
        std::cout << checks << " backend checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        if (!temp.empty()) std::filesystem::remove_all(temp);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
