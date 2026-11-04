#include "InstrumentProcessor.h"
#include "DrumBundle.h"
#include <array>
#include <iostream>

int bundleHostRecallRed()
{
    using namespace airack;
    using namespace airack::drums;
    InstrumentProcessor original;
    ProjectDocument project;
    std::uint64_t edit = 0;
    if (!original.tryProjectSnapshot(project,edit)) return 2;
    juce::MemoryBlock compact;
    if (!encodeProjectState(project,compact)) return 2;
    InstrumentProcessor numeric;
    if (!numeric.restoreProjectState(compact.getData(),int(compact.getSize()))) return 2;
    const std::array<float,4> pcm { .25f, -.125f, .0625f, 0.f };
    AssetPtr asset;
    AssetError ae;
    if (!makeSampleAsset(pcm,1,asset,ae)) return 2;
    DrumSlots slots {};
    slots[0][0].asset=asset->id();
    slots[0][0].name="owned marker";
    DrumDocumentPtr document;
    DrumError de;
    if (!makeDrumDocument(slots,document,de)) return 2;
    const std::array<AssetPtr,1> assets {asset};
    DrumBundlePtr bundle;
    const auto input=std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize());
    if (!makeDrumBundle(document,input,assets,bundle,de)) return 2;
    juce::MemoryBlock bytes;
    if (!encodeDrumBundle(*bundle,bytes,de)) return 2;
    InstrumentProcessor restored;
    const bool accepted=restored.restoreProjectState(bytes.getData(),int(bytes.getSize()));
    std::cout << "valid_bundle_host_restore=" << accepted << '\n';
    if (!accepted) {
        std::cerr << "FAIL self_contained_bundle_host_restore_accepts_valid_closed_assets\n";
        return 1;
    }
    return 0;
}

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <windows.h>
#include <dbghelp.h>
#endif
#ifndef NDEBUG
namespace airack {
struct RoutedDrumTestAccess {
    static auto snapshot(const RoutedSynthEngine& e){return e.drumTestSnapshot();}
    static void workerHook(RoutedSynthEngine& e,void* context,void (*hook)(void*,unsigned)){e.setDrumSourceBorrowTestHook(context,hook);}
    static bool prepareSpace(RoutedSynthEngine& e,const space::SpaceActivationOptions& options){return e.prepareWithPipelineLaunch(48000,512,RenderQuality::realtime,{},nullptr,options);}
};
struct InstrumentDrumTestAccess {
    using Point=InstrumentProcessor::DrumTestPoint;
    using History=InstrumentProcessor::DrumHistoryDebugView;
    struct State {
        ProjectDocument project;drums::DrumSlots slots;
        History history;
        std::array<const drums::SampleAsset*,64> owners {};
        drums::TargetTokens targets {};
        std::array<std::uint64_t,65> publications {};
        std::array<std::uint64_t,3> tickets {},reservations {};
        std::array<drums::AssetId,3> candidates {};
        std::array<bool,3> running {},cancelled {};
        const drums::BorrowedPublication* published=nullptr;
        std::uint64_t generation=0,source=0,acknowledged=0,edit=0,reset=0,context=0,targetEpoch=0;
        std::uint64_t retained=0,reserved=0,workspace=0,lifecycle=0;
        ProjectDocument groupBaseline;std::uint64_t groupEdit=0;bool groupDirty=false,valid=false;
        bool operator==(const State&) const=default;
    };
    static std::unique_ptr<State> snapshot(InstrumentProcessor& p){
        auto out=std::make_unique<State>();const std::lock_guard lock(p.restoreMutex);
        out->valid=p.tryProjectSnapshotOwned(out->project,out->edit);out->slots=p.drumOwnership->current_->document->slots();
        p.drumHistoryDebugSnapshot(out->history);auto& state=*p.drumOwnership;
        for(std::size_t n=0;n<64;++n){out->owners[n]=state.current_->owners[n].get();out->targets[n]=p.drumTargetTokens[n].load();}
        for(std::size_t n=0;n<65;++n)if(state.publications_[n])out->publications[n]=state.publications_[n]->borrowed.generation;
        for(std::size_t n=0;n<3;++n){const auto& slot=state.preparations_[n];out->tickets[n]=slot.ticket;out->reservations[n]=slot.decoded;
            out->running[n]=slot.running;out->cancelled[n]=slot.cancelled;if(slot.asset)out->candidates[n]=slot.asset->id();}
        out->published=state.published_.load();out->generation=state.current_->borrowed.generation;
        out->source=p.structuralGeneration.load();out->acknowledged=p.drumAcknowledgedSource.load();out->reset=p.resetGeneration.load();
        out->context=p.aiContextRevision;out->targetEpoch=p.drumTargetEpoch;out->retained=state.retainedBytes();
        out->reserved=state.reservedDecoded_;out->workspace=state.reservedWorkspace_;out->lifecycle=state.lifecycle_;
        out->groupBaseline=p.groupControl->committed;out->groupEdit=p.groupControl->committedEdit;out->groupDirty=p.groupControl->dirty;
        return out;
    }
    static std::unique_ptr<History> history(InstrumentProcessor& p){auto out=std::make_unique<History>();const std::lock_guard lock(p.restoreMutex);p.drumHistoryDebugSnapshot(*out);return out;}
    static void drain(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.drainDrumFeedbackOwned();}
    static void clear(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.clearHistoryOwned();}
    static bool retained(InstrumentProcessor& p,const juce::UndoableAction* action,bool undo){const std::lock_guard lock(p.restoreMutex);return p.replayRetainedOwnedAction(action,undo);}
    static bool insert(InstrumentProcessor& p,std::unique_ptr<juce::UndoableAction> action){const std::lock_guard lock(p.restoreMutex);return p.performHistoryAction(std::move(action));}
    static void hook(InstrumentProcessor& p,void* context,void (*callback)(void*,Point)){const std::lock_guard lock(p.restoreMutex);p.drumTestContext=context;p.drumTestHook=callback;}
    static bool recordAlive(InstrumentProcessor& p,std::uint64_t generation){const std::lock_guard lock(p.restoreMutex);
        for(const auto& record:p.drumOwnership->publications_)if(record&&record->borrowed.generation==generation)return true;return false;}
    using PreviewOwners=drums::DrumOwnership::Owners;
    using PreviewStatus=drums::DrumOwnership::AiImageStatus;
    static bool previewRegister(drums::DrumOwnership& state,drums::AssetPtr& owner){return state.registerAsset(owner);}
    static bool previewFixtureAppendPhysical(drums::DrumOwnership& state,drums::AssetPtr owner){
        for(auto& entry:state.ledger_)if(!entry){entry=std::move(owner);return true;}return false;}
    static bool previewAlive(const drums::DrumOwnership& state,const drums::SampleAsset* owner){
        return std::any_of(state.ledger_.begin(),state.ledger_.end(),[&](const auto& entry){return entry.get()==owner;});}
    static std::uint64_t previewRetained(const drums::DrumOwnership& state){return state.retainedBytes();}
    static std::uint64_t previewReserved(const drums::DrumOwnership& state){return state.reservedDecoded_;}
    static constexpr std::uint64_t previewImageBytes(){return sizeof(drums::DrumOwnership::AiImage);}
    static constexpr std::size_t previewLedgerCapacity(){return drums::DrumOwnership::ledgerCapacity;}
    static void previewCollect(drums::DrumOwnership& state){state.collect();}
    static void previewPreparation(drums::DrumOwnership& state,drums::AssetPtr owner){state.preparations_[0].asset=std::move(owner);}
    static auto previewStatus(drums::DrumOwnership& state,std::size_t role,drums::DrumDocumentPtr document,const PreviewOwners& owners){
        return state.installAiImage(role,std::move(document),owners);}
    static bool previewInstall(drums::DrumOwnership& state,std::size_t role,drums::DrumDocumentPtr document,const PreviewOwners& owners){
        return previewStatus(state,role,std::move(document),owners)==PreviewStatus::ready;}
    static void previewClear(drums::DrumOwnership& state,std::size_t role){state.clearAiImage(role);}
    static bool previewBusy(const drums::DrumOwnership& state){return state.aiImagesBusy();}
    static const void* previewImage(const drums::DrumOwnership& state,std::size_t role){return state.aiImages_[role].get();}
    static const drums::DrumDocument* previewDocument(const drums::DrumOwnership& state,std::size_t role){return state.aiImages_[role]?state.aiImages_[role]->document.get():nullptr;}
    static drums::AssetPtr previewOwner(const drums::DrumOwnership& state,std::size_t role,std::size_t at){return state.aiImages_[role]->owners[at];}
    static bool previewReserve(drums::DrumOwnership& state,std::uint64_t bytes){return state.reserveDecoded(bytes);}
    static void previewRelease(drums::DrumOwnership& state,std::uint64_t bytes){state.releaseDecoded(bytes);}
    static std::array<std::size_t,2> legacySizes(){return InstrumentProcessor::legacyHistorySizes();}
    static bool aiEdit(InstrumentProcessor& p,float value=.613f){const std::lock_guard lock(p.restoreMutex);auto before=std::make_unique<ProjectDocument>();std::uint64_t edit;
        if(!p.tryProjectSnapshotOwned(*before,edit))return false;auto after=std::make_unique<ProjectDocument>(*before);after->sound.values[0]=value;
        return p.performAiEdit(*before,*after,false);}
    static void busy(InstrumentProcessor& p,bool value){const std::lock_guard lock(p.restoreMutex);p.gesture->index=value?0:parameterCount;}
    static void structuralHook(void (*hook)(int)){InstrumentProcessor::setStructuralTestHook(hook);}
    static auto report(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->latest_;}
    static auto identity(InstrumentProcessor& p){return p.engine.installedDrumIdentity();}
    static void workerHook(InstrumentProcessor& p,void* context,void (*hook)(void*,unsigned)){RoutedDrumTestAccess::workerHook(p.engine,context,hook);}
    static bool teardownLocksAvailable(InstrumentProcessor& p){
        const bool restore=p.restoreMutex.try_lock();if(restore)p.restoreMutex.unlock();
        const bool operation=p.drumRestoreOperationMutex.try_lock();if(operation)p.drumRestoreOperationMutex.unlock();return restore&&operation;
    }
    static void armMessageCallbacks(InstrumentProcessor& p){p.startTimer(25);p.triggerAsyncUpdate();}
    static bool messageCallbacksStopped(InstrumentProcessor& p){return !p.isTimerRunning()&&!p.isUpdatePending()&&!p.drumControlPump;}
    static auto workers(InstrumentProcessor& p){return p.engine.dspWorkerStats();}
    static bool prepareSpace(InstrumentProcessor& p,const space::SpaceActivationOptions& options){const std::lock_guard lock(p.restoreMutex);return RoutedDrumTestAccess::prepareSpace(p.engine,options);}
    static auto engineSnapshot(InstrumentProcessor& p){return RoutedDrumTestAccess::snapshot(p.engine);}
    static std::weak_ptr<const drums::SampleAsset> weakCurrentAsset(InstrumentProcessor& p,unsigned n=0){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->current_->owners[n];}
    static std::array<std::uint64_t,5> accounting(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);auto& state=*p.drumOwnership;
        return {state.retainedBytes(),state.reservedDecoded_,state.reservedWorkspace_,std::uint64_t(state.saving_),std::uint64_t(state.restoring_)};}
    static DrumCommitError status(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.drumCommitStatus;}
    static std::uint64_t sourceWord(InstrumentProcessor& p){return p.structuralGeneration.load();}
    static bool enqueue(InstrumentProcessor& p,const drums::ReferenceReport& report){return p.drumOwnership->enqueue(report);}
    static bool alive(InstrumentProcessor& p,const drums::SampleAsset* raw){const std::lock_guard lock(p.restoreMutex);
        return std::any_of(p.drumOwnership->ledger_.begin(),p.drumOwnership->ledger_.end(),[&](const auto& owner){return owner.get()==raw;});}
};
}
using namespace airack;
using namespace airack::drums;
namespace {
using Access=InstrumentDrumTestAccess;
juce::File fixtureFile(const char*);
extern InstrumentProcessor* casWriter;extern std::uint64_t casExternalWord;extern unsigned casWrites;void externalCasWrite(int);
thread_local unsigned* admissionCasCounter=nullptr;
void countAdmissionCas(int point){if(point==3&&admissionCasCounter)++*admissionCasCounter;}
int checks=0,failures=0,sections=0;const char* section="";
bool check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<section<<':'<<line<<' '<<text<<'\n';}return ok;}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* text){section=text;++sections;std::cout<<"OWNERSHIP_SECTION "<<text<<'\n';}
void localPreviewRootRetention(){
    begin("local_preview_physical_owner_roots_survive_collection");
    for(std::size_t role=0;role<3;++role){
        auto state=std::make_unique<DrumOwnership>();const auto baseline=Access::previewRetained(*state);CHECK(baseline==0);
        AssetPtr asset;AssetError ae;const std::array<float,4> pcm{float(role+1)*.125f,0,0,0};
        if(!CHECK(makeSampleAsset(pcm,1,asset,ae)))return;const auto* physical=asset.get();
        if(!CHECK(Access::previewRegister(*state,asset)))return;
        Access::previewPreparation(*state,asset);Access::previewCollect(*state);
        CHECK(Access::previewAlive(*state,physical));CHECK(Access::previewRetained(*state)==sizeof(pcm));
        DrumSlots slots{};const auto at=role==0?0u:role==1?31u:63u;slots[at/16][at%16].asset=asset->id();
        DrumDocumentPtr document;DrumError de;if(!CHECK(makeDrumDocument(slots,document,de)))return;
        Access::PreviewOwners owners{};owners[at]=asset;
        if(!CHECK(Access::previewInstall(*state,role,std::move(document),owners)))return;
        Access::previewPreparation(*state,{});Access::previewCollect(*state);
        CHECK(Access::previewAlive(*state,physical));
        CHECK(Access::previewRetained(*state)==sizeof(pcm)+Access::previewImageBytes());
        CHECK(physical->samples()[0]==pcm[0]);
        Access::previewClear(*state,role);CHECK(!Access::previewAlive(*state,physical));CHECK(Access::previewRetained(*state)==baseline);
    }
}
std::size_t index(const juce::String& id){const auto registry=parameterRegistry();
    for(std::size_t n=0;n<registry.size();++n)if(registry[n].id==id)return n;CHECK(false);return 0;}
void physical(SoundDocument& d,const juce::String& id,float value){const auto n=index(id);const auto& descriptor=parameterRegistry()[n];
    juce::NormalisableRange<float> range(descriptor.minimum,descriptor.maximum,descriptor.kind==ParameterKind::continuous?0.f:1.f,descriptor.skew);
    d.values[n]=range.convertTo0to1(value);}
struct Fixture {
    std::unique_ptr<InstrumentProcessor> owner=std::make_unique<InstrumentProcessor>();
    std::array<juce::Uuid,4> racks;
    unsigned banks;
    explicit Fixture(unsigned count=1):banks(count){
        for(unsigned bank=0;bank<count;++bank){ProjectDocument d;std::uint64_t edit;CHECK(owner->tryProjectSnapshot(d,edit));
            RackDesign design;design.name="Owned Drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};
            RackOperation op;op.design=design;op.newInstance=racks[bank];op.allowSlotReuse=true;op.allocation={{0,static_cast<std::uint8_t>(bank)}};
            op.destination={RackRole::synth,masterChain,static_cast<std::uint8_t>(bank)};
            CHECK(owner->applyRackOperation(op,edit)==RackCandidateStatus::ready);
        }
        auto d=std::make_unique<SoundDocument>();std::uint64_t edit;CHECK(owner->trySoundSnapshot(*d,edit));
        d->values[0]=1;physical(*d,"output.gain_db",0);physical(*d,"output.protection",0);
        for(unsigned bank=0;bank<count;++bank){const auto prefix="synth."+juce::String(bank+1)+".";
            physical(*d,prefix+"enabled",1);physical(*d,prefix+"level",1);physical(*d,prefix+"pan",0);}
        CHECK(owner->applySoundEdit(*d,SoundEditOrigin::manual,edit));Access::clear(*owner);
    }
    InstrumentProcessor& p(){return *owner;}
    DrumPadTarget target(unsigned bank=0,unsigned pad=0){DrumPadTarget out;DrumCommitError error;CHECK(owner->captureDrumPadTarget(racks[bank],0,static_cast<std::uint8_t>(pad),out,error));return out;}
};
struct Prepared {std::uint64_t ticket=0;DrumPad metadata;};
Prepared prepare(InstrumentProcessor& p,float marker,std::size_t frames=32,unsigned channels=1,const char* name="p"){
    std::vector<float> samples(frames*channels,marker);Prepared out;DrumCommitError error;
    CHECK(p.prepareDrumAsset(samples,channels,out.ticket,error));CHECK(out.ticket!=0&&error==DrumCommitError::none);
    const auto state=Access::snapshot(p);for(std::size_t n=0;n<3;++n)if(state->tickets[n]==out.ticket)out.metadata.asset=state->candidates[n];
    out.metadata.name=name;CHECK(out.metadata.asset.has_value());return out;
}
bool apply(Fixture& f,unsigned bank,unsigned pad,const Prepared& candidate,std::span<const ParameterGestureValue> values={}){
    DrumCommitError error;const auto target=f.target(bank,pad);const bool result=f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,values,error);
    CHECK(result&&error==DrumCommitError::none);return result;
}
std::array<std::vector<float>,2> process(InstrumentProcessor& p,int frames=64,int note=-1){
    juce::AudioBuffer<float> audio(2,frames);audio.clear();juce::MidiBuffer midi;
    if(note>=0)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(127)),0);
    p.processBlock(audio,midi);std::array<std::vector<float>,2> out;
    for(unsigned channel=0;channel<2;++channel)out[channel].assign(audio.getReadPointer(channel),audio.getReadPointer(channel)+frames);
    return out;
}
void callbackAndDrain(Fixture& f){(void)process(f.p(),1);Access::drain(f.p());}
bool constant(const std::array<std::vector<float>,2>& audio,float expected){
    for(const auto& channel:audio)for(float sample:channel)if(std::abs(sample-expected)>2e-6f)return false;return true;
}
void sameNumericAndTargets(){
    begin("same_numeric_asset_generation_installs");Fixture f;auto a=prepare(f.p(),.25f,8192);CHECK(apply(f,0,0,a));
    f.p().prepareToPlay(48000,64);CHECK(constant(process(f.p(),7,36),.25f));
    const auto old=Access::snapshot(f.p());const auto other=f.target(0,1),stale=f.target();
    auto b=prepare(f.p(),.5f,8192);CHECK(apply(f,0,0,b));const auto replaced=Access::snapshot(f.p());
    CHECK(replaced->generation>old->generation&&replaced->targets[0]!=old->targets[0]);
    for(std::size_t n=1;n<64;++n)CHECK(replaced->targets[n]==old->targets[n]);
    CHECK(constant(process(f.p(),7),.25f));CHECK(constant(process(f.p(),7,36),.75f));
    auto c=prepare(f.p(),.125f);DrumCommitError error;
    Access::drain(f.p());auto before=Access::snapshot(f.p());CHECK(!f.p().assignPreparedDrumPad(stale,c.ticket,c.metadata,{},error));
    CHECK(error==DrumCommitError::staleTarget);auto after=Access::snapshot(f.p());CHECK(*before==*after);
    CHECK(f.p().assignPreparedDrumPad(other,c.ticket,c.metadata,{},error));
    f.p().releaseResources();
}
void padScopedHistory(){
    begin("pad_undo_merges_neighbor_and_automation");Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));
    auto b=prepare(f.p(),.5f);const auto trim=drumParameterStart+3;
    const std::array<ParameterGestureValue,1> values{{{trim,.125f}}};CHECK(apply(f,0,0,b,values));
    auto history=Access::history(f.p());const auto* action=history->identities[history->count-1];
    auto c=prepare(f.p(),.375f);CHECK(apply(f,0,1,c));f.p().control(0).setValue(.71f);
    auto before=Access::snapshot(f.p());CHECK(Access::retained(f.p(),action,true));auto undone=Access::snapshot(f.p());
    CHECK(undone->slots[0][0].asset==a.metadata.asset&&undone->slots[0][1]==before->slots[0][1]);
    CHECK(undone->project.sound.values[0]==.71f&&undone->project.sound.values[trim]==0);
    CHECK(undone->history==before->history);
    CHECK(Access::retained(f.p(),action,false));auto redone=Access::snapshot(f.p());
    CHECK(redone->slots[0][0]==b.metadata&&redone->slots[0][1]==c.metadata&&redone->project.sound.values[trim]==.125f);
    CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->slots[0][1]==DrumPad{});
    CHECK(f.p().redoSoundEdit());CHECK(Access::snapshot(f.p())->slots[0][1]==c.metadata);
}

RackOperation placeFor(const juce::Uuid& id,unsigned bank=0,unsigned position=0){
    RackDesign design;design.name="Owned Drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};
    RackOperation op;op.newInstance=id;op.design=design;op.allowSlotReuse=true;op.allocation={{0,static_cast<std::uint8_t>(bank)}};
    op.destination={RackRole::synth,masterChain,static_cast<std::uint8_t>(position)};return op;
}
SourceProvenance generated(){SourceProvenance source;source.kind=SourceKind::generated;source.sourceFrames=32;source.end=32;
    source.generator="literal-test-generator";source.condition="literal pulse";source.seed=42;return source;}
