#pragma once
#include "abcurves_assist.hpp"
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace receiver {
class AbcurvesWorker {
  public:
    struct Request {
        uint64_t id;
        double x, y;
        mouse_history::Trace history;
    };
    struct Reply {
        uint64_t id;
        std::array<std::array<double, 2>, abcurves_horizon_ms> positions{};
    };
    struct State {
        bool ready = false;
        std::string error;
        std::optional<Reply> reply;
    };

  private:
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_ = false;
    uint64_t revision_ = 0;
    std::string python_;
    std::optional<Request> request_;
    State state_;
    std::thread thread_;
    void run();

  public:
    explicit AbcurvesWorker(std::function<void()> wake);
    ~AbcurvesWorker();
    void configure(std::string_view python);
    State poll();
    void submit(Request request);
};
} // namespace receiver
