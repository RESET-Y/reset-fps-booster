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
#include "logger.h"

#include "motion_estimation.h"
#include "interpolation.h"

#include <winrt/base.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    timeBeginPeriod(1);

    std::vector<std::wstring> args;
    {
        int count = 0;
        if (LPWSTR* raw = CommandLineToArgvW(lpCmdLine, &count)) {
            for (int i = 0; i < count; ++i) args.emplace_back(raw[i]);
            LocalFree(raw);
        }
    }

    Logger::Init();
    Logger::Log("[FrameBoostV2] ==== RESET FRAMEBOOST V2 ====");
    Logger::Log("[FrameBoostV2] One source pair produces exactly one generated frame, "
                "at phase 0.5 of that pair's own timestamps. Output is twice the source "
                "rate whatever the monitor runs at.");

    // The window to follow: whatever is in front when we start, which is the
    // game, because the app launches this from the game.
    HWND target = GetForegroundWindow();
    if (!target || !IsWindow(target)) {
        Logger::Log("[FrameBoostV2] No foreground window to capture - exiting.");
        return 1;
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
    } else if (!capture.StartWindow(target, device.get())) {
        Logger::Log("[FrameBoostV2] Capture could not start - exiting.");
        return 1;
    }

    RECT client{};
    GetClientRect(target, &client);
    const UINT initialW = std::max<UINT>(1, static_cast<UINT>(client.right - client.left));
    const UINT initialH = std::max<UINT>(1, static_cast<UINT>(client.bottom - client.top));

    Presenter presenter;
    if (!presenter.Create(device.get(), initialW, initialH, target)) {
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
            telemetry.ReportIfDue();
            continue;
        }

        telemetry.NoteSourceArrival();
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
                if (presenter.Present(context.get(), interpolator.GeneratedFrameTexture())) {
                    const double shownAt = NowMs();
                    telemetry.NoteGenerated(shownAt - frame.arrivalMs);
                    telemetry.NoteSequence({ nextOutputId++, true, prevId, currId,
                                             genContentMs, 0.5, shownAt });
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
            const double rawOffset = frame.arrivalMs - frame.contentMs;
            if (rawOffset < epochOffsetMs) epochOffsetMs = rawOffset;
            const double captureLatencyMs = rawOffset - epochOffsetMs;
            telemetry.NoteNative(captureLatencyMs, shownAt - frame.arrivalMs);
            telemetry.NotePipelineLatencyMs(shownAt - frame.arrivalMs + captureLatencyMs);
            telemetry.NoteSequence({ nextOutputId++, false, 0, 0,
                                     currContentMs, 0.0, shownAt });
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
        if (telemetry.ReportIfDue()) presenter.ResetStats();
    }

    Logger::Log("[FrameBoostV2] Shutting down.");
    if (!syntheticMode) capture.Stop();
    presenter.Destroy();
    if (timer) CloseHandle(timer);
    timeEndPeriod(1);
    return 0;
}
