#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
namespace receiver {
struct TestOptions {
    std::string python, model, image, nvrtc, raw, profile;
    int width = 320, height = 320;
};
struct TestEntry {
    std::string name, description, status = "Not run", details;
    double seconds = 0;
};
class TestHub {
    mutable std::mutex mutex_;
    std::vector<TestEntry> entries_;
    std::thread worker_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::filesystem::path root_, binaries_, report_;
    void work(std::vector<bool> selected, TestOptions options);

  public:
    TestHub();
    ~TestHub();
    std::vector<TestEntry> entries() const;
    std::string report_path() const;
    bool running() const {
        return running_;
    }
    void cancel() {
        cancel_ = true;
    }
    void start(std::vector<bool> selected, TestOptions options);
    static std::string find_python();
};
} // namespace receiver
