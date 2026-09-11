#include "mouse_tracker.h"
#include "logger.h"

#include <sstream>
#include <vector>

namespace FrameBoostBeta {

bool MouseTracker::Start(HWND hwnd) {
    if (!hwnd) return false;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // generic desktop
    rid.usUsage = 0x02;     // mouse
    // INPUTSINK: deliver even when the game has the foreground, which is the
    // only case this exists for. NOLEGACY is deliberately NOT set - that would
    // suppress ordinary mouse messages process-wide, and this must change
    // nothing about how input behaves anywhere else.
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        std::ostringstream oss;
        oss << "[FrameBoostBeta] Raw mouse input unavailable (error " << GetLastError()
            << ") - camera prediction will be off.";
        Logger::Log(oss.str());
        return false;
    }

    m_running = true;
    Logger::Log("[FrameBoostBeta] Raw mouse input registered. This reads our own input stream through the"
                " documented Windows API; the game receives exactly what it would without us.");
    return true;
}

void MouseTracker::Stop() {
    if (!m_running) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    m_running = false;
}

void MouseTracker::OnRawInput(LPARAM lParam) {
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0)
        return;
    if (size == 0 || size > 1024) return;

    BYTE buffer[1024];
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;

    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw->header.dwType != RIM_TYPEMOUSE) return;

    // MOUSE_MOVE_ABSOLUTE is what tablets and some remote-desktop setups send:
    // the values are then screen coordinates rather than movement, and adding
    // them would be meaningless. Games use relative motion, and that is the
    // only case this is for.
    if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return;

    const double dx = static_cast<double>(raw->data.mouse.lLastX);
    const double dy = static_cast<double>(raw->data.mouse.lLastY);
    if (dx == 0.0 && dy == 0.0) return;

    // Plain loads and stores rather than fetch_add: doubles have no atomic
    // add, and the only writer is this handler, on the thread that owns the
    // window. The reader takes whatever has accumulated so far, and a value
    // read half a frame late costs nothing here.
    m_accumX.store(m_accumX.load(std::memory_order_relaxed) + dx, std::memory_order_relaxed);
    m_accumY.store(m_accumY.load(std::memory_order_relaxed) + dy, std::memory_order_relaxed);
    m_totalAbsX.store(m_totalAbsX.load(std::memory_order_relaxed) + (dx < 0 ? -dx : dx),
                      std::memory_order_relaxed);
    m_totalAbsY.store(m_totalAbsY.load(std::memory_order_relaxed) + (dy < 0 ? -dy : dy),
                      std::memory_order_relaxed);
    m_events.fetch_add(1, std::memory_order_relaxed);
}

void MouseTracker::TakeDelta(double& outDx, double& outDy) {
    outDx = m_accumX.exchange(0.0, std::memory_order_relaxed);
    outDy = m_accumY.exchange(0.0, std::memory_order_relaxed);
}

} // namespace FrameBoostBeta
