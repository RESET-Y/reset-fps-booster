// WINDOWS GRAPHICS CAPTURE, WITH A TIMELINE ATTACHED.
//
// The one job here that the old engine got wrong by omission: a captured
// picture is worthless to an interpolator unless it stays welded to the moment
// it was drawn. Every slot therefore carries its timestamps, and they are
// published in the same lock as the pixels.
//
// Two clocks, deliberately, because they answer different questions:
//
//   contentMs  - WGC's SystemRelativeTime, which is the compositor's stamp for
//                when the game drew it. This is the one the interpolation
//                phase is computed from. It is the game's timeline.
//   arrivalMs  - QueryPerformanceCounter at the moment we took delivery. This
//                is our timeline, and the difference between the two is the
//                capture latency - measurable rather than assumed.
//
// Both are in the QPC domain: WGC's SystemRelativeTime was confirmed
// empirically to share it (QueryInterruptTimePrecise was consistently ~14 ms
// off, a different clock entirely).
//
// FRAMES COME OUT IN ORDER, one at a time.
//
// The old engine exposed only "the newest slot" and silently discarded
// everything that arrived while the loop was busy - 34 frames a second at one
// point, visible in the telemetry only as a per-poll count of 1. An
// interpolator cannot pair frames it never saw, so this hands them over in
// sequence and counts what it has to drop when the ring genuinely overflows.
#pragma once

#include <d3d11.h>
#include <d3d11_4.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace fbv2 {

// A captured frame and the timeline it belongs to. The texture is owned by the
// ring; it stays valid between Acquire() and Release() of the same frame.
struct CapturedFrame {
    ID3D11Texture2D* texture = nullptr;
    uint64_t frameId = 0;
    double   contentMs = 0.0;   // game's own clock, from WGC
    double   arrivalMs = 0.0;   // ours, from QPC
    UINT     width = 0;
    UINT     height = 0;
    int      slot = -1;         // internal; do not interpret
};

class Capture {
public:
    ~Capture();

    // captureMonitorHz sets the delivery floor - see MinUpdateInterval in
    // capture.cpp. Pass the refresh rate of the monitor the target is on; 0
    // falls back to 60, which is what the API assumes anyway.
    bool StartWindow(HWND window, ID3D11Device* device, int captureMonitorHz);

    // THE WHOLE DISPLAY, as a control for the window path.
    //
    // Window capture goes through DWM. A game in borderless fullscreen can be
    // on independent flip, where DWM is not composing it at all, and what WGC
    // then hands over is a different and less reliable thing - this project
    // has already proved a game unrepresentable that way (CS2, byte-identical
    // frames across 114,012 sampled offsets).
    //
    // Monitor capture takes what is actually on the panel. If the window path
    // delivers 50 frames a second and the monitor path delivers 72 of the same
    // game, the loss is in window capture and not in the game. That is the only
    // way to tell those two apart from outside the game.
    bool StartMonitor(HMONITOR monitor, ID3D11Device* device, int captureMonitorHz);
    void Stop();
    bool IsCapturing() const { return m_capturing.load(std::memory_order_acquire); }

    // Takes the OLDEST frame not yet consumed, or returns false if there is
    // none. Every frame handed out must be given back with Release() before
    // the next Acquire(), which is what keeps the producer from overwriting a
    // slot that is still being read.
    bool Acquire(CapturedFrame& out);
    void Release(const CapturedFrame& frame);

    // Counters, reset per telemetry line.
    uint64_t Produced()  const { return m_produced.load(std::memory_order_relaxed); }
    uint64_t Consumed()  const { return m_consumed.load(std::memory_order_relaxed); }
    uint64_t Overflows() const { return m_overflows.load(std::memory_order_relaxed); }

