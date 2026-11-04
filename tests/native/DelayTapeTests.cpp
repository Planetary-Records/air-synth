#include "LegacyPersonalityProjection.h"
#include "FrozenRackExamples.h"
#include "AiCatalog.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "ProjectDocument.h"
#include "SoundState.h"
#include "UserReach.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace
{
using namespace airack;
int checks=0,failures=0;
void check(bool good,const char* expression,int line)
{++checks;if(!good){++failures;if(failures<85)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
// Today's registry and states, before the delay's tape keys: a rich sound has a delay in slot 2 with every delay key
// away from its default, and the embedded one adds a placed Echo Thread.
bool captureLegacy(const juce::File& directory)
{
    if (parameterCount != 1959 || directory.exists() || !directory.createDirectory().wasOk()) return false;
    const auto write = [&](const char* name, const juce::MemoryBlock& bytes) {
        const auto file = directory.getChildFile(name);
        if (file.exists() || !file.replaceWithData(bytes.getData(), bytes.getSize())) return false;
        std::cout << "DELAY_TAPE_LEGACY " << name << " bytes=" << bytes.getSize() << " sha256=" << juce::SHA256(bytes).toHexString() << '\n';
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
    if (!write("registry-v18.json", juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()))) return false;
    SoundDocument empty, rich;
    RackEdit edit; edit.kind=RackEditKind::addEffect; edit.slot=2; edit.effect=EffectKind::delay;
    if (editRackLayout(rich.layout, edit, rich.layout) != RackEditResult::applied) return false;
    const std::array<std::pair<const char*,float>,8> delayValues {{{"bypassed",1.f},{"delay.synced",1.f},{"delay.seconds",.37f},{"delay.beats",.62f},
        {"delay.feedback",.71f},{"delay.stereo",.44f},{"delay.filter_hz",.29f},{"delay.mix",.83f}}};
    for (const auto& [key, value] : delayValues) {
        const auto index = rackParameterIndex(NativeModule::delay, 2, key);
        if (!index || rich.values[*index] == value) return false;
        rich.values[*index] = value;
    }
    const auto feedback = rackParameterIndex(NativeModule::delay, 2, "delay.feedback"), mix = rackParameterIndex(NativeModule::delay, 2, "delay.mix");
    if (!feedback || !mix) return false;
    rich.values[0]=.3f; rich.controlLocks[*feedback]=true; rich.effectLocks[2]=true;
    rich.macros[0]={parameterRegistry()[*mix].stateKey,.21f};
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto factory=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Echo Thread";});
    if (factory==factories.end()) return false;
    RackOperation place; place.kind=RackOperationKind::place; place.design=*factory;
    place.newInstance=juce::Uuid("5d0c3f7e-2b8a-4c61-9e47-a1f3b6d82c09"); place.destination={RackRole::fx,masterChain,0}; place.allocation={{0,0}};
    SoundDocument embedded; RackDifference delta;
    if (makeRackCandidate(rich,place,SoundEditOrigin::manual,embedded,delta)!=RackCandidateStatus::ready) return false;
    juce::MemoryBlock bytes;
    for (const auto& [name, sound] : std::array<std::pair<const char*,const SoundDocument*>,3> {{{"default-v18.soundstate",&empty},{"rich-v18.soundstate",&rich},{"embedded-v18.soundstate",&embedded}}})
        if (!encodeSoundState(*sound,bytes) || !write(name,bytes)) return false;
    ProjectDocument project; project.sound=rich;
    return encodeProjectState(project,bytes) && write("rich-v17.airackproject",bytes);
}
const juce::File fixtures(juce::String(RACKSYNTH_FIXTURE_DIR));
juce::MemoryBlock fixture(const char* name)
{ juce::MemoryBlock bytes; CHECK(fixtures.getChildFile(name).loadFileAsData(bytes)); return bytes; }
void legacyFixturesPinned()
{
    std::cout<<"SECTION legacy_fixtures_registry18_19_sound18_19_project17_18\n";
    const std::array<std::pair<const char*,const char*>,10> pinned {{
        {"registry-v18.json","b340008a2f9413f7962eebf065e27de01117786bb83b20f790637c4dc3a6aba3"},
        {"default-v18.soundstate","5634e17005e72a23c177e1380b2b90c68ab622d3ae912782578d6edb7498ac13"},
        {"rich-v18.soundstate","d5101cbc2f0cf79f35a0f02a2da25c1970d7e6ad50084597ef967cd80e3e5d62"},
        {"embedded-v18.soundstate","24ee0d61e23bc356aac3495931bcdd6bb91af5f7521e5046b9721d889696b7e6"},
        {"rich-v17.airackproject","4f2d63d7006bf917e724c768f5bcc742d91066a56ffed11245663acf73928a1d"},
        {"registry-v19.json","d80b3bb04745e7eede07bafa0af120ee4c8a2830847abac1ccaec76026af09d9"},
        {"default-v19.soundstate","ba2b010530f74cc71982b7e72a3e66bffc29610f66ea00fe5b3f8253812f3c08"},
        {"rich-v19.soundstate","1c87f3e15ff44a2fbf079022828d5d13869971b11bef8b84ed03ab8da3adbc08"},
        {"embedded-v19.soundstate","749fd9f4d87f8c9efe7ec1759d1ceab1549927b1a594251c92cab9861e0f4a7f"},
        {"rich-v18.airackproject","9511d707633926d41ecd30cab6878b984cfa066c3f61c9522de5f6b21d6b280e"}}};
    for(const auto& [name,hash]:pinned)CHECK(juce::SHA256(fixture(name)).toHexString()==hash);
}
struct TapeKey { const char* key; const char* name; const char* unit; float minimum, maximum, initial; bool toggle; };
constexpr TapeKey tapeKeys[] {
    {"delay.saturation","Delay saturation","",0,1,0,false},
    {"delay.low_cut_hz","Delay low cut","Hz",20,2000,20,false},
    {"delay.wobble","Delay wobble","",0,1,0,false},
    {"delay.wobble_rate_hz","Delay wobble rate","Hz",.1f,10,.8f,false},
    {"delay.glide","Delay glide","",0,1,0,true}};
std::optional<std::size_t> tapeIndex(std::uint8_t slot,std::size_t field)
{ return rackParameterIndex(NativeModule::delay,slot,tapeKeys[field].key); }
void tapeKeysRegistryAndFrozenPrefix()
{
    std::cout<<"SECTION tape_keys_appended_after_frozen_1959_and_2023_descriptors\n";
    const auto registry=parameterRegistry();
    CHECK(delayTapeParameterStart==2023&&dynamicEqDetectorParameterStart==2215&&parameterCount==4519&&registry.size()==4519);
    for(const auto& [name,count]:std::array<std::pair<const char*,int>,2>{{{"registry-v18.json",1959},{"registry-v19.json",2023}}}){
        const auto bytes=fixture(name);
        const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())));
        CHECK(rows.isArray()&&rows.size()==count);
        if(rows.size()==count&&registry.size()>=std::size_t(count))for(int i=0;i<count;++i){
            const auto& d=registry[std::size_t(i)];const auto row=rows[i];
            CHECK(d.id==row["id"].toString());CHECK(d.name==row["name"].toString());CHECK(d.unit==row["unit"].toString());
            CHECK(int(d.kind)==int(row["kind"]));CHECK(d.minimum==float(row["minimum"]));CHECK(d.maximum==float(row["maximum"]));
            CHECK(d.defaultValue==float(row["default"]));CHECK(d.skew==float(row["skew"]));CHECK(d.stateKey==std::uint32_t(juce::int64(row["key"])));
            CHECK(d.choices.size()==std::size_t(row["choices"].size()));for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());
        }
    }
    for(std::uint8_t slot=0;slot<8;++slot){
        const auto reach=unitReach(NativeModule::delay,slot);CHECK(reach.size()==16);
        for(std::size_t field=0;field<5;++field){
            const auto& k=tapeKeys[field];const auto index=tapeIndex(slot,field);
            CHECK(index==std::optional<std::size_t>(delayTapeParameterStart+5*slot+field));if(!index||*index>=registry.size())continue;
            const auto& d=registry[*index];const auto n=juce::String(slot+1);
            CHECK(d.id=="effect."+n+"."+k.key);CHECK(d.name=="Effect "+n+" "+k.name);CHECK(d.unit==k.unit);
            CHECK(d.minimum==k.minimum&&d.maximum==k.maximum&&d.defaultValue==k.initial&&d.skew==1);
            CHECK(k.toggle?d.kind==ParameterKind::toggle&&d.choices==std::vector<juce::String>({"Off","On"}):d.kind==ParameterKind::continuous&&d.choices.empty());
            CHECK(std::find(reach.begin(),reach.end(),*index)!=reach.end());
            for(int m=int(NativeModule::drive);m<=int(NativeModule::spaceReverb);++m)if(NativeModule(m)!=NativeModule::delay){
                CHECK(!rackParameterActive(NativeModule(m),k.key));
                const auto other=unitReach(NativeModule(m),slot);CHECK(std::find(other.begin(),other.end(),*index)==other.end());
            }
        }
    }
}
void tapeKeysMapIntoDelaySettings()
{
    std::cout<<"SECTION tape_keys_map_into_delay_settings_and_refuse_out_of_range\n";
    for(std::uint8_t slot=0;slot<8;++slot){
        SoundDocument sound;RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
        const auto& neutral=patch.effects[slot].delay;
        CHECK(neutral.saturation==0&&neutral.lowCutHz==20&&neutral.wobble==0&&std::abs(neutral.wobbleRateHz-.8f)<1e-6f&&!neutral.glide);
        for(const float end:{0.f,1.f}){
            for(std::size_t field=0;field<5;++field)if(const auto index=tapeIndex(slot,field))sound.values[*index]=end;
            CHECK(makeRoutedPatch(sound,patch));const auto& d=patch.effects[slot].delay;
            CHECK(d.saturation==end&&std::abs(d.lowCutHz-(end?2000.f:20.f))<1e-3f&&d.wobble==end&&std::abs(d.wobbleRateHz-(end?10.f:.1f))<1e-5f&&d.glide==(end==1));
        }
        for(const float bad:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN()})if(const auto index=tapeIndex(slot,1)){
            auto invalid=sound;invalid.values[*index]=bad;const auto before=patch;CHECK(!makeRoutedPatch(invalid,patch));CHECK(patch==before);
        }
    }
    SoundDocument sound;RoutedPatch base;CHECK(makeRoutedPatch(sound,base));CHECK(validateRoutedPatch(base));
    for(int bad=0;bad<9;++bad){
        auto p=base;auto& d=p.effects[3].delay;
        if(bad==0)d.saturation=-.01f;
        if(bad==1)d.saturation=1.01f;
        if(bad==2)d.lowCutHz=19.9f;
        if(bad==3)d.lowCutHz=2001;
        if(bad==4)d.wobble=1.01f;
        if(bad==5)d.wobbleRateHz=.09f;
        if(bad==6)d.wobbleRateHz=10.1f;
        if(bad==7)d.saturation=std::numeric_limits<float>::quiet_NaN();
        if(bad==8)d.wobbleRateHz=std::numeric_limits<float>::infinity();
        CHECK(!validateRoutedPatch(p));
    }
}
struct OldGeneration { std::array<const char*,3> sounds; const char* project; std::size_t records, numericBytes, compatibilityBytes; short projectVersion; };
// The 1959 keys before the Dynamic EQ placement, then the 2023 keys before the tape keys.
const std::array<OldGeneration,2> oldGenerations {{
    {{"default-v18.soundstate","rich-v18.soundstate","embedded-v18.soundstate"},"rich-v17.airackproject",oldSound18ParameterCount,oldSound18NumericBytes,oldSound18CompatibilityBytes,17},
    {{"default-v19.soundstate","rich-v19.soundstate","embedded-v19.soundstate"},"rich-v18.airackproject",oldSound19ParameterCount,oldSound19NumericBytes,oldSound19CompatibilityBytes,18}}};
