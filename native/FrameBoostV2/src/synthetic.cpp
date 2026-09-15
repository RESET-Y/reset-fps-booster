#include "synthetic.h"
#include "logger.h"

#include <string>

namespace fbv2 {

bool SyntheticSource::Init(ID3D11Device* device, UINT width, UINT height,
                           double fps, double pixelsPerSecond) {
    if (!device || fps <= 0.0) return false;

    m_width = width;
    m_height = height;
    m_intervalMs = 1000.0 / fps;
    m_pixelsPerMs = pixelsPerSecond / 1000.0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    for (int i = 0; i < kRing; ++i) {
        if (FAILED(device->CreateTexture2D(&desc, nullptr, m_tex[i].put()))) return false;
        if (FAILED(device->CreateRenderTargetView(m_tex[i].get(), nullptr, m_rtv[i].put())))
            return false;
    }

    m_startMs = NowMs();
    m_nextDueMs = m_startMs;

    Logger::Log("[FrameBoostV2] Synthetic source: " + std::to_string(fps)
                + " fps, white square at " + std::to_string(pixelsPerSecond)
                + " px/s across " + std::to_string(width) + "x" + std::to_string(height)
                + ". The panel's refresh rate is irrelevant to this number.");
    return true;
}

bool SyntheticSource::Produce(ID3D11DeviceContext* context, CapturedFrame& out) {
    if (!context || m_width == 0) return false;

    const double now = NowMs();
    if (now < m_nextDueMs) return false;

    if (!m_context1) context->QueryInterface(IID_PPV_ARGS(m_context1.put()));

    const int slot = m_next;
    m_next = (m_next + 1) % kRing;

    // The content moment is the SCHEDULED one, not the moment we got round to
    // drawing it. A synthetic source that stamped frames with their draw time
    // would hand the scheduler its own jitter back and prove nothing.
    const double contentMs = m_nextDueMs;

    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(m_rtv[slot].get(), black);

    if (m_context1) {
        const double elapsed = contentMs - m_startMs;
        const UINT side = m_height / 8;
        const double travel = static_cast<double>(m_width) - side;
        double x = m_pixelsPerMs * elapsed;
        if (travel > 0.0) {
            // Bounce, so the square stays on screen for a long run without the
            // position wrapping - a wrap looks exactly like a step backwards,
            // and this test exists to make a step backwards visible.
            const double cycle = std::fmod(x, travel * 2.0);
            x = (cycle <= travel) ? cycle : (travel * 2.0 - cycle);
        }

        const UINT left = static_cast<UINT>(x);
        const UINT top = (m_height - side) / 2;
        const D3D11_RECT rect{ static_cast<LONG>(left), static_cast<LONG>(top),
                               static_cast<LONG>(left + side),
                               static_cast<LONG>(top + side) };
        const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_context1->ClearView(m_rtv[slot].get(), white, &rect, 1);
    }

    out.texture   = m_tex[slot].get();
    out.frameId   = ++m_frameId;
    out.contentMs = contentMs;
    out.arrivalMs = now;
    out.width     = m_width;
    out.height    = m_height;
    out.slot      = -1;   // not a ring slot; nothing to release

    // Advance by exactly one interval rather than from "now", so a late draw
    // does not push the whole timeline out. If we fall a long way behind,
    // catch up to the present instead of trying to replay the past.
    m_nextDueMs += m_intervalMs;
    if (m_nextDueMs < now - m_intervalMs * 4.0) m_nextDueMs = now + m_intervalMs;
    return true;
}

} // namespace fbv2
