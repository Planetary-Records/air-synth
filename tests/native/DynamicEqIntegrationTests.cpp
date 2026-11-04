#include "LegacyPersonalityProjection.h"
#include "FrozenRackExamples.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include "RackExamples.h"
#include "FxMonitor.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace airack;
int checks=0, failures=0;
void check(bool good,const char* expression,int line)
{ ++checks; if(!good){++failures;if(failures<85)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';} }
#define CHECK(x) check(bool(x),#x,__LINE__)
std::optional<std::size_t> index(const juce::String& id)
{
    const auto registry=parameterRegistry();
    for(std::size_t i=0;i<registry.size();++i)if(registry[i].id==id)return i;
    return {};
}
juce::MemoryBlock fixture(const char* name)
{
    juce::MemoryBlock bytes;
    CHECK(juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getChildFile(name).loadFileAsData(bytes));return bytes;
}
int readInt(const juce::MemoryBlock& bytes,int position)
{ juce::MemoryInputStream stream(bytes,false);stream.setPosition(position);return stream.readInt(); }
void sign(juce::MemoryBlock& bytes)
{
    const auto digest=juce::SHA256(bytes.getData(),bytes.getSize()-32).getRawData();
    std::memcpy(static_cast<char*>(bytes.getData())+bytes.getSize()-32,digest.getData(),32);
}
void frozenPrefix()
{
    std::cout<<"SECTION frozen_registry_prefix\n";
    const auto bytes=fixture("registry-v16.json"); const auto rows=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())));
    CHECK(rows.isArray()&&rows.size()==1231);const auto registry=parameterRegistry();CHECK(registry.size()>=1231);
    if(rows.size()!=1231||registry.size()<1231)return;
    for(int i=0;i<1231;++i){const auto& d=registry[std::size_t(i)];const auto row=rows[i];
        CHECK(d.id==row["id"].toString());CHECK(d.name==row["name"].toString());CHECK(d.unit==row["unit"].toString());
        CHECK(int(d.kind)==int(row["kind"]));CHECK(d.minimum==float(row["minimum"]));CHECK(d.maximum==float(row["maximum"]));
        CHECK(d.defaultValue==float(row["default"]));CHECK(d.skew==float(row["skew"]));CHECK(d.stateKey==std::uint32_t(juce::int64(row["key"])));
        CHECK(d.choices.size()==std::size_t(row["choices"].size()));for(int c=0;c<row["choices"].size();++c)CHECK(d.choices[std::size_t(c)]==row["choices"][c].toString());
    }
}
void appendedGeometry()
{
    std::cout<<"SECTION appended_control_geometry_and_kind\n";
    CHECK(parameterCount==4519);CHECK(effectBankCount==19);
    const auto registry=parameterRegistry(); std::set<std::uint32_t> keys;
    for(const auto& d:registry)CHECK(keys.insert(d.stateKey).second);
    const char* fields[]{"active","bypassed","shape","frequency_hz","gain_db","q","slope","range_db","threshold_db","attack_ms","release_ms"};
    for(int slot=1;slot<=8;++slot){
        for(int band=1;band<=8;++band)for(int field=0;field<11;++field){
            const auto id="effect."+juce::String(slot)+".dynamic_eq.band."+juce::String(band)+"."+fields[field];
            const auto found=index(id);CHECK(found.has_value());if(found)CHECK(*found==1231+std::size_t(slot-1)*89+std::size_t(band-1)*11+field);
        }
        const auto trim=index("effect."+juce::String(slot)+".dynamic_eq.output_gain_db");CHECK(trim.has_value());if(trim)CHECK(*trim==1231+std::size_t(slot-1)*89+88);
        RackLayout layout;RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=std::uint8_t(slot-1);edit.effect=EffectKind(15);
        CHECK(editRackLayout(layout,edit,layout)==RackEditResult::applied);
    }
    SoundDocument sound;juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));
    RoutedPatch defaults;CHECK(makeRoutedPatch(sound,defaults));for(const auto& b:defaults.effects[0].dynamicEq.bands){
        CHECK(!b.active&&!b.bypassed&&b.shape==DynamicEqShape::bell&&b.slope==DynamicEqSlope::db12&&b.placement==DynamicEqPlacement::stereo);
        CHECK(std::abs(b.frequencyHz-1000)<.001&&b.gainDb==0&&std::abs(b.q-1)<1e-5);
        CHECK(b.rangeDb==0&&b.thresholdDb==-24&&std::abs(b.attackMs-10)<1e-4&&std::abs(b.releaseMs-100)<.001);
    }
    CHECK(readInt(bytes,4)==31);CHECK(readInt(bytes,8)==4519);CHECK(bytes.getSize()==41053);
    SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);
    ProjectDocument project;CHECK(encodeProjectState(project,bytes));
    CHECK(static_cast<const unsigned char*>(bytes.getData())[4]==30);CHECK(readInt(bytes,24)==31);CHECK(bytes.getSize()==41161);
    ProjectDocument decoded;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),decoded)&&decoded==project);
}
void actualOldMigration()
{
    std::cout<<"SECTION actual_old16_state_and_project_migration\n";
    for(const auto* name:{"default-v16.soundstate","rich-v16.soundstate","embedded-v16.soundstate"}){
        const auto bytes=fixture(name);SoundDocument decoded;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),decoded));
        juce::MemoryInputStream stream(bytes,false);stream.setPosition(16);
        for(int i=0;i<1231;++i){const auto key=std::uint32_t(stream.readInt());const auto value=stream.readFloat();const auto found=parameterIndexForKey(key);
            CHECK(found&&*found<1231);if(found)CHECK(std::bit_cast<std::uint32_t>(decoded.values[*found])==std::bit_cast<std::uint32_t>(value));}
        const auto defaults=defaultParameterValues();for(std::size_t i=1231;i<parameterCount;++i)CHECK(decoded.values[i]==defaults[i]&&!decoded.controlLocks[i]);
        stream.setPosition(16+1231*8+78);
        for(const auto& macro:decoded.macros){CHECK(macro.targetKey==std::uint32_t(stream.readInt()));CHECK(macro.amount==stream.readFloat());}
        for(std::size_t i=0;i<1231;++i)CHECK(decoded.controlLocks[i]==(stream.readByte()!=0));
        for(const auto lock:decoded.synthLocks)CHECK(lock==(stream.readByte()!=0));for(const auto lock:decoded.effectLocks)CHECK(lock==(stream.readByte()!=0));
        CHECK(stream.getPosition()==11249);CHECK(stream.readInt()==136);stream.skipNextBytes(8);
        for(const auto& banks:decoded.compatibility.synth)for(const auto revision:banks)CHECK(revision==std::uint8_t(stream.readByte()));
        for(const auto& banks:decoded.compatibility.effects){for(std::size_t i=0;i<15;++i)CHECK(banks[i]==std::uint8_t(stream.readByte()));if(banks.size()>15)CHECK(banks[15]==1);}
        juce::MemoryBlock current;CHECK(encodeSoundState(decoded,current));SoundDocument recalled;CHECK(decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded);
        if(juce::String(name).startsWith("embedded"))CHECK(decoded.personalities&&decoded.personalities->instances.size()==2);
    }
    const auto bytes=fixture("rich-v15.airackproject");ProjectDocument decoded;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),decoded));
    const auto sound=fixture("rich-v16.soundstate");SoundDocument expected;CHECK(decodeSoundState(sound.getData(),sound.getSize(),expected));CHECK(decoded.sound==expected);
    juce::MemoryBlock current;CHECK(encodeProjectState(decoded,current));ProjectDocument recalled;CHECK(decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded);
    auto bad=fixture("default-v16.soundstate");auto* data=static_cast<unsigned char*>(bad.getData());data[16+1231*8+10]=15;sign(bad);
    const auto before=expected;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),expected));CHECK(expected==before);
    if(const auto newKey=index("effect.1.dynamic_eq.band.1.frequency_hz")){
        bad=fixture("default-v16.soundstate");juce::MemoryOutputStream output(bad,true);CHECK(output.setPosition(16));output.writeInt(std::bit_cast<std::int32_t>(parameterRegistry()[*newKey].stateKey));sign(bad);
        CHECK(!decodeSoundState(bad.getData(),bad.getSize(),expected));CHECK(expected==before);
        bad=fixture("default-v16.soundstate");juce::MemoryOutputStream macro(bad,true);CHECK(macro.setPosition(16+1231*8+78));macro.writeInt(std::bit_cast<std::int32_t>(parameterRegistry()[*newKey].stateKey));sign(bad);
        CHECK(!decodeSoundState(bad.getData(),bad.getSize(),expected));CHECK(expected==before);
        const auto embedded=fixture("embedded-v16.soundstate");SoundDocument owner;
        CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),owner));
        juce::MemoryBlock control;CHECK(airack::test::oldEnvelopeWithPersonality(embedded,11389,owner,control));CHECK(control==embedded);
        SoundDocument valid;CHECK(decodeSoundState(control.getData(),control.getSize(),valid)&&valid==owner);
        auto personalities=std::make_shared<RackPersonalityState>(*owner.personalities);
        personalities->instances.back().design.units[0].defaults.push_back({0,"dynamic_eq.band.1.frequency_hz",.5f});
        owner.personalities=personalities;juce::MemoryBlock modern;
        // Current recall admits retained foreign banks; Sound16 must keep its
        // independently limited old nested parameter domain.
        CHECK(encodeSoundState(owner,modern));
        juce::MemoryBlock forged;CHECK(airack::test::oldEnvelopeWithPersonality(embedded,11389,owner,forged));
        CHECK(!decodeSoundState(forged.getData(),forged.getSize(),expected));CHECK(expected==before);
        // The current Rack2 decoder now knows module17. Its signed presence in
        // an old Sound16 envelope must still fail the frozen nested-module domain.
        CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),owner));
        personalities=std::make_shared<RackPersonalityState>(*owner.personalities);
        const auto factories=airack::test::frozenFactoryRackDesigns();
        const auto dynamic=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Contour Dynamics";});
        CHECK(dynamic!=factories.end());
        if(dynamic!=factories.end()){
            auto& placed=personalities->instances.back();placed.design=*dynamic;
            owner.layout.effects[placed.bindings.front().slot].kind=EffectKind::dynamicEq;
            owner.personalities=personalities;CHECK(encodeSoundState(owner,modern));
            juce::MemoryBlock nested;CHECK(airack::test::oldEnvelopeWithPersonality(embedded,11389,owner,nested));
            CHECK(!decodeSoundState(nested.getData(),nested.getSize(),expected));CHECK(expected==before);
        }
    }
}
void settingsAndLocks()
{
    std::cout<<"SECTION all_bands_mapping_retention_and_slot_locks\n";
    SoundDocument sound;
    const auto set=[&](const juce::String& id,float value){
        const auto found=index(id);CHECK(found.has_value());if(!found)return;
        const auto& d=parameterRegistry()[*found];
        sound.values[*found]=d.kind==ParameterKind::continuous?juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(value)
            :(value-d.minimum)/(d.maximum-d.minimum);
    };
    for(int slot=0;slot<8;++slot){
        RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=std::uint8_t(slot);edit.destination=slot%2?masterChain:0;
        edit.position=sound.layout.effectCount[edit.destination];edit.effect=EffectKind(15);
        CHECK(editRackLayout(sound.layout,edit,sound.layout)==RackEditResult::applied);
        const auto prefix="effect."+juce::String(slot+1)+".dynamic_eq.";
        for(int band=0;band<8;++band){const auto p=prefix+"band."+juce::String(band+1)+".";
            set(p+"active",float(band%2));set(p+"bypassed",float(band%3==0));set(p+"shape",float((slot+band)%6));
            set(p+"frequency_hz",20.f*float(1<<band));set(p+"gain_db",float(band*6-21));set(p+"q",band%2?18.f:.25f);
            set(p+"slope",float(band%2));set(p+"range_db",band%2?12.f:-12.f);set(p+"threshold_db",float(-48+band*6));
            set(p+"attack_ms",band%2?200.f:.1f);set(p+"release_ms",band%2?2000.f:10.f);
            set(p+"placement",float((slot+band)%5));
        }
        set(prefix+"output_gain_db",float(slot*4-20));
    }
    RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
    for(int slot=0;slot<8;++slot){const auto& eq=patch.effects[std::size_t(slot)].dynamicEq;
        CHECK(std::abs(eq.outputGainDb-float(slot*4-20))<1e-4);
        for(int band=0;band<8;++band){const auto& b=eq.bands[std::size_t(band)];
            CHECK(b.active==bool(band%2));CHECK(b.bypassed==(band%3==0));CHECK(int(b.shape)==(slot+band)%6);
            CHECK(std::abs(b.frequencyHz-20.f*float(1<<band))<.01);CHECK(std::abs(b.gainDb-float(band*6-21))<1e-4);
            CHECK(std::abs(b.q-(band%2?18.f:.25f))<1e-4);CHECK(int(b.slope)==band%2);
            CHECK(std::abs(b.rangeDb-(band%2?12.f:-12.f))<1e-4);CHECK(std::abs(b.thresholdDb-float(-48+band*6))<1e-4);
            CHECK(std::abs(b.attackMs-(band%2?200.f:.1f))<1e-3);CHECK(std::abs(b.releaseMs-(band%2?2000.f:10.f))<.01);
            CHECK(int(b.placement)==(slot+band)%5);
        }
        auto locked=sound;locked.effectLocks[std::size_t(slot)]=true;
        for(const auto control:std::array<std::size_t,4>{1231+std::size_t(slot)*89,1231+std::size_t(slot)*89+88,1959+std::size_t(slot)*8,1959+std::size_t(slot)*8+7}){
            const auto changedValue=sound.values[control]==1.f?0.f:1.f;
            auto changed=locked;changed.values[control]=changedValue;
            CHECK(!validateAiSoundEdit(locked,changed));
            auto individual=sound;individual.controlLocks[control]=true;changed=individual;changed.values[control]=changedValue;
            CHECK(!validateAiSoundEdit(individual,changed));
        }
        auto macro=locked;macro.macros[0]={parameterRegistry()[1231+std::size_t(slot)*89+3].stateKey,.1f};CHECK(!validateAiSoundEdit(locked,macro));
    }
    juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);
    for(std::size_t field=0;field<89;++field){auto invalid=sound;invalid.values[1231+7*89+field]=std::numeric_limits<float>::quiet_NaN();
        const auto before=patch;CHECK(!makeRoutedPatch(invalid,patch));CHECK(patch==before);
    }
    auto revision=sound;revision.compatibility.effects[7][15]=2;CHECK(!validateSoundDocument(revision));
}
void malformedNewStates()
{
    std::cout<<"SECTION strict_new_state_lengths_keys_values_and_revisions\n";
    SoundDocument sound;juce::MemoryBlock valid;CHECK(encodeSoundState(sound,valid));
    const auto reject=[&](juce::MemoryBlock bytes){sign(bytes);auto output=sound;output.values[0]=.125f;const auto before=output;
        CHECK(!decodeSoundState(bytes.getData(),bytes.getSize(),output));CHECK(output==before);};
    auto bad=valid;std::memcpy(static_cast<char*>(bad.getData())+24,static_cast<const char*>(bad.getData())+16,4);reject(bad);
    bad=valid;const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(static_cast<char*>(bad.getData())+16+(1231+7*89)*8+4,&nan,4);reject(bad);
    bad=valid;const float noncanonical=.3f;std::memcpy(static_cast<char*>(bad.getData())+16+(1231+2)*8+4,&noncanonical,4);reject(bad);
    bad=valid;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+4+8+8+7*19+15]=2;reject(bad);
    bad=valid;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+4+5]=15;reject(bad);
    bad=valid;bad.setSize(bad.getSize()-1);reject(bad);
    bad=valid;bad.insert("x",1,bad.getSize()-32);reject(bad);
    auto output=sound;const auto before=output;CHECK(!decodeSoundState(nullptr,valid.getSize(),output));CHECK(output==before);
}
std::vector<float> render(const RoutedPatch& patch,int partition=64,RenderQuality quality=RenderQuality::realtime,int frames=8192)
{
    RoutedSynthEngine engine;CHECK(engine.prepare(48000,512,quality));CHECK(engine.setPatch(patch));engine.reset();
    RoutedSynthEngine baseline;CHECK(baseline.prepare(48000,512,quality));CHECK(engine.latencySamples()==baseline.latencySamples());
    std::vector<float> output(std::size_t(frames)*2);juce::AudioBuffer<float> storage(2,512);juce::MidiBuffer midi;midi.ensureSize(256);
    int start=0,turn=0;
    while(start<frames){const int block=std::min(frames-start,partition==0?std::array<int,5>{17,139,512,64,211}[std::size_t(turn++%5)]:partition);
        juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(),2,block);midi.clear();
        if(start==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        if(start<=4096&&4096<start+block)midi.addEvent(juce::MidiMessage::noteOff(1,60),4096-start);
        engine.render(view,midi,120.);
        for(int n=0;n<block;++n)for(int ch=0;ch<2;++ch){const auto x=view.getSample(ch,n);CHECK(std::isfinite(x));output[std::size_t(start+n)*2+ch]=x;}
        start+=block;
    }
    return output;
}
double difference(const std::vector<float>& a,const std::vector<float>& b)
{
    CHECK(a.size()==b.size());double error=0;
    for(std::size_t i=0;i<std::min(a.size(),b.size());++i)error=std::max(error,std::abs(double(a[i])-b[i]));return error;
}
void routedAudio()
{
    std::cout<<"SECTION actual_physical_slot_routed_audio\n";
    RoutedPatch dry;dry.synth.masterLevel=1;dry.synth.layers[0].level=.2f;
    const auto carrier=render(dry);double peak=0;for(const auto x:carrier)peak=std::max(peak,std::abs(double(x)));CHECK(peak>1e-5);
    for(const std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)})for(const std::uint8_t chain:{std::uint8_t(0),masterChain}){
        auto wet=dry;RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=slot;edit.destination=chain;edit.effect=EffectKind::dynamicEq;
        CHECK(editRackLayout(wet.layout,edit,wet.layout)==RackEditResult::applied);
        CHECK(difference(carrier,render(wet))==0);
        auto& settings=wet.effects[slot].dynamicEq;auto& band=settings.bands[0];band.active=true;band.gainDb=9;band.frequencyHz=1000;
        const auto actual=render(wet);CHECK(difference(carrier,actual)>1e-5);
        DynamicEq oracle;CHECK(oracle.setSettings(settings));CHECK(oracle.prepare(48000));
        double error=0;
        for(std::size_t n=0;n<carrier.size()/2;++n){const auto y=oracle.process({carrier[n*2],carrier[n*2+1]});
            error=std::max({error,std::abs(double(actual[n*2])-float(y[0])),std::abs(double(actual[n*2+1])-float(y[1]))});}
        std::cout<<"ROUTED slot="<<int(slot)<<" chain="<<int(chain)<<" core_error="<<error<<'\n';CHECK(error<1e-7);
        wet.effects[slot].bypassed=true;CHECK(difference(carrier,render(wet))==0);wet.effects[slot].bypassed=false;
        band.bypassed=true;CHECK(difference(carrier,render(wet))==0);band.bypassed=false;
        band.active=false;CHECK(difference(carrier,render(wet))==0);band.active=true;
        for(float range:{-12.f,12.f}){band.rangeDb=range;band.thresholdDb=-60;band.attackMs=.1f;const auto dynamic=render(wet);
            CHECK(difference(actual,dynamic)>1e-6);CHECK(difference(dynamic,render(wet,512))==0);CHECK(difference(dynamic,render(wet,0))==0);}
        band.rangeDb=0;
        auto inactive=wet;inactive.effects[slot==0?7:0].dynamicEq.bands[3].gainDb=-24;inactive.effects[slot==0?7:0].dynamicEq.bands[3].active=true;
        CHECK(difference(actual,render(inactive))==0);
    }
    auto wet=dry;RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=0;edit.effect=EffectKind::dynamicEq;CHECK(editRackLayout(wet.layout,edit,wet.layout)==RackEditResult::applied);
    wet.effects[0].dynamicEq.bands[0].active=true;wet.effects[0].dynamicEq.bands[0].gainDb=-9;
    const auto higher=render(wet,64,RenderQuality::higher);CHECK(difference(higher,render(wet,512,RenderQuality::higher))==0);
    CHECK(difference(higher,render(wet,0,RenderQuality::higher))==0);
}
void externalDetectorRouting()
{
    std::cout<<"SECTION external_detector_routing_quality_alignment_and_worker_partitions\n";
    constexpr int frames=8192;
    const auto trigger=[](int frame,double rate)->DynamicEq::Frame{
        if(frame<513||frame>=4097)return {};
        const double x=.5*std::sin(2*juce::MathConstants<double>::pi*100*(frame-513)/rate);
        return {x,-x*.7};
    };
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})
    for(std::uint8_t chain:{std::uint8_t(0),masterChain}){
        RoutedPatch patch;patch.synth.masterLevel=1;patch.synth.layers[0].level=.2f;patch.synth.layers[0].ampEnvelope.sustain=1;
        for(std::size_t i=0;i<4;++i){patch.synth.layers[i].algorithmRevision=modernSynthAlgorithmRevision;patch.compatibility.synth[i].fill(modernSynthAlgorithmRevision);}
        for(std::uint8_t slot=0;slot<2;++slot){
            CHECK(editRackLayout(patch.layout,{RackEditKind::addEffect,slot,chain,slot,EffectKind::dynamicEq},patch.layout)==RackEditResult::applied);
            auto& b=patch.effects[slot].dynamicEq.bands[0];b.active=true;b.frequencyHz=261.62555f;b.rangeDb=-12;b.thresholdDb=-30;b.attackMs=.1f;b.releaseMs=10;
            b.detectorSource=DynamicEqDetectorSource::externalFree;b.detectorLowHz=50;b.detectorHighHz=200;
        }
        const int factor=quality==RenderQuality::higher?2:1;
        DynamicEq oracle;CHECK(oracle.setSettings(patch.effects[0].dynamicEq)&&oracle.prepare(rate*factor));
        std::vector<float> movement(std::size_t(frames*factor));
        for(int n=0;n<frames*factor;++n){(void)oracle.process({},trigger(n/factor,rate));movement[std::size_t(n)]=oracle.dynamicGainsDb()[0];}
        CHECK(*std::min_element(movement.begin(),movement.end()) < -6);
        const auto phrase=[&](int partition,bool connected,bool internal=false){
            auto requested=patch;if(internal)for(auto& e:requested.effects)e.dynamicEq.bands[0].detectorSource=DynamicEqDetectorSource::internalBand;
            FxMonitor monitor;RoutedSynthEngine engine;engine.attachFxMonitor(&monitor);CHECK(engine.setPatch(requested)&&engine.prepare(rate,512,quality));
            CHECK(engine.latencySamples()==(factor==2?21:0));
            const auto token=monitor.subscribe(0);CHECK(token.has_value());
            std::vector<float> result(std::size_t(frames)*2);juce::AudioBuffer<float> storage(2,512);juce::MidiBuffer midi;midi.ensureSize(256);
            std::array<DynamicEq::Frame,512> sidechain;bool finite=true,meters=true,unchanged=true;std::size_t packets=0;
            for(int start=0,turn=0;start<frames;){
                const int block=std::min(frames-start,partition==0?std::array<int,5>{17,139,512,64,211}[std::size_t(turn++%5)]:partition);
                for(int n=0;n<block;++n)sidechain[std::size_t(n)]=trigger(start+n,rate);
                juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(),2,block);midi.clear();if(start==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
                if(connected)engine.render(view,midi,120.,std::span<const DynamicEq::Frame>(sidechain).first(std::size_t(block)));
                else engine.render(view,midi,120.);
                for(int n=0;n<block;++n){unchanged=unchanged&&sidechain[std::size_t(n)]==trigger(start+n,rate);for(int ch=0;ch<2;++ch){const auto x=view.getSample(ch,n);finite=finite&&std::isfinite(x);result[std::size_t(start+n)*2+ch]=x;}}
                FxMonitor::Packet packet;while(token&&monitor.read(*token,packet)){
                    ++packets;const auto last=std::size_t(packet.firstSample)+FxMonitor::packetFrames-1;
                    if(!internal)meters=meters&&last<movement.size()&&std::abs(packet.movementDb[0]-(connected?movement[last]:0))<1e-5;
                }
                start+=block;
            }
            CHECK(finite&&unchanged&&meters);CHECK(packets==std::size_t(frames*factor)/FxMonitor::packetFrames);
            const auto workers=engine.dspWorkerStats();CHECK(workers.prepared==3);
            if(chain==masterChain&&partition>=64)CHECK(workers.blocks>0);
            return result;
        };
        const auto wet=phrase(64,true),dry=phrase(64,false);
        CHECK(difference(wet,dry)>1e-5);
        CHECK(std::equal(wet.begin(),wet.begin()+513*2,dry.begin()));
        CHECK(difference(wet,phrase(512,true))==0);CHECK(difference(wet,phrase(0,true))==0);
        if(rate==48000)CHECK(difference(wet,phrase(1,true))==0);
        CHECK(difference(phrase(64,true,true),phrase(64,false,true))==0);
    }
}
void externalDetectorLifetimes()
{
    std::cout<<"SECTION external_detector_disconnect_reset_invalid_blocks_and_prepared_heap\n";
#if defined(_MSC_VER) && defined(_DEBUG)
    struct Heap {
        static int& calls(){static thread_local int calls=0;return calls;}
        static bool& active(){static thread_local bool active=false;return active;}
        static int __cdecl hook(int kind,void*,std::size_t,int,long,const unsigned char*,int)
        {if(active()&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC||kind==_HOOK_FREE))++calls();return 1;}
    };
    const auto previousHook=_CrtSetAllocHook(Heap::hook);
