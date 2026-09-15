#include "present_hook.h"
#include "logger.h"
#include "bmp_writer.h"
#include "motion_estimation.h"
#include "interpolation.h"

#include <d3d11.h>
#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>

namespace FrameBoost::PresentHook {

namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);

std::atomic<bool> g_hooked{false};
PresentFn g_originalPresent = nullptr;
std::atomic<uint64_t> g_frameCount{0};
LARGE_INTEGER g_qpcFrequency{};
LARGE_INTEGER g_lastReportTime{};
uint64_t g_framesSinceReport = 0;

// Milestone 3: real GPU block-matching motion estimation between
// consecutive real frames. Single global instance - v0.1 supports one
// swapchain/device, matching every other piece of state in this file.
MotionEstimation::Estimator g_motionEstimator;

// Milestone 4: real GPU motion-compensated interpolation from the estimator's
// output. Not yet inserted into the actual presentation (that is Milestone
// 5) - v0.1 generates it and validates it via debug dump only, for now.
Interpolation::Interpolator g_interpolator;

// Milestone 2/3 debug capture: dump real captured frames + their motion
// field to disk as .bmp so we can visually confirm they are genuine and
// meaningful. This uses a blocking CPU readback (Map), which is fine for an
// occasional debug dump but is NOT how the real-time pipeline will work
// later (Phase 6 requires staying GPU-side; this validation path is
// explicitly separate).
//
// Armed by pressing F9 in-game, rather than starting automatically on
// swapchain creation - loading screens/menus can take minutes, and an
// automatic timer would burn the whole capture budget before real gameplay
// motion ever happens. Press F9 once you are actually moving.
constexpr int kMaxDebugDumps = 40; // ~2min of coverage at one dump/3s once armed
std::atomic<int> g_debugDumpsDone{0};
LARGE_INTEGER g_lastDumpTime{};
std::atomic<bool> g_dumpArmed{false};
std::atomic<bool> g_f9WasDown{false};

// Milestone 5 / Phase 8: FrameBoost ON/OFF toggle, F10 in-game for this
// native prototype (the real app UI toggle comes later, Milestone 8).
// Defaults ON so the first live test actually exercises the insertion path.
std::atomic<bool> g_frameBoostEnabled{true};
std::atomic<bool> g_f10WasDown{false};
std::atomic<uint64_t> g_generatedFramesPresented{0};

void PollFrameBoostToggleHotkey() {
    bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (isDown && !g_f10WasDown.load()) {
        bool newState = !g_frameBoostEnabled.load();
        g_frameBoostEnabled = newState;
        Logger::Log(newState
            ? "[FrameBoost] F10 pressed - FrameBoost ENABLED, inserting generated frames."
            : "[FrameBoost] F10 pressed - FrameBoost DISABLED, normal rendering only.");
    }
    g_f10WasDown = isDown;
}

void PollDumpHotkey() {
    bool isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (isDown && !g_f9WasDown.load()) {
        g_debugDumpsDone = 0;
        g_lastDumpTime.QuadPart = 0;
        g_dumpArmed = true;
        Logger::Log("[FrameBoost] F9 pressed - debug capture armed, dumping the next frames.");
    }
    g_f9WasDown = isDown;
}

std::wstring GetCaptureDumpDir() {
    PWSTR localAppData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        dir = std::wstring(localAppData) + L"\\ResetFpsBooster\\Logs\\FrameBoostCaptures";
        CoTaskMemFree(localAppData);
        CreateDirectoryW(dir.c_str(), nullptr); // parent (Logs) already exists from Logger::Init
    }
    return dir;
}

