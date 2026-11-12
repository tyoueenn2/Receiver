#pragma once
#include "lan_discovery.hpp"
#include "control.hpp"
#include <atomic>
#include <mutex>
#include <thread>
namespace receiver {
class DiscoveryBeacon {
    lan_discovery::Socket socket_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::string bind_ip_, error_;
    uint16_t frame_port_ = 0;
    std::thread thread_;
    void run();

  public:
    explicit DiscoveryBeacon(const Settings& settings, uint16_t port = lan_discovery::receiver_port);
    ~DiscoveryBeacon();
    void configure(const Settings& settings);
    std::string error() const;
    uint16_t port() const { return socket_.port(); }
};
std::vector<lan_discovery::Endpoint> discover_pi(uint16_t port,
                                               std::vector<std::string> targets = {}, int timeout_ms = 1600);
} // namespace receiver
