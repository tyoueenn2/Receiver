#include "receiver/app.hpp"
#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
volatile std::sig_atomic_t interrupted = 0;

void interrupt(int signal) {
    interrupted = signal;
}

enum class LogLevel { quiet = 0, error = 1, info = 2, debug = 3, trace = 4 };

LogLevel parse_log_level(std::string_view value) {
    if (value == "quiet" || value == "off" || value == "0")
        return LogLevel::quiet;
    if (value == "error" || value == "1")
        return LogLevel::error;
    if (value == "info" || value == "2")
        return LogLevel::info;
    if (value == "debug" || value == "3")
        return LogLevel::debug;
    if (value == "trace" || value == "4")
        return LogLevel::trace;
    throw std::runtime_error("Log level must be quiet, error, info, debug, or trace");
}

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::error: return "ERROR";
    case LogLevel::info: return "INFO";
    case LogLevel::debug: return "DEBUG";
    case LogLevel::trace: return "TRACE";
    case LogLevel::quiet: return "QUIET";
    }
    return "UNKNOWN";
}

class HeadlessLogger {
    LogLevel level_ = LogLevel::info;
    int64_t started_ = receiver::now_ns();

  public:
    void set_level(LogLevel level) {
        level_ = level;
    }
    bool enabled(LogLevel level) const {
        return level != LogLevel::quiet && int(level) <= int(level_);
    }
    void write(LogLevel level, const std::string& message) const {
        if (!enabled(level))
            return;
        auto& out = level == LogLevel::error ? std::cerr : std::cout;
        out << '[' << std::fixed << std::setprecision(3)
            << double(receiver::now_ns() - started_) / 1e9 << "] " << log_level_name(level) << ' '
            << message << std::endl;
    }
};

void log_summary(const HeadlessLogger& log, const receiver::Stats& s) {
    std::ostringstream line;
    line << "summary status=" << std::quoted(s.status) << " frames=" << s.inferred
         << " commands=" << s.sent << " armed=" << s.armed << " active=" << s.active
         << " pi_ready=" << s.pi_ready << " synchronized=" << s.synchronized
         << " physical=0x" << std::hex << unsigned(s.physical) << " synthetic=0x"
         << unsigned(s.injection.persistent_mask) << std::dec
         << " pending_clicks=" << s.injection.pending_clicks << " frame_age_ms="
         << std::fixed << std::setprecision(3) << s.frame_age_ms;
    log.write(LogLevel::info, line.str());

    if (log.enabled(LogLevel::debug)) {
        line.str("");
        line.clear();
        line << "network packets=" << s.network.packets << " completed=" << s.network.completed
             << " invalid=" << s.network.invalid << " duplicate=" << s.network.duplicates
             << " old=" << s.network.old << " expired=" << s.network.expired
             << " evicted=" << s.network.evicted << " pool_drops=" << s.network.pool_drops
             << " telemetry_rejected=" << s.telemetry_rejected;
        log.write(LogLevel::debug, line.str());

        line.str("");
        line.clear();
        line << "output synthetic_snapshots=" << s.synthetic_snapshots
             << " release_snapshots=" << s.release_snapshots
             << " hold_heartbeats=" << s.hold_heartbeats << " stale=" << s.stale
             << " superseded=" << s.replaced << " click_submitted=" << s.injection.click_submitted
             << " click_accepted=" << s.injection.click_accepted
             << " click_completed=" << s.injection.click_completed
             << " click_retries=" << s.injection.click_retries
             << " release_retries=" << s.injection.release_retries;
        log.write(LogLevel::debug, line.str());
    }

    if (log.enabled(LogLevel::trace)) {
        line.str("");
        line.clear();
        line << std::fixed << std::setprecision(3) << "latency_ms reassembly_p95="
             << s.reassembly.percentile(.95) << " inference_p95=" << s.inference.percentile(.95)
             << " handoff_p95=" << s.control_handoff.percentile(.95)
             << " submission_p95=" << s.submit.percentile(.95)
             << " total_p95=" << s.receiver_total.percentile(.95)
             << " clock_uncertainty=" << s.clock_uncertainty_ms << " mouse_speed=" << s.mouse_speed
             << " assist_strength=" << s.assist_strength << " gpu_free_mib=" << s.gpu_free_mib
             << " gpu_total_mib=" << s.gpu_total_mib;
        log.write(LogLevel::trace, line.str());
    }
}
} // namespace

