#include "InstrumentProcessor.h"
#include "DrumBundle.h"
#include <iostream>
#include <memory>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <limits>
#include <juce_dsp/juce_dsp.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#ifndef NDEBUG
namespace airack {
struct RoutedDrumTestAccess {
    static auto snapshot(const RoutedSynthEngine& e){return e.drumTestSnapshot();}
    static bool prepareSpace(RoutedSynthEngine& e,double rate,int frames,RenderQuality quality,const space::SpaceActivationOptions& options){return e.prepareWithPipelineLaunch(rate,frames,quality,{},nullptr,options);}
    static bool serial(RoutedSynthEngine& e,double rate,int frames,RenderQuality quality){return e.prepareWithPipelineLaunch(rate,frames,quality,{},[](detail::DspPipeline::Entry,detail::DspPipeline*,unsigned)->std::thread{throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));});}
};
struct InstrumentDrumTestAccess {
    using Point=InstrumentProcessor::DrumTestPoint;
    static void hook(InstrumentProcessor& p,void* context,void (*callback)(void*,Point)){const std::lock_guard lock(p.restoreMutex);p.drumTestContext=context;p.drumTestHook=callback;}
    static void clearHistory(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.clearHistoryOwned();}
    static void drain(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.drainDrumFeedbackOwned();}
    static auto preparedId(InstrumentProcessor& p,std::uint64_t ticket){const std::lock_guard lock(p.restoreMutex);drums::AssetId id{};for(const auto& s:p.drumOwnership->preparations_)if(s.ticket==ticket&&s.asset)id=s.asset->id();return id;}
    static bool enqueue(InstrumentProcessor& p,const DrumPadTarget& target,std::uint8_t velocity,std::uint8_t channel,std::uint32_t offset){return p.enqueueDrumPadAtOffset(target,velocity,channel,offset);}
    static int queued(const InstrumentProcessor& p){return p.performance->drumFifo.getNumReady();}
    static auto keyboardGeneration(const InstrumentProcessor& p){return p.performance->generation.load();}
    static auto word(const InstrumentProcessor& p){return p.structuralGeneration.load();}
    static auto ack(const InstrumentProcessor& p){return p.drumAcknowledgedSource.load();}
    static auto revision(const InstrumentProcessor& p){return p.revision.load();}
    static auto targets(const InstrumentProcessor& p){drums::TargetTokens out{};for(unsigned n=0;n<64;++n)out[n]=p.drumTargetTokens[n].load();return out;}
    static auto image(const InstrumentProcessor& p){return RoutedDrumTestAccess::snapshot(p.engine);}
    static auto events(const InstrumentProcessor& p){return p.performance->drumEvents();}
    static auto assignments(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->current_->borrowed.samples;}
    static bool prepareSpace(InstrumentProcessor& p,const space::SpaceActivationOptions& options,RenderQuality quality=RenderQuality::realtime){return RoutedDrumTestAccess::prepareSpace(p.engine,48000,512,quality,options);}
    static void structuralHook(void (*callback)(int)){InstrumentProcessor::setStructuralTestHook(callback);}
    static constexpr auto stageBytes(){return sizeof(PerformanceMidi::Event)*PerformanceMidi::capacity;}
    static constexpr auto expiryBytes(){return sizeof(std::bitset<65>);}
};
}
#endif

