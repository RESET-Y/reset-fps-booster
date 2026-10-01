// MOTION BLUR ALONG THE REAL MOTION - the "Apex but it's 240 fps" look, live.
//
// Those videos are made offline by blending many recorded frames into one.
// Live there are no future frames to blend, but the estimator already knows
// where every 8x8 block of the picture moved between the last two real
// frames. Averaging a few samples along that path, centred on the pixel,
// gives the same streak a camera shutter would have caught.
//
// It is a look, not frames. Nothing here is counted as FPS and nothing here
// makes a new moment in time - the frame being blurred is exactly the one that
// would have been shown anyway.
//
// NOTHING IS KEPT SHARP ON PURPOSE. Earlier versions left still pixels (HUD,
// crosshair) and slow movement untouched; that made sharp islands next to
// smeared ones and read as "it is trying to stay sharp" - Lukas wanted it
// gone (2026-09-30). Every pixel is smeared by exactly as much as it moved.
// What does not move has no streak because it has no motion, not by a rule.
//
// Averaging happens in linear light. Blending gamma-encoded values darkens
// bright streaks, which reads as dirt, not motion.

Texture2D<float4> Source        : register(t0); // the frame about to be shown
Texture2D<float4> PrevFrame     : register(t1); // the two real frames the vectors came from, at the
Texture2D<float4> CurrFrame     : register(t2); // estimator's size (half size in Smooth Motion)
Texture2D<float4> MotionVectors : register(t3); // block resolution; xy = pixels, see motion_estimation.hlsl
RWTexture2D<float4> Output      : register(u0);

SamplerState LinearClamp : register(s0);

cbuffer BlurParams : register(b0)
{
    uint  FrameWidth;
    uint  FrameHeight;
    uint  BlockSize;
    uint  Samples;          // most samples along a streak; fewer on short ones

    float Shutter;          // streak length as a fraction of one SOURCE interval's motion
    float MaxLength;        // pixels; a wrong huge vector must not smear the whole screen
    float StaticThreshold;  // unused; kept so the constant buffer layout stays the same
    uint  DebugMode;        // 1 = show streak length as colour

    float MinLength;        // unused; see above
    float MotionScale;      // vectors from a half-size estimate count double
    uint  PixelSelect;      // 1 = Source is CurrFrame, so each pixel can pick its own vector
    uint  UsePyramid;       // 1 = Source is CurrFrame: long streaks may read its mip chain
};

float3 SrgbToLinear(float3 c)
{
    float3 low = c / 12.92;
    float3 high = pow(max(c + 0.055, 0.0) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, c));
}

float3 LinearToSrgb(float3 c)
{
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, c));
}

// Same anchoring as the interpolation shader: a block's vector sits at the
// block's centre and is interpolated between neighbours, so the streak does
// not change length in 8-pixel steps. .z is the block's MATCH ERROR - how
// badly its best offset actually matched (motion_estimation.hlsl); a clean
// match reads ~0.018, a block that matched nothing ~0.3.
float3 SampleMotionBilinear(float2 pixelCenter, uint2 blockCount)
{
    float2 gridPos = pixelCenter / BlockSize - 0.5;
    float2 baseF = floor(gridPos);
    float2 frac = gridPos - baseF;

    int2 b00 = clamp(int2(baseF), int2(0, 0), int2(blockCount) - 1);
    int2 b11 = clamp(b00 + int2(1, 1), int2(0, 0), int2(blockCount) - 1);
    int2 b10 = int2(b11.x, b00.y);
    int2 b01 = int2(b00.x, b11.y);

    float3 m00 = MotionVectors.Load(int3(b00, 0)).xyz;
    float3 m10 = MotionVectors.Load(int3(b10, 0)).xyz;
    float3 m01 = MotionVectors.Load(int3(b01, 0)).xyz;
    float3 m11 = MotionVectors.Load(int3(b11, 0)).xyz;
    const float3 m = lerp(lerp(m00, m10, frac.x), lerp(m01, m11, frac.x), frac.y);
    return float3(m.xy * MotionScale, m.z);
}

// SMOOTH STREAKS. Samples sat at the same offsets in every pixel, so a long
// streak showed as stacked copies. A per-pixel offset of less than one step
// (interleaved gradient noise) turns those steps into an even smear. This
// only softens; it never keeps anything sharp.
//
// Tried and removed 2026-10-01, all because they kept parts of the picture
// sharp ("alles soll verschwommen werden"): shortening streaks by the block
// match error, weighting samples by how alike their motion was, a per-pixel
// choice between nearby vectors (PickPixelVector, still behind "pixelselect"
// for comparisons - it picked "still" on contrasty edges and left them crisp).
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// PER-PIXEL MOTION (2026-10-01). A block carries one vector, but at an edge
// two motions meet - the weapon that turns with the player in front of a
// world that sweeps past. The block's one vector smeared the weapon's edge
// with the world's motion and the world next to it with a blend of both:
// halos and bubbles along every silhouette.
//
// Each pixel now tries the vectors of the four blocks around it and standing
// still, and keeps the one under which it actually came from the matching
// spot in the previous frame (vectors point from the current pixel to where
// it was: prev = p + v). A small three-tap patch makes the test robust. The
// interpolated vector stays unless another is clearly better, so smooth
// regions stay smooth. Nothing is kept sharp by rule: a pixel that did not
// move picks "still" because it did not move.
static const float2 kPatchX = float2(2.0, 0.0);
static const float2 kPatchY = float2(0.0, 2.0);
static const float kClearlyBetter = 0.85;   // a candidate must beat the current pick by 15 %
static const float kNoiseFloor = 0.02;      // and by more than sensor-level noise

