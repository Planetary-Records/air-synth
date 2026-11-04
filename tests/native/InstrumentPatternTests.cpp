#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "PatternIO.h"
#include "OwnedFixture.h"
#include "PerformanceMidi.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <iostream>
#include <thread>
#include <limits>
#include <initializer_list>
#include <memory>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
using namespace airack;
int checks=0, failures=0, sections=0;
void check(bool b,const char* s,int line){++checks;if(!b){++failures;std::cout<<"FAIL "<<line<<": "<<s<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void section(const char* s){++sections;std::cout<<"SECTION "<<s<<'\n';}
ProjectDocument snapshot(InstrumentProcessor& p,std::uint64_t* edit=nullptr,std::uint64_t* reset=nullptr){ProjectDocument d;std::uint64_t e=0,r=0;CHECK(p.tryProjectSnapshot(d,e,r));if(edit)*edit=e;if(reset)*reset=r;return d;}
juce::MemoryBlock bytes(const ProjectDocument& d){juce::MemoryBlock b;CHECK(encodeProjectState(d,b));return b;}
bool recall(InstrumentProcessor& p,const ProjectDocument& d){auto b=bytes(d);return p.restoreProjectState(b.getData(),int(b.getSize()));}
ProjectDocument phrase(){ProjectDocument d;d.mode=PlaybackMode::internalPattern;d.pattern.noteCount=2;d.pattern.notes[0]={0,.01,60,100,1};d.pattern.notes[1]={.02,.005,67,80,2};return d;}
struct Head:juce::AudioPlayHead {
    bool available=true; HostPatternTiming h;std::optional<PhraseMeter> meter;
    juce::Optional<PositionInfo> getPosition() const override {if(!available)return {};PositionInfo p;p.setIsPlaying(h.playing);p.setIsLooping(h.looping);if(h.bpm)p.setBpm(*h.bpm);if(h.ppqPosition)p.setPpqPosition(*h.ppqPosition);if(h.samplePosition)p.setTimeInSamples(*h.samplePosition);if(h.loop)p.setLoopPoints(LoopPoints{h.loop->startBeats,h.loop->endBeats});if(meter)p.setTimeSignature(TimeSignature{meter->numerator,meter->denominator});return p;}
};
float peak(const juce::AudioBuffer<float>& b){return b.getMagnitude(0,b.getNumSamples());}
void same(const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b){bool good=true;for(int c=0;c<2;++c)for(int i=0;i<a.getNumSamples();++i)good&=std::abs(a.getSample(c,i)-b.getSample(c,i))<1e-7f;CHECK(good);}
// Match the preset stem so saving preserves this named fixture; old assets stay unchanged.
void fullProjectAssets(InstrumentProcessor& p,const ProjectDocument& d,juce::MemoryBlock& saved,
                       juce::File& sound,juce::File& pattern,PatternDocument& changed)
{
 const auto now=[&](std::uint64_t* edit=nullptr,std::uint64_t* reset=nullptr){return airack::testing::fixtureValue([&]{return snapshot(p,edit,reset);});};
 CHECK(recall(p,d));CHECK((*now())==d);p.getStateInformation(saved);const auto decodedOwner=std::make_unique<ProjectDocument>();auto& out=*decodedOwner;CHECK(decodeProjectState(saved.getData(),saved.getSize(),out));CHECK(out==d);CHECK(saved.getSize()==41204);CHECK(p.getParameters().size()==int(parameterCount));
 auto folder=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/task-2-assets");CHECK(folder.createDirectory());sound=folder.getChildFile("sound.arsi");pattern=folder.getChildFile("pattern.aipattern");auto midi=folder.getChildFile("pattern.mid");CHECK(p.saveSoundPreset(sound));CHECK(sound.getSize()==41058);CHECK(p.savePattern(pattern));CHECK(p.exportPattern(midi));PatternDocument loaded;CHECK(loadPatternAsset(pattern,loaded));CHECK(loaded==d.pattern);juce::MemoryBlock mid;CHECK(midi.loadFileAsData(mid));CHECK(mid.getSize()>30);if(mid.getSize()>14){const auto* x=static_cast<const unsigned char*>(mid.getData());CHECK(x[0]=='M'&&x[1]=='T'&&x[2]=='h'&&x[3]=='d');CHECK(x[12]==3&&x[13]==192);}
 std::uint64_t e=0,r=0;auto current=(*now(&e,&r));changed=current.pattern;changed.notes[0].pitch=72;CHECK(p.applyPatternEdit(changed,e));CHECK(!p.applyPatternEdit(current.pattern,e));current=(*now(&e));CHECK(p.setPlaybackMode(PlaybackMode::externalMidi,e));current=(*now(&e));auto soundEdit=current.sound;soundEdit.values[0]=.7f;CHECK(p.applySoundEdit(soundEdit,SoundEditOrigin::manual,e));current=(*now());CHECK(current.pattern==changed);CHECK(current.mode==PlaybackMode::externalMidi);CHECK(p.loadSoundPreset(sound));CHECK((*now()).sound==d.sound);CHECK((*now()).pattern==changed);CHECK(p.undoSoundEdit());CHECK((*now()).sound==soundEdit);CHECK(p.redoSoundEdit());CHECK(p.loadPattern(pattern));CHECK((*now()).pattern==d.pattern);CHECK((*now()).sound==d.sound);CHECK((*now()).mode==PlaybackMode::externalMidi);CHECK(p.undoSoundEdit());CHECK((*now()).pattern==changed);CHECK(p.redoSoundEdit());
}
void fullProjectRejectionAndLifecycle(InstrumentProcessor& p,const ProjectDocument& d,const juce::MemoryBlock& saved,
                                    const juce::File& pattern,const PatternDocument& changed)
{
 const auto now=[&](std::uint64_t* edit=nullptr,std::uint64_t* reset=nullptr){return airack::testing::fixtureValue([&]{return snapshot(p,edit,reset);});};
 std::uint64_t e=0,r=0;auto current=(*now(&e,&r));CHECK(p.applyPatternEdit(current.pattern,e));CHECK(p.setPlaybackMode(current.mode,e));std::uint64_t ee=0,rr=0;CHECK((*now(&ee,&rr))==current);CHECK(ee==e&&rr==r);
 auto corrupt=saved;static_cast<char*>(corrupt.getData())[25]^=1;CHECK(!p.restoreProjectState(corrupt.getData(),int(corrupt.getSize())));CHECK(p.stateLoadFailed());CHECK((*now(&ee,&rr))==current);CHECK(ee==e&&rr==r);CHECK(p.undoSoundEdit());CHECK(p.redoSoundEdit());CHECK(p.loadPattern(pattern));CHECK(p.stateLoadFailed());CHECK(recall(p,d));CHECK(!p.stateLoadFailed());CHECK(!p.undoSoundEdit());
 for(const char* path:{"tests/fixtures/m1-analog-pluck.soundstate","tests/fixtures/m2-analog-pluck.v2.soundstate","tests/fixtures/m2-calibrated-pluck.v3.soundstate"}){juce::MemoryBlock old;CHECK(juce::File(RACKSYNTH_WORKSPACE).getChildFile(path).loadFileAsData(old));SoundDocument oldSound;CHECK(decodeSoundState(old.getData(),old.getSize(),oldSound));CHECK(recall(p,d));CHECK(p.restoreSoundState(old.getData(),int(old.getSize())));current=(*now());CHECK(current.sound==oldSound);CHECK(current.pattern==PatternDocument{});CHECK(current.mode==PlaybackMode::externalMidi);}
 CHECK(recall(p,d));static_cast<void>(*now(&e));CHECK(p.beginParameterGesture(0));CHECK(p.setParameterGestureValue(0,.2f));static_cast<void>(*now(&e));CHECK(!p.applyPatternEdit(changed,e));CHECK(!p.setPlaybackMode(PlaybackMode::externalMidi,e));CHECK(recall(p,d));CHECK(!p.setParameterGestureValue(0,.3f));CHECK(p.endParameterGesture(0));CHECK((*now())==d);CHECK(!p.undoSoundEdit());
 Head h;h.meter=PhraseMeter{7,8};p.setPlayHead(&h);p.prepareToPlay(48000,64);juce::AudioBuffer<float> a(2,64);juce::MidiBuffer m;p.processBlock(a,m);CHECK(p.currentHostMeter()==h.meter);static_cast<void>(*now(&e));CHECK(p.createPattern(3,e));current=(*now());CHECK((current.pattern.meter==PhraseMeter{7,8}));CHECK(current.pattern.bars==3&&current.pattern.noteCount==0);h.meter=PhraseMeter{3,4};p.processBlock(a,m);CHECK((*now())==current);h.meter=PhraseMeter{0,4};p.processBlock(a,m);CHECK(!p.currentHostMeter());static_cast<void>(*now(&e));CHECK(p.createPattern(2,e));CHECK(((*now()).pattern.meter==PhraseMeter{4,4}));CHECK(!p.createPattern(9,e));p.setPlayHead(nullptr);
}
void fullProject(){
 // Separate completed calls bound Debug snapshot temporaries on the default 1 MiB test stack.
 section("full_project_recall_and_asset_isolation");const auto processorOwner=std::make_unique<InstrumentProcessor>();auto& p=*processorOwner;auto d=phrase();d.sound.name="sound";d.sound.values[0]=.4f;
 juce::MemoryBlock saved;juce::File sound,pattern;PatternDocument changed;
 fullProjectAssets(p,d,saved,sound,pattern,changed);
 fullProjectRejectionAndLifecycle(p,d,saved,pattern,changed);
}

struct Reentrant:juce::AudioProcessorParameter::Listener {InstrumentProcessor& p;ProjectDocument target;bool good=true,fired=false;Reentrant(InstrumentProcessor& v,ProjectDocument d):p(v),target(d){}void parameterGestureChanged(int,bool) override{}void parameterValueChanged(int,float) override{juce::MemoryBlock b;p.getStateInformation(b);ProjectDocument d;good&=decodeProjectState(b.getData(),b.getSize(),d);if(!fired){fired=true;good&=recall(p,target);}}};
void publication(){
 section("coherent_project_publication_reentrant_history");InstrumentProcessor p;auto a=phrase(),b=phrase();a.sound.values[0]=.1f;b.sound.values[0]=.8f;b.pattern.notes[0].pitch=95;b.mode=PlaybackMode::externalMidi;CHECK(recall(p,a));Reentrant l(p,b);p.control(0).addListener(&l);CHECK(recall(p,a));juce::MessageManager::getInstance()->runDispatchLoopUntil(20);p.control(0).removeListener(&l);CHECK(l.fired&&l.good);CHECK(snapshot(p)==b);
 std::atomic<bool> done=false,good=true;CHECK(recall(p,a));std::thread writer([&]{for(int i=0;i<150;++i)if(!recall(p,i%2?a:b))good=false;done=true;});int reads=0;while(!done){ProjectDocument d;std::uint64_t e=999,r=888;auto original=d;if(p.tryProjectSnapshot(d,e,r)){if(!(d==a||d==b))good=false;++reads;}else if(!(d==original&&e==999&&r==888))good=false;}writer.join();CHECK(good);CHECK(reads>0);
 CHECK(recall(p,a));std::uint64_t e=0;snapshot(p,&e);p.control(0).setValue(.6f);auto pat=a.pattern;pat.notes[0].pitch=65;CHECK(p.applyPatternEdit(pat,e));CHECK(snapshot(p).sound.values[0]==.6f);CHECK(p.undoSoundEdit());CHECK(snapshot(p).sound.values[0]==.6f);CHECK(snapshot(p).pattern==a.pattern);CHECK(p.redoSoundEdit());snapshot(p,&e);CHECK(p.setPlaybackMode(PlaybackMode::externalMidi,e));CHECK(p.undoSoundEdit());CHECK(snapshot(p).mode==PlaybackMode::internalPattern);CHECK(p.undoSoundEdit());CHECK(snapshot(p).pattern==a.pattern);
 // Invalid independent routing automation must not poison project history.
 auto registry=parameterRegistry();std::size_t target=parameterCount;for(std::size_t i=0;i<registry.size();++i)if(registry[i].id=="synth.1.modulation.1.target")target=i;CHECK(target<parameterCount);if(target<parameterCount){p.control(target).setValue(1);CHECK(!validateSoundDocument(snapshot(p).sound));snapshot(p,&e);CHECK(p.applyPatternEdit(pat,e));CHECK(validateProjectDocument(snapshot(p)));CHECK(p.undoSoundEdit());CHECK(validateProjectDocument(snapshot(p)));}
}
std::vector<float> render(const ProjectDocument& d,double rate,int partition,int total,HostPatternTiming origin){InstrumentProcessor p;CHECK(recall(p,d));Head h;h.h=origin;p.setPlayHead(&h);p.prepareToPlay(rate,256);std::vector<float> out;out.reserve(total);juce::MidiBuffer m;for(int pos=0;pos<total;pos+=partition){int size=std::min(partition,total-pos);if(origin.ppqPosition&&origin.bpm){h.h.ppqPosition=*origin.ppqPosition+pos* *origin.bpm/(60*rate);if(origin.looping&&origin.loop&&*h.h.ppqPosition>=origin.loop->endBeats)h.h.ppqPosition=origin.loop->startBeats+std::fmod(*h.h.ppqPosition-origin.loop->startBeats,origin.loop->endBeats-origin.loop->startBeats);}if(origin.samplePosition)h.h.samplePosition=*origin.samplePosition+pos;juce::AudioBuffer<float> a(2,size);p.processBlock(a,m);out.insert(out.end(),a.getReadPointer(0),a.getReadPointer(0)+size);}p.setPlayHead(nullptr);return out;}
void compare(const std::vector<float>& a,const std::vector<float>& b){CHECK(a.size()==b.size());double error=0;for(std::size_t i=0;i<std::min(a.size(),b.size());++i)error=std::max(error,double(std::abs(a[i]-b[i])));CHECK(error<1e-7);}
void clockAudio(){
 section("actual_processor_clock_and_chunk_audio");auto d=phrase();HostPatternTiming h;h.playing=true;h.bpm=120;h.ppqPosition=0;h.samplePosition=0;
 for(double rate:{44100.,48000.,96000.}){compare(render(d,rate,512,2048,h),render(d,rate,64,2048,h));compare(render(d,rate,257,2048,h),render(d,rate,64,2048,h));compare(render(d,rate,65536,65536,h),render(d,rate,512,65536,h));auto neg=h;neg.ppqPosition=-.02;compare(render(d,rate,512,2048,neg),render(d,rate,64,2048,neg));auto loop=h;loop.looping=true;loop.loop=PatternLoopRange{0,.025};compare(render(d,rate,512,4096,loop),render(d,rate,64,4096,loop));}
 InstrumentProcessor p;CHECK(recall(p,d));Head head;head.h=h;p.setPlayHead(&head);p.prepareToPlay(48000,512);RoutedSynthEngine ref;RoutedPatch patch;CHECK(makeRoutedPatch(d.sound,patch));CHECK(ref.setPatch(patch));CHECK(ref.prepare(48000,512));juce::AudioBuffer<float> a(2,512),b(2,512);juce::MidiBuffer m,literal;literal.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);literal.addEvent(juce::MidiMessage::noteOff(1,60),240);literal.addEvent(juce::MidiMessage::noteOn(2,67,juce::uint8(80)),480);m.addEvent(juce::MidiMessage::noteOn(1,90,juce::uint8(127)),3);m.addEvent(juce::MidiMessage::midiStop(),22);p.processBlock(a,m);ref.render(b,literal,120);same(a,b);CHECK(p.patternPlaybackStatus()==PatternPlaybackStatus::playing);m.clear();
 head.h.playing=false;p.processBlock(a,m);CHECK(p.patternPlaybackStatus()==PatternPlaybackStatus::stopped);head.available=false;p.processBlock(a,m);CHECK(p.patternPlaybackStatus()==PatternPlaybackStatus::missingTiming);head.available=true;head.h=h;
 for(int kind=0;kind<8;++kind){head.h=h;if(kind==0)head.h.bpm.reset();if(kind==1)head.h.ppqPosition.reset();if(kind==2){head.h.looping=true;head.h.loop.reset();}if(kind==3)head.h.bpm=std::numeric_limits<double>::quiet_NaN();if(kind==4)head.h.ppqPosition=16777217.;if(kind==5){head.h.looping=true;head.h.loop=PatternLoopRange{0,16777217.};}if(kind==6){head.h.looping=true;head.h.loop=PatternLoopRange{1,0};}if(kind==7){head.h.looping=true;head.h.loop=PatternLoopRange{0,1e-12};}p.processBlock(a,m);CHECK(p.patternPlaybackStatus()==(kind<3?PatternPlaybackStatus::missingTiming:PatternPlaybackStatus::invalidTiming));}
 // Accepted scheduler oracle for sequential tempo, seek, chase and loop lifecycle.
 CHECK(recall(p,d));PatternScheduler scheduler;CHECK(scheduler.prepare(48000,512));CHECK(scheduler.setPattern(d.pattern));CHECK(ref.prepare(48000,512));for(int i=0;i<12;++i){head.h=h;head.h.ppqPosition=(i<3?i*.005:i==3?.002:i==4?3.999:i==5?-.004:i*.01);head.h.samplePosition=i*512;head.h.bpm=i%2?93.:127.;if(i==8)head.h.samplePosition.reset();if(i==9)head.h.playing=false;if(i>=10){head.h.looping=true;head.h.loop=PatternLoopRange{0,.04};}PatternEventBuffer ev;auto status=scheduler.processBlock(head.h,512,d.mode,ev);literal.clear();for(std::size_t n=0;n<ev.count;++n){auto e=ev.events[n];literal.addEvent(e.noteOn?juce::MidiMessage::noteOn(e.channel,e.pitch,e.velocity):juce::MidiMessage::noteOff(e.channel,e.pitch),e.sampleOffset);}p.processBlock(a,m);ref.render(b,literal,head.h.bpm);same(a,b);CHECK(p.patternPlaybackStatus()==status);}
 auto dense=d;dense.pattern.noteCount=256;dense.pattern.swing=.7;for(int i=0;i<256;++i)dense.pattern.notes[i]={i*.001,.002,std::uint8_t(i%128),100,std::uint8_t(i/128+1)};compare(render(dense,48000,512,8192,h),render(dense,48000,64,8192,h));auto empty=d;empty.pattern={};auto silent=render(empty,48000,512,512,h);CHECK(*std::max_element(silent.begin(),silent.end())==0);
 // A note exactly on the callback's last frame must retain its offset.
 auto last=d;last.mode=PlaybackMode::externalMidi;CHECK(recall(p,last));p.prepareToPlay(48000,64);CHECK(ref.prepare(48000,512));m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),511);p.processBlock(a,m);ref.render(b,m,120);same(a,b);m.clear();
 // Invalid prepare is transactional; valid callbacks larger than prepare chunk.
 InstrumentProcessor control;CHECK(recall(control,last));control.prepareToPlay(48000,64);CHECK(recall(p,last));p.prepareToPlay(48000,64);m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);p.processBlock(a,m);control.processBlock(b,m);same(a,b);m.clear();for(auto rate:{0.,std::numeric_limits<double>::quiet_NaN(),400000.}){p.prepareToPlay(rate,64);p.processBlock(a,m);control.processBlock(b,m);same(a,b);}p.prepareToPlay(48000,0);p.processBlock(a,m);control.processBlock(b,m);same(a,b);juce::AudioBuffer<float> huge(2,65537),mono(1,64);p.processBlock(huge,m);CHECK(peak(huge)==0);CHECK(p.patternPlaybackStatus()==PatternPlaybackStatus::invalidBlock);p.processBlock(mono,m);CHECK(peak(mono)==0);p.setPlayHead(nullptr);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking=false;thread_local int allocations=0;
