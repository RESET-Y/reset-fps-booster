#include "duplicate_detector.h"
#include "logger.h"

#include <string>
#include <cstring>
#include <algorithm>

#include "thumbnail_cs.h"

namespace FrameBoostBeta {

namespace {
void SafeRelease(IUnknown* obj) { if (obj) obj->Release(); }

} // namespace

bool DuplicateDetector::EnsureResources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDesc) {
    if (m_staging[0] && frameDesc.Width == m_frameWidth && frameDesc.Height == m_frameHeight && frameDesc.Format == m_format)
        return true;

    SafeRelease(m_thumbUAV);    m_thumbUAV = nullptr;
    SafeRelease(m_thumbTarget); m_thumbTarget = nullptr;
    SafeRelease(m_frameSRV);    m_frameSRV = nullptr;
    m_frameSRVSource = nullptr;
    SafeRelease(m_staging[0]); m_staging[0] = nullptr;
    SafeRelease(m_staging[1]); m_staging[1] = nullptr;
    m_stagingFilled[0] = m_stagingFilled[1] = false;
    m_writeIndex = 0;
    m_lastThumb.clear(); // frame geometry changed - previous samples are meaningless

    m_frameWidth = frameDesc.Width;
    m_frameHeight = frameDesc.Height;
    m_format = frameDesc.Format;

    // Thumbnail size, keeping the frame.s aspect ratio so each texel covers a
    // square-ish region.
    m_thumbActualWidth = kThumbWidth;
    m_thumbActualHeight = (std::max)(1u, kThumbWidth * frameDesc.Height / (std::max)(1u, frameDesc.Width));


    D3D11_TEXTURE2D_DESC thumbDesc{};
    thumbDesc.Width = m_thumbActualWidth;
    thumbDesc.Height = m_thumbActualHeight;
    thumbDesc.MipLevels = 1;
    thumbDesc.ArraySize = 1;
    thumbDesc.Format = frameDesc.Format;
    thumbDesc.SampleDesc.Count = 1;
    thumbDesc.Usage = D3D11_USAGE_DEFAULT;
    thumbDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&thumbDesc, nullptr, &m_thumbTarget))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the thumbnail target - duplicate detection disabled.");
        return false;
    }
    if (FAILED(device->CreateUnorderedAccessView(m_thumbTarget, nullptr, &m_thumbUAV))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the thumbnail UAV - duplicate detection disabled.");
        return false;
    }

    if (!m_thumbnailCS &&
        FAILED(device->CreateComputeShader(g_ThumbnailCS, sizeof(g_ThumbnailCS), nullptr, &m_thumbnailCS))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the thumbnail shader - duplicate detection disabled.");
        return false;
    }

    if (!m_dimsCB) {
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = 16; // four uints
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &m_dimsCB))) {
            Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the dimensions buffer - duplicate detection disabled.");
            return false;
        }
    }

    thumbDesc.Usage = D3D11_USAGE_STAGING;
    thumbDesc.BindFlags = 0;
    thumbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&thumbDesc, nullptr, &m_staging[0])) ||
        FAILED(device->CreateTexture2D(&thumbDesc, nullptr, &m_staging[1]))) {
        Logger::Log("[FrameBoostBeta] DuplicateDetector: could not create the staging thumbnail - duplicate detection disabled.");
        return false;
    }

    Logger::Log("[FrameBoostBeta] DuplicateDetector: full-frame thumbnail comparison at "
        + std::to_string(m_thumbActualWidth) + "x" + std::to_string(m_thumbActualHeight)
        + " - one compute dispatch, 64 samples per texel, whole frame covered.");
    return true;
}

bool DuplicateDetector::IsDuplicate(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame) {
    if (!frame) return false;

    D3D11_TEXTURE2D_DESC frameDesc{};
    frame->GetDesc(&frameDesc);
    if (!EnsureResources(device, frameDesc)) return false;

    // One dispatch: every thumbnail texel averages a grid of samples from the
    // region it stands for. Nothing on screen can move without changing it,
    // and nothing full-frame is copied to find that out.
    if (m_frameSRVSource != frame) {
        SafeRelease(m_frameSRV);
        m_frameSRV = nullptr;
        if (FAILED(device->CreateShaderResourceView(frame, nullptr, &m_frameSRV))) return false;
        m_frameSRVSource = frame;
    }

    D3D11_MAPPED_SUBRESOURCE cb{};
    if (SUCCEEDED(context->Map(m_dimsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb))) {
        UINT* dims = static_cast<UINT*>(cb.pData);
        dims[0] = m_frameWidth; dims[1] = m_frameHeight;
        dims[2] = m_thumbActualWidth; dims[3] = m_thumbActualHeight;
        context->Unmap(m_dimsCB, 0);
    }

    context->CSSetShader(m_thumbnailCS, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &m_dimsCB);
    context->CSSetShaderResources(0, 1, &m_frameSRV);
    context->CSSetUnorderedAccessViews(0, 1, &m_thumbUAV, nullptr);
    context->Dispatch((m_thumbActualWidth + 7) / 8, (m_thumbActualHeight + 7) / 8, 1);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    context->CSSetShaderResources(0, 1, &nullSrv);
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context->CopyResource(m_staging[m_writeIndex], m_thumbTarget);
    m_stagingFilled[m_writeIndex] = true;

    // Read the OTHER copy - the one filled on the previous call, whose GPU work
    // has long since finished. Mapping the one just written would mean waiting
    // for the GPU, which is the cost this avoids.
    const int readIndex = 1 - m_writeIndex;
    m_writeIndex = readIndex;
    if (!m_stagingFilled[readIndex]) return false; // first call: nothing to compare yet

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_staging[readIndex], 0, D3D11_MAP_READ, 0, &mapped))) return false;

    const size_t rowBytes = static_cast<size_t>(m_thumbActualWidth) * 4;
    std::vector<uint8_t> current(rowBytes * m_thumbActualHeight);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < m_thumbActualHeight; ++y)
        memcpy(current.data() + y * rowBytes, src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
    context->Unmap(m_staging[readIndex], 0);

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
    m_lastVerdict = duplicate;
    return duplicate;
}

DuplicateDetector::~DuplicateDetector() {
    SafeRelease(m_thumbUAV);
    SafeRelease(m_frameSRV);
    SafeRelease(m_thumbnailCS);
    SafeRelease(m_dimsCB);
    SafeRelease(m_thumbTarget);
    SafeRelease(m_staging[0]);
    SafeRelease(m_staging[1]);
}

} // namespace FrameBoostBeta
