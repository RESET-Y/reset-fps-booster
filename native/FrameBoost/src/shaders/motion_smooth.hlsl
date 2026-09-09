// Second real GPU pass: smooths the raw per-block motion field with a 3x3
// neighborhood average. Real block-matching on detailed/noisy scenes
// produces individual outlier vectors (a block "locks on" to the wrong
// offset); real motion is spatially coherent (neighboring blocks usually
// move together), so averaging genuinely reduces that noise. This is a
// standard technique in motion-compensated video coding, not a trick to
// hide bad data - it trades a little precision at true motion boundaries
// for much less speckle everywhere else.

Texture2D<float2> RawMotionVectors : register(t0);
RWTexture2D<float2> SmoothedMotionVectors : register(u0);

cbuffer BlockGridDims : register(b0)
{
    uint BlockCountX;
    uint BlockCountY;
    uint _pad0;
    uint _pad1;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= BlockCountX || id.y >= BlockCountY)
        return;

    float2 sum = float2(0, 0);
    int count = 0;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 p = int2(id.xy) + int2(dx, dy);
            if (p.x < 0 || p.y < 0 || p.x >= (int)BlockCountX || p.y >= (int)BlockCountY)
                continue;

            sum += RawMotionVectors.Load(int3(p, 0));
            count += 1;
        }
    }

    SmoothedMotionVectors[id.xy] = sum / max(count, 1);
}