int __cdecl allocationHook(int kind,void*,size_t,int,long,const unsigned char*,int){if(tracking&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC))++allocations;return 1;}
#endif
void keyboard(){
 section("keyboard_queue_ownership_and_capacity");InstrumentProcessor p;CHECK(blankInstance(p));juce::AudioBuffer<float> a(2,512);juce::MidiBuffer m;p.prepareToPlay(48000,64);CHECK(!p.enqueueKeyboardNote(true,128,100,1));CHECK(!p.enqueueKeyboardNote(true,60,128,1));CHECK(!p.enqueueKeyboardNote(true,60,100,0));CHECK(!p.enqueueKeyboardNote(true,60,100,17));CHECK(p.enqueueKeyboardNote(true,60,100,1));p.processBlock(a,m);CHECK(peak(a)>0);CHECK(p.enqueueKeyboardNote(false,60,0,1));for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);
 for(auto mode:{PlaybackMode::externalMidi,PlaybackMode::internalPattern}){auto d=ProjectDocument{};d.mode=mode;CHECK(recall(p,d));p.processBlock(a,m);CHECK(p.enqueueKeyboardNote(true,64,100,2));CHECK(p.enqueueKeyboardNote(false,64,0,2));p.processBlock(a,m);for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);CHECK(p.enqueueKeyboardNote(true,64,100,2));CHECK(p.enqueueKeyboardNote(true,64,0,2));p.processBlock(a,m);for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);CHECK(p.enqueueKeyboardNote(true,64,100,2));p.processBlock(a,m);CHECK(peak(a)>0);p.releaseKeyboardNotes();p.processBlock(a,m);for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);}
 CHECK(recall(p,ProjectDocument{}));p.processBlock(a,m);for(int i=0;i<256;++i)CHECK(p.enqueueKeyboardNote(true,60,100,1));CHECK(!p.enqueueKeyboardNote(true,60,100,1));CHECK(p.performanceInputStatus()==PerformanceInputStatus::keyboardOverflow);p.processBlock(a,m);CHECK(peak(a)==0);CHECK(p.performanceInputStatus()==PerformanceInputStatus::keyboardOverflow);CHECK(p.enqueueKeyboardNote(true,60,100,1));CHECK(p.performanceInputStatus()==PerformanceInputStatus::ready);p.processBlock(a,m);CHECK(peak(a)>0);
 for(int lifecycle=0;lifecycle<4;++lifecycle){CHECK(recall(p,ProjectDocument{}));p.processBlock(a,m);CHECK(p.enqueueKeyboardNote(true,60,100,1));if(lifecycle==0)p.releaseKeyboardNotes();if(lifecycle==1)CHECK(recall(p,ProjectDocument{}));if(lifecycle==2){std::uint64_t e;snapshot(p,&e);CHECK(p.setPlaybackMode(PlaybackMode::internalPattern,e));}if(lifecycle==3)p.prepareToPlay(48000,64);p.processBlock(a,m);CHECK(peak(a)==0);}
 // Both same-key release orders: one source must not release another.
 for(bool keyboardFirst:{false,true}){CHECK(recall(p,ProjectDocument{}));p.processBlock(a,m);m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);CHECK(p.enqueueKeyboardNote(true,60,100,1));p.processBlock(a,m);m.clear();if(keyboardFirst)CHECK(p.enqueueKeyboardNote(false,60,0,1));else m.addEvent(juce::MidiMessage::noteOff(1,60),0);p.processBlock(a,m);m.clear();for(int i=0;i<30;++i)p.processBlock(a,m);CHECK(peak(a)>0);if(keyboardFirst)m.addEvent(juce::MidiMessage::noteOff(1,60),0);else CHECK(p.enqueueKeyboardNote(false,60,0,1));p.processBlock(a,m);m.clear();for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);}
 CHECK(recall(p,ProjectDocument{}));CHECK(p.enqueueKeyboardNote(true,67,100,1));m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);p.processBlock(a,m);m.clear();m.addEvent(juce::MidiMessage::allNotesOff(1),0);p.processBlock(a,m);m.clear();for(int i=0;i<30;++i)p.processBlock(a,m);CHECK(peak(a)>0);m.addEvent(juce::MidiMessage::allSoundOff(1),0);p.processBlock(a,m);CHECK(peak(a)==0);m.clear();CHECK(p.enqueueKeyboardNote(true,67,100,1));p.processBlock(a,m);CHECK(peak(a)>0);m.addEvent(juce::MidiMessage::midiStop(),0);p.processBlock(a,m);CHECK(peak(a)==0);m.clear();
 // Original host order at one offset: rapid on/off must finish released.
 CHECK(recall(p,ProjectDocument{}));m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);m.addEvent(juce::MidiMessage::noteOff(1,60),0);p.processBlock(a,m);m.clear();for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);
 // Internal stop releases pattern ordinarily but keeps keyboard and tails.
 auto d=phrase();d.pattern.noteCount=1;d.pattern.notes[1]={};d.pattern.notes[0].durationBeats=2;CHECK(recall(p,d));Head head;head.h.playing=true;head.h.bpm=120;head.h.ppqPosition=0;p.setPlayHead(&head);CHECK(p.enqueueKeyboardNote(true,60,100,1));p.processBlock(a,m);head.h.playing=false;p.processBlock(a,m);for(int i=0;i<30;++i)p.processBlock(a,m);CHECK(peak(a)>0);CHECK(p.enqueueKeyboardNote(false,60,0,1));p.processBlock(a,m);for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);p.setPlayHead(nullptr);
 CHECK(recall(p,ProjectDocument{}));m.ensureSize(8193*9+64);for(int i=0;i<8193;++i)m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);p.processBlock(a,m);CHECK(p.performanceInputStatus()==PerformanceInputStatus::midiCapacity);CHECK(peak(a)==0);m.clear();m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),511);p.processBlock(a,m);CHECK(p.performanceInputStatus()==PerformanceInputStatus::ready);m.clear();p.processBlock(a,m);CHECK(peak(a)>0);
 // Literal delayed tail survives an Internal stop; reference receives ordinary off.
 auto tail=phrase();tail.pattern.noteCount=1;tail.pattern.notes[1]={};tail.pattern.notes[0].durationBeats=2;RackLayout layout;CHECK(editRackLayout(tail.sound.layout,{RackEditKind::addEffect,noSlot,masterChain,0,EffectKind::delay,false},layout)==RackEditResult::applied);tail.sound.layout=layout;CHECK(recall(p,tail));Head th;th.h.playing=true;th.h.bpm=120;th.h.ppqPosition=0;p.setPlayHead(&th);p.prepareToPlay(48000,512);RoutedSynthEngine reference;RoutedPatch routed;CHECK(makeRoutedPatch(tail.sound,routed));CHECK(reference.setPatch(routed));CHECK(reference.prepare(48000,512));juce::AudioBuffer<float> expected(2,512);juce::MidiBuffer events;events.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);m.clear();p.processBlock(a,m);reference.render(expected,events,120);same(a,expected);events.clear();events.addEvent(juce::MidiMessage::noteOff(1,60),0);th.h.playing=false;p.processBlock(a,m);reference.render(expected,events,120);same(a,expected);CHECK(peak(a)>0);events.clear();for(int i=0;i<12;++i){p.processBlock(a,m);reference.render(expected,events,120);same(a,expected);}p.setPlayHead(nullptr);
 // Releasing keyboard first while pattern owns the same key keeps the note.
 CHECK(recall(p,tail));th.h.playing=true;th.h.ppqPosition=0;p.setPlayHead(&th);CHECK(p.enqueueKeyboardNote(true,60,100,1));p.processBlock(a,m);CHECK(p.enqueueKeyboardNote(false,60,0,1));for(int i=0;i<30;++i){th.h.ppqPosition=(i+1)*512./24000;p.processBlock(a,m);}CHECK(peak(a)>0);p.setPlayHead(nullptr);CHECK(recall(p,ProjectDocument{}));p.processBlock(a,m);
 // SysEx is ignored without copying/allocating; prepared large callbacks too.
 std::vector<unsigned char> data(100000,1);m.addEvent(juce::MidiMessage::createSysExMessage(data.data(),int(data.size())),0);juce::AudioBuffer<float> large(2,65536);
