#include "receiver/backend.hpp"
#include "receiver/network.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace receiver {
namespace {
using nlohmann::json;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket s) {
    if (s != invalid_socket)
        closesocket(s);
}
bool retry_socket() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
void nonblocking(Socket s) {
    u_long enabled = 1;
    if (ioctlsocket(s, FIONBIO, &enabled))
        throw std::runtime_error("Cannot configure worker connection");
}
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 worker argument");
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n);
    return out;
}
// Quote each argument for the Windows C runtime; never invoke a shell.
std::wstring quote(const std::string& s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : wide(s)) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket s) {
    if (s >= 0)
        close(s);
}
bool retry_socket() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
void nonblocking(Socket s) {
    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) < 0 || fcntl(s, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("Cannot configure worker connection");
}
#endif

class OmniBackend final : public Backend {
    Socket listener_ = invalid_socket, connection_ = invalid_socket;
#ifdef _WIN32
    HANDLE process_ = nullptr;
#else
    pid_t process_ = -1;
#endif
    BackendCancel cancelled_;
    std::vector<float> output_;
    std::vector<std::string> names_;
    std::string description_, log_path_;
    size_t free_memory_ = 0, total_memory_ = 0;
    int size_ = 0, classes_ = 0;

    bool exited() const {
#ifdef _WIN32
        return process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0;
#else
        if (process_ <= 0)
            return false;
        siginfo_t info{};
        return waitid(P_PID, id_t(process_), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
               info.si_pid == process_;
#endif
    }
    std::runtime_error error(const std::string& message) const {
        return std::runtime_error("YOLO-Omni: " + message + ". Worker log: " + log_path_);
    }
    bool ready(Socket s, bool write, int64_t deadline) const {
        if (cancelled_ && cancelled_())
            throw error("loading/inference cancelled");
        if (now_ns() >= deadline)
            throw error("worker timed out");
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval wait{0, 20'000};
        int n = select(int(s + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &wait);
        if (n < 0 && !retry_socket())
            throw error("connection failed");
        if (n > 0)
            return true;
        if (exited())
            throw error("Python exited; check its dependencies and model");
        return false;
    }
    void transfer(void* data, size_t size, bool write, int64_t deadline) const {
        auto* p = static_cast<char*>(data);
        while (size) {
            if (!ready(connection_, write, deadline))
                continue;
            int chunk = int(std::min<size_t>(size, 64 * 1024));
#ifdef _WIN32
            int n = write ? send(connection_, p, chunk, 0) : recv(connection_, p, chunk, 0);
#else
            int n = int(write ? send(connection_, p, size_t(chunk), MSG_NOSIGNAL)
                              : recv(connection_, p, size_t(chunk), 0));
#endif
            if (n < 0 && retry_socket())
                continue;
            if (n <= 0)
                throw error("worker disconnected");
            p += n;
            size -= size_t(n);
        }
    }
    void send_json(const json& value, int64_t deadline) const {
        auto text = value.dump();
        std::array<uint8_t, 4> header{};
        put32(header.data(), uint32_t(text.size()));
        transfer(header.data(), header.size(), true, deadline);
        transfer(text.data(), text.size(), true, deadline);
    }
    json receive_json(int64_t deadline) const {
        std::array<uint8_t, 4> header{};
        transfer(header.data(), header.size(), false, deadline);
        auto length = be32(header.data());
        if (!length || length > 65536)
            throw error("invalid response size");
        std::string text(length, '\0');
        transfer(text.data(), text.size(), false, deadline);
        auto value = json::parse(text);
        if (!value.is_object())
            throw error("invalid response");
        if (value.value("status", "") == "error")
            throw error(value.value("message", "worker error"));
        return value;
    }
    void launch(const Settings& s, int port, uint64_t token) {
        auto worker = std::filesystem::absolute(s.omni_worker);
        if (!std::filesystem::is_regular_file(worker))
            throw error("missing worker script " + worker.string());
        std::vector<std::string> args{s.omni_python,        "-u",      worker.string(),      "--port",
                                      std::to_string(port), "--token", std::to_string(token)};
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE log = CreateFileW(wide(log_path_).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
            if (log != INVALID_HANDLE_VALUE)
                CloseHandle(log);
            if (input != INVALID_HANDLE_VALUE)
                CloseHandle(input);
            throw error("cannot open worker log");
        }
        // Inherit only the three redirected standard handles.
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<uint8_t> attributes(bytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log;
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        HANDLE handles[]{input, log};
        bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes);
        bool configured = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                                                   sizeof(handles), nullptr, nullptr);
        std::wstring command;
        for (const auto& arg : args) {
            if (!command.empty())
                command += L' ';
            command += quote(arg);
        }
        PROCESS_INFORMATION process{};
        bool started = configured && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                                    nullptr, &startup.StartupInfo, &process);
        if (initialized)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(log);
        CloseHandle(input);
        if (!started)
            throw error("cannot start Python; set the Python executable in Advanced connection settings");
        process_ = process.hProcess;
        CloseHandle(process.hThread);
#else
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions))
            throw error("cannot prepare Python process");
        int result = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!result)
            result = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path_.c_str(),
                                                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!result)
            result = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!result)
            result = posix_spawnp(&process_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (result)
            throw error("cannot start Python; check omni_python in the profile");
#endif
    }
    void cleanup() {
        close_socket(connection_);
        close_socket(listener_);
        connection_ = listener_ = invalid_socket;
#ifdef _WIN32
        if (process_) {
            if (WaitForSingleObject(process_, 200) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 1000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
#else
        if (process_ > 0) {
            int status;
            if (waitpid(process_, &status, WNOHANG) == 0) {
                kill(process_, SIGKILL);
                while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {
                }
            }
            process_ = -1;
        }
#endif
    }
    void initialize(const Settings& s, const BackendProgress& progress) {
        size_ = s.input_size;
        if (!std::filesystem::is_regular_file(s.model))
            throw std::runtime_error("YOLO-Omni weights not found: " + s.model);
        if (progress)
            progress("YOLO-Omni: starting Python runtime");
        auto loopback = address("127.0.0.1", 0); // Also initializes Winsock.
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == invalid_socket)
            throw error("cannot create worker socket");
        nonblocking(listener_);
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = loopback.ip;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) || listen(listener_, 1))
            throw error("cannot listen for Python worker");
