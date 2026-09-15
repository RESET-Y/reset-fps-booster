# RESET FRAMEBOOST — native capture layer (v0.1, Milestone 1)

Real, GPU-side DirectX 11 frame capture proof of concept. Target: **Watch Dogs
(2014), DirectX 11 only.** No other games, no DX12/Vulkan, no AI upscaling —
see the project scope discussion in the main conversation for why.

## How it works

`d3d11.dll` built from this project is a **proxy DLL**. Deployed next to
`Watch_Dogs.exe`, it is loaded by Windows' normal DLL search order (app
directory before `System32`) instead of the real system `d3d11.dll` — no
process injection, no kernel driver, no modification of any game file. The
same technique overlay/capture tools like ReShade use.

The proxy:
1. Loads the *real* `System32\d3d11.dll` itself (by full path, so it can
   never recursively load itself).
2. Forwards `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` straight
   through — the game renders exactly as it always did.
3. The moment a real swapchain exists, patches `IDXGISwapChain::Present`'s
   vtable entry to point at our own function first.
4. Our `Present` detour observes the **real, already-rendered** back buffer
   (Milestone 1: proven by logging its real width/height/format on the first
   frame), measures real native FPS from actual `Present` call timing, then
   calls the original `Present` so the game displays normally.

Nothing here fabricates a frame, a number, or a capability that isn't
actually happening.

## Status

- [x] Milestone 1 — proxy loads, forwards device/swapchain creation, hooks
      `Present`, proves back-buffer access via log output.
- [ ] Milestone 2 — captured frames displayed/validated (needs a way to view
      the captured texture — likely a debug overlay or dumped-to-disk PNG).
- [ ] Milestone 3+ — motion estimation, interpolation, presentation of a
      generated frame, RESET UI integration. Not started.

## Known issues to fix in a final polish pass

- **Generated frame is visibly darker / lower-contrast than real frames**
  (confirmed via live A/B comparison on 2026-09-09). Root cause: the
  bidirectional blend `0.5*Prev + 0.5*Curr` loses contrast wherever the
  motion vector for that region is even slightly wrong, which is common
  with the current simple block-matching estimator. Needs either a better
  motion field (see below) or an occlusion/confidence-aware blend that
  doesn't just average when the two samples disagree a lot.
- Motion field is noisy on detailed/fast scenes even after the 3x3 smoothing
  pass (see git history around the "verbessere die Motion Estimation"
  request) - a hierarchical/pyramid search would likely help both this and
  the darkening issue above, since they share the same root cause (bad
  motion vectors).

## Known limitation (documented, not hidden)

Only `D3D11CreateDeviceAndSwapChain` is hooked for swapchain discovery. If
Watch Dogs instead creates its device via `D3D11CreateDevice` and then a
swapchain separately via `IDXGIFactory::CreateSwapChain`, this v0.1 will not
see it, and the log will show the proxy loaded but never report a capture
proof. If that turns out to be the case, the fix is to also hook
`IDXGIFactory::CreateSwapChain`, which needs its own vtable hook — straight-
forward, just not built yet because we don't know we need it until we test
against the real game.

## Build

Requires the Visual C++ desktop workload + Windows SDK (installed
separately — see main conversation). Then:

```
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/bin/Release/d3d11.dll` (or `build/bin/d3d11.dll` for a Ninja
generator).

## Testing against the real game

1. Copy the built `d3d11.dll` into the Watch Dogs install folder, next to
   `Watch_Dogs.exe` (found on this machine at
   `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Watch_Dogs`).
2. Launch the game normally.
3. Check `%LOCALAPPDATA%\ResetFpsBooster\Logs\frameboost_native.log` for:
   - `Proxy d3d11.dll loaded into host process.`
   - `Real D3D11 swapchain created - installing capture hook.`
   - `Present hook active - first real frame observed.`
   - `Milestone 1 capture proof: back buffer <W>x<H> format=<N>`
   - Repeated `Native FPS (real, measured): <value>` lines, once per second.
4. To remove: delete the copied `d3d11.dll`. The game's own file tree is
   never modified — this is purely an added file next to the executable.

If any of these log lines are missing, Milestone 1 has not actually been
proven yet on the real target — the next debugging step, not a success to
report.
