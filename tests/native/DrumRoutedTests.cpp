#include "SoundDocument.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <thread>
#include <system_error>
#include <vector>
#include <type_traits>
#include <juce_dsp/juce_dsp.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
using namespace airack;
#ifndef NDEBUG
namespace airack {
#ifndef NDEBUG
struct RoutedDrumTestAccess {
    static bool prepareWithLaunch(RoutedSynthEngine& e,double r,int b,RenderQuality q,const EqPreparationModes& m,detail::DspPipeline::Launch launch){return e.prepareWithPipelineLaunch(r,b,q,m,launch);}
    static bool prepareWithSpaceCheckpoint(RoutedSynthEngine& e,double r,int b,RenderQuality q,const EqPreparationModes& m,const space::SpaceActivationOptions& o){return e.prepareWithPipelineLaunch(r,b,q,m,nullptr,o);}
    static auto snapshot(const RoutedSynthEngine& e){return e.drumTestSnapshot();}
};
#endif
}
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
using Audio=std::array<std::vector<float>,2>;
struct Event{int offset;juce::MidiMessage message;};
juce::MidiMessage on(int note=36,int velocity=127,int channel=1){return juce::MidiMessage::noteOn(channel,note,juce::uint8(velocity));}
drums::AssetPtr asset(std::vector<float> samples,unsigned channels=1){drums::AssetPtr out;drums::AssetError error;CHECK(drums::makeSampleAsset(samples,channels,out,error));return out;}
bool near(double a,double b,double epsilon=2e-6){return std::abs(a-b)<=epsilon;}
double energy(const Audio& a){double sum=0;for(const auto& c:a)for(auto v:c)sum+=std::abs(v);return sum;}
Audio render(RoutedSynthEngine& e,int frames,int block=512,const std::vector<Event>& events={}){
    Audio out;for(auto& c:out)c.resize(frames);
    for(int first=0;first<frames;first+=block){int count=std::min(block,frames-first);juce::AudioBuffer<float> audio(2,count);juce::MidiBuffer midi;
        for(const auto& event:events)if(event.offset>=first&&event.offset<first+count)midi.addEvent(event.message,event.offset-first);
        e.render(audio,midi,123.);for(int ch=0;ch<2;++ch)std::copy_n(audio.getReadPointer(ch),count,out[ch].data()+first);
    }return out;
}
RoutedPatch kit(unsigned banks=1){RoutedPatch p;p.synth.masterLevel=1;
    for(unsigned bank=0;bank<banks;++bank){if(bank)CHECK(editRackLayout(p.layout,{RackEditKind::addSynth,std::uint8_t(bank),masterChain,std::uint8_t(bank)},p.layout)==RackEditResult::applied);
        p.synth.layers[bank].enabled=true;p.synth.layers[bank].level=1;p.drumSource[bank]=true;
    }return p;
}
void add(RoutedPatch& p,EffectKind kind,unsigned chain=masterChain){unsigned slot=0;while(slot<8&&p.layout.effects[slot].use==SlotUse::present)++slot;CHECK(slot<8);if(slot<8)CHECK(editRackLayout(p.layout,{RackEditKind::addEffect,std::uint8_t(slot),std::uint8_t(chain),p.layout.effectCount[chain],kind},p.layout)==RackEditResult::applied);}
void prepare(RoutedSynthEngine& e,const RoutedPatch& p,const BorrowedDrumAssignments& a,double rate=48000,RenderQuality q=RenderQuality::realtime){CHECK(e.setPatch(p,a));CHECK(e.prepare(rate,512,q));e.reset();}
double modeEnergy(bool drum){
    SoundDocument d;d.values[drumModeParameterStart]=drum?1.f:0.f;
    RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));
    RoutedSynthEngine engine;CHECK(engine.setPatch(patch));CHECK(engine.prepare(48000,512));engine.reset();
    juce::AudioBuffer<float> output(2,512);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(127)),0);
    engine.render(output,midi);double sum=0;for(int c=0;c<2;++c)for(int i=0;i<512;++i)sum+=std::abs(output.getSample(c,i));return sum;
}
void unassignedSource(){
    static_assert(std::is_trivially_copyable_v<BorrowedDrumAssignments>);
    static_assert(sizeof(BorrowedDrumAssignments)==4*16*sizeof(const drums::SampleAsset*));
    BorrowedDrumAssignments unbraced;bool empty=true;for(const auto& bank:unbraced)for(auto* sample:bank)empty&=sample==nullptr;CHECK(empty);
    const auto melodic=modeEnergy(false);CHECK(melodic>0);
    const auto drum=modeEnergy(true);CHECK(drum==0); // silent-unassigned-Drum
    std::cout<<"melodic positive control energy="<<melodic<<" silent-unassigned-Drum energy="<<drum<<'\n';
}
void literalRouting(){
    std::cout<<"literal unity, independent gain laws, every physical bank and reordered sparse layout\n";
    auto mono=asset({.25f,-.5f,.75f,.125f});auto stereo=asset({.25f,-.5f,.75f,.125f,-.25f,.5f},2);
    for(unsigned bank=0;bank<4;++bank)for(bool two:{false,true})for(int velocity:{64,127}){
        auto p=kit(bank+1);for(unsigned other=0;other<bank;++other)CHECK(editRackLayout(p.layout,{RackEditKind::removeSynth,std::uint8_t(other)},p.layout)==RackEditResult::applied);
        BorrowedDrumAssignments bindings{};bindings[bank][0]=two?stereo.get():mono.get();RoutedSynthEngine e;prepare(e,p,bindings);
        auto y=render(e,12,512,{{3,on(36,velocity)}});
        const std::array<float,4> left{.25f,two?.75f:-.5f,two?-.25f:.75f,.125f};
        const std::array<float,3> right{-.5f,.125f,.5f};bool exact=true;
        for(int ch=0;ch<2;++ch)for(int n=0;n<12;++n){double expected=0;const int i=n-3;if(i>=0&&i<(two?3:4))expected=(two&&ch?right[i]:left[i])*velocity/127.;exact&=near(y[ch][n],expected);}
        CHECK(exact); // literal-drum-unity (1/32 mutant)
        p.synth.layers[bank].level=.75f;p.synth.layers[bank].pan=-.25f;p.drumPads[bank][0].gainDb=-6;p.drumPads[bank][0].pan=.5f;p.synth.masterLevel=.8f;p.outputGainDb=3;
        prepare(e,p,bindings);y=render(e,4,512,{{0,on(36,velocity)}});
        const double gain=std::pow(10.,-6./20)*.75*.8*std::pow(10.,3./20)*velocity/127;
        CHECK(near(y[0][0],.25*gain*.5));CHECK(near(y[1][0],(two?-.5:.25)*gain*.75));
    }
    auto p=kit(4);BorrowedDrumAssignments bindings{};for(unsigned bank=0;bank<4;++bank){bindings[bank][0]=mono.get();p.drumPads[bank][0].midiNote=36+int(bank);}
    CHECK(editRackLayout(p.layout,{RackEditKind::moveSynth,3,masterChain,0},p.layout)==RackEditResult::applied);RoutedSynthEngine e;prepare(e,p,bindings);
    auto y=render(e,8,512,{{0,on(36)},{1,on(39)}});CHECK(y[0]==std::vector<float>({.25f,-.25f,.25f,.875f,.125f,0,0,0}));CHECK(y[0]==y[1]);
    add(p,EffectKind::stereo,0);p.effects[0].stereo.gainDb=6;p.effects[0].stereo.pan=.5f;
    prepare(e,p,bindings);y=render(e,4,512,{{0,on(36)}});CHECK(near(y[0][0],.25*std::pow(10.,6./20)*.5));CHECK(near(y[1][0],.25*std::pow(10.,6./20)));
    auto repeatedMono=asset(std::vector<float>(500,.25f));std::vector<float> words(1000);for(int n=0;n<500;++n){words[n*2]=.5f;words[n*2+1]=.125f;}auto repeatedStereo=asset(words,2);
    auto duplicated=kit();duplicated.drumPads[0][0].chokeGroup=duplicated.drumPads[0][1].chokeGroup=1;duplicated.drumPads[0][1].midiNote=36;BorrowedDrumAssignments duplicateBindings;duplicateBindings[0][0]=repeatedMono.get();duplicateBindings[0][1]=repeatedStereo.get();RoutedSynthEngine duplicates;prepare(duplicates,duplicated,duplicateBindings);
    const auto hits=render(duplicates,512,17,{{0,on()},{1,on()}});bool duplicateExact=true;for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n){const double base=ch?.375:.75;const double expected=n==0?base:n<=240?base*(1+(241-n)/240.):n<=500?base:0.;duplicateExact&=near(hits[ch][n],expected,1e-7);}CHECK(duplicateExact);CHECK(RoutedDrumTestAccess::snapshot(duplicates).hits[0]==0);
}
void partitionsAndHigher(){
    std::cout<<"physical event/range matrix and accepted Higher FIR oracle\n";
    auto sample=asset({.25f,-.5f,.75f,.125f,-.25f,.5f,.375f,-.125f});auto p=kit();BorrowedDrumAssignments a{};a[0][0]=sample.get();
    const std::vector<Event> events{{0,on()},{15,on()},{16,on()},{511,on()},{512,on()},{1023,on(36,64)}};
    for(double rate:{44100.,48000.,96000.})for(auto q:{RenderQuality::realtime,RenderQuality::higher})for(int block:{1,7,64,512}){
        RoutedSynthEngine e,reference;prepare(e,p,a,rate,q);prepare(reference,p,a,rate,q);const auto actual=render(e,1536,block,events);const auto expected=render(reference,1536,512,events);CHECK(actual==expected); // range-event-history mutant
        if(rate==48000&&q==RenderQuality::realtime){CHECK(actual[0][0]==.25f);CHECK(actual[0][15]==.25f);CHECK(actual[0][511]==.25f);}
    }
    for(double rate:{44100.,48000.,96000.}){
        RoutedSynthEngine e;prepare(e,p,a,rate,RenderQuality::higher);const auto y=render(e,1536,512,{{17,on()},{511,on(36,64)}});
        juce::dsp::Oversampling<float> fir(2);fir.clearOversamplingStages();fir.addOversamplingStage(juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,.08f,-100.f,.08f,-100.f);fir.initProcessing(512);
        const auto latency=fir.getLatencyInSamples()*.5f;CHECK(e.latencySamples()==int(std::ceil(latency)));const bool delay=latency!=std::floor(latency);std::array<float,2> last{};
        drums::RackSettings settings;settings.pads[0].sample=sample.get();drums::DrumEngine reference;CHECK(reference.setSettings(settings));CHECK(reference.prepare(rate*2,1024));
        bool exact=true;for(int first=0;first<1536;first+=512){juce::AudioBuffer<float> host(2,512);host.clear();auto block=juce::dsp::AudioBlock<float>(host);auto high=fir.processSamplesUp(block);std::array<float*,2> ptr{high.getChannelPointer(0),high.getChannelPointer(1)};juce::AudioBuffer<float> view(ptr.data(),2,1024);juce::MidiBuffer midi;
            for(const auto& event:std::vector<Event>{{17,on()},{511,on(36,64)}})if(event.offset>=first&&event.offset<first+512)midi.addEvent(event.message,(event.offset-first)*2);
            reference.render(view,midi);if(delay)for(unsigned ch=0;ch<2;++ch)for(int n=0;n<1024;++n)std::swap(last[ch],ptr[ch][n]);fir.processSamplesDown(block);
            for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n)exact&=y[ch][first+n]==host.getSample(ch,n);
        }CHECK(exact); // Higher-outer-factor-once mutant
    }
    auto coordinate=asset(std::vector<float>(8,.25f));a[0][0]=coordinate.get();
    for(double rate:{44100.,48000.,96000.})for(float tune:{-24.f,12.f}){p.drumPads[0][0].tuneSemitones=tune;RoutedSynthEngine e;prepare(e,p,a,rate);const auto y=render(e,128,7,{{3,on()}});
        const double step=48000/rate*std::exp2(double(tune)/12);const int natural=int(std::ceil(8/step));bool coordinates=true;for(int ch=0;ch<2;++ch)for(int n=0;n<128;++n){const double expected=n>=3&&n<3+natural?.25:0.;coordinates&=near(y[ch][n],expected);}CHECK(coordinates);CHECK(y[0][3+natural-1]>0&&y[0][3+natural]==0);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);
    }
}
Audio transitionTimeline(int block,EngineKind engine=EngineKind::analog,AlgorithmRevision revision=legacyAlgorithmRevision,unsigned bank=0,bool coalesce=false,bool zeroOnset=true){
    const auto other=(bank+1)%4;auto marker=asset(std::vector<float>(1024,.25f));auto unrelated=asset(std::vector<float>(4096,.125f));auto p=kit(4);p.drumSource[bank]=false;p.synth.layers[bank].pan=-1;p.synth.layers[bank].engine=engine;p.synth.layers[bank].algorithmRevision=revision;p.compatibility.synth[bank][std::size_t(engine)]=revision;p.synth.layers[other].pan=1;
    BorrowedDrumAssignments a{};a[bank][0]=marker.get();a[other][0]=unrelated.get();p.drumPads[other][0].midiNote=37;add(p,EffectKind::delay,other);p.effects[0].delay.mix=.5f;p.effects[0].delay.seconds=.001f;p.effects[0].delay.feedback=.5f;
    RoutedSynthEngine e,reference;prepare(e,p,a);prepare(reference,p,a);Audio out;for(auto& c:out)c.resize(1400);
    const std::vector<Event> events{{0,on(37)},{10,on(48)},{zeroOnset?339:350,on()},{350,on(49)},{850,on(50)}};
    for(int first=0;first<1400;){if(first==100){p.drumSource[bank]=true;CHECK(e.setPatch(p,a));}if(first==600){p.drumSource[bank]=false;CHECK(e.setPatch(p));}
        if(coalesce&&first==180){p.drumSource[bank]=false;CHECK(e.setPatch(p));}if(coalesce&&first==200){p.drumSource[bank]=true;CHECK(e.setPatch(p));}
        int count=std::min(block,1400-first);for(int boundary:coalesce?std::vector<int>{100,180,200,600}:std::vector<int>{100,600})if(first<boundary)count=std::min(count,boundary-first);
        juce::AudioBuffer<float> audio(2,count);juce::MidiBuffer midi;for(const auto& event:events)if(event.offset>=first&&event.offset<first+count)midi.addEvent(event.message,event.offset-first);e.render(audio,midi);
        for(int ch=0;ch<2;++ch)std::copy_n(audio.getReadPointer(ch),count,out[ch].data()+first);first+=count;
    }
    const auto continued=render(reference,1400,block,events);bool rightExact=true;int firstMismatch=-1;for(int n=0;n<1400;++n)if(out[1][n]!=continued[1][n]){rightExact=false;if(firstMismatch<0)firstMismatch=n;}
    if(!rightExact)std::cerr<<"unrelated right first="<<firstMismatch<<" value="<<out[1][firstMismatch]<<" engine="<<int(engine)<<" revision="<<int(revision)<<'\n';
    CHECK(rightExact); // unrelated-bank-reset mutant
    if(zeroOnset){CHECK(out[0][339]==0);CHECK(out[0][340]>0);}else CHECK(out[0][350]>0); // source-zero-span mutant: boundary also precedes later onsets
    return out;
}
void transitions(){
    std::cout<<"inside-packet source zero, same-offset onset, unrelated bank continuity\n";
    for(unsigned bank=0;bank<4;++bank)for(auto engine:{EngineKind::analog,EngineKind::wavetable})for(auto revision:{legacyAlgorithmRevision,modernSynthAlgorithmRevision}){
        const auto expected=transitionTimeline(1,engine,revision,bank);for(int block:{7,64,512})CHECK(transitionTimeline(block,engine,revision,bank)==expected);CHECK(transitionTimeline(512,engine,revision,bank,true)==expected);
    }
    CHECK(transitionTimeline(512,EngineKind::analog,legacyAlgorithmRevision,0,false,false)==transitionTimeline(1,EngineKind::analog,legacyAlgorithmRevision,0,false,false));
}
void melodicContinuity(){
    std::cout<<"unrelated hard-right melodic voice/clocks/insert continuation and held-note-free Drum return\n";
    auto sample=asset(std::vector<float>(4096,.25f));
    for(unsigned bank=0;bank<4;++bank)for(int partition:{7,512}){const auto other=(bank+1)%4;auto p=kit(4);p.synth.layers[bank].pan=-1;p.drumSource[other]=false;p.synth.layers[other].engine=EngineKind::wavetable;p.synth.layers[other].algorithmRevision=modernSynthAlgorithmRevision;p.compatibility.synth[other][1]=modernSynthAlgorithmRevision;p.synth.layers[other].pan=1;
        add(p,EffectKind::delay,other);p.effects[0].delay.seconds=.001f;p.effects[0].delay.feedback=.5f;p.effects[0].delay.mix=.5f;BorrowedDrumAssignments a{};a[bank][0]=sample.get();RoutedSynthEngine e,reference;prepare(e,p,a);prepare(reference,p,a);
        bool same=true,heldSilent=true,newOnset=false;
        for(int first=0;first<2200;){if(first==100){p.drumSource[bank]=false;CHECK(e.setPatch(p));}if(first==1000){p.drumSource[bank]=true;CHECK(e.setPatch(p));}
            int count=std::min(partition,2200-first);for(int boundary:{100,1000})if(first<boundary)count=std::min(count,boundary-first);
            std::vector<Event> events;for(const auto& event:std::vector<Event>{{0,on()},{1500,juce::MidiMessage::noteOff(1,36)},{1900,on()}})if(event.offset>=first&&event.offset<first+count)events.push_back({event.offset-first,event.message});
            const auto actual=render(e,count,count,events),continued=render(reference,count,count,events);same&=actual[1]==continued[1];
            for(int n=0;n<count;++n){const int absolute=first+n;if(absolute>=600&&absolute<1900)heldSilent&=std::abs(actual[0][n])<1e-9;if(absolute==1900)newOnset=actual[0][n]>.2f;}first+=count;
        }CHECK(same);CHECK(heldSilent);CHECK(newOnset);
    }
}
void lastUsableAndDisable(){
    std::cout<<"asset replacement captures old hit; failed playable candidates preserve last usable; gate events\n";
    auto old=asset(std::vector<float>(4096,.25f));auto replacement=asset(std::vector<float>(4096,-.125f));auto p=kit();BorrowedDrumAssignments a{},b{};a[0][0]=old.get();b[0][0]=replacement.get();RoutedSynthEngine e,reference;prepare(e,p,a);prepare(reference,p,a);
    CHECK(render(e,32,32,{{0,on()}})==render(reference,32,32,{{0,on()}}));auto bad=p;bad.drumPads[0][0].trimStart=.75f;bad.drumPads[0][0].trimEnd=.25f;
    CHECK(!e.setPatch(bad,b));CHECK(e.patch()==p);CHECK(render(e,64,64,{{1,on()}})==render(reference,64,64,{{1,on()}})); // last-usable-assignment mutant
    auto invalidScalar=p;invalidScalar.drumPads[0][0].gainDb=std::numeric_limits<float>::quiet_NaN();CHECK(!e.setPatch(invalidScalar,b));CHECK(RoutedDrumTestAccess::snapshot(e).committedAssignments==a);
    CHECK(e.setPatch(p,b));auto y=render(e,8,8,{{0,on()}});CHECK(y[0][0]==.375f); // two old .25 captured hits plus new -.125
    auto snapshot=RoutedDrumTestAccess::snapshot(e);CHECK(snapshot.references[0][16]==old.get());CHECK(snapshot.committedAssignments==b);
    p.synth.layers[0].enabled=false;CHECK(e.setPatch(p));y=render(e,17,17,{{0,on()},{8,juce::MidiMessage::controllerEvent(1,123,0)}});CHECK(y[0][0]>.3f);for(int n=8;n<17;++n)CHECK(y[0][n]==0);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);
    CHECK(render(e,512,512,{{0,on()}})==Audio({std::vector<float>(512),std::vector<float>(512)}));
    p.synth.layers[0].enabled=true;CHECK(e.setPatch(p));(void)render(e,512);CHECK(energy(render(e,8,8,{{0,on()}}))>0);
    juce::AudioBuffer<float> invalid(1,8);juce::MidiBuffer midi;midi.addEvent(on(),0);const auto before=RoutedDrumTestAccess::snapshot(e);e.render(invalid,midi);CHECK(RoutedDrumTestAccess::snapshot(e).hits==before.hits);CHECK(RoutedDrumTestAccess::snapshot(e).sourceRemaining==before.sourceRemaining);
    CHECK(!e.prepare(0,512));CHECK(e.patch()==p);CHECK(e.setPatchQuiescent(p));CHECK(RoutedDrumTestAccess::snapshot(e).committedAssignments==b);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);
    auto dormant=p;dormant.drumSource[0]=false;dormant.drumPads[0][0].trimStart=.75f;dormant.drumPads[0][0].trimEnd=.25f;CHECK(e.setPatch(dormant));(void)render(e,512);
    auto unplayable=dormant;unplayable.drumSource[0]=true;CHECK(!e.setPatch(unplayable,a));CHECK(e.patch()==dormant&&RoutedDrumTestAccess::snapshot(e).committedAssignments==b);
    CHECK(e.setPatch(p));(void)render(e,512);CHECK(render(e,8,8,{{0,on()}})[0][0]==-.125f);
    for(int command:{120,123,252}){auto enabled=kit();RoutedSynthEngine stops;prepare(stops,enabled,a);(void)render(stops,8,8,{{0,on()}});enabled.synth.layers[0].enabled=false;CHECK(stops.setPatch(enabled));
        const auto stop=command==252?juce::MidiMessage::midiStop():juce::MidiMessage::controllerEvent(1,command,0);auto audio=render(stops,17,17,{{0,on()},{8,stop}});CHECK(audio[0][0]>0);CHECK(audio[0][8]==0&&audio[0][16]==0);CHECK(RoutedDrumTestAccess::snapshot(stops).hits[0]==0);
    }
    for(unsigned bank=0;bank<4;++bank)for(int elapsed:{80,239}){
        auto enabled=kit(4);BorrowedDrumAssignments bindings{};bindings[bank][0]=old.get();RoutedSynthEngine stops;prepare(stops,enabled,bindings);(void)render(stops,8,8,{{0,on()}});enabled.synth.layers[bank].enabled=false;CHECK(stops.setPatch(enabled));(void)render(stops,elapsed,7);
        CHECK(RoutedDrumTestAccess::snapshot(stops).gateRemaining[bank]==240-elapsed);enabled.drumSource[bank]=false;enabled.drumPads[bank][0].trimStart=.75f;enabled.drumPads[bank][0].trimEnd=.25f;CHECK(stops.setPatch(enabled));(void)render(stops,240-elapsed,7);
        const auto atGate=RoutedDrumTestAccess::snapshot(stops);CHECK(atGate.installed[bank]&&!atGate.target[bank]);CHECK(atGate.gateRemaining[bank]==0&&atGate.sourceRemaining[bank]==elapsed);CHECK(!atGate.configuredEnabled[bank]);CHECK(atGate.hits[bank]==0);CHECK(atGate.references[bank][0]==old.get());(void)render(stops,512);CHECK(!RoutedDrumTestAccess::snapshot(stops).installed[bank]);
    }
}
void layoutAndCommonGates(){
    std::cout<<"live source plus bank add/remove, common mute/solo and tail filtering\n";
    auto marker=asset(std::vector<float>(48000,.25f));auto unrelated=asset(std::vector<float>(48000,.125f));BorrowedDrumAssignments bindings{};bindings[0][0]=marker.get();bindings[1][0]=unrelated.get();
    const auto rightSame=[](const Audio& a,const Audio& b){return a[1]==b[1];};
    for(bool adding:{false,true}){
        auto a=kit(adding?2:3);a.drumSource[0]=false;a.synth.layers[0].pan=-1;a.synth.layers[1].pan=1;a.drumPads[1][0].midiNote=37;add(a,EffectKind::delay,1);a.effects[0].delay.mix=.5f;a.effects[0].delay.seconds=.001f;a.effects[0].delay.feedback=.5f;
        RoutedSynthEngine e,reference;prepare(e,a,bindings);prepare(reference,a,bindings);CHECK(render(e,100,100,{{0,on()},{0,on(37)}})==render(reference,100,100,{{0,on()},{0,on(37)}}));
        auto b=a;b.drumSource[0]=true;b.drumSource[2]=true;b.synth.layers[2].enabled=true;b.synth.layers[2].level=1;CHECK(editRackLayout(b.layout,{adding?RackEditKind::addSynth:RackEditKind::removeSynth,2,masterChain,2},b.layout)==RackEditResult::applied);auto layoutOnly=b;layoutOnly.drumSource[0]=false;CHECK(e.setPatch(b));CHECK(reference.setPatch(layoutOnly));CHECK(rightSame(render(e,239,7),render(reference,239,7)));
        const auto before=RoutedDrumTestAccess::snapshot(e);CHECK(!before.installed[0]&&before.target[0]&&before.sourceRemaining[0]==1);
        const auto zero=render(e,1,1,{{0,on()}});CHECK(rightSame(zero,render(reference,1,1,{{0,on()}})));CHECK(zero[0][0]==0);const auto installed=RoutedDrumTestAccess::snapshot(e);CHECK(installed.installed[0]&&installed.hits[0]==1&&installed.references[0][0]==marker.get());CHECK(installed.installed[2]==adding);
        const auto after=render(e,512,512,{{16,on()}});CHECK(rightSame(after,render(reference,512,512,{{16,on()}})));CHECK(after[0][300]>.25f);
        CHECK(e.setPatch(a));CHECK(reference.setPatch(a));const auto returning=render(e,512,7);CHECK(rightSame(returning,render(reference,512,7)));CHECK(returning[0].back()==0);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);CHECK(!RoutedDrumTestAccess::snapshot(e).installed[0]);
    }
    for(bool solo:{false,true}){
        auto p=kit(2);p.synth.layers[0].pan=-1;p.synth.layers[1].pan=1;p.drumPads[1][0].midiNote=37;add(p,EffectKind::delay,1);p.effects[0].delay.mix=.5f;p.effects[0].delay.seconds=.001f;p.effects[0].delay.feedback=.5f;
        RoutedSynthEngine e,reference;prepare(e,p,bindings);prepare(reference,p,bindings);CHECK(render(e,100,100,{{0,on()},{0,on(37)}})==render(reference,100,100,{{0,on()},{0,on(37)}}));auto gated=p;if(solo)gated.solo[1]=true;else gated.mute[0]=true;CHECK(e.setPatch(gated));
        const auto down=render(e,240,7);CHECK(rightSame(down,render(reference,240,7)));CHECK(near(down[0][0],.25*239/240.));CHECK(down[0][239]==0);const auto muted=RoutedDrumTestAccess::snapshot(e);CHECK(muted.hits[0]==1&&muted.configuredEnabled[0]);CHECK(e.sourceTailSeconds()[0]==1);const auto quiet=render(e,512);CHECK(rightSame(quiet,render(reference,512)));CHECK(energy({quiet[0],std::vector<float>(512)})==0);
        CHECK(e.setPatch(p));const auto up=render(e,512,7,{{0,on()}});CHECK(rightSame(up,render(reference,512,7,{{0,on()}})));CHECK(up[0][300]==.5f);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==2);
        p.synth.layers[0].enabled=false;CHECK(e.setPatch(p));(void)render(e,240,7);CHECK(e.sourceTailSeconds()[0]==0);CHECK(e.sourceTailSeconds()[1]==1);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);
    }
}
struct Checkpoint {
    std::atomic<bool> entered{false},release{false},expired{false};
    static void wait(void* raw,std::uint64_t,space::detail::PreparationControl control) noexcept {
        auto& gate=*static_cast<Checkpoint*>(raw);gate.entered.store(true,std::memory_order_release);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!gate.release.load(std::memory_order_acquire)){
            if(control.isCancelled&&control.isCancelled(control.context))return;
            if(std::chrono::steady_clock::now()>deadline){gate.expired=true;return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    bool observe(){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));return entered.load();}
    ~Checkpoint(){release.store(true,std::memory_order_release);}
};
bool admit(RoutedSynthEngine& e,const RoutedPatch& p,const BorrowedDrumAssignments& a){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(std::chrono::steady_clock::now()<deadline){if(e.setPatch(p,a))return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return false;}
void lateSpace(){
    std::cout<<"real held Space checkpoint, late commit/cancel with coherent borrowed views\n";
    auto old=asset(std::vector<float>(480000,.25f));auto replacement=asset(std::vector<float>(480000,-.125f));auto right=asset(std::vector<float>(480000,.0625f));
    for(int family:{-1,0,1,2})for(bool pointerPreserving:{false,true})for(bool disable:{false,true}){
        auto a=kit(2);a.synth.layers[0].pan=-1;a.synth.layers[1].pan=1;a.drumPads[1][0].midiNote=37;add(a,EffectKind::delay,1);a.effects[0].delay.mix=.25f;a.effects[0].delay.seconds=.01f;
        auto b=a;add(b,EffectKind::spaceReverb);b.effects[1].spaceReverb.mix=0;b.synth.layers[0].level=.5f;b.synth.layers[0].enabled=!disable;b.drumPads[0][0].trimStart=.25f;b.drumPads[0][0].trimEnd=.75f;b.drumPads[0][0].tuneSemitones=12;
        if(family>=0){b.drumSource[0]=false;b.synth.layers[0].engine=family?EngineKind::wavetable:EngineKind::analog;const auto rev=family==2?modernSynthAlgorithmRevision:legacyAlgorithmRevision;b.synth.layers[0].algorithmRevision=rev;b.compatibility.synth[0][std::size_t(b.synth.layers[0].engine)]=rev;}
        auto control=b;control.drumSource=a.drumSource;control.drumPads=a.drumPads;control.synth=a.synth;control.compatibility=a.compatibility;
        BorrowedDrumAssignments bindings{},next{};bindings[0][0]=old.get();bindings[1][0]=right.get();next=bindings;next[0][0]=replacement.get();
        Checkpoint gate;RoutedSynthEngine e,reference;CHECK(e.setPatch(a,bindings));CHECK(RoutedDrumTestAccess::prepareWithSpaceCheckpoint(e,48000,512,RenderQuality::realtime,{}, {Checkpoint::wait,&gate}));prepare(reference,a,bindings);
        CHECK(render(e,64,64,{{0,on()},{0,on(37)}})==render(reference,64,64,{{0,on()},{0,on(37)}}));
        CHECK(!e.setPatch(b,next));CHECK(gate.observe());CHECK(e.patch()==a);auto before=RoutedDrumTestAccess::snapshot(e);CHECK(before.preparing&&before.pendingValid&&!before.installPending&&before.committedAssignments==bindings&&before.pendingAssignments==next);
        CHECK(render(e,17,17,{{0,on()}})==render(reference,17,17,{{0,on()}}));gate.release=true;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);bool accepted=false;
        while(std::chrono::steady_clock::now()<deadline){if(e.setPatch(b,next)){accepted=true;break;}CHECK(render(e,7,7)==render(reference,7,7));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        CHECK(accepted);CHECK(admit(reference,control,bindings));CHECK(e.patch()==b);auto staged=RoutedDrumTestAccess::snapshot(e);CHECK(staged.installPending&&staged.pendingValid&&staged.previousValid);CHECK(*staged.committed==a&&staged.committedAssignments==bindings&&staged.pendingAssignments==next&&staged.previousAssignments==bindings);CHECK(staged.installed[0]&&staged.target[0]);
        auto different=b;different.outputGainDb=1;auto changed=next;changed[0][1]=replacement.get();CHECK(!e.setPatch(different,changed));CHECK(RoutedDrumTestAccess::snapshot(e).pendingAssignments==next);CHECK(*RoutedDrumTestAccess::snapshot(e).pending==b);
        CHECK(e.setPatch(b,next));CHECK(render(e,17,17,{{0,on()}})==render(reference,17,17,{{0,on()}})); // staged disable still admits committed A onsets
        CHECK(RoutedDrumTestAccess::snapshot(e).committedAssignments==bindings);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]>=3);
        CHECK(pointerPreserving?e.setPatch(a):e.setPatch(a,bindings));CHECK(reference.setPatch(a,bindings));auto cancelled=RoutedDrumTestAccess::snapshot(e);
        CHECK(e.patch()==a&&*cancelled.committed==a&&cancelled.committedAssignments==bindings&&!cancelled.pendingValid&&!cancelled.previousValid&&!cancelled.installPending);CHECK(cancelled.target[0]);
        CHECK(render(e,512,64,{{0,on()}})==render(reference,512,64,{{0,on()}})); // actual-late-cancel-assignment/source-target mutant
        CHECK(!gate.expired.load());
    }
    {
        auto initial=kit(2);initial.synth.layers[0].pan=-1;initial.synth.layers[1].pan=1;initial.drumPads[1][0].midiNote=37;
        auto a=initial;a.drumSource[0]=false;auto b=a;add(b,EffectKind::spaceReverb);b.effects[0].spaceReverb.mix=0;b.drumSource[0]=true;
        auto control=b;control.drumSource=a.drumSource;BorrowedDrumAssignments bindings{},next{};bindings[0][0]=old.get();bindings[1][0]=right.get();next=bindings;next[0][0]=replacement.get();
        Checkpoint gate;RoutedSynthEngine e,reference;CHECK(e.setPatch(initial,bindings));CHECK(RoutedDrumTestAccess::prepareWithSpaceCheckpoint(e,48000,512,RenderQuality::realtime,{}, {Checkpoint::wait,&gate}));prepare(reference,initial,bindings);
        CHECK(render(e,8,8,{{0,on()},{0,on(37)}})==render(reference,8,8,{{0,on()},{0,on(37)}}));CHECK(e.setPatch(a)&&reference.setPatch(a));CHECK(RoutedDrumTestAccess::snapshot(e).sourceRemaining[0]==240);
        CHECK(!e.setPatch(b,next));CHECK(gate.observe());CHECK(render(e,17,17)==render(reference,17,17));CHECK(RoutedDrumTestAccess::snapshot(e).sourceRemaining[0]==223);CHECK(!RoutedDrumTestAccess::snapshot(e).target[0]);gate.release=true;
        bool accepted=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(std::chrono::steady_clock::now()<deadline){if(e.setPatch(b,next)){accepted=true;break;}CHECK(render(e,7,7)==render(reference,7,7));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        CHECK(accepted&&admit(reference,control,bindings));CHECK(!RoutedDrumTestAccess::snapshot(e).target[0]);CHECK(render(e,17,17)==render(reference,17,17));
        CHECK(e.setPatch(a)&&reference.setPatch(a));CHECK(!RoutedDrumTestAccess::snapshot(e).target[0]);CHECK(render(e,512,64,{{0,on()}})==render(reference,512,64,{{0,on()}}));CHECK(RoutedDrumTestAccess::snapshot(e).committedAssignments==bindings);
    }
    for(int operation:{0,1,2,3,4,5,6})for(bool melodic:{false,true}){
        auto a=kit();auto b=a;add(b,EffectKind::spaceReverb);b.effects[0].spaceReverb.mix=0;b.drumPads[0][0].tuneSemitones=12;b.synth.layers[0].level=.5f;b.drumSource[0]=!melodic;
        BorrowedDrumAssignments bindings{},next{};bindings[0][0]=old.get();next[0][0]=replacement.get();Checkpoint gate;RoutedSynthEngine e;
        CHECK(e.setPatch(a,bindings));CHECK(RoutedDrumTestAccess::prepareWithSpaceCheckpoint(e,48000,512,RenderQuality::realtime,{}, {Checkpoint::wait,&gate}));(void)render(e,8,8,{{0,on()}});
        CHECK(!e.setPatch(b,next));CHECK(gate.observe());
        if(operation==5){CHECK(!e.prepare(0,512));auto s=RoutedDrumTestAccess::snapshot(e);CHECK(*s.committed==a&&s.pendingValid&&s.preparing&&s.pendingAssignments==next&&s.hits[0]==1);CHECK(energy(render(e,17,17,{{0,on()}}))>0);e.reset();gate.release=true;CHECK(!RoutedDrumTestAccess::snapshot(e).pendingValid);continue;}
        if(operation==6){CHECK(e.prepare(48000,512));gate.release=true;auto s=RoutedDrumTestAccess::snapshot(e);CHECK(e.patch()==a&&s.committedAssignments==bindings&&!s.pendingValid&&!s.previousValid&&!s.installPending&&s.hits[0]==0);continue;}
        if(operation==4){e.reset();gate.release=true;auto s=RoutedDrumTestAccess::snapshot(e);CHECK(*s.committed==a&&!s.pendingValid&&!s.previousValid&&!s.installPending);continue;}
        gate.release=true;CHECK(admit(e,b,next));CHECK(RoutedDrumTestAccess::snapshot(e).installPending);
        if(operation==0){CHECK(!e.prepare(0,512));CHECK(RoutedDrumTestAccess::snapshot(e).pendingAssignments==next);const auto down=render(e,240,240);for(int n=0;n<240;++n)for(int ch=0;ch<2;++ch)CHECK(near(down[ch][n],.25*(1-(n+1)/240.),1e-7));auto s=RoutedDrumTestAccess::snapshot(e);CHECK(*s.committed==b&&s.committedAssignments==next&&!s.pendingValid&&!s.previousValid);CHECK(s.references[0][16]==old.get());
            if(melodic){(void)render(e,512);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);auto back=b;back.drumSource[0]=true;CHECK(e.setPatch(back));(void)render(e,512);auto y=render(e,8,8,{{0,on()}});CHECK(y[0][0]<0);}
            else{auto y=render(e,8,8,{{0,on()}});for(int n=0;n<8;++n)for(int ch=0;ch<2;++ch)CHECK(near(y[ch][n],(.25-.125)*(1-.5*(n+2)/240.)*(n+1)/240.,1e-7));} // common ramp starts at zero boundary; global up-fade starts one frame later
        }else if(operation==1){CHECK(e.prepare(48000,512));auto s=RoutedDrumTestAccess::snapshot(e);CHECK(e.patch()==a&&s.committedAssignments==bindings&&!s.pendingValid&&!s.previousValid);}
        else if(operation==2){e.reset();auto s=RoutedDrumTestAccess::snapshot(e);CHECK(*s.committed==b&&s.committedAssignments==next&&!s.pendingValid&&!s.previousValid&&s.hits[0]==0);}
        else{CHECK(e.setPatchQuiescent(b,next));auto s=RoutedDrumTestAccess::snapshot(e);CHECK(e.patch()==b&&s.committedAssignments==next&&!s.pendingValid&&!s.previousValid&&s.hits[0]==0);if(!melodic)CHECK(render(e,8,8,{{0,on()}})[0][0]==-.0625f);}
    }
}
std::atomic<int> live{0},exited{0};unsigned launches=0,failAt=0;
std::thread failingLaunch(detail::DspPipeline::Entry entry,detail::DspPipeline* pipeline,unsigned worker){
    if(launches++==failAt)throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    return std::thread([=]{++live;entry(pipeline,worker);--live;++exited;});
}
void workerPaths(){
    std::cout<<"three workers, mixed source/EQ/Space packets and actual partial-start failures\n";
    auto sample=asset(std::vector<float>(4096,.25f));
    for(int route:{0,1,2})for(auto q:{RenderQuality::realtime,RenderQuality::higher}){
        auto p=kit(4);BorrowedDrumAssignments bindings{};for(unsigned bank=0;bank<4;++bank){bindings[bank][0]=sample.get();p.drumPads[bank][0].midiNote=36+int(bank);if(bank==2){p.drumSource[bank]=false;p.synth.layers[bank].algorithmRevision=modernSynthAlgorithmRevision;p.compatibility.synth[bank][0]=modernSynthAlgorithmRevision;}}
        if(route==0){add(p,EffectKind::dynamicEq);add(p,EffectKind::dynamicEq);}else{add(p,EffectKind::spaceReverb);p.effects[0].spaceReverb.mix=0;if(route==2)add(p,EffectKind::stereo,1);}
        RoutedSynthEngine normal;prepare(normal,p,bindings,48000,q);CHECK(normal.dspWorkerStats().prepared==3);
        const std::vector<Event> events{{0,on()},{16,on(37)},{511,on(38)},{512,on(39)}};const auto expected=render(normal,1536,512,events);CHECK(normal.dspWorkerStats().blocks>0);
        const auto completed=RoutedDrumTestAccess::snapshot(normal);for(unsigned stream=0;stream<completed.streams;++stream)CHECK(completed.ready[stream]>0);
        if(q==RenderQuality::realtime&&route<2)for(int extraKind:{0,1,2}){RoutedSynthEngine ignored,reference;prepare(ignored,p,bindings);prepare(reference,p,bindings);if(extraKind==2)CHECK(render(ignored,64,64,{{0,on(38)}})==render(reference,64,64,{{0,on(38)}}));const auto previousBlocks=ignored.dspWorkerStats().blocks;juce::AudioBuffer<float> actual(2,512),expectedAudio(2,512);juce::MidiBuffer valid,extra;valid.addEvent(on(),0);extra=valid;if(extraKind==2)extra.addEvent(juce::MidiMessage::noteOff(1,38),-1);else if(extraKind==1)extra.addEvent(on(38),-1);else extra.addEvent(juce::MidiMessage::midiClock(),0);ignored.render(actual,extra);reference.render(expectedAudio,valid);bool same=true;for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n)same&=actual.getSample(ch,n)==expectedAudio.getSample(ch,n);CHECK(same);CHECK(ignored.dspWorkerStats().prepared==3&&ignored.dspWorkerStats().blocks==previousBlocks);}
        for(unsigned failure=0;failure<3;++failure){failAt=failure;launches=0;exited=0;live=0;RoutedSynthEngine serial;CHECK(serial.setPatch(p,bindings));CHECK(RoutedDrumTestAccess::prepareWithLaunch(serial,48000,512,q,{},failingLaunch));CHECK(serial.dspWorkerStats().prepared==0);CHECK(live==0&&exited==int(failure));CHECK(launches==failure+1);CHECK(render(serial,1536,7,events)==expected);CHECK(serial.dspWorkerStats().blocks==0);}
        RoutedSynthEngine tiny;prepare(tiny,p,bindings,48000,q);(void)render(tiny,31,31,{{0,on()}});CHECK(tiny.dspWorkerStats().blocks==0);
    }
    auto p=kit(4);BorrowedDrumAssignments bindings{};for(auto& bank:bindings)bank[0]=sample.get();add(p,EffectKind::dynamicEq);add(p,EffectKind::dynamicEq);RoutedSynthEngine all;prepare(all,p,bindings);(void)render(all,512,512,{{0,on()}});CHECK(all.dspWorkerStats().blocks>0);
    p.drumSource[2]=false;RoutedSynthEngine legacy;prepare(legacy,p,bindings);(void)render(legacy,512,512,{{0,on()}});CHECK(legacy.dspWorkerStats().blocks==0);
}
void longTail(){
    std::cout<<"one bounded forty-second captured duration at 8kHz\n";
    auto sample=asset(std::vector<float>(480000,.25f));auto p=kit();p.drumPads[0][0].tuneSemitones=-24;BorrowedDrumAssignments a{};a[0][0]=sample.get();RoutedSynthEngine e;prepare(e,p,a,8000);
    CHECK(e.sourceTailSeconds()[0]==40);auto y=render(e,8,8,{{0,on()}});CHECK(y[0][0]==.25f);
    p.drumPads[0][0].trimEnd=.001f;p.drumPads[0][0].tuneSemitones=24;CHECK(e.setPatch(p));CHECK(e.sourceTailSeconds()[0]>39.99);
    y=render(e,319992);CHECK(y[0].back()==.25f);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);CHECK(e.sourceTailSeconds()[0]<.01);CHECK(energy(render(e,512))==0);
    auto no=a;no[0][0]=nullptr;CHECK(e.setPatch(p,no));CHECK(e.sourceTailSeconds()[0]==0);CHECK(energy(render(e,16,16,{{0,on()}}))==0);
    CHECK(e.setPatch(p,a));(void)render(e,8,8,{{0,on()}});p.drumSource[0]=false;CHECK(e.setPatch(p));(void)render(e,512);CHECK(e.sourceTailSeconds()[0]==0);CHECK(RoutedDrumTestAccess::snapshot(e).hits[0]==0);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool callbackOwner=false;std::atomic<unsigned> allocations{0},frees{0};
