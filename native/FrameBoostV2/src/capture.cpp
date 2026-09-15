#include "capture.h"
#include "logger.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>

#include <string>

using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace fbv2 {

namespace {

// The documented bridge between a plain D3D11 device and the WinRT capture
// API. Nothing clever, and the only place this engine touches WinRT interop.
IDirect3DDevice WrapD3DDevice(ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

LARGE_INTEGER QpcFrequency() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
}

} // namespace

double NowMs() {
    static const LARGE_INTEGER freq = QpcFrequency();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return freq.QuadPart ? (static_cast<double>(t.QuadPart) * 1000.0
                            / static_cast<double>(freq.QuadPart))
                         : 0.0;
}

Capture::~Capture() { Stop(); }

bool Capture::StartMonitor(HMONITOR monitor, ID3D11Device* device) {
    if (!monitor || !device) {
        Logger::Log("[FrameBoostV2] Capture: no usable monitor handle.");
        return false;
    }

    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    try {
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForMonitor(
            monitor, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(m_item)));
    } catch (...) {
        Logger::Log("[FrameBoostV2] Capture: CreateForMonitor failed.");
        return false;
    }
    return StartFromItem(device);
}

bool Capture::StartWindow(HWND window, ID3D11Device* device) {
    if (!window || !IsWindow(window) || !device) {
        Logger::Log("[FrameBoostV2] Capture: no usable window handle.");
        return false;
    }

    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);

    try {
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(
            window, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(m_item)));
    } catch (...) {
        Logger::Log("[FrameBoostV2] Capture: CreateForWindow failed.");
        return false;
    }

    return StartFromItem(device);
}

// Everything after the item exists is identical for a window and a monitor.
bool Capture::StartFromItem(ID3D11Device* device) {
    m_device.copy_from(device);
    device->GetImmediateContext(m_context.put());
    // See the comment on m_slotTex in the header: protected, not duplicated,
    // and that choice rests on a measurement rather than on preference.
    if (auto multithread = m_context.try_as<ID3D11Multithread>())
        multithread->SetMultithreadProtected(TRUE);

    m_wrappedDevice = WrapD3DDevice(device);

    const auto size = m_item.Size();
    m_poolWidth = static_cast<UINT>(size.Width);
    m_poolHeight = static_cast<UINT>(size.Height);
    m_width.store(m_poolWidth, std::memory_order_relaxed);
    m_height.store(m_poolHeight, std::memory_order_relaxed);

    // SIX BUFFERS, NOT TWO, and the reason is measured rather than argued.
    //
    // The comment that stood here said two was the documented minimum and all
    // this needs, because the ring below is where depth belongs. That was
    // wrong, and V1 had already proved it wrong in a comment I moved into the
    // legacy tree with my own hands:
    //
    //   "With two, a source producing frames faster than we poll silently
    //    loses the ones that do not fit - and silently is the problem: nothing
    //    in the API reports it, so the frames we DO get look evenly spaced and
    //    the source looks slower than it is. Measured in a game reporting 75
    //    FPS internally: WGC handed us 52, spaced 19.2 ms."
    //
    // Apex reports 72 and arrives at 48-54, spaced 18.6-20.8 ms. The same
    // signature, to the millisecond.
    //
    // The ring cannot cover for this. It holds frames AFTER the pool hands
    // them over; a frame the pool never issued because it had no free buffer
    // never reaches the ring at all, and no counter here or anywhere else says
    // so. That silence is what sent the last hour looking at the game, at the
    // window mode, at the compositor and at monitor capture - all of which
    // agreed, because all of them were downstream of this line.
    m_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_wrappedDevice,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        6,
        size);

    m_session = m_pool.CreateCaptureSession(m_item);

    // No yellow border where the OS allows it to be turned off. Older builds
    // simply do not have the property; that is not an error.
    try {
        if (auto s3 = m_session.try_as<IGraphicsCaptureSession3>())
            s3.IsBorderRequired(false);
    } catch (...) {}

    {
        std::lock_guard<std::mutex> ring(m_ringMutex);
        m_head = m_tail = 0;
        m_inUseSlot = -1;
        m_nextFrameId = 1;
        for (auto& t : m_slotTex) t = nullptr;
    }

    m_arrivedRevoker = m_pool.FrameArrived(
        winrt::auto_revoke,
        [this](auto&&, auto&&) { OnFrameArrived(); });

    m_capturing.store(true, std::memory_order_release);
    m_session.StartCapture();

    Logger::Log("[FrameBoostV2] Capture started at "
                + std::to_string(m_poolWidth) + "x"
                + std::to_string(m_poolHeight) + ".");
    return true;
}

