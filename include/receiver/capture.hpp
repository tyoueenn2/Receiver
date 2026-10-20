#pragma once
#include "protocol.hpp"
#include "cursor_protocol.hpp"
#include <string>

namespace receiver {
struct CaptureOutput {
    int index = 0;
    std::string name;
    int width = 0, height = 0;
};
std::vector<CaptureOutput> capture_outputs();
bool local_capture_supported();

// Owned and called only by the inference worker. No hooks or process injection.
class LocalCapture {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit LocalCapture(int output);
    ~LocalCapture();
    // Bounded wait for a new desktop image; null for timeout or pointer-only updates.
    std::shared_ptr<const Frame> next(int size);
    cursor_protocol::Sample cursor() const;
};
} // namespace receiver
