#include "preview_window.hpp"
#include "receiver/color.hpp"
#include <algorithm>
#include <cstdio>
#include <imgui_impl_dx11.h>
#include <stdexcept>

namespace receiver::gui {
namespace {
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
