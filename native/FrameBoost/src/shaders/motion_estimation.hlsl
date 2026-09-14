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
// 3. Widening to 6 was tried against a flickering motion field in a
// low-altitude pass and did not calm it, while generation cost went from 3 ms
// to 8-11 ms against an 8.3 ms deadline.
//
// So the flicker there is not the fine stage failing to reach the right
// answer - it finds what it looks for, and looking further finds no better.
// What is left is that the answer itself is ambiguous: ground texture at
// speed offers many near-equal matches, and which one wins is decided by
// noise.
static const int kSearchRadius = 3;
static const int kSearchWindow = kSearchRadius * 2 + 1; // 7
static const int kCandidateCount = kSearchWindow * kSearchWindow; // 49

// Cost per pixel of straying from the neighbourhood.s estimate.
//
// Raised from 0.01 to 0.05 against an ambiguous match rather than a missing
// one. Ground texture at speed, and foliage at any speed, offer a block dozens
// of nearly identical candidates; the SAD surface is almost flat and noise
// decides which wins. A different one wins next frame, the same object is
// displaced differently in consecutive generated frames, and that is what
// reads as a double image.
//
// Searching wider does not help - that was measured, at 8-11 ms against an
// 8.3 ms deadline, with no improvement - because the problem is not that the
// right answer is out of reach. It is that several answers look equally
// right. When they do, the one agreeing with the neighbourhood should win.
//
// At 0.05 the whole search window adds at most 0.3, which still loses to any
// genuinely better match: a clean block scores well under 1.0 across 16
// samples and 3 channels, and a real difference between candidates is far
// larger than that.
// 0.15, up from 0.05 - and only safe to raise now.
//
// Measured from a dumped fast turn once the viewmodel was fixed: the HUD is
// pixel-identical, the snow field is flawless, and the damage sits exactly on
// content that is high-contrast and SELF-SIMILAR - facade panels, repeated
// horizontal lines, lattice structures. That is the classic failure of block
// matching on repetitive structure: many displacements match almost equally
// well, the SAD surface is nearly flat, and noise decides which candidate wins
// - differently in each block and differently in each frame. On screen that is
// the 8 px mosaic.
//
// At 0.05 the whole search window adds at most 0.3, which cannot outvote that
// noise. At 0.15 it adds up to 0.9, while a genuinely better match - a clean
// block scores well under 1.0 across 16 samples and 3 channels - still wins
// outright.
//
// Why it could not be raised before: while the search centre was always the
// coarse seed, a strong bias only pulled a viewmodel HARDER into the motion of
// the world behind it. Now that the centre is chosen between the coarse seed,
// zero and the temporal predictors before the search runs, the bias reinforces
// whichever centre actually won - on the weapon, that is zero.
//
// The risk it carries is the opposite one: pulled too far, genuinely different
// motions get averaged together at object boundaries. Watch the edge between a
// moving object and its background.
static const float kNeighbourhoodBias = 0.15;

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
static const int kCoarseBlockRatio = 4;
static const int kCoarseToFineScale = 4;

Texture2D<float4> CoarseMotionVectors : register(t2);
// Last frame.s finished motion field, used as a PREDICTOR - see the candidate
// evaluation below. Zero on the first frame after a resolution change.
Texture2D<float4> PreviousMotionField : register(t3);

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

