#pragma once

#include <cstdint>

// FO4-only keys in Data/F4SE/Plugins/DynamicTextureLOD.ini that the shared
// core does not parse ([Budget] HostRamFloorMB, SteerToTarget).
namespace PluginIni
{
    std::uint32_t Read(const char* a_section, const char* a_key, std::uint32_t a_fallback);
}