void typedReuseAndIdentity(){
    begin("fresh_logical_uuid_cannot_impersonate_retained_owned_action");Fixture f;auto a=prepare(f.p(),.25f);a.metadata.source=generated();CHECK(apply(f,0,0,a));
    const auto original=Access::snapshot(f.p());RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=f.racks[0];
    const std::array<RackOperation,2> same{remove,placeFor(f.racks[0])};
    const auto status=f.p().applyRackOperations(same,original->edit);CHECK(status!=RackCandidateStatus::ready);
    if(status==RackCandidateStatus::ready)return;
    CHECK(*original==*Access::snapshot(f.p()));
    begin("typed_reuse_entrypoint_owned_action_roundtrip");const juce::Uuid fresh;const std::array<RackOperation,2> replacement{remove,placeFor(fresh)};
    CHECK(f.p().applyRackOperations(replacement,original->edit)==RackCandidateStatus::ready);const auto replaced=Access::snapshot(f.p());
    CHECK(replaced->slots[0]==DrumKit{}&&replaced->targets[0]!=original->targets[0]);
    const auto last=replaced->history.count-1;CHECK(replaced->history.kinds[last]==2&&replaced->history.masks[last]==0xffff);
    CHECK(std::accumulate(replaced->history.chargeTerms[last].begin(),replaced->history.chargeTerms[last].end(),std::uint64_t(0))==replaced->history.units[last]);
    CHECK(!Access::retained(f.p(),original->history.identities[original->history.count-1],true));CHECK(*replaced==*Access::snapshot(f.p()));
    CHECK(f.p().undoSoundEdit());auto undone=Access::snapshot(f.p());CHECK(undone->slots==original->slots&&undone->project.sound==original->project.sound);
    CHECK(undone->targets[0]!=original->targets[0]);CHECK(f.p().redoSoundEdit());CHECK(Access::snapshot(f.p())->slots==replaced->slots);
    CHECK(f.p().undoSoundEdit());CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->slots[0][0]==DrumPad{});
}
void clearedHistoryIdentity(){
    begin("apply_sound_edit_cleared_pad_history_blocks_fresh_owner");Fixture f;auto a=prepare(f.p(),.25f);a.metadata.source=generated();CHECK(apply(f,0,0,a));
    DrumCommitError error;CHECK(f.p().clearDrumPad(f.target(),error));const auto cleared=Access::snapshot(f.p());CHECK(cleared->slots==DrumSlots{});
    const auto* clearAction=cleared->history.identities[cleared->history.count-1];RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=f.racks[0];
    CHECK(f.p().applyRackOperation(remove,cleared->edit)==RackCandidateStatus::ready);auto b=prepare(f.p(),.5f);const auto removed=Access::snapshot(f.p());
    CHECK(removed->slots==DrumSlots{});auto candidate=std::make_unique<SoundDocument>(cleared->project.sound);
    CHECK(validateSoundDocument(*candidate));unsigned sourceCas=0;admissionCasCounter=&sourceCas;Access::structuralHook(countAdmissionCas);
    const bool accepted=f.p().applySoundEdit(*candidate,SoundEditOrigin::manual,removed->edit);Access::structuralHook(nullptr);admissionCasCounter=nullptr;CHECK(!accepted&&sourceCas==0);
    begin("apply_sound_edit_empty_history_identity_compatibility");juce::MemoryBlock numeric;CHECK(encodeProjectState(removed->project,numeric));
    auto legacy=std::make_unique<InstrumentProcessor>();CHECK(legacy->restoreProjectState(numeric.getData(),int(numeric.getSize())));
    auto baseline=Access::snapshot(*legacy);CHECK(legacy->applySoundEdit(*candidate,SoundEditOrigin::manual,baseline->edit));
    baseline=Access::snapshot(*legacy);candidate->values[0]=.73f;CHECK(legacy->applySoundEdit(*candidate,SoundEditOrigin::manual,baseline->edit));
    std::cout<<"I1_LEGACY_POSITIVE accepted_empty_history=1 same_owner_numeric=1\n";
    if(accepted)return;
    CHECK(*removed==*Access::snapshot(f.p()));CHECK(!Access::retained(f.p(),clearAction,true));CHECK(*removed==*Access::snapshot(f.p()));
    CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->slots==DrumSlots{});CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->slots[0][0]==a.metadata);
    f.p().discardPreparedDrumAsset(b.ticket);
}
void duplicateRoundTrip(){
    begin("duplicate_entrypoint_owned_action_roundtrip");Fixture f;auto a=prepare(f.p(),.25f);a.metadata.source=generated();CHECK(apply(f,0,0,a));
    const auto before=Access::snapshot(f.p());RackOperation copy;copy.kind=RackOperationKind::duplicate;copy.selected=f.racks[0];copy.newInstance=juce::Uuid();
    copy.allocation={{0,1}};copy.destination={RackRole::synth,masterChain,1};CHECK(f.p().applyRackOperation(copy,before->edit)==RackCandidateStatus::ready);
    f.racks[1]=copy.newInstance;f.banks=2;auto copied=Access::snapshot(f.p());CHECK(copied->slots[1]==before->slots[0]&&copied->owners[16]==before->owners[0]);
    const auto* duplicate=copied->history.identities[copied->history.count-1];CHECK(copied->history.kinds[copied->history.count-1]==2);
    CHECK(copied->history.masks[copied->history.count-1]==(std::uint64_t(0xffff)<<16));
    auto b=prepare(f.p(),.5f);CHECK(apply(f,0,0,b));auto c=prepare(f.p(),.75f);CHECK(apply(f,1,0,c));
    CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->slots[1][0]==a.metadata);CHECK(f.p().redoSoundEdit());CHECK(Access::snapshot(f.p())->slots[1][0]==c.metadata);
    const auto current=Access::snapshot(f.p());CHECK(Access::retained(f.p(),duplicate,true));auto undone=Access::snapshot(f.p());
    CHECK(undone->slots[1]==DrumKit{}&&undone->slots[0][0]==b.metadata&&undone->history==current->history);
    CHECK(Access::retained(f.p(),duplicate,false));auto redone=Access::snapshot(f.p());CHECK(redone->slots[1][0]==a.metadata&&redone->slots[0][0]==b.metadata);
}
void publicationSaturation(){
    begin("sixty_four_commits_without_callbacks");Fixture f(4);f.p().prepareToPlay(48000,64);
    CHECK(Access::snapshot(f.p())->generation==1);
    for(unsigned n=0;n<64;++n){auto candidate=prepare(f.p(),float(n+1)/128,4);CHECK(apply(f,n/16,n%16,candidate));}
    auto candidate=prepare(f.p(),.99f,4);auto before=Access::snapshot(f.p());
    CHECK(before->generation==65&&std::count_if(before->publications.begin(),before->publications.end(),[](auto g){return g!=0;})==65);
    DrumCommitError error;const auto target=f.target();CHECK(!f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,{},error));
    CHECK(error==DrumCommitError::publicationFull);auto after=Access::snapshot(f.p());CHECK(*before==*after);
    CHECK(!f.p().undoSoundEdit());CHECK(*Access::snapshot(f.p())==*after);
    callbackAndDrain(f);CHECK(f.p().undoSoundEdit());callbackAndDrain(f);CHECK(f.p().redoSoundEdit());
    f.p().releaseResources();auto released=Access::snapshot(f.p());
    CHECK(std::count_if(released->publications.begin(),released->publications.end(),[](auto g){return g!=0;})==1);
    CHECK(released->slots==before->slots);f.p().prepareToPlay(48000,64);
    CHECK(apply(f,0,0,candidate));f.p().releaseResources();
}
void preparationSlots(){
    begin("prepare_cancel_and_late_completion");Fixture f;
    struct Barrier {std::atomic<unsigned> entered{0};std::atomic<bool> release{false};} barrier;
    Access::hook(f.p(),&barrier,[](void* context,Access::Point point){if(point!=Access::Point::afterPreparationReservation)return;
        auto& barrier=*static_cast<Barrier*>(context);barrier.entered.fetch_add(1);
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!barrier.release.load()&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();});
    std::array<std::uint64_t,3> ids{999,999,999};std::array<DrumCommitError,3> errors{};std::array<bool,3> made{};
    std::array<std::thread,3> workers;
    for(unsigned n=0;n<3;++n)workers[n]=std::thread([&,n]{const std::array<float,4> data{.1f,.2f,.3f,.4f};made[n]=f.p().prepareDrumAsset(data,1,ids[n],errors[n]);});
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(barrier.entered.load()!=3&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
    auto running=Access::snapshot(f.p());CHECK(barrier.entered.load()==3&&running->reserved==48);
    std::uint64_t ticket=0;for(std::size_t n=0;n<3;++n)if(running->running[n]){ticket=running->tickets[n];break;}
    f.p().discardPreparedDrumAsset(ticket);auto cancelled=Access::snapshot(f.p());CHECK(cancelled->reserved==running->reserved);
    std::uint64_t fourth=777;DrumCommitError error;const std::array<float,1> data{.25f};
    CHECK(!f.p().prepareDrumAsset(data,1,fourth,error)&&error==DrumCommitError::preparationFull&&fourth==777);
    barrier.release.store(true);for(auto& worker:workers)worker.join();Access::hook(f.p(),nullptr,nullptr);
    CHECK(std::count(made.begin(),made.end(),false)==1);
    for(unsigned n=0;n<3;++n)if(made[n])f.p().discardPreparedDrumAsset(ids[n]);else CHECK(ids[n]==999&&errors[n]==DrumCommitError::cancelled);
    CHECK(Access::snapshot(f.p())->reserved==0);
}
void padAdmission(){
    begin("pad_target_metadata_nine_fields_and_lock_admission");Fixture f;auto candidate=prepare(f.p(),.25f);auto target=f.target();DrumCommitError error;
    auto output=target;CHECK(!f.p().captureDrumPadTarget(juce::Uuid::null(),0,0,output,error)&&error==DrumCommitError::invalid&&output==target);
    CHECK(!f.p().captureDrumPadTarget(f.racks[0],0,16,output,error)&&error==DrumCommitError::invalid&&output==target);
    auto generic=std::make_unique<InstrumentProcessor>();CHECK(!generic->captureDrumPadTarget(juce::Uuid(),0,0,output,error)&&error==DrumCommitError::invalid&&output==target);
    for(unsigned kind=0;kind<8;++kind){std::vector<ParameterGestureValue> edits{{drumParameterStart,.2f}};auto metadata=candidate.metadata;
        if(kind==0)edits.push_back(edits.front());if(kind==1)edits[0].index=drumParameterStart+9;
        if(kind==2)edits[0].value=std::numeric_limits<float>::quiet_NaN();if(kind==3)edits[0].value=-.01f;if(kind==4)edits[0].value=1.01f;
        if(kind==5)for(unsigned n=1;n<10;++n)edits.push_back({drumParameterStart+n,.2f});if(kind==6)metadata.name.assign(65,'x');
        if(kind==7){metadata.source=generated();metadata.source->begin=33;}
        auto before=Access::snapshot(f.p());CHECK(!f.p().assignPreparedDrumPad(target,candidate.ticket,metadata,edits,error));CHECK(error==DrumCommitError::invalid&&*before==*Access::snapshot(f.p()));
    }
    for(unsigned field=0;field<10;++field){auto before=Access::snapshot(f.p());auto document=std::make_unique<SoundDocument>(before->project.sound);
        if(field==9)document->synthLocks[0]=true;else document->controlLocks[drumParameterStart+field]=true;
        CHECK(f.p().applySoundEdit(*document,SoundEditOrigin::manual,before->edit));before=Access::snapshot(f.p());
        CHECK(!f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,{},error)&&error==DrumCommitError::locked);CHECK(*before==*Access::snapshot(f.p()));
        document->synthLocks[0]=false;document->controlLocks.fill(false);CHECK(f.p().applySoundEdit(*document,SoundEditOrigin::manual,before->edit));
    }
    std::array<ParameterGestureValue,9> edits;for(unsigned n=0;n<9;++n)edits[n]={drumParameterStart+n,.2f};edits[4].value=.8f;edits[7].value=37.f/127.f;edits[8].value=1.f/16.f;
    CHECK(f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,edits,error));const auto after=Access::snapshot(f.p());
    for(unsigned n=0;n<9;++n)CHECK(after->project.sound.values[drumParameterStart+n]==edits[n].value);
}
struct SmallHistoryProbeAction final : juce::UndoableAction {
    int& counter;const int before,after,id;
    SmallHistoryProbeAction(int& value,int next,int tag):counter(value),before(value),after(next),id(tag){}
    bool perform() override {counter=after;return true;}
    bool undo() override {counter=before;return true;}
    int getSizeInUnits() override {return int(sizeof(*this));}
};
static_assert(64*sizeof(SmallHistoryProbeAction)<=30000);
struct PayloadHistoryProbeAction final : juce::UndoableAction {
    int& counter;const int before,after,id;const std::size_t payloadBytes;const std::unique_ptr<std::byte[]> payload;
    PayloadHistoryProbeAction(int& value,int next,int tag,std::size_t bytes):counter(value),before(value),after(next),id(tag),payloadBytes(bytes),payload(bytes?std::make_unique<std::byte[]>(bytes):nullptr)
    {if(bytes)std::fill_n(payload.get(),bytes,std::byte{0x5a});}
    bool perform() override {counter=after;return true;}
    bool undo() override {counter=before;return true;}
    int getSizeInUnits() override {return int(sizeof(*this)+payloadBytes);}
};
void literalHistoryBoundaries(){
    begin("history_hard64_honest_small_action");auto p=std::make_unique<InstrumentProcessor>();int counter=0;
    std::array<const juce::UndoableAction*,66> ids{};
    for(int id=1;id<=65;++id){auto action=std::make_unique<SmallHistoryProbeAction>(counter,id,id);ids[id]=action.get();CHECK(Access::insert(*p,std::move(action)));
        auto view=Access::history(*p);const int first=id==65?2:1;
        CHECK(view->count==std::min(id,64)&&view->cursor==view->count&&view->storedUnits==view->count*sizeof(SmallHistoryProbeAction));
        for(std::size_t n=0;n<view->count;++n)CHECK(view->identities[n]==ids[first+n]);}
    CHECK(counter==65);
    begin("history_minimum30_over_budget");Access::clear(*p);counter=0;
    for(int id=1;id<=31;++id){const auto charge=id==31?1000:1001;
        auto action=std::make_unique<PayloadHistoryProbeAction>(counter,id,id,charge-sizeof(PayloadHistoryProbeAction));ids[id]=action.get();CHECK(Access::insert(*p,std::move(action)));}
    auto view=Access::history(*p);CHECK(view->count==30&&view->cursor==30&&view->storedUnits==30029);
    for(std::size_t n=0;n<30;++n)CHECK(view->identities[n]==ids[n+2]);
}
void realHistoryBoundary(){
    begin("history_real_pad_budget_boundary");Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));
    f.p().prepareToPlay(48000,64);callbackAndDrain(f);Access::clear(f.p());
    auto b=prepare(f.p(),.5f);CHECK(apply(f,0,0,b));callbackAndDrain(f);auto view=Access::history(f.p());
    const auto cost=view->units[0];CHECK(cost>0&&std::accumulate(view->chargeTerms[0].begin(),view->chargeTerms[0].end(),std::uint64_t(0))==cost);
    const auto count=std::min<std::uint64_t>(64,std::max<std::uint64_t>(30,30000/cost));
    std::cout<<"PRODUCTION_PAD_HISTORY cost="<<cost<<" reachable="<<count<<" sixty_four_cost="<<64ull*cost<<'\n';
    std::vector<const juce::UndoableAction*> ids{view->identities[0]};
    for(std::size_t n=1;n<=count;++n){auto candidate=prepare(f.p(),n%2?.25f:.5f);CHECK(apply(f,0,0,candidate));callbackAndDrain(f);
        view=Access::history(f.p());ids.push_back(view->identities[view->count-1]);CHECK(view->units[view->count-1]==cost);}
    view=Access::history(f.p());CHECK(view->count==count&&view->cursor==count&&view->storedUnits==count*cost);
    for(std::size_t n=0;n<count;++n)CHECK(view->identities[n]==ids[n+1]);
    const auto sizes=Access::legacySizes();std::cout<<"LEGACY_ACTION_SIZES document="<<sizes[0]<<" ai="<<sizes[1]<<'\n';CHECK(sizes[0]>30000&&sizes[1]>30000);
    f.p().releaseResources();
}

void mixedRealHistory(){
    for(bool ai:{false,true}){
        begin(ai?"history_mixed_real_ai_pad_minimum":"history_mixed_real_legacy_pad_minimum");
        Fixture f;f.p().prepareToPlay(48000,64);std::vector<const juce::UndoableAction*> ids;
        std::uint64_t charge=0;
        for(unsigned n=0;n<29;++n){auto candidate=prepare(f.p(),n%2?.25f:.5f);CHECK(apply(f,0,0,candidate));callbackAndDrain(f);
            auto view=Access::history(f.p());ids.push_back(view->identities[view->count-1]);if(n==0)charge=view->units[0];CHECK(view->units[view->count-1]==charge);}
        if(ai)CHECK(Access::aiEdit(f.p()));
        else{auto d=std::make_unique<SoundDocument>();std::uint64_t edit;CHECK(f.p().trySoundSnapshot(*d,edit));d->values[0]=.613f;CHECK(f.p().applySoundEdit(*d,SoundEditOrigin::manual,edit));}
        auto view=Access::history(f.p());const auto legacy=view->units[29];ids.push_back(view->identities[29]);
        CHECK(view->count==30&&view->cursor==30&&legacy>30000&&view->storedUnits==29*charge+legacy&&view->rootCounts[29]==0);
        for(unsigned n=0;n<30;++n)CHECK(view->identities[n]==ids[n]);
        auto candidate=prepare(f.p(),.25f);CHECK(apply(f,0,0,candidate));callbackAndDrain(f);view=Access::history(f.p());
        ids.push_back(view->identities[29]);CHECK(view->count==30&&view->cursor==30&&view->storedUnits==29*charge+legacy);
        for(unsigned n=0;n<30;++n)CHECK(view->identities[n]==ids[n+1]);
        std::cout<<"MIXED_REAL_HISTORY ai="<<ai<<" pad="<<charge<<" legacy="<<legacy<<" retained="<<view->count<<" units="<<view->storedUnits<<'\n';
        f.p().releaseResources();
    }
}
void exactThresholdHistory(){
    begin("history_mixed_exact_threshold_and_redo");Fixture f;auto candidate=prepare(f.p(),.25f);CHECK(apply(f,0,0,candidate));
    auto view=Access::history(f.p());const auto cost=view->units[0];const auto size=std::uint64_t(sizeof(PayloadHistoryProbeAction));
    CHECK(cost+30*size<=30000);int counter=0;std::vector<const juce::UndoableAction*> ids{view->identities[0]};
    for(unsigned n=2;n<=31;++n){const auto charge=n==2?30000-cost-29*size:size;
        auto action=std::make_unique<PayloadHistoryProbeAction>(counter,int(n),int(n),charge-size);ids.push_back(action.get());CHECK(Access::insert(f.p(),std::move(action)));}
    view=Access::history(f.p());CHECK(view->count==31&&view->cursor==31&&view->storedUnits==30000);
    for(unsigned n=0;n<31;++n)CHECK(view->identities[n]==ids[n]);CHECK(f.p().undoSoundEdit());
    view=Access::history(f.p());CHECK(view->count==31&&view->cursor==30&&view->storedUnits==30000&&counter==30);
    auto rejected=prepare(f.p(),.5f);const auto target=f.target();auto before=Access::snapshot(f.p());DrumCommitError error;
    Access::busy(f.p(),true);CHECK(!f.p().assignPreparedDrumPad(target,rejected.ticket,rejected.metadata,{},error));Access::busy(f.p(),false);
    CHECK(error==DrumCommitError::busy&&*before==*Access::snapshot(f.p()));
    auto next=std::make_unique<PayloadHistoryProbeAction>(counter,32,32,size+cost+1-size);const auto* final=next.get();CHECK(Access::insert(f.p(),std::move(next)));
    view=Access::history(f.p());CHECK(view->count==30&&view->cursor==30&&view->storedUnits==30001&&counter==32);
    for(unsigned n=0;n<29;++n)CHECK(view->identities[n]==ids[n+1]);CHECK(view->identities[29]==final);f.p().discardPreparedDrumAsset(rejected.ticket);
}
void realRedoReject(){
    begin("history_real_rejected_insertion_keeps_redo");Fixture f;f.p().prepareToPlay(48000,64);
    std::uint64_t cost=0,count=0;
    for(unsigned n=0;n<34;++n){auto candidate=prepare(f.p(),n%2?.25f:.5f);CHECK(apply(f,0,0,candidate));callbackAndDrain(f);
        auto view=Access::history(f.p());cost=view->units[view->count-1];count=std::min<std::uint64_t>(64,std::max<std::uint64_t>(30,30000/cost));}
    CHECK(f.p().undoSoundEdit());callbackAndDrain(f);CHECK(f.p().undoSoundEdit());callbackAndDrain(f);
    auto candidate=prepare(f.p(),.75f);const auto target=f.target();auto before=Access::snapshot(f.p());DrumCommitError error;
    CHECK(before->history.count==count&&before->history.cursor==count-2);
    Access::busy(f.p(),true);CHECK(!f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,{},error));Access::busy(f.p(),false);
    CHECK(error==DrumCommitError::busy&&*before==*Access::snapshot(f.p()));
    casWriter=&f.p();casWrites=0;casExternalWord=0;Access::structuralHook(externalCasWrite);
    CHECK(!f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,{},error));Access::structuralHook(nullptr);casWriter=nullptr;
    CHECK(casWrites==1&&error!=DrumCommitError::none);const auto conflicted=Access::snapshot(f.p());
    CHECK(conflicted->history==before->history&&conflicted->slots==before->slots&&conflicted->owners==before->owners&&conflicted->candidates==before->candidates&&conflicted->tickets==before->tickets);
    CHECK(conflicted->project==before->project&&conflicted->generation==before->generation&&conflicted->source==casExternalWord);
    CHECK(f.p().assignPreparedDrumPad(f.target(),candidate.ticket,candidate.metadata,{},error));auto after=Access::snapshot(f.p());
    CHECK(after->history.count==count-1&&after->history.cursor==count-1);
    for(std::size_t n=0;n<count-2;++n)CHECK(after->history.identities[n]==before->history.identities[n]);
    CHECK(after->history.identities[count-2]!=before->history.identities[count-2]&&after->history.identities[count-2]!=before->history.identities[count-1]);
    f.p().releaseResources();
}
void fullLegacyHistory(){
    for(bool ai:{false,true}){begin(ai?"sixty_four_real_ai_actions_keep_exactly_thirty":"sixty_four_real_legacy_actions_keep_exactly_thirty");Fixture f;
        std::vector<const juce::UndoableAction*> ids;std::vector<std::uint32_t> units;
        for(unsigned n=0;n<64;++n){const float value=n%2?.713f:.613f;if(ai)CHECK(Access::aiEdit(f.p(),value));else{auto before=Access::snapshot(f.p());
                auto next=std::make_unique<SoundDocument>(before->project.sound);next->values[0]=value;CHECK(f.p().applySoundEdit(*next,SoundEditOrigin::manual,before->edit));}
            const auto view=Access::history(f.p());ids.push_back(view->identities[view->count-1]);units.push_back(view->units[view->count-1]);
            const auto retained=std::min(n+1,30u);CHECK(view->count==retained&&view->cursor==retained);std::uint64_t sum=0;
            for(unsigned j=0;j<retained;++j){const auto old=n+1-retained+j;CHECK(view->identities[j]==ids[old]&&view->units[j]==units[old]&&view->rootCounts[j]==0);sum+=units[old];}
            CHECK(view->storedUnits==sum&&units.back()>30000);
            if(n==30||n==63)std::cout<<"REAL_LEGACY_HISTORY ai="<<ai<<" inserted="<<n+1<<" retained="<<view->count<<" units="<<view->storedUnits<<" sizeof="<<Access::legacySizes()[ai?1:0]<<'\n';
        }
    }
}

struct Pause {
    std::atomic<unsigned> entered{0};std::atomic<bool> release{false},expired{false};
    void wait(){entered.fetch_add(1,std::memory_order_release);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!release.load(std::memory_order_acquire)){if(std::chrono::steady_clock::now()>deadline){expired=true;return;}std::this_thread::yield();}}
    bool observe(){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();return entered.load()!=0;}
    ~Pause(){release=true;}
};
void queuedEqualAck(){
    begin("queued_equal_ack");Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));f.p().prepareToPlay(48000,64);
    callbackAndDrain(f);const auto generation=Access::snapshot(f.p())->generation;
    (void)process(f.p(),1);auto b=prepare(f.p(),.5f);CHECK(apply(f,0,0,b));Access::clear(f.p());
    CHECK(Access::recordAlive(f.p(),generation));CHECK(Access::report(f.p()).adoptedGeneration==generation);
    callbackAndDrain(f);CHECK(!Access::recordAlive(f.p(),generation));f.p().releaseResources();
}
void failedReadBorrow(){
    begin("failed_read_borrow_remains_pinned");Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));f.p().prepareToPlay(48000,64);callbackAndDrain(f);
    const auto original=Access::snapshot(f.p());const auto* raw=original->owners[0];auto b=prepare(f.p(),.5f);Pause gate;
    Access::hook(f.p(),&gate,[](void* context,Access::Point point){if(point!=Access::Point::afterBorrowedLoad)return;
        auto& gate=*static_cast<Pause*>(context);if(gate.entered.load()==0)gate.wait();else gate.entered.fetch_add(1);});
    std::thread audio([&]{(void)process(f.p(),8);});const bool entered=gate.observe();CHECK(entered);
    if(entered){CHECK(apply(f,0,0,b));Access::clear(f.p());Access::drain(f.p());
        const bool pinned=Access::recordAlive(f.p(),original->generation);CHECK(pinned);CHECK(Access::alive(f.p(),raw));
        if(pinned)CHECK(raw->samples()[0]==.25f);}
    gate.release=true;audio.join();Access::hook(f.p(),nullptr,nullptr);CHECK(!gate.expired&&gate.entered>=2);
    Access::drain(f.p());CHECK(!Access::recordAlive(f.p(),original->generation));CHECK(!Access::alive(f.p(),raw));f.p().releaseResources();
}
void capturedHitRoots(){
    begin("captured_hits_pin_after_metadata_retirement");Fixture f;auto a=prepare(f.p(),.25f,1024);CHECK(apply(f,0,0,a));f.p().prepareToPlay(48000,64);
    CHECK(constant(process(f.p(),1,36),.25f));Access::drain(f.p());auto original=Access::snapshot(f.p());const auto* raw=original->owners[0];
    auto b=prepare(f.p(),.5f,1024);CHECK(apply(f,0,0,b));Access::clear(f.p());CHECK(constant(process(f.p(),1),.25f));Access::drain(f.p());
    CHECK(!Access::recordAlive(f.p(),original->generation)&&Access::alive(f.p(),raw));const auto report=Access::report(f.p());
    CHECK(report.complete&&std::count(report.references.begin(),report.references.end(),raw)>0&&report.references[0]!=raw);
    (void)process(f.p(),1024);Access::drain(f.p());CHECK(!Access::alive(f.p(),raw));f.p().releaseResources();
}
void claimedWorkerFence(){
    begin("claimed_source_worker_fence_precedes_report");Fixture f(4);for(unsigned bank=0;bank<4;++bank){auto a=prepare(f.p(),.0625f,8192);CHECK(apply(f,bank,0,a));}
    for(unsigned slot=0;slot<2;++slot){auto snapshot=Access::snapshot(f.p());CHECK(f.p().applyRackEdit({RackEditKind::addEffect,static_cast<std::uint8_t>(slot),masterChain,static_cast<std::uint8_t>(slot),EffectKind::dynamicEq},snapshot->edit)==RackEditResult::applied);}
    f.p().prepareToPlay(48000,512);callbackAndDrain(f);CHECK(Access::workers(f.p()).prepared>0);Pause gate;
    Access::workerHook(f.p(),&gate,[](void* context,unsigned){auto& gate=*static_cast<Pause*>(context);if(!gate.release.load())gate.wait();});
    std::atomic<unsigned> reports{0};Access::hook(f.p(),&reports,[](void* context,Access::Point point){if(point==Access::Point::beforeReportPublish)static_cast<std::atomic<unsigned>*>(context)->fetch_add(1);});
    std::thread audio([&]{(void)process(f.p(),512,36);});const bool entered=gate.observe();CHECK(entered);
    if(entered){CHECK(reports==0);const auto old=Access::report(f.p());auto b=prepare(f.p(),.125f);CHECK(apply(f,0,0,b));
        Access::clear(f.p());CHECK(Access::report(f.p())==old);CHECK(Access::recordAlive(f.p(),old.adoptedGeneration));}
    gate.release=true;audio.join();Access::workerHook(f.p(),nullptr,nullptr);Access::hook(f.p(),nullptr,nullptr);CHECK(!gate.expired&&reports==1);Access::drain(f.p());callbackAndDrain(f);f.p().releaseResources();
}
void targetWaba(){
    begin("closed_editor_host_W_A_W_ABA_invalidates_targets");Fixture f;auto a=prepare(f.p(),.25f,1024);CHECK(apply(f,0,0,a));f.p().prepareToPlay(48000,64);callbackAndDrain(f);
    const auto target=f.target();const auto before=Access::snapshot(f.p());f.p().closeAiPanel();
    f.p().control(drumModeParameterStart).setValue(0);f.p().control(drumModeParameterStart).setValue(1);
    (void)process(f.p(),512,36);const auto installed=Access::identity(f.p());CHECK(installed.targets[0]==0);
    auto b=prepare(f.p(),.5f);DrumCommitError error;CHECK(!f.p().assignPreparedDrumPad(target,b.ticket,b.metadata,{},error)&&error==DrumCommitError::staleTarget);
    const auto reconciled=Access::snapshot(f.p());CHECK(reconciled->acknowledged==reconciled->source&&reconciled->source!=before->source);
    CHECK(reconciled->targets[0]!=before->targets[0]);for(std::size_t n=0;n<64;++n)CHECK(reconciled->targets[n]!=before->targets[n]);
    (void)process(f.p(),512);CHECK(Access::identity(f.p()).targets[0]==reconciled->targets[0]);CHECK(apply(f,0,0,b));
    f.p().releaseResources();const auto life=Access::snapshot(f.p())->lifecycle;auto obsolete=Access::report(f.p());obsolete.lifecycle=life-1;obsolete.complete=true;obsolete.adoptedGeneration=~std::uint64_t(0);
    CHECK(Access::enqueue(f.p(),obsolete));Access::drain(f.p());CHECK(Access::report(f.p()).adoptedGeneration==0);
    f.p().prepareToPlay(48000,64);CHECK(constant(process(f.p(),1,36),.5f));f.p().releaseResources();
}
void boundedReadDiagnostic(){
    begin("real_host_source_conflicts_report_busy_for_assign_and_clear");Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));
    auto next=prepare(f.p(),.5f);
    for(bool clear:{false,true}){const auto target=f.target();const auto before=Access::snapshot(f.p());
        struct Conflict {InstrumentProcessor* processor;unsigned attempts=0;} conflict{&f.p()};
        Access::hook(f.p(),&conflict,[](void* raw,Access::Point point){if(point!=Access::Point::beforeReadValidation)return;
            auto& c=*static_cast<Conflict*>(raw);++c.attempts;c.processor->control(drumModeParameterStart).setValue(0);c.processor->control(drumModeParameterStart).setValue(1);});
        DrumCommitError error=DrumCommitError::none;
        const bool accepted=clear?f.p().clearDrumPad(target,error):f.p().assignPreparedDrumPad(target,next.ticket,next.metadata,{},error);
        Access::hook(f.p(),nullptr,nullptr);CHECK(!accepted&&conflict.attempts==2);CHECK(error==DrumCommitError::busy);
        const auto after=Access::snapshot(f.p());CHECK(after->valid&&after->source!=before->source&&after->project==before->project);
        CHECK(after->history==before->history&&after->slots==before->slots&&after->owners==before->owners&&after->generation==before->generation);
        CHECK(after->tickets==before->tickets&&after->candidates==before->candidates);
    }
}
void independentControlPump(){
    begin("closed_editor_independent_control_pump_drains_feedback");Fixture f;auto a=prepare(f.p(),.25f,8192);CHECK(apply(f,0,0,a));
    f.p().prepareToPlay(48000,64);callbackAndDrain(f);const auto previous=Access::snapshot(f.p())->generation;
    auto b=prepare(f.p(),.5f,8192);CHECK(apply(f,0,0,b));Access::clear(f.p());f.p().closeAiPanel();
    (void)process(f.p(),1024);CHECK(Access::recordAlive(f.p(),previous));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(Access::recordAlive(f.p(),previous)&&std::chrono::steady_clock::now()<deadline)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    CHECK(!Access::recordAlive(f.p(),previous));f.p().releaseResources();
}

