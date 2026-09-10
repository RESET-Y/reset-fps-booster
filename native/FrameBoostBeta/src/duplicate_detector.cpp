#include "duplicate_detector.h"
#include "logger.h"

#include <string>
#include <cstring>

namespace FrameBoostBeta {

namespace {
void SafeRelease(IUnknown* obj) { if (obj) obj->Release(); }

// Number of mip levels for a full chain down to 1x1.
UINT FullMipLevels(UINT width, UINT height) {
    UINT levels = 1;
    while (width > 1 || height > 1) { width = width > 1 ? width / 2 : 1; height = height > 1 ? height / 2 : 1; ++levels; }
    return levels;
}

UINT MipDimension(UINT base, UINT level) {
    UINT d = base >> level;
    return d ? d : 1;
}
} // namespace

bool DuplicateDetector::EnsureResources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDesc) {
    if (m_staging && frameDesc.Width == m_frameWidth && frameDesc.Height == m_frameHeight && frameDesc.Format == m_format)
        return true;

    SafeRelease(m_mipSRV);      m_mipSRV = nullptr;
    SafeRelease(m_mipSource);   m_mipSource = nullptr;
    SafeRelease(m_thumbTarget); m_thumbTarget = nullptr;
    SafeRelease(m_staging);     m_staging = nullptr;
    m_lastThumb.clear(); // frame geometry changed - previous samples are meaningless

    m_frameWidth = frameDesc.Width;
    m_frameHeight = frameDesc.Height;
    m_format = frameDesc.Format;
    m_mipLevels = FullMipLevels(frameDesc.Width, frameDesc.Height);

    // Pick the mip level closest to (but not smaller than) the target
    // thumbnail width, so the GPU does the whole reduction and we read back
    // only a few KB.
    m_thumbMipLevel = 0;
    for (UINT level = 0; level < m_mipLevels; ++level) {
        m_thumbMipLevel = level;
        if (MipDimension(frameDesc.Width, level) <= kThumbWidth) break;
    }
    m_thumbActualWidth = MipDimension(frameDesc.Width, m_thumbMipLevel);
    m_thumbActualHeight = MipDimension(frameDesc.Height, m_thumbMipLevel);

    D3D11_TEXTURE2D_DESC mipDesc{};
    mipDesc.Width = frameDesc.Width;
    mipDesc.Height = frameDesc.Height;
    mipDesc.MipLevels = m_mipLevels;
    mipDesc.ArraySize = 1;
    mipDesc.Format = frameDesc.Format;
    mipDesc.SampleDesc.Count = 1;
    mipDesc.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET and the GENERATE_MIPS flag are both required for
    // GenerateMips to be allowed to write the chain.
    mipDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    mipDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    if (FAILED(device->CreateTexture2D(&mipDesc, nullptr, &m_mipSource))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the mip source - duplicate detection disabled.");
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(m_mipSource, nullptr, &m_mipSRV))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the mip SRV - duplicate detection disabled.");
        return false;
    }

    D3D11_TEXTURE2D_DESC thumbDesc{};
    thumbDesc.Width = m_thumbActualWidth;
    thumbDesc.Height = m_thumbActualHeight;
    thumbDesc.MipLevels = 1;
    thumbDesc.ArraySize = 1;
    thumbDesc.Format = frameDesc.Format;
    thumbDesc.SampleDesc.Count = 1;
    thumbDesc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&thumbDesc, nullptr, &m_thumbTarget))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the thumbnail target - duplicate detection disabled.");
        return false;
    }

    thumbDesc.Usage = D3D11_USAGE_STAGING;
    thumbDesc.BindFlags = 0;
    thumbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&thumbDesc, nullptr, &m_staging))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the staging thumbnail - duplicate detection disabled.");
        return false;
    }

    Logger::Log("[FrameBoostBeta] DuplicateDetector: full-frame thumbnail comparison at "
        + std::to_string(m_thumbActualWidth) + "x" + std::to_string(m_thumbActualHeight)
        + " (mip " + std::to_string(m_thumbMipLevel) + " of " + std::to_string(m_mipLevels) + ") - 100% frame coverage.");
    return true;
}

bool DuplicateDetector::IsDuplicate(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame) {
    if (!frame) return false;

    D3D11_TEXTURE2D_DESC frameDesc{};
    frame->GetDesc(&frameDesc);
    if (!EnsureResources(device, frameDesc)) return false;

    // Full frame into mip 0, then let the GPU reduce it. Every source pixel
    // contributes to the thumbnail, which is the whole point: nothing on
    // screen can move without changing it.
    context->CopySubresourceRegion(m_mipSource, 0, 0, 0, 0, frame, 0, nullptr);
    context->GenerateMips(m_mipSRV);
    context->CopySubresourceRegion(m_thumbTarget, 0, 0, 0, 0, m_mipSource, m_thumbMipLevel, nullptr);
    context->CopyResource(m_staging, m_thumbTarget);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;

    const size_t rowBytes = static_cast<size_t>(m_thumbActualWidth) * 4;
    std::vector<uint8_t> current(rowBytes * m_thumbActualHeight);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < m_thumbActualHeight; ++y)
        memcpy(current.data() + y * rowBytes, src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
    context->Unmap(m_staging, 0);

    bool duplicate = false;
    if (m_lastThumb.size() == current.size()) {
        // Strongest single TILE change, not the average over the image: a
        // small video area changing would otherwise be diluted by the static
        // desktop around it and the frame wrongly discarded.
        double maxTileDiff = 0.0;
        for (UINT ty = 0; ty < kTilesY; ++ty) {
            for (UINT tx = 0; tx < kTilesX; ++tx) {
                const UINT x0 = m_thumbActualWidth * tx / kTilesX;
                const UINT x1 = m_thumbActualWidth * (tx + 1) / kTilesX;
                const UINT y0 = m_thumbActualHeight * ty / kTilesY;
                const UINT y1 = m_thumbActualHeight * (ty + 1) / kTilesY;

                uint64_t sum = 0;
                size_t samples = 0;
                for (UINT y = y0; y < y1; ++y) {
                    const size_t rowStart = static_cast<size_t>(y) * rowBytes;
                    for (UINT x = x0; x < x1; ++x) {
                        const size_t px = rowStart + static_cast<size_t>(x) * 4;
                        for (int c = 0; c < 3; ++c) { // RGB; alpha carries no visible motion
                            int d = static_cast<int>(current[px + c]) - static_cast<int>(m_lastThumb[px + c]);
                            sum += static_cast<uint64_t>(d < 0 ? -d : d);
                            ++samples;
                        }
                    }
                }
                double avg = samples ? static_cast<double>(sum) / samples : 0.0;
                if (avg > maxTileDiff) maxTileDiff = avg;
            }
        }

        m_lastDifference = maxTileDiff;
        duplicate = maxTileDiff <= kDuplicateThreshold;
    }

    m_lastThumb = std::move(current);
    return duplicate;
}

DuplicateDetector::~DuplicateDetector() {
    SafeRelease(m_mipSRV);
    SafeRelease(m_mipSource);
    SafeRelease(m_thumbTarget);
    SafeRelease(m_staging);
}

} // namespace FrameBoostBeta
