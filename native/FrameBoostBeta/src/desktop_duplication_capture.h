#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <cstdint>

namespace FrameBoostBeta {

// Captures a monitor through DXGI Desktop Duplication, as a drop-in
// alternative to the Windows Graphics Capture path in capture_engine.h -
// same polling shape, same timestamp domain, so the rest of the engine does
// not need to know which one it is talking to.
//
// Why a second capture path exists at all, measured rather than assumed:
// with Rocket League reporting 90 FPS, on the same monitor in the same
// minute, WGC produced 44.60 frames per second while Desktop Duplication
// reported 90.20 desktop presents per second, spaced 11.09 ms. The WGC
// numbers were not a case of us reading too slowly - its own audit showed
// 892 frames announced, 892 retrieved, 0 lost in the pool. Windows Graphics
// Capture simply hands out about half of what the compositor presents, and
// half the source frame rate is half of what the interpolator has to work
// with.
//
// Desktop Duplication is a public, documented, user-mode API - the same one
// every screen recorder and remote-desktop tool uses. Nothing here reads or
// touches a game.
//
// It also honours WDA_EXCLUDEFROMCAPTURE, which the overlay depends on so
// the engine does not capture its own output. Verified with a control run:
// an unexcluded probe window came back on 64 of 64 sampled points, the same
// window with the flag set on 0 of 64.
class DesktopDuplicationCapture {
public:
    bool StartMonitor(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context);
    void Stop();
    bool IsCapturing() const { return m_duplication != nullptr; }

    // Same contract as CaptureEngine::PollLatestFrame: non-blocking, returns
    // the newest frame, outIsNewFrame false when nothing new arrived.
    //
    // outFrameTimestamp100ns is LastPresentTime converted to the same
    // 100ns-since-boot QPC domain the rest of the engine measures in, so
    // latency and interval maths are unchanged. It is the moment the desktop
    // was actually presented - a better timestamp than WGC's, which describes
    // when the capture was taken.
    ID3D11Texture2D* PollLatestFrame(UINT& outWidth, UINT& outHeight,
                                     int64_t& outFrameTimestamp100ns, bool& outIsNewFrame);

    // Frames the compositor presented that were folded into one acquire
    // because we did not ask in time. The direct "are we too slow" number;
    // Desktop Duplication reports it, WGC has no equivalent.
    uint64_t CoalescedFrames() const { return m_coalescedFrames; }
    uint64_t FramesRetrieved() const { return m_framesRetrieved; }

    // Cursor-only updates, which carry no new game content and must not be
    // counted as frames.
    uint64_t CursorOnlyUpdates() const { return m_cursorOnlyUpdates; }

    // The display mode changed, or something took the output away (a game
    // entering exclusive fullscreen, a resolution change, a driver reset).
    // The duplication is rebuilt automatically; this counts how often.
    uint64_t Reconnects() const { return m_reconnects; }

    ~DesktopDuplicationCapture();

private:
    bool CreateDuplication();

    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<IDXGIOutput1> m_output;
    winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
    winrt::com_ptr<ID3D11Texture2D> m_frameTex; // our own copy, valid past ReleaseFrame
    HMONITOR m_monitor = nullptr;
    UINT m_width = 0, m_height = 0;

    uint64_t m_framesRetrieved = 0;
    uint64_t m_coalescedFrames = 0;
    uint64_t m_cursorOnlyUpdates = 0;
    uint64_t m_reconnects = 0;
    double m_nextReconnectAttemptMs = 0.0;
};

} // namespace FrameBoostBeta
