#include "FrozenRackExamples.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "RackReconciliation.h"
#include "UserReach.h"
#include "AiCatalog.h"
#include "ProposalDocument.h"
#include "SoundState.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

#ifndef RACKSYNTH_SPACE_BASELINE_DIR
#define RACKSYNTH_SPACE_BASELINE_DIR ""
#endif

namespace {
using namespace airack;
constexpr auto module=NativeModule(18);
int checks=0,failures=0;
void check(bool good,const char* expression,int line) {
    ++checks;if(!good){++failures;if(failures<90)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}
}
#define CHECK(x) check(bool(x),#x,__LINE__)
std::optional<RackDesign> factory() {
    const auto all=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(all.begin(),all.end(),[](const auto& d){return d.name=="Lumen Field";});
    CHECK(found!=all.end());return found==all.end()?std::nullopt:std::optional<RackDesign>(*found);
}
void frozenLibrary() {
    std::cout<<"SECTION exact_frozen_twenty_two_library_designs\n";
    auto library=airack::test::frozenLibraryRackDesigns();std::erase_if(library,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});const auto tile=airack::test::frozenTileArrayRackDesign();CHECK(library.size()==26&&std::count(library.begin(),library.end(),tile)==1);std::erase(library,tile);CHECK(library.size()==25);const auto small=airack::test::frozenExampleRackDesigns()[4].id,night=airack::test::frozenExampleRackDesigns()[5].id;
    std::vector<RackDesign> designs;for(const auto& d:library)if(d.id!=small&&d.id!=night)designs.push_back(d);CHECK(designs.size()==23);if(designs.size()<22)return;
    const auto directory=juce::File(juce::String(RACKSYNTH_SPACE_BASELINE_DIR));
    for(std::size_t n=0;n<22;++n){const auto& design=designs[n];juce::MemoryBlock old,current;
        CHECK(directory.getChildFile("factory-"+design.id.toString()+".rackdesign").loadFileAsData(old));
        RackDesign historical;const bool decoded=decodeRackDesign(old.getData(),old.getSize(),historical);CHECK(decoded);if(!decoded)continue;
        CHECK(encodeRackDesign(historical,current));CHECK(current==old);
        auto preserved=design;
        if(design.id==juce::Uuid("330cb53b-cd9b-443c-94d8-2700bc8b88f7")){
            CHECK(preserved.units.size()==1&&historical.units.size()==1);if(preserved.units.size()!=1||historical.units.size()!=1)continue;
            auto& values=preserved.units.front().defaults;
            CHECK(values.size()==historical.units.front().defaults.size()+1);
            CHECK(!values.empty()&&values.back().key=="delay.note"&&values.back().normalized==11.f/21.f);
            if(!values.empty())values.pop_back();
        }
        CHECK(encodeRackDesign(preserved,current));CHECK(current==old);
    }
}
void frozenLegacyAudio() {
    std::cout<<"SECTION actual_frozen_sound20_legacy_audio_is_bit_exact\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_SPACE_BASELINE_DIR));
    juce::MemoryBlock state,raw;CHECK(directory.getChildFile("rich-v20.soundstate").loadFileAsData(state));
    CHECK(directory.getChildFile("rich-v20-48k-64.raw").loadFileAsData(raw));if(raw.getSize()<16)return;
    SoundDocument sound;CHECK(decodeSoundState(state.getData(),state.getSize(),sound));RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
    RoutedSynthEngine engine;CHECK(engine.prepare(48000,512)&&engine.setPatch(patch));engine.reset();
    juce::MemoryInputStream input(raw,false);CHECK(input.readInt()==0x30465241);CHECK(input.readInt()==48000);
    CHECK(input.readInt()==12032);CHECK(input.readInt()==64);juce::AudioBuffer<float> buffer(2,64);juce::MidiBuffer midi;midi.ensureSize(256);
    for(int start=0;start<12032;start+=64){midi.clear();if(start==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        if(start==4096)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);engine.render(buffer,midi,120.);
        for(int n=0;n<64;++n)for(int channel=0;channel<2;++channel){const auto expected=input.readFloat(),actual=buffer.getSample(channel,n);CHECK(std::isfinite(actual));CHECK(actual==expected);}
    }CHECK(input.getPosition()==juce::int64(raw.getSize()));
}
void moduleRecipeAndReach() {
    std::cout<<"SECTION space_module_complete97_field_recipe_and_ordinary_reach\n";
    CHECK(moduleDisplayName(module)=="Space Reverb");CHECK(effectDisplayName(EffectKind(16))=="Space Reverb");
    CHECK(supportsAlgorithm(module,1));CHECK(!supportsAlgorithm(module,2));CHECK(supportsAlgorithm(NativeModule(20),1));CHECK(supportsAlgorithm(NativeModule(21),1));CHECK(!supportsAlgorithm(NativeModule(22),1));
    CHECK(rackUnitParameterCapacity(module)==97);CHECK(rackUnitParameterCapacity(NativeModule::dynamicEq)==181);
    const auto found=factory();if(!found)return;const auto& design=*found;
    CHECK(design.id==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0"));CHECK(design.revision==1);
    CHECK(design.role==RackRole::fx&&design.units.size()==1);if(design.units.size()!=1)return;
    const auto& unit=design.units[0];CHECK(unit.module==module&&unit.algorithmRevision==1&&unit.defaults.size()==97);
    CHECK(validateRackDesign(design,RackValidationMode::portableDesign));
    juce::MemoryBlock wire;RackDesign recalled;CHECK(encodeRackDesign(design,wire));CHECK(decodeRackDesign(wire.getData(),wire.getSize(),recalled));CHECK(recalled==design);
    std::set<juce::String> keys;for(const auto& value:unit.defaults){CHECK(keys.insert(value.key).second);CHECK(value.key.getNumBytesAsUTF8()<=32);
        CHECK(rackParameterActive(module,value.key));CHECK(rackParameterIndex(module,0,value.key));}
    CHECK(keys.contains("bypassed")&&keys.contains("space_reverb.decay.6.multiplier")&&keys.contains("space_reverb.post.6.slope"));
    CHECK(!rackParameterActive(module,"dynamic_eq.band.1.gain_db"));
    const std::array<const char*,8> labels{"Size","Decay","Predelay","Brightness","Character","Distance","Width","Mix"};
    CHECK(design.panel.controls.size()==8);for(std::size_t i=0;i<std::min(labels.size(),design.panel.controls.size());++i)CHECK(design.panel.controls[i].label==labels[i]);
    const auto schema=rackDesignSchema();const auto choices=schema["properties"]["units"]["items"]["anyOf"];
    CHECK(choices.isArray()&&choices.size()==22);if(choices.isArray()&&choices.size()==22)CHECK(int(choices[18]["properties"]["defaults"]["maxItems"])==97);
    for(std::uint8_t slot=0;slot<8;++slot)for(const auto chain:{std::uint8_t(0),masterChain}){
        SoundDocument sound;for(std::size_t i=2063;i<2215;++i){sound.values[i]=parameterRegistry()[i].kind==ParameterKind::continuous?.375f:1.f;sound.controlLocks[i]=i%2==0;}
        RackOperation operation;operation.design=design;operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,chain,0};operation.allocation={{0,slot}};
        SoundDocument placed;RackDifference difference;CHECK(makeRackCandidate(sound,operation,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        // Fresh placement initializes its whole physical slot; unrelated slots and all locks retain their values.
        const auto defaults=defaultParameterValues();const auto allocated="effect."+juce::String(slot+1)+".";
        for(std::size_t i=2063;i<2215;++i){CHECK(placed.values[i]==(parameterRegistry()[i].id.startsWith(allocated)?defaults[i]:sound.values[i]));CHECK(placed.controlLocks[i]==sound.controlLocks[i]);}
        CHECK(placed.layout.effects[slot].kind==EffectKind(16));CHECK(placed.layout.effectCount[chain]==1);CHECK(placed.personalities&&placed.personalities->instances.size()==1);
        const auto reach=unitReach(module,slot);CHECK(reach.size()==97);const auto mask=userReach(placed);for(const auto i:reach)CHECK(mask[i]);
        const auto prefix="effect."+juce::String(slot+1)+".";
        for(const auto& value:unit.defaults){const auto i=rackParameterIndex(module,slot,value.key);CHECK(i);if(i)CHECK(placed.values[*i]==value.normalized);}
        const auto physical=[&](const char* key){const auto i=rackParameterIndex(module,slot,key);CHECK(i);return i?parameterPhysicalValue(placed.values,*i):0.f;};
        CHECK(std::abs(physical("space_reverb.size")-.65f)<1e-5f);CHECK(std::abs(physical("space_reverb.decay_seconds")-3.2f)<1e-4f);
        CHECK(physical("space_reverb.decay.1.shape")==2&&physical("space_reverb.post.1.shape")==3);
        CHECK(physical("space_reverb.decay.1.active")==1&&physical("space_reverb.post.1.active")==1);
        auto detached=placed;detached.personalities.reset();RackDesign captured;
        CHECK(captureGenericRackDesign(detached,RackRole::fx,std::array<UnitBinding,1>{{{0,slot}}},design.panel,juce::Uuid(),"Captured Space","All retained values",captured));
        CHECK(captured.units.size()==1);if(captured.units.size()==1)CHECK(captured.units[0].defaults==unit.defaults);
        juce::MemoryBlock state;CHECK(encodeSoundState(placed,state));SoundDocument again;CHECK(decodeSoundState(state.getData(),state.getSize(),again));CHECK(again==placed);
    }
}
void fullInventory() {
    std::cout<<"SECTION all67_sources_paginate_resolve_and_ai_inventory\n";
    const auto chosen=factory();if(!chosen)return;auto builtins=airack::test::frozenLibraryRackDesigns();std::erase_if(builtins,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});const auto tile=airack::test::frozenTileArrayRackDesign();CHECK(builtins.size()==26&&std::count(builtins.begin(),builtins.end(),tile)==1);std::erase(builtins,tile);CHECK(builtins.size()==25);
    std::vector<RackDesign> embedded;
    for(int n=0;n<42;++n){auto design=*chosen;design.id=juce::Uuid();design.name="Embedded Space "+juce::String(n+1);embedded.push_back(design);}
    RackCatalogCaptureLimits historicalLimits;historicalLimits.maximumEntries=historicalLimits.maximumSources=67;
    RackCatalogSnapshot snapshot;RackCatalogStatus status;CHECK(captureRackCatalog(builtins,embedded,historicalLimits,snapshot,&status));CHECK(status==RackCatalogStatus::ready);CHECK(snapshot.entries.size()==67);
    if(snapshot.entries.size()!=67)return;
    std::size_t cursor=0,seen=0;while(cursor<snapshot.entries.size()){
        RackCatalogPage page;CHECK(catalogPage(snapshot,{},cursor,65536,page));CHECK(page.next>cursor);if(page.next<=cursor)break;
        for(const auto& entry:page.entries){RackDesign resolved;CHECK(resolveCatalogDesign(snapshot,entry.definition.id,entry.definition.revision,entry.contentSha256,resolved));CHECK(resolved==entry.definition);++seen;}
        cursor=page.next;
    }CHECK(seen==67);
    juce::var inventory;CHECK(ai::catalogInventoryFor(snapshot,inventory));
    auto authority=std::make_shared<ai::CatalogAuthority>();authority->root={juce::Uuid(),1,0,0};authority->snapshot=snapshot.id;authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();
    CHECK(ai::detail::catalogAuthorityValid(*authority));
    auto extra=*chosen;extra.id=juce::Uuid();embedded.push_back(extra);const auto before=snapshot.id;
    CHECK(!captureRackCatalog(builtins,embedded,historicalLimits,snapshot,&status));CHECK(status==RackCatalogStatus::countLimit);CHECK(snapshot.id==before);
    auto limits=historicalLimits;limits.maximumPreparedBytes=1;embedded.pop_back();CHECK(!captureRackCatalog(builtins,embedded,limits,snapshot,&status));CHECK(status==RackCatalogStatus::preparedLimit);CHECK(snapshot.id==before);
    std::cout<<"SPACE_INVENTORY entries="<<seen<<" descriptor_bytes="<<authority->descriptorJson.size()<<'\n';
}
void aiProposals() {
    using namespace ai;std::cout<<"SECTION catalog_place_refine_all97_fields_locks_stale_and_foreign_edits\n";
    const auto chosen=factory();if(!chosen)return;
    ProjectDocument original,placed;ProposalPolicy policy;ProposalEdits selection;
    for(std::uint8_t slot=0;slot<7;++slot){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.position=slot;add.effect=EffectKind::drive;CHECK(editRackLayout(original.sound.layout,add,original.sound.layout)==RackEditResult::applied);}
    RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(std::vector<RackDesign>{*chosen},{},{},snapshot));if(snapshot.entries.size()!=1){CHECK(false);return;}
    juce::var inventory;CHECK(catalogInventoryFor(snapshot,inventory));CatalogAuthority authority;authority.root={juce::Uuid(),1,0,0};authority.snapshot=snapshot.id;authority.descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();
    const auto& entry=snapshot.entries.front();CatalogDefinition definition{chosen->id,chosen->revision,entry.contentSha256,{}};CHECK(encodeRackDesign(*chosen,definition.canonical));authority.definitions.push_back(definition);
    selection.catalogSelections={{7,chosen->id,chosen->revision,entry.contentSha256,{RackRole::fx,masterChain,7}}};
    CHECK(admitProposal(original,policy,selection,authority,placed)==ProposalStatus::accepted);CHECK(placed.sound.personalities&&placed.sound.personalities->instances.size()==1);if(!placed.sound.personalities)return;
    CHECK(placed.sound.personalities->instances.front().bindings==std::vector<UnitBinding>({{0,7}}));
    ProposalEdits edits;
    const auto reach=unitReach(module,7);CHECK(reach.size()==97);edits={};for(const auto i:reach){const auto& field=parameterRegistry()[i];
        edits.parameters.push_back({field.id,field.kind==ParameterKind::continuous?(i%2?.75f:.25f):placed.sound.values[i]});}
    ProjectDocument candidate;CHECK(admitProposal(placed,policy,edits,candidate)==ProposalStatus::accepted);
    for(std::size_t i=0;i<parameterCount;++i)if(!std::binary_search(reach.begin(),reach.end(),i))CHECK(candidate.sound.values[i]==placed.sound.values[i]);
    for(const auto& edit:edits.parameters){const auto key=edit.id.substring(9);const auto index=rackParameterIndex(module,7,key);CHECK(index);if(index)CHECK(candidate.sound.values[*index]==edit.normalized);}
    const auto reject=[&](const ProjectDocument& source,const ProposalPolicy& rule,const ProposalEdits& proposal){auto output=original;CHECK(admitProposal(source,rule,proposal,output)!=ProposalStatus::accepted);CHECK(output==original);};
    auto locked=placed;locked.sound.effectLocks[7]=true;reject(locked,policy,edits);
    const auto gain=rackParameterIndex(module,7,"space_reverb.post.1.gain_db");CHECK(gain);if(gain){locked=placed;locked.sound.controlLocks[*gain]=true;reject(locked,policy,edits);}
    auto bad=edits;bad.parameters.push_back(edits.parameters.front());reject(placed,policy,bad);
    bad=edits;bad.parameters.front().normalized=std::numeric_limits<float>::quiet_NaN();reject(placed,policy,bad);
    bad=edits;bad.parameters.push_back({"effect.7.space_reverb.size",.7f});reject(placed,policy,bad);
    bad=edits;bad.parameters.push_back({"effect.8.dynamic_eq.band.1.gain_db",.7f});reject(placed,policy,bad);
    auto stale=selection;++stale.catalogSelections.front().revision;auto sentinel=original;CHECK(admitProposal(original,policy,stale,authority,sentinel)!=ProposalStatus::accepted);CHECK(sentinel==original);
    juce::var catalog,provider;CHECK(catalogFor(placed,catalog));CHECK(providerCatalogFor(placed,provider));
    CHECK(provider["module_keys"]["space_reverb"].isArray());CHECK(provider["module_keys"]["space_reverb"].size()==97);
    const auto* controls=catalog["controls"].getArray();CHECK(controls);if(controls)for(const auto i:reach){CHECK(bool((*controls)[int(i)]["active"]));}
    juce::MemoryBlock state;CHECK(encodeProjectState(candidate,state));ProjectDocument recalled;CHECK(decodeProjectState(state.getData(),state.getSize(),recalled));CHECK(recalled==candidate);
}
void physicalOwnershipAndCapacity() {
    std::cout<<"SECTION duplicate_move_retire_replacement_and_eight_slot_capacity\n";
    const auto chosen=factory();if(!chosen)return;SoundDocument sound,placed;RackDifference delta;RackOperation operation;
    operation.design=*chosen;operation.newInstance=juce::Uuid();const auto selected=operation.newInstance;operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
    CHECK(makeRackCandidate(sound,operation,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    operation={};operation.kind=RackOperationKind::duplicate;operation.selected=selected;operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,masterChain,1};operation.allocation={{0,7}};
    SoundDocument duplicate;CHECK(makeRackCandidate(placed,operation,SoundEditOrigin::manual,duplicate,delta)==RackCandidateStatus::ready);
    operation={};operation.kind=RackOperationKind::move;operation.selected=selected;operation.destination={RackRole::fx,0,0};SoundDocument moved;
    CHECK(makeRackCandidate(duplicate,operation,SoundEditOrigin::manual,moved,delta)==RackCandidateStatus::ready);CHECK(moved.values==duplicate.values);CHECK(moved.layout.effects[0].chain==0);
    operation={};operation.kind=RackOperationKind::remove;operation.selected=selected;SoundDocument removed;
    CHECK(makeRackCandidate(moved,operation,SoundEditOrigin::manual,removed,delta)==RackCandidateStatus::ready);CHECK(removed.values==moved.values);CHECK(removed.layout.effects[0].use==SlotUse::retired);
    RackLayout layout;for(std::uint8_t slot=0;slot<8;++slot){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.position=slot;add.effect=EffectKind(16);CHECK(editRackLayout(layout,add,layout)==RackEditResult::applied);}
    CHECK(layout.effectCount[masterChain]==8);RackEdit ninth;ninth.kind=RackEditKind::addEffect;ninth.effect=EffectKind(16);ninth.position=8;const auto prior=layout;
    CHECK(editRackLayout(layout,ninth,layout)!=RackEditResult::applied);CHECK(layout==prior);
}
void explicitRecipeReplacement() {
    std::cout<<"SECTION explicit_space_other_and_same_bank_recipe_swaps_preserve_inactive_banks\n";
    const auto chosen=factory();if(!chosen)return;RackOperation operation;operation.design=*chosen;operation.newInstance=juce::Uuid();
    const auto selected=operation.newInstance;operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
    SoundDocument empty,space;RackDifference delta;CHECK(makeRackCandidate(empty,operation,SoundEditOrigin::manual,space,delta)==RackCandidateStatus::ready);
    const auto registry=parameterRegistry();const auto prefix=juce::String("effect.1.");
    // Nondefault retained values make a broad bank reset observable.
    for(std::size_t i=0;i<registry.size();++i)if(registry[i].id.startsWith(prefix)&&!rackParameterActive(module,registry[i].id.substring(prefix.length())))
        space.values[i]=registry[i].kind==ParameterKind::continuous?.723f:1.f;
    for(std::size_t band=0;band<8;++band){const auto base=dynamicEqDetectorParameterStart+band*dynamicEqDetectorParametersPerBand;space.values[base+3]=.2f;space.values[base+4]=.8f;}
    space.values[dynamicEqSpectralParameterStart]=0; // Retained arbitrary detectors require Ordinary mode.
    // Same-bank swaps retain these inactive main-append values and locks, unlike fresh placement.
    for(std::size_t i=2063;i<2215;++i){space.values[i]=registry[i].kind==ParameterKind::continuous?.375f:1.f;space.controlLocks[i]=i%2==0;}
    CHECK(validateSoundDocument(space));
    const auto preservesInactive=[&](const SoundDocument& before,const SoundDocument& after,NativeModule active){
        for(std::size_t i=0;i<registry.size();++i)if(!registry[i].id.startsWith(prefix)||!rackParameterActive(active,registry[i].id.substring(prefix.length())))CHECK(after.values[i]==before.values[i]);
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t bank=0;bank<effectBankCount;++bank)if(slot!=0||bank!=unsigned(active)-2)CHECK(after.compatibility.effects[slot][bank]==before.compatibility.effects[slot][bank]);
        CHECK(after.macros==before.macros&&after.controlLocks==before.controlLocks&&after.effectLocks==before.effectLocks&&after.synthLocks==before.synthLocks);
    };
    const auto swaps=[&](const SoundDocument& before,const RackDesign& recipe,SoundDocument& after){
        RackOperation replace;replace.kind=RackOperationKind::swap;replace.selected=selected;replace.design=recipe;replace.destination={RackRole::fx,masterChain,0};replace.allocation={{0,0}};
        auto untouched=before;CHECK(makeRackCandidate(before,replace,SoundEditOrigin::manual,untouched,delta)==RackCandidateStatus::replacementRequired);CHECK(untouched==before);
        replace.allowSlotReuse=true;const auto status=makeRackCandidate(before,replace,SoundEditOrigin::manual,after,delta);CHECK(status==RackCandidateStatus::ready);if(status!=RackCandidateStatus::ready)return false;
        CHECK(after.layout.effects[0].kind==EffectKind(unsigned(recipe.units.front().module)-2)&&after.layout.effects[0].chain==masterChain);
        CHECK(after.personalities&&after.personalities->instances.size()==1);if(!after.personalities||after.personalities->instances.size()!=1)return false;
        const auto& instance=after.personalities->instances.front();CHECK((instance.id==selected&&instance.design==recipe&&instance.bindings==std::vector<UnitBinding>{{0,0}}));CHECK(delta.automationWarning.isNotEmpty());
        for(const auto& value:recipe.units.front().defaults){const auto i=rackParameterIndex(recipe.units.front().module,0,value.key);CHECK(i);if(i)CHECK(after.values[*i]==value.normalized);}
        preservesInactive(before,after,recipe.units.front().module);CHECK(validateSoundDocument(after));return true;
    };
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto legacy=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Lumen Drift";});CHECK(legacy!=factories.end());if(legacy==factories.end())return;
    SoundDocument other;if(!swaps(space,*legacy,other))return;
    auto replacement=*chosen;replacement.id=juce::Uuid();replacement.name="Replacement Space";replacement.revision=3;
    for(auto& value:replacement.units.front().defaults)if(value.key=="space_reverb.size"||value.key=="space_reverb.mix")value.normalized=.73f;
    SoundDocument returned;if(!swaps(other,replacement,returned))return;
    auto sameBank=*chosen;sameBank.id=juce::Uuid();sameBank.name="Second Space";sameBank.revision=7;
    for(auto& value:sameBank.units.front().defaults)if(value.key=="space_reverb.size"||value.key=="space_reverb.mix")value.normalized=.29f;
    SoundDocument same;if(!swaps(returned,sameBank,same))return;CHECK(same.values!=returned.values);
    CHECK(rackUnitParameterCapacity(NativeModule::reverb)==82); // Keep the existing legacy recipe limit.
}
}
int main() {
    frozenLibrary();frozenLegacyAudio();moduleRecipeAndReach();fullInventory();aiProposals();physicalOwnershipAndCapacity();explicitRecipeReplacement();
    std::cout<<"SPACE_RACK checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
