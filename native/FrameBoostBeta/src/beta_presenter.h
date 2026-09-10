#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <string>

namespace FrameBoostBeta {

// Our OWN window and OWN swapchain - this is what makes the beta
// architecturally different (and more robust) than the Watch Dogs proxy-DLL
// prototype: we are never at the mercy of some other application's legacy
// swapchain configuration. This one is always a modern flip-model
// swapchain, created by us, so frame insertion has a real, working buffer
// queue behind it.
class Presenter {
public:
    // overlayTarget: when non-null, the window is created as a click-through,
    // never-activating, always-on-top overlay positioned exactly over that
    // window instead of as a normal standalone window.
    //
    // This is what makes the whole approach usable: measured on Watch Dogs,
    // the target game throttles itself from ~37 FPS to ~11 FPS the moment
    // its window loses foreground focus. A normal output window therefore
    // starves its own input signal simply by being looked at. An overlay
    // that never takes focus keeps the game running at full speed while
    // still showing the boosted result on top of it.
    bool Create(ID3D11Device* device, UINT width, UINT height, const wchar_t* title, HWND overlayTarget = nullptr);
    void Resize(UINT width, UINT height);

    // Monitor mode: the overlay covers a whole monitor rather than tracking
    // a single window. Must be set before Create().
    void SetOverlayMonitor(HMONITOR monitor) { m_overlayMonitor = monitor; }

    // Keeps the overlay aligned with the target window (or monitor).
    void TrackOverlayTarget();

    // Copies the given texture into the current back buffer and presents
    // it. syncInterval 0 or 1 as needed by the caller's pacing strategy.
    HRESULT PresentFrame(ID3D11DeviceContext* context, ID3D11Texture2D* sourceTexture, UINT syncInterval);

    // How many of our presents the DISPLAY actually showed, as opposed to how
    // many we submitted. These are different numbers and only the first one
    // corresponds to what the eye sees: a layered window cannot use the flip
    // model, so its content reaches the panel through DWM's redirection
    // surface on DWM's schedule, not ours. Submitting 144 perfectly spaced
    // frames per second proves nothing if DWM picks up only some of them -
    // which would explain measured-perfect pacing alongside visible judder.
    //
    // Returns false when the swapchain cannot report statistics at all
    // (itself the answer: the legacy BitBlt path makes no per-refresh
    // guarantee).
    bool QueryPresentStats(UINT& outPresentCount, UINT& outPresentRefreshCount, UINT& outSyncRefreshCount);

    HWND WindowHandle() const { return m_hwnd; }
    bool ShouldClose() const { return m_shouldClose; }
    void PumpMessages();

    // So the F9 A/B toggle state is visible at a glance (taskbar/title bar)
    // instead of only in the log file.
    void SetTitleSuffix(const wchar_t* suffix);

    ~Presenter();

private:
    HWND m_hwnd = nullptr;
    IDXGISwapChain1* m_swapChain = nullptr;
    UINT m_width = 0, m_height = 0;
    bool m_shouldClose = false;
    std::wstring m_baseTitle;
    HWND m_overlayTarget = nullptr;
    HMONITOR m_overlayMonitor = nullptr;

    // DirectComposition path. A layered window (the old way to get
    // click-through) cannot host a flip-model swapchain, so its frames reach
    // the panel through DWM's redirection surface with no per-refresh
    // delivery guarantee - and DXGI refuses to report frame statistics for
    // it at all, so it cannot even be measured. A composition swapchain on a
    // WS_EX_NOREDIRECTIONBITMAP window is a real flip-model swapchain,
    // keeps click-through, reports statistics, and supports per-pixel alpha.
    IDCompositionDevice* m_dcompDevice = nullptr;
    IDCompositionTarget* m_dcompTarget = nullptr;
    IDCompositionVisual* m_dcompVisual = nullptr;
    bool m_usingComposition = false;
    // Set when the composition path has already failed once, so the layered
    // fallback cannot loop back into trying it again.
    bool m_compositionDisabled = false;

    bool TryCreateCompositionWindow(ID3D11Device* device, UINT width, UINT height, const wchar_t* title);
    bool AttachComposition(ID3D11Device* device, IDXGISwapChain1* swapChain);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace FrameBoostBeta
