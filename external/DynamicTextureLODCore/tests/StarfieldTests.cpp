#include "core/FrameBudget.h"
#include "core/GpuProcessMemory.h"
#include "core/StarfieldPlanner.h"
#include "core/StarfieldPolicy.h"
#include "core/StreamingLedger.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); } } while(false)
    constexpr std::uint64_t MiB(std::uint64_t n) { return n << 20; }
    bool Near(float a, float b) { return std::abs(a - b) < 0.000001f; }

    dtl::PlanEntry Entry(std::uint64_t key, float rank, std::uint8_t current = 0,
        std::uint8_t lowest = 4, std::array<std::uint64_t, 5> bytes = { 100, 50, 25, 12, 6 })
    {
        dtl::PlanEntry result;
        result.key = key;
        result.priority = rank;
        result.seen = rank > 0;
        result.current = result.previous = current;
        result.lowest = lowest;
        result.bytes = bytes;
        result.generation = 7;
        result.resourceIdentity = key + 100;
        return result;
    }

    const dtl::Budget kRoom{ dtl::Mode::Room, 500, 2000, 1000, 1650 };
    const dtl::Budget kSwap{ dtl::Mode::Tight, 1700, 2000, 1000, 1650 };
    dtl::BudgetLines Lines(std::uint64_t up = 34, std::uint64_t down = 34)
    {
        dtl::BudgetLines lines;
        lines.upgradePerCycle = up;
        lines.minimumDecreasePerCycle = down;
        return lines;
    }
    struct Sink
    {
        std::vector<dtl::PlanRequest> accepted;
        std::uint64_t rejectKey = 0, staleKey = 0;
        bool completed = false;
        static dtl::RequestReceipt Call(void* ptr, const dtl::PlanRequest& request)
        {
            auto& self = *static_cast<Sink*>(ptr);
            if (request.identity.key == self.staleKey) return { dtl::RequestStatus::Stale, 0 };
            if (request.identity.key == self.rejectKey) return {};
            self.accepted.push_back(request);
            return { self.completed ? dtl::RequestStatus::Completed : dtl::RequestStatus::AcceptedPending, request.estimatedBytes };
        }
    };
    void Finish(dtl::StarfieldPlanSession& session, Sink& sink, std::size_t step = 100)
    {
        std::size_t calls = 0;
        while (!session.Step(Sink::Call, &sink, step) && ++calls < 10000) {}
        CHECK(calls < 10000);
        CHECK(session.Done());
    }

    void FinishMeshes(dtl::StarfieldPlanSession& session, Sink& textures, Sink& meshes, std::size_t step=100)
    {
        std::size_t calls=0;
        while (!session.Step(Sink::Call,&textures,step,Sink::Call,&meshes) && ++calls<10000) {}
        CHECK(calls<10000 && session.Done());
    }

    void MeshStages()
    {
        const auto derived=dtl::StarfieldMeshThresholds();
        for (std::uint8_t i=0;i<4;++i) {
            CHECK(Near(derived[i],dtl::kStarfieldMeshThresholds[i]));
            CHECK(dtl::StarfieldWantedLevel(derived[i],4,derived)==i);
            CHECK(dtl::StarfieldWantedLevel(std::nextafter(derived[i],0.0f),4,derived)==i+1);
        }
        dtl::StarfieldPlanSession session;
        dtl::StarfieldCycleInput input;
        Sink textures, meshes;
        // Non-power-of-two VB/IB totals; matching keys are separated by sink.
        const std::array bytes{std::uint64_t{900},std::uint64_t{613},std::uint64_t{179},std::uint64_t{0},std::uint64_t{0}};
        std::vector geometry{Entry(1,0.3f,2,2,bytes),Entry(2,0.2f,2,2,bytes)};
        std::vector images{Entry(1,0.2f,1)};
        session.Begin(images,kRoom,Lines(34),input,geometry);
        CHECK(session.Stage()==dtl::PlanStage::MeshUpgrade);
        FinishMeshes(session,textures,meshes,1);
        CHECK(meshes.accepted.size()==2); // bypass continues after combined allowance is exhausted
        CHECK(meshes.accepted[0].estimatedBytes==721 && meshes.accepted[1].estimatedBytes==721);
        CHECK(textures.accepted.empty()); // real accepted mesh spending leaves texture upgrade held
        CHECK(session.Result().meshRaised==1442 && session.Result().raised==0);
        CHECK(session.Result().meshUpgradesHeld==0 && session.Result().upgradesHeld==1);

        input.noMeshesUpgradeLimits=false;
        textures={}; meshes={};
        session.Begin(images,kRoom,Lines(34),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.size()==1 && session.Result().meshUpgradesHeld==1); // soft final overshoot only
        CHECK(textures.accepted.empty());

        // Rejected/stale requests do not spend a slice or fall through again.
        input.noMeshesUpgradeLimits=true;
        textures={}; meshes={}; meshes.rejectKey=1; meshes.staleKey=2;
        session.Begin(images,kRoom,Lines(34),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.empty() && session.Result().meshStaleRequests==1);
        CHECK(textures.accepted.size()==1 && textures.accepted[0].reason==dtl::RequestReason::Upgrade);
        CHECK(session.Result().meshRaised==0 && session.Result().meshUpgradesHeld==2);

        const dtl::Budget over{dtl::Mode::Over,2100,2000,1000,1650};
        geometry={Entry(1,0.2f,2,2,bytes),Entry(2,0.08f,0,2,bytes)};
        textures={}; meshes={};
        session.Begin({},over,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.size()==2 && session.Result().meshRaised==721);
        CHECK(session.Result().meshLowered==287 && session.Result().meshEmergency==0);
        CHECK(session.Result().meshDecreaseCharged==0 && session.Result().decreaseRemaining==100);
        CHECK(meshes.accepted[1].target==1); // emergency + bypass still follows screen-size desired level
        input.noMeshesUpgradeLimits=false;
        textures={}; meshes={};
        session.Begin({},over,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.size()==1 && meshes.accepted[0].identity.key==2);
        CHECK(meshes.accepted[0].target==2 && meshes.accepted[0].reason==dtl::RequestReason::MeshEmergency);
        CHECK(session.Result().meshRaised==0 && session.Result().meshEmergency==721);
        CHECK(session.Result().meshDecreaseCharged==721 && session.Result().decreaseRemaining==0);

        // Textures run before trailing mesh demotions; paid decrease means Reset.
        images={Entry(1,0)}; geometry={Entry(1,0,0,2,bytes)};
        textures={}; meshes={};
        session.Begin(images,kRoom,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(textures.accepted.size()==1 && meshes.accepted.empty());
        CHECK(session.Result().lowered==94 && session.Result().meshLowered==0);
        // Exhausting the texture list and balance without any payable donor
        // must still enter DegradeMeshes, including the zero-texture case.
        for (bool bypass : {false,true}) {
            input.noMeshesUpgradeLimits=bypass;
            textures={}; meshes={};
            session.Begin({},kSwap,Lines(),input,geometry);
            FinishMeshes(session,textures,meshes);
            CHECK(meshes.accepted.size()==1 && meshes.accepted[0].reason==dtl::RequestReason::MeshDegrade);
            CHECK(session.Result().meshLowered==721);
            CHECK(session.Result().decreaseRemaining==(bypass ? 700 : 0));
        }

        // pending/no-degrade and malformed capacity tables stay intact.
        geometry={Entry(1,0,0,2,bytes),Entry(2,0,0,2,bytes),Entry(3,0,0,2,{100,150,200,0,0})};
        geometry[0].pending=true; geometry[1].noDegrade=true;
        textures={}; meshes={};
        session.Begin({},kRoom,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.empty());

        // Identical allocation size need not imply identical mesh selection.
        geometry={Entry(1,0.08f,0,2,{100,100,20,0,0})};
        textures={}; meshes={}; meshes.completed=true;
        session.Begin({},kRoom,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.size()==1 && meshes.accepted[0].estimatedBytes==0);
        CHECK(session.MeshEntries()[0].current==1 && session.Result().meshAcceptedRequests==1);
        CHECK(session.Result().meshLowered==0 && session.Result().decreaseRemaining==34);
        geometry.assign(session.MeshEntries().begin(),session.MeshEntries().end());
        textures={}; meshes={}; meshes.completed=true;
        session.Begin({},kRoom,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.empty());

        // Refresh-all overrides rank but keeps source per-class/shared limits
        // when the bypass is false, including externally accepted carry-in.
        dtl::Budget unlimited; unlimited.osBudget=100;
        geometry={Entry(1,0,2,2,bytes),Entry(2,0,2,2,bytes)};
        input.noMeshesUpgradeLimits=false; input.meshUpgradeBytes=100;
        textures={}; meshes={};
        session.Begin({},unlimited,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.empty() && session.Result().meshSpendingCarryIn==100);
        input.meshUpgradeBytes=0;
        textures={}; meshes={};
        session.Begin({},unlimited,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.size()==1 && meshes.accepted[0].target==0);
        input.noMeshesUpgradeLimits=true;
        textures={}; meshes={};
        session.Begin({},unlimited,Lines(),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.size()==2);

        // Bounded visitation and partition invariance across both resource lists.
        geometry.clear(); images.clear();
        for (std::uint64_t key=1;key<=250;++key) geometry.push_back(Entry(key,0.2f,1,2,{2,1,0,0,0}));
        textures={}; meshes={};
        session.Begin(images,kRoom,Lines(1000),input,geometry);
        CHECK(!session.StepFor(Sink::Call,&textures,0.0,Sink::Call,&meshes));
        CHECK(meshes.accepted.size()==100);
        FinishMeshes(session,textures,meshes,1);
        CHECK(meshes.accepted.size()==250 && session.Result().meshRaised==250);
        Sink otherTextures,otherMeshes;
        session.Begin(images,kRoom,Lines(1000),input,geometry);
        FinishMeshes(session,otherTextures,otherMeshes,73);
        CHECK(otherMeshes.accepted.size()==meshes.accepted.size());
        for (std::size_t i=0;i<otherMeshes.accepted.size();++i)
            CHECK(otherMeshes.accepted[i].identity==meshes.accepted[i].identity);

        // Coarse-tail early stop is source behavior, including a lower entry
        // still needing an upgrade. Custom mesh thresholds reach this stage.
        input.meshThresholds={0.4f,0.2f,0.1f,0.05f};
        geometry={Entry(1,0.075f,3),Entry(2,0.06f,4)};
        textures={}; meshes={};
        session.Begin({},kRoom,Lines(1000),input,geometry);
        FinishMeshes(session,textures,meshes);
        CHECK(meshes.accepted.empty() && session.Result().meshUpgradesHeld==1);
        CHECK(session.MeshEntries()[1].current==4);

        // Stable mixed mesh demand cannot alternate directions across cycles.
        geometry={Entry(1,0.2f,2,2,bytes),Entry(2,0.08f,0,2,bytes),Entry(3,0.01f,0,2,bytes)};
        input={}; std::array<int,4> direction{};
        for (int cycle=0;cycle<8;++cycle) {
            textures={}; meshes={}; meshes.completed=true;
            session.Begin({},kRoom,Lines(),input,geometry);
            FinishMeshes(session,textures,meshes);
            for (const auto& request:meshes.accepted) {
                const int sign=request.target>request.current ? 1 : -1;
                CHECK(direction[request.identity.key]==0 || direction[request.identity.key]==sign);
                direction[request.identity.key]=sign;
            }
            geometry.assign(session.MeshEntries().begin(),session.MeshEntries().end());
            if (cycle==7) CHECK(meshes.accepted.empty());
        }
    }

    void NativeDistanceTerm()
    {
        // Native DPPS/COMISS/JB and MAXPS(-d,d)/SUBPS/DPPS/SQRTPS
        // evidence distinguishes the source from standard AABB distance.
        CHECK(dtl::StarfieldDistanceTerm({0,0,0}, {3,4,0}).value() == 0);
        CHECK(dtl::StarfieldDistanceTerm({4,0,0}, {3,4,0}).value() == 0);
        CHECK(Near(dtl::StarfieldDistanceTerm({5,0,0}, {3,4,0}).value(), std::sqrt(20.0f)));
        CHECK(dtl::StarfieldDistanceTerm({std::nextafter(5.0f,0.0f),0,0}, {3,4,0}).value() == 0);
        CHECK(Near(dtl::StarfieldDistanceTerm({-5,0,0}, {3,4,0}).value(), std::sqrt(20.0f)));
        CHECK(Near(dtl::StarfieldDistanceTerm({4,0,0}, {1,2,2}).value(), std::sqrt(17.0f)));
        CHECK(dtl::StarfieldDistanceTerm({3,4,0}, {0,0,0}).value() == 5);
        CHECK(dtl::StarfieldDistanceTerm({0,0,0}, {0,0,0}).value() == 0);
        CHECK(dtl::StarfieldDistanceTerm({6,8,0}, {3,4,0}).value() == 5);
        CHECK(dtl::StarfieldDistanceTerm({0,-6,8}, {0,3,4}).value() == 5);
        // Invalid geometry cannot masquerade as a zero-distance demand.
        CHECK(!dtl::StarfieldDistanceTerm({1,2,3}, {-1,2,3}));
        CHECK(!dtl::StarfieldDistanceTerm({std::numeric_limits<float>::quiet_NaN(),0,0}, {1,1,1}));
        CHECK(!dtl::StarfieldDistanceTerm({0,0,0}, {1,std::numeric_limits<float>::infinity(),1}));
        CHECK(!dtl::StarfieldDistanceTerm({std::numeric_limits<float>::max(),0,0}, {1,1,1}));
    }

    void AngularAndGpu()
    {
        CHECK(Near(dtl::StarfieldAngularSize(5, 100), 0.1f));
        CHECK(dtl::StarfieldAngularSize(5, 0.1f) == 10);
        CHECK(dtl::StarfieldAngularSize(-1, 10) == 0);
        CHECK(dtl::StarfieldTextureDemand(0.2f, 2, 4, 100, 12.5f) == 0.1f);
        CHECK(dtl::StarfieldTextureDemand(0.2f, 0, 4, 100, 12.5f) == 0.2f);
        CHECK(dtl::StarfieldTextureDemand(0.2f, 2, 4, 0, 12.5f) == 3);
        CHECK(Near(dtl::StarfieldTextureDemand(0.2f, 2, 4, 12.5f, 12.5f), 2.1f));
        CHECK(Near(dtl::StarfieldTextureDemand(0.2f, 2, 4, 12.5001f, 12.5f), 0.1f));
        CHECK(dtl::StarfieldTextureDemand(0.2f, 2, 4, -1, 12.5f) == 0.2f);
        CHECK(dtl::StarfieldTextureDemand(4, 0, 0, -1, 12.5f) == 3);
        CHECK(dtl::StarfieldTextureDemand(0.2f, 1, std::numeric_limits<float>::quiet_NaN(), 20, 12.5f) == 3);
        CHECK(dtl::StarfieldTextureRank(0.1f, false, 0) == 2.1f);
        CHECK(dtl::StarfieldTextureRank(0.1f, false, 1) == 0);
        CHECK(dtl::StarfieldTextureRank(0.1f, true, 2) == 0.05f);
        const std::array thresholds{ 0.1f, 0.05f, 0.025f, 0.0125f };
        for (std::uint8_t i = 0; i < thresholds.size(); ++i) {
            CHECK(dtl::StarfieldWantedLevel(thresholds[i], 4) == i);
            CHECK(dtl::StarfieldWantedLevel(std::nextafter(thresholds[i], 0.0f), 4) == i + 1);
        }
        CHECK(dtl::StarfieldWantedLevel(0, 2) == 2);
        CHECK(dtl::StarfieldGpuEncode(4).value() == 0xffffff00u);
        CHECK(dtl::StarfieldGpuEncode(8) == dtl::StarfieldGpuEncode(4));
        CHECK(dtl::StarfieldGpuEncode(-1).value() == 0);
        CHECK(dtl::StarfieldGpuEncode(1).value() == 0x3fffffc0u);
        CHECK(!dtl::StarfieldGpuEncode(std::numeric_limits<float>::quiet_NaN()));
        CHECK(dtl::StarfieldGpuDecode(dtl::StarfieldGpuEncode(1).value()) < 1.0f);
        CHECK(dtl::StarfieldGpuUvReduce(100, 2, 4, true).value() == 50);
        CHECK(dtl::StarfieldGpuUvReduce(100, 0, 4, false).value() == 100);
        CHECK(dtl::StarfieldGpuUvReduce(1, 0, 4, true).value() == 100000);
        CHECK(!dtl::StarfieldGpuUvReduce(1000000, 0, 4, true));
        CHECK(!dtl::StarfieldGpuUvReduce(0xffffffffu, 1, 1, true)); // uint->float rounds to 2^32
    }

    void SmoothingAndCounters()
    {
        dtl::FrameBudgetSmoother smoother;
        CHECK(smoother.Observe(1, MiB(8000)) == MiB(8000));
        CHECK(smoother.Observe(1, MiB(9000)) == MiB(8000));
        CHECK(smoother.Observe(2, MiB(9000)) == MiB(8010));
        CHECK(smoother.Observe(500, MiB(7000)) == MiB(8000));
        CHECK(smoother.Observe(499, MiB(8005)) == MiB(8005)); // distinct ID, not elapsed-frame interpolation
        smoother.Reset();
        CHECK(smoother.Observe(0, 0) == 0);
        CHECK(smoother.Observe(1, MiB(7000)) == MiB(7000));
        CHECK(dtl::CorrectCachedUsage(100, 50, 70) == 120);
        CHECK(dtl::CorrectCachedUsage(100, 50, 20) == 70);
        CHECK(dtl::CorrectCachedUsage(10, 50, 0) == 0);
        CHECK(dtl::CorrectCachedUsage(std::numeric_limits<std::uint64_t>::max() - 1, 0, 50) == std::numeric_limits<std::uint64_t>::max());
    }

    void LedgerProof()
    {
        dtl::StreamingLedger ledger;
        const dtl::TextureIdentity id{ 42, 1 }, replacement{ 42, 2 };
        const auto down = ledger.Request(id, 100, 25);
        CHECK(down != 0);
        CHECK(ledger.Request(id, 100, 50) == 0);
        CHECK(ledger.Pending(id));
        CHECK(!ledger.CompleteChange(down, replacement));
        CHECK(ledger.Snapshot().requestedDecreaseBytes == 75);
        CHECK(ledger.Snapshot().releasedAwaitingDestructionBytes == 0);
        CHECK(ledger.QueueRelease(id, 75, dtl::ReleaseEvidence::GpuWorkComplete) == 0);
        CHECK(ledger.QueueRelease(id, 75, dtl::ReleaseEvidence::Unknown) == 0);
        CHECK(ledger.CompleteChange(down, id));
        CHECK(!ledger.CompleteChange(down, id));
        CHECK(ledger.Snapshot().requestedDecreaseBytes == 0);
        CHECK(ledger.Snapshot().observedDestroyedBytes == 0);
        const auto release = ledger.QueueRelease(id, 75, dtl::ReleaseEvidence::EngineDeferredDestroyQueued);
        CHECK(release != 0);
        CHECK(ledger.Snapshot().releasedAwaitingDestructionBytes == 75);
        CHECK(!ledger.CompleteRelease(release, dtl::ReleaseEvidence::GpuWorkComplete));
        CHECK(ledger.CompleteRelease(release, dtl::ReleaseEvidence::EngineDestroyObserved));
        CHECK(!ledger.CompleteRelease(release, dtl::ReleaseEvidence::EngineDestroyObserved));
        CHECK(ledger.Snapshot().observedDestroyedBytes == 75);
        CHECK(ledger.Snapshot().releasedAwaitingDestructionBytes == 0);
        const auto up = ledger.Request(replacement, 25, 100);
        CHECK(ledger.Snapshot().requestedIncreaseBytes == 75);
        CHECK(ledger.CancelChange(up, replacement));
        CHECK(ledger.Snapshot().pendingOperations == 0);
        const auto revoked = ledger.QueueRelease(id, 50, dtl::ReleaseEvidence::EngineDeferredDestroyQueued);
        CHECK(ledger.RevokeRelease(revoked));
        CHECK(!ledger.RevokeRelease(revoked));
        CHECK(ledger.Snapshot().observedDestroyedBytes == 75);
    }

    void BudgetStages()
    {
        CHECK(dtl::BudgetLines{}.minimumDecreasePerCycle == MiB(34));
        dtl::StarfieldPlanSession session;
        CHECK(session.Done());
        CHECK(session.Stage() == dtl::PlanStage::Idle);
        Sink sink;
        std::vector entries{ Entry(1, 0), Entry(2, 0) };
        session.Begin(entries, kRoom, Lines());
        CHECK(!session.Step(Sink::Call, &sink, 0));
        Finish(session, sink);
        CHECK(session.Result().decreaseTarget == 34);
        CHECK(session.Result().lowered == 94); // soft final operation overshoots
        CHECK(sink.accepted.size() == 1 && sink.accepted[0].identity.key == 2);
        CHECK(session.Result().releasedAwaitingDestruction == 0);
        CHECK(session.Result().decreaseRemaining == 0);

        sink = {};
        entries = { Entry(1, 0.2f) };
        session.Begin(entries, kRoom, Lines());
        Finish(session, sink);
        CHECK(sink.accepted.empty()); // minimum target does not force arbitrary room-mode lowering
        CHECK(session.Result().decreaseRemaining == 34);

        sink = {};
        entries = { Entry(1, 0.2f, 4), Entry(2, 0.15f, 4) };
        auto small = kRoom;
        small.usage = small.ceiling - 10;
        session.Begin(entries, small, Lines());
        Finish(session, sink);
        CHECK(sink.accepted.size() == 1);
        CHECK(session.Result().raised == 94);
        CHECK(session.Result().upgradesHeld == 1);

        sink = {};
        entries = { Entry(1, 0.2f, 2), Entry(2, 0, 4) };
        session.Begin(entries, {}, Lines(1000));
        Finish(session, sink);
        CHECK(sink.accepted.size() == 2); // source refresh-all, including unseen
        CHECK(session.Result().lowered == 0);
        CHECK(session.Result().decreaseTarget == 0);
        for (const auto& request : sink.accepted) CHECK(request.target == 0);

        entries = { Entry(1, 0), Entry(2, 0) };
        const dtl::Budget over{ dtl::Mode::Over, 2100, 2000, 1000, 1650 };
        dtl::StarfieldCycleInput input;
        input.accounting.requestedDecreaseBytes = 1000;
        session.Begin(entries, over, Lines(), input);
        CHECK(session.Result().decreaseTarget == 100); // requested work cannot pay for current overage
        input.accounting.releasedAwaitingDestructionBytes = 70;
        session.Begin(entries, over, Lines(), input);
        CHECK(session.Result().decreaseTarget == 30);
        input.accounting.releasedAwaitingDestructionBytes = 100;
        session.Begin(entries, over, Lines(), input);
        sink = {};
        Finish(session, sink);
        CHECK(session.Result().decreaseTarget == 0);
        CHECK(sink.accepted.empty());
    }

    void SwapsAndBalance()
    {
        dtl::StarfieldPlanSession session;
        for (const std::uint64_t donated : { 40, 50, 51 }) {
            auto candidate = Entry(1, 0.2f, 1, 4, { 100, 50, 25, 12, 6 });
            auto donor = Entry(2, 0.06f, 0, 4, { donated + 30, 50, 30, 10, 1 });
            const std::array entries{ candidate, donor };
            session.Begin(entries, kSwap, Lines(34, 1000));
            Sink sink;
            Finish(session, sink);
            CHECK(session.Result().lowered == donated); // partial trades are not rolled back
            CHECK(session.Result().raised == (donated > 50 ? 50 : 0));
            CHECK(session.Result().swapped == (donated > 50));
        }
        const auto candidate = Entry(1, 0.3f, 3, 4, { 100, 70, 40, 10, 1 });
        const auto donorA = Entry(2, 0.2f, 1, 4, { 150, 100, 40, 10, 1 });
        const auto donorB = Entry(3, 0.15f, 1, 4, { 150, 100, 40, 10, 1 });
        std::vector entries{ candidate, donorA, donorB };
        Sink sink;
        session.Begin(entries, kSwap, Lines(34, 1000));
        Finish(session, sink, 1);
        CHECK(session.Result().balanceLowered == 120);
        CHECK(session.Result().balanceRaised == 90);
        CHECK(sink.accepted.back().identity.key == 1 && sink.accepted.back().target == 0);
        sink = {};
        sink.rejectKey = 3;
        session.Begin(entries, kSwap, Lines(34, 1000));
        Finish(session, sink);
        CHECK(session.Result().balanceLowered == 60);
        CHECK(session.Result().balanceRaised == 60); // fall back to level1 after rejected donor
        CHECK(sink.accepted.back().target == 1);
        sink = {};
        entries = { candidate, donorA };
        session.Begin(entries, kSwap, Lines(34, 1000));
        Finish(session, sink);
        CHECK(session.Result().balanceRaised == 60); // balance accepts exact equality
        entries[1].noDegrade = true;
        sink = {};
        session.Begin(entries, kSwap, Lines(34, 1000));
        Finish(session, sink);
        CHECK(sink.accepted.empty());

        // The candidate freezes at the first later less-degraded non-max
        // entry. A later, even more degraded candidate must not replace it.
        entries = { Entry(1, 0.4f, 2, 2, { 100, 60, 20, 5, 1 }),
            Entry(2, 0.3f, 1, 4, { 150, 100, 40, 10, 1 }),
            Entry(3, 0.2f, 3, 4, { 150, 100, 40, 10, 1 }),
            Entry(4, 0.1f, 1, 4, { 150, 100, 40, 10, 1 }) };
        sink = {};
        session.Begin(entries, kSwap, Lines(34, 1000));
        Finish(session, sink);
        CHECK(session.Result().balanceRaised == 80);
        CHECK(sink.accepted.back().identity.key == 1);
    }

    void SafetyAndResumption()
    {
        auto live = Entry(1, 0, 0);
        dtl::PlanRequest request{ { live.key, live.generation }, live.resourceIdentity, 0, 4, 94 };
        CHECK(dtl::MatchesRequest(live, request));
        ++live.generation;
        CHECK(!dtl::MatchesRequest(live, request));
        --live.generation;
        ++live.resourceIdentity;
        CHECK(!dtl::MatchesRequest(live, request));
        --live.resourceIdentity;
        live.pending = true;
        CHECK(!dtl::MatchesRequest(live, request));
        live.pending = false;
        live.noDegrade = true;
        CHECK(!dtl::MatchesRequest(live, request));

        dtl::StarfieldPlanSession session;
        std::vector entries{ Entry(1, 0), Entry(2, 0) };
        Sink sink;
        sink.staleKey = 2;
        session.Begin(entries, kRoom, Lines());
        Finish(session, sink);
        CHECK(session.Result().staleRequests == 1);
        CHECK(session.Result().lowered == 94 && sink.accepted[0].identity.key == 1);
        entries[0].pending = entries[1].pending = true;
        sink = {};
        session.Begin(entries, kRoom, Lines());
        Finish(session, sink);
        CHECK(sink.accepted.empty());

        entries.clear();
        for (std::uint64_t key = 1; key <= 250; ++key)
            entries.push_back(Entry(key, 0.2f, 1, 4, { 2, 1, 0, 0, 0 }));
        sink = {};
        session.Begin(entries, kRoom, Lines(1000));
        CHECK(!session.Step(Sink::Call, &sink, 100));
        CHECK(sink.accepted.size() == 100);
        CHECK(session.Stage() == dtl::PlanStage::Upgrade);
        CHECK(!session.Step(Sink::Call, &sink, 100));
        CHECK(sink.accepted.size() == 200);
        Finish(session, sink);
        CHECK(sink.accepted.size() == 250);
        Sink oneAtATime;
        dtl::StarfieldPlanSession other;
        other.Begin(entries, kRoom, Lines(1000));
        Finish(other, oneAtATime, 1);
        CHECK(other.Result().raised == session.Result().raised);
        CHECK(oneAtATime.accepted.size() == sink.accepted.size());
        for (std::size_t i = 0; i < sink.accepted.size(); ++i)
            CHECK(oneAtATime.accepted[i].identity.key == sink.accepted[i].identity.key);

        dtl::StarfieldCycleInput input;
        input.unseen = dtl::Unseen::Keep;
        entries = { Entry(1, 0, 1) };
        sink = {};
        session.Begin(entries, kRoom, Lines(), input);
        Finish(session, sink);
        CHECK(sink.accepted.empty());
        input.unseen = dtl::Unseen::UnderPressure;
        session.Begin(entries, kRoom, Lines(), input);
        Finish(session, sink);
        CHECK(sink.accepted.empty());
        session.Begin(entries, kSwap, Lines(), input);
        Finish(session, sink);
        CHECK(sink.accepted.size() == 1 && sink.accepted[0].target == 4);

        entries = { Entry(1, 2.0f, 1) };
        entries[0].seen = false; // source bias-zero exclusion from unseen decay
        input.unseen = dtl::Unseen::Lowest;
        sink = {};
        session.Begin(entries, kRoom, Lines(), input);
        Finish(session, sink);
        CHECK(sink.accepted.size() == 1 && sink.accepted[0].target == 0);
    }

    void SourceTunablesAndMeshCoupling()
    {
        CHECK(dtl::ValidStarfieldThresholds(dtl::kStarfieldTextureThresholds));
        auto custom=dtl::kStarfieldTextureThresholds;
        custom[1]=0.2f;
        CHECK(!dtl::ValidStarfieldThresholds(custom));
        custom={0.2f,0.1f,0.05f,0.025f};
        CHECK(dtl::ValidStarfieldThresholds(custom));
        CHECK(dtl::StarfieldWantedLevel(0.15f,4,custom)==1);
        auto malformed=custom;
        malformed[2]=std::numeric_limits<float>::quiet_NaN();
        CHECK(!dtl::ValidStarfieldThresholds(malformed));
        malformed[2]=-1;
        CHECK(!dtl::ValidStarfieldThresholds(malformed));

        dtl::StarfieldPlanSession session;
        dtl::StarfieldCycleInput input;
        input.textureThresholds=custom;
        std::vector entries{Entry(1,0.15f,0)};
        Sink sink;
        session.Begin(entries,kRoom,Lines(),input);
        Finish(session,sink);
        CHECK(sink.accepted.size()==1 && sink.accepted[0].target==1);
        input.textureThresholds=malformed;
        sink={};
        session.Begin(entries,kRoom,Lines(),input);
        Finish(session,sink);
        CHECK(sink.accepted.empty()); // invalid settings use verified defaults as a set

        // Ignore-budget mode is refresh-all with finite B/B/2B allowances.
        // One accepted request may overshoot, but later requests are held.
        entries={Entry(1,0.2f,1),Entry(2,0.1f,1),Entry(3,0,1)};
        const auto ignored=dtl::IgnoreBudget(dtl::Assess(Lines(),99,60));
        CHECK(ignored.mode==dtl::Mode::Unlimited && ignored.osBudget==60 && ignored.line==60);
        CHECK(ignored.target==60 && ignored.ceiling==60 && ignored.usage==99);
        input={}; sink={};
        session.Begin(entries,ignored,Lines(1),input);
        Finish(session,sink);
        CHECK(sink.accepted.size()==2 && session.Result().raised==100);
        CHECK(session.Result().upgradesHeld==1 && session.Result().decreaseTarget==0);
        CHECK(!session.Result().usedBudgetFallback);
        input.meshUpgradeBytes=100;
        sink={};
        session.Begin(entries,ignored,Lines(1),input);
        Finish(session,sink);
        CHECK(sink.accepted.size()==1 && session.Result().raised==50);
        input.meshUpgradeBytes=120;
        sink={};
        session.Begin(entries,ignored,Lines(1),input);
        Finish(session,sink);
        CHECK(sink.accepted.empty() && session.Result().upgradesHeld==3);
        input={}; sink={};
        session.Begin(entries,{},Lines(60),input);
        Finish(session,sink);
        CHECK(sink.accepted.size()==2 && session.Result().usedBudgetFallback);

        entries={Entry(1,0.2f,1),Entry(2,0.06f,0,4,{90,50,30,10,1})};
        input={}; input.meshUpgradeBytes=1000;
        sink={};
        session.Begin(entries,kRoom,Lines(),input);
        Finish(session,sink);
        CHECK(session.Result().raised==0 && !session.Result().swapped);
        CHECK(session.Result().traded==0 && session.Result().lowered==40);
        CHECK(sink.accepted.size()==1 && sink.accepted[0].target==1);
        // Shared allowance exhaustion alone must not activate texture trades.
        // The source tests the texture-specific allowance for swap mode.
        input={};
        auto small=kRoom; small.usage=small.ceiling-20;
        sink={};
        session.Begin(entries,small,Lines(),input);
        Finish(session,sink);
        CHECK(session.Result().raised==50 && !session.Result().swapped);
        input.noMeshesUpgradeLimits=false;
        sink={};
        session.Begin(entries,small,Lines(),input);
        Finish(session,sink);
        CHECK(session.Result().swapped && session.Result().traded==60);

        // Custom threshold0 also gates Balance's important prefix.
        entries={Entry(1,0.3f,3),Entry(2,0.15f,1,4,{150,100,40,10,1})};
        input={}; input.textureThresholds=custom;
        sink={}; sink.rejectKey=2;
        session.Begin(entries,kSwap,Lines(34,1000),input);
        Finish(session,sink);
        CHECK(session.Result().balanceRaised==0 && session.Result().balanceLowered==0);
    }

    void TerrainDemand()
    {
        const auto infinity = std::numeric_limits<float>::infinity();
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        struct Example { float distance, raw; std::uint8_t level; };
        // Independent native branch table, including exact upper boundaries.
        const std::array examples{
            Example{-1,1,0}, Example{0,1,0}, Example{1,0.105f,0},
            Example{12.5f,0.105f,0}, Example{13,0.095f,1}, Example{25,0.095f,1},
            Example{26,0.045f,2}, Example{50,0.045f,2}, Example{51,0.020f,3},
            Example{100,0.020f,3}, Example{101,0.0075f,4}};
        for (const auto& example : examples) {
            const auto value = dtl::StarfieldTerrainAngularDemand(example.distance);
            CHECK(value && Near(*value, example.raw));
            if (!value) continue;
            const auto demand = dtl::StarfieldTextureDemand(*value,1,1,example.distance,12.5f);
            CHECK(dtl::StarfieldWantedLevel(dtl::StarfieldTextureRank(demand,true,1),4) == example.level);
        }
        for (std::size_t i=0;i<4;++i) {
            const auto distance = dtl::kStarfieldTerrainDistances[i];
            const auto before = dtl::StarfieldTerrainAngularDemand(std::nextafter(distance,0.0f));
            const auto at = dtl::StarfieldTerrainAngularDemand(distance);
            const auto after = dtl::StarfieldTerrainAngularDemand(std::nextafter(distance,infinity));
            CHECK(before && at && after && *before == *at && *after < *at);
        }
        const auto far = dtl::StarfieldTerrainAngularDemand(100);
        CHECK(far && dtl::StarfieldWantedLevel(dtl::StarfieldTextureDemand(*far,2,2,100,12.5f),4) == 4);
        CHECK(far && dtl::StarfieldWantedLevel(dtl::StarfieldTextureRank(*far,true,0),4) == 0);
        // A nearer/shared ordinary use dominates the terrain suggestion.
        CHECK(far && dtl::StarfieldWantedLevel(std::max(*far,0.2f),4) == 0);
        CHECK(!dtl::StarfieldTerrainAngularDemand(nan));
        CHECK(!dtl::StarfieldTerrainAngularDemand(infinity));
        CHECK(!dtl::StarfieldTerrainAngularDemand(-infinity));
        CHECK(!dtl::StarfieldTerrainAngularDemand(1,{0,25,50,100}));
        CHECK(!dtl::StarfieldTerrainAngularDemand(1,{25,12.5f,50,100}));
        CHECK(!dtl::StarfieldTerrainAngularDemand(1,{12.5f,25,50,infinity}));
        CHECK(!dtl::StarfieldTerrainAngularDemand(1,dtl::kStarfieldTerrainDistances,{0.1f,0.05f,0.025f,0}));
        CHECK(Near(*dtl::StarfieldTerrainAngularDemand(200,{25,50,100,200},{0.2f,0.1f,0.05f,0.025f}),0.045f));
    }

    void StableDemandAndTimeSlice()
    {
        std::vector<dtl::PlanEntry> entries;
        const std::array ranks{0.2f,0.075f,0.03f,0.02f,0.0f};
        for (std::uint64_t key=1;key<=30;++key)
            entries.push_back(Entry(key,ranks[key%ranks.size()],key%2 ? 0 : 4));
        dtl::StarfieldPlanSession session;
        std::array<int,31> direction{};
        for (int cycle=0;cycle<100;++cycle) {
            Sink sink; sink.completed=true;
            session.Begin(entries,kRoom,Lines());
            Finish(session,sink);
            for (const auto& request : sink.accepted) {
                const int sign=request.target>request.current ? 1 : -1;
                CHECK(direction[request.identity.key]==0 || direction[request.identity.key]==sign);
                direction[request.identity.key]=sign;
            }
            entries.assign(session.Entries().begin(),session.Entries().end());
            if (cycle==99) CHECK(sink.accepted.empty());
        }
        for (const auto& entry : entries) {
            const auto wanted=dtl::StarfieldWantedLevel(entry.priority,entry.lowest);
            // Source's early-stop at the first settled wanted>2 entry can
            // leave later coarse upgrades held even with stable Room budget.
            CHECK(wanted<=2 ? entry.current==wanted : entry.current>=wanted);
        }

        entries.clear();
        for (std::uint64_t key=1;key<=250;++key) entries.push_back(Entry(key,0.2f,1,4,{2,1,0,0,0}));
        Sink sink;
        session.Begin(entries,kRoom,Lines(1000));
        CHECK(!session.StepFor(Sink::Call,&sink,0.0));
        CHECK(sink.accepted.size()==100); // zero time budget still guarantees one bounded chunk
        CHECK(!session.StepFor(Sink::Call,&sink,0.0));
        CHECK(sink.accepted.size()==200);
        CHECK(session.StepFor(Sink::Call,&sink,10.0));
        CHECK(sink.accepted.size()==250);
    }

    // Departures for engines that re-create a texture for every level change.
    // Every one is off at its default, so the source tests above still hold.
    void QualityGuardAndSmoothing()
    {
        dtl::StarfieldPlanSession session;
        const auto run = [&](std::vector<dtl::PlanEntry> entries, const dtl::Budget& budget,
            const dtl::StarfieldCycleInput& input = {}, std::uint64_t up = 1000) {
            Sink sink;
            session.Begin(entries, budget, Lines(up), input);
            Finish(session, sink);
            return sink.accepted;
        };
        const auto targetOf = [](const std::vector<dtl::PlanRequest>& requests, std::uint64_t key) {
            for (const auto& request : requests) if (request.identity.key == key) return static_cast<int>(request.target);
            return -1;
        };
        const dtl::Budget tight{ dtl::Mode::Tight, 1700, 2000, 1000, 1650 };
        const dtl::Budget over{ dtl::Mode::Over, 2100, 2000, 1000, 1650 };

        // Never more detail than the game's own level.
        auto entry = Entry(1, 0.5f, 2);
        entry.minLevel = 1;
        CHECK(targetOf(run({ entry }, kRoom), 1) == 1);
        entry.current = entry.previous = 0;  // already above it: lowered back to it
        CHECK(targetOf(run({ entry }, kRoom), 1) == 1);

        // On screen, never past what the screen can show.
        entry = Entry(1, 0.001f, 0);
        CHECK(targetOf(run({ entry }, kRoom), 1) == 4);
        entry.seenCap = 1;
        CHECK(targetOf(run({ entry }, kRoom), 1) == 1);
        entry.current = entry.previous = 3;  // seen again below the cap: raised at once
        CHECK(targetOf(run({ entry }, kRoom), 1) == 1);
        CHECK(targetOf(run({ entry }, over), 1) == 4);  // only an emergency goes past the cap

        // Out of view only briefly: held.
        dtl::StarfieldCycleInput input;
        input.unseenDelayMs = 3000;
        entry = Entry(1, 0.0f, 0);
        entry.unseenMs = 1000;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == -1);
        entry.unseenMs = 5000;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == 4);
        entry.unseenMs = dtl::kLongAgoMs;  // never seen
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == 4);
        input.unseen = dtl::Unseen::UnderPressure;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == -1);
        CHECK(targetOf(run({ entry }, tight, input), 1) == 4);

        // A lowering needs the rank well under its threshold; a raising never waits.
        input = {};
        entry = Entry(1, 0.09f, 0);
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == 1);
        input.hysteresis = 0.2f;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == -1);
        entry.priority = 0.07f;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == 1);
        entry = Entry(1, 0.11f, 1);
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == 0);

        // No lowering right after a change, unless over the line.
        input = {};
        input.degradeCooldownMs = 3000;
        entry = Entry(1, 0.001f, 0);
        entry.sinceChangeMs = 1000;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == -1);
        CHECK(targetOf(run({ entry }, over, input), 1) == 4);
        entry.sinceChangeMs = 4000;
        CHECK(targetOf(run({ entry }, kRoom, input), 1) == 4);

        // Lowerings already requested count against an overage.
        input = {};
        input.accounting.recentRequestedDecreaseBytes = 60;
        session.Begin(std::vector<dtl::PlanEntry>{ Entry(1, 0.0f), Entry(2, 0.0f) }, over, Lines(), input);
        CHECK(session.Result().decreaseTarget == 100);  // source: no credit
        input.creditPendingFrees = true;
        session.Begin(std::vector<dtl::PlanEntry>{ Entry(1, 0.0f), Entry(2, 0.0f) }, over, Lines(), input);
        CHECK(session.Result().decreaseTarget == 40);
        input.accounting.recentRequestedDecreaseBytes = 500;
        session.Begin(std::vector<dtl::PlanEntry>{ Entry(1, 0.0f), Entry(2, 0.0f) }, over, Lines(), input);
        CHECK(session.Result().decreaseTarget == 0);

        // Raisings still on their way count against the headroom.
        const dtl::Budget nearCeiling{ dtl::Mode::Tight, 1600, 2000, 1000, 1650 };
        input = {};
        input.accounting.requestedIncreaseBytes = 60;
        CHECK(targetOf(run({ Entry(1, 0.5f, 1) }, nearCeiling, input, 34), 1) == 0);
        input.headroomNetOfGrowth = true;
        CHECK(targetOf(run({ Entry(1, 0.5f, 1) }, nearCeiling, input, 34), 1) == -1);

        // The source stops upgrading at the first rank wanting level 3; a
        // visible texture held under its cap further down still comes back.
        input = {};
        auto capped = Entry(3, 0.013f, 3);
        capped.seenCap = 1;
        const std::vector<dtl::PlanEntry> list{ Entry(1, 0.5f, 0), Entry(2, 0.02f, 3), capped };
        CHECK(targetOf(run(list, kRoom, input), 3) == -1);
        input.fullUpgradeScan = true;
        CHECK(targetOf(run(list, kRoom, input), 3) == 1);

        // Swap donors on screen stop at their cap.
        input = {};
        auto donor = Entry(2, 0.06f, 1);
        CHECK(targetOf(run({ Entry(1, 0.5f, 1), donor }, tight, input, 34), 2) == 2);
        donor.seenCap = 1;
        CHECK(targetOf(run({ Entry(1, 0.5f, 1), donor }, tight, input, 34), 2) == -1);

        // One pending change per texture, found without a scan.
        dtl::StreamingLedger ledger;
        const dtl::TextureIdentity id{ 5, 1 };
        const auto ticket = ledger.Request(id, 100, 40);
        CHECK(ticket != 0 && ledger.Pending(id));
        CHECK(ledger.Request(id, 100, 30) == 0);
        CHECK(ledger.Snapshot().requestedDecreaseBytes == 60);
        CHECK(ledger.Snapshot().recentRequestedDecreaseBytes == 60);
        CHECK(ledger.Snapshot(std::chrono::milliseconds(-1)).recentRequestedDecreaseBytes == 0);
        CHECK(ledger.CompleteChange(ticket, id) && !ledger.Pending(id));
        CHECK(ledger.Request(id, 100, 30) != 0);

        // Read every 16 frames, the budget moves as far as read every frame.
        dtl::FrameBudgetSmoother smoother;
        CHECK(smoother.Observe(1, MiB(1000)) == MiB(1000));
        CHECK(smoother.Observe(17, MiB(800), 16) == MiB(840));
        CHECK(smoother.Observe(18, MiB(800)) == MiB(830));
        CHECK(smoother.Observe(30, MiB(800)) == MiB(820));  // the source adds no steps for skipped frames
    }
}

