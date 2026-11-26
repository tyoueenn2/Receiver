#include "abcurves_worker.hpp"
#include "python_worker.hpp"
#include "receiver/protocol.hpp"
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
    State state = state_;
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
            if (hello.value("status", "") != "ready" || hello.value("version", 0) != 2)
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
                worker.send_json({{"generation", request.generation},
                                  {"step_ms", request.step_ms},
                                  {"target", {request.x, request.y}},
                                  {"executed", {request.executed_x, request.executed_y}}},
                                 deadline);
                const auto result = worker.receive_json(deadline);
                const auto& reports = result.at("reports");
                if (result.value("status", "") != "result" || !reports.is_array() ||
                    reports.size() != size_t(request.step_ms))
                    throw worker.error("invalid movement response");
                Reply reply{request.id};
                for (const auto& report : reports) {
                    if (!report.is_array() || report.size() != 2)
                        throw worker.error("invalid rendered report");
                    auto& axes = reply.reports[reply.count++];
                    for (size_t axis = 0; axis < axes.size(); ++axis) {
                        const auto& value = report[axis];
                        if (!value.is_number_integer())
                            throw worker.error("invalid rendered report");
                        const auto counts = value.get<double>();
                        if (counts < -32768 || counts > 32768)
                            throw worker.error("rendered report is out of range");
                        axes[axis] = int(counts);
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    if (stopped_ || current != revision_)
                        break;
                    state_.reply = reply;
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
