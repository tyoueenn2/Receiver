#pragma once
#include "model.hpp"
#include <filesystem>
namespace receiver {
nlohmann::json settings_json(const Settings& settings);
Settings settings_from_json(const nlohmann::json& value);
void write_json_atomic(const nlohmann::json& value, const std::filesystem::path& path);
struct LibraryEntry {
    std::filesystem::path path;
    std::string name;
    bool model = false;
};
class Preferences {
    std::filesystem::path root_;
    nlohmann::json models_ = nlohmann::json::object();
    std::vector<std::filesystem::path> model_dirs_{"models"}, profile_dirs_{"profiles"};
    static std::string key(const std::string& model);

  public:
    Settings current;
    std::string profile;
    explicit Preferences(std::filesystem::path root = "user-settings") : root_(std::move(root)) {}
    void load();
    void save(const Settings& settings, const std::string& profile_path,
              const std::vector<std::string>& names = {});
    Settings select_model(const Settings& base, const std::string& path) const;
    void add_directory(const std::filesystem::path& directory, bool model);
    std::vector<LibraryEntry> library() const;
};
} // namespace receiver
