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
    // Same lock as Stop() and the drain. Building the pool and registering the
    // handler must not overlap a callback from a previous session, and the
    // handler can fire the moment it is registered - before the members below
    // it have finished being assigned. Checked for a nested Stop() first: there
    // is none, so this cannot deadlock on itself.
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    try {
        m_device.copy_from(device);
        // The immediate context is used from the pool.s worker thread as well
        // as from the main loop, so D3D11 has to be told - without this it is
        // explicitly not safe, and the failure mode is corruption rather than
        // an error.
        //
        // An earlier attempt at a capture thread collapsed the output to 3-7
        // FPS and this was blamed. It was not the cause: that thread used
        // Desktop Duplication with a blocking AcquireNextFrame, which is
        // documented to pause OTHER threads in the process until a frame is
        // available. Nothing blocks here - the work on this thread is one
        // CopyResource per frame.
        device->GetImmediateContext(m_context.put());
        if (auto multithread = m_context.try_as<ID3D11Multithread>())
            multithread->SetMultithreadProtected(TRUE);

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
        // Kept so the pool can be rebuilt at a new size without tearing the
        // whole session down - see CollectArrivedFrames.
        m_wrappedDevice = wrappedDevice;
        m_poolWidth = size.Width;
        m_poolHeight = size.Height;

        // Consumes the frame here, on the pool.s worker thread, instead of
        // leaving it for the main loop to fetch. See the ring buffer in the
        // header for why.
        m_frameArrivedRevoker = m_framePool.FrameArrived(winrt::auto_revoke,
            [this](auto&&, auto&&) {
                m_framesProduced.fetch_add(1, std::memory_order_relaxed);
                CollectArrivedFrames();

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
        // No capture border, and one less reason for the compositor to treat
        // this as a window that needs decorating.
        try { m_session.IsBorderRequired(false); } catch (...) {}

        // MinUpdateInterval = 1000 microseconds. NOT zero, and not less.
        //
        // The default of 0 - and any value below 1 ms - caps this API at
        // roughly 50 frames a second. It is a defect, not a documented limit:
        // 1000 us captures the display.s full rate while 999 us captures 50.
        // Sunshine and Apollo both carry the same one-line fix.
        //
        // This matters here because that cap is why this capture path was
        // abandoned. Measured against a game running at 90 FPS, Desktop
        // Duplication delivered 90.20 and this delivered 44.60, and the
        // conclusion drawn was that the API could not keep up. 44.60 sits
        // exactly on the documented broken value, so the measurement was
        // real and the conclusion was wrong.
        //
        // Worth retrying because Desktop Duplication has a structural problem
        // this one does not: it accumulates updates by design - Microsoft
        // states plainly that it "is not designed to capture every update" -
        // and roughly 25 frames a second arrive already merged, which is what
        // breaks the content timeline.
        //
        // Guarded because the property only exists on Windows 11 24H2 and
        // later. On older builds this throws and the capture runs as before.
        // Reached through the raw interface: the property lives on
        // IGraphicsCaptureSession5, which this SDK.s C++/WinRT projection does
        // not expose yet even though the ABI header declares it.
        {
            struct __declspec(uuid("67c0ea62-1f85-5061-925a-239be0ac09cb")) IGraphicsCaptureSession5
                : ::IInspectable
            {
                virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(INT64* value) = 0;
                virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(INT64 value) = 0;
            };

            // 1000 microseconds expressed in 100 ns units, which is what a
            // Windows.Foundation.TimeSpan carries.
            constexpr INT64 kOneMillisecondIn100ns = 10000;

            auto session5 = m_session.try_as<IGraphicsCaptureSession5>();
            if (session5 && SUCCEEDED(session5->put_MinUpdateInterval(kOneMillisecondIn100ns))) {
                Logger::Log("[FrameBoostBeta] WGC: MinUpdateInterval set to 1000 us - the default of 0"
                            " caps this API near 50 FPS.");
            } else {
                Logger::Log("[FrameBoostBeta] WGC: MinUpdateInterval unavailable (needs Windows 11 24H2);"
                            " capture may be capped near 50 FPS.");
            }
        }

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

namespace {
// Milliseconds on the performance counter, for the rebuild rate limit below.
double QpcNowMs() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return freq.QuadPart ? (1000.0 * static_cast<double>(now.QuadPart) / freq.QuadPart) : 0.0;
}
} // namespace

void CaptureEngine::CollectArrivedFrames() {
    // See m_lifecycleMutex: this runs on the capture worker thread and Stop()
    // runs on the main one.
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    if (!m_capturing || !m_framePool || !m_context) return;

    try {
        // Drain rather than take one: the pool can hold several by the time
        // this runs, and every one of them is a frame the game drew.
        for (;;) {
            auto frame = m_framePool.TryGetNextFrame();
            if (!frame) break;

            const auto contentSize = frame.ContentSize();
            if (contentSize.Width <= 0 || contentSize.Height <= 0) continue;

            // THE POOL HAS TO BE TOLD WHEN THE ITEM CHANGES SIZE.
            //
            // Recreate was never called here. The pool was built once, for the
            // size the window had at start, and nothing ever updated it -
            // ContentSize was read only to label our own slot textures.
            //
            // Microsoft names Recreate as the way to change "aspects of the
            // frame pool, including the size of frame buffers", and OSSS, which
            // does this same job with these same APIs, describes what happens
            // when it is skipped: "the frame pool was sized against a surface
            // that no longer exists, and the session never recovers on its
            // own."
            //
            // That is a precise description of the CS2 failure measured today -
            // frames still arriving, byte-identical across 114,012 sampled
            // offsets, while the game was visibly rendering - and it happens on
            // any resolution change, window resize or mode switch, not only
            // there.
            //
            // Recreate discards whatever is pending, which is why it is done
            // here and then the drain restarts: taking one more frame from a
            // pool that is about to be replaced would be reading a surface that
            // is going away.
            //
            // RATE-LIMITED, because an unbounded rebuild is worse than the bug.
            //
            // If an item ever reports a size that a Recreate does not settle -
            // DPI rounding, a window mid-resize, a driver quirk - this would
            // rebuild the pool on every single frame and the capture would stop
            // dead while looking busy. One rebuild a second is far more often
            // than any real size change happens, and cannot run away.
            if ((contentSize.Width != m_poolWidth || contentSize.Height != m_poolHeight)
                    && (QpcNowMs() - m_lastRecreateMs > 1000.0)) {
                const int32_t newW = contentSize.Width, newH = contentSize.Height;
                m_lastRecreateMs = QpcNowMs();
                frame = nullptr; // give the lease back before replacing the pool
                try {
                    m_framePool.Recreate(m_wrappedDevice,
                        DirectXPixelFormat::B8G8R8A8UIntNormalized, 6,
                        { newW, newH });
                    m_poolWidth = newW;
                    m_poolHeight = newH;
                    ++m_poolRecreates;
                    Logger::Log("[FrameBoostBeta] Capture item changed size to "
                        + std::to_string(newW) + "x" + std::to_string(newH)
                        + " - frame pool rebuilt.");
                } catch (...) {
                    Logger::Log("[FrameBoostBeta] Frame pool could not be rebuilt for the new size.");
                }
                continue;
            }

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            if (FAILED(access->GetInterface(IID_PPV_ARGS(tex.put())))) continue;

            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);

            int slot = -1;
            {
                std::lock_guard<std::mutex> lock(m_slotMutex);
                for (int i = 0; i < kSlotCount; ++i)
                    if (i != m_inUseSlot && i != m_newestSlot) { slot = i; break; }
                if (slot < 0) slot = (m_newestSlot + 1) % kSlotCount;
            }

            if (!m_slotTex[slot]) {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.MiscFlags = 0;
                if (FAILED(m_device->CreateTexture2D(&copyDesc, nullptr, m_slotTex[slot].put())))
                    continue;
            } else {
                D3D11_TEXTURE2D_DESC slotDesc{};
                m_slotTex[slot]->GetDesc(&slotDesc);
                if (slotDesc.Width != desc.Width || slotDesc.Height != desc.Height) {
                    m_slotTex[slot] = nullptr;
                    D3D11_TEXTURE2D_DESC copyDesc = desc;
                    copyDesc.Usage = D3D11_USAGE_DEFAULT;
                    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    copyDesc.CPUAccessFlags = 0;
                    copyDesc.MiscFlags = 0;
                    if (FAILED(m_device->CreateTexture2D(&copyDesc, nullptr, m_slotTex[slot].put())))
                        continue;
                }
            }

            // COPIED, not held.
            //
            // Holding the frame object instead would save a full-frame copy -
            // 14 MB at 1440p, 840 MB/s at 60 frames a second, bandwidth taken
            // from the game as much as from us - because the pool.s surface
            // stays valid as long as the frame is alive.
            //
            // It was tried and it stopped the capture dead: source FPS frozen
            // at one value, nothing retrieved, nothing generated. Holding four
            // frames out of a six-buffer pool leaves the pool unable to
            // continue, so "six minus four leaves two" does not hold in
            // practice. Noticed because the output suddenly felt TOO smooth -
            // which is what the raw game looks like when the booster has
            // quietly stopped.
            //
            // Worth retrying only with a much larger pool, and only with this
            // failure mode in mind.
            m_context->CopyResource(m_slotTex[slot].get(), tex.get());

            {
                std::lock_guard<std::mutex> lock(m_slotMutex);
                m_slotTimestamp100ns[slot] = frame.SystemRelativeTime().count();
                m_newestSlot = slot;
                ++m_newestSerial;
                m_width = static_cast<UINT>(contentSize.Width);
                m_height = static_cast<UINT>(contentSize.Height);
            }
            m_framesRetrieved.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        // A capture that fails must never take the engine down with it.
    }
}

ID3D11Texture2D* CaptureEngine::PollLatestFrame(UINT& outWidth, UINT& outHeight, int64_t& outFrameTimestamp100ns, bool& outIsNewFrame) {
    // Reads the ring that FrameArrived fills; it no longer talks to the frame
    // pool itself. The pool is drained on its own thread, so a slow turn of
    // the main loop no longer costs frames.
    outIsNewFrame = false;
    outWidth = m_width;
    outHeight = m_height;
    outFrameTimestamp100ns = 0;

    std::lock_guard<std::mutex> lock(m_slotMutex);
    if (m_newestSlot < 0) return nullptr;

    ID3D11Texture2D* tex = m_slotTex[m_newestSlot].get();
    outFrameTimestamp100ns = m_slotTimestamp100ns[m_newestSlot];

    if (m_newestSerial != m_consumedSerial) {
        // Everything between the last consumed serial and this one was
        // overtaken - counted so the cost of a slow loop stays visible.
        m_lastDiscardedStaleFrames =
            static_cast<int>(m_newestSerial - m_consumedSerial) - 1;
        m_consumedSerial = m_newestSerial;
        m_inUseSlot = m_newestSlot;
        outIsNewFrame = true;
    } else {
        m_lastDiscardedStaleFrames = 0;
    }
    return tex;
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
    // REVOKE FIRST, THEN TAKE THE LOCK, in that order and not the other way.
    //
    // Revoking stops new callbacks. Taking the lock afterwards waits for the
    // one that may already be running. Locking first would deadlock if revoke()
    // ever waits for a handler that is itself blocked on this lock.
    m_frameArrivedRevoker.revoke();

    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    m_capturing = false; // set before anything is torn down

    if (m_session) { try { m_session.Close(); } catch (...) {} }

    // Held frames must go back before the pool does, or the pool is closed
    // while we still own surfaces from it.
    {
        std::lock_guard<std::mutex> lock(m_slotMutex);
        for (auto& f : m_slotFrame) f = nullptr;
        for (auto& t : m_slotTex) t = nullptr;
        m_newestSlot = -1;
        m_inUseSlot = -1;
    }

    if (m_framePool) { try { m_framePool.Close(); } catch (...) {} }
    m_capturing = false;
}

CaptureEngine::~CaptureEngine() {
    Stop();
}

} // namespace FrameBoostBeta
