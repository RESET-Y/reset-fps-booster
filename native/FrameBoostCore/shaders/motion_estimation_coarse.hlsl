// Coarse stage of the pyramid search: block matching on a QUARTER-resolution
// mip of both source frames.
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
RWTexture2D<float4> CoarseMotionVectors : register(u0);

// Blocks and offsets below are in MIP-2 texels throughout.
static const int kMipLevel = 2;
static const int kMipScale = 4;              // 1 texel here = 4 full-res pixels
// 8 texels at mip 2 = 32 full-resolution pixels, halved from 64.
//
// At 64 px a moving object smaller than that sits entirely inside one coarse
// block together with the background around it, and the block reports whatever
// covers more of it - the background. Every fine block on the object then
// starts from "the background moves like this" and may refine it by +-6 px,
// so an object moving differently from the scene behind it is unrepresentable.
// Measured during live movement: the motion field reported 40 to 112 px, and
// an enemy crossing that scene was reported as duplicating itself a few pixels
// away - a generated frame that leaves the object almost where the real frame
// has it, which is what an unreachable vector looks like.
//
// At 32 px an object of that size dominates its own block instead of being
// outvoted by its surroundings. Four times as many coarse blocks, but this
// level runs at quarter resolution and is a small part of the total.
static const int kBlockSize = 8;             // = 32 full-resolution pixels
static const int kBlockSampleStride = 4;     // 4x4 = 16 samples per candidate
// Radius reduced from 12 now that a coarsest level (mip 4) runs first: this
// stage only refines around that result instead of searching from zero, so it
// needs to cover the coarsest level's step size (16 full-resolution pixels =
// 4 texels here) plus a margin. Six texels = 24 full-resolution pixels is
// ample, and costs 169 candidates per block instead of 625.
// 4 texels = 16 full-resolution pixels, cut from 6 (24 px).
//
// This stage does not have to FIND motion, only bridge the step of the level
// above it, and that step is 16 full-resolution pixels - which radius 4 covers
// exactly. Radius 6 bought margin that halving the block size made
// unaffordable: four times as many blocks at 169 candidates each took motion
// estimation from 3.5 ms to 9 ms, past the 6.9 ms a generated frame has. At
// radius 4 it is 81 candidates.
static const int kSearchRadius = 4;          // = 16 full-resolution pixels
static const int kSearchWindow = kSearchRadius * 2 + 1; // 9
static const int kCandidateCount = kSearchWindow * kSearchWindow; // 169

// One coarsest block covers 4x4 of this level's blocks, and its vectors are
// stored in mip-4 texels - four of this level's texels each.
// 256 px coarsest block / 32 px coarse block. Was 4 when the coarse block
// was 64 px wide; halving that block doubled how many of them one coarsest
// block spans.
static const int kCoarsestBlockRatio = 8;
static const int kCoarsestToCoarseScale = 4;

Texture2D<float4> CoarsestMotionVectors : register(t2);

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

groupshared float g_sad[kCandidateCount];

float BlockSAD(int2 currBlockOrigin, int2 candidateOffset, int2 mipDims)
{
    float sad = 0.0;
    [unroll]
    for (int y = 0; y < kBlockSize; y += kBlockSampleStride)
    {
        [unroll]
        for (int x = 0; x < kBlockSize; x += kBlockSampleStride)
        {
            int2 currTexel = currBlockOrigin + int2(x, y);
            if (currTexel.x >= mipDims.x || currTexel.y >= mipDims.y)
                continue;

            int2 prevTexel = clamp(currTexel + candidateOffset, int2(0, 0), mipDims - 1);

            float3 currColor = CurrFrame.Load(int3(currTexel, kMipLevel)).rgb;
            float3 prevColor = PrevFrame.Load(int3(prevTexel, kMipLevel)).rgb;

            sad += dot(abs(currColor - prevColor), float3(1.0, 1.0, 1.0));
        }
    }
    return sad;
}

// One thread group per coarse block, one thread per candidate offset.
[numthreads(kSearchWindow, kSearchWindow, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    int2 mipDims = int2(max(FrameWidth / kMipScale, 1u), max(FrameHeight / kMipScale, 1u));
    int2 blockOrigin = int2(groupId.xy) * kBlockSize;

    // Seed from the coarsest level, converted from mip-4 texels into this
    // level's mip-2 texels.
    int2 coarsestIndex = clamp(int2(groupId.xy) / kCoarsestBlockRatio,
        int2(0, 0), int2(max(CoarsestWidth, 1u), max(CoarsestHeight, 1u)) - 1);
    int2 seed = int2(round(CoarsestMotionVectors.Load(int3(coarsestIndex, 0)).xy)) * kCoarsestToCoarseScale;

    int2 candidateOffset = seed + int2(groupThreadId.xy) - kSearchRadius;

    g_sad[groupIndex] = BlockSAD(blockOrigin, candidateOffset, mipDims);
    GroupMemoryBarrierWithGroupSync();

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

        int2 bestOffset = seed + int2(bestIndex % kSearchWindow, bestIndex / kSearchWindow) - kSearchRadius;
        // Stored in mip-2 texels; the fine stage scales it up by kMipScale.
        CoarseMotionVectors[groupId.xy] = float4(float2(bestOffset), 0.0, 0.0);
    }
}
