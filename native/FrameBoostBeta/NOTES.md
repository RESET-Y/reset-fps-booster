# RESET FRAMEBOOST BETA - state, measurements, next step

Experimental system-level frame generation. Isolated from the Stable build:
everything here compiles only under the `Beta` solution configuration
(`RFB_BETA`), and Stable has no reference to it at all.

## What works today

Real frame generation via Windows Graphics Capture -> block-matching motion
estimation -> motion-compensated interpolation -> own swapchain. No injection,
no game memory access, no kernel driver, no anti-cheat interaction. The target
application is only ever read from its already-composited image.

Measured live on a 2560x1440 @ 144 Hz display, Opera playing a YouTube video:

| Metric | Value |
| --- | --- |
| Native FPS (real, captured) | ~42-50 |
| Generated FPS | ~30 |
| Output FPS | 72.00 (refresh-locked) |
| Capture latency | ~1.4 ms |
| Motion estimation (GPU) | 0.083 ms |
| Interpolation (GPU) | 0.150 ms |
| CPU per iteration | 13.91 ms (of which 6.6 ms is the paced wait) |

## Modes

| Argument | Behaviour |
| --- | --- |
| *(none)* | Click-through overlay tracking a single window |
| `monitor` | Capture and display the whole monitor the target sits on |
| `monitor2` | Capture one monitor, display on the other - nothing is covered |

Hotkeys: **F8** refresh lock, **F9** generation on/off, **F10** vsync,
**F11** tint generated frames red.

## Findings that cost real time to establish

- **Whenever our output covers the source, Windows stops compositing the
  source and the capture starves.** Proven in three separate configurations
  (window overlay, fullscreen monitor overlay, and covered browser). This is
  why DLSS/FSR3 run in-process rather than as an external overlay.
- **A layered window cannot host a flip-model swapchain.** Click-through
  needs `WS_EX_LAYERED`, which forces the legacy BitBlt swap effect.
- **`WS_EX_TRANSPARENT` alone does not give click-through**; it needs
  `WS_EX_LAYERED` as well, plus `WM_NCHITTEST` -> `HTTRANSPARENT`.
- **An opaque overlay triggers occlusion detection** in Chromium browsers and
  in games, which then stop rendering. Alpha 254 avoids being counted as an
  occluder; a 1px inset does the same job for window mode.
- **Duplicate detection needs coverage, not resolution.** 12 patches covered
  0.08% of the screen and missed a playing video entirely.
- **Never skip the present on a duplicate frame.** Skipping generation is
  correct; skipping the present makes the output look frozen and
  indistinguishable from a crash.
- **Refresh-locked output made no visible difference** in the live A/B test
  (F8), even though the cadence is exact to 0.02 ms. The judder the user
  perceives therefore does not come from uneven output spacing - so the
  remaining suspects are interpolation quality and added latency.

## The remaining judder cause, and the fix that is still owed

Judder during motion is the one open complaint, and everything else has been
ruled out by measurement:

| Suspect | Measured | Verdict |
| --- | --- | --- |
| Frame rate | 144.0 output, 96 generated | not it |
| Frame pacing | 6.94 ms interval, 0.01 ms jitter, 0% missed | not it |
| Frames reaching the panel | displayed == submitted (DXGI) | not it |
| Generated frames too dark | fixed by blending in linear light | fixed |
| **Wrong motion vectors** | **18-23% of moving blocks on the search edge** | **this one** |

Roughly one moving block in five has a vector that is wrong by construction:
the true match lies outside the +-12 px search window, so the block gets the
closest wrong answer. That is ~2600 blocks per frame showing content in the
wrong place, concentrated in fast scenes - which is exactly where the eye is
looking.

The fix is a real pyramid search: build mip levels of both source frames,
search coarsely at quarter resolution (where the same radius reaches 48 px
AND the fine detail is averaged away), then refine at full resolution.

**A shortcut was tried and failed - do not repeat it.** Sampling a 4-pixel
coarse grid on the FULL-resolution frame, without downsampling, let 16px
blocks match distant repeating detail (text, noise) better than their true
small motion. Measured: mean motion jumped 6.2 -> 37.5 px and saturation
6.6% -> 32.4%, i.e. the field filled with false matches, and output fell to
83-120 FPS. Reverted. The downsampling is not an optimisation in a pyramid
search, it is the part that makes the coarse stage valid at all.

## Earlier step (done - kept for the reasoning)

Adaptive generation factor instead of a fixed 2x.

Measure the incoming real frame rate, compare it against the display refresh
rate, and generate exactly as many intermediate frames as are missing:

| Native FPS (144 Hz display) | Factor | Output |
| --- | --- | --- |
| 144+ | none | native, untouched |
| 72 | 2x | 144 |
| 60 | 2x | 120 |
| 48 | 3x | 144 |
| 30 | 4x | 120 |

The first row matters most: when the source already saturates the display,
generation disables itself. No quality loss, no added latency, no GPU cost -
exactly where there was nothing to gain anyway. It is also the honest
behaviour: never manufacture frames that are not missing.

Only real change needed: `frame_interpolation.hlsl` currently interpolates
the fixed midpoint. Factors above 2x need a time parameter t so it can
produce frames at 1/3, 2/3 etc. instead of only 1/2 - roughly ten lines,
not a rewrite. Everything else is bookkeeping in the pacing loop.

Hysteresis is required around the switch points, otherwise a source hovering
near a threshold will flip factors every second and that change is itself
visible.

## Next step after that (the promising but risky one)

Show the generated frames ONLY, and let the real desktop show through
untouched in between.

Today the viewer never sees their real screen - every frame is our captured,
copied and rescaled version of it, which is the source of the visible quality
loss. If the overlay were fully transparent during the real-frame slots, the
real image would be seen at native quality and we would contribute only the
frames that would not otherwise exist. This is how in-process frame
generation effectively behaves.

Requirements:

- Per-frame alpha of 0% or 100%, never in between. A partially transparent
  generated frame blends with the real frame underneath, which is a
  crossfade - explicitly out of scope.
- Needs `WS_EX_NOREDIRECTIONBITMAP` + DirectComposition with a premultiplied-
  alpha flip-model swapchain. The current layered window only supports one
  uniform alpha for the whole window.

Main risk: the transparent/opaque alternation has to land between the
desktop's own composited frames. If it does not, the result is flicker rather
than smoothness. Unproven - estimate roughly 50/50, and it has to be measured
live rather than argued about.

## Known issue, deliberately deferred

Generated frames lose contrast / appear darker than real ones. Documented in
`../FrameBoost/README.md`; to be fixed together with the other quality passes
at the end.
