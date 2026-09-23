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
#include "bmp_writer.h"

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

// THE THREE FRAMES, WRITTEN OUT ONCE, so the generated one can be checked
// against its own parents with arithmetic instead of with an eye.
//
// Every attempt today to settle "does the generated frame carry a new moment"
// from a screen capture failed on its own noise: edge detection on a textured
// object wobbles by a pixel or two, which is enough to invent intermediate
// positions that are not there. These are the exact GPU surfaces, so there is
// nothing left to misread.
void DumpTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11Texture2D* tex, const std::wstring& path) {
    if (!tex) return;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);

    D3D11_TEXTURE2D_DESC s{};
    // MipLevels must MATCH, not be 1. The estimator's frame textures carry a
    // mip chain - the search runs on mip 1, 4 and 16 - and CopyResource
    // requires identical descriptions, so a staging texture with one level
    // fails silently and leaves a black image. That cost a round of confusion
    // here: two of three dumps came out empty and looked like missing frames.
    s.Width = d.Width; s.Height = d.Height; s.MipLevels = d.MipLevels; s.ArraySize = 1;
    s.Format = d.Format; s.SampleDesc.Count = 1;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    winrt::com_ptr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&s, nullptr, staging.put());
    if (FAILED(hr)) {
        std::ostringstream e; e << "[FrameBoostV2] dump: CreateTexture2D failed 0x"
          << std::hex << static_cast<unsigned>(hr) << " format=" << std::dec << d.Format;
        Logger::Log(e.str()); return;
    }
    context->CopyResource(staging.get(), tex);

    D3D11_MAPPED_SUBRESOURCE m{};
    hr = context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        std::ostringstream e; e << "[FrameBoostV2] dump: Map failed 0x" << std::hex << static_cast<unsigned>(hr);
        Logger::Log(e.str()); return;
    }
    const bool ok = FrameBoost::BmpWriter::SaveRgba8AsBmp(path, d.Width, d.Height,
                                          static_cast<const uint8_t*>(m.pData), m.RowPitch);
    context->Unmap(staging.get(), 0);
    if (!ok) Logger::Log("[FrameBoostV2] dump: SaveRgba8AsBmp returned false.");
}

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

    // ON by default, decided by measurement rather than by argument.
    //
    // Two runs of one build against Apex, 178 s without and 149 s with:
    //
    //                          without hold      with hold
    //   present gaps > 20 ms        261               10
    //   worst gap                 66.72 ms         36.95 ms
    //   seconds with a gap > 20 ms     39 %              5 %
    //   pipeline latency          10.48 ms         14.41 ms
    //
    // The 261 are almost all the source's own: 258 of them carry a source
    // interval that already explains the gap, most at exactly 27.78 ms, which
    // is two frames of a 72 fps source - Apex missing one. Without the hold
    // that lands on screen as a 27 ms freeze, because G sits right next to its
    // partner and leaves the whole interval empty. With the hold G sits IN the
    // gap, and the same stutter becomes two ordinary intervals.
    //
    // It is not free: 3.9 ms of input latency on average, 6.5 ms at worst, on
    // the real frame the mouse is attached to. Bought deliberately.
    //
    // `nohold` turns it off so the comparison stays one build, two runs.
    // LOW LATENCY MODE, and what it actually trades.
    //
    // Measured in CS2, 49 s, with the hold already off - total 10.79 ms:
    //
    //   capture (WGC delivery)      4.73 ms   not ours to cut
    //   motion estimation GPU       3.25 ms   peaks to 9.59
    //   interpolation GPU           0.88 ms
    //   loop and present            ~1.9 ms
    //
    // So the GPU is about a third of it, and the motion search is nearly all
    // of that. Generating at half resolution and dropping the expensive warp
    // filter cuts into exactly that part - worth a few milliseconds on
    // average and considerably more at the motion peaks, which is where a
    // stall is felt. It does NOT touch the capture cost, so this cannot
    // halve the number.
    //
    // The hold is the larger single lever at roughly half a source interval,
    // so low latency turns it off as well.
    const bool lowLatency = HasArg(args, L"lowlatency");
    const bool holdHalfInterval = !HasArg(args, L"nohold") && !lowLatency;

    // A red block on generated frames, green on real ones. Diagnostic only.
    const bool markFrames = HasArg(args, L"mark");

    // Overlay on the left half only, untouched game on the right.
    const bool halfWidth = HasArg(args, L"half");

    Presenter presenter;
    presenter.EnableHalfWidth(halfWidth);
    presenter.EnableVsync(HasArg(args, L"vsync"));
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
    if (lowLatency) {
        interpolator.SetInterpScale(2);   // generate at half resolution
        interpolator.SetWarpFilter(0);    // bilinear instead of Catmull-Rom
        Logger::Log("[FrameBoostV2] LOW LATENCY: hold off, interpolation at half "
                    "resolution, cheap warp filter. Generated frames are softer.");
    }

    // `showblend` paints every cross-faded pixel blue. The interpolation shader
    // falls back to a plain lerp of the two real frames wherever it does not
    // trust its motion vectors, and a blend carries no new moment in time - two
    // samples are two copies, not a midpoint. If the screen turns blue, the
    // doubling is nominal and the eye is right to see no difference.
    if (HasArg(args, L"showblend")) interpolator.SetDebugTint(5);

    // `dump` writes prev / generated / curr once, a few seconds in so the
    // pipeline is settled, then keeps running untouched.
    const bool dumpFrames = HasArg(args, L"dump");
    int dumpCountdown = dumpFrames ? 150 : -1;

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
    double   prevArrivalMs = 0.0;
    int      pairInvalidLogged = 0;
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
            pairInvalidLogged = 0;
            if (!syntheticMode) capture.ResetCounters();
                const std::string tr = capture.TakeTrace();
                if (!tr.empty()) Logger::Log("[FrameBoostV2][arrivals]\n" + tr);
            }
            continue;
        }

        // The first stamp of the chain: the frame is ours from here on, so
        // everything after this point is time WE spent.
        const double acquireMs = NowMs();
        double genStartMs = 0.0, genEndMs = 0.0;
        double holdRequestedMs = 0.0, holdWaitMs = 0.0;
        double presentStartMs = 0.0, presentReturnMs = 0.0;
        Presenter::PresentTiming pt{};
        int queueAtPresent = 0;
        const int queueDepthNow = syntheticMode ? 0 : capture.QueueDepth();

        telemetry.NoteSourceArrival();

        if (measureOnly) {
            // The arrival timeline and nothing else. Recorded through the same
            // sequence log so the content deltas can be read out exactly as
            // they are for a normal run.
            if (havePrev) {
                telemetry.NotePairIntervalMs(frame.arrivalMs - prevArrivalMs);
                telemetry.NoteSrtIntervalMs(frame.contentMs - prevContentMs);
                if (frame.contentMs - prevContentMs <= 0.0) telemetry.NoteContentDtZero();
            }
            telemetry.NoteSequence({ nextOutputId++, false, frame.frameId, frame.frameId,
                                     frame.contentMs, 0.0, frame.arrivalMs,
                                     frame.contentMs, frame.contentMs });
            capture.Release(frame);
            havePrev = true;
            prevId = frame.frameId;
            prevContentMs = frame.contentMs;
            prevArrivalMs = frame.arrivalMs;
            telemetry.NoteQueue(capture.QueueDepth(), static_cast<int>(capture.Overflows()));
            if (!syntheticMode)
                telemetry.NoteCapture(capture.Acquired(), capture.DupTimestamp(),
                                      capture.DupContent(),
                                      capture.FingerprintCount()
                                          ? capture.FingerprintMsSum() / capture.FingerprintCount()
                                          : 0.0);
            if (telemetry.ReportIfDue()) {
            pairInvalidLogged = 0;
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
        const double currArrivalMs = frame.arrivalMs;

        // THE CADENCE CLOCK IS QPC ARRIVAL. SystemRelativeTime is measured
        // beside it and decides nothing.
        //
        // SystemRelativeTime is the moment DWM COMPOSED the frame, not the
        // moment Apex rendered it. Two different game frames that land in one
        // compose window carry one stamp, which is why dt could be zero while
        // the fingerprint proved the pixels differed - a pair with real motion
        // in it, thrown away because the compositor's clock had not ticked.
        //
        // QPC arrival is the moment the frame reached THIS process. It is a
        // different thing from the render time and does not pretend otherwise:
        // it carries scheduling jitter that the render clock would not have.
        // What it does have is monotonicity and a tick per frame, which is what
        // a cadence needs and what the compose stamp could not supply.
        //
        // The frame keeps both stamps. srtIntervalMs is reported so the choice
        // stays checkable rather than becoming an assumption.
        const double pairIntervalMs = havePrev ? (currArrivalMs - prevArrivalMs) : 0.0;
        const double srtIntervalMs  = havePrev ? (currContentMs - prevContentMs) : 0.0;

        if (havePrev) {
            telemetry.NotePairIntervalMs(pairIntervalMs);
            telemetry.NoteSrtIntervalMs(srtIntervalMs);
            if (srtIntervalMs <= 0.0) telemetry.NoteContentDtZero();
        }

        // dt > 0 IS STILL THE WHOLE RULE, and it is still not a tolerance.
        //
        // QPC is monotonic, so on this clock dtQpc <= 0 should never happen. If
        // the log ever shows one, it is not repaired: no substituted stamp, no
        // forced 0.5, no minimum interval stood in for the real one. The pair
        // produces no generated frame, the real frame goes out on its own, and
        // PAIR_INVALID below prints every clock for both halves so the cause
        // can be found instead of guessed at.
        const bool dtValid = havePrev && pairIntervalMs > 0.0 && pairIntervalMs < 200.0;
        const bool pairUsable = dtValid && haveMotion;

        if (havePrev) {
            if (!dtValid) {
                telemetry.NotePairDtZero();
                // Capped per report window. QPC being monotonic, this should
                // print nothing at all; the cap is only so that a clock which
                // surprises us cannot turn the logger into the bottleneck and
                // destroy the very measurement it is reporting.
                if (pairInvalidLogged < 50) {
                    ++pairInvalidLogged;
                    std::ostringstream pi;
                    pi.setf(std::ios::fixed); pi.precision(4);
                    pi << "[FrameBoostV2] PAIR_INVALID"
                       << " A=" << prevId << " B=" << currId
                       << " | qpcA=" << prevArrivalMs << " qpcB=" << currArrivalMs
                       << " dtQpc=" << pairIntervalMs
                       << " | srtA=" << prevContentMs << " srtB=" << currContentMs
                       << " dtSrt=" << srtIntervalMs;
                    Logger::Log(pi.str());
                }
            }
            else if (!haveMotion)    telemetry.NotePairNoMotion();
            else                     telemetry.NoteValidPair();
        }

        if (pairUsable) {
            D3D11_TEXTURE2D_DESC desc{};
            estimator.CurrFrameTexture()->GetDesc(&desc);

            genStartMs = NowMs();
            if (interpolator.GenerateFrame(device.get(), context.get(),
                                           estimator.PrevFrameSRV(),
                                           estimator.CurrFrameSRV(),
                                           estimator.MotionVectorSRV(),
                                           desc.Width, desc.Height,
                                           DXGI_FORMAT_B8G8R8A8_UNORM, nullptr)) {
                genEndMs = NowMs();
                // The midpoint lives in the same clock the interval was
                // measured in. Mixing the two domains here would put the
                // generated frame's stamp on a timeline nothing else uses.
                const double genContentMs = prevArrivalMs + pairIntervalMs * 0.5;
                telemetry.NoteGeneratedProduced();

                if (dumpCountdown > 0 && --dumpCountdown == 0) {
                    // Forward slashes on purpose: Windows accepts them and they
                    // survive every layer of escaping between here and the disk.
                    const std::wstring dir =
                        L"C:/Users/Eto jA/FPS Booster/native/FrameBoostV2/tools/";
                    DumpTexture(device.get(), context.get(),
                                estimator.PrevFrameTexture(), dir + L"dump_1_prev.bmp");
                    DumpTexture(device.get(), context.get(),
                                interpolator.GeneratedFrameTexture(), dir + L"dump_2_generated.bmp");
                    DumpTexture(device.get(), context.get(),
                                estimator.CurrFrameTexture(), dir + L"dump_3_curr.bmp");
                    Logger::Log("[FrameBoostV2] Frame dump written to tools/.");
                }
                presentStartMs = NowMs();
                queueAtPresent = syntheticMode ? 0 : capture.QueueDepth();
                const bool okG = presenter.Present(context.get(), interpolator.GeneratedFrameTexture(), &pt,
                                                  markFrames ? Presenter::Marker::Generated
                                                             : Presenter::Marker::None);
                presentReturnMs = NowMs();
                if (okG) {
                    const double shownAt = presentReturnMs;
                    if (lastPresentMs > 0.0) telemetry.NotePresentInterval(shownAt - lastPresentMs);
                    lastPresentMs = shownAt;
                    telemetry.NoteGenerated(shownAt - frame.arrivalMs);
                    FrameRecord r{ nextOutputId++, true, prevId, currId,
                                   genContentMs, 0.5, shownAt,
                                   prevArrivalMs, currArrivalMs };
                    r.acquireMs = acquireMs;
                    r.genStartMs = genStartMs; r.genEndMs = genEndMs;
                    r.holdRequestedMs = holdRequestedMs; r.holdWaitMs = holdWaitMs;
                    r.presentStartMs = presentStartMs; r.presentReturnMs = presentReturnMs;
                    r.queueDepth = queueDepthNow;
                    r.copyMs = pt.copyMs; r.presentCallMs = pt.presentMs;
                    r.waitableFree = pt.waitableFree; r.waitableKnown = pt.waitableKnown;
                    r.submitted = pt.submitted; r.displayed = pt.displayed;
                    r.queueAtPresent = queueAtPresent;
                    r.statPresentCount = pt.statPresentCount;
                    r.statPresentRefresh = pt.statPresentRefresh;
                    r.statSyncRefresh = pt.statSyncRefresh;
                    r.statRefreshKnown = pt.statRefreshKnown;
                    telemetry.NoteSequence(r);
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

        // THE HALF-INTERVAL HOLD IS OFF BY DEFAULT, and this is the trade.
        //
        // It used to be unconditional: the real frame B was held back half a
        // source interval after its generated partner, so the two went out
        // evenly spaced. That spacing cost ~6.94 ms of input latency on the
        // REAL frame - the one the mouse is attached to - and it is the single
        // largest deliberate delay left in the pipeline.
        //
        // Without it, G and B are presented back to back. Spacing then comes
        // only from the source's own arrival cadence, and there is a real risk
        // to watch for: with tearing presents at sync interval 0, a G that is
        // overwritten microseconds later may never be scanned out at all. That
        // would be a counted frame nobody saw, which is exactly the thing this
        // engine does not do. "Present interval: min" is the number that says
        // whether it is happening - a min near zero means G is being buried.
        //
        // Both paths stay in one binary; `nohold` selects the other one.
        //
        // The measured answer to the buried-frame worry above: without the hold
        // the shortest gap averaged 1.53 ms and reached 0.28 ms, WITH it 0.88
        // and 0.25 - the hold does not fix that and slightly worsens it, since
        // it only moves the tight gap from G-then-B to B-then-G. It was kept
        // for what it does fix, which is the long gaps, not this.
        if (pairUsable && holdHalfInterval) {
            const double holdFrom = NowMs();
            holdRequestedMs = pairIntervalMs * 0.5;
            WaitUntil(holdFrom + holdRequestedMs, timer);
            holdWaitMs = NowMs() - holdFrom;
        }

        presentStartMs = NowMs();
        queueAtPresent = syntheticMode ? 0 : capture.QueueDepth();
        pt = Presenter::PresentTiming{};
        const bool okN = presenter.Present(context.get(), frame.texture, &pt,
                                          markFrames ? Presenter::Marker::Native
                                                     : Presenter::Marker::None);
        presentReturnMs = NowMs();
        if (okN) {
            const double shownAt = presentReturnMs;
            if (lastPresentMs > 0.0) telemetry.NotePresentInterval(shownAt - lastPresentMs);
            lastPresentMs = shownAt;
            const double rawOffset = frame.arrivalMs - frame.contentMs;
            if (rawOffset < epochOffsetMs) epochOffsetMs = rawOffset;
            const double captureLatencyMs = rawOffset - epochOffsetMs;
            telemetry.NoteNative(captureLatencyMs, shownAt - frame.arrivalMs);
            telemetry.NotePipelineLatencyMs(shownAt - frame.arrivalMs + captureLatencyMs);
            FrameRecord r{ nextOutputId++, false, currId, currId,
                           currArrivalMs, 0.0, shownAt,
                           currArrivalMs, currArrivalMs };
            r.acquireMs = acquireMs;
            r.genStartMs = genStartMs; r.genEndMs = genEndMs;
            r.holdRequestedMs = holdRequestedMs; r.holdWaitMs = holdWaitMs;
            r.presentStartMs = presentStartMs; r.presentReturnMs = presentReturnMs;
            r.queueDepth = queueDepthNow;
            r.copyMs = pt.copyMs; r.presentCallMs = pt.presentMs;
            r.waitableFree = pt.waitableFree; r.waitableKnown = pt.waitableKnown;
            r.submitted = pt.submitted; r.displayed = pt.displayed;
            r.queueAtPresent = queueAtPresent;
            r.statPresentCount = pt.statPresentCount;
            r.statPresentRefresh = pt.statPresentRefresh;
            r.statSyncRefresh = pt.statSyncRefresh;
            r.statRefreshKnown = pt.statRefreshKnown;
            telemetry.NoteSequence(r);
        } else {
            telemetry.NoteDropped();
        }

        if (!syntheticMode) capture.Release(frame);

        // ONLY PUBLISHED FRAMES ADVANCE THE CADENCE. A frame the ring never
        // handed over never reaches this line, so no QPC delta is ever taken
        // across a frame that was dropped or filtered - the interval is always
        // between two frames that both became source frames.
        havePrev = true;
        prevId = currId;
        prevContentMs = currContentMs;
        prevArrivalMs = currArrivalMs;

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
            pairInvalidLogged = 0;
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
