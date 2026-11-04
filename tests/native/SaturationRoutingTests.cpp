#include "FrozenRackExamples.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "ProjectDocument.h"
#include "FxMonitor.h"
#include "DspPipeline.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack {struct SynthParallelTestAccess {
    static bool prepareLayers(SynthEngine& synth,double rate,int maximum,int midiScale){return synth.prepareInternal(rate,maximum,midiScale);}
};}

namespace {
using namespace airack;
std::size_t checks=0,failures=0;
void check(bool ok,const char* expression,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL:"<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
bool place(SoundAudioState& sound,std::uint8_t slot,std::uint8_t chain=masterChain){
    RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=slot;edit.effect=EffectKind::saturation;edit.destination=chain;edit.position=sound.layout.effectCount[chain];
    const auto result=editRackLayout(sound.layout,edit,sound.layout);CHECK(result==RackEditResult::applied);return result==RackEditResult::applied;
}
void engagedRouting(){
    SoundAudioState sound;CHECK(place(sound,7));sound.values[572+7*23]=0;
    RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));CHECK(!patch.effects[7].bypassed);
    RoutedSynthEngine engine;CHECK(engine.setPatch(patch));CHECK(engine.prepare(48000,64));
    const auto factories=airack::test::frozenFactoryRackDesigns();for(const auto index:{19u,20u}){const auto& design=factories[index];
    for(const auto& p:design.units[0].defaults){const auto i=rackParameterIndex(NativeModule::saturation,7,p.key);CHECK(i);if(i)sound.values[*i]=p.normalized;}
    sound.values[572+7*23]=0;CHECK(makeRoutedPatch(sound,patch));
    RoutedPatch bypass=patch;bypass.effects[7].bypassed=true;
    RoutedSynthEngine reference;CHECK(engine.setPatch(patch)&&reference.setPatch(bypass));CHECK(engine.prepare(48000,64)&&reference.prepare(48000,64));engine.reset();reference.reset();
    juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.ensureSize(256);double energy=0,difference=0;
    for(int first=0;first<2048;first+=64){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        engine.render(a,midi,120.);reference.render(b,midi,120.);
        for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){energy+=std::abs(double(b.getSample(ch,n)));difference=std::max(difference,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));}}
    CHECK(energy>1e-6);CHECK(difference>1e-6);std::cout<<"ENGAGED factory="<<design.name<<" energy="<<energy<<" maximum_difference="<<difference<<'\n';}
}
std::array<float,35> fields(const SaturationSettings& a){
    std::array<float,35> v{float(a.mode),a.inputTrimDb,a.outputTrimDb,a.mix,float(a.driveCompensation),a.lowCrossoverHz,a.highCrossoverHz,
        float(a.preLowCutEnabled),a.preLowCutHz,float(a.postHighCutEnabled),a.postHighCutHz};
    for(std::size_t lane=0;lane<4;++lane){const auto& l=a.lanes[lane];const auto n=11+6*lane;
        v[n]=float(l.bypassed);v[n+1]=float(l.character);v[n+2]=l.driveDb;v[n+3]=l.preTiltDb;v[n+4]=l.mix;v[n+5]=l.trimDb;}
    return v;
}
void mappingAndValidation(){
    const auto registry=parameterRegistry();
    for(std::uint8_t slot=0;slot<8;++slot){SoundDocument d;CHECK(place(d,slot));
        for(std::size_t field=0;field<35;++field){const auto i=saturationParameterStart+slot*35+field;const auto& descriptor=registry[i];
            CHECK(parameterIndexForKey(descriptor.stateKey)==i);
            const auto local=juce::String(descriptor.id).fromFirstOccurrenceOf(".",false,false).fromFirstOccurrenceOf(".",false,false);
            CHECK(rackParameterIndex(NativeModule::saturation,slot,local)==i);
            d.values[i]=descriptor.choices.empty()?float((field*7+slot*3)%83+7)/100.f:float((field+slot)%descriptor.choices.size())/float(descriptor.choices.size()-1);}
        RoutedPatch p;CHECK(validateSoundDocument(d)&&makeRoutedPatch(d,p));
        auto assertMapped=[&](const ParameterValues& effective){const auto actual=fields(p.effects[slot].saturation);
            for(std::size_t field=0;field<35;++field)CHECK(actual[field]==parameterPhysicalValue(effective,saturationParameterStart+slot*35+field));};
        assertMapped(d.values);
        // Crossed corners are retained raw; only the DSP's effective view clamps them.
        d.values[saturationParameterStart+slot*35+5]=1;d.values[saturationParameterStart+slot*35+6]=0;
        CHECK(makeRoutedPatch(d,p));assertMapped(d.values);
        for(std::size_t field=0;field<35;++field){const auto i=saturationParameterStart+slot*35+field;
            if(isContinuousParameter(i)){d.macros[0]={registry[i].stateKey,.17f};d.values[1]=.6f;ParameterValues effective;CHECK(makeEffectiveParameterValues(d,effective));CHECK(makeRoutedPatch(d,p));assertMapped(effective);d.macros={};}
            auto bad=d;bad.values[i]=std::numeric_limits<float>::quiet_NaN();CHECK(!validateSoundDocument(bad)&&!makeRoutedPatch(bad,p));}
        auto absent=d;RackEdit remove;remove.kind=RackEditKind::removeEffect;remove.slot=slot;CHECK(editRackLayout(absent.layout,remove,absent.layout)==RackEditResult::applied);
        CHECK(makeRoutedPatch(absent,p));p.effects[slot].saturation.lanes[3].driveDb=std::numeric_limits<float>::quiet_NaN();CHECK(!validateRoutedPatch(p));
        absent.values[saturationParameterStart+slot*35+34]=std::numeric_limits<float>::infinity();CHECK(!makeRoutedPatch(absent,p));
        juce::MemoryBlock bytes;SoundDocument recalled;CHECK(encodeSoundState(d,bytes)&&decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==d);
        ProjectDocument project,loaded;project.sound=d;CHECK(encodeProjectState(project,bytes)&&decodeProjectState(bytes.getData(),bytes.getSize(),loaded)&&loaded==project);
    }
}
RoutedPatch flatPatch(){
    SoundAudioState sound;RoutedPatch p;CHECK(makeRoutedPatch(sound,p));p.synth.masterLevel=1;p.outputGainDb=0;p.outputProtection=false;
    auto& l=p.synth.layers[0];l.level=1;l.oscillators[0].waveform=Waveform::saw;l.oscillators[0].level=.7f;l.oscillators[1].level=0;
    l.cutoffHz=18000;l.resonance=0;l.ampEnvelope=l.filterEnvelope={0,0,.01f,1};return p;
}
void addEffect(RoutedPatch& p,std::uint8_t slot,EffectKind kind,std::uint8_t chain=masterChain){
    RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=slot;edit.effect=kind;edit.destination=chain;edit.position=p.layout.effectCount[chain];
    CHECK(editRackLayout(p.layout,edit,p.layout)==RackEditResult::applied);
}
struct DryPhase {
    std::array<double,5> direct{},stateDirect{};std::array<double,4> delayed{},stateDelayed{};unsigned nd=0,nl=0;double held=0;
    DryPhase(double width,double rejection){const auto c=juce::dsp::FilterDesign<double>::designIIRLowpassHalfBandPolyphaseAllpassMethod(width,rejection);
        nd=unsigned(c.directPath.size());nl=unsigned(c.delayedPath.size()-1);CHECK(nd<=5&&nl<=4);
        for(unsigned n=0;n<nd;++n)direct[n]=c.directPath.getObjectPointer(int(n))->coefficients[0];for(unsigned n=0;n<nl;++n)delayed[n]=c.delayedPath.getObjectPointer(int(n+1))->coefficients[0];}
    template<std::size_t N>static double allpass(double x,const std::array<double,N>& coefficients,std::array<double,N>& state,unsigned count){for(unsigned n=0;n<count;++n){const double y=coefficients[n]*x+state[n];state[n]=x-coefficients[n]*y;x=y;}return x;}
    std::array<double,2> up(double x){return {allpass(x,direct,stateDirect,nd),allpass(x,delayed,stateDelayed,nl)};}
    double down(double even,double odd){const double result=.5*(held+allpass(even,direct,stateDirect,nd));held=allpass(odd,delayed,stateDelayed,nl);return result;}
};
struct MatchedDry {
    std::array<DryPhase,2> up1{{{.045,-110},{.045,-110}}},up2{{{.12,-100},{.12,-100}}},down1{{{.045,-110},{.045,-110}}},down2{{{.12,-100},{.12,-100}}};
    Saturation::Frame previous{};
    Saturation::Frame process(Saturation::Frame input){Saturation::Frame result{};
        for(unsigned ch=0;ch<2;++ch){const auto first=up1[ch].up(input[ch]),a=up2[ch].up(first[0]),b=up2[ch].up(first[1]);std::array<double,4> high{a[0],a[1],b[0],b[1]};
            for(auto& x:high){const double next=x;x=.5*(x+previous[ch]);previous[ch]=next;}const auto even=down2[ch].down(high[0],high[1]),odd=down2[ch].down(high[2],high[3]);result[ch]=down1[ch].down(even,odd);}return result;}
};
// Independent composition of the public synth layer stream, saturation and the
// existing Higher FIR. No routed saturation dispatch is used in this reference.
struct Reference {
    SynthEngine synth;Saturation saturation;std::array<juce::AudioBuffer<float>,4> layers;
    std::array<juce::AudioBuffer<float>*,4> pointers{};std::vector<float> gains;
    std::unique_ptr<juce::dsp::Oversampling<float>> fir;std::unique_ptr<MatchedDry> dry;std::array<float,2> last{};bool delay=false;int factor=1;
    bool prepare(double rate,RenderQuality quality){factor=quality==RenderQuality::higher?2:1;
        if(!SynthParallelTestAccess::prepareLayers(synth,rate*factor,1024,factor)||!saturation.prepare(rate*factor))return false;
        gains.resize(1024);for(std::size_t i=0;i<4;++i){layers[i].setSize(2,1024);pointers[i]=&layers[i];}
        if(factor==2){fir=std::make_unique<juce::dsp::Oversampling<float>>(2);fir->clearOversamplingStages();fir->addOversamplingStage(juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,.08f,-100.f,.08f,-100.f);fir->initProcessing(512);const auto latency=fir->getLatencyInSamples()*.5f;delay=latency!=std::floor(latency);}return true;
    }
    void reset(const RoutedPatch& p,std::uint8_t bank){CHECK(synth.setPatch(p.synth));synth.reset();CHECK(saturation.setSettings(p.effects[7].saturation));saturation.reset();dry=std::make_unique<MatchedDry>();if(fir)fir->reset();last={};(void)bank;}
    void render(juce::AudioBuffer<float>& output,const juce::MidiBuffer& midi,std::uint8_t bank,bool bypass=false,bool matched=false){
        const int count=output.getNumSamples()*factor;for(auto& layer:layers)layer.setSize(2,count,false,false,true);
        synth.renderLayers(pointers,std::span(gains).first(std::size_t(count)),midi,120.);
        auto host=juce::dsp::AudioBlock<float>(output);juce::dsp::AudioBlock<float> high;
        if(fir)high=fir->processSamplesUp(host);
        for(int n=0;n<count;++n){Saturation::Frame frame{layers[bank].getSample(0,n),layers[bank].getSample(1,n)};if(matched)frame=dry->process(frame);else if(!bypass)frame=saturation.process(frame);
            for(int ch=0;ch<2;++ch){float value=float(frame[std::size_t(ch)]*gains[std::size_t(n)]);if(fir){if(delay)std::swap(last[std::size_t(ch)],value);high.setSample(ch,n,value);}else output.setSample(ch,n,value);}}
        if(fir)fir->processSamplesDown(host);
    }
};
void standaloneMatrix(){
    unsigned cases=0;double maximum=0;
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher})for(const double rate:{8000.,48000.,96000.,384000.}){
        RoutedSynthEngine engine;Reference reference;const bool ready=engine.prepare(rate,512,quality)&&reference.prepare(rate,quality);CHECK(ready);if(!ready)continue;
        for(std::uint8_t slot=0;slot<8;++slot)for(std::uint8_t chain=0;chain<=masterChain;++chain){auto p=flatPatch();
            const auto bank=chain==masterChain?std::uint8_t(0):chain;
            while(p.layout.synthCount<=bank){RackEdit edit;edit.kind=RackEditKind::addSynth;edit.position=p.layout.synthCount;CHECK(editRackLayout(p.layout,edit,p.layout)==RackEditResult::applied);}
            for(std::size_t b=0;b<4;++b){p.synth.layers[b]=p.synth.layers[0];p.synth.layers[b].enabled=b==bank;}
            addEffect(p,slot,EffectKind::saturation,chain);auto& sat=p.effects[slot].saturation;sat.mode=(slot%2)?SaturationMode::threeBand:SaturationMode::fullBand;
            sat.inputTrimDb=2;sat.mix=.87f;sat.preLowCutEnabled=true;sat.preLowCutHz=31;sat.postHighCutEnabled=true;sat.postHighCutHz=15000;
            for(std::size_t lane=0;lane<4;++lane){sat.lanes[lane].character=SaturationCharacter((slot+lane)%3);sat.lanes[lane].driveDb=9+float(lane)*3;sat.lanes[lane].mix=.72f;}
            CHECK(engine.setPatch(p));engine.reset();auto refPatch=p;refPatch.effects[7].saturation=sat;reference.reset(refPatch,bank);
            juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.ensureSize(256);double energy=0,error=0;
            for(int first=0;first<256;first+=64){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);engine.render(a,midi,120.);reference.render(b,midi,bank);
                for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){CHECK(std::isfinite(a.getSample(ch,n)));energy+=std::abs(double(b.getSample(ch,n)));error=std::max(error,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));}}
            CHECK(energy>1e-6);CHECK(error<1e-6);if(error>=1e-6)std::cout<<"REFERENCE_MISMATCH slot="<<int(slot)<<" chain="<<int(chain)<<" quality="<<int(quality)<<" rate="<<rate<<" error="<<error<<'\n';maximum=std::max(maximum,error);++cases;
        }
    }
    std::cout<<"STANDALONE_MATRIX cases="<<cases<<" maximum_error="<<maximum<<" effective_rate_max=768000\n";
}
void matchedDryRouting(){
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){auto p=flatPatch();addEffect(p,7,EffectKind::saturation);auto& s=p.effects[7].saturation;s.mix=0;s.preLowCutEnabled=s.postHighCutEnabled=true;s.preLowCutHz=600;s.postHighCutHz=1200;s.lanes[0].driveDb=36;s.lanes[0].character=SaturationCharacter::glow;
        RoutedSynthEngine actual,raw;Reference independent;CHECK(actual.setPatch(p)&&actual.prepare(48000,512,quality));auto bypass=p;bypass.effects[7].bypassed=true;CHECK(raw.setPatch(bypass)&&raw.prepare(48000,512,quality));CHECK(independent.prepare(48000,quality));independent.reset(p,0);
        juce::AudioBuffer<float> a(2,64),b(2,64),c(2,64);juce::MidiBuffer midi;midi.ensureSize(256);double maximum=0,rawDifference=0;
        for(int first=0;first<2048;first+=64){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);actual.render(a,midi,120.);raw.render(c,midi,120.);independent.render(b,midi,0,false,true);
            for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){maximum=std::max(maximum,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));rawDifference=std::max(rawDifference,std::abs(double(a.getSample(ch,n))-c.getSample(ch,n)));}}
        CHECK(maximum<1e-6);CHECK(rawDifference>1e-6);std::cout<<"MATCHED_DRY quality="<<int(quality)<<" independent_error="<<maximum<<" raw_difference="<<rawDifference<<'\n';
    }
}
void auditionRouting(){
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher})for(const std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)})
        for(const std::uint8_t chain:{std::uint8_t(0),masterChain}){
        auto patch=flatPatch();addEffect(patch,slot,EffectKind::saturation,chain);auto& sat=patch.effects[slot].saturation;
        sat.mode=SaturationMode::threeBand;sat.mix=0;sat.inputTrimDb=-2;sat.outputTrimDb=3;
        sat.preLowCutEnabled=sat.postHighCutEnabled=true;sat.preLowCutHz=40;sat.postHighCutHz=3000;
        for(unsigned band=1;band<=3;++band){sat.lanes[band].character=SaturationCharacter(band%3);sat.lanes[band].driveDb=6.f*band;sat.lanes[band].mix=.7f;}
        FxMonitor monitor;RoutedSynthEngine actual,normal;Reference reference;
        actual.attachFxMonitor(&monitor);CHECK(actual.setPatch(patch)&&normal.setPatch(patch));
        CHECK(actual.prepare(48000,512,quality)&&normal.prepare(48000,512,quality)&&reference.prepare(48000,quality));
        auto refPatch=patch;refPatch.effects[7].saturation=sat;reference.reset(refPatch,0);
        const auto token=monitor.acquireSaturationAudition(slot);CHECK(token);if(!token)continue;
        CHECK(monitor.requestSaturationAudition(*token,SaturationLane::mid));CHECK(reference.saturation.setAudition(SaturationLane::mid));
        juce::AudioBuffer<float> a(2,64),b(2,64),c(2,64);juce::MidiBuffer midi;midi.ensureSize(256);
        double maximum=0,energy=0,differenceFromNormal=0;
        for(int block=0;block<32;++block){midi.clear();if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
            actual.render(a,midi);normal.render(c,midi);reference.render(b,midi,0);
            for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){maximum=std::max(maximum,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));energy+=std::abs(b.getSample(ch,n));differenceFromNormal=std::max(differenceFromNormal,std::abs(double(a.getSample(ch,n))-c.getSample(ch,n)));}}
        CHECK(energy>1e-6);CHECK(maximum<1e-6);CHECK(differenceFromNormal>1e-6);
        CHECK(monitor.retireSaturationAudition(*token));CHECK(!monitor.requestSaturationAudition(*token,SaturationLane::low));reference.saturation.cancelAudition();
        for(int block=0;block<18;++block){actual.render(a,midi);normal.render(c,midi);reference.render(b,midi,0);}
        double restored=0;for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)restored=std::max(restored,std::abs(double(a.getSample(ch,n))-c.getSample(ch,n)));
        CHECK(restored==0);CHECK(monitor.stats(slot).published==0&&monitor.stats(slot).capturedFrames==0);
        std::cout<<"AUDITION_ROUTING quality="<<int(quality)<<" slot="<<int(slot)<<" chain="<<int(chain)<<" selected_energy="<<energy<<" oracle_error="<<maximum<<" normal_difference="<<differenceFromNormal<<" restored="<<restored<<'\n';
        for(int event=0;event<4;++event){const auto old=monitor.acquireSaturationAudition(slot);CHECK(old);if(!old)continue;CHECK(monitor.requestSaturationAudition(*old,SaturationLane::low));
            if(event==0){patch.effects[slot].bypassed=true;CHECK(actual.setPatch(patch));actual.render(a,midi);patch.effects[slot].bypassed=false;CHECK(actual.setPatch(patch));}
            if(event==1){patch.effects[slot].saturation.mode=SaturationMode::fullBand;CHECK(actual.setPatch(patch));actual.render(a,midi);patch.effects[slot].saturation.mode=SaturationMode::threeBand;CHECK(actual.setPatch(patch));}
            if(event==2){actual.attachFxMonitor(nullptr);actual.attachFxMonitor(&monitor);}
            if(event==3)CHECK(actual.prepare(96000,512,quality));
            CHECK(!monitor.requestSaturationAudition(*old,SaturationLane::mid));
        }
    }
}
using Audio=std::array<std::vector<float>,2>;
Audio stream(RoutedSynthEngine& engine,int partition,int count=1536){
    Audio result;for(auto& ch:result)ch.resize(std::size_t(count));
    juce::AudioBuffer<float> buffer(2,partition);juce::MidiBuffer midi;midi.ensureSize(256);
    for(int first=0;first<count;first+=partition){const int length=std::min(partition,count-first);buffer.setSize(2,length,false,false,true);midi.clear();
        if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);engine.render(buffer,midi,120.);
        for(int ch=0;ch<2;++ch)for(int n=0;n<length;++n){const auto value=buffer.getSample(ch,n);CHECK(std::isfinite(value));result[std::size_t(ch)][std::size_t(first+n)]=value;}}
    return result;
}
double difference(const Audio& a,const Audio& b){double result=0;for(std::size_t ch=0;ch<2;++ch)for(std::size_t n=0;n<a[ch].size();++n)result=std::max(result,std::abs(double(a[ch][n])-b[ch][n]));return result;}
void legacyDriveCoexistence(){
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        struct Fixture {RoutedPatch patch,withoutSaturation,bypassed;RoutedSynthEngine serial;};auto fixture=std::make_unique<Fixture>();auto& p=fixture->patch;
        p=flatPatch();p.compatibility=newInstanceKernelCompatibility();for(auto& layer:p.synth.layers)layer.algorithmRevision=modernSynthAlgorithmRevision;
        addEffect(p,0,EffectKind::spaceReverb);p.effects[0].spaceReverb.mix=.2f;
        addEffect(p,1,EffectKind::dynamicEq);p.effects[1].dynamicEq.bands[0].active=true;p.effects[1].dynamicEq.bands[0].gainDb=3;
        addEffect(p,2,EffectKind::drive);p.effects[2].algorithmRevision=legacyAlgorithmRevision;p.compatibility.effects[2][driveEffectBank]=legacyAlgorithmRevision;
        p.effects[2].drive.gainDb=12;p.effects[2].drive.mix=.65f;
        for(std::uint8_t slot=3;slot<8;++slot){addEffect(p,slot,EffectKind::saturation);auto& s=p.effects[slot].saturation;s.mix=.4f;s.lanes[0].driveDb=12;}
        CHECK(p.layout.effectCount[masterChain]==8&&p.layout.effects[2].kind==EffectKind::drive&&p.effects[2].algorithmRevision==legacyAlgorithmRevision);
        CHECK(fixture->serial.setPatch(p)&&fixture->serial.prepare(48000,512,quality));const auto expected=stream(fixture->serial,31);CHECK(fixture->serial.dspWorkerStats().blocks==0);
        double energy=0;for(const auto& channel:expected)for(const auto value:channel)energy+=std::abs(value);CHECK(energy>1e-6);
        for(const int partition:{64,73,512}){auto workers=std::make_unique<RoutedSynthEngine>();CHECK(workers->setPatch(p)&&workers->prepare(48000,512,quality));const auto actual=stream(*workers,partition);const auto stats=workers->dspWorkerStats();
            CHECK(stats.prepared==3&&stats.blocks>0);const auto error=difference(actual,expected);CHECK(error<1e-6);
            std::cout<<"LEGACY_DRIVE_WORKERS quality="<<int(quality)<<" partition="<<partition<<" energy="<<energy<<" blocks="<<stats.blocks<<" maximum_error="<<error<<'\n';}
        fixture->withoutSaturation=p;fixture->bypassed=p;
        for(std::uint8_t slot=3;slot<8;++slot){RackEdit remove;remove.kind=RackEditKind::removeEffect;remove.slot=slot;CHECK(editRackLayout(fixture->withoutSaturation.layout,remove,fixture->withoutSaturation.layout)==RackEditResult::applied);fixture->bypassed.effects[slot].bypassed=true;}
        auto absent=std::make_unique<RoutedSynthEngine>(),bypass=std::make_unique<RoutedSynthEngine>();CHECK(absent->setPatch(fixture->withoutSaturation)&&absent->prepare(48000,512,quality));CHECK(bypass->setPatch(fixture->bypassed)&&bypass->prepare(48000,512,quality));
        const auto raw=stream(*absent,73),bypassed=stream(*bypass,73);const auto fidelity=difference(raw,bypassed),audible=difference(expected,raw);CHECK(fidelity==0);CHECK(audible>1e-6);
        std::cout<<"LEGACY_DRIVE_FIDELITY quality="<<int(quality)<<" bypass_absence_error="<<fidelity<<" saturation_difference="<<audible<<'\n';
    }
}
void completeChainsAndWorkers(){
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher})for(const auto mode:{SaturationMode::fullBand,SaturationMode::threeBand}){
        auto p=flatPatch();for(std::uint8_t slot=0;slot<8;++slot){addEffect(p,slot,EffectKind::saturation);auto& s=p.effects[slot].saturation;s.mode=mode;
            s.mix=.35f;for(std::size_t l=0;l<4;++l){s.lanes[l].character=SaturationCharacter((slot+l)%3);s.lanes[l].driveDb=9+slot;}}
        CHECK(p.layout.effectCount[masterChain]==8);RoutedSynthEngine actual,raw;CHECK(actual.setPatch(p));auto dry=p;for(auto& e:dry.effects)e.bypassed=true;
        CHECK(raw.setPatch(dry));CHECK(actual.prepare(48000,512,quality)&&raw.prepare(48000,512,quality));const auto wetAudio=stream(actual,73);const auto dryAudio=stream(raw,73);CHECK(difference(wetAudio,dryAudio)>1e-6);
        auto reversed=p;std::reverse(reversed.layout.effectOrder[masterChain].begin(),reversed.layout.effectOrder[masterChain].end());CHECK(actual.setPatchQuiescent(reversed));actual.reset();CHECK(difference(wetAudio,stream(actual,73))>1e-6);
        RoutedSynthEngine absent;auto noFx=flatPatch();CHECK(absent.setPatch(noFx)&&absent.prepare(48000,512,quality));CHECK(difference(dryAudio,stream(absent,73))==0);
        std::cout<<"EIGHT_SLOT mode="<<int(mode)<<" quality="<<int(quality)<<" ordinary_worker_blocks="<<actual.dspWorkerStats().blocks<<'\n';
    }
    // Space presence selects the existing three-worker source path; saturation
    // stays on its ordinary serial FX path and does not change EQ scheduling.
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){auto p=flatPatch();p.compatibility=newInstanceKernelCompatibility();for(auto& layer:p.synth.layers)layer.algorithmRevision=modernSynthAlgorithmRevision;addEffect(p,0,EffectKind::spaceReverb);p.effects[0].spaceReverb.mix=.2f;
        addEffect(p,1,EffectKind::dynamicEq);p.effects[1].dynamicEq.bands[0].active=true;p.effects[1].dynamicEq.bands[0].gainDb=3;
        for(std::uint8_t slot=2;slot<8;++slot){addEffect(p,slot,EffectKind::saturation);p.effects[slot].saturation.mix=.4f;p.effects[slot].saturation.lanes[0].driveDb=12;}
        RoutedSynthEngine serial;CHECK(serial.setPatch(p)&&serial.prepare(48000,512,quality));const auto expected=stream(serial,31);CHECK(serial.dspWorkerStats().blocks==0);
        for(const int partition:{64,73,512}){RoutedSynthEngine workers;CHECK(workers.setPatch(p)&&workers.prepare(48000,512,quality));const auto audio=stream(workers,partition);const auto stats=workers.dspWorkerStats();
            CHECK(stats.prepared==3&&stats.blocks>0);const auto error=difference(audio,expected);CHECK(error<1e-6);std::cout<<"MIXED_WORKERS quality="<<int(quality)<<" partition="<<partition<<" blocks="<<stats.blocks<<" maximum_error="<<error<<'\n';}
    }
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking=false,refuseNext=false,refused=false,auditionCallbackCounting=false;std::size_t allocations=0,frees=0,allocatedBytes=0;
std::atomic<bool> workerTracking=false;std::atomic<std::size_t> workerAllocations=0,workerFrees=0,unmarkedAllocations=0,unmarkedFrees=0;
int __cdecl heapHook(int kind,void*,size_t bytes,int type,long,const unsigned char*,int){if(refuseNext&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)){refuseNext=false;refused=true;return 0;}if(workerTracking.load(std::memory_order_relaxed)&&type!=_CRT_BLOCK){const bool marked=auditionCallbackCounting||airack::detail::DspPipeline::renderingOnThisThread();if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++(marked?workerAllocations:unmarkedAllocations);if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++(marked?workerFrees:unmarkedFrees);}if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){++allocations;allocatedBytes+=bytes;}if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++frees;}return 1;}
#endif
void auditionWorkerHeap(){
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto patch=flatPatch();patch.compatibility=newInstanceKernelCompatibility();for(auto& layer:patch.synth.layers)layer.algorithmRevision=modernSynthAlgorithmRevision;
        addEffect(patch,0,EffectKind::spaceReverb);patch.effects[0].spaceReverb.mix=.2f;addEffect(patch,1,EffectKind::dynamicEq);
        for(std::uint8_t slot=2;slot<8;++slot){addEffect(patch,slot,EffectKind::saturation);auto& s=patch.effects[slot].saturation;s.mode=SaturationMode::threeBand;s.mix=0;s.lanes[2].driveDb=18;}
        FxMonitor monitor;RoutedSynthEngine actual;actual.attachFxMonitor(&monitor);const bool prepared=actual.setPatch(patch)&&actual.prepare(48000,512,quality);CHECK(prepared);if(!prepared)continue;
        const auto master=monitor.acquireSaturationAudition(2),second=monitor.acquireSaturationAudition(3);CHECK(master&&second);if(!master||!second)continue;
        CHECK(monitor.requestSaturationAudition(*master,SaturationLane::mid)&&monitor.requestSaturationAudition(*second,SaturationLane::high));
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        workerAllocations=workerFrees=unmarkedAllocations=unmarkedFrees=0;const auto hook=_CrtSetAllocHook(heapHook);workerTracking=true;auditionCallbackCounting=true;
#endif
        double energy=0;for(int block=0;block<16;++block){actual.render(audio,midi);midi.clear();energy+=audio.getMagnitude(0,64);}
        monitor.cancelSaturationAudition(2);monitor.cancelSaturationAudition(3);for(int block=0;block<16;++block)actual.render(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
        auditionCallbackCounting=false;workerTracking=false;_CrtSetAllocHook(hook);CHECK(workerAllocations.load()==0&&workerFrees.load()==0);
        std::cout<<"AUDITION_CALLBACK_AND_WORKERS quality="<<int(quality)<<" allocations="<<workerAllocations<<" frees="<<workerFrees<<'\n';
        std::cout<<"AUDITION_HEAP_CONTEXT quality="<<int(quality)<<" marked=callback_or_DspPipeline_worker marked_allocations="<<workerAllocations<<" marked_frees="<<workerFrees<<" unmarked_allocations="<<unmarkedAllocations<<" unmarked_frees="<<unmarkedFrees<<'\n';
#endif
        const auto workers=actual.dspWorkerStats();CHECK(workers.prepared==3&&workers.blocks>0);CHECK(energy>1e-9);CHECK(monitor.auditionTailSeconds()==0);CHECK(monitor.stats(2).published==0&&monitor.stats(3).published==0);
        std::cout<<"AUDITION_WORKER_OWNERS quality="<<int(quality)<<" workers="<<workers.prepared<<" blocks="<<workers.blocks<<" held_energy="<<energy<<" independent_capture=0\n";
    }
}
void callbackAndLifecycle(){
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){auto p=flatPatch();for(std::uint8_t slot=0;slot<8;++slot)addEffect(p,slot,EffectKind::saturation);
        RoutedSynthEngine engine;CHECK(engine.setPatch(p));
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations=frees=allocatedBytes=0;const auto hook=_CrtSetAllocHook(heapHook);tracking=true;
#endif
        const bool prepared=engine.prepare(48000,512,quality);
#if defined(_MSC_VER) && defined(_DEBUG)
        tracking=false;_CrtSetAllocHook(hook);std::cout<<"ROUTED_PREPARED quality="<<int(quality)<<" bytes="<<allocatedBytes<<" allocations="<<allocations<<'\n';
#endif
        CHECK(prepared);const auto latency=engine.latencySamples();juce::AudioBuffer<float> buffer(2,64);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        bool admitted=true,finite=true;
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations=frees=0;const auto callbackHook=_CrtSetAllocHook(heapHook);tracking=true;
#endif
        engine.render(buffer,midi,120.);midi.clear();
        for(unsigned edit=0;edit<12;++edit){for(auto& e:p.effects){e.bypassed=edit%3==1;auto& s=e.saturation;s.mode=edit%2?SaturationMode::threeBand:SaturationMode::fullBand;s.preLowCutEnabled=edit%2==0;s.postHighCutEnabled=edit%3==0;s.driveCompensation=edit%2==0;
                for(std::size_t lane=0;lane<4;++lane){auto& l=s.lanes[lane];l.character=SaturationCharacter((edit+lane)%3);l.bypassed=(edit+lane)%2==0;l.driveDb=float(edit)*2;l.preTiltDb=float(int(edit)%7)-3;}}
            admitted=engine.setPatch(p)&&admitted;engine.render(buffer,midi,120.);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)finite=finite&&std::isfinite(buffer.getSample(ch,n));if(edit==6)engine.reset();}
        engine.reset();
