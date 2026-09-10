#include "capture_engine.h"
#include "logger.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <string>
#include <cmath>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace FrameBoostBeta {

namespace {

// Wraps our own ID3D11Device as a WinRT IDirect3DDevice - the standard
// bridge documented by Microsoft for using Graphics Capture with a plain
// Win32 D3D11 device instead of a WinRT-native one.
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
WrapD3DDevice(ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

} // namespace

bool CaptureEngine::StartMonitor(HMONITOR monitor, ID3D11Device* device) {
    if (!monitor) {
        Logger::Log("[FrameBoostBeta] CaptureEngine: monitor handle is invalid.");
        return false;
    }
    try {
        auto interopFactory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interopFactory->CreateForMonitor(
            monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            reinterpret_cast<void**>(winrt::put_abi(m_item))));
        Logger::Log("[FrameBoostBeta] Capturing an entire monitor (window capture stalls when the window is covered).");
        return StartFromItem(device);
    } catch (const winrt::hresult_error& ex) {
        Logger::Log("[FrameBoostBeta] CaptureEngine::StartMonitor failed: " + winrt::to_string(ex.message()));
        m_capturing = false;
        return false;
    } catch (...) {
        Logger::Log("[FrameBoostBeta] CaptureEngine::StartMonitor failed with an unknown exception.");
        m_capturing = false;
        return false;
    }
}

bool CaptureEngine::Start(HWND targetWindow, ID3D11Device* device) {
    if (!targetWindow || !IsWindow(targetWindow)) {
        Logger::Log("[FrameBoostBeta] CaptureEngine: target window handle is invalid.");
        return false;
    }

    try {
        auto interopFactory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interopFactory->CreateForWindow(
            targetWindow, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            reinterpret_cast<void**>(winrt::put_abi(m_item))));
        Logger::Log("[FrameBoostBeta] Capture started for window handle "
            + std::to_string(reinterpret_cast<uintptr_t>(targetWindow)));
        return StartFromItem(device);
    } catch (const winrt::hresult_error& ex) {
        Logger::Log("[FrameBoostBeta] CaptureEngine::Start failed: " + winrt::to_string(ex.message()));
        m_capturing = false;
        return false;
    } catch (...) {
        Logger::Log("[FrameBoostBeta] CaptureEngine::Start failed with an unknown exception.");
        m_capturing = false;
        return false;
    }
}

bool CaptureEngine::StartFromItem(ID3D11Device* device) {
    try {
        m_device.copy_from(device);
        auto wrappedDevice = WrapD3DDevice(device);
        auto size = m_item.Size();
        m_poolSize = size;

        // CreateFreeThreaded: no DispatcherQueue required, since this class
        // polls for frames itself instead of subscribing to FrameArrived -
        // keeps this a plain Win32 app with no UI-thread/COM apartment
        // ceremony beyond winrt::init_apartment() at process start.
        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            // Six buffers, not two. With two, a source producing frames faster
            // than we poll silently loses the ones that do not fit - and
            // silently is the problem: nothing in the API reports it, so the
            // frames we DO get look evenly spaced and the source looks slower
            // than it is. Measured in a game reporting 75 FPS internally:
            // WGC handed us 52, spaced 19.2 ms, with no dropped-frame count to
            // show for it.
            wrappedDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 6, size);
        m_poolBufferCount = 6;

        // Counter only - deliberately does NOT call TryGetNextFrame, so the
        // polling path behaves exactly as it did before this was added.
        m_frameArrivedRevoker = m_framePool.FrameArrived(winrt::auto_revoke,
            [this](auto&&, auto&&) {
                m_framesProduced.fetch_add(1, std::memory_order_relaxed);

                LARGE_INTEGER now{}, freq{};
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&freq);
                const int64_t nowTicks = now.QuadPart;

                std::lock_guard<std::mutex> lock(m_producedIntervalMutex);
                if (m_lastProducedTimestamp100ns != 0 && m_producedIntervalsMs.size() < 4096) {
                    m_producedIntervalsMs.push_back(
                        1000.0 * static_cast<double>(nowTicks - m_lastProducedTimestamp100ns) / freq.QuadPart);
                }
                m_lastProducedTimestamp100ns = nowTicks;
            });

        m_session = m_framePool.CreateCaptureSession(m_item);

        // Best-effort - not all Windows versions/hardware support hiding
        // the cursor from capture; failing to set this is not fatal.
        try { m_session.IsCursorCaptureEnabled(false); } catch (...) {}

        m_session.StartCapture();
        m_capturing = true;
        return true;
    } catch (const winrt::hresult_error& ex) {
        Logger::Log("[FrameBoostBeta] CaptureEngine::StartFromItem failed: "
            + winrt::to_string(ex.message()));
        m_capturing = false;
        return false;
    } catch (...) {
        Logger::Log("[FrameBoostBeta] CaptureEngine::StartFromItem failed with an unknown exception.");
        m_capturing = false;
        return false;
    }
}

