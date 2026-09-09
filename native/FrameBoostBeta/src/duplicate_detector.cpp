#include "duplicate_detector.h"
#include "logger.h"

#include <string>
#include <cstring>

namespace FrameBoostBeta {

namespace {
void SafeRelease(IUnknown* obj) { if (obj) obj->Release(); }
} // namespace

bool DuplicateDetector::EnsureResources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDesc) {
    if (m_staging && frameDesc.Width == m_frameWidth && frameDesc.Height == m_frameHeight && frameDesc.Format == m_format)
        return true;

    SafeRelease(m_staging); m_staging = nullptr;
    m_lastPatches.clear(); // frame geometry changed - previous samples are meaningless

    m_frameWidth = frameDesc.Width;
    m_frameHeight = frameDesc.Height;
    m_format = frameDesc.Format;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kPatchSize * kPatchesX * kPatchesY; // all patches in one row
    desc.Height = kPatchSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = frameDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_staging))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create staging texture - duplicate detection disabled.");
        return false;
    }
    return true;
}

bool DuplicateDetector::IsDuplicate(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame) {
    if (!frame) return false;

    D3D11_TEXTURE2D_DESC frameDesc{};
    frame->GetDesc(&frameDesc);
    if (!EnsureResources(device, frameDesc)) return false;
    if (frameDesc.Width < kPatchSize * 2 || frameDesc.Height < kPatchSize * 2) return false;

    // Copy sample patches spread evenly over the frame into one staging row.
    UINT patchIndex = 0;
    for (UINT py = 0; py < kPatchesY; ++py) {
        for (UINT px = 0; px < kPatchesX; ++px) {
            UINT srcX = static_cast<UINT>((frameDesc.Width - kPatchSize) * (px + 1) / (kPatchesX + 1));
            UINT srcY = static_cast<UINT>((frameDesc.Height - kPatchSize) * (py + 1) / (kPatchesY + 1));

            D3D11_BOX box{};
            box.left = srcX;
            box.top = srcY;
            box.front = 0;
            box.right = srcX + kPatchSize;
            box.bottom = srcY + kPatchSize;
            box.back = 1;

            context->CopySubresourceRegion(m_staging, 0, patchIndex * kPatchSize, 0, 0, frame, 0, &box);
            ++patchIndex;
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;

    const uint8_t* rows = static_cast<const uint8_t*>(mapped.pData);
    const size_t bytesPerRow = static_cast<size_t>(kPatchSize) * kPatchesX * kPatchesY * 4;

    std::vector<uint8_t> current(bytesPerRow * kPatchSize);
    for (UINT y = 0; y < kPatchSize; ++y) {
        memcpy(current.data() + static_cast<size_t>(y) * bytesPerRow,
               rows + static_cast<size_t>(y) * mapped.RowPitch,
               bytesPerRow);
    }
    context->Unmap(m_staging, 0);

    bool duplicate = false;
    if (m_lastPatches.size() == current.size()) {
        // Strongest single-patch change, NOT the average across all patches.
        // Averaging fails badly for monitor capture: a small video area
        // changing gets diluted by a static desktop around it, and frames
        // with real new content were wrongly discarded as duplicates
        // (measured: 23-50 frames/s falsely skipped). Any patch that clearly
        // changed means the frame carries new content.
        const size_t patchBytes = static_cast<size_t>(kPatchSize) * 4;
        const size_t patchCount = kPatchesX * kPatchesY;
        double maxPatchDiff = 0.0;

        for (size_t p = 0; p < patchCount; ++p) {
            uint64_t patchDiff = 0;
            size_t samples = 0;
            for (UINT y = 0; y < kPatchSize; ++y) {
                const size_t rowStart = static_cast<size_t>(y) * (patchBytes * patchCount) + p * patchBytes;
                for (size_t i = 0; i < patchBytes; ++i) {
                    int d = static_cast<int>(current[rowStart + i]) - static_cast<int>(m_lastPatches[rowStart + i]);
                    patchDiff += static_cast<uint64_t>(d < 0 ? -d : d);
                    ++samples;
                }
            }
            double avg = samples ? static_cast<double>(patchDiff) / samples : 0.0;
            if (avg > maxPatchDiff) maxPatchDiff = avg;
        }

        m_lastDifference = maxPatchDiff;
        duplicate = maxPatchDiff <= kDuplicateThreshold;
    }

    m_lastPatches = std::move(current);
    return duplicate;
}

DuplicateDetector::~DuplicateDetector() {
    SafeRelease(m_staging);
}

} // namespace FrameBoostBeta
