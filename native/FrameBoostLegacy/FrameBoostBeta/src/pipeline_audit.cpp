#include "pipeline_audit.h"

#include "capture_engine.h"
#include "duplicate_detector.h"
#include "logger.h"

#include <dxgi1_2.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace FrameBoostBeta {

namespace {

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return 1000.0 * static_cast<double>(now.QuadPart) / freq.QuadPart;
}

// Interval statistics over one stage of the pipeline. Mean alone hides the
// pattern that matters: a source alternating 7 ms / 21 ms and a source running
// steadily at 14 ms have the same mean and look nothing alike on screen.
struct IntervalAccumulator {
    std::vector<double> samples;
    double lastMs = -1.0;

    void Mark(double timestampMs) {
        if (lastMs >= 0.0 && samples.size() < 65536) samples.push_back(timestampMs - lastMs);
        lastMs = timestampMs;
    }

    std::string Describe() const {
        if (samples.empty()) return "no samples";
        double sum = 0.0, lo = samples.front(), hi = samples.front();
        for (double v : samples) { sum += v; lo = (std::min)(lo, v); hi = (std::max)(hi, v); }
        const double mean = sum / samples.size();
        double variance = 0.0;
        for (double v : samples) variance += (v - mean) * (v - mean);
        const double sd = std::sqrt(variance / samples.size());

        std::ostringstream oss;
        oss.precision(2);
        oss << std::fixed << mean << " ms mean (" << (mean > 0.0 ? 1000.0 / mean : 0.0) << "/s), min "
            << lo << ", max " << hi << ", sd " << sd << " (" << samples.size() << " samples)";
        return oss.str();
    }

    void Clear() { samples.clear(); }
};

} // namespace

