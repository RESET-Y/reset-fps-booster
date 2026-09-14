// BILINEAR RESAMPLE OF THE HALF-RESOLUTION GENERATED FRAME.
//
// Interpolation at 2560x1440 costs 10-12 ms against a budget of half a source
// interval - 6.9 ms at 72 fps - and takes 86-89% of the graphics card, at which
// point the game feeding us collapses. Computing it at half resolution is a
// quarter of the work and fits with room to spare.
//
// The first attempt at putting that half-resolution result on screen replicated
// each computed pixel across a 2x2 square. That is nearest-neighbour
// magnification and it looked like it: every edge in a generated frame gained
// two-pixel stairs, which is what "the quality looks bad" was.
//
// Framegen, which does the same job for video in the browser, renders its
// inserted frames at 480 lines by default and upscales. The upscale is the part
// that makes that affordable rather than ugly, and this is it: one bilinear tap
// per output pixel, which the texture unit does in hardware for free.
//
// A generated frame stands on screen for about 7 ms between two sharp real
// ones. At half resolution with a proper filter, that is soft; at half
// resolution with nearest neighbour, it is blocky - and the eye forgives the
// first and not the second.

Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Dest : register(u0);

// Deliberately a PREFIX of the interpolation shader's constant buffer, so the
// same buffer can be bound to both passes without a second one to keep in
// step. HLSL reads the leading bytes and ignores the rest; only these two
// fields are needed here, and they sit in the first register.
//
// The pair that has to stay true: these are the DESTINATION dimensions, the
// full frame. The source dimensions are not needed - sampling by normalised
// coordinate asks the texture for "the same place", whatever its size.
cbuffer InterpolationParams : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint _unusedBlockSize;
    uint _unusedDebugTint;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight)
        return;

    // The centre of this output pixel, in normalised coordinates. Sampling the
    // half-resolution source there lands between its texels exactly where the
    // hardware's bilinear weights belong - no arithmetic of our own, and no
    // half-texel offset to get wrong, because both grids are centred the same
    // way once the coordinate is normalised.
    const float2 uv = (float2(id.xy) + 0.5) / float2(FrameWidth, FrameHeight);

    Dest[id.xy] = Source.SampleLevel(LinearClamp, uv, 0);
}
