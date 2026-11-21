// COARSEST stage of the pyramid search: block matching on a SIXTEENTH-
// resolution mip of both source frames.
//
// Exists because the two-level pyramid still saturated: 10-14% of moving
// blocks sat on the edge of its 54 px reach during fast motion, with mean
// motion measured at 19.6 px. At one sixteenth resolution a single texel
// spans 16 full-resolution pixels, so the same radius of 12 reaches 192 px -
// and it costs almost nothing, because there are 256 times fewer blocks than
// at full resolution.
//
// It also makes the levels below CHEAPER: each now only has to refine a few
// pixels around the level above rather than search from zero.
//
// Why the downsampling is the essential part, not an optimisation: a first
// attempt widened the reach by sampling a 4-pixel candidate grid on the
// FULL-resolution frame, with no downsampling. It made the motion field
// dramatically worse - 16px blocks matched distant repeating detail (text,
// noise) better than their own true small motion, and the measured mean
// motion jumped from 6.2 to 37.5 px with a third of all blocks saturated.
// Averaging the fine detail away first is exactly what stops that: at
// quarter resolution a block covers four times as much of the picture and
// has no high-frequency detail left to false-match against.
//
// One texel here is 4 full-resolution pixels, so a search radius of 12
// reaches 48 pixels of real displacement - four times what the single-stage
// search could cover, for less work, because the expensive fine stage now
// only has to look a few pixels around this result.

Texture2D<float4> PrevFrame : register(t0);
Texture2D<float4> CurrFrame : register(t1);
RWTexture2D<float4> CoarsestMotionVectors : register(u0);

// Blocks and offsets below are in MIP-2 texels throughout.
static const int kMipLevel = 4;
static const int kMipScale = 16;        // 1 texel here = 16 full-res pixels
static const int kBlockSize = 16;            // = 64 full-resolution pixels
static const int kBlockSampleStride = 4;     // 4x4 = 16 samples per candidate
// Radius 15, not 12: the maximum measured motion pegged at exactly 239.7 px in
// every reporting line - a vector of (192, 144), which is this stage.s own edge
// at radius 12. Everything moving faster than that got a wrong vector, and the
// finer stages could not help, because they only refine a few pixels around
// wherever this stage pointed. That is what "parts of the picture shift" looks
// like from the inside.
//
// 15 is the ceiling here: one GPU thread per candidate, and 31 x 31 = 961 fits
// under D3D11.s limit of 1024 threads per group where 33 x 33 = 1089 does not.
// This stage runs on a sixteenth-resolution image, so its cost is the smallest
// of the three.
//
// RADIUS 30 (2026-11-21), each thread now testing a 2x2 cell of candidates.
// Measured from a replay capture in Destiny 2, 3x: a fast camera turn moved the
// picture 256-272 px per real frame, past this stage's 240 px plus the ~22 px
// the two finer stages add. Every generated frame in that stretch came apart
// into blocks, doubled name tags and torn weapon edges; at 168 px in the same
// capture the frames were clean. The thread limit stays at 31 x 31; four
// candidates per thread reach 480 px. The stage runs on 60 blocks at 1440p, so
// four times its cost is still noise.
static const int kSearchRadius = 30;         // = 480 full-resolution pixels
static const int kThreadsPerAxis = 31;       // 31 x 31 = 961 <= 1024
static const int kThreadCount = kThreadsPerAxis * kThreadsPerAxis;

cbuffer FrameDims : register(b0)
{
    uint FrameWidth;    // full resolution
    uint FrameHeight;
    uint CoarseWidth;
    uint CoarseHeight;
    uint CoarsestWidth;
    uint CoarsestHeight;
    uint _pad0;
    uint _pad1;
};

groupshared float g_score[kThreadCount];
groupshared int2  g_offset[kThreadCount];

// Samples whose partner lands outside the picture are LEFT OUT and the sum is
// scaled back to the full sample count, instead of being compared against a
// clamped edge texel. At radius 15 that clamp touched few candidates; at 30 a
// large share of every edge block's offsets reach past the border, and a
// smeared edge column can out-score the true match on a flat block.
float BlockSAD(int2 currBlockOrigin, int2 candidateOffset, int2 mipDims)
{
    float sad = 0.0;
    float used = 0.0;
    [unroll]
    for (int y = 0; y < kBlockSize; y += kBlockSampleStride)
    {
        [unroll]
        for (int x = 0; x < kBlockSize; x += kBlockSampleStride)
        {
            int2 currTexel = currBlockOrigin + int2(x, y);
            if (currTexel.x >= mipDims.x || currTexel.y >= mipDims.y)
                continue;

            int2 prevTexel = currTexel + candidateOffset;
            if (any(prevTexel < int2(0, 0)) || any(prevTexel >= mipDims))
                continue;

            float3 currColor = CurrFrame.Load(int3(currTexel, kMipLevel)).rgb;
            float3 prevColor = PrevFrame.Load(int3(prevTexel, kMipLevel)).rgb;

            sad += dot(abs(currColor - prevColor), float3(1.0, 1.0, 1.0));
            used += 1.0;
        }
    }
    const float total = (kBlockSize / kBlockSampleStride) * (kBlockSize / kBlockSampleStride);
    // Fewer than a quarter of the samples inside: not a comparison at all.
    if (used < total * 0.25) return 1e9;
    return sad * (total / used);
}

// A tie on a flat block (sky, a wall) goes to the SHORTER vector. Without it the
// first candidate in index order won, which is the far corner of the window -
// harmless at 240 px, a needless 480 px vector now. The weight is far below any
// real SAD difference.
float Score(float sad, int2 offset)
{
    return sad + 1e-5 * float(dot(offset, offset));
}

// One thread group per coarsest block; each thread tests a 2x2 cell of
// candidate offsets, so 31 x 31 threads cover -30..+31 (+31 is skipped).
[numthreads(kThreadsPerAxis, kThreadsPerAxis, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    int2 mipDims = int2(max(FrameWidth / kMipScale, 1u), max(FrameHeight / kMipScale, 1u));
    int2 blockOrigin = int2(groupId.xy) * kBlockSize;
    int2 cellOrigin = int2(groupThreadId.xy) * 2 - kSearchRadius;

    float bestScore = 1e30;
    int2 bestOffset = int2(0, 0);
    [unroll]
    for (int j = 0; j < 2; ++j)
    {
        [unroll]
        for (int i = 0; i < 2; ++i)
        {
            int2 offset = cellOrigin + int2(i, j);
            if (any(offset > int2(kSearchRadius, kSearchRadius)))
                continue;
            float s = Score(BlockSAD(blockOrigin, offset, mipDims), offset);
            if (s < bestScore) { bestScore = s; bestOffset = offset; }
        }
    }
    g_score[groupIndex] = bestScore;
    g_offset[groupIndex] = bestOffset;
    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0)
    {
        float best = g_score[0];
        int bestIndex = 0;
        for (int k = 1; k < kThreadCount; ++k)
        {
            if (g_score[k] < best)
            {
                best = g_score[k];
                bestIndex = k;
            }
        }
        // Stored in mip-4 texels; the coarse stage scales it up.
        CoarsestMotionVectors[groupId.xy] = float4(float2(g_offset[bestIndex]), 0.0, 0.0);
    }
}
