#include "presenter.h"
#include "capture.h"   // NowMs
#include "logger.h"

#include <string>

namespace fbv2 {

namespace {

constexpr wchar_t kClassName[] = L"ResetFrameBoostV2Overlay";

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // WM_CLOSE matters to the contract: the C# side calls CloseMainWindow()
    // and only resorts to Kill() two seconds later. Answering it is what makes
    // "FrameBoost off" a clean stop rather than a terminated process.
    if (msg == WM_CLOSE) { PostQuitMessage(0); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterOverlayClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

Presenter::~Presenter() { Destroy(); }

bool Presenter::CreateOverlayWindow(UINT width, UINT height) {
    RegisterOverlayClass();

    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE
                        | WS_EX_TRANSPARENT | WS_EX_TOPMOST
                        | WS_EX_TOOLWINDOW | WS_EX_LAYERED;

    m_hwnd = CreateWindowExW(exStyle, kClassName, L"RESET FRAMEBOOST",
                             WS_POPUP, 0, 0,
                             static_cast<int>(width), static_cast<int>(height),
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) {
        Logger::Log("[FrameBoostV2] Presenter: overlay window creation failed.");
        return false;
    }

    // 254, never 255 - see the header. This one value decides whether the game
    // underneath keeps rendering at all.
    SetLayeredWindowAttributes(m_hwnd, 0, 254, LWA_ALPHA);
    return true;
}

bool Presenter::CreateSwapChain(ID3D11Device* device, UINT width, UINT height) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) return false;

    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) return false;

    winrt::com_ptr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.put())))) return false;

    if (auto factory5 = factory.try_as<IDXGIFactory5>()) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allow, sizeof(allow))))
            m_tearingSupported = (allow == TRUE);
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    // Three buffers, not two.
    //
    // Two with a maximum frame latency of 1 is the lowest latency the API
    // offers and was measured in V1 to cost throughput: 21-42% of presents
    // collided and generated frames missed their deadline by 3-5 ms with only
    // 3.65 ms of slack. Three gives the queue room to absorb jitter without
    // the loop stalling inside Present.
    desc.BufferCount = 3;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_tearingSupported) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    if (FAILED(factory->CreateSwapChainForComposition(device, &desc, nullptr,
                                                      m_swapChain.put()))) {
        Logger::Log("[FrameBoostV2] Presenter: CreateSwapChainForComposition failed.");
        return false;
    }

    if (auto sc2 = m_swapChain.try_as<IDXGISwapChain2>()) {
        sc2->SetMaximumFrameLatency(2);
        m_frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
    }

    // DirectComposition v2 where available: IDCompositionVisual3::SetOpacity
    // only exists on a visual created by a v2 device, and V1 spent an evening
    // discovering that the hard way.
    winrt::com_ptr<IDCompositionDevice> comp;
    if (FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(comp.put())))) {
        Logger::Log("[FrameBoostV2] Presenter: DCompositionCreateDevice failed.");
        return false;
    }
    m_compDevice = comp;

    if (FAILED(m_compDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_compTarget.put()))) return false;
    if (FAILED(m_compDevice->CreateVisual(m_compVisual.put()))) return false;
    if (FAILED(m_compVisual->SetContent(m_swapChain.get()))) return false;
    if (FAILED(m_compTarget->SetRoot(m_compVisual.get()))) return false;
    if (FAILED(m_compDevice->Commit())) return false;

    m_width = width;
    m_height = height;
    return true;
}

bool Presenter::Create(ID3D11Device* device, UINT width, UINT height, HWND overlayTarget) {
    m_device.copy_from(device);
    m_target = overlayTarget;

    if (!CreateOverlayWindow(width, height)) return false;
    if (!CreateSwapChain(device, width, height)) { Destroy(); return false; }

    TrackTarget();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);

    Logger::Log(std::string("[FrameBoostV2] Presenter: DirectComposition flip-model swapchain, ")
                + "3 buffers, frame latency 2, tearing "
                + (m_tearingSupported ? "supported" : "unavailable") + ".");
    return true;
}

void Presenter::TrackTarget() {
    if (!m_hwnd || !m_target || !IsWindow(m_target)) return;

    RECT client{};
    if (!GetClientRect(m_target, &client)) return;

    POINT topLeft{ client.left, client.top };
    ClientToScreen(m_target, &topLeft);

    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return;

    // No inset. A one-pixel offset was tried in V1 and reported within ten
    // minutes as "das Spielmenue verschiebt sich immer links rechts links
    // rechts" - the overlay and the game disagreeing about where a pixel is.
    SetWindowPos(m_hwnd, HWND_TOPMOST, topLeft.x, topLeft.y, w, h,
                 SWP_NOACTIVATE | SWP_NOREDRAW);
}

void Presenter::Resize(UINT width, UINT height) {
    if (!m_swapChain || width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_tearingSupported) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    if (SUCCEEDED(m_swapChain->ResizeBuffers(0, width, height,
                                             DXGI_FORMAT_UNKNOWN, flags))) {
        m_width = width;
        m_height = height;
    }
}

bool Presenter::Present(ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    if (!m_swapChain || !texture || !context) return false;

    // NO WAIT ON THE FRAME-LATENCY WAITABLE. It is a cap, and this engine
    // does not have one.
    //
    // That object is signalled as the display CONSUMES presents, so blocking
    // on it throttles the output to the refresh rate. It is not written as a
    // number anywhere, which is exactly why it survived an audit for "144":
    // it is the same limit expressed as a wait. At a 100 fps source the
    // engine wants 200 presents a second and this would have held it at 144.
    //
    // Sync interval 0 with DXGI_PRESENT_ALLOW_TEARING is what makes going
    // past the refresh legal: the scan-out takes whatever is current and
    // tears, which is how any uncapped game exceeds its monitor. The panel
    // still cannot show 200 distinct pictures a second - that is physics -
    // but it is the panel that drops them, at the very end, and not us.
    //
    // Nothing runs away as a result: the present rate is two per source frame
    // and no more, so the source is the only thing that sets it.
    //
    // The waitable handle is kept for measurement, unused for control.
    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())))) return false;

    const double callT0 = NowMs();
    context->CopyResource(backBuffer.get(), texture);

    const UINT flags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = m_swapChain->Present(0, flags);

    const double callMs = NowMs() - callT0;
    m_callMsSum += callMs;
    if (callMs > m_callMsMax) m_callMsMax = callMs;
    ++m_presents;

    return SUCCEEDED(hr);
}

bool Presenter::QueryDisplayed(UINT& outDisplayed, UINT& outSubmitted) const {
    if (!m_swapChain) return false;
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(m_swapChain->GetFrameStatistics(&stats))) return false;
    UINT submitted = 0;
    if (FAILED(m_swapChain->GetLastPresentCount(&submitted))) return false;
    // PresentCount counts presents actually DISPLAYED; GetLastPresentCount
    // counts what we submitted. The difference is what never reached the panel.
    outDisplayed = stats.PresentCount;
    outSubmitted = submitted;
    return true;
}

void Presenter::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { PostQuitMessage(static_cast<int>(msg.wParam)); return; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Presenter::Destroy() {
    m_compTarget = nullptr;
    m_compVisual = nullptr;
    m_compDevice = nullptr;
    m_swapChain = nullptr;
    m_device = nullptr;
    m_frameLatencyWaitable = nullptr;
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

} // namespace fbv2
