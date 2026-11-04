#include "LegacyPersonalityProjection.h"
#include "FrozenRackExamples.h"
#include "ProjectDocument.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "UserReach.h"
#include "RackLibrary.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
namespace {
using namespace airack;
constexpr std::size_t predecessor=2023,added=280,expected=4519,perSlot=35;
std::size_t checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL:"<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
juce::MemoryBlock fixture(const juce::String& name){juce::MemoryBlock bytes;CHECK(juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getChildFile(name).loadFileAsData(bytes));return bytes;}
juce::var json(const char* name){const auto bytes=fixture(name);return juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())));}
void sign(juce::MemoryBlock& bytes){if(bytes.getSize()<32)return;const auto digest=juce::SHA256(bytes.getData(),bytes.getSize()-32).getRawData();std::memcpy(static_cast<char*>(bytes.getData())+bytes.getSize()-32,digest.getData(),32);}
void integer(juce::MemoryBlock& bytes,int offset,int value){juce::MemoryOutputStream stream(bytes,true);CHECK(stream.setPosition(offset));stream.writeInt(value);}
int integer(const juce::MemoryBlock& bytes,int offset){juce::MemoryInputStream stream(bytes,false);stream.setPosition(offset);return stream.readInt();}
void number(juce::MemoryBlock& bytes,int offset,float value){juce::MemoryOutputStream stream(bytes,true);CHECK(stream.setPosition(offset));stream.writeFloat(value);}
void reject(const juce::MemoryBlock& bytes){SoundDocument output;output.values[0]=.17f;const auto before=output;CHECK(!decodeSoundState(bytes.getData(),bytes.getSize(),output));CHECK(output==before);}
std::optional<std::size_t> index(const juce::String& id){const auto r=parameterRegistry();for(std::size_t i=0;i<r.size();++i)if(r[i].id==id)return i;return {};}
void prefix(){
    const auto rows=json("registry-predecessor.json");const auto registry=parameterRegistry();CHECK(rows.isArray()&&rows.size()==int(predecessor));CHECK(registry.size()==4239+added);
    if(!rows.isArray()||rows.size()!=int(predecessor)||registry.size()<predecessor)return;
    for(int i=0;i<rows.size();++i){const auto& d=registry[std::size_t(i)];const auto row=rows[i];CHECK(d.id==row["id"].toString());CHECK(d.name==row["name"].toString());CHECK(d.unit==row["unit"].toString());CHECK(int(d.kind)==int(row["kind"]));CHECK(d.minimum==float(row["minimum"]));CHECK(d.maximum==float(row["maximum"]));CHECK(d.defaultValue==float(row["default"]));CHECK(d.skew==float(row["skew"]));CHECK(d.stateKey==std::uint32_t(juce::int64(row["key"])));CHECK(d.choices.size()==std::size_t(row["choices"].size()));for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());}
    std::set<std::uint32_t> keys;for(const auto& d:registry)CHECK(keys.insert(d.stateKey).second);
    const auto manifest=json("manifest.json");CHECK(int(manifest["parameters"])==2023&&int(manifest["sound_version"])==19&&int(manifest["project_version"])==18&&int(manifest["effect_banks"])==16);
    const auto outputs=manifest["outputs"];CHECK(outputs.isArray());for(int i=0;i<outputs.size();++i){const auto bytes=fixture(outputs[i]["file"].toString());CHECK(juce::int64(bytes.getSize())==juce::int64(outputs[i]["bytes"]));CHECK(juce::SHA256(bytes).toHexString()==outputs[i]["sha256"].toString());}
}
void geometry(){
    CHECK(parameterCount==expected&&effectBankCount==19);CHECK(rackUnitParameterCapacity(NativeModule::saturation)==36);CHECK(moduleDisplayName(NativeModule::saturation)=="Saturation");CHECK(effectDisplayName(EffectKind::saturation)=="Saturation");
    const char* globals[]{"mode","input_trim_db","output_trim_db","mix","drive_compensation","low_crossover_hz","high_crossover_hz","pre_low_cut_enabled","pre_low_cut_hz","post_high_cut_enabled","post_high_cut_hz"};
    const char* lanes[]{"full","low","mid","high"};const char* fields[]{"bypassed","character","drive_db","pre_tilt_db","mix","trim_db"};
    for(std::size_t slot=0;slot<8;++slot){RackLayout layout;RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=std::uint8_t(slot);edit.effect=EffectKind::saturation;CHECK(editRackLayout(layout,edit,layout)==RackEditResult::applied);std::size_t local=0;
        const auto test=[&](const juce::String& key){const auto found=rackParameterIndex(NativeModule::saturation,std::uint8_t(slot),key);CHECK(found&&*found==saturationParameterStart+slot*perSlot+local);CHECK(rackParameterActive(NativeModule::saturation,key));++local;};
        for(const auto* key:globals)test("saturation."+juce::String(key));for(const auto* lane:lanes)for(const auto* key:fields)test("saturation."+juce::String(lane)+"."+key);
        const auto bypass=rackParameterIndex(NativeModule::saturation,std::uint8_t(slot),"bypassed");CHECK(bypass&&*bypass==572+slot*23);CHECK(local==35);
    }
    if(parameterCount!=expected)return;
    const auto registry=parameterRegistry();for(std::size_t slot=0;slot<8;++slot){CHECK(registry[saturationParameterStart+slot*35].choices==std::vector<juce::String>({"Full band","Three band"}));for(std::size_t lane=0;lane<4;++lane)CHECK(registry[saturationParameterStart+slot*35+12+lane*6].choices==std::vector<juce::String>({"Soft","Glow","Dense"}));}
    struct Expected {const char* key;const char* unit;float minimum,maximum,initial,skew;int choices;};
    const Expected expectedFields[]{
        {"saturation.mode","",0.f,1.f,0.f,1.f,2},
        {"saturation.input_trim_db","dB",-24.f,24.f,0.f,1.f,0},
        {"saturation.output_trim_db","dB",-24.f,12.f,0.f,1.f,0},
        {"saturation.mix","",0.f,1.f,1.f,1.f,0},
        {"saturation.drive_compensation","",0.f,1.f,1.f,1.f,2},
        {"saturation.low_crossover_hz","Hz",40.f,2000.f,180.f,.35f,0},
        {"saturation.high_crossover_hz","Hz",500.f,16000.f,3500.f,.35f,0},
        {"saturation.pre_low_cut_enabled","",0.f,1.f,0.f,1.f,2},
        {"saturation.pre_low_cut_hz","Hz",20.f,2000.f,20.f,.35f,0},
        {"saturation.post_high_cut_enabled","",0.f,1.f,0.f,1.f,2},
        {"saturation.post_high_cut_hz","Hz",1000.f,20000.f,20000.f,.35f,0},
        {"saturation.full.bypassed","",0.f,1.f,0.f,1.f,2},
        {"saturation.full.character","",0.f,2.f,0.f,1.f,3},
        {"saturation.full.drive_db","dB",0.f,36.f,0.f,1.f,0},
        {"saturation.full.pre_tilt_db","dB",-6.f,6.f,0.f,1.f,0},
        {"saturation.full.mix","",0.f,1.f,1.f,1.f,0},
        {"saturation.full.trim_db","dB",-18.f,18.f,0.f,1.f,0},
        {"saturation.low.bypassed","",0.f,1.f,0.f,1.f,2},
        {"saturation.low.character","",0.f,2.f,0.f,1.f,3},
        {"saturation.low.drive_db","dB",0.f,36.f,0.f,1.f,0},
        {"saturation.low.pre_tilt_db","dB",-6.f,6.f,0.f,1.f,0},
        {"saturation.low.mix","",0.f,1.f,1.f,1.f,0},
        {"saturation.low.trim_db","dB",-18.f,18.f,0.f,1.f,0},
        {"saturation.mid.bypassed","",0.f,1.f,0.f,1.f,2},
        {"saturation.mid.character","",0.f,2.f,0.f,1.f,3},
        {"saturation.mid.drive_db","dB",0.f,36.f,0.f,1.f,0},
        {"saturation.mid.pre_tilt_db","dB",-6.f,6.f,0.f,1.f,0},
        {"saturation.mid.mix","",0.f,1.f,1.f,1.f,0},
        {"saturation.mid.trim_db","dB",-18.f,18.f,0.f,1.f,0},
        {"saturation.high.bypassed","",0.f,1.f,0.f,1.f,2},
        {"saturation.high.character","",0.f,2.f,0.f,1.f,3},
        {"saturation.high.drive_db","dB",0.f,36.f,0.f,1.f,0},
        {"saturation.high.pre_tilt_db","dB",-6.f,6.f,0.f,1.f,0},
        {"saturation.high.mix","",0.f,1.f,1.f,1.f,0},
        {"saturation.high.trim_db","dB",-18.f,18.f,0.f,1.f,0}
    };
    const auto defaults=defaultParameterValues();
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<35;++field){
        const auto i=saturationParameterStart+slot*35+field;const auto& d=registry[i];const auto& f=expectedFields[field];
        CHECK(d.id=="effect."+juce::String(slot+1)+"."+f.key);CHECK(d.unit==f.unit);
        CHECK(d.minimum==f.minimum&&d.maximum==f.maximum&&d.defaultValue==f.initial&&d.skew==f.skew);
        CHECK(d.choices.size()==std::size_t(f.choices));CHECK(std::abs(parameterPhysicalValue(defaults,i)-f.initial)<.001f);
        CHECK(isContinuousParameter(i)==(f.choices==0));
    }
    SoundDocument sound;juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));CHECK(integer(bytes,4)==31&&integer(bytes,8)==4519&&bytes.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);
    ProjectDocument p;CHECK(encodeProjectState(p,bytes));CHECK(static_cast<const unsigned char*>(bytes.getData())[4]==30&&integer(bytes,24)==31&&bytes.getSize()==41161);ProjectDocument q;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),q)&&q==p);
}
void migration(){
    const auto defaults=defaultParameterValues();
    for(const auto* stem:{"default","rich","inactive","embedded"}){const auto name=juce::String(stem)+"-predecessor";const auto bytes=fixture(name+".soundstate");SoundDocument decoded;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),decoded));if(bytes.getSize()<18561)continue;
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(16);for(std::size_t i=0;i<predecessor;++i){const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();const auto found=parameterIndexForKey(key);CHECK(found&&*found<predecessor);if(found)CHECK(std::bit_cast<std::uint32_t>(decoded.values[*found])==std::bit_cast<std::uint32_t>(value));}
        for(std::size_t i=predecessor;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]&&!decoded.controlLocks[i]);for(const auto& bank:decoded.compatibility.effects)if(bank.size()>16)CHECK(bank[16]==1);
        for(const auto& slot:decoded.layout.effects)CHECK(unsigned(slot.kind)<=15);
        juce::MemoryBlock modern;CHECK(encodeSoundState(decoded,modern));SoundDocument repeat;CHECK(decodeSoundState(modern.getData(),modern.getSize(),repeat)&&repeat==decoded);
        const auto project=fixture(name+".airackproject");ProjectDocument p;CHECK(decodeProjectState(project.getData(),project.getSize(),p)&&p.sound==decoded);CHECK(encodeProjectState(p,modern));ProjectDocument r;CHECK(decodeProjectState(modern.getData(),modern.getSize(),r)&&r==p);
    }
    const auto factories=json("factories-predecessor.json");auto current=airack::test::frozenFactoryRackDesigns();
    CHECK(current.size()==21);CHECK(factories.size()==21);
    for(int i=0;i<18&&std::size_t(i)<current.size();++i)CHECK(current[std::size_t(i)].id==juce::Uuid(factories[i]["id"].toString()));
    CHECK(current[19].id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf01")&&current[19].name=="Character Drive");
    for(const auto& d:airack::test::frozenExampleRackDesigns())current.push_back(d);
    for(int i=0;i<factories.size();++i){const auto id=juce::Uuid(factories[i]["id"].toString());const auto found=std::find_if(current.begin(),current.end(),[&](const auto& d){return d.id==id;});CHECK(found!=current.end());if(found==current.end())continue;juce::MemoryBlock bytes;CHECK(encodeRackDesign(*found,bytes));CHECK(juce::SHA256(bytes).toHexString()==factories[i]["sha256"].toString());const auto frozen=fixture(factories[i]["file"].toString());CHECK(bytes==frozen);}
}
void malformedAndLocks(){
    if(parameterCount!=expected)return;
    SoundDocument source;juce::MemoryBlock good;CHECK(encodeSoundState(source,good));
    // Every retained bank is validated even while the layout contains no effect.
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t field=0;field<35;++field)for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-0.0001f,1.0001f}){const auto i=saturationParameterStart+slot*35+field;auto d=source;d.values[i]=bad;juce::MemoryBlock untouched("hold",4);CHECK(!encodeSoundState(d,untouched)&&untouched==juce::MemoryBlock("hold",4));auto bytes=good;number(bytes,int(16+i*8+4),bad);sign(bytes);reject(bytes);}
        auto d=source;d.values[saturationParameterStart+slot*35+12]=.25f;CHECK(!validateSoundDocument(d));
        d=source;d.compatibility.effects[slot][saturationEffectBank]=2;juce::MemoryBlock untouched("hold",4);CHECK(!encodeSoundState(d,untouched)&&untouched==juce::MemoryBlock("hold",4));
        auto bytes=good;static_cast<unsigned char*>(bytes.getData())[currentSoundNumericBytes+4+8+8+slot*effectBankCount+saturationEffectBank]=2;sign(bytes);reject(bytes);
        auto locked=source;locked.effectLocks[slot]=true;auto edited=locked;edited.values[saturationParameterStart+slot*35+13]=.5f;CHECK(!validateAiSoundEdit(locked,edited));auto independent=locked;independent.values[saturationParameterStart+((slot+1)%8)*35+13]=.5f;CHECK(validateAiSoundEdit(locked,independent));
        locked=source;locked.controlLocks[saturationParameterStart+slot*35+13]=true;edited=locked;edited.values[saturationParameterStart+slot*35+13]=.5f;CHECK(!validateAiSoundEdit(locked,edited));
        d=source;d.macros[0]={parameterRegistry()[saturationParameterStart+slot*35+13].stateKey,.25f};CHECK(validateSoundDocument(d));d.macros[0].targetKey=parameterRegistry()[saturationParameterStart+slot*35].stateKey;CHECK(!validateSoundDocument(d));
    }
    auto bytes=good;integer(bytes,24,integer(bytes,16));sign(bytes);reject(bytes);std::uint32_t unknown=1;while(parameterIndexForKey(unknown))++unknown;bytes=good;integer(bytes,16,std::bit_cast<int>(unknown));sign(bytes);reject(bytes);
    bytes=good;static_cast<unsigned char*>(bytes.getData())[bytes.getSize()-1]^=1;reject(bytes);
    auto old=fixture("default-predecessor.soundstate");const auto satKey=parameterRegistry()[saturationParameterStart].stateKey;bytes=old;integer(bytes,16,std::bit_cast<int>(satKey));sign(bytes);reject(bytes);
    bytes=old;integer(bytes,int(16+predecessor*8+78),std::bit_cast<int>(parameterRegistry()[saturationParameterStart+1].stateKey));sign(bytes);reject(bytes);
    bytes=old;static_cast<unsigned char*>(bytes.getData())[16+predecessor*8+10]=16;sign(bytes);reject(bytes);
    ProjectDocument p;CHECK(encodeProjectState(p,bytes));static_cast<unsigned char*>(bytes.getData())[4]=18;sign(bytes);ProjectDocument out;out.sound.values[0]=.21f;const auto before=out;CHECK(!decodeProjectState(bytes.getData(),bytes.getSize(),out)&&out==before);
    auto embeddedBytes=fixture("embedded-predecessor.soundstate");SoundDocument embedded;CHECK(decodeSoundState(embeddedBytes.getData(),embeddedBytes.getSize(),embedded));if(embedded.personalities&&!embedded.personalities->instances.empty()){
        juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(embeddedBytes,18525,embedded,control));CHECK(control==embeddedBytes);SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==embedded);
        auto personalities=std::make_shared<RackPersonalityState>(*embedded.personalities);auto& defaults=personalities->instances.back().design.units.front().defaults;CHECK(!defaults.empty());if(!defaults.empty()){defaults.back().key="saturation.full.drive_db";defaults.back().normalized=.5f;embedded.personalities=personalities;juce::MemoryBlock modern;CHECK(encodeSoundState(embedded,modern));if(modern.getSize()>currentPersonalityLengthOffset){juce::MemoryBlock forged;CHECK(airack::test::oldEnvelopeWithPersonality(embeddedBytes,18525,embedded,forged));reject(forged);}}
    }
}
void nestedNewModule(){
    if(parameterCount!=expected)return;
    const auto old=fixture("embedded-predecessor.soundstate");SoundDocument d;CHECK(decodeSoundState(old.getData(),old.getSize(),d));
    if(!d.personalities||d.personalities->instances.empty())return;
    juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(old,18525,d,control));CHECK(control==old);SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==d);
    auto personalities=std::make_shared<RackPersonalityState>(*d.personalities);auto& instance=personalities->instances.back();
    if(instance.design.units.empty())return;auto& unit=instance.design.units.front();const auto* binding=rackUnitBinding(instance,unit.id);CHECK(binding);if(!binding)return;
    unit.module=NativeModule::saturation;unit.algorithmRevision=1;unit.defaults={{unit.id,"saturation.full.drive_db",.5f}};instance.design.panel.controls.clear();instance.edited=true;
    d.layout.effects[binding->slot].kind=EffectKind::saturation;d.compatibility.effects[binding->slot][saturationEffectBank]=1;d.personalities=personalities;
    juce::MemoryBlock modern;CHECK(encodeSoundState(d,modern));if(modern.getSize()<41053||old.getSize()<18561)return;
    juce::MemoryBlock forged;CHECK(airack::test::oldEnvelopeWithPersonality(old,18525,d,forged));reject(forged);
}

