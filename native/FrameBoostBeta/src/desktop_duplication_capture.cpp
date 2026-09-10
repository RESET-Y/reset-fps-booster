#include "desktop_duplication_capture.h"
#include "logger.h"

#include <sstream>
#include <string>

namespace FrameBoostBeta {

namespace {

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return 1000.0 * static_cast<double>(now.QuadPart) / freq.QuadPart;
}

int64_t QpcTicksTo100ns(int64_t ticks) {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    return static_cast<int64_t>(static_cast<double>(ticks) / freq.QuadPart * 10000000.0);
}

} // namespace

bool DesktopDuplicationCapture::StartMonitor(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context) {
    m_monitor = monitor;
    m_device.copy_from(device);
    m_context.copy_from(context);

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
        Logger::Log("[FrameBoostBeta] Desktop Duplication: could not get the DXGI device.");
        return false;
    }
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
        Logger::Log("[FrameBoostBeta] Desktop Duplication: could not get the DXGI adapter.");
        return false;
    }

    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, output.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
            m_output = output.try_as<IDXGIOutput1>();
            break;
        }
    }
    if (!m_output) {
        Logger::Log("[FrameBoostBeta] Desktop Duplication: no DXGI output matches that monitor.");
        return false;
    }

    if (!CreateDuplication()) return false;

    Logger::Log("[FrameBoostBeta] Desktop Duplication: capture is pumped from the output loop's idle time,"
                " so waiting for a refresh boundary no longer means missing frames.");
    return true;
}

bool DesktopDuplicationCapture::CreateDuplication() {
    m_duplication = nullptr;
    if (!m_output || !m_device) return false;

    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    const HRESULT hr = m_output->DuplicateOutput(m_device.get(), duplication.put());
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[FrameBoostBeta] Desktop Duplication unavailable (hr 0x" << std::hex << hr << std::dec
            << "). E_ACCESSDENIED usually means the display is held elsewhere -"
               " another duplication client, a secure desktop, or a mode change in progress.";
        Logger::Log(oss.str());
        return false;
    }

    m_duplication = duplication;
    DXGI_OUTDUPL_DESC desc{};
    m_duplication->GetDesc(&desc);
    m_width = desc.ModeDesc.Width;
    m_height = desc.ModeDesc.Height;

    std::ostringstream oss;
    oss << "[FrameBoostBeta] Desktop Duplication active: " << m_width << "x" << m_height
        << " | desktop image is "
        << (desc.DesktopImageInSystemMemory ? "in system memory (slow path)" : "on the GPU");
    Logger::Log(oss.str());

    // A new duplication hands back differently sized frames after a mode
    // change, so the slots have to be rebuilt with it.
    {
        std::lock_guard<std::mutex> lock(m_slotMutex);
        for (auto& tex : m_slotTex) tex = nullptr;
        m_newestSlot = -1;
        m_inUseSlot = -1;
    }
    return true;
}

