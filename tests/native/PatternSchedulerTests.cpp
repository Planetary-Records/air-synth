#include "PatternScheduler.h"
#include "RoutedSynthEngine.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#include <memory>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
using namespace airack;
using Status=PatternPlaybackStatus;
constexpr auto internal=PlaybackMode::internalPattern;
int checks=0, failures=0, sections=0;
void check(bool value,const char* expression,int line) {
    ++checks; if(!value) { ++failures; std::cout<<"FAIL "<<line<<" "<<expression<<'\n'; }
}
#define CHECK(x) check(bool(x),#x,__LINE__)
void section(const char* name) { ++sections; std::cout<<"SECTION "<<name<<'\n'; }
PatternEvent on(int offset,int pitch=60,int velocity=100,int channel=1) {
    return {offset,true,static_cast<std::uint8_t>(pitch),static_cast<std::uint8_t>(velocity),static_cast<std::uint8_t>(channel)};
}
PatternEvent off(int offset,int pitch=60,int channel=1) { return {offset,false,static_cast<std::uint8_t>(pitch),0,static_cast<std::uint8_t>(channel)}; }
PatternDocument pattern(std::initializer_list<PatternNote> notes) {
    PatternDocument p; p.noteCount=static_cast<std::uint16_t>(notes.size()); std::copy(notes.begin(),notes.end(),p.notes.begin()); return p;
}
HostPatternTiming clockAt(double ppq=0,double bpm=120) { HostPatternTiming h; h.playing=true; h.ppqPosition=ppq; h.bpm=bpm; return h; }
void expect(const PatternEventBuffer& b,std::initializer_list<PatternEvent> expected) {
    CHECK(b.count==expected.size()); size_t i=0;
    for(auto event:expected) { if(i<b.count) CHECK(b.events[i]==event); ++i; }
}
void ready(PatternScheduler& s,const PatternDocument& p,double rate=48000,int block=65536) { CHECK(s.prepare(rate,block)); CHECK(s.setPattern(p)); }
std::vector<PatternEvent> collect(const PatternDocument& p,double rate,double bpm,double start,int total,const std::vector<int>& blocks) {
    PatternScheduler s; ready(s,p,rate); PatternEventBuffer b; std::vector<PatternEvent> events;
    int cursor=0; size_t index=0;
    while(cursor<total) {
        int n=std::min(blocks[index++%blocks.size()],total-cursor);
        auto h=clockAt(start+cursor*bpm/(60*rate),bpm); h.samplePosition=cursor;
        CHECK(s.processBlock(h,n,internal,b)==Status::playing);
        for(size_t i=0;i<b.count;++i) { auto event=b.events[i]; CHECK(event.sampleOffset>=0&&event.sampleOffset<n); event.sampleOffset+=cursor; events.push_back(event); }
        cursor+=n;
    }
    return events;
}
void effective_swing_and_incomplete_pairs_preserve_source() {
    section(__func__); // Catches destructive swing, implicit grid quantization and wrong incomplete-pair scaling.
    auto p=pattern({{0,.1},{.25,.1},{.375,.1},{.49,.01}}); p.swing=1;
    auto original=p; PatternPlaybackNotes n; CHECK(makePatternPlaybackNotes(p,n)); CHECK(p==original);
    CHECK(n.noteCount==4); CHECK(n.notes[1].startBeats==.375); CHECK(n.notes[2].startBeats==.4375);
    CHECK(std::abs(n.notes[3].startBeats-.495)<1e-14); CHECK(n.notes[1].endBeats==.475);
    for(size_t i=n.noteCount;i<maximumPatternNotes;++i) CHECK(n.notes[i]==PatternPlaybackNote{});
    p=pattern({{.25,.01},{.3125,.0625}}); p.meter={3,32}; p.grid=PatternGrid::sixteenth; p.swing=1;
    CHECK(makePatternPlaybackNotes(p,n)); CHECK(n.lengthBeats==.375); CHECK(n.notes[0].startBeats==.3125); CHECK(n.notes[1].startBeats==.34375); CHECK(n.notes[1].endBeats==.375);
    p=pattern({{.1,.02}}); p.meter={1,32}; p.grid=PatternGrid::quarter; p.swing=1;
    CHECK(makePatternPlaybackNotes(p,n)); CHECK(n.notes[0].startBeats==.1);
    for(auto grid:{PatternGrid::quarter,PatternGrid::eighth,PatternGrid::sixteenth,PatternGrid::thirtySecond,PatternGrid::sixtyFourth}) {
        p.grid=grid; p.swing=0; CHECK(makePatternPlaybackNotes(p,n)); CHECK(n.notes[0].startBeats==.1);
    }
    auto saved=n; p.swing=std::numeric_limits<double>::quiet_NaN(); CHECK(!makePatternPlaybackNotes(p,n)); CHECK(n==saved);
}
void sample_tables_and_partitions() {
    section(__func__); // Catches buffer-relative rounding, wrong tie direction and omitted exact boundary events.
    auto p=pattern({{0,.125,60,91,1},{.25,.125,64,87,2},{.375,.125,67,110,1}}); p.swing=1;
    std::vector<PatternEvent> expected {on(0,60,91),off(3000),on(9000,64,87,2),on(10500,67,110),off(12000,64,2),off(13500,67)};
    for(auto blocks:{std::vector<int>{64},std::vector<int>{512},std::vector<int>{1,17,503,64,997}})
        CHECK(collect(p,48000,120,0,14000,blocks)==expected);
    auto simple=pattern({{.125,.125}});
    for(auto rate:{44100.,48000.,96000.}) for(auto bpm:{20.,120.,400.}) {
        // Independent rational: one eighth of a quarter is 7.5*rate/BPM samples.
        int a=int(std::floor(7.5*rate/bpm+.5)),z=int(std::floor(15*rate/bpm+.5));
        const std::vector<PatternEvent> want {on(a),off(z)};
        for(auto blocks:{std::vector<int>{64},std::vector<int>{512},std::vector<int>{71,13,809}})
            CHECK(collect(simple,rate,bpm,0,z+2,blocks)==want);
    }
    auto tie=pattern({{.5/24000.,1./24000.}});
    CHECK(collect(tie,48000,120,0,4,{1})==std::vector<PatternEvent>({on(1),off(2)}));
}
void negative_ppq_start_seek_and_chase() {
    section(__func__); // Catches wrong negative modulo, duplicated entry onsets and missing chased ends.
    auto p=pattern({{3.5,.5,61,99,2},{.5,.5,62,80,1}}); PatternScheduler s; ready(s,p); PatternEventBuffer b;
    CHECK(s.processBlock(clockAt(-.25),6001,internal,b)==Status::playing); expect(b,{on(0,61,99,2),off(6000,61,2)});
    CHECK(s.processBlock(clockAt(.5),6000,internal,b)==Status::playing); expect(b,{on(0,62,80)});
    CHECK(s.processBlock(clockAt(.75),6001,internal,b)==Status::playing); expect(b,{off(6000,62)});
    CHECK(s.processBlock(clockAt(1),10,internal,b)==Status::playing); expect(b,{});
    CHECK(s.processBlock(clockAt(.75),10,internal,b)==Status::playing); expect(b,{on(0,62,80)});
    CHECK(s.processBlock(clockAt(.75+11./24000),10,internal,b)==Status::playing); expect(b,{off(0,62),on(0,62,80)});
    for(double ppq:{-16777216.,16777216.}) { CHECK(s.processBlock(clockAt(ppq),1,internal,b)==Status::playing); }
    CHECK(s.processBlock(clockAt(16777217.),1,internal,b)==Status::invalidTiming);
    // Negative half-sample event coordinates round toward the later sample (zero).
    ready(s,pattern({{0,1}})); CHECK(s.processBlock(clockAt(.5/24000),2,internal,b)==Status::playing); expect(b,{on(0)});
}
void phrase_and_host_loop_inside_block() {
    section(__func__); // Catches missing boundary release, wrong host-loop length and omitted destination chase.
    auto p=pattern({{0,.125},{3.75,.25,62,100,2}}); PatternScheduler s; ready(s,p); PatternEventBuffer b;
    CHECK(s.processBlock(clockAt(3.875),6001,internal,b)==Status::playing);
    expect(b,{on(0,62,100,2),off(3000,62,2),on(3000),off(6000)});
    p=pattern({{.25,.5,65,77,3}}); ready(s,p);
    auto h=clockAt(.5); h.looping=true; h.loop=PatternLoopRange{.5,1};
    CHECK(s.processBlock(h,18001,internal,b)==Status::playing);
    expect(b,{on(0,65,77,3),off(6000,65,3),on(12000,65,77,3),off(18000,65,3)});
    ready(s,pattern({{0,4}})); h=clockAt(.9); h.looping=true; h.loop=PatternLoopRange{.1,1};
    CHECK(s.processBlock(h,2401,internal,b)==Status::playing); expect(b,{on(0),off(2400),on(2400)});
    h=clockAt(.10004166666666667); h.looping=true; h.loop=PatternLoopRange{.1,1};
    CHECK(s.processBlock(h,100,internal,b)==Status::playing); expect(b,{});
}
void tempo_changes_and_optional_sample_continuity() {
    section(__func__); // Catches premature tempo-change release and ignored one-frame host sample seeks/overflow.
    PatternScheduler s; ready(s,pattern({{0,1}})); PatternEventBuffer b;
    auto h=clockAt(); h.samplePosition=100;
    CHECK(s.processBlock(h,6000,internal,b)==Status::playing); expect(b,{on(0)});
    h=clockAt(.25,240); h.samplePosition=6100;
    CHECK(s.processBlock(h,9001,internal,b)==Status::playing); expect(b,{off(9000)});
    ready(s,pattern({{0,1}})); h=clockAt(); h.samplePosition=0; s.processBlock(h,64,internal,b);
    h=clockAt(64./24000); h.samplePosition=65;
    CHECK(s.processBlock(h,64,internal,b)==Status::playing); expect(b,{off(0),on(0)});
    for(auto sample:{std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()}) {
        ready(s,pattern({{0,1}})); h=clockAt(); h.samplePosition=sample; s.processBlock(h,64,internal,b);
        h=clockAt(64./24000); h.samplePosition.reset(); s.processBlock(h,64,internal,b); expect(b,{});
    }
    ready(s,pattern({{0,1}})); h=clockAt(); s.processBlock(h,64,internal,b);
    h=clockAt(64.2/24000); s.processBlock(h,64,internal,b); expect(b,{});
    h=clockAt(128.6/24000); s.processBlock(h,64,internal,b); expect(b,{off(0),on(0)});
}
void overlap_key_ownership_and_same_sample_order() {
    section(__func__); // Catches premature same-key off, order instability and confusing refs with unique keys.
    auto p=pattern({{0,.5,60,90,1},{.25,.5,60,110,1},{.5,.25,60,80,2},{.75,.25,64,70,1}});
    PatternScheduler s; ready(s,p); PatternEventBuffer b;
    s.processBlock(clockAt(),6001,internal,b); expect(b,{on(0,60,90),on(6000,60,110)}); CHECK(s.heldNoteCount()==2);
    s.processBlock(clockAt(6001./24000),18000,internal,b);
    expect(b,{on(5999,60,80,2),off(11999),off(11999,60,2),on(11999,64,70),off(17999,64)}); CHECK(s.heldNoteCount()==0);
    p=pattern({{0,.1,70,90,2},{0,.1,60,80,1},{0,.1,65,100,3}}); ready(s,p);
    s.processBlock(clockAt(),1,internal,b); expect(b,{on(0,70,90,2),on(0,60,80),on(0,65,100,3)});
    s.release(b); CHECK(b.count==3); CHECK(s.heldNoteCount()==0); for(size_t i=0;i<b.count;++i) CHECK(!b.events[i].noteOn&&b.events[i].velocity==0&&b.events[i].sampleOffset==0);
    p={}; p.noteCount=256;
    for(int i=0;i<256;++i) p.notes[size_t(i)]={0,.25,static_cast<std::uint8_t>(60+i%2),100,static_cast<std::uint8_t>(1+i%16)};
    ready(s,p); s.processBlock(clockAt(),1,internal,b); CHECK(b.count==256); CHECK(s.heldNoteCount()==256);
    s.release(b); CHECK(b.count==16); CHECK(s.heldNoteCount()==0);
}
void subsample_notes_phrase_end_and_capacity() {
    section(__func__); // Catches unmatched rounded on/off, last-frame onset escape, and partial music on overflow.
    auto tiny=std::numeric_limits<double>::denorm_min(); auto p=pattern({{0,tiny},{std::nextafter(4.,0.),tiny,62,100,2}});
    CHECK(collect(p,48000,120,0,96001,{512})==std::vector<PatternEvent>({on(0),off(1),on(95999,62,100,2),off(96000,62,2),on(96000)}));
    PatternScheduler s; ready(s,pattern({{0,4}}),8000); PatternEventBuffer b;
    auto h=clockAt(0,400); s.processBlock(h,1,internal,b); expect(b,{on(0)});
    h=clockAt(1./1200,400); h.looping=true; h.loop=PatternLoopRange{0,1./1200};
    CHECK(s.processBlock(h,65536,internal,b)==Status::eventCapacity); expect(b,{off(0)}); CHECK(s.heldNoteCount()==0);
    h=clockAt(0,400); CHECK(s.processBlock(h,1,internal,b)==Status::playing); expect(b,{on(0)});
    h=clockAt(1./1200,400); h.looping=true; h.loop=PatternLoopRange{0,1./1200};
    CHECK(s.processBlock(h,4,internal,b)==Status::playing); CHECK(b.count==8); CHECK(s.heldNoteCount()==1);
}
void missing_invalid_stopped_mode_pattern_reprepare() {
    section(__func__); // Catches guessing missing metadata and losing deferred releases on state/preparation changes.
    PatternScheduler s; PatternEventBuffer b; auto p=pattern({{0,1}}); ready(s,p);
    const auto seed=[&]{ auto h=clockAt(); CHECK(s.processBlock(h,1,internal,b)==Status::playing); };
    seed(); auto h=clockAt(); h.bpm.reset(); CHECK(s.processBlock(h,1,internal,b)==Status::missingTiming); expect(b,{off(0)});
    seed(); h=clockAt(); h.ppqPosition.reset(); CHECK(s.processBlock(h,1,internal,b)==Status::missingTiming); expect(b,{off(0)});
    seed(); h=clockAt(); h.looping=true; CHECK(s.processBlock(h,1,internal,b)==Status::missingTiming); expect(b,{off(0)});
    for(double bpm:{19.,401.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        seed(); h=clockAt(0,bpm); CHECK(s.processBlock(h,1,internal,b)==Status::invalidTiming); expect(b,{off(0)});
    }
    for(auto range:{PatternLoopRange{1,1},PatternLoopRange{2,1},PatternLoopRange{0,1e-9},PatternLoopRange{0,std::numeric_limits<double>::infinity()}}) {
        seed(); h=clockAt(); h.looping=true; h.loop=range; CHECK(s.processBlock(h,1,internal,b)==Status::invalidTiming); expect(b,{off(0)});
    }
    seed(); h=clockAt(); h.playing=false; CHECK(s.processBlock(h,1,internal,b)==Status::stopped); expect(b,{off(0)});
    seed(); CHECK(s.processBlock(clockAt(),1,PlaybackMode::externalMidi,b)==Status::externalMidi); expect(b,{off(0)});
    seed(); CHECK(s.processBlock(clockAt(),1,static_cast<PlaybackMode>(9),b)==Status::invalidTiming); expect(b,{off(0)});
    seed(); auto bad=p; bad.noteCount=257; CHECK(!s.setPattern(bad)); CHECK(s.pattern()==p);
    CHECK(s.setPattern(p)); s.processBlock(clockAt(1./24000),1,internal,b); expect(b,{});
    auto other=pattern({{0,1,65,90,2}}); CHECK(s.setPattern(other)); CHECK(s.prepare(96000,512)); CHECK(s.prepare(48000,512));
    s.processBlock(clockAt(),1,internal,b); expect(b,{off(0),on(0,65,90,2)});
    CHECK(s.prepare(44100,512)); h=clockAt(); h.playing=false; s.processBlock(h,1,internal,b); expect(b,{off(0,65,2)});
    s.release(b); expect(b,{});
}
void prepare_block_rejection_preserves_outputs() {
    section(__func__); // Catches invalid block mutation and rejected prepare destroying valid clock/ownership.
    PatternScheduler s; PatternEventBuffer b; CHECK(s.processBlock(clockAt(),1,internal,b)==Status::unprepared);
    ready(s,pattern({{0,1}}),48000,512); s.processBlock(clockAt(),64,internal,b); auto before=b;
    for(int count:{0,-1,513,65537}) { CHECK(s.processBlock(clockAt(),count,internal,b)==Status::invalidBlock); CHECK(b.count==before.count&&b.events==before.events); CHECK(s.heldNoteCount()==1); }
    for(double rate:{0.,7999.,384001.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) CHECK(!s.prepare(rate,512));
    for(int block:{0,-1,65537}) CHECK(!s.prepare(48000,block));
    s.processBlock(clockAt(64./24000),64,internal,b); expect(b,{}); CHECK(s.heldNoteCount()==1);
    for(double rate:{8000.,384000.}) CHECK(s.prepare(rate,65536));
}
#if defined(_MSC_VER) && defined(_DEBUG)
int allocations=0;
int __cdecl allocationHook(int kind,void*,size_t,int,long,const unsigned char*,int) { if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC) ++allocations; return 1; }
#endif
std::vector<float> audio(bool scheduled,int partition) {
    PatternScheduler s; ready(s,pattern({{0,.125,60,100,1},{.25,.125,67,90,2}}));
    RoutedSynthEngine e; CHECK(e.prepare(48000,512));
    juce::AudioBuffer<float> out(2,512); juce::MidiBuffer midi; midi.ensureSize(4096); PatternEventBuffer b;
    const std::array<PatternEvent,4> table {on(0),off(3000),on(6000,67,90,2),off(9000,67,2)};
    std::vector<float> result(24000); int cursor=0;
    while(cursor<12000) {
        const int n=std::min(partition,12000-cursor); out.setSize(2,n,false,false,true); midi.clear();
        if(scheduled) {
            CHECK(s.processBlock(clockAt(cursor/24000.),n,internal,b)==Status::playing);
            for(size_t i=0;i<b.count;++i) { auto event=b.events[i]; midi.addEvent(event.noteOn?juce::MidiMessage::noteOn(event.channel,event.pitch,event.velocity):juce::MidiMessage::noteOff(event.channel,event.pitch),event.sampleOffset); }
        } else for(auto event:table) if(event.sampleOffset>=cursor&&event.sampleOffset<cursor+n) midi.addEvent(event.noteOn?juce::MidiMessage::noteOn(event.channel,event.pitch,event.velocity):juce::MidiMessage::noteOff(event.channel,event.pitch),event.sampleOffset-cursor);
        e.render(out,midi,120);
        for(int c=0;c<2;++c) std::copy_n(out.getReadPointer(c),n,result.begin()+c*12000+cursor);
        cursor+=n;
    }
    return result;
}
void prepared_allocation_and_native_audio_equivalence() {
    section(__func__); // Catches callback/publication allocation and scheduler timing differing from independently played core MIDI.
    auto expected=audio(false,512); CHECK(audio(true,512)==expected); CHECK(audio(true,64)==expected);
    double energy=0; for(auto x:expected) { CHECK(std::isfinite(x)); energy+=x*x; } CHECK(energy>0);
    PatternScheduler s; auto p=pattern({{0,.5},{.25,.5,64,90,2}}),changed=pattern({{0,.25,70,80,3}}); ready(s,p);
    PatternEventBuffer b; auto h=clockAt();
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0; auto previous=_CrtSetAllocHook(allocationHook);
#endif
    auto first=s.processBlock(h,512,internal,b); auto changedOK=s.setPattern(changed); auto second=s.processBlock(h,512,internal,b); s.release(b);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previous); CHECK(allocations==0); std::cout<<"prepared allocations "<<allocations<<'\n';
#else
    CHECK(false); // This target's required allocation oracle is a Debug MSVC run.
#endif
    CHECK(first==Status::playing&&second==Status::playing&&changedOK); CHECK(s.heldNoteCount()==0);
}
}
namespace {
void drum_discontinuity_is_separate_from_melodic_loop_entry(){
    section("drum_discontinuity_is_separate_from_melodic_loop_entry");
    auto scheduler=std::make_unique<PatternScheduler>();auto output=std::make_unique<PatternEventBuffer>();
    CHECK(scheduler->prepare(48000,65536));HostPatternTiming host{true,120.,.99,true,PatternLoopRange{0,1},23760};
    CHECK(scheduler->processBlock(host,240,internal,*output)==Status::playing);CHECK(!output->drumStopAtStart);
    host.ppqPosition=0;host.samplePosition=0;CHECK(scheduler->processBlock(host,64,internal,*output)==Status::playing);CHECK(!output->drumStopAtStart);
    host.ppqPosition=64./24000;host.samplePosition=63;CHECK(scheduler->processBlock(host,64,internal,*output)==Status::playing);CHECK(output->drumStopAtStart);
    host.playing=false;CHECK(scheduler->processBlock(host,64,internal,*output)==Status::stopped);CHECK(output->drumStopAtStart);
    CHECK(scheduler->processBlock(host,64,internal,*output)==Status::stopped);CHECK(!output->drumStopAtStart);
}
}
int main() {
    effective_swing_and_incomplete_pairs_preserve_source(); sample_tables_and_partitions(); negative_ppq_start_seek_and_chase();
    phrase_and_host_loop_inside_block(); tempo_changes_and_optional_sample_continuity(); overlap_key_ownership_and_same_sample_order();
    subsample_notes_phrase_end_and_capacity(); missing_invalid_stopped_mode_pattern_reprepare(); prepare_block_rejection_preserves_outputs(); prepared_allocation_and_native_audio_equivalence();
    drum_discontinuity_is_separate_from_melodic_loop_entry();
    std::cout<<checks<<" checks in "<<sections<<" sections; "<<failures<<" failures\n"; return failures==0?0:1;
}

