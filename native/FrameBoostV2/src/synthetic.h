// A SOURCE WE CONTROL, so the pipeline can be tested at rates no window here
// produces.
//
// The question it exists to answer: is the engine's output rate set by the
// source or by the panel? A 144 Hz monitor and a desktop window that redraws
// at 48 cannot distinguish "twice the source" from "capped at the refresh" -
// both predict the same numbers below 72. Above it they diverge sharply, and
// this is how to get there.
//
// Deliberately cheap: a white square on black, moved by ClearView on a
// sub-rectangle. No shader, no upload, a few microseconds per frame. That
// keeps the measurement about timing rather than about GPU load - if the
// output falls short here, it is the scheduler, not the card.
//
// It is also the visual test: a square crossing at a constant speed makes
// judder, a step backwards, or a doubled image obvious to the eye in a way no
// counter can be.
#pragma once

#include <d3d11_1.h>
#include <winrt/base.h>

#include "capture.h"   // CapturedFrame

namespace fbv2 {

class SyntheticSource {
public:
    bool Init(ID3D11Device* device, UINT width, UINT height, double fps, double pixelsPerSecond);

    // Produces the next frame when its moment has come, and reports how long
    // to wait otherwise. Timestamps are generated on the same monotonic clock
    // the capture path uses, so everything downstream is unchanged.
    bool Produce(ID3D11DeviceContext* context, CapturedFrame& out);
    double NextDueMs() const { return m_nextDueMs; }

    UINT Width()  const { return m_width; }
    UINT Height() const { return m_height; }

private:
    static constexpr int kRing = 3;   // in flight: generating, presenting, spare

    winrt::com_ptr<ID3D11Texture2D>       m_tex[kRing];
    winrt::com_ptr<ID3D11RenderTargetView> m_rtv[kRing];
    winrt::com_ptr<ID3D11DeviceContext1>  m_context1;

    UINT   m_width = 0, m_height = 0;
    double m_intervalMs = 0.0;
    double m_pixelsPerMs = 0.0;
    double m_startMs = 0.0;
    double m_nextDueMs = 0.0;
    uint64_t m_frameId = 0;
    int    m_next = 0;
};

} // namespace fbv2
