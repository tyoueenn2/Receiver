#include "receiver/preferences.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
namespace receiver {
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
    auto path = root_ / "preferences.json";
    if (!std::filesystem::exists(path))
        return;
    const auto j = read_json_file(path);
    if (j.value("version", 0) != 1)
        throw std::runtime_error("Unsupported preference version");
    auto settings = settings_from_json(j.at("settings"));
    auto models = j.value("models", nlohmann::json::object());
    if (!models.is_object() || models.size() > 1000)
        throw std::runtime_error("Invalid saved model library");
    for (const auto& value : models.items())
        settings_from_json(value.value());
    auto next = *this;
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
    *this = std::move(next);
}
void Preferences::save(const Settings& settings, const std::string& profile_path,
                       const std::vector<std::string>& names) {
    auto s = settings;
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
        s = settings_from_json(models_[identity]);
        // Connections and activation are application preferences, not properties of a model.
        s.bind_ip = base.bind_ip;
        s.sender_ip = base.sender_ip;
        s.pi_ip = base.pi_ip;
        s.frame_port = base.frame_port;
        s.pi_port = base.pi_port;
        s.mouse_backend = base.mouse_backend;
        s.activation_button = base.activation_button;
        s.secondary_button = base.secondary_button;
        s.preview = base.preview;
    } else {
        s.metadata.clear();
        s.classes.clear();
        s.selected_class_names.clear();
        s.inference_backend = "auto";
        s.auto_size = true;
        s.input_size = 320;
        s.inference_fps = 0;
    }
    s.model = path;
    // Header inspection takes place on the inference worker while running.
    return s;
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
