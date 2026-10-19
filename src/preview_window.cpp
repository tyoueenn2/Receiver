#include "preview_window.hpp"
#include "receiver/color.hpp"
#include <algorithm>
#include <cstdio>
#include <imgui_impl_dx11.h>
#include <stdexcept>

namespace receiver::gui {
namespace {
bool draw_preview_image(const PreviewImage& image, const Preview& picture, const Settings& settings,
                        const Stats& stats, const std::vector<std::string>& names, bool simulated,
                        ImVec2 available, float maximum_scale) {
    if (!image.view || image.width <= 0 || image.height <= 0 || available.x < 1 || available.y < 21)
        return false;
    const float scale =
        std::min(maximum_scale, std::min(available.x / image.width, (available.y - 20) / image.height));
    const float margin = (available.x - image.width * scale) * .5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, margin));
    auto origin = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(image.view)),
                 ImVec2(image.width * scale, image.height * scale));
    auto* draw = ImGui::GetWindowDrawList();
    const auto& visual = settings.visual;
    const auto color = [&](const std::array<float, 3>& rgb) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], visual.opacity));
    };
    draw->PushClipRect(origin, ImVec2(origin.x + image.width * scale, origin.y + image.height * scale), true);
    for (auto& d : picture.detections) {
        if (visual.boxes)
            draw->AddRect(ImVec2(origin.x + d.x * scale, origin.y + d.y * scale),
                          ImVec2(origin.x + (d.x + d.w) * scale, origin.y + (d.y + d.h) * scale),
                          color(visual.box_color), visual.corner_radius, 0, visual.thickness);
        char label[64];
        std::string object = simulated                                    ? "Example"
                             : d.cls >= 0 && size_t(d.cls) < names.size() ? names[d.cls]
                                                                          : "Object " + std::to_string(d.cls);
        std::snprintf(label, sizeof(label), "%s  %.0f%%", object.c_str(), d.score * 100);
        if (visual.confidence)
            draw->AddText(ImGui::GetFont(), visual.font_size,
                          ImVec2(origin.x + d.x * scale, origin.y + d.y * scale), color(visual.box_color),
                          label);
        if (visual.tracers) {
            const float from = visual.tracer_origin == 0 ? 1.f : visual.tracer_origin == 1 ? .5f : 0.f;
            draw->AddLine(
                ImVec2(origin.x + image.width * scale * .5f, origin.y + image.height * scale * from),
                ImVec2(origin.x + (d.x + d.w * .5f) * scale, origin.y + (d.y + d.h * .5f) * scale),
                color(visual.box_color), visual.thickness);
        }
    }
    float rx = settings.reference_x < 0 ? image.width * .5f : settings.reference_x,
          ry = settings.reference_y < 0 ? image.height * .5f : settings.reference_y;
    if (visual.fov)
        draw->AddCircle(
            ImVec2(origin.x + rx * scale, origin.y + ry * scale),
            (settings.tracking.dynamic_fov && (stats.physical & (1u << (settings.tracking.fov_button - 1)))
                 ? settings.tracking.held_radius
                 : settings.fov_radius) *
                scale,
            color(visual.fov_color), 0, visual.thickness);
    if (picture.correction.target)
        draw->AddCircleFilled(
            ImVec2(origin.x + picture.correction.aim_x * scale, origin.y + picture.correction.aim_y * scale),
            4, IM_COL32(255, 100, 80, 255));
    if (settings.color.mode && settings.color.preview && picture.colors) {
        // Reuse target selection so preview agrees with class preferences and fallback.
        auto view_settings = settings;
        view_settings.tracking.prediction = false;
        receiver::Controller view_controller(0);
        receiver::Telemetry limits;
        limits.physical = stats.physical;
        limits.xmin = limits.ymin = -127;
        limits.xmax = limits.ymax = 127;
        const auto aim = view_controller.update(picture.detections, image.width, image.height, view_settings,
                                                limits, receiver::now_ns(), {}, 0, picture.colors.get());
        if (aim.target) {
            ImVec2 at(origin.x + aim.aim_x * scale, origin.y + aim.aim_y * scale);
            draw->AddLine(ImVec2(at.x - 6, at.y), ImVec2(at.x + 6, at.y), IM_COL32(255, 220, 80, 255), 2);
            draw->AddLine(ImVec2(at.x, at.y - 6), ImVec2(at.x, at.y + 6), IM_COL32(255, 220, 80, 255), 2);
        }
    }
    draw->PopClipRect();
    if (settings.color.mode && picture.colors)
        ImGui::Text("HSV matches: %llu | yellow cross: selected aim / fallback",
                    static_cast<unsigned long long>(picture.colors->count));
    return true;
}
constexpr auto window_class = L"ReceiverSeparatePreview";
struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* current) {
        ImGui::SetCurrentContext(current);
    }
    ~ContextScope() {
        ImGui::SetCurrentContext(previous);
    }
};
} // namespace

