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
// Today's registry and states, before the delay's stereo keys: a rich sound has a delay in slot 2 with all 13 delay
// keys away from their defaults, and the embedded one adds a placed Oxide Return, whose recipe holds all 13.
bool captureLegacy(const juce::File& directory)
{
    if (parameterCount != 2063 || directory.exists() || !directory.createDirectory().wasOk()) return false;
    const auto write = [&](const char* name, const juce::MemoryBlock& bytes) {
        const auto file = directory.getChildFile(name);
        if (file.exists() || !file.replaceWithData(bytes.getData(), bytes.getSize())) return false;
        std::cout << "DELAY_STEREO_LEGACY " << name << " bytes=" << bytes.getSize() << " sha256=" << juce::SHA256(bytes).toHexString() << '\n';
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
    if (!write("registry-v20.json", juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()))) return false;
    SoundDocument empty, rich;
    RackEdit edit; edit.kind=RackEditKind::addEffect; edit.slot=2; edit.effect=EffectKind::delay;
    if (editRackLayout(rich.layout, edit, rich.layout) != RackEditResult::applied) return false;
    const std::array<std::pair<const char*,float>,13> delayValues {{{"bypassed",1.f},{"delay.synced",1.f},{"delay.seconds",.37f},{"delay.beats",.62f},
        {"delay.feedback",.71f},{"delay.stereo",.44f},{"delay.filter_hz",.29f},{"delay.mix",.83f},{"delay.saturation",.41f},{"delay.low_cut_hz",.23f},
        {"delay.wobble",.57f},{"delay.wobble_rate_hz",.34f},{"delay.glide",1.f}}};
    for (const auto& [key, value] : delayValues) {
        const auto index = rackParameterIndex(NativeModule::delay, 2, key);
        if (!index || rich.values[*index] == value) return false;
        rich.values[*index] = value;
    }
    const auto feedback = rackParameterIndex(NativeModule::delay, 2, "delay.feedback"), mix = rackParameterIndex(NativeModule::delay, 2, "delay.mix");
    if (!feedback || !mix) return false;
    rich.values[0]=.3f; rich.controlLocks[*feedback]=true; rich.effectLocks[2]=true;
    rich.macros[0]={parameterRegistry()[*mix].stateKey,.21f};
    const auto oxide=airack::test::frozenExampleRackDesigns()[3];
    if (oxide.name!="Oxide Return" || oxide.units.size()!=1 || oxide.units[0].defaults.size()!=13) return false;
    RackOperation place; place.kind=RackOperationKind::place; place.design=oxide;
    place.newInstance=juce::Uuid("6e1d4f8a-3c9b-4d72-af58-b2c7e4d93e1a"); place.destination={RackRole::fx,masterChain,0}; place.allocation={{0,0}};
    SoundDocument embedded; RackDifference delta;
    if (makeRackCandidate(rich,place,SoundEditOrigin::manual,embedded,delta)!=RackCandidateStatus::ready) return false;
    juce::MemoryBlock bytes;
    for (const auto& [name, sound] : std::array<std::pair<const char*,const SoundDocument*>,3> {{{"default-v20.soundstate",&empty},{"rich-v20.soundstate",&rich},{"embedded-v20.soundstate",&embedded}}})
        if (!encodeSoundState(*sound,bytes) || !write(name,bytes)) return false;
    ProjectDocument project; project.sound=rich;
    return encodeProjectState(project,bytes) && write("rich-v19.airackproject",bytes);
}
const juce::File fixtures(juce::String(RACKSYNTH_FIXTURE_DIR));
juce::MemoryBlock fixture(const char* name)
{ juce::MemoryBlock bytes; CHECK(fixtures.getChildFile(name).loadFileAsData(bytes)); return bytes; }
void legacyFixturesPinned()
{
    std::cout<<"SECTION legacy_fixtures_registry20_sound20_project19\n";
    const std::array<std::pair<const char*,const char*>,5> pinned {{
        {"registry-v20.json","9940eb65ec525d7530b0aaaf188c45d5716ded7cafde82b832946c42a88a7620"},
        {"default-v20.soundstate","9d1d81ee90c482dab34e94f7f406066477a47671b3d5a8e7167a211e9b4ef14a"},
        {"rich-v20.soundstate","e6d25da4f58c30e0e84121d0cc1007e74c41a59ec483dfe26d1388d1b85dc877"},
        {"embedded-v20.soundstate","13522bf28472ab47195ace639dd2883f3348e69acc6bce23f63696ec081dfe08"},
        {"rich-v19.airackproject","1c3aced5ddb3792a921875cb648b89020bd1ba25957ac01456d85c5c4c89554d"}}};
    for(const auto& [name,hash]:pinned)CHECK(juce::SHA256(fixture(name)).toHexString()==hash);
}
const std::vector<juce::String> noteNames {"Beats","1/64 triplet","1/64","1/32 triplet","1/64 dotted","1/32","1/16 triplet","1/32 dotted",
    "1/16","1/8 triplet","1/16 dotted","1/8","1/4 triplet","1/8 dotted","1/4","1/2 triplet","1/4 dotted","1/2","1/1 triplet","1/2 dotted","1/1","1/1 dotted"};
