#include "Fo4MeshData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Fo4Mesh {
    namespace {
        bool Valid(SourceView source) {
            if (source.stride == 0 || source.stride > 256 || source.vertices.empty() ||
                source.vertices.size() % source.stride != 0 || source.indices.empty() || source.indices.size() % 3 != 0)
                return false;
            const auto count = source.vertices.size() / source.stride;
            // FO4 BSTriShape's embedded vertex count is u16; 65536 cannot be represented.
            return count <= std::numeric_limits<std::uint16_t>::max() &&
                std::ranges::all_of(source.indices, [count](auto index) { return index < count; });
        }
    }

    std::optional<IndexRange> NativeLODRange(std::array<std::uint32_t, 3> counts,
                                           std::uint8_t mode, std::uint32_t totalTriangles) {
        const std::uint64_t sum = std::uint64_t(counts[0]) + counts[1] + counts[2];
        if (!totalTriangles || sum != totalTriangles || totalTriangles > UINT32_MAX / 3) return {};
        const auto index = static_cast<unsigned>(mode & 0x7f);
        if ((mode & 0x80) ? index >= 3 : index > 3) return {};
        std::uint32_t preceding = 0;
        for (unsigned i = 0; i < index; ++i) preceding += counts[i];
        return (mode & 0x80) ? IndexRange{preceding * 3, counts[index] * 3} : IndexRange{0, preceding * 3};
    }

    std::optional<Data> CompactPreservingVertexIDs(SourceView source, IndexRange range) {
        if (!Valid(source) || !range.count || range.first % 3 || range.count % 3 ||
            range.first > source.indices.size() || range.count > source.indices.size() - range.first) return {};
        const auto indices = source.indices.subspan(range.first, range.count);
        const auto lastVertex = *std::ranges::max_element(indices);
        Data out;
        out.stride = source.stride;
        out.indices.assign(indices.begin(), indices.end());
        out.vertices.assign(source.vertices.begin(), source.vertices.begin() + (std::size_t(lastVertex) + 1) * source.stride);
        return out;
    }

    std::optional<PrefixLevels> BuildPrefixLevels(SourceView source, std::array<std::uint32_t, 3> counts) {
        if (!Valid(source) || source.indices.size() / 3 > UINT32_MAX / 3) return {};
        const auto triangles = static_cast<std::uint32_t>(source.indices.size() / 3);
        if (!NativeLODRange(counts, 3, triangles)) return {};
        PrefixLevels result;
        result.levels[0] = {{0, static_cast<std::uint32_t>(source.indices.size())},
            static_cast<std::uint32_t>(source.vertices.size() / source.stride),
            AllocationBytes(source.vertices.size()) + AllocationBytes(source.indices.size() * sizeof(std::uint16_t))};
        result.count = 1;
        for (const std::uint8_t mode : {std::uint8_t(2), std::uint8_t(1)}) {
            const auto range = *NativeLODRange(counts, mode, triangles);
            if (!range.count || range.count == result.levels[result.count - 1].range.count) continue;
            const auto indices = source.indices.first(range.count);
            const auto vertices = static_cast<std::uint32_t>(*std::ranges::max_element(indices)) + 1;
            const auto bytes = AllocationBytes(std::uint64_t(vertices) * source.stride) +
                AllocationBytes(std::uint64_t(range.count) * sizeof(std::uint16_t));
            if (bytes >= result.levels[result.count - 1].bytes) return {};
            result.levels[result.count++] = {range, vertices, bytes};
        }
        return result;
    }

}