namespace {
using namespace airack;
using namespace airack::drums;
using Access=InstrumentDrumTestAccess;
int checks=0,failures=0,sections=0;const char* current="";
bool check(bool value,const char* text,int line){++checks;if(!value){++failures;std::cerr<<"FAIL "<<current<<':'<<line<<' '<<text<<'\n';}return value;}
#define CHECK(x) check(bool(x),#x,__LINE__)
void section(const char* name){current=name;++sections;std::cout<<"DRUM_PERFORMANCE_SECTION "<<name<<'\n';}
std::size_t parameter(const juce::String& id){const auto registry=parameterRegistry();for(std::size_t n=0;n<registry.size();++n)if(registry[n].id==id)return n;CHECK(false);return 0;}
void physical(SoundDocument& d,const juce::String& id,float value){const auto n=parameter(id);const auto& p=parameterRegistry()[n];juce::NormalisableRange<float> range(p.minimum,p.maximum,p.kind==ParameterKind::continuous?0.f:1.f,p.skew);d.values[n]=range.convertTo0to1(value);}
RackOperation place(const juce::Uuid& id,unsigned bank){RackDesign design;design.name="Exact drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};RackOperation op;op.design=design;op.newInstance=id;op.allowSlotReuse=true;op.allocation={{0,static_cast<std::uint8_t>(bank)}};op.destination={RackRole::synth,masterChain,static_cast<std::uint8_t>(bank)};return op;}
struct Head:juce::AudioPlayHead {
    bool available=true;HostPatternTiming h;
    juce::Optional<PositionInfo> getPosition() const override {if(!available)return {};PositionInfo p;p.setIsPlaying(h.playing);p.setIsLooping(h.looping);if(h.bpm)p.setBpm(*h.bpm);if(h.ppqPosition)p.setPpqPosition(*h.ppqPosition);if(h.samplePosition)p.setTimeInSamples(*h.samplePosition);if(h.loop)p.setLoopPoints(LoopPoints{h.loop->startBeats,h.loop->endBeats});return p;}
};
struct Fixture {
    std::unique_ptr<InstrumentProcessor> owner=std::make_unique<InstrumentProcessor>();
    std::array<juce::Uuid,4> racks;unsigned banks=1;
    explicit Fixture(unsigned count=1):banks(count){
        for(unsigned b=0;b<count;++b){auto d=snapshot();CHECK(p().applyRackOperation(place(racks[b],b),d.second)==RackCandidateStatus::ready);}
        edit([&](SoundDocument& d){d.values[0]=1;physical(d,"output.gain_db",0);physical(d,"output.protection",0);for(unsigned b=0;b<count;++b){const auto prefix="synth."+juce::String(b+1)+".";physical(d,prefix+"enabled",1);physical(d,prefix+"level",1);physical(d,prefix+"pan",0);}});
        Access::clearHistory(p());
    }
    ~Fixture(){p().setPlayHead(nullptr);p().releaseResources();}
    InstrumentProcessor& p(){return *owner;}
    auto snapshot(){auto d=std::make_unique<ProjectDocument>();std::uint64_t edit=0;CHECK(p().tryProjectSnapshot(*d,edit));return std::pair{std::move(d),edit};}
    template<class F>void edit(F&& change){auto d=snapshot();change(d.first->sound);CHECK(p().applySoundEdit(d.first->sound,SoundEditOrigin::manual,d.second));}
    void mode(PlaybackMode mode){auto d=snapshot();CHECK(p().setPlaybackMode(mode,d.second));}
    DrumPadTarget target(unsigned bank=0,unsigned pad=0,bool editTarget=false){DrumPadTarget target;DrumCommitError error;CHECK(editTarget?p().captureDrumPadTarget(racks[bank],0,static_cast<std::uint8_t>(pad),target,error):p().captureDrumPerformanceTarget(racks[bank],0,static_cast<std::uint8_t>(pad),target,error));return target;}
    void assign(unsigned bank,unsigned pad,float marker,std::size_t frames=48000,unsigned channels=1){
        std::vector<float> pcm(frames*channels,marker);assign(bank,pad,pcm,channels);
    }
    void assign(unsigned bank,unsigned pad,const std::vector<float>& pcm,unsigned channels){
        std::uint64_t ticket=0;DrumCommitError error;CHECK(p().prepareDrumAsset(pcm,channels,ticket,error));DrumPad metadata;metadata.asset=Access::preparedId(p(),ticket);metadata.name="marker";
        CHECK(p().assignPreparedDrumPad(target(bank,pad,true),ticket,metadata,{},error));
    }
    void prepare(int block=512,double rate=48000,RenderQuality quality=RenderQuality::realtime){p().setNonRealtime(quality==RenderQuality::higher);p().control(758).setValue(quality==RenderQuality::higher?1.f:0.f);p().prepareToPlay(rate,block);}
};
juce::AudioBuffer<float> process(InstrumentProcessor& p,int frames=64,const juce::MidiBuffer& input={}){juce::AudioBuffer<float> audio(2,frames);audio.clear();auto midi=input;p.processBlock(audio,midi);return audio;}
juce::MidiBuffer note(int pitch=36,int velocity=127,int channel=1,int offset=0){juce::MidiBuffer out;out.addEvent(juce::MidiMessage::noteOn(channel,pitch,juce::uint8(velocity)),offset);return out;}
bool constant(const juce::AudioBuffer<float>& audio,float value,int first=0,int count=-1,float epsilon=2e-8f){if(count<0)count=audio.getNumSamples()-first;for(int ch=0;ch<2;++ch)for(int n=first;n<first+count;++n)if(std::abs(audio.getSample(ch,n)-value)>epsilon)return false;return true;}
void stop(InstrumentProcessor& p){juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::midiStop(),0);(void)process(p,1,midi);Access::drain(p);}
void exact_one_pad_duplicate_mappings(){
    section("exact_one_pad_duplicate_mappings");Fixture f(4);
    for(unsigned b=0;b<4;++b)for(unsigned pad=0;pad<16;++pad)f.assign(b,pad,pad==1?-.002f:.001f,4096);
    f.edit([](SoundDocument& d){for(unsigned b=0;b<4;++b)for(unsigned pad=0;pad<16;++pad){d.values[drumParameterStart+b*drumParametersPerSlot+pad*drumParametersPerPad+7]=36.f/127;d.values[drumParameterStart+b*drumParametersPerSlot+pad*drumParametersPerPad+8]=1.f/16;}});f.prepare();
    for(unsigned b=0;b<4;++b)for(unsigned pad=0;pad<16;++pad)for(unsigned velocity:{1u,64u,127u}){stop(f.p());CHECK(f.p().enqueueDrumPad(f.target(b,pad),std::uint8_t(velocity)));CHECK(constant(process(f.p(),17),(pad==1?-.002f:.001f)*velocity/127.f));CHECK(Access::image(f.p()).hits[b]==1);}
    stop(f.p());CHECK(constant(process(f.p(),17,note()),4*(15*.001f-.002f)));for(const auto hits:Access::image(f.p()).hits)CHECK(hits==16);
    stop(f.p());auto d=f.snapshot();d.first->pattern.noteCount=1;d.first->pattern.notes[0]={0,.01,36,127,1};CHECK(f.p().applyPatternEdit(d.first->pattern,d.second));f.mode(PlaybackMode::internalPattern);Head head;head.h={true,120.,0.,false,{},0};f.p().setPlayHead(&head);CHECK(constant(process(f.p(),17),4*(15*.001f-.002f)));
    Fixture stereo;stereo.assign(0,0,.001f);std::vector<float> words(48000*2);for(unsigned n=0;n<48000;++n){words[n*2]=-.002f;words[n*2+1]=.003f;}stereo.assign(0,1,words,2);stereo.edit([](SoundDocument& sound){sound.values[drumParameterStart+drumParametersPerPad+7]=36.f/127;sound.values[drumParameterStart+8]=sound.values[drumParameterStart+drumParametersPerPad+8]=1.f/16;});stereo.prepare();
    CHECK(stereo.p().enqueueDrumPad(stereo.target(0,1),64));auto asymmetric=process(stereo.p(),32);for(int n=0;n<32;++n){CHECK(std::abs(asymmetric.getSample(0,n)-(-.002f*64/127))<2e-8f);CHECK(std::abs(asymmetric.getSample(1,n)-(.003f*64/127))<2e-8f);}stop(stereo.p());asymmetric=process(stereo.p(),32,note());for(int n=0;n<32;++n){CHECK(std::abs(asymmetric.getSample(0,n)-(-.001f))<2e-8f);CHECK(std::abs(asymmetric.getSample(1,n)-(.004f))<2e-8f);}
    stop(stereo.p());CHECK(stereo.p().enqueueDrumPad(stereo.target(),127));(void)process(stereo.p(),1);CHECK(Access::enqueue(stereo.p(),stereo.target(0,1),127,1,1));asymmetric=process(stereo.p(),242);for(int n=0;n<242;++n){const double old=n==0?.001:n<=240?.001*(241-n)/240.:0.;CHECK(std::abs(asymmetric.getSample(0,n)-(old+(n>=1?-.002:0.)))<2e-8);CHECK(std::abs(asymmetric.getSample(1,n)-(old+(n>=1?.003:0.)))<2e-8);}CHECK(Access::image(stereo.p()).hits[0]==1);
}
void performance_capture_keeps_locked_racks_playable(){
    section("performance_capture_keeps_locked_racks_playable");Fixture f;f.assign(0,0,.001f);f.assign(0,1,-.002f);f.prepare();auto original=f.target(0,1);
    f.edit([](SoundDocument& d){d.synthLocks[0]=true;for(unsigned n=0;n<drumParametersPerPad;++n)d.controlLocks[drumParameterStart+drumParametersPerPad+n]=true;});
    CHECK(f.target(0,1).token==original.token);DrumPadTarget rejectedTarget;DrumCommitError error;CHECK(!f.p().captureDrumPadTarget(f.racks[0],0,1,rejectedTarget,error)&&error==DrumCommitError::locked);CHECK(!f.p().clearDrumPad(original,error)&&error==DrumCommitError::locked);
    std::uint64_t ticket=0;CHECK(f.p().prepareDrumAsset(std::vector<float>(32,.003f),1,ticket,error));DrumPad metadata;metadata.asset=Access::preparedId(f.p(),ticket);CHECK(!f.p().assignPreparedDrumPad(original,ticket,metadata,{},error)&&error==DrumCommitError::locked);f.p().discardPreparedDrumAsset(ticket);
    CHECK(f.p().enqueueDrumPad(original,127));CHECK(constant(process(f.p(),17),-.002f));stop(f.p());CHECK(constant(process(f.p(),17,note(37)),-.002f));f.edit([](SoundDocument& d){d.synthLocks[0]=false;d.controlLocks={};});CHECK(f.target(0,1).token==original.token);
}
void captured_selection_and_stale_component(){
    section("captured_selection_and_stale_component");Fixture f(2);f.assign(0,0,.001f);f.assign(1,0,-.002f);f.prepare();auto captured=f.target();(void)f.target(1);CHECK(f.p().enqueueDrumPad(captured,127));CHECK(constant(process(f.p(),8),.001f));stop(f.p());
    auto d=f.snapshot();RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=f.racks[0];juce::Uuid fresh;const std::array<RackOperation,2> operations{remove,place(fresh,0)};CHECK(f.p().applyRackOperations(operations,d.second)==RackCandidateStatus::ready);CHECK(!f.p().enqueueDrumPad(captured,127));f.racks[0]=fresh;f.assign(0,0,.003f);f.edit([](SoundDocument& sound){physical(sound,"synth.1.enabled",1);physical(sound,"synth.1.level",1);physical(sound,"synth.1.pan",0);});(void)process(f.p(),512);CHECK(f.p().enqueueDrumPad(f.target(),127));CHECK(constant(process(f.p(),512),.003f));
}
void real_invalidation_scopes(){
    section("real_invalidation_scopes");
    for(unsigned kind=0;kind<9;++kind){std::cout<<"INVALIDATION_KIND "<<kind<<'\n';Fixture f;f.assign(0,0,.001f);f.assign(0,1,-.002f);f.prepare();const auto target=f.target();CHECK(f.p().enqueueDrumPad(target,127));
        if(kind==0){f.p().control(drumModeParameterStart).setValue(0);f.p().control(drumModeParameterStart).setValue(1);auto d=f.snapshot();}
        if(kind==1){auto d=f.snapshot();RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=f.racks[0];juce::Uuid fresh;const std::array<RackOperation,2> ops{remove,place(fresh,0)};CHECK(f.p().applyRackOperations(ops,d.second)==RackCandidateStatus::ready);f.racks[0]=fresh;}
        if(kind==2){auto d=f.snapshot();RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=f.racks[0];CHECK(f.p().applyRackOperation(remove,d.second)==RackCandidateStatus::ready);d=f.snapshot();CHECK(f.p().applyRackEdit({RackEditKind::addSynth,0,masterChain,0,EffectKind::drive,true},d.second)==RackEditResult::applied);f.p().control(drumModeParameterStart).setValue(1);(void)f.snapshot();}
        if(kind==3){juce::MemoryBlock bytes;f.p().getStateInformation(bytes);CHECK(f.p().restoreProjectState(bytes.getData(),int(bytes.getSize())));}
        if(kind==4){f.assign(0,0,.003f);CHECK(f.p().undoSoundEdit());CHECK(f.p().redoSoundEdit());}
        if(kind==5){std::uint64_t ticket=0;DrumCommitError error;CHECK(f.p().prepareDrumAsset(std::vector<float>(48000,.001f),1,ticket,error));DrumPad metadata;metadata.asset=Access::preparedId(f.p(),ticket);metadata.name="same canonical asset, new assignment";CHECK(f.p().assignPreparedDrumPad(f.target(0,0,true),ticket,metadata,{},error));}
        if(kind==6){DrumCommitError error;CHECK(f.p().clearDrumPad(f.target(0,0,true),error));}
        if(kind==7){f.p().releaseResources();f.prepare();}
        if(kind==8){f.edit([](SoundDocument& sound){sound.values[drumModeParameterStart]=0;});f.edit([](SoundDocument& sound){sound.values[drumModeParameterStart]=1;});}
        CHECK(constant(process(f.p(),32),0));CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::staleTarget);if(kind!=7)CHECK(!f.p().enqueueDrumPad(target,127));
    }
    Fixture f;f.assign(0,0,.001f);f.prepare();const auto target=f.target();CHECK(f.p().enqueueDrumPad(target,127));f.assign(0,1,-.002f);f.edit([](SoundDocument& d){d.values[0]=.75f;d.values[drumParameterStart+3]=.1f;d.values[drumParameterStart+4]=.9f;d.values[drumParameterStart+2]=.75f;d.values[drumParameterStart+8]=.125f;d.name="same captured pad";});auto d=f.snapshot();d.first->pattern.noteCount=1;d.first->pattern.notes[0]={0,.01,99,100,1};CHECK(f.p().applyPatternEdit(d.first->pattern,d.second));CHECK(f.target().token==target.token);CHECK(process(f.p(),32).getMagnitude(0,32)>0);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::ready);
    stop(f.p());CHECK(f.p().enqueueDrumPad(target,127));f.p().control(drumParameterStart+3).setValue(.9f);f.p().control(drumParameterStart+4).setValue(.1f);CHECK(f.target().token==target.token);(void)process(f.p(),32);CHECK(f.p().drumPerformanceStatus()!=DrumPerformanceStatus::staleTarget);
}
struct Pause {
    std::atomic<unsigned> entered{0};std::atomic<bool> released{false},expired{false};
    void wait(){entered.fetch_add(1);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!released.load()){if(std::chrono::steady_clock::now()>deadline){expired=true;return;}std::this_thread::yield();}}
    bool observe(){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!entered.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();return entered.load()!=0;}
    ~Pause(){released=true;}
};
thread_local InstrumentProcessor* casWriter=nullptr;thread_local unsigned casWrites=0;
void casWrite(int point){if(point==3&&casWriter&&!casWrites++){casWriter->control(drumModeParameterStart).setValue(0);casWriter->control(drumModeParameterStart).setValue(1);}}
void foreign_epoch_and_cas_races(){
    section("foreign_epoch_and_cas_races");Fixture f;f.assign(0,0,.001f);f.prepare();auto target=f.target();CHECK(f.p().enqueueDrumPad(target,127));f.p().control(drumModeParameterStart).setValue(0);f.p().control(drumModeParameterStart).setValue(1);CHECK(Access::word(f.p())!=Access::ack(f.p()));CHECK(constant(process(f.p(),32,note()),.001f));CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::staleTarget);stop(f.p());
    target=f.target();const auto before=Access::targets(f.p());std::uint64_t ticket=0;DrumCommitError error;CHECK(f.p().prepareDrumAsset(std::vector<float>(32,-.002f),1,ticket,error));DrumPad pad;pad.asset=Access::preparedId(f.p(),ticket);casWriter=&f.p();casWrites=0;Access::structuralHook(casWrite);CHECK(!f.p().assignPreparedDrumPad(target,ticket,pad,{},error));Access::structuralHook(nullptr);casWriter=nullptr;CHECK(casWrites>=1);CHECK(Access::targets(f.p())==before);(void)f.target();CHECK(Access::targets(f.p())!=before);CHECK(constant(process(f.p(),1,note()),.001f));stop(f.p());f.p().discardPreparedDrumAsset(ticket);
    struct Context{InstrumentProcessor* p;unsigned count=0;}context{&f.p()};Access::hook(f.p(),&context,[](void* raw,Access::Point point){auto& c=*static_cast<Context*>(raw);if(point==Access::Point::afterSourceCas&&!c.count++){c.p->control(drumModeParameterStart).setValue(0);c.p->control(drumModeParameterStart).setValue(1);}});
    f.assign(0,1,.003f);Access::hook(f.p(),nullptr,nullptr);CHECK(context.count>=1);CHECK(Access::word(f.p())!=Access::ack(f.p()));CHECK(constant(process(f.p(),1,note()),.001f));
}
void cutoff_precedes_authority_read(){
    section("cutoff_precedes_authority_read");
    for(auto point:{Access::Point::afterDrumCutoff,Access::Point::afterDrumAuthority}){Fixture f;f.assign(0,0,.001f);f.prepare();Pause gate;const auto audioId=std::make_shared<std::thread::id>();struct Context{Pause* gate;Access::Point point;std::shared_ptr<std::thread::id> audio;}context{&gate,point,audioId};
        Access::hook(f.p(),&context,[](void* raw,Access::Point p){auto& c=*static_cast<Context*>(raw);if(p==c.point&&std::this_thread::get_id()==*c.audio)c.gate->wait();});
        auto audio=std::make_unique<juce::AudioBuffer<float>>();std::thread worker([&]{*audioId=std::this_thread::get_id();*audio=process(f.p(),16);});const bool entered=gate.observe();CHECK(entered);
        if(entered){f.assign(0,0,-.002f);CHECK(f.p().enqueueDrumPad(f.target(),127));}gate.released=true;worker.join();Access::hook(f.p(),nullptr,nullptr);CHECK(!gate.expired);CHECK(constant(*audio,0));CHECK(Access::queued(f.p())==1);CHECK(constant(process(f.p(),16),-.002f));CHECK(Access::queued(f.p())==0);CHECK(constant(process(f.p(),16),-.002f));CHECK(Access::image(f.p()).hits[0]==1);
    }
}
void transient_read_defers_without_prefix(){
    section("transient_read_defers_without_prefix");Fixture f;f.assign(0,0,.001f);f.assign(0,1,-.002f);f.prepare();const auto a=f.target(),b=f.target(0,1);CHECK(f.p().enqueueDrumPad(a,127));CHECK(Access::enqueue(f.p(),a,127,1,17));CHECK(f.p().enqueueDrumPad(b,127));
    auto d=f.snapshot();d.first->sound.values[0]=.75f;Pause gate;std::atomic<unsigned> attempts{0};struct Context{Pause* gate;std::atomic<unsigned>* attempts;}context{&gate,&attempts};
    Access::hook(f.p(),&context,[](void* raw,Access::Point point){auto& c=*static_cast<Context*>(raw);if(point==Access::Point::afterSourceCas)c.gate->wait();if(point==Access::Point::beforeDrumReadAttempt)++*c.attempts;});bool edited=false;
    std::thread control([&]{edited=f.p().applySoundEdit(d.first->sound,SoundEditOrigin::manual,d.second);});CHECK(gate.observe());CHECK(Access::revision(f.p())&1u);
    for(int n=0;n<3;++n){CHECK(constant(process(f.p(),64),0));CHECK(Access::queued(f.p())==3);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::deferred);}CHECK(attempts==6);gate.released=true;control.join();Access::hook(f.p(),nullptr,nullptr);CHECK(edited&&!gate.expired);
    CHECK(f.p().enqueueDrumPad(a,127));CHECK(constant(process(f.p(),64),0.f));CHECK(Access::image(f.p()).hits[0]==3);CHECK(Access::queued(f.p())==0);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::missedOffset);(void)process(f.p(),64);CHECK(Access::image(f.p()).hits[0]==3);
}
struct SpacePause {
    std::atomic<bool> entered{false},released{false},expired{false};
    static void wait(void* raw,std::uint64_t,space::detail::PreparationControl control) noexcept {auto& gate=*static_cast<SpacePause*>(raw);gate.entered=true;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!gate.released){if(control.isCancelled&&control.isCancelled(control.context))return;if(std::chrono::steady_clock::now()>deadline){gate.expired=true;return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    bool observe(){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!entered&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));return entered;}
    ~SpacePause(){released=true;}
};
void installed_authority_and_same_numeric_adoption(){
    section("installed_authority_and_same_numeric_adoption");Fixture f(2);f.assign(0,0,.001f);f.assign(1,0,.003f);f.prepare();SpacePause gate;CHECK(Access::prepareSpace(f.p(),{SpacePause::wait,&gate}));
    const auto old=f.target();f.assign(0,0,-.002f);auto d=f.snapshot();CHECK(f.p().applyRackEdit({RackEditKind::addEffect,0,masterChain,0,EffectKind::spaceReverb},d.second)==RackEditResult::applied);
    (void)process(f.p(),1);CHECK(gate.observe());const auto current=f.target();CHECK(current.token!=old.token);CHECK(f.p().enqueueDrumPad(current,127));auto audio=process(f.p(),32);CHECK(constant(audio,0));CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::unavailable);CHECK(!f.p().enqueueDrumPad(old,127));
    CHECK(f.p().enqueueDrumPad(f.target(1),127));audio=process(f.p(),32);CHECK(audio.getMagnitude(0,32)>0);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::ready);
    gate.released=true;for(int n=0;n<400&&Access::image(f.p()).pendingValid;++n){(void)process(f.p(),512);std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(!gate.expired);CHECK(!Access::image(f.p()).pendingValid);
    // The full-word reconciliation changes tokens even when final numeric selectors match.
    f.p().control(drumModeParameterStart).setValue(0);f.p().control(drumModeParameterStart).setValue(1);const auto fresh=f.target();CHECK(fresh.token!=current.token);CHECK(f.p().enqueueDrumPad(fresh,127));(void)process(f.p(),512);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::ready);CHECK(Access::image(f.p()).committedIdentity.targets[0]==fresh.token);
    Fixture invalid;invalid.assign(0,0,.001f);invalid.prepare();const auto installed=invalid.target();invalid.assign(0,0,-.002f);invalid.p().control(drumParameterStart+3).setValue(.9f);invalid.p().control(drumParameterStart+4).setValue(.1f);const auto pending=invalid.target();CHECK(pending.token!=installed.token);CHECK(invalid.p().enqueueDrumPad(pending,127));CHECK(constant(process(invalid.p(),64),0));CHECK(invalid.p().drumPerformanceStatus()==DrumPerformanceStatus::unavailable);CHECK(Access::image(invalid.p()).committedIdentity.targets[0]==installed.token);
    // First Drum/Space preparation can fail before sourceAware becomes true.
    SpacePause first;auto melodic=std::make_unique<InstrumentProcessor>();melodic->prepareToPlay(48000,512);CHECK(Access::prepareSpace(*melodic,{SpacePause::wait,&first}));auto project=std::make_unique<ProjectDocument>();std::uint64_t edit=0;CHECK(melodic->tryProjectSnapshot(*project,edit));const juce::Uuid rack;CHECK(melodic->applyRackOperation(place(rack,0),edit)==RackCandidateStatus::ready);
    std::uint64_t ticket=0;DrumCommitError error;CHECK(melodic->prepareDrumAsset(std::vector<float>(48000,.001f),1,ticket,error));DrumPad metadata;metadata.asset=Access::preparedId(*melodic,ticket);DrumPadTarget target;CHECK(melodic->captureDrumPadTarget(rack,0,0,target,error));CHECK(melodic->assignPreparedDrumPad(target,ticket,metadata,{},error));CHECK(melodic->tryProjectSnapshot(*project,edit));CHECK(melodic->applyRackEdit({RackEditKind::addEffect,0,masterChain,0,EffectKind::spaceReverb},edit)==RackEditResult::applied);CHECK(melodic->captureDrumPerformanceTarget(rack,0,0,target,error));CHECK(melodic->enqueueDrumPad(target,127));CHECK(constant(process(*melodic,16),0));CHECK(first.observe());CHECK(melodic->drumPerformanceStatus()==DrumPerformanceStatus::unavailable);CHECK(!Access::image(*melodic).installed[0]);first.released=true;melodic->releaseResources();
}
void fixed_queue_saturation(){
    section("fixed_queue_saturation");Fixture f;f.assign(0,0,.001f);f.prepare();const auto target=f.target();const auto keyboard=Access::keyboardGeneration(f.p());
    for(int n=0;n<64;++n)CHECK(f.p().enqueueDrumPad(target,127));CHECK(Access::queued(f.p())==64);CHECK(!f.p().enqueueDrumPad(target,127));CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::queueFull);CHECK(Access::keyboardGeneration(f.p())==keyboard);CHECK(constant(process(f.p(),1),.032f));CHECK(Access::events(f.p()).size()==64);CHECK(Access::queued(f.p())==0);CHECK(Access::image(f.p()).hits[0]==32);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::queueFull);
    for(int round=0;round<5;++round){stop(f.p());for(int n=0;n<64;++n)CHECK(f.p().enqueueDrumPad(target,1));CHECK(constant(process(f.p(),1),.032f/127));CHECK(Access::queued(f.p())==0);}
    for(auto velocity:{0,128,255})CHECK(!f.p().enqueueDrumPad(target,std::uint8_t(velocity)));for(auto channel:{0,17,255})CHECK(!f.p().enqueueDrumPad(target,127,std::uint8_t(channel)));auto invalid=target;invalid.bank=4;CHECK(!f.p().enqueueDrumPad(invalid,127));invalid=target;invalid.pad=16;CHECK(!f.p().enqueueDrumPad(invalid,127));invalid=target;invalid.token=0;CHECK(!f.p().enqueueDrumPad(invalid,127));invalid=target;++invalid.reset;CHECK(!f.p().enqueueDrumPad(invalid,127));CHECK(Access::queued(f.p())==0);
    stop(f.p());std::atomic<bool> done{false},cancel{false};std::atomic<unsigned> accepted{0};std::thread producer([&]{for(unsigned n=0;n<256&&!cancel;++n){while(!cancel&&!f.p().enqueueDrumPad(target,1))std::this_thread::yield();if(!cancel)++accepted;}done=true;});unsigned callbacks=0,dispatched=0;while(!done||Access::queued(f.p())){(void)process(f.p(),1);dispatched+=unsigned(Access::events(f.p()).size());++callbacks;if(callbacks>10000){cancel=true;break;}}producer.join();CHECK(done&&accepted==256&&dispatched==256&&Access::queued(f.p())==0);CHECK(callbacks<=10000);
    // Callback-start cleanup must retain a concurrent producer queue-full error.
    stop(f.p());for(int n=0;n<64;++n)CHECK(f.p().enqueueDrumPad(target,1));Pause gate;Access::hook(f.p(),&gate,[](void* raw,Access::Point point){if(point==Access::Point::afterDrumCutoff)static_cast<Pause*>(raw)->wait();});std::thread audio([&]{(void)process(f.p(),1);});CHECK(gate.observe());CHECK(!f.p().enqueueDrumPad(target,1));gate.released=true;audio.join();Access::hook(f.p(),nullptr,nullptr);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::queueFull);CHECK(!gate.expired);
}
void raw_channel_controls_both_modes(){
    section("raw_channel_controls_both_modes");
    for(auto mode:{PlaybackMode::externalMidi,PlaybackMode::internalPattern})for(int controller:{120,123})for(int offset:{0,15,16,17,511,512,513,1023}){
        Fixture f(2);f.assign(0,0,.001f);f.assign(1,0,-.002f);auto d=f.snapshot();d.first->pattern.noteCount=1;d.first->pattern.notes[0]={0,1,36,127,1};CHECK(f.p().applyPatternEdit(d.first->pattern,d.second));f.mode(mode);f.prepare(512);Head head;head.h={true,120.,0.,false,{},0};f.p().setPlayHead(&head);CHECK(f.p().enqueueDrumPad(f.target(),127,1));CHECK(f.p().enqueueDrumPad(f.target(1),127,2));CHECK(f.p().enqueueKeyboardNote(true,36,64,1));auto a=process(f.p(),1,note());CHECK(a.getMagnitude(0,1)>0);head.h.ppqPosition=1./24000;head.h.samplePosition=1;
        juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::controllerEvent(1,controller,0),offset);a=process(f.p(),1024,midi);CHECK(Access::image(f.p()).hits[0]==0);CHECK(Access::image(f.p()).hits[1]>=1);CHECK(constant(a,-.002f,offset,1024-offset));
    }
    for(bool ccFirst:{false,true}){Fixture f;f.assign(0,0,.001f);f.prepare();juce::MidiBuffer midi;const auto cc=juce::MidiMessage::controllerEvent(1,123,0);if(ccFirst)midi.addEvent(cc,17);midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(127)),17);if(!ccFirst)midi.addEvent(cc,17);CHECK(constant(process(f.p(),32,midi),ccFirst?.001f:0.f,17,15));}
}
struct ClockStep{HostPatternTiming h;int frames=1;bool stop=false;};
HostPatternTiming clock(double ppq,std::optional<std::int64_t> sample,std::optional<PatternLoopRange> loop={},double bpm=120){return {true,bpm,ppq,loop.has_value(),loop,sample};}
void clockCase(const char* name,const std::vector<ClockStep>& steps){
    std::cout<<"CLOCK_CASE "<<name<<'\n';auto scheduler=std::make_unique<PatternScheduler>();auto events=std::make_unique<PatternEventBuffer>();CHECK(scheduler->prepare(48000,65536));
    for(const auto& step:steps){CHECK(scheduler->processBlock(step.h,step.frames,PlaybackMode::internalPattern,*events)==PatternPlaybackStatus::playing);if(!CHECK(events->drumStopAtStart==step.stop)){std::cerr.precision(17);std::cerr<<"clock="<<name<<" ppq="<<step.h.ppqPosition.value_or(-999)<<" sample="<<step.h.samplePosition.value_or(-999)<<" frames="<<step.frames<<'\n';}}
    for(int prepared:{64,512}){Fixture f;f.assign(0,0,.001f,480000);f.mode(PlaybackMode::internalPattern);f.prepare(prepared);Head head;f.p().setPlayHead(&head);CHECK(f.p().enqueueDrumPad(f.target(),127));bool stopped=false;
        for(const auto& step:steps){head.h=step.h;auto audio=process(f.p(),step.frames);stopped|=step.stop;const bool valid=constant(audio,stopped?0.f:.001f);if(!CHECK(valid)){std::cerr.precision(17);std::cerr<<"processor_clock="<<name<<" prepared="<<prepared<<" ppq="<<step.h.ppqPosition.value_or(-999)<<" sample="<<step.h.samplePosition.value_or(-999)<<" frames="<<step.frames<<" magnitude="<<audio.getMagnitude(0,step.frames)<<'\n';}}
    }
}
void stop_seek_missing_mode_and_wrap(){
    section("stop_seek_missing_mode_and_wrap");constexpr double S=24000;
    for(auto sample:{0LL,24000LL})clockCase("unit-loop-reset",{{clock(.99,23760,PatternLoopRange{0,1}),240,false},{clock(0,sample,PatternLoopRange{0,1}),64,false}});
    for(auto sample:{-24000LL,0LL})clockCase("negative-loop",{{clock(-.01,-240,PatternLoopRange{-1,0}),240,false},{clock(-1,sample,PatternLoopRange{-1,0}),64,false}});
    for(auto sample:{68LL,1508LL})clockCase("multiple-wrap",{{clock(228/S,228,PatternLoopRange{0,.01}),1280,false},{clock(68/S,sample,PatternLoopRange{0,.01}),64,false}});
    std::cout.precision(17);const double F=(240.25/S)*S,span=4*F;std::cout<<"ACTUAL_FRACTIONAL F="<<F<<" span="<<span<<" floor="<<std::floor(span)<<" ceil="<<std::ceil(span)<<'\n';
    for(auto sample:{17LL,978LL,16LL,18LL,15LL})clockCase("fractional-four-wrap",{{clock(.0095,228,PatternLoopRange{0,240.25/S}),750,false},{clock(17/S,sample,PatternLoopRange{0,240.25/S}),64,sample==18||sample==15}});
    for(auto sample:{18LL,17LL,19LL})clockCase("integral-four-wrap-control",{{clock(228/S,228,PatternLoopRange{0,.01}),750,false},{clock(18/S,sample,PatternLoopRange{0,.01}),64,sample!=18}});
    for(auto sample:{0LL,1LL})clockCase("sequential-240.4-ambiguous",{{clock(228/S,228,PatternLoopRange{0,240.4/S}),13,false},{clock(.6/S,1,PatternLoopRange{0,240.4/S}),240,false},{clock(.2/S,sample,PatternLoopRange{0,240.4/S}),64,false}});
    for(double b:{100.2,-100.2})clockCase("translated-sequential-240.4",{{clock((b+228)/S,std::int64_t(std::floor(b+228+.5)),PatternLoopRange{b/S,(b+240.4)/S}),13,false},{clock((b+.6)/S,std::int64_t(std::floor(b+.6+.5)),PatternLoopRange{b/S,(b+240.4)/S}),240,false},{clock((b+.2)/S,std::int64_t(std::floor(b+.2+.5)),PatternLoopRange{b/S,(b+240.4)/S}),64,false}});
    for(double length:{24000.25,24000.5})for(auto sample:{1LL,24001LL})clockCase("large-fractional-loop",{{clock(.99,23760,PatternLoopRange{0,length/S}),241,false},{clock((length==24000.25?.75:.5)/S,sample,PatternLoopRange{0,length/S}),64,false}});
    for(auto sample:{12063LL,12065LL})clockCase("incompatible-one-frame",{{clock(.5,12000,PatternLoopRange{0,1}),64,false},{clock(.5+64/S,sample,PatternLoopRange{0,1}),64,true}});
    for(auto next:{.6,.4})clockCase("within-loop-seek",{{clock(.5,12000,PatternLoopRange{0,1}),64,false},{clock(next,std::int64_t(next*S),PatternLoopRange{0,1}),64,true}});
    clockCase("changed-range",{{clock(.5,12000,PatternLoopRange{0,1}),64,false},{clock(.5+64/S,12064,PatternLoopRange{0,2}),64,true}});
    for(auto sample:{64LL,-95936LL})clockCase("preroll",{{clock(0,0,PatternLoopRange{4,8}),64,false},{clock(64/S,sample,PatternLoopRange{4,8}),64,sample!=64}});
    for(auto sample:{96000LL,0LL})clockCase("phrase-wrap",{{clock(3.99,95760),240,false},{clock(4,sample),64,sample==0}});
    clockCase("optional-samples",{{clock(0,0,PatternLoopRange{0,1}),64,false},{clock(64/S,{},PatternLoopRange{0,1}),64,false},{clock(128/S,128,PatternLoopRange{0,1}),64,false},{clock(192/S,192,PatternLoopRange{0,1}),64,false}});
    for(double prior:{120.,93.}){const double spb=60*48000/prior;clockCase("previous-tempo-loop",{{clock(228/spb,228,PatternLoopRange{0,240.4/spb},prior),13,false},{clock(.6/spb,1,PatternLoopRange{0,240.4/spb},prior),240,false},{clock(.2/spb,0,PatternLoopRange{0,240.4/spb},prior==120?93:120),64,false}});}
    for(unsigned kind=0;kind<10;++kind){std::cout<<"DISCONTINUITY_KIND "<<kind<<'\n';Fixture f;f.assign(0,0,.001f);f.mode(PlaybackMode::internalPattern);f.prepare();Head head;head.h=clock(0,0,PatternLoopRange{0,1});f.p().setPlayHead(&head);CHECK(f.p().enqueueDrumPad(f.target(),127));CHECK(constant(process(f.p(),64),.001f));head.h=clock(64/S,64,PatternLoopRange{0,1});
        if(kind==0)head.h.playing=false;if(kind==1)head.h.bpm.reset();if(kind==2)head.h.ppqPosition.reset();if(kind==3)head.h.loop.reset();if(kind==4)head.available=false;if(kind==5)head.h.bpm=0.;if(kind==6){f.mode(PlaybackMode::externalMidi);f.mode(PlaybackMode::internalPattern);}if(kind==7){auto d=f.snapshot();CHECK(f.p().createPattern(2,d.second));d=f.snapshot();CHECK(f.p().createPattern(1,d.second));}if(kind==8){juce::AudioBuffer<float> bad(1,64);juce::MidiBuffer empty;f.p().processBlock(bad,empty);}if(kind==9){auto d=f.snapshot();d.first->pattern.noteCount=1;d.first->pattern.notes[0]={0,.01,99,100,1};CHECK(f.p().applyPatternEdit(d.first->pattern,d.second));}
        CHECK(constant(process(f.p(),64),0));CHECK(f.p().enqueueDrumPad(f.target(),127));if(kind<=5){CHECK(constant(process(f.p(),64),.001f));}
    }
    Fixture order;order.assign(0,0,.001f);order.assign(0,1,-.002f);auto d=order.snapshot();d.first->pattern.noteCount=1;d.first->pattern.notes[0]={1,.1,37,127,1};CHECK(order.p().applyPatternEdit(d.first->pattern,d.second));order.mode(PlaybackMode::internalPattern);order.prepare();Head head;head.h=clock(0,0);order.p().setPlayHead(&head);CHECK(order.p().enqueueDrumPad(order.target(),127));CHECK(constant(process(order.p(),64),.001f));head.h=clock(1,24000);CHECK(constant(process(order.p(),64),-.002f));CHECK(Access::image(order.p()).hits[0]==1);
}

