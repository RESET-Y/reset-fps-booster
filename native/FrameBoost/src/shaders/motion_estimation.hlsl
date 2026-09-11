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
//
// RAISED BACK TO 16 after measuring the cost in a real game at 2560x1440:
// motion estimation took 4.2-11.8 ms per real frame, which at ~45 frames a
// second is 190-530 ms of GPU time every second - and it was why only ~40 of
// ~65 frames carrying new content ever reached the estimator. The block size
// sets the dispatch grid: at 8 px a 2560x1440 frame is 320x180 = 57,600 thread
// groups, at 16 px it is 160x90 = 14,400. Four times less work, for the same
// 16 samples per candidate.
//
// The cost is granularity: a block shares one motion vector, so the boundary
// between a moving object and its background is 16 px of content forced into
// one answer instead of 8. That is why it was halved originally - but a
// sharper field that arrives for half the frames is worth less than a coarser
// one that arrives for all of them.
//
// 16 px, and this time the reason is not our own frame budget but THE GAME'S.
//
// Logging our GPU time against the source frame rate second by second, during
// real movement in a game capped at 72 FPS:
//
//   motion estimation 1.5-4.2 ms   ->  source 50-72 FPS, mean ~63
//   motion estimation 9.3-11.1 ms  ->  source 50-67 FPS, mean ~57
//
// We are not a bystander on this GPU. At 8 px the estimator costs 2-11 ms of
// every frame and the game gives up about six frames a second to pay for it -
// frames the doubling then has to earn back before it is worth anything. At
// 16 px the same measurement reads 0.70 ms.
//
// Finer blocks really are better, and 8 px is the right choice on a GPU with
// room to spare. Sharing one with the game it is boosting is not that.
//
// BACK TO 8 px, now that the search runs on mip 1 and costs 0.5-0.7 ms.
//
// Reported from a live game: moving bots dragging a trail behind them. That is
// what one motion vector per 16 pixels does at the edge of a small moving
// object - the block holds both the object and the background it is passing
// over, both get the same vector, and whichever of the two is wrong smears.
// Halving the block quarters the area where object and background are forced
// into a single answer.
//
// Affordable again for a different reason than the last time: the search is
// four times cheaper per block on mip 1, so four times as many blocks costs
// about what 16 px cost at full resolution.
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
// Reduced from 6 to 3 after measuring where the frame time actually goes.
//
// This stage costs 12.4 ms of GPU time per frame, measured on an idle desktop
// with 0.19% of blocks moving - the price is paid for the search itself, not
// for the content. Against a real frame interval of 16 ms that is fatal: a
// generated frame has to be finished inside HALF an interval, so the engine
// kept standing aside for lack of GPU room and the output dropped to nothing,
// or to 36 -> 73 when it did run. Reported as feeling like 30 fps, which is
// what 28 ms of generation cost per frame actually is.
//
// The arithmetic behind the cost: at 2560x1440 this dispatches 57,600 blocks,
// and at radius 6 each searches 169 candidates - 9.7 million block matches per
// frame. Radius 3 leaves 49, a third of the work.
//
// Quality should barely notice, because this stage does not FIND motion - the
// two coarser levels already did, with a reach of 240 px. It only refines
// their answer, and a refinement window of +-3 texels is +-6 full-resolution
// pixels around a vector that is already close.
static const int kSearchRadius = 3;
static const int kSearchWindow = kSearchRadius * 2 + 1; // 7
static const int kCandidateCount = kSearchWindow * kSearchWindow; // 49

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

// Below this, a block is identical to where it already was: 16 samples across
// 3 channels, so 0.5 is a mean absolute difference of about 0.01 per channel -
// compression noise and nothing more.
static const float kStaticBlockSad = 0.5;

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

// The same "did this block move at all" question, asked at FULL resolution.
//
// The search runs on mip 1, and half resolution erases fine detail: a
// sidebar of small text washes into a grey smear, every candidate then scores
// about the same, and the neighbourhood bias hands the block whatever its
// moving neighbours are doing. Seen in a dumped frame: a Twitch page.s left
// column, which never moved at all, came out doubled and smeared while the
// video in the middle of the screen interpolated cleanly.
//
// At full resolution that same block matches itself almost exactly, so this
// one extra comparison settles it. It costs 16 samples per block, once,
// against 169 candidates for the search itself.
groupshared float g_zeroMotionSadFull;

