// Builds the small thumbnail the duplicate check compares, in one dispatch.
//
// This replaces copying the whole frame into a mip-capable texture and calling
// GenerateMips on it, which built a full twelve-level pyramid of a 2560x1440
// image for every arriving frame - about a hundred times a second - to produce
// forty by twenty-two pixels. Measured at 0.7-1.8 ms per arrival, and 2.8-3.0
// once the readback stall was removed and the copy stopped hiding behind it:
// 100-300 ms of every second spent shrinking an image.
//
// Here each thread owns one thumbnail texel and averages a fixed grid of
// samples from the region it covers. Sampling rather than averaging every
// pixel is deliberate: the thumbnail only has to answer "did anything change",
// and 64 samples spread across a tile catch any change large enough to be
// visible while reading a fraction of the memory.

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Thumbnail : register(u0);

cbuffer ThumbnailDims : register(b0)
{
    uint SourceWidth;
    uint SourceHeight;
    uint ThumbWidth;
    uint ThumbHeight;
};

static const int kSamplesPerAxis = 8; // 8x8 = 64 samples per thumbnail texel

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= ThumbWidth || id.y >= ThumbHeight) return;

    // The source region this thumbnail texel stands for.
    const uint x0 = SourceWidth * id.x / ThumbWidth;
    const uint x1 = max(SourceWidth * (id.x + 1) / ThumbWidth, x0 + 1);
    const uint y0 = SourceHeight * id.y / ThumbHeight;
    const uint y1 = max(SourceHeight * (id.y + 1) / ThumbHeight, y0 + 1);

    const uint stepX = max((x1 - x0) / kSamplesPerAxis, 1u);
    const uint stepY = max((y1 - y0) / kSamplesPerAxis, 1u);

    float4 sum = 0.0;
    float count = 0.0;
    for (uint y = y0; y < y1; y += stepY)
    {
        for (uint x = x0; x < x1; x += stepX)
        {
            sum += Source.Load(int3(x, y, 0));
            count += 1.0;
        }
    }

    Thumbnail[id.xy] = count > 0.0 ? sum / count : float4(0, 0, 0, 1);
}