void RunCaptureAudit(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context, int seconds) {
    Logger::Log("[Audit] === Windows Graphics Capture: full pipeline audit ===");

    CaptureEngine capture;
    if (!capture.StartMonitor(monitor, device)) {
        Logger::Log("[Audit] FATAL: monitor capture failed to start.");
        return;
    }
    Logger::Log("[Audit] Frame pool buffers: " + std::to_string(capture.PoolBufferCount())
        + " | polling as fast as the loop allows, no generation, no presenting.");

    DuplicateDetector duplicateDetector;

    // Totals across the whole run, so the summary is not the last second only.
    uint64_t totalRetrieved = 0, totalUnique = 0, totalDuplicate = 0, totalStale = 0;
    IntervalAccumulator readIntervals;      // when WE read frames
    IntervalAccumulator captureIntervals;   // WGC's own capture timestamps
    IntervalAccumulator uniqueIntervals;    // spacing of frames whose content actually changed

    // Per-second counters.
    uint64_t producedAtSecondStart = capture.FramesProduced();
    uint64_t retrievedAtSecondStart = capture.FramesRetrieved();
    uint64_t uniqueThisSecond = 0, duplicateThisSecond = 0, staleThisSecond = 0;

    const double startMs = NowMs();
    double secondStartMs = startMs;
    int64_t lastCaptureTimestamp100ns = 0;

    while (NowMs() - startMs < seconds * 1000.0) {
        UINT width = 0, height = 0;
        int64_t timestamp100ns = 0;
        bool isNew = false;
        ID3D11Texture2D* tex = capture.PollLatestFrame(width, height, timestamp100ns, isNew);

        if (tex && isNew) {
            const double nowMs = NowMs();
            readIntervals.Mark(nowMs);

            if (lastCaptureTimestamp100ns != 0)
                captureIntervals.Mark(timestamp100ns / 10000.0);
            else
                captureIntervals.lastMs = timestamp100ns / 10000.0;
            lastCaptureTimestamp100ns = timestamp100ns;

            const int stale = capture.LastDiscardedStaleFrames();
            if (stale > 0) { totalStale += stale; staleThisSecond += stale; }

            if (duplicateDetector.IsDuplicate(device, context, tex)) {
                ++totalDuplicate; ++duplicateThisSecond;
            } else {
                ++totalUnique; ++uniqueThisSecond;
                uniqueIntervals.Mark(timestamp100ns / 10000.0);
            }
            ++totalRetrieved;
        }

        const double nowMs = NowMs();
        if (nowMs - secondStartMs >= 1000.0) {
            const uint64_t produced = capture.FramesProduced();
            const uint64_t retrieved = capture.FramesRetrieved();
            const uint64_t producedThisSecond = produced - producedAtSecondStart;
            const uint64_t retrievedThisSecond = retrieved - retrievedAtSecondStart;

            std::ostringstream oss;
            oss.precision(1);
            oss << std::fixed
                << "[Audit] " << (nowMs - startMs) / 1000.0 << "s"
                << " | WGC produced: " << producedThisSecond
                << " | retrieved: " << retrievedThisSecond
                << " | lost in pool: " << (producedThisSecond > retrievedThisSecond
                                            ? producedThisSecond - retrievedThisSecond : 0)
                << " | stale discarded: " << staleThisSecond
                << " | unique: " << uniqueThisSecond
                << " | duplicate: " << duplicateThisSecond;
            Logger::Log(oss.str());

            producedAtSecondStart = produced;
            retrievedAtSecondStart = retrieved;
            uniqueThisSecond = duplicateThisSecond = staleThisSecond = 0;
            secondStartMs = nowMs;
        }
    }

    const double elapsedSeconds = (NowMs() - startMs) / 1000.0;
    const uint64_t produced = capture.FramesProduced();
    const uint64_t retrieved = capture.FramesRetrieved();
    const auto producedStats = capture.ProducedIntervalStats();

    std::ostringstream summary;
    summary.precision(2);
    summary << std::fixed
        << "[Audit] --- WGC summary over " << elapsedSeconds << "s ---\n"
        << "[Audit]   produced   " << produced << " (" << produced / elapsedSeconds << "/s)\n"
        << "[Audit]   retrieved  " << retrieved << " (" << retrieved / elapsedSeconds << "/s)\n"
        << "[Audit]   lost in pool " << capture.FramesLostInPool()
        << " - frames WGC announced that were recycled before we read them\n"
        << "[Audit]   stale      " << totalStale << " - read, then dropped to reach a newer frame\n"
        << "[Audit]   unique     " << totalUnique << " (" << totalUnique / elapsedSeconds << "/s)\n"
        << "[Audit]   duplicate  " << totalDuplicate << " (" << totalDuplicate / elapsedSeconds << "/s)"
        << " - identical content, the compositor republishing an unchanged screen\n"
        << "[Audit]   FrameArrived spacing: " << producedStats.meanMs << " ms mean, min "
        << producedStats.minMs << ", max " << producedStats.maxMs << ", sd " << producedStats.stdDevMs
        << " (" << producedStats.samples << " samples)\n"
        << "[Audit]   WGC capture timestamps: " << captureIntervals.Describe() << "\n"
        << "[Audit]   our read times:         " << readIntervals.Describe() << "\n"
        << "[Audit]   unique-content spacing: " << uniqueIntervals.Describe();
    Logger::Log(summary.str());

    capture.Stop();
}

namespace {

// Finds the DXGI output that is this monitor, so every measurement here is
// about the same display the rest of the engine is working on.
winrt::com_ptr<IDXGIOutput1> FindOutputForMonitor(ID3D11Device* device, HMONITOR monitor) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) return nullptr;
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) return nullptr;

    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, output.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
            return output.try_as<IDXGIOutput1>();
    }
    return nullptr;
}

