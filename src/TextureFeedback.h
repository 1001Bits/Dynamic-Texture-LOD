#pragma once

#include <cstdint>

namespace GpuGather { struct Draw; }

namespace TextureFeedback
{
    using Resolve = bool (*)(const void* texture, std::uint64_t& generation);
    using Deliver = void (*)(std::uint64_t key, std::uint64_t generation, float demand, std::uint64_t frame);
    struct Stats
    {
        std::uint64_t submitted{}, completed{}, expired{}, rejected{}, failures{}, delivered{}, bytes{}, droppedDraws{};
        bool enabled{}, ready{};
    };
    // Initial integration adds GPU demand conservatively to native observations.
    // It does not infer occlusion or UV semantics for unclassified material slots.
    bool Install(bool enabled, Resolve resolve, Deliver deliver);
    void Observe(const GpuGather::Draw& draw);
    void OnFrame(std::uint64_t frame) noexcept;
    Stats Statistics();
}
