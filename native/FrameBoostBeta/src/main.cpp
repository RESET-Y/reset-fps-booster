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
#include <algorithm>
#include <iomanip>
#include <vector>
#include <cmath>
#include <functional>
#include <fstream>
#include <map>
#include <cwctype>
#include <cstdlib>

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

    // SCHEDULING: tell Windows this process is latency-critical.
    //
    // Three separate things, and only the last one is a priority.
    //
    // EcoQoS first, because it is the one that actually matches "Windows
    // throttles us in the background". Since Windows 11 the scheduler may park
    // a process on efficiency cores and cap its clock when it has no visible
    // foreground window - which is exactly our situation: the game is in front,
    // our overlay has no title bar and is hidden from the taskbar. Opting out
    // is a documented, per-process switch that changes nothing for anyone else.
    //
    // Then HIGH_PRIORITY_CLASS for the process and TIME_CRITICAL for this
    // thread, which together put the pacing loop at priority 15 - the band
    // media and audio engines run in.
    //
    // NOT realtime, and that is a deliberate refusal rather than caution for
    // its own sake. REALTIME_PRIORITY_CLASS with TIME_CRITICAL is priority 31,
    // above the kernel threads that service input, audio and paging. This loop
    // BUSY-WAITS - "while (NowMs() < dueAtMs) { ddCapture.Pump(); }" appears
    // three times in the pacing path - and a spin at 31 on a core the mouse
    // driver needs is how a machine stops responding entirely. It also needs
    // SeIncreaseBasePriorityPrivilege: without elevation Windows silently gives
    // HIGH instead, so the flag usually does nothing at all, and does damage
    // exactly when it works.
    //
    // "realtime" forces it anyway, for someone who wants to measure the
    // difference on a machine they are willing to hang.
    {
        // EcoQoS opt-out. The struct is looked up dynamically so the binary
        // still runs on Windows 10 builds that lack it.
        typedef BOOL (WINAPI *SetProcessInformationFn)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            if (auto setInfo = reinterpret_cast<SetProcessInformationFn>(
                    GetProcAddress(kernel, "SetProcessInformation"))) {
                PROCESS_POWER_THROTTLING_STATE throttling{};
                throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                throttling.StateMask = 0; // 0 = do not throttle
                setInfo(GetCurrentProcess(), ProcessPowerThrottling,
                        &throttling, sizeof(throttling));
            }
        }

        // NORMAL by default, and that is a retraction.
        //
        // HIGH_PRIORITY_CLASS with a TIME_CRITICAL loop thread went in earlier
        // today on request. No benefit for it was ever measured, and then this
        // was: 102-121% of a core spun at HIGH priority, part of it while the
        // engine was standing aside with output at zero. The pacing loop busy-
        // waits by design, and a busy-wait above the game's own threads is a
        // core the game cannot have. Reported as "die Menue-Buttons bleiben
        // bisschen zurueck" - input handling waiting behind us.
        //
        // A change with no measured benefit that correlates with a reported
        // regression comes out. "highpriority" puts it back for anyone who
        // wants to measure it properly; "realtime" is still there and still a
        // bad idea for the reasons below.
        bool wantRealtime = false;
        bool wantHigh = false;
        for (int i = 1; i < argc; ++i) {
            if (!argv) break;
            if (_wcsicmp(argv[i], L"realtime") == 0) wantRealtime = true;
            if (_wcsicmp(argv[i], L"highpriority") == 0) wantHigh = true;
        }

        const DWORD cls = wantRealtime ? REALTIME_PRIORITY_CLASS
                        : wantHigh     ? HIGH_PRIORITY_CLASS
                                       : NORMAL_PRIORITY_CLASS;
        const BOOL clsOk = SetPriorityClass(GetCurrentProcess(), cls);
        const DWORD actual = GetPriorityClass(GetCurrentProcess());
        // Above normal, not time-critical. Enough to be scheduled promptly,
        // not enough to hold a core against the game.
        SetThreadPriority(GetCurrentThread(),
                          (wantRealtime || wantHigh) ? THREAD_PRIORITY_TIME_CRITICAL
                                                     : THREAD_PRIORITY_ABOVE_NORMAL);

        std::ostringstream prio;
        prio << "[FrameBoostBeta] Scheduling: EcoQoS throttling off, thread TIME_CRITICAL, priority class "
             << (actual == REALTIME_PRIORITY_CLASS ? "REALTIME"
                 : actual == HIGH_PRIORITY_CLASS ? "HIGH"
                 : actual == ABOVE_NORMAL_PRIORITY_CLASS ? "ABOVE_NORMAL" : "NORMAL")
             << (clsOk ? "" : " (the request was refused)")
             << (wantRealtime && actual != REALTIME_PRIORITY_CLASS
                 ? " - realtime was asked for and Windows declined it, which needs elevation." : "");
        FrameBoostBeta::Logger::Log(prio.str());
    }
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

    // SETTINGS THAT SURVIVE A RESTART.
    //
    //     %LOCALAPPDATA%\ResetFpsBooster\frameboost.ini
    //
    // The app launches this engine with no useful arguments and the C# that
    // would pass them cannot be rebuilt here, so a plain file is the only way a
    // choice outlives a session. It replaces the one-file-per-switch markers
    // that grew up ad hoc.
    //
    // Deliberately only the switches that EXIST. There is no mouse scaling
    // factor to persist: the raw-mouse prediction was removed entirely, and the
    // calibration it would have stored never settled - 0.397 in one session,
    // 0.185 in the next, with the raw fit and the smoothed value agreeing to
    // three digits each time, so it was the measurement that disagreed and not
    // convergence. Horizontal picture motion comes from strafing as well as
    // turning, and the mix decides the number. A stored 0.40 would be a
    // constant that was never true.
    //
    // Format is "name = on" / "name = off", one per line, # for comments.
    // Unknown names are ignored rather than rejected, so a file written for a
    // later build does not stop an earlier one from starting.
    std::map<std::wstring, std::wstring> settingsFile;
    {
        const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
        if (localAppData) {
            const std::wstring path =
                std::wstring(localAppData) + L"\\ResetFpsBooster\\frameboost.ini";
            std::wifstream in(path);
            std::wstring line;
            while (in && std::getline(in, line)) {
                const size_t hash = line.find(L'#');
                if (hash != std::wstring::npos) line.erase(hash);
                const size_t eq = line.find(L'=');
                if (eq == std::wstring::npos) continue;
                std::wstring key = line.substr(0, eq), value = line.substr(eq + 1);
                auto trim = [](std::wstring& t) {
                    const wchar_t* ws = L" \t\r\n";
                    const size_t b = t.find_first_not_of(ws);
                    const size_t e = t.find_last_not_of(ws);
                    t = (b == std::wstring::npos) ? L"" : t.substr(b, e - b + 1);
                };
                trim(key); trim(value);
                if (key.empty()) continue;
                for (auto& ch : key) ch = towlower(ch);
                for (auto& ch : value) ch = towlower(ch);
                settingsFile[key] = value;
            }
            if (!settingsFile.empty()) {
                FrameBoostBeta::Logger::Log("[FrameBoostBeta] Settings file read: "
                    + std::to_string(settingsFile.size()) + " entries.");
            }
        }
    }

    // WHAT TO TELL THE USER AT STARTUP.
    //
    // Two settings decide more about how this feels than anything in the
    // engine, and both are in the game rather than here. Measured, not assumed:
    //
    //   The cap must divide the refresh rate. At 60 on a 144 Hz panel the
    //   capture spacing read mean 16.67 ms with min 8.8 and max 27.6 - every
    //   frame waiting two or three compositor ticks. At 72 it reads 13.88 with
    //   a standard deviation under one.
    //
    //   The game must not already be using the whole card. At high detail the
    //   source collapsed to 44-63 fps with the regulator pinned; with detail
    //   lowered the same scene held 72.0 at 98% moving blocks and 0.0% missed
    //   slots.
    //
    // Deliberately says "leave headroom" rather than naming a cause. Lowering
    // resolution and detail both fix it, which is consistent with bandwidth and
    // with raw shader throughput alike, and we have not separated the two. The
    // advice is measured; the mechanism is not, and should not be stated as if
    // it were.
    auto LogStartupAdvice = [&](int displayHz) {
        std::ostringstream advice;
        advice << "[FrameBoostBeta] Two things in the GAME decide most of how this feels:\n"
               << "  1. Cap the game so the cap divides your refresh rate exactly.";
        if (displayHz > 0) {
            advice << " At " << displayHz << " Hz that means ";
            bool first = true;
            for (int div = 2; div <= 4; ++div) {
                if (displayHz % div != 0) continue;
                advice << (first ? "" : ", ") << (displayHz / div);
                first = false;
            }
            advice << " - and NOT 60, unless 60 divides it.";
        }
        advice << "\n  2. Leave the graphics card some headroom - around 75-80% total load,"
                  " not 100%. Frame generation cannot help a game that is already using"
                  " the whole card; it can only take from it.";
        FrameBoostBeta::Logger::Log(advice.str());
    };

    // An argument always wins, so a one-off test never needs the file edited.
    auto Setting = [&](const wchar_t* name) {
        if (HasArg(name)) return true;
        const auto it = settingsFile.find(name);
        if (it == settingsFile.end()) return false;
        const std::wstring& v = it->second;
        return v == L"on" || v == L"1" || v == L"true" || v == L"yes";
    };

    // Numeric settings. Same precedence: an argument beats the file.
    auto SettingInt = [&](const wchar_t* name, int fallback) {
        const auto it = settingsFile.find(name);
        if (it == settingsFile.end() || it->second.empty()) return fallback;
        return _wtoi(it->second.c_str());
    };

    HWND targetWindow = nullptr;
    for (const auto& a : args) {
        wchar_t* end = nullptr;
        uintptr_t value = wcstoull(a.c_str(), &end, 0); // accepts "0x..." or decimal
        if (value && end && *end == L'\0') { targetWindow = reinterpret_cast<HWND>(value); break; }
    }

    // "window": capture the GAME.s window instead of the whole monitor.
    //
    // Monitor capture hands us every recomposition of the desktop, not only the
    // ones the game caused - measured at 62 to 96 duplicates per second that
    // the duplicate detector has to sort out, and every misjudgement there
    // costs a real frame. Capturing the window removes that question entirely:
    // what arrives is what the game drew.
    //
    // It was avoided because Windows stops redrawing a window that is fully
    // covered, and our overlay covers it - observed live as the image freezing
    // until something forced a repaint. But the product this is being compared
    // against does exactly this: it requires windowed or borderless mode and
    // covers the game. So the limitation is evidently avoidable, and the old
    // observation may have had another cause.
    //
    // Waits five seconds and takes whatever is in the foreground then, so the
    // user can start this and alt-tab into the game.
    // WINDOW CAPTURE IS THE DEFAULT NOW. "monitor" opts out.
    //
    // It used to need the "window" flag because window capture stalled as soon
    // as the overlay covered the game: Windows stops drawing what it believes
    // is hidden, which starved the very frames this needs - 32-35 duplicate
    // frames a second and gaps up to 485 ms.
    //
    // That is fixed at its source. The overlay is created one step below opaque
    // (alpha 254), which Windows does not count as an occluder, so the game
    // keeps rendering underneath. Measured through a full day on 2026-09-14:
    // 0 duplicate frames a second against 45-96 on the monitor path, and it is
    // the single largest quality win this engine has had.
    //
    // The default matters because the app launches this engine with no
    // arguments, so whatever is default is what users actually get.
    if (!HasArg(L"monitor") && !targetWindow) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] Window mode: switch to the game now - the window in the"
                                    " foreground in five seconds will be captured.");
        Sleep(5000);
        HWND fg = GetForegroundWindow();
        if (fg) {
            targetWindow = fg;
            wchar_t title[256] = {};
            GetWindowTextW(fg, title, 255);
            std::wstring w(title);
            FrameBoostBeta::Logger::Log("[FrameBoostBeta] Capturing window: "
                + std::string(w.begin(), w.end()));
        } else {
            FrameBoostBeta::Logger::Log("[FrameBoostBeta] No foreground window found - falling back to the"
                                        " whole monitor.");
        }
    }

    g_hotkeysEnabled = HasArg(L"hotkeys");
    // "tint": paint every generated frame red, so it is unmistakable on screen
    // which frames are ours. The one test that settles whether generated frames
    // reach the display as distinct pictures at all.
    // "tint" paints generated frames red; "showocclusion" paints the pixels the
    // occlusion test distrusts green; "showfallback" paints the replacement
    // pixels magenta - which answers whether the region the detector marks is
    // the region the artefact actually occupies.
    // "measureoutput": KNOWN BROKEN - it reports exactly 0.000000 for both
    // real and generated frames while the capture.s own detector reports 18 to
    // 61 on the same motion. Copying both kinds into one scratch texture to
    // stop the detector rebuilding its buffers did not fix it. Do not read
    // anything into its output until that is understood.
    //
    // It is left in place because the question it asks is still the right one,
    // and because the question turned out to be answerable without it: during
    // the same movement the motion field reported 40 to 112 px, and a
    // generated frame is displaced by half of that. A picture shifted by 20 to
    // 56 px is not a copy of its neighbour, so the extra frames do carry real
    // intermediate motion. What is wrong with them is their CONTENT, not their
    // existence.
    //
    // Originally: compares each PRESENTED frame with the one before it,
    // in pixels.
    //
    // Every smoothness metric in this engine so far measures TIME - when a
    // frame was shown, and what timestamp its content claims. None of them
    // has ever checked that a generated frame actually looks different from
    // the real frame beside it. If it does not, the content-step metric still
    // reports a perfect 6.94 ms while the eye sees each picture twice, which
    // is exactly "144 on the counter, feels like half".
    //
    // Forces generation into our own texture rather than straight into the
    // back buffer, because the back buffer cannot be read back.
    // The green badge in the corner, on unless switched off. It is the only
    // way the user can tell that the booster is running without opening a log,
    // and it reports the thing itself: it can only be drawn by a generated
    // frame. "nobadge" hides it, for screenshots and recordings.
    const unsigned int badgeFlag = HasArg(L"nobadge") ? 0u : 4u;

    // "nowarp": the generated frame becomes an exact copy of the previous real
    // frame - phase 0, no motion compensation at all.
    //
    // A diagnostic, not a mode. Doubling survived switching the blend off
    // entirely, which rules out averaging two pictures. What is left is that a
    // single warped picture puts content in the wrong place. If the doubling
    // also survives having nothing warped at all, then it is not the
    // interpolation - and everything we have been adjusting for hours is the
    // wrong suspect.
    const bool noWarpDiagnostic = HasArg(L"nowarp");

    // "cutoff=N": refuse to displace anything moving faster than N pixels per
    // real-frame interval, and cross-fade the two real frames there instead.
    // 0 (the default) leaves it off. A command-line value rather than a shader
    // constant because the right number depends entirely on how fast the
    // content moves, which can only be found by trying it in a running game.
    double motionCutoffPx = 0.0;
    for (const auto& a : args) {
        if (a.rfind(L"cutoff=", 0) == 0) {
            motionCutoffPx = _wtof(a.substr(7).c_str());
            if (motionCutoffPx < 0.0) motionCutoffPx = 0.0;
        }
    }

    // "nogapfill" exists to settle cause, not to configure anything. After the
    // constant-buffer fix the double images were "so gut wie weg", two changes
    // followed - the gap filler, and the unsharp pass off - and they came back
    // worse. Which one did it is a question for an A/B, not for reasoning.
    //
    // "dump": write the generated frame and its two real sources to disk
    // whenever the picture is moving fast. OFF by default, and that is not
    // tidiness - it is correctness.
    //
    // Each dump reads three full 2560x1440 frames back from the GPU and writes
    // 31 MB to disk, which stalls the pipeline for a whole frame. Firing that
    // every two seconds during fast motion put jitter at 3.3 ms in the affected
    // seconds against 0.35 ms otherwise, with up to 3.6% of slots missed -
    // reported as "mal fluessig mal nicht", and it was the diagnostic, not the
    // engine.
    //
    // Worse than the stutter: it corrupts its own evidence. A stall lengthens
    // the interval between the two real frames the next dump interpolates
    // between, so every dumped frame is a harder case than anything that
    // happens in normal play, and the artefacts in it read as worse than they
    // are.
    const bool frameDumpEnabled = HasArg(L"dump");

    // THE REASON "generated" IS NOT EXACTLY "native".
    //
    // Doubling itself is exact - one generated frame per real frame, placed at
    // half the measured interval. The gap filler is a SECOND mechanism: when
    // the source is late it predicts the newest frame forward and puts that in
    // the hole. Those count as generated, and the sum is visible in the log:
    //
    //   11:58:31  native 28.7  generated 41.6  gap fills 12.9   28.7+12.9=41.6
    //   11:58:32  native 37.0  generated 45.0  gap fills  8.0   37.0+ 8.0=45.0
    //
    // So "warum nicht genau x2" has an exact answer, and turning this off gives
    // exactly 2x. The trade is real in the other direction: without it a
    // stalling source leaves the same picture on screen for 70-80 ms, which is
    // what it was written for, and it is already capped at about 55 ms because
    // a prediction drifts further from the truth the longer it runs.
    //
    //     gapfill = off      in frameboost.ini for exactly 2x
    const bool gapFillOff = HasArg(L"nogapfill")
                         || (settingsFile.count(L"gapfill") && !Setting(L"gapfill"));

    const bool measureOutputDiff = HasArg(L"measureoutput");
    // "showblend" paints blue wherever vector validation refused to displace a
    // pixel and fell back to cross-fading the two real frames. The amount of
    // blue is the amount of picture that is being given up to avoid a double
    // image - the number that decides whether the thresholds are right.
    const unsigned int debugTintMode = HasArg(L"showblend") ? 5u
        : HasArg(L"showmotion") ? 4u
        : HasArg(L"showfallback") ? 3u
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
    //
    // "lowlatency" is the same thing under the name it gets asked for. The
    // difference IS latency: interpolation has to hold the newest real frame
    // back half an interval so its generated partner can go out first, and that
    // hold is pure input lag. Extrapolation predicts forward from the newest
    // frame instead and holds nothing back.
    //
    // A FILE TURNS IT ON, because nothing else can reach it.
    //
    // The app launches this engine with no arguments, and the C# that would
    // pass one cannot be rebuilt on this machine - there is no .NET SDK here,
    // only the C++ build tools. A switch that needs a rebuild to reach is not a
    // switch. So the presence of a file is read instead:
    //
    //   %LOCALAPPDATA%\ResetFpsBooster\lowlatency.on
    //
    // Create it and low latency is on at the next start; delete it and it is
    // off. This belongs in the app's own interface and should move there as
    // soon as the C# can be built - it is a way in, not a design.
    // Same mechanism for every file switch: the app passes no arguments and the
    // C# that would pass one cannot be built here.
    auto MarkerPresent = [](const wchar_t* name) {
        const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
        if (!localAppData) return false;
        const std::wstring marker =
            std::wstring(localAppData) + L"\\ResetFpsBooster\\" + name;
        return GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    auto LowLatencyMarkerPresent = [&]() { return MarkerPresent(L"lowlatency.on"); };

    const bool extrapolateMode = Setting(L"extrapolate") || Setting(L"lowlatency")
                              || LowLatencyMarkerPresent();

    // THE AMBER SQUARE, top left, means low latency is on.
    //
    // That is what the shader has always documented it as - "amber: low
    // latency" - but it was wired to "maxFactor == 2" on one of the three
    // present paths and to nothing at all on the other two, so it effectively
    // stopped appearing. An indicator that does not track the thing it names is
    // worse than none: its absence was read as the mode being gone.
    const unsigned int lowLatencyFlag = extrapolateMode ? 1u : 0u;


    FrameBoostBeta::Logger::Init();

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    // "gpupriority=N": -7 to 7, 0 = normal.
    // GPU THREAD PRIORITY, now settable from the file as well as the command
    // line, because the app passes no arguments and the C# cannot be rebuilt
    // here.
    //
    //     gpupriority = 2      in frameboost.ini
    //
    // What it is: IDXGIDevice::SetGPUThreadPriority, -7 to +7, a scheduling
    // HINT for this device's GPU thread. It reserves nothing and guarantees no
    // share - no user-mode Windows API does - so it cannot make this process
    // untouchable, whatever a comment might promise.
    //
    // What it did last time: raised together with REALTIME_PRIORITY_CLASS, it
    // starved the game's input and the menu buttons visibly lagged. That was
    // process priority rather than this call, and the two were changed at once,
    // so this one has never actually been measured on its own.
    //
    // Which is why it is a setting rather than a default. Lukas asked for it
    // after being told twice that it is a hint; that is his call to make, and
    // the honest way to settle it is one value in a file, an A/B in the same
    // scene, and the missed-slot column.
    //
    // Watch for: input lag in the game's menus, and whether src holds its cap.
    // If the game's frames start arriving unevenly while ours go out on time,
    // that is this setting taking from the wrong place - set it back to 0.
    int gpuPriority = SettingInt(L"gpupriority", 0);
    for (const auto& a : args) {
        if (a.rfind(L"gpupriority=", 0) == 0) {
            const int parsed = _wtoi(a.c_str() + 12);
            if (parsed >= -7 && parsed <= 7) gpuPriority = parsed;
        }
    }
    if (gpuPriority < -7) gpuPriority = -7;
    if (gpuPriority > 7) gpuPriority = 7;
    if (gpuPriority != 0) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] GPU thread priority requested: "
            + std::to_string(gpuPriority) + " (a scheduling hint, not a reserved share -"
            " compare missed slots and source FPS against 0 before keeping it).");
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
    // That conclusion was WRONG, and the reason is worth recording: the 44.60
    // was a defect with a one-line fix, not a property of the API.
    // GraphicsCaptureSession.MinUpdateInterval caps capture near 50 frames a
    // second at its default of 0, and at any value below 1 ms; at exactly
    // 1000 us it delivers the display.s full rate. Sunshine and Apollo carry
    // the same fix. 44.60 sits right on the broken value - the measurement
    // was real, the inference from it was not.
    //
    // Re-measured with it set, against the same game on its 72 fps cap:
    //
    //   Windows Graphics Capture  720 produced, 720 retrieved, 0 lost,
    //                             0 duplicates, 13.89 ms mean spacing
    //   Desktop Duplication       722 acquired - just as good, in an audit
    //                             that does nothing else
    //
    // Both are perfect when the loop is idle, and that is the point: in
    // actual use the loop spends 4-10 ms per frame generating and presenting,
    // and only one of these two keeps collecting during it. Desktop
    // Duplication has to be ASKED, and Microsoft states plainly that it
    // "accumulates monitor updates until you request them" and "is not
    // designed to capture every update" - measured at 25 merged frames a
    // second. Windows Graphics Capture DELIVERS, on its own thread, and no
    // longer cares what the main thread is doing.
    //
    // It also removes the duplicate guessing entirely: 35-110 frames a second
    // were being discarded as suspected duplicates, each one a chance to
    // throw away a real frame. WGC reported zero duplicates over ten seconds.
    //
    // "dxgi" on the command line selects Desktop Duplication for comparison.
    bool useDesktopDuplication = monitorMode && HasArg(L"dxgi");

    bool captureStarted = useDesktopDuplication
        ? ddCapture.StartMonitor(targetMonitor, device.get(), context.get())
        : (monitorMode ? capture.StartMonitor(targetMonitor, device.get())
                       : capture.Start(targetWindow, device.get()));

    // Desktop Duplication can be refused outright (another duplication client,
    // a secure desktop). Rather than fail, fall back to the path that has been
    // working all along - halved frame rate is still better than no boost.
    if (!captureStarted && useDesktopDuplication) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] Desktop Duplication could not start - falling back to"
            " Windows Graphics Capture.");
        useDesktopDuplication = false;
        captureStarted = capture.StartMonitor(targetMonitor, device.get());
    }
    // And the other way round: WGC needs Windows 10 1903 and a supported
    // compositor. If it will not start, Desktop Duplication is still a
    // working capture path, just one that loses frames under load.
    if (!captureStarted && !useDesktopDuplication && monitorMode) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] Windows Graphics Capture could not start - falling back"
            " to Desktop Duplication.");
        useDesktopDuplication = true;
        captureStarted = ddCapture.StartMonitor(targetMonitor, device.get(), context.get());
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
    presenter.SetExcludeFromCapture(monitorMode);
    // Room in the present queue, unless asked for the tight pair. See the
    // BufferCount comment in beta_presenter.cpp for the measurement.
    presenter.SetPresentSlack(!Setting(L"lowlatencypresent"));
    if (!presenter.Create(device.get(), initialWidth, initialHeight, L"RESET FRAMEBOOST - BETA", monitorMode ? nullptr : targetWindow)) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] FATAL: could not create the presentation window/swapchain.");
        return 4;
    }
    presenter.SetTitleSuffix(L"GENERATING (F9 to toggle)");

    // The raw mouse tracker and its calibration lived here, and are gone.
    //
    // They fed one extra motion candidate to the interpolation shader: where
    // the camera is being turned, known a whole frame before any rendered
    // picture shows it. The idea is what VR calls reprojection and it is sound.
    //
    // Removed because it was measured to do nothing. With the prediction fully
    // disabled and then enabled, no artefact anyone could see changed: "nein
    // nichts". And the conversion from mouse counts to screen pixels refused to
    // settle between sessions - 0.397 against 0.185, with the raw fit and the
    // smoothed value agreeing to three digits each time, so the measurement
    // itself disagreed rather than lagging. Horizontal picture motion comes
    // from strafing as well as from turning, and the mix decides the number.
    //
    // Git holds the whole implementation, calibration persistence included, if
    // it is ever worth retrying with a conversion that does not have to be
    // fitted against motion the player also produces by strafing.

    FrameBoost::MotionEstimation::Estimator estimator;
    FrameBoost::Interpolation::Interpolator interpolator;
    // Handed over here, not only in the F11 handler - which is dead while the
    // hotkeys are disabled, so "tint" on the command line set a variable that
    // never reached the shader. The one diagnostic that answers "do our frames
    // reach the screen at all" silently did nothing, and its blank result was
    // nearly taken as evidence that they do not.
    interpolator.SetDebugTint(debugTintMode);

    // HALF THE SAMPLING DENSITY FOR THE GENERATED FRAME, and "fullres" opts out.
    //
    // Interpolation became the bottleneck the moment motion estimation stopped
    // being one: 4.9 ms at the median, 9.4-10.1 ms at the 95th percentile
    // against a 6.9-7.9 ms deadline, showing up as waves of 9-14% missed
    // display slots.
    //
    // Framegen - the same job for video in the browser, with a distilled RIFE
    // network - renders its inserted frames at 480 lines by default and
    // upscales. Per megapixel against their published numbers we are already
    // the cheaper of the two (1.33 ms/MP against their 1.81 at 1080p); we just
    // run on nine times their default pixel count. So the resolution is what to
    // spend, not the algorithm.
    //
    // Default on, because the measurement says the deadline is being missed
    // today. "fullres" is there so the two can be compared directly rather than
    // argued about.
    // FULL SAMPLING DENSITY AGAIN. "halfres" opts into the cheap one.
    //
    // Half density was switched on tonight to stop interpolation overrunning
    // its deadline - 4.9 ms median and 9.4-10.1 ms at the 95th percentile
    // against 6.9-7.9 ms. It worked, and it is also visible: the generated
    // frame is computed at half resolution and written to 2x2 squares, which is
    // a nearest-neighbour upscale, not the bilinear one Framegen uses. Reported
    // as the quality looking bad, which is exactly what that trade costs.
    //
    // The reason it is no longer needed as a default: the deadline problem was
    // fixed at its root afterwards. Staging the motion-estimation operands in
    // groupshared took that stage from 3.19 ms to 0.90-1.07 ms, and the quality
    // regulator now runs off missed display slots and backs off on its own when
    // a scene really is too expensive. Paying permanently for a peak the
    // regulator can handle is the wrong trade.
    // MEASURED: FULL DENSITY DOES NOT FIT HERE.
    //
    // Switched back to full resolution when the quality was reported as bad,
    // and the source cap then moved 60 -> 72. Together that is four times the
    // interpolation work on twenty percent more frames, and the measurement is
    // unambiguous:
    //
    //   00:46:21  relief  9.99  interp median 10.40 ms  deadline 6.90  gpu 86.8%
    //   00:46:23  relief  4.89  interp median 10.70 ms  deadline 6.90  gpu 88.8%
    //
    // Against 2.1-3.5 ms at half density. Interpolation at 2560x1440 costs
    // 10-12 ms and the budget is half a source interval, 6.9 ms. It does not
    // fit, and at 86-89% of the card the game that feeds us collapses - source
    // fell to 14-52 fps with up to 100% of output slots missed.
    //
    // The regulator can hide the misses. It cannot give the game its graphics
    // card back. So half density is the affordable configuration and "fullres"
    // opts into the expensive one.
    //
    // The quality complaint is still valid, and half density is not the real
    // answer to it: the 2x2 write is NEAREST NEIGHBOUR, the crudest possible
    // upscale. Framegen renders its inserted frames at 480 lines and upscales
    // bilinearly. Doing the same here needs a half-resolution intermediate and
    // a second, cheap pass - not a one-line change, and the honest next piece
    // of work rather than something to bolt on at midnight.
    // FULL RESOLUTION WITH A CHEAPER FILTER, instead of half resolution with a
    // better one.
    //
    // Half density was affordable - 33-39% of the card in a quiet scene against
    // 86-89% at full - and it was reported back as blur, which is exactly what
    // it is: the generated frame is computed at 1280x720 and resampled, and no
    // filter invents detail that was never computed. The bilinear upscale
    // removed the 2x2 stairs and could not remove that.
    //
    // So the resolution comes back and the saving is taken from the warp
    // filter instead: Catmull-Rom is five bilinear taps per sample, twice per
    // pixel; bilinear is one, twice. Ten fetches become two, on an engine that
    // has twice been measured as limited by memory traffic rather than
    // arithmetic.
    //
    // Reasoning, not measurement, which is why both switches stay: "halfres"
    // for the old density, "catmull" for the old filter. If the numbers say
    // this is worse, all four combinations are one argument away.
    // Also reachable as a file, because the reason to reach for it arrives
    // while the game is running: our share of the graphics card measures 55-82%
    // at full density, and raising the game.s detail level is what makes that
    // share unaffordable. Half density is a quarter of the interpolation work,
    // resampled bilinearly rather than replicated into 2x2 blocks.
    //
    //     %LOCALAPPDATA%\ResetFpsBooster\halfres.on
    const bool halfDensity = Setting(L"halfres") || MarkerPresent(L"halfres.on");
    interpolator.SetInterpScale(halfDensity ? 2u : 1u);
    // CATMULL-ROM IS THE DEFAULT AGAIN. "bilinearwarp = on" takes it away.
    //
    // I switched this to bilinear this morning to afford full sampling
    // density - ten texture fetches per pixel against two, on an engine that
    // is bandwidth-bound. The reasoning was sound and the premise has since
    // expired: interpolation now measures 0.3-0.7 ms, so the ten fetches are
    // affordable and the trade no longer needs making.
    //
    // And the comment beside the warp says plainly what removing it costs,
    // from a measurement made before I arrived at it: "Removing it brought the
    // double images straight back. Bilinear filtering at a fractional position
    // mixes four neighbours, so each of the two warped samples is smeared
    // before they are combined - and two smeared samples that disagree even
    // slightly overlap visibly, where two sharp ones do not."
    //
    // That is exactly the complaint still open. The ghosting logic downstream
    // compares the two warped samples and distrusts the vector where they
    // disagree; smearing both of them first is the one thing that most
    // directly blunts it.
    interpolator.SetWarpFilter(Setting(L"bilinearwarp") ? 0u : 1u);
    FrameBoostBeta::Logger::Log(std::string("[FrameBoostBeta] Generated frames at ")
        + (halfDensity ? "HALF sampling density with a bilinear upscale"
                       : "full sampling density")
        + ", warped with "
        + (Setting(L"bilinearwarp") ? "bilinear (one tap per sample)."
                                    : "Catmull-Rom (five taps per sample)."));
    FrameBoostBeta::DuplicateDetector duplicateDetector;
    // Separate instance, fed the PRESENTED frames in the order they go out, so
    // each comparison is between two consecutive output frames.
    FrameBoostBeta::DuplicateDetector outputDiff;
    double realDiffSum = 0.0, generatedDiffSum = 0.0;
    uint64_t realDiffCount = 0, generatedDiffCount = 0;
    // Both kinds of frame are copied into ONE scratch texture before being
    // measured. The detector rebuilds its buffers whenever the format or size
    // of what it is given changes, and that throws away the previous frame it
    // was going to compare against - so feeding it two different textures in
    // alternation produced a comparison every time against nothing, and a
    // difference of exactly 0.000000 while the capture.s own detector was
    // simultaneously reporting 36.9 and 56.8 on the same motion. A measuring
    // device that reads zero during visible movement is not measuring.
    winrt::com_ptr<ID3D11Texture2D> outputDiffScratch;
    auto MeasureOutputFrame = [&](ID3D11Texture2D* frame, double& sum, uint64_t& count) {
        if (!frame) return;
        D3D11_TEXTURE2D_DESC src{};
        frame->GetDesc(&src);

        if (outputDiffScratch) {
            D3D11_TEXTURE2D_DESC have{};
            outputDiffScratch->GetDesc(&have);
            if (have.Width != src.Width || have.Height != src.Height || have.Format != src.Format)
                outputDiffScratch = nullptr;
        }
        if (!outputDiffScratch) {
            D3D11_TEXTURE2D_DESC desc = src;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, outputDiffScratch.put()))) return;
        }

        // Mip 0 only - the source may carry a chain, the scratch never does.
        context->CopySubresourceRegion(outputDiffScratch.get(), 0, 0, 0, 0, frame, 0, nullptr);
        outputDiff.IsDuplicate(device.get(), context.get(), outputDiffScratch.get());
        sum += outputDiff.LastDifference();
        ++count;
    };
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
    // Frames predicted forward to cover a source that was late. Counted
    // separately from generated frames: they are a different promise, and a
    // rising number means the game is stuttering, not that we are working.
    uint64_t gapFillsSinceReport = 0;
    // Re-presents of the newest real frame, made only because nothing else went
    // out. A screen that stops updating is the worst failure this can have.
    uint64_t keepAlivePresents = 0;
    // Duplicates shown rather than dropped - see the duplicate branch.
    uint64_t duplicatePassthroughs = 0;
    // Last computed overlay visibility. The real decision is made further down
    // the loop than the duplicate branch that needs to read it, so it is kept
    // from the previous iteration - one frame stale at worst, and the only
    // consequence is one present made or skipped at a visibility change.
    bool overlayVisibleLastIteration = false;
    bool presentedAnythingYet = false;
    // Generated frames thrown away because their moment had already passed.
    uint64_t generatedDroppedLate = 0;
    // HOW LATE, not just how many. A count alone cannot tell a frame that
    // missed by a fraction of a millisecond - which is a slack problem - from
    // one that missed by a whole interval, which means the pipeline is running
    // behind the clock it schedules against. Those need opposite fixes, and
    // reasoning about which one it was has already cost a round today.
    double droppedLateByMsSum = 0.0;
    double droppedLateByMsMax = 0.0;
    double slackWhenOnTimeMsSum = 0.0;
    uint64_t slackWhenOnTimeCount = 0;

    // STILL PICTURE: stand aside instead of generating.
    //
    // Menus are the one place this engine does damage and buys nothing. The GPU
    // is idle there, so the headroom guard lets us run; the content is
    // self-similar interface - rows of identical boxes, repeated lines, a
    // full-width highlight strip - which is the worst case block matching has;
    // and a research tree at 72 fps needs no doubling whatever. In flight,
    // where doubling would actually be worth something, War Thunder sits at
    // 22-32 fps and the headroom guard stands us aside anyway.
    //
    // Reported after a long hunt through the wrong causes: "meistens alles was
    // mit Menue zu tun hat".
    //
    // The separation is measured, not guessed. In War Thunder menus the share
    // of moving blocks reads 0.02%, 0.24%, 1.64%, 2.13%; in Apex gameplay it
    // reads 38%, 56%, 62%, 86%. Two orders of magnitude, with nothing in
    // between, so a threshold at 5% does not have to be clever.
    //
    // And the rule is honest beyond menus: when almost nothing on screen is
    // moving, an interpolated frame carries no new information - it can only
    // differ from its neighbours by being wrong. Standing aside there costs
    // nothing that anyone can see.
    //
    // Hysteresis because a menu is not perfectly still - a cursor crosses it, a
    // highlight animates - and flipping between generating and not at those
    // moments would be its own artefact.
    // Whether we are currently doing the work that needs the priority.
    bool holdingHighPriority = true;
    bool pictureIsStill = false;
    // A running average, not a run of consecutive frames.
    //
    // Counting consecutive frames was the wrong instrument for a value that
    // jumps. Measured in a War Thunder sortie, one second apart: 6.7, 4.1,
    // 25.6, 31.8, 2.8, 0.9, 25.0, 0.2 percent of blocks moving. Any frame under
    // the lower line reset the resume counter, so thirty-six in a row above the
    // upper one never happened and the booster stayed switched off through
    // active gameplay - reported as "deaktiviert sich manchmal von selbst und
    // kommt dann wieder".
    //
    // The clean two-orders-of-magnitude separation this was built on came from
    // Apex against menus. It does not hold in a game whose frames arrive
    // irregularly, where the share of moving blocks swings by a factor of a
    // hundred between one second and the next. An average is immune to that in
    // a way a run of consecutive samples can never be.
    double movingEma = -1.0;
    // How much the picture itself changed, smoothed the same way. See where the
    // still detector uses it: the share of moving BLOCKS alone cannot tell a
    // menu from a slow scene, and this can.
    double diffEma = -1.0;
    // When the still state last flipped. A minimum dwell is what actually stops
    // the flapping - see where it is enforced.
    double lastStillFlipMs = 0.0;

    // THE STILL DETECTOR IS OFF BY DEFAULT. "stillguard" turns it back on.
    //
    // It was fixed twice in one evening - the frame-difference test, then a
    // one-second dwell - and it still switched the booster off for seconds at a
    // time in live gameplay:
    //
    //   23:54:30  out   0.0  standing aside
    //   23:54:31  out   0.0  standing aside
    //   23:54:32  out   0.0  standing aside
    //   23:54:33  out  55.8  generating   jitter 518.88 ms
    //   23:54:36  out  13.9  standing aside
    //   23:54:37  out 100.8  generating   jitter  98.59 ms
    //
    // Three seconds of nothing, half a second of jump, off again. THAT is the
    // stutter that was reported all evening, and every change made tonight was
    // aimed at jitter of two milliseconds while this sat in the same log at
    // five hundred.
    //
    // What it is for - not inventing a frame between two identical ones in a
    // menu - is real but small. What it costs when it is wrong is a frozen
    // picture. It has now been wrong in every session it has been measured in,
    // under three different threshold schemes, because the question it asks
    // cannot be answered reliably from block motion and frame difference alone.
    //
    // Off until something answers it correctly. The menu artefacts it used to
    // prevent are worth less than this.
    const bool stillGuardEnabled = Setting(L"stillguard");
    // On unless switched off - see scheduleAnchorMs.
    const bool smoothClockEnabled = !(settingsFile.count(L"smoothclock")
                                      && !Setting(L"smoothclock"));
    uint64_t stillSecondsSinceReport = 0;
    int gapFillsInARow = 0;
    static constexpr int kMaxGapFills = 8;
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
    // How much later the real frame actually appears than the moment its own
    // schedule names. Seeded below any plausible value so the first sample
    // sets it outright.
    double presentBiasEmaMs = -1000.0;

    // REMOVED: a self-correcting term that shifted the generated frame by half
    // the measured asymmetry between the two spacings.
    //
    // The asymmetry is real - both frames hang on the capture clock, their
    // spacings sum to exactly one source period, and they split 7.19 / 9.61
    // instead of 8.4 / 8.4, identically across every second. Measuring the
    // result and removing it looked safer than guessing at the cause a fourth
    // time.
    //
    // It was much worse live - "jetzt haengt alles hinterher". Delaying the
    // generated frame to centre it in its pair adds latency to the half of the
    // stream that was on time, which is felt immediately, while the evenness
    // it buys is not. The asymmetry costs less than the correction for it.
    //
    // Whatever causes the 1.2 ms has to be found rather than compensated.
    double generationCostEmaMs = -1.0;

    // HEADROOM CHECK: does this machine actually have time to do the work?
    //
    // A generated frame is due half a source period after the real one it
    // follows, and that deadline is not the same on every machine. At 72 fps on
    // a 144 Hz panel it is 6.9 ms; at 120 fps on a 240 Hz panel it is 4.2 ms,
    // and 120 of them have to be produced every second rather than 72. The same
    // engine that is comfortable here can be hopeless there, and the user has
    // no way to tell which - they only see stutter and blame the tool.
    //
    // So the cost is kept as a distribution, not an average. The average is
    // what the guard uses to decide whether to stand aside, but the question
    // "will this work for you" is decided by the BAD frames: an engine that
    // makes its deadline 90% of the time misses it 12 times a second.
    //
    // Content-dependent, so it has to be measured on content - the same
    // lesson that cost two wrong verdicts today, both taken on an idle
    // desktop where the expensive paths are never entered.
    // TWO constraints, and they are not the same question - which the first
    // version of this check got wrong, and said so loudly: it reported "NOT
    // ENOUGH" on a machine that had just been described as very smooth.
    //
    // The DEADLINE belongs to interpolation alone. Motion estimation runs once
    // per real frame, when that frame arrives - it is finished long before the
    // generated frame is due. Charging it against the half-period deadline
    // counts work that happens in the other half.
    //
    // THROUGHPUT is both of them together, against the whole period: every
    // source frame costs one estimation and one interpolation, and whatever
    // that adds up to is taken from the game.
    static constexpr int kCostHistorySize = 600; // ~8 s of generated frames
    double costHistory[kCostHistorySize] = {};      // estimation + interpolation
    double interpHistory[kCostHistorySize] = {};    // interpolation alone
    int costHistoryCount = 0;
    int costHistoryNext = 0;

    // Returns the verdict as a line meant for a human, not for a log reader.
    bool gpuHasRoom = true;
    double gpuRoomVerdictSinceMs = 0.0;
    double lastInterpolationRunMs = 0.0;
    // Long enough that a swing in the measured interval cannot toggle the
    // overlay, short enough that a game genuinely out of GPU is left alone
    // quickly.
    // Two seconds, up from half.
    //
    // The guard was switching the booster off and on repeatedly - measured
    // within thirty seconds: "no room" at 13.0 ms, "room again" at 5.1 ms,
    // then a still-picture stop and a resume. Each flip shows or hides the
    // overlay, and the player sees the feature turning itself off. Meanwhile
    // the engine's own headroom verdict in the same log line read "TIGHT -
    // interpolation 0.9 ms median against a 17.6 ms deadline, 0.2% late". We
    // were meeting the deadline essentially always and standing aside anyway.
    //
    // Half a second is not long enough to outlast a burst of contention, and
    // this is a guard against a sustained condition, not a momentary one.
    static constexpr double kGpuRoomHoldMs = 2000.0;

    // Standing aside has to mean getting out of the way COMPLETELY.
    //
    // Measured in Watch Dogs with the new guard active: output dropped to 0
    // frames per second while the overlay stayed on screen, holding its last
    // picture over a game that was still running underneath. Pausing generation
    // without hiding the window is worse than anything it was meant to prevent.
    //
    // Hiding it also costs nothing: no capture processing reaches the screen, no
    // present, no latency - the player simply sees their game.
    // The overlay is excluded from capture by the presenter itself, which
    // already carries the reasoning and the monitor-only condition. A second
    // call was added here earlier today in ignorance of that; it is gone.

    // NEVER SHOW THE OVERLAY - a diagnostic, not a mode anyone should run.
    //
    //     nooverlay = on      in frameboost.ini
    //
    // Window capture on CS2 delivers frames that are byte-identical while the
    // game renders - proved with the dump, 114,012 sampled offsets and zero
    // differences - and the source rate reads 34 for a game capped at 72,
    // which is half. Both are what a game does when Windows tells it its
    // window is covered: DXGI reports the swapchain occluded and presentation
    // throttles.
    //
    // The only thing covering that window is this overlay. There is already a
    // guard against exactly this - alpha 254 rather than 255, so the window is
    // not counted as an opaque occluder - but it predates the
    // DirectComposition swapchain and has never been re-tested against it.
    //
    // With this on, everything runs except the presenting: capture, duplicate
    // detection, estimation, generation, telemetry. If native FPS then climbs
    // to the source rate and the frame-to-frame difference stops reading 0.02,
    // the overlay is what kills the capture and the fix is in this file. If
    // nothing changes, it is CS2's presentation path and no amount of work
    // here will reach it.
    const bool overlaySuppressed = Setting(L"nooverlay");
    if (overlaySuppressed) {
        FrameBoostBeta::Logger::Log("[FrameBoostBeta] DIAGNOSTIC: the overlay will never be shown."
            " Nothing will appear on screen - this measures whether our own window is what stops"
            " the capture from updating.");
    }

    bool overlayHidden = false;
    auto SetOverlayVisible = [&](bool visible) {
        if (overlaySuppressed) visible = false;
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
    // OFF by default. "refreshlock" brings it back.
    //
    // This snapped every present to the display grid, and a second mechanism -
    // WaitForDisplaySlot - held a minimum spacing on top of it. Both are
    // estimates of when the display will next be ready, written before the
    // swapchain could be asked directly.
    //
    // It can be asked directly now: FRAME_LATENCY_WAITABLE_OBJECT with a
    // maximum frame latency of 1 blocks until the display is actually ready.
    // Two mechanisms both deciding the right moment can only get in each
    // other.s way, and the grid is the one that is provably wrong whenever the
    // output rate does not divide the refresh rate - 120 frames on a 144 Hz
    // panel means every frame waits either one boundary or two, alternating
    // forever, which is judder we create ourselves by trying to be neat.
    //
    // Tested live with both off: "vieeeel besser".
    bool refreshLockEnabled = (outputSlotMs > 0.0) && HasArg(L"refreshlock");
    {
        std::ostringstream oss;
        oss << "[FrameBoostBeta] Display refresh: "
            << (outputRefreshHz > 0 ? std::to_string(outputRefreshHz) + " Hz" : "unknown")
            << " | Refresh-locked output cadence: "
            << (refreshLockEnabled ? std::to_string(outputSlotMs) + " ms per slot" : "disabled")
            << " | F8 toggles the lock.";
        FrameBoostBeta::Logger::Log(oss.str());
    }
    // Said once per start, after the refresh rate is known so the advice can
    // name the caps that actually divide it.
    LogStartupAdvice(static_cast<int>(outputRefreshHz + 0.5));
    bool f8WasDown = false;

    // F7: transparency mode, or "opaque" on the command line to start without
    // it.
    //
    // Defaults ON because it addresses a problem measurement could not
    // otherwise solve - a covered source stops being drawn by Windows. But it
    // has a consequence that was never weighed against that: in this mode WE
    // DO NOT SHOW THE REAL FRAMES AT ALL. The overlay simply turns
    // transparent and lets the game.s own window through, so half the output
    // stream - every second frame - appears when the GAME presents it, on the
    // game.s clock, not on the schedule this engine works so hard to keep.
    //
    // Which means the spacing telemetry measures when we PRESENT, not when
    // the picture actually changes: 7.21 and 7.23 ms of perfectly matched
    // spacing can sit on top of a real frame that appeared whenever it liked.
    // A competing product that draws both kinds of frame itself was tried
    // side by side on the same scene and was clearly better, which is what
    // made this worth questioning.
    bool transparentRealFrames = !HasArg(L"opaque");
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
    // Filled once per report from the capture engine - see the telemetry below.
    FrameBoostBeta::CaptureEngine::IntervalStats captureIntervals{};

    // WATCHDOG: NOTICE WHEN THE CAPTURE HAS SIMPLY STOPPED.
    //
    // Measured this morning, forty-five seconds of it:
    //
    //   09:46:37  src 52.8  native 0.0  out 0.0
    //             produced/retrieved 0/0  lost 0  reconnects 0
    //
    // Nothing produced, nothing retrieved, nothing lost - Windows Graphics
    // Capture had stopped delivering entirely, and the engine sat there with a
    // frozen source estimate showing an output of zero. The "Capture
    // reconnects" counter that would have said so belongs to the Desktop
    // Duplication path, which is not the one in use; WGC had no recovery at
    // all, and nothing even logged it.
    //
    // It happens for ordinary reasons - the window minimised, the game
    // toggling exclusive fullscreen, a driver reset - and the capture item
    // becomes invalid without the frame pool raising anything we watched for.
    //
    // So: if the produced count has not moved for two seconds while we believe
    // we are capturing, stop and start again. Two seconds is far longer than
    // any legitimate gap (a static menu still produces frames at the source
    // rate) and short enough not to be sat through.
    uint64_t watchdogLastProduced = 0;
    double watchdogLastProgressMs = 0.0;
    uint64_t captureRestarts = 0;
    // Seconds the capture was running and produced nothing at all. See the
    // comment at haveProducedCount: these used to be reported as a healthy
    // source frame rate, so they had no visible trace whatsoever.
    uint64_t captureStallSeconds = 0;
    // Arrivals the loop stepped over, summed per report. The existing
    // "Stale frames dropped/poll" is a snapshot of the LAST poll only - it
    // read 1 through eight seconds of the engine serving a 72 fps source at
    // 38, which is 34 frames a second stepped over, and a per-poll 1 is not
    // a number anyone reads as that.
    uint64_t staleFramesSinceReport = 0;
    // When frames keep arriving but every one of them is a duplicate.
    double staleSurfaceSinceMs = 0.0;
    double lastStaleRebuildMs = 0.0;
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

    // WOULD THIS FRAME PUT THE PICTURE BACK IN TIME?
    //
    // The step backwards was measured for weeks and only reported. In the
    // scene Lukas calls a drop it is not rare:
    //
    //   23:50:11  irregular 38.3%  step sd 2.38  backwards 1
    //   23:50:12  irregular 30.7%  step sd 4.81  backwards 2  missed 13.3%
    //   23:50:13  irregular 43.5%  step sd 7.24  backwards 7  missed  8.3%
    //
    // Seven times in one second the next frame shown carried content from
    // BEFORE what was already on screen. That is not a missed slot - the
    // picture visibly jumps back and then forward again, which is what a drop
    // feels like.
    //
    // It happens when the source interval is uneven: the phase is placed
    // against an expected interval, the real frame then arrives early or late,
    // and the next pair.s generated frame lands behind the one just shown.
    //
    // Showing it cannot help. A frame from the past adds no information the
    // viewer has not already had, and it costs the jump twice - back, then
    // forward. Skipping costs one output slot. Framegen, which adapts its
    // factor rather than insisting on one, says the same thing in its own
    // terms: take the factor the GPU and compositor actually sustain. Where we
    // cannot sustain 2x cleanly, the honest answer is 1x for that pair.
    //
    // Deliberately only applied to GENERATED frames. A real frame is the truth
    // and is shown whatever its timestamp says; suppressing one would hold back
    // the newest picture the game has produced.
    // A SMALL STEP BACKWARDS IS INVISIBLE; THE GAP IT LEAVES IS NOT.
    //
    // The first version skipped on any backwards step at all, and the counters
    // showed immediately what that costs. Standing still, with 0.08-1.83% of
    // blocks moving:
    //
    //   23:57:21  gap fills 5.0/s   skipped backwards 5.0/s
    //   23:57:22  gap fills 5.9/s   skipped backwards 5.9/s
    //   23:57:23  gap fills 6.0/s   skipped backwards 6.0/s
    //
    // Identical, row after row: every frame skipped tears a hole in the output
    // that the gap filler then plugs with a repeat. Five or six times a second,
    // exactly where Lukas reports the frame rate feeling inconsistent when he
    // stops moving.
    //
    // The error was treating the sign as the thing that matters. What matters
    // is the SIZE: a frame a fraction of a millisecond behind the screen shows
    // a picture the eye cannot tell from the right one, while the empty slot
    // left by skipping it is a real repeated frame.
    //
    // A fifth of a source interval - about 2.8 ms at 72 fps - is the line. Below
    // it, show the frame; the content has moved less than a fifth of what one
    // real frame moves, which is inside what the interpolation itself rounds.
    // Above it, the jump back is worth more than the gap.
    //
    // This is also why the source matters here: standing still, src swings
    // between 60.2 and 81.4 while nothing on screen moves. The phase is placed
    // against an interval that noisy, so small backwards steps are constant and
    // meaningless.
    // The interval is a parameter rather than a capture: lockedPeriodMs is
    // declared below this point, and the two call sites both have it.
    // ...AND ONLY WHERE THE PICTURE IS ACTUALLY CHANGING.
    //
    // The size threshold was not enough. Standing still, with 0.02-0.93% of
    // blocks moving, the counters still ran together:
    //
    //   00:00:47  mv 0.15%  diff  0.74  gap fills 4.0/s  skipped 4.0/s
    //   00:00:50  mv 0.28%  diff  0.96  gap fills 6.0/s  skipped 6.0/s
    //   00:00:54  mv 0.17%  diff  1.49  gap fills 9.0/s  skipped 9.0/s
    //
    // against fast movement, which is completely clean:
    //
    //   00:00:59  mv 62.54%  diff 14.85  gap fills 0.0/s  skipped 0.0/s
    //   00:01:08  mv 55.73%  diff 28.51  gap fills 0.0/s  skipped 0.0/s
    //
    // Standing still is the BAD case, not the easy one. When nothing changes,
    // frames get skipped as duplicates, the pairs then span uneven stretches of
    // time, and the phase arithmetic stops producing content moments that climb.
    //
    // But look at what the difference column does across those two blocks:
    // 0.74-1.49 standing still against 14.85-28.51 moving. Where the difference
    // is that small the two frames ARE nearly the same picture, so a step
    // backwards cannot be seen - there is almost nothing to see differently.
    // The gap left by skipping it can: it is a repeated frame, nine times a
    // second.
    //
    // So the guard applies only where a wrong order would actually show. Three
    // sits between the two measured populations with a factor of two below and
    // five above.
    auto ContentWouldGoBackwards = [&](double shownContentMs, double periodMs,
                                       double frameDifference) {
        if (lastShownContentMs <= 0.0) return false;
        const double back = lastShownContentMs - shownContentMs;
        if (back <= 0.0) return false;
        if (frameDifference < 3.0) return false;
        const double interval = (periodMs > 1.0 && periodMs < 100.0) ? periodMs : 13.89;
        return back > interval * 0.2;
    };
    // Generated frames skipped because their content sat behind the screen.
    uint64_t generatedSkippedBackwards = 0;
    uint64_t phaseCountForReport = 0, timelineSlotsForReport = 0;
    double motionPrevTimestampMs = 0.0;
    double motionCurrTimestampMs = 0.0;

    // A STEADY CLOCK TO SCHEDULE AGAINST, instead of every arrival's jitter.
    //
    // Output timing is anchored to the capture timestamp of each real frame.
    // Those timestamps are not evenly spaced - measured arrival spacing has a
    // standard deviation of 2.9-4.7 ms around a 13.9 ms mean - so every wobble
    // in the source is handed straight through to the display. Output jitter
    // measures 2-4 ms, which is the same wobble arriving on the other side.
    //
    // This is a model of when a frame SHOULD arrive, advanced by the pacing
    // interval each time and pulled slowly toward what actually happened. A
    // tenth of the error per frame: fast enough to follow a real rate change
    // within a dozen frames, slow enough that a single late arrival moves the
    // schedule by a fraction of a millisecond instead of a whole one.
    //
    // It resynchronises outright when the error passes half an interval,
    // because past that point the source has genuinely changed rate or
    // stalled, and a clock that insists on its model through a stall is worse
    // than no clock at all. That is the failure mode this kind of filter has,
    // and the guard against it is the reason it can be trusted here.
    //
    // Used ONLY for deciding when to present. The content timeline - what
    // moment a frame represents, which the interpolation phase and the content
    // step are measured against - stays on the real timestamps, because that
    // is a statement about the pictures and must not be modelled.
    //
    // "smoothclock = off" in frameboost.ini disables it.
    double scheduleAnchorMs = 0.0;
    uint64_t scheduleResyncs = 0;
    int scheduleMissRun = 0;

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
    // Fractional carry for adaptive output - see outputPerReal below. Holds
    // the part of an output frame this real frame earned but could not spend,
    // which is what turns 2.4 into 2,2,3,2,2,3 instead of a flat 2.
    double outputCredit = 0.0;
    uint64_t adaptiveExtraFrames = 0;
    // Persists BETWEEN iterations. The credit may only advance once per real
    // frame; the count it produces has to survive the iterations in between.
    int adaptiveOutputPerReal = 2;
    // WHY A REAL FRAME PRODUCED NO GENERATED FRAME. Every one of these was a
    // silent early-out, which is how "generated 4 at a 60 fps source" could sit
    // in the log with Doubling on, COMFORTABLE headroom and nothing else to
    // point at. Reasoning about it from the surrounding numbers produced two
    // contradictory explanations and no answer; naming the branch is cheaper.
    uint64_t skipNoMotionField = 0, skipNoGpuRoom = 0, skipDegraded = 0;
    uint64_t skipStill = 0, skipFactorOne = 0, skipGenerateFailed = 0;
    // How far the per-pixel search has to back off to make its deadline.
    //
    // Driven by the SAME measurement the headroom verdict reports, so the
    // engine acts on what it already knows instead of only complaining about
    // it. Adjusted slowly: quality that oscillates is worse than quality that
    // is merely lower, because the eye notices the change more than the level.
    // THE FRACTION OF DISPLAY SLOTS THAT WENT OUT EMPTY, smoothed.
    //
    // This is the ground truth the quality regulator was missing. It is not a
    // percentile against a derived deadline - it is the count of frames that
    // did not reach the screen. Fed in below at the point each output gap is
    // measured; about 100 slots of memory, which is 0.7 s at 144 Hz.
    double missedSlotEma = -1.0;
    // When the relief regulator last stepped. It is rated per SECOND, not per
    // call - see UpdateQualityRelief.
    double lastReliefStepMs = 0.0;

    double qualityRelief = 1.0;

    auto UpdateQualityRelief = [&]() {
        if (costHistoryCount < 60 || lockedPeriodMs <= 1.0) return;

        // DRIVEN BY MISSED DISPLAY SLOTS, WHICH IS WHAT LATENESS ACTUALLY IS.
        //
        // This was pinned at 1.0 - measured and reported, driving nothing -
        // because three criteria in a row said "too slow" while the engine was
        // delivering 144 frames a second with zero missed slots. The percentile
        // against 70% of the deadline, the percentile against the deadline, and
        // the fraction of interpolations longer than the deadline: each backed
        // off from a problem that was not happening, and backing off switches
        // off the per-pixel search that visibly improves edges.
        //
        // The note left here said what was needed: "a measure of lateness that
        // is actually about lateness". This is it. Not a percentile against a
        // deadline derived from the source period, but the count of output
        // slots that went out empty - frames that did not reach the screen.
        //
        // Why it is needed now: staging the motion-estimation operands in
        // groupshared took that stage from 3.19 ms to 0.90-1.07 ms, and moved
        // the bottleneck rather than removing it. Interpolation now reads 4.9 ms
        // at the median and 9.4-10.1 ms at the 95th percentile against a 6.9-7.9
        // ms deadline, and the log shows what that costs in waves:
        //
        //   23:33:09  src 63.8  out 127.0  jitter 2.18  missed 12.6%  age 7.1
        //   23:33:20  src 58.8  out 128.5  jitter 1.97  missed 13.7%  age 10.3
        //
        // Against 0.35-0.50 ms of jitter and 0% missed in between. Every ninth
        // to eleventh frame not arriving is exactly what reads as a stutter.
        //
        // The tolerance this scales is the one gating the per-pixel search: at
        // relief 1 every pixel whose incumbent vector disagrees pays for nine
        // residuals; at 4 only badly broken pixels do. A slightly worse pixel
        // shown on time beats a better one that is not shown at all.
        //
        // RATED PER SECOND, NOT PER CALL.
        //
        // The first version stepped 1.02 up and 0.995 down per call, on the
        // assumption this runs once per frame - about 72 times a second. It
        // does not; it sits in the pacing loop. Working backwards from the log,
        // where the relief fell from 24 to 1 inside one second, 0.995 has to
        // have been applied more than six hundred times:
        //
        //   23:46:26  rel 24.0
        //   23:46:31  rel  1.0
        //   23:46:41  rel 24.0
        //   23:46:44  rel  1.0
        //
        // So "up quickly, down deliberately" was in fact a switch slamming
        // between 1 and the ceiling every second or two - and at 24 the
        // per-pixel search is effectively off, so the visible quality was
        // flapping on and off for no reason the picture could show.
        //
        // Tied to the clock instead: about 1.6x per second upward, 0.7x per
        // second down, whatever the call rate happens to be. From 1 to 24 takes
        // roughly seven seconds of sustained dropping, and back again about
        // nine seconds of none.
        // NowMs is declared below this lambda, so the counter is read directly.
        LARGE_INTEGER qpcRelief{};
        QueryPerformanceCounter(&qpcRelief);
        const double nowReliefMs =
            static_cast<double>(qpcRelief.QuadPart) / qpcFreq.QuadPart * 1000.0;
        if (lastReliefStepMs <= 0.0) lastReliefStepMs = nowReliefMs;
        const double reliefDtSec = (nowReliefMs - lastReliefStepMs) / 1000.0;
        lastReliefStepMs = nowReliefMs;

        // AIMD: MULTIPLICATIVE BACKOFF, ADDITIVE RECOVERY.
        //
        // Multiplicative in both directions hunts, and the log shows it doing
        // exactly that - 24.00, 21.64, 15.14, 14.33, 9.99, 6.99, 4.89 over six
        // seconds while the interpolation median climbed back 1.0, 8.4, 9.8,
        // 10.4, 10.6, 10.7. It backs off, the misses stop, it relaxes, they
        // return. A controller whose recovery is as fast as its retreat cannot
        // settle.
        //
        // Framegen describes its own as an "AIMD controller driven by a
        // leaky-bucket drop detector" - Framegen, not Lossless Scaling, which
        // this comment credited at first - and AIMD is the shape this needs:
        // congestion is answered fast because frames are being lost now,
        // recovery is slow because nothing is being lost and there is no hurry.
        // It is the rule that makes TCP converge instead of oscillate.
        //
        // 1.6x per second up; one unit per second down, so 24 back to 1 takes
        // twenty-three seconds instead of three.
        // A SLOT CAN GO OUT EMPTY BECAUSE WE WERE SLOW, OR BECAUSE THE SOURCE
        // SENT NOTHING. Only the first is ours to answer.
        //
        // Measured in a menu, where the picture is static and the game stops
        // producing frames:
        //
        //   09:38:43  src 28.8  moving 0.1%  gap fills 49.5/s  missed 15.0%  gpu 23.2%  relief  1.26
        //   09:38:46  src 24.8  moving 0.1%  gap fills 42.0/s  missed 39.2%  gpu 16.5%  relief  5.17
        //   09:38:50  src 30.1  moving 0.2%  gap fills 41.7/s  missed 36.7%  gpu 16.2%  relief 24.00
        //
        // Thirty-six percent of slots missed at SIXTEEN PERCENT of the graphics
        // card. We were not slow; there was nothing to show. The regulator read
        // it as congestion and went to its ceiling, and the additive recovery
        // then held quality down for the ten seconds after the menu closed:
        // 23.99, 23.46, 22.46, 21.45, 21.23, 21.10 while the game ran at a
        // perfect 72 again.
        //
        // So the missed-slot rate keeps the trigger, and our own cost becomes
        // the gate. The 95th percentile of interpolation against the deadline
        // was tried ALONE as a trigger and failed - it fired while the engine
        // was delivering 144 fps with zero misses. As a gate it is sound,
        // because the conjunction is what carries the meaning: slots are being
        // missed AND our own work is near its deadline. Either alone lies; both
        // together do not.
        std::vector<double> interps(interpHistory, interpHistory + costHistoryCount);
        std::sort(interps.begin(), interps.end());
        const double interpP95 = interps[(interps.size() * 95) / 100];
        const double deadlineMs = lockedPeriodMs * 0.5;
        const bool weAreTheBottleneck = (interpP95 > deadlineMs * 0.7);

        if (missedSlotEma >= 0.0 && reliefDtSec > 0.0 && reliefDtSec < 1.0) {
            if (missedSlotEma > 0.05 && weAreTheBottleneck) {
                qualityRelief *= std::pow(1.6, reliefDtSec);
            } else if (missedSlotEma < 0.01 || !weAreTheBottleneck) {
                // Recovering whenever we are NOT the bottleneck, not only when
                // nothing is being missed: a source that has stopped delivering
                // must not keep quality suppressed while it does.
                qualityRelief -= 1.0 * reliefDtSec;
            }
        }
        if (qualityRelief < 1.0) qualityRelief = 1.0;
        if (qualityRelief > 24.0) qualityRelief = 24.0;
    };

    auto HeadroomVerdict = [&]() -> std::string {
        if (costHistoryCount < 60 || lockedPeriodMs <= 1.0)
            return "measuring - play for a few seconds";

        std::vector<double> totals(costHistory, costHistory + costHistoryCount);
        std::sort(totals.begin(), totals.end());
        const double medianTotal = totals[totals.size() / 2];

        std::vector<double> interps(interpHistory, interpHistory + costHistoryCount);
        std::sort(interps.begin(), interps.end());
        const double medianInterp = interps[interps.size() / 2];
        const double p95Interp = interps[(interps.size() * 95) / 100];

        const double deadlineMs = lockedPeriodMs * 0.5;
        const double sourceFps = 1000.0 / lockedPeriodMs;
        // Share of every second this engine takes from the graphics card - the
        // other way it can fail, by starving the game rather than by missing a
        // deadline. One estimation and one interpolation per source frame.
        // INFLATED UNDER CONTENTION - this is not occupancy.
        //
        // The arithmetic is right: our GPU time per second over one second.
        // medianTotal is not, because it comes from GPU timestamp queries, and
        // those measure ELAPSED time between the start and end markers. Work
        // the game does in between is counted as ours.
        //
        // Measured against Task Manager in the same scene: it reports about 12%
        // for this process while this line said 55-82%. Task Manager measures
        // actual utilisation through the scheduler and is the number to believe.
        //
        // Kept because the RELATIVE movement is still informative - it rises
        // when we get more expensive - but it must not be read as "we take N
        // percent of the card", and it has now been read that way three times.
        const double gpuSharePercent = medianTotal * sourceFps / 10.0;

        int overDeadline = 0;
        for (double c : interps) if (c > deadlineMs) ++overDeadline;
        const double missPercent = 100.0 * overDeadline / costHistoryCount;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        if (missPercent < 2.0 && gpuSharePercent < 35.0)
            oss << "COMFORTABLE";
        else if (missPercent < 15.0 && gpuSharePercent < 55.0)
            oss << "TIGHT";
        else
            oss << "NOT ENOUGH";
        oss << " - interpolation " << medianInterp << " ms median, " << p95Interp
            << " ms at the 95th percentile, against a " << deadlineMs << " ms deadline ("
            << missPercent << "% late); estimation plus interpolation takes "
            << gpuSharePercent << "% of a source interval at " << sourceFps
            << " source FPS (elapsed-time measure, inflated by contention - not occupancy;"
            << " Task Manager is the number to believe for that)";
        return oss.str();
    };
    // THE LAST THREE SOURCE INTERVALS, for spacing - separate from the lock.
    //
    // lockedPeriodMs is an EMA with a time constant of about thirty frames,
    // half a second, and that is right for what it answers: the source's true
    // long-run period, where a stall is part of the average rather than an
    // outlier to reject. It is the wrong answer for where to put THIS pair's
    // generated frame.
    //
    // Measured in the log: the source fell from 72.5 to 45.8 fps inside three
    // seconds. For half of that the spacing still used 13.9 ms while the real
    // interval was 22, so the generated frame and its real partner went out
    // 7 ms apart and then nothing came for 15. The output arrives in pairs with
    // a gap behind them, which is uneven however correct the frame count is.
    //
    // FIFTEEN INTERVALS, AND A RESET WHEN ONE IS ABSURD.
    //
    // This was a median of three. Three is too short for a source that measures
    // a standard deviation of 1.3-4.9 ms around a 13.88 ms mean: the spacing
    // estimate jumps with almost every frame, and the phase placed against it
    // jumps with it. That unsteadiness has been visible all evening as content
    // steps going backwards and as gap fills.
    //
    // Lossless Scaling's shipped config.ini - plain text beside the binary, not
    // anything taken out of it - carries these two:
    //
    //     frametime_buffer_size = 15
    //     frametime_buffer_reset_multiplier = 6
    //
    // Fifteen is about a fifth of a second at 72 fps. Long enough that ordinary
    // capture jitter averages out; short enough to follow a real rate change
    // within a fifth of a second.
    //
    // The reset multiplier is the half that makes the long window safe, and it
    // is why a longer window is not simply a slower one. A stall - alt-tab, a
    // shader compile, a level load - puts one interval of 200 ms into the
    // buffer, and a median of fifteen would carry that for the next fifteen
    // frames. Above six times the current estimate the buffer is not smoothed,
    // it is EMPTIED: whatever came before that gap describes a situation that
    // has ended.
    //
    // Still a median rather than a mean, because the median of fifteen also
    // survives the one or two outliers that sit below the reset line.
    // How often the 6x rule fired. Reported, so a window that is constantly
    // being emptied shows up as that rather than as mysterious unsteadiness.
    uint64_t intervalResetsSinceReport = 0;
    static const int kIntervalWindow = 15;
    static const double kIntervalResetMultiple = 6.0;
    double recentIntervalMs[kIntervalWindow] = { 0.0 };
    int recentIntervalNext = 0;
    int recentIntervalCount = 0;

    auto UpdateSourcePeriod = [&](double intervalMs) {
        if (!(intervalMs > 1.0 && intervalMs < 100.0)) return;

        // ABSURD INTERVAL: throw the window away rather than average it in.
        if (recentIntervalCount > 0 && lockedPeriodMs > 1.0
            && intervalMs > lockedPeriodMs * kIntervalResetMultiple) {
            recentIntervalNext = 0;
            recentIntervalCount = 0;
            ++intervalResetsSinceReport;
        }

        recentIntervalMs[recentIntervalNext] = intervalMs;
        recentIntervalNext = (recentIntervalNext + 1) % kIntervalWindow;
        if (recentIntervalCount < kIntervalWindow) ++recentIntervalCount;
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
        // The median of the window, so the spacing follows the source rather
        // than an average of where it used to be. See the ring above.
        //
        // Sorted into a copy: fifteen doubles on the stack, once per generated
        // frame, against a source period the whole output is placed against.
        // Partial selection would be faster and is not worth the second
        // implementation to get wrong.
        if (recentIntervalCount >= 3) {
            double sorted[kIntervalWindow];
            for (int i = 0; i < recentIntervalCount; ++i) sorted[i] = recentIntervalMs[i];
            std::sort(sorted, sorted + recentIntervalCount);
            const double median = sorted[recentIntervalCount / 2];
            if (median > 1.0 && median < 100.0) return median;
        }
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
    // ADAPTIVE OUTPUT - fractional multiplier aimed at the display rate.
    // On by default: a flat 2x is only even when twice the source lands on the
    // panel exactly, and that is a condition on the game, not on us. Turn it
    // off with "adaptive = off" in frameboost.ini to get the flat 2x back;
    // at a 72 fps source on a 144 Hz panel the two are identical anyway.
    const bool adaptiveOutput = !(settingsFile.count(L"adaptive") && !Setting(L"adaptive"))
                             && !HasArg(L"noadaptive");
    // ITS OWN CEILING, and not maxFactor.
    //
    // maxFactor defaults to 2 and is the manual multiplier the F-key toggles.
    // Clamping the adaptive multiplier to it undoes the entire point: 60 fps on
    // a 144 Hz panel needs 2.4, the clamp would hand back 2.0, and the output
    // would sit at 120 - which is the judder this exists to remove. Caught by
    // re-reading the change rather than by testing it, and it would have looked
    // like "adaptive does nothing" in exactly the case it was built for.
    //
    // 4 covers every source from a third of the refresh upward (48 -> 144 is 3,
    // 36 -> 144 is 4). Below that the generated share is too large to be worth
    // showing and the GPU room check stands aside anyway.
    const int adaptiveMaxFactor = [&] {
        const int v = SettingInt(L"adaptivemax", 4);
        return (v < 2) ? 2 : ((v > 6) ? 6 : v);
    }();
    // Also the default, and "noslotwait" opts out. Every measurement of this
    // engine that came out well was taken with it: 72 -> 144 fps at 0.3 ms
    // jitter and 0% missed slots.
    // THE ONE THING THAT ACTUALLY COUPLES OUTPUT TO THE REFRESH RATE.
    //
    // WaitForDisplaySlot holds every present at least outputSlotMs * 0.95
    // after the previous one, and outputSlotMs is 1000 / refresh - 6.94 ms at
    // 144 Hz. That is a hard ceiling of about 144 presents a second whatever
    // the source is doing. At an 85 fps source the engine wants to present 170
    // and is not allowed to, which is why the output read 126-144 instead.
    //
    // Lukas has asked repeatedly for the output to be twice the source and not
    // matched to the panel, and this is the code that was preventing it. The
    // engine's generation is already exactly 2x - one generated frame per real
    // frame, placed at half the measured interval. Only the presentation was
    // capped.
    //
    // Now a setting, and OFF, because that is what was asked for:
    //
    //     slotwait = on      in frameboost.ini to bring it back
    //
    // The honest note on each side. With it on, presents cannot collide inside
    // one refresh interval, and a display cannot show two frames in one tick -
    // the second is discarded. With it off, output tracks the source exactly,
    // and above the refresh rate some of those frames are thrown away by the
    // panel rather than by us. Which of those looks better is a judgement about
    // a picture, and the person watching it decides.
    //
    // A contradiction worth recording: the comment in WaitForDisplaySlot says
    // it was measured to make the picture WORSE and should stay opt-in, while
    // the default was flipped to on last night on the grounds that the good
    // measurements had been taken with it. Both were written by me, two weeks
    // apart, and they have never been tested against each other on one machine
    // in one scene.
    const bool waitForDisplaySlot = Setting(L"slotwait") && !HasArg(L"noslotwait");
    bool f4WasDown = false;
    bool f12WasDown = false;
    // The automatic frame dump fires REPEATEDLY, on a cooldown, rather than
    // once per session.
    //
    // Once was enough to find the viewmodel artefact, and then immediately not
    // enough: the one dump it took landed on a moment with the camera pointing
    // at the ground, and the weapon - the thing being investigated - was not
    // even in frame. The hotkey is no answer either. "Ich kann mich nicht
    // drehen und waehrenddessen so viele Sachen gleichzeitig druecken" is
    // exactly right: a diagnostic that requires a three-key chord during the
    // manoeuvre it is meant to capture does not get used.
    //
    // So it triggers itself on the measurement, repeatedly, and overwrites the
    // same three files - the newest fast turn is always on disk, and nothing
    // accumulates.
    double nextAutoDumpAtMs = 0.0;
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
    // Sampled every turn of the loop, not once a second: the depth is the
    // buffer that absorbs a late frame, and a single snapshot cannot tell a
    // queue that is always empty from one that is merely empty at that moment.
    int queueDepthMin = 9999, queueDepthMax = 0;
    double queueDepthSum = 0.0;
    uint64_t queueDepthSamples = 0;
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

    // Gaps split by WHAT was presented, not just averaged.
    //
    // The overall average cannot see the failure being looked for here: a real
    // frame followed 5 ms later by a generated one, followed 11 ms later by
    // the next real one, averages to a perfect 8 ms while the eye sees a limp.
    // Reported exactly that way - "als waere zwischen den generierten und
    // nicht generierten ein ungleichmaessiger abstand".
    //
    // realToGen is the wait from a real frame to the generated one after it;
    // genToReal from a generated frame to the next real one. At a factor of
    // two these must be equal, and each half the source period.
    double realToGenSum = 0.0, genToRealSum = 0.0;
    uint64_t realToGenCount = 0, genToRealCount = 0;

    // HOW FAR THE REAL FRAME ACTUALLY LANDS FROM ITS SLOT, corrected for.
    //
    // The due times are right: the generated frame belongs at the capture
    // timestamp plus the arrival lag, the real one half an interval after it,
    // and both are computed that way. The measurement disagrees:
    //
    //   real -> generated  5.28 ms        generated -> real  7.30 ms
    //
    // The sum is correct - 12.58 against a 13.9 ms interval, the rest being the
    // frames themselves - so nothing is lost or late. The SPLIT is wrong by
    // about a millisecond, seventy times a second.
    //
    // The reason is structural rather than arithmetic. A generated frame is
    // written straight into the back buffer; a real frame goes through
    // PresentFrame with a full-frame copy - 14 MB at 2560x1440 - and the
    // timestamp is taken when the present RETURNS. So the real frame is
    // recorded about a millisecond after the moment it was due, and the gap
    // before it grows while the gap after it shrinks.
    //
    // Estimating that copy cost would be a guess that goes stale on another
    // machine or resolution. The engine already measures both halves, so the
    // correction is taken from the measurement itself: half the difference,
    // which is exactly the shift that makes the two equal.
    //
    // Slow, because it steers the output timing and a fast loop here would
    // hunt: one tenth of the error per report, at most one report a second.
    // Bounded at 3 ms, so no bad measurement can move the presentation
    // further than the thing it is correcting could possibly account for.
    double realPhaseCorrectionMs = 0.0;
    bool lastPresentWasGenerated = false;

    auto RecordPresentGap = [&](double presentEndMs, bool thisOneGenerated) {
        if (lastPresentAtMs > 0.0) {
            const double gap = presentEndMs - lastPresentAtMs;
            gapSumMs += gap;
            gapSumSqMs += gap * gap;
            if (gap < gapMinMs) gapMinMs = gap;
            if (gap > gapMaxMs) gapMaxMs = gap;
            ++gapSamples;
            const bool slotMissed =
                (outputSlotMs > 0.0 && std::abs(gap - outputSlotMs) > outputSlotMs * 0.5);
            if (slotMissed) ++gapMissed;
            // The same judgement the telemetry reports, kept as a running value
            // so the quality regulator can read it between reports.
            missedSlotEma = (missedSlotEma < 0.0)
                ? (slotMissed ? 1.0 : 0.0)
                : missedSlotEma * 0.99 + (slotMissed ? 1.0 : 0.0) * 0.01;
            // Closer than one refresh: counted by us, never seen by anyone.
            if (outputSlotMs > 0.0 && gap < outputSlotMs * 0.9) ++gapCollapsed;

            if (thisOneGenerated && !lastPresentWasGenerated) {
                realToGenSum += gap;
                ++realToGenCount;
            } else if (!thisOneGenerated && lastPresentWasGenerated) {
                genToRealSum += gap;
                ++genToRealCount;
            }
        }
        lastPresentWasGenerated = thisOneGenerated;
        lastPresentAtMs = presentEndMs;
        // The keep-alive must not fire before the first real present, or it
        // would push an empty texture at a window that has never drawn.
        presentedAnythingYet = true;
    };

    double phasePresentMsSum = 0.0;   // CopyResource + Present, incl. any vsync block
    double phaseIterationMsSum = 0.0; // whole iteration
    uint64_t phaseSamples = 0;

    auto NowMs = [&]() {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / qpcFreq.QuadPart * 1000.0;
    };

    // SLEEP UNTIL JUST BEFORE THE DEADLINE, THEN SPIN THE REST.
    //
    // Every wait in the pacing path was a bare busy-wait. That buys sub-
    // millisecond precision and costs a whole core: measured at 106% of one,
    // continuously, for an engine whose real work is a few milliseconds of GPU
    // per frame.
    //
    // It matters because of what it is taken from. Apex measured on its own -
    // externally, with the booster stopped - delivers 71.75 frames a second at
    // 13.88 ms spacing, standard deviation 0.86 ms. A game running perfectly
    // evenly. With us present the same source swings between 45 and 72. We are
    // the difference, and a permanently occupied core is the largest single
    // thing we take.
    //
    // A high-resolution waitable timer sleeps to within about a tenth of a
    // millisecond, so the spin only has to cover the last stretch. The
    // precision is kept where it is needed and the core is handed back for the
    // rest of the wait.
    //
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION needs Windows 10 1803. Without it
    // the handle is null, every wait falls back to spinning, and the behaviour
    // is exactly what this replaces.
    HANDLE preciseTimer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);

    // Waits until dueMs, never past ceilingMs, pumping desktop duplication
    // throughout.
    //
    // The sleep is capped at 2 ms a time rather than taken in one go because
    // the desktop-duplication path has to keep asking: a frame arriving during
    // a long sleep would sit unclaimed. Windows Graphics Capture drains itself
    // on the pool thread and needs no pumping, but one wait serves both paths.
    // Read once, so a bad value cannot be edited into the file mid-session.
    // Bounded: below 0.1 the tail cannot correct anything, above 4 it is a busy
    // wait by another name.
    const double spinTailMs = [&]() {
        const double v = static_cast<double>(SettingInt(L"spintail", 2));
        return (v < 0.1) ? 0.1 : (v > 4.0 ? 4.0 : v);
    }();

    auto WaitUntilMs = [&](double dueMs, double ceilingMs) {
        // HOW LONG TO SPIN AT THE END OF A WAIT, and why it is not 0.35 ms.
        //
        // The waitable timer is documented as accurate to about a tenth of a
        // millisecond, so a 0.35 ms tail should be plenty. Measured output
        // jitter is 2-4 ms, which is an order of magnitude more than the timer
        // is supposed to miss by - the sleep overshoots, and a tail that short
        // has nothing left to correct with.
        //
        // AMD ships this problem solved the blunt way. Their FidelityFX frame
        // interpolation swapchain "handles frame pacing automatically using a
        // busy wait loop to achieve the best possible timing behaviour, since
        // Windows is not a real-time operating system" - a production frame
        // generator, burning a core on purpose, for exactly this.
        //
        // We went the other way in this file once already and for a good
        // reason: a full busy wait measured 106% of a core and starved the
        // game. This is the middle - sleep for the bulk, spin the last two
        // milliseconds, which is wide enough to absorb an overshoot the timer
        // can actually produce.
        //
        // "spintail = N" in frameboost.ini sets it, because the right value is
        // a property of the machine and not of the code.
        const double kSpinTailMs = spinTailMs;
        for (;;) {
            const double now = NowMs();
            if (now >= dueMs || now >= ceilingMs) return;
            const double target = (dueMs < ceilingMs) ? dueMs : ceilingMs;
            const double remaining = target - now;
            if (preciseTimer && remaining > kSpinTailMs) {
                double sleepMs = remaining - kSpinTailMs;
                if (sleepMs > 2.0) sleepMs = 2.0;
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(sleepMs * 10000.0); // 100 ns, relative
                if (SetWaitableTimer(preciseTimer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(preciseTimer, 5);
            }
            ddCapture.Pump();
        }
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
        // Opt-in again. It removed present collisions completely (35-41% down
        // to 0.0%) and the picture got WORSE, not better - the added latency
        // was real and the promised gain never showed up. And it caps output at
        // the refresh rate, which is not what this tool is for.
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
        // Slow regulator, once per report rather than per frame: quality that
        // oscillates is worse than quality that is merely lower, because the
        // eye notices the change more than the level.
        UpdateQualityRelief();
        interpolator.SetQualityRelief(static_cast<float>(qualityRelief));
        interpolator.SetMotionCutoff(static_cast<float>(motionCutoffPx));

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double elapsed = static_cast<double>(now.QuadPart - lastReport.QuadPart) / qpcFreq.QuadPart;
        if (elapsed < 1.0) return;

        double nativeFps = nativeFramesSinceReport / elapsed;

        double generatedFps = generatedFramesSinceReport / elapsed;
        // EVERYTHING THAT REACHED THE SCREEN, not only what was interesting.
        //
        // This counted real plus generated frames and nothing else, so presents
        // that carried a near-static picture were invisible to it:
        //
        //   10:57:41  source 71.0  duplicates 71/s  passthrough 71/s  OUTPUT 0.0
        //
        // Seventy-one frames a second went to the display and the number said
        // zero. That is the same failure as a counter that is never incremented,
        // pointing the other way: it understated exactly when the picture was
        // quiet, which is when it was being looked at.
        //
        // Duplicate passthroughs and keep-alive presents are real presents of
        // the newest picture there is. Counting them is not inflating anything -
        // not counting them was the lie. It also makes the shape correct:
        // roughly the source rate at rest, where doubling a static picture can
        // add nothing, and twice the source in motion, where it can.
        // A REPEAT OF THE SAME PICTURE IS NOT OUTPUT.
        //
        // Half an hour ago this started counting keep-alive presents too, on the
        // argument that everything reaching the screen should count. That was
        // right for duplicate passthroughs, which present the NEWEST CAPTURED
        // frame and can carry a small change - typed text was the case. It was
        // wrong for keep-alives, which re-present the frame already on screen.
        //
        // What it produced, immediately:
        //
        //   11:06:44  source 34.0  native 0.0  OUTPUT 72.0
        //             moving blocks 0.3%  passthrough 5/s  keep-alive 67/s
        //
        // Seventy-two in the counter and one frozen picture on the display,
        // shown seventy-two times. That is precisely the number this project
        // promised never to produce, and I built it.
        //
        // Keep-alives stay - a screen that stops updating is still the worst
        // failure - but they are reported on their own line and counted nowhere
        // else. If output reads near zero while keep-alive reads 67, that is the
        // truth being told plainly: nothing new is being shown.
        // WHAT THE ENGINE PRODUCED: real frames plus generated frames. Nothing
        // else, and this is where it stays.
        //
        // I moved this three times today and Lukas caught the arithmetic three
        // times - "72 generated 71 und 160 output", then 150, then "ES IST
        // IMMERNOCH 160 fps OBWOHL 60 GAME FPS 60 GENERATED FPS". He is right
        // every time: a number that does not add up destroys trust in every
        // other number on the line, and the repeats were what did not add up.
        //
        // Repeats still happen and still matter - a screen that stops updating
        // is the worst failure this can have - so they are still counted and
        // still reported, on their own lines, as "Keep-alive/s" and "Duplicate
        // passthrough/s". What they are not is output. Output is what we made.
        //
        // The consequence is accepted rather than worked around: on a static
        // picture this reads near zero, because near zero is what the engine
        // generated. The keep-alive line beside it says the screen was still
        // being served.
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
        // COUNTED, NOT DERIVED FROM A SMOOTHED INTERVAL.
        //
        // This used to be 1000 / (EMA of frame intervals), and it read HIGHER
        // than the game's own counter - 73.5, 76.2, 78.7 against a 72 cap,
        // which the source cannot deliver.
        //
        // The cause is in the same telemetry: after a brief stall the capture
        // hands us several frames in a burst, and arrival spacings of 0.07-0.13
        // ms have been measured. Those pull an exponentially weighted mean of
        // intervals sharply down, and its reciprocal sharply up, and it recovers
        // slowly. The number was wrong in the one direction that makes an engine
        // look better than it is.
        //
        // Since yesterday the honest figure is in hand: how many frames the
        // capture actually announced in the last second, reset per report. A
        // count over a known interval cannot exceed what arrived, and needs no
        // assumption about the shape of the distribution.
        //
        // The EMA stays as the fallback for the Desktop Duplication path, which
        // keeps its own counters, and for the first second before any count.
        // ZERO IS A MEASUREMENT, NOT A MISSING ONE.
        //
        // The fallback below used to apply whenever the count was zero, which
        // is precisely when the count is most worth believing. With the capture
        // running and producing nothing, this reported
        // 1000 / realFrameIntervalEmaMs - an interval that stops being updated
        // the moment frames stop arriving, so it freezes at its last value and
        // keeps reporting it:
        //
        //   16:13:46  src 62.0  nat 0.0  produced/retrieved 0/0  interval 16.124
        //   16:13:47  src 62.0  nat 0.0  produced/retrieved 0/0  interval 16.124
        //   16:13:49  src 62.0  nat 0.0  produced/retrieved 0/0  interval 16.124
        //   16:13:50  src 62.0  nat 0.0  produced/retrieved 0/0  interval 16.124
        //
        // Four seconds of a completely dead capture, reported as a healthy 62
        // frames a second, with the same three digits each time because the
        // number was a division of two constants. It cost hours: the zero in
        // "Native FPS" was read as an output problem for as long as the source
        // beside it looked alive.
        //
        // So the fallback now covers only the case it was written for - the
        // Desktop Duplication path, which keeps no such count, and the first
        // report before any window has elapsed. While Windows Graphics Capture
        // is running, its count is the answer, zero included.
        const bool haveProducedCount = !useDesktopDuplication && capture.IsCapturing() && elapsed > 0.0;
        const uint64_t producedThisWindow = haveProducedCount ? capture.FramesProduced() : 0;
        const double sourceFps = haveProducedCount
            ? producedThisWindow / elapsed
            : (realFrameIntervalEmaMs > 0.0 ? 1000.0 / realFrameIntervalEmaMs : -1.0);
        // Seconds in which the capture was running and delivered nothing at
        // all. Counted separately because a stall that ends before the
        // watchdog's two-second threshold leaves no other trace at all.
        if (haveProducedCount && producedThisWindow == 0) ++captureStallSeconds;
        // Native FPS counts real frames presented UNCHANGED. On the clock-driven
        // path that is a small share by design - a frame whose phase lands mid
        // interval is shown as an interpolation of itself and its neighbour,
        // not skipped - so Source FPS is the number that describes the game.
        // Read once here, so every field in the line below describes the same
        // moment. ResetAuditCounters at the end of the report starts the next
        // window.
        captureIntervals = capture.ProducedIntervalStats();

        // Steer the real frame back onto its slot - see realPhaseCorrectionMs.
        //
        // The two halves should be equal. Half their difference is the shift
        // that makes them so, and moving the real frame EARLIER by that amount
        // is what closes the gap before it and opens the one after.
        if (realToGenCount > 30 && genToRealCount > 30) {
            const double realToGen = realToGenSum / realToGenCount;
            const double genToReal = genToRealSum / genToRealCount;
            const double wanted = (genToReal - realToGen) * 0.5;
            realPhaseCorrectionMs += (wanted - realPhaseCorrectionMs) * 0.1;
            if (realPhaseCorrectionMs > 3.0) realPhaseCorrectionMs = 3.0;
            if (realPhaseCorrectionMs < -3.0) realPhaseCorrectionMs = -3.0;
        }

        // The watchdog runs here because this is already once a second and
        // already holds the capture's counters.
        if (!useDesktopDuplication && capture.IsCapturing()) {
            // ZERO, not "unchanged".
            //
            // ResetAuditCounters zeroes this at the end of every report, so the
            // value read here is the count for the last second - and at a locked
            // 72 fps it reads exactly 72 second after second. The first version
            // of this watchdog tested for "has not moved", which that satisfies
            // perfectly, and it would have restarted a perfectly healthy capture
            // roughly whenever the source held a steady rate. A capture that has
            // stopped reports nothing at all, so that is what to test.
            const uint64_t producedNow = capture.FramesProduced();
            if (producedNow > 0 || watchdogLastProgressMs <= 0.0) {
                watchdogLastProduced = producedNow;
                watchdogLastProgressMs = NowMs();
            }

            // PRODUCING, BUT ALWAYS THE SAME PICTURE: the pool is holding a
            // surface that has stopped being filled.
            //
            // The watchdog above only catches a capture that stops entirely.
            // CS2 in borderless fullscreen fails a different way and the
            // numbers are unmistakable:
            //
            //   source 34-36, duplicates 34-36 - every single arrival - with
            //   native 0 and a frame-to-frame difference of 0.12, while the
            //   game was visibly being played.
            //
            // A frame dump settled it: two consecutive captured frames,
            // byte-identical across 114,012 sampled offsets.
            //
            // OSSS, which does the same job with the same APIs, documents the
            // mechanism and the remedy: "the frame pool was sized against a
            // surface that no longer exists, and the session never recovers on
            // its own" - so it rebuilds the session rather than waiting.
            //
            // That fits every measurement here, including the one that made no
            // sense under the occlusion theory: five separate attempts to stop
            // being an occluder changed nothing, while hiding the overlay
            // entirely fixed it instantly. Appearing is what makes the game
            // change how it presents; the stale pool is the consequence.
            //
            // Rebuilt at most once every five seconds, because a genuinely
            // static screen - a menu, a paused game - also produces nothing but
            // duplicates and must not be allowed to restart the capture in a
            // loop.
            const bool everythingDuplicate =
                producedNow > 0 && duplicateFramesSinceReport >= producedNow;
            if (!everythingDuplicate) {
                staleSurfaceSinceMs = 0.0;
            } else {
                if (staleSurfaceSinceMs <= 0.0) staleSurfaceSinceMs = NowMs();
                const bool longEnough = NowMs() - staleSurfaceSinceMs > 2000.0;
                const bool notTooSoon = NowMs() - lastStaleRebuildMs > 5000.0;
                if (longEnough && notTooSoon && overlayVisibleLastIteration) {
                    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Capture is producing frames but every one"
                        " is identical - the frame pool is holding a surface that stopped being filled."
                        " Rebuilding the capture session.");
                    capture.Stop();
                    HWND retarget = targetWindow;
                    if (retarget && !IsWindow(retarget)) retarget = nullptr;
                    if (!retarget && !monitorMode) retarget = GetForegroundWindow();
                    const bool rebuilt = monitorMode
                        ? capture.StartMonitor(targetMonitor, device.get())
                        : (retarget ? capture.Start(retarget, device.get()) : false);
                    if (rebuilt && !monitorMode) targetWindow = retarget;
                    ++captureRestarts;
                    FrameBoostBeta::Logger::Log(rebuilt
                        ? "[FrameBoostBeta] Capture session rebuilt."
                        : "[FrameBoostBeta] Capture session could not be rebuilt - will retry.");
                    lastStaleRebuildMs = NowMs();
                    staleSurfaceSinceMs = 0.0;
                }
            }

            if (producedNow == 0 && watchdogLastProgressMs > 0.0
                    && NowMs() - watchdogLastProgressMs > 2000.0) {
                FrameBoostBeta::Logger::Log("[FrameBoostBeta] Capture has produced no frames for two seconds"
                    " - restarting it. The window was probably minimised, or the game changed its"
                    " presentation mode.");
                capture.Stop();

                // If the window we were following is gone, take whatever is in
                // front now - the same rule the engine starts with.
                HWND retarget = targetWindow;
                if (retarget && !IsWindow(retarget)) retarget = nullptr;
                if (!retarget && !monitorMode) retarget = GetForegroundWindow();

                const bool restarted = monitorMode
                    ? capture.StartMonitor(targetMonitor, device.get())
                    : (retarget ? capture.Start(retarget, device.get()) : false);

                if (restarted) {
                    if (!monitorMode) targetWindow = retarget;
                    ++captureRestarts;
                    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Capture restarted.");
                } else {
                    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Capture could not be restarted - will try"
                        " again in two seconds.");
                }
                watchdogLastProduced = capture.FramesProduced();
                watchdogLastProgressMs = NowMs();
            }
        }

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
            << " | Capture restarts: " << captureRestarts
            << " | Capture stall seconds: " << captureStallSeconds
            << " | Arrivals stepped over/s: " << (staleFramesSinceReport / elapsed)
            << " | Duplicate frames skipped/s: " << ((duplicateFramesSinceReport + (ddCapture.UnchangedFrames() - ddUnchangedAtReport)) / elapsed)
            << " | Unchanged by dirty rects: " << ddCapture.UnchangedFrames()
            << " | Frame-to-frame difference: " << duplicateDetector.LastDifference()
            << " | Real frame interval (measured): " << (realFrameIntervalEmaMs > 0 ? std::to_string(realFrameIntervalEmaMs) + " ms" : "N/A")
            << " | Dropped late: " << (generatedDroppedLate / elapsed) << "/s"
            << " (by " << (generatedDroppedLate ? droppedLateByMsSum / generatedDroppedLate : 0.0)
            << " ms avg, " << droppedLateByMsMax << " ms max; slack when on time "
            << (slackWhenOnTimeCount ? slackWhenOnTimeMsSum / slackWhenOnTimeCount : 0.0) << " ms)"
            << " | Skipped backwards: " << (generatedSkippedBackwards / elapsed)
            << " | Schedule clock: " << (smoothClockEnabled ? "smoothed" : "raw")
            << ", drift " << (scheduleAnchorMs > 0.0 ? scheduleAnchorMs - motionCurrTimestampMs : 0.0)
            << " ms, resyncs " << scheduleResyncs
            << " | Pacing window: " << recentIntervalCount << "/" << kIntervalWindow
            << ", resets " << intervalResetsSinceReport << "/s" << "/s"
            << " | Still picture: " << (pictureIsStill ? "standing aside" : "no")
            << " (moving EMA " << movingEma << ", difference EMA " << diffEma << ")"
            << " | Gap fills/s: " << (gapFillsSinceReport / elapsed)
            << " | Keep-alive/s: " << (keepAlivePresents / elapsed)
            << " | Duplicate passthrough/s: " << (duplicatePassthroughs / elapsed)
            << " | Vsync: " << (presentSyncInterval == 0 ? "off" : "on")
            << " | Refresh lock: " << (refreshLockEnabled ? "on" : "off")
            << " | Generation factor: " << generationFactor << "x"
            << " | Adaptive output: " << (adaptiveOutput ? "on" : "off")
            << " | Adaptive extra frames/s: " << (adaptiveExtraFrames / elapsed)
            << " | Output per real: " << adaptiveOutputPerReal
            << " | Skips/s - no motion field: " << (skipNoMotionField / elapsed)
            << ", no GPU room: " << (skipNoGpuRoom / elapsed)
            << ", degraded: " << (skipDegraded / elapsed)
            << ", still: " << (skipStill / elapsed)
            << ", factor 1: " << (skipFactorOne / elapsed)
            << ", generate failed: " << (skipGenerateFailed / elapsed)
            << " | On-screen age: " << (presentAgeSamples ? std::to_string(presentAgeSumMs / presentAgeSamples) + " ms avg, " + std::to_string(presentAgeMaxMs) + " ms max" : "N/A")
            << " | Quality relief: " << qualityRelief
            << " (missed-slot EMA " << (missedSlotEma * 100.0) << "%)"
            << " | Headroom: " << HeadroomVerdict()
            << " | Locked source period: " << lockedPeriodMs << " ms (" << (lockedPeriodMs > 0 ? 1000.0 / lockedPeriodMs : 0.0) << " FPS)"
            << " | Source regularity: " << ((realFrameIntervalEmaMs > 0 && intervalDeviationEmaMs >= 0)
                ? std::to_string(100.0 * intervalDeviationEmaMs / realFrameIntervalEmaMs) + "% deviation, " + (sourceIsIrregular ? "IRREGULAR (generation continues - the one-frame buffer covers it)" : "steady")
                : std::string("N/A"))
            << (measureOutputDiff
                ? " | Output pixel change: real " + std::to_string(realDiffCount ? realDiffSum / realDiffCount : 0.0)
                  + ", generated " + std::to_string(generatedDiffCount ? generatedDiffSum / generatedDiffCount : 0.0)
                  + " (mean per-tile difference, 0-255; equal values mean both carry real motion,"
                  + " generated near 0 means the extra frames are copies)"
                : std::string(""))
            << " | Queue depth: " << queueCount
            << " (min " << (queueDepthSamples ? queueDepthMin : 0)
            << ", max " << queueDepthMax
            << ", mean " << (queueDepthSamples ? queueDepthSum / queueDepthSamples : 0.0)
            << ", dropped/s " << (queueDroppedSinceReport / elapsed) << ")"
            << " | Presents lost to collision: " << (gapSamples ? 100.0 * gapCollapsed / gapSamples : -1.0) << "%"
            << " | Real->generated " << (realToGenCount ? realToGenSum / realToGenCount : -1.0)
            << " ms, generated->real " << (genToRealCount ? genToRealSum / genToRealCount : -1.0)
            << " ms (correction " << realPhaseCorrectionMs << " ms)"
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
            // WHAT THE CAPTURE ITSELF IS DOING - previously invisible.
            //
            // "Capture arrivals/s" was declared, reported and reset, and never
            // incremented anywhere. It read 0 in every line of every log, and I
            // read that as "nothing to see" rather than "nothing measured" -
            // for a whole evening spent judging whether the source was at
            // fault.
            //
            // Meanwhile CaptureEngine already measured all of this and main.cpp
            // referenced none of it. The instrumentation was built and never
            // connected.
            //
            // These answer the question directly. Produced/s is how often WGC
            // ANNOUNCED a frame; the interval statistics are the spacing of
            // those announcements, timed in the handler with QPC. Lost-in-pool
            // is frames that existed and were recycled before we read them,
            // which is the one number that says the delay is OURS.
            //
            //   produced steady, spacing tight, our Source FPS swinging
            //     -> we mis-measure or drain unevenly; look here.
            //   spacing itself wide
            //     -> the frames genuinely arrive unevenly; not our pacing.
            //   lost-in-pool above zero
            //     -> we are too slow to drain, and that is ours.
            << " | Capture produced/retrieved: " << capture.FramesProduced()
            << "/" << capture.FramesRetrieved()
            << " | Lost in pool: " << capture.FramesLostInPool()
            << " | Pool buffers: " << capture.PoolBufferCount()
            << " | Pool rebuilds: " << capture.PoolRecreates()
            << " | Capture arrival spacing: " << captureIntervals.meanMs << " ms mean, min "
            << captureIntervals.minMs << ", max " << captureIntervals.maxMs
            << ", sd " << captureIntervals.stdDevMs
            << " (" << captureIntervals.samples << " samples)"
            << " | Transparency: " << ((transparentRealFrames && presenter.SupportsTransparency()) ? "on" : "off")
            << " | Moving blocks: " << (motionStats.MovingBlockPercent() >= 0 ? std::to_string(motionStats.MovingBlockPercent()) + "%" : "N/A")
            << " | Motion mean/max px: " << motionStats.MeanMagnitudePixels() << "/" << motionStats.MaxMagnitudePixels()
            << " | Search-saturated blocks: " << motionStats.SaturatedBlockPercent() << "%"
            << " | Match error mean/max: " << motionStats.MeanMatchError() << "/" << motionStats.MaxMatchError()
            << " | Blocks with no real match: " << motionStats.PoorMatchPercent() << "%"
            << " | Displayed/submitted: " << displayedPerSecond << "/" << submittedPerSecond
            << " | ME stages: coarsest " << estimator.LastCoarsestMs()
            << " ms, coarse " << estimator.LastCoarseMs()
            << " ms, fine " << estimator.LastFineMs()
            << " ms, smooth " << estimator.LastSmoothMs() << " ms"
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

        realDiffSum = generatedDiffSum = 0.0; realDiffCount = generatedDiffCount = 0;
        queueDepthMin = 9999; queueDepthMax = 0; queueDepthSum = 0.0; queueDepthSamples = 0;
        nativeFramesSinceReport = 0;
        generatedFramesSinceReport = 0;
        gapFillsSinceReport = 0;
        keepAlivePresents = 0;
        staleFramesSinceReport = 0;
        adaptiveExtraFrames = 0;
        skipNoMotionField = skipNoGpuRoom = skipDegraded = 0;
        skipStill = skipFactorOne = skipGenerateFailed = 0;
        duplicatePassthroughs = 0;
        generatedDroppedLate = 0;
        droppedLateByMsSum = 0.0;
        droppedLateByMsMax = 0.0;
        slackWhenOnTimeMsSum = 0.0;
        slackWhenOnTimeCount = 0;
        generatedSkippedBackwards = 0;
        intervalResetsSinceReport = 0;
        stillSecondsSinceReport = 0;

        duplicateFramesSinceReport = 0;
        ddUnchangedAtReport = ddCapture.UnchangedFrames();
        duplicateCheckMsSum = 0.0;
        duplicateCheckSamples = 0;
        latencySumMs = 0.0;
        latencySamples = 0;
        phaseComputeMsSum = 0.0;
        presentAgeSumMs = 0.0; presentAgeMaxMs = 0.0; presentAgeSamples = 0;
        // Per-report window, so the spacing describes the last second rather
        // than the whole session.
        capture.ResetAuditCounters();
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
        // 80 px of MEAN motion, not 30: a real turn rather than walking. At 30
        // the trigger fired on almost any movement and caught the wrong moment.
        // Two seconds of cooldown so the readback - which stalls the pipeline
        // for a whole frame - cannot fire on consecutive frames and turn the
        // diagnostic into the stutter it is supposed to explain.
        const bool fastTurnToDump = frameDumpEnabled
            && haveMotionField
            && motionStats.MeanMagnitudePixels() > 80.0
            && NowMs() >= nextAutoDumpAtMs;
        if (fastTurnToDump) nextAutoDumpAtMs = NowMs() + 2000.0;

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

        // HOW MANY ARRIVALS THIS POLL STEPPED OVER. Read immediately, because
        // the next poll overwrites it, and needed further down to keep the
        // measured source interval honest - see UpdateSourcePeriod below.
        const int staleThisPoll = useDesktopDuplication ? 0 : capture.LastDiscardedStaleFrames();
        if (staleThisPoll > 0) staleFramesSinceReport += staleThisPoll;

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

            // "ALMOST unchanged" IS NOT "unchanged" - SHOW IT ANYWAY.
            //
            // A duplicate carries no motion worth estimating, so it rightly
            // does not become a new frame for the interpolator. But it is the
            // NEWEST PICTURE THERE IS, and our overlay is on top, so whatever
            // it does contain is the only thing the viewer can be shown.
            //
            // Reported: typing in the CS2 console did not appear. The measured
            // shape of it:
            //
            //   10:54:47  duplicates 54/s  native 9  out 42  keep-alive 46
            //             frame-to-frame difference 0.099, moving blocks 0.1%
            //
            // The detector was right - the picture really had barely changed.
            // What was wrong was the keep-alive added an hour ago: it presents
            // estimator.CurrFrameTexture(), which is the last frame that PASSED
            // the duplicate test. So it faithfully re-presented a stale picture
            // forty-six times a second while every frame containing the typed
            // characters was thrown away. A few characters of text move the
            // frame difference by about a tenth, well under the 0.3 tolerance,
            // and that tolerance exists for good reason - lowering it until
            // text registers would make compression noise register too.
            //
            // So the duplicate is presented directly, unchanged. One present,
            // no generation, and the content is by definition nearly identical
            // to what is already on screen - it cannot look wrong, and it
            // carries the small change that matters.
            // ONLY INTO A GAP, never on top of a full stream.
            //
            // The first version presented every duplicate the moment it
            // arrived, whatever else was going out. Lukas saw the result
            // immediately: with the cap at 71 the output reached 148 instead of
            // 142, and it "kann ungleichmaessig wirken" - which it is. Those
            // extra presents are not paced; they squeeze in between two frames
            // that were, and a present that arrives early is exactly the shape
            // of judder.
            //
            // The output cadence is half a source interval, because that is
            // what doubling means - 7.0 ms at 71 fps. So a duplicate goes out
            // only if that long has passed with nothing presented. Then it
            // fills a hole that would otherwise hold a stale picture, and it
            // can never add to a stream that is already complete.
            //
            // Deliberately measured against the SOURCE period and not the
            // display refresh: the output tracks the game, not the panel, which
            // is the whole point of removing the slot wait.
            const double dupMinSpacingMs = (lockedPeriodMs > 1.0 && lockedPeriodMs < 100.0)
                                         ? lockedPeriodMs * 0.5 : 7.0;
            if (capturedTex && overlayVisibleLastIteration
                    && (lastPresentAtMs <= 0.0
                        || NowMs() - lastPresentAtMs >= dupMinSpacingMs)) {
                presenter.PresentFrame(context.get(), capturedTex, presentSyncInterval);
                ++duplicatePassthroughs;
                RecordPresentGap(NowMs(), false);
            }

            // Recomposited but unchanged: no new content to ESTIMATE from.
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
                // DIVIDE BY THE FRAMES THIS GAP ACTUALLY SPANS.
                //
                // This is the gap between two frames we CONSUMED, and the loop
                // does not consume every arrival - when it steps over one, the
                // gap covers two source frames and reads double. That number
                // then sets the pacing period, the longer period makes the loop
                // step over one again, and it holds itself there:
                //
                //   16:26:09  arrival spacing 13.908  measured interval 24.298
                //             locked period 26.032 (38.4 fps)  source 72
                //             native 38.0  generated 38.0  out 75.9
                //
                // Eight seconds of a 72 fps source being served at 38, with no
                // duplicates and nothing queued - the engine had halved itself
                // and had no way back, because every measurement it took
                // confirmed the rate it had settled on.
                //
                // The arrival spacing beside it read a steady 13.9 the whole
                // time and is not used here on purpose: it counts overlay and
                // cursor updates the game never drew, and locked the period at
                // 124 fps against a 68 fps source once already.
                //
                // The count of stepped-over arrivals is exact, so the gap can
                // be divided by the frames it really spans instead. 24.298 over
                // two frames is 12.1, which paces at the source rate, which
                // stops the loop stepping over anything - the same feedback
                // running in the direction that recovers.
                if (staleThisPoll > 0 && staleThisPoll < 8) {
                    intervalMs /= static_cast<double>(staleThisPoll + 1);
                }
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

                    // Advance the schedule clock alongside the real one.
                    if (!smoothClockEnabled || scheduleAnchorMs <= 0.0) {
                        scheduleAnchorMs = motionCurrTimestampMs;
                    } else {
                        // RESYNC ON A RUN, NOT ON ONE SAMPLE.
                        //
                        // Resyncing whenever a single arrival missed by half an
                        // interval threw the model away about ten times a
                        // second - 758 resyncs climbing to 861 over the same ten
                        // seconds - so it smoothed nothing and was pure
                        // overhead. The cause is the source itself: arrival
                        // spacing has a standard deviation of 3-5 ms with
                        // bursts down to 0.07 ms, so a 7 ms miss is ordinary
                        // rather than exceptional.
                        //
                        // One large error is a burst, which is exactly what the
                        // model should ignore. Three in a row is a source that
                        // has genuinely changed rate or stalled, which is what
                        // it must not sit through. That distinction is the whole
                        // point of the guard, and measuring it per-sample made
                        // it fire on the noise instead.
                        const double step = PacingInterval();
                        const double predicted = scheduleAnchorMs + step;
                        const double error = motionCurrTimestampMs - predicted;
                        const bool missed = std::abs(error) > step * 0.5;
                        if (missed) ++scheduleMissRun; else scheduleMissRun = 0;

                        if (scheduleMissRun >= 3) {
                            scheduleAnchorMs = motionCurrTimestampMs;
                            scheduleMissRun = 0;
                            ++scheduleResyncs;
                        } else if (missed) {
                            // An outlier does not reach the filter at all.
                            //
                            // Pulling a tenth of the way toward a burst still
                            // moves the schedule by more than a millisecond on a
                            // 14 ms error, and bursts repeat - so a "small"
                            // correction toward nonsense accumulates into drift.
                            // The sample is rejected and the clock simply
                            // advances by its own step; three of them in a row
                            // resynchronise it above instead.
                            scheduleAnchorMs = predicted;
                        } else {
                            scheduleAnchorMs = predicted + error * 0.1;
                        }
                    }
                    motionCurrArrivalMs = NowMs();
                }
            }
        }

        if (ranEstimationThisTick) {
            // Is this a still picture? Judged over several frames in a row so a
            // single quiet frame during gameplay cannot switch generation off.
            // A Schmitt trigger on the AVERAGE: two thresholds, one smoothed
            // signal.
            //
            // With a single 5% line and only three moving frames needed to
            // resume, this flapped - hidden, shown, hidden, shown three times
            // in five seconds in the War Thunder pause menu, which has a live
            // 3D background of water, clouds and the aircraft on deck. The
            // share of moving blocks pendulums around the line, and every
            // moment it crossed upward we started generating again. The tearing
            // was in exactly those moments.
            //
            // Stop below 5%, resume only above 15%, and require half a second
            // of it. Gameplay measures 38-86%, so nothing real is kept waiting
            // by a resume threshold three times the stopping one.
            const double movingPercent = motionStats.MovingBlockPercent();
            if (movingPercent >= 0.0) {
                // ~10 frames of memory: long enough to ride out a single quiet
                // or busy frame, short enough that opening a menu is noticed
                // within a fifth of a second.
                movingEma = (movingEma < 0.0) ? movingPercent
                                              : movingEma * 0.9 + movingPercent * 0.1;

                // Stop under 2%, resume over 4%.
                //
                // It was 3 and 8, and that left the booster switched off for 91
                // seconds during an active sortie: "20:48:48 Still picture -
                // standing aside" through "20:50:19 Picture moving again". Not
                // a hiccup - a minute and a half of raw game, and then the jump
                // back to 144. Reported as the picture briefly stopping
                // altogether.
                //
                // The share of moving blocks runs at 7-13% while flying, which
                // sat right against the old resume line. It is that low because
                // of what the metric counts: a block gets a non-zero vector
                // only if it can be matched, and uniform sky cannot, so a
                // sky-heavy game reads as barely moving while the whole world
                // sweeps past.
                //
                // Menus measure 0.02-2.13%, so 2 and 4 separate them with room
                // to spare and resume within a few frames instead of a minute.
                // FEW BLOCKS MOVING IS NOT THE SAME AS NOTHING HAPPENING.
                //
                // 2 and 4 still left the booster flapping once a second, and
                // this time the measurement says plainly why:
                //
                //   23:25:45  moving 2.198%  diff 5.49  out   0.0  standing
                //   23:25:46  moving 2.326%  diff 4.50  out   0.0  standing
                //   23:25:48  moving 7.422%  diff 6.81  out 123.5  no
                //   23:25:49  moving 2.653%  diff 4.19  out 109.8  standing
                //   23:25:54  moving 7.156%  diff 5.03  out  73.3  no
                //
                // The content sits in the dead zone between the two lines and
                // crosses both several times a second. Moving the lines does not
                // fix that - wherever they go, some scene will sit on them.
                //
                // What fixes it is the column beside it. On a REAL still picture,
                // measured on the desktop at 21:32, the frame-to-frame difference
                // reads 0.00-0.16. Here it reads 3.6-6.8 - the picture is changing
                // every single frame. Few blocks cross the motion threshold because
                // of what that metric counts: a block gets a vector only if it can
                // be matched, and slow, small or low-contrast motion - a cockpit at
                // cruise, distant terrain, an animated menu backdrop - moves the
                // pixels without ever winning a block match.
                //
                // So the two questions are different, and standing aside needs both
                // answered: hardly any block moved AND the picture did not change.
                // The gap between 0.16 and 3.6 is wide enough that no threshold in
                // it is delicate; 1.0 sits with a factor of six either side.
                //
                // Smoothed because the raw difference spikes - 16.36 appears in the
                // desktop log between readings of 0.00, and one spike must not
                // restart generation any more than one quiet frame must stop it.
                if (!stillGuardEnabled) {
                    pictureIsStill = false;
                } else {
                const double diffNow = duplicateDetector.LastDifference();
                diffEma = (diffEma < 0.0) ? diffNow : diffEma * 0.9 + diffNow * 0.1;

                constexpr double kStillDiff = 1.0;
                constexpr double kMovingDiff = 2.0;

                // HYSTERESIS IN TIME, BECAUSE HYSTERESIS IN VALUE KEEPS LOSING.
                //
                // Adding the difference test fixed gameplay - fifty seconds at
                // 124-147 fps with no dropout where it used to flap every
                // second - and then flapped again on a near-static screen,
                // because the difference EMA sat exactly on the new line:
                //
                //   23:30:29  moving 0.16%  diff 0.89  out  67.9  standing
                //   23:30:31  moving 0.97%  diff 1.44  out   0.0  standing
                //   23:30:32  moving 5.85%  diff 2.91  out  96.0  no
                //   23:30:33  moving 0.54%  diff 1.10  out 147.2  no
                //   23:30:35  moving 0.07%  diff 0.79  out   0.0  standing
                //
                // 0.79, 0.89, 0.91, 1.01, 1.10, 1.19 against a threshold of
                // 1.0. That is the fourth threshold pair to be defeated the
                // same way, and moving it a fifth time would only choose which
                // scene sits on it next.
                //
                // So the value decides WHAT the state should be and the clock
                // decides HOW OFTEN it may change. A second of dwell costs at
                // most a second of generating into a menu, or a second of raw
                // game after one opens - both far below what a switch every
                // second costs, which is what is actually visible.
                //
                // This cannot be defeated by content sitting on a line, because
                // it does not ask where the content sits.
                constexpr double kStillDwellMs = 1000.0;
                const double nowStillMs = NowMs();
                const bool mayFlip = (lastStillFlipMs <= 0.0)
                                  || (nowStillMs - lastStillFlipMs >= kStillDwellMs);

                if (mayFlip && !pictureIsStill && movingEma < 2.0 && diffEma < kStillDiff) {
                    pictureIsStill = true;
                    lastStillFlipMs = nowStillMs;
                    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Still picture (few blocks moving AND the"
                        " picture itself unchanged) - standing aside. A generated frame between two"
                        " identical ones carries no information and can only be wrong; menus are where"
                        " that shows.");
                } else if (mayFlip && pictureIsStill && (movingEma > 4.0 || diffEma > kMovingDiff)) {
                    pictureIsStill = false;
                    lastStillFlipMs = nowStillMs;
                    FrameBoostBeta::Logger::Log("[FrameBoostBeta] Picture moving again - generating.");
                }
                }
            }
            if (pictureIsStill) ++stillSecondsSinceReport;

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
                costHistory[costHistoryNext] = costMs;
                interpHistory[costHistoryNext] = (interpMs >= 0.0) ? interpMs : 0.0;
                costHistoryNext = (costHistoryNext + 1) % kCostHistorySize;
                if (costHistoryCount < kCostHistorySize) ++costHistoryCount;

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
                    // A "quality" mode that added a whole source period of latency to
                    // move this line was tried and removed. Latency buys a later
                    // deadline, not throughput: 72 generated frames a second at
                    // 9 ms each is 650 ms of graphics card per second, the game
                    // starves, and its own rate fell from 72 to 63 while this
                    // still read "on". The guard was right; the work was too
                    // expensive per frame, and no deadline changes that.
                    // Judged on the MEDIAN cost, not on an average a spike can
                    // drag up.
                    //
                    // Measured while the engine had switched itself off in War
                    // Thunder: "interpolation 2.8 ms median, 15.1 ms at the 95th
                    // percentile, against a 8.3 ms deadline". The median says we
                    // fit three times over. The verdict was being made on an EMA
                    // that a single 15 ms sample lifts above the line for the
                    // next several seconds.
                    //
                    // And those samples are the least trustworthy ones we have.
                    // GPU timestamp queries measure ELAPSED time on the GPU
                    // timeline, not exclusive occupancy, so under contention our
                    // dispatch appears to take as long as the game's frame it is
                    // queued behind. The same artefact produced "we take 43% of
                    // the graphics card" earlier today, which Task Manager put
                    // at 13%. A tail measured that way is not evidence of
                    // anything, and a guard built on it switches the feature off
                    // in exactly the games that need it most.
                    //
                    // The median of sixty samples ignores the tail and still
                    // reacts within a second to a real change in cost.
                    double medianCostMs = generationCostEmaMs;
                    if (costHistoryCount >= 15) {
                        std::vector<double> costs(costHistory, costHistory + costHistoryCount);
                        std::sort(costs.begin(), costs.end());
                        medianCostMs = costs[costs.size() / 2];
                    }
                    // The question is whether we can sustain generation at all,
                    // not whether we are comfortable.
                    //
                    // At 0.7 of a source interval the guard fired while the
                    // engine was hitting its deadline 99.8% of the time. That
                    // threshold answers "is this getting expensive", and the
                    // honest consequence of expensive-but-affordable is a
                    // missed slot here and there, which the pacing already
                    // reports and the viewer does not notice. Switching the
                    // whole feature off is not a proportionate response to it -
                    // and the viewer notices THAT immediately.
                    //
                    // Generation genuinely cannot be sustained only when one
                    // generated frame costs about as much as a whole source
                    // interval: then the work for frame N is still running when
                    // frame N+1 arrives and the engine falls permanently
                    // behind. That is the line worth guarding, and it is 1.0,
                    // not 0.7. Return at 0.8 keeps the gap that stops it
                    // oscillating around the threshold.
                    const bool hasRoom = gpuHasRoom
                        ? (medianCostMs < realFrameIntervalEmaMs * 1.0)   // leave only when we cannot keep up
                        : (medianCostMs < realFrameIntervalEmaMs * 0.8);  // return with a margin

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
        const bool overlayShowing = doublingFitsDisplay && gpuHasRoom
                                 && !forcePassthroughOnly && !pictureIsStill;
        SetOverlayVisible(overlayShowing);
        overlayVisibleLastIteration = overlayShowing;

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

            // A real frame ends the famine, whatever it contained.
            if (haveNewContent) gapFillsInARow = 0;

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
            // HOW MANY OUTPUT FRAMES THIS REAL FRAME IS WORTH - fractional.
            //
            // This was a flat 2, and a flat 2 is only even when twice the
            // source lands exactly on the panel. 72 doubles to 144 and is
            // perfect; 60 doubles to 120 on a 144 Hz display and the missing 24
            // frames a second are the judder. The comment on the refresh grid
            // further up reached the same conclusion and gave up on snapping
            // because the cure "was always a demand on the user - cap the game".
            //
            // There is a third option it did not consider, and it is what
            // Lossless Scaling shipped in 3.1 as Adaptive Frame Generation:
            // keep the output on the display rate and let the MULTIPLIER be
            // fractional. 144/60 is 2.4, spent as 2,2,3,2,2,3 - which averages
            // exactly 2.4 and asks nothing of the person playing. Their own
            // note says it is for sources that are "not integer multiples of
            // the screen refresh (e.g. 60 -> 144, 165 Hz)" and that it paces
            // more smoothly than a fixed multiplier.
            //
            // At a 72 fps source this is 144/72 = 2.0 exactly, so the credit
            // never carries and the behaviour is bit-for-bit the old flat 2.
            // The case Lukas already has working does not change.
            //
            // The loop below turns this into outputPerReal - 1 generated frames
            // at evenly spaced phases, so the whole distribution already exists
            // and only the count had to stop being a constant.
            // ONCE PER REAL FRAME, not once per turn of the loop.
            //
            // The first version advanced the credit at the top of this block,
            // which runs on every iteration - hundreds of times a second, not
            // 60. The counter then reported 3448 extra frames a second, a
            // number that cannot happen, and that impossibility is the only
            // reason it was caught before it was shipped as working.
            if (adaptiveOutput && haveNewContent && outputRefreshHz > 0.0
                    && lockedPeriodMs > 1.0 && lockedPeriodMs < 100.0) {
                const double srcFps = 1000.0 / lockedPeriodMs;
                double wanted = outputRefreshHz / srcFps;
                // Below 1 means the source already fills the panel: generate
                // nothing rather than invent frames there is no room to show.
                if (wanted < 1.0) wanted = 1.0;
                // BRACES. This clamp lost its body to a bad edit and the next
                // statement became it, so the credit only advanced when the
                // multiplier was ABOVE the ceiling - which is never, in the case
                // this was built for:
                //
                //   16:55:00  src 60.9  gen 5.0  out 65.9  opr 1  factor-1 skips 60.9/s
                //   16:55:38  src 36.0  gen 103.2 out 133.4 opr 5  factor-1 skips 0.0/s
                //
                // At 60 fps the multiplier is 2.5, never above 4, so the credit
                // stayed at zero and every real frame was skipped for a factor of
                // one. At 36 it is 4.1, above the ceiling, so it ran - which is
                // why this looked like "adaptive works sometimes" rather than
                // like a syntax accident.
                if (wanted > static_cast<double>(adaptiveMaxFactor)) {
                    wanted = static_cast<double>(adaptiveMaxFactor);
                }
                outputCredit += wanted;
                adaptiveOutputPerReal = static_cast<int>(outputCredit);
                if (adaptiveOutputPerReal < 1) adaptiveOutputPerReal = 1;
                outputCredit -= static_cast<double>(adaptiveOutputPerReal);
                // The carry is bounded on both sides. Letting it run negative
                // or past one frame would let a stretch of bad measurements
                // borrow frames from the future and pay them back in a burst,
                // which is the uneven output this exists to remove.
                if (outputCredit < 0.0) outputCredit = 0.0;
                if (outputCredit > 1.0) outputCredit = 1.0;
                if (adaptiveOutputPerReal > 2) adaptiveExtraFrames += (adaptiveOutputPerReal - 2);
            }
            const int outputPerReal = adaptiveOutput ? adaptiveOutputPerReal : 2;



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
                        && !pictureIsStill
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

                    const double realDueAtMs = motionCurrTimestampMs + offsetMs
                        - realPhaseCorrectionMs;
                    const double realCeilingMs = NowMs() + 20.0;
                    WaitUntilMs(realDueAtMs, realCeilingMs);

                    if (transparentRealFrames && presenter.SupportsTransparency()) {
                        presenter.PresentTransparent(device.get(), context.get(), presentSyncInterval);
                    } else if (estimator.CurrFrameTexture()) {
                        presenter.PresentFrame(context.get(), estimator.CurrFrameTexture(), presentSyncInterval);
                    }
                    if (measureOutputDiff) MeasureOutputFrame(estimator.CurrFrameTexture(), realDiffSum, realDiffCount);
                    ++nativeFramesSinceReport;
                    RecordContentStep(motionCurrTimestampMs);
                    RecordPresentGap(NowMs(), false);
                    RecordPresentAge();

                    // Half an interval after the real frame ACTUALLY APPEARED, not
                    // after the timestamp it was captured with.
                    //
                    // Those are not the same moment. Between a frame being
                    // drawn by the game and being shown by us lies the
                    // capture, the wait for a refresh boundary, and whatever
                    // else the loop was doing - and the real frame absorbs all
                    // of it while the generated one, planned from the original
                    // timestamp, did not. It therefore arrived late by exactly
                    // that delay, every single time.
                    //
                    // Measured with the two spacings separated, at a 13.9 ms
                    // source period where both should be 6.94 ms:
                    // real -> generated 10.00 ms, generated -> real 7.55 ms,
                    // identical to two decimals across every second. Not
                    // jitter, not refresh quantisation - a constant 3 ms of
                    // bias. Reported before it was measured, as the spacing
                    // between generated and real frames being uneven.
                    // Anchored to the capture clock as before, but corrected by how
                    // late the real frame actually appeared - averaged, not
                    // taken from this one frame.
                    //
                    // Re-anchoring directly to "when the real frame appeared"
                    // did equalise the two spacings - 10.00 ms became 7.13
                    // against 7.54 - and made everything else worse: native FPS
                    // 69.6-71.5 -> 64.2-67.0, content-step deviation 1.16-1.97
                    // -> 2.54-3.49. A late real frame pushed the generated one
                    // with it, which delayed the next real frame, which pushed
                    // again. A clock that follows its own output has nothing
                    // left to correct against.
                    //
                    // The bias is real and worth removing; the jitter around it
                    // is not worth inheriting. So the delay is measured slowly
                    // and applied as a constant, and the schedule stays tied to
                    // the capture timestamps, which do not drift.
                    const double shownLateByMs = NowMs() - (motionCurrTimestampMs + offsetMs);
                    if (shownLateByMs > -20.0 && shownLateByMs < 20.0) {
                        presentBiasEmaMs = presentBiasEmaMs < -100.0
                            ? shownLateByMs
                            : presentBiasEmaMs * 0.9 + shownLateByMs * 0.1;
                    }
                    generatedDueAtMs = motionCurrTimestampMs + pairIntervalMs * 0.5 + offsetMs
                                     + presentBiasEmaMs;
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

                    // Where the camera has been turned since the last real frame.
                    // The generated frame stands half an interval ahead of it,
                    // so half the movement is what it should already show.
                    //
                    // The sign is inverted because a motion vector points from
                    // where content is now to where it WAS: turning right moves
                    // the world left, so the vector points right.
                    interpolator.SetStatusFlags(badgeFlag | lowLatencyFlag
                        | ((transparentRealFrames && presenter.SupportsTransparency()) ? 2u : 0u));

                    ID3D11UnorderedAccessView* uav = measureOutputDiff
                        ? nullptr
                        : presenter.AcquireBackBufferUAV(device.get());
                    if (interpolator.GenerateFrame(device.get(), context.get(),
                            estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                            desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, uav)) {
                        lastInterpolationRunMs = NowMs();

                        const double genCeilingMs = NowMs() + 20.0;
                        while (NowMs() < generatedDueAtMs && NowMs() < genCeilingMs) { ddCapture.Pump(); }

                        if (ContentWouldGoBackwards(generatedContentMs, lockedPeriodMs,
                                duplicateDetector.LastDifference())) {
                            ++generatedSkippedBackwards;
                        } else {
                        if (uav) presenter.PresentBackBuffer(presentSyncInterval);
                        else presenter.PresentFrame(context.get(), interpolator.GeneratedFrameTexture(), presentSyncInterval);
                        if (measureOutputDiff) MeasureOutputFrame(interpolator.GeneratedFrameTexture(), generatedDiffSum, generatedDiffCount);
                        ++generatedFramesSinceReport;
                        RecordContentStep(generatedContentMs);
                        RecordPresentGap(NowMs(), true);
                        RecordPresentAge();
                        }
                    }
                    generatedPendingSimple = false;
                }

                Sleep(0);
                ReportTelemetryIfDue();
                continue;
            }

            if (haveNewContent) {
                // Counted, not deduced. Exactly one of these fires per real
                // frame that produces nothing, and the telemetry prints them
                // per second, so "generated 4 at a 60 fps source" names its own
                // cause instead of having to be reasoned out of the numbers
                // around it. Two rounds of reasoning from the surrounding
                // fields produced two contradictory explanations and no answer.
                if (!haveMotionField) ++skipNoMotionField;
                else if (!gpuHasRoom) ++skipNoGpuRoom;
                else if (inDegradedMode) ++skipDegraded;
                else if (pictureIsStill) ++skipStill;
                else if (outputPerReal < 2) ++skipFactorOne;
            }
            if (haveNewContent && haveMotionField && doublingFitsDisplay && gpuHasRoom
                    && !forcePassthroughOnly && !inDegradedMode && !pictureIsStill
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
                    // The phase follows WHEN the frame will actually be shown.
                    //
                    // The generated frame carries content from the middle of
                    // its pair, and it is shown 7.19 ms after the real frame
                    // where the middle is 8.4 ms - so its content belongs to a
                    // moment 1.2 ms later than the moment it appears. The eye
                    // judges smoothness by whether content advances evenly on
                    // the display.s timeline, and a frame whose content does
                    // not match its display time is judder however evenly the
                    // presents themselves are spaced.
                    //
                    // Delaying the frame to match its content was tried first
                    // and was immediately worse - it adds latency to the half
                    // of the stream that was on time. Moving the content to
                    // match the timing costs nothing: the interpolator can
                    // produce any phase, and 0.428 is no harder than 0.5.
                    float phaseForStep = noWarpDiagnostic
                        ? 0.0f
                        : static_cast<float>(step) / static_cast<float>(outputPerReal);
                    if (!noWarpDiagnostic && realToGenCount > 10 && pairIntervalMs > 1.0) {
                        const double measured = (realToGenSum / realToGenCount) / pairIntervalMs;
                        // WHERE THE FIRST ONE LANDS, then one slot per step.
                        //
                        // This assigned "measured" to every step. With one
                        // generated frame per real frame that is exactly right,
                        // and it was written when outputPerReal was always 2.
                        // Adaptive output made the count vary, and at a factor
                        // of 4 all three intermediate frames were handed the
                        // SAME content phase:
                        //
                        //   content:  0.42 -> 0.42 -> 0.42 -> 1.00
                        //
                        // Three frames of one moment, then the real frame
                        // jumping the remaining 0.58. The output counter reads
                        // 144 and the picture advances in steps - "nicht
                        // gleichmaessig verteilt", measured from the inside.
                        //
                        // The measurement says where the FIRST generated frame
                        // lands after a real one. The rest follow it one output
                        // slot apart, which is 1/outputPerReal of the pair - so
                        // the correction shifts the whole ladder instead of
                        // collapsing it. At outputPerReal = 2 there is only
                        // step 1 and this is bit-for-bit what it was.
                        if (measured > 0.2 && measured < 0.8) {
                            const double slot = 1.0 / static_cast<double>(outputPerReal);
                            double shifted = measured + (step - 1) * slot;
                            // A phase at either end is a copy of a real frame,
                            // which is what this whole engine exists to avoid.
                            if (shifted > 0.95) shifted = 0.95;
                            if (shifted < 0.05) shifted = 0.05;
                            phaseForStep = static_cast<float>(shifted);
                        }
                    }
                    interpolator.SetPhase(phaseForStep);
                    interpolator.SetStatusFlags(badgeFlag | lowLatencyFlag
                        | ((transparentRealFrames && presenter.SupportsTransparency()) ? 2u : 0u));

                    ID3D11UnorderedAccessView* uav = presenter.AcquireBackBufferUAV(device.get());
                    if (!interpolator.GenerateFrame(device.get(), context.get(),
                            estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                            desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, uav)) {
                        ++skipGenerateFailed;
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
                    // Scheduled on the CAPTURE clock, like the real frame beside it.
                    //
                    // This read "nowMs + interval * (step-1)/outputPerReal",
                    // and at a factor of two that second term is exactly zero -
                    // so the generated frame went out the instant the loop
                    // noticed the arrival, with no schedule at all. Every
                    // hiccup of our own loop landed straight in the spacing:
                    // two arrivals close together sent two frames out close
                    // together, then a gap. Described from the screen, with the
                    // generated frames tinted red, as morse code - bursts with
                    // pauses between them.
                    //
                    // The real frame is already anchored to its capture
                    // timestamp plus the arrival lag plus half an interval.
                    // The generated frame holds the content halfway between
                    // this pair, so it belongs exactly half an interval
                    // earlier - on the same clock, not on ours.
                    const double dueAtMs = scheduleAnchorMs + arrivalLagEmaMs
                        + pairIntervalMs * (static_cast<double>(step - 1) / outputPerReal);
                    // Same backstop as below: never wait longer than one source
                    // interval, so no arithmetic mistake can freeze the picture.
                    // A fixed ceiling, not one derived from the interval: a wait
                    // must never be able to inherit a bad measurement.
                    const double genCeilingMs = NowMs() + 20.0;
                    // ALREADY TOO LATE: drop it rather than show it out of place.
                    //
                    // A generated frame carries a specific instant between two
                    // real ones. Presenting it after that instant has gone does
                    // not add smoothness - it puts a picture from the past in
                    // front of the viewer and pushes the real frame behind it
                    // further back. Half an output slot is the tolerance;
                    // beyond that the frame is worth less than the slot it
                    // would occupy.
                    // Drop it only when the REAL frame's own moment has
                    // arrived - not merely because this one is late.
                    //
                    // The first version dropped anything more than half an
                    // output slot late, on the reasoning that a frame shown
                    // after its instant adds no smoothness. Measured, that was
                    // wrong: 8-12 generated frames a second were being thrown
                    // away, and each one leaves a gap of 20-50 ms where a
                    // slightly stale picture would have been. The output
                    // interval ran from 6.7 ms to 56 ms with 42-77% of slots
                    // missed. The rule was producing the stutter it was written
                    // to prevent.
                    //
                    // A late frame still advances the picture. The only case
                    // where showing it genuinely hurts is when the real frame
                    // it precedes is already due, because then it would appear
                    // out of order - so that, and nothing weaker, is the test.
                    // MEASURED AGAINST THE SAME CLOCK THE FRAME WAS SCHEDULED
                    // ON, which since the smoothed schedule clock went in is no
                    // longer the raw timestamp.
                    //
                    // Left on motionCurrTimestampMs it compared a due time
                    // computed from the model against a deadline computed from
                    // the measurement, and the two differ by exactly the jitter
                    // the model exists to absorb - so a frame sitting precisely
                    // where it was planned could be thrown away for being late.
                    // A bug I introduced twenty minutes ago and found by reading
                    // rather than by Lukas reporting dropped frames.
                    const double realMomentMs = scheduleAnchorMs + arrivalLagEmaMs
                        + pairIntervalMs * (static_cast<double>(outputPerReal - 1) / outputPerReal);
                    const double slackMs = realMomentMs - NowMs();
                    if (slackMs < 0.0) {
                        ++generatedDroppedLate;
                        droppedLateByMsSum += -slackMs;
                        if (-slackMs > droppedLateByMsMax) droppedLateByMsMax = -slackMs;
                        continue;
                    }
                    slackWhenOnTimeMsSum += slackMs;
                    ++slackWhenOnTimeCount;
                    // The moment this frame represents, in the source's own
                    // timeline. Computed before the wait so the skip decision
                    // does not depend on when we got here.
                    const double generatedMomentMs = motionPrevTimestampMs
                        + phaseForStep * (motionCurrTimestampMs - motionPrevTimestampMs);
                    if (ContentWouldGoBackwards(generatedMomentMs, lockedPeriodMs,
                            duplicateDetector.LastDifference())) {
                        ++generatedSkippedBackwards;
                        continue;
                    }

                    WaitUntilMs(dueAtMs, genCeilingMs);
                    WaitForDisplaySlot();
                    WaitForRefreshBoundary();

                    if (uav) presenter.PresentBackBuffer(presentSyncInterval);
                    else presenter.PresentFrame(context.get(), interpolator.GeneratedFrameTexture(), presentSyncInterval);
                    ++generatedFramesSinceReport;
                    RecordContentStep(generatedMomentMs);
                    RecordPresentGap(NowMs(), true);
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
                // The same clock as the generated frames above - the capture
                // timestamp plus the arrival lag - not "now".
                //
                // This read "nowMs + interval * (outputPerReal-1)/outputPerReal"
                // while the generated frame beside it had already been moved
                // onto the capture clock, so the two hung on different clocks
                // again: measured 8.53 ms from real to generated and 9.45 ms
                // back, where both should be half a source period. The same
                // mistake as the generated frame had, mirrored, in the twin
                // branch - fixing one of a pair and not looking for the other.
                const double realDueAtMs = scheduleAnchorMs + arrivalLagEmaMs
                    + pairIntervalMs * (static_cast<double>(outputPerReal - 1) / outputPerReal)
                    - realPhaseCorrectionMs;

                // Never wait longer than one source interval, whatever the
                // arithmetic says. A wait that can grow without bound is how the
                // picture froze; this is the backstop that makes that
                // impossible rather than merely unlikely.
                const double waitCeilingMs = NowMs() + 20.0;
                WaitUntilMs(realDueAtMs, waitCeilingMs);
                WaitForDisplaySlot();

                WaitForRefreshBoundary();
                if (transparentRealFrames && presenter.SupportsTransparency()) {
                    presenter.PresentTransparent(device.get(), context.get(), presentSyncInterval);
                } else if (estimator.CurrFrameTexture()) {
                    presenter.PresentFrame(context.get(), estimator.CurrFrameTexture(), presentSyncInterval);
                }
                ++nativeFramesSinceReport;
                RecordContentStep(motionCurrTimestampMs);
                RecordPresentGap(NowMs(), false);
                RecordPresentAge();
            }

            // GAP FILLER: keep the picture moving while the source is late.
            //
            // Measured on screen, twice over. The engine's own telemetry: jitter
            // 10.4 ms in a second whose source deviation was 34.5% and whose
            // largest content step was 72.5 ms. The independent audit, which
            // generates and presents nothing of its own: unique-content spacing
            // 8.45 ms mean but max 27.78 ms. The game delivers its ~72 frames
            // but bunched - one long pause, then the rest. Reported as "manchmal
            // hab ich das gefuehl es dropt auf 30 fps", and for 72 ms it is
            // worse than that: it is 14.
            //
            // Nothing in our scheduling causes it and nothing in our scheduling
            // can repair it after the fact. PacingInterval() returns the LOCKED
            // period, not the raw pair interval, so a long gap does not stretch
            // the hold on the newest real frame - that was checked before this
            // was written, because it would have been the cheaper fix. During
            // those 72 ms the loop simply has nothing new to show and the screen
            // holds its last picture.
            //
            // So put something there. The newest real frame is carried FORWARD
            // along the motion it already had - the same prediction the
            // "extrapolate" mode uses, applied only where interpolation has
            // nothing to offer because the second frame does not exist yet.
            // This is what a VR headset does when a frame misses its deadline,
            // and for the same reason: a picture that keeps moving beats a
            // picture that is briefly correct and frozen.
            //
            // Bounded on both sides. It starts only once the source is a third
            // of an interval late, so ordinary jitter never triggers it, and it
            // stops after kMaxGapFills frames - about 55 ms - because a
            // prediction drifts further from the truth the longer it runs, and
            // a genuinely paused game should look paused.
            if (!extrapolateMode && !gapFillOff && !forcePassthroughOnly && !inDegradedMode
                    && !pictureIsStill && gpuHasRoom
                    && outputSlotMs > 0.0 && lockedPeriodMs > 1.0
                    && motionCurrTimestampMs > 0.0 && lastPresentAtMs > 0.0
                    && gapFillsInARow < kMaxGapFills
                    && estimator.PrevFrameSRV() && estimator.CurrFrameSRV()
                    && estimator.MotionVectorSRV()) {
                const double fillNowMs = NowMs();
                const double sinceRealMs = fillNowMs - (motionCurrTimestampMs + arrivalLagEmaMs);
                const double sincePresentMs = fillNowMs - lastPresentAtMs;

                // Same correction as the keep-alive below: the minimum spacing
                // is the OUTPUT cadence, half the source period, not the panel
                // slot. outputSlotMs only tracks the source when the "doublerate"
                // argument is passed and the app does not pass it, so this let a
                // fill land 6.94 ms after a present that was 13.9 ms apart from
                // its neighbour - an extra frame inside a gap that was already
                // the right size.
                const double fillMinSpacingMs = (lockedPeriodMs > 1.0 && lockedPeriodMs < 100.0)
                                              ? lockedPeriodMs * 0.5
                                              : (outputSlotMs > 0.0 ? outputSlotMs : 6.94);
                if (sinceRealMs > lockedPeriodMs * 1.3 && sincePresentMs >= fillMinSpacingMs) {
                    D3D11_TEXTURE2D_DESC fillDesc{};
                    if (ID3D11Texture2D* curTex = estimator.CurrFrameTexture())
                        curTex->GetDesc(&fillDesc);

                    if (fillDesc.Width > 0 && fillDesc.Height > 0) {
                        // How far past the newest real frame this picture sits,
                        // as a fraction of one interval - which is exactly the
                        // unit the motion field is in, since it measures the
                        // displacement across one interval.
                        const double ahead = sinceRealMs / lockedPeriodMs;
                        interpolator.SetExtrapolateAhead(
                            static_cast<float>(ahead > 1.0 ? 1.0 : ahead));

                        ID3D11UnorderedAccessView* uav = presenter.AcquireBackBufferUAV(device.get());
                        if (interpolator.GenerateFrame(device.get(), context.get(),
                                estimator.PrevFrameSRV(), estimator.CurrFrameSRV(),
                                estimator.MotionVectorSRV(),
                                fillDesc.Width, fillDesc.Height,
                                DXGI_FORMAT_B8G8R8A8_UNORM, uav)) {
                            if (uav) presenter.PresentBackBuffer(presentSyncInterval);
                            else presenter.PresentFrame(context.get(),
                                    interpolator.GeneratedFrameTexture(), presentSyncInterval);
                            ++generatedFramesSinceReport;
                            ++gapFillsSinceReport;
                            ++gapFillsInARow;
                            lastInterpolationRunMs = NowMs();
                            // Its content sits ahead of the newest real frame,
                            // so the content step is recorded there and not at
                            // the frame it was predicted from.
                            RecordContentStep(motionCurrTimestampMs + ahead * lockedPeriodMs);
                            RecordPresentGap(NowMs(), true);
                            RecordPresentAge();
                        }

                        // Back to interpolation for everything else. Left set,
                        // this would send the next ordinary generated frame down
                        // the extrapolation branch, which returns early and
                        // skips the whole interpolation path.
                        interpolator.SetExtrapolateAhead(0.0f);
                    }
                }
            }

            // KEEP-ALIVE: THE SCREEN MUST NEVER STOP FOLLOWING THE GAME.
            //
            // Reported: standing still, the output goes to 0 and stays there -
            // "ich sehe 0 bewegungen". The chain that produces it:
            //
            //   the picture barely changes -> the duplicate detector calls every
            //   arriving frame a duplicate (it compares a 64x36 thumbnail, which
            //   cannot see small movement) -> haveNewContent is cleared -> the
            //   gap filler covers about 55 ms and then stops by design, because
            //   "a genuinely paused game should look paused" -> and from there
            //   nothing is presented at all.
            //
            // The flaw in that last step is that our overlay is ON TOP. Stopping
            // presenting does not reveal the paused game; it leaves OUR last
            // picture on the screen. Anything that happens underneath - a menu
            // animation, the view moving when the mouse does - is never shown.
            // Measured in CS2: duplicates 42 of 42 arrivals, output 0.0, jitter
            // 576 ms.
            //
            // So when nothing has gone out for two output slots, the newest real
            // frame goes out again, unchanged. No generation, no extrapolation,
            // nothing that can drift - the worst case is showing a correct frame
            // twice, which is exactly what a paused game looks like anyway. It
            // costs one present and cannot make the picture wrong.
            // HOLD THE OUTPUT CADENCE, do not merely rescue a stall.
            //
            // This used to wait for two output slots of silence, so on a static
            // picture the output fell to zero and climbed back only when motion
            // returned. Lukas: "beim stehen geht es wieder auf 0 output fps
            // runter es soll immer auf max bleiben".
            //
            // He is right about the mechanism, and the reason is the transition
            // rather than the still picture. Stopping and restarting the present
            // stream is where the 500 ms jitter spikes measured this morning
            // came from; a cadence that never stops has no such transition. The
            // frames themselves add nothing while nothing moves - two identical
            // source frames have an identical frame between them - so this buys
            // steadiness, not detail, and that is the honest reason to do it.
            //
            // Half a source interval is the doubled cadence: 6.94 ms at 72 fps.
            // Measured against the SOURCE period rather than the display, like
            // everything else since the slot wait came out.
            const double cadenceMs = (lockedPeriodMs > 1.0 && lockedPeriodMs < 100.0)
                                   ? lockedPeriodMs * 0.5
                                   : (outputSlotMs > 0.0 ? outputSlotMs : 6.94);

            // A MARGIN, or this races the stream it is meant to back up.
            //
            // Firing at exactly the cadence means firing at exactly the moment
            // the scheduled frame is also due. Ordinary jitter then puts the
            // keep-alive a fraction earlier, the scheduled present follows right
            // behind it, and the output carries more frames than it should at
            // uneven spacing. Measured immediately: source 72, generated 71 -
            // which is 143 - against an output of 160.
            //
            // At one and a half cadences the scheduled present has always
            // happened and reset the timer, so during motion this never fires
            // at all. On a genuinely still picture, where nothing else
            // presents, it holds a steady stream on its own - slower than the
            // doubled rate, and steady, which is the property that was wanted.
            // THE DISPLAY SLOT IS THE TARGET, not half the source interval.
            //
            // Measured with the previous version: source 71, generated 72,
            // keep-alive 7 - an output of 150 where 144 was wanted. And when
            // standing still the source itself falls to 36, so doubling gives
            // 71.8 and no margin can turn that into 144.
            //
            // Both follow from pacing the keep-alive against the SOURCE. Paced
            // against the DISPLAY instead, one slot is 6.94 ms at 144 Hz and the
            // arithmetic works out in both states:
            //
            //   moving, source 72:  real and generated already fill every slot
            //                       6.94 ms apart, so this never fires
            //   still,  source 36:  the stream presents every 13.9 ms and this
            //                       fills the slot between, giving 144
            //
            // 1.3 slots as the threshold rather than exactly one: at exactly one
            // it fires at the same instant the scheduled frame is due and jitter
            // decides which goes first, which is what produced 160 earlier.
            //
            // Honest about what it does NOT fix: on a still picture the spacing
            // alternates rather than being even, because the filler goes in at
            // 9 ms after the last present and the next scheduled frame follows
            // 4.9 ms later. The count is right and the rhythm is not quite. On a
            // still picture every one of those frames is the same image, so the
            // unevenness has nothing to show through it - but it is there, and
            // pacing it to the midpoint of the expected gap is the real answer
            // if it ever matters.
            // THE OUTPUT CADENCE, NOT THE PANEL. cadenceMs is computed from the
            // locked source period a few lines up and was then thrown away here
            // in favour of outputSlotMs - and outputSlotMs only ever follows the
            // source when the "doublerate" argument is passed, which the app does
            // not pass. So it sat at 1000/144 = 6.94 ms whatever the source did,
            // and this fired at 9.02 ms while the real output cadence was 13.9:
            //
            //   16:33:25  nat 32.9  gen 33.9  out 66.8  keep-alive 52.8  gap fill 9.0
            //             presents actually reaching the panel: 128.6
            //
            // Half of everything on screen was an untimed repeat squeezed into a
            // gap that was not a gap. The output spacing shows it exactly: the
            // source arrived with 12% deviation and went out with 67%, min 0.83
            // ms, max 47.5 - two frames less than a millisecond apart, then a
            // hole. "Die generated sind nicht gleichmaessig wie die Input" is
            // this line.
            //
            // Measured against the cadence, a complete stream never trips it and
            // a genuinely stalled one still gets held up at ~55/s.
            const double slotMs = cadenceMs;
            constexpr double kKeepAliveMargin = 1.3;
            if (overlayShowing && presentedAnythingYet
                    && lastPresentAtMs > 0.0
                    && NowMs() - lastPresentAtMs >= slotMs * kKeepAliveMargin) {
                // THE NEWEST CAPTURED FRAME, which is not the same thing as the
                // newest frame the interpolator has.
                //
                // This presented estimator.CurrFrameTexture() - the last frame
                // that PASSED the duplicate test. The duplicate passthrough
                // added later the same day presents capturedTex, the newest
                // arrival. On a near-static picture both fire, alternating a
                // frame that contains a small change with an older one that does
                // not: typed characters appearing and disappearing seventy times
                // a second, which on text reads as shimmer.
                //
                // A bug I built by adding the passthrough after the keep-alive
                // without checking they draw from the same place. They must, or
                // the output steps backwards between them.
                //
                // capturedTex is the latest slot whether or not it is new, so it
                // is the right source in both cases; the estimator texture stays
                // as the fallback for the first frames, before any capture.
                ID3D11Texture2D* newest = capturedTex ? capturedTex
                                                      : estimator.CurrFrameTexture();
                if (newest) {
                    presenter.PresentFrame(context.get(), newest, presentSyncInterval);
                    ++keepAlivePresents;
                    // NOT counted into Native FPS, and the attempt to do so is
                    // worth leaving written down.
                    //
                    // This present was invisible in the output figure, which
                    // made a still picture held at a steady 108 frames a second
                    // read as an output of zero. Adding it to the native count
                    // fixed that number and broke a more important one:
                    //
                    //   16:26:52  src 35.0  nat 94.0  gen 33.0  out 127.0  ka 61.0
                    //
                    // 35 frames arrived and the output claimed 127. The
                    // keep-alive re-presents a frame that is already on screen,
                    // so counting it means counting the same picture several
                    // times - which is the one thing this engine must never do,
                    // because a frame rate that counts repeats is exactly the
                    // lie the whole project exists to avoid.
                    //
                    // Keep-alive/s beside it already says how many of these went
                    // out. That is the honest place for them: what reaches the
                    // panel, kept separate from how much of it is new.
                    RecordPresentGap(NowMs(), false);
                }
            }

            // GIVE THE CORE BACK WHEN WE ARE NOT USING IT.
            //
            // The comment that used to sit here said "yield without burning a
            // core", and it was wrong in the one way that matters. Sleep(0)
            // yields only to threads of EQUAL OR HIGHER priority. This process
            // runs at HIGH with a TIME_CRITICAL loop thread, so the game's
            // ordinary threads are not equal or higher, and Sleep(0) hands them
            // nothing at all.
            //
            // Measured while standing aside with the overlay hidden and output
            // at zero: 121% of a core, at HIGH priority, for no output
            // whatever. The game's input handling waits behind that, which is
            // what "die Menue-Buttons bleiben bisschen zurueck" is.
            //
            // So: Sleep(1) whenever there is nothing to pace, which genuinely
            // releases the core, and the priority itself is dropped to normal
            // while standing aside - see below. Sleep(0) stays for the case it
            // was meant for, a loop that is about to present on a schedule.
            const bool workingNow = !pictureIsStill && gpuHasRoom && !forcePassthroughOnly;
            if (workingNow != holdingHighPriority) {
                holdingHighPriority = workingNow;
                // Only the thread priority moves, and only within the modest
                // band. The process class is whatever was asked for at start.
                SetThreadPriority(GetCurrentThread(),
                                  workingNow ? THREAD_PRIORITY_ABOVE_NORMAL
                                             : THREAD_PRIORITY_NORMAL);
                FrameBoostBeta::Logger::Log(workingNow
                    ? "[FrameBoostBeta] Boosting."
                    : "[FrameBoostBeta] Standing aside - core released.");
            }
            if (workingNow) Sleep(0); else Sleep(1);
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
        if (queueCount < queueDepthMin) queueDepthMin = queueCount;
        if (queueCount > queueDepthMax) queueDepthMax = queueCount;
        queueDepthSum += queueCount;
        ++queueDepthSamples;

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

        // Snapped to a whole number of source periods before the phase is
        // computed from it.
        //
        // The raw difference between two capture timestamps is not the time
        // between two game frames. Those timestamps come from the compositor
        // and are quantised to ITS refresh grid - 6.94 ms at 144 Hz - while a
        // 72 fps game draws every 13.89 ms. That does not fit the grid, so the
        // same constant interval is reported alternately as two or three grid
        // steps. Measured against a game locked at 72 fps: 12.43, 14.00,
        // 14.51, 16.36, 13.42, 14.45 ms - a swing of 18% from a source that
        // was not varying at all.
        //
        // The phase is a fraction of this interval, so a wrong denominator
        // does not cost a frame - it changes the SPEED the content is shown
        // at, frame by frame, in step with the rounding error. The output can
        // be perfectly paced and still judder, which is what every clean
        // timing measurement next to a bad-looking picture has been saying.
        //
        // The true period is already known to within 2% from the lock, and a
        // game.s frame period is a constant. So the measurement is rounded to
        // the nearest whole multiple of it. Whole MULTIPLE, not the period
        // itself: when a frame is lost to coalescing the pair genuinely spans
        // two periods, the content really is twice as far apart, and forcing
        // that to one period would play it back at double speed.
        //
        // Pairs that span a gap are still interpolated rather than skipped.
        // Skipping would leave a hole exactly where a frame is already
        // missing, which is the one place the output can least afford one.
        double realIntervalMs = motionCurrTimestampMs - motionPrevTimestampMs;
        if (lockedPeriodMs > 1.0 && realIntervalMs > 0.0) {
            const double steps = realIntervalMs / lockedPeriodMs;
            const double nearest = std::floor(steps + 0.5);
            // Within a third of a period of a whole multiple, this is that
            // multiple seen through the grid. Further out, the source really
            // did something else (a stall, a scene change) and the measurement
            // is the better answer.
            if (nearest >= 1.0 && std::abs(steps - nearest) < 0.34)
                realIntervalMs = nearest * lockedPeriodMs;
        }
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
                // Proportional, with a hard brake when the queue runs dry.
                //
                // A flat 0.02 ms per frame is 2.9 ms of correction per second,
                // which cannot answer a queue that has just emptied. And an
                // empty queue is not a small error: the clock has nothing to
                // advance to, so the content STOPS, and when the next frame
                // arrives it jumps the whole accumulated distance at once.
                //
                // Measured over 471 seconds against the simple path: this mode
                // is three times steadier on average (content-step deviation
                // 0.79 against 2.38) and has five times as many hard stalls
                // (28% of seconds contain a jump above 14 ms, against 5.7%).
                // Both halves were reported in the same breath - "smoother"
                // and "unbelievable stutters" - and the queue.s minimum depth
                // of 0 is where the second half comes from.
                //
                // So the correction scales with how far off the depth is, and
                // an empty queue gets 0.5 ms at once. That slows the content
                // clock briefly, which is visible as a slight slowdown rather
                // than as a stall followed by a jump - the better of the two.
                // 3 was tried and measured no better: the mean depth stayed at 1.5
                // either way, because frames arrive and are consumed at the
                // same rate and the depth is set by the clock offset, not by
                // the target. Content steps above 14 ms became slightly more
                // frequent, so it went back.
                constexpr int kTargetQueueDepth = 2;
                constexpr double kClockNudgeMs = 0.02;
                constexpr double kEmptyQueueBrakeMs = 0.5;
                if (queueCount == 0) {
                    presentOffsetMs += kEmptyQueueBrakeMs;
                } else if (queueCount > kTargetQueueDepth) {
                    presentOffsetMs -= kClockNudgeMs * (queueCount - kTargetQueueDepth);
                } else if (queueCount < kTargetQueueDepth) {
                    presentOffsetMs += kClockNudgeMs * (kTargetQueueDepth - queueCount);
                }

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

        // Collect whatever the compositor has produced while this loop was
        // busy, immediately before the work starts and again after it.
        //
        // Every other Pump in this file sits inside a wait, so the capture was
        // asked constantly while the loop had nothing to do and not once while
        // it worked - and generating plus presenting a frame takes 4 to 10 ms,
        // against a compositor that produces one every 6.9 ms. Frames landing
        // in that window were merged by the OS before we ever saw them: 25 per
        // second, measured.
        //
        // Those losses are what break the content timeline. Measured across
        // twelve seconds, the single second in which native FPS read exactly
        // 72.0 had a content step of 6.94 ms with a standard deviation of
        // 0.22 - textbook. Every second that lost even one frame had a
        // deviation between 2.5 and 4.3 ms, with steps ranging from 0.4 to 19
        // ms. One missing frame does not cost one frame: it leaves a gap of
        // twice the period for the phase to cross, and the content lurches.
        if (useDesktopDuplication) ddCapture.Pump();

        if (wantGenerated) {
            interpolator.SetStatusFlags(badgeFlag | lowLatencyFlag
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

        // Present the real frame only when the overlay is actually on screen,
        // and only when there is something new in it.
        //
        // Without those two conditions this ran on EVERY turn of the loop.
        // Measured: 1198 native frames per second - the same picture presented
        // twelve hundred times a second, into an overlay that was hidden
        // anyway, because vsync is off and nothing here waits for anything.
        //
        // Invisible, and not harmless. It closes a loop: no GPU room, so
        // generation stands aside, so this path runs unthrottled, so there is
        // still no GPU room. That is most likely why War Thunder sat at
        // "standing aside (no GPU room)" while the game itself was struggling
        // at 22-32 fps - we were taking the card with presents nobody could see.
        if (!presentedGenerated && overlayShowing && haveNewContent) {
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

        if (useDesktopDuplication) ddCapture.Pump();

        const double presentEndMs = NowMs();
        // Only count and pace what was actually presented. Counting a present
        // that did not happen is how "Native FPS: 1198" looked plausible for as
        // long as it did.
        const bool presentedSomething = presentedGenerated || (overlayShowing && haveNewContent);
        if (presentedSomething) {
            if (presentedGenerated) ++generatedFramesSinceReport;
            else ++nativeFramesSinceReport;
            RecordPresentGap(presentEndMs, presentedGenerated);
            RecordPresentAge();
        }
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
