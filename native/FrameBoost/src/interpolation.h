#pragma once
#include <d3d11.h>

namespace FrameBoost::Interpolation {

// Milestone 4: generates a real intermediate frame from two real frames plus
// their motion field - genuine motion-compensated interpolation (see
// frame_interpolation.hlsl for exactly how, and why it isn't a blend).
// Entirely GPU-side per frame; no CPU readback except the optional debug
// dump path outside this class.
class Interpolator {
public:
    // prevSRV/currSRV: the two real source frames. motionSRV: the SMOOTHED
    // block-resolution motion field. width/height: real frame dimensions,
    // format: must match the source frames' format so the generated frame
    // can later be copied into a real swapchain back buffer (Milestone 5).
    bool GenerateFrame(ID3D11Device* device, ID3D11DeviceContext* context,
                        ID3D11ShaderResourceView* prevSRV, ID3D11ShaderResourceView* currSRV,
                        ID3D11ShaderResourceView* motionSRV, UINT width, UINT height, DXGI_FORMAT format);

    // Developer aid: tints generated frames so it is visually unambiguous
    // which frames on screen are generated (and whether they arrive at all).
    void SetDebugTint(bool enabled) { m_debugTint = enabled; }

    ID3D11Texture2D* GeneratedFrameTexture() const { return m_generatedTex; }
    double LastGpuTimeMs() const { return m_lastGpuTimeMs; }

    ~Interpolator();

private:
    bool EnsureResources(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format);
    void ResolvePendingGpuTiming(ID3D11DeviceContext* context);

    bool m_initialized = false;
    UINT m_width = 0, m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* m_generatedTex = nullptr;
    ID3D11UnorderedAccessView* m_generatedUAV = nullptr;
    ID3D11ComputeShader* m_computeShader = nullptr;
    ID3D11SamplerState* m_linearClampSampler = nullptr;
    ID3D11Buffer* m_paramsCB = nullptr;
    bool m_debugTint = false;
    bool m_debugTintInBuffer = false;

    static constexpr int kQueryRingSize = 4;
    struct QuerySet {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* start = nullptr;
        ID3D11Query* end = nullptr;
        bool pending = false;
    };
    QuerySet m_queries[kQueryRingSize];
    int m_queryWriteIndex = 0;
    double m_lastGpuTimeMs = -1.0;
};

} // namespace FrameBoost::Interpolation
