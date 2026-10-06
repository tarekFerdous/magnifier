#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// DXGI Desktop Duplication of a single monitor. Owns the D3D11 device, which is created
// on the adapter that drives that monitor (required on hybrid-GPU laptops).
class DesktopDup {
public:
    enum class Result { NewFrame, NoChange, AccessLost, Error };

    // Creates the device and the duplication for `monitor`. Safe to call again to switch monitors.
    bool init(HMONITOR monitor);
    void shutdown();

    // Recreates just the duplication (after ACCESS_LOST), keeping the device.
    bool recreateDuplication();

    // Waits up to timeoutMs for a desktop update and copies only `box` (monitor-relative)
    // into the top-left of frameSrv(). Returns NewFrame when the copy changed.
    Result acquire(UINT timeoutMs, const RECT& box);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    ID3D11ShaderResourceView* frameSrv() const { return frameSrv_.Get(); }
    UINT frameWidth() const { return frameWidth_; }
    UINT frameHeight() const { return frameHeight_; }
    // True when frameSrv() holds pixels for exactly this box.
    bool hasFrameFor(const RECT& box) const { return frameSrv_ && EqualRect(&box, &copiedBox_); }
    void invalidateFrame() { copiedBox_ = {}; }
    DXGI_FORMAT format() const { return format_; }
    float refreshHz() const { return refreshHz_; }
    // QPC time at which the desktop image of the last NewFrame was updated (0 if unknown).
    LONGLONG lastUpdateQpc() const { return lastUpdateQpc_; }
    HMONITOR monitor() const { return monitor_; }
    RECT monitorRect() const { return monitorRect_; }  // virtual-desktop coordinates, physical px
    int width() const { return monitorRect_.right - monitorRect_.left; }
    int height() const { return monitorRect_.bottom - monitorRect_.top; }

private:
    bool ensureFrameTexture(const D3D11_TEXTURE2D_DESC& src);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frameSrv_;
    UINT frameWidth_ = 0, frameHeight_ = 0;
    RECT copiedBox_{};
    float refreshHz_ = 0;
    LONGLONG lastUpdateQpc_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    HMONITOR monitor_ = nullptr;
    RECT monitorRect_{};
};
