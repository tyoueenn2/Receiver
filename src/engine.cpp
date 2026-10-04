#include "receiver/engine.hpp"
#include <stdexcept>

namespace receiver {
EnginePayload inspect_engine_payload(Bytes data) {
    if (data.empty())
        throw std::runtime_error("Empty TensorRT engine");
    if (data.size() < 5)
        return {};
    uint32_t length =
        uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
    // A raw TensorRT plan has a binary header, not a JSON object at byte four.
    if (data[4] != '{')
        return {};
    if (!length || length > 65536 || size_t(length) + 4 >= data.size())
        throw std::runtime_error("Invalid/truncated Ultralytics engine metadata header");
    auto metadata = nlohmann::json::parse(data.begin() + 4, data.begin() + 4 + length);
    if (!metadata.is_object())
        throw std::runtime_error("Engine metadata must be an object");
    return {size_t(length) + 4, std::move(metadata)};
}
std::vector<std::string> engine_class_names(const nlohmann::json& meta, const std::string& file_sha256) {
    if (!meta.is_object() || !meta.contains("names"))
        throw std::runtime_error("Engine needs class metadata: use an Ultralytics .engine export or "
                                 "tools/prepare_engine.py with the ONNX manifest");
    if (meta.value("task", "") != "detect" || meta.value("layout", "NCHW") != "NCHW")
        throw std::runtime_error(
            "Engine must be an NCHW detection model; pose/segmentation/OBB are unsupported");
    auto output = meta.value("output", "raw_yolo");
    if ((output != "raw_yolo" && output != "raw_yolo11" && output != "raw_yolo_omni") ||
        meta.value("nms", false) || (meta.contains("args") && meta.at("args").value("nms", false)))
        throw std::runtime_error("Engine must expose raw detections without embedded NMS");
    if (meta.contains("sha256") && meta.at("sha256") != file_sha256)
        throw std::runtime_error("Engine metadata SHA-256 mismatch");
    const auto& values = meta.at("names");
    if ((!values.is_array() && !values.is_object()) || values.empty() || values.size() > 10000)
        throw std::runtime_error("Invalid engine class metadata");
    std::vector<std::string> names;
    names.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        auto& name = values.is_array() ? values.at(i) : values.at(std::to_string(i));
        if (!name.is_string())
            throw std::runtime_error("Engine class names must be strings with contiguous IDs");
        names.push_back(name.get<std::string>());
    }
    return names;
}
int validate_engine_shape(std::span<const int64_t> input, std::span<const int64_t> output, int size,
                          int classes) {
    if (input.size() != 4 || input[0] != 1 || input[1] != 3 || input[2] != size || input[3] != size)
        throw std::runtime_error("Engine needs [1,3,size,size] input; choose a processing size matching its "
                                 "fixed size or optimization profile");
    if (classes < 1 || classes > 10000 || output.size() != 3 || output[0] != 1 || output[1] != 4 + classes ||
        output[2] < 1 || output[2] > 100000 || output[2] * (4 + classes) > 16 * 1024 * 1024)
        throw std::runtime_error(
            "Engine needs raw [1,4+classes,candidates] output; transposed/NMS outputs are unsupported");
    return int(output[2]);
}
} // namespace receiver