void raw_stop_and_fx_preservation(){
    section("raw_stop_and_fx_preservation");Fixture f;f.assign(0,0,.001f,32);auto d=f.snapshot();CHECK(f.p().applyRackEdit({RackEditKind::addEffect,0,masterChain,0,EffectKind::delay},d.second)==RackEditResult::applied);f.edit([](SoundDocument& sound){physical(sound,"effect.1.delay.mix",.5f);physical(sound,"effect.1.delay.seconds",.02f);physical(sound,"effect.1.delay.feedback",.5f);});
    f.mode(PlaybackMode::internalPattern);d=f.snapshot();d.first->pattern.noteCount=1;d.first->pattern.notes[0]={0,1,36,127,1};CHECK(f.p().applyPatternEdit(d.first->pattern,d.second));f.prepare();Head head;head.h=clock(0,0);f.p().setPlayHead(&head);(void)process(f.p(),512);head.h=clock(512./24000,512);head.h.playing=false;(void)process(f.p(),512);const auto tail=process(f.p(),512);CHECK(tail.getMagnitude(0,512)>0);CHECK(Access::image(f.p()).hits[0]==0);
    // Repeated paused callbacks keep the generated stop coalesced; delayed history survives.
    auto later=process(f.p(),512);CHECK(later.getMagnitude(0,512)>0);f.mode(PlaybackMode::externalMidi);(void)process(f.p(),512);juce::MidiBuffer raw;raw.addEvent(juce::MidiMessage::midiStop(),17);auto killed=process(f.p(),512,raw);CHECK(constant(killed,0,17));
    Fixture order;order.assign(0,0,-.002f);order.prepare();CHECK(order.p().enqueueDrumPad(order.target(),127));raw.clear();raw.addEvent(juce::MidiMessage::midiStop(),0);CHECK(constant(process(order.p(),64,raw),-.002f));
    order.mode(PlaybackMode::internalPattern);(void)process(order.p(),1);CHECK(order.p().enqueueDrumPad(order.target(),127));CHECK(constant(process(order.p(),1),-.002f));raw.clear();raw.addEvent(juce::MidiMessage::midiStop(),17);CHECK(constant(process(order.p(),64,raw),-.002f));
    for(auto effect:{EffectKind::delay,EffectKind::spaceReverb})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        Fixture paired(2);paired.assign(0,0,.001f,32);paired.edit([](SoundDocument& sound){sound.values[drumModeParameterStart+1]=0;});auto project=paired.snapshot();CHECK(paired.p().applyRackEdit({RackEditKind::addEffect,0,masterChain,0,effect},project.second)==RackEditResult::applied);
        if(effect==EffectKind::delay)paired.edit([](SoundDocument& sound){physical(sound,"effect.1.delay.seconds",.02f);physical(sound,"effect.1.delay.mix",.5f);physical(sound,"effect.1.delay.feedback",.5f);});
        paired.mode(PlaybackMode::internalPattern);project=paired.snapshot();project.first->pattern.noteCount=1;project.first->pattern.notes[0]={0,1,60,100,1};CHECK(paired.p().applyPatternEdit(project.first->pattern,project.second));paired.prepare(512,48000,quality);project=paired.snapshot();RoutedPatch patch;CHECK(makeRoutedPatch(project.first->sound,patch));BorrowedDrumAssignments assignments;const auto borrowed=Access::assignments(paired.p());for(unsigned b=0;b<4;++b)for(unsigned pad=0;pad<16;++pad)assignments[b][pad]=borrowed[b][pad];auto reference=std::make_unique<RoutedSynthEngine>();const auto identity=Access::image(paired.p()).committedIdentity;CHECK(reference->setPatch(patch,assignments,identity));CHECK(reference->prepare(48000,512,quality));Head playing;playing.h=clock(0,0);paired.p().setPlayHead(&playing);CHECK(paired.p().enqueueDrumPad(paired.target(),127));auto actual=process(paired.p(),512);juce::AudioBuffer<float> expected(2,512);auto melodic=note(60,100);const RenderEvent drum{0,0,RenderEventKind::noteOn,0,36,127,1};reference->render(expected,melodic,120.,{},std::span(&drum,1));bool equal=true;
        for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n)equal&=actual.getSample(ch,n)==expected.getSample(ch,n);CHECK(equal);
        playing.h=clock(512./24000,512);playing.h.playing=false;melodic.clear();melodic.addEvent(juce::MidiMessage::noteOff(1,60),0);double tailEnergy=0;
        for(int block=0;block<5;++block){actual=process(paired.p(),512);reference->render(expected,melodic,120.,{},{});equal=true;for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n){equal&=actual.getSample(ch,n)==expected.getSample(ch,n);tailEnergy+=std::abs(actual.getSample(ch,n));}CHECK(equal);melodic.clear();}CHECK(tailEnergy>0);
    }
}
void overflow_atomicity(){
    section("overflow_atomicity");Fixture f;f.assign(0,0,.001f);f.prepare();juce::MidiBuffer midi;midi.ensureSize(8193*9+64);
    for(int n=0;n<8192;++n)midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(1)),0);CHECK(constant(process(f.p(),64,midi),.032f/127));CHECK(f.p().performanceInputStatus()==PerformanceInputStatus::ready);stop(f.p());
    CHECK(f.p().enqueueDrumPad(f.target(),127));CHECK(constant(process(f.p(),1),.001f));midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(1)),0);auto overflow=process(f.p(),1024,midi);CHECK(constant(overflow,0));CHECK(f.p().performanceInputStatus()==PerformanceInputStatus::midiCapacity);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::eventCapacity);CHECK(Access::image(f.p()).hits[0]==0);
    CHECK(f.p().enqueueDrumPad(f.target(),127));midi.clear();for(int n=0;n<8192;++n)midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(1)),0);CHECK(constant(process(f.p(),64,midi),0));CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::eventCapacity);
    CHECK(f.p().enqueueDrumPad(f.target(),127));midi.clear();for(int n=0;n<8192;++n)midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(1)),0);midi.addEvent(juce::MidiMessage::controllerEvent(1,123,0),0);CHECK(constant(process(f.p(),64,midi),0));
    auto performance=std::make_unique<PerformanceMidi>();performance->prepare();performance->beginChunk();for(std::size_t n=0;n<8192;++n){const juce::uint8 data[]{0x90,36,1};performance->stageHost({data,3,0},0);}performance->requestDrumStop();CHECK(!performance->commit());CHECK(performance->drumEvents().size()==1&&performance->drumEvents()[0].kind==RenderEventKind::stopAll);
    auto scheduler=std::make_unique<PatternScheduler>();auto events=std::make_unique<PatternEventBuffer>();CHECK(scheduler->prepare(384000,65536));PatternDocument pattern;pattern.bars=1;pattern.noteCount=maximumPatternNotes;for(auto& n:pattern.notes)n={0,.00001,36,127,1};CHECK(scheduler->setPattern(pattern));const auto status=scheduler->processBlock(clock(0,0,PatternLoopRange{0,1./19200},400),65536,PlaybackMode::internalPattern,*events);CHECK(status==PatternPlaybackStatus::eventCapacity);CHECK(events->drumStopAtStart);
    // Literal old melodic timeline is independent of the new drum overflow policy.
    Fixture actual(2);actual.assign(0,0,.001f);actual.assign(0,1,-.002f);actual.edit([](SoundDocument& sound){physical(sound,"synth.1.pan",-1);physical(sound,"synth.2.pan",1);sound.values[drumModeParameterStart+1]=0;});actual.mode(PlaybackMode::internalPattern);auto project=actual.snapshot();project.first->pattern.noteCount=maximumPatternNotes;for(auto& n:project.first->pattern.notes)n={0,1,36,1,1};CHECK(actual.p().applyPatternEdit(project.first->pattern,project.second));actual.prepare(512);project=actual.snapshot();RoutedPatch patch;CHECK(makeRoutedPatch(project.first->sound,patch));const auto borrowed=Access::assignments(actual.p());BorrowedDrumAssignments assignments;for(unsigned b=0;b<4;++b)for(unsigned pad=0;pad<16;++pad)assignments[b][pad]=borrowed[b][pad];auto reference=std::make_unique<RoutedSynthEngine>();CHECK(reference->setPatch(patch,assignments,Access::image(actual.p()).committedIdentity));CHECK(reference->prepare(48000,512));Head head;head.h=clock(0,0,PatternLoopRange{0,1./24000});actual.p().setPlayHead(&head);CHECK(actual.p().enqueueKeyboardNote(true,70,100,2));CHECK(actual.p().enqueueDrumPad(actual.target(),127));auto warm=process(actual.p(),1);juce::MidiBuffer literal;for(unsigned n=0;n<maximumPatternNotes;++n)literal.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(1)),0);literal.addEvent(juce::MidiMessage::noteOn(2,70,juce::uint8(100)),0);juce::AudioBuffer<float> expected(2,1);reference->render(expected,literal,120.,{},{});bool melodicEqual=warm.getSample(1,0)==expected.getSample(1,0);const bool setup=warm.getMagnitude(0,1)>0&&actual.p().patternPlaybackStatus()==PatternPlaybackStatus::playing&&Access::image(actual.p()).hits[0]==32&&melodicEqual;std::cout<<"PROCESSOR_PATTERN_OVERFLOW setup_active_small_callback="<<setup<<'\n';if(!CHECK(setup))return;
    head.h=clock(0,1,PatternLoopRange{0,1./24000});CHECK(actual.p().enqueueKeyboardNote(true,71,90,2));CHECK(actual.p().enqueueDrumPad(actual.target(0,1),127));CHECK(Access::enqueue(actual.p(),actual.target(0,1),127,1,513));juce::MidiBuffer host;for(int offset:{0,17,512,513,1023})host.addEvent(juce::MidiMessage::noteOn(1,37,juce::uint8(127)),offset);const auto failed=process(actual.p(),1024,host);bool silent=true;double melodicEnergy=0;literal.clear();literal.addEvent(juce::MidiMessage::noteOff(1,36),0);literal.addEvent(juce::MidiMessage::noteOn(2,71,juce::uint8(90)),0);expected.setSize(2,512);for(int chunk=0;chunk<2;++chunk){reference->render(expected,literal,120.,{},{});for(int n=0;n<512;++n){silent&=std::abs(failed.getSample(0,chunk*512+n))<2e-8f;melodicEqual&=failed.getSample(1,chunk*512+n)==expected.getSample(1,n);melodicEnergy+=std::abs(expected.getSample(1,n));}literal.clear();}const auto hits=Access::image(actual.p()).hits[0];ObservedMidiSnapshot observed;actual.p().captureObservedMidi(observed);
    std::cout<<"PROCESSOR_PATTERN_OVERFLOW no_prefix_or_later_chunk="<<silent<<" remaining_hits="<<hits<<" melodic_held_and_new_equal="<<melodicEqual<<" melodic_energy="<<melodicEnergy<<" melodic_status="<<int(actual.p().performanceInputStatus())<<" drum_status="<<int(actual.p().drumPerformanceStatus())<<" observed_reason="<<int(observed.reason)<<'\n';CHECK(silent);CHECK(hits==0);CHECK(melodicEqual&&melodicEnergy>0);CHECK(actual.p().patternPlaybackStatus()==PatternPlaybackStatus::eventCapacity);CHECK(actual.p().performanceInputStatus()==PerformanceInputStatus::ready);CHECK(actual.p().drumPerformanceStatus()==DrumPerformanceStatus::eventCapacity);CHECK(observed.reason==ObservedMidiReason::internalMode&&observed.noteCount==0);CHECK(Access::queued(actual.p())==0);
    head.h=clock(0,1025,PatternLoopRange{0,1./24000});auto recovery=process(actual.p(),1);CHECK(recovery.getSample(0,0)>=0);CHECK(Access::image(actual.p()).hits[0]==32);
}
RoutedPatch kit(unsigned banks=1){RoutedPatch patch;patch.synth.masterLevel=1;for(unsigned b=0;b<banks;++b){if(b)CHECK(editRackLayout(patch.layout,{RackEditKind::addSynth,std::uint8_t(b),masterChain,std::uint8_t(b)},patch.layout)==RackEditResult::applied);patch.drumSource[b]=true;patch.synth.layers[b].level=1;patch.synth.layers[b].enabled=true;}return patch;}
void range_offsets_and_equal_order(){
    section("range_offsets_and_equal_order");
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(int block:{1,7,64,512}){
        Fixture f;std::vector<float> sparse(1024,0);sparse[0]=.001f;sparse[3]=-.002f;sparse[17]=.003f;f.assign(0,0,sparse,1);f.prepare(block,rate,quality);
        std::vector<int> offsets{0,1,15,16,17,63,64,511,512,513,65535};for(auto offset:offsets)CHECK(Access::enqueue(f.p(),f.target(),127,1,std::uint32_t(offset)));auto out=process(f.p(),65536);CHECK(Access::queued(f.p())==0);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::ready);
        // Independent direct MIDI reference has exactly the selected sparse sample and literal physical offsets.
        auto patch=kit();BorrowedDrumAssignments assigned{};AssetPtr asset;AssetError error;CHECK(makeSampleAsset(sparse,1,asset,error));assigned[0][0]=asset.get();RoutedSynthEngine reference;CHECK(reference.setPatch(patch,assigned));CHECK(reference.prepare(rate,512,quality));bool equal=true;
        for(int first=0;first<65536;first+=512){const int size=std::min(512,65536-first);juce::AudioBuffer<float> expected(2,size);juce::MidiBuffer midi;for(auto offset:offsets)if(offset>=first&&offset<first+size)midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(127)),offset-first);reference.render(expected,midi);for(int ch=0;ch<2;++ch)for(int n=0;n<size;++n)equal&=out.getSample(ch,first+n)==expected.getSample(ch,n);}CHECK(equal);
        stop(f.p());f.prepare(block,rate,quality);CHECK(Access::enqueue(f.p(),f.target(),127,1,64));CHECK(constant(process(f.p(),64),0));CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::missedOffset);
    }
    AssetPtr asset;AssetError error;CHECK(makeSampleAsset(std::vector<float>(4096,.001f),1,asset,error));auto patch=kit();BorrowedDrumAssignments assignments{};assignments[0][0]=asset.get();PublicationIdentity id;id.generation=1;id.targets.fill(101);RoutedSynthEngine engine,reference;CHECK(engine.setPatch(patch,assignments,id));CHECK(reference.setPatch(patch,assignments,id));CHECK(engine.prepare(48000,512));CHECK(reference.prepare(48000,512));juce::AudioBuffer<float> audio(2,64),expected(2,64);juce::MidiBuffer midi=note();
    engine.render(audio,midi,{}, {},{});CHECK(constant(audio,0));reference.render(expected,{}, {},{},{});CHECK(constant(expected,0));
    const RenderEvent valid{101,17,RenderEventKind::exactPad,0,0,127,1};
    for(unsigned kind=0;kind<14;++kind){auto event=valid;std::vector<RenderEvent> events{event};if(kind==0)events[0].hostOffset=-1;if(kind==1)events[0].hostOffset=64;if(kind==2)events[0].bank=4;if(kind==3)events[0].padOrNote=16;if(kind==4)events[0].velocity=0;if(kind==5)events[0].velocity=128;if(kind==6)events[0].channel=0;if(kind==7)events[0].channel=17;if(kind==8)events[0].token=0;if(kind==9)events[0].kind=static_cast<RenderEventKind>(255);if(kind==10)events={valid,{101,16,RenderEventKind::exactPad,0,0,127,1}};if(kind==11)events.resize(8193,valid);if(kind==12){events[0].kind=RenderEventKind::noteOn;events[0].padOrNote=128;}if(kind==13){events[0].kind=RenderEventKind::stopChannel;events[0].channel=0;}
        engine.render(audio,{}, {},{},events);CHECK(constant(audio,0));engine.render(audio,{}, {},{},{});reference.render(expected,{}, {},{},{});CHECK(constant(audio,0));CHECK(constant(expected,0));
    }
    for(bool stopFirst:{false,true}){engine.reset();std::array<RenderEvent,2> events{valid,{0,17,RenderEventKind::stopChannel,0,0,0,1}};if(stopFirst)std::swap(events[0],events[1]);engine.render(audio,{}, {},{},events);CHECK(constant(audio,stopFirst?.001f:0.f,17));}
    engine.reset();const RenderEvent zero{0,0,RenderEventKind::noteOn,0,36,0,1};engine.render(audio,midi,{}, {},std::span(&zero,1));CHECK(constant(audio,0));
}
// A scalar timeline plus the independent accepted Higher FIR is the left-channel oracle.
std::vector<float> spaceBoundaryLeft(RenderQuality quality,int consumed,int offset,bool channelStop,bool firstEligible){
    const int factor=quality==RenderQuality::higher?2:1,total=240*factor;
    const auto fade=[&](int frame){return frame<total?std::max(0.,1.-double(frame+1)/total):std::min(1.,double(frame-total+1)/total);};
    const auto value=[&](int frame){double marker=channelStop&&frame<offset*factor?double(.001f):0.;if(!channelStop&&firstEligible&&frame>=offset*factor)marker+=double(-.002f);for(int onset:{511,512,513})if(frame>=onset*factor)marker+=double(-.002f);return float(marker*fade(consumed+frame));};
    std::vector<float> result(768);
    if(factor==1){for(int n=0;n<768;++n)result[n]=value(n);return result;}
    juce::dsp::Oversampling<float> fir(2);fir.clearOversamplingStages();fir.addOversamplingStage(juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,.08f,-100.f,.08f,-100.f);fir.initProcessing(512);
    const auto latency=fir.getLatencyInSamples()*.5f;const bool delay=latency!=std::floor(latency);std::array<float,2> last{};
    const auto render=[&](int frames,auto signal,float* destination){juce::AudioBuffer<float> host(2,frames);host.clear();auto block=juce::dsp::AudioBlock<float>(host);auto high=fir.processSamplesUp(block);for(int ch=0;ch<2;++ch)for(int n=0;n<frames*2;++n){float sample=ch==0?signal(n):0.f;if(delay)std::swap(sample,last[ch]);high.getChannelPointer(ch)[n]=sample;}fir.processSamplesDown(block);if(destination)std::copy_n(host.getReadPointer(0),frames,destination);};
    render(512,[&](int){return channelStop?.001f:0.f;},nullptr);
    if(consumed)render(consumed/2,[&](int n){return float((channelStop?double(.001f):0.)*fade(n));},nullptr);
    for(int first=0;first<768;first+=512){const int count=std::min(512,768-first);render(count,[&](int n){return value(first*2+n);},result.data()+first);}
    return result;
}
void actual_space_install_boundaries(){
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(int prepared:{64,512})for(int fence:{16,64})for(int relative:{-1,0,1})for(bool channelStop:{false,true}){
        const int factor=quality==RenderQuality::higher?2:1;SpacePause subjectGate,referenceGate;Fixture subject(2),reference(2);
        for(auto* fixture:{&subject,&reference}){
            fixture->assign(0,0,.001f,48000);fixture->assign(1,0,.003f,48000);fixture->edit([](SoundDocument& sound){physical(sound,"synth.1.pan",-1);physical(sound,"synth.2.pan",1);});auto project=fixture->snapshot();CHECK(fixture->p().applyRackEdit({RackEditKind::addEffect,0,1,0,EffectKind::delay},project.second)==RackEditResult::applied);fixture->edit([](SoundDocument& sound){physical(sound,"effect.1.delay.mix",.5f);physical(sound,"effect.1.delay.seconds",.001f);physical(sound,"effect.1.delay.feedback",0);});fixture->prepare(prepared,48000,quality);CHECK(Access::prepareSpace(fixture->p(),{SpacePause::wait,fixture==&subject?&subjectGate:&referenceGate},quality));CHECK(fixture->p().enqueueDrumPad(fixture->target(1),127,2));if(fixture==&subject&&channelStop)CHECK(fixture->p().enqueueDrumPad(fixture->target(),127,1));(void)process(fixture->p(),16384);
            project=fixture->snapshot();CHECK(fixture->p().applyRackEdit({RackEditKind::addEffect,1,0,0,EffectKind::spaceReverb},project.second)==RackEditResult::applied);fixture->edit([](SoundDocument& sound){physical(sound,"effect.2.space_reverb.mix",0);});fixture->assign(0,0,-.002f,48000);(void)process(fixture->p(),1);
        }
        CHECK(subjectGate.observe()&&referenceGate.observe());const auto oldSubject=Access::image(subject.p()).committedIdentity.targets[0],newSubject=subject.target().token;CHECK(oldSubject!=newSubject);CHECK(Access::image(subject.p()).pendingValid);subjectGate.released=true;referenceGate.released=true;
        std::array<int,2> polls{},remaining{};int index=0;
        for(auto* fixture:{&subject,&reference}){
            // Even the longest admitted wait keeps the one-second markers active.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!Access::image(fixture->p()).installPending&&polls[index]<4096&&std::chrono::steady_clock::now()<deadline){(void)process(fixture->p(),1);++polls[index];std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            auto image=Access::image(fixture->p());if(!CHECK(image.installPending&&image.previousValid&&image.transitionRemaining>factor*fence)){subjectGate.released=true;referenceGate.released=true;return;}
            const int firstEligible=(image.transitionRemaining-1+factor-1)/factor;const int advance=firstEligible-fence;if(advance)(void)process(fixture->p(),advance);image=Access::image(fixture->p());remaining[index]=image.transitionRemaining;CHECK(image.installPending&&image.previousValid);CHECK((remaining[index]-1+factor-1)/factor==fence);CHECK(image.committedIdentity.targets[0]!=fixture->target().token);++index;
        }
        CHECK(remaining[0]==remaining[1]);const auto current=subject.target();const int offset=fence+relative;juce::MidiBuffer stopEvent;if(channelStop)stopEvent.addEvent(juce::MidiMessage::controllerEvent(1,123,0),offset);else CHECK(Access::enqueue(subject.p(),current,127,1,offset));for(int onset:{511,512,513})CHECK(Access::enqueue(subject.p(),current,127,1,onset));const auto actual=process(subject.p(),768,stopEvent),matched=process(reference.p(),768);const bool eligible=relative>=0;const auto expected=spaceBoundaryLeft(quality,240*factor-remaining[0],offset,channelStop,eligible);
        float leftError=0,rightError=0;double rightEnergy=0;for(int n=0;n<768;++n){leftError=std::max(leftError,std::abs(actual.getSample(0,n)-expected[n]));rightError=std::max(rightError,std::abs(actual.getSample(1,n)-matched.getSample(1,n)));rightEnergy+=std::abs(matched.getSample(1,n));}auto image=Access::image(subject.p());const auto expectedHits=std::size_t(3+(!channelStop&&eligible));std::cout<<"ACTUAL_SPACE_FENCE quality="<<int(quality)<<" prepared="<<prepared<<" fence="<<fence<<" offset="<<offset<<" stop="<<channelStop<<" polls="<<polls[0]<<','<<polls[1]<<" remaining="<<remaining[0]<<" old="<<oldSubject<<" current="<<newSubject<<" committed="<<image.committedIdentity.targets[0]<<" hits="<<image.hits[0]<<" expected_hits="<<expectedHits<<" left_error="<<leftError<<" right_error="<<rightError<<'\n';CHECK(leftError<2e-8f);CHECK(rightError<2e-8f&&rightEnergy>0);CHECK(image.hits[0]==expectedHits);CHECK(image.hits[1]==1&&Access::image(reference.p()).hits[1]==1);CHECK(image.committedIdentity.targets[0]==current.token);CHECK(!image.installPending&&!image.pendingValid&&!subjectGate.expired&&!referenceGate.expired);CHECK(subject.p().drumPerformanceStatus()==(!channelStop&&!eligible?DrumPerformanceStatus::unavailable:DrumPerformanceStatus::ready));CHECK(Access::queued(subject.p())==0);
        const auto borrowed=Access::assignments(subject.p());CHECK(image.references[0][0]==borrowed[0][0]);std::size_t references=0;for(unsigned hit=16;hit<48;++hit)if(auto* asset=image.references[0][hit]){++references;CHECK(asset==borrowed[0][0]);}CHECK(references==expectedHits);
    }
}
void source_gate_space_owner_boundaries(){
    section("source_gate_space_owner_boundaries");
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(int block:{64,512})for(unsigned relative=0;relative<3;++relative){
        const int factor=quality==RenderQuality::higher?2:1;const unsigned boundary=quality==RenderQuality::higher?240:239;const unsigned offset=boundary+relative-1;
        Fixture source;source.assign(0,0,.001f);source.edit([](SoundDocument& sound){sound.values[drumModeParameterStart]=0;});source.prepare(block,48000,quality);source.edit([](SoundDocument& sound){sound.values[drumModeParameterStart]=1;});const auto target=source.target();CHECK(Access::enqueue(source.p(),target,127,1,offset));auto audio=process(source.p(),512);
        CHECK(source.p().drumPerformanceStatus()==(relative==0?DrumPerformanceStatus::unavailable:DrumPerformanceStatus::ready));CHECK(Access::image(source.p()).hits[0]==(relative==0?0:1));
        if(quality==RenderQuality::realtime){for(unsigned n=0;n<512;++n){double expected=0;if(relative&&n>=offset&&n>boundary)expected=.001*std::min(1.,double(n-boundary)/240);CHECK(std::abs(audio.getSample(0,int(n))-expected)<2e-8);}}
        Fixture gate;gate.assign(0,0,.001f);gate.prepare(block,48000,quality);CHECK(gate.p().enqueueDrumPad(gate.target(),127));(void)process(gate.p(),512);gate.edit([](SoundDocument& sound){physical(sound,"synth.1.enabled",0);});CHECK(Access::enqueue(gate.p(),gate.target(),127,1,offset));audio=process(gate.p(),512);CHECK(gate.p().drumPerformanceStatus()==DrumPerformanceStatus::unavailable);CHECK(Access::image(gate.p()).hits[0]==0);
        if(quality==RenderQuality::realtime)for(int n=0;n<512;++n)CHECK(std::abs(audio.getSample(0,n)-(.001*std::max(0.,1.-double(n+1)/240)))<2e-8);
        std::cout<<"REAL_OWNER_BOUNDARY quality="<<int(quality)<<" factor="<<factor<<" prepared="<<block<<" physical_onset="<<offset<<" first_admissible="<<boundary<<'\n';
    }
    for(bool serial:{false,true})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        AssetPtr a,b;AssetError error;CHECK(makeSampleAsset(std::vector<float>(48000,.001f),1,a,error));CHECK(makeSampleAsset(std::vector<float>(48000,-.002f),1,b,error));auto patch=kit(2);BorrowedDrumAssignments assignments{};assignments[0][0]=a.get();assignments[1][0]=a.get();PublicationIdentity identity;identity.generation=1;identity.targets.fill(101);auto engine=std::make_unique<RoutedSynthEngine>();CHECK(engine->setPatch(patch,assignments,identity));CHECK(serial?RoutedDrumTestAccess::serial(*engine,48000,512,quality):engine->prepare(48000,512,quality));
        juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;std::array<RenderEvent,2> onsets{{{101,0,RenderEventKind::exactPad,0,0,127,1},{101,0,RenderEventKind::exactPad,1,0,127,2}}};engine->render(audio,midi,120.,{},onsets);CHECK(!engine->drumDispatchUnavailable());auto disabled=patch;disabled.synth.layers[0].enabled=false;CHECK(engine->setPatch(disabled,assignments,identity));
        for(int n=0;n<2;++n){std::array<RenderEvent,4> events{{{101,15,RenderEventKind::exactPad,0,0,127,1},{101,16,RenderEventKind::exactPad,1,0,127,2},{0,17,RenderEventKind::stopChannel,0,0,0,1},{101,511,RenderEventKind::exactPad,0,0,127,1}}};engine->render(audio,midi,120.,{},events);CHECK(engine->drumDispatchUnavailable());CHECK(RoutedDrumTestAccess::snapshot(*engine).hits[1]>=1);}
        auto melodic=disabled;melodic.drumSource[0]=false;CHECK(engine->setPatch(melodic,assignments,identity));for(int n=0;n<2;++n)engine->render(audio,midi,120.,{},{});auto replacement=patch;assignments[0][0]=b.get();identity.targets[0]=202;identity.generation=2;CHECK(engine->setPatch(replacement,assignments,identity));std::array<RenderEvent,3> events{{{202,15,RenderEventKind::exactPad,0,0,127,1},{202,16,RenderEventKind::exactPad,0,0,127,1},{202,17,RenderEventKind::exactPad,0,0,127,1}}};engine->render(audio,midi,120.,{},events);CHECK(engine->drumDispatchUnavailable());engine->render(audio,midi,120.,{},std::span(events).first(1));CHECK(!engine->drumDispatchUnavailable());
    }
    actual_space_install_boundaries();
    // Retain the distinct preparation-pending phase and first-install fast path.
    installed_authority_and_same_numeric_adoption();
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool heapTracking=false;std::atomic<unsigned> heapAllocations{0},heapFrees{0};std::atomic<bool> workerHeapTracking{false};
int __cdecl heapHook(int kind,void*,size_t,int,long,const unsigned char*,int){if(heapTracking||(workerHeapTracking&&detail::DspPipeline::renderingOnThisThread())){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++heapAllocations;if(kind==_HOOK_FREE)++heapFrees;}return 1;}
#endif
void callback_worker_heap_and_owner_exclusion(){
    section("callback_worker_heap_and_owner_exclusion");std::cout<<"SIZES DrumPadPress="<<sizeof(DrumPadPress)<<" RenderEvent="<<sizeof(RenderEvent)<<" PerformanceMidi="<<sizeof(PerformanceMidi)<<" exactQueue="<<65*sizeof(DrumPadPress)<<" exactCutoff="<<64*sizeof(DrumPadPress)<<" expiry="<<Access::expiryBytes()<<" staged="<<Access::stageBytes()<<" projected="<<8192*sizeof(RenderEvent)<<" pattern="<<sizeof(PatternEventBuffer)<<'\n';
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){Fixture f(4);for(unsigned b=0;b<4;++b)f.assign(b,0,.001f);for(unsigned slot=0;slot<2;++slot){auto d=f.snapshot();CHECK(f.p().applyRackEdit({RackEditKind::addEffect,std::uint8_t(slot),masterChain,std::uint8_t(slot),EffectKind::dynamicEq},d.second)==RackEditResult::applied);}f.prepare(512,48000,quality);juce::AudioBuffer<float> audio(2,1024);juce::MidiBuffer midi;midi.ensureSize(8193*9+64);(void)process(f.p(),1024);const auto target=f.target();
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(heapHook);heapAllocations=0;heapFrees=0;
#endif
        for(unsigned kind=0;kind<4;++kind){CHECK(f.p().enqueueDrumPad(target,127));midi.clear();if(kind==1)midi.addEvent(juce::MidiMessage::controllerEvent(1,123,0),17);if(kind==2)midi.addEvent(juce::MidiMessage::midiStop(),513);if(kind==3)for(unsigned n=0;n<8193;++n)midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(1)),0);
