#include "InstrumentProcessor.h"
#include "DspPipeline.h"
#include "PeriodicCaller.h"
#include "ProbePeerMonitor.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack {
struct InstrumentStructuralTestAccess {
    static std::size_t activeNotes(const InstrumentProcessor& p) noexcept {return p.engine.activeNoteCount();}
};
}
namespace {
using namespace airack;
using namespace airack::space::probe;
using Clock=std::chrono::steady_clock;
using Ns=std::int64_t;
using J=juce::var;
constexpr const char* condition="space-product-capacity1-four-mixed-layers16-notes64-routes8-serial-space";
void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}
J object(){return J(new juce::DynamicObject);}
void put(J& value,const char* key,const J& field){value.getDynamicObject()->setProperty(key,field);}
J integer(std::uint64_t n){require(n<=std::uint64_t(std::numeric_limits<juce::int64>::max()),"JSON integer overflow");return J(juce::int64(n));}
Ns tick(){return Ns(Clock::now().time_since_epoch().count());}
double nanos(Ns ticks,std::uint64_t frequency){return double(ticks)*1e9/double(frequency);}
std::uint64_t blocksFor(unsigned secondsNumerator,unsigned secondsDenominator,int rate,int block){
    require(secondsDenominator>0&&rate>0&&block>0,"invalid extent geometry");
    const auto numerator=std::uint64_t(secondsNumerator)*unsigned(rate),denominator=std::uint64_t(secondsDenominator)*unsigned(block);
    return (numerator+denominator-1)/denominator;
}
struct Stats {double mean=0,median=0,p99=0,maximum=0;std::size_t misses=0;};
Stats statistics(const std::vector<double>& ns,double period){
    require(!ns.empty()&&std::isfinite(period)&&period>0,"empty observations/invalid period");
    require(std::all_of(ns.begin(),ns.end(),[](double n){return std::isfinite(n)&&n>=0;}),"invalid observation");
    auto sorted=ns;std::sort(sorted.begin(),sorted.end());const auto count=sorted.size();
    return {std::accumulate(ns.begin(),ns.end(),0.)/double(count),count%2?sorted[count/2]:(sorted[count/2-1]+sorted[count/2])*.5,
        sorted[std::size_t(std::ceil(.99*double(count)))-1],sorted.back(),std::size_t(std::count_if(ns.begin(),ns.end(),[&](double n){return n>period;}))};
}
std::size_t index(const juce::String& id){
    const auto registry=parameterRegistry();for(std::size_t i=0;i<registry.size();++i)if(registry[i].id==id)return i;
    throw std::runtime_error("workload parameter absent: "+id.toStdString());
}
float normalized(std::size_t i,float physical){const auto& field=parameterRegistry()[i];
    require(std::isfinite(physical)&&physical>=field.minimum&&physical<=field.maximum,"workload value outside descriptor range");
    return juce::NormalisableRange<float>(field.minimum,field.maximum,0,field.skew).convertTo0to1(physical);
}
void set(SoundDocument& sound,const juce::String& id,float physical){const auto i=index(id);sound.values[i]=normalized(i,physical);}
SoundDocument workload(bool higher){
    SoundDocument d;d.compatibility=newInstanceKernelCompatibility();
    for(std::uint8_t layer=1;layer<4;++layer)require(editRackLayout(d.layout,{RackEditKind::addSynth,layer,masterChain,layer},d.layout)==RackEditResult::applied,"workload synth layout");
    set(d,"global.voice_mode",0);set(d,"global.master_level",.25f);set(d,"output.gain_db",0);set(d,"output.protection",0);set(d,"output.render_quality",higher?1.f:0.f);
    const std::array<ModTarget,8> targets{ModTarget::level,ModTarget::pan,ModTarget::cutoff,ModTarget::osc1Tune,ModTarget::osc2Detune,ModTarget::osc1Level,ModTarget::resonance,ModTarget::osc1Width};
    for(int layer=0;layer<4;++layer){const bool wt=layer%2!=0;const auto prefix="synth."+juce::String(layer+1)+".";
        set(d,prefix+"enabled",1);set(d,prefix+"engine",wt?1.f:0.f);set(d,prefix+"level",.2f);set(d,prefix+"pan",layer%2?.35f:-.35f);
        for(const auto* bank:{"analog.","wavetable."}){const auto key=prefix+bank;set(d,key+"amp.attack",.005f);set(d,key+"amp.sustain",1);set(d,key+"amp.release",.15f);set(d,key+"filter.cutoff",4000);set(d,key+"filter.resonance",.25f);
            for(int oscillator=1;oscillator<=2;++oscillator){const auto osc=key+"osc."+juce::String(oscillator)+".";if(juce::String(bank)=="analog.")set(d,osc+"waveform",float(Waveform::saw));else{set(d,osc+"table",0);set(d,osc+"position",.7f);}}}
        set(d,prefix+"lfo.1.shape",float(LfoShape::sine));set(d,prefix+"lfo.1.rate_hz",3);set(d,prefix+"lfo.1.synced",0);
        set(d,prefix+"lfo.2.shape",float(LfoShape::triangle));set(d,prefix+"lfo.2.synced",1);set(d,prefix+"lfo.2.beats",.25f);
        for(int route=0;route<16;++route){const auto key=prefix+"modulation."+juce::String(route+1)+".";auto target=targets[std::size_t(route%8)];if(wt&&route%8==7)target=ModTarget::osc1Position;
            set(d,key+"enabled",1);set(d,key+"source",float(1+route%6));set(d,key+"target",float(target));set(d,key+"bank",float(route%8<2?ModBank::common:wt?ModBank::wavetable:ModBank::analog));set(d,key+"amount",route%2?-.05f:.05f);}
    }
    constexpr std::array<float,6> frequency{60,180,540,1620,4860,14580},multiplier{.5f,2,.75f,1.5f,.65f,2.5f},q{.5f,.8f,1.3f,2.1f,3.5f,4};
    for(std::uint8_t slot=0;slot<8;++slot){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.destination=masterChain;add.position=slot;add.effect=EffectKind(16);
        require(editRackLayout(d.layout,add,d.layout)==RackEditResult::applied,"workload Space layout");const auto p="effect."+juce::String(slot+1)+".space_reverb.";
        set(d,p+"size",.8f);set(d,p+"decay_seconds",10);set(d,p+"predelay_ms",float(slot)*31);set(d,p+"brightness",.5f);set(d,p+"character",.7f);set(d,p+"distance",.6f);
        set(d,p+"width",1.5f);set(d,p+"mix",.35f);set(d,p+"duck_range_db",6);set(d,p+"duck_threshold_db",-36);set(d,p+"duck_attack_ms",.1f);set(d,p+"duck_release_ms",2000);
        for(int band=0;band<6;++band){const auto decay=p+"decay."+juce::String(band+1)+".",post=p+"post."+juce::String(band+1)+".";
            set(d,decay+"active",1);set(d,decay+"shape",float(band%3));set(d,decay+"freq_hz",frequency[std::size_t(band)]);set(d,decay+"multiplier",multiplier[std::size_t(band)]);set(d,decay+"q",q[std::size_t(band)]);
            set(d,post+"active",1);set(d,post+"shape",float(band%5));set(d,post+"freq_hz",frequency[std::size_t(band)]);set(d,post+"gain_db",band%2?-6.f:6.f);set(d,post+"bell_q",1.5f);set(d,post+"shelf_q",.8f);set(d,post+"slope",1);}
    }
    require(validateSoundDocument(d),"invalid complete product workload");return d;
}
bool admitted(const SoundDocument& source){
    RoutedPatch patch;if(!makeRoutedPatch(source,patch)||patch.layout.synthCount!=4||patch.layout.effectCount[masterChain]!=8||patch.outputProtection)return false;
    std::size_t routes=0,analog=0,wavetable=0;
    for(const auto& layer:patch.synth.layers){if(!layer.enabled)return false;layer.engine==EngineKind::analog?++analog:++wavetable;
        for(const auto& route:layer.modulation)routes+=route.enabled&&route.source!=ModSource::none&&route.target!=ModTarget::none&&route.amount!=0;}
    if(routes!=64||analog!=2||wavetable!=2)return false;
    for(std::size_t slot=0;slot<8;++slot)if(patch.layout.effects[slot].use!=SlotUse::present||patch.layout.effects[slot].kind!=EffectKind(16)||patch.effects[slot].bypassed||patch.effects[slot].algorithmRevision!=1)return false;
    for(int slot=1;slot<=8;++slot)for(int band=1;band<=6;++band)for(const auto* family:{"decay.","post."})if(parameterPhysicalValue(source.values,index("effect."+juce::String(slot)+".space_reverb."+family+juce::String(band)+".active"))!=1)return false;
    return true;
}
enum class Action {heavy,fast,recall,bypass,unbypass,release,stop,retrigger};
struct Event {Action action;const char* name;std::uint64_t requested=0,actual=0;double operationNs=0;bool executed=false;};
std::array<Event,8> events(int rate,int block){
    std::array<Event,8> result{{{Action::heavy,"heavy-room-decay-post-automation"},{Action::fast,"fast-width-mix-duck-automation"},{Action::recall,"full-recall-retrigger16"},{Action::bypass,"bypass-eight"},
        {Action::unbypass,"unbypass-eight"},{Action::release,"release16"},{Action::stop,"sample-offset-midi-stop"},{Action::retrigger,"retrigger16"}}};
    constexpr std::array<unsigned,8> hundredths{25,50,75,100,125,150,165,180};
    for(std::size_t n=0;n<result.size();++n){auto& event=result[n];event.requested=(std::uint64_t(rate)*hundredths[n]+99)/100;
        event.actual=n<5?(event.requested+unsigned(block)-1)/unsigned(block)*unsigned(block):event.requested;}
    return result;
}
struct Automation {std::size_t index=0;float value=0;};
std::vector<Automation> automation(bool heavy){std::vector<Automation> values;values.reserve(24);
    for(int slot=1;slot<=8;++slot){const auto p="effect."+juce::String(slot)+".space_reverb.";
        const std::array<const char*,3> keys=heavy?std::array<const char*,3>{"size","decay.1.multiplier","post.1.gain_db"}:std::array<const char*,3>{"width","mix","duck_range_db"};
        const std::array<float,3> physical=heavy?std::array<float,3>{.9f,1.75f,-6.f}:std::array<float,3>{1.1f,.4f,2.f};
        for(std::size_t n=0;n<3;++n){const auto i=index(p+keys[n]);values.push_back({i,normalized(i,physical[n])});}}
    return values;
}
void notes(juce::MidiBuffer& midi,bool on,int offset=0){for(int key=48;key<64;++key)midi.addEvent(on?juce::MidiMessage::noteOn(1,key,juce::uint8(100)):juce::MidiMessage::noteOff(1,key),offset);}
struct Head final:juce::AudioPlayHead {PositionInfo info;
    juce::Optional<PositionInfo> getPosition() const override{return info;}
    void advance(std::uint64_t frame,int rate){info.setTimeInSamples(juce::int64(frame));info.setPpqPosition(double(frame)*2/rate);info.setBpm(120);info.setTimeSignature(TimeSignature{4,4});info.setIsPlaying(true);}
};
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool counting=false;std::atomic<std::size_t> allocations{0},frees{0};
int heapHook(int operation,void*,std::size_t,int type,long,const unsigned char*,int){if((counting||detail::DspPipeline::renderingOnThisThread())&&type!=_CRT_BLOCK){if(operation==_HOOK_FREE)++frees;else ++allocations;}return 1;}
struct HeapHook {decltype(_CrtSetAllocHook(nullptr)) previous;
    HeapHook():previous(_CrtSetAllocHook(heapHook)){allocations=frees=0;}
    ~HeapHook(){_CrtSetAllocHook(previous);}
};
struct CountScope {CountScope(){counting=true;}~CountScope(){counting=false;}};
constexpr bool heapAvailable=true;
#else
struct HeapHook {};
struct CountScope {};
constexpr bool heapAvailable=false;
#endif
using SpaceStats=RoutedSynthEngine::SpaceRenderStats;
bool spaceProgress(const SpaceStats& before,const SpaceStats& after,unsigned frames,unsigned factor,bool allowBypass) noexcept {
    for(const auto* state:{&before,&after})if(!state->ready||state->modeCount!=128||state->physicalFactor!=factor
        ||state->converterBackend!=space::runtime::ModalConverterBackend::mirroredAvx2
        ||(state->activeMask!=255&&(!allowBypass||state->activeMask!=0)))return false;
    std::uint64_t common=0;
    for(std::size_t slot=0;slot<8;++slot){if(after.renderedPhysicalFrames[slot]<before.renderedPhysicalFrames[slot])return false;
        const auto delta=after.renderedPhysicalFrames[slot]-before.renderedPhysicalFrames[slot];if(delta>frames||(slot&&delta!=common))return false;common=delta;
        if(before.activeMask==after.activeMask&&delta!=(before.activeMask?frames:0))return false;}
    return true;
}
bool spaceMaskForPhase(unsigned before,unsigned after,std::uint64_t ordinal,int rate,int block,bool lifecycle) noexcept {
    if(!lifecycle)return before==255&&after==255;
    const auto trace=events(rate,block);const auto first=ordinal*unsigned(block),end=first+unsigned(block);
    const auto bypass=trace[3].actual,unbypass=trace[4].actual,fade=std::uint64_t(std::ceil(.005*rate))+unsigned(block);
    if(end<=bypass||first>=unbypass+fade)return before==255&&after==255;
    if(first>=bypass+fade&&end<=unbypass)return before==0&&after==0;
    return (before==0||before==255)&&(after==0||after==255);
}
J frameCounts(const std::array<std::uint64_t,8>& frames){J values(juce::Array<J>{});for(const auto count:frames)values.append(integer(count));return values;}
J spaceMetadata(const SpaceStats& state){return juce::Array<J>{state.ready,int(state.modeCount),int(state.physicalFactor),int(state.activeMask),state.converterBackend?int(*state.converterBackend):-1};}
struct Observation {ScheduleRecord schedule;SpaceStats spaceBefore,spaceAfter;std::uint64_t allocations=0,frees=0;std::size_t activeNotes=0;bool completionLate=false;};
struct Audio {bool finite=true;double peak=0;std::uint64_t nonzero=0,excursions=0,checksum=14695981039346656037ull;};
void scan(Audio& result,const juce::AudioBuffer<float>& audio){for(int ch=0;ch<2;++ch)for(int frame=0;frame<audio.getNumSamples();++frame){const auto x=audio.getSample(ch,frame);const auto bits=std::bit_cast<std::uint32_t>(x);
    for(unsigned byte=0;byte<4;++byte)result.checksum=(result.checksum^((bits>>(8*byte))&255))*1099511628211ull;
    result.finite=result.finite&&std::isfinite(x);if(std::isfinite(x)){result.peak=std::max(result.peak,std::abs(double(x)));result.nonzero+=x!=0;result.excursions+=std::abs(x)>1;}}}
