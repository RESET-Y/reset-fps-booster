// RESET FRAMEBOOST BETA - system-level engine (Windows Graphics Capture
// based). Launched by the RFB Beta UI with a target window handle as its
// only argument. Captures that window via the public WGC API (no injection,
// no game-memory access), runs the SAME motion estimation + interpolation
// GPU pipeline built for the Watch Dogs prototype, and displays the result
// in its own window with its own modern flip-model swapchain.
//
// Failsafe by construction: this process never touches the target
// application at all beyond reading a copy of its already-composited
// image via WGC. If anything here fails, this process logs it and exits -
// the target application is completely unaffected either way.
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <sstream>
#include <utility>
#include <string>
#include <vector>
#include <cmath>
#include <functional>

#include "logger.h"
#include "capture_engine.h"
#include "beta_presenter.h"
#include "duplicate_detector.h"
#include "motion_stats.h"
#include "frame_dump.h"
#include "../../FrameBoost/src/motion_estimation.h"
#include "../../FrameBoost/src/interpolation.h"
#include "pipeline_audit.h"
#include "desktop_duplication_capture.h"

namespace {

HWND ParseTargetWindow(int argc, wchar_t** argv) {
    if (argc < 2) return nullptr;
    uintptr_t value = wcstoull(argv[1], nullptr, 0); // accepts "0x..." or decimal
    return reinterpret_cast<HWND>(value);
}

// Finds a monitor other than the given one, so the boosted output can be
// shown without covering (and thereby stalling) the captured source.
// Measured the hard way tonight: every configuration where our output
// covered the source - window overlay AND fullscreen monitor overlay -
// made Windows stop compositing the source, which starved the capture.
BOOL CALLBACK PickOtherMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* out = reinterpret_cast<std::pair<HMONITOR, HMONITOR>*>(data);
    if (monitor != out->first && !out->second) out->second = monitor;
    return TRUE;
}

HMONITOR FindSecondaryMonitor(HMONITOR captured) {
    std::pair<HMONITOR, HMONITOR> data{ captured, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, PickOtherMonitor, reinterpret_cast<LPARAM>(&data));
    return data.second;
}

// The display's actual refresh rate, which is what output pacing has to be
// built on. Measured live: presenting ~80 evenly-intended frames per second
// to a 144 Hz panel means every frame is held for either one or two refresh
// intervals (6.94 ms / 13.89 ms) in an irregular pattern - visible judder,
// even though the FPS number looks good. Only an output rate that divides
// the refresh rate exactly gives every frame the same on-screen duration.
double MonitorRefreshHz(HMONITOR monitor) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            return static_cast<double>(dm.dmDisplayFrequency);
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return static_cast<double>(dm.dmDisplayFrequency);
    return 0.0; // unknown - callers fall back to unlocked pacing
}

// Hotkeys require CTRL+ALT, because they are read globally with
// GetAsyncKeyState and games bind the function keys themselves. Without the
// modifier, playing the game silently reconfigured the engine: a single
// 15-second session in Delta Force toggled the frame buffer twice, the
// refresh lock, the latency cap and the debug tint - and the "test" that
// followed was therefore of a random configuration, not of the fix.
//
// CTRL+ALT was still not enough. Measured again, from the app's own run:
// three combos arrived within two seconds of gameplay - F4 turned the simple
// 2x path off, F5 dropped the one-frame buffer, F6 raised the cap to 3x. With
// the buffer gone and this source measuring 17-25% interval deviation, the
// irregularity guard then disabled generation completely: 70 real frames per
// second passed straight through to a 144 Hz panel, and every one of them was
// held for either one refresh or two. That is the judder that came back, and
// nobody chose any of it.
//
// So the hotkeys are now off unless the engine is started with "hotkeys" on
// the command line. Started from the app, the tuned configuration is the only
// one that can be running - a switch, not a keyboard full of traps.
bool g_hotkeysEnabled = false;

