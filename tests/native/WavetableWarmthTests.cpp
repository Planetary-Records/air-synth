#include "LegacyPersonalityProjection.h"
#include "FrozenRackExamples.h"
#include "AiCatalog.h"
#include "ProposalDocument.h"
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
// Today's registry and states, before the wavetable warmth keys: a rich sound has an enabled wavetable synth in
// slot 2 at revision 2 with every synth.2.wavetable key away from its default, and the embedded one adds a placed
// Prism Relay in slot 3.
bool captureLegacy(const juce::File& directory)
{
    if (parameterCount != 3503 || directory.exists() || !directory.createDirectory().wasOk()) return false;
    const auto write = [&](const char* name, const juce::MemoryBlock& bytes) {
        const auto file = directory.getChildFile(name);
        if (file.exists() || !file.replaceWithData(bytes.getData(), bytes.getSize())) return false;
        std::cout << "WAVETABLE_WARMTH_LEGACY " << name << " bytes=" << bytes.getSize() << " sha256=" << juce::SHA256(bytes).toHexString() << '\n';
        return true;
    };
    const auto registry = parameterRegistry();
    juce::Array<juce::var> rows;
    for (const auto& d : registry) {
        auto* row = new juce::DynamicObject;
        row->setProperty("id", d.id); row->setProperty("name", d.name); row->setProperty("unit", d.unit);
        row->setProperty("kind", int(d.kind)); row->setProperty("minimum", double(d.minimum)); row->setProperty("maximum", double(d.maximum));
        row->setProperty("default", double(d.defaultValue)); row->setProperty("skew", double(d.skew)); row->setProperty("key", juce::int64(d.stateKey));
        juce::Array<juce::var> choices; for (const auto& choice : d.choices) choices.add(choice); row->setProperty("choices", choices);
        rows.add(juce::var(row));
    }
    const auto json = juce::JSON::toString(rows, true, 17);
    if (!write("registry-v26.json", juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()))) return false;
    SoundDocument empty, rich;
    RackEdit edit; edit.kind=RackEditKind::addSynth; edit.slot=1;
    if (editRackLayout(rich.layout, edit, rich.layout) != RackEditResult::applied) return false;
    const auto enabled = rackParameterIndex(NativeModule::wavetable, 1, "enabled"), engine = rackParameterIndex(NativeModule::wavetable, 1, "engine");
    const auto cutoff = rackParameterIndex(NativeModule::wavetable, 1, "wavetable.filter.cutoff");
    const auto position = rackParameterIndex(NativeModule::wavetable, 1, "wavetable.osc.1.position");
    if (!enabled || !engine || !cutoff || !position) return false;
    rich.values[*enabled]=1; rich.values[*engine]=1;
    const auto defaults = defaultParameterValues();
    int moved=0;
    for (std::size_t i = 0; i < registry.size(); ++i) {
        if (!registry[i].id.startsWith("synth.2.wavetable.")) continue;
        if (const auto steps = int(registry[i].choices.size()); steps > 1)
            rich.values[i] = float((int(std::lround(defaults[i] * float(steps - 1))) + 1) % steps) / float(steps - 1);
        else rich.values[i] = defaults[i] < .5f ? defaults[i] + .31f : defaults[i] - .29f;
        if (rich.values[i] == defaults[i]) return false;
        ++moved;
    }
    std::cout << "WAVETABLE_WARMTH_LEGACY moved=" << moved << '\n';
    rich.compatibility.synth[1][1]=modernSynthAlgorithmRevision;
    rich.synthLocks[1]=true; rich.controlLocks[*cutoff]=true;
    rich.macros[0]={registry[*position].stateKey,.21f};
    const auto factory = airack::test::frozenFactoryRackDesigns();
    const auto prism = std::find_if(factory.begin(), factory.end(), [](const RackDesign& d) { return d.name == "Prism Relay"; });
    if (prism == factory.end() || prism->units.size() != 1 || prism->units[0].module != NativeModule::wavetable
        || prism->units[0].algorithmRevision != modernSynthAlgorithmRevision) return false;
    RackOperation place; place.kind=RackOperationKind::place; place.design=*prism;
    place.newInstance=juce::Uuid("3f0b6c2e-8d41-4a97-b5e3-c19a7d24f68b"); place.destination={RackRole::synth,masterChain,2}; place.allocation={{0,2}};
    SoundDocument embedded; RackDifference delta;
    if (makeRackCandidate(rich,place,SoundEditOrigin::manual,embedded,delta)!=RackCandidateStatus::ready) return false;
    juce::MemoryBlock bytes;
    for (const auto& [name, sound] : std::array<std::pair<const char*,const SoundDocument*>,3> {{{"default-v26.soundstate",&empty},{"rich-v26.soundstate",&rich},{"embedded-v26.soundstate",&embedded}}})
        if (!encodeSoundState(*sound,bytes) || !write(name,bytes)) return false;
    ProjectDocument project; project.sound=rich;
    return encodeProjectState(project,bytes) && write("rich-v25.airackproject",bytes);
}
const juce::File fixtures(juce::String(RACKSYNTH_FIXTURE_DIR));
juce::MemoryBlock fixture(const char* name)
{ juce::MemoryBlock bytes; CHECK(fixtures.getChildFile(name).loadFileAsData(bytes)); return bytes; }
void legacyFixturesPinned()
{
    std::cout<<"SECTION legacy_fixtures_registry26_sound26_project25\n";
    const std::array<std::pair<const char*,const char*>,5> fixtureHashes {{
        {"registry-v26.json","ff57385d518fb7c6d3d4fde12445194fc13c07a093d2b5e81fdcb469a4c55a40"},
        {"default-v26.soundstate","83222877da76c654ce066e9c85199b7a968f788044e156c04e831e99dd59aa08"},
        {"rich-v26.soundstate","6b20709431e30795c6574e827e9758ad37afc9aee6cb7e8984b52a9d7c5ac715"},
        {"embedded-v26.soundstate","8dd89abf4deb20898f5acd63ed64b8dd431c5951331b327c9af6282955985daf"},
        {"rich-v25.airackproject","b6bdccb62b90a471ecbb7e915a79f37289f13955e2b7b7559547e33a6a14c4aa"}}};
    for(const auto& [name,hash]:fixtureHashes)CHECK(juce::SHA256(fixture(name)).toHexString()==hash);
}
struct WarmthKey { const char* key; const char* name; const char* unit; ParameterKind kind; float minimum, maximum, initial, skew; std::vector<juce::String> choices; };
const std::vector<juce::String> voiceNames {"1","2","3","4","5","6","7"};
const WarmthKey warmthKeys[] {
    {"wavetable.osc.1.unison","Osc 1 unison","voices",ParameterKind::choice,0,6,0,1,voiceNames},
    {"wavetable.osc.1.unison_cents","Osc 1 unison spread","cent",ParameterKind::continuous,0,100,10,1,{}},
    {"wavetable.osc.2.unison","Osc 2 unison","voices",ParameterKind::choice,0,6,0,1,voiceNames},
    {"wavetable.osc.2.unison_cents","Osc 2 unison spread","cent",ParameterKind::continuous,0,100,10,1,{}},
    {"wavetable.unison_width","Unison width","",ParameterKind::continuous,0,1,1,1,{}},
    {"wavetable.drift","Drift","",ParameterKind::continuous,0,1,0,1,{}},
    {"wavetable.filter.type","Filter type","",ParameterKind::choice,0,3,0,1,{"Clean","Ladder","Classic","Vowel"}},
    {"wavetable.filter.vowel_size","Vowel size","x",ParameterKind::continuous,.5f,2,1,.63092975f,{}},
    {"wavetable.amp.attack_curve","Amp attack curve","",ParameterKind::continuous,-1,1,.25f,1,{}}};
