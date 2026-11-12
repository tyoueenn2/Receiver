#include "receiver/discovery.hpp"
#include "receiver/move_protocol.hpp"
#include "receiver/preferences.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace receiver;
using namespace lan_discovery;
#define CHECK(x) test::check(bool(x), #x)
namespace {
class PiFixture {
    std::atomic<bool> stop_{false};
    std::thread thread_;

  public:
    Socket socket{0, "127.0.0.1"};
    std::atomic<int> queries{0}, unexpected{0};
    PiFixture() {
        thread_ = std::thread([this] {
            while (!stop_) {
                std::array<uint8_t, 256> packet{};
                Endpoint peer;
                const auto count = socket.receive(packet, peer);
                if (count <= 0)
                    continue;
                uint64_t session = 0, token = 0;
                if (!move_protocol::parse_query(packet.data(), size_t(count), session, token)) {
                    ++unexpected;
                    continue;
                }
                ++queries;
                move_protocol::Capabilities c;
                c.session = session;
                c.token = token + 1;
                c.epoch = 100;
                socket.send(move_protocol::packet(c), peer); // Unrelated/stale response.
                c.token = token;
                const auto response = move_protocol::packet(c);
                socket.send(std::span<const uint8_t>(response.data(), 40), peer); // Truncated.
                socket.send(response, peer);
                socket.send(response, peer); // Retries must not duplicate a result.
            }
        });
    }
    ~PiFixture() {
        stop_ = true;
        thread_.join();
    }
};
}
int main() {
    try {
        Settings settings;
        settings.bind_ip = "127.0.0.1";
        settings.frame_port = 55123;
        DiscoveryBeacon beacon(settings, 0);
        auto found = find_receivers({"127.0.0.1"}, beacon.port(), 400);
        CHECK(found.size() == 1 && found[0].ip == "127.0.0.1" && found[0].port == 55123);
        settings.frame_port = 55124;
        beacon.configure(settings);
        found = find_receivers({"127.0.0.1"}, beacon.port(), 150);
        CHECK(found.size() == 1 && found[0].port == 55124);
        settings.frame_source = "local";
        beacon.configure(settings);
        CHECK(find_receivers({"127.0.0.1"}, beacon.port(), 150).empty());
        CHECK(beacon.error().empty());

        PiFixture pi;
        const auto peers = discover_pi(pi.socket.port(), {"127.0.0.1"}, 450);
        CHECK(peers.size() == 1 && peers[0].ip == "127.0.0.1" && peers[0].port == pi.socket.port());
        CHECK(!peers[0].ready); // Discovery also works before USB enumeration finishes.
        CHECK(pi.queries >= 2 && pi.unexpected == 0); // No subscriptions or movement packets.
        Socket quiet(0, "127.0.0.1");
        CHECK(discover_pi(quiet.port(), {"127.0.0.1"}, 100).empty());

        test::TempDirectory temp;
        Preferences prefs(temp.path);
        prefs.load();
        auto setup = read_json_file(prefs.setup_path());
        setup["omni_python"] = "C:/Edited/python.exe";
        write_json_atomic(setup, prefs.setup_path());
        auto connected = prefs.connect_pi(settings, peers[0].ip, peers[0].port);
        CHECK(connected.mouse_backend == 0 && connected.pi_ip == "127.0.0.1");
        CHECK(connected.pi_port == pi.socket.port());
        CHECK(connected.omni_python == "C:/Edited/python.exe");
        Preferences restarted(temp.path);
        restarted.load();
        CHECK(restarted.current.pi_port == pi.socket.port());
        test::rejects([&] { prefs.connect_pi(settings, "not an IPv4", 12345); });
        test::rejects([&] { prefs.connect_pi(settings, "127.0.0.1", 0); });
        CHECK(read_json_file(prefs.setup_path()).at("pi_port") == pi.socket.port());
        std::cout << test::checks << " discovery checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
