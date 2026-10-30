#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Optional largest texture sizes, as Skyrim's Texture Downscaler sets them:
// a largest edge per kind of texture, and path rules that override it. Off
// unless the ini sets one. Pure, for tests; the backend applies the levels.
namespace dtl::cap
{
    enum class Kind : std::uint8_t { Diffuse, Normal, Specular, Glow, Other };
    inline constexpr std::size_t kKinds = 5;
    inline constexpr std::array<std::string_view, kKinds> kKindNames{ "Diffuse", "Normal", "Specular", "Glow", "Other" };

    // A path rule: the first rule whose text is part of the path decides, for
    // every kind or only the one named before a colon ("Normal:door").
    struct Rule
    {
        std::string  text;        // lower case, backslashes
        std::int8_t  kind = -1;   // -1: every kind
        std::uint32_t edge = 0;   // 0: full size
    };

    // Fallout 76's mip skip by compression format ([Texture] iTextureMipSkip*
    // with iTextureMipSkipMinDimension): mips left off the top of textures of
    // one format, for example diffuse maps but not normal maps. Here it only
    // touches files whose larger edge is above the minimum, and never takes
    // one below it. BC3 includes BC2; Other is BC6H and uncompressed.
    enum class Format : std::uint8_t { BC1, BC3, BC4, BC5, BC7, Other };
    inline constexpr std::size_t kFormats = 6;
    inline constexpr std::array<std::string_view, kFormats> kFormatNames{ "BC1", "BC3", "BC4", "BC5", "BC7", "Other" };

    // From a DXGI_FORMAT value.
    inline constexpr Format FormatOf(std::uint8_t a_format)
    {
        if (a_format >= 70 && a_format <= 72) return Format::BC1;
        if (a_format >= 73 && a_format <= 78) return Format::BC3;
        if (a_format >= 79 && a_format <= 81) return Format::BC4;
        if (a_format >= 82 && a_format <= 84) return Format::BC5;
        if (a_format >= 97 && a_format <= 99) return Format::BC7;
        return Format::Other;
    }

    struct Skip
    {
        std::array<std::uint32_t, kFormats> levels{};  // 0: none
        std::uint32_t minDimension = 1024;

        bool Any() const { return std::ranges::any_of(levels, [](std::uint32_t l) { return l != 0; }); }
    };

    struct Limits
    {
        std::array<std::uint32_t, kKinds> edge{};  // 0: full size
        std::vector<Rule> rules;
        Skip skip;

        bool Any() const
        {
            return std::ranges::any_of(edge, [](std::uint32_t e) { return e != 0; }) ||
                   std::ranges::any_of(rules, [](const Rule& r) { return r.edge != 0; }) || skip.Any();
        }
    };

    // Lower case with backslashes, as rules are compared.
    inline std::string Normalize(std::string_view a_path)
    {
        std::string out(a_path);
        for (auto& c : out) {
            if (c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }

    // By the end of the file name: name_n.dds is a normal map, and so on.
    // Anything without a known ending counts as a diffuse map.
    inline Kind KindOf(std::string_view a_normalized)
    {
        auto name = a_normalized.substr(a_normalized.find_last_of('\\') + 1);
        if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) name = name.substr(0, dot);
        const auto under = name.find_last_of('_');
        if (under == std::string_view::npos) return Kind::Diffuse;
        const auto end = name.substr(under + 1);
        if (end == "d") return Kind::Diffuse;
        if (end == "n" || end == "msn") return Kind::Normal;
        if (end == "s") return Kind::Specular;
        if (end == "g" || end == "glow") return Kind::Glow;
        constexpr std::array<std::string_view, 14> others{ "e", "em", "m", "h", "p", "l", "lg", "r", "sk", "b",
            "a", "t", "rmaos", "mask" };
        return std::ranges::find(others, end) != others.end() ? Kind::Other : Kind::Diffuse;
    }

    // An ini key of [MaxSizeFolders]: "text" or "Kind:text". Empty text or an
    // unknown kind is no rule.
    inline std::optional<Rule> ParseRule(std::string_view a_key, std::uint32_t a_edge)
    {
        Rule rule;
        rule.edge = a_edge;
        if (const auto colon = a_key.find(':'); colon != std::string_view::npos) {
            const auto kind = Normalize(a_key.substr(0, colon));
            for (std::size_t i = 0; i < kKinds; ++i)
                if (kind == Normalize(kKindNames[i])) rule.kind = static_cast<std::int8_t>(i);
            if (rule.kind < 0) return std::nullopt;
            a_key = a_key.substr(colon + 1);
        }
        rule.text = Normalize(a_key);
        if (rule.text.empty()) return std::nullopt;
        return rule;
    }

    // The largest edge allowed for a texture at this path; 0 is full size.
    inline std::uint32_t EdgeFor(const Limits& a_limits, std::string_view a_normalized)
    {
        const auto kind = KindOf(a_normalized);
        for (const auto& rule : a_limits.rules) {
            if (rule.kind >= 0 && rule.kind != static_cast<std::int8_t>(kind)) continue;
            if (a_normalized.find(rule.text) != std::string_view::npos) return rule.edge;
        }
        return a_limits.edge[static_cast<std::size_t>(kind)];
    }

    // Mips to leave off the top of a width x height file so that its larger
    // edge fits. The backend bounds this by what the texture can lose.
    inline std::uint32_t LevelsFor(std::uint32_t a_edge, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (a_edge == 0) return 0;
        std::uint32_t levels = 0;
        for (auto larger = std::max(a_width, a_height); larger > a_edge && larger > 1; larger >>= 1) ++levels;
        return levels;
    }

    // Mips [MipSkip] leaves off a file of this DXGI format: at most its
    // format's count, while the larger edge stays at least the minimum.
    inline std::uint32_t SkipLevelsFor(const Skip& a_skip, std::uint8_t a_format, std::uint32_t a_width,
        std::uint32_t a_height)
    {
        const auto wanted = a_skip.levels[static_cast<std::size_t>(FormatOf(a_format))];
        std::uint32_t levels = 0;
        for (auto larger = std::max(a_width, a_height); levels < wanted && larger / 2 >= a_skip.minDimension &&
             larger > 1; larger >>= 1) ++levels;
        return levels;
    }

    // Both limits together: the stronger one decides.
    inline std::uint32_t CapLevels(const Limits& a_limits, std::string_view a_normalized, std::uint8_t a_format,
        std::uint32_t a_width, std::uint32_t a_height)
    {
        return std::max(LevelsFor(EdgeFor(a_limits, a_normalized), a_width, a_height),
            SkipLevelsFor(a_limits.skip, a_format, a_width, a_height));
    }
}