void pumpTeardownBarrier(){
    begin("background_teardown_waits_for_real_pump_callback_before_owner_clear");
    struct Gate {
        InstrumentProcessor* owner=nullptr;
        std::atomic<bool> entered{false},started{false},release{false},returned{false},earlyDelete{false},deleted{false},expired{false};
        std::atomic<bool> locksAvailable{false},dispatchAfterStop{false},outsideMessageLock{false},stopped{false};
        std::atomic<unsigned> callbacks{0},lateCallbacks{0};std::atomic<bool> ownerClear{false};
        static bool wait(const std::atomic<bool>& flag){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while(!flag.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));return flag.load(std::memory_order_acquire);}
        static void observe(void* raw,Access::Point point){auto& gate=*static_cast<Gate*>(raw);
            if(point==Access::Point::beforeControlPumpWork){++gate.callbacks;if(gate.ownerClear)++gate.lateCallbacks;
                if(gate.entered.load())return;
                // This queued witness runs only after the actual Timer callback has returned.
                if(!juce::MessageManager::callAsync([&gate]{gate.returned=true;}))gate.expired=true;
                gate.entered.store(true,std::memory_order_release);
                if(!wait(gate.release))gate.expired=true;
            }else if(point==Access::Point::beforeMessageCallbackStop)gate.started=true;
            else if(point==Access::Point::beforeCallbackStorageRelease){
                if(!gate.returned.load()){gate.earlyDelete=true;if(!wait(gate.returned))gate.expired=true;}
            }else if(point==Access::Point::beforeOwnerClear){
                gate.ownerClear=true;gate.outsideMessageLock=!juce::MessageManager::existsAndIsLockedByCurrentThread();
                gate.stopped=Access::messageCallbacksStopped(*gate.owner);
                if(!juce::MessageManager::callAsync([&gate]{gate.dispatchAfterStop=true;}))gate.expired=true;
                if(!wait(gate.dispatchAfterStop))gate.expired=true;
            }
        }
    } gate;
    Fixture f;auto sample=prepare(f.p(),.25f);CHECK(apply(f,0,0,sample));gate.owner=&f.p();Access::hook(f.p(),&gate,Gate::observe);
    std::thread lifecycle([&]{if(Gate::wait(gate.entered))f.owner.reset();else gate.expired=true;gate.deleted=true;});
    std::thread controller([&]{if(Gate::wait(gate.started)){
        gate.locksAvailable=Access::teardownLocksAvailable(*gate.owner);
        // Hold the real callback while the requested destructor reaches its stop boundary.
        // The old path records earlyDelete and waits safely; the fixed path waits in JUCE dispatch exclusion.
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        while(!gate.earlyDelete.load()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }else gate.expired=true;gate.release=true;});
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(35);
    while(!gate.deleted.load()&&std::chrono::steady_clock::now()<end)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    controller.join();lifecycle.join();CHECK(!gate.expired&&gate.entered&&gate.returned&&gate.deleted);CHECK(!gate.earlyDelete);
    CHECK(gate.locksAvailable&&gate.outsideMessageLock&&gate.dispatchAfterStop&&gate.stopped&&gate.ownerClear);
    const auto count=gate.callbacks.load();juce::MessageManager::getInstance()->runDispatchLoopUntil(100);CHECK(gate.callbacks==count&&gate.lateCallbacks==0);
    std::cout<<"PUMP_TEARDOWN background=1 guarded_early_delete="<<gate.earlyDelete<<" callback_returned="<<gate.returned<<" locks_available="<<gate.locksAvailable
        <<" dispatch_before_owner_clear="<<gate.dispatchAfterStop<<" late_callbacks="<<gate.lateCallbacks<<'\n';

    struct InlineGate {InstrumentProcessor* owner=nullptr;bool observed=false,stopped=false,locked=false;
        static void observe(void* raw,Access::Point point){if(point!=Access::Point::beforeOwnerClear)return;auto& gate=*static_cast<InlineGate*>(raw);
            gate.observed=true;gate.stopped=Access::messageCallbacksStopped(*gate.owner);gate.locked=juce::MessageManager::existsAndIsLockedByCurrentThread();}
    };
    begin("message_thread_teardown_stops_all_private_callbacks");auto inlineOwner=std::make_unique<InstrumentProcessor>();InlineGate inlineGate{inlineOwner.get()};
    Access::hook(*inlineOwner,&inlineGate,InlineGate::observe);Access::armMessageCallbacks(*inlineOwner);inlineOwner.reset();CHECK(inlineGate.observed&&inlineGate.stopped&&inlineGate.locked);
    begin("already_message_locked_background_teardown_is_recursive");auto lockedOwner=std::make_unique<InstrumentProcessor>();InlineGate lockedGate{lockedOwner.get()};
    Access::hook(*lockedOwner,&lockedGate,InlineGate::observe);std::atomic<bool> lockedDone{false};
    std::thread lockedLifecycle([&]{const juce::MessageManagerLock callerLock;Access::armMessageCallbacks(*lockedOwner);lockedOwner.reset();lockedDone=true;});
    while(!lockedDone.load())juce::MessageManager::getInstance()->runDispatchLoopUntil(10);lockedLifecycle.join();CHECK(lockedGate.observed&&lockedGate.stopped&&lockedGate.locked);

    begin("background_teardown_respects_another_threads_dispatch_exclusion");
    struct ExcludedGate {
        InstrumentProcessor* owner=nullptr;std::atomic<bool> held{false},started{false},released{false},deleted{false},expired{false},earlyDelete{false},stopped{false};
        static void observe(void* raw,Access::Point point){auto& gate=*static_cast<ExcludedGate*>(raw);
            if(point==Access::Point::beforeMessageCallbackStop)gate.started=true;
            else if(point==Access::Point::beforeCallbackStorageRelease&&!gate.released.load())gate.earlyDelete=true;
            else if(point==Access::Point::beforeOwnerClear)gate.stopped=Access::messageCallbacksStopped(*gate.owner);
        }
    } excluded;
    auto excludedOwner=std::make_unique<InstrumentProcessor>();excluded.owner=excludedOwner.get();Access::hook(*excludedOwner,&excluded,ExcludedGate::observe);
    std::thread dispatchHolder([&]{const juce::MessageManagerLock holder;excluded.held=true;
        if(!Gate::wait(excluded.started))excluded.expired=true;
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        while(!excluded.deleted.load()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        excluded.released=true;});
    std::thread excludedLifecycle([&]{if(Gate::wait(excluded.held))excludedOwner.reset();else excluded.expired=true;excluded.deleted=true;});
    while(!excluded.deleted.load())juce::MessageManager::getInstance()->runDispatchLoopUntil(10);dispatchHolder.join();excludedLifecycle.join();
    CHECK(!excluded.expired&&excluded.held&&excluded.started&&excluded.released&&excluded.deleted);CHECK(!excluded.earlyDelete&&excluded.stopped);
}


struct SpacePause {
    std::atomic<bool> entered{false},release{false},expired{false};
    static void wait(void* raw,std::uint64_t,space::detail::PreparationControl control) noexcept {
        auto& gate=*static_cast<SpacePause*>(raw);gate.entered.store(true,std::memory_order_release);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!gate.release.load(std::memory_order_acquire)){if(control.isCancelled&&control.isCancelled(control.context))return;
            if(std::chrono::steady_clock::now()>deadline){gate.expired=true;return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    bool observe(){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));return entered.load();}
    ~SpacePause(){release=true;}
};
void routedBlock(RoutedSynthEngine& engine,int frames=512,int note=-1){juce::AudioBuffer<float> audio(2,frames);audio.clear();juce::MidiBuffer midi;
    if(note>=0)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(127)),0);engine.render(audio,midi,123.);}
