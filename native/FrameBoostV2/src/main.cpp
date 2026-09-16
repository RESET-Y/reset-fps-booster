// RESET FRAMEBOOST V2
//
// One pair in, one frame out. That is the whole engine.
//
//   A arrives          -> nothing yet, an interpolator needs two
//   B arrives at T_B   -> G(A,B) at phase 0.5 goes out NOW
//                      -> B goes out at T_B + (t_B - t_A)/2
//   C arrives at T_C   -> G(B,C) goes out NOW
//                      -> C goes out at T_C + (t_C - t_B)/2
//
// The presented sequence is therefore A, G(A,B), B, G(B,C), C - strictly
// alternating, strictly forward - and the spacing is half a SOURCE interval,
// computed from the frames' own timestamps. Twice the source rate falls out of
// that arithmetic at any source rate and on any panel; the monitor is where
// the picture lands, never what sets the beat.
//
// The cost is half an interval of latency on the real frame, and it is not
// avoidable: G(A,B) cannot exist before B does, and it has to be shown before
// B or the picture goes backwards. V1 measured that backwards step at -8.8 ms
// and it is what "es fuehlt sich an wie 15 fps" actually was.
//
// WHAT IS DELIBERATELY ABSENT, all of it removed on measurement:
//
//   gap fill          extrapolated past the newest real frame, so the next
//                     generated frame landed BEHIND it. Correlated exactly
//                     with the backwards content steps: in twenty measured
//                     seconds, not one backwards step occurred without it.
//   keep-alive        re-presented a picture already on screen, 52 times a
//                     second into a complete stream. A repeat is not a frame.
//   adaptive factor   aimed the output at the panel instead of at the source,
//                     which is the opposite of the requirement.
//   refresh snapping  tied the whole feature to a refresh rate that divides.
//   still detector    froze the picture for seconds when it guessed wrong.
//
// None of them are coming back. If a frame cannot be generated correctly, the
// real frame goes out on its own and the counter says so.

#include "capture.h"
#include "presenter.h"
#include "telemetry.h"
#include "synthetic.h"
#include "picker.h"
#include "logger.h"

#include "motion_estimation.h"
#include "interpolation.h"

#include <winrt/base.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <sstream>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

using namespace fbv2;
using FrameBoost::MotionEstimation::Estimator;
using FrameBoost::Interpolation::Interpolator;

namespace {

bool HasArg(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (const auto& a : args) if (a == name) return true;
    return false;
}

// "synthetic" on its own means 120; "synthetic 200" means 200. The value is
// the SOURCE rate, so the output to look for is twice it.
double ArgValue(const std::vector<std::wstring>& args, const wchar_t* name, double fallback) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != name) continue;
        try { return std::stod(args[i + 1]); } catch (...) { return fallback; }
    }
    return fallback;
}

double MonitorRefreshHz(HWND window) {
    HMONITOR mon = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(mon, &info)) return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0.0;
    return static_cast<double>(mode.dmDisplayFrequency);
}

// SLEEP UNTIL JUST BEFORE, THEN SPIN.
//
// A bare busy-wait costs a whole core continuously, which is taken from the
// game we are trying to help. A bare Sleep oversleeps by more than a
// millisecond, which at half a source interval is most of the budget. The
// timer covers the bulk and a short spin covers the tail.
void WaitUntil(double dueMs, HANDLE timer) {
    for (;;) {
        const double remaining = dueMs - NowMs();
        if (remaining <= 0.0) return;
        if (remaining > 1.5 && timer) {
            LARGE_INTEGER due{};
            // Negative means relative, in 100 ns units. Half a millisecond
            // short of the target, so the spin below finishes the job.
            due.QuadPart = -static_cast<LONGLONG>((remaining - 0.5) * 10000.0);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, static_cast<DWORD>(remaining) + 2);
        } else if (remaining > 0.3) {
            Sleep(0);
        }
    }
}

