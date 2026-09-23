#include "presenter.h"
#include "capture.h"   // NowMs
#include "logger.h"
#include <sstream>

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

    const UINT w = m_halfWidth ? (width / 2) : width;
    if (!CreateOverlayWindow(w, height)) return false;
    if (!CreateSwapChain(device, w, height)) { Destroy(); return false; }

    TrackTarget();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);

    LogConfiguration();
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

    int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (m_halfWidth) w /= 2;
    if (w <= 0 || h <= 0) return;

    // No inset. A one-pixel offset was tried in V1 and reported within ten
    // minutes as "das Spielmenue verschiebt sich immer links rechts links
    // rechts" - the overlay and the game disagreeing about where a pixel is.
    SetWindowPos(m_hwnd, HWND_TOPMOST, topLeft.x, topLeft.y, w, h,
                 SWP_NOACTIVATE | SWP_NOREDRAW);
}

void Presenter::Resize(UINT width, UINT height) {
    if (!m_swapChain || width == 0 || height == 0) return;
    if (m_halfWidth) width /= 2;
    if (width == m_width && height == m_height) return;

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_tearingSupported) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    if (SUCCEEDED(m_swapChain->ResizeBuffers(0, width, height,
                                             DXGI_FORMAT_UNKNOWN, flags))) {
        m_width = width;
        m_height = height;
    }
}

bool Presenter::Present(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                        PresentTiming* timing, Marker marker) {
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
    // Peeked BEFORE the copy, so it describes the state Present is about to
    // meet rather than one the copy may have changed.
    if (timing && m_frameLatencyWaitable) {
        const DWORD w = WaitForSingleObject(m_frameLatencyWaitable, 0);
        timing->waitableFree = (w == WAIT_OBJECT_0);
        timing->waitableKnown = (w == WAIT_OBJECT_0 || w == WAIT_TIMEOUT);
    }

    const double callT0 = NowMs();

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())))) return false;
    // CopySubresourceRegion, not CopyResource, because in half-width mode the
    // back buffer is narrower than the captured frame and CopyResource
    // requires them to match. The box is the back buffer's own size clamped
    // to the source, so the full-width path copies exactly what it always did.
    {
        D3D11_TEXTURE2D_DESC bdesc{}, sdesc{};
        backBuffer->GetDesc(&bdesc);
        texture->GetDesc(&sdesc);
        D3D11_BOX box{};
        box.left = 0; box.top = 0; box.front = 0;
        box.right  = (bdesc.Width  < sdesc.Width)  ? bdesc.Width  : sdesc.Width;
        box.bottom = (bdesc.Height < sdesc.Height) ? bdesc.Height : sdesc.Height;
        box.back = 1;
        context->CopySubresourceRegion(backBuffer.get(), 0, 0, 0, 0,
                                       texture, 0, &box);
    }

    // Drawn straight into the back buffer, after the copy and before the
    // present, so what the marker says is what that present carries.
    if (marker != Marker::None) {
        winrt::com_ptr<ID3D11DeviceContext1> ctx1;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(ctx1.put())))) {
            winrt::com_ptr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(m_device->CreateRenderTargetView(backBuffer.get(), nullptr,
                                                           rtv.put()))) {
                D3D11_TEXTURE2D_DESC bd{};
                backBuffer->GetDesc(&bd);
                // THREE BLOCKS DOWN THE LEFT EDGE, because one at the top
                // cannot tell the two explanations apart.
                //
                // Reported: the marker looks mostly green under fast movement,
                // while the counters say every generated frame was produced
                // AND presented, evenly spaced at p50 8.04 ms. Both can be
                // true at once if what reaches the panel depends on WHERE on
                // the screen it is: we present with SyncInterval 0 and
                // tearing, so a present arriving mid-scan only replaces the
                // rows below the beam. A block at y=24 then shows whichever
                // frame was presented just before the vertical blank, not the
                // most recent one.
                //
                // Top, middle and bottom are painted by the SAME present. If
                // they disagree, it is the scan-out position and nothing is
                // being lost. If all three agree and go green together, the
                // generated frame really is being buried and the counters are
                // measuring the wrong thing.
                // Premultiplied alpha: the colour is already multiplied by the
                // alpha, or the block washes out on an ALPHA_MODE_PREMULTIPLIED
                // swapchain.
                const float kMarkerRed[4]   = { 1.0f, 0.0f, 0.0f, 1.0f };
                const float kMarkerGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

                const LONG side = 160;
                const LONG h = static_cast<LONG>(bd.Height);
                // POSITION, NOT ONLY COLOUR, because colour alone could not
                // answer the question. Red and green alternating 128 times a
                // second integrate in the eye, and green carries roughly three
                // times the luminance of red at equal saturation - so a clean
                // 50/50 alternation reads as "mostly green", which is exactly
                // what was reported while every counter said both kinds were
                // being presented and DXGI said 11,974 of 11,975 presents were
                // displayed.
                //
                // Real frames now mark the LEFT edge, generated frames the
                // RIGHT. Two columns visible at once means both reach the
                // panel whatever the eye makes of the colours. One column
                // means the other kind never arrives.
                const LONG w = static_cast<LONG>(bd.Width);
                const LONG x = (marker == Marker::Generated) ? (w - side - 24) : 24;
                const LONG tops[3] = { 24, h / 2 - side / 2, h - side - 24 };
                for (int i = 0; i < 3; ++i) {
                    D3D11_RECT r{ x, tops[i], x + side, tops[i] + side };
                    if (r.left < 0) r.left = 0;
                    if (r.top < 0) r.top = 0;
                    if (r.right  > static_cast<LONG>(bd.Width)) r.right = static_cast<LONG>(bd.Width);
                    if (r.bottom > h) r.bottom = h;
                    if (r.bottom <= r.top) continue;
                    ctx1->ClearView(rtv.get(),
                                    marker == Marker::Generated
                                        ? kMarkerRed : kMarkerGreen,
                                    &r, 1);
                }
            }
        }
    }

    const double copyDone = NowMs();

    const UINT flags = (m_tearingSupported && !m_vsync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = m_swapChain->Present(m_vsync ? 1u : 0u, flags);

    const double presentDone = NowMs();
    if (timing) {
        timing->copyMs = copyDone - callT0;
        timing->presentMs = presentDone - copyDone;
        UINT disp = 0, sub = 0;
        if (QueryDisplayed(disp, sub)) {
            timing->displayed = disp; timing->submitted = sub; timing->statsKnown = true;
        }
        // Read separately and flagged separately: a swapchain can hand back a
        // PresentCount while the refresh fields are unusable, and a run where
        // this stays false is a result too, not a gap to be filled in.
        DXGI_FRAME_STATISTICS fs{};
        if (SUCCEEDED(m_swapChain->GetFrameStatistics(&fs))) {
            timing->statPresentCount   = fs.PresentCount;
            timing->statPresentRefresh = fs.PresentRefreshCount;
            timing->statSyncRefresh    = fs.SyncRefreshCount;
            timing->statRefreshKnown   = true;
        }
    }

    const double callMs = presentDone - callT0;
    m_callMsSum += callMs;
    if (callMs > m_callMsMax) m_callMsMax = callMs;
    ++m_presents;

    return SUCCEEDED(hr);
}

