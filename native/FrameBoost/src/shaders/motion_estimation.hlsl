// Real block-matching optical flow, entirely GPU-side. For each block in
// the CURRENT frame, search a window in the PREVIOUS frame for the offset
// that minimizes sum-of-absolute-differences (SAD). The winning offset is
// the block's motion vector. This is a genuine, if simple, motion-
// estimation algorithm - not a placeholder and not a blend.
//
// v0.3 rewrite: the original version ran the ENTIRE per-block candidate
// search (169 candidate offsets x 16 samples each) sequentially on a SINGLE
// GPU thread ([numthreads(1,1,1)]) - meaning every block wasted essentially
// all of a GPU warp's parallelism. Confirmed as the dominant cost via live
// testing: motion estimation alone was blocking the whole FrameBoost Beta
// pipeline down to ~3 FPS under any real GPU contention. This version
// spreads the candidate search across one GPU thread PER CANDIDATE OFFSET,
// with a simple groupshared reduction to find the best one - the same
// total work, done in true parallel instead of one thread at a time.

Texture2D<float4> PrevFrame : register(t0);
Texture2D<float4> CurrFrame : register(t1);
RWTexture2D<float4> MotionVectors : register(u0);

// 16px blocks with a matching 4px sampling stride: 4x4 = 16 samples per
// candidate either way, so the stride scales with the block and only the
// number of blocks changes.
//
// This was 32px for most of the project's life, and for a good reason: the
// original 16px version cost ~35 ms of GPU time per frame at 2560x1440 and
// capped the loop at ~14 FPS. What made 16px affordable again was the
// parallel rewrite above (one thread per candidate rather than one thread
// per block), which cut motion estimation from ~350 ms to 0.09 ms - four
// times as many blocks is a cost that budget can absorb.
//
// Finer blocks matter because a block shares ONE motion vector: at 32px,
// a moving object and the static background behind it are forced into the
// same vector wherever they meet, and one of the two is always wrong.
// Halved to 8px after live testing in a game: during fast turning the whole
// image moves 90 px between real frames, with peaks over 200. A block shares
// one motion vector, so at 16px the boundary between a moving object and its
// background was 16 px of content forced into one wrong answer. At 8px that
// error zone is a quarter the area.
//
// Four times as many blocks, but motion estimation runs once per REAL frame -
// about 50 times a second, with a 20 ms budget each - and it was costing
// 0.3-2.3 ms.
static const int kBlockSize = 8;
static const int kBlockSampleStride = 2; // 4x4 = 16 samples per candidate, as before
// FINE stage of the pyramid. The search no longer starts from zero: it
// starts from the coarse stage's result (motion_estimation_coarse.hlsl,
// which searches +-48 px on a quarter-resolution mip) and only refines it
// locally.
//
// That is why the radius here is small again. Growing the single-stage
// radius had run out of road: one thread per candidate means radius 15
// already hits D3D11's 1024-thread group limit, and measurement still
// showed 18-23% of moving blocks pinned to the edge of a radius-12 window
// during motion - ~2600 blocks per frame whose vector was wrong by
// construction. Reach is now 48 + 6 = 54 px while costing LESS: 169
// candidates per block instead of 625.
static const int kSearchRadius = 6;
static const int kSearchWindow = kSearchRadius * 2 + 1; // 13
static const int kCandidateCount = kSearchWindow * kSearchWindow; // 169

// Cost per pixel of straying from the neighbourhood`s estimate. Deliberately
// small: a block matching 16 samples across 3 channels typically scores well
// under 1.0 when it matches cleanly, so 0.01 per pixel adds at most 0.06 over
// the whole search window - enough to settle ties, not enough to override a
// real match.
static const float kNeighbourhoodBias = 0.01;

// How much better "not moving" has to be before it is believed. At 1.15 it
// needs to beat the searched winner by 15%, which a genuinely static overlay
// does by a wide margin (it matches almost exactly) while a moving block in a
// noisy scene does not.
static const float kZeroMotionMargin = 1.15;

// One coarse block covers 4x4 fine blocks (64px vs 16px), and coarse
// vectors are stored in mip-2 texels.
// One coarse block spans 64 full-resolution pixels, so with 8px fine blocks
// it now covers 8 of them per axis rather than 4.
static const int kCoarseBlockRatio = 8;
static const int kCoarseToFineScale = 4;

Texture2D<float4> CoarseMotionVectors : register(t2);

cbuffer FrameDims : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint CoarseWidth;
    uint CoarseHeight;
};

groupshared float g_sad[kCandidateCount];
groupshared float g_zeroMotionSad;

float BlockSAD(int2 currBlockOrigin, int2 candidateOffset)
{
    float sad = 0.0;
    [unroll]
    for (int y = 0; y < kBlockSize; y += kBlockSampleStride)
    {
        [unroll]
        for (int x = 0; x < kBlockSize; x += kBlockSampleStride)
        {
            int2 currPixel = currBlockOrigin + int2(x, y);
            int2 prevPixel = currPixel + candidateOffset;

            if (currPixel.x >= (int)FrameWidth || currPixel.y >= (int)FrameHeight)
                continue;

            float3 currColor = CurrFrame.Load(int3(currPixel, 0)).rgb;
            float3 prevColor = PrevFrame.Load(int3(clamp(prevPixel, int2(0, 0),
                int2(FrameWidth - 1, FrameHeight - 1)), 0)).rgb;

            sad += dot(abs(currColor - prevColor), float3(1.0, 1.0, 1.0));
        }
    }
    return sad;
}