    // THE THREE NUMBERS THAT DECIDE WHETHER THE FILTER IS RIGHT.
    //
    // Acquired is every frame WGC handed over. Published is what reached the
    // ring as a source frame. The two duplicate counters say why the rest did
    // not, and they are kept apart because they answer different questions:
    // a timestamp duplicate is certain, a fingerprint duplicate is a judgement
    // about pixels.
    uint64_t Acquired()   const { return m_acquired.load(std::memory_order_relaxed); }
    uint64_t DupTimestamp() const { return m_dupTimestamp.load(std::memory_order_relaxed); }
    uint64_t DupContent()   const { return m_dupContent.load(std::memory_order_relaxed); }
    double   FingerprintMsSum() const { return m_fpMsSum.load(std::memory_order_relaxed); }
    uint64_t FingerprintCount() const { return m_fpCount.load(std::memory_order_relaxed); }
    bool     FingerprintReady() const { return m_fpReady; }
    int      QueueDepth() const;
    void     ResetCounters();

    // EVERY ARRIVAL, RAW, straight off the WGC callback.
    //
    // Counters cannot answer "where did the frames go". An interval of 20.83
    // ms averages the same whether WGC delivered evenly at 48 or delivered at
    // 72 and something threw one in three away. Only the individual arrivals
    // can tell those apart, so they are recorded and handed over once a second
    // rather than summarised.
    //
    // Both clocks are kept separate and unprocessed: the WGC stamp is what the
    // compositor says about the frame, the QPC stamp is when we took delivery.
    // Nothing here is smoothed, clamped or rejected.
    void EnableTrace(bool on) { m_trace = on; }
    std::string TakeTrace();

    UINT Width()  const { return m_width.load(std::memory_order_relaxed); }
    UINT Height() const { return m_height.load(std::memory_order_relaxed); }

private:
    bool StartFromItem(ID3D11Device* device, int captureMonitorHz);
    void OnFrameArrived();
    void ProcessArrival(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame);
    void ReleaseSlots();

