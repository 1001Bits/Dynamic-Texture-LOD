#include "Lod.h"

#include "Addresses.h"
#include "FaultReport.h"
#include "FramePump.h"
#include "TextureLifecycle.h"
#include "RequestDemand.h"
#include "NativeAnswer.h"
#include "ChangeRate.h"
#include "NativeLimits.h"
#include "TextureFeedback.h"
#include "GpuGather.h"
#include "Fo4Mesh.h"
#include "Fo4MeshLifetimeDiagnostics.h"
#include "NativeTextureLevels.h"
#include "PluginIni.h"
#include "Vram.h"
#include "core/BudgetMode.h"
#include "core/Frustum.h"
#include "core/LodPolicy.h"
#include "core/Observation.h"
#include "core/Planner.h"
#include "core/StarfieldPlanner.h"
#include "core/StarfieldPolicy.h"
#include "core/StreamingLedger.h"
#include "core/Settings.h"
#include "core/SpeedHold.h"
#include "core/TextureFormat.h"

#include <cctype>
#include <cstring>
#include <condition_variable>
#include <format>
#include <string>
#include <typeinfo>

namespace
{
    // GetTextureMaxDegradeLevel's answer on every runtime.
    constexpr std::uint32_t kEngineMaxLevel = 3;
    constexpr std::uint32_t kSourceMaxLevel = 4;
    std::uint32_t g_sourceMaxLevel = kEngineMaxLevel;

    constexpr float kDefaultTanHalfFovY = 0.7f;  // about 70 degrees
    constexpr float kDefaultScreenHeight = 1080.0f;

    // ── Tuning ───────────────────────────────────────────────────────────────

    constexpr auto kPlanEvery = std::chrono::milliseconds(100);

    // The engine may take arbitrarily long to finish a texture traversal.
    // Keep observations from the active and last completed round.
    std::atomic<std::uint64_t> g_round{ 1 };

    std::atomic<bool>         g_installed{ false };
    bool g_meshObservations{}, g_meshInstalled{};
    // Read on every visit; fixed at install from the settings.
    bool g_qualityGuard = false, g_timeline = false, g_exact = false;
    float g_nearDistance = 0.0f;
    // Wider than the view, so the edge of a turn is already counted as seen.
    constexpr float kGuardBand = 1.25f;
    std::atomic<std::uint8_t> g_mode{ static_cast<std::uint8_t>(dtl::Mode::Unlimited) };

    std::atomic<std::uint64_t> g_objectsMeasured{ 0 };
    std::atomic<std::uint64_t> g_texturesAnswered{ 0 };
    // Opt-in diagnostics: transitions between native visits, not a count of
    // every engine streaming operation or proof of plugin-attributed savings.
    std::atomic<std::uint64_t> g_observedLowerings{ 0 };
    std::atomic<std::uint64_t> g_observedRestores{ 0 };
    std::atomic<std::uint64_t> g_frames{ 0 }, g_cycles{ 0 }, g_requests{ 0 };
    std::atomic<std::uint64_t> g_completedRequests{ 0 }, g_cancelledRequests{ 0 };
    std::atomic<std::uint64_t> g_peakPlanMicroseconds{ 0 };
    std::atomic<std::uint64_t> g_births{ 0 }, g_retirements{ 0 };
    std::atomic<std::uint64_t> g_staleDemand{ 0 };
    // Timeline only: the last finished planning cycle.
    std::atomic<std::uint32_t> g_planHeld{ 0 }, g_planAccepted{ 0 }, g_planStale{ 0 };
    std::atomic<std::uint64_t> g_inheritedObservations{ 0 }, g_scopedVisitors{ 0 };
    // Test arms ([Experimental]), fixed at install: 1 does all of this
    // plugin's work but gives the game its own answers, 2 only forwards.
    // A planner failure raises 0 to 1 (FailSafe).
    std::atomic<std::uint32_t> g_passthrough{ 0 };
    std::uint32_t Passthrough() { return g_passthrough.load(std::memory_order_relaxed); }
    // Lod::SetActive: the passthrough configured at load, whether the fail-safe ran (never undone), and a
    // switch-off whose upgrade limit OnFrame still has to hand back.
    std::uint32_t g_configuredPassthrough = 0;
    std::atomic<bool> g_failedSafe{ false }, g_releaseLimit{ false };

    // Pressure first with room to spare: what this plugin did not lower takes
    // the game's own level, as without the plugin.
    bool FollowsGame(const dtl::Settings& a_settings)
    {
        return a_settings.policy == static_cast<std::uint32_t>(dtl::DetailPolicy::Starfield) && a_settings.pressureOnly &&
            g_mode.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(dtl::Mode::Room);
    }
    bool g_legacyPending = false;
    // Always counted, logged every few seconds: native texture rounds and
    // textures newly put in a native batch.
    std::atomic<std::uint64_t> g_batchAdds{ 0 };
    // When the first object was measured: the world began rendering.
    std::atomic<std::int64_t> g_worldStartMs{ 0 };
    // Coverage, timeline only: each texture the native pass offers, counted
    // once a round by what becomes of it, with its resident bytes. Offered
    // ones reach this plugin's answer; fixed ones the engine keeps at full
    // detail (no allow-degrade flag); unmeasured ones come from passes with
    // no object measurement (decals, detached). Two banks by round parity:
    // the log shows the last finished round.
    enum Coverage : std::uint8_t { kOffered, kNativeFixed, kUnmeasured, kNoRenderer, kCoverageClasses };
    std::array<std::array<std::atomic<std::uint64_t>, kCoverageClasses>, 2> g_coverageTextures{}, g_coverageBytes{};
    const volatile std::uint64_t* g_nativeTextureBytes = nullptr;
    volatile std::uint64_t* g_nativeUpgradeLimit = nullptr;
    // [Budget] SteerUpgradeLimit, SteerToTarget (FO4 only) and [MaxSize]
    // [MipSkip], fixed at install.
    bool g_steerUpgradeLimit = false, g_steerToTarget = false, g_caps = false;
    std::atomic<std::uint64_t> g_upgradeLimitWrites{ 0 };
    // [LOD] NoLowerRadius, fixed at install (0 when off), and the object
    // visits it gave to the game.
    std::uint32_t g_noLowerRadius = 0;
    // [Smoothing] MaxChangesPerSecond, configured at install. Planner thread only.
    ChangeRate g_changeRate;
    std::atomic<std::uint64_t> g_nearVisits{ 0 };
    // [Smoothing] UpgradeHoldSpeed: the camera moves too fast for raises.
    // Written by the planner thread, read when requests are accepted.
    std::atomic<bool> g_upgradeHold{ false };
    std::atomic<std::uint64_t> g_upgradeHolds{ 0 };
    std::unordered_map<std::uint64_t, RequestDemand::Snapshot> g_cycleDemand;

    std::uintptr_t g_screenHeight = 0;  // BSGraphics::State::screenHeight
    using WorldRootCamera_t = RE::NiCamera* (*)();
    WorldRootCamera_t g_worldRootCamera = nullptr;
    dtl::StreamingLedger g_ledger;
    dtl::StarfieldPlanSession g_session;
    std::mutex g_jobLock;
    std::condition_variable g_jobWake, g_jobDone;
    std::uint64_t g_pendingFrame = 0, g_completedFrame = 0;

    // ── Engine layout ────────────────────────────────────────────────────────
    // The same on OG, NG, AE and VR.

    // How the engine describes a texture file.
    struct TextureHeader
    {
        std::uint16_t height;    // 0
        std::uint16_t width;     // 2
        std::uint8_t  mipCount;  // 4
        std::uint8_t  format;    // 5 DXGI_FORMAT
        std::uint8_t  flags;     // 6 bit 0: cubemap
        std::uint8_t  pad7;      // 7
    };
    static_assert(sizeof(TextureHeader) == 0x8);

    constexpr std::uint8_t kHeaderCubemap = 1u << 0;

    // BSGraphics::Texture, only the parts used here.
    struct EngineTexture
    {
        void*         shaderResourceView;  // 00
        void*         resource;            // 08
        std::byte     pad10[0x18];         // 10
        TextureHeader header;              // 28
        std::uint32_t pendingRequests;    // 30 native resource-creator requests
        std::uint32_t refCount;           // 34
        std::byte     pad38[5];           // 38
        std::uint8_t  degradeLevel;        // 3D mips missing from the top now
        std::byte     pad3E;
        std::uint8_t  stateFlags;          // 3F bit0 ready, bit1 deferred operation
    };
    static_assert(offsetof(EngineTexture, header) == 0x28);
    static_assert(offsetof(EngineTexture, degradeLevel) == 0x3D);
    static_assert(offsetof(EngineTexture, stateFlags) == 0x3F);

    // ── Notes, per thread ────────────────────────────────────────────────────

    // The paired per-object calls write and consume this note synchronously.
    // The texture visitor clears it on exit, including the always-rendered
    // pass. Fixed-level passes cannot inherit another object's observation.

    struct Note
    {
        float         pixels = 0.0f;        // the object's size on screen
        float         angular = 0.0f;       // sphere proxy until AABB gather exists
        float         nearest = 0.0f;       // camera to its bounding sphere
        float         screenHeight = 0.0f;
        std::uint32_t vanilla = 0;          // the engine's own level for it
        bool          visible = false;      // in the view frustum
        bool          fresh = false;
        // The visitor's level came from Hook_ObjectLevel's answer, not the
        // engine's: the engine's own is in vanilla.
        bool          direct = false;
        // Within [LOD] NoLowerRadius: the game's own answers, as passthrough.
        bool          near = false;
    };

    thread_local Note t_note;
    // The game's own level for the texture being visited: the plugin never
    // answers with more detail. Set by the functor around each native visit.
    thread_local std::uint32_t t_floor = 0;
    // [MaxSize]: the least detail a size cap leaves the texture being visited,
    // as a level; 0 when none applies. Set by the functor like t_floor.
    thread_local std::uint32_t t_cap = 0;
    // Timeline only: the native visitor's own fields for that visit, before
    // the scoped copy replaced them. A direct visitor's base is this
    // plugin's answer; the floor used the object's own level instead.
    struct VisitDetail
    {
        std::uint8_t base = 0, bias = 0, suppress = 0;
        bool         direct = false;
    };
    thread_local VisitDetail t_visit;

    // The UV tiling of the material whose textures are being visited.
    thread_local float t_uvScale = 1.0f;
    thread_local float t_sourceUv = 1.0f;
    thread_local RE::NiTexture* t_textureOwner = nullptr;
    thread_local RE::NiAVObject* t_node = nullptr;

    // ── View ─────────────────────────────────────────────────────────────────

    struct View
    {
        RE::NiPoint3 eye;
        RE::NiPoint3 forward;
        RE::NiPoint3 up;
        RE::NiPoint3 right;
        RE::NiFrustum frustum;
        dtl::View    view;
    };

    RE::NiPoint3 CameraAxis(const RE::NiMatrix3& a_matrix, std::size_t a_axis)
    {
        // FO4 stores each basis vector contiguously in a padded NiPoint4.
        // NiCamera::GetWorldDirection reads rotate+00/+04/+08, up +10,
        // right +20. This differs from Skyrim's matrix layout.
        const auto& axis = a_matrix.entry[a_axis];
        return { axis.x, axis.y, axis.z };
    }

    float Dot(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right)
    {
        return a_left.x * a_right.x + a_left.y * a_right.y + a_left.z * a_right.z;
    }

    // Read on whatever thread asks, like the engine's own WorldCenter: a value
    // a frame old only moves a borderline texture by one level.
    bool ReadView(View& a_out)
    {
        auto* camera = g_worldRootCamera ? g_worldRootCamera() : nullptr;
        if (!camera) {
            return false;
        }

        // NetImmerse cameras look down their local X axis, with Y up and Z
        // to the right.
        a_out.eye = camera->world.translate;
        a_out.forward = CameraAxis(camera->world.rotate, 0);
        a_out.up = CameraAxis(camera->world.rotate, 1);
        a_out.right = CameraAxis(camera->world.rotate, 2);
        a_out.frustum = camera->viewFrustum;

        const float halfHeight = (a_out.frustum.top - a_out.frustum.bottom) * 0.5f;
        a_out.view.tanHalfFovY = halfHeight > 0.1f && halfHeight < 3.0f ? halfHeight : kDefaultTanHalfFovY;

        const auto height = g_screenHeight ? *reinterpret_cast<const std::uint32_t*>(g_screenHeight) : 0u;
        a_out.view.screenHeight = height >= 240 && height <= 16384 ? static_cast<float>(height) : kDefaultScreenHeight;
        return true;
    }

    // A bounding sphere against the four sides of the view, and behind it.
    // The frustum's sides are slopes at unit distance.
    bool InView(const View& a_view, const RE::NiPoint3& a_center, float a_radius, float a_nearest)
    {
        // WorldRootCamera's stereo coverage is not established. Retain VR
        // textures as visible rather than reject one eye's edge of view.
        if (REL::Module::IsVR()) {
            return true;
        }
        // Whatever is close is one turn away from filling the screen.
        if (g_qualityGuard && g_nearDistance > 0.0f && a_nearest <= g_nearDistance) {
            return true;
        }

        const RE::NiPoint3 offset{ a_center.x - a_view.eye.x, a_center.y - a_view.eye.y, a_center.z - a_view.eye.z };
        const float        depth = Dot(offset, a_view.forward);
        const float across = Dot(offset, a_view.right);
        const float rise = Dot(offset, a_view.up);
        const float band = g_qualityGuard ? kGuardBand : 1.0f;
        return dtl::SphereInFrustum(depth, across, rise, a_radius, a_view.frustum.left * band,
            a_view.frustum.right * band, a_view.frustum.bottom * band, a_view.frustum.top * band, a_view.frustum.ortho);
    }