#if defined(_MSC_VER) && defined(_DEBUG)
 auto previous=_CrtSetAllocHook(allocationHook);tracking=true;p.processBlock(large,m);tracking=false;m.clear();tracking=true;for(int i=0;i<20;++i){p.enqueueKeyboardNote(i%2==0,60,100,1);p.processBlock(a,m);}tracking=false;_CrtSetAllocHook(previous);CHECK(allocations==0);std::cout<<"Audio allocations="<<allocations<<'\n';
#else
 CHECK(false);
#endif
 std::atomic<bool> done=false;std::thread producer([&]{for(int i=0;i<500;++i){p.enqueueKeyboardNote(i%2==0,60,100,1);if(i%20==0)p.releaseKeyboardNotes();}done=true;});while(!done)p.processBlock(a,m);producer.join();p.releaseKeyboardNotes();for(int i=0;i<40;++i)p.processBlock(a,m);CHECK(peak(a)<1e-8f);
}
void netEqualLifecycle(){
 section("net_equal_mode_and_pattern_edits_release_at_boundary");
 for(bool patternCase:{false,true}){
    InstrumentProcessor p;auto d=ProjectDocument{};
    if(patternCase){d.mode=PlaybackMode::internalPattern;d.pattern.noteCount=1;d.pattern.notes[0]={0,2,60,100,1};}
    CHECK(recall(p,d));Head head;head.h.playing=true;head.h.bpm=120;head.h.ppqPosition=0;p.setPlayHead(&head);p.prepareToPlay(48000,512);
    RoutedSynthEngine reference;RoutedPatch patch;CHECK(makeRoutedPatch(d.sound,patch));CHECK(reference.setPatch(patch));CHECK(reference.prepare(48000,512));
    juce::AudioBuffer<float> actual(2,512),expected(2,512);juce::MidiBuffer incoming,events;
    events.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
    if(!patternCase)incoming.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
    p.processBlock(actual,incoming);reference.render(expected,events,120);same(actual,expected);incoming.clear();events.clear();
    std::uint64_t edit=0;snapshot(p,&edit);
    if(patternCase){auto changed=d.pattern;changed.notes[0].pitch=72;CHECK(p.applyPatternEdit(changed,edit));snapshot(p,&edit);CHECK(p.applyPatternEdit(d.pattern,edit));}
    else {CHECK(p.setPlaybackMode(PlaybackMode::internalPattern,edit));snapshot(p,&edit);CHECK(p.setPlaybackMode(PlaybackMode::externalMidi,edit));}
    CHECK(snapshot(p)==d);head.h.ppqPosition=512./24000;
    events.addEvent(juce::MidiMessage::noteOff(1,60),0);
    if(patternCase)events.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
    p.processBlock(actual,incoming);reference.render(expected,events,120);same(actual,expected);p.setPlayHead(nullptr);
 }
}
void modeStopPriority(){
 section("mode_change_stop_priority_preserves_literal_tail");
 for(int scenario=0;scenario<3;++scenario){
    InstrumentProcessor p;auto d=ProjectDocument{};d.mode=scenario==1?PlaybackMode::externalMidi:PlaybackMode::internalPattern;
    d.pattern.noteCount=1;d.pattern.notes[0]={0,2,60,100,1};RackLayout layout;
    CHECK(editRackLayout(d.sound.layout,{RackEditKind::addEffect,noSlot,masterChain,0,EffectKind::delay,false},layout)==RackEditResult::applied);d.sound.layout=layout;
    CHECK(recall(p,d));Head head;head.h.playing=true;head.h.bpm=120;head.h.ppqPosition=0;p.setPlayHead(&head);p.prepareToPlay(48000,512);
    RoutedSynthEngine reference;RoutedPatch patch;CHECK(makeRoutedPatch(d.sound,patch));CHECK(reference.setPatch(patch));CHECK(reference.prepare(48000,512));
    juce::AudioBuffer<float> actual(2,512),expected(2,512);juce::MidiBuffer incoming,events;
    events.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);if(scenario==1)incoming.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
    for(int i=0;i<40;++i){head.h.ppqPosition=i*512./24000;p.processBlock(actual,incoming);reference.render(expected,events,120);same(actual,expected);incoming.clear();events.clear();}
    std::uint64_t edit=0;snapshot(p,&edit);
    if(scenario==1){CHECK(p.setPlaybackMode(PlaybackMode::internalPattern,edit));snapshot(p,&edit);}
    if(scenario==2){d.mode=PlaybackMode::externalMidi;CHECK(recall(p,d));reference.reset();}
    else {CHECK(p.setPlaybackMode(PlaybackMode::externalMidi,edit));events.addEvent(juce::MidiMessage::noteOff(1,60),0);}
    head.h.playing=false;
    for(int i=0;i<40;++i){p.processBlock(actual,incoming);reference.render(expected,events,120);same(actual,expected);if(i==0&&scenario!=2)CHECK(peak(actual)>0);events.clear();}
    p.setPlayHead(nullptr);
 }
}
// Fix1: keep original six groups and add labelled timing/concurrency/allocation oracles.
void labelledAudio(const juce::AudioBuffer<float>& actual,
                   const juce::AudioBuffer<float>& expected,
                   const char* scenario, int block)
{
    double error = 0;
    bool finite = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < actual.getNumSamples(); ++frame)
        {
            const float a = actual.getSample(channel, frame);
            const float b = expected.getSample(channel, frame);
            finite &= std::isfinite(a) && std::isfinite(b);
            error = std::max(error, double(std::abs(a - b)));
        }
    ++checks;
    if (!finite || error >= 1e-7)
    {
        ++failures;
        std::cout << "FAIL audio scenario=" << scenario << " block=" << block
                  << " max_error=" << error << " finite=" << finite << '\n';
    }
}

