#include "receiver/test_hub.hpp"
#include "receiver/app.hpp"
#include "receiver/color.hpp"
#include "receiver/capture.hpp"
#include "receiver/preferences.hpp"
#include "receiver/assist.hpp"
#include "preview_window.hpp"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <future>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <sstream>
#include <type_traits>
#include <filesystem>
#include <nlohmann/json.hpp>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapchain = nullptr;
ID3D11RenderTargetView* target = nullptr;
ImFont* title_font = nullptr;
ImFont* label_font = nullptr;
ImFont* detail_font = nullptr;
ImVec4 accent{.19f, .59f, 1.f, 1.f};
constexpr ImVec4 muted{.53f, .59f, .68f, 1.f};

enum class Page {
    connections,
    targeting,
    activation,
    profiles,
    movement,
    diagnostics,
    models,
    preferences,
    auto_click,
    preview,
    capture
};
struct PageInfo {
    Page page;
    const char* slug;
    const char* label;
    const char* group;
    const char* description;
    const char* keywords;
};
constexpr PageInfo settings_pages[] = {
    {Page::connections, "connections", "Connections", "GET STARTED",
     "Choose the video source and where mouse commands are sent.",
     "setup practice demo network ip address ports raspberry pi mouse backend local windows capture udp "
     "display desktop"},
    {Page::models, "models", "Model", "GET STARTED",
     "Load a detection model and choose its speed and input size.",
     "onnx pt engine python runtime gpu fps performance benchmark image resolution inference"},
    {Page::activation, "activation", "Activation", "ASSISTANCE",
     "Choose when assistance runs and when it should pause.",
     "aim keybind primary secondary button constant tracking continuous cursor frame age stop safety "
     "maximum"},
    {Page::targeting, "targeting", "Targeting", "ASSISTANCE",
     "Choose what to follow, where to aim, and how to track moving targets.",
     "hsv color hybrid detection confidence class preference preferred head player proximity distance type "
     "fov search radius sticky persistence prediction kalman lead offset "
     "crosshair"},
    {Page::movement, "movement", "Movement", "ASSISTANCE",
     "Adjust sensitivity, smoothing, and the path of each mouse correction.",
     "motion humanization ema gain speed deadzone curve bezier jitter noise direction strength maximum "
     "correction limits response units counts"},
    {Page::auto_click, "auto-click", "Auto click", "ASSISTANCE",
     "Click automatically when a selected target stays under the crosshair.",
     "trigger spray fire repeat interval delay press duration button mouse hold timing first"},
    {Page::preview, "preview", "Preview", "WORKSPACE",
     "Inspect the received image and customize its detection overlays.",
     "display visual boxes fov circle confidence labels tracers color opacity border font lines detected "
     "targets search area label text size separate window popout detached"},
    {Page::capture, "data-capture", "Data capture", "WORKSPACE",
     "Save received images and optional training labels to a local folder.",
     "dataset collection frames save images yolo auto label interval directory recording output"},
    {Page::profiles, "profiles", "Profiles", "WORKSPACE",
     "Save a setup, restore a profile, or import an Aimmy configuration.",
     "saved settings load open file json cfg import export configuration"},
    {Page::preferences, "app", "App preferences", "WORKSPACE",
     "Customize this window and its keyboard shortcuts.",
     "settings theme accent always top stream guard capture exclusion model switch hotkey"},
    {Page::diagnostics, "diagnostics", "Diagnostics", "TOOLS",
     "Run setup checks and inspect live connection and performance details.",
     "test hub troubleshooting timings latency metrics error reports logs telemetry"},
};

void release_target() {
    if (target) {
        target->Release();
        target = nullptr;
    }
}
void create_target() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        device->CreateRenderTargetView(back, nullptr, &target);
        back->Release();
    }
}
LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l))
        return true;
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(l);
        limits->ptMinTrackSize = {1060, 740};
        return 0;
    }
    if (message == WM_SIZE && device && w != SIZE_MINIMIZED) {
        release_target();
        swapchain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
        create_target();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
bool input_string(const char* label, std::string& value) {
    std::array<char, 1024> b{};
    std::snprintf(b.data(), b.size(), "%s", value.c_str());
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    const bool edited = ImGui::InputText("##value", b.data(), b.size());
    ImGui::PopID();
    if (edited) {
        value = b.data();
        return true;
    }
    return false;
}
void hint(const char* text) {
    ImGui::PushFont(detail_font);
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
void section(const char* title, const char* description) {
    ImGui::Spacing();
    ImGui::PushFont(label_font);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    hint(description);
}
void field_label(const char* label, const char* value = nullptr) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                        (value ? ImGui::CalcTextSize(value).x : 0);
    ImGui::PushTextWrapPos(value ? right - 10 : 0);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    if (value) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(right);
        ImGui::TextColored(accent, "%s", value);
    }
    ImGui::SetNextItemWidth(-1);
}
bool slider_float(const char* label, float* value, float low, float high, const char* format = "%.3f") {
    ImGui::PushID(label);
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), format, *value);
    field_label(label, formatted);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const bool edited = ImGui::SliderFloat("##value", value, low, high, "");
    ImGui::PopStyleVar();
    ImGui::PopID();
    return edited;
}
bool slider_int(const char* label, int* value, int low, int high, const char* format = "%d") {
    ImGui::PushID(label);
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), format, *value);
    field_label(label, formatted);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const bool edited = ImGui::SliderInt("##value", value, low, high, "");
    ImGui::PopStyleVar();
    ImGui::PopID();
    return edited;
}
bool begin_combo(const char* label, const char* value) {
    const std::string id = std::string("##") + label;
    field_label(label);
    return ImGui::BeginCombo(id.c_str(), value);
}
template <typename Items, typename... Args>
bool combo(const char* label, int* value, Items items, Args... args) {
    const bool hidden = label[0] == '#' && label[1] == '#';
    if (!hidden)
        field_label(label);
    else
        ImGui::SetNextItemWidth(-1);
    const std::string id = hidden ? label : std::string("##") + label;
    return ImGui::Combo(id.c_str(), value, items, args...);
}
template <typename Value, typename... Args> bool number_input(const char* label, Value* value, Args... args) {
    field_label(label);
    const std::string id = std::string("##") + label;
    if constexpr (std::is_same_v<Value, int>)
        return ImGui::InputInt(id.c_str(), value, args...);
    else
        return ImGui::InputFloat(id.c_str(), value, args...);
}
bool setting_columns(const char* id) {
    const int columns = ImGui::GetContentRegionAvail().x >= 580 ? 2 : 1;
    const bool opened = ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchSame);
    if (opened)
        ImGui::TableNextColumn();
    return opened;
}
bool percent_slider(const char* label, float& value) {
    float percent = value * 100;
    if (!slider_float(label, &percent, 0, 100, "%.0f%%"))
        return false;
    value = percent / 100;
    return true;
}
std::optional<std::string> choose_file(HWND window, bool save, int model = 0) {
    std::array<char, 4096> path{};
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = DWORD(path.size());
    dialog.lpstrFilter = model == 3   ? "Aimmy configs (*.cfg;*.json)\0*.cfg;*.json\0\0"
                         : model == 2 ? "Programs (*.exe)\0*.exe\0\0"
                         : model      ? "Detection models (*.onnx;*.pt;*.engine)\0*.onnx;*.pt;*.engine\0\0"
                                      : "Saved settings (*.json)\0*.json\0\0";
    dialog.lpstrDefExt = model ? "onnx" : "json";
    dialog.Flags =
        OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog))
        return std::string(path.data());
    return std::nullopt;
}
const char* mouse_buttons[] = {"Left mouse button",
                               "Right mouse button",
                               "Middle mouse button",
                               "Side button 1",
                               "Side button 2",
                               "Button 6",
                               "Button 7",
                               "Button 8"};