void Capture::OnFrameArrived() {
    if (!m_capturing.load(std::memory_order_acquire)) return;

    Direct3D11CaptureFrame frame{ nullptr };
    try {
        frame = m_pool.TryGetNextFrame();
    } catch (...) {
        return;
    }
    if (!frame) return;

    // A WGC frame is a LEASE, not a copy: the surface goes back to the pool the
    // moment this object dies, and the pool may hand the same memory out again.
    // Everything that has to outlive this scope is copied out first.
    const double arrivalMs = NowMs();
    const double contentMs = static_cast<double>(frame.SystemRelativeTime().count()) / 10000.0;

    const auto contentSize = frame.ContentSize();
    const UINT w = static_cast<UINT>(contentSize.Width);
    const UINT h = static_cast<UINT>(contentSize.Height);

    winrt::com_ptr<ID3D11Texture2D> source;
    try {
        auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::
                                        IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(source.put())));
    } catch (...) {
        return;
    }
    if (!source) return;

    D3D11_TEXTURE2D_DESC srcDesc{};
    source->GetDesc(&srcDesc);

    int slot = -1;
    {
        std::lock_guard<std::mutex> ring(m_ringMutex);

        // FULL: drop the OLDEST, never the newest. A frame generator that keeps
        // the stale end of a backlog would be replaying the past; the whole
        // point of a bounded ring is that it stays near live. Counted, because
        // an overflow means the consumer is behind and that has to be visible.
        if (m_head - m_tail >= static_cast<uint64_t>(kSlots)) {
            const int oldest = static_cast<int>(m_tail % kSlots);
            if (oldest == m_inUseSlot) {
                // The consumer is holding the oldest frame right now. Dropping
                // it would pull the texture out from under a running
                // ProcessFrame, so this arrival is the one that goes instead.
                m_overflows.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            ++m_tail;
            m_overflows.fetch_add(1, std::memory_order_relaxed);
        }
        slot = static_cast<int>(m_head % kSlots);
    }

    // Allocate or resize the slot outside the ring lock: the arrival thread is
    // the only writer of slot textures, and the consumer never touches a slot
    // that is not between tail and head.
    bool needNew = true;
    if (m_slotTex[slot]) {
        D3D11_TEXTURE2D_DESC have{};
        m_slotTex[slot]->GetDesc(&have);
        needNew = (have.Width != srcDesc.Width || have.Height != srcDesc.Height
                   || have.Format != srcDesc.Format);
    }
    if (needNew) {
        D3D11_TEXTURE2D_DESC desc = srcDesc;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> fresh;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, fresh.put()))) return;
        m_slotTex[slot] = fresh;
    }

    m_context->CopyResource(m_slotTex[slot].get(), source.get());

    {
        std::lock_guard<std::mutex> ring(m_ringMutex);
        m_slotFrameId[slot]  = m_nextFrameId++;
        m_slotContentMs[slot] = contentMs;
        m_slotArrivalMs[slot] = arrivalMs;
        ++m_head;
    }

    m_width.store(w ? w : srcDesc.Width, std::memory_order_relaxed);
    m_height.store(h ? h : srcDesc.Height, std::memory_order_relaxed);
    m_produced.fetch_add(1, std::memory_order_relaxed);
}

bool Capture::Acquire(CapturedFrame& out) {
    std::lock_guard<std::mutex> ring(m_ringMutex);
    if (m_inUseSlot >= 0) return false;   // previous frame not released yet
    if (m_tail >= m_head) return false;   // nothing waiting

    const int slot = static_cast<int>(m_tail % kSlots);
    if (!m_slotTex[slot]) { ++m_tail; return false; }

    out.texture   = m_slotTex[slot].get();
    out.frameId   = m_slotFrameId[slot];
    out.contentMs = m_slotContentMs[slot];
    out.arrivalMs = m_slotArrivalMs[slot];
    out.width     = m_width.load(std::memory_order_relaxed);
    out.height    = m_height.load(std::memory_order_relaxed);
    out.slot      = slot;

    m_inUseSlot = slot;
    return true;
}

void Capture::Release(const CapturedFrame& frame) {
    std::lock_guard<std::mutex> ring(m_ringMutex);
    if (frame.slot != m_inUseSlot) return;
    m_inUseSlot = -1;
    ++m_tail;
    m_consumed.fetch_add(1, std::memory_order_relaxed);
}

int Capture::QueueDepth() const {
    std::lock_guard<std::mutex> ring(m_ringMutex);
    return static_cast<int>(m_head - m_tail);
}

void Capture::ResetCounters() {
    m_produced.store(0, std::memory_order_relaxed);
    m_consumed.store(0, std::memory_order_relaxed);
    m_overflows.store(0, std::memory_order_relaxed);
}

void Capture::ReleaseSlots() {
    std::lock_guard<std::mutex> ring(m_ringMutex);
    for (auto& t : m_slotTex) t = nullptr;
    m_head = m_tail = 0;
    m_inUseSlot = -1;
}

void Capture::Stop() {
    // REVOKE FIRST, THEN LOCK, in that order. Revoking stops new callbacks;
    // taking the lock afterwards waits for one that may already be running.
    // Locking first can deadlock against a handler blocked on the same lock.
    m_arrivedRevoker.revoke();

    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    if (!m_capturing.exchange(false, std::memory_order_acq_rel)) return;

    if (m_session) { try { m_session.Close(); } catch (...) {} }
    ReleaseSlots();
    if (m_pool) { try { m_pool.Close(); } catch (...) {} }

    m_session = nullptr;
    m_pool = nullptr;
    m_item = nullptr;
    m_wrappedDevice = nullptr;
    m_context = nullptr;
    m_device = nullptr;

    Logger::Log("[FrameBoostV2] Capture stopped.");
}

} // namespace fbv2
