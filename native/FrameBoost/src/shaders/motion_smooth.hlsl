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

Texture2D<float2> RawMotionVectors : register(t0);
Texture2D<float2> PreviousMotionVectors : register(t1);
RWTexture2D<float2> SmoothedMotionVectors : register(u0);

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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= BlockCountX || id.y >= BlockCountY)
        return;

    float2 sum = float2(0, 0);
    int count = 0;

    [unroll]
    for (int dy = -kSpatialRadius; dy <= kSpatialRadius; ++dy)
    {
        [unroll]
        for (int dx = -kSpatialRadius; dx <= kSpatialRadius; ++dx)
        {
            int2 p = int2(id.xy) + int2(dx, dy);
            if (p.x < 0 || p.y < 0 || p.x >= (int)BlockCountX || p.y >= (int)BlockCountY)
                continue;

            sum += RawMotionVectors.Load(int3(p, 0));
            count += 1;
        }
    }

    float2 spatial = sum / max(count, 1);

    if (HavePreviousField != 0)
    {
        float2 previous = PreviousMotionVectors.Load(int3(id.xy, 0));
        spatial = lerp(previous, spatial, kNewFieldWeight);
    }

    SmoothedMotionVectors[id.xy] = spatial;
}
