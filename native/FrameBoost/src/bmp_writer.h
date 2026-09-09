#pragma once
#include <cstdint>
#include <string>

namespace FrameBoost::BmpWriter {

// Saves a top-down RGBA8 buffer as a standard 24bpp BMP (alpha dropped -
// this is a debug validation dump, not part of the real-time pipeline).
// rowPitchBytes is the stride of srcRgba as returned by Map(), which can be
// larger than width * 4 due to GPU row alignment.
bool SaveRgba8AsBmp(const std::wstring& path, uint32_t width, uint32_t height,
                     const uint8_t* srcRgba, uint32_t rowPitchBytes);

} // namespace FrameBoost::BmpWriter
