#include "frame_dump.h"
#include "logger.h"
#include "../../FrameBoost/src/bmp_writer.h"

#include <shlobj.h>
#include <string>

namespace FrameBoostBeta::FrameDump {

namespace {

std::wstring DumpDirectory() {
    PWSTR localAppData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        dir = std::wstring(localAppData) + L"\\ResetFpsBooster\\Logs";
        CoTaskMemFree(localAppData);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

} // namespace

bool SaveTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11Texture2D* texture, const std::wstring& path) {
    if (!device || !context || !texture) return false;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    // A staging copy is the only way to read a DEFAULT-usage texture, and the
    // copy has to drop everything the GPU-side texture carries for its own
    // purposes: mip levels, bind flags, and the misc flags that go with them.
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        Logger::Log("[FrameBoostBeta] Frame dump: could not create the staging texture.");
        return false;
    }

    context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        Logger::Log("[FrameBoostBeta] Frame dump: could not map the staging texture.");
        return false;
    }

    const bool ok = FrameBoost::BmpWriter::SaveRgba8AsBmp(
        path, desc.Width, desc.Height,
        static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch);

    context->Unmap(staging, 0);
    staging->Release();
    return ok;
}

void SaveComparison(ID3D11Device* device, ID3D11DeviceContext* context,
                    ID3D11Texture2D* previousReal,
                    ID3D11Texture2D* generated,
                    ID3D11Texture2D* currentReal) {
    const std::wstring dir = DumpDirectory();
    if (dir.empty()) return;

    // Numbered so they sort into the order they belong on the timeline: the
    // generated frame sits between the two real ones, which is exactly the
    // comparison worth looking at.
    const bool a = SaveTexture(device, context, previousReal, dir + L"\\frame_1_previous_real.bmp");
    const bool b = SaveTexture(device, context, generated, dir + L"\\frame_2_generated.bmp");
    const bool c = SaveTexture(device, context, currentReal, dir + L"\\frame_3_current_real.bmp");

    Logger::Log(std::string("[FrameBoostBeta] Frame dump written to the log directory: previous=")
        + (a ? "ok" : "FAILED") + ", generated=" + (b ? "ok" : "FAILED")
        + ", current=" + (c ? "ok" : "FAILED"));
}

} // namespace FrameBoostBeta::FrameDump
