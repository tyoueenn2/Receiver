#pragma once
#include "protocol.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace receiver {
struct EnginePayload {
    size_t offset = 0;
    nlohmann::json metadata;
};
// Ultralytics prefixes its plan with a little-endian JSON length and JSON object.
EnginePayload inspect_engine_payload(Bytes data);
std::vector<std::string> engine_class_names(const nlohmann::json& metadata, const std::string& file_sha256);
int validate_engine_shape(std::span<const int64_t> input, std::span<const int64_t> output, int input_size,
                          int classes);
} // namespace receiver
