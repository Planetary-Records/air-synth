#include "FrozenRackExamples.h"
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include "RackLibrary.h"
#include "RackExamples.h"
#include "UserReach.h"
#include "OwnedFixture.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
using namespace airack;
namespace {
int checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cerr<<__LINE__<<": "<<#x<<'\n';} } while(false)
void drumModuleContract() {
    const auto drum=static_cast<NativeModule>(20);
    CHECK(unsigned(NativeModule::eraReverb)==19);
    CHECK(unsigned(EffectKind::eraReverb)==17);
    CHECK(moduleDisplayName(drum)=="Drum");
    CHECK(supportsAlgorithm(drum,1));CHECK(!supportsAlgorithm(drum,2));
    CHECK(!supportsAlgorithm(NativeModule::wavetable,3));
    CHECK(rackUnitParameterCapacity(drum)==293);
    CHECK(rackUnitParameterCapacity(NativeModule::wavetable)==293);
    if(!supportsAlgorithm(drum,1))return;
    for(std::uint8_t bank=0;bank<4;++bank) {
        CHECK(unitReach(drum,bank,1).size()==147);
        CHECK(unitReach(NativeModule::analog,bank,1).size()==116);
        CHECK(unitReach(NativeModule::wavetable,bank,1).size()==116);
        CHECK(unitReach(NativeModule::wavetable,bank,2).size()==125);
    }
}
void drumRetainedSource() {
    std::cout<<"drum_retained_149_293_294\n";
    const auto drum=static_cast<NativeModule>(20);
    if(!supportsAlgorithm(drum,1))return;
    for(const AlgorithmRevision remembered:{AlgorithmRevision(1),AlgorithmRevision(2)}) {
        auto before=std::make_unique<SoundDocument>();
        before->values[13]=1;before->compatibility.synth[0][1]=remembered;
        const auto registry=parameterRegistry();
        for(std::size_t f=0;f<9;++f)before->values[wavetableWarmthParameterStart+f]=f==6?1.f/3:f==0||f==2?2.f/6:.4f;
        before->values[*rackParameterIndex(NativeModule::wavetable,0,"wavetable.filter.cutoff")]=.37f;
        auto state=std::make_shared<RackPersonalityState>();RackInstance instance;instance.id=juce::Uuid();instance.bindings={{0,0}};
        auto& design=instance.design;design.name="All retained melodic defaults";design.units={{0,NativeModule::wavetable,{},remembered}};design.panel.sections={"Tone"};
        design.panel.controls={{"Cutoff",0,0,"wavetable.filter.cutoff",FocusWidget::rotary,FocusResponse::logarithmic,20,20000},
                               {"Pad",0,0,"drum.pad.1.gain_db",FocusWidget::rotary,FocusResponse::linear,-60,12}};
        for(std::size_t i=0;i<drumParameterStart;++i)if(registry[i].id.startsWith("synth.1."))design.units[0].defaults.push_back({0,registry[i].id.substring(8),before->values[i]});
        CHECK(design.units[0].defaults.size()==149);const auto oldDefaults=design.units[0].defaults;
        state->instances.push_back(instance);before->personalities=state;
        CHECK(validateSoundDocument(*before));juce::MemoryBlock raw;CHECK(encodeSoundState(*before,raw));auto decoded=std::make_unique<SoundDocument>();CHECK(decodeSoundState(raw.getData(),raw.getSize(),*decoded)&&*decoded==*before);
        before->values[*rackParameterIndex(drum,0,"drum.pad.1.gain_db")]=.21f;
        before->values[*rackParameterIndex(drum,0,"drum.pad.16.choke_group")]=1;
        const auto compatibility=before->compatibility;
        for(const auto [mode,engine]:{std::pair{1.f,1.f},{0.f,1.f},{1.f,1.f},{0.f,0.f},{1.f,0.f},{0.f,1.f}}) {
            auto proposed=airack::testing::fixtureValue([&]{return *before;});proposed->values[drumModeParameterStart]=mode;proposed->values[13]=engine;
            auto out=std::make_unique<SoundDocument>();RackDifference diff;
            const auto result=reconcileRackEdit(*before,*proposed,{},SoundEditOrigin::manual,*out,diff);CHECK(result==RackCandidateStatus::ready);if(result!=RackCandidateStatus::ready)break;
            CHECK(out->values==proposed->values&&out->compatibility==compatibility);
            CHECK(out->personalities&&out->personalities->instances.size()==1);if(!out->personalities)break;
            const auto& current=out->personalities->instances[0];const auto& unit=current.design.units[0];
            CHECK(unit.module==(mode==1?drum:engine==1?NativeModule::wavetable:NativeModule::analog));
            CHECK(unit.algorithmRevision==(mode==1?1:engine==1?remembered:compatibility.synth[0][0]));
            CHECK(unit.defaults.size()==293);
            for(const auto& old:oldDefaults){const auto at=std::find_if(unit.defaults.begin(),unit.defaults.end(),[&](const auto& p){return p.key==old.key;});CHECK(at!=unit.defaults.end());if(at!=unit.defaults.end())CHECK(at->normalized==(old.key=="engine"?engine:old.normalized));}
            CHECK(std::none_of(unit.defaults.begin(),unit.defaults.end(),[](const auto& p){return p.key=="drum_mode";}));
            CHECK(rackControlActive(*out,current,0)==(mode==0&&engine==1));CHECK(rackControlActive(*out,current,1)==(mode==1));
            CHECK(encodeSoundState(*out,raw));CHECK(decodeSoundState(raw.getData(),raw.getSize(),*decoded)&&*decoded==*out);
            auto invalid=unit;invalid.defaults.push_back({unit.id,"drum_mode",mode});auto bad=current.design;bad.units[0]=invalid;
            CHECK(!validateRackDesign(bad,RackValidationMode::instanceRecall));juce::MemoryBlock sentinel("keep",4),saved=sentinel;CHECK(!encodeRackDesign(bad,sentinel,RackValidationMode::instanceRecall)&&sentinel==saved);
            bad=current.design;bad.units[0].defaults.push_back(bad.units[0].defaults.front());CHECK(!validateRackDesign(bad,RackValidationMode::instanceRecall));
            before=std::move(out);
        }
        CHECK(unitReach(NativeModule::wavetable,0,1).size()==116&&unitReach(NativeModule::wavetable,0,2).size()==125);
    }
}
void drumPlacementBanks() {
    std::cout<<"drum_placement_all_banks_copy_reset_and_adoption\n";
    const auto drum=static_cast<NativeModule>(20);if(!supportsAlgorithm(drum,1))return;
    RackDesign recipe;recipe.name="Drum controls";recipe.units={{0,drum,{{0,"level",.42f},{0,"drum.pad.1.gain_db",.8f}},1}};recipe.panel.sections={"Drum"};
    for(std::uint8_t bank=0;bank<4;++bank){
        auto before=std::make_unique<SoundDocument>(),out=std::make_unique<SoundDocument>();RackDifference diff;
        before->values[13+4*bank]=1;before->values[wavetableWarmthParameterStart+bank*9+1]=.71f;
        RackOperation op;op.newInstance=juce::Uuid();op.design=recipe;op.allocation={{0,bank}};op.allowSlotReuse=true;op.destination={RackRole::synth,masterChain,std::uint8_t(bank==0?0:1)};
        CHECK(makeRackCandidate(*before,op,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);
        CHECK(out->layout.synthCount==(bank==0?1:2));CHECK(out->values[drumModeParameterStart+bank]==1&&out->values[13+4*bank]==0);
        CHECK(out->layout.effects==before->layout.effects&&out->compatibility==before->compatibility);
        const auto prefix="synth."+juce::String(bank+1)+".";std::size_t reset=0;
        const auto defaults=defaultParameterValues();const auto registry=parameterRegistry();
        for(std::size_t i=0;i<registry.size();++i){if(!registry[i].id.startsWith(prefix)){CHECK(out->values[i]==before->values[i]);continue;}++reset;const auto key=registry[i].id.substring(prefix.length());const auto at=std::find_if(recipe.units[0].defaults.begin(),recipe.units[0].defaults.end(),[&](const auto& p){return p.key==key;});CHECK(out->values[i]==(key=="drum_mode"?1.f:at==recipe.units[0].defaults.end()?defaults[i]:at->normalized));}CHECK(reset==294);
        auto bare=airack::testing::fixtureValue([&]{return *out;});bare->personalities.reset();RackOperation adopt=op;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();CHECK(makeRackCandidate(*bare,adopt,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);
        const auto refuse=[&](RackOperation wrong){auto preserved=airack::testing::fixtureValue([&]{return *out;});RackDifference marked{"sonic","presentation","structural","automation"};CHECK(makeRackCandidate(*bare,wrong,SoundEditOrigin::manual,*out,marked)==RackCandidateStatus::invalid);CHECK(*out==*preserved&&marked.sonic=="sonic"&&marked.structural=="structural");};
        auto wrong=adopt;wrong.design->units[0].module=NativeModule::analog;wrong.design->units[0].defaults.clear();refuse(wrong);
        wrong=adopt;wrong.design->units[0].algorithmRevision=2;refuse(wrong);wrong=adopt;wrong.allocation={{0,4}};refuse(wrong);wrong=adopt;wrong.allocation={{0,std::uint8_t((bank+1)%4)}};refuse(wrong);
    }

    {
        auto current=std::make_unique<SoundDocument>(),swapped=std::make_unique<SoundDocument>();current->values[13]=1;current->compatibility.synth[0]={1,2};
        for(std::size_t f=0;f<9;++f)current->values[wavetableWarmthParameterStart+f]=f==6?1.f/3:f==0||f==2?2.f/6:.4f;
        current->values[*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff")]=.19f;current->values[drumParameterStart]=.23f;
        RackDesign wave=recipe;wave.name="Wave";wave.units={{0,NativeModule::wavetable,{},2}};RackOperation adoption;adoption.kind=RackOperationKind::adopt;adoption.newInstance=juce::Uuid();adoption.design=wave;adoption.allocation={{0,0}};RackDifference diff;
        CHECK(makeRackCandidate(*current,adoption,SoundEditOrigin::manual,*swapped,diff)==RackCandidateStatus::ready);current=std::move(swapped);swapped=std::make_unique<SoundDocument>();
        for(const auto [module,revision]:{std::pair{drum,AlgorithmRevision(1)},std::pair{NativeModule::wavetable,AlgorithmRevision(1)},std::pair{drum,AlgorithmRevision(1)},std::pair{NativeModule::wavetable,AlgorithmRevision(2)}}){
            RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=adoption.newInstance;swap.design=recipe;swap.design->units={{0,module,{},revision}};swap.allowSlotReuse=true;
            const auto oldValues=current->values;const auto compatibility=current->compatibility;CHECK(makeRackCandidate(*current,swap,SoundEditOrigin::manual,*swapped,diff)==RackCandidateStatus::ready);
            CHECK(swapped->values[13]==1&&swapped->values[drumModeParameterStart]==(module==drum?1:0));CHECK(swapped->values[*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff")]==.19f);
            for(std::size_t f=0;f<9;++f)CHECK(swapped->values[wavetableWarmthParameterStart+f]==(module==NativeModule::wavetable&&revision==2?defaultParameterValues()[wavetableWarmthParameterStart+f]:oldValues[wavetableWarmthParameterStart+f]));
            auto expected=compatibility;if(module==NativeModule::wavetable)expected.synth[0][1]=revision;CHECK(swapped->compatibility==expected);if(module==NativeModule::wavetable)CHECK(swapped->values[drumParameterStart]==oldValues[drumParameterStart]);
            current=std::move(swapped);swapped=std::make_unique<SoundDocument>();
        }
    }
    auto before=std::make_unique<SoundDocument>(),out=std::make_unique<SoundDocument>();RackDifference diff;
    before->values[13]=1;before->values[wavetableWarmthParameterStart+1]=.71f;before->values[drumParameterStart]=.23f;before->compatibility.synth[0]={1,2};
    RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=recipe;adopt.allocation={{0,0}};before->values[drumModeParameterStart]=1;CHECK(makeRackCandidate(*before,adopt,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);before=std::move(out);out=std::make_unique<SoundDocument>();

    {
        auto locked=airack::testing::fixtureValue([&]{return *before;});locked->synthLocks[0]=true;locked->effectLocks[7]=true;auto next=airack::testing::fixtureValue([&]{return *locked;});next->values[drumModeParameterStart]=0;
        CHECK(reconcileRackEdit(*locked,*next,{},SoundEditOrigin::aiCandidate,*out,diff)==RackCandidateStatus::lockedDependency);CHECK(reconcileRackEdit(*locked,*next,{},SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);
        locked->synthLocks[0]=false;locked->controlLocks[drumParameterStart]=true;next->controlLocks=locked->controlLocks;CHECK(reconcileRackEdit(*locked,*next,{},SoundEditOrigin::aiCandidate,*out,diff)==RackCandidateStatus::lockedDependency);
        auto mapped=airack::testing::fixtureValue([&]{return *before;});mapped->macros[0]={parameterRegistry()[drumParameterStart].stateKey,.3f};next=airack::testing::fixtureValue([&]{return *mapped;});next->values[drumModeParameterStart]=0;CHECK(reconcileRackEdit(*mapped,*next,{},SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);CHECK(diff.automationWarning.contains("Global macro 1 target affected"));
        RackOperation fx;fx.kind=RackOperationKind::place;fx.newInstance=juce::Uuid();fx.design=recipe;fx.design->role=RackRole::fx;fx.design->units={{0,NativeModule::drive,{}}};fx.destination={RackRole::fx,0,0};CHECK(makeRackCandidate(*before,fx,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);auto dependent=airack::testing::fixtureValue([&]{return *out;});RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=adopt.newInstance;CHECK(makeRackCandidate(*dependent,remove,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::reconciliationRequired);
        RackOperation move;move.kind=RackOperationKind::move;move.selected=fx.newInstance;move.destination={RackRole::fx,masterChain,0};CHECK(makeRackCandidate(*dependent,move,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);dependent=airack::testing::fixtureValue([&]{return *out;});CHECK(makeRackCandidate(*dependent,remove,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);
    }
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=adopt.newInstance;duplicate.newInstance=juce::Uuid();duplicate.allocation={{0,3}};duplicate.destination={RackRole::synth,masterChain,1};
    CHECK(makeRackCandidate(*before,duplicate,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);std::size_t copied=0;
    for(const auto& p:parameterRegistry())if(p.id.startsWith("synth.1.")){const auto key=p.id.substring(8);CHECK(out->values[*rackParameterIndex(drum,3,key)]==before->values[*rackParameterIndex(drum,0,key)]);++copied;}CHECK(copied==294);CHECK(out->compatibility.synth[3]==before->compatibility.synth[0]);
    before=std::move(out);out=std::make_unique<SoundDocument>();RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=duplicate.newInstance;CHECK(makeRackCandidate(*before,remove,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);CHECK(out->values==before->values);
    before=std::move(out);out=std::make_unique<SoundDocument>();auto reuse=adopt;reuse.kind=RackOperationKind::place;reuse.newInstance=juce::Uuid();reuse.allocation={{0,3}};reuse.destination={RackRole::synth,masterChain,1};CHECK(makeRackCandidate(*before,reuse,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::replacementRequired);reuse.allowSlotReuse=true;CHECK(makeRackCandidate(*before,reuse,SoundEditOrigin::manual,*out,diff)==RackCandidateStatus::ready);CHECK(out->values[wavetableWarmthParameterStart+3*9+1]==defaultParameterValues()[wavetableWarmthParameterStart+3*9+1]);
}
RackDesign design(NativeModule module=NativeModule::analog) {
    RackDesign d;d.name="Original";d.intent="Native";d.role=module<NativeModule::drive?RackRole::synth:RackRole::fx;
    d.units={{0,module,{}}};d.panel.sections={"Tone"};
    if(d.role==RackRole::synth){d.units[0].defaults={{0,"analog.filter.cutoff",.5f}};d.panel.controls={{"Tone",0,0,"analog.filter.cutoff",FocusWidget::rotary,FocusResponse::logarithmic,20,20000}};}
    return d;
}
SoundDocument owned() {
    SoundDocument d;RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.design=design();op.allocation={{0,0}};
    SoundDocument out;RackDifference diff;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);return out;
}
RackCandidateStatus reconcile(const SoundDocument& a,const SoundDocument& b,SoundDocument& out,const RackReconcileChoices& choices={}) {
    RackDifference diff;return reconcileRackEdit(a,b,choices,SoundEditOrigin::manual,out,diff);
}
void patch_is_not_design() {
    std::cout<<"patch_is_not_design\n";auto d=owned(),next=d,out=d;
    const auto index=*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff");next.values[index]=.8f;
    CHECK(reconcile(d,next,out)==RackCandidateStatus::ready);CHECK(out.values[index]==.8f);CHECK(*out.personalities==*d.personalities);
    auto separate=d;separate.personalities=std::make_shared<RackPersonalityState>(*d.personalities);CHECK(separate==d);
    auto generic=d;generic.personalities.reset();CHECK(generic!=d);
    next=d;next.values[13]=1;CHECK(reconcile(d,next,out)==RackCandidateStatus::ready);
    CHECK(out.personalities->instances[0].design.revision==2);CHECK(out.personalities->instances[0].edited);
    CHECK(out.personalities->instances[0].design.units[0].module==NativeModule::wavetable);
    const auto& retained=out.personalities->instances[0].design.units[0].defaults;
    CHECK(std::any_of(retained.begin(),retained.end(),[](auto& p){return p.key=="analog.filter.cutoff"&&p.normalized==.5f;}));
    CHECK(out.personalities->instances[0].design.panel.controls==d.personalities->instances[0].design.panel.controls);
    CHECK(!rackControlActive(out,out.personalities->instances[0],0));
    next=out;next.values[13]=0;auto switched=out;CHECK(reconcile(switched,next,out)==RackCandidateStatus::ready);CHECK(rackControlActive(out,out.personalities->instances[0],0));
    auto maximum=std::make_shared<RackPersonalityState>(*d.personalities);maximum->instances[0].design.revision=std::numeric_limits<std::uint64_t>::max();d.personalities=maximum;next=d;next.values[13]=1;
    out=generic;const auto sentinel=out;CHECK(reconcile(d,next,out)==RackCandidateStatus::invalid);CHECK(out==sentinel);
}
void all_paths_reconcile() {
    std::cout<<"all_paths_reconcile\n";auto d=owned(),out=d;
    auto bad=std::make_shared<RackPersonalityState>(*d.personalities);bad->instances.push_back(bad->instances[0]);bad->instances.back().id=juce::Uuid();auto next=d;next.personalities=bad;CHECK(!validateRackInstances(next));
    bad=std::make_shared<RackPersonalityState>(*d.personalities);bad->instances[0].design.panel.controls[0].key="deleted";next.personalities=bad;CHECK(!validateRackInstances(next));
    for(int i=0;i<13;++i){bad->instances.push_back(bad->instances[0]);bad->instances.back().id=juce::Uuid();}CHECK(!validateRackInstances(next));
    next=d;CHECK(editRackLayout(next.layout,{RackEditKind::removeSynth,0},next.layout)==RackEditResult::applied);
    CHECK(reconcile(d,next,out)==RackCandidateStatus::reconciliationRequired);
    RackReconcileChoices choices;choices.detach={d.personalities->instances[0].id};CHECK(reconcile(d,next,out,choices)==RackCandidateStatus::ready);CHECK(!out.personalities);
    RackOperation op;op.kind=RackOperationKind::place;op.design=design();op.newInstance=juce::Uuid();op.destination={RackRole::synth,masterChain,1};
    RackDifference diff;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.layout.synthCount==2);CHECK(out.personalities->instances.size()==2);
    op.allocation={{0,0}};const auto sentinel=out;diff.sonic="sentinel";CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::replacementRequired);CHECK(out==sentinel);CHECK(diff.sonic=="sentinel");
    op.allowSlotReuse=true;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::reconciliationRequired);
    auto locked=d;locked.synthLocks[0]=true;next=locked;next.values[13]=1;CHECK(reconcileRackEdit(locked,next,{},SoundEditOrigin::aiCandidate,out,diff)==RackCandidateStatus::lockedDependency);
    CHECK(reconcile(locked,next,out)==RackCandidateStatus::ready);
    auto full=d;for(std::uint8_t s=1;s<4;++s)CHECK(editRackLayout(full.layout,{RackEditKind::addSynth,s,masterChain,s},full.layout)==RackEditResult::applied);
    op.allocation.clear();op.allowSlotReuse=false;CHECK(makeRackCandidate(full,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::capacityReached);
    op.design=design(NativeModule::drive);op.destination={RackRole::fx,masterChain,0};for(std::uint8_t s=0;s<8;++s)CHECK(editRackLayout(full.layout,{RackEditKind::addEffect,s,masterChain,s},full.layout)==RackEditResult::applied);
    CHECK(makeRackCandidate(full,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::capacityReached);
}
void segment_move_and_split() {
    std::cout<<"segment_move_and_split\n";SoundDocument d,out;RackDifference diff;RackOperation op;op.kind=RackOperationKind::place;op.newInstance=juce::Uuid();op.design=design(NativeModule::drive);op.design->units.push_back({1,NativeModule::delay,{}});op.destination={RackRole::fx,masterChain,0};
    CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;
    op={};op.kind=RackOperationKind::move;op.selected=d.personalities->instances[0].id;op.destination={RackRole::fx,0,0};CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.personalities->instances[0].bindings==d.personalities->instances[0].bindings);
    auto split=out;CHECK(editRackLayout(split.layout,{RackEditKind::moveEffect,1,masterChain,0},split.layout)==RackEditResult::applied);d=out;CHECK(reconcile(d,split,out)==RackCandidateStatus::reconciliationRequired);
    RackReconcileChoices c;c.detach={d.personalities->instances[0].id};CHECK(reconcile(d,split,out,c)==RackCandidateStatus::ready);
    op={};op.kind=RackOperationKind::duplicate;op.selected=d.personalities->instances[0].id;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};
    const auto gain=*rackParameterIndex(NativeModule::drive,0,"drive.gain_db");d.values[gain]=.8f;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.values[*rackParameterIndex(NativeModule::drive,2,"drive.gain_db")]==.8f);
}
void dependent_fx_survives_synth_removal() {
    std::cout<<"dependent_fx_survives_synth_removal\n";auto d=owned(),out=d;RackDifference diff;RackOperation fx;fx.kind=RackOperationKind::place;fx.newInstance=juce::Uuid();fx.design=design(NativeModule::drive);fx.destination={RackRole::fx,0,0};CHECK(makeRackCandidate(d,fx,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=d.personalities->instances[0].id;CHECK(makeRackCandidate(d,remove,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::reconciliationRequired);CHECK(out==d);
    RackOperation move;move.kind=RackOperationKind::move;move.selected=d.personalities->instances[1].id;move.destination={RackRole::fx,masterChain,0};CHECK(makeRackCandidate(d,move,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;
    CHECK(makeRackCandidate(d,remove,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.personalities->instances.size()==1);CHECK(out.personalities->instances[0].id==fx.newInstance);CHECK(out.layout.effects[0].use==SlotUse::present);
}
void structural_edge_contracts() {
    std::cout<<"structural_edge_contracts\n";auto d=owned(),out=d;RackDifference diff;
    auto defaults=std::make_shared<RackPersonalityState>(*d.personalities);defaults->instances[0].design.units[0].defaults.push_back({0,"engine",0});d.personalities=defaults;
    auto next=d;next.values[13]=1;CHECK(reconcile(d,next,out)==RackCandidateStatus::ready);
    if(out.personalities){const auto& p=out.personalities->instances[0].design.units[0].defaults;CHECK(std::any_of(p.begin(),p.end(),[](auto& x){return x.key=="engine"&&x.normalized==1;}));CHECK(std::any_of(p.begin(),p.end(),[](auto& x){return x.key=="analog.filter.cutoff"&&x.normalized==.5f;}));}
    d=owned();RackOperation fx;fx.kind=RackOperationKind::place;fx.newInstance=juce::Uuid();fx.design=design(NativeModule::drive);fx.destination={RackRole::fx,0,0};CHECK(makeRackCandidate(d,fx,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;
    RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=d.personalities->instances[0].id;swap.design=design(NativeModule::wavetable);swap.design->units[0].defaults.clear();swap.design->panel.controls.clear();swap.allowSlotReuse=true;
    CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.layout.effects[0].use==SlotUse::present);CHECK(out.personalities->instances.size()==2);
    d={};fx.destination={RackRole::fx,masterChain,0};CHECK(makeRackCandidate(d,fx,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;
    swap.selected=fx.newInstance;swap.design=design(NativeModule::drive);swap.design->units.push_back({1,NativeModule::delay,{}});swap.destination={RackRole::fx,masterChain,0};CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.layout.effectCount[masterChain]==2);
    d=owned();swap={};swap.kind=RackOperationKind::swap;swap.selected=d.personalities->instances[0].id;swap.design=design();swap.design->units.push_back({1,NativeModule::drive,{}});swap.design->units.push_back({2,NativeModule::drive,{}});swap.allowSlotReuse=true;CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    if(!out.personalities||out.personalities->instances[0].design.units.size()!=3)return;
    d=out;next=d;CHECK(editRackLayout(next.layout,{RackEditKind::moveEffect,0,0,1},next.layout)==RackEditResult::applied);CHECK(reconcile(d,next,out)==RackCandidateStatus::ready);CHECK(out.personalities->instances[0].design.units[1].id==2);
}
void explicit_resolutions_and_bounds() {
    std::cout<<"explicit_resolutions_and_bounds\n";SoundDocument d,out;RackDifference diff;RackOperation op;op.kind=RackOperationKind::place;op.newInstance=juce::Uuid();op.design=design(NativeModule::drive);op.design->units.push_back({1,NativeModule::drive,{}});op.design->panel.controls={{"Gain",0,0,"drive.gain_db",FocusWidget::rotary,FocusResponse::linear,0,24}};op.destination={RackRole::fx,masterChain,0};
    CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;d.macros[0]={parameterRegistry()[*rackParameterIndex(NativeModule::drive,0,"drive.gain_db")].stateKey,.5f};auto next=d;CHECK(editRackLayout(next.layout,{RackEditKind::removeEffect,0},next.layout)==RackEditResult::applied);
    CHECK(reconcile(d,next,out)==RackCandidateStatus::reconciliationRequired);RackReconcileChoices choices;choices.controls={{op.newInstance,0,{}}};CHECK(reconcileRackEdit(d,next,choices,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.personalities->instances[0].design.panel.controls.empty());CHECK(diff.automationWarning.contains("macro 1"));
    choices.controls[0].replacement=d.personalities->instances[0].design.panel.controls[0];choices.controls[0].replacement->unit=1;CHECK(reconcile(d,next,out,choices)==RackCandidateStatus::ready);CHECK(rackControlActive(out,out.personalities->instances[0],0));
    const auto sentinel=out;diff.sonic="sentinel";choices.controls.push_back(choices.controls[0]);CHECK(reconcileRackEdit(d,next,choices,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::invalid);CHECK(out==sentinel&&diff.sonic=="sentinel");
    choices.controls.resize(97);CHECK(reconcile(d,next,out,choices)==RackCandidateStatus::invalid);choices={};choices.detach.resize(13,op.newInstance);CHECK(reconcile(d,next,out,choices)==RackCandidateStatus::invalid);
    next=d;CHECK(editRackLayout(next.layout,{RackEditKind::addEffect,2,masterChain,1},next.layout)==RackEditResult::applied);CHECK(!validateRackInstances(next));CHECK(reconcile(d,next,out)==RackCandidateStatus::reconciliationRequired);
    auto controls=std::make_shared<RackPersonalityState>(*d.personalities);auto& panel=controls->instances[0].design.panel;for(int n=1;n<4;++n){auto c=panel.controls[0];c.label=juce::String(n);c.unit=std::uint8_t(n%2);if(n>=2){c.key="drive.tone_hz";c.minimum=200;c.maximum=20000;}panel.controls.push_back(c);}d.personalities=controls;next=d;choices={};choices.controls={{op.newInstance,0,{}},{op.newInstance,2,{}}};CHECK(reconcile(d,next,out,choices)==RackCandidateStatus::ready);CHECK(out.personalities->instances[0].design.panel.controls.size()==2);CHECK(out.personalities->instances[0].design.panel.controls[0].label=="1"&&out.personalities->instances[0].design.panel.controls[1].label=="3");
}
}
void library_operation_bank_contracts() {
    std::cout<<"library_operation_bank_contracts\n";SoundDocument d,out;RackDifference diff;
    RackOperation place;place.kind=RackOperationKind::place;place.newInstance=juce::Uuid();place.design=design(NativeModule::drive);place.design->units.push_back({1,NativeModule::delay,{}});place.destination={RackRole::fx,masterChain,0};
    CHECK(makeRackCandidate(d,place,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);d=out;
    const auto retained=*rackParameterIndex(NativeModule::drive,0,"reverb.mix");d.values[retained]=.81f;
    RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=place.newInstance;swap.design=design(NativeModule::drive);swap.destination=place.destination;swap.allowSlotReuse=true;
    CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.personalities->instances[0].bindings[0].slot==0);CHECK(out.layout.effects[1].use==SlotUse::retired);
    CHECK(out.values[retained]==.81f); // Inactive banks do not reset on a same-bank swap.
    // Supplied active defaults and common keys still initialize normally;
    // changing the active type preserves the now-inactive Drive coordinates.
    const auto active=*rackParameterIndex(NativeModule::drive,0,"drive.gain_db");d.values[active]=.79f;
    swap.design->units[0].defaults={{0,"drive.gain_db",.31f},{0,"bypassed",1}};
    CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.values[active]==.31f);CHECK(out.values[*rackParameterIndex(NativeModule::drive,0,"bypassed")]==1);CHECK(out.values[retained]==.81f);
    swap.design=design(NativeModule::reverb);swap.design->units[0].defaults={{0,"reverb.mix",.46f}};
    CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.values[retained]==.46f);CHECK(out.values[active]==.79f);CHECK(out.layout.effects[0].kind==EffectKind::reverb);
    swap.design=design(NativeModule::drive);
    CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    d=out;swap.design->units.push_back({1,NativeModule::delay,{}});swap.design->units.push_back({2,NativeModule::reverb,{}});swap.allocation={{0,0},{1,2},{2,3}};
    CHECK(makeRackCandidate(d,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.layout.effects[1].use==SlotUse::retired);
    CHECK(out.personalities->instances[0].bindings==swap.allocation);CHECK(diff.automationWarning.contains("Reuse bank 1"));
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=place.newInstance;d=out;CHECK(makeRackCandidate(d,remove,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(!out.personalities);
    d=out;place.design=design(NativeModule::drive);place.allocation={{0,0}};place.newInstance=juce::Uuid();const auto before=d;diff.sonic="sentinel";
    CHECK(makeRackCandidate(d,place,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::replacementRequired);CHECK(out==before&&diff.sonic=="sentinel");
    place.allowSlotReuse=true;CHECK(makeRackCandidate(d,place,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(diff.automationWarning.contains("Reuse bank 1"));
    CHECK(out.values[retained]==.25f); // Explicit placement/reuse keeps its existing initialization policy.
}
void library_transactions() {
    std::cout<<"library_transactions\n";const auto dir=juce::File::getCurrentWorkingDirectory().getChildFile("task3-library-"+juce::Uuid().toString());CHECK(dir.createDirectory());
    const auto file=dir.getChildFile("native.airack");auto d=design(),loaded=design(NativeModule::drive);const auto sentinel=loaded;
    for(const auto revision:{9223372036854775807ULL,9223372036854775808ULL,18446744073709551615ULL}){d.revision=revision;CHECK(saveRackDesign(d,file));CHECK(loadRackDesign(file,loaded));CHECK(loaded==d);}
    juce::MemoryBlock prior;CHECK(file.loadFileAsData(prior));auto invalid=d;invalid.units[0].module=NativeModule::drive;CHECK(!saveRackDesign(invalid,file));juce::MemoryBlock current;CHECK(file.loadFileAsData(current));CHECK(current==prior);
    CHECK(file.setReadOnly(true));auto replacement=design();CHECK(!saveRackDesign(replacement,file));current.reset();CHECK(file.loadFileAsData(current));CHECK(current==prior);CHECK(file.setReadOnly(false));
    for(const auto bad:{dir.getChildFile("missing"),dir}){loaded=sentinel;CHECK(!loadRackDesign(bad,loaded));CHECK(loaded==sentinel);}
    CHECK(file.replaceWithText("corrupt"));loaded=sentinel;CHECK(!loadRackDesign(file,loaded));CHECK(loaded==sentinel);
    const std::string oversized(65537,' ');CHECK(file.replaceWithData(oversized.data(),oversized.size()));CHECK(!loadRackDesign(file,loaded));CHECK(loaded==sentinel);
    juce::MemoryBlock legal;CHECK(encodeRackDesign(d,legal));const std::string padding(65536-legal.getSize(),' ');legal.append(padding.data(),padding.size());
    CHECK(file.replaceWithData(legal.getData(),legal.getSize()));CHECK(loadRackDesign(file,loaded));CHECK(loaded==d);
    SoundDocument sound;sound.values[*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff")]=.73f;sound.values[0]=.9f;sound.values[1]=.8f;sound.synthLocks[0]=true;
    sound.macros[0]={parameterRegistry()[29].stateKey,.6f};const auto before=sound;const auto id=juce::Uuid();RackDesign captured;
    const std::array<UnitBinding,1> synthSelection{{{0,0}}};CHECK(captureGenericRackDesign(sound,RackRole::synth,synthSelection,d.panel,id,"Manual","Offline",captured));
    CHECK(sound==before);CHECK(captured.id==id&&captured.revision==1);CHECK(captured.units.size()==1&&captured.units[0].module==NativeModule::analog);
    CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](auto& p){return p.key=="analog.filter.cutoff"&&p.normalized==.73f;}));
    for(const auto& p:captured.units[0].defaults){CHECK(!p.key.startsWith("wavetable."));CHECK(!p.key.startsWith("synth."));CHECK(p.unit==0);}
    CHECK(saveRackDesign(captured,file));CHECK(loadRackDesign(file,loaded));CHECK(loaded==captured);
    RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=captured;adopt.allocation.assign(synthSelection.begin(),synthSelection.end());SoundDocument ownedSound;RackDifference difference;
    CHECK(makeRackCandidate(sound,adopt,SoundEditOrigin::manual,ownedSound,difference)==RackCandidateStatus::ready);CHECK(ownedSound.values==sound.values);CHECK(ownedSound.macros==sound.macros);CHECK(ownedSound.synthLocks==sound.synthLocks);
    const auto out=loaded;CHECK(!captureGenericRackDesign(ownedSound,RackRole::synth,synthSelection,d.panel,id,"Manual","Offline",loaded));CHECK(loaded==out);
    CHECK(!captureGenericRackDesign(sound,RackRole::synth,{},d.panel,id,"Manual","Offline",loaded));CHECK(loaded==out);
    const std::array<UnitBinding,2> duplicateLocal{{{0,0},{0,1}}};CHECK(!captureGenericRackDesign(sound,RackRole::synth,duplicateLocal,d.panel,id,"Manual","Offline",loaded));CHECK(loaded==out);
    const std::array<UnitBinding,1> absent{{{0,3}}};CHECK(!captureGenericRackDesign(sound,RackRole::synth,absent,d.panel,id,"Manual","Offline",loaded));CHECK(loaded==out);
    for(std::uint8_t n=0;n<3;++n)CHECK(editRackLayout(sound.layout,{RackEditKind::addEffect,n,masterChain,n},sound.layout)==RackEditResult::applied);
    const std::array<UnitBinding,2> split{{{0,0},{1,2}}};CHECK(!captureGenericRackDesign(sound,RackRole::fx,split,design(NativeModule::drive).panel,id,"FX","Offline",loaded));CHECK(loaded==out);
    const std::array<UnitBinding,2> contiguous{{{0,0},{1,1}}};CHECK(captureGenericRackDesign(sound,RackRole::fx,contiguous,design(NativeModule::drive).panel,id,"FX","Offline",loaded));CHECK(loaded.role==RackRole::fx&&loaded.units.size()==2);
    const auto fxSentinel=loaded;CHECK(!captureGenericRackDesign(sound,RackRole::fx,duplicateLocal,design(NativeModule::drive).panel,id,"FX","Offline",loaded));CHECK(loaded==fxSentinel);
    CHECK(dir.deleteRecursively());
}


void twelve_record_native_replacement() {
    auto source=owned();SoundDocument out;RackDifference diff;juce::Uuid old;
    for(std::uint8_t slot=1;slot<4;++slot){RackOperation op;op.newInstance=juce::Uuid();op.design=design();op.destination.position=slot;op.allocation={{0,slot}};CHECK(makeRackCandidate(source,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);source=out;}
    for(std::uint8_t slot=0;slot<8;++slot){RackOperation op;op.newInstance=juce::Uuid();op.design=design(NativeModule::drive);op.destination={RackRole::fx,masterChain,slot};op.allocation={{0,slot}};CHECK(makeRackCandidate(source,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);source=out;if(slot==0)old=op.newInstance;}
    CHECK(source.personalities->instances.size()==12);RackOperation replacement;replacement.newInstance=juce::Uuid();replacement.design=design(NativeModule::drive);replacement.destination={RackRole::fx,masterChain,0};replacement.allocation={{0,0}};replacement.allowSlotReuse=true;replacement.choices.remove={old};
    CHECK(makeRackCandidate(source,replacement,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);CHECK(out.personalities->instances.size()==12);CHECK(out.personalities->instances.back().id==replacement.newInstance);CHECK(validateSoundDocument(out));
    replacement.choices={};auto sentinel=out;CHECK(makeRackCandidate(source,replacement,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::capacityReached);CHECK(out==sentinel);
}
void explicit_removal_order_and_replacement() {
    std::cout<<"explicit_removal_order_and_replacement\n";
    auto source=owned(),out=source;RackDifference diff;
    RackOperation fx;fx.newInstance=juce::Uuid();fx.design=design(NativeModule::drive);fx.destination={RackRole::fx,0,0};
    CHECK(makeRackCandidate(source,fx,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);source=out;
    const auto synthId=source.personalities->instances[0].id;
    SoundDocument forward,reverse;RackDifference fd,rd;
    RackReconcileChoices c;c.remove={synthId,fx.newInstance};
    CHECK(reconcileRackEdit(source,source,c,SoundEditOrigin::manual,forward,fd)==RackCandidateStatus::ready);
    c.remove={fx.newInstance,synthId};
    CHECK(reconcileRackEdit(source,source,c,SoundEditOrigin::manual,reverse,rd)==RackCandidateStatus::ready);
    CHECK(forward==reverse);CHECK(fd.structural==rd.structural&&fd.sonic==rd.sonic&&fd.presentation==rd.presentation&&fd.automationWarning==rd.automationWarning);CHECK(!forward.personalities);CHECK(forward.values==source.values);
    c.remove={synthId};out=source;CHECK(reconcileRackEdit(source,source,c,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::reconciliationRequired);CHECK(out==source);
    auto locked=source;locked.effectLocks[0]=true;c.remove={synthId,fx.newInstance};
    CHECK(reconcileRackEdit(locked,locked,c,SoundEditOrigin::aiCandidate,out,diff)==RackCandidateStatus::lockedDependency);CHECK(out==source);
    source={};fx.destination={RackRole::fx,masterChain,0};CHECK(makeRackCandidate(source,fx,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);source=out;
    RackOperation replacement=fx;replacement.newInstance=juce::Uuid();replacement.allocation={{0,0}};replacement.allowSlotReuse=true;replacement.choices.remove={fx.newInstance};replacement.design->units[0].defaults={{0,"drive.gain_db",.37f}};
    const auto gain=*rackParameterIndex(NativeModule::drive,0,"drive.gain_db");
    CHECK(makeRackCandidate(source,replacement,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.personalities&&out.personalities->instances.size()==1&&out.personalities->instances[0].id==replacement.newInstance);
    CHECK(out.layout.effects[0].use==SlotUse::present);CHECK(out.values[gain]==.37f);CHECK(diff.automationWarning.contains("Reuse bank"));CHECK(out.macros==source.macros);
    auto bad=replacement;bad.allowSlotReuse=false;out=source;CHECK(makeRackCandidate(source,bad,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::replacementRequired);CHECK(out==source);
    bad=replacement;bad.choices={};CHECK(makeRackCandidate(source,bad,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::reconciliationRequired);CHECK(out==source);
    locked=source;locked.effectLocks[0]=true;CHECK(makeRackCandidate(locked,replacement,SoundEditOrigin::aiCandidate,out,diff)==RackCandidateStatus::lockedDependency);CHECK(out==source);
}
void algorithm_revision_transaction(){
    std::cout<<"algorithm_revision_transaction: snapshot, duplicate and unsupported swap\n";
    const auto original=owned();const auto selected=original.personalities->instances[0].id;
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=selected;duplicate.newInstance=juce::Uuid();duplicate.destination={RackRole::synth,masterChain,1};duplicate.allocation={{0,1}};
    auto out=original;RackDifference diff;CHECK(makeRackCandidate(original,duplicate,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.compatibility.synth[1]==original.compatibility.synth[0]);CHECK(out.personalities->instances.size()==2);
    CHECK(out.personalities->instances[0].bindings!=out.personalities->instances[1].bindings);
    for(const auto& instance:out.personalities->instances)for(const auto& unit:instance.design.units)CHECK(unit.algorithmRevision==1);
    auto unsupported=design();unsupported.units[0].algorithmRevision=3;RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=selected;swap.design=unsupported;swap.allowSlotReuse=true;
    const auto before=out;CHECK(makeRackCandidate(original,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::invalid);CHECK(out==before);
    auto invalid=original;invalid.compatibility.synth[3][1]=3;CHECK(reconcile(original,invalid,out)==RackCandidateStatus::invalid);CHECK(out==before);
    auto generic=original;generic.personalities.reset();const std::array<UnitBinding,1> chosen{{{0,0}}};RackDesign captured;
    CHECK(captureGenericRackDesign(generic,RackRole::synth,chosen,design().panel,juce::Uuid(),"Captured","Legacy numeric identity",captured));CHECK(captured.units[0].algorithmRevision==1);
}
void modern_reverb_swap_duplicate_and_capture(){
    std::cout<<"modern_reverb_swap_duplicate_and_capture\n";
    SoundDocument initial,legacy;RackDifference diff;RackOperation place;
    place.design=design(NativeModule::reverb);place.design->units[0].defaults={{0,"reverb.mix",.18f}};
    place.design->panel.controls={{"Mix",0,0,"reverb.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
    place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
    CHECK(makeRackCandidate(initial,place,SoundEditOrigin::manual,legacy,diff)==RackCandidateStatus::ready);
    const auto mix=*rackParameterIndex(NativeModule::reverb,0,"reverb.mix");
    const auto drive=*rackParameterIndex(NativeModule::drive,0,"drive.gain_db");
    const auto delay=*rackParameterIndex(NativeModule::delay,0,"delay.feedback");
    legacy.values[drive]=.79f;legacy.values[delay]=.29f;legacy.compatibility.effects[7][2]=2;
    legacy.values[*rackParameterIndex(NativeModule::reverb,7,"reverb.mix")]=.67f;
    legacy.macros[0]={parameterRegistry()[mix].stateKey,.31f};CHECK(validateSoundDocument(legacy));
    const auto before=legacy;
    auto modern=design(NativeModule::reverb);modern.id=juce::Uuid("9138771513d34c57a47912251842d603");modern.revision=9;
    modern.units[0].algorithmRevision=2;modern.units[0].defaults={{0,"reverb.mix",.46f},{0,"reverb.decay_seconds",.27f}};
    modern.panel.controls=place.design->panel.controls;
    RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=place.newInstance;swap.design=modern;
    swap.allowSlotReuse=true;swap.destination=place.destination;swap.allocation={{0,0}};
    SoundDocument upgraded=legacy;
    CHECK(makeRackCandidate(legacy,swap,SoundEditOrigin::aiCandidate,upgraded,diff)==RackCandidateStatus::ready);
    CHECK(legacy==before);CHECK(upgraded.values[mix]==.46f&&upgraded.values[drive]==.79f&&upgraded.values[delay]==.29f);
    auto expectedCompatibility=before.compatibility;expectedCompatibility.effects[0][2]=2;
    CHECK(upgraded.compatibility==expectedCompatibility);CHECK(upgraded.macros==before.macros);
    CHECK(upgraded.controlLocks==before.controlLocks&&upgraded.effectLocks==before.effectLocks&&upgraded.synthLocks==before.synthLocks);
    CHECK(upgraded.personalities&&upgraded.personalities->instances.size()==1);if(!upgraded.personalities||upgraded.personalities->instances.size()!=1)return;
    const auto& upgradedInstance=upgraded.personalities->instances[0];
    CHECK(upgradedInstance.id==place.newInstance&&upgradedInstance.design==modern&&upgradedInstance.bindings==place.allocation&&!upgradedInstance.edited);
    for(int lock:{0,1}){
        auto locked=legacy;if(lock==0)locked.effectLocks[0]=true;else locked.controlLocks[mix]=true;
        auto out=upgraded;RackDifference retained{"sonic","presentation","structural","automation"};
        CHECK(makeRackCandidate(locked,swap,SoundEditOrigin::aiCandidate,out,retained)==RackCandidateStatus::lockedDependency);
        CHECK(out==upgraded&&retained.sonic=="sonic"&&retained.presentation=="presentation"&&retained.structural=="structural"&&retained.automationWarning=="automation");
    }
    for(auto module:{NativeModule::drive,NativeModule::delay,NativeModule::compressor,NativeModule::reverb}){
        auto invalidSwap=swap;invalidSwap.design=design(module);invalidSwap.design->units[0].algorithmRevision=3;
        auto out=upgraded;diff.sonic="sentinel";CHECK(makeRackCandidate(legacy,invalidSwap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::invalid);
        CHECK(out==upgraded&&diff.sonic=="sentinel");
    }
    auto invalidSource=upgraded;invalidSource.compatibility.effects[7][0]=3;auto out=upgraded;
    CHECK(reconcile(upgraded,invalidSource,out)==RackCandidateStatus::invalid);CHECK(out==upgraded);
    upgraded.values[mix]=.61f;
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid();duplicate.destination={RackRole::fx,masterChain,1};duplicate.allocation={{0,1}};
    SoundDocument duplicated;CHECK(makeRackCandidate(upgraded,duplicate,SoundEditOrigin::manual,duplicated,diff)==RackCandidateStatus::ready);
    CHECK(duplicated.compatibility.effects[1]==upgraded.compatibility.effects[0]);CHECK(duplicated.compatibility.effects[7]==upgraded.compatibility.effects[7]);
    CHECK(duplicated.personalities&&duplicated.personalities->instances.size()==2);if(!duplicated.personalities||duplicated.personalities->instances.size()!=2)return;
    CHECK(duplicated.personalities->instances[0].design==modern&&duplicated.personalities->instances[1].design==modern);
    CHECK(duplicated.personalities->instances[0].id==place.newInstance&&duplicated.personalities->instances[1].id==duplicate.newInstance);
    CHECK(duplicated.personalities->instances[1].bindings==duplicate.allocation);
    for(const auto& parameter:parameterRegistry().first(dynamicEqParameterStart))if(parameter.id.startsWith("effect.1.")){
        const auto key=parameter.id.substring(9);CHECK(duplicated.values[*rackParameterIndex(NativeModule::reverb,1,key)]==upgraded.values[*rackParameterIndex(NativeModule::reverb,0,key)]);
    }
    RackOperation detach;detach.kind=RackOperationKind::detach;detach.selected=place.newInstance;
    SoundDocument generic;CHECK(makeRackCandidate(duplicated,detach,SoundEditOrigin::manual,generic,diff)==RackCandidateStatus::ready);
    CHECK(generic.values==duplicated.values&&generic.compatibility==duplicated.compatibility);
    const auto genericBefore=generic;RackDesign captured;
    const std::array<UnitBinding,1> chosen{{{0,0}}};
    CHECK(captureGenericRackDesign(generic,RackRole::fx,chosen,modern.panel,juce::Uuid(),"Captured modern","Native Reverb2",captured));
    CHECK(generic==genericBefore);CHECK(captured.units.size()==1);if(captured.units.size()!=1)return;
    CHECK(captured.units[0].module==NativeModule::reverb&&captured.units[0].algorithmRevision==2);
    CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& value){return value.key=="reverb.mix"&&value.normalized==.61f;}));
    RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=captured;adopt.allocation={{0,0}};adopt.destination=place.destination;
    CHECK(makeRackCandidate(generic,adopt,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.values==generic.values&&out.compatibility==generic.compatibility&&out.macros==generic.macros);
    const auto adopted=out;adopt.design->units[0].algorithmRevision=1;diff.sonic="sentinel";
    CHECK(makeRackCandidate(generic,adopt,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::invalid);CHECK(out==adopted&&diff.sonic=="sentinel");
    swap.design=place.design;CHECK(makeRackCandidate(upgraded,swap,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.compatibility.effects[0][2]==1&&out.compatibility.effects[7][2]==2&&out.values[drive]==.79f&&out.values[delay]==.29f);
}

namespace {
const RackCatalogEntry* catalog_entry(const RackCatalogSnapshot& snapshot,juce::Uuid id) {
    const auto found=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[&](const auto& entry){return entry.definition.id==id;});
    return found==snapshot.entries.end()?nullptr:&*found;
}
bool catalog_difference_unchanged(const RackDifference& difference) {
    return difference.sonic=="sonic"&&difference.presentation=="presentation"&&difference.structural=="structural"&&difference.automationWarning=="automation";
}
bool catalog_public_snapshot_equal(const RackCatalogSnapshot& a,const RackCatalogSnapshot& b) {
    if(a.id!=b.id||a.entries.size()!=b.entries.size())return false;
    for(std::size_t n=0;n<a.entries.size();++n)if(a.entries[n].definition!=b.entries[n].definition||a.entries[n].contentSha256!=b.entries[n].contentSha256)return false;
    return true;
}
RackDesign catalog_full_retained_fx() {
    auto retained=design(NativeModule::drive);retained.id=juce::Uuid("3a16a73a-1e9c-4522-a5e0-e12774a5bc01");retained.revision=29;
    retained.name="Retained native bank";retained.intent="Full active and inactive native values";
    retained.panel.controls={{"Drive",0,0,"drive.gain_db",FocusWidget::rotary,FocusResponse::linear,0,24}};
    std::size_t ordinal=0;
    for(const auto& parameter:parameterRegistry().first(dynamicEqParameterStart))if(parameter.id.startsWith("effect.1.")) {
        const auto key=parameter.id.substring(9);
        const float normalized=parameter.kind==ParameterKind::continuous?float((ordinal%17)+1)/19.f:(ordinal%2?1.f:0.f);
        retained.units[0].defaults.push_back({0,key,normalized});++ordinal;
    }
    for(auto& value:retained.units[0].defaults) {
        if(value.key=="drive.gain_db")value.normalized=.37f;
        if(value.key=="reverb.mix")value.normalized=.63f;
        if(value.key=="delay.feedback")value.normalized=.42f;
        if(value.key=="frequency_shift.hz")value.normalized=.72f;
    }
    return retained;
}
void catalog_trusted_factory_fresh_placement() {
    std::cout<<"catalog_trusted_factory_fresh_placement\n";
    const auto factories=airack::test::frozenFactoryRackDesigns();CHECK(factories.size()==21);
    RackCatalogSnapshot snapshot;RackCatalogStatus status=RackCatalogStatus::unavailable;
    const bool captured=captureRackCatalog(factories,{},{},snapshot,&status);CHECK(captured);if(!captured)return;
    CHECK(status==RackCatalogStatus::ready&&snapshot.entries.size()==21);
    for(const auto& factory:factories) {
        CHECK(factory.units.size()==1);if(factory.units.size()!=1)continue;
        const auto* entry=catalog_entry(snapshot,factory.id);CHECK(entry);if(!entry)continue;
        RackDesign resolved;CHECK(resolveCatalogDesign(snapshot,factory.id,factory.revision,entry->contentSha256,resolved,&status));CHECK(resolved==factory);
        for(const auto origin:{SoundEditOrigin::manual,SoundEditOrigin::aiCandidate}) {
            SoundDocument before;const auto untouchedSynth=*rackParameterIndex(NativeModule::analog,3,"analog.filter.cutoff");
            const auto untouchedFx=*rackParameterIndex(NativeModule::delay,7,"delay.feedback");
            before.values[untouchedSynth]=.73f;before.values[untouchedFx]=.79f;before.compatibility.effects[0][2]=2;before.compatibility.effects[7][2]=2;
            before.controlLocks[untouchedSynth]=true;before.synthLocks[3]=true;before.effectLocks[7]=true;before.macros[0]={parameterRegistry()[untouchedFx].stateKey,.31f};
            CHECK(validateSoundDocument(before));const auto source=before;
            RackOperation operation;operation.newInstance=juce::Uuid();operation.destination={factory.role,masterChain,std::uint8_t(factory.role==RackRole::synth?1:0)};
            const std::uint8_t slot=factory.role==RackRole::synth?1:0;operation.allocation={{factory.units[0].id,slot}};
            SoundDocument placed;RackDifference difference;
            const auto result=makeCatalogRackCandidate(before,snapshot,factory.id,factory.revision,entry->contentSha256,operation,origin,placed,difference);
            CHECK(result==RackCandidateStatus::ready);CHECK(before==source);if(result!=RackCandidateStatus::ready)continue;
            CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.size()!=1)continue;
            const auto& instance=placed.personalities->instances[0];CHECK(instance.id==operation.newInstance&&instance.id!=factory.id&&instance.design==factory&&!instance.edited);
            CHECK(instance.bindings==operation.allocation);CHECK(instance.design.revision==factory.revision);
            const auto& unit=factory.units[0];const bool synth=factory.role==RackRole::synth;
            auto expectedCompatibility=before.compatibility;
            if(synth)expectedCompatibility.synth[slot][static_cast<std::size_t>(unit.module)]=unit.algorithmRevision;
            else expectedCompatibility.effects[slot][static_cast<std::size_t>(effectKindForModule(unit.module))]=unit.algorithmRevision;
            CHECK(placed.compatibility==expectedCompatibility);
            for(const auto& value:unit.defaults) {const auto index=rackParameterIndex(unit.module,slot,value.key);CHECK(index.has_value());if(index)CHECK(placed.values[*index]==value.normalized);}
            for(std::size_t n=0;n<factory.panel.controls.size();++n)CHECK(rackControlActive(placed,instance,n));
            const auto ownedPrefix=juce::String(synth?"synth.":"effect.")+juce::String(slot+1)+".";
            const auto registry=parameterRegistry();for(std::size_t n=0;n<registry.size();++n)if(!registry[n].id.startsWith(ownedPrefix))CHECK(placed.values[n]==before.values[n]);
            CHECK(placed.macros==before.macros&&placed.controlLocks==before.controlLocks&&placed.synthLocks==before.synthLocks&&placed.effectLocks==before.effectLocks);
            CHECK(validateRackInstances(placed)&&validateSoundDocument(placed));
        }
    }
}
void catalog_embedded_full_bank_and_independent_duplicate() {
    std::cout<<"catalog_embedded_full_bank_and_independent_duplicate\n";
    const auto retained=catalog_full_retained_fx();CHECK(retained.units[0].defaults.size()==82);
    CHECK(validateRackDesign(retained,RackValidationMode::instanceRecall));CHECK(!validateRackDesign(retained,RackValidationMode::portableDesign));
    SoundDocument before;before.compatibility.effects[0][2]=2;before.compatibility.effects[7][2]=2;
    const auto untouched=*rackParameterIndex(NativeModule::delay,7,"delay.feedback");before.values[untouched]=.91f;before.macros[0]={parameterRegistry()[untouched].stateKey,.27f};
    CHECK(validateSoundDocument(before));const auto original=before;
    RackOperation operation;operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
    auto ordinary=operation;ordinary.design=retained;auto out=before;RackDifference difference{"sonic","presentation","structural","automation"};
    CHECK(makeRackCandidate(before,ordinary,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::invalid);
    CHECK(out==before&&catalog_difference_unchanged(difference));
    const std::array<RackDesign,1> embedded{{retained}};RackCatalogSnapshot snapshot;RackCatalogStatus status=RackCatalogStatus::unavailable;
    const bool captured=captureRackCatalog({},embedded, {},snapshot,&status);CHECK(captured);if(!captured)return;
    CHECK(snapshot.entries.size()==1);const auto* entry=catalog_entry(snapshot,retained.id);CHECK(entry);if(!entry)return;
    CHECK(rackCatalogNativeEntry(snapshot,0));
    for(const auto origin:{SoundEditOrigin::manual,SoundEditOrigin::aiCandidate}) {
        SoundDocument placed;const auto result=makeCatalogRackCandidate(before,snapshot,retained.id,retained.revision,entry->contentSha256,operation,origin,placed,difference);
        CHECK(result==RackCandidateStatus::ready);if(result!=RackCandidateStatus::ready)continue;
        CHECK(before==original&&placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.size()!=1)continue;
        CHECK(placed.personalities->instances[0].design==retained&&placed.personalities->instances[0].bindings==operation.allocation);
        CHECK(placed.compatibility==before.compatibility&&placed.values[untouched]==.91f&&placed.macros==before.macros);
        for(const auto& value:retained.units[0].defaults)CHECK(placed.values[*rackParameterIndex(NativeModule::drive,0,value.key)]==value.normalized);
        CHECK(placed.values[*rackParameterIndex(NativeModule::drive,0,"drive.gain_db")]==.37f);
        CHECK(placed.values[*rackParameterIndex(NativeModule::drive,0,"reverb.mix")]==.63f);
        CHECK(placed.values[*rackParameterIndex(NativeModule::drive,0,"delay.feedback")]==.42f);
        CHECK(placed.values[*rackParameterIndex(NativeModule::drive,0,"frequency_shift.hz")]==.72f);
        RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=operation.newInstance;duplicate.newInstance=juce::Uuid();duplicate.destination={RackRole::fx,masterChain,1};duplicate.allocation={{0,1}};
        SoundDocument duplicated;const auto duplicateStatus=makeRackCandidate(placed,duplicate,origin,duplicated,difference);CHECK(duplicateStatus==RackCandidateStatus::ready);if(duplicateStatus!=RackCandidateStatus::ready)continue;
        CHECK(duplicated.personalities&&duplicated.personalities->instances.size()==2);if(!duplicated.personalities||duplicated.personalities->instances.size()!=2)continue;
        const auto& first=duplicated.personalities->instances[0];const auto& second=duplicated.personalities->instances[1];
        CHECK(first.id==operation.newInstance&&second.id==duplicate.newInstance&&first.id!=second.id&&first.design==retained&&second.design==retained);
        CHECK(first.bindings==operation.allocation&&second.bindings==duplicate.allocation&&first.bindings!=second.bindings);
        CHECK(duplicated.compatibility.effects[1]==placed.compatibility.effects[0]&&duplicated.compatibility.effects[7]==before.compatibility.effects[7]);
        for(const auto& value:retained.units[0].defaults)CHECK(duplicated.values[*rackParameterIndex(NativeModule::drive,1,value.key)]==value.normalized);
        const auto firstGain=*rackParameterIndex(NativeModule::drive,0,"drive.gain_db"),secondGain=*rackParameterIndex(NativeModule::drive,1,"drive.gain_db");
        const auto firstInactive=*rackParameterIndex(NativeModule::drive,0,"reverb.mix"),secondInactive=*rackParameterIndex(NativeModule::drive,1,"reverb.mix");
        duplicated.values[firstGain]=.11f;duplicated.values[firstInactive]=.22f;
        CHECK(duplicated.values[secondGain]==.37f&&duplicated.values[secondInactive]==.63f);
        CHECK(first.design==retained&&second.design==retained);CHECK(validateRackInstances(duplicated)&&validateSoundDocument(duplicated));
    }
}
void catalog_reuse_capacity_and_locks_are_atomic() {
    std::cout<<"catalog_reuse_capacity_and_locks_are_atomic\n";
    const auto factories=airack::test::frozenFactoryRackDesigns();RackCatalogSnapshot snapshot;RackCatalogStatus status=RackCatalogStatus::unavailable;
    const bool captured=captureRackCatalog(factories,{},{},snapshot,&status);CHECK(captured);if(!captured)return;
    const auto* synth=catalog_entry(snapshot,juce::Uuid("91387715-13d3-4c57-a479-12251842a402"));
    const auto* fx=catalog_entry(snapshot,juce::Uuid("91387715-13d3-4c57-a479-12251842e402"));CHECK(synth&&fx);if(!synth||!fx)return;
    SoundDocument before;RackOperation reuse;reuse.newInstance=juce::Uuid();reuse.destination={RackRole::synth,masterChain,0};reuse.allocation={{0,0}};
    for(const auto origin:{SoundEditOrigin::manual,SoundEditOrigin::aiCandidate}) {
        auto out=before;RackDifference difference{"sonic","presentation","structural","automation"};
        CHECK(makeCatalogRackCandidate(before,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,reuse,origin,out,difference)==RackCandidateStatus::replacementRequired);
        CHECK(out==before&&catalog_difference_unchanged(difference));
        reuse.allowSlotReuse=true;const auto result=makeCatalogRackCandidate(before,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,reuse,origin,out,difference);
        CHECK(result==RackCandidateStatus::ready);if(result==RackCandidateStatus::ready)CHECK(out.layout.synthCount==1&&out.personalities&&out.personalities->instances.size()==1&&out.personalities->instances[0].bindings==reuse.allocation);
        reuse.allowSlotReuse=false;
    }
    reuse.allowSlotReuse=true;const auto cutoff=*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff");
    for(int lock:{0,1}) {
        auto locked=before;locked.values[cutoff]=.91f;if(lock==0)locked.synthLocks[0]=true;else locked.controlLocks[cutoff]=true;
        auto out=before;RackDifference difference{"sonic","presentation","structural","automation"};
        CHECK(makeCatalogRackCandidate(locked,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,reuse,SoundEditOrigin::aiCandidate,out,difference)==RackCandidateStatus::lockedDependency);
        CHECK(out==before&&catalog_difference_unchanged(difference));
        CHECK(makeCatalogRackCandidate(locked,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,reuse,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::ready);
        CHECK(out.synthLocks==locked.synthLocks&&out.controlLocks==locked.controlLocks);
    }
    auto fullSynth=before,fullFx=before;
    for(std::uint8_t slot=1;slot<4;++slot)CHECK(editRackLayout(fullSynth.layout,{RackEditKind::addSynth,slot,masterChain,slot},fullSynth.layout)==RackEditResult::applied);
    for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(fullFx.layout,{RackEditKind::addEffect,slot,masterChain,slot},fullFx.layout)==RackEditResult::applied);
    for(const auto origin:{SoundEditOrigin::manual,SoundEditOrigin::aiCandidate})for(const auto* entry:{synth,fx}) {
        const bool isSynth=entry==synth;const auto& full=isSynth?fullSynth:fullFx;RackOperation operation;operation.newInstance=juce::Uuid();operation.destination={entry->definition.role,masterChain,std::uint8_t(isSynth?4:8)};
        auto out=before;RackDifference difference{"sonic","presentation","structural","automation"};
        CHECK(makeCatalogRackCandidate(full,snapshot,entry->definition.id,entry->definition.revision,entry->contentSha256,operation,origin,out,difference)==RackCandidateStatus::capacityReached);
        CHECK(out==before&&catalog_difference_unchanged(difference));
    }
    RackOperation first;first.newInstance=juce::Uuid();first.destination={RackRole::synth,masterChain,1};first.allocation={{0,1}};
    SoundDocument owned;RackDifference difference;const auto result=makeCatalogRackCandidate(before,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,first,SoundEditOrigin::manual,owned,difference);
    CHECK(result==RackCandidateStatus::ready);if(result!=RackCandidateStatus::ready)return;
    auto collision=first;collision.newInstance=juce::Uuid();auto out=before;difference={"sonic","presentation","structural","automation"};
    CHECK(makeCatalogRackCandidate(owned,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,collision,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::replacementRequired);
    CHECK(out==before&&catalog_difference_unchanged(difference));collision.allowSlotReuse=true;
    CHECK(makeCatalogRackCandidate(owned,snapshot,synth->definition.id,synth->definition.revision,synth->contentSha256,collision,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::reconciliationRequired);
    CHECK(out==before&&catalog_difference_unchanged(difference));
}
void catalog_public_and_registered_data_cannot_forge_native_authority() {
    std::cout<<"catalog_public_and_registered_data_cannot_forge_native_authority\n";
    const auto retained=catalog_full_retained_fx();const std::array<RackDesign,1> embedded{{retained}};
    RackCatalogSnapshot trusted;RackCatalogStatus status=RackCatalogStatus::unavailable;const bool captured=captureRackCatalog({},embedded, {},trusted,&status);CHECK(captured);if(!captured)return;
    const auto* entry=catalog_entry(trusted,retained.id);CHECK(entry);if(!entry)return;
    RackOperation operation;operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
    SoundDocument before;const auto sentinel=before;RackDifference difference;
    RackCatalogSnapshot forged;forged.id=trusted.id;forged.entries=trusted.entries;
    auto tampered=trusted;tampered.entries[0].definition.units[0].defaults[0].normalized=tampered.entries[0].definition.units[0].defaults[0].normalized==0.f?1.f:0.f;
    auto changedIdentity=trusted;changedIdentity.id=juce::Uuid();
    for(const auto* invalid:{&forged,&tampered,&changedIdentity}) {
        CHECK(!rackCatalogNativeEntry(*invalid,0));auto resolved=design();const auto previous=resolved;
        CHECK(!resolveCatalogDesign(*invalid,retained.id,retained.revision,entry->contentSha256,resolved,&status));CHECK(resolved==previous);
        auto out=sentinel;difference={"sonic","presentation","structural","automation"};
        CHECK(makeCatalogRackCandidate(before,*invalid,retained.id,retained.revision,entry->contentSha256,operation,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::invalid);
        CHECK(out==sentinel&&catalog_difference_unchanged(difference));
    }
    for(int mismatch:{0,1,2}) {
        const auto id=mismatch==0?juce::Uuid():retained.id;const auto revision=mismatch==1?retained.revision+1:retained.revision;
        const juce::String hash=mismatch==2?juce::String("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"):entry->contentSha256;
        auto out=sentinel;difference={"sonic","presentation","structural","automation"};
        CHECK(makeCatalogRackCandidate(before,trusted,id,revision,hash,operation,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::invalid);
        CHECK(out==sentinel&&catalog_difference_unchanged(difference));
    }
    auto supplied=operation;supplied.design=retained;auto out=sentinel;difference={"sonic","presentation","structural","automation"};
    CHECK(makeCatalogRackCandidate(before,trusted,retained.id,retained.revision,entry->contentSha256,supplied,SoundEditOrigin::manual,out,difference)==RackCandidateStatus::invalid);
    CHECK(out==sentinel&&catalog_difference_unchanged(difference));
    const auto directory=juce::File::getCurrentWorkingDirectory().getChildFile("task11-catalog-authority-"+juce::Uuid().toString());
    CHECK(directory.createDirectory());const auto source=directory.getChildFile("matching-native.airack");juce::MemoryBlock nativeBytes;
    CHECK(encodeRackDesign(retained,nativeBytes,RackValidationMode::instanceRecall));CHECK(source.replaceWithData(nativeBytes.getData(),nativeBytes.getSize()));
    auto portableOutput=design();const auto portableBefore=portableOutput;
    CHECK(!loadRackDesign(source,portableOutput));CHECK(portableOutput==portableBefore);
    CHECK(directory.deleteRecursively());
}
// The face editor (its plan's Task 1): the selected rack's design takes another panel and name, forked, and nothing the
// sound plays changes. A refused edit leaves the output and the difference as they were.
void face_edit(){
    std::cout<<"face_edit\n";
    const auto d=copperArcSound();const auto copper=d.personalities->instances[0];
    RackOperation op;op.kind=RackOperationKind::face;op.selected=copper.id;
    auto edited=copper.design;edited.name="Copper Bite";edited.panel.controls[0].label="Bite";edited.panel.controls[7].section=0;edited.panel.accent=1;
    op.design=edited;SoundDocument out;RackDifference diff;
    CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(out.personalities&&out.personalities->instances.size()==1);
    if(out.personalities&&out.personalities->instances.size()==1){
        const auto& i=out.personalities->instances[0];
        CHECK(out.values==d.values&&out.layout==d.layout&&out.compatibility==d.compatibility&&out.macros==d.macros);
        CHECK(i.id==copper.id&&i.bindings==copper.bindings&&i.design.id==copper.design.id&&i.design.units==copper.design.units&&i.design.intent==copper.design.intent);
        CHECK(i.design.panel==edited.panel&&i.design.name=="Copper Bite");
        CHECK(i.design.revision==copper.design.revision+1&&i.edited);
        const std::vector<RackDesign> embedded{i.design};RackCatalogSnapshot snapshot;RackCatalogStatus status{};
        CHECK(captureRackCatalog(airack::test::frozenLibraryRackDesigns(),embedded,{},snapshot,&status)&&status==RackCatalogStatus::ready);
        // Two edits fork twice.
        auto again=op;again.selected=i.id;again.design=i.design;again.design->panel.controls[1].label="Peak";SoundDocument twice;
        CHECK(makeRackCandidate(out,again,SoundEditOrigin::manual,twice,diff)==RackCandidateStatus::ready);
        if(twice.personalities&&twice.personalities->instances.size()==1)CHECK(twice.personalities->instances[0].design.revision==copper.design.revision+2);
    }
    CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    for(const auto* line:{"Name Copper Arc -> Copper Bite","Change control Brightness -> Bite","Change control Motion","Accent neutral -> cool"})CHECK(diff.presentation.contains(line));
    CHECK(!diff.presentation.contains("Change control Resonance"));
    // A control added and one removed.
    auto grown=copper.design;grown.panel.controls.pop_back();grown.panel.controls.insert(grown.panel.controls.begin(),{"Mode",0,0,"analog.filter.mode",FocusWidget::choice,FocusResponse::linear,0,2});
    op.design=grown;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(diff.presentation.contains("Add control Mode")&&diff.presentation.contains("Remove control Motion")&&!diff.presentation.contains("Reorder controls")&&!diff.presentation.contains("Change control"));
    auto swapped=copper.design;std::swap(swapped.panel.controls[0],swapped.panel.controls[1]);
    op.design=swapped;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
    CHECK(diff.presentation.contains("Reorder controls")&&!diff.presentation.contains("Change control"));
    // Refused: the output and the difference stay as they were.
    const auto sentinel=out;const RackDifference marked{"sonic","presentation","structural","automation"};
    const auto refused=[&](RackOperation o,int line){
        auto result=sentinel;auto difference=marked;const auto status=makeRackCandidate(d,o,SoundEditOrigin::manual,result,difference);
        if(status!=RackCandidateStatus::invalid||result!=sentinel||difference.sonic!="sonic"||difference.presentation!="presentation")std::cerr<<"face_edit refusal at line "<<line<<'\n';
        CHECK(status==RackCandidateStatus::invalid&&result==sentinel&&difference.presentation=="presentation");
    };
    auto bad=op;auto design=copper.design;
    design.panel.controls[0].key="wavetable.osc.1.position";design.panel.controls[0].minimum=0;design.panel.controls[0].maximum=1;design.panel.controls[0].response=FocusResponse::linear;bad.design=design;refused(bad,__LINE__);
    design=copper.design;design.panel.controls[0].section=3;bad.design=design;refused(bad,__LINE__);
    design=copper.design;design.panel.controls.push_back({"Mode",0,0,"analog.filter.mode",FocusWidget::choice,FocusResponse::linear,0,2});bad.design=design;refused(bad,__LINE__);
    design=copper.design;design.panel.sections.clear();for(auto& c:design.panel.controls)c.section=0;bad.design=design;refused(bad,__LINE__);
    design=copper.design;design.units[0].defaults.clear();design.name="Other units";bad.design=design;refused(bad,__LINE__);
    design=copper.design;design.id=juce::Uuid();design.name="Other ID";bad.design=design;refused(bad,__LINE__);
    bad=op;bad.selected=juce::Uuid();refused(bad,__LINE__);
    bad=op;bad.design=copper.design;refused(bad,__LINE__);
    bad=op;bad.design.reset();refused(bad,__LINE__);
    auto last=d;auto state=std::make_shared<RackPersonalityState>(*d.personalities);state->instances[0].design.revision=std::numeric_limits<std::uint64_t>::max();last.personalities=state;
    bad=op;bad.design=edited;bad.design->revision=std::numeric_limits<std::uint64_t>::max();
    {auto result=sentinel;auto difference=marked;CHECK(makeRackCandidate(last,bad,SoundEditOrigin::manual,result,difference)==RackCandidateStatus::invalid&&result==sentinel);}
}
}
int main(){drumModuleContract();drumRetainedSource();drumPlacementBanks();face_edit();catalog_trusted_factory_fresh_placement();catalog_embedded_full_bank_and_independent_duplicate();catalog_reuse_capacity_and_locks_are_atomic();catalog_public_and_registered_data_cannot_forge_native_authority();algorithm_revision_transaction();modern_reverb_swap_duplicate_and_capture();twelve_record_native_replacement();explicit_removal_order_and_replacement();patch_is_not_design();all_paths_reconcile();segment_move_and_split();dependent_fx_survives_synth_removal();structural_edge_contracts();explicit_resolutions_and_bounds();library_operation_bank_contracts();library_transactions();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
