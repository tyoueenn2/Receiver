#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <stdexcept>
#include <string>

namespace receiver {
class PythonWorker {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    PythonWorker(const std::string& python, const std::string& script, const std::string& label,
                 const std::string& log_prefix, int64_t deadline, std::function<bool()> cancelled);
    ~PythonWorker();
    std::runtime_error error(const std::string& message) const;
    void transfer(void* data, size_t size, bool write, int64_t deadline) const;
    void send_json(const nlohmann::json& value, int64_t deadline) const;
    nlohmann::json receive_json(int64_t deadline) const;
};
} // namespace receiver
