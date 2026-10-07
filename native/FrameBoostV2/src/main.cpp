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
#include "motion_blur.h"

#include <winrt/base.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
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

// Monitors in the order the app lists them: the primary first, then the rest
// in the order Windows enumerates them. The app does the same, so "screen 1"
// means the same monitor on both sides.
HMONITOR MonitorByIndex(int index) {
    std::vector<HMONITOR> all;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&all));
    std::stable_partition(all.begin(), all.end(), [](HMONITOR m) {
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        return GetMonitorInfoW(m, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
    });
    if (all.empty()) return MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    return all[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(all.size()) - 1))];
}

double MonitorRefreshHzOf(HMONITOR mon) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(mon, &info)) return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0.0;
    return static_cast<double>(mode.dmDisplayFrequency);
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

// One line, with its own timestamp, appended to Logs\tuning.log. Opened and
// closed per call: once a second is nothing, and nothing is left open.
void AppendTuningFile(const char* line) {
    wchar_t lad[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) return;
    const std::wstring path = std::wstring(lad) + L"\\ResetFpsBooster\\Logs\\tuning.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u  ",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    DWORD written = 0;
    WriteFile(f, stamp, static_cast<DWORD>(strlen(stamp)), &written, nullptr);
    WriteFile(f, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    CloseHandle(f);
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

    // `screen <n>`: THE WHOLE MONITOR, no window at all. Smooth Motion on
    // everything that is shown - game, video, desktop. The overlay covers the
    // monitor and is excluded from capture (see Presenter::SetScreenRect).
    const bool screenMode = HasArg(args, L"screen");
    HMONITOR screenMon = nullptr;
    RECT screenRect{};
    if (screenMode) {
        screenMon = MonitorByIndex(static_cast<int>(ArgValue(args, L"screen", 0.0)));
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(screenMon, &mi);
        screenRect = mi.rcMonitor;
        Logger::Log("[FrameBoostV2] SCREEN mode: whole monitor " + std::to_string(screenRect.right - screenRect.left)
                    + "x" + std::to_string(screenRect.bottom - screenRect.top) + " at "
                    + std::to_string(screenRect.left) + "," + std::to_string(screenRect.top));
    }

    const double handleArg = ArgValue(args, L"hwnd", 0.0);
    if (screenMode) {
        // no window to pick
    } else if (handleArg > 0.0) {
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

    if (!target && !screenMode) {
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

    // GPU PRIORITY. A game keeps the GPU busy, and our few milliseconds of
    // work queue behind it: motion estimation measured 2.5 ms normally and
    // 27 ms under an uncapped game. This raises the priority of THIS
    // process's GPU work only - the game is not touched. It is the documented
    // DXGI call; the log says whether Windows accepted it.
    //
    // First for Smooth Motion alone, then FrameBoost too (2026-09-30): its
    // baseline in Apex had motion estimation swinging 0.8-7.7 ms from one
    // second to the next - the same waiting behind the game, since the work
    // itself is under 1 ms. "nogpuprio" turns it off for A/B comparisons.
    if (!HasArg(args, L"nogpuprio")) {
        if (auto dxgiDevice = device.try_as<IDXGIDevice>()) {
            const HRESULT hr = dxgiDevice->SetGPUThreadPriority(7);
            Logger::Log(SUCCEEDED(hr) ? "[FrameBoostV2] GPU thread priority raised to 7."
                                      : "[FrameBoostV2] GPU thread priority could not be raised; continuing at normal priority.");
        }

        // Priority 7 was accepted and still not enough. Measured 2026-09-30
        // with a game loading the GPU: capture kept arriving at 140-160 a
        // second, but each of our presents waited 10-15 ms behind the game and
        // output fell to 63-90 fps. The thread priority only orders work
        // inside one scheduling class; this moves the whole process up a
        // class - the same call OBS makes so its capture does not stutter
        // under a full GPU. Our own process only; exported by gdi32, loaded
        // by name because the declaration lives in the driver kit.
        //
        // REALTIME NEEDS ADMIN RIGHTS, AND RFB DOES NOT HAVE THEM: the app is
        // asInvoker and the engine inherits that. The fallback used to be HIGH,
        // which measured worst of all (pipeline 31 ms, spikes to 89, against
        // 14.4 at NORMAL and 10.5 at REALTIME) - so every user without admin got
        // the slowest engine. Refused now means NORMAL, with the thread priority
        // put back to 0 as well: exactly the configuration measured at 14.4 ms.
        // HIGH happens only when gpuclass.txt asks for it.
        //
        // Which class is on trial. FrameBoost in Apex with REALTIME: pipeline
        // latency 14.4 -> 10.5 ms, but capture delivered 90.8 frames a second
        // against 98.7 while Apex itself rendered as many as before. Suspected:
        // REALTIME sits above the compositor that hands us those frames.
        // %LOCALAPPDATA%\ResetFpsBooster\gpuclass.txt picks "realtime", "high"
        // or "normal" for the A/B without a rebuild; missing means REALTIME.
        using SetClassFn = LONG(APIENTRY*)(HANDLE, int);
        constexpr int kClassNormal = 2, kClassHigh = 4, kClassRealtime = 5;
        int wanted = kClassRealtime;
        {
            wchar_t base[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
                std::ifstream f(std::wstring(base) + L"\\ResetFpsBooster\\gpuclass.txt");
                std::string word;
                if (f >> word) {
                    for (auto& ch : word) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                    if (word == "high") wanted = kClassHigh;
                    else if (word == "normal") wanted = kClassNormal;
                }
            }
        }
        if (auto gdi = GetModuleHandleW(L"gdi32.dll") ? GetModuleHandleW(L"gdi32.dll") : LoadLibraryW(L"gdi32.dll")) {
            if (auto setClass = reinterpret_cast<SetClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))) {
                bool atNormal = false;
                if (wanted == kClassNormal) {
                    Logger::Log("[FrameBoostV2] GPU scheduling class left at NORMAL (gpuclass.txt).");
                    atNormal = true;
                } else if (wanted == kClassRealtime) {
                    if (setClass(GetCurrentProcess(), kClassRealtime) == 0) {
                        Logger::Log("[FrameBoostV2] GPU scheduling class raised to REALTIME.");
                    } else {
                        Logger::Log("[FrameBoostV2] REALTIME refused (no admin rights); staying at NORMAL - HIGH measured worse than NORMAL.");
                        atNormal = true;
                    }
                } else if (setClass(GetCurrentProcess(), kClassHigh) == 0) {
                    Logger::Log("[FrameBoostV2] GPU scheduling class raised to HIGH (gpuclass.txt).");
                } else {
                    Logger::Log("[FrameBoostV2] GPU scheduling class could not be raised; staying at NORMAL.");
                    atNormal = true;
                }
                if (atNormal) {
                    if (auto dxgiDevice = device.try_as<IDXGIDevice>()) dxgiDevice->SetGPUThreadPriority(0);
                }
            }
        }
    }

    // WHAT WE ARE ACTUALLY POINTED AT, spelled out.
    //
    // Every measurement so far assumed the window named in the log is the
    // window being captured and that its present timeline is the game's. That
    // has not been checked once. Ruling it out costs six lines.
    if (!screenMode) {
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

    const double displayHz = screenMode ? MonitorRefreshHzOf(screenMon) : MonitorRefreshHz(target);
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
    } else if (screenMode) {
        if (!capture.StartMonitor(screenMon, device.get(), static_cast<int>(displayHz))) {
            Logger::Log("[FrameBoostV2] Screen capture could not start - exiting.");
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
    if (screenMode) client = { 0, 0, screenRect.right - screenRect.left, screenRect.bottom - screenRect.top };
    else GetClientRect(target, &client);
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

    // THE HOLD AS A FRACTION of the source interval (2026-10-06, latency plan
    // step 1). It was all or nothing: half an interval, or off with
    // "nohold"/"lowlatency". The question is how little of it the pacing
    // really needs, so it is now a number: "hold 0.25" on the command line,
    // 0 to 0.5. %LOCALAPPDATA%\ResetFpsBooster\hold.txt overrides it LIVE,
    // read twice a second, so one game session can walk through every value
    // without a restart. The [tuning] log line says what each one costs.
    double holdFraction = (HasArg(args, L"nohold") || lowLatency) ? 0.0
                        : std::clamp(ArgValue(args, L"hold", 0.5), 0.0, 0.5);
    std::wstring holdFile;
    {
        wchar_t lad[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH))
            holdFile = std::wstring(lad) + L"\\ResetFpsBooster\\hold.txt";
    }
    double holdFileCheckAt = 0.0;
    auto readHoldFile = [&]() {
        if (holdFile.empty() || NowMs() < holdFileCheckAt) return;
        holdFileCheckAt = NowMs() + 500.0;
        HANDLE f = CreateFileW(holdFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        char buf[32] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        try {
            const double v = std::clamp(std::stod(std::string(buf, got)), 0.0, 0.5);
            if (v != holdFraction) {
                holdFraction = v;
                Logger::Log("[FrameBoostV2][tuning] hold set to " + std::to_string(v) + " of the source interval (hold.txt).");
            }
        } catch (...) {}
    };

    // THE [tuning] LINE, once a second, beside the telemetry line the app
    // parses (that one must not change). Per hold setting it answers: what
    // does the real frame cost (pipeline mean, p95), and what does the
    // spacing look like - G to the real frame after it, the real frame to the
    // next G, how many gaps are under 2 ms (a frame tearing presents may bury)
    // and over 20 ms (a visible hitch), and how deep the queue got.
    struct TuningStats {
        std::vector<double> pipe, gToN, nToG;
        int gaps = 0, under2 = 0, over20 = 0, queueMax = 0;
        double holdWaitSum = 0.0; int holdCount = 0;
        double lastShown = -1.0; bool lastWasG = false; double reportAt = 0.0;
        void Present(bool isG, double at) {
            if (lastShown > 0.0) {
                const double d = at - lastShown;
                ++gaps; if (d < 2.0) ++under2; if (d > 20.0) ++over20;
                if (isG && !lastWasG) nToG.push_back(d);
                else if (!isG && lastWasG) gToN.push_back(d);
            }
            lastShown = at; lastWasG = isG;
        }
        static double Pct(std::vector<double>& v, double q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, static_cast<size_t>(q * v.size()))];
        }
        static double Mean(const std::vector<double>& v) {
            if (v.empty()) return 0.0;
            double t = 0.0; for (double x : v) t += x; return t / v.size();
        }
        void ReportIfDue(double holdFraction) {
            if (NowMs() < reportAt) return;
            const bool first = reportAt == 0.0;
            reportAt = NowMs() + 1000.0;
            if (first || gaps == 0) { Clear(); return; }
            char line[512];
            snprintf(line, sizeof(line),
                "[FrameBoostV2][tuning] hold %.2f | pipeline mean %.2f p95 %.2f ms | hold wait %.2f ms"
                " | G->N p5 %.2f p50 %.2f p95 %.2f | N->G p5 %.2f p50 %.2f p95 %.2f"
                " | gaps <2ms %.1f%% >20ms %d of %d | queue max %d",
                holdFraction, Mean(pipe), Pct(pipe, 0.95),
                holdCount ? holdWaitSum / holdCount : 0.0,
                Pct(gToN, 0.05), Pct(gToN, 0.5), Pct(gToN, 0.95),
                Pct(nToG, 0.05), Pct(nToG, 0.5), Pct(nToG, 0.95),
                100.0 * under2 / gaps, over20, gaps, queueMax);
            Logger::Log(line);
            // ALSO INTO ITS OWN FILE. The main log is capped at 4 MB with one
            // old generation, and at ~120 fps the per-frame [seq] lines fill
            // it in about a minute - a five-minute hold test lost its first
            // four phases that way (2026-10-06). This file only gets one short
            // line a second and is never rotated by the logger.
            AppendTuningFile(line);
            Clear();
        }
        void Clear() {
            pipe.clear(); gToN.clear(); nToG.clear();
            gaps = under2 = over20 = queueMax = 0; holdWaitSum = 0.0; holdCount = 0;
        }
    } tuning;

    // A red block on generated frames, green on real ones. Diagnostic only.
    const bool markFrames = HasArg(args, L"mark");

    // Overlay on the left half only, untouched game on the right.
    const bool halfWidth = HasArg(args, L"half");

    Presenter presenter;
    presenter.EnableHalfWidth(halfWidth);
    presenter.EnableShortQueue(HasArg(args, L"nogen"));
    presenter.EnableVsync(HasArg(args, L"vsync"));
    if (measureOnly) {
        Logger::Log("[FrameBoostV2] MEASURE ONLY - capturing and timestamping, presenting "
                    "nothing. Nothing will appear on screen; this measures what the game "
                    "delivers when we are not in its way.");
    } else if ((screenMode ? (presenter.SetScreenRect(screenRect), true) : true)
               && !presenter.Create(device.get(), initialW, initialH, target)) {
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

    // `blur [strength]` streaks every frame along its real motion - the look,
    // not frames. Strength is the shutter as a fraction of one source
    // interval, 0.5 by default. `blurdebug` paints the streak length instead:
    // green = short, red = long, untouched = still.
    //
    // SMOOTH MOTION (`smooth`) is the same pass with a FULL SHUTTER: every
    // shown frame is smeared over the whole time until the next one, so the
    // streak of one frame ends where the next begins and motion reads as
    // continuous. It used to shorten the shutter at higher frame rates "so the
    // picture stays sharp" and to leave slow movement untouched; both were
    // taken out on 2026-09-30 - Lukas: "es soll flüssig aussehen und nicht
    // Bilder scharf halten". The slider scales it (50 % = full shutter).
    // `hz <n>` is the refresh rate the user picked in the app; without it the
    // game window's monitor is read.
    MotionBlur motionBlur;
    const bool smoothAuto = HasArg(args, L"smooth");
    // `nogen`: SMOOTH MOTION ON ITS OWN. No generated frames at all - every
    // real frame is shown once, blurred, and the loop never holds anything
    // back, since with nothing to space out there is nothing to wait for.
    const bool generate = !HasArg(args, L"nogen");
    if (!generate) Logger::Log("[FrameBoostV2] Frame generation OFF (nogen): real frames only.");
    const bool blurOn = smoothAuto || HasArg(args, L"blur");

    // SMOOTH MOTION ESTIMATES AT HALF SIZE. FrameBoost needs every pixel of
    // precision - its vectors place the generated frame - but a streak only
    // needs the direction and length of the motion. Measured 2026-10-01 in
    // Apex: motion estimation was ~1.1 ms of every frame at up to 144 a
    // second, at REALTIME priority, taken straight from the game. At half
    // size it does a quarter of the work, and each 8-pixel block covers 16
    // screen pixels, which also gives a calmer field. The GPU makes the small
    // copy itself (one mip level); the blur still runs on the full frame.
    // "fullme" estimates at full size again, for comparisons.
    const bool halfEstimate = !generate && smoothAuto && !HasArg(args, L"fullme");
    winrt::com_ptr<ID3D11Texture2D> meMipTex, meHalfTex;
    winrt::com_ptr<ID3D11ShaderResourceView> meMipSRV;
    UINT meW = 0, meH = 0;
    auto feedEstimator = [&](ID3D11Texture2D* full) -> bool {
        if (!halfEstimate) return estimator.ProcessFrame(device.get(), context.get(), full);
        D3D11_TEXTURE2D_DESC fd{};
        full->GetDesc(&fd);
        if (!meMipTex || fd.Width != meW || fd.Height != meH) {
            meMipTex = nullptr; meHalfTex = nullptr; meMipSRV = nullptr;
            D3D11_TEXTURE2D_DESC md = fd;
            md.MipLevels = 2; md.ArraySize = 1; md.SampleDesc = { 1, 0 };
            md.Usage = D3D11_USAGE_DEFAULT; md.CPUAccessFlags = 0;
            md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            D3D11_TEXTURE2D_DESC hd = fd;
            hd.Width = std::max(1u, fd.Width / 2); hd.Height = std::max(1u, fd.Height / 2);
            hd.MipLevels = 1; hd.ArraySize = 1; hd.SampleDesc = { 1, 0 };
            hd.Usage = D3D11_USAGE_DEFAULT; hd.CPUAccessFlags = 0;
            hd.BindFlags = D3D11_BIND_SHADER_RESOURCE; hd.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&md, nullptr, meMipTex.put())) ||
                FAILED(device->CreateShaderResourceView(meMipTex.get(), nullptr, meMipSRV.put())) ||
                FAILED(device->CreateTexture2D(&hd, nullptr, meHalfTex.put()))) {
                Logger::Log("[FrameBoostV2] Half-size motion estimation unavailable; estimating at full size.");
                meMipTex = nullptr; meHalfTex = nullptr; meMipSRV = nullptr; meW = meH = 0;
                return estimator.ProcessFrame(device.get(), context.get(), full);
            }
            meW = fd.Width; meH = fd.Height;
            Logger::Log("[FrameBoostV2] Smooth motion estimates at half size: " + std::to_string(hd.Width) + "x" + std::to_string(hd.Height) + ".");
        }
        context->CopySubresourceRegion(meMipTex.get(), 0, 0, 0, 0, full, 0, nullptr);
        context->GenerateMips(meMipSRV.get());
        context->CopySubresourceRegion(meHalfTex.get(), 0, 0, 0, 0, meMipTex.get(), 1, nullptr);
        return estimator.ProcessFrame(device.get(), context.get(), meHalfTex.get());
    };
    const double outputHz = ArgValue(args, L"hz", displayHz > 0.0 ? displayHz : 60.0);
    if (blurOn) {
        if (!smoothAuto) motionBlur.SetStrength(static_cast<float>(ArgValue(args, L"blur", 0.5)));
        // No speed threshold and no still-pixel exception any more: they made
        // sharp islands next to blurred ones, which read as wrong. Everything
        // that moves is blurred by how much it moves; what does not move has
        // no streak because it has no motion.
        motionBlur.SetMinLength(0.5f);
        motionBlur.SetStillProtection(false);
        if (HasArg(args, L"blurdebug")) motionBlur.SetDebug(1);
        Logger::Log(std::string("[FrameBoostV2] ") + (smoothAuto ? "SMOOTH MOTION on (automatic strength)"
                                                                  : "MOTION BLUR on, fixed strength " + std::to_string(motionBlur.Strength()))
                    + ", display " + std::to_string(outputHz) + " Hz. A look, not frames: nothing blurred is counted as FPS.");
    }
    // Recomputed before every blurred present in automatic mode.
    // THE USER'S SLIDER, on top of the automatic strength: a multiplier, 1 =
    // as chosen automatically. Starts from `smooth <n>` and then follows the
    // small file the app writes whenever the slider moves, read twice a
    // second - so the slider works live, without restarting anything.
    double smoothUser = std::clamp(ArgValue(args, L"smooth", 3.0), 0.0, 6.0);
    double smoothFileCheckAt = 0.0;
    std::wstring smoothFile;
    {
        wchar_t lad[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH))
            smoothFile = std::wstring(lad) + L"\\ResetFpsBooster\\smooth_strength.txt";
    }
    auto readSmoothFile = [&]() {
        if (smoothFile.empty() || NowMs() < smoothFileCheckAt) return;
        smoothFileCheckAt = NowMs() + 500.0;
        HANDLE f = CreateFileW(smoothFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        char buf[32] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        try { smoothUser = std::clamp(std::stod(std::string(buf, got)), 0.0, 6.0); } catch (...) {}
    };
    auto updateSmoothMotion = [&](double sourceIntervalMs, bool generating) {
        if (!smoothAuto || sourceIntervalMs <= 0.0) return;
        readSmoothFile();
        const double sourceFps = 1000.0 / sourceIntervalMs;
        const double outputFps = std::min(sourceFps * (generating ? 2.0 : 1.0), outputHz);
        // A FIXED EXPOSURE, like the "240 FPS" videos (2026-10-01, Lukas:
        // "Effekt verstärken um wie auf Video auszusehen"). Those clips are
        // 60 fps with every frame exposed for 1/60 s, so a streak shows 16.7 ms
        // of motion. One frame of a 150 fps game is only 6.7 ms, which is why
        // a full one-frame shutter still looked weak. The streak now covers a
        // fixed time: the app sends 3 for the slider's middle -> 16.7 ms, the
        // video look at any frame rate; 6 -> 33 ms; 0.75 -> 4 ms. The vectors
        // span one source interval, so the shutter is exposure / interval.
        (void)outputFps;
        const double exposureMs = 1000.0 / 60.0 * (smoothUser / 3.0);
        motionBlur.SetStrength(static_cast<float>(exposureMs / sourceIntervalMs));
    };
    double blurReportAt = 0.0;

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

    // THE HOLD, from the mean of MEASURED arrival intervals.
    //
    // It used to be half of this pair's own arrival interval. That interval
    // is not the game's: Apex renders a steady 16.7 ms, but frames reach us on
    // the compositor's 144 Hz grid, so arrivals alternate between two and
    // three refreshes - 13.9 and 20.8 ms. Half of that swung the hold between
    // 5 and 12 ms, and a long hold running into a short interval put the next
    // generated frame 0.2 to 2 ms after the real one. With tearing presents
    // the first of two frames that close is never scanned out, so those pairs
    // showed as 60: "ich spuere ab und zu 120 fps". Measured over 20 s of
    // Apex: present interval min 0.2, p50 9.4, p95 17 ms.
    //
    // cadenceMs is an exponential mean of the real QPC arrival intervals - no
    // refresh rate, no fixed interval, no substituted timestamp, and the
    // interpolation phase stays 0.5 of the actual pair. Only the hold before
    // the REAL frame uses it. The generated frame still goes out the moment
    // it exists.
    //
    // THAT LAST PART IS NOT NEGOTIABLE, and the reason is measured. A first
    // version also held the generated frame back to keep it half a beat from
    // its predecessor. That made one loop turn a FULL beat plus the work -
    // slightly slower than the source - and the ring filled: queue 7, source
    // consumed at 20 instead of 60, latency 400 ms and climbing.
    double cadenceMs = 0.0;

    bool running = true;
    while (running) {
        presenter.PumpMessages();

        MSG peek;
        if (PeekMessageW(&peek, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE)) break;

        CapturedFrame frame{};
        // Smooth Motion on its own shows every frame once, so an older frame
        // waiting behind a newer one is only delay. Measured before this, with
        // an uncapped game holding the GPU: queue 7-8 deep, frames shown 65 to
        // 150 ms after they arrived, 255 ms at worst. Skip to the newest.
        if (!generate && !syntheticMode) capture.SkipToNewest();
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
        const bool haveMotion = feedEstimator(frame.texture);

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
            // Only plausible real intervals; a stall is not a beat.
            if (pairIntervalMs > 0.0 && pairIntervalMs < 100.0)
                cadenceMs = (cadenceMs <= 0.0) ? pairIntervalMs
                                               : cadenceMs + 0.1 * (pairIntervalMs - cadenceMs);
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

        if (pairUsable && generate) {
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
                ID3D11Texture2D* shownG = interpolator.GeneratedFrameTexture();
                if (blurOn) {
                    updateSmoothMotion(cadenceMs > 0.0 ? cadenceMs : pairIntervalMs, true);
                    motionBlur.SetMotionScale(1.0f);
                    motionBlur.SetPixelSelect(false);  // a generated frame sits between the real ones
                    motionBlur.SetUsePyramid(false);   // and is not the frame the mips were built from
                    if (ID3D11Texture2D* b = motionBlur.Apply(device.get(), context.get(), shownG,
                            estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                            desc.Width, desc.Height, Estimator::BlockSizePixels()))
                        shownG = b;
                }
                const bool okG = presenter.Present(context.get(), shownG, &pt,
                                                  markFrames ? Presenter::Marker::Generated
                                                             : Presenter::Marker::None);
                presentReturnMs = NowMs();
                if (okG) {
                    const double shownAt = presentReturnMs;
                    if (generate) tuning.Present(true, shownAt);
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

        // THE HOLD (half an interval by default, see holdFraction above), and
        // this is the trade.
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
        // NO HOLD WHILE A NEWER FRAME IS ALREADY WAITING. Holding then only
        // pushes everything behind it later; skipping it lets the loop catch up
        // instead of settling into a backlog it can never leave.
        // > 1, not > 0: QueueDepth counts the frame being worked on, which is
        // only released after its present. With > 0 this was always true and
        // the hold never ran - measured as a 6.5 ms pipeline, below the 8.3 ms
        // a hold alone would cost.
        const bool behind = !syntheticMode && capture.QueueDepth() > 1;
        if (generate) readHoldFile();
        if (pairUsable && generate && holdFraction > 0.0 && !behind) {
            const double holdFrom = NowMs();
            holdRequestedMs = (cadenceMs > 0.0 ? cadenceMs : pairIntervalMs) * holdFraction;
            WaitUntil(holdFrom + holdRequestedMs, timer);
            holdWaitMs = NowMs() - holdFrom;
            tuning.holdWaitSum += holdWaitMs; ++tuning.holdCount;
        }

        presentStartMs = NowMs();
        queueAtPresent = syntheticMode ? 0 : capture.QueueDepth();
        pt = Presenter::PresentTiming{};
        // The real frame gets the same streak when there is a fresh motion
        // field for it. Read from the estimator's copy: the capture texture
        // itself is not a shader input.
        ID3D11Texture2D* shownN = frame.texture;
        if (blurOn && haveMotion) {
            updateSmoothMotion(cadenceMs > 0.0 ? cadenceMs : pairIntervalMs, pairUsable && generate);
            // The full frame from the capture ring is what gets blurred and
            // shown; the estimator may only hold the half-size copy.
            D3D11_TEXTURE2D_DESC cd{};
            frame.texture->GetDesc(&cd);
            const UINT scale = (halfEstimate && meW) ? 2u : 1u;
            motionBlur.SetMotionScale(static_cast<float>(scale));
            // Per-pixel vector choice: OFF. Tried 2026-10-01 and judged clearly
            // worse ("ekelhaft" - "schlecht"). Kept behind "pixelselect" only to
            // compare against; the block field with confidence is the default.
            motionBlur.SetPixelSelect(HasArg(args, L"pixelselect"));
            // Reading long streaks from the mip chain: OFF. Tried 2026-10-01 and
            // rejected at once ("Nein zurück"). Behind "pyramid" for comparisons only.
            motionBlur.SetUsePyramid(HasArg(args, L"pyramid"));
            if (ID3D11Texture2D* b = motionBlur.Apply(device.get(), context.get(), frame.texture,
                    estimator.PrevFrameSRV(), estimator.CurrFrameSRV(), estimator.MotionVectorSRV(),
                    cd.Width, cd.Height, Estimator::BlockSizePixels() * scale))
                shownN = b;
            if (NowMs() > blurReportAt) {
                blurReportAt = NowMs() + 5000.0;
                Logger::Log("[FrameBoostV2] Smooth motion: strength " + std::to_string(motionBlur.Strength())
                            + ", GPU " + std::to_string(motionBlur.LastGpuTimeMs()) + " ms per frame"
                            + ", stale frames skipped so far " + std::to_string(syntheticMode ? 0 : capture.Skipped()));
            }
        }
        const bool okN = presenter.Present(context.get(), shownN, &pt,
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
            if (generate) {
                tuning.Present(false, shownAt);
                tuning.pipe.push_back(shownAt - frame.arrivalMs + captureLatencyMs);
                tuning.queueMax = std::max(tuning.queueMax, queueDepthNow);
                tuning.ReportIfDue(holdFraction);
            }
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
