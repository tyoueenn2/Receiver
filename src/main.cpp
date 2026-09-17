#include "receiver/app.hpp"
#include <atomic>
#include <csignal>
#include <iostream>
static volatile std::sig_atomic_t interrupted = 0;
static void interrupt(int) {
    interrupted = 1;
}
int main(int argc, char** argv) {
    try {
        receiver::Settings cfg;
        bool simulate = false, arm = false;
        int seconds = 10;
        int hold_button = 0, release_after_ms = -1;
        struct ClickOptions { int button = 0, count = 1, press_ms = 10, interval_ms = 10; } click;
        std::string metrics = "metrics.csv";
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("Missing value for " + a);
                return std::string(argv[i]);
            };
            if (a == "--profile")
                cfg = receiver::load_settings(value());
            else if (a == "--simulate")
                simulate = true;
            else if (a == "--arm")
                arm = true;
            else if (a == "--seconds")
                seconds = std::stoi(value());
            else if (a == "--metrics")
                metrics = value();
            else if (a == "--hold-button")
                hold_button = std::stoi(value());
            else if (a == "--release-after-ms")
                release_after_ms = std::stoi(value());
            else if (a == "--click") {
                click.button = std::stoi(value());
                click.count = std::stoi(value());
                click.press_ms = std::stoi(value());
                click.interval_ms = std::stoi(value());
            }
            else if (a == "--help") {
                std::cout << "receiver_headless [--profile FILE] [--simulate] [--arm] [--seconds N] "
                             "[--metrics FILE] [--hold-button 1..8] [--release-after-ms N] "
                             "[--click BUTTON COUNT PRESS_MS INTERVAL_MS]\nSimulation is loopback-only. "
                             "Synthetic input requires --arm and fresh sender/Pi telemetry.\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option: " + a);
        }
        if (seconds < 1 || seconds > 86400)
            throw std::runtime_error("Seconds must be 1..86400");
        if (hold_button < 0 || hold_button > 8 || release_after_ms < -1 ||
            click.button < 0 || click.button > 8 || click.count < 1 || click.count > 10000 ||
            click.press_ms < 1 || click.press_ms > 5000 || click.interval_ms < 0 ||
            click.interval_ms > 60000)
            throw std::runtime_error("Invalid synthetic button or click option");
        std::signal(SIGINT, interrupt);
        receiver::App app;
        app.start(cfg, simulate);
        if (arm)
            app.arm(true);
        auto start = receiver::now_ns();
        int64_t reported = 0;
        bool hold_sent = false, click_sent = false, released = false;
        while (!interrupted && receiver::now_ns() - start < int64_t(seconds) * 1'000'000'000) {
            auto s = app.stats();
            if (!s.running || !s.error.empty())
                break;
            if (receiver::now_ns() - reported >= 1'000'000'000) {
                std::cout << s.status << " | frames=" << s.inferred << " sent=" << s.sent
                          << " synthetic=0x" << std::hex << unsigned(s.injection.persistent_mask) << std::dec
                          << " clicks=" << s.injection.pending_clicks << " sync=" << s.synchronized
                          << " age_ms=" << s.frame_age_ms << std::endl;
                reported = receiver::now_ns();
            }
            if (arm && s.pi_ready && s.synchronized && !hold_sent && hold_button) {
                app.button_down(hold_button);
                hold_sent = true;
            }
            if (arm && s.pi_ready && s.synchronized && s.injection.has_upt3 &&
                s.injection.endpoint_poll_us && !click_sent && click.button) {
                app.click(click.button, uint32_t(click.count), std::chrono::milliseconds(click.press_ms),
                          std::chrono::milliseconds(click.interval_ms));
                click_sent = true;
            }
            if (!released && release_after_ms >= 0 &&
                receiver::now_ns() - start >= int64_t(release_after_ms) * 1'000'000) {
                app.release_all();
                released = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        app.stop();
        app.export_metrics(metrics);
        auto s = app.stats();
        if (!s.error.empty()) {
            std::cerr << s.error << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