    // ── Tracking ─────────────────────────────────────────────────────────────

    constexpr std::uint8_t kNotPlanned = 0xFF;
    constexpr std::uint8_t kNoFloor = 0xFF;

    // One texture, as the update stage has seen it.
    struct Tracked
    {
        // Who it is: a texture freed and another made at the same address
        // starts over.
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint8_t  mips = 0;
        std::uint8_t  format = 0;
        std::uint32_t generation = 0;

        std::uint8_t lowest = kEngineMaxLevel;  // GetTextureMaxDegradeLevel
        std::uint8_t current = 0;               // its level at the last visit
        std::uint8_t cap = 0;                   // [MaxSize] level, never planned below
        std::uint8_t planned = kNotPlanned;
        // The accepted target keeps less detail than the game's own level:
        // this plugin's lowering, waiting for refill to bring it back.
        bool held = false;
        std::uint64_t resourceIdentity = 0;
        std::uint64_t requestRound = 0;
        dtl::StreamingLedger::Ticket ticket = 0;
        bool nativePending = false;
        bool nativeNoDegrade = false;
        // [LOD] NoLowerRadius: last visited that close, so the game decides.
        bool near = false;
        RequestDemand::Snapshot requestDemand;
        bool emergencyRequest = false;
        float gpuDemand = 0;
        std::uint64_t gpuFrame = 0;

        dtl::Observation observation;

        // The game's own level for it, the most detailed over each native
        // round and the one before: the plugin never plans more detail.
        std::uint8_t  floor[2]{ kNoFloor, kNoFloor };
        std::uint64_t floorRound = 0;
        // Planner thread only: when it was last seen, last changed level and
        // last came up a level.
        std::uint64_t lastSeenMs = 0, lastChangeMs = 0, lastRaiseMs = 0;
        std::uint8_t  snapshotLevel = kNotPlanned;

        // Timeline only: why it is where it is, from its last native visit
        // and the planner's last snapshot of it.
        struct Diagnosis
        {
            std::string   name;
            VisitDetail   visit;
            std::uint8_t  objectLevel = 0;  // the object's own level from the engine
            std::uint8_t  visitFloor = 0;
            std::uint8_t  answer = kNotPlanned;  // this plugin's answer to the visit
            std::uint8_t  ownerDesired = 0;
            std::uint8_t  stateFlags = 0;
            std::uint32_t ownerFlags = 0, nativeRequests = 0;
            bool          barrel = false;  // the validated scene's softened barrel, by name
            bool          snapshotted = false, pending = false;
            std::uint8_t  seenCap = kNotPlanned, minLevel = kNotPlanned, rankLevel = kNotPlanned;
            float         pixels = 0.0f, priority = 0.0f;
        };
        std::unique_ptr<Diagnosis> diagnosis;

        void RecordFloor(std::uint64_t a_round, std::uint8_t a_floor)
        {
            if (floorRound != a_round) {
                floor[1] = floorRound + 1 == a_round ? floor[0] : kNoFloor;
                floor[0] = a_floor;
                floorRound = a_round;
            } else {
                floor[0] = std::min(floor[0], a_floor);
            }
        }
        std::uint8_t Floor() const
        {
            const auto least = std::min(floor[0], floor[1]);
            return least == kNoFloor ? 0 : least;
        }
    };

    // [MaxSize] [MipSkip]: one NiTexture's cap, kept while its name and file match.
    struct CapMemo
    {
        const char*   name = nullptr;  // pooled BSFixedString data
        std::uint16_t width = 0, height = 0;
        std::uint8_t  mips = 0, format = 0, levels = 0;
        std::uint32_t generation = 0;  // g_capGeneration it was worked out for
    };

    // MCM "4/6 GB budget mode", live: every kind of texture at most 1024 pixels on its longer edge, all the time
    // (the measured below-cap1024 arm: much less video memory, smoother under pressure, visibly softer in places).
    // A smaller [MaxSize] in the INI still wins. Switching re-works every remembered cap.
    constexpr std::uint32_t kBudgetModeEdge = 1024;
    std::atomic<bool> g_budgetMode{ false };
    std::atomic<std::uint32_t> g_capGeneration{ 1 };

    const dtl::cap::Limits& BudgetModeLimits()
    {
        static const auto limits = [] {
            auto result = dtl::GetSettings().maxSize;
            for (auto& edge : result.edge) edge = edge ? std::min(edge, kBudgetModeEdge) : kBudgetModeEdge;
            return result;
        }();
        return limits;
    }
    constexpr std::size_t kCapMemosPerShard = 8192;

    // Shards, so the update stage and the planner rarely wait on each other.
    struct Shard
    {
        std::mutex                                  lock;
        std::unordered_map<const void*, Tracked>    textures;
        // Timeline only: the round each visited texture was last counted in.
        std::unordered_map<const void*, std::uint64_t> coverage;
        // [MaxSize] only, keyed by NiTexture.
        std::unordered_map<const void*, CapMemo>    caps;
    };

    std::array<Shard, 16> g_shards;
    std::atomic<std::uint32_t> g_generation{ 0 };

    Shard& ShardOf(const void* a_texture)
    {
        return g_shards[(reinterpret_cast<std::uintptr_t>(a_texture) >> 6) & (g_shards.size() - 1)];
    }

    bool FeedbackIdentity(const void* texture, std::uint64_t& generation)
    {
        auto& shard = ShardOf(texture);
        std::scoped_lock lock(shard.lock);
        const auto it = shard.textures.find(texture);
        if (it == shard.textures.end() || !it->second.resourceIdentity) return false;
        generation = it->second.generation;
        return true;
    }

    void FeedbackDemand(std::uint64_t key, std::uint64_t generation, float demand, std::uint64_t frame)
    {
        if (!std::isfinite(demand) || demand < 0) return;
        const auto* texture = reinterpret_cast<const void*>(key);
        auto& shard = ShardOf(texture);
        std::scoped_lock lock(shard.lock);
        const auto it = shard.textures.find(texture);
        if (it == shard.textures.end() || it->second.generation != generation || it->second.gpuFrame > frame) return;
        it->second.gpuDemand = demand;
        it->second.gpuFrame = frame;
    }

    void ForgetTexture(void* texture)
    {
        auto& shard = ShardOf(texture);
        std::scoped_lock lock(shard.lock);
        const auto it = shard.textures.find(texture);
        if (it == shard.textures.end()) return;
        if (it->second.ticket) g_ledger.CancelChange(it->second.ticket,
            { reinterpret_cast<std::uint64_t>(texture), it->second.generation });
        shard.textures.erase(it);
    }

    void OnTextureBirth(void* texture)
    {
        // The header has not been initialized yet. Remove stale ownership
        // without touching it; the next native visit gets a new generation.
        ForgetTexture(texture);
        g_births.fetch_add(1, std::memory_order_relaxed);
    }

    void OnTextureRetire(void* texture)
    {
        ForgetTexture(texture);
        g_retirements.fetch_add(1, std::memory_order_relaxed);
    }

    // [MaxSize] [MipSkip]: the level that brings this texture within its size
    // limits, never more than the game itself can take off it; 0 when none
    // applies. Matched by path once per texture, then remembered by name and file.
    std::uint32_t CapFor(RE::NiTexture* a_owner)
    {
        const bool budgetMode = g_budgetMode.load(std::memory_order_relaxed);
        if (!(g_caps || (budgetMode && Passthrough() == 0)) || !a_owner || !a_owner->rendererTexture) return 0;
        const auto& header = reinterpret_cast<const EngineTexture*>(a_owner->rendererTexture)->header;
        if ((header.flags & kHeaderCubemap) || header.width == 0 || header.height == 0 || header.mipCount <= 1 ||
            !TextureFormat::IsSupported(header.format)) return 0;
        const std::string_view name = a_owner->GetName();
        auto& shard = ShardOf(a_owner);
        std::scoped_lock lock(shard.lock);
        if (shard.caps.size() >= kCapMemosPerShard && !shard.caps.contains(a_owner)) shard.caps.clear();
        auto& memo = shard.caps[a_owner];
        const auto generation = g_capGeneration.load(std::memory_order_relaxed);
        if (memo.generation != generation || memo.name != name.data() || memo.width != header.width ||
            memo.height != header.height || memo.mips != header.mipCount || memo.format != header.format) {
            const auto levels = dtl::cap::CapLevels(budgetMode ? BudgetModeLimits() : dtl::GetSettings().maxSize,
                dtl::cap::Normalize(name), header.format, header.width, header.height);
            const auto most = NativeTextureLevels::Maximum(header.width, header.height, header.mipCount, kEngineMaxLevel,
                TextureFormat::IsBlockCompressed(header.format));
            memo = { name.data(), header.width, header.height, header.mipCount, header.format,
                static_cast<std::uint8_t>(std::min(levels, most)), generation };
        }
        return memo.levels;
    }

    std::uint32_t WantedNow(std::uint32_t a_size, float a_pixels, float a_priority, std::uint32_t a_lowest)
    {
        const auto& settings = dtl::GetSettings();
        if (!settings.screenSize) {
            return 0;
        }
        if (settings.policy == static_cast<std::uint32_t>(dtl::DetailPolicy::Starfield)) {
            return dtl::StarfieldWantedLevel(a_priority, static_cast<std::uint8_t>(a_lowest), settings.sourceTextureThresholds);
        }
        const dtl::Policy policy{ settings.safetyMargin, a_lowest };
        return dtl::WantedLevel(policy, a_size, a_pixels, a_priority >= 2.0f);
    }

    RequestDemand::Snapshot DemandNow(const Tracked& tracked, std::uint64_t round)
    {
        const bool byDistance = dtl::GetSettings().unseen == static_cast<std::uint32_t>(dtl::Unseen::ByDistance);
        return RequestDemand::Capture(tracked.observation, round, byDistance,
            [&tracked](float pixels, float priority) {
                return WantedNow(std::min(tracked.width, tracked.height), pixels, priority, tracked.lowest);
            });
    }

    // ── GetDesiredDegradeLevel ───────────────────────────────────────────────

    using GetDesiredDegradeLevel_t = std::uint32_t (*)(const RE::NiPoint3*, float);
    GetDesiredDegradeLevel_t g_getDesiredDegradeLevel = nullptr;

    // While the player is high-speed loading (fast travel), the engine answers
    // 2 for everything, whatever the distance, so loads finish sooner. Asked
    // about a sphere that reaches everything, it otherwise answers 0, which
    // tells the two apart without reading the player's flags.
    constexpr float kReachesEverything = 1.0e30f;

    bool EngineForcesLevel(const RE::NiPoint3* a_center)
    {
        return g_getDesiredDegradeLevel(a_center, kReachesEverything) != 0;
    }

    // The answer doesn't depend on the object, and fast travel lasts seconds:
    // asked once per native round instead of once per object.
    std::atomic<std::uint64_t> g_forcedRound{ 0 };
    std::atomic<bool>          g_forced{ false };
    bool EngineForcesLevelThisRound(const RE::NiPoint3* a_center)
    {
        const auto round = g_round.load(std::memory_order_relaxed);
        if (g_forcedRound.load(std::memory_order_acquire) == round) return g_forced.load(std::memory_order_relaxed);
        const bool forced = EngineForcesLevel(a_center);
        g_forced.store(forced, std::memory_order_relaxed);
        g_forcedRound.store(round, std::memory_order_release);
        // High-speed loading: video memory reads emptier than it is until
        // the scene has streamed in, so refill waits for calm after it.
        if (forced) dtl::budget::NoteLoad();
        return forced;
    }