struct Texture {
    ID3D11Texture2D* image = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    int w = 0, h = 0;
    uint64_t session = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> rgba;
    ~Texture() {
        clear();
    }
    void clear() {
        if (view)
            view->Release();
        if (image)
            image->Release();
        view = nullptr;
        image = nullptr;
        w = h = 0;
        session = 0;
        rgba.clear();
    }
    void update(const receiver::Frame& f) {
        if (view && session == f.header.session && sequence == f.header.sequence)
            return;
        if (w != f.header.width || h != f.header.height) {
            clear();
            w = f.header.width;
            h = f.header.height;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &image)) ||
                FAILED(device->CreateShaderResourceView(image, nullptr, &view))) {
                clear();
                return;
            }
            rgba.resize(size_t(w) * h * 4);
        }
        int c = f.header.format == 1 ? 3 : 4;
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            rgba[i * 4] = f.pixels[i * c + (c == 4 ? 2 : 0)];
            rgba[i * 4 + 1] = f.pixels[i * c + 1];
            rgba[i * 4 + 2] = f.pixels[i * c + (c == 4 ? 0 : 2)];
            rgba[i * 4 + 3] = 255;
        }
        context->UpdateSubresource(image, 0, nullptr, rgba.data(), UINT(w * 4), 0);
        session = f.header.session;
        sequence = f.header.sequence;
    }
};
void save_backbuffer(const char* path, IDXGISwapChain* source = swapchain) {
    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(source->GetBuffer(0, IID_PPV_ARGS(&back))))
        throw std::runtime_error("Cannot read GUI framebuffer");
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        back->Release();
        throw std::runtime_error("Cannot stage GUI framebuffer");
    }
    context->CopyResource(staging, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        staging->Release();
        throw std::runtime_error("Cannot map GUI framebuffer");
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = LONG(desc.Width);
    info.biHeight = -LONG(desc.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    std::vector<uint8_t> row(desc.Width * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        auto* src = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            row[4 * x] = src[4 * x + 2];
            row[4 * x + 1] = src[4 * x + 1];
            row[4 * x + 2] = src[4 * x];
            row[4 * x + 3] = 255;
        }
        out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    context->Unmap(staging, 0);
    staging->Release();
    if (!out)
        throw std::runtime_error("Cannot save GUI smoke-test image");
}
} // namespace
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR arguments, int) {
    const std::string options(arguments);
    bool smoke = options.find("--smoke-test") != std::string::npos;
    bool demo = options.find("--demo") != std::string::npos;
    const bool compact = smoke && options.find("--compact") != std::string::npos;
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, window_proc,          0,      0, instance, nullptr, nullptr,
                   nullptr,    nullptr,    L"UdpVisionReceiver", nullptr};
    RegisterClassExW(&wc);
    HWND window =
        CreateWindowW(wc.lpszClassName, L"UDP Vision Receiver", WS_OVERLAPPEDWINDOW, 80, 60,
                      compact ? 1060 : 1280, compact ? 740 : 900, nullptr, nullptr, instance, nullptr);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL feature;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
                                             D3D11_SDK_VERSION, &desc, &swapchain, &device, &feature,
                                             &context))) {
        MessageBoxW(window, L"DirectX 11 initialization failed", L"Receiver", MB_ICONERROR);
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }
    create_target();
    ShowWindow(window, smoke ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(window);
    IMGUI_CHECKVERSION();
    auto* main_gui = ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    auto* fonts = ImGui::GetIO().Fonts;
    fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16);
    title_font = fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\bahnschrift.ttf", 28);
    label_font = fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 15);
    detail_font = fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 14);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.WindowBorderSize = 0;
    style.FrameRounding = 2;
    style.ChildRounding = 3;
    style.PopupRounding = 3;
    style.ScrollbarRounding = 3;
    style.GrabRounding = 8;
    style.GrabMinSize = 12;
    style.ScrollbarSize = 6;
    style.ItemSpacing = ImVec2(12, 7);
    style.ItemInnerSpacing = ImVec2(8, 5);
    style.WindowPadding = ImVec2(24, 20);
    style.FramePadding = ImVec2(10, 4);
    style.CellPadding = ImVec2(12, 5);
    style.SeparatorTextPadding = ImVec2(0, 5);
    style.SeparatorTextBorderSize = 1;
    style.Colors[ImGuiCol_Text] = ImVec4(.88f, .90f, .94f, 1);
    style.Colors[ImGuiCol_TextDisabled] = muted;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.035f, .05f, .075f, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(.055f, .067f, .09f, 1);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(.07f, .09f, .13f, 1);
    style.Colors[ImGuiCol_Border] = ImVec4(.13f, .18f, .25f, 1);
    style.Colors[ImGuiCol_Separator] = ImVec4(.13f, .18f, .25f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.09f, .13f, .19f, 1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.12f, .21f, .32f, 1);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(.14f, .27f, .41f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(.10f, .18f, .28f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.15f, .30f, .46f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(.16f, .39f, .65f, 1);
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.Colors[ImGuiCol_SliderGrab] = accent;
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(.42f, .73f, 1.f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(.09f, .15f, .23f, 1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.12f, .24f, .37f, 1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(.14f, .30f, .47f, 1);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(.23f, .30f, .39f, 1);
    style.Colors[ImGuiCol_NavCursor] = accent;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    receiver::App app;
    receiver::TestHub test_hub;
    receiver::TestOptions test_options;
    test_options.python = receiver::TestHub::find_python();
    std::vector<bool> test_selection(13, false);
    for (int i = 0; i < 8; ++i)
        test_selection[i] = true;
    int selected_test = 0;
    receiver::Settings settings;
    std::vector<receiver::CaptureOutput> capture_displays;
    bool capture_displays_loaded = false;
    bool simulate = false, done = false;
#ifndef RECEIVER_HAS_TENSORRT
    simulate = true;
#endif
    std::vector<std::string> names;
    uint64_t previous_frames = 0;
    int64_t last_frame = 0;
    std::string notice;

    Page page = Page::connections;
    std::array<char, 128> settings_search{};
    auto go_to = [&](Page destination) {
        page = destination;
        settings_search.fill(0);
    };
    if (smoke) {
        const auto find = options.find("--find=");
        if (find != std::string::npos) {
            auto value = options.substr(find + 7, options.find(' ', find) - (find + 7));
            std::replace(value.begin(), value.end(), '+', ' ');
            std::snprintf(settings_search.data(), settings_search.size(), "%s", value.c_str());
        }
    }
    for (const auto& item : settings_pages)
        if (options.find(std::string("--page=") + item.slug) != std::string::npos)
            page = item.page;
    // Keep existing launch links compatible.
    const std::pair<const char*, Page> aliases[] = {
        {"setup", Page::connections}, {"detect", Page::targeting}, {"mouse", Page::activation},
        {"human", Page::movement},    {"saved", Page::profiles},   {"settings", Page::preferences},
        {"hub", Page::diagnostics}};
    for (const auto& [name, destination] : aliases)
        if (options.find(std::string("--page=") + name) != std::string::npos)
            page = destination;
    if (smoke) {
        auto port = [&](const std::string& key, int current) {
            auto pos = options.find(key);
            return pos == std::string::npos ? current : std::stoi(options.substr(pos + key.size()));
        };
        settings.frame_port = port("--test-frame-port=", settings.frame_port);
        settings.pi_port = port("--test-pi-port=", settings.pi_port);
    }
    std::string profile, error, class_text;
    std::vector<std::string> import_notes;
    bool capture_model_key = false;
    std::array<bool, 256> previous_keys{};
    int last_topmost = -1;
    int last_stream_guard = -1;
    receiver::Preferences preferences;
    bool preferences_dirty = false;
    int64_t preferences_changed = 0, library_updated = 0;
    uint64_t model_revision = 0;
    std::string model_draft, model_filter, profile_filter;
    int performance_goal = 1;
    std::vector<receiver::LibraryEntry> library;
    if (!smoke && !demo) {
        try {
            preferences.load();
            settings = preferences.current;
            profile = preferences.profile;
            if (!settings.model.empty())
                simulate = false;
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    model_draft = settings.model;
    auto mark_preferences = [&] {
        preferences_dirty = true;
        preferences_changed = receiver::now_ns();
    };
    auto open_model = [&](const std::string& path) {
        try {
            if (!settings.model.empty())
                preferences.save(app.stats().running ? app.settings() : settings, profile, names);
            auto selected = preferences.select_model(settings, path);
            if (selected.color.mode != 1)
                receiver::select_backend(selected);
            if (app.stats().running)
                app.configure(selected, true);
            settings = selected;
            model_draft = settings.model;
            preferences.add_directory(std::filesystem::absolute(path).parent_path(), true);
            library_updated = 0;
            names.clear();
            class_text.clear();
            mark_preferences();
            error.clear();
        } catch (const std::exception& e) {
            error = e.what();
        }
    };
    auto open_profile = [&](const std::string& path) {
        try {
            auto loaded = receiver::load_settings(path);
            app.arm(false);
            if (app.stats().running)
                app.configure(loaded);
            settings = loaded;
            profile = path;
            model_draft = settings.model;
            class_text.clear();
            names.clear();
            preferences.add_directory(std::filesystem::absolute(path).parent_path(), false);
            library_updated = 0;
            mark_preferences();
            notice = "Settings loaded. Mouse control is off.";
            error.clear();
        } catch (const std::exception& e) {
            error = e.what();
        }
    };
    auto matches = [](std::string value, std::string query) {
        for (auto& c : value)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : query)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return value.find(query) != std::string::npos;
    };
    auto matches_settings = [&](std::string text, const std::string& query) {
        std::istringstream words(query);
        std::string word;
        while (words >> word)
            if (!matches(text, word))
                return false;
        return true;
    };
    std::future<void> operation;
    Texture texture;
    receiver::gui::PreviewWindow separate_preview;
    bool pending_preview_close = false;
    std::array<float, 180> latency{};
    size_t graph = 0;
    auto smoke_started = receiver::now_ns();
    int smoke_exit = 0;
    ImVec2 last_display_size(compact ? 1060.f : 1280.f, compact ? 740.f : 900.f);
    if (smoke || demo) {
        simulate = true;
        names = {"Example object"};
        settings.preview = true;
        if (smoke && options.find("--test-preference=global") != std::string::npos)
            settings.class_preference = {0, "Example object", 0, 32};
        if (smoke && options.find("--test-preference=near") != std::string::npos)
            settings.class_preference = {0, "Example object", 1, 32};
        if (smoke && options.find("--test-color=only") != std::string::npos)
            settings.color.mode = 1;
        if (smoke && options.find("--test-color=hybrid") != std::string::npos)
            settings.color.mode = 2;
        app.start(settings, true);
    }
    auto busy = [&]() {
        return operation.valid() && operation.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    };
    while (!done) {
        auto ui_start = receiver::now_ns();
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (operation.valid() && !busy()) {
            try {
                operation.get();
                error.clear();
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        auto stats = app.stats();
        if (stats.running && stats.model_revision != model_revision && !stats.model_loading) {
            model_revision = stats.model_revision;
            settings = app.settings();
            model_draft = settings.model;
            names = stats.model_classes;
            class_text.clear();
            for (int id : settings.classes) {
                if (!class_text.empty())
                    class_text += ",";
                class_text += std::to_string(id);
            }
            mark_preferences();
        }
        if (receiver::now_ns() - library_updated > 1'000'000'000) {
            library = preferences.library();
            library_updated = receiver::now_ns();
        }
        if (!simulate && !stats.model_classes.empty())
            names = stats.model_classes;
        auto previous_settings = settings;
        accent = ImVec4(settings.ui.accent[0], settings.ui.accent[1], settings.ui.accent[2], 1);
        style.Colors[ImGuiCol_CheckMark] = style.Colors[ImGuiCol_SliderGrab] =
            style.Colors[ImGuiCol_NavCursor] = accent;
        if (!smoke && last_topmost != int(settings.ui.always_on_top)) {
            SetWindowPos(window, settings.ui.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            last_topmost = int(settings.ui.always_on_top);
        }
        if (!smoke && last_stream_guard != int(settings.ui.stream_guard)) {
            if (!SetWindowDisplayAffinity(window, settings.ui.stream_guard ? 0x11 : 0))
                error = "Windows could not apply capture exclusion to this window.";
            last_stream_guard = int(settings.ui.stream_guard);
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        auto& io = ImGui::GetIO();
        if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
            last_display_size = io.DisplaySize;
        else
            io.DisplaySize = last_display_size; // A minimized main window must not disturb the preview.
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Receiver", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        bool changed = false;
        if (separate_preview.close_requested()) {
            separate_preview.close();
            pending_preview_close = true;
        }
        if (pending_preview_close) {
            settings.preview = false;
            changed = !stats.model_loading && !stats.benchmarking;
        }
        if (smoke && options.find("--test-preview-reopen") != std::string::npos &&
            separate_preview.openings() == 1 && !separate_preview.handle() &&
            receiver::now_ns() - smoke_started > 1'800'000'000) {
            settings.preview = true;
            changed = true;
        }
        if (stats.inferred != previous_frames) {
            last_frame = receiver::now_ns();
            previous_frames = stats.inferred;
        }
        const bool receiving =
            stats.running && stats.inferred && receiver::now_ns() - last_frame < 500'000'000;
        ImGui::PushFont(title_font);
        ImGui::TextUnformatted("VISION");
        ImGui::SameLine();
        ImGui::TextColored(accent, "RECEIVER");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 230);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(simulate ? muted : accent, "%s",
                           settings.color.mode == 1 ? "HSV COLOR"
                           : simulate               ? "PRACTICE MODE"
                           : settings.model.empty() ? "NO MODEL SELECTED"
                                                    : "MODEL SELECTED");
        hint("DEVICE SETUP  /  TARGETING  /  ASSISTANCE");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, accent);
        ImGui::Separator();
        ImGui::PopStyleColor();

        std::vector<const PageInfo*> visible_pages;
        for (const auto& item : settings_pages)
            if (matches_settings(std::string(item.label) + " " + item.description + " " + item.keywords,
                                 settings_search.data()))
                visible_pages.push_back(&item);
        std::array<bool, 256> pressed{};
        for (int key = 8; key < 255; ++key) {
            const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
            pressed[key] = down && !previous_keys[key];
            previous_keys[key] = down;
        }
        if (!smoke && GetForegroundWindow() == window && !ImGui::GetIO().WantTextInput) {
            if (capture_model_key) {
                for (int key = 8; key < 255; ++key)
                    if (pressed[key] && key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU &&
                        (key < VK_LSHIFT || key > VK_RMENU) && key != VK_ESCAPE && key != VK_DELETE &&
                        key != 'Q' && key != 'E') {
                        settings.ui.model_switch_key = key;
                        capture_model_key = false;
                        changed = true;
                        break;
                    }
                if (pressed[VK_ESCAPE])
                    capture_model_key = false;
            } else if (pressed['Q'] || pressed['E']) {
                for (size_t i = 0; i < visible_pages.size(); ++i)
                    if (page == visible_pages[i]->page) {
                        page = visible_pages[(i + (pressed['E'] ? 1 : visible_pages.size() - 1)) %
                                             visible_pages.size()]
                                   ->page;
                        break;
                    }
            } else if (settings.ui.model_switch && pressed[settings.ui.model_switch_key] && !busy() &&
                       !stats.model_loading && !stats.benchmarking) {
                std::vector<std::string> models;
                for (const auto& entry : library)
                    if (entry.model)
                        models.push_back(entry.path.string());
                if (!models.empty()) {
                    const auto current = std::find(models.begin(), models.end(), settings.model);
                    const size_t next =
                        current == models.end() ? 0 : (size_t(current - models.begin()) + 1) % models.size();
                    app.arm(false);
                    open_model(models[next]);
                } else
                    notice = "Add a model in Model before using the switch key.";
            }
        }
        const char* next_step =
            !stats.error.empty() ? "Something needs attention. Stop, check the details below, then try again."
            : stats.model_loading || stats.benchmarking || busy()
                ? "Getting ready. Loading a new model can take a few minutes."
            : !stats.running
                ? (settings.color.mode == 1
                       ? "Choose Connections and the HSV range in Targeting, then start disarmed."
                       : "Choose Connections and Model, then start the session. Assistance starts disarmed.")
            : !receiving && settings.frame_source == "local"
                ? "Waiting for a fresh desktop image. Keep the capture region visible on the selected "
                  "display."
            : !receiving          ? "Waiting for pictures. Start the picture sender on the sending computer."
            : !stats.synchronized ? "Pictures are arriving. Waiting for the sender's timing check."
            : !stats.pi_ready && settings.mouse_backend == 1 ? "Waiting for the local Windows mouse reader."
            : !stats.pi_ready ? "Waiting for the mouse device. Check the Pi address and install its receiver "
                                "support update."
            : settings.direction.enabled && !stats.motion_available && settings.mouse_backend == 1
                ? "Waiting for local physical mouse readings."
            : settings.direction.enabled && !stats.motion_available
                ? "Waiting for physical mouse movement data. Install the Pi motion update or turn "
                  "direction-based help off."
            : !stats.armed
                ? "Session ready. Arm assistance to allow movement; use Preview to inspect the video first."
            : !stats.active               ? "Assistance is armed. Activation settings determine when it runs."
            : settings.mouse_backend == 1 ? "Local assistance is active. Press Delete to disarm."
            : simulate
                ? "Practice is active. Commands go to the simulator; your real mouse is unaffected."
                : "Mouse control is enabled. Movement requires a fresh picture and a matching detection.";
        ImGui::BeginDisabled(busy() || test_hub.running());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.13f, .46f, .80f, 1));
        if (!stats.running) {
            if (ImGui::Button(simulate ? "Start practice" : "Start session", ImVec2(150, 34))) {
                auto cfg = settings;
                const bool use_simulation = simulate;
                if (use_simulation)
                    cfg.sender_ip = cfg.pi_ip = "127.0.0.1";
                names =
                    use_simulation ? std::vector<std::string>{"Example object"} : std::vector<std::string>{};
                model_revision = 0;
                last_frame = 0;
                previous_frames = 0;
                operation = std::async(std::launch::async,
                                       [&, cfg, use_simulation] { app.start(cfg, use_simulation); });
            }
        } else {
            if (ImGui::Button("End session", ImVec2(150, 34))) {
                app.arm(false);
                operation = std::async(std::launch::async, [&] { app.stop(); });
            }
        }
        ImGui::PopStyleColor();
        if (stats.running) {
            ImGui::SameLine(0, 20);
            bool armed = stats.armed;
            if (ImGui::Checkbox(settings.mouse_backend == 1 ? "Arm local assistance"
                                : simulate                  ? "Arm practice assistance"
                                                            : "Arm assistance",
                                &armed))
                app.arm(armed);
        }
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 220);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.22f, .11f, .15f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.40f, .15f, .20f, 1));
        if (ImGui::Button("Disarm  [Delete]", ImVec2(220, 34)))
            app.arm(false);
        ImGui::PopStyleColor(2);
        hint(next_step);
        if (!stats.error.empty() || !error.empty()) {
            ImGui::TextColored(ImVec4(1, .65f, .5f, 1), "Unable to finish that step");
            ImGui::TextWrapped("%s", error.empty() ? stats.error.c_str() : error.c_str());
        }
        if (!stats.model_error.empty())
            ImGui::TextColored(ImVec4(1, .65f, .5f, 1), "%s", stats.model_error.c_str());
        if (!stats.model_message.empty())
            hint(stats.model_message.c_str());
        if (!notice.empty())
            ImGui::TextWrapped("%s", notice.c_str());
        ImGui::Spacing();

        const float body_height = std::max(100.f, ImGui::GetContentRegionAvail().y - 34);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
        ImGui::BeginChild("Settings navigation", ImVec2(190, body_height), ImGuiChildFlags_Borders);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##find-settings", "Find a setting...", settings_search.data(),
                                 settings_search.size());
        visible_pages.clear();
        for (const auto& item : settings_pages)
            if (matches_settings(std::string(item.label) + " " + item.description + " " + item.keywords,
                                 settings_search.data()))
                visible_pages.push_back(&item);
        if (settings_search[0] && !visible_pages.empty() &&
            std::none_of(visible_pages.begin(), visible_pages.end(),
                         [&](const auto* item) { return item->page == page; }))
            page = visible_pages.front()->page;
        const char* last_group = nullptr;
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, .5f));
        for (const auto* item : visible_pages) {
            if (!last_group || std::string(last_group) != item->group) {
                ImGui::Spacing();
                ImGui::PushFont(detail_font);
                ImGui::TextDisabled("%s", item->group);
                ImGui::PopFont();
                last_group = item->group;
            }
            const bool selected = item->page == page;
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  selected ? ImVec4(.08f, .16f, .25f, 1) : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, selected ? style.Colors[ImGuiCol_Text] : muted);
            if (ImGui::Button(item->label, ImVec2(-1, 25)))
                page = item->page;
            if (selected) {
                const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(a, ImVec2(a.x, b.y),
                                                    ImGui::ColorConvertFloat4ToU32(accent), 2);
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleVar();
        if (visible_pages.empty()) {
            hint("No matching category.");
            if (ImGui::Button("Clear search"))
                settings_search.fill(0);
        }
        const float navigation_overflow = ImGui::GetScrollMaxY();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::SameLine();
        const Page rendered_page = page;
        const std::string controls_id = "Settings content##" + std::to_string(static_cast<int>(page));
        ImGui::BeginChild(controls_id.c_str(), ImVec2(0, body_height), ImGuiChildFlags_Borders);
        ImGui::PushItemWidth(-1);
        for (const auto& item : settings_pages)
            if (item.page == page) {
                ImGui::PushFont(title_font);
                ImGui::TextUnformatted(item.label);
                ImGui::PopFont();
                hint(item.description);
                ImGui::Spacing();
                ImGui::Separator();
                break;
            }
        if (rendered_page == Page::connections) {

            const bool columns = setting_columns("Connections groups");
            section("Session mode", "Practice with simulated devices, or connect a live video feed.");
            ImGui::BeginDisabled(stats.running || busy());
            if (ImGui::RadioButton("Practice with simulated devices", simulate)) {
                simulate = true;
                settings.sender_ip = settings.pi_ip = "127.0.0.1";
                mark_preferences();
            }
            if (ImGui::RadioButton("Use connected devices", !simulate)) {
                simulate = false;
                mark_preferences();
            }
            section("Mouse output", "Choose which device receives the corrections.");
            if (combo("Output device", &settings.mouse_backend,
                      "Raspberry Pi / simulated Pi\0This computer's Windows mouse\0")) {
                settings.activation_button = 2;
                settings.secondary_button = 0;
                settings.tracking.fov_button = 1;
                changed = true;
            }
            hint("Windows output moves this computer's real cursor when assistance is armed.");
            if (columns)
                ImGui::TableNextColumn();
            section("Video source", "Capture this computer's desktop or receive pictures over UDP.");
            int source = settings.frame_source == "local" ? 1 : 0;
            if (combo("Picture source", &source, "UDP sender\0Local desktop capture\0")) {
                settings.frame_source = source ? "local" : "udp";
                changed = true;
            }
            if (settings.frame_source == "local") {
                if (ImGui::Button("Refresh displays") || !capture_displays_loaded) {
                    capture_displays_loaded = true;
                    try {
                        capture_displays = receiver::capture_outputs();
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                }
                std::string selected = "Display " + std::to_string(settings.capture_output);
                for (const auto& display : capture_displays)
                    if (display.index == settings.capture_output)
                        selected += " (" + display.name + ")";
                if (ImGui::BeginCombo("Capture display", selected.c_str())) {
                    for (const auto& display : capture_displays) {
                        auto label = std::to_string(display.index) + ": " + display.name + "  " +
                                     std::to_string(display.width) + " x " + std::to_string(display.height);
                        if (ImGui::Selectable(label.c_str(), display.index == settings.capture_output)) {
                            settings.capture_output = display.index;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                hint("Captures an exact center crop matching the model input size. Keep Receiver outside "
                     "that region. No separate sender is needed.");
            }
            if (simulate) {
                settings.sender_ip = settings.pi_ip = "127.0.0.1";
                hint(settings.frame_source == "local"
                         ? "Practice uses desktop images with example detections and a simulated Pi."
                         : "Run Start Demo.cmd to launch the example video sender and simulated Pi. No model or "
                           "hardware is required.");
            } else {
                if (settings.frame_source == "udp")
                    changed |= input_string("Video sender IP address", settings.sender_ip);
                ImGui::BeginDisabled(settings.mouse_backend != 0);
                changed |= input_string("Raspberry Pi IP address", settings.pi_ip);
                ImGui::EndDisabled();
                hint("For example: 192.168.1.20. Addresses apply only to connected UDP devices.");
            }
            if (ImGui::CollapsingHeader("Advanced network options")) {
                if (settings.frame_source == "udp") {
                    changed |= input_string("Listen on IP address", settings.bind_ip);
                    changed |= number_input("Video UDP port", &settings.frame_port);
                }
                ImGui::BeginDisabled(settings.mouse_backend != 0);
                changed |= number_input("Pi command UDP port", &settings.pi_port);
                ImGui::EndDisabled();
                hint("0.0.0.0 listens on every local interface. Ports must match the sender and Pi.");
            }
            ImGui::EndDisabled();
            if (stats.running || busy())
                hint("End the session before changing connections.");
            section("Next step", "Choose a model for live detection, then start the session. Preview works "
                                 "while assistance is disarmed.");
            if (ImGui::Button("Choose model"))
                go_to(Page::models);
            ImGui::SameLine();
            if (ImGui::Button("Open preview"))
                go_to(Page::preview);
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::models) {

            const bool columns = setting_columns("Model groups");
            section("Choose a model", "Switch models without reconnecting. Your previous model stays "
                                      "available if loading fails.");
            ImGui::BeginDisabled(busy() || stats.model_loading || stats.benchmarking ||
                                 (stats.running && simulate));
            input_string("Model file", model_draft);
            if (ImGui::Button("Load selected model"))
                open_model(model_draft);
            ImGui::SameLine();
            if (ImGui::Button("Browse..."))
                if (auto path = choose_file(window, false, true))
                    open_model(*path);
            hint("Models and profiles in your chosen folders refresh automatically. Settings are "
                 "remembered separately for each model.");
            input_string("Search models", model_filter);
            ImGui::BeginChild("Model library", ImVec2(0, 115), ImGuiChildFlags_Borders);
            bool found_model = false;
            for (const auto& item : library)
                if (item.model && matches(item.name, model_filter)) {
                    found_model = true;
                    ImGui::PushID(item.path.string().c_str());
                    if (ImGui::Selectable(item.name.c_str(), item.path.string() == settings.model))
                        open_model(item.path.string());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", item.path.string().c_str());
                    ImGui::PopID();
                }
            if (!found_model)
                hint("No matching models. Use Browse to add a model file.");
            ImGui::EndChild();
            section("Inference settings",
                    "Fixed-size models always use their required size. Dynamic models can "
                    "use a saved size or a performance recommendation.");
            changed |= ImGui::Checkbox("Use model-recommended input size", &settings.auto_size);
            if (!settings.auto_size)
                changed |= number_input("Input resolution", &settings.input_size, 32, 160);
            ImGui::Text("Current size: %d x %d", settings.input_size, settings.input_size);
            if (!stats.running && settings.auto_size)
                hint("The model's preferred size will be read when you start.");
            changed |= number_input("Inference FPS limit (0 = unlimited)", &settings.inference_fps, 10);
            if (ImGui::CollapsingHeader("Advanced model runtime")) {
                const char* backends[]{"Auto", "TensorRT (.onnx / .engine)", "YOLO-Omni / PyTorch (.pt)"};
                int backend = settings.inference_backend == "tensorrt"    ? 1
                              : settings.inference_backend == "yolo_omni" ? 2
                                                                          : 0;
                if (combo("Detection backend", &backend, backends, 3)) {
                    settings.inference_backend = backend == 1   ? "tensorrt"
                                                 : backend == 2 ? "yolo_omni"
                                                                : "auto";
                    changed = true;
                }
                input_string("Python executable", settings.omni_python);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    changed = true;
                input_string("YOLO-Omni source folder", settings.omni_source);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    changed = true;
                int device = settings.omni_device == "cpu" ? 1 : 0;
                if (combo("PyTorch device", &device, "NVIDIA GPU\0CPU (testing)\0")) {
                    settings.omni_device = device == 1 ? "cpu" : "cuda:0";
                    changed = true;
                }
                input_string("Model metadata file", settings.metadata);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    changed = true;
                hint("Object names and supported image sizes are read from the model. An optional "
                     "companion file can supply missing metadata.");
            }
            ImGui::EndDisabled();
            if (columns)
                ImGui::TableNextColumn();
            section("Performance benchmark",
                    "Measures this model on a received picture at supported sizes. Mouse control stays "
                    "off. This measures speed, not detection accuracy or network delay.");
            combo("Optimization goal", &performance_goal, "Fastest\0Balanced\0Lower GPU load\0");
            ImGui::BeginDisabled(settings.color.mode == 1 || !stats.running || simulate ||
                                 stats.model_loading || stats.benchmarking || busy());
            if (ImGui::Button("Measure model performance")) {
                try {
                    app.benchmark(receiver::PerformanceGoal(performance_goal));
                    error.clear();
                } catch (const std::exception& e) {
                    error = e.what();
                }
            }
            ImGui::EndDisabled();
            if (stats.benchmarking && ImGui::Button("Cancel measurement"))
                app.cancel_benchmark();
            if (!stats.benchmark.samples.empty() &&
                ImGui::BeginTable("Model performance", 4, ImGuiTableFlags_Borders)) {
                for (auto label : {"Size", "FPS", "Average ms", "95% ms"})
                    ImGui::TableSetupColumn(label);
                ImGui::TableHeadersRow();
                for (const auto& sample : stats.benchmark.samples) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", sample.size);
                    ImGui::TableNextColumn();
                    if (!sample.error.empty()) {
                        ImGui::TextUnformatted("Unavailable");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", sample.error.c_str());
                    } else
                        ImGui::Text("%.1f", sample.fps);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", sample.mean_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", sample.p95_ms);
                }
                ImGui::EndTable();
            }
            if (stats.benchmark.recommended_size) {
                ImGui::Text("Suggested: %d pixels, %d FPS", stats.benchmark.recommended_size,
                            stats.benchmark.recommended_fps);
                ImGui::BeginDisabled(stats.model_loading || stats.benchmarking || busy());
                if (ImGui::Button("Apply recommendation")) {
                    settings.input_size = stats.benchmark.recommended_size;
                    settings.inference_fps = stats.benchmark.recommended_fps;
                    settings.auto_size = false;
                    changed = true;
                }
                ImGui::EndDisabled();
            }
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::activation) {

            const bool columns = setting_columns("Activation groups");
            section("Activation mode",
                    "Arming permits output. This mode determines when that output is active.");
            int mode = settings.constant_tracking ? 1 : 0;
            if (combo("Run assistance", &mode,
                      "While an activation button is held\0Continuously while armed\0")) {
                settings.constant_tracking = mode == 1;
                changed = true;
            }
            hint("Changing activation mode disarms assistance. Arm it again using the control at the top.");
            section("Activation buttons", "Use either button on the mouse selected in Connections.");
            ImGui::BeginDisabled(settings.constant_tracking);
            int button = settings.activation_button - 1;
            if (combo("Primary button", &button, mouse_buttons, settings.mouse_backend == 1 ? 5 : 8)) {
                settings.activation_button = button + 1;
                changed = true;
            }
            const char* secondary[] = {
                "None",          "Left mouse button", "Right mouse button", "Middle mouse button",
                "Side button 1", "Side button 2",     "Button 6",           "Button 7",
                "Button 8"};
            changed |= combo("Alternate button", &settings.secondary_button, secondary,
                             settings.mouse_backend == 1 ? 6 : 9);
            ImGui::EndDisabled();
            if (settings.constant_tracking)
                hint("Activation buttons are unused in continuous mode.");
            if (simulate && settings.mouse_backend == 0)
                hint("The simulated Pi reports the right button as held. Your real buttons do not control "
                     "it.");
            if (columns)
                ImGui::TableNextColumn();
            section("Pause conditions", "These checks still apply in continuous mode.");
            ImGui::BeginDisabled(settings.mouse_backend != 1);
            changed |= ImGui::Checkbox("Pause while the Windows cursor is visible", &settings.cursor_check);
            ImGui::EndDisabled();
            hint("Cursor visibility is available only with Windows mouse output. The other computer's cursor "
                 "cannot be inspected.");
            changed |= slider_int("Maximum frame age", &settings.max_age_ms, 1, 250, "%d ms");
            hint("Older images cannot produce movement or automatic clicks. Fresh video and mouse telemetry "
                 "are always required.");
            section("Stop assistance", "Delete or Disarm at the top stops output in either activation mode. "
                                       "In held mode, releasing the button also pauses assistance.");
            if (ImGui::Button("Change mouse output"))
                go_to(Page::connections);
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::targeting) {

            const bool columns = setting_columns("Targeting groups");
            auto& t = settings.tracking;
            section("Targeting mode", "Use a model, a color range, or colors inside the selected model box.");
            auto& c = settings.color;
            changed |= combo("Target source", &c.mode, "Model\0HSV color only\0Model + HSV color\0");
            if (c.mode) {
                hint(c.mode == 1
                         ? "Targets the highest matching pixel in the search area. No model is needed."
                         : "Selects the model target normally, then aims at its highest matching color. "
                           "If none matches, uses the normal box aim point.");
                section("HSV range", "Hue uses degrees. Saturation and value use percentages. Default: red.");
                changed |= slider_float("Minimum hue", &c.min_h, 0, 360, "%.0f deg");
                changed |= slider_float("Maximum hue", &c.max_h, 0, 360, "%.0f deg");
                hint("Minimum hue above maximum hue wraps through red (for example 330 to 30).");
                if (slider_float("Minimum saturation", &c.min_s, 0, 100, "%.0f%%")) {
                    c.max_s = std::max(c.max_s, c.min_s);
                    changed = true;
                }
                changed |= slider_float("Maximum saturation", &c.max_s, c.min_s, 100, "%.0f%%");
                if (slider_float("Minimum value", &c.min_v, 0, 100, "%.0f%%")) {
                    c.max_v = std::max(c.max_v, c.min_v);
                    changed = true;
                }
                changed |= slider_float("Maximum value", &c.max_v, c.min_v, 100, "%.0f%%");
                changed |= slider_float("Color horizontal offset", &c.offset_x, -1024, 1024, "%.1f px");
                changed |= slider_float("Color vertical offset", &c.offset_y, -1024, 1024, "%.1f px");
                hint("Positive offsets move right/down. Color offsets stay inside the image or selected box. "
                     "On the highest row, ties favor the pixel nearest the crosshair horizontally.");
                changed |= ImGui::Checkbox("Show color aim in preview", &c.preview);
                if (c.mode == 1) {
                    int units = settings.input_size / 32;
                    if (slider_int("Local capture size", &units, 1, 32, "%d x 32 px")) {
                        settings.input_size = units * 32;
                        changed = true;
                    }
                    hint("UDP frames keep the sender's resolution. Model class and confidence filters are "
                         "unused.");
                }
            }
            ImGui::BeginDisabled(c.mode == 1);
            section("Detection filters", "Choose which model detections can become targets.");
            changed |= percent_slider("Minimum confidence", settings.confidence);
            hint("Higher values reject uncertain detections.");
            std::string selected_class = settings.classes.empty() ? "All classes" : "Custom selection";
            if (settings.classes.size() == 1 && size_t(settings.classes[0]) < names.size())
                selected_class = names[settings.classes[0]];
            if (begin_combo("Target class", selected_class.c_str())) {
                if (ImGui::Selectable("All classes", settings.classes.empty())) {
                    settings.classes.clear();
                    settings.selected_class_names.clear();
                    class_text.clear();
                    changed = true;
                }
                for (size_t i = 0; i < names.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (ImGui::Selectable(names[i].c_str(), settings.classes == std::vector<int>{int(i)})) {
                        settings.classes = {int(i)};
                        settings.selected_class_names = {names[i]};
                        class_text = std::to_string(i);
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (names.empty())
                hint("Object names appear automatically after the model loads. "
                     "Advanced detection filters also accept class IDs.");

            int selection = settings.highest_confidence ? 1 : 0;
            if (combo("Target priority", &selection, "Closest to crosshair\0Highest confidence\0")) {
                settings.highest_confidence = selection == 1;
                changed = true;
            }
            section("Class preference", "Prefer a class while keeping the normal target class as fallback.");
            auto& preference = settings.class_preference;
            std::string preferred_label = preference.class_id < 0 ? "Off"
                                          : size_t(preference.class_id) < names.size()
                                              ? names[size_t(preference.class_id)]
                                              : "Class " + std::to_string(preference.class_id);
            if (begin_combo("Preferred class", preferred_label.c_str())) {
                if (ImGui::Selectable("Off", preference.class_id < 0)) {
                    preference.class_id = -1;
                    preference.class_name.clear();
                    changed = true;
                }
                for (size_t i = 0; i < names.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (ImGui::Selectable(names[i].c_str(), preference.class_id == int(i))) {
                        preference.class_id = int(i);
                        preference.class_name = names[i];
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (names.empty()) {
                if (number_input("Preferred class ID", &preference.class_id)) {
                    preference.class_name.clear();
                    changed = true;
                }
                hint("-1 turns preference off. Class names appear after the model loads.");
            }
            if (preference.class_id >= 0) {
                changed |= combo("Preference type", &preference.mode,
                                 "Whenever available\0Near the normal target\0");
                if (preference.mode == 1) {
                    changed |= slider_float("Maximum association distance", &preference.max_distance,
                                            0, 512, "%.0f px");
                    hint("Choose player as Target class and head as Preferred class. A head's center "
                         "must be inside the selected player's box or within this distance of its edge. "
                         "A head near another player does not change which player is selected.");
                } else
                    hint("An eligible preferred detection takes priority anywhere in the search area. "
                         "Otherwise the normal target class and priority apply.");
                hint("Preference can add a class outside the normal class filter. Confidence and search "
                     "radius still apply. Near mode needs a normal target of a different class.");
            }
            changed |= ImGui::Checkbox("Keep the current target", &settings.persistence);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Reduce switching between nearby detections. Matching options appear when enabled.");
            if (settings.persistence) {
                changed |= slider_float("Maximum target displacement", &t.sticky_distance, 0, 500, "%.0f px");
                hint("0 matches by box overlap only. Large distances can confuse nearby objects of the same "
                     "class.");
                if (ImGui::CollapsingHeader("Advanced target matching"))
                    changed |=
                        slider_float("Required box overlap", &settings.persistence_iou, .01f, 1, "%.2f IoU");
            }
            ImGui::EndDisabled();
            section("Search area", "Only target aim points inside this radius can be selected.");
            changed |= slider_float("Search radius", &settings.fov_radius, 0, 1024, "%.0f px");
            if (ImGui::CollapsingHeader("Change radius while holding a button")) {
                changed |= ImGui::Checkbox("Use a second search radius", &t.dynamic_fov);
                ImGui::BeginDisabled(!t.dynamic_fov);
                int button = t.fov_button - 1;
                if (combo("Radius-change button", &button, mouse_buttons,
                          settings.mouse_backend == 1 ? 5 : 8)) {
                    t.fov_button = button + 1;
                    changed = true;
                }
                changed |= slider_float("Radius while held", &t.held_radius, 1, 1024, "%.0f px");
                ImGui::EndDisabled();
                hint("Releasing this button restores the normal radius. This button does not activate "
                     "assistance.");
            }
            if (ImGui::CollapsingHeader("Advanced detection filters")) {
                changed |= slider_float("Duplicate-box threshold", &settings.nms_iou, 0, 1, "%.2f IoU");
                hint("Combines overlapping detections of the same class. The default is 0.45.");
                if (input_string("Class IDs (comma-separated)", class_text)) {

                    try {
                        std::vector<int> ids;
                        std::stringstream text(class_text);
                        std::string part;
                        while (std::getline(text, part, ',')) {
                            size_t used = 0;
                            int id = std::stoi(part, &used);
                            if (part.find_first_not_of(" \t", used) != std::string::npos || id < 0 ||
                                id > 9999)
                                throw std::runtime_error("Invalid object ID");
                            ids.push_back(id);
                        }
                        settings.classes = std::move(ids);
                        settings.selected_class_names.clear();
                        changed = true;
                    } catch (...) {
                        error = "Use object numbers separated by commas, or leave the field empty for "
                                "all types.";
                    }
                }
            }
            if (columns)
                ImGui::TableNextColumn();
            section("Aim point inside the box",
                    "Percentages position the point within each selected detection.");
            if (ImGui::Button("Box center")) {
                settings.aim_x = settings.aim_y = .5f;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Upper center")) {
                settings.aim_x = .5f;
                settings.aim_y = .25f;
                changed = true;
            }
            changed |= percent_slider("Horizontal position", settings.aim_x);
            changed |= percent_slider("Vertical position", settings.aim_y);
            if (ImGui::CollapsingHeader("Fine-tune with pixel offsets")) {
                changed |= slider_float("Horizontal offset", &settings.offset_x, -1024, 1024, "%.0f px");
                changed |= slider_float("Vertical offset", &settings.offset_y, -1024, 1024, "%.0f px");
                hint("Offsets are added after the box position. Positive values move right or down.");
            }
            if (ImGui::CollapsingHeader("Custom crosshair location")) {
                bool center = settings.reference_x < 0 && settings.reference_y < 0;
                if (ImGui::Checkbox("Use the image center", &center)) {
                    settings.reference_x = center ? -1 : stats.width > 0 ? stats.width * .5f : 160;
                    settings.reference_y = center ? -1 : stats.height > 0 ? stats.height * .5f : 160;
                    changed = true;
                }
                if (!center) {
                    changed |= number_input("Crosshair X", &settings.reference_x);
                    changed |= number_input("Crosshair Y", &settings.reference_y);
                    hint("Coordinates use the received image. -1 follows the center on that axis.");
                }
            }
            section("Movement prediction", "Aim ahead of a moving target using its recent positions.");
            changed |= ImGui::Checkbox("Predict target movement", &t.prediction);
            if (t.prediction) {
                changed |= combo("Prediction method", &t.prediction_method,
                                 "Kalman filter\0Smoothed velocity\0Recent velocity average\0");
                changed |= slider_float("Look-ahead time", &t.lead_ms, 0, 300, "%.0f ms");
                changed |= slider_float("Maximum lead distance", &t.max_lead, 0, 500, "%.0f px");
                if (ImGui::CollapsingHeader("Advanced prediction tuning")) {
                    changed |= slider_float("Lead multiplier", &t.lead_multiplier, 0, 5, "%.2fx");
                    if (t.prediction_method == 1)
                        changed |=
                            slider_float("Velocity smoothing", &t.velocity_smoothing_ms, 1, 200, "%.0f ms");
                }
            }
            hint("Prediction only applies to visible detections. Too much lead can overshoot.");
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::movement) {

            auto& h = settings.humanization;
            auto& d = settings.direction;
            const bool columns = setting_columns("Movement groups");
            section("Sensitivity", "How strongly the mouse corrects the distance to the aim point.");
            changed |= slider_float("Horizontal sensitivity", &settings.gain_x, 0, 2, "%.2f");
            changed |= slider_float("Vertical sensitivity", &settings.gain_y, 0, 2, "%.2f");
            section("Smoothing", "Reduce sudden changes between corrections.");
            int smoothing = h.ema_enabled ? 2 : settings.smoothing_ms > 0 ? 1 : 0;
            if (combo("Smoothing method", &smoothing,
                      "None\0Time-based\0Exponential moving average (EMA)\0")) {
                h.ema_enabled = smoothing == 2;
                if (smoothing == 0)
                    settings.smoothing_ms = 0;
                if (smoothing == 1 && settings.smoothing_ms == 0)
                    settings.smoothing_ms = 20;
                changed = true;
            }
            if (smoothing == 1)
                changed |= slider_float("Smoothing time", &settings.smoothing_ms, 1, 100, "%.0f ms");
            if (smoothing == 2)
                changed |= slider_float("EMA response", &h.ema_alpha, .01f, 1, "%.2f");
            hint("Longer smoothing times or lower EMA response make movement smoother and slower to react.");
            section("Correction limits", "Limit individual movements and ignore tiny remaining errors.");
            changed |= slider_int("Maximum correction per update", &settings.max_step, 1, 127, "%d counts");
            changed |= slider_float("Dead zone around aim point", &settings.deadzone, 0, 20, "%.1f px");
            hint("Counts are mouse movement units. Pixels refer to the received image.");
            if (ImGui::Button("Reset movement settings")) {
                h = {};
                d = {};
                settings.gain_x = settings.gain_y = .2f;
                settings.smoothing_ms = 0;
                settings.deadzone = 1;
                settings.max_step = 32;
                changed = true;
            }
            hint("Resets this page only. Targeting and activation are kept.");
            if (columns)
                ImGui::TableNextColumn();
            section("Movement path", "Shape each correction as it approaches the target.");
            changed |= ImGui::Checkbox("Customize movement path", &h.enabled);
            ImGui::BeginDisabled(!h.enabled);
            changed |= combo("Path style", &h.path,
                             "Straight\0Curved (Bezier)\0Gradual acceleration\0Slower near the "
                             "target\0Smooth random variation\0");
            if (h.path == 1)
                changed |= percent_slider("Curve strength", h.curve);
            if (h.path == 1 || h.path == 2)
                changed |= slider_float("Ramp-up duration", &h.duration_ms, 10, 1000, "%.0f ms");
            if (h.path == 2)
                changed |= slider_float("Acceleration exponent", &h.exponent, 1, 5);
            if (h.path == 3)
                changed |= slider_float("Slow-down distance", &h.adaptive_distance, 1, 500, "%.0f px");
            if (h.path == 4) {
                changed |= slider_float("Smooth variation strength", &h.noise, 0, 20, "%.1f counts");
                changed |= slider_float("Smooth variation interval", &h.noise_period_ms, 10, 1000, "%.0f ms");
            }
            if (ImGui::CollapsingHeader("Random jitter")) {
                changed |= slider_float("Jitter amount", &h.jitter, 0, 20, "%.1f counts");
                changed |= slider_float("Jitter interval", &h.jitter_interval_ms, 8, 250, "%.0f ms");
                hint("0 removes jitter. Extra variation can reduce accuracy.");
            }
            ImGui::EndDisabled();
            section("Directional assistance",
                    "Change correction strength based on your physical mouse movement.");
            changed |= ImGui::Checkbox("Adapt to my mouse direction", &d.enabled);
            if (d.enabled) {
                changed |= combo("Assistance behavior", &d.mode,
                                 "Stronger when moving away\0Stronger when moving toward\0Different strength "
                                 "for left and right\0");
                changed |= percent_slider("Direction effect", d.strength);
                if (d.mode == 2) {
                    float left = d.left_strength * 100, right = d.right_strength * 100;
                    if (slider_float("Left-side strength", &left, 0, 200, "%.0f%%")) {
                        d.left_strength = left / 100;
                        changed = true;
                    }
                    if (slider_float("Right-side strength", &right, 0, 200, "%.0f%%")) {
                        d.right_strength = right / 100;
                        changed = true;
                    }
                }
                float slow = d.slow_strength * 100;
                if (slider_float("Strength at rest", &slow, 0, 200, "%.0f%%")) {
                    d.slow_strength = slow / 100;
                    changed = true;
                }
                changed |= slider_float("Low-speed threshold", &d.slow_speed, 1, 500, "%.0f counts/s");
                if (ImGui::CollapsingHeader("Advanced direction filtering"))
                    changed |= slider_float("Direction smoothing", &d.window_ms, 5, 100, "%.0f ms");
                if (!stats.motion_available)
                    hint("Waiting for physical mouse readings. Pi output requires motion telemetry.");
                else
                    ImGui::Text("Physical mouse speed: %.0f counts/s", stats.mouse_speed);
            }
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::auto_click) {

            const bool columns = setting_columns("Auto click groups");
            section("Automatic clicking", "Uses the selected target and crosshair from Targeting.");
            ImGui::BeginDisabled(settings.mouse_backend != 0);
            changed |= ImGui::Checkbox("Enable automatic clicks", &settings.trigger.enabled);
            ImGui::BeginDisabled(!settings.trigger.enabled);
            int repeat = settings.trigger.spray ? 1 : 0;
            if (combo("Click behavior", &repeat, "Once per target\0Repeat while target stays centered\0")) {
                settings.trigger.spray = repeat == 1;
                changed = true;
            }
            int button = settings.trigger.button - 1;
            if (combo("Mouse button to click", &button, mouse_buttons, 8)) {
                settings.trigger.button = button + 1;
                changed = true;
            }
            if (columns)
                ImGui::TableNextColumn();
            section("Click timing", "All times are measured in milliseconds.");
            changed |= slider_int("Wait before first click", &settings.trigger.delay_ms, 0, 1000, "%d ms");
            if (settings.trigger.spray)
                changed |=
                    slider_int("Time between clicks", &settings.trigger.interval_ms, 10, 1000, "%d ms");
            changed |= slider_int("Button hold duration", &settings.trigger.press_ms, 1, 1000, "%d ms");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            section("Requirements and status",
                    "Assistance must be armed and active. Changing click settings disarms it.");
            if (settings.mouse_backend != 0)
                hint("Automatic clicks require Raspberry Pi output. Windows mouse output supports movement "
                     "only.");
            else
                hint("Requires current Pi telemetry with scheduled-click support. A physical hold on the "
                     "click button prevents another automatic click.");
            ImGui::TextWrapped("Status: %s", !settings.trigger.enabled      ? "Off"
                                             : stats.trigger_status.empty() ? "Waiting for activation"
                                                                            : stats.trigger_status.c_str());
            ImGui::Text("Clicks submitted: %llu", static_cast<unsigned long long>(stats.auto_triggered));
            if (ImGui::Button("Configure activation"))
                go_to(Page::activation);
            ImGui::SameLine();
            if (ImGui::Button("Configure target selection"))
                go_to(Page::targeting);
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::preview) {

            const bool columns = ImGui::BeginTable("Preview layout", 2, ImGuiTableFlags_SizingStretchProp);
            if (columns) {
                ImGui::TableSetupColumn("Window controls", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                ImGui::TableSetupColumn("Overlay controls", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableNextColumn();
            }
            section("Preview window", "View the video and overlays in a separate, resizable window.");
            changed |= ImGui::Checkbox("Enable preview window", &settings.preview);
            hint("The preview stays open while you browse settings. Move it to another monitor or "
                 "resize it as needed. Closing it turns this option off.");
            if (settings.preview && separate_preview.handle() && ImGui::Button("Bring preview to front")) {
                ShowWindow(separate_preview.handle(), SW_RESTORE);
                SetForegroundWindow(separate_preview.handle());
            }
            if (!receiving)
                hint(stats.running ? "Waiting for fresh video. The preview may show the last received frame."
                                   : "Start the session to receive video.");
            if (simulate)
                hint("Practice shows a fixed example detection; it does not run the model.");
            if (stats.inferred)
                ImGui::Text("%d x %d | %llu frames processed", stats.width, stats.height,
                            static_cast<unsigned long long>(stats.inferred));
            if (columns)
                ImGui::TableNextColumn();
            auto& v = settings.visual;
            section("Overlay content", "These controls change the preview drawing, not target selection.");
            changed |= ImGui::Checkbox("Detection boxes", &v.boxes);
            changed |= ImGui::Checkbox("Search-area circle", &v.fov);
            changed |= ImGui::Checkbox("Confidence labels", &v.confidence);
            changed |= ImGui::Checkbox("Lines to detected targets", &v.tracers);
            if (v.tracers)
                changed |= combo("Line starting point", &v.tracer_origin,
                                 "Bottom of image\0Image center\0Top of image\0");
            section("Overlay appearance", "Colors and sizes are saved with your settings.");
            changed |= slider_float("Overlay opacity", &v.opacity, .1f, 1, "%.2f");
            changed |= slider_float("Line thickness", &v.thickness, 1, 6, "%.1f px");
            changed |= slider_float("Box corner radius", &v.corner_radius, 0, 30, "%.0f px");
            changed |= slider_float("Label text size", &v.font_size, 10, 32, "%.0f px");
            changed |= ImGui::ColorEdit3("Target color", v.box_color.data(), ImGuiColorEditFlags_NoInputs);
            changed |=
                ImGui::ColorEdit3("Search-area color", v.fov_color.data(), ImGuiColorEditFlags_NoInputs);
            if (ImGui::Button("Change search radius"))
                go_to(Page::targeting);
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::capture) {

            const bool columns = setting_columns("Capture groups");
            section("Image capture", "Save the video images Receiver actually processes.");
            changed |= ImGui::Checkbox("Save received images", &settings.collection.enabled);
            changed |= input_string("Output folder", settings.collection.directory);
            float interval = settings.collection.interval_ms / 1000.f;
            if (slider_float("Time between saved images", &interval, .1f, 10, "%.1f s")) {
                settings.collection.interval_ms = int(std::round(interval * 1000));
                changed = true;
            }
            hint("Capture runs during a session, including while assistance is disarmed. Short intervals can "
                 "reduce processing speed.");
            if (columns)
                ImGui::TableNextColumn();
            section("Training labels", "Optionally save the model's boxes and class IDs beside each image.");
            changed |= ImGui::Checkbox("Save YOLO labels with each image", &settings.collection.auto_label);
            hint("Images go in images/ as lossless PPM files. Labels go in labels/ as normalized YOLO text "
                 "files. Predictions need review before training.");
            section("Capture status", "Save images from the selected video source at the collection interval.");
            ImGui::Text("Capture: %s", !settings.collection.enabled ? "Off"
                                       : stats.running              ? "Saving received images"
                                                                    : "Waiting for a session");
            ImGui::Text("Images saved: %llu", static_cast<unsigned long long>(stats.collected_frames));
            if (!stats.collection_error.empty())
                ImGui::TextWrapped("Capture error: %s", stats.collection_error.c_str());
            if (ImGui::Button("View incoming video"))
                go_to(Page::preview);
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::profiles) {

            section("Save and restore profiles",
                    "Save your connections, detection settings, and mouse preferences together. Mouse "
                    "control is never turned on by loading a file.");
            ImGui::BeginDisabled(busy() || stats.model_loading || stats.benchmarking);
            if (ImGui::Button("Import Aimmy config...", ImVec2(190, 36))) {
                if (auto path = choose_file(window, false, 3)) {
                    try {
                        auto imported = receiver::import_aimmy(*path, settings);
                        app.arm(false);
                        settings = imported.settings;
                        import_notes = std::move(imported.notes);
                        profile.clear();
                        changed = true;
                        notice = "Aimmy settings imported. Review the import details in Profiles.";
                        error.clear();
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                }
            }
            ImGui::EndDisabled();
            if (!import_notes.empty() &&
                ImGui::CollapsingHeader("Import details", ImGuiTreeNodeFlags_DefaultOpen))
                for (const auto& note : import_notes)
                    hint(note.c_str());
            if (ImGui::Button("Save profile as...", ImVec2(190, 36))) {
                if (auto path = choose_file(window, true)) {
                    try {
                        receiver::save_settings(settings, *path);
                        profile = *path;
                        preferences.add_directory(std::filesystem::absolute(*path).parent_path(), false);
                        library_updated = 0;
                        mark_preferences();
                        notice = "Settings saved.";
                        error.clear();
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                }
            }
            ImGui::Spacing();
            hint("Your latest settings restore automatically next time. Mouse control always starts off.");
            ImGui::BeginDisabled(busy() || stats.model_loading || stats.benchmarking);
            if (ImGui::Button("Load profile...", ImVec2(190, 36)))
                if (auto path = choose_file(window, false))
                    open_profile(*path);
            input_string("Search profiles", profile_filter);
            for (const auto& item : library)
                if (!item.model && matches(item.name, profile_filter)) {
                    ImGui::PushID(item.path.string().c_str());
                    if (ImGui::Selectable(item.name.c_str(), item.path.string() == profile))
                        open_profile(item.path.string());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", item.path.string().c_str());
                    ImGui::PopID();
                }
            ImGui::EndDisabled();
            hint("A profile with different connection addresses requires stopping the session first.");
            if (!profile.empty()) {
                ImGui::Spacing();
                hint("Current settings file");
                ImGui::TextWrapped("%s", profile.c_str());
            }
        }

        if (rendered_page == Page::preferences) {

            const bool columns = setting_columns("App groups");
            section("Window and theme", "Preferences for the Receiver application window.");
            changed |= ImGui::Checkbox("Keep Receiver above other windows", &settings.ui.always_on_top);
            changed |= ImGui::Checkbox("Hide Receiver from screen capture", &settings.ui.stream_guard);
            hint("Capture exclusion applies to this window and depends on Windows and the recording "
                 "software.");
            changed |= ImGui::ColorEdit3("Interface accent color", settings.ui.accent.data(),
                                         ImGuiColorEditFlags_NoInputs);
            if (columns)
                ImGui::TableNextColumn();
            section("Keyboard shortcuts",
                    "Shortcuts apply while Receiver is focused, except the global Delete stop key.");
            ImGui::TextUnformatted("Delete  -  Disarm assistance");
            ImGui::TextUnformatted("Q / E   -  Previous / next category");
            changed |= ImGui::Checkbox("Enable next-model shortcut", &settings.ui.model_switch);
            char key_name[64]{};
            const UINT scan = MapVirtualKeyA(settings.ui.model_switch_key, MAPVK_VK_TO_VSC);
            if (!GetKeyNameTextA(LONG(scan << 16), key_name, sizeof(key_name)))
                std::snprintf(key_name, sizeof(key_name), "Key %d", settings.ui.model_switch_key);
            ImGui::Text("Next model: %s", key_name);
            if (ImGui::Button(capture_model_key ? "Press a key (Esc cancels)" : "Change model shortcut"))
                capture_model_key = !capture_model_key;
            hint("Cycles the model library and disarms assistance. Shortcuts do not run while typing in a "
                 "field.");
            if (columns)
                ImGui::EndTable();
        }

        if (rendered_page == Page::diagnostics) {

            if (ImGui::BeginTabBar("Diagnostic views")) {
                if (ImGui::BeginTabItem("Setup checks")) {

                    section("Check your setup",
                            "Run the checks below without a Pi. Starting tests stops your "
                            "current session. Automatic checks never move your real cursor.");
                    auto launch = [&](std::vector<bool> chosen) {
                        app.arm(false);
                        auto opts = test_options;
                        operation = std::async(std::launch::async, [&, chosen, opts] {
                            app.stop();
                            test_hub.start(chosen, opts);
                        });
                    };
                    ImGui::BeginDisabled(busy() || test_hub.running());
                    if (ImGui::Button("Run selected checks", ImVec2(180, 36)))
                        launch(test_selection);
                    ImGui::SameLine();
                    if (ImGui::Button("Run all checks", ImVec2(160, 36)))
                        launch(std::vector<bool>(test_selection.size(), true));
                    ImGui::EndDisabled();
                    if (test_hub.running()) {
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel tests", ImVec2(140, 36)))
                            test_hub.cancel();
                        hint("Tests are running in separate processes. You can read results while they "
                             "finish.");
                    }
                    auto entries = test_hub.entries();
                    if (ImGui::BeginTable("Automatic checks", 4,
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_ScrollY,
                                          ImVec2(0, 280))) {
                        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 40);
                        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 110);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();
                        for (size_t i = 0; i < entries.size(); ++i) {
                            auto& entry = entries[i];
                            ImGui::PushID(int(i));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            bool checked = test_selection[i];
                            if (ImGui::Checkbox("##use", &checked))
                                test_selection[i] = checked;
                            ImGui::TableNextColumn();
                            if (ImGui::Selectable(entry.name.c_str(), selected_test == int(i)))
                                selected_test = int(i);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", entry.description.c_str());
                            ImGui::TableNextColumn();
                            auto color = entry.status == "Passed"   ? ImVec4(.3f, .85f, .65f, 1)
                                         : entry.status == "Failed" ? ImVec4(1, .5f, .45f, 1)
                                                                    : ImVec4(.8f, .8f, .8f, 1);
                            ImGui::TextColored(color, "%s", entry.status.c_str());
                            ImGui::TableNextColumn();
                            ImGui::BeginDisabled(busy() || test_hub.running());
                            if (ImGui::SmallButton("Run")) {
                                auto chosen = std::vector<bool>(entries.size(), false);
                                chosen[i] = true;
                                selected_test = int(i);
                                launch(chosen);
                            }
                            ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (selected_test >= 0 && size_t(selected_test) < entries.size()) {
                        const auto& entry = entries[selected_test];
                        ImGui::Text("%s  |  %s  |  %.2f s", entry.name.c_str(), entry.status.c_str(),
                                    entry.seconds);
                        hint(entry.description.c_str());
                        ImGui::BeginChild("Test result details", ImVec2(0, 110), ImGuiChildFlags_Borders);
                        ImGui::TextWrapped("%s",
                                           entry.details.empty()
                                               ? "Select Run to see the result and its explanation here."
                                               : entry.details.c_str());
                        ImGui::EndChild();
                    }
                    if (ImGui::CollapsingHeader("Test settings and optional GPU checks")) {
                        ImGui::BeginDisabled(test_hub.running() || busy());
                        input_string("Python executable", test_options.python);
                        if (ImGui::Button("Choose Python..."))
                            if (auto file = choose_file(window, false, 2))
                                test_options.python = *file;
                        hint("The first four checks need no Python. The demo launcher fills this path "
                             "automatically. Optional checks require their model files and Python "
                             "dependencies.");
                        input_string("YOLO11n model file", test_options.model);
                        input_string("Reference image file", test_options.image);
                        input_string("CUDA NVRTC DLL", test_options.nvrtc);
                        input_string("GPU settings file", test_options.profile);
                        input_string("Raw RGB test image", test_options.raw);
                        number_input("Raw image width", &test_options.width);
                        number_input("Raw image height", &test_options.height);
                        ImGui::EndDisabled();
                    }
                    auto report = test_hub.report_path();
                    if (!report.empty()) {
                        hint("Detailed logs and a JSON report are saved here:");
                        ImGui::TextWrapped("%s", report.c_str());
                    }

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Live status and timing")) {

                    hint("Technical details for checking connections and measuring performance.");
                    ImGui::TextWrapped("%s | %s", stats.status.c_str(), stats.backend.c_str());
                    if (settings.frame_source == "local")
                        ImGui::Text("Video: local display %d | Timing: receiver clock", settings.capture_output);
                    else
                        ImGui::Text("Video: UDP | Sender timing: %s | Uncertainty: %.3f ms",
                                    stats.synchronized ? "ready" : "waiting", stats.clock_uncertainty_ms);
                    ImGui::Text("Last picture age bound: %.2f ms", stats.frame_age_ms);
                    ImGui::Text("Commands sent: %llu | Physical buttons: 0x%02x",
                                static_cast<unsigned long long>(stats.sent), stats.physical);
                    ImGui::Text("Desired persistent: 0x%02x", stats.injection.persistent_mask);
                    if (stats.pi_ready && stats.injection.has_upt3) {
                        ImGui::Text("Pi applied persistent: 0x%02x | Pi scheduled: 0x%02x | Poll: %u us",
                                    stats.injection.pi_applied_persistent_mask,
                                    stats.injection.pi_scheduled_mask, stats.injection.endpoint_poll_us);
                    } else {
                        ImGui::TextUnformatted(
                            "Pi applied/scheduled state: unavailable (fresh UPT3 required)");
                    }
                    ImGui::Text("Pending click commands: %llu",
                                static_cast<unsigned long long>(stats.injection.pending_clicks));
                    ImGui::Text(
                        "Submitted locally: %llu | Accepted by Pi: %llu | Completed by USB writer: %llu",
                        static_cast<unsigned long long>(stats.injection.click_submitted),
                        static_cast<unsigned long long>(stats.injection.click_accepted),
                        static_cast<unsigned long long>(stats.injection.click_completed));
                    ImGui::Text(
                        "Rejected before acceptance: %llu | Accepted then cancelled/failed: %llu",
                        static_cast<unsigned long long>(stats.injection.click_rejected_before_acceptance),
                        static_cast<unsigned long long>(stats.injection.click_accepted_failed));
                    ImGui::Text("Click retries: %llu | Response timeouts: %llu | QueueFull: %llu",
                                static_cast<unsigned long long>(stats.injection.click_retries),
                                static_cast<unsigned long long>(stats.injection.click_timeouts),
                                static_cast<unsigned long long>(stats.injection.click_queue_full));
                    ImGui::Text(
                        "Release retries: %llu | Superseded cancellations: %llu | Server-epoch resets: %llu",
                        static_cast<unsigned long long>(stats.injection.release_retries),
                        static_cast<unsigned long long>(stats.injection.release_superseded),
                        static_cast<unsigned long long>(stats.injection.click_epoch_resets));
                    ImGui::Text("Last release: %s | ReleaseAll: %s",
                                stats.injection.last_release_reason.c_str(),
                                stats.injection.last_release_all_state.c_str());
                    if (stats.pi_ready && stats.injection.has_upt3) {
                        ImGui::Text(
                            "Pi click totals accepted/completed: %u/%u | Active/queued sequences: %u/%u",
                            stats.injection.pi_accepted_click_total, stats.injection.pi_completed_click_total,
                            stats.injection.pi_active_sequences, stats.injection.pi_queued_sequences);
                        ImGui::Text(
                            "Pi output/pending depths: %u/%u | Physical reports received/submitted: %u/%u",
                            stats.injection.pi_output_queue_depth, stats.injection.pi_pending_synthetic_depth,
                            stats.injection.pi_physical_reports_received,
                            stats.injection.pi_physical_reports_submitted);
                        ImGui::Text("Pi superseded movement: %u | USB writer failures: %u",
                                    stats.injection.pi_superseded_synthetic,
                                    stats.injection.pi_writer_failures);
                    }
                    for (const auto& command : stats.injection.commands) {
                        ImGui::Text(
                            "Command %llu (%s, button %u): %s [accepted %u, completed %u/%u]",
                            static_cast<unsigned long long>(command.id),
                            command.operation == receiver::ClickOperation::release_all ? "ReleaseAll"
                                                                                       : "click",
                            unsigned(command.button), receiver::local_command_state_name(command.state),
                            command.accepted_clicks, command.completed_clicks, command.requested_clicks);
                    }
                    ImGui::Text("Frame pool: 28 MiB | GPU free at load: %.0f / %.0f MiB", stats.gpu_free_mib,
                                stats.gpu_total_mib);
                    ImGui::Text("Expired: %llu | Replaced: %llu | Stale: %llu",
                                static_cast<unsigned long long>(stats.network.expired),
                                static_cast<unsigned long long>(stats.replaced),
                                static_cast<unsigned long long>(stats.stale));
                    ImGui::Text("Invalid: %llu | Duplicates: %llu | Pool drops: %llu",
                                static_cast<unsigned long long>(stats.network.invalid),
                                static_cast<unsigned long long>(stats.network.duplicates),
                                static_cast<unsigned long long>(stats.network.pool_drops));
                    if (ImGui::BeginTable("Timings", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        for (auto label : {"Stage", "p50 ms", "p95 ms", "p99 ms"})
                            ImGui::TableSetupColumn(label);
                        ImGui::TableHeadersRow();
                        for (auto pair : {std::pair{"Build picture", &stats.reassembly},
                                          {"Wake movement worker", &stats.control_handoff},
                                          {"Prepare image", &stats.upload},
                                          {"Run model", &stats.inference},
                                          {"Filter results", &stats.postprocess},
                                          {"Send command", &stats.submit},
                                          {"Receive to send", &stats.receiver_total}}) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(pair.first);
                            for (double percentile : {.5, .95, .99}) {
                                ImGui::TableNextColumn();
                                ImGui::Text("%.3f", pair.second->percentile(percentile));
                            }
                        }
                        ImGui::EndTable();
                    }
                    hint("p50 is the median; p95 and p99 show slower cases. Practice skips the model, so its "
                         "timings "
                         "do not measure real detection speed.");
                    latency[graph++ % latency.size()] = float(stats.receiver_total.percentile(.95));
                    ImGui::PlotLines("p95 ms", latency.data(), int(latency.size()),
                                     int(graph % latency.size()), nullptr, 0, 50, ImVec2(0, 70));
                    if (ImGui::Button("Save performance report")) {
                        try {
                            app.export_metrics("metrics.csv");
                            notice = "Performance report saved to metrics.csv in the app folder.";
                            error.clear();
                        } catch (const std::exception& e) {
                            error = e.what();
                        }
                    }
                    hint("Timing ends when a command is sent. It does not measure when the other computer "
                         "actually "
                         "moves its cursor.");

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }

        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::PushFont(detail_font);
        ImGui::Separator();
        if (ImGui::BeginTable("Connection overview", 3, ImGuiTableFlags_SizingStretchSame)) {
            for (auto item : {std::pair{"VIDEO", receiving ? "Receiving" : "Waiting"},
                              {settings.mouse_backend == 1 ? "WINDOWS MOUSE"
                               : simulate                  ? "SIMULATOR"
                                                           : "MOUSE DEVICE",
                               stats.pi_ready ? "Connected" : "Waiting"},
                              {"ASSISTANCE", stats.active  ? "Active"
                                             : stats.armed ? "Armed"
                                                           : "Disarmed"}}) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", item.first);
                ImGui::SameLine();
                ImGui::TextUnformatted(item.second);
            }
            ImGui::EndTable();
        }
        ImGui::PopFont();
        if (changed) {
            try {
                receiver::validate(settings);
                if (stats.running)
                    app.configure(settings);
                pending_preview_close = false;
                mark_preferences();
                error.clear();
            } catch (const std::exception& e) {
                error = e.what();
                settings = stats.running ? app.settings() : previous_settings;
            }
        }
        if (settings.preview) {
            try {
                separate_preview.open(device, context, swapchain, smoke);
                if (!separate_preview.update_options(settings.ui))
                    notice = "Windows could not apply capture exclusion to the separate preview.";
            } catch (const std::exception& e) {
                separate_preview.close();
                settings.preview = false;
                pending_preview_close = true;
                error = e.what();
            }
        } else {
            if (separate_preview.handle())
                separate_preview.close();
            texture.clear();
        }
        if (!smoke && !demo && preferences_dirty && !stats.model_loading && !stats.benchmarking &&
            !app.stats().model_loading && !app.stats().benchmarking && stats.error.empty() &&
            receiver::now_ns() - preferences_changed > 750'000'000) {
            try {
                preferences.save(settings, profile, names);
                preferences_dirty = false;
            } catch (const std::exception& e) {
                error = e.what();
                preferences_changed = receiver::now_ns();
            }
        }
        ImGui::End();
        ImGui::Render();
        if (target) {
            const float clear[] = {.06f, .07f, .09f, 1};
            context->OMSetRenderTargets(1, &target, nullptr);
            context->ClearRenderTargetView(target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (separate_preview.handle()) {
                try {
                    const auto picture = app.preview();
                    if (picture.frame)
                        texture.update(*picture.frame);
                    separate_preview.render({texture.view, texture.w, texture.h}, picture, settings, stats,
                                            names, simulate);
                } catch (const std::exception& e) {
                    separate_preview.close();
                    pending_preview_close = true;
                    error = e.what();
                }
            }
            if (smoke && receiver::now_ns() - smoke_started > 3'000'000'000) {
                try {
                    save_backbuffer("gui-smoke.bmp");
                    if (separate_preview.handle())
                        save_backbuffer("gui-smoke-preview.bmp", separate_preview.swap_chain());
                    auto result = app.stats();
                    if (!result.inferred || !result.pi_ready || !result.synchronized)
                        smoke_exit = 2;
                    app.export_metrics("gui-smoke-metrics.csv");
                    std::vector<std::string> visible;
                    for (const auto* item : visible_pages)
                        visible.emplace_back(item->slug);
                    for (const auto& item : settings_pages)
                        if (item.page == page) {
                            std::ofstream state("gui-smoke-state.json");
                            state << nlohmann::json{{"page", item.slug},
                                                    {"label", item.label},
                                                    {"visible", visible},
                                                    {"armed", result.armed},
                                                    {"preferred_class", settings.class_preference.class_id},
                                                    {"preference_type", settings.class_preference.mode},
                                                    {"color_mode", settings.color.mode},
                                                    {"preview_enabled", settings.preview},
                                                    {"preview_window_open", bool(separate_preview.handle())},
                                                    {"preview_frames", separate_preview.rendered_frames()},
                                                    {"preview_openings", separate_preview.openings()},
                                                    {"preview_width", separate_preview.width()},
                                                    {"preview_height", separate_preview.height()},
                                                    {"main_context_restored",
                                                     ImGui::GetCurrentContext() == main_gui},
                                                    {"gui_error", error},
                                                    {"width", ImGui::GetIO().DisplaySize.x},
                                                    {"navigation_overflow", navigation_overflow}}
                                         .dump(2);
                            break;
                        }
                } catch (...) {
                    smoke_exit = 3;
                }
                done = true;
            }
            swapchain->Present(0, 0);
            try {
                separate_preview.present();
            } catch (const std::exception& e) {
                separate_preview.close();
                pending_preview_close = true;
                error = e.what();
            }
        }
        auto remaining = 16'666'667 - (receiver::now_ns() - ui_start);
        if (remaining > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
    }
    if (operation.valid())
        try {
            operation.get();
        } catch (...) {
        }
    if (!smoke && !demo) {
        try {
            if (app.stats().running)
                settings = app.settings();
            preferences.save(settings, profile, names);
        } catch (...) {
        }
    }
    app.stop();
    separate_preview.close();
    texture.clear();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_target();
    swapchain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, instance);
    return smoke_exit;
}