void completeSpaceIdentity(){
    begin("pending_space_complete_identities_and_watermark");AssetPtr a,b;AssetError error;
    CHECK(makeSampleAsset(std::vector<float>(8192,.25f),1,a,error));CHECK(makeSampleAsset(std::vector<float>(8192,.5f),1,b,error));
    for(unsigned operation=0;operation<6;++operation){
        SpacePause gate;auto engine=std::make_unique<RoutedSynthEngine>();RoutedPatch before;before.synth.masterLevel=1;before.synth.layers[0].enabled=true;before.synth.layers[0].level=1;before.drumSource[0]=true;
        BorrowedDrumAssignments old{},replacement{};old[0][0]=a.get();replacement[0][0]=b.get();PublicationIdentity first,second;first.generation=11;second.generation=12;
        for(std::size_t n=0;n<64;++n){first.targets[n]=101+n;second.targets[n]=201+n;}
        CHECK(engine->setPatch(before,old,first));CHECK(RoutedDrumTestAccess::prepareSpace(*engine,{SpacePause::wait,&gate}));routedBlock(*engine,8,36);
        CHECK(engine->drumReferenceReport(7).adoptedGeneration==11);
        auto after=before;CHECK(editRackLayout(after.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::spaceReverb},after.layout)==RackEditResult::applied);after.effects[0].spaceReverb.mix=0;
        CHECK(!engine->setPatch(after,replacement,second));CHECK(gate.observe());auto pending=RoutedDrumTestAccess::snapshot(*engine);
        CHECK(pending.pendingValid&&pending.pendingIdentity==second&&pending.committedIdentity==first&&pending.pendingAssignments==replacement);
        CHECK(engine->drumReferenceReport(7).adoptedGeneration==11);CHECK(engine->installedDrumIdentity().targets[0]==first.targets[0]);
        if(operation==1){CHECK(engine->setPatch(before,old,first));CHECK(!RoutedDrumTestAccess::snapshot(*engine).pendingValid);}
        else if(operation==2){engine->reset();CHECK(RoutedDrumTestAccess::snapshot(*engine).committedIdentity==first&&!RoutedDrumTestAccess::snapshot(*engine).pendingValid);}
        else if(operation==3){CHECK(engine->prepare(48000,512));CHECK(RoutedDrumTestAccess::snapshot(*engine).committedIdentity==first&&!RoutedDrumTestAccess::snapshot(*engine).pendingValid);}
        else{
            if(operation==4){CHECK(!engine->prepare(0,512));auto failed=RoutedDrumTestAccess::snapshot(*engine);CHECK(failed.pendingValid&&failed.pendingIdentity==second&&failed.committedIdentity==first);}
            gate.release=true;bool accepted=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while(std::chrono::steady_clock::now()<deadline){if(engine->setPatch(after,replacement,second)){accepted=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            CHECK(accepted);auto admitted=RoutedDrumTestAccess::snapshot(*engine);CHECK(admitted.installPending&&admitted.previousValid&&admitted.previousIdentity==first&&admitted.pendingIdentity==second);
            CHECK(engine->drumReferenceReport(7).adoptedGeneration==11);CHECK(engine->installedDrumIdentity().targets[0]==0);
            if(operation==5){engine->reset();CHECK(RoutedDrumTestAccess::snapshot(*engine).committedIdentity==second);}
            else{routedBlock(*engine,17);CHECK(engine->drumReferenceReport(7).adoptedGeneration==11);routedBlock(*engine,512);routedBlock(*engine,512);}
            auto complete=engine->drumReferenceReport(7);CHECK(complete.complete&&complete.adoptedGeneration==12&&complete.references[0]==b.get());
            CHECK(engine->installedDrumIdentity().targets[0]==second.targets[0]);CHECK(!engine->setPatch(before,old,first));
        }
        gate.release=true;CHECK(!gate.expired);engine->clearDrumBorrowersQuiescent();auto cleared=RoutedDrumTestAccess::snapshot(*engine);
        CHECK(cleared.committedIdentity==PublicationIdentity{}&&cleared.pendingIdentity==PublicationIdentity{}&&cleared.previousIdentity==PublicationIdentity{});
        for(const auto& bank:cleared.references)for(const auto* pointer:bank)CHECK(pointer==nullptr);
    }
}
#if defined(_MSC_VER) && defined(_DEBUG)

thread_local bool trackControl=false,denyControl=false,trackCallback=false,calibratingAllocations=false;
thread_local std::uint64_t controlAllocations=0,supportedAllocations=0,failAllocation=0,deniedAllocations=0,proxyAllocations=0;
thread_local unsigned pendingDenialWarnings=0;
thread_local int pendingDenialWarningType=0;
thread_local int lastDeniedType=0;thread_local std::size_t lastDeniedBytes=0;thread_local void* lastDeniedData=nullptr;
std::atomic<bool> trackRuntime{false};std::atomic<std::uint64_t> callbackAllocations{0},callbackFrees{0};
enum class FaultContext {control,hostSave,presetSave};
thread_local FaultContext faultContext=FaultContext::control;
struct AllocationEvent {std::size_t bytes=0;unsigned short count=0;int type=0;FaultContext context=FaultContext::control;std::array<void*,40> frames{};};
std::array<AllocationEvent,8192> allocationEvents;
std::array<std::array<void*,3>,256> unsupportedProxyCallers{};std::size_t unsupportedProxyCallerCount=0;
struct FrameworkSignature {FaultContext context=FaultContext::control;int type=0;unsigned short count=0;bool attributes=false;std::array<void*,32> frames{};bool operator==(const FrameworkSignature&)const=default;};
std::array<FrameworkSignature,128> unsupportedSaveCallers{};std::size_t unsupportedSaveCallerCount=0;
FrameworkSignature optionalPresetBuffer;thread_local bool deniedOptionalPresetBuffer=false;
bool matchesSignature(const FrameworkSignature& signature,FaultContext context,int type,const void* const* frames,unsigned count) noexcept {
    if(!signature.count||signature.context!=context||signature.type!=type)return false;
    for(unsigned at=0;at+signature.count<=count;++at){bool same=true;for(unsigned j=0;j<signature.count;++j)same&=frames[at+j]==signature.frames[j];if(same)return true;}return false;
}
bool observedSaveFramework(FaultContext context,int type,const void* const* frames,unsigned count) noexcept {
    if((type!=_HOOK_ALLOC&&type!=_HOOK_REALLOC)||context==FaultContext::control)return false;
    for(std::size_t n=0;n<unsupportedSaveCallerCount;++n){const auto& signature=unsupportedSaveCallers[n];if(signature.context!=context||signature.type!=type)continue;
        for(unsigned at=0;at+signature.count<=count;++at){bool same=true;for(unsigned j=0;j<signature.count;++j)same&=frames[at+j]==signature.frames[j];if(same)return true;}}
    return false;
}
thread_local bool insideAllocationHook=false;
std::atomic<unsigned> runtimeReports{0},expectedDenialWarnings{0};std::array<wchar_t,2048> lastRuntimeReport{};
int __cdecl runtimeReport(int type,wchar_t* message,int* value){
    const std::wstring_view text(message?message:L"");
    const bool known=pendingDenialWarningType==_HOOK_ALLOC
        ?(text==L"Client hook allocation failure.\n"||text==L"Client hook allocation failure.\r\n"||text==L"Client hook allocation failure.")
        :pendingDenialWarningType==_HOOK_REALLOC&&(text==L"Client hook re-allocation failure.\n"||text==L"Client hook re-allocation failure.\r\n"||text==L"Client hook re-allocation failure.");
    if(type==_CRT_WARN&&trackControl&&(failAllocation||denyControl)&&pendingDenialWarnings!=0&&known){--pendingDenialWarnings;if(!pendingDenialWarnings)pendingDenialWarningType=0;++expectedDenialWarnings;}
    else{++runtimeReports;std::size_t n=0;while(message&&message[n]&&n+1<lastRuntimeReport.size()){lastRuntimeReport[n]=message[n];++n;}lastRuntimeReport[n]=0;}
    if(value)*value=0;return 1;
}
int allocationHook(int type,void* data,size_t bytes,int use,long,const unsigned char*,int){
    if(insideAllocationHook)return 1;insideAllocationHook=true;
    struct Exit {~Exit(){insideAllocationHook=false;}} exit;
    if(data&&(type==_HOOK_FREE||type==_HOOK_REALLOC))use=_CrtReportBlockType(data);
    use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK)return 1;
    if(type==_HOOK_ALLOC||type==_HOOK_REALLOC){
        if(trackRuntime&&(trackCallback||detail::DspPipeline::renderingOnThisThread()))callbackAllocations.fetch_add(1);
        if(trackControl){
            std::array<void*,40> frames{};const auto count=RtlCaptureStackBackTrace(0,static_cast<DWORD>(frames.size()),frames.data(),nullptr);
            bool proxy=false;for(unsigned n=0;n+1<count;++n)for(std::size_t k=0;k<unsupportedProxyCallerCount;++k){const auto& signature=unsupportedProxyCallers[k];
                proxy|=frames[n]==signature[0]&&frames[n+1]==signature[1]&&(!signature[2]||(n+2<count&&frames[n+2]==signature[2]));}
            proxy|=observedSaveFramework(faultContext,type,frames.data(),count);
            const auto event=controlAllocations++;if(calibratingAllocations&&event<allocationEvents.size())allocationEvents[event]={bytes,count,type,faultContext,frames};
            if(proxy)++proxyAllocations;else ++supportedAllocations;
            if(denyControl||(!proxy&&failAllocation&&supportedAllocations==failAllocation)){++deniedAllocations;++pendingDenialWarnings;pendingDenialWarningType=type;lastDeniedType=type;lastDeniedBytes=bytes;lastDeniedData=data;deniedOptionalPresetBuffer=matchesSignature(optionalPresetBuffer,faultContext,type,frames.data(),count);return 0;}
        }
    }else if(type==_HOOK_FREE&&trackRuntime&&(trackCallback||detail::DspPipeline::renderingOnThisThread()))callbackFrees.fetch_add(1);
    return 1;
}
std::uint64_t classifyUnsupportedProxyCalls(std::size_t events){
    auto module=LoadLibraryW(L"dbghelp.dll");CHECK(module!=nullptr);if(!module)return 0;
    const auto init=reinterpret_cast<decltype(&SymInitializeW)>(GetProcAddress(module,"SymInitializeW"));
    const auto load=reinterpret_cast<decltype(&SymLoadModuleExW)>(GetProcAddress(module,"SymLoadModuleExW"));
    const auto symbol=reinterpret_cast<decltype(&SymFromAddr)>(GetProcAddress(module,"SymFromAddr"));
    const auto lineFor=reinterpret_cast<decltype(&SymGetLineFromAddr64)>(GetProcAddress(module,"SymGetLineFromAddr64"));
    const auto cleanup=reinterpret_cast<decltype(&SymCleanup)>(GetProcAddress(module,"SymCleanup"));
    CHECK(init&&load&&symbol&&lineFor&&cleanup);if(!init||!load||!symbol||!lineFor||!cleanup){FreeLibrary(module);return 0;}
    wchar_t path[MAX_PATH]{};CHECK(GetModuleFileNameW(nullptr,path,MAX_PATH)>0);const auto executable=juce::File(juce::String(path));
    const auto process=GetCurrentProcess();const bool ready=init(process,executable.getParentDirectory().getFullPathName().toWideCharPointer(),FALSE);CHECK(ready);
    if(!ready){FreeLibrary(module);return 0;}
    CHECK(load(process,nullptr,path,nullptr,reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr)),0,nullptr,0)!=0);
    unsupportedProxyCallerCount=0;unsupportedSaveCallerCount=0;optionalPresetBuffer={};
    for(std::size_t n=0;n<events&&n<allocationEvents.size();++n){const auto& event=allocationEvents[n];
        std::array<std::string,40> names;std::array<unsigned,40> sourceLines{};std::array<std::string,40> sourceFiles;
        for(unsigned frame=0;frame<event.count;++frame){alignas(SYMBOL_INFO)std::array<std::byte,sizeof(SYMBOL_INFO)+2048> storage{};
            auto& info=*reinterpret_cast<SYMBOL_INFO*>(storage.data());info.SizeOfStruct=sizeof(SYMBOL_INFO);info.MaxNameLen=2048;DWORD64 displacement=0;
            if(!symbol(process,reinterpret_cast<DWORD64>(event.frames[frame]),&displacement,&info))continue;
            names[frame]=info.Name;IMAGEHLP_LINE64 currentLine{};currentLine.SizeOfStruct=sizeof(currentLine);DWORD currentDisplacement=0;
            if(lineFor(process,reinterpret_cast<DWORD64>(event.frames[frame]),&currentDisplacement,&currentLine)&&currentLine.FileName){sourceFiles[frame]=currentLine.FileName;sourceLines[frame]=currentLine.LineNumber;}
            if(std::strstr(info.Name,"_Container_base12::_Alloc_proxy<")==nullptr)continue;

            if(frame+1>=event.count)continue;const auto pc=event.frames[frame];
            IMAGEHLP_LINE64 caller{};caller.SizeOfStruct=sizeof(caller);DWORD sourceDisplacement=0;
            if(!lineFor(process,reinterpret_cast<DWORD64>(event.frames[frame+1]),&sourceDisplacement,&caller)||!caller.FileName)continue;
            const auto callerFile=juce::File(caller.FileName).getFileName();const auto callerLine=caller.LineNumber;
            bool noThrow=callerFile=="vector"&&((callerLine>=670&&callerLine<=676)||(callerLine>=759&&callerLine<=766));void* outer=nullptr;
            noThrow|=callerFile=="xstring"&&callerLine>=1027&&callerLine<=1048;
            if(callerFile=="xstring"&&callerLine>=855&&callerLine<=859&&frame+2<event.count){
                IMAGEHLP_LINE64 ctor{};ctor.SizeOfStruct=sizeof(ctor);
                if(lineFor(process,reinterpret_cast<DWORD64>(event.frames[frame+2]),&sourceDisplacement,&ctor)&&ctor.FileName
                    &&juce::File(ctor.FileName).getFileName()=="xstring"&&ctor.LineNumber>=707&&ctor.LineNumber<=715){noThrow=true;outer=event.frames[frame+2];}}
            if(!noThrow){std::cout<<"RECOVERABLE_OR_UNKNOWN_PROXY allocation="<<n+1<<" caller="<<caller.FileName<<':'<<callerLine<<'\n';continue;}
            const std::array<void*,3> signature{pc,event.frames[frame+1],outer};
            if(std::find(unsupportedProxyCallers.begin(),unsupportedProxyCallers.begin()+unsupportedProxyCallerCount,signature)==unsupportedProxyCallers.begin()+unsupportedProxyCallerCount){
                CHECK(unsupportedProxyCallerCount<unsupportedProxyCallers.size());if(unsupportedProxyCallerCount<unsupportedProxyCallers.size())unsupportedProxyCallers[unsupportedProxyCallerCount++]=signature;}
            std::cout<<"UNSUPPORTED_DEBUG_PROXY allocation="<<n+1<<" bytes="<<event.bytes<<" pc="<<pc<<" symbol="<<info.Name
                <<" caller_pc="<<signature[1]<<" outer_pc="<<outer<<" caller_source="<<caller.FileName<<':'<<callerLine<<'\n';
        }
        if((event.type==_HOOK_ALLOC||event.type==_HOOK_REALLOC)&&event.context!=FaultContext::control){
            const auto contains=[&](unsigned f,const char* name){return f<event.count&&names[f].find(name)!=std::string::npos;};
            const auto find=[&](const char* name){for(unsigned f=0;f<event.count;++f)if(contains(f,name))return f;return unsigned(event.count);};
            if(event.context==FaultContext::presetSave&&event.type==_HOOK_ALLOC){
                const auto first=find("`juce::HeapBlock<char,0>::mallocWrapper'::"),ctor=find("juce::FileOutputStream::FileOutputStream"),caller=find("airack::InstrumentProcessor::saveSoundPreset");
                if(first+4==ctor&&caller<event.count&&caller-first+1<=32&&contains(first+1,"juce::HeapBlock<char,0>::wrapper<")
                    &&contains(first+2,"juce::HeapBlock<char,0>::mallocWrapper")&&contains(first+3,"juce::HeapBlock<char,0>::HeapBlock<")
                    &&juce::File(sourceFiles[first]).getFileName()=="juce_HeapBlock.h"&&sourceLines[first]==356
                    &&juce::File(sourceFiles[ctor]).getFileName()=="juce_FileOutputStream.cpp"&&sourceLines[ctor]==42
                    &&find("juce::File::createOutputStream")<caller){
                    CHECK(!optionalPresetBuffer.count);optionalPresetBuffer.context=event.context;optionalPresetBuffer.type=event.type;optionalPresetBuffer.count=static_cast<unsigned short>(caller-first+1);
                    std::copy_n(event.frames.begin()+first,optionalPresetBuffer.count,optionalPresetBuffer.frames.begin());
                    std::cout<<"OPTIONAL_PRESET_STREAM_BUFFER original="<<n+1<<" bytes="<<event.bytes<<" counted_eligible=1 signature=";
                    for(unsigned f=first;f<=caller;++f)std::cout<<event.frames[f]<<':'<<names[f]<<'@'<<sourceFiles[f]<<':'<<sourceLines[f]<<';';std::cout<<'\n';
                }
            }
            for(unsigned route=0;route<4;++route){const bool named=route==0,copy=route==2,destructor=route==3;
            if(event.type!=(destructor?_HOOK_REALLOC:_HOOK_ALLOC))continue;
            const auto first=find(destructor?"`juce::HeapBlock<char,1>::reallocWrapper'::":named?"`juce::HeapBlock<juce::NamedValue,0>::mallocWrapper'::":"`juce::HeapBlock<juce::var,0>::mallocWrapper'::");
            const auto caller=find(event.context==FaultContext::hostSave?"airack::InstrumentProcessor::getStateInformation":"airack::InstrumentProcessor::saveSoundPreset");
            unsigned orderedAt=first+4;bool copyOrdered=true;
            if(copy)for(const char* role:{"juce::ArrayBase<juce::var,juce::DummyCriticalSection>::setAllocatedSizeInternal",
                "juce::ArrayBase<juce::var,juce::DummyCriticalSection>::setAllocatedSize","juce::ArrayBase<juce::var,juce::DummyCriticalSection>::ensureAllocatedSize",
                "juce::ArrayBase<juce::var,juce::DummyCriticalSection>::addArray<juce::var>","juce::Array<juce::var,juce::DummyCriticalSection,0>::Array<",
                "juce::var::VariantType::RefCountedArray::RefCountedArray","juce::var::var","std::pair<char const *,juce::var>::pair<",
                "airack::encodeRackDesignObject","airack::encodeRackDesign","writePersonalities","airack::encodeSoundState","airack::encodeProjectState",
                "airack::InstrumentProcessor::encodeOwnedDrumState",event.context==FaultContext::hostSave?"airack::InstrumentProcessor::getStateInformation":"airack::InstrumentProcessor::saveSoundPreset"}){
                if(event.context==FaultContext::presetSave&&std::string_view(role)=="airack::encodeProjectState")continue;
                while(orderedAt<event.count&&!contains(orderedAt,role))++orderedAt;if(orderedAt==event.count){copyOrdered=false;break;}++orderedAt;}
            if(destructor){const auto dtor=first+6,sound=find("airack::encodeSoundState"),project=find("airack::encodeProjectState"),owned=find("airack::InstrumentProcessor::encodeOwnedDrumState");
                const bool serializer=(contains(dtor+1,"writePersonalities")&&sourceLines[dtor+1]==114
                    ||contains(dtor+1,"airack::encodeSoundState")&&sourceLines[dtor+1]==221)
                    &&juce::File(sourceFiles[dtor+1]).getFileName()=="SoundState.cpp";
                const bool chain=first+7<event.count&&caller<event.count&&caller-first+1<=32&&serializer
                    &&contains(first+1,"juce::HeapBlock<char,1>::wrapper<")&&contains(first+2,"juce::HeapBlock<char,1>::reallocWrapper")
                    &&contains(first+3,"juce::HeapBlock<char,1>::realloc<")&&contains(first+4,"juce::MemoryBlock::setSize")
                    &&contains(first+5,"juce::MemoryOutputStream::trimExternalBlockSize")&&contains(dtor,"juce::MemoryOutputStream::~MemoryOutputStream")
                    &&juce::File(sourceFiles[first]).getFileName()=="juce_HeapBlock.h"&&sourceLines[first]==366
                    &&juce::File(sourceFiles[first+4]).getFileName()=="juce_MemoryBlock.cpp"&&sourceLines[first+4]==139
                    &&juce::File(sourceFiles[first+5]).getFileName()=="juce_MemoryOutputStream.cpp"&&sourceLines[first+5]==71
                    &&sourceLines[dtor]==60&&dtor<sound&&sound<owned&&owned<caller
                    &&(event.context!=FaultContext::hostSave||(sound<project&&project<owned));
                if(!chain)continue;
            }
            const bool common=destructor||(named?(find("juce::NamedValueSet::set")<caller&&find("juce::DynamicObject::setProperty")<caller)
                :copy?(find("juce::ArrayBase<juce::var,juce::DummyCriticalSection>::addArray<juce::var>")<caller
                    &&find("juce::Array<juce::var,juce::DummyCriticalSection,0>::Array<")<caller
                    &&find("juce::var::VariantType::RefCountedArray::RefCountedArray")<caller
                    &&find("juce::var::var")<caller&&find("std::pair<char const *,juce::var>::pair<")<caller)
                    :find("juce::Array<juce::var,juce::DummyCriticalSection,0>::add")<caller)
                &&find("airack::encodeRackDesignObject")<caller&&find("writePersonalities")<caller&&find("airack::encodeSoundState")<caller
                &&find("airack::InstrumentProcessor::encodeOwnedDrumState")<caller
                &&(event.context!=FaultContext::hostSave||find("airack::encodeProjectState")<caller);
            if(first+4<event.count&&caller<event.count&&caller>=first&&caller-first+1<=32&&common&&copyOrdered&&(destructor||(
                contains(first+1,named?"juce::HeapBlock<juce::NamedValue,0>::wrapper<":"juce::HeapBlock<juce::var,0>::wrapper<")
                &&contains(first+2,named?"juce::HeapBlock<juce::NamedValue,0>::mallocWrapper":"juce::HeapBlock<juce::var,0>::mallocWrapper")
                &&contains(first+3,named?"juce::HeapBlock<juce::NamedValue,0>::HeapBlock<":"juce::HeapBlock<juce::var,0>::HeapBlock<")
                &&contains(first+4,named?"juce::ArrayBase<juce::NamedValue,juce::DummyCriticalSection>::setAllocatedSizeInternal":"juce::ArrayBase<juce::var,juce::DummyCriticalSection>::setAllocatedSizeInternal")
                &&juce::File(sourceFiles[first]).getFileName()=="juce_HeapBlock.h"&&sourceLines[first]==356
                &&juce::File(sourceFiles[first+4]).getFileName()=="juce_ArrayBase.h"&&sourceLines[first+4]==436))){
                FrameworkSignature signature;signature.context=event.context;signature.type=event.type;signature.count=static_cast<unsigned short>(caller-first+1);
                std::copy_n(event.frames.begin()+first,signature.count,signature.frames.begin());
                if(std::find(unsupportedSaveCallers.begin(),unsupportedSaveCallers.begin()+unsupportedSaveCallerCount,signature)==unsupportedSaveCallers.begin()+unsupportedSaveCallerCount){
                    CHECK(unsupportedSaveCallerCount<unsupportedSaveCallers.size());if(unsupportedSaveCallerCount<unsupportedSaveCallers.size())unsupportedSaveCallers[unsupportedSaveCallerCount++]=signature;
                    std::cout<<"OBSERVED_UNSUPPORTED_SAVE_FRAMEWORK specialization="<<(destructor?"charTrueDestructor":named?"NamedValue":copy?"varArrayCopy":"varAdd")<<" context="<<int(event.context)<<" request="<<n+1<<" type="<<event.type<<" bytes="<<event.bytes<<" signature=";
                    for(unsigned f=first;f<=caller;++f)std::cout<<event.frames[f]<<':'<<names[f]<<'@'<<sourceFiles[f]<<':'<<sourceLines[f]<<';';std::cout<<'\n';
                }
            }
            }
            if(event.context==FaultContext::presetSave&&event.type==_HOOK_ALLOC){
                const auto string=find("juce::StringHolderUtils::createUninitialisedBytes"),atts=find("juce::WindowsFileHelpers::getAtts"),caller=find("airack::InstrumentProcessor::saveSoundPreset");
                const auto first=string>=2?string-2:unsigned(event.count);
                const bool core=atts==first+7&&caller<event.count&&caller>=atts&&caller-first+1<=32&&names[first]=="operator new"&&names[first+1]=="operator new[]"
                    &&contains(first+3,"juce::StringHolderUtils::makeUniqueWithByteSize")&&contains(first+4,"juce::String::preallocateBytes")
                    &&contains(first+5,"juce::StringEncodingConverter<juce::CharPointer_UTF8,juce::CharPointer_UTF16>::convert")&&contains(first+6,"juce::String::toWideCharPointer")
                    &&juce::File(sourceFiles[string]).getFileName()=="juce_String.cpp"&&sourceLines[string]==82&&sourceLines[first+5]==2056
                    &&juce::File(sourceFiles[atts]).getFileName()=="juce_Files_windows.cpp"&&sourceLines[atts]==81;
                const bool directory=caller==atts+3&&contains(atts+1,"juce::File::isDirectory")&&sourceLines[atts+1]==285&&contains(atts+2,"juce::File::createDirectory")&&sourceLines[atts+2]==533;
                const bool temporary=caller==atts+6&&contains(atts+1,"juce::File::exists")&&sourceLines[atts+1]==273&&contains(atts+2,"juce::File::getNonexistentChildFile")&&sourceLines[atts+2]==627
                    &&contains(atts+3,"juce::createTempFile")&&sourceLines[atts+3]==44&&contains(atts+4,"juce::TemporaryFile::TemporaryFile")&&sourceLines[atts+4]==62
                    &&contains(atts+5,"juce::TemporaryFile::TemporaryFile")&&sourceLines[atts+5]==51;
                const bool overwrite=caller==atts+4&&contains(atts+1,"juce::File::exists")&&sourceLines[atts+1]==273&&contains(atts+2,"juce::File::replaceFileIn")&&sourceLines[atts+2]==328
                    &&contains(atts+3,"juce::TemporaryFile::overwriteTargetFileWithTemporary")&&sourceLines[atts+3]==105;
                if(core&&(directory||temporary||overwrite)){FrameworkSignature signature;signature.context=event.context;signature.type=event.type;signature.attributes=true;signature.count=static_cast<unsigned short>(caller-first+1);
                    std::copy_n(event.frames.begin()+first,signature.count,signature.frames.begin());
                    if(std::find(unsupportedSaveCallers.begin(),unsupportedSaveCallers.begin()+unsupportedSaveCallerCount,signature)==unsupportedSaveCallers.begin()+unsupportedSaveCallerCount){
                        CHECK(unsupportedSaveCallerCount<unsupportedSaveCallers.size());if(unsupportedSaveCallerCount<unsupportedSaveCallers.size())unsupportedSaveCallers[unsupportedSaveCallerCount++]=signature;
                        std::cout<<"OBSERVED_UNSUPPORTED_SAVE_FRAMEWORK specialization=getAtts context="<<int(event.context)<<" request="<<n+1<<" type="<<event.type<<" bytes="<<event.bytes<<" signature=";
                        for(unsigned f=first;f<=caller;++f)std::cout<<event.frames[f]<<':'<<names[f]<<'@'<<sourceFiles[f]<<':'<<sourceLines[f]<<';';std::cout<<'\n';
                    }
                }
            }
        }
    }
    cleanup(process);FreeLibrary(module);if(!CHECK(unsupportedProxyCallerCount>0))return 0;
    std::uint64_t supported=0,excluded=0;
    for(std::size_t n=0;n<events&&n<allocationEvents.size();++n){bool proxy=false;
        for(unsigned f=0;f+1<allocationEvents[n].count;++f)for(std::size_t k=0;k<unsupportedProxyCallerCount;++k){const auto& signature=unsupportedProxyCallers[k];
            proxy|=allocationEvents[n].frames[f]==signature[0]&&allocationEvents[n].frames[f+1]==signature[1]
                &&(!signature[2]||(f+2<allocationEvents[n].count&&allocationEvents[n].frames[f+2]==signature[2]));}
        proxy|=observedSaveFramework(allocationEvents[n].context,allocationEvents[n].type,allocationEvents[n].frames.data(),allocationEvents[n].count);
        if(proxy)++excluded;else{++supported;std::cout<<"SUPPORTED_CHECKPOINT point="<<supported<<" original="<<n+1<<" bytes="<<allocationEvents[n].bytes<<" hook_type="<<allocationEvents[n].type<<" pcs=";
            for(unsigned f=0;f<allocationEvents[n].count;++f)std::cout<<allocationEvents[n].frames[f]<<',';std::cout<<'\n';}}
    std::cout<<"ALLOCATION_CLASSIFICATION all="<<events<<" supported="<<supported<<" excluded_observed_runtime="<<excluded<<" exact_proxy_pcs="<<unsupportedProxyCallerCount<<" exact_save_signatures="<<unsupportedSaveCallerCount<<'\n';return supported;
}
void allocationTransactions(){
    begin("history_allocations_precede_commit");_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);Fixture f;auto a=prepare(f.p(),.25f);a.metadata.source=generated();CHECK(apply(f,0,0,a));
    f.p().prepareToPlay(48000,64);callbackAndDrain(f);auto b=prepare(f.p(),.5f);b.metadata.source=generated();const auto target=f.target();DrumCommitError error;
    std::array<void*,40> warm{};(void)RtlCaptureStackBackTrace(0,40,warm.data(),nullptr);
    auto previous=_CrtSetAllocHook(allocationHook);controlAllocations=0;supportedAllocations=0;failAllocation=0;deniedAllocations=0;calibratingAllocations=true;trackControl=true;
    Access::hook(f.p(),nullptr,[](void*,Access::Point point){if(point==Access::Point::afterSourceCas)trackControl=false;});
    const bool accepted=f.p().assignPreparedDrumPad(target,b.ticket,b.metadata,{},error);trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);Access::hook(f.p(),nullptr,nullptr);CHECK(accepted);
    const auto allCheckpoints=controlAllocations;const auto checkpoints=classifyUnsupportedProxyCalls(static_cast<std::size_t>(allCheckpoints));
    std::cout<<"ALLOCATION_CALIBRATION all="<<allCheckpoints<<" supported="<<checkpoints<<'\n';CHECK(checkpoints>0);CHECK(f.p().undoSoundEdit());callbackAndDrain(f);
    for(std::uint64_t point=1;point<=checkpoints;++point){
        auto next=prepare(f.p(),.5f);next.metadata.source=generated();const auto fresh=f.target();Access::drain(f.p());const auto before=Access::snapshot(f.p());
        std::cout<<"ALLOCATION_DENIAL point="<<point<<'\n';
        CHECK(pendingDenialWarnings==0);previous=_CrtSetAllocHook(allocationHook);controlAllocations=0;supportedAllocations=0;proxyAllocations=0;deniedAllocations=0;failAllocation=point;trackControl=true;
        const bool committed=f.p().assignPreparedDrumPad(fresh,next.ticket,next.metadata,{},error);trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);
        CHECK(deniedAllocations==1&&!committed&&error!=DrumCommitError::none&&pendingDenialWarnings==0);CHECK(*before==*Access::snapshot(f.p()));f.p().discardPreparedDrumAsset(next.ticket);
    }
    previous=_CrtSetAllocHook(allocationHook);controlAllocations=0;supportedAllocations=0;deniedAllocations=0;failAllocation=1;trackControl=true;
    void* payload=std::malloc(16);trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);
    CHECK(payload==nullptr&&controlAllocations==1&&supportedAllocations==1&&deniedAllocations==1&&pendingDenialWarnings==0);std::free(payload);
    std::cout<<"REAL_16_BYTE_PAYLOAD counted="<<controlAllocations<<" declined="<<deniedAllocations<<'\n';
    auto next=prepare(f.p(),.75f);next.metadata.source=generated();const auto fresh=f.target();
    std::cout<<"ALLOCATION_POST_CAS_DENIAL begin\n";
    previous=_CrtSetAllocHook(allocationHook);controlAllocations=0;deniedAllocations=0;failAllocation=0;
    Access::hook(f.p(),nullptr,[](void*,Access::Point point){if(point==Access::Point::afterSourceCas){trackControl=true;denyControl=true;}});
    const bool committed=f.p().assignPreparedDrumPad(fresh,next.ticket,next.metadata,{},error);trackControl=false;denyControl=false;_CrtSetAllocHook(previous);Access::hook(f.p(),nullptr,nullptr);
    CHECK(committed&&deniedAllocations==0&&controlAllocations==0);std::cout<<"CONTROL_ALLOCATION_STAGING checkpoints="<<checkpoints<<" post_cas_allocations="<<controlAllocations<<'\n';f.p().releaseResources();_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);if(runtimeReports)std::wcerr<<lastRuntimeReport.data()<<'\n';CHECK(runtimeReports==0);std::cout<<"EXPECTED_CRT_DENIAL_WARNINGS "<<expectedDenialWarnings<<'\n';
}
void failedPrepareAuthority(){
    begin("failed_reprepare_preserves_released_raw_authority");Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));
    f.p().prepareToPlay(48000,64);callbackAndDrain(f);f.p().releaseResources();auto before=Access::snapshot(f.p());CHECK(before->published==nullptr);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);const auto previous=_CrtSetAllocHook(allocationHook);
    controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=1;trackControl=true;
    f.p().prepareToPlay(48000,64);trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);
    CHECK(deniedAllocations==1&&pendingDenialWarnings==0);CHECK(*before==*Access::snapshot(f.p()));
    f.p().prepareToPlay(48000,64);CHECK(Access::snapshot(f.p())->published!=nullptr);CHECK(constant(process(f.p(),4,36),.25f));f.p().releaseResources();
}
void runtimeReportControls(){
    begin("crt_report_hook_requires_single_active_denial_credit");const auto unexpected=runtimeReports.load(),expected=expectedDenialWarnings.load();CHECK(pendingDenialWarnings==0);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);const auto previous=_CrtSetAllocHook(allocationHook);
    controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=1;trackControl=true;void* payload=std::malloc(16);
    trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);CHECK(payload==nullptr&&deniedAllocations==1&&pendingDenialWarnings==0);
    CHECK(expectedDenialWarnings==expected+1&&runtimeReports==unexpected);
    _CrtDbgReportW(_CRT_WARN,nullptr,0,nullptr,L"Client hook allocation failure.\n");
    trackControl=true;failAllocation=1;
    _CrtDbgReportW(_CRT_WARN,nullptr,0,nullptr,L"Client hook allocation failure.\n");
    _CrtDbgReportW(_CRT_WARN,nullptr,0,nullptr,L"Deliberate unexpected warning control\n");
    trackControl=false;failAllocation=0;pendingDenialWarnings=1;
    _CrtDbgReportW(_CRT_WARN,nullptr,0,nullptr,L"Client hook allocation failure.\n");
    CHECK(runtimeReports==unexpected+4&&expectedDenialWarnings==expected+1&&pendingDenialWarnings==1);
    pendingDenialWarnings=1;pendingDenialWarningType=_HOOK_ALLOC;trackControl=true;failAllocation=1;
    _CrtDbgReportW(_CRT_WARN,nullptr,0,nullptr,L"Client hook re-allocation failure.\n");
    trackControl=false;failAllocation=0;CHECK(runtimeReports==unexpected+5&&pendingDenialWarnings==1&&pendingDenialWarningType==_HOOK_ALLOC);
    pendingDenialWarningType=_HOOK_REALLOC;trackControl=true;failAllocation=1;
    _CrtDbgReportW(_CRT_WARN,nullptr,0,nullptr,L"Client hook allocation failure.\n");
    trackControl=false;failAllocation=0;CHECK(runtimeReports==unexpected+6&&pendingDenialWarnings==1&&pendingDenialWarningType==_HOOK_REALLOC);
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);
    pendingDenialWarnings=0;pendingDenialWarningType=0;std::cout<<"CRT_REPORT_CONTROLS actual_denial_expected=1 outside_window_unexpected=1 duplicate_unexpected=1 unrelated_unexpected=1 stale_credit_outside_window_unexpected=1 wrong_allocation_type_unexpected=1 wrong_reallocation_type_unexpected=1\n";
    runtimeReports=unexpected;std::free(payload);
}
void applicationReallocControl(){
    auto* original=static_cast<unsigned char*>(std::malloc(1024));CHECK(original!=nullptr);if(!original)return;std::memset(original,0x5a,1024);
    CHECK(pendingDenialWarnings==0);lastDeniedType=0;lastDeniedBytes=0;lastDeniedData=nullptr;const auto previous=_CrtSetAllocHook(allocationHook);controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=1;trackControl=true;
    void* replacement=std::realloc(original,448);trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);
    CHECK(replacement==nullptr&&controlAllocations==1&&supportedAllocations==1&&deniedAllocations==1&&pendingDenialWarnings==0);
    CHECK(lastDeniedType==_HOOK_REALLOC&&lastDeniedBytes==448&&lastDeniedData==original);
    if(!replacement){for(unsigned n=0;n<1024;++n)CHECK(original[n]==0x5a);std::free(original);}else std::free(replacement);
    std::cout<<"REAL_NON_NULL_REALLOC_PAYLOAD old_bytes=1024 requested_bytes=448 counted="<<controlAllocations<<" declined="<<deniedAllocations<<" actual_hook_type="<<lastDeniedType<<" actual_old_pointer_matched="<<(lastDeniedData==original)<<" context="<<int(faultContext)<<'\n';
}
void applicationStringControl(){
    for(bool string:{true,false}){juce::String original=juce::String::repeatedString("supported application string ",20);const auto expected=original.toStdString();bool caught=false;
        CHECK(pendingDenialWarnings==0);controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=1;lastDeniedType=0;
        const auto previous=_CrtSetAllocHook(allocationHook);trackControl=true;
        try {if(string){const auto wide=original.toWideCharPointer();volatile auto first=*wide;(void)first;}else{auto array=std::make_unique<char[]>(435);array[0]='x';}}
        catch(const std::bad_alloc&){caught=true;}
        trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);
        CHECK(caught&&controlAllocations==1&&supportedAllocations==1&&deniedAllocations==1&&lastDeniedType==_HOOK_ALLOC&&pendingDenialWarnings==0);
        CHECK(original.toStdString()==expected);
        std::cout<<"REAL_THROWING_STRING_NEW_ARRAY case="<<(string?"StringUTF16":"newArray435")<<" context="<<int(faultContext)<<" caught="<<caught<<" actual_hook_type="<<lastDeniedType<<" counted="<<supportedAllocations<<" declined="<<deniedAllocations<<'\n';
    }
}
bool verifySaveCalibration(std::size_t events){
    const auto expectedAttributes=faultContext==FaultContext::presetSave?3u:0u;
    bool valid=CHECK(unsupportedSaveCallerCount==10+expectedAttributes);unsigned destructors=0,attributes=0;
    for(std::size_t n=0;n<unsupportedSaveCallerCount;++n){const auto& signature=unsupportedSaveCallers[n];destructors+=signature.type==_HOOK_REALLOC;attributes+=signature.attributes;bool matched=false;
        for(std::size_t e=0;e<events&&e<allocationEvents.size();++e){const auto& event=allocationEvents[e];
            if(event.context!=signature.context||event.type!=signature.type)continue;
            for(unsigned f=0;f<event.count;++f)if(event.frames[f]==signature.frames[0])matched|=observedSaveFramework(event.context,event.type,event.frames.data(),event.count);}
        valid=CHECK(matched)&&valid;
    }
    valid=CHECK(destructors==2&&attributes==expectedAttributes)&&valid;std::cout<<"SAVE_SIGNATURE_MATCH_CONTROLS signatures="<<unsupportedSaveCallerCount<<" destructor_realloc="<<destructors<<" getAtts="<<attributes<<" every_actual_event_matched="<<valid<<'\n';return valid;
}
void hostSaveDenials(){
    begin("host_save_destination_and_diagnostic_at_each_checkpoint");Fixture source;auto a=prepare(source.p(),.25f);a.metadata.source=generated();CHECK(apply(source,0,0,a));
    juce::MemoryBlock good;source.p().getStateInformation(good);CHECK(good.getSize()>32);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);juce::MemoryBlock output;faultContext=FaultContext::hostSave;
    auto previous=_CrtSetAllocHook(allocationHook);controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=0;calibratingAllocations=true;trackControl=true;
    source.p().getStateInformation(output);trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);CHECK(output==good);
    const auto checkpoints=classifyUnsupportedProxyCalls(static_cast<std::size_t>(controlAllocations));CHECK(checkpoints>0);
    if(!verifySaveCalibration(static_cast<std::size_t>(controlAllocations))){faultContext=FaultContext::control;_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);return;}
    for(const auto bytes:{128u,192u}){previous=_CrtSetAllocHook(allocationHook);controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=1;trackControl=true;void* payload=std::malloc(bytes);
        trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);CHECK(payload==nullptr&&controlAllocations==1&&supportedAllocations==1&&deniedAllocations==1&&pendingDenialWarnings==0);std::free(payload);
        std::cout<<"REAL_SAVE_BOUNDARY_PAYLOAD bytes="<<bytes<<" counted=1 declined=1\n";}
    applicationReallocControl();
    for(std::uint64_t point=1;point<=checkpoints;++point){std::cout<<"HOST_SAVE_DENIAL point="<<point<<'\n';auto p=std::make_unique<InstrumentProcessor>();CHECK(p->restoreProjectState(good.getData(),int(good.getSize())));
        const auto before=Access::snapshot(*p);const std::array<std::byte,3> old{std::byte{1},std::byte{2},std::byte{3}};juce::MemoryBlock destination(old.data(),old.size());
        previous=_CrtSetAllocHook(allocationHook);controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=point;trackControl=true;
        p->getStateInformation(destination);trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);
        CHECK(deniedAllocations==1);CHECK(destination.getSize()==old.size()&&std::memcmp(destination.getData(),old.data(),old.size())==0);
        CHECK(*before==*Access::snapshot(*p));CHECK(Access::status(*p)!=DrumCommitError::none);
    }
    faultContext=FaultContext::control;_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);CHECK(runtimeReports==0);std::cout<<"HOST_SAVE_FAULTS supported="<<checkpoints<<" warnings="<<expectedDenialWarnings<<'\n';
}
void presetSaveCalibration(){
    begin("preset_save_positive_caller_allocation_calibration");Fixture f;auto a=prepare(f.p(),.25f);a.metadata.source=generated();CHECK(apply(f,0,0,a));
    const auto file=fixtureFile("preset-save-fault.airack");auto before=Access::snapshot(f.p());auto document=std::make_unique<SoundDocument>(before->project.sound);
    document->name=file.getFileNameWithoutExtension();CHECK(f.p().applySoundEdit(*document,SoundEditOrigin::manual,before->edit));Access::clear(f.p());
    const std::array<std::byte,3> old{std::byte{1},std::byte{2},std::byte{3}};CHECK(file.replaceWithData(old.data(),old.size()));
    faultContext=FaultContext::presetSave;controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;calibratingAllocations=true;
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);const auto previous=_CrtSetAllocHook(allocationHook);trackControl=true;
    const bool saved=f.p().saveSoundPreset(file);trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);
    CHECK(saved&&deniedAllocations==0&&controlAllocations<allocationEvents.size());const auto all=controlAllocations;
    const auto supported=classifyUnsupportedProxyCalls(static_cast<std::size_t>(all));
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);faultContext=FaultContext::control;
    CHECK(runtimeReports==0);juce::MemoryBlock bytes;CHECK(file.loadFileAsData(bytes));CHECK(bytes.getSize()>8&&std::memcmp(bytes.getData(),"ARBNDL01",8)==0);
    std::cout<<"PRESET_SAVE_POSITIVE all="<<all<<" currently_supported="<<supported<<" actual_file_written=1 denials=0\n";CHECK(file.deleteFile());
}
void presetRenameDenials(){
    begin("successful_preset_file_commit_contains_separate_live_name_edit_failure");Fixture f;auto a=prepare(f.p(),.25f);a.metadata.source=generated();CHECK(apply(f,0,0,a));
    auto initial=Access::snapshot(f.p());auto sound=std::make_unique<SoundDocument>(initial->project.sound);sound->name="Original live sound before the separate preset naming action";
    CHECK(f.p().applySoundEdit(*sound,SoundEditOrigin::manual,initial->edit));auto candidate=prepare(f.p(),.75f);
    const auto file=fixtureFile("separate-name-fault.airack");const std::array<std::byte,3> old{std::byte{1},std::byte{2},std::byte{3}};CHECK(file.replaceWithData(old.data(),old.size()));
    const auto observe=[](void*,Access::Point point){if(point==Access::Point::afterPresetFileCommit)trackControl=true;
        if(point==Access::Point::afterSourceCas)trackControl=false;};
    Access::hook(f.p(),nullptr,observe);_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);faultContext=FaultContext::control;
    controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;calibratingAllocations=true;trackControl=false;
    auto previous=_CrtSetAllocHook(allocationHook);const bool calibrated=f.p().saveSoundPreset(file);trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);
    Access::hook(f.p(),nullptr,nullptr);const auto all=controlAllocations;CHECK(calibrated&&all>0&&all<allocationEvents.size());
    const auto points=classifyUnsupportedProxyCalls(static_cast<std::size_t>(all));CHECK(points>0&&unsupportedSaveCallerCount==0);
    juce::MemoryBlock expected;CHECK(file.loadFileAsData(expected));CHECK(expected.getSize()>8&&std::memcmp(expected.getData(),"ARBNDL01",8)==0);
    CHECK(f.p().undoSoundEdit());const auto before=Access::snapshot(f.p());CHECK(before->project.sound.name==sound->name&&before->history.cursor<before->history.count);
    for(std::uint64_t point=1;point<=points;++point){std::cout<<"PRESET_NAMING_DENIAL point="<<point<<'\n';CHECK(pendingDenialWarnings==0);
        Access::hook(f.p(),nullptr,observe);controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=0;failAllocation=point;trackControl=false;
        previous=_CrtSetAllocHook(allocationHook);bool saved=false,escaped=false;try{saved=f.p().saveSoundPreset(file);}catch(...){escaped=true;}
        trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);Access::hook(f.p(),nullptr,nullptr);
        juce::MemoryBlock actual;const bool fileSaved=file.loadFileAsData(actual)&&actual==expected;const auto after=Access::snapshot(f.p());const bool unchanged=*before==*after;
        CHECK(saved&&!escaped&&deniedAllocations==1&&pendingDenialWarnings==0);CHECK(fileSaved&&unchanged);
        if(!saved||escaped||!fileSaved||!unchanged){std::cout<<"NAMING_FAILURE_DETAIL point="<<point<<" saved="<<saved<<" escaped="<<escaped<<" file_saved="<<fileSaved<<" state_same="<<unchanged
            <<" project="<<(before->project==after->project)<<" history="<<(before->history==after->history)<<" slots="<<(before->slots==after->slots)<<" owners="<<(before->owners==after->owners)
            <<" sources="<<before->source<<','<<after->source<<" edits="<<before->edit<<','<<after->edit<<" generation="<<before->generation<<','<<after->generation
            <<" group="<<(before->groupBaseline==after->groupBaseline)<<" group_edit="<<before->groupEdit<<','<<after->groupEdit<<" reserved="<<before->reserved<<','<<after->reserved
            <<" workspace="<<before->workspace<<','<<after->workspace<<" destination_bytes="<<actual.getSize()<<','<<expected.getSize()<<'\n';break;}
    }
    CHECK(runtimeReports==0);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);
    std::cout<<"PRESET_NAMING_FAULTS checkpoints="<<points<<" file_commit_separate_from_live_edit=1\n";CHECK(file.deleteFile());
}
void presetSaveDenials(){
    begin("preset_save_pre_file_commit_denials_preserve_old_file_state_and_diagnostic");Fixture source;auto a=prepare(source.p(),.25f);a.metadata.source=generated();CHECK(apply(source,0,0,a));
    const auto file=fixtureFile("preset-save-fault.airack");auto initial=Access::snapshot(source.p());auto sound=std::make_unique<SoundDocument>(initial->project.sound);
    sound->name=file.getFileNameWithoutExtension();CHECK(source.p().applySoundEdit(*sound,SoundEditOrigin::manual,initial->edit));Access::clear(source.p());
    juce::MemoryBlock good;source.p().getStateInformation(good);auto calibration=std::make_unique<InstrumentProcessor>();CHECK(calibration->restoreProjectState(good.getData(),int(good.getSize())));auto held=prepare(*calibration,.75f);
    const auto observe=[](void*,Access::Point point){if(point==Access::Point::afterPresetFileCommit)trackControl=false;};
    const std::array<std::byte,3> old{std::byte{1},std::byte{2},std::byte{3}};CHECK(file.replaceWithData(old.data(),old.size()));
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);faultContext=FaultContext::presetSave;Access::hook(*calibration,nullptr,observe);
    controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;calibratingAllocations=true;
    auto previous=_CrtSetAllocHook(allocationHook);trackControl=true;const bool positive=calibration->saveSoundPreset(file);trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);
    Access::hook(*calibration,nullptr,nullptr);const auto all=controlAllocations;CHECK(positive&&all>0&&all<allocationEvents.size());
    const auto points=classifyUnsupportedProxyCalls(static_cast<std::size_t>(all));CHECK(points>0);
    if(!verifySaveCalibration(static_cast<std::size_t>(all))){faultContext=FaultContext::control;_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);CHECK(file.deleteFile());return;}
    juce::MemoryBlock expected;CHECK(file.loadFileAsData(expected));
    if(!CHECK(optionalPresetBuffer.count>0&&expected.getSize()>=32768)){faultContext=FaultContext::control;_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);CHECK(file.deleteFile());return;}
    for(auto bytes:{std::size_t(128),std::size_t(192)}){controlAllocations=supportedAllocations=deniedAllocations=0;failAllocation=1;
        previous=_CrtSetAllocHook(allocationHook);trackControl=true;void* payload=std::malloc(bytes);trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);
        CHECK(payload==nullptr&&controlAllocations==1&&supportedAllocations==1&&deniedAllocations==1&&pendingDenialWarnings==0);std::free(payload);
        std::cout<<"REAL_PRESET_BOUNDARY_PAYLOAD bytes="<<bytes<<" counted=1 declined=1\n";}
    applicationReallocControl();
    applicationStringControl();
    for(std::uint64_t point=1;point<=points;++point){std::cout<<"PRESET_SAVE_DENIAL point="<<point<<'\n';auto p=std::make_unique<InstrumentProcessor>();CHECK(p->restoreProjectState(good.getData(),int(good.getSize())));
        auto candidate=prepare(*p,.75f);const auto before=Access::snapshot(*p);CHECK(Access::status(*p)==DrumCommitError::none);CHECK(file.replaceWithData(old.data(),old.size()));
        Access::hook(*p,nullptr,observe);controlAllocations=supportedAllocations=deniedAllocations=0;deniedOptionalPresetBuffer=false;failAllocation=point;
        previous=_CrtSetAllocHook(allocationHook);trackControl=true;bool saved=false,escaped=false;try{saved=p->saveSoundPreset(file);}catch(...){escaped=true;}
        trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);Access::hook(*p,nullptr,nullptr);
        juce::MemoryBlock actual;const bool fileSame=file.loadFileAsData(actual)&&actual.getSize()==old.size()&&std::memcmp(actual.getData(),old.data(),old.size())==0;
        const bool unchanged=*before==*Access::snapshot(*p);
        const bool optionalSuccess=deniedOptionalPresetBuffer&&lastDeniedType==_HOOK_ALLOC&&lastDeniedBytes==32768&&expected.getSize()>=32768;
        const bool correctResult=optionalSuccess?saved&&actual==expected&&Access::status(*p)==DrumCommitError::none:!saved&&fileSame&&Access::status(*p)!=DrumCommitError::none;
        CHECK(!escaped&&deniedAllocations==1&&pendingDenialWarnings==0);CHECK(correctResult&&unchanged);
        if(optionalSuccess)std::cout<<"OPTIONAL_PRESET_BUFFER_DENIED point="<<point<<" actual_type="<<lastDeniedType<<" bytes="<<lastDeniedBytes<<" single_write="<<expected.getSize()<<" exact_saved_bytes="<<(actual==expected)<<" saved="<<saved<<" unchanged="<<unchanged<<" status="<<int(Access::status(*p))<<" denied="<<deniedAllocations<<" pending_warning="<<pendingDenialWarnings<<'\n';
        if(escaped||!correctResult||!unchanged||deniedAllocations!=1||pendingDenialWarnings!=0){std::cout<<"PRESET_SAVE_FAILURE_DETAIL point="<<point<<" saved="<<saved<<" escaped="<<escaped<<" denied="<<deniedAllocations<<" pending_warning="<<pendingDenialWarnings<<" file_same="<<fileSame<<" state_same="<<unchanged<<'\n';break;}
    }
    faultContext=FaultContext::control;_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);CHECK(runtimeReports==0&&pendingDenialWarnings==0);
    std::cout<<"PRESET_SAVE_FAULTS supported="<<points<<" before_file_commit_only=1\n";CHECK(file.deleteFile());
}
void callbackHeap(){
    begin("callback_worker_heap_and_owner_type_exclusion");
    for(unsigned quality=0;quality<2;++quality)for(bool parallel:{false,true}){Fixture f(4);for(unsigned bank=0;bank<4;++bank){auto candidate=prepare(f.p(),.0625f,8192);CHECK(apply(f,bank,0,candidate));}
        if(parallel)for(unsigned slot=0;slot<2;++slot){auto snapshot=Access::snapshot(f.p());CHECK(f.p().applyRackEdit({RackEditKind::addEffect,static_cast<std::uint8_t>(slot),masterChain,static_cast<std::uint8_t>(slot),EffectKind::dynamicEq},snapshot->edit)==RackEditResult::applied);}
        f.p().control(758).setValue(float(quality));f.p().setNonRealtime(true);f.p().prepareToPlay(48000,512);
        juce::AudioBuffer<float> audio(2,512);audio.clear();juce::MidiBuffer midi;midi.ensureSize(4096);
        for(unsigned note=36;note<52;++note)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(127)),0);
        callbackAllocations=0;callbackFrees=0;auto previous=_CrtSetAllocHook(allocationHook);trackRuntime=true;
        const auto render=[&]{trackCallback=true;f.p().processBlock(audio,midi);trackCallback=false;midi.clear();};
        for(unsigned block=0;block<8;++block)render();
        // Installation, held old tail, full feedback queue and rejected stale Apply.
        const auto stale=f.target();auto replacement=prepare(f.p(),.03125f,8192);CHECK(apply(f,0,0,replacement));
        for(unsigned block=0;block<3;++block)render();auto rejected=prepare(f.p(),.015625f,8192);DrumCommitError error;
        CHECK(!f.p().assignPreparedDrumPad(stale,rejected.ticket,rejected.metadata,{},error)&&error==DrumCommitError::staleTarget);
        f.p().control(drumModeParameterStart).setValue(0);f.p().control(drumModeParameterStart).setValue(1);render();
        CHECK(f.target().token!=stale.token);render();
        f.p().control(drumModeParameterStart).setValue(0);render();f.p().control(drumModeParameterStart).setValue(1);render();
        midi.addEvent(juce::MidiMessage::midiStop(),0);render();
        // A real control transaction paused in its odd source-CAS window forces
        // both callback reads to fail while the installed old view remains live.
        const auto target=f.target();Pause gate;Access::hook(f.p(),&gate,[](void* raw,Access::Point point){if(point==Access::Point::afterSourceCas)static_cast<Pause*>(raw)->wait();});
        bool committed=false;std::thread control([&]{DrumCommitError failure;committed=f.p().assignPreparedDrumPad(target,rejected.ticket,rejected.metadata,{},failure);});
        CHECK(gate.observe());render();gate.release=true;control.join();Access::hook(f.p(),nullptr,nullptr);CHECK(committed&&!gate.expired);render();
        trackCallback=false;trackRuntime=false;_CrtSetAllocHook(previous);CHECK(callbackAllocations==0&&callbackFrees==0);
        CHECK(!parallel||Access::workers(f.p()).blocks>0);std::cout<<"CALLBACK_HEAP quality="<<quality<<" parallel="<<parallel<<" allocations="<<callbackAllocations<<" frees="<<callbackFrees<<" worker_blocks="<<Access::workers(f.p()).blocks<<'\n';f.p().releaseResources();
    }
    begin("space_pending_cancel_install_callback_heap");AssetPtr a,b;AssetError error;
    CHECK(makeSampleAsset(std::vector<float>(8192,.25f),1,a,error));CHECK(makeSampleAsset(std::vector<float>(8192,.5f),1,b,error));
    for(bool cancel:{true,false}){SpacePause gate;auto engine=std::make_unique<RoutedSynthEngine>();RoutedPatch before;
        before.synth.masterLevel=1;before.synth.layers[0].enabled=true;before.synth.layers[0].level=1;before.drumSource[0]=true;
        BorrowedDrumAssignments old{},replacement{};old[0][0]=a.get();replacement[0][0]=b.get();PublicationIdentity first,second;
        first.generation=11;second.generation=12;first.targets.fill(101);second.targets.fill(201);
        CHECK(engine->setPatch(before,old,first));CHECK(RoutedDrumTestAccess::prepareSpace(*engine,{SpacePause::wait,&gate}));
        auto after=before;CHECK(editRackLayout(after.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::spaceReverb},after.layout)==RackEditResult::applied);after.effects[0].spaceReverb.mix=0;
        juce::AudioBuffer<float> audio(2,512);audio.clear();juce::MidiBuffer midi;midi.ensureSize(4096);
        midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(127)),0);
        callbackAllocations=callbackFrees=0;const auto previous=_CrtSetAllocHook(allocationHook);trackRuntime=true;
        const auto render=[&]{trackCallback=true;engine->render(audio,midi,123.);trackCallback=false;midi.clear();};render();
        trackCallback=true;const bool initial=engine->setPatch(after,replacement,second);trackCallback=false;CHECK(!initial&&gate.observe());render();
        if(cancel){trackCallback=true;const bool accepted=engine->setPatch(before,old,first);trackCallback=false;CHECK(accepted);render();}
        else {gate.release=true;bool accepted=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while(std::chrono::steady_clock::now()<deadline){trackCallback=true;accepted=engine->setPatch(after,replacement,second);trackCallback=false;
                if(accepted)break;render();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            CHECK(accepted);for(unsigned n=0;n<4;++n)render();const auto installed=engine->installedDrumIdentity();CHECK(installed.generation==second.generation&&installed.targets[0]==second.targets[0]);
            for(unsigned n=16;n<64;++n)CHECK(installed.targets[n]==0);}
        trackCallback=false;trackRuntime=false;_CrtSetAllocHook(previous);CHECK(callbackAllocations==0&&callbackFrees==0);
        std::cout<<"SPACE_CALLBACK_HEAP cancel="<<cancel<<" allocations="<<callbackAllocations<<" frees="<<callbackFrees<<'\n';gate.release=true;engine->clearDrumBorrowersQuiescent();CHECK(!gate.expired);
    }
}
namespace ownershipHeap {
struct Row {long request=0;std::size_t bytes=0;};
struct Result {std::size_t live=0,peak=0,overlap=0,allocated=0,freed=0,baselineFreed=0,allocations=0,reallocations=0;bool valid=true;};
static std::array<Row,32768> rows;
Result result;
std::atomic<bool> recording{false};thread_local bool inside=false;
std::atomic_flag ledgerGate=ATOMIC_FLAG_INIT;std::atomic<DWORD> ownerThread{0};unsigned observation=0;
std::atomic<std::size_t> otherAllocations{0},otherFrees{0},otherAllocatedBytes{0},otherFreedBytes{0};
void remove(long request,std::size_t size) noexcept {
    auto found=std::find_if(rows.begin(),rows.end(),[&](auto r){return r.request==request;});
    if(found==rows.end()){result.baselineFreed+=size;return;}
    if(found->bytes>result.live){result.valid=false;return;}
    result.live-=found->bytes;result.freed+=found->bytes;*found={};
}
int __cdecl hook(int kind,void* data,std::size_t bytes,int use,long request,const unsigned char*,int){
    if(!recording.load(std::memory_order_acquire)||inside)return 1;inside=true;struct Exit{~Exit(){inside=false;}}exit;
    long oldRequest=0;std::size_t oldBytes=0;bool valid=true;
    // Resolve CRT block identity before the private ledger gate. The gate's
    // critical section performs only fixed-array/scalar operations.
    if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){use=_CrtReportBlockType(data);if(use==-1)valid=false;
        if(valid){oldBytes=_msize_dbg(data,use);valid=oldBytes<=UINT_MAX&&_CrtIsMemoryBlock(data,unsigned(oldBytes),&oldRequest,nullptr,nullptr);}}
    use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK)return 1;
    while(ledgerGate.test_and_set(std::memory_order_acquire)){}
    struct Unlock{~Unlock(){ledgerGate.clear(std::memory_order_release);}}unlock;
    if(!recording.load(std::memory_order_acquire))return 1;if(!valid){result.valid=false;return 1;}
    if(GetCurrentThreadId()!=ownerThread.load()){
        if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){++otherAllocations;otherAllocatedBytes+=bytes;}
        if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){++otherFrees;otherFreedBytes+=oldBytes;}}
    if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){
        if(kind==_HOOK_REALLOC){++result.reallocations;result.overlap=std::max(result.overlap,result.live+bytes);if(data)remove(oldRequest,oldBytes);}
        auto row=std::find_if(rows.begin(),rows.end(),[](auto r){return r.request==0;});
        if(request<=0||row==rows.end()){result.valid=false;return 1;}*row={request,bytes};
        result.live+=bytes;result.allocated+=bytes;++result.allocations;result.peak=std::max(result.peak,result.live);
    }else if(kind==_HOOK_FREE&&data)remove(oldRequest,oldBytes);return 1;
}
template<class F> Result observe(F&& work){
    rows.fill({});result={};inside=false;otherAllocations=0;otherFrees=0;otherAllocatedBytes=0;otherFreedBytes=0;ownerThread=GetCurrentThreadId();_CrtMemState before{},after{};_CrtMemCheckpoint(&before);
    const auto old=_CrtSetAllocHook(hook);recording.store(true,std::memory_order_release);
    try{work();}catch(...){recording=false;_CrtSetAllocHook(old);throw;}
    recording.store(false,std::memory_order_release);while(ledgerGate.test_and_set(std::memory_order_acquire)){}const auto out=result;ledgerGate.clear(std::memory_order_release);
    _CrtSetAllocHook(old);_CrtMemCheckpoint(&after);
    const auto delta=std::int64_t(after.lSizes[_NORMAL_BLOCK])-std::int64_t(before.lSizes[_NORMAL_BLOCK])
        +std::int64_t(after.lSizes[_CLIENT_BLOCK])-std::int64_t(before.lSizes[_CLIENT_BLOCK]);
    std::cout<<"REQUESTED_HEAP_OBSERVATION number="<<++observation<<" live="<<out.live<<" baseline_freed="<<out.baselineFreed<<" checkpoint_delta="<<delta
        <<" peak="<<out.peak<<" other_allocations="<<otherAllocations<<" other_frees="<<otherFrees<<" other_allocated_bytes="<<otherAllocatedBytes<<" other_freed_bytes="<<otherFreedBytes<<'\n';
    CHECK(out.valid);CHECK(std::int64_t(out.live)-std::int64_t(out.baselineFreed)==delta);return out;
}
void calibrate(){const auto a=observe([]{juce::MemoryBlock block(1024,false);});CHECK(a.peak==1024&&a.live==0&&a.freed==1024);
    auto baseline=_malloc_dbg(80,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(baseline!=nullptr);
    const auto b=observe([&]{auto p=_malloc_dbg(64,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(p!=nullptr);
        auto q=_realloc_dbg(p,128,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(q!=nullptr);_free_dbg(q?q:p,_NORMAL_BLOCK);_free_dbg(baseline,_NORMAL_BLOCK);});
    CHECK(b.peak==128&&b.overlap==192&&b.live==0&&b.allocated==192&&b.freed==192&&b.baselineFreed==80);
}
}
void retentionMemory(bool reservationOnly=false){
    begin("full_maximum_physical_retention_and_operation_workspace");ownershipHeap::calibrate();
    constexpr std::uint64_t pcmBytes=480000ull*2*4;
    constexpr std::uint64_t decodeWork=8ull*3840052+2*maxDrumDocumentBytes+2*maxCompactDocumentBytes;
    constexpr std::uint64_t saveWork=maxDrumBundleBytes+decodeWork;
    SpacePause spaceGate;Fixture f(4);std::vector<float> input(960000);unsigned marker=0;
    const auto make=[&]{std::fill(input.begin(),input.end(),float(++marker)/1024);Prepared p;DrumCommitError error;
        CHECK(f.p().prepareDrumAsset(input,2,p.ticket,error));const auto state=Access::snapshot(f.p());
        for(std::size_t n=0;n<3;++n)if(state->tickets[n]==p.ticket)p.metadata.asset=state->candidates[n];
        p.metadata.name.assign(64,'m');p.metadata.source=generated();CHECK(p.metadata.asset.has_value());return p;};
    for(unsigned n=0;n<64;++n){auto p=make();CHECK(apply(f,n/16,n%16,p));}
    Access::clear(f.p());CHECK(Access::accounting(f.p())[0]==64*pcmBytes);
    {auto before=Access::snapshot(f.p());RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=f.racks[3];RackDesign melodic;
        melodic.name="Dormant maximum";melodic.panel.sections={"Tone"};melodic.units={{0,NativeModule::analog,{},1}};swap.design=melodic;swap.allowSlotReuse=true;
        swap.destination={RackRole::synth,masterChain,3};CHECK(f.p().applyRackOperation(swap,before->edit)==RackCandidateStatus::ready);CHECK(Access::accounting(f.p())[0]==64*pcmBytes);}
    f.p().prepareToPlay(48000,64);CHECK(Access::prepareSpace(f.p(),{SpacePause::wait,&spaceGate}));callbackAndDrain(f);
    // Sixteen different real hit borrowers survive replacement of their configured pad.
    for(unsigned n=0;n<16;++n){auto p=make();CHECK(apply(f,0,0,p));(void)process(f.p(),1,36);Access::drain(f.p());Access::clear(f.p());}
    const auto roots=Access::report(f.p());CHECK(std::count_if(roots.references.begin()+16,roots.references.begin()+48,[](auto p){return p!=nullptr;})==16);
    CHECK(Access::accounting(f.p())[0]==79*pcmBytes);
    // The actual late Space path also holds previous/pending complete views over
    // maximum-sized assets; its older watermark pins their immutable metadata.
    {const auto before=Access::snapshot(f.p());CHECK(f.p().applyRackEdit({RackEditKind::addEffect,0,masterChain,0,EffectKind::spaceReverb},before->edit)==RackEditResult::applied);}
    auto firstPending=make();CHECK(apply(f,0,0,firstPending));(void)process(f.p(),1);Access::drain(f.p());CHECK(spaceGate.observe());
    CHECK(Access::engineSnapshot(f.p()).pendingValid);spaceGate.release=true;bool previousSeen=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(std::chrono::steady_clock::now()<deadline){(void)process(f.p(),1);Access::drain(f.p());const auto view=Access::engineSnapshot(f.p());
        if(view.installPending&&view.previousValid){previousSeen=true;CHECK(view.previousIdentity.generation<view.pendingIdentity.generation);break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    CHECK(previousSeen&&!spaceGate.expired);
    // Stop callback progress. Strict generation retirement must pin every following publication.
    for(unsigned n=0;n<56;++n){auto p=make();CHECK(apply(f,0,0,p));}
    std::array<Prepared,3> candidates;for(auto& p:candidates)p=make();
    CHECK(Access::accounting(f.p())[0]==139*pcmBytes);
    struct SaveGate {Pause pause;unsigned factoryEntries=0,restoreEntries=0;} saveGate;
    auto captured=Access::snapshot(f.p());
    Access::hook(f.p(),&saveGate,[](void* raw,Access::Point point){auto& gate=*static_cast<SaveGate*>(raw);
        if(point==Access::Point::afterSaveCapture)gate.pause.wait();if(point==Access::Point::afterPreparationReservation)++gate.factoryEntries;if(point==Access::Point::afterRestoreReservation)++gate.restoreEntries;});
    juce::MemoryBlock state;std::thread save([&]{f.p().getStateInformation(state);});CHECK(saveGate.pause.observe());
    auto account=Access::accounting(f.p());CHECK(account[0]==139*pcmBytes&&account[2]==saveWork&&account[3]==1);
    std::uint64_t unchangedTicket=123;DrumCommitError error;
    CHECK(!f.p().prepareDrumAsset(input,2,unchangedTicket,error)&&error==DrumCommitError::preparationFull&&unchangedTicket==123);
    CHECK(apply(f,0,0,candidates[0]));auto before=Access::snapshot(f.p());
    // Even matching input content requires the incoming allocation reservation before factory/hash.
    CHECK(!f.p().prepareDrumAsset(input,2,unchangedTicket,error)&&error==DrumCommitError::decodedBudget&&unchangedTicket==123);
    CHECK(*before==*Access::snapshot(f.p()));CHECK(saveGate.factoryEntries==0);saveGate.pause.release=true;save.join();CHECK(!saveGate.pause.expired);
    CHECK(state.getSize()>64*pcmBytes);CHECK(Access::accounting(f.p())[2]==0);
    const auto* wire=static_cast<const std::uint8_t*>(state.getData());const auto u32=[](const std::uint8_t* p){return p[0]|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;};
    DrumDocumentPtr decoded;DrumError drumError;CHECK(decodeDrumDocument(std::span(wire+24+u32(wire+8),u32(wire+12)),decoded,drumError));
    CHECK(decoded&&decoded->slots()==captured->slots);decoded.reset();
    // A valid matching-content preset also needs D+A before decode, even though
    // canonicalization would later find its sole physical body already owned.
    {AssetPtr asset;AssetError assetError;CHECK(makeSampleAsset(input,2,asset,assetError));DrumSlots slots{};
        for(auto& bank:slots)for(auto& pad:bank){pad.asset=asset->id();pad.name="matching preset budget";}
        DrumDocumentPtr document;CHECK(makeDrumDocument(slots,document,drumError));const auto current=Access::snapshot(f.p());juce::MemoryBlock compact;
        CHECK(encodeSoundState(current->project.sound,compact));const std::array<AssetPtr,1> assets{asset};DrumBundlePtr bundle;
        CHECK(makeDrumBundle(document,std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()),assets,bundle,drumError));
        juce::MemoryBlock bytes;CHECK(encodeDrumBundle(*bundle,bytes,drumError));const auto file=fixtureFile("matching-max-pressure.airack");CHECK(file.replaceWithData(bytes.getData(),bytes.getSize()));
        {auto positive=std::make_unique<InstrumentProcessor>();CHECK(positive->loadSoundPreset(file));CHECK(Access::snapshot(*positive)->slots==slots);}
        const auto beforePreset=Access::snapshot(f.p());const bool admitted=f.p().loadSoundPreset(file);
        CHECK(!admitted&&Access::status(f.p())==DrumCommitError::decodedBudget);CHECK(*beforePreset==*Access::snapshot(f.p()));CHECK(saveGate.restoreEntries==0);CHECK(file.deleteFile());
        if(admitted){Access::hook(f.p(),nullptr,nullptr);for(unsigned n=1;n<3;++n)f.p().discardPreparedDrumAsset(candidates[n].ticket);
            f.p().releaseResources();Access::clear(f.p());std::cout<<"RESERVATION_MUTANT_PREFIX unexpected_preset_admission=1 stopped_after_quiescent_cleanup=1\n";return;}
    }
    // The full incoming decoded D+A reservation rejects before decoding while old roots still live.
    before=Access::snapshot(f.p());CHECK(!f.p().restoreProjectState(state.getData(),int(state.getSize())));CHECK(*before==*Access::snapshot(f.p()));CHECK(saveGate.restoreEntries==0);Access::hook(f.p(),nullptr,nullptr);
    for(unsigned n=1;n<3;++n)f.p().discardPreparedDrumAsset(candidates[n].ticket);
    f.p().releaseResources();Access::clear(f.p());CHECK(Access::accounting(f.p())[0]==64*pcmBytes);
    if(reservationOnly){std::cout<<"RESERVATION_MUTANT_PREFIX original_maximum_assets=139 decode_entries="<<saveGate.restoreEntries<<" stopped_after_quiescent_cleanup=1\n";return;}
    juce::MemoryBlock output(3,false);const auto oldDestination=output.getSize();
    const auto encodedHeap=ownershipHeap::observe([&]{f.p().getStateInformation(output);});
    CHECK(output.getSize()>64*pcmBytes&&std::max(encodedHeap.peak,encodedHeap.overlap)<=saveWork);
    struct RestoreObservation {InstrumentProcessor* processor;std::array<std::uint64_t,5> values{};} observed{&f.p()};
    Access::hook(f.p(),&observed,[](void* raw,Access::Point point){if(point==Access::Point::afterRestoreReservation){auto& o=*static_cast<RestoreObservation*>(raw);o.values=Access::accounting(*o.processor);}});
    bool restored=false;const auto restoredHeap=ownershipHeap::observe([&]{restored=f.p().restoreProjectState(output.getData(),int(output.getSize()));});
    Access::hook(f.p(),nullptr,nullptr);CHECK(restored);
    CHECK(observed.values[0]==129*pcmBytes&&observed.values[0]-observed.values[1]==64*pcmBytes&&observed.values[1]==65*pcmBytes&&observed.values[2]==decodeWork&&observed.values[4]==1);
    CHECK(std::max(restoredHeap.peak,restoredHeap.overlap)<=65*pcmBytes+decodeWork);CHECK(Access::accounting(f.p())[0]==64*pcmBytes);
    Pause overlap;std::array<std::uint64_t,5> overlapReservation{};
    struct Overlap {Pause* gate;InstrumentProcessor* p;std::array<std::uint64_t,5>* reservation;} both{&overlap,&f.p(),&overlapReservation};
    Access::hook(f.p(),&both,[](void* raw,Access::Point point){auto& b=*static_cast<Overlap*>(raw);if(point==Access::Point::afterSaveCapture)b.gate->wait();
        if(point==Access::Point::afterRestoreReservation)*b.reservation=Access::accounting(*b.p);});
    juce::MemoryBlock concurrentSave;std::thread saver([&]{f.p().getStateInformation(concurrentSave);});CHECK(overlap.observe());
    CHECK(f.p().restoreProjectState(output.getData(),int(output.getSize())));CHECK(overlapReservation[2]==saveWork+decodeWork&&overlapReservation[3]==1&&overlapReservation[4]==1);
    overlap.release=true;saver.join();CHECK(!overlap.expired&&concurrentSave==output);Access::hook(f.p(),nullptr,nullptr);
    f.p().prepareToPlay(48000,64);callbackAndDrain(f);f.p().releaseResources();
    std::cout<<"RETENTION_MEMORY assets=139 dormant_assets=16 pending_previous_seen="<<previousSeen<<" physical_pcm="<<139*pcmBytes<<" intended_pcm="<<64*pcmBytes<<" borrowed_prepare_input="<<input.size()*sizeof(float)
        <<" old_destination="<<oldDestination<<" save_peak="<<encodedHeap.peak<<" save_bound="<<saveWork<<" restore_peak="<<restoredHeap.peak
        <<" restore_bound="<<65*pcmBytes+decodeWork<<" borrowed_host_input="<<output.getSize()<<" save_valid="<<encodedHeap.valid<<" restore_valid="<<restoredHeap.valid<<'\n';
}
#else
void allocationTransactions(){CHECK(false);}void callbackHeap(){CHECK(false);}void retentionMemory(){CHECK(false);}void failedPrepareAuthority(){CHECK(false);}void hostSaveDenials(){CHECK(false);}void runtimeReportControls(){CHECK(false);}
#endif

juce::File fixtureFile(const char* name){const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});CHECK(root.isNotEmpty());
    const auto folder=juce::File(root).getChildFile("owned-closure-fixtures");CHECK(folder.createDirectory().wasOk());return folder.getChildFile(name);}
GroupIntent groupIntent(){GroupIntent intent;intent.group=juce::Uuid();intent.member=juce::Uuid();CHECK(setGroupText("Owned lifetime fixture",intent.name));
    CHECK(setGroupText("A long enough role to allocate its context string",intent.role));CHECK(setGroupText("C",intent.key));CHECK(setGroupText("minor",intent.scale));intent.bpm=123.;CHECK(validateGroupIntent(intent));return intent;}
void presetRoundTrip(){
    begin("preset_entrypoint_owned_action_and_host_success_clear");const auto file=fixtureFile("all64-owned.airack");DrumSlots expected;juce::MemoryBlock host;
    {Fixture source(4);for(unsigned n=0;n<64;++n){auto candidate=prepare(source.p(),float(n+1)/256,32,n%2+1);candidate.metadata.source=generated();CHECK(apply(source,n/16,n%16,candidate));}
        expected=Access::snapshot(source.p())->slots;CHECK(source.p().saveSoundPreset(file));source.p().getStateInformation(host);}
    Fixture destination;auto d=prepare(destination.p(),.875f);CHECK(apply(destination,0,0,d));auto before=Access::snapshot(destination.p());
    CHECK(destination.p().createPattern(1,before->edit));before=Access::snapshot(destination.p());CHECK(destination.p().setPlaybackMode(PlaybackMode::internalPattern,before->edit));
    CHECK(destination.p().setGroupIntent(groupIntent()));before=Access::snapshot(destination.p());CHECK(destination.p().loadSoundPreset(file));auto after=Access::snapshot(destination.p());
    CHECK(after->slots==expected&&after->project.pattern==before->project.pattern&&after->project.mode==before->project.mode&&after->project.group==before->project.group);
    const auto last=after->history.count-1;const auto* preset=after->history.identities[last];CHECK(after->history.kinds[last]==3&&after->history.masks[last]==~std::uint64_t(0));
    CHECK(std::accumulate(after->history.chargeTerms[last].begin(),after->history.chargeTerms[last].end(),std::uint64_t(0))==after->history.units[last]);
    CHECK(destination.p().undoSoundEdit());auto undone=Access::snapshot(destination.p());CHECK(undone->slots==before->slots&&undone->project.sound==before->project.sound);
    CHECK(undone->project.pattern==before->project.pattern&&undone->project.mode==before->project.mode&&undone->project.group==before->project.group);
    CHECK(destination.p().redoSoundEdit());CHECK(Access::snapshot(destination.p())->slots==expected);
    auto live=Access::snapshot(destination.p());auto pattern=live->project.pattern;pattern.swing=.2f;CHECK(destination.p().applyPatternEdit(pattern,live->edit));
    auto changedGroup=groupIntent();CHECK(destination.p().setGroupIntent(changedGroup));live=Access::snapshot(destination.p());
    CHECK(Access::retained(destination.p(),preset,true));auto merged=Access::snapshot(destination.p());CHECK(merged->project.pattern==live->project.pattern&&merged->project.group==live->project.group&&merged->project.mode==live->project.mode);
    CHECK(merged->slots==before->slots);CHECK(Access::retained(destination.p(),preset,false));
    CHECK(destination.p().restoreProjectState(host.getData(),int(host.getSize())));CHECK(Access::history(destination.p())->count==0&&Access::snapshot(destination.p())->slots==expected);
    CHECK(file.deleteFile());
}
void identicalPresetHistory(){
    for(unsigned banks:{0u,1u}){
        begin(banks?"identical_owned_preset_resets_without_history":"identical_empty_preset_resets_without_history");Fixture f(banks);
        if(banks){auto candidate=prepare(f.p(),.25f,8192);candidate.metadata.source=generated();candidate.metadata.source->sourceFrames=8192;candidate.metadata.source->end=8192;
            CHECK(apply(f,0,0,candidate));Access::clear(f.p());}
        const auto file=fixtureFile(banks?"identical-owned-history.airack":"identical-empty-history.airack");
        auto beforeKnob=Access::snapshot(f.p());CHECK(f.p().beginParameterGesture(0));CHECK(f.p().setParameterGestureValue(0,.42f));CHECK(f.p().endParameterGesture(0));
        auto unnamed=Access::snapshot(f.p());CHECK(unnamed->project.sound.values[0]==.42f&&unnamed->history.count==1);
        CHECK(f.p().saveSoundPreset(file));auto saved=Access::snapshot(f.p());
        CHECK(saved->project.sound.name==file.getFileNameWithoutExtension()&&saved->history.count==unnamed->history.count+1);
        f.p().prepareToPlay(48000,64);callbackAndDrain(f);
        const auto recall=[&]{
            const auto sounding=process(f.p(),64,banks?36:60);CHECK(banks?!constant(sounding,0):constant(sounding,0));const auto before=Access::snapshot(f.p());
            CHECK(f.p().loadSoundPreset(file));auto after=Access::snapshot(f.p());
            CHECK(after->project==before->project&&after->slots==before->slots&&after->owners==before->owners);
            CHECK(after->history==before->history);
            CHECK(after->generation==before->generation+1&&after->published!=before->published);
            CHECK(after->source!=before->source&&after->acknowledged==after->source);
            CHECK(after->edit==before->edit+1&&after->reset==before->reset+1);
            for(unsigned n=0;n<64;++n)CHECK(after->targets[n]!=before->targets[n]);
            CHECK(constant(process(f.p(),64),0));Access::drain(f.p());
        };
        recall();
        CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->project==unnamed->project);
        CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->project==beforeKnob->project);
        CHECK(f.p().redoSoundEdit());CHECK(f.p().redoSoundEdit());CHECK(Access::snapshot(f.p())->project==saved->project);
        CHECK(f.p().beginParameterGesture(0));CHECK(f.p().setParameterGestureValue(0,.63f));CHECK(f.p().endParameterGesture(0));
        auto redo=Access::snapshot(f.p());CHECK(f.p().undoSoundEdit());auto branch=Access::snapshot(f.p());
        CHECK(branch->project==saved->project&&branch->history.cursor<branch->history.count);
        recall();CHECK(f.p().redoSoundEdit());CHECK(Access::snapshot(f.p())->project==redo->project);
        f.p().releaseResources();CHECK(file.deleteFile());
    }
}
void sameNumericChangedPresetHistory(){
    for(unsigned kind=0;kind<3;++kind){
        begin(kind==0?"same_numeric_changed_pcm_preset_is_history":kind==1?"same_numeric_changed_provenance_preset_is_history":"same_numeric_changed_pad_name_preset_is_history");
        Fixture f;auto original=prepare(f.p(),.25f);original.metadata.source=generated();CHECK(apply(f,0,0,original));
        const auto file=fixtureFile(kind==0?"changed-pcm.airack":kind==1?"changed-provenance.airack":"changed-pad-name.airack");
        CHECK(f.p().saveSoundPreset(file));auto saved=Access::snapshot(f.p());Access::clear(f.p());
        auto changed=prepare(f.p(),kind==0?.5f:.25f);changed.metadata.source=original.metadata.source;
        if(kind==1)++changed.metadata.source->seed;if(kind==2)changed.metadata.name="changed pad label";
        CHECK(apply(f,0,0,changed));auto before=Access::snapshot(f.p());
        CHECK(before->project.sound==saved->project.sound&&before->slots!=saved->slots);
        CHECK(f.p().loadSoundPreset(file));auto after=Access::snapshot(f.p());
        CHECK(after->project.sound==before->project.sound&&after->slots==saved->slots);
        CHECK(after->history.count==before->history.count+1&&after->history.cursor==after->history.count);
        const auto last=after->history.count-1;CHECK(after->history.kinds[last]==3&&after->history.masks[last]==~std::uint64_t(0));
        CHECK(f.p().undoSoundEdit());CHECK(Access::snapshot(f.p())->slots==before->slots);
        CHECK(f.p().redoSoundEdit());CHECK(Access::snapshot(f.p())->slots==saved->slots);CHECK(file.deleteFile());
    }
}
void transactionalRecallFaults(){
    begin("restore_and_save_faults_are_transactional");Fixture source;auto a=prepare(source.p(),.25f);CHECK(apply(source,0,0,a));juce::MemoryBlock good;source.p().getStateInformation(good);
    Fixture destination;auto b=prepare(destination.p(),.5f);CHECK(apply(destination,0,0,b));const auto before=Access::snapshot(destination.p());
    for(const auto length:std::array<std::size_t,7>{0,4,8,23,24,good.getSize()/2,good.getSize()-1}){
        CHECK(!destination.p().restoreProjectState(good.getData(),int(length)));CHECK(*before==*Access::snapshot(destination.p()));}
    auto corrupt=good;static_cast<std::uint8_t*>(corrupt.getData())[corrupt.getSize()-1]^=1;
    CHECK(!destination.p().restoreProjectState(corrupt.getData(),int(corrupt.getSize())));CHECK(*before==*Access::snapshot(destination.p()));
    CHECK(!destination.p().restoreProjectState(nullptr,int(maxDrumBundleBytes)+1));CHECK(*before==*Access::snapshot(destination.p()));
    const auto absent=fixtureFile("absent-never-created.airack");CHECK(!destination.p().loadSoundPreset(absent));CHECK(*before==*Access::snapshot(destination.p()));
    CHECK(destination.p().restoreProjectState(good.getData(),int(good.getSize())));CHECK(Access::snapshot(destination.p())->slots==Access::snapshot(source.p())->slots);
    const auto original=good;destination.p().getStateInformation(good);CHECK(good==original);
    begin("named_empty_manifest_survives_host_and_preset");DrumSlots named{};named[2][7].name="Unassigned named pad";DrumDocumentPtr document;DrumError error;CHECK(makeDrumDocument(named,document,error));
    auto numeric=std::make_unique<ProjectDocument>();juce::MemoryBlock compact;CHECK(encodeProjectState(*numeric,compact));DrumBundlePtr bundle;
    CHECK(makeDrumBundle(document,std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()),{},bundle,error));juce::MemoryBlock encoded;CHECK(encodeDrumBundle(*bundle,encoded,error));
    CHECK(destination.p().restoreProjectState(encoded.getData(),int(encoded.getSize())));CHECK(Access::snapshot(destination.p())->slots==named);
    const auto file=fixtureFile("named-empty.airack");CHECK(destination.p().saveSoundPreset(file));auto recalled=std::make_unique<InstrumentProcessor>();CHECK(recalled->loadSoundPreset(file));CHECK(Access::snapshot(*recalled)->slots==named);CHECK(file.deleteFile());
}
void numericEmptyHostRecall(){
    const auto publicationCount=[](const Access::State& state){return std::count_if(state.publications.begin(),state.publications.end(),[](auto generation){return generation!=0;});};
    const auto freshAuthority=[](const Access::State& before,const Access::State& after){
        CHECK(after.source!=before.source&&after.acknowledged==after.source);CHECK(after.reset==before.reset+1&&after.edit==before.edit+1);
        for(unsigned n=0;n<64;++n)CHECK(after.targets[n]!=before.targets[n]);CHECK(after.history.count==0);
    };
    const auto envelope=[](const DrumSlots& slots,const juce::MemoryBlock& compact){DrumDocumentPtr document;DrumBundlePtr bundle;DrumError error;juce::MemoryBlock result;
        CHECK(makeDrumDocument(slots,document,error));CHECK(makeDrumBundle(document,std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()),{},bundle,error));
        if(bundle)CHECK(encodeDrumBundle(*bundle,result,error));return result;};
    begin("default_empty_numeric_host_recall_reuses_immutable_record_and_refreshes_authority");Fixture numeric;
    numeric.p().prepareToPlay(48000,64);callbackAndDrain(numeric);auto baseline=Access::snapshot(numeric.p());juce::MemoryBlock compact;
    CHECK(encodeProjectState(baseline->project,compact));const auto* publication=baseline->published;const auto generation=baseline->generation;const auto records=publicationCount(*baseline);
    unsigned accepted=0;DrumCommitError firstError=DrumCommitError::none;
    for(unsigned recall=0;recall<96;++recall){const auto before=Access::snapshot(numeric.p());const bool ok=numeric.p().restoreProjectState(compact.getData(),int(compact.getSize()));CHECK(ok);
        if(!ok){if(firstError==DrumCommitError::none)firstError=Access::status(numeric.p());continue;}++accepted;
        const auto after=Access::snapshot(numeric.p());CHECK(after->published==publication&&after->generation==generation&&publicationCount(*after)==records);
        CHECK(after->slots==DrumSlots{}&&after->project==baseline->project);freshAuthority(*before,*after);
    }
    std::cout<<"EMPTY_NUMERIC_RECALL calls=96 accepted="<<accepted<<" rejected="<<96-accepted<<" first_error="<<int(firstError)<<" initial_records="<<records<<'\n';CHECK(accepted==96);
    (void)process(numeric.p(),64);Access::drain(numeric.p());
    auto emptyClosed=envelope(baseline->slots,compact);auto beforeClosed=Access::snapshot(numeric.p());
    for(unsigned repeat=0;repeat<2;++repeat){const bool ok=numeric.p().restoreProjectState(emptyClosed.getData(),int(emptyClosed.getSize()));CHECK(ok);if(!ok)break;
        auto after=Access::snapshot(numeric.p());CHECK(after->published!=beforeClosed->published&&after->generation==beforeClosed->generation+1);freshAuthority(*beforeClosed,*after);beforeClosed=std::move(after);}
    numeric.p().releaseResources();

    begin("empty_numeric_reuse_keeps_all_retired_nonempty_records_until_actual_later_report");Fixture retained;auto oldest=prepare(retained.p(),.25f,4);CHECK(apply(retained,0,0,oldest));
    retained.p().prepareToPlay(48000,64);callbackAndDrain(retained);auto oldAsset=Access::weakCurrentAsset(retained.p());
    for(unsigned n=0;n<63;++n){auto replacement=prepare(retained.p(),float(n+1)/128,4);CHECK(apply(retained,0,0,replacement));}
    DrumCommitError error;CHECK(retained.p().clearDrumPad(retained.target(),error));auto full=Access::snapshot(retained.p());CHECK(full->slots==DrumSlots{}&&publicationCount(*full)==65&&!oldAsset.expired());
    juce::MemoryBlock retainedCompact;CHECK(encodeProjectState(full->project,retainedCompact));auto closedAtCapacity=envelope(full->slots,retainedCompact);
    CHECK(!retained.p().restoreProjectState(closedAtCapacity.getData(),int(closedAtCapacity.getSize())));CHECK(Access::status(retained.p())==DrumCommitError::publicationFull);CHECK(*full==*Access::snapshot(retained.p()));
    unsigned reused=0;
    for(unsigned recall=0;recall<96;++recall){auto before=Access::snapshot(retained.p());const bool ok=retained.p().restoreProjectState(retainedCompact.getData(),int(retainedCompact.getSize()));CHECK(ok);
        if(!ok)continue;++reused;auto after=Access::snapshot(retained.p());CHECK(after->published==full->published&&after->generation==full->generation&&publicationCount(*after)==65);
        CHECK(!oldAsset.expired());freshAuthority(*before,*after);}
    CHECK(reused==96&&!oldAsset.expired());
    for(unsigned block=0;block<64&&!oldAsset.expired();++block){(void)process(retained.p(),64);Access::drain(retained.p());}
    CHECK(oldAsset.expired());auto retired=Access::snapshot(retained.p());CHECK(publicationCount(*retired)==1&&retired->published==full->published&&retired->generation==full->generation);
    retained.p().releaseResources();

    begin("named_and_provenance_host_recalls_keep_strict_assignment_generations");Fixture metadata;auto ordinary=Access::snapshot(metadata.p());juce::MemoryBlock metadataCompact;CHECK(encodeProjectState(ordinary->project,metadataCompact));
    DrumSlots named{};named[0][3].name="Named without sample";auto namedClosed=envelope(named,metadataCompact);CHECK(metadata.p().restoreProjectState(namedClosed.getData(),int(namedClosed.getSize())));
    auto namedBefore=Access::snapshot(metadata.p());CHECK(metadata.p().restoreProjectState(metadataCompact.getData(),int(metadataCompact.getSize())));auto namedAfter=Access::snapshot(metadata.p());
    CHECK(namedAfter->slots==DrumSlots{}&&namedAfter->generation==namedBefore->generation+1&&namedAfter->published!=namedBefore->published);freshAuthority(*namedBefore,*namedAfter);
    auto sample=prepare(metadata.p(),.375f,4);sample.metadata.source=generated();CHECK(apply(metadata,0,0,sample));auto provenance=Access::snapshot(metadata.p());
    CHECK(metadata.p().restoreProjectState(metadataCompact.getData(),int(metadataCompact.getSize())));auto cleared=Access::snapshot(metadata.p());
    CHECK(cleared->slots==DrumSlots{}&&cleared->generation==provenance->generation+1&&cleared->published!=provenance->published);freshAuthority(*provenance,*cleared);
}
void numericEmptyHostRecallDenials(){
#if defined(_MSC_VER) && defined(_DEBUG)
    begin("default_empty_numeric_recall_supported_failures_and_post_CAS_deny_all");
    const auto makeFixture=[](){auto out=std::make_unique<Fixture>();
        for(unsigned n=0;n<2;++n){auto before=Access::snapshot(out->p());auto next=std::make_unique<SoundDocument>(before->project.sound);next->values[0]=.4f+.1f*n;
            CHECK(out->p().applySoundEdit(*next,SoundEditOrigin::manual,before->edit));}
        CHECK(out->p().undoSoundEdit());(void)prepare(out->p(),.625f);CHECK(out->p().setGroupIntent(groupIntent()));return out;};
    auto calibration=makeFixture(),rejected=makeFixture();const auto positiveBefore=Access::snapshot(calibration->p()),before=Access::snapshot(rejected->p());
    CHECK(before->slots==DrumSlots{}&&before->history.cursor<before->history.count&&before->tickets[0]!=0);
    auto incoming=std::make_unique<ProjectDocument>();incoming->sound.name="Empty numeric compatibility recall with staged name and context";incoming->sound.values[0]=.73f;
    auto group=groupIntent();CHECK(setGroupText(std::string(96,'r'),group.role));CHECK(setGroupText(std::string(96,'k'),group.key));CHECK(setGroupText(std::string(96,'s'),group.scale));incoming->group=group;
    juce::MemoryBlock bytes;CHECK(encodeProjectState(*incoming,bytes));const auto contextBefore=rejected->p().aiMusicalContext();const auto groupBefore=rejected->p().groupLinkView();
    const auto checkSuccess=[&](InstrumentProcessor& p,const Access::State& old){auto after=Access::snapshot(p);
        CHECK(after->project==*incoming&&after->slots==old.slots&&after->owners==old.owners&&after->publications==old.publications&&after->published==old.published&&after->generation==old.generation);
        CHECK(after->source!=old.source&&after->acknowledged==after->source&&after->reset==old.reset+1&&after->edit==old.edit+1);
        for(unsigned n=0;n<64;++n)CHECK(after->targets[n]!=old.targets[n]);CHECK(after->history.count==0&&after->history.cursor==0&&after->history.storedUnits==0);
        CHECK(after->tickets==old.tickets&&after->candidates==old.candidates&&after->running==old.running&&after->cancelled==old.cancelled&&after->reserved==0&&after->workspace==0);
        const auto context=p.aiMusicalContext();CHECK(context.role==std::string(groupTextView(group.role))&&context.key==std::string(groupTextView(group.key))&&context.scale==std::string(groupTextView(group.scale))&&context.auditionBpm==group.bpm);
        CHECK(p.groupLinkView().intent==incoming->group&&p.groupLinkView().phase==GroupLinkPhase::pending);
    };
    unsigned sourceCas=0;const auto observe=[](void* raw,Access::Point point){if(point==Access::Point::afterSourceCas){++*static_cast<unsigned*>(raw);trackControl=false;}};
    Access::hook(calibration->p(),&sourceCas,observe);unsupportedProxyCallerCount=unsupportedSaveCallerCount=0;
    controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;faultContext=FaultContext::control;denyControl=false;calibratingAllocations=true;
    const auto initialReports=runtimeReports.load();_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);auto previous=_CrtSetAllocHook(allocationHook);trackControl=true;
    const bool accepted=calibration->p().restoreProjectState(bytes.getData(),int(bytes.getSize()));trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);Access::hook(calibration->p(),nullptr,nullptr);
    const auto all=controlAllocations;CHECK(accepted&&sourceCas==1&&deniedAllocations==0&&pendingDenialWarnings==0&&all>0&&all<allocationEvents.size());
    if(!accepted||sourceCas!=1||all==0||all>=allocationEvents.size()){_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);return;}
    const auto points=classifyUnsupportedProxyCalls(static_cast<std::size_t>(all));CHECK(points>0&&points<=all&&unsupportedSaveCallerCount==0);checkSuccess(calibration->p(),*positiveBefore);
    std::cout<<"EMPTY_NUMERIC_FAULT_CALIBRATION all="<<all<<" supported="<<points<<" exact_debug_proxy="<<all-points<<" save_signatures="<<unsupportedSaveCallerCount<<'\n';
    bool valid=true;
    for(std::uint64_t point=1;point<=points&&valid;++point){CHECK(*before==*Access::snapshot(rejected->p()));sourceCas=0;Access::hook(rejected->p(),&sourceCas,observe);
        controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=0;failAllocation=point;const auto reports=runtimeReports.load(),warnings=expectedDenialWarnings.load();
        std::cout<<"EMPTY_NUMERIC_FAULT_DENIAL point="<<point<<'\n';previous=_CrtSetAllocHook(allocationHook);trackControl=true;bool committed=false,escaped=false;
        try{committed=rejected->p().restoreProjectState(bytes.getData(),int(bytes.getSize()));}catch(...){escaped=true;}
        trackControl=false;failAllocation=0;_CrtSetAllocHook(previous);Access::hook(rejected->p(),nullptr,nullptr);
        const bool unchanged=*before==*Access::snapshot(rejected->p());const bool sameContext=rejected->p().aiMusicalContext()==contextBefore&&rejected->p().groupLinkView()==groupBefore;
        CHECK(!committed&&!escaped&&sourceCas==0);CHECK(deniedAllocations==1&&pendingDenialWarnings==0&&runtimeReports==reports&&expectedDenialWarnings==warnings+1);CHECK(unchanged&&sameContext);
        valid=!committed&&!escaped&&sourceCas==0&&deniedAllocations==1&&pendingDenialWarnings==0&&runtimeReports==reports&&expectedDenialWarnings==warnings+1&&unchanged&&sameContext;
    }
    if(valid){sourceCas=0;controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;
        Access::hook(rejected->p(),&sourceCas,[](void* raw,Access::Point point){if(point==Access::Point::afterSourceCas){++*static_cast<unsigned*>(raw);trackControl=denyControl=true;}
            if(point==Access::Point::afterCoreInstall)trackControl=denyControl=false;});
        previous=_CrtSetAllocHook(allocationHook);const bool committed=rejected->p().restoreProjectState(bytes.getData(),int(bytes.getSize()));trackControl=denyControl=false;_CrtSetAllocHook(previous);Access::hook(rejected->p(),nullptr,nullptr);
        CHECK(committed&&sourceCas==1&&controlAllocations==0&&deniedAllocations==0&&pendingDenialWarnings==0);if(committed)checkSuccess(rejected->p(),*before);
        std::cout<<"EMPTY_NUMERIC_POST_CAS_DENY_ALL allocations="<<controlAllocations<<" denied="<<deniedAllocations<<" supported_denials="<<points<<'\n';
    }
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);CHECK(runtimeReports==initialReports&&pendingDenialWarnings==0);
    begin("default_empty_numeric_recall_source_CAS_conflict_preserves_assignment_and_history");auto conflict=makeFixture();auto old=Access::snapshot(conflict->p());
    casWriter=&conflict->p();casWrites=0;casExternalWord=0;Access::structuralHook(externalCasWrite);
    CHECK(!conflict->p().restoreProjectState(bytes.getData(),int(bytes.getSize())));Access::structuralHook(nullptr);casWriter=nullptr;auto after=Access::snapshot(conflict->p());
    CHECK(casWrites==1&&Access::status(conflict->p())!=DrumCommitError::none&&after->source==casExternalWord&&after->source!=old->source);
    CHECK(after->project==old->project&&after->history==old->history&&after->slots==old->slots&&after->owners==old->owners&&after->published==old->published&&after->generation==old->generation);
    CHECK(after->tickets==old->tickets&&after->candidates==old->candidates&&after->reset==old->reset&&after->context==old->context&&after->reserved==0&&after->workspace==0);
