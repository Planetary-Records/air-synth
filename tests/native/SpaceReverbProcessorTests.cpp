#include "InstrumentProcessor.h"
#include "UserReach.h"
#include "space/runtime/ModalService.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace preparationHold {
thread_local bool owner=false;
std::atomic<bool> armed=false,entered=false,released=true;
void beforeAllocation(std::size_t bytes) noexcept {
    if(!owner&&bytes==sizeof(airack::space::runtime::ModalReadyBundle)&&armed.exchange(false)) {
        entered.store(true,std::memory_order_release);
        while(!released.load(std::memory_order_acquire))std::this_thread::yield();
    }
}
struct Hold {
    Hold(){entered.store(false);released.store(false);armed.store(true);}
    ~Hold(){release();}
    void release() noexcept {released.store(true,std::memory_order_release);armed.store(false);}
};
}
void* operator new(std::size_t bytes){preparationHold::beforeAllocation(bytes);if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t bytes,std::align_val_t alignment){
    preparationHold::beforeAllocation(bytes);const auto a=std::size_t(alignment);void* p=nullptr;
#if defined(_MSC_VER)
    p=_aligned_malloc(bytes?bytes:1,a);
#else
    if(bytes>std::numeric_limits<std::size_t>::max()-(a-1))throw std::bad_alloc();
    p=std::aligned_alloc(a,((bytes?bytes:1)+a-1)/a*a);
#endif
    if(p)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t bytes,std::align_val_t alignment){return ::operator new(bytes,alignment);}
void operator delete(void* p,std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}
void operator delete(void* p,std::size_t,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}
void operator delete[](void* p,std::size_t,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}

namespace airack {
struct InstrumentStructuralTestAccess {
    static std::uint64_t requestedReset(const InstrumentProcessor& p) noexcept {return p.resetGeneration.load();}
    static std::uint64_t installedReset(const InstrumentProcessor& p) noexcept {return p.renderResetGeneration;}
    static std::size_t activeNotes(const InstrumentProcessor& p) noexcept {return p.engine.activeNoteCount();}
};
}

namespace {
using namespace airack;
int checks=0,failures=0;
void check(bool good,const char* expression,int line) {
    ++checks;if(!good){++failures;if(failures<90)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}
}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Notifications final : juce::AudioProcessorParameter::Listener {
    explicit Notifications(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
    ~Notifications() override{parameter.removeListener(this);}
    void parameterValueChanged(int,float) override{++values;}
    void parameterGestureChanged(int,bool began) override{if(began){++starts;if(onStart)onStart();}else ++ends;}
    std::function<void()> onStart;
    juce::AudioProcessorParameter& parameter;int starts=0,ends=0,values=0;
};
SoundDocument snapshot(InstrumentProcessor& processor) {
    SoundDocument sound;std::uint64_t revision=0;CHECK(processor.trySoundSnapshot(sound,revision));return sound;
}
void guardedWholeSpace() {
    std::cout<<"SECTION guarded97_space_fields_undo_host_automation_and_reentrant_preflight\n";
    const auto reach=unitReach(NativeModule(18),7);CHECK(reach.size()==97);if(reach.size()!=97)return;
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("space-group-"+juce::Uuid().toString());
    CHECK(root.createDirectory().wasOk());companion::ClientOptions offline;offline.allowLaunch=false;
    offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    auto owner=std::make_unique<InstrumentProcessor>(offline);auto& processor=*owner;
    SoundDocument source;for(std::uint8_t slot:{6,7}){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind(16);
        CHECK(editRackLayout(source.layout,add,source.layout)==RackEditResult::applied);}
    const auto other=rackParameterIndex(NativeModule(18),6,"space_reverb.size");CHECK(other);if(!other)return;
    source.macros[0]={parameterRegistry()[reach[10]].stateKey,.15f};source.values[1]=.25f;
    source.controlLocks[0]=true;source.effectLocks[6]=true;
    const auto restore=[&](const SoundDocument& document){juce::MemoryBlock bytes;CHECK(encodeSoundState(document,bytes));CHECK(processor.restoreSoundState(bytes.getData(),int(bytes.getSize())));};
    restore(source);
    std::vector<ParameterGestureValue> edits;std::vector<std::unique_ptr<Notifications>> events;
    for(const auto index:reach){const auto& field=parameterRegistry()[index];const auto old=source.values[index];
        const auto value=field.kind==ParameterKind::continuous?(old<.5f?.731f:.273f):
            (old==0.f?1.f/(field.maximum-field.minimum):0.f);
        edits.push_back({index,value});events.push_back(std::make_unique<Notifications>(processor.control(index)));}
    CHECK(edits.size()==97);const auto leader=reach.front();CHECK(parameterRegistry()[leader].id=="effect.8.bypassed");
    SoundDocument before;std::uint64_t revision=0,reset=0;CHECK(processor.trySoundSnapshot(before,revision,reset));
    const bool began=processor.beginParameterGesture(leader,edits,before,revision,reset);CHECK(began);
    if(!began)return;CHECK(snapshot(processor)==before);for(const auto& event:events)CHECK(event->starts==1&&event->ends==0);
    CHECK(processor.setParameterGestureValues(leader,edits,before,revision,reset));
    processor.control(*other).setValueNotifyingHost(.8f);CHECK(processor.endParameterGesture(leader));
    const auto after=snapshot(processor);for(const auto& edit:edits)CHECK(after.values[edit.index]==edit.value);
    for(const auto& event:events)CHECK(event->starts==1&&event->ends==1&&event->values==1);
    CHECK(after.macros==before.macros&&after.controlLocks==before.controlLocks&&after.effectLocks==before.effectLocks&&after.synthLocks==before.synthLocks);
    auto expected=before;expected.values[*other]=.8f;CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==expected);
    processor.control(0).setValueNotifyingHost(.23f);expected=after;expected.values[0]=.23f;
    CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==expected);
    for(std::size_t index=0;index<parameterCount;++index)if(index!=0&&index!=*other&&!std::binary_search(reach.begin(),reach.end(),index))CHECK(expected.values[index]==before.values[index]);