// Milestone 3 validation: color-codes the real motion vector field (from
// MotionEstimation::Estimator) into a viewable image. Blue-ish/neutral where
// motion is near zero (static scene), colored where blocks actually moved -
// direction encoded in hue-ish R/G channels. This is a debug-only CPU
// readback, same throttling as the frame dump above, never part of the
// real-time path.
void DumpMotionFieldToDisk(ID3D11Device* device, ID3D11DeviceContext* context, int dumpIndex) {
    ID3D11Texture2D* mvTex = g_motionEstimator.MotionVectorTexture();
    if (!mvTex) return;

    UINT bx = g_motionEstimator.BlockCountX();
    UINT by = g_motionEstimator.BlockCountY();
    if (bx == 0 || by == 0) return;

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = bx;
    stagingDesc.Height = by;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return;

    context->CopyResource(staging, mvTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        std::vector<uint8_t> rgba(static_cast<size_t>(bx) * by * 4);
        double sumMagnitude = 0.0;

        for (UINT y = 0; y < by; ++y) {
            const float* row = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < bx; ++x) {
                float vx = row[x * 2 + 0];
                float vy = row[x * 2 + 1];
                sumMagnitude += std::sqrt(vx * vx + vy * vy);

                size_t idx = (static_cast<size_t>(y) * bx + x) * 4;
                rgba[idx + 0] = static_cast<uint8_t>(std::clamp(128.0f + vx * 12.0f, 0.0f, 255.0f)); // R <- x motion
                rgba[idx + 1] = static_cast<uint8_t>(std::clamp(128.0f + vy * 12.0f, 0.0f, 255.0f)); // G <- y motion
                rgba[idx + 2] = 96;
                rgba[idx + 3] = 255;
            }
        }
        context->Unmap(staging, 0);

        std::wstring dir = GetCaptureDumpDir();
        if (!dir.empty()) {
            wchar_t filename[64];
            swprintf_s(filename, L"\\motion_%03d.bmp", dumpIndex);
            BmpWriter::SaveRgba8AsBmp(dir + filename, bx, by, rgba.data(), bx * 4);

            std::ostringstream oss;
            oss << "[FrameBoost] Milestone 3: saved motion vector field ("
                << bx << "x" << by << " blocks, avg magnitude "
                << (sumMagnitude / (bx * by)) << " px) for visual validation.";
            Logger::Log(oss.str());
        }
    }

    staging->Release();
}

// Milestone 4 validation: dump the real generated (interpolated) frame -
// this is what actually gets checked against Frame A / Frame B to see
// whether the motion-compensated interpolation looks like a plausible
// in-between frame, or shows artifacts (ghosting, tearing at motion
// boundaries, etc).
void DumpGeneratedFrameToDisk(ID3D11Device* device, ID3D11DeviceContext* context, int dumpIndex) {
    ID3D11Texture2D* generated = g_interpolator.GeneratedFrameTexture();
    if (!generated) return;

    D3D11_TEXTURE2D_DESC desc{};
    generated->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return;

    context->CopyResource(staging, generated);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        std::wstring dir = GetCaptureDumpDir();
        if (!dir.empty()) {
            wchar_t filename[64];
            swprintf_s(filename, L"\\generated_%03d.bmp", dumpIndex);
            if (BmpWriter::SaveRgba8AsBmp(dir + filename, desc.Width, desc.Height,
                    reinterpret_cast<const uint8_t*>(mapped.pData), mapped.RowPitch)) {
                Logger::Log("[FrameBoost] Milestone 4: saved real generated (interpolated) frame for visual validation.");
            }
        }
        context->Unmap(staging, 0);
    }

    staging->Release();
}

void TryDumpFrameToDisk(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!g_dumpArmed.load()) return;

    int done = g_debugDumpsDone.load();
    if (done >= kMaxDebugDumps) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastDumpTime.QuadPart != 0) {
        double elapsed = static_cast<double>(now.QuadPart - g_lastDumpTime.QuadPart) / g_qpcFrequency.QuadPart;
        if (elapsed < 3.0) return; // one dump every 3 seconds at most
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
        return;

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        context->CopyResource(staging, backBuffer);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::wstring dir = GetCaptureDumpDir();
            if (!dir.empty()) {
                wchar_t filename[64];
                swprintf_s(filename, L"\\frame_%03d.bmp", done + 1);
                std::wstring path = dir + filename;

                if (BmpWriter::SaveRgba8AsBmp(path, desc.Width, desc.Height,
                        reinterpret_cast<const uint8_t*>(mapped.pData), mapped.RowPitch)) {
                    g_debugDumpsDone = done + 1;
                    g_lastDumpTime = now;
                    Logger::Log("[FrameBoost] Milestone 2: saved real captured frame to disk for visual validation.");
                    DumpMotionFieldToDisk(device, context, done + 1);
                    DumpGeneratedFrameToDisk(device, context, done + 1);
                }
            }
            context->Unmap(staging, 0);
        }
        staging->Release();
    }

    backBuffer->Release();
}

