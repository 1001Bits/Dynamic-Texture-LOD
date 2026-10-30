#pragma once

// ── Engine addresses, per runtime ───────────────────────────────────────────
//
// OG  1.10.163            Address Library IDs
// NG  1.10.980 / 984      Address Library IDs (NG numbering)
// AE  1.11.191 and later  the same NG IDs, present in every 1.11 bin checked
// VR  1.2.72              offsets from the image base
//
// Each was found in the Combined Ghidra project for OG, NG 1.10.984, AE
// 1.11.240 and VR 1.2.72, and its ID read back from that runtime's Address
// Library. Call sites are not listed: Lod.cpp finds them by scanning for
// calls to these functions and checks how many it finds.

namespace Addr
{
    struct Location
    {
        std::uint32_t  og;  // 1.10.163 ID
        std::uint32_t  ng;  // 1.10.980+ ID
        std::uintptr_t vr;  // 1.2.72 offset
    };

    // Exact ID lookup: CommonLib's desktop lower_bound accepts the following
    // entry when an ID is absent. Never use that behavior to install a hook.
    std::uintptr_t Resolve(const Location& a_location);
    std::uintptr_t ImageAddress(std::uintptr_t offset);
    std::uintptr_t RendererDataSlot();

    // std::uint32_t GetDesiredDegradeLevel(const NiPoint3* center, float radius)
    // An object's texture level from its distance, 0 to 2. Asked when objects
    // load and by the texture update stage.
    inline constexpr Location GetDesiredDegradeLevel{ 363807, 2229495, 0xDBAFA0 };

    // std::uint32_t BSGraphics::Renderer::GetTextureMaxDegradeLevel(const Texture*)
    // Always 3. The texture update stage takes the smaller of this and the
    // object's level for each texture.
    inline constexpr Location GetTextureMaxDegradeLevel{ 1170498, 2277263, 0x1DBF440 };

    // The texture update stage: ProcessObjects::UpdateRec walks a node and its
    // children, ProcessObjects4's pass does the same for always-rendered
    // objects, and the texture functor runs once per texture of an object.
    inline constexpr Location UpdateRec{ 145470, 2229528, 0xDBDD70 };
    inline constexpr Location UpdateAlwaysRender{ 324232, 2229473, 0xDBCED0 };
    inline constexpr Location TextureFunctor{ 105704, 2229468, 0xDBBD70 };

    // Toggles bRoundToggleS after the texture traversal completes (also on
    // an explicit reset). Observations age by these rounds, never by time.
    inline constexpr Location StartTextureRound{ 812159, 2229526, 0xDBD940 };

    // Test arm only, OG only (no NG/VR IDs): the texture update stage's first
    // stage object's vtable (TextureUpdateStart, slot 0 is Update, which
    // returns true to end the stage for this frame), that Update, and Main's
    // bForcedUpdate byte (set during ForceUpdateTextures after loads).
    inline constexpr Location TextureUpdateStartVtable{ 492907, 0, 0 };
    inline constexpr Location TextureUpdateStartUpdate{ 943457, 0, 0 };
    inline constexpr Location ForcedTextureUpdate{ 745903, 0, 0 };

    // How far into each of those the calls can be. Their sizes, rounded up:
    // UpdateRec 0x2DA (OG) / 0x362 (NG), the always-render pass ~0x3A0 /
    // 0x407, the texture functor 0x1EF / 0x1F3.
    inline constexpr std::size_t kUpdateRecSize = 0x400;
    inline constexpr std::size_t kUpdateAlwaysRenderSize = 0x480;
    inline constexpr std::size_t kTextureFunctorSize = 0x220;

    // The vtables of BSLightingShaderProperty and BSEffectShaderProperty.
    // Slot 0x35 (0x36 on VR) is ForEachTexture(ForEachVisitor&), which the texture update
    // stage calls with its functor; their materials carry the UV tiling.
    inline constexpr Location LightingShaderPropertyVtbl{ 241915, 241915, 0x30A5C18 };
    inline constexpr Location EffectShaderPropertyVtbl{ 708622, 708622, 0x30A5938 };
    inline std::size_t ForEachTextureSlot() { return REL::Module::IsVR() ? 0x36 : 0x35; }

    // BSGraphics::State, screenHeight at +0x84 on every runtime.
    inline constexpr Location GraphicsState{ 600795, 2704621, 0x65A2AB0 };
    inline constexpr std::size_t kScreenHeightOffset = 0x84;

    // ID3D11Device* BSGraphics::pDevice, for the video memory readings.
    inline constexpr Location D3DDevice{ 1454788, 2704430, 0x60F3CF0 };
    inline constexpr Location WorldRootCamera{ 384264, 2228956, 0xD87550 };
    inline constexpr Location RendererData{ 1235449, 2704429, 0x60F3CE8 };
    // Native D3D texture accounting and stream-upgrade ceiling, uint64 bytes.
    // The latter is initialized from the selected adapter at device creation.
    inline constexpr Location NativeTextureBytes{ 495855, 2704734, 0x65A3C78 };
    inline constexpr Location NativeTextureUpgradeLimit{ 610992, 2666768, 0x38AD6F0 };
}
