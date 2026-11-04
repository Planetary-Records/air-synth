#include "FxMonitor.h"
#include "DynamicEq.h"
#include "RoutedSynthEngine.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <atomic>
#include <barrier>
#include <algorithm>
#include <chrono>
#include <string_view>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack {
struct FxMonitorTestAccess {
    static void exhaustSaturation(FxMonitor& monitor) { monitor.nextSaturationGeneration=0; }
    static void lastSaturation(FxMonitor& monitor) { monitor.nextSaturationGeneration=(std::uint64_t{1}<<60)-1; }
};
}
namespace
{
using namespace airack;
int checks=0,failures=0;
void check(bool good,const char* expression,int line){++checks;if(!good){++failures;if(failures<40)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void saturationCommandAuthority()
{
    std::cout<<"SECTION saturation_kind_safe_latest_command_no_capture\n";
    FxMonitor monitor;FxMonitor::ActiveSlots active{};active[3]=true;CHECK(monitor.publishSampleRate(48000));
    CHECK(!monitor.acquireSaturationAudition(8));const auto token=monitor.acquireSaturationAudition(3);CHECK(token);if(!token)return;
    CHECK(!monitor.acquireSaturationAudition(3));CHECK((token->id>>63)==1);
    CHECK(!monitor.requestSaturationAudition(*token,SaturationLane::full));CHECK(!monitor.requestSaturationAudition(*token,SaturationLane(255)));
    const auto eq=monitor.subscribe(3);CHECK(eq);if(!eq)return;
    CHECK(!monitor.requestAudition({token->id,3},0));CHECK(!monitor.retire({token->id,3}));
    CHECK(!monitor.requestSaturationAudition({eq->id,3},SaturationLane::low));CHECK(!monitor.retireSaturationAudition({eq->id,3}));
    CHECK(!monitor.requestSaturationAudition({token->id|(std::uint64_t{1}<<61),3},SaturationLane::low));
    CHECK(!monitor.requestSaturationAudition({std::uint64_t{1}<<63,3},SaturationLane::low));
    CHECK(!monitor.requestSaturationAudition({token->id,8},SaturationLane::low));
    CHECK(monitor.retire(*eq));CHECK(monitor.publishSampleRate(48000));
    for(auto lane:{SaturationLane::low,SaturationLane::high,SaturationLane::mid}){
        CHECK(monitor.requestSaturationAudition(*token,lane));monitor.beginSaturationBlock(active);CHECK(monitor.saturationAuditionLane(3)==lane);
        CHECK(!monitor.saturationAuditionLane(2));CHECK(!monitor.saturationAuditionLane(8));
    }
    CHECK(monitor.auditionTailSeconds()>=2.02);monitor.saturationAuditionRunning(3,true);
    CHECK(monitor.retireSaturationAudition(*token));CHECK(!monitor.saturationAuditionLane(3));CHECK(monitor.auditionTailSeconds()>=2.02);
    const auto next=monitor.acquireSaturationAudition(3);CHECK(next&&next->id!=token->id);if(!next)return;
    CHECK(monitor.requestSaturationAudition(*next,SaturationLane::high));
    CHECK(!monitor.requestSaturationAudition(*token,std::nullopt));CHECK(!monitor.retireSaturationAudition(*token));
    monitor.beginSaturationBlock(active);CHECK(monitor.saturationAuditionLane(3)==SaturationLane::high);
    CHECK(monitor.requestSaturationAudition(*next,std::nullopt));CHECK(!monitor.saturationAuditionLane(3));
    monitor.saturationAuditionRunning(3,false);CHECK(monitor.auditionTailSeconds()==0);
    for(int event=0;event<5;++event){monitor.cancelSaturationAudition(3);const auto t=monitor.acquireSaturationAudition(3);CHECK(t);if(!t)return;
        CHECK(monitor.requestSaturationAudition(*t,SaturationLane::mid));monitor.beginSaturationBlock(active);
        if(event==0)monitor.cancelSaturationAudition(3);
        if(event==1)monitor.invalidate();
        if(event==2)CHECK(monitor.publishSampleRate(0));
        if(event==3){active[3]=false;monitor.beginSaturationBlock(active);active[3]=true;}
        if(event==4)CHECK(monitor.publishSampleRate(96000));
        CHECK(!monitor.requestSaturationAudition(*t,SaturationLane::low));CHECK(!monitor.retireSaturationAudition(*t));CHECK(!monitor.saturationAuditionLane(3));
    }
    FxMonitorTestAccess::lastSaturation(monitor);const auto last=monitor.acquireSaturationAudition(1);CHECK(last);if(last)CHECK(monitor.retireSaturationAudition(*last));
    CHECK(!monitor.acquireSaturationAudition(1));FxMonitorTestAccess::exhaustSaturation(monitor);CHECK(!monitor.acquireSaturationAudition(2));
    for(std::uint8_t slot=0;slot<8;++slot){const auto stats=monitor.stats(slot);CHECK(stats.published==0&&stats.capturedFrames==0&&stats.queued==0&&stats.dropped==0&&stats.clipped==0);}
    // A late owner races retirement/replacement; its complete-word CAS cannot
    // alter the next generation, even when the selected code is unchanged.
    FxMonitor raced;std::atomic<bool> done=false;std::atomic<unsigned> successes=0;
    auto old=raced.acquireSaturationAudition(0);CHECK(old);if(!old)return;
    std::jthread late([&]{while(!done.load())if(raced.requestSaturationAudition(*old,SaturationLane::low))++successes;});
    raced.cancelSaturationAudition(0);const auto replacement=raced.acquireSaturationAudition(0);CHECK(replacement);if(replacement)CHECK(raced.requestSaturationAudition(*replacement,SaturationLane::high));
    done=true;late.join();active={};active[0]=true;raced.beginSaturationBlock(active);CHECK(raced.saturationAuditionLane(0)==SaturationLane::high);
}
void detectorCommandsReadbacksAndAvailability()
{
    std::cout<<"SECTION detector_command_snapshot_readbacks_and_explicit_aux_availability\n";
    FxMonitor monitor;CHECK(!monitor.externalAvailable());const auto token=monitor.subscribe(2);CHECK(token);if(!token)return;
    FxMonitor::ActiveSlots active{};active[2]=true;CHECK(monitor.beginBlock(48000,active,true)&&monitor.externalAvailable());
    for(std::size_t band:{std::size_t(0),std::size_t(7)})for(bool detector:{false,true}){
        CHECK(monitor.requestAudition(*token,band,detector));const auto request=monitor.auditionRequest(2);
        CHECK(request&&request->band==band&&request->detector==detector);CHECK(monitor.requestedAudition(*token)==band&&monitor.auditionBand(2)==band);
        CHECK(!monitor.requestAudition(*token,8,detector));CHECK(!monitor.requestAudition({token->id,3},band,detector));
    }
    CHECK(monitor.requestAudition(*token,0));const auto normal=monitor.auditionRequest(2);CHECK(normal&&normal->band==0&&!normal->detector);
    DynamicEq eq;DynamicEqSettings settings;auto& b=settings.bands[0];b.active=true;b.autoThreshold=true;b.adaptiveTiming=true;
    b.detectorSource=DynamicEqDetectorSource::externalFree;b.detectorLowHz=50;b.detectorHighHz=500;b.rangeDb=-6;
    CHECK(eq.setSettings(settings)&&eq.prepare(48000));
    for(int n=0;n<256;++n){const DynamicEq::Frame input{.1,-.1},external{.5*std::sin(n*.03),0};monitor.capture(2,input,eq.process(input,external),&eq);}
    FxMonitor::Packet packet;CHECK(monitor.read(*token,packet));CHECK(packet.detectorReadbacksValid&&packet.externalAvailable);
    const auto expected=eq.detectorReadbacks();for(std::size_t n=0;n<8;++n){const auto actual=packet.detectorReadbacks[n];CHECK(actual.thresholdDb==expected[n].thresholdDb&&actual.attackMs==expected[n].attackMs&&actual.releaseMs==expected[n].releaseMs);}
    CHECK(packet.detectorReadbacks[0].thresholdDb!=settings.bands[0].thresholdDb);
    CHECK(monitor.beginBlock(48000,active)&&!monitor.externalAvailable());
    for(int n=0;n<256;++n)monitor.capture(2,{},{},nullptr);
    CHECK(monitor.read(*token,packet)&&!packet.detectorReadbacksValid&&!packet.externalAvailable);
    monitor.invalidate();CHECK(!monitor.auditionRequest(2));CHECK(monitor.requestAudition(*token,7,true));CHECK(monitor.retire(*token));CHECK(!monitor.auditionRequest(2));
    CHECK(monitor.beginBlock(48000,active,true)&&monitor.externalAvailable());CHECK(monitor.publishSampleRate(0)&&!monitor.externalAvailable());
}
void blockPacketEquivalence()
{
    std::cout<<"SECTION exact_block_capture_packet_boundaries\n";
    for(double rate:{48000.,192000.}){
        FxMonitor reference,blocked;DynamicEq a,b;DynamicEqSettings settings;
        for(std::size_t n=0;n<8;++n){auto& band=settings.bands[n];band.active=true;band.frequencyHz=100+float(n)*700;band.rangeDb=n%2?12.f:-12.f;band.thresholdDb=-35;}
        CHECK(a.setSettings(settings)&&b.setSettings(settings)&&a.prepare(rate)&&b.prepare(rate));
        const auto ta=reference.subscribe(0),tb=blocked.subscribe(0);CHECK(ta&&tb);if(!ta||!tb)return;
        FxMonitor::ActiveSlots active{};active[0]=true;
        CHECK(blocked.framesUntilPacket(8)==0&&blocked.framesUntilPacket(0)==0);
        std::array<DynamicEq::Frame,513> input{},output{};
        std::uint64_t sample=0;
        for(int callback=0;callback<37;++callback){
            CHECK(reference.beginBlock(rate,active)&&blocked.beginBlock(rate,active));
            if(callback==9||callback==18){settings.bands[2].frequencyHz+=900;settings.bands[3].bypassed=!settings.bands[3].bypassed;CHECK(a.setSettings(settings)&&b.setSettings(settings));}
            const auto count=std::size_t(std::array{1,64,257,513}[std::size_t(callback)%4]);
            for(std::size_t n=0;n<count;++n){input[n]={.3*std::sin(double(sample)*.13),.2*std::cos(double(sample++)*.07)};reference.capture(0,input[n],a.process(input[n]),&a);output[n]=input[n];}
            std::size_t offset=0;
            while(offset<count){const auto next=blocked.framesUntilPacket(0);CHECK(next>0&&next<=FxMonitor::packetFrames);const auto size=std::min(next,count-offset);b.processBlock(std::span(output).subspan(offset,size));for(std::size_t n=0;n<size;++n)blocked.capture(0,input[offset+n],output[offset+n],&b);offset+=size;}
            FxMonitor::Packet pa,pb;
            while(reference.read(*ta,pa)){CHECK(blocked.read(*tb,pb));CHECK(pa.samples==pb.samples);CHECK(pa.movementDb==pb.movementDb);CHECK(pa.firstSample==pb.firstSample&&pa.sampleRate==pb.sampleRate&&pa.generation==pb.generation);}
            CHECK(!blocked.read(*tb,pb));
        }
        CHECK(reference.stats(0).capturedFrames==blocked.stats(0).capturedFrames);
        active[0]=false;CHECK(blocked.beginBlock(rate,active));CHECK(blocked.framesUntilPacket(0)==0);
    }
}
void preparedRateMetadata()
{
    std::cout<<"SECTION prepared_rate_without_spectrum_or_audio\n";
    FxMonitor monitor;CHECK(monitor.sampleRate()==0);{
        RoutedSynthEngine engine;engine.attachFxMonitor(&monitor);CHECK(engine.prepare(8000,64));CHECK(monitor.sampleRate()==8000);
        CHECK(!engine.prepare(0,64));CHECK(monitor.sampleRate()==8000);engine.reset();CHECK(monitor.sampleRate()==8000);
        CHECK(engine.prepare(96000,64,RenderQuality::higher));CHECK(monitor.sampleRate()==192000);
        engine.attachFxMonitor(nullptr);CHECK(monitor.sampleRate()==0);engine.attachFxMonitor(&monitor);CHECK(monitor.sampleRate()==192000);
    }CHECK(monitor.sampleRate()==0);
}
void auditionAuthority()
{
    std::cout<<"SECTION transient_audition_token_authority\n";
    FxMonitor monitor;const auto token=monitor.subscribe(3);CHECK(token);if(!token)return;
    CHECK(!monitor.requestAudition(*token,8));CHECK(!monitor.requestAudition({token->id,2},0));
    CHECK(monitor.requestAudition(*token,0));CHECK(monitor.requestedAudition(*token)==0);CHECK(monitor.auditionTailSeconds()>=8);
    FxMonitor::ActiveSlots active{};active[3]=true;CHECK(monitor.beginBlock(48000,active));CHECK(monitor.sampleRate()==48000);
    CHECK(monitor.auditionBand(3)==0);CHECK(!monitor.auditionBand(2));
    monitor.auditionRunning(3,true);CHECK(monitor.requestAudition(*token,std::nullopt));CHECK(!monitor.auditionBand(3));CHECK(monitor.auditionTailSeconds()>=8);
    monitor.auditionRunning(3,false);CHECK(monitor.auditionTailSeconds()==0);
    const auto second=monitor.subscribe(6);CHECK(second);if(second){CHECK(monitor.requestAudition(*token,0));CHECK(monitor.requestAudition(*second,1));CHECK(monitor.auditionTailSeconds()==16);CHECK(monitor.retire(*second));}
    CHECK(monitor.requestAudition(*token,7));monitor.invalidate();CHECK(!monitor.requestedAudition(*token));CHECK(monitor.auditionTailSeconds()==0);
    CHECK(monitor.requestAudition(*token,2));CHECK(monitor.retire(*token));CHECK(!monitor.requestedAudition(*token));
    const auto replacement=monitor.subscribe(3);CHECK(replacement);if(!replacement)return;
    CHECK(monitor.requestAudition(*replacement,6));CHECK(!monitor.requestAudition(*token,std::nullopt));CHECK(monitor.requestedAudition(*replacement)==6);
    active[3]=false;CHECK(monitor.beginBlock(48000,active));CHECK(!monitor.auditionBand(3));CHECK(!monitor.requestedAudition(*replacement));
}
void subscriptionAndFrames()
{
    std::cout<<"SECTION fixed_slot_subscriptions_and_exact_frames\n";
    FxMonitor monitor;DynamicEq eq;CHECK(eq.prepare(48000));FxMonitor::ActiveSlots active{};active[0]=true;
    CHECK(monitor.beginBlock(48000,active));
    for(int n=0;n<512;++n)monitor.capture(0,{double(n),-double(n)},{double(n)*2,-double(n)*2},&eq);
    CHECK(monitor.stats(0).capturedFrames==0);CHECK(!monitor.subscribe(8));
    const auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    CHECK(!monitor.subscribe(0));CHECK(monitor.generation(*token)!=0);
    CHECK(monitor.beginBlock(48000,active));FxMonitor::Packet packet;
    for(int block=0;block<4;++block){
        CHECK(monitor.beginBlock(48000,active));
        for(int frame=0;frame<64;++frame){const auto n=block*64+frame;monitor.capture(0,{double(n),-double(n)},{double(n)*2,-double(n)*2},&eq);}
        if(block<3)CHECK(!monitor.read(*token,packet));
    }
    CHECK(monitor.read(*token,packet));CHECK(packet.token==token->id&&packet.generation==monitor.generation(*token));
    CHECK(packet.sampleRate==48000&&packet.firstSample==0);
    for(std::size_t n=0;n<FxMonitor::packetFrames;++n){CHECK(packet.samples[n][0]==float(n));CHECK(packet.samples[n][1]==-float(n));CHECK(packet.samples[n][2]==float(n)*2);CHECK(packet.samples[n][3]==-float(n)*2);}
    CHECK(!monitor.read(*token,packet));CHECK(monitor.stats(0).published==1&&monitor.stats(0).capturedFrames==256);
    const auto generation=monitor.generation(*token);monitor.invalidate();CHECK(monitor.generation(*token)!=generation);
    CHECK(monitor.retire(*token));CHECK(!monitor.retire(*token));CHECK(monitor.generation(*token)==0);
    const auto next=monitor.subscribe(0);CHECK(next&&next->id!=token->id);if(next){CHECK(!monitor.retire(*token));CHECK(monitor.retire(*next));}
}
void overflowAndEpochs()
{
    std::cout<<"SECTION bounded_overflow_token_epoch_and_partition_rules\n";
    FxMonitor monitor;FxMonitor::ActiveSlots active{};active[7]=true;const auto token=monitor.subscribe(7);CHECK(token);if(!token)return;
    CHECK(monitor.beginBlock(48000,active));
    const auto produce=[&](int packets,int offset=0){for(int n=0;n<packets*int(FxMonitor::packetFrames);++n)monitor.capture(7,{double(offset+n),1},{2,3});};
    produce(int(FxMonitor::packetCapacity)+2);const auto full=monitor.stats(7);
    CHECK(full.queued==int(FxMonitor::packetCapacity)&&full.published==FxMonitor::packetCapacity&&full.dropped==2);
    FxMonitor::Packet packet;std::size_t count=0;
    while(monitor.read(*token,packet)){CHECK(packet.firstSample==count*FxMonitor::packetFrames);
        for(std::size_t n=0;n<FxMonitor::packetFrames;++n)CHECK(packet.samples[n][0]==float(count*FxMonitor::packetFrames+n));++count;}
    CHECK(count==FxMonitor::packetCapacity);produce(1,90000);CHECK(monitor.read(*token,packet));
    CHECK(packet.firstSample==(FxMonitor::packetCapacity+2)*FxMonitor::packetFrames&&packet.samples[0][0]==90000);
    produce(1);const auto oldEpoch=monitor.generation(*token);CHECK(monitor.beginBlock(96000,active));CHECK(monitor.generation(*token)!=oldEpoch);
    packet.sampleRate=123;CHECK(!monitor.read(*token,packet));CHECK(packet.sampleRate==123);
    produce(1);CHECK(monitor.read(*token,packet));CHECK(packet.sampleRate==96000&&packet.firstSample==0);
    produce(1);CHECK(monitor.retire(*token));const auto replacement=monitor.subscribe(7);CHECK(replacement);if(!replacement)return;
    CHECK(!monitor.retire(*token));CHECK(monitor.beginBlock(96000,active));CHECK(!monitor.read(*replacement,packet));
    // An incomplete old stream cannot become the first half of a new one.
    for(int n=0;n<127;++n)monitor.capture(7,{999,0},{0,0});monitor.invalidate();
    produce(1,2000);CHECK(monitor.read(*replacement,packet));CHECK(packet.firstSample==0&&packet.samples[0][0]==2000&&packet.samples[255][0]==2255);
    active[7]=false;CHECK(monitor.beginBlock(96000,active));produce(1);CHECK(!monitor.read(*replacement,packet));
    active[7]=true;CHECK(monitor.beginBlock(96000,active));produce(1);CHECK(monitor.read(*replacement,packet));
    const auto epoch=monitor.generation(*replacement);CHECK(!monitor.beginBlock(std::numeric_limits<double>::quiet_NaN(),active));CHECK(monitor.generation(*replacement)==epoch);
    for(std::size_t n=0;n<FxMonitor::packetFrames;++n)monitor.capture(7,{1e200,std::numeric_limits<double>::infinity()},{-1e200,std::numeric_limits<double>::quiet_NaN()});
    CHECK(monitor.read(*replacement,packet));CHECK(monitor.stats(7).clipped==4*FxMonitor::packetFrames);
    for(const auto& sample:packet.samples){for(auto x:sample)CHECK(std::isfinite(x));CHECK(sample[0]==1e12f&&sample[1]==0&&sample[2]==-1e12f&&sample[3]==0);}
}
void concurrentRetirement()
{
    std::cout<<"SECTION concurrent_copy_and_subscription_retirement\n";
    FxMonitor monitor;FxMonitor::ActiveSlots active{};active[0]=true;
    auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    std::barrier boundary(2);constexpr int waves=64;std::atomic<bool> producerOk=true;
    std::jthread producer([&]{int frame=0;for(int wave=0;wave<waves;++wave){boundary.arrive_and_wait();
        for(int block=0;block<64;++block){if(!monitor.beginBlock(48000,active))producerOk=false;
            for(int n=0;n<64;++n,++frame)monitor.capture(0,{double(frame),-double(frame)},{double(frame)*2,-double(frame)*2});}
        boundary.arrive_and_wait();}});
    std::size_t received=0;FxMonitor::Packet packet;
    const auto inspect=[&]{++received;CHECK(packet.token==token->id);
        for(std::size_t n=0;n<FxMonitor::packetFrames;++n){const auto& x=packet.samples[n];CHECK(x[1]==-x[0]&&x[2]==2*x[0]&&x[3]==-2*x[0]);
            if(n>0)CHECK(x[0]==packet.samples[n-1][0]+1);}};
    for(int wave=0;wave<waves;++wave){boundary.arrive_and_wait();
        if(wave%2==0){CHECK(monitor.retire(*token));token=monitor.subscribe(0);CHECK(token);}
        for(int poll=0;poll<16;++poll){if(token&&monitor.read(*token,packet))inspect();std::this_thread::yield();}
        boundary.arrive_and_wait();if(!token)continue;
        while(monitor.read(*token,packet))inspect();
    }
    producer.join();CHECK(producerOk.load());CHECK(received>0);CHECK(monitor.stats(0).queued==0);
    std::cout<<"CONCURRENCY received_packets="<<received<<" frames="<<monitor.stats(0).capturedFrames<<'\n';
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool countHeap=false;thread_local std::size_t heapCalls=0,heapBytes=0,heapFrees=0;
int heapHook(int operation,void*,std::size_t bytes,int type,long,const unsigned char*,int)
{if(countHeap&&type!=_CRT_BLOCK){if(operation==_HOOK_FREE)++heapFrees;else{++heapCalls;heapBytes+=bytes;}}return 1;}
#endif
void fixedAllocation()
{
    std::cout<<"SECTION fixed_capture_storage_and_no_prepared_allocation\n";
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto old=_CrtSetAllocHook(heapHook);heapCalls=heapBytes=heapFrees=0;countHeap=true;
    auto monitor=std::make_unique<FxMonitor>();countHeap=false;
    std::cout<<"MONITOR storage_requested="<<heapBytes<<" constructor_calls="<<heapCalls<<'\n';CHECK(heapBytes>4000000&&heapBytes<=5*1024*1024);
    FxMonitor::ActiveSlots active{};active.fill(true);std::array<FxMonitor::Token,8> tokens;
    for(std::uint8_t i=0;i<8;++i){const auto token=monitor->subscribe(i);CHECK(token);if(!token){_CrtSetAllocHook(old);return;}tokens[i]=*token;}
    DynamicEq eq;DynamicEqSettings settings;settings.bands[0].active=true;settings.bands[0].rangeDb=6;settings.bands[0].thresholdDb=-60;
    CHECK(eq.setSettings(settings)&&eq.prepare(48000));for(int n=0;n<12000;++n)eq.process({.5*std::sin(n*.13),.25*std::cos(n*.13)});
    CHECK(eq.dynamicGainsDb()[0]>1);heapCalls=heapBytes=heapFrees=0;countHeap=true;
    bool ok=true;for(int block=0;block<600;++block){ok=monitor->beginBlock(block<300?48000:96000,active)&&ok;
        for(int n=0;n<64;++n)for(std::uint8_t i=0;i<8;++i)monitor->capture(i,{.5,-.5},{.25,-.25},&eq);
        if(block%17==0)monitor->invalidate();}
    for(const auto token:tokens)ok=monitor->retire(token)&&ok;countHeap=false;_CrtSetAllocHook(old);
    CHECK(ok);CHECK(heapCalls==0&&heapFrees==0);std::cout<<"MONITOR prepared_allocations="<<heapCalls<<" frees="<<heapFrees<<'\n';
    CHECK(monitor->stats(0).published>0&&monitor->stats(0).dropped>0);
#endif
}
void routedCapture()
{
    std::cout<<"SECTION physical_slot_input_output_and_unchanged_audio\n";
    FxMonitor monitor;const auto token=monitor.subscribe(7);CHECK(token);if(!token)return;
    RoutedSynthEngine tapped,reference;tapped.attachFxMonitor(&monitor);
    CHECK(tapped.prepare(48000,64)&&reference.prepare(48000,64));CHECK(monitor.sampleRate()==48000);
    RoutedPatch patch;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(patch.layout,add,patch.layout)==RackEditResult::applied);
    auto& band=patch.effects[7].dynamicEq.bands[0];band.active=true;band.gainDb=9;band.q=1;
    CHECK(tapped.setPatch(patch)&&reference.setPatch(patch));tapped.reset();reference.reset();
    juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
    for(int block=0;block<8;++block){tapped.render(a,midi);reference.render(b,midi);midi.clear();
        for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));}
    FxMonitor::Packet packet;CHECK(monitor.read(*token,packet));CHECK(packet.sampleRate==48000);
    bool different=false;for(const auto& frame:packet.samples)different=different||frame[0]!=frame[2]||frame[1]!=frame[3];CHECK(different);
    const auto generation=monitor.generation(*token);tapped.reset();CHECK(monitor.generation(*token)!=generation);CHECK(!monitor.read(*token,packet));
}
void routedOracles()
{
    std::cout<<"SECTION physical_insert_master_oracles_rates_quality_bypass_and_overflow\n";
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})
        for(std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)})for(std::uint8_t chain:{std::uint8_t(0),masterChain}){
        FxMonitor monitor;const auto token=monitor.subscribe(slot);CHECK(token);if(!token)continue;
        RoutedSynthEngine tapped,reference;tapped.attachFxMonitor(&monitor);CHECK(tapped.prepare(rate,64,quality)&&reference.prepare(rate,64,quality));
        RoutedPatch patch;const std::uint8_t up=slot==0?1:0,down=slot==7?1:7;std::uint8_t position=0;
        for(const auto physical:{up,slot,down}){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=physical;add.destination=chain;add.position=position++;
            add.effect=physical==slot?EffectKind::dynamicEq:EffectKind::stereo;CHECK(editRackLayout(patch.layout,add,patch.layout)==RackEditResult::applied);}
        patch.effects[up].stereo.gainDb=-6;patch.effects[down].stereo.gainDb=9;patch.outputGainDb=-12;patch.outputProtection=true;
        auto& settings=patch.effects[slot].dynamicEq;settings.outputGainDb=3;auto& band=settings.bands[0];band.active=true;band.gainDb=6;band.rangeDb=-6;band.thresholdDb=-60;band.attackMs=.1f;
        CHECK(tapped.setPatch(patch)&&reference.setPatch(patch));tapped.reset();reference.reset();
        DynamicEq oracle;const auto internalRate=rate*(quality==RenderQuality::higher?2:1);CHECK(oracle.setSettings(settings)&&oracle.prepare(internalRate));
        juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        FxMonitor::Packet packet;std::size_t packets=0;double maximumError=0;bool identical=true;
        for(int block=0;block<32;++block){tapped.render(a,midi);reference.render(b,midi);midi.clear();
            for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)identical=identical&&a.getSample(ch,n)==b.getSample(ch,n);
            while(monitor.read(*token,packet)){CHECK(packet.sampleRate==internalRate);CHECK(packet.firstSample==packets*FxMonitor::packetFrames);++packets;
                for(const auto& frame:packet.samples){const auto expected=oracle.process({frame[0],frame[1]});maximumError=std::max({maximumError,std::abs(expected[0]-frame[2]),std::abs(expected[1]-frame[3])});}}
        }
        CHECK(identical);CHECK(packets==std::size_t(32*64*(quality==RenderQuality::higher?2:1))/FxMonitor::packetFrames);CHECK(maximumError<2e-6);
        const auto epoch=monitor.generation(*token);CHECK(!tapped.prepare(0,64,quality));CHECK(monitor.generation(*token)==epoch);
        patch.effects[slot].bypassed=true;CHECK(tapped.setPatch(patch)&&reference.setPatch(patch));
        for(int n=0;n<24;++n){tapped.render(a,midi);reference.render(b,midi);for(int ch=0;ch<2;++ch)for(int f=0;f<64;++f)CHECK(a.getSample(ch,f)==b.getSample(ch,f));}
        CHECK(monitor.generation(*token)!=epoch);CHECK(monitor.read(*token,packet));
        for(const auto& frame:packet.samples)CHECK(frame[0]==frame[2]&&frame[1]==frame[3]);for(auto gain:packet.movementDb)CHECK(gain==0);
        while(monitor.read(*token,packet)){}
        if(rate==48000&&quality==RenderQuality::realtime&&slot==7&&chain==masterChain){
#if defined(_MSC_VER) && defined(_DEBUG)
            const auto previous=_CrtSetAllocHook(heapHook);heapCalls=heapFrees=0;countHeap=true;
#endif
            for(int block=0;block<600;++block){tapped.render(a,midi);reference.render(b,midi);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)identical=identical&&a.getSample(ch,n)==b.getSample(ch,n);}
