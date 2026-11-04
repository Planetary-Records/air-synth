#include "FrozenRackExamples.h"
#include "LegacyPersonalityProjection.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "RackReconciliation.h"
#include "UserReach.h"
#include "AiCatalog.h"
#include "CompanionWire.h"
#include "DynamicEqEdits.h"
#include "EqCurveFit.h"
#include "EqComparison.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>

namespace
{
using namespace airack;
using airack::test::legacyPersonalitySection;
using airack::test::oldEnvelopeWithPersonality;
int checks=0,failures=0;
void check(bool good,const char* expression,int line)
{++checks;if(!good){++failures;if(failures<85)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void detectorControlsAreRegisteredForTheLastBand()
{
    std::cout<<"SECTION detector_controls_append_to_last_band\n";
    const auto registry=parameterRegistry();
    for(const char* field:{"auto_threshold","adaptive_timing","detector_source","detector_low_hz","detector_high_hz"}){
        const auto id=juce::String("effect.8.dynamic_eq.band.8.")+field;
        CHECK(std::any_of(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;}));
    }
}
void detectorCatalogStateAndAdmission()
{
    std::cout<<"SECTION detector_catalog_frozen_prefix_state_domains_and_atomic_admission\n";
    CHECK(parameterCount==4519&&dynamicEqDetectorParameterStart==2215&&rackUnitParameterCapacity(NativeModule::dynamicEq)==181);
    const auto registry=parameterRegistry();
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-detection-legacy");
    const auto load=[&](const char* name){juce::MemoryBlock bytes;CHECK(directory.getChildFile(name).loadFileAsData(bytes));return bytes;};
    const auto prefix=load("registry-v21.json");CHECK(juce::SHA256(prefix).toHexString()=="61ec8fcdd1d000a53d706aa3f4f7fec8f2c0ec874eead5baf6edadd366aef402");
    const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(prefix.getData()),int(prefix.getSize())));
    CHECK(rows.isArray()&&rows.size()==2191);if(rows.size()!=2191||registry.size()<2535)return;
    for(std::size_t i=0;i<2191;++i){const auto& d=registry[i];const auto row=rows[int(i)];
        CHECK(d.id==row["id"].toString()&&d.name==row["name"].toString()&&d.unit==row["unit"].toString());
        CHECK(int(d.kind)==int(row["kind"])&&d.minimum==float(row["minimum"])&&d.maximum==float(row["maximum"])&&d.defaultValue==float(row["default"])&&d.skew==float(row["skew"])&&d.stateKey==std::uint32_t(juce::int64(row["key"])));
        CHECK(d.choices.size()==std::size_t(row["choices"].size()));for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());
    }
    const auto defaults=defaultParameterValues();SoundDocument sound;
    const char* fields[]{"auto_threshold","adaptive_timing","detector_source","detector_low_hz","detector_high_hz"};
    for(std::uint8_t slot=0;slot<8;++slot){
        CHECK(unitReach(NativeModule::dynamicEq,slot).size()==181);
        for(std::size_t band=0;band<8;++band){const auto base=2215+slot*40+band*5;
            for(std::size_t f=0;f<5;++f){const auto id="effect."+juce::String(slot+1)+".dynamic_eq.band."+juce::String(band+1)+"."+fields[f];CHECK(registry[base+f].id==id);CHECK(defaults[base+f]==(f==4?1.f:0.f));CHECK(rackParameterIndex(NativeModule::dynamicEq,slot,id.fromFirstOccurrenceOf(".",false,false).fromFirstOccurrenceOf(".",false,false))==base+f);}
            sound.values[base]=sound.values[base+1]=1;sound.values[base+2]=float((slot+band)%4)/3;
            const auto physical=[&](std::size_t i,float value){const auto& d=registry[i];return juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(value);};
            sound.values[base+3]=physical(base+3,80+float(band)*10);sound.values[base+4]=physical(base+4,800+float(band)*100);
        }
    }
    RoutedPatch mapped;CHECK(makeRoutedPatch(sound,mapped));
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto& b=mapped.effects[slot].dynamicEq.bands[band];CHECK(b.autoThreshold&&b.adaptiveTiming&&int(b.detectorSource)==int((slot+band)%4));CHECK(std::abs(b.detectorLowHz-(80+float(band)*10))<.01f&&std::abs(b.detectorHighHz-(800+float(band)*100))<.01f);}
    for(std::uint8_t slot=0;slot<8;++slot){auto before=sound;before.effectLocks[slot]=true;auto changed=before;changed.values[2215+slot*40]=0;CHECK(!validateAiSoundEdit(before,changed));before=sound;before.controlLocks[2215+slot*40+39]=true;changed=before;changed.values[2215+slot*40+39]=1;CHECK(!validateAiSoundEdit(before,changed));}
    auto invalid=sound;invalid.values[2217]=1;invalid.values[2218]=.8f;invalid.values[2219]=.2f;
    CHECK(!validateParameterValues(invalid.values)&&!validateSoundDocument(invalid));const auto before=mapped;CHECK(!makeRoutedPatch(invalid,mapped)&&mapped==before);
    auto retained=invalid;retained.values[2217]=0;CHECK(validateSoundDocument(retained));
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});CHECK(found!=factories.end());
    if(found!=factories.end()){auto definition=*found;definition.units[0].defaults={{0,"dynamic_eq.band.1.detector_source",1},{0,"dynamic_eq.band.1.detector_low_hz",.8f},{0,"dynamic_eq.band.1.detector_high_hz",.2f}};CHECK(!validateRackDesign(definition,RackValidationMode::portableDesign));}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes)&&bytes.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);
    const auto word=[](const juce::MemoryBlock& b,int at){juce::MemoryInputStream in(b,false);in.setPosition(at);return in.readInt();};
    CHECK(word(bytes,4)==31&&word(bytes,8)==4519);ProjectDocument project;project.sound=sound;CHECK(encodeProjectState(project,bytes)&&bytes.getSize()==41161);CHECK(static_cast<const unsigned char*>(bytes.getData())[4]==30&&word(bytes,24)==31);ProjectDocument roundtrip;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),roundtrip)&&roundtrip==project);
    const auto sign=[](juce::MemoryBlock& b){const auto digest=juce::SHA256(b.getData(),b.getSize()-32).getRawData();std::memcpy(static_cast<char*>(b.getData())+b.getSize()-32,digest.getData(),32);};
    for(const char* name:{"default-v21.soundstate","rich-v21.soundstate","embedded-v21.soundstate"}){
        const auto old=load(name);SoundDocument decoded;CHECK(decodeSoundState(old.getData(),old.getSize(),decoded));
        juce::MemoryInputStream in(old,false);in.setPosition(16);for(std::size_t i=0;i<2191;++i){CHECK(std::uint32_t(in.readInt())==registry[i].stateKey);CHECK(std::bit_cast<std::uint32_t>(in.readFloat())==std::bit_cast<std::uint32_t>(decoded.values[i]));}
        in.skipNextBytes(78);for(const auto& macro:decoded.macros){CHECK(macro.targetKey==std::uint32_t(in.readInt()));CHECK(std::bit_cast<std::uint32_t>(macro.amount)==std::bit_cast<std::uint32_t>(in.readFloat()));}
        for(std::size_t i=0;i<2191;++i)CHECK(decoded.controlLocks[i]==(in.readByte()!=0));for(bool lock:decoded.synthLocks)CHECK(lock==(in.readByte()!=0));for(bool lock:decoded.effectLocks)CHECK(lock==(in.readByte()!=0));
        for(std::size_t i=2191;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]&&!decoded.controlLocks[i]);
        CHECK(encodeSoundState(decoded,bytes));SoundDocument current;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),current)&&current==decoded);
        auto bad=old;const auto key=registry[2217].stateKey;std::memcpy(static_cast<char*>(bad.getData())+16,&key,4);sign(bad);auto output=sound;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),output)&&output==sound);
        bad=old;std::memcpy(static_cast<char*>(bad.getData())+16+2191*8+78,&key,4);sign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),output)&&output==sound);
    }
    const auto oldProject=load("rich-v20.airackproject"),oldSound=load("rich-v21.soundstate");SoundDocument old;CHECK(decodeSoundState(oldSound.getData(),oldSound.getSize(),old));CHECK(decodeProjectState(oldProject.getData(),oldProject.getSize(),roundtrip)&&roundtrip.sound==old);
    if(found!=factories.end()){
        auto nested=old;auto personalities=std::make_shared<RackPersonalityState>();RackInstance instance;
        instance.id=juce::Uuid("adacbed3-dfe9-4c4d-a239-7f52db74e609");instance.design=*found;instance.bindings={{0,0}};
        instance.design.units[0].defaults={{0,"dynamic_eq.band.1.auto_threshold",1}};personalities->instances.push_back(instance);nested.personalities=personalities;
        juce::MemoryBlock current;CHECK(encodeSoundState(nested,current));SoundDocument checked;CHECK(decodeSoundState(current.getData(),current.getSize(),checked)&&checked==nested);
        constexpr int oldSection=19889+4+144;
        auto control=nested;auto validPersonalities=std::make_shared<RackPersonalityState>(*nested.personalities);
        validPersonalities->instances[0].design.units[0].defaults.clear();control.personalities=validPersonalities;
        juce::MemoryBlock validWire;CHECK(oldEnvelopeWithPersonality(oldSound,oldSection,control,validWire));
        SoundDocument valid;CHECK(decodeSoundState(validWire.getData(),validWire.getSize(),valid)&&valid==control);
        juce::MemoryBlock forged;CHECK(oldEnvelopeWithPersonality(oldSound,oldSection,nested,forged));
        CHECK(!decodeSoundState(forged.getData(),forged.getSize(),checked)&&checked==nested);
    }
}
void detectorClipboardAndSnapshots()
{
    std::cout<<"SECTION detector_clipboard_four_and_old_three_neutral_migration\n";
    const auto file=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-detection-legacy/clipboard-v3.json");
    std::vector<EqBandPatch> legacy;CHECK(decodeEqClipboard(file.loadFileAsString(),legacy));CHECK(legacy.size()==1&&legacy[0].fields.size()==22);
    SoundDocument sound;CHECK(editRackLayout(sound.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::dynamicEq},sound.layout)==RackEditResult::applied);
    sound.values[2215]=sound.values[2216]=1;sound.values[2217]=1;sound.values[2218]=.1f;sound.values[2219]=.8f;
    std::vector<EqBandPatch> bands(8);for(std::uint8_t band=0;band<8;++band)CHECK(captureEqBand(sound,0,band,bands[band]));
    juce::String clipboard;CHECK(encodeEqClipboard(bands,clipboard));CHECK(juce::JSON::parse(clipboard)["version"]==juce::var(5));CHECK(clipboard.getNumBytesAsUTF8()<=16*1024);
    std::vector<EqBandPatch> decoded;CHECK(decodeEqClipboard(clipboard,decoded)&&decoded==bands);
    studio::EqComparison comparison;CHECK(comparison.store(false,bands,{}));const auto snapshot=comparison.recall(false);CHECK(snapshot&&snapshot->bands==bands);
    auto bad=bands;for(auto& field:bad[0].fields)if(field.key.endsWith("detector_high_hz"))field.normalized=0;
    juce::String preserved="retained";CHECK(!encodeEqClipboard(bad,preserved)&&preserved=="retained");CHECK(!comparison.store(false,bad,{}));CHECK(comparison.recall(false)==snapshot);
    auto forged=juce::JSON::parse(clipboard);for(auto& field:*forged["bands"][0]["fields"].getArray())if(field["key"].toString().endsWith("detector_high_hz"))field.getDynamicObject()->setProperty("normalized",0);
    const auto prior=decoded;CHECK(!decodeEqClipboard(juce::JSON::toString(forged),decoded)&&decoded==prior);
}
void detectorAiEditIsTransactional()
{
    std::cout<<"SECTION detector_AI_source_range_flags_and_lock_transaction\n";
    ProjectDocument source;CHECK(editRackLayout(source.sound.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::dynamicEq},source.sound.layout)==RackEditResult::applied);
    ai::ProposalPolicy policy;policy.scope=ai::RequestScope::sound;ai::ProposalEdits edits;
    edits.parameters={{"effect.1.dynamic_eq.band.1.auto_threshold",1},{"effect.1.dynamic_eq.band.1.adaptive_timing",1},{"effect.1.dynamic_eq.band.1.detector_source",1},{"effect.1.dynamic_eq.band.1.detector_low_hz",.2f},{"effect.1.dynamic_eq.band.1.detector_high_hz",.8f}};
    const auto before=source;ProjectDocument candidate;CHECK(ai::admitProposal(source,policy,edits,candidate)==ai::ProposalStatus::accepted&&source==before);
    CHECK(candidate.sound.values[2215]==1&&candidate.sound.values[2216]==1&&candidate.sound.values[2217]==1&&candidate.sound.values[2218]==.2f&&candidate.sound.values[2219]==.8f);
    const auto accepted=candidate;auto bad=edits;bad.parameters.back().normalized=.1f;
    CHECK(ai::admitProposal(source,policy,bad,candidate)!=ai::ProposalStatus::accepted&&candidate==accepted&&source==before);
    source.sound.controlLocks[2217]=true;CHECK(ai::admitProposal(source,policy,edits,candidate)!=ai::ProposalStatus::accepted&&candidate==accepted);
    source=before;source.sound.effectLocks[0]=true;CHECK(ai::admitProposal(source,policy,edits,candidate)!=ai::ProposalStatus::accepted&&candidate==accepted);
}
void flatSketchDoesNotInventBands()
{
    std::cout << "SECTION sketch_flat_and_cancelled\n";
    EqCurve flat;
    DynamicEqSettings base;
    const auto fit=fitEqCurve(flat,base,std::bitset<8>(255),48000);
    CHECK(fit.ready);
    CHECK(fit.rmsDb<.001f);
    CHECK(fit.evaluations<=4096);
    for(const auto& band:fit.settings.bands) CHECK(!band.active);
    std::atomic_bool cancelled{true};
    CHECK(!fitEqCurve(flat,base,std::bitset<8>(255),48000,&cancelled).ready);
}
EqCurve curveFromResponse(const DynamicEqSettings& settings,double rate)
{
    EqCurve curve;
    for(std::size_t i=0;i<curve.db.size();++i){
        const auto response=dynamicEqResponse(settings,rate,20.*std::pow(1000.,double(i)/128.));
        CHECK(response.has_value());
        if(response)curve.db[i]=float(20.*std::log10(std::max(std::abs(*response),1e-12)));
    }
    return curve;
}
double measuredFitError(const EqCurve& target,const DynamicEqSettings& settings,double rate)
{
    double squared=0;std::size_t count=0;
    for(std::size_t i=0;i<target.db.size();++i){
        const double hz=20.*std::pow(1000.,double(i)/128.);if(hz>rate*.5)continue;
        const auto response=dynamicEqResponse(settings,rate,hz);CHECK(response.has_value());
        if(response){const auto error=double(target.db[i])-20.*std::log10(std::max(std::abs(*response),1e-12));squared+=error*error;++count;}
    }
    return std::sqrt(squared/double(std::max(std::size_t(1),count)));
}
void sketchFitsBroadResponsesDeterministically()
{
    std::cout<<"SECTION sketch_broad_response_fit_and_independent_rms\n";
    for(const double rate:{44100.,48000.,96000.})for(int fixture=0;fixture<4;++fixture){
        DynamicEqSettings desired;
        auto& a=desired.bands[0];a.active=true;a.frequencyHz=1000;a.q=.7071f;a.gainDb=6;
        if(fixture==1){a.frequencyHz=180;a.gainDb=4;auto& b=desired.bands[1];b.active=true;b.frequencyHz=3000;b.gainDb=-5;b.q=1;}
        if(fixture>=2){a.shape=fixture==2?DynamicEqShape::lowShelf:DynamicEqShape::highShelf;a.frequencyHz=fixture==2?600.f:5000.f;a.gainDb=fixture==2?5.f:-6.f;}
        const auto target=curveFromResponse(desired,rate);const DynamicEqSettings base;
        const auto start=std::chrono::steady_clock::now();
        const auto fit=fitEqCurve(target,base,std::bitset<8>(255),rate);
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const auto again=fitEqCurve(target,base,std::bitset<8>(255),rate);
        CHECK(fit.ready);CHECK(fit.evaluations>0&&fit.evaluations<=4096);CHECK(fit.rmsDb<=1.f);
        CHECK(fit.settings==again.settings);CHECK(fit.rmsDb==again.rmsDb);CHECK(fit.evaluations==again.evaluations);
        CHECK(validateDynamicEq(fit.settings));CHECK(std::abs(measuredFitError(target,fit.settings,rate)-fit.rmsDb)<.001);
        for(const auto& band:fit.settings.bands)if(band.active){CHECK(band.shape<=DynamicEqShape::highShelf);CHECK(band.gainDb>=-12&&band.gainDb<=12);CHECK(band.q>=.25f&&band.q<=(band.shape==DynamicEqShape::bell?18.f:4.f));CHECK(band.rangeDb==0);}
        std::cout<<"SKETCH_FIT fixture="<<fixture<<" rate="<<rate<<" evaluations="<<fit.evaluations<<" rms="<<fit.rmsDb<<" milliseconds="<<elapsed<<'\n';
    }
}
void sketchPreservesOccupiedBandsAndGainControls()
{
    std::cout<<"SECTION sketch_occupied_raw_controls_and_gain_law\n";
    for(const bool automatic:{false,true})for(const float scale:{.55f,1.6f}){
        DynamicEqSettings base;base.autoGain=automatic;base.gainScale=scale;base.outputGainDb=-3;
        auto& occupied=base.bands[0];occupied.active=true;occupied.gainDb=-2;occupied.frequencyHz=180;occupied.q=2;occupied.rangeDb=-9;occupied.thresholdDb=-31;occupied.attackMs=8;occupied.releaseMs=140;
        base.bands[2].bypassed=true;base.bands[2].gainDb=11;base.bands[2].rangeDb=17;
        base.bands[5].rangeDb=12;base.bands[5].thresholdDb=-17;
        auto desired=base;desired.bands[5]=DynamicEqBand{};desired.bands[5].active=true;desired.bands[5].frequencyHz=2500;desired.bands[5].gainDb=6;desired.bands[5].q=.7071f;
        const auto before=base;const auto target=curveFromResponse(desired,48000);
        const auto fit=fitEqCurve(target,base,std::bitset<8>((1u<<0)|(1u<<5)),48000);
        CHECK(base==before);CHECK(fit.ready);CHECK(fit.rmsDb<1);CHECK(fit.settings.bands[5].active);CHECK(fit.settings.bands[5].rangeDb==0);CHECK(fit.settings.bands[5].thresholdDb==DynamicEqBand{}.thresholdDb);
        for(std::size_t i=0;i<8;++i)if(i!=5)CHECK(fit.settings.bands[i]==base.bands[i]);
        CHECK(fit.settings.autoGain==base.autoGain);CHECK(fit.settings.gainScale==base.gainScale);CHECK(fit.settings.outputGainDb==base.outputGainDb);
        CHECK(std::abs(measuredFitError(target,fit.settings,48000)-fit.rmsDb)<.001);
    }
}
void sketchRefusesInvalidInputAndReportsResidual()
{
    std::cout<<"SECTION sketch_capacity_invalid_cancel_and_nonrepresentable\n";
    EqCurve target;for(std::size_t i=0;i<target.db.size();++i)target.db[i]=(i%2)?12.f:-12.f;
    DynamicEqSettings base;const auto before=base;
    const auto sharp=fitEqCurve(target,base,std::bitset<8>(255),48000);
    CHECK(sharp.ready);CHECK(std::isfinite(sharp.rmsDb)&&sharp.rmsDb>1);CHECK(sharp.evaluations<=4096);CHECK(base==before);
    CHECK(std::abs(measuredFitError(target,sharp.settings,48000)-sharp.rmsDb)<.001);
    const auto noRoom=fitEqCurve(target,base,{},48000);CHECK(!noRoom.ready);CHECK(noRoom.settings==base);CHECK(std::isfinite(noRoom.rmsDb)&&noRoom.rmsDb>1);
    auto full=base;for(auto& band:full.bands)band.active=true;
    const auto occupied=fitEqCurve(target,full,std::bitset<8>(255),48000);CHECK(!occupied.ready);CHECK(occupied.settings==full);
    for(const double rate:{0.,7999.,1536001.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})CHECK(!fitEqCurve(target,base,std::bitset<8>(255),rate).ready);
    for(const float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
        auto broken=target;broken.db[128]=invalid;CHECK(!fitEqCurve(broken,base,std::bitset<8>(255),48000).ready);CHECK(!fitEqCurve(broken,base,std::bitset<8>(255),8000).ready);
    }
    auto invalid=base;invalid.bands[7].q=0;CHECK(!fitEqCurve(target,invalid,std::bitset<8>(255),48000).ready);
    auto zeroScale=base;zeroScale.gainScale=0;CHECK(!fitEqCurve(target,zeroScale,std::bitset<8>(255),48000).ready);CHECK(fitEqCurve(EqCurve{},zeroScale,{},48000).ready);
    const auto lowRate=fitEqCurve(target,base,std::bitset<8>(255),8000);CHECK(lowRate.ready);CHECK(std::isfinite(lowRate.rmsDb));CHECK(lowRate.evaluations<=4096);
    for(const auto& band:lowRate.settings.bands)if(band.active)CHECK(band.frequencyHz<=3600.f);
    std::atomic_bool cancelled{true};const auto stopped=fitEqCurve(target,base,std::bitset<8>(255),48000,&cancelled);CHECK(!stopped.ready);CHECK(stopped.settings==base);CHECK(stopped.evaluations==0);
}
void sketchChecks(){flatSketchDoesNotInventBands();sketchFitsBroadResponsesDeterministically();sketchPreservesOccupiedBandsAndGainControls();sketchRefusesInvalidInputAndReportsResidual();}
bool captureComparisonBaseline(const juce::File& directory,bool placement=false,bool shapes=false)
{
    if ((shapes&&!placement) || parameterCount != (shapes?2023:placement?1959:1943) || directory.exists() || !directory.createDirectory().wasOk()) return false;
    const auto version=juce::String(shapes?19:placement?18:17);
    const auto write = [&](const char* name, const juce::MemoryBlock& bytes) {
        const auto file = directory.getChildFile(name);
        if (file.exists() || !file.replaceWithData(bytes.getData(), bytes.getSize())) return false;
        std::cout << "COMPARISON_BASELINE " << name << " bytes=" << bytes.getSize() << " sha256=" << juce::SHA256(bytes).toHexString() << '\n';
        return true;
    };
    juce::Array<juce::var> rows;
    for (const auto& d : parameterRegistry()) {
        auto* row = new juce::DynamicObject;
        row->setProperty("id", d.id); row->setProperty("name", d.name); row->setProperty("unit", d.unit);
        row->setProperty("kind", int(d.kind)); row->setProperty("minimum", double(d.minimum)); row->setProperty("maximum", double(d.maximum));
        row->setProperty("default", double(d.defaultValue)); row->setProperty("skew", double(d.skew)); row->setProperty("key", juce::int64(d.stateKey));
        juce::Array<juce::var> choices; for (const auto& choice : d.choices) choices.add(choice); row->setProperty("choices", choices);
        rows.add(juce::var(row));
    }
    const auto json = juce::JSON::toString(rows, true, 17);
    if (!write(("registry-v"+version+".json").toRawUTF8(), juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()))) return false;
    SoundDocument empty, rich;
    for (std::uint8_t slot : {std::uint8_t(0), std::uint8_t(7)}) {
        RackEdit edit; edit.kind=RackEditKind::addEffect; edit.slot=slot; edit.effect=EffectKind::dynamicEq;
        if (editRackLayout(rich.layout, edit, rich.layout) != RackEditResult::applied) return false;
        const auto base=dynamicEqParameterStart+slot*dynamicEqParametersPerSlot;
        for (std::size_t band=0; band<8; ++band) {
            rich.values[base+band*11]=1; rich.values[base+band*11+2]=float(band%6)/5;
            rich.values[base+band*11+3]=.1f+.09f*float(band); rich.values[base+band*11+4]=.2f+.08f*float(band);
            rich.values[base+band*11+7]=.3f+.05f*float(band);
            if(shapes){rich.values[base+band*11+6]=float(band%2);rich.values[dynamicEqPlacementParameterStart+slot*8+band]=float(band%5)/4;}
        }
    }
    rich.values[0]=.3f; rich.values[filterParameterStart+3]=.61f;
    if(placement){rich.values[dynamicEqGainParameterStart]=1;rich.values[dynamicEqGainParameterStart+1]=.68f;rich.values[dynamicEqGainParameterStart+14]=1;rich.values[dynamicEqGainParameterStart+15]=.31f;rich.controlLocks[dynamicEqGainParameterStart+1]=true;}
    rich.macros[0]={parameterRegistry()[dynamicEqParameterStart+3].stateKey,.21f};
    rich.controlLocks[dynamicEqParameterStart+4]=true; rich.effectLocks[7]=true;
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto factory=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    if (factory==factories.end()) return false;
    RackOperation place; place.kind=RackOperationKind::place; place.design=*factory;
    place.newInstance=juce::Uuid("72b0ea6d-4c2d-4d93-a3d8-7a904b0500ab"); place.destination={RackRole::fx,masterChain,0}; place.allocation={{0,0}};
    SoundDocument embedded; RackDifference delta;
    if (makeRackCandidate(empty,place,SoundEditOrigin::manual,embedded,delta)!=RackCandidateStatus::ready) return false;
    juce::MemoryBlock bytes;
    for (const auto& [name, sound] : std::array<std::pair<const char*,const SoundDocument*>,3> {{{"default-v17.soundstate",&empty},{"rich-v17.soundstate",&rich},{"embedded-v17.soundstate",&embedded}}})
        if (!encodeSoundState(*sound,bytes) || !write(juce::String(name).replace("v17","v"+version).toRawUTF8(),bytes)) return false;
    ProjectDocument project; project.sound=rich;
    if (!encodeProjectState(project,bytes) || !write(("rich-v"+juce::String(shapes?18:placement?17:16)+".airackproject").toRawUTF8(),bytes)) return false;
    if(placement){
        std::vector<EqBandPatch> bands;for(std::uint8_t band=0;band<8;++band){EqBandPatch captured;if(!captureEqBand(rich,0,band,captured))return false;bands.push_back(std::move(captured));}
        juce::String clipboard;if(!encodeEqClipboard(bands,clipboard)||!write(shapes?"eight-bands-v2.eqclipboard.json":"eight-bands-v1.eqclipboard.json",juce::MemoryBlock(clipboard.toRawUTF8(),clipboard.getNumBytesAsUTF8())))return false;
    }
    return encodeRackDesign(*factory,bytes) && write("contour-v1.rackdesign",bytes);
}
void comparisonSnapshotIsIndependent()
{
    std::cout << "SECTION comparison_raw_owned_snapshot_and_atomic_refusal\n";
    studio::EqComparison ab;
    CHECK(!ab.recall(false)); CHECK(!ab.recall(true));
    std::vector<EqBandPatch> bands {{0, {{"dynamic_eq.band.1.gain_db", .75f}}}};
    std::vector<EqField> fields {{"dynamic_eq.output_gain_db", .5f}};
    const EqPatch stored {bands, fields};
    CHECK(ab.store(false, bands, fields));
    bands[0].fields[0].normalized = .2f; fields[0].normalized = .9f;
    auto recalled = ab.recall(false); CHECK(recalled && *recalled==stored);
    if (recalled) recalled->bands[0].fields[0].normalized = .1f;
    CHECK(ab.recall(false)==std::optional<EqPatch>(stored));
    for (int bad=0; bad<9; ++bad) {
        auto invalid=stored;
        if(bad==0)invalid.bands[0].band=8;
        if(bad==1)invalid.bands[0].fields[0].normalized=std::numeric_limits<float>::quiet_NaN();
        if(bad==2)invalid.bands[0].fields[0].normalized=1.01f;
        if(bad==3)invalid.bands.push_back(invalid.bands[0]);
        if(bad==4)invalid.bands[0].fields[0].key="dynamic_eq.band.8.gain_db";
        if(bad==5)invalid.slotFields[0].key="delay.seconds";
        if(bad==6)invalid.bands[0].fields.clear();
        if(bad==7)invalid.slotFields[0].key=juce::String::repeatedString("a",97);
        if(bad==8)invalid.bands[0].fields.resize(93,invalid.bands[0].fields[0]);
        CHECK(!ab.store(false,invalid.bands,invalid.slotFields));
        CHECK(ab.recall(false)==std::optional<EqPatch>(stored));
    }
    CHECK(!ab.store(true,{},{})); CHECK(!ab.recall(true));
    CHECK(ab.store(true,bands,fields)); CHECK(ab.recall(true)==std::optional<EqPatch>(EqPatch{bands,fields}));
    std::vector<EqField> extension {{"dynamic_eq.auto_gain",1},{"dynamic_eq.gain_scale",.75f}};
    CHECK(ab.store(true,{},extension));
    const auto extended=ab.recall(true); CHECK(extended && extended->slotFields==extension);
    ab.clear(); CHECK(!ab.recall(false)); CHECK(!ab.recall(true));
}
void gainControlCatalogAndFrozenPrefix()
{
    std::cout << "SECTION appended_gain_controls_and_frozen_1943_descriptors\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-comparison-legacy");
    juce::MemoryBlock bytes; CHECK(directory.getChildFile("registry-v17.json").loadFileAsData(bytes));
    CHECK(juce::SHA256(bytes).toHexString()=="b20522b492d30ae54daf2bf8e633d8d14cea7c5f3d6d3be2a5236441a41ebb37");
    const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())));
    CHECK(rows.isArray()&&rows.size()==1943);
    const auto registry=parameterRegistry(); CHECK(registry.size()==4519);
    if(rows.size()==1943&&registry.size()>=1943)for(int i=0;i<1943;++i) {
        const auto& d=registry[std::size_t(i)]; const auto row=rows[i];
        CHECK(d.id==row["id"].toString()); CHECK(d.name==row["name"].toString()); CHECK(d.unit==row["unit"].toString());
        CHECK(int(d.kind)==int(row["kind"])); CHECK(d.minimum==float(row["minimum"])); CHECK(d.maximum==float(row["maximum"]));
        CHECK(d.defaultValue==float(row["default"])); CHECK(d.skew==float(row["skew"])); CHECK(d.stateKey==std::uint32_t(juce::int64(row["key"])));
        CHECK(d.choices.size()==std::size_t(row["choices"].size())); for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());
    }
    for(std::uint8_t slot=0;slot<8;++slot) {
        CHECK(unitReach(NativeModule::dynamicEq,slot).size()==181);
        const auto automatic=rackParameterIndex(NativeModule::dynamicEq,slot,"dynamic_eq.auto_gain");
        const auto scale=rackParameterIndex(NativeModule::dynamicEq,slot,"dynamic_eq.gain_scale");
        CHECK(automatic.has_value()); CHECK(scale.has_value()); if(!automatic||!scale)continue;
        CHECK(*automatic==1943+slot*2&&*scale==1944+slot*2);
        CHECK(registry[*automatic].kind==ParameterKind::toggle&&registry[*automatic].choices==std::vector<juce::String>({"Off","On"}));
        CHECK(registry[*scale].minimum==0&&registry[*scale].maximum==2&&registry[*scale].defaultValue==1&&registry[*scale].skew==1);
        SoundDocument sound; RoutedPatch patch;
        CHECK(makeRoutedPatch(sound,patch)); CHECK(!patch.effects[slot].dynamicEq.autoGain&&patch.effects[slot].dynamicEq.gainScale==1);
        sound.values[*automatic]=1; sound.values[*scale]=.75f;
        CHECK(makeRoutedPatch(sound,patch)); CHECK(patch.effects[slot].dynamicEq.autoGain&&patch.effects[slot].dynamicEq.gainScale==1.5f);
        EqPatch edits; edits.slotFields={{"dynamic_eq.auto_gain",1},{"dynamic_eq.gain_scale",.75f}};
        RackEdit add; add.kind=RackEditKind::addEffect; add.slot=slot; add.effect=EffectKind::dynamicEq;
        CHECK(editRackLayout(sound.layout,add,sound.layout)==RackEditResult::applied);
        SoundDocument changed; CHECK(makeEqEdit(sound,slot,edits,changed));
        sound.effectLocks[slot]=true; const auto before=sound;
        CHECK(!makeEqEdit(sound,slot,edits,changed)); CHECK(sound==before);
        for(const auto control:{*automatic,*scale}) {
            auto candidate=sound;candidate.values[control]=control==*automatic?0.f:.5f;
            CHECK(!validateAiSoundEdit(sound,candidate));
            SoundDocument reconciled;RackDifference delta;
            CHECK(reconcileRackEdit(sound,candidate,{},SoundEditOrigin::aiCandidate,reconciled,delta)!=RackCandidateStatus::ready);
            auto individual=sound;individual.effectLocks[slot]=false;individual.controlLocks[control]=true;
            candidate=individual;candidate.values[control]=control==*automatic?0.f:.5f;
            CHECK(!validateAiSoundEdit(individual,candidate));
            individual.controlLocks[control]=false;candidate=individual;candidate.values[control]=control==*automatic?0.f:.25f;
            CHECK(validateAiSoundEdit(individual,candidate));
        }
        for(float value:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
            auto invalid=sound;invalid.effectLocks[slot]=false;invalid.values[*scale]=value;
            const auto original=patch;CHECK(!makeRoutedPatch(invalid,patch));CHECK(patch==original);
            juce::MemoryBlock wire;CHECK(!encodeSoundState(invalid,wire));
        }
    }
    bytes.reset(); CHECK(directory.getChildFile("contour-v1.rackdesign").loadFileAsData(bytes));
    CHECK(juce::SHA256(bytes).toHexString()=="629c009553850112c958a236a0facedd3d99b6eaa5ae5f47e4caa9c2569a0b6e");
    const auto factories=airack::test::frozenFactoryRackDesigns(); const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    CHECK(found!=factories.end()); if(found!=factories.end()){juce::MemoryBlock current;CHECK(encodeRackDesign(*found,current)&&current==bytes);}
}
void gainStateAndNativeOldMigration()
{
    std::cout << "SECTION gain_current_state_and_actual_sound17_project16_migration\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-comparison-legacy");
    const std::array<std::pair<const char*,const char*>,3> sounds {{{"default-v17.soundstate","b61fd861fda0200bed01393258975e4c3fd64a5d1ec6d9f8d40117168981cf1d"},
        {"rich-v17.soundstate","5480313163ce5464ed107bb33443f0f8329e3820f110e8756f5792e8a1757dae"},
        {"embedded-v17.soundstate","723e3428c74ef2fcc41138c7ecd2b620e7abf9d1e58eda747b99d0eb6c8adfd9"}}};
    for(const auto& [name,hash]:sounds) {
        juce::MemoryBlock bytes; CHECK(directory.getChildFile(name).loadFileAsData(bytes)); CHECK(juce::SHA256(bytes).toHexString()==hash);
        SoundDocument decoded; const bool read=decodeSoundState(bytes.getData(),bytes.getSize(),decoded); CHECK(read); if(!read)continue;
        juce::MemoryInputStream stream(bytes,false); stream.setPosition(16);
        for(int record=0;record<1943;++record) {const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();const auto index=parameterIndexForKey(key);CHECK(index&&decoded.values[*index]==value);}
        RoutedPatch patch;CHECK(makeRoutedPatch(decoded,patch)); for(const auto& effect:patch.effects)CHECK(!effect.dynamicEq.autoGain&&effect.dynamicEq.gainScale==1);
        if(juce::String(name).startsWith("rich")){CHECK(decoded.effectLocks[7]&&decoded.controlLocks[dynamicEqParameterStart+4]);CHECK(decoded.macros[0].targetKey==parameterRegistry()[dynamicEqParameterStart+3].stateKey&&decoded.macros[0].amount==.21f);}
        if(juce::String(name).startsWith("embedded"))CHECK(decoded.personalities&&decoded.personalities->instances.size()==1&&decoded.personalities->instances[0].design.units[0].defaults.size()==90);
        const auto reject=[&](juce::MemoryBlock malformed) {
            const auto digest=juce::SHA256(malformed.getData(),malformed.getSize()-32).getRawData();
            std::memcpy(static_cast<char*>(malformed.getData())+malformed.getSize()-32,digest.getData(),32);
            auto output=decoded;CHECK(!decodeSoundState(malformed.getData(),malformed.getSize(),output));CHECK(output==decoded);
        };
        // A newly registered key remains outside the signed Sound17 domain,
        // including a neutral nested default or a continuous macro target.
        const auto future=rackParameterIndex(NativeModule::dynamicEq,0,"dynamic_eq.gain_scale");CHECK(future.has_value());
        if(future) {
            const auto key=parameterRegistry()[*future].stateKey;
            auto malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16,&key,4);reject(malformed);
            malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16+1943*8+78,&key,4);reject(malformed);
            if(decoded.personalities) {
                auto candidate=decoded;auto personalities=std::make_shared<RackPersonalityState>(*decoded.personalities);
                personalities->instances[0].design.units[0].defaults.push_back({0,"dynamic_eq.gain_scale",.5f});candidate.personalities=personalities;
                juce::MemoryBlock modern;const bool encoded=encodeSoundState(candidate,modern);CHECK(encoded);
                if(encoded) {
                    juce::MemoryBlock control;CHECK(oldEnvelopeWithPersonality(bytes,oldSound17NumericBytes+4+oldSound17CompatibilityBytes,decoded,control));
                    CHECK(control==bytes);SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==decoded);
                    juce::MemoryBlock nested;CHECK(oldEnvelopeWithPersonality(bytes,oldSound17NumericBytes+4+oldSound17CompatibilityBytes,candidate,nested));reject(nested);
                }
            }
        }
        juce::MemoryBlock upgraded; CHECK(encodeSoundState(decoded,upgraded));
        juce::MemoryInputStream header(upgraded,false);header.setPosition(4); CHECK(header.readInt()==31&&header.readInt()==4519);
        SoundDocument again;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),again)&&again==decoded);
    }
    juce::MemoryBlock oldProject;CHECK(directory.getChildFile("rich-v16.airackproject").loadFileAsData(oldProject));
    CHECK(juce::SHA256(oldProject).toHexString()=="83ffbec86f70437d6153848d0b0ff464149410522da6ff4e38e4ff67b5905435");
    ProjectDocument project;CHECK(decodeProjectState(oldProject.getData(),oldProject.getSize(),project));
    for(std::uint8_t slot=0;slot<8;++slot) {
        const auto automatic=rackParameterIndex(NativeModule::dynamicEq,slot,"dynamic_eq.auto_gain"),scale=rackParameterIndex(NativeModule::dynamicEq,slot,"dynamic_eq.gain_scale");
        if(!automatic||!scale)continue;
        project.sound.values[*automatic]=1;project.sound.values[*scale]=.75f;
    }
    juce::MemoryBlock current;CHECK(encodeProjectState(project,current));juce::MemoryInputStream header(current,false);header.setPosition(4);CHECK(header.readShort()==30);header.setPosition(24);CHECK(header.readInt()==31&&header.readInt()==4519);
    ProjectDocument again;CHECK(decodeProjectState(current.getData(),current.getSize(),again)&&again==project);
}
bool captureOld(const juce::File& file)
{
    const auto designs=airack::test::frozenLibraryRackDesigns();
    if(file.exists()||designs.size()!=20)return false;
    juce::MemoryBlock bytes;juce::MemoryOutputStream stream(bytes,false);stream.writeInt(0x314c5241);stream.writeInt(20);
    for(const auto& design:designs){juce::MemoryBlock encoded;if(!encodeRackDesign(design,encoded))return false;
        stream.writeInt(int(encoded.getSize()));stream.write(encoded.getData(),encoded.getSize());}
    const auto digest=juce::SHA256(stream.getData(),stream.getDataSize()).getRawData();stream.write(digest.getData(),digest.getSize());stream.flush();
    if(!file.replaceWithData(bytes.getData(),bytes.getSize()))return false;
    std::cout<<"OLD_LIBRARY count=20 bytes="<<bytes.getSize()<<" sha256="<<juce::SHA256(bytes).toHexString()<<'\n';return true;
}
void originalLibrary()
{
    std::cout<<"SECTION original_twenty_library_definitions\n";
    juce::MemoryBlock bytes;CHECK(juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getChildFile("old-library.racklist").loadFileAsData(bytes));
    if(bytes.getSize()<40)return;
    const auto digest=juce::SHA256(bytes.getData(),bytes.getSize()-32).getRawData();CHECK(std::memcmp(digest.getData(),static_cast<const char*>(bytes.getData())+bytes.getSize()-32,32)==0);
    juce::MemoryInputStream input(bytes,false);CHECK(input.readInt()==0x314c5241);CHECK(input.readInt()==20);
    const auto current=airack::test::frozenLibraryRackDesigns();CHECK(current.size()>=20);
    if(current.size()<20)return;
    for(std::size_t i=0;i<20;++i){const auto size=input.readInt();CHECK(size>0&&size<=65536);
        if(size<1||size>65536)return;juce::MemoryBlock expected;input.readIntoMemoryBlock(expected,size);
        RackDesign old;CHECK(decodeRackDesign(expected.getData(),expected.getSize(),old));
        const auto found=std::find_if(current.begin(),current.end(),[&](const auto& d){return d.id==old.id;});CHECK(found!=current.end());if(found==current.end())continue;
        juce::MemoryBlock actual;CHECK(encodeRackDesign(*found,actual));CHECK(actual==expected);
    }
    CHECK(input.getPosition()==juce::int64(bytes.getSize()-32));
}
void moduleAndFactory()
{
    std::cout<<"SECTION dynamic_module_factory_and_owned_fields\n";
    const auto module=NativeModule(17);
    CHECK(moduleDisplayName(module)=="Dynamic EQ");CHECK(effectDisplayName(EffectKind(15))=="Dynamic EQ");
    const auto supported=supportsAlgorithm(module,1);CHECK(supported);CHECK(!supportsAlgorithm(module,2));
    CHECK(supportsAlgorithm(NativeModule(20),1));
    CHECK(!supportsAlgorithm(NativeModule(22),1));
    const auto factories=airack::test::frozenFactoryRackDesigns();CHECK(factories.size()==21);CHECK(airack::test::frozenLibraryRackDesigns().size()==28);
    const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& design){return design.name=="Contour Dynamics";});
    CHECK(found!=factories.end());
    // Keep the availability RED about assertions, without calling an old
    // token table using a module that it has not admitted yet.
    if(!supported||found==factories.end())return;
    const auto& design=*found;CHECK(design.id==juce::Uuid("72b0ea6d-4c2d-4d93-a3d8-7a904b050001"));
    CHECK(design.role==RackRole::fx&&design.units.size()==1&&design.revision==1);
    CHECK(validateRackDesign(design,RackValidationMode::portableDesign));
    juce::MemoryBlock designWire;RackDesign loaded;
    CHECK(encodeRackDesign(design,designWire)&&decodeRackDesign(designWire.getData(),designWire.getSize(),loaded)&&loaded==design);
    CHECK(design.panel.layout==PanelLayout::orderedFxStrip&&design.panel.controls.size()==8);
    const auto schema=rackDesignSchema();const auto choices=schema["properties"]["units"]["items"]["anyOf"];
    CHECK(choices.isArray()&&choices.size()==22);
    if(choices.isArray()&&choices.size()==22){
        CHECK(int(choices[17]["properties"]["defaults"]["maxItems"])==181);
        CHECK(int(choices[2]["properties"]["defaults"]["maxItems"])==140);
    }
    const auto& unit=design.units[0];CHECK(unit.module==module&&unit.algorithmRevision==1&&unit.defaults.size()==90);
    const auto reach=unitReach(module,0);CHECK(reach.size()==181);std::set<juce::String> keys;
    for(const auto& value:unit.defaults){CHECK(keys.insert(value.key).second);CHECK(rackParameterActive(module,value.key));CHECK(rackParameterIndex(module,0,value.key).has_value());}
    CHECK(keys.contains("bypassed"));CHECK(keys.contains("dynamic_eq.band.1.active"));CHECK(keys.contains("dynamic_eq.band.8.release_ms"));CHECK(keys.contains("dynamic_eq.output_gain_db"));
    CHECK(!rackParameterActive(module,"eq.bell.gain_db"));
    for(std::uint8_t slot=0;slot<8;++slot)for(std::uint8_t chain:{std::uint8_t(0),masterChain}){
        SoundDocument sound;RackOperation place;place.kind=RackOperationKind::place;place.design=design;
        place.newInstance=juce::Uuid();place.destination={RackRole::fx,chain,0};place.allocation={{0,slot}};
        SoundDocument next;RackDifference delta;CHECK(makeRackCandidate(sound,place,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);
        CHECK(next.layout.effects[slot].kind==EffectKind(15));CHECK(next.personalities&&next.personalities->instances.size()==1);
        RoutedPatch patch;CHECK(makeRoutedPatch(next,patch));CHECK(!patch.effects[slot].dynamicEq.bands[0].active);
        CHECK(std::none_of(patch.effects[slot].dynamicEq.bands.begin(),patch.effects[slot].dynamicEq.bands.end(),[](const auto& band){return band.active;}));
        CHECK(patch.effects[slot].dynamicEq.bands[0].gainDb==0&&patch.effects[slot].dynamicEq.bands[0].rangeDb==0);
        for(std::size_t band=1;band<8;++band)CHECK(!patch.effects[slot].dynamicEq.bands[band].active);
        juce::MemoryBlock encoded;CHECK(encodeSoundState(next,encoded));SoundDocument recalled;CHECK(decodeSoundState(encoded.getData(),encoded.getSize(),recalled)&&recalled==next);
        const auto mask=userReach(next);for(const auto i:unitReach(module,slot))CHECK(mask[i]);
    }
}
void fullInventory(bool current=false,bool retainedRevision1=false)
{
    std::cout<<"SECTION full_factory_embedded_and_registered_inventory\n";
    auto factory=airack::test::frozenLibraryRackDesigns();const auto historicalTile=airack::test::frozenTileArrayRackDesign();
    CHECK(factory.size()==28&&std::count(factory.begin(),factory.end(),historicalTile)==1);
    if(!current){const auto examples=airack::test::frozenExampleRackDesigns();factory.assign(examples.begin(),examples.begin()+4);
        for(const auto& design:airack::test::frozenFactoryRackDesigns())if(design.units[0].module!=NativeModule::spaceReverb)factory.push_back(design);}
    const bool originalSaturation=!current||retainedRevision1;
    if(originalSaturation){
        const auto fixtures=juce::File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("fixtures/saturation-release-20261026");
        for(auto& design:factory)if(design.units[0].module==NativeModule::saturation){juce::MemoryBlock bytes;
            CHECK(fixtures.getChildFile(design.name=="Character Drive"?"character-drive-revision-1.rackdesign":"band-color-revision-1.rackdesign").loadFileAsData(bytes));
            RackDesign old;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),old)&&old.id==design.id&&old.revision==1);design=std::move(old);}
    }
    CHECK(factory.size()==std::size_t(current?28:24));
    const auto found=std::find_if(factory.begin(),factory.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    if(found==factory.end())return;
    std::vector<RackDesign> embedded;
    const auto synth=std::find_if(factory.begin(),factory.end(),[](const auto& d){return d.role==RackRole::synth&&d.units.size()==1;});CHECK(synth!=factory.end());
    if(synth==factory.end())return;
    for(int i=0;i<12;++i){auto design=i<4?*synth:*found;design.id=juce::Uuid();design.name="Embedded "+juce::String(i+1);embedded.push_back(std::move(design));}
    auto inventory=embedded;
    for(int i=0;i<32;++i){auto design=*found;design.id=juce::Uuid();design.name="Extra EQ "+juce::String(i+1);inventory.push_back(std::move(design));}
    RackCatalogSnapshot snapshot;RackCatalogStatus status;
    const auto captured=captureRackCatalog(factory,inventory,{},snapshot,&status);
    std::cout<<"INVENTORY captured="<<captured<<" status="<<int(status)<<" entries="<<snapshot.entries.size()<<'\n';
    CHECK(captured);if(!captured)return;CHECK(factory.size()==std::size_t(current?28:24)&&snapshot.entries.size()==std::size_t(current?72:68));
    const std::size_t sourceCount=current?72:68;
    CHECK(maximumRackCatalogEntries==72&&maximumRackCatalogBytes==4194304&&RackCatalogCaptureLimits{}.maximumPreparedBytes==41943040);
    const auto preserved=snapshot;
    auto refuses=[&](RackCatalogCaptureLimits limits,RackCatalogStatus expected){RackCatalogSnapshot output=preserved;
        CHECK(!captureRackCatalog(factory,inventory,limits,output,&status));
        CHECK(status==expected&&output.id==preserved.id&&output.entries==preserved.entries);};
    auto boundary=RackCatalogCaptureLimits{};boundary.maximumEntries=sourceCount;boundary.maximumSources=sourceCount;
    // Reviewed no-sharing upper shape: 68 entries, original 4 MiB descriptors and fixed scratch/resolved copies.
    // At72, retain the worst partial71 reservation plus the full source metadata; factory credits do not relax externals.
    const std::size_t preparedExact=current?41898036:41249919;
    CHECK(41898036==20971520+205+2048+2928+134368+131074+262144+12*71774+3*71*72001+4194304+72*27);
    boundary.maximumPreparedBytes=preparedExact;RackCatalogSnapshot exact;
    CHECK(captureRackCatalog(factory,inventory,boundary,exact,&status)&&exact.entries==snapshot.entries);
    --boundary.maximumPreparedBytes;refuses(boundary,RackCatalogStatus::preparedLimit);
    boundary={};boundary.maximumSources=sourceCount-1;refuses(boundary,RackCatalogStatus::countLimit);
    boundary={};boundary.maximumEntries=sourceCount-1;refuses(boundary,RackCatalogStatus::countLimit);
    std::size_t definitionBytes=0,descriptorBytes=0;
    for(const auto& entry:snapshot.entries){juce::MemoryBlock definition;CHECK(encodeRackDesign(entry.definition,definition,RackValidationMode::instanceRecall));
        definitionBytes+=definition.getSize();std::size_t bytes=0;CHECK(rackCatalogEscapedSize(rackCatalogDescriptor(entry),65536,bytes));descriptorBytes+=bytes;}
    std::cout<<"INVENTORY_BYTES definitions="<<definitionBytes<<" descriptors_sum="<<descriptorBytes<<'\n';
    boundary={};boundary.maximumDefinitionBytes=definitionBytes;boundary.maximumSourceBytes=definitionBytes;boundary.maximumDescriptorBytes=descriptorBytes;
    CHECK(captureRackCatalog(factory,inventory,boundary,exact,&status)&&exact.entries==snapshot.entries);
    auto shortBoundary=boundary;--shortBoundary.maximumDefinitionBytes;refuses(shortBoundary,RackCatalogStatus::byteLimit);
    shortBoundary=boundary;--shortBoundary.maximumSourceBytes;refuses(shortBoundary,RackCatalogStatus::byteLimit);
    shortBoundary=boundary;--shortBoundary.maximumDescriptorBytes;refuses(shortBoundary,RackCatalogStatus::byteLimit);
    std::cout<<"CATALOG_FENCES sources="<<sourceCount<<" saturation_design_revision="<<(originalSaturation?1:2)<<" prepared_exact="<<preparedExact<<" prepared_cap="<<RackCatalogCaptureLimits{}.maximumPreparedBytes
        <<" definitions_exact="<<definitionBytes<<" descriptors_exact="<<descriptorBytes<<" minus_one_refusals=6\n";
    if(current){
        for(unsigned field=0;field<6;++field){auto invalid=RackCatalogCaptureLimits{};
            if(field==0)invalid.maximumEntries=std::numeric_limits<std::size_t>::max();
            if(field==1)invalid.maximumSources=std::numeric_limits<std::size_t>::max();
            if(field==2)invalid.maximumDefinitionBytes=std::numeric_limits<std::size_t>::max();
            if(field==3)invalid.maximumSourceBytes=std::numeric_limits<std::size_t>::max();
            if(field==4)invalid.maximumDescriptorBytes=std::numeric_limits<std::size_t>::max();
            if(field==5)invalid.maximumPreparedBytes=std::numeric_limits<std::size_t>::max();
            refuses(invalid,field<2?RackCatalogStatus::countLimit:field<5?RackCatalogStatus::byteLimit:RackCatalogStatus::preparedLimit);}
        auto low=RackCatalogCaptureLimits{};low.maximumPreparedBytes=0;refuses(low,RackCatalogStatus::preparedLimit);
        auto extra=inventory;auto additional=extra.back();additional.id=juce::Uuid();extra.push_back(additional);auto untouched=preserved;
        CHECK(!captureRackCatalog(factory,extra,{},untouched,&status)&&status==RackCatalogStatus::countLimit&&untouched.id==preserved.id&&untouched.entries==preserved.entries);
        auto oversized=factory;auto& caller=oversized[0];caller.name.preallocateBytes(1024*1024);caller.intent.preallocateBytes(1024*1024);
        caller.units.reserve(1024);caller.panel.sections.reserve(1024);caller.panel.controls.reserve(1024);caller.units[0].defaults.reserve(4096);
        const auto capacity=caller.units[0].defaults.capacity();auto normalized=preserved;
        CHECK(captureRackCatalog(oversized,inventory,boundary,normalized,&status)&&normalized.id==preserved.id&&normalized.entries==preserved.entries);
        CHECK(caller.units[0].defaults.capacity()==capacity&&normalized.entries[0].definition.units[0].defaults.capacity()<capacity);
        juce::var owned;CHECK(ai::catalogInventoryFor(snapshot,ai::CatalogRendering::ownedLocal,owned));CHECK(owned["entries"].size()==72);
    }
    juce::var descriptor;CHECK(ai::catalogInventoryFor(snapshot,descriptor));
    CHECK(descriptor["entries"].size()==int(current?71:68));
    auto authority=std::make_shared<ai::CatalogAuthority>();authority->root={juce::Uuid(),1,0,0};authority->snapshot=snapshot.id;
    authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
    CHECK(ai::detail::catalogAuthorityValid(*authority));
    ai::GenerationRequest request;request.tag=authority->root;request.model="offline-test";request.instruction="Use the complete captured library";
    ai::CatalogRequestContext context;context.root=request.tag;context.authority=authority;request.catalog=context;
    ai::GenerationRequest recalled;juce::MemoryBlock requestWire;CHECK(companion::encodeRequest(request,requestWire));
    CHECK(companion::decodeRequest({static_cast<const std::byte*>(requestWire.getData()),requestWire.getSize()},recalled));
    CHECK(recalled.catalog&&recalled.catalog->authority->descriptorJson==authority->descriptorJson);
    request.source.sound.layout.synthUse.fill(SlotUse::neverUsed);request.source.sound.layout.synthOrder.fill(noSlot);request.source.sound.layout.synthCount=0;
    for(std::size_t i=0;i<embedded.size();++i){
        const auto synthUnit=i<4;RackOperation operation;
        operation.design=embedded[i];operation.newInstance=juce::Uuid();
        operation.allocation={{embedded[i].units[0].id,std::uint8_t(synthUnit?i:i-4)}};
        operation.destination={synthUnit?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(synthUnit?i:i-4)};
        SoundDocument next;RackDifference delta;CHECK(makeRackCandidate(request.source.sound,operation,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);request.source.sound=next;
    }
    CHECK(companion::encodeRequest(request,requestWire));
    CHECK(companion::decodeRequest({static_cast<const std::byte*>(requestWire.getData()),requestWire.getSize()},recalled));CHECK(recalled.source==request.source);
    std::cout<<"FULL_INVENTORY_AI_BYTES descriptor="<<authority->descriptorJson.size()<<" request_with_12_racks="<<requestWire.getSize()<<'\n';
    auto limits=RackCatalogCaptureLimits{};limits.maximumEntries=64;RackCatalogSnapshot keep;keep.id=juce::Uuid();const auto before=keep.id;
    CHECK(!captureRackCatalog(factory,inventory,limits,keep,&status));CHECK(keep.id==before);
}
void aiBoundaries()
{
    using namespace ai;
    std::cout<<"SECTION active_catalog_and_native_ai_admission\n";
    ProjectDocument source;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=0;add.effect=EffectKind::midSideSplit;
    CHECK(editRackLayout(source.sound.layout,add,source.sound.layout)==RackEditResult::applied);
    juce::var catalog;CHECK(catalogFor(source,catalog));
    const auto* controls=catalog["controls"].getArray();CHECK(controls);if(!controls)return;
    const auto& first=(*controls)[int(dynamicEqParameterStart)];
    const auto correctlyInactive=first["module"].toString()=="dynamic_eq"&&!bool(first["active"]);
    CHECK(correctlyInactive);
    // A failing old token table must not be indexed using kind15 in this RED.
    if(!correctlyInactive)return;
    auto factories=airack::test::frozenLibraryRackDesigns();const auto historicalTile=airack::test::frozenTileArrayRackDesign();
    CHECK(factories.size()==28&&std::count(factories.begin(),factories.end(),historicalTile)==1);
    std::erase(factories,historicalTile);CHECK(factories.size()==27);RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(factories,{},{},snapshot));
    juce::var inventory;CHECK(catalogInventoryFor(snapshot,inventory));
    auto authority=std::make_shared<CatalogAuthority>();authority->root={juce::Uuid(),1,0,0};authority->snapshot=snapshot.id;
    authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();
    const auto valid=ai::detail::catalogAuthorityValid(*authority);CHECK(valid);if(!valid)return;
    const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(auto& effect:source.sound.layout.effects)if(effect.use==SlotUse::present)effect.kind=EffectKind::dynamicEq;
    CHECK(catalogFor(source,catalog));controls=catalog["controls"].getArray();CHECK(controls);if(!controls)return;
    for(std::size_t i=dynamicEqParameterStart;i<dynamicEqParameterStart+89;++i){CHECK(bool((*controls)[int(i)]["active"]));CHECK((*controls)[int(i)]["module"].toString()=="dynamic_eq");}
    juce::var provider;CHECK(providerCatalogFor(source,snapshot,provider));
    CHECK(provider["module_keys"]["dynamic_eq"].isArray());
    if(provider["module_keys"]["dynamic_eq"].isArray())CHECK(provider["module_keys"]["dynamic_eq"].size()==181);
    GenerationRequest request;request.tag=authority->root;request.source=source;request.model="offline-test";request.instruction="Create a dynamic EQ rack";
    CatalogRequestContext context;context.root=authority->root;context.authority=authority;request.catalog=context;
    juce::MemoryBlock wire;const auto encoded=companion::encodeRequest(request,wire);CHECK(encoded);
    if(encoded){GenerationRequest recalled;CHECK(companion::decodeRequest({static_cast<const std::byte*>(wire.getData()),wire.getSize()},recalled));
        CHECK(recalled.source==source);CHECK(recalled.catalog&&recalled.catalog->authority->descriptorJson==authority->descriptorJson);}
    std::cout<<"AI_BYTES inventory="<<authority->descriptorJson.size()<<" provider="<<juce::JSON::toString(provider,true,22).getNumBytesAsUTF8()<<" request="<<wire.getSize()
        <<" schema="<<juce::JSON::toString(proposalSchema(request.policy,*authority),true,22).getNumBytesAsUTF8()<<'\n';
}
void nativeProposals()
{
    using namespace ai;
    std::cout<<"SECTION placed_rack_all_owned_fields_and_atomic_rejection\n";
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto& design=*found;
    // The user places the rack; CREATE SOUND then reaches every field the rack owns.
    ProjectDocument source;const auto original=source;ProposalPolicy policy;
    RackOperation place;place.kind=RackOperationKind::place;place.design=design;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,7}};
    ProjectDocument placed;RackDifference difference;CHECK(makeRackCandidate(source.sound,place,SoundEditOrigin::manual,placed.sound,difference)==RackCandidateStatus::ready);
    CHECK(placed.sound.personalities&&placed.sound.personalities->instances.size()==1);if(!placed.sound.personalities)return;
    ProposalEdits edits;const auto reach=unitReach(NativeModule::dynamicEq,7);CHECK(reach.size()==181);
    for(const auto i:reach){const auto& field=parameterRegistry()[i];
        const auto value=field.kind==ParameterKind::continuous?(i%2?.75f:.25f):placed.sound.values[i];
        edits.parameters.push_back({field.id,value});}
    ProjectDocument candidate;CHECK(admitProposal(placed,policy,edits,candidate)==ProposalStatus::accepted);
    for(const auto& edit:edits.parameters){const auto tokens=juce::StringArray::fromTokens(edit.id,".","");
        const auto i=rackParameterIndex(NativeModule::dynamicEq,7,tokens.joinIntoString(".",2));CHECK(i);if(i)CHECK(candidate.sound.values[*i]==edit.normalized);}
    for(std::size_t i=0;i<parameterCount;++i)if(!std::binary_search(reach.begin(),reach.end(),i))CHECK(candidate.sound.values[i]==placed.sound.values[i]);
    CHECK(candidate.pattern==placed.pattern&&candidate.sound.macros==placed.sound.macros&&candidate.sound.layout==placed.sound.layout);
    juce::MemoryBlock state;CHECK(encodeProjectState(candidate,state));ProjectDocument recalled;CHECK(decodeProjectState(state.getData(),state.getSize(),recalled)&&recalled==candidate);
    int rejection=0;
    const auto reject=[&](const ProjectDocument& input,const ProposalPolicy& p,const ProposalEdits& e){++rejection;auto output=original;const auto status=admitProposal(input,p,e,output);if(status==ProposalStatus::accepted)std::cerr<<"UNEXPECTED_ACCEPT rejection="<<rejection<<'\n';CHECK(status!=ProposalStatus::accepted);CHECK(output==original);};
    auto locked=placed;locked.sound.effectLocks[7]=true;reject(locked,policy,edits);
    const auto gain=rackParameterIndex(NativeModule::dynamicEq,7,"dynamic_eq.band.1.gain_db");CHECK(gain);
    if(gain){locked=placed;locked.sound.controlLocks[*gain]=true;reject(locked,policy,edits);}
    auto bad=edits;bad.parameters.push_back(edits.parameters.front());reject(placed,policy,bad);
    bad=edits;bad.parameters.front().normalized=std::numeric_limits<float>::quiet_NaN();reject(placed,policy,bad);
    bad=edits;bad.parameters.push_back({"effect.7.dynamic_eq.band.1.gain_db",.7f});reject(placed,policy,bad);
    bad=edits;bad.parameters.push_back({"effect.8.eq.bell.gain_db",.7f});reject(placed,policy,bad);
    // Normalized choices keep the established nearest-detent contract.
    bad=edits;for(auto& e:bad.parameters)if(e.id.endsWith("band.1.shape"))e.normalized=.3f;
    ProjectDocument snapped;CHECK(admitProposal(placed,policy,bad,snapped)==ProposalStatus::accepted);
    const auto shape=rackParameterIndex(NativeModule::dynamicEq,7,"dynamic_eq.band.1.shape");CHECK(shape);if(shape)CHECK(snapped.sound.values[*shape]==.4f);
    auto pattern=policy;pattern.scope=RequestScope::pattern;reject(placed,pattern,edits);
    bad=edits;bad.macros={{1,"effect.8.dynamic_eq.band.1.frequency_hz",.25f}};reject(placed,policy,bad);
    auto legacy=factories.front();const auto old=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.role==RackRole::fx&&d.units[0].module!=NativeModule::dynamicEq;});
    CHECK(old!=factories.end());if(old!=factories.end()){
        legacy=*old;auto& unit=legacy.units[0];unit.defaults.clear();legacy.panel.controls.clear();
        for(std::size_t i=0;i<dynamicEqParameterStart;++i){const auto& field=parameterRegistry()[i];
            if(field.id.startsWith("effect.1."))unit.defaults.push_back({unit.id,field.id.substring(9),defaultParameterValues()[i]});}
        CHECK(unit.defaults.size()==82);CHECK(validateRackDesign(legacy,RackValidationMode::instanceRecall));
        unit.defaults.push_back({unit.id,"dynamic_eq.band.1.active",0});CHECK(!validateRackDesign(legacy,RackValidationMode::instanceRecall));
    }
    CHECK(rackUnitParameterCapacity(NativeModule::drive)==82&&rackUnitParameterCapacity(NativeModule::dynamicEq)==181);
}
void rackOperations()
{
    std::cout<<"SECTION dynamic_duplicate_move_remove_swap_and_neutral_audio\n";
    const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    CHECK(found!=designs.end());if(found==designs.end())return;
    SoundDocument original,placed;RackDifference difference;RackOperation operation;operation.newInstance=juce::Uuid();operation.design=*found;
    operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
    CHECK(makeRackCandidate(original,operation,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
    RoutedPatch dryPatch,wetPatch;CHECK(makeRoutedPatch(original,dryPatch));CHECK(makeRoutedPatch(placed,wetPatch));
    RoutedSynthEngine dry,wet;CHECK(dry.prepare(48000,64)&&wet.prepare(48000,64));CHECK(dry.setPatch(dryPatch)&&wet.setPatch(wetPatch));dry.reset();wet.reset();
    juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
    for(int block=0;block<8;++block){dry.render(a,midi);wet.render(b,midi);midi.clear();
        for(int c=0;c<2;++c)for(int n=0;n<64;++n)CHECK(a.getSample(c,n)==b.getSample(c,n));}
    const auto selected=operation.newInstance;operation={};operation.kind=RackOperationKind::duplicate;operation.selected=selected;
    operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,masterChain,1};operation.allocation={{0,7}};
    SoundDocument duplicated;CHECK(makeRackCandidate(placed,operation,SoundEditOrigin::manual,duplicated,difference)==RackCandidateStatus::ready);
    CHECK(duplicated.personalities&&duplicated.personalities->instances.size()==2);if(!duplicated.personalities)return;
    CHECK(duplicated.personalities->instances[0].id!=duplicated.personalities->instances[1].id);
    const auto gain=*rackParameterIndex(NativeModule::dynamicEq,0,"dynamic_eq.band.8.gain_db");const auto otherGain=*rackParameterIndex(NativeModule::dynamicEq,7,"dynamic_eq.band.8.gain_db");
    auto changed=duplicated;changed.values[gain]=.75f;SoundDocument edited;
    CHECK(reconcileRackEdit(duplicated,changed,{},SoundEditOrigin::manual,edited,difference)==RackCandidateStatus::ready);CHECK(edited.values[otherGain]==duplicated.values[otherGain]);
    operation={};operation.kind=RackOperationKind::move;operation.selected=selected;operation.destination={RackRole::fx,0,0};SoundDocument moved;
    CHECK(makeRackCandidate(edited,operation,SoundEditOrigin::manual,moved,difference)==RackCandidateStatus::ready);CHECK(moved.values==edited.values);CHECK(moved.layout.effects[0].chain==0);
    const auto peer=moved.personalities->instances[1];operation={};operation.kind=RackOperationKind::remove;operation.selected=selected;SoundDocument removed;
    CHECK(makeRackCandidate(moved,operation,SoundEditOrigin::manual,removed,difference)==RackCandidateStatus::ready);
    CHECK(removed.layout.effects[0].use==SlotUse::retired);CHECK(removed.values==moved.values);CHECK(removed.personalities->instances.front()==peer);
    operation={};operation.kind=RackOperationKind::swap;operation.selected=peer.id;operation.destination={RackRole::fx,masterChain,0};operation.design=*found;operation.allowSlotReuse=true;SoundDocument swapped;
    CHECK(makeRackCandidate(removed,operation,SoundEditOrigin::manual,swapped,difference)==RackCandidateStatus::ready);CHECK(swapped.layout.effects[7].kind==EffectKind::dynamicEq);
    operation={};operation.design=*found;operation.destination={RackRole::fx,masterChain,1};operation.allocation={{0,7}};
    auto preserved=swapped;CHECK(makeRackCandidate(swapped,operation,SoundEditOrigin::manual,preserved,difference)!=RackCandidateStatus::ready);CHECK(preserved==swapped);
}
EqBandPatch rawEqBand(const SoundDocument& sound,std::uint8_t slot,std::uint8_t band)
{
    EqBandPatch result;result.band=band;
    const auto slotPrefix="effect."+juce::String(slot+1)+".",bandPrefix="dynamic_eq.band."+juce::String(band+1)+".";
    for(const auto index:unitReach(NativeModule::dynamicEq,slot)){
        const auto key=parameterRegistry()[index].id.substring(slotPrefix.length());
        if(key.startsWith(bandPrefix))result.fields.push_back({key,sound.values[index]});
    }
    return result;
}
SoundDocument eqEditSound(std::uint8_t slot)
{
    SoundDocument sound;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(sound.layout,add,sound.layout)==RackEditResult::applied);
    return sound;
}
void eqPatchClipboardRoundTrip()
{
    std::cout<<"SECTION eq_raw_band_capture_strict_clipboard_and_macro_independence\n";
    for(std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)}){
        auto sound=eqEditSound(slot);const auto base=dynamicEqParameterStart+slot*dynamicEqParametersPerSlot;
        sound.values[base]=1;sound.values[base+4]=.31415927f;sound.values[base+7*11]=1;sound.values[base+7*11+4]=.73f;
        sound.macros[0]={parameterRegistry()[base+4].stateKey,.2f};sound.values[1]=1;
        const auto before=sound;EqBandPatch captured;CHECK(captureEqBand(sound,slot,0,captured));CHECK(captured==rawEqBand(sound,slot,0));
        const auto expected=std::vector<EqBandPatch>{rawEqBand(sound,slot,7),rawEqBand(sound,slot,0)};
        juce::String text="unchanged";CHECK(encodeEqClipboard(expected,text));CHECK(text.getNumBytesAsUTF8()<=16384);
        std::vector<EqBandPatch> decoded;CHECK(decodeEqClipboard(text,decoded));CHECK(decoded==expected);
        CHECK(sound==before);
        for(const auto& field:captured.fields)if(field.key.endsWith(".gain_db"))CHECK(field.normalized==.31415927f);
        auto sentinel=rawEqBand(sound,slot,7),output=sentinel;CHECK(!captureEqBand(sound,8,0,output));CHECK(output==sentinel);CHECK(!captureEqBand(sound,slot,8,output));CHECK(output==sentinel);
        auto malformed=expected;malformed[0].fields.pop_back();text="keep";CHECK(!encodeEqClipboard(malformed,text));CHECK(text=="keep");
        malformed=expected;malformed[1]=malformed[0];CHECK(!encodeEqClipboard(malformed,text));CHECK(text=="keep");
        std::vector<EqBandPatch> preserved{sentinel};const auto preservedBefore=preserved;
        const std::array<juce::String,10> invalid{
            "{}","[]",R"({"format":"airack.eq-bands","version":99,"bands":[]})",R"({"format":"airack.eq-bands","version":1,"bands":[]})",
            R"({"format":"airack.eq-bands","format":"airack.eq-bands","version":1,"bands":[]})",
            juce::String::repeatedString("x",16385),juce::String::repeatedString("[",30)+"0"+juce::String::repeatedString("]",30),
            "{",R"({"format":"airack.eq-bands","version":1,"bands":null})",R"({"format":"airack.eq-bands","version":1.0,"bands":[]})"};
        for(const auto& input:invalid){CHECK(!decodeEqClipboard(input,preserved));CHECK(preserved==preservedBefore);}
        const bool encoded=encodeEqClipboard(expected,text);CHECK(encoded);if(!encoded)continue;
        CHECK(!decodeEqClipboard(text.replace("airack.eq-bands","airack.eq-bands\\u0000junk"),preserved));CHECK(preserved==preservedBefore);
        CHECK(!decodeEqClipboard(text.replaceFirstOccurrenceOf("\"version\"","\"unknown\""),preserved));CHECK(preserved==preservedBefore);
        CHECK(!decodeEqClipboard(text.replaceFirstOccurrenceOf("\"version\"","\"format\":\"airack.eq-bands\",\"version\""),preserved));CHECK(preserved==preservedBefore);
        CHECK(!decodeEqClipboard(text.replaceFirstOccurrenceOf("\"version\"","\"extra\":0,\"version\""),preserved));CHECK(preserved==preservedBefore);
        CHECK(!decodeEqClipboard(text.replaceFirstOccurrenceOf("\"normalized\"","\"normalized\":0,\"normalized\""),preserved));CHECK(preserved==preservedBefore);
        auto extra=juce::JSON::parse(text);extra["bands"][0].getDynamicObject()->setProperty("extra",1);
        CHECK(!decodeEqClipboard(juce::JSON::toString(extra,true),preserved));CHECK(preserved==preservedBefore);
        extra=juce::JSON::parse(text);extra.getDynamicObject()->setProperty("version",1.0);
        CHECK(!decodeEqClipboard(juce::JSON::toString(extra,true),preserved));CHECK(preserved==preservedBefore);
        auto wire=juce::JSON::parse(text);auto* fields=wire["bands"][0]["fields"].getArray();CHECK(fields&&fields->size()>0);
        if(fields&&fields->size()>0){(*fields)[0].getDynamicObject()->setProperty("normalized",true);CHECK(!decodeEqClipboard(juce::JSON::toString(wire,true),preserved));CHECK(preserved==preservedBefore);}
        std::vector<EqBandPatch> eight;for(std::uint8_t band=0;band<8;++band)eight.push_back(rawEqBand(sound,slot,band));
        CHECK(encodeEqClipboard(eight,text));CHECK(decodeEqClipboard(text,decoded));CHECK(decoded==eight);
        eight.push_back(eight[0]);text="keep";CHECK(!encodeEqClipboard(eight,text));CHECK(text=="keep");
    }
}
void eqPatchesAndPasteAreAtomic()
{
    std::cout<<"SECTION eq_typed_patches_paste_order_capacity_locks_and_atomic_refusal\n";
    auto source=eqEditSound(0);source.values[dynamicEqParameterStart]=source.values[dynamicEqParameterStart+7*11]=1;
    source.values[dynamicEqParameterStart+4]=.31f;source.values[dynamicEqParameterStart+7*11+4]=.73f;
    const std::vector<EqBandPatch> copied{rawEqBand(source,0,7),rawEqBand(source,0,0)};
    auto target=eqEditSound(7);const auto base=dynamicEqParameterStart+7*dynamicEqParametersPerSlot;
    target.values[base]=target.values[base+2*11]=1;target.macros[0]={parameterRegistry()[base+11+4].stateKey,.15f};target.values[1]=.8f;
    const auto before=target;EqPatch paste;CHECK(makeEqPaste(target,7,copied,paste));CHECK(paste.bands.size()==2);
    if(paste.bands.size()==2){CHECK(paste.bands[0].band==1);CHECK(paste.bands[1].band==3);}
    SoundDocument result;CHECK(makeEqEdit(target,7,paste,result));CHECK(result.values[base+11]==1);CHECK(result.values[base+3*11]==1);
    CHECK(result.values[base+11+4]==.31f);CHECK(result.values[base+3*11+4]==.73f);CHECK(result.macros==target.macros);CHECK(target==before);
    std::set<std::size_t> touched;for(const auto& band:paste.bands)for(const auto& field:band.fields){const auto index=rackParameterIndex(NativeModule::dynamicEq,7,field.key);CHECK(index.has_value());if(index)touched.insert(*index);}
    for(std::size_t n=0;n<parameterCount;++n)if(!touched.contains(n))CHECK(result.values[n]==before.values[n]);
    EqPatch partial;partial.bands.push_back({1,{{"dynamic_eq.band.2.gain_db",.625f}}});CHECK(makeEqEdit(target,7,partial,result));CHECK(result.values[base+15]==.625f);
    partial.slotFields={{"dynamic_eq.output_gain_db",.4f},{"bypassed",1}};CHECK(makeEqEdit(target,7,partial,result));CHECK(result.layout==target.layout);
    auto alias=target;CHECK(makeEqEdit(alias,7,partial,alias));CHECK(alias==result);
    for(int reason=0;reason<12;++reason){auto sound=target;auto patch=partial;auto untouched=source;
        if(reason==0)sound.effectLocks[7]=true;
        if(reason==1)sound.controlLocks[base+15]=true;
        if(reason==2)patch.bands.push_back(patch.bands[0]);
        if(reason==3)patch.bands[0].fields.push_back(patch.bands[0].fields[0]);
        if(reason==4)patch.bands[0].fields[0].key="dynamic_eq.band.1.gain_db";
        if(reason==5)patch.bands[0].fields[0].normalized=std::numeric_limits<float>::quiet_NaN();
        if(reason==6)patch.bands[0].fields[0].normalized=1.01f;
        if(reason==7)patch.slotFields[0].key="delay.seconds";
        if(reason==8)patch.bands[0].fields={{"dynamic_eq.band.2.shape",.1f}};
        if(reason==9)sound.layout.effects[7].kind=EffectKind::eq;
        if(reason==10)patch.bands[0].fields[0].normalized=-.01f;
        if(reason==11)patch.bands[0].fields[0].normalized=std::numeric_limits<float>::infinity();
        CHECK(!makeEqEdit(sound,7,patch,untouched));CHECK(untouched==source);
    }
    auto full=target;for(std::size_t band=0;band<8;++band)full.values[base+band*11]=1;
    auto sentinel=partial;CHECK(!makeEqPaste(full,7,copied,sentinel));CHECK(sentinel==partial);
    auto locked=target;locked.controlLocks[base+11+5]=true;CHECK(!makeEqPaste(locked,7,copied,sentinel));CHECK(sentinel==partial);
    auto duplicate=copied;duplicate[1]=duplicate[0];CHECK(!makeEqPaste(target,7,duplicate,sentinel));CHECK(sentinel==partial);
    CHECK(!makeEqEdit(target,8,partial,result));CHECK(target==before);
}

