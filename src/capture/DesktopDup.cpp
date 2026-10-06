#include "capture/DesktopDup.h"

#include <algorithm>

#include "app/Log.h"

using Microsoft::WRL::ComPtr;

bool DesktopDup::init(HMONITOR monitor) {
    shutdown();

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(HR_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))) return false;

    // Find the adapter + output that drive this monitor.
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(o->GetDesc(&desc)) && desc.Monitor == monitor) {
                output = o;
                monitorRect_ = desc.DesktopCoordinates;
                if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                    desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
                    logf("Warning: monitor is rotated; lens image will be rotated too");
                break;
            }
        }
    }
    if (!output) {
        logf("No DXGI output found for monitor %p", monitor);
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr,
                                   &context_);
#ifdef _DEBUG
    if (FAILED(hr))  // debug layer not installed
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               flags & ~D3D11_CREATE_DEVICE_DEBUG, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
#endif
    if (FAILED(hr)) {
        logf("D3D11CreateDevice failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // Keep GPU queue short: we want the newest frame, not throughput. Highest GPU thread
    // priority so a GPU-bound game doesn't starve our tiny per-frame workload.
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice))) {
        dxgiDevice->SetMaximumFrameLatency(1);
        HR_CHECK(dxgiDevice->SetGPUThreadPriority(7));
    }

    if (FAILED(HR_CHECK(output.As(&output_)))) return false;
    monitor_ = monitor;
    return recreateDuplication();
}

void DesktopDup::shutdown() {
    dup_.Reset();
    frameSrv_.Reset();
    frameTex_.Reset();
    frameWidth_ = frameHeight_ = 0;
    copiedBox_ = {};
    output_.Reset();
    context_.Reset();
    device_.Reset();
    monitor_ = nullptr;
}

bool DesktopDup::recreateDuplication() {
    dup_.Reset();
    if (!output_) return false;

    // Prefer DuplicateOutput1 so HDR desktops come through as FP16 instead of being refused.
    HRESULT hr = E_NOINTERFACE;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output_.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT};
        hr = output5->DuplicateOutput1(device_.Get(), 0, ARRAYSIZE(formats), formats, &dup_);
    }
    if (FAILED(hr)) hr = output_->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        // E_ACCESSDENIED is expected on the secure desktop (UAC, lock screen); caller retries.
        logf("DuplicateOutput failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    DXGI_OUTDUPL_DESC desc;
    dup_->GetDesc(&desc);
    format_ = desc.ModeDesc.Format;
    const auto& rr = desc.ModeDesc.RefreshRate;
    refreshHz_ = rr.Denominator ? static_cast<float>(rr.Numerator) / rr.Denominator : 0.0f;
    if (refreshHz_ <= 0) {  // some drivers leave the duplication mode's rate empty
        DXGI_OUTPUT_DESC od;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (SUCCEEDED(output_->GetDesc(&od)) &&
            EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            refreshHz_ = static_cast<float>(dm.dmDisplayFrequency);
    }
    logf("Duplicating %dx%d @ %.2f Hz, format %d", width(), height(), refreshHz_,
         static_cast<int>(format_));
    return true;
}

bool DesktopDup::ensureFrameTexture(const D3D11_TEXTURE2D_DESC& src) {
    if (frameTex_) {
        D3D11_TEXTURE2D_DESC cur;
        frameTex_->GetDesc(&cur);
        if (cur.Width == src.Width && cur.Height == src.Height && cur.Format == src.Format)
            return true;
    }
    frameSrv_.Reset();
    frameTex_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = src.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(HR_CHECK(device_->CreateTexture2D(&desc, nullptr, &frameTex_)))) return false;
    if (FAILED(HR_CHECK(device_->CreateShaderResourceView(frameTex_.Get(), nullptr, &frameSrv_)))) {
        frameTex_.Reset();
        return false;
    }
    frameWidth_ = desc.Width;
    frameHeight_ = desc.Height;
    copiedBox_ = {};
    return true;
}

DesktopDup::Result DesktopDup::acquire(UINT timeoutMs, const RECT& box) {
    if (!dup_) return Result::AccessLost;

    DXGI_OUTDUPL_FRAME_INFO info;
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) return Result::AccessLost;
    if (FAILED(hr)) {
        logf("AcquireNextFrame failed: 0x%08X", static_cast<unsigned>(hr));
        return Result::Error;
    }

    // The acquired surface always holds the whole current desktop, so a pointer-only update
    // is still a valid source when the box changed (e.g. radius slider dragged).
    Result result = Result::NoChange;
    if (info.LastPresentTime.QuadPart != 0 || !hasFrameFor(box)) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);
            if (ensureFrameTexture(desc)) {
                // Copy only the lens region: at 4K/HDR a full-frame copy every refresh
                // costs GBs/s of bandwidth that a GPU-bound game needs.
                auto clampTo = [](LONG v, UINT hi) {
                    return static_cast<UINT>(std::clamp<LONG>(v, 0, static_cast<LONG>(hi)));
                };
                D3D11_BOX src{clampTo(box.left, desc.Width), clampTo(box.top, desc.Height), 0,
                              clampTo(box.right, desc.Width), clampTo(box.bottom, desc.Height), 1};
                context_->CopySubresourceRegion(frameTex_.Get(), 0, 0, 0, 0, tex.Get(), 0, &src);
                copiedBox_ = box;
                lastUpdateQpc_ = info.LastPresentTime.QuadPart;
                result = Result::NewFrame;
            } else {
                result = Result::Error;
            }
        }
    }
    dup_->ReleaseFrame();
    return result;
}
