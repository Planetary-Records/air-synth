#include "LegacyPersonalityProjection.h"
// The 2063-control capture remains historical; accepted main appends precede Space.
#include "SoundDocument.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstring>
#include "SpaceReverbControls.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <vector>
#ifndef RACKSYNTH_SPACE_BASELINE_DIR
#define RACKSYNTH_SPACE_BASELINE_DIR ""
#endif

namespace {
using namespace airack;
int checks=0,failures=0;
void check(bool good,const char* expression,int line) {
    ++checks;if(!good){++failures;if(failures<90)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}
}
#define CHECK(x) check(bool(x),#x,__LINE__)
constexpr std::size_t frozenBaseline=2063, currentPreSpace=2735, fieldsPerSlot=96;
void frozenRegistryPrefix() {
    std::cout<<"SECTION exact_frozen_published_registry_prefix\n";
    const auto file=juce::File(juce::String(RACKSYNTH_SPACE_BASELINE_DIR)).getChildFile("registry-v20.json");
    CHECK(file.existsAsFile());if(!file.existsAsFile())return;
    const auto rows=juce::JSON::parse(file);CHECK(rows.isArray());
    CHECK(rows.size()==int(frozenBaseline));const auto registry=parameterRegistry();
    CHECK(registry.size()>=frozenBaseline);
    if(!rows.isArray()||rows.size()!=int(frozenBaseline)||registry.size()<frozenBaseline)return;
    for(std::size_t i=0;i<frozenBaseline;++i){const auto row=rows[int(i)];const auto& d=registry[i];
        CHECK(row["id"].toString()==d.id);CHECK(row["name"].toString()==d.name);CHECK(row["unit"].toString()==d.unit);
        CHECK(int(row["kind"])==int(d.kind));CHECK(float(row["minimum"])==d.minimum);
        CHECK(float(row["maximum"])==d.maximum);CHECK(float(row["default"])==d.defaultValue);
        CHECK(float(row["skew"])==d.skew);CHECK(juce::int64(row["key"])==juce::int64(d.stateKey));
        const auto choices=row["choices"];CHECK(choices.isArray());CHECK(choices.size()==int(d.choices.size()));
        for(int c=0;c<std::min(choices.size(),int(d.choices.size()));++c)CHECK(choices[c].toString()==d.choices[std::size_t(c)]);
    }
}
struct ExpectedField {juce::String key;float minimum,maximum,initial,skew;int choices;};
std::vector<ExpectedField> expectedFields() {
    std::vector<ExpectedField> out{
        {"size",0,1,.5f,1,0},{"decay_seconds",.1f,20,2,.4f,0},
        {"predelay_ms",0,250,20,1,0},{"brightness",0,1,.5f,1,0},
        {"character",0,1,.25f,1,0},{"distance",0,1,.5f,1,0},
        {"width",0,2,1,1,0},{"mix",0,1,.25f,1,0},
        {"duck_range_db",0,24,0,1,0},{"duck_threshold_db",-60,0,-24,1,0},
        {"duck_attack_ms",.1f,200,10,.4f,0},{"duck_release_ms",10,2000,250,.4f,0}
    };
    for(int band=1;band<=6;++band){const auto p="decay."+juce::String(band)+".";
        for(const auto& f:std::vector<ExpectedField>{{"active",0,1,0,1,2},{"bypassed",0,1,0,1,2},
            {"shape",0,2,0,1,3},{"freq_hz",20,20000,1000,.35f,0},
            {"multiplier",.25f,4,1,.5f,0},{"q",.25f,4,1,.5f,0}})
            out.push_back({p+f.key,f.minimum,f.maximum,f.initial,f.skew,f.choices});
    }
    for(int band=1;band<=6;++band){const auto p="post."+juce::String(band)+".";
        for(const auto& f:std::vector<ExpectedField>{{"active",0,1,0,1,2},{"bypassed",0,1,0,1,2},
            {"shape",0,4,0,1,5},{"freq_hz",20,20000,1000,.35f,0},
            {"gain_db",-18,18,0,1,0},{"bell_q",.25f,12,1,.5f,0},
            {"shelf_q",.25f,4,1,.5f,0},{"slope",0,1,0,1,2}})
            out.push_back({p+f.key,f.minimum,f.maximum,f.initial,f.skew,f.choices});
    }
    for(auto& f:out)f.key="space_reverb."+f.key;
    CHECK(out.size()==fieldsPerSlot);return out;
}
std::optional<std::size_t> find(const juce::String& id) {
    const auto registry=parameterRegistry();
    const auto i=std::find_if(registry.begin(),registry.end(),[&](const auto& f){return f.id==id;});
    return i==registry.end()?std::nullopt:std::optional<std::size_t>(std::size_t(i-registry.begin()));
}
bool close(double a,double b) {return std::isfinite(a)&&std::abs(a-b)<=3e-5*std::max(1.,std::abs(b));}
void expect(const space::Settings& actual,const space::Settings& expected) {
    CHECK(close(actual.size,expected.size));CHECK(close(actual.decay.decaySeconds,expected.decay.decaySeconds));
    CHECK(close(actual.decay.brightness,expected.decay.brightness));CHECK(close(actual.predelayMs,expected.predelayMs));
    CHECK(close(actual.character,expected.character));CHECK(close(actual.distance,expected.distance));
    CHECK(close(actual.width,expected.width));CHECK(close(actual.mix,expected.mix));
    CHECK(close(actual.duckRangeDb,expected.duckRangeDb));CHECK(close(actual.duckThresholdDb,expected.duckThresholdDb));
    CHECK(close(actual.duckAttackMs,expected.duckAttackMs));CHECK(close(actual.duckReleaseMs,expected.duckReleaseMs));
    for(std::size_t i=0;i<6;++i){const auto& a=actual.decay.bands[i];const auto& b=expected.decay.bands[i];
        CHECK(a.active==b.active);CHECK(a.bypassed==b.bypassed);CHECK(a.shape==b.shape);
        CHECK(close(a.frequencyHz,b.frequencyHz));CHECK(close(a.multiplier,b.multiplier));CHECK(close(a.q,b.q));
        const auto& c=actual.post[i];const auto& d=expected.post[i];
        CHECK(c.active==d.active);CHECK(c.bypassed==d.bypassed);CHECK(c.shape==d.shape);CHECK(c.slope==d.slope);
        CHECK(close(c.frequencyHz,d.frequencyHz));CHECK(close(c.gainDb,d.gainDb));
        CHECK(close(c.bellQ,d.bellQ));CHECK(close(c.shelfQ,d.shelfQ));
    }
}
template<class Fx> void expectMapped(const Fx& fx,const space::Settings& expected) {
    // The exact same test compiles before the future retained member exists.
    if constexpr(requires {fx.spaceReverb;})expect(fx.spaceReverb,expected);
    else CHECK(false);
}
void setExpected(space::Settings& s,std::size_t field,double v) {
    if(field<12){switch(field){
        case 0:s.size=v;break;case 1:s.decay.decaySeconds=v;break;case 2:s.predelayMs=v;break;
        case 3:s.decay.brightness=v;break;case 4:s.character=v;break;case 5:s.distance=v;break;
        case 6:s.width=v;break;case 7:s.mix=v;break;case 8:s.duckRangeDb=v;break;
        case 9:s.duckThresholdDb=v;break;case 10:s.duckAttackMs=v;break;case 11:s.duckReleaseMs=v;break;
    }return;}
    if(field<48){auto& b=s.decay.bands[(field-12)/6];switch((field-12)%6){
        case 0:b.active=v==1;break;case 1:b.bypassed=v==1;break;case 2:b.shape=space::DecayShape(int(v));break;
        case 3:b.frequencyHz=v;break;case 4:b.multiplier=v;break;case 5:b.q=v;break;
    }return;}
    auto& b=s.post[(field-48)/8];switch((field-48)%8){
        case 0:b.active=v==1;break;case 1:b.bypassed=v==1;break;case 2:b.shape=space::PostShape(int(v));break;
        case 3:b.frequencyHz=v;break;case 4:b.gainDb=v;break;case 5:b.bellQ=v;break;
        case 6:b.shelfQ=v;break;case 7:b.slope=space::CutSlope(int(v));break;
    }
}
void descriptorsAndRetainedMapping() {
    std::cout<<"SECTION space_all768_descriptors_retained_mapping_no_layout_insertion\n";
    const auto fields=expectedFields();const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    CHECK(registry.size()==parameterCount&&wavetableWarmthParameterStart==currentPreSpace+8*fieldsPerSlot);
    CHECK(spaceReverbParameterStart==currentPreSpace&&dynamicEqExtensionParameterStart==frozenBaseline&&delayStereoParameterStart==2191);
    std::set<juce::String> ids;std::set<std::uint32_t> keys;
    for(const auto& p:registry){CHECK(ids.insert(p.id).second);CHECK(keys.insert(p.stateKey).second);}
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t f=0;f<fields.size();++f){
        const auto& expected=fields[f];const auto id="effect."+juce::String(int(slot+1))+"."+expected.key;
        const auto index=find(id);CHECK(index.has_value());if(!index)continue;
        CHECK(*index==currentPreSpace+slot*fieldsPerSlot+f);CHECK(expected.key.getNumBytesAsUTF8()<=32);
        const auto& p=registry[*index];CHECK(p.minimum==expected.minimum);CHECK(p.maximum==expected.maximum);
        CHECK(p.defaultValue==expected.initial);CHECK(p.skew==expected.skew);CHECK(p.choices.size()==std::size_t(expected.choices));
        CHECK(p.kind==(expected.choices==0?ParameterKind::continuous:expected.key.endsWith(".shape")||expected.key.endsWith(".slope")?ParameterKind::choice:ParameterKind::toggle));
        CHECK(isContinuousParameter(*index)==(expected.choices==0));CHECK(parameterIndexForKey(p.stateKey)==index);
        CHECK(close(parameterPhysicalValue(defaults,*index),expected.initial));
        std::vector<float> physical{expected.minimum,expected.maximum};
        if(expected.choices)for(int n=0;n<expected.choices;++n)physical.push_back(float(n));
        else physical.push_back(expected.minimum+(expected.maximum-expected.minimum)*.371f);
        for(float value:physical){SoundDocument d;const auto before=d.layout;
            d.values[*index]=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(value);
            CHECK(close(parameterPhysicalValue(d.values,*index),value));CHECK(validateSoundDocument(d));
            RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));CHECK(d.layout==before);
            space::Settings desired;setExpected(desired,f,value);
            for(std::size_t other=0;other<8;++other)expectMapped(patch.effects[other],other==slot?desired:space::Settings{});
        }
        for(float bad:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
            SoundDocument d;d.values[*index]=bad;CHECK(!validateSoundDocument(d));
            RoutedPatch output;const auto prior=output;CHECK(!makeRoutedPatch(d,output));CHECK(output==prior);
        }
        if(expected.choices){SoundDocument d;d.values[*index]=.1234f;CHECK(!validateSoundDocument(d));}
        // All retained fields keep their physical owner, even with no module inserted.
        for(bool owner:{false,true}){SoundDocument current;current.effectLocks[owner?slot:(slot+1)%8]=true;
            auto candidate=current;candidate.values[*index]=defaults[*index]<.5f?1.f:0.f;
            CHECK(validateAiSoundEdit(current,candidate)==!owner);
            current.macros[0]={p.stateKey,.25f};candidate=current;candidate.macros[0].amount=.5f;
            if(isContinuousParameter(*index)) {
                CHECK(validateSoundDocument(current));CHECK(validateSoundDocument(candidate));
                CHECK(validateAiSoundEdit(current,candidate)==!owner);
            } else {
                // Macro targets retain their established continuous-only domain.
                CHECK(!validateSoundDocument(current));CHECK(!validateSoundDocument(candidate));
                CHECK(!validateAiSoundEdit(current,candidate));
            }
        }
        SoundDocument locked;locked.controlLocks[*index]=true;auto changed=locked;
        changed.values[*index]=defaults[*index]<.5f?1.f:0.f;CHECK(!validateAiSoundEdit(locked,changed));
    }
}
template<class Patch> void directInvalidRetained(Patch source) {
    std::cout<<"SECTION direct_invalid_inactive_retained_space_banks\n";
    if constexpr(requires {source.effects[0].spaceReverb;}){
        CHECK(validateRoutedPatch(source));const auto fields=expectedFields();
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t f=0;f<fields.size();++f)if(!fields[f].choices){
            auto bad=source;setExpected(bad.effects[slot].spaceReverb,f,std::numeric_limits<double>::quiet_NaN());
            CHECK(!validateRoutedPatch(bad));
        }
        for(std::size_t slot=0;slot<8;++slot){auto bad=source;
            bad.effects[slot].spaceReverb.decay.bands[5].shape=space::DecayShape(255);CHECK(!validateRoutedPatch(bad));
            bad=source;bad.effects[slot].spaceReverb.post[5].shape=space::PostShape(255);CHECK(!validateRoutedPatch(bad));
            bad=source;bad.effects[slot].spaceReverb.post[5].slope=space::CutSlope(255);CHECK(!validateRoutedPatch(bad));
        }
    }else CHECK(false);
}
void resign(juce::MemoryBlock& bytes) {
    CHECK(bytes.getSize()>=32);if(bytes.getSize()<32)return;
    const auto digest=juce::SHA256(bytes.getData(),bytes.getSize()-32).getRawData();
    std::memcpy(static_cast<char*>(bytes.getData())+bytes.getSize()-32,digest.getData(),32);
}
void put32(juce::MemoryBlock& bytes,std::size_t offset,std::uint32_t value) {
    CHECK(offset+4<=bytes.getSize());if(offset+4>bytes.getSize())return;
    auto* p=static_cast<unsigned char*>(bytes.getData())+offset;
    for(unsigned n=0;n<4;++n)p[n]=static_cast<unsigned char>(value>>(8*n));
}
void rejectSound(juce::MemoryBlock bytes,const SoundDocument& sentinel) {
    resign(bytes);auto output=sentinel;
    CHECK(!decodeSoundState(bytes.getData(),bytes.getSize(),output));CHECK(output==sentinel);
}
void soundAndProjectVersions() {
    std::cout<<"SECTION deliberate_sound27_project26_geometry_and_atomic_rejection\n";
    SoundDocument sound;const auto registry=parameterRegistry();
    if(registry.size()<3539){CHECK(false);return;}
    for(std::size_t i=2063;i<registry.size();++i){const auto& d=registry[i];
        sound.values[i]=d.kind==ParameterKind::continuous?float(i%7)/6.f:float(i%d.choices.size())/float(d.choices.size()-1);
        sound.controlLocks[i]=i%3==0;
    }
    for(std::size_t base=dynamicEqDetectorParameterStart;base<dynamicEqSpectralParameterStart;base+=dynamicEqDetectorParametersPerBand){sound.values[base+3]=.2f;sound.values[base+4]=.8f;}
    for(std::size_t slot=0;slot<8;++slot)sound.values[dynamicEqSpectralParameterStart+slot*dynamicEqSpectralParametersPerSlot]=0;
    sound.macros[0]={registry[currentPreSpace].stateKey,.2f};sound.values[1]=.25f;sound.effectLocks[7]=true;
    juce::MemoryBlock bytes;const bool encoded=encodeSoundState(sound,bytes);CHECK(encoded);if(!encoded)return;CHECK(bytes.getSize()==41053);
    juce::MemoryInputStream header(bytes,false);CHECK(header.readInt()==0x49535241);CHECK(header.readInt()==31);CHECK(header.readInt()==4519);
    SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));CHECK(recalled==sound);
    CHECK(currentSoundNumericBytes==40841&&kernelCompatibilitySectionBytes==168);
    CHECK(maximumSoundStateBytes==830205&&currentSoundStateSize==41053);
    for(int version:{23,25,26,27,28,29,32}){auto malformed=bytes;put32(malformed,4,version);rejectSound(malformed,sound);}
    auto malformed=bytes;put32(malformed,12,1);rejectSound(malformed,sound);
    malformed=bytes;put32(malformed,16+currentPreSpace*8,registry[0].stateKey);rejectSound(malformed,sound);
    malformed=bytes;static_cast<unsigned char*>(malformed.getData())[40841+4+8+8+16]=2;rejectSound(malformed,sound);
    malformed=bytes;malformed.append("\0",1);rejectSound(malformed,sound);
    malformed=bytes;malformed.setSize(bytes.getSize()-1);rejectSound(malformed,sound);
    auto invalid=sound;invalid.compatibility.effects[7][16]=2;juce::MemoryBlock unchanged("keep",4);
    CHECK(!encodeSoundState(invalid,unchanged));CHECK(unchanged==juce::MemoryBlock("keep",4));
    ProjectDocument project;project.sound=sound;juce::MemoryBlock wire;
    CHECK(encodeProjectState(project,wire));CHECK(wire.getSize()==41161);
    juce::MemoryInputStream projectHeader(wire,false);projectHeader.setPosition(4);CHECK(projectHeader.readShort()==30);
    ProjectDocument again;CHECK(decodeProjectState(wire.getData(),wire.getSize(),again));CHECK(again==project);
    CHECK(currentMinimumProjectStateBytes==41161&&maximumProjectStateBytes==835738);
    for(int revision:{19,20,21,22,23,24,25,26,27,28,31}){auto bad=wire;auto* p=static_cast<unsigned char*>(bad.getData());p[4]=static_cast<unsigned char>(revision);p[5]=0;
        resign(bad);auto output=project;CHECK(!decodeProjectState(bad.getData(),bad.getSize(),output));CHECK(output==project);}
    auto bad=wire;put32(bad,24,20);resign(bad);auto output=project;
    CHECK(!decodeProjectState(bad.getData(),bad.getSize(),output));CHECK(output==project);
    bad=wire;put32(bad,12,0x7fffffff);resign(bad);output=project;
    CHECK(!decodeProjectState(bad.getData(),bad.getSize(),output));CHECK(output==project);
}
void historicalSound19And20() {
    std::cout<<"SECTION actual_frozen_sound19_20_and_project18_19_domains_remain_closed\n";
    const auto directory=juce::File(juce::String(RACKSYNTH_SPACE_BASELINE_DIR));
    const auto registry=parameterRegistry();if(registry.size()<2983){CHECK(false);return;}
    const auto newKey=registry[currentPreSpace].stateKey;
    struct Old {int version;std::size_t records,numeric,minimum;};
    for(const auto old:std::array<Old,2>{{{19,2023,18377,18561},{20,2063,18737,18921}}}){
      for(const auto* prefix:{"default","rich","embedded"}){
        juce::MemoryBlock bytes;CHECK(directory.getChildFile(juce::String(prefix)+"-v"+juce::String(old.version)+".soundstate").loadFileAsData(bytes));if(bytes.getSize()<old.minimum)continue;
        SoundDocument decoded;const auto accepted=decodeSoundState(bytes.getData(),bytes.getSize(),decoded);CHECK(accepted);if(!accepted)continue;
        juce::MemoryInputStream header(bytes,false);header.setPosition(4);CHECK(header.readInt()==old.version);CHECK(header.readInt()==int(old.records));header.setPosition(16);
        for(std::size_t record=0;record<old.records;++record){const auto key=std::uint32_t(header.readInt());const auto value=header.readFloat();const auto index=parameterIndexForKey(key);
            CHECK(index.has_value());if(index)CHECK(decoded.values[*index]==value);}
        const auto defaults=defaultParameterValues();for(std::size_t i=old.records;i<parameterCount;++i){CHECK(decoded.values[i]==defaults[i]);CHECK(!decoded.controlLocks[i]);}
        for(const auto& effect:decoded.compatibility.effects)CHECK(effect[16]==1);
        auto bad=bytes;put32(bad,16,newKey);rejectSound(bad,decoded);
        bad=bytes;put32(bad,16+old.records*8+78,newKey);rejectSound(bad,decoded);
        bad=bytes;static_cast<unsigned char*>(bad.getData())[16+old.records*8+10]=16;rejectSound(bad,decoded);
        if(decoded.personalities){
            auto candidate=decoded;auto personalities=std::make_shared<RackPersonalityState>(*decoded.personalities);
            RackUnit* fx=nullptr;for(auto& instance:personalities->instances)for(auto& unit:instance.design.units)if(unit.module>=NativeModule::drive)fx=&unit;
            CHECK(fx!=nullptr);if(!fx)continue;fx->defaults.push_back({fx->id,"space_reverb.size",.5f});candidate.personalities=personalities;
            juce::MemoryBlock modern;const bool encoded=encodeSoundState(candidate,modern);CHECK(encoded);
            if(encoded&&modern.getSize()>currentPersonalityLengthOffset+32){
                juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,old.numeric+4+144,decoded,control));CHECK(control==bytes);
                SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==decoded);
                juce::MemoryBlock nested;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,old.numeric+4+144,candidate,nested));rejectSound(nested,decoded);
            }
        }
        juce::MemoryBlock upgraded;CHECK(encodeSoundState(decoded,upgraded));SoundDocument again;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),again));CHECK(again==decoded);
      }
      juce::MemoryBlock bytes;CHECK(directory.getChildFile("rich-v"+juce::String(old.version-1)+".airackproject").loadFileAsData(bytes));
      ProjectDocument decoded;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),decoded));
      if(bytes.getSize()>=old.minimum+108){auto bad=bytes;auto* p=static_cast<unsigned char*>(bad.getData());p[4]=20;p[5]=0;resign(bad);
        auto output=decoded;CHECK(!decodeProjectState(bad.getData(),bad.getSize(),output));CHECK(output==decoded);}
    }
    CHECK(oldSound19ParameterCount==2023&&oldSound19StateSize==18561&&oldSound19MaximumBytes==805477);
    CHECK(oldProject18MinimumBytes==18669&&oldProject18MaximumBytes==811010);
}
void historicalSound23LayoutsMigrateExactly() {
    std::cout<<"SECTION actual_main_and_pr47_sound23_project22_prefix_keys_locks_macros_and_racks_migrate\n";
    const auto parent=juce::File(juce::String(RACKSYNTH_SPACE_BASELINE_DIR)).getParentDirectory();
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    struct Schema {const char* directory;std::size_t count;bool space;const char* registrySha;const char* defaultSha;const char* richSha;const char* projectSha;};
    const std::array<Schema,2> schemas{{
        {"space-reverb-pr47-sound23",2983,true,
         "539c25b3651bad64189e8d8c4d408a70925074b795f6cb25567646cfa2f6ffd3",
         "7c6cda1c2ef2472a20f58020cc9358bf86697d21281a0807936dd4bfeb0787c6",
         "420ac810ce7d2f67b6cc16e3f08fc581c173b49b3b973cd657224cddbabdebdc",
         "47c6aee469064173e2d8aa426847f201e0b5edbc335748227b547fbe19907020"},
        {"space-reverb-accepted-main55-sound23",2535,false,
         "0098522dc4c00cff88b76b282ab8053df4ba818fa6bb11cb62bb300417dc8189",
         "4cb06929f0be3a84f8834f4435717f1c2a03abc57e99ea3d5486325a0b07cee8",
         "aa018619f867bf8242195e9a098e31338d3a5883e2c8c2d914cec80f9d488c44",
         "48c2c750d9f0c6b6a167b919088ffcef4052c7917cf9d1ee98768483afc41a51"}}};
    struct Witness {const char* name;const char* sha;};
    for(const auto& schema:schemas){
        const auto directory=parent.getChildFile(schema.directory);
        const auto mapped=[&](std::size_t old){return old+(schema.space&&old>=2215?520:0);};
        juce::MemoryBlock catalog;CHECK(directory.getChildFile("registry-v23.json").loadFileAsData(catalog));CHECK(juce::SHA256(catalog).toHexString()==schema.registrySha);
        const auto rows=juce::JSON::parse(catalog.toString());CHECK(rows.isArray()&&rows.size()==int(schema.count));
        if(rows.isArray()&&rows.size()==int(schema.count))for(std::size_t old=0;old<schema.count;++old){
            const auto row=rows[int(old)];const auto& d=registry[mapped(old)];
            CHECK(row["id"].toString()==d.id&&row["name"].toString()==d.name&&row["unit"].toString()==d.unit);
            CHECK(int(row["kind"])==int(d.kind)&&juce::int64(row["key"])==juce::int64(d.stateKey));
            CHECK(float(row["minimum"])==d.minimum&&float(row["maximum"])==d.maximum&&float(row["default"])==d.defaultValue&&float(row["skew"])==d.skew);
            const auto choices=row["choices"];CHECK(choices.isArray()&&choices.size()==int(d.choices.size()));
            for(int c=0;c<std::min(choices.size(),int(d.choices.size()));++c)CHECK(choices[c].toString()==d.choices[std::size_t(c)]);
        }
        auto rich=std::make_unique<SoundDocument>();bool richRead=false;
        for(const auto witness:std::array<Witness,2>{{{"default-v23.soundstate",schema.defaultSha},{"rich-v23.soundstate",schema.richSha}}}){
            juce::MemoryBlock bytes;CHECK(directory.getChildFile(witness.name).loadFileAsData(bytes));CHECK(juce::SHA256(bytes).toHexString()==witness.sha);
            auto decoded=std::make_unique<SoundDocument>();const bool read=decodeSoundState(bytes.getData(),bytes.getSize(),*decoded);CHECK(read);if(!read)continue;
            juce::MemoryInputStream records(bytes,false);records.setPosition(4);CHECK(records.readInt()==23&&records.readInt()==int(schema.count));records.setPosition(16);
            for(std::size_t old=0;old<schema.count;++old){
                const auto key=std::uint32_t(records.readInt());const auto value=records.readFloat();const auto index=parameterIndexForKey(key);CHECK(index.has_value());
                if(index){CHECK(*index==mapped(old));CHECK(decoded->values[*index]==value);}
            }
            records.setPosition(16+schema.count*8+78);
            for(const auto& macro:decoded->macros){CHECK(macro.targetKey==std::uint32_t(records.readInt()));CHECK(macro.amount==records.readFloat());}
            for(std::size_t old=0;old<schema.count;++old){const auto value=records.readByte();CHECK(value==0||value==1);CHECK(decoded->controlLocks[mapped(old)]==(value==1));}
            const auto first=schema.space?dynamicEqDetectorParameterStart:dynamicEqSpectralParameterStart;
            const auto last=schema.space?spaceReverbParameterStart:parameterCount;
            for(std::size_t i=first;i<last;++i){CHECK(decoded->values[i]==defaults[i]);CHECK(!decoded->controlLocks[i]);}
            if(!schema.space)for(const auto& banks:decoded->compatibility.effects)CHECK(banks[16]==1);
            juce::MemoryBlock modern;CHECK(encodeSoundState(*decoded,modern));juce::MemoryInputStream header(modern,false);header.setPosition(4);CHECK(header.readInt()==31&&header.readInt()==4519);
            auto roundTrip=std::make_unique<SoundDocument>();CHECK(decodeSoundState(modern.getData(),modern.getSize(),*roundTrip)&&*roundTrip==*decoded);
            const auto foreignKey=registry[schema.space?dynamicEqDetectorParameterStart+3:spaceReverbParameterStart].stateKey;
            auto foreign=bytes;put32(foreign,16,foreignKey);rejectSound(foreign,*decoded);
            foreign=bytes;put32(foreign,16+schema.count*8+78,foreignKey);rejectSound(foreign,*decoded);
            if(schema.space&&registry.size()>3659){
                const auto drumKey=registry[3659].stateKey;
                auto drumRecord=bytes;put32(drumRecord,16,drumKey);rejectSound(drumRecord,*decoded);
                auto drumMacro=bytes;put32(drumMacro,16+schema.count*8+78,drumKey);rejectSound(drumMacro,*decoded);
            }
            if(juce::String(witness.name).startsWith("rich")){
                CHECK(decoded->personalities&&!decoded->personalities->instances.empty());
                if(schema.space)CHECK(std::any_of(decoded->personalities->instances.begin(),decoded->personalities->instances.end(),[](const auto& i){return i.design.name=="Lumen Field";}));
                CHECK(decoded->synthLocks[2]&&decoded->effectLocks[7]&&decoded->controlLocks[13]&&decoded->controlLocks[mapped(2215)]&&decoded->controlLocks[mapped(schema.count-1)]);
                *rich=*decoded;richRead=true;
            }
        }
        juce::MemoryBlock projectBytes;CHECK(directory.getChildFile("rich-v22.airackproject").loadFileAsData(projectBytes));CHECK(juce::SHA256(projectBytes).toHexString()==schema.projectSha);
        auto project=std::make_unique<ProjectDocument>();const bool read=decodeProjectState(projectBytes.getData(),projectBytes.getSize(),*project);CHECK(read);
        if(read){CHECK(richRead&&project->sound==*rich);juce::MemoryBlock modern;CHECK(encodeProjectState(*project,modern));auto again=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(modern.getData(),modern.getSize(),*again)&&*again==*project);}
    }
}

}
int main() {
    frozenRegistryPrefix();
    descriptorsAndRetainedMapping();
    directInvalidRetained(RoutedPatch{});
    soundAndProjectVersions();
    historicalSound19And20();
    historicalSound23LayoutsMigrateExactly();
    std::cout<<"SPACE_PRODUCT_MODEL checks="<<checks<<" failures="<<failures<<'\n';
    return failures?1:0;
}
