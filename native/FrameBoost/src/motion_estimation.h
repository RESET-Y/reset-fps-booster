#pragma once
#include <d3d11.h>

namespace FrameBoost::MotionEstimation {

// Owns the persistent GPU resources (previous-frame texture, motion vector
// UAV, compute shader) needed to estimate motion between consecutive real
// frames. All per-frame work stays on the GPU - no CPU readback in the
// steady-state path (Phase 6). The only CPU readback is the optional debug
// visualization dump, which is explicitly throttled and separate.
class Estimator {
public:
    // Call once per Present with the CURRENT frame's back buffer. Internally
    // copies it into a persistent GPU texture, and - starting from the
    // second call onward - dispatches the compute shader comparing it
    // against the previous frame's copy. Returns true if a motion field was
    // actually computed this call (false on the very first frame, since
    // there is no previous frame yet).
    bool ProcessFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer);

    // Real, measured GPU time (in milliseconds) the last motion-estimation
    // dispatch took, via D3D11 timestamp queries. Returns -1 if not yet
    // available (queries are read a few frames late to avoid stalling).
    double LastGpuTimeMs() const { return m_lastGpuTimeMs; }

    // For debug visualization / Milestone 3 validation only. Returns the
    // SMOOTHED field (post spatial-averaging pass) - that is what any real
    // consumer (interpolation, visualization) should use, not the noisier
    // raw block-matching output.
    ID3D11Texture2D* MotionVectorTexture() const { return m_motionVectorSmoothTex; }
    UINT BlockCountX() const { return m_blockCountX; }
    UINT BlockCountY() const { return m_blockCountY; }

    // Milestone 4: what the Interpolator needs to consume this estimator's
    // output directly on the GPU - no readback anywhere in this path.
    ID3D11ShaderResourceView* PrevFrameSRV() const { return m_prevFrameSRV; }
    ID3D11ShaderResourceView* CurrFrameSRV() const { return m_currFrameSRV; }
    // Milestone 5: the actual GPU copy of this call's real rendered frame -
    // needed to restore the swapchain back buffer after presenting the
    // generated frame, so the real frame still gets displayed right after it.
    ID3D11Texture2D* CurrFrameTexture() const { return m_currFrameTex; }
    ID3D11Texture2D* PrevFrameTexture() const { return m_prevFrameTex; }
    ID3D11ShaderResourceView* MotionVectorSRV() const { return m_motionVectorSmoothSRV; }
    static constexpr UINT BlockSizePixels() { return 8; }

    ~Estimator();

private:
    bool EnsureResources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDesc);
    void ResolvePendingGpuTiming(ID3D11DeviceContext* context);

    bool m_initialized = false;
    UINT m_width = 0, m_height = 0;
    UINT m_blockCountX = 0, m_blockCountY = 0;
    // Coarse pyramid level: one coarse block per 4x4 fine blocks.
    UINT m_coarseCountX = 0, m_coarseCountY = 0;
    UINT m_coarsestCountX = 0, m_coarsestCountY = 0;

    ID3D11Texture2D* m_prevFrameTex = nullptr;
    ID3D11Texture2D* m_currFrameTex = nullptr;
    ID3D11ShaderResourceView* m_prevFrameSRV = nullptr;
    ID3D11ShaderResourceView* m_currFrameSRV = nullptr;

    ID3D11Texture2D* m_motionVectorRawTex = nullptr;
    ID3D11UnorderedAccessView* m_motionVectorRawUAV = nullptr;
    ID3D11ShaderResourceView* m_motionVectorRawSRV = nullptr;

    // Smoothed (3x3-averaged) motion field - this is what downstream
    // consumers (visualization now, interpolation from Milestone 4) read.
    ID3D11Texture2D* m_motionVectorSmoothTex = nullptr;
    ID3D11UnorderedAccessView* m_motionVectorSmoothUAV = nullptr;
    ID3D11ShaderResourceView* m_motionVectorSmoothSRV = nullptr;

    ID3D11ComputeShader* m_computeShader = nullptr;
    ID3D11ComputeShader* m_smoothShader = nullptr;
    ID3D11ComputeShader* m_coarseShader = nullptr;
    ID3D11ComputeShader* m_coarsestShader = nullptr;
    // Previous frame`s smoothed field, for temporal damping of the "wiggle".
    ID3D11Texture2D* m_motionVectorHistoryTex = nullptr;
    ID3D11ShaderResourceView* m_motionVectorHistorySRV = nullptr;
    bool m_haveMotionHistory = false;
    ID3D11Texture2D* m_coarsestMotionTex = nullptr;
    ID3D11UnorderedAccessView* m_coarsestMotionUAV = nullptr;
    ID3D11ShaderResourceView* m_coarsestMotionSRV = nullptr;

    // Quarter-resolution search stage. Its output seeds the fine search, so
    // the fine stage only has to refine locally - which is what makes a 54px
    // reach cheaper than the old 12px one.
    ID3D11Texture2D* m_coarseMotionTex = nullptr;
    ID3D11UnorderedAccessView* m_coarseMotionUAV = nullptr;
    ID3D11ShaderResourceView* m_coarseMotionSRV = nullptr;
    ID3D11Buffer* m_frameDimsCB = nullptr;
    ID3D11Buffer* m_blockGridDimsCB = nullptr;

    bool m_havePrevFrame = false;

    // GPU timing (Phase 6/7): a small ring of query sets so we never stall
    // waiting on the GPU - we only read a query's result several frames
    // after it was issued, by which point it is essentially always ready.
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

} // namespace FrameBoost::MotionEstimation