void write(const juce::File& file,const J& value){require(!file.exists(),"refusing evidence overwrite");require(file.replaceWithText(juce::JSON::toString(value,false,17)),"cannot write evidence");}
J runCase(const SoundDocument& source,int rate,int block,bool higher,bool lifecycle,const juce::File& root){
    const auto frequency=nativePeriodicTicksPerSecond();require(frequency>0,"periodic clock unavailable");
    const auto warm=blocksFor(1,4,rate,block),count=blocksFor(lifecycle?2:1,1,rate,block);const auto period=double(block)*1e9/rate;
    auto observations=std::vector<Observation>(std::size_t(count));std::size_t completed=0;std::uint64_t warmCompleted=0,warmAllocations=0,warmFrees=0;
    auto trace=events(rate,block);const auto heavy=automation(true),fast=automation(false);std::array<std::size_t,8> bypass{};for(std::size_t n=0;n<8;++n)bypass[n]=index("effect."+juce::String(int(n+1))+".bypassed");
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    auto setupStart=tick();auto processor=std::make_unique<InstrumentProcessor>(offline);const auto constructNs=nanos(tick()-setupStart,frequency);
    juce::MemoryBlock encoded;require(encodeSoundState(source,encoded),"capacity sound encode");setupStart=tick();require(processor->restoreSoundState(encoded.getData(),int(encoded.getSize())),"capacity initial restore");const auto restoreNs=nanos(tick()-setupStart,frequency);
    processor->setNonRealtime(higher);Head head;processor->setPlayHead(&head);setupStart=tick();processor->prepareToPlay(rate,block);const auto prepareNs=nanos(tick()-setupStart,frequency);
    const auto quality=higher?RenderQuality::higher:RenderQuality::realtime;
    require(processor->selectedRenderQuality()==quality&&processor->effectiveRenderQuality()==quality&&!processor->renderQualityPending(),"actual prepared quality mismatch");
    const auto selectedQuality=int(processor->selectedRenderQuality()),effectiveQuality=int(processor->effectiveRenderQuality()),latency=processor->getLatencySamples();const auto pendingQuality=processor->renderQualityPending(),nonRealtime=processor->isNonRealtime();
    juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(65536);notes(midi,true);
    Audio measuredAudio;std::atomic<bool> done=false;juce::String failure;PeerDetails peer;std::uint64_t scans=0;double maxScan=0;bool periodicOpened=false,periodicClosed=false,heapCalibrated=false;
    std::uint64_t callbacksAllocated=0,callbacksFreed=0;double wallSeconds=0;Ns measurementAnchor=0;std::size_t warmVoices=0;RoutedSynthEngine::DspWorkerStats workers;SpaceStats warmSpaceStart,warmSpaceFinish,measuredSpaceStart,measuredSpaceFinish;
    HeapHook heap;
    std::thread render([&]{
        std::unique_ptr<ConcurrentPeerGuard> guard;PeriodicSession caller;
        try {
            guard=std::make_unique<ConcurrentPeerGuard>(true);guard->force();require(caller.open(nativePeriodicApi(),frequency),"capacity periodic caller setup");periodicOpened=true;
#if defined(_MSC_VER) && defined(_DEBUG)
            allocations=frees=0;{CountScope tracked;void* test=_malloc_dbg(64,_NORMAL_BLOCK,__FILE__,__LINE__);require(test!=nullptr,"heap hook calibration allocation");_free_dbg(test,_NORMAL_BLOCK);}
            heapCalibrated=allocations==1&&frees==1;require(heapCalibrated,"heap hook calibration");allocations=frees=0;
#endif
            warmSpaceStart=processor->spaceRenderStats();ReleaseSequence sequence;require(sequence.initialize(tick(),frequency,unsigned(rate),unsigned(block)),"warm release sequence");
            for(std::uint64_t n=0;n<warm;++n){Ns release=0;require(sequence.tickAt(n+1,release)&&caller.waitTo(release),"warm periodic wait");head.advance(n*unsigned(block),rate);
                const auto before=processor->spaceRenderStats();{CountScope tracked;processor->processBlock(audio,midi);}const auto after=processor->spaceRenderStats();
                require(spaceProgress(before,after,unsigned(block)*(higher?2U:1U),higher?2U:1U,false),"warm eight-Space frame/backend/density contract");midi.clear();++warmCompleted;(*guard)();}
            warmSpaceFinish=processor->spaceRenderStats();measuredSpaceStart=warmSpaceFinish;
            warmVoices=InstrumentStructuralTestAccess::activeNotes(*processor);require(warmVoices==16,"sixteen actual musical voices missing after warmup");
#if defined(_MSC_VER) && defined(_DEBUG)
            warmAllocations=allocations;warmFrees=frees;allocations=frees=0;
#endif
            guard->force();measurementAnchor=tick();require(sequence.initialize(measurementAnchor,frequency,unsigned(rate),unsigned(block)),"measured release sequence");
            std::size_t next=0;
            for(std::uint64_t n=0;n<count;++n){const auto frame=n*unsigned(block);midi.clear();head.advance(warm*unsigned(block)+frame,rate);
                if(lifecycle)while(next<trace.size()&&trace[next].actual<frame+unsigned(block)){auto& event=trace[next++];const auto start=tick();const auto offset=int(event.actual-frame);
                    switch(event.action){case Action::heavy:case Action::fast:for(const auto& edit:event.action==Action::heavy?heavy:fast)processor->control(edit.index).setValueNotifyingHost(edit.value);break;
                        case Action::recall:require(processor->restoreSoundState(encoded.getData(),int(encoded.getSize())),"lifecycle recall refused");notes(midi,true,offset);break;
                        case Action::bypass:case Action::unbypass:for(const auto i:bypass)processor->control(i).setValueNotifyingHost(event.action==Action::bypass?1.f:0.f);break;
                        case Action::release:notes(midi,false,offset);break;case Action::stop:midi.addEvent(juce::MidiMessage::midiStop(),offset);break;case Action::retrigger:notes(midi,true,offset);break;}
                    event.operationNs=nanos(tick()-start,frequency);event.executed=true;
                }
                auto& observation=observations[std::size_t(n)];require(sequence.tickAt(n+1,observation.schedule.releaseTick)&&caller.waitTo(observation.schedule.releaseTick),"measured periodic wait");
#if defined(_MSC_VER) && defined(_DEBUG)
                const auto allocatedBefore=allocations.load(),freedBefore=frees.load();
#endif
                observation.spaceBefore=processor->spaceRenderStats();observation.schedule.startTick=tick();{CountScope tracked;processor->processBlock(audio,midi);}observation.schedule.finishTick=tick();observation.spaceAfter=processor->spaceRenderStats();
#if defined(_MSC_VER) && defined(_DEBUG)
                observation.allocations=allocations.load()-allocatedBefore;observation.frees=frees.load()-freedBefore;
                callbacksAllocated+=observation.allocations;callbacksFreed+=observation.frees;
#endif
                ++completed;require(spaceProgress(observation.spaceBefore,observation.spaceAfter,unsigned(block)*(higher?2U:1U),higher?2U:1U,lifecycle)
                    &&spaceMaskForPhase(observation.spaceBefore.activeMask,observation.spaceAfter.activeMask,n,rate,block,lifecycle),"measured eight-Space frame/backend/density/mask contract");observation.activeNotes=InstrumentStructuralTestAccess::activeNotes(*processor);
                const auto end=frame+unsigned(block);
                if(!lifecycle||end<=trace[5].actual||end>trace[7].actual)require(observation.activeNotes==16,"measured workload lost held or retriggered voices");
                else if(end>trace[6].actual&&end<=trace[7].actual)require(observation.activeNotes==0,"MIDI Stop did not clear workload voices");
                scan(measuredAudio,audio);
                const auto disposition=classifySchedule(sequence,n,observation.schedule);require(disposition!=ScheduleDisposition::invalid,"invalid measured schedule record");observation.completionLate=disposition==ScheduleDisposition::late;
                require(processor->selectedRenderQuality()==quality&&processor->effectiveRenderQuality()==quality&&!processor->renderQualityPending(),"quality changed during measurement");(*guard)();
            }
            measuredSpaceFinish=processor->spaceRenderStats();wallSeconds=nanos(tick()-measurementAnchor,frequency)/1e9;require(!lifecycle||next==trace.size(),"unexecuted lifecycle event");guard->force();workers=processor->dspWorkerStats();
        }catch(const PeerObserved& observed){failure=observed.reason;peer=observed.details;}
        catch(const std::exception& error){failure=error.what();}
        catch(...){failure="unknown profile exception";}
        if(measurementAnchor&&wallSeconds==0)wallSeconds=nanos(tick()-measurementAnchor,frequency)/1e9;
        periodicClosed=caller.close();if(guard){guard->stop();const auto monitoring=guard->statistics();scans=monitoring.scans;maxScan=monitoring.maximumScanSeconds;}done.store(true,std::memory_order_release);
    });
    auto* manager=juce::MessageManager::getInstance();while(!done.load(std::memory_order_acquire))manager->runDispatchLoopUntil(1);render.join();
    SoundDocument after;std::uint64_t revision=0;const bool retained=processor->trySoundSnapshot(after,revision)&&after==source;
    const auto tail=processor->getTailLengthSeconds();setupStart=tick();processor->releaseResources();const auto releaseNs=nanos(tick()-setupStart,frequency);processor->setPlayHead(nullptr);setupStart=tick();processor.reset();const auto destroyNs=nanos(tick()-setupStart,frequency);
    J result=object();put(result,"condition",condition);put(result,"rate",rate);put(result,"host_block",block);put(result,"higher_offline",higher);put(result,"phase",lifecycle?"lifecycle":"steady");
    put(result,"build_heap_audit",heapAvailable);put(result,"acceptance_class",heapAvailable?"DEBUG_HEAP_AUDIT_TIMING_DIAGNOSTIC":higher?"HIGHER_OFFLINE_CAPACITY_OBSERVATION":rate==96000&&block==64?"NORMAL_HISTORICAL_STRESS":"NORMAL_HISTORICAL_QUALIFIED_TARGET");
    put(result,"selected_quality",selectedQuality);put(result,"effective_quality",effectiveQuality);put(result,"quality_pending_initial",pendingQuality);put(result,"host_non_realtime",nonRealtime);put(result,"host_latency_samples",latency);put(result,"observed_tail_seconds",tail);put(result,"active_musical_voices_after_warm",integer(warmVoices));
    put(result,"construct_ns",constructNs);put(result,"initial_restore_ns",restoreNs);put(result,"prepare_ns",prepareNs);put(result,"release_resources_ns",releaseNs);put(result,"destroy_ns",destroyNs);
    put(result,"space_metadata_columns",juce::Array<J>{"ready","mode_count","physical_factor","active_mask","converter_backend_enum"});put(result,"space_required_converter_backend","mirrored_avx2");put(result,"space_required_converter_backend_enum",int(space::runtime::ModalConverterBackend::mirroredAvx2));
    put(result,"space_warm_start_frames",frameCounts(warmSpaceStart.renderedPhysicalFrames));put(result,"space_warm_finish_frames",frameCounts(warmSpaceFinish.renderedPhysicalFrames));put(result,"space_measured_start_frames",frameCounts(measuredSpaceStart.renderedPhysicalFrames));put(result,"space_measured_finish_frames",frameCounts(measuredSpaceFinish.renderedPhysicalFrames));
    put(result,"workload_sound_sha256",juce::SHA256(encoded).toHexString());put(result,"warm_callbacks_expected",integer(warm));put(result,"warm_callbacks_completed",integer(warmCompleted));put(result,"measured_callbacks_expected",integer(count));put(result,"measured_callbacks_completed",integer(completed));
    put(result,"warm_host_frames",integer(warm*unsigned(block)));put(result,"measured_host_frames",integer(count*unsigned(block)));put(result,"measured_wall_seconds",wallSeconds);
    put(result,"period_ns",period);put(result,"cadence","PRO_AUDIO_HIGH_RESOLUTION_RATIONAL_HOST_PERIOD");put(result,"periodic_opened",periodicOpened);put(result,"periodic_closed",periodicClosed);put(result,"retained_sound_unchanged",retained);
    put(result,"failure",failure);put(result,"peer_pid",int(peer.pid));put(result,"peer_name",juce::String(peerImageAscii(peer).data()));put(result,"inventory_site",inventorySiteName(peer.inventorySite));put(result,"peer_scan_count",integer(scans));put(result,"maximum_peer_scan_seconds",maxScan);
    put(result,"peer_parent_pid",int(peer.parentPid));put(result,"peer_image_path",juce::String(peer.imagePath.data()));
    put(result,"peer_creation_filetime",juce::int64(peer.creationFileTime));put(result,"peer_observed_filetime",juce::int64(peer.observedFileTime));
    put(result,"clock_ticks_per_second",integer(frequency));put(result,"measurement_anchor_tick",J(juce::int64(measurementAnchor)));put(result,"prepared_synth_workers",int(workers.prepared));put(result,"parallel_synth_blocks",integer(workers.blocks));put(result,"parallel_internal_frames",integer(workers.internalFrames));
    J raw(juce::Array<J>{}),schedule(juce::Array<J>{}),misses(juce::Array<J>{}),completionMisses(juce::Array<J>{}),dispatch(juce::Array<J>{}),heapRows(juce::Array<J>{}),held(juce::Array<J>{}),spaceStates(juce::Array<J>{}),spaceFrames(juce::Array<J>{});std::vector<double> times;times.reserve(completed);
    std::array<std::uint64_t,8> spaceTotals{};std::size_t spaceFull=0,spaceBypassed=0,spaceTransitions=0;
    std::size_t completionLate=0;for(std::size_t n=0;n<completed;++n){const auto& observed=observations[n];const auto time=nanos(observed.schedule.finishTick-observed.schedule.startTick,frequency);times.push_back(time);raw.append(time);
        schedule.append(juce::Array<J>{J(juce::int64(observed.schedule.releaseTick)),J(juce::int64(observed.schedule.startTick)),J(juce::int64(observed.schedule.finishTick))});
        dispatch.append(nanos(observed.schedule.startTick-observed.schedule.releaseTick,frequency));held.append(int(observed.activeNotes));if(time>period)misses.append(integer(n));
        spaceStates.append(juce::Array<J>{spaceMetadata(observed.spaceBefore),spaceMetadata(observed.spaceAfter)});spaceFrames.append(juce::Array<J>{frameCounts(observed.spaceBefore.renderedPhysicalFrames),frameCounts(observed.spaceAfter.renderedPhysicalFrames)});
        for(std::size_t slot=0;slot<8;++slot)if(observed.spaceAfter.renderedPhysicalFrames[slot]>=observed.spaceBefore.renderedPhysicalFrames[slot])spaceTotals[slot]+=observed.spaceAfter.renderedPhysicalFrames[slot]-observed.spaceBefore.renderedPhysicalFrames[slot];
        if(observed.spaceBefore.activeMask!=observed.spaceAfter.activeMask)++spaceTransitions;else if(observed.spaceBefore.activeMask==255)++spaceFull;else ++spaceBypassed;
        completionLate+=observed.completionLate;if(observed.completionLate)completionMisses.append(integer(n));
        if(heapAvailable)heapRows.append(juce::Array<J>{integer(observed.allocations),integer(observed.frees)});
    }
    put(result,"process_block_wall_ns",raw);put(result,"schedule_columns",juce::Array<J>{"release_tick","start_tick","finish_tick"});put(result,"schedule_ticks",schedule);put(result,"dispatch_lateness_ns",dispatch);put(result,"processing_deadline_miss_ordinals",misses);put(result,"release_completion_deadline_miss_count",integer(completionLate));put(result,"release_completion_deadline_miss_ordinals",completionMisses);put(result,"actual_active_musical_voices_after_callbacks",held);
    put(result,"space_metadata_before_after",spaceStates);put(result,"space_physical_frames_before_after",spaceFrames);
    put(result,"space_total_measured_physical_frames",frameCounts(spaceTotals));put(result,"space_callbacks_full_active",integer(spaceFull));put(result,"space_callbacks_fully_bypassed",integer(spaceBypassed));put(result,"space_callbacks_mask_transition",integer(spaceTransitions));
    put(result,"allocation_observation",heapAvailable?"DEBUG_CRT_CALLBACK_AND_PREPARED_SYNTH_DSP_WORKER_JOBS_EXCLUDES_OFFAUDIO_PREPARATION":"UNAVAILABLE_RELEASE_TIMING_ONLY");
    put(result,"heap_hook_calibrated",heapCalibrated);put(result,"callback_heap_allocations",heapAvailable?integer(callbacksAllocated):J());put(result,"callback_heap_frees",heapAvailable?integer(callbacksFreed):J());put(result,"warm_callback_allocations",heapAvailable?integer(warmAllocations):J());put(result,"warm_callback_frees",heapAvailable?integer(warmFrees):J());put(result,"callback_heap_by_ordinal",heapRows);
    put(result,"raw_finite",measuredAudio.finite);put(result,"raw_peak",measuredAudio.peak);put(result,"nonzero_output_samples",integer(measuredAudio.nonzero));put(result,"full_scale_excursions",integer(measuredAudio.excursions));put(result,"checksum_fnv1a64_float_bits",juce::String::toHexString(juce::int64(measuredAudio.checksum)));
    bool targets=false;if(!times.empty()){const auto stats=statistics(times,period);put(result,"mean_ns",stats.mean);put(result,"median_ns",stats.median);put(result,"p99_ns",stats.p99);put(result,"max_ns",stats.maximum);put(result,"processing_deadline_miss_count",integer(stats.misses));targets=stats.mean<=.75*period&&stats.p99<=period;}
    put(result,"comparative_nominal_targets_met",targets);const bool integrity=failure.isEmpty()&&completed==count&&warmCompleted==warm&&periodicOpened&&periodicClosed&&retained&&measuredAudio.finite&&measuredAudio.nonzero>0&&(!heapAvailable||heapCalibrated)&&(!heapAvailable||(callbacksAllocated==0&&callbacksFreed==0&&warmAllocations==0&&warmFrees==0));put(result,"technical_integrity",integrity);
    J eventRows(juce::Array<J>{});if(lifecycle)for(const auto& event:trace){auto row=object();put(row,"event",event.name);put(row,"requested_host_frame",integer(event.requested));put(row,"actual_host_frame",integer(event.actual));put(row,"offcallback_operation_ns",event.operationNs);put(row,"executed",event.executed);eventRows.append(row);}put(result,"events",eventRows);
    return result;
}
int profile(const char* path){
    require(juce::SystemStats::getEnvironmentVariable("AIRACK_NATIVE_LEASE",{})=="exclusive","capacity profile requires NativeRun exclusive lease");
    const auto argument=juce::String::fromUTF8(path);require(juce::File::isAbsolutePath(argument),"profile requires an absolute output path");const auto root=juce::File(argument);require(!root.exists(),"profile requires fresh output directory");require(root.createDirectory().wasOk(),"profile directory");
    J report=object();put(report,"condition",condition);put(report,"module24_gate_remains_separate_required",true);put(report,"scene","4_MIXED_LAYERS_16_HELD_NOTES_64_EXPECTED_SYNTH_VOICES_64_NONZERO_ROUTES_8_SERIAL_SPACE_48_DECAY_48_POST_BANDS");
    put(report,"rate_order",juce::Array<J>{44100,48000,96000});put(report,"host_block_order",juce::Array<J>{64,512});put(report,"quality_order",juce::Array<J>{"Normal","Higher offline"});put(report,"phase_order",juce::Array<J>{"steady","lifecycle"});put(report,"case_count_expected",24);
    put(report,"warm_seconds",.25);put(report,"steady_seconds",1);put(report,"lifecycle_seconds",2);put(report,"mean_callback_target_fraction",.75);put(report,"p99_callback_target_fraction",1.0);put(report,"all_individual_deadline_misses_retained",true);
    put(report,"space_runtime_contract","128 modes,actual mirroredAvx2,factor1/2,all8physical counters advance every active callback; bypass only within declared event interval plus5ms routing fade and one host block");
    put(report,"required_timing_scope","ALL24_RELEASE_CASES_ZERO_PROCESSING_AND_RELEASE_COMPLETION_MISSES;Higher remains offline;Debug audits heap only");put(report,"caller_scope","Probe's Pro Audio rational periodic caller; prepared HighQoS conventional-priority15 timer helper follows caller CPU inside inherited mask, owns highresolution timer, skips sub-millisecond sleeps and precisely waits up to2ms without MMCSS registration; caller can preempt helper; measured caller blocks then precisely waits at most150us; sampled clock origins, releases/deadlines/caller priority,power,affinity retained; actual processor plus normal message dispatch, no visible editor; not a DAW underrun measurement");put(report,"caller_precise_wait_ns",150000);put(report,"timer_worker_precise_wait_ns",2000000);put(report,"helper_minimum_sleep_100ns",10000);put(report,"helper_policy","local_preemptible_non_mmcss15");put(report,"prepared_non_mmcss_timer_threads",1);put(report,"prepared_mmcss_timer_threads",0);
    put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"heap_audit_available",heapAvailable);write(root.getChildFile("preregistration.json"),report);
    bool integrity=true,requiredTiming=true,allTargets=true;std::size_t count=0;J rows(juce::Array<J>{});
    for(bool higher:{false,true}){const auto source=workload(higher);require(admitted(source),"workload admission");juce::MemoryBlock wire;require(encodeSoundState(source,wire),"capacity state encode");require(root.getChildFile(higher?"higher-offline.soundstate":"normal.soundstate").replaceWithData(wire.getData(),wire.getSize()),"capacity state save");
        for(int rate:{44100,48000,96000})for(int block:{64,512})for(bool lifecycle:{false,true}){auto row=runCase(source,rate,block,higher,lifecycle,root);const auto name="case-"+juce::String(int(++count)).paddedLeft('0',2)+".json";write(root.getChildFile(name),row);
            integrity=integrity&&bool(row["technical_integrity"]);allTargets=allTargets&&bool(row["comparative_nominal_targets_met"]);if(!heapAvailable&&!higher&&!(rate==96000&&block==64))requiredTiming=requiredTiming&&bool(row["comparative_nominal_targets_met"]);
            auto entry=object();put(entry,"file",name);put(entry,"technical_integrity",row["technical_integrity"]);put(entry,"comparative_nominal_targets_met",row["comparative_nominal_targets_met"]);rows.append(entry);
            std::cout<<"SPACE_PRODUCT_CASE "<<count<<" rate="<<rate<<" block="<<block<<" higher="<<higher<<" lifecycle="<<lifecycle<<" integrity="<<bool(row["technical_integrity"])<<" nominal_targets="<<bool(row["comparative_nominal_targets_met"])<<'\n'<<std::flush;
            if(!bool(row["technical_integrity"]))goto complete;
        }
    }