// PREDICTED candidates: vectors that were right somewhere else, offered to the
// search for free.
//
// The refinement window is +-3 texels, which is +-6 full-resolution pixels
// around whatever the coarse level proposed. For a camera pan that is plenty,
// because the coarse level finds the pan. For a bot running across a still
// background it is hopeless: a coarse block covers 64 pixels and is dominated
// by the motionless ground around the bot, so the seed says "still" and the
// fine stage cannot reach the bot.s real 20 px however well it refines. The
// bot then gets a near-zero vector, the generated frame leaves it almost
// where the real frame has it, and the two show up as one object drawn twice
// a few pixels apart - reported exactly as duplicating itself slightly offset.
//
// Widening the search is not available: radius 6 alongside the backward field
// measured 11 ms against a 6.9 ms budget and the engine stood aside entirely.
//
// So instead of searching wider, the search is given good guesses. This is the
// 3DRS idea from television frame-rate conversion: a small candidate set drawn
// from where this motion has already been seen - the same block one frame ago,
// and its neighbours one frame ago. A bot that was tracked once stays tracked,
// and a correct vector spreads sideways across the object a block per frame,
// without any candidate ever costing more than one block comparison.
// Switched OFF after measuring it live: no visible change to the duplicated
// object at all, while generation cost went from 3.7 to between 3.5 and 9.6 ms
// and crossed the 6.9 ms budget repeatedly - including one drop to 32 native
// FPS when the engine stood aside. Five predictors read five arbitrary places
// in the picture per block, which is exactly the access pattern a GPU cache
// handles worst.
//
// The code is kept because the reasoning behind it still holds - a bot moving
// 20 px genuinely cannot be represented by a +-6 px refinement of a seed that
// says "still" - but the conclusion has to be that this is not WHY the object
// duplicates, since giving the search the right vector for free changed
// nothing. Re-enabling it needs a cheaper form (one predictor, or only where
// the coarse seed matches badly) and a reason to expect a different result.
// ON again, and the reason it was off no longer applies.
//
// It was switched off at midday because it "changed nothing visible" while
// pushing generation cost past the budget. Both halves of that have since
// turned out to be circumstances rather than facts: at the time two
// confidence constants were discarding roughly 90% of every generated frame,
// so nothing about the field COULD become visible - and the pipeline was
// pressed against its deadline, where it now takes 9-16% of the card.
//
// What it is for is exactly the problem now on screen. Displaying the motion
// field during gentle flight shows heavy flicker over trees and lighter
// flicker over fields: foliage offers a block dozens of nearly identical
// matches, noise picks the winner, and a different one wins next frame. The
// same object is then displaced differently in consecutive generated frames,
// which is what reads as a double image.
//
// Offering the vector this block had last frame as a candidate means a block
// that was right stays right, instead of being re-decided from scratch
// against a field of ties. That is the 3DRS idea from television frame-rate
// conversion, and ambiguity in repetitive texture is the case it exists for.
static const bool kUsePredictors = true;
static const int kPredictorCount = 5;
groupshared float g_predictorSad[kPredictorCount];
groupshared float2 g_predictorVector[kPredictorCount];

// What straying from the search centre costs, per texel, when that centre is
// the ZERO vector.
//
// Far above the ordinary kNeighbourhoodBias of 0.05, and it has to be. Zero is
// not a guess to be refined away - it is the claim that this block did not move
// with the scene. On a static HUD it matches almost exactly, but a 49-candidate
// search will still find some neighbour a hair better on noise, sub-pixel-refine
// towards it, and drag the ammo counter off its pixel. Measured exactly that
// way: the weapon came back intact and the HUD, which had been pixel-perfect,
// came out doubled.
//
// At 0.4 a neighbour must be better by a visible margin to displace a
// near-perfect zero, while a viewmodel whose true motion is a few pixels away
// still reaches it.
static const float kZeroCentreBias = 0.4;

// How much better than the coarse seed the ZERO vector must be, at the point
// stage, before it is allowed to become the search centre.
//
// Not a tie-break - a handicap, and it corrects an error in judging the three
// seed candidates "on equal terms". A single SAD value is a noisy estimator,
// and the two candidates are not the same kind of thing: the coarse seed's
// worth is that a search AROUND it will find the truth, while zero has only
// its one point. For a block moving 200 px the coarse seed is merely
// approximately right, so its point SAD is mediocre - and on low-contrast
// distant texture zero can beat it by accident. Then the centre snaps to zero,
// kZeroCentreBias pins it there, and the far scenery freezes and tears.
//
// Measured exactly that way: judging the three at par rescued the viewmodel
// and moved the damage into the background - the tower doubled, the rock faces
// mosaicked, distant geometry smeared.
//
// 1.3 costs the viewmodel nothing. There, world-displaced content does not
// resemble the weapon at all, so zero wins by a wide margin rather than a
// narrow one.
static const float kZeroCentreMargin = 1.3;