// One thread GROUP per block (dispatched width/kBlockSize x height/kBlockSize),
// one THREAD per candidate offset within the group - the real parallelism fix.
[numthreads(kSearchWindow, kSearchWindow, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    int2 blockOrigin = int2(groupId.xy) * kBlockSize;

    // Seed from the coarse stage: which coarse block this fine block sits in,
    // scaled from mip-2 texels back to full-resolution pixels.
    int2 coarseIndex = clamp(int2(groupId.xy) / kCoarseBlockRatio,
        int2(0, 0), int2(max(CoarseWidth, 1u), max(CoarseHeight, 1u)) - 1);
    int2 seed = int2(round(CoarseMotionVectors.Load(int3(coarseIndex, 0)).xy)) * kCoarseToFineScale;

    int2 refinement = int2(groupThreadId.xy) - kSearchRadius;
    int2 candidateOffset = seed + refinement;

    // A candidate that agrees with the neighbourhood wins ties.
    //
    // Pure lowest-SAD picking is unstable wherever the picture is flat - sky,
    // walls, dust - because many candidates match almost equally well and
    // noise decides which one wins. The field then flickers from frame to
    // frame and has to be smoothed afterwards, which blurs real motion along
    // with the noise.
    //
    // The coarse level's result is a good predictor of the neighbourhood: it
    // covers 64 pixels, so it describes the local motion rather than this
    // block's own. Charging a small cost for straying from it settles ties in
    // favour of coherence, while a genuinely better match - anything beyond
    // the noise floor - still wins outright.
    const float distanceFromSeed = length(float2(refinement));
    g_sad[groupIndex] = BlockSAD(blockOrigin, candidateOffset)
        + kNeighbourhoodBias * distanceFromSeed;
    // ZERO MOTION is always a candidate, whatever the neighbourhood says.
    //
    // The fine stage may only refine +-6 px around the coarse prediction, and
    // that makes motion DISCONTINUITIES unrepresentable: during a 90 px camera
    // turn the prediction is 90 px, so a block that is actually still - the
    // HUD, the crosshair, any screen-space overlay - would have to say 90 px
    // away from the prediction to be right, and simply cannot.
    //
    // Seen directly in a dumped frame: the score panel, the ammo counter and
    // the "RESET" label were smeared and doubled while the scene behind them
    // interpolated cleanly. They do not move at all; the engine was dragging
    // them along with the camera because it had no way to say "this one is
    // still". No metric showed it - the blocks reported a plausible vector and
    // a mediocre match, indistinguishable from ordinary difficulty.
    //
    // One extra evaluation per block, and it costs the same whichever thread
    // does it since the rest are waiting at the barrier anyway.
    if (groupIndex == 1)
    {
        g_zeroMotionSad = BlockSAD(blockOrigin, int2(0, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    // Cheap serial reduction over already-computed SAD values (no more
    // texture sampling here) - negligible cost compared to the search itself.
    if (groupIndex == 0)
    {
        float bestSad = g_sad[0];
        int bestIndex = 0;
        for (int i = 1; i < kCandidateCount; ++i)
        {
            if (g_sad[i] < bestSad)
            {
                bestSad = g_sad[i];
                bestIndex = i;
            }
        }

        // Relative to the coarse seed, so the final vector is seed + refinement.
        int2 bestOffset = seed + int2(bestIndex % kSearchWindow, bestIndex / kSearchWindow) - kSearchRadius;
        int2 bestRefinementForError = int2(bestIndex % kSearchWindow, bestIndex / kSearchWindow) - kSearchRadius;
        float bestMatchSad = bestSad - kNeighbourhoodBias * length(float2(bestRefinementForError));

        // Standing still wins only when it is CLEARLY better, so ordinary
        // noise in a moving scene cannot make blocks stick.
        if (g_zeroMotionSad * kZeroMotionMargin < bestMatchSad)
        {
            bestOffset = int2(0, 0);
            bestMatchSad = g_zeroMotionSad;
        }

        // .z carries how WELL that best candidate actually matched, as a mean
        // absolute difference per colour channel (0 = identical, 1 = maximal).
        // The vector alone cannot distinguish a confident match from the least
        // bad of a set of equally wrong ones - and those are different problems
        // with different fixes: a mis-estimated vector can be corrected, while
        // content that was simply not present in the previous frame cannot be
        // interpolated at all.
        // The neighbourhood bias is taken back out, so .z stays a measurement
        // of how well the block actually matched rather than of how far its
        // winner sat from the seed. The smoothing pass and the diagnostics
        // both read this as real match quality.
        const int2 bestRefinement = int2(bestIndex % kSearchWindow, bestIndex / kSearchWindow) - kSearchRadius;
        const float matchSad = bestSad - kNeighbourhoodBias * length(float2(bestRefinement));

        const float kSamplesPerCandidate = 16.0 * 3.0; // 4x4 samples, 3 channels
        MotionVectors[groupId.xy] = float4(float2(bestOffset), max(matchSad, 0.0) / kSamplesPerCandidate, 0.0);
    }
}
