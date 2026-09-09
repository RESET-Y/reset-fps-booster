#pragma once
#include <dxgi.h>

namespace FrameBoost::PresentHook {

// Installs the IDXGISwapChain::Present hook on the given swapchain the first
// time a real one is created. Safe to call multiple times - only the first
// swapchain instance gets hooked in this v0.1 (Watch Dogs only creates one).
void InstallIfNeeded(IDXGISwapChain* swapChain);

} // namespace FrameBoost::PresentHook