// Milestone 1 proof of capture: on a real Present call, grab a reference to
// the actual back buffer texture and log its real dimensions/format. This is
// the "Frame A"/"Frame B" source the rest of the pipeline will consume in
// later milestones - no synthetic data anywhere in this path.
void LogCaptureProof(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
        return;

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    backBuffer->Release();

    std::ostringstream oss;
    oss << "[FrameBoost] Milestone 1 capture proof: back buffer "
        << desc.Width << "x" << desc.Height << " format=" << desc.Format;
    Logger::Log(oss.str());
}

// Diagnostic for the stutter/no-perceived-smoothness report: windowed-mode
// swapchains are composited by the Desktop Window Manager, which decides
// independently which completed frame to actually scan out - two Present
// calls issued back-to-back with no timing control can both complete before
// DWM's own next composition tick, in which case DWM simply shows the LATER
// one and the generated frame was never actually seen. This log confirms
// whether that is even possible for this swapchain.
void LogPresentationDiagnostics(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return;

    BOOL isFullscreen = FALSE;
    swapChain->GetFullscreenState(&isFullscreen, nullptr);

    std::ostringstream oss;
    oss << "[FrameBoost] Presentation diagnostics: Windowed=" << (desc.Windowed ? "true" : "false")
        << " ExclusiveFullscreen=" << (isFullscreen ? "true" : "false")
        << " SwapEffect=" << desc.SwapEffect
        << " BufferCount=" << desc.BufferCount;
    Logger::Log(oss.str());
}

