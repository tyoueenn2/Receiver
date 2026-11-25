#pragma once
#include "network.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace receiver {
enum class VideoState { off, waiting, live, stale, error };

struct VideoSnapshot {
    bool running = false;
    std::shared_ptr<const Frame> frame;
    uint64_t completed = 0;
    int64_t mismatched_sender_ns = 0;
    std::string error;

    VideoState state(int64_t now = now_ns()) const noexcept;
    bool recent_sender_mismatch(int64_t now = now_ns()) const noexcept {
        return running && mismatched_sender_ns && now - mismatched_sender_ns < 2'000'000'000;
    }
};

const char* video_state_label(VideoState state);

// Deliberately separate from App: accepts video only, with no model or output-device dependencies.
class VideoPreview {
    mutable std::mutex mutex_;
    std::atomic<bool> stop_{true};
    std::thread reader_;
    VideoSnapshot snapshot_;
    void receive_udp(const std::string& bind_ip, int port, Address allowed);

  public:
    VideoPreview() = default;
    VideoPreview(const VideoPreview&) = delete;
    VideoPreview& operator=(const VideoPreview&) = delete;
    ~VideoPreview() { stop(); }
    // Start/stop are called by the same owner (the GUI thread).
    void start_udp(const std::string& bind_ip, int port, const std::string& sender_ip);
    void stop();
    VideoSnapshot snapshot() const;
};
} // namespace receiver
