// Smooths the raw per-block motion field, spatially AND over time.
//
// Real block-matching on detailed scenes produces individual outlier vectors
// (a block "locks on" to the wrong offset), while real motion is spatially
// coherent - neighbouring blocks usually move together. Averaging genuinely
// reduces that noise; it is standard in motion-compensated video coding, not
// a trick to hide bad data.
//
// Two things were added after a live report of the image "wiggling" in a
// game - warping slightly and differently from frame to frame rather than
// moving cleanly:
//
//  1. A 5x5 spatial kernel instead of 3x3. At 16px blocks a 3x3 window
//     covers only 48px, which is smaller than the motion being measured
//     (mean ~16px, up to 207px), so neighbouring blocks still disagreed
//     enough to warp visibly at their shared edges.
//
//  2. Temporal blending with the previous field. Nothing smoothed the field
//     across time at all, so every new real frame brought a slightly
//     different set of vectors - and with roughly three generated frames per
//     real frame, each inconsistency was shown three times in a row. Camera
//     motion is strongly correlated frame to frame, so carrying a fraction
//     of the previous field forward removes that flicker at the cost of a
//     little responsiveness when motion changes direction abruptly.

Texture2D<float4> RawMotionVectors : register(t0);
Texture2D<float4> PreviousMotionVectors : register(t1);
Texture2D<float4> BackwardMotionVectors : register(t2);
RWTexture2D<float4> SmoothedMotionVectors : register(u0);

cbuffer BlockGridDims : register(b0)
{
    uint BlockCountX;
    uint BlockCountY;
    uint HavePreviousField; // 0 on the first frame after a resolution change
    uint BlockSizePixels;
    uint HaveBackwardField; // 0 when the backward pass did not run
    uint CoarseBlockRatio; // fine blocks per coarse block, each axis
    uint CoarseCountX;
    uint CoarseCountY;
};

// How far the round trip may miss before the block counts as content that was
// not visible in the previous frame.
//
// Relative to the motion, because a 6 px discrepancy is nothing at 60 px of
// travel and everything at 3 px, with a floor so still areas are not judged
// against nearly zero - and a CEILING, which the first version lacked.
//
// Without the ceiling the allowance grew without limit: at 80 px of motion it
// permitted a 31 px miss, and a real disocclusion typically misses by around
// 20, so it passed as consistent. That showed up precisely as the test
// working at a distance and failing as the player walked closer - the same
// bot at the same speed covers far more pixels up close. Reported exactly
// that way: dark when far away, "komme ich naeher wird es genauso".
//
// 12 px is wider than the noise between two honest fields (a few pixels) and
// narrower than the disagreement at an uncovered edge, which is the size of
// the object.s own displacement.
static const float kRoundTripTolerance = 0.35;
static const float kRoundTripFloorPx = 3.0;
static const float kRoundTripCeilingPx = 12.0;

// The forward-backward consistency test, in the direction that actually means
// something.
//
// Follow this block.s vector back to where it claims to have come from, and
// ask the BACKWARD field what the content at that spot says it did. If both
// describe the same movement, the round trip returns here and the two cancel:
// F(x) + B(x + F(x)) is about zero. Where an object has uncovered ground, it
// does not - this block points at the object, and the object points somewhere
// else entirely, because the object moved on while the ground stayed put.
//
// An earlier version of this test compared F(x) with the forward field at
// x + F(x) and expected them to AGREE. That is a smoothness test, not a
// consistency test: it fires on every moving pixel, which is exactly what was
// observed when the result was painted on screen, and it led to the wrong
// conclusion that the whole idea was useless. The idea was right; it needed
// the second field, which did not exist yet.
bool IsDisoccluded(int2 blockPos, float2 motion)
{
    // Without the backward pass there is nothing to compare against, and the
    // stale contents of that texture would mark almost everything.
    if (HaveBackwardField == 0) return false;

    const float2 sourcePixel = float2(blockPos) * BlockSizePixels + motion;
    const int2 sourceBlock = int2(round(sourcePixel / BlockSizePixels));
    if (sourceBlock.x < 0 || sourceBlock.y < 0 ||
        sourceBlock.x >= (int)BlockCountX || sourceBlock.y >= (int)BlockCountY)
        return true; // came from outside the picture: not visible before

    // The backward field is COARSE: one entry per 32-pixel block, and its
    // vectors are in mip-2 texels rather than pixels. Both conversions happen
    // here rather than in the estimator, so the forward path stays untouched.
    const int2 coarseBlock = clamp(sourceBlock / (int)max(CoarseBlockRatio, 1u),
                                   int2(0, 0),
                                   int2(max(CoarseCountX, 1u), max(CoarseCountY, 1u)) - 1);
    const float2 backward = BackwardMotionVectors.Load(int3(coarseBlock, 0)).xy * 4.0;
    const float2 roundTrip = motion + backward;
    const float miss = length(roundTrip);
    const float allowed = min(kRoundTripFloorPx + kRoundTripTolerance * length(motion),
                              kRoundTripCeilingPx);
    return miss > allowed;
}