ProjectDocument sustainedInternal()
{
    ProjectDocument d;
    d.mode = PlaybackMode::internalPattern;
    d.pattern.noteCount = 1;
    d.pattern.notes[0] = {0, 2, 60, 100, 1};
    return d;
}

void originalCoordinateAdmission()
{
    section("original_coordinate_admission_survives_chunking_and_loops");
    constexpr double bound = 16777216.;
    struct InvalidOrigin { const char* name; double ppq; };
    const std::array<InvalidOrigin, 5> invalid {{
        {"below_negative_bound", -bound - .001},
        {"above_positive_bound", bound + .001},
        {"nan", std::numeric_limits<double>::quiet_NaN()},
        {"positive_infinity", std::numeric_limits<double>::infinity()},
        {"negative_infinity", -std::numeric_limits<double>::infinity()}
    }};
    for (const auto& origin : invalid)
        for (bool looping : {false, true})
        {
            InstrumentProcessor p;
            CHECK(recall(p, sustainedInternal()));
            Head head;
            head.h.playing = true;
            head.h.bpm = 120;
            head.h.ppqPosition = origin.ppq;
            head.h.looping = looping;
            // Both endpoints are valid; an invalid input coordinate cannot be
            // admitted by the scheduler's normalization of a derived chunk.
            head.h.loop = PatternLoopRange{-bound, -bound + .0005};
            p.setPlayHead(&head);
            p.prepareToPlay(48000, 64);
            juce::AudioBuffer<float> actual(2, 512), silence(2, 512);
            silence.clear();
            juce::MidiBuffer incoming;
            for (int block = 0; block < 2; ++block)
            {
                p.processBlock(actual, incoming);
                ++checks;
                if (p.patternPlaybackStatus() != PatternPlaybackStatus::invalidTiming)
                {
                    ++failures;
                    std::cout << "FAIL invalid origin=" << origin.name
                              << " loop=" << looping << " block=" << block
                              << " status=" << int(p.patternPlaybackStatus()) << '\n';
                }
                const auto label = std::string(origin.name) + (looping ? "/loop" : "/plain");
                labelledAudio(actual, silence, label.c_str(), block);
            }
            p.setPlayHead(nullptr);
        }

    // Exact endpoints remain admitted. A single64-frame callback at either
    // boundary has the literal note-on at0; the upper endpoint is not clamped.
    for (double ppq : {-bound, bound})
    {
        InstrumentProcessor p;
        const auto d = sustainedInternal();
        CHECK(recall(p, d));
        Head head;
        head.h.playing = true;
        head.h.bpm = 120;
        head.h.ppqPosition = ppq;
        p.setPlayHead(&head);
        p.prepareToPlay(48000, 64);
        RoutedSynthEngine reference;
        RoutedPatch patch;
        CHECK(makeRoutedPatch(d.sound, patch));
        CHECK(reference.setPatch(patch));
        CHECK(reference.prepare(48000, 64));
        juce::AudioBuffer<float> actual(2, 64), expected(2, 64);
        juce::MidiBuffer incoming, literal;
        literal.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        p.processBlock(actual, incoming);
        reference.render(expected, literal, 120);
        CHECK(p.patternPlaybackStatus() == PatternPlaybackStatus::playing);
        labelledAudio(actual, expected, ppq < 0 ? "valid_negative_boundary" : "valid_positive_boundary", 0);
        CHECK(peak(actual) > 0);
        p.setPlayHead(nullptr);
    }
    // A valid upper-bound origin may normalize through a valid supplied loop
    // and stay playable across the entire512-frame callback.
    {
        InstrumentProcessor p;
        CHECK(recall(p, sustainedInternal()));
        Head head;
        head.h.playing = true;
        head.h.bpm = 120;
        head.h.ppqPosition = bound;
        head.h.looping = true;
        head.h.loop = PatternLoopRange{0, 4};
        p.setPlayHead(&head);
        p.prepareToPlay(48000, 64);
        juce::AudioBuffer<float> actual(2, 512);
        juce::MidiBuffer incoming;
        p.processBlock(actual, incoming);
        CHECK(p.patternPlaybackStatus() == PatternPlaybackStatus::playing);
        CHECK(peak(actual) > 0);
        p.setPlayHead(nullptr);
    }

    // Admission failure releases only pattern ownership. Its ordinary release
    // tail and the independent held keyboard note must match literal MIDI.
    for (bool looping : {false, true})
    {
        InstrumentProcessor p;
        auto d = sustainedInternal();
        RackLayout layout;
        CHECK(editRackLayout(d.sound.layout,
              {RackEditKind::addEffect, noSlot, masterChain, 0, EffectKind::delay, false},
              layout) == RackEditResult::applied);
        d.sound.layout = layout;
        CHECK(recall(p, d));
        Head head;
        head.h.playing = true;
        head.h.bpm = 120;
        head.h.ppqPosition = 0;
        p.setPlayHead(&head);
        p.prepareToPlay(48000, 64);
        RoutedSynthEngine reference;
        RoutedPatch patch;
        CHECK(makeRoutedPatch(d.sound, patch));
        CHECK(reference.setPatch(patch));
        CHECK(reference.prepare(48000, 512));
        juce::AudioBuffer<float> actual(2, 512), expected(2, 512);
        juce::MidiBuffer incoming, literal;
        CHECK(p.enqueueKeyboardNote(true, 67, 100, 2));
        literal.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        literal.addEvent(juce::MidiMessage::noteOn(2, 67, juce::uint8(100)), 0);
        for (int block = 0; block < 32; ++block)
        {
            head.h.ppqPosition = block * 512. / 24000;
            p.processBlock(actual, incoming);
            reference.render(expected, literal, 120);
            labelledAudio(actual, expected, "admission_release/warmup", block);
            literal.clear();
        }
        head.h.ppqPosition = -bound - .001;
        head.h.looping = looping;
        head.h.loop = PatternLoopRange{-bound, -bound + .0005};
        literal.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        for (int block = 0; block < 4; ++block)
        {
            p.processBlock(actual, incoming);
            reference.render(expected, literal, 120);
            labelledAudio(actual, expected,
                          looping ? "admission_release/loop" : "admission_release/plain", block);
            CHECK(p.patternPlaybackStatus() == PatternPlaybackStatus::invalidTiming);
            CHECK(peak(actual) > 0);
            literal.clear();
        }
        CHECK(p.enqueueKeyboardNote(false, 67, 0, 2));
        literal.addEvent(juce::MidiMessage::noteOff(2, 67), 0);
        for (int block = 0; block < 32; ++block)
        {
            p.processBlock(actual, incoming);
            reference.render(expected, literal, 120);
            labelledAudio(actual, expected,
                          looping ? "admission_tail/loop" : "admission_tail/plain", block);
            literal.clear();
        }
        p.setPlayHead(nullptr);
    }
}