// THE GAME, NOT THE APP THAT LAUNCHED US.
//
// GetForegroundWindow() at start-up returns whatever is in front, and what is
// in front is RESET FPS Booster - the user just clicked its button. The
// engine then captured the app, which is exactly what was reported: "sobald
// ich auf Button druecke wird der RFB Window gecaptured".
//
// V1 handled this by waiting five seconds and telling the user to switch. That
// works and asks the person to count. Skipping the windows that cannot be the
// game is better: our own process, the app that started us, and anything with
// no title. Then wait for a real one, with a timeout so a mistake ends in a
// clean exit rather than a hang.
bool IsOwnOrLauncher(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return true;

    winrt::handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
    if (!process) return false;   // cannot tell; assume it is fair game

    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    if (!QueryFullProcessImageNameW(process.get(), 0, path, &len)) return false;

    std::wstring exe(path);
    const size_t slash = exe.find_last_of(L'\\');
    if (slash != std::wstring::npos) exe = exe.substr(slash + 1);
    for (auto& c : exe) c = static_cast<wchar_t>(towlower(c));
    return exe == L"resetfpsbooster.exe";
}

// FIVE SECONDS, THEN WHATEVER IS IN FRONT. The V1 behaviour, restored.
//
// A picker was built first and could not be used - "kann nichts auswaehlen".
// It is still there behind the "picker" argument, but it is not what ships:
// a dialog that cannot be clicked is worse than a countdown that can be
// followed, and this countdown is the one Lukas already knows.
//
// The launcher is still skipped. That part was not the problem - the app is
// in front when its button is pressed, and capturing it was the original
// report - so if five seconds pass and RFB is still in front, this keeps
// waiting rather than capturing the app again.
HWND WaitForGameWindow() {
    constexpr int kAnnounceMs = 5000;
    constexpr int kExtraMs = 15000;
    constexpr int kPollMs = 250;

    Logger::Log("[FrameBoostV2] Switch to the game now - the window in the foreground "
                "in five seconds will be captured.");
    Sleep(kAnnounceMs);

    for (int waited = 0; waited <= kExtraMs; waited += kPollMs) {
        HWND fg = GetForegroundWindow();
        if (fg && IsWindow(fg) && !IsOwnOrLauncher(fg)) {
            wchar_t title[256] = {};
            GetWindowTextW(fg, title, 255);
            if (title[0]) {
                const std::wstring w(title);
                Logger::Log("[FrameBoostV2] Capturing window: "
                            + std::string(w.begin(), w.end()));
                return fg;
            }
        }
        if (waited == 0)
            Logger::Log("[FrameBoostV2] RESET FPS Booster is still in front - waiting for "
                        "the game rather than capturing the app.");
        Sleep(kPollMs);
    }
    return nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    timeBeginPeriod(1);

    // OPT OUT OF ECOQOS, or Windows runs this engine at its convenience.
    //
    // Windows 11 puts processes without a foreground window into efficiency
    // mode: reduced clocks, parked on efficiency cores, timers coalesced. This
    // engine is never the foreground window - the game is - so it qualifies,
    // and the capture callback is a WinRT worker thread inside it.
    //
    // The trace says exactly that. With Apex presenting every 13.889 ms, our
    // arrivals land at a flat 19-22 ms apart, every time:
    //
    //   dWgc = 13.888  dQpc = 19.261
    //   dWgc = 27.777  dQpc = 22.405
    //   dWgc = 13.888  dQpc = 19.683
    //   dWgc = 27.777  dQpc = 22.026
    //
    // Steady at roughly 50 Hz on our side against 72 on the compositor's. A
    // frame-availability problem is ragged; this is a metronome, and a
    // metronome at the wrong rate is a scheduler, not a shortage.
    //
    // Draining the pool was tried first and changed nothing, which rules out
    // frames waiting for us: they are not there when we ask.
    //
    // This asks Windows not to throttle us. It is a request about our own
    // process and nothing else - no priority stolen from the game, no
    // scheduler class raised, no driver touched.
    {
        PROCESS_POWER_THROTTLING_STATE st{};
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;   // 0 = do not throttle
        const BOOL ok = SetProcessInformation(GetCurrentProcess(),
                                              ProcessPowerThrottling,
                                              &st, sizeof(st));
        Logger::Init();
        Logger::Log(ok ? "[FrameBoostV2] EcoQoS opt-out accepted - this process will not be "
                         "throttled for running in the background."
                       : "[FrameBoostV2] EcoQoS opt-out refused; continuing. If arrivals stay "
                         "at ~50 Hz this is worth revisiting.");
    }

    std::vector<std::wstring> args;
    {
        int count = 0;
        if (LPWSTR* raw = CommandLineToArgvW(lpCmdLine, &count)) {
            for (int i = 0; i < count; ++i) args.emplace_back(raw[i]);
            LocalFree(raw);
        }
    }

    // ONE ENGINE AT A TIME.
    //
    // Three were found running at once: started 23:06:01, 23:06:35 and
    // 23:07:56, all capturing the same game and all presenting over each
    // other. The log had stopped at 23:02:32 - the logger appends by opening
    // the file, and when several processes contend for it the open fails and
    // the line is dropped in silence. So the runs happened and nothing wrote
    // them down, which is why two measurements in a row appeared not to exist.
    //
    // A named mutex, not a process scan: the check has to be atomic, or two
    // engines started a second apart both see an empty field and both proceed.
    HANDLE onlyOne = CreateMutexW(nullptr, TRUE, L"Global\ResetFrameBoostV2SingleInstance");
    if (!onlyOne || GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::Init();
        Logger::Log("[FrameBoostV2] Another FrameBoost engine is already running - exiting. "
                    "Two engines capture the same game and present over each other, and "
                    "neither measurement is worth anything.");
        if (onlyOne) CloseHandle(onlyOne);
        return 0;
    }

    Logger::Init();
    Logger::Log("[FrameBoostV2] ==== RESET FRAMEBOOST V2 ====");
    Logger::Log("[FrameBoostV2] One source pair produces exactly one generated frame, "
                "at phase 0.5 of that pair's own timestamps. Output is twice the source "
                "rate whatever the monitor runs at.");

    // The window to follow: whatever is in front when we start, which is the
    // game, because the app launches this from the game.
    // THREE WAYS TO NAME THE WINDOW, in order of how much the user meant it.
    //
    // 1. "hwnd <value>" - RFB passes the handle. Nothing is guessed and no
    //    dialog appears. This is the path the app will use once it can be
    //    rebuilt; it is wired now so that integration is a change there and
    //    none here.
    // 2. the countdown - default. Five seconds to switch to the game, then
    //    whatever is in front, skipping the launcher.
    // 3. "picker" - a list to choose from. Built, then reported unusable
    //    ("kann nichts auswaehlen"), so it is kept and not the default.
    HWND target = nullptr;

    const double handleArg = ArgValue(args, L"hwnd", 0.0);
    if (handleArg > 0.0) {
        target = reinterpret_cast<HWND>(static_cast<uintptr_t>(handleArg));
        if (!IsWindow(target)) {
            Logger::Log("[FrameBoostV2] The handle passed in is not a window - exiting.");
            return 1;
        }
    } else if (HasArg(args, L"picker")) {
        target = PickWindow();
    } else {
        target = WaitForGameWindow();
    }

    if (!target) {
        Logger::Log("[FrameBoostV2] No window selected - exiting rather than "
                    "capturing the wrong thing.");
        return 0;
    }

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 createFlags | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 wanted, ARRAYSIZE(wanted), D3D11_SDK_VERSION,
                                 device.put(), &level, context.put()))) {
        Logger::Log("[FrameBoostV2] D3D11CreateDevice failed - exiting.");
        return 1;
    }

    // WHAT WE ARE ACTUALLY POINTED AT, spelled out.
    //
    // Every measurement so far assumed the window named in the log is the
    // window being captured and that its present timeline is the game's. That
    // has not been checked once. Ruling it out costs six lines.
    {
        wchar_t title[256] = {};
        GetWindowTextW(target, title, 255);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        std::wstring exe;
        if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH] = {};
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
                exe = path;
                const size_t slash = exe.find_last_of(L'\\');
                if (slash != std::wstring::npos) exe = exe.substr(slash + 1);
            }
            CloseHandle(proc);
        }
        RECT wr{}, cr{};
        GetWindowRect(target, &wr);
        GetClientRect(target, &cr);
        HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        const std::wstring t(title), dev(mi.szDevice);
        std::ostringstream id;
        id << "[FrameBoostV2] TARGET hwnd=0x" << std::hex
           << reinterpret_cast<uintptr_t>(target) << std::dec
           << " pid=" << pid
           << " exe=" << std::string(exe.begin(), exe.end())
           << " title=\"" << std::string(t.begin(), t.end()) << "\""
           << " window=" << (wr.right - wr.left) << "x" << (wr.bottom - wr.top)
           << " client=" << (cr.right - cr.left) << "x" << (cr.bottom - cr.top)
           << " at " << wr.left << "," << wr.top
           << " display=" << std::string(dev.begin(), dev.end())
           << " desktop=" << (mi.rcMonitor.right - mi.rcMonitor.left) << "x"
           << (mi.rcMonitor.bottom - mi.rcMonitor.top);
        Logger::Log(id.str());
    }

    const double displayHz = MonitorRefreshHz(target);
    Logger::Log("[FrameBoostV2] Display refresh: " + std::to_string(displayHz)
                + " Hz. Noted for the record only - it does not gate the output rate.");

    // DIAGNOSTIC SOURCE, for the rates no window on this machine produces.
    // Everything downstream is identical - same pairing, same phase, same
    // scheduler, same presenter - so what it measures is the real pipeline.
    const bool syntheticMode = HasArg(args, L"synthetic");
    SyntheticSource synthetic;
    Capture capture;

    if (syntheticMode) {
        const double fps = ArgValue(args, L"synthetic", 120.0);
        if (!synthetic.Init(device.get(), 1280, 720, fps, 400.0)) {
            Logger::Log("[FrameBoostV2] Synthetic source could not start - exiting.");
            return 1;
        }
    } else if (HasArg(args, L"monitor")) {
        // THE CONTROL FOR THE WINDOW PATH, not a shipping mode.
        //
        // "Apex delivers 50" was measured through window capture, and window
        // capture goes through DWM. A game in borderless fullscreen can be on
        // independent flip, where DWM is not composing it at all. Monitor
        // capture takes what reaches the panel instead. Same game, same second,
        // two paths: if this one shows 72 and the window one shows 50, the
        // frames are lost in window capture and the game was never the problem.
        HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTOPRIMARY);
        Logger::Log("[FrameBoostV2] MONITOR capture - measuring the whole display as a "
                    "control against the window path.");
        if (!capture.StartMonitor(mon, device.get(), static_cast<int>(displayHz))) {
            Logger::Log("[FrameBoostV2] Monitor capture could not start - exiting.");
            return 1;
        }
    } else if (!capture.StartWindow(target, device.get(), static_cast<int>(displayHz))) {
        Logger::Log("[FrameBoostV2] Capture could not start - exiting.");
        return 1;
    }

    RECT client{};
    GetClientRect(target, &client);
    const UINT initialW = std::max<UINT>(1, static_cast<UINT>(client.right - client.left));
    const UINT initialH = std::max<UINT>(1, static_cast<UINT>(client.bottom - client.top));

    // MEASURE ONLY: capture and timestamp, present nothing, show nothing.
    //
    // Apex is capped at 72 and arrives at 48 - every frame exactly three
    // display refreshes apart instead of two. Two explanations, pointing at
    // opposite places: the game cannot hold 72 under load, or our overlay and
    // our presents are costing it the difference. V1 had that second failure
    // and it took a day to find ("our own overlay was halving the game's frame
    // rate"), so it is not a theoretical concern.
    //
    // With no overlay and no presents, whatever the source then delivers is
    // what the game does on its own. If it is 72 here and 48 with the engine
    // running, the cost is ours.
    const bool measureOnly = HasArg(args, L"measure");

    Presenter presenter;
    if (measureOnly) {
        Logger::Log("[FrameBoostV2] MEASURE ONLY - capturing and timestamping, presenting "
                    "nothing. Nothing will appear on screen; this measures what the game "
                    "delivers when we are not in its way.");
    } else if (!presenter.Create(device.get(), initialW, initialH, target)) {
        Logger::Log("[FrameBoostV2] Presenter could not start - exiting.");
        return 1;
    }

    Estimator estimator;
    Interpolator interpolator;
    // Half of the pair, always. The phase is 0.5 of the interval the pair
    // itself defines; because that interval is measured rather than assumed,
    // an uneven source moves the generated frame with it instead of leaving it
    // stranded on a grid.
    interpolator.SetPhase(0.5f);

    Telemetry telemetry;
    telemetry.Init(displayHz);
    // ON for the gaming test: the app cannot pass arguments, and the sequence
    // is the only thing that can tell N G N G apart from N N G G. "noseq"
    // turns it off; it is buffered, so it costs one write a second.
    telemetry.EnableSequenceLog(!HasArg(args, L"noseq"));
    capture.EnableTrace(HasArg(args, L"trace"));

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // The pair, carried explicitly. No global "previous frame" and no implicit
    // pairing: every generated frame below names the two it came from.
    bool     havePrev = false;
    uint64_t prevId = 0;
    double   prevContentMs = 0.0;
    uint64_t nextOutputId = 1;

    // THE TWO CLOCKS DO NOT SHARE AN ORIGIN, and the first run said so:
    //
    //   Capture latency (real, avg): -12.312 ms
    //
    // WGC's SystemRelativeTime and QueryPerformanceCounter both count from
    // boot and are close, but not identical, so their difference carries a
    // fixed offset. Pairing is untouched by it - the phase comes from a
    // DIFFERENCE of content stamps and the offset cancels - but the reported
    // latency was nonsense, and a field that reads nonsense is worse than one
    // that is missing.
    //
    // Calibrated against the best case rather than assumed: the smallest
    // arrival-minus-content ever seen is the frame that waited least, so
    // everything above it is real waiting. Self-correcting, and it needs no
    // constant that would be wrong on another machine.
    double epochOffsetMs = 1e12;
    // Measured at the present, not derived from a rate, because the rate is
    // an average and the average is what hides uneven spacing.
    double lastPresentMs = -1.0;

    bool running = true;
    while (running) {
        presenter.PumpMessages();

        MSG peek;
        if (PeekMessageW(&peek, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE)) break;

        CapturedFrame frame{};
        const bool got = syntheticMode ? synthetic.Produce(context.get(), frame)
                                       : capture.Acquire(frame);
        if (!got) {
            // Nothing new. Give the core back rather than spin on it - the game
            // needs it more than we do.
            Sleep(1);
            telemetry.NoteQueue(syntheticMode ? 0 : capture.QueueDepth(),
                            syntheticMode ? 0 : static_cast<int>(capture.Overflows()));
            if (!syntheticMode)
                telemetry.NoteCapture(capture.Acquired(), capture.DupTimestamp(),
                                      capture.DupContent(),
                                      capture.FingerprintCount()
                                          ? capture.FingerprintMsSum() / capture.FingerprintCount()
                                          : 0.0);
            if (telemetry.ReportIfDue()) {
            if (!syntheticMode) capture.ResetCounters();
                const std::string tr = capture.TakeTrace();
                if (!tr.empty()) Logger::Log("[FrameBoostV2][arrivals]\n" + tr);
            }
            continue;
        }

        telemetry.NoteSourceArrival();

        if (measureOnly) {
            // The arrival timeline and nothing else. Recorded through the same
            // sequence log so the content deltas can be read out exactly as
            // they are for a normal run.
            if (havePrev) telemetry.NotePairIntervalMs(frame.contentMs - prevContentMs);
            telemetry.NoteSequence({ nextOutputId++, false, frame.frameId, frame.frameId,
                                     frame.contentMs, 0.0, frame.arrivalMs,
                                     frame.contentMs, frame.contentMs });
            capture.Release(frame);
            havePrev = true;
            prevId = frame.frameId;
            prevContentMs = frame.contentMs;
            telemetry.NoteQueue(capture.QueueDepth(), static_cast<int>(capture.Overflows()));
            if (!syntheticMode)
                telemetry.NoteCapture(capture.Acquired(), capture.DupTimestamp(),
                                      capture.DupContent(),
                                      capture.FingerprintCount()
                                          ? capture.FingerprintMsSum() / capture.FingerprintCount()
                                          : 0.0);
            if (telemetry.ReportIfDue()) {
            if (!syntheticMode) capture.ResetCounters();
                const std::string tr = capture.TakeTrace();
                if (!tr.empty()) Logger::Log("[FrameBoostV2][arrivals]\n" + tr);
            }
            continue;
        }

        presenter.TrackTarget();
        presenter.Resize(frame.width, frame.height);

        // Push B into the estimator. It keeps prev/curr itself, so frames must
        // be fed strictly in order - which is exactly what the ring hands over.
        const bool haveMotion = estimator.ProcessFrame(device.get(), context.get(), frame.texture);

        const uint64_t currId = frame.frameId;
        const double currContentMs = frame.contentMs;
        const double pairIntervalMs = havePrev ? (currContentMs - prevContentMs) : 0.0;

        // A pair is only usable if its two halves are a plausible interval
        // apart. Outside that, the safe answer is the real frame on its own.
        const bool pairUsable = havePrev && haveMotion
                             && pairIntervalMs > 1.0 && pairIntervalMs < 200.0;

        if (pairUsable) {
            telemetry.NotePairIntervalMs(pairIntervalMs);

            D3D11_TEXTURE2D_DESC desc{};
            estimator.CurrFrameTexture()->GetDesc(&desc);

            if (interpolator.GenerateFrame(device.get(), context.get(),
                                           estimator.PrevFrameSRV(),
                                           estimator.CurrFrameSRV(),
                                           estimator.MotionVectorSRV(),
                                           desc.Width, desc.Height,
                                           DXGI_FORMAT_B8G8R8A8_UNORM, nullptr)) {
                const double genContentMs = prevContentMs + pairIntervalMs * 0.5;
                telemetry.NoteGeneratedProduced();
                if (presenter.Present(context.get(), interpolator.GeneratedFrameTexture())) {
                    const double shownAt = NowMs();
                    if (lastPresentMs > 0.0) telemetry.NotePresentInterval(shownAt - lastPresentMs);
                    lastPresentMs = shownAt;
                    telemetry.NoteGenerated(shownAt - frame.arrivalMs);
                    telemetry.NoteSequence({ nextOutputId++, true, prevId, currId,
                                             genContentMs, 0.5, shownAt,
                                             prevContentMs, currContentMs });
                } else {
                    telemetry.NoteDropped();
                }
            } else {
                // Generation failed. The real frame still goes out below - a
                // missing generated frame costs smoothness, a wrong one costs
                // the picture.
                telemetry.NoteDropped();
            }
        }

        // The real frame, half a source interval after its generated partner.
        // Held from the moment that partner went out, not from an absolute
        // grid, so a source that speeds up or slows down carries the spacing
        // with it.
        if (pairUsable) {
            WaitUntil(NowMs() + pairIntervalMs * 0.5, timer);
        }

        if (presenter.Present(context.get(), frame.texture)) {
            const double shownAt = NowMs();
            if (lastPresentMs > 0.0) telemetry.NotePresentInterval(shownAt - lastPresentMs);
            lastPresentMs = shownAt;
            const double rawOffset = frame.arrivalMs - frame.contentMs;
            if (rawOffset < epochOffsetMs) epochOffsetMs = rawOffset;
            const double captureLatencyMs = rawOffset - epochOffsetMs;
            telemetry.NoteNative(captureLatencyMs, shownAt - frame.arrivalMs);
            telemetry.NotePipelineLatencyMs(shownAt - frame.arrivalMs + captureLatencyMs);
            telemetry.NoteSequence({ nextOutputId++, false, currId, currId,
                                     currContentMs, 0.0, shownAt,
                                     currContentMs, currContentMs });
        } else {
            telemetry.NoteDropped();
        }

        if (!syntheticMode) capture.Release(frame);

        havePrev = true;
        prevId = currId;
        prevContentMs = currContentMs;

        telemetry.NoteGpu(estimator.LastGpuTimeMs(), interpolator.LastGpuTimeMs());
        telemetry.NoteQueue(syntheticMode ? 0 : capture.QueueDepth(),
                            syntheticMode ? 0 : static_cast<int>(capture.Overflows()));
        telemetry.NotePresentWaitMs(presenter.WaitMsSum(), presenter.CallMsSum(),
                                    presenter.CallMsMax(), presenter.Presents());
        if (!syntheticMode)
            telemetry.NoteCapture(capture.Acquired(), capture.DupTimestamp(),
                                  capture.DupContent(),
                                  capture.FingerprintCount()
                                      ? capture.FingerprintMsSum() / capture.FingerprintCount()
                                      : 0.0);
        if (telemetry.ReportIfDue()) {
            if (!syntheticMode) capture.ResetCounters();
            presenter.ResetStats();
            const std::string tr = capture.TakeTrace();
            if (!tr.empty()) Logger::Log("[FrameBoostV2][arrivals]\n" + tr);
        }
    }

    Logger::Log("[FrameBoostV2] Shutting down.");
    if (!syntheticMode) capture.Stop();
    if (!measureOnly) presenter.Destroy();
    if (timer) CloseHandle(timer);
    if (onlyOne) { ReleaseMutex(onlyOne); CloseHandle(onlyOne); }
    timeEndPeriod(1);
    return 0;
}