// 3x3, cut from 5x5 after measuring what this pass costs.
//
// The geometric median compares every candidate in the window with every
// other, so the cost is the SQUARE of the window: 625 comparisons at 5x5, 81
// at 3x3. Timestamps between the pyramid stages showed this pass taking
// 1.2-1.7 ms - as much as the entire motion search beside it, which is 2.1 ms
// at 90% of blocks moving. It had never been measured on its own.
//
// What 5x5 was for is in the comment above: at 16 px blocks a 3x3 window
// covered only 48 px, less than the motion being measured. Blocks are 8 px
// now, and the median does not average across a boundary the way the mean it
// replaced did, so the reason for the wider window is largely spent.
//
// The median itself stays. Its point is that it cannot invent a vector no
// block reported, and that holds at any window size.
static const int kSpatialRadius = 1; // 3x3

// Weight of the new field. Deliberately high: the point is to damp flicker,
// not to average motion away. At 0.7 a wrong vector decays to ~3% influence
// after three real frames, while a genuine change in motion is 70% applied
// immediately.
// Back to 0.7 after testing 1.0 live. Switching the carry-over off shortened
// the trail only slightly and cost a lot of smoothness - reported as feeling
// like 20 fps - so the temporal term is not what makes the trail, and it is
// earning its keep. The trail is spatial: see kMotionDifferenceSensitivity.
//
// At 0.7 a block keeps 30% of last frame.s vector, 9% the frame after. Where
// a moving object has just passed, the background it uncovered goes on
// carrying a fading remnant of that object.s motion - which is a trail, in the
// time domain. That is the one place nothing else in the pipeline reaches:
// finer blocks, edge-aware smoothing, a stricter blend threshold and a
// confidence fallback to the real frame all left it untouched, and falling
// back to the real frame not helping proves the trail is made of CONFIDENT
// WRONG vectors rather than of blocks that failed to match.
//
// If the trail goes and flicker returns, the mechanism is confirmed and the
// fix is to re-add this as a search CANDIDATE (3DRS-style: the previous
// vector competes on match error and has to win) rather than as a blend on
// the result, which cannot be outvoted by anything.
static const float kNewFieldWeight = 0.7;

// How sharply a poor match reduces a block`s influence. At 30, a block whose
// best candidate differs by 0.3 per channel - no real match at all - carries
// about a tenth of the weight of a clean one.
static const float kMatchErrorSensitivity = 30.0;

// How sharply a changed motion cancels the carry-over from the previous field.
//
// 0.1, not the 0.5 this started at - the first value did not do what its own
// comment claimed. At 0.5 a change of 2 px already halves the damping, so the
// temporal smoothing was effectively switched off across the whole picture
// rather than at object edges, and the flicker it exists to prevent came
// straight back: reported within a minute as the image flickering.
//
// At 0.1 the numbers match the intent: 2 px keeps 83% of the damping, 20 px
// keeps a third, 40 px - an object arriving or leaving - keeps a fifth.
static const float kMotionChangeSensitivity = 0.1;

