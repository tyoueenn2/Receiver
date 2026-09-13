#include "receiver/app.hpp"
#include "receiver/local_mouse.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
namespace receiver {
App::App() : injected_(random_id()) {
    injected_.set_wake([this] { control_wake_.notify(); });
}
double Samples::percentile(double p) const {
    auto n = size_t(std::min<uint64_t>(count, values.size()));
    if (!n)
        return 0;
    auto sorted = values;
    std::sort(sorted.begin(), sorted.begin() + n);
    return sorted[size_t(std::ceil(p * (n - 1)))];
}
void App::fail(const std::string& error, ReleaseReason reason) {
    std::lock_guard lock(mutex_);
    stats_.error = error;
    stats_.status = "Failed";
    stats_.armed = stats_.active = false;
    ++epoch_;
    result_.reset();
    stop_ = true;
    injected_.release_all(reason, true);
    cv_.notify_all();
    control_wake_.notify();
}
void App::start(const Settings& settings, bool simulated) {
    stop();
    validate(settings);
    address(settings.bind_ip, settings.frame_port);
    auto sender = address(settings.sender_ip, 0);
    auto pi = address(settings.mouse_backend == 1 ? "127.0.0.1" : settings.pi_ip, settings.pi_port);
    auto local = address("127.0.0.1", 0);
    if (simulated && (sender.ip != local.ip || (settings.mouse_backend == 0 && pi.ip != local.ip)))
        throw std::runtime_error("Simulation requires both sender and Pi to be 127.0.0.1");
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
        stats_ = {};
        stats_.running = true;
        stats_.status = "Starting";
        latest_.reset();
        result_.reset();
        preview_ = {};
        clock_.reset();
        ++epoch_;
        simulated_ = simulated;
        stop_ = false;
        sender_seen_ = 0;
    }
    injected_.reset(random_id());
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    receive_thread_ = std::thread([this] {
        try {
            receive_loop();
        } catch (const std::exception& e) {
            fail(e.what(), ReleaseReason::sender_loss);
        }
    });
    inference_thread_ = std::thread([this] {
        try {
            inference_loop();
        } catch (const std::exception& e) {
            fail(e.what(), ReleaseReason::gpu_error);
        }
    });
    pi_thread_ = std::thread([this] {
        try {
            pi_loop();
        } catch (const std::exception& e) {
            fail(e.what(), ReleaseReason::pi_error);
        }
    });
}
void App::stop() {
    bool had_threads = receive_thread_.joinable();
    {
        std::lock_guard lock(mutex_);
        if (had_threads && stats_.error.empty())
            injected_.release_all(ReleaseReason::shutdown, true);
        stop_ = true;
        stats_.armed = stats_.active = false;
        ++epoch_;
        result_.reset();
    }
    cv_.notify_all();
    control_wake_.notify();
    for (auto* t : {&receive_thread_, &inference_thread_, &pi_thread_})
        if (t->joinable())
            t->join();
    {
        std::lock_guard lock(mutex_);
        stats_.running = false;
        latest_.reset();
        preview_ = {};
        if (stats_.error.empty())
            stats_.status = "Stopped";
    }
#ifdef _WIN32
    if (had_threads)
        timeEndPeriod(1);
#else
    (void)had_threads;
#endif
}
void App::arm(bool enabled) {
    {
        std::lock_guard lock(mutex_);
        stats_.armed = enabled && !stop_ && stats_.error.empty();
        ++epoch_;
        result_.reset();
        stats_.active = false;
        if (!enabled)
            injected_.release_all(ReleaseReason::disarm, true);
    }
    control_wake_.notify();
}
void App::configure(const Settings& s) {
    validate(s);
    bool restart;
    {
        std::lock_guard lock(mutex_);
        restart = s.bind_ip != settings_.bind_ip || s.sender_ip != settings_.sender_ip ||
                  s.pi_ip != settings_.pi_ip || s.frame_port != settings_.frame_port ||
                  s.pi_port != settings_.pi_port || s.model != settings_.model ||
                  s.metadata != settings_.metadata || s.input_size != settings_.input_size ||
                  s.mouse_backend != settings_.mouse_backend;
        if (restart) {
            stats_.armed = stats_.active = false;
            ++epoch_;
            result_.reset();
            injected_.release_all(ReleaseReason::configuration_restart, true);
        } else {
            settings_ = s;
            ++epoch_;
            result_.reset();
            if (!s.preview)
                preview_ = {};
        }
    }
    control_wake_.notify();
    if (restart)
        throw std::runtime_error("Stop and restart to change networking or model");
}
namespace {
void require_synthetic_ready(bool stopped, const Stats& stats, const Settings& settings,
                             int64_t sender_seen) {
    if (stopped || !stats.running || !stats.armed || !stats.error.empty())
        throw std::runtime_error("Synthetic buttons require a running, armed Receiver");
    if (settings.mouse_backend != 0)
        throw std::runtime_error("Synthetic buttons require the Pi UDP backend");
    const auto now = now_ns();
    if (!stats.pi_ready || !sender_seen || now < sender_seen || now - sender_seen > 100'000'000)
        throw std::runtime_error("Synthetic buttons require fresh sender and Pi telemetry");
}
}
void App::button_down(int button) {
    std::lock_guard lock(mutex_);
    require_synthetic_ready(stop_, stats_, settings_, sender_seen_);
    injected_.button_down(button);
}
void App::button_up(int button) {
    std::lock_guard lock(mutex_);
    injected_.button_up(button);
}
void App::set_button(int button, bool state) {
    if (state)
        button_down(button);
    else
        button_up(button);
}
void App::release_all() {
    std::lock_guard lock(mutex_);
    injected_.release_all(ReleaseReason::manual, true);
}
InjectedButtonManager::Hold App::hold(int button) {
    std::lock_guard lock(mutex_);
    require_synthetic_ready(stop_, stats_, settings_, sender_seen_);
    return injected_.hold(button);
}
uint64_t App::click(int button, uint32_t count, std::chrono::microseconds press_duration,
                    std::chrono::microseconds interval) {
    std::lock_guard lock(mutex_);
    require_synthetic_ready(stop_, stats_, settings_, sender_seen_);
    return injected_.click(button, count, press_duration, interval);
}
Stats App::stats() const {
    auto injection = injected_.metrics();
    std::lock_guard lock(mutex_);
    auto s = stats_;
    s.injection = std::move(injection);
    s.running = !stop_;
    return s;
}
Preview App::preview() const {
    std::lock_guard lock(mutex_);
    return preview_;
}
void App::receive_loop() {
    Settings cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = settings_;
    }
    UdpSocket socket(cfg.bind_ip, cfg.frame_port);
    auto allowed = address(cfg.sender_ip, 0);
    Address peer{};
    uint64_t session = 0;
    int64_t last_hello = 0, last_ping = 0, pending_ping = 0;
    std::array<uint64_t, 16> retired{};
    size_t retire_at = 0;
    Reassembler reassembly;
    bool sender_lost = false;
    std::array<uint8_t, 2048> data{};
    while (!stop_) {
        Address from;
        int n = socket.receive(data, from, 2);
        auto now = now_ns();
        if (n > 0 && from.ip == allowed.ip) {
            Bytes p(data.data(), size_t(n));
            if (n == 16 && !std::memcmp(data.data(), "UVH1", 4) && be32(data.data() + 4) == 0) {
                uint64_t next = be64(data.data() + 8);
                if (next && std::find(retired.begin(), retired.end(), next) == retired.end() &&
                    (!session || from == peer || now - last_hello > 100'000'000)) {
                    bool restarted = false;
                    if (next != session || from != peer) {
                        restarted = session != 0;
                        if (session)
                            retired[retire_at++ % retired.size()] = session;
                        session = next;
                        peer = from;
                        reassembly.reset(session);
                        pending_ping = last_ping = 0;
                        std::lock_guard lock(mutex_);
                        clock_.reset();
                        latest_.reset();
                        result_.reset();
                        preview_ = {};
                        ++epoch_;
                        if (restarted)
                            injected_.release_all(ReleaseReason::sender_restart, true);
                    }
                    last_hello = now;
                    sender_seen_ = now;
                    sender_lost = false;
                }
            } else if (session && from == peer) {
                if (n == 40 && !std::memcmp(data.data(), "UVS1", 4) && be32(data.data() + 4) == 0 &&
                    be64(data.data() + 8) == session && be64(data.data() + 16) == uint64_t(pending_ping) &&
                    pending_ping) {
                    auto t1 = be64(data.data() + 24), t2 = be64(data.data() + 32);
                    if (t1 <= INT64_MAX && t2 <= INT64_MAX) {
                        std::lock_guard lock(mutex_);
                        clock_.observe(pending_ping, int64_t(t1), int64_t(t2), now);
                    }
                    pending_ping = 0;
                } else if (n >= 48 && !std::memcmp(data.data(), "UVF1", 4)) {
                    auto frame = reassembly.accept(p, now);
                    if (frame) {
                        std::lock_guard lock(mutex_);
                        if (latest_)
                            ++stats_.replaced;
                        latest_ = std::move(frame);
                        stats_.width = latest_->header.width;
                        stats_.height = latest_->header.height;
                        stats_.reassembly.add(double(latest_->complete_ns - latest_->first_ns) / 1e6);
                        cv_.notify_one();
                    }
                }
            }
        }
        reassembly.expire(now);
        auto seen = sender_seen_.load();
        if (seen && now >= seen && now - seen > 100'000'000 && !sender_lost) {
            sender_seen_ = 0;
            std::lock_guard lock(mutex_);
            stats_.active = false;
            ++epoch_;
            result_.reset();
            injected_.release_all(ReleaseReason::sender_loss, true);
            sender_lost = true;
        }
        if (session && now - last_ping >= 250'000'000) {
            std::array<uint8_t, 24> ping{};
            std::memcpy(ping.data(), "UVC1", 4);
            put64(ping.data() + 8, session);
            put64(ping.data() + 16, uint64_t(now));
            if (socket.send(ping, peer)) {
                pending_ping = last_ping = now;
            }
        }
        {
            std::lock_guard lock(mutex_);
            stats_.network = reassembly.stats;
            stats_.synchronized = clock_.synchronized(now);
            stats_.clock_uncertainty_ms = clock_.uncertainty_ms();
        }
    }
}
void App::inference_loop() {
    Settings initial;
    {
        std::lock_guard lock(mutex_);
        initial = settings_;
    }
    std::unique_ptr<Backend> backend;
    if (!simulated_)
        backend = make_backend(initial);
    {
        std::lock_guard lock(mutex_);
        stats_.backend =
            simulated_ ? "SIMULATION (fixed test detection, loopback only)" : backend->description();
        if (backend) {
            auto [free, total] = backend->device_memory();
            stats_.gpu_free_mib = double(free) / (1024 * 1024);
            stats_.gpu_total_mib = double(total) / (1024 * 1024);
        }
    }
    int64_t last_preview = 0;
    while (!stop_) {
        std::shared_ptr<const Frame> frame;
        Settings cfg;
        uint64_t epoch;
        int64_t deadline = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10), [&] { return stop_ || bool(latest_); });
            if (stop_)
                break;
            if (!latest_)
                continue;
            frame = std::move(latest_);
            cfg = settings_;
            epoch = epoch_;
            auto at = now_ns();
            auto age = clock_.age_upper(frame->header.capture_ns, at);
            if (age) {
                stats_.frame_age_ms = double(*age) / 1e6;
                const int64_t horizon = int64_t(cfg.max_age_ms) * 1'000'000;
                deadline = at + horizon - *age - horizon / 5000;
            }
            if ((deadline && at > deadline) || at - frame->first_ns > int64_t(cfg.max_age_ms) * 1'000'000) {
                ++stats_.stale;
                continue;
            }
        }
        std::vector<Detection> detections;
        double upload = 0, infer = 0;
        auto post_start = now_ns();
        if (simulated_) {
            detections.push_back({frame->header.width * .60f, frame->header.height * .4f,
                                  frame->header.width * .2f, frame->header.height * .2f, .95f, 0});
        } else {
            auto result = backend->run(*frame);
            upload = result.upload_ms;
            infer = result.inference_ms;
            post_start = now_ns();
            detections = decode_yolo(result.output, result.candidates, result.classes, frame->header.width,
                                     frame->header.height, cfg.input_size, cfg);
        }
        auto done = now_ns();
        {
            std::lock_guard lock(mutex_);
            ++stats_.inferred;
            stats_.upload.add(upload);
            stats_.inference.add(infer);
            stats_.postprocess.add(double(done - post_start) / 1e6);
            if (epoch == epoch_ && !stop_) {
                result_ = Result{frame, std::move(detections), epoch, ++result_id_, deadline, done};
                control_wake_.notify();
                if (cfg.preview && done - last_preview >= 33'333'333) {
                    preview_ = {frame, result_->detections, {}};
                    last_preview = done;
                }
            }
        }
    }
}
void App::pi_loop() {
    Settings initial;
    {
        std::lock_guard lock(mutex_);
        initial = settings_;
    }
    UdpSocket socket("0.0.0.0", 0);
    auto pi = address(initial.mouse_backend == 1 ? "127.0.0.1" : initial.pi_ip, initial.pi_port);
    const uint64_t client = random_id();
    TelemetryGate gate;
    gate.reset(client);
    uint64_t token = random_id(), last_result = 0, last_epoch = 0, sent_revision = 0;
    uint32_t sequence = uint32_t(random_id());
    int64_t renewal = 0, last_snapshot = 0;
    bool have_sent_revision = false;
    std::unique_ptr<LocalMouse> local_mouse;
    if (initial.mouse_backend == 1)
        local_mouse = std::make_unique<LocalMouse>();
    auto input_fresh = [&](int64_t at) { return local_mouse ? local_mouse->fresh(at) : gate.fresh(at); };
    Controller controller;
    MotionTracker motion_tracker;
    bool was_active = false;
    ReleaseReason unsafe_reason = ReleaseReason::none;
    bool had_input = false;
    enum class Probe { v3, selected_v1, selected_v3 };
    Probe probe = Probe::v3;
    int64_t probe_started = now_ns(), shutdown_deadline = 0;
    bool last_direction = initial.direction.enabled;
    std::array<uint8_t, 2048> bytes{};
    while (true) {
        auto loop_start = now_ns();
        if (stop_ && !shutdown_deadline)
            shutdown_deadline = loop_start + 100'000'000;
        if (shutdown_deadline && loop_start >= shutdown_deadline)
            break;
        control_wake_.wait(local_mouse ? nullptr : &socket, bool(local_mouse));
        auto now = now_ns();
#ifdef _WIN32
        if (GetAsyncKeyState(VK_DELETE) & 0x8000)
            arm(false);
#endif
        if (!local_mouse) {
            bool need_motion;
            {
                std::lock_guard lock(mutex_);
                need_motion = settings_.direction.enabled;
            }
            if (need_motion != last_direction && !shutdown_deadline) {
                probe = Probe::v3;
                probe_started = now;
                renewal = 0;
                gate.reset(client);
                motion_tracker.reset();
                had_input = false;
                last_direction = need_motion;
            }
            if (probe == Probe::v3 && now - probe_started >= 100'000'000) {
                probe = Probe::selected_v1;
                renewal = 0;
                gate.reset(client);
            }
            if (!shutdown_deadline && now - renewal >= 20'000'000) {
                const auto version = (probe == Probe::v3 || probe == Probe::selected_v3)
                                         ? SubscriptionVersion::v3
                                         : SubscriptionVersion::v1;
                auto p = subscribe(client, ++token, version);
                gate.issue(token, now);
                if (!socket.send(p, pi))
                    throw std::runtime_error("Pi subscription send failed");
                renewal = now;
            }
        }
        Address from;
        int n = 0;
        if (local_mouse) {
            gate.state = local_mouse->poll();
            float window;
            {
                std::lock_guard lock(mutex_);
                window = settings_.direction.window_ms;
            }
            motion_tracker.observe(gate.state, now_ns(), window);

        } else
#ifdef _WIN32
            n = socket.receive(bytes, from, 0);
#else
            n = socket.receive(bytes, from, 1);
#endif
        now = now_ns();
        bool proxy_error = false;
        bool protocol_error = false;
        if (n > 0 && from == pi) {
            if (n >= 4 && (!std::memcmp(bytes.data(), "UPT1", 4) ||
                           !std::memcmp(bytes.data(), "UPT2", 4) ||
                           !std::memcmp(bytes.data(), "UPT3", 4))) {
                const bool wire_v1 = !std::memcmp(bytes.data(), "UPT1", 4);
                const bool wire_v3 = !std::memcmp(bytes.data(), "UPT3", 4);
                const bool expected_wire = ((probe == Probe::v3 || probe == Probe::selected_v3) && wire_v3) ||
                                           (probe == Probe::selected_v1 && wire_v1);
                if (!expected_wire) {
                    // UPT2 has incompatible 80-byte layouts with no discriminator. Never
                    // infer which one an unsolicited packet uses; UPT3 is authoritative.
                    std::lock_guard lock(mutex_);
                    ++stats_.telemetry_rejected;
                } else {
                    auto parsed = parse_telemetry(Bytes(bytes.data(), size_t(n)));
                    if (!parsed) {
                        protocol_error = true;
                        std::lock_guard lock(mutex_);
                        ++stats_.telemetry_rejected;
                        stats_.error = "Invalid Pi telemetry protocol";
                        stats_.armed = false;
                        ++epoch_;
                        result_.reset();
                    } else if (!gate.accept(Bytes(bytes.data(), size_t(n)), now)) {
                        std::lock_guard lock(mutex_);
                        ++stats_.telemetry_rejected;
                    } else {
                        if (probe == Probe::v3)
                            probe = Probe::selected_v3;
                        const bool epoch_changed = injected_.observe_telemetry(gate.state);
                        float window;
                        {
                            std::lock_guard lock(mutex_);
                            window = settings_.direction.window_ms;
                        }
                        motion_tracker.observe(gate.state, now, window);
                        had_input = true;
                        if (epoch_changed) {
                            probe = Probe::v3;
                            probe_started = now;
                            renewal = 0;
                            gate.reset(client);
                            motion_tracker.reset();
                            had_input = false;
                        }
                    }
                }
            } else if (n >= 4 && !std::memcmp(bytes.data(), "UPA1", 4)) {
                const auto ack = injected_.process_click_ack(Bytes(bytes.data(), size_t(n)), now);
                if (ack == AckResult::protocol_error) {
                    protocol_error = true;
                    std::lock_guard lock(mutex_);
                    ++stats_.telemetry_rejected;
                    stats_.error = "Pi click protocol error";
                    stats_.armed = false;
                    ++epoch_;
                    result_.reset();
                } else if (ack == AckResult::ignored) {
                    std::lock_guard lock(mutex_);
                    ++stats_.telemetry_rejected;
                }
            } else {
                std::string reply(reinterpret_cast<char*>(bytes.data()), size_t(n));
                const bool expected_probe_error = probe == Probe::v3 &&
                    (reply.find("unknown") != std::string::npos ||
                     reply.find("unsupported") != std::string::npos ||
                     reply.find("invalid command") != std::string::npos);
                if (!expected_probe_error && (reply == "busy" || reply.rfind("error", 0) == 0)) {
                    {
                        std::lock_guard lock(mutex_);
                        stats_.error = "Pi: " + reply;
                        stats_.armed = false;
                        ++epoch_;
                        result_.reset();
                    }
                    proxy_error = true;
                }
            }
        }
        const bool input_ok = input_fresh(now);
        const auto seen = sender_seen_.load();
        const bool sender_ok = local_mouse || (seen && now >= seen && now - seen <= 100'000'000);
        bool have_error = false;
        {
            std::lock_guard lock(mutex_);
            if (!input_ok)
                stats_.pi_ready = false;
            have_error = !stats_.error.empty();
        }
        ReleaseReason next_unsafe = ReleaseReason::none;
        if (have_error || proxy_error || protocol_error)
            next_unsafe = ReleaseReason::pi_error;
        else if (seen && !sender_ok)
            next_unsafe = ReleaseReason::sender_loss;
        else if (!local_mouse && had_input && !input_ok)
            next_unsafe = ReleaseReason::stale_telemetry;
        if (!shutdown_deadline && next_unsafe != ReleaseReason::none && next_unsafe != unsafe_reason)
            injected_.release_all(next_unsafe, true);
        unsafe_reason = next_unsafe;

        // Requests and snapshots use the same persistent socket. No state-manager mutex is
        // held across a socket operation, and ReleaseAll requests are always selected first.
        if (!local_mouse) {
            auto state = injected_.snapshot();
            bool allow_schedules = false;
            {
                std::lock_guard lock(mutex_);
                allow_schedules = !shutdown_deadline && stats_.armed && sender_ok && input_ok &&
                                  stats_.error.empty() && !proxy_error && !protocol_error;
            }
            if (allow_schedules || state.release_barrier) {
                for (const auto& request : injected_.due_requests(now)) {
                    if (!request.release_all && !allow_schedules)
                        continue;
                    if (!socket.send(request.bytes, pi))
                        throw std::runtime_error("Pi UPC1 v2 request send failed");
                }
            }
            state = injected_.snapshot();
            const bool heartbeat =
                !state.release_barrier && state.mask && last_snapshot &&
                now - last_snapshot >= injected_heartbeat_ns;
            if (injected_snapshot_due(state, have_sent_revision, sent_revision, last_snapshot, now)) {
                const bool release_batch = state.release_snapshot_batches != 0;
                const uint8_t wire_mask = state.wire_mask();
                const int copies = release_batch ? 3 : 1;
                auto start = now_ns();
                for (int i = 0; i < copies; ++i) {
                    auto packet = movement(sequence++, 0, 0, 0, 0, wire_mask);
                    if (!socket.send(packet, pi))
                        throw std::runtime_error("Pi synthetic-button snapshot send failed");
                }
                last_snapshot = now_ns();
                if (release_batch)
                    have_sent_revision = false;
                else {
                    sent_revision = state.revision;
                    have_sent_revision = true;
                }
                injected_.mark_snapshot_sent(state.revision, wire_mask, release_batch);
                {
                    std::lock_guard lock(mutex_);
                    stats_.synthetic_snapshots += uint64_t(copies);
                    if (!wire_mask)
                        stats_.release_snapshots += uint64_t(copies);
                    if (heartbeat)
                        ++stats_.hold_heartbeats;
                    stats_.submit.add(double(last_snapshot - start) / 1e6);
                }
            }
        }

        if (shutdown_deadline)
            continue;

        std::lock_guard lock(mutex_);
        auto physical_motion = motion_tracker.estimate(now);
        stats_.motion_available = input_ok && physical_motion.available;
        stats_.mouse_speed = stats_.motion_available ? std::hypot(physical_motion.x, physical_motion.y) : 0;
        bool motion_ok = !settings_.direction.enabled || stats_.motion_available;
        bool active = stats_.armed && !proxy_error && sender_ok && input_ok && motion_ok &&
                      ((gate.state.physical & (1u << (settings_.activation_button - 1))) ||
                       (settings_.secondary_button &&
                        (gate.state.physical & (1u << (settings_.secondary_button - 1)))));
        stats_.pi_ready = input_ok;
        stats_.physical = gate.state.physical;
        if (active != was_active) {
            ++epoch_;
            result_.reset();
            controller.reset();
            was_active = active;
        }
        stats_.active = active;
        stats_.status = !stats_.error.empty() ? "Error - stop and restart"
                        : !sender_ok          ? "Waiting for capture sender"
                        : !input_ok           ? "Waiting for fresh Pi telemetry"
                        : !stats_.armed       ? "Disarmed"
                        : !motion_ok ? "Waiting for cumulative physical motion telemetry (UPT3 or legacy UPT2 required)"
                        : !active    ? "Armed - hold activation button"
                                     : "Active - waiting for fresh detection";
        if (!active || epoch_ != last_epoch) {
            stats_.assist_strength = 0;
            controller.reset();
            last_epoch = epoch_;
        }
        if (active && result_ && (!result_->deadline || now > result_->deadline)) {
            stats_.assist_strength = 0;
            controller.reset();
            stats_.status = "Active - waiting for fresh capture";
        }
        if (!active || !result_ || result_->id == last_result)
            continue;
        auto& r = *result_;
        last_result = r.id;
        if (r.epoch != epoch_ || !r.deadline || now > r.deadline ||
            now - r.frame->first_ns > int64_t(settings_.max_age_ms) * 1'000'000) {
            controller.reset();
            ++stats_.stale;
            continue;
        }
        stats_.control_handoff.add(double(now_ns() - r.completed_ns) / 1e6);
        auto correction =
            controller.update(r.detections, r.frame->header.width, r.frame->header.height, settings_,
                              gate.state, now, physical_motion, r.frame->header.capture_ns);
        stats_.assist_strength = correction.strength;
        if (preview_.frame && preview_.frame->header.sequence == r.frame->header.sequence)
            preview_.correction = correction;
        if (!correction.target) {
            stats_.status = "Active - no target";
            continue;
        }
        if (correction.dx || correction.dy) {
            // Serialize final gating, current complete synthetic snapshot and send against disarm/config changes.
            auto send_at = now_ns();
            if (send_at > r.deadline || !input_fresh(send_at)) {
                controller.reset();
                ++stats_.stale;
                continue;
            }
            auto state = injected_.snapshot();
            auto packet = movement(sequence++, correction.dx, correction.dy, 0, 0, state.wire_mask());
            if (!(local_mouse ? local_mouse->send(correction.dx, correction.dy) : socket.send(packet, pi))) {
                stats_.armed = false;
                stats_.error = local_mouse ? "Windows blocked local mouse output. Use a normal desktop "
                                             "window at the same privilege level."
                                           : "Pi command send failed";
                ++epoch_;
                controller.reset();
                injected_.release_all(local_mouse ? ReleaseReason::gpu_error : ReleaseReason::pi_error,
                                      true);
                continue;
            }
            auto sent = now_ns();
            ++stats_.sent;
            stats_.submit.add(double(sent - send_at) / 1e6);
            stats_.receiver_total.add(double(sent - r.frame->first_ns) / 1e6);
            stats_.capture_age.add(double(int64_t(settings_.max_age_ms) * 1'000'000 - (r.deadline - sent)) /
                                   1e6);
            stats_.status = "Active - correction submitted";
        }
    }
}
void App::export_metrics(const std::string& path) const {
    auto s = stats();
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot write metrics");
    f << "stage,samples,p50_ms,p95_ms,p99_ms\n";
    for (auto pair : {std::pair{"reassembly", &s.reassembly},
                      {"inference_to_control", &s.control_handoff},
                      {"copies_preprocess", &s.upload},
                      {"inference", &s.inference},
                      {"postprocess", &s.postprocess},
                      {"command_submission", &s.submit},
                      {"receiver_to_submission", &s.receiver_total},
                      {"capture_age_upper_at_submission", &s.capture_age}})
        f << pair.first << ',' << pair.second->count << ',' << pair.second->percentile(.5) << ','
          << pair.second->percentile(.95) << ',' << pair.second->percentile(.99) << '\n';
    f << "# backend," << s.backend << "\n# frames," << s.inferred << "\n# sent," << s.sent
      << "\n# synthetic_snapshots," << s.synthetic_snapshots << "\n# release_snapshots,"
      << s.release_snapshots << "\n# hold_heartbeats," << s.hold_heartbeats
      << "\n# persistent_injected_mask," << unsigned(s.injection.persistent_mask)
      << "\n# pending_click_commands," << s.injection.pending_clicks
      << "\n# click_commands_submitted_locally," << s.injection.click_submitted
      << "\n# click_commands_accepted_by_pi," << s.injection.click_accepted
      << "\n# click_commands_completed_by_usb_writer," << s.injection.click_completed
      << "\n# click_commands_rejected_before_acceptance,"
      << s.injection.click_rejected_before_acceptance
      << "\n# accepted_click_commands_cancelled_or_failed," << s.injection.click_accepted_failed
      << "\n# click_request_retries," << s.injection.click_retries
      << "\n# click_ack_timeouts," << s.injection.click_timeouts
      << "\n# click_queue_full_backpressure_events," << s.injection.click_queue_full
      << "\n# click_server_epoch_resets," << s.injection.click_epoch_resets
      << "\n# release_all_retries," << s.injection.release_retries
      << "\n# release_all_terminal_cancellations_superseded," << s.injection.release_superseded
      << "\n# last_release_reason," << s.injection.last_release_reason
      << "\n# last_release_all_state," << s.injection.last_release_all_state
      << "\n# pi_applied_persistent_mask," << unsigned(s.injection.pi_applied_persistent_mask)
      << "\n# pi_scheduled_click_mask," << unsigned(s.injection.pi_scheduled_mask)
      << "\n# pi_accepted_click_total," << s.injection.pi_accepted_click_total
      << "\n# pi_completed_click_total," << s.injection.pi_completed_click_total
      << "\n# pi_active_sequences," << s.injection.pi_active_sequences
      << "\n# pi_queued_sequences," << s.injection.pi_queued_sequences
      << "\n# pi_output_queue_depth," << s.injection.pi_output_queue_depth
      << "\n# pi_pending_synthetic_depth," << s.injection.pi_pending_synthetic_depth
      << "\n# pi_physical_reports_received," << s.injection.pi_physical_reports_received
      << "\n# pi_physical_reports_submitted," << s.injection.pi_physical_reports_submitted
      << "\n# pi_superseded_synthetic_movement," << s.injection.pi_superseded_synthetic
      << "\n# pi_usb_writer_failures," << s.injection.pi_writer_failures
      << "\n# superseded," << s.replaced << "\n# stale," << s.stale << "\n# incomplete_expired,"
      << s.network.expired << "\n# incomplete_evicted," << s.network.evicted << "\n# invalid_packets,"
      << s.network.invalid << "\n# duplicate_packets," << s.network.duplicates << "\n# pool_drops,"
      << s.network.pool_drops << "\n# host_frame_pool_mib,28\n# gpu_free_at_start_mib," << s.gpu_free_mib
      << "\n# gpu_total_mib," << s.gpu_total_mib << '\n';
    for (const auto& command : s.injection.commands)
        f << "# command," << command.id << ','
          << (command.operation == ClickOperation::release_all ? "ReleaseAll" : "Schedule") << ','
          << unsigned(command.button) << ',' << local_command_state_name(command.state) << ','
          << command.requested_clicks << ',' << command.accepted_clicks << ','
          << command.completed_clicks << '\n';
}
} // namespace receiver
