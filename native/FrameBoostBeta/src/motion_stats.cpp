#include "motion_stats.h"
#include "logger.h"

#include <cmath>
#include <string>
#include <vector>

namespace FrameBoostBeta {

bool MotionStats::SampleIfDue(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* motionTex) {
    if (!motionTex) return false;
    if (++m_frameCounter < kSampleIntervalFrames) return false;
    m_frameCounter = 0;

    D3D11_TEXTURE2D_DESC desc{};
    motionTex->GetDesc(&desc);

    if (!m_staging || desc.Width != m_width || desc.Height != m_height) {
        if (m_staging) { m_staging->Release(); m_staging = nullptr; }
        m_width = desc.Width;
        m_height = desc.Height;

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &m_staging))) {
            Logger::Log("[FrameBoostBeta] MotionStats: could not create the staging texture - motion diagnostics disabled.");
            return false;
        }
    }

    context->CopyResource(m_staging, motionTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;

    size_t movingBlocks = 0, totalBlocks = 0;
    double magnitudeSum = 0.0, magnitudeMax = 0.0;

    for (UINT y = 0; y < m_height; ++y) {
        const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < m_width; ++x) {
            const float mx = row[x * 2];
            const float my = row[x * 2 + 1];
            const double magnitude = std::sqrt(static_cast<double>(mx) * mx + static_cast<double>(my) * my);
            ++totalBlocks;
            // Half a pixel: below that the block is standing still as far as
            // interpolation is concerned.
            if (magnitude >= 0.5) {
                ++movingBlocks;
                magnitudeSum += magnitude;
                if (magnitude > magnitudeMax) magnitudeMax = magnitude;
            }
        }
    }
    context->Unmap(m_staging, 0);

    if (totalBlocks == 0) return false;
    m_movingPercent = 100.0 * static_cast<double>(movingBlocks) / static_cast<double>(totalBlocks);
    m_meanMagnitude = movingBlocks ? magnitudeSum / static_cast<double>(movingBlocks) : 0.0;
    m_maxMagnitude = magnitudeMax;
    return true;
}

MotionStats::~MotionStats() {
    if (m_staging) m_staging->Release();
}

} // namespace FrameBoostBeta
