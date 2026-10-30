#pragma once

#include "GpuBounds.h"
#include <cstdint>

namespace GpuGather
{
    struct Eye
    {
        std::array<float, 3> position{};
        // left,right,top,bottom,near,far copied from the actual NiCamera view.
        std::array<float, 6> frustum{};
        bool orthographic{};
    };

    struct Draw
    {
        // Borrowed only for this synchronous callback. Never dereference them
        // from the planner or delayed GPU readback. Resolve texture generation
        // identities while the material is held by the native draw.
        void* geometry{};
        void* shaderProperty{};
        void* material{};
        std::uintptr_t vertexBuffer{};
        std::uint64_t bufferGeneration{}, frame{};
        GpuBounds::Bounds worldBounds;
        std::array<Eye, 2> eyes;
        std::uint32_t eyeCount{}, passDescriptor{};
        std::uint64_t vertexDescriptor{};
        // True means a native main-camera draw or cached command was submitted.
        // It does NOT mean passed per-pixel depth/HiZ occlusion.
        bool nativeMainViewDraw{ true };
    };

    struct Stats
    {
        std::uint64_t uploads{}, bounds{}, retired{}, drawCalls{}, accepted{},
            unsupported{}, missingBounds{}, otherView{}, callbackFailures{}, capacityRejected{};
        std::uint64_t cameraCalls{}, mainCameras{}, rejectedCameras{}, invalidCameras{}, lightingCalls{},
            missingView{}, staleView{}, nullPass{}, shaderSkinned{}, shaderTechnique{}, nullGeometry{},
            geometryType{}, geometrySkinned{}, missingShape{}, missingBuffer{}, descriptorMismatch{},
            invalidTransform{}, nullProperty{}, cachedCalls{}, cachedPasses{}, invalidCachedLists{},
            immediateCalls{}, shaderType{}, materialType{}, deformation{};
    };
    using Callback = void (*)(const Draw&);

    // Explicit experimental gate; absent/false means no patch. Install during
    // plugin load before game resources are created. Requires384 trampoline bytes.
    // Flat OG/NG/AE only; VR fails closed until its submitted-draw ABI is mapped.
    // No immediate-context calls occur here: submit the captured POD records to
    // GpuFeedback::Backend only from the renderer's actual Present/frame hook.
    bool Install(bool experimental, Callback callback);
    // Same exact rigid shader/material/property/geometry eligibility as gather,
    // without camera, upload-bound, buffer-ownership or frame requirements.
    // Borrowed live pass only. No statistics/engine writes. False before Install.
    bool CompatibleRigidPass(void* pass) noexcept;
    // Earlier geometry/property admission shares the exact deformation/skin
    // checks, but does not establish any future shader/technique compatibility.
    bool CompatibleRigidGeometry(void* geometry, void* property) noexcept;
    void SetFrame(std::uint64_t frame) noexcept;
    Stats Statistics() noexcept;
}
