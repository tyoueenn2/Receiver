// Test-only JSON control adapter around the production App. All peers are loopback.
#include "receiver/app.hpp"
#include "receiver/preferences.hpp"
#include <iostream>
using namespace receiver;
using nlohmann::json;
namespace {
void loopback(const Settings& s) {
    if (s.bind_ip != "127.0.0.1" || s.sender_ip != "127.0.0.1" || s.pi_ip != "127.0.0.1" ||
        s.mouse_backend != 0)
        throw std::runtime_error("Scenario tests require loopback peers and the UDP mouse backend");
}
json snapshot(App& app) {
    const auto s = app.stats();
    return {{"running", s.running},
            {"armed", s.armed},
            {"active", s.active},
            {"pi_ready", s.pi_ready},
            {"synchronized", s.synchronized},
            {"loading", s.model_loading},
            {"benchmarking", s.benchmarking},
            {"revision", s.model_revision},
            {"error", s.error},
            {"model_error", s.model_error},
            {"backend", s.backend},
            {"classes", s.model_classes},
            {"settings", settings_json(app.settings())},
            {"inferred", s.inferred},
            {"completed", s.network.completed},
            {"invalid", s.network.invalid},
            {"stale", s.stale},
            {"sent", s.sent},
            {"persistent", s.injection.persistent_mask},
            {"release_state", s.injection.last_release_all_state},
            {"release_reason", s.injection.last_release_reason},
            {"click_completed", s.injection.click_completed},
            {"release_retries", s.injection.release_retries}};
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("scenario_runner PROFILE");
        App app;
        auto initial = load_settings(argv[1]);
        loopback(initial);
        app.start(initial);
        std::string line;
        while (std::getline(std::cin, line)) {
            bool finish = false;
            json reply;
            try {
                auto request = json::parse(line);
                auto op = request.at("operation").get<std::string>();
                if (op == "configure" || op == "restart") {
                    auto values = settings_json(app.settings());
                    values.merge_patch(request.at("settings"));
                    auto settings = settings_from_json(values);
                    loopback(settings);
                    if (op == "restart")
                        app.start(settings);
                    else
                        app.configure(settings, request.value("reload", false));
                } else if (op == "arm")
                    app.arm(request.at("enabled").get<bool>());
                else if (op == "button")
                    app.set_button(request.at("button").get<int>(), request.at("enabled").get<bool>());
                else if (op == "click")
                    app.click(3, 2, std::chrono::milliseconds(10), std::chrono::milliseconds(15));
                else if (op == "benchmark")
                    app.benchmark(PerformanceGoal::balanced);
                else if (op == "cancel")
                    app.cancel_benchmark();
                else if (op == "stop") {
                    app.stop();
                    finish = true;
                } else if (op != "stats")
                    throw std::runtime_error("Unknown scenario operation");
                reply = {{"ok", true}, {"stats", snapshot(app)}};
            } catch (const std::exception& e) {
                reply = {{"ok", false}, {"error", e.what()}, {"stats", snapshot(app)}};
            }
            std::cout << reply.dump() << std::endl;
            if (finish)
                break;
        }
        app.stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
