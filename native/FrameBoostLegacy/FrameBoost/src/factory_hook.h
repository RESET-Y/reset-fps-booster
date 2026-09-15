#pragma once
#include <dxgi.h>

namespace FrameBoost::FactoryHook {

// Hooks IDXGIFactory::CreateSwapChain on the given factory instance so we
// catch swapchain creation for games (Watch Dogs included) that create their
// device via D3D11CreateDevice and the swapchain separately via DXGI,
// instead of the combined D3D11CreateDeviceAndSwapChain call.
void InstallIfNeeded(IDXGIFactory* factory);

} // namespace FrameBoost::FactoryHook
