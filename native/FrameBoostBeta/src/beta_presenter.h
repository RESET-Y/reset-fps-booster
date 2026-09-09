#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace FrameBoostBeta