HRESULT __stdcall PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    uint64_t frame = ++g_frameCount;
    ++g_framesSinceReport;

    // Prove real capture access exactly once, right after the hook takes hold.
    if (frame == 1) {
        Logger::Log("[FrameBoost] Present hook active - first real frame observed.");
        LogCaptureProof(swapChain);
        LogPresentationDiagnostics(swapChain);
    }

    PollDumpHotkey();
    PollFrameBoostToggleHotkey();

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;
    bool haveMotionField = false;

    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        device->GetImmediateContext(&context);
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
            // Milestone 3: real motion estimation between this frame and the
            // previous one, entirely GPU-side.
            haveMotionField = g_motionEstimator.ProcessFrame(device, context, backBuffer);

            // Milestone 4: real motion-compensated interpolation, once a
            // motion field actually exists (never on the very first frame).
            if (haveMotionField) {
                D3D11_TEXTURE2D_DESC bbDesc{};
                backBuffer->GetDesc(&bbDesc);
                haveMotionField = g_interpolator.GenerateFrame(device, context,
                    g_motionEstimator.PrevFrameSRV(), g_motionEstimator.CurrFrameSRV(),
                    g_motionEstimator.MotionVectorSRV(), bbDesc.Width, bbDesc.Height, bbDesc.Format);
            }
        }
    }

    // Real, measured native FPS - never fabricated. Reported once per second.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastReportTime.QuadPart == 0) g_lastReportTime = now;

    double elapsedSeconds = static_cast<double>(now.QuadPart - g_lastReportTime.QuadPart) / g_qpcFrequency.QuadPart;
    if (elapsedSeconds >= 1.0) {
        double realFps = g_framesSinceReport / elapsedSeconds;
        double outputFps = realFps + (g_generatedFramesPresented.exchange(0) / elapsedSeconds);
        std::ostringstream oss;
        oss << "[FrameBoost] Native FPS (real, measured): " << realFps
            << " | Output FPS (real, measured): " << outputFps
            << " | FrameBoost: " << (g_frameBoostEnabled.load() ? "ON" : "OFF");
        double gpuMs = g_motionEstimator.LastGpuTimeMs();
        if (gpuMs >= 0.0) oss << " | Motion estimation GPU cost: " << gpuMs << " ms";
        else oss << " | Motion estimation GPU cost: N/A";
        double interpMs = g_interpolator.LastGpuTimeMs();
        if (interpMs >= 0.0) oss << " | Interpolation GPU cost: " << interpMs << " ms";
        else oss << " | Interpolation GPU cost: N/A";
        Logger::Log(oss.str());
        g_framesSinceReport = 0;
        g_lastReportTime = now;
    }

    if (device && context) {
        TryDumpFrameToDisk(swapChain, device, context);
    }

    // Milestone 5: actually insert the generated frame into presentation.
    // Real Present() is called TWICE for one real rendered frame - once for
    // the generated (interpolated) frame, once for the real one - which is
    // exactly what produces genuine 2x OUTPUT frames on screen, not just a
    // number we report. Any failure anywhere in this path falls back to a
    // single normal Present with the real frame (Phase 10 failsafe) - never
    // leaves the screen stuck on the generated frame or a black frame.
    HRESULT hr;
    bool insertedGeneratedFrame = false;

    if (g_frameBoostEnabled.load() && haveMotionField && device && context && backBuffer) {
        ID3D11Texture2D* generatedTex = g_interpolator.GeneratedFrameTexture();
        ID3D11Texture2D* realCurrTex = g_motionEstimator.CurrFrameTexture();

        if (generatedTex && realCurrTex) {
            context->CopyResource(backBuffer, generatedTex);
            // Both presents now wait for their own vsync tick (at least 1),
            // instead of the generated frame using syncInterval 0. That
            // earlier choice presented the generated frame near-instantly
            // and then made the real frame wait for the game's full
            // interval, producing uneven frame timing - exactly the
            // "ruckeln" (stutter) reported on the first live test, even
            // though the raw Present count genuinely did double. Waiting a
            // full vsync for each gives the two frames roughly equal,
            // evenly-spaced screen time instead.
            UINT generatedSyncInterval = syncInterval == 0 ? 1 : syncInterval;
            HRESULT hrGenerated = g_originalPresent(swapChain, generatedSyncInterval, flags);

            if (SUCCEEDED(hrGenerated)) {
                ++g_generatedFramesPresented;

                // The back buffer index rotates after Present (flip model) -
                // must re-fetch the CURRENT one before writing the real frame.
                ID3D11Texture2D* newBackBuffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&newBackBuffer)))) {
                    context->CopyResource(newBackBuffer, realCurrTex);
                    hr = g_originalPresent(swapChain, syncInterval, flags);
                    newBackBuffer->Release();
                } else {
                    Logger::Log("[FrameBoost] Failsafe: could not re-fetch back buffer after generated present.");
                    hr = hrGenerated;
                }
            } else {
                Logger::Log("[FrameBoost] Failsafe: presenting the generated frame failed - restoring real frame and presenting normally.");
                context->CopyResource(backBuffer, realCurrTex);
                hr = g_originalPresent(swapChain, syncInterval, flags);
            }
            insertedGeneratedFrame = true;
        }
    }

    if (!insertedGeneratedFrame) {
        hr = g_originalPresent(swapChain, syncInterval, flags);
    }

    if (backBuffer) backBuffer->Release();
    if (context) context->Release();
    if (device) device->Release();

    return hr;
}

} // namespace

void InstallIfNeeded(IDXGISwapChain* swapChain) {
    bool expected = false;
    if (!g_hooked.compare_exchange_strong(expected, true))
        return; // already hooked - v0.1 only supports a single swapchain

    QueryPerformanceFrequency(&g_qpcFrequency);

    // IDXGISwapChain vtable layout: IUnknown(3) + IDXGIObject(4) +
    // IDXGIDeviceSubObject(1) + Present is the next slot -> index 8.
    void** vtable = *reinterpret_cast<void***>(swapChain);
    constexpr size_t kPresentIndex = 8;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[kPresentIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Logger::Log("[FrameBoost] ERROR: could not unprotect swapchain vtable - Present hook NOT installed.");
        g_hooked = false;
        return;
    }

    g_originalPresent = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);
    vtable[kPresentIndex] = reinterpret_cast<void*>(&PresentDetour);

    VirtualProtect(&vtable[kPresentIndex], sizeof(void*), oldProtect, &oldProtect);

    Logger::Log("[FrameBoost] IDXGISwapChain::Present hook installed.");
}

} // namespace FrameBoost::PresentHook
