#include "receiver/injection.hpp"
#include "receiver/network.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace receiver {
namespace {
constexpr int64_t response_timeout_ns = 50'000'000;
constexpr int64_t accepted_lease_ns = 75'000'000;
constexpr size_t max_pending_clicks = 32;
constexpr size_t max_history = 32;
bool terminal(LocalCommandState state) {
    return state == LocalCommandState::completed || state == LocalCommandState::rejected ||
           state == LocalCommandState::cancelled || state == LocalCommandState::unknown;
}
}
bool injected_snapshot_due(const InjectionSnapshot& state, bool have_sent_revision,
                           uint64_t sent_revision, int64_t last_snapshot, int64_t now) {
    if (state.release_snapshot_batches)
        return true;
    if (state.release_barrier)
        return false;
    if ((!have_sent_revision || state.revision != sent_revision) && (state.mask || state.revision))
        return true;
    return state.mask && last_snapshot && now >= last_snapshot &&
           now - last_snapshot >= injected_heartbeat_ns;
}
const char* release_reason_name(ReleaseReason reason) {
    switch (reason) {
    case ReleaseReason::none: return "none";
    case ReleaseReason::manual: return "manual";
    case ReleaseReason::disarm: return "disarm";
    case ReleaseReason::shutdown: return "shutdown";
    case ReleaseReason::sender_loss: return "sender loss";
    case ReleaseReason::sender_restart: return "sender restart";
    case ReleaseReason::stale_telemetry: return "stale telemetry";
    case ReleaseReason::pi_error: return "Pi error";
    case ReleaseReason::gpu_error: return "GPU error";
    case ReleaseReason::configuration_restart: return "configuration restart";
    case ReleaseReason::server_epoch_change: return "server epoch change";
    case ReleaseReason::command_id_exhaustion: return "command ID exhaustion";
    }
    return "unknown";
}
const char* local_command_state_name(LocalCommandState state) {
    switch (state) {
    case LocalCommandState::queued: return "queued locally";
    case LocalCommandState::awaiting_response: return "sent; awaiting Pi response";
    case LocalCommandState::accepted: return "accepted by Pi";
    case LocalCommandState::backpressure: return "Pi queue full; retrying";
    case LocalCommandState::completed: return "completed by USB writer";
    case LocalCommandState::rejected: return "rejected before acceptance";
    case LocalCommandState::cancelled: return "cancelled";
    case LocalCommandState::unknown: return "accepted; completion unknown";
    case LocalCommandState::release_retrying: return "release not completed; retrying same ID";
    }
    return "unknown";
}
size_t InjectedButtonManager::button_index(int button) {
    if (button < 1 || button > 8)
        throw std::invalid_argument("Mouse button must be 1..8");
    return size_t(button - 1);
}
uint8_t InjectedButtonManager::mask_locked() const {
    uint8_t result = manual_;
    for (size_t i = 0; i < scoped_.size(); ++i)
        if (scoped_[i])
            result |= uint8_t(1u << i);
    return result;
}
bool InjectedButtonManager::release_barrier_locked() const {
    return std::any_of(requests_.begin(), requests_.end(), [](const PendingRequest& request) {
        return request.command.operation == ClickOperation::release_all;
    });
}
size_t InjectedButtonManager::scheduled_count_locked() const {
    return size_t(std::count_if(requests_.begin(), requests_.end(), [](const PendingRequest& request) {
        return request.command.operation == ClickOperation::schedule;
    }));
}
void InjectedButtonManager::changed_locked(uint8_t before) {
    if (mask_locked() != before)
        ++revision_;
}
uint64_t InjectedButtonManager::allocate_command_locked() {
    if (!next_command_)
        throw std::overflow_error("Click command ID space exhausted");
    return next_command_++;
}
void InjectedButtonManager::remember_locked(const PendingRequest& request, LocalCommandState state) {
    history_.push_back({request.command.command, request.command.operation, request.command.button,
                        request.command.count, request.accepted_clicks, request.completed_clicks, state});
    while (history_.size() > max_history)
        history_.pop_front();
}
void InjectedButtonManager::cancel_clicks_locked() {
    for (auto it = requests_.begin(); it != requests_.end();) {
        if (it->command.operation == ClickOperation::release_all) {
            ++it;
            continue;
        }
        if (it->accepted) {
            ++metrics_.click_accepted_failed;
            remember_locked(*it, LocalCommandState::unknown);
        } else {
            ++metrics_.click_rejected_before_acceptance;
            remember_locked(*it, LocalCommandState::cancelled);
        }
        it = requests_.erase(it);
    }
}
void InjectedButtonManager::begin_release_locked(ReleaseReason reason, bool force_snapshots) {
    const auto before = mask_locked();
    manual_ = 0;
    scoped_ = {};
    ++hold_generation_;
    if (before)
        ++revision_;
    cancel_clicks_locked();
    last_release_ = reason;
    metrics_.last_release_reason = release_reason_name(reason);
    if (before || force_snapshots)
        ++release_snapshot_batches_;
    if (next_command_) {
        ClickCommand command{ClickOperation::release_all, click_client_, allocate_command_locked(), 0, 0, 0, 0};
        PendingRequest pending;
        pending.command = command;
        pending.bytes = encode_click_request(command);
        requests_.push_back(pending);
        metrics_.last_release_all_state = local_command_state_name(LocalCommandState::queued);
    } else {
        metrics_.last_release_all_state = "command ID exhausted; existing fence retained";
    }
}
void InjectedButtonManager::finish_locked(std::deque<PendingRequest>::iterator request,
                                          LocalCommandState state) {
    remember_locked(*request, state);
    requests_.erase(request);
}
InjectedButtonManager::InjectedButtonManager(uint64_t click_client)
    : click_client_(click_client ? click_client : random_id()) {}
