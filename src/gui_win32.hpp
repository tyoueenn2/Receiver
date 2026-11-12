#pragma once
#include "preview_window.hpp"
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
using NativeWindow = HWND;
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
struct Texture {
    ID3D11Texture2D* image = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    int w = 0, h = 0;
    uint64_t session = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> rgba;
    ImTextureID id() const {
        return ImTextureID(reinterpret_cast<uintptr_t>(view));
    }
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

struct GuiBackend {
    HWND window = nullptr;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    bool renderer_ready = false;
    ~GuiBackend() {
        close();
    }
    void open(bool compact, bool smoke) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        wc = WNDCLASSEXW{sizeof(wc), CS_CLASSDC, window_proc,          0,      0, instance, nullptr, nullptr,
                         nullptr,    nullptr,    L"UdpVisionReceiver", nullptr};
        RegisterClassExW(&wc);
        window =
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
            throw std::runtime_error("DirectX 11 initialization failed");
        }
        create_target();
        ShowWindow(window, smoke ? SW_HIDE : SW_SHOWDEFAULT);
        UpdateWindow(window);
    }
    void initialize_imgui() {
        ImGui_ImplWin32_Init(window);
        ImGui_ImplDX11_Init(device, context);
        renderer_ready = true;
    }
    bool poll(receiver::gui::PreviewWindow&) {
        MSG message;
        bool running = true;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                running = false;
        }
        return running;
    }
    void set_topmost(bool enabled) {
        SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    bool set_capture_exclusion(bool enabled) {
        return SetWindowDisplayAffinity(window, enabled ? 0x11 : 0);
    }
    void new_frame() {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
    }
    bool focused() const {
        return GetForegroundWindow() == window;
    }
    bool key_down(int key) const {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    std::string key_name(int key) const {
        char name[64]{};
        const UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
        if (!GetKeyNameTextA(LONG(scan << 16), name, sizeof(name)))
            std::snprintf(name, sizeof(name), "Key %d", key);
        return name;
    }
    void show_preview(receiver::gui::PreviewWindow& preview) {
        ShowWindow(preview.handle(), SW_RESTORE);
        SetForegroundWindow(preview.handle());
    }
    void open_preview(receiver::gui::PreviewWindow& preview, bool hidden) {
        preview.open(device, context, swapchain, hidden);
    }
    bool render() {
        if (!target)
            return false;
        const float clear[] = {.06f, .07f, .09f, 1};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->ClearRenderTargetView(target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return true;
    }
    void save_frame(const char* path, receiver::gui::PreviewWindow* preview = nullptr) {
        save_backbuffer(path, preview ? preview->swap_chain() : swapchain);
    }
    void present() {
        swapchain->Present(0, 0);
    }
    void close() {
        if (renderer_ready) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            renderer_ready = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        release_target();
        if (swapchain) {
            swapchain->Release();
            swapchain = nullptr;
        }
        if (context) {
            context->Release();
            context = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (window) {
            DestroyWindow(window);
            window = nullptr;
        }
        if (wc.lpszClassName)
            UnregisterClassW(wc.lpszClassName, instance);
    }
};
} // namespace
