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

    // Share of MOVING blocks whose vector sits on the edge of the search
    // window. Those blocks did not find their match - the search ran out of
    // room - so their vector is the closest wrong answer rather than the
    // right one. This is the number that says whether the search radius is
    // the quality limiter or not.
    double SaturatedBlockPercent() const { return m_saturatedPercent; }

    // How well the winning candidate actually matched, as a mean absolute
    // difference per colour channel. This separates two problems that look
    // identical on screen but need opposite fixes: a wrong vector (a good
    // match existed and was missed) versus content that was not present in
    // the previous frame at all, which no interpolation can recover.
    double MeanMatchError() const { return m_meanMatchError; }
    double MaxMatchError() const { return m_maxMatchError; }
    double PoorMatchPercent() const { return m_poorMatchPercent; }

    ~MotionStats();

private:
    // Sampled often enough that a fast turn is actually caught - the readback
    // stalls the pipeline, so this is still far from every frame.
    static constexpr int kSampleIntervalFrames = 30;

    ID3D11Texture2D* m_staging = nullptr;
    UINT m_width = 0, m_height = 0;
    int m_frameCounter = 0;
    double m_movingPercent = -1.0;
    double m_meanMagnitude = -1.0;
    double m_maxMagnitude = -1.0;
    double m_saturatedPercent = -1.0;
    double m_meanMatchError = -1.0;
    double m_maxMatchError = -1.0;
    double m_poorMatchPercent = -1.0;

    // Must match kSearchRadius in motion_estimation.hlsl.
    // Total reach of the pyramid search: coarse stage 48 px (radius 12 on a
    // quarter-resolution mip) plus the fine stage refining 6 px around it.
    // Total reach of the three-level pyramid: 192 px from the coarsest level
    // (radius 12 on a sixteenth-resolution mip), plus 24 px of refinement at
    // quarter resolution and 6 px at full resolution.
    static constexpr double kSearchRadiusPixels = 222.0;
};

} // namespace FrameBoostBeta
