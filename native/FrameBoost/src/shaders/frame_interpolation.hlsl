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
static const float kMismatchSensitivity = 14.0;

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
static const float kBlockErrorSensitivity = 30.0;

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
    float3 motionAndError = SampleMotionBilinear(pixelCenter, blockCount);
    float2 mv = motionAndError.xy;
    float blockMatchError = motionAndError.z;

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
    const float3 motionAtSource = SampleMotionBilinear(pixelCenter + mv, blockCount);
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
        GeneratedFrame[id.xy] = outColor;
        return;
    }

    float2 prevSamplePos = pixelCenter + (1.0 - PhaseT) * mv;
    float2 currSamplePos = pixelCenter - PhaseT * mv;

    float4 prevColor = PrevFrame.SampleLevel(LinearClamp, prevSamplePos / dims, 0);
    float4 currColor = CurrFrame.SampleLevel(LinearClamp, currSamplePos / dims, 0);

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
    // Mirrored around which source is the nearer one: at a phase below 0.5 the
    // previous frame leads, so the far weight belongs to the current frame.
    const float trustedWeight = (nearestSource > 0.5) ? (1.0 - kFarFrameWeight)
                                                      : kFarFrameWeight;
    float sourceWeight = lerp(nearestSource, trustedWeight, confidence);
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
            // Same weighting as the pixel itself, so the sharpening compares
            // like with like - a blur built with different weights would push
            // the result toward the other source instead of just restoring
            // local contrast.
            blurLinear += lerp(p, c, sourceWeight);
        }
        blurLinear *= 0.25;
    }
    // Deliberately gentle: enough to match a real frame's perceived
    // sharpness, not enough to ring on edges.
    static const float kSharpenAmount = 0.35;
    float3 sharpenedLinear = max(blendedLinear + (blendedLinear - blurLinear) * kSharpenAmount, 0.0);

    float4 result;
    result.rgb = LinearToSrgb(lerp(fallbackLinear, sharpenedLinear, confidence));
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

    GeneratedFrame[id.xy] = result;
}
