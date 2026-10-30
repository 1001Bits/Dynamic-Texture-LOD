#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <bit>
#include <cstdint>
#include <iostream>
#include "../src/NativeTextureLevels.h"

int main()
{
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessAffinityMask(GetCurrentProcess(), 3);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL feature{};
    const auto created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &feature, nullptr);
    if (FAILED(created)) return 1;
    unsigned checked{};
    for (const auto format : { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC7_UNORM }) {
        const bool bc = format != DXGI_FORMAT_R8G8B8A8_UNORM;
        for (const auto width : { 4u, 16u, 64u, 132u, 512u, 520u })
            for (const auto height : { 4u, 16u, 64u, 512u }) {
                const auto mips = std::bit_width(std::max(width, height));
                const auto maximum = NativeTextureLevels::Maximum(width, height, mips, 4, bc);
                for (std::uint32_t level = 0; level <= maximum; ++level) {
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = width >> level;
                    desc.Height = height >> level;
                    desc.MipLevels = mips - level;
                    desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.Format = format;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    // Validation only: WARP must accept each native descriptor
                    // admitted by the policy, including all intermediate mips.
                    const auto accepted = device->CreateTexture2D(&desc, nullptr, nullptr);
                    if (FAILED(accepted)) {
                        std::cerr << "Invalid admitted descriptor: " << width << 'x' << height
                            << " level=" << level << " format=" << format << " hr=" << accepted << '\n';
                        return 2;
                    }
                    ++checked;
                }
            }
    }
    std::cout << checked << " native descriptors accepted by software D3D11 validation\n";
}