#if defined(_MSC_VER) && defined(_DEBUG)
            countHeap=false;_CrtSetAllocHook(previous);CHECK(heapCalls==0&&heapFrees==0);
#endif
            CHECK(identical);CHECK(monitor.stats(slot).dropped>0);
        }
        if(rate==48000&&quality==RenderQuality::realtime&&slot==7&&chain==0){
            const auto old=monitor.generation(*token);CHECK(tapped.prepare(96000,64,RenderQuality::higher));CHECK(monitor.generation(*token)!=old);
            for(int n=0;n<8;++n)tapped.render(a,midi);CHECK(monitor.read(*token,packet));CHECK(packet.sampleRate==192000);
            const auto beforeStop=monitor.generation(*token);midi.addEvent(juce::MidiMessage::midiStop(),17);tapped.render(a,midi);midi.clear();
            CHECK(monitor.generation(*token)!=beforeStop);CHECK(!monitor.read(*token,packet));for(int n=0;n<8;++n)tapped.render(a,midi);
            CHECK(monitor.read(*token,packet));CHECK(packet.firstSample==0);while(monitor.read(*token,packet)){}
            RackEdit move;move.kind=RackEditKind::moveEffect;move.slot=slot;move.destination=masterChain;move.position=0;
            CHECK(editRackLayout(patch.layout,move,patch.layout)==RackEditResult::applied);const auto beforeMove=monitor.generation(*token);CHECK(tapped.setPatch(patch));
            for(int n=0;n<24;++n)tapped.render(a,midi);CHECK(monitor.generation(*token)!=beforeMove);CHECK(monitor.read(*token,packet));CHECK(packet.sampleRate==192000);
            std::cout<<"TAP_LIFECYCLE moved_slot=7 rate=192000 stop_and_reprepare=1\n";
        }
        RackEdit remove;remove.kind=RackEditKind::removeEffect;remove.slot=slot;CHECK(editRackLayout(patch.layout,remove,patch.layout)==RackEditResult::applied);
        const auto beforeRemoval=monitor.generation(*token);CHECK(tapped.setPatch(patch));for(int n=0;n<24;++n)tapped.render(a,midi);
        CHECK(monitor.generation(*token)!=beforeRemoval);CHECK(!monitor.read(*token,packet));
        std::cout<<"TAP rate="<<rate<<" quality="<<int(quality)<<" slot="<<int(slot)<<" chain="<<int(chain)<<" oracle_error="<<maximumError<<" unchanged="<<identical<<'\n';
    }
}
void routedAudition()
{
    std::cout<<"SECTION actual_routed_audition_release_stop_and_bypass\n";
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        FxMonitor monitor;const auto token=monitor.subscribe(7);CHECK(token);if(!token)continue;
        RoutedSynthEngine tapped,reference;tapped.attachFxMonitor(&monitor);CHECK(tapped.prepare(48000,64,quality)&&reference.prepare(48000,64,quality));
        RoutedPatch patch;patch.outputProtection=false;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.effect=EffectKind::dynamicEq;
        CHECK(editRackLayout(patch.layout,add,patch.layout)==RackEditResult::applied);auto& band=patch.effects[7].dynamicEq.bands[0];band.active=true;band.frequencyHz=4000;band.q=4;
        CHECK(tapped.setPatch(patch)&&reference.setPatch(patch));tapped.reset();reference.reset();
        juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        for(int block=0;block<8;++block){tapped.render(a,midi);reference.render(b,midi);midi.clear();}
        CHECK(monitor.requestAudition(*token,0));double difference=0;
#if defined(_MSC_VER) && defined(_DEBUG)
        const auto auditionHook=_CrtSetAllocHook(heapHook);heapCalls=heapFrees=0;countHeap=true;
#endif
        for(int block=0;block<32;++block){tapped.render(a,midi);reference.render(b,midi);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)difference=std::max(difference,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));}