    const auto attempts=[&]{int calls=0;for(const auto& event:events)calls+=event->starts+event->ends+event->values;return calls;};
    const auto refuse=[&](const auto& values){const auto live=snapshot(processor);const auto calls=attempts();
        const bool opened=processor.beginParameterGesture(leader,values,before,revision,reset);CHECK(!opened);
        CHECK(snapshot(processor)==live);CHECK(attempts()==calls);if(opened)CHECK(processor.endParameterGesture(leader));};
    auto locked=source;locked.controlLocks[reach.back()]=true;restore(locked);CHECK(processor.trySoundSnapshot(before,revision,reset));
    auto noOpLocked=edits;noOpLocked.back().value=before.values[reach.back()];refuse(noOpLocked);
    locked=source;locked.effectLocks[7]=true;restore(locked);CHECK(processor.trySoundSnapshot(before,revision,reset));refuse(edits);
    restore(source);CHECK(processor.trySoundSnapshot(before,revision,reset));
    auto malformed=edits;malformed.push_back(edits.front());refuse(malformed);
    malformed=edits;malformed.front().value=std::numeric_limits<float>::quiet_NaN();refuse(malformed);
    malformed=edits;malformed.back().index=*other;refuse(malformed);
    const auto calls=attempts();CHECK(!processor.beginParameterGesture(leader,edits,before,revision+1,reset));CHECK(attempts()==calls);
    CHECK(!processor.beginParameterGesture(leader,edits,before,revision,reset+1));CHECK(attempts()==calls);