void tapeStateAndOldMigration()
{
    std::cout<<"SECTION tape_current_state_and_actual_sound18_19_project17_18_migration\n";
    const auto defaults=defaultParameterValues();
    const auto reject=[](juce::MemoryBlock malformed,const SoundDocument& decoded){
        const auto digest=juce::SHA256(malformed.getData(),malformed.getSize()-32).getRawData();
        std::memcpy(static_cast<char*>(malformed.getData())+malformed.getSize()-32,digest.getData(),32);
        auto output=decoded;CHECK(!decodeSoundState(malformed.getData(),malformed.getSize(),output));CHECK(output==decoded);
    };
    for(const auto& old:oldGenerations){
    const auto growth=9*(parameterCount-old.records)+(kernelCompatibilitySectionBytes-old.compatibilityBytes)+4;
    for(const auto* name:old.sounds){
        const auto bytes=fixture(name);
        SoundDocument decoded;const bool read=decodeSoundState(bytes.getData(),bytes.getSize(),decoded);CHECK(read);if(!read)continue;
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(16);
        for(std::size_t record=0;record<old.records;++record){const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();const auto index=parameterIndexForKey(key);CHECK(index&&*index<old.records&&decoded.values[*index]==value);}
        for(std::size_t i=old.records;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]);
        RoutedPatch patch;CHECK(makeRoutedPatch(decoded,patch));
        for(const auto& effect:patch.effects)CHECK(effect.delay.saturation==0&&effect.delay.lowCutHz==20&&effect.delay.wobble==0&&std::abs(effect.delay.wobbleRateHz-.8f)<1e-6f&&!effect.delay.glide);
        const auto feedback=rackParameterIndex(NativeModule::delay,2,"delay.feedback"),mix=rackParameterIndex(NativeModule::delay,2,"delay.mix");
        if(juce::String(name)!=old.sounds[0]&&feedback&&mix){
            CHECK(decoded.layout.effects[2].kind==EffectKind::delay&&decoded.effectLocks[2]&&decoded.controlLocks[*feedback]);
            CHECK(decoded.macros[0].targetKey==parameterRegistry()[*mix].stateKey&&decoded.macros[0].amount==.21f&&decoded.values[*mix]==.83f);
            // The 2023-key states keep their band placement: slot 6, band 4 at Mid.
            if(old.records==oldSound19ParameterCount)
                CHECK(decoded.values[dynamicEqPlacementParameterStart+5*8+3]==.75f&&patch.effects[5].dynamicEq.bands[3].placement==DynamicEqPlacement(3));
        }
        if(juce::String(name)==old.sounds[2])
            {CHECK(decoded.personalities&&decoded.personalities->instances.size()==1&&decoded.personalities->instances[0].design.units[0].defaults.size()==8);if(decoded.personalities&&decoded.personalities->instances.size()==1){CHECK(decoded.personalities->instances[0].design.id==juce::Uuid("91387715-13d3-4c57-a479-12251842f402"));CHECK(!decoded.personalities->instances[0].package);}}
        // A new key stays outside the signed old domain: as a value record, a macro target or a nested default.
        const auto future=tapeIndex(0,0);CHECK(future.has_value());
        if(future){
            const auto key=parameterRegistry()[*future].stateKey;
            auto malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16,&key,4);reject(malformed,decoded);
            malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16+old.records*8+78,&key,4);reject(malformed,decoded);
            if(decoded.personalities){
                auto candidate=decoded;auto personalities=std::make_shared<RackPersonalityState>(*decoded.personalities);
                personalities->instances[0].design.units[0].defaults.push_back({0,"delay.saturation",.5f});candidate.personalities=personalities;
                juce::MemoryBlock modern;const bool encoded=encodeSoundState(candidate,modern);CHECK(encoded);
                if(encoded){
                    juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,old.numericBytes+4+old.compatibilityBytes,decoded,control));CHECK(control==bytes);
                    SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==decoded);
                    juce::MemoryBlock nested;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,old.numericBytes+4+old.compatibilityBytes,candidate,nested));reject(nested,decoded);
                }
            }
        }
        juce::MemoryBlock upgraded;CHECK(encodeSoundState(decoded,upgraded));CHECK(upgraded.getSize()==bytes.getSize()+growth+(decoded.personalities?decoded.personalities->instances.size():0));
        juce::MemoryInputStream header(upgraded,false);header.setPosition(4);CHECK(header.readInt()==31&&header.readInt()==4519);
        SoundDocument again;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),again)&&again==decoded);
    }
    SoundDocument rich;const auto richBytes=fixture(old.sounds[1]);CHECK(decodeSoundState(richBytes.getData(),richBytes.getSize(),rich));
    const auto oldProject=fixture(old.project);
    juce::MemoryInputStream oldHeader(oldProject,false);oldHeader.setPosition(4);CHECK(oldHeader.readShort()==old.projectVersion);
    ProjectDocument project;CHECK(decodeProjectState(oldProject.getData(),oldProject.getSize(),project));CHECK(project.sound==rich);
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field)if(const auto index=tapeIndex(slot,field))project.sound.values[*index]=field==4?1.f:.4f;
    juce::MemoryBlock current;CHECK(encodeProjectState(project,current));CHECK(current.getSize()==oldProject.getSize()+growth);
    juce::MemoryInputStream header(current,false);header.setPosition(4);CHECK(header.readShort()==30);header.setPosition(24);CHECK(header.readInt()==31&&header.readInt()==4519);
    ProjectDocument again;CHECK(decodeProjectState(current.getData(),current.getSize(),again)&&again==project);
    }
}
void tapeKeysInAiCatalog()
{
    std::cout<<"SECTION tape_keys_in_the_ai_catalog\n";
    ProjectDocument source;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=4;add.effect=EffectKind::delay;
    CHECK(editRackLayout(source.sound.layout,add,source.sound.layout)==RackEditResult::applied);
    juce::var provider;CHECK(ai::providerCatalogFor(source,provider));
    const auto* keys=provider["module_keys"]["delay"].getArray();CHECK(keys&&keys->size()==16);if(!keys||keys->size()!=16)return;
    for(std::size_t field=0;field<5;++field){
        const auto& k=tapeKeys[field];const auto& row=(*keys)[int(8+field)];
        CHECK(row[0].toString()==k.key&&row[1].toString()==k.unit&&int(row[2])==int(k.toggle?ParameterKind::toggle:ParameterKind::continuous));
        CHECK(float(double(row[3]))==k.minimum&&float(double(row[4]))==k.maximum&&float(double(row[5]))==k.initial&&double(row[6])==1);
    }
    const auto* controls=provider["controls"].getArray();CHECK(controls!=nullptr);if(!controls)return;
    for(std::size_t field=0;field<5;++field){
        const auto index=tapeIndex(4,field);if(!index)continue;
        const auto id=parameterRegistry()[*index].id;
        CHECK(std::any_of(controls->begin(),controls->end(),[&](const juce::var& row){return row[0].toString()==id&&bool(row[9]);}));
    }
}
}
int main(int argc,char** argv)
{
    if(argc==3&&juce::String(argv[1])=="--capture-legacy")return captureLegacy(juce::File(juce::String::fromUTF8(argv[2])))?0:1;
    if(argc!=1)return 2;
    legacyFixturesPinned();tapeKeysRegistryAndFrozenPrefix();tapeKeysMapIntoDelaySettings();tapeStateAndOldMigration();tapeKeysInAiCatalog();
    std::cout<<"Delay tape: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
