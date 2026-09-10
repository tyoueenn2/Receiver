#include "receiver/test_hub.hpp"
#include "receiver/protocol.hpp"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
namespace receiver {
namespace {
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring quote(const std::string& s) {
    auto w = wide(s);
    std::wstring r = L"\"";
    size_t slashes = 0;
    for (auto c : w) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        r.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            r += L'\\';
        r += c;
    }
    r.append(slashes * 2, L'\\');
    return r + L'"';
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
int execute(const std::vector<std::string>& args, const std::filesystem::path& directory,
            const std::filesystem::path& log, std::atomic<bool>& cancel) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, 0, nullptr)};
    if (output.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE)
        return -4;
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value ||
        !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)))
        return -4;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = output.value;
    startup.hStdInput = input.value;
    std::wstring command;
    for (auto& arg : args) {
        if (!command.empty())
            command += L' ';
        command += quote(arg);
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(wide(args.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &process))
        return -1;
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 5000);
        return -4;
    }
    ResumeThread(thread.value);
    auto started = std::chrono::steady_clock::now();
    while (WaitForSingleObject(child.value, 30) == WAIT_TIMEOUT) {
        if (cancel || std::chrono::steady_clock::now() - started > std::chrono::minutes(3)) {
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(child.value, 5000);
            return cancel ? -2 : -3;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(child.value, &code);
    return int(code);
}
std::string log_text(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(std::max<std::streamoff>(0, std::streamoff(n) - 32000));
    return std::string(std::istreambuf_iterator<char>(f), {});
}
} // namespace
TestHub::TestHub() {
    std::array<wchar_t, 32768> file{};
    GetModuleFileNameW(nullptr, file.data(), DWORD(file.size()));
    binaries_ = std::filesystem::path(file.data()).parent_path();
    root_ = binaries_;
    for (auto p = binaries_; !p.empty(); p = p.parent_path()) {
        if (std::filesystem::exists(p / "tools" / "test_sender.py")) {
            root_ = p;
            break;
        }
        if (p == p.parent_path())
            break;
    }
    entries_ = {
        {"Core checks", "Frame parsing, dropped packets, clock checks, detection and saved settings."},
        {"Humanization checks", "Movement styles, direction strength, slow input, resets and limits."},
        {"Tracking and input checks",
         "Prediction methods, sticky distance, dynamic search area and injected-input filtering."},
        {"Local mouse startup", "Starts the Windows mouse reader briefly. Does not move or click the mouse."},
        {"Network pipeline",
         "Runs a simulated sender and Pi through the real receiver, including disconnects."},
        {"Direction pipeline", "Checks direction ratios, missing motion data, release and recovery."},
        {"Interface pages", "Renders every page in a separate hidden test app and checks disarmed startup."},
        {"Model reference comparison",
         "Optional: requires a YOLO11n model, test image and Python ML packages."},
        {"CUDA kernel compilation", "Optional: requires the NVRTC DLL from a CUDA toolkit."},
        {"CUDA preprocessing comparison", "Optional: requires NVRTC, an NVIDIA GPU and NumPy."},
        {"TensorRT reference comparison",
         "Optional: requires a GPU build, saved profile, raw RGB image and Python ML packages."},
        {"Physical Pi / USB validation",
         "Requires running the Pi tests on Linux and checking actual USB movement."}};
}
TestHub::~TestHub() {
    cancel();
    if (worker_.joinable())
        worker_.join();
}
std::vector<TestEntry> TestHub::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}
std::string TestHub::report_path() const {
    std::lock_guard lock(mutex_);
    return report_.string();
}
std::string TestHub::find_python() {
    std::array<wchar_t, 32768> path{};
    if (GetEnvironmentVariableW(L"RECEIVER_PYTHON", path.data(), DWORD(path.size())))
        return utf8(path.data());
    for (auto name : {L"python.exe", L"py.exe"})
        if (SearchPathW(nullptr, name, nullptr, DWORD(path.size()), path.data(), nullptr))
            return utf8(path.data());
    return {};
}
void TestHub::start(std::vector<bool> selected, TestOptions options) {
    if (running_)
        return;
    if (worker_.joinable())
        worker_.join();
    cancel_ = false;
    running_ = true;
    {
        std::lock_guard lock(mutex_);
        for (auto& e : entries_) {
            e.status = "Not run";
            e.details.clear();
            e.seconds = 0;
        }
    }
    worker_ = std::thread([this, selected = std::move(selected), options = std::move(options)] {
        try {
            work(selected, options);
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            for (auto& item : entries_)
                if (item.status == "Running" || item.status == "Not run") {
                    item.status = "Unavailable";
                    item.details = e.what();
                }
        }
        running_ = false;
    });
}
void TestHub::work(std::vector<bool> selected, TestOptions o) {
    auto output =
        root_ / "test-results" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    {
        std::lock_guard lock(mutex_);
        report_ = output / "report.json";
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (i >= selected.size() || !selected[i] || cancel_)
            continue;
        std::vector<std::string> command;
        std::string blocked;
        auto require = [&](const std::string& path, const char* description) {
            if (path.empty() || !std::filesystem::exists(wide(path)))
                blocked = description;
        };
        if (i < 4) {
            const char* names[] = {"receiver_tests.exe", "receiver_humanization_tests.exe",
                                   "receiver_tracking_tests.exe", "receiver_local_mouse_tests.exe"};
            command = {(binaries_ / names[i]).string()};
            require(command[0], "This test executable is not included. Rebuild/package the test targets.");
        } else if (i == 11)
            blocked = "Run make test on the Pi with the included patch, then validate physical USB output. "
                      "This Windows hub cannot perform that hardware check.";
        else {
            require(o.python, "Choose a Python executable in Test settings.");
            command = {o.python};
            auto add = [&](std::initializer_list<std::string> args) { command.insert(command.end(), args); };
            if (i == 4)
                add({(root_ / "tests/integration_test.py").string(),
                     (binaries_ / "receiver_headless.exe").string()});
            if (i == 5)
                add({(root_ / "tests/direction_integration_test.py").string(),
                     (binaries_ / "receiver_headless.exe").string()});
            if (i == 6)
                add({(root_ / "tests/gui_smoke.py").string(), (binaries_ / "receiver.exe").string(),
                     "--all-pages"});
            if (i == 7) {
                require(o.model, "Choose the YOLO11n ONNX model in Test settings.");
                require(o.image, "Choose a test image with detectable objects in Test settings.");
                add({(root_ / "tests/model_reference_test.py").string(), "--model", o.model, "--image",
                     o.image, "--decoder", (binaries_ / "receiver_decode.exe").string()});
            }
            if (i == 8) {
                require(o.nvrtc, "Choose the CUDA NVRTC DLL in Test settings.");
                add({(root_ / "tools/check_cuda_kernel.py").string(), "--nvrtc", o.nvrtc, "--output",
                     (output / "preprocess.ptx").string()});
            }
            if (i == 9) {
                require(o.nvrtc, "Choose the CUDA NVRTC DLL in Test settings.");
                add({(root_ / "tests/preprocess_gpu_test.py").string(), "--nvrtc", o.nvrtc});
            }
            if (i == 10) {
                require((binaries_ / "receiver_verify.exe").string(),
                        "Requires a TensorRT build containing receiver_verify.exe.");
                require(o.profile, "Choose a saved GPU profile in Test settings.");
                require(o.raw, "Choose a tightly packed RGB24 test file in Test settings.");
                add({(root_ / "tools/validate_gpu.py").string(), "--verify-exe",
                     (binaries_ / "receiver_verify.exe").string(), "--profile", o.profile, "--raw", o.raw,
                     "--width", std::to_string(o.width), "--height", std::to_string(o.height)});
            }
            if (command.size() > 1)
                require(command[1], "The test scripts are missing from this package.");
        }
        if (!blocked.empty()) {
            std::lock_guard lock(mutex_);
            entries_[i].status = "Unavailable";
            entries_[i].details = blocked;
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            entries_[i].status = "Running";
        }
        auto began = std::chrono::steady_clock::now();
        auto log = output / (std::to_string(i + 1) + ".log");
        int result = execute(command, i == 10 ? root_ : output, log, cancel_);
        std::string details = log_text(log);
        if (result == -1)
            details = "Could not launch the test. Check the executable path.\n" + details;
        if (result == -3)
            details = "The test exceeded its three-minute time limit.\n" + details;
        if (result == -4)
            details = "Windows could not create the isolated test process or its log.\n" + details;
        {
            std::lock_guard lock(mutex_);
            auto& entry = entries_[i];
            entry.status = result == 0                    ? "Passed"
                           : result == -2                 ? "Cancelled"
                           : (result < 0 || result == 77) ? "Unavailable"
                                                          : "Failed";
            entry.details = details;
            entry.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        }
    }
    nlohmann::json report = nlohmann::json::array();
    for (auto& entry : entries())
        report.push_back({{"test", entry.name},
                          {"status", entry.status},
                          {"seconds", entry.seconds},
                          {"details", entry.details}});
    std::ofstream file(output / "report.json");
    file << report.dump(2);
}
} // namespace receiver