complete:
    put(report,"cases",rows);put(report,"case_count_completed",integer(count));put(report,"technical_integrity",integrity&&count==24);put(report,"required_normal_targets_met",heapAvailable?J():J(requiredTiming));put(report,"all_comparative_nominal_targets_met",allTargets);put(report,"allocation_verified",heapAvailable&&integrity&&count==24);
    put(report,"status",integrity&&count==24&&requiredTiming?heapAvailable?"PASS_ACTUAL_PRODUCT_HEAP_AUDIT":"PASS_DECLARED_NORMAL_PRODUCT_CAPACITY_SCOPE":"NOT_PASS");write(root.getChildFile("report.json"),report);return integrity&&count==24&&requiredTiming?0:1;
}
int selfTest(){
    int checks=0;const auto check=[&](bool good){++checks;require(good,"product profiler self-test");};
    const std::vector<double> raw{2,8,99,1};const auto stats=statistics(raw,50);check(stats.mean==27.5&&stats.median==5&&stats.p99==99&&stats.maximum==99&&stats.misses==1);check(raw==std::vector<double>({2,8,99,1}));
    check(blocksFor(1,4,44100,64)*64==11072);check(blocksFor(1,1,44100,64)*64==44160);check(blocksFor(2,1,44100,64)*64==88256);
    check(blocksFor(1,4,96000,512)*512==24064);check(blocksFor(2,1,96000,512)*512==192000);
    SpaceStats before;before.ready=true;before.modeCount=128;before.physicalFactor=2;before.activeMask=255;before.converterBackend=space::runtime::ModalConverterBackend::mirroredAvx2;
    auto after=before;after.renderedPhysicalFrames.fill(1024);check(spaceProgress(before,after,1024,2,false));
    auto bad=after;bad.renderedPhysicalFrames[7]=1023;check(!spaceProgress(before,bad,1024,2,false));bad=after;bad.modeCount=256;check(!spaceProgress(before,bad,1024,2,false));
    bad=after;bad.converterBackend=space::runtime::ModalConverterBackend::mirroredSse2;check(!spaceProgress(before,bad,1024,2,false));bad=after;bad.ready=false;check(!spaceProgress(before,bad,1024,2,false));
    bad=after;bad.activeMask=0;bad.renderedPhysicalFrames.fill(512);check(spaceProgress(before,bad,1024,2,true));check(!spaceProgress(before,bad,1024,2,false));
    before.activeMask=0;after=before;check(spaceProgress(before,after,1024,2,true));after.renderedPhysicalFrames.fill(1);check(!spaceProgress(before,after,1024,2,true));
    check(spaceMaskForPhase(255,255,0,48000,64,true));check(!spaceMaskForPhase(0,0,0,48000,64,true));
    check(spaceMaskForPhase(0,0,800,48000,64,true));check(!spaceMaskForPhase(255,255,800,48000,64,true));
    check(spaceMaskForPhase(255,255,1400,48000,64,true));check(!spaceMaskForPhase(0,0,1400,48000,64,true));
    for(int rate:{44100,48000,96000})for(int block:{64,512}){const auto list=events(rate,block);for(std::size_t n=0;n<list.size();++n){check(list[n].actual>=list[n].requested);check(n>=5||list[n].actual%unsigned(block)==0);check(list[n].actual<blocksFor(2,1,rate,block)*unsigned(block));}}
    for(bool higher:{false,true}){auto source=workload(higher);check(admitted(source));auto bad=source;set(bad,"synth.4.enabled",0);check(!admitted(bad));bad=source;set(bad,"synth.1.modulation.16.amount",0);check(!admitted(bad));bad=source;set(bad,"effect.8.bypassed",1);check(!admitted(bad));bad=source;set(bad,"effect.8.space_reverb.post.6.active",0);check(!admitted(bad));}
    std::cout<<"SPACE_PRODUCT_PROFILE_SELF_TEST checks="<<checks<<" failures=0; no timed processor callbacks\n";return 0;
}
}
int main(int argc,char** argv){try{juce::ScopedJuceInitialiser_GUI juce;if(argc==3&&juce::String(argv[1])=="--profile")return profile(argv[2]);if(argc==1||(argc==2&&juce::String(argv[1])=="--self-test"))return selfTest();return 2;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}}