    // Measures the object whose textures are about to be walked. Direct when
    // the engine called for its level and gets this plugin's answer back;
    // not when a room passed its own level down to it.
    std::uint32_t MeasureObject(const RE::NiPoint3* a_center, float a_radius, bool a_direct)
    {
        const auto vanilla = g_getDesiredDegradeLevel(a_center, a_radius);

        View view;
        if (!a_center || EngineForcesLevelThisRound(a_center) || !ReadView(view)) {
            t_note.fresh = false;
            return vanilla;
        }

        const float distance = a_center->GetDistance(view.eye);
        if (!(a_radius > 0.0f) || !std::isfinite(a_radius) || !std::isfinite(distance)) {
            t_note.fresh = false;
            return vanilla;
        }
        t_note.pixels = dtl::ProjectedPixels(view.view, { 0, a_radius, distance });
        t_note.angular = dtl::StarfieldAngularSize(a_radius, distance);
        t_note.nearest = dtl::NearestDistance(distance, a_radius);
        t_note.screenHeight = view.view.screenHeight;
        t_note.vanilla = vanilla;
        t_note.visible = InView(view, *a_center, a_radius, t_note.nearest);
        t_note.fresh = true;
        t_note.direct = a_direct;
        t_note.near = t_note.nearest < static_cast<float>(g_noLowerRadius);
        if (t_note.near) g_nearVisits.fetch_add(1, std::memory_order_relaxed);
        if (g_timeline) g_objectsMeasured.fetch_add(1, std::memory_order_relaxed);
        if (g_worldStartMs.load(std::memory_order_relaxed) == 0) {
            std::int64_t none = 0;
            g_worldStartMs.compare_exchange_strong(none, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        // The texture's own answer is the smaller one, so it decides.
        if (Passthrough() == 1 || t_note.near) return vanilla;
        return dtl::GetSettings().policy == 1 ? g_sourceMaxLevel : kEngineMaxLevel;
    }

    std::uint32_t Hook_ObjectLevel(const RE::NiPoint3* a_center, float a_radius)
    {
        if (Passthrough() == 2) return g_getDesiredDegradeLevel(a_center, a_radius);
        return MeasureObject(a_center, a_radius, true);
    }

    // Everything else: objects loading in, the bounds of a room, decals. Over
    // the budget line, what loads now comes in at the lowest level, and the
    // planner brings it back once there is room.
    std::uint32_t Hook_OtherLevel(const RE::NiPoint3* a_center, float a_radius)
    {
        t_note.fresh = false;
        const auto vanilla = g_getDesiredDegradeLevel(a_center, a_radius);
        if (Passthrough()) return vanilla;
        const bool over = !dtl::GetSettings().dryRun &&
            g_mode.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(dtl::Mode::Over);
        if (!over || EngineForcesLevel(a_center)) return vanilla;
        // [LOD] NoLowerRadius: what loads in right beside the camera keeps
        // the game's level even now.
        View view;
        if (g_noLowerRadius && a_center && ReadView(view) && std::isfinite(a_radius) &&
            dtl::NearestDistance(a_center->GetDistance(view.eye), a_radius) < static_cast<float>(g_noLowerRadius))
            return vanilla;
        return kEngineMaxLevel;
    }

    // ── ForEachTexture ───────────────────────────────────────────────────────

    using ForEachTexture_t = std::int32_t (*)(RE::BSShaderProperty*, void*);
    std::int32_t Hook_ForEachTexture(RE::BSShaderProperty* a_property, void* a_visitor)
    {
        if (Passthrough() == 2) {
            const auto table = *reinterpret_cast<const std::uintptr_t* const*>(a_property);
            return reinterpret_cast<ForEachTexture_t>(table[Addr::ForEachTextureSlot()])(a_property, a_visitor);
        }
        // Room traversal inherits a distance value and can skip the adjacent
        // GetDesired call for a child geometry. Gather that geometry directly
        // at its verified ForEach call instead of treating the visit as absent.
        if (!t_note.fresh && t_node && dtl::GetSettings().policy == 1) {
            MeasureObject(&t_node->worldBound.center, t_node->worldBound.fRadius, false);
            if (t_note.fresh && g_timeline) g_inheritedObservations.fetch_add(1, std::memory_order_relaxed);
        }
        const float outer = t_uvScale;
        const float outerSource = t_sourceUv;
        const auto* material = a_property ? a_property->material : nullptr;
        t_uvScale = 1.0f;
        t_sourceUv = 1.0f;
        if (material && REL::Module::IsVR()) {
            // FO4 VR has a single UV scale at +0x14 in a 0x28-byte base
            // material. CommonLibF4's desktop +0x1C field is the VR hash.
            const auto* uv = reinterpret_cast<const RE::NiPoint2*>(
                reinterpret_cast<const std::byte*>(material) + 0x14);
            t_uvScale = dtl::UvScale(uv->x, uv->y);
            t_sourceUv = std::min(uv->x, uv->y);
        } else if (material) {
            // Desktop alternates transform banks. This worker runs apart
            // from rendering, so keep the more detailed of the two banks.
            t_uvScale = std::min(dtl::UvScale(material->texCoordScale[0].x, material->texCoordScale[0].y),
                dtl::UvScale(material->texCoordScale[1].x, material->texCoordScale[1].y));
            t_sourceUv = std::min({ material->texCoordScale[0].x, material->texCoordScale[0].y,
                material->texCoordScale[1].x, material->texCoordScale[1].y });
        }
        const auto table = *reinterpret_cast<const std::uintptr_t* const*>(a_property);
        const auto original = reinterpret_cast<ForEachTexture_t>(table[Addr::ForEachTextureSlot()]);
        const auto result = original(a_property, a_visitor);
        t_uvScale = outer;
        t_sourceUv = outerSource;
        t_note.fresh = false;
        return result;
    }

    // ── GetTextureMaxDegradeLevel ────────────────────────────────────────────

    using GetTextureMaxDegradeLevel_t = std::uint32_t (*)(void*, const void*);
    GetTextureMaxDegradeLevel_t g_getTextureMaxDegradeLevel = nullptr;

    std::uint32_t Hook_TextureLevel(void* a_renderer, const void* a_texture)
    {
        const auto engineMax = g_getTextureMaxDegradeLevel(a_renderer, a_texture);
        if (Passthrough() == 2) return engineMax;
        // The native maximum function is constant. The enclosing functor's
        // owner is an authoritative texture identity even if its compiler
        // leaves unused argument registers containing another value.
        if (t_textureOwner) a_texture = t_textureOwner->rendererTexture;
        if (!a_texture || !t_note.fresh) {
            return engineMax;
        }

        const auto& texture = *static_cast<const EngineTexture*>(a_texture);
        const auto& header = texture.header;
        // The game's own target for this visit. The functor's visitor lost
        // its level and bias to this plugin, so it has to come from here.
        const auto gameFloor = g_qualityGuard ? t_floor : 0u;
        // A [MaxSize] cap holds the texture at least that low, as the game's
        // own level does; 0 for unsupported textures.
        const auto floor = std::max(gameFloor, t_cap);
        // [LOD] NoLowerRadius: right beside the camera, the game decides, as
        // in the passthrough arm; the bookkeeping below still runs.
        const bool gameDecides = Passthrough() == 1 || t_note.near;
        if ((header.flags & kHeaderCubemap) || header.width == 0 || header.height == 0 ||
            header.mipCount <= 1 || !TextureFormat::IsSupported(header.format)) {
            return gameDecides ? engineMax : std::max(std::min(t_note.vanilla, engineMax), floor);
        }
        const auto& settings = dtl::GetSettings();
        const bool starfield = settings.policy == static_cast<std::uint32_t>(dtl::DetailPolicy::Starfield);
        // Extend only the proven constant native maximum on a fresh,
        // authoritative source-policy visit. All native readiness/eligibility
        // and pending-request checks still run in the original functor.
        const auto policyMaximum = starfield && t_textureOwner &&
            t_textureOwner->rendererTexture == a_texture && engineMax == kEngineMaxLevel ? g_sourceMaxLevel : engineMax;
        const auto lowest = NativeTextureLevels::Maximum(header.width, header.height,
            header.mipCount, policyMaximum, TextureFormat::IsBlockCompressed(header.format));
        const float pixels = dtl::PixelsPerRepeat(t_note.pixels, t_uvScale);
        const float priority = starfield ? dtl::StarfieldTextureRank(
            dtl::StarfieldTextureDemand(t_note.angular, t_sourceUv, t_sourceUv, t_note.nearest,
                static_cast<float>(settings.nearDistance)), true, 1.0f) :
            dtl::Priority(pixels, t_note.screenHeight, t_note.nearest, static_cast<float>(settings.nearDistance));

        std::uint8_t planned = kNotPlanned;
        bool held = false;
        const bool followGame = FollowsGame(settings);
        {
            auto&            shard = ShardOf(a_texture);
            std::scoped_lock lock(shard.lock);
            const auto round = g_round.load(std::memory_order_relaxed);

            auto& tracked = shard.textures[a_texture];
            if (tracked.width != header.width || tracked.height != header.height || tracked.mips != header.mipCount ||
                tracked.format != header.format) {
                if (tracked.ticket) g_ledger.CancelChange(tracked.ticket,
                    { reinterpret_cast<std::uint64_t>(a_texture), tracked.generation });
                tracked = Tracked{};
                tracked.width = header.width;
                tracked.height = header.height;
                tracked.mips = header.mipCount;
                tracked.format = header.format;
                tracked.generation = g_generation.fetch_add(1, std::memory_order_relaxed);
            }

            if (settings.timeline && tracked.observation.lastVisit != 0 &&
                tracked.current < header.mipCount && texture.degradeLevel < header.mipCount) {
                if (texture.degradeLevel > tracked.current) {
                    g_observedLowerings.fetch_add(1, std::memory_order_relaxed);
                } else if (texture.degradeLevel < tracked.current) {
                    g_observedRestores.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto resourceNow = reinterpret_cast<std::uint64_t>(texture.resource);
            const bool towardsTarget = tracked.ticket &&
                ((tracked.planned > tracked.current && texture.degradeLevel > tracked.current &&
                    texture.degradeLevel <= tracked.planned) ||
                 (tracked.planned < tracked.current && texture.degradeLevel < tracked.current &&
                    texture.degradeLevel >= tracked.planned));
            if (resourceNow != tracked.resourceIdentity && !towardsTarget) {
                if (tracked.ticket) g_ledger.CancelChange(tracked.ticket,
                    { reinterpret_cast<std::uint64_t>(a_texture), tracked.generation });
                tracked.ticket = 0;
                tracked.planned = kNotPlanned;
                tracked.held = false;
            }
            if (towardsTarget) tracked.requestRound = round;
            tracked.lowest = static_cast<std::uint8_t>(lowest);
            tracked.current = texture.degradeLevel;
            tracked.cap = static_cast<std::uint8_t>(std::min(t_cap, lowest));
            tracked.resourceIdentity = resourceNow;
            if (g_qualityGuard) tracked.RecordFloor(round, static_cast<std::uint8_t>(std::min<std::uint32_t>(gameFloor, kNoFloor - 1)));
            const bool ownerMatches = t_textureOwner && t_textureOwner->rendererTexture == a_texture;
            tracked.nativeNoDegrade = !ownerMatches || (t_textureOwner->flags & 0x40u) == 0;
            tracked.near = t_note.near;
            tracked.nativePending = NativeAnswer::NativeBusy(texture.pendingRequests, texture.stateFlags,
                ownerMatches && (t_textureOwner->flags & 0x10u) != 0,
                ownerMatches ? static_cast<std::uint8_t>(t_textureOwner->desiredDegradeLevel) : texture.degradeLevel,
                texture.degradeLevel, g_legacyPending);
            if (tracked.ticket && tracked.current == tracked.planned && !tracked.nativePending) {
                g_ledger.CompleteChange(tracked.ticket,
                    { reinterpret_cast<std::uint64_t>(a_texture), tracked.generation });
                g_completedRequests.fetch_add(1, std::memory_order_relaxed);
                tracked.ticket = 0;
            } else if (tracked.ticket && !tracked.nativePending && round - tracked.requestRound > 2) {
                // A suggested native target is not proof the engine accepted
                // a request. Retry after two traversals without any progress.
                g_ledger.CancelChange(tracked.ticket,
                    { reinterpret_cast<std::uint64_t>(a_texture), tracked.generation });
                g_cancelledRequests.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<std::uint32_t> reported{ 0 };
                if (settings.timeline && reported.fetch_add(1, std::memory_order_relaxed) < 12) {
                    logger::info("Native target unfulfilled: name={} {}x{} mips={} format={} headerFlags={} "
                        "ownerFlags={} resourceFlags={} current={} suggested={} nativeDesired={}",
                        t_textureOwner ? t_textureOwner->GetName() : std::string_view{},
                        header.width, header.height, header.mipCount, header.format, header.flags,
                        t_textureOwner ? t_textureOwner->flags : 0, texture.stateFlags, tracked.current, tracked.planned,
                        t_textureOwner ? static_cast<int>(t_textureOwner->desiredDegradeLevel) : -1);
                }
                tracked.ticket = 0;
                tracked.planned = kNotPlanned;
                tracked.held = false;
            }
            const auto frame = g_frames.load(std::memory_order_relaxed);
            const bool recentGpu = starfield && tracked.gpuFrame != 0 && frame >= tracked.gpuFrame &&
                frame - tracked.gpuFrame <= 2;
            // Initial GPU integration can retain additional detail. Unknown
            // slots/views retain native CPU coverage until classified.
            tracked.observation.Visit(round, t_note.visible || recentGpu, pixels,
                recentGpu ? std::max(priority, tracked.gpuDemand) : priority);
            const auto now = DemandNow(tracked, round);
            if (starfield && tracked.planned != kNotPlanned && tracked.planned > tracked.current &&
                !tracked.emergencyRequest && RequestDemand::Stronger(tracked.requestDemand, now)) {
                if (tracked.ticket) g_ledger.CancelChange(tracked.ticket,
                    { reinterpret_cast<std::uint64_t>(a_texture), tracked.generation });
                tracked.ticket = 0;
                tracked.planned = kNotPlanned;
                tracked.held = false;
                g_staleDemand.fetch_add(1, std::memory_order_relaxed);
            }
            planned = tracked.planned;
            held = tracked.held;
            if (g_timeline) {
                if (!tracked.diagnosis) tracked.diagnosis = std::make_unique<Tracked::Diagnosis>();
                // Named once its owner is first seen; the inventory line lists the scene's textures.
                if (ownerMatches && tracked.diagnosis->name.empty()) {
                    auto& name = tracked.diagnosis->name;
                    name = t_textureOwner->GetName();
                    std::string lower(name);
                    std::ranges::transform(lower, lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    tracked.diagnosis->barrel = lower.find("barrel") != std::string::npos;
                    if (!name.empty()) {
                        logger::info("[lod-name] name={} size={}x{} mips={} format={}",
                            name, header.width, header.height, header.mipCount, header.format);
                    }
                }
                auto& diagnosis = *tracked.diagnosis;
                diagnosis.visit = t_visit;
                diagnosis.objectLevel = static_cast<std::uint8_t>(std::min<std::uint32_t>(t_note.vanilla, kNotPlanned));
                diagnosis.visitFloor = static_cast<std::uint8_t>(std::min<std::uint32_t>(gameFloor, kNotPlanned));
                diagnosis.answer = starfield ? static_cast<std::uint8_t>(NativeAnswer::Level(
                    { planned, held, texture.degradeLevel, lowest, floor, followGame })) : kNotPlanned;
                diagnosis.ownerFlags = ownerMatches ? t_textureOwner->flags : 0;
                diagnosis.ownerDesired = ownerMatches ? static_cast<std::uint8_t>(t_textureOwner->desiredDegradeLevel) : 0;
                diagnosis.stateFlags = texture.stateFlags;
                diagnosis.nativeRequests = texture.pendingRequests;
            }
        }

        if (g_timeline) g_texturesAnswered.fetch_add(1, std::memory_order_relaxed);
        // The full-work test arm: everything above ran, the game decides.
        if (gameDecides) return engineMax;
        // Starfield-mode changes must first reserve a session request. The
        // native renderer can still make its own independent transitions.
        // Never more detail than the game's own level for it: the game's own
        // distance lowering still happens, and nothing is held above it.
        if (starfield) return NativeAnswer::Level({ planned, held, texture.degradeLevel, lowest, floor, followGame });
        if (planned != kNotPlanned) {
            return std::max(std::min<std::uint32_t>(planned, lowest), floor);
        }

        // Not planned yet, for at most one cycle. Out of view, the engine's
        // own level; in view, only lower until the planner pays for upgrades.
        if (!t_note.visible) {
            return std::max(std::min(t_note.vanilla, lowest), floor);
        }
        const auto wanted = WantedNow(std::min(header.width, header.height), pixels, priority, lowest);
        return std::max(std::min(std::max<std::uint32_t>(wanted, texture.degradeLevel), lowest), floor);
    }

    using TextureFunctor_t = std::uint64_t (*)(void*, RE::NiTexture*);
    TextureFunctor_t g_textureFunctor = nullptr;

    std::uint64_t Hook_TextureFunctor(void* visitor, RE::NiTexture* owner)
    {
        const bool batchedBefore = owner && (owner->flags & 0x10u) != 0;
        if (Passthrough() == 2) {
            const auto result = g_textureFunctor(visitor, owner);
            if (owner && !batchedBefore && (owner->flags & 0x10u) != 0)
                g_batchAdds.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        const auto outer = t_textureOwner;
        t_textureOwner = owner;
        // All four verified functors read only through offset0x23 and never
        // mutate the visitor or pass it elsewhere. Preserve its flags while
        // removing the inherited room/bias clamp for a valid source-policy
        // observation. Native same-round owner demand, readiness and queue
        // checks remain in the original function.
        alignas(8) std::array<std::byte, 0x28> scopedVisitor{};
        const auto outerFloor = t_floor;
        const auto outerVisit = t_visit;
        const auto outerCap = t_cap;
        t_floor = 0;
        t_visit = {};
        const bool near = t_note.fresh && t_note.near;
        t_cap = near ? 0 : CapFor(owner);
        if (visitor && owner && t_note.fresh && dtl::GetSettings().policy == 1) {
            // The game's own target, as ProcessObjects::Func1 computes it:
            // the object's level, plus the room bias for a degradable
            // texture unless the bias is the 100 sentinel, at most 3. A
            // direct visitor holds this plugin's answer, not the game's.
            std::uint32_t base = 0, bias = 0;
            std::memcpy(&base, static_cast<const std::byte*>(visitor) + 0x18, sizeof(base));
            std::memcpy(&bias, static_cast<const std::byte*>(visitor) + 0x20, sizeof(bias));
            if (t_note.direct) base = t_note.vanilla;
            const bool degradable = (owner->flags & 0x40u) != 0;
            t_floor = NativeLimits::GameLevel(base, bias, degradable, kEngineMaxLevel);
            if (g_timeline) {
                std::uint32_t raw = 0;
                std::memcpy(&raw, static_cast<const std::byte*>(visitor) + 0x18, sizeof(raw));
                t_visit.base = static_cast<std::uint8_t>(std::min<std::uint32_t>(raw, 0xFF));
                t_visit.bias = static_cast<std::uint8_t>(std::min<std::uint32_t>(bias, 0xFF));
                t_visit.suppress = *(static_cast<const std::uint8_t*>(visitor) + 0x1E);
                t_visit.direct = t_note.direct;
            }

            // The full-work test arm keeps the game's own visitor, as does
            // anything within [LOD] NoLowerRadius.
            if (Passthrough() != 1 && !near) {
                std::memcpy(scopedVisitor.data(), visitor, 0x24);
                const std::uint32_t maximum = g_sourceMaxLevel, noBias = 0;
                std::memcpy(scopedVisitor.data() + 0x18, &maximum, sizeof(maximum));
                std::memcpy(scopedVisitor.data() + 0x20, &noBias, sizeof(noBias));
                visitor = scopedVisitor.data();
                if (g_timeline) g_scopedVisitors.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (visitor && t_cap) {
            // Any other visit keeps the game's level, which the native
            // maximum can only lower. A [MaxSize] cap above it replaces the
            // visitor's level, with no bias; the native checks still run.
            std::uint32_t base = 0, bias = 0;
            std::memcpy(&base, static_cast<const std::byte*>(visitor) + 0x18, sizeof(base));
            std::memcpy(&bias, static_cast<const std::byte*>(visitor) + 0x20, sizeof(bias));
            if (t_cap > NativeLimits::GameLevel(base, bias, (owner->flags & 0x40u) != 0, kEngineMaxLevel)) {
                std::memcpy(scopedVisitor.data(), visitor, 0x24);
                const std::uint32_t capped = t_cap, noBias = 0;
                std::memcpy(scopedVisitor.data() + 0x18, &capped, sizeof(capped));
                std::memcpy(scopedVisitor.data() + 0x20, &noBias, sizeof(noBias));
                visitor = scopedVisitor.data();
            }
        }
        // Classified before the native call: it may queue a level change.
        auto coverage = kCoverageClasses;
        std::uint64_t residentBytes = 0;
        if (g_timeline && owner) {
            if (!owner->rendererTexture) coverage = kNoRenderer;
            else {
                const auto& engine = *reinterpret_cast<const EngineTexture*>(owner->rendererTexture);
                const auto& header = engine.header;
                // The native functor returns early for a texture without the
                // allow-degrade flag that sits at full detail.
                coverage = (owner->flags & 0x40u) == 0 && engine.degradeLevel == 0 ? kNativeFixed :
                    t_note.fresh && dtl::GetSettings().policy == 1 ? kOffered : kUnmeasured;
                if (TextureFormat::IsSupported(header.format) && engine.degradeLevel < header.mipCount)
                    residentBytes = TextureFormat::ChainBytes(header.format, header.width, header.height,
                        engine.degradeLevel, header.mipCount - engine.degradeLevel);
            }
        }
        const auto result = g_textureFunctor(visitor, owner);
        t_textureOwner = outer;
        t_floor = outerFloor;
        t_visit = outerVisit;
        t_cap = outerCap;
        if (coverage == kNoRenderer) {
            const auto round = g_round.load(std::memory_order_relaxed);
            g_coverageTextures[round & 1][kNoRenderer].fetch_add(1, std::memory_order_relaxed);
        }
        const bool batched = owner && (owner->flags & 0x10u) != 0;
        if (batched && !batchedBefore) g_batchAdds.fetch_add(1, std::memory_order_relaxed);
        if (owner && owner->rendererTexture) {
            // Native TryAddToBatch changes this flag AFTER GetTextureMax's
            // call. Observe the accepted batch on the same native visit.
            auto& shard = ShardOf(owner->rendererTexture);
            std::scoped_lock lock(shard.lock);
            if (const auto it = shard.textures.find(owner->rendererTexture); it != shard.textures.end()) {
                it->second.nativeNoDegrade = (owner->flags & 0x40u) == 0;
                if (g_legacyPending) {
                    if (batched) it->second.nativePending = true;
                } else {
                    // The queued level is the round's minimum so far: the
                    // latest visit knows best whether the batch does anything.
                    const auto& engine = *reinterpret_cast<const EngineTexture*>(owner->rendererTexture);
                    it->second.nativePending = NativeAnswer::NativeBusy(engine.pendingRequests, engine.stateFlags,
                        batched, static_cast<std::uint8_t>(owner->desiredDegradeLevel), engine.degradeLevel, false);
                }
            }
            if (coverage != kCoverageClasses) {
                const auto round = g_round.load(std::memory_order_relaxed);
                if (auto& last = shard.coverage[owner->rendererTexture]; last != round) {
                    last = round;
                    g_coverageTextures[round & 1][coverage].fetch_add(1, std::memory_order_relaxed);
                    g_coverageBytes[round & 1][coverage].fetch_add(residentBytes, std::memory_order_relaxed);
                }
            }
        }
        return result;
    }

    // ── UpdateRec ────────────────────────────────────────────────────────────

    using UpdateRec_t = std::int32_t (*)(void*, void*);
    UpdateRec_t g_updateRec = nullptr;

    // Every arm: how long the native texture pass runs on each thread, timed
    // around the outermost UpdateRec, and which threads present and start
    // rounds. Threads get small tags in order of first use.
    struct StageThread
    {
        std::atomic<std::uint32_t> tag{ 0 };
        std::atomic<std::uint64_t> nanoseconds{ 0 }, calls{ 0 };
    };
    std::array<StageThread, 8> g_stageThreads;
    std::atomic<std::uint32_t> g_nextThreadTag{ 1 }, g_presentThread{ 0 }, g_roundThread{ 0 };
    thread_local std::uint32_t t_threadTag = 0;
    thread_local std::uint32_t t_recDepth = 0;
    thread_local StageThread* t_stageSlot = nullptr;

    std::uint32_t ThreadTag()
    {
        if (t_threadTag == 0) t_threadTag = g_nextThreadTag.fetch_add(1, std::memory_order_relaxed);
        return t_threadTag;
    }

    StageThread* StageSlot()
    {
        if (t_stageSlot) return t_stageSlot;
        const auto tag = ThreadTag();
        for (auto& slot : g_stageThreads) {
            std::uint32_t current = slot.tag.load(std::memory_order_relaxed);
            if (current == 0 && slot.tag.compare_exchange_strong(current, tag)) return t_stageSlot = &slot;
            if (current == tag) return t_stageSlot = &slot;
        }
        return nullptr;  // more threads than slots: not timed
    }

    // A node's own textures are walked right after its own level call, never
    // across a nested UpdateRec, so a note never needs to outlive one.
    std::int32_t Hook_UpdateRec(void* a_stage, void* a_node)
    {
        const bool outermost = t_recDepth++ == 0;
        const auto begun = outermost ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto outerNode = t_node;
        t_node = static_cast<RE::NiAVObject*>(a_node);
        t_note.fresh = false;
        const auto result = g_updateRec(a_stage, a_node);
        t_node = outerNode;
        t_note.fresh = false;
        --t_recDepth;
        if (outermost) {
            if (auto* slot = StageSlot()) {
                slot->nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begun).count()), std::memory_order_relaxed);
                slot->calls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return result;
    }

    using StartTextureRound_t = void (*)();
    StartTextureRound_t g_startTextureRound = nullptr;

    // ── Round throttle (test arm, OG only) ──────────────────────────────────
    // The native pass starts its next round as soon as the last one's batch
    // is processed, 7-10 times a second in a still scene, re-batching mostly
    // unchanged levels. TextureUpdateStart::Update returning true ends the
    // pass for this frame, as it does while the batch is still in flight.

    std::int64_t SteadyNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    using StageUpdate_t = bool (*)(void*);
    StageUpdate_t g_textureUpdateStart = nullptr;
    std::uint32_t g_minRoundMs = 0;
    std::atomic<std::int64_t> g_roundStartedMs{ 0 };
    std::atomic<std::uint64_t> g_throttledFrames{ 0 };
    const volatile std::uint8_t* g_forcedUpdate = nullptr;

    bool Hook_TextureUpdateStart(void* a_stage)
    {
        // Never during ForceUpdateTextures (loads, teleports): it needs the
        // whole pass at once.
        if (g_minRoundMs && !(g_forcedUpdate && *g_forcedUpdate)) {
            const auto started = g_roundStartedMs.load(std::memory_order_relaxed);
            if (started && SteadyNowMs() - started < static_cast<std::int64_t>(g_minRoundMs)) {
                g_throttledFrames.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return g_textureUpdateStart(a_stage);
    }

    void InstallRoundThrottle(std::uint32_t a_minRoundMs)
    {
        if (REL::Module::IsNG() || REL::Module::IsVR()) {
            logger::warn("Round throttle: OG 1.10.163 only; not installed");
            return;
        }
        const auto vtable = Addr::Resolve(Addr::TextureUpdateStartVtable);
        const auto update = Addr::Resolve(Addr::TextureUpdateStartUpdate);
        const auto forced = Addr::Resolve(Addr::ForcedTextureUpdate);
        auto* slot = reinterpret_cast<std::uintptr_t*>(vtable);
        if (!vtable || !update || !forced || *slot != update) {
            logger::warn("Round throttle: TextureUpdateStart vtable slot does not hold its Update; not installed");
            return;
        }
        std::uint32_t protection{};
        if (!REX::W32::VirtualProtect(slot, sizeof(*slot), REX::W32::PAGE_READWRITE, &protection)) {
            logger::warn("Round throttle: vtable not writable; not installed");
            return;
        }
        g_textureUpdateStart = reinterpret_cast<StageUpdate_t>(update);
        g_forcedUpdate = reinterpret_cast<const volatile std::uint8_t*>(forced);
        g_minRoundMs = a_minRoundMs;
        *slot = reinterpret_cast<std::uintptr_t>(&Hook_TextureUpdateStart);
        std::uint32_t ignored{};
        REX::W32::VirtualProtect(slot, sizeof(*slot), protection, &ignored);
        logger::info("Round throttle installed: at most one native texture round per {} ms, never while forced",
            a_minRoundMs);
    }

    void Hook_StartTextureRound()
    {
        g_startTextureRound();
        g_roundStartedMs.store(SteadyNowMs(), std::memory_order_relaxed);
        g_roundThread.store(ThreadTag(), std::memory_order_relaxed);
        const auto round = g_round.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_timeline) {
            for (auto& count : g_coverageTextures[round & 1]) count.store(0, std::memory_order_relaxed);
            for (auto& bytes : g_coverageBytes[round & 1]) bytes.store(0, std::memory_order_relaxed);
        }
        t_note.fresh = false;
    }

    // ── Planning ─────────────────────────────────────────────────────────────

    std::uint64_t SteadyMs()
    {
        return 1 + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    std::uint32_t ElapsedMs(std::uint64_t a_now, std::uint64_t a_then)
    {
        if (a_then == 0) return dtl::kLongAgoMs;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(a_now > a_then ? a_now - a_then : 0, dtl::kLongAgoMs - 1));
    }

    // One cycle over every tracked texture: the shared planner decides, and
    // the update stage hands out its answers from the next visit on.
    std::vector<dtl::PlanEntry> SnapshotEntries()
    {
        static std::size_t lastCount = 0;  // planner thread only
        std::vector<dtl::PlanEntry>                       entries;
        entries.reserve(lastCount + lastCount / 8 + 64);
        const auto nowMs = SteadyMs();
        const auto safetyMargin = dtl::GetSettings().safetyMargin;
        // By distance, every visit counts, in view or not.
        const bool byDistance = dtl::GetSettings().unseen == static_cast<std::uint32_t>(dtl::Unseen::ByDistance);
        for (auto& shard : g_shards) {
            std::scoped_lock lock(shard.lock);
            const auto round = g_round.load(std::memory_order_relaxed);
            for (auto it = shard.textures.begin(); it != shard.textures.end();) {
                auto& tracked = it->second;
                if (round - tracked.observation.lastVisit > 2) {
                    if (tracked.ticket) g_ledger.CancelChange(tracked.ticket,
                        { reinterpret_cast<std::uint64_t>(it->first), tracked.generation });
                    it = shard.textures.erase(it);
                    continue;
                }
                tracked.observation.Advance(round);

                const float pixels = byDistance ? tracked.observation.ReachPixels() : tracked.observation.Pixels();
                const float priority = byDistance ? tracked.observation.Reach() : tracked.observation.Priority();

                dtl::PlanEntry entry;
                entry.key = reinterpret_cast<std::uint64_t>(it->first);
                entry.generation = tracked.generation;
                entry.resourceIdentity = tracked.resourceIdentity;
                // Within [LOD] NoLowerRadius the game decides: nothing to plan.
                entry.pending = tracked.ticket != 0 || tracked.nativePending || !tracked.resourceIdentity || tracked.near;
                entry.noDegrade = tracked.nativeNoDegrade;
                entry.seen = tracked.observation.Seen(round);
                entry.priority = priority;
                entry.current = static_cast<std::uint8_t>(std::min<std::uint32_t>(tracked.current, dtl::kMaxPlanLevel));
                entry.previous = tracked.planned != kNotPlanned ? tracked.planned : entry.current;
                entry.lowest = tracked.lowest;
                entry.wanted = static_cast<std::uint8_t>(
                    WantedNow(std::min(tracked.width, tracked.height), pixels, priority, tracked.lowest));
                dtl::FillBytes(entry, tracked.format, tracked.width, tracked.height, tracked.mips);

                // Timed here, once a cycle, rather than on every native visit.
                if (entry.seen) tracked.lastSeenMs = nowMs;
                if (tracked.snapshotLevel != tracked.current) {
                    if (tracked.snapshotLevel != kNotPlanned) {
                        tracked.lastChangeMs = nowMs;
                        if (tracked.current < tracked.snapshotLevel) tracked.lastRaiseMs = nowMs;
                    }
                    tracked.snapshotLevel = tracked.current;
                }
                entry.unseenMs = entry.seen ? 0 : ElapsedMs(nowMs, tracked.lastSeenMs);
                entry.sinceChangeMs = ElapsedMs(nowMs, tracked.lastChangeMs);
                entry.sinceRaiseMs = ElapsedMs(nowMs, tracked.lastRaiseMs);
                if (g_qualityGuard) {
                    const auto floor = tracked.Floor();
                    entry.minLevel = floor;
                    // The game holds it lower than this plugin's levels reach:
                    // its own streamer has it, and the visit answers its level.
                    if (floor > entry.lowest) entry.pending = true;
                    // On screen, never past what the screen can show.
                    const dtl::Policy policy{ safetyMargin, entry.lowest };
                    entry.seenCap = static_cast<std::uint8_t>(dtl::WantedLevel(policy,
                        std::min(tracked.width, tracked.height), pixels, priority >= 2.0f));
                }
                // [MaxSize] [MipSkip]: never planned with more detail than its cap.
                entry.minLevel = std::max(entry.minLevel, tracked.cap);
                // [Smoothing] UpgradeHoldSpeed: moving fast, nothing gains detail.
                if (g_upgradeHold.load(std::memory_order_relaxed))
                    entry.minLevel = std::max(entry.minLevel, entry.current);
                if (tracked.diagnosis) {
                    auto& diagnosis = *tracked.diagnosis;
                    diagnosis.snapshotted = true;
                    diagnosis.pending = entry.pending;
                    diagnosis.seenCap = g_qualityGuard ? entry.seenCap : kNotPlanned;
                    diagnosis.minLevel = g_qualityGuard ? entry.minLevel : kNotPlanned;
                    diagnosis.rankLevel = entry.wanted;
                    diagnosis.pixels = pixels;
                    diagnosis.priority = priority;
                }

                entries.push_back(entry);
                ++it;
            }
            if (g_timeline)
                std::erase_if(shard.coverage, [round](const auto& item) { return round - item.second > 2; });
        }
        lastCount = entries.size();
        return entries;
    }

    void Plan()
    {
        auto entries = SnapshotEntries();
        const auto result = dtl::budget::PlanCycle(entries);
        g_mode.store(static_cast<std::uint8_t>(result.mode), std::memory_order_relaxed);
        // [Smoothing] UpgradeHoldSpeed: this planner does not read minLevel.
        const bool hold = g_upgradeHold.load(std::memory_order_relaxed);

        for (const auto& entry : entries) {
            const auto* texture = reinterpret_cast<const void*>(entry.key);
            auto&       shard = ShardOf(texture);

            std::scoped_lock lock(shard.lock);
            const auto       it = shard.textures.find(texture);
            if (it != shard.textures.end() && it->second.generation == entry.generation &&
                it->second.resourceIdentity == entry.resourceIdentity &&
                !(hold && entry.planned < it->second.current)) {
                it->second.planned = entry.planned;
            }
        }
    }

    dtl::RequestReceipt AcceptRequest(void*, const dtl::PlanRequest& request)
    {
        const auto* texture = reinterpret_cast<const void*>(request.identity.key);
        auto& shard = ShardOf(texture);
        std::scoped_lock lock(shard.lock);
        const auto it = shard.textures.find(texture);
        if (it == shard.textures.end()) return { dtl::RequestStatus::Stale, 0 };
        auto& tracked = it->second;
        if (tracked.generation != request.identity.generation ||
            tracked.resourceIdentity != request.resourceIdentity || tracked.current != request.current)
            return { dtl::RequestStatus::Stale, 0 };
        if (tracked.ticket || tracked.nativePending || !tracked.resourceIdentity || request.target > tracked.lowest ||
            tracked.near)
            return { dtl::RequestStatus::Rejected, 0 };
        // Moving fast: no raises (the snapshot already held them).
        if (request.target < request.current && g_upgradeHold.load(std::memory_order_relaxed))
            return { dtl::RequestStatus::Rejected, 0 };
        if (tracked.nativeNoDegrade && request.target > tracked.current)
            return { dtl::RequestStatus::Rejected, 0 };
        // Never more detail than the game's own level: the visit would not
        // answer it, and the request would wait out its retries for nothing.
        if (g_qualityGuard && request.target < request.current && request.target < tracked.Floor())
            return { dtl::RequestStatus::Rejected, 0 };
        // Nor more than a [MaxSize] cap allows.
        if (request.target < tracked.cap) return { dtl::RequestStatus::Rejected, 0 };
        if (NativeAnswer::StaleFollow(FollowsGame(dtl::GetSettings()), request.current, request.target,
                std::max<std::uint32_t>(tracked.Floor(), tracked.cap)))
            return { dtl::RequestStatus::Stale, 0 };
        const auto round = g_round.load(std::memory_order_relaxed);
        tracked.observation.Advance(round);
        const auto now = DemandNow(tracked, round);
        const auto snapshot = g_cycleDemand.find(request.identity.key);
        const bool emergency = request.reason == dtl::RequestReason::Emergency;
        if (snapshot == g_cycleDemand.end() ||
            (request.target > request.current && !emergency && RequestDemand::Stronger(snapshot->second, now)))
            return { dtl::RequestStatus::Stale, 0 };
        // Each accepted change becomes a re-create on the game's creation thread.
        if (!g_changeRate.Take(SteadyMs())) return { dtl::RequestStatus::Rejected, 0 };
        dtl::PlanEntry entry;
        dtl::FillBytes(entry, tracked.format, tracked.width, tracked.height, tracked.mips);
        tracked.planned = request.target;
        tracked.held = NativeAnswer::Holds(request.target, std::max<std::uint32_t>(tracked.Floor(), tracked.cap));
        tracked.requestRound = g_round.load(std::memory_order_relaxed);
        tracked.requestDemand = now;
        tracked.emergencyRequest = emergency;
        tracked.ticket = g_ledger.Request(request.identity, entry.bytes[request.current], entry.bytes[request.target]);
        g_requests.fetch_add(1, std::memory_order_relaxed);
        // This reserves the plugin's suggested change; the native visit may
        // decline or delay it. No requested delta is counted as physical free.
        return { dtl::RequestStatus::AcceptedPending, request.estimatedBytes };
    }

    // Test arms only: once, SyntheticPressureFromS after the world began
    // rendering, the usage reading gains what puts it SyntheticPressureMB over
    // the line, for SyntheticPressureForS. Lowering then frees real memory as
    // under real pressure; afterwards refill has to bring everything back.
    void UpdateSyntheticPressure()
    {
        enum class Phase { Waiting, On, Done };
        static Phase phase = Phase::Waiting;  // planner thread only
        const auto& settings = dtl::GetSettings();
        const auto start = g_worldStartMs.load(std::memory_order_relaxed);
        if (!settings.syntheticPressureMB || phase == Phase::Done || start == 0) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - start;
        const auto from = static_cast<std::int64_t>(settings.syntheticPressureFromS) * 1000;
        const auto until = from + static_cast<std::int64_t>(settings.syntheticPressureForS) * 1000;
        if (phase == Phase::Waiting && elapsed >= from) {
            const auto budget = dtl::budget::Current();
            if (budget.line == 0) return;  // no reading yet
            const std::uint64_t extra = (budget.line > budget.usage ? budget.line - budget.usage : 0) +
                (static_cast<std::uint64_t>(settings.syntheticPressureMB) << 20);
            dtl::budget::SetSyntheticUsage(extra);
            phase = Phase::On;
            logger::info("[lod-synthetic] on: extra {} MB, usage {} MB, line {} MB, {} s after the world began",
                extra >> 20, budget.usage >> 20, budget.line >> 20, elapsed / 1000);
        } else if (phase == Phase::On && elapsed >= until) {
            dtl::budget::SetSyntheticUsage(0);
            phase = Phase::Done;
            logger::info("[lod-synthetic] off, {} s after the world began", elapsed / 1000);
        }
    }

    // [Budget] SteerUpgradeLimit: tight or over, and for RefillCalmMs after,
    // the game's own streamer may raise textures only into the room left
    // under the budget line; after a calm, the game's own limit. Planner
    // thread only.
    // Planner thread only, except OnFrame's hand-back when switched off, after the planner's last job.
    std::uint64_t g_writtenUpgradeLimit = 0, g_gameUpgradeLimit = 0;

    void SteerUpgradeLimit()
    {
        auto& written = g_writtenUpgradeLimit;
        auto& game = g_gameUpgradeLimit;
        static bool steering = false;            // planner thread only
        static std::uint64_t lastPressureMs = 0;  // planner thread only
        if (!g_steerUpgradeLimit || !g_nativeUpgradeLimit || !g_nativeTextureBytes) return;
        const std::uint64_t now = *g_nativeUpgradeLimit;
        // The game writes its limit at device creation: a value this plugin
        // did not write is the game's own.
        if (now != written) game = now;
        const auto budget = dtl::budget::Current();
        const bool tight = budget.mode == dtl::Mode::Tight || budget.mode == dtl::Mode::Over;
        const auto nowMs = SteadyMs();
        if (tight) lastPressureMs = nowMs;
        const bool pressure = NativeLimits::KeepSteering(tight, nowMs, lastPressureMs, dtl::GetSettings().refillCalmMs);
        const std::uint64_t textures = *g_nativeTextureBytes;
        // [Budget] SteerToTarget: the game's own raises may fill only the room
        // under the target, at all times; by default only under pressure and up
        // to the line. In the heaviest n28 setup shipped's usage climbed from
        // 5.8 to 6.74 GB while turning, game raises filling up to the line.
        const auto cap = g_steerToTarget && budget.target ? budget.target : budget.line;
        const auto limit = NativeLimits::UpgradeLimit(game, textures, cap, budget.usage, pressure || g_steerToTarget);
        if (limit != now) {
            *g_nativeUpgradeLimit = limit;
            g_upgradeLimitWrites.fetch_add(1, std::memory_order_relaxed);
        }
        written = limit;
        if (pressure != steering) {
            steering = pressure;
            if (pressure) logger::info("Native upgrade limit {} MB: textures {} MB plus {} MB under the {} (game's own {} MB)",
                limit >> 20, textures >> 20, (cap > budget.usage ? cap - budget.usage : 0) >> 20,
                cap == budget.line ? "line" : "target", game >> 20);
            else logger::info("Native upgrade limit back to the game's own {} MB", game >> 20);
        }
    }

    // [Smoothing] UpgradeHoldSpeed, from the camera's movement. Planner
    // thread only.
    void UpdateUpgradeHold()
    {
        static dtl::SpeedHold speed;  // planner thread only
        const auto limit = dtl::GetSettings().upgradeHoldSpeed;
        if (!limit || Passthrough()) return;
        // Only once the game's own texture pass has measured an object: on its
        // first frame the camera may not exist yet, and reading it then could
        // stop planning on the very first frame.
        if (g_worldStartMs.load(std::memory_order_relaxed) == 0) return;
        View view;
        const bool hold = ReadView(view) ?
            speed.Update(view.eye.x, view.eye.y, view.eye.z, SteadyMs(), limit) :
            (speed.Reset(), false);
        if (hold != g_upgradeHold.load(std::memory_order_relaxed)) {
            g_upgradeHold.store(hold, std::memory_order_relaxed);
            if (hold) {
                g_upgradeHolds.fetch_add(1, std::memory_order_relaxed);
                logger::debug("Raises held: camera at {:.0f} units/s (limit {})", speed.Speed(), limit);
            } else {
                logger::debug("Raises resume");
            }
        }
    }

    // The step PlanFrame is in, so a failure report and the frame hook's stall
    // report can say where planning stopped. Planner thread only.
    const char* g_planStep = "none";

    void PlanFrame(std::uint64_t frame)
    {
        const auto started = std::chrono::steady_clock::now();
        g_frames.store(frame, std::memory_order_relaxed);
        g_planStep = "synthetic pressure";
        UpdateSyntheticPressure();
        g_planStep = "upgrade limit";
        SteerUpgradeLimit();
        g_planStep = "upgrade hold";
        UpdateUpgradeHold();
        static auto lastBegin = std::chrono::steady_clock::time_point{};  // planner thread only
        const bool idle = g_session.Done() || g_session.Stage() == dtl::PlanStage::Idle;
        // With room to spare, a new snapshot a few times a second is plenty;
        // tight or over, every frame as the source does.
        if (idle && !dtl::budget::CycleDue(lastBegin)) {
            // Nothing to plan this frame.
        } else if (idle) {
            lastBegin = started;
            g_planStep = "snapshot";
            auto entries = SnapshotEntries();
            g_cycleDemand.clear();
            const bool byDistance = dtl::GetSettings().unseen == static_cast<std::uint32_t>(dtl::Unseen::ByDistance);
            for (const auto& entry : entries)
                g_cycleDemand.emplace(entry.key, RequestDemand::Snapshot{ entry.seen || byDistance,
                    entry.wanted, entry.priority });
            g_planStep = "mesh gather";
            const auto meshes = g_meshInstalled ? Fo4Mesh::Gather(g_round.load(std::memory_order_relaxed)) :
                std::vector<dtl::PlanEntry>{};
            // Mesh requests accepted by this cycle spend inside the shared
            // planner. Pending work from earlier cycles is not carry-in work.
            g_planStep = "begin plan";
            dtl::budget::BeginPlan(g_session, entries, g_ledger.Snapshot(), 0, meshes);
            g_mode.store(static_cast<std::uint8_t>(dtl::budget::CurrentMode()), std::memory_order_relaxed);
            // Gather is a frame barrier in the source job graph. Do not run
            // multiple complete gather/plan cycles within one renderer frame.
        } else {
            g_planStep = "step plan";
            const bool done = dtl::budget::StepPlan(g_session, &AcceptRequest, nullptr,
                g_meshInstalled ? &Fo4Mesh::Accept : nullptr, nullptr);
            if (done) {
                g_planStep = "record plan";
                dtl::budget::RecordPlan(g_session.Result());
                g_cycles.fetch_add(1, std::memory_order_relaxed);
                if (g_timeline) {
                    const auto& result = g_session.Result();
                    g_planHeld.store(result.upgradesHeld, std::memory_order_relaxed);
                    g_planAccepted.store(result.acceptedRequests, std::memory_order_relaxed);
                    g_planStale.store(result.staleRequests, std::memory_order_relaxed);
                }
            }
        }
        const auto micros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        auto peak = g_peakPlanMicroseconds.load(std::memory_order_relaxed);
        while (peak < micros && !g_peakPlanMicroseconds.compare_exchange_weak(peak, micros)) {}
        static auto nextTimeline = std::chrono::steady_clock::now();
        if (dtl::GetSettings().timeline && std::chrono::steady_clock::now() >= nextTimeline) {
            g_planStep = "timeline";
            Lod::LogSummary();
            nextTimeline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
    }

    // The planner stopped: give every texture back to the game, as without
    // the plugin. Visits take the game's own answers (the passthrough arm),
    // nothing loads at the lowest level any more, raises are no longer held,
    // and a native upgrade limit this plugin steered returns to the game's.
    // Planner thread only.
    void FailSafe()
    {
        g_failedSafe.store(true, std::memory_order_relaxed);
        g_passthrough.store(1, std::memory_order_relaxed);
        g_mode.store(static_cast<std::uint8_t>(dtl::Mode::Unlimited), std::memory_order_relaxed);
        g_upgradeHold.store(false, std::memory_order_relaxed);
        if (g_nativeUpgradeLimit && g_writtenUpgradeLimit && *g_nativeUpgradeLimit == g_writtenUpgradeLimit)
            *g_nativeUpgradeLimit = g_gameUpgradeLimit;
    }

    void StarfieldWorker()
    {
        FaultReport::TranslateOnThisThread();
        bool healthy = true;
        for (;;) {
            std::uint64_t frame;
            {
                std::unique_lock lock(g_jobLock);
                g_jobWake.wait(lock, [] { return g_pendingFrame != g_completedFrame; });
                frame = g_pendingFrame;
            }
            if (healthy) {
                std::string failure;
                try { PlanFrame(frame); }
                catch (const std::exception& e) {
                    // Out of memory may fail the report too; short strings need no allocation.
                    try { failure = std::format("{}: {}", typeid(e).name(), e.what()); } catch (...) { failure = "std::exception"; }
                }
                catch (...) { failure = "not a std::exception"; }
                if (!failure.empty()) {
                    healthy = false;
                    try { FailSafe(); } catch (...) {}
                    logger::error("Texture planning stopped after an unexpected worker failure at frame {} in {} ({}); "
                                  "the game decides every texture now", frame, g_planStep, failure);
                }
            }
            // Diagnostic drain performs no GPU work and holds no adapter,
            // renderer, allocator or worker coordination lock here.
            Fo4Mesh::Lifetime::Diagnostics::PollLog();
            {
                std::scoped_lock lock(g_jobLock);
                g_completedFrame = frame;
            }
            g_jobDone.notify_one();
        }
    }

    // The source waits for the previous frame's planning job here; so do the
    // exact comparison mode and the experimental GPU and mesh paths, which
    // were validated with it. Otherwise Present never waits on the planner:
    // a frame it is still busy with is simply not handed another one.
    bool g_presentWaits = true;
    std::atomic<std::uint64_t> g_framesSkipped{ 0 };

    // Every arm, timeline or not: cumulative renderer frames, native texture
    // rounds and newly batched textures, for rates between arms.
    void LogRates(std::uint64_t frame)
    {
        static auto next = std::chrono::steady_clock::time_point{};  // renderer thread only
        const auto now = std::chrono::steady_clock::now();
        if (now < next) return;
        next = now + std::chrono::seconds(10);
        g_presentThread.store(ThreadTag(), std::memory_order_relaxed);
        // Cumulative per thread: tag:milliseconds:outermost calls.
        std::string stage;
        for (const auto& slot : g_stageThreads) {
            const auto tag = slot.tag.load(std::memory_order_relaxed);
            if (!tag) continue;
            stage += std::format("{}{}:{}:{}", stage.empty() ? "" : ",", tag,
                slot.nanoseconds.load(std::memory_order_relaxed) / 1000000, slot.calls.load(std::memory_order_relaxed));
        }
        logger::info("[lod-rate] frames={} rounds={} batchAdds={} passthrough={} legacyPending={} presentThread={} "
                     "roundThread={} throttledFrames={} minRoundMs={} stage={}", frame,
            g_round.load(std::memory_order_relaxed), g_batchAdds.load(std::memory_order_relaxed), Passthrough(),
            g_legacyPending ? 1 : 0, g_presentThread.load(std::memory_order_relaxed),
            g_roundThread.load(std::memory_order_relaxed), g_throttledFrames.load(std::memory_order_relaxed),
            g_minRoundMs, stage.empty() ? "none" : stage);
    }

    void OnFrame(std::uint64_t frame)
    {
        if (!g_installed.load(std::memory_order_acquire)) return;
        FramePump::Step("texture planner: rate log");
        LogRates(frame);
        // Hooks only: no planning, no video memory sampling.
        if (Passthrough() == 2) {
            // Switched off: once the planner's last job is done, the game gets its own upgrade limit back.
            if (g_releaseLimit.exchange(false, std::memory_order_acq_rel)) {
                FramePump::Step("texture planner: last job before mod off");
                std::unique_lock lock(g_jobLock);
                g_jobDone.wait(lock, [] { return g_completedFrame == g_pendingFrame; });
                const bool steered = g_nativeUpgradeLimit && g_writtenUpgradeLimit &&
                    *g_nativeUpgradeLimit == g_writtenUpgradeLimit;
                if (steered) *g_nativeUpgradeLimit = g_gameUpgradeLimit;
                g_writtenUpgradeLimit = 0;
                g_upgradeHold.store(false, std::memory_order_relaxed);
                if (steered) logger::info("Native upgrade limit back to the game's own {} MB (mod off)", g_gameUpgradeLimit >> 20);
            }
            return;
        }
        bool busy = false;
        {
            FramePump::Step("texture planner: job lock");
            std::unique_lock lock(g_jobLock);
            if (g_presentWaits) g_jobDone.wait(lock, [] { return g_completedFrame == g_pendingFrame; });
            else busy = g_completedFrame != g_pendingFrame;
        }
        FramePump::Step("texture planner: feedback");
        if (g_meshInstalled) Fo4Mesh::OnPresent(frame);
        TextureFeedback::OnFrame(frame);
        GpuGather::SetFrame(frame);
        // A kernel call: it can wait while Windows moves video memory.
        FramePump::Step("texture planner: video memory reading");
        dtl::budget::OnFrame(frame);
        FramePump::Step("texture planner: wake");
        if (busy) {
            g_framesSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        {
            std::scoped_lock lock(g_jobLock);
            g_pendingFrame = frame;
        }
        g_jobWake.notify_one();
    }

    void ObserveDraw(const GpuGather::Draw& observed)
    {
        auto draw = observed;
        if (g_meshObservations) Fo4Mesh::Observe(draw, g_round.load(std::memory_order_relaxed));
        TextureFeedback::Observe(draw);
    }

    void Planner()
    {
        const bool timeline = dtl::GetSettings().timeline;
        auto nextTimeline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;) {
            std::this_thread::sleep_for(kPlanEvery);
            try {
                SteerUpgradeLimit();
                UpdateUpgradeHold();
                Plan();
            } catch (...) {
                try { FailSafe(); } catch (...) {}
                logger::error("Texture planning stopped after an unexpected planner failure; the game decides every texture now");
                return;
            }
            if (timeline && std::chrono::steady_clock::now() >= nextTimeline) {
                // Plan has released every shard lock before taking a snapshot.
                Lod::LogSummary();
                nextTimeline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }
    }

    // ── Finding the calls ────────────────────────────────────────────────────

    bool IsCall(std::uintptr_t a_at) { return *reinterpret_cast<const std::uint8_t*>(a_at) == 0xE8; }

    std::uintptr_t CallTarget(std::uintptr_t a_at)
    {
        const auto relative = *reinterpret_cast<const std::int32_t*>(a_at + 1);
        return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(a_at + 5) + relative);
    }

    // Every direct call to a_function in the game's code. Matching the exact
    // target makes a false hit from stray bytes vanishingly unlikely, and the
    // counts are checked below.
    std::vector<std::uintptr_t> CallsTo(std::uintptr_t a_function)
    {
        const auto text = REL::Module::get().segment(REL::Segment::text);
        const auto begin = text.address();
        const auto end = begin + text.size();

        std::vector<std::uintptr_t> sites;
        for (auto at = begin; at + 5 <= end; ++at) {
            if (IsCall(at) && CallTarget(at) == a_function) {
                sites.push_back(at);
            }
        }
        return sites;
    }

    bool Within(std::uintptr_t a_at, std::uintptr_t a_function, std::size_t a_size)
    {
        return a_at >= a_function && a_at < a_function + a_size;
    }

    // The first call inside a function, which in both update passes is the
    // one for the object itself; the later ones are for room bounds.
    std::optional<std::uintptr_t> FirstWithin(const std::vector<std::uintptr_t>& a_sites, std::uintptr_t a_function,
        std::size_t a_size)
    {
        for (const auto site : a_sites) {  // ascending
            if (Within(site, a_function, a_size)) {
                return site;
            }
        }
        return std::nullopt;
    }

    // The synchronous virtual call directly after the object's level call:
    // FF 90 A8 01 00 00 = call [rax + 0x1A8], or +0x1B0 on VR.
    // Validate before any patch.
    std::optional<std::uintptr_t> TextureVisitAfter(std::optional<std::uintptr_t> a_level)
    {
        if (!a_level) {
            return std::nullopt;
        }
        const std::uint8_t call[]{ 0xFF, 0x90,
            static_cast<std::uint8_t>(Addr::ForEachTextureSlot() * 8), 0x01, 0x00, 0x00 };
        for (auto at = *a_level + 5; at < *a_level + 40; ++at) {
            if (std::memcmp(reinterpret_cast<const void*>(at), call, sizeof(call)) == 0) {
                return at;
            }
        }
        return std::nullopt;
    }

    std::size_t FunctorPrologueSize(std::uintptr_t function)
    {
        // Complete, RIP-independent pushes, verified against OG/NG/AE/VR.
        constexpr std::uint8_t classic[]{ 0x40, 0x53, 0x55, 0x41, 0x56 };
        constexpr std::uint8_t modern[]{ 0x40, 0x53, 0x41, 0x56, 0x41, 0x57 };
        const auto bytes = REL::Module::IsNG() ? std::span<const std::uint8_t>(modern) : std::span<const std::uint8_t>(classic);
        return std::memcmp(reinterpret_cast<const void*>(function), bytes.data(), bytes.size()) == 0 ? bytes.size() : 0;
    }

    void InstallFunctor(std::uintptr_t function, std::size_t size)
    {
        auto& trampoline = F4SE::GetTrampoline();
        auto* resume = static_cast<std::uint8_t*>(trampoline.allocate(size + 14));
        std::memcpy(resume, reinterpret_cast<const void*>(function), size);
        constexpr std::uint8_t jump[]{ 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(resume + size, jump, sizeof(jump));
        const auto next = function + size;
        std::memcpy(resume + size + 6, &next, sizeof(next));
        g_textureFunctor = reinterpret_cast<TextureFunctor_t>(resume);
        REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), resume, size + 14);
        if (size == 6) trampoline.write_branch<6>(function, Hook_TextureFunctor);
        else trampoline.write_branch<5>(function, Hook_TextureFunctor);
    }

    // Timeline only, every few seconds: how much is held below the game's own
    // level and why, the refill gate, the most visible held textures, and any
    // texture named like the validated scene's barrel. Nothing plans from it.
    void LogHolds()
    {
        struct Row
        {
            std::uint16_t      width, height;
            std::uint8_t       mips, format, current, planned, lowest, floor0, floor1;
            bool               held;
            Tracked::Diagnosis diagnosis;
        };
        constexpr std::size_t kBarrels = 12, kHeld = 16;
        std::vector<Row> barrels, held;
        std::uint64_t belowFloor = 0, belowFloorBytes = 0, pending = 0, overSeenCap = 0, awaitingRefill = 0;
        std::uint64_t belowObject = 0, belowObjectBytes = 0, undiagnosed = 0;
        for (auto& shard : g_shards) {
            std::scoped_lock lock(shard.lock);
            for (const auto& [texture, tracked] : shard.textures) {
                if (!tracked.diagnosis || !tracked.diagnosis->snapshotted ||
                    tracked.diagnosis->minLevel == kNotPlanned) {
                    ++undiagnosed;
                    continue;
                }
                const auto& diagnosis = *tracked.diagnosis;
                const auto current = tracked.current;
                const bool underFloor = current > diagnosis.minLevel && current < tracked.mips;
                const bool underObject = current > diagnosis.objectLevel && current < tracked.mips;
                if (underFloor) {
                    ++belowFloor;
                    belowFloorBytes += TextureFormat::ChainBytes(tracked.format, tracked.width, tracked.height,
                        diagnosis.minLevel, current - diagnosis.minLevel);
                    if (diagnosis.pending) ++pending;
                    else if (current > diagnosis.seenCap) ++overSeenCap;
                    else ++awaitingRefill;
                }
                if (underObject) {
                    ++belowObject;
                    belowObjectBytes += TextureFormat::ChainBytes(tracked.format, tracked.width, tracked.height,
                        diagnosis.objectLevel, current - diagnosis.objectLevel);
                }
                if (!diagnosis.barrel && !underFloor && !underObject) continue;
                (diagnosis.barrel ? barrels : held).push_back(Row{ tracked.width, tracked.height, tracked.mips,
                    tracked.format, current, tracked.planned, tracked.lowest, tracked.floor[0], tracked.floor[1],
                    tracked.held, diagnosis });
            }
        }
        const auto byPixels = [](const Row& a, const Row& b) { return a.diagnosis.pixels > b.diagnosis.pixels; };
        std::ranges::sort(barrels, byPixels);
        std::ranges::sort(held, byPixels);
        if (barrels.size() > kBarrels) barrels.resize(kBarrels);
        if (held.size() > kHeld) held.resize(kHeld);

        const auto refill = dtl::budget::LastRefill();
        logger::info("[lod-refill] considered={} quiet={} refilling={} refillMB={} sinceLoadMs={} sincePressureMs={} "
                     "sharedRiseMB={} spentMB={} targetMB={} planHeld={} planAccepted={} planStale={} "
                     "belowFloor={} belowFloorMB={} pending={} overSeenCap={} awaitingRefill={} "
                     "belowObjectLevel={} belowObjectMB={} undiagnosed={}",
            refill.considered, refill.quiet, refill.refilling, refill.refillBytes >> 20, refill.sinceLoadMs,
            refill.sincePressureMs, refill.sharedRise >> 20, refill.spent >> 20, refill.target >> 20,
            g_planHeld.load(), g_planAccepted.load(), g_planStale.load(), belowFloor, belowFloorBytes >> 20, pending,
            overSeenCap, awaitingRefill, belowObject, belowObjectBytes >> 20, undiagnosed);
        const auto log = [](std::string_view tag, const Row& row) {
            const auto& d = row.diagnosis;
            logger::info("[lod-hold] tag={} name={} size={}x{} mips={} format={} current={} planned={} held={} floor={}/{} "
                         "minLevel={} seenCap={} rankLevel={} lowest={} pending={} nativeRequests={} stateFlags={:#x} "
                         "ownerFlags={:#x} ownerDesired={} visitBase={} visitBias={} visitSuppress={} visitDirect={} "
                         "objectLevel={} visitFloor={} answer={} pixels={:.0f} priority={:.4f}",
                tag, d.name, row.width, row.height, row.mips, row.format, row.current, row.planned, row.held, row.floor0,
                row.floor1, d.minLevel, d.seenCap, d.rankLevel, row.lowest, d.pending, d.nativeRequests, d.stateFlags,
                d.ownerFlags, d.ownerDesired, d.visit.base, d.visit.bias, d.visit.suppress, d.visit.direct,
                d.objectLevel, d.visitFloor, d.answer, d.pixels, d.priority);
        };
        for (const auto& row : barrels) log("barrel", row);
        for (const auto& row : held) log("held", row);
    }
}

namespace Lod
{
    bool Install()
    {
        if (g_installed.load()) {
            return true;
        }

        const auto getDesired = Addr::Resolve(Addr::GetDesiredDegradeLevel);
        const auto getTextureMax = Addr::Resolve(Addr::GetTextureMaxDegradeLevel);
        const auto updateRec = Addr::Resolve(Addr::UpdateRec);
        const auto updateAlwaysRender = Addr::Resolve(Addr::UpdateAlwaysRender);
        const auto textureFunctor = Addr::Resolve(Addr::TextureFunctor);
        const auto startRound = Addr::Resolve(Addr::StartTextureRound);
        g_worldRootCamera = reinterpret_cast<WorldRootCamera_t>(Addr::Resolve(Addr::WorldRootCamera));
        g_nativeTextureBytes = reinterpret_cast<const volatile std::uint64_t*>(Addr::Resolve(Addr::NativeTextureBytes));
        g_nativeUpgradeLimit = reinterpret_cast<volatile std::uint64_t*>(Addr::Resolve(Addr::NativeTextureUpgradeLimit));
        g_screenHeight = Addr::Resolve(Addr::GraphicsState);
        if (g_screenHeight) {
            g_screenHeight += Addr::kScreenHeightOffset;
        }

        if (!getDesired || !getTextureMax || !updateRec || !updateAlwaysRender || !textureFunctor ||
            !startRound || !g_worldRootCamera) {
            logger::error("Engine addresses missing for this runtime, texture LOD stays the game's own");
            return false;
        }

        const bool starfield = dtl::GetSettings().policy == static_cast<std::uint32_t>(dtl::DetailPolicy::Starfield);
        {
            const auto& settings = dtl::GetSettings();
            g_qualityGuard = starfield && settings.QualityGuard();
            g_timeline = settings.timeline;
            g_exact = settings.starfieldExact;
            g_nearDistance = static_cast<float>(settings.nearDistance);
            g_presentWaits = g_exact || settings.gpuFeedback || settings.meshStreaming;
            g_passthrough.store(settings.passthrough, std::memory_order_relaxed);
            g_configuredPassthrough = settings.passthrough;
            g_legacyPending = settings.legacyPending;
            // The limit follows the budget's readings, and a dry run applies
            // no limits. Neither runs in the test arms that leave the game
            // its own answers.
            g_steerUpgradeLimit = settings.steerUpgradeLimit && settings.budget && !settings.dryRun && !Passthrough();
            g_steerToTarget = g_steerUpgradeLimit && PluginIni::Read("Budget", "SteerToTarget", 0) != 0;
            if (g_steerToTarget) logger::info("Native upgrade limit steers to the target, not the line");
            g_caps = settings.maxSize.Any() && !Passthrough();
            g_noLowerRadius = Passthrough() ? 0 : settings.noLowerRadius;
            g_changeRate.Configure(Passthrough() ? 0 : settings.maxChangesPerSecond);
        }
        const auto version = REL::Module::get().version();
        const bool level4Proven = version == REL::Version{ 1, 10, 163, 0 } ||
            version == REL::Version{ 1, 10, 984, 0 } || version == REL::Version{ 1, 11, 240, 0 } ||
            version == REL::Version{ 1, 2, 72, 0 };
        g_sourceMaxLevel = level4Proven ? kSourceMaxLevel : kEngineMaxLevel;
        logger::info("Native texture target capacity: source={} legacy={} per-texture mip/dimension/BC-alignment guard=true",
            g_sourceMaxLevel, kEngineMaxLevel);
        if (starfield && !Addr::RendererDataSlot()) return false;

        const auto desiredCalls = CallsTo(getDesired);
        const auto updateCalls = CallsTo(updateRec);
        const auto roundCalls = CallsTo(startRound);
        const auto textureCalls = [&] {
            std::vector<std::uintptr_t> inside;
            for (const auto site : CallsTo(getTextureMax)) {
                if (Within(site, textureFunctor, Addr::kTextureFunctorSize)) {
                    inside.push_back(site);
                }
            }
            return inside;
        }();

        const auto objectInRec = FirstWithin(desiredCalls, updateRec, Addr::kUpdateRecSize);
        const auto objectInPass = FirstWithin(desiredCalls, updateAlwaysRender, Addr::kUpdateAlwaysRenderSize);
        const auto visitInRec = TextureVisitAfter(objectInRec);
        const auto visitInPass = TextureVisitAfter(objectInPass);
        const auto functorPrologue = FunctorPrologueSize(textureFunctor);

        // 12 and 11 calls on OG and NG, 2 and 1 in the texture functor, about
        // a dozen calls to UpdateRec. Anything far off means different code.
        const bool expected = desiredCalls.size() >= 8 && desiredCalls.size() <= 24 && objectInRec && objectInPass &&
                              !textureCalls.empty() && textureCalls.size() <= 4 && updateCalls.size() >= 3 &&
                              updateCalls.size() <= 48 && visitInRec && visitInPass &&
                              roundCalls.size() >= 1 && roundCalls.size() <= 8 && functorPrologue != 0;
        if (!expected) {
            logger::error("Unexpected code: {} level calls, {} texture calls, {} update calls, object calls {}/{}. "
                          "Texture LOD stays the game's own",
                desiredCalls.size(), textureCalls.size(), updateCalls.size(), objectInRec.has_value(),
                objectInPass.has_value());
            logger::error("Texture visit calls {}/{}, round calls {}", visitInRec.has_value(),
                visitInPass.has_value(), roundCalls.size());
            return false;
        }

        if (!TextureLifecycle::Install(&OnTextureBirth, &OnTextureRetire)) return false;
        if (starfield && (dtl::GetSettings().gpuFeedback || dtl::GetSettings().meshStreaming)) {
            if (GpuGather::Install(true, &ObserveDraw)) {
                if (dtl::GetSettings().meshStreaming) {
                    Fo4Mesh::Lifetime::Diagnostics::InitializeFromEnvironment();
                    g_meshObservations = true;
                    g_meshInstalled = Fo4Mesh::Install();
                }
                if (dtl::GetSettings().gpuFeedback)
                    TextureFeedback::Install(true, &FeedbackIdentity, &FeedbackDemand);
            }
        }
        if (starfield) FramePump::Start(&OnFrame); // Renderer slot validated before any patches.

        g_getDesiredDegradeLevel = reinterpret_cast<GetDesiredDegradeLevel_t>(getDesired);
        g_getTextureMaxDegradeLevel = reinterpret_cast<GetTextureMaxDegradeLevel_t>(getTextureMax);
        g_updateRec = reinterpret_cast<UpdateRec_t>(updateRec);
        g_startTextureRound = reinterpret_cast<StartTextureRound_t>(startRound);

        // Textures, then the scopes, then the objects that start writing
        // notes, so nothing reads a note before notes are scoped.
        auto& trampoline = F4SE::GetTrampoline();
        InstallFunctor(textureFunctor, functorPrologue);
        trampoline.write_call<6>(*visitInRec, Hook_ForEachTexture);
        trampoline.write_call<6>(*visitInPass, Hook_ForEachTexture);
        for (const auto site : roundCalls) {
            trampoline.write_call<5>(site, Hook_StartTextureRound);
        }
        for (const auto site : textureCalls) {
            trampoline.write_call<5>(site, Hook_TextureLevel);
        }
        for (const auto site : updateCalls) {
            trampoline.write_call<5>(site, Hook_UpdateRec);
        }
        for (const auto site : desiredCalls) {
            const bool object = site == *objectInRec || site == *objectInPass;
            trampoline.write_call<5>(site, object ? Hook_ObjectLevel : Hook_OtherLevel);
        }
        if (const auto minRoundMs = dtl::GetSettings().minRoundMs) InstallRoundThrottle(minRoundMs);

        if (starfield) std::thread(StarfieldWorker).detach();
        else std::thread(Planner).detach();

        g_installed.store(true);
        if (starfield) logger::info("Starfield texture planner: angular thresholds, frame-budgeted stages; "
            "native gather uses bounding-sphere proxy, UV uses material banks, constructor distance bias=1. "
            "Native suggestions have no physical-free credit; experimental GPU gather is reported separately");
        logger::info("Texture LOD installed: {} level calls, {} texture calls, {} update calls, {} round calls; "
                     "material UV tiling on, visibility {}",
            desiredCalls.size(), textureCalls.size(), updateCalls.size(), roundCalls.size(),
            REL::Module::IsVR() ? "conservative for VR" : "camera frustum");
        return true;
    }

    void LogSummary()
    {
        if (!g_installed.load()) {
            return;
        }

        std::size_t   tracked = 0;
        std::size_t   lowered = 0;
        std::uint64_t missing = 0;
        std::array<std::uint64_t, kSourceMaxLevel + 1> levels{};
        std::uint64_t otherLevels = 0, resident = 0;
        for (auto& shard : g_shards) {
            std::scoped_lock lock(shard.lock);
            tracked += shard.textures.size();
            for (const auto& [texture, entry] : shard.textures) {
                const auto level = entry.current;
                if (level < levels.size()) {
                    ++levels[level];
                } else {
                    ++otherLevels;
                }
                if (level < entry.mips)
                    resident += TextureFormat::ChainBytes(entry.format, entry.width, entry.height, level, entry.mips - level);
                if (level == 0 || level >= entry.mips) {
                    continue;
                }
                ++lowered;
                missing += TextureFormat::ChainBytes(entry.format, entry.width, entry.height, 0, level);
            }
        }

        if (dtl::GetSettings().timeline) {
            if (g_meshObservations) Fo4Mesh::LogSummary();
            const auto accounting = g_ledger.Snapshot();
            const auto feedback = TextureFeedback::Statistics();
            const auto gather = GpuGather::Statistics();
            logger::info("[lod-tl] backend=FO4 observationRound={} objectsMeasured={} textureAnswers={} "
                         "tracked={} lowered={} levels0={} levels1={} levels2={} levels3={} levels4={} otherLevels={} "
                         "estimatedMissingMipBytes={} observedLowerings={} observedRestores={} "
                         "policy={} rendererFrame={} plannerCycles={} acceptedSuggestions={} completedSuggestions={} "
                         "timedOutSuggestions={} pendingSuggestions={} requestedDecreaseBytes={} requestedIncreaseBytes={} "
                         "releasedAwaitingDestructionBytes={} peakPlanMicroseconds={} nativeBirths={} nativeRetirements={} "
                         "staleDemandSuggestions={} nativeTextureBytes={} nativeUpgradeLimitBytes={} nativeCountersKnown={} "
                         "gpuFeedbackEnabled={} gpuFeedbackReady={} gpuSubmitted={} gpuCompleted={} gpuExpired={} "
                         "gpuRejected={} gpuFailures={} gpuDemands={} gpuBytes={} gpuDroppedDraws={} "
                         "aabbUploads={} aabbCaptured={} aabbDraws={} aabbOtherView={} aabbMissing={} "
                         "gatherCameraCalls={} gatherMainCameras={} gatherRejectedCameras={} gatherInvalidCameras={} "
                         "gatherLightingCalls={} gatherDrawCalls={} gatherMissingView={} gatherStaleView={} "
                         "gatherNullPass={} gatherShaderSkinned={} gatherShaderTechnique={} gatherNullGeometry={} "
                         "gatherGeometryType={} gatherGeometrySkinned={} gatherMissingShape={} gatherMissingBuffer={} "
                         "gatherDescriptorMismatch={} gatherInvalidTransform={} gatherNullProperty={} gatherCallbackFailures={} "
                         "gatherCachedCalls={} gatherCachedPasses={} gatherInvalidCachedLists={} gatherImmediateCalls={} "
                         "gatherShaderType={} gatherMaterialType={} gatherDeformation={} "
                         "inheritedRoomObservations={} scopedNativeVisitors={} presentFramesSkipped={} qualityGuard={} "
                         "upgradeLimitWrites={} measuredKeepFreeBytes={} nearVisits={} upgradeHolds={} upgradeHold={}",
                g_round.load(std::memory_order_relaxed), g_objectsMeasured.load(std::memory_order_relaxed),
                g_texturesAnswered.load(std::memory_order_relaxed), tracked, lowered,
                levels[0], levels[1], levels[2], levels[3], levels[4], otherLevels, missing,
                g_observedLowerings.load(std::memory_order_relaxed),
                g_observedRestores.load(std::memory_order_relaxed), dtl::GetSettings().policy,
                g_frames.load(), g_cycles.load(), g_requests.load(), g_completedRequests.load(),
                g_cancelledRequests.load(), accounting.pendingOperations, accounting.requestedDecreaseBytes,
                accounting.requestedIncreaseBytes, accounting.releasedAwaitingDestructionBytes, g_peakPlanMicroseconds.load(),
                g_births.load(), g_retirements.load(), g_staleDemand.load(),
                g_nativeTextureBytes ? *g_nativeTextureBytes : 0,
                g_nativeUpgradeLimit ? *g_nativeUpgradeLimit : 0,
                g_nativeTextureBytes && g_nativeUpgradeLimit ? 1 : 0,
                feedback.enabled ? 1 : 0, feedback.ready ? 1 : 0, feedback.submitted, feedback.completed,
                feedback.expired, feedback.rejected, feedback.failures, feedback.delivered, feedback.bytes,
                feedback.droppedDraws, gather.uploads, gather.bounds, gather.accepted, gather.otherView, gather.missingBounds,
                gather.cameraCalls, gather.mainCameras, gather.rejectedCameras, gather.invalidCameras,
                gather.lightingCalls, gather.drawCalls, gather.missingView, gather.staleView,
                gather.nullPass, gather.shaderSkinned, gather.shaderTechnique, gather.nullGeometry,
                gather.geometryType, gather.geometrySkinned, gather.missingShape, gather.missingBuffer,
                gather.descriptorMismatch, gather.invalidTransform, gather.nullProperty, gather.callbackFailures,
                gather.cachedCalls, gather.cachedPasses, gather.invalidCachedLists, gather.immediateCalls,
                gather.shaderType, gather.materialType, gather.deformation,
                g_inheritedObservations.load(), g_scopedVisitors.load(), g_framesSkipped.load(), g_qualityGuard ? 1 : 0,
                g_upgradeLimitWrites.load(), dtl::budget::MeasuredKeepFree(), g_nearVisits.load(), g_upgradeHolds.load(),
                g_upgradeHold.load() ? 1 : 0);
            // What the last finished native round offered: the share of the
            // engine's texture memory this plugin can lower at all.
            const auto bank = (g_round.load(std::memory_order_relaxed) - 1) & 1;
            const auto count = [bank](Coverage c) { return g_coverageTextures[bank][c].load(std::memory_order_relaxed); };
            const auto mb = [bank](Coverage c) { return g_coverageBytes[bank][c].load(std::memory_order_relaxed) >> 20; };
            logger::info("[lod-cov] backend=FO4 offeredTextures={} offeredMB={} nativeFixedTextures={} nativeFixedMB={} "
                         "unmeasuredTextures={} unmeasuredMB={} noRendererVisits={} trackedResidentMB={} nativeTextureMB={}",
                count(kOffered), mb(kOffered), count(kNativeFixed), mb(kNativeFixed), count(kUnmeasured), mb(kUnmeasured),
                count(kNoRenderer), resident >> 20, (g_nativeTextureBytes ? *g_nativeTextureBytes : 0) >> 20);
            if (const auto host = Vram::ReadHostMemory(); host.valid)
                logger::info("[lod-mem] backend=FO4 privateMB={} workingSetMB={} availableMB={} trackedResidentMB={}",
                    host.privateBytes >> 20, host.workingSet >> 20, host.available >> 20, resident >> 20);
            static std::uint32_t holdReport = 0;  // the caller's thread only
            if (++holdReport % 5 == 0) LogHolds();
            return;
        }
        logger::info("Texture LOD: {} objects measured, {} texture answers, {} textures tracked, {} below full size "
                     "(~{} MB), native upgrade limit written {} times, {} visits within NoLowerRadius, raises held {} times",
            g_objectsMeasured.load(std::memory_order_relaxed), g_texturesAnswered.load(std::memory_order_relaxed),
            tracked, lowered, missing >> 20, g_upgradeLimitWrites.load(std::memory_order_relaxed),
            g_nearVisits.load(std::memory_order_relaxed), g_upgradeHolds.load(std::memory_order_relaxed));
    }

    bool SetActive(bool a_active)
    {
        if (!g_installed.load(std::memory_order_acquire) || g_failedSafe.load(std::memory_order_relaxed))
            return Active();
        const std::uint32_t next = a_active ? g_configuredPassthrough : 2;
        if (g_passthrough.exchange(next, std::memory_order_acq_rel) != next) {
            if (!a_active) g_releaseLimit.store(true, std::memory_order_release);
            logger::info("Texture LOD switched {}", a_active ? "on" : "off");
        }
        return Active();
    }

    bool Active()
    {
        return g_installed.load(std::memory_order_acquire) && Passthrough() != 2;
    }

    const char* PlanStep()
    {
        return g_planStep;
    }

    void SetBudgetMode(bool a_on)
    {
        if (g_budgetMode.exchange(a_on, std::memory_order_relaxed) == a_on) return;
        g_capGeneration.fetch_add(1, std::memory_order_relaxed);
        if (a_on) logger::info("4/6 GB budget mode on: every texture at most {} pixels", kBudgetModeEdge);
        else logger::info("4/6 GB budget mode off");
    }
}