void placementCatalogAndFrozenPredecessor()
{
    std::cout<<"SECTION appended_placement_bank_and_frozen_1959_predecessor\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-placement-legacy");
    juce::MemoryBlock bytes;CHECK(directory.getChildFile("registry-v18.json").loadFileAsData(bytes));CHECK(juce::SHA256(bytes).toHexString()=="b340008a2f9413f7962eebf065e27de01117786bb83b20f790637c4dc3a6aba3");
    const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())));const auto registry=parameterRegistry();
    CHECK(rows.isArray()&&rows.size()==1959);CHECK(registry.size()==4519);CHECK(dynamicEqPlacementParameterStart==1959);CHECK(dynamicEqPlacementParametersPerSlot==8);
    if(rows.size()==1959&&registry.size()>=1959)for(int i=0;i<1959;++i){const auto& d=registry[std::size_t(i)];const auto row=rows[i];
        CHECK(d.id==row["id"].toString());CHECK(d.name==row["name"].toString());CHECK(d.unit==row["unit"].toString());CHECK(int(d.kind)==int(row["kind"]));CHECK(d.minimum==float(row["minimum"]));CHECK(d.maximum==float(row["maximum"]));CHECK(d.defaultValue==float(row["default"]));CHECK(d.skew==float(row["skew"]));CHECK(d.stateKey==std::uint32_t(juce::int64(row["key"])));CHECK(d.choices.size()==std::size_t(row["choices"].size()));for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());}
    for(std::uint8_t slot=0;slot<8;++slot){CHECK(unitReach(NativeModule::dynamicEq,slot).size()==181);
        for(std::uint8_t band=0;band<8;++band){const auto key="dynamic_eq.band."+juce::String(band+1)+".placement";const auto index=rackParameterIndex(NativeModule::dynamicEq,slot,key);CHECK(index);if(!index)continue;
            CHECK(*index==1959+slot*8+band);const auto& d=registry[*index];CHECK(d.id=="effect."+juce::String(slot+1)+"."+key);CHECK(d.kind==ParameterKind::choice);CHECK(d.minimum==0&&d.maximum==4&&d.defaultValue==0&&d.skew==1);CHECK(d.choices==std::vector<juce::String>({"Stereo","Left","Right","Mid","Side"}));
            SoundDocument sound;RoutedPatch patch;for(int mode=0;mode<5;++mode){sound.values[*index]=float(mode)/4;CHECK(makeRoutedPatch(sound,patch));CHECK(patch.effects[slot].dynamicEq.bands[band].placement==DynamicEqPlacement(mode));}
            sound.values[*index]=0;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind::dynamicEq;CHECK(editRackLayout(sound.layout,add,sound.layout)==RackEditResult::applied);
            EqPatch edits;edits.bands={{band,{{key,.75f}}}};SoundDocument changed;CHECK(makeEqEdit(sound,slot,edits,changed));CHECK(changed.values[*index]==.75f);
            sound.effectLocks[slot]=true;auto candidate=sound;candidate.values[*index]=1;CHECK(!validateAiSoundEdit(sound,candidate));CHECK(!makeEqEdit(sound,slot,edits,changed));
            sound.effectLocks[slot]=false;sound.controlLocks[*index]=true;candidate=sound;candidate.values[*index]=1;CHECK(!validateAiSoundEdit(sound,candidate));CHECK(!makeEqEdit(sound,slot,edits,changed));
        }
    }
    CHECK(currentSoundStateVersion==31);CHECK(currentProjectStateVersion==30);CHECK(currentSoundStateSize==41053);CHECK(currentMinimumProjectStateBytes==41161);
    ProjectDocument oldProject;bytes.reset();CHECK(directory.getChildFile("rich-v17.airackproject").loadFileAsData(bytes));CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),oldProject));
    for(const auto* name:{"default-v18.soundstate","rich-v18.soundstate","embedded-v18.soundstate"}){SoundDocument loaded;bytes.reset();CHECK(directory.getChildFile(name).loadFileAsData(bytes));CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),loaded));RoutedPatch patch;CHECK(makeRoutedPatch(loaded,patch));for(const auto& effect:patch.effects)for(const auto& band:effect.dynamicEq.bands)CHECK(band.placement==DynamicEqPlacement::stereo);}
}


