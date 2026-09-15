// RESET FRAMEBOOST - dxgi.dll proxy (Milestone 1, path B)
//
// Real-world testing against Watch Dogs (2014) showed the game creates its
// D3D11 device via D3D11CreateDevice and its swapchain SEPARATELY via
// IDXGIFactory::CreateSwapChain, rather than the combined
// D3D11CreateDeviceAndSwapChain that the d3d11.dll proxy hooks. This mirrors
// exactly why this game's existing ReShade install (found in Bin_mod/) also
// uses a dxgi.dll proxy rather than a d3d11.dll one. This file adds that
// second entry point using the same non-invasive proxy-DLL technique.
#include <windows.h>
#include <dxgi.h>
#include <string>

#include "logger.h"
#include "factory_hook.h"

namespace {

HMODULE g_realDXGI = nullptr;

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

CreateDXGIFactoryFn g_realCreateDXGIFactory = nullptr;
CreateDXGIFactory1Fn g_realCreateDXGIFactory1 = nullptr;
CreateDXGIFactory2Fn g_realCreateDXGIFactory2 = nullptr;

void LoadRealDXGI() {
    wchar_t systemDir[MAX_PATH]{};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring realPath = std::wstring(systemDir) + L"\\dxgi.dll";

    g_realDXGI = LoadLibraryExW(realPath.c_str(), nullptr, 0);
    if (!g_realDXGI) {
        FrameBoost::Logger::Log("[FrameBoost] FATAL: could not load the real system dxgi.dll - rendering will fail.");
        return;
    }

    g_realCreateDXGIFactory = reinterpret_cast<CreateDXGIFactoryFn>(GetProcAddress(g_realDXGI, "CreateDXGIFactory"));
    g_realCreateDXGIFactory1 = reinterpret_cast<CreateDXGIFactory1Fn>(GetProcAddress(g_realDXGI, "CreateDXGIFactory1"));
    g_realCreateDXGIFactory2 = reinterpret_cast<CreateDXGIFactory2Fn>(GetProcAddress(g_realDXGI, "CreateDXGIFactory2"));
}

void HookIfFactory(REFIID riid, void* obj) {
    if (!obj) return;
    // Every IID we care about (IDXGIFactory and its newer versions) derives
    // from IDXGIFactory, so a straight reinterpret is safe here: they all
    // keep CreateSwapChain at the same vtable slot (10) by COM inheritance
    // rules - a newer interface only ever appends methods.
    FrameBoost::FactoryHook::InstallIfNeeded(reinterpret_cast<IDXGIFactory*>(obj));
}

} // namespace

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    if (!g_realCreateDXGIFactory) {
        FrameBoost::Logger::Log("[FrameBoost] CreateDXGIFactory called but real dxgi.dll was not loaded - failing safe.");
        return E_FAIL;
    }
    HRESULT hr = g_realCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr)) HookIfFactory(riid, ppFactory ? *ppFactory : nullptr);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    if (!g_realCreateDXGIFactory1) {
        FrameBoost::Logger::Log("[FrameBoost] CreateDXGIFactory1 called but real dxgi.dll was not loaded - failing safe.");
        return E_FAIL;
    }
    HRESULT hr = g_realCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr)) HookIfFactory(riid, ppFactory ? *ppFactory : nullptr);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    if (!g_realCreateDXGIFactory2) {
        FrameBoost::Logger::Log("[FrameBoost] CreateDXGIFactory2 called but real dxgi.dll was not loaded - failing safe.");
        return E_FAIL;
    }
    HRESULT hr = g_realCreateDXGIFactory2(flags, riid, ppFactory);
    if (SUCCEEDED(hr)) HookIfFactory(riid, ppFactory ? *ppFactory : nullptr);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        FrameBoost::Logger::Init();
        FrameBoost::Logger::Log("[FrameBoost] Proxy dxgi.dll loaded into host process.");
        LoadRealDXGI();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_realDXGI) FreeLibrary(g_realDXGI);
    }
    return TRUE;
}

} // extern "C"
