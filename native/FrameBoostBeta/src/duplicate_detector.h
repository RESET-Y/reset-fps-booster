#pragma once
#include <d3d11.h>
#include <cstdint>
#include <vector>

namespace FrameBoostBeta {

// Detects when a freshly captured frame is pixel-identical to the previous
// one.
//
// Why this exists: Windows Graphics Capture delivers a frame whenever the
// target WINDOW is recomposited, which is not the same thing as its CONTENT
// changing. Watching a 30 FPS video in a browser that recomposites at
// 45-60 Hz produces a stream like A A B B C C - and feeding those pairs
// into motion estimation yields a zero motion field, so the "generated"
// frame is a byte-for-byte copy of a real one. The output frame counter
// still doubles while nothing on screen actually gets smoother, which is
// exactly what was observed live on a YouTube video after the same setup
// visibly worked on a Twitch stream.
//
// Detection is done by copying a handful of small pixel patches spread
// across the frame into a staging texture and hashing them. Cheap (a few KB
// per frame), and enough to distinguish real content changes from
// recomposition of identical content.
class DuplicateDetector {
public:
    // Returns true if this frame carries no meaningful new content compared
    // to the previous one. On any failure it returns false (treat as "not a
    // duplicate") so a detection problem can never stall the pipeline.
    bool IsDuplicate(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame);

    // Mean absolute per-channel difference (0-255) between the last two
    // sampled frames - exposed so the tolerance can be judged from real
    // telemetry instead of guessed.
    double LastDifference() const { return m_lastDifference; }

    ~DuplicateDetector();

private:
    bool EnsureResources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDesc);

    // Coverage matters more than patch size: with only 12 patches (0.08% of
    // a 1440p screen) a playing video that happened to sit between them was
    // read as "nothing changed", and the boosted output froze whenever the
    // mouse stopped moving. A denser grid of smaller patches samples the
    // same number of pixels while actually covering the screen.
    static constexpr UINT kPatchSize = 8;
    static constexpr UINT kPatchesX = 12;
    static constexpr UINT kPatchesY = 8;

    // Compared with a tolerance rather than exactly: a browser scaling a
    // video re-encodes essentially identical content with tiny per-pixel
    // differences, so an exact hash reported every recomposition as "new
    // content" and defeated the whole check.
    static constexpr double kDuplicateThreshold = 1.2; // mean abs difference per channel (0-255)

    ID3D11Texture2D* m_staging = nullptr;    // holds all patches side by side
    UINT m_frameWidth = 0, m_frameHeight = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> m_lastPatches;
    double m_lastDifference = -1.0;
};

} // namespace FrameBoostBeta
