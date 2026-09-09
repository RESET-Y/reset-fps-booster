// RESET FRAMEBOOST - native capture proxy (Milestone 1)
//
// This DLL is deployed as "d3d11.dll" next to Watch_Dogs.exe. Windows' normal
// DLL search order loads an app-local d3d11.dll before the one in
// System32, so the game loads us without any process injection, kernel
// component, or modification of the game's own files. We then load the
// REAL system d3d11.dll ourselves and forward every call the game makes,
// so the game renders completely normally - the only addition is the
// IDXGISwapChain::Present hook that gives FrameBoost access to real,
// already-rendered frames.
#include <windows.h>
#include <d3d11.h>
#include <string>

#include "logger.h"
#include "present_hook.h"

namespace {

HMODULE g_realD3D11 = nullptr;

using D3D11CreateDeviceFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

D3D11CreateDeviceFn g_realCreateDevice = nullptr;
D3D11CreateDeviceAndSwapChainFn g_realCreateDeviceAndSwapChain = nullptr;

void LoadRealD3D11() {
    wchar_t systemDir[MAX_PATH]{};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring realPath = std::wstring(systemDir) + L"\\d3d11.dll";

    // Load by full System32 path so we can never recursively load ourselves
    // even if an app-local d3d11.dll (us) is earlier in the search order.
    g_realD3D11 = LoadLibraryExW(realPath.c_str(), nullptr, 0);
    if (!g_realD3D11) {
        FrameBoost::Logger::Log("[FrameBoost] FATAL: could not load the real system d3d11.dll - rendering will fail.");
        return;
    }

    g_realCreateDevice = reinterpret_cast<D3D11CreateDeviceFn>(
        GetProcAddress(g_realD3D11, "D3D11CreateDevice"));
    g_realCreateDeviceAndSwapChain = reinterpret_cast<D3D11CreateDeviceAndSwapChainFn>(
        GetProcAddress(g_realD3D11, "D3D11CreateDeviceAndSwapChain"));

    if (!g_realCreateDevice || !g_realCreateDeviceAndSwapChain) {
        FrameBoost::Logger::Log("[FrameBoost] FATAL: real d3d11.dll is missing expected exports.");
    }
}

} // namespace

extern "C" {

HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
    ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
    if (!g_realCreateDevice) {
        FrameBoost::Logger::Log("[FrameBoost] D3D11CreateDevice called but real d3d11.dll was not loaded - failing safe.");
        return E_FAIL;
    }
    // No swapchain is created here, so nothing for FrameBoost to hook yet -
    // this path exists purely so the game keeps working normally if it uses it.
    return g_realCreateDevice(adapter, driverType, software, flags, featureLevels,
        numFeatureLevels, sdkVersion, device, featureLevel, immediateContext);
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
    if (!g_realCreateDeviceAndSwapChain) {
        FrameBoost::Logger::Log("[FrameBoost] D3D11CreateDeviceAndSwapChain called but real d3d11.dll was not loaded - failing safe.");
        return E_FAIL;
    }

    HRESULT hr = g_realCreateDeviceAndSwapChain(adapter, driverType, software, flags, featureLevels,
        numFeatureLevels, sdkVersion, swapChainDesc, swapChain, device, featureLevel, immediateContext);

    // Failsafe (Phase 10): if the real call failed, or gave us no swapchain,
    // FrameBoost simply never activates - the game already renders normally
    // through the pass-through above, nothing more to fall back from.
    if (SUCCEEDED(hr) && swapChain && *swapChain) {
        FrameBoost::Logger::Log("[FrameBoost] Real D3D11 swapchain created - installing capture hook.");
        FrameBoost::PresentHook::InstallIfNeeded(*swapChain);
    }

    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        FrameBoost::Logger::Init();
        FrameBoost::Logger::Log("[FrameBoost] Proxy d3d11.dll loaded into host process.");
        LoadRealD3D11();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_realD3D11) FreeLibrary(g_realD3D11);
    }
    return TRUE;
}

} // extern "C"