float PatchError(float2 p, float2 v, float2 dims, float3 c0, float3 c1, float3 c2)
{
    const float3 a = PrevFrame.SampleLevel(LinearClamp, (p + v) / dims, 0).rgb;
    const float3 b = PrevFrame.SampleLevel(LinearClamp, (p + v + kPatchX) / dims, 0).rgb;
    const float3 c = PrevFrame.SampleLevel(LinearClamp, (p + v + kPatchY) / dims, 0).rgb;
    return dot(abs(a - c0) + abs(b - c1) + abs(c - c2), float3(1.0, 1.0, 1.0));
}

float2 BlockVector(int2 b, uint2 blockCount)
{
    b = clamp(b, int2(0, 0), int2(blockCount) - 1);
    return MotionVectors.Load(int3(b, 0)).xy * MotionScale;
}

float2 PickPixelVector(float2 p, float2 vInterp, uint2 blockCount, float2 dims)
{
    const float3 c0 = CurrFrame.SampleLevel(LinearClamp, p / dims, 0).rgb;
    const float3 c1 = CurrFrame.SampleLevel(LinearClamp, (p + kPatchX) / dims, 0).rgb;
    const float3 c2 = CurrFrame.SampleLevel(LinearClamp, (p + kPatchY) / dims, 0).rgb;

    float2 best = vInterp;
    float bestErr = PatchError(p, vInterp, dims, c0, c1, c2);

    const int2 b0 = int2(floor(p / BlockSize - 0.5));
    float2 cand[5];
    cand[0] = BlockVector(b0, blockCount);
    cand[1] = BlockVector(b0 + int2(1, 0), blockCount);
    cand[2] = BlockVector(b0 + int2(0, 1), blockCount);
    cand[3] = BlockVector(b0 + int2(1, 1), blockCount);
    cand[4] = float2(0.0, 0.0);
    [unroll]
    for (int i = 0; i < 5; ++i) {
        const float e = PatchError(p, cand[i], dims, c0, c1, c2);
        if (e < bestErr * kClearlyBetter - kNoiseFloor) { bestErr = e; best = cand[i]; }
    }
    return best;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight) return;

    const int2 texel = int2(id.xy);
    const float4 center = Source.Load(int3(texel, 0));

    const float2 dims = float2(FrameWidth, FrameHeight);
    const uint2 blockCount = (uint2(FrameWidth, FrameHeight) + BlockSize - 1) / BlockSize;
    const float2 p = float2(texel) + 0.5;

    // The vector covers one whole source interval; the shutter takes a slice
    // of it. Its sign does not matter - the streak is centred on the pixel.
    // EVERYTHING IS SMEARED BY ITS FULL MOTION (2026-10-01, Lukas: "alles
    // muss weg, alles soll verschwommen werden"). No confidence shortening,
    // no per-pixel choice, no weighting by motion, no minimum length - each of
    // those kept something sharp and was taken out. Only the length cap stays,
    // high, against a broken giant vector.
    const float3 motion = SampleMotionBilinear(p, blockCount);
    const float2 vPixel = (PixelSelect != 0u) ? PickPixelVector(p, motion.xy, blockCount, dims) : motion.xy;
    float2 v = vPixel * Shutter;
    float len = length(v);
    if (len > MaxLength) { v *= MaxLength / len; len = MaxLength; }

    if (DebugMode == 1u) {
        const float k = saturate(len / MaxLength);
        Output[texel] = float4(k, 1.0 - k, 0.0, 1.0);
        return;
    }

    // One sample every ~5 pixels of streak, at most Samples: a long streak
    // with too few samples shows separate copies instead of a smear.
    // LONG STREAKS READ A SMALLER COPY (2026-10-01, for the stutter after the
    // video-length exposure: blur went from ~0.2 to 0.6-1.0 ms a frame at up
    // to 32 full-size reads per pixel, at REALTIME priority, taken from the
    // game). A streak averages many pixels anyway, so reading every one at
    // full size buys nothing the eye can see. When this frame IS the
    // estimator's current frame, its mip chain is already built (half size
    // and below in Smooth Motion): one sample every ~8 px, each from the mip
    // whose texels are about one step wide - the same average for a fraction
    // of the memory traffic, which is what this engine is bound by. Short
    // streaks keep the full frame and its detail.
    const bool pyramid = (UsePyramid != 0u) && len >= 12.0;
    const uint n = pyramid ? clamp((uint)ceil(len / 8.0), 3u, max(Samples, 3u))
                           : clamp((uint)ceil(len / 5.0), 3u, max(Samples, 3u));
    const float lod = pyramid ? clamp(log2(max((len / n) / MotionScale, 1.0)), 0.0, 4.0) : 0.0;
    const float jitter = InterleavedGradientNoise(p);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    [loop]
    for (uint i = 0; i < n; ++i) {
        const float s = (i + jitter) / n - 0.5;          // -0.5 .. +0.5 along the streak
        const float2 q = clamp(p + v * s, float2(0.5, 0.5), dims - 0.5);
        const float3 c = pyramid ? CurrFrame.SampleLevel(LinearClamp, q / dims, lod).rgb
                                 : Source.SampleLevel(LinearClamp, q / dims, 0).rgb;
        sum += SrgbToLinear(c);
        weight += 1.0;
    }

    Output[texel] = float4(LinearToSrgb(sum / weight), center.a);
}
