#pragma once
#include <d3d11.h>
#include <string>

namespace FrameBoostBeta {

// Writes GPU textures to disk as BMPs, for looking at what the interpolation
// actually produces.
//
// A normal screen capture cannot answer that: the overlay sets
// WDA_EXCLUDEFROMCAPTURE so that monitor capture does not feed back on
// itself, which also makes it invisible to any screenshot. So the frames
// have to come from inside the engine, where the generated frame and the two
// real frames it was built from can be saved side by side and compared.
//
// Debug path only - it stalls the GPU pipeline to read back a full frame,
// and is triggered by hand.
namespace FrameDump {

// Saves one texture. Returns false and logs on failure.
bool SaveTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11Texture2D* texture, const std::wstring& path);

// Saves the generated frame together with the two real frames it came from,
// numbered so they sort in order, into the log directory.
void SaveComparison(ID3D11Device* device, ID3D11DeviceContext* context,
                    ID3D11Texture2D* previousReal,
                    ID3D11Texture2D* generated,
                    ID3D11Texture2D* currentReal);

} // namespace FrameDump
} // namespace FrameBoostBeta
