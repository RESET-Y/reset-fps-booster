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

    bool StartWindow(HWND window, ID3D11Device* device);

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
    bool StartMonitor(HMONITOR monitor, ID3D11Device* device);
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
    bool StartFromItem(ID3D11Device* device);
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

    bool m_trace = false;
    std::mutex m_traceMutex;
    std::string m_traceBuffer;
    double m_lastTraceWgcMs = 0.0, m_lastTraceQpcMs = 0.0;
};

// QueryPerformanceCounter in milliseconds, monotonic, shared by every
// timestamp in this engine.
double NowMs();

} // namespace fbv2
