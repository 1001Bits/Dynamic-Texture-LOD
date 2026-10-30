#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

// No engine hooks or automatic enablement. Every method, including destruction,
// must run on the owner of the game's immediate context. Inputs must originate
// from verified AABB/main-view gather; this component cannot prove visibility.
namespace GpuFeedback
{
    struct Identity
    {
        std::uint64_t key{}, generation{};
        bool operator==(const Identity&) const = default;
    };

    enum RecordFlags : std::uint32_t
    {
        MaterialVisible = 1,
        MeshEligible = 2,
        ForceMeshDemandFour = 4
    };

    // One record per geometry/material/view. Repeated IDs are merged by GPU
    // unsigned atomic max. This naturally supports both VR eyes; the caller
    // must provide the verified camera and world AABB for each eye/view.
    struct Record
    {
        std::array<float, 3> center{};
        float meshRatio{ 1 };
        std::array<float, 3> halfExtents{};
        std::uint32_t materialSlot{};
        std::array<float, 3> camera{};
        float viewScale{ 1 };
        std::uint32_t meshSlot{};
        std::uint32_t flags{};
        std::uint32_t reserved[2]{};
    };
    static_assert(sizeof(Record) == 64);

    struct Binding
    {
        std::uint32_t materialSlot{}, textureSlot{}, halfUV{};
        std::uint32_t applyUV{};  // 0: direct slot; 1: packed IEEE half U/V
    };
    static_assert(sizeof(Binding) == 16);

    struct Batch
    {
        std::span<const Record> records;
        std::span<const Binding> bindings;
        std::span<const Identity> textures;
        std::span<const Identity> meshes;
        std::uint32_t materialCount{};
    };

    struct Options
    {
        std::uint32_t maxRecords{ 65536 }, maxBindings{ 65536 };
        std::uint32_t maxMaterials{ 16384 }, maxTextures{ 32768 }, maxMeshes{ 8192 };
        std::uint32_t maxReadbackAge{ 8 };
    };

    struct Demand
    {
        Identity identity;
        std::uint32_t encoded{};
        float decoded{};
    };

    struct Result
    {
        std::uint64_t sourceFrame{}, completedFrame{};
        std::vector<Demand> textures, meshes;
        std::uint32_t invalidConversions{};
        // An undefined float-to-uint conversion invalidates the entire result.
        bool valid{};
    };

    struct Stats
    {
        std::uint64_t submitted{}, completed{}, expired{}, ringFull{}, rejected{}, failures{};
        std::uint64_t allocatedBytes{}, maxCompletedAge{};
    };

    class Backend
    {
    public:
        Backend();
        ~Backend();
        Backend(const Backend&) = delete;
        Backend& operator=(const Backend&) = delete;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* immediate, const Options& options = {});
        bool Submit(const Batch& batch, std::uint64_t frame);
        std::vector<Result> Poll(std::uint64_t frame);
        const Stats& Statistics() const noexcept;
        const std::string& LastError() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
