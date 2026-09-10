// Real motion-compensated bidirectional interpolation - explicitly NOT a
// 50/50 crossfade. For each output pixel, the block's real motion vector
// (from motion_estimation.hlsl, smoothed by motion_smooth.hlsl) tells us
// where that content came from in the previous frame. We sample BOTH source
// frames at positions shifted halfway along that real motion path toward
// the midpoint, then blend those motion-compensated samples - not the
// pixel at the same fixed (x,y) in both frames.
//
// v0.3 quality pass, driven by live feedback that motion was visibly
// smoother but quality was visibly worse:
//   1. The motion field is bilinearly interpolated between neighbouring
//      blocks instead of read per-block. A single vector shared by a whole
//      32x32 block makes motion snap at block borders - textbook blocking.
//   2. Occlusion/mismatch rejection: where the two motion-compensated
//      samples disagree strongly the motion vector is simply wrong (newly
//      revealed background, thin fast-moving objects). Averaging two
//      unrelated pixels there is exactly what produced the ghosting and
//      washed-out contrast reported earlier, so those pixels fall back to
//      the real current frame instead of being blended.

Texture2D<float4> PrevFrame : register(t0);
Texture2D<float4> CurrFrame : register(t1);
Texture2D<float2> MotionVectors : register(t2); // block-resolution, from motion estimation
RWTexture2D<float4> GeneratedFrame : register(u0);

SamplerState LinearClamp : register(s0);

cbuffer InterpolationParams : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint BlockSize;
    uint DebugTintGenerated; // 1 = tint generated frames red (developer aid)

    // Where on the timeline between the two real frames this generated
    // frame sits: 0 = exactly the previous frame, 1 = exactly the current
    // one. A hardcoded 0.5 can only ever produce ONE intermediate frame,
    // i.e. a fixed 2x factor. Making it a parameter is what allows 3x
    // (t = 1/3, 2/3), 4x (t = 1/4, 1/2, 3/4) and so on from the very same
    // motion field, estimated once per real frame pair.
    float PhaseT;
    float3 _padTo32Bytes;
};

// How quickly disagreement between the two motion-compensated samples turns
// into distrust of the motion vector. Tuned so ordinary lighting/noise
// differences still blend normally, while genuinely mismatched content
// (occlusion) falls back to the real frame.
static const float kMismatchSensitivity = 6.0;

// Blending has to happen in LINEAR light, not in the gamma-encoded values
// the frame is stored in. This was the cause of the contrast loss and
// darkening noted early on and deliberately deferred: averaging two
// gamma-encoded values does not give the average brightness. The midpoint
// of 0 and 255 encoded is 128, which is only ~22% of the light - the
// correct 50% is encoded as ~188. Every pixel where the two source frames
// differ came out too dark, which is every edge and everything in motion.
//
// At two generated frames out of every three displayed, that darkening also
// arrived periodically, so the brightness itself flickered at the real
// frame rate - seen as "not quite smooth" even with perfect frame pacing.
//
// Exact sRGB transfer function, not a 2.2 power approximation: the linear
// segment near black is where banding in dark scenes would otherwise show.
float3 SrgbToLinear(float3 c)
{
    float3 low = c / 12.92;
    float3 high = pow(max(c + 0.055, 0.0) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, c));
}

float3 LinearToSrgb(float3 c)
{
    c = max(c, 0.0);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, c));
}