bool HotkeyDown(int vk) {
    if (!g_hotkeysEnabled) return false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl && alt && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool CreateSharedDevice(winrt::com_ptr<ID3D11Device>& device, winrt::com_ptr<ID3D11DeviceContext>& context,
                        int gpuPriority) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // required for Windows Graphics Capture interop
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), &obtained, context.put());
    if (FAILED(hr)) return false;

    // Where our work sits in the GPU.s queue.
    //
    // Measured under a GPU-bound game: motion estimation reported 30-33 ms
    // where the same dispatch costs 0.50 ms on an idle GPU. That difference is
    // not our arithmetic - it is our commands waiting behind an entire frame of
    // the game.s. We do not need more of the GPU, we need to be scheduled
    // sooner, and this is the documented way to ask: IDXGIDevice::
    // SetGPUThreadPriority, -7 to +7, no hooking, no driver tricks, nothing
    // done to the game.
    //
    // It is still a trade, and an honest one: priority we take is priority the
    // game loses. Off by default until measurement shows what it costs the
    // source frame rate; "gpupriority=N" sets it.
    if (gpuPriority != 0) {
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
            const HRESULT pr = dxgiDevice->SetGPUThreadPriority(gpuPriority);
            FrameBoostBeta::Logger::Log(SUCCEEDED(pr)
                ? "[FrameBoostBeta] GPU thread priority set to " + std::to_string(gpuPriority)
                  + " (0 = normal, 7 = highest). Priority taken here is priority the game loses."
                : "[FrameBoostBeta] Could not set the GPU thread priority - running at normal.");
        }
    }
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Ask Windows for a 1 ms scheduler tick before anything else.
    //
    // Without this, Sleep(1) does not sleep 1 ms - it sleeps until the next
    // scheduler tick, which defaults to 15.6 ms. Both output waits pump the
    // capture between sleeps precisely so no frame is missed while waiting,
    // and that was silently defeated: the capture was only asked roughly once
    // every 15 ms, while the compositor presents every 6.9 ms.
    //
    // Measured with it missing: 51-59 updates per second arriving already
    // coalesced by the OS - a frame Windows had to merge because we had not
    // collected the previous one. That is why a game sitting exactly on its
    // 72 fps cap was read as a wandering 56-70, reported directly as the
    // booster showing a different number every second while the game did not
    // move off 72. Frames lost this way are lost unevenly, which is the one
    // thing the period lock cannot repair.
    //
    // System-wide and released on exit. The engine only runs while the user
    // has the booster switched on.
    const bool haveHighResTimer = (timeBeginPeriod(1) == TIMERR_NOERROR);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Arguments are read by keyword rather than by position, and a window
    // handle is optional. With no handle the engine boosts the whole primary
    // display - which is what the app's switch does, and what a player
    // actually wants: the screen, not one window they first have to pick out
    // of a list.
    std::vector<std::wstring> args;
    for (int i = 1; i < argc && argv; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    auto HasArg = [&](const wchar_t* keyword) {
        for (const auto& a : args)
            if (_wcsicmp(a.c_str(), keyword) == 0) return true;
        return false;
    };

    HWND targetWindow = nullptr;
    for (const auto& a : args) {
        wchar_t* end = nullptr;
        uintptr_t value = wcstoull(a.c_str(), &end, 0); // accepts "0x..." or decimal
        if (value && end && *end == L'\0') { targetWindow = reinterpret_cast<HWND>(value); break; }
    }

    g_hotkeysEnabled = HasArg(L"hotkeys");
    // "tint": paint every generated frame red, so it is unmistakable on screen
    // which frames are ours. The one test that settles whether generated frames
    // reach the display as distinct pictures at all.
    // "tint" paints generated frames red; "showocclusion" paints the pixels the
    // occlusion test distrusts green; "showfallback" paints the replacement
    // pixels magenta - which answers whether the region the detector marks is
    // the region the artefact actually occupies.
    const unsigned int debugTintMode = HasArg(L"showfallback") ? 3u
        : HasArg(L"showocclusion") ? 2u
        : HasArg(L"tint") ? 1u : 0u;
    // "dupcheck": keep comparing frames on the GPU even when the capture API
    // reports dirty rectangles, so the two can be compared against each other.
    const bool useDirtyRectsOnly = HasArg(L"dirtyonly");
    // Interpolation is the default again.
    //
    // Extrapolation removes the half-interval hold and with it 7.8 ms of
    // latency, and it was worth trying: the raw game felt more responsive than
    // the boosted output, and that hold was most of the difference. But the
    // hold was doing a second job nobody had asked it to do - running every
    // present off an even clock, which hid a source that measures 30-45%
    // deviation between frame intervals. Three attempts to replace that with a
    // smaller pacing buffer each made things worse by eye: a blocking wait that
    // halved the frame rate, a drifting clock that halved it again, and a
    // catch-up rule that turned the hitches into "ultra stutter".
    //
    // The smoothest configuration measured and judged so far is the
    // interpolating one, so that is what runs unless "extrapolate" is passed.
    const bool extrapolateMode = HasArg(L"extrapolate");

    FrameBoostBeta::Logger::Init();

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    // "gpupriority=N": -7 to 7, 0 = normal.
    int gpuPriority = 0;
    for (const auto& a : args) {
        if (a.rfind(L"gpupriority=", 0) == 0) {
            const int parsed = _wtoi(a.c_str() + 12);
            if (parsed >= -7 && parsed <= 7) gpuPriority = parsed;
        }
    }

    if (!CreateSharedDevice(device, context, gpuPriority)) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: could not create a BGRA-capable D3D11 device.");
        return 2;
    }

    // "monitor" as the second argument captures the whole monitor the target
    // window sits on, instead of the window itself. Window capture stalls
    // when the window is covered (Windows stops redrawing hidden windows);
    // a monitor is always composited, so this mode keeps receiving frames
    // even with our own output displayed on top of everything.
    // With no window handle there is nothing to capture BUT a monitor, so
    // whole-screen mode is also the default.
    bool monitorMode = HasArg(L"monitor") || !targetWindow;
    // "monitor2": capture the monitor the target sits on, but display the
    // boosted result on a DIFFERENT monitor. This is the only tested
    // configuration where nothing gets covered, so the source keeps
    // rendering at full speed and the capture never starves.
    // "measure": capture and time the source, and do nothing else - no motion
    // estimation, no interpolation, no presenting. A diagnostic mode, for
    // answering whether the engine's own GPU load is what makes a game's frame
    // delivery irregular. Everything the engine normally does competes with
    // the game and the compositor for the same GPU, so the only way to know
    // what the source looks like undisturbed is to stop disturbing it.
    bool measureOnlyMode = HasArg(L"measure");

    // "audit": measure the whole pipeline and exit. Both capture paths, every
    // stage counted separately, nothing generated and nothing displayed - so
    // the numbers describe the source and the capture, not what our own load
    // does to them.
    // "modes": list the refresh rates this display actually offers at its
    // current resolution, and exit. Which rates exist decides whether doubling
    // can ever land on the refresh grid: 60 doubled is 120, which divides a
    // 120 Hz panel exactly and a 144 Hz one not at all.
    if (HasArg(L"modes")) {
        DEVMODEW current{}; current.dmSize = sizeof(current);
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current);
        std::ostringstream oss;
        oss << "[FrameBoostBeta] Current mode: " << current.dmPelsWidth << "x" << current.dmPelsHeight
            << " @ " << current.dmDisplayFrequency << " Hz. Available at this resolution:";
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
            if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight)
                oss << " " << mode.dmDisplayFrequency;
        }
        FrameBoostBeta::Logger::Log(oss.str());
        return 0;
    }

    if (HasArg(L"audit")) {
        HMONITOR auditMonitor = targetWindow
            ? MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST)
            : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

        int auditSeconds = 20;
        for (const auto& a : args) {
            if (a.rfind(L"seconds=", 0) == 0) {
                const int parsed = _wtoi(a.c_str() + 8);
                if (parsed >= 3 && parsed <= 600) auditSeconds = parsed;
            }
        }

        FrameBoostBeta::Logger::Log("[Audit] Starting pipeline audit for " + std::to_string(auditSeconds)
            + " s per capture path. Nothing is generated or displayed during the audit.");
        if (HasArg(L"selftest")) {
            FrameBoostBeta::RunSelfCaptureTest(auditMonitor, device.get(), context.get());
            FrameBoostBeta::Logger::Log("[Audit] Pipeline audit complete (self-capture test only).");
            return 0;
        }

        FrameBoostBeta::RunCaptureAudit(auditMonitor, device.get(), context.get(), auditSeconds);
        FrameBoostBeta::RunDesktopDuplicationAudit(auditMonitor, device.get(), auditSeconds);
        FrameBoostBeta::Logger::Log("[Audit] Pipeline audit complete.");
        return 0;
    }

    bool secondScreenMode = HasArg(L"monitor2");
    if (secondScreenMode) monitorMode = true;
    // No handle: the primary display, which is the one being played on.
    HMONITOR targetMonitor = targetWindow
        ? MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR outputMonitor = targetMonitor;

    if (secondScreenMode) {
        HMONITOR other = FindSecondaryMonitor(targetMonitor);
        if (!other) {
            FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: second-screen mode requested but no other monitor was found.");
            return 5;
        }
        outputMonitor = other;
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] Second-screen mode: capturing one monitor, displaying on the other - nothing is covered.");
    }

    FrameBoostBeta::CaptureEngine capture;
    FrameBoostBeta::DesktopDuplicationCapture ddCapture;

    // Desktop Duplication is the default source for whole-screen capture, and
    // the reason is measured, not architectural taste. With Rocket League
    // reporting 90 FPS, on the same monitor within the same minute:
    //
    //   Windows Graphics Capture  44.60 frames/s produced, 0 lost in our pool
    //   Desktop Duplication       90.20 presents/s, spacing 11.09 ms, max 1 coalesced
    //
    // WGC was not being read too slowly - it announced 892 frames and we
    // retrieved all 892. It simply hands out about half of what the compositor
    // presents, and the interpolator can only work with what it is given.
    // "wgc" on the command line selects the old path for comparison.
    bool useDesktopDuplication = monitorMode && !HasArg(L"wgc");

    bool captureStarted = useDesktopDuplication
        ? ddCapture.StartMonitor(targetMonitor, device.get(), context.get())
        : (monitorMode ? capture.StartMonitor(targetMonitor, device.get())
                       : capture.Start(targetWindow, device.get()));

    // Desktop Duplication can be refused outright (another duplication client,
    // a secure desktop). Rather than fail, fall back to the path that has been
    // working all along - halved frame rate is still better than no boost.
    if (!captureStarted && useDesktopDuplication) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] Desktop Duplication could not start - falling back to"
            " Windows Graphics Capture (about half the source frame rate).");
        useDesktopDuplication = false;
        captureStarted = capture.StartMonitor(targetMonitor, device.get());
    }
    if (!captureStarted) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: capture failed to start - target window may be unsupported or closed. Falling back safely (no display, exiting).");
        return 3;
    }

    RECT targetRect{};
    if (targetWindow) {
        GetClientRect(targetWindow, &targetRect);
    } else {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(targetMonitor, &mi)) targetRect = mi.rcMonitor;
    }
    UINT initialWidth = static_cast<UINT>(targetRect.right - targetRect.left);
    UINT initialHeight = static_cast<UINT>(targetRect.bottom - targetRect.top);
    if (initialWidth == 0 || initialHeight == 0) { initialWidth = 1280; initialHeight = 720; }

    FrameBoostBeta::Presenter presenter;
    if (monitorMode) {
        presenter.SetOverlayMonitor(outputMonitor);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (GetMonitorInfoW(outputMonitor, &mi)) {
            initialWidth = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
            initialHeight = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
        }
    }
    if (!presenter.Create(device.get(), initialWidth, initialHeight, L"RESET FRAMEBOOST - BETA", monitorMode ? nullptr : targetWindow)) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: could not create the presentation window/swapchain.");
        return 4;
    }
    presenter.SetTitleSuffix(L"GENERATING (F9 to toggle)");

    FrameBoost::MotionEstimation::Estimator estimator;
    FrameBoost::Interpolation::Interpolator interpolator;
    // Handed over here, not only in the F11 handler - which is dead while the
    // hotkeys are disabled, so "tint" on the command line set a variable that
    // never reached the shader. The one diagnostic that answers "do our frames
    // reach the screen at all" silently did nothing, and its blank result was
    // nearly taken as evidence that they do not.
    interpolator.SetDebugTint(debugTintMode);
    FrameBoostBeta::DuplicateDetector duplicateDetector;
    FrameBoostBeta::MotionStats motionStats;
    uint64_t duplicateFramesSinceReport = 0;
    // Snapshot of the capture.s own unchanged-frame counter at the last report,
    // so the per-second figure covers both ways of spotting an unchanged frame.
    uint64_t ddUnchangedAtReport = 0;

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER lastReport{};
    QueryPerformanceCounter(&lastReport);
    uint64_t nativeFramesSinceReport = 0;
    uint64_t generatedFramesSinceReport = 0;
    double lastCaptureMs = -1.0;
    double duplicateCheckMsSum = 0.0;
    uint64_t duplicateCheckSamples = 0;
    double lastCaptureLatencyMs = -1.0;
    double latencySumMs = 0.0;
    uint64_t latencySamples = 0;

    // GPU-contention circuit breaker: the first live Watch Dogs test showed
    // real, measured multi-hundred-ms GPU stalls (motion estimation jumping
    // from ~12ms to ~340ms) that self-recovered after a few seconds - most
    // likely two processes contending for the same physical GPU during a
    // heavy load/streaming burst in the game. We cannot prevent that
    // contention from a separate process, but we CAN stop compounding it:
    // once GPU time spikes well above its recent rolling average, skip the
    // extra interpolation + double-present work for a few ticks and just
    // pass the real frame through, so FrameBoost's own overhead doesn't
    // pile onto an already-stressed GPU. Never a crash risk - purely skips
    // optional generation.
    double gpuTimeEmaMs = -1.0;

    // Does this GPU have room to do our work at all?
    //
    // Separate from the spike breaker below, which only catches sudden jumps
    // (five times the running average). A GPU-bound game does not produce
    // spikes - it produces a sustained high cost, and the average rises with
    // it, so the spike test never fires. Measured in Watch Dogs at 1440p:
    // motion estimation 30.6-33.4 ms and interpolation 29.9-30.6 ms, against
    // 0.50 and 0.45 ms in a game that leaves the GPU some room. Our work sat in
    // the queue behind the game.s, the picture reached the screen 79-95 ms old,
    // and the output ran BELOW the source: 14 frames shown for 17 delivered.
    //
    // Taking frames away from someone who asked for more of them is the one
    // outcome this feature must never produce.
    double generationCostEmaMs = -1.0;
    bool gpuHasRoom = true;
    double gpuRoomVerdictSinceMs = 0.0;
    double lastInterpolationRunMs = 0.0;
    // Long enough that a swing in the measured interval cannot toggle the
    // overlay, short enough that a game genuinely out of GPU is left alone
    // quickly.
    static constexpr double kGpuRoomHoldMs = 500.0;

    // Standing aside has to mean getting out of the way COMPLETELY.
    //
    // Measured in Watch Dogs with the new guard active: output dropped to 0
    // frames per second while the overlay stayed on screen, holding its last
    // picture over a game that was still running underneath. Pausing generation
    // without hiding the window is worse than anything it was meant to prevent.
    //
    // Hiding it also costs nothing: no capture processing reaches the screen, no
    // present, no latency - the player simply sees their game.
    bool overlayHidden = false;
    auto SetOverlayVisible = [&](bool visible) {
        if (visible == !overlayHidden) return;
        overlayHidden = !visible;
        if (HWND hwnd = presenter.WindowHandle())
            ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        FrameBoostBeta::Logger::Log(visible
            ? "[FrameBoostBeta] Overlay shown - boosting again."
            : "[FrameBoostBeta] Overlay hidden - standing aside completely, the game is displayed directly.");
    };
    constexpr double kEmaAlpha = 0.1;
    bool inDegradedMode = false;
    int degradedFrameCounter = 0;
    constexpr int kDegradedRetryIntervalFrames = 30; // ~once every 0.5-1s at typical passthrough rates

    // A/B comparison toggle (F9): the first live test made the boosted
    // window feel WORSE than the real game despite a higher reported
    // output FPS. Real, plausible causes are added end-to-end latency and
    // interpolation-quality artifacts - separate problems needing separate
    // fixes. F9 forces pure passthrough (real captured frame only, no
    // generation) so the two can be compared directly without restarting.
    bool forcePassthroughOnly = false;
    bool f9WasDown = false;

    // F10: vsync on/off. Phase timing proved ~99.5% of every iteration is
    // spent inside Present (34.2 of 34.4 ms), i.e. blocked on presentation,
    // not on our own GPU work (0.012 ms to submit). This toggle separates
    // "blocked waiting for the display/compositor" from "the per-present
    // copy+rescale itself is expensive" in a single live run.
    UINT presentSyncInterval = 0; // measured: syncInterval 1 blocked ~34ms/present and capped output at 29 FPS
    bool f10WasDown = false;

    // F11: tint generated frames red. Answers "are generated frames actually
    // reaching the screen?" instantly and unambiguously - a question that
    // could not be settled by screenshots, because the overlay shows a copy
    // of the captured window and therefore looks identical either way.
    unsigned int debugTint = debugTintMode;
    bool f11WasDown = false;

    // Frame pacing. Measured: presenting with syncInterval 1 blocked ~34ms
    // per present (two 60Hz vblanks), capping output at ~29 FPS, while
    // syncInterval 0 cost only ~6.5ms and reached 144 output FPS. So vsync
    // blocking - not our shaders (0.08ms + 0.12ms) - was the entire
    // bottleneck. But presenting both frames back-to-back unpaced is what
    // made generated frames invisible in the earlier Watch Dogs prototype:
    // without spacing, the compositor simply shows the newest one. So we
    // present unblocked AND place the generated frame at the measured
    // temporal midpoint between two real frames ourselves.
    bool pacingEnabled = true;

    // Refresh-locked output cadence. Free-running pacing produced ~80 output
    // FPS on a 144 Hz panel - not a divisor of 144, so frames alternated
    // between one and two refresh intervals on screen and the result juddered
    // despite the healthy FPS figure. Locking the cadence to refresh/2
    // (72 Hz here) gives every frame an identical 13.89 ms on-screen
    // duration, which is what actually reads as smooth.
    const double outputRefreshHz = MonitorRefreshHz(outputMonitor);
    // One output slot per refresh interval. How many of those slots get
    // filled is now decided per real frame by the adaptive factor below,
    // instead of being fixed at "every second slot" (a hardcoded 2x).
    double outputSlotMs = outputRefreshHz > 0.0 ? 1000.0 / outputRefreshHz : 0.0;
    double nextPresentDueMs = 0.0;   // absolute deadline for the next present
    bool refreshLockEnabled = outputSlotMs > 0.0;
    {
        std::ostringstream oss;
        oss << "[FrameBoostBeta] Display refresh: "
            << (outputRefreshHz > 0 ? std::to_string(outputRefreshHz) + " Hz" : "unknown")
            << " | Refresh-locked output cadence: "
            << (refreshLockEnabled ? std::to_string(outputSlotMs) + " ms per slot" : "disabled")
            << " | F8 toggles the lock.";
        FrameBoostBeta::Logger::Log(oss.str());
    }
    bool f8WasDown = false;

    // F7: transparency mode. Defaults ON where the composition path is
    // available, because it addresses the one problem that measurement could
    // not otherwise solve - a covered source stops being drawn by Windows.
    bool transparentRealFrames = true;
    bool f7WasDown = false;

    // Adaptive generation factor. A fixed 2x is wrong in both directions: it
    // wastes the display when the source is slow (30 FPS doubled is 60 on a
    // 144 Hz panel, leaving 84 Hz unused) and it manufactures frames that
    // are not missing when the source is already fast. So: measure the real
    // frame rate, and produce only the frames needed to reach the refresh
    // rate - which at factor 1 means generating nothing at all.
    //
    // Capped at 4x because every generated frame past the first sits further
    // from a real reference, and interpolation error grows with that
    // distance. Beyond 4x the artefacts cost more than the smoothness gains.
    // Latency cap. Each extra generated frame per pair holds the REAL frame
    // back one more output slot (6.94 ms at 144 Hz) so its partners can be
    // shown first - that hold IS the added latency. Measured end to end:
    // 3-6 ms average on-screen age at factor 3, 12-20 ms worst case, against
    // ~1.4 ms for pure passthrough. F6 caps the factor at 2 to halve the
    // hold, trading output frames for responsiveness.
    //
    // Fixed at 2. The engine doubles whatever the game is running at - that
    // is the whole promise, and it is what held up by eye: every higher
    // factor tested worse, because it holds the real frame back longer and
    // puts more guessed frames between two known ones.
    int maxFactor = 2;
    // False while the source runs faster than half the refresh rate, where a
    // doubled frame cannot be displayed any more (see the simple-2x path).
    // Doubling is never switched off for being "too fast for the display".
    //
    // It was, briefly: above half the refresh rate the extra frames cannot all
    // be shown, so the engine stood aside. Measured on a steady 96 FPS source
    // that is the better outcome - but a source hovering near the threshold,
    // which is the normal case, made it switch back and forth, and the panel
    // announced each switch. Constant announcements about doing nothing are
    // worse than the frames they were saving.
    //
    // The GPU-room guard below stays: that one prevents actual harm - taking
    // frames away from the game - rather than declining a marginal gain.
    const bool doublingFitsDisplay = true;
    bool f6WasDown = false;
    int generationFactor = 1; // 1 = pure passthrough, nothing generated

    // Hysteresis. Without it a source hovering near a switch point (say 47-49
    // FPS against a 144 Hz display) flips between 3x and 2x every second -
    // and the switch itself is visible, so the cure would be worse than the
    // disease. A new factor must be the better fit by a clear margin AND
    // stay that way for several consecutive evaluations before it is taken.
    constexpr double kFactorSwitchMargin = 0.18; // ~18% better fit required
    constexpr int kFactorSwitchHoldFrames = 30;  // and sustained this long
    int candidateFactor = 1;
    int candidateFactorHeldFrames = 0;

    double lastRealPresentMs = 0.0;
    double realFrameIntervalEmaMs = -1.0;
    // What the output pacing runs on: the rate frames ARRIVE at, which is not
    // the same as the rate the game produces new pictures (see where it is set).
    double pacingIntervalMs = -1.0;

    // Fixed delay between a frame.s capture timestamp and the moment it is
    // shown. Everything the 2x path presents is scheduled against this, so the
    // spacing on screen mirrors the spacing the source produced. Seeded on the
    // first pair and then only nudged.
    double simplePresentOffsetMs = -1.0;
    // How long a frame takes to reach us after the compositor timestamped it.
    double arrivalLagEmaMs = -1.0;
    // Content time of the predicted frame waiting to be shown.

    double generatedContentMs = 0.0;

    // The interval the OUTPUT is paced on: the arrival rate where it is known,
    // the processed rate otherwise.
    auto OutputInterval = [&]() {
        return (pacingIntervalMs > 0.5 && pacingIntervalMs < 100.0) ? pacingIntervalMs : realFrameIntervalEmaMs;
    };

    // Regularity of the source, and the gate built on it.
    //
    // Interpolation cannot smooth an irregular input. The interval between two
    // captured frames is replayed uniformly across the slots it spans, so a
    // varying interval becomes varying motion speed - several times a second,
    // which reads as heavy judder no matter how evenly the output itself is
    // paced. Generating in that state actively makes the picture worse than
    // passing it through untouched, so the engine stops generating instead.
    //
    // Same principle as the adaptive factor: produce nothing where there is
    // nothing to gain.
    double intervalDeviationEmaMs = -1.0;
    bool sourceIsIrregular = false;
    int regularityHoldFrames = 0;

    // Deviation as a fraction of the interval, calibrated against the only
    // measurement that settles the question - which of the two actually looks
    // better, generation or plain passthrough:
    //
    //   browser video   0.0-2.2%   generation clearly better
    //   Delta Force    11.6-24.2%  passthrough clearly better (reported)
    //
    // The threshold sits in the gap between those two regimes, with margin on
    // both sides. A first attempt put it at 20% by guesswork, which left the
    // game hovering right on the boundary and never tripping.
    //
    // Separate enter/leave values so a source sitting near the line does not
    // switch back and forth - the switch itself is visible.
    constexpr double kIrregularEnterRatio = 0.08;
    constexpr double kIrregularLeaveRatio = 0.05;
    constexpr int kRegularityHoldFrames = 45; // ~1 s at 45 real FPS
    double generatedFrameDueAtMs = 0.0;
    int64_t lastFrameTimestamp100ns = 0;

    // END-TO-END latency: how old the content is at the moment it is put on
    // screen, measured against WGC's own capture timestamp for the frame it
    // came from. This is the honest number - capture latency alone (~1.4 ms)
    // says nothing about the delay the interpolation scheme itself adds by
    // holding a real frame back so its generated partners can be shown first.
    int64_t currentPairTimestamp100ns = 0;
    // Diagnostic: how many frames WGC actually hands over per second when we
    // poll at the full output rate, as opposed to once per pair. If this is
    // higher than Native FPS, the surplus was being discarded unseen.
    int64_t pendingCaptureTimestamp100ns = 0;
    bool havePendingCapture = false;
    uint64_t captureArrivalsSinceReport = 0;
    double presentAgeSumMs = 0.0, presentAgeMaxMs = 0.0;
    uint64_t presentAgeSamples = 0;

    auto RecordPresentAge = [&]() {
        if (currentPairTimestamp100ns <= 0) return;
        LARGE_INTEGER qpcNow{};
        QueryPerformanceCounter(&qpcNow);
        const double now100ns = static_cast<double>(qpcNow.QuadPart) / qpcFreq.QuadPart * 10000000.0;
        double ageMs = (now100ns - static_cast<double>(currentPairTimestamp100ns)) / 10000.0;
        if (ageMs < 0.0) ageMs = 0.0;
        presentAgeSumMs += ageMs;
        if (ageMs > presentAgeMaxMs) presentAgeMaxMs = ageMs;
        ++presentAgeSamples;
    };
    FrameBoostBeta::Logger::Log(std::string("[FrameBoostBeta] Frame generation scheme: ")
        + (extrapolateMode ? "EXTRAPOLATION (predicted forward, real frame not held back)"
                           : "INTERPOLATION (between two real frames, newest held back half an interval)"));
    FrameBoostBeta::Logger::Log(g_hotkeysEnabled
        ? "[FrameBoostBeta] Hotkeys ENABLED (started with \"hotkeys\") and need CTRL+ALT: CTRL+ALT+F4 simple 2x, F5 one-frame buffer, F6 latency cap, F7 transparency, F8 refresh lock, F9 generation on/off, F11 tint generated frames."
        : "[FrameBoostBeta] Hotkeys disabled - the tuned configuration cannot be changed by anything the game sends. Start with \"hotkeys\" to enable them for testing.");
    // Timestamps of the two real frames the motion field spans, in the same
    // clock as NowMs(). The output is driven from these, not from a counter.
    double phaseSumForReport = 0.0;
    double lastShownContentMs = 0.0;
    double contentStepSum = 0.0, contentStepSumSq = 0.0;
    double contentStepMin = 1e9, contentStepMax = -1e9;
    uint64_t contentStepCount = 0, contentStepBackwards = 0;

    // Records how far the displayed CONTENT moved since the last shown frame.
    // Called from every present, on both output paths, with the moment in the
    // source.s own timeline that the frame represents.
    auto RecordContentStep = [&](double shownContentMs) {
        if (lastShownContentMs > 0.0) {
            const double step = shownContentMs - lastShownContentMs;
            if (step > -50.0 && step < 100.0) {
                contentStepSum += step;
                contentStepSumSq += step * step;
                ++contentStepCount;
                if (step < contentStepMin) contentStepMin = step;
                if (step > contentStepMax) contentStepMax = step;
                if (step < 0.0) ++contentStepBackwards;
            }
        }
        lastShownContentMs = shownContentMs;
    };
    uint64_t phaseCountForReport = 0, timelineSlotsForReport = 0;
    double motionPrevTimestampMs = 0.0;
    double motionCurrTimestampMs = 0.0;

    // The pair interval with outliers taken out - and it MUST be this rather
    // than the raw difference.
    //
    // motionPrevTimestampMs starts at zero, so the very first pair measures not
    // 13 ms but every millisecond since the machine booted. Offsets built on
    // that were astronomically large, every wait ran to its ceiling - a ceiling
    // computed from the same number, so it protected nothing - and the engine
    // sat for 36 seconds without writing a single telemetry line. On screen
    // that is one frozen frame: reported as "stuck again". The same guard
    // covers a pause, an alt-tab or a loading screen.
    // The source's TRUE period, locked on rather than followed.
    //
    // Pacing used to take the raw interval between the two frames of the
    // current pair. That interval is what the capture reports, and the capture
    // is not a clock: measured live against a game sitting steadily on its
    // 72 fps cap, it delivered 13.66 ms +- 2.47, 18.03 +- 8.44, and at times
    // 25.57 +- 15.32 - deviations of 18% to 60% on a source that was not
    // varying at all.
    //
    // That interval is then replayed uniformly across the output slots it
    // spans, so a wrong interval does not cost a frame, it changes the SPEED
    // the motion is shown at. Alternating fast and slow playback is read by
    // the eye as stutter, and it is read that way even when every present
    // lands on its refresh - which is why the output measured a clean 144 and
    // was still described as feeling like 20-30 fps.
    //
    // A game.s frame period is a physical constant over the second or two that
    // matters here: a 72 fps cap is 13.889 ms and stays there. So the period is
    // tracked with a slow average, and pacing uses THAT, not the per-pair
    // measurement. Jitter in the capture stops reaching the output at all.
    double lockedPeriodMs = -1.0;
    auto UpdateSourcePeriod = [&](double intervalMs) {
        if (!(intervalMs > 1.0 && intervalMs < 100.0)) return;
        // A plain slow average, with no tolerance window around the current
        // value.
        //
        // The window was the bug. It was there to reject outliers, but the
        // arithmetic mean of the intervals IS the true period by definition -
        // total elapsed time over the number of frames - so a stall is not a
        // distortion to be rejected, it is part of the answer. Rejecting
        // samples relative to the current estimate rejects them asymmetrically
        // instead, which locks in whatever bias the estimate already has: it
        // was measured stuck at 19.3 ms (51.8 FPS) against a source delivering
        // 63.6, and could not walk back to it.
        //
        // 0.03 gives a time constant of about 30 real frames - half a second
        // at these rates. Long enough that per-frame capture jitter never
        // reaches the output, short enough to follow a genuine rate change
        // without needing a special case for one.
        lockedPeriodMs = (lockedPeriodMs > 1.0 && lockedPeriodMs < 100.0)
            ? lockedPeriodMs * 0.97 + intervalMs * 0.03
            : intervalMs;
    };

    auto PacingInterval = [&]() {
        // The lock, not the measurement - see UpdateSourcePeriod above.
        if (lockedPeriodMs > 1.0 && lockedPeriodMs < 100.0) return lockedPeriodMs;
        const double raw = motionCurrTimestampMs - motionPrevTimestampMs;
        if (raw > 1.0 && raw < 100.0) return raw;
        if (realFrameIntervalEmaMs > 1.0 && realFrameIntervalEmaMs < 100.0) return realFrameIntervalEmaMs;
        return 16.7; // nothing measured yet: assume 60 FPS until it is
    };
    // Wall-clock moment the newer of the two frames reached us. The phase is
    // measured from here, so capture latency is not counted twice.
    double motionCurrArrivalMs = 0.0;

    // Frame QUEUE (F5), not a single slot.
    //
    // A single slot holds only the newest frame, so every frame that arrives
    // while the clock is still replaying the current pair is overwritten and
    // lost. That is not a small waste - it is self-reinforcing: the pair then
    // spans several source intervals, which takes proportionally longer to
    // replay, which loses proportionally more frames. Measured with the single
    // slot: frames arriving every 18 ms but pairs spanning 62-69 ms, so two
    // frames in three were discarded and the interpolation had to bridge gaps
    // three times longer than it should.
    //
    // A queue keeps them in order, and the clock consumes them one at a time.
    // Time-driven output is the default: one frame per refresh, so every
    // refresh shows something new.
    //
    // Simple 2x (CTRL+ALT+F4) doubles the source instead, which at ~50 real
    // FPS means 100 output on a 144 Hz panel - and 100 does not divide 144.
    // Its content steps evenly every 10 ms while the display holds each frame
    // for either 7 or 14 ms, alternating. That is the same mismatch that makes
    // 24 fps film judder on a 60 Hz television, it is visible however clean
    // each individual frame is, and no amount of interpolation quality touches
    // it - which is exactly what was reported: a dumped frame with no visible
    // artefacts, and judder unchanged.
    // "timedriven" selects the clock-driven output path instead: one frame per
    // refresh with the phase taken from the clock, fed by the frame queue. It
    // was switched off back when capture delivered half the source rate and the
    // queue was starved; that reason is gone.
    // EXACTLY DOUBLE, always: one generated frame per real frame, whatever the
    // display refresh rate is.
    //
    // The clock-driven path fills every refresh instead, which means a 60 FPS
    // source on a 144 Hz panel gets 2.4 output frames per real frame - more
    // generated frames than real ones, at phases that shift from pair to pair.
    // It measured better (139-140 output of 144) and it is the more general
    // design, but generating until the refresh rate is full is not what this
    // feature promises, and every extra generated frame is another guess
    // between the same two known ones. "timedriven" selects it.
    bool simpleDoubleMode = !HasArg(L"timedriven");

    // DOUBLE THE SOURCE, ON A SMOOTH CLOCK.
    //
    // The clock-driven path places frames by asking the wall clock where it
    // stands between two real frames, which is what makes an uneven source come
    // out even. Its output rate was the refresh rate, so a 60 FPS source got
    // 2.4 generated frames per real one - more guesses than knowns.
    //
    // Its output rate is now twice the measured source rate instead. Same
    // smoothing, exactly one generated frame per real frame, and no frame
    // invented only to fill a refresh that had nothing new for it.
    // Off by default: tried, measured, worse. Limiting the clock-driven path.s
    // output rate to twice the source made content advance LESS evenly than the
    // pair-paced path (sd 4.1-6.2 ms against 3.0-5.1), added steps of exactly
    // 0 ms - the content standing still for a frame because the output clock no
    // longer lands on the display.s grid - and produced a backwards step.
    // "doublerate" re-enables it for further work.
    const bool doubleRateOutput = HasArg(L"doublerate");
    const bool snapToRefreshGrid = HasArg(L"refreshsnap");
    const bool waitForDisplaySlot = HasArg(L"slotwait");
    bool f4WasDown = false;
    bool f12WasDown = false;
    bool autoDumpDone = false;
    bool realFramePendingSimple = false;

    // Extrapolation is the default: the real frame is never held back, which is
    // where interpolation spends more than half its added latency. "interpolate"
    // on the command line selects the older scheme for comparison.
    bool generatedPendingSimple = false;
    double generatedDueAtMs = 0.0;
    double nextRealPresentDueMs = 0.0;
    // Fixed anchor for the refresh grid the 2x mode snaps its presents to.
    double refreshAnchorMs = 0.0;
    double realFrameDueAtMs = 0.0;

    // On for the time-driven output: the queue is what lets a clock-paced
    // output survive a source that delivers unevenly, and without it the
    // regularity gate switches generation off entirely in a game.
    bool bufferOneFrame = true;
    bool f5WasDown = false;
    constexpr int kFrameQueueSize = 4;
    ID3D11Texture2D* queueTex[kFrameQueueSize] = {};
    double queueTimestampMs[kFrameQueueSize] = {};
    int queueHead = 0;   // next to be fed to the estimator
    int queueCount = 0;
    UINT queueWidth = 0, queueHeight = 0;
    uint64_t queueDroppedSinceReport = 0;

    auto EnsureFrameQueue = [&](ID3D11Texture2D* like) -> bool {
        if (!like) return false;
        D3D11_TEXTURE2D_DESC desc{};
        like->GetDesc(&desc);
        if (queueTex[0] && desc.Width == queueWidth && desc.Height == queueHeight) return true;

        for (auto& t : queueTex) { if (t) { t->Release(); t = nullptr; } }
        queueHead = 0;
        queueCount = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        for (auto& t : queueTex) {
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &t))) {
                FrameBoostBeta::Logger::Log("[FrameBoostBeta] Could not create the frame queue - "
                    "running without buffering.");
                return false;
            }
        }
        queueWidth = desc.Width;
        queueHeight = desc.Height;
        return true;
    };

    // Presentation clock offset behind the capture clock. Rather than being
    // computed once, it is nudged every slot to keep the phase inside the
    // interval it is meant to cover - the same idea an audio player uses to
    // stay in sync with a clock it does not control. A fixed offset cannot
    // work here because the source's own rate drifts.
    double presentOffsetMs = -1.0;
    bool haveMotionField = false;

    // Per-phase CPU wall-clock accounting. The GPU timestamp queries turned
    // out to be ambiguous under cross-process GPU contention (whichever
    // dispatch happened to be measured absorbed the scheduling wait, so the
    // "cost" appeared to jump between motion estimation and interpolation
    // without either shader actually changing). These CPU-side numbers say
    // unambiguously where the ~34ms per iteration really goes.
    double phaseComputeMsSum = 0.0;   // submitting estimation + interpolation
    // Output interval jitter - the number that actually corresponds to
    // "smooth". An average of 144.0 FPS says nothing about evenness: 144
    // frames with occasional hitches look worse than 120 perfectly spaced
    // ones, and the average hides exactly that. So measure the gap between
    // consecutive presents and report its spread, plus how many gaps missed
    // the refresh interval by more than half of one.
    double lastPresentAtMs = -1.0;
    double gapSumMs = 0.0, gapSumSqMs = 0.0;
    double gapMinMs = 1e9, gapMaxMs = 0.0;
    uint64_t gapSamples = 0, gapMissed = 0, gapCollapsed = 0;

    auto RecordPresentGap = [&](double presentEndMs) {
        if (lastPresentAtMs > 0.0) {
            const double gap = presentEndMs - lastPresentAtMs;
            gapSumMs += gap;
            gapSumSqMs += gap * gap;
            if (gap < gapMinMs) gapMinMs = gap;
            if (gap > gapMaxMs) gapMaxMs = gap;
            ++gapSamples;
            if (outputSlotMs > 0.0 && std::abs(gap - outputSlotMs) > outputSlotMs * 0.5) ++gapMissed;
            // Closer than one refresh: counted by us, never seen by anyone.
            if (outputSlotMs > 0.0 && gap < outputSlotMs * 0.9) ++gapCollapsed;
        }
        lastPresentAtMs = presentEndMs;
    };

    double phasePresentMsSum = 0.0;   // CopyResource + Present, incl. any vsync block
    double phaseIterationMsSum = 0.0; // whole iteration
    uint64_t phaseSamples = 0;

    auto NowMs = [&]() {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / qpcFreq.QuadPart * 1000.0;
    };

    // Two frames cannot share one refresh: the display scans out once per
    // interval, so a present that follows too closely replaces one that was
    // never shown. Measured in Apex: 35-41% of all presents landed closer
    // together than a refresh, some 0.34 ms apart - four frames in ten counted
    // and never displayed, which is why an output of 146 did not even look
    // like 72.
    //
    // Waiting costs latency, and that is the trade being made deliberately
    // here: a frame nobody sees is worth less than a frame that arrives a
    // little later.
    auto WaitForDisplaySlot = [&]() {
        // Off unless asked for: it removed the collisions completely (35-41%
        // down to 0.0%) and the picture got WORSE, not better - the added
        // latency was real and the promised gain never showed up. Kept behind
        // "slotwait" because the measurement it produced still stands.
        if (!waitForDisplaySlot) return;
        if (outputSlotMs <= 0.0 || lastPresentAtMs <= 0.0) return;
        const double earliest = lastPresentAtMs + outputSlotMs * 0.95;
        const double ceiling = NowMs() + 20.0;
        while (NowMs() < earliest && NowMs() < ceiling) { ddCapture.Pump(); }
    };

    // Presents the real frame whose slot is still owed, if any. Returns true
    // when it did.

    // Holds until this frame's slot on the refresh-locked grid comes up, then
    // advances the grid by exactly one slot. Coarse Sleep for the bulk of the
    // wait (cheap, but only millisecond-accurate) plus a short spin for the
    // remainder, because a slot is only 13.89 ms and being 1 ms late means
    // missing a whole refresh interval. If we have already fallen behind by
    // more than a slot, the grid is re-anchored to now rather than trying to
    // catch up with a burst of presents that would themselves judder.
    auto WaitForOutputSlot = [&]() {
        if (!refreshLockEnabled || !pacingEnabled) return;
        const double nowMs = NowMs();

        if (nextPresentDueMs <= 0.0) {
            nextPresentDueMs = nowMs; // first present establishes the grid
        } else if (nowMs > nextPresentDueMs) {
            // Late. Advance to the next grid line in WHOLE slots rather than
            // snapping the grid to "now" - snapping was measured to be the
            // single biggest smoothness bug in the whole pipeline: every
            // fourth present went out 0.29 ms after the previous one (mean
            // interval 5.22 ms against a 6.94 ms refresh, 25% of presents
            // off-grid). The display cannot show two frames in one refresh,
            // so it simply discarded one: nominally 144 FPS, actually ~96
            // reaching the screen at uneven intervals.
            //
            // Skipping a slot shows one frame for two refresh intervals,
            // which is visibly better than presenting two frames into one
            // interval and having the display throw one away.
            const double behindMs = nowMs - nextPresentDueMs;
            nextPresentDueMs += std::ceil(behindMs / outputSlotMs) * outputSlotMs;
        }

        double remainingMs = nextPresentDueMs - NowMs();
        // Acquire while waiting, rather than sleeping through frames the
        // compositor is presenting right now. Measured before this: 60-70
        // coalesced updates per second - two thirds of a 90 FPS source read
        // as one frame, because the loop only asked once per output slot.
        while (remainingMs > 1.5) {
            ddCapture.Pump();
            Sleep(1);
            remainingMs = nextPresentDueMs - NowMs();
        }
        while (NowMs() < nextPresentDueMs) { ddCapture.Pump(); }

        nextPresentDueMs += outputSlotMs;
    };

    // Picks the factor whose resulting output rate lands closest to the
    // display's refresh rate, and only switches when the new choice is
    // clearly better and has stayed better (see the hysteresis constants).
    auto EvaluateGenerationFactor = [&]() {
        if (!refreshLockEnabled || realFrameIntervalEmaMs <= 0.0) return;
        const double nativeFps = 1000.0 / realFrameIntervalEmaMs;

        auto relativeError = [&](int factor) {
            return std::abs(factor * nativeFps - outputRefreshHz) / outputRefreshHz;
        };

        int best = 1;
        for (int f = 2; f <= maxFactor; ++f)
            if (relativeError(f) < relativeError(best)) best = f;

        // The source already saturates the display: generate nothing. No
        // added latency, no quality loss, no GPU cost - there are no missing
        // frames to supply, and inventing them anyway would be dishonest.
        if (nativeFps >= outputRefreshHz * 0.95) best = 1;

        if (best == generationFactor) { candidateFactor = best; candidateFactorHeldFrames = 0; return; }

        if (best != candidateFactor) { candidateFactor = best; candidateFactorHeldFrames = 0; }
        ++candidateFactorHeldFrames;

        const bool clearlyBetter = relativeError(candidateFactor) + kFactorSwitchMargin < relativeError(generationFactor);
        if (clearlyBetter && candidateFactorHeldFrames >= kFactorSwitchHoldFrames) {
            std::ostringstream oss;
            oss << "[FrameBoostBeta] Generation factor " << generationFactor << "x -> " << candidateFactor
                << "x (native " << nativeFps << " FPS, display " << outputRefreshHz
                << " Hz, target output " << (candidateFactor * nativeFps) << " FPS)";
            if (candidateFactor == 1) oss << " - source saturates the display, generating nothing.";
            FrameBoostBeta::Logger::Log(oss.str());
            generationFactor = candidateFactor;
            candidateFactorHeldFrames = 0;
        }
    };

    // Submitted vs actually displayed, per reporting interval. The gap
    // between them is the part of the pipeline our own timing cannot see.
    UINT lastStatsPresentCount = 0, lastStatsRefreshCount = 0;
    bool statsAvailable = false, statsUnsupportedLogged = false;
    double submittedPerSecond = -1.0, displayedPerSecond = -1.0;

    auto ReportTelemetryIfDue = [&]() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double elapsed = static_cast<double>(now.QuadPart - lastReport.QuadPart) / qpcFreq.QuadPart;
        if (elapsed < 1.0) return;

        double nativeFps = nativeFramesSinceReport / elapsed;

        double generatedFps = generatedFramesSinceReport / elapsed;
        double outputFps = nativeFps + generatedFps;
        double avgLatencyMs = latencySamples > 0 ? (latencySumMs / latencySamples) : -1.0;

        // What the display actually showed, straight from DXGI, versus what
        // we submitted. PresentRefreshCount advances by one per refresh that
        // showed a new present, so its delta is the number of our frames that
        // genuinely reached the panel.
        UINT presentCount = 0, presentRefresh = 0, syncRefresh = 0;
        if (presenter.QueryPresentStats(presentCount, presentRefresh, syncRefresh)) {
            if (statsAvailable) {
                displayedPerSecond = (presentCount - lastStatsPresentCount) / elapsed;
                submittedPerSecond = (presentRefresh - lastStatsRefreshCount) / elapsed;
            }
            lastStatsPresentCount = presentCount;
            lastStatsRefreshCount = presentRefresh;
            statsAvailable = true;
        } else if (!statsUnsupportedLogged) {
            statsUnsupportedLogged = true;
            FrameBoostBeta::Logger::Log("[FrameBoostBeta] DXGI frame statistics unavailable on this swapchain "
                "(expected for the legacy BitBlt path a layered window forces). That path gives no per-refresh "
                "delivery guarantee, so submitted frames and displayed frames cannot be compared here.");
        }

        std::ostringstream oss;
        // Two different numbers that were conflated until now: how fast the
        // source actually delivers frames, and how many of those reach the
        // screen as real (non-generated) frames. The first is the game.s frame
        // rate; the second is what our pacing manages to place. Reporting only
        // the second under the name "Native FPS" made a pacing problem look
        // like a capture problem for most of a day.
        const double sourceFps = realFrameIntervalEmaMs > 0.0 ? 1000.0 / realFrameIntervalEmaMs : -1.0;
        // Native FPS counts real frames presented UNCHANGED. On the clock-driven
        // path that is a small share by design - a frame whose phase lands mid
        // interval is shown as an interpolation of itself and its neighbour,
        // not skipped - so Source FPS is the number that describes the game.
        oss << "[FrameBoostBeta] Source FPS: " << sourceFps
            << " | Native FPS: " << nativeFps
            << " | Generated FPS: " << generatedFps
            << " | Output FPS: " << outputFps
            << " | Poll time: " << lastCaptureMs << " ms"
            << " | Duplicate check: " << (duplicateCheckSamples ? duplicateCheckMsSum / duplicateCheckSamples : -1.0) << " ms avg"
            << " | Capture latency (real, avg): " << (avgLatencyMs >= 0 ? std::to_string(avgLatencyMs) + " ms" : "N/A")
            << " | Display Hz: " << outputRefreshHz
            << " | Capture path: " << (useDesktopDuplication ? "Desktop Duplication" : "Windows Graphics Capture")
            << " | Stale frames dropped/poll: " << capture.LastDiscardedStaleFrames()
            << " | Doubling: " << (gpuHasRoom ? "on" : "standing aside (no GPU room)")
            << " | Generation cost: " << generationCostEmaMs << " ms"
            << " | Coalesced by us: " << ddCapture.CoalescedFrames()
            << " | Capture published/consumed: " << ddCapture.FramesPublished() << "/" << ddCapture.FramesConsumed()
            << " | Cursor-only updates: " << ddCapture.CursorOnlyUpdates()
            << " | Capture reconnects: " << ddCapture.Reconnects()
            << " | Duplicate frames skipped/s: " << ((duplicateFramesSinceReport + (ddCapture.UnchangedFrames() - ddUnchangedAtReport)) / elapsed)
            << " | Unchanged by dirty rects: " << ddCapture.UnchangedFrames()
            << " | Frame-to-frame difference: " << duplicateDetector.LastDifference()
            << " | Real frame interval (measured): " << (realFrameIntervalEmaMs > 0 ? std::to_string(realFrameIntervalEmaMs) + " ms" : "N/A")
            << " | Vsync: " << (presentSyncInterval == 0 ? "off" : "on")
            << " | Refresh lock: " << (refreshLockEnabled ? "on" : "off")
            << " | Generation factor: " << generationFactor << "x"
            << " | On-screen age: " << (presentAgeSamples ? std::to_string(presentAgeSumMs / presentAgeSamples) + " ms avg, " + std::to_string(presentAgeMaxMs) + " ms max" : "N/A")
            << " | Locked source period: " << lockedPeriodMs << " ms (" << (lockedPeriodMs > 0 ? 1000.0 / lockedPeriodMs : 0.0) << " FPS)"
            << " | Source regularity: " << ((realFrameIntervalEmaMs > 0 && intervalDeviationEmaMs >= 0)
                ? std::to_string(100.0 * intervalDeviationEmaMs / realFrameIntervalEmaMs) + "% deviation, " + (sourceIsIrregular ? "IRREGULAR (generation continues - the one-frame buffer covers it)" : "steady")
                : std::string("N/A"))
            << " | Queue depth: " << queueCount << " (dropped/s " << (queueDroppedSinceReport / elapsed) << ")"
            << " | Presents lost to collision: " << (gapSamples ? 100.0 * gapCollapsed / gapSamples : -1.0) << "%"
            << " | Content step: " << (contentStepCount ? contentStepSum / contentStepCount : -1.0) << " ms mean, min "
            << (contentStepCount ? contentStepMin : -1.0) << ", max " << (contentStepCount ? contentStepMax : -1.0)
            << ", sd " << (contentStepCount > 1
                ? std::sqrt((std::max)(0.0, contentStepSumSq / contentStepCount
                    - (contentStepSum / contentStepCount) * (contentStepSum / contentStepCount)))
                : -1.0)
            << ", backwards " << contentStepBackwards
            << " | Phase avg: " << (phaseCountForReport ? phaseSumForReport / phaseCountForReport : -1.0)
            << " | Timeline slots: " << (phaseCountForReport ? 100.0 * timelineSlotsForReport / phaseCountForReport : -1.0) << "%"
            << " | Real interval: " << (motionCurrTimestampMs - motionPrevTimestampMs) << " ms"
            << " | Capture arrivals/s: " << (captureArrivalsSinceReport / elapsed)
            << " | Transparency: " << ((transparentRealFrames && presenter.SupportsTransparency()) ? "on" : "off")
            << " | Moving blocks: " << (motionStats.MovingBlockPercent() >= 0 ? std::to_string(motionStats.MovingBlockPercent()) + "%" : "N/A")
            << " | Motion mean/max px: " << motionStats.MeanMagnitudePixels() << "/" << motionStats.MaxMagnitudePixels()
            << " | Search-saturated blocks: " << motionStats.SaturatedBlockPercent() << "%"
            << " | Match error mean/max: " << motionStats.MeanMatchError() << "/" << motionStats.MaxMatchError()
            << " | Blocks with no real match: " << motionStats.PoorMatchPercent() << "%"
            << " | Displayed/submitted: " << displayedPerSecond << "/" << submittedPerSecond
            << " | Motion estimation GPU: " << estimator.LastGpuTimeMs() << " ms"
            << " | Interpolation GPU: " << interpolator.LastGpuTimeMs() << " ms";
        if (gapSamples > 1) {
            const double mean = gapSumMs / gapSamples;
            const double variance = gapSumSqMs / gapSamples - mean * mean;
            oss << " || Output interval: " << mean << " ms"
                << " (min " << gapMinMs << ", max " << gapMaxMs
                << ", jitter " << (variance > 0.0 ? std::sqrt(variance) : 0.0) << " ms"
                << ", missed slots " << (100.0 * gapMissed / gapSamples) << "%)";
        }
        if (phaseSamples > 0) {
            oss << " || CPU per iteration: " << (phaseIterationMsSum / phaseSamples) << " ms"
                << " (compute submit " << (phaseComputeMsSum / phaseSamples) << " ms"
                << ", present " << (phasePresentMsSum / phaseSamples) << " ms)";
        }
        FrameBoostBeta::Logger::Log(oss.str());

        nativeFramesSinceReport = 0;
        generatedFramesSinceReport = 0;
        duplicateFramesSinceReport = 0;
        ddUnchangedAtReport = ddCapture.UnchangedFrames();
        duplicateCheckMsSum = 0.0;
        duplicateCheckSamples = 0;
        latencySumMs = 0.0;
        latencySamples = 0;
        phaseComputeMsSum = 0.0;
        presentAgeSumMs = 0.0; presentAgeMaxMs = 0.0; presentAgeSamples = 0;
        captureArrivalsSinceReport = 0;
        queueDroppedSinceReport = 0;
        phaseSumForReport = 0.0; phaseCountForReport = 0; timelineSlotsForReport = 0;
        contentStepSum = 0.0; contentStepSumSq = 0.0; contentStepCount = 0;
        contentStepMin = 1e9; contentStepMax = -1e9; contentStepBackwards = 0;
        gapSumMs = 0.0; gapSumSqMs = 0.0; gapMinMs = 1e9; gapMaxMs = 0.0; gapSamples = 0; gapMissed = 0; gapCollapsed = 0;
        phasePresentMsSum = 0.0;
        phaseIterationMsSum = 0.0;
        phaseSamples = 0;
        lastReport = now;
    };

    while (!presenter.ShouldClose()) {
        double iterationStartMs = NowMs();
        presenter.PumpMessages();
        if (presenter.ShouldClose()) break;

        bool f9IsDown = HotkeyDown(VK_F9);
        if (f9IsDown && !f9WasDown) {
            forcePassthroughOnly = !forcePassthroughOnly;
            FrameBoostBeta::Logger::Log(forcePassthroughOnly
                ? "[FrameBoostBeta] F9: forcing PURE PASSTHROUGH (no generation) for A/B comparison."
                : "[FrameBoostBeta] F9: frame generation re-enabled.");
            presenter.SetTitleSuffix(forcePassthroughOnly ? L"PASSTHROUGH ONLY (F9 to toggle)" : L"GENERATING (F9 to toggle)");
        }
        f9WasDown = f9IsDown;

        // F12: dump the generated frame and its two real sources to disk. A
        // screenshot cannot show what the engine produces - the overlay is
        // excluded from capture so that monitor capture does not feed back on
        // itself - so the comparison has to be written from inside.
        // Triggered by hand OR automatically on the first fast turn.
        //
        // The hotkey alone was not enough: F12 never arrived, being commonly
        // claimed by screenshot tools and debuggers, and on many keyboards it
        // needs Fn as well. Firing the dump from the measurement itself
        // removes the dependency entirely and catches exactly the case worth
        // looking at - the fast motion where the picture breaks down - rather
        // than whatever happened to be on screen when a key was pressed.
        const bool fastTurnToDump = !autoDumpDone
            && motionStats.MeanMagnitudePixels() > 30.0
            && haveMotionField;
        if (fastTurnToDump) autoDumpDone = true;

        bool f12IsDown = HotkeyDown(VK_F12);
        if (((f12IsDown && !f12WasDown) || fastTurnToDump) && estimator.CurrFrameTexture() && haveMotionField) {
            // Generate once into the interpolator's OWN texture. In normal
            // operation the shader writes straight into the back buffer to
            // avoid a full-frame copy, which leaves that texture empty - so
            // for the dump the frame has to be produced again where it can be
            // read back.
            D3D11_TEXTURE2D_DESC dumpDesc{};
            estimator.CurrFrameTexture()->GetDesc(&dumpDesc);
            interpolator.SetPhase(0.5f);
            interpolator.SetStatusFlags(0u); // no status squares in the dump
            if (interpolator.GenerateFrame(device.get(), context.get(),
                    estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                    dumpDesc.Width, dumpDesc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, nullptr)) {
                FrameBoostBeta::FrameDump::SaveComparison(device.get(), context.get(),
                    estimator.PrevFrameTexture(), interpolator.GeneratedFrameTexture(), estimator.CurrFrameTexture());
            }
        }
        f12WasDown = f12IsDown;

        // F4: simple 2x vs. the time-driven output.
        bool f4IsDown = HotkeyDown(VK_F4);
        if (f4IsDown && !f4WasDown) {
            simpleDoubleMode = !simpleDoubleMode;
            realFramePendingSimple = false;
            FrameBoostBeta::Logger::Log(simpleDoubleMode
                ? "[FrameBoostBeta] F4: SIMPLE 2x - one generated frame per real frame at the midpoint, paced off the source."
                : "[FrameBoostBeta] F4: time-driven output - one frame per refresh, phase from the clock.");
        }
        f4WasDown = f4IsDown;

        // F5: one-frame buffer. The fix for an irregular source, at the cost
        // of one frame of latency - worth toggling, since video does not care
        // about the latency and a shooter does.
        bool f5IsDown = HotkeyDown(VK_F5);
        if (f5IsDown && !f5WasDown) {
            bufferOneFrame = !bufferOneFrame;
            queueHead = 0; queueCount = 0;
            presentOffsetMs = -1.0;
            FrameBoostBeta::Logger::Log(bufferOneFrame
                ? "[FrameBoostBeta] F5: ONE-FRAME BUFFER on - the displayed pair is fully in the past, so an uneven source no longer freezes and jumps. Costs one frame of latency."
                : "[FrameBoostBeta] F5: one-frame buffer off - lowest latency, but an uneven source will freeze and jump again.");
        }
        f5WasDown = f5IsDown;

        // F6: low-latency mode. Caps the generation factor at 2, so a real
        // frame is held one output slot instead of two before being shown.
        bool f6IsDown = HotkeyDown(VK_F6);
        if (f6IsDown && !f6WasDown) {
            maxFactor = (maxFactor == 2) ? 3 : 2;
            candidateFactorHeldFrames = 0;
            if (generationFactor > maxFactor) generationFactor = maxFactor;
            FrameBoostBeta::Logger::Log(maxFactor == 2
                ? "[FrameBoostBeta] F6: 2x - one generated frame per real frame, at the symmetric midpoint."
                : "[FrameBoostBeta] F6: 3x - two generated frames per real frame, for sources running at about a third of the display's refresh rate.");
        }
        f6WasDown = f6IsDown;

        bool f7IsDown = HotkeyDown(VK_F7);
        if (f7IsDown && !f7WasDown) {
            transparentRealFrames = !transparentRealFrames;
            FrameBoostBeta::Logger::Log(transparentRealFrames
                ? "[FrameBoostBeta] F7: TRANSPARENCY ON - real frames show the actual screen, only generated frames come from us."
                : "[FrameBoostBeta] F7: transparency off - every frame is our own captured copy.");
        }
        f7WasDown = f7IsDown;

        // F8: refresh-lock on/off. The whole point of the lock is that it is
        // meant to look smoother at a LOWER frame count than free-running
        // pacing, which is counterintuitive enough that it has to be
        // A/B-comparable live rather than argued about.
        bool f8IsDown = HotkeyDown(VK_F8);
        if (f8IsDown && !f8WasDown && outputSlotMs > 0.0) {
            refreshLockEnabled = !refreshLockEnabled;
            nextPresentDueMs = 0.0; // re-anchor the grid on the next present
            FrameBoostBeta::Logger::Log(refreshLockEnabled
                ? "[FrameBoostBeta] F8: refresh-locked cadence ON (evenly spaced frames)."
                : "[FrameBoostBeta] F8: refresh-locked cadence OFF (free-running).");
        }
        f8WasDown = f8IsDown;

        bool f10IsDown = HotkeyDown(VK_F10);
        if (f10IsDown && !f10WasDown) {
            presentSyncInterval = presentSyncInterval == 0 ? 1 : 0;
            FrameBoostBeta::Logger::Log(presentSyncInterval == 0
                ? "[FrameBoostBeta] F10: VSYNC OFF (present syncInterval 0)."
                : "[FrameBoostBeta] F10: VSYNC ON (present syncInterval 1).");
        }
        f10WasDown = f10IsDown;

        bool f11IsDown = HotkeyDown(VK_F11);
        if (f11IsDown && !f11WasDown) {
            debugTint = debugTint ? 0u : 1u;
            interpolator.SetDebugTint(debugTint);
            FrameBoostBeta::Logger::Log(debugTint
                ? "[FrameBoostBeta] F11: DEBUG TINT ON - generated frames are tinted red."
                : "[FrameBoostBeta] F11: debug tint off.");
            presenter.SetTitleSuffix(debugTint ? L"DEBUG TINT ON (generated frames red)" : L"GENERATING (F9 to toggle)");
        }
        f11WasDown = f11IsDown;

        // TIME-DRIVEN OUTPUT. One iteration = one output slot = one present,
        // always, whatever the source is doing.
        //
        // This replaced a scheme that emitted a fixed number of frames per
        // real frame (the "factor"). That can only be evenly paced when the
        // source rate divides the refresh rate, which is true of a browser
        // locking itself to 48 FPS on a 144 Hz display, and false of every
        // game: measured in Delta Force, ~50 real FPS against 144 Hz left a
        // third of all output slots empty ("missed slots 32-36%") and output
        // intervals swinging between 6.9 and 20 ms. No integer factor fixes
        // that - 50 x 2 = 100 and 50 x 3 = 150, neither of which is 144.
        //
        // Instead, each slot asks what the content SHOULD look like at this
        // instant and interpolates to exactly that point on the real
        // timeline. The spacing is then always one refresh interval, and the
        // source rate becomes irrelevant - 50, 63 or 87 FPS all work.
        //
        // Capture and estimation run before the slot wait, so that work
        // overlaps the idle time rather than delaying the next slot.
        LARGE_INTEGER captureStart{};
        QueryPerformanceCounter(&captureStart);

        // Pump here as well as inside the pacing waits: on the simple 2x path the
        // loop may never reach a wait until it has a frame to work with, and
        // pumping only from the waits left the engine with no frames at all.
        if (useDesktopDuplication) ddCapture.Pump();

        UINT frameW = 0, frameH = 0;
        int64_t frameTimestamp100ns = 0;
        bool isNewFrame = false;
        ID3D11Texture2D* capturedTex = useDesktopDuplication
            ? ddCapture.PollLatestFrame(frameW, frameH, frameTimestamp100ns, isNewFrame)
            : capture.PollLatestFrame(frameW, frameH, frameTimestamp100ns, isNewFrame);

        LARGE_INTEGER captureEnd{};
        QueryPerformanceCounter(&captureEnd);
        // Poll/retrieval time - how long the (usually already-ready) frame
        // took to fetch, not the true capture latency (see below).
        lastCaptureMs = static_cast<double>(captureEnd.QuadPart - captureStart.QuadPart) / qpcFreq.QuadPart * 1000.0;

        // Whether this iteration brought genuinely new content to estimate
        // from. Either way the slot below is still presented - it just shows
        // the real frame when there is nothing new to interpolate toward.
        // Feeding an unchanged frame into the estimator would produce a zero
        // motion field and poison its previous-frame reference.
        bool haveNewContent = (capturedTex != nullptr) && isNewFrame;

        if (haveNewContent) {
            presenter.Resize(frameW, frameH);
            presenter.TrackOverlayTarget(); // follow the window if it moves/resizes
        }

        // A recomposited-but-unchanged frame carries no new information.
        // Interpolating against it yields a zero motion field and a
        // byte-identical "generated" frame - the output counter rises while
        // nothing gets smoother. Skip it entirely and wait for content that
        // actually changed, so interpolation always runs between two
        // genuinely different frames.
        //
        // This runs BEFORE the timing below, and that ordering is the whole
        // point. It used to come after, so the "real frame interval" was
        // measured between arrivals rather than between frames that differ -
        // and with Desktop Duplication the two are nothing alike: the
        // compositor republishes an unchanged screen ~144 times a second, so
        // a game capped at 60 measured as a ~14 ms source instead of ~16.7 ms.
        // Everything downstream took that number at face value: generated
        // frames were placed at the midpoint of an interval shorter than the
        // real one, and the doubling check computed 2 x 72 = 144 and switched
        // generation off on a display that could have shown 2 x 60 = 120
        // comfortably. Seen live as "Doubling fits display" flipping yes/no
        // every second while only 6-10 frames a second carried new content.
        // Timed, because it runs on EVERY arrival - about ninety times a second
        // with Desktop Duplication - and it reads back from the GPU, which stalls
        // the pipeline. Suspected of being why only half the frames that carry
        // new content ever reach the estimator.
        LARGE_INTEGER dupStart{};
        QueryPerformanceCounter(&dupStart);
        // MEASURED, and it did not work out: replacing this comparison with the
        // capture API.s dirty-rectangle metadata caught nothing at all - zero
        // unchanged frames over 20 seconds and ~75 arrivals per second, where
        // the comparison below finds 20-45 a second on the same screen. A game
        // presenting full-screen flips has the driver report the whole screen
        // as dirty on every present, identical content or not, so "dirty" says
        // nothing about whether anything changed.
        //
        // The dirty-rect early-out still runs inside the capture, where it
        // costs nothing and is correct on the rare frame that reports no dirty
        // region at all. "dirtyonly" trusts it alone, for re-testing this on
        // other hardware.
        const bool useDirtyRects = useDesktopDuplication && useDirtyRectsOnly;
        const bool frameIsDuplicate = haveNewContent && !useDirtyRects
            && duplicateDetector.IsDuplicate(device.get(), context.get(), capturedTex);
        LARGE_INTEGER dupEnd{};
        QueryPerformanceCounter(&dupEnd);
        if (haveNewContent && !useDirtyRects) {
            duplicateCheckMsSum += static_cast<double>(dupEnd.QuadPart - dupStart.QuadPart) / qpcFreq.QuadPart * 1000.0;
            ++duplicateCheckSamples;
        }

        if (frameIsDuplicate) {
            ++duplicateFramesSinceReport;
            // Recomposited but unchanged: no new content to estimate from, so
            // it is treated exactly like "no new frame". The slot below still
            // gets presented - skipping the present made the output look
            // frozen on a static screen, which is indistinguishable from a
            // crash to the viewer.
            haveNewContent = false;
        }

        // Real additional latency: how old the frame handed to us actually is,
        // measured against the same 100ns-since-boot clock the capture path
        // timestamps with - the honest number, not the poll time above.
        if (frameTimestamp100ns > 0 && haveNewContent) {
            // WGC's SystemRelativeTime uses the QueryPerformanceCounter
            // clock domain (confirmed empirically - QueryInterruptTimePrecise
            // was consistently ~14ms off, a different clock entirely), so
            // "now" must be computed the same way for a correct comparison.
            LARGE_INTEGER qpcNow{};
            QueryPerformanceCounter(&qpcNow);
            double now100ns = static_cast<double>(qpcNow.QuadPart) / qpcFreq.QuadPart * 10000000.0;
            double latency100ns = now100ns - static_cast<double>(frameTimestamp100ns);
            // Sub-millisecond cross-timestamp jitter can occasionally make
            // this very slightly negative for an unchanged/duplicate frame -
            // clamp rather than discard, so the metric doesn't go missing
            // for genuinely near-zero-latency frames.
            if (latency100ns < 0) latency100ns = 0;

            lastCaptureLatencyMs = latency100ns / 10000.0;
            latencySumMs += lastCaptureLatencyMs;
            ++latencySamples;

            // Real frame interval, measured from the target application's own
            // capture timestamps rather than from our loop's timing - this is
            // the interval the generated frame has to be placed in the middle
            // of. Outliers (loading hitches, alt-tab) are rejected so one
            // stall doesn't poison the pacing for the following seconds.
            if (lastFrameTimestamp100ns > 0) {
                double intervalMs = (frameTimestamp100ns - lastFrameTimestamp100ns) / 10000.0;
                if (intervalMs > 1.0 && intervalMs < 100.0) {
                    // How far this interval sits from the running average,
                    // tracked alongside the average itself. This is the number
                    // that decides whether generating anything is worthwhile:
                    // the interval between two captured frames is replayed
                    // UNIFORMLY across the output slots it spans, so if that
                    // interval keeps changing, the apparent speed of motion
                    // changes with it. Measured in a game: 20.83 -> 27.18 ->
                    // 34.70 ms within a second, which is why a perfectly paced
                    // 144 FPS output was reported as feeling like ~35.
                    if (realFrameIntervalEmaMs > 0.0) {
                        const double deviation = std::abs(intervalMs - realFrameIntervalEmaMs);
                        intervalDeviationEmaMs = intervalDeviationEmaMs < 0.0
                            ? deviation
                            : intervalDeviationEmaMs * 0.8 + deviation * 0.2;
                    }
                    realFrameIntervalEmaMs = realFrameIntervalEmaMs < 0.0
                        ? intervalMs
                        : realFrameIntervalEmaMs * 0.8 + intervalMs * 0.2;
                    // The interval between two REAL frames - the thing pacing
                    // has to reproduce. Not the capture.s publish rate, which
                    // counts overlays and cursor updates the game never drew,
                    // and which locked the period at 124 FPS against a 68 FPS
                    // source.
                    UpdateSourcePeriod(intervalMs);
                }
            }
            // The source rate is taken from the CAPTURE, where every published
            // frame is seen, rather than from the frames this loop had time to
            // process.
            //
            // Measuring it downstream is a feedback loop the moment the loop
            // falls behind: skip every second frame and the measured interval
            // doubles, so the output waits twice as long for its real frame,
            // which guarantees the next one is skipped as well. Measured in
            // Apex: 74 frames per second published, 37 consumed, the source
            // reported as 36.5, and the doubled output landing at 73 - the
            // game's own rate, so the doubling was worth nothing. That state is
            // stable; the engine cannot climb out of it by itself.
            //
            // And it is used for PACING ONLY. The capture publishes more than
            // the game draws - overlays, the cursor, other windows - so using
            // it for the displayed rate made "GAME FPS" read far above what
            // Apex was actually running at. The number the panel shows still
            // comes from frames that carried new content.
            if (useDesktopDuplication) {
                const double published = ddCapture.PublishedIntervalMs();
                if (published > 0.5 && published < 100.0) pacingIntervalMs = published;
                // And it is what the period lock tracks. It is measured AT the
                // capture, where every published frame is seen, so it does not
                // depend on how fast this loop manages to pace its output -
                // which an earlier attempt did, producing a feedback loop that
                // stabilised the whole engine at 73 FPS.
            }

            lastFrameTimestamp100ns = frameTimestamp100ns;
            currentPairTimestamp100ns = frameTimestamp100ns;

            // Regularity gate. Evaluated per real frame, and only switched
            // after the new verdict has held for about a second, because
            // toggling generation is itself visible.
            if (realFrameIntervalEmaMs > 0.0 && intervalDeviationEmaMs >= 0.0) {
                const double ratio = intervalDeviationEmaMs / realFrameIntervalEmaMs;
                const bool verdictIrregular = sourceIsIrregular
                    ? (ratio > kIrregularLeaveRatio)   // stay irregular until clearly settled
                    : (ratio > kIrregularEnterRatio);  // become irregular only when clearly bad

                if (verdictIrregular == sourceIsIrregular) {
                    regularityHoldFrames = 0;
                } else if (++regularityHoldFrames >= kRegularityHoldFrames) {
                    sourceIsIrregular = verdictIrregular;
                    regularityHoldFrames = 0;
                    std::ostringstream oss;
                    oss << "[FrameBoostBeta] Source is " << (sourceIsIrregular ? "IRREGULAR" : "steady again")
                        << " (interval " << realFrameIntervalEmaMs << " ms +- " << intervalDeviationEmaMs
                        << " ms, " << (100.0 * ratio) << "%) - "
                        << (sourceIsIrregular
                            ? "generation continues - the one-frame buffer is what makes an uneven source usable. "
                              "This only reports the source; it does not switch anything off."
                            : "generation back on.");
                    FrameBoostBeta::Logger::Log(oss.str());
                }
            }

            // Re-evaluated here, as soon as the measured interval updates,
            // rather than further down the loop: the duplicate-frame path
            // below returns early, so on a static screen the factor would
            // otherwise never be reconsidered and would still read 1x when
            // motion resumes.
            EvaluateGenerationFactor();

            // Output slot follows the source: exactly two output frames per real
            // frame. Recomputed as the measured interval moves, so a game that
            // speeds up or slows down keeps its doubling instead of drifting
            // toward whatever the panel happens to refresh at.
            if (doubleRateOutput && realFrameIntervalEmaMs > 1.0) {
                const double wanted = realFrameIntervalEmaMs * 0.5;
                // Never faster than the display can show, and never so slow that
                // a stalled source freezes the output clock entirely.
                const double floorMs = outputRefreshHz > 0.0 ? 1000.0 / outputRefreshHz : 1.0;
                outputSlotMs = (wanted < floorMs) ? floorMs : ((wanted > 40.0) ? 40.0 : wanted);
            }
        }

        // While degraded, DON'T run motion estimation at all except a
        // periodic retry - the first live test showed that once GPU
        // contention pushes motion estimation's own cost into the hundreds
        // of milliseconds, simply skipping the (cheap) interpolation step
        // was not enough: this heavy GPU dispatch itself was still running
        // every single tick and single-handedly capping the whole loop at
        // ~3 FPS. Skipping the dispatch entirely during degradation is what
        // actually restores a responsive passthrough framerate.
        double computeStartMs = NowMs();

        bool ranEstimationThisTick = false;
        ++degradedFrameCounter;

        if (haveNewContent && !forcePassthroughOnly && !measureOnlyMode
                && (!inDegradedMode || degradedFrameCounter >= kDegradedRetryIntervalFrames)) {
            degradedFrameCounter = 0;
            // Motion field and frame timestamps persist across slots: the
            // output is driven by the clock, not by frame arrivals, so several
            // slots interpolate from the same pair of real frames.
            // ONE FRAME OF BUFFER. The newest frame is held back and only fed
            // to the estimator when the frame after it arrives.
            //
            // Without it the engine starts replaying an interval the moment
            // its second frame lands, without knowing how long that interval
            // will last - so a late frame leaves the phase pinned at 1 (the
            // picture freezes) and the next arrival jumps. That freeze-and-jump
            // is what an irregular source turns into, and it is what made a
            // perfectly paced 144 FPS output feel like ~35 in a game measuring
            // +-25% jitter.
            //
            // Holding one frame back means the pair being displayed is always
            // fully in the past: its duration is known exactly, and the frame
            // that ends it is already in hand. Costs one frame of latency,
            // which is why it is a toggle (F5).
            // The queue belongs to the time-driven output only. Simple 2x skips
            // that section entirely, so with the queue in the way its frames
            // were parked and never handed to the estimator: the motion field
            // stopped updating and the same intermediate frame was generated
            // over and over. Measured: 121 generated frames per second, zero
            // real frames presented, and content stepping only ~50 times a
            // second - reported as "no judder at all, but it looks like 30 fps".
            if (bufferOneFrame && !simpleDoubleMode && EnsureFrameQueue(capturedTex)) {
                // Buffered: the frame is only parked here. Whether it becomes
                // the next pair is decided by the presentation CLOCK further
                // down, not by its arrival.
                //
                // Tying that to arrival - the obvious first attempt - only
                // moves the problem: a late frame still leaves the clock
                // sitting at the end of the current pair with nothing to
                // advance to, so it freezes exactly as before. Measured with
                // that version: average phase 0.67-1.00, pinned at 1 whenever
                // a frame ran late.
                if (queueCount == kFrameQueueSize) {
                    // Full: the clock has fallen far enough behind that the
                    // oldest entry is stale. Drop it rather than the newest -
                    // dropping the newest would stall the timeline entirely.
                    queueHead = (queueHead + 1) % kFrameQueueSize;
                    --queueCount;
                    ++queueDroppedSinceReport;
                }
                const int tail = (queueHead + queueCount) % kFrameQueueSize;
                context->CopyResource(queueTex[tail], capturedTex);
                queueTimestampMs[tail] = frameTimestamp100ns / 10000.0;
                ++queueCount;
            } else {
                haveMotionField = estimator.ProcessFrame(device.get(), context.get(), capturedTex);
                ranEstimationThisTick = true;
                if (haveMotionField) {
                    motionPrevTimestampMs = motionCurrTimestampMs;
                    motionCurrTimestampMs = frameTimestamp100ns / 10000.0;
                    motionCurrArrivalMs = NowMs();
                }
            }
        }

        if (ranEstimationThisTick) {
            double meGpuMs = estimator.LastGpuTimeMs();

            // What generating one frame currently costs us on this GPU,
            // estimation plus interpolation, against what a source frame is
            // worth in time. Above half the source interval there is no room to
            // do the work without delaying the very frames we are meant to be
            // adding to - so generation stops until there is.
            // The interpolation figure is only usable while interpolation is
            // actually running - and once this guard has paused it, it is not.
            // Reading LastGpuTimeMs() then returns whatever it measured during
            // the contention that triggered the pause, forever, so the cost never
            // falls back below the threshold and the guard never releases. It
            // latched: measured 9 ms of "generation cost" while the two parts it
            // is made of were reporting 0.53 and 0.43 ms in the same telemetry
            // line.
            //
            // Motion estimation always runs, whether or not a frame is generated,
            // so it is the honest half of the measurement. While interpolation is
            // idle its cost is estimated as equal to it - measured repeatedly at
            // roughly one to one - and the guard can recover on its own.
            const bool interpFresh = (NowMs() - lastInterpolationRunMs) < 500.0;
            const double interpMs = interpFresh ? interpolator.LastGpuTimeMs() : meGpuMs;
            const double costMs = (meGpuMs >= 0.0 ? meGpuMs : 0.0) + (interpMs >= 0.0 ? interpMs : 0.0);
            if (costMs > 0.0) {
                generationCostEmaMs = generationCostEmaMs < 0.0
                    ? costMs
                    : generationCostEmaMs * 0.8 + costMs * 0.2;

                if (realFrameIntervalEmaMs > 1.0) {
                    // 0.7 to leave, 0.5 to return - raised from 0.5/0.3 after it began
                    // firing on healthy frames. Measured: generation costs
                    // 3.0-4.0 ms against source intervals of 9.8-33 ms, so a
                    // single measurement spike of 8.7 ms was enough to trip a
                    // threshold set at half a 13 ms interval, and the player got
                    // told their game was using the whole graphics card when it
                    // was not.
                    //
                    // The case this exists for is nowhere near the new line
                    // either: in a GPU-bound game the same figures read 59 ms
                    // against 22 ms, which trips it several times over.
                    const bool hasRoom = gpuHasRoom
                        ? (generationCostEmaMs < realFrameIntervalEmaMs * 0.7)   // leave once clearly over
                        : (generationCostEmaMs < realFrameIntervalEmaMs * 0.5);  // return only with margin

                    // ...and only after the verdict has held for about half a
                    // second. Without this it flipped twice within the same
                    // second - "has room" at a 32 ms interval, "no room" at 18 ms,
                    // both at the same cost - because the measured source rate
                    // swings, and each flip shows or hides the overlay. A guard
                    // that blinks is worse than the problem it guards against.
                    // Held by TIME, not by a count of measurements. Counting
                    // measurements ties recovery to how often motion estimation
                    // happens to run - and on a quiet screen it barely runs at
                    // all, so the engine could sit paused long after the GPU had
                    // room again, waiting for ticks that were not coming.
                    if (hasRoom == gpuHasRoom) gpuRoomVerdictSinceMs = 0.0;
                    else if (gpuRoomVerdictSinceMs <= 0.0) gpuRoomVerdictSinceMs = NowMs();

                    if (hasRoom != gpuHasRoom && gpuRoomVerdictSinceMs > 0.0
                            && NowMs() - gpuRoomVerdictSinceMs >= kGpuRoomHoldMs) {
                        gpuRoomVerdictSinceMs = 0.0;
                        gpuHasRoom = hasRoom;
                        std::ostringstream oss;
                        oss << "[FrameBoostBeta] " << (hasRoom ? "GPU has room again" : "GPU has no room")
                            << ": generating a frame costs " << generationCostEmaMs
                            << " ms against a source interval of " << realFrameIntervalEmaMs << " ms."
                            << (hasRoom ? " Doubling resumes."
                                        : " Passing the game through untouched rather than slowing it down.");
                        FrameBoostBeta::Logger::Log(oss.str());
                    }
                }
            }

            if (meGpuMs >= 0.0) {
                if (gpuTimeEmaMs < 0.0) {
                    gpuTimeEmaMs = meGpuMs; // seed on first real reading
                } else {
                    bool isSpike = meGpuMs > 50.0 && meGpuMs > gpuTimeEmaMs * 5.0;
                    if (isSpike && !inDegradedMode) {
                        inDegradedMode = true;
                        FrameBoostBeta::Logger::Log("[FrameBoostBeta] GPU contention spike detected ("
                            + std::to_string(meGpuMs) + " ms vs ~" + std::to_string(gpuTimeEmaMs)
                            + " ms average) - pausing frame generation entirely, passing real frames through until it recovers.");
                    } else if (!isSpike && inDegradedMode && meGpuMs < gpuTimeEmaMs * 2.0) {
                        inDegradedMode = false;
                        FrameBoostBeta::Logger::Log("[FrameBoostBeta] GPU contention recovered - resuming frame generation.");
                    } else if (isSpike && inDegradedMode) {
                        // Still bad - stay degraded, and don't let this
                        // probe sample pollute the rolling average.
                    }
                    if (!isSpike) gpuTimeEmaMs = gpuTimeEmaMs * (1.0 - kEmaAlpha) + meGpuMs * kEmaAlpha;
                }
            }
        }

        double computeEndMs = NowMs();

        // Diagnostics: how much of the frame the estimator actually sees
        // moving. Sampled rarely (the readback stalls the pipeline).
        if (haveMotionField) motionStats.SampleIfDue(device.get(), context.get(), estimator.MotionVectorTexture());

        // Diagnostic mode: measure the source and present nothing at all, so
        // the engine stops competing with the game for the GPU.
        if (measureOnlyMode) {
            Sleep(1);
            ReportTelemetryIfDue();
            continue;
        }

        // ---- SIMPLE 2x -----------------------------------------------------
        // One generated frame per real frame, at the exact midpoint, paced off
        // the source rather than the refresh grid. Deliberately the simplest
        // scheme there is: every earlier version that tried to be cleverer -
        // adaptive factors, arbitrary phases, a frame buffer - was judged
        // worse by eye than plain doubling, so this is the one to get right
        // first.
        //
        // The real frame is held back half an interval so its generated
        // partner has somewhere to sit. That half interval IS the added
        // latency, and it is the least any interpolator can manage.
        // Show the overlay only while we are actually adding something.
        SetOverlayVisible(doublingFitsDisplay && gpuHasRoom && !forcePassthroughOnly);

        if (simpleDoubleMode) {
            // Every present is snapped to a refresh boundary.
            //
            // Presenting the moment a frame is ready leaves the display to
            // round it up to its own next refresh, and how long that takes
            // depends on where the frame happened to land - so a frame stands
            // for one refresh or for two, at random. Measured: intervals
            // scattered between 6.76 and 15.73 ms with 2.91 ms of jitter. The
            // eye reads that unevenness as judder even though the frame count
            // is right.
            //
            // Snapping makes the alternation regular instead of random. It
            // costs up to one refresh interval of latency (3.5 ms on average),
            // which is cheap against 2.7 ms of total added latency today.
            auto WaitForRefreshBoundary = [&]() {
                // OFF by default: the output follows the SOURCE, not the display.
                //
                // Snapping every present to the panel.s grid ties the whole
                // feature to the refresh rate: doubling 62 FPS gives 124, which
                // does not divide 144, so frames were held for one refresh or
                // two and the content advanced in steps between 3.7 and 20.2 ms.
                // The cure for that was always a demand on the user - cap the
                // game, or set the monitor to 120 Hz - and a tool that only
                // works at the right refresh rate is not a general tool.
                //
                // Without snapping, frames go out at twice the source rate, from
                // the source.s own timestamps, and the display shows what it can
                // reach. That is what doubling means, on any monitor.
                // "refreshsnap" restores the old behaviour.
                if (!snapToRefreshGrid) return;
                if (outputSlotMs <= 0.0) return;
                if (refreshAnchorMs <= 0.0) refreshAnchorMs = NowMs();
                const double t = NowMs();
                const double elapsed = t - refreshAnchorMs;
                const double boundary = refreshAnchorMs + std::ceil(elapsed / outputSlotMs) * outputSlotMs;
                const double waitMs = boundary - t;
                double remaining = waitMs;
                while (remaining > 1.5) { ddCapture.Pump(); Sleep(1); remaining = boundary - NowMs(); }
                while (NowMs() < boundary) { ddCapture.Pump(); }
            };

            const double nowMs = NowMs();

            // Always exactly double: one generated frame per real frame, at the
            // midpoint of the interval the source itself sets.
            //
            // This used to derive the count from the display instead - three
            // output frames per real frame at ~48 FPS, because 3 x 48 lands
            // on a 144 Hz refresh exactly. That is the better answer for
            // pacing on paper, and it lost every comparison by eye: a higher
            // factor holds the real frame back longer (more latency) and
            // stacks more guessed frames between two known ones (more
            // artefacts). Doubling is the honest promise the feature makes -
            // whatever the game runs at, it runs at twice that - and it is
            // the one the person testing it asked for.
            const int outputPerReal = 2;



            // EXTRAPOLATION: show the real frame the instant it arrives, and
            // predict the in-between frame forward from it afterwards.
            //
            // Interpolation cannot do this. To place a frame between N and N+1
            // it must have N+1 in hand, so N+1 waits half an interval - 7.8 ms
            // of the 13.4 ms measured at 64 FPS, and the reason the raw game
            // still felt more responsive than the boosted output. Predicting
            // forward pays none of that: the newest real frame goes straight
            // out, and the generated frame that follows is a guess about a
            // moment that has not happened yet.
            //
            // The guess is wrong exactly where a moving object uncovers
            // background, because no later frame exists to copy it from. That
            // is the trade, and it is for the eye to judge, not the numbers.
            if (extrapolateMode) {
                // PREDICT FORWARD, so the real frame is never held back.
                //
                // Interpolation has to wait: a frame placed between N and N+1
                // needs N+1 in hand, so N+1 is shown half an interval late -
                // 6.8 ms at 73 FPS, and the only part of the delay that belongs
                // to us rather than to the game. Predicting forward from the
                // newest real frame pays none of it: the real frame goes out as
                // soon as it arrives, and the generated one follows it.
                //
                // What it cannot know is what a moving object uncovers, because
                // there is no later frame to copy that from - the block match
                // error decides how far the prediction is trusted, and where it
                // is weak the pixel stays put instead of smearing.
                if (haveNewContent && !forcePassthroughOnly && !inDegradedMode
                        && gpuHasRoom && realFrameIntervalEmaMs > 1.0) {
                    const double pairIntervalMs = PacingInterval();
                    const double arrivalLagMs = NowMs() - motionCurrTimestampMs;
                    arrivalLagEmaMs = arrivalLagEmaMs < 0.0
                        ? arrivalLagMs
                        : arrivalLagEmaMs * 0.9 + arrivalLagMs * 0.1;

                    // No half-interval term here, and that IS the latency win:
                    // the offset only has to cover the trip from the compositor
                    // to us, not the wait for a frame that comes after.
                    const double offsetMs = arrivalLagEmaMs + 1.0;

                    const double realDueAtMs = motionCurrTimestampMs + offsetMs;
                    const double realCeilingMs = NowMs() + 20.0;
                    while (NowMs() < realDueAtMs && NowMs() < realCeilingMs) { ddCapture.Pump(); }

                    if (transparentRealFrames && presenter.SupportsTransparency()) {
                        presenter.PresentTransparent(device.get(), context.get(), presentSyncInterval);
                    } else if (estimator.CurrFrameTexture()) {
                        presenter.PresentFrame(context.get(), estimator.CurrFrameTexture(), presentSyncInterval);
                    }
                    ++nativeFramesSinceReport;
                    RecordContentStep(motionCurrTimestampMs);
                    RecordPresentGap(NowMs());
                    RecordPresentAge();

                    // The prediction belongs half an interval AFTER the frame it
                    // was made from - it is the future, not an in-between.
                    generatedDueAtMs = motionCurrTimestampMs + pairIntervalMs * 0.5 + offsetMs;
                    generatedContentMs = motionCurrTimestampMs + pairIntervalMs * 0.5;
                    generatedPendingSimple = haveMotionField;
                }

                // The predicted frame, once its moment comes - or at once if a
                // new real frame has arrived, because then its moment has passed
                // and holding it back would only delay the real one behind it.
                if (generatedPendingSimple && estimator.CurrFrameTexture()
                        && (haveNewContent || NowMs() >= generatedDueAtMs)) {
                    D3D11_TEXTURE2D_DESC desc{};
                    estimator.CurrFrameTexture()->GetDesc(&desc);

                    interpolator.SetExtrapolateAhead(0.5f);
                    interpolator.SetPhase(0.5f);
                    interpolator.SetStatusFlags((transparentRealFrames && presenter.SupportsTransparency()) ? 2u : 0u);

                    ID3D11UnorderedAccessView* uav = presenter.AcquireBackBufferUAV(device.get());
                    if (interpolator.GenerateFrame(device.get(), context.get(),
                            estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                            desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, uav)) {
                        lastInterpolationRunMs = NowMs();

                        const double genCeilingMs = NowMs() + 20.0;
                        while (NowMs() < generatedDueAtMs && NowMs() < genCeilingMs) { ddCapture.Pump(); }

                        if (uav) presenter.PresentBackBuffer(presentSyncInterval);
                        else presenter.PresentFrame(context.get(), interpolator.GeneratedFrameTexture(), presentSyncInterval);
                        ++generatedFramesSinceReport;
                        RecordContentStep(generatedContentMs);
                        RecordPresentGap(NowMs());
                        RecordPresentAge();
                    }
                    generatedPendingSimple = false;
                }

                Sleep(0);
                ReportTelemetryIfDue();
                continue;
            }

            if (haveNewContent && haveMotionField && doublingFitsDisplay && gpuHasRoom
                    && !forcePassthroughOnly && !inDegradedMode
                    && realFrameIntervalEmaMs > 1.0) {
                // A new real frame just landed. Emit the intermediate frames
                // that belong before it, evenly spaced across the interval,
                // then queue the real frame for the end of it.
                //
                // At outputPerReal = 3 the phases are 1/3 and 2/3, presented
                // one third and two thirds of an interval apart, so with the
                // real frame the display gets three evenly spaced frames per
                // source frame - 144 on a 144 Hz panel.
                D3D11_TEXTURE2D_DESC desc{};
                estimator.CurrFrameTexture()->GetDesc(&desc);

                // The offset is COMPUTED from what it has to cover, never
                // accumulated.
                //
                // It was integrated at first - a fifth of a millisecond added
                // whenever a frame arrived late - which at 75 frames a second
                // grows by 15 ms every second with no way back down. Within
                // seconds the output was waiting hundreds of milliseconds and
                // the picture froze on a single frame. Reported as exactly
                // that: "it is always the same frame, non stop".
                //
                // Two things have to fit inside it: how long a frame takes to
                // reach us after the compositor timestamped it, and half an
                // interval, because the generated frame belongs BEFORE the real
                // frame it was made from and can only be shown once that frame
                // exists. Both are measured, so the offset tracks them instead
                // of drifting away from them.
                const double pairIntervalMs = PacingInterval();
                const double arrivalLagMs = NowMs() - motionCurrTimestampMs;
                arrivalLagEmaMs = arrivalLagEmaMs < 0.0
                    ? arrivalLagMs
                    : arrivalLagEmaMs * 0.9 + arrivalLagMs * 0.1;
                // No safety margin on top: measured, every millimetre of it shows
                // up in the on-screen age, and the age is what the hand feels.
                // Anchoring with a 1 ms margin read 8.1 ms against 4.4 ms for
                // pacing off the processing time, and bought only 0.4 ms less
                // jitter for it - the wrong trade for a shooter.
                simplePresentOffsetMs = arrivalLagEmaMs + pairIntervalMs * 0.5;

                for (int step = 1; step < outputPerReal; ++step) {
                    const float phaseForStep = static_cast<float>(step) / static_cast<float>(outputPerReal);
                    interpolator.SetPhase(phaseForStep);
                    interpolator.SetStatusFlags((transparentRealFrames && presenter.SupportsTransparency()) ? 2u : 0u);

                    ID3D11UnorderedAccessView* uav = presenter.AcquireBackBufferUAV(device.get());
                    if (!interpolator.GenerateFrame(device.get(), context.get(),
                            estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                            desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, uav)) {
                        break;
                    }
                    lastInterpolationRunMs = NowMs();

                    // Shown at the moment its CONTENT belongs to, plus a fixed
                    // offset - not at "now plus a fraction of an interval".
                    //
                    // Pacing from the moment we happened to finish processing
                    // writes every hiccup of our own loop straight into the
                    // spacing: measured content steps between 0.5 and 17 ms
                    // around a mean of 6.8, a standard deviation of 30-40%.
                    // Anchoring to the frame's own capture timestamp shows an
                    // uneven source exactly as unevenly as it was produced -
                    // which is what smooth motion actually requires - and drops
                    // our own scheduling noise out of the result.
                    // ...and it was measured, twice, and it did not pay off:
                    // 8.1 ms on-screen age anchored with a 1 ms margin, 6.5-6.9
                    // without it, against 4.4 ms when paced from the moment the
                    // pair was processed - with the same jitter either way (sd
                    // 2.1-2.5 against 2.2-2.6). Two milliseconds of hand-felt
                    // delay for nothing measurable is not a trade worth making,
                    // so the pacing runs off the processing time after all.
                    const double dueAtMs = nowMs
                        + pairIntervalMs * (static_cast<double>(step - 1) / outputPerReal);
                    // Same backstop as below: never wait longer than one source
                    // interval, so no arithmetic mistake can freeze the picture.
                    // A fixed ceiling, not one derived from the interval: a wait
                    // must never be able to inherit a bad measurement.
                    const double genCeilingMs = NowMs() + 20.0;
                    while (NowMs() < dueAtMs && NowMs() < genCeilingMs) { ddCapture.Pump(); }
                    WaitForDisplaySlot();
                    WaitForRefreshBoundary();

                    if (uav) presenter.PresentBackBuffer(presentSyncInterval);
                    else presenter.PresentFrame(context.get(), interpolator.GeneratedFrameTexture(), presentSyncInterval);
                    ++generatedFramesSinceReport;
                    // Half way between the two real frames of this pair, in the
                    // source.s own timeline.
                    RecordContentStep(motionPrevTimestampMs + phaseForStep * (motionCurrTimestampMs - motionPrevTimestampMs));
                    RecordPresentGap(NowMs());
                    RecordPresentAge();
                }

                // And then the real frame itself, at the end of the interval it
                // belongs to - waited out here rather than left pending.
                //
                // It used to be deferred to a later pass of the loop, tested
                // against a timestamp taken before the generated frame was made.
                // That worked only while every present also waited for a refresh
                // boundary, because the waiting was what made the old timestamp
                // late enough to pass the test. With the output paced off the
                // source instead, nothing consumed that time any more, the test
                // stopped passing, and the next arrival overwrote the frame:
                // measured 0-2 real frames per second against 73 generated ones.
                // Every frame on screen was a guess, and the output was the
                // source rate rather than twice it.
                //
                // The wait is bounded by half a source interval - about 7 ms -
                // and capture keeps running through it.
                // Same anchor for the real frame: its own capture timestamp
                // plus the offset.
                const double realDueAtMs = nowMs + pairIntervalMs
                    * (static_cast<double>(outputPerReal - 1) / outputPerReal);

                // Never wait longer than one source interval, whatever the
                // arithmetic says. A wait that can grow without bound is how the
                // picture froze; this is the backstop that makes that
                // impossible rather than merely unlikely.
                const double waitCeilingMs = NowMs() + 20.0;
                while (NowMs() < realDueAtMs && NowMs() < waitCeilingMs) { ddCapture.Pump(); }
                WaitForDisplaySlot();

                WaitForRefreshBoundary();
                if (transparentRealFrames && presenter.SupportsTransparency()) {
                    presenter.PresentTransparent(device.get(), context.get(), presentSyncInterval);
                } else if (estimator.CurrFrameTexture()) {
                    presenter.PresentFrame(context.get(), estimator.CurrFrameTexture(), presentSyncInterval);
                }
                ++nativeFramesSinceReport;
                RecordContentStep(motionCurrTimestampMs);
                RecordPresentGap(NowMs());
                RecordPresentAge();
            }

            Sleep(0); // yield without burning a core; the pacing is by clock above
            ReportTelemetryIfDue();
            continue;
        }

        // ---- The output slot ----------------------------------------------
        // Wait for this slot, then decide from the CLOCK what to show in it.
        WaitForOutputSlot();

        // The clock starts one and a half intervals behind the capture: far
        // enough that a late frame is already buffered, close enough that the
        // latency stays near the theoretical minimum. It is nudged from there.
        if (bufferOneFrame && presentOffsetMs < 0.0 && realFrameIntervalEmaMs > 0.0) {
            presentOffsetMs = realFrameIntervalEmaMs * 1.5;
        }

        // Advance to the next pair when the presentation clock has consumed
        // the current one - driven by the clock, never by frame arrivals. This
        // is what actually absorbs an irregular source: a frame that arrives
        // late was already buffered, and one that arrives early simply waits.
        if (bufferOneFrame && queueCount > 0 && presentOffsetMs > 0.0) {
            const double contentTimeMs = NowMs() - presentOffsetMs;
            const bool pairConsumed = (motionCurrTimestampMs <= 0.0) || (contentTimeMs >= motionCurrTimestampMs);
            // The OLDEST queued frame, so no real frame is skipped: consuming
            // the newest instead makes the pair span several source intervals
            // and discards everything in between.
            if (pairConsumed && queueTimestampMs[queueHead] > motionCurrTimestampMs) {
                if (estimator.ProcessFrame(device.get(), context.get(), queueTex[queueHead])) {
                    haveMotionField = true;
                    motionPrevTimestampMs = motionCurrTimestampMs;
                    motionCurrTimestampMs = queueTimestampMs[queueHead];
                    motionCurrArrivalMs = NowMs();
                }
                queueHead = (queueHead + 1) % kFrameQueueSize;
                --queueCount;
            }
        }

        const double realIntervalMs = motionCurrTimestampMs - motionPrevTimestampMs;
        const bool haveTimeline = haveMotionField && realIntervalMs > 1.0 && realIntervalMs < 200.0;

        // The interval between the two real frames is replayed over the wall
        // clock starting from the moment the newer one ARRIVED. Phase then
        // sweeps 0 to 1 across exactly one interval, whatever the capture
        // latency happens to be.
        //
        // Subtracting an estimated lag from "now" instead, as a first version
        // did, double-counts the capture latency: the frame reaches us ~7 ms
        // after it was composed, so a content time of now minus one full
        // interval lands BEFORE the older of the two frames and the phase
        // clamps to 0. Measured: average phase 0.28 where an even sweep must
        // average ~0.5, with ~45 slots per second pinned to the previous real
        // frame instead of interpolating.
        double phase = 1.0;
        if (haveTimeline) {
            if (bufferOneFrame) {
                // Content time = now minus the presentation offset, mapped
                // onto the pair's own timestamps. Because the pair is fully in
                // the past, both ends are known and the phase moves at the
                // right speed even when frames arrive unevenly.
                if (presentOffsetMs < 0.0) presentOffsetMs = realIntervalMs * 1.5;

                const double contentTimeMs = NowMs() - presentOffsetMs;
                phase = (contentTimeMs - motionPrevTimestampMs) / realIntervalMs;

                // The clock is corrected against QUEUE DEPTH, not against the
                // phase.
                //
                // Correcting against the phase is what the first version did,
                // and it is self-defeating: the phase jumps from ~1 back to ~0
                // every time a pair is consumed, so the clock was pulled
                // forward and back once per source frame - up to 36 ms per
                // second of adjustment. The output stayed perfectly paced
                // while the CONTENT sped up and slowed down, which is exactly
                // what judder is. Reported on a video, whose source timing is
                // steady to 0.0%, so the cause could only be here.
                //
                // Queue depth does not jump: it rises when the clock runs too
                // slowly and falls when it runs too fast, so a gentle pull
                // toward a target depth holds the content moving at a constant
                // rate.
                constexpr int kTargetQueueDepth = 2;
                constexpr double kClockNudgeMs = 0.02;
                if (queueCount > kTargetQueueDepth) presentOffsetMs -= kClockNudgeMs;
                else if (queueCount < kTargetQueueDepth) presentOffsetMs += kClockNudgeMs;

                const double minOffset = realIntervalMs * 0.6;
                const double maxOffset = realIntervalMs * 3.0;
                if (presentOffsetMs < minOffset) presentOffsetMs = minOffset;
                if (presentOffsetMs > maxOffset) presentOffsetMs = maxOffset;
            } else {
                phase = (NowMs() - motionCurrArrivalMs) / realIntervalMs;
            }
            phase = phase < 0.0 ? 0.0 : (phase > 1.0 ? 1.0 : phase);
        }

        // Close enough to a real frame that generating one would just
        // reproduce it - show the real thing instead. This is also what
        // happens when the source has stopped delivering: phase saturates at
        // 1 and the real frame keeps being shown, which is correct rather
        // than frozen.
        constexpr double kRealFrameEpsilon = 0.04;
        const bool wantGenerated = haveTimeline && !inDegradedMode && !forcePassthroughOnly && !(sourceIsIrregular && !bufferOneFrame) // the buffer is what makes an uneven source usable
            && phase > kRealFrameEpsilon && phase < 1.0 - kRealFrameEpsilon;

        if (haveTimeline) RecordContentStep(motionPrevTimestampMs + phase * realIntervalMs);

        phaseSumForReport += phase;
        ++phaseCountForReport;
        if (haveTimeline) ++timelineSlotsForReport;

        const double presentStartMs = NowMs();
        bool presentedGenerated = false;

        if (wantGenerated) {
            interpolator.SetPhase(static_cast<float>(phase));
            interpolator.SetStatusFlags((maxFactor == 2 ? 1u : 0u)
                | ((transparentRealFrames && presenter.SupportsTransparency()) ? 2u : 0u));
            D3D11_TEXTURE2D_DESC desc{};
            estimator.CurrFrameTexture()->GetDesc(&desc);
            // Straight into the back buffer where possible: writing to our own
            // texture and copying it costs a full-frame copy per presented
            // frame - 14 MB at 2560x1440, at up to 144 frames a second.
            ID3D11UnorderedAccessView* backBufferUAV = presenter.AcquireBackBufferUAV(device.get());

            if (interpolator.GenerateFrame(device.get(), context.get(),
                    estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                    desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, backBufferUAV)) {
                if (backBufferUAV) {
                    presenter.PresentBackBuffer(presentSyncInterval);
                } else {
                    presenter.PresentFrame(context.get(), interpolator.GeneratedFrameTexture(), presentSyncInterval);
                }
                presentedGenerated = true;
            }
        }

        if (!presentedGenerated) {
            if (transparentRealFrames && presenter.SupportsTransparency() && haveMotionField) {
                // Show nothing of ours: the real screen underneath is what the
                // viewer sees, at native quality.
                presenter.PresentTransparent(device.get(), context.get(), presentSyncInterval);
            } else if (estimator.CurrFrameTexture()) {
                presenter.PresentFrame(context.get(), estimator.CurrFrameTexture(), presentSyncInterval);
            } else if (capturedTex) {
                presenter.PresentFrame(context.get(), capturedTex, presentSyncInterval);
            }
        }

        const double presentEndMs = NowMs();
        if (presentedGenerated) ++generatedFramesSinceReport;
        else ++nativeFramesSinceReport;
        RecordPresentGap(presentEndMs);
        RecordPresentAge();
        // Present time is the present alone. A leftover second addition here
        // also counted the slot wait, and reported a "present" of 9.5 ms
        // inside a 7.8 ms iteration - a part larger than the whole, which is
        // how it was caught.
        phasePresentMsSum += presentEndMs - presentStartMs;
        phaseComputeMsSum += computeEndMs - computeStartMs;
        phaseIterationMsSum += presentEndMs - iterationStartMs;
        ++phaseSamples;

        ReportTelemetryIfDue();
    }

    for (auto& t : queueTex) if (t) t->Release();
    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Window closed - shutting down cleanly.");
    capture.Stop();
    ddCapture.Stop();
    // Systemwide setting - give it back, or every other process on the
    // machine keeps paying for our scheduler tick after we are gone.
    if (haveHighResTimer) timeEndPeriod(1);
    return 0;
}
