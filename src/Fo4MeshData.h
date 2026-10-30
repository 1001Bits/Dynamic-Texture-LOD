#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Fo4Mesh {
    // Native AddBufferToCreate rounds each owning allocation to four bytes.
    constexpr std::uint64_t AllocationBytes(std::uint64_t bytes) { return (bytes + 3) & ~3ull; }
    struct IndexRange { std::uint32_t first = 0, count = 0; };
    struct PrefixLevel { IndexRange range; std::uint32_t vertices = 0; std::uint64_t bytes = 0; };
    struct PrefixLevels { std::array<PrefixLevel, 3> levels{}; std::uint8_t count = 0; };
    struct SourceView {
        std::span<const std::byte> vertices;
        std::span<const std::uint16_t> indices;
        std::uint32_t stride = 0;
    };
    struct Data {
        std::vector<std::byte> vertices;
        std::vector<std::uint16_t> indices;
        std::uint32_t stride = 0;
        SourceView View() const { return {vertices, indices, stride}; }
        std::uint64_t Bytes() const { return vertices.size() + indices.size() * sizeof(std::uint16_t); }
    };

    // Keep original vertex IDs (including holes) so every shader sees the same
    // SV_VertexID. Only the unused vertex tail and unselected index range leave
    // residency. This is the conservative runtime adapter's compaction mode.
    std::optional<Data> CompactPreservingVertexIDs(SourceView source, IndexRange range);

    // FO4 BSMeshLODTriShape's three triangle-count segments. A high-bit
    // mode draws one segment; a normal mode draws the preceding prefix.
    std::optional<IndexRange> NativeLODRange(std::array<std::uint32_t, 3> triangleCounts,
                                            std::uint8_t mode, std::uint32_t totalTriangles);
    // Source policy levels for native cumulative prefixes only. Empty levels
    // would hide the geometry, and duplicate levels cannot save residency;
    // neither is exposed to the streaming planner. The full source is level0.
    std::optional<PrefixLevels> BuildPrefixLevels(SourceView source, std::array<std::uint32_t, 3> counts);

}
