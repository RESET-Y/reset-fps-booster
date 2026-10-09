# Product

<!-- impeccable:product-schema 1 -->

## Platform

windows-desktop (WPF, .NET 8). This is a native Windows app; none of the schema values web, ios, android or adaptive fit it, so the iOS and Android references do not apply.

## Users
Windows gamers who want more FPS from their PC. They open RFB before a game session, check what the machine is, then switch on the features they need.

## Product Purpose
RESET FPS BOOSTER (RFB) optimizes Windows gaming performance: FrameBoost (generated frames), Smooth Motion (motion smoothing), a crosshair overlay, and game-related tuning. Success means the user trusts the tool, sees real numbers from their own PC, and gets into the features fast.

## Positioning
RFB reads the real hardware of the machine it runs on and shows exactly that, never invented specs. It runs in user mode and never touches the game's memory or files.

## Operating Context
Started on the user's Windows PC, usually shortly before playing. The start-up screen is seen on every launch.

## Capabilities and Constraints
- Hardware read via WMI (CPU, GPU name, RAM and speed, monitor refresh rate, Windows build). WMI reports GPU memory capped at 4 GB, so VRAM is not shown.
- The start-up screen (SplashWindow) runs at most 5 seconds before the dashboard appears.
- It can be skipped with a click or a key press.
- If the hardware read fails or is slow, the screen still goes on; it never blocks the app from starting.
- The intro video has been removed on purpose; there is no video.
- Written in plain German for the UI texts.

## Brand Commitments
- Palette is red, black and white, made binding by the user on 2026-10-09 for the intro and the app: accent red #E8121F (bright variant #FF3242), black and near-black grounds, white for type and light. No blue or other hues in the default look. (Users may still pick another accent colour in the app settings; it replaces the red.)
- Logo: assets/logo_wide.png.
- Typefaces in use: Bahnschrift (Windows) and Consolas for labels. Observed in the code, not a confirmed brand decision.

## Evidence on Hand
- Real hardware data from src/ResetFpsBooster/Hardware/HardwareService.cs.
- Logos in src/ResetFpsBooster/Assets/.
- Nothing invented: no testimonials, benchmarks or customer claims for the start-up screen.

## Product Principles
- Show only real values from the user's machine.
- Get the user into the app quickly.
- Never block the start; a failed read is shown honestly, not hidden.
- Do not claim what the tool does not do (no cheat features, no game file access).
