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
RWTexture2D<float4> SmoothedMotionVectors : register(u0);

cbuffer BlockGridDims : register(b0)
{
    uint BlockCountX;
    uint BlockCountY;
    uint HavePreviousField; // 0 on the first frame after a resolution change
    uint _pad1;
};

static const int kSpatialRadius = 2; // 5x5

// Weight of the new field. Deliberately high: the point is to damp flicker,
// not to average motion away. At 0.7 a wrong vector decays to ~3% influence
// after three real frames, while a genuine change in motion is 70% applied
// immediately.
static const float kNewFieldWeight = 0.7;

// How sharply a poor match reduces a block`s influence. At 30, a block whose
// best candidate differs by 0.3 per channel - no real match at all - carries
// about a tenth of the weight of a clean one.
static const float kMatchErrorSensitivity = 30.0;

// How sharply a changed motion cancels the carry-over from the previous field.
// At 0.5 a block whose vector moved by 2 px still keeps most of the damping,
// while one that jumped by 20 px - an object arriving or leaving - keeps
// almost none of it.
static const float kMotionChangeSensitivity = 0.5;

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

    float2 sum = float2(0, 0);
    float weightSum = 0.0;

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
            // (error ~0.3) gives ~0.1, so it still contributes, but its
            // neighbours decide.
            float weight = 1.0 / (1.0 + kMatchErrorSensitivity * neighbour.z);

            // A neighbour moving somewhere else entirely barely counts.
            //
            // Averaging across a motion boundary is what drags a moving object.s
            // vector into the still background beside it, and at 8 px blocks
            // this window reaches 40 px in every direction. Reported from a live
            // game as a trail behind moving bots: the background they crossed
            // inherited their motion and was pulled along with them.
            //
            // Averaging still smooths noise inside an object, where neighbours
            // agree, and stops at the edge, where they do not - which is where
            // the smoothing was doing harm rather than good.
            const float2 delta = neighbour.xy - centre.xy;
            const float distance = sqrt(dot(delta, delta));
            weight *= 1.0 / (1.0 + kMotionDifferenceSensitivity * distance);

            sum += neighbour.xy * weight;
            weightSum += weight;
        }
    }

    float2 spatial = weightSum > 0.0 ? sum / weightSum : float2(0, 0);

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
    SmoothedMotionVectors[id.xy] = float4(spatial, matchError, 0.0);
}