void combinedProjectKeyboardAudioStress()
{
    section("combined_pattern_writer_keyboard_producer_audio_consumer");
    InstrumentProcessor p;
    auto a = sustainedInternal();
    a.pattern.noteCount = 2;
    a.pattern.notes[1] = {.25, .5, 64, 90, 2};
    auto b = a;
    b.pattern.meter = {7, 8};
    b.pattern.swing = .3;
    b.pattern.noteCount = 3;
    b.pattern.notes[0] = {0, 1, 72, 80, 3};
    b.pattern.notes[1] = {.125, .25, 67, 95, 4};
    b.pattern.notes[2] = {2, .3, 55, 50, 1};
    CHECK(recall(p, a));
    Head head;
    head.h.playing = true;
    head.h.bpm = 120;
    head.h.ppqPosition = 0;
    head.h.samplePosition = 0;
    p.setPlayHead(&head);
    p.prepareToPlay(48000, 64);
    juce::AudioBuffer<float> audio(2, 257);
    juce::MidiBuffer incoming;
    constexpr int rounds = 128;
    std::atomic<int> ready {0}, arrivals {0}, phase {0};
    std::atomic<bool> start {false};
    bool writerGood = true, producerGood = true, audioGood = true, snapshotsGood = true;
    int writes = 0, queued = 0, callbacks = 0, accepted = 0, rejected = 0;
    float maximumPeak = 0;
    // A reusable three-party barrier bounds the run and guarantees each role
    // executes once per round; no producer can race far ahead of consumption.
    const auto rendezvous = [&]
    {
        const int before = phase.load();
        if (arrivals.fetch_add(1) == 2)
        {
            arrivals.store(0);
            phase.fetch_add(1);
        }
        else
            while (phase.load() == before) std::this_thread::yield();
    };
    const auto awaitStart = [&]
    {
        ready.fetch_add(1);
        while (!start.load()) std::this_thread::yield();
    };
    std::thread writer([&]
    {
        awaitStart();
        for (int round = 0; round < rounds; ++round)
        {
            rendezvous();
            ProjectDocument current;
            std::uint64_t edit = 0;
            if (!p.tryProjectSnapshot(current, edit)
                || !p.applyPatternEdit(round % 2 == 0 ? b.pattern : a.pattern, edit))
                writerGood = false;
            ++writes;
            rendezvous();
        }
    });
    std::thread producer([&]
    {
        awaitStart();
        for (int round = 0; round < rounds; ++round)
        {
            rendezvous();
            if (!p.enqueueKeyboardNote(round % 2 == 0, 60, 100, 1))
                producerGood = false;
            ++queued;
            if (round % 16 == 15) p.releaseKeyboardNotes();
            rendezvous();
        }
    });
    std::thread consumer([&]
    {
        awaitStart();
        const auto inspect = [&]
        {
            ProjectDocument current;
            const auto before = current;
            std::uint64_t edit = 999, reset = 888;
            if (p.tryProjectSnapshot(current, edit, reset))
            {
                snapshotsGood &= current == a || current == b;
                ++accepted;
            }
            else
            {
                snapshotsGood &= current == before && edit == 999 && reset == 888;
                ++rejected;
            }
        };
        for (int round = 0; round < rounds; ++round)
        {
            rendezvous();
            inspect();
            head.h.ppqPosition = round * 257. / 24000;
            head.h.samplePosition = round * 257;
            p.processBlock(audio, incoming);
            ++callbacks;
            inspect();
            audioGood &= p.patternPlaybackStatus() == PatternPlaybackStatus::playing;
            audioGood &= p.performanceInputStatus() == PerformanceInputStatus::ready;
            for (int channel = 0; channel < 2; ++channel)
                for (int frame = 0; frame < audio.getNumSamples(); ++frame)
                {
                    const float sample = audio.getSample(channel, frame);
                    audioGood &= std::isfinite(sample) && std::abs(sample) <= 1.0f;
                    maximumPeak = std::max(maximumPeak, std::abs(sample));
                }
            rendezvous();
        }
    });
    while (ready.load() != 3) std::this_thread::yield();
    start.store(true);
    writer.join();
    producer.join();
    consumer.join();
    CHECK(writerGood);
    CHECK(producerGood);
    CHECK(audioGood);
    CHECK(snapshotsGood);
    CHECK(writes == rounds && queued == rounds && callbacks == rounds);
    CHECK(accepted > 0 && accepted + rejected == rounds * 2);
    CHECK(maximumPeak > 0);
    CHECK(snapshot(p) == a);
    // All producer/control threads are joined before this final source release.
    // A stopped Internal scheduler plus generation release must leave no key held.
    head.h.playing = false;
    p.releaseKeyboardNotes();
    for (int block = 0; block < 96; ++block) p.processBlock(audio, incoming);
    CHECK(p.patternPlaybackStatus() == PatternPlaybackStatus::stopped);
    CHECK(peak(audio) < 1e-8f);
    p.setPlayHead(nullptr);
    std::cout << "Combined stress writes=" << writes << " queued=" << queued
              << " callbacks=" << callbacks << " snapshots=" << accepted
              << " rejected=" << rejected << " peak=" << maximumPeak << '\n';
}

