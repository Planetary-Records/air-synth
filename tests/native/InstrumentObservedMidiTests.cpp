#include "IsolatedRackLibrary.h"
#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <atomic>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <crtdbg.h>

namespace
{
using namespace airack;
int checks=0,failures=0,sections=0;const char* currentSection="";
void check(bool ok,const char* expression,int line)
{++checks;if(!ok){++failures;if(failures<100)std::cerr<<currentSection<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void section(const char* name){currentSection=name;++sections;std::cout<<name<<'\n';}
bool close(double a,double b){return std::abs(a-b)<1e-9;}
struct Head final : juce::AudioPlayHead
{
    bool available=true;HostPatternTiming timing;
    std::optional<TimeSignature> signature=TimeSignature{4,4};
    Head(){timing.playing=true;timing.bpm=120;timing.ppqPosition=0;timing.samplePosition=0;}
    juce::Optional<PositionInfo> getPosition() const override
    {if(!available)return {};PositionInfo p;p.setIsPlaying(timing.playing);p.setIsLooping(timing.looping);
     if(timing.bpm)p.setBpm(*timing.bpm);if(timing.ppqPosition)p.setPpqPosition(*timing.ppqPosition);
     if(timing.samplePosition)p.setTimeInSamples(*timing.samplePosition);
     if(timing.loop)p.setLoopPoints(LoopPoints{timing.loop->startBeats,timing.loop->endBeats});
     if(signature)p.setTimeSignature(*signature);return p;}
    void at(std::int64_t sample){timing.samplePosition=sample;timing.ppqPosition=sample/24000.;}
};
struct Fixture
{
    Head head;std::unique_ptr<InstrumentProcessor> processor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    Fixture(){CHECK(copperArcInstance(*processor));processor->setPlayHead(&head);}
    ~Fixture(){processor->setPlayHead(nullptr);}
    void prepare(int chunk=512){processor->prepareToPlay(48000,chunk);}
};
ProjectDocument project(InstrumentProcessor& p,std::uint64_t* edit=nullptr,std::uint64_t* reset=nullptr)
{ProjectDocument d;std::uint64_t e=0,r=0;CHECK(p.tryProjectSnapshot(d,e,r));if(edit)*edit=e;if(reset)*reset=r;return d;}
juce::MemoryBlock encoded(InstrumentProcessor& p){juce::MemoryBlock b;p.getStateInformation(b);return b;}
bool recall(InstrumentProcessor& p,const ProjectDocument& d)
{juce::MemoryBlock b;CHECK(encodeProjectState(d,b));return p.restoreProjectState(b.getData(),int(b.getSize()));}
ObservedMidiSnapshot capture(InstrumentProcessor& p)
{ObservedMidiSnapshot s;p.captureObservedMidi(s);CHECK(validateObservedMidiSnapshot(s));return s;}
void reason(InstrumentProcessor& p,ObservedMidiReason why)
{auto s=capture(p);CHECK(s.reason==why);CHECK(s.availability==ObservedMidiAvailability::unavailable);CHECK(s.noteCount==0);if(s.reason!=why||s.availability!=ObservedMidiAvailability::unavailable||s.noteCount!=0)std::cout<<"OBSERVED_MIDI_REASON_FAILURE expected="<<int(why)<<" actual="<<int(s.reason)<<" availability="<<int(s.availability)<<" notes="<<s.noteCount<<" pattern_status="<<int(p.patternPlaybackStatus())<<std::endl;}
void timed(const ObservedMidiSnapshot& s)
{CHECK(s.noteCount>0);CHECK(s.bpm==120);CHECK(s.meter.has_value());
 CHECK(s.ageMs<=observedMidiFreshnessMs?(s.availability==ObservedMidiAvailability::fresh&&s.reason==ObservedMidiReason::none)
                                      :(s.availability==ObservedMidiAvailability::stale&&s.reason==ObservedMidiReason::stalePublisher));}
void raw(juce::MidiBuffer& m,int status,int data1,int data2,int offset)
{const std::uint8_t bytes[]{std::uint8_t(status),std::uint8_t(data1),std::uint8_t(data2)};m.addEvent(bytes,3,offset);}
void note(juce::MidiBuffer& m,int pitch=60,int offset=0,int channel=1,int velocity=100)
{raw(m,0x90+channel-1,pitch,velocity,offset);}
void musicalEqual(const ObservedMidiSnapshot& a,const ObservedMidiSnapshot& b)
{CHECK(a.meter==b.meter&&a.bpm==b.bpm&&a.windowBeats==b.windowBeats&&a.noteCount==b.noteCount&&a.truncated==b.truncated);
 CHECK(a.notes==b.notes);}
GroupIntent group()
{GroupIntent g;g.group.bytes[0]=1;g.member.bytes[0]=2;CHECK(setGroupText("Session",g.name));CHECK(setGroupText("bass",g.role));
 CHECK(setGroupText("A",g.key));CHECK(setGroupText("minor",g.scale));g.bpm=120;CHECK(validateGroupIntent(g));return g;}
std::uint64_t context(InstrumentProcessor& p)
{auto r=airack::testing::beginAiRequest(p,{"Observation context oracle","synthetic-selected_model:1.0",{ai::RequestScope::sound,false,false}},120);
 CHECK(r.has_value());if(!r)return 0;
 const auto revision=r->tag.contextRevision;
 // The oracle never submits its capture; release its own request before the next sample.
 p.discardAiProposal();return revision;}
void external_callback_observation()
{
 section("external_callback_observation");Fixture whole,split;whole.prepare(512);split.prepare(64);
 juce::AudioBuffer<float> a(2,2048),b(2,2048);juce::MidiBuffer midi;
 note(midi,60,100);raw(midi,0x80,60,0,600);note(midi,60,800,2,80);note(midi,60,1000,2,90);raw(midi,0x81,60,0,1300);
 whole.processor->processBlock(a,midi);split.processor->processBlock(b,midi);
 auto s=capture(*whole.processor),t=capture(*split.processor);timed(s);musicalEqual(s,t);
 CHECK((s.meter==PhraseMeter{4,4}));CHECK(s.windowBeats==16&&s.noteCount==3);
 const double left=2048/24000.-16;
 CHECK(close(s.notes[0].startBeats,100/24000.-left)&&close(s.notes[0].durationBeats,500/24000.));
 CHECK(s.notes[0].pitch==60&&s.notes[0].channel==1&&s.notes[0].velocity==100&&!s.notes[0].held);
 CHECK(close(s.notes[1].startBeats,800/24000.-left)&&close(s.notes[1].durationBeats,200/24000.));
 CHECK(s.notes[1].channel==2&&s.notes[1].velocity==80&&!s.notes[1].held);
 CHECK(close(s.notes[2].durationBeats,300/24000.)&&s.notes[2].channel==2&&s.notes[2].velocity==90);
 whole.head.at(2048);midi.clear();note(midi,64,20);note(midi,65,30,2);raw(midi,0xb0,123,0,120);
 const std::uint8_t stop=0xfc;midi.addEvent(&stop,1,200);whole.processor->processBlock(a,midi);s=capture(*whole.processor);
 CHECK(s.noteCount==5);CHECK(!s.notes[3].held&&!s.notes[4].held);CHECK(close(s.notes[3].durationBeats,100/24000.));CHECK(close(s.notes[4].durationBeats,170/24000.));
 whole.head.at(4096);midi.clear();note(midi,67,0);whole.processor->processBlock(a,midi);const auto beforeStop=capture(*whole.processor);
 whole.head.timing.playing=false;whole.head.timing.bpm.reset();whole.head.timing.ppqPosition.reset();whole.head.signature.reset();
 whole.processor->processBlock(a,midi);s=capture(*whole.processor);
 CHECK(s.availability==ObservedMidiAvailability::stale&&s.reason==ObservedMidiReason::stopped);CHECK(s.noteCount==beforeStop.noteCount);
 CHECK(!s.notes[s.noteCount?s.noteCount-1:0].held);CHECK(s.ageMs>=beforeStop.ageMs);CHECK(s.meter==beforeStop.meter&&s.bpm==beforeStop.bpm);
 Fixture odd;odd.head.signature=Head::TimeSignature{7,8};odd.prepare();odd.processor->processBlock(a,midi);s=capture(*odd.processor);
 CHECK((s.meter==PhraseMeter{7,8}));CHECK(s.windowBeats==14);timed(s);
 Fixture loop;loop.prepare();loop.head.timing.ppqPosition=.09;loop.head.timing.looping=true;loop.head.timing.loop=PatternLoopRange{0,.1};
 juce::AudioBuffer<float> shortAudio(2,512);midi.clear();note(midi,70,480);loop.processor->processBlock(shortAudio,midi);s=capture(*loop.processor);
 CHECK(s.noteCount==1&&s.notes[0].held);CHECK(close(s.notes[0].startBeats,16-32/24000.));CHECK(close(s.notes[0].durationBeats,32/24000.));
 loop.head.timing.ppqPosition=.09+512/24000.-.1;loop.head.timing.samplePosition=512;midi.clear();loop.processor->processBlock(shortAudio,midi);reason(*loop.processor,ObservedMidiReason::noNotes);
 for(int mutation=0;mutation<10;++mutation){Fixture f;f.prepare();
   if(mutation==0)f.head.timing.bpm.reset();if(mutation==1)f.head.timing.ppqPosition.reset();if(mutation==2)f.head.signature.reset();
   if(mutation==3)f.head.timing.bpm=std::numeric_limits<double>::quiet_NaN();if(mutation==4)f.head.timing.ppqPosition=16777217.;
   if(mutation==5)f.head.signature=Head::TimeSignature{-1,4};if(mutation==6)f.head.signature=Head::TimeSignature{256,4};
   if(mutation==7)f.head.signature=Head::TimeSignature{4,3};if(mutation==8)f.head.signature=Head::TimeSignature{4,0};
   if(mutation==9)f.head.timing.samplePosition=(std::numeric_limits<std::int64_t>::max)()-100;
   midi.clear();note(midi);f.processor->processBlock(shortAudio,midi);reason(*f.processor,mutation<3?ObservedMidiReason::missingTiming:ObservedMidiReason::invalidTiming);
   if(mutation<3)CHECK(shortAudio.getMagnitude(0,512)>0);}
 Fixture keyboard;keyboard.prepare();CHECK(keyboard.processor->enqueueKeyboardNote(true,60,100,1));midi.clear();keyboard.processor->processBlock(shortAudio,midi);
 CHECK(shortAudio.getMagnitude(0,512)>0);reason(*keyboard.processor,ObservedMidiReason::noNotes);
 Fixture internal;auto d=project(*internal.processor);d.mode=PlaybackMode::internalPattern;d.pattern.noteCount=1;d.pattern.notes[0]={0,1,48,100,1};
 CHECK(recall(*internal.processor,d));internal.prepare();midi.clear();note(midi,80);CHECK(internal.processor->enqueueKeyboardNote(true,81,100,1));internal.processor->processBlock(shortAudio,midi);
 CHECK(shortAudio.getMagnitude(0,512)>0);reason(*internal.processor,ObservedMidiReason::internalMode);
 // I1 regression: absent PositionInfo is unknown timing, distinct from an available stop.
 {
   const auto captureMissing=[](Fixture& fixture,juce::AudioBuffer<float>& audio)
   {
     auto& p=*fixture.processor;const auto saved=encoded(p);const auto link=p.groupLinkView();const auto musical=p.aiMusicalContext();
     std::uint64_t edit=0,reset=0;const auto state=project(p,&edit,&reset);juce::AudioBuffer<float> before(audio);
     auto missing=capture(p);CHECK(missing.availability==ObservedMidiAvailability::unavailable);CHECK(missing.reason==ObservedMidiReason::missingTiming);
     CHECK(missing.noteCount==0&&!missing.meter&&!missing.bpm&&missing.windowBeats==0&&!missing.truncated);
     CHECK(encoded(p)==saved&&p.groupLinkView()==link&&p.aiMusicalContext()==musical);std::uint64_t nextEdit=0,nextReset=0;
     CHECK(project(p,&nextEdit,&nextReset)==state&&nextEdit==edit&&nextReset==reset);
     bool exact=true;for(int ch=0;ch<2;++ch)for(int frame=0;frame<audio.getNumSamples();++frame)
       exact&=std::bit_cast<std::uint32_t>(audio.getSample(ch,frame))==std::bit_cast<std::uint32_t>(before.getSample(ch,frame));CHECK(exact);
   };
   Fixture unavailable;CHECK(unavailable.processor->setGroupIntent(group()));unavailable.head.available=false;unavailable.prepare();
   juce::AudioBuffer<float> unknownAudio(2,512);juce::MidiBuffer incoming;note(incoming,52);unavailable.processor->processBlock(unknownAudio,incoming);
   CHECK(unknownAudio.getMagnitude(0,512)>0);captureMissing(unavailable,unknownAudio);
   Fixture lost;CHECK(lost.processor->setGroupIntent(group()));lost.prepare();lost.processor->processBlock(unknownAudio,incoming);
   auto playing=capture(*lost.processor);CHECK(playing.noteCount==1&&playing.notes[0].held);lost.head.available=false;
   incoming.clear();note(incoming,55);lost.processor->processBlock(unknownAudio,incoming);CHECK(unknownAudio.getMagnitude(0,512)>0);captureMissing(lost,unknownAudio);
   Fixture noHead;CHECK(noHead.processor->setGroupIntent(group()));noHead.prepare();noHead.processor->processBlock(unknownAudio,incoming);
   CHECK(capture(*noHead.processor).noteCount==1);noHead.processor->setPlayHead(nullptr);incoming.clear();note(incoming,57);
   noHead.processor->processBlock(unknownAudio,incoming);CHECK(unknownAudio.getMagnitude(0,512)>0);captureMissing(noHead,unknownAudio);
   Fixture actualStop;actualStop.prepare();actualStop.processor->processBlock(unknownAudio,incoming);const auto lastPlaying=capture(*actualStop.processor);
   CHECK(lastPlaying.noteCount==1&&lastPlaying.notes[0].held);actualStop.head.timing.playing=false;
   actualStop.head.timing.bpm.reset();actualStop.head.timing.ppqPosition.reset();actualStop.head.signature.reset();incoming.clear();
   actualStop.processor->processBlock(unknownAudio,incoming);const auto stopped=capture(*actualStop.processor);
   CHECK(stopped.availability==ObservedMidiAvailability::stale&&stopped.reason==ObservedMidiReason::stopped);
   CHECK(stopped.noteCount==lastPlaying.noteCount&&stopped.meter==lastPlaying.meter&&stopped.bpm==lastPlaying.bpm&&stopped.windowBeats==lastPlaying.windowBeats);
   CHECK(stopped.notes[0].pitch==lastPlaying.notes[0].pitch&&stopped.notes[0].startBeats==lastPlaying.notes[0].startBeats);
   CHECK(!stopped.notes[0].held&&stopped.notes[0].durationBeats==lastPlaying.notes[0].durationBeats&&stopped.ageMs>=lastPlaying.ageMs);
 }
}
void lifecycle_capacity_and_isolation()
{
 section("lifecycle_capacity_and_isolation");Fixture f;juce::AudioBuffer<float> a(2,512);juce::MidiBuffer midi;note(midi);
 f.processor->processBlock(a,midi);reason(*f.processor,ObservedMidiReason::unprepared);f.prepare();f.processor->processBlock(a,midi);timed(capture(*f.processor));
 f.processor->releaseResources();reason(*f.processor,ObservedMidiReason::unprepared);f.processor->processBlock(a,midi);reason(*f.processor,ObservedMidiReason::unprepared);
 for(double rate:{0.,7999.,384001.,std::numeric_limits<double>::quiet_NaN()}){
   f.prepare();Fixture control;control.prepare();const auto saved=encoded(*f.processor);const auto latency=f.processor->getLatencySamples();
   juce::AudioBuffer<float> expected(2,512);f.processor->prepareToPlay(rate,512);f.processor->processBlock(a,midi);control.processor->processBlock(expected,midi);
   timed(capture(*f.processor));CHECK(encoded(*f.processor)==saved&&f.processor->getLatencySamples()==latency);
   bool exact=true;for(int ch=0;ch<2;++ch)for(int frame=0;frame<512;++frame)exact&=std::bit_cast<std::uint32_t>(a.getSample(ch,frame))==std::bit_cast<std::uint32_t>(expected.getSample(ch,frame));CHECK(exact);
   CHECK(a.getMagnitude(0,512)>0);Fixture cold;juce::AudioBuffer<float> rejected(2,512);cold.processor->prepareToPlay(rate,512);cold.processor->processBlock(rejected,midi);reason(*cold.processor,ObservedMidiReason::unprepared);
 }
 for(int size:{0,65537}){
   f.prepare();Fixture control;control.prepare();const auto saved=encoded(*f.processor);const auto latency=f.processor->getLatencySamples();
   juce::AudioBuffer<float> expected(2,512);f.processor->prepareToPlay(48000,size);f.processor->processBlock(a,midi);control.processor->processBlock(expected,midi);
   timed(capture(*f.processor));CHECK(encoded(*f.processor)==saved&&f.processor->getLatencySamples()==latency);
   bool exact=true;for(int ch=0;ch<2;++ch)for(int frame=0;frame<512;++frame)exact&=std::bit_cast<std::uint32_t>(a.getSample(ch,frame))==std::bit_cast<std::uint32_t>(expected.getSample(ch,frame));CHECK(exact);
   CHECK(a.getMagnitude(0,512)>0);Fixture cold;juce::AudioBuffer<float> rejected(2,512);cold.processor->prepareToPlay(48000,size);cold.processor->processBlock(rejected,midi);reason(*cold.processor,ObservedMidiReason::unprepared);
 }
 for(int invalid=0;invalid<4;++invalid){f.prepare();
   juce::AudioBuffer<float> bad(invalid==0?1:2,invalid==1?0:invalid==2?65537:512);
   std::array<float,513> overlapping{};std::array<float*,2> pointers{overlapping.data(),overlapping.data()+1};juce::AudioBuffer<float> alias(pointers.data(),2,512);
   f.processor->processBlock(invalid==3?alias:bad,midi);reason(*f.processor,ObservedMidiReason::invalidBlock);}
 f.prepare();f.head.at(0);f.processor->processBlock(a,midi);auto old=capture(*f.processor);CHECK(old.noteCount==1);
 CHECK(f.processor->setGroupIntent(group()));std::uint64_t edit=0,reset=0;auto d=project(*f.processor,&edit,&reset);auto pattern=d.pattern;pattern.noteCount=1;pattern.notes[0]={0,.25,90,80,1};
 CHECK(f.processor->applyPatternEdit(pattern,edit));f.head.at(512);midi.clear();f.processor->processBlock(a,midi);auto retained=capture(*f.processor);
 CHECK(retained.noteCount==1&&retained.notes[0].pitch==60&&retained.notes[0].held);CHECK(retained.windowBeats==old.windowBeats);
 auto saved=encoded(*f.processor);const auto authority=f.processor->groupLinkView();const auto musical=f.processor->aiMusicalContext();const auto ctx=context(*f.processor);
 d=project(*f.processor,&edit,&reset);f.head.at(1024);f.processor->processBlock(a,midi);capture(*f.processor);std::uint64_t e2=0,r2=0;
 CHECK(project(*f.processor,&e2,&r2)==d&&e2==edit&&r2==reset);CHECK(encoded(*f.processor)==saved);
 CHECK(f.processor->groupLinkView()==authority&&f.processor->aiMusicalContext()==musical);CHECK(context(*f.processor)==ctx);
 auto corrupt=saved;static_cast<char*>(corrupt.getData())[25]^=1;CHECK(!f.processor->restoreProjectState(corrupt.getData(),int(corrupt.getSize())));
 CHECK(encoded(*f.processor)==saved);CHECK(f.processor->groupLinkView()==authority);f.head.at(1536);f.processor->processBlock(a,midi);CHECK(capture(*f.processor).noteCount==1);
 CHECK(recall(*f.processor,d));f.head.at(2048);f.processor->processBlock(a,midi);reason(*f.processor,ObservedMidiReason::noNotes);
 note(midi,62);f.head.at(2560);f.processor->processBlock(a,midi);CHECK(capture(*f.processor).noteCount==1);project(*f.processor,&edit);
 CHECK(f.processor->setPlaybackMode(PlaybackMode::internalPattern,edit));f.head.at(3072);midi.clear();f.processor->processBlock(a,midi);reason(*f.processor,ObservedMidiReason::internalMode);
 project(*f.processor,&edit);CHECK(f.processor->setPlaybackMode(PlaybackMode::externalMidi,edit));f.head.at(3584);f.processor->processBlock(a,midi);reason(*f.processor,ObservedMidiReason::noNotes);
 Fixture limit,over;limit.prepare();over.prepare();juce::AudioBuffer<float> b(2,512);juce::MidiBuffer records,extra;
 records.ensureSize(70000);note(records);for(int n=1;n<8192;++n)raw(records,0xb0,74,1,0);extra=records;raw(extra,0xb0,74,1,0);
 limit.processor->processBlock(a,records);over.processor->processBlock(b,extra);CHECK(capture(*limit.processor).noteCount==1);reason(*over.processor,ObservedMidiReason::inputCapacity);
 CHECK(limit.processor->performanceInputStatus()==PerformanceInputStatus::ready&&over.processor->performanceInputStatus()==PerformanceInputStatus::ready);
 for(int ch=0;ch<2;++ch)for(int frame=0;frame<512;++frame)CHECK(std::bit_cast<std::uint32_t>(a.getSample(ch,frame))==std::bit_cast<std::uint32_t>(b.getSample(ch,frame)));
 Fixture performance;performance.prepare();records.clear();for(int n=0;n<8192;++n)note(records,60);CHECK(performance.processor->enqueueKeyboardNote(true,61,100,1));
 performance.processor->processBlock(a,records);CHECK(performance.processor->performanceInputStatus()==PerformanceInputStatus::midiCapacity);reason(*performance.processor,ObservedMidiReason::inputCapacity);
 performance.head.at(512);records.clear();note(records,63);performance.processor->processBlock(a,records);CHECK(capture(*performance.processor).noteCount==1);
 // Invalid raw offsets/messages reach the owner but do not become notes.
 Fixture invalidRaw;invalidRaw.prepare();records.clear();note(records,50,-1);note(records,51,512);raw(records,0xb0,74,1,0);invalidRaw.processor->processBlock(a,records);reason(*invalidRaw.processor,ObservedMidiReason::noNotes);
}
thread_local bool tracking=false;thread_local int allocations=0,frees=0;
int __cdecl hook(int kind,void*,size_t,int,long,const unsigned char*,int)
{if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE)++frees;}return 1;}
void audio_and_control_safety()
{
 section("audio_and_control_safety");
 for(auto mode:{PlaybackMode::externalMidi,PlaybackMode::internalPattern}){Fixture captured,uncaptured;auto d=project(*captured.processor);d.mode=mode;
   d.pattern.noteCount=1;d.pattern.notes[0]={0,2,48,100,1};d.group=group();CHECK(recall(*captured.processor,d)&&recall(*uncaptured.processor,d));captured.prepare();uncaptured.prepare();
   const auto saved=encoded(*captured.processor),other=encoded(*uncaptured.processor);std::uint64_t edit=0,reset=0;project(*captured.processor,&edit,&reset);
   const auto link=captured.processor->groupLinkView();const auto ctx=context(*captured.processor);juce::AudioBuffer<float> a(2,512),b(2,512);juce::MidiBuffer m;m.ensureSize(256);note(m,48);
   ObservedMidiSnapshot observed;auto previous=_CrtSetAllocHook(hook);allocations=frees=0;float peak=0;
   for(int n=0;n<16;++n){captured.head.at(n*512);uncaptured.head.at(n*512);tracking=true;
     captured.processor->processBlock(a,m);captured.processor->captureObservedMidi(observed);uncaptured.processor->processBlock(b,m);tracking=false;
     CHECK(validateObservedMidiSnapshot(observed));if(mode==PlaybackMode::externalMidi)CHECK(observed.noteCount==1);else CHECK(observed.reason==ObservedMidiReason::internalMode);
     for(int ch=0;ch<2;++ch)for(int frame=0;frame<512;++frame){CHECK(std::bit_cast<std::uint32_t>(a.getSample(ch,frame))==std::bit_cast<std::uint32_t>(b.getSample(ch,frame)));peak=std::max(peak,std::abs(a.getSample(ch,frame)));}m.clear();}
   _CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);CHECK(peak>0);CHECK(encoded(*captured.processor)==saved&&encoded(*uncaptured.processor)==other);
   std::uint64_t e2=0,r2=0;CHECK(project(*captured.processor,&e2,&r2)==d&&e2==edit&&r2==reset);CHECK(captured.processor->groupLinkView()==link);CHECK(context(*captured.processor)==ctx);
   std::cout<<"AUDIO mode="<<int(mode)<<" allocations="<<allocations<<" frees="<<frees<<" exact samples=16384\n";}
 Fixture concurrent;concurrent.prepare(64);juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;note(midi,60);concurrent.processor->processBlock(audio,midi);midi.clear();
 std::atomic<bool> go=false,done=false,good=true;std::thread producer([&]{while(!go.load())std::this_thread::yield();
   for(int n=1;n<=200;++n){concurrent.head.at(n*64);concurrent.processor->processBlock(audio,midi);}done=true;});
 go=true;for(int n=0;n<1000;++n){auto s=capture(*concurrent.processor);CHECK(s.noteCount<=1);if(s.noteCount)CHECK(s.notes[0].pitch==60&&s.notes[0].channel==1&&s.notes[0].held);}
 producer.join();CHECK(done&&good);auto final=capture(*concurrent.processor);CHECK(final.noteCount==1&&final.notes[0].held);CHECK(close(final.notes[0].durationBeats,201*64/24000.));
}
}
namespace {
void exact_pad_is_not_observed_host_midi(){
 auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());Head head;p->setPlayHead(&head);auto d=std::make_unique<ProjectDocument>();std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(*d,edit));
 RackDesign design;design.name="Observed exact pad";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};const juce::Uuid rack;RackOperation operation;operation.design=design;operation.newInstance=rack;operation.allowSlotReuse=true;operation.allocation={{0,0}};operation.destination={RackRole::synth,masterChain,0};CHECK(p->applyRackOperation(operation,edit)==RackCandidateStatus::ready);
 const std::vector<float> pcm(48000,.001f);drums::AssetPtr canonical;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,canonical,assetError));std::uint64_t ticket=0;DrumCommitError error;CHECK(p->prepareDrumAsset(pcm,1,ticket,error));drums::DrumPad metadata;metadata.asset=canonical->id();DrumPadTarget target;CHECK(p->captureDrumPadTarget(rack,0,0,target,error));CHECK(p->assignPreparedDrumPad(target,ticket,metadata,{},error));p->prepareToPlay(48000,512);
 CHECK(p->captureDrumPerformanceTarget(rack,0,0,target,error));CHECK(p->enqueueDrumPad(target,127));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p->processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);reason(*p,ObservedMidiReason::noNotes);
 head.at(512);CHECK(p->enqueueDrumPad(target,64));note(midi,36,17);p->processBlock(audio,midi);const auto observed=capture(*p);CHECK(observed.noteCount==1&&observed.notes[0].pitch==36&&observed.notes[0].velocity==100);p->setPlayHead(nullptr);p->releaseResources();
}
}
int main(int argc,char** argv)
{
 juce::ScopedJuceInitialiser_GUI gui;FILETIME c{},e{},k{},u{};GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER time{};time.LowPart=c.dwLowDateTime;time.HighPart=c.dwHighDateTime;
 wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(module)<<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<'\n';
 for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
 external_callback_observation();lifecycle_capacity_and_isolation();audio_and_control_safety();CHECK(sections==3);
 exact_pad_is_not_observed_host_midi();
 std::cout<<"checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;
}
