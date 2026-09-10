# RESET FRAMEBOOST BETA - state, measurements, next step

Experimental system-level frame generation. Isolated from the Stable build:
everything here compiles only under the `Beta` solution configuration
(`RFB_BETA`), and Stable has no reference to it at all.

## How it works

Windows Graphics Capture -> three-level pyramid block-matching motion
estimation -> motion-compensated interpolation in linear light -> our own
DirectComposition flip-model swapchain. No injection, no game memory access,
no kernel driver, no anti-cheat interaction: the target application is only
ever read from the image Windows has already composited.

Output is **time-driven**. Every refresh interval is one output slot, and each
slot asks what the content should look like at that instant:

```
phase = (now - arrival_of_newer_frame) / (t_newer - t_older)
```

clamped to [0, 1]. There is no "factor" any more - the source rate does not
have to divide the refresh rate.

## Measured, browser video (2560x1440 @ 144 Hz, Opera, YouTube)

| Metric | Value |
| --- | --- |
| Native FPS captured | ~48 (the browser locks to 144/3) |
| Generated FPS | ~96 |
| Output FPS | **144.0** |
| Output interval | 6.94 ms, jitter 0.01 ms, 0% missed slots |
| On-screen age (end to end) | 6.6 ms average, 13.4 ms worst |
| Motion search saturation | 0.0% |
| Duplicates discarded | 0 |

## Measured, game (Delta Force, shooting range)

Reaches 144.0 output FPS with 0.0% empty slots and 6.94 ms intervals - but
**the user reports it feels like ~35 FPS**, and that is not a measurement
error. See the open problem below.

## Hotkeys

**F6** low-latency cap (legacy, no longer affects pacing) · **F7** transparency
· **F8** refresh lock · **F9** generation on/off · **F11** tint generated frames

Modes are shown as small squares in the top-left corner of generated frames:
amber = low latency, cyan = transparency.

## THE OPEN PROBLEM - read this first

In a demanding game the captured frames do not arrive at a steady rate:

```
measured interval: 20.83 ms -> 27.18 ms -> 27.01 ms -> 34.70 ms
                   (48 Hz)     (37 Hz)     (37 Hz)     (29 Hz)
```

The interval between two captured frames is replayed *uniformly* across the
slots it spans. When that interval swings between 20.8 and 34.7 ms, the
apparent speed of motion swings with it, several times a second. Interpolation
cannot smooth an irregular input - it converts the irregularity into varying
motion speed and makes it MORE visible. This is why perfectly paced 144 output
still feels like ~35.

Ruled out by measurement, so do not re-investigate:

- Output pacing (6.94 ms, 0.01 ms jitter, 0% missed slots)
- Frames not reaching the panel (DXGI: displayed == submitted)
- Capture dropping frames (raised the pool 2 -> 6 and polled every slot:
  0-8 extra frames per second, so the surplus does not exist)
- Generated frames too dark (fixed: blending is in linear light)
- Search saturation (fixed: three-level pyramid, 0.0%)
- Transparency mixing live and delayed frames (turning it off changed nothing)
- In-game V-Sync (changed nothing)

### Next step agreed with the user

**Detect an irregular source and disable generation while it lasts.** Clean
passthrough at 50 FPS beats wobbling 144. Same principle as the adaptive
factor - generate nothing where there is nothing to gain - applied to the
regularity of the signal rather than its rate.

Suggested shape: track the variance of the measured real-frame interval; when
the spread exceeds roughly a quarter of the mean for a sustained period, fall
back to passthrough and log it; resume when it settles. Hysteresis is required,
as with the factor switch.

### The ceiling behind it

A game reporting 75 FPS internally delivers ~50 to Windows Graphics Capture,
at irregular intervals. The compositor simply does not build a desktop frame
for every present the game makes, and an external capture can only ever see
what it builds. This is why DLSS 3 and FSR 3 run inside the game process.
That path is deliberately not taken here: it would mean injecting into games,
which the project rules exclude, and which risks anti-cheat bans.

For video and steadier content the approach measures very well. For a
demanding game it hits a limit that is not in this code.

## Other findings worth keeping

- **Whenever our output covers the source, Windows stops compositing the
  source and the capture starves.** Fixed for monitor mode by leaving a single
  pixel column uncovered: duplicates went 32-35/s -> 0, gaps 485 ms -> 7 ms.
- **A layered window cannot host a flip-model swapchain**, and DXGI refuses to
  report frame statistics for the BitBlt path at all. The presenter is
  therefore WS_EX_NOREDIRECTIONBITMAP + DirectComposition - but WS_EX_LAYERED
  still has to be set, because mouse pass-through genuinely requires LAYERED
  together with TRANSPARENT. Both work together; verified with WindowFromPoint.
- **Blend in linear light.** Averaging gamma-encoded values does not give the
  average brightness (the midpoint of 0 and 255 encodes ~22% of the light),
  so every pixel where the two sources differed came out too dark.
- **Weight the blend by agreement, not by temporal position.** At phase 1/3
  the previous frame is sampled 2/3 of the way along the motion - the larger
  displacement, the higher error risk - and weighting it 2/3 amplified exactly
  the least reliable sample.
- **Fall back to the temporally NEARER real frame** where the vector cannot be
  trusted. Always falling back to the current frame made low-confidence pixels
  jump forward and back at the asymmetric phases.
- **A coarse search only works on a downsampled image.** Sampling a coarse
  candidate grid on the full-resolution frame let blocks false-match distant
  detail: mean motion 6.2 -> 37.5 px, saturation 6.6% -> 32.4%. Reverted.
- **Duplicate detection must cover the whole frame.** Sparse patches touched
  0.17% of the screen and missed a playing video entirely. A GenerateMips
  thumbnail compared in tiles covers 100%. Its threshold had to drop from 1.2
  to 0.3, because a thumbnail texel averages a 64x64 block and shrinks
  differences by roughly that factor.
- **Three measurement bugs cost real time.** Each was caught by an impossible
  number, and each would have led to optimising something that was not broken:
  a phantom gap per pair (192 intervals/s against 144 frames), reading
  PresentRefreshCount as frames displayed (a stalled output looked like a
  perfect 144), and double-counting present time (a 9.5 ms present inside a
  7.8 ms iteration). Check that parts are smaller than wholes.

## Known issue, deliberately deferred

Generated frames are gently sharpened to match a real frame's perceived
sharpness, since interpolation is systematically softer. If edges ever look
overdrawn, `kSharpenAmount` in `frame_interpolation.hlsl` is the dial.
