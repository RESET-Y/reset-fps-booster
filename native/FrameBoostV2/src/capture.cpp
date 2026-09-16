#include "capture.h"
#include "logger.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#include "frame_fingerprint_cs.h"

#include <cstdio>
#include <cstring>
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

bool Capture::StartMonitor(HMONITOR monitor, ID3D11Device* device, int captureMonitorHz) {
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
    return StartFromItem(device, captureMonitorHz);
}

bool Capture::StartWindow(HWND window, ID3D11Device* device, int captureMonitorHz) {
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

    return StartFromItem(device, captureMonitorHz);
}

// Everything after the item exists is identical for a window and a monitor.
bool Capture::StartFromItem(ID3D11Device* device, int captureMonitorHz) {
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

    // THE DELIVERY FLOOR. This was the whole of "Apex renders 72 and we see 50".
    //
    // MinUpdateInterval is the minimum spacing WGC will put between delivered
    // frames. Left unset it defaults to about 16.6 ms, which caps delivery at
    // roughly 60 a second no matter what the game draws. Apex draws every
    // 13.889 ms, so frames were being withheld, and because delivery lands on
    // composition boundaries the result settled at 48-54.
    //
    // Measured side by side on this machine, same game, same minute:
    //
    //   our V2, property unset      ~50 unique frames/s
    //   PhyriadFG, property set     ~71 unique frames/s
    //
    // That is where a week went. Nothing downstream mattered - not the pool
    // depth, not draining it, not the window mode, not the compositor, not
    // background throttling. All of them sit behind this one call.
    //
    // Half the capture monitor's composition period, clamped to [0.5, 8] ms.
    // The floor has to sit BELOW the compose period or quantisation strangles
    // delivery back down to a slower cadence; half is the anti-quantisation
    // margin. It scales with the user's monitor and is never a constant: at
    // 144 Hz it is 3.47 ms, at 240 Hz 2.08 ms, at 60 Hz 8.33 -> clamped to 8.
    //
    // The idea is PhyriadFG's (MIT, see THIRD_PARTY_LICENSES.txt) and the
    // arithmetic matches theirs; the code here is our own.
    //
    // A low floor makes WGC deliver DUPLICATES in micro-bursts up to the
    // compose rate - the same composed picture handed over again. Those must
    // be filtered before pairing or the interpolator gets two identical
    // frames and a zero motion field. See the fingerprint below.
    {
        const int hz = (captureMonitorHz > 0) ? captureMonitorHz : 60;
        long long mui100ns = 10000000LL / (2LL * static_cast<long long>(hz));
        if (mui100ns < 5000LL)  mui100ns = 5000LL;    // 0.5 ms
        if (mui100ns > 80000LL) mui100ns = 80000LL;   // 8 ms
        try {
            if (auto s5 = m_session.try_as<IGraphicsCaptureSession5>()) {
                s5.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ mui100ns });
                Logger::Log("[FrameBoostV2] WGC MinUpdateInterval set to "
                            + std::to_string(static_cast<double>(mui100ns) / 10000.0)
                            + " ms (half the " + std::to_string(hz)
                            + " Hz capture-monitor period). Unset it defaults to ~16.6 ms,"
                            " which caps delivery at ~60/s.");
            } else {
                Logger::Log("[FrameBoostV2] IGraphicsCaptureSession5 unavailable - delivery stays "
                            "at the ~16.6 ms default, so the source cannot exceed ~60/s. "
                            "Needs Windows 11 24H2 or newer.");
            }
        } catch (...) {
            Logger::Log("[FrameBoostV2] MinUpdateInterval was refused; delivery stays at the default.");
        }
    }

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

// DRAIN THE POOL, do not take one and leave.
//
// This called TryGetNextFrame once per callback and returned. When two frames
// were sitting in the pool by the time it ran, one was taken and the other was
// recycled underneath us - and nothing anywhere reports that. Traced raw off
// the callback with Apex at 72:
//
//   dWgc = 13.888   one game frame
//   dWgc = 27.777   two game frames, one of them never seen
//   dWgc = 13.889
//   dWgc = 27.778
//
// Perfectly alternating, without a single exception: two arrivals for every
// three frames the game drew. 72 x 2/3 = 48, which is the number that has been
// on screen all evening and was blamed on the game, on the window mode, on the
// compositor and on the frame pool depth in turn.
//
// V1 got this right and said why: "Drain rather than take one: the pool can
// hold several by the time this runs, and every one of them is a frame the game
// drew." I moved that file into the legacy tree and then wrote the naive
// version here. Second regression of exactly this kind tonight.
void Capture::OnFrameArrived() {
    if (!m_capturing.load(std::memory_order_acquire)) return;

    for (;;) {
        Direct3D11CaptureFrame frame{ nullptr };
        try {
            frame = m_pool.TryGetNextFrame();
        } catch (...) {
            return;
        }
        if (!frame) return;
        ProcessArrival(frame);
    }
}

