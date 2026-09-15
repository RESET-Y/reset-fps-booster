// Asks which window to boost, and returns it. Null means the user cancelled.
//
// Skipped entirely when RFB passes "hwnd <value>" - see picker.cpp for why
// that path exists before the app can use it.
#pragma once
#include <windows.h>

namespace fbv2 {
HWND PickWindow();
}
