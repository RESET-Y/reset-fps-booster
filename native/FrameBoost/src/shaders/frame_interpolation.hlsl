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
Texture2D<float4> MotionVectors : register(t2); // block-resolution, from motion estimation
RWTexture2D<float4> GeneratedFrame : register(u0);

SamplerState LinearClamp : register(s0);

cbuffer InterpolationParams : register(b0)
{
    uint FrameWidth;
    uint FrameHeight;
    uint BlockSize;
    uint DebugTintGenerated; // 1 = tint generated frames red, 2 = show occluded pixels green

    // Where on the timeline between the two real frames this generated
    // frame sits: 0 = exactly the previous frame, 1 = exactly the current
    // one. A hardcoded 0.5 can only ever produce ONE intermediate frame,
    // i.e. a fixed 2x factor. Making it a parameter is what allows 3x
    // (t = 1/3, 2/3), 4x (t = 1/4, 1/2, 3/4) and so on from the very same
    // motion field, estimated once per real frame pair.
    float PhaseT;

    // Predict forward from CurrFrame by this fraction of an interval, instead
    // of interpolating between PrevFrame and CurrFrame. 0 = interpolate.
    //
    // Interpolation must hold the newest real frame back so its generated
    // partner can be shown first, and that hold is half an interval of pure
    // added latency - 7.8 ms of a measured 13.4 ms at 64 FPS, which is why
    // the raw game still felt more responsive than the boosted output.
    // Extrapolating pays none of it: the real frame is shown the moment it
    // arrives and the predicted frame follows.
    //
    // The price is that nothing behind a moving object is known - there is no
    // later frame to copy it from - so revealed areas can only be filled with
    // what the current frame already shows there.
    float ExtrapolateAhead;
    // Where the camera is being turned, in pixels of expected screen shift,
    // derived from raw mouse movement. Zero when unknown.
    float2 MousePrediction;
    // 1 = full quality, higher = the per-pixel search gives up sooner. Set from
    // measured headroom, so the same build runs at full quality where there is
    // time and backs off where there is not, instead of missing its deadline.
    float QualityRelief;
    float _mousePad;

    // Status indicator, drawn as small squares in the top-left corner so the
    // active modes are visible on screen. Needed because the hotkeys had no
    // visible feedback at all - the overlay window has no title bar and is
    // hidden from the taskbar, so mode changes could only be confirmed by
    // reading the log file.
    //   bit 0 - low-latency mode (generation factor capped at 2)
    //   bit 1 - transparency mode (real frames show the actual screen)
    uint StatusFlags;
    float2 _padTo32Bytes;
};

// How quickly disagreement between the two motion-compensated samples turns
// into distrust of the motion vector. Tuned so ordinary lighting/noise
// differences still blend normally, while genuinely mismatched content
// (occlusion) falls back to the real frame.
//
// RAISED FROM 6 TO 14 against a trail behind moving bots that survived both
// finer blocks and edge-aware smoothing. The vectors there are not wrong at
// all: ground a bot has just left is background in the new frame and was the
// bot in the old one, so blending the two halfway paints a ghost of the bot
// where it used to be. This test is the only thing standing between that and
// the screen, and at 6 the two samples had to differ by a sixth of full range
// before it fully distrusted them - which a dark bot against a dark background
// never manages. At 14 a difference of 7% is enough.
//
// The cost is that genuinely difficult pixels stop being interpolated and hold
// on the nearer real frame. A patch that stands still for one frame is a
// smaller lie than a ghost of something that has already moved on.
// 4.0, cut from 14.0 - the same mistake as kBlockErrorSensitivity, at the
// other of the two tests that can discard an interpolated pixel.
//
// This one compares the two motion-compensated samples by COLOUR. At 14 a
// pixel loses all its confidence at a 7% mean channel difference, and two
// samples taken from different frames differ by more than that almost
// everywhere in a moving scene - from lighting, from noise, from the game.s
// own antialiasing and temporal accumulation, none of which mean the vector
// is wrong.
//
// Loosening the block test alone visibly reduced how much of the screen was
// being replaced by the real frame, and the result was better but still short
// of a doubled frame rate. Two gates in series: opening one leaves the other
// closed.
//
// At 4.0 a pixel keeps its interpolation up to a 25% difference, which is far
// beyond anything ordinary rendering produces between two neighbouring frames
// and still catches a sample that landed on entirely different content.
// 1.5, after testing where the limit actually is. At this value a
// pixel keeps its interpolation up to a 67% colour difference - effectively
// only content that is completely unrelated falls back.
static const float kMismatchSensitivity = 1.5;

// How sharply a RELATIVE disagreement between a vector and the field it points
// into turns into distrust - the ratio of that disagreement to the local
// motion speed, not a pixel count.
//
// At 2.2. The relative test aimed correctly from the first live run - camera
// turns stopped being flagged, the weapon stayed calm, the trail stood alone -
// and it shortened the trail. Raising it to 3.0 then changed nothing at all,
// which says the trail pixels already sat at zero confidence and are already
// showing the real frame. Strictness beyond that buys no removal, only the
// risk of freezing edges, so this sits just above where it saturated.
//
// During a camera turn of 40 px, a neighbour differing by 6 px scores
// 0.14 and still keeps 57% of its confidence, so the scene interpolates.
// Against still ground, a vector pointing into something moving 6 px already
// scores 1.2 and keeps nothing - and that is the faint tail end of the trail
// which 1.5 was still letting through.
static const float kOcclusionSensitivity = 2.2;