// Four uints on the GPU, three staging copies on the way back. Three because
// the readback must never wait: write slot N, read slot N-2, and the copy for
// N-1 is still in flight in between.
bool Capture::EnsureFingerprint(const D3D11_TEXTURE2D_DESC&) {
    if (m_fpShader) return true;
    if (!m_device) return false;

    if (FAILED(m_device->CreateComputeShader(g_FrameFingerprintCS,
                                             sizeof(g_FrameFingerprintCS),
                                             nullptr, m_fpShader.put())))
        return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFpLanes * sizeof(uint32_t);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(uint32_t);
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, m_fpBuffer.put()))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kFpLanes;
    if (FAILED(m_device->CreateUnorderedAccessView(m_fpBuffer.get(), &ud, m_fpUAV.put())))
        return false;

    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = kFpLanes * sizeof(uint32_t);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < kFpSlots; ++i)
        if (FAILED(m_device->CreateBuffer(&sd, nullptr, m_fpStaging[i].put()))) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, m_fpParams.put()))) return false;

    m_fpReady = true;
    Logger::Log("[FrameBoostV2] Fingerprint armed: sparse GPU hash, 16 bytes read back "
                "one arrival later. No frame is copied to the CPU.");
    return true;
}

int Capture::RunFingerprint(int slot, const D3D11_TEXTURE2D_DESC& desc) {
    if (!m_fpReady || !m_context) return -1;
    const double t0 = NowMs();

    if (!m_fpSlotSRV[slot]) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};
        sr.Format = desc.Format;
        sr.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sr.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(m_slotTex[slot].get(), &sr,
                                                      m_fpSlotSRV[slot].put())))
            return -1;
    }

    constexpr UINT kStride = 8;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(m_context->Map(m_fpParams.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        UINT* p = static_cast<UINT*>(m.pData);
        p[0] = desc.Width; p[1] = desc.Height; p[2] = kStride; p[3] = 0;
        m_context->Unmap(m_fpParams.get(), 0);
    }

    const UINT zero[4] = { 0, 0, 0, 0 };
    m_context->ClearUnorderedAccessViewUint(m_fpUAV.get(), zero);

    ID3D11ShaderResourceView* srv = m_fpSlotSRV[slot].get();
    ID3D11UnorderedAccessView* uav = m_fpUAV.get();
    ID3D11Buffer* cbs = m_fpParams.get();
    m_context->CSSetShader(m_fpShader.get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &srv);
    m_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    m_context->CSSetConstantBuffers(0, 1, &cbs);

    const UINT samplesX = (desc.Width  + kStride - 1) / kStride;
    const UINT samplesY = (desc.Height + kStride - 1) / kStride;
    m_context->Dispatch((samplesX + 7) / 8, (samplesY + 7) / 8, 1);

    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    m_context->CSSetShaderResources(0, 1, &noSrv);
    m_context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);

    const int writeSlot = static_cast<int>(m_fpSeq % kFpSlots);
    m_context->CopyResource(m_fpStaging[writeSlot].get(), m_fpBuffer.get());
    ++m_fpSeq;

    m_fpMsSum.store(m_fpMsSum.load(std::memory_order_relaxed) + (NowMs() - t0),
                    std::memory_order_relaxed);
    m_fpCount.fetch_add(1, std::memory_order_relaxed);
    return writeSlot;
}

// DO_NOT_WAIT, always. By the time the next frame arrives - 7 ms at the very
// fastest source this engine has measured - a dispatch of 57,600 point samples
// and a 16-byte copy are long finished. If it somehow is not ready, the answer
// is "unknown", never a wait.
bool Capture::TryReadFingerprint(int stagingSlot, uint32_t out[4]) {
    if (!m_fpReady || !m_context || stagingSlot < 0) return false;
    D3D11_MAPPED_SUBRESOURCE rm{};
    if (FAILED(m_context->Map(m_fpStaging[stagingSlot].get(), 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &rm)))
        return false;
    std::memcpy(out, rm.pData, 4 * sizeof(uint32_t));
    m_context->Unmap(m_fpStaging[stagingSlot].get(), 0);
    return true;
}

// The verdict on the frame held at m_head, delivered by the arrival that
// follows it. Publish it, or drop it where it lies.
void Capture::ResolvePending() {
    if (!m_havePending) return;

    uint32_t fp[4]{};
    const bool haveFp = TryReadFingerprint(m_pendingFpSlot, fp);

    bool duplicate = false;
    if (haveFp) {
        duplicate = m_haveLastPubFp && std::memcmp(fp, m_lastPubFp, sizeof(fp)) == 0;
        if (!duplicate) {
            std::memcpy(m_lastPubFp, fp, sizeof(fp));
            m_haveLastPubFp = true;
        }
    }
    // No fingerprint means no evidence, and a frame is not thrown away on
    // suspicion. Publishing an occasional duplicate is a blemish; discarding a
    // real frame is a hole in the timeline, and the timeline is the product.

    m_havePending = false;
    m_pendingFpSlot = -1;

    if (duplicate) {
        m_dupContent.fetch_add(1, std::memory_order_relaxed);
        return;   // head does not move; the next arrival overwrites this slot
    }

    std::lock_guard<std::mutex> ring(m_ringMutex);
    ++m_head;
}