void denseInternalAllocations()
{
    section("prepared_dense_internal_callbacks_allocate_zero");
    InstrumentProcessor p;
    auto d = sustainedInternal();
    d.pattern.noteCount = 256;
    d.pattern.swing = .7;
    for (int note = 0; note < 256; ++note)
        d.pattern.notes[note] = {note * .001, .002, std::uint8_t(note % 128),
                                100, std::uint8_t(note / 128 + 1)};
    CHECK(recall(p, d));
    Head head;
    head.h.playing = true;
    head.h.bpm = 120;
    head.h.ppqPosition = 0;
    head.h.samplePosition = 0;
    head.h.looping = true;
    head.h.loop = PatternLoopRange{0, .3};
    p.setPlayHead(&head);
    p.prepareToPlay(48000, 64);
    juce::AudioBuffer<float> audio(2, 65536);
    juce::MidiBuffer incoming;
    incoming.ensureSize(8192 * 9 + 64);
    CHECK(p.enqueueKeyboardNote(true, 60, 100, 1));
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous = _CrtSetAllocHook(allocationHook);
    const int before = allocations;
    tracking = true;
    p.processBlock(audio, incoming);
    tracking = false;
    CHECK(p.patternPlaybackStatus() == PatternPlaybackStatus::playing);
    CHECK(peak(audio) > 0);
    head.h.ppqPosition = std::fmod(65536. / 24000, .3);
    head.h.samplePosition = 65536;
    tracking = true;
    p.processBlock(audio, incoming);
    tracking = false;
    const int internalAllocations = allocations - before;
    _CrtSetAllocHook(previous);
    CHECK(internalAllocations == 0);
    CHECK(p.patternPlaybackStatus() == PatternPlaybackStatus::playing);
    CHECK(p.performanceInputStatus() == PerformanceInputStatus::ready);
    bool finite = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < audio.getNumSamples(); ++frame)
            finite &= std::isfinite(audio.getSample(channel, frame));
    CHECK(finite);
    CHECK(peak(audio) > 0);
    std::cout << "Dense Internal allocations=" << internalAllocations
              << " callbacks=2 frames=65536 prepared_chunk=64 notes=256 swing=0.7 rate=48000\n";