LRESULT CALLBACK ProbeWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        HBRUSH brush = CreateSolidBrush(RGB(255, 0, 255)); // magenta: appears in no real desktop
        FillRect(hdc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

namespace {

// Shows a magenta window over the whole monitor - with or without the
// exclusion flag - and reports how many of 64 sampled points come back
// magenta through Desktop Duplication. Returns -1 if no frame could be read.
//
// Run twice, because "we saw no magenta" on its own proves nothing: a window
// that never made it onto the screen produces exactly the same reading as a
// window that was successfully excluded. The unexcluded run is the control
// that tells those two apart.
int SampleProbeThroughDesktopDuplication(HMONITOR monitor, ID3D11Device* device,
                                         ID3D11DeviceContext* context, bool useExclusionFlag) {
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(monitor, &mi)) {
        Logger::Log("[Audit] Self-capture test: could not get the monitor rectangle.");
        return -1;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ProbeWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FrameBoostBetaCaptureProbe";
    RegisterClassExW(&wc);

    HWND probe = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"FrameBoost capture probe", WS_POPUP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!probe) {
        Logger::Log("[Audit] Self-capture test: could not create the probe window.");
        return -1;
    }

    SetLayeredWindowAttributes(probe, 0, 255, LWA_ALPHA);
    if (useExclusionFlag) {
        const BOOL ok = SetWindowDisplayAffinity(probe, WDA_EXCLUDEFROMCAPTURE);
        Logger::Log(ok
            ? "[Audit] Probe shown WITH WDA_EXCLUDEFROMCAPTURE."
            : "[Audit] WARNING: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) FAILED on the probe window.");
    } else {
        Logger::Log("[Audit] Control run: probe shown WITHOUT the exclusion flag - this one MUST be captured.");
    }

    ShowWindow(probe, SW_SHOWNOACTIVATE);
    UpdateWindow(probe);

    // Give the compositor time to actually put the window on screen, pumping
    // messages so it paints.
    const double showUntilMs = NowMs() + 700.0;
    while (NowMs() < showUntilMs) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }

    auto output1 = FindOutputForMonitor(device, monitor);
    if (!output1) {
        Logger::Log("[Audit] Self-capture test: no DXGI output matches that monitor.");
        DestroyWindow(probe);
        return -1;
    }

    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    HRESULT hr = output1->DuplicateOutput(device, duplication.put());
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[Audit] Self-capture test: DuplicateOutput failed (hr 0x" << std::hex << hr << ").";
        Logger::Log(oss.str());
        DestroyWindow(probe);
        return -1;
    }

    // Take a frame that actually carries new content, not the first one the
    // API happens to hand over.
    winrt::com_ptr<ID3D11Texture2D> captured;
    int result = -1;
    for (int attempt = 0; attempt < 200 && !captured; ++attempt) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        hr = duplication->AcquireNextFrame(50, &info, resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;

        if (info.LastPresentTime.QuadPart != 0) captured = resource.try_as<ID3D11Texture2D>();
        if (!captured) { duplication->ReleaseFrame(); continue; }

        // Copy out before releasing: the duplication surface is only ours
        // until ReleaseFrame.
        D3D11_TEXTURE2D_DESC desc{};
        captured->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        winrt::com_ptr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) {
            duplication->ReleaseFrame();
            break;
        }
        context->CopyResource(staging.get(), captured.get());
        duplication->ReleaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) break;

        // Sample a grid across the frame rather than one pixel: a single
        // sample could land on something magenta by accident, and a partly
        // excluded window would look like a clean pass.
        int magentaSamples = 0, totalSamples = 0;
        for (int y = 1; y <= 8; ++y) {
            for (int x = 1; x <= 8; ++x) {
                const UINT px = desc.Width * x / 9, py = desc.Height * y / 9;
                const auto* row = static_cast<const uint8_t*>(mapped.pData) + py * mapped.RowPitch;
                const uint8_t b = row[px * 4 + 0], g = row[px * 4 + 1], r = row[px * 4 + 2];
                if (r > 200 && g < 60 && b > 200) ++magentaSamples;
                ++totalSamples;
            }
        }
        context->Unmap(staging.get(), 0);

        std::ostringstream oss;
        oss << "[Audit] " << (useExclusionFlag ? "Excluded" : "Control") << " run: sampled "
            << totalSamples << " points, " << magentaSamples << " show the probe colour.";
        Logger::Log(oss.str());
        result = magentaSamples;
        break;
    }

    if (!captured) Logger::Log("[Audit] Self-capture test: no frame with new content arrived - inconclusive.");

    DestroyWindow(probe);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return result;
}

} // namespace

void RunSelfCaptureTest(HMONITOR monitor, ID3D11Device* device, ID3D11DeviceContext* context) {
    Logger::Log("[Audit] === Does WDA_EXCLUDEFROMCAPTURE hide a window from Desktop Duplication? ===");

    const int excludedRun = SampleProbeThroughDesktopDuplication(monitor, device, context, true);
    const int controlRun = SampleProbeThroughDesktopDuplication(monitor, device, context, false);

    if (excludedRun < 0 || controlRun < 0) {
        Logger::Log("[Audit] RESULT: inconclusive - one of the two runs could not read a frame.");
        return;
    }
    if (controlRun == 0) {
        // Without this check the test would happily "prove" exclusion works
        // when in fact the probe window never reached the screen.
        Logger::Log("[Audit] RESULT: INVALID - the control window was not captured either, so the probe"
                    " never made it onto the screen. The test proves nothing about the exclusion flag.");
        return;
    }
    Logger::Log(excludedRun == 0
        ? "[Audit] RESULT: WDA_EXCLUDEFROMCAPTURE IS honoured by Desktop Duplication - the control window"
          " was captured, the excluded one was not. Our overlay can stay out of the DD capture."
        : "[Audit] RESULT: Desktop Duplication CAPTURES the excluded window - the overlay would feed itself"
          " back into the engine, so the DD path needs another way to display the result.");
}

