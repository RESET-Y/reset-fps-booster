// THE OVERLAY AND THE SWAPCHAIN, and nothing else.
//
// This class shows a texture. It does not decide when. Everything about
// timing lives in the scheduler, because the V1 presenter grew a display-slot
// wait, a refresh-boundary snap, a keep-alive and a duplicate passthrough, and
// each of those quietly became a second clock competing with the first.
//
// The window flags below are carried over from the V1 presenter unchanged and
// are the single largest quality win the engine has had. They are not
// stylistic:
//
//   WS_EX_NOREDIRECTIONBITMAP  no redirection surface, so the content comes
//                              from a composition swapchain on the flip model
//   WS_EX_LAYERED|TRANSPARENT  mouse pass-through to another process needs
//                              both; dropping LAYERED made the captured window
//                              completely uninteractable
//   alpha 254, not 255         a window one step below opaque is not counted
//                              as an occluder, so games and browsers with a
//                              backgrounded mode keep rendering underneath.
//                              At 255 they stop, which starves the capture.
#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <winrt/base.h>

namespace fbv2 {

class Presenter {
public:
    ~Presenter();

    bool Create(ID3D11Device* device, UINT width, UINT height, HWND overlayTarget);
    void Destroy();

    // Follow the captured window if it moves or is resized.
    void TrackTarget();
    void Resize(UINT width, UINT height);
    void PumpMessages();

    HWND Window() const { return m_hwnd; }
    bool Ready()  const { return m_swapChain != nullptr; }

    // Copies the texture into the back buffer and presents it. Sync interval 0
    // with ALLOW_TEARING where the adapter supports it: the output follows the
    // source's timeline, not the panel's, which is the whole point of the
    // scheduler that drives this.
    bool Present(ID3D11DeviceContext* context, ID3D11Texture2D* texture);

    // Split so the two halves stay distinguishable, because they need
    // different cures. The wait is the present queue being full and costs
    // latency; the call is DXGI's own work and holds the device.
    double WaitMsSum()  const { return m_waitMsSum; }
    double CallMsSum()  const { return m_callMsSum; }
    double CallMsMax()  const { return m_callMsMax; }
    uint64_t Presents() const { return m_presents; }
    void ResetStats() { m_waitMsSum = m_callMsSum = m_callMsMax = 0.0; m_presents = 0; }

    // What DXGI itself says reached the panel, as opposed to what we handed it.
    bool QueryDisplayed(UINT& outDisplayed, UINT& outSubmitted) const;

private:
    bool CreateOverlayWindow(UINT width, UINT height);
    bool CreateSwapChain(ID3D11Device* device, UINT width, UINT height);

    HWND m_hwnd = nullptr;
    HWND m_target = nullptr;

    winrt::com_ptr<IDXGISwapChain1>     m_swapChain;
    winrt::com_ptr<IDCompositionDevice> m_compDevice;
    winrt::com_ptr<IDCompositionTarget> m_compTarget;
    winrt::com_ptr<IDCompositionVisual> m_compVisual;
    winrt::com_ptr<ID3D11Device>        m_device;

    HANDLE m_frameLatencyWaitable = nullptr;
    bool   m_tearingSupported = false;
    UINT   m_width = 0, m_height = 0;

    double m_waitMsSum = 0.0, m_callMsSum = 0.0, m_callMsMax = 0.0;
    uint64_t m_presents = 0;
};

} // namespace fbv2
