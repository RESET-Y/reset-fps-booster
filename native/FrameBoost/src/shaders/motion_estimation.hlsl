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
RWTexture2D<float2> MotionVectors : register(u0);

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
static const int kBlockSize = 16;
static const int kBlockSampleStride = 4;
// Raised from 6 after live measurement showed the search saturating: with
// radius 6 the reported maximum motion magnitude sat at exactly 8.5 px
// frame after frame, which is sqrt(6^2 + 6^2) - the diagonal corner of the
// search window. A maximum pinned to the window's own edge means content
// was moving FURTHER than the search could look, so those blocks got the
// closest wrong answer instead of the right one. That is precisely the kind
// of wrong vector that produces visible interpolation artefacts.
//
// One thread per candidate, so the group is kSearchWindow^2 threads: radius
// 12 gives 625, still inside D3D11's 1024-thread limit (radius 15 would be
// 961 - the practical ceiling for this design).
static const int kSearchRadius = 12;
static const int kSearchWindow = kSearchRadius * 2 + 1; // 25
static const int kCandidateCount = kSearchWindow * kSearchWindow; // 625

cbuffer FrameDims : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint _pad0;
    uint _pad1;
};

groupshared float g_sad[kCandidateCount];

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
    int2 candidateOffset = int2(groupThreadId.xy) - kSearchRadius;

    g_sad[groupIndex] = BlockSAD(blockOrigin, candidateOffset);
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

        int2 bestOffset = int2(bestIndex % kSearchWindow, bestIndex / kSearchWindow) - kSearchRadius;
        MotionVectors[groupId.xy] = float2(bestOffset);
    }
}
