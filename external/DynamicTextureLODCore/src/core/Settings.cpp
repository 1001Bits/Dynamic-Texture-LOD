#include "Settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <SimpleIni.h>
#include <spdlog/spdlog.h>

namespace dtl
{
    namespace
    {
        Settings g_settings;

        constexpr std::string_view kDefaultFile = R"(; Dynamic Texture LOD. Read when the game starts.

[General]
; 0 turns the plugin off.
Enabled=1
; 0 trace, 1 debug, 2 info, 3 warnings, 4 errors, 5 critical only.
LogLevel=2

[LOD]
; 1: Starfield 1.16.236 angular thresholds and staged budget planner.
; 0: legacy resolution-aware pixel policy. Backend geometry/visibility and
; GPU destruction evidence still determine how closely a port can match.
Policy=1
; 1: each texture keeps only the detail its object can show at its size on
; screen, and gets it back as you come closer. 0: full detail on everything
; you can see, unless video memory runs short.
ScreenSize=1
; Extra mips kept above what the screen can show, for meshes that pack more
; of a texture into a small area than the estimate assumes. Policy 1 never
; lowers a texture on screen past this check, except over the budget line. 0 to 3.
SafetyMargin=1
; Within this many game units of the camera, request full detail and give
; restores priority. 875 is about 12.5 metres in Fallout 4 and Skyrim;
; it approximates Starfield's near range. Emergency pressure can still lower
; these textures. 0 turns the near preference off.
NearDistance=875
; Textures on nothing you can see. 0: keep their detail. 1: drop to their
; lowest level first when video memory gets tight. 2: always drop to their
; lowest level, as Starfield does; frees the most, but they come back as you
; turn around. 3: treat them like the ones you see, by their size at their
; distance; turning around reloads nothing.
Unseen=3
; 1: lower nothing while video memory has room to spare, and lower only what
; the shortage needs once it runs short: least needed first, never a texture
; past what the screen shows outside an emergency. 0: follow the screen size
; at all times, as Starfield does.
PressureOnly=1
; 1: Starfield 1.16.236 exactly, for comparison tests: Unseen=2, no quality
; guard (never more detail than the game, never a visible texture lowered past
; what the screen shows), no [Smoothing], PressureOnly=0, Target=512, none of
; the refill or counter settings, AutoTarget, SteerUpgradeLimit, UpgradeHoldSpeed,
; NoLowerRadius, [MaxSize] or [MipSkip]. 0 is the normal setting.
StarfieldExact=0
; Textures of anything this close to the camera (game units, measured to its
; bounds) always keep the game's own detail, even in an emergency and under
; [MaxSize] or [MipSkip], as Fallout 76's fPRTNoDegradeRadius (100) keeps them.
; Large objects you stand inside count as close. 0 turns it off. 0 to 100000.
NoLowerRadius=0

[Smoothing]
; Fallout 4 and Skyrim re-create a texture for every level change and re-read
; its file to raise it again, so changes are spaced out.
; Out of view this long (ms) before a texture counts as unseen. 0 to 60000.
UnseenDelayMs=3000
; No lowering this long (ms) after a texture's last change, unless video
; memory is over the budget line. 0 to 60000.
DegradeCooldownMs=3000
; A lowering needs the texture's rank this far (percent) under its threshold.
; Raising never waits. 0 to 50.
HysteresisPercent=20
; With video memory to spare, plan at most this often (ms). When it is tight
; or over, every frame. 0 to 2000.
RoomCycleMs=250
; No lowering this long (ms) after a texture was raised, unless video memory
; is over the budget line: lowering it again would waste the file read.
; 0 to 600000.
RaiseCooldownMs=30000
; While the camera moves faster than this (game units per second), raise
; nothing; lowering goes on. Detail comes back once it has been slower for a
; second. The game holds its own raises the same way above its
; fTextureUpgradeDisabledMinSpeed, 600; running and sprinting stay below it.
; Fast travel and loading doors do not count as movement. 0 turns it off.
; 0 to 100000.
UpgradeHoldSpeed=600
; Every texture change is a re-create on the game's single creation thread,
; which the frame waits on. Lowerings that free less than this (KB) are not
; made. 0 turns it off. 0 to 65536.
MinLoweringKB=0
; Accept at most this many texture level changes per second. 0 turns it off.
; 0 to 100000.
MaxChangesPerSecond=0

[Budget]
; 1: apply memory priorities. Starfield policy uses a minimum 34 MB demotion
; target per cycle, a 10 MB/frame smoothed budget, staged trades and balance.
; Enabled=0 requests full detail under Starfield policy, still in finite slices.
; Detail comes back in slices
; below the upgrade ceiling, including above the target. At the ceiling,
; upgrades require a trade with less important textures. Over the budget
; line, the least important textures lower until the estimated overage is met.
Enabled=1
; MB subtracted from the driver's video memory budget to set the budget line.
; Starfield: 128. 0 to 4096.
Reserve=128
; MB from the budget line down to the target. Above the target, video memory
; counts as tight. With Reserve=128 the default keeps 1 GB of the budget free:
; doors and new cells take 0.5 to 1.5 GB at once on any card. Starfield: 512.
; 128 to 8192.
Target=896
; How far from the target towards the budget line (percent) detail still
; comes back. Starfield: 65. 0 to 100.
UpgradeCeiling=65
; Frames in a row over the budget line before emergency lowering starts. A
; single reading spikes during loads. Starfield: 1. 1 to 30.
OverDebounceFrames=3
; 1: lowerings requested in the last two seconds count against an overage,
; since they are not in the driver's reading yet. Starfield: 0.
CreditPendingFrees=1
; 1: with over a gigabyte of headroom, read video memory every 16 frames
; instead of every frame. Starfield: 0.
AdaptiveSampling=1
; With PressureOnly=1, textures lowered under pressure come back to the game's
; own level ("refill") only once this many MB under the target are free, and
; then only until three quarters of it is used, so refill never brings the
; pressure straight back. 0 to 8192.
RefillGap=1024
; ...and only this long (ms) after video memory was last tight and after the
; last load: during loads video memory looks emptier than it is. 0 to 600000.
RefillCalmMs=30000
; 1: also read this game's GPU memory from Windows' performance counters, as
; Task Manager shows it, and use the larger of that and DXGI's reading, which
; can miss allocations. Memory paged out of video memory counts as over the
; budget while it keeps rising. No admin rights needed. Starfield: 0.
ProcessCounters=1
; Paged-out memory (MB) that, while still rising, counts as over the budget.
; 0 turns this off. 0 to 8192.
OverflowSharedMB=256
; 1: after 3 minutes of play, keep free the largest jump in video memory seen
; within 3 seconds outside loading screens, plus a quarter, when that is more
; than Reserve plus Target; at most a quarter of the driver's budget. Skyrim
; Performance Solution recommends its reserve the same way. Starfield: 0.
AutoTarget=1
; Fallout 4 only. 1: while video memory is tight or over, the game's own
; streamer may raise textures only into the room left under the budget line.
; Otherwise it stops only at its fixed limit, video memory minus 640 MB. With
; room to spare the game keeps its own limit. Starfield: 0.
SteerUpgradeLimit=1
; 1: log budget decisions without applying memory-pressure limits.
; Starfield policy requests full detail; the hypothetical limited plan is logged.
; Pixel policy keeps ScreenSize and Unseen=2 active.
DryRun=0
; 1: log video memory use every second, for bug reports.
LogTimeline=0

[Source]
; Starfield 1.16.236 fTextureDegradeScreenSize0..3. Descending angular ranks;
; the final threshold also gates source material gathering. Nonnegative values.
TextureDegradeScreenSize0=0.1
TextureDegradeScreenSize1=0.05
TextureDegradeScreenSize2=0.025
TextureDegradeScreenSize3=0.0125
; fMeshLODScreenSize0..3; mesh thresholds are these base sizes multiplied by
; (1 - fMeshLODFadeScreenSizePercent - fMeshLODScreenSizePercentStreamingBias).
; Defaults produce 0.15/0.075/0.0375/0.01875. Fade+bias must be at most 1.
MeshLODScreenSize0=0.2
MeshLODScreenSize1=0.1
MeshLODScreenSize2=0.05
MeshLODScreenSize3=0.025
MeshLODFadeScreenSizePercent=0.2
MeshLODScreenSizePercentStreamingBias=0.05
; uiMaximumIncreasePerIterationScreenSize and uiMinimumDecreasePerIterationScreenSize,
; expressed here in MiB. Accepted requests may overshoot the final slice. 0..4096.
MaximumIncreasePerIterationMB=34
MinimumDecreasePerIterationMB=34
; bNoMeshesUpgradeLimits: mesh upgrades bypass emergency and allowance stops,
; but accepted bytes still consume the shared allowance before textures run.
; When 1, mesh demotions also do not pay the shared decrease target.
NoMeshesUpgradeLimits=1
; fLODScreenSizeTimeSlice defaults to 10; its source unit is inferred as ms.
; This port uses milliseconds and checks after each 100-entry chunk. 0..100.
; 0 still runs one bounded chunk. Snapshot gathering/sorting is outside this slice.
TimeSliceMilliseconds=10

[MaxSize]
; Optional largest size per kind of texture, in pixels along its longer edge,
; as Skyrim's Texture Downscaler sets them. 0 keeps the game's own size. A
; size here keeps less detail than the game would, near or far, to save video
; memory. Fallout 4 takes at most three halvings off a texture's file size, and
; only from textures the game itself may lower. The kind comes from the end of
; the file name: _d diffuse, _n normal, _s specular, _g glow; masks, height
; maps and the like are Other; anything else counts as diffuse. 0 to 16384.
Diffuse=0
Normal=0
Specular=0
Glow=0
Other=0

[MaxSizeFolders]
; Path rules that override [MaxSize], first match wins: part of a texture's
; path, then its largest size, 0 for the game's own size. A kind and a colon
; in front limit a rule to that kind. Examples:
; \actors\character\=0
; \interface\=0
; Normal:\landscape\=1024

[MipSkip]
; Optional mips left off the top of textures by compression format, as Fallout
; 76's iTextureMipSkip* settings do: for example 1 for BC1 and BC3 diffuse maps
; while BC5 normal maps keep their detail. Only textures whose longer edge is
; above MinDimension lose mips, and never below it: with 1024, a 4096 texture
; and skip 1 becomes 2048, a 1024 texture stays. BC3 includes BC2; Other is
; BC6H and uncompressed. Combined with [MaxSize], the smaller size wins. Same
; limits as [MaxSize]. 0 to 3.
BC1=0
BC3=0
BC4=0
BC5=0
BC7=0
Other=0
; 1 to 16384.
MinDimension=1024

[Experimental]
; Paths under separate engine integration and correctness validation.
; Leave off unless intentionally testing a backend that implements them.
GpuFeedback=0
MeshStreaming=0
)";

