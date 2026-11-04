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
#include <bit>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <type_traits>
using namespace airack;
namespace {
int checks=0,failures=0,sections=0;
struct SectionTotals {
    std::string name;int firstCheck=checks,firstFailure=failures;
    explicit SectionTotals(std::string label):name(std::move(label)){++sections;std::cout<<"SECTION "<<name<<'\n';}
    ~SectionTotals(){std::cout<<"SECTION_TOTAL name="<<name<<" checks="<<checks-firstCheck<<" failures="<<failures-firstFailure<<'\n';}
};
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
constexpr std::size_t finalCount=4519,finalDrum=3659,finalMode=4235,drumEnd=4239,privateCount=4083,oldPrefix=3503;
juce::File fixtures(){return juce::File(RACKSYNTH_FIXTURE_DIR);}
juce::File root(){return fixtures().getChildFile("drum-main-reconciliation");}
bool load(const juce::File& file,juce::MemoryBlock& bytes){bytes.reset();const bool ok=file.loadFileAsData(bytes);CHECK(ok);return ok;}
juce::MemoryBlock descriptors(const std::vector<std::size_t>& indices){
    juce::MemoryOutputStream s;const auto r=parameterRegistry();
    for(const auto i:indices){CHECK(i<r.size());if(i>=r.size())continue;const auto& d=r[i];
        s.writeString(d.id);s.writeString(d.name);s.writeString(d.unit);s.writeInt(int(d.kind));
        for(float v:{d.minimum,d.maximum,d.defaultValue,d.skew})s.writeFloat(v);
        s.writeInt(std::bit_cast<std::int32_t>(d.stateKey));s.writeBool(d.automatable);
        s.writeInt(int(d.choices.size()));for(const auto& c:d.choices)s.writeString(c);
    }return s.getMemoryBlock();
}
std::vector<std::size_t> range(std::size_t n){std::vector<std::size_t> out(n);std::iota(out.begin(),out.end(),0);return out;}
template<class F> auto reach(F function,NativeModule module,AlgorithmRevision revision){if constexpr(std::is_invocable_v<F,NativeModule,std::uint8_t,AlgorithmRevision>)return function(module,0,revision);else return function(module,0);}
std::size_t index(const juce::String& id){const auto r=parameterRegistry();const auto it=std::find_if(r.begin(),r.end(),[&](const auto& d){return d.id==id;});CHECK(it!=r.end());return it==r.end()?0:std::size_t(it-r.begin());}
void varied(SoundDocument& d,std::size_t start,std::size_t end){const auto r=parameterRegistry();CHECK(end<=r.size());if(end>r.size())return;
    for(std::size_t i=start;i<end;++i){const auto choices=r[i].choices.size();
        d.values[i]=choices>1?float((i-start+1)%choices)/float(choices-1):float(1+(i-start)%89)/97.f;
        if(d.values[i]==defaultParameterValues()[i])d.values[i]=choices>1?float((i-start+2)%choices)/float(choices-1):.913f;
        d.controlLocks[i]=(i-start)%3==0;
    }
}
void retained(SoundDocument& d,bool privateDrum){
    auto p=std::make_shared<RackPersonalityState>();const auto r=parameterRegistry();
    if(d.layout.synthUse[1]!=SlotUse::present)CHECK(editRackLayout(d.layout,{RackEditKind::addSynth,1,masterChain,1},d.layout)==RackEditResult::applied);
    for(int bank=0;bank<2;++bank){RackInstance instance;instance.id=juce::Uuid(bank==0?"22345678-1234-4321-9234-123456789abc":"22345678-1234-4321-9234-123456789abd");
        instance.design.id=juce::Uuid(bank==0?"32345678-1234-4321-9234-123456789abc":"32345678-1234-4321-9234-123456789abd");
        instance.design.name=bank==0?"Retained endpoint":"All melodic defaults";instance.design.panel.sections={"SOURCE"};
        RackUnit unit;unit.id=0;unit.module=d.values[13+4*std::size_t(bank)]==0?NativeModule::analog:NativeModule::wavetable;unit.algorithmRevision=2;d.compatibility.synth[std::size_t(bank)][std::size_t(unit.module)]=2;
        for(std::size_t i=0;i<r.size();++i)if(r[i].id.startsWith("synth."+juce::String(bank+1)+".")&&i<(privateDrum?oldPrefix:finalDrum))unit.defaults.push_back({0,r[i].id.substring(8),d.values[i]});
        CHECK(unit.defaults.size()==std::size_t(privateDrum?140:149));
        if(privateDrum&&bank==0){const auto key="drum.pad.1.gain_db";unit.defaults[0]={0,key,.731f};
            instance.design.panel.controls={{"Dormant gain",0,0,key,FocusWidget::rotary,FocusResponse::linear,-60,12}};
            d.values[index("synth.1."+juce::String(key))]=.731f;
        }
        instance.design.units={unit};instance.bindings={{0,std::uint8_t(bank)}};
        CHECK(validateRackDesign(instance.design,RackValidationMode::instanceRecall));p->instances.push_back(instance);
    }d.personalities=p;d.synthLocks[0]=true;
}
void namedFx(SoundDocument& d,NativeModule module){const auto library=airack::test::frozenFactoryRackDesigns();const auto it=std::find_if(library.begin(),library.end(),[&](const auto& x){return x.units.size()==1&&x.units[0].module==module;});
    RackDesign design;
    if(it!=library.end())design=*it;
    else if(unsigned(module)==19&&parameterCount>=3659){
        // Main has a native Era module but no factory personality; capture a valid named instance through real placement.
        design.id=juce::Uuid("62345678-1234-4321-9234-123456789abc");design.name="Era native capture";design.role=RackRole::fx;design.panel.layout=PanelLayout::orderedFxStrip;design.panel.sections={"ERA"};
        RackUnit unit;unit.module=module;unit.defaults={{0,"bypassed",0}};const auto r=parameterRegistry();const auto values=defaultParameterValues();
        for(std::size_t i=3539;i<3554;++i)unit.defaults.push_back({0,r[i].id.substring(9),values[i]});design.units={unit};
        design.panel.controls={{"Decay",0,0,"era_reverb.decay_seconds",FocusWidget::rotary,FocusResponse::logarithmic,.2f,70},{"Mix",0,0,"era_reverb.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        CHECK(validateRackDesign(design,RackValidationMode::portableDesign));
    }else {CHECK(it!=library.end());return;}
    RackOperation op;op.design=design;op.newInstance=juce::Uuid("12345678-1234-4321-9234-123456789abc");op.destination={RackRole::fx,masterChain,0};op.allocation={{design.units[0].id,0}};
    auto placed=std::make_unique<SoundDocument>();RackDifference diff;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,*placed,diff)==RackCandidateStatus::ready);d=*placed;d.effectLocks[0]=true;
}
juce::String witness(const ProjectDocument& p){
    juce::String out;const auto r=parameterRegistry();out<<"name\t"<<p.sound.name<<"\n";
    for(std::size_t i=0;i<r.size();++i)out<<juce::String(r[i].stateKey)<<'\t'<<juce::String(std::bit_cast<std::uint32_t>(p.sound.values[i]))<<'\t'<<int(p.sound.controlLocks[i])<<'\t'<<r[i].id<<'\n';
    for(const auto& m:p.sound.macros)out<<"macro\t"<<juce::String(m.targetKey)<<'\t'<<juce::String(std::bit_cast<std::uint32_t>(m.amount))<<'\n';
    for(bool b:p.sound.synthLocks)out<<"synthlock\t"<<int(b)<<'\n';for(bool b:p.sound.effectLocks)out<<"fxlock\t"<<int(b)<<'\n';
    for(const auto& b:p.sound.compatibility.synth)for(auto v:b)out<<"synthrevision\t"<<int(v)<<'\n';
    for(const auto& b:p.sound.compatibility.effects)for(auto v:b)out<<"fxrevision\t"<<int(v)<<'\n';
    if(p.sound.personalities)for(const auto& i:p.sound.personalities->instances){juce::var object;CHECK(encodeRackDesignObject(i.design,object,RackValidationMode::instanceRecall));out<<"instance\t"<<i.id.toString()<<'\t'<<juce::JSON::toString(object,true)<<'\n';for(const auto& b:i.bindings)out<<"binding\t"<<int(b.unit)<<'\t'<<int(b.slot)<<'\n';}
    return out;
}
std::unique_ptr<ProjectDocument> captureDocument(bool privateDrum,int kind){
    auto p=std::make_unique<ProjectDocument>();auto& d=p->sound;
    if(kind==1){
        if(privateDrum){varied(d,oldPrefix,privateCount);for(auto i:{3502,3503,3646,3647,4078,4079,4082})d.controlLocks[std::size_t(i)]=true;
            d.macros[0]={parameterRegistry()[3503].stateKey,.37f};d.macros[7]={parameterRegistry()[4076].stateKey,-.63f};d.synthLocks[2]=true;d.effectLocks[7]=true;
        }else{varied(d,3503,3539);d.values[13]=1;d.compatibility.synth[0][1]=2;d.compatibility.synth[1][1]=1;retained(d,false);
            for(auto i:{3502,3503,3538})d.controlLocks[std::size_t(i)]=true;d.macros[0]={parameterRegistry()[3504].stateKey,.37f};}
    }else if(kind==2){if(privateDrum)retained(d,true);else {namedFx(d,static_cast<NativeModule>(19));varied(d,3539,3659);for(auto i:{3539,3658})d.controlLocks[std::size_t(i)]=true;d.macros[0]={parameterRegistry()[3541].stateKey,.37f};}}
    else if(kind==3){namedFx(d,NativeModule::spaceReverb);d.values[2735+4]=.37f;d.controlLocks[2735]=true;d.macros[0]={parameterRegistry()[2735+4].stateKey,-.42f};if(privateDrum)d.values[3503]=.625f;d.compatibility.synth[2][0]=2;d.compatibility.effects[1][0]=2;}
    if(kind!=0){d.name=(privateDrum?"Private27 ":"Main28 ")+juce::String(kind);p->pattern.bars=2;p->pattern.swing=.21;p->pattern.noteCount=2;p->pattern.notes[0]={0,.25,36,97,1};p->pattern.notes[1]={1.5,.5,51,113,2};p->mode=PlaybackMode::internalPattern;
        GroupIntent group;group.group=juce::Uuid("42345678-1234-4321-9234-123456789abc");group.member=juce::Uuid("52345678-1234-4321-9234-123456789abc");CHECK(setGroupText("Captured group",group.name));CHECK(setGroupText("Percussion",group.role));group.bpm=123.5;p->group=group;}
    CHECK(validateSoundDocument(d));CHECK(validateProjectDocument(*p));return p;
}
int capture(bool privateDrum,const juce::File& dest){
    const bool graph=privateDrum?parameterCount==4083&&currentSoundStateVersion==27&&currentProjectStateVersion==26&&kernelCompatibilitySectionBytes==152:parameterCount==3659&&currentSoundStateVersion==28&&currentProjectStateVersion==27&&kernelCompatibilitySectionBytes==160;
    if(!graph){std::cerr<<"capture refused: wrong production graph\n";return 2;}
    if(!privateDrum){const auto source=juce::File::getCurrentWorkingDirectory();const auto receipt=source.getChildFile("build/drum-machine-native-20261022/main-reconciliation/main-capture-source.json");
        const auto manifest=juce::JSON::parse(receipt.loadFileAsString());const auto files=manifest["files"];
        if(manifest["pinnedMain"].toString()!="f4115c5349d4a4bcfbb19672f8b2c797a81e5e04"||!files.isArray()||files.size()<20){std::cerr<<"capture refused: missing exact main source receipt\n";return 2;}
        for(const auto& row:*files.getArray()){juce::MemoryBlock bytes;if(!source.getChildFile(row["path"].toString()).loadFileAsData(bytes)||juce::SHA256(bytes).toHexString()!=row["sha256"].toString()){std::cerr<<"capture refused: main source mismatch "<<row["path"].toString()<<'\n';return 2;}}
        std::cout<<"REACH analog="<<reach(&unitReach,NativeModule::analog,2).size()<<" wave1="<<reach(&unitReach,NativeModule::wavetable,1).size()<<" wave2="<<reach(&unitReach,NativeModule::wavetable,2).size()<<'\n';
    }
    if(dest.exists()){std::cerr<<"capture refused: destination exists\n";return 2;}
    juce::MemoryBlock frozen;if(!load(fixtures().getChildFile("drum-state26/registry3503.bin"),frozen))return 2;CHECK(descriptors(range(oldPrefix))==frozen);if(failures)return 1;
    CHECK(dest.createDirectory().wasOk());auto save=[&](const juce::String& name,const juce::MemoryBlock& b){const auto file=dest.getChildFile(name);CHECK(!file.exists());if(!file.exists())CHECK(file.replaceWithData(b.getData(),b.getSize()));};
    save(privateDrum?"registry4083.bin":"registry3659.bin",descriptors(range(parameterCount)));
    const juce::StringArray names=privateDrum?juce::StringArray{"private27-default","private27-pads","private27-retained140","private27-space"}:juce::StringArray{"main28-default","main28-warmth","main28-era","main28-space"};
    for(int kind=0;kind<4;++kind){std::cout<<"CAPTURE "<<names[kind]<<'\n';auto p=captureDocument(privateDrum,kind);if(failures)return 1;juce::MemoryBlock b;CHECK(encodeSoundState(p->sound,b));auto sound=std::make_unique<SoundDocument>();CHECK(decodeSoundState(b.getData(),b.getSize(),*sound));CHECK(*sound==p->sound);if(failures)return 1;save(names[kind]+".arsi",b);
        CHECK(encodeProjectState(*p,b));auto decoded=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(b.getData(),b.getSize(),*decoded));CHECK(*decoded==*p);save(names[kind]+".aipj",b);CHECK(dest.getChildFile(names[kind]+".witness.txt").replaceWithText(witness(*p)));}
    std::cout<<checks<<" capture checks, "<<failures<<" failures\n";return failures?1:0;
}
void contract(){SectionTotals section("contract");CHECK(parameterCount==finalCount);CHECK(currentSoundStateVersion==31);CHECK(currentProjectStateVersion==30);CHECK(kernelCompatibilitySectionBytes==168);CHECK(currentSoundNumericBytes==40841);CHECK(currentSoundStateSize==41053);CHECK(maximumSoundStateBytes==830205);CHECK(currentMinimumProjectStateBytes==41161);CHECK(maximumProjectStateBytes==835738);CHECK(companion::wireVersion==9);}
void catalog(){SectionTotals section("catalog");contract();juce::MemoryBlock frozen;if(load(fixtures().getChildFile("drum-state26/registry3503.bin"),frozen))CHECK(descriptors(range(oldPrefix))==frozen);
    if(parameterCount!=finalCount)return;
    if(load(root().getChildFile("main28/registry3659.bin"),frozen))CHECK(descriptors(range(finalDrum))==frozen);
    auto projected=range(oldPrefix);for(std::size_t i=finalDrum;i<drumEnd;++i)projected.push_back(i);if(load(root().getChildFile("private27/registry4083.bin"),frozen))CHECK(descriptors(projected)==frozen);
    std::set<juce::String> ids;std::set<std::uint32_t> keys;const auto r=parameterRegistry();
    for(const auto& d:r){std::uint32_t key=2166136261u;for(auto c:d.id.toStdString())key=(key^std::uint8_t(c))*16777619u;CHECK(key==d.stateKey&&key!=0);CHECK(ids.insert(d.id).second);CHECK(keys.insert(d.stateKey).second);}
    for(std::size_t bank=0;bank<4;++bank){
        for(const auto target:{finalDrum+bank*144,finalDrum+bank*144+143,finalMode+bank}){auto before=std::make_unique<SoundDocument>();before->synthLocks[bank]=true;auto changed=std::make_unique<SoundDocument>(*before);changed->values[target]=target>=finalMode?1:.5f;CHECK(!validateAiSoundEdit(*before,*changed));before->synthLocks[bank]=false;before->effectLocks.fill(true);*changed=*before;changed->values[target]=target>=finalMode?1:.5f;CHECK(validateAiSoundEdit(*before,*changed));}
        const auto note=finalDrum+bank*144+7;CHECK(r[note].choices.size()==128);if(r[note].choices.size()==128)for(std::size_t n=0;n<128;++n)CHECK(r[note].choices[n]==juce::String(int(n)));
        CHECK(r[finalDrum+bank*144+8].choices.size()==17);CHECK(r[13+4*bank].choices==std::vector<juce::String>({"Analog","Wavetable"}));
        for(const auto module:{NativeModule::analog,NativeModule::wavetable}){const auto active=reach(&unitReach,module,2);CHECK(std::none_of(active.begin(),active.end(),[](auto i){return i>=finalDrum;}));CHECK(!rackParameterActive(module,"drum_mode")&&!rackParameterActive(module,"drum.pad.1.gain_db"));}
    }
    CHECK(reach(&unitReach,NativeModule::analog,2).size()==116);CHECK(reach(&unitReach,NativeModule::wavetable,1).size()==116);CHECK(reach(&unitReach,NativeModule::wavetable,2).size()==125);CHECK(!supportsSynthAlgorithmRevision(3));CHECK(rackUnitParameterCapacity(NativeModule::drum)==293);CHECK(rackUnitParameterCapacity(static_cast<NativeModule>(22))==0);
}
std::uint32_t get(const juce::MemoryBlock& b,std::size_t at){CHECK(at+4<=b.getSize());if(at+4>b.getSize())return 0;const auto* p=static_cast<const unsigned char*>(b.getData())+at;return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
void put(juce::MemoryBlock& b,std::size_t at,std::uint32_t value){CHECK(at+4<=b.getSize());if(at+4>b.getSize())return;auto* p=static_cast<unsigned char*>(b.getData())+at;for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(value>>(8*i));}
void sign(juce::MemoryBlock& b){CHECK(b.getSize()>=32);if(b.getSize()<32)return;const auto hash=juce::SHA256(b.getData(),b.getSize()-32).getRawData();std::memcpy(static_cast<char*>(b.getData())+b.getSize()-32,hash.getData(),32);}
void rejectSound(juce::MemoryBlock b){sign(b);auto before=std::make_unique<SoundDocument>();before->name="Unchanged sentinel";auto personalities=std::make_shared<RackPersonalityState>();before->personalities=personalities;auto after=std::make_unique<SoundDocument>(*before);CHECK(!decodeSoundState(b.getData(),b.getSize(),*after));CHECK(*after==*before&&after->personalities==personalities);
    auto project=std::make_unique<ProjectDocument>();project->sound=*before;auto retained=std::make_unique<ProjectDocument>(*project);CHECK(!decodeProjectState(b.getData(),b.getSize(),*retained));CHECK(*retained==*project&&retained->sound.personalities==personalities);}
void rejectProject(juce::MemoryBlock b){if(b.getSize()>=32)sign(b);auto before=std::make_unique<ProjectDocument>();before->sound.name="Unchanged project";before->sound.personalities=std::make_shared<RackPersonalityState>();auto after=std::make_unique<ProjectDocument>(*before);CHECK(!decodeProjectState(b.getData(),b.getSize(),*after));CHECK(*after==*before&&after->sound.personalities==before->sound.personalities);}
void layoutBytes(juce::MemoryOutputStream& s,const RackLayout& l){for(auto x:l.synthUse)s.writeByte(char(x));for(auto x:l.synthOrder)s.writeByte(char(x));s.writeByte(char(l.synthCount));for(const auto& e:l.effects){s.writeByte(char(e.use));s.writeByte(char(e.kind));s.writeByte(char(e.chain));}for(const auto& order:l.effectOrder)for(auto x:order)s.writeByte(char(x));for(auto x:l.effectCount)s.writeByte(char(x));}
std::size_t numeric(const juce::MemoryBlock& b){return 16+std::size_t(get(b,8))*9+78+64+12;}
void migrated(const juce::MemoryBlock& b,SoundDocument& d){
    const auto count=std::size_t(get(b,8));const bool priv=get(b,4)==27&&count==4083;CHECK(decodeSoundState(b.getData(),b.getSize(),d));
    juce::MemoryInputStream in(b,false);in.setPosition(16);
    for(std::size_t saved=0;saved<count;++saved){const auto key=std::uint32_t(in.readInt());const auto value=in.readFloat();const auto target=parameterIndexForKey(key);CHECK(target.has_value());if(target)CHECK(std::bit_cast<std::uint32_t>(d.values[*target])==std::bit_cast<std::uint32_t>(value));}
    juce::MemoryOutputStream layout;layoutBytes(layout,d.layout);CHECK(layout.getDataSize()==78);CHECK(std::memcmp(layout.getData(),static_cast<const char*>(b.getData())+16+count*8,78)==0);
    in.setPosition(16+count*8+78);for(const auto& m:d.macros){CHECK(m.targetKey==std::uint32_t(in.readInt()));CHECK(std::bit_cast<std::uint32_t>(m.amount)==std::bit_cast<std::uint32_t>(in.readFloat()));}
    for(std::size_t saved=0;saved<count;++saved){const auto target=priv&&saved>=3503?saved+156:saved;const auto locked=in.readByte();CHECK(locked==0||locked==1);if(target<parameterCount)CHECK(d.controlLocks[target]==bool(locked));}
    for(bool locked:d.synthLocks)CHECK(locked==bool(in.readByte()));for(bool locked:d.effectLocks)CHECK(locked==bool(in.readByte()));
    CHECK(in.getPosition()==juce::int64(numeric(b)));const auto compat=in.readInt();CHECK(compat==152||compat==160);CHECK(in.readShort()==1&&in.readByte()==4&&in.readByte()==2&&in.readByte()==8);const auto banks=std::uint8_t(in.readByte());CHECK(banks==17||banks==18);CHECK(in.readShort()==0);
    for(const auto& bank:d.compatibility.synth)for(auto revision:bank)CHECK(revision==std::uint8_t(in.readByte()));
    for(const auto& bank:d.compatibility.effects){for(std::size_t i=0;i<banks;++i)CHECK(bank[i]==std::uint8_t(in.readByte()));for(std::size_t i=banks;i<bank.size();++i)CHECK(bank[i]==1);}
    const auto section=in.readInt();if(section){CHECK(in.readShort()==1);const auto instances=std::uint8_t(in.readByte());CHECK(in.readByte()==0);CHECK(d.personalities&&d.personalities->instances.size()==instances);
        if(d.personalities&&d.personalities->instances.size()==instances)for(const auto& instance:d.personalities->instances){std::uint8_t uuid[16];in.read(uuid,16);CHECK(instance.id==juce::Uuid(uuid));CHECK(instance.edited==bool(in.readByte()));CHECK(instance.bindings.size()==std::uint8_t(in.readByte()));for(const auto& binding:instance.bindings){CHECK(binding.unit==std::uint8_t(in.readByte()));CHECK(binding.slot==std::uint8_t(in.readByte()));}const auto length=in.readInt();RackDesign design;CHECK(length>0&&length<=65536);if(length>0&&length<=65536){CHECK(decodeRackDesign(static_cast<const char*>(b.getData())+in.getPosition(),std::size_t(length),design,RackValidationMode::instanceRecall));CHECK(design==instance.design);in.skipNextBytes(length);}}}
    const auto name=in.readInt();CHECK(name>=0&&name<=160);if(name>=0&&name<=160)CHECK(d.name==juce::String::fromUTF8(static_cast<const char*>(b.getData())+in.getPosition(),name));
    if(priv&&parameterCount==finalCount)for(std::size_t i=3503;i<3659;++i){CHECK(d.values[i]==defaultParameterValues()[i]);CHECK(!d.controlLocks[i]);}
    if(!priv)for(std::size_t i=count;i<parameterCount;++i){CHECK(d.values[i]==defaultParameterValues()[i]);CHECK(!d.controlLocks[i]);}
}
juce::MemoryBlock nested(const juce::MemoryBlock& original,const SoundDocument& document,const std::function<void(juce::var&)>& edit={}){
    juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(char(document.personalities->instances.size()));section.writeByte(0);
    for(const auto& instance:document.personalities->instances){juce::MemoryBlock design;CHECK(encodeRackDesign(instance.design,design,RackValidationMode::instanceRecall));if(edit){juce::var object;CHECK(encodeRackDesignObject(instance.design,object,RackValidationMode::instanceRecall));edit(object);const auto json=juce::JSON::toString(object,true);design=juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8()));}section.write(instance.id.getRawData(),16);section.writeByte(instance.edited?1:0);section.writeByte(char(instance.bindings.size()));for(const auto& binding:instance.bindings){section.writeByte(char(binding.unit));section.writeByte(char(binding.slot));}section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());}
    const auto offset=numeric(original)+4+get(original,numeric(original));const auto oldLength=get(original,offset);CHECK(offset+4+oldLength+32<=original.getSize());
    juce::MemoryOutputStream out;out.write(original.getData(),offset);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(static_cast<const char*>(original.getData())+offset+4+oldLength,original.getSize()-offset-4-oldLength);
    auto result=out.getMemoryBlock();
    // Only the synthetic source-owner witness becomes melodic. Locate the private
    // numeric record by its stable key; original fixtures and other records stay exact.
    const auto count=std::size_t(get(original,8));std::size_t flagOffset=0,flagMatches=0;
    for(std::size_t n=0;n<count;++n)if(get(original,16+n*8)==parameterRegistry()[finalMode].stateKey){flagOffset=20+n*8;++flagMatches;}
    CHECK(flagMatches<=1);
    if(flagMatches==1&&get(original,flagOffset)!=std::bit_cast<std::uint32_t>(document.values[finalMode])){
        CHECK(get(original,4)==27&&count==privateCount&&document.values[finalMode]==0&&get(original,flagOffset)==std::bit_cast<std::uint32_t>(1.f));
        put(result,flagOffset,std::bit_cast<std::uint32_t>(document.values[finalMode]));
        for(std::size_t n=0;n<count;++n){CHECK(get(result,16+n*8)==get(original,16+n*8));if(20+n*8!=flagOffset)CHECK(get(result,20+n*8)==get(original,20+n*8));}
    }
    juce::MemoryOutputStream layout;layoutBytes(layout,document.layout);std::memcpy(static_cast<char*>(result.getData())+16+std::size_t(get(original,8))*8,layout.getData(),78);sign(result);return result;
}
std::unique_ptr<SoundDocument> nestedWitness(const SoundDocument& source,std::size_t key,bool panel){
    auto d=std::make_unique<SoundDocument>(source);const auto& descriptor=parameterRegistry()[key];const bool synth=descriptor.id.startsWith("synth.");RackInstance instance;instance.id=juce::Uuid("72345678-1234-4321-9234-123456789abc");instance.design.id=juce::Uuid("82345678-1234-4321-9234-123456789abc");instance.design.name="Native domain witness";instance.design.role=synth?RackRole::synth:RackRole::fx;instance.design.panel.sections={"WITNESS"};
    if(synth)d->values[finalMode]=0; // Local old-family melodic template, after unmodified fixture migration.
    RackUnit unit;unit.module=synth?(d->values[13]==0?NativeModule::analog:NativeModule::wavetable):NativeModule::spaceReverb;unit.algorithmRevision=synth?d->compatibility.synth[0][std::size_t(unit.module)]:1;
    if(!synth){d->layout.effects[0]={SlotUse::present,EffectKind::spaceReverb,masterChain};for(auto& order:d->layout.effectOrder)order.fill(noSlot);d->layout.effectCount.fill(0);d->layout.effectOrder[masterChain][0]=0;d->layout.effectCount[masterChain]=1;for(std::size_t i=1;i<8;++i)d->layout.effects[i].use=SlotUse::neverUsed;}
    const auto local=descriptor.id.endsWith(".drum_mode")?juce::String("enabled"):descriptor.id.substring(synth?8:9);if(panel)instance.design.panel.controls={{"Endpoint",0,0,local,descriptor.kind==ParameterKind::continuous?FocusWidget::rotary:descriptor.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice,FocusResponse::linear,descriptor.minimum,descriptor.maximum}};else unit.defaults={{0,local,defaultParameterValues()[key]}};
    instance.design.units={unit};instance.bindings={{0,0}};auto state=std::make_shared<RackPersonalityState>();state->instances={instance};d->personalities=state;CHECK(validateSoundDocument(*d));return d;
}
void historicalFamily(const juce::File& file,const std::vector<std::size_t>& validMacros,const std::vector<std::size_t>& foreign,const std::vector<std::size_t>& macroForeign,bool ordinalPrefix=true){
    SectionTotals section("historical "+file.getFileName().toStdString());juce::MemoryBlock b;if(!load(file,b))return;auto d=std::make_unique<SoundDocument>();const bool decoded=decodeSoundState(b.getData(),b.getSize(),*d);CHECK(decoded);if(!decoded)return;if(ordinalPrefix)migrated(b,*d);
    auto project=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(b.getData(),b.getSize(),*project));CHECK(project->sound==*d);
    const auto count=std::size_t(get(b,8));
    for(const auto key:validMacros){CHECK(isContinuousParameter(key));auto accepted=b;put(accepted,16+count*8+78,parameterRegistry()[key].stateKey);put(accepted,16+count*8+82,std::bit_cast<std::uint32_t>(.2f));sign(accepted);auto read=std::make_unique<SoundDocument>();CHECK(decodeSoundState(accepted.getData(),accepted.getSize(),*read));CHECK(read->macros[0].targetKey==parameterRegistry()[key].stateKey&&read->macros[0].amount==.2f);}
    for(bool panel:{false,true})for(const auto key:{39u,2735u}){auto witness=nestedWitness(*d,key,panel);auto accepted=nested(b,*witness);auto read=std::make_unique<SoundDocument>();CHECK(decodeSoundState(accepted.getData(),accepted.getSize(),*read));CHECK(read->personalities&&*read->personalities==*witness->personalities);}
    auto source=nestedWitness(*d,39,false);rejectSound(nested(b,*source,[](juce::var& object){object["units"][0].getDynamicObject()->setProperty("algorithm_revision",3);}));
    if(get(b,4)==27){auto fx=nestedWitness(*d,2735,false);auto state=std::make_shared<RackPersonalityState>(*fx->personalities);state->instances[0].design.units[0].defaults.clear();fx->personalities=state;CHECK(validateSoundDocument(*fx));auto accepted=nested(b,*fx);auto read=std::make_unique<SoundDocument>();CHECK(decodeSoundState(accepted.getData(),accepted.getSize(),*read));rejectSound(nested(b,*fx,[](juce::var& object){object["units"][0].getDynamicObject()->setProperty("module","era_reverb");}));}
    for(const auto key:foreign){if(key>=parameterCount){CHECK(key<parameterCount);continue;}auto forged=b;put(forged,16,parameterRegistry()[key].stateKey);put(forged,20,std::bit_cast<std::uint32_t>(defaultParameterValues()[key]));rejectSound(forged);
        for(bool panel:{false,true}){auto witness=nestedWitness(*d,key,panel);
            if(parameterRegistry()[key].id.endsWith(".drum_mode"))rejectSound(nested(b,*witness,[&](juce::var& object){
                auto endpoint=panel?object["panel"]["controls"][0]:object["units"][0]["defaults"][0];
                CHECK(endpoint["key"].toString()=="enabled");endpoint.getDynamicObject()->setProperty("key","drum_mode");
                if(panel){CHECK(endpoint["widget"].toString()=="toggle");CHECK(double(endpoint["minimum"])==0&&double(endpoint["maximum"])==1);}
            }));else rejectSound(nested(b,*witness));}}
    for(const auto key:macroForeign){if(key>=parameterCount){CHECK(key<parameterCount);continue;}CHECK(isContinuousParameter(key));auto forged=b;put(forged,16+count*8+78,parameterRegistry()[key].stateKey);rejectSound(forged);}
    for(auto unknown:{std::uint32_t(0),std::uint32_t(0xffffffff)}){auto forged=b;put(forged,16,unknown);rejectSound(forged);forged=b;put(forged,16+count*8+78,unknown);if(unknown==0)put(forged,16+count*8+82,std::bit_cast<std::uint32_t>(.5f));rejectSound(forged);}
    auto duplicate=b;put(duplicate,16,get(b,24));rejectSound(duplicate);
    auto reorder=b;auto* bytes=static_cast<char*>(reorder.getData());for(std::size_t i=0;i<count/2;++i)for(int j=0;j<8;++j)std::swap(bytes[16+i*8+std::size_t(j)],bytes[16+(count-1-i)*8+std::size_t(j)]);sign(reorder);auto moved=std::make_unique<SoundDocument>();CHECK(decodeSoundState(reorder.getData(),reorder.getSize(),*moved));CHECK(*moved==*d);
    auto badLock=b;static_cast<char*>(badLock.getData())[16+count*8+78+64+count-1]=2;rejectSound(badLock);
}
void rejectHeaderSwaps(const juce::MemoryBlock& b){
    for(const auto [version,count]:std::initializer_list<std::pair<int,int>>{{27,3503},{27,3539},{27,3659},{27,4083},{28,3659},{28,4083},{28,4239},{29,3659},{29,4083},{29,4239}}){
        if(version==int(get(b,4))&&count==int(get(b,8)))continue;
        auto forged=b;put(forged,4,std::uint32_t(version));put(forged,8,std::uint32_t(count));rejectSound(forged);
    }
}
void shapeAndNesting(){
    SectionTotals section("exact_headers_compatibility_and_project_envelopes");
    for(const auto& path:juce::StringArray{"drum-main-reconciliation/private27/private27-pads.arsi","drum-main-reconciliation/main28/main28-era.arsi","era-reverb-sound27/warm-v27.soundstate"}){juce::MemoryBlock b;if(!load(fixtures().getChildFile(path),b))continue;
        rejectHeaderSwaps(b);
        auto reserved=b;put(reserved,12,1);rejectSound(reserved);
        const auto at=numeric(b);const auto oldBytes=std::size_t(get(b,at));const auto newBytes=oldBytes==152?160u:152u;juce::MemoryOutputStream compatibility;compatibility.writeInt(int(newBytes));compatibility.writeShort(1);compatibility.writeByte(4);compatibility.writeByte(2);compatibility.writeByte(8);compatibility.writeByte(newBytes==160?18:17);compatibility.writeShort(0);const auto* bytes=static_cast<const char*>(b.getData());compatibility.write(bytes+at+12,8);
        for(std::size_t bank=0;bank<8;++bank){compatibility.write(bytes+at+20+bank*((oldBytes-16)/8),17);if(newBytes==160)compatibility.writeByte(1);}
        juce::MemoryOutputStream changed;changed.write(b.getData(),at);changed.write(compatibility.getData(),compatibility.getDataSize());changed.write(bytes+at+4+oldBytes,b.getSize()-at-4-oldBytes);rejectSound(changed.getMemoryBlock());
    }
    for(const auto& path:juce::StringArray{"private27/private27-default","private27/private27-retained140","main28/main28-default","main28/main28-warmth"}){juce::MemoryBlock b;if(!load(root().getChildFile(path+".aipj"),b))continue;for(const auto v:{25,26,27,28})if(v!=int(get(b,4))){auto forged=b;put(forged,4,std::uint32_t(v));rejectProject(forged);}
        for(const auto at:{12u,16u})for(const auto value:{0u,15u,0x80000000u,0xffffffffu}){auto forged=b;put(forged,at,value);rejectProject(forged);}
        for(const auto at:{24u,28u,32u}){auto forged=b;put(forged,at,get(b,at)+1);const auto soundBytes=std::size_t(get(forged,12));juce::MemoryBlock sound(static_cast<const char*>(forged.getData())+20,soundBytes);sign(sound);std::memcpy(static_cast<char*>(forged.getData())+20,sound.getData(),soundBytes);rejectProject(forged);}
        auto extra=b;extra.insert("x",1,extra.getSize()-32);rejectProject(extra);auto shortBlob=b;shortBlob.setSize(30);rejectProject(shortBlob);
    }
    if(parameterCount!=finalCount)return;
    auto current=std::make_unique<SoundDocument>();varied(*current,finalDrum,finalCount);current->values[finalDrum+3]=.8f;current->values[finalDrum+4]=.2f;CHECK(validateSoundDocument(*current));juce::MemoryBlock modern;CHECK(encodeSoundState(*current,modern));auto recalled=std::make_unique<SoundDocument>();CHECK(decodeSoundState(modern.getData(),modern.getSize(),*recalled));CHECK(*recalled==*current);rejectHeaderSwaps(modern);
    auto invalid=modern;put(invalid,16,0xffffffff);rejectSound(invalid);invalid=modern;put(invalid,16,get(modern,24));rejectSound(invalid);invalid=modern;put(invalid,20,0x7fc00000);rejectSound(invalid);
    auto currentProject=std::make_unique<ProjectDocument>();currentProject->sound=*current;juce::MemoryBlock projectBytes;CHECK(encodeProjectState(*currentProject,projectBytes));for(const auto version:{25,26,27}){auto forged=projectBytes;put(forged,4,std::uint32_t(version));rejectProject(forged);}
    for(const auto key:{3504u,3538u,3541u,3658u,3659u,4232u}){auto accepted=modern;put(accepted,16+finalCount*8+78,parameterRegistry()[key].stateKey);put(accepted,16+finalCount*8+82,std::bit_cast<std::uint32_t>(.2f));sign(accepted);CHECK(decodeSoundState(accepted.getData(),accepted.getSize(),*recalled));CHECK(recalled->macros[0].targetKey==parameterRegistry()[key].stateKey);}
    companion::Frame frame;frame.kind=companion::MessageKind::generate;frame.session=juce::Uuid("92345678-1234-4321-9234-123456789abc");ai::GenerationRequest request;request.tag={juce::Uuid(),1,0,0};request.model="fixture";request.instruction="Keep the sound";CHECK(companion::encodeRequest(request,frame.payload));juce::MemoryBlock framed;CHECK(companion::encodeFrame(frame,framed));companion::Frame decoded;CHECK(companion::decodeFrame({static_cast<const std::byte*>(framed.getData()),framed.getSize()},decoded));CHECK(decoded.session==frame.session&&decoded.payload==frame.payload);for(const auto version:{7u,8u}){auto bad=framed;static_cast<unsigned char*>(bad.getData())[4]=static_cast<unsigned char>(version);const auto before=decoded;CHECK(!companion::decodeFrame({static_cast<const std::byte*>(bad.getData()),bad.getSize()},decoded));CHECK(decoded.session==before.session&&decoded.payload==before.payload);}
    juce::MemoryBlock old;if(load(root().getChildFile("private27/private27-retained140.arsi"),old)){auto d=std::make_unique<SoundDocument>();CHECK(decodeSoundState(old.getData(),old.getSize(),*d));if(d->personalities&&d->personalities->instances.size()==2){auto state=std::make_shared<RackPersonalityState>(*d->personalities);auto& unit=state->instances[0].design.units[0];unit.defaults=state->instances[1].design.units[0].defaults;CHECK(unit.defaults.size()==140);unit.defaults.push_back({0,"drum.pad.1.gain_db",.731f});CHECK(unit.defaults.size()==141);d->personalities=state;CHECK(validateSoundDocument(*d));rejectSound(nested(old,*d));}}
    for(const auto& path:juce::StringArray{"era-reverb-sound27/warm-v27.soundstate","drum-main-reconciliation/main28/main28-warmth.arsi"}){juce::MemoryBlock b;if(!load(fixtures().getChildFile(path),b))continue;auto d=std::make_unique<SoundDocument>();CHECK(decodeSoundState(b.getData(),b.getSize(),*d));auto retained=nestedWitness(*d,3504,false);auto state=std::make_shared<RackPersonalityState>(*retained->personalities);auto& unit=state->instances[0].design.units[0];unit.defaults.clear();const auto r=parameterRegistry();for(std::size_t i=0;i<3539;++i)if(r[i].id.startsWith("synth.1."))unit.defaults.push_back({0,r[i].id.substring(8),retained->values[i]});CHECK(unit.defaults.size()==149);retained->personalities=state;CHECK(validateSoundDocument(*retained));auto accepted=nested(b,*retained);auto read=std::make_unique<SoundDocument>();CHECK(decodeSoundState(accepted.getData(),accepted.getSize(),*read));CHECK(read->personalities&&read->personalities->instances[0].design.units[0].defaults.size()==149);rejectSound(nested(b,*retained,[](juce::var& object){auto* defaults=object["units"][0]["defaults"].getArray();CHECK(defaults&&defaults->size()==149);if(!defaults)return;auto extra=juce::var(new juce::DynamicObject());extra.getDynamicObject()->setProperty("unit",0);extra.getDynamicObject()->setProperty("key","drum_mode");extra.getDynamicObject()->setProperty("normalized",0.);defaults->add(extra);CHECK(defaults->size()==150);}));}
}
void domains(){SectionTotals section("domains");contract();
    historicalFamily(fixtures().getChildFile("drum-state26/sound26-0.arsi"),{2735,3501},{3503,3539,3659,4238},{3504,3541,3659,4232});
    historicalFamily(fixtures().getChildFile("era-reverb-sound27/warm-v27.soundstate"),{3504,3538},{3539,3658,3659,4238},{3541,3658,3659,4232});
    historicalFamily(root().getChildFile("private27/private27-pads.arsi"),{3659,4232},{3503,3538,3539,3658},{3504,3538,3541,3658});
    historicalFamily(root().getChildFile("main28/main28-era.arsi"),{3504,3538,3541,3658},{3659,4238},{3659,4232});
    historicalFamily(fixtures().getChildFile("space-reverb-pr47-sound23/rich-v23.soundstate"),{2735,3501},{3503,3539,3659,4238},{3504,3541,3659,4232},false);
    for(const auto& family:juce::StringArray{"private27","main28"}){const auto names=family=="private27"?juce::StringArray{"private27-default","private27-pads","private27-retained140","private27-space"}:juce::StringArray{"main28-default","main28-warmth","main28-era","main28-space"};
        for(const auto& name:names){juce::MemoryBlock b;if(load(root().getChildFile(family+"/"+name+".arsi"),b)){auto sound=std::make_unique<SoundDocument>();if(decodeSoundState(b.getData(),b.getSize(),*sound))migrated(b,*sound);else CHECK(false);}
            if(load(root().getChildFile(family+"/"+name+".aipj"),b)){auto project=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(b.getData(),b.getSize(),*project));if(!name.endsWith("default")){CHECK(project->pattern.bars==2&&project->pattern.noteCount==2&&project->pattern.swing==.21);CHECK(project->pattern.notes[0].pitch==36&&project->pattern.notes[1].pitch==51);CHECK(project->group&&groupTextView(project->group->name)=="Captured group"&&project->group->bpm==123.5);}
                juce::MemoryBlock encoded;if(encodeProjectState(*project,encoded)){auto again=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(encoded.getData(),encoded.getSize(),*again));CHECK(*again==*project);}else CHECK(false);}}
    }shapeAndNesting();
}
}
int main(int argc,char** argv){std::cout<<std::unitbuf;std::cerr<<std::unitbuf;(void)parameterRegistry();
    if(argc==3&&juce::String(argv[1])=="--capture-private27")return capture(true,juce::File(argv[2]));
    if(argc==3&&juce::String(argv[1])=="--capture-main28")return capture(false,juce::File(argv[2]));
    const juce::String option=argc==2?argv[1]:"";
    if(argc>2||(option.isNotEmpty()&&option!="--contract"&&option!="--catalog"&&option!="--domains"))return 2;
    if(option=="--contract")contract();else if(option=="--catalog")catalog();else if(option=="--domains")domains();else {catalog();domains();}
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
}
