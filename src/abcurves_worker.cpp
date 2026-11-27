#include "abcurves_worker.hpp"
#include "python_worker.hpp"
#include "receiver/protocol.hpp"
#include <cmath>
#include <nlohmann/json.hpp>
#include <utility>

namespace receiver {
AbcurvesWorker::AbcurvesWorker(std::function<void()> wake)
    : wake_(std::move(wake)), thread_([this] { run(); }) {}

AbcurvesWorker::~AbcurvesWorker() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_one();
    thread_.join();
}

void AbcurvesWorker::configure(std::string_view python) {
    std::lock_guard lock(mutex_);
    if (python_ != python) {
        python_ = python;
        ++revision_;
        state_ = {};
        request_.reset();
        changed_.notify_one();
    }
}

AbcurvesWorker::State AbcurvesWorker::poll() {
    std::lock_guard lock(mutex_);
    State state{state_.ready, state_.error, std::move(state_.reply)};
    state_.reply.reset();
    return state;
}

void AbcurvesWorker::submit(Request request) {
    std::lock_guard lock(mutex_);
    if (state_.ready) {
        request_ = request;
        changed_.notify_one();
    }
}

void AbcurvesWorker::run() {
    uint64_t current = 0;
    while (true) {
        std::string python;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopped_ || current != revision_; });
            if (stopped_)
                return;
            current = revision_;
            python = python_;
        }
        if (python.empty())
            continue;
        try {
            auto cancelled = [&] {
                std::lock_guard lock(mutex_);
                return stopped_ || current != revision_;
            };
            const auto startup_deadline = now_ns() + 120'000'000'000ll;
            PythonWorker worker(python, "tools/abcurves_worker.py", "ABCurves", "abcurves",
                                startup_deadline, cancelled);
            auto hello = worker.receive_json(startup_deadline);
            if (hello.value("status", "") != "ready" || hello.value("version", 0) != 4)
                throw worker.error("unsupported worker protocol");
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || current != revision_)
                    continue;
                state_.ready = true;
            }
            wake_();
            while (true) {
                Request request;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [&] { return stopped_ || current != revision_ || request_; });
                    if (stopped_ || current != revision_)
                        break;
                    request = *request_;
                    request_.reset();
                }
                const auto deadline = now_ns() + 1'000'000'000;
                worker.send_json({{"target", {request.x, request.y}}, {"history", request.history},
                                  {"trajectory", request.trajectory}},
                                 deadline);
                const auto result = worker.receive_json(deadline);
                Reply reply{request.id};
                if (request.trajectory)
                    reply.positions.resize(abcurves_trajectory_ms);
                std::span<std::array<double, 2>> points = request.trajectory
                    ? std::span(reply.positions) : std::span(reply.directions.data(), reply.directions.size());
                const auto& forecast = result.at(request.trajectory ? "positions" : "directions");
                if (result.value("status", "") != "result" || !forecast.is_array() ||
                    forecast.size() != points.size())
                    throw worker.error("invalid movement response");
                for (size_t i = 0; i < points.size(); ++i) {
                    const auto& point = forecast[i];
                    if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number())
                        throw worker.error("invalid forecast point");
                    const double x = point[0].get<double>(), y = point[1].get<double>();
                    if (!std::isfinite(x) || !std::isfinite(y))
                        throw worker.error("invalid forecast point");
                    points[i] = {x, y};
                }
                {
                    std::lock_guard lock(mutex_);
                    if (stopped_ || current != revision_)
                        break;
                    state_.reply = std::move(reply);
                }
                wake_();
            }
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || current != revision_)
                    continue;
                state_ = {false, e.what(), {}};
                request_.reset();
            }
            wake_();
        }
    }
}
} // namespace receiver