        void WriteDefault(const char* a_path)
        {
            std::error_code ec;
            if (std::filesystem::exists(a_path, ec) || ec) {
                return;
            }

            std::ofstream file(a_path, std::ios::binary);
            if (file) {
                file << kDefaultFile;
                spdlog::info("Wrote the default {}", a_path);
            }
        }

        std::uint32_t ReadClamped(const CSimpleIniA& a_ini, const char* a_section, const char* a_key,
            std::uint32_t a_default, std::uint32_t a_min, std::uint32_t a_max)
        {
            const auto value = a_ini.GetLongValue(a_section, a_key, static_cast<long>(a_default));
            const auto clamped = std::clamp<long>(value, a_min, a_max);
            if (clamped != value) {
                spdlog::warn("[{}] {}={} is outside {} to {}, using {}", a_section, a_key, value, a_min, a_max, clamped);
            }
            return static_cast<std::uint32_t>(clamped);
        }

        void ApplyLogLevel(std::uint32_t a_level)
        {
            static constexpr spdlog::level::level_enum kLevels[] = {
                spdlog::level::trace, spdlog::level::debug, spdlog::level::info,
                spdlog::level::warn, spdlog::level::err, spdlog::level::critical
            };
            const auto level = kLevels[a_level < 6 ? a_level : 2];

            spdlog::set_level(level);
            // Trace and debug lines are only worth having if they survive a crash.
            // Otherwise only warnings flush at once: info lines can come from
            // the render thread, and a flush there is a disk write per line.
            spdlog::default_logger()->flush_on(level <= spdlog::level::debug ? level : spdlog::level::warn);
            spdlog::flush_every(std::chrono::seconds(2));
        }