#else
    CHECK(false);
#endif
    p.setPlayHead(nullptr);
}

// The PATTERN unit's playhead: the phrase position of the latest block's first sample, only while Internal plays.
void playbackPosition(){
 section("pattern_playback_position");InstrumentProcessor p;auto d=phrase();d.pattern.bars=4;d.pattern.meter=PhraseMeter{4,4};CHECK(recall(p,d));Head head;head.h.playing=true;head.h.bpm=120;head.h.ppqPosition=6.0;p.setPlayHead(&head);juce::AudioBuffer<float> a(2,64);juce::MidiBuffer m;
 p.processBlock(a,m);CHECK(!p.patternPlaybackPosition());p.prepareToPlay(48000,64);CHECK(!p.patternPlaybackPosition());
 for(auto [at,want]:{std::pair{6.0,6.0},std::pair{22.5,6.5},std::pair{-1.0,15.0}}){head.h.ppqPosition=at;p.processBlock(a,m);const auto got=p.patternPlaybackPosition();CHECK(got&&std::abs(*got-want)<1e-9);}
 head.h.playing=false;p.processBlock(a,m);CHECK(!p.patternPlaybackPosition());head.h.playing=true;head.available=false;p.processBlock(a,m);CHECK(!p.patternPlaybackPosition());head.available=true;head.h.ppqPosition=6.0;p.processBlock(a,m);CHECK(p.patternPlaybackPosition());
 std::uint64_t e=0;snapshot(p,&e);CHECK(p.setPlaybackMode(PlaybackMode::externalMidi,e));p.processBlock(a,m);CHECK(!p.patternPlaybackPosition());snapshot(p,&e);CHECK(p.setPlaybackMode(PlaybackMode::internalPattern,e));p.processBlock(a,m);CHECK(p.patternPlaybackPosition());
 p.releaseResources();CHECK(!p.patternPlaybackPosition());p.setPlayHead(nullptr);
}
void directDeferredMidiOwnership() {
    section("deferred_recall_midi_velocity_source_channel_and_cancellation");
    const auto host=[](PerformanceMidi& p,std::initializer_list<juce::MidiMessage> messages){
        juce::MidiBuffer midi;for(const auto& message:messages)midi.addEvent(message,0);
        for(const auto event:midi)p.stageHost(event,event.samplePosition);
    };
    const auto callback=[&](PerformanceMidi& p,std::initializer_list<juce::MidiMessage> messages){p.beginCallback();p.beginChunk();host(p,messages);p.stageKeyboard();CHECK(p.commit());};
    const auto adopt=[&](PerformanceMidi& p,std::uint64_t token,bool includeHost=true){p.clearOwners(true);p.beginCallback();p.beginChunk();p.stageDeferredReset(token,includeHost);p.stageKeyboard();CHECK(p.commit());};
    const auto messages=[](const PerformanceMidi& p){std::vector<juce::MidiMessage> result;for(const auto event:p.midi())result.push_back(event.getMessage());return result;};
    const auto note=[&](const std::vector<juce::MidiMessage>& rows,std::size_t row,int channel,int key,int velocity){CHECK(row<rows.size());if(row<rows.size())CHECK(rows[row].isNoteOn()&&rows[row].getChannel()==channel&&rows[row].getNoteNumber()==key&&rows[row].getVelocity()==velocity);};
    const auto on=[](int key,int velocity=70,int channel=1){return juce::MidiMessage::noteOn(channel,key,juce::uint8(velocity));};
    const auto fresh=[](){auto p=std::make_unique<PerformanceMidi>();p->prepare();return p;};
    {
        auto p=fresh();CHECK(p->enqueue(true,49,80,1));callback(*p,{on(48)});
        p->deferReset(1);CHECK(p->enqueue(true,60,90,1));callback(*p,{on(60)});
        callback(*p,{juce::MidiMessage::noteOff(1,60)});CHECK(p->midi().isEmpty());
        adopt(*p,1);const auto rows=messages(*p);CHECK(rows.size()==1);note(rows,0,1,60,90);
    }
    {
        auto p=fresh();p->deferReset(1);callback(*p,{on(60)});p->deferReset(1);callback(*p,{on(60,110)});
        adopt(*p,1);const auto rows=messages(*p);CHECK(rows.size()==1);note(rows,0,1,60,110);
    }
    {
        auto p=fresh();p->deferReset(1);callback(*p,{on(60)});p->deferReset(2);callback(*p,{on(61,91)});
        adopt(*p,2);const auto rows=messages(*p);CHECK(rows.size()==1);note(rows,0,1,61,91);
        adopt(*p,1);CHECK(p->midi().isEmpty());
    }
    {
        auto p=fresh();p->deferReset(1);CHECK(p->enqueue(true,61,90,1));callback(*p,{on(60)});
        callback(*p,{juce::MidiMessage::controllerEvent(1,123,0)});
        adopt(*p,1);const auto rows=messages(*p);CHECK(rows.size()==1);note(rows,0,1,61,90);
    }
    {
        auto p=fresh();p->deferReset(1);CHECK(p->enqueue(true,61,90,1));CHECK(p->enqueue(true,63,95,2));callback(*p,{on(60),on(62,80,2)});
        callback(*p,{juce::MidiMessage::controllerEvent(1,120,0)});
        adopt(*p,1);const auto rows=messages(*p);CHECK(rows.size()==2);note(rows,0,2,62,80);note(rows,1,2,63,95);
    }
    {
        auto p=fresh();p->deferReset(1);CHECK(p->enqueue(true,61,90,1));callback(*p,{on(60)});
        callback(*p,{juce::MidiMessage::midiStop()});CHECK(p->enqueue(true,65,95,1));callback(*p,{on(64,80)});
        adopt(*p,1);const auto rows=messages(*p);CHECK(rows.size()==2);note(rows,0,1,64,80);note(rows,1,1,65,95);
    }
    {
        auto p=fresh();p->deferReset(1);CHECK(p->enqueue(true,61,90,1));callback(*p,{on(60)});p->cancelKeyboard();
        adopt(*p,1);const auto rows=messages(*p);CHECK(rows.size()==1);note(rows,0,1,60,70);
    }
    {
        auto p=fresh();p->deferReset(1);CHECK(p->enqueue(true,61,90,1));callback(*p,{on(60)});
        adopt(*p,1,false);const auto rows=messages(*p);CHECK(rows.size()==1);note(rows,0,1,61,90);
    }
    {
        auto p=fresh();p->deferReset(1);callback(*p,{on(60)});adopt(*p,2);CHECK(p->midi().isEmpty());adopt(*p,1);CHECK(p->midi().isEmpty());
        p->deferReset(3);callback(*p,{on(62)});p->cancelDeferredReset();adopt(*p,3);CHECK(p->midi().isEmpty());
        p->deferReset(4);callback(*p,{on(63)});p->prepare();adopt(*p,4);CHECK(p->midi().isEmpty());
    }
    {
        auto p=fresh();p->deferReset(1);CHECK(p->enqueue(true,60,90,1));callback(*p,{on(60)});adopt(*p,1);CHECK(messages(*p).size()==2);
        callback(*p,{juce::MidiMessage::noteOff(1,60)});CHECK(p->midi().isEmpty());CHECK(p->enqueue(false,60,0,1));callback(*p,{});
        const auto rows=messages(*p);CHECK(rows.size()==1);if(rows.size()==1)CHECK(rows[0].isNoteOff()&&rows[0].getNoteNumber()==60);
    }
}
}
namespace {
void directDrumProjection(){
    section("drum_projection_precedes_melodic_owner_conversion");auto p=std::make_unique<PerformanceMidi>();p->prepare();p->beginCallback();p->beginChunk();
    const juce::uint8 raw[]{0x90,36,100};p->stageHost({raw,3,0},0);CHECK(p->commit());CHECK(p->drumEvents().size()==1);p->beginChunk();
    CHECK(p->enqueue(true,36,64,1));p->beginCallback();p->stageKeyboard();CHECK(p->commit());CHECK(p->drumEvents().size()==1&&p->drumEvents()[0].velocity==64);
    p->beginChunk();const juce::uint8 cc[]{0xb0,123,0};p->stageHost({cc,3,17},17,true);CHECK(p->commit());CHECK(p->midi().isEmpty());CHECK(p->drumEvents().size()==1&&p->drumEvents()[0].kind==drums::RenderEventKind::stopChannel&&p->drumEvents()[0].hostOffset==17);
    p->beginChunk();p->requestDrumStop();p->stageHost({raw,3,0},0);CHECK(p->commit());CHECK(p->drumEvents().size()==2&&p->drumEvents()[0].kind==drums::RenderEventKind::stopAll&&p->drumEvents()[1].kind==drums::RenderEventKind::noteOn);
}
}
int main(){std::cout<<std::unitbuf;juce::ScopedJuceInitialiser_GUI gui;fullProject();publication();clockAudio();keyboard();netEqualLifecycle();modeStopPriority();originalCoordinateAdmission();combinedProjectKeyboardAudioStress();denseInternalAllocations();playbackPosition();directDeferredMidiOwnership();directDrumProjection();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;}