// How quickly a block`s own match error turns into distrust. Measured in a
// game: a clean match scores 0.002-0.015, while a block that found nothing
// resembling itself scores above 0.06.
//
// RAISED FROM 15 TO 30 on a live report of patches of the picture visibly
// shifting - "as if the generated frames move some areas". At 15 a block is
// only fully distrusted at an error of 0.067, so the middling cases - a wrong
// vector that still half-matches - were displaced by more than half their
// distance. Measured at the same moment: 0.26% of blocks find no real match
// when little is moving, 6.34% during fast motion, which over 14,400 blocks is
// some 870 scattered patches per frame, each one moving content that did not
// move.
//
// At 30 the same full distrust arrives at 0.033, so those middling blocks now
// show the real frame at that spot instead. The cost is that genuinely
// difficult areas stop being interpolated and simply hold - a small judder in
// a corner of the picture rather than a wrong shift, which is the better
// failure of the two.
// 3.0, cut from 30.0 - the single most damaging constant in the engine.
//
// This decides how far a block.s match error pushes a pixel away from the
// interpolated result and onto the unwarped real frame. At 30 a pixel loses
// ALL confidence at an error of 0.033, and the field measures a mean error of
// 0.0177 with a maximum of 0.37 - ten times over the line. The average pixel
// was therefore already half real frame, and most of the picture was entirely
// real frame.
//
// Which means the generated frames were copies of their neighbours. Measured
// from outside, watching the screen with the same instrument used on a
// competing product: 59.9 distinct pictures a second reaching the display
// while the engine reported 144 output FPS, against 118.6 for the other
// product on the same machine in the same game. That is the whole of "it
// feels like half the frame rate", and it explains why hours of pacing work
// changed nothing: the timing of frames that carry no new content cannot
// matter. Confirmed directly by painting the fallback magenta - almost the
// entire screen was magenta.
//
// The value came from motion_smooth.hlsl, where 30 weights a NEIGHBOUR.s
// influence and a tenth of the weight at error 0.3 is sensible. Here the same
// number decides whether a pixel is interpolated at all, which is not the
// same question. At 3.0 a block with no real match at all (error ~0.3) still
// falls back completely, while the ordinary 0.0177 keeps 95% of its
// interpolation.
// 1.0 - the fallback is now almost never taken, so what is left
// is whatever the interpolation itself can do. Tested live at these values:
// smoother AND no new artifacts, which settles what these gates were doing.
// They were not catching bad interpolation. They were preventing good
// interpolation, and the picture they fell back to looked cleaner only
// because a copy of a real frame always does.
// 2.0. At 1.0 a block that matched nothing (error around 0.3) still kept 70%
// of its interpolation, and that shows up where content CHANGES without
// moving: a menu entry lighting up under the cursor has no true motion, the
// search has to name a winner anyway, and the wrong vector it names then warps
// the still layout around it. Reported as the layout sliding slightly - not
// judder, a shift, which is what a wrong vector does to text.
//
// At 2.0 that same block keeps 40% and a clean match (0.0177) still keeps 96%,
// so real motion is untouched. The right value sits between the 30 that threw
// away nearly everything and the 1.0 that trusts a block which matched
// nothing.
static const float kBlockErrorSensitivity = 2.0;

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
// Returns the motion vector in .xy and the block`s MATCH ERROR in .z - how
// well the winning candidate actually fitted, measured over 16 samples by the
// estimator. That is a far more reliable signal than comparing two single
// pixels: in a grey industrial scene two entirely different places often
// differ by less than the per-pixel test`s threshold, so it waves them
// through and the two get blended into a ghost.
// The block vectors around a pixel, so a pixel can choose between them instead
// of being handed their average.
//
// Four - the blocks this pixel.s own vector was interpolated from.
//
// It was nine, the full 3x3 neighbourhood, which reaches further into the
// band that bilinear blending spoils. But nine candidates plus standstill
// plus the mouse is eleven residuals of two samples each, and with the
// quality regulator no longer suppressing it that put interpolation at 10.5-
// 11.4 ms against a 6.94 ms deadline - output collapsed from 144 to 52 while
// motion estimation sat at a comfortable 1.4-2.1 ms.
//
// Four candidates is the version that measured 0.55 ms and was reported as
// clearly better when it first appeared. The move to nine was never A/B
// tested on its own - it went in together with other changes, and the
// regulator hid its cost immediately afterwards.
static const int kMotionCandidates = 4;

// The four block vectors around a pixel, read ONCE.
//
// They were being read three times over: four Loads for the bilinear vector,
// four more for the candidate list, four more for the occlusion diagnostic.
// The same four texels every time. One read, three uses.
void LoadBlockMotion(float2 pixelCenter, uint2 blockCount,
                     out float3 corners[kMotionCandidates], out float2 frac)
{
    // Position within the block grid, offset by half a block so that a block's
    // vector is anchored at the block's CENTRE.
    const float2 gridPos = pixelCenter / BlockSize - 0.5;
    const float2 baseF = floor(gridPos);
    frac = gridPos - baseF;

    const int2 b00 = clamp(int2(baseF), int2(0, 0), int2(blockCount) - 1);
    const int2 b11 = clamp(b00 + int2(1, 1), int2(0, 0), int2(blockCount) - 1);

    corners[0] = MotionVectors.Load(int3(b00, 0)).xyz;                  // 00
    corners[1] = MotionVectors.Load(int3(int2(b11.x, b00.y), 0)).xyz;   // 10
    corners[2] = MotionVectors.Load(int3(int2(b00.x, b11.y), 0)).xyz;   // 01
    corners[3] = MotionVectors.Load(int3(b11, 0)).xyz;                  // 11
}

float3 BilinearFromCorners(float3 corners[kMotionCandidates], float2 frac)
{
    return lerp(lerp(corners[0], corners[1], frac.x),
                lerp(corners[2], corners[3], frac.x), frac.y);
}

// ---------------------------------------------------------------------------
// VECTOR VALIDATION - two questions asked BEFORE a pixel is moved at all.
//
// Everything else in this shader judges a vector by its RESULT: sample the two
// frames along it and see whether they agree. That test has a blind spot, and
// it is the one behind "in Apex ist jetzt alles komplett doppelt". When the
// camera whips around, a block moves further between two frames than the
// estimator can reliably resolve; the vector it returns is then not a
// measurement but a guess, and a wrong vector in a detailed scene can still
// make two arbitrary patches of texture agree well enough to pass the test.
// The result is content moved to a place it never was - a hard, displaced
// second copy.
//
// These two ask about the VECTOR ITSELF rather than about its result, and
// where they fail the pixel is not moved at all. See the cross-fade below.
// ---------------------------------------------------------------------------

// 1. MAGNITUDE - REMOVED, and the measurement is worth keeping.
//
//    There was a gate here that refused to displace anything moving faster
//    than 64 px, then 240 px, per real-frame interval. It came from a sound-
//    sounding premise: during a fast turn the displacement exceeds what the
//    search can resolve, so the vector is a guess.
//
//    Our own counters do not support it. Measured in Apex during hard turns,
//    with peak motion of 308 px:
//
//        search-saturated blocks     0%  (one 1.5% outlier)
//        blocks with no real match   mostly under 0.3%, 8.5% at the peak
//
//    Zero blocks pinned at the edge of the search window means the pyramid is
//    reaching the motion, not running out of road. And that fits what block
//    matching is actually good at: a fast camera pan moves the whole picture
//    TOGETHER, which is the easiest thing a coarse search can find. Large
//    motion is not the failure case; large INCOHERENT motion is.
//
//    So the gate was refusing to displace exactly the frames it estimates
//    best. On screen: the whole picture blue on every turn, and a stutter with
//    it, because a cross-fade carries no step of motion. Diagnosed by looking,
//    which is the only reason the premise ever got tested.