#endif
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedPatch patch;patch.synth.layers[0].level=.1f;
        for(std::size_t i=0;i<4;++i){patch.synth.layers[i].algorithmRevision=modernSynthAlgorithmRevision;patch.compatibility.synth[i].fill(modernSynthAlgorithmRevision);}
        for(std::uint8_t slot=0;slot<2;++slot){
            CHECK(editRackLayout(patch.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::dynamicEq},patch.layout)==RackEditResult::applied);
            for(std::size_t j=0;j<8;++j){auto& b=patch.effects[slot].dynamicEq.bands[j];b.active=true;b.frequencyHz=100+300*float(j);b.rangeDb=j%2?-6.f:6.f;b.thresholdDb=-40;b.attackMs=.1f;b.releaseMs=10;
                b.detectorSource=j%2?DynamicEqDetectorSource::externalBand:DynamicEqDetectorSource::externalFree;b.detectorLowHz=30;b.detectorHighHz=1000;
                b.placement=DynamicEqPlacement(j%5);b.autoThreshold=j%3==0;b.adaptiveTiming=j%2==0;}
        }
        FxMonitor monitor;RoutedSynthEngine engine,reference;engine.attachFxMonitor(&monitor);
        CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));CHECK(reference.setPatch(patch)&&reference.prepare(rate,512,quality));
        auto token=monitor.subscribe(0);CHECK(token.has_value());const int factor=quality==RenderQuality::higher?2:1;
        DynamicEq oracle;CHECK(oracle.setSettings(patch.effects[0].dynamicEq)&&oracle.prepare(rate*factor));
        juce::AudioBuffer<float> actual(2,512),expected(2,512),invalid(2,513);juce::MidiBuffer midi;midi.ensureSize(256);
        std::array<DynamicEq::Frame,512> sidechain;std::vector<std::array<float,8>> gains;gains.reserve(60000);bool exact=true,finite=true,meters=true,invalidSilent=true;std::size_t packets=0;int position=0;
        for(int trial=0;trial<180;++trial){
            if(trial==90){engine.reset();reference.reset();oracle.reset();gains.clear();position=0;}
            if(trial==120){const auto nextRate=rate==44100?96000.:44100.;CHECK(engine.prepare(nextRate,512,quality)&&reference.prepare(nextRate,512,quality)&&oracle.prepare(nextRate*factor));gains.clear();position=0;}
            const int count=std::array<int,6>{1,17,64,255,511,512}[std::size_t(trial%6)];const bool connected=trial<30||(trial>=60&&trial<145);
            const double runningRate=trial>=120?(rate==44100?96000.:44100.):rate;
            for(int n=0;n<count;++n){const double x=.5*std::sin(2*juce::MathConstants<double>::pi*500*(position+n)/runningRate);sidechain[std::size_t(n)]={x,-.4*x};}
            if(trial==65)sidechain[0]={std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()};
            auto external=connected?std::span<const DynamicEq::Frame>(sidechain).first(std::size_t(count)):std::span<const DynamicEq::Frame>{};
            for(int n=0;n<count;++n){auto x=connected?sidechain[std::size_t(n)]:DynamicEq::Frame{};for(auto& v:x)if(!std::isfinite(v))v=0;
                for(int phase=0;phase<factor;++phase){(void)oracle.process({},x);gains.push_back(oracle.dynamicGainsDb());}}
            actual.setSize(2,count,false,false,true);expected.setSize(2,count,false,false,true);midi.clear();
            if(trial==0||trial==90||trial==120)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
#if defined(_MSC_VER) && defined(_DEBUG)
            Heap::active()=true;
#endif
            if(trial%11==0){engine.render(invalid,midi,120.,external);invalidSilent=invalidSilent&&invalid.getMagnitude(0,513)==0;
                if(count>1){engine.render(actual,midi,120.,std::span<const DynamicEq::Frame>(sidechain).first(std::size_t(count-1)));invalidSilent=invalidSilent&&actual.getMagnitude(0,count)==0;}}
            engine.render(actual,midi,120.,external);reference.render(expected,midi,120.,external);
#if defined(_MSC_VER) && defined(_DEBUG)
            Heap::active()=false;
#endif
            for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch){const auto x=actual.getSample(ch,n);exact=exact&&std::bit_cast<std::uint32_t>(x)==std::bit_cast<std::uint32_t>(expected.getSample(ch,n));finite=finite&&std::isfinite(x);}
            FxMonitor::Packet packet;while(token&&monitor.read(*token,packet)){
                ++packets;const auto last=std::size_t(packet.firstSample)+FxMonitor::packetFrames-1;meters=meters&&last<gains.size();
                if(last<gains.size())for(std::size_t j=0;j<8;++j)meters=meters&&std::abs(packet.movementDb[j]-gains[last][j])<1e-5;
            }
            position+=count;
        }
        CHECK(exact&&finite&&meters&&invalidSilent);CHECK(packets>100);CHECK(engine.dspWorkerStats().blocks>0);
