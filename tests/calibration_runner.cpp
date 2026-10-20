#include "receiver/app.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    receiver::App app;
    nlohmann::json report;
    try {
        auto config = receiver::load_settings(argv[1]);
        const std::string mode = argv[3];
        const int context = mode == "view" ? 1 : 0;
        app.start_calibration(config, context, mode == "manual");
        const auto started = receiver::now_ns();
        while (app.calibration_status().running && receiver::now_ns() - started < 25'000'000'000ll) {
            if (mode == "cancel" && app.calibration_status().completed >= 1)
                app.cancel_calibration();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto status = app.calibration_status();
        auto record = app.settings().calibration;
        report = {{"verified", record.verified},
                  {"error", status.error},
                  {"xx", record.xx},
                  {"yy", record.yy},
                  {"range", record.max_counts},
                  {"completed", status.completed},
                  {"armed", app.stats().armed},
                  {"release_reason", app.stats().injection.last_release_reason}};
        if (record.verified && status.error.empty() && (mode == "desktop" || mode == "miss")) {
            app.move_click_return(968, 549, true, true);
            const auto deadline = receiver::now_ns() + 3'000'000'000ll;
            while (app.calibration_status().operation_active && receiver::now_ns() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto result = app.calibration_status();
            report["operation"] = result.operation;
            report["operation_error"] = result.error;
            report["action_status"] = uint8_t(result.action.status);
            report["action_flags"] = result.action.flags;
        }
        app.stop();
    } catch (const std::exception& e) {
        report["exception"] = e.what();
    }
    std::ofstream(argv[2]) << report.dump(2) << '\n';
    return report.contains("exception") ? 1 : 0;
}
