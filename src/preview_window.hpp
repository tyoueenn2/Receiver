#pragma once
#include "receiver/app.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>

namespace receiver::gui {
struct PreviewImage {
    ID3D11ShaderResourceView* view = nullptr;
    int width = 0, height = 0;
};
// UI-thread owner. Borrows the app's D3D device and image texture; owns its window,
// swap chain, render target and isolated ImGui renderer context.
class PreviewWindow {
    HWND window_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* device_context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* target_ = nullptr;
    ImGuiContext* gui_ = nullptr;
    bool renderer_ready_ = false, close_requested_ = false;
    bool drawn_ = false;
    int last_topmost_ = -1, last_guard_ = -1;
    int width_ = 0, height_ = 0;
    RECT placement_{};
    bool have_placement_ = false;
    uint64_t rendered_frames_ = 0, openings_ = 0;
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l);
    void release_target();

  public:
    PreviewWindow() = default;
    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;
    ~PreviewWindow() {
        close();
    }
    void open(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* main_chain, bool hidden);
    void close();
    bool update_options(const InterfaceSettings& settings);
    void render(const PreviewImage& image, const Preview& picture, const Settings& settings,
                const Stats& stats, const std::vector<std::string>& names, bool simulated);
    void present();
    bool close_requested() const {
        return close_requested_;
    }
    HWND handle() const {
        return window_;
    }
    IDXGISwapChain* swap_chain() const {
        return swap_chain_;
    }
    uint64_t rendered_frames() const {
        return rendered_frames_;
    }
    uint64_t openings() const {
        return openings_;
    }
    int width() const {
        return width_;
    }
    int height() const {
        return height_;
    }
};
} // namespace receiver::gui