#if defined(_MSC_VER) && defined(_DEBUG)
        CHECK(Heap::calls()==0);
#endif
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previousHook);
#endif
}
void routedSchedulingFingerprint()
{
    std::cout<<"SECTION routed_eq_automation_and_scheduling_characterization\n";
    constexpr std::array<std::uint64_t,4> expected {0x47808e181c44df0cull,0xa7796cda9a63d194ull,0x88c99ef3fa80cfb0ull,0xb1020f1ee0cbea07ull};
    std::size_t scene=0;
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedPatch initial;
        for(std::uint8_t i=1;i<4;++i)CHECK(editRackLayout(initial.layout,{RackEditKind::addSynth,i,masterChain,i},initial.layout)==RackEditResult::applied);
        for(std::size_t i=0;i<4;++i){auto& l=initial.synth.layers[i];l.enabled=true;l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.algorithmRevision=modernSynthAlgorithmRevision;initial.compatibility.synth[i].fill(modernSynthAlgorithmRevision);l.level=.2f;l.ampEnvelope.sustain=1;}
        for(std::uint8_t i=0;i<8;++i){CHECK(editRackLayout(initial.layout,{RackEditKind::addEffect,i,masterChain,i,EffectKind::dynamicEq},initial.layout)==RackEditResult::applied);
            for(std::size_t j=0;j<8;++j){auto& b=initial.effects[i].dynamicEq.bands[j];b.active=true;b.frequencyHz=180+700*float(j);b.gainDb=j%2?2.f:-2.f;b.rangeDb=j%2?12.f:-12.f;b.thresholdDb=-40;}}
        initial.outputGainDb=-6;
        const auto phrase=[&](int partition){RoutedSynthEngine engine;CHECK(engine.setPatch(initial)&&engine.prepare(rate,512,quality));auto patch=initial;std::vector<float> result;juce::AudioBuffer<float> storage(2,512);juce::MidiBuffer midi;midi.ensureSize(2048);
            const auto append=[&](int frames,bool start=false,bool stop=false){for(int offset=0;offset<frames;){const int count=std::min(partition,frames-offset);juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(),2,count);midi.clear();
                if(start&&offset==0)for(int k=0;k<16;++k)midi.addEvent(juce::MidiMessage::noteOn(1,48+k,juce::uint8(80+k)),0);
                if(stop){if(offset<=17&&17<offset+count)midi.addEvent(juce::MidiMessage::midiStop(),17-offset);if(offset<=23&&23<offset+count)midi.addEvent(juce::MidiMessage::noteOn(1,67,.7f),23-offset);}
                engine.render(view,midi,120.);for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)result.push_back(view.getSample(ch,n));offset+=count;
            }};
            const int ramp=int(std::ceil(rate*.005));append(int(rate*.013),true);
            patch.effects[3].stereo={6,.25f,1.5f,StereoChannelMode::swap};
            for(std::size_t i=0;i<8;++i){auto& b=patch.effects[i].dynamicEq.bands[i];b.frequencyHz=i%2?20.f:20000.f;b.shape=static_cast<DynamicEqShape>(i%6);b.gainDb=i%2?-9.f:9.f;b.rangeDb=-b.rangeDb;}
            patch.mute[0]=true;patch.solo[2]=true;patch.outputGainDb=3;CHECK(engine.setPatch(patch));append(ramp-1);
            patch.mute[0]=false;patch.solo[2]=false;patch.effects[0].dynamicEq.outputGainDb=-3;CHECK(engine.setPatch(patch));append(ramp+1);
            patch.effects[0].bypassed=true;CHECK(engine.setPatch(patch));append(ramp/2);patch.effects[0].bypassed=false;CHECK(engine.setPatch(patch));append(3*ramp+13);
            patch.layout.effects[3].kind=EffectKind::stereo;CHECK(engine.setPatch(patch));append(3*ramp+17);patch.layout.effects[3].kind=EffectKind::dynamicEq;CHECK(engine.setPatch(patch));append(3*ramp+19);
            for(int k=0;k<3;++k){CHECK(engine.setPatch(patch));append(37);}append(401,false,true);
            CHECK(!engine.prepare(7999,512));append(103);engine.reset();append(137,true);return result;
        };
        const auto fine=phrase(64),coarse=phrase(512),irregular=phrase(73);CHECK(difference(fine,coarse)==0&&difference(fine,irregular)==0);
        std::uint64_t hash=14695981039346656037ull;bool finite=true;for(float v:fine){finite=finite&&std::isfinite(v);const auto bits=std::bit_cast<std::uint32_t>(v);for(int k=0;k<4;++k)hash=(hash^((bits>>(8*k))&255))*1099511628211ull;}CHECK(finite);
        CHECK(hash==expected[scene++]);std::cout<<"ROUTED_SCHEDULING_FINGERPRINT rate="<<rate<<" quality="<<int(quality)<<" frames="<<fine.size()/2<<" fnv="<<std::hex<<hash<<std::dec<<'\n';
        if(const auto* directory=std::getenv("AIRACK_ROUTED_EQ_CHARACTERIZATION_DIR")){const juce::File folder{juce::String(directory)};CHECK(juce::File::isAbsolutePath(directory)&&folder.isAChildOf(juce::File::getCurrentWorkingDirectory()));CHECK(folder.createDirectory().wasOk());const auto file=folder.getChildFile("routed-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");CHECK(!file.exists());if(!file.exists())CHECK(file.replaceWithData(fine.data(),fine.size()*sizeof(float)));}
    }
}
void pipelineFingerprint()
{
    std::cout<<"SECTION original_long_pipeline_and_serial_handoff_audio_packets\n";
    constexpr std::array<std::uint64_t,4> audioExpected {0x45599cc9c40cf5e3ull,0x6f3bc68d0fb71013ull,0x979129ef62113d67ull,0xae6283ab83c5dd0ull};
    constexpr std::array<std::uint64_t,12> packetExpected {0xa00cecd4ef04a1e6ull,0xd255eceeb4a898b3ull,0xff83cce89a75fb69ull,0x43e21e54765c125cull,0x8ffc14cfaeb4f2d7ull,0x454bbad300cd55d9ull,0x58cd1897226f949aull,0xa73c08842bc3f7bcull,0xbdf8b51f0eab2eccull,0x7f6ce7ed28a79f24ull,0xa304fdc769f35136ull,0x122f7ec751faa144ull};
    std::size_t scene=0,packetScene=0;
    using Event=std::pair<int,juce::MidiMessage>;
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedPatch initial;
        for(std::uint8_t i=1;i<4;++i)CHECK(editRackLayout(initial.layout,{RackEditKind::addSynth,i,masterChain,i},initial.layout)==RackEditResult::applied);
        for(std::size_t i=0;i<4;++i){auto& l=initial.synth.layers[i];l.enabled=true;l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.algorithmRevision=modernSynthAlgorithmRevision;initial.compatibility.synth[i].fill(modernSynthAlgorithmRevision);l.level=.2f;l.ampEnvelope.sustain=1;}
        for(std::uint8_t i=0;i<8;++i){CHECK(editRackLayout(initial.layout,{RackEditKind::addEffect,i,masterChain,i,EffectKind::dynamicEq},initial.layout)==RackEditResult::applied);
            for(std::size_t j=0;j<8;++j){auto& b=initial.effects[i].dynamicEq.bands[j];b.active=true;b.frequencyHz=1000;b.q=1;b.rangeDb=j%2?12.f:-12.f;b.thresholdDb=j%2?-43.f:-40.f;b.attackMs=10;b.releaseMs=100;}}
        initial.outputGainDb=-6;
        const auto phrase=[&](int partition){
            FxMonitor monitor;auto engine=std::make_unique<RoutedSynthEngine>();engine->attachFxMonitor(&monitor);CHECK(engine->setPatch(initial)&&engine->prepare(rate,512,quality));auto patch=initial;
            std::array<std::optional<FxMonitor::Token>,8> tokens;for(std::uint8_t i=0;i<8;++i){tokens[i]=monitor.subscribe(i);CHECK(tokens[i].has_value());}
            std::array<std::uint64_t,8> packetHashes;packetHashes.fill(14695981039346656037ull);std::size_t packetCount=0;std::vector<float> result;
            const auto fold=[](std::uint64_t& hash,std::uint64_t word,int bytes){for(int k=0;k<bytes;++k)hash=(hash^((word>>(8*k))&255))*1099511628211ull;};
            const auto consume=[&]{FxMonitor::Packet packet;for(std::size_t i=0;i<8;++i)if(tokens[i])while(monitor.read(*tokens[i],packet)){
                auto& hash=packetHashes[i];++packetCount;fold(hash,packet.token,8);fold(hash,packet.generation,8);fold(hash,packet.firstSample,8);fold(hash,std::bit_cast<std::uint64_t>(packet.sampleRate),8);
                for(const auto& frame:packet.samples)for(float v:frame)fold(hash,std::bit_cast<std::uint32_t>(v),4);for(float v:packet.movementDb)fold(hash,std::bit_cast<std::uint32_t>(v),4);
            }};
            juce::AudioBuffer<float> storage(2,512);juce::MidiBuffer midi;midi.ensureSize(4096);
            const auto append=[&](int frames,const std::vector<Event>& events=std::vector<Event>{},std::optional<double> tempo=120.){
                for(int offset=0;offset<frames;){const int count=std::min(partition,frames-offset);juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(),2,count);midi.clear();
                    for(const auto& [at,message]:events)if(at>=offset&&at<offset+count)midi.addEvent(message,at-offset);engine->render(view,midi,tempo);
                    for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)result.push_back(view.getSample(ch,n));consume();offset+=count;}
            };
            std::vector<Event> notes;for(int n=48;n<64;++n)notes.emplace_back(0,juce::MidiMessage::noteOn(1,n,juce::uint8(100)));
            append(int(rate*.25),notes);
            for(std::size_t i=0;i<8;++i){patch.effects[i].dynamicEq.bands[i].frequencyHz=200+1300*float(i);patch.effects[i].dynamicEq.bands[i].gainDb=i%2?3.f:-3.f;}CHECK(engine->setPatch(patch));append(int(rate*.017));
            patch.effects[1].bypassed=true;CHECK(engine->setPatch(patch));append(int(rate*.014));patch.effects[1].bypassed=false;CHECK(engine->setPatch(patch));append(int(rate*.017));
            CHECK(monitor.requestAudition(*tokens[4],0));append(int(rate*.009));CHECK(monitor.retire(*tokens[4]));tokens[4].reset();append(int(rate*.013));tokens[4]=monitor.subscribe(4);CHECK(tokens[4].has_value());append(int(rate*.013));
            patch.synth.layers[0].cutoffHz=1700;patch.outputGainDb=2;patch.mute[1]=true;CHECK(engine->setPatch(patch));append(int(rate*.013));
            patch.synth.layers[0].lfos[0].rateHz=3;patch.synth.layers[0].modulation[0]={true,ModSource::lfo1,ModTarget::cutoff,ModBank::analog,.1f};CHECK(engine->setPatch(patch));append(int(rate*.017));
            patch.synth.layers[0].modulation[0].enabled=false;patch.mute[1]=false;CHECK(engine->setPatch(patch));append(int(rate*.021),{},std::nullopt);
            append(int(rate*.019),{{19,juce::MidiMessage::noteOff(1,60)}});
            append(int(rate*.021),{{17,juce::MidiMessage::midiStop()},{23,juce::MidiMessage::noteOn(1,67,.7f)}});
            patch.effects[7].stereo={6,.25f,1.5f,StereoChannelMode::swap};patch.layout.effects[7].kind=EffectKind::stereo;CHECK(engine->setPatch(patch));append(int(rate*.021));patch.layout.effects[7].kind=EffectKind::dynamicEq;CHECK(engine->setPatch(patch));append(int(rate*.021));
            const auto active=engine->activeNoteCount();juce::MidiBuffer empty;juce::AudioBuffer<float> invalid(1,64),oversized(2,513),zero(2,0);invalid.clear();invalid.setSample(0,0,1);engine->render(invalid,empty);CHECK(invalid.getMagnitude(0,64)==0);engine->render(oversized,empty);engine->render(zero,empty);CHECK(engine->activeNoteCount()==active);CHECK(!engine->prepare(7999,512));append(137);
            engine->reset();append(1337,{{0,juce::MidiMessage::noteOn(1,55,.8f)}});CHECK(engine->prepare(rate==48000?96000:48000,512,quality));append(1103,{{0,juce::MidiMessage::noteOn(1,62,.8f)}});
            auto moved=std::make_unique<RoutedSynthEngine>(std::move(*engine));engine=std::move(moved);append(1511);consume();CHECK(packetCount>100);
            const auto workers=engine->dspWorkerStats();CHECK(workers.prepared==3);CHECK(partition*(quality==RenderQuality::higher?2:1)<64||workers.blocks>0);

            std::uint64_t packetHash=14695981039346656037ull;for(auto hash:packetHashes)fold(packetHash,hash,8);
            CHECK(packetHash==packetExpected[packetScene++]);std::cout<<"PIPELINE_PACKETS rate="<<rate<<" quality="<<int(quality)<<" partition="<<partition<<" packets="<<packetCount<<" fnv="<<std::hex<<packetHash<<std::dec<<'\n';return result;
        };
        const auto fine=phrase(64),coarse=phrase(512),irregular=phrase(73);CHECK(difference(fine,coarse)==0&&difference(fine,irregular)==0);
        std::uint64_t hash=14695981039346656037ull;bool finite=true;for(float value:fine){finite=finite&&std::isfinite(value);const auto word=std::bit_cast<std::uint32_t>(value);for(int k=0;k<4;++k)hash=(hash^((word>>(8*k))&255))*1099511628211ull;}CHECK(finite);CHECK(hash==audioExpected[scene++]);
        std::cout<<"PIPELINE_AUDIO rate="<<rate<<" quality="<<int(quality)<<" frames="<<fine.size()/2<<" fnv="<<std::hex<<hash<<std::dec<<'\n';
        if(const auto* directory=std::getenv("AIRACK_PIPELINE_CHARACTERIZATION_DIR")){const juce::File folder{juce::String(directory)};CHECK(juce::File::isAbsolutePath(directory)&&folder.isAChildOf(juce::File::getCurrentWorkingDirectory()));CHECK(folder.createDirectory().wasOk());const auto file=folder.getChildFile("pipeline-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");CHECK(!file.exists());if(!file.exists())CHECK(file.replaceWithData(fine.data(),fine.size()*sizeof(float)));}
    }
}
void pipelineEffectCounts()
{
    std::cout<<"SECTION variable_eq_chain_parallel_and_serial_identity\n";
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(std::uint8_t effects=2;effects<=8;++effects){
        RoutedPatch initial;
        for(std::uint8_t i=1;i<4;++i)CHECK(editRackLayout(initial.layout,{RackEditKind::addSynth,i,masterChain,i},initial.layout)==RackEditResult::applied);
        for(std::size_t i=0;i<4;++i){auto& layer=initial.synth.layers[i];layer.enabled=true;layer.engine=i%2?EngineKind::wavetable:EngineKind::analog;layer.algorithmRevision=modernSynthAlgorithmRevision;initial.compatibility.synth[i].fill(modernSynthAlgorithmRevision);layer.level=.08f;layer.ampEnvelope.sustain=1;}
        for(std::uint8_t i=0;i<effects;++i){CHECK(editRackLayout(initial.layout,{RackEditKind::addEffect,i,masterChain,i,EffectKind::dynamicEq},initial.layout)==RackEditResult::applied);
            for(std::size_t band=0;band<8;++band){auto& b=initial.effects[i].dynamicEq.bands[band];b.active=true;b.frequencyHz=90+1200*float(band);b.gainDb=band%2?2.f:-2.f;b.rangeDb=band%2?4.f:-4.f;b.thresholdDb=-48;}}
        initial.outputGainDb=-18;
        const auto phrase=[&](int partition){
            RoutedSynthEngine engine;CHECK(engine.setPatch(initial)&&engine.prepare(rate,512,quality));auto patch=initial;
            std::vector<float> result;juce::AudioBuffer<float> storage(2,512);juce::MidiBuffer midi;midi.ensureSize(2048);
            const auto append=[&](int frames,bool start=false){for(int offset=0;offset<frames;){const int count=std::min(partition,frames-offset);juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(),2,count);midi.clear();
                if(start&&offset==0)for(int note=48;note<64;++note)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);
                engine.render(view,midi,120.);for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)result.push_back(view.getSample(ch,n));offset+=count;
            }};
            append(1025,true);patch.effects[effects-1].dynamicEq.bands[0].frequencyHz=730;CHECK(engine.setPatch(patch));append(37);
            patch.effects[effects-1].bypassed=true;CHECK(engine.setPatch(patch));append(529);
            patch.effects[effects-1].bypassed=false;CHECK(engine.setPatch(patch));append(137);engine.reset();append(257,true);
            const auto workers=engine.dspWorkerStats();CHECK(workers.prepared==3);CHECK(partition==31?workers.blocks==0:workers.blocks>0);return result;
        };
        // 31 host frames stay below the parallel threshold in both quality modes.
        const auto reference=phrase(31);CHECK(std::all_of(reference.begin(),reference.end(),[](float value){return std::isfinite(value);}));
        for(int partition:{64,73,512}){const auto actual=phrase(partition);CHECK(actual.size()==reference.size());if(actual.size()==reference.size())CHECK(std::memcmp(actual.data(),reference.data(),reference.size()*sizeof(float))==0);}
        std::cout<<"EQ_CHAIN_IDENTITY rate="<<rate<<" quality="<<int(quality)<<" effects="<<int(effects)<<" serial_partition=31 parallel_partitions=64,73,512\n";
    }
}
void actualOldAudio()
{
    std::cout<<"SECTION frozen_old16_raw_audio_identity\n";
    const auto bytes=fixture("rich-v16.soundstate");SoundDocument sound;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),sound));RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
    const auto frozen=fixture("rich-v16-48k-64.raw");juce::MemoryInputStream input(frozen,false);
    CHECK(input.readInt()==0x30465241);CHECK(input.readInt()==48000);CHECK(input.readInt()==12032);CHECK(input.readInt()==64);
    const auto actual=render(patch,64,RenderQuality::realtime,12032);double error=0;
    for(const auto sample:actual){const auto expected=input.readFloat();error=std::max(error,std::abs(double(sample)-expected));}
    CHECK(error==0);std::cout<<"OLD_RAW maximum_error="<<error<<'\n';
}
void preparedLifecycle()
{
    std::cout<<"SECTION routed_live_publication_reset_and_allocation\n";
#if defined(_MSC_VER) && defined(_DEBUG)
    struct Heap {
        static int& count(){static thread_local int count=0;return count;}
        static bool& active(){static thread_local bool active=false;return active;}
        static int __cdecl hook(int kind,void*,std::size_t,int,long,const unsigned char*,int)
        {if(active()&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC))++count();return 1;}
    };
    const auto previousHook=_CrtSetAllocHook(Heap::hook);
