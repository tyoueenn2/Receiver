#include "receiver/backend.hpp"
#include "receiver/model.hpp"
#include "receiver/network.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace receiver;
namespace {
std::shared_ptr<const Frame> make_frame(uint16_t width, uint16_t height, uint8_t format) {
    auto frame = std::make_shared<Frame>();
    auto& header = frame->header;
    header.width = width;
    header.height = height;
    header.format = format;
    header.bytes = uint32_t(width) * height * (format == 1 ? 3 : 4);
    std::mt19937 random(width + height + format);
    std::generate_n(frame->pixels.begin(), header.bytes, [&] { return uint8_t(random()); });
    return frame;
}
void require_equal(const Inference& actual, const std::vector<float>& expected) {
    if (!std::ranges::equal(actual.output, expected))
        throw std::runtime_error("Pipelined predictions differ from the synchronous output");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("Usage: receiver_gpu_pipeline_check MODEL.onnx");
        Settings settings;
        settings.model = argv[1];
        settings.input_size = 640;
        settings.auto_size = false;
        settings.tensorrt_pipeline = true;
        settings = prepare_model_settings(settings);
        ControlWake wake;
        auto backend = make_backend(settings);
        if (backend->input_size() != 640 || !backend->pipeline())
            throw std::runtime_error("Smoke check needs a TensorRT pipeline at 640x640");
        auto& pipeline = *backend->pipeline();
        pipeline.set_wake([&] { wake.notify(); });

        const auto first = make_frame(640, 640, 2);
        const auto second = make_frame(480, 320, 1);
        auto sync_first = backend->run(*first);
        const std::vector<float> expected_first(sync_first.output.begin(), sync_first.output.end());
        auto sync_second = backend->run(*second);
        const std::vector<float> expected_second(sync_second.output.begin(), sync_second.output.end());
        if (expected_first.empty() || expected_first == expected_second)
            throw std::runtime_error("Smoke inputs did not produce distinct predictions");
        auto take_ready = [&] {
            const auto deadline = now_ns() + 10'000'000'000LL;
            while (!pipeline.ready()) {
                if (now_ns() >= deadline)
                    throw std::runtime_error("Timed out waiting for TensorRT");
                wake.wait(nullptr, false, deadline);
            }
            return pipeline.take();
        };
        for (int i = 0; i < 4; ++i) {
            pipeline.submit(first);
            pipeline.submit(second);
            require_equal(take_ready(), expected_first);
            require_equal(take_ready(), expected_second);
        }
        pipeline.submit(second);
        pipeline.discard();
        pipeline.submit(first);
        require_equal(take_ready(), expected_first);

        std::vector<double> samples;
        for (int i = 0; i < 100; ++i) {
            const auto start = now_ns();
            auto result = backend->run(*first);
            decode_yolo(result.output, result.candidates, result.classes, 640, 640, 640, settings);
            samples.push_back(double(now_ns() - start) / 1e6);
        }
        std::sort(samples.begin(), samples.end());
        const double p95 = samples[size_t(std::ceil(.95 * (samples.size() - 1)))];
        const auto start = now_ns();
        for (int i = 0; i < 100; ++i) {
            if (i == 0)
                pipeline.submit(first);
            if (i + 1 < 100)
                pipeline.submit(first);
            auto result = take_ready();
            decode_yolo(result.output, result.candidates, result.classes, 640, 640, 640, settings);
        }
        const auto throughput = 100e9 / double(now_ns() - start);
        std::cout << backend->description() << "\n"
                  << "640x640 synchronous model, transfer and decode p95: " << p95 << " ms\n"
                  << "640x640 pipelined model, transfer and decode: " << throughput << " FPS\n";
        if (const auto* limit = std::getenv("RECEIVER_GPU_MAX_P95_MS"); limit && *limit) {
            size_t used = 0;
            const double budget = std::stod(limit, &used);
            if (limit[used] || !std::isfinite(budget) || budget <= 0)
                throw std::runtime_error("RECEIVER_GPU_MAX_P95_MS must be a positive finite number");
            if (p95 > budget)
                throw std::runtime_error("Synchronous model latency exceeded its p95 budget");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
