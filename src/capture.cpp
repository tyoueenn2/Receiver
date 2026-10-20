#include "receiver/capture.hpp"
#include <stdexcept>
#ifdef _WIN32
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <sstream>

namespace receiver {
using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT hr, const char* action) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << "Local capture: " << action << " failed (0x" << std::hex << uint32_t(hr)
                << "). Check the selected display and desktop access, then restart the session.";
        throw std::runtime_error(message.str());
    }
}
struct Output {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
};
std::vector<Output> enumerate_outputs() {
    ComPtr<IDXGIFactory1> factory;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    std::vector<Output> outputs;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        auto hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        checked(hr, "EnumAdapters1");
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(o, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            checked(hr, "EnumOutputs");
            Output item{adapter, output, {}};
            checked(output->GetDesc(&item.desc), "GetDesc");
            if (item.desc.AttachedToDesktop)
                outputs.push_back(std::move(item));
        }
    }
    return outputs;
}
// Map DXGI's QPC timestamp into the same steady-clock domain as UDP deadlines.
// A delta avoids depending on the C++ library's steady-clock epoch.
int64_t present_time(int64_t ticks) {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto now = now_ns();
    QueryPerformanceCounter(&counter);
    const auto delta = counter.QuadPart - ticks;
    if (delta < 0)
        throw std::runtime_error("Local capture returned a future presentation timestamp");
    return now - (delta / frequency.QuadPart) * 1'000'000'000ll -
           (delta % frequency.QuadPart) * 1'000'000'000ll / frequency.QuadPart;
}
} // namespace
bool local_capture_supported() {
    return true;
}
std::vector<CaptureOutput> capture_outputs() {
    std::vector<CaptureOutput> result;
    for (const auto& output : enumerate_outputs()) {
        std::wstring wide(output.desc.DeviceName);
        std::string name(wide.begin(), wide.end()); // DXGI device names are ASCII.
        result.push_back({int(result.size()), name,
                          int(output.desc.DesktopCoordinates.right - output.desc.DesktopCoordinates.left),
                          int(output.desc.DesktopCoordinates.bottom - output.desc.DesktopCoordinates.top)});
    }
    return result;
}
struct LocalCapture::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    DXGI_OUTDUPL_DESC desc{};
    std::array<std::shared_ptr<Frame>, 4> pool{};
    uint32_t sequence = 0;
    int staging_size = 0;
    RECT bounds{};
    uint64_t display = 1469598103934665603ull;
    int64_t last_stamp = 0;

    explicit Impl(int index) {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        auto outputs = enumerate_outputs();
        if (index < 0 || size_t(index) >= outputs.size())
            throw std::runtime_error("Local capture display is unavailable. Choose an attached display.");
        auto& output = outputs[size_t(index)];
        bounds = output.desc.DesktopCoordinates;
        for (auto c : std::wstring(output.desc.DeviceName)) {
            display ^= uint16_t(c);
            display *= 1099511628211ull;
        }
        checked(D3D11CreateDevice(output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
                                  nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
                "D3D11CreateDevice");
        ComPtr<IDXGIOutput1> output1;
        checked(output.output.As(&output1), "Query IDXGIOutput1");
        checked(output1->DuplicateOutput(device.Get(), &duplication), "DuplicateOutput");
        duplication->GetDesc(&desc);
        if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED))
            throw std::runtime_error("Local capture requires an unrotated BGRA8 desktop display");
    }
    std::shared_ptr<const Frame> next(int size) {
        if (size < 1 || size > 1024 || UINT(size) > desc.ModeDesc.Width || UINT(size) > desc.ModeDesc.Height)
            throw std::runtime_error("Local capture crop must fit the selected display (maximum 1024)");
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        auto hr = duplication->AcquireNextFrame(10, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return {};
        checked(hr, "AcquireNextFrame");
        struct Release {
            IDXGIOutputDuplication* duplication;
            ~Release() {
                duplication->ReleaseFrame();
            }
        } release{duplication.Get()};
        if (!info.LastPresentTime.QuadPart)
            return {};
        ComPtr<ID3D11Texture2D> desktop;
        checked(resource.As(&desktop), "Query desktop texture");
        D3D11_TEXTURE2D_DESC source{};
        desktop->GetDesc(&source);
        if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || UINT(size) > source.Width ||
            UINT(size) > source.Height)
            throw std::runtime_error(
                "Local capture desktop format or dimensions changed. Restart the session.");
        std::shared_ptr<Frame> frame;
        for (auto& slot : pool) {
            if (!slot)
                slot = std::make_shared<Frame>();
            if (slot.use_count() == 1) {
                frame = slot;
                break;
            }
        }
        if (!frame)
            return {}; // A preview/benchmark still owns the pool: drop, never queue.
        const auto first = now_ns();
        if (staging_size != size) {
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = texture.Height = UINT(size);
            texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
            texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texture.Usage = D3D11_USAGE_STAGING;
            texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            checked(device->CreateTexture2D(&texture, nullptr, &staging), "Create crop staging texture");
            staging_size = size;
        }
        const UINT left = (source.Width - UINT(size)) / 2, top = (source.Height - UINT(size)) / 2;
        const D3D11_BOX crop{left, top, 0, left + UINT(size), top + UINT(size), 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, desktop.Get(), 0, &crop);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map crop staging texture");
        struct Unmap {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* texture;
            ~Unmap() {
                context->Unmap(texture, 0);
            }
        } unmap{context.Get(), staging.Get()};
        const size_t row = size_t(size) * 4;
        for (int y = 0; y < size; ++y)
            std::memcpy(frame->pixels.data() + size_t(y) * row,
                        static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, row);
        frame->header = {};
        frame->header.session = 1;
        frame->header.sequence = ++sequence;
        frame->header.capture_ns = present_time(info.LastPresentTime.QuadPart);
        last_stamp = frame->header.capture_ns;
        frame->header.width = frame->header.height = uint16_t(size);
        frame->header.format = 2;
        frame->header.bytes = uint32_t(row * size);
        frame->first_ns = first;
        frame->complete_ns = now_ns();
        return frame;
    }
};
LocalCapture::LocalCapture(int output) : impl_(std::make_unique<Impl>(output)) {}
LocalCapture::~LocalCapture() = default;
std::shared_ptr<const Frame> LocalCapture::next(int size) {
    return impl_->next(size);
}
cursor_protocol::Sample LocalCapture::cursor() const {
    cursor_protocol::Sample s;
    POINT p{};
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    s.session = s.token = 1;
    s.sample_ns = uint64_t(now_ns());
    s.sequence = uint32_t(s.sample_ns);
    s.generation = uint32_t(impl_->staging_size + 1);
    if (GetCursorPos(&p)) {
        s.flags |= 1;
        s.x = p.x;
        s.y = p.y;
    }
    if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING))
        s.flags |= 2;
    s.left = impl_->bounds.left;
    s.top = impl_->bounds.top;
    s.width = impl_->bounds.right - s.left;
    s.height = impl_->bounds.bottom - s.top;
    s.display = impl_->display;
    if (impl_->sequence) {
        s.flags |= 4;
        s.frame_sequence = impl_->sequence;
        s.frame_ns = uint64_t(impl_->last_stamp);
        s.crop_width = s.crop_height = impl_->staging_size;
        s.crop_left = s.left + (s.width - s.crop_width) / 2;
        s.crop_top = s.top + (s.height - s.crop_height) / 2;
    }
    return s;
}
} // namespace receiver
#else
namespace receiver {
bool local_capture_supported() {
    return false;
}
std::vector<CaptureOutput> capture_outputs() {
    return {};
}
struct LocalCapture::Impl {};
LocalCapture::LocalCapture(int) {
    throw std::runtime_error("Local desktop capture requires Windows 10/11. Select the UDP frame source.");
}
LocalCapture::~LocalCapture() = default;
std::shared_ptr<const Frame> LocalCapture::next(int) {
    return {};
}
cursor_protocol::Sample LocalCapture::cursor() const {
    return {};
}
} // namespace receiver
#endif
