#pragma once
#include <d3d11.h>

namespace FrameBoostBeta {

// Reads the motion field back to the CPU occasionally and reports how much
// of the frame is actually moving.
//
// Why this exists: after the duplicate-detection fix the mouse cursor
// visibly gained smoothness while a playing video did not change at all.
// Both explanations are plausible from the outside - either the video's own
// update rate is below our capture rate (so most frame pairs hold an
// identical video region and there is genuinely nothing to interpolate), or
// motion estimation is failing on video content and returning a zero field.
// Those demand opposite fixes, so the field itself has to be measured
// rather than argued about.
//
// Sampled every N frames, not every frame: the readback is a full GPU
// pipeline stall, which is acceptable as diagnostics but not on the hot
// path.
class MotionStats {
public:
    // Returns true when fresh numbers were produced by this call.
    bool SampleIfDue(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* motionTex);

    double MovingBlockPercent() const { return m_movingPercent; }  // blocks with a non-zero vector
    double MeanMagnitudePixels() const { return m_meanMagnitude; } // averaged over MOVING blocks only
    double MaxMagnitudePixels() const { return m_maxMagnitude; }

    ~MotionStats();

private:
    static constexpr int kSampleIntervalFrames = 120; // ~1-2 seconds of output

    ID3D11Texture2D* m_staging = nullptr;
    UINT m_width = 0, m_height = 0;
    int m_frameCounter = 0;
    double m_movingPercent = -1.0;
    double m_meanMagnitude = -1.0;
    double m_maxMagnitude = -1.0;
};

} // namespace FrameBoostBeta
