#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

namespace FrameBoostBeta {

// Wraps the Windows Graphics Capture API for ONE target window. This is the
// entire reason the beta can be "system-level" instead of per-game: it
// captures whatever the OS compositor has already produced for that window,
// via a public, documented, user-mode-only API - no injection into the
// target process, no reading its memory, nothing that could look like
// game-hacking. If the target window closes or capture fails for any
// reason, this fails safely (returns false / null) rather than throwing.
class CaptureEngine {
public:
    // device: OUR OWN D3D11 device (shared with the presenter and the
    // motion-estimation/interpolation pipeline so all this stays GPU-side,
    // no cross-device copies).
    bool Start(HWND targetWindow, ID3D11Device* device);

    // Captures a whole monitor instead of a single window.
    //
    // Window capture has a hard limitation that no amount of overlay
    // tuning fixes: Windows stops redrawing a window that is fully covered,
    // so capturing a window while displaying over it starves the capture -
    // observed live as the image freezing until some event forced a repaint.
    // A monitor is always being composited, so monitor capture keeps
    // delivering frames. The presenter's own window is excluded from
    // capture (WDA_EXCLUDEFROMCAPTURE) to avoid capturing our own output.
    bool StartMonitor(HMONITOR monitor, ID3D11Device* device);

    void Stop();

    // Non-blocking: returns the most recently captured frame as a real GPU
    // texture, or nullptr if no new frame is available yet. Caller does NOT
    // own the returned pointer's lifetime beyond this call - copy out of it
    // immediately if needed past the next PollLatestFrame() call.
    //
    // outFrameTimestamp100ns: the frame's real WGC-reported capture
    // timestamp (100ns ticks since system boot, same clock domain as
    // QueryInterruptTimePrecise) - this is what makes real additional-
    // latency measurement possible, as opposed to just timing how long
    // retrieving an already-ready frame took.
    //
    // outIsNewFrame: false means the target application has not produced a
    // new frame since the last call and the returned texture is the previous
    // one. The caller MUST NOT treat that as a fresh frame - doing so was a
    // real bug: motion estimation then compared a frame against itself
    // (yielding a zero motion field), overwrote its own previous-frame
    // reference with a duplicate, and presented duplicate frames. That
    // combination produced exactly the heavy judder observed in the first
    // overlay test.
    ID3D11Texture2D* PollLatestFrame(UINT& outWidth, UINT& outHeight, int64_t& outFrameTimestamp100ns, bool& outIsNewFrame);

    bool IsCapturing() const { return m_capturing; }

    // How many already-stale frames the last poll had to throw away to reach
    // the newest one. Consistently above zero means the processing loop is
    // running slower than the target application renders - real, measurable
    // evidence of where added latency is coming from, not a guess.
    int LastDiscardedStaleFrames() const { return m_lastDiscardedStaleFrames; }

private:
    // Runs on the frame pool.s worker thread: drains the pool and copies each
    // frame into the ring. Declared here, used from the FrameArrived handler.
    void CollectArrivedFrames();

public:

    // --- Pipeline audit counters -------------------------------------------
    //
    // The one number the engine never had: how many frames Windows Graphics
    // Capture actually PRODUCED. Everything downstream was measured against
    // what we managed to read, so a frame the pool overwrote before we got to
    // it was indistinguishable from a frame the game never rendered - and the
    // difference between those two decides whether the bottleneck is ours or
    // the source's.
    //
    // FrameArrived is subscribed purely as a counter here; it never consumes
    // a frame, so polling behaves exactly as before. Free-threaded pool, so
    // the handler runs on a WGC worker thread - hence the atomics.
    uint64_t FramesProduced() const { return m_framesProduced.load(std::memory_order_relaxed); }
    uint64_t FramesRetrieved() const { return m_framesRetrieved.load(std::memory_order_relaxed); }

    // Produced minus retrieved: frames that existed in the pool and were
    // recycled before we read them. This is the direct measurement of "are we
    // too slow to drain the pool", which no WGC API reports.
    uint64_t FramesLostInPool() const {
        const uint64_t produced = FramesProduced(), retrieved = FramesRetrieved();
        return produced > retrieved ? produced - retrieved : 0;
    }