void Capture::ProcessArrival(const Direct3D11CaptureFrame& frame) {
    // A WGC frame is a LEASE, not a copy: the surface goes back to the pool the
    // moment this object dies, and the pool may hand the same memory out again.
    // Everything that has to outlive this scope is copied out first.
    const double arrivalMs = NowMs();
    const double contentMs = static_cast<double>(frame.SystemRelativeTime().count()) / 10000.0;

    m_acquired.fetch_add(1, std::memory_order_relaxed);

    // The frame held back last time is judged now, before anything else - its
    // fingerprint has had a whole arrival to come back, and the slot it sits in
    // is the one this arrival may need.
    ResolvePending();

    // THE TIMESTAMP TEST DECIDES NOTHING. It counts, and that is all.
    //
    // It was a filter, and it was wrong. With WGC delivering exactly Apex's
    // rate it still discarded 5 to 12 frames a second:
    //
    //   acq 70.9-72.0   ts-equal 5-12/s   fp-dup 0   unique 60-67
    //   arrival spacing 15.1-16.7 ms against the source's 13.889
    //
    // Unique below the source rate is a hole in the timeline, and the holes
    // were ours. Worse, dropping here meant those very frames never got a
    // fingerprint - the one measurement that could have said whether the
    // timestamp was telling the truth. The test that needed checking had
    // arranged for itself never to be checked.
    //
    // So it is telemetry now. Every frame goes to the fingerprint, and only a
    // content match discards anything. Whether an identical stamp really does
    // mean an identical picture becomes something the log answers instead of
    // something the code assumes.
    if (m_lastArrivalContentMs >= 0.0 && contentMs == m_lastArrivalContentMs)
        m_dupTimestamp.fetch_add(1, std::memory_order_relaxed);
    m_lastArrivalContentMs = contentMs;

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
        // One slot short of the ring: the frame at m_head is pending and not
        // yet published, so it is not part of [m_tail, m_head) and must still
        // have somewhere to live.
        if (m_head - m_tail >= static_cast<uint64_t>(kSlots - 1)) {
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
        // The pending frame lives at m_head, so that is where this one goes -
        // either over the duplicate that was just dropped, or into the space
        // the one just published left behind.
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

    // Written into the slot at m_head and left there, invisible: the consumer
    // only ever sees [m_tail, m_head). ResolvePending on the next arrival
    // decides whether this becomes a source frame.
    {
        std::lock_guard<std::mutex> ring(m_ringMutex);
        m_slotFrameId[slot]  = m_nextFrameId++;
        m_slotContentMs[slot] = contentMs;
        m_slotArrivalMs[slot] = arrivalMs;
    }

    m_pendingFpSlot = EnsureFingerprint(srcDesc) ? RunFingerprint(slot, srcDesc) : -1;
    m_havePending = true;

    if (m_trace) {
        std::lock_guard<std::mutex> tr(m_traceMutex);
        if (m_traceBuffer.size() < 512 * 1024) {
            char line[192];
            const double dWgc = m_lastTraceWgcMs > 0.0 ? contentMs - m_lastTraceWgcMs : 0.0;
            const double dQpc = m_lastTraceQpcMs > 0.0 ? arrivalMs - m_lastTraceQpcMs : 0.0;
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "  ARR wgc=%.3f dWgc=%7.3f  qpc=%.3f dQpc=%7.3f  lag=%7.3f  %ux%u\n",
                        contentMs, dWgc, arrivalMs, dQpc, arrivalMs - contentMs,
                        srcDesc.Width, srcDesc.Height);
            m_traceBuffer += line;
        }
        m_lastTraceWgcMs = contentMs;
        m_lastTraceQpcMs = arrivalMs;
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

std::string Capture::TakeTrace() {
    std::lock_guard<std::mutex> tr(m_traceMutex);
    std::string out;
    out.swap(m_traceBuffer);
    return out;
}

void Capture::ResetCounters() {
    m_produced.store(0, std::memory_order_relaxed);
    m_consumed.store(0, std::memory_order_relaxed);
    m_overflows.store(0, std::memory_order_relaxed);
    m_acquired.store(0, std::memory_order_relaxed);
    m_dupTimestamp.store(0, std::memory_order_relaxed);
    m_dupContent.store(0, std::memory_order_relaxed);
    m_fpMsSum.store(0.0, std::memory_order_relaxed);
    m_fpCount.store(0, std::memory_order_relaxed);
}

void Capture::ReleaseSlots() {
    std::lock_guard<std::mutex> ring(m_ringMutex);
    for (auto& t : m_slotTex) t = nullptr;
    for (auto& v : m_fpSlotSRV) v = nullptr;
    m_head = m_tail = 0;
    m_inUseSlot = -1;
    m_havePending = false;
    m_pendingFpSlot = -1;
    m_haveLastPubFp = false;
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
