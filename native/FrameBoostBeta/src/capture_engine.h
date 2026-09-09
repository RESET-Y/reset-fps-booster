#pragma once
#include <windows.h>
#include <d3d11.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

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

    ~CaptureEngine();

private:
    bool StartFromItem(ID3D11Device* device); // shared setup for window and monitor capture

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::com_ptr<ID3D11Texture2D> m_lastFrameTex;
    winrt::com_ptr<ID3D11Device> m_device;
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
};

} // namespace FrameBoostBeta