std::optional<std::size_t> warmthIndex(std::uint8_t slot,std::size_t field)
{ return rackParameterIndex(NativeModule::wavetable,slot,warmthKeys[field].key); }
bool neutral(const LayerSettings& l)
{
    return l.oscillators[0].unison==1&&l.oscillators[1].unison==1&&l.oscillators[0].unisonCents==10&&l.oscillators[1].unisonCents==10
        &&l.unisonWidth==1&&l.drift==0&&l.filterType==FilterType::clean&&std::abs(l.vowelSize-1)<1e-6f&&l.attackCurve==.25f;
}
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
void warmthKeysRegistered()
{
    std::cout<<"SECTION warmth_keys_appended_after_frozen_3503_descriptors\n";
    const auto registry=parameterRegistry();
    CHECK(registry.size()==parameterCount&&wavetableWarmthParameterStart==3503&&wavetableWarmthParametersPerSlot==9&&eraReverbParameterStart==3539);
    registryPrefix(fixture("registry-v26.json"),3503);
    for(std::uint8_t slot=0;slot<4;++slot)for(std::size_t field=0;field<9;++field){
        const auto& k=warmthKeys[field];const auto index=warmthIndex(slot,field);
        CHECK(index==std::optional<std::size_t>(wavetableWarmthParameterStart+9*slot+field));if(!index||*index>=registry.size())continue;
        const auto& d=registry[*index];const auto n=juce::String(slot+1);
        CHECK(d.id=="synth."+n+"."+k.key);CHECK(d.name=="Synth "+n+" Wavetable "+k.name);CHECK(d.unit==k.unit&&d.kind==k.kind);
        CHECK(d.minimum==k.minimum&&d.maximum==k.maximum&&d.defaultValue==k.initial&&d.skew==k.skew&&d.choices==k.choices);
        CHECK(rackParameterActive(NativeModule::wavetable,k.key)&&!rackParameterActive(NativeModule::analog,k.key));
        for(int m=int(NativeModule::drive);m<=int(NativeModule::spaceReverb);++m)CHECK(!rackParameterActive(NativeModule(m),k.key));
    }
    // Unison 1 voice, spread 10 cents, width 1, no drift, Clean, vowel size 1 at mid-travel, today's attack curve.
    const auto defaults=defaultParameterValues();const auto w=wavetableWarmthParameterStart;
    CHECK(defaults[w]==0&&defaults[w+2]==0&&defaults[w+4]==1&&defaults[w+5]==0&&defaults[w+6]==0&&defaults[w+8]==.625f);
    CHECK(std::abs(defaults[w+1]-.1f)<1e-6f&&std::abs(defaults[w+3]-.1f)<1e-6f&&std::abs(defaults[w+7]-.5f)<1e-6f);
}
void warmthKeysMapIntoWavetableLayers()
{
    std::cout<<"SECTION warmth_keys_map_into_wavetable_layers_and_refuse_out_of_range\n";
    for(std::uint8_t slot=0;slot<4;++slot){
        const auto engine=rackParameterIndex(NativeModule::wavetable,slot,"engine");CHECK(engine.has_value());if(!engine)continue;
        for(const float wavetable:{0.f,1.f}){
            auto values=defaultParameterValues();values[*engine]=wavetable;SynthPatch patch;
            CHECK(makeSynthPatch(values,patch));for(const auto& l:patch.layers)CHECK(neutral(l));
            for(const float end:{0.f,1.f}){
                for(std::size_t field=0;field<9;++field)values[wavetableWarmthParameterStart+9*slot+field]=end;
                CHECK(makeSynthPatch(values,patch));const auto& l=patch.layers[slot];
                for(std::size_t other=0;other<4;++other)if(other!=slot||wavetable==0)CHECK(neutral(patch.layers[other]));
                if(wavetable==0)continue;
                for(const auto& o:l.oscillators)CHECK(o.unison==(end?7:1)&&o.unisonCents==(end?100.f:0.f));
                CHECK(l.unisonWidth==end&&l.drift==end&&l.filterType==(end?FilterType::vowel:FilterType::clean));
                CHECK(std::abs(l.vowelSize-(end?2.f:.5f))<1e-6f&&l.attackCurve==(end?1.f:-1.f));
            }
            const auto base=wavetableWarmthParameterStart+9*slot;
            values[base]=2.f/6;values[base+2]=5.f/6;values[base+6]=1.f/3;
            CHECK(makeSynthPatch(values,patch));
            if(wavetable==1)CHECK(patch.layers[slot].oscillators[0].unison==3&&patch.layers[slot].oscillators[1].unison==6&&patch.layers[slot].filterType==FilterType::ladder);
            values[base+6]=2.f/3;CHECK(makeSynthPatch(values,patch));if(wavetable==1)CHECK(patch.layers[slot].filterType==FilterType::classic);
            auto invalid=values;invalid[base]=.25f;const auto before=patch;CHECK(!makeSynthPatch(invalid,patch));CHECK(patch==before);
        }
    }
    const auto nan=std::numeric_limits<float>::quiet_NaN(),infinity=std::numeric_limits<float>::infinity();
    const SynthPatch base;CHECK(validateSynthPatch(base));
    for(int bad=0;bad<17;++bad){
        auto p=base;auto& l=p.layers[2];auto& o=l.oscillators[1];
        switch(bad){
            case 0:o.unison=0;break;case 1:o.unison=8;break;
            case 2:o.unisonCents=-.01f;break;case 3:o.unisonCents=100.01f;break;case 4:o.unisonCents=nan;break;
            case 5:l.unisonWidth=-.01f;break;case 6:l.unisonWidth=1.01f;break;
            case 7:l.drift=-.01f;break;case 8:l.drift=1.01f;break;
            case 9:l.filterType=FilterType(-1);break;case 10:l.filterType=FilterType(4);break;
            case 11:l.vowelSize=.49f;break;case 12:l.vowelSize=2.01f;break;case 13:l.vowelSize=nan;break;
            case 14:l.attackCurve=-1.01f;break;case 15:l.attackCurve=1.01f;break;default:l.attackCurve=infinity;break;
        }
        CHECK(!validateSynthPatch(p));
    }
}
void reject(juce::MemoryBlock malformed,const SoundDocument& decoded)
{
    const auto digest=juce::SHA256(malformed.getData(),malformed.getSize()-32).getRawData();
    std::memcpy(static_cast<char*>(malformed.getData())+malformed.getSize()-32,digest.getData(),32);
    auto output=decoded;CHECK(!decodeSoundState(malformed.getData(),malformed.getSize(),output));CHECK(output==decoded);
}
// A current state as Sound26 writes it: its first 3503 records and locks and its 17 effect banks, the rest unchanged.
// Every key after them must be at its default and unlocked.
juce::MemoryBlock asSound26(const SoundDocument& sound)
{
    juce::MemoryBlock current,old;CHECK(encodeSoundState(sound,current));
    const auto* raw=static_cast<const char*>(current.getData());
    const std::size_t records=oldSound26ParameterCount,layout=16+8*parameterCount,locks=layout+78+64,compatibility=currentSoundNumericBytes;
    juce::MemoryOutputStream s(old,false);
    s.writeInt(0x49535241);s.writeInt(26);s.writeInt(int(records));s.writeInt(0);
    s.write(raw+16,8*records);s.write(raw+layout,78+64+records);
    s.write(raw+locks+parameterCount,compatibility-locks-parameterCount);
    s.writeInt(int(oldSound26CompatibilityBytes));s.write(raw+compatibility+4,5);s.writeByte(17);s.write(raw+compatibility+10,10);
    for(std::size_t slot=0;slot<8;++slot)s.write(raw+compatibility+20+effectBankCount*slot,17);
    const auto rest=compatibility+4+kernelCompatibilitySectionBytes;
    juce::MemoryBlock section;const bool projected=airack::test::legacyPersonalitySection(current,section);CHECK(projected);if(!projected)return {};
    const auto* bytes=reinterpret_cast<const unsigned char*>(raw);const auto length=std::size_t(bytes[rest])|(std::size_t(bytes[rest+1])<<8)|(std::size_t(bytes[rest+2])<<16)|(std::size_t(bytes[rest+3])<<24);
    s.write(section.getData(),section.getSize());s.write(raw+rest+4+length,current.getSize()-32-rest-4-length);
    s.writeRepeatedByte(0,32);s.flush();
    const auto digest=juce::SHA256(old.getData(),old.getSize()-32).getRawData();
    std::memcpy(static_cast<char*>(old.getData())+old.getSize()-32,digest.getData(),32);
    return old;
}
void warmthStateAndSound26Migration()
{
    std::cout<<"SECTION warmth_current_state_and_actual_sound26_project25_migration\n";
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    const std::size_t records=3503,macros=16+8*records+78,locks=macros+64,growth=9*(parameterCount-records)+kernelCompatibilitySectionBytes-oldSound26CompatibilityBytes;
    const std::array<const char*,3> sounds{"default-v26.soundstate","rich-v26.soundstate","embedded-v26.soundstate"};
    const auto cutoff=rackParameterIndex(NativeModule::wavetable,1,"wavetable.filter.cutoff"),position=rackParameterIndex(NativeModule::wavetable,1,"wavetable.osc.1.position");
    CHECK(cutoff&&position);if(!cutoff||!position)return;
    std::array<SoundDocument,3> decodedSounds;
    for(std::size_t n=0;n<sounds.size();++n){
        const auto bytes=fixture(sounds[n]);auto& decoded=decodedSounds[n];
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(4);CHECK(stream.readInt()==26&&stream.readInt()==3503);
        const bool read=decodeSoundState(bytes.getData(),bytes.getSize(),decoded);CHECK(read);if(!read)continue;
        const auto* raw=static_cast<const unsigned char*>(bytes.getData());stream.setPosition(16);
        for(std::size_t record=0;record<records;++record){
            const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();
            CHECK(key==registry[record].stateKey&&decoded.values[record]==value&&decoded.controlLocks[record]==(raw[locks+record]==1));
        }
        for(std::size_t i=records;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]&&!decoded.controlLocks[i]);
        SynthPatch patch;CHECK(makeSynthPatch(decoded.values,patch));for(const auto& l:patch.layers)CHECK(neutral(l));
        if(n>0){
            CHECK(decoded.layout.synthUse[1]==SlotUse::present&&decoded.synthLocks[1]&&decoded.controlLocks[*cutoff]&&decoded.compatibility.synth[1][1]==modernSynthAlgorithmRevision);
            CHECK(decoded.macros[0].targetKey==registry[*position].stateKey&&decoded.macros[0].amount==.21f);
            CHECK(patch.layers[1].enabled&&patch.layers[1].engine==EngineKind::wavetable);
        }
        if(n==2)CHECK(decoded.personalities&&decoded.personalities->instances.size()==1&&decoded.personalities->instances[0].design.name=="Prism Relay"
            &&decoded.personalities->instances[0].bindings==(std::vector<UnitBinding>{{0,2}}));
        // A warmth key stays outside the signed Sound26 domain: as a value record, a macro target or a nested default.
        if(const auto future=warmthIndex(0,5)){
            const auto key=registry[*future].stateKey;
            auto malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+16,&key,4);reject(malformed,decoded);
            malformed=bytes;std::memcpy(static_cast<char*>(malformed.getData())+macros,&key,4);reject(malformed,decoded);
            if(decoded.personalities){
                auto candidate=decoded;auto personalities=std::make_shared<RackPersonalityState>(*decoded.personalities);
                personalities->instances[0].design.units[0].defaults.push_back({0,"wavetable.drift",.5f});candidate.personalities=personalities;
                juce::MemoryBlock modern;const bool encoded=encodeSoundState(candidate,modern);CHECK(encoded);
                if(encoded){
                    juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,oldSound26NumericBytes+4+oldSound26CompatibilityBytes,decoded,control));CHECK(control==bytes);
                    SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==decoded);
                    juce::MemoryBlock nested;CHECK(airack::test::oldEnvelopeWithPersonality(bytes,oldSound26NumericBytes+4+oldSound26CompatibilityBytes,candidate,nested));reject(nested,decoded);
                }
            }
        }
        if(n==2&&decoded.personalities&&decoded.personalities->instances.size()==1){CHECK(decoded.personalities->instances[0].design.id==juce::Uuid("91387715-13d3-4c57-a479-12251842b402"));CHECK(!decoded.personalities->instances[0].package);}
        const auto rebuilt=asSound26(decoded);CHECK(rebuilt==bytes);
        juce::MemoryBlock upgraded;CHECK(encodeSoundState(decoded,upgraded));CHECK(upgraded.getSize()==bytes.getSize()+growth+(decoded.personalities?decoded.personalities->instances.size():0));
        juce::MemoryInputStream header(upgraded,false);header.setPosition(4);CHECK(header.readInt()==31&&header.readInt()==4519);
        SoundDocument again;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),again)&&again==decoded);
    }
    // Sound26 already held Space Reverb units, so its layout may name them.
    SoundDocument space;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=5;add.effect=EffectKind::spaceReverb;
    CHECK(editRackLayout(space.layout,add,space.layout)==RackEditResult::applied);
    if(const auto mix=rackParameterIndex(NativeModule::spaceReverb,5,"space_reverb.mix"))space.values[*mix]=.37f;
    const auto spaceBytes=asSound26(space);SoundDocument spaceRead;
    CHECK(decodeSoundState(spaceBytes.getData(),spaceBytes.getSize(),spaceRead)&&spaceRead==space);
    // And its retained designs could hold Space Reverb units and keys.
    const auto factory=airack::test::frozenFactoryRackDesigns();
    const auto lumen=std::find_if(factory.begin(),factory.end(),[](const RackDesign& d){return d.name=="Lumen Field";});CHECK(lumen!=factory.end());
    if(lumen!=factory.end()){
        RackOperation place;place.kind=RackOperationKind::place;place.design=*lumen;
        place.newInstance=juce::Uuid("5b8e2d71-0c4f-4e93-a6d2-7f19c3b80e54");place.destination={RackRole::fx,masterChain,0};place.allocation={{0,4}};
        SoundDocument placed;RackDifference delta;CHECK(makeRackCandidate(SoundDocument{},place,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
        const auto placedBytes=asSound26(placed);SoundDocument placedRead;
        auto expected=placed;CHECK(expected.personalities&&expected.personalities->instances.size()==1);
        if(expected.personalities&&expected.personalities->instances.size()==1){auto racks=std::make_shared<RackPersonalityState>(*expected.personalities);CHECK(racks->instances[0].design.id==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0")&&!racks->instances[0].package);racks->instances[0].package=RackPackageRef{"lumen-field","1.0.0","5d24c141a237a89030587ba8c9f918461511f8ee04a2e2e1973618f1b5f22f40"};expected.personalities=racks;}
        CHECK(decodeSoundState(placedBytes.getData(),placedBytes.getSize(),placedRead));CHECK(placedRead==expected);
    }
    const auto oldProject=fixture("rich-v25.airackproject");
    juce::MemoryInputStream oldHeader(oldProject,false);oldHeader.setPosition(4);CHECK(oldHeader.readShort()==25);oldHeader.setPosition(24);CHECK(oldHeader.readInt()==26&&oldHeader.readInt()==3503);
    ProjectDocument project;CHECK(decodeProjectState(oldProject.getData(),oldProject.getSize(),project));CHECK(project.sound==decodedSounds[1]);
    for(std::size_t field=0;field<9;++field)if(const auto index=warmthIndex(1,field))project.sound.values[*index]=field==6?1.f/3:field==0||field==2?2.f/6:.4f;
    juce::MemoryBlock current;CHECK(encodeProjectState(project,current));CHECK(current.getSize()==oldProject.getSize()+growth);
    juce::MemoryInputStream header(current,false);header.setPosition(4);CHECK(header.readShort()==30);header.setPosition(24);CHECK(header.readInt()==31&&header.readInt()==4519);
    ProjectDocument again;CHECK(decodeProjectState(current.getData(),current.getSize(),again)&&again==project);
}
void warmthReach()
{
    std::cout<<"SECTION warmth_keys_in_reach_of_revision_2_wavetable_slots_only\n";
    const auto bytes=fixture("rich-v26.soundstate");SoundDocument rich;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),rich));
    const auto modern=unitReach(NativeModule::wavetable,1),legacy=unitReach(NativeModule::wavetable,1,legacyAlgorithmRevision);
    CHECK(modern.size()==legacy.size()+9&&std::equal(legacy.begin(),legacy.end(),modern.begin()));
    for(std::size_t field=0;field<9;++field)CHECK(modern.size()>=9&&modern[modern.size()-9+field]==warmthIndex(1,field));
    CHECK(unitReach(NativeModule::analog,1)==unitReach(NativeModule::analog,1,legacyAlgorithmRevision));
    // A unit that has been both engines retains a default for every key of its slot.
    const auto registry=parameterRegistry();
    const auto retained=registry.first(drumParameterStart);
    const auto slotKeys=std::count_if(retained.begin(),retained.end(),[](const auto& d){return d.id.startsWith("synth.2.");});
    CHECK(slotKeys==149);
    const auto currentKeys=std::count_if(registry.begin(),registry.end(),[](const auto& d){return d.id.startsWith("synth.2.")&&d.id!="synth.2.drum_mode";});
    CHECK(currentKeys==293&&std::size_t(currentKeys)==rackUnitParameterCapacity(NativeModule::analog)&&std::size_t(currentKeys)==rackUnitParameterCapacity(NativeModule::wavetable));
    auto reach=userReach(rich);
    for(std::size_t field=0;field<9;++field){
        CHECK(reach[*warmthIndex(1,field)]);
        for(std::uint8_t slot:{std::uint8_t(0),std::uint8_t(2),std::uint8_t(3)})CHECK(!reach[*warmthIndex(slot,field)]);
    }
    auto old=rich;old.compatibility.synth[1][1]=legacyAlgorithmRevision;reach=userReach(old);
    for(std::size_t field=0;field<9;++field)CHECK(!reach[*warmthIndex(1,field)]);
    CHECK(reach[*rackParameterIndex(NativeModule::wavetable,1,"wavetable.filter.cutoff")]);
    // The AI may edit drift on the revision-2 slot; on the revision-1 slot the edit is out of reach.
    ProjectDocument source;source.sound=rich;source.sound.synthLocks[1]=false;source.sound.controlLocks.fill(false);
    const auto drift=warmthIndex(1,5);ai::ProposalEdits edits;edits.parameters={{"synth.2.wavetable.drift",.6f}};
    ProjectDocument admitted;CHECK(ai::admitProposal(source,{},edits,admitted)==ai::ProposalStatus::accepted);
    CHECK(drift&&admitted.sound.values[*drift]==.6f);
    source.sound.compatibility.synth[1][1]=legacyAlgorithmRevision;
    CHECK(ai::admitProposal(source,{},edits,admitted)==ai::ProposalStatus::outOfReach);
}
void warmthKeysInAiCatalog()
{
    std::cout<<"SECTION warmth_keys_in_the_ai_catalog\n";
    ProjectDocument source;juce::var provider;CHECK(ai::providerCatalogFor(source,provider));
    const auto* keys=provider["module_keys"]["wavetable"].getArray();const auto reach=unitReach(NativeModule::wavetable,0);
    CHECK(keys&&keys->size()==int(reach.size()));if(!keys||keys->size()!=int(reach.size())||reach.size()<9)return;
    const auto* sets=provider["choice_sets"].getArray();CHECK(sets!=nullptr);if(!sets)return;
    for(std::size_t field=0;field<9;++field){
        const auto& k=warmthKeys[field];const auto& row=(*keys)[keys->size()-9+int(field)];
        CHECK(row[0].toString()==k.key&&row[1].toString()==k.unit&&int(row[2])==int(k.kind));
        CHECK(float(double(row[3]))==k.minimum&&float(double(row[4]))==k.maximum&&float(double(row[5]))==k.initial&&float(double(row[6]))==k.skew);
        const auto set=int(row[7]);CHECK(set>=0&&set<sets->size());if(set<0||set>=sets->size())continue;
        const auto* names=(*sets)[set].getArray();CHECK(names&&std::size_t(names->size())==k.choices.size());
        if(names)for(int c=0;c<names->size()&&std::size_t(c)<k.choices.size();++c)CHECK((*names)[c].toString()==k.choices[std::size_t(c)]);
    }
    if(const auto* analog=provider["module_keys"]["analog"].getArray())
        for(const auto& row:*analog)for(const auto& k:warmthKeys)CHECK(row[0].toString()!=k.key);
}
}

int main(int argc,char** argv)
{
    if(argc==3&&juce::String(argv[1])=="--capture-legacy")return captureLegacy(juce::File(juce::String::fromUTF8(argv[2])))?0:1;
    if(argc!=1)return 2;
    legacyFixturesPinned();warmthKeysRegistered();warmthKeysMapIntoWavetableLayers();warmthStateAndSound26Migration();warmthReach();warmthKeysInAiCatalog();
    std::cout<<"RESULT checks="<<checks<<" failures="<<failures<<'\n';
    return failures==0?0:1;
}
