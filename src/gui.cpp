#include "receiver/test_hub.hpp"
#include "receiver/app.hpp"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cstdio>
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <future>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <sstream>
#include <filesystem>
#include <nlohmann/json.hpp>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapchain = nullptr;
ID3D11RenderTargetView* target = nullptr;
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
    if (ImGui::InputText(label, b.data(), b.size())) {
        value = b.data();
        return true;
    }
    return false;
}
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.62f, .69f, .78f, 1));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
void section(const char* title, const char* description) {
    ImGui::Spacing();
    ImGui::TextUnformatted(title);
    hint(description);
    ImGui::Spacing();
}
bool percent_slider(const char* label, float& value) {
    float percent = value * 100;
    if (!ImGui::SliderFloat(label, &percent, 0, 100, "%.0f%%"))
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
    dialog.lpstrFilter = model == 2 ? "Programs (*.exe)\0*.exe\0\0"
                         : model    ? "Detection models (*.onnx)\0*.onnx\0\0"
                                    : "Saved settings (*.json)\0*.json\0\0";
    dialog.lpstrDefExt = model ? "onnx" : "json";
    dialog.Flags =
        OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog))
        return std::string(path.data());
    return std::nullopt;
}
std::vector<std::string> class_names(const receiver::Settings& settings) {
    try {
        std::ifstream input(settings.metadata.empty() ? settings.model + ".json" : settings.metadata);
        auto manifest = nlohmann::json::parse(input);
        return manifest.at("names").get<std::vector<std::string>>();
    } catch (...) {
        return {};
    }
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
void save_backbuffer(const char* path) {
    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))))
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
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, window_proc,          0,      0, instance, nullptr, nullptr,
                   nullptr,    nullptr,    L"UdpVisionReceiver", nullptr};
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"UDP Vision Receiver", WS_OVERLAPPEDWINDOW, 80, 60, 1280,
                                900, nullptr, nullptr, instance, nullptr);
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
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 6;
    style.FrameRounding = 4;
    style.ItemSpacing = ImVec2(10, 12);
    style.WindowPadding = ImVec2(18, 16);
    style.FramePadding = ImVec2(10, 7);
    style.ChildRounding = 10;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.055f, .07f, .10f, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(.075f, .095f, .13f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.13f, .17f, .23f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(.10f, .32f, .36f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.14f, .43f, .47f, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(.30f, .85f, .72f, 1);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(.30f, .75f, .69f, 1);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(.12f, .34f, .37f, 1);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    receiver::App app;
    receiver::TestHub test_hub;
    receiver::TestOptions test_options;
    test_options.python = receiver::TestHub::find_python();
    std::vector<bool> test_selection(12, false);
    for (int i = 0; i < 7; ++i)
        test_selection[i] = true;
    int selected_test = 0;
    bool show_hub = false;
    receiver::Settings settings;
    bool simulate = false, done = false;
#ifndef RECEIVER_HAS_TENSORRT
    simulate = true;
#endif
    std::vector<std::string> names;
    uint64_t previous_frames = 0;
    int64_t last_frame = 0;
    std::string notice;
    int page = -1;
    bool custom_size = false;
    if (options.find("--page=detect") != std::string::npos)
        page = 1;
    if (options.find("--page=mouse") != std::string::npos)
        page = 2;
    if (options.find("--page=human") != std::string::npos)
        page = 4;
    if (options.find("--page=saved") != std::string::npos)
        page = 3;
    if (options.find("--page=hub") != std::string::npos)
        page = 5;
    if (smoke) {
        auto port = [&](const std::string& key, int current) {
            auto pos = options.find(key);
            return pos == std::string::npos ? current : std::stoi(options.substr(pos + key.size()));
        };
        settings.frame_port = port("--test-frame-port=", settings.frame_port);
        settings.pi_port = port("--test-pi-port=", settings.pi_port);
    }
    std::string profile, error, class_text;
    std::future<void> operation;
    Texture texture;
    std::array<float, 180> latency{};
    size_t graph = 0;
    auto smoke_started = receiver::now_ns();
    int smoke_exit = 0;
    if (smoke || demo) {
        simulate = true;
        names = {"Example object"};
        settings.preview = true;
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
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Receiver", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        bool changed = false;
        bool drawing_hub = false;
        if (stats.inferred != previous_frames) {
            last_frame = receiver::now_ns();
            previous_frames = stats.inferred;
        }
        const bool receiving =
            stats.running && stats.inferred && receiver::now_ns() - last_frame < 500'000'000;
        ImGui::Text("VISION RECEIVER");
        ImGui::SameLine();
        ImGui::TextDisabled("  /  %s", simulate ? "Practice mode" : "Two-computer setup");
        const char* next_step =
            !stats.error.empty() ? "Something needs attention. Stop, check the details below, then try again."
            : busy()             ? "Getting ready. Loading a new model can take a few minutes."
            : !stats.running ? "Choose your setup below, then press Start. Mouse control starts turned off."
            : !receiving     ? "Waiting for pictures. Start the picture sender on the sending computer."
            : !stats.synchronized ? "Pictures are arriving. Waiting for the sender's timing check."
            : !stats.pi_ready && settings.mouse_backend == 1 ? "Waiting for the local Windows mouse reader."
            : !stats.pi_ready ? "Waiting for the mouse device. Check the Pi address and install its receiver "
                                "support update."
            : settings.direction.enabled && !stats.motion_available && settings.mouse_backend == 1
                ? "Waiting for local physical mouse readings."
            : settings.direction.enabled && !stats.motion_available
                ? "Waiting for physical mouse movement data. Install the Pi motion update or turn "
                  "direction-based help off."
            : !stats.armed  ? "Ready to preview. Turn on mouse control only when you want to test movement."
            : !stats.active ? "Mouse control is enabled. Hold your chosen mouse button to activate it."
            : settings.mouse_backend == 1
                ? "Local cursor control is active. Release your activation button to stop."
            : simulate
                ? "Practice is active. Commands go to the simulator; your real mouse is unaffected."
                : "Mouse control is enabled. Movement requires a fresh picture and a matching detection.";
        hint(next_step);
        ImGui::Spacing();
        ImGui::BeginDisabled(busy() || test_hub.running());
        if (!stats.running) {
            if (ImGui::Button(simulate ? "Start practice" : "Start", ImVec2(145, 36))) {
                auto cfg = settings;
                const bool use_simulation = simulate;
                if (use_simulation)
                    cfg.sender_ip = cfg.pi_ip = "127.0.0.1";
                names = use_simulation ? std::vector<std::string>{"Example object"} : class_names(settings);
                last_frame = 0;
                previous_frames = 0;
                operation = std::async(std::launch::async,
                                       [&, cfg, use_simulation] { app.start(cfg, use_simulation); });
            }
        } else {
            if (ImGui::Button("Stop session", ImVec2(145, 36))) {
                app.arm(false);
                operation = std::async(std::launch::async, [&] { app.stop(); });
            }
            ImGui::SameLine();
            bool armed = stats.armed;
            if (ImGui::Checkbox(settings.mouse_backend == 1 ? "Enable local cursor movement"
                                : simulate                  ? "Enable practice movement"
                                                            : "Enable mouse control",
                                &armed))
                app.arm(armed);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.42f, .16f, .20f, 1));
        if (ImGui::Button("Turn movement off  [Delete]", ImVec2(240, 36)))
            app.arm(false);
        ImGui::PopStyleColor();
        if (!stats.error.empty() || !error.empty()) {
            ImGui::TextColored(ImVec4(1, .65f, .5f, 1), "Unable to finish that step");
            ImGui::TextWrapped("%s", error.empty() ? stats.error.c_str() : error.c_str());
        }
        if (!notice.empty())
            ImGui::TextWrapped("%s", notice.c_str());
        ImGui::Spacing();
        if (ImGui::BeginTable("Connection overview", 3, ImGuiTableFlags_SizingStretchSame)) {
            for (auto item : {std::pair{"PICTURES", receiving ? "Arriving" : "Waiting"},
                              {settings.mouse_backend == 1 ? "LOCAL WINDOWS MOUSE"
                               : simulate                  ? "SIMULATED DEVICE"
                                                           : "MOUSE DEVICE",
                               stats.pi_ready ? "Connected" : "Waiting"},
                              {"MOUSE CONTROL", stats.active  ? "Enabled - button held"
                                                : stats.armed ? "Enabled - hold to activate"
                                                              : "Off"}}) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", item.first);
                ImGui::TextUnformatted(item.second);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const float controls_width = show_hub
                                         ? ImGui::GetContentRegionAvail().x
                                         : std::clamp(ImGui::GetContentRegionAvail().x * .45f, 445.f, 550.f);
        ImGui::BeginChild("Controls", ImVec2(controls_width, 0), ImGuiChildFlags_Borders);
        ImGui::PushItemWidth(210);
        if (ImGui::BeginTabBar("Settings pages")) {
            if (ImGui::BeginTabItem("Setup", nullptr, page == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                section("1. Choose how to use Receiver",
                        "Try the controls first, or connect your two computers.");
                ImGui::BeginDisabled(stats.running || busy());
                if (ImGui::RadioButton("Practice on this computer", simulate)) {
                    simulate = true;
                    settings.sender_ip = settings.pi_ip = "127.0.0.1";
                }
#ifndef RECEIVER_HAS_TENSORRT
                hint("This test build supports practice mode. Real detection needs the GPU build described "
                     "in the setup guide.");
                ImGui::BeginDisabled();
#endif
                if (ImGui::RadioButton("Connect my computers and Raspberry Pi", !simulate))
                    simulate = false;
#ifndef RECEIVER_HAS_TENSORRT
                ImGui::EndDisabled();
#endif
                if (simulate) {
                    section("2. Prepare the practice pictures",
                            "The demo launcher starts a picture sender and simulated mouse device for you. "
                            "No Raspberry Pi or model is needed.");
                    hint("If you opened this app directly, double-click Start Demo in the app folder. "
                         "Practice uses a fixed example detection.");
                    // Profiles may contain real addresses. Practice always uses local peers.
                    settings.sender_ip = settings.pi_ip = "127.0.0.1";
                } else {
                    section("2. Connect your devices", "Enter the local network addresses of the computer "
                                                       "sending pictures and your Raspberry Pi.");
                    input_string("Sending computer address", settings.sender_ip);
                    input_string("Raspberry Pi address", settings.pi_ip);
                    hint("An address looks like 192.168.1.20. Both devices must be on your local network. "
                         "The picture sender is a separate program.");
                    section("3. Choose a detection model",
                            "A model tells the app what objects to look for. Choose an exported .onnx file.");
                    input_string("Model file", settings.model);
                    if (ImGui::Button("Browse for model...")) {
                        if (auto path = choose_file(window, false, true)) {
                            settings.model = *path;
                            settings.metadata.clear();
                            names = class_names(settings);
                        }
                    }
                    int size = custom_size                  ? 2
                               : settings.input_size == 160 ? 0
                               : settings.input_size == 320 ? 1
                                                            : 2;
                    if (ImGui::Combo("Processing detail", &size,
                                     "160 x 160 - smaller\0"
                                     "320 x 320 - standard\0"
                                     "Custom size\0")) {
                        custom_size = size == 2;
                        settings.input_size = size == 0 ? 160 : size == 1 ? 320 : settings.input_size;
                    }
                    if (size == 2)
                        ImGui::InputInt("Processing size", &settings.input_size, 32, 160);
                    hint("Smaller images can be faster but may miss small objects. This resizes incoming "
                         "pictures; it does not change the sender's capture area.");
                }
                if (ImGui::CollapsingHeader("Advanced connection settings")) {
                    input_string("Local listen address", settings.bind_ip);
                    ImGui::InputInt("Picture port", &settings.frame_port);
                    ImGui::InputInt("Mouse device port", &settings.pi_port);
                    if (!simulate) {
                        input_string("Model metadata file", settings.metadata);
                        if (ImGui::Button("Read object names"))
                            names = class_names(settings);
                    }
                    hint("Keep the defaults unless your sender or Pi uses different ports. Model metadata is "
                         "found automatically beside the model when this field is empty.");
                }
                ImGui::EndDisabled();
                if (stats.running || busy())
                    hint("Stop the session to change connections or models.");
                section("Next: Start and check the preview",
                        "Press Start at the top. You can view detections with mouse control turned off.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Detection", nullptr, page == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                section("What should the app select?",
                        "These settings choose an object from each incoming picture.");
                changed |= percent_slider("Minimum certainty", settings.confidence);
                hint("Higher values hide uncertain matches. Start at 45% and raise it if you see unwanted "
                     "detections.");
                std::string selected_class =
                    settings.classes.empty() ? "All object types" : "Custom selection";
                if (settings.classes.size() == 1 && size_t(settings.classes[0]) < names.size())
                    selected_class = names[settings.classes[0]];
                if (ImGui::BeginCombo("Object type", selected_class.c_str())) {
                    if (ImGui::Selectable("All object types", settings.classes.empty())) {
                        settings.classes.clear();
                        class_text.clear();
                        changed = true;
                    }
                    for (size_t i = 0; i < names.size(); ++i) {
                        ImGui::PushID(int(i));
                        if (ImGui::Selectable(names[i].c_str(),
                                              settings.classes == std::vector<int>{int(i)})) {
                            settings.classes = {int(i)};
                            class_text = std::to_string(i);
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (names.empty())
                    hint("Object names appear when the model's companion metadata file is available. "
                         "Advanced settings also accept numeric object IDs.");
                changed |= ImGui::SliderFloat("Search radius", &settings.fov_radius, 0, 1024, "%.0f pixels");
                hint("Only aim points inside the circle in the preview are eligible.");
                int selection = settings.highest_confidence ? 1 : 0;
                if (ImGui::Combo("Choose an object", &selection,
                                 "Nearest to the center\0Most certain match\0")) {
                    settings.highest_confidence = selection == 1;
                    changed = true;
                }
                changed |= ImGui::Checkbox("Keep the same object when possible", &settings.persistence);
                hint("Reduces switching between objects. Movement stops if the selected object disappears.");
                section("Where should movement point?", "Choose a position within the detected box.");
                if (ImGui::Button("Center of box")) {
                    settings.aim_x = settings.aim_y = .5f;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Upper center")) {
                    settings.aim_x = .5f;
                    settings.aim_y = .25f;
                    changed = true;
                }
                changed |= percent_slider("Left to right", settings.aim_x);
                changed |= percent_slider("Top to bottom", settings.aim_y);
                if (ImGui::CollapsingHeader("Advanced detection settings")) {
                    changed |= ImGui::SliderFloat("Duplicate box overlap", &settings.nms_iou, 0, 1);
                    hint("Overlapping detections of the same type are combined. Usually leave this at 0.45.");
                    changed |= ImGui::SliderFloat("Same-object overlap", &settings.persistence_iou, .01f, 1);
                    changed |= ImGui::InputFloat("Center X (-1 = automatic)", &settings.reference_x);
                    changed |= ImGui::InputFloat("Center Y (-1 = automatic)", &settings.reference_y);
                    changed |= ImGui::InputFloat("Extra horizontal offset", &settings.offset_x);
                    changed |= ImGui::InputFloat("Extra vertical offset", &settings.offset_y);
                    hint("Centers and offsets use pixels in the received picture. Positive offsets go right "
                         "and down.");
                    if (input_string("Object IDs", class_text)) {
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
                            changed = true;
                        } catch (...) {
                            error = "Use object numbers separated by commas, or leave the field empty for "
                                    "all types.";
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mouse", nullptr, page == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginDisabled(stats.running || busy());
                if (ImGui::Combo("Mouse connection", &settings.mouse_backend,
                                 "Raspberry Pi / simulator\0Local Windows mouse\0")) {
                    settings.activation_button = 2;
                    settings.secondary_button = 0;
                    settings.tracking.fov_button = 1;
                    changed = true;
                }
                ImGui::EndDisabled();
                hint("Stop the session to change connections. Local Windows mouse reads your mouse and moves "
                     "your real cursor when enabled. Press the activation button after starting.");
                section("When should movement happen?",
                        "Turn on mouse control at the top, then hold this button on the mouse connected "
                        "through your selected connection.");
                int button = settings.activation_button - 1;
                if (ImGui::Combo("Hold to activate", &button, mouse_buttons,
                                 settings.mouse_backend == 1 ? 5 : 8)) {
                    settings.activation_button = button + 1;
                    changed = true;
                }
                const char* secondary[] = {
                    "None",          "Left mouse button", "Right mouse button", "Middle mouse button",
                    "Side button 1", "Side button 2",     "Button 6",           "Button 7",
                    "Button 8"};
                changed |= ImGui::Combo("Alternate activation", &settings.secondary_button, secondary,
                                        settings.mouse_backend == 1 ? 6 : 9);
                if (simulate && settings.mouse_backend == 0)
                    hint("The practice device pretends the right button is held. Your real mouse buttons do "
                         "not control the simulator.");
                hint(
                    "Sensitivity, smoothing, movement styles, and direction-based help are in Humanization.");
                if (ImGui::CollapsingHeader("Advanced movement limits")) {
                    changed |= ImGui::SliderInt("Largest movement per update", &settings.max_step, 1, 127);
                    hint("Measured in mouse movement units, not screen pixels. The device's own limit also "
                         "applies.");
                    changed |=
                        ImGui::SliderInt("Oldest allowed picture", &settings.max_age_ms, 1, 250, "%d ms");
                    hint("Pictures older than this cannot move the mouse. The default is 50 milliseconds.");
                }
                section("Stopping is always available",
                        "Release the chosen button, press Delete, or use Turn movement off at the top. Lost "
                        "connections and old pictures also stop new movement.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Humanization", nullptr, page == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                auto& d = settings.direction;
                auto& h = settings.humanization;
                section("Make assistance fit your movement",
                        "Choose how the app responds to your hand movement, then adjust how its corrections "
                        "feel.");
                changed |= ImGui::Checkbox("Adjust help to my mouse direction", &d.enabled);
                if (ImGui::CollapsingHeader("Direction-based strength", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextUnformatted("Assistance behavior");
                    ImGui::SetNextItemWidth(-1);
                    changed |= ImGui::Combo("##direction mode", &d.mode,
                                            "More help when moving away from the target\0More help when "
                                            "moving toward the target\0Different help for left and right\0");
                    changed |= percent_slider("Strength of this effect", d.strength);
                    hint("0% keeps both directions equal. 100% allows the full difference. Angles in between "
                         "change gradually.");
                    if (d.mode != 2) {
                        float toward = 1 + (d.mode == 0 ? -d.strength : d.strength);
                        ImGui::Text("Toward: %.0f%% help   |   Away: %.0f%% help", toward * 100,
                                    (2 - toward) * 100);
                    } else {
                        float left = d.left_strength * 100, right = d.right_strength * 100;
                        if (ImGui::SliderFloat("Leftward help", &left, 0, 200, "%.0f%%")) {
                            d.left_strength = left / 100;
                            changed = true;
                        }
                        if (ImGui::SliderFloat("Rightward help", &right, 0, 200, "%.0f%%")) {
                            d.right_strength = right / 100;
                            changed = true;
                        }
                        hint("Applies when your hand moves toward a target on that side. Moving away keeps "
                             "the normal strength. The effect slider blends these values with normal "
                             "strength.");
                    }
                    float slow = d.slow_strength * 100;
                    if (ImGui::SliderFloat("Help when barely moving", &slow, 0, 200, "%.0f%%")) {
                        d.slow_strength = slow / 100;
                        changed = true;
                    }
                    changed |=
                        ImGui::SliderFloat("Slow movement threshold", &d.slow_speed, 1, 500, "%.0f units/s");
                    hint("Below this speed, assistance gradually uses the barely-moving value. Lower the "
                         "threshold if small, slow movements should count as intentional direction.");
                    if (d.enabled) {
                        if (!stats.motion_available)
                            hint("Waiting for physical movement data. The Pi needs the updated "
                                 "motion-telemetry patch. Assistance pauses until this data is ready.");
                        else
                            ImGui::Text("Physical mouse speed: %.0f units/s", stats.mouse_speed);
                        if (stats.active)
                            ImGui::Text("Last calculated help: %.0f%%", stats.assist_strength * 100);
                    }
                    if (simulate && settings.mouse_backend == 0)
                        hint("Demo mouse input is steady by default. The launcher option --cycle-motion "
                             "enables direction changes. "
                             "Enable practice movement at the top to see its effect. Your real mouse is "
                             "unaffected.");
                    if (ImGui::TreeNode("Advanced direction filtering")) {
                        changed |= ImGui::SliderFloat("Direction averaging", &d.window_ms, 5, 100, "%.0f ms");
                        hint("Higher values reduce direction flicker but react more slowly to a change in "
                             "your hand movement.");
                        ImGui::TreePop();
                    }
                }
                if (ImGui::CollapsingHeader("Sensitivity and smoothing")) {
                    changed |= ImGui::SliderFloat("Horizontal sensitivity", &settings.gain_x, 0, 2, "%.2f");
                    changed |= ImGui::SliderFloat("Vertical sensitivity", &settings.gain_y, 0, 2, "%.2f");
                    changed |=
                        ImGui::SliderFloat("Movement smoothing", &settings.smoothing_ms, 0, 100, "%.0f ms");
                    hint(
                        "Higher sensitivity moves farther. Smoothing makes changes gradual; 0 turns it off.");
                    changed |= ImGui::Checkbox("Use EMA smoothing", &h.ema_enabled);
                    if (h.ema_enabled)
                        changed |= ImGui::SliderFloat("EMA response", &h.ema_alpha, .01f, 1.f, "%.2f");
                    hint("EMA replaces time-based smoothing. Lower response feels softer; higher response "
                         "follows changes faster.");
                    changed |= ImGui::SliderFloat("Ignore tiny adjustments", &settings.deadzone, 0, 20,
                                                  "%.1f pixels");
                    changed |= ImGui::Checkbox("Keep the same target when possible", &settings.persistence);
                    hint("Reduces target switching. No movement is sent toward a target that has "
                         "disappeared.");
                }
                if (ImGui::CollapsingHeader("Prediction and target tracking")) {
                    auto& t = settings.tracking;
                    changed |= ImGui::Checkbox("Lead moving targets", &t.prediction);
                    ImGui::SetNextItemWidth(-1);
                    changed |= ImGui::Combo("##prediction method", &t.prediction_method,
                                            "Kalman - filter noisy motion\0Smoothed velocity - gradual "
                                            "response\0Velocity history - average recent motion\0");
                    changed |= ImGui::SliderFloat("Prediction lead time", &t.lead_ms, 0, 300, "%.0f ms");
                    changed |= ImGui::SliderFloat("Lead multiplier", &t.lead_multiplier, 0, 5, "%.2fx");
                    changed |=
                        ImGui::SliderFloat("Maximum lead distance", &t.max_lead, 0, 500, "%.0f pixels");
                    if (t.prediction_method == 1)
                        changed |= ImGui::SliderFloat("Velocity averaging", &t.velocity_smoothing_ms, 1, 200,
                                                      "%.0f ms");
                    hint("Aimmy offers Kalman, EMA-based and velocity-history prediction. These independent "
                         "versions use the current detection and recent motion. Higher lead can overshoot. "
                         "Missing targets never receive predicted movement.");
                    changed |= ImGui::Checkbox("Prefer the current target", &settings.persistence);
                    changed |= ImGui::SliderFloat("Sticky target distance", &t.sticky_distance, 0, 500,
                                                  "%.0f pixels");
                    hint("Allows the same object to move this far between pictures without switching "
                         "identity. 0 uses box overlap only; large values may mix nearby objects of the same "
                         "type.");
                }
                if (ImGui::CollapsingHeader("Dynamic search area")) {
                    auto& t = settings.tracking;
                    changed |= ImGui::Checkbox("Change search area while holding a button", &t.dynamic_fov);
                    int button = t.fov_button - 1;
                    if (ImGui::Combo("Search-area button", &button, mouse_buttons,
                                     settings.mouse_backend == 1 ? 5 : 8)) {
                        t.fov_button = button + 1;
                        changed = true;
                    }
                    changed |= ImGui::SliderFloat("Search radius while held", &t.held_radius, 1, 1024,
                                                  "%.0f pixels");
                    hint("Similar to Aimmy's dynamic FOV. Releasing the button restores the normal search "
                         "circle. This does not activate movement on its own.");
                }

                if (ImGui::CollapsingHeader("Movement style and variation")) {
                    changed |= ImGui::Checkbox("Enable movement shaping", &h.enabled);
                    ImGui::SetNextItemWidth(-1);
                    changed |= ImGui::Combo(
                        "##movement style", &h.path,
                        "Direct - straight corrections\0Curved - gentle Bezier bend\0Ease in - build up "
                        "gradually\0Adaptive - gentler near the target\0Smooth noise - flowing variation\0");
                    if (h.path == 1)
                        changed |= percent_slider("Curve amount", h.curve);
                    if (h.path == 1 || h.path == 2) {
                        changed |= ImGui::SliderFloat("Build-up time", &h.duration_ms, 10, 1000, "%.0f ms");
                        hint("Restarts when the target changes or assistance is interrupted. No future "
                             "movements are queued.");
                    }
                    if (h.path == 2)
                        changed |= ImGui::SliderFloat("Ease-in intensity", &h.exponent, 1, 5);
                    if (h.path == 3)
                        changed |= ImGui::SliderFloat("Gentle approach distance", &h.adaptive_distance, 1,
                                                      500, "%.0f pixels");
                    if (h.path == 4) {
                        changed |=
                            ImGui::SliderFloat("Smooth variation amount", &h.noise, 0, 20, "%.1f units");
                        changed |=
                            ImGui::SliderFloat("Variation interval", &h.noise_period_ms, 10, 1000, "%.0f ms");
                    }
                    changed |= ImGui::SliderFloat("Small random variations", &h.jitter, 0, 20, "%.1f units");
                    changed |= ImGui::SliderFloat("Random change interval", &h.jitter_interval_ms, 8, 250,
                                                  "%.0f ms");
                    hint("Like Aimmy's mouse jitter, this varies individual corrections. Start at 0; extra "
                         "variation can reduce accuracy. Variation fades near the aim point and stops inside "
                         "the dead zone.");
                }
                if (ImGui::Button("Reset humanization")) {
                    settings.tracking = {};
                    h = {};
                    d = {};
                    settings.gain_x = settings.gain_y = .2f;
                    settings.smoothing_ms = 0;
                    settings.deadzone = 1;
                    changed = true;
                }
                hint("All these settings are included when you save your setup.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Test Hub", nullptr, page == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
                drawing_hub = true;
                section("Check your setup", "Run the checks below without a Pi. Starting tests stops your "
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
                    hint("Tests are running in separate processes. You can read results while they finish.");
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
                    ImGui::TextWrapped("%s", entry.details.empty()
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
                         "automatically. Optional checks require their model files and Python dependencies.");
                    input_string("YOLO11n model file", test_options.model);
                    input_string("Reference image file", test_options.image);
                    input_string("CUDA NVRTC DLL", test_options.nvrtc);
                    input_string("GPU settings file", test_options.profile);
                    input_string("Raw RGB test image", test_options.raw);
                    ImGui::InputInt("Raw image width", &test_options.width);
                    ImGui::InputInt("Raw image height", &test_options.height);
                    ImGui::EndDisabled();
                }
                auto report = test_hub.report_path();
                if (!report.empty()) {
                    hint("Detailed logs and a JSON report are saved here:");
                    ImGui::TextWrapped("%s", report.c_str());
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Saved settings", nullptr,
                                    page == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                section("Keep a setup you like",
                        "Save your connections, detection settings, and mouse preferences together. Mouse "
                        "control is never turned on by loading a file.");
                if (ImGui::Button("Save settings as...", ImVec2(190, 36))) {
                    if (auto path = choose_file(window, true)) {
                        try {
                            receiver::save_settings(settings, *path);
                            profile = *path;
                            notice = "Settings saved.";
                            error.clear();
                        } catch (const std::exception& e) {
                            error = e.what();
                        }
                    }
                }
                ImGui::Spacing();
                ImGui::BeginDisabled(stats.running || busy());
                if (ImGui::Button("Open saved settings...", ImVec2(190, 36))) {
                    if (auto path = choose_file(window, false)) {
                        try {
                            auto loaded = receiver::load_settings(*path);
                            settings = loaded;
                            custom_size = settings.input_size != 160 && settings.input_size != 320;
                            profile = *path;
                            class_text.clear();
                            for (auto id : settings.classes) {
                                if (!class_text.empty())
                                    class_text += ",";
                                class_text += std::to_string(id);
                            }
                            names = class_names(settings);
                            notice = "Settings loaded. Mouse control is off.";
                            error.clear();
                        } catch (const std::exception& e) {
                            error = e.what();
                        }
                    }
                }
                ImGui::EndDisabled();
                if (stats.running || busy())
                    hint("Stop the session before opening a different setup.");
                if (!profile.empty()) {
                    ImGui::Spacing();
                    hint("Current settings file");
                    ImGui::TextWrapped("%s", profile.c_str());
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        show_hub = drawing_hub;
        page = -1;
        ImGui::PopItemWidth();
        ImGui::EndChild();
        if (!show_hub) {
            ImGui::SameLine();
            ImGui::BeginChild("Monitor");
            ImGui::Text("PICTURE PREVIEW");
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Show preview", &settings.preview);
            hint(simulate ? "White cross = picture center. Bullseye = test target. The target stays fixed; "
                            "this is not YOLO detection."
                          : "Green boxes are detections. The circle is your search area; the dot marks the "
                            "selected aim point.");
            if (settings.preview && !receiving)
                hint(stats.running ? "No recent picture. Any image below is the last received frame."
                                   : "Preview paused. Start a session to receive pictures.");

            if (settings.preview) {
                auto p = app.preview();
                if (p.frame) {
                    texture.update(*p.frame);
                    if (texture.view) {
                        float scale =
                            std::min(2.f, std::min(ImGui::GetContentRegionAvail().x / texture.w,
                                                   std::min(380.f, ImGui::GetContentRegionAvail().y - 90.f) /
                                                       texture.h));
                        scale = std::max(.1f, scale);
                        auto origin = ImGui::GetCursorScreenPos();
                        ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(texture.view)),
                                     ImVec2(texture.w * scale, texture.h * scale));
                        auto* draw = ImGui::GetWindowDrawList();
                        for (auto& d : p.detections) {
                            draw->AddRect(
                                ImVec2(origin.x + d.x * scale, origin.y + d.y * scale),
                                ImVec2(origin.x + (d.x + d.w) * scale, origin.y + (d.y + d.h) * scale),
                                IM_COL32(80, 230, 160, 255), 0, 0, 2);
                            char label[64];
                            std::string object = simulate ? "Example"
                                                 : d.cls >= 0 && size_t(d.cls) < names.size()
                                                     ? names[d.cls]
                                                     : "Object " + std::to_string(d.cls);
                            std::snprintf(label, sizeof(label), "%s  %.0f%%", object.c_str(), d.score * 100);
                            draw->AddText(ImVec2(origin.x + d.x * scale, origin.y + d.y * scale),
                                          IM_COL32_WHITE, label);
                        }
                        float rx = settings.reference_x < 0 ? texture.w * .5f : settings.reference_x,
                              ry = settings.reference_y < 0 ? texture.h * .5f : settings.reference_y;
                        draw->AddCircle(ImVec2(origin.x + rx * scale, origin.y + ry * scale),
                                        (settings.tracking.dynamic_fov &&
                                                 (stats.physical & (1u << (settings.tracking.fov_button - 1)))
                                             ? settings.tracking.held_radius
                                             : settings.fov_radius) *
                                            scale,
                                        IM_COL32(180, 180, 255, 180));
                        if (p.correction.target)
                            draw->AddCircleFilled(ImVec2(origin.x + p.correction.aim_x * scale,
                                                         origin.y + p.correction.aim_y * scale),
                                                  4, IM_COL32(255, 100, 80, 255));
                    }
                } else
                    section("Your pictures will appear here",
                            "Start the session and its picture sender. In practice mode, the demo launcher "
                            "supplies the pictures.");
            } else {
                texture.clear();
                ImGui::BeginChild("Preview off", ImVec2(0, 250), ImGuiChildFlags_Borders);
                ImGui::Spacing();
                section(
                    "See what the app sees",
                    "Turn on Show preview to check the picture, detections, and search area. You can turn it "
                    "off later to reduce the display's workload.");
                if (ImGui::Button("Show preview", ImVec2(150, 36))) {
                    settings.preview = true;
                    changed = true;
                }
                ImGui::EndChild();
            }

            if (simulate) {
                hint("Hold your activation button to move toward the bullseye. The picture does not react to "
                     "your desktop cursor; reaching a screen edge can look like a pause.");
                ImGui::TextWrapped("Movement: %s", stats.status.c_str());
                ImGui::Text("Corrections sent: %llu", static_cast<unsigned long long>(stats.sent));
            }
            ImGui::Spacing();
            if (stats.inferred)
                ImGui::Text("Picture size: %d x %d  |  Pictures processed: %llu", stats.width, stats.height,
                            static_cast<unsigned long long>(stats.inferred));
            if (ImGui::CollapsingHeader("Performance and troubleshooting")) {
                hint("Technical details for checking connections and measuring performance.");
                ImGui::TextWrapped("%s | %s", stats.status.c_str(), stats.backend.c_str());
                ImGui::Text("Sender timing: %s | Uncertainty: %.3f ms",
                            stats.synchronized ? "ready" : "waiting", stats.clock_uncertainty_ms);
                ImGui::Text("Last picture age bound: %.2f ms", stats.frame_age_ms);
                ImGui::Text("Commands sent: %llu | Physical buttons: 0x%02x",
                            static_cast<unsigned long long>(stats.sent), stats.physical);
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
                ImGui::PlotLines("p95 ms", latency.data(), int(latency.size()), int(graph % latency.size()),
                                 nullptr, 0, 50, ImVec2(0, 70));
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
            }
            ImGui::EndChild();
        }
        if (changed) {
            try {
                receiver::validate(settings);
                if (stats.running)
                    app.configure(settings);
                error.clear();
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        ImGui::End();
        ImGui::Render();
        if (target) {
            const float clear[] = {.06f, .07f, .09f, 1};
            context->OMSetRenderTargets(1, &target, nullptr);
            context->ClearRenderTargetView(target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (smoke && receiver::now_ns() - smoke_started > 3'000'000'000) {
                try {
                    save_backbuffer("gui-smoke.bmp");
                    auto result = app.stats();
                    if (!result.inferred || !result.pi_ready || !result.synchronized)
                        smoke_exit = 2;
                    app.export_metrics("gui-smoke-metrics.csv");
                } catch (...) {
                    smoke_exit = 3;
                }
                done = true;
            }
            swapchain->Present(0, 0);
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
    app.stop();
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