#if defined(_MSC_VER) && defined(_DEBUG)
            heapTracking=true;workerHeapTracking=true;
#endif
            f.p().processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
            heapTracking=false;workerHeapTracking=false;
#endif
        }
        CHECK(f.p().enqueueDrumPad(target,127));f.assign(0,0,-.002f);midi.clear();
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=true;workerHeapTracking=true;
#endif
        f.p().processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=false;workerHeapTracking=false;
#endif
        CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::staleTarget);CHECK(f.p().dspWorkerStats().blocks>0);
        const auto fresh=f.target();CHECK(f.p().enqueueDrumPad(fresh,127));auto d=f.snapshot();d.first->sound.values[0]=.75f;Pause gate;Access::hook(f.p(),&gate,[](void* raw,Access::Point point){if(point==Access::Point::afterSourceCas)static_cast<Pause*>(raw)->wait();});bool edited=false;std::thread control([&]{edited=f.p().applySoundEdit(d.first->sound,SoundEditOrigin::manual,d.second);});CHECK(gate.observe());
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=true;workerHeapTracking=true;
#endif
        f.p().processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=false;workerHeapTracking=false;
#endif
        gate.released=true;control.join();Access::hook(f.p(),nullptr,nullptr);CHECK(edited&&!gate.expired);CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::deferred);CHECK(Access::queued(f.p())==1);(void)process(f.p(),1024);
        SpacePause spaceGate;CHECK(Access::prepareSpace(f.p(),{SpacePause::wait,&spaceGate},quality));d=f.snapshot();CHECK(f.p().applyRackEdit({RackEditKind::addEffect,2,masterChain,2,EffectKind::spaceReverb},d.second)==RackEditResult::applied);f.assign(0,0,.003f);(void)process(f.p(),1);CHECK(spaceGate.observe());CHECK(f.p().enqueueDrumPad(f.target(),127));
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=true;workerHeapTracking=true;
#endif
        f.p().processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=false;workerHeapTracking=false;
