#pragma once
#include <d3d11.h>

// MOTION BLUR along the estimator's motion vectors, as a pass between a
// finished frame and the presenter. See shaders/motion_blur.hlsl for what it
// does and what it deliberately leaves sharp.
//
// Off unless the engine is started with "blur" - a look some players want and
// competitive players do not, so it is never on by default.
class MotionBlur {
public:
    ~MotionBlur();

    // Strength = shutter as a fraction of one source interval's motion.
    // 0.5 is a 180-degree shutter at the source rate: clearly visible streaks.
    void SetStrength(float s) { m_shutter = s < 0.05f ? 0.05f : (s > 1.5f ? 1.5f : s); }
    void SetDebug(unsigned int mode) { m_debug = mode; }
    void SetMinLength(float px) { m_minLength = px < 0.5f ? 0.5f : px; }
    // Off = pixels that match in both real frames (HUD, crosshair) are blurred
    // like everything else instead of being kept sharp.
    void SetStillProtection(bool on) { m_stillProtection = on; }
    float Strength() const { return m_shutter; }

    // Blurs `source` (the frame about to be presented) and returns the result,
    // or nullptr if anything failed - the caller then presents `source` as is.
    // prev/curr/motion are the estimator's views of the pair the vectors
    // describe.
    ID3D11Texture2D* Apply(ID3D11Device* device, ID3D11DeviceContext* context,
                           ID3D11Texture2D* source,
                           ID3D11ShaderResourceView* prevSRV, ID3D11ShaderResourceView* currSRV,
                           ID3D11ShaderResourceView* motionSRV, UINT width, UINT height, UINT blockSize);

    double LastGpuTimeMs() const { return m_lastGpuMs; }

private:
    bool EnsureResources(ID3D11Device* device, UINT width, UINT height);
    ID3D11ShaderResourceView* SourceView(ID3D11Device* device, ID3D11Texture2D* source);

    ID3D11ComputeShader* m_shader = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11Buffer* m_params = nullptr;

    ID3D11Texture2D* m_outTex = nullptr;
    ID3D11UnorderedAccessView* m_outUAV = nullptr;
    UINT m_width = 0, m_height = 0;

    // Views onto the textures handed in, cached by pointer: the interpolator's
    // output and the estimator's current frame are the same two textures
    // every frame, so one view each is created once.
    static constexpr int kViewCache = 4;
    ID3D11Texture2D* m_viewTex[kViewCache] = {};
    ID3D11ShaderResourceView* m_viewSRV[kViewCache] = {};
    int m_viewNext = 0;

    // GPU time, read back a few frames late so the CPU never waits on it.
    static constexpr int kQueries = 4;
    ID3D11Query* m_disjoint[kQueries] = {};
    ID3D11Query* m_start[kQueries] = {};
    ID3D11Query* m_end[kQueries] = {};
    bool m_pending[kQueries] = {};
    int m_query = 0;
    double m_lastGpuMs = 0.0;

    float m_shutter = 0.5f;
    float m_minLength = 2.0f;
    bool m_stillProtection = true;
    unsigned int m_debug = 0;
    bool m_failed = false;
};
