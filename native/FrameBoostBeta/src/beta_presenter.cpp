#include "beta_presenter.h"
#include "logger.h"

#include <winrt/base.h>
#include <string>

namespace FrameBoostBeta {

namespace {
constexpr wchar_t kWindowClassName[] = L"ResetFrameBoostBetaWindow";
Presenter* g_instanceForWndProc = nullptr; // single window per process for this beta
}

LRESULT CALLBACK Presenter::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        if (g_instanceForWndProc) g_instanceForWndProc->m_shouldClose = true;
        if (msg == WM_DESTROY) PostQuitMessage(0);
        return 0;
    }

    // Click-through for the overlay. WS_EX_TRANSPARENT alone was NOT enough
    // in practice - verified live with WindowFromPoint, which still resolved
    // to this overlay rather than the application underneath, and the user
    // could not click or type into the captured window at all. Answering
    // HTTRANSPARENT tells Windows this window is not a hit target at any
    // point, so mouse input is routed to the window below instead.
    if (msg == WM_NCHITTEST && g_instanceForWndProc && (g_instanceForWndProc->m_overlayTarget || g_instanceForWndProc->m_overlayMonitor)) {
        return HTTRANSPARENT;
    }

    // Never become the active/foreground window by being clicked - the
    // captured application must keep focus or it throttles its own frame
    // rate (measured: ~37 FPS focused vs ~11 FPS unfocused).
    if (msg == WM_MOUSEACTIVATE && g_instanceForWndProc && (g_instanceForWndProc->m_overlayTarget || g_instanceForWndProc->m_overlayMonitor)) {
        return MA_NOACTIVATE;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Presenter::TryCreateCompositionWindow(ID3D11Device* device, UINT width, UINT height, const wchar_t* title) {
    if (m_compositionDisabled) return false;

    // WS_EX_NOREDIRECTIONBITMAP: the window gets no redirection surface at
    // all, which is exactly the point - content comes from a composition
    // swapchain instead, on the flip model, with per-refresh delivery and
    // working frame statistics.
    //
    // WS_EX_LAYERED is here despite NOREDIRECTIONBITMAP, and it has to be:
    // mouse pass-through to another process genuinely requires LAYERED plus
    // TRANSPARENT. Dropping it in the first DirectComposition build made the
    // captured window completely uninteractable - no clicking, scrolling or
    // typing - which is the same failure seen before with TRANSPARENT alone.
    // The content still comes from the composition swapchain rather than a
    // redirection surface, so the flip model survives.
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE
        | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;

    m_hwnd = CreateWindowExW(exStyle, kWindowClassName, title, WS_POPUP,
        0, 0, static_cast<int>(width), static_cast<int>(height),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) {
        Logger::Log("[FrameBoostBeta] Presenter: WS_EX_NOREDIRECTIONBITMAP window creation failed - falling back to the layered path.");
        return false;
    }

    // Alpha 254, deliberately not 255: Chromium-based browsers and games with
    // a backgrounded mode stop rendering entirely when they believe their
    // window is fully covered by an OPAQUE window, which starves the very
    // capture this overlay exists to improve. One step below opaque is not
    // counted as an occluder and is invisible in practice.
    SetLayeredWindowAttributes(m_hwnd, 0, 254, LWA_ALPHA);

    // Still hide ourselves from capture: in monitor mode we would otherwise
    // capture our own output and feed it back into the next frame.
    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        Logger::Log("[FrameBoostBeta] Presenter: could not exclude the overlay from capture "
            "(needs Windows 10 2004+). Monitor capture would feed back on itself.");
    }
    return true;
}

bool Presenter::AttachComposition(ID3D11Device* device, IDXGISwapChain1* swapChain) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) return false;

    if (FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(&m_dcompDevice)))) {
        Logger::Log("[FrameBoostBeta] Presenter: DCompositionCreateDevice failed.");
        return false;
    }
    // topmost = TRUE so the visual sits above the window's own (absent) content.
    if (FAILED(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget))) {
        Logger::Log("[FrameBoostBeta] Presenter: CreateTargetForHwnd failed.");
        return false;
    }
    if (FAILED(m_dcompDevice->CreateVisual(&m_dcompVisual))) return false;
    if (FAILED(m_dcompVisual->SetContent(swapChain))) return false;
    if (FAILED(m_dcompTarget->SetRoot(m_dcompVisual))) return false;
    if (FAILED(m_dcompDevice->Commit())) return false;

    return true;
}

