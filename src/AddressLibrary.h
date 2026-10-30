#pragma once

#include <cstdint>
#include <istream>
#include <optional>

namespace Addr
{
    // Desktop Address Library: count followed by sorted {id, RVA} u64 pairs.
    // Read only O(log N) records, and fail closed on absent IDs/truncation.
    inline std::optional<std::uint64_t> ExactOffset(std::istream& input, std::uint64_t id)
    {
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size < std::streamoff(8)) return {};
        input.seekg(0);
        std::uint64_t count = 0;
        if (!input.read(reinterpret_cast<char*>(&count), 8) ||
            count != (static_cast<std::uint64_t>(size) - 8) / 16 ||
            (static_cast<std::uint64_t>(size) - 8) % 16) return {};
        std::uint64_t first = 0, last = count;
        while (first < last) {
            const auto middle = first + (last - first) / 2;
            std::uint64_t entry[2]{};
            input.seekg(static_cast<std::streamoff>(8 + middle * 16));
            if (!input.read(reinterpret_cast<char*>(entry), sizeof(entry))) return {};
            if (entry[0] == id) return entry[1] ? std::optional{entry[1]} : std::nullopt;
            if (entry[0] < id) first = middle + 1;
            else last = middle;
        }
        return {};
    }
}
