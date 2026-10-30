#include "core/Settings.h"
#include "core/BudgetMode.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <sstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

// No adapter, GPU, worker thread or game is used by this integration test.
namespace dtl::vram {
    Reading sample;
    bool frameSampling{};
    void SetFrameSampling(bool enabled) { frameSampling=enabled; }
    Reading SampleFrame(std::uint64_t frame) { auto result=sample; result.frame=frame; result.frameSample=true; return result; }
}

namespace {
    int checks{}, calls{}, meshCalls{};
    void Check(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
    std::uint64_t MiB(std::uint64_t value) { return value<<20; }
    void Load(const std::filesystem::path& path,const char* text) {
        { std::ofstream out(path); out<<text; }
        dtl::LoadSettings(path.string().c_str());
    }
    auto Entries() {
        std::array<dtl::PlanEntry,2> entries{};
        for (std::size_t i=0;i<entries.size();++i) {
            auto& e=entries[i];
            e.key=i+1; e.priority=0; e.seen=false; e.current=e.previous=1; e.lowest=4;
            e.bytes={MiB(100),MiB(50),MiB(25),MiB(12),MiB(6)};
        }
        return entries;
    }
    dtl::RequestReceipt Accept(void*,const dtl::PlanRequest& request) {
        ++calls;
        return {dtl::RequestStatus::AcceptedPending,request.estimatedBytes};
    }
    dtl::RequestReceipt AcceptMesh(void*,const dtl::PlanRequest& request) {
        ++meshCalls;
        return {dtl::RequestStatus::AcceptedPending,request.estimatedBytes};
    }
    void Finish(dtl::StarfieldPlanSession& session) {
        int iterations{};
        while (!dtl::budget::StepPlan(session,Accept,nullptr) && ++iterations<1000) {}
        Check(session.Done(),"bounded session terminates");
    }
}

int main() {
    const auto folder=std::filesystem::temp_directory_path()/
        ("dtl-core-settings-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(folder);
        const auto ini=folder/"settings.ini";
        Load(ini,"[LOD]\nPolicy=0\n[Budget]\nEnabled=1\n");
        Check(dtl::GetSettings().policy==0,"legacy policy remains selectable");
        Check(!dtl::GetSettings().gpuFeedback && !dtl::GetSettings().meshStreaming,"experimental paths default off");
        Check(dtl::GetSettings().sourceTextureThresholds==dtl::kStarfieldTextureThresholds,"absent source fields default correctly");
        Check(dtl::GetSettings().sourceMaximumIncreaseMB==34 && dtl::GetSettings().sourceTimeSliceMs==10,"source defaults");
        Check(dtl::budget::FromIni().meshThresholds==dtl::kStarfieldMeshThresholds,"derived source mesh defaults");
        Load(ini,"[LOD]\nPolicy=1\n[Budget]\nEnabled=0\n[Source]\nTextureDegradeScreenSize0=0.2\n"
             "TextureDegradeScreenSize1=0.1\nTextureDegradeScreenSize2=0.05\nTextureDegradeScreenSize3=0.025\n"
             "MaximumIncreasePerIterationMB=1\nMinimumDecreasePerIterationMB=2\nNoMeshesUpgradeLimits=0\n"
             "MeshLODScreenSize0=0.4\nMeshLODScreenSize1=0.2\nMeshLODScreenSize2=0.1\nMeshLODScreenSize3=0.05\n"
             "MeshLODFadeScreenSizePercent=0.125\nMeshLODScreenSizePercentStreamingBias=0.375\n"
             "TimeSliceMilliseconds=0\n[Experimental]\nGpuFeedback=1\nMeshStreaming=1\n");
        auto settings=dtl::budget::FromIni();
        Check(dtl::GetSettings().gpuFeedback && dtl::GetSettings().meshStreaming,"experimental switches parsed");
        Check(settings.textureThresholds[0]==0.2f && settings.textureThresholds[3]==0.025f,"thresholds reach budget settings");
        Check(settings.lines.upgradePerCycle==MiB(1) && settings.lines.minimumDecreasePerCycle==MiB(2),"MiB source controls reach planner");
        Check(!settings.noMeshesUpgradeLimits && settings.timeSliceMs==0,"bypass and scheduler controls reach planner");
        Check(settings.meshThresholds==std::array<float,4>{0.2f,0.1f,0.05f,0.025f},"base mesh sizes, fade and bias reach planner");
        dtl::budget::Configure(settings,nullptr);
        Check(dtl::vram::frameSampling,"ignored Policy1 budget still samples actual frames");
        dtl::vram::sample={MiB(70),MiB(60)};
        dtl::budget::OnFrame(1);
        const auto ignored=dtl::budget::Current();
        Check(ignored.mode==dtl::Mode::Unlimited && ignored.osBudget==MiB(60),"disabled limits preserve GPU budget reading");
        Check(ignored.line==MiB(60) && ignored.usage==MiB(70),"ignore metadata keeps raw line and observed usage");
        dtl::StarfieldPlanSession session;
        dtl::budget::BeginPlan(session,Entries());
        calls=0; Finish(session);
        Check(calls==2 && session.Result().raised==MiB(100),"ignore uses GPU B, not one-MiB source slice");
        Check(!session.Result().usedBudgetFallback,"reading is not claimed as fallback");

        Load(ini,"[LOD]\nPolicy=1\n[Budget]\nEnabled=1\nDryRun=1\n[Source]\nMaximumIncreasePerIterationMB=1\n");
        settings=dtl::budget::FromIni();
        dtl::budget::Configure(settings,nullptr);
        dtl::budget::OnReading({MiB(1000),MiB(4000),1,true});
        auto images=Entries(), meshes=Entries();
        for (auto* list : {&images,&meshes}) for (auto& entry:*list) { entry.priority=.2f; entry.seen=true; }
        std::ostringstream diagnostic;
        auto previousLogger=spdlog::default_logger();
        auto diagnosticLogger=std::make_shared<spdlog::logger>("test",std::make_shared<spdlog::sinks::ostream_sink_mt>(diagnostic));
        spdlog::set_default_logger(diagnosticLogger);
        calls=meshCalls=0;
        dtl::budget::BeginPlan(session,images,{},0,meshes);
        Check(calls==0 && meshCalls==0,"hypothetical plan never invokes actual resource callbacks");
        while (!dtl::budget::StepPlan(session,Accept,nullptr,AcceptMesh,nullptr)) {}
        spdlog::set_default_logger(previousLogger);
        Check(calls==2 && session.Result().raised==MiB(100),"dry run preserves real B for actual refresh-all");
        Check(meshCalls==2 && session.Result().meshRaised==MiB(100),"actual dry-run refresh includes real mesh sink");
        Check(diagnostic.str().find("hypothetical down 0 MB, up 0 MB")!=std::string::npos &&
              diagnostic.str().find("mesh down/up 0/100 MB")!=std::string::npos,
              "hypothetical mesh spending holds textures without affecting actual refresh");
        Check(images[0].current==1 && meshes[0].current==1 && !images[0].pending && !meshes[0].pending,
              "both hypothetical and actual sessions own value snapshots");
        Check(!session.Result().usedBudgetFallback,"dry run does not lose observed budget");

        Load(ini,"[Source]\nTextureDegradeScreenSize1=8\nTimeSliceMilliseconds=200\n"
             "MaximumIncreasePerIterationMB=-3\n[Experimental]\nMeshStreaming=0\n");
        Check(dtl::GetSettings().sourceTextureThresholds==dtl::kStarfieldTextureThresholds,"invalid ordered tuple resets as set");
        // A zero slice would leave swap mode on for good: one MiB is the floor.
        Check(dtl::GetSettings().sourceTimeSliceMs==100 && dtl::GetSettings().sourceMaximumIncreaseMB==1,"source controls bounded");
        Check(!dtl::GetSettings().meshStreaming && !dtl::GetSettings().gpuFeedback,"reload defaults do not retain experiment opt-ins");
        Load(ini,"[Source]\nMeshLODScreenSize0=0.8\nMeshLODFadeScreenSizePercent=0.75\nMeshLODScreenSizePercentStreamingBias=0.5\n");
        Check(dtl::GetSettings().sourceMeshScreenSizes==dtl::kStarfieldMeshScreenSizes &&
              dtl::budget::FromIni().meshThresholds==dtl::kStarfieldMeshThresholds,"invalid fade/bias restores entire mesh tuple");
        Load(ini,"[Source]\nMeshLODScreenSize3=10\n");
        Check(dtl::budget::FromIni().meshThresholds==dtl::kStarfieldMeshThresholds,"invalid mesh size ordering restores source defaults");
        Load(ini,"[Source]\nMeshLODFadeScreenSizePercent=0.8\nMeshLODScreenSizePercentStreamingBias=0.2\n");
        Check(dtl::budget::FromIni().meshThresholds==dtl::kStarfieldMeshThresholds &&
              dtl::GetSettings().sourceMeshFadePercent==0.2f,"derived float threshold guards keep backend and planner tuples identical");

        // The reserve from measured jumps, the native upgrade limit and the
        // optional size caps.
        Load(ini,"[Budget]\n");
        Check(dtl::GetSettings().autoTarget && dtl::GetSettings().steerUpgradeLimit && dtl::budget::FromIni().autoTarget,
              "reserve from jumps and the native limit default on");
        Check(!dtl::GetSettings().maxSize.Any(),"no size caps by default");
        Load(ini,"[Budget]\nAutoTarget=0\nSteerUpgradeLimit=0\n[MaxSize]\nDiffuse=1024\nNormal=99999\n"
             "[MaxSizeFolders]\n\\Actors\\=0\nNormal:Landscape=512\nBogus:x=256\n");
        const auto& caps=dtl::GetSettings().maxSize;
        Check(!dtl::GetSettings().autoTarget && !dtl::GetSettings().steerUpgradeLimit && !dtl::budget::FromIni().autoTarget,
              "both switch off");
        Check(caps.edge[0]==1024 && caps.edge[1]==16384 && caps.edge[2]==0,"kind sizes read and bounded");
        Check(caps.rules.size()==2 && caps.rules[0].text=="\\actors\\" && caps.rules[0].kind==-1 &&
              caps.rules[1].kind==1 && caps.rules[1].text=="landscape" && caps.rules[1].edge==512,
              "folder rules in file order, unknown kinds dropped");
        Check(dtl::cap::EdgeFor(caps,"textures\\actors\\a_d.dds")==0 && dtl::cap::EdgeFor(caps,"textures\\b_d.dds")==1024 &&
              dtl::cap::EdgeFor(caps,"textures\\landscape\\c_n.dds")==512,"rules reach the limits");
        Load(ini,"[MaxSize]\nDiffuse=1024\n[LOD]\nStarfieldExact=1\n");
        Check(!dtl::GetSettings().maxSize.Any() && !dtl::GetSettings().autoTarget && !dtl::GetSettings().steerUpgradeLimit,
              "the exact comparison clears all three");
        Load(ini,"[MaxSize]\nDiffuse=1024\n[Experimental]\nPassthrough=1\n");
        Check(!dtl::GetSettings().maxSize.Any() && !dtl::GetSettings().steerUpgradeLimit && dtl::GetSettings().autoTarget,
              "test arms that leave the game its own answers change nothing else in the game");
        Load(ini,"[MaxSize]\nDiffuse=1024\n");
        Check(dtl::GetSettings().maxSize.Any() && dtl::GetSettings().maxSize.rules.empty(),"a reload starts from no rules");
        // Fallout 76's mip skip by format, no-lower radius, and the speed hold.
        Load(ini,"");
        Check(!dtl::GetSettings().maxSize.skip.Any() && dtl::GetSettings().maxSize.skip.minDimension==1024 &&
              dtl::GetSettings().noLowerRadius==0 && dtl::GetSettings().upgradeHoldSpeed==600,
              "no skip, no radius, the game's hold speed by default");
        Load(ini,"[MipSkip]\nBC1=1\nBC3=9\nBC5=0\nMinDimension=2048\n[LOD]\nNoLowerRadius=100\n"
             "[Smoothing]\nUpgradeHoldSpeed=900\n");
        const auto& skip=dtl::GetSettings().maxSize.skip;
        Check(skip.levels[0]==1 && skip.levels[1]==3 && skip.levels[3]==0 && skip.minDimension==2048 &&
              dtl::GetSettings().maxSize.Any(),"skips read and bounded, and count as a cap");
        Check(dtl::GetSettings().noLowerRadius==100 && dtl::GetSettings().upgradeHoldSpeed==900,"radius and hold speed read");
        Load(ini,"[MipSkip]\nBC1=1\n[LOD]\nNoLowerRadius=100\nStarfieldExact=1\n");
        Check(!dtl::GetSettings().maxSize.Any() && dtl::GetSettings().noLowerRadius==0 &&
              dtl::GetSettings().upgradeHoldSpeed==0,"the exact comparison clears them");
        Load(ini,"[MipSkip]\nBC1=1\n[Experimental]\nPassthrough=1\n");
        Check(!dtl::GetSettings().maxSize.Any(),"passthrough arms skip nothing");
        // Pacing for engines that re-create a texture for every change.
        Load(ini,"");
        Check(dtl::GetSettings().minLoweringKB==0 && dtl::GetSettings().maxChangesPerSecond==0 &&
              dtl::budget::FromIni().minLoweringBytes==0,"no pacing by default");
        Load(ini,"[Smoothing]\nMinLoweringKB=512\nMaxChangesPerSecond=999999\n");
        Check(dtl::GetSettings().minLoweringKB==512 && dtl::GetSettings().maxChangesPerSecond==100000 &&
              dtl::budget::FromIni().minLoweringBytes==(512u<<10),"pacing read, bounded and passed to the planner");
        Load(ini,"[Smoothing]\nMinLoweringKB=512\nMaxChangesPerSecond=120\n[LOD]\nStarfieldExact=1\n");
        Check(dtl::GetSettings().minLoweringKB==0 && dtl::GetSettings().maxChangesPerSecond==0,
              "the exact comparison clears pacing");
        // The shipped default file is the documented defaults.
        Load(ini,"");
        std::filesystem::remove(ini);
        dtl::LoadSettings(ini.string().c_str());
        Check(std::filesystem::exists(ini) && dtl::GetSettings().autoTarget && dtl::GetSettings().steerUpgradeLimit &&
              !dtl::GetSettings().maxSize.Any() && dtl::GetSettings().targetMB==896 &&
              dtl::GetSettings().upgradeHoldSpeed==600 && dtl::GetSettings().noLowerRadius==0 &&
              dtl::GetSettings().maxSize.skip.minDimension==1024,"the written default file reads back");
        std::filesystem::remove_all(folder);
        std::cout<<checks<<" settings/budget integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored; std::filesystem::remove_all(folder,ignored);
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