// 2. COHERENCE. How far the four surrounding block vectors may disagree with
//    the one this pixel was handed.
//
//    Real motion is shared: an object moves as one, a camera pan moves the
//    whole picture together. Four neighbouring blocks pointing four different
//    ways is not a scene that does that - it is a search that found nothing
//    and returned noise. Which is precisely the case in foliage at speed and
//    in ground texture during a low pass, where the SAD surface is almost flat
//    and a different candidate wins in every block.
//
//    Judged RELATIVE to how fast this area is moving, for the reason the
//    occlusion work established the hard way: during a camera pan neighbouring
//    blocks differ by several pixels from perspective alone, everywhere at
//    once. An absolute threshold fires across the whole screen the moment the
//    view turns, which is worthless. The floor plus a share of the local speed
//    asks "disagreeing by more than this scene's own perspective can explain".
//
//    16 px, up from 6 and then 12. At 6 the floor sat below ordinary estimator
//    noise: standing perfectly still, ten pixels of disagreement between two
//    neighbouring blocks already pulled trust down, and the diagnostic duly
//    showed blue on a motionless screen. At 12 the weapon in the player's own
//    hands was still going blue - a near-field object with its own sway
//    against a still world genuinely does have two motions along its edge, and
//    that edge is precisely what the per-pixel candidate selection below
//    already handles well. It does not need rescuing by a cross-fade.
//
//    The slope matters more, and is now 1.0 - full proportionality. Perspective
//    spread grows WITH speed: the near half of a scene sweeps past faster than
//    the far half, in proportion. A quarter, then a half, understated it at
//    exactly the speeds this test has to survive, so an honest 200 px pan was
//    read as chaos.
//
//    What survives at 1.0 is only real incoherence: neighbours pointing
//    OPPOSITE ways, where the spread is about twice the speed rather than a
//    fraction of it. That is foliage at speed and ground texture in a low pass
//    - the cases this was built for.
static const float kCoherenceFloor = 16.0;
static const float kCoherenceSlope = 1.0;

float3 SampleMotionBilinear(float2 pixelCenter, uint2 blockCount)
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

    float3 m00 = MotionVectors.Load(int3(b00, 0)).xyz;
    float3 m10 = MotionVectors.Load(int3(b10, 0)).xyz;
    float3 m01 = MotionVectors.Load(int3(b01, 0)).xyz;
    float3 m11 = MotionVectors.Load(int3(b11, 0)).xyz;

    return lerp(lerp(m00, m10, frac.x), lerp(m01, m11, frac.x), frac.y);
}

// How badly the two frames disagree around this pixel when read along v. Zero
// means both frames show the same content there, which is what a correct
// vector produces.
//
float Residual(float2 pixelCenter, float2 dims, float2 v)
{
    const float3 p = PrevFrame.SampleLevel(LinearClamp, (pixelCenter + (1.0 - PhaseT) * v) / dims, 0).rgb;
    const float3 q = CurrFrame.SampleLevel(LinearClamp, (pixelCenter - PhaseT * v) / dims, 0).rgb;
    return dot(abs(p - q), float3(1.0, 1.0, 1.0));
}

// A five-tap window version of this lived here and was removed with the
// per-pixel search it was built for. It broke ties honestly - on a wall, in
// sky, in smoke, a dozen vectors land on the same colour and a single pixel
// lets noise pick the winner - but five times the samples is five times the
// cost on exactly the pixels that already cost the most, and in a moving game
// that is most of the screen.