#if defined(_MSC_VER) && defined(_DEBUG)
        tracking=false;_CrtSetAllocHook(callbackHook);CHECK(allocations==0&&frees==0);std::cout<<"ROUTED_CALLBACK quality="<<int(quality)<<" allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
        CHECK(admitted&&finite);CHECK(engine.latencySamples()==latency);const auto before=engine.patch();CHECK(!engine.prepare(7999,64,quality)&&engine.patch()==before);CHECK(engine.latencySamples()==latency);
#if defined(_MSC_VER) && defined(_DEBUG)
        RoutedSynthEngine control;CHECK(control.setPatch(before)&&control.prepare(48000,512,quality));engine.reset();control.reset();juce::AudioBuffer<float> expected(2,64);
        midi.clear();midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);engine.render(buffer,midi,120.);control.render(expected,midi,120.);midi.clear();
        refused=false;const auto failureHook=_CrtSetAllocHook(heapHook);refuseNext=true;const bool replacement=engine.prepare(96000,64,quality);refuseNext=false;_CrtSetAllocHook(failureHook);
        CHECK(refused&&!replacement&&engine.patch()==before&&engine.latencySamples()==latency);engine.render(buffer,midi,120.);control.render(expected,midi,120.);
        for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)CHECK(std::bit_cast<std::uint32_t>(buffer.getSample(ch,n))==std::bit_cast<std::uint32_t>(expected.getSample(ch,n)));
        std::cout<<"ROUTED_PREPARE_FAILURE quality="<<int(quality)<<" actual_heap_refusal="<<refused<<" charged_prior_graph_exact=1\n";engine.reset();
#endif
        auto silence=stream(engine,64,128);double sum=0;for(const auto& ch:silence)for(auto x:ch)sum+=std::abs(x);CHECK(sum>0);
        engine.reset();midi.clear();engine.render(buffer,midi,120.);CHECK(buffer.getMagnitude(0,64)==0);
        CHECK(engine.prepare(96000,64,quality));CHECK(engine.patch()==before);engine.render(buffer,midi,120.);CHECK(buffer.getMagnitude(0,64)==0);
        midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);engine.render(buffer,midi,120.);midi.clear();midi.addEvent(juce::MidiMessage::midiStop(),0);engine.render(buffer,midi,120.);midi.clear();engine.render(buffer,midi,120.);CHECK(buffer.getMagnitude(0,64)==0);
    }
}
}
int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);if(argc==2&&std::string_view(argv[1])=="--audition"){auditionRouting();auditionWorkerHeap();}else if(argc==1){engagedRouting();mappingAndValidation();standaloneMatrix();matchedDryRouting();auditionRouting();completeChainsAndWorkers();legacyDriveCoexistence();auditionWorkerHeap();callbackAndLifecycle();}else return 2;std::cout<<"SATURATION_ROUTING checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
