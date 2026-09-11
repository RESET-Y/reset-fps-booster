#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace FrameBoostBeta {

// Reads the raw mouse stream, so the engine knows where the camera is being
// turned before the game has drawn the frame that shows it.
//
// Why this is worth having: a generated frame is currently a guess made from
// two frames that are already in the past. It cannot know that the player has
// just flicked right, because that movement has not reached any rendered frame
// yet. Mouse input is available a whole frame earlier than the picture it
// eventually produces, and in a shooter the mouse IS the camera - so the
// dominant motion in the next frame is knowable in advance.
//
// This is what VR headsets call reprojection: take the last finished frame and
// shift it by the newest head movement so the display follows the player even
// when no new frame is ready. The mouse is the head here.
//
// On the constraints this project works under: this reads OUR OWN raw input
// stream through a documented Windows API, on our own window. It does not hook
// the game, read its memory, or intercept anything on its way to the game -
// the game receives exactly what it would receive without us. It is the same
// mechanism any application uses to read a mouse.
//
// RIDEV_INPUTSINK is the part that matters: it delivers input even while the
// game holds the foreground, which is the only situation this is for.
class MouseTracker {
public:
    // hwnd: any window of ours; raw input is delivered to it.
    bool Start(HWND hwnd);
    void Stop();

    // Called from the window procedure for WM_INPUT.
    void OnRawInput(LPARAM lParam);

    // Accumulated mouse counts since the last call, and clears them - so each
    // caller sees the movement that happened during its own frame.
    void TakeDelta(double& outDx, double& outDy);

    // Totals, for telemetry that should not disturb the per-frame accounting.
    double TotalAbsX() const { return m_totalAbsX.load(std::memory_order_relaxed); }
    double TotalAbsY() const { return m_totalAbsY.load(std::memory_order_relaxed); }
    uint64_t EventCount() const { return m_events.load(std::memory_order_relaxed); }

    bool IsRunning() const { return m_running; }

private:
    // Signed accumulators. Raw mouse deltas arrive as integers in device
    // counts, which are not pixels and not degrees - the conversion to screen
    // motion depends on the game's sensitivity and field of view, and is
    // measured rather than assumed. See the calibration in main.cpp.
    std::atomic<double> m_accumX{ 0.0 };
    std::atomic<double> m_accumY{ 0.0 };
    std::atomic<double> m_totalAbsX{ 0.0 };
    std::atomic<double> m_totalAbsY{ 0.0 };
    std::atomic<uint64_t> m_events{ 0 };
    bool m_running = false;
};

} // namespace FrameBoostBeta