namespace
{
    // Skyrim Performance Solution's lessons: lower only under pressure, size
    // unseen textures by distance, refill only with room and calm, and treat
    // memory still being paged out as an emergency.
    void PressureFirst()
    {
        dtl::StarfieldPlanSession session;
        const auto run = [&](std::vector<dtl::PlanEntry> entries, const dtl::Budget& budget,
            const dtl::StarfieldCycleInput& input, std::uint64_t up = 1000) {
            Sink sink;
            session.Begin(entries, budget, Lines(up), input);
            Finish(session, sink);
            return sink.accepted;
        };
        const auto targetOf = [](const std::vector<dtl::PlanRequest>& requests, std::uint64_t key) {
            for (const auto& request : requests) if (request.identity.key == key) return static_cast<int>(request.target);
            return -1;
        };
        const dtl::Budget tight{ dtl::Mode::Tight, 1700, 2000, 1000, 1650 };
        const dtl::Budget over{ dtl::Mode::Over, 2100, 2000, 1000, 1650 };
        dtl::StarfieldCycleInput input;
        input.pressureOnly = true;

        // With room to spare nothing is lowered, however small it is on screen.
        CHECK(run({ Entry(1, 0.001f, 0) }, kRoom, input).empty());
        // Lowered under pressure earlier: held until refill allows it back,
        // then back to the game's own level and no further.
        auto held = Entry(2, 0.5f, 3);
        held.minLevel = 1;
        CHECK(run({ held }, kRoom, input).empty());
        input.refillBytes = 1000;
        CHECK(targetOf(run({ held }, kRoom, input), 2) == 1);
        // Refill fills only part of the room: three restores of 38 bytes each
        // against 60; the second overshoots the slice, the third waits.
        input.refillBytes = 60;
        auto second = held, third = held;
        second.key = 3; second.resourceIdentity = 103;
        third.key = 4; third.resourceIdentity = 104;
        CHECK(run({ held, second, third }, kRoom, input).size() == 2);
        // A sized texture held past what the screen shows comes back at once,
        // refill or not.
        input.refillBytes = 0;
        auto visible = Entry(5, 0.5f, 3);
        visible.seenCap = 1;
        CHECK(targetOf(run({ visible }, kRoom, input), 5) == 1);

        // Under pressure, by distance: out of view is sized like in view,
        // never dropped to its lowest just for being behind the camera.
        input = {};
        input.pressureOnly = true;
        input.unseen = dtl::Unseen::ByDistance;
        auto behind = Entry(6, 0.03f, 0);  // rank 0.03: level 2
        behind.seen = false;
        behind.unseenMs = dtl::kLongAgoMs;
        CHECK(targetOf(run({ behind }, tight, input), 6) == 2);
        behind.seenCap = 1;  // its size at its distance shows level 1
        CHECK(targetOf(run({ behind }, tight, input), 6) == 1);
        // ...and with room to spare, a sized unseen texture held past its cap comes back.
        behind.current = behind.previous = 3;
        CHECK(targetOf(run({ behind }, kRoom, input), 6) == 1);
        // The old rule drops it to its lowest.
        input.unseen = dtl::Unseen::UnderPressure;
        behind.current = behind.previous = 0;
        behind.seenCap = dtl::kMaxPlanLevel;
        CHECK(targetOf(run({ behind }, tight, input), 6) == 4);

        // Lowering waits after a raise, except over the line.
        input = {};
        input.pressureOnly = true;
        input.raiseCooldownMs = 30000;
        auto raised = Entry(7, 0.03f, 0);
        raised.sinceRaiseMs = 1000;
        CHECK(targetOf(run({ raised }, tight, input), 7) == -1);
        raised.sinceRaiseMs = 40000;
        CHECK(targetOf(run({ raised }, tight, input), 7) == 2);
        raised.sinceRaiseMs = 1000;
        CHECK(targetOf(run({ raised }, over, input), 7) == 4);

        // Paging out under the line is still an emergency, sized by what was
        // pushed out.
        dtl::Budget paging{ dtl::Mode::Over, 1900, 2000, 1000, 1650 };
        paging.overflowBytes = 80;
        Sink sink;
        session.Begin(std::vector{ Entry(8, 0.001f, 0) }, paging, Lines(), {});
        CHECK(session.Result().decreaseTarget == 80);
        Finish(session, sink);
        CHECK(targetOf(sink.accepted, 8) == 4);

        // Paged-out memory counts only while it is still rising.
        using dtl::gpumem::Sample;
        std::vector<Sample> history{ { 0, MiB(300), 0, true }, { 0, MiB(310), 5000, true }, { 0, MiB(345), 9000, true } };
        CHECK(dtl::gpumem::Rise(history, 10000) == MiB(45));
        CHECK(dtl::gpumem::Rising(history, MiB(32), 10000));
        CHECK(!dtl::gpumem::Rising(history, MiB(32), 3000));
        CHECK(!dtl::gpumem::Rising(history, 0, 10000));
        history.push_back({ 0, MiB(345), 20000, true });  // flat for 11 s
        history.push_back({ 0, 0, 21000, false });         // an invalid sample counts for nothing
        CHECK(dtl::gpumem::Rise(history, 10000) == 0);
        CHECK(dtl::gpumem::Rise(history, 30000) == MiB(45));
        CHECK(dtl::gpumem::Rise(std::vector<Sample>{}, 10000) == 0);
    }

