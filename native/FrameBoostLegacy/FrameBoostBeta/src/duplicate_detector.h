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

    // Thumbnail size, raised from 64x36 because that was throwing away frames
    // that were plainly not duplicates.
    //
    // Measured in CS2 running uncapped, with the game actively rendering:
    //
    //   10:51:17  produced/retrieved 127/127   duplicates 91.6/s   native 26.9
    //   10:51:18  produced/retrieved 129/129   duplicates 78.8/s   native 46.9
    //
    // Windows delivered 127 frames, we retrieved all 127 and lost none - and
    // then discarded 92 of them here. A game rendering at 127 fps does not
    // produce 92 pixel-identical frames a second. The detector was wrong, and
    // not marginally.
    //
    // Why: at 64x36 each thumbnail texel is the average of a 40x40 block of a
    // 2560x1440 frame. Standing still and turning slightly moves the picture by
    // a few pixels, which that averaging erases completely. 192x108 puts a
    // texel at about 13x13 pixels - still cheap (83 KB, a few tens of thousands
    // of byte subtractions) and no longer blind to small movement.
    //
    // The threshold below is deliberately NOT changed with it. Less dilution
    // makes real differences read LARGER against the same number, which is the
    // direction that was needed. Moving both at once would leave neither
    // measured - and the history in this file is two rounds of exactly that.
    static constexpr UINT kThumbWidth = 192;
    static constexpr UINT kThumbHeight = 108;

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
    // 0.3 -> 0.1, and this time from the two measured populations rather than
    // from reasoning about them.
    //
    // Raising the thumbnail to 192x108 changed what this number means, and I
    // left it at 0.3 with the argument that less dilution makes real
    // differences read larger. The measurement says otherwise:
    //
    //   truly identical frames (CS2 with a frozen capture surface, proved
    //   byte-identical across 114,012 sampled offsets)   0.018 - 0.047
    //   real content, quiet scene, 0.1-6.4% blocks moving  0.21 - 0.27
    //
    // 0.3 sits ABOVE the second population, so ordinary quiet gameplay was
    // being discarded wholesale: 63 of 70 arrivals called duplicates, native
    // FPS down to 2-5, output collapsing from 143 to 12-26. That is the frame
    // rate drop being reported.
    //
    // 0.1 sits in the gap between the two, a factor of two clear on each side.
    // Below it lies only the genuinely frozen surface and the compression
    // noise this check exists to reject; above it lies real content.
    static constexpr double kDuplicateThreshold = 0.1; // mean abs difference per channel (0-255)

    // One compute dispatch shrinks the frame straight to thumbnail size.
    //
    // This used to copy the whole frame into a mip-capable texture and call
    // GenerateMips - a full twelve-level pyramid of a 2560x1440 image, roughly
    // a hundred times a second, to produce 40x22 pixels. Measured at 0.7-1.8 ms
    // per arrival, and 2.8-3.0 ms once a readback stall stopped hiding it.
    ID3D11ComputeShader* m_thumbnailCS = nullptr;
    ID3D11Buffer* m_dimsCB = nullptr;
    ID3D11Texture2D* m_thumbTarget = nullptr;    // thumbnail-sized, written by the shader
    ID3D11UnorderedAccessView* m_thumbUAV = nullptr;
    ID3D11ShaderResourceView* m_frameSRV = nullptr; // view onto the frame being checked
    ID3D11Texture2D* m_frameSRVSource = nullptr;    // which texture that view belongs to
    // Two staging copies, read one frame behind the one being written.
    //
    // Mapping the texture the GPU was just told to fill means waiting for it,
    // and that wait was measured at 0.9-1.8 ms on every arrival - about 120 ms
    // of every second at ~100 arrivals. Reading the PREVIOUS copy costs
    // nothing, because that work finished long ago.
    //
    // The verdict is then one frame old: it says whether the previous frame
    // differed from the one before it. On a still screen that is the same
    // answer; it can only be wrong on the single frame where motion starts or
    // stops, and being wrong there costs one processed duplicate or one
    // skipped frame - far cheaper than stalling the pipeline on every frame.
    ID3D11Texture2D* m_staging[2] = { nullptr, nullptr };
    int m_writeIndex = 0;
    bool m_stagingFilled[2] = { false, false };
    bool m_lastVerdict = false;
    UINT m_thumbActualWidth = 0, m_thumbActualHeight = 0;

    UINT m_frameWidth = 0, m_frameHeight = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> m_lastThumb;
    double m_lastDifference = -1.0;
};

} // namespace FrameBoostBeta
