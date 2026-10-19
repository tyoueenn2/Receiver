#pragma once
#include "protocol.hpp"
#include <memory>
#include <string>

namespace receiver {
// MAKCU mixes ASCII replies with legacy raw or "km."-prefixed five-bit button reports.
class MakcuProtocol {
    std::string line_, probe_;
    bool text_ = false, overflow_ = false, skip_lf_ = false;
    uint8_t buttons_ = 0;
    int64_t acknowledged_ = 0, pending_cr_ = 0;
    void finish_line(int64_t now);

  public:
    std::string probe(uint64_t token);
    void receive(Bytes bytes, int64_t now);
    uint8_t buttons() const {
        return buttons_;
    }
    int64_t acknowledged() const {
        return acknowledged_;
    }
    bool pending() const {
        return !probe_.empty();
    }
    static std::string movement(int dx, int dy);
};

class MakcuMouse {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit MakcuMouse(const std::string& port);
    ~MakcuMouse();
    MakcuMouse(const MakcuMouse&) = delete;
    MakcuMouse& operator=(const MakcuMouse&) = delete;
    Telemetry poll();
    bool fresh(int64_t now) const;
    bool send(int dx, int dy);
};
} // namespace receiver
