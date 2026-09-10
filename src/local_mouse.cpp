#include "receiver/local_mouse.hpp"
#include "receiver/network.hpp"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace receiver {
void LocalMouseState::pointer(int px, int py, bool injected, int64_t now) {
    if (have_position && !injected) {
        auto dx = int64_t(px) - x, dy = int64_t(py) - y;
        total_x += uint32_t(dx);
        total_y += uint32_t(dy);
        if (dx || dy)
            last_motion = now;
    }
    // An injected event still updates the baseline, but never physical totals.
    x = px;
    y = py;
    have_position = true;
}
void LocalMouseState::button(uint8_t mask, bool down, bool injected) {
    if (!injected)
        buttons = down ? uint8_t(buttons | mask) : uint8_t(buttons & ~mask);
}
#ifdef _WIN32
struct LocalMouse::Impl {
    HHOOK hook = nullptr;
    HANDLE timer = nullptr;
    ~Impl() {
        if (timer)
            CloseHandle(timer);
    }
    LocalMouseState state;
    int64_t polled = 0;
    int64_t button_at = 0;
    bool ready = false;
    uint64_t generation = random_id();
    uint32_t sequence = 0;
    static thread_local Impl* current;
    static LRESULT CALLBACK callback(int code, WPARAM message, LPARAM payload) {
        auto* self = current;
        if (code == HC_ACTION && self) {
            auto& e = *reinterpret_cast<MSLLHOOKSTRUCT*>(payload);
            bool injected = (e.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
            if (message == WM_MOUSEMOVE)
                self->state.pointer(e.pt.x, e.pt.y, injected, now_ns());
            uint8_t mask = 0;
            bool down = false;
            switch (message) {
            case WM_LBUTTONDOWN:
                mask = 1;
                down = true;
                break;
            case WM_LBUTTONUP:
                mask = 1;
                break;
            case WM_RBUTTONDOWN:
                mask = 2;
                down = true;
                break;
            case WM_RBUTTONUP:
                mask = 2;
                break;
            case WM_MBUTTONDOWN:
                mask = 4;
                down = true;
                break;
            case WM_MBUTTONUP:
                mask = 4;
                break;
            case WM_XBUTTONDOWN:
                mask = HIWORD(e.mouseData) == XBUTTON1 ? 8 : 16;
                down = true;
                break;
            case WM_XBUTTONUP:
                mask = HIWORD(e.mouseData) == XBUTTON1 ? 8 : 16;
                break;
            }
            if (mask) {
                self->state.button(mask, down, injected);
                if (!injected)
                    self->button_at = now_ns();
            }
        }
        return CallNextHookEx(nullptr, code, message, payload);
    }
};
thread_local LocalMouse::Impl* LocalMouse::Impl::current = nullptr;
LocalMouse::LocalMouse() : impl_(std::make_unique<Impl>()) {
    if (Impl::current)
        throw std::runtime_error("A local mouse reader is already active on this worker");
    impl_->timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!impl_->timer)
        throw std::runtime_error("Windows could not create the local mouse timer");
    Impl::current = impl_.get();
    POINT p{};
    if (GetCursorPos(&p))
        impl_->state.pointer(p.x, p.y, true, now_ns());
    impl_->hook = SetWindowsHookExW(WH_MOUSE_LL, Impl::callback, GetModuleHandleW(nullptr), 0);
    if (!impl_->hook) {
        Impl::current = nullptr;
        throw std::runtime_error("Windows could not start local mouse input");
    }
}
LocalMouse::~LocalMouse() {
    if (impl_->hook)
        UnhookWindowsHookEx(impl_->hook);
    Impl::current = nullptr;
}
void LocalMouse::wait() {
    LARGE_INTEGER due;
    due.QuadPart = -10000; // One millisecond, relative.
    if (!SetWaitableTimer(impl_->timer, &due, 0, nullptr, nullptr, FALSE))
        throw std::runtime_error("Windows local mouse timer failed");
    // Pump input promptly even while waiting for the next control update.
    if (MsgWaitForMultipleObjectsEx(1, &impl_->timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
        WAIT_FAILED)
        throw std::runtime_error("Windows local mouse wait failed");
}
Telemetry LocalMouse::poll() {
    MSG message{};
    for (int i = 0; i < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    POINT p{};
    impl_->ready = GetCursorPos(&p) != 0;
    const int keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    if (now_ns() - impl_->button_at > 20'000'000)
        for (int i = 0; i < 5; ++i)
            if (!(GetAsyncKeyState(keys[i]) & 0x8000))
                impl_->state.buttons &= uint8_t(~(1u << i));
    impl_->polled = now_ns();
    Telemetry t;
    t.ready = impl_->ready;
    t.has_motion = true;
    t.physical = impl_->state.buttons;
    t.xmin = t.ymin = -127;
    t.xmax = t.ymax = 127;
    t.server = t.motion_generation = impl_->generation;
    t.sample_ns = uint64_t(impl_->polled);
    t.sequence = ++impl_->sequence;
    t.total_x = impl_->state.total_x;
    t.total_y = impl_->state.total_y;
    t.motion_age_us =
        impl_->state.last_motion
            ? uint32_t(std::min<int64_t>((impl_->polled - impl_->state.last_motion) / 1000, 0xffffffffll))
            : 0xffffffffu;
    return t;
}
bool LocalMouse::fresh(int64_t now) const {
    return impl_->ready && now >= impl_->polled && now - impl_->polled <= 50'000'000;
}
bool LocalMouse::send(int dx, int dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dwExtraInfo = 0x52564352;
    return SendInput(1, &input, sizeof(input)) == 1;
}
#else
struct LocalMouse::Impl {};
LocalMouse::LocalMouse() {
    throw std::runtime_error("Local mouse testing requires Windows");
}
LocalMouse::~LocalMouse() = default;
void LocalMouse::wait() {
    LARGE_INTEGER due;
    due.QuadPart = -10000; // One millisecond, relative.
    if (!SetWaitableTimer(impl_->timer, &due, 0, nullptr, nullptr, FALSE))
        throw std::runtime_error("Windows local mouse timer failed");
    // Pump input promptly even while waiting for the next control update.
    if (MsgWaitForMultipleObjectsEx(1, &impl_->timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
        WAIT_FAILED)
        throw std::runtime_error("Windows local mouse wait failed");
}
void LocalMouse::wait() {}
Telemetry LocalMouse::poll() {
    return {};
}
bool LocalMouse::fresh(int64_t) const {
    return false;
}
bool LocalMouse::send(int, int) {
    return false;
}
#endif
} // namespace receiver
