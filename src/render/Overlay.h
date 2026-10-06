#pragma once

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// Topmost, click-through, capture-excluded window sized to the lens bounding box,
// presented through DirectComposition with per-pixel alpha.
class Overlay {
public:
    enum class InitResult { Ok, Failed, CaptureExclusionUnsupported };

    ~Overlay() { shutdown(); }

    // `format` is the desktop duplication format; the swap chain matches it.
    // Must be called (and the overlay used) from a single thread that pumps messages.
    InitResult init(HINSTANCE inst, ID3D11Device* device, DXGI_FORMAT format);
    void shutdown();

    // Captured pixels the lens samples from: the source box copied to the texture's top-left.
    struct Source {
        ID3D11ShaderResourceView* srv;
        POINT boxOrigin;  // monitor-relative top-left of the copied box
        UINT texWidth, texHeight;
    };

    // Radius actually used on this monitor (at least 1, at most half the short side).
    static int clampRadius(const RECT& monitorRect, int radius);

    // Monitor-relative region the lens can sample for this radius (+1px for bilinear filtering).
    static RECT sourceBox(const RECT& monitorRect, int radius);

    // Places the lens centered on `monitorRect`. Resizes window + swap chain if needed.
    bool setGeometry(const RECT& monitorRect, int radius);

    // Draws the lens and presents without waiting for vblank (DWM picks up the newest frame).
    bool render(ID3D11DeviceContext* ctx, const Source& source, float strength, bool gradual);

    // Blocks until DWM has taken our previous frame, so the next capture is as fresh as
    // possible and never queues behind an older one. Returns immediately if nothing is pending.
    void waitUntilReady(DWORD timeoutMs);

    void show(bool visible);
    void bringToTop();
    HWND hwnd() const { return hwnd_; }

private:
    bool createWindow(HINSTANCE inst);
    bool createPipeline();
    bool resizeSwapChain(UINT size);

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    HANDLE frameWaitable_ = nullptr;
    bool presentPending_ = false;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    RECT monitorRect_{};
    int radius_ = 0;
    UINT size_ = 0;  // swap chain edge length = 2 * radius
};