#endif
    for(const double rate:{48000.,96000.})for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedPatch patch;patch.synth.layers[0].level=.02f;
        for(std::uint8_t slot=0;slot<8;++slot){
            RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=slot;edit.effect=EffectKind::dynamicEq;
            edit.destination=slot%2?masterChain:0;edit.position=patch.layout.effectCount[edit.destination];CHECK(editRackLayout(patch.layout,edit,patch.layout)==RackEditResult::applied);
            for(std::size_t band=0;band<8;++band){auto& b=patch.effects[slot].dynamicEq.bands[band];b.active=true;b.frequencyHz=40.f*float(1<<band);b.rangeDb=-6;b.thresholdDb=-60;}
        }
        RoutedSynthEngine engine;CHECK(engine.prepare(rate,512,quality));CHECK(engine.setPatch(patch));engine.reset();
        juce::AudioBuffer<float> storage(2,512);juce::MidiBuffer midi;midi.ensureSize(256);
        bool admitted=true,finite=true,preserved=true;double peak=0;
#if defined(_MSC_VER) && defined(_DEBUG)
        Heap::count()=0;
#endif
        for(int trial=0;trial<24;++trial){
            const int block=std::array<int,3>{64,512,139}[std::size_t(trial%3)];juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(),2,block);
            midi.clear();if(trial==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(trial==19)midi.addEvent(juce::MidiMessage::midiStop(),17);
            auto& fx=patch.effects[std::size_t(trial%8)];auto& b=fx.dynamicEq.bands[std::size_t(trial%8)];
            b.shape=DynamicEqShape(trial%6);b.frequencyHz=trial%2?20.f:20000.f;b.q=trial%2?18.f:.25f;
            b.rangeDb=trial%2?-6.f:6.f;b.bypassed=trial%7==0;fx.bypassed=trial%11==0;
            const auto before=engine.patch();auto invalid=patch;invalid.effects[7].dynamicEq.bands[7].q=std::numeric_limits<float>::quiet_NaN();
#if defined(_MSC_VER) && defined(_DEBUG)
            Heap::active()=true;
#endif
            preserved=preserved&&!engine.setPatch(invalid)&&engine.patch()==before;
            admitted=engine.setPatch(patch)&&engine.setPatch(patch)&&admitted;
            if(trial==12)engine.reset();engine.render(view,midi,120.);
            const auto tail=dynamicEqTailSeconds(fx.dynamicEq,rate*(quality==RenderQuality::higher?2:1));finite=finite&&std::isfinite(tail);
            for(int ch=0;ch<2;++ch)for(int n=0;n<block;++n){const auto x=view.getSample(ch,n);finite=finite&&std::isfinite(x);peak=std::max(peak,std::abs(double(x)));}
#if defined(_MSC_VER) && defined(_DEBUG)
            Heap::active()=false;
#endif
        }
        CHECK(admitted);CHECK(finite);CHECK(preserved);
