#include "receiver/network.hpp"
#include <random>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace receiver {
#ifdef _WIN32
struct Winsock {
    Winsock() {
        WSADATA d{};
        if (WSAStartup(MAKEWORD(2, 2), &d))
            throw std::runtime_error("WSAStartup failed");
    }
    ~Winsock() {
        WSACleanup();
    }
};
static void init() {
    static Winsock w;
}
using Native = SOCKET;
#else
static void init() {}
using Native = int;
#endif
Address address(const std::string& ip, int port) {
    init();
    in_addr a{};
    if (port < 0 || port > 65535 || inet_pton(AF_INET, ip.c_str(), &a) != 1)
        throw std::runtime_error("Expected IPv4 address and valid port: " + ip);
    return {a.s_addr, uint16_t(port)};
}
UdpSocket::UdpSocket(const std::string& ip, int port) {
    auto a = address(ip, port);
    auto fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == Native(-1))
        throw std::runtime_error("UDP socket creation failed");
    socket_ = uintptr_t(fd);
    int size = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = a.ip;
    sa.sin_port = htons(a.port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa))) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        socket_ = ~uintptr_t(0);
        throw std::runtime_error("Cannot bind UDP address/port");
    }
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(fd, FIONBIO, &nonblocking);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}
UdpSocket::~UdpSocket() {
    if (socket_ == ~uintptr_t(0))
        return;
#ifdef _WIN32
    closesocket(Native(socket_));
    if (readable_)
        WSACloseEvent(readable_);
#else
    close(Native(socket_));
#endif
}
int UdpSocket::receive(std::span<uint8_t> b, Address& from, int timeout) {
    Native fd = Native(socket_);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{timeout / 1000, (timeout % 1000) * 1000};
    int ready = select(int(fd + 1), &set, nullptr, nullptr, &tv);
    if (ready <= 0)
        return ready;
    sockaddr_in sa{};
#ifdef _WIN32
    int len = sizeof(sa);
    int n = recvfrom(fd, reinterpret_cast<char*>(b.data()), int(b.size()), 0,
                     reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0 && WSAGetLastError() == WSAEMSGSIZE)
        return 0;
#else
    socklen_t len = sizeof(sa);
    int n = int(recvfrom(fd, b.data(), b.size(), MSG_TRUNC, reinterpret_cast<sockaddr*>(&sa), &len));
    if (n > int(b.size()))
        return 0;
#endif
    from = {sa.sin_addr.s_addr, ntohs(sa.sin_port)};
    return n;
}
bool UdpSocket::send(Bytes p, const Address& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = a.ip;
    sa.sin_port = htons(a.port);
    return sendto(Native(socket_), reinterpret_cast<const char*>(p.data()), int(p.size()), 0,
                  reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == int(p.size());
}
ControlWake::ControlWake() {
#ifdef _WIN32
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_)
        throw std::runtime_error("Control event creation failed");
#endif
}
ControlWake::~ControlWake() {
#ifdef _WIN32
    if (event_)
        CloseHandle(event_);
#endif
}
void ControlWake::notify() noexcept {
#ifdef _WIN32
    SetEvent(event_);
#endif
}
void ControlWake::wait(UdpSocket* socket, bool mouse_messages) {
#ifdef _WIN32
    HANDLE events[2] = {event_, nullptr};
    DWORD count = 1;
    if (socket) {
        if (!socket->readable_) {
            socket->readable_ = WSACreateEvent();
            if (!socket->readable_ || WSAEventSelect(Native(socket->socket_), socket->readable_, FD_READ))
                throw std::runtime_error("Control socket event setup failed");
        }
        events[count++] = socket->readable_;
    }
    // Timeout is housekeeping only. Inference, telemetry and mouse messages wake immediately.
    DWORD result = mouse_messages
                       ? MsgWaitForMultipleObjectsEx(count, events, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
                       : WaitForMultipleObjects(count, events, FALSE, 10);
    if (result == WAIT_FAILED)
        throw std::runtime_error("Control event wait failed");
    if (socket && result == WAIT_OBJECT_0 + 1) {
        WSANETWORKEVENTS network{};
        if (WSAEnumNetworkEvents(Native(socket->socket_), socket->readable_, &network))
            throw std::runtime_error("Control socket event read failed");
        if (network.iErrorCode[FD_READ_BIT])
            throw std::runtime_error("Control socket read failed");
    }
#else
    (void)socket;
    (void)mouse_messages;
#endif
}
uint64_t random_id() {
    std::random_device r;
    uint64_t v = uint64_t(r()) << 32 | r();
    return v ? v : 1;
}
} // namespace receiver
