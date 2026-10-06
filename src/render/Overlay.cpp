#include "render/Overlay.h"

#include <algorithm>

#include <d3dcompiler.h>

#include "app/Log.h"
#include "render/LensShader.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"McScopeLensOverlay";
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

struct LensConstants {
    float center[2];
    float viewOrigin[2];
    float boxOrigin[2];
    float invTexSize[2];
    float radius;
    float strength;
    float gradual;
    float pad;
};
static_assert(sizeof(LensConstants) % 16 == 0);

LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kLensShaderSource, sizeof(kLensShaderSource) - 1, "Lens.hlsl", nullptr,
                            nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out,
                            &errors);
    if (FAILED(hr)) {
        logf("Shader %s failed to compile: %s", entry,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
        return false;
    }
    return true;
}

}  // namespace

Overlay::InitResult Overlay::init(HINSTANCE inst, ID3D11Device* device, DXGI_FORMAT format) {
    shutdown();
    device_ = device;
    format_ = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? format : DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!createWindow(inst)) return InitResult::Failed;

    // Without this, Desktop Duplication would capture our own lens and zoom it recursively.
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        logf("SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed: %lu", GetLastError());
        shutdown();
        return InitResult::CaptureExclusionUnsupported;
    }
    DWORD affinity = 0;
    if (!GetWindowDisplayAffinity(hwnd_, &affinity) || affinity != WDA_EXCLUDEFROMCAPTURE) {
        logf("Capture exclusion not honored (affinity=%lu); needs Windows 10 2004+", affinity);
        shutdown();
        return InitResult::CaptureExclusionUnsupported;
    }

    if (!createPipeline()) {
        shutdown();
        return InitResult::Failed;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(HR_CHECK(device_.As(&dxgiDevice))) ||
        FAILED(HR_CHECK(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_)))) ||
        FAILED(HR_CHECK(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) ||
        FAILED(HR_CHECK(dcomp_->CreateVisual(&dcompVisual_))) ||
        FAILED(HR_CHECK(dcompTarget_->SetRoot(dcompVisual_.Get())))) {
        shutdown();
        return InitResult::Failed;
    }
    return InitResult::Ok;
}

void Overlay::shutdown() {
    rtv_.Reset();
    if (frameWaitable_) CloseHandle(frameWaitable_);
    frameWaitable_ = nullptr;
    presentPending_ = false;
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    swapChain_.Reset();
    vs_.Reset();
    ps_.Reset();
    cbuffer_.Reset();
    sampler_.Reset();
    device_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    visible_ = false;
    size_ = 0;
    radius_ = 0;
}

bool Overlay::createWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = overlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logf("RegisterClassEx(overlay) failed: %lu", GetLastError());
        return false;
    }

    // LAYERED + TRANSPARENT = click-through; NOREDIRECTIONBITMAP = content comes from DComp only.
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                          WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    hwnd_ = CreateWindowExW(exStyle, kClassName, L"McScope Lens", WS_POPUP, 0, 0, 1, 1, nullptr,
                            nullptr, inst, nullptr);
    if (!hwnd_) {
        logf("CreateWindowEx(overlay) failed: %lu", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    return true;
}

bool Overlay::createPipeline() {
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compile("VS", "vs_5_0", vsBlob) || !compile("PS", "ps_5_0", psBlob)) return false;

    if (FAILED(HR_CHECK(device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                    vsBlob->GetBufferSize(), nullptr, &vs_))) ||
        FAILED(HR_CHECK(device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                                   psBlob->GetBufferSize(), nullptr, &ps_))))
        return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(LensConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(HR_CHECK(device_->CreateBuffer(&cb, nullptr, &cbuffer_)))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(HR_CHECK(device_->CreateSamplerState(&sd, &sampler_)));
}

