#include "receiver/video_preview.hpp"
#include <algorithm>
#include <stdexcept>

namespace receiver {
VideoState VideoSnapshot::state(int64_t now) const noexcept {
    if (!error.empty())
        return VideoState::error;
    if (!running)
        return VideoState::off;
    if (!frame)
        return VideoState::waiting;
    return now - frame->complete_ns < 500'000'000 ? VideoState::live : VideoState::stale;
}

const char* video_state_label(VideoState state) {
    switch (state) {
    case VideoState::off: return "Off";
    case VideoState::waiting: return "Waiting";
    case VideoState::live: return "Live";
    case VideoState::stale: return "Stale";
    case VideoState::error: return "Error";
    }
    return "Off";
}

void VideoPreview::start_udp(const std::string& bind_ip, int port, const std::string& sender_ip) {
    stop();
    if (!port)
        throw std::invalid_argument("Choose a nonzero UDP video port.");
    (void)address(bind_ip, port);
    const auto allowed = address(sender_ip, 0);
    {
        std::lock_guard lock(mutex_);
        snapshot_.running = true;
        stop_ = false;
    }
    try {
        reader_ = std::thread([this, bind_ip, port, allowed] {
            try {
                receive_udp(bind_ip, port, allowed);
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex_);
                snapshot_.running = false;
                snapshot_.frame.reset();
                snapshot_.error = e.what();
            }
        });
    } catch (...) {
        stop();
        throw;
    }
}

void VideoPreview::stop() {
    stop_ = true;
    if (reader_.joinable())
        reader_.join();
    std::lock_guard lock(mutex_);
    snapshot_ = {};
}

VideoSnapshot VideoPreview::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void VideoPreview::receive_udp(const std::string& bind_ip, int port, Address allowed) {
    UdpSocket socket(bind_ip, port);
    Reassembler reassembly;
    Address peer{};
    uint64_t session = 0;
    int64_t last_hello = 0, last_size_request = 0;
    std::array<uint64_t, 16> retired{};
    size_t retire_at = 0;
    std::array<uint8_t, 2048> buffer{};
    // Only video negotiation is sent. With the sender's automatic crop mode,
    // request a fixed 320 px square independently of any model configuration.
    while (!stop_) {
        Address from;
        const int size = socket.receive(buffer, from, 20);
        const auto now = now_ns();
        if (size <= 0) {
            reassembly.expire(now);
            continue;
        }
        const Bytes packet(buffer.data(), size_t(size));
        const auto hello = size == 16 && std::memcmp(packet.data(), "UVH1", 4) == 0 &&
                                   be32(packet.data() + 4) == 0
                               ? be64(packet.data() + 8)
                               : uint64_t{0};
        if (from.ip != allowed.ip) {
            if (hello) {
                std::lock_guard lock(mutex_);
                snapshot_.mismatched_sender_ns = now;
            }
            continue;
        }
        if (hello) {
            const auto next = hello;
            if (!next || std::find(retired.begin(), retired.end(), next) != retired.end() ||
                (session && from != peer && now - last_hello < 100'000'000))
                continue;
            if (next != session || from != peer) {
                // A different endpoint may reuse a session ID; retire only a different ID.
                if (session && session != next)
                    retired[retire_at++ % retired.size()] = session;
                session = next;
                peer = from;
                reassembly.reset(session);
                last_size_request = 0;
                std::lock_guard lock(mutex_);
                snapshot_.frame.reset();
            }
            last_hello = now;
            if (!last_size_request || now - last_size_request >= 250'000'000) {
                const auto size_request = encode_resolution_request(session, 320, 320);
                if (socket.send(size_request, peer))
                    last_size_request = now;
            }
        } else if (session && from == peer && size >= int(frame_header_size) &&
                   std::memcmp(packet.data(), "UVF1", 4) == 0) {
            if (auto frame = reassembly.accept(packet, now)) {
                std::lock_guard lock(mutex_);
                snapshot_.frame = std::move(frame);
                ++snapshot_.completed;
                snapshot_.mismatched_sender_ns = 0;
            }
        }
    }
}
} // namespace receiver