// The point SAD of the coarse seed, and the centre the search will actually
// use.
groupshared float g_coarseSeedSad;
groupshared int2 g_searchCentre;
groupshared int g_centreIsZero;

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
// What it cost: motion resolved to two-pixel precision instead of one - said to
// sit below the noise floor of the estimate, "for deciding where a SIXTEEN-pixel
// block went".
//
// That justification is stale twice over. The blocks are 8 px now, not 16. And
// two-pixel precision does not sit below the noise floor; it IS the artefact.
// Measured on 2026-09-14: the double images appear where the search saturates
// 0% of blocks, the match error is low and the four neighbouring vectors agree
// - vectors that are slightly wrong, not wrong. At 200 px of displacement a 1%
// error is 2 px, and averaging two copies of an edge 2 px apart is the
// definition of a double edge. Half resolution puts a floor under that error
// that no amount of searching can get below, because the sub-pixel parabola is
// fitted on the halved SAD surface and then multiplied by two.
//
// TRIED AND REVERTED on 2026-09-14. The search ran at full resolution for one
// build and the output collapsed: 40-135 FPS against a steady 144, with jitter
// of 164 ms and 113 ms in single seconds. Reported simply as "es ruckelt".
//
// The risk this change named beforehand did NOT happen - search saturation
// stayed at 0%, so reach was never the problem and the fine stage still found
// what it was looking for, at 0.47-1.81 ms. The cost landed somewhere else
// entirely: the same 16 samples per block now span four times the memory, so
// the search window no longer fits the cache the way it did, and the whole
// pipeline stalled around it.
//
// The precision theory is therefore UNTESTED, not disproved - we could not
// afford to test it this way. A cheaper route to the same end would be to keep
// the mip-1 search and add a +-1 pixel refinement at full resolution for the
// winning candidate only: 9 candidates instead of 49, on a window that is
// already in cache because the block was just read.
//
// Left at half resolution. The sample count is unchanged -
// kSampleStrideTexels follows kMipScale, so a block is still 16 samples - and
// so is the candidate count. What changes is reach: +-3 texels was +-6 full
// pixels and is now +-3. That is the real risk of this change, and it is
// directly measurable: "Search-saturated blocks" counts blocks pinned at the
// edge of the window. It read 0% at 308 px peak motion with the old reach. If
// it climbs now, the fine stage is no longer able to correct the coarse one and
// this has to be paid for with a wider radius or a third stage.
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

    // A plain difference of colours, which assumes the same object keeps the
    // same brightness between frames. Games break that: a G-force blackout
    // darkens everything, a cloud shadow passes, a flash lights the scene -
    // reported directly, the whole motion field lighting up when the screen
    // dims under G-load, because every block looks changed.
    //
    // Subtracting each block.s mean first would leave only the pattern and fix
    // that. Implemented by storing sixteen samples per thread, it cost the
    // fine stage 17-30 ms against 1.2 - 49 threads per group each holding
    // sixteen float3s blows the register budget and the GPU spills to memory.
    //
    // The idea is right and the implementation has to avoid keeping samples
    // around: either two fetch passes with no storage, or a gradient-based
    // measure, which cancels a brightness offset without needing the mean at
    // all.
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

    // DID THIS BLOCK CHANGE AT ALL? If not, skip the search entirely.
    //
    // The search costs 49 block comparisons per block and runs over every
    // block of the screen, every frame, whether or not anything there moved.
    // Measured live: 0.19% of blocks moving on a quiet screen and 55-80% in a
    // firefight - so most of the time most of this work is answering a
    // question whose answer is zero.
    //
    // A sky, a wall, the HUD, the letterbox bars of a cinematic: all of them
    // are searched at full price today.
    //
    // The test itself already existed - it is the same full-resolution
    // comparison the search uses at the end to decide "this did not move
    // after all". It was simply asked AFTER paying for the search rather than
    // before. One thread computes it, the group waits once, and a block whose
    // content is unchanged writes a zero vector and returns.
    //
    // The barrier costs the whole group a synchronisation it did not have
    // before, which is why this is worth it only because the saving is the
    // entire search rather than part of it.
    if (groupIndex == 0)
        g_zeroMotionSadFull = BlockSADFullRes(int2(groupId.xy) * kBlockSize);
    GroupMemoryBarrierWithGroupSync();

    if (g_zeroMotionSadFull < kStaticBlockSad)
    {
        if (groupIndex == 0)
        {
            // .w counts HOW LONG this block has been standing still.
            //
            // A HUD element stands still always; a tree never does. One frame
            // of stillness means nothing - a bird can pause, a wall can match
            // itself by accident - but sixty frames of it is a screen-space
            // overlay. That distinction is what lets the interpolator allow
            // "did not move" where it is true and refuse it where it is a
            // coincidence, which is what tears landscape apart along the edge
            // of a crosshair.
            //
            // Capped so it can fall back quickly: a menu that closes must stop
            // being treated as static within a few frames, not after a second.
            // The counter needs a far stricter test than the search skip above.
            //
            // kStaticBlockSad exists to save work: "close enough that searching
            // would find nothing". An OVERLAY is a different claim - a HUD is
            // pixel-identical from frame to frame, while an aircraft filling
            // the screen barely moves relative to the display and still
            // changes constantly through lighting, vibration and fine texture.
            // At the loose threshold the aircraft qualified as static after
            // eight frames and was handed the HUD treatment, so its edges tore
            // against the landscape exactly as the crosshair.s had.
            //
            // A quarter of the threshold is the difference between "would not
            // repay a search" and "did not change at all".
            const float wasStatic = PreviousMotionField.Load(int3(groupId.xy, 0)).w;
            const bool identicalToLastFrame = g_zeroMotionSadFull < kStaticBlockSad * 0.25;
            MotionVectors[groupId.xy] = float4(0.0, 0.0,
                max(g_zeroMotionSadFull, 0.0) / (16.0 * 3.0),
                identicalToLastFrame ? min(wasStatic + 1.0, 30.0) : 0.0);
        }
        return;
    }

    // Seed from the coarse stage: which coarse block this fine block sits in.
    // The coarse vector is in full-resolution pixels, so it is halved to land
    // in this stage's coordinates.
    int2 coarseIndex = clamp(int2(groupId.xy) / kCoarseBlockRatio,
        int2(0, 0), int2(max(CoarseWidth, 1u), max(CoarseHeight, 1u)) - 1);
    int2 seed = int2(round(CoarseMotionVectors.Load(int3(coarseIndex, 0)).xy))
        * kCoarseToFineScale / kMipScale;
    int2 refinement = int2(groupThreadId.xy) - kSearchRadius;

    // ---- STEP 1: three seed candidates, each judged at ONE point ----------
    //
    // The fine stage refines +-3 texels around a centre. Everything therefore
    // depends on that centre being in the right basin, and until now it was
    // always the coarse seed, with zero and the temporal predictors allowed
    // only to overturn the FINISHED search afterwards.
    //
    // That comparison was unfair in a way that cost a day. The coarse window
    // gets to try 49 positions and keep its best; zero and the predictors were
    // each tried at exactly one. On an Apex viewmodel - a weapon rigidly
    // attached to the camera while the world sweeps past at 100-300 px - the
    // world-seeded window always finds SOME passable match on world content,
    // and zero, which is near the weapon's truth but not exactly on it because
    // the weapon also sways, loses to it. The weapon came out shredded while
    // the HUD stayed perfect: dumped, looked at, and unmistakable.
    //
    // So the three candidates are now compared like for like, at one point
    // each, BEFORE any search runs. The winner becomes the centre. One search,
    // not two - cheaper than what this replaces.
    if (groupIndex == 0)
    {
        g_coarseSeedSad = BlockSAD(blockOrigin, seed);
    }

    // ZERO MOTION: the candidate that matters for anything pinned to the
    // camera or to the screen - a viewmodel, a crosshair, an ammo counter.
    // The coarse stages work on 64 and 128 px blocks that mix such an object
    // with the world behind it, and the neighbourhood bias pulls them to the
    // world, so the seed they hand down can be hundreds of pixels wrong.
    if (groupIndex == 1)
    {
        g_zeroMotionSad = BlockSAD(blockOrigin, int2(0, 0));
    }

    // TEMPORAL PREDICTORS: this block's own vector from the previous frame and
    // its four neighbours'. Continuous motion is predicted by its own past far
    // better than by a coarse pyramid, and once a viewmodel has been found
    // correctly one frame, these carry it forward for nothing.
    if (kUsePredictors && groupIndex >= 8 && groupIndex < 8 + kPredictorCount)
    {
        const int slot = groupIndex - 8;
        const int2 offsets[kPredictorCount] = {
            int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
        };
        const int2 blockGrid = int2(((int)FrameWidth + kBlockSize - 1) / kBlockSize,
                                   ((int)FrameHeight + kBlockSize - 1) / kBlockSize);
        const int2 neighbourBlock = clamp(int2(groupId.xy) + offsets[slot],
            int2(0, 0), max(blockGrid - 1, int2(0, 0)));

        const float2 predictedPixels = PreviousMotionField.Load(int3(neighbourBlock, 0)).xy;
        const int2 predicted = int2(round(predictedPixels / kMipScale));

        g_predictorVector[slot] = float2(predicted);
        g_predictorSad[slot] = BlockSAD(blockOrigin, predicted);
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- STEP 2: the winner becomes the centre of the search --------------
    //
    // No bias anywhere in this comparison. The bias exists to settle ties in
    // favour of coherence during refinement, and here the whole question is
    // whether the coherent answer is the right one at all.
    if (groupIndex == 0)
    {
        float centreSad = g_coarseSeedSad;
        int2 centre = seed;
        int which = 0; // 0 = coarse seed, 1 = zero, 2 = temporal predictor

        // Zero has to be clearly better, not merely better. See
        // kZeroCentreMargin - this is the difference between rescuing the
        // viewmodel and freezing the background.
        if (g_zeroMotionSad * kZeroCentreMargin < centreSad)
        {
            centreSad = g_zeroMotionSad;
            centre = int2(0, 0);
            which = 1;
        }

        for (int k = 0; kUsePredictors && k < kPredictorCount; ++k)
        {
            if (g_predictorSad[k] < centreSad)
            {
                centreSad = g_predictorSad[k];
                centre = int2(g_predictorVector[k]);
                which = 2;
            }
        }

        g_searchCentre = centre;
        g_centreIsZero = (which == 1) ? 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- STEP 3: one search, around the winner ---------------------------
    //
    // A candidate that agrees with the neighbourhood wins ties: the bias costs
    // a little per texel of distance from the centre, so where several
    // positions match equally well - flat ground, foliage, sky - the one
    // nearest the prediction is taken instead of whichever noise favoured.
    // Ground texture at speed offers dozens of near-identical candidates, and
    // a different winner each frame is what reads as a double image.
    //
    // A zero centre is held much harder. See kZeroCentreBias.
    const float seedBias = (g_centreIsZero != 0) ? kZeroCentreBias : kNeighbourhoodBias;
    g_sad[groupIndex] = BlockSAD(blockOrigin, g_searchCentre + refinement)
        + seedBias * length(float2(refinement));
    GroupMemoryBarrierWithGroupSync();

    // ---- STEP 4: reduce, refine to sub-pixel, write ----------------------
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

        const int2 bestRefinement =
            int2(bestIndex % kSearchWindow, bestIndex / kSearchWindow) - kSearchRadius;
        int2 bestOffset = g_searchCentre + bestRefinement;
        // The bias is taken back out: .z has to be a measurement of how well
        // the block actually matched, not of how far its winner sat from the
        // centre. The smoothing pass and the interpolation shader both read it
        // as real match quality.
        float bestMatchSad = bestSad - seedBias * length(float2(bestRefinement));

        // Standing still wins outright when it is CLEARLY better, so ordinary
        // noise in a moving scene cannot make blocks stick. Sub-pixel
        // refinement must not run afterwards: a block judged still has no error
        // surface around its winner to interpolate.
        bool snappedToZero = false;
        if (g_zeroMotionSad * kZeroMotionMargin < bestMatchSad)
        {
            bestOffset = int2(0, 0);
            bestMatchSad = g_zeroMotionSad;
            snappedToZero = true;
        }

        // Full resolution has the last word on standing still. A block this
        // close to identical where it already is did not move, whatever the
        // half-resolution search made of it.
        if (g_zeroMotionSadFull < kStaticBlockSad)
        {
            bestOffset = int2(0, 0);
            bestMatchSad = min(bestMatchSad, g_zeroMotionSadFull);
            snappedToZero = true;
        }

        const float kSamplesPerCandidate = 16.0 * 3.0; // 4x4 samples, 3 channels

        // Sub-pixel by a parabola through the SAD minimum, per axis.
        float2 subTexel = float2(0.0, 0.0);
        if (!snappedToZero)
        {
            const int bx = bestIndex % kSearchWindow;
            const int by = bestIndex / kSearchWindow;
            const float centreSad = g_sad[bestIndex];

            if (bx > 0 && bx < kSearchWindow - 1)
            {
                const float left  = g_sad[by * kSearchWindow + bx - 1];
                const float right = g_sad[by * kSearchWindow + bx + 1];
                const float curvature = left - 2.0 * centreSad + right;
                if (curvature > 1e-7)
                    subTexel.x = clamp(0.5 * (left - right) / curvature, -0.5, 0.5);
            }
            if (by > 0 && by < kSearchWindow - 1)
            {
                const float up   = g_sad[(by - 1) * kSearchWindow + bx];
                const float down = g_sad[(by + 1) * kSearchWindow + bx];
                const float curvature = up - 2.0 * centreSad + down;
                if (curvature > 1e-7)
                    subTexel.y = clamp(0.5 * (up - down) / curvature, -0.5, 0.5);
            }
        }

        float2 finalMotion = (float2(bestOffset) + subTexel) * kMipScale;
        if (dot(finalMotion, finalMotion) < 0.75 * 0.75)
            finalMotion = float2(0.0, 0.0);

        // Moving: the stillness counter resets to zero.
        MotionVectors[groupId.xy] = float4(finalMotion,
            max(bestMatchSad, 0.0) / kSamplesPerCandidate, 0.0);
    }
}
