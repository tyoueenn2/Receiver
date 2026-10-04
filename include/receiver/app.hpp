#pragma once
#include "backend.hpp"
#include "injection.hpp"
#include "network.hpp"
#include "performance.hpp"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
namespace receiver {
struct Samples {
    // Snapshots contain eight rings. Heap storage avoids large Debug stack frames.
    std::vector<double> values = std::vector<double>(2048);
    uint64_t count = 0;
    void add(double v) {
        values[count++ % values.size()] = v;
    }
    double percentile(double p) const;
};
struct Stats {
    ReassemblyStats network;
    InjectionMetrics injection;
    uint64_t inferred = 0, sent = 0, synthetic_snapshots = 0, release_snapshots = 0, hold_heartbeats = 0,
             replaced = 0, stale = 0, telemetry_rejected = 0;
    bool running = false, armed = false, active = false, synchronized = false, pi_ready = false;
    int width = 0, height = 0;
    uint8_t physical = 0;
    double clock_uncertainty_ms = 0, frame_age_ms = 0;
    double gpu_free_mib = 0, gpu_total_mib = 0;
    bool motion_available = false;
    double mouse_speed = 0, assist_strength = 0;
    std::string status = "Stopped", backend = "Not loaded", error;
    std::vector<std::string> model_classes;
    bool model_loading = false, benchmarking = false;
    uint64_t model_revision = 0;
    std::string model_message, model_error;
    BenchmarkReport benchmark;
    Samples reassembly, upload, inference, postprocess, submit, receiver_total, capture_age, control_handoff;
};
struct Preview {
    std::shared_ptr<const Frame> frame;
    std::vector<Detection> detections;
    Correction correction;
};
class App {
    struct Result {
        std::shared_ptr<const Frame> frame;
        std::vector<Detection> detections;
        uint64_t epoch = 0, id = 0;
        int64_t deadline = 0, completed_ns = 0;
    };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    ControlWake control_wake_;
    InjectedButtonManager injected_;
    Settings settings_;
    Stats stats_;
    std::atomic<bool> stop_{true};
    std::atomic<int64_t> sender_seen_{0};
    std::thread receive_thread_, inference_thread_, pi_thread_;
    std::shared_ptr<const Frame> latest_;
    std::optional<Result> result_;
    Preview preview_;
    ClockSync clock_;
    uint64_t epoch_ = 0, result_id_ = 0;
    bool simulated_ = false;
    BackendFactory backend_factory_;
    std::optional<Settings> pending_model_;
    std::optional<PerformanceGoal> pending_benchmark_;
    std::atomic<bool> benchmark_cancel_{false};
    void receive_loop();
    void inference_loop();
    void pi_loop();
    void fail(const std::string& error, ReleaseReason reason);

  public:
    explicit App(BackendFactory factory = make_backend);
    ~App() {
        stop();
    }
    void start(const Settings& settings, bool simulated = false);
    void stop();
    void arm(bool enabled);
    void configure(const Settings& settings, bool reload_model = false);
    Settings settings() const;
    void benchmark(PerformanceGoal goal);
    void cancel_benchmark();
    void button_down(int button);
    void button_up(int button);
    void set_button(int button, bool state);
    void release_all();
    InjectedButtonManager::Hold hold(int button);
    uint64_t click(int button, uint32_t count = 1,
                   std::chrono::microseconds press_duration = std::chrono::milliseconds(10),
                   std::chrono::microseconds interval = std::chrono::milliseconds(10));
    Stats stats() const;
    Preview preview() const;
    void export_metrics(const std::string& path) const;
};
} // namespace receiver
