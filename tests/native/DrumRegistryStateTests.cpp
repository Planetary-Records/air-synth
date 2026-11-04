#include "FrozenRackExamples.h"
#include "ParameterRegistry.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "UserReach.h"
#include "CompanionWire.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
using namespace airack;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
constexpr std::size_t oldCount=3503,newCount=4519,padStart=3659,modeStart=4235;
juce::File root(){return juce::File(RACKSYNTH_FIXTURE_DIR).getChildFile("drum-state26");}
std::uint32_t fnv(const juce::String& s){std::uint32_t n=2166136261u;for(auto c:s.toStdString())n=(n^static_cast<unsigned char>(c))*16777619u;return n;}
juce::MemoryBlock prefix(){
    juce::MemoryOutputStream s;
    const auto r=parameterRegistry();
    for(std::size_t i=0;i<std::min(oldCount,r.size());++i){const auto& d=r[i];
        s.writeString(d.id);s.writeString(d.name);s.writeString(d.unit);s.writeInt(int(d.kind));
        for(float v:{d.minimum,d.maximum,d.defaultValue,d.skew})s.writeFloat(v);
        s.writeInt(std::bit_cast<std::int32_t>(d.stateKey));s.writeBool(d.automatable);
        s.writeInt(int(d.choices.size()));for(const auto& c:d.choices)s.writeString(c);
    }return s.getMemoryBlock();
}
SoundDocument predecessor(int kind){
    SoundDocument d;d.name=kind==0?"":kind==1?"Named Space":"Retained Source";
    if(kind==1){
        const auto library=airack::test::frozenFactoryRackDesigns();const auto it=std::find_if(library.begin(),library.end(),[](const auto& r){return r.units.size()==1&&r.units[0].module==NativeModule::spaceReverb;});
        CHECK(it!=library.end());if(it==library.end())return d;
        RackOperation op;op.design=*it;op.newInstance=juce::Uuid("12345678-1234-4321-9234-123456789abc");
        op.destination={RackRole::fx,masterChain,0};op.allocation={{it->units[0].id,0}};
        SoundDocument placed;RackDifference diff;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,placed,diff)==RackCandidateStatus::ready);d=placed;d.name="Named Space";
        d.controlLocks[2735]=true;d.effectLocks[0]=true;
    }else if(kind==2){
        RackInstance i;i.id=juce::Uuid("22345678-1234-4321-9234-123456789abc");
        i.design.id=juce::Uuid("32345678-1234-4321-9234-123456789abc");i.design.name="Retained Source";i.design.panel.sections={"SOURCE"};
        RackUnit u;u.id=0;const auto r=parameterRegistry();
        for(std::size_t n=0;n<std::min(oldCount,r.size());++n)if(r[n].id.startsWith("synth.1."))u.defaults.push_back({0,r[n].id.substring(8),d.values[n]});
        CHECK(u.defaults.size()==140);i.design.units={u};i.bindings={{0,0}};
        auto p=std::make_shared<RackPersonalityState>();p->instances={i};d.personalities=p;d.synthLocks[0]=true;
    }CHECK(validateSoundDocument(d));return d;
}
bool load(const juce::String& name,juce::MemoryBlock& b){b.reset();const bool ok=root().getChildFile(name).loadFileAsData(b);CHECK(ok);return ok;}
int capture(){
    if(parameterCount!=oldCount||currentSoundStateVersion!=26||currentProjectStateVersion!=25){std::cerr<<"capture26 requires the unchanged accepted predecessor\n";return 2;}
    CHECK(root().createDirectory().wasOk());
    auto save=[&](const juce::String& n,const juce::MemoryBlock& b){const auto f=root().getChildFile(n);CHECK(!f.exists());if(!f.exists())CHECK(f.replaceWithData(b.getData(),b.getSize()));};
    save("registry3503.bin",prefix());
    for(int kind=0;kind<3;++kind){auto sound=predecessor(kind);juce::MemoryBlock b;CHECK(encodeSoundState(sound,b));save("sound26-"+juce::String(kind)+".arsi",b);
        ProjectDocument project;project.sound=sound;CHECK(encodeProjectState(project,b));save("project25-"+juce::String(kind)+".aipj",b);}
    return failures?1:0;
}
void rehash(juce::MemoryBlock& b){const auto d=juce::SHA256(b.getData(),b.getSize()-32).getRawData();std::memcpy(static_cast<char*>(b.getData())+b.getSize()-32,d.getData(),32);}
void put(juce::MemoryBlock& b,std::size_t at,std::uint32_t value){auto* p=static_cast<unsigned char*>(b.getData());for(unsigned i=0;i<4;++i)p[at+i]=static_cast<unsigned char>(value>>(8*i));}
struct Field{const char* suffix;float low,high,initial,skew;int choices;};
constexpr Field fields[]{
    {"gain_db",-60,12,0,1,0},{"pan",-1,1,0,1,0},{"tune_semitones",-24,24,0,1,0},
    {"trim_start",0,1,0,1,0},{"trim_end",0,1,1,1,0},{"attack_seconds",0,1,0,.3f,0},
    {"decay_seconds",0,10,0,.3f,0},{"midi_note",0,127,36,1,128},{"choke_group",0,16,0,1,17}};