    events.back()->onStart=[&]{processor.control(*other).setValueNotifyingHost(.82f);};
    bool opened=processor.beginParameterGesture(leader,edits,before,revision,reset);CHECK(opened);
    if(opened){CHECK(!processor.setParameterGestureValues(leader,edits,before,revision,reset));CHECK(processor.endParameterGesture(leader));}
    auto live=snapshot(processor);CHECK(live.values[*other]==.82f);for(const auto index:reach)CHECK(live.values[index]==before.values[index]);
    CHECK(live.macros==source.macros&&live.controlLocks==source.controlLocks&&live.effectLocks==source.effectLocks);
    for(const auto& event:events)CHECK(event->starts==event->ends);events.back()->onStart=nullptr;
    restore(source);CHECK(processor.trySoundSnapshot(before,revision,reset));
    events.front()->onStart=[&]{restore(source);};
    opened=processor.beginParameterGesture(leader,edits,before,revision,reset);CHECK(opened);
    if(opened){CHECK(!processor.setParameterGestureValues(leader,edits,before,revision,reset));CHECK(processor.endParameterGesture(leader));}
    CHECK(snapshot(processor)==before);for(const auto& event:events)CHECK(event->starts==event->ends);events.front()->onStart=nullptr;
    CHECK(processor.beginParameterGesture(leader));CHECK(!processor.setParameterGestureValues(leader,std::span(edits).first(6)));CHECK(processor.endParameterGesture(leader));
}
void activeSpaceQualityLifecycle() {
    std::cout<<"SECTION active_space_quality_nonrealtime_pending_until_prepare_and_fresh_audio\n";
    const auto spaceSize=rackParameterIndex(NativeModule(18),0,"space_reverb.size");
    const auto spaceDecay=rackParameterIndex(NativeModule(18),0,"space_reverb.decay_seconds");
    const auto spaceMix=rackParameterIndex(NativeModule(18),0,"space_reverb.mix");
    const auto spacePredelay=rackParameterIndex(NativeModule(18),0,"space_reverb.predelay_ms");
    CHECK(spaceSize&&spaceDecay&&spaceMix&&spacePredelay);if(!spaceSize||!spaceDecay||!spaceMix||!spacePredelay)return;
    const auto normalized=[](std::size_t index,float physical){const auto& d=parameterRegistry()[index];return juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(physical);};
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("space-quality-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    for(const int block:{64,512}){
        InstrumentProcessor processor(offline);
        SoundDocument source;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=0;add.effect=EffectKind(16);CHECK(editRackLayout(source.layout,add,source.layout)==RackEditResult::applied);
        source.values[*spaceSize]=.17f;source.values[*spaceDecay]=normalized(*spaceDecay,.7f);source.values[*spaceMix]=1;source.values[*spacePredelay]=0;
        juce::MemoryBlock bytes;CHECK(encodeSoundState(source,bytes));CHECK(processor.restoreSoundState(bytes.getData(),int(bytes.getSize())));processor.setNonRealtime(false);processor.prepareToPlay(48000,block);
        CHECK(processor.selectedRenderQuality()==RenderQuality::realtime&&processor.effectiveRenderQuality()==RenderQuality::realtime&&!processor.renderQualityPending()&&processor.getLatencySamples()==0);
        juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer notes,empty;notes.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        const auto process=[&](unsigned factor,const juce::MidiBuffer& midi){const auto before=processor.spaceRenderStats();auto input=midi;processor.processBlock(audio,input);const auto after=processor.spaceRenderStats();
            CHECK(before.ready&&after.ready&&before.modeCount==128&&after.modeCount==128&&before.physicalFactor==factor&&after.physicalFactor==factor&&before.activeMask==1&&after.activeMask==1);
            CHECK(after.converterBackend==before.converterBackend&&after.converterBackend.has_value());
            CHECK(after.renderedPhysicalFrames[0]-before.renderedPhysicalFrames[0]==unsigned(block)*factor);for(std::size_t slot=1;slot<8;++slot)CHECK(after.renderedPhysicalFrames[slot]==before.renderedPhysicalFrames[slot]);};
        process(1,notes);
        processor.control(758).setValueNotifyingHost(1);processor.control(*spaceSize).setValueNotifyingHost(.87f);processor.control(*spaceDecay).setValueNotifyingHost(normalized(*spaceDecay,1.3f));
        CHECK(processor.selectedRenderQuality()==RenderQuality::higher&&processor.renderQualityPending());process(1,empty);CHECK(processor.effectiveRenderQuality()==RenderQuality::realtime&&processor.getLatencySamples()==0);
        processor.setNonRealtime(true);CHECK(processor.renderQualityPending());process(1,empty);CHECK(processor.effectiveRenderQuality()==RenderQuality::realtime);
        const auto freshAudio=[&](bool nonRealtime,RenderQuality quality){
            const auto desired=snapshot(processor);juce::MemoryBlock saved;CHECK(encodeSoundState(desired,saved));
            InstrumentProcessor fresh(offline);CHECK(fresh.restoreSoundState(saved.getData(),int(saved.getSize())));fresh.setNonRealtime(nonRealtime);
            // Both preparations must synchronously use the latest requested room,
            // including coefficients which were still being prepared while live.
            processor.prepareToPlay(48000,block);fresh.prepareToPlay(48000,block);CHECK(processor.effectiveRenderQuality()==quality&&fresh.effectiveRenderQuality()==quality);
            const auto factor=quality==RenderQuality::higher?2U:1U;CHECK(processor.spaceRenderStats().physicalFactor==factor&&fresh.spaceRenderStats().physicalFactor==factor);
            CHECK(processor.spaceRenderStats().ready&&fresh.spaceRenderStats().ready&&processor.spaceRenderStats().modeCount==128&&fresh.spaceRenderStats().modeCount==128);
            CHECK(processor.getLatencySamples()==fresh.getLatencySamples()&&processor.getTailLengthSeconds()==fresh.getTailLengthSeconds());
            juce::AudioBuffer<float> reference(2,block);double energy=0;
            for(int n=0;n<32;++n){auto first=n==0?notes:empty,second=first;processor.processBlock(audio,first);fresh.processBlock(reference,second);
                for(int channel=0;channel<2;++channel){CHECK(std::memcmp(audio.getReadPointer(channel),reference.getReadPointer(channel),std::size_t(block)*sizeof(float))==0);
                    for(int frame=0;frame<block;++frame){const auto value=audio.getSample(channel,frame);CHECK(std::isfinite(value));energy+=double(value)*value;}}}
            CHECK(energy>0);fresh.releaseResources();
        };
        freshAudio(true,RenderQuality::higher);CHECK(!processor.renderQualityPending()&&processor.getLatencySamples()==21);
        processor.control(758).setValueNotifyingHost(0);CHECK(processor.renderQualityPending());process(2,empty);CHECK(processor.effectiveRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==21);
        processor.control(758).setValueNotifyingHost(1);CHECK(!processor.renderQualityPending());processor.setNonRealtime(false);CHECK(processor.renderQualityPending());process(2,empty);CHECK(processor.effectiveRenderQuality()==RenderQuality::higher);
        freshAudio(false,RenderQuality::realtime);CHECK(processor.renderQualityPending()&&processor.selectedRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==0);
        processor.control(758).setValueNotifyingHost(0);CHECK(!processor.renderQualityPending());process(1,empty);processor.releaseResources();
    }
}
void pendingRecallMidi() {
    std::cout<<"SECTION pending_space_recall_keeps_only_new_surviving_midi_notes\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("space-recall-midi-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    SoundDocument source;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind(16);CHECK(editRackLayout(source.layout,add,source.layout)==RackEditResult::applied);
    const auto fields=parameterRegistry();for(std::size_t i=0;i<fields.size();++i)if(fields[i].id=="synth.1.analog.amp.release")source.values[i]=0;
    juce::MemoryBlock saved;CHECK(encodeSoundState(source,saved));
    constexpr int block=64;
    for(int cancellation=0;cancellation<5;++cancellation) {
        InstrumentProcessor processor(offline);CHECK(processor.restoreSoundState(saved.getData(),int(saved.getSize())));processor.prepareToPlay(48000,block);
        juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(90)),0);processor.processBlock(audio,midi);CHECK(InstrumentStructuralTestAccess::activeNotes(processor)==1);
        const auto installed=InstrumentStructuralTestAccess::installedReset(processor);
        preparationHold::Hold hold;
        CHECK(processor.restoreSoundState(saved.getData(),int(saved.getSize())));
        const auto desired=InstrumentStructuralTestAccess::requestedReset(processor);CHECK(desired!=installed);
        midi.clear();midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(70)),0);processor.processBlock(audio,midi);
        const auto enteredDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!preparationHold::entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<enteredDeadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(preparationHold::entered.load(std::memory_order_acquire));CHECK(InstrumentStructuralTestAccess::installedReset(processor)==installed);
        midi.clear();
        if(cancellation==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);
        if(cancellation==1)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
        if(cancellation==2)midi.addEvent(juce::MidiMessage::controllerEvent(1,123,0),0);
        if(cancellation==3)midi.addEvent(juce::MidiMessage::controllerEvent(1,120,0),0);
        if(cancellation==4)midi.addEvent(juce::MidiMessage::midiStop(),0);
        processor.processBlock(audio,midi);CHECK(InstrumentStructuralTestAccess::installedReset(processor)==installed);
        hold.release();midi.clear();const auto installDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(InstrumentStructuralTestAccess::installedReset(processor)!=desired&&std::chrono::steady_clock::now()<installDeadline){processor.processBlock(audio,midi);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        CHECK(InstrumentStructuralTestAccess::installedReset(processor)==desired);
        CHECK(InstrumentStructuralTestAccess::activeNotes(processor)==std::size_t(cancellation==0?1:0));
        // The first admitted callback must match a fresh reset with only the
        // surviving post-request note and its latest velocity. The old48 drops.
        InstrumentProcessor fresh(offline);CHECK(fresh.restoreSoundState(saved.getData(),int(saved.getSize())));fresh.prepareToPlay(48000,block);
        juce::AudioBuffer<float> expected(2,block);midi.clear();if(cancellation==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);fresh.processBlock(expected,midi);
        for(int channel=0;channel<2;++channel)CHECK(std::memcmp(audio.getReadPointer(channel),expected.getReadPointer(channel),std::size_t(block)*sizeof(float))==0);
        fresh.releaseResources();processor.releaseResources();
    }
}
void pendingRecallModeMidi() {
    std::cout<<"SECTION pending_space_recall_keeps_desired_mode_midi_without_old_mode_leak\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("space-recall-mode-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    constexpr int block=64;
    for(int scenario=0;scenario<4;++scenario) {
        const bool reverse=scenario==3;
        ProjectDocument initial;initial.mode=reverse?PlaybackMode::externalMidi:PlaybackMode::internalPattern;
        RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind(16);CHECK(editRackLayout(initial.sound.layout,add,initial.sound.layout)==RackEditResult::applied);
        const auto fields=parameterRegistry();for(std::size_t i=0;i<fields.size();++i)if(fields[i].id=="synth.1.analog.amp.release")initial.sound.values[i]=0;
        auto target=initial;target.mode=reverse?PlaybackMode::internalPattern:PlaybackMode::externalMidi;
        juce::MemoryBlock initialBytes,targetBytes;CHECK(encodeProjectState(initial,initialBytes));CHECK(encodeProjectState(target,targetBytes));
        InstrumentProcessor processor(offline);CHECK(processor.restoreProjectState(initialBytes.getData(),int(initialBytes.getSize())));processor.prepareToPlay(48000,block);
        juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;
        if(reverse)midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(90)),0);
        processor.processBlock(audio,midi);CHECK(InstrumentStructuralTestAccess::activeNotes(processor)==std::size_t(reverse?1:0));
        const auto oldStatus=reverse?PatternPlaybackStatus::externalMidi:PatternPlaybackStatus::missingTiming;
        CHECK(processor.patternPlaybackStatus()==oldStatus);
        const auto installed=InstrumentStructuralTestAccess::installedReset(processor);
        preparationHold::Hold hold;CHECK(processor.restoreProjectState(targetBytes.getData(),int(targetBytes.getSize())));
        const auto desired=InstrumentStructuralTestAccess::requestedReset(processor);CHECK(desired!=installed);
        midi.clear();midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(70)),0);processor.processBlock(audio,midi);
        const auto enteredDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!preparationHold::entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<enteredDeadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(preparationHold::entered.load(std::memory_order_acquire));CHECK(InstrumentStructuralTestAccess::installedReset(processor)==installed);
        CHECK(processor.patternPlaybackStatus()==oldStatus);CHECK(InstrumentStructuralTestAccess::activeNotes(processor)==std::size_t(reverse?2:0));
        midi.clear();
        if(scenario==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);
        if(scenario==1)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
        if(scenario==2)midi.addEvent(juce::MidiMessage::midiStop(),0);
        processor.processBlock(audio,midi);CHECK(InstrumentStructuralTestAccess::installedReset(processor)==installed);CHECK(processor.patternPlaybackStatus()==oldStatus);
        if(!reverse){CHECK(InstrumentStructuralTestAccess::activeNotes(processor)==0);CHECK(audio.getMagnitude(0,audio.getNumSamples())==0);}
        hold.release();midi.clear();const auto installDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(InstrumentStructuralTestAccess::installedReset(processor)!=desired&&std::chrono::steady_clock::now()<installDeadline){processor.processBlock(audio,midi);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        CHECK(InstrumentStructuralTestAccess::installedReset(processor)==desired);
        CHECK(processor.patternPlaybackStatus()==(reverse?PatternPlaybackStatus::missingTiming:PatternPlaybackStatus::externalMidi));
        CHECK(InstrumentStructuralTestAccess::activeNotes(processor)==std::size_t(scenario==0?1:0));
        InstrumentProcessor fresh(offline);CHECK(fresh.restoreProjectState(targetBytes.getData(),int(targetBytes.getSize())));fresh.prepareToPlay(48000,block);
        juce::AudioBuffer<float> expected(2,block);midi.clear();if(scenario==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);fresh.processBlock(expected,midi);
        for(int channel=0;channel<2;++channel)CHECK(std::memcmp(audio.getReadPointer(channel),expected.getReadPointer(channel),std::size_t(block)*sizeof(float))==0);
        fresh.releaseResources();processor.releaseResources();
    }
}
}
int main() {
    preparationHold::owner=true;
    juce::ScopedJuceInitialiser_GUI juce;guardedWholeSpace();activeSpaceQualityLifecycle();pendingRecallMidi();pendingRecallModeMidi();
    std::cout<<"SPACE_PROCESSOR checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
