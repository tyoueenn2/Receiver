#include "test_support.hpp"
#include "receiver/makcu.hpp"
#include "receiver/preferences.hpp"
#include <iostream>
#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#endif
using namespace receiver;
using test::rejects;
namespace {
void feed(MakcuProtocol& protocol, const std::string& text, int64_t now) {
    protocol.receive(Bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size()), now);
}
void protocol_tests() {
    CHECK(MakcuProtocol::movement(12, -34) == "km.move(12,-34)\r\n");
    CHECK(MakcuProtocol::movement(-32767, 32767) == "km.move(-32767,32767)\r\n");
    rejects([] { MakcuProtocol::movement(32768, 0); });
    rejects([] { MakcuProtocol::movement(0, -32768); });
    rejects([] { MakcuProtocol().probe(0); });
    rejects([] { MakcuProtocol().probe(0x80000000); });
    for (uint8_t mask = 0; mask < 32; ++mask) {
        MakcuProtocol protocol;
        protocol.receive(Bytes(&mask, 1), 1'000'000);
        protocol.receive({}, 4'000'000);
        CHECK(protocol.buttons() == mask);
        CHECK(protocol.acknowledged() == 0); // Button events cannot establish link health.
        feed(protocol, "km.", 5'000'000);
        protocol.receive(Bytes(&mask, 1), 6'000'000);
        CHECK(protocol.buttons() == mask); // New firmware prefixes all combinations, including CR/LF.
    }
    MakcuProtocol protocol;
    auto command = protocol.probe(1234567);
    CHECK(command == "km.version()#1234567\r\n");
    for (char c : std::string(">>> ") + command)
        feed(protocol, std::string(1, c), 10'000'000);
    CHECK(!protocol.pending() && protocol.acknowledged() == 10'000'000);
    CHECK(protocol.buttons() == 0); // Echoes/prompts/line endings never press a button.
    feed(protocol, "\r", 11'000'000);
    feed(protocol, "\n", 12'000'000);
    CHECK(protocol.buttons() == 0);
    feed(protocol, ">>> ", 13'000'000);
    feed(protocol, std::string(1, '\x09'), 14'000'000);
    CHECK(protocol.buttons() == 9);
    feed(protocol, std::string(1, '\x0a'), 15'000'000);
    CHECK(protocol.buttons() == 10);
    feed(protocol, std::string(1, '\0'), 16'000'000);
    CHECK(protocol.buttons() == 0);
    protocol.probe(7654321);
    feed(protocol, command + "MAKCU firmware\r\nkm.move(1,2)\r\n", 20'000'000);
    CHECK(protocol.pending() && protocol.acknowledged() == 10'000'000);
    feed(protocol, std::string(5000, 'x') + "\r\n", 21'000'000);
    CHECK(protocol.pending() && protocol.buttons() == 0);
    feed(protocol, "km.version()#7654321\r", 22'000'000);
    feed(protocol, "\n", 23'000'000);
    CHECK(!protocol.pending() && protocol.acknowledged() == 22'000'000);
    protocol.probe(42);
    feed(protocol, "km.version()#41:km.MAKCU:3.9\r\n", 24'000'000);
    CHECK(protocol.pending());
    feed(protocol, "km.version()#42:error\r\n", 25'000'000);
    CHECK(protocol.pending());
    feed(protocol, "km.version()#42:km.MAKCU:3.9\r\n", 26'000'000);
    CHECK(!protocol.pending() && protocol.acknowledged() == 26'000'000);
    protocol.probe(43);
    feed(protocol, "km.MAKCU:legacy firmware\r\n", 27'000'000);
    CHECK(!protocol.pending() && protocol.acknowledged() == 27'000'000);
}
void settings_tests() {
    auto legacy = settings_from_json(nlohmann::json{{"mouse_backend", 0}});
    CHECK(legacy.mouse_backend == 0 && legacy.makcu_port.empty());
    Settings settings;
    settings.mouse_backend = 2;
    rejects([&] { validate(settings); });
    settings.makcu_port = "COM12";
    validate(settings);
    test::TempDirectory temp;
    auto file = temp.path / "makcu.json";
    save_settings(settings, file.string());
    auto loaded = load_settings(file.string());
    CHECK(loaded.mouse_backend == 2 && loaded.makcu_port == "COM12");
    auto changed = settings;
    changed.makcu_port = "COM13";
    CHECK(connection_settings_changed(settings, changed));
    CHECK(!model_settings_changed(settings, changed));
    changed = settings;
    changed.mouse_backend = 0;
    CHECK(connection_settings_changed(settings, changed));
    for (int mask : {6, 8}) {
        auto bad = settings;
        bad.activation_button = mask;
        rejects([&] { validate(bad); });
        bad = settings;
        bad.secondary_button = mask;
        rejects([&] { validate(bad); });
        bad = settings;
        bad.tracking.fov_button = mask;
        rejects([&] { validate(bad); });
    }
    auto bad = settings;
    bad.direction.enabled = true;
    rejects([&] { validate(bad); });
    for (const auto& name :
         {std::string("COM3\r\nkm.move(1,1)"), std::string("COM\0bad", 7), std::string(257, 'x')}) {
        bad = settings;
        bad.makcu_port = name;
        rejects([&] { validate(bad); });
    }
    Preferences preferences(temp.path / "preferences");
    auto model = settings;
    model.model = (temp.path / "model.pt").string();
    model.mouse_backend = 0;
    model.direction.enabled = true;
    model.tracking.fov_button = 8;
    preferences.save(model, "");
    settings.makcu_port = "COM42";
    loaded = preferences.select_model(settings, model.model);
    CHECK(loaded.mouse_backend == 2 && loaded.makcu_port == "COM42");
    CHECK(!loaded.direction.enabled && loaded.tracking.fov_button == settings.tracking.fov_button);
    validate(loaded);
    preferences.save(loaded, "");
    Preferences restored(temp.path / "preferences");
    restored.load();
    CHECK(restored.current.mouse_backend == 2 && restored.current.makcu_port == "COM42");
}
#ifndef _WIN32
class Terminal {
  public:
    int master = -1;
    std::string name;
    Terminal() {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master < 0 || grantpt(master) || unlockpt(master))
            throw std::runtime_error("Cannot create pseudo-terminal");
        name = ptsname(master);
    }
    ~Terminal() {
        if (master >= 0)
            close(master);
    }
    std::string receive() {
        std::array<char, 4096> buffer{};
        auto count = read(master, buffer.data(), buffer.size());
        return count > 0 ? std::string(buffer.data(), size_t(count)) : "";
    }
    void send(const std::string& value) {
        CHECK(write(master, value.data(), value.size()) == ssize_t(value.size()));
    }
};
template <class F> void until(F predicate) {
    auto end = now_ns() + 1'000'000'000;
    while (!predicate()) {
        CHECK(now_ns() < end);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void serial_tests() {
    rejects([] { MakcuMouse mouse("/dev/receiver-no-such-port"); });
    Terminal terminal;
    {
        MakcuMouse mouse(terminal.name);
        CHECK(!mouse.fresh(now_ns()) && !mouse.send(10, 0));
        std::string initialization;
        until([&] {
            initialization += terminal.receive();
            return initialization.find("km.version()#") != std::string::npos &&
                   initialization.ends_with("\r\n");
        });
        const std::string baud("\xde\xad\x05\0\xa5\0\x09\x3d\0", 9);
        CHECK(initialization.starts_with(baud + "\r\nkm.buttons(1)\r\nkm.version()#"));
        auto probe = initialization.substr(initialization.find("km.version()#"));
        CHECK(probe.ends_with("\r\n"));
        rejects([&] { MakcuMouse duplicate(terminal.name); });
        terminal.send(">>> " + probe + ">>> ");
        terminal.send(std::string(1, '\x02'));
        Telemetry state;
        until([&] {
            state = mouse.poll();
            return state.ready;
        });
        CHECK(state.physical == 2 && !state.has_motion && mouse.fresh(now_ns()));
        termios config{};
        CHECK(tcgetattr(terminal.master, &config) == 0 && cfgetospeed(&config) == B4000000);
        CHECK(!(config.c_lflag & (ECHO | ICANON)) && !(config.c_iflag & (IXON | ICRNL)));
        CHECK(mouse.send(27, -19));
        std::string sent;
        until([&] {
            sent += terminal.receive();
            return sent.ends_with("\r\n");
        });
        CHECK(sent == "km.move(27,-19)\r\n");
        terminal.send(std::string(1, '\0'));
        until([&] { return mouse.poll().physical == 0; });
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        CHECK(!mouse.fresh(now_ns()) && !mouse.send(1, 0));
        rejects([&] { mouse.poll(); });
    }
    std::string shutdown;
    until([&] {
        shutdown += terminal.receive();
        return shutdown.find("km.buttons(0)\r\n") != std::string::npos;
    });
    {
        MakcuMouse mouse(terminal.name);
        std::this_thread::sleep_for(std::chrono::milliseconds(510));
        rejects([&] { mouse.poll(); });
    }
    Terminal disconnected;
    MakcuMouse mouse(disconnected.name);
    close(disconnected.master);
    disconnected.master = -1;
    rejects([&] { mouse.poll(); });
    CHECK(!mouse.send(1, 0));
}
#endif
} // namespace
int main() {
    try {
        protocol_tests();
        settings_tests();
#ifndef _WIN32
        serial_tests();
#endif
        std::cout << "MAKCU protocol, profile and serial checks passed: " << test::checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
