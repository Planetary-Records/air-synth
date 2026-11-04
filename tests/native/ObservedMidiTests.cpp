#include "ObservedMidi.h"
#define NOMINMAX
#include <windows.h>
#undef near
#include <algorithm>
#include <atomic>
#include <cmath>
#include <crtdbg.h>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>

namespace
{
using namespace airack;
constexpr auto external=PlaybackMode::externalMidi,internal=PlaybackMode::internalPattern;
constexpr auto fresh=ObservedMidiAvailability::fresh,stale=ObservedMidiAvailability::stale,unavailable=ObservedMidiAvailability::unavailable;
int checks=0,failures=0,sections=0;const char* section="";
void check(bool value,const char* expression,int line)
{++checks;if(!value){++failures;if(failures<100)std::cerr<<section<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){section=name;++sections;std::cout<<name<<'\n';}
HostPatternTiming clock(double ppq=0,double bpm=120)
{HostPatternTiming h;h.playing=true;h.ppqPosition=ppq;h.bpm=bpm;return h;}
void send(ObservedMidi& owner,std::initializer_list<std::uint8_t> bytes,int offset)
{owner.event({bytes.begin(),bytes.size()},offset);}
ObservedMidiSnapshot take(ObservedMidi& owner,std::uint64_t now)
{ObservedMidiSnapshot s;owner.capture(now,s);CHECK(validateObservedMidiSnapshot(s));return s;}
void start(ObservedMidi& owner,double ppq,int frames,std::uint64_t now,PhraseMeter meter={4,4},double bpm=120)
{CHECK(owner.beginBlock(clock(ppq,bpm),meter,frames,external,now));}
void near(double actual,double expected){CHECK(std::abs(actual-expected)<1e-9);}
void note(const ObservedMidiSnapshot& s,int i,double at,double duration,int pitch,int velocity,int channel,bool held)
{CHECK(i<s.noteCount);if(i>=s.noteCount)return;const auto& n=s.notes[i];near(n.startBeats,at);near(n.durationBeats,duration);
 CHECK(n.pitch==pitch&&n.velocity==velocity&&n.channel==channel&&n.held==held);}
void timing_notes_and_window()
{
    begin(__func__);static_assert(!std::is_copy_constructible_v<ObservedMidi> && !std::is_copy_assignable_v<ObservedMidi>);
    CHECK(maximumObservedMidiNotes==256&&maximumObservedMidiEvents==8192&&observedMidiFreshnessMs==2000&&observedMidiBars==4);
    ObservedMidi owner;CHECK(owner.prepare(48000));start(owner,0,48000,1000);
    send(owner,{0x90,60,90},0);send(owner,{0x80,60,64},240);send(owner,{0x91,61,80},120);owner.endBlock();auto s=take(owner,1000);
    CHECK(s.availability==fresh&&s.reason==ObservedMidiReason::none&&s.noteCount==2&&!s.truncated&&s.ageMs==0);
    CHECK((s.meter==PhraseMeter{4,4})&&s.bpm==120&&s.windowBeats==16);note(s,0,14,.01,60,90,1,false);note(s,1,14.005,1.995,61,80,2,true);
    start(owner,2,24000,1001);send(owner,{0x91,61,0},12000);owner.endBlock();s=take(owner,1001);note(s,1,13.005,2.495,61,80,2,false);
    CHECK(owner.prepare(48000));start(owner,0,24000,1100,{7,8});send(owner,{0x90,40,100},12000);send(owner,{0x80,40,0},18000);owner.endBlock();s=take(owner,1100);
    CHECK((s.meter==PhraseMeter{7,8})&&s.windowBeats==14);note(s,0,13.5,.25,40,100,1,false);
    CHECK(owner.prepare(48000));start(owner,0,24000,1200);send(owner,{0x90,60,100},0);send(owner,{0x91,60,80},0);send(owner,{0x90,60,50},6000);
    send(owner,{0x90,60,0},12000);send(owner,{0x81,60,90},18000);send(owner,{0x80,99,0},100);owner.endBlock();s=take(owner,1200);
    CHECK(s.noteCount==3);note(s,0,15,.25,60,100,1,false);note(s,1,15,.75,60,80,2,false);note(s,2,15.25,.25,60,50,1,false);
    CHECK(owner.prepare(48000));start(owner,0,24000,1300);send(owner,{0x90,10,1},0);send(owner,{0x91,10,2},0);send(owner,{0x92,10,3},0);
    send(owner,{0xb0,120,0},6000);send(owner,{0xb1,123,0},12000);send(owner,{0xfc},18000);owner.endBlock();s=take(owner,1300);
    CHECK(s.noteCount==3);note(s,0,15,.25,10,1,1,false);note(s,1,15,.5,10,2,2,false);note(s,2,15,.75,10,3,3,false);
    CHECK(owner.prepare(48000));start(owner,0,24000,1400);send(owner,{0x90,30,127},12000);send(owner,{0x80,30,0},12000);
    send(owner,{0x9f,0,1},18000);send(owner,{0x8f,0,0},6000);owner.endBlock();s=take(owner,1400);
    CHECK(s.noteCount==2);note(s,0,15.5,0,30,127,1,false);note(s,1,15.75,0,0,1,16,false);
    // Fixed input order is retained, rather than silently sorting message offsets.
    CHECK(owner.prepare(48000));start(owner,0,24000,1500);send(owner,{0x90,1,100},12000);send(owner,{0x90,2,100},100);owner.endBlock();s=take(owner,1500);
    CHECK(s.noteCount==2&&s.notes[0].pitch==1&&s.notes[1].pitch==2&&s.notes[0].startBeats>s.notes[1].startBeats);
    // Partitions of the same actual elapsed events have the same trailing window.
    ObservedMidi whole,partition;CHECK(whole.prepare(48000)&&partition.prepare(48000));start(whole,0,48000,1600);send(whole,{0x90,60,90},12000);send(whole,{0x80,60,0},36000);whole.endBlock();
    for(int n=0;n<4;++n){start(partition,n*.5,12000,1600+n);if(n==1)send(partition,{0x90,60,90},0);if(n==3)send(partition,{0x80,60,0},0);partition.endBlock();}
    const auto a=take(whole,1600),b=take(partition,1603);CHECK(a.noteCount==1&&b.noteCount==1);if(a.noteCount&&b.noteCount){near(a.notes[0].startBeats,b.notes[0].startBeats);near(a.notes[0].durationBeats,b.notes[0].durationBeats);}
    // Crop a long-held onset, retain an exactly touching completed note, then drop it strictly outside.
    CHECK(owner.prepare(48000));start(owner,0,48000,1700);send(owner,{0x90,60,100},0);send(owner,{0x90,61,99},0);send(owner,{0x80,61,0},0);owner.endBlock();
    for(int n=1;n<8;++n){start(owner,n*2.,48000,1700+n);owner.endBlock();}s=take(owner,1707);CHECK(s.noteCount==2);note(s,0,0,16,60,100,1,true);note(s,1,0,0,61,99,1,false);
    start(owner,16,48000,1708);owner.endBlock();s=take(owner,1708);CHECK(s.noteCount==1);note(s,0,0,16,60,100,1,true);
    // Continuity tolerance, changing tempo, present sample jumps, seeks and loop-origin wraps.
    CHECK(owner.prepare(48000));auto h=clock();h.samplePosition=-480;CHECK(owner.beginBlock(h,PhraseMeter{},480,external,1800));send(owner,{0x90,60,100},0);owner.endBlock();
    h=clock(.02+1./24000);h.samplePosition=0;CHECK(owner.beginBlock(h,PhraseMeter{},480,external,1801));owner.endBlock();CHECK(take(owner,1801).noteCount==1);
    h=clock(.04+1./24000,60);h.samplePosition=480;CHECK(owner.beginBlock(h,PhraseMeter{},480,external,1802));owner.endBlock();CHECK(take(owner,1802).noteCount==1);
    h=clock(.05+1./24000,60);h.samplePosition=961;CHECK(owner.beginBlock(h,PhraseMeter{},480,external,1803));owner.endBlock();CHECK(take(owner,1803).noteCount==0);
    start(owner,3,480,1810);send(owner,{0x90,60,100},0);owner.endBlock();start(owner,0,480,1811);owner.endBlock();CHECK(take(owner,1811).noteCount==0);
    start(owner,.02,480,1812);send(owner,{0x90,60,100},0);owner.endBlock();start(owner,.04,480,1813,{7,8});owner.endBlock();s=take(owner,1813);CHECK(s.noteCount==0&&s.windowBeats==14);
    start(owner,.06,480,1814,{7,8});send(owner,{0x90,60,100},0);owner.endBlock();start(owner,.08,480,1813,{7,8});owner.endBlock();CHECK(take(owner,1813).noteCount==0);
    CHECK(owner.prepare(48000));start(owner,0,480,1900);send(owner,{0x90,60,100},0);owner.endBlock();start(owner,.0202,480,1901);owner.endBlock();CHECK(take(owner,1901).noteCount==0);
    for(int kind=0;kind<13;++kind){CHECK(owner.prepare(48000));auto invalid=clock();std::optional<PhraseMeter> meter=PhraseMeter{};int frames=64;auto mode=external;ObservedMidiReason reason=ObservedMidiReason::invalidTiming;
        if(kind==0){invalid.bpm.reset();reason=ObservedMidiReason::missingTiming;}if(kind==1){invalid.ppqPosition.reset();reason=ObservedMidiReason::missingTiming;}if(kind==2){meter.reset();reason=ObservedMidiReason::missingTiming;}
        if(kind==3)invalid.bpm=19;if(kind==4)invalid.bpm=401;if(kind==5)invalid.bpm=std::numeric_limits<double>::quiet_NaN();if(kind==6)invalid.ppqPosition=std::numeric_limits<double>::infinity();if(kind==7)invalid.ppqPosition=16777217;
        if(kind==8)meter=PhraseMeter{4,3};if(kind==9)invalid.samplePosition=std::numeric_limits<std::int64_t>::max();
        if(kind==10){frames=0;reason=ObservedMidiReason::invalidBlock;}if(kind==11){frames=65537;reason=ObservedMidiReason::invalidBlock;}if(kind==12){mode=internal;reason=ObservedMidiReason::internalMode;}
        CHECK(!owner.beginBlock(invalid,meter,frames,mode,2000+kind));send(owner,{0x90,60,100},0);owner.endBlock();s=take(owner,2000+kind);
        CHECK(s.availability==unavailable&&s.reason==reason&&s.noteCount==0&&!s.meter&&!s.bpm&&s.windowBeats==0&&!s.truncated);}
    for(double rate:{8000.,384000.}){CHECK(owner.prepare(rate));CHECK(owner.beginBlock(clock(-16777216,20),PhraseMeter{1,32},1,external,2100));owner.endBlock();CHECK(take(owner,2100).reason==ObservedMidiReason::noNotes);}
    for(double rate:{7999.,384001.,0.,std::numeric_limits<double>::quiet_NaN()}){CHECK(!owner.prepare(rate));CHECK(!owner.beginBlock(clock(),PhraseMeter{},64,external,2200));owner.endBlock();CHECK(take(owner,2200).reason==ObservedMidiReason::unprepared);}
    CHECK(owner.prepare(48000));h=clock(16777216,400);h.samplePosition=std::numeric_limits<std::int64_t>::max()-64;CHECK(owner.beginBlock(h,PhraseMeter{32,1},64,external,2300));owner.endBlock();s=take(owner,2300);CHECK(s.windowBeats==512);
    CHECK(owner.prepare(48000));start(owner,0,64,2400);send(owner,{0x90,60,100},0);start(owner,1,64,2401);CHECK(take(owner,2401).reason==ObservedMidiReason::invalidBlock);owner.endBlock();CHECK(take(owner,2401).noteCount==0);
}
void capacity_transport_and_age()
{
    begin(__func__);ObservedMidi owner;CHECK(owner.prepare(48000));start(owner,0,64,100);
    for(int n=0;n<256;++n)send(owner,{std::uint8_t(0x90+n/128),std::uint8_t(n%128),100},0);owner.endBlock();auto s=take(owner,100);CHECK(s.noteCount==256&&!s.truncated&&s.notes[0].pitch==0&&s.notes[255].pitch==127);
    start(owner,64./24000,64,101);send(owner,{0x92,0,90},0);send(owner,{0x80,0,0},1);send(owner,{0x80,1,0},2);owner.endBlock();s=take(owner,101);
    CHECK(s.noteCount==256&&s.truncated&&s.notes[0].pitch==1&&!s.notes[0].held&&s.notes[1].held&&s.notes[255].channel==3&&s.notes[255].held);
    start(owner,128./24000,64,102);for(int c=0;c<16;++c)send(owner,{std::uint8_t(0xb0+c),123,0},20);owner.endBlock();CHECK(take(owner,102).truncated);
    double origin=192./24000;for(int n=0;n<6;++n){start(owner,origin,65536,103+n);owner.endBlock();origin+=65536./24000;}s=take(owner,108);CHECK(s.noteCount==0&&!s.truncated&&s.reason==ObservedMidiReason::noNotes);
    CHECK(owner.prepare(48000));start(owner,0,64,200);send(owner,{0x90,60,100},0);for(int n=1;n<8192;++n)owner.event({},0);owner.endBlock();CHECK(take(owner,200).noteCount==1);
    start(owner,64./24000,64,201);for(int n=0;n<8193;++n)owner.event({},0);owner.endBlock();s=take(owner,201);CHECK(s.reason==ObservedMidiReason::inputCapacity&&s.availability==unavailable&&!s.meter&&!s.truncated);
    start(owner,1,64,202);send(owner,{0x90,61,100},0);owner.endBlock(false);CHECK(take(owner,202).reason==ObservedMidiReason::inputCapacity);
    start(owner,2,64,203);send(owner,{0x90,62,100},0);owner.endBlock();CHECK(take(owner,203).noteCount==1&&!take(owner,203).truncated);
    CHECK(owner.prepare(48000));start(owner,0,64,300);send(owner,{0x90,60},0);send(owner,{0x90,60,100,0},0);send(owner,{0x90,128,100},0);send(owner,{0x90,60,128},0);
    send(owner,{0x80,60},0);send(owner,{0xb0,123},0);send(owner,{0xfc,0},0);send(owner,{0x90,60,100},-1);send(owner,{0x90,60,100},64);send(owner,{0xf0,1,2,3,4,0xf7},0);
    send(owner,{0xc0,60},0);send(owner,{0xe0,0,64},0);owner.endBlock();s=take(owner,300);CHECK(s.reason==ObservedMidiReason::noNotes&&s.noteCount==0);
    start(owner,64./24000,64,301);send(owner,{0x90,60,100},0);send(owner,{0xb0,123,0,0},1);send(owner,{0xfc,0},2);owner.endBlock();CHECK(take(owner,301).notes[0].held);
    CHECK(owner.prepare(48000));start(owner,0,480,400);send(owner,{0x90,60,100},0);owner.endBlock();s=take(owner,2400);CHECK(s.availability==fresh&&s.ageMs==2000);s=take(owner,2401);CHECK(s.availability==stale&&s.reason==ObservedMidiReason::stalePublisher&&s.ageMs==2001);
    s=take(owner,399);CHECK(s.availability==stale&&s.reason==ObservedMidiReason::clockRollback&&s.ageMs==std::numeric_limits<std::uint64_t>::max());CHECK(take(owner,400).availability==fresh);
    HostPatternTiming stopped;CHECK(!owner.beginBlock(stopped,{},480,external,900));owner.endBlock();s=take(owner,900);
    CHECK(s.availability==stale&&s.reason==ObservedMidiReason::stopped&&s.ageMs==500&&s.noteCount==1&&!s.notes[0].held);near(s.notes[0].durationBeats,.02);
    CHECK(!owner.beginBlock(stopped,{},480,external,9999));owner.endBlock();s=take(owner,9999);CHECK(s.reason==ObservedMidiReason::stopped&&s.ageMs==9599);CHECK(take(owner,399).reason==ObservedMidiReason::clockRollback);
    start(owner,.02,480,10000);owner.endBlock();CHECK(take(owner,10000).noteCount==1); // Same prior valid PPQ end, no invented stop advancement.
    owner.release();s=take(owner,10001);CHECK(s.reason==ObservedMidiReason::unprepared&&s.noteCount==0&&s.ageMs==1);
    CHECK(owner.prepare(48000));s=take(owner,10002);CHECK(s.reason==ObservedMidiReason::noNotes&&s.ageMs==2);s=take(owner,9999);CHECK(s.availability==unavailable&&s.reason==ObservedMidiReason::noNotes&&s.ageMs==std::numeric_limits<std::uint64_t>::max());
    CHECK(!owner.beginBlock(stopped,{},64,external,11000));owner.endBlock();CHECK(take(owner,11000).reason==ObservedMidiReason::noNotes);
    // Transport-boundary validator fixtures never enter the mailbox.
    ObservedMidiSnapshot valid;CHECK(validateObservedMidiSnapshot(valid));valid.meter=PhraseMeter{};valid.bpm=120;valid.windowBeats=16;CHECK(validateObservedMidiSnapshot(valid));
    valid.availability=fresh;valid.reason=ObservedMidiReason::none;valid.noteCount=1;valid.notes[0]={15,1,127,127,16,false};CHECK(validateObservedMidiSnapshot(valid));
    for(int kind=0;kind<27;++kind){auto bad=valid;
        if(kind==0)bad.availability=static_cast<ObservedMidiAvailability>(3);if(kind==1)bad.reason=static_cast<ObservedMidiReason>(11);if(kind==2)bad.noteCount=257;
        if(kind==3)bad.meter.reset();if(kind==4)bad.bpm.reset();if(kind==5)bad.meter=PhraseMeter{0,4};if(kind==6)bad.bpm=401;if(kind==7)bad.windowBeats=15;if(kind==8)bad.windowBeats=std::numeric_limits<double>::quiet_NaN();
        if(kind==9)bad.notes[0].startBeats=-1;if(kind==10)bad.notes[0].durationBeats=2;if(kind==11)bad.notes[0].durationBeats=-1;if(kind==12)bad.notes[0].startBeats=std::numeric_limits<double>::quiet_NaN();
        if(kind==13)bad.notes[0].pitch=128;if(kind==14)bad.notes[0].velocity=0;if(kind==15)bad.notes[0].channel=0;if(kind==16)bad.notes[0].channel=17;
        if(kind==17)bad.notes[1].held=true;if(kind==18)bad.ageMs=2001;if(kind==19)bad.reason=ObservedMidiReason::noNotes;
        if(kind==20)bad.bpm=19;if(kind==21)bad.bpm=std::numeric_limits<double>::quiet_NaN();if(kind==22)bad.notes[0].durationBeats=std::numeric_limits<double>::quiet_NaN();
        if(kind==23)bad.notes[0].durationBeats=std::numeric_limits<double>::infinity();if(kind==24)bad.notes[0].startBeats=17;if(kind==25)bad.notes[1].durationBeats=1;if(kind==26)bad.windowBeats=std::numeric_limits<double>::infinity();CHECK(!validateObservedMidiSnapshot(bad));}
    valid.notes[0].durationBeats=0;CHECK(validateObservedMidiSnapshot(valid));valid.ageMs=2000;CHECK(validateObservedMidiSnapshot(valid));
    for(auto reason:{ObservedMidiReason::stopped,ObservedMidiReason::stalePublisher,ObservedMidiReason::clockRollback}){auto aged=valid;aged.availability=stale;aged.reason=reason;aged.ageMs=std::numeric_limits<std::uint64_t>::max();CHECK(validateObservedMidiSnapshot(aged));}
    auto bad=valid;bad.availability=stale;CHECK(!validateObservedMidiSnapshot(bad));bad.reason=ObservedMidiReason::noNotes;CHECK(!validateObservedMidiSnapshot(bad));bad=valid;bad.availability=unavailable;bad.reason=ObservedMidiReason::stopped;CHECK(!validateObservedMidiSnapshot(bad));
    bad=ObservedMidiSnapshot{};bad.reason=ObservedMidiReason::none;CHECK(!validateObservedMidiSnapshot(bad));bad=valid;bad.noteCount=0;CHECK(!validateObservedMidiSnapshot(bad));bad=ObservedMidiSnapshot{};bad.notes[255].pitch=61;CHECK(!validateObservedMidiSnapshot(bad));
}
thread_local bool tracking=false;thread_local int allocations=0,frees=0;
int __cdecl hook(int kind,void*,size_t,int,long,const unsigned char*,int)
{if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE)++frees;}return 1;}
void coherent_mailbox_and_allocations()
{
    begin(__func__);auto storage=std::make_unique<ObservedMidi>();auto& owner=*storage;CHECK(owner.prepare(48000));
    // A slow consumer gets the latest publication; prepare/release do not reset slot ownership.
    for(int n=0;n<200;++n){CHECK(owner.prepare(48000));start(owner,n,64,100+n);send(owner,{0x90,std::uint8_t(n%128),100},0);owner.endBlock();}
    auto s=take(owner,299);CHECK(s.noteCount==1&&s.notes[0].pitch==71&&s.ageMs==0);owner.release();CHECK(take(owner,299).reason==ObservedMidiReason::unprepared);
    CHECK(owner.prepare(48000));start(owner,1,256,10,{7,8},180);send(owner,{0x91,1,2},64);send(owner,{0x81,1,0},160);owner.endBlock();
    auto previous=_CrtSetAllocHook(hook);std::atomic<bool> go=false,producerGood=true;std::atomic<int> producerAllocations=0,producerFrees=0;
    std::thread producer([&]{while(!go.load())std::this_thread::yield();allocations=0;frees=0;
        for(int n=2;n<=2000;++n){tracking=true;if(n%23==0)owner.release();if(!owner.prepare(48000))producerGood=false;
            if(!owner.beginBlock(clock(n,120+(n%2)*60),n%2?PhraseMeter{7,8}:PhraseMeter{},256,external,n*10))producerGood=false;
            send(owner,{std::uint8_t(0x90+n%16),std::uint8_t(n%128),std::uint8_t(n%126+1)},64);send(owner,{std::uint8_t(0x80+n%16),std::uint8_t(n%128),0},160);owner.endBlock();tracking=false;}
        producerAllocations=allocations;producerFrees=frees;});
    allocations=0;frees=0;go=true;int wholeSnapshots=0;
    for(int n=0;n<4000;++n){ObservedMidiSnapshot out;tracking=true;owner.capture(1000000,out);tracking=false;CHECK(validateObservedMidiSnapshot(out));
        if(out.noteCount){++wholeSnapshots;CHECK(out.noteCount==1&&out.availability==stale&&out.reason==ObservedMidiReason::stalePublisher);const auto stamp=1000000-out.ageMs;CHECK(stamp%10==0);const auto token=stamp/10;
            CHECK(token>=1&&token<=2000);CHECK(out.notes[0].pitch==token%128&&out.notes[0].velocity==token%126+1&&out.notes[0].channel==token%16+1&&!out.notes[0].held);
            CHECK(out.bpm==120+(token%2)*60);CHECK((out.meter==(token%2?PhraseMeter{7,8}:PhraseMeter{})));near(out.notes[0].durationBeats,96.*(120+(token%2)*60)/(60*48000));
            near(out.notes[0].startBeats,(token%2?14.:16.)-192.*(120+(token%2)*60)/(60*48000));}
        else CHECK(out.availability==unavailable&&(out.reason==ObservedMidiReason::noNotes||out.reason==ObservedMidiReason::unprepared));}
    producer.join();s=take(owner,20000);CHECK(s.noteCount==1&&s.notes[0].pitch==80&&s.ageMs==0&&s.availability==fresh);CHECK(producerGood);
    tracking=true;owner.release();owner.capture(20000,s);owner.prepare(48000);owner.capture(20000,s);owner.beginBlock(clock(),PhraseMeter{},64,internal,20001);owner.event({},0);owner.endBlock();owner.capture(20001,s);tracking=false;
    _CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0&&producerAllocations==0&&producerFrees==0);CHECK(s.reason==ObservedMidiReason::internalMode);
    std::cout<<"MAILBOX producer publications=2000 control captures=4000 whole note snapshots="<<wholeSnapshots<<" allocations="<<allocations+producerAllocations<<" frees="<<frees+producerFrees<<'\n';
}
}
int main(int argc,char** argv)
{
    FILETIME creation{},exit{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user);ULARGE_INTEGER stamp{};stamp.LowPart=creation.dwLowDateTime;stamp.HighPart=creation.dwHighDateTime;
    std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<stamp.QuadPart<<'\n';for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
    timing_notes_and_window();capacity_transport_and_age();coherent_mailbox_and_allocations();CHECK(sections==3);
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
}
