// Dynamic Texture LOD for Fallout 4 (OG 1.10.163 / NG 1.10.980+ / AE 1.11.x / VR 1.2.72)

#include "Budget.h"
#include "Controls.h"
#include "FramePump.h"
#include "Lod.h"
#include "Logging.h"
#include "core/Planner.h"
#include "core/Settings.h"
#include "Vram.h"

namespace
{
    constexpr REL::Version kRuntimeOG{ 1, 10, 163, 0 };
    constexpr REL::Version kRuntimeNG980{ 1, 10, 980, 0 };
    constexpr REL::Version kRuntimeNG984{ 1, 10, 984, 0 };
    constexpr REL::Version kRuntimeAE191{ 1, 11, 191, 0 };
    constexpr REL::Version kRuntimeAE221{ 1, 11, 221, 0 };
    constexpr REL::Version kRuntimeAE240{ 1, 11, 240, 0 };
    constexpr REL::Version kRuntimeVR{ 1, 2, 72, 0 };

    // Admit only the versions in the compatibility table. Exact Address
    // Library membership and code validation are additional checks.
    bool IsSupportedRuntime(const REL::Version& a_version)
    {
        if (REL::Module::IsVR()) {
            return a_version == kRuntimeVR;
        }
        if (REL::Module::IsNG()) {
            return a_version == kRuntimeNG980 || a_version == kRuntimeNG984 ||
                a_version == kRuntimeAE191 || a_version == kRuntimeAE221 || a_version == kRuntimeAE240;
        }
        return a_version == kRuntimeOG;
    }

    // MCM and F11 controls, also with the mod off or disabled in the INI.
    void StartControls()
    {
        Controls::Start();
        FramePump::SetDraw([](IDXGISwapChain*) {
            FramePump::Step("controls");
            Controls::OnFrame();
        });
    }

    void F4SEAPI OnMessage(F4SE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }

        switch (a_message->type) {
        case F4SE::MessagingInterface::kPreLoadGame:
            // Video memory empties and refills while a save loads: refill
            // waits for calm after it, and no jump is measured across it.
            dtl::budget::SetLoading(true);
            break;
        case F4SE::MessagingInterface::kNewGame:
        case F4SE::MessagingInterface::kPostLoadGame:
            dtl::budget::SetLoading(false);
            [[fallthrough]];
        case F4SE::MessagingInterface::kPostSaveGame:
            Lod::LogSummary();
            if (dtl::GetSettings().budget) {
                dtl::budget::LogSummary();
            }
            break;
        default:
            break;
        }
    }
}

// OG and VR loaders ask here first. NG and AE read F4SEPlugin_Version instead.
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    if (!a_f4se || !a_info) {
        return false;
    }

    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = Plugin::VERSION[0];

    return !a_f4se->IsEditor();
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    if (!a_f4se) {
        return false;
    }

    F4SE::Init(a_f4se, false);
    OpenLog();

    const auto version = REL::Module::get().version();
    logger::info("Fallout 4{} {}", REL::Module::IsVR() ? " VR" : "", version.string("."sv));

    if (!IsSupportedRuntime(version)) {
        logger::critical("This runtime isn't supported, the plugin stays off");
        return false;
    }

    dtl::LoadSettings("Data/F4SE/Plugins/DynamicTextureLOD.ini");

    const auto& settings = dtl::GetSettings();
    if (!settings.enabled) {
        logger::info("Disabled in the settings");
        if (settings.timeline) {
            // Observe native behavior without installing LOD hooks or starting
            // its planner. Budget assessment only feeds the diagnostic log.
            auto diagnostic = dtl::budget::FromIni();
            diagnostic.enabled = true;
            dtl::budget::Configure(diagnostic, nullptr);
            logger::info("Diagnostic baseline: DXGI telemetry only; native texture behavior, "
                         "LOD hooks and planner disabled");
            Vram::Start();
        }
        StartControls();
        return true;
    }
    if (!settings.screenSize && !settings.budget && settings.unseen != static_cast<std::uint32_t>(dtl::Unseen::Lowest)) {
        logger::info("ScreenSize and Budget are both off, nothing to do");
        StartControls();
        return true;
    }

    // Some forty call sites at 14 bytes each.
    F4SE::AllocTrampoline(1 << 12);

    // Now rather than at a later message, so objects loaded for the main
    // menu already go through the hooks.
    Budget::Configure();
    if (!Lod::Install()) {
        return false;
    }
    StartControls();
    // Ignoring pressure still needs the adapter's finite memory budget for
    // Starfield's per-cycle upgrade allowances.
    if (settings.budget || settings.policy == 1) {
        Vram::Start();
    }

    if (const auto messaging = F4SE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnMessage)) {
        logger::warn("No messaging interface, the summary won't be logged");
    }

    return true;
}

F4SE_EXPORT auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginName(Plugin::NAME);
    data.PluginVersion(Plugin::VERSION);
    data.AuthorName("Asciimov");
    data.UsesAddressLibrary(true);
    data.UsesSigScanning(false);
    data.IsLayoutDependent(true);
    data.HasNoStructUse(false);
    data.CompatibleVersions({ kRuntimeOG, kRuntimeNG980, kRuntimeNG984, kRuntimeAE191, kRuntimeAE221, kRuntimeAE240 });
    return data;
}();