void predecessorCarriers(){
    const auto state=fixture("rich-predecessor.soundstate");SoundDocument sound;CHECK(decodeSoundState(state.getData(),state.getSize(),sound));
    RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
    for(int block:{64,512}){const auto frozen=fixture("rich-predecessor-48k-"+juce::String(block)+".raw");CHECK(frozen.getSize()==16+12288*2*sizeof(float));if(frozen.getSize()!=16+12288*2*sizeof(float))continue;
        juce::MemoryInputStream stream(frozen,false);CHECK(stream.readInt()==0x30465241&&stream.readInt()==48000&&stream.readInt()==12288&&stream.readInt()==block);
        RoutedSynthEngine engine;CHECK(engine.prepare(48000,512)&&engine.setPatch(patch));engine.reset();juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(256);
        for(int first=0;first<12288;first+=block){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(first==4096)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);engine.render(audio,midi,120.);
            for(int n=0;n<block;++n)for(int ch=0;ch<2;++ch)CHECK(std::bit_cast<std::uint32_t>(audio.getSample(ch,n))==std::bit_cast<std::uint32_t>(stream.readFloat()));}
        CHECK(stream.isExhausted());
    }
}
void bandColorFactory(){
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto examples=airack::test::frozenExampleRackDesigns();
    CHECK(factories.size()==21&&airack::test::frozenLibraryRackDesigns().size()==28&&parameterCount==4519);
    const auto fixtureRoot=juce::File(RACKSYNTH_FIXTURE_DIR);
    const auto packet=fixtureRoot.getParentDirectory().getChildFile("rack-library/v1/historical");
    const auto rows=juce::JSON::parse(fixtureRoot.getChildFile("band-color-predecessor-manifest.json"));CHECK(rows.isArray()&&rows.size()==22);
    for(int n=0;n<rows.size();++n){const auto row=rows[n];const int position=row["position"];
        const auto& designs=row["kind"].toString()=="factory"?factories:std::vector<RackDesign>(examples.begin(),examples.end());
        CHECK(position>=0&&std::size_t(position)<designs.size());if(position<0||std::size_t(position)>=designs.size())continue;
        const auto saved=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==juce::Uuid(row["id"].toString());});CHECK(saved!=designs.end());if(saved==designs.end())continue;
        const auto& d=*saved;juce::MemoryBlock actual,frozen;
        CHECK(d.id==juce::Uuid(row["id"].toString()));CHECK(encodeRackDesign(d,actual));
        CHECK(packet.getChildFile(row["sha256"].toString()+".json").loadFileAsData(frozen));CHECK(juce::int64(frozen.getSize())==juce::int64(row["bytes"]));
        if(d.units[0].module==NativeModule::saturation){
            RackDesign retained;CHECK(decodeRackDesign(frozen.getData(),frozen.getSize(),retained));CHECK(retained.revision==1&&d.revision==2);
            auto unchanged=retained;unchanged.intent=d.intent;unchanged.revision=d.revision;CHECK(unchanged==d);
            juce::MemoryBlock oldBytes;CHECK(encodeRackDesign(retained,oldBytes)&&oldBytes==frozen);CHECK(juce::SHA256(oldBytes).toHexString()==row["sha256"].toString());
        }else {CHECK(actual==frozen);CHECK(juce::SHA256(actual).toHexString()==row["sha256"].toString());}
    }
    const auto id=juce::Uuid("91387715-13d3-4c57-a479-12251842bf02");
    CHECK(std::count_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==id;})==1);
    const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==id;});
    CHECK(found!=factories.end());if(found==factories.end())return;const auto& d=*found;
    CHECK(found==factories.begin()+20&&factories[19].name=="Character Drive");
    CHECK(d.name=="Band Color"&&d.revision==2&&d.role==RackRole::fx&&d.units.size()==1);
    CHECK(!d.intent.containsIgnoreCase("pending")&&d.intent.containsIgnoreCase("Three-band saturation"));
    CHECK(validateRackDesign(d,RackValidationMode::portableDesign));juce::MemoryBlock wire;RackDesign decoded;
    CHECK(encodeRackDesign(d,wire)&&decodeRackDesign(wire.getData(),wire.getSize(),decoded)&&decoded==d);
    if(d.units.size()!=1)return;const auto& unit=d.units[0];
    CHECK(unit.id==0&&unit.module==NativeModule::saturation&&unit.algorithmRevision==1&&unit.defaults.size()==36);
    std::set<juce::String> keys;for(const auto& v:unit.defaults){CHECK(v.unit==0&&keys.insert(v.key).second);CHECK(rackParameterIndex(unit.module,0,v.key).has_value());}
    const auto expect=[&](const juce::String& key,float physical){const auto i=rackParameterIndex(unit.module,0,key);CHECK(i);if(!i)return;
        const auto v=std::find_if(unit.defaults.begin(),unit.defaults.end(),[&](const auto& p){return p.key==key;});CHECK(v!=unit.defaults.end());
        if(v!=unit.defaults.end())CHECK(std::abs(juce::NormalisableRange<float>(parameterRegistry()[*i].minimum,parameterRegistry()[*i].maximum,0,parameterRegistry()[*i].skew).convertFrom0to1(v->normalized)-physical)<.001f);};
    expect("bypassed",1);expect("saturation.mode",1);expect("saturation.input_trim_db",0);expect("saturation.output_trim_db",0);
    expect("saturation.mix",1);expect("saturation.drive_compensation",1);expect("saturation.low_crossover_hz",180);expect("saturation.high_crossover_hz",3500);
    expect("saturation.pre_low_cut_enabled",0);expect("saturation.pre_low_cut_hz",20);expect("saturation.post_high_cut_enabled",0);expect("saturation.post_high_cut_hz",20000);
    for(const auto* lane:{"full","low","mid","high"}){const juce::String prefix="saturation."+juce::String(lane)+".";const bool low=juce::String(lane)=="low",mid=juce::String(lane)=="mid",high=juce::String(lane)=="high";
        expect(prefix+"bypassed",low?1.f:0.f);expect(prefix+"character",mid?2.f:0.f);expect(prefix+"drive_db",mid?9.f:high?6.f:0.f);
        expect(prefix+"pre_tilt_db",high?-1.f:0.f);expect(prefix+"mix",mid||high?.5f:1.f);expect(prefix+"trim_db",0);}
    CHECK(d.panel.layout==PanelLayout::orderedFxStrip&&d.panel.sections==std::vector<juce::String>({"Split","Mid","High","Blend"}));
    const char* labels[]{"Low crossover","High crossover","Mid Drive","Mid Mix","High Drive","High Mix","High Tilt","Global Mix"};
    const char* focusKeys[]{"saturation.low_crossover_hz","saturation.high_crossover_hz","saturation.mid.drive_db","saturation.mid.mix","saturation.high.drive_db","saturation.high.mix","saturation.high.pre_tilt_db","saturation.mix"};
    const std::uint8_t sections[]{0,0,1,1,2,2,2,3};CHECK(d.panel.controls.size()==8);
    for(std::size_t n=0;n<d.panel.controls.size()&&n<8;++n){const auto& c=d.panel.controls[n];const auto i=rackParameterIndex(unit.module,0,focusKeys[n]);CHECK(i);if(!i)continue;const auto& descriptor=parameterRegistry()[*i];
        CHECK(c.label==labels[n]&&c.key==focusKeys[n]&&c.unit==0&&c.section==sections[n]&&c.widget==FocusWidget::rotary);
        CHECK(c.minimum==descriptor.minimum&&c.maximum==descriptor.maximum&&c.response==(n<2?FocusResponse::logarithmic:FocusResponse::linear));}
    for(std::uint8_t slot=0;slot<8;++slot){SoundDocument source,next;RackOperation place;place.design=d;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,slot}};RackDifference delta;
        CHECK(makeRackCandidate(source,place,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);CHECK(next.layout.effects[slot].kind==EffectKind::saturation);
        const auto reach=userReach(next);for(const auto i:unitReach(unit.module,slot))CHECK(reach[i]);
        for(const auto& v:unit.defaults){const auto i=rackParameterIndex(unit.module,slot,v.key);CHECK(i&&next.values[*i]==v.normalized);}
        RoutedPatch patch;CHECK(makeRoutedPatch(next,patch)&&patch.effects[slot].bypassed);
        next.values[*rackParameterIndex(unit.module,slot,"bypassed")]=0;CHECK(makeRoutedPatch(next,patch)&&!patch.effects[slot].bypassed);
        juce::MemoryBlock sound;SoundDocument recalled;CHECK(encodeSoundState(next,sound)&&decodeSoundState(sound.getData(),sound.getSize(),recalled)&&recalled==next);
        ProjectDocument project,loaded;project.sound=next;CHECK(encodeProjectState(project,sound)&&decodeProjectState(sound.getData(),sound.getSize(),loaded)&&loaded==project);
        RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;duplicate.newInstance=juce::Uuid();duplicate.destination={RackRole::fx,masterChain,1};duplicate.allocation={{0,std::uint8_t((slot+1)%8)}};
        SoundDocument copy;CHECK(makeRackCandidate(next,duplicate,SoundEditOrigin::manual,copy,delta)==RackCandidateStatus::ready);
        CHECK(copy.personalities&&copy.personalities->instances.size()==2&&copy.personalities->instances[1].design==d);
    }
}
void mixedFactoryRevisions(){
    const auto root=juce::File(RACKSYNTH_FIXTURE_DIR).getSiblingFile("saturation-release-20261026");
    const auto factories=airack::test::frozenFactoryRackDesigns();std::vector<RackDesign> old;
    const char* names[]{"character-drive-revision-1.rackdesign","band-color-revision-1.rackdesign"};
    const char* hashes[]{"c55cee48271b8f402a05b903d0a16dca99beb9d1b5b2141c580670ac38fb804d","2ecfd2a23a613dde5d8d895754f478124d2c5c0174990dc7edaab37ed0be3d5d"};
    for(unsigned n=0;n<2;++n){juce::MemoryBlock bytes;CHECK(root.getChildFile(names[n]).loadFileAsData(bytes));CHECK(juce::SHA256(bytes).toHexString()==hashes[n]);RackDesign decoded;
        CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),decoded));CHECK(decoded.id==factories[19+n].id&&decoded.revision==1);auto amended=decoded;amended.revision=2;amended.intent=factories[19+n].intent;CHECK(amended==factories[19+n]);old.push_back(decoded);}
    RackCatalogSnapshot catalog;RackCatalogStatus status;CHECK(captureRackCatalog(factories,old,{},catalog,&status)&&status==RackCatalogStatus::ready);
    for(const auto& design:old){const auto found=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& e){return e.definition.id==design.id&&e.definition.revision==1;});CHECK(found!=catalog.entries.end());
        if(found!=catalog.entries.end()){RackDesign resolved;CHECK(resolveCatalogDesign(catalog,design.id,1,found->contentSha256,resolved,&status)&&resolved==design);}}
    SoundDocument sound;RackDifference delta;
    for(unsigned n=0;n<4;++n){RackOperation place;place.design=n<2?old[n]:factories[19+n-2];place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,std::uint8_t(n)};place.allocation={{0,std::uint8_t(n)}};SoundDocument next;
        CHECK(makeRackCandidate(sound,place,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);sound=next;}
    juce::MemoryBlock bytes;SoundDocument recalled;CHECK(encodeSoundState(sound,bytes)&&decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);
    ProjectDocument project,loaded;project.sound=sound;CHECK(encodeProjectState(project,bytes)&&decodeProjectState(bytes.getData(),bytes.getSize(),loaded)&&loaded==project);
}
}
int main(){bandColorFactory();mixedFactoryRevisions();prefix();geometry();migration();malformedAndLocks();nestedNewModule();predecessorCarriers();std::cout<<"SATURATION_STATE checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
