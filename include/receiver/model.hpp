#pragma once
#include "control.hpp"
#include <nlohmann/json.hpp>
namespace receiver {
struct ModelInfo {
    int fixed_size = 0, preferred_size = 0;
    bool dynamic = true;
    std::vector<std::string> names;
    nlohmann::json metadata = nlohmann::json::object();
};
// Bounded metadata inspection only; the inference backend still validates the graph/engine.
ModelInfo inspect_onnx(Bytes data);
ModelInfo inspect_model(const Settings& settings);
Settings prepare_model_settings(Settings settings);
void reconcile_classes(Settings& settings, const std::vector<std::string>& names);
bool model_settings_changed(const Settings& a, const Settings& b);
bool connection_settings_changed(const Settings& a, const Settings& b);
} // namespace receiver
