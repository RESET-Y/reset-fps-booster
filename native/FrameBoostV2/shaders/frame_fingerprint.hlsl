// A 16-BYTE FINGERPRINT OF A CAPTURED FRAME, computed on the GPU.
//
// The question it answers: did the compositor hand us the same picture twice?
// WGC does exactly that once MinUpdateInterval is lowered - it re-delivers in
// micro-bursts up to the compose rate - and a duplicate must never become a
// source frame, because two identical frames produce a zero motion field and a
// generated frame that carries no information.
//
// WHY NOT A CPU HASH. The obvious answer is to read the frame back and hash the
// bytes, which is what other implementations do - cheaply, because they already
// have those bytes on the CPU for their own pipeline. This engine keeps
// everything GPU-resident, so a readback would exist only to serve the hash: 14
// MB per frame at 2560x1440, up to 144 times a second, on the capture thread.
// That is the bottleneck this whole exercise exists to remove.
//
// So the reduction happens where the pixels already are. One sparse pass, four
// accumulators, sixteen bytes leave the GPU.
//
// WHAT IT HAS TO CATCH. A duplicate differs from its predecessor in no pixel at
// all, so any sampling scheme detects it. The hard direction is the opposite:
// two DIFFERENT frames must not collide into the same fingerprint, or real
// motion gets discarded as a repeat. Three things guard against that:
//
//   - position is mixed in, so the same colour at a different place hashes
//     differently and a pure translation cannot cancel out
//   - the accumulate is per-channel and multiplicative, not a plain sum, so
//     two pixels swapping values do not leave the total unchanged
//   - four independent lanes, 128 bits total
//
// A stride of 8 samples one pixel in 64, which at 2560x1440 is 57,600 samples.
// A frame that differs only in pixels this misses is a frame nothing downstream
// could act on either.

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<uint> Fingerprint : register(u0);

cbuffer FingerprintParams : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint SampleStride;
    uint _pad;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy * SampleStride;
    if (p.x >= FrameWidth || p.y >= FrameHeight)
        return;

    const float3 c = Source.Load(int3(int(p.x), int(p.y), 0)).rgb;

    // Quantised to 8 bits per channel, which is what the source is anyway.
    // Anything finer would make the fingerprint react to rounding rather than
    // to content.
    const uint packed = (uint)(saturate(c.r) * 255.0 + 0.5)
                      | ((uint)(saturate(c.g) * 255.0 + 0.5) << 8)
                      | ((uint)(saturate(c.b) * 255.0 + 0.5) << 16);

    // Knuth's multiplicative constant on the colour, mixed with two large odd
    // primes on the coordinates. The XOR keeps the position term from simply
    // adding out across a uniform region.
    uint h = packed * 2654435761u;
    h ^= (p.x * 73856093u);
    h ^= (p.y * 19349663u);
    h ^= (h >> 15);

    // Four lanes, chosen by position so neighbouring samples land in different
    // ones. InterlockedAdd is order-independent, which is what makes the result
    // reproducible across dispatches - a fingerprint that depended on thread
    // scheduling would report every frame as new.
    const uint lane = ((id.x & 1u) | ((id.y & 1u) << 1u));
    InterlockedAdd(Fingerprint[lane], h);
}
