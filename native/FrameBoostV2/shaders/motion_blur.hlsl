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
// Two things must NOT smear:
//   - still pixels: the HUD, the crosshair, a menu. A pixel that is the same
//     in the previous and the current real frame is left untouched, whatever
//     its block's vector says - a block that straddles the HUD edge carries
//     the scene's motion.
//   - still pixels INTO moving ones: a sample that lands on a still pixel is
//     skipped, so a streak behind the scene does not drag the HUD along.
//
// Averaging happens in linear light. Blending gamma-encoded values darkens
// bright streaks, which reads as dirt, not motion.

Texture2D<float4> Source        : register(t0); // the frame about to be shown
Texture2D<float4> PrevFrame     : register(t1); // the two real frames the vectors came from
Texture2D<float4> CurrFrame     : register(t2);
Texture2D<float4> MotionVectors : register(t3); // block resolution; xy = pixels, see motion_estimation.hlsl
RWTexture2D<float4> Output      : register(u0);

SamplerState LinearClamp : register(s0);

cbuffer BlurParams : register(b0)
{
    uint  FrameWidth;
    uint  FrameHeight;
    uint  BlockSize;
    uint  Samples;          // along the streak, >= 2

    float Shutter;          // streak length as a fraction of one SOURCE interval's motion
    float MaxLength;        // pixels; a wrong huge vector must not smear the whole screen
    float StaticThreshold;  // summed |prev - curr| per pixel below which it counts as still
    uint  DebugMode;        // 1 = show streak length as colour

    float MinLength;        // pixels; below this no blur at all, fading in up to 3x it
    float3 _pad;
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
// not change length in 8-pixel steps.
float2 SampleMotionBilinear(float2 pixelCenter, uint2 blockCount)
{
    float2 gridPos = pixelCenter / BlockSize - 0.5;
    float2 baseF = floor(gridPos);
    float2 frac = gridPos - baseF;

    int2 b00 = clamp(int2(baseF), int2(0, 0), int2(blockCount) - 1);
    int2 b11 = clamp(b00 + int2(1, 1), int2(0, 0), int2(blockCount) - 1);
    int2 b10 = int2(b11.x, b00.y);
    int2 b01 = int2(b00.x, b11.y);

    float2 m00 = MotionVectors.Load(int3(b00, 0)).xy;
    float2 m10 = MotionVectors.Load(int3(b10, 0)).xy;
    float2 m01 = MotionVectors.Load(int3(b01, 0)).xy;
    float2 m11 = MotionVectors.Load(int3(b11, 0)).xy;
    return lerp(lerp(m00, m10, frac.x), lerp(m01, m11, frac.x), frac.y);
}

bool IsStill(int2 texel)
{
    if (StaticThreshold <= 0.0) return false;   // protection switched off
    const float3 p = PrevFrame.Load(int3(texel, 0)).rgb;
    const float3 c = CurrFrame.Load(int3(texel, 0)).rgb;
    return dot(abs(p - c), float3(1.0, 1.0, 1.0)) < StaticThreshold;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight) return;

    const int2 texel = int2(id.xy);
    const float4 center = Source.Load(int3(texel, 0));

    if (IsStill(texel)) { Output[texel] = center; return; }

    const float2 dims = float2(FrameWidth, FrameHeight);
    const uint2 blockCount = (uint2(FrameWidth, FrameHeight) + BlockSize - 1) / BlockSize;
    const float2 p = float2(texel) + 0.5;

    // The vector covers one whole source interval; the shutter takes a slice
    // of it. Its sign does not matter - the streak is centred on the pixel.
    float2 v = SampleMotionBilinear(p, blockCount) * Shutter;
    float len = length(v);

    // FADE IN WITH SPEED. Slow motion stays sharp - aiming, strafing a few
    // pixels, a scene at rest - and the streak only takes over once the
    // movement is fast enough that the eye would see the gaps between frames.
    const float fade = smoothstep(MinLength, MinLength * 2.0, len);
    if (fade <= 0.0) { Output[texel] = center; return; }
    if (len > MaxLength) { v *= MaxLength / len; len = MaxLength; }

    if (DebugMode == 1u) {
        const float k = saturate(len / MaxLength);
        Output[texel] = float4(k, 1.0 - k, 0.0, 1.0);
        return;
    }

    const uint n = max(Samples, 2u);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    [loop]
    for (uint i = 0; i < n; ++i) {
        const float s = (i + 0.5) / n - 0.5;             // -0.5 .. +0.5 along the streak
        const float2 q = clamp(p + v * s, float2(0.5, 0.5), dims - 0.5);
        if (IsStill(int2(q))) continue;                   // do not drag still pixels along
        sum += SrgbToLinear(Source.SampleLevel(LinearClamp, q / dims, 0).rgb);
        weight += 1.0;
    }

    if (weight <= 0.0) { Output[texel] = center; return; }
    const float3 blurredLin = sum / weight;
    const float3 centerLin = SrgbToLinear(center.rgb);
    Output[texel] = float4(LinearToSrgb(lerp(centerLin, blurredLin, fade)), center.a);
}