bool Presenter::Create(ID3D11Device* device, UINT width, UINT height, const wchar_t* title, HWND overlayTarget) {
    m_width = width;
    m_height = height;
    m_baseTitle = title;
    m_overlayTarget = overlayTarget;

    WNDCLASSW wc{};
    wc.lpfnWndProc = &Presenter::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc); // ignore failure - already-registered on a second instance is fine

    g_instanceForWndProc = this;

    // Preferred path: composition swapchain on a non-redirected window.
    if (m_overlayTarget || m_overlayMonitor) {
        m_usingComposition = TryCreateCompositionWindow(device, width, height, title);
    }

    if (!m_usingComposition && (m_overlayTarget || m_overlayMonitor)) {
        // WS_EX_NOACTIVATE  - never take foreground focus from the game
        // WS_EX_TRANSPARENT - clicks/input pass straight through to the game
        // WS_EX_TOPMOST     - stays visible above the game window
        // WS_POPUP          - no title bar/border, so it lines up exactly
        // WS_EX_LAYERED is required on top of WS_EX_TRANSPARENT for click-
        // through that actually works across processes. Verified the hard
        // way: with WS_EX_TRANSPARENT alone (and even with a WM_NCHITTEST
        // handler returning HTTRANSPARENT), the captured application stayed
        // completely uninteractable - no clicking, no typing.
        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
        m_hwnd = CreateWindowExW(exStyle, kWindowClassName, title, WS_POPUP,
            0, 0, static_cast<int>(width), static_cast<int>(height),
            nullptr, nullptr, wc.hInstance, nullptr);

        // Alpha 254, deliberately NOT 255. Applications with occlusion
        // detection (all Chromium-based browsers, and games with a
        // "backgrounded" mode) stop rendering entirely when they believe
        // their window is fully covered by an opaque window. Observed live:
        // an opaque overlay froze Opera's output until a tab switch forced
        // a repaint. A window that is even slightly translucent is not
        // counted as an occluder, so the application underneath keeps
        // drawing normally - and 1/255 translucency is invisible.
        if (m_hwnd) SetLayeredWindowAttributes(m_hwnd, 0, 254, LWA_ALPHA);

        // Make this window invisible to screen capture. Essential in
        // monitor-capture mode: without it we would capture our own output
        // and feed it back into the next frame (a capture feedback loop).
        // Same mechanism capture tools use to hide their own preview.
        if (m_hwnd && !SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
            Logger::Log("[FrameBoostBeta] Presenter: could not exclude the overlay from capture "
                "(needs Windows 10 2004+). Monitor capture would feed back on itself.");
        }
    } else if (!m_usingComposition) {
        m_hwnd = CreateWindowExW(0, kWindowClassName, title, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(width), static_cast<int>(height),
            nullptr, nullptr, wc.hInstance, nullptr);
    }

    if (!m_hwnd) {
        Logger::Log("[FrameBoostBeta] Presenter: CreateWindowExW failed.");
        return false;
    }

    if (m_overlayTarget || m_overlayMonitor) {
        TrackOverlayTarget();
        // SW_SHOWNOACTIVATE: become visible WITHOUT stealing focus, which is
        // the entire point - the game must stay foreground so it keeps
        // rendering at full speed.
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        Logger::Log("[FrameBoostBeta] Presenter running as a click-through overlay on top of the target window (game keeps focus).");
    } else {
        ShowWindow(m_hwnd, SW_SHOW);
    }

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) return false;

    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) return false;

    winrt::com_ptr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.put())))) return false;

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // flip model does not support the _SRGB variants directly
    scDesc.SampleDesc.Count = 1;
    // UNORDERED_ACCESS so the interpolation shader can write STRAIGHT into
    // the back buffer. Without it every generated frame had to be written to
    // an intermediate texture and then copied - 14 MB per frame, 144 times a
    // second, for nothing.
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    scDesc.BufferCount = 3; // extra slack so the immediate (syncInterval 0) generated-frame present never stalls waiting for a free buffer

    IDXGISwapChain1* swapChain = nullptr;
    HRESULT hr = E_FAIL;

    if (m_usingComposition) {
        // Flip model with premultiplied alpha: the per-refresh delivery path,
        // and the prerequisite for ever showing only the generated frames
        // while the real screen shows through in between.
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = factory->CreateSwapChainForComposition(device, &scDesc, nullptr, &swapChain);
        if (FAILED(hr)) {
            Logger::Log("[FrameBoostBeta] Presenter: CreateSwapChainForComposition failed, hr="
                + std::to_string(hr) + " - falling back to the layered path.");
        } else if (!AttachComposition(device, swapChain)) {
            swapChain->Release();
            swapChain = nullptr;
            hr = E_FAIL;
            Logger::Log("[FrameBoostBeta] Presenter: DirectComposition attach failed - falling back to the layered path.");
        }

        if (FAILED(hr)) {
            // Safe fallback: tear the composition window down and rebuild the
            // old layered overlay, so a DirectComposition problem degrades to
            // the previously working behaviour instead of no output at all.
            m_usingComposition = false;
            m_compositionDisabled = true;
            if (m_dcompVisual) { m_dcompVisual->Release(); m_dcompVisual = nullptr; }
            if (m_dcompTarget) { m_dcompTarget->Release(); m_dcompTarget = nullptr; }
            if (m_dcompDevice) { m_dcompDevice->Release(); m_dcompDevice = nullptr; }
            if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
            return Create(device, width, height, m_baseTitle.c_str(), m_overlayTarget);
        }
    } else {
        // A layered window cannot host a flip-model swapchain - DXGI only
        // supports the older BitBlt model there.
        scDesc.SwapEffect = (m_overlayTarget || m_overlayMonitor) ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForHwnd(device, m_hwnd, &scDesc, nullptr, nullptr, &swapChain);
        if (FAILED(hr)) {
            Logger::Log("[FrameBoostBeta] Presenter: CreateSwapChainForHwnd failed, hr=" + std::to_string(hr));
            return false;
        }
    }

    m_swapChain = swapChain;

    Logger::Log(m_usingComposition
        ? "[FrameBoostBeta] Presenter: DirectComposition flip-model swapchain (per-refresh delivery, frame statistics available, per-pixel alpha capable)."
        : "[FrameBoostBeta] Presenter: layered-window BitBlt swapchain (legacy path - no per-refresh guarantee).");
    return true;
}

