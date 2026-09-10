#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

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
// Capture simply hands out about half of what the compositor presents.
//
// Desktop Duplication is a public, documented, user-mode API - the same one
// every screen recorder and remote-desktop tool uses. Nothing here reads or
// touches a game.
//
// It also honours WDA_EXCLUDEFROMCAPTURE, which the overlay depends on so
// the engine does not capture its own output. Verified with a control run:
// an unexcluded probe window came back on 64 of 64 sampled points, the same
// window with the flag set on 0 of 64.
//
// ACQUISITION IS PUMPED FROM THE MAIN LOOP'S IDLE TIME, and the reason is
// measured. Reading once per output slot let AccumulatedFrames climb to 60-70
// coalesced updates per second - two thirds of a 90 FPS source arriving as one
// frame, because the loop spends most of each slot waiting for a refresh
// boundary. Moving acquisition onto its own thread fixed that (coalescing fell
// to zero) and broke something worse: sharing one D3D11 immediate context
// across two threads needs SetMultithreadProtected, and the full-screen copies
// then serialised against the render work - output collapsed to 3-7 FPS with
// 100% missed slots and 86-153 ms on-screen age.
//
// So the capture is pumped instead: the loop already spends milliseconds doing
// nothing but waiting for its next refresh boundary, and Pump() spends that
// time acquiring. Same thread, same context, no lock contention, and the
// compositor is asked several times per output slot instead of once.
class DesktopDuplicationCapture {
public:
    bool StartMonitor(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context);
    void Stop();
    bool IsCapturing() const { return m_duplication != nullptr; }

    // Acquires everything the compositor has ready, without blocking. Call it
    // from anywhere the loop would otherwise be idle - the more often it runs,
    // the less the compositor has to coalesce.
    void Pump();

    // Same contract as CaptureEngine::PollLatestFrame: non-blocking, returns
    // the newest frame acquired so far, outIsNewFrame false
    // when nothing new arrived since the last call.
    //
    // outFrameTimestamp100ns is LastPresentTime converted to the same
    // 100ns-since-boot QPC domain the rest of the engine measures in, so
    // latency and interval maths are unchanged. It is the moment the desktop
    // was actually presented - a better timestamp than WGC's, which describes
    // when the capture was taken.
    ID3D11Texture2D* PollLatestFrame(UINT& outWidth, UINT& outHeight,
                                     int64_t& outFrameTimestamp100ns, bool& outIsNewFrame);

    // Frames the compositor presented that were folded into one acquire
    // because nobody asked in time. Desktop Duplication reports this directly
    // (AccumulatedFrames); WGC has no equivalent. Near zero means the pump is
    // keeping up with the compositor; a climbing count means it is not.
    uint64_t CoalescedFrames() const { return m_coalescedFrames.load(std::memory_order_relaxed); }

    // Frames acquired versus frames the output side actually took. A gap is
    // not a capture loss - it means the output is consuming slower than the
    // source produces, which is normal for a pair-based interpolator.
    uint64_t FramesPublished() const { return m_framesPublished.load(std::memory_order_relaxed); }
    uint64_t FramesConsumed() const { return m_framesConsumed; }

    // Cursor-only updates, which carry no new game content and must not be
    // counted as frames.
    uint64_t CursorOnlyUpdates() const { return m_cursorOnlyUpdates.load(std::memory_order_relaxed); }

    // Presents that changed no pixels at all, according to the API.s own
    // dirty-rectangle metadata. This replaces comparing thumbnails on the GPU:
    // that comparison cost 0.9-1.8 ms per arrival and, at ~90 arrivals a
    // second, 80-160 ms of every second - a readback that stalls the pipeline
    // to answer a question the compositor already knows the answer to.
    uint64_t UnchangedFrames() const { return m_unchangedFrames.load(std::memory_order_relaxed); }

    // True when the driver gave us usable dirty-rect metadata for the last
    // frame. Without it, "no dirty rects" cannot be distinguished from "no
    // information", and the frame has to be assumed changed.
    bool DirtyRectsAvailable() const { return m_dirtyRectsAvailable.load(std::memory_order_relaxed); }

    // The display mode changed, or something took the output away (a game
    // entering exclusive fullscreen, a resolution change, a driver reset).
    // The duplication is rebuilt automatically; this counts how often.
    uint64_t Reconnects() const { return m_reconnects.load(std::memory_order_relaxed); }

    // Average interval between PUBLISHED frames, in milliseconds - the source.s
    // real rate, seen where every frame passes.
    //
    // The engine used to derive this from the frames it had processed, which
    // turns into a feedback loop the moment it cannot keep up: skipping every
    // second frame makes the measured interval twice as long, so the output
    // waits twice as long for its real frame, which guarantees it skips every
    // second frame again. Measured in Apex: capture publishing 74 frames a
    // second, the loop consuming 37, the source reported as 36.5, and the
    // doubled output landing at 73 - the game.s own rate, so the boost was
    // worth nothing. It is stable in that state and cannot climb out on its own.
    double PublishedIntervalMs() const {
        std::lock_guard<std::mutex> lock(m_slotMutex);
        return m_publishIntervalEmaMs;
    }

    ~DesktopDuplicationCapture();

private:
    bool CreateDuplication();

    // Four slots, so a frame the loop is still reading is never overwritten by
    // the next acquire.
    static constexpr int kSlotCount = 4;

    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<IDXGIOutput1> m_output;
    winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
    HMONITOR m_monitor = nullptr;

    winrt::com_ptr<ID3D11Texture2D> m_slotTex[kSlotCount];
    int64_t m_slotTimestamp100ns[kSlotCount]{};
    UINT m_width = 0, m_height = 0;

    mutable std::mutex m_slotMutex;
    int m_newestSlot = -1;
    int m_inUseSlot = -1;    // held by the main loop, never overwritten
    uint64_t m_newestSerial = 0, m_consumedSerial = 0;

    std::atomic<uint64_t> m_coalescedFrames{ 0 };
    std::atomic<uint64_t> m_cursorOnlyUpdates{ 0 };
    std::atomic<uint64_t> m_unchangedFrames{ 0 };
    std::atomic<bool> m_dirtyRectsAvailable{ false };
    std::vector<uint8_t> m_metadataBuffer;
    std::atomic<uint64_t> m_reconnects{ 0 };
    std::atomic<uint64_t> m_framesPublished{ 0 };
    uint64_t m_framesConsumed = 0;
    double m_publishIntervalEmaMs = -1.0;
    double m_lastPublishTimestampMs = 0.0;
    double m_nextReconnectAttemptMs = 0.0;
};

} // namespace FrameBoostBeta