void RunDesktopDuplicationAudit(HMONITOR monitor, ID3D11Device* device, int seconds) {
    Logger::Log("[Audit] === DXGI Desktop Duplication: same question, other capture path ===");

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
        Logger::Log("[Audit] Desktop Duplication: could not get the DXGI device.");
        return;
    }
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
        Logger::Log("[Audit] Desktop Duplication: could not get the DXGI adapter.");
        return;
    }

    // Find the DXGI output that is the monitor we are auditing, so both paths
    // measure the same display.
    winrt::com_ptr<IDXGIOutput1> output1;
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, output.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
            output1 = output.try_as<IDXGIOutput1>();
            break;
        }
    }
    if (!output1) {
        Logger::Log("[Audit] Desktop Duplication: no DXGI output matches that monitor.");
        return;
    }

    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    HRESULT hr = output1->DuplicateOutput(device, duplication.put());
    if (FAILED(hr)) {
        // E_ACCESSDENIED is the usual answer while a game holds exclusive
        // fullscreen, and that is itself a measurement worth logging.
        std::ostringstream oss;
        oss << "[Audit] Desktop Duplication unavailable (hr 0x" << std::hex << hr << std::dec
            << "). E_ACCESSDENIED normally means something else holds the display"
               " (exclusive fullscreen, or another duplication client).";
        Logger::Log(oss.str());
        return;
    }

    IntervalAccumulator presentIntervals;   // LastPresentTime: when the desktop actually changed
    IntervalAccumulator acquireIntervals;   // when we got handed a frame
    uint64_t acquired = 0, withNewContent = 0, accumulatedTotal = 0, timeouts = 0;
    UINT accumulatedMax = 0;

    static LARGE_INTEGER qpcFreq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    const double startMs = NowMs();

    while (NowMs() - startMs < seconds * 1000.0) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        hr = duplication->AcquireNextFrame(16, &info, resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts; continue; }
        if (FAILED(hr)) {
            std::ostringstream oss;
            oss << "[Audit] Desktop Duplication AcquireNextFrame failed (hr 0x" << std::hex << hr << ").";
            Logger::Log(oss.str());
            break;
        }

        ++acquired;
        acquireIntervals.Mark(NowMs());

        // AccumulatedFrames is the number the whole argument turns on: how
        // many desktop updates were folded into this one because we were not
        // there to take them individually. Zero means we kept up.
        if (info.LastPresentTime.QuadPart != 0) {
            ++withNewContent;
            accumulatedTotal += info.AccumulatedFrames;
            accumulatedMax = (std::max)(accumulatedMax, info.AccumulatedFrames);
            presentIntervals.Mark(1000.0 * static_cast<double>(info.LastPresentTime.QuadPart) / qpcFreq.QuadPart);
        }

        duplication->ReleaseFrame();
    }

    const double elapsedSeconds = (NowMs() - startMs) / 1000.0;
    std::ostringstream summary;
    summary.precision(2);
    summary << std::fixed
        << "[Audit] --- Desktop Duplication summary over " << elapsedSeconds << "s ---\n"
        << "[Audit]   acquired            " << acquired << " (" << acquired / elapsedSeconds << "/s)\n"
        << "[Audit]   with new content    " << withNewContent << " (" << withNewContent / elapsedSeconds << "/s)"
        << " - the rest were mouse-only updates\n"
        << "[Audit]   timeouts (16 ms)    " << timeouts << " - nothing new was ready\n"
        << "[Audit]   coalesced updates   " << accumulatedTotal << " total, " << accumulatedMax
        << " max in one acquire - updates we missed by not asking sooner\n"
        << "[Audit]   desktop present spacing (LastPresentTime): " << presentIntervals.Describe() << "\n"
        << "[Audit]   our acquire spacing:                       " << acquireIntervals.Describe();
    Logger::Log(summary.str());
}

} // namespace FrameBoostBeta
