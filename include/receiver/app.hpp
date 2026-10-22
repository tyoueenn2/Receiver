#pragma once
#include "backend.hpp"
#include "injection.hpp"
#include "network.hpp"
#include "performance.hpp"
#include "cursor_protocol.hpp"
#include "measurement_log.hpp"
#include <nlohmann/json.hpp>
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
    bool running = false, armed = false, active = false, synchronized = false, pi_ready = false,
         calibration_only = false;
    int width = 0, height = 0;
    uint8_t physical = 0;
    double clock_uncertainty_ms = 0, frame_age_ms = 0;
    double gpu_free_mib = 0, gpu_total_mib = 0;
    bool motion_available = false;
    double mouse_speed = 0, assist_strength = 0;
    std::string status = "Stopped", backend = "Not loaded", error;
    std::vector<std::string> model_classes;
    bool model_loading = false, benchmarking = false;
    uint64_t collected_frames = 0, auto_triggered = 0;
    std::string collection_error, trigger_status;
    uint64_t model_revision = 0;
    std::string model_message, model_error;
    BenchmarkReport benchmark;
    Samples reassembly, upload, inference, postprocess, submit, receiver_total, capture_age, control_handoff;
};
struct Preview {
    std::shared_ptr<const Frame> frame;
    std::vector<Detection> detections;
    Correction correction;
    std::shared_ptr<const ColorMask> colors;
};
class App {
    struct Result {
        std::shared_ptr<const Frame> frame;
        std::vector<Detection> detections;
        uint64_t epoch = 0, id = 0;
        int64_t deadline = 0, completed_ns = 0;
        std::shared_ptr<const ColorMask> colors;
    };
    measurement::Log measurement_{"RECEIVER_MEASUREMENT_LOG", "receiver"};
    void measure_locked(const char* event, nlohmann::json fields = nlohmann::json::object());
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
    bool simulated_ = false, calibration_only_ = false;
    std::thread calibration_thread_;
    std::atomic<bool> calibration_cancel_{false};
    CalibrationStatus calibration_state_;
    move_protocol::Capabilities move_capabilities_;
    int64_t capabilities_at_ = 0, move_next_send_ = 0, move_deadline_ = 0, move_stage_at_ = 0;
    std::optional<move_protocol::Request> move_request_;
    move_protocol::Ack move_result_;
    cursor_protocol::Sample cursor_sample_;
    int64_t cursor_received_ = 0, cursor_requested_ = 0;
    std::shared_ptr<const Frame> calibration_frame_;
    bool action_latched_ = false;
    std::optional<std::pair<double, double>> move_target_, move_origin_;
    int64_t move_completed_at_ = 0;
    uint64_t submit_move_locked(int dx, int dy, int button, bool midpoint);
    void calibration_loop(int context, bool verify);
    move_protocol::Ack wait_move(uint64_t id, bool midpoint);
    void continue_move(uint64_t id);
    void cancel_move_locked(const char* reason = "local_stop");
    std::pair<double, double> observe_cursor(int64_t after);
    std::shared_ptr<const Frame> observe_frame(int64_t after);
    bool calibration_compatible_locked() const;
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
    void start(const Settings& settings, bool simulated = false, bool calibration_only = false);
    void stop();
    void arm(bool enabled);
    void configure(const Settings& settings, bool reload_model = false);
    Settings settings() const;
    void benchmark(PerformanceGoal goal);
    void cancel_benchmark();
    void start_calibration(const Settings& settings, int context, bool verify = false);
    void cancel_calibration();
    CalibrationStatus calibration_status() const;
    uint64_t move_click_return(double x, double y, bool desktop = false, bool click = true);
    void clear_calibration();
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
