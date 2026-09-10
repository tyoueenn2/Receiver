#pragma once
#include "protocol.hpp"
#include <memory>
namespace receiver {
struct LocalMouseState {
    uint32_t total_x = 0, total_y = 0;
    int x = 0, y = 0;
    bool have_position = false;
    uint8_t buttons = 0;
    int64_t last_motion = 0;
    void pointer(int px, int py, bool injected, int64_t now);
    void button(uint8_t mask, bool down, bool injected);
};
class LocalMouse {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    LocalMouse();
    ~LocalMouse();
    LocalMouse(const LocalMouse&) = delete;
    Telemetry poll();
    void wait();
    bool fresh(int64_t now) const;
    bool send(int dx, int dy);
};
} // namespace receiver
