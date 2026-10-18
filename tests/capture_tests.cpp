#include "receiver/app.hpp"
#include "receiver/capture.hpp"
#include "receiver/model.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    using namespace receiver;
    try {
        Settings settings;
        auto changed = settings;
        changed.capture_output = 1;
        check(connection_settings_changed(settings, changed), "Display changes must require restart");
#ifndef _WIN32
        check(!local_capture_supported() && capture_outputs().empty(), "Linux must retain UDP-only support");
        settings.frame_source = "local";
        App app;
        try {
            app.start(settings, true);
        } catch (const std::exception& e) {
            check(std::string(e.what()).find("Windows") != std::string::npos, "Missing platform error");
            check(!app.stats().running, "Unsupported capture must not start workers");
            std::cout << "Local capture platform checks passed\n";
            return 0;
        }
        throw std::runtime_error("Linux unexpectedly started desktop capture");
#else
        std::vector<CaptureOutput> outputs;
        std::unique_ptr<LocalCapture> capture;
        try {
            outputs = capture_outputs();
            if (outputs.empty())
                throw std::runtime_error("No attached desktop display");
            capture = std::make_unique<LocalCapture>(0);
        } catch (const std::exception& e) {
            std::cout << "SKIP: Desktop Duplication unavailable: " << e.what() << '\n';
            return 77;
        }
        DEVMODEA mode{};
        mode.dmSize = sizeof(mode);
        check(EnumDisplaySettingsA(outputs[0].name.c_str(), ENUM_CURRENT_SETTINGS, &mode), "Display mode");
        const auto brush = CreateSolidBrush(RGB(17, 89, 203));
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hbrBackground = brush;
        wc.lpszClassName = "ReceiverCaptureTest";
        check(RegisterClassA(&wc) != 0, "Register capture test window");
        struct Window {
            HWND handle = nullptr;
            HBRUSH brush = nullptr;
            ~Window() {
                if (handle)
                    DestroyWindow(handle);
                UnregisterClassA("ReceiverCaptureTest", GetModuleHandle(nullptr));
                DeleteObject(brush);
            }
        } window{nullptr, brush};
        window.handle = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName,
                                        "Capture test", WS_POPUP | WS_VISIBLE,
                                        mode.dmPosition.x + int(mode.dmPelsWidth) / 2 - 128,
                                        mode.dmPosition.y + int(mode.dmPelsHeight) / 2 - 128, 256, 256,
                                        nullptr, nullptr, wc.hInstance, nullptr);
        check(window.handle != nullptr, "Create capture test window");
        auto paint = [&] {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
            InvalidateRect(window.handle, nullptr, TRUE);
            UpdateWindow(window.handle);
            // Force an actual present even when DWM suppresses identical repaints.
            // The marker is outside both center crops checked below.
            static bool marker = false;
            const auto dc = GetDC(window.handle);
            check(dc != nullptr, "Get capture test DC");
            SetPixel(dc, 0, 0, (marker = !marker) ? RGB(0, 0, 0) : RGB(255, 255, 255));
            ReleaseDC(window.handle, dc);
        };
        bool matched = false;
        const auto end = now_ns() + 3'000'000'000ll;
        while (!matched && now_ns() < end) {
            paint();
            auto frame = capture->next(32);
            if (!frame)
                continue;
            check(frame->header.format == 2 && frame->header.bytes == 32 * 32 * 4, "Raw BGRA crop shape");
            check(frame->header.capture_ns <= frame->complete_ns,
                  "Presentation time must not be in the future");
            if (frame->complete_ns - frame->header.capture_ns >= 1'000'000'000ll)
                continue; // Initial duplication can precede the test window's first present.
            matched = true;
            for (size_t p = 0; p < frame->header.bytes; p += 4)
                matched &=
                    frame->pixels[p] == 203 && frame->pixels[p + 1] == 89 && frame->pixels[p + 2] == 17;
        }
        check(matched, "Center crop pixels or BGRA row packing are incorrect");
        capture.reset();
        settings.frame_source = "local";
        settings.preview = true;
        settings.input_size = 32;
        settings.max_age_ms = 250;
        settings.frame_port = 30000 + int(random_id() % 20000);
        UdpSocket occupied("0.0.0.0", settings.frame_port);
        settings.bind_ip = settings.sender_ip = "unused-local-source";
        App app;
        app.start(settings, true); // Fixed detections; never arms or moves a real mouse.
        auto until_size = [&](int size) {
            const auto deadline = now_ns() + 3'000'000'000ll;
            while (now_ns() < deadline) {
                paint();
                const auto stats = app.stats();
                check(stats.error.empty(), stats.error.c_str());
                const auto preview = app.preview();
                if (stats.inferred && stats.width == size && preview.frame &&
                    preview.frame->header.width == size)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            throw std::runtime_error("Local capture did not reach inference and preview");
        };
        until_size(32);
        check(app.stats().synchronized && app.stats().network.packets == 0 && !app.stats().armed,
              "Local source must bypass UDP clock sync and remain disarmed");
        settings.input_size = 160;
        app.configure(settings);
        until_size(160);
        const auto stop_started = now_ns();
        app.stop();
        check(now_ns() - stop_started < 1'000'000'000ll, "Capture shutdown must be bounded");
        check(!app.stats().running, "Local session did not stop");
        std::cout << "Local BGRA pixels, timestamps, UDP bypass, resize, preview and shutdown passed\n";
#endif
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