void DesktopDuplicationCapture::Pump() {
    // Drain everything that is ready, then return - never block, because the
    // caller is in the middle of pacing its next present.
    for (int guard = 0; guard < 16; ++guard) {
        if (!m_duplication) {
            if (NowMs() < m_nextReconnectAttemptMs) return;
            m_nextReconnectAttemptMs = NowMs() + 200.0;
            if (CreateDuplication()) m_reconnects.fetch_add(1, std::memory_order_relaxed);
            if (!m_duplication) return;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        const HRESULT hr = m_duplication->AcquireNextFrame(0, &info, resource.put());

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return; // nothing ready, back to pacing
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            Logger::Log("[FrameBoostBeta] Desktop Duplication access lost (mode change, or the display was"
                        " taken over) - rebuilding.");
            m_duplication = nullptr;
            m_nextReconnectAttemptMs = NowMs() + 100.0;
            return;
        }
        if (FAILED(hr)) {
            std::ostringstream oss;
            oss << "[FrameBoostBeta] Desktop Duplication AcquireNextFrame failed (hr 0x" << std::hex << hr
                << ") - rebuilding.";
            Logger::Log(oss.str());
            m_duplication = nullptr;
            m_nextReconnectAttemptMs = NowMs() + 500.0;
            return;
        }

        // LastPresentTime == 0 means only the cursor moved. That is not a new
        // frame, and counting it as one would feed the motion estimator two
        // identical images.
        if (info.LastPresentTime.QuadPart == 0) {
            m_cursorOnlyUpdates.fetch_add(1, std::memory_order_relaxed);
            m_duplication->ReleaseFrame();
            continue;
        }
        if (info.AccumulatedFrames > 1)
            m_coalescedFrames.fetch_add(info.AccumulatedFrames - 1, std::memory_order_relaxed);

        auto surface = resource.try_as<ID3D11Texture2D>();
        if (!surface) { m_duplication->ReleaseFrame(); continue; }

        D3D11_TEXTURE2D_DESC desc{};
        surface->GetDesc(&desc);

        // Pick a slot that is neither the one the main loop is reading nor the
        // newest published one, so a consumer holding a frame never has it
        // overwritten underneath.
        int slot = -1;
        {
            std::lock_guard<std::mutex> lock(m_slotMutex);
            for (int i = 0; i < kSlotCount; ++i) {
                if (i != m_inUseSlot && i != m_newestSlot) { slot = i; break; }
            }
            if (slot < 0) slot = (m_newestSlot + 1) % kSlotCount;
        }

        if (!m_slotTex[slot] || m_width != desc.Width || m_height != desc.Height) {
            D3D11_TEXTURE2D_DESC copyDesc = desc;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;

            winrt::com_ptr<ID3D11Texture2D> tex;
            if (FAILED(m_device->CreateTexture2D(&copyDesc, nullptr, tex.put()))) {
                m_duplication->ReleaseFrame();
                continue;
            }
            m_slotTex[slot] = tex;
            m_width = desc.Width;
            m_height = desc.Height;
        }

        // The duplication surface is only valid until ReleaseFrame, and it is
        // read-only, so the frame is copied into a texture of our own that the
        // rest of the pipeline can bind as a shader resource.
        m_context->CopyResource(m_slotTex[slot].get(), surface.get());
        m_duplication->ReleaseFrame();

        {
            std::lock_guard<std::mutex> lock(m_slotMutex);
            m_slotTimestamp100ns[slot] = QpcTicksTo100ns(info.LastPresentTime.QuadPart);
            m_newestSlot = slot;
            ++m_newestSerial;
        }
        m_framesPublished.fetch_add(1, std::memory_order_relaxed);
    }
}

ID3D11Texture2D* DesktopDuplicationCapture::PollLatestFrame(UINT& outWidth, UINT& outHeight,
                                                            int64_t& outFrameTimestamp100ns, bool& outIsNewFrame) {
    outIsNewFrame = false;
    outWidth = m_width;
    outHeight = m_height;
    outFrameTimestamp100ns = 0;

    std::lock_guard<std::mutex> lock(m_slotMutex);
    if (m_newestSlot < 0) return nullptr;

    ID3D11Texture2D* tex = m_slotTex[m_newestSlot].get();
    outFrameTimestamp100ns = m_slotTimestamp100ns[m_newestSlot];

    if (m_newestSerial != m_consumedSerial) {
        m_consumedSerial = m_newestSerial;
        m_inUseSlot = m_newestSlot;
        outIsNewFrame = true;
        ++m_framesConsumed;
    }
    return tex;
}

void DesktopDuplicationCapture::Stop() {
    m_duplication = nullptr;
    for (auto& tex : m_slotTex) tex = nullptr;
    m_output = nullptr;
}

DesktopDuplicationCapture::~DesktopDuplicationCapture() {
    Stop();
}

} // namespace FrameBoostBeta