int heapHook(int kind,void*,std::size_t,int,long,const unsigned char*,int){if(callbackOwner||detail::DspPipeline::renderingOnThisThread()){
    if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE)++frees;}return 1;}
#endif
void realtimeHeap(){
    std::cout<<"callback and claimed worker CRT allocation/free guard\n";
    auto a=asset(std::vector<float>(4096,.25f));auto b=asset(std::vector<float>(4096,-.125f));auto p=kit(4);BorrowedDrumAssignments bindings{};for(auto& bank:bindings)bank[0]=a.get();add(p,EffectKind::dynamicEq);add(p,EffectKind::dynamicEq);RoutedSynthEngine e;prepare(e,p,bindings,48000,RenderQuality::higher);
    juce::AudioBuffer<float> output(2,512);juce::MidiBuffer notes,empty,cc;notes.addEvent(on(),0);notes.addEvent(on(),16);cc.addEvent(juce::MidiMessage::controllerEvent(1,123,0),17);const auto count=a.use_count();bool ok=true;
#if defined(_MSC_VER) && defined(_DEBUG)
    auto* previous=_CrtSetAllocHook(heapHook);allocations=0;frees=0;callbackOwner=true;
#endif
    e.render(output,notes);for(unsigned n=0;n<12;++n){bindings[n%4][0]=n%2?a.get():b.get();ok&=e.setPatch(p,bindings);e.render(output,notes);}
    p.drumSource[0]=false;ok&=e.setPatch(p);e.render(output,empty);p.drumSource[0]=true;ok&=e.setPatch(p);e.render(output,notes);p.synth.layers[1].enabled=false;ok&=e.setPatch(p);e.render(output,cc);e.reset();
#if defined(_MSC_VER) && defined(_DEBUG)
    callbackOwner=false;_CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);std::cout<<"DRUM_ROUTED_CALLBACK allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
    CHECK(ok);CHECK(a.use_count()==count);
}
}
int main(){unassignedSource();literalRouting();partitionsAndHigher();transitions();melodicContinuity();lastUsableAndDisable();layoutAndCommonGates();lateSpace();workerPaths();longTail();realtimeHeap();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
#else
int main(){std::cerr<<"Drum routed acceptance requires a Debug build\n";return 2;}
#endif
