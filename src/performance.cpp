#include "receiver/performance.hpp"
#include "receiver/model.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <stdexcept>
namespace receiver {
void recommend_performance(BenchmarkReport& report, PerformanceGoal goal, int max_age_ms) {
    const BenchmarkSample* fastest = nullptr;
    for (const auto& s : report.samples)
        if (s.frames && s.error.empty() && s.fps > 0 && (!fastest || s.fps > fastest->fps))
            fastest = &s;
    if (!fastest)
        throw std::runtime_error("No model size completed a benchmark");
    auto chosen = fastest;
    for (const auto& s : report.samples) {
        if (!s.frames || !s.error.empty() || s.fps <= 0)
            continue;
        if (goal == PerformanceGoal::balanced && s.fps >= fastest->fps * .7 && s.p95_ms < max_age_ms * .5 &&
            s.size > chosen->size)
            chosen = &s;
        if (goal == PerformanceGoal::efficient && s.size < chosen->size)
            chosen = &s;
    }
    report.recommended_size = chosen->size;
    double cap = chosen->fps * .8;
    if (goal == PerformanceGoal::efficient)
        cap = std::min(cap, 60.);
    report.recommended_fps = std::clamp(int(std::floor(std::min(cap, 240.))), 1, 240);
}
BenchmarkReport benchmark_model(const Settings& original, const Frame& frame, const BackendFactory& factory,
                                PerformanceGoal goal, const BackendProgress& progress,
                                const BackendCancel& cancelled, BenchmarkOptions options) {
    if (options.warmup_ms < 0 || options.sample_ms < 1)
        throw std::runtime_error("Invalid benchmark duration");
    auto info = inspect_model(original);
    std::vector<int> sizes =
        info.fixed_size ? std::vector<int>{info.fixed_size} : std::vector<int>{160, 256, 320, 416, 512, 640};
    auto extension = std::filesystem::path(original.model).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension == ".engine") {
        // Serialized engines expose their real optimization profiles only after deserialization.
        auto probe = factory(original, progress, cancelled);
        auto supported = probe->supported_sizes();
        if (!supported.empty())
            sizes = std::move(supported);
    }
    BenchmarkReport report;
    for (int size : sizes) {
        if (cancelled && cancelled())
            throw std::runtime_error("Benchmark cancelled");
        BenchmarkSample sample;
        sample.size = size;
        try {
            auto settings = original;
            settings.input_size = size;
            settings.auto_size = false;
            settings.classes.clear();
            settings.selected_class_names.clear();
            if (progress)
                progress("Preparing " + std::to_string(size) + " pixel model");
            auto backend = factory(settings, progress, cancelled);
            int effective = backend->input_size();
            if (effective && effective != size)
                throw std::runtime_error("Model uses a different fixed input size");
            if (progress)
                progress("Warming " + std::to_string(size) + " pixel model");
            auto warm_end = now_ns() + int64_t(options.warmup_ms) * 1'000'000;
            do {
                if (cancelled && cancelled())
                    throw std::runtime_error("Benchmark cancelled");
                backend->run(frame);
            } while (now_ns() < warm_end);
            if (progress)
                progress("Measuring " + std::to_string(size) + " pixel model");
            std::vector<double> times;
            double total_ms = 0;
            auto start = now_ns(), end = start + int64_t(options.sample_ms) * 1'000'000;
            do {
                if (cancelled && cancelled())
                    throw std::runtime_error("Benchmark cancelled");
                auto begin = now_ns();
                auto output = backend->run(frame);
                decode_yolo(output.output, output.candidates, output.classes, frame.header.width,
                            frame.header.height, size, settings);
                auto elapsed = double(now_ns() - begin) / 1e6;
                total_ms += elapsed;
                ++sample.frames;
                if (times.size() < 100000)
                    times.push_back(elapsed);
            } while (now_ns() < end);
            sample.fps = double(sample.frames) * 1e9 / double(now_ns() - start);
            sample.mean_ms = total_ms / double(sample.frames);
            std::sort(times.begin(), times.end());
            sample.p95_ms = times[size_t(std::ceil(.95 * (times.size() - 1)))];
        } catch (const std::exception& e) {
            if (cancelled && cancelled())
                throw;
            sample.error = e.what();
        }
        report.samples.push_back(std::move(sample));
    }
    recommend_performance(report, goal, original.max_age_ms);
    return report;
}
} // namespace receiver