#endif
        CHECK(f.p().drumPerformanceStatus()==DrumPerformanceStatus::unavailable);spaceGate.released=true;for(unsigned n=0;n<400&&Access::image(f.p()).pendingValid;++n){(void)process(f.p(),512);std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(!spaceGate.expired&&!Access::image(f.p()).pendingValid);CHECK(f.p().enqueueDrumPad(f.target(),127));
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=true;workerHeapTracking=true;
#endif
        f.p().processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
        heapTracking=false;workerHeapTracking=false;
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtSetAllocHook(previous);std::cout<<"CALLBACK_WORKER_HEAP quality="<<int(quality)<<" allocations="<<heapAllocations<<" frees="<<heapFrees<<'\n';CHECK(heapAllocations==0&&heapFrees==0);
#endif
    }
}
}

static bool rawCc123ProcessorRed()
{
    using namespace airack;
    using namespace airack::drums;
    auto project = std::make_unique<ProjectDocument>();
    project->sound.values[drumModeParameterStart] = 1.f;
    project->sound.values[758] = 0.f;
    juce::MemoryBlock compact;
    if (!encodeProjectState(*project, compact)) return false;
    auto numeric = std::make_unique<InstrumentProcessor>();
    if (!numeric->restoreProjectState(compact.getData(), int(compact.getSize()))) return false;
    std::vector<float> pcm(48000, .001f);
    AssetPtr asset;
    AssetError ae;
    if (!makeSampleAsset(pcm, 1, asset, ae)) return false;
    DrumSlots slots {};
    slots[0][0].asset = asset->id();
    DrumDocumentPtr document;
    DrumError de;
    if (!makeDrumDocument(slots, document, de)) return false;
    const std::array<AssetPtr,1> assets {asset};
    DrumBundlePtr bundle;
    const auto input = std::span(static_cast<const std::uint8_t*>(compact.getData()), compact.getSize());
    if (!makeDrumBundle(document, input, assets, bundle, de)) return false;
    juce::MemoryBlock bytes;
    if (!encodeDrumBundle(*bundle, bytes, de)) return false;
    const auto run = [&](int cc) {
        auto p = std::make_unique<InstrumentProcessor>();
        if (!p->restoreProjectState(bytes.getData(), int(bytes.getSize()))) return false;
        p->prepareToPlay(48000, 512);
        if (p->effectiveRenderQuality() != RenderQuality::realtime) return false;
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 36, juce::uint8(127)), 0);
        p->processBlock(audio, midi);
        midi.clear();
        for (int n = 0; n < 20; ++n) p->processBlock(audio, midi);
        const bool active=audio.getMagnitude(0, 512)>0.f;
        std::cout << "setup/active_marker_cc" << cc << '=' << active << '\n';
        if (!active) return false;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, cc, 0), 17);
        p->processBlock(audio, midi);
        if (!(audio.getMagnitude(0, 17) > 0.f)) return false;
        for (int c = 0; c < 2; ++c)
            for (int n = 17; n < 512; ++n)
                if (audio.getSample(c, n) != 0.f) return false;
        return true;
    };
    const bool cc120 = run(120);
    const bool cc123 = run(123);
    std::cout << "RED setup/CC120=" << cc120 << " raw_cc123_kills_drum_at_17=" << cc123 << '\n';
    if(!cc123)std::cerr << "FAIL raw_cc123_kills_drum_at_17\n";
    return cc120 && cc123;
}
int main(int argc,char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    const std::string_view group=argc>1?argv[1]:"";
    if(group=="--drum-performance-core"){exact_one_pad_duplicate_mappings();performance_capture_keeps_locked_racks_playable();captured_selection_and_stale_component();real_invalidation_scopes();foreign_epoch_and_cas_races();cutoff_precedes_authority_read();transient_read_defers_without_prefix();installed_authority_and_same_numeric_adoption();fixed_queue_saturation();}
    else if(group=="--drum-performance-lifecycle"){CHECK(rawCc123ProcessorRed());raw_channel_controls_both_modes();stop_seek_missing_mode_and_wrap();raw_stop_and_fx_preservation();overflow_atomicity();}
    else if(group=="--drum-performance-ranges"){range_offsets_and_equal_order();source_gate_space_owner_boundaries();}
    else if(group=="--drum-performance-heap")callback_worker_heap_and_owner_exclusion();
    else return 2;
    std::cout<<"DRUM_PERFORMANCE checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;
}