    // Each change is a re-create on the engine's creation thread: a lowering
    // that frees less than the minimum is skipped, in an emergency too, and
    // the next candidate pays instead. Off by default.
    void MinimumLowering()
    {
        dtl::StarfieldPlanSession session;
        const dtl::Budget over{ dtl::Mode::Over, 2100, 2000, 1000, 1650 };
        const std::vector<dtl::PlanEntry> entries{ Entry(1, 0), Entry(2, 0, 0, 4, { 10, 5, 2, 1, 0 }) };
        dtl::StarfieldCycleInput input;
        Sink sink;
        session.Begin(entries, over, Lines(), input);
        Finish(session, sink);
        CHECK(sink.accepted.size() == 2);
        input.minLoweringBytes = 50;
        session.Begin(entries, over, Lines(), input);
        sink = {};
        Finish(session, sink);
        CHECK(sink.accepted.size() == 1 && sink.accepted[0].identity.key == 1 &&
              sink.accepted[0].reason == dtl::RequestReason::Emergency && sink.accepted[0].target == 4);
        CHECK(session.Result().lowered == 94);
    }
}

int main()
{
    MinimumLowering();
    PressureFirst();
    NativeDistanceTerm();
    TerrainDemand();
    AngularAndGpu();
    SmoothingAndCounters();
    LedgerProof();
    BudgetStages();
    SwapsAndBalance();
    SafetyAndResumption();
    SourceTunablesAndMeshCoupling();
    StableDemandAndTimeSlice();
    MeshStages();
    QualityGuardAndSmoothing();
    std::printf("Starfield core tests: %d checks, %d failures\n", checks, failures);
    return failures;
}