bool Overlay::resizeSwapChain(UINT size) {
    rtv_.Reset();
    if (swapChain_) {
        if (FAILED(HR_CHECK(swapChain_->ResizeBuffers(0, size, size, DXGI_FORMAT_UNKNOWN,
                                                      kSwapChainFlags))))
            return false;
    } else {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(HR_CHECK(device_.As(&dxgiDevice))) ||
            FAILED(HR_CHECK(dxgiDevice->GetAdapter(&adapter))) ||
            FAILED(HR_CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)))))
            return false;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = size;
        desc.Height = size;
        desc.Format = format_;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = kSwapChainFlags;
        // FLIP_DISCARD + sync interval 0: DWM always takes the newest frame and drops older
        // queued ones, so a late frame can never leave a permanent extra frame of latency.
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr,
                                                          &swapChain_))) {
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            if (FAILED(HR_CHECK(factory->CreateSwapChainForComposition(device_.Get(), &desc,
                                                                       nullptr, &swapChain_))))
                return false;
        }

        // At most one frame queued; the waitable tells us when DWM has taken it.
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(swapChain_.As(&sc2)) && SUCCEEDED(HR_CHECK(sc2->SetMaximumFrameLatency(1)))) {
            frameWaitable_ = sc2->GetFrameLatencyWaitableObject();
            // Starts signaled once; consume it now so each later signal means "DWM took a frame".
            WaitForSingleObjectEx(frameWaitable_, 100, TRUE);
            presentPending_ = false;
        }

        if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            // HDR desktop: duplicated pixels are linear scRGB, present them as such.
            ComPtr<IDXGISwapChain3> sc3;
            if (SUCCEEDED(swapChain_.As(&sc3)))
                HR_CHECK(sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
        }
        if (FAILED(HR_CHECK(dcompVisual_->SetContent(swapChain_.Get()))) ||
            FAILED(HR_CHECK(dcomp_->Commit())))
            return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(HR_CHECK(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) ||
        FAILED(HR_CHECK(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_))))
        return false;
    size_ = size;
    return true;
}

int Overlay::clampRadius(const RECT& monitorRect, int radius) {
    const int w = monitorRect.right - monitorRect.left;
    const int h = monitorRect.bottom - monitorRect.top;
    return std::clamp(radius, 1, std::max(1, std::min(w, h) / 2));
}

RECT Overlay::sourceBox(const RECT& monitorRect, int radius) {
    const int w = monitorRect.right - monitorRect.left;
    const int h = monitorRect.bottom - monitorRect.top;
    radius = clampRadius(monitorRect, radius);
    const int cx = w / 2, cy = h / 2;
    return {std::max(0, cx - radius - 1), std::max(0, cy - radius - 1),
            std::min(w, cx + radius + 1), std::min(h, cy + radius + 1)};
}

bool Overlay::setGeometry(const RECT& monitorRect, int radius) {
    const int w = monitorRect.right - monitorRect.left;
    const int h = monitorRect.bottom - monitorRect.top;
    radius = clampRadius(monitorRect, radius);

    const UINT size = static_cast<UINT>(radius * 2);
    if (size != size_ && !resizeSwapChain(size)) return false;

    monitorRect_ = monitorRect;
    radius_ = radius;
    const int x = monitorRect.left + w / 2 - radius;
    const int y = monitorRect.top + h / 2 - radius;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, static_cast<int>(size), static_cast<int>(size),
                 SWP_NOACTIVATE);
    return true;
}

bool Overlay::render(ID3D11DeviceContext* ctx, const Source& source, float strength,
                     bool gradual) {
    if (!rtv_ || !source.srv) return false;

    const int w = monitorRect_.right - monitorRect_.left;
    const int h = monitorRect_.bottom - monitorRect_.top;
    const float cx = static_cast<float>(w / 2);
    const float cy = static_cast<float>(h / 2);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(HR_CHECK(ctx->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))))
        return false;
    auto* c = static_cast<LensConstants*>(mapped.pData);
    *c = {{cx, cy},
          {cx - radius_, cy - radius_},
          {static_cast<float>(source.boxOrigin.x), static_cast<float>(source.boxOrigin.y)},
          {1.0f / source.texWidth, 1.0f / source.texHeight},
          static_cast<float>(radius_),
          strength,
          gradual ? 1.0f : 0.0f,
          0};
    ctx->Unmap(cbuffer_.Get(), 0);
    ID3D11ShaderResourceView* frame = source.srv;

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(size_), static_cast<float>(size_), 0, 1};
    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    ID3D11Buffer* cbs[] = {cbuffer_.Get()};
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &frame);
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);  // frame texture is a copy target next frame

    // Sync interval 0: never block until vblank. Pacing comes from waitUntilReady() and
    // AcquireNextFrame; waiting here as well made each iteration span two vblanks.
    HRESULT hr = swapChain_->Present(0, 0);
    if (FAILED(hr)) {
        logf("Present failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    presentPending_ = true;
    return true;
}

void Overlay::waitUntilReady(DWORD timeoutMs) {
    if (!frameWaitable_ || !presentPending_) return;
    // A timeout is harmless (DWM still shows the newest frame); it only guards against
    // a waitable that ResizeBuffers or a hidden window left unsignaled.
    WaitForSingleObjectEx(frameWaitable_, timeoutMs, TRUE);
    presentPending_ = false;
}

void Overlay::show(bool visible) {
    if (!hwnd_ || visible == visible_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = visible;
}

void Overlay::bringToTop() {
    if (hwnd_ && visible_)
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