void placementStateDomainsAndClipboard()
{
    std::cout<<"SECTION placement_saved_state_roundtrip_sound18_domain_and_legacy_clipboard\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-placement-legacy");
    SoundDocument current;for(std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)}){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind::dynamicEq;CHECK(editRackLayout(current.layout,add,current.layout)==RackEditResult::applied);
        for(std::size_t band=0;band<8;++band){const auto index=1959+slot*8+band;current.values[index]=float((band+slot)%5)/4;current.controlLocks[index]=band%2==0;}}
    current.macros[2]={parameterRegistry()[dynamicEqParameterStart+3].stateKey,.2f};current.values[3]=.75f;
    auto discreteMacro=current;discreteMacro.macros[2].targetKey=parameterRegistry()[2022].stateKey;CHECK(!validateSoundDocument(discreteMacro));
    juce::MemoryBlock encoded;CHECK(encodeSoundState(current,encoded));SoundDocument recalled;CHECK(decodeSoundState(encoded.getData(),encoded.getSize(),recalled)&&recalled==current);
    ProjectDocument project;project.sound=current;encoded.reset();CHECK(encodeProjectState(project,encoded));ProjectDocument loaded;CHECK(decodeProjectState(encoded.getData(),encoded.getSize(),loaded)&&loaded==project);
    juce::MemoryBlock old;CHECK(directory.getChildFile("embedded-v18.soundstate").loadFileAsData(old));SoundDocument original;const bool oldRead=decodeSoundState(old.getData(),old.getSize(),original);CHECK(oldRead);if(!oldRead||!original.personalities||original.personalities->instances.empty())return;
    const auto reject=[&](juce::MemoryBlock malformed){const auto digest=juce::SHA256(malformed.getData(),malformed.getSize()-32).getRawData();std::memcpy(static_cast<char*>(malformed.getData())+malformed.getSize()-32,digest.getData(),32);auto out=original;CHECK(!decodeSoundState(malformed.getData(),malformed.getSize(),out));CHECK(out==original);};
    const auto future=parameterRegistry()[1959].stateKey;
    auto malformed=old;std::memcpy(static_cast<char*>(malformed.getData())+16,&future,4);reject(malformed);
    malformed=old;std::memcpy(static_cast<char*>(malformed.getData())+16+1959*8+78,&future,4);reject(malformed);
    for(bool panel:{false,true}){
        auto candidate=original;auto personalities=std::make_shared<RackPersonalityState>(*original.personalities);auto& design=personalities->instances[0].design;
        if(panel){auto control=design.panel.controls[0];control.key="dynamic_eq.band.1.placement";control.label="Placement";control.widget=FocusWidget::choice;control.response=FocusResponse::linear;control.minimum=0;control.maximum=4;design.panel.controls[0]=control;}
        else design.units[0].defaults.push_back({0,"dynamic_eq.band.1.placement",0});
        candidate.personalities=personalities;juce::MemoryBlock modern;const bool wrote=encodeSoundState(candidate,modern);CHECK(wrote);if(!wrote)continue;
        juce::MemoryBlock control;CHECK(oldEnvelopeWithPersonality(old,oldSound18NumericBytes+4+oldSound18CompatibilityBytes,original,control));CHECK(control==old);
        SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==original);
        juce::MemoryBlock nested;CHECK(oldEnvelopeWithPersonality(old,oldSound18NumericBytes+4+oldSound18CompatibilityBytes,candidate,nested));reject(nested);
    }
    EqBandPatch currentBand;CHECK(captureEqBand(current,0,0,currentBand));juce::String currentClipboard;CHECK(encodeEqClipboard(std::span(&currentBand,1),currentClipboard));CHECK(int(juce::JSON::parse(currentClipboard)["version"])==5);
    const auto text=directory.getChildFile("eight-bands-v1.eqclipboard.json").loadFileAsString();CHECK(text.isNotEmpty());std::vector<EqBandPatch> bands;CHECK(decodeEqClipboard(text,bands));CHECK(bands.size()==8);
    if(bands.size()==8){for(const auto& band:bands){CHECK(band.fields.size()==22);const auto it=std::find_if(band.fields.begin(),band.fields.end(),[](const auto& field){return field.key.endsWith(".placement");});CHECK(it!=band.fields.end()&&it->normalized==0);}
        auto sentinel=bands;const auto invalid=juce::JSON::parse(text);auto changed=invalid;changed.getDynamicObject()->setProperty("version",2);CHECK(!decodeEqClipboard(juce::JSON::toString(changed),sentinel));CHECK(sentinel==bands);
        juce::String modern;CHECK(encodeEqClipboard(bands,modern));const auto document=juce::JSON::parse(modern);CHECK(int(document["version"])==5);std::vector<EqBandPatch> copy;CHECK(decodeEqClipboard(modern,copy)&&copy==bands);
    }
}