#else
    CHECK(false);
#endif
}
void selfContainedRecall(){
    begin("full_recall_self_contained_after_owner_expiry");juce::MemoryBlock state;
    std::array<DrumSlots,1> slots;
    {Fixture f(4);for(unsigned n=0;n<64;++n){auto candidate=prepare(f.p(),float(n+1)/256,8,n%2+1);CHECK(apply(f,n/16,n%16,candidate));}
        slots[0]=Access::snapshot(f.p())->slots;f.p().getStateInformation(state);CHECK(state.getSize()>8&&std::memcmp(state.getData(),"ARBNDL01",8)==0);}
    auto restored=std::make_unique<InstrumentProcessor>();CHECK(restored->restoreProjectState(state.getData(),int(state.getSize())));
    CHECK(Access::snapshot(*restored)->slots==slots[0]);CHECK(Access::history(*restored)->count==0);
    restored->prepareToPlay(48000,64);CHECK(constant(process(*restored,4,36),float(1+17+33+49)/256));restored->releaseResources();
    auto numeric=std::make_unique<ProjectDocument>();std::uint64_t edit;CHECK(restored->tryProjectSnapshot(*numeric,edit));juce::MemoryBlock compact;CHECK(encodeProjectState(*numeric,compact));
    CHECK(restored->restoreProjectState(compact.getData(),int(compact.getSize())));CHECK(Access::snapshot(*restored)->slots==DrumSlots{});
}
InstrumentProcessor* casWriter=nullptr;std::uint64_t casExternalWord=0;unsigned casWrites=0;
void externalCasWrite(int point){if(point==3&&casWriter&&casWrites==0){++casWrites;casWriter->control(drumModeParameterStart).setValue(0);casWriter->control(drumModeParameterStart).setValue(1);casExternalWord=Access::sourceWord(*casWriter);}}
void sourceCasTransactions(){
    const auto file=fixtureFile("cas-owned-preset.airack");juce::MemoryBlock incoming;
    {Fixture source;auto p=prepare(source.p(),.75f);CHECK(apply(source,0,0,p));source.p().getStateInformation(incoming);CHECK(source.p().saveSoundPreset(file));}
    for(unsigned kind=0;kind<4;++kind){begin(kind==0?"pad_source_cas_rejection_keeps_redo":kind==1?"rack_source_cas_rejection_keeps_redo":kind==2?"host_source_cas_rejection_keeps_history":"preset_source_cas_rejection_keeps_redo");
        Fixture f;auto a=prepare(f.p(),.25f);CHECK(apply(f,0,0,a));auto b=prepare(f.p(),.5f);CHECK(apply(f,0,0,b));CHECK(f.p().undoSoundEdit());
        auto candidate=prepare(f.p(),.625f);const auto target=f.target();const auto before=Access::snapshot(f.p());CHECK(before->history.cursor<before->history.count);
        RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=f.racks[0];const juce::Uuid replacement;const std::array<RackOperation,2> operations{remove,placeFor(replacement)};
        const auto call=[&]{DrumCommitError error;if(kind==0)return f.p().assignPreparedDrumPad(target,candidate.ticket,candidate.metadata,{},error);
            if(kind==1)return f.p().applyRackOperations(operations,before->edit)==RackCandidateStatus::ready;
            if(kind==2)return f.p().restoreProjectState(incoming.getData(),int(incoming.getSize()));return f.p().loadSoundPreset(file);};
        casWriter=&f.p();casWrites=0;casExternalWord=0;Access::structuralHook(externalCasWrite);const bool committed=call();Access::structuralHook(nullptr);casWriter=nullptr;
        CHECK(!committed&&casWrites==1&&casExternalWord!=before->source);const auto after=Access::snapshot(f.p());
        CHECK(after->valid&&after->project==before->project&&after->history==before->history&&after->slots==before->slots&&after->owners==before->owners);
        CHECK(after->published==before->published&&after->generation==before->generation&&after->tickets==before->tickets&&after->candidates==before->candidates);
        CHECK(after->reset==before->reset&&after->context==before->context&&after->groupBaseline==before->groupBaseline&&after->groupEdit==before->groupEdit);
        CHECK(after->source==casExternalWord&&after->acknowledged==casExternalWord&&after->edit==before->edit+1);
        for(unsigned n=0;n<64;++n)CHECK(after->targets[n]!=before->targets[n]);
        if(kind==0){DrumCommitError error;CHECK(f.p().assignPreparedDrumPad(f.target(),candidate.ticket,candidate.metadata,{},error));}
        else if(kind==1)CHECK(f.p().applyRackOperations(operations,after->edit)==RackCandidateStatus::ready);
        else if(kind==2)CHECK(f.p().restoreProjectState(incoming.getData(),int(incoming.getSize())));
        else CHECK(f.p().loadSoundPreset(file));
        std::cout<<"CAS_REJECTION kind="<<kind<<" external_word="<<casExternalWord<<" candidate_preserved=1 history_preserved=1 retry=1\n";
    }
    CHECK(file.deleteFile());
}
// Parent integration: place after sourceCasTransactions(), before this namespace closes.
// Uses the existing exact-PC classifier unchanged. This cohort excludes no save-framework path.
#if defined(_MSC_VER) && defined(_DEBUG)
void operationDenials(unsigned firstKind=0)
{
    begin("caller_allocation_failures_preserve_history_redo_candidates_and_closures");
    const auto initialReports=runtimeReports.load();
    CHECK(pendingDenialWarnings==0 && !trackControl && !denyControl && !calibratingAllocations);
    faultContext=FaultContext::control;
    const auto file=fixtureFile("caller-fault.airack");
    auto incoming=std::make_unique<ProjectDocument>();
    incoming->sound.name="A complete generic owned caller failure fixture with a long saved name";
    incoming->sound.values[0]=.43f;
    auto incomingGroup=groupIntent();
    CHECK(setGroupText(std::string(96,'r'),incomingGroup.role));
    CHECK(setGroupText(std::string(96,'k'),incomingGroup.key));
    CHECK(setGroupText(std::string(96,'s'),incomingGroup.scale));
    incoming->group=incomingGroup;
    CHECK(!incoming->sound.personalities && validateProjectDocument(*incoming));
    auto expectedSlots=std::make_unique<DrumSlots>();
    AssetId incomingId{};
    juce::MemoryBlock hostBytes,presetBytes;
    {
        const std::array<float,8> samples {.875f,-.25f,.125f,0,.5f,0,-.125f,0};
        AssetPtr asset;AssetError assetError;
        if(!makeSampleAsset(samples,1,asset,assetError)){CHECK(false);return;}
        incomingId=asset->id();
        for(unsigned bank=0;bank<4;++bank)for(unsigned pad=0;pad<16;++pad){
            auto& image=(*expectedSlots)[bank][pad];image.asset=incomingId;
            image.name="generic bank "+std::to_string(bank)+" pad "+std::to_string(pad)+" caller closure";
            auto provenance=generated();provenance.sourceFrames=provenance.end=samples.size();
            provenance.generator="literal caller full closure generator with a retained identity";
            provenance.condition="generated caller metadata for every physical pad "+std::to_string(bank*16+pad);
            provenance.seed=1000+bank*16+pad;image.source=std::move(provenance);
        }
        DrumDocumentPtr document;DrumError error;
        if(!makeDrumDocument(*expectedSlots,document,error)){CHECK(false);return;}
        const std::array<AssetPtr,1> assets {asset};
        for(bool project:{true,false}){
            juce::MemoryBlock compact;
            const bool numeric=project?encodeProjectState(*incoming,compact):encodeSoundState(incoming->sound,compact);
            if(!numeric){CHECK(false);return;}
            DrumBundlePtr bundle;
            if(!makeDrumBundle(document,std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()),assets,bundle,error)){CHECK(false);return;}
            if(!encodeDrumBundle(*bundle,project?hostBytes:presetBytes,error)){CHECK(false);return;}
        }
    } // Every external AssetPtr/DrumBundle owner is gone before any public caller runs.
    CHECK(hostBytes.getSize()>32 && presetBytes.getSize()>32 && hostBytes.getSize()<131072 && presetBytes.getSize()<131072);
    if(!file.replaceWithData(presetBytes.getData(),presetBytes.getSize())){CHECK(false);return;}
    auto fileBytes=std::make_unique<juce::MemoryBlock>();
    if(!file.loadFileAsData(*fileBytes) || *fileBytes!=presetBytes){CHECK(false);CHECK(file.deleteFile());return;}

    struct CallerFixture {
        std::unique_ptr<Fixture> fixture=std::make_unique<Fixture>();
        Prepared candidate;
        juce::Uuid replacement;
        std::array<RackOperation,2> operations;
    };
    const auto makeFixture=[&]() {
        auto out=std::make_unique<CallerFixture>();auto& f=*out->fixture;
        f.p().prepareToPlay(48000,64);
        std::vector<const juce::UndoableAction*> literalIds;
        std::vector<std::uint32_t> literalUnits;
        for(unsigned n=0;n<34;++n){
            auto prepared=prepare(f.p(),n%2?.5f:.25f,32,1,"caller fixed metadata");
            // Maximal valid strings make the unchanged positive production charge cross30000 by30 entries.
            prepared.metadata.name=std::string(64,'n');auto provenance=generated();
            provenance.generator=std::string(96,'g');provenance.condition=std::string(128,'c');
            prepared.metadata.source=std::move(provenance);
            if(!apply(f,0,0,prepared)){CHECK(false);return std::unique_ptr<CallerFixture>{};}
            callbackAndDrain(f);const auto history=Access::history(f.p());
            if(history->count==0){CHECK(false);return std::unique_ptr<CallerFixture>{};}
            literalIds.push_back(history->identities[history->count-1]);literalUnits.push_back(history->units[history->count-1]);
            const auto retained=std::min(n+1,30u);CHECK(history->count==retained && history->cursor==retained);
            std::uint64_t expectedUnits=0;
            for(unsigned at=0;at<retained;++at){const auto original=n+1-retained+at;
                CHECK(history->identities[at]==literalIds[original] && history->units[at]==literalUnits[original]);
                CHECK(history->kinds[at]==1 && history->masks[at]==1 && history->rootCounts[at]==(original?2:1));
                CHECK(std::accumulate(history->chargeTerms[at].begin(),history->chargeTerms[at].end(),std::uint64_t(0))==history->units[at]);
                expectedUnits+=literalUnits[original];
            }
            CHECK(history->storedUnits==expectedUnits);
        }
        const auto full=Access::history(f.p());CHECK(full->count==30 && full->storedUnits>30000 && full->units[29]>1000);
        CHECK(f.p().undoSoundEdit());callbackAndDrain(f);
        CHECK(f.p().undoSoundEdit());callbackAndDrain(f);
        out->candidate=prepare(f.p(),.625f);out->candidate.metadata.source=generated();
        CHECK(f.p().setGroupIntent(groupIntent()));callbackAndDrain(f);
        Access::drain(f.p());const auto ready=Access::snapshot(f.p());
        CHECK(ready->history.count==30 && ready->history.cursor==28 && ready->history.storedUnits==full->storedUnits);
        CHECK(ready->retained==3*32*sizeof(float) && ready->reserved==0 && ready->workspace==0);
        CHECK(std::count_if(ready->publications.begin(),ready->publications.end(),[](auto g){return g!=0;})==1);
        CHECK(ready->slots[0][0].asset && ready->owners[0] && ready->tickets[0]!=0);
        out->operations[0].kind=RackOperationKind::remove;out->operations[0].selected=f.racks[0];
        out->operations[1]=placeFor(out->replacement);
        return out;
    };
    const auto call=[&](CallerFixture& fixture,unsigned kind,std::uint64_t edit){auto& p=fixture.fixture->p();
        if(kind==0)return p.undoSoundEdit();
        if(kind==1)return p.applyRackOperations(fixture.operations,edit)==RackCandidateStatus::ready;
        if(kind==2)return p.restoreProjectState(hostBytes.getData(),int(hostBytes.getSize()));
        return p.loadSoundPreset(file);
    };
    const auto checkSuccess=[&](CallerFixture& fixture,unsigned kind,const Access::State& before,const Access::State& after){
        auto& p=fixture.fixture->p();CHECK(after.valid && after.generation>before.generation);
        CHECK(after.tickets==before.tickets && after.candidates==before.candidates && after.running==before.running && after.cancelled==before.cancelled);
        CHECK(after.reserved==0 && after.workspace==0);
        if(kind==0){
            auto exact=std::make_unique<Access::History>(before.history);--exact->cursor;CHECK(after.history==*exact);
            const auto action=before.history.cursor-1;
            CHECK(before.history.kinds[action]==1 && before.history.rootCounts[action]==2);
            CHECK(after.slots[0][0].asset==before.history.assetIds[action][0] && after.owners[0]==before.history.roots[action][0]);
            CHECK(after.project==before.project);
        }else if(kind==2){
            CHECK(after.project==*incoming && after.slots==*expectedSlots && after.history.count==0 && after.history.cursor==0 && after.history.storedUnits==0);
            for(unsigned at=0;at<64;++at)CHECK(after.history.identities[at]==nullptr && after.history.units[at]==0 && after.history.rootCounts[at]==0);
            const auto context=p.aiMusicalContext();
            CHECK(context.role==std::string(groupTextView(incomingGroup.role)) && context.key==std::string(groupTextView(incomingGroup.key))
                && context.scale==std::string(groupTextView(incomingGroup.scale)) && context.auditionBpm==incomingGroup.bpm);
            CHECK(p.groupLinkView().intent==incoming->group && p.groupLinkView().phase==GroupLinkPhase::pending);
        }else{
            // A successful insertion drops precisely the existing two redo entries, then obeys the unchanged budget/minimum.
            std::vector<const juce::UndoableAction*> ids;std::vector<std::uint32_t> units;
            for(std::size_t at=0;at<before.history.cursor;++at){ids.push_back(before.history.identities[at]);units.push_back(before.history.units[at]);}
            CHECK(after.history.cursor!=0);if(after.history.cursor==0)return;
            const auto last=after.history.cursor-1;const auto* inserted=after.history.identities[last];
            CHECK(inserted!=before.history.identities[28] && inserted!=before.history.identities[29]);
            ids.push_back(inserted);units.push_back(after.history.units[last]);
            auto total=std::accumulate(units.begin(),units.end(),std::uint64_t(0));
            if(ids.size()>64){total-=units.front();ids.erase(ids.begin());units.erase(units.begin());}
            while(total>30000 && ids.size()>30){total-=units.front();ids.erase(ids.begin());units.erase(units.begin());}
            CHECK(after.history.count==ids.size() && after.history.cursor==ids.size() && after.history.storedUnits==total);
            for(std::size_t at=0;at<ids.size();++at){CHECK(after.history.identities[at]==ids[at] && after.history.units[at]==units[at]);
                for(std::size_t old=before.history.cursor;old<before.history.count;++old)CHECK(after.history.identities[at]!=before.history.identities[old]);}
            CHECK(std::accumulate(after.history.chargeTerms[last].begin(),after.history.chargeTerms[last].end(),std::uint64_t(0))==after.history.units[last]);
            CHECK(after.history.kinds[last]==(kind==1?2:3));
            if(kind==1){
                CHECK(after.slots[0]==DrumKit{} && after.owners[0]==nullptr && after.history.masks[last]==0xffff && after.history.rootCounts[last]==1);
                for(unsigned bank=1;bank<4;++bank)CHECK(after.slots[bank]==before.slots[bank]);
                CHECK(after.project.sound.personalities && after.project.sound.personalities->instances.size()==1);
                if(after.project.sound.personalities && !after.project.sound.personalities->instances.empty())CHECK(after.project.sound.personalities->instances.front().id==fixture.replacement);
            }else{
                CHECK(after.slots==*expectedSlots && after.project.sound==incoming->sound && after.history.masks[last]==~std::uint64_t(0));
                CHECK(after.history.rootCounts[last]==65);
                CHECK(after.project.pattern==before.project.pattern && after.project.group==before.project.group && after.project.mode==before.project.mode);
            }
        }
        if(kind>=2){for(unsigned n=0;n<64;++n){CHECK(after.slots[n/16][n%16].asset==incomingId && after.owners[n]!=nullptr);
                CHECK(after.owners[n]==after.owners[0]);}
            // Host clear legitimately drops the other historical marker; its last raw/current marker stays pinned until feedback.
            CHECK(after.retained==(kind==2?before.retained-32*sizeof(float)+8*sizeof(float):before.retained+8*sizeof(float)));}
    };

    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);
    bool stop=false;
    for(unsigned kind=firstKind;kind<4 && !stop;++kind){
        begin(kind==0?"undo_each_supported_allocation_keeps_exact_history":kind==1?"typed_compound_each_supported_allocation_keeps_both_halves":
            kind==2?"generic_full64_host_each_supported_allocation_keeps_context":"full64_preset_each_supported_allocation_keeps_redo");
        auto calibration=makeFixture();auto rejected=makeFixture();
        if(!calibration || !rejected){CHECK(false);stop=true;break;}
        const auto positiveBefore=Access::snapshot(calibration->fixture->p());
        const auto before=Access::snapshot(rejected->fixture->p());
        const auto contextBefore=rejected->fixture->p().aiMusicalContext();const auto groupBefore=rejected->fixture->p().groupLinkView();
        unsigned sourceCas=0;
        const auto observe=[](void* raw,Access::Point point){if(point==Access::Point::afterSourceCas){++*static_cast<unsigned*>(raw);trackControl=false;}};
        Access::hook(calibration->fixture->p(),&sourceCas,observe);
        std::array<void*,40> warm{};(void)RtlCaptureStackBackTrace(0,40,warm.data(),nullptr);
        unsupportedProxyCallerCount=unsupportedSaveCallerCount=0;
        controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=0;failAllocation=0;denyControl=false;
        faultContext=FaultContext::control;calibratingAllocations=true;
        const auto previous=_CrtSetAllocHook(allocationHook);bool accepted=false,escaped=false;
        trackControl=true;
        try {accepted=call(*calibration,kind,positiveBefore->edit);}catch(...){escaped=true;}
        trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(previous);Access::hook(calibration->fixture->p(),nullptr,nullptr);
        const auto all=controlAllocations;
        std::cout<<"CALLER_CALIBRATION_CAPTURE kind="<<kind<<" requests="<<all<<" capacity="<<allocationEvents.size()<<'\n';
        CHECK(accepted && !escaped && sourceCas==1 && deniedAllocations==0 && pendingDenialWarnings==0);
        CHECK(all>0 && all<allocationEvents.size());
        if(!accepted || escaped || sourceCas!=1 || all==0 || all>=allocationEvents.size()){stop=true;break;}
        const auto points=classifyUnsupportedProxyCalls(static_cast<std::size_t>(all));
        CHECK(points>0 && points<=all && unsupportedSaveCallerCount==0);
        for(std::size_t at=0;at<all;++at)CHECK(allocationEvents[at].context==FaultContext::control);
        std::cout<<"CALLER_FAULT_CALIBRATION kind="<<kind<<" all="<<all<<" supported="<<points
            <<" excluded_exact_debug_proxy="<<all-points<<" save_framework_signatures="<<unsupportedSaveCallerCount<<"\n";
        const auto positiveAfter=Access::snapshot(calibration->fixture->p());checkSuccess(*calibration,kind,*positiveBefore,*positiveAfter);
        calibration->fixture->p().releaseResources();calibration.reset();
        if(points==0 || unsupportedSaveCallerCount!=0){stop=true;break;}
        for(std::uint64_t point=1;point<=points;++point){
            CHECK(*before==*Access::snapshot(rejected->fixture->p()));
            CHECK(pendingDenialWarnings==0);sourceCas=0;
            Access::hook(rejected->fixture->p(),&sourceCas,observe);
            controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=0;failAllocation=point;denyControl=false;
            faultContext=FaultContext::control;const auto reportsBefore=runtimeReports.load(),warningsBefore=expectedDenialWarnings.load();
            const auto hookBefore=_CrtSetAllocHook(allocationHook);bool committed=false,escapedBadAlloc=false,escapedOther=false;
            std::cout<<"CALLER_FAULT_DENIAL kind="<<kind<<" point="<<point<<'\n';trackControl=true;
            try {committed=call(*rejected,kind,before->edit);}catch(const std::bad_alloc&){escapedBadAlloc=true;}catch(...){escapedOther=true;}
            trackControl=false;failAllocation=0;_CrtSetAllocHook(hookBefore);Access::hook(rejected->fixture->p(),nullptr,nullptr);
            const auto after=Access::snapshot(rejected->fixture->p());const bool unchanged=*before==*after;
            const bool sameContext=rejected->fixture->p().aiMusicalContext()==contextBefore && rejected->fixture->p().groupLinkView()==groupBefore;
            fileBytes->reset();const bool sameFile=file.loadFileAsData(*fileBytes) && *fileBytes==presetBytes;
            const bool valid=!committed && !escapedBadAlloc && !escapedOther && sourceCas==0 && deniedAllocations==1
                && pendingDenialWarnings==0 && runtimeReports==reportsBefore && expectedDenialWarnings==warningsBefore+1 && unchanged && sameContext && sameFile;
            CHECK(!committed && !escapedBadAlloc && !escapedOther && sourceCas==0);
            CHECK(deniedAllocations==1 && pendingDenialWarnings==0 && runtimeReports==reportsBefore && expectedDenialWarnings==warningsBefore+1);
            CHECK(unchanged);
            CHECK(sameContext && sameFile);
            if(!valid){stop=true;break;} // Never reuse a fixture after any changed-state/unknown-failure assertion.
        }
        if(!stop){
            CHECK(*before==*Access::snapshot(rejected->fixture->p()));
            unsigned postCas=0;controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;trackControl=denyControl=false;
            Access::hook(rejected->fixture->p(),&postCas,[](void* raw,Access::Point point){
                if(point==Access::Point::afterSourceCas){++*static_cast<unsigned*>(raw);trackControl=denyControl=true;}
                if(point==Access::Point::afterCoreInstall)trackControl=denyControl=false;});
            const auto postHook=_CrtSetAllocHook(allocationHook);
            bool committed=false,escaped=false;try {committed=call(*rejected,kind,before->edit);}catch(...){escaped=true;}
            trackControl=denyControl=false;_CrtSetAllocHook(postHook);Access::hook(rejected->fixture->p(),nullptr,nullptr);
            CHECK(postCas==1&&controlAllocations==0&&deniedAllocations==0&&pendingDenialWarnings==0);
            std::cout<<"CALLER_POST_CAS_DENY_ALL kind="<<kind<<" allocations="<<controlAllocations<<" denied="<<deniedAllocations<<'\n';
            CHECK(committed && !escaped);
            if(committed && !escaped){const auto after=Access::snapshot(rejected->fixture->p());checkSuccess(*rejected,kind,*before,*after);
                std::cout<<"CALLER_FAULT_RETRY kind="<<kind<<" supported="<<points<<" count_before="<<before->history.count
                    <<" cursor_before="<<before->history.cursor<<" count_after="<<after->history.count<<" cursor_after="<<after->history.cursor<<'\n';}
            else stop=true;
        }
        rejected->fixture->p().releaseResources();
    }
    trackControl=false;calibratingAllocations=false;failAllocation=0;denyControl=false;faultContext=FaultContext::control;
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);
    CHECK(runtimeReports==initialReports && pendingDenialWarnings==0);
    if(runtimeReports!=initialReports)std::wcerr<<lastRuntimeReport.data()<<'\n';
    CHECK(file.deleteFile());
}
#else
void operationDenials(){CHECK(false);}
#endif