void catalog(){
    const auto r=parameterRegistry();CHECK(r.size()==newCount);CHECK(parameterCount==newCount);
    juce::MemoryBlock frozen;if(load("registry3503.bin",frozen))CHECK(prefix()==frozen);
    std::set<std::uint32_t> keys;std::set<juce::String> ids;
    for(const auto& d:r){CHECK(d.stateKey==fnv(d.id));CHECK(d.stateKey!=0);CHECK(keys.insert(d.stateKey).second);CHECK(ids.insert(d.id).second);}
    for(std::size_t b=0;b<4;++b){const auto& e=r[13+4*b];CHECK(e.kind==ParameterKind::choice);CHECK(e.choices==std::vector<juce::String>({"Analog","Wavetable"}));CHECK(e.minimum==0&&e.maximum==1);
        if(r.size()!=newCount)continue;
        const auto defaults=defaultParameterValues();
        for(std::size_t p=0;p<16;++p)for(std::size_t f=0;f<9;++f){const auto i=padStart+b*144+p*9+f;const auto& d=r[i];const auto& x=fields[f];
            const auto id="synth."+juce::String(int(b+1))+".drum.pad."+juce::String(int(p+1))+"."+x.suffix;
            CHECK(d.id==id);CHECK(d.minimum==x.low&&d.maximum==x.high&&d.skew==x.skew);CHECK(d.defaultValue==(f==7?float(36+p):x.initial));
            CHECK(d.kind==(x.choices?ParameterKind::choice:ParameterKind::continuous));CHECK(d.choices.size()==std::size_t(x.choices));CHECK(d.automatable);
            CHECK(parameterIndexForKey(d.stateKey)==i);CHECK(std::abs(parameterPhysicalValue(defaults,i)-d.defaultValue)<1.e-4f);
            for(int n=0;n<x.choices;++n){auto values=defaults;values[i]=float(n)/float(x.choices-1);CHECK(validateParameterValues(values));CHECK(parameterPhysicalValue(values,i)==float(n));CHECK(std::size_t(n)<d.choices.size()&&d.choices[std::size_t(n)]==(f==8&&n==0?juce::String("Off"):juce::String(n)));}
            if(x.choices){auto bad=defaults;bad[i]=.5f/float(x.choices-1);CHECK(!validateParameterValues(bad));}
        }
        const auto& mode=r[modeStart+b];CHECK(mode.id=="synth."+juce::String(int(b+1))+".drum_mode");CHECK(mode.kind==ParameterKind::toggle&&mode.defaultValue==0&&mode.choices==std::vector<juce::String>({"Off","On"}));
        for(auto m:{NativeModule::analog,NativeModule::wavetable}){const auto reached=unitReach(m,std::uint8_t(b));CHECK(std::none_of(reached.begin(),reached.end(),[](auto i){return i>=padStart;}));CHECK(!rackParameterActive(m,"drum_mode"));CHECK(!rackParameterActive(m,"drum.pad.1.gain_db"));}
    }
}
void formatsAndLocks(){
    CHECK(currentSoundStateVersion==31&&currentProjectStateVersion==30);CHECK(currentSoundNumericBytes==40841&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    CHECK(kernelCompatibilitySectionBytes==168&&currentMinimumProjectStateBytes==41161&&maximumProjectStateBytes==835738);
    for(int kind=0;kind<3;++kind){juce::MemoryBlock b;SoundDocument sound;ProjectDocument project;
        auto expected=predecessor(kind);
        if(kind==1){CHECK(expected.personalities&&expected.personalities->instances.size()==1);if(expected.personalities&&expected.personalities->instances.size()==1){
            auto racks=std::make_shared<RackPersonalityState>(*expected.personalities);auto& rack=racks->instances[0];
            CHECK(rack.design.id==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0")&&rack.design.revision==1&&!rack.package);
            rack.package=RackPackageRef{"lumen-field","1.0.0","5d24c141a237a89030587ba8c9f918461511f8ee04a2e2e1973618f1b5f22f40"};expected.personalities=racks;
        }}
        const auto checkPackage=[&](const SoundDocument& actual){if(kind==0){CHECK(!actual.personalities||actual.personalities->instances.empty());return;}
            CHECK(actual.personalities&&actual.personalities->instances.size()==1);if(!actual.personalities||actual.personalities->instances.size()!=1)return;
            const auto& rack=actual.personalities->instances[0];if(kind==1){CHECK(rack.design.id==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0"));CHECK(rack.package==expected.personalities->instances[0].package);}else CHECK(!rack.package);};
        if(load("sound26-"+juce::String(kind)+".arsi",b)){CHECK(decodeSoundState(b.getData(),b.getSize(),sound));checkPackage(sound);CHECK(sound==expected);
            if(parameterCount==newCount){auto forged=b;put(forged,16,parameterRegistry()[padStart].stateKey);rehash(forged);auto retained=sound;CHECK(!decodeSoundState(forged.getData(),forged.getSize(),retained));CHECK(retained==sound);}}
        if(load("project25-"+juce::String(kind)+".aipj",b)){CHECK(decodeProjectState(b.getData(),b.getSize(),project));checkPackage(project.sound);CHECK(project.sound==expected);}
    }
    if(parameterCount!=newCount)return;
    SoundDocument sound;for(std::size_t b=0;b<4;++b){sound.values[modeStart+b]=1;sound.values[padStart+b*144+3]=.75f;sound.values[padStart+b*144+4]=.25f;sound.controlLocks[padStart+b*144+7]=true;}
    // Scalar storage permits temporary crossed independent trim coordinates; later active mapping owns usable-pair admission.
    CHECK(validateSoundDocument(sound));juce::MemoryBlock blob;CHECK(encodeSoundState(sound,blob));SoundDocument read;CHECK(decodeSoundState(blob.getData(),blob.getSize(),read));CHECK(read==sound);
    ProjectDocument project;project.sound=sound;CHECK(encodeProjectState(project,blob));ProjectDocument decoded;CHECK(decodeProjectState(blob.getData(),blob.getSize(),decoded));CHECK(decoded==project);
    for(std::size_t bank=0;bank<4;++bank){SoundDocument before;before.synthLocks[bank]=true;auto changed=before;changed.values[padStart+bank*144]=.9f;CHECK(!validateAiSoundEdit(before,changed));changed=before;changed.values[modeStart+bank]=1;CHECK(!validateAiSoundEdit(before,changed));
        before.synthLocks[bank]=false;before.effectLocks.fill(true);changed=before;changed.values[padStart+bank*144]=.9f;CHECK(validateAiSoundEdit(before,changed));
        before=SoundDocument{};before.controlLocks[14+4*bank]=true;changed=before;changed.values[modeStart+bank]=1;CHECK(!validateAiSoundEdit(before,changed));}
    auto bad=sound;bad.values[modeStart]=.5f;CHECK(!validateSoundDocument(bad));bad=sound;bad.values[padStart]=std::numeric_limits<float>::quiet_NaN();CHECK(!validateSoundDocument(bad));
    juce::MemoryBlock sentinel("keep",4);CHECK(!encodeSoundState(bad,sentinel));CHECK(sentinel==juce::MemoryBlock("keep",4));
}
void wire(){using namespace airack::companion;CHECK(wireVersion==9);CHECK(maximumGenerationRequestBytes==917504&&maximumControlPayloadBytes==1048576&&maximumResultPayloadBytes==8388608);
    ai::GenerationRequest request;request.tag={juce::Uuid(),1,0,0};request.model="fixture";request.instruction="Keep the sound";
    juce::MemoryBlock payload;CHECK(encodeRequest(request,payload));ai::GenerationRequest decoded;
    CHECK(decodeRequest({static_cast<const std::byte*>(payload.getData()),payload.getSize()},decoded));CHECK(decoded.source==request.source);
    Frame f;f.kind=MessageKind::generate;f.session=juce::Uuid();f.payload=payload;juce::MemoryBlock framed;CHECK(encodeFrame(f,framed));
    if(framed.getSize()>6){auto* p=static_cast<unsigned char*>(framed.getData());p[4]=7;p[5]=0;Frame retained;CHECK(!decodeFrame({static_cast<const std::byte*>(framed.getData()),framed.getSize()},retained));}}
void playablePairs(){
    for(std::uint8_t bank=0;bank<4;++bank)for(bool enabled:{false,true})for(bool macro:{false,true}){
        SoundDocument d;if(bank)CHECK(editRackLayout(d.layout,{RackEditKind::addSynth,bank,masterChain,1},d.layout)==RackEditResult::applied);
        d.values[12+bank*4]=enabled?1.f:0.f;d.values[modeStart+bank]=1;
        const auto start=padStart+bank*144+3,end=start+1;
        if(macro){d.values[start]=.25f;d.values[end]=.75f;d.values[1]=1;d.macros[0]={parameterRegistry()[start].stateKey,1};}
        else{d.values[start]=.75f;d.values[end]=.25f;}
        CHECK(validateSoundDocument(d));juce::MemoryBlock soundBytes,projectBytes;CHECK(encodeSoundState(d,soundBytes));SoundDocument decoded;
        CHECK(decodeSoundState(soundBytes.getData(),soundBytes.getSize(),decoded));CHECK(decoded==d);juce::MemoryBlock exact;CHECK(encodeSoundState(decoded,exact));CHECK(exact==soundBytes);
        ProjectDocument project;project.sound=d;CHECK(encodeProjectState(project,projectBytes));ProjectDocument restored;CHECK(decodeProjectState(projectBytes.getData(),projectBytes.getSize(),restored));CHECK(restored==project);CHECK(encodeProjectState(restored,exact));CHECK(exact==projectBytes);
        RoutedPatch sentinel;sentinel.outputGainDb=7;const auto before=sentinel;CHECK(!makeRoutedPatch(d,sentinel));CHECK(sentinel==before);
        d.values[modeStart+bank]=0;CHECK(validateSoundDocument(d));CHECK(makeRoutedPatch(d,sentinel));CHECK(sentinel.drumPads[bank][0].trimStart>=sentinel.drumPads[bank][0].trimEnd);
    }
}
}
int main(int argc,char** argv){(void)parameterRegistry();if(argc==2&&juce::String(argv[1])=="--capture26")return capture();catalog();formatsAndLocks();wire();playablePairs();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