#if defined(_MSC_VER) && defined(_DEBUG)
        countHeap=false;_CrtSetAllocHook(auditionHook);CHECK(heapCalls==0&&heapFrees==0);
#endif
        CHECK(difference>1e-5);CHECK(monitor.auditionTailSeconds()>=8);
        CHECK(monitor.requestAudition(*token,std::nullopt));double restored=0;
        for(int block=0;block<32;++block){tapped.render(a,midi);reference.render(b,midi);if(block>16)for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)restored=std::max(restored,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));}
        CHECK(restored==0);CHECK(monitor.auditionTailSeconds()==0);
        CHECK(monitor.requestAudition(*token,0));tapped.render(a,midi);midi.addEvent(juce::MidiMessage::midiStop(),17);tapped.render(a,midi);midi.clear();
        CHECK(!monitor.requestedAudition(*token));CHECK(monitor.auditionTailSeconds()==0);
        CHECK(monitor.requestAudition(*token,0));tapped.reset();CHECK(!monitor.requestedAudition(*token));
        patch.effects[7].bypassed=true;CHECK(tapped.setPatch(patch)&&reference.setPatch(patch));tapped.reset();reference.reset();
        CHECK(monitor.requestAudition(*token,0));tapped.render(a,midi);reference.render(b,midi);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));
        CHECK(!monitor.requestedAudition(*token));
        std::cout<<"ROUTED_AUDITION quality="<<int(quality)<<" difference="<<difference<<" restored="<<restored<<'\n';
    }
}
void routedDetectorAudition()
{
    std::cout<<"SECTION routed_external_trigger_audition_is_distinct_and_cancels_when_missing\n";
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        FxMonitor monitor;RoutedSynthEngine engine;engine.attachFxMonitor(&monitor);RoutedPatch patch;patch.synth.layers[0].enabled=false;patch.synth.masterLevel=1;
        CHECK(editRackLayout(patch.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::dynamicEq},patch.layout)==RackEditResult::applied);
        auto& b=patch.effects[0].dynamicEq.bands[0];b.active=true;b.frequencyHz=4000;b.detectorSource=DynamicEqDetectorSource::externalFree;b.detectorLowHz=800;b.detectorHighHz=1200;
        CHECK(engine.setPatch(patch)&&engine.prepare(48000,512,quality));CHECK(engine.latencySamples()==(quality==RenderQuality::higher?21:0));
        const auto token=monitor.subscribe(0);CHECK(token);if(!token)continue;juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;std::array<DynamicEq::Frame,512> external;
        int frame=0;const auto render=[&](bool connected){for(auto& x:external){const auto sample=.5*std::sin(2*juce::MathConstants<double>::pi*1000*frame++/48000);x={sample,-sample};}
            engine.render(audio,midi,{},connected?std::span<const DynamicEq::Frame>(external):std::span<const DynamicEq::Frame>{});};
        render(true);CHECK(audio.getMagnitude(0,512)==0&&monitor.externalAvailable());
        CHECK(monitor.requestAudition(*token,0,true));float peak=0;for(int n=0;n<12;++n){render(true);peak=std::max(peak,audio.getMagnitude(0,512));}
        CHECK(peak>.1f);const auto request=monitor.auditionRequest(0);CHECK(request&&request->detector&&request->band==0);CHECK(monitor.auditionTailSeconds()>=8);
        render(false);CHECK(!monitor.externalAvailable()&&!monitor.requestedAudition(*token));for(int n=0;n<12;++n)render(false);
        CHECK(audio.getMagnitude(0,512)==0&&monitor.auditionTailSeconds()==0);
        CHECK(monitor.requestAudition(*token,0,true));render(false);CHECK(!monitor.requestedAudition(*token));
        CHECK(monitor.requestAudition(*token,0,true));for(int n=0;n<3;++n)render(true);CHECK(audio.getMagnitude(0,512)>.1f);CHECK(monitor.retire(*token));for(int n=0;n<12;++n)render(true);CHECK(audio.getMagnitude(0,512)==0&&monitor.auditionTailSeconds()==0);
    }
}
void profileCapture()
{
    using Clock=std::chrono::steady_clock;
    for(const auto rate:{48000.,96000.})for(const int block:{64,512})for(int mode=0;mode<5;++mode){
        std::array<DynamicEq,8> eqs;DynamicEqSettings settings;
        for(std::size_t band=0;band<8;++band){auto& b=settings.bands[band];b.active=true;b.frequencyHz=40.f*float(std::pow(2.,band));b.q=2;b.rangeDb=band%2?6.f:-6.f;b.thresholdDb=-60;}
        for(auto& eq:eqs)CHECK(eq.setSettings(settings)&&eq.prepare(rate));
        FxMonitor monitor;FxMonitor::ActiveSlots active{};active.fill(true);std::array<std::optional<FxMonitor::Token>,8> tokens;
        for(std::uint8_t slot=0;slot<8;++slot)if(mode>=3||(mode==2&&slot==0))tokens[slot]=monitor.subscribe(slot);
        const int blocks=int(std::ceil(rate/block));std::vector<double> durations(std::size_t(blocks),0.);FxMonitor::Packet packet;double checksum=0;
        for(int trial=0;trial<2*blocks;++trial){const auto started=Clock::now();if(mode>0)(void)monitor.beginBlock(rate,active);
            for(int n=0;n<block;++n){const auto frame=trial*block+n;const FxMonitor::Frame input{.25*std::sin(frame*.13),.5*std::cos(frame*.07)};
                for(std::uint8_t slot=0;slot<8;++slot){const auto output=eqs[slot].process(input);checksum+=output[0]+output[1];if(mode>0)monitor.capture(slot,input,output,&eqs[slot]);}}
            const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-started).count();if(trial>=blocks)durations[std::size_t(trial-blocks)]=elapsed;
            // UI/consumer work is deliberately outside callback timing.
            if(mode==2||mode==3)for(auto token:tokens)if(token)while(monitor.read(*token,packet)){}
        }
        std::sort(durations.begin(),durations.end());const auto p99=durations[std::size_t((durations.size()-1)*.99)];
        std::cout<<"CAPTURE_PROFILE rate="<<rate<<" block="<<block<<" mode="<<mode<<" p50_ms="<<durations[durations.size()/2]<<" p99_ms="<<p99
            <<" worst_ms="<<durations.back()<<" period_ms="<<block/rate*1000<<" checksum="<<checksum<<" drops="<<monitor.stats(0).dropped<<'\n';
        for(const auto& eq:eqs)for(const auto gain:eq.dynamicGainsDb())CHECK(std::abs(gain)>.1f);
        CHECK(std::isfinite(checksum));CHECK(p99<block/rate*1000*.5);
    }
}
}
int main(int argc,char** argv){if(argc==2&&std::string_view(argv[1])=="--saturation")saturationCommandAuthority();else if(argc==2&&std::string_view(argv[1])=="--profile")profileCapture();else if(argc==2&&std::string_view(argv[1])=="--detector-route")routedDetectorAudition();else if(argc==2&&std::string_view(argv[1])=="--detectors")detectorCommandsReadbacksAndAvailability();else if(argc==1){saturationCommandAuthority();detectorCommandsReadbacksAndAvailability();routedDetectorAudition();blockPacketEquivalence();preparedRateMetadata();auditionAuthority();subscriptionAndFrames();overflowAndEpochs();concurrentRetirement();fixedAllocation();routedCapture();routedOracles();routedAudition();}else return 2;
    std::cout<<"FX monitor: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
