// FLICK DETECTION, from the coarsest motion field, on the GPU.
//
// Asked for 2026-11-21 after replay captures in Destiny 2 at 48 FPS, 3x:
// camera turns of up to ~270 px per real frame now interpolate cleanly (the
// coarsest search reaches 480 px), but flicks of 340-1100 px per real frame
// still turn every generated frame into blocks. Nothing in between two real
// frames that far apart can be reconstructed - half of what is visible was
// not on screen in the other frame - and a frame of misplaced blocks is far
// more visible than a short step. During a flick the generated frames show
// the nearest REAL frame instead. "Die Flicks stören mich, bau das ein."
//
// Judged on what the estimator itself managed, not on the picture: the share
// of coarsest blocks whose vector is faster than the search can follow
// reliably, or sits on the edge of its window (the true motion was further).
//
// Runs once per real frame pair as a single group, right after the forward
// coarsest pass, and writes one float4 that the interpolation shader reads.
// No CPU round trip, so the decision applies to the same pair it was made on.
//
// Output (FlickState[0]):
//   x  flick amount 0..1 - 0 interpolate as usual, 1 show the nearest real frame
//   y  share of blocks counted as flicking this pair
//   z  mean block speed in full-resolution pixels per real frame
//   w  share of blocks on the edge of the search window

Texture2D<float4> CoarsestMotionVectors : register(t0);   // mip-4 texels, .xy
RWStructuredBuffer<float4> FlickState : register(u0);

cbuffer FrameDims : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint CoarseWidth;
    uint CoarseHeight;
    uint CoarsestWidth;
    uint CoarsestHeight;
    uint _pad0;
    uint _pad1;
};

static const float kMipScale = 16.0;      // one coarsest texel = 16 px
static const float kEdgeTexels = 29.0;    // radius 30: |v| >= 29 is the edge

// The line between "fast turn" and "flick", in full-resolution pixels per real
// frame. Measured in the captures above: 264 px still interpolated cleanly,
// 336 px and above did not.
static const float kFlickSpeed = 300.0;

// Share of blocks that must be flicking before the fallback starts, and where
// it is complete. The weapon and the HUD hold still during any turn and cover
// a good part of the screen, so a real flick never reaches 100%; a single
// fast object crossing the view stays well under the lower bound.
static const float kShareStart = 0.25;
static const float kShareFull = 0.50;

// Coming back down is spread over about three pairs, so a flick that slows
// through the threshold does not alternate between the two looks from one
// pair to the next. Going up is immediate - a flick is short.
static const float kDecayPerPair = 0.34;

groupshared float g_fast[64];
groupshared float g_edge[64];
groupshared float g_speed[64];
groupshared float g_count[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_GroupThreadID)
{
    const uint total = CoarsestWidth * CoarsestHeight;
    float fast = 0.0, edge = 0.0, speed = 0.0, count = 0.0;
    for (uint i = tid.x; i < total; i += 64)
    {
        const int2 cell = int2(i % CoarsestWidth, i / CoarsestWidth);
        const float2 v = CoarsestMotionVectors.Load(int3(cell, 0)).xy;
        const float s = length(v) * kMipScale;
        const bool onEdge = max(abs(v.x), abs(v.y)) >= kEdgeTexels;
        fast += (s >= kFlickSpeed || onEdge) ? 1.0 : 0.0;
        edge += onEdge ? 1.0 : 0.0;
        speed += s;
        count += 1.0;
    }
    g_fast[tid.x] = fast;
    g_edge[tid.x] = edge;
    g_speed[tid.x] = speed;
    g_count[tid.x] = count;
    GroupMemoryBarrierWithGroupSync();

    if (tid.x == 0)
    {
        float f = 0.0, e = 0.0, s = 0.0, c = 0.0;
        for (uint k = 0; k < 64; ++k)
        {
            f += g_fast[k]; e += g_edge[k]; s += g_speed[k]; c += g_count[k];
        }
        c = max(c, 1.0);
        const float share = f / c;
        const float now = saturate((share - kShareStart) / (kShareFull - kShareStart));
        const float previous = FlickState[0].x;
        const float flick = max(now, previous - kDecayPerPair);
        FlickState[0] = float4(flick, share, s / c, e / c);
    }
}