// The swapchain as it actually is at runtime, not as the source reads.
void Presenter::LogConfiguration() const {
    if (!m_swapChain) return;
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (FAILED(m_swapChain->GetDesc1(&d))) return;

    UINT maxLatency = 0;
    bool latencyKnown = false;
    if (auto sc2 = m_swapChain.try_as<IDXGISwapChain2>())
        latencyKnown = SUCCEEDED(sc2->GetMaximumFrameLatency(&maxLatency));

    std::ostringstream oss;
    oss << "[FrameBoostV2] Swapchain: " << d.Width << "x" << d.Height
        << " BufferCount=" << d.BufferCount
        << " SwapEffect=" << (d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                              ? "FLIP_SEQUENTIAL" : "FLIP_DISCARD")
        << " Flags=0x" << std::hex << d.Flags << std::dec
        << " (waitable=" << ((d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ? "yes" : "no")
        << ", allow_tearing=" << ((d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? "yes" : "no") << ")"
        << " | MaximumFrameLatency=";
    if (latencyKnown) oss << maxLatency; else oss << "unknown";
    oss << " | waitable handle=" << (m_frameLatencyWaitable ? "held, never waited on" : "none")
        << " | Present(SyncInterval=0, Flags="
        << (m_tearingSupported ? "DXGI_PRESENT_ALLOW_TEARING" : "0") << ")";

    // GetFrameStatistics is not guaranteed on every swapchain configuration -
    // it can fail outright, or return DXGI_ERROR_FRAME_STATISTICS_DISJOINT
    // after a mode change. Asked here so the answer is on the record BEFORE any
    // refresh counts are read out of it, rather than a silent zero later.
    DXGI_FRAME_STATISTICS fs{};
    const HRESULT sh = m_swapChain->GetFrameStatistics(&fs);
    oss << " | GetFrameStatistics: ";
    if (SUCCEEDED(sh))
        oss << "ok (PresentCount=" << fs.PresentCount
            << ", PresentRefreshCount=" << fs.PresentRefreshCount
            << ", SyncRefreshCount=" << fs.SyncRefreshCount << ")";
    else if (sh == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) oss << "DISJOINT";
    else if (sh == DXGI_ERROR_INVALID_CALL)              oss << "INVALID_CALL";
    else oss << "hr=0x" << std::hex << static_cast<unsigned>(sh) << std::dec;

    Logger::Log(oss.str());
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