void placementSketchUsesActualEnergy()
{
    std::cout<<"SECTION sketch_mixed_placement_energy_fit_preserves_occupied_bands\n";
    for(double rate:{48000.,96000.}){
        DynamicEqSettings base;base.autoGain=true;base.gainScale=1.4f;base.outputGainDb=-2;
        base.bands[0].active=true;base.bands[0].placement=DynamicEqPlacement::side;base.bands[0].frequencyHz=700;base.bands[0].gainDb=-6;base.bands[0].q=.7071f;
        base.bands[1].active=true;base.bands[1].placement=DynamicEqPlacement::left;base.bands[1].shape=DynamicEqShape::highShelf;base.bands[1].frequencyHz=5500;base.bands[1].gainDb=3;
        base.bands[2].placement=DynamicEqPlacement::side;base.bands[2].rangeDb=9;
        auto desired=base;desired.bands[2]={};desired.bands[2].active=true;desired.bands[2].frequencyHz=1800;desired.bands[2].gainDb=6;desired.bands[2].q=.7071f;
        EqCurve target;for(std::size_t i=0;i<129;++i){const auto response=dynamicEqEnergyResponse(desired,rate,20.*std::pow(1000.,double(i)/128));CHECK(response);target.db[i]=response?float(20.*std::log10(std::max(*response,1.e-12))):0;}
        const auto fit=fitEqCurve(target,base,std::bitset<8>(4),rate);CHECK(fit.ready);CHECK(fit.evaluations<=4096);CHECK(fit.settings.bands[0]==base.bands[0]&&fit.settings.bands[1]==base.bands[1]);CHECK(fit.settings.autoGain==base.autoGain&&fit.settings.gainScale==base.gainScale&&fit.settings.outputGainDb==base.outputGainDb);
        CHECK(fit.settings.bands[2].active&&fit.settings.bands[2].placement==DynamicEqPlacement::stereo&&fit.settings.bands[2].rangeDb==0);
        double squared=0;for(std::size_t i=0;i<129;++i){const auto response=dynamicEqEnergyResponse(fit.settings,rate,20.*std::pow(1000.,double(i)/128));CHECK(response);const auto difference=target.db[i]-(response?20.*std::log10(std::max(*response,1.e-12)):0);squared+=difference*difference;}
        const auto rms=std::sqrt(squared/129);CHECK(rms<1);CHECK(std::abs(rms-fit.rmsDb)<.001);
    }
}