// THE SEARCH RUNS ON MIP 1 - half resolution - while a block still covers the
// same 16 full-resolution pixels, so the motion field keeps its granularity.
//
// Reason, measured in a GPU-bound game: this dispatch reported 30 ms where the
// same work costs 0.50 ms on an idle GPU. The arithmetic is not the problem,
// the memory traffic is: 169 candidates x 16 samples per block, every one a
// texture read. At half resolution the same block is eight texels across
// instead of sixteen, so a search window fits in a quarter of the footprint.
//
// It also doubles the reach for free: a refinement of six texels here is twelve
// full-resolution pixels, where before it was six.
//
// What it costs: motion is resolved to two-pixel precision instead of one. For
// deciding where a sixteen-pixel block went, that sits below the noise floor of
// the estimate itself.
static const int kSearchMip = 1;
static const int kMipScale = 2;                                        // 1 << kSearchMip
static const int kBlockTexels = kBlockSize / kMipScale;                // 8 texels
static const int kSampleStrideTexels = kBlockSampleStride / kMipScale; // 2 -> 4x4 = 16 samples

// Zero motion, judged on the real pixels rather than the halved ones.
float BlockSADFullRes(int2 blockOriginPixels)
{
    const int2 maxPixel = int2((int)FrameWidth, (int)FrameHeight) - 1;

    float sad = 0.0;
    [unroll]
    for (int y = 0; y < kBlockSize; y += kBlockSampleStride)
    {
        [unroll]
        for (int x = 0; x < kBlockSize; x += kBlockSampleStride)
        {
            const int2 p = min(blockOriginPixels + int2(x, y), maxPixel);
            float3 currColor = CurrFrame.Load(int3(p, 0)).rgb;
            float3 prevColor = PrevFrame.Load(int3(p, 0)).rgb;
            sad += dot(abs(currColor - prevColor), float3(1.0, 1.0, 1.0));
        }
    }
    return sad;
}

float BlockSAD(int2 currBlockOriginTexels, int2 candidateOffsetTexels)
{
    const int2 mipMax = int2(max((int)FrameWidth / kMipScale, 1),
                             max((int)FrameHeight / kMipScale, 1)) - 1;

    float sad = 0.0;
    [unroll]
    for (int y = 0; y < kBlockTexels; y += kSampleStrideTexels)
    {
        [unroll]
        for (int x = 0; x < kBlockTexels; x += kSampleStrideTexels)
        {
            int2 currTexel = currBlockOriginTexels + int2(x, y);
            int2 prevTexel = currTexel + candidateOffsetTexels;

            if (currTexel.x > mipMax.x || currTexel.y > mipMax.y)
                continue;

            float3 currColor = CurrFrame.Load(int3(currTexel, kSearchMip)).rgb;
            float3 prevColor = PrevFrame.Load(int3(clamp(prevTexel, int2(0, 0), mipMax), kSearchMip)).rgb;

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
    // Everything in this stage is counted in MIP 1 TEXELS; only the vector
    // written at the end is converted back to full-resolution pixels.
    int2 blockOrigin = int2(groupId.xy) * kBlockTexels;

    // Seed from the coarse stage: which coarse block this fine block sits in.
    // The coarse vector is in full-resolution pixels, so it is halved to land
    // in this stage's coordinates.
    int2 coarseIndex = clamp(int2(groupId.xy) / kCoarseBlockRatio,
        int2(0, 0), int2(max(CoarseWidth, 1u), max(CoarseHeight, 1u)) - 1);
    int2 seed = int2(round(CoarseMotionVectors.Load(int3(coarseIndex, 0)).xy))
        * kCoarseToFineScale / kMipScale;

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
    if (groupIndex == 2)
    {
        g_zeroMotionSadFull = BlockSADFullRes(int2(groupId.xy) * kBlockSize);
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

        // Full resolution has the last word on standing still. A block this
        // close to identical where it already is did not move, whatever the
        // half-resolution search made of it.
        if (g_zeroMotionSadFull < kStaticBlockSad)
        {
            bestOffset = int2(0, 0);
            bestMatchSad = min(bestMatchSad, g_zeroMotionSadFull);
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
        // Back to full-resolution pixels: everything above was counted in
        // mip-1 texels, and every consumer of this field - the smoothing pass,
        // the interpolation shader, the statistics - works in real pixels.
        MotionVectors[groupId.xy] = float4(float2(bestOffset * kMipScale),
            max(matchSad, 0.0) / kSamplesPerCandidate, 0.0);
    }
}