LRESULT CALLBACK PreviewWindow::window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PreviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_GETMINMAXINFO) {
        reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {320, 240};
        return 0;
    }
    if (message == WM_CLOSE && self) {
        self->close_requested_ = true;
        return 0; // The UI saves the toggle and destroys this window between frames.
    }
    // Closing the preview must never post WM_QUIT or touch the main swap chain.
    return DefWindowProcW(window, message, w, l);
}
void PreviewWindow::release_target() {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
}
void PreviewWindow::open(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* main_chain,
                         bool hidden) {
    if (window_)
        return;
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = window_class;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register the preview window");
    device_ = device;
    device_context_ = context;
    close_requested_ = false;
    try {
        window_ = CreateWindowExW(WS_EX_APPWINDOW, window_class, L"Receiver Preview", WS_OVERLAPPEDWINDOW,
                                  have_placement_ ? placement_.left : CW_USEDEFAULT,
                                  have_placement_ ? placement_.top : CW_USEDEFAULT,
                                  have_placement_ ? placement_.right - placement_.left : 720,
                                  have_placement_ ? placement_.bottom - placement_.top : 600, nullptr,
                                  nullptr, instance, this);
        if (!window_)
            throw std::runtime_error("Cannot create the preview window");
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window_;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGIFactory* factory = nullptr;
        if (FAILED(main_chain->GetParent(IID_PPV_ARGS(&factory))))
            throw std::runtime_error("Cannot access the preview display factory");
        const auto result = factory->CreateSwapChain(device_, &desc, &swap_chain_);
        factory->Release();
        if (FAILED(result))
            throw std::runtime_error("Cannot create the preview display surface");
        auto* main_gui = ImGui::GetCurrentContext();
        gui_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(main_gui);
        {
            ContextScope scope(gui_);
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().LogFilename = nullptr;
            ImGui::StyleColorsDark();
            if (!ImGui_ImplDX11_Init(device_, device_context_))
                throw std::runtime_error("Cannot initialize the preview renderer");
            renderer_ready_ = true;
        }
        last_topmost_ = last_guard_ = -1;
        ShowWindow(window_, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
        ++openings_;
    } catch (...) {
        close();
        throw;
    }
}
void PreviewWindow::close() {
    if (window_ && !IsIconic(window_))
        have_placement_ = bool(GetWindowRect(window_, &placement_));
    if (device_context_)
        device_context_->OMSetRenderTargets(0, nullptr, nullptr);
    if (gui_) {
        ContextScope scope(gui_);
        if (renderer_ready_)
            ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(gui_);
        gui_ = nullptr;
    }
    renderer_ready_ = false;
    drawn_ = false;
    release_target();
    if (swap_chain_) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    UnregisterClassW(window_class, GetModuleHandleW(nullptr));
    device_ = nullptr;
    device_context_ = nullptr;
    width_ = height_ = 0;
    close_requested_ = false;
}
bool PreviewWindow::update_options(const InterfaceSettings& settings) {
    if (last_topmost_ != int(settings.always_on_top)) {
        SetWindowPos(window_, settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        last_topmost_ = int(settings.always_on_top);
    }
    if (last_guard_ != int(settings.stream_guard)) {
        last_guard_ = int(settings.stream_guard);
        return bool(SetWindowDisplayAffinity(window_, settings.stream_guard ? 0x11 : 0));
    }
    return true;
}
void PreviewWindow::render(const PreviewImage& image, const Preview& picture, const Settings& settings,
                           const Stats& stats, const std::vector<std::string>& names, bool simulated) {
    drawn_ = false;
    if (!window_ || IsIconic(window_))
        return;
    RECT client{};
    if (!GetClientRect(window_, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    if (!target_ || width_ != client.right || height_ != client.bottom) {
        release_target();
        if (FAILED(swap_chain_->ResizeBuffers(0, UINT(client.right), UINT(client.bottom), DXGI_FORMAT_UNKNOWN,
                                              0)))
            throw std::runtime_error("Cannot resize the preview display surface");
        ID3D11Texture2D* buffer = nullptr;
        if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer))))
            throw std::runtime_error("Cannot access the preview image surface");
        const auto result = device_->CreateRenderTargetView(buffer, nullptr, &target_);
        buffer->Release();
        if (FAILED(result))
            throw std::runtime_error("Cannot draw the preview image surface");
        width_ = client.right;
        height_ = client.bottom;
    }
    ContextScope scope(gui_);
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width_), float(height_));
    io.DeltaTime = 1.f / 60;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Preview image", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted(stats.status.c_str());
    if (picture.frame && draw_preview_image(image, picture, settings, stats, names, simulated,
                                            ImGui::GetContentRegionAvail(), 128))
        ++rendered_frames_;
    else
        ImGui::TextWrapped("Waiting for video. Start a session to see the preview.");
    ImGui::End();
    ImGui::Render();
    const float clear[] = {.035f, .05f, .075f, 1};
    device_context_->OMSetRenderTargets(1, &target_, nullptr);
    device_context_->ClearRenderTargetView(target_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    device_context_->OMSetRenderTargets(0, nullptr, nullptr);
    drawn_ = true;
}
void PreviewWindow::present() {
    if (!drawn_)
        return;
    drawn_ = false;
    if (FAILED(swap_chain_->Present(0, 0)))
        throw std::runtime_error("Cannot present the preview window");
}
} // namespace receiver::gui