#if defined(_MSC_VER) && defined(_DEBUG)
        std::cout<<"ROUTED_ALLOC rate="<<rate<<" quality="<<int(quality)<<" count="<<Heap::count()<<" peak="<<peak<<'\n';CHECK(Heap::count()==0);
#endif
        CHECK(engine.prepare(rate==48000?96000:48000,512,quality));CHECK(engine.setPatch(patch));engine.reset();
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previousHook);
#endif
    CHECK(dynamicEqTailSeconds({},48000)==0);CHECK(std::isnan(dynamicEqTailSeconds({},0)));
    auto setting=DynamicEqSettings{};setting.bands[0].active=true;setting.bands[0].gainDb=24;setting.bands[0].frequencyHz=20;setting.bands[0].q=18;
    DynamicEq core;CHECK(core.setSettings(setting));CHECK(core.prepare(48000));CHECK(core.tailSeconds()==dynamicEqTailSeconds(setting,48000));
}
}
void spectralPreparation()
{
    std::cout<<"SECTION prepared_spectral_mode_and_routed_latency\n";
    EqPreparationModes modes;
    RoutedSynthEngine engine;
    CHECK(engine.prepare(48000,64,RenderQuality::realtime,modes));
    CHECK(engine.latencySamples()==0);
    modes.slots[0]=DynamicEqProcessingMode::spectral;
    CHECK(engine.prepare(48000,64,RenderQuality::realtime,modes));
    CHECK(engine.latencySamples()==0); // An unused prepared slot has no audible delay.
    RoutedPatch patch;
    RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=0;edit.effect=EffectKind::dynamicEq;edit.destination=masterChain;
    CHECK(editRackLayout(patch.layout,edit,patch.layout)==RackEditResult::applied);
    patch.effects[0].dynamicEq.processingMode=DynamicEqProcessingMode::spectral;
    RoutedSynthEngine routed;CHECK(routed.setPatch(patch));
    CHECK(routed.prepare(48000,64,RenderQuality::realtime,modes));
    CHECK(routed.latencySamples()==5120);
    CHECK(routed.effectiveEqMode(0)==DynamicEqProcessingMode::spectral);
    auto bypass=patch;bypass.effects[0].bypassed=true;CHECK(routed.setPatch(bypass));
    CHECK(routed.latencySamples()==5120);
    CHECK(!routed.setPatch(RoutedPatch{.layout=patch.layout}));
    patch.effects[0].dynamicEq.bands[0].active=true;patch.effects[0].dynamicEq.bands[0].spectralEnabled=true;
    FxMonitor monitor;RoutedSynthEngine audition;audition.attachFxMonitor(&monitor);
    CHECK(audition.setPatch(patch)&&audition.prepare(48000,64,RenderQuality::realtime,modes));
    const auto token=monitor.subscribe(0);CHECK(token);
    if(token){CHECK(monitor.requestAudition(*token,0));juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;audition.render(audio,midi);
        CHECK(monitor.requestedAudition(*token)==std::optional<std::size_t>(0));
    }
}
void spectralRoutedAudio()
{
    std::cout<<"SECTION spectral_serial_parallel_quality_and_bypass_alignment\n";
    for(const auto rate:{48000.,96000.})for(const auto quality:{RenderQuality::realtime,RenderQuality::higher})
    for(const int stages:{1,3})for(const bool bypass:{false,true})for(const bool withSpace:{false,true}){
        RoutedPatch patch;
        RackEdit layer;layer.kind=RackEditKind::addSynth;layer.slot=1;
        CHECK(editRackLayout(patch.layout,layer,patch.layout)==RackEditResult::applied);
        patch.synth.layers[0].level=.05f;patch.synth.layers[1].enabled=true;patch.synth.layers[1].level=.04f;
        if(withSpace){RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=7;edit.effect=EffectKind::spaceReverb;edit.destination=masterChain;
            CHECK(editRackLayout(patch.layout,edit,patch.layout)==RackEditResult::applied);patch.effects[7].bypassed=true;}
        auto spectral=patch;EqPreparationModes modes;
        for(int slot=0;slot<stages;++slot){
            RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=std::uint8_t(slot);edit.effect=EffectKind::dynamicEq;
            edit.destination=slot==0?0:masterChain;edit.position=spectral.layout.effectCount[edit.destination];
            CHECK(editRackLayout(spectral.layout,edit,spectral.layout)==RackEditResult::applied);
            spectral.effects[std::size_t(slot)].dynamicEq.processingMode=DynamicEqProcessingMode::spectral;
            spectral.effects[std::size_t(slot)].bypassed=bypass;modes.slots[std::size_t(slot)]=DynamicEqProcessingMode::spectral;
        }
        RoutedSynthEngine original,processed;
        CHECK(original.setPatch(patch)&&original.prepare(rate,512,quality));
        CHECK(processed.setPatch(spectral)&&processed.prepare(rate,512,quality,modes));
        const int delay=int(rate/48000)*5120*stages;
        CHECK(processed.latencySamples()==original.latencySamples()+delay);
        const int length=delay+8192;std::array<std::vector<float>,2> reference;
        for(auto& channel:reference)channel.resize(std::size_t(length));
        juce::AudioBuffer<float> raw(2,512),actual(2,512);juce::MidiBuffer midi;midi.ensureSize(128);
        double error=0;bool finite=true;
        for(int offset=0,part=0;offset<length;++part){
            const int count=std::min(std::array<int,4>{64,139,512,7}[std::size_t(part%4)],length-offset);
            juce::AudioBuffer<float> a(raw.getArrayOfWritePointers(),2,count),b(actual.getArrayOfWritePointers(),2,count);
            midi.clear();if(offset==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
            original.render(a,midi);processed.render(b,midi);
            for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n){
                reference[std::size_t(ch)][std::size_t(offset+n)]=a.getSample(ch,n);
                const double expected=offset+n>=delay?reference[std::size_t(ch)][std::size_t(offset+n-delay)]:0.;
                const auto value=b.getSample(ch,n);finite=finite&&std::isfinite(value);error=std::max(error,std::abs(value-expected));
            }
            offset+=count;
        }
        std::cout<<"SPECTRAL_ALIGNMENT rate="<<rate<<" quality="<<int(quality)<<" stages="<<stages<<" bypass="<<bypass<<" space="<<withSpace<<" delay="<<delay<<" error="<<error<<'\n';
        CHECK(finite);CHECK(error<=2.e-6);
        processed.reset();CHECK(processed.latencySamples()==original.latencySamples()+delay);
    }
}
int main(int argc,char** argv)
{
    if(argc==3&&std::string_view(argv[1])=="--capture-spectral-predecessor"){
        const juce::File directory(juce::String::fromUTF8(argv[2]));CHECK(directory.createDirectory());
        if(parameterCount!=2535)return 2;
        juce::Array<juce::var> descriptors;
        for(const auto& d:parameterRegistry()){
            auto* row=new juce::DynamicObject;row->setProperty("id",d.id);row->setProperty("name",d.name);row->setProperty("unit",d.unit);
            row->setProperty("kind",int(d.kind));row->setProperty("minimum",d.minimum);row->setProperty("maximum",d.maximum);
            row->setProperty("default",d.defaultValue);row->setProperty("skew",d.skew);row->setProperty("key",juce::int64(d.stateKey));
            juce::Array<juce::var> choices;for(const auto& choice:d.choices)choices.add(choice);row->setProperty("choices",choices);descriptors.add(row);
        }
        const auto save=[&](const juce::String& name,const juce::MemoryBlock& bytes){const auto file=directory.getChildFile(name);CHECK(!file.exists());CHECK(file.replaceWithData(bytes.getData(),bytes.getSize()));};
        const auto json=juce::JSON::toString(descriptors);juce::MemoryBlock catalog(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8()));save("registry-v23.json",catalog);
        SoundDocument sound;juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));CHECK(readInt(bytes,4)==23);save("default-v23.soundstate",bytes);
        sound.values[0]=.625f;sound.controlLocks.back()=true;sound.effectLocks[7]=true;
        CHECK(encodeSoundState(sound,bytes));save("rich-v23.soundstate",bytes);
        ProjectDocument project;project.sound=sound;CHECK(encodeProjectState(project,bytes));save("rich-v22.airackproject",bytes);
        std::cout<<"Spectral predecessor capture checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
    }
    if(argc==2&&std::string_view(argv[1])=="--spectral"){spectralPreparation();spectralRoutedAudio();std::cout<<"Spectral integration checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--sidechain"){externalDetectorRouting();externalDetectorLifetimes();std::cout<<"External detector routing checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--pipeline-fingerprint"){pipelineFingerprint();std::cout<<"Pipeline fingerprint checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--pipeline-counts"){pipelineEffectCounts();std::cout<<"Pipeline count checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    const bool audio=argc==2&&std::string_view(argv[1])=="--audio";
    if(argc==2&&std::string_view(argv[1])=="--routing-fingerprint"){routedSchedulingFingerprint();std::cout<<"Routing fingerprint checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc>2||(argc==2&&!audio))return 2;
    if(!audio){frozenPrefix();appendedGeometry();actualOldMigration();settingsAndLocks();malformedNewStates();}
    if(audio){routedAudio();externalDetectorRouting();externalDetectorLifetimes();actualOldAudio();preparedLifecycle();routedSchedulingFingerprint();pipelineFingerprint();pipelineEffectCounts();}
    std::cout<<"Dynamic EQ integration: "<<checks<<" checks, "<<failures<<" failures\n";return failures==0?0:1;
}
