#include "receiver/makcu.hpp"
#include "receiver/network.hpp"
#include <array>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace receiver {
std::string MakcuProtocol::probe(uint64_t token) {
    if (!token || token > 0x7fffffff)
        throw std::runtime_error("MAKCU command IDs must be 1..2147483647");
    probe_ = "km.version()#" + std::to_string(token);
    return probe_ + "\r\n";
}
void MakcuProtocol::finish_line(int64_t now) {
    auto line = std::string_view(line_);
    if (line.starts_with(">>> "))
        line.remove_prefix(4);
    bool response = !probe_.empty() && line == probe_;
    if (!probe_.empty()) {
        auto tag = probe_.substr(probe_.find('#')) + ":";
        auto at = line.find(tag);
        if (at != std::string_view::npos) {
            auto payload = line.substr(at + tag.size());
            response = payload.starts_with("km.MAKCU") || payload.starts_with("MAKCU");
        }
        // Older firmware replies with an untagged version string.
        response = response || (line.find('#') == std::string_view::npos && line.starts_with("km.MAKCU"));
    }
    if (!overflow_ && response) {
        acknowledged_ = now;
        probe_.clear();
    }
    line_.clear();
    text_ = overflow_ = false;
}
void MakcuProtocol::receive(Bytes bytes, int64_t now) {
    // A bare CR can be button mask 13 or the first half of an ASCII CRLF.
    // Prefer the delimiter when paired, including when a read splits the pair.
    if (pending_cr_ && bytes.empty() && now - pending_cr_ >= 2'000'000) {
        buttons_ = '\r';
        pending_cr_ = 0;
    }
    for (uint8_t byte : bytes) {
        if (pending_cr_) {
            pending_cr_ = 0;
            if (byte == '\n')
                continue;
            buttons_ = '\r';
        }
        if (skip_lf_ && byte == '\n') {
            skip_lf_ = false;
            continue;
        }
        skip_lf_ = false;
        if (line_ == "km." && byte < 32) {
            buttons_ = byte;
            line_.clear();
            text_ = overflow_ = false;
            continue;
        }
        if (byte >= 32) {
            text_ = true;
            if (line_.size() < 256)
                line_.push_back(char(byte));
            else
                overflow_ = true;
            if (line_ == ">>> ") {
                line_.clear();
                text_ = false;
            }
        } else if (text_ && (byte == '\r' || byte == '\n')) {
            finish_line(now);
            skip_lf_ = byte == '\r';
        } else if (byte == '\r') {
            pending_cr_ = now;
        } else {
            // CR, LF and TAB are also valid button combinations outside ASCII replies.
            buttons_ = byte;
            line_.clear();
            text_ = overflow_ = false;
        }
    }
}
std::string MakcuProtocol::movement(int dx, int dy) {
    if (dx < -32767 || dx > 32767 || dy < -32767 || dy > 32767)
        throw std::runtime_error("MAKCU movement must be within -32767..32767");
    return "km.move(" + std::to_string(dx) + "," + std::to_string(dy) + ")\r\n";
}
namespace {
class SerialPort {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif

  public:
    explicit SerialPort(const std::string& port) {
#ifdef _WIN32
        // Use the device namespace so COM10 and higher work too.
        std::string path = port.starts_with("\\\\.\\") ? port : "\\\\.\\" + port;
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
        if (!length)
            throw std::runtime_error("Invalid MAKCU serial port name");
        std::wstring wide(size_t(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, wide.data(), length);
        handle_ =
            CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open MAKCU serial port " + port);
#else
        handle_ = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (handle_ < 0)
            throw std::runtime_error("Cannot open MAKCU serial port " + port);
        if (flock(handle_, LOCK_EX | LOCK_NB)) {
            close(handle_);
            handle_ = -1;
            throw std::runtime_error("MAKCU serial port is already in use");
        }
#endif
    }
    ~SerialPort() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (handle_ >= 0)
            close(handle_);
#endif
    }
    void baud(bool fast) {
#ifdef _WIN32
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb))
            throw std::runtime_error("Cannot read MAKCU serial configuration");
        dcb.BaudRate = fast ? 4000000 : 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = dcb.fInX = dcb.fErrorChar = dcb.fNull = dcb.fAbortOnError = FALSE;
        dcb.fDsrSensitivity = FALSE;
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = 10;
        if (!SetCommState(handle_, &dcb) || !SetCommTimeouts(handle_, &timeouts))
            throw std::runtime_error("Cannot configure MAKCU serial port at the required baud rate");
#else
        termios settings{};
        if (tcgetattr(handle_, &settings))
            throw std::runtime_error("MAKCU port is not a serial terminal");
        cfmakeraw(&settings);
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
        settings.c_cc[VMIN] = settings.c_cc[VTIME] = 0;
        auto speed = fast ? B4000000 : B115200;
        if (cfsetispeed(&settings, speed) || cfsetospeed(&settings, speed) ||
            tcsetattr(handle_, TCSANOW, &settings))
            throw std::runtime_error("Cannot configure MAKCU serial port at the required baud rate");
#endif
    }
    bool write(Bytes bytes) {
#ifdef _WIN32
        DWORD written = 0;
        return WriteFile(handle_, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
               written == bytes.size();
#else
        auto deadline = now_ns() + 10'000'000;
        size_t offset = 0;
        while (offset < bytes.size()) {
            auto count = ::write(handle_, bytes.data() + offset, bytes.size() - offset);
            if (count > 0) {
                offset += size_t(count);
                continue;
            }
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                return false;
            if (now_ns() >= deadline)
                return false;
            pollfd fd{handle_, POLLOUT, 0};
            int result = ::poll(&fd, 1, 1);
            if (result > 0 && fd.revents & (POLLERR | POLLHUP | POLLNVAL))
                return false;
            if (result < 0 && errno != EINTR)
                return false;
        }
        return true;
#endif
    }
    bool write(const std::string& text) {
        return write(Bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    }
    int read(std::span<uint8_t> bytes) {
#ifdef _WIN32
        DWORD received = 0;
        if (!ReadFile(handle_, bytes.data(), DWORD(bytes.size()), &received, nullptr))
            throw std::runtime_error("MAKCU serial read failed; stop and reconnect the device");
        return int(received);
#else
        pollfd fd{handle_, POLLIN, 0};
        if (::poll(&fd, 1, 0) > 0 && fd.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("MAKCU serial port disconnected");
        auto count = ::read(handle_, bytes.data(), bytes.size());
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            throw std::runtime_error("MAKCU serial read failed; stop and reconnect the device");
        return count > 0 ? int(count) : 0;
#endif
    }
};
} // namespace
struct MakcuMouse::Impl {
    SerialPort port;
    MakcuProtocol protocol;
    uint32_t token = uint32_t(random_id() % 0x7fffffff);
    uint32_t sequence = 0;
    int64_t started = 0, next_probe = 0;
    bool failed = false;
    explicit Impl(const std::string& name) : port(name) {}
    std::string probe() {
        token = token == 0x7fffffff ? 1 : token + 1;
        return protocol.probe(token);
    }
    bool write(const std::string& command) {
        if (failed || !port.write(command)) {
            failed = true;
            return false;
        }
        return true;
    }
};
MakcuMouse::MakcuMouse(const std::string& port) : impl_(std::make_unique<Impl>(port)) {
    impl_->port.baud(false);
    constexpr std::array<uint8_t, 9> fast_baud{0xde, 0xad, 0x05, 0x00, 0xa5, 0x00, 0x09, 0x3d, 0x00};
    if (!impl_->port.write(fast_baud))
        throw std::runtime_error("MAKCU baud negotiation write failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    impl_->port.baud(true);
    impl_->started = now_ns();
    if (!impl_->write("\r\nkm.buttons(1)\r\n") || !impl_->write(impl_->probe()))
        throw std::runtime_error("MAKCU initialization write failed");
    impl_->next_probe = impl_->started + 100'000'000;
}
MakcuMouse::~MakcuMouse() {
    if (impl_ && !impl_->failed)
        impl_->write("km.buttons(0)\r\n");
}
Telemetry MakcuMouse::poll() {
    if (impl_->failed)
        throw std::runtime_error("MAKCU serial write failed; stop and reconnect the device");
    std::array<uint8_t, 4096> bytes{};
    int count = impl_->port.read(bytes);
    auto now = now_ns();
    impl_->protocol.receive(Bytes(bytes.data(), size_t(count)), now);
    auto ack = impl_->protocol.acknowledged();
    if ((ack && now - ack > 250'000'000) || (!ack && now - impl_->started > 500'000'000))
        throw std::runtime_error("MAKCU is not responding; stop and reconnect the device");
    if (now >= impl_->next_probe && !impl_->protocol.pending()) {
        if (!impl_->write(impl_->probe()))
            throw std::runtime_error("MAKCU heartbeat write failed");
        impl_->next_probe = now + 100'000'000;
    }
    Telemetry state;
    state.ready = fresh(now);
    state.physical = state.ready ? impl_->protocol.buttons() : 0;
    state.xmin = state.ymin = -32767;
    state.xmax = state.ymax = 32767;
    state.sequence = ++impl_->sequence;
    // Button reports do not provide physical movement totals. Never substitute corrections.
    return state;
}
bool MakcuMouse::fresh(int64_t now) const {
    auto ack = impl_->protocol.acknowledged();
    return !impl_->failed && ack && now >= ack && now - ack <= 250'000'000;
}
bool MakcuMouse::send(int dx, int dy) {
    if (!fresh(now_ns()))
        return false;
    return impl_->write(MakcuProtocol::movement(dx, dy));
}
} // namespace receiver