#ifdef _WIN32
        int length = sizeof(endpoint);
#else
        socklen_t length = sizeof(endpoint);
#endif
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&endpoint), &length))
            throw error("cannot read worker port");
        uint64_t token = random_id();
        std::filesystem::create_directories("cache");
        log_path_ = std::filesystem::absolute("cache/omni-worker-" + std::to_string(token) + ".log").string();
        launch(s, ntohs(endpoint.sin_port), token);
        auto deadline = now_ns() + 120'000'000'000ll;
        while (!ready(listener_, false, deadline)) {
        }
        connection_ = accept(listener_, nullptr, nullptr);
        if (connection_ == invalid_socket)
            throw error("cannot accept worker connection");
        nonblocking(connection_);
        int enabled = 1;
        setsockopt(connection_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                   sizeof(enabled));
        std::array<uint8_t, 8> handshake{};
        transfer(handshake.data(), handshake.size(), false, deadline);
        if (be64(handshake.data()) != token)
            throw error("worker authentication failed");
        close_socket(listener_);
        listener_ = invalid_socket;
        send_json({{"version", 1},
                   {"model", std::filesystem::absolute(s.model).string()},
                   {"source", s.omni_source.empty() ? "" : std::filesystem::absolute(s.omni_source).string()},
                   {"input_size", size_},
                   {"auto_size", s.auto_size},
                   {"device", s.omni_device}},
                  deadline);
        if (progress)
            progress("YOLO-Omni: loading weights and warming model");
        auto reply = receive_json(deadline);
        if (reply.value("status", "") != "ready" || reply.value("version", 0) != 1)
            throw error("unsupported worker protocol");
        size_ = reply.value("input_size", size_);
        if (size_ < 32 || size_ > 1024 || size_ % 32)
            throw error("invalid worker input size");
        names_ = reply.at("names").get<std::vector<std::string>>();
        classes_ = int(names_.size());
        if (classes_ < 1 || classes_ > 10000)
            throw error("invalid class count");
        for (int c : s.classes)
            if (c >= classes_)
                throw error("selected class is absent from model");
        description_ = reply.at("description").get<std::string>();
        free_memory_ = reply.value("free_memory", size_t(0));
        total_memory_ = reply.value("total_memory", size_t(0));
    }

  public:
    OmniBackend(const Settings& s, const BackendProgress& progress, const BackendCancel& cancelled)
        : cancelled_(cancelled) {
        try {
            initialize(s, progress);
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~OmniBackend() override {
        cleanup();
    }
    std::string description() const override {
        return description_;
    }
    std::vector<std::string> class_names() const override {
        return names_;
    }
    int input_size() const override {
        return size_;
    }
    std::pair<size_t, size_t> device_memory() const override {
        return {free_memory_, total_memory_};
    }
    Inference run(const Frame& frame) override {
        const auto& h = frame.header;
        size_t expected = size_t(h.width) * h.height * (h.format == 1 ? 3 : 4);
        if (!h.width || !h.height || h.width > 1024 || h.height > 1024 || (h.format != 1 && h.format != 2) ||
            h.bytes != expected || expected > frame.pixels.size())
            throw error("invalid input frame");
        auto begin = now_ns(), deadline = begin + 10'000'000'000ll;
        send_json({{"width", h.width}, {"height", h.height}, {"format", h.format}, {"bytes", h.bytes}},
                  deadline);
        transfer(const_cast<uint8_t*>(frame.pixels.data()), expected, true, deadline);
        auto reply = receive_json(deadline);
        int count = reply.at("candidates").get<int>();
        if (reply.value("status", "") != "result" || reply.at("classes") != classes_ || count < 1 ||
            count > 100000 || size_t(count) * (classes_ + 4) > 16 * 1024 * 1024)
            throw error("unsupported raw detection output shape");
        double inference = reply.at("inference_ms").get<double>();
        if (!std::isfinite(inference) || inference < 0)
            throw error("invalid inference timing");
        output_.resize(size_t(count) * (classes_ + 4));
        transfer(output_.data(), output_.size() * sizeof(float), false, deadline);
        static_assert(std::endian::native == std::endian::little, "Worker output is little-endian float32");
        double elapsed = double(now_ns() - begin) / 1e6;
        return {output_, count, classes_, std::max(0., elapsed - inference), inference};
    }
};
} // namespace
std::unique_ptr<Backend> make_omni_backend(const Settings& s, const BackendProgress& progress,
                                           const BackendCancel& cancelled) {
    return std::make_unique<OmniBackend>(s, progress, cancelled);
}
} // namespace receiver