// How sharply a neighbour.s weight falls off when it is moving somewhere else.
// At 0.25 a neighbour differing by 4 px still carries half the weight - noise
// inside one object is smoothed as before - while one differing by 40 px, which
// is a genuine motion boundary, carries a tenth.
static const float kMotionDifferenceSensitivity = 0.25;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= BlockCountX || id.y >= BlockCountY)
        return;

    // Weighted by how well each neighbour actually matched, not averaged
    // blindly.
    //
    // Measured during a fast turn in a game: 14.9% of blocks find no real
    // match at all, because the content they cover was simply not in the
    // previous frame - it was revealed BY the turn. Their vectors are the
    // least bad of a set of equally wrong candidates, and letting them
    // contribute equally spreads that error into their neighbours.
    //
    // It matters most for those blocks themselves. A block with no match of
    // its own now takes its motion almost entirely from the neighbourhood,
    // which during a camera turn is very nearly the right answer: newly
    // revealed background moves with the camera like everything else. The
    // alternative - the interpolator falling back to a real frame for those
    // pixels - leaves 15% of the image standing still while the rest moves,
    // and that patchwork is what the eye reads as judder.
    // This block's own vector, which decides which neighbours are talking
    // about the same motion and which belong to something else entirely.
    const float4 centre = RawMotionVectors.Load(int3(id.xy, 0));

    // A weighted vector MEDIAN, not a weighted average.
    //
    // The average was the trail. Averaging "the object moves 30 px" with "the
    // background is still" gives 15 px - a vector no block reported and no
    // content actually has. Every block in the 5x5 window straddling an object
    // boundary got one of those, and at 8 px blocks this window reaches 40 px
    // in every direction, so a 40 px apron of still background around a moving
    // object was warped as if it were moving. That is a trail, and it is the
    // one mechanism consistent with everything measured: it is made of
    // CONFIDENT vectors (so falling back to the real frame on low confidence
    // never touched it), it is spatial (so switching off the temporal
    // carry-over barely shortened it), and it sits beside the object rather
    // than only behind it (reported on the weapon and the surroundings too).
    //
    // A median cannot invent a value: it picks one of the vectors that was
    // actually reported. Inside an object, where neighbours agree, it still
    // removes outliers exactly as the average did. At a boundary it lands on
    // whichever side is in the majority, instead of halfway between two
    // things that are both real.
    //
    // Implemented as the weighted geometric median over the window: the
    // candidate whose total weighted distance to all the others is smallest.
    // Pure ALU on values already loaded, no extra texture reads.
    const int kWindow = (2 * kSpatialRadius + 1) * (2 * kSpatialRadius + 1);
    float2 candidate[kWindow];
    float candidateWeight[kWindow];
    int candidateCount = 0;

    [unroll]
    for (int dy = -kSpatialRadius; dy <= kSpatialRadius; ++dy)
    {
        [unroll]
        for (int dx = -kSpatialRadius; dx <= kSpatialRadius; ++dx)
        {
            int2 p = int2(id.xy) + int2(dx, dy);
            if (p.x < 0 || p.y < 0 || p.x >= (int)BlockCountX || p.y >= (int)BlockCountY)
                continue;

            float4 neighbour = RawMotionVectors.Load(int3(p, 0));
            // Match error 0 gives weight 1; a thoroughly unmatched block
            // (error ~0.3) gives ~0.1, so it still has a say, but its
            // neighbours decide.
            float weight = 1.0 / (1.0 + kMatchErrorSensitivity * neighbour.z);

            // A neighbour moving somewhere else entirely barely counts. This
            // still matters with a median: it decides which side of a boundary
            // holds the majority, and a badly matched block should not be the
            // one casting that vote.
            const float2 delta = neighbour.xy - centre.xy;
            const float distance = sqrt(dot(delta, delta));
            weight *= 1.0 / (1.0 + kMotionDifferenceSensitivity * distance);

            // A disoccluded neighbour does not get a vote. Its vector belongs
            // to the object that uncovered it, not to the ground it covers,
            // and letting it vote is what let whole uncovered regions agree
            // on the wrong answer and out-vote the background around them.
            if (IsDisoccluded(p, neighbour.xy))
                continue;

            candidate[candidateCount] = neighbour.xy;
            candidateWeight[candidateCount] = weight;
            ++candidateCount;
        }
    }

    // Nothing but disoccluded neighbours: keep this block.s own vector rather
    // than inventing one. Rare, and the confidence below marks it anyway.
    float2 spatial = centre.xy;
    float bestCost = 1e30;
    for (int i = 0; i < candidateCount; ++i)
    {
        float cost = 0.0;
        for (int j = 0; j < candidateCount; ++j)
        {
            const float2 d = candidate[i] - candidate[j];
            cost += candidateWeight[j] * sqrt(dot(d, d));
        }
        // A candidate that matched badly is a worse representative of the
        // neighbourhood even when it sits centrally, so its own weight
        // discounts its cost as well.
        cost /= max(candidateWeight[i], 1e-4);
        if (cost < bestCost)
        {
            bestCost = cost;
            spatial = candidate[i];
        }
    }

    if (HavePreviousField != 0)
    {
        float2 previous = PreviousMotionVectors.Load(int3(id.xy, 0)).xy;

        // Carrying the previous field over is what leaves a trail.
        //
        // At 0.7 a block keeps 30% of the vector it had last frame, and 9% the
        // frame after that. Where a moving object has just passed, the
        // background it uncovered goes on carrying a fading remnant of that
        // object.s motion for several frames - which is exactly a trail behind
        // it, and exactly why finer blocks, edge-aware smoothing and a
        // stricter blend threshold all left it untouched: none of them are in
        // the time domain.
        //
        // So the carry-over applies only where the two fields agree about the
        // motion. Where this frame says something clearly different from the
        // last one, the new answer is taken whole: damping flicker is worth a
        // lot inside a steadily moving area and nothing at all where the
        // motion has genuinely changed.
        const float2 change = spatial - previous;
        const float changeDistance = sqrt(dot(change, change));
        const float carryOver = (1.0 - kNewFieldWeight)
            / (1.0 + kMotionChangeSensitivity * changeDistance);
        spatial = lerp(spatial, previous, carryOver);
    }

    // .z (the match error) is carried through unsmoothed: it describes this
    // block`s own match quality, and averaging it with its neighbours` would
    // blur exactly the localisation the metric exists to provide.
    float matchError = RawMotionVectors.Load(int3(id.xy, 0)).z;

    // A disoccluded block is NOT marked as a bad match, although it was for one
    // build and that looked reasonable.
    //
    // Raising its error drove the interpolator.s confidence to zero, which
    // makes it fall back to the unwarped real frame - and that freezes those
    // pixels for one generated frame. Painting the fallback magenta showed
    // where it landed: a strip directly behind the moving bot. But in the
    // generated frame the bot is supposed to be HALFWAY along, so its trailing
    // edge still sits in that strip. Freezing it replaced the bot.s tail with
    // ground it has not uncovered yet, so the bot was cut off at the back and
    // drawn twice - a trail again, in the same place, produced by the repair
    // rather than by the estimator.
    //
    // Nothing more is needed here: the median above has already given these
    // blocks the motion of their non-disoccluded neighbours, which is the
    // background.s own motion, and that is what uncovered ground actually
    // does. It is still ground; it just was not visible before.

    // The stillness counter is carried through untouched - it describes this
    // block.s own history, and averaging it with its neighbours. would smear
    // exactly the boundary between a static overlay and the moving world that
    // it exists to mark.
    const float staticFrames = RawMotionVectors.Load(int3(id.xy, 0)).w;
    SmoothedMotionVectors[id.xy] = float4(spatial, matchError, staticFrames);
}
