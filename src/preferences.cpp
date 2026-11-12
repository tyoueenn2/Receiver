#include "receiver/preferences.hpp"
#include "receiver/network.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
namespace receiver {
namespace {
// Machine-specific values have one editable home, independent of model/profile snapshots.
constexpr const char* setup_fields[] = {"bind_ip", "sender_ip", "pi_ip", "frame_port", "pi_port",
                                      "makcu_port", "omni_python", "omni_source", "omni_device",
                                      "omni_worker"};
nlohmann::json setup_values(const Settings& settings) {
    const auto all = settings_json(settings);
    auto result = nlohmann::json::object();
    for (auto field : setup_fields)
        result[field] = all.at(field);
    return result;
}
nlohmann::json read_setup(const std::filesystem::path& path) {
    try {
        const auto value = read_json_file(path);
        if (!value.is_object())
            throw std::runtime_error("Expected a JSON object");
        auto result = setup_values(Settings{});
        for (const auto& item : value.items()) {
            if (!result.contains(item.key()))
                throw std::runtime_error("Unknown setup setting: " + item.key());
            result[item.key()] = item.value();
        }
        // Reuse the profile's type and range checks for these shared fields.
        settings_from_json(result);
        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error("Cannot load " + path.string() + ": " + e.what());
    }
}
} // namespace
Settings Preferences::apply_setup(const Settings& settings) const {
    auto value = settings_json(settings);
    value.update(setup_);
    return settings_from_json(value);
}
Settings Preferences::load_profile(const std::string& path) const {
    auto value = read_json_file(path);
    value.update(setup_);
    return settings_from_json(value);
}
Settings Preferences::reload_setup(const Settings& settings) {
    auto next = *this;
    next.setup_ = read_setup(setup_path());
    auto result = next.apply_setup(settings);
    setup_ = std::move(next.setup_);
    return result;
}
Settings Preferences::connect_pi(const Settings& settings, const std::string& ip, uint16_t port) {
    address(ip, port);
    auto next = *this;
    // Read the latest file so connecting a Pi preserves edits made in a text editor.
    next.setup_ = read_setup(setup_path());
    next.setup_["pi_ip"] = ip;
    next.setup_["pi_port"] = port;
    auto connected = settings;
    connected.mouse_backend = 0;
    auto result = next.apply_setup(connected);
    write_json_atomic(next.setup_, setup_path());
    setup_ = std::move(next.setup_);
    return result;
}
std::string Preferences::key(const std::string& model) {
    if (model.empty())
        return {};
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(model, error);
    if (error)
        path = std::filesystem::absolute(model).lexically_normal();
    auto value = path.generic_string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return value;
}
void Preferences::add_directory(const std::filesystem::path& directory, bool model) {
    auto normalized = std::filesystem::absolute(directory).lexically_normal();
    auto& dirs = model ? model_dirs_ : profile_dirs_;
    if (dirs.size() < 64 && std::find(dirs.begin(), dirs.end(), normalized) == dirs.end())
        dirs.push_back(normalized);
}
void Preferences::load() {
    auto next = *this;
    next.setup_ = nlohmann::json::object();
    auto path = root_ / "preferences.json";
    const bool has_preferences = std::filesystem::exists(path);
    const bool has_setup = std::filesystem::exists(setup_path());
    if (has_setup)
        next.setup_ = read_setup(setup_path());
    if (has_preferences) {
        const auto j = read_json_file(path);
        if (j.value("version", 0) != 1)
            throw std::runtime_error("Unsupported preference version");
        auto saved = j.at("settings");
        saved.update(next.setup_);
        auto settings = settings_from_json(saved);
        auto models = j.value("models", nlohmann::json::object());
        if (!models.is_object() || models.size() > 1000)
            throw std::runtime_error("Invalid saved model library");
        for (const auto& value : models.items()) {
            auto model = value.value();
            model.update(next.setup_);
            settings_from_json(model);
        }
        next.current = std::move(settings);
        next.models_ = std::move(models);
        next.profile = j.value("profile", "");
        for (const auto& [field, is_model] :
             {std::pair{"model_directories", true}, {"profile_directories", false}}) {
            if (!j.contains(field))
                continue;
            if (!j[field].is_array() || j[field].size() > 64)
                throw std::runtime_error("Invalid saved library directories");
            for (const auto& directory : j[field])
                next.add_directory(directory.get<std::string>(), is_model);
        }
    }
    if (!has_setup) {
        // The first run migrates the user's existing connections and Python installation.
        next.setup_ = setup_values(next.current);
        std::filesystem::create_directories(root_);
        write_json_atomic(next.setup_, setup_path());
    }
    next.current = next.apply_setup(next.current);
    *this = std::move(next);
}
void Preferences::save(const Settings& settings, const std::string& profile_path,
                       const std::vector<std::string>& names) {
    auto s = apply_setup(settings);
    reconcile_classes(s, names);
    validate(s);
    auto next = *this;
    next.current = s;
    next.profile = profile_path;
    if (!s.model.empty()) {
        auto identity = key(s.model);
        if (!next.models_.contains(identity) && next.models_.size() >= 1000)
            next.models_.erase(next.models_.begin());
        next.models_[identity] = settings_json(s);
        next.add_directory(std::filesystem::absolute(s.model).parent_path(), true);
    }
    if (!next.profile.empty())
        next.add_directory(std::filesystem::absolute(next.profile).parent_path(), false);
    std::filesystem::create_directories(root_);
    std::vector<std::string> model_dirs, profile_dirs;
    for (const auto& d : next.model_dirs_)
        model_dirs.push_back(d.string());
    for (const auto& d : next.profile_dirs_)
        profile_dirs.push_back(d.string());
    write_json_atomic({{"version", 1},
                       {"settings", settings_json(s)},
                       {"profile", next.profile},
                       {"models", next.models_},
                       {"model_directories", model_dirs},
                       {"profile_directories", profile_dirs}},
                      root_ / "preferences.json");
    *this = std::move(next);
}
Settings Preferences::select_model(const Settings& base, const std::string& path) const {
    auto identity = key(path);
    auto s = base;
    if (models_.contains(identity)) {
        auto saved = models_[identity];
        saved.update(setup_);
        s = settings_from_json(saved);
        // Connections and activation are application preferences, not properties of a model.
        s.frame_source = base.frame_source;
        s.capture_output = base.capture_output;
        s.bind_ip = base.bind_ip;
        s.sender_ip = base.sender_ip;
        s.pi_ip = base.pi_ip;
        s.frame_port = base.frame_port;
        s.pi_port = base.pi_port;
        s.mouse_backend = base.mouse_backend;
        s.makcu_port = base.makcu_port;
        s.activation_button = base.activation_button;
        s.secondary_button = base.secondary_button;
        s.preview = base.preview;
        s.ui = base.ui;
        s.visual = base.visual;
        s.collection = base.collection;
        s.cursor_check = base.cursor_check;
        s.constant_tracking = base.constant_tracking;
        s.color = base.color; // Color ranges and target source are application preferences.
        if (s.mouse_backend == 2) {
            s.direction.enabled = false;
            if (s.tracking.fov_button > 5)
                s.tracking.fov_button = base.tracking.fov_button;
        }
    } else {
        s.metadata.clear();
        s.classes.clear();
        s.selected_class_names.clear();
        s.class_preference = {};
        s.inference_backend = "auto";
        s.auto_size = true;
        s.input_size = 320;
        s.inference_fps = 0;
    }
    s.model = path;
    // Header inspection takes place on the inference worker while running.
    return apply_setup(s);
}
std::vector<LibraryEntry> Preferences::library() const {
    std::vector<LibraryEntry> result;
    for (const auto& [dirs, is_model] : {std::pair{&model_dirs_, true}, {&profile_dirs_, false}}) {
        for (const auto& directory : *dirs) {
            std::error_code error;
            std::filesystem::directory_iterator it(
                directory, std::filesystem::directory_options::skip_permission_denied, error),
                end;
            while (!error && it != end && result.size() < 5000) {
                auto entry = *it;
                it.increment(error);
                std::error_code file_error;
                if (!entry.is_regular_file(file_error))
                    continue;
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                if (is_model ? ext != ".onnx" && ext != ".engine" && ext != ".pt" : ext != ".json")
                    continue;
                auto file = std::filesystem::absolute(entry.path()).lexically_normal();
                result.push_back({file, file.filename().string(), is_model});
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const auto& a, const auto& b) { return a.path == b.path; }),
                 result.end());
    return result;
}
} // namespace receiver