// Bilinear read of the low-resolution motion field. Done with four explicit
// Loads rather than a sampler so it does not depend on linear-filtering
// support for 32-bit float formats.
float2 SampleMotionBilinear(float2 pixelCenter, uint2 blockCount)
{
    // Position within the block grid, offset by half a block so that a
    // block's vector is anchored at the block's CENTRE.
    float2 gridPos = pixelCenter / BlockSize - 0.5;
    float2 baseF = floor(gridPos);
    float2 frac = gridPos - baseF;

    int2 b00 = clamp(int2(baseF), int2(0, 0), int2(blockCount) - 1);
    int2 b11 = clamp(b00 + int2(1, 1), int2(0, 0), int2(blockCount) - 1);
    int2 b10 = int2(b11.x, b00.y);
    int2 b01 = int2(b00.x, b11.y);

    float2 m00 = MotionVectors.Load(int3(b00, 0));
    float2 m10 = MotionVectors.Load(int3(b10, 0));
    float2 m01 = MotionVectors.Load(int3(b01, 0));
    float2 m11 = MotionVectors.Load(int3(b11, 0));

    return lerp(lerp(m00, m10, frac.x), lerp(m01, m11, frac.x), frac.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight)
        return;

    uint2 blockCount = (uint2(FrameWidth, FrameHeight) + BlockSize - 1) / BlockSize;
    float2 pixelCenter = float2(id.xy) + 0.5;
    float2 dims = float2(FrameWidth, FrameHeight);

    // mv is defined (see motion_estimation.hlsl) such that
    // CurrFrame(p) approx= PrevFrame(p + mv) - i.e. mv points from this
    // pixel's current position back to where that content was previously.
    float2 mv = SampleMotionBilinear(pixelCenter, blockCount);

    // Motion-compensated sample positions - THIS is what makes this real
    // interpolation rather than a static blend: both samples are pulled
    // along the actual estimated motion path toward this frame's point in
    // time, not read from the same (x,y) in both frames. Each source is
    // shifted by its own temporal distance to that point, so both land on
    // the same content: the previous frame is (1 - t) away, the current
    // frame t. At t = 0.5 this reduces to the original halfway case.
    float2 prevSamplePos = pixelCenter + (1.0 - PhaseT) * mv;
    float2 currSamplePos = pixelCenter - PhaseT * mv;

    float4 prevColor = PrevFrame.SampleLevel(LinearClamp, prevSamplePos / dims, 0);
    float4 currColor = CurrFrame.SampleLevel(LinearClamp, currSamplePos / dims, 0);

    // Confidence in this pixel's motion vector: if the two samples the
    // vector claims are "the same content, half a frame apart" do not
    // actually look alike, the vector is wrong here.
    float mismatch = dot(abs(prevColor.rgb - currColor.rgb), float3(1.0, 1.0, 1.0)) / 3.0;
    float confidence = saturate(1.0 - mismatch * kMismatchSensitivity);

    float4 safeFallback = CurrFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0);

    // Both mixes - the temporal blend and the confidence fallback - are done
    // in linear light and converted back once at the end. See the transfer
    // functions above for why this is not optional.
    float3 prevLinear = SrgbToLinear(prevColor.rgb);
    float3 currLinear = SrgbToLinear(currColor.rgb);

    // Weighted toward whichever real frame this generated frame sits closer
    // to in time, so a t = 1/3 frame resembles the previous frame rather
    // than the midpoint of the pair.
    float3 blendedLinear = lerp(prevLinear, currLinear, PhaseT);
    float3 fallbackLinear = SrgbToLinear(safeFallback.rgb);

    float4 result;
    result.rgb = LinearToSrgb(lerp(fallbackLinear, blendedLinear, confidence));
    // Fully opaque, NOT the source frames' alpha. The presenter's composition
    // swapchain uses premultiplied alpha, so whatever ends up here decides how
    // much of the screen behind shows through. Captured desktop frames carry
    // an alpha channel that is often 0 or otherwise meaningless - inheriting
    // it made generated frames semi-transparent and washed out. Because they
    // now alternate with untouched real frames, that difference showed up as a
    // brightness pulse at the real frame rate rather than as uniform softness.
    result.a = 1.0;

    // Developer aid: makes it unambiguous on screen whether generated frames
    // are actually reaching the display, and which ones they are.
    if (DebugTintGenerated != 0)
        result.rgb = lerp(result.rgb, float3(1.0, 0.0, 0.0), 0.45);

    GeneratedFrame[id.xy] = result;
}
