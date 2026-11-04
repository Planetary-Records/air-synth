#include "FrozenRackExamples.h"
#include "ProjectDocument.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace {
using namespace airack;
namespace fs=std::filesystem;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fixture {juce::String name;ProjectDocument project;std::uint8_t slot=0;};
void physical(SoundDocument& sound,const juce::String& id,float value){
    const auto registry=parameterRegistry();const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;});
    require(found!=registry.end(),"Authored physical control absent");
    sound.values[std::size_t(found-registry.begin())]=juce::NormalisableRange<float>(found->minimum,found->maximum,0,found->skew).convertTo0to1(value);
}
std::vector<Fixture> fixtures(){
    const char* names[]{"character-drive","band-color","three-band-all-controls","full-all-controls","bass","pluck","lead","layered"};
    const auto factory=airack::test::frozenFactoryRackDesigns();std::vector<Fixture> result;
    for(std::uint8_t n=0;n<8;++n){Fixture f;f.name=names[n];f.slot=n;f.project.sound=copperArcSound();auto& sound=f.project.sound;
        // Existing authored rounded-bass/articulate-pluck/resonant-lead recipes and phrase roles.
        physical(sound,"global.master_level",1);physical(sound,"synth.1.level",.7f);physical(sound,"output.gain_db",0);physical(sound,"output.protection",0);
        const juce::String bank="synth.1.analog.";const bool bass=n==4,pluck=n==5,lead=n==6;
        physical(sound,bank+"amp.attack",.002f);physical(sound,bank+"amp.decay",pluck?.2f:.15f);physical(sound,bank+"amp.sustain",pluck?0.f:.7f);physical(sound,bank+"amp.release",.15f);
        physical(sound,bank+"filter.cutoff",bass?700.f:pluck?3200.f:lead?6500.f:4200.f);physical(sound,bank+"filter.resonance",lead?.7f:.2f);physical(sound,bank+"filter_env.octaves",pluck?2.f:0.f);
        if(bass){physical(sound,bank+"osc.1.waveform",0);physical(sound,bank+"osc.2.level",.15f);physical(sound,bank+"osc.2.tune",-12);}
        if(n==7){const auto source=sound.personalities->instances.front().id;RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=source;duplicate.newInstance=juce::Uuid("0aa6b771-9a22-4bf4-ae20-914777bc0010");duplicate.destination.position=1;
            SoundDocument layered;RackDifference difference;require(makeRackCandidate(sound,duplicate,SoundEditOrigin::manual,layered,difference)==RackCandidateStatus::ready,"Authored layered rack duplication failed");sound=std::move(layered);physical(sound,"synth.1.pan",-.45f);physical(sound,"synth.2.pan",.65f);physical(sound,"synth.2.level",.35f);}
        const bool three=n==1||n==2||n==7;const auto found=std::find_if(factory.begin(),factory.end(),[&](const auto& d){return d.name==(three?"Band Color":"Character Drive");});require(found!=factory.end(),"Release factory absent");
        RackOperation op;op.kind=RackOperationKind::place;op.design=*found;op.destination={RackRole::fx,masterChain,0};op.newInstance=juce::Uuid("0aa6b771-9a22-4bf4-ae20-914777bc000"+juce::String(n+1));op.allocation={{found->units.front().id,n}};
        SoundDocument placed;RackDifference difference;require(makeRackCandidate(sound,op,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready,"Actual saturation factory placement failed");sound=std::move(placed);
        const auto start=saturationParameterStart+n*35;
        if(n==2||n==3){const auto registry=parameterRegistry();for(std::size_t field=0;field<35;++field){const auto i=start+field;const auto& d=registry[i];sound.values[i]=d.choices.empty()?float(11+(field*7+n*3)%77)/100.f:float((field+n)%d.choices.size())/float(d.choices.size()-1);}
            sound.values[start]=three?1.f:0.f;sound.values[start+5]=1;sound.values[start+6]=0;sound.values[start+7]=sound.values[start+9]=1;
            for(const auto field:{11u,17u,23u,29u})sound.values[start+field]=0;
        }
        sound.values[572+n*23]=0;physical(sound,"effect."+juce::String(n+1)+".saturation.input_trim_db",-3);physical(sound,"effect."+juce::String(n+1)+".saturation.output_trim_db",-6);
        f.project.mode=PlaybackMode::internalPattern;auto& phrase=f.project.pattern;
        if(pluck||bass){phrase.noteCount=4;for(unsigned i=0;i<4;++i)phrase.notes[i]={double(i),pluck?.25:.5,std::uint8_t(bass?36:60+i%2*7),100,1};}
        else if(n==7){phrase.noteCount=4;for(unsigned i=0;i<4;++i)phrase.notes[i]={0,3,std::uint8_t(48+std::array<int,4>{0,3,7,10}[i]),100,1};}
        else{phrase.noteCount=1;phrase.notes[0]={0,2,std::uint8_t(lead?72:48),100,1};}
        require(validateProjectDocument(f.project),"Authored release Project invalid");result.push_back(std::move(f));
    }return result;
}
std::array<float,35> fields(const SaturationSettings& a){
    std::array<float,35> v{float(a.mode),a.inputTrimDb,a.outputTrimDb,a.mix,float(a.driveCompensation),a.lowCrossoverHz,a.highCrossoverHz,
        float(a.preLowCutEnabled),a.preLowCutHz,float(a.postHighCutEnabled),a.postHighCutHz};
    for(std::size_t lane=0;lane<4;++lane){const auto& l=a.lanes[lane];const auto n=11+6*lane;
        v[n]=float(l.bypassed);v[n+1]=float(l.character);v[n+2]=l.driveDb;v[n+3]=l.preTiltDb;v[n+4]=l.mix;v[n+5]=l.trimDb;}
    return v;
}
void validate(const Fixture& f){
    require(validateProjectDocument(f.project),"Complete authored Project invalid");
    juce::MemoryBlock bytes;ProjectDocument recalled;
    require(encodeProjectState(f.project,bytes)&&decodeProjectState(bytes.getData(),bytes.getSize(),recalled)&&recalled==f.project,"Complete Project roundtrip differs");
    RoutedPatch patch;ParameterValues effective;
    require(makeEffectiveParameterValues(f.project.sound,effective)&&makeRoutedPatch(f.project.sound,patch),"Effective patch mapping rejected");
    require(f.project.sound.layout.effects[f.slot].kind==EffectKind::saturation,"Authored saturation absent");
    const auto actual=fields(patch.effects[f.slot].saturation);
    for(std::size_t field=0;field<35;++field)require(actual[field]==parameterPhysicalValue(effective,saturationParameterStart+f.slot*35+field),"Exact effective saturation field mapping differs");
    require(patch.effects[f.slot].bypassed==(f.project.sound.values[572+f.slot*23]==1),"Existing shared bypass mapping differs");
}
fs::path freshOutput(const juce::String& input){
    require(juce::File::isAbsolutePath(input),"Output must be absolute");
    const fs::path output(input.toWideCharPointer()),workspace(juce::String(RACKSYNTH_WORKSPACE).toWideCharPointer());
    const auto absolute=output.lexically_normal(),root=fs::weakly_canonical(workspace);
    const auto relative=absolute.lexically_relative(root);
    require(!relative.empty()&&relative!="."&&!relative.is_absolute()&&*relative.begin()!="..","Output escapes workspace");
    require(!fs::exists(fs::symlink_status(absolute)),"Output must be fresh, including dangling links");
    for(auto parent=absolute.parent_path();!parent.empty();parent=parent.parent_path()){
        require(!fs::is_symlink(fs::symlink_status(parent)),"Redirected output ancestor refused");
        #if JUCE_WINDOWS
        const auto attributes=GetFileAttributesW(parent.c_str());
        require(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)==0,"Reparse output ancestor refused");
        #endif
        if(parent==parent.root_path())break;
    }
    require(fs::weakly_canonical(absolute)==absolute,"Redirected output refused");return absolute;
}
int selfTest(){
    std::size_t checks=0,failures=0;const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<label<<'\n';}};
    auto sound=copperArcSound();const auto factory=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factory.begin(),factory.end(),[](const auto& d){return d.name=="Band Color";});
    check(found!=factory.end(),"actual Band Color factory exists");
    if(found!=factory.end()){RackOperation op;op.kind=RackOperationKind::place;op.design=*found;op.destination={RackRole::fx,masterChain,0};op.newInstance=juce::Uuid("0aa6b771-9a22-4bf4-ae20-914777bc0001");
        SoundDocument placed;RackDifference difference;check(makeRackCandidate(sound,op,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready,"actual factory placement ready");check(validateSoundDocument(placed),"actual placed sound valid");}
    const auto authored=fixtures();check(authored.size()==8,"eight named authored release inputs");std::set<juce::String> names,hashes;
    for(const auto& f:authored){validate(f);++checks;names.insert(f.name);juce::MemoryBlock bytes;encodeProjectState(f.project,bytes);hashes.insert(juce::SHA256(bytes).toHexString());
        auto bypass=f;bypass.project.sound.values[572+f.slot*23]=1;validate(bypass);++checks;
        auto expected=f.project;expected.sound.values[572+f.slot*23]=1;check(bypass.project==expected,"companion changes only explicit shared bypass");}
    check(names==std::set<juce::String>{"character-drive","band-color","three-band-all-controls","full-all-controls","bass","pluck","lead","layered"},"exact required input names");
    check(hashes.size()==8,"distinct complete authored Projects");
    const auto rejects=[&](const juce::String& path){try{freshOutput(path);return false;}catch(const std::exception&){return true;}};
    check(rejects(RACKSYNTH_WORKSPACE),"existing output refused");check(rejects("relative"),"relative output refused");
    check(rejects(juce::File(RACKSYNTH_WORKSPACE).getParentDirectory().getChildFile("outside-saturation-release").getFullPathName()),"outside-workspace output refused");
    std::cout<<"Saturation release fixtures "<<checks<<" checks / "<<failures<<" failures\n";return failures?1:0;
}
int exportInputs(const juce::String& path){
    const auto output=freshOutput(path);const auto authored=fixtures();require(authored.size()==8,"Missing eight release inputs");
    for(const auto& f:authored)validate(f);
    require(fs::create_directories(output),"Fresh output creation failed");juce::Array<juce::var> entries;
    for(const auto& f:authored)for(bool bypass:{false,true}){auto p=f.project;p.sound.values[572+f.slot*23]=bypass?1.f:0.f;
        juce::MemoryBlock bytes;require(encodeProjectState(p,bytes),"Input encoding rejected");const auto name=f.name+(bypass?"-bypass":"")+".airackproject";
        const auto file=juce::File(juce::String(output.c_str())).getChildFile(name);require(file.replaceWithData(bytes.getData(),bytes.getSize()),"Input write failed");
        juce::MemoryBlock read;ProjectDocument decoded;require(file.loadFileAsData(read)&&read==bytes&&decodeProjectState(read.getData(),read.getSize(),decoded)&&decoded==p,"Written complete Project differs");
        auto row=juce::var(new juce::DynamicObject);auto* o=row.getDynamicObject();o->setProperty("name",f.name);o->setProperty("file",name);o->setProperty("slot",int(f.slot));o->setProperty("bypassed",bypass);o->setProperty("bytes",juce::int64(bytes.getSize()));o->setProperty("sha256",juce::SHA256(bytes).toHexString());entries.add(row);}
    auto manifest=juce::var(new juce::DynamicObject);auto* m=manifest.getDynamicObject();m->setProperty("schema","saturation-release-authored-inputs-v1");m->setProperty("inputs",entries);m->setProperty("bpm",120);m->setProperty("expected_audio","EXTERNAL; no generated golden");
    require(juce::File(juce::String(output.c_str())).getChildFile("manifest.json").replaceWithText(juce::JSON::toString(manifest,true)),"Manifest write failed");
    std::cout<<"Exported eight complete Project inputs and eight explicit bypass companions\n";return 0;
}
}
int main(int argc,char** argv){try{if(argc==2&&std::string(argv[1])=="--self-test")return selfTest();if(argc==3&&std::string(argv[1])=="--output")return exportInputs(juce::String::fromUTF8(argv[2]));throw std::runtime_error("Usage: --self-test | --output FRESH_ABSOLUTE_WORKSPACE_DIRECTORY");}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