    // Interval statistics over the FrameArrived signals themselves: the
    // spacing at which WGC announced frames, not the spacing at which we got
    // around to reading them. Timed with QPC in the handler, because the
    // frame's own SystemRelativeTime can only be read by taking the frame out
    // of the pool - which the audit must not do, or it would change the very
    // behaviour it is measuring. Read timestamps carry SystemRelativeTime;
    // these two together bracket where spacing is introduced.
    struct IntervalStats {
        int samples = 0;
        double meanMs = 0.0, minMs = 0.0, maxMs = 0.0, stdDevMs = 0.0;
    };
    IntervalStats ProducedIntervalStats() const;
    void ResetAuditCounters();

    // Number of buffers the frame pool currently holds.
    int PoolBufferCount() const { return m_poolBufferCount; }
    // How often the frame pool had to be rebuilt for a size change.
    uint64_t PoolRecreates() const { return m_poolRecreates; }

    ~CaptureEngine();

private:
    bool StartFromItem(ID3D11Device* device); // shared setup for window and monitor capture

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    // The device the pool was created with, and the size it was created FOR.
    // Both are needed to call Recreate when the captured item changes size -
    // see CollectArrivedFrames.
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_wrappedDevice{ nullptr };
    int32_t m_poolWidth = 0;
    int32_t m_poolHeight = 0;
    uint64_t m_poolRecreates = 0;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::com_ptr<ID3D11Texture2D> m_lastFrameTex;
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;

    // Frames are COPIED OUT in FrameArrived, on the pool.s own worker thread,
    // into this ring - rather than being fetched when the main loop happens to
    // ask for one.
    //
    // That is the whole reason for moving to this capture path. Desktop
    // Duplication has to be asked, and while the loop spends 4-10 ms
    // generating and presenting a frame it is not asking, so Windows merges
    // what arrives in the meantime: 25 frames a second, measured, against a
    // compositor producing one every 6.9 ms. Here Windows delivers, and the
    // delivery does not care what the main thread is doing.
    //
    // Four slots, with the one the consumer holds protected, so a frame being
    // read is never overwritten underneath it.
    // The captured frames are HELD, not copied.
    //
    // A full-frame copy is 14 MB at 1440p, and at 60 frames a second that is
    // 840 MB/s of pure copying - bandwidth taken from the game as much as from
    // us. The frame pool owns its surfaces and recycles them when the frame
    // object is released, so keeping the frame object alive keeps the surface
    // valid and it can be bound directly.
    //
    // Safe because the pool has six buffers and this holds at most four, and
    // because a slot is never reused while the consumer is reading it.
    static constexpr int kSlotCount = 4;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_slotFrame[kSlotCount]
        { nullptr, nullptr, nullptr, nullptr };
    winrt::com_ptr<ID3D11Texture2D> m_slotTex[kSlotCount];
    int64_t m_slotTimestamp100ns[kSlotCount] = {};
    int m_newestSlot = -1;
    int m_inUseSlot = -1;
    uint64_t m_newestSerial = 0;
    uint64_t m_consumedSerial = 0;
    std::mutex m_slotMutex;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_capturing = false;
    // The size the frame pool's buffers are CURRENTLY allocated at. Windows
    // Graphics Capture does not automatically resize the pool when the
    // target window is resized - without tracking this and calling
    // Recreate() on a mismatch, a resized window either stretches into
    // stale-sized buffers or (as found on the first live Watch Dogs test)
    // can cause severe performance cliffs from the resulting size churn
    // propagating into the motion-estimation resource allocator downstream.
    winrt::Windows::Graphics::SizeInt32 m_poolSize{};
    int m_lastDiscardedStaleFrames = 0;
    int m_poolBufferCount = 0;

    // Audit state. The FrameArrived handler writes m_framesProduced and the
    // produced-interval accumulators from a WGC worker thread; the polling
    // thread reads them.
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrivedRevoker;
    std::atomic<uint64_t> m_framesProduced{ 0 };
    std::atomic<uint64_t> m_framesRetrieved{ 0 };
    mutable std::mutex m_producedIntervalMutex;
    int64_t m_lastProducedTimestamp100ns = 0;
    std::vector<double> m_producedIntervalsMs;
};

} // namespace FrameBoostBeta