ID3D11UnorderedAccessView* Presenter::AcquireBackBufferUAV(ID3D11Device* device) {
    if (!m_swapChain || !device) return nullptr;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return nullptr;

    // The flip model rotates through several buffers, so cache the view per
    // buffer rather than recreating it every frame.
    if (m_backBufferUAV && backBuffer == m_uavBackBuffer) {
        backBuffer->Release();
        return m_backBufferUAV;
    }

    if (m_backBufferUAV) { m_backBufferUAV->Release(); m_backBufferUAV = nullptr; }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    if (FAILED(device->CreateUnorderedAccessView(backBuffer, &uavDesc, &m_backBufferUAV))) {
        m_backBufferUAV = nullptr;
        backBuffer->Release();
        return nullptr;
    }

    m_uavBackBuffer = backBuffer; // borrowed pointer, used only for identity
    backBuffer->Release();
    return m_backBufferUAV;
}

HRESULT Presenter::PresentBackBuffer(UINT syncInterval) {
    if (!m_swapChain) return E_FAIL;
    return m_swapChain->Present(syncInterval, 0);
}

HRESULT Presenter::PresentTransparent(ID3D11Device* device, ID3D11DeviceContext* context, UINT syncInterval) {
    if (!m_swapChain || !m_usingComposition) return E_FAIL;

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return hr;

    // A render target view is needed just to clear; created on demand and
    // cached, since this runs on every real-frame slot.
    if (!m_clearRTV) {
        if (FAILED(device->CreateRenderTargetView(backBuffer, nullptr, &m_clearRTV))) {
            backBuffer->Release();
            return E_FAIL;
        }
    }

    // Fully transparent, premultiplied: alpha 0 with zero colour. The real
    // screen underneath shows through untouched - no capture, no copy, no
    // rescale, so those frames cost nothing in quality, and the window below
    // is never fully covered by opaque content.
    const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(m_clearRTV, transparent);
    backBuffer->Release();

    return m_swapChain->Present(syncInterval, 0);
}

