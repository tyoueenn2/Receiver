#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace {
// Asynchronous in-app file picker on Linux; keep native Windows dialogs.
class FileDialog {
    std::string request_, completed_, error_;
    std::optional<std::string> selected_;
    std::array<char, 4096> path_{};
    bool save_ = false, open_ = false, overwrite_ = false;
    int kind_ = 0;
    std::filesystem::path directory_;
    void set_path(const std::filesystem::path& path) {
        std::snprintf(path_.data(), path_.size(), "%s", path.string().c_str());
        overwrite_ = false;
    }

  public:
    bool ready(const char* id) const {
        return completed_ == id;
    }
    std::optional<std::string> choose(NativeWindow window, bool save, int kind, const char* id) {
#ifdef _WIN32
        return choose_file(window, save, kind);
#else
        (void)window;
        if (ready(id)) {
            completed_.clear();
            auto result = std::move(selected_);
            selected_.reset();
            return result;
        }
        request_ = id;
        save_ = save;
        kind_ = kind;
        directory_ = std::filesystem::current_path();
        set_path(directory_ / (save ? "profile.json" : ""));
        error_.clear();
        open_ = true;
        return {};
#endif
    }
    void render() {
        if (open_) {
            ImGui::OpenPopup("Choose a file");
            open_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(660, 490), ImGuiCond_FirstUseEver);
        if (!ImGui::BeginPopupModal("Choose a file", nullptr, ImGuiWindowFlags_NoSavedSettings))
            return;
        ImGui::TextUnformatted(save_ ? "Save profile" : "Open file");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##path", path_.data(), path_.size()))
            overwrite_ = false;
        if (ImGui::Button("Browse this folder")) {
            const std::filesystem::path path(path_.data());
            std::error_code ec;
            auto next = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
            if (std::filesystem::is_directory(next, ec)) {
                directory_ = next;
                error_.clear();
            } else
                error_ = "Folder does not exist or cannot be opened.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Parent folder")) {
            directory_ = directory_.parent_path();
            set_path(directory_);
        }
        ImGui::TextWrapped("%s", directory_.string().c_str());
        ImGui::BeginChild("Files", ImVec2(0, -100), ImGuiChildFlags_Borders);
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> entries;
        for (std::filesystem::directory_iterator
                 it(directory_, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             it != end && !ec; it.increment(ec)) {
            const auto& entry = *it;
            const auto ext = entry.path().extension().string();
            if (entry.is_directory(ec) || kind_ == 2 ||
                (kind_ == 1 ? ext == ".onnx" || ext == ".pt" || ext == ".engine"
                            : ext == ".json" || (kind_ == 3 && ext == ".cfg")))
                entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
        for (const auto& entry : entries) {
            const bool folder = entry.is_directory(ec);
            auto label = (folder ? "[Folder] " : "") + entry.path().filename().string();
            if (ImGui::Selectable(label.c_str())) {
                set_path(entry.path());
                if (folder)
                    directory_ = entry.path();
            }
        }
        ImGui::EndChild();
        if (!error_.empty())
            ImGui::TextWrapped("%s", error_.c_str());
        if (overwrite_)
            ImGui::TextUnformatted("This file exists. Select Replace to overwrite it.");
        if (ImGui::Button(save_ ? (overwrite_ ? "Replace" : "Save") : "Open")) {
            auto path = std::filesystem::absolute(std::filesystem::path(path_.data()), ec);
            if (save_ && path.extension().empty())
                path += ".json";
            if (std::filesystem::is_directory(path, ec)) {
                directory_ = path;
                error_.clear();
            } else if (!save_ && !std::filesystem::is_regular_file(path, ec)) {
                error_ = "Select an existing file.";
            } else if (save_ && !std::filesystem::is_directory(path.parent_path(), ec)) {
                error_ = "The destination folder does not exist.";
            } else if (save_ && std::filesystem::exists(path, ec) && !overwrite_) {
                overwrite_ = true;
            } else {
                selected_ = std::filesystem::absolute(path).string();
                completed_ = request_;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
};
} // namespace
