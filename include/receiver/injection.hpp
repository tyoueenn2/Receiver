#pragma once
#include "protocol.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace receiver {
enum class ReleaseReason : uint8_t {
    none,
    manual,
    disarm,
    shutdown,
    sender_loss,
    sender_restart,
    stale_telemetry,
    pi_error,
    gpu_error,
    configuration_restart,
    server_epoch_change,
    command_id_exhaustion,
};
const char* release_reason_name(ReleaseReason reason);

enum class LocalCommandState : uint8_t {
    queued,
    awaiting_response,
    accepted,
    backpressure,
    completed,
    rejected,
    cancelled,
    interrupted,
    superseded,
    unknown,
    release_retrying,
};
const char* local_command_state_name(LocalCommandState state);

struct CommandView {
    uint64_t id = 0;
    ClickOperation operation = ClickOperation::schedule;
    uint8_t button = 0;
    uint32_t requested_clicks = 0, accepted_clicks = 0, completed_clicks = 0;
    LocalCommandState state = LocalCommandState::queued;
};
struct InjectionSnapshot {
    uint8_t mask = 0;
    uint64_t revision = 0;
    uint32_t release_snapshot_batches = 0;
    bool release_barrier = false;
    uint8_t wire_mask() const { return release_barrier ? 0 : mask; }
};
struct InjectionMetrics {
    uint8_t persistent_mask = 0, pi_applied_persistent_mask = 0, pi_scheduled_mask = 0;
    bool has_upt3 = false;
    uint32_t endpoint_poll_us = 0;
    uint64_t pending_clicks = 0, click_submitted = 0, click_accepted = 0, click_completed = 0,
             click_rejected_before_acceptance = 0, click_accepted_failed = 0, click_retries = 0,
             click_timeouts = 0, click_queue_full = 0, click_epoch_resets = 0,
             release_retries = 0, release_superseded = 0;
    uint32_t pi_accepted_click_total = 0, pi_completed_click_total = 0;
    uint16_t pi_active_sequences = 0, pi_queued_sequences = 0, pi_output_queue_depth = 0,
             pi_pending_synthetic_depth = 0;
    uint32_t pi_physical_reports_received = 0, pi_physical_reports_submitted = 0,
             pi_superseded_synthetic = 0, pi_writer_failures = 0;
    std::string last_release_reason = "none", last_release_all_state = "none";
    std::vector<CommandView> commands;
};
struct OutboundClickRequest {
    uint64_t id = 0;
    bool release_all = false;
    std::array<uint8_t, 40> bytes{};
};
enum class AckResult : uint8_t { handled, ignored, protocol_error };

constexpr int64_t injected_heartbeat_ns = 75'000'000;
bool injected_snapshot_due(const InjectionSnapshot& state, bool have_sent_revision,
                           uint64_t sent_revision, int64_t last_snapshot, int64_t now);

// Thread-safe owner of Receiver's synthetic state. Physical telemetry is accepted only
// for UPT3 capability/applied-state diagnostics and is never copied into the desired mask.
class InjectedButtonManager {
    struct PendingRequest {
        ClickCommand command;
        std::array<uint8_t, 40> bytes{};
        int64_t sent_at = 0, next_send = 0;
        uint32_t accepted_clicks = 0, completed_clicks = 0;
        uint8_t queue_full_attempts = 0;
        LocalCommandState state = LocalCommandState::queued;
        bool sent = false, accepted = false;
    };
    mutable std::mutex mutex_;
    uint8_t manual_ = 0;
    std::array<uint32_t, 8> scoped_{};
    uint64_t hold_generation_ = 1, revision_ = 0;
    uint32_t release_snapshot_batches_ = 0;
    ReleaseReason last_release_ = ReleaseReason::none;
    std::function<void()> wake_;
    uint64_t click_client_ = 0, next_command_ = 1, click_server_ = 0;
    uint32_t endpoint_poll_us_ = 0;
    std::deque<PendingRequest> requests_;
    std::deque<CommandView> history_;
    InjectionMetrics metrics_;

    static size_t button_index(int button);
    uint8_t mask_locked() const;
    bool release_barrier_locked() const;
    size_t scheduled_count_locked() const;
    void changed_locked(uint8_t before);
    void release_scoped(size_t index, uint64_t generation) noexcept;
    uint64_t allocate_command_locked();
    void remember_locked(const PendingRequest& request, LocalCommandState state);
    void cancel_clicks_locked();
    void begin_release_locked(ReleaseReason reason, bool force_snapshots);
    void finish_locked(std::deque<PendingRequest>::iterator request, LocalCommandState state);

  public:
    class Hold {
        InjectedButtonManager* owner_ = nullptr;
        size_t index_ = 0;
        uint64_t generation_ = 0;
        friend class InjectedButtonManager;
        Hold(InjectedButtonManager* owner, size_t index, uint64_t generation)
            : owner_(owner), index_(index), generation_(generation) {}

      public:
        Hold() = default;
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        Hold(Hold&& other) noexcept;
        Hold& operator=(Hold&& other) noexcept;
        ~Hold();
        void release() noexcept;
    };

    explicit InjectedButtonManager(uint64_t click_client = 0);
    void set_wake(std::function<void()> wake);
    void reset(uint64_t click_client);
    void button_down(int button);
    void button_up(int button);
    void set_button(int button, bool state);
    void release_all();
    void release_all(ReleaseReason reason, bool force_snapshots = true);
    Hold hold(int button);
    uint64_t click(int button, uint32_t count = 1,
                   std::chrono::microseconds press_duration = std::chrono::milliseconds(10),
                   std::chrono::microseconds interval = std::chrono::milliseconds(10));
    InjectionSnapshot snapshot() const;
    void mark_snapshot_sent(uint64_t revision, uint8_t mask, bool release_batch);
    bool observe_telemetry(const Telemetry& telemetry);
    std::vector<OutboundClickRequest> due_requests(int64_t now);
    AckResult process_click_ack(Bytes bytes, int64_t now);
    InjectionMetrics metrics() const;
    uint64_t click_client_session() const;
};
} // namespace receiver