void localPreviewAdmission(){
    using Status=Access::PreviewStatus;
    begin("local_preview_empty_images_stay_busy_until_each_real_root_is_destroyed");
    auto state=std::make_unique<DrumOwnership>();const auto imageBytes=Access::previewImageBytes();
    CHECK(!Access::previewBusy(*state));DrumDocumentPtr empty;DrumError de;
    if(!CHECK(makeDrumDocument({},empty,de)))return;
    for(std::size_t role=0;role<3;++role){CHECK(Access::previewStatus(*state,role,empty,{})==Status::ready);
        CHECK(Access::previewBusy(*state));CHECK(Access::previewRetained(*state)==(role+1)*imageBytes);}
    const auto* originalImage=Access::previewImage(*state,0);const auto* originalDocument=Access::previewDocument(*state,0);
    CHECK(Access::previewStatus(*state,3,empty,{})==Status::invalid);
    CHECK(Access::previewStatus(*state,0,{},{})==Status::invalid);
    CHECK(Access::previewImage(*state,0)==originalImage&&Access::previewDocument(*state,0)==originalDocument);
    CHECK(Access::previewRetained(*state)==3*imageBytes);
    Access::previewClear(*state,3);CHECK(Access::previewRetained(*state)==3*imageBytes);
    Access::previewClear(*state,0);CHECK(Access::previewBusy(*state));Access::previewClear(*state,1);CHECK(Access::previewBusy(*state));
    Access::previewClear(*state,2);CHECK(!Access::previewBusy(*state));CHECK(Access::previewRetained(*state)==0);

    begin("local_preview_requires_complete_metadata_and_registered_physical_identity");
    AssetPtr first,twin,other;AssetError ae;const std::array<float,4> pcm{.125f,0,0,0},different{.25f,0,0,0};
    if(!CHECK(makeSampleAsset(pcm,1,first,ae)&&makeSampleAsset(pcm,1,twin,ae)&&makeSampleAsset(different,1,other,ae)))return;
    CHECK(first->id()==twin->id()&&first.get()!=twin.get());
    if(!CHECK(Access::previewRegister(*state,first)))return;Access::previewPreparation(*state,first);
    DrumSlots slots{};slots[3][15].asset=first->id();DrumDocumentPtr assigned;
    if(!CHECK(makeDrumDocument(slots,assigned,de)))return;
    Access::PreviewOwners owners{};owners[63]=first;
    if(!CHECK(Access::previewStatus(*state,0,assigned,owners)==Status::ready))return;
    Access::previewPreparation(*state,{});Access::previewCollect(*state);
    originalImage=Access::previewImage(*state,0);originalDocument=Access::previewDocument(*state,0);
    const auto originalRetained=Access::previewRetained(*state),originalReserved=Access::previewReserved(*state);
    CHECK(Access::previewStatus(*state,0,assigned,{})==Status::invalid);
    CHECK(Access::previewStatus(*state,0,empty,owners)==Status::invalid);
    auto wrong=owners;wrong[63]=other;CHECK(Access::previewStatus(*state,0,assigned,wrong)==Status::invalid);
    auto unregistered=owners;unregistered[63]=twin;CHECK(Access::previewStatus(*state,0,assigned,unregistered)==Status::invalid);
    CHECK(Access::previewImage(*state,0)==originalImage&&Access::previewDocument(*state,0)==originalDocument);
    CHECK(Access::previewOwner(*state,0,63).get()==first.get());
    CHECK(Access::previewRetained(*state)==originalRetained&&Access::previewReserved(*state)==originalReserved);

    begin("local_preview_equal_ids_do_not_rebate_distinct_live_allocations");
    const auto* twinPhysical=twin.get();const std::weak_ptr<const SampleAsset> twinLifetime=twin;
    if(!CHECK(Access::previewFixtureAppendPhysical(*state,std::move(twin))))return;
    if(!CHECK(Access::previewStatus(*state,1,assigned,unregistered)==Status::ready))return;
    unregistered={};wrong={};
    CHECK(Access::previewAlive(*state,first.get())&&Access::previewAlive(*state,twinPhysical));
    CHECK(Access::previewOwner(*state,0,63).get()!=Access::previewOwner(*state,1,63).get());
    CHECK(Access::previewRetained(*state)==2*sizeof(pcm)+2*imageBytes);
    auto canonical=Access::previewOwner(*state,1,63);CHECK(Access::previewRegister(*state,canonical));CHECK(canonical.get()==first.get());
    CHECK(Access::previewRetained(*state)==2*sizeof(pcm)+2*imageBytes&&!twinLifetime.expired());
    auto shared=owners;shared[63]=canonical;CHECK(Access::previewStatus(*state,1,assigned,shared)==Status::ready);
    CHECK(twinLifetime.expired()&&!Access::previewAlive(*state,twinPhysical));
    CHECK(Access::previewRetained(*state)==sizeof(pcm)+2*imageBytes);
    Access::previewClear(*state,0);CHECK(Access::previewRetained(*state)==sizeof(pcm)+imageBytes);
    Access::previewClear(*state,1);CHECK(Access::previewRetained(*state)==0&&!Access::previewBusy(*state));

    begin("local_preview_three_closed64_roots_share_actual_pcm_until_last_release");
    state=std::make_unique<DrumOwnership>();owners={};slots={};
    for(std::size_t n=0;n<64;++n){AssetPtr owner;const std::array<float,4> marker{float(n+1)/128.f,0,0,0};
        if(!CHECK(makeSampleAsset(marker,1,owner,ae)&&Access::previewRegister(*state,owner)))return;
        owners[n]=std::move(owner);slots[n/16][n%16].asset=owners[n]->id();slots[n/16][n%16].name="closed pad "+std::to_string(n+1);}
    if(!CHECK(makeDrumDocument(slots,assigned,de)))return;
    for(std::size_t role=0;role<3;++role){if(!CHECK(Access::previewStatus(*state,role,assigned,owners)==Status::ready))return;
        for(std::size_t n=0;n<64;++n)CHECK(Access::previewOwner(*state,role,n).get()==owners[n].get());
        CHECK(Access::previewRetained(*state)==64*sizeof(pcm)+(role+1)*imageBytes);}
    Access::previewClear(*state,0);Access::previewClear(*state,1);Access::previewCollect(*state);
    for(std::size_t n=0;n<64;++n)CHECK(Access::previewAlive(*state,owners[n].get()));
    CHECK(Access::previewRetained(*state)==64*sizeof(pcm)+imageBytes);Access::previewClear(*state,2);
    CHECK(Access::previewRetained(*state)==0&&!Access::previewBusy(*state));
    std::cout<<"LOCAL_PREVIEW_C_LAYOUT image_bytes="<<imageBytes<<" ledger_entries="<<Access::previewLedgerCapacity()<<" owner_bytes="<<sizeof(DrumOwnership)<<'\n';
    CHECK(Access::previewLedgerCapacity()==12675+3*64);

    begin("local_preview_one_byte_refusal_and_allocation_failure_preserve_existing_role");
    state=std::make_unique<DrumOwnership>();if(!CHECK(Access::previewStatus(*state,0,empty,{})==Status::ready))return;
    originalImage=Access::previewImage(*state,0);originalDocument=Access::previewDocument(*state,0);
    const auto oneShort=retainedDecodedLimit-2*imageBytes+1;
    if(!CHECK(Access::previewReserve(*state,oneShort)))return;
    const auto beforeRetained=Access::previewRetained(*state),beforeReserved=Access::previewReserved(*state);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto reportsBefore=runtimeReports.load(),warningsBefore=expectedDenialWarnings.load();CHECK(pendingDenialWarnings==0);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);std::array<void*,40> warm{};(void)RtlCaptureStackBackTrace(0,40,warm.data(),nullptr);
    controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;
    const auto previous=_CrtSetAllocHook(allocationHook);trackControl=true;
