#include "RoutedSynthEngine.h"
#include "space/SpaceActivation.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
const auto allocationOwner=std::this_thread::get_id();
std::atomic<bool> observeReadyAllocations{false},failReadyAllocation{false};
std::atomic<unsigned> readyAllocationAttempts{0},readyAllocationFailures{0};
void readyAllocation(std::size_t bytes){
    if(observeReadyAllocations.load(std::memory_order_acquire)&&bytes==sizeof(airack::space::runtime::ModalReadyBundle)&&std::this_thread::get_id()!=allocationOwner){
        ++readyAllocationAttempts;if(failReadyAllocation.exchange(false,std::memory_order_acq_rel)){++readyAllocationFailures;throw std::bad_alloc();}
    }
}
}
void* operator new(std::size_t n){readyAllocation(n);if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){readyAllocation(n);
#if defined(_MSC_VER)
    auto* p=_aligned_malloc(n?n:1,std::size_t(a));
#else
    auto* p=std::aligned_alloc(std::size_t(a),((n?n:1)+std::size_t(a)-1)/std::size_t(a)*std::size_t(a));
#endif
    if(p)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
namespace {
using namespace airack;
int checks=0,failures=0;
void check(bool value,const char* text,int line){++checks;if(!value){++failures;std::cout<<"FAIL "<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
using Audio=std::array<std::vector<float>,2>;
using Event=std::pair<int,juce::MidiMessage>;
RoutedPatch base(){RoutedPatch p;auto& l=p.synth.layers[0];l.oscillators[0].waveform=Waveform::sine;l.oscillators[0].level=1;l.oscillators[1].level=0;l.level=1;l.cutoffHz=20000;l.resonance=0;l.ampEnvelope=l.filterEnvelope={0,0,0,1};p.synth.masterLevel=1;return p;}
void add(RoutedPatch& p,EffectKind kind,unsigned chain=masterChain){RackLayout result;CHECK(editRackLayout(p.layout,{RackEditKind::addEffect,noSlot,static_cast<std::uint8_t>(chain),p.layout.effectCount[chain],kind,false},result)==RackEditResult::applied);p.layout=result;}
Audio render(RoutedSynthEngine& engine,int frames,int partition=512,const std::vector<Event>& events={},std::span<const DynamicEq::Frame> external={}){
    Audio result;for(auto& ch:result)ch.resize(static_cast<std::size_t>(frames));juce::AudioBuffer<float> audio(2,partition);juce::MidiBuffer midi;midi.ensureSize(1024);
    for(int first=0;first<frames;first+=partition){const int n=std::min(partition,frames-first);audio.setSize(2,n,false,false,true);midi.clear();for(const auto& [offset,message]:events)if(offset>=first&&offset<first+n)midi.addEvent(message,offset-first);engine.render(audio,midi,120,external.empty()?std::span<const DynamicEq::Frame>{}:external.subspan(static_cast<std::size_t>(first),static_cast<std::size_t>(n)));for(unsigned c=0;c<2;++c)std::copy_n(audio.getReadPointer(static_cast<int>(c)),n,result[c].data()+first);}return result;
}
bool bits(const Audio& a,const Audio& b){for(unsigned c=0;c<2;++c)if(a[c].size()!=b[c].size()||std::memcmp(a[c].data(),b[c].data(),a[c].size()*sizeof(float))!=0)return false;return true;}
double difference(const Audio& a,const Audio& b){double result=0;for(unsigned c=0;c<2;++c)for(std::size_t n=0;n<a[c].size();++n)result=std::max(result,std::abs(double(a[c][n])-b[c][n]));return result;}
double energy(const Audio& a){double result=0;for(const auto& ch:a)for(float x:ch){CHECK(std::isfinite(x));result+=double(x)*x;}return result;}
void mixedExternalDetectorAlignment(){
    std::cout<<"SECTION mixed_space_and_eq_external_detector_preserves_chain_order_stop_offsets_and_both_qualities\n";
    constexpr int frames=4096;constexpr double rate=48000;
    std::vector<DynamicEq::Frame> external(frames);
    for(int n=257;n<3401;++n){const double x=.5*std::sin(2*juce::MathConstants<double>::pi*100*(n-257)/rate);external[std::size_t(n)]={x,-.7*x};}
    const auto original=external;
    const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))},{913,juce::MidiMessage::midiStop()},{977,juce::MidiMessage::noteOn(1,60,juce::uint8(100))},{3607,juce::MidiMessage::noteOff(1,60)}};
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher})for(const auto chain:{std::uint8_t(0),masterChain})for(bool spaceFirst:{false,true}){
        auto patch=base();add(patch,spaceFirst?EffectKind::spaceReverb:EffectKind::dynamicEq,chain);add(patch,spaceFirst?EffectKind::dynamicEq:EffectKind::spaceReverb,chain);
        const auto eq=spaceFirst?1u:0u,space=spaceFirst?0u:1u;patch.effects[space].spaceReverb.mix=0;
        auto& band=patch.effects[eq].dynamicEq.bands[0];band.active=true;band.frequencyHz=261.62555f;band.rangeDb=-12;band.thresholdDb=-30;band.attackMs=.1f;band.releaseMs=10;
        band.detectorSource=DynamicEqDetectorSource::externalFree;band.detectorLowHz=50;band.detectorHighHz=200;
        auto reference=patch;reference.layout.effects[space].kind=EffectKind::stereo;
        const auto phrase=[&](const RoutedPatch& source,int partition,bool connected){RoutedSynthEngine engine;CHECK(engine.setPatch(source)&&engine.prepare(rate,512,quality));return render(engine,frames,partition,notes,connected?std::span<const DynamicEq::Frame>(external):std::span<const DynamicEq::Frame>{});};
        const auto expected=phrase(reference,64,true),disconnected=phrase(reference,64,false);
        CHECK(difference(expected,disconnected)>1e-5);CHECK(energy(expected)>0);
        for(const int partition:{17,64,512})CHECK(bits(phrase(patch,partition,true),expected));
        CHECK(bits(phrase(patch,64,false),disconnected));CHECK(external==original);
    }
}
void coldAndOffline(){
    auto patch=base();add(patch,EffectKind::spaceReverb);patch.effects[0].spaceReverb.mix=1;patch.effects[0].spaceReverb.predelayMs=0;patch.effects[0].spaceReverb.character=0;
    RoutedSynthEngine initial,offline,dry;CHECK(initial.setPatch(patch));CHECK(initial.prepare(48000,512));CHECK(offline.prepare(48000,512));CHECK(offline.setPatchQuiescent(patch));CHECK(dry.setPatch(base()));CHECK(dry.prepare(48000,512));
    const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))},{2400,juce::MidiMessage::noteOff(1,60)}};
    const auto actual=render(initial,8192,512,notes),copy=render(offline,8192,512,notes),unprocessed=render(dry,8192,512,notes);CHECK(bits(actual,copy));CHECK(difference(actual,unprocessed)>1e-4);CHECK(energy(actual)>1e-6);CHECK(initial.spaceTailSeconds()>0);
    initial.reset();CHECK(bits(render(initial,8192,512,notes),copy));
}
void exactDryMixedAndPartitions(){
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto patch=base();for(unsigned i=1;i<4;++i){RackLayout result;CHECK(editRackLayout(patch.layout,{RackEditKind::addSynth,noSlot,0,static_cast<std::uint8_t>(i),EffectKind::drive,false},result)==RackEditResult::applied);patch.layout=result;patch.synth.layers[i]=patch.synth.layers[0];patch.synth.layers[i].level=.2f;}
        for(unsigned i=0;i<8;++i)add(patch,i%2==0?EffectKind::spaceReverb:(i%3==0?EffectKind::filter:EffectKind::stereo),i%5);
        auto reference=patch;for(unsigned i=0;i<8;++i)if(patch.layout.effects[i].kind==EffectKind::spaceReverb){patch.effects[i].spaceReverb.mix=0;reference.layout.effects[i].kind=EffectKind::stereo;}
        const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(90))},{97,juce::MidiMessage::noteOn(1,67,juce::uint8(80))},{713,juce::MidiMessage::midiStop()},{809,juce::MidiMessage::noteOn(1,64,juce::uint8(100))}};
        Audio canonical;for(int partition:{1,3,17,64,511,512}){RoutedSynthEngine engine,dry;CHECK(engine.setPatch(patch)&&dry.setPatch(reference));CHECK(engine.prepare(rate,512,quality)&&dry.prepare(rate,512,quality));CHECK(engine.latencySamples()==dry.latencySamples());
            const auto actual=render(engine,1536,partition,notes),expected=render(dry,1536,partition,notes);CHECK(bits(actual,expected));if(canonical[0].empty())canonical=actual;else CHECK(difference(actual,canonical)<=1e-6);
            // Every retained filter/control clock has the same history when Space is replaced.
            auto next=patch,normal=reference;for(unsigned i=0;i<8;++i){if(next.layout.effects[i].kind==EffectKind::spaceReverb)next.layout.effects[i].kind=EffectKind::filter;if(normal.layout.effects[i].kind==EffectKind::stereo&&patch.layout.effects[i].kind==EffectKind::spaceReverb)normal.layout.effects[i].kind=EffectKind::filter;next.effects[i].filter.cutoffHz=normal.effects[i].filter.cutoffHz=1500;}
            CHECK(engine.setPatch(next)&&dry.setPatch(normal));CHECK(bits(render(engine,1024,partition),render(dry,1024,partition)));
        }
    }
}
void retainedBankClocksDuringSpaceSwitch(){
    constexpr std::array<std::array<EffectKind,8>,2> kinds{{
        {EffectKind::drive,EffectKind::delay,EffectKind::reverb,EffectKind::compressor,EffectKind::filter,EffectKind::chorus,EffectKind::phaser,EffectKind::stereo},
        {EffectKind::flanger,EffectKind::dimension,EffectKind::frequencyShift,EffectKind::convolution,EffectKind::frequencySplit,EffectKind::midSideSplit,EffectKind::filter,EffectKind::drive}}};
    const auto changeRetained=[](EffectSettings& e){
        e.drive.gainDb=11;e.drive.toneHz=3200;e.drive.mix=.6f;
        e.delay.seconds=.013f;e.delay.feedback=.6f;e.delay.stereo=.3f;e.delay.filterHz=2400;e.delay.mix=.7f;
        e.delay.saturation=.4f;e.delay.lowCutHz=170;e.delay.wobble=.3f;e.delay.wobbleRateHz=2;
        e.reverb={.8f,.7f,.2f,7,.6f};e.compressor={-30,7,3,250,2,.6f};
        e.filter={FilterFxMode::lowpass,1300,2,7,.6f};e.chorus={.7f,.6f,27,.3f,.7f};e.phaser={.8f,.7f,2700,.4f,.8f};
        e.flanger={.6f,.3f,4,.2f,.7f};e.stereo={-7,.4f,1.6f,StereoChannelMode::swap};
        e.dimension={.3f,.7f,1.4f,.8f};e.frequencyShift={-23,11,.6f};e.convolution={ConvolutionIr::shortRoom,13,-4,.6f};
        e.frequencySplit={2300,-7,true,.4f,3,false,-.3f,.7f};e.midSideSplit={-5,true,.3f,4,false,-.2f,.8f};
    };
    for(const auto& selected:kinds)for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(int partition:{511,512}){
        auto patch=base();for(unsigned slot=0;slot<8;++slot){add(patch,EffectKind::spaceReverb,0);patch.effects[slot].spaceReverb.mix=0;}
        auto dry=patch;for(auto& slot:dry.layout.effects)slot.kind=EffectKind::dynamicEq;
        // Flat insert EQs use the ordinary per-frame retained clocks; this
        // layout cannot enter the master-only EQ block/pipeline shortcut.
        CHECK(dry.layout.effectCount[masterChain]==0&&dry.layout.effectCount[0]==8);
        RoutedSynthEngine actual,reference;CHECK(actual.setPatch(patch)&&reference.setPatch(dry));CHECK(actual.prepare(rate,512,quality)&&reference.prepare(rate,512,quality));
        const std::vector<Event> note{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))}};
        CHECK(bits(render(actual,513,partition,note),render(reference,513,partition,note)));
        // Reset while Space owns the slot, then revisit every legacy bank below.
        actual.reset();reference.reset();
        CHECK(bits(render(actual,513,partition,note),render(reference,513,partition,note)));
        auto next=patch;for(unsigned slot=0;slot<8;++slot)next.layout.effects[slot].kind=selected[slot];
        CHECK(actual.setPatch(next)&&reference.setPatch(next));
        const int beforeLateEdit=int(std::ceil(.005*rate))-17;
        CHECK(bits(render(actual,beforeLateEdit,partition),render(reference,beforeLateEdit,partition)));
        // Restart all unused-bank ramps just before the structural switch.
        // Their moving end states must retain each original addition, including
        // the split banks' current-before-increment convention.
        for(auto& effect:next.effects)changeRetained(effect);
        CHECK(actual.setPatch(next)&&reference.setPatch(next));
        const std::vector<Event> stopAndResume{{317,juce::MidiMessage::midiStop()},{373,juce::MidiMessage::noteOn(1,64,juce::uint8(90))}};
        CHECK(bits(render(actual,1024,partition,stopAndResume),render(reference,1024,partition,stopAndResume)));
        CHECK(actual.patch()==next&&reference.patch()==next);CHECK(reference.dspWorkerStats().blocks==0);
    }
}
void eightWetSlots(){
    for(bool serial:{false,true}){auto patch=base();for(std::uint8_t slot=1;slot<4;++slot)CHECK(editRackLayout(patch.layout,{RackEditKind::addSynth,slot,masterChain,slot},patch.layout)==RackEditResult::applied);for(unsigned i=0;i<8;++i){add(patch,EffectKind::spaceReverb,serial?masterChain:i%5);patch.effects[i].spaceReverb.mix=.15;patch.effects[i].spaceReverb.character=0;patch.effects[i].spaceReverb.predelayMs=0;}CHECK(std::count_if(patch.layout.effects.begin(),patch.layout.effects.end(),[](const auto& slot){return slot.use==SlotUse::present&&slot.kind==EffectKind::spaceReverb;})==8);
    const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))},{999,juce::MidiMessage::noteOff(1,60)}};
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){RoutedSynthEngine a,b;CHECK(a.setPatch(patch)&&b.setPatch(patch));CHECK(a.prepare(48000,512,quality)&&b.prepare(48000,512,quality));const auto first=render(a,4096,512,notes),fine=render(b,4096,17,notes);CHECK(difference(first,fine)<=1e-6);CHECK(energy(first)>1e-6);a.reset();CHECK(energy(render(a,2048,64))==0);}}
}
bool admit(RoutedSynthEngine& engine,const RoutedPatch& patch,std::uint64_t token=0){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer empty;while(std::chrono::steady_clock::now()<end){if(engine.setPatch(patch,token))return true;engine.render(audio,empty);std::this_thread::sleep_for(std::chrono::milliseconds(1));}return false;}
void liveRecallCoherence(){
    RoutedSynthEngine engine;const auto before=base();CHECK(engine.setPatch(before)&&engine.prepare(48000,512));auto desired=before;add(desired,EffectKind::spaceReverb);desired.effects[0].spaceReverb.mix=1;desired.effects[0].spaceReverb.decay.decaySeconds=.7;
    const bool immediate=engine.setPatch(desired);if(!immediate)CHECK(engine.patch()==before);CHECK(immediate||admit(engine,desired));(void)render(engine,512);CHECK(engine.patch()==desired);
    // An exact requested-value recall must cold-install its actual coefficients,
    // even if live coefficient preparation has not finished that same request.
    desired.effects[0].spaceReverb.decay.decaySeconds=7;CHECK(engine.setPatch(desired));(void)render(engine,17);const bool recalled=engine.setPatch(desired,1);CHECK(recalled||admit(engine,desired,1));engine.reset();
    RoutedSynthEngine fresh;CHECK(fresh.setPatch(desired,1)&&fresh.prepare(48000,512));const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,64,juce::uint8(100))}};CHECK(bits(render(engine,4096,64,notes),render(fresh,4096,64,notes)));
    auto removed=before;CHECK(engine.setPatch(removed,1));(void)render(engine,512);CHECK(engine.patch()==removed);engine.reset();CHECK(energy(render(engine,2048))==0);
    for(unsigned i=0;i<3;++i){CHECK(admit(engine,desired,2+i));engine.reset();CHECK(energy(render(engine,1024,64,notes))>0);CHECK(engine.setPatch(removed,2+i));(void)render(engine,512);}
}
void cancelledBeforeBoundary(){
    auto original=base();auto desired=original;add(desired,EffectKind::spaceReverb);
    RoutedSynthEngine edited,fresh;CHECK(edited.setPatch(original)&&fresh.setPatch(original));CHECK(edited.prepare(48000,512)&&fresh.prepare(48000,512));
    CHECK(admit(edited,desired));CHECK(edited.setPatch(original));
    const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))}};
    CHECK(bits(render(edited,1024,64,notes),render(fresh,1024,64,notes)));
}
bool admitPaired(RoutedSynthEngine& edited,RoutedSynthEngine& reference,const RoutedPatch& wanted,std::uint64_t token=0){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(std::chrono::steady_clock::now()<end){if(edited.setPatch(wanted,token))return true;CHECK(bits(render(edited,64,64),render(reference,64,64)));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    return false;
}
RoutedPatch oneSpace(){auto p=base();add(p,EffectKind::spaceReverb);p.effects[0].spaceReverb.character=0;p.effects[0].spaceReverb.predelayMs=0;p.effects[0].spaceReverb.mix=1;return p;}
void cancelWithInstalledTail(){
    const auto original=oneSpace();auto added=original;add(added,EffectKind::spaceReverb);added.effects[1].spaceReverb.mix=.7;
    const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))},{1536,juce::MidiMessage::noteOff(1,60)}};
    RoutedSynthEngine edited,reference;CHECK(edited.setPatch(original)&&reference.setPatch(original));CHECK(edited.prepare(48000,512)&&reference.prepare(48000,512));
    const auto warm=render(edited,4096,64,notes);CHECK(bits(warm,render(reference,4096,64,notes)));CHECK(energy(warm)>0);
    CHECK(admitPaired(edited,reference,added));CHECK(edited.setPatch(original));CHECK(edited.patch()==original);
    const auto retained=render(edited,2048,64);CHECK(bits(retained,render(reference,2048,64)));CHECK(energy(retained)>0);CHECK(edited.spaceRenderStats().renderedPhysicalFrames[1]==0);
    auto removed=original;removed.layout.effects[0].kind=EffectKind::stereo;
    CHECK(edited.setPatch(removed));CHECK(edited.setPatch(original));CHECK(edited.patch()==original);
    CHECK(bits(render(edited,2048,64),render(reference,2048,64)));CHECK(edited.spaceRenderStats().renderedPhysicalFrames[1]==0);
    CHECK(admitPaired(edited,reference,added));CHECK(admitPaired(edited,reference,original,7));
    CHECK(edited.spaceRenderStats().renderedPhysicalFrames[1]==0);edited.reset();reference.reset();
    CHECK(bits(render(edited,4096,64,notes),render(reference,4096,64,notes)));CHECK(edited.spaceRenderStats().renderedPhysicalFrames[1]==0);
}
void failedPreparationCanRecover(){
    for(bool inactiveCorrection:{false,true}){
    const auto original=oneSpace();auto wanted=original;add(wanted,EffectKind::spaceReverb);wanted.effects[1].spaceReverb.mix=.7;
    RoutedSynthEngine edited,reference;CHECK(edited.setPatch(original)&&reference.setPatch(original));CHECK(edited.prepare(48000,512)&&reference.prepare(48000,512));
    const std::vector<Event> notes{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))},{1536,juce::MidiMessage::noteOff(1,60)}};
    CHECK(bits(render(edited,4096,64,notes),render(reference,4096,64,notes)));
    readyAllocationAttempts=0;readyAllocationFailures=0;observeReadyAllocations.store(true,std::memory_order_release);failReadyAllocation.store(true,std::memory_order_release);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(edited.spacePreparationFailures()==0&&std::chrono::steady_clock::now()<end){CHECK(!edited.setPatch(wanted));CHECK(bits(render(edited,64,64),render(reference,64,64)));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    CHECK(edited.spacePreparationFailures()==1&&readyAllocationFailures==1&&readyAllocationAttempts==1);CHECK(edited.patch()==original);
    for(unsigned i=0;i<32;++i){CHECK(!edited.setPatch(wanted));CHECK(bits(render(edited,64,64),render(reference,64,64)));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    CHECK(readyAllocationAttempts==1&&edited.spacePreparationFailures()==1);
    const unsigned correctedSlot=inactiveCorrection?7:1;
    wanted.effects[correctedSlot].spaceReverb.size=.73;wanted.effects[correctedSlot].spaceReverb.decay.decaySeconds=.7;
    if(inactiveCorrection)CHECK(wanted.layout.effects[correctedSlot].use!=SlotUse::present);
    CHECK(admitPaired(edited,reference,wanted));CHECK(readyAllocationAttempts==2&&readyAllocationFailures==1&&edited.spacePreparationFailures()==1);observeReadyAllocations.store(false,std::memory_order_release);
    edited.reset();RoutedSynthEngine fresh;CHECK(fresh.setPatch(wanted)&&fresh.prepare(48000,512));
    CHECK(bits(render(edited,4096,64,notes),render(fresh,4096,64,notes)));CHECK(edited.spaceRenderStats().renderedPhysicalFrames[1]>0);
    }
}
void owningChannel(){
    struct Owned {std::unique_ptr<unsigned> value;};space::OwnedLatestMailbox<Owned> channel;
    channel.publish(Owned{std::make_unique<unsigned>(1)});const auto* held=channel.takeView();CHECK(held&&*held->value==1);
    for(unsigned i=2;i<100;++i){channel.publish(Owned{std::make_unique<unsigned>(i)});CHECK(*held->value==1);}const auto* latest=channel.takeView();CHECK(latest&&*latest->value==99);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking=false;thread_local unsigned allocations=0,frees=0;
int allocationHook(int kind,void*,std::size_t,int,long,const unsigned char*,int){if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE)++frees;}return 1;}
#endif
void callbackHeap(){
    auto patch=base();for(std::uint8_t slot=1;slot<4;++slot)CHECK(editRackLayout(patch.layout,{RackEditKind::addSynth,slot,masterChain,slot},patch.layout)==RackEditResult::applied);for(unsigned i=0;i<8;++i)add(patch,EffectKind::spaceReverb,i%5);CHECK(std::count_if(patch.layout.effects.begin(),patch.layout.effects.end(),[](const auto& slot){return slot.use==SlotUse::present&&slot.kind==EffectKind::spaceReverb;})==8);RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(48000,512,RenderQuality::higher));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer notes,empty,stop;notes.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);stop.addEvent(juce::MidiMessage::midiStop(),113);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous=_CrtSetAllocHook(allocationHook);allocations=frees=0;tracking=true;