    // Eight is a deliberate number: deep enough to absorb the burst arrivals
    // that were measured on this machine (two frames 0.9 ms apart, then a 26 ms
    // gap, at a nominal 13.9 ms spacing), shallow enough that a consumer which
    // falls behind is caught by the overflow counter within a tenth of a second
    // rather than accumulating latency in silence.
    static constexpr int kSlots = 8;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem     m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_pool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession  m_session{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_arrivedRevoker;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_wrappedDevice{ nullptr };

    winrt::com_ptr<ID3D11Device>        m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;

    // THE SAME IMMEDIATE CONTEXT AS THE REST OF THE ENGINE, protected rather
    // than duplicated - and that is a measurement, not a shortcut.
    //
    // A deferred context was the obvious answer and is the wrong one here: a
    // deferred context records a command list that the IMMEDIATE context has to
    // execute, so the capture thread would end up waiting on the render loop to
    // run its copy. That is more coupling, not less.
    //
    // The question it was meant to solve was measured directly instead. With
    // the copy timed on the capture thread at a 72 fps source:
    //
    //   Capture blocked: 0.00-0.01 ms avg, 0.25 ms max
    //   Present wait:    0.00 ms
    //
    // The capture thread is not waiting for anything. If that ever stops being
    // true the honest fix is a second D3D11 device with shared textures, not a
    // deferred context.
    winrt::com_ptr<ID3D11Texture2D> m_slotTex[kSlots];
    uint64_t m_slotFrameId[kSlots]{};
    double   m_slotContentMs[kSlots]{};
    double   m_slotArrivalMs[kSlots]{};

    mutable std::mutex m_ringMutex;
    uint64_t m_head = 0;         // next slot to write
    uint64_t m_tail = 0;         // next slot to read
    int      m_inUseSlot = -1;   // held by the consumer between Acquire/Release

    std::mutex m_lifecycleMutex;
    std::atomic<bool> m_capturing{ false };
    std::atomic<UINT> m_width{ 0 }, m_height{ 0 };
    std::atomic<uint64_t> m_produced{ 0 }, m_consumed{ 0 }, m_overflows{ 0 };
    uint64_t m_nextFrameId = 1;

    UINT m_poolWidth = 0, m_poolHeight = 0;

    // STAGE 1: the free, exact test.
    //
    // WGC stamps every frame with the compositor time of the picture it holds.
    // Re-delivering the same composed frame re-uses that stamp exactly, which
    // showed up in the trace the moment MinUpdateInterval was lowered:
    //
    //   dWgc = 13.888   a real new frame
    //   dWgc =  0.000   the same frame again
    //
    // Equality is proof. A SHORT gap is not - 6.944 ms is half a source
    // interval and can carry genuinely new content, so nothing here thresholds
    // on time.
    double m_lastArrivalContentMs = -1.0;

    // STAGE 2: the fingerprint, on the GPU.
    //
    // Catches the other kind: a different timestamp carrying an identical
    // picture, because DWM recomposed a frame the game did not redraw.
    //
    // MEASURES BEFORE IT FILTERS, deliberately. The readback is asynchronous,
    // so a fingerprint is only readable once the NEXT frame arrives - filtering
    // on it would mean holding every frame back one arrival, about 13.9 ms at
    // 72 fps. That is a real latency cost to pay for a class of duplicate we
    // have not yet seen a single instance of. So it counts them first; if the
    // count is zero the cost is never paid, and if it is not, the number says
    // what the trade would buy.
    static constexpr int kFpSlots = 3;
    static constexpr UINT kFpLanes = 4;
    winrt::com_ptr<ID3D11ComputeShader>       m_fpShader;
    winrt::com_ptr<ID3D11Buffer>              m_fpBuffer;      // 4 uints, GPU
    winrt::com_ptr<ID3D11UnorderedAccessView> m_fpUAV;
    winrt::com_ptr<ID3D11Buffer>              m_fpParams;
    winrt::com_ptr<ID3D11Buffer>              m_fpStaging[kFpSlots];
    winrt::com_ptr<ID3D11ShaderResourceView>  m_fpSlotSRV[kSlots];
    uint64_t m_fpSeq = 0;
    bool     m_fpReady = false;
    uint32_t m_fpPrev[kFpLanes]{};
    bool     m_fpHavePrev = false;
    std::atomic<double>   m_fpMsSum{ 0.0 };
    std::atomic<uint64_t> m_fpCount{ 0 };
    std::atomic<uint64_t> m_acquired{ 0 }, m_dupTimestamp{ 0 }, m_dupContent{ 0 };

    bool EnsureFingerprint(const D3D11_TEXTURE2D_DESC& desc);
    // Dispatches the hash for one slot and starts its copy back. Returns the
    // staging index the result will land in, or -1 if unavailable.
    int  RunFingerprint(int slot, const D3D11_TEXTURE2D_DESC& desc);
    bool TryReadFingerprint(int stagingSlot, uint32_t out[4]);
    void ResolvePending();

    // ONE FRAME IS ALWAYS HELD BACK, and this is why.
    //
    // A fingerprint is only readable once the next frame arrives - the copy
    // back from the GPU is asynchronous by design, because waiting for it
    // would be the per-frame stall this whole approach exists to avoid.
    //
    // So a frame cannot be judged at the moment it arrives. It waits in the
    // slot at m_head, invisible to the consumer, until the next arrival brings
    // its verdict: published if it carries new content, dropped where it lies
    // if it does not. A duplicate therefore never becomes a source frame and
    // never reaches the screen.
    //
    // The cost is one arrival of latency, about 13.9 ms at 72 fps, on top of
    // the half interval interpolation needs. That is the price of the
    // requirement, and it is paid in full rather than half-paid by letting one
    // duplicate through.
    //
    // Published frames are [m_tail, m_head). The pending one sits AT m_head,
    // so publishing it is a single increment and dropping it is doing nothing -
    // the next arrival simply writes over the same slot.
    bool     m_havePending = false;
    int      m_pendingFpSlot = -1;
    uint32_t m_lastPubFp[4]{};
    bool     m_haveLastPubFp = false;

    bool m_trace = false;
    std::mutex m_traceMutex;
    std::string m_traceBuffer;
    double m_lastTraceWgcMs = 0.0, m_lastTraceQpcMs = 0.0;
};

// QueryPerformanceCounter in milliseconds, monotonic, shared by every
// timestamp in this engine.
double NowMs();

} // namespace fbv2
