#include "../../tests/native/FrozenRackExamples.h"
#include "ProjectDocument.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <utility>
#include <stdexcept>

namespace {
using namespace airack;
juce::Array<juce::var> outputs;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
juce::var object(){return juce::var(new juce::DynamicObject);}
void property(juce::var& value,const char* key,const juce::var& data){value.getDynamicObject()->setProperty(key,data);}
juce::String hash(const juce::MemoryBlock& bytes){return juce::SHA256(bytes).toHexString();}
void save(const juce::File& directory,const juce::String& name,const juce::MemoryBlock& bytes){
    const auto file=directory.getChildFile(name);require(!file.exists(),"Refusing fixture overwrite");
    require(file.replaceWithData(bytes.getData(),bytes.getSize()),"Cannot write fixture");
    auto row=object();property(row,"file",name);property(row,"bytes",juce::int64(bytes.getSize()));property(row,"sha256",hash(bytes));outputs.add(row);
    std::cout<<name<<" bytes="<<bytes.getSize()<<" sha256="<<hash(bytes)<<'\n';
}
void json(const juce::File& directory,const juce::String& name,const juce::var& value){
    const auto text=juce::JSON::toString(value,false,17);save(directory,name,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8()));
}
void sound(const juce::File& directory,const char* name,const SoundDocument& value){
    juce::MemoryBlock bytes;SoundDocument decoded;require(encodeSoundState(value,bytes),"Predecessor Sound encode failed");
    juce::MemoryInputStream header(bytes,false);require(header.readInt()==0x49535241&&header.readInt()==19&&header.readInt()==2023,"Freeze requires unchanged Sound19/2023 writer");
    require(decodeSoundState(bytes.getData(),bytes.getSize(),decoded)&&decoded==value,"Predecessor Sound roundtrip differs");save(directory,name,bytes);
}
void project(const juce::File& directory,const char* name,const SoundDocument& value){
    ProjectDocument input;input.sound=value;
    if(juce::String(name).startsWith("rich")){GroupIntent group;group.group=juce::Uuid("4ec4d2b1-6517-4563-9023-f69f9623b401");group.member=juce::Uuid("a3c63713-4294-4af0-bcc9-c6c62775ae47");require(setGroupText("Predecessor capture",group.name)&&setGroupText("Saturation compatibility",group.role)&&setGroupText("F#",group.key)&&setGroupText("minor",group.scale),"Group fixture text invalid");group.bpm=123.75;input.group=group;}
    juce::MemoryBlock bytes;ProjectDocument decoded;
    require(encodeProjectState(input,bytes),"Predecessor Project encode failed");juce::MemoryInputStream header(bytes,false);header.skipNextBytes(4);
    require(header.readShort()==18&&decodeProjectState(bytes.getData(),bytes.getSize(),decoded)&&decoded==input,"Freeze requires unchanged Project18 writer/roundtrip");save(directory,name,bytes);
}
void verifyBindings(const juce::var& bindings){
    const auto sources=bindings["sources"],external=bindings["external_artifacts"];
    require(sources.isArray()&&sources.size()>=14&&external.isArray(),"Bindings require all source files and independent external evidence");
    int approvedWaves=0,raw=0,pipeline=0;std::set<juce::String> seenPaths;
    for(const auto rows:{sources,external})for(int i=0;i<rows.size();++i){
        const auto row=rows[i];const auto path=row["path"].toString();require(juce::File::isAbsolutePath(path),"Binding paths must be absolute");require(seenPaths.insert(path).second,"Duplicate source/evidence binding path");
        juce::MemoryBlock bytes;require(juce::File(path).loadFileAsData(bytes),"Bound source/evidence missing");require(hash(bytes)==row["sha256"].toString(),"Bound source/evidence hash changed");
        const auto kind=row["kind"].toString();approvedWaves+=kind=="approved_wave";raw+=kind=="raw_fingerprint";pipeline+=kind=="pipeline_fingerprint";
    }
    require(approvedWaves==32&&raw>=1&&pipeline>=1,"Supply32 approved WAV hashes plus accepted raw/pipeline evidence before freezing");
}
void carrier(const juce::File& directory,const SoundDocument& input,int block){
    RoutedPatch patch;require(makeRoutedPatch(input,patch),"Predecessor typed carrier invalid");RoutedSynthEngine engine;
    require(engine.prepare(48000,512)&&engine.setPatch(patch),"Predecessor carrier prepare failed");engine.reset();
    constexpr int frames=12288;juce::MemoryBlock bytes;juce::MemoryOutputStream stream(bytes,false);
    stream.writeInt(0x30465241);stream.writeInt(48000);stream.writeInt(frames);stream.writeInt(block);
    juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(256);double peak=0;
    for(int first=0;first<frames;first+=block){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(first==4096)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);engine.render(audio,midi,120.);
        for(int n=0;n<block;++n)for(int ch=0;ch<2;++ch){const auto x=audio.getSample(ch,n);require(std::isfinite(x),"Predecessor carrier nonfinite");peak=std::max(peak,std::abs(double(x)));stream.writeFloat(x);}}
    require(peak>1e-6,"Predecessor carrier must sound");stream.flush();save(directory,"rich-predecessor-48k-"+juce::String(block)+".raw",bytes);
}
}
int main(int argc,char** argv){try{
    require(argc==3,"Usage: RackSynthFreezeSaturationLegacy ABSOLUTE_FRESH_OUTPUT_DIRECTORY ABSOLUTE_BINDINGS_JSON");
    require(parameterCount==2023&&effectBankCount==16&&currentSoundStateVersion==19&&currentProjectStateVersion==18,"Unchanged predecessor handoff required; do not run after append");
    const auto outputPath=juce::String::fromUTF8(argv[1]),bindingsPath=juce::String::fromUTF8(argv[2]);require(juce::File::isAbsolutePath(outputPath)&&juce::File::isAbsolutePath(bindingsPath),"Output and binding paths must be absolute");
    const auto bindingsFile=juce::File(bindingsPath);
    const auto bindings=juce::JSON::parse(bindingsFile);verifyBindings(bindings);
    const juce::File directory(outputPath);require(!directory.exists(),"Output must be fresh; no reused directory");require(directory.createDirectory().wasOk(),"Cannot create fixture directory");
    const auto registry=parameterRegistry();juce::Array<juce::var> catalog;
    for(const auto& d:registry){auto row=object();property(row,"id",d.id);property(row,"name",d.name);property(row,"unit",d.unit);property(row,"kind",int(d.kind));property(row,"minimum",d.minimum);property(row,"maximum",d.maximum);property(row,"default",d.defaultValue);property(row,"skew",d.skew);property(row,"key",juce::int64(d.stateKey));juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);property(row,"choices",choices);catalog.add(row);}json(directory,"registry-predecessor.json",catalog);
    SoundDocument defaults;auto rich=newInstanceSound();rich.personalities.reset();rich.compatibility=newInstanceKernelCompatibility();
    for(std::size_t i=eqParameterStart;i<parameterCount;++i){const auto& d=registry[i];rich.values[i]=d.choices.empty()?float(i%7)/6.f:float(i%d.choices.size())/float(d.choices.size()-1);}
    const std::array<EffectKind,8> kinds{EffectKind::drive,EffectKind::delay,EffectKind::reverb,EffectKind::compressor,EffectKind::eq,EffectKind::filter,EffectKind::midSideSplit,EffectKind::dynamicEq};
    for(std::uint8_t slot=0;slot<8;++slot){RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=slot;edit.effect=kinds[slot];edit.destination=slot%2?masterChain:0;edit.position=rich.layout.effectCount[edit.destination];require(editRackLayout(rich.layout,edit,rich.layout)==RackEditResult::applied,"Predecessor layout invalid");}
    rich.controlLocks.back()=true;rich.controlLocks[eqParameterStart+1]=true;rich.effectLocks[7]=true;rich.synthLocks[0]=true;rich.macros[0]={registry[dynamicEqPlacementParameterStart-1].stateKey,0};rich.macros[1]={registry[dynamicEqGainParameterStart+1].stateKey,.125f};rich.values[2]=.25f;
    auto inactive=rich;inactive.layout=RackLayout{};auto embedded=newInstanceSound();bool placed=false;
    for(const auto& design:airack::test::frozenFactoryRackDesigns())if(design.units.size()==1&&design.units[0].module==NativeModule::dynamicEq){RackOperation op;op.kind=RackOperationKind::place;op.design=design;op.newInstance=juce::Uuid("71abac19-7e09-4a4e-a683-1900615a0081");op.destination={RackRole::fx,masterChain,0};SoundDocument next;RackDifference diff;require(makeRackCandidate(embedded,op,SoundEditOrigin::manual,next,diff)==RackCandidateStatus::ready,"Predecessor terminal module cannot embed");embedded=std::move(next);placed=true;break;}
    require(placed,"Predecessor terminal factory missing");
    for(const auto& pair:std::array<std::pair<const char*,const SoundDocument*>,4>{{{"default",&defaults},{"rich",&rich},{"inactive",&inactive},{"embedded",&embedded}}}){
        const auto stem=juce::String(pair.first)+"-predecessor";sound(directory,(stem+".soundstate").toRawUTF8(),*pair.second);project(directory,(stem+".airackproject").toRawUTF8(),*pair.second);}
    juce::Array<juce::var> factories;const auto capture=[&](const RackDesign& d,const char* kind){juce::MemoryBlock bytes;require(encodeRackDesign(d,bytes),"Predecessor factory encode failed");const auto file=juce::String(kind)+"-"+d.id.toDashedString()+".airack";save(directory,file,bytes);auto row=object();property(row,"id",d.id.toDashedString());property(row,"kind",kind);property(row,"file",file);property(row,"sha256",hash(bytes));factories.add(row);};
    for(const auto& d:airack::test::frozenFactoryRackDesigns())capture(d,"factory");for(const auto& d:airack::test::frozenExampleRackDesigns())capture(d,"example");json(directory,"factories-predecessor.json",factories);
    carrier(directory,rich,64);carrier(directory,rich,512);
    verifyBindings(bindings);
    auto manifest=object();property(manifest,"parameters",2023);property(manifest,"sound_version",19);property(manifest,"project_version",18);property(manifest,"effect_banks",16);property(manifest,"last_effect",15);property(manifest,"last_module",17);property(manifest,"factory_and_example_count",factories.size());property(manifest,"bindings",bindings);property(manifest,"outputs",outputs);
    juce::MemoryBlock executable;require(juce::File::getSpecialLocation(juce::File::currentExecutableFile).loadFileAsData(executable),"Cannot bind actual freeze executable");property(manifest,"tool_sha256",hash(executable));json(directory,"manifest.json",manifest);
    std::cout<<"SATURATION_PREDECESSOR_FREEZE parameters=2023 sound=19 project=18 banks=16 complete\n";return 0;
}catch(const std::exception& e){std::cerr<<"SATURATION_PREDECESSOR_FREEZE rejected: "<<e.what()<<'\n';return 1;}}
