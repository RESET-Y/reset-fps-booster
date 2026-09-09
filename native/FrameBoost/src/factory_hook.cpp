#include "factory_hook.h"
#include "present_hook.h"
#include "logger.h"

#include <windows.h>
#include <atomic>

namespace FrameBoost::FactoryHook {

namespace {

using CreateSwapChainFn = HRESULT(__stdcall*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

std::atomic<bool> g_hooked{false};
CreateSwapChainFn g_originalCreateSwapChain = nullptr;

HRESULT __stdcall CreateSwapChainDetour(
    IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
    HRESULT hr = g_originalCreateSwapChain(factory, device, desc, swapChain);

    if (SUCCEEDED(hr) && swapChain && *swapChain) {
        Logger::Log("[FrameBoost] Real swapchain created via IDXGIFactory::CreateSwapChain - installing capture hook.");
        PresentHook::InstallIfNeeded(*swapChain);
    }

    return hr;
}

} // namespace

void InstallIfNeeded(IDXGIFactory* factory) {
    bool expected = false;
    if (!g_hooked.compare_exchange_strong(expected, true))
        return; // v0.1 only supports a single factory/swapchain

    // IDXGIFactory vtable layout: IUnknown(3) + IDXGIObject(4) +
    // EnumAdapters(1) + MakeWindowAssociation(1) + GetWindowAssociation(1) ->
    // CreateSwapChain is the next slot -> index 10.
    void** vtable = *reinterpret_cast<void***>(factory);
    constexpr size_t kCreateSwapChainIndex = 10;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[kCreateSwapChainIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Logger::Log("[FrameBoost] ERROR: could not unprotect factory vtable - CreateSwapChain hook NOT installed.");
        g_hooked = false;
        return;
    }

    g_originalCreateSwapChain = reinterpret_cast<CreateSwapChainFn>(vtable[kCreateSwapChainIndex]);
    vtable[kCreateSwapChainIndex] = reinterpret_cast<void*>(&CreateSwapChainDetour);

    VirtualProtect(&vtable[kCreateSwapChainIndex], sizeof(void*), oldProtect, &oldProtect);

    Logger::Log("[FrameBoost] IDXGIFactory::CreateSwapChain hook installed.");
}

} // namespace FrameBoost::FactoryHook
