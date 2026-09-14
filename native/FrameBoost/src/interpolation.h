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
                        ID3D11ShaderResourceView* motionSRV, UINT width, UINT height, DXGI_FORMAT format,
                        // When given, the shader writes straight into this view instead of the
                        // interpolator`s own texture, which saves copying a full frame into the
                        // swapchain afterwards - 14 MB at 2560x1440, once per presented frame.
                        ID3D11UnorderedAccessView* targetUAV = nullptr);

    // Developer aid: tints generated frames so it is visually unambiguous
    // which frames on screen are generated (and whether they arrive at all).
    /// 0 = off, 1 = tint generated frames red, 2 = paint the pixels the
    /// occlusion test distrusts green.
    void SetDebugTint(unsigned int mode) { m_debugTint = mode; }

    // Where the next generated frame sits between the two real source
    // frames: 0 = the previous frame, 1 = the current one. 0.5 gives the
    // single midpoint frame of a 2x factor; a 3x factor calls this with
    // 1/3 and 2/3 before each real frame, a 4x factor with 1/4, 1/2, 3/4.
    // Clamped to keep the shader's sampling well behaved.
    void SetPhase(float t) { m_phaseT = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

    /// Predict FORWARD from the current frame instead of interpolating between
    /// two known ones. 0 = interpolate (the default); 0.5 = half an interval
    /// past the newest real frame.
    ///
    /// Interpolation has to hold the newest real frame back so its generated
    /// partner can be shown first, and that hold is a full half interval of
    /// added latency - 7.8 ms of a measured 13.4 ms at 64 FPS. Extrapolation
    /// pays nothing for it: the real frame goes out the moment it arrives.
    /// What it cannot do is know what is behind a moving object, because there
    /// is no later frame to copy that from.
    // Where the camera is being turned, in PIXELS of expected screen shift for
    // the instant this frame represents - derived from raw mouse movement that
    // no rendered frame has shown yet. Offered to the shader as one more
    // candidate vector, so a wrong prediction simply loses and costs nothing.
    // How far the per-pixel search should back off, from measured headroom.
    // 1 = full quality. Higher means only badly broken pixels pay for the
    // search - worse pixels on time, rather than better pixels too late.
    // Ceiling raised from 6 to 24. At 6 the regulator was pegged at its maximum
    // while interpolation still measured 13.3 ms against an 8.3 ms deadline,
    // 62% of frames late and 89% of the card taken - a regulator that cannot
    // reach the value it needs is not regulating.
    void SetQualityRelief(float r) { m_qualityRelief = r < 1.0f ? 1.0f : (r > 24.0f ? 24.0f : r); }

    void SetExtrapolateAhead(float a) { m_extrapolateAhead = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }

    // Pixels per real-frame interval above which no pixel is displaced at all.
    // 0 disables the cutoff, which is the default.
    void SetMotionCutoff(float c) { m_motionCutoff = c < 0.0f ? 0.0f : c; }

    // Draws small status squares in the generated frames' top-left corner:
    // bit 0 = low-latency mode (amber), bit 1 = transparency mode (cyan).
    void SetStatusFlags(unsigned int flags) { m_statusFlags = flags; }

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
    unsigned int m_debugTint = 0;
    unsigned int m_debugTintInBuffer = 0xFFFFFFFFu;
    float m_phaseT = 0.5f;          // midpoint - the 2x case
    float m_phaseTInBuffer = -1.0f; // forces the first upload

    // On-screen status indicator. The hotkeys had no visible feedback at all -
    // the overlay has no title bar and is hidden from the taskbar, so a mode
    // change could only be confirmed by reading the log file.
    unsigned int m_statusFlags = 0;
    float m_extrapolateAhead = 0.0f;
    float m_qualityRelief = 1.0f;
    float m_qualityReliefInBuffer = -1.0f;
    float m_motionCutoff = 0.0f;
    float m_motionCutoffInBuffer = -1.0f;
    float m_extrapolateAheadInBuffer = -1.0f;
    unsigned int m_statusFlagsInBuffer = 0xFFFFFFFFu;

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
