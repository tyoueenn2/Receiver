#pragma once
#include "backend.hpp"
namespace receiver {
enum class PerformanceGoal { fastest, balanced, efficient };
struct BenchmarkSample {
    int size = 0;
    uint64_t frames = 0;
    double fps = 0, mean_ms = 0, p95_ms = 0;
    std::string error;
};
struct BenchmarkReport {
    std::vector<BenchmarkSample> samples;
    int recommended_size = 0, recommended_fps = 0;
};
struct BenchmarkOptions {
    int warmup_ms = 500, sample_ms = 1500;
};
BenchmarkReport benchmark_model(const Settings&, const Frame&, const BackendFactory&, PerformanceGoal,
                                const BackendProgress& = {}, const BackendCancel& = {},
                                BenchmarkOptions = {});
void recommend_performance(BenchmarkReport&, PerformanceGoal, int max_age_ms);
} // namespace receiver
