#include "Controls.h"
#include "Lod.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    std::atomic<bool> g_devKeys{ false };
    bool g_f11Down = false;  // render thread only

    std::string Absolute(const char* a_relative)
    {
        // A relative path would make Windows look in its own directory.
        return std::filesystem::absolute(a_relative).string();
    }

    // MCM's saved value, else the plugin INI's, else MCM's default, else a_default.
    int Read(const char* a_mcmKey, const char* a_pluginKey, int a_default)
    {
        static const auto saved = Absolute("Data/MCM/Settings/DynamicTextureLOD.ini");
        static const auto plugin = Absolute("Data/F4SE/Plugins/DynamicTextureLOD.ini");
        static const auto defaults = Absolute("Data/MCM/Config/DynamicTextureLOD/settings.ini");
        if (const int value = ::GetPrivateProfileIntA("General", a_mcmKey, -1, saved.c_str()); value >= 0) return value;
        if (a_pluginKey)
            if (const int value = ::GetPrivateProfileIntA("General", a_pluginKey, -1, plugin.c_str()); value >= 0) return value;
        if (const int value = ::GetPrivateProfileIntA("General", a_mcmKey, -1, defaults.c_str()); value >= 0) return value;
        return a_default;
    }

    void Watch()
    {
        // The live switch follows MCM only when it changes there, so F11 can flip it in between.
        int enabled = -1;
        for (;;) {
            g_devKeys.store(Read("bDevKeys", "DevKeys", 0) != 0, std::memory_order_relaxed);
            Lod::SetBudgetMode(Read("bLowVramMode", "LowVramMode", 0) != 0);
            if (const int now = Read("bEnabled", nullptr, 1); now != enabled) {
                if (enabled >= 0 || now == 0) Lod::SetActive(now != 0);
                enabled = now;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    bool GameFocused()
    {
        DWORD process = 0;
        ::GetWindowThreadProcessId(::GetForegroundWindow(), &process);
        return process == ::GetCurrentProcessId();
    }
}

namespace Controls
{
    void Start()
    {
        std::thread(Watch).detach();
    }

    void OnFrame()
    {
        const bool down = g_devKeys.load(std::memory_order_relaxed) && (::GetAsyncKeyState(VK_F11) & 0x8000) != 0 &&
                          GameFocused();
        if (down && !g_f11Down) {
            const bool active = Lod::SetActive(!Lod::Active());
            logger::info("F11: Texture LOD {}", active ? "on" : "off");
        }
        g_f11Down = down;
    }
}
