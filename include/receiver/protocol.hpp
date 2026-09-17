#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace receiver {
using Bytes = std::span<const uint8_t>;
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline uint16_t be16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
    return uint32_t(be16(p)) << 16 | be16(p + 2);
}
inline uint64_t be64(const uint8_t* p) {
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}
inline void put16(uint8_t* p, uint16_t x) {
    p[0] = uint8_t(x >> 8);
    p[1] = uint8_t(x);
}
inline void put32(uint8_t* p, uint32_t x) {
    put16(p, uint16_t(x >> 16));
    put16(p + 2, uint16_t(x));
}
inline void put64(uint8_t* p, uint64_t x) {
    put32(p, uint32_t(x >> 32));
    put32(p + 4, uint32_t(x));
}
inline bool newer(uint32_t a, uint32_t b) {
    auto d = a - b;
    return d != 0 && d < 0x80000000u;
}
constexpr size_t frame_header_size = 48, max_frame_bytes = 1024 * 1024 * 4, max_fragments = 4096;
struct FrameHeader {
    uint64_t session = 0;
    uint32_t sequence = 0;
    int64_t capture_ns = 0;
    uint16_t width = 0, height = 0;
    uint8_t format = 1;
    uint32_t bytes = 0, offset = 0;
    uint16_t index = 0, count = 0, stride = 0;
    bool operator==(const FrameHeader&) const = default;
};
std::optional<FrameHeader> parse_frame(Bytes packet);
std::array<uint8_t, 48> encode_frame(const FrameHeader& h);
struct Frame {
    FrameHeader header;
    int64_t first_ns = 0, complete_ns = 0;
    std::vector<uint8_t> pixels;
    Frame() : pixels(max_frame_bytes) {}
};
struct ReassemblyStats {
    uint64_t packets = 0, invalid = 0, duplicates = 0, expired = 0, evicted = 0, completed = 0, old = 0,
             pool_drops = 0;
};
class Reassembler {
    struct Slot {
        std::shared_ptr<Frame> frame;
        std::array<uint8_t, max_fragments> seen{};
        uint16_t received = 0;
    };
    std::array<Slot, 3> slots_{};
    std::array<std::shared_ptr<Frame>, 7> pool_;
    uint64_t session_ = 0;
    uint32_t completed_ = 0;
    bool have_completed_ = false;

  public:
    ReassemblyStats stats;
    Reassembler();
    void reset(uint64_t session);
    void expire(int64_t now);
    std::shared_ptr<const Frame> accept(Bytes data, int64_t now);
};
// Sender clock minus receiver clock is bounded by [lower,upper].
class ClockSync {
    int64_t lower_ = 0, upper_ = 0, updated_ = 0;

  public:
    void reset() {
        updated_ = 0;
    }
    bool observe(int64_t t0, int64_t t1, int64_t t2, int64_t t3);
    bool synchronized(int64_t now) const {
        return updated_ && now >= updated_ && now - updated_ <= 2'000'000'000;
    }
    std::optional<int64_t> age_upper(int64_t capture, int64_t now) const;
    double uncertainty_ms() const {
        return double(upper_ - lower_) / 1e6;
    }
};
struct Telemetry {
    enum class Kind : uint8_t { upt1, upt2_public, upt2_legacy, upt3 };
    Kind kind = Kind::upt1;
    uint64_t client = 0, server = 0, token = 0;
    uint32_t sequence = 0;
    bool ready = false;
    uint8_t physical = 0, applied_persistent = 0, scheduled = 0;
    int xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    bool has_motion = false;
    bool motion_counters_64 = false;
    uint32_t endpoint_poll_us = 0, motion_age_us = 0;
    uint64_t motion_generation = 0, sample_ns = 0;
    int64_t total_x = 0, total_y = 0;
    int16_t last_physical_dx = 0, last_physical_dy = 0;
    uint32_t accepted_click_total = 0, completed_click_total = 0;
    uint16_t active_sequences = 0, queued_sequences = 0;
    uint32_t physical_reports_received = 0, physical_reports_submitted = 0;
    uint16_t output_queue_depth = 0, pending_synthetic_depth = 0;
    uint32_t superseded_synthetic = 0, writer_failures = 0;
};
enum class SubscriptionVersion : uint8_t { v1 = 1, v2 = 2, v3 = 3 };
std::array<uint8_t, 24> subscribe(uint64_t client, uint64_t token,
                                  SubscriptionVersion version = SubscriptionVersion::v1);
inline std::array<uint8_t, 24> subscribe(uint64_t client, uint64_t token, bool motion) {
    return subscribe(client, token, motion ? SubscriptionVersion::v2 : SubscriptionVersion::v1);
}
std::optional<Telemetry> parse_telemetry(Bytes bytes);
// UPX1 carries a complete synthetic-button snapshot. Physical buttons never belong here.
std::array<uint8_t, 16> movement(uint32_t sequence, int dx, int dy, int wheel, int pan,
                                 uint8_t injected_buttons);
enum class ClickStatus : uint8_t {
    accepted = 1,
    duplicate = 2,
    completed = 3,
    busy = 4,
    queue_full = 5,
    unsupported_button = 6,
    invalid = 7,
    cancelled = 8,
    button_active = 9,
    not_ready = 10,
    stale_command = 11,
};
enum class ClickOperation : uint8_t { schedule = 1, release_all = 2 };
struct ClickCommand {
    ClickOperation operation = ClickOperation::schedule;
    uint64_t client = 0, command = 0;
    uint8_t button = 0;
    uint32_t count = 0, press_us = 0, interval_us = 0;
};
struct ClickAck {
    ClickStatus status = ClickStatus::invalid;
    uint8_t button = 0;
    uint64_t client = 0, command = 0, server = 0;
    uint32_t accepted_clicks = 0, completed_clicks = 0;
    uint16_t queue_depth = 0;
};
std::array<uint8_t, 40> encode_click_request(const ClickCommand& command);
std::optional<ClickAck> parse_click_ack(Bytes bytes);
class TelemetryGate {
    struct IssuedToken {
        uint64_t token = 0;
        int64_t time = 0;
        SubscriptionVersion version = SubscriptionVersion::v1;
    };
    uint64_t client_ = 0, server_ = 0;
    uint32_t sequence_ = 0;
    bool have_ = false;
    int64_t received_ = 0, issued_ = 0;
    std::array<IssuedToken, 8> tokens_{};
    size_t next_ = 0;

  public:
    Telemetry state;
    void reset(uint64_t client);
    void issue(uint64_t token, int64_t time,
               SubscriptionVersion version = SubscriptionVersion::v1);
    bool accept(Bytes bytes, int64_t time);
    bool fresh(int64_t time) const;
};
} // namespace receiver