        // Starfield's own values for everything that departs from it.
        void ApplyStarfieldExact(Settings& a_settings)
        {
            a_settings.unseen = 2;
            a_settings.pressureOnly = false;
            a_settings.unseenDelayMs = 0;
            a_settings.degradeCooldownMs = 0;
            a_settings.hysteresisPercent = 0;
            a_settings.roomCycleMs = 0;
            a_settings.raiseCooldownMs = 0;
            a_settings.targetMB = 512;
            a_settings.overDebounceFrames = 1;
            a_settings.creditPendingFrees = false;
            a_settings.adaptiveSampling = false;
            a_settings.refillGapMB = 0;
            a_settings.refillCalmMs = 0;
            a_settings.processCounters = false;
            a_settings.overflowSharedMB = 0;
            a_settings.autoTarget = false;
            a_settings.steerUpgradeLimit = false;
            a_settings.maxSize = {};
            a_settings.noLowerRadius = 0;
            a_settings.upgradeHoldSpeed = 0;
            a_settings.minLoweringKB = 0;
            a_settings.maxChangesPerSecond = 0;
        }
    }

    void LoadSettings(const char* a_path)
    {
        WriteDefault(a_path);

        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(a_path) < 0) {
            spdlog::warn("Couldn't read {}, using the defaults", a_path);
            ApplyLogLevel(g_settings.logLevel);
            return;
        }

        auto& s = g_settings;
        s.enabled = ini.GetBoolValue("General", "Enabled", true);
        s.logLevel = ReadClamped(ini, "General", "LogLevel", 2, 0, 5);
        ApplyLogLevel(s.logLevel);

        s.policy = ReadClamped(ini, "LOD", "Policy", 1, 0, 1);
        s.screenSize = ini.GetBoolValue("LOD", "ScreenSize", true);
        s.safetyMargin = ReadClamped(ini, "LOD", "SafetyMargin", 1, 0, 3);
        s.nearDistance = ReadClamped(ini, "LOD", "NearDistance", 875, 0, 100000);
        s.unseen = ReadClamped(ini, "LOD", "Unseen", 3, 0, 3);
        s.pressureOnly = ini.GetBoolValue("LOD", "PressureOnly", true);
        s.starfieldExact = ini.GetBoolValue("LOD", "StarfieldExact", false);
        s.noLowerRadius = ReadClamped(ini, "LOD", "NoLowerRadius", 0, 0, 100000);

        s.unseenDelayMs = ReadClamped(ini, "Smoothing", "UnseenDelayMs", 3000, 0, 60000);
        s.degradeCooldownMs = ReadClamped(ini, "Smoothing", "DegradeCooldownMs", 3000, 0, 60000);
        s.hysteresisPercent = ReadClamped(ini, "Smoothing", "HysteresisPercent", 20, 0, 50);
        s.roomCycleMs = ReadClamped(ini, "Smoothing", "RoomCycleMs", 250, 0, 2000);
        s.raiseCooldownMs = ReadClamped(ini, "Smoothing", "RaiseCooldownMs", 30000, 0, 600000);
        s.upgradeHoldSpeed = ReadClamped(ini, "Smoothing", "UpgradeHoldSpeed", 600, 0, 100000);
        s.minLoweringKB = ReadClamped(ini, "Smoothing", "MinLoweringKB", 0, 0, 65536);
        s.maxChangesPerSecond = ReadClamped(ini, "Smoothing", "MaxChangesPerSecond", 0, 0, 100000);

        s.budget = ini.GetBoolValue("Budget", "Enabled", true);
        s.reserveMB = ReadClamped(ini, "Budget", "Reserve", 128, 0, 4096);
        s.targetMB = ReadClamped(ini, "Budget", "Target", 896, 128, 8192);
        s.upgradeCeiling = ReadClamped(ini, "Budget", "UpgradeCeiling", 65, 0, 100);
        s.overDebounceFrames = ReadClamped(ini, "Budget", "OverDebounceFrames", 3, 1, 30);
        s.creditPendingFrees = ini.GetBoolValue("Budget", "CreditPendingFrees", true);
        s.adaptiveSampling = ini.GetBoolValue("Budget", "AdaptiveSampling", true);
        s.refillGapMB = ReadClamped(ini, "Budget", "RefillGap", 1024, 0, 8192);
        s.refillCalmMs = ReadClamped(ini, "Budget", "RefillCalmMs", 30000, 0, 600000);
        s.processCounters = ini.GetBoolValue("Budget", "ProcessCounters", true);
        s.overflowSharedMB = ReadClamped(ini, "Budget", "OverflowSharedMB", 256, 0, 8192);
        s.autoTarget = ini.GetBoolValue("Budget", "AutoTarget", true);
        s.steerUpgradeLimit = ini.GetBoolValue("Budget", "SteerUpgradeLimit", true);
        s.dryRun = ini.GetBoolValue("Budget", "DryRun", false);
        s.timeline = ini.GetBoolValue("Budget", "LogTimeline", false);

        s.maxSize = {};
        for (std::size_t i = 0; i < cap::kKinds; ++i)
            s.maxSize.edge[i] = ReadClamped(ini, "MaxSize", cap::kKindNames[i].data(), 0, 0, 16384);
        CSimpleIniA::TNamesDepend folderKeys;
        ini.GetAllKeys("MaxSizeFolders", folderKeys);
        folderKeys.sort(CSimpleIniA::Entry::LoadOrder());
        for (const auto& key : folderKeys) {
            const auto edge = ReadClamped(ini, "MaxSizeFolders", key.pItem, 0, 0, 16384);
            if (auto rule = cap::ParseRule(key.pItem, edge)) s.maxSize.rules.push_back(std::move(*rule));
            else spdlog::warn("[MaxSizeFolders] {} is not a rule (unknown kind or no path), ignored", key.pItem);
        }
        for (std::size_t i = 0; i < cap::kFormats; ++i)
            s.maxSize.skip.levels[i] = ReadClamped(ini, "MipSkip", cap::kFormatNames[i].data(), 0, 0, 3);
        s.maxSize.skip.minDimension = ReadClamped(ini, "MipSkip", "MinDimension", 1024, 1, 16384);

        static constexpr const char* thresholdKeys[]{"TextureDegradeScreenSize0","TextureDegradeScreenSize1",
            "TextureDegradeScreenSize2","TextureDegradeScreenSize3"};
        for (std::size_t i=0;i<s.sourceTextureThresholds.size();++i)
            s.sourceTextureThresholds[i] = static_cast<float>(ini.GetDoubleValue("Source",thresholdKeys[i],kStarfieldTextureThresholds[i]));
        if (!ValidStarfieldThresholds(s.sourceTextureThresholds) || !(s.sourceTextureThresholds[3] > 0.0f)) {
            // A zero threshold would map unseen textures (rank 0) to a level.
            spdlog::warn("[Source] Texture thresholds must be finite, positive and descending; using source defaults");
            s.sourceTextureThresholds = kStarfieldTextureThresholds;
        }
        static constexpr const char* meshKeys[]{"MeshLODScreenSize0","MeshLODScreenSize1",
            "MeshLODScreenSize2","MeshLODScreenSize3"};
        for (std::size_t i=0;i<s.sourceMeshScreenSizes.size();++i)
            s.sourceMeshScreenSizes[i] = static_cast<float>(ini.GetDoubleValue("Source",meshKeys[i],kStarfieldMeshScreenSizes[i]));
        s.sourceMeshFadePercent = static_cast<float>(ini.GetDoubleValue("Source","MeshLODFadeScreenSizePercent",0.2));
        s.sourceMeshStreamingBias = static_cast<float>(ini.GetDoubleValue("Source","MeshLODScreenSizePercentStreamingBias",0.05));
        if (!ValidStarfieldThresholds(s.sourceMeshScreenSizes) || !std::isfinite(s.sourceMeshFadePercent) ||
            !std::isfinite(s.sourceMeshStreamingBias) || s.sourceMeshFadePercent < 0 || s.sourceMeshStreamingBias < 0 ||
            s.sourceMeshFadePercent + s.sourceMeshStreamingBias > 1 ||
            !ValidStarfieldThresholds(StarfieldMeshThresholds(s.sourceMeshScreenSizes,s.sourceMeshFadePercent,s.sourceMeshStreamingBias))) {
            spdlog::warn("[Source] Mesh sizes/fade/bias are invalid; using the complete source default tuple");
            s.sourceMeshScreenSizes = kStarfieldMeshScreenSizes;
            s.sourceMeshFadePercent = 0.2f;
            s.sourceMeshStreamingBias = 0.05f;
        }
        // Zero would stop all lowering, or leave swap mode on for good.
        s.sourceMaximumIncreaseMB = ReadClamped(ini,"Source","MaximumIncreasePerIterationMB",34,1,4096);
        s.sourceMinimumDecreaseMB = ReadClamped(ini,"Source","MinimumDecreasePerIterationMB",34,1,4096);
        s.sourceNoMeshesUpgradeLimits = ini.GetBoolValue("Source","NoMeshesUpgradeLimits",true);
        const auto slice = ini.GetDoubleValue("Source","TimeSliceMilliseconds",10.0);
        s.sourceTimeSliceMs = std::isfinite(slice) ? std::clamp(slice,0.0,100.0) : 10.0;
        if (s.sourceTimeSliceMs != slice)
            spdlog::warn("[Source] TimeSliceMilliseconds={} is invalid/outside0..100; using {}",slice,s.sourceTimeSliceMs);
        s.gpuFeedback = ini.GetBoolValue("Experimental","GpuFeedback",false);
        s.meshStreaming = ini.GetBoolValue("Experimental","MeshStreaming",false);
        s.passthrough = ReadClamped(ini, "Experimental", "Passthrough", 0, 0, 2);
        s.legacyPending = ini.GetBoolValue("Experimental", "LegacyPending", false);
        s.syntheticPressureMB = ReadClamped(ini, "Experimental", "SyntheticPressureMB", 0, 0, 8192);
        s.syntheticPressureFromS = ReadClamped(ini, "Experimental", "SyntheticPressureFromS", 10, 0, 600);
        s.syntheticPressureForS = ReadClamped(ini, "Experimental", "SyntheticPressureForS", 30, 1, 600);
        s.minRoundMs = ReadClamped(ini, "Experimental", "MinRoundMs", 0, 0, 2000);
        // Hooks only: no planner, no video memory sampling, no counter thread.
        if (s.passthrough == 2) s.processCounters = false;
        // Test arms that give the game its own answers change nothing else.
        if (s.passthrough) {
            s.steerUpgradeLimit = false;
            s.maxSize = {};
        }
        if (s.starfieldExact) ApplyStarfieldExact(s);

        spdlog::info("Enabled={} Policy={} ScreenSize={} SafetyMargin={} NearDistance={} Unseen={} Budget={} Reserve={} MB "
                     "Target={} MB UpgradeCeiling={}%{}",
            s.enabled, s.policy, s.screenSize, s.safetyMargin, s.nearDistance, s.unseen, s.budget, s.reserveMB, s.targetMB,
            s.upgradeCeiling, s.dryRun ? " (dry run)" : "");
        spdlog::info("StarfieldExact={} QualityGuard={} UnseenDelay={} ms DegradeCooldown={} ms Hysteresis={}% "
                     "RoomCycle={} ms OverDebounce={} frames CreditPendingFrees={} AdaptiveSampling={}",
            s.starfieldExact, s.QualityGuard(), s.unseenDelayMs, s.degradeCooldownMs, s.hysteresisPercent,
            s.roomCycleMs, s.overDebounceFrames, s.creditPendingFrees, s.adaptiveSampling);
        spdlog::info("PressureOnly={} RaiseCooldown={} ms RefillGap={} MB RefillCalm={} ms ProcessCounters={} "
                     "OverflowShared={} MB AutoTarget={} SteerUpgradeLimit={} UpgradeHoldSpeed={} NoLowerRadius={} "
                     "MinLowering={} KB MaxChangesPerSecond={}",
            s.pressureOnly, s.raiseCooldownMs, s.refillGapMB, s.refillCalmMs, s.processCounters, s.overflowSharedMB,
            s.autoTarget, s.steerUpgradeLimit, s.upgradeHoldSpeed, s.noLowerRadius, s.minLoweringKB,
            s.maxChangesPerSecond);
        if (s.maxSize.Any()) {
            const auto& skip = s.maxSize.skip.levels;
            spdlog::info("MaxSize Diffuse={} Normal={} Specular={} Glow={} Other={}, {} folder rules; MipSkip BC1={} "
                         "BC3={} BC4={} BC5={} BC7={} Other={} above {}: textures keep less detail than the game's own "
                         "where these apply", s.maxSize.edge[0], s.maxSize.edge[1], s.maxSize.edge[2], s.maxSize.edge[3],
                s.maxSize.edge[4], s.maxSize.rules.size(), skip[0], skip[1], skip[2], skip[3], skip[4], skip[5],
                s.maxSize.skip.minDimension);
        }
        spdlog::info("Source texture ranks={}/{}/{}/{} Increase/Decrease={}/{} MiB NoMeshesUpgradeLimits={} "
            "TimeSlice={} ms (port unit); Experimental GpuFeedback={} MeshStreaming={}",
            s.sourceTextureThresholds[0],s.sourceTextureThresholds[1],s.sourceTextureThresholds[2],s.sourceTextureThresholds[3],
            s.sourceMaximumIncreaseMB,s.sourceMinimumDecreaseMB,s.sourceNoMeshesUpgradeLimits,s.sourceTimeSliceMs,
            s.gpuFeedback,s.meshStreaming);
        const auto meshThresholds = StarfieldMeshThresholds(s.sourceMeshScreenSizes,s.sourceMeshFadePercent,s.sourceMeshStreamingBias);
        spdlog::info("Source mesh ranks={}/{}/{}/{} (base sizes times 1-fade-bias); DB bias fixed at source default 0",
            meshThresholds[0],meshThresholds[1],meshThresholds[2],meshThresholds[3]);
        spdlog::info("Experimental Passthrough={} LegacyPending={} SyntheticPressureMB={} SyntheticPressureFromS={} "
            "SyntheticPressureForS={} MinRoundMs={}", s.passthrough, s.legacyPending, s.syntheticPressureMB,
            s.syntheticPressureFromS, s.syntheticPressureForS, s.minRoundMs);
    }

    const Settings& GetSettings() { return g_settings; }

    const char* DefaultSettingsFile() { return kDefaultFile.data(); }
}
