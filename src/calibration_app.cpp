#include "receiver/app.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace receiver {
bool App::calibration_compatible_locked() const {
    const auto& r = settings_.calibration;
    if (!r.verified || !move_capabilities_.ready || now_ns() - capabilities_at_ > 500'000'000 ||
        r.layout != move_capabilities_.layout || r.poll_us != move_capabilities_.poll_us)
        return false;
    if (r.context == 0) {
        if (!cursor_sample_.valid() || now_ns() - cursor_received_ > 100'000'000 ||
            r.display != cursor_sample_.display || r.display_width != cursor_sample_.width ||
            r.display_height != cursor_sample_.height || r.display_left != cursor_sample_.left ||
            r.display_top != cursor_sample_.top)
            return false;
    }
    if (r.frame_width && calibration_frame_ &&
        (r.frame_width != calibration_frame_->header.width ||
         r.frame_height != calibration_frame_->header.height))
        return false;
    return true;
}
CalibrationStatus App::calibration_status() const {
    std::lock_guard lock(mutex_);
    auto status = calibration_state_;
    status.supported = move_capabilities_.ready && now_ns() - capabilities_at_ < 500'000'000;
    status.desktop_feedback = cursor_sample_.valid() && now_ns() - cursor_received_ < 100'000'000;
    status.operation_active = move_request_ && !move_protocol::terminal(move_result_.status);
    if (!status.running && status.error.empty())
        status.message = calibration_compatible_locked()  ? "Ready"
                         : settings_.calibration.verified ? "Needs verification"
                                                          : "Not calibrated";
    return status;
}
void App::clear_calibration() {
    std::lock_guard lock(mutex_);
    if (calibration_state_.running)
        throw std::runtime_error("Cancel calibration first");
    settings_.calibration = {};
    calibration_state_.error.clear();
    ++calibration_state_.revision;
}
void App::cancel_move_locked() {
    if (move_request_ && !move_protocol::terminal(move_result_.status)) {
        move_result_.status = move_protocol::Status::Cancelled;
        move_result_.flags |= 1;
        calibration_state_.action = move_result_;
        calibration_state_.operation = "Stopped; return is unverified";
    }
    move_target_.reset();
    move_origin_.reset();
    cv_.notify_all();
}
uint64_t App::submit_move_locked(int dx, int dy, int button, bool midpoint) {
    if (stop_ || settings_.mouse_backend != 0 || !stats_.pi_ready || !move_capabilities_.ready ||
        now_ns() - capabilities_at_ > 500'000'000)
        throw std::runtime_error("Requires a ready Pi with move and return support");
    if (move_request_ && !move_protocol::terminal(move_result_.status))
        throw std::runtime_error("A move and return operation is already pending");
    auto state = injected_.snapshot();
    if (state.mask || state.release_barrier ||
        (button ? (stats_.physical & (1u << (button - 1))) : stats_.physical) ||
        injected_.metrics().pending_clicks)
        throw std::runtime_error("Release mouse buttons and wait for previous commands to finish");
    move_protocol::Request r;
    r.session = injected_.click_client_session();
    r.command = injected_.reserve_command();
    r.epoch = move_capabilities_.epoch;
    r.generation = move_capabilities_.generation;
    r.dx = dx;
    r.dy = dy;
    r.button = uint8_t(button);
    r.flags = midpoint ? 1 : 0;
    r.settle_us = uint32_t(settings_.return_action.settle_ms) * 1000;
    r.press_us = std::max(uint32_t(settings_.return_action.press_ms) * 1000, move_capabilities_.poll_us);
    r.return_us = uint32_t(settings_.return_action.return_delay_ms) * 1000;
    r.budget_us = 5'000'000;
    r.wait_us = 1'500'000;
    auto wire = move_protocol::packet(r);
    move_protocol::Request check;
    if (button < 0 || button > 8 || !move_protocol::parse(wire.data(), wire.size(), check))
        throw std::runtime_error("Movement or timing is outside the Pi limits");
    move_request_ = r;
    move_result_ = {};
    move_result_.status = move_protocol::Status::Accepted;
    move_result_.session = r.session;
    move_result_.command = r.command;
    move_result_.epoch = r.epoch;
    move_result_.generation = r.generation;
    move_stage_at_ = now_ns();
    move_deadline_ = move_stage_at_ + 5'500'000'000ll;
    move_next_send_ = 0;
    move_completed_at_ = 0;
    move_target_.reset();
    move_origin_.reset();
    if (!calibration_state_.running)
        calibration_state_.error.clear();
    calibration_state_.action = move_result_;
    calibration_state_.operation = "Submitting";
    control_wake_.notify();
    return r.command;
}
uint64_t App::move_click_return(double x, double y, bool desktop, bool click) {
    std::lock_guard lock(mutex_);
    if (calibration_state_.running)
        throw std::runtime_error("Finish calibration first");
    if (!calibration_compatible_locked())
        throw std::runtime_error("Verify a compatible calibration first");
    const auto& r = settings_.calibration;
    double dx = x, dy = y;
    if (desktop) {
        if (r.context != 0)
            throw std::runtime_error("Desktop coordinates require desktop calibration");
        if (x < cursor_sample_.left || y < cursor_sample_.top ||
            x >= int64_t(cursor_sample_.left) + cursor_sample_.width ||
            y >= int64_t(cursor_sample_.top) + cursor_sample_.height)
            throw std::runtime_error("Target is outside the calibrated display");
        dx = x - cursor_sample_.x;
        dy = y - cursor_sample_.y;
    } else if (r.context == 0) {
        if (!calibration_frame_ || !cursor_sample_.has_frame() ||
            cursor_sample_.frame_sequence != calibration_frame_->header.sequence ||
            now_ns() - calibration_frame_->first_ns > int64_t(settings_.max_age_ms) * 1'000'000)
            throw std::runtime_error("Wait for fresh frame geometry matching the cursor feedback");
        if (x < 0 || y < 0 || x >= calibration_frame_->header.width || y >= calibration_frame_->header.height)
            throw std::runtime_error("Target is outside the captured image");
        x += cursor_sample_.crop_left;
        y += cursor_sample_.crop_top;
        dx = x - cursor_sample_.x;
        dy = y - cursor_sample_.y;
    } else {
        if (!calibration_frame_ ||
            now_ns() - calibration_frame_->first_ns > int64_t(settings_.max_age_ms) * 1'000'000)
            throw std::runtime_error("Wait for a fresh view frame");
        dx = x - (settings_.reference_x < 0 ? calibration_frame_->header.width / 2.0 : settings_.reference_x);
        dy =
            y - (settings_.reference_y < 0 ? calibration_frame_->header.height / 2.0 : settings_.reference_y);
    }
    if (settings_.return_action.verified_target && r.context != 0)
        throw std::runtime_error("Verification before clicking requires desktop cursor feedback");
    auto counts = calibrated_counts(r, dx, dy);
    const auto id =
        submit_move_locked(counts.first, counts.second, click ? settings_.return_action.button : 0,
                           settings_.return_action.verified_target);
    if (r.context == 0) {
        move_origin_ = std::pair<double, double>{cursor_sample_.x, cursor_sample_.y};
        if (settings_.return_action.verified_target)
            move_target_ = std::pair<double, double>{x, y};
    }
    stats_.armed = stats_.active = false;
    action_latched_ = true;
    result_.reset();
    ++epoch_;
    return id;
}
void App::start_calibration(const Settings& config, int context, bool verify) {
    validate(config);
    if (context < 0 || context > 1 || config.mouse_backend != 0)
        throw std::runtime_error("Calibration requires Pi UDP output and a valid context");
    {
        std::lock_guard lock(mutex_);
        if (calibration_state_.running)
            throw std::runtime_error("Calibration is already running");
        if (stats_.model_loading || stats_.benchmarking)
            throw std::runtime_error("Finish the model operation first");
        if (move_request_ && !move_protocol::terminal(move_result_.status))
            throw std::runtime_error("Wait for the current move operation");
    }
    if (calibration_thread_.joinable())
        calibration_thread_.join();
    if (stop_)
        start(config, false, true);
    {
        std::lock_guard lock(mutex_);
        settings_.calibration = config.calibration;
        stats_.armed = stats_.active = false;
        result_.reset();
        ++epoch_;
        calibration_cancel_ = false;
        calibration_state_.running = true;
        calibration_state_.error.clear();
        calibration_state_.message = "Starting in one second. Leave the mouse still.";
        calibration_state_.completed = 0;
        calibration_state_.total = 40;
        injected_.release_all(ReleaseReason::manual, true);
    }
    calibration_thread_ = std::thread([this, context, verify] { calibration_loop(context, verify); });
    control_wake_.notify();
}
void App::cancel_calibration() {
    calibration_cancel_ = true;
    {
        std::lock_guard lock(mutex_);
        if (move_request_ && !move_protocol::terminal(move_result_.status)) {
            move_request_->operation = move_protocol::Operation::Cancel;
            move_next_send_ = 0;
        }
    }
    cv_.notify_all();
    control_wake_.notify();
}
move_protocol::Ack App::wait_move(uint64_t id, bool midpoint) {
    std::unique_lock lock(mutex_);
    const auto deadline = now_ns() + 5'600'000'000ll;
    for (;;) {
        if (stop_ || calibration_cancel_)
            throw std::runtime_error("Calibration cancelled");
        if (move_result_.command != id)
            throw std::runtime_error("Calibration command was superseded");
        if (move_protocol::terminal(move_result_.status)) {
            if (move_result_.status != move_protocol::Status::Completed || (move_result_.flags & 1))
                throw std::runtime_error(std::string("Pi operation: ") +
                                         move_protocol::status_name(move_result_.status));
            return move_result_;
        }
        if (midpoint && move_result_.stage == move_protocol::Stage::Waiting)
            return move_result_;
        if (now_ns() >= deadline)
            throw std::runtime_error("Pi operation did not complete");
        cv_.wait_for(lock, std::chrono::milliseconds(20));
    }
}
void App::continue_move(uint64_t id) {
    std::lock_guard lock(mutex_);
    if (move_request_ && move_request_->command == id && !move_protocol::terminal(move_result_.status)) {
        move_request_->operation = move_protocol::Operation::Continue;
        move_next_send_ = 0;
        control_wake_.notify();
    }
}
std::pair<double, double> App::observe_cursor(int64_t after) {
    std::unique_lock lock(mutex_);
    const auto deadline = now_ns() + 1'000'000'000;
    for (;;) {
        if (stop_ || calibration_cancel_)
            throw std::runtime_error("Calibration cancelled");
        if (cursor_sample_.valid() && cursor_requested_ >= after &&
            now_ns() - cursor_received_ < 100'000'000) {
            const auto& s = cursor_sample_;
            if (s.x <= s.left || s.y <= s.top || s.x >= int64_t(s.left) + s.width - 1 ||
                s.y >= int64_t(s.top) + s.height - 1)
                throw std::runtime_error(
                    "Cursor reached a display boundary. Move it toward the center and retry.");
            return {s.x, s.y};
        }
        if (now_ns() >= deadline)
            throw std::runtime_error("No fresh cursor feedback. Update Sender or choose view calibration.");
        cv_.wait_for(lock, std::chrono::milliseconds(10));
    }
}
std::shared_ptr<const Frame> App::observe_frame(int64_t after) {
    std::unique_lock lock(mutex_);
    const auto deadline = now_ns() + 1'000'000'000;
    for (;;) {
        if (stop_ || calibration_cancel_)
            throw std::runtime_error("Calibration cancelled");
        if (calibration_frame_) {
            const auto at = now_ns();
            const auto age = settings_.frame_source == "local"
                                 ? std::optional<int64_t>(at - calibration_frame_->header.capture_ns)
                                 : clock_.age_upper(calibration_frame_->header.capture_ns, at);
            if (!after || (age && *age >= 0 && at - *age >= after))
                return calibration_frame_;
        }
        if (now_ns() >= deadline)
            throw std::runtime_error(
                "No causally fresh view frame after the probe. Check capture and clock synchronization.");
        cv_.wait_for(lock, std::chrono::milliseconds(10));
    }
}
void App::calibration_loop(int context, bool verify) {
    try {
        CalibrationRecord record;
        {
            std::unique_lock lock(mutex_);
            const auto countdown = now_ns() + 1'000'000'000;
            while (now_ns() < countdown && !stop_ && !calibration_cancel_)
                cv_.wait_for(lock, std::chrono::milliseconds(20));
            const auto deadline = now_ns() + 2'000'000'000;
            while (!stop_ && !calibration_cancel_ &&
                   (!move_capabilities_.ready || now_ns() - capabilities_at_ > 500'000'000 ||
                    injected_.snapshot().release_barrier)) {
                if (now_ns() >= deadline)
                    throw std::runtime_error("Pi does not expose a ready move and return endpoint. Update "
                                             "the proxy and reconnect.");
                cv_.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (stop_ || calibration_cancel_)
                throw std::runtime_error("Calibration cancelled");
            record = settings_.calibration;
            record.context = context;
            record.verified = false;
        }
        double max_return_error = 0;
        std::vector<CalibrationObservation> samples;
        bool frozen = false;
        move_protocol::Capabilities device;
        cursor_protocol::Sample geometry;
        FrameHeader frame_geometry;
        auto ensure_stable = [&] {
            std::lock_guard lock(mutex_);
            if (!frozen) {
                device = move_capabilities_;
                geometry = cursor_sample_;
                if (calibration_frame_)
                    frame_geometry = calibration_frame_->header;
                frozen = true;
                return;
            }
            if (device.epoch != move_capabilities_.epoch ||
                device.generation != move_capabilities_.generation ||
                device.layout != move_capabilities_.layout || device.poll_us != move_capabilities_.poll_us)
                throw std::runtime_error(
                    "Pi endpoint changed during calibration; retry on the current device");
            if (context == 0 &&
                (geometry.session != cursor_sample_.session ||
                 geometry.generation != cursor_sample_.generation ||
                 geometry.display != cursor_sample_.display || geometry.left != cursor_sample_.left ||
                 geometry.top != cursor_sample_.top || geometry.width != cursor_sample_.width ||
                 geometry.height != cursor_sample_.height))
                throw std::runtime_error("Display, crop, or Sender changed during calibration");
            if (context == 1 &&
                (!calibration_frame_ || frame_geometry.session != calibration_frame_->header.session ||
                 frame_geometry.width != calibration_frame_->header.width ||
                 frame_geometry.height != calibration_frame_->header.height))
                throw std::runtime_error("Capture geometry changed during view calibration");
        };
        auto probe = [&](int dx, int dy) {
            std::pair<double, double> baseline{};
            std::shared_ptr<const Frame> before;
            if (context == 0)
                baseline = observe_cursor(now_ns());
            else
                before = observe_frame(0);
            ensure_stable();
            uint64_t id;
            {
                std::lock_guard lock(mutex_);
                calibration_state_.message = "Measuring " + std::string(dx ? "horizontal" : "vertical") +
                                             " response. Leave the mouse still.";
                id = submit_move_locked(dx, dy, 0, true);
                move_request_->settle_us = context ? 60000 : 20000;
                move_request_->return_us = 0;
            }
            wait_move(id, true);
            int64_t barrier;
            {
                std::lock_guard lock(mutex_);
                barrier = move_stage_at_;
            }
            CalibrationObservation sample{double(dx), double(dy), 0, 0};
            if (context == 0) {
                auto p = observe_cursor(barrier);
                sample.pixels_x = p.first - baseline.first;
                sample.pixels_y = p.second - baseline.second;
            } else {
                auto after = observe_frame(barrier);
                auto d = register_view(*before, *after);
                sample.pixels_x = -d.x;
                sample.pixels_y = -d.y;
            }
            ensure_stable();
            continue_move(id);
            wait_move(id, false);
            {
                std::lock_guard lock(mutex_);
                barrier = move_stage_at_;
            }
            double error;
            if (context == 0) {
                auto p = observe_cursor(barrier);
                error = std::hypot(p.first - baseline.first, p.second - baseline.second);
            } else {
                auto returned = observe_frame(barrier);
                auto d = register_view(*before, *returned);
                error = std::hypot(d.x, d.y);
            }
            ensure_stable();
            max_return_error = std::max(max_return_error, error);
            if (error > record.tolerance_px)
                throw std::runtime_error("Return error exceeds tolerance. Check acceleration, scene "
                                         "movement, and pointer settings.");
            {
                std::lock_guard lock(mutex_);
                ++calibration_state_.completed;
            }
            return sample;
        };
        int tested = 0;
        if (!verify) {
            for (int amplitude : {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048}) {
                if (amplitude > record.max_counts)
                    break;
                if (!samples.empty()) {
                    double scale = 0;
                    for (const auto& s : samples)
                        scale = std::max(scale, std::hypot(s.pixels_x, s.pixels_y) /
                                                    std::hypot(s.counts_x, s.counts_y));
                    if (context && scale * amplitude > 24)
                        break;
                    if (!context) {
                        std::lock_guard lock(mutex_);
                        const auto& c = cursor_sample_;
                        const double margin =
                            std::min({double(c.x) - c.left, double(c.y) - c.top,
                                      double(c.left) + c.width - c.x, double(c.top) + c.height - c.y}) -
                            16;
                        if (scale * amplitude > margin)
                            break;
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    const auto& c = move_capabilities_;
                    const int limit = std::min({c.xmax, -c.xmin, c.ymax, -c.ymin}) * int(c.max_segments);
                    if (amplitude > limit)
                        break;
                    calibration_state_.total =
                        std::max(calibration_state_.total, calibration_state_.completed + 8);
                }
                for (auto delta :
                     {std::pair<int, int>{amplitude, 0}, {-amplitude, 0}, {0, amplitude}, {0, -amplitude}})
                    samples.push_back(probe(delta.first, delta.second));
                tested = amplitude;
            }
            record = fit_calibration(samples, record.tolerance_px);
            record.context = context;
            record.max_counts = tested;
        } else {
            validate_calibration(record);
            tested = record.max_counts;
        }
        if (verify) {
            for (auto delta : {std::pair<int, int>{tested, 0}, {-tested, 0}, {0, tested}, {0, -tested}}) {
                auto sample = probe(delta.first, delta.second);
                const double error =
                    std::hypot(sample.pixels_x - record.xx * sample.counts_x - record.xy * sample.counts_y,
                               sample.pixels_y - record.yx * sample.counts_x - record.yy * sample.counts_y);
                if (error > record.tolerance_px)
                    throw std::runtime_error("Manual calibration does not match the measured response");
                record.error_px = std::max(record.error_px, error);
            }
        }
        const int a = std::max(1, tested * 3 / 5), b = std::max(1, tested * 2 / 5);
        for (auto delta : {std::pair<int, int>{a, b}, {-a, -b}, {b, -a}, {-b, a}}) {
            auto s = probe(delta.first, delta.second);
            const double error = std::hypot(s.pixels_x - record.xx * s.counts_x - record.xy * s.counts_y,
                                            s.pixels_y - record.yx * s.counts_x - record.yy * s.counts_y);
            record.error_px = std::max(record.error_px, error);
            if (error > record.tolerance_px)
                throw std::runtime_error(
                    "Independent verification failed. Use fixed pointer settings or smaller probes.");
        }
        {
            std::lock_guard lock(mutex_);
            if (stop_ || calibration_cancel_)
                throw std::runtime_error("Calibration cancelled");
            record.layout = move_capabilities_.layout;
            record.poll_us = move_capabilities_.poll_us;
            record.display = cursor_sample_.display;
            record.display_width = cursor_sample_.width;
            record.display_height = cursor_sample_.height;
            record.display_left = cursor_sample_.left;
            record.display_top = cursor_sample_.top;
            record.frame_width = calibration_frame_ ? calibration_frame_->header.width : 0;
            record.frame_height = calibration_frame_ ? calibration_frame_->header.height : 0;
            record.return_error_px = max_return_error;
            record.verified = true;
            record.method = verify && record.method == "manual" ? "verified manual" : "automatic";
            validate_calibration(record);
            settings_.calibration = record;
            ++calibration_state_.revision;
            calibration_state_.message = "Calibration verified and applied";
            calibration_state_.total = calibration_state_.completed;
            calibration_state_.running = false;
        }
    } catch (const std::exception& e) {
        std::unique_lock lock(mutex_);
        if (move_request_ && !move_protocol::terminal(move_result_.status) && !stop_) {
            move_request_->operation = move_protocol::Operation::Cancel;
            move_next_send_ = 0;
            control_wake_.notify();
            const auto deadline = now_ns() + 700'000'000;
            while (!stop_ && !move_protocol::terminal(move_result_.status) && now_ns() < deadline)
                cv_.wait_for(lock, std::chrono::milliseconds(20));
            if (!move_protocol::terminal(move_result_.status)) {
                cancel_move_locked();
                injected_.release_all(ReleaseReason::manual, true);
            }
        }
        calibration_state_.running = false;
        calibration_state_.message = "Calibration stopped";
        calibration_state_.error = e.what();
    }
    cv_.notify_all();
    control_wake_.notify();
}
} // namespace receiver
