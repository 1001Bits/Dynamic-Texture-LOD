#pragma once

#include <cstdint>
#include "StarfieldPolicy.h"
#include "TextureCap.h"

// The plugin's ini, the same in every game. Read once, before any hook is
// installed, and never changed afterwards, so every thread reads it without
// locking.
namespace dtl
{
    struct Settings
    {
        bool          enabled = true;
        std::uint32_t logLevel = 2;  // 0 trace .. 5 critical

        // [LOD] Texture detail follows each object's size on screen.
        std::uint32_t policy = 1;          // 0 legacy pixels, 1 Starfield angular policy
        bool          screenSize = true;
        std::uint32_t safetyMargin = 1;    // mips kept above what the screen can show
        std::uint32_t nearDistance = 875;  // game units; full detail and first in line within it
        std::uint32_t unseen = 3;          // dtl::Unseen; 3 plans them by distance
        // Lower nothing while video memory has room to spare (Skyrim
        // Performance Solution's budget mode); lower under pressure only.
        bool          pressureOnly = true;
        // Starfield 1.16.236 exactly, for comparison tests: no quality guard
        // and none of the spacing below. Off by default.
        bool          starfieldExact = false;
        // Fallout 76's no-degrade radius (fPRTNoDegradeRadius, 100 there):
        // textures of objects this close to the camera keep the game's own
        // level, even in an emergency and under [MaxSize]. Game units; 0 off.
        std::uint32_t noLowerRadius = 0;

        // [Smoothing] These engines re-create a texture for every level change
        // and re-read its file to raise it again, so changes are spaced out.
        std::uint32_t unseenDelayMs = 3000;      // out of view this long before it counts as unseen
        std::uint32_t degradeCooldownMs = 3000;  // no lowering this soon after a change, unless over budget
        std::uint32_t hysteresisPercent = 20;    // a lowering needs the rank this far under its threshold
        std::uint32_t roomCycleMs = 250;         // with room to spare, plan at most this often
        std::uint32_t raiseCooldownMs = 30000;   // no lowering this soon after a raise, unless over budget
        // No raises while the camera moves faster than this, in game units
        // per second, as the game's fTextureUpgradeDisabledMinSpeed (600) does
        // for its own; lowering still happens. 0 is off.
        std::uint32_t upgradeHoldSpeed = 600;
        // Each change is a re-create on the game's one creation thread, which
        // the frame waits on. Both 0 (off) by default.
        std::uint32_t minLoweringKB = 0;        // lowerings that free less are skipped
        std::uint32_t maxChangesPerSecond = 0;  // accepted level changes per second, at most

        // [Budget] Starfield's lines under the OS video memory budget.
        bool          budget = true;
        std::uint32_t reserveMB = 128;
        std::uint32_t targetMB = 896;       // with the reserve, 1 GB under the budget (SPS's reserve)
        std::uint32_t upgradeCeiling = 65;  // percent
        std::uint32_t overDebounceFrames = 3;  // frames over the line before an emergency
        bool          creditPendingFrees = true;
        bool          adaptiveSampling = true;   // read video memory less often with lots of headroom
        std::uint32_t refillGapMB = 1024;      // refill starts this far under the target
        std::uint32_t refillCalmMs = 30000;    // and this long after pressure or a load
        bool          processCounters = true;  // also read this process's GPU memory counters
        std::uint32_t overflowSharedMB = 256;  // paged-out memory that, while rising, means over budget
        // After 3 minutes of play, keep the largest jump seen free, if that is
        // more than Reserve plus Target (SPS's reserve recommendation).
        bool          autoTarget = true;
        // Fallout 4: tight or over, the game's own streamer may raise textures
        // only into the room left under the budget line.
        bool          steerUpgradeLimit = true;
        bool          dryRun = false;
        bool          timeline = false;

        // [MaxSize] [MaxSizeFolders] Optional largest sizes, as Skyrim's
        // Texture Downscaler sets them, and [MipSkip], Fallout 76's mips left
        // off by format. All off by default: they keep less detail than the
        // game would.
        cap::Limits   maxSize;

        // [Source] Verified Starfield controls, shared by game adapters.
        std::array<float,4> sourceTextureThresholds = kStarfieldTextureThresholds;
        std::array<float,4> sourceMeshScreenSizes = kStarfieldMeshScreenSizes;
        float         sourceMeshFadePercent = 0.2f;
        float         sourceMeshStreamingBias = 0.05f;
        std::uint32_t sourceMaximumIncreaseMB = 34;
        std::uint32_t sourceMinimumDecreaseMB = 34;
        bool          sourceNoMeshesUpgradeLimits = true;
        double        sourceTimeSliceMs = 10.0; // source unit inferred, explicit port unit

        // [Experimental] Unsupported paths remain opt-in during validation.
        bool          gpuFeedback = false;
        bool          meshStreaming = false;
        // Test arms only, absent from the shipped file. Passthrough 1: all of
        // this plugin's work, the game's own answers; 2: hooks forward only.
        std::uint32_t passthrough = 0;
        // Owner batch flag counts as pending even when the queued level is
        // the current one (the earlier rule, for comparison).
        bool          legacyPending = false;
        // Pretends this much over the line, once, for a while after the
        // world first renders; 0 is off.
        std::uint32_t syntheticPressureMB = 0;
        std::uint32_t syntheticPressureFromS = 10;
        std::uint32_t syntheticPressureForS = 30;
        // At most one native texture round per this many ms; 0 is off.
        std::uint32_t minRoundMs = 0;

        // Never more detail than the game's own, never a visible texture
        // lowered past what the screen can show, outside an emergency.
        bool QualityGuard() const { return !starfieldExact; }
    };

    // Writes the default file at a_path if there is none, reads it, and
    // applies the log level to spdlog's default logger.
    void LoadSettings(const char* a_path);

    const Settings& GetSettings();

    // The default file, as the plugin writes it and as it ships.
    const char* DefaultSettingsFile();
}