void extensionClipboardAndSnapshots()
{
    std::cout<<"SECTION extension_clipboard3_legacy1_2_and_raw_AB\n";
    SoundDocument source,target;CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::dynamicEq},source.layout)==RackEditResult::applied);
    CHECK(editRackLayout(target.layout,{RackEditKind::addEffect,7,masterChain,0,EffectKind::dynamicEq},target.layout)==RackEditResult::applied);
    const auto first=dynamicEqExtensionParameterStart+7*2;
    source.values[first]=2.f/3;source.values[first+1]=1;
    EqBandPatch band;CHECK(captureEqBand(source,0,7,band));CHECK(band.fields.size()==22);
    juce::String text;CHECK(encodeEqClipboard(std::span(&band,1),text));CHECK(int(juce::JSON::parse(text)["version"])==5);
    std::vector<EqBandPatch> bands;CHECK(decodeEqClipboard(text,bands));CHECK(bands==std::vector<EqBandPatch>{band});
    studio::EqComparison ab;CHECK(ab.store(false,std::span(&band,1),{}));const auto saved=ab.recall(false);CHECK(saved&&saved->bands==bands);
    band.fields.back().normalized=0;CHECK(ab.recall(false)==saved);
    EqPatch paste;if(saved){CHECK(makeEqPaste(target,7,saved->bands,paste));SoundDocument pasted;CHECK(makeEqEdit(target,7,paste,pasted));
        CHECK(pasted.values[dynamicEqExtensionParameterStart+7*16]==2.f/3&&pasted.values[dynamicEqExtensionParameterStart+7*16+1]==1);}
    auto futureAsOld=juce::JSON::parse(text);futureAsOld.getDynamicObject()->setProperty("version",2);auto sentinel=bands;
    CHECK(!decodeEqClipboard(juce::JSON::toString(futureAsOld),sentinel)&&sentinel==bands);
    const auto fixtures=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory();
    for(const auto* name:{"eq-placement-legacy/eight-bands-v1.eqclipboard.json","eq-shapes-legacy/eight-bands-v2.eqclipboard.json"}){
        const auto old=fixtures.getChildFile(name).loadFileAsString();CHECK(old.isNotEmpty());std::vector<EqBandPatch> decoded;const bool read=decodeEqClipboard(old,decoded);CHECK(read);if(!read)continue;
        CHECK(decoded.size()==8);for(const auto& b:decoded){CHECK(b.fields.size()==22);for(const auto* key:{"shape_extension","cut_slope_extension"}){
            const auto found=std::find_if(b.fields.begin(),b.fields.end(),[&](const auto& f){return f.key=="dynamic_eq.band."+juce::String(b.band+1)+"."+key;});CHECK(found!=b.fields.end()&&found->normalized==0);}}
        juce::String current;CHECK(encodeEqClipboard(decoded,current));CHECK(int(juce::JSON::parse(current)["version"])==5);
        std::vector<EqBandPatch> roundtrip;CHECK(decodeEqClipboard(current,roundtrip)&&roundtrip==decoded);
        auto incomplete=juce::JSON::parse(old);incomplete.getDynamicObject()->setProperty("version",3);sentinel=decoded;CHECK(!decodeEqClipboard(juce::JSON::toString(incomplete),sentinel)&&sentinel==decoded);
    }
}
void extensionSelectorsPreserveLegacyAutomation()
{
    std::cout<<"SECTION appended_shape_slope_selectors_and_frozen2023_catalog\n";
    const auto registry=parameterRegistry();CHECK(parameterCount==4519);CHECK(registry.size()==4519);
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-shapes-legacy");
    const auto frozen=juce::JSON::parse(directory.getChildFile("registry-v19.json").loadFileAsString());
    const auto* rows=frozen.getArray();CHECK(rows&&rows->size()==2023);
    if(rows)for(int i=0;i<rows->size()&&std::size_t(i)<registry.size();++i){
        const auto& row=rows->getReference(i);const auto& d=registry[std::size_t(i)];
        CHECK(row["id"].toString()==d.id&&row["name"].toString()==d.name&&row["unit"].toString()==d.unit);
        CHECK(int(row["kind"])==int(d.kind)&&float(row["minimum"])==d.minimum&&float(row["maximum"])==d.maximum);
        CHECK(float(row["default"])==d.defaultValue&&float(row["skew"])==d.skew&&juce::int64(row["key"])==d.stateKey);
        const auto* choices=row["choices"].getArray();CHECK(choices&&choices->size()==int(d.choices.size()));
        if(choices)for(int j=0;j<choices->size()&&std::size_t(j)<d.choices.size();++j)CHECK(choices->getReference(j).toString()==d.choices[std::size_t(j)]);
    }
    for(std::uint8_t slot=0;slot<8;++slot){
        SoundDocument source;CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,slot,masterChain,0,EffectKind::dynamicEq},source.layout)==RackEditResult::applied);
        for(std::size_t band=0;band<8;++band){
            const auto base=dynamicEqParameterStart+slot*dynamicEqParametersPerSlot+band*11;
            CHECK(registry[base+2].choices.size()==6&&registry[base+2].minimum==0&&registry[base+2].maximum==5);
            CHECK(registry[base+6].choices.size()==2&&registry[base+6].minimum==0&&registry[base+6].maximum==1);
            source.values[base]=1;
            for(int legacy=0;legacy<6;++legacy){source.values[base+2]=float(legacy)/5;RoutedPatch p;CHECK(makeRoutedPatch(source,p));CHECK(p.effects[slot].dynamicEq.bands[band].shape==DynamicEqShape(legacy));}
            for(int field=0;field<2;++field){
                const auto suffix=field==0?"shape_extension":"cut_slope_extension";
                const auto id="effect."+juce::String(slot+1)+".dynamic_eq.band."+juce::String(band+1)+"."+suffix;
                const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;});
                CHECK(found!=registry.end());if(found==registry.end())continue;
                const auto index=std::size_t(found-registry.begin());CHECK(index==2063+slot*16+band*2+std::size_t(field));
                const int count=field==0?4:5;CHECK(found->choices.size()==std::size_t(count)&&found->minimum==0&&found->maximum==count-1&&found->defaultValue==0);
                CHECK(source.values[index]==0);
                constexpr std::array shapes{DynamicEqShape::notch,DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt};
                constexpr std::array slopes{DynamicEqSlope::db24,DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96};
                for(int selection=0;selection<count;++selection){
                    auto sound=source;sound.values[base+2]=field==0?1.f:.6f;sound.values[base+6]=1;sound.values[index]=float(selection)/float(count-1);
                    RoutedPatch p;CHECK(makeRoutedPatch(sound,p));const auto& b=p.effects[slot].dynamicEq.bands[band];
                    if(field==0)CHECK(b.shape==shapes[std::size_t(selection)]);
                    else CHECK(b.shape==DynamicEqShape::lowCut&&b.slope==slopes[std::size_t(selection)]);
                    CHECK(sound.values[base+2]==(field==0?1.f:.6f)&&sound.values[base+6]==1);
                }
                auto invalid=source;invalid.values[index]=.2f;CHECK(!validateSoundDocument(invalid));
                auto candidate=source;candidate.values[index]=1;CHECK(validateAiSoundEdit(source,candidate));
                EqPatch edit;edit.bands.push_back({std::uint8_t(band),{{"dynamic_eq.band."+juce::String(band+1)+"."+suffix,1}}});
                SoundDocument edited;CHECK(makeEqEdit(source,slot,edit,edited));CHECK(edited.values[index]==1);
                for(bool rackLock:{false,true}){
                    auto locked=source;if(rackLock)locked.effectLocks[slot]=true;else locked.controlLocks[index]=true;
                    candidate=locked;candidate.values[index]=1;CHECK(!validateAiSoundEdit(locked,candidate));
                    SoundDocument output=source;const auto before=output;CHECK(!makeEqEdit(locked,slot,edit,output));CHECK(output==before);
                }
            }
        }
    }
}
}
void acceptedDelayAndDraftEqMigration()
{
    std::cout<<"SECTION accepted_delay20_and_draft_eq20_have_distinct_domains\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-shapes-accepted-delay");
    const auto load=[&](const char* name,std::size_t size,const char* sha){juce::MemoryBlock raw;CHECK(directory.getChildFile(name).loadFileAsData(raw));CHECK(raw.getSize()==size);CHECK(juce::SHA256(raw).toHexString()==sha);return raw;};
    const auto word=[](const juce::MemoryBlock& raw,std::size_t at){juce::MemoryInputStream s(raw,false);s.setPosition(juce::int64(at));return std::bit_cast<std::uint32_t>(s.readInt());};
    const auto put=[](juce::MemoryBlock& raw,std::size_t at,std::uint32_t value){for(int byte=0;byte<4;++byte)static_cast<unsigned char*>(raw.getData())[at+byte]=static_cast<unsigned char>(value>>(8*byte));};
    const auto sign=[](juce::MemoryBlock& raw){const auto digest=juce::SHA256(raw.getData(),raw.getSize()-32).getRawData();std::memcpy(static_cast<char*>(raw.getData())+raw.getSize()-32,digest.getData(),32);};
    const auto registry=parameterRegistry();const SoundDocument defaults;
    CHECK(parameterCount==4519&&delayTapeParameterStart==2023&&dynamicEqExtensionParameterStart==2063&&delayStereoParameterStart==2191);
    const auto prefix=load("registry-v20.json",452593,"9940eb65ec525d7530b0aaaf188c45d5716ded7cafde82b832946c42a88a7620");
    const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(prefix.getData()),int(prefix.getSize())));CHECK(rows.isArray()&&rows.size()==2063);
    for(int i=0;i<rows.size()&&i<2063;++i){const auto& row=rows[i];const auto& d=registry[std::size_t(i)];
        CHECK(row["id"].toString()==d.id&&row["name"].toString()==d.name&&row["unit"].toString()==d.unit);
        CHECK(int(row["kind"])==int(d.kind)&&float(row["minimum"])==d.minimum&&float(row["maximum"])==d.maximum&&float(row["default"])==d.defaultValue&&float(row["skew"])==d.skew);
        CHECK(std::uint32_t(juce::int64(row["key"]))==d.stateKey&&row["choices"].size()==int(d.choices.size()));
        for(std::size_t c=0;c<d.choices.size();++c)CHECK(row["choices"][int(c)].toString()==d.choices[c]);
    }
    const auto compare=[&](const juce::MemoryBlock& raw,const SoundDocument& sound,bool draft){
        const auto count=draft?2151u:2063u;CHECK(word(raw,4)==20&&word(raw,8)==count);
        const auto locks=16+8*count+78+64,macros=16+8*count+78;
        for(std::size_t i=0;i<count;++i){const auto mapped=draft&&i>=2023?i+40:i;
            CHECK(word(raw,16+8*i)==registry[mapped].stateKey);
            CHECK(word(raw,20+8*i)==std::bit_cast<std::uint32_t>(sound.values[mapped]));
            CHECK(sound.controlLocks[mapped]==(static_cast<const unsigned char*>(raw.getData())[locks+i]==1));
        }
        for(std::size_t i=0;i<8;++i){CHECK(word(raw,macros+8*i)==sound.macros[i].targetKey);CHECK(word(raw,macros+8*i+4)==std::bit_cast<std::uint32_t>(sound.macros[i].amount));}
        for(std::size_t i=draft?2023u:2063u;i<(draft?2063u:2191u);++i)CHECK(sound.values[i]==defaults.values[i]&&!sound.controlLocks[i]);
        for(std::size_t i=delayStereoParameterStart;i<parameterCount;++i)CHECK(sound.values[i]==defaults.values[i]&&!sound.controlLocks[i]);
        juce::MemoryBlock upgraded;SoundDocument roundtrip;CHECK(encodeSoundState(sound,upgraded)&&word(upgraded,4)==31&&word(upgraded,8)==4519);CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),roundtrip)&&roundtrip==sound);
    };
    struct Fixture{const char* name;std::size_t size;const char* hash;};
    const std::array<Fixture,3> sounds{{
        {"default-v20.soundstate",18921,"9d1d81ee90c482dab34e94f7f406066477a47671b3d5a8e7167a211e9b4ef14a"},
        {"rich-v20.soundstate",18921,"41affb097a2a0816d18ce21315e2c1e581f9f5044c9bda98b5e4b03c47333125"},
        {"embedded-v20.soundstate",21816,"23c2c9e0b9686db60cdf8bf9948b17429ec77004028d24cbfdbc533c0c6de3ba"}}};
    for(const auto& fixture:sounds){const auto raw=load(fixture.name,fixture.size,fixture.hash);if(raw.getSize()!=fixture.size)continue;SoundDocument sound;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),sound);CHECK(decoded);if(!decoded)continue;compare(raw,sound,false);
        auto bad=raw;put(bad,16+8*2023,registry[2063].stateKey);sign(bad);auto out=sound;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==sound);
        bad=raw;put(bad,8,2151);sign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==sound);
    }
    for(const auto& fixture:std::array<Fixture,2>{{
        {"rich-v19.airackproject",19029,"e8d919e07862017d3e8f91d06d6747ab7fe8fd4048c1290be77c2a681491703b"},
        {"embedded-v19.airackproject",21924,"5be0b72a176e6493b42f09111e2cb2c4a703fa5b5dbd45b2398795a3321785c4"}}}){
        const auto raw=load(fixture.name,fixture.size,fixture.hash);if(raw.getSize()!=fixture.size)continue;ProjectDocument project;const bool decoded=decodeProjectState(raw.getData(),raw.getSize(),project);CHECK(decoded);if(!decoded)continue;
        const juce::MemoryBlock sound(static_cast<const char*>(raw.getData())+20,word(raw,12));compare(sound,project.sound,false);
        juce::MemoryBlock upgraded;ProjectDocument roundtrip;CHECK(encodeProjectState(project,upgraded));CHECK(decodeProjectState(upgraded.getData(),upgraded.getSize(),roundtrip)&&roundtrip==project);
    }
    auto draft=load("draft-eq-flat-tilt-v19.airackproject",19897,"9b94d37a847d0f2d8bd2bb061fb3f9039363dbfe941300151b8c72c61c50cabf");
    if(draft.getSize()!=19897)return;
    ProjectDocument project;const bool decoded=decodeProjectState(draft.getData(),draft.getSize(),project);CHECK(decoded);if(!decoded)return;
    juce::MemoryBlock sound(static_cast<const char*>(draft.getData())+20,word(draft,12));compare(sound,project.sound,true);
    constexpr std::size_t macros=16+8*2151+78,locks=macros+64;
    static_cast<unsigned char*>(sound.getData())[locks+2023]=1;static_cast<unsigned char*>(sound.getData())[locks+2150]=1;
    put(sound,macros,registry[1234].stateKey);put(sound,macros+4,std::bit_cast<std::uint32_t>(.23f));sign(sound);
    SoundDocument migrated;CHECK(decodeSoundState(sound.getData(),sound.getSize(),migrated));compare(sound,migrated,true);CHECK(migrated.controlLocks[2063]&&migrated.controlLocks[2190]);
    std::memcpy(static_cast<char*>(draft.getData())+20,sound.getData(),sound.getSize());sign(draft);ProjectDocument restored;CHECK(decodeProjectState(draft.getData(),draft.getSize(),restored)&&restored.sound==migrated);
    auto bad=sound;put(bad,16+8*2023,registry[2023].stateKey);sign(bad);auto out=migrated;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==migrated);
    bad=sound;put(bad,macros,registry[2023].stateKey);sign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==migrated);
    bad=sound;static_cast<unsigned char*>(bad.getData())[locks+2023]=2;sign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==migrated);
}
void acceptedMainStereo22Migration()
{
    std::cout<<"SECTION published_main22_domain_survives_detector_append\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-detector-accepted-delay22");
    const auto load=[&](const char* name,std::size_t size,const char* sha){
        juce::MemoryBlock raw;CHECK(directory.getChildFile(name).loadFileAsData(raw));
        CHECK(raw.getSize()==size);CHECK(juce::SHA256(raw).toHexString()==sha);return raw;
    };
    const auto word=[](const juce::MemoryBlock& raw,std::size_t at){
        juce::MemoryInputStream input(raw,false);input.setPosition(juce::int64(at));return std::bit_cast<std::uint32_t>(input.readInt());
    };
    const auto put=[](juce::MemoryBlock& raw,std::size_t at,std::uint32_t value){
        for(int byte=0;byte<4;++byte)static_cast<unsigned char*>(raw.getData())[at+byte]=static_cast<unsigned char>(value>>(8*byte));
    };
    const auto sign=[](juce::MemoryBlock& raw){
        const auto digest=juce::SHA256(raw.getData(),raw.getSize()-32).getRawData();
        std::memcpy(static_cast<char*>(raw.getData())+raw.getSize()-32,digest.getData(),32);
    };
    constexpr std::size_t oldCount=2215,newCount=4519,macros=16+8*oldCount+78,locks=macros+64;
    const auto registry=parameterRegistry();const SoundDocument defaults;
    CHECK(parameterCount==newCount&&dynamicEqDetectorParameterStart==oldCount);
    CHECK(currentSoundStateVersion==31&&currentProjectStateVersion==30);
    const auto prefix=load("registry-v22.json",494192,"4414a58b067af08a26a1b8608f29e2849a66ccdd23e77ada83873d53762c3187");
    const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(prefix.getData()),int(prefix.getSize())));
    CHECK(rows.isArray()&&rows.size()==int(oldCount));
    for(int i=0;i<rows.size()&&i<int(oldCount)&&i<int(registry.size());++i){
        const auto& row=rows[i];const auto& d=registry[std::size_t(i)];
        CHECK(row["id"].toString()==d.id&&row["name"].toString()==d.name&&row["unit"].toString()==d.unit);
        CHECK(int(row["kind"])==int(d.kind)&&float(row["minimum"])==d.minimum&&float(row["maximum"])==d.maximum
              &&float(row["default"])==d.defaultValue&&float(row["skew"])==d.skew);
        CHECK(std::uint32_t(juce::int64(row["key"]))==d.stateKey&&row["choices"].size()==int(d.choices.size()));
        for(std::size_t c=0;c<d.choices.size()&&c<std::size_t(row["choices"].size());++c)CHECK(row["choices"][int(c)].toString()==d.choices[c]);
    }
    const auto asMain22=[&](juce::MemoryBlock raw){
        CHECK(word(raw,4)==31&&word(raw,8)==newCount);
        juce::MemoryBlock section;const bool projected=legacyPersonalitySection(raw,section);CHECK(projected);if(!projected)return juce::MemoryBlock{};
        const auto length=std::size_t(word(raw,currentPersonalityLengthOffset));
        juce::MemoryBlock adjusted;juce::MemoryOutputStream out(adjusted,false);
        out.write(raw.getData(),currentPersonalityLengthOffset);out.write(section.getData(),section.getSize());
        out.write(static_cast<const char*>(raw.getData())+currentPersonalityLengthOffset+4+length,raw.getSize()-(currentPersonalityLengthOffset+4+length));out.flush();raw.swapWith(adjusted);
        raw.removeSection(raw.getSize()-36,4);
        raw.removeSection(16+8*newCount+78+64+oldCount,newCount-oldCount);
        raw.removeSection(16+8*oldCount,8*(newCount-oldCount));
        for(std::size_t slot=8;slot-->0;)raw.removeSection(20105+20+slot*19+16,3);
        put(raw,20105,144);static_cast<unsigned char*>(raw.getData())[20105+9]=16;
        put(raw,4,22);put(raw,8,std::uint32_t(oldCount));sign(raw);return raw;
    };
    const auto compare=[&](const juce::MemoryBlock& raw,const SoundDocument& sound){
        CHECK(word(raw,4)==22&&word(raw,8)==oldCount);
        for(std::size_t i=0;i<oldCount;++i){
            CHECK(word(raw,16+8*i)==registry[i].stateKey);
            CHECK(word(raw,20+8*i)==std::bit_cast<std::uint32_t>(sound.values[i]));
            CHECK(sound.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[locks+i]==1));
        }
        for(std::size_t i=0;i<8;++i){
            CHECK(word(raw,macros+8*i)==sound.macros[i].targetKey);
            CHECK(word(raw,macros+8*i+4)==std::bit_cast<std::uint32_t>(sound.macros[i].amount));
        }
        for(std::size_t i=oldCount;i<parameterCount;++i)CHECK(sound.values[i]==defaults.values[i]&&!sound.controlLocks[i]);
        juce::MemoryBlock upgraded;SoundDocument roundtrip;
        const bool embedded=bool(sound.personalities);std::size_t packageBytes=0;
        if(embedded){CHECK(sound.personalities->instances.size()==1);if(sound.personalities->instances.size()==1){
            const auto& ref=sound.personalities->instances[0].package;CHECK(ref.has_value());
            if(ref){CHECK(ref->id=="oxide-return");CHECK(ref->version=="1.0.0");CHECK(ref->sha256=="5a5081b0d1e6a2f013b28744af4c79fe1f08b11a4d8564f455145536c01401d0");
                packageBytes=1+3*4+ref->id.getNumBytesAsUTF8()+ref->version.getNumBytesAsUTF8()+ref->sha256.getNumBytesAsUTF8();CHECK(packageBytes==94);}
        }}
        CHECK(encodeSoundState(sound,upgraded));CHECK(upgraded.getSize()==raw.getSize()+(newCount-oldCount)*9+28+packageBytes);
        CHECK(word(upgraded,4)==31&&word(upgraded,8)==newCount);
        CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),roundtrip)&&roundtrip==sound);
        CHECK(asMain22(upgraded)==raw);
    };
    struct Fixture{const char* name;std::size_t size;const char* hash;};
    for(const auto& f:std::array<Fixture,3>{{
        {"default-v22.soundstate",20289,"2dafa869d9a21f7ba77d52d601a582c92843e1eb5b83e51808527e9e6a564451"},
        {"rich-v22.soundstate",20289,"39f6535709d3e435b8d669d82ba12037b191ebb06e7d2810dd5d4acb7abf438e"},
        {"embedded-v22.soundstate",23258,"646230850fceacc898376faeadd454c1e625cda1320f70364d2da33eae2f0ac2"}}}){
        const auto raw=load(f.name,f.size,f.hash);if(raw.getSize()!=f.size)continue;
        SoundDocument sound;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),sound);
        std::cout<<"MAIN22_READ "<<f.name<<" decoded="<<decoded<<'\n';CHECK(decoded);if(!decoded)continue;compare(raw,sound);
        auto out=sound;auto bad=raw;put(bad,16+8*(oldCount-1),registry[dynamicEqDetectorParameterStart].stateKey);sign(bad);
        CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==sound);
        bad=raw;put(bad,macros,registry[dynamicEqDetectorParameterStart+3].stateKey);sign(bad);
        CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==sound);
        bad=raw;static_cast<unsigned char*>(bad.getData())[locks+oldCount-1]=2;sign(bad);
        CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==sound);
        bad=raw;put(bad,8,2511);sign(bad);
        CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==sound);
    }
    for(const auto& f:std::array<Fixture,2>{{
        {"rich-v21.airackproject",20996,"de1f90d6fa02143c28249137a0a9a8a07d441971dcb7f854ca9f4fa9bc6150b0"},
        {"embedded-v21.airackproject",23404,"1e5e0ece3b0d3f672e10644f4941636621519ed57fee56f732da8b619fcac7ab"}}}){
        const auto raw=load(f.name,f.size,f.hash);if(raw.getSize()!=f.size)continue;
        ProjectDocument project;const bool decoded=decodeProjectState(raw.getData(),raw.getSize(),project);
        std::cout<<"MAIN21_READ "<<f.name<<" decoded="<<decoded<<'\n';CHECK(decoded);if(!decoded)continue;
        CHECK(word(raw,4)==21&&project.mode==PlaybackMode::internalPattern&&project.pattern.noteCount==2);
        CHECK((project.pattern.notes[0]==PatternNote{0,.125,65,91,1}));
        CHECK((project.pattern.notes[1]==PatternNote{.25,.25,72,83,2}));
        CHECK(project.group.has_value()==(juce::String(f.name)=="rich-v21.airackproject"));
        if(project.group)CHECK(groupTextView(project.group->name)=="Accepted stereo"&&groupTextView(project.group->role)=="lead");
        const juce::MemoryBlock sound(static_cast<const char*>(raw.getData())+20,word(raw,12));compare(sound,project.sound);
        juce::MemoryBlock upgraded;ProjectDocument roundtrip;
        CHECK(encodeProjectState(project,upgraded));
        const auto packageBytes=project.sound.personalities?std::size_t(94):std::size_t(0);
        CHECK(upgraded.getSize()==raw.getSize()+(newCount-oldCount)*9+28+packageBytes);CHECK(word(upgraded,4)==30);
        CHECK(decodeProjectState(upgraded.getData(),upgraded.getSize(),roundtrip)&&roundtrip==project);
    }
    if(parameterCount!=newCount)return;
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto contour=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});
    CHECK(contour!=factories.end());if(contour==factories.end())return;
    for(bool detectorDefault:{false,true}){
        auto design=*contour;design.id=juce::Uuid("bf380875-8e85-4281-951f-f2c0290308f6");design.name="Legacy domain guard";
        if(detectorDefault)design.units[0].defaults.push_back({0,"dynamic_eq.band.1.detector_low_hz",.2f});
        RackOperation place;place.kind=RackOperationKind::place;place.design=design;
        place.newInstance=juce::Uuid("57212c42-76f2-424d-83d0-d2e3bf068f9b");place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        SoundDocument nested;RackDifference delta;CHECK(makeRackCandidate(defaults,place,SoundEditOrigin::manual,nested,delta)==RackCandidateStatus::ready);
        juce::MemoryBlock encoded;CHECK(encodeSoundState(nested,encoded));const auto historical=asMain22(encoded);
        auto out=defaults;out.values[0]=.123f;const auto before=out;const bool accepted=decodeSoundState(historical.getData(),historical.getSize(),out);
        CHECK(accepted==!detectorDefault);if(detectorDefault)CHECK(out==before);else CHECK(out==nested);
    }
}
void spectralCatalogAndState()
{
    std::cout<<"SECTION spectral_append_mode_metadata_and_predecessor\n";
    const auto registry=parameterRegistry();
    const auto find=[&](const juce::String& id){return std::find_if(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;});};
    const auto mode=find("effect.8.dynamic_eq.processing_mode");CHECK(mode!=registry.end());
    if(mode!=registry.end())CHECK(!mode->automatable);
    for(const auto* key:{"spectral_enabled","spectral_density","spectral_tilt_db_oct"}){
        const auto control=find(juce::String("effect.8.dynamic_eq.band.8.")+key);CHECK(control!=registry.end());
        if(control!=registry.end())CHECK(control->automatable);
    }
    CHECK(parameterCount==4519);CHECK(rackUnitParameterCapacity(NativeModule::dynamicEq)==181);
    if(parameterCount!=4519)return;
    const auto directory=juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getParentDirectory().getChildFile("eq-spectral-legacy");
    const auto load=[&](const char* name){juce::MemoryBlock bytes;CHECK(directory.getChildFile(name).loadFileAsData(bytes));return bytes;};
    const auto catalog=load("registry-v23.json");const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(catalog.getData()),int(catalog.getSize())));
    CHECK(rows.isArray()&&rows.size()==2535);
    if(rows.size()==2535)for(int n=0;n<2535;++n){const auto& d=registry[std::size_t(n)];const auto row=rows[n];
        CHECK(d.id==row["id"].toString()&&d.name==row["name"].toString()&&d.unit==row["unit"].toString());
        CHECK(int(d.kind)==int(row["kind"])&&d.minimum==float(row["minimum"])&&d.maximum==float(row["maximum"])&&d.defaultValue==float(row["default"])&&d.skew==float(row["skew"])&&d.stateKey==std::uint32_t(juce::int64(row["key"])));
        CHECK(d.automatable);CHECK(d.choices.size()==std::size_t(row["choices"].size()));
        for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());
    }
    for(const auto* name:{"default-v23.soundstate","rich-v23.soundstate","default-main-v24.soundstate","named-main-v24.soundstate"}){
        const auto bytes=load(name);SoundDocument old;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),old));
        const auto defaults=defaultParameterValues();for(std::size_t i=2535;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
        if(juce::String(name).startsWith("named-"))CHECK(old.name==juce::String::fromUTF8("Precision caf\xc3\xa9"));
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(16);
        for(int n=0;n<2535;++n){const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();const auto i=parameterIndexForKey(key);CHECK(i&&*i<2535);if(i)CHECK(std::bit_cast<std::uint32_t>(old.values[*i])==std::bit_cast<std::uint32_t>(value));}
    }
    const auto oldProject=load("rich-v22.airackproject");ProjectDocument migrated;CHECK(decodeProjectState(oldProject.getData(),oldProject.getSize(),migrated));
    const auto namedProject=load("named-main-v23.airackproject");CHECK(decodeProjectState(namedProject.getData(),namedProject.getSize(),migrated));CHECK(migrated.sound.name==juce::String::fromUTF8("Precision caf\xc3\xa9"));
    SoundDocument sound;
    RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.destination=masterChain;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(sound.layout,add,sound.layout)==RackEditResult::applied);
    const auto base=dynamicEqSpectralParameterStart+7*dynamicEqSpectralParametersPerSlot;
    sound.values[base]=1;sound.values[base+1+7*3]=1;sound.values[base+2+7*3]=.75f;sound.values[base+3+7*3]=.25f;
    RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));CHECK(patch.effects[7].dynamicEq.processingMode==DynamicEqProcessingMode::spectral);
    const auto& band=patch.effects[7].dynamicEq.bands[7];CHECK(band.spectralEnabled&&band.spectralDensity==.75f&&band.spectralTiltDbOct==-3);
    juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));SoundDocument decoded;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),decoded)&&decoded==sound);
    juce::MemoryInputStream header(bytes,false);header.skipNextBytes(4);CHECK(header.readInt()==31);CHECK(bytes.getSize()==41053);
    ProjectDocument project;project.sound=sound;CHECK(encodeProjectState(project,bytes));ProjectDocument restored;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),restored)&&restored==project);
    const auto reach=unitReach(NativeModule::dynamicEq,7);CHECK(reach.size()==181);
    for(std::size_t i=base;i<base+25;++i){CHECK(std::find(reach.begin(),reach.end(),i)!=reach.end());
        auto locked=sound;locked.effectLocks[7]=true;auto edited=locked;edited.values[i]=edited.values[i]==1?0.f:1.f;CHECK(!validateAiSoundEdit(locked,edited));
    }
    auto invalid=sound;invalid.values[dynamicEqDetectorParameterStart+7*40+7*5+2]=1;CHECK(!validateSoundDocument(invalid));
    invalid.values[base]=0;CHECK(validateSoundDocument(invalid)); // Retained detector intent survives Ordinary mode.
    EqBandPatch copy;CHECK(captureEqBand(sound,7,7,copy));CHECK(copy.fields.size()==22);
    juce::String clipboard;CHECK(encodeEqClipboard(std::span(&copy,1),clipboard));std::vector<EqBandPatch> recalled;
    CHECK(decodeEqClipboard(clipboard,recalled));CHECK(recalled.size()==1&&recalled[0]==copy);
}
int main(int argc,char** argv)
{
    if(argc==2&&juce::String(argv[1])=="--catalog-capacity"){fullInventory();fullInventory(true,true);fullInventory(true);aiBoundaries();std::cout<<"Catalog capacity checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--spectral-state"){spectralCatalogAndState();std::cout<<"Spectral rack checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--accepted-main22"){acceptedMainStereo22Migration();std::cout<<"EQ main22 migration: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--detectors"){detectorControlsAreRegisteredForTheLastBand();detectorCatalogStateAndAdmission();detectorClipboardAndSnapshots();detectorAiEditIsTransactional();std::cout<<"EQ detector rack: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--merged-formats"){acceptedDelayAndDraftEqMigration();std::cout<<"EQ merged formats: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--extension-clipboard"){extensionClipboardAndSnapshots();std::cout<<"EQ extension clipboard: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--extensions"){extensionSelectorsPreserveLegacyAutomation();std::cout<<"EQ extension rack: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[1])=="--capture-shapes-baseline")return captureComparisonBaseline(juce::File(juce::String::fromUTF8(argv[2])),true,true)?0:1;
    if(argc==2&&juce::String(argv[1])=="--placement"){placementCatalogAndFrozenPredecessor();placementStateDomainsAndClipboard();placementSketchUsesActualEnergy();std::cout<<"EQ placement rack: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--sketch"){sketchChecks();std::cout<<"EQ Sketch fit: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[1])=="--capture-placement-baseline")return captureComparisonBaseline(juce::File(juce::String::fromUTF8(argv[2])),true)?0:1;
    if(argc==3&&juce::String(argv[1])=="--capture-comparison-baseline")return captureComparisonBaseline(juce::File(juce::String::fromUTF8(argv[2])))?0:1;
    if(argc==3&&juce::String(argv[1])=="--capture-old-library")return captureOld(juce::File(juce::String::fromUTF8(argv[2])))?0:1;
    if(argc!=1)return 2;
    spectralCatalogAndState();acceptedMainStereo22Migration();detectorControlsAreRegisteredForTheLastBand();
    detectorCatalogStateAndAdmission();detectorClipboardAndSnapshots();detectorAiEditIsTransactional();
    acceptedDelayAndDraftEqMigration();extensionClipboardAndSnapshots();extensionSelectorsPreserveLegacyAutomation();placementCatalogAndFrozenPredecessor();placementStateDomainsAndClipboard();placementSketchUsesActualEnergy();originalLibrary();moduleAndFactory();fullInventory();fullInventory(true,true);fullInventory(true);aiBoundaries();nativeProposals();rackOperations();eqPatchClipboardRoundTrip();eqPatchesAndPasteAreAtomic();comparisonSnapshotIsIndependent();gainControlCatalogAndFrozenPrefix();gainStateAndNativeOldMigration();sketchChecks();std::cout<<"Dynamic EQ rack: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
