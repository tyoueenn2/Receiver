#include "receiver/app.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>

using namespace receiver;
namespace {
template <class F> void until(F predicate) {
    const auto deadline = now_ns() + 3'000'000'000ll;
    while (!predicate()) {
        if (now_ns() > deadline)
            throw std::runtime_error("Sender synchronization timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
class FakeBackend : public Backend {
    int size_;
    std::array<float, 5> output_{80, 80, 20, 20, .9f};

  public:
    explicit FakeBackend(int size) : size_(size) {}
    Inference run(const Frame&) override {
        return {output_, 1, 1, 0, .1};
    }
    std::string description() const override {
        return "Sender sync test";
    }
    std::vector<std::string> class_names() const override {
        return {"object"};
    }
    int input_size() const override {
        return size_;
    }
};

// Independent control peer: decode UVN1's bytes and send frames using the
// requested shape. Clock service also proves negotiation shares the owned socket.
class Peer {
    Address target_;
    int64_t last_hello_ = 0, last_frame_ = 0;
    uint32_t sequence_ = 0;
    uint16_t size_ = 0;

  public:
    UdpSocket socket{"127.0.0.1", 0};
    uint64_t session = 0x0102030405060708ull;
    std::vector<uint16_t> requests;
    bool frames = false;
    explicit Peer(int port) : target_(address("127.0.0.1", port)) {}
    void poll(bool heartbeat = true) {
        auto now = now_ns();
        if (heartbeat && now - last_hello_ >= 20'000'000) {
            std::array<uint8_t, 16> hello{};
            std::memcpy(hello.data(), "UVH1", 4);
            put64(hello.data() + 8, session);
            CHECK(socket.send(hello, target_));
            last_hello_ = now;
        }
        std::array<uint8_t, 256> packet{};
        Address from{};
        for (int i = 0; i < 64; ++i) {
            int n = socket.receive(packet, from, 0);
            if (n <= 0)
                break;
            CHECK(from == target_);
            if (n != 24 || be64(packet.data() + 8) != session)
                continue;
            if (!std::memcmp(packet.data(), "UVC1", 4)) {
                std::array<uint8_t, 40> reply{};
                std::memcpy(reply.data(), "UVS1", 4);
                put64(reply.data() + 8, session);
                put64(reply.data() + 16, be64(packet.data() + 16));
                put64(reply.data() + 24, uint64_t(now_ns()));
                put64(reply.data() + 32, uint64_t(now_ns()));
                CHECK(socket.send(reply, target_));
            } else if (!std::memcmp(packet.data(), "UVN1", 4)) {
                CHECK(packet[4] == 1 && packet[5] == 2 && be16(packet.data() + 6) == 0);
                CHECK(be32(packet.data() + 20) == 0);
                size_ = be16(packet.data() + 16);
                CHECK(size_ >= 1 && size_ <= 1024 && be16(packet.data() + 18) == size_);
                requests.push_back(size_);
            }
        }
        if (frames && size_ && now - last_frame_ >= 25'000'000) {
            FrameHeader h;
            h.session = session;
            h.sequence = sequence_++;
            h.capture_ns = now_ns();
            h.width = h.height = size_;
            h.format = 2;
            h.bytes = uint32_t(size_) * size_ * 4;
            h.stride = 1352;
            h.count = uint16_t((h.bytes + h.stride - 1) / h.stride);
            for (h.index = 0; h.index < h.count; ++h.index) {
                h.offset = uint32_t(h.index) * h.stride;
                auto header = encode_frame(h);
                std::vector<uint8_t> data(header.begin(), header.end());
                data.resize(48 + std::min<uint32_t>(h.stride, h.bytes - h.offset), 114);
                CHECK(socket.send(data, target_));
            }
            last_frame_ = now;
        }
    }
    void pump_for(int ms, bool heartbeat = true) {
        const auto deadline = now_ns() + int64_t(ms) * 1'000'000;
        while (now_ns() < deadline) {
            poll(heartbeat);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    void expect(uint16_t size) {
        until([&] {
            poll();
            return !requests.empty() && requests.back() == size;
        });
    }
};

void protocol_vectors() {
    constexpr std::array<uint8_t, 24> expected{
        'U', 'V', 'N', '1', 1, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8,
        0x02, 0x80, 0x01, 0x40, 0, 0, 0, 0};
    CHECK(encode_resolution_request(0x0102030405060708ull, 640, 320) == expected);
    test::rejects([] { encode_resolution_request(0, 320, 320); });
    test::rejects([] { encode_resolution_request(1, 0, 320); });
    test::rejects([] { encode_resolution_request(1, 320, 1025); });
}

void synchronization() {
    std::atomic<bool> initial_allowed{false}, replacement_allowed{false};
    App app([&](const Settings& s, const BackendProgress&, const BackendCancel& cancel) {
        while ((s.model == "initial.pt" && !initial_allowed) ||
               (s.model == "replacement.pt" && !replacement_allowed)) {
            if (cancel())
                throw std::runtime_error("Cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (s.model == "broken.pt")
            throw std::runtime_error("Bad checkpoint");
        return std::make_unique<FakeBackend>(s.model == "replacement.pt" ? 320 : 160);
    });
    Settings s;
    s.model = "initial.pt";
    s.bind_ip = s.sender_ip = s.pi_ip = "127.0.0.1";
    s.frame_port = 30000 + int(random_id() % 20000);
    s.pi_port = s.frame_port + 1;
    s.preview = true;
    s.max_age_ms = 250;
    app.start(s);
    Peer peer(s.frame_port);
    peer.pump_for(100);
    CHECK(peer.requests.empty()); // Model loading must not publish a guessed profile size.
    initial_allowed = true;
    peer.expect(160);
    until([&] { peer.poll(); return peer.requests.size() >= 2; }); // First request can be lost.
    CHECK(std::all_of(peer.requests.begin(), peer.requests.end(), [](auto size) { return size == 160; }));
    peer.frames = true;
    until([&] {
        peer.poll();
        auto preview = app.preview();
        return app.stats().synchronized && app.stats().inferred && preview.frame &&
               preview.frame->header.width == 160 && preview.frame->header.format == 2;
    });

    s = app.settings();
    s.model = "replacement.pt";
    s.input_size = 512;
    app.configure(s);
    peer.requests.clear();
    peer.pump_for(300);
    CHECK(!peer.requests.empty());
    CHECK(std::all_of(peer.requests.begin(), peer.requests.end(), [](auto size) { return size == 160; }));
    replacement_allowed = true;
    peer.expect(320); // The loaded backend's size wins over the requested 512.
    until([&] {
        peer.poll();
        auto preview = app.preview();
        return preview.frame && preview.frame->header.width == 320;
    });

    s = app.settings();
    s.model = "broken.pt";
    s.input_size = 960;
    app.configure(s);
    until([&] { peer.poll(); return !app.stats().model_error.empty(); });
    peer.requests.clear();
    peer.expect(320);
    CHECK(app.settings().input_size == 320 && !app.stats().armed);

    peer.frames = false;
    peer.pump_for(180, false);
    peer.requests.clear();
    peer.pump_for(300, false);
    CHECK(peer.requests.empty()); // Stop negotiating with an expired sender lease.
    peer.expect(320);
    ++peer.session;
    peer.requests.clear();
    peer.expect(320); // Same endpoint, fresh sender session.

    app.stop();
    s = app.settings();
    app.start(s);
    peer.requests.clear();
    peer.expect(320); // Receiver restart with the sender still running.
    s = app.settings();
    s.color.mode = 1;
    s.input_size = 64;
    app.configure(s);
    peer.expect(64);
    CHECK(app.stats().error.empty() && !app.stats().armed);
    app.stop();
}
} // namespace
int main() {
    try {
        protocol_vectors();
        synchronization();
        std::cout << test::checks << " sender synchronization checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