// Catmull-Rom sampling, for the two motion-compensated reads that become the
// generated frame.
//
// Those reads are at fractional positions - "6.3 pixels left of here" - and
// bilinear filtering at a fractional position is a low-pass filter: it mixes
// four neighbours and throws away detail every time. Two such reads, averaged,
// give a frame measurably softer than either real frame beside it. A soft
// frame between two sharp ones does not read as a step of motion; the eye
// takes it for blur on the previous one. That is the difference the tester
// sees between this and a product whose generated frames are sharp: measured
// on screen, both deliver about the same number of frames at about the same
// spacing - 112 against 119 per second - so what is left is what they contain.
//
// Catmull-Rom is the standard answer: it reconstructs the value between
// samples from a cubic through them instead of a straight line, which keeps
// edges crisp rather than averaging them away. Nine bilinear taps in the
// usual formulation, five in this one - the corner weights are small enough
// that dropping them is invisible, and it is the form everyone ships.
float3 SampleCatmullRom(Texture2D<float4> tex, float2 posPixels, float2 dims)
{
    const float2 samplePos = posPixels;
    const float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    const float2 f = samplePos - texPos1;

    // Catmull-Rom weights for the four taps along each axis.
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    // The middle two taps are fetched as one bilinear sample at a weighted
    // position - the trick that turns nine taps into five.
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / max(w12, 1e-5);

    const float2 texPos0 = (texPos1 - 1.0) / dims;
    const float2 texPos3 = (texPos1 + 2.0) / dims;
    const float2 texPos12 = (texPos1 + offset12) / dims;

    float3 result = 0.0;
    result += tex.SampleLevel(LinearClamp, float2(texPos12.x, texPos0.y), 0).rgb * w12.x * w0.y;
    result += tex.SampleLevel(LinearClamp, float2(texPos0.x, texPos12.y), 0).rgb * w0.x * w12.y;
    result += tex.SampleLevel(LinearClamp, float2(texPos12.x, texPos12.y), 0).rgb * w12.x * w12.y;
    result += tex.SampleLevel(LinearClamp, float2(texPos3.x, texPos12.y), 0).rgb * w3.x * w12.y;
    result += tex.SampleLevel(LinearClamp, float2(texPos12.x, texPos3.y), 0).rgb * w12.x * w3.y;

    // The five taps do not sum to one, so the result is renormalised rather
    // than left darker or brighter than the source.
    const float weightSum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y
                          + w3.x * w12.y + w12.x * w3.y;
    return max(result / max(weightSum, 1e-5), 0.0);
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
    float3 blockCorners[kMotionCandidates];
    float2 blockFrac;
    LoadBlockMotion(pixelCenter, blockCount, blockCorners, blockFrac);

    float3 motionAndError = BilinearFromCorners(blockCorners, blockFrac);
    float2 mv = motionAndError.xy;
    float blockMatchError = motionAndError.z;

    // --- vector validation, on the vector the block grid handed us ---------
    //
    // Deliberately judged BEFORE the per-pixel selection below. A pixel that
    // picks one of four chaotic neighbours has not found the truth; it has
    // picked a different piece of noise, and it would pick it ON RESIDUAL - so
    // the residual test cannot be the one to catch this.
    const float motionMagnitude = length(mv);

    // The largest disagreement among the four, not their average: an average
    // hides one wild neighbour among three sane ones, and one wild neighbour
    // already means the block grid does not know what is happening here.
    float coherenceSpread = 0.0;
    [unroll]
    for (int nb = 0; nb < kMotionCandidates; ++nb)
        coherenceSpread = max(coherenceSpread, length(blockCorners[nb].xy - mv));

    // A ramp rather than a switch. A hard switch would trade the double image
    // for a visible outline around every fast object, popping on and off at
    // the generated frame rate - the shape of every per-pixel switch that has
    // been tried in this shader and reverted.
    //
    // Full trust up to the limit, zero at twice it. How much of this pixel's
    // displacement is allowed to happen.
    const float coherenceLimit = kCoherenceFloor + motionMagnitude * kCoherenceSlope;
    const float vectorTrust = saturate(2.0 - coherenceSpread / max(coherenceLimit, 1e-3));

    // Where nothing may be displaced, nothing is COMPUTED either.
    //
    // This skips the per-pixel candidate search, two Catmull-Rom
    // reconstructions and an eight-tap unsharp pass - about forty texture
    // reads - on exactly the pixels that are currently the most expensive,
    // because fast chaotic motion is what makes the incumbent fail and the
    // search run. The safety fix and the cost fix are the same edit.
    if (vectorTrust <= 0.0 && ExtrapolateAhead <= 0.0)
    {
        // Weighted by PhaseT rather than a fixed half: at a factor of 2 the
        // generated frame sits at t = 0.5 and this IS 50/50, but at 3x it sits
        // at 1/3 and 2/3, where a fixed half would place the picture at the
        // wrong instant - the same mistake the confidence fallback further
        // down already had to be corrected for.
        const float3 pLin = SrgbToLinear(PrevFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0).rgb);
        const float3 cLin = SrgbToLinear(CurrFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0).rgb);
        float4 outColor = float4(LinearToSrgb(lerp(pLin, cLin, PhaseT)), 1.0);

        if (DebugTintGenerated == 1) outColor.r = min(outColor.r + 0.35, 1.0);
        // "showblend": paint every cross-faded pixel blue, so the area this
        // fallback actually covers can be seen instead of guessed at. If the
        // screen turns blue during an ordinary turn, the thresholds are wrong
        // and this is costing picture everywhere rather than rescuing edges.
        if (DebugTintGenerated == 5) outColor.rgb = float3(0.1, 0.3, 1.0);

        // The badge belongs on every path that writes a frame. It was once
        // missing from the extrapolation branch and therefore never drawn at
        // all, while 60 frames a second were being generated.
        if ((StatusFlags & 4u) != 0)
        {
            const int kBarWidth = 64, kBarHeight = 6, kBarMargin = 12;
            const int2 b = int2(id.xy) - int2(kBarMargin, kBarMargin + 20);
            if (b.x >= 0 && b.x < kBarWidth && b.y >= 0 && b.y < kBarHeight)
                outColor.rgb = float3(0.1, 0.95, 0.3);
        }

        GeneratedFrame[id.xy] = outColor;
        return;
    }

    // PER-PIXEL vector selection, between the interpolated vector and the four
    // block vectors it was interpolated from.
    //
    // The interpolated vector is right in the middle of an object and right in
    // the middle of the background, and wrong along the boundary between them -
    // where it is a mixture of two motions that no content actually has. A
    // pixel there is pulled to a position that belongs to neither, which is
    // what draws a moving object a second time a few pixels off. Blocks are 8
    // px and the interpolation spans two of them, so this band is 16 px wide
    // around every moving edge in the picture.
    //
    // A pixel can settle this for itself without any search: a vector is right
    // for this pixel when the two frames, sampled along it, agree HERE. So the
    // candidates are tested and the one with the smallest disagreement wins.
    // Five candidates, no extra motion-field reads beyond the four corners,
    // and every pixel inside an object keeps the vector it already had,
    // because there all five candidates are the same.
    //
    // This is the standard answer in frame-rate conversion for exactly this
    // artefact - per-pixel selection among neighbouring block vectors rather
    // than smoothing the vector field, which cannot help: the field is not
    // noisy, it is correct on both sides and undefined in between.
    {

        // The interpolated vector is the incumbent: it starts as the winner, so
        // a neighbour has to be strictly better to displace it.
        float bestResidual = Residual(pixelCenter, dims, mv);
        float2 bestMv = mv;
        float bestError = blockMatchError;

        // Only pixels the incumbent FAILS pay for the rest.
        //
        // Where the blended vector already brings the two frames into
        // agreement, no other candidate can do better than agreement, and the
        // search is nine residuals spent to confirm what one already said. That
        // is most of the picture: the interior of every object and of the
        // background, where all the neighbouring blocks agree anyway.
        //
        // Triggering on how much the neighbouring VECTORS differ was tried
        // first and does not work - during a camera pan, neighbouring blocks
        // differ by several pixels from perspective alone, everywhere at once,
        // so it fired across the whole picture and saved nothing. The residual
        // asks the question that actually matters: not "do the blocks around me
        // disagree" but "is what I have wrong HERE".
        //
        // 0.045 is a mean absolute difference of 1.5% per channel across three
        // channels - comfortably above sampling noise on a clean match, well
        // below the disagreement at an edge where two motions meet.
        // Scaled by the measured headroom. At relief 1 this is the tolerance the
        // quality work was tuned with; at 4 only badly broken pixels still pay
        // for the search, which is the difference between a game where we have
        // a third of the graphics card and one where we have a tenth.
        //
        // Backing off beats missing the deadline: a slightly worse pixel is
        // shown on time, and a better one that arrives late is not shown at
        // all. Measured in War Thunder before this existed - 40% of generated
        // frames late, 80-91% of the card taken from a game that needed it.
        const float kIncumbentTolerance = 0.045 * max(QualityRelief, 1.0);
        if (bestResidual > kIncumbentTolerance)
        {
            // The same four corners already loaded at the top of CSMain.
            float3 candidates[kMotionCandidates] = blockCorners;



        [unroll]
        for (int c = 0; c < kMotionCandidates; ++c)
        {
            const float residual = Residual(pixelCenter, dims, candidates[c].xy);
            if (residual < bestResidual)
            {
                bestResidual = residual;
                bestMv = candidates[c].xy;
                bestError = candidates[c].z;
            }
        }

        // STANDING STILL is always a candidate, whatever the blocks report.
        //
        // A crosshair, an ammo counter, a health bar: screen-space overlays do
        // not move while the camera sweeps the world behind them at 100 px a
        // frame. Every block covering them is dominated by that world, so no
        // block reports "still" and until now no pixel could choose it - the
        // HUD was dragged along with the scene. The estimator has a zero-motion
        // candidate for the same reason; this is its per-pixel counterpart, and
        // it costs one more residual.
        //
        // It has to be clearly better, not merely equal: where the picture is
        // flat, standing still looks as good as any real motion, and letting it
        // win ties would freeze smooth surfaces.
        //
        // And it is only offered where this pixel.s block has ACTUALLY been
        // standing still for a while - .w counts the frames.
        //
        // Without that condition any pixel could take it, and next to a
        // crosshair that is a real trap: the world rushes past at over a
        // hundred pixels a frame while the HUD holds perfectly still, so at
        // that boundary a landscape pixel can find "did not move" a better
        // match than its own vector by pure coincidence, and freezes. The HUD
        // stays sharp, the landscape beside it tears - which is exactly how it
        // was reported from a low-altitude pass in a flight game.
        //
        // Eight frames is long enough that an accident does not qualify and
        // short enough that a menu closing stops being treated as static
        // almost at once.
        const float staticFrames = MotionVectors.Load(int3(clamp(int2(pixelCenter / BlockSize),
            int2(0, 0), int2(blockCount) - 1), 0)).w;
        const float stillResidual = (staticFrames >= 8.0)
            ? Residual(pixelCenter, dims, float2(0.0, 0.0))
            : 1e30;
        if (stillResidual < bestResidual * 0.8)
        {
            bestResidual = stillResidual;
            bestMv = float2(0.0, 0.0);
        }

        // THE MOUSE is a candidate too.
        //
        // Every other candidate here comes from two frames that are already in
        // the past. None of them can know that the player has just flicked
        // right, because no rendered frame shows it yet - the input exists a
        // whole frame before the picture it eventually produces, and in a
        // shooter the mouse IS the camera, so it is the dominant motion of the
        // next frame. This is what VR calls reprojection.
        //
        // Offering it as a candidate rather than applying it is what makes it
        // safe, and it also sidesteps a calibration that measurement showed we
        // cannot get exactly: mouse counts convert to pixels through the game.s
        // sensitivity and field of view, and fitting that against measured
        // picture motion gave a vertical factor stable to 1% but a horizontal
        // one wandering by a factor of two - because horizontal picture motion
        // comes from strafing as well as from turning, while vertical motion is
        // almost purely the mouse.
        //
        // As a candidate it needs no precision. If the prediction is right it
        // wins on residual and the generated frame shows input the game has not
        // drawn yet; if it is wrong - because the player was strafing, or the
        // pixel belongs to a weapon or an enemy rather than the world - it
        // loses and costs one comparison. The picture judges, not the
        // calibration.
        if (any(MousePrediction != 0.0))
        {
            const float mouseResidual = Residual(pixelCenter, dims, MousePrediction);
            if (mouseResidual < bestResidual)
            {
                bestResidual = mouseResidual;
                bestMv = MousePrediction;
            }
        }

        // A per-pixel SEARCH around the winner was tried here and removed.
        //
        // The idea is sound and nothing else covers it: everything above is
        // selection, so an object narrower than a block, or moving unlike every
        // block that overlaps it, never had its vector estimated at all. A
        // weapon barrel or an arm is exactly that.
        //
        // It cost too much, and the reason the first measurements missed that
        // is worth more than the feature: they were taken on an idle desktop,
        // where almost nothing moves, almost no pixel fails and the expensive
        // branch is almost never entered - 0.5 ms. In a game half the screen is
        // moving, the branch fires across all of it, and the same code measured
        // up to 18.5 ms with 59 of 212 seconds over budget and eight stalls.
        // Reported as the booster switching itself off, which it was.
        //
        // Anything whose cost depends on picture content has to be measured on
        // picture content.

        }

        mv = bestMv;
        blockMatchError = bestError;
    }

    // OCCLUSION, found in the motion field itself rather than in the colours.
    //
    // The trail behind a moving object is not a wrong vector - every vector
    // there is right. The ground an object has just left is background in the
    // new frame and was the object in the old one, so blending the two halfway
    // paints a ghost of the object where it used to be. The colour test that
    // was supposed to catch this needs the two samples to look different, and a
    // dark bot against a dark background never does; raising its sensitivity
    // from 6 to 14 changed nothing visible.
    //
    // This asks a geometric question instead: follow this pixel's own vector to
    // where it says the content came from, and read the field THERE. On honest
    // motion the two agree - the whole object is moving together. Across an
    // occlusion they disagree, because the place a vector points into belongs
    // to something else entirely. Colour-blind by construction, so darkness
    // does not hide it.
    //
    // One extra motion-field read per pixel, against 169 candidates per block
    // for the search that produced it.
    // Only computed when the diagnostic that uses it is on.
    //
    // This costs four motion-field reads per pixel and its result drives
    // nothing: occlusionConfidence was taken out of the confidence calculation
    // after three attempts at using it all made the picture worse, and it now
    // feeds only the "showocclusion" tint. Four reads per pixel on every pixel
    // of every generated frame, to colour a diagnostic nobody is looking at.
    const float3 motionAtSource = (DebugTintGenerated == 2)
        ? SampleMotionBilinear(pixelCenter + mv, blockCount)
        : float3(0.0, 0.0, 0.0);
    const float2 motionDisagreement = motionAtSource.xy - mv;

    // Judged RELATIVE to how fast this area is moving, not in absolute pixels.
    //
    // An absolute threshold marked the whole picture as occluded during any
    // camera movement, which the diagnostic showed directly: the trail lit up
    // green, and so did every surface in the scene as soon as the view turned.
    // Of course it did - a turning camera moves the entire image, and
    // perspective makes neighbouring areas differ by several pixels while
    // every one of those vectors is correct. At 40 px of camera motion, 6 px
    // of difference is agreement; against still ground, 20 px is an object
    // that does not belong there.
    //
    // Dividing by the local speed asks the right question: not "how
    // different", but "how different compared to what is happening here". The
    // 4 px floor keeps a still area from dividing by nearly zero.
    const float localSpeed = length(mv) + 4.0;
    const float relativeDisagreement = length(motionDisagreement) / localSpeed;
    const float occlusionConfidence = saturate(1.0 - relativeDisagreement * kOcclusionSensitivity);

    // Motion-compensated sample positions - THIS is what makes this real
    // interpolation rather than a static blend: both samples are pulled
    // along the actual estimated motion path toward this frame's point in
    // time, not read from the same (x,y) in both frames. Each source is
    // shifted by its own temporal distance to that point, so both land on
    // the same content: the previous frame is (1 - t) away, the current
    // frame t. At t = 0.5 this reduces to the original halfway case.
    // Extrapolation: there is only one frame to read from, and the content is
    // carried FORWARD along the motion it already had. Content moved by -mv
    // between the two real frames, so the pixel at p a fraction a later was at
    // p + a*mv in the current frame.
    if (ExtrapolateAhead > 0.0)
    {
        float2 aheadPos = pixelCenter + ExtrapolateAhead * mv;
        float4 aheadColor = CurrFrame.SampleLevel(LinearClamp, aheadPos / dims, 0);

        // No second frame exists at this instant, so the per-pixel agreement
        // test that interpolation uses is not available. The block's own match
        // error is: where the estimator found no real match, the vector is a
        // guess and moving the picture by it would smear.
        float aheadConfidence = saturate(1.0 - blockMatchError * kBlockErrorSensitivity);
        float4 aheadFallback = CurrFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0);

        float3 aheadLinear = SrgbToLinear(aheadColor.rgb);
        float3 aheadFallbackLinear = SrgbToLinear(aheadFallback.rgb);
        float3 aheadResult = lerp(aheadFallbackLinear, aheadLinear, aheadConfidence);

        float4 outColor = float4(LinearToSrgb(aheadResult), 1.0);
        if (DebugTintGenerated) outColor.r = min(outColor.r + 0.35, 1.0);

        // The badge has to be drawn here as well.
        //
        // This branch writes its frame and returns, and everything below it -
        // including the status drawing at the end - is skipped. Since this is
        // the branch the engine uses by default, the badge was never drawn at
        // all: reported as "kein Balken" while 59-64 frames a second were
        // being generated. A status light behind an early return reports
        // nothing about anything.
        if ((StatusFlags & 4u) != 0)
        {
            const int kBarWidth = 64, kBarHeight = 6, kBarMargin = 12;
            const int2 b = int2(id.xy) - int2(kBarMargin, kBarMargin + 20);
            if (b.x >= 0 && b.x < kBarWidth && b.y >= 0 && b.y < kBarHeight)
                outColor.rgb = float3(0.1, 0.95, 0.3);
        }

        GeneratedFrame[id.xy] = outColor;
        return;
    }

    float2 prevSamplePos = pixelCenter + (1.0 - PhaseT) * mv;
    float2 currSamplePos = pixelCenter - PhaseT * mv;

    // Catmull-Rom, and it earns its ten texture reads after all.
    //
    // It was removed as dead weight: it is the better reconstruction filter,
    // but tested on its own it made no visible difference, and at five reads
    // per sample against one it was a third of an interpolation pass that had
    // grown to 9 ms against an 8.3 ms deadline.
    //
    // Removing it brought the double images straight back. Bilinear filtering
    // at a fractional position mixes four neighbours, so each of the two
    // warped samples is smeared before they are combined - and two smeared
    // samples that disagree even slightly overlap visibly, where two sharp
    // ones do not. It was not invisible; it was invisible in isolation, at a
    // time when the doubling had other causes large enough to hide it.
    float4 prevColor = float4(SampleCatmullRom(PrevFrame, prevSamplePos, dims), 1.0);
    float4 currColor = float4(SampleCatmullRom(CurrFrame, currSamplePos, dims), 1.0);

    // Confidence in this pixel's motion vector: if the two samples the
    // vector claims are "the same content, half a frame apart" do not
    // actually look alike, the vector is wrong here.
    float mismatch = dot(abs(prevColor.rgb - currColor.rgb), float3(1.0, 1.0, 1.0)) / 3.0;
    float pixelConfidence = saturate(1.0 - mismatch * kMismatchSensitivity);

    // The block's own match error decides as well, and the stricter of the two
    // wins. Seen in a dumped frame during a fast turn: the half of the picture
    // that the turn was revealing came out as two views superimposed - a clean
    // double image. Those pixels are blended because the per-pixel test finds
    // them similar enough, which in a grey industrial scene two entirely
    // different places often are. The block's error, measured over 16 samples
    // by the estimator, does not make that mistake: where no real match was
    // found it is high regardless of how similar two individual pixels happen
    // to look.
    float blockConfidence = saturate(1.0 - blockMatchError * kBlockErrorSensitivity);

    // The stricter of the pixel and block tests decides how far to fall back
    // to an UNWARPED real frame. Both describe content that is not properly
    // present in both frames, and for those a still patch is the least bad
    // answer.
    //
    // The occlusion test is deliberately NOT part of this minimum, although it
    // was at first, and that was the bug behind the trail.
    //
    // Painting the fallback magenta showed it immediately: magenta covered
    // every moving object, not just the area being uncovered behind one. That
    // is what the cross-check actually detects - an object's own vectors point
    // back into a region the field describes differently, so the whole object
    // disagrees, not only its trailing edge. Feeding that into the static
    // fallback froze each moving object on its previous position for one
    // generated frame, so it appeared twice in the same place and then jumped.
    // That doubling IS the trail: it was reported as trailing behind moving
    // bots, it disappeared when the booster was switched off, and raising the
    // strictness from 1.5 to 3.0 changed nothing because the whole object was
    // already pinned at zero.
    //
    // An occluded pixel is not missing content - it is content that one of the
    // two frames shows properly and the other does not. So it still gets
    // motion compensation; it just stops being an average of both frames and
    // is taken from the nearer one alone, below.
    float confidence = min(pixelConfidence, blockConfidence);

    // Where the motion vector cannot be trusted, fall back to the real frame
    // this generated frame is NEARER TO IN TIME - not always the current one.
    //
    // Always falling back to CurrFrame is correct only at the symmetric
    // midpoint. At phase 1/3 the current frame is two thirds of an interval
    // in the future, so every low-confidence pixel jumped forward and then
    // back again on the next frame. That flicker appears only at the
    // asymmetric phases, which is to say only at factors above 2 - matching
    // the report that a factor of 2 looked better even after its latency
    // advantage was gone.
    float4 safeFallback = (PhaseT < 0.5)
        ? PrevFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0)
        : CurrFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0);

    // Diagnostic (DebugTintGenerated == 3): paint the FALLBACK itself, not the
    // pixels the occlusion test flags.
    //
    // Every reading so far agreed that trail pixels end at zero confidence and
    // are therefore replaced by the untouched real frame - which contains no
    // trail. The trail survives anyway, and raising the strictness from 1.5 to
    // 3.0 changed nothing. One assumption in that chain was never tested: that
    // the region the detector marks IS the region the trail occupies. They
    // only looked alike on screen.
    //
    // Colouring the replacement answers it without ambiguity. If the trail
    // comes out magenta, those pixels really are being replaced. If the trail
    // keeps its normal colours while magenta sits somewhere else, the detector
    // has been marking the wrong place all along.
    if (DebugTintGenerated == 3)
        safeFallback.rgb = float3(1.0, 0.0, 1.0);

    // Both mixes - the temporal blend and the confidence fallback - are done
    // in linear light and converted back once at the end. See the transfer
    // functions above for why this is not optional.
    float3 prevLinear = SrgbToLinear(prevColor.rgb);
    float3 currLinear = SrgbToLinear(currColor.rgb);

    // Both samples are motion-compensated to the SAME instant t, so when they
    // agree their average is the best estimate - and the sharpest, since
    // averaging two views of the same content cancels sampling noise. Only
    // when they disagree does it matter which one to believe, and then the
    // temporally NEARER frame wins: its sample was pulled a shorter distance
    // along the motion path, so it is the less likely of the two to have
    // landed on the wrong content.
    //
    // The previous rule weighted purely by temporal position (weight = t),
    // which had it backwards at the asymmetric phases. At t = 1/3 the
    // previous frame is sampled 2/3 of the way along the motion - the larger
    // displacement, the higher error risk - and was then given 2/3 of the
    // weight. That is why a factor of 3, whose frames sit at 1/3 and 2/3,
    // looked worse than a factor of 2, whose single frame sits at the
    // symmetric midpoint: reported directly, "low latency mode is better",
    // and it stayed better after the latency gap was closed to ~5 ms.
    float nearestSource = (PhaseT < 0.5) ? 0.0 : 1.0; // 0 = previous frame
    // Occlusion was applied here for one build, and that was worse again: the
    // cross-check fires on every moving pixel, so half the picture switched to
    // a single source on generated frames and pulsed against the real ones -
    // reported as everything flickering, "like a trail effect was put on top".
    //
    // The test does not detect occlusion. It detects MOTION, which is why both
    // ways of consuming it hurt: as a fallback it froze moving objects, as a
    // source selector it de-blended them. A signal that fires on every moving
    // pixel carries no information to act on, so it now drives nothing and is
    // kept only for the "showocclusion" diagnostic.
    //
    // Worth recording plainly: the trail existed before this test was written.
    // It was an attempted fix, never the cause, and it fixed nothing.
    //
    // How much of the FARTHER frame is allowed into the mix at full confidence.
    //
    // 0.5 - a straight average - is the textbook answer and is right only when
    // the vectors are exact. They never are: a few pixels of error in a
    // detailed scene lays two slightly offset views of the same content on top
    // of each other, which is the definition of blur. Sharp real frames then
    // alternate with soft generated ones, and the eye reads the pair as one
    // blurred image rather than two crisp ones - reported exactly that way,
    // "like 30 fps with high motion blur".
    //
    // 0.3 was tried and reverted. It did reduce the blur, but the generated
    // frame is supposed to stand in the MIDDLE between two real ones, and
    // weighting it toward the nearer source moves it there in space as well as
    // in colour. The output then arrives in pairs - two nearly identical
    // pictures, a jump, two more - which is worse than softness. Reported as
    // looking worse the SLOWER the camera moved, which fits exactly: at speed
    // the pairing is lost in the motion, while a slow pan lets the eye track
    // an edge and see it stall.
    //
    // The blur is real, but the cure is better vectors or sharpening, not a
    // weight that buys sharpness by putting the frame at the wrong instant.
    //
    // Originally: at 0.3 the generated frame is mostly the temporally nearer source,
    // motion-compensated to this instant, with the other frame contributing
    // enough to cancel sampling noise but not enough to ghost. Applied
    // uniformly across the whole picture, unlike the per-pixel switch that
    // produced a flickering patchwork.
    static const float kFarFrameWeight = 0.5;

    // ...but only while the two samples land on the SAME content, and that
    // depends on speed.
    //
    // A vector's error scales with its length. At 200 px of displacement a 1%
    // error is 2 px, and averaging two copies of an edge 2 px apart IS a
    // double edge - which is what came back the moment the cross-fade stopped
    // covering fast turns. blendTrust cannot catch this by construction: 2 px
    // of offset on similar content produces only a small colour difference, so
    // the test waves it through. The doubling appears exactly where the
    // diagnostic says the vectors are FINE - coherent neighbours, no search
    // saturation, low match error. Slightly wrong, not wrong.
    //
    // So the mixture narrows as the displacement grows: the far frame's share
    // falls from a half to an eighth between 24 px and 120 px per interval.
    //
    // This is a narrowed form of something already tried and reverted. A flat
    // 0.3 everywhere failed because it moves the generated frame toward one
    // source in SPACE as well as in colour, so the output arrives in pairs -
    // two near-identical pictures, a jump, two more - and it was reported as
    // looking worse the SLOWER the camera moved, because a slow pan lets the
    // eye track an edge and see it stall.
    //
    // That failure mode is what the threshold is for. Below 24 px per interval
    // - 1700 px per second at 72 fps - nothing changes and the symmetric
    // average stands. Above it no eye is tracking an edge, and a sharp frame
    // taken mostly from one source beats two overlaid copies.
    const float finalSpeed = length(mv);
    const float speedFade = saturate((finalSpeed - 24.0) / 96.0);
    const float farFrameWeight = lerp(kFarFrameWeight, 0.125, speedFade);

    // Mirrored around which source is the nearer one: at a phase below 0.5 the
    // previous frame leads, so the far weight belongs to the current frame.
    const float trustedWeight = (nearestSource > 0.5) ? (1.0 - farFrameWeight)
                                                      : farFrameWeight;
    // WHETHER TO BLEND is judged more strictly than whether to interpolate.
    //
    // Two questions that were sharing one number. "Is this vector usable at
    // all" should be lenient - being strict there discarded most of every
    // generated frame and made the output feel like half the frame rate.
    // "Should these two samples be AVERAGED" is a different question, and
    // averaging two samples that disagree is precisely what a double image is.
    //
    // They disagree wherever one block covers content at two depths: in a
    // low-altitude pass a near tree crosses the screen at 130 px a frame
    // while the ground behind it moves 20, and one vector per block cannot be
    // right for both. The search is not at fault - saturated blocks measure
    // 0%, so it finds what it looks for - the block simply contains two
    // motions.
    //
    // Where that happens the answer is not to blend more carefully but to
    // stop blending: take the temporally nearer sample alone, still motion
    // compensated. One slightly wrong picture beats two overlaid.
    // 12.0: blending stops at roughly an 8% disagreement between the two
    // samples. Averaging two samples that disagree is what a double image is.
    const float blendTrust = saturate(1.0 - mismatch * 12.0);
    float sourceWeight = lerp(nearestSource, trustedWeight, min(confidence, blendTrust));
    float3 blendedLinear = lerp(prevLinear, currLinear, sourceWeight);
    float3 fallbackLinear = SrgbToLinear(safeFallback.rgb);

    // Interpolation is systematically softer than a real frame: both source
    // samples are read with bilinear filtering at non-integer positions, and
    // then the two are mixed. On its own that reads as slight blur - but two
    // generated frames alternating with one untouched real frame make the
    // sharpness pulse at the real frame rate, which is what makes generated
    // frames identifiable at a glance.
    //
    // A mild unsharp mask compensates for that specific loss. It adds no
    // detail that is not already in the source frames - it restores local
    // contrast the resampling removed, using the same motion-compensated
    // neighbourhood the pixel itself came from.
    float3 blurLinear = float3(0.0, 0.0, 0.0);
    {
        const float2 offsets[4] = {
            float2(-1.0, 0.0), float2(1.0, 0.0), float2(0.0, -1.0), float2(0.0, 1.0)
        };
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float3 p = SrgbToLinear(PrevFrame.SampleLevel(LinearClamp, (prevSamplePos + offsets[i]) / dims, 0).rgb);
            float3 c = SrgbToLinear(CurrFrame.SampleLevel(LinearClamp, (currSamplePos + offsets[i]) / dims, 0).rgb);
            blurLinear += lerp(p, c, sourceWeight);
        }
        blurLinear *= 0.25;
    }
    // Deliberately gentle: enough to match a real frame's perceived
    // sharpness, not enough to ring on edges.
    // 0.15, down from 0.35, now that the two samples are reconstructed with
    // Catmull-Rom instead of bilinear. That already keeps the detail this was
    // compensating for, and the tester suspected slight over-sharpening after
    // the change - two sharpeners stacked on the same image.
    static const float kSharpenAmount = 0.15;
    float3 sharpenedLinear = max(blendedLinear + (blendedLinear - blurLinear) * kSharpenAmount, 0.0);

    float3 warpedLinear = lerp(fallbackLinear, sharpenedLinear, confidence);

    // THE OUTER GATE: how much of the displacement this pixel is allowed to
    // keep, decided at the top of CSMain from the vector rather than from its
    // result.
    //
    // Note what this is NOT mixed with. The confidence fallback above replaces
    // a pixel with an UNWARPED SINGLE frame - sharp, but frozen for the
    // duration of the generated frame, which is why it may only ever cover
    // small patches. This one replaces it with both frames cross-faded: softer,
    // but it moves, because the two sources are half a frame apart in content.
    // Over a large area - a fast turn, a low pass over foliage - a soft moving
    // region is the lesser evil, and a frozen one would read as the picture
    // sticking.
    //
    // Straight, honest statement of the cost: at 130 px of real displacement
    // this cross-fade IS a double image, just a symmetric and low-contrast one
    // instead of a hard displaced copy. It is the right answer only while it
    // stays local. "showblend" exists to check that it does.
    if (vectorTrust < 1.0)
    {
        const float3 pLin = SrgbToLinear(PrevFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0).rgb);
        const float3 cLin = SrgbToLinear(CurrFrame.SampleLevel(LinearClamp, pixelCenter / dims, 0).rgb);
        warpedLinear = lerp(lerp(pLin, cLin, PhaseT), warpedLinear, vectorTrust);
    }

    float4 result;
    result.rgb = LinearToSrgb(warpedLinear);

    // Diagnostic: the amount of blue IS the amount of displacement given up.
    if (DebugTintGenerated == 5 && vectorTrust < 1.0)
        result.rgb = lerp(float3(0.1, 0.3, 1.0), result.rgb, vectorTrust);
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
    // THE BADGE: a green bar in the top-left corner while the booster is
    // actually doubling.
    //
    // It is honest by construction rather than by care. This shader runs only
    // when a generated frame is being produced, so the bar cannot be drawn
    // while nothing is being generated - and when the engine stands aside for
    // lack of GPU room it hides its overlay entirely, so the bar disappears
    // with it. A status light that is wired to the thing it reports, not to a
    // variable that says what the thing is supposed to be doing.
    //
    // Bit 2 of StatusFlags. Bits 0 and 1 stay the small amber and cyan squares
    // beside it.
    if ((StatusFlags & 4u) != 0)
    {
        const int kBarWidth = 64, kBarHeight = 6, kBarMargin = 12;
        const int2 b = int2(id.xy) - int2(kBarMargin, kBarMargin + 20);
        if (b.x >= 0 && b.x < kBarWidth && b.y >= 0 && b.y < kBarHeight)
        {
            // Drawn over whatever is underneath rather than blended, so it
            // reads the same on a bright sky and in a dark corridor.
            result.rgb = float3(0.1, 0.95, 0.3);
        }
    }

    // Status squares: 14x14 px each, 4 px apart, starting 12 px from the
    // top-left corner. Amber = low latency, cyan = transparency. Drawn only
    // into generated frames, which is all we produce - enough to read at a
    // glance without covering anything that matters.
    if (StatusFlags != 0)
    {
        const int kSize = 14, kGap = 4, kMargin = 12;
        int2 p = int2(id.xy) - int2(kMargin, kMargin);
        if (p.y >= 0 && p.y < kSize && p.x >= 0)
        {
            int slot = p.x / (kSize + kGap);
            int withinSlot = p.x % (kSize + kGap);
            if (withinSlot < kSize && slot < 2 && (StatusFlags & (1u << (uint)slot)) != 0)
            {
                result.rgb = (slot == 0) ? float3(1.0, 0.65, 0.0)   // amber: low latency
                                         : float3(0.0, 0.8, 1.0);   // cyan: transparency
            }
        }
    }

    // 1 = paint every generated frame red, so it is obvious which frames are
    // ours. 2 = paint only the pixels the occlusion test distrusts, so it is
    // obvious WHERE it fires: the trail behind a moving object either lights up
    // green - the detector sees it and the fallback is too weak - or it does
    // not, and no amount of tuning the constant will help. Guessing between
    // those two has already cost several rounds.
    if (DebugTintGenerated == 1)
        result.rgb = lerp(result.rgb, float3(1.0, 0.0, 0.0), 0.45);
    else if (DebugTintGenerated == 2 && occlusionConfidence < 0.5)
        result.rgb = lerp(result.rgb, float3(0.0, 1.0, 0.0), 0.8);
    // Mode 4: show the MOTION FIELD itself instead of the picture.
    //
    // Four hypotheses about the trail have now been tested and all four were
    // wrong - the confidence fallback, the occlusion cross-check, the temporal
    // carry-over and the spatial averaging. Every one of them judged the
    // OUTPUT. None of them looked at what the field says where the trail is,
    // which is the input that everything else is derived from.
    //
    // Still content must read black. If the ground a bot has just crossed
    // glows while the rest of the still scene stays dark, the field claims
    // motion where there is none and the trail is made in the estimator. If
    // that ground is black, the field is right and the trail is made after it
    // - in the warp or the blend - and no amount of work on the estimator
    // will touch it.
    //
    // Green is horizontal speed, red vertical, blue the total, each saturating
    // at 20 px so slow motion is still clearly visible.
    else if (DebugTintGenerated == 4)
    {
        const float2 m = mv;
        const float speed = length(m);
        result.rgb = float3(saturate(abs(m.y) / 20.0),
                            saturate(abs(m.x) / 20.0),
                            saturate(speed / 20.0));
    }

    GeneratedFrame[id.xy] = result;
}
