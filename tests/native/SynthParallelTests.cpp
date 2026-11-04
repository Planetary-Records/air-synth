#include "SynthEngine.h"
#include "RoutedSynthEngine.h"
#include "DspPipeline.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <vector>
#if defined(_WIN32)
#include <emmintrin.h>
#endif

namespace airack {
struct SynthParallelTestAccess {
    static bool begin(SynthEngine& s,const std::array<juce::AudioBuffer<float>*,4>& b,std::span<float> g,const juce::MidiBuffer& m){return s.beginParallelLayers(b,g,m,123.,true);}
    static void render(SynthEngine& s,unsigned first,unsigned last,int offset,int count){s.renderParallelLayers(first,last,offset,count);}
    static void end(SynthEngine& s){s.endParallelLayers();}
    static void segment(SynthEngine& s,const std::array<juce::AudioBuffer<float>*,4>& b,std::span<float> g,juce::MidiBufferIterator first,juce::MidiBufferIterator last,int origin,unsigned mask=0){s.renderLayerSegment(b,g,first,last,origin,123.,mask);}
    static void retire(SynthEngine& s,unsigned layer){s.retireSourceLayer(layer);}
#ifndef NDEBUG
    static auto source(const SynthEngine& s){return s.sourceTestSnapshot();}
#endif
    static bool excluded(SynthEngine& s,const std::array<juce::AudioBuffer<float>*,4>& b,std::span<float> g,const juce::MidiBuffer& m,unsigned mask){return s.beginParallelLayerSegment(b,g,m.cbegin(),m.cend(),123.,true,mask);}
};
}
namespace {
using namespace airack;
int checks=0,failures=0;std::uint64_t compared=0;
void check(bool ok,const char* expression,int line){++checks;if(!ok){++failures;if(failures<20)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Buffers {
    std::array<juce::AudioBuffer<float>,4> audio;
    std::array<juce::AudioBuffer<float>*,4> pointers{};
    std::array<float,512> gains{};
    void size(int count){for(unsigned i=0;i<4;++i){audio[i].setSize(2,count);pointers[i]=&audio[i];}}
    std::span<float> gain(){return std::span(gains).first(std::size_t(audio[0].getNumSamples()));}
};
bool same(const Buffers& a,const Buffers& b){
    bool good=true;const auto count=a.audio[0].getNumSamples();
    for(int n=0;n<count;++n){good=good&&(std::bit_cast<std::uint32_t>(a.gains[std::size_t(n)])==std::bit_cast<std::uint32_t>(b.gains[std::size_t(n)]));++compared;
        for(unsigned layer=0;layer<4;++layer)for(int ch=0;ch<2;++ch){good=good&&(std::bit_cast<std::uint32_t>(a.audio[layer].getSample(ch,n))==std::bit_cast<std::uint32_t>(b.audio[layer].getSample(ch,n)));++compared;}}
    return good;
}
SynthPatch patch(){
    SynthPatch p;p.masterLevel=.27f;
    for(unsigned i=0;i<4;++i){auto& l=p.layers[i];l.enabled=true;l.algorithmRevision=modernSynthAlgorithmRevision;l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.pan=float(i)*.2f-.3f;l.noiseLevel=.01f;l.filterEnvelopeOctaves=1.3f;
        l.ampEnvelope.release=.002f+.003f*float(i);
        l.lfos[0]={LfoShape::sine,false,2.75f,1};l.lfos[1]={LfoShape::triangle,true,1,.25f};
        for(unsigned j=0;j<16;++j){const auto target=ModTarget(1+j%16);auto bank=i%2?ModBank::wavetable:ModBank::analog;
            if(target==ModTarget::level||target==ModTarget::pan)bank=ModBank::common;
            if(target==ModTarget::osc1Width||target==ModTarget::osc2Width)bank=ModBank::analog;
            if(target==ModTarget::osc1Position||target==ModTarget::osc2Position)bank=ModBank::wavetable;
            l.modulation[j]={true,ModSource(1+j%6),target,bank,j%2?-.025f:.025f};}}
    return p;
}
void sourceSegments(){
    auto p=patch();SynthEngine whole,segments;CHECK(whole.setPatch(p));CHECK(segments.setPatch(p));CHECK(whole.prepare(48000,512));CHECK(segments.prepare(48000,512));
    Buffers a,b;a.size(512);b.size(512);juce::MidiBuffer midi;
    for(int at:{0,7,16,17,255,511})midi.addEvent(juce::MidiMessage::noteOn(1,48+at%12,juce::uint8(100)),at);
    whole.renderLayers(a.pointers,a.gain(),midi,123.);
    auto first=midi.cbegin();for(int offset=0;offset<512;offset+=17){const int count=std::min(17,512-offset);auto last=first;while(last!=midi.cend()&&(*last).samplePosition<offset+count)++last;
        std::array<juce::AudioBuffer<float>,4> views;std::array<juce::AudioBuffer<float>*,4> pointers{};
        for(unsigned layer=0;layer<4;++layer){views[layer].setDataToReferTo(b.audio[layer].getArrayOfWritePointers(),2,offset,count);pointers[layer]=&views[layer];}
        SynthParallelTestAccess::segment(segments,pointers,b.gain().subspan(offset,count),first,last,offset);first=last;
    }CHECK(same(a,b));
    SynthParallelTestAccess::retire(segments,0);midi.clear();whole.renderLayers(a.pointers,a.gain(),midi,123.);segments.renderLayers(b.pointers,b.gain(),midi,123.);
    bool silent=true,unrelated=true;for(int n=0;n<512;++n)for(int ch=0;ch<2;++ch){silent&=b.audio[0].getSample(ch,n)==0;for(unsigned layer=1;layer<4;++layer)unrelated&=a.audio[layer].getSample(ch,n)==b.audio[layer].getSample(ch,n);}
    CHECK(silent);CHECK(unrelated);
}
#ifndef NDEBUG
std::uint32_t advanceSeed(std::uint32_t seed,unsigned draws){while(draws--)seed=seed*1664525u+1013904223u;return seed;}
void freshSourceOnsets(){
    for(bool mono:{false,true})for(unsigned bank=0;bank<4;++bank){
        auto p=patch();p.performance.mode=mono?VoiceMode::monophonic:VoiceMode::polyphonic;p.performance.legato=true;p.performance.glideSeconds=.025f;
        p.layers[bank]=LayerSettings{};p.layers[bank].enabled=true;p.layers[bank].engine=EngineKind::wavetable;p.layers[bank].algorithmRevision=modernSynthAlgorithmRevision;p.layers[bank].ampEnvelope.attack=0;
        SynthEngine e;CHECK(e.setPatch(p));CHECK(e.prepare(48000,512));Buffers b;b.size(512);juce::MidiBuffer midi;
        const auto block=[&](int note,unsigned excluded){midi.clear();if(note>=0)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);SynthParallelTestAccess::segment(e,b.pointers,b.gain(),midi.cbegin(),midi.cend(),0,excluded);};
        block(48,0);const auto before=SynthParallelTestAccess::source(e);SynthParallelTestAccess::retire(e,bank);const auto retired=SynthParallelTestAccess::source(e);
        CHECK(before.phase==retired.phase&&before.unison==retired.unison);CHECK(retired.fresh&(1u<<bank));
        for(int iteration=0;iteration<18;++iteration){const int note=iteration<2?48:48+iteration;const auto pre=SynthParallelTestAccess::source(e);unsigned chosen=0;
            if(!mono){bool found=false;for(unsigned v=0;v<16;++v)if(pre.voices[v].active&&pre.voices[v].held&&pre.voices[v].note==note&&pre.voices[v].channel==1){chosen=v;found=true;break;}
                if(!found){for(unsigned v=0;v<16;++v)if(!pre.voices[v].active){chosen=v;found=true;break;}if(!found)chosen=0;}}
            const unsigned noncontinuing=std::popcount(pre.modern&~pre.voices[chosen].continuing);
            block(note,1u<<bank);const auto post=SynthParallelTestAccess::source(e);
            if(mono){CHECK(post.phase==pre.phase&&post.unison==pre.unison);}else{CHECK(post.phase==advanceSeed(pre.phase,noncontinuing*2));CHECK(post.unison==advanceSeed(pre.unison,noncontinuing*12));}
            CHECK(post.fresh&(1u<<bank));
        }
        const auto releaseBefore=SynthParallelTestAccess::source(e);midi.clear();midi.addEvent(juce::MidiMessage::noteOff(1,65),0);SynthParallelTestAccess::segment(e,b.pointers,b.gain(),midi.cbegin(),midi.cend(),0,1u<<bank);const auto released=SynthParallelTestAccess::source(e);CHECK(releaseBefore.phase==released.phase&&releaseBefore.unison==released.unison);CHECK(released.fresh&(1u<<bank));
        block(-1,0);bool silent=true;for(int n=0;n<512;++n)for(int ch=0;ch<2;++ch)silent&=b.audio[bank].getSample(ch,n)==0;CHECK(silent);
        const auto pre=SynthParallelTestAccess::source(e);block(75,0);const auto post=SynthParallelTestAccess::source(e);CHECK(!(post.fresh&(1u<<bank)));
        bool sounds=false;for(int n=0;n<512;++n)for(int ch=0;ch<2;++ch)sounds|=b.audio[bank].getSample(ch,n)!=0;CHECK(sounds);if(mono)CHECK(pre.phase==post.phase&&pre.unison==post.unison);
        if(mono)CHECK(b.audio[bank].getSample(0,0)==0&&b.audio[bank].getSample(1,0)==0); // reset sine phase/filter on actual legato onset; later samples must sound
        SynthParallelTestAccess::retire(e,bank);const auto rebuildBefore=SynthParallelTestAccess::source(e);p.performance.mode=mono?VoiceMode::polyphonic:VoiceMode::monophonic;CHECK(e.setPatch(p));block(-1,1u<<bank);const auto rebuilt=SynthParallelTestAccess::source(e);
        const unsigned starts=mono?16:1;CHECK(rebuilt.phase==advanceSeed(rebuildBefore.phase,starts*4*2));CHECK(rebuilt.unison==advanceSeed(rebuildBefore.unison,starts*4*12));block(-1,0);
        silent=true;for(int n=0;n<512;++n)for(int ch=0;ch<2;++ch)silent&=b.audio[bank].getSample(ch,n)==0;CHECK(silent);CHECK(SynthParallelTestAccess::source(e).fresh&(1u<<bank));
    }
}
#endif
void excludedControlContinuation(){
    auto p=patch();SynthEngine serial,parallel;CHECK(serial.setPatch(p));CHECK(parallel.setPatch(p));CHECK(serial.prepare(48000,512));CHECK(parallel.prepare(48000,512));Buffers a,b;a.size(512);b.size(512);juce::MidiBuffer midi;
    const auto block=[&](int note,unsigned mask){midi.clear();if(note>=0)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);
        SynthParallelTestAccess::segment(serial,a.pointers,a.gain(),midi.cbegin(),midi.cend(),0,mask);CHECK(SynthParallelTestAccess::excluded(parallel,b.pointers,b.gain(),midi,mask));
        for(int first=0;first<512;first+=17)SynthParallelTestAccess::render(parallel,0,4,first,std::min(17,512-first));SynthParallelTestAccess::end(parallel);CHECK(same(a,b));
#ifndef NDEBUG
        const auto s=SynthParallelTestAccess::source(serial),t=SynthParallelTestAccess::source(parallel);CHECK(s.phase==t.phase&&s.unison==t.unison);
#endif
    };
    block(48,0);SynthParallelTestAccess::retire(serial,0);SynthParallelTestAccess::retire(parallel,0);p.layers[0].enabled=false;p.layers[0].cutoffHz=9700;p.layers[0].lfos[0].rateHz=9;p.layers[0].modulation[2].amount=.4f;CHECK(serial.setPatch(p));CHECK(parallel.setPatch(p));
    block(60,1);block(-1,1);block(-1,1);p.layers[0].enabled=true;CHECK(serial.setPatch(p));CHECK(parallel.setPatch(p));block(65,0);block(-1,0);
}
void negativeSourceEvents(){
    for(bool release:{false,true}){
        const auto p=patch();SynthEngine serial,parallel;CHECK(serial.setPatch(p)&&parallel.setPatch(p));CHECK(serial.prepare(48000,512)&&parallel.prepare(48000,512));Buffers a,b;a.size(512);b.size(512);juce::MidiBuffer valid,extra;
        valid.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(100)),0);SynthParallelTestAccess::segment(serial,a.pointers,a.gain(),valid.cbegin(),valid.cend(),0,1);SynthParallelTestAccess::segment(parallel,b.pointers,b.gain(),valid.cbegin(),valid.cend(),0,1);CHECK(same(a,b));
        valid.clear();valid.addEvent(juce::MidiMessage::noteOn(1,67,juce::uint8(100)),0);extra=valid;extra.addEvent(release?juce::MidiMessage::noteOff(1,48):juce::MidiMessage::noteOn(1,60,juce::uint8(100)),-1);
        const auto notes=parallel.activeNoteCount();const bool admitted=SynthParallelTestAccess::excluded(parallel,b.pointers,b.gain(),extra,1);CHECK(!admitted);if(!admitted){CHECK(parallel.activeNoteCount()==notes);CHECK(same(a,b));}
        SynthParallelTestAccess::segment(serial,a.pointers,a.gain(),valid.cbegin(),valid.cend(),0,1);
        if(admitted){SynthParallelTestAccess::render(parallel,0,4,0,512);SynthParallelTestAccess::end(parallel);}else SynthParallelTestAccess::segment(parallel,b.pointers,b.gain(),extra.cbegin(),extra.cend(),0,1);
        CHECK(same(a,b));CHECK(serial.activeNoteCount()==parallel.activeNoteCount());valid.clear();serial.renderLayers(a.pointers,a.gain(),valid,123.);parallel.renderLayers(b.pointers,b.gain(),valid,123.);CHECK(same(a,b));
    }
}
struct Job {
    SynthEngine* synth=nullptr;int offset=0,count=0;
    static void run(void* ptr,unsigned stage) noexcept {auto& job=*static_cast<Job*>(ptr);if(stage<2)SynthParallelTestAccess::render(*job.synth,stage*2,stage*2+2,job.offset,job.count);}
};
void compare(double rate){
    auto source=patch();SynthEngine serial,parallel;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));CHECK(serial.prepare(rate,512));CHECK(parallel.prepare(rate,512));
    Job job{&parallel};detail::DspPipeline pool;CHECK(pool.prepare(&job,Job::run));
    Buffers a,b;juce::MidiBuffer midi;midi.ensureSize(16384);for(int key=48;key<64;++key)midi.addEvent(juce::MidiMessage::noteOn(1,key,juce::uint8(70+key%40)),0);
    const auto block=[&](int count,bool expected){
        a.size(count);b.size(count);juce::ScopedNoDenormals denormals;
        serial.renderLayers(a.pointers,a.gain(),midi,123.);
        const bool admitted=SynthParallelTestAccess::begin(parallel,b.pointers,b.gain(),midi);CHECK(admitted==expected);
        // Workers must own their layer clocks across arbitrary packet boundaries.
        if(admitted){for(int first=0;first<count;first+=17){job.offset=first;job.count=std::min(17,count-first);pool.begin();pool.finish();}SynthParallelTestAccess::end(parallel);}else parallel.renderLayers(b.pointers,b.gain(),midi,123.);
        CHECK(same(a,b));CHECK(serial.activeNoteCount()==parallel.activeNoteCount());midi.clear();
    };
    block(64,true);
    for(int n=0;n<24;++n){if(n==8){source.layers[0].cutoffHz=9700;source.layers[2].modulation[2].amount=.07f;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));}
        block(std::array<int,6>{31,64,127,512,17,257}[std::size_t(n)%6],true);}
    for(int key=48;key<64;++key)midi.addEvent(juce::MidiMessage::noteOff(1,key),0);
    block(127,true);
    for(int n=0;n<12;++n)block(n%2?512:127,true);
    CHECK(serial.activeNoteCount()==0);CHECK(parallel.activeNoteCount()==0);
    midi.addEvent(juce::MidiMessage::noteOn(1,64,juce::uint8(100)),0);block(127,true);
    source.layers[1].filterMode=FilterMode::bandpass;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));
    block(64,false);block(512,false);block(64,true);
    source.layers[2].algorithmRevision=legacyAlgorithmRevision;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));
    block(64,false);block(512,false);block(64,false);
    source.layers[2].algorithmRevision=modernSynthAlgorithmRevision;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));
    block(64,false);block(512,false);block(64,true);
    midi.addEvent(juce::MidiMessage::noteOff(1,64),13);block(127,false);block(64,true);
    midi.addEvent(juce::MidiMessage::midiStop(),19);block(64,false);CHECK(serial.activeNoteCount()==0);CHECK(parallel.activeNoteCount()==0);
    midi.addEvent(juce::MidiMessage::noteOn(1,64,juce::uint8(100)),0);block(127,true);block(512,true);
    source.performance.mode=VoiceMode::monophonic;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));block(64,false);block(512,false);
    serial.reset();parallel.reset();block(64,false);
}
// Four warm revision-2 wavetable layers: both oscillators at 7 voices, drift, Classic in two and Vowel in two, a
// fast and a late attack curve, and LFO routes on the cutoff and position.
SynthPatch warmPatch(){
    SynthPatch p;p.masterLevel=.27f;
    for(unsigned i=0;i<4;++i){auto& l=p.layers[i];l.enabled=true;l.algorithmRevision=modernSynthAlgorithmRevision;l.engine=EngineKind::wavetable;l.pan=float(i)*.2f-.3f;l.noiseLevel=.01f;l.filterEnvelopeOctaves=1.3f;
        l.resonance=.5f;l.filterType=i<2?FilterType::classic:FilterType::vowel;l.vowelSize=1.3f;l.drift=1;l.unisonWidth=.8f;l.attackCurve=i%2?-1.f:1.f;
        l.ampEnvelope.attack=.02f;l.ampEnvelope.release=.002f+.003f*float(i);
        for(auto& o:l.oscillators){o.unison=7;o.unisonCents=30;o.position=.4f;}
        l.lfos[0]={LfoShape::sine,false,2.75f,1};l.lfos[1]={LfoShape::triangle,true,1,.25f};
        l.modulation[0]={true,ModSource::lfo1,ModTarget::cutoff,ModBank::wavetable,.1f};
        l.modulation[1]={true,ModSource::lfo2,ModTarget::osc1Position,ModBank::wavetable,.2f};}
    return p;
}
// The warm scene renders bit for bit alike through two workers and serially, while its continuous keys ramp and
// its counts and types fade structurally (those blocks take the serial path in both engines).
void compareWarm(double rate){
    auto source=warmPatch();SynthEngine serial,parallel;CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));CHECK(serial.prepare(rate,512));CHECK(parallel.prepare(rate,512));
    Job job{&parallel};detail::DspPipeline pool;CHECK(pool.prepare(&job,Job::run));
    Buffers a,b;juce::MidiBuffer midi;midi.ensureSize(16384);for(int key=48;key<64;++key)midi.addEvent(juce::MidiMessage::noteOn(1,key,juce::uint8(70+key%40)),0);
    int admitted=0;
    const auto block=[&](int count){
        a.size(count);b.size(count);juce::ScopedNoDenormals denormals;
        serial.renderLayers(a.pointers,a.gain(),midi,123.);
        if(SynthParallelTestAccess::begin(parallel,b.pointers,b.gain(),midi)){++admitted;for(int first=0;first<count;first+=17){job.offset=first;job.count=std::min(17,count-first);pool.begin();pool.finish();}SynthParallelTestAccess::end(parallel);}
        else parallel.renderLayers(b.pointers,b.gain(),midi,123.);
        CHECK(same(a,b));CHECK(serial.activeNoteCount()==parallel.activeNoteCount());midi.clear();
    };
    for(int n=0;n<24;++n){
        if(n==6)for(auto& l:source.layers){l.vowelSize=.7f;l.drift=.5f;l.unisonWidth=.3f;l.cutoffHz=1800;l.attackCurve=0;for(auto& o:l.oscillators)o.unisonCents=12;}
        if(n==12){source.layers[0].oscillators[1].unison=3;source.layers[3].filterType=FilterType::ladder;}
        if(n==6||n==12){CHECK(serial.setPatch(source));CHECK(parallel.setPatch(source));}
        block(std::array<int,6>{31,64,127,512,17,257}[std::size_t(n)%6]);}
    for(int key=48;key<64;++key)midi.addEvent(juce::MidiMessage::noteOff(1,key),0);
    for(int n=0;n<12;++n)block(n%2?512:127);
    CHECK(admitted>20);
}
void routedSpace(double rate,RenderQuality quality,unsigned slots=1){
    RoutedPatch p;p.synth=patch();
    for(std::uint8_t i=1;i<4;++i)CHECK(editRackLayout(p.layout,{RackEditKind::addSynth,i,masterChain,i},p.layout)==RackEditResult::applied);
    RackEdit add;add.kind=RackEditKind::addEffect;add.slot=0;add.destination=masterChain;add.position=0;add.effect=EffectKind::spaceReverb;
    for(unsigned slot=0;slot<slots;++slot){add.slot=static_cast<std::uint8_t>(slot);add.position=static_cast<std::uint8_t>(slot);CHECK(editRackLayout(p.layout,add,p.layout)==RackEditResult::applied);}CHECK(validateRoutedPatch(p));
    RoutedSynthEngine serial,parallel;CHECK(serial.setPatch(p));CHECK(parallel.setPatch(p));CHECK(serial.prepare(rate,512,quality));CHECK(parallel.prepare(rate,512,quality));
    juce::AudioBuffer<float> a,b;juce::MidiBuffer actual,reference;actual.ensureSize(16384);reference.ensureSize(16384);
    for(int key=48;key<64;++key)actual.addEvent(juce::MidiMessage::noteOn(1,key,juce::uint8(100)),0);
    for(unsigned iteration=0;iteration<72;++iteration){
        const int count=std::array<int,5>{64,512,127,512,64}[iteration%5];a.setSize(2,count);b.setSize(2,count);
        if(iteration==18)for(int key=48;key<64;++key)actual.addEvent(juce::MidiMessage::noteOff(1,key),17+(key%4)*37);
        if(iteration==25)for(int key=48;key<64;++key)actual.addEvent(juce::MidiMessage::noteOn(1,key,juce::uint8(100)),5+(key%3)*29);
        if(iteration==32)actual.addEvent(juce::MidiMessage::noteOff(1,51),17);
        if(iteration==36)actual.addEvent(juce::MidiMessage::midiStop(),19);
        if(iteration==58)actual.addEvent(juce::MidiMessage::noteOn(1,68,juce::uint8(100)),0);
        reference.clear();reference.addEvents(actual,0,count,0);
        // This ignored event forces the unchanged serial synth path without changing sound.
        reference.addEvent(juce::MidiMessage::midiClock(),0);
        const auto priorBlocks=parallel.dspWorkerStats().blocks;
        const auto priorSpaceFrames=parallel.spaceRenderStats().renderedPhysicalFrames;
        serial.render(a,reference,123.);parallel.render(b,actual,123.);actual.clear();
        for(unsigned slot=0;slot<slots;++slot)CHECK(parallel.spaceRenderStats().renderedPhysicalFrames[slot]-priorSpaceFrames[slot]==static_cast<std::uint64_t>(count)*(quality==RenderQuality::higher?2u:1u));
        if(iteration==18||iteration==25||iteration==32)CHECK(parallel.dspWorkerStats().blocks>priorBlocks);
        bool equal=true;for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n){equal=equal&&(std::bit_cast<std::uint32_t>(a.getSample(ch,n))==std::bit_cast<std::uint32_t>(b.getSample(ch,n)));++compared;}
        CHECK(equal);CHECK(serial.activeNoteCount()==parallel.activeNoteCount());
    }
    CHECK(serial.dspWorkerStats().blocks==0);CHECK(parallel.dspWorkerStats().blocks>0);
    const auto before=parallel.dspWorkerStats().blocks;parallel.reset();serial.reset();
    actual.addEvent(juce::MidiMessage::noteOn(1,55,juce::uint8(100)),0);a.setSize(2,512);b.setSize(2,512);serial.render(a,actual,123.);parallel.render(b,actual,123.);
    CHECK(parallel.dspWorkerStats().blocks>before);
}
}
int main(){
    sourceSegments();
#ifndef NDEBUG
    freshSourceOnsets();
#endif
    excludedControlContinuation();
    negativeSourceEvents();
#if defined(_WIN32)
    const auto original=_mm_getcsr();
    for(auto rounding:{_MM_ROUND_NEAREST,_MM_ROUND_DOWN,_MM_ROUND_UP,_MM_ROUND_TOWARD_ZERO}){_mm_setcsr((original&~_MM_ROUND_MASK)|rounding);for(double rate:{44100.,48000.,96000.}){compare(rate);compareWarm(rate);}}
    _mm_setcsr(original);
#else
    for(double rate:{44100.,48000.,96000.}){compare(rate);compareWarm(rate);}
#endif
#if defined(_WIN32)
    for(auto rounding:{_MM_ROUND_NEAREST,_MM_ROUND_DOWN,_MM_ROUND_UP,_MM_ROUND_TOWARD_ZERO}){
        _mm_setcsr((original&~_MM_ROUND_MASK)|rounding);
        for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(double rate:{44100.,48000.,96000.})for(unsigned slots:{1u,8u})routedSpace(rate,quality,slots);
    }
    _mm_setcsr(original);
#else
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(double rate:{44100.,48000.,96000.})for(unsigned slots:{1u,8u})routedSpace(rate,quality,slots);
#endif
    std::cout<<"Synth modulated parallel: "<<checks<<" checks, "<<compared<<" float-bit comparisons, "<<failures<<" failures\n";return failures?1:0;
}
