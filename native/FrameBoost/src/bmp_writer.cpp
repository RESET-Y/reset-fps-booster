#include "bmp_writer.h"

#include <windows.h>
#include <fstream>
#include <vector>

namespace FrameBoost::BmpWriter {

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type = 0x4D42; // 'BM'
    uint32_t fileSize = 0;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t pixelDataOffset = 0;
};

struct BmpInfoHeader {
    uint32_t headerSize = 40;
    int32_t width = 0;
    int32_t height = 0;
    uint16_t planes = 1;
    uint16_t bitsPerPixel = 24;
    uint32_t compression = 0;
    uint32_t imageSize = 0;
    int32_t xPixelsPerMeter = 0;
    int32_t yPixelsPerMeter = 0;
    uint32_t colorsUsed = 0;
    uint32_t colorsImportant = 0;
};
#pragma pack(pop)

bool SaveRgba8AsBmp(const std::wstring& path, uint32_t width, uint32_t height,
                     const uint8_t* srcRgba, uint32_t rowPitchBytes) {
    if (!srcRgba || width == 0 || height == 0) return false;

    // BMP rows are padded to 4 bytes and stored bottom-up.
    const uint32_t dstRowBytes = ((width * 3 + 3) / 4) * 4;
    std::vector<uint8_t> pixelData(static_cast<size_t>(dstRowBytes) * height, 0);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcRgba + static_cast<size_t>(y) * rowPitchBytes;
        uint8_t* dstRow = pixelData.data() + static_cast<size_t>(height - 1 - y) * dstRowBytes;
        for (uint32_t x = 0; x < width; ++x) {
            // Source is RGBA8 -> BMP wants BGR.
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // B
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // R
        }
    }

    BmpFileHeader fileHeader;
    BmpInfoHeader infoHeader;
    infoHeader.width = static_cast<int32_t>(width);
    infoHeader.height = static_cast<int32_t>(height);
    infoHeader.imageSize = static_cast<uint32_t>(pixelData.size());
    fileHeader.pixelDataOffset = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fileHeader.fileSize = fileHeader.pixelDataOffset + infoHeader.imageSize;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    file.write(reinterpret_cast<const char*>(pixelData.data()), pixelData.size());

    return file.good();
}

} // namespace FrameBoost::BmpWriter