int main(int argc, char** argv) {
    HeadlessLogger log;
    try {
        receiver::Settings cfg;
        bool simulate = false, arm = false;
        int seconds = 10, log_interval_ms = 1000;
        int hold_button = 0, release_after_ms = -1;
        LogLevel log_level = LogLevel::info;
        struct ClickOptions { int button = 0, count = 1, press_ms = 10, interval_ms = 10; } click;
        std::string metrics = "metrics.csv", profile = "<defaults>";
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("Missing value for " + a);
                return std::string(argv[i]);
            };
            if (a == "--profile") {
                profile = value();
                cfg = receiver::load_settings(profile);
            } else if (a == "--simulate")
                simulate = true;
            else if (a == "--arm")
                arm = true;
            else if (a == "--seconds")
                seconds = std::stoi(value());
            else if (a == "--metrics")
                metrics = value();
            else if (a == "--log-level")
                log_level = parse_log_level(value());
            else if (a == "--log-interval-ms")
                log_interval_ms = std::stoi(value());
            else if (a == "--quiet")
                log_level = LogLevel::quiet;
            else if (a == "--verbose")
                log_level = LogLevel::debug;
            else if (a == "--trace")
                log_level = LogLevel::trace;
            else if (a == "--hold-button")
                hold_button = std::stoi(value());
            else if (a == "--release-after-ms")
                release_after_ms = std::stoi(value());
            else if (a == "--click") {
                click.button = std::stoi(value());
                click.count = std::stoi(value());
                click.press_ms = std::stoi(value());
                click.interval_ms = std::stoi(value());
            } else if (a == "--help") {
                std::cout
                    << "receiver_headless [--profile FILE] [--simulate] [--arm] [--seconds N]\n"
                       "  [--metrics FILE] [--log-level quiet|error|info|debug|trace]\n"
                       "  [--log-interval-ms N] [--quiet|--verbose|--trace]\n"
                       "  [--hold-button 1..8] [--release-after-ms N]\n"
                       "  [--click BUTTON COUNT PRESS_MS INTERVAL_MS]\n\n"
                       "Use --seconds 0 to run until SIGINT or SIGTERM. A zero log interval disables\n"
                       "periodic summaries; lifecycle and action logs still follow --log-level.\n"
                       "Simulation is loopback-only. Synthetic input requires --arm and fresh\n"
                       "sender/Pi telemetry.\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option: " + a);
        }
        log.set_level(log_level);
        if (seconds < 0 || seconds > 86400)
            throw std::runtime_error("Seconds must be 0..86400 (0 runs until interrupted)");
        if (log_interval_ms < 0 || log_interval_ms > 60000)
            throw std::runtime_error("Log interval must be 0..60000 milliseconds");
        if (hold_button < 0 || hold_button > 8 || release_after_ms < -1 ||
            click.button < 0 || click.button > 8 || click.count < 1 || click.count > 10000 ||
            click.press_ms < 1 || click.press_ms > 5000 || click.interval_ms < 0 ||
            click.interval_ms > 60000)
            throw std::runtime_error("Invalid synthetic button or click option");
        std::signal(SIGINT, interrupt);
        std::signal(SIGTERM, interrupt);

        {
            std::ostringstream line;
            line << "starting mode=" << (simulate ? "simulation" : "tensorrt")
                 << " profile=" << std::quoted(profile) << " armed=" << arm << " duration_s=";
            if (seconds)
                line << seconds;
            else
                line << "unlimited";
            line << " metrics=" << std::quoted(metrics) << " log_interval_ms=" << log_interval_ms;
            log.write(LogLevel::info, line.str());
        }
        {
            std::ostringstream line;
            line << "configuration bind=" << cfg.bind_ip << ':' << cfg.frame_port
                 << " sender=" << cfg.sender_ip << " pi=" << cfg.pi_ip << ':' << cfg.pi_port
                 << " model=" << std::quoted(cfg.model) << " input=" << cfg.input_size
                 << " max_age_ms=" << cfg.max_age_ms << " mouse_backend=" << cfg.mouse_backend;
            log.write(LogLevel::debug, line.str());
        }

        receiver::App app;
        app.start(cfg, simulate);
        if (arm) {
            app.arm(true);
            log.write(LogLevel::info, "arming requested");
        }
        auto start = receiver::now_ns();
        int64_t reported = 0, hold_started = 0;
        bool hold_sent = false, click_sent = false, released = false, observed_state = false,
             application_stopped = false;
        bool last_running = false, last_armed = false, last_active = false, last_pi_ready = false,
             last_synchronized = false;
        std::string last_backend;
        while (!interrupted &&
               (!seconds || receiver::now_ns() - start < int64_t(seconds) * 1'000'000'000)) {
            auto s = app.stats();
            if (!observed_state || s.running != last_running || s.armed != last_armed ||
                s.active != last_active || s.pi_ready != last_pi_ready ||
                s.synchronized != last_synchronized || s.backend != last_backend) {
                std::ostringstream line;
                line << "state status=" << std::quoted(s.status) << " backend=" << std::quoted(s.backend)
                     << " running=" << s.running << " armed=" << s.armed << " active=" << s.active
                     << " pi_ready=" << s.pi_ready << " synchronized=" << s.synchronized;
                log.write(LogLevel::info, line.str());
                observed_state = true;
                last_running = s.running;
                last_armed = s.armed;
                last_active = s.active;
                last_pi_ready = s.pi_ready;
                last_synchronized = s.synchronized;
                last_backend = s.backend;
            }
            if (!s.running || !s.error.empty()) {
                application_stopped = true;
                break;
            }
            auto now = receiver::now_ns();
            if (log_interval_ms &&
                (!reported || now - reported >= int64_t(log_interval_ms) * 1'000'000)) {
                log_summary(log, s);
                reported = now;
            }
            if (arm && s.pi_ready && s.synchronized && !hold_sent && hold_button) {
                app.button_down(hold_button);
                hold_sent = true;
                hold_started = receiver::now_ns();
                log.write(LogLevel::info, "synthetic hold established button=" +
                                              std::to_string(hold_button));
            }
            if (arm && s.pi_ready && s.synchronized && s.injection.has_upt3 &&
                s.injection.endpoint_poll_us && !click_sent && click.button) {
                const auto id = app.click(click.button, uint32_t(click.count),
                                          std::chrono::milliseconds(click.press_ms),
                                          std::chrono::milliseconds(click.interval_ms));
                click_sent = true;
                log.write(LogLevel::info,
                          "click scheduled id=" + std::to_string(id) +
                              " button=" + std::to_string(click.button) +
                              " count=" + std::to_string(click.count));
            }
            // When paired with --hold-button, measure the release delay from the actual
            // press rather than process startup. Pi discovery and clock synchronization
            // can be slow on a loaded CI runner and must not shorten the requested hold.
            const auto release_base = hold_button ? hold_started : start;
            if (!released && release_after_ms >= 0 && release_base &&
                receiver::now_ns() - release_base >= int64_t(release_after_ms) * 1'000'000) {
                app.release_all();
                released = true;
                log.write(LogLevel::info, "release-all requested");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (interrupted)
            log.write(LogLevel::info, "shutdown requested by signal=" +
                                          std::to_string(int(interrupted)));
        else if (application_stopped)
            log.write(LogLevel::info, "application worker requested shutdown");
        else if (seconds)
            log.write(LogLevel::info, "configured run duration completed");
        app.stop();
        app.export_metrics(metrics);
        auto s = app.stats();
        log_summary(log, s);
        log.write(LogLevel::info, "metrics written path=" + metrics);
        if (!s.error.empty()) {
            log.write(LogLevel::error, s.error);
            return 1;
        }
        log.write(LogLevel::info, "stopped cleanly");
        return 0;
    } catch (const std::exception& e) {
        log.write(LogLevel::error, e.what());
        return 1;
    }
}
