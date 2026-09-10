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
#include <d3d11.h>
#include <winrt/base.h>
#include <sstream>
#include <utility>
#include <string>
#include <cmath>
#include <functional>

#include "logger.h"
#include "capture_engine.h"
#include "beta_presenter.h"
#include "duplicate_detector.h"
#include "motion_stats.h"
#include "../../FrameBoost/src/motion_estimation.h"
#include "../../FrameBoost/src/interpolation.h"

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

bool CreateSharedDevice(winrt::com_ptr<ID3D11Device>& device, winrt::com_ptr<ID3D11DeviceContext>& context) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // required for Windows Graphics Capture interop
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), &obtained, context.put());
    return SUCCEEDED(hr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HWND targetWindow = ParseTargetWindow(argc, argv);
    std::wstring argv2Storage = (argc >= 3 && argv) ? argv[2] : L"";
    if (argv) LocalFree(argv);

    FrameBoostBeta::Logger::Init();

    if (!targetWindow) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: no target window handle provided on the command line.");
        return 1;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (!CreateSharedDevice(device, context)) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: could not create a BGRA-capable D3D11 device.");
        return 2;
    }

    // "monitor" as the second argument captures the whole monitor the target
    // window sits on, instead of the window itself. Window capture stalls
    // when the window is covered (Windows stops redrawing hidden windows);
    // a monitor is always composited, so this mode keeps receiving frames
    // even with our own output displayed on top of everything.
    bool monitorMode = (argc >= 3 && _wcsicmp(argv2Storage.c_str(), L"monitor") == 0);
    // "monitor2": capture the monitor the target sits on, but display the
    // boosted result on a DIFFERENT monitor. This is the only tested
    // configuration where nothing gets covered, so the source keeps
    // rendering at full speed and the capture never starves.
    bool secondScreenMode = (argc >= 3 && _wcsicmp(argv2Storage.c_str(), L"monitor2") == 0);
    if (secondScreenMode) monitorMode = true;
    HMONITOR targetMonitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
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
    bool captureStarted = monitorMode
        ? capture.StartMonitor(targetMonitor, device.get())
        : capture.Start(targetWindow, device.get());
    if (!captureStarted) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: capture failed to start - target window may be unsupported or closed. Falling back safely (no display, exiting).");
        return 3;
    }

    RECT targetRect{};
    GetClientRect(targetWindow, &targetRect);
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
    FrameBoostBeta::DuplicateDetector duplicateDetector;
    FrameBoostBeta::MotionStats motionStats;
    uint64_t duplicateFramesSinceReport = 0;

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER lastReport{};
    QueryPerformanceCounter(&lastReport);
    uint64_t nativeFramesSinceReport = 0;
    uint64_t generatedFramesSinceReport = 0;
    double lastCaptureMs = -1.0;
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
    bool debugTint = false;
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
    int maxFactor = 4;
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
    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Press F9 (while this window is focused) to toggle pure passthrough vs. frame generation for A/B comparison.");
    // Timestamps of the two real frames the motion field spans, in the same
    // clock as NowMs(). The output is driven from these, not from a counter.
    double phaseSumForReport = 0.0;
    uint64_t phaseCountForReport = 0, timelineSlotsForReport = 0;
    double motionPrevTimestampMs = 0.0;
    double motionCurrTimestampMs = 0.0;
    // Wall-clock moment the newer of the two frames reached us. The phase is
    // measured from here, so capture latency is not counted twice.
    double motionCurrArrivalMs = 0.0;
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
    uint64_t gapSamples = 0, gapMissed = 0;

    auto RecordPresentGap = [&](double presentEndMs) {
        if (lastPresentAtMs > 0.0) {
            const double gap = presentEndMs - lastPresentAtMs;
            gapSumMs += gap;
            gapSumSqMs += gap * gap;
            if (gap < gapMinMs) gapMinMs = gap;
            if (gap > gapMaxMs) gapMaxMs = gap;
            ++gapSamples;
            if (outputSlotMs > 0.0 && std::abs(gap - outputSlotMs) > outputSlotMs * 0.5) ++gapMissed;
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

        const double remainingMs = nextPresentDueMs - NowMs();
        if (remainingMs > 1.5) Sleep(static_cast<DWORD>(remainingMs - 1.0)); // coarse, cheap
        while (NowMs() < nextPresentDueMs) { /* spin out the last fraction */ }

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
        oss << "[FrameBoostBeta] Native FPS: " << nativeFps
            << " | Generated FPS: " << generatedFps
            << " | Output FPS: " << outputFps
            << " | Poll time: " << lastCaptureMs << " ms"
            << " | Capture latency (real, avg): " << (avgLatencyMs >= 0 ? std::to_string(avgLatencyMs) + " ms" : "N/A")
            << " | Stale frames dropped/poll: " << capture.LastDiscardedStaleFrames()
            << " | Duplicate frames skipped/s: " << (duplicateFramesSinceReport / elapsed)
            << " | Frame-to-frame difference: " << duplicateDetector.LastDifference()
            << " | Real frame interval (measured): " << (realFrameIntervalEmaMs > 0 ? std::to_string(realFrameIntervalEmaMs) + " ms" : "N/A")
            << " | Vsync: " << (presentSyncInterval == 0 ? "off" : "on")
            << " | Refresh lock: " << (refreshLockEnabled ? "on" : "off")
            << " | Generation factor: " << generationFactor << "x"
            << " | On-screen age: " << (presentAgeSamples ? std::to_string(presentAgeSumMs / presentAgeSamples) + " ms avg, " + std::to_string(presentAgeMaxMs) + " ms max" : "N/A")
            << " | Phase avg: " << (phaseCountForReport ? phaseSumForReport / phaseCountForReport : -1.0)
            << " | Timeline slots: " << (phaseCountForReport ? 100.0 * timelineSlotsForReport / phaseCountForReport : -1.0) << "%"
            << " | Real interval: " << (motionCurrTimestampMs - motionPrevTimestampMs) << " ms"
            << " | Capture arrivals/s: " << (captureArrivalsSinceReport / elapsed)
            << " | Transparency: " << ((transparentRealFrames && presenter.SupportsTransparency()) ? "on" : "off")
            << " | Moving blocks: " << (motionStats.MovingBlockPercent() >= 0 ? std::to_string(motionStats.MovingBlockPercent()) + "%" : "N/A")
            << " | Motion mean/max px: " << motionStats.MeanMagnitudePixels() << "/" << motionStats.MaxMagnitudePixels()
            << " | Search-saturated blocks: " << motionStats.SaturatedBlockPercent() << "%"
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
        latencySumMs = 0.0;
        latencySamples = 0;
        phaseComputeMsSum = 0.0;
        presentAgeSumMs = 0.0; presentAgeMaxMs = 0.0; presentAgeSamples = 0;
        captureArrivalsSinceReport = 0;
        phaseSumForReport = 0.0; phaseCountForReport = 0; timelineSlotsForReport = 0;
        gapSumMs = 0.0; gapSumSqMs = 0.0; gapMinMs = 1e9; gapMaxMs = 0.0; gapSamples = 0; gapMissed = 0;
        phasePresentMsSum = 0.0;
        phaseIterationMsSum = 0.0;
        phaseSamples = 0;
        lastReport = now;
    };

    while (!presenter.ShouldClose()) {
        double iterationStartMs = NowMs();
        presenter.PumpMessages();
        if (presenter.ShouldClose()) break;

        bool f9IsDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9IsDown && !f9WasDown) {
            forcePassthroughOnly = !forcePassthroughOnly;
            FrameBoostBeta::Logger::Log(forcePassthroughOnly
                ? "[FrameBoostBeta] F9: forcing PURE PASSTHROUGH (no generation) for A/B comparison."
                : "[FrameBoostBeta] F9: frame generation re-enabled.");
            presenter.SetTitleSuffix(forcePassthroughOnly ? L"PASSTHROUGH ONLY (F9 to toggle)" : L"GENERATING (F9 to toggle)");
        }
        f9WasDown = f9IsDown;

        // F6: low-latency mode. Caps the generation factor at 2, so a real
        // frame is held one output slot instead of two before being shown.
        bool f6IsDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6IsDown && !f6WasDown) {
            maxFactor = (maxFactor == 2) ? 4 : 2;
            candidateFactorHeldFrames = 0;
            if (generationFactor > maxFactor) generationFactor = maxFactor;
            FrameBoostBeta::Logger::Log(maxFactor == 2
                ? "[FrameBoostBeta] F6: LOW LATENCY - factor capped at 2x, real frames held one slot instead of two."
                : "[FrameBoostBeta] F6: latency cap released - factor free up to 4x.");
        }
        f6WasDown = f6IsDown;

        bool f7IsDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
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
        bool f8IsDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8IsDown && !f8WasDown && outputSlotMs > 0.0) {
            refreshLockEnabled = !refreshLockEnabled;
            nextPresentDueMs = 0.0; // re-anchor the grid on the next present
            FrameBoostBeta::Logger::Log(refreshLockEnabled
                ? "[FrameBoostBeta] F8: refresh-locked cadence ON (evenly spaced frames)."
                : "[FrameBoostBeta] F8: refresh-locked cadence OFF (free-running).");
        }
        f8WasDown = f8IsDown;

        bool f10IsDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10IsDown && !f10WasDown) {
            presentSyncInterval = presentSyncInterval == 0 ? 1 : 0;
            FrameBoostBeta::Logger::Log(presentSyncInterval == 0
                ? "[FrameBoostBeta] F10: VSYNC OFF (present syncInterval 0)."
                : "[FrameBoostBeta] F10: VSYNC ON (present syncInterval 1).");
        }
        f10WasDown = f10IsDown;

        bool f11IsDown = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11IsDown && !f11WasDown) {
            debugTint = !debugTint;
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

        UINT frameW = 0, frameH = 0;
        int64_t frameTimestamp100ns = 0;
        bool isNewFrame = false;
        ID3D11Texture2D* capturedTex = capture.PollLatestFrame(frameW, frameH, frameTimestamp100ns, isNewFrame);

        LARGE_INTEGER captureEnd{};
        QueryPerformanceCounter(&captureEnd);
        // Poll/retrieval time - how long the (usually already-ready) frame
        // took to fetch, not the true capture latency (see below).
        lastCaptureMs = static_cast<double>(captureEnd.QuadPart - captureStart.QuadPart) / qpcFreq.QuadPart * 1000.0;

        // Real additional latency: how old the frame WGC handed us actually
        // is, measured against the same 100ns-since-boot clock WGC itself
        // uses for SystemRelativeTime - this is the honest number, not the
        // poll time above.
        if (frameTimestamp100ns > 0) {
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
                    realFrameIntervalEmaMs = realFrameIntervalEmaMs < 0.0
                        ? intervalMs
                        : realFrameIntervalEmaMs * 0.8 + intervalMs * 0.2;
                }
            }
            lastFrameTimestamp100ns = frameTimestamp100ns;
            currentPairTimestamp100ns = frameTimestamp100ns;

            // Re-evaluated here, as soon as the measured interval updates,
            // rather than further down the loop: the duplicate-frame path
            // below returns early, so on a static screen the factor would
            // otherwise never be reconsidered and would still read 1x when
            // motion resumes.
            EvaluateGenerationFactor();
        }

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
        if (haveNewContent && duplicateDetector.IsDuplicate(device.get(), context.get(), capturedTex)) {
            ++duplicateFramesSinceReport;
            // Recomposited but unchanged: no new content to estimate from, so
            // it is treated exactly like "no new frame". The slot below still
            // gets presented - skipping the present made the output look
            // frozen on a static screen, which is indistinguishable from a
            // crash to the viewer.
            haveNewContent = false;
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

        if (haveNewContent && !forcePassthroughOnly
                && (!inDegradedMode || degradedFrameCounter >= kDegradedRetryIntervalFrames)) {
            degradedFrameCounter = 0;
            // Motion field and frame timestamps persist across slots: the
            // output is driven by the clock, not by frame arrivals, so several
            // slots interpolate from the same pair of real frames.
            haveMotionField = estimator.ProcessFrame(device.get(), context.get(), capturedTex);
            ranEstimationThisTick = true;
            if (haveMotionField) {
                motionPrevTimestampMs = motionCurrTimestampMs;
                motionCurrTimestampMs = frameTimestamp100ns / 10000.0;
                motionCurrArrivalMs = NowMs();
            }
        }

        if (ranEstimationThisTick) {
            double meGpuMs = estimator.LastGpuTimeMs();
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

        // ---- The output slot ----------------------------------------------
        // Wait for this slot, then decide from the CLOCK what to show in it.
        WaitForOutputSlot();

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
            phase = (NowMs() - motionCurrArrivalMs) / realIntervalMs;
            phase = phase < 0.0 ? 0.0 : (phase > 1.0 ? 1.0 : phase);
        }

        // Close enough to a real frame that generating one would just
        // reproduce it - show the real thing instead. This is also what
        // happens when the source has stopped delivering: phase saturates at
        // 1 and the real frame keeps being shown, which is correct rather
        // than frozen.
        constexpr double kRealFrameEpsilon = 0.04;
        const bool wantGenerated = haveTimeline && !inDegradedMode && !forcePassthroughOnly
            && phase > kRealFrameEpsilon && phase < 1.0 - kRealFrameEpsilon;

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
            if (interpolator.GenerateFrame(device.get(), context.get(),
                    estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                    desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM)) {
                presenter.PresentFrame(context.get(), interpolator.GeneratedFrameTexture(), presentSyncInterval);
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

    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Window closed - shutting down cleanly.");
    capture.Stop();
    return 0;
}
