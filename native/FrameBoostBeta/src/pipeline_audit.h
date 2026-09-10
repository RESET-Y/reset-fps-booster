#pragma once
#include <windows.h>
#include <d3d11.h>

namespace FrameBoostBeta {

// Measures where frames are lost between the game and the screen, and does
// nothing else - no motion estimation, no interpolation, no overlay. The
// engine's normal telemetry can only count what it managed to read, so a
// frame the capture pool recycled before we reached it looked exactly like a
// frame the game never rendered. Those two have opposite fixes, so the audit
// counts every stage separately:
//
//   produced   frames Windows Graphics Capture announced (FrameArrived)
//   retrieved  frames we actually pulled out of the frame pool
//   lost       produced - retrieved: recycled before we read them
//   stale      retrieved but thrown away to reach a newer one
//   unique     frames whose content differs from the one before
//   duplicate  frames identical to the one before (the compositor
//              republishing an unchanged screen)
//
// plus interval statistics at two points: when WGC announced the frames, and
// what WGC's own capture timestamps say about their spacing.
//
// seconds: how long to measure. Logs one line per second plus a summary.
void RunCaptureAudit(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context, int seconds);

// The same question asked through the other supported capture path: DXGI
// Desktop Duplication. It reports two things Windows Graphics Capture does
// not, and both are exactly what is in dispute here - AccumulatedFrames says
// how many desktop updates were coalesced into the one we were handed (i.e.
// how many we missed by reading too slowly), and LastPresentTime says when
// the frame in front of us was actually presented, on the same clock.
//
// Running both against the same game in the same minute is what settles
// whether ~50 updates per second is the compositor's real output or an
// artefact of how we read it.
void RunDesktopDuplicationAudit(HMONITOR monitor, ID3D11Device* device, int seconds);

// Does WDA_EXCLUDEFROMCAPTURE hide a window from Desktop Duplication, the
// way it does from Windows Graphics Capture?
//
// The whole DD plan depends on the answer. Our output is a fullscreen
// overlay sitting on the monitor we capture; if DD sees it, the engine
// captures its own last frame, interpolates that, displays it, captures it
// again - a feedback loop, not a frame generator. WGC solves this with
// SetWindowDisplayAffinity, and it would be reasonable to assume DD honours
// the same flag. Reasonable is not measured.
//
// Puts a known solid colour over the whole monitor with the exclusion flag
// set, takes one DD frame, and reads the pixels back: our colour means DD
// captured the overlay, anything else means it was excluded.
void RunSelfCaptureTest(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context);

} // namespace FrameBoostBeta