ID3D11Texture2D* CaptureEngine::PollLatestFrame(UINT& outWidth, UINT& outHeight, int64_t& outFrameTimestamp100ns, bool& outIsNewFrame) {
    outIsNewFrame = false;
    if (!m_capturing || !m_framePool) return nullptr;

    try {
        // Drain the pool to the NEWEST available frame instead of taking one
        // frame per call. Our processing loop runs slower than the target
        // application renders, so taking a single queued frame per tick
        // built up a backlog of stale frames - measured at ~78ms of pure
        // "the picture you are looking at is already old" latency during
        // the first live A/B test, which is exactly what still felt laggy
        // after the presentation-latency fix. Everything older than the
        // newest frame is deliberately discarded: showing an old frame has
        // no value, and dropping them is what keeps latency bounded.
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };
        int discardedStaleFrames = -1;
        for (;;) {
            auto next = m_framePool.TryGetNextFrame();
            if (!next) break;
            frame = next;
            ++discardedStaleFrames;
        }
        if (!frame) { outFrameTimestamp100ns = 0; return m_lastFrameTex.get(); } // no new frame yet
        m_lastDiscardedStaleFrames = discardedStaleFrames;

        auto contentSize = frame.ContentSize();

        // The real fix for the resolution-transition performance cliff seen
        // on the first live test: when the captured window's content size
        // no longer matches the frame pool's allocated buffer size (window
        // resize, fullscreen/windowed transition, etc.), recreate the pool
        // at the new size BEFORE reading this frame's surface, instead of
        // silently working with mismatched dimensions frame after frame.
        if (contentSize.Width != m_poolSize.Width || contentSize.Height != m_poolSize.Height) {
            Logger::Log("[FrameBoostBeta] Capture content size changed "
                + std::to_string(m_poolSize.Width) + "x" + std::to_string(m_poolSize.Height) + " -> "
                + std::to_string(contentSize.Width) + "x" + std::to_string(contentSize.Height)
                + " - recreating frame pool.");
            auto wrappedDevice = WrapD3DDevice(m_device.get());
            // Six buffers here too. This path used to recreate the pool with
            // TWO, silently undoing the depth chosen at startup - and it runs
            // on exactly the transitions worth measuring: a resolution change,
            // or a game moving between windowed, borderless and exclusive
            // fullscreen. Every measurement taken after such a switch was
            // therefore taken on a two-buffer pool.
            m_framePool.Recreate(wrappedDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 6, contentSize);
            m_poolSize = contentSize;
            m_poolBufferCount = 6;
        }

        auto surface = frame.Surface();
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));

        outWidth = static_cast<UINT>(contentSize.Width);
        outHeight = static_cast<UINT>(contentSize.Height);
        // SystemRelativeTime is a TimeSpan in 100ns ticks since system boot -
        // the real moment WGC captured this frame, not when we got around
        // to polling for it.
        outFrameTimestamp100ns = frame.SystemRelativeTime().count();

        m_lastFrameTex = tex;
        outIsNewFrame = true;
        // Everything the drain loop above pulled out counts as retrieved, not
        // just the newest one: the stale ones were read by us and thrown away
        // by us, which is a different loss from the pool overwriting them.
        m_framesRetrieved.fetch_add(static_cast<uint64_t>(discardedStaleFrames) + 1, std::memory_order_relaxed);
        return m_lastFrameTex.get();
    } catch (const winrt::hresult_error& ex) {
        Logger::Log("[FrameBoostBeta] PollLatestFrame failed: " + winrt::to_string(ex.message()));
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

CaptureEngine::IntervalStats CaptureEngine::ProducedIntervalStats() const {
    IntervalStats stats;
    std::lock_guard<std::mutex> lock(m_producedIntervalMutex);
    if (m_producedIntervalsMs.empty()) return stats;

    stats.samples = static_cast<int>(m_producedIntervalsMs.size());
    stats.minMs = stats.maxMs = m_producedIntervalsMs.front();
    double sum = 0.0;
    for (double v : m_producedIntervalsMs) {
        sum += v;
        if (v < stats.minMs) stats.minMs = v;
        if (v > stats.maxMs) stats.maxMs = v;
    }
    stats.meanMs = sum / stats.samples;

    double variance = 0.0;
    for (double v : m_producedIntervalsMs) variance += (v - stats.meanMs) * (v - stats.meanMs);
    stats.stdDevMs = std::sqrt(variance / stats.samples);
    return stats;
}

void CaptureEngine::ResetAuditCounters() {
    m_framesProduced.store(0, std::memory_order_relaxed);
    m_framesRetrieved.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_producedIntervalMutex);
    m_producedIntervalsMs.clear();
}

void CaptureEngine::Stop() {
    if (m_session) { try { m_session.Close(); } catch (...) {} }
    m_frameArrivedRevoker.revoke();
    if (m_framePool) { try { m_framePool.Close(); } catch (...) {} }
    m_capturing = false;
}

CaptureEngine::~CaptureEngine() {
    Stop();
}

} // namespace FrameBoostBeta
