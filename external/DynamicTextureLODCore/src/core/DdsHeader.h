#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

// The few fields of a .dds file that matter for finding a mip in it. The
// pixel format itself is taken from the texture already in memory, which the
// game made from the same file, so it is not decoded here.
namespace dtl::dds
{
    struct Info
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipCount = 1;    // at least 1
        std::uint32_t dataOffset = 0;  // where the top mip starts
        bool          cubemap = false;
        bool          dx10 = false;    // has the extended header
    };

    // Reads the header, or nothing if this isn't a .dds file.
    inline std::optional<Info> Parse(std::span<const std::byte> a_file)
    {
        constexpr std::size_t kHeaderEnd = 4 + 124;  // magic + DDS_HEADER
        constexpr std::size_t kDx10Size = 20;        // DDS_HEADER_DXT10

        if (a_file.size() < kHeaderEnd) {
            return std::nullopt;
        }

        const auto read32 = [&](std::size_t a_offset) {
            std::uint32_t value = 0;
            std::memcpy(&value, a_file.data() + a_offset, sizeof(value));
            return value;
        };

        if (read32(0) != 0x20534444) {  // "DDS "
            return std::nullopt;
        }

        Info info;
        info.height = read32(0x0C);
        info.width = read32(0x10);
        info.mipCount = read32(0x1C) == 0 ? 1 : read32(0x1C);
        info.cubemap = (read32(0x70) & 0x200) != 0;  // DDSCAPS2_CUBEMAP

        const bool fourCC = (read32(0x50) & 0x4) != 0;  // DDPF_FOURCC
        info.dx10 = fourCC && read32(0x54) == 0x30315844;  // "DX10"
        info.dataOffset = static_cast<std::uint32_t>(kHeaderEnd + (info.dx10 ? kDx10Size : 0));

        if (a_file.size() < info.dataOffset || info.width == 0 || info.height == 0) {
            return std::nullopt;
        }
        return info;
    }
}
