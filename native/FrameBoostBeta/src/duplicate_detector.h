#pragma once
#include <d3d11.h>
#include <cstdint>
#include <vector>

namespace FrameBoostBeta {

// Detects when a freshly captured frame carries no new content compared to
// the previous one.
//
// Why this exists: Windows Graphics Capture delivers a frame whenever the
// captured surface is recomposited, which is not the same thing as its
// CONTENT changing. Watching a 30 FPS video in a browser that recomposites
// at 45-60 Hz produces a stream like A A B B C C - and feeding those pairs
// into motion estimation yields a zero motion field, so the "generated"
// frame is a byte-for-byte copy of a real one. The output frame counter
// still rises while nothing on screen actually gets smoother.
//
// How it works: both frames are reduced on the GPU to a thumbnail a few
// thousand pixels large, and the thumbnails are compared on the CPU.
//
// This replaced sparse patch sampling, which failed for the reason patch
// sampling always fails - coverage. Even a dense 96-patch grid touched only
// 0.17% of a 1440p screen, so a video playing in a window fell between the
// sample points: measured live at 46 native FPS, 38 frames per second were
// declared duplicates with a reported difference of exactly 0, and only ~8
// frame pairs per second were left to generate from. A thumbnail covers
// 100% of the frame - every pixel contributes to some output pixel - so
// motion anywhere on screen is seen, while the comparison stays small
// enough to do per frame.
class DuplicateDetector {
public:
    // Returns true if this frame carries no meaningful new content compared
    // to the previous one. On any failure it returns false (treat as "not a
    // duplicate") so a detection problem can never stall the pipeline.
    bool IsDuplicate(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame);

    // Largest per-tile mean absolute channel difference (0-255) between the
    // last two frames - exposed so the tolerance can be judged from real
    // telemetry instead of guessed.
    double LastDifference() const { return m_lastDifference; }

    ~DuplicateDetector();

private:
    bool EnsureResources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDesc);

    // Thumbnail size. 64x36 keeps a 16:9 frame's aspect ratio and is small
    // enough that the CPU-side comparison is a few thousand byte subtractions
    // per frame. Each thumbnail pixel is the average of a large block of the
    // real frame, so a small moving object is diluted - which is why the
    // comparison below works on tiles and takes the maximum rather than
    // averaging over the whole thumbnail.
    static constexpr UINT kThumbWidth = 64;
    static constexpr UINT kThumbHeight = 36;

    // The thumbnail is compared in tiles, and the LARGEST tile difference
    // decides. Averaging over the whole image fails exactly where it matters:
    // a video playing in a small window surrounded by a static desktop is
    // averaged down into "nothing changed". (Measured with the previous patch
    // implementation: 23-50 frames per second wrongly discarded.)
    static constexpr UINT kTilesX = 8;
    static constexpr UINT kTilesY = 6;

    // Compared with a tolerance rather than exactly: a browser scaling a
    // video re-encodes essentially identical content with tiny per-pixel
    // differences, so an exact comparison reported every recomposition as
    // "new content" and defeated the whole check.
    // Lowered from 1.2 when the comparison moved from full-resolution patches
    // to a thumbnail. The number means something quite different there: each
    // thumbnail texel averages a 64x64 block of real pixels, which shrinks
    // differences by roughly that factor. Carrying 1.2 over discarded frames
    // that plainly were not duplicates - measured live at 0.44 and 0.80 on a
    // playing video with 89% of blocks in motion, costing 12-47 real frames
    // per second.
    static constexpr double kDuplicateThreshold = 0.3; // mean abs difference per channel (0-255)

    // Full-size intermediate carrying a mip chain: the frame is copied into
    // mip 0 and GenerateMips does the reduction on the GPU, which is both
    // faster and better quality than sampling a handful of pixels ourselves.
    ID3D11Texture2D* m_mipSource = nullptr;
    ID3D11ShaderResourceView* m_mipSRV = nullptr;
    ID3D11Texture2D* m_thumbTarget = nullptr; // thumbnail-sized, copied from the matching mip
    ID3D11Texture2D* m_staging = nullptr;     // CPU-readable copy of the thumbnail
    UINT m_mipLevels = 0;
    UINT m_thumbMipLevel = 0;
    UINT m_thumbActualWidth = 0, m_thumbActualHeight = 0;

    UINT m_frameWidth = 0, m_frameHeight = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> m_lastThumb;
    double m_lastDifference = -1.0;
};

} // namespace FrameBoostBeta
