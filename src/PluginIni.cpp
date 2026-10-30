#include "PluginIni.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace PluginIni
{
    std::uint32_t Read(const char* a_section, const char* a_key, std::uint32_t a_fallback)
    {
        // The core reads the same file. A relative path would make Windows
        // look in its own directory.
        static const auto path = std::filesystem::absolute("Data/F4SE/Plugins/DynamicTextureLOD.ini").string();
        return ::GetPrivateProfileIntA(a_section, a_key, static_cast<INT>(a_fallback), path.c_str());
    }
}