HRESULT Presenter::PresentFrame(ID3D11DeviceContext* context, ID3D11Texture2D* sourceTexture, UINT syncInterval) {
    if (!m_swapChain || !sourceTexture) return E_FAIL;

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return hr;

    context->CopyResource(backBuffer, sourceTexture);
    backBuffer->Release();

    return m_swapChain->Present(syncInterval, 0);
}

bool Presenter::QueryPresentStats(UINT& outPresentCount, UINT& outPresentRefreshCount, UINT& outSyncRefreshCount) {
    if (!m_swapChain) return false;

    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(m_swapChain->GetFrameStatistics(&stats))) return false;

    UINT lastPresent = 0;
    if (FAILED(m_swapChain->GetLastPresentCount(&lastPresent))) return false;

    // stats.PresentCount counts presents that were actually DISPLAYED;
    // GetLastPresentCount counts presents we SUBMITTED. The difference is
    // frames the display never showed.
    //
    // PresentRefreshCount is deliberately not used for this: it is the
    // refresh counter at which the last present appeared, so its delta is
    // simply the number of refreshes elapsed (~144/s whatever we submit).
    // Reading it as "frames displayed" made a completely static output look
    // like a perfect 144 FPS.
    outPresentCount = stats.PresentCount;                 // displayed
    outPresentRefreshCount = lastPresent;                 // submitted
    outSyncRefreshCount = stats.SyncRefreshCount;
    return true;
}

void Presenter::Resize(UINT width, UINT height) {
    if (!m_swapChain || (width == m_width && height == m_height) || width == 0 || height == 0) return;
    m_width = width;
    m_height = height;
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
}

void Presenter::SetTitleSuffix(const wchar_t* suffix) {
    if (!m_hwnd) return;
    std::wstring fullTitle = m_baseTitle + L" - " + suffix;
    SetWindowTextW(m_hwnd, fullTitle.c_str());
}

void Presenter::TrackOverlayTarget() {
    if (!m_hwnd) return;

    // Monitor mode: cover the whole monitor instead of tracking a window.
    if (m_overlayMonitor) {
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (GetMonitorInfoW(m_overlayMonitor, &mi)) {
            // Leave one pixel column uncovered, the same trick already used
            // for window mode. A window Windows considers FULLY occluded stops
            // being drawn, which starves the capture this overlay exists to
            // improve - measured as gaps of 82-166 ms every few seconds with
            // the monitor fully covered. Covering all but one pixel column is
            // invisible and keeps the content below being drawn.
            constexpr int kAntiOcclusionInsetPx = 1;
            SetWindowPos(m_hwnd, HWND_TOPMOST,
                mi.rcMonitor.left + kAntiOcclusionInsetPx, mi.rcMonitor.top,
                (mi.rcMonitor.right - mi.rcMonitor.left) - kAntiOcclusionInsetPx,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        return;
    }

    if (!m_overlayTarget) return;
    if (!IsWindow(m_overlayTarget)) { m_shouldClose = true; return; }

    // Match the target's CLIENT area in screen coordinates, so the overlay
    // covers exactly the rendered game image and not its window borders.
    RECT clientRect{};
    if (!GetClientRect(m_overlayTarget, &clientRect)) return;

    POINT topLeft{ clientRect.left, clientRect.top };
    ClientToScreen(m_overlayTarget, &topLeft);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) return;

    // Deliberately leave a 1px sliver of the target visible. A window that
    // Windows considers FULLY occluded gets its presentation throttled -
    // measured live: covering Watch Dogs completely dropped it from ~37 FPS
    // to ~25 FPS, starving the very input signal this overlay exists to
    // improve. One uncovered pixel column is enough to avoid being counted
    // as fully occluded while being invisible in practice.
    constexpr int kAntiOcclusionInsetPx = 1;
    SetWindowPos(m_hwnd, HWND_TOPMOST,
        topLeft.x + kAntiOcclusionInsetPx, topLeft.y,
        width - kAntiOcclusionInsetPx, height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Presenter::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

Presenter::~Presenter() {
    if (m_clearRTV) m_clearRTV->Release();
    if (m_backBufferUAV) m_backBufferUAV->Release();
    if (m_dcompVisual) m_dcompVisual->Release();
    if (m_dcompTarget) m_dcompTarget->Release();
    if (m_dcompDevice) m_dcompDevice->Release();
    if (m_swapChain) m_swapChain->Release();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

} // namespace FrameBoostBeta