#endif
    bool accepted=true;engine.render(audio,notes);for(unsigned i=0;i<32;++i){patch.effects[i%8].spaceReverb.width=.5+.01*i;patch.effects[i%8].spaceReverb.decay.brightness=.1+.02*i;accepted&=engine.setPatch(patch);engine.render(audio,empty);}engine.render(audio,stop);engine.reset();
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);std::cout<<"SPACE_CALLBACK allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
    CHECK(accepted);
    const auto stats=engine.spaceRenderStats();CHECK(stats.ready&&stats.modeCount==128&&stats.physicalFactor==2&&stats.activeMask==255);CHECK(stats.converterBackend==(space::runtime::modalConverterAvx2Available()?space::runtime::ModalConverterBackend::mirroredAvx2:space::runtime::ModalConverterBackend::mirroredSse2));
    for(auto count:stats.renderedPhysicalFrames)CHECK(count==34*1024);engine.reset();CHECK(engine.spaceRenderStats().renderedPhysicalFrames==stats.renderedPhysicalFrames);
}
}
int main(){coldAndOffline();exactDryMixedAndPartitions();mixedExternalDetectorAlignment();retainedBankClocksDuringSpaceSwitch();eightWetSlots();liveRecallCoherence();cancelledBeforeBoundary();cancelWithInstalledTail();failedPreparationCanRecover();owningChannel();callbackHeap();std::cout<<"SPACE_PRODUCT_ROUTING checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