struct StereoKey { const char* key; const char* name; const char* unit; ParameterKind kind; float minimum, maximum, initial; std::vector<juce::String> choices; };
const StereoKey stereoKeys[] {
    {"delay.right_ratio","Delay right time","x",ParameterKind::continuous,.25f,4,1,{}},
    {"delay.ping_pong","Delay ping-pong","",ParameterKind::toggle,0,1,0,{"Off","On"}},
    {"delay.note","Delay note","",ParameterKind::choice,0,21,0,noteNames}};
std::optional<std::size_t> stereoIndex(std::uint8_t slot,std::size_t field)
{ return rackParameterIndex(NativeModule::delay,slot,stereoKeys[field].key); }
void registryPrefix(const juce::MemoryBlock& bytes,int count)
{
    const auto registry=parameterRegistry();
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
void stereoKeysRegistryAndFrozenPrefix()
{
    std::cout<<"SECTION stereo_keys_appended_after_frozen_2191_descriptors\n";
    const auto registry=parameterRegistry();
    CHECK(parameterCount==4519&&registry.size()==4519&&delayStereoParameterStart==2191);
    CHECK(spaceReverbParameterStart==2735&&wavetableWarmthParameterStart-spaceReverbParameterStart==768);
    registryPrefix(fixture("registry-v20.json"),2063);
    for(std::uint8_t slot=0;slot<8;++slot){
        const auto reach=unitReach(NativeModule::delay,slot);CHECK(reach.size()==16);
        for(std::size_t field=0;field<3;++field){
            const auto& k=stereoKeys[field];const auto index=stereoIndex(slot,field);
            CHECK(index==std::optional<std::size_t>(delayStereoParameterStart+3*slot+field));if(!index||*index>=registry.size())continue;
            const auto& d=registry[*index];const auto n=juce::String(slot+1);
            CHECK(d.id=="effect."+n+"."+k.key);CHECK(d.name=="Effect "+n+" "+k.name);CHECK(d.unit==k.unit);CHECK(d.kind==k.kind);
            CHECK(d.minimum==k.minimum&&d.maximum==k.maximum&&d.defaultValue==k.initial&&d.skew==1&&d.choices==k.choices);
            CHECK(std::find(reach.begin(),reach.end(),*index)!=reach.end());
            for(int m=int(NativeModule::drive);m<=int(NativeModule::dynamicEq);++m)if(NativeModule(m)!=NativeModule::delay){
                CHECK(!rackParameterActive(NativeModule(m),k.key));
                const auto other=unitReach(NativeModule(m),slot);CHECK(std::find(other.begin(),other.end(),*index)==other.end());
            }
        }
    }
}
void stereoKeysMapIntoDelaySettings()
{
    std::cout<<"SECTION stereo_keys_map_into_delay_settings_and_refuse_out_of_range\n";
    for(std::uint8_t slot=0;slot<8;++slot){
        SoundDocument sound;RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
        const auto& neutral=patch.effects[slot].delay;
        CHECK(neutral.rightRatio==1&&!neutral.pingPong&&neutral.note==0);
        for(const float end:{0.f,1.f}){
            for(std::size_t field=0;field<3;++field)if(const auto index=stereoIndex(slot,field))sound.values[*index]=end;
            CHECK(makeRoutedPatch(sound,patch));const auto& d=patch.effects[slot].delay;
            CHECK(d.rightRatio==(end?4.f:.25f)&&d.pingPong==(end==1)&&d.note==(end?21:0));
        }
        if(const auto index=stereoIndex(slot,2)){sound.values[*index]=11.f/21;CHECK(makeRoutedPatch(sound,patch)&&patch.effects[slot].delay.note==11);}
        for(const float bad:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN()})if(const auto index=stereoIndex(slot,0)){
            auto invalid=sound;invalid.values[*index]=bad;const auto before=patch;CHECK(!makeRoutedPatch(invalid,patch));CHECK(patch==before);
        }
    }
    SoundDocument sound;RoutedPatch base;CHECK(makeRoutedPatch(sound,base));CHECK(validateRoutedPatch(base));
    for(int bad=0;bad<6;++bad){
        auto p=base;auto& d=p.effects[3].delay;
        if(bad==0)d.rightRatio=.24f;
        if(bad==1)d.rightRatio=4.01f;
        if(bad==2)d.rightRatio=std::numeric_limits<float>::quiet_NaN();
        if(bad==3)d.rightRatio=std::numeric_limits<float>::infinity();
        if(bad==4)d.note=-1;
        if(bad==5)d.note=22;
        CHECK(!validateRoutedPatch(p));
    }
}
void reject(juce::MemoryBlock malformed,const SoundDocument& decoded)
{
    const auto digest=juce::SHA256(malformed.getData(),malformed.getSize()-32).getRawData();
    std::memcpy(static_cast<char*>(malformed.getData())+malformed.getSize()-32,digest.getData(),32);
    auto output=decoded;CHECK(!decodeSoundState(malformed.getData(),malformed.getSize(),output));CHECK(output==decoded);
}
void stereoStateAndOldMigration()
{
    std::cout<<"SECTION stereo_current_state_and_actual_sound20_project19_migration\n";
    const auto defaults=defaultParameterValues();
    const std::array<const char*,3> sounds{"default-v20.soundstate","rich-v20.soundstate","embedded-v20.soundstate"};
    // Current compatibility appends twenty-four bytes plus the four-byte name length.
    const std::size_t records=2063,growth=9*(parameterCount-records)+28;
    for(const auto* name:sounds){
        const auto bytes=fixture(name);
        SoundDocument decoded;const bool read=decodeSoundState(bytes.getData(),bytes.getSize(),decoded);CHECK(read);if(!read)continue;
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(16);
        for(std::size_t record=0;record<records;++record){const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();const auto index=parameterIndexForKey(key);CHECK(index&&*index<records&&decoded.values[*index]==value);}
        for(std::size_t i=records;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]);
        RoutedPatch patch;CHECK(makeRoutedPatch(decoded,patch));
        for(const auto& effect:patch.effects)CHECK(effect.delay.rightRatio==1&&!effect.delay.pingPong&&effect.delay.note==0);
        const auto feedback=rackParameterIndex(NativeModule::delay,2,"delay.feedback"),mix=rackParameterIndex(NativeModule::delay,2,"delay.mix");
        if(juce::String(name)!=sounds[0]&&feedback&&mix){
            CHECK(decoded.layout.effects[2].kind==EffectKind::delay&&decoded.effectLocks[2]&&decoded.controlLocks[*feedback]);
            CHECK(decoded.macros[0].targetKey==parameterRegistry()[*mix].stateKey&&decoded.macros[0].amount==.21f&&decoded.values[*mix]==.83f);
            CHECK(patch.effects[2].delay.synced&&patch.effects[2].delay.glide);
        }
        if(juce::String(name)==sounds[2])
            {CHECK(decoded.personalities&&decoded.personalities->instances.size()==1&&decoded.personalities->instances[0].design.units[0].defaults.size()==13);if(decoded.personalities&&decoded.personalities->instances.size()==1){CHECK(decoded.personalities->instances[0].design.id==juce::Uuid("330cb53b-cd9b-443c-94d8-2700bc8b88f7"));CHECK(!decoded.personalities->instances[0].package);}}
        // A new key stays outside the signed old domain: as a value record, a macro target or a nested default.
        const auto future=stereoIndex(0,0);CHECK(future.has_value());
        if(future){
            const auto key=parameterRegistry()[*future].stateKey;
            auto malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16,&key,4);reject(malformed,decoded);
            malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16+records*8+78,&key,4);reject(malformed,decoded);
            if(decoded.personalities){
                auto candidate=decoded;auto personalities=std::make_shared<RackPersonalityState>(*decoded.personalities);
                personalities->instances[0].design.units[0].defaults.push_back({0,"delay.right_ratio",.5f});candidate.personalities=personalities;
                juce::MemoryBlock modern;const bool encoded=encodeSoundState(candidate,modern);CHECK(encoded);
                if(encoded){
                    juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,oldSound20NumericBytes+4+oldSound20CompatibilityBytes,decoded,control));CHECK(control==bytes);
                    SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==decoded);
                    juce::MemoryBlock nested;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,oldSound20NumericBytes+4+oldSound20CompatibilityBytes,candidate,nested));reject(nested,decoded);
                }
            }
        }
        juce::MemoryBlock upgraded;CHECK(encodeSoundState(decoded,upgraded));CHECK(upgraded.getSize()==bytes.getSize()+growth+(decoded.personalities?decoded.personalities->instances.size():0));
        juce::MemoryInputStream header(upgraded,false);header.setPosition(4);CHECK(header.readInt()==31&&header.readInt()==4519);
        SoundDocument again;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),again)&&again==decoded);
    }
    SoundDocument rich;const auto richBytes=fixture(sounds[1]);CHECK(decodeSoundState(richBytes.getData(),richBytes.getSize(),rich));
    const auto oldProject=fixture("rich-v19.airackproject");
    juce::MemoryInputStream oldHeader(oldProject,false);oldHeader.setPosition(4);CHECK(oldHeader.readShort()==19);
    ProjectDocument project;CHECK(decodeProjectState(oldProject.getData(),oldProject.getSize(),project));CHECK(project.sound==rich);
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<3;++field)if(const auto index=stereoIndex(slot,field))project.sound.values[*index]=field==0?.4f:1.f;
    juce::MemoryBlock current;CHECK(encodeProjectState(project,current));CHECK(current.getSize()==oldProject.getSize()+growth);
    juce::MemoryInputStream header(current,false);header.setPosition(4);CHECK(header.readShort()==30);header.setPosition(24);CHECK(header.readInt()==31&&header.readInt()==4519);
    ProjectDocument again;CHECK(decodeProjectState(current.getData(),current.getSize(),again)&&again==project);
}
const juce::File acceptedEq=fixtures.getSiblingFile("delay-stereo-accepted-eq");
juce::MemoryBlock acceptedFixture(const char* name)
{ juce::MemoryBlock bytes; CHECK(acceptedEq.getChildFile(name).loadFileAsData(bytes)); return bytes; }
void stereoKeysAfterAcceptedEqFormats()
{
    std::cout<<"SECTION stereo_keys_after_accepted_eq_sound21_project20\n";
    const std::array<std::pair<const char*,const char*>,6> pinned {{
        {"registry-v21.json","190c858307b853e569674a9dfeb8039b2f0b29cd1726ea1dfa9e1dce4156dccb"},
        {"default-v21.soundstate","c3e10638971089603605a0552f92322bfc63e697284131022c65dbfff50025fe"},
        {"rich-v21.soundstate","2e9b5a2d6ea8181af02e3b10984a71241bd57b46618135bf3f828aa643b00a96"},
        {"embedded-v21.soundstate","3d9d4542ff6ba94312846fc70d7eafafe759bbde1e98a0015d2f493b29fc0666"},
        {"rich-v20.airackproject","cf18eda70ce44da684b9fc90e30f54245d102da7014f4e378b4ecb0b4de2a4e3"},
        {"embedded-v20.airackproject","0869b6f913c3a28528e52ee0be777b50039700808d5e233283e4ff958e346580"}}};
    for(const auto& [name,hash]:pinned)CHECK(juce::SHA256(acceptedFixture(name)).toHexString()==hash);
    registryPrefix(acceptedFixture("registry-v21.json"),2191);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    const std::size_t records=2191,macros=16+8*records+78,locks=macros+64,growth=9*(parameterCount-records)+28;
    const std::array<const char*,3> sounds{"default-v21.soundstate","rich-v21.soundstate","embedded-v21.soundstate"};
    std::array<SoundDocument,3> decodedSounds;
    for(std::size_t n=0;n<sounds.size();++n){
        const auto bytes=acceptedFixture(sounds[n]);auto& decoded=decodedSounds[n];
        CHECK(bytes.getSize()>=oldSound21StateSize);if(bytes.getSize()<oldSound21StateSize)continue;
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(4);CHECK(stream.readInt()==21&&stream.readInt()==2191);
        const bool read=decodeSoundState(bytes.getData(),bytes.getSize(),decoded);CHECK(read);if(!read)continue;
        const auto* raw=static_cast<const unsigned char*>(bytes.getData());stream.setPosition(16);
        for(std::size_t record=0;record<records;++record){
            const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();
            CHECK(key==registry[record].stateKey&&decoded.values[record]==value&&decoded.controlLocks[record]==(raw[locks+record]==1));
        }
        stream.setPosition(juce::int64(macros));
        for(const auto& macro:decoded.macros){CHECK(std::uint32_t(stream.readInt())==macro.targetKey);CHECK(stream.readFloat()==macro.amount);}
        for(std::size_t i=delayStereoParameterStart;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]&&!decoded.controlLocks[i]);
        RoutedPatch patch;CHECK(makeRoutedPatch(decoded,patch));
        for(const auto& effect:patch.effects)CHECK(effect.delay.rightRatio==1&&!effect.delay.pingPong&&effect.delay.note==0);
        const auto feedback=rackParameterIndex(NativeModule::delay,2,"delay.feedback"),mix=rackParameterIndex(NativeModule::delay,2,"delay.mix"),wobble=rackParameterIndex(NativeModule::delay,2,"delay.wobble");
        if(n==1&&feedback&&mix&&wobble){
            CHECK(decoded.layout.effects[2].kind==EffectKind::delay&&decoded.effectLocks[2]&&decoded.controlLocks[*feedback]&&decoded.values[0]==.3f&&decoded.values[*mix]==.83f);
            CHECK(decoded.macros[0].targetKey==registry[*mix].stateKey&&decoded.macros[0].amount==.21f);
            CHECK(decoded.macros[1].targetKey==registry[*wobble].stateKey&&decoded.macros[1].amount==-.17f);
            CHECK(patch.effects[2].delay.synced&&patch.effects[2].delay.glide);
            for(std::size_t i=dynamicEqExtensionParameterStart;i<delayStereoParameterStart;++i){
                const int count=int(registry[i].choices.size());
                CHECK(count>1&&decoded.values[i]==float(int(1+i%7)%count)/float(count-1)&&decoded.controlLocks[i]==(i%3==0));
            }
        }
        if(n==2)CHECK(decoded.personalities&&decoded.personalities->instances.size()==1&&decoded.personalities->instances[0].design.name=="Oxide Return"
            &&decoded.personalities->instances[0].design.units[0].defaults.size()==13);if(n==2&&decoded.personalities&&decoded.personalities->instances.size()==1){CHECK(decoded.personalities->instances[0].design.id==juce::Uuid("330cb53b-cd9b-443c-94d8-2700bc8b88f7"));CHECK(!decoded.personalities->instances[0].package);}
        // A delay stereo key stays outside the accepted EQ domain: as a value record, a macro target or a nested default.
        if(const auto future=stereoIndex(0,0)){
            const auto key=registry[*future].stateKey;
            auto malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16,&key,4);reject(malformed,decoded);
            malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+macros,&key,4);reject(malformed,decoded);
            if(decoded.personalities){
                auto candidate=decoded;auto personalities=std::make_shared<RackPersonalityState>(*decoded.personalities);
                personalities->instances[0].design.units[0].defaults.push_back({0,"delay.right_ratio",.5f});candidate.personalities=personalities;
                juce::MemoryBlock modern;const bool encoded=encodeSoundState(candidate,modern);CHECK(encoded);
                if(encoded){
                    juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,oldSound21NumericBytes+4+oldSound21CompatibilityBytes,decoded,control));CHECK(control==bytes);
                    SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==decoded);
                    juce::MemoryBlock nested;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,oldSound21NumericBytes+4+oldSound21CompatibilityBytes,candidate,nested));reject(nested,decoded);
                }
            }
        }
        juce::MemoryBlock upgraded;CHECK(encodeSoundState(decoded,upgraded));CHECK(upgraded.getSize()==bytes.getSize()+growth+(decoded.personalities?decoded.personalities->instances.size():0));
        juce::MemoryInputStream header(upgraded,false);header.setPosition(4);CHECK(header.readInt()==31&&header.readInt()==4519);
        SoundDocument again;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),again)&&again==decoded);
    }
    for(const auto& [name,n]:std::array<std::pair<const char*,std::size_t>,2>{{{"rich-v20.airackproject",1},{"embedded-v20.airackproject",2}}}){
        const auto bytes=acceptedFixture(name);
        juce::MemoryInputStream oldHeader(bytes,false);oldHeader.setPosition(4);CHECK(oldHeader.readShort()==20);oldHeader.setPosition(24);CHECK(oldHeader.readInt()==21&&oldHeader.readInt()==2191);
        ProjectDocument project;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),project));CHECK(project.sound==decodedSounds[n]);
        for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<3;++field)if(const auto index=stereoIndex(slot,field))project.sound.values[*index]=field==0?.4f:1.f;
        juce::MemoryBlock current;CHECK(encodeProjectState(project,current));CHECK(current.getSize()==bytes.getSize()+growth+(project.sound.personalities?project.sound.personalities->instances.size():0));
        juce::MemoryInputStream header(current,false);header.setPosition(4);CHECK(header.readShort()==30);header.setPosition(24);CHECK(header.readInt()==31&&header.readInt()==4519);
        ProjectDocument again;CHECK(decodeProjectState(current.getData(),current.getSize(),again)&&again==project);
    }
}
void stereoKeysInAiCatalog()
{
    std::cout<<"SECTION stereo_keys_in_the_ai_catalog\n";
    ProjectDocument source;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=4;add.effect=EffectKind::delay;
    CHECK(editRackLayout(source.sound.layout,add,source.sound.layout)==RackEditResult::applied);
    juce::var provider;CHECK(ai::providerCatalogFor(source,provider));
    const auto* keys=provider["module_keys"]["delay"].getArray();CHECK(keys&&keys->size()==16);if(!keys||keys->size()!=16)return;
    const auto* sets=provider["choice_sets"].getArray();CHECK(sets!=nullptr);if(!sets)return;
    for(std::size_t field=0;field<3;++field){
        const auto& k=stereoKeys[field];const auto& row=(*keys)[int(13+field)];
        CHECK(row[0].toString()==k.key&&row[1].toString()==k.unit&&int(row[2])==int(k.kind));
        CHECK(float(double(row[3]))==k.minimum&&float(double(row[4]))==k.maximum&&float(double(row[5]))==k.initial&&double(row[6])==1);
        const auto set=int(row[7]);CHECK(set>=0&&set<sets->size());if(set<0||set>=sets->size())continue;
        const auto* names=(*sets)[set].getArray();CHECK(names&&std::size_t(names->size())==k.choices.size());
        if(names)for(int c=0;c<names->size()&&std::size_t(c)<k.choices.size();++c)CHECK((*names)[c].toString()==k.choices[std::size_t(c)]);
    }
    const auto* controls=provider["controls"].getArray();CHECK(controls!=nullptr);if(!controls)return;
    for(std::size_t field=0;field<3;++field){
        const auto index=stereoIndex(4,field);if(!index)continue;
        const auto id=parameterRegistry()[*index].id;
        CHECK(std::any_of(controls->begin(),controls->end(),[&](const juce::var& row){return row[0].toString()==id&&bool(row[9]);}));
    }
}
}
int main(int argc,char** argv)
{
    if(argc==3&&juce::String(argv[1])=="--capture-legacy")return captureLegacy(juce::File(juce::String::fromUTF8(argv[2])))?0:1;
    if(argc!=1)return 2;
    legacyFixturesPinned();stereoKeysRegistryAndFrozenPrefix();stereoKeysMapIntoDelaySettings();stereoStateAndOldMigration();stereoKeysAfterAcceptedEqFormats();stereoKeysInAiCatalog();
    std::cout<<"Delay stereo: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
