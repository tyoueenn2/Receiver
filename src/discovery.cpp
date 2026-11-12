#include "receiver/discovery.hpp"
#include "receiver/move_protocol.hpp"
namespace receiver {
DiscoveryBeacon::DiscoveryBeacon(const Settings& settings, uint16_t port) : socket_(port) {
    if (port && settings.frame_source == "udp" && settings.frame_port == port)
        throw std::runtime_error("Video and discovery need different UDP ports");
    configure(settings);
    thread_ = std::thread([this] { run(); });
}
DiscoveryBeacon::~DiscoveryBeacon() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}
void DiscoveryBeacon::configure(const Settings& settings) {
    std::lock_guard lock(mutex_);
    bind_ip_ = settings.bind_ip;
    frame_port_ = settings.frame_source == "udp" ? uint16_t(settings.frame_port) : 0;
}
std::string DiscoveryBeacon::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}
void DiscoveryBeacon::run() {
    try {
        auto next_reply = std::chrono::steady_clock::now();
        while (!stop_) {
            std::array<uint8_t, 256> packet{};
            lan_discovery::Endpoint from;
            const auto count = socket_.receive(packet, from);
            if (count <= 0)
                continue;
            const auto nonce = lan_discovery::parse_query({packet.data(), size_t(count)});
            const auto now = std::chrono::steady_clock::now();
            if (!nonce || now < next_reply)
                continue;
            std::lock_guard lock(mutex_);
            if (frame_port_)
                socket_.send(lan_discovery::reply(nonce, frame_port_, bind_ip_), from);
            next_reply = now + std::chrono::milliseconds(10);
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        error_ = e.what();
    }
}
std::vector<lan_discovery::Endpoint> discover_pi(uint16_t port, std::vector<std::string> targets,
                                               int timeout_ms) {
    const auto session = lan_discovery::token(), token = lan_discovery::token();
    // UPQ1 only reads capabilities; UPS subscriptions would displace the active controller.
    return lan_discovery::scan(move_protocol::query(session, token), port,
        [=](auto packet, auto from) -> std::optional<lan_discovery::Endpoint> {
            move_protocol::Capabilities capabilities;
            if (from.port != port || !move_protocol::parse_capabilities(packet.data(), packet.size(), capabilities) ||
                capabilities.session != session || capabilities.token != token)
                return {};
            from.ready = capabilities.ready;
            return from;
        }, std::move(targets), timeout_ms);
}
} // namespace receiver
