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

## Next step (the promising one)

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
