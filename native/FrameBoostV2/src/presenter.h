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

    // SIDE BY SIDE WITH THE UNTOUCHED GAME. Asked for as a way to compare
    // without comparing memories: the overlay covers only the LEFT half of
    // the window, so the right half is the game as it renders itself, in the
    // same scene at the same instant. Diagnostic; off unless `half` is given.
    void EnableHalfWidth(bool on) { m_halfWidth = on; }
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
    // THE TWO HALVES OF "PRESENT" WERE BEING MEASURED AS ONE, and that was a
    // measurement error: the span around this call also contains GetBuffer and
    // a full-resolution CopyResource. "Present blocks in 11% of frames" was
    // therefore copy-plus-present, and the copy of a 2560x1440 surface is not
    // a small part of it. PresentTiming separates them so the claim can be
    // made about the right one.
    //
    // waitableFree peeks at the frame-latency semaphore with a zero timeout -
    // it never blocks. Signalled means DXGI had room for another present;
    // not signalled means the queue was full and Present was about to pay for
    // it. That is the direct test of "we present faster than DXGI drains",
    // rather than an inference from how long the call took.
    //
    // Peeking CONSUMES a count from that semaphore. Nothing waits on it - the
    // comment in Present() explains why it is deliberately never waited on -
    // so there is nothing for the consumption to starve, and DXGI's own queue
    // is unaffected by it.
    struct PresentTiming {
        double copyMs = 0.0;      // GetBuffer + CopyResource
        double presentMs = 0.0;   // the Present call alone
        bool   waitableFree = false;
        bool   waitableKnown = false;
        UINT   submitted = 0;     // GetLastPresentCount
        UINT   displayed = 0;     // DXGI_FRAME_STATISTICS::PresentCount
        bool   statsKnown = false;

        // WHAT THE PANEL DID, as opposed to what we asked it to do.
        //
        // Every other number in this struct is about the moment we CALLED
        // Present. None of them can say for how many refreshes a picture
        // actually stood on the screen, and deriving that from present
        // timestamps is the mistake this exists to stop: by that arithmetic
        // generated frames occupy 58% of the time, while what is visible on
        // screen is the opposite.
        //
        // PresentRefreshCount is the refresh the present became visible at.
        // The difference between one present's value and the next one's is how
        // many refreshes that picture actually held the panel.
        //
        // The statistics LAG: right after our Present, DXGI usually still
        // describes an earlier one. So statPresentCount travels with the pair
        // and says WHICH present the refresh number belongs to; matching that
        // against our own submitted count happens in analysis, never by
        // assuming the two line up.
        UINT   statPresentCount = 0;    // DXGI's PresentCount for these stats
        UINT   statPresentRefresh = 0;  // PresentRefreshCount
        UINT   statSyncRefresh = 0;     // SyncRefreshCount
        bool   statRefreshKnown = false;
    };
    // A CORNER BLOCK SAYING WHICH KIND OF FRAME THIS IS.
    //
    // Both kinds are marked, not just the generated ones: with only one
    // colour, "no block" means either a real frame or a frame nobody looked
    // at, and those are exactly the two things this is meant to tell apart.
    // Red alternating with green at the source rate is the engine working;
    // red appearing rarely or never means generated frames are produced and
    // then buried before scan-out, which the present-interval minimum has
    // been hinting at and no counter can settle.
    //
    // Off unless the `mark` argument is given. It draws into the back buffer
    // after the copy, so it costs one ClearView and changes nothing else.
    enum class Marker { None, Native, Generated };
    bool Present(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                 PresentTiming* timing = nullptr, Marker marker = Marker::None);

    // Logged once at start-up: everything about the swapchain that could
    // explain a blocking Present, so the configuration is on the record beside
    // the measurements rather than read out of the source months later.
    void LogConfiguration() const;

    // VSYNC AS AN EXPERIMENT, not as a cap.
    //
    // Tearing presents exist so the output can exceed the refresh rate, which
    // is a hard requirement. But with a 30 fps source the output is 60 on a
    // 144 Hz panel - nowhere near the ceiling - and every present still lands
    // mid-scan, so the screen shows a torn composite of two moments instead of
    // a clean doubled sequence. That was reported the moment generated frames
    // stopped being copies of their neighbours: "die linke Seite hat jetzt
    // tearing".
    //
    // This switch is here to find out whether the tearing is what hides the
    // benefit. It is NOT the shipping answer: sync interval 1 would cap the
    // output at the refresh rate, which the engine may not do.
    void EnableVsync(bool on) { m_vsync = on; }

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
    bool   m_halfWidth = false;
    bool   m_vsync = false;
    UINT   m_width = 0, m_height = 0;

    double m_waitMsSum = 0.0, m_callMsSum = 0.0, m_callMsMax = 0.0;
    uint64_t m_presents = 0;
};

} // namespace fbv2