#endif
    auto status=Access::previewStatus(*state,0,empty,{});
#if defined(_MSC_VER) && defined(_DEBUG)
    trackControl=false;_CrtSetAllocHook(previous);CHECK(controlAllocations==0&&deniedAllocations==0);
#endif
    CHECK(status==Status::budget);CHECK(Access::previewImage(*state,0)==originalImage&&Access::previewDocument(*state,0)==originalDocument);
    CHECK(Access::previewRetained(*state)==beforeRetained&&Access::previewReserved(*state)==beforeReserved);
    Access::previewRelease(*state,oneShort);const auto exact=retainedDecodedLimit-2*imageBytes;
    if(!CHECK(Access::previewReserve(*state,exact))){
#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);
#endif
        return;
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=failAllocation=0;calibratingAllocations=true;
    const auto exactHook=_CrtSetAllocHook(allocationHook);trackControl=true;
#endif
    status=Access::previewStatus(*state,0,empty,{});
#if defined(_MSC_VER) && defined(_DEBUG)
    trackControl=false;calibratingAllocations=false;_CrtSetAllocHook(exactHook);
    CHECK(controlAllocations==1&&supportedAllocations==1&&deniedAllocations==0);
    CHECK(allocationEvents[0].type==_HOOK_ALLOC&&allocationEvents[0].bytes==imageBytes);
    std::cout<<"LOCAL_PREVIEW_IMAGE_ALLOCATION requested="<<allocationEvents[0].bytes<<" requests="<<controlAllocations<<" one_byte_refusal_requests=0\n";
#endif
    CHECK(status==Status::ready);CHECK(Access::previewRetained(*state)==retainedDecodedLimit-imageBytes);
    Access::previewRelease(*state,exact);CHECK(Access::previewRetained(*state)==imageBytes);
#if defined(_MSC_VER) && defined(_DEBUG)
    originalImage=Access::previewImage(*state,0);originalDocument=Access::previewDocument(*state,0);
    controlAllocations=supportedAllocations=proxyAllocations=deniedAllocations=0;failAllocation=1;
    const auto denialHook=_CrtSetAllocHook(allocationHook);trackControl=true;
    status=Access::previewStatus(*state,0,empty,{});
    trackControl=false;failAllocation=0;_CrtSetAllocHook(denialHook);
    CHECK(status==Status::allocation&&controlAllocations==1&&supportedAllocations==1&&deniedAllocations==1);
    CHECK(pendingDenialWarnings==0&&runtimeReports==reportsBefore&&expectedDenialWarnings==warningsBefore+1);
    CHECK(Access::previewImage(*state,0)==originalImage&&Access::previewDocument(*state,0)==originalDocument);
    CHECK(Access::previewRetained(*state)==imageBytes&&Access::previewReserved(*state)==imageBytes);
    CHECK(Access::previewStatus(*state,0,empty,{})==Status::ready);
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);
#endif
    Access::previewClear(*state,0);CHECK(Access::previewRetained(*state)==0&&!Access::previewBusy(*state));
}

}
#endif
int main(int argc,char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;std::cout.setf(std::ios::unitbuf);
#ifndef NDEBUG
    if(argc!=2)return 2;
    const juce::String group(argv[1]);
    try {
        if(group=="--ownership-local-preview-root")localPreviewRootRetention();
        else if(group=="--ownership-local-preview"){localPreviewRootRetention();localPreviewAdmission();}
        else if(group=="--ownership-core"){CHECK(bundleHostRecallRed()==0);sameNumericAndTargets();padScopedHistory();typedReuseAndIdentity();clearedHistoryIdentity();duplicateRoundTrip();publicationSaturation();preparationSlots();padAdmission();}
        else if(group=="--ownership-history"){literalHistoryBoundaries();realHistoryBoundary();mixedRealHistory();exactThresholdHistory();realRedoReject();fullLegacyHistory();}
        else if(group=="--ownership-recall"){selfContainedRecall();presetRoundTrip();identicalPresetHistory();sameNumericChangedPresetHistory();transactionalRecallFaults();numericEmptyHostRecall();numericEmptyHostRecallDenials();}
        else if(group=="--ownership-empty-numeric"){numericEmptyHostRecall();numericEmptyHostRecallDenials();}
        else if(group=="--ownership-races"){queuedEqualAck();failedReadBorrow();capturedHitRoots();claimedWorkerFence();targetWaba();boundedReadDiagnostic();independentControlPump();pumpTeardownBarrier();completeSpaceIdentity();sourceCasTransactions();runtimeReportControls();allocationTransactions();failedPrepareAuthority();callbackHeap();}
        else if(group=="--ownership-pump-teardown"){independentControlPump();pumpTeardownBarrier();}
        else if(group=="--ownership-allocations")allocationTransactions();
        else if(group=="--ownership-runtime-heap")callbackHeap();
        else if(group=="--ownership-retention-memory")retentionMemory();
        else if(group=="--ownership-save-denials")hostSaveDenials();
        else if(group=="--ownership-preset-save-calibration")presetSaveCalibration();
        else if(group=="--ownership-preset-naming-denials")presetRenameDenials();
        else if(group=="--ownership-preset-save-denials")presetSaveDenials();
        else if(group=="--ownership-crt-report-controls")runtimeReportControls();
        else if(group=="--ownership-realloc-control"){_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,runtimeReport);applicationReallocControl();_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,runtimeReport);CHECK(runtimeReports==0&&pendingDenialWarnings==0);}
        else if(group=="--ownership-read-diagnostic")boundedReadDiagnostic();
        else if(group=="--ownership-caller-denials")operationDenials();
        else if(group=="--ownership-preset-load-denials")operationDenials(3);
        else if(group=="--mutant-queue")queuedEqualAck();
        else if(group=="--mutant-pending")completeSpaceIdentity();
        else if(group=="--mutant-pad-merge")padScopedHistory();
        else if(group=="--mutant-failed-replay")publicationSaturation();
        else if(group=="--mutant-redo")realRedoReject();
        else if(group=="--mutant-hardcap")literalHistoryBoundaries();
        else if(group=="--mutant-threshold")exactThresholdHistory();
        else if(group=="--mutant-typed")typedReuseAndIdentity();
        else if(group=="--mutant-i1")clearedHistoryIdentity();
        else if(group=="--mutant-reservation")retentionMemory(true);
        else return 2;
    }catch(const std::exception& error){++failures;std::cerr<<"unexpected exception "<<error.what()<<'\n';}
    std::cout<<"OWNERSHIP_RESULT checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;
#else
    return 2;
#endif
}
