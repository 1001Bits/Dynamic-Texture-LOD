#pragma once

// In-game controls: the mod's live on/off, the 4/6 GB budget mode and the development keys, from MCM
// (Data/MCM/Settings/DynamicTextureLOD.ini, where MCM saves what the player changed), else the plugin INI
// ([General] LowVramMode, DevKeys), else MCM's defaults (Data/MCM/Config/DynamicTextureLOD/settings.ini).
namespace Controls
{
    // Reads the settings now and once a second after, on its own thread.
    void Start();

    // Render thread, every frame: F11 switches the mod on or off while the development keys are on.
    void OnFrame();
}