void InjectedButtonManager::set_wake(std::function<void()> wake) {
    std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
}
void InjectedButtonManager::reset(uint64_t client) {
    if (!client)
        throw std::invalid_argument("Click client session must be nonzero");
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        manual_ = 0;
        scoped_ = {};
        ++hold_generation_;
        revision_ = 0;
        release_snapshot_batches_ = 0;
        last_release_ = ReleaseReason::none;
        click_client_ = client;
        next_command_ = 1;
        click_server_ = 0;
        endpoint_poll_us_ = 0;
        requests_.clear();
        history_.clear();
        metrics_ = {};
        wake = wake_;
    }
    if (wake)
        wake();
}
void InjectedButtonManager::button_down(int button) { set_button(button, true); }
void InjectedButtonManager::button_up(int button) { set_button(button, false); }
void InjectedButtonManager::set_button(int button, bool state) {
    const auto index = button_index(button);
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        if (state && std::any_of(requests_.begin(), requests_.end(), [&](const PendingRequest& request) {
                return request.command.operation == ClickOperation::schedule &&
                       request.command.button == index + 1;
            }))
            throw std::runtime_error("Cannot hold a button while its scheduled click is pending; use ReleaseAll first");
        const auto before = mask_locked();
        if (state)
            manual_ |= uint8_t(1u << index);
        else
            manual_ &= uint8_t(~(1u << index));
        changed_locked(before);
        if (mask_locked() != before)
            wake = wake_;
    }
    if (wake)
        wake();
}
void InjectedButtonManager::release_all() { release_all(ReleaseReason::manual); }
void InjectedButtonManager::release_all(ReleaseReason reason, bool force_snapshots) {
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        begin_release_locked(reason, force_snapshots);
        wake = wake_;
    }
    if (wake)
        wake();
}
InjectedButtonManager::Hold InjectedButtonManager::hold(int button) {
    const auto index = button_index(button);
    std::function<void()> wake;
    uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (std::any_of(requests_.begin(), requests_.end(), [&](const PendingRequest& request) {
                return request.command.operation == ClickOperation::schedule &&
                       request.command.button == index + 1;
            }))
            throw std::runtime_error("Cannot hold a button while its scheduled click is pending; use ReleaseAll first");
        const auto before = mask_locked();
        generation = hold_generation_;
        if (scoped_[index] == std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("Too many scoped holds for mouse button");
        ++scoped_[index];
        changed_locked(before);
        if (mask_locked() != before)
            wake = wake_;
    }
    if (wake)
        wake();
    return Hold(this, index, generation);
}
void InjectedButtonManager::release_scoped(size_t index, uint64_t generation) noexcept {
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        if (generation != hold_generation_ || !scoped_[index])
            return;
        const auto before = mask_locked();
        --scoped_[index];
        changed_locked(before);
        if (mask_locked() != before)
            wake = wake_;
    }
    if (wake)
        wake();
}
InjectedButtonManager::Hold::Hold(Hold&& other) noexcept
    : owner_(other.owner_), index_(other.index_), generation_(other.generation_) {
    other.owner_ = nullptr;
}
InjectedButtonManager::Hold& InjectedButtonManager::Hold::operator=(Hold&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        index_ = other.index_;
        generation_ = other.generation_;
        other.owner_ = nullptr;
    }
    return *this;
}
InjectedButtonManager::Hold::~Hold() { release(); }
void InjectedButtonManager::Hold::release() noexcept {
    if (owner_) {
        owner_->release_scoped(index_, generation_);
        owner_ = nullptr;
    }
}
uint64_t InjectedButtonManager::click(int button, uint32_t count,
                                      std::chrono::microseconds press_duration,
                                      std::chrono::microseconds interval) {
    const auto index = button_index(button);
    const auto press = press_duration.count(), gap = interval.count();
    if (!count || count > 10'000 || press <= 0 || press > 5'000'000 || gap < 0 || gap > 60'000'000 ||
        uint64_t(press) > std::numeric_limits<uint32_t>::max() ||
        uint64_t(gap) > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Click count/durations are outside UPC1 v2 limits");
    const uint64_t total = uint64_t(count) * uint64_t(press) + uint64_t(count - 1) * uint64_t(gap);
    if (total < uint64_t(press))
        throw std::overflow_error("Click schedule duration overflow");
    std::function<void()> wake;
    uint64_t id = 0;
    bool exhausted = false;
    {
        std::lock_guard lock(mutex_);
        if (!click_server_ || !endpoint_poll_us_)
            throw std::runtime_error("UPC1 v2 requires valid UPT3 capability and endpoint timing");
        if (press < endpoint_poll_us_)
            throw std::invalid_argument("Click press duration is below the USB endpoint polling interval");
        if (mask_locked() & (1u << index))
            throw std::runtime_error("Cannot schedule a click on a persistently held button");
        if (scheduled_count_locked() >= max_pending_clicks) {
            ++metrics_.click_rejected_before_acceptance;
            throw std::runtime_error("Local click queue is full");
        }
        if (next_command_ == std::numeric_limits<uint64_t>::max()) {
            begin_release_locked(ReleaseReason::command_id_exhaustion, true);
            wake = wake_;
            exhausted = true;
        } else {
            id = allocate_command_locked();
            ClickCommand command{ClickOperation::schedule, click_client_, id, uint8_t(index + 1), count,
                                 uint32_t(press), uint32_t(gap)};
            PendingRequest pending;
            pending.command = command;
            pending.bytes = encode_click_request(command);
            requests_.push_back(pending);
            ++metrics_.click_submitted;
            wake = wake_;
        }
    }
    if (wake)
        wake();
    if (exhausted)
        throw std::overflow_error("Click command IDs exhausted; ReleaseAll fence started");
    return id;
}
InjectionSnapshot InjectedButtonManager::snapshot() const {
    std::lock_guard lock(mutex_);
    return {mask_locked(), revision_, release_snapshot_batches_, release_barrier_locked()};
}
void InjectedButtonManager::mark_snapshot_sent(uint64_t revision, uint8_t mask, bool release_batch) {
    std::lock_guard lock(mutex_);
    if (release_batch && release_snapshot_batches_)
        --release_snapshot_batches_;
    (void)revision;
    (void)mask;
}
bool InjectedButtonManager::observe_telemetry(const Telemetry& telemetry) {
    if (!telemetry.server)
        return false;
    std::function<void()> wake;
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        if (!click_server_) {
            click_server_ = telemetry.server;
        } else if (click_server_ != telemetry.server) {
            ++metrics_.click_epoch_resets;
            cancel_clicks_locked();
            for (const auto& request : requests_)
                remember_locked(request, LocalCommandState::unknown);
            requests_.clear();
            click_client_ = random_id();
            next_command_ = 1;
            click_server_ = telemetry.server;
            endpoint_poll_us_ = 0;
            begin_release_locked(ReleaseReason::server_epoch_change, true);
            changed = true;
            wake = wake_;
        }
        if (telemetry.kind == Telemetry::Kind::upt3) {
            endpoint_poll_us_ = telemetry.ready ? telemetry.endpoint_poll_us : 0;
            metrics_.has_upt3 = true;
            metrics_.endpoint_poll_us = endpoint_poll_us_;
            metrics_.pi_applied_persistent_mask = telemetry.applied_persistent;
            metrics_.pi_scheduled_mask = telemetry.scheduled;
            metrics_.pi_accepted_click_total = telemetry.accepted_click_total;
            metrics_.pi_completed_click_total = telemetry.completed_click_total;
            metrics_.pi_active_sequences = telemetry.active_sequences;
            metrics_.pi_queued_sequences = telemetry.queued_sequences;
            metrics_.pi_physical_reports_received = telemetry.physical_reports_received;
            metrics_.pi_physical_reports_submitted = telemetry.physical_reports_submitted;
            metrics_.pi_output_queue_depth = telemetry.output_queue_depth;
            metrics_.pi_pending_synthetic_depth = telemetry.pending_synthetic_depth;
            metrics_.pi_superseded_synthetic = telemetry.superseded_synthetic;
            metrics_.pi_writer_failures = telemetry.writer_failures;
        } else {
            endpoint_poll_us_ = 0;
            metrics_.has_upt3 = false;
            metrics_.endpoint_poll_us = 0;
            metrics_.pi_applied_persistent_mask = 0;
            metrics_.pi_scheduled_mask = 0;
            metrics_.pi_accepted_click_total = 0;
            metrics_.pi_completed_click_total = 0;
            metrics_.pi_active_sequences = 0;
            metrics_.pi_queued_sequences = 0;
            metrics_.pi_physical_reports_received = 0;
            metrics_.pi_physical_reports_submitted = 0;
            metrics_.pi_output_queue_depth = 0;
            metrics_.pi_pending_synthetic_depth = 0;
            metrics_.pi_superseded_synthetic = 0;
            metrics_.pi_writer_failures = 0;
        }
    }
    if (wake)
        wake();
    return changed;
}
std::vector<OutboundClickRequest> InjectedButtonManager::due_requests(int64_t now) {
    std::vector<OutboundClickRequest> result;
    std::lock_guard lock(mutex_);
    const bool barrier = release_barrier_locked();
    for (auto& pending : requests_) {
        const bool release = pending.command.operation == ClickOperation::release_all;
        if (barrier && !release)
            break;
        if (terminal(pending.state))
            continue;
        const bool due = !pending.sent || now >= pending.next_send;
        if (due) {
            if (pending.sent) {
                if (release)
                    ++metrics_.release_retries;
                else {
                    ++metrics_.click_retries;
                    if (pending.state != LocalCommandState::backpressure)
                        ++metrics_.click_timeouts;
                }
            }
            pending.sent = true;
            pending.sent_at = now;
            pending.state = pending.accepted ? LocalCommandState::accepted
                                             : LocalCommandState::awaiting_response;
            pending.next_send = now + (pending.accepted ? accepted_lease_ns : response_timeout_ns);
            if (release)
                metrics_.last_release_all_state = local_command_state_name(pending.state);
            result.push_back({pending.command.command, release, pending.bytes});
        }
        if (!pending.accepted)
            break;
    }
    return result;
}
AckResult InjectedButtonManager::process_click_ack(Bytes bytes, int64_t now) {
    auto ack = parse_click_ack(bytes);
    if (!ack)
        return AckResult::protocol_error;
    std::function<void()> wake;
    AckResult outcome = AckResult::handled;
    {
        std::lock_guard lock(mutex_);
        if (ack->server != click_server_ || ack->client != click_client_)
            return AckResult::ignored;
        auto found = std::find_if(requests_.begin(), requests_.end(),
                                  [&](const PendingRequest& request) {
                                      return request.command.command == ack->command;
                                  });
        if (found == requests_.end() || !found->sent)
            return AckResult::ignored;
        const bool release = found->command.operation == ClickOperation::release_all;
        const uint8_t expected_button = release ? 0 : found->command.button;
        if (ack->button != expected_button || ack->completed_clicks > ack->accepted_clicks ||
            ack->accepted_clicks > found->command.count || ack->completed_clicks > found->command.count ||
            (release && (ack->accepted_clicks || ack->completed_clicks)))
            return AckResult::protocol_error;
        const bool success = ack->status == ClickStatus::accepted ||
                             ack->status == ClickStatus::duplicate ||
                             ack->status == ClickStatus::completed;
        if (!release && success && ack->accepted_clicks != found->command.count)
            return AckResult::protocol_error;
        if (!release && ack->status == ClickStatus::completed &&
            ack->completed_clicks != found->command.count)
            return AckResult::protocol_error;
        const bool zero_count_status = ack->status == ClickStatus::busy ||
                                       ack->status == ClickStatus::queue_full ||
                                       ack->status == ClickStatus::unsupported_button ||
                                       ack->status == ClickStatus::invalid ||
                                       ack->status == ClickStatus::button_active ||
                                       ack->status == ClickStatus::not_ready ||
                                       ack->status == ClickStatus::stale_command;
        if (zero_count_status && (ack->accepted_clicks || ack->completed_clicks))
            return AckResult::protocol_error;
        if (ack->status == ClickStatus::queue_full && found->accepted)
            return AckResult::protocol_error;
        found->accepted_clicks = ack->accepted_clicks;
        found->completed_clicks = ack->completed_clicks;
        switch (ack->status) {
        case ClickStatus::accepted:
        case ClickStatus::duplicate:
            if (!found->accepted) {
                found->accepted = true;
                if (!release)
                    ++metrics_.click_accepted;
            }
            found->state = LocalCommandState::accepted;
            found->next_send = now + accepted_lease_ns;
            if (release)
                metrics_.last_release_all_state = local_command_state_name(LocalCommandState::accepted);
            break;
        case ClickStatus::completed:
            if (!found->accepted && !release)
                ++metrics_.click_accepted;
            if (!release)
                ++metrics_.click_completed;
            else
                metrics_.last_release_all_state = local_command_state_name(LocalCommandState::completed);
            finish_locked(found, LocalCommandState::completed);
            if (release && !next_command_ && !release_barrier_locked()) {
                click_client_ = random_id();
                next_command_ = 1;
            }
            wake = wake_;
            break;
        case ClickStatus::queue_full: {
            ++metrics_.click_queue_full;
            found->state = LocalCommandState::backpressure;
            if (release)
                metrics_.last_release_all_state = local_command_state_name(LocalCommandState::backpressure);
            const uint8_t attempt = std::min<uint8_t>(found->queue_full_attempts++, 2);
            constexpr int64_t delays[] = {25'000'000, 50'000'000, 75'000'000};
            found->next_send = now + delays[attempt];
            break;
        }
        case ClickStatus::busy:
        case ClickStatus::unsupported_button:
        case ClickStatus::button_active:
        case ClickStatus::not_ready:
        case ClickStatus::invalid:
        case ClickStatus::stale_command:
        case ClickStatus::cancelled: {
            if (release) {
                // A rejected ReleaseAll never opens the fence: safety requires the same
                // immutable ID to remain live until Completed or a confirmed epoch reset.
                found->state = LocalCommandState::release_retrying;
                found->next_send = now + response_timeout_ns;
                metrics_.last_release_all_state =
                    local_command_state_name(LocalCommandState::release_retrying);
                if (ack->status == ClickStatus::invalid || ack->status == ClickStatus::stale_command)
                    outcome = AckResult::protocol_error;
                break;
            }
            if (found->accepted)
                ++metrics_.click_accepted_failed;
            else
                ++metrics_.click_rejected_before_acceptance;
            const auto terminal_state = ack->status == ClickStatus::cancelled
                                            ? LocalCommandState::cancelled
                                            : LocalCommandState::rejected;
            finish_locked(found, terminal_state);
            wake = wake_;
            if (ack->status == ClickStatus::invalid || ack->status == ClickStatus::stale_command)
                outcome = AckResult::protocol_error;
            break;
        }
        }
    }
    if (wake)
        wake();
    return outcome;
}
InjectionMetrics InjectedButtonManager::metrics() const {
    std::lock_guard lock(mutex_);
    auto result = metrics_;
    result.persistent_mask = mask_locked();
    result.pending_clicks = scheduled_count_locked();
    result.last_release_reason = release_reason_name(last_release_);
    result.commands.assign(history_.begin(), history_.end());
    for (const auto& request : requests_)
        result.commands.push_back({request.command.command, request.command.operation,
                                   request.command.button, request.command.count,
                                   request.accepted_clicks, request.completed_clicks, request.state});
    return result;
}
uint64_t InjectedButtonManager::click_client_session() const {
    std::lock_guard lock(mutex_);
    return click_client_;
}
} // namespace receiver
