#include "PatternIO.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#if JUCE_WINDOWS
#include <windows.h>
#endif

using namespace airack;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
void check(bool pass, int line)
{
    ++checks;
    if (!pass) { ++failures; if (failures < 45) std::cerr << section << ':' << line << " failed\n"; }
}
#define CHECK(x) check(bool(x), __LINE__)
void begin(const char* name) { section = name; ++sections; std::cout << name << '\n'; }
using Bytes = std::vector<std::uint8_t>;
Bytes bytes(const juce::MemoryBlock& b)
{
    const auto* p = static_cast<const std::uint8_t*>(b.getData());
    return b.getSize() == 0 ? Bytes{} : Bytes(p, p + b.getSize());
}
void digest(Bytes& b)
{
    const auto d = juce::SHA256(b.data(), b.size() - 32).getRawData();
    std::memcpy(b.data() + b.size() - 32, d.getData(), 32);
}
void putDouble(Bytes& b, std::size_t offset, double d)
{
    auto bits = std::bit_cast<std::uint64_t>(d);
    for (int i = 0; i < 8; ++i) b[offset + i] = static_cast<std::uint8_t>(bits >> (8 * i));
}
bool exact(const PatternDocument& a, const PatternDocument& b)
{
    if (!(a == b) || std::bit_cast<std::uint64_t>(a.swing) != std::bit_cast<std::uint64_t>(b.swing)) return false;
    for (std::size_t i = 0; i < maximumPatternNotes; ++i)
        if (std::bit_cast<std::uint64_t>(a.notes[i].startBeats) != std::bit_cast<std::uint64_t>(b.notes[i].startBeats)
            || std::bit_cast<std::uint64_t>(a.notes[i].durationBeats) != std::bit_cast<std::uint64_t>(b.notes[i].durationBeats)) return false;
    return true;
}
PatternDocument fixture()
{
    PatternDocument d; d.bars = 2; d.meter = {7, 8}; d.grid = PatternGrid::eighth; d.swing = .5;
    d.noteCount = 2; d.notes[0] = {-0.0, .25, 60, 99, 1}; d.notes[1] = {1.5, .5, 127, 1, 16};
    return d;
}
struct Event
{
    int tick, status, pitch, velocity;
    bool operator==(const Event&) const = default;
};
struct Readback { std::vector<Event> events; int numerator=0, denominator=0, meterCount=0, ends=0; double end=-1; };
Readback readMidi(const juce::MemoryBlock& block)
{
    Readback result;
    const auto b = bytes(block);
    // Independent SMF header: type zero, one track, division 0x03c0.
    const Bytes header {'M','T','h','d',0,0,0,6,0,0,0,1,3,192};
    CHECK(b.size() >= 22);
    if (b.size() < 22) return result;
    CHECK(std::equal(header.begin(), header.end(), b.begin()));
    CHECK(b[14]=='M' && b[15]=='T' && b[16]=='r' && b[17]=='k');
    juce::MemoryInputStream stream(block, false); juce::MidiFile midi;
    // Disable synthetic pair repair; JUCE still normalizes same-tick off/on order.
    CHECK(midi.readFrom(stream, false)); CHECK(midi.getNumTracks()==1); CHECK(midi.getTimeFormat()==960);
    if (midi.getNumTracks()!=1) return result;
    const auto* track = midi.getTrack(0);
    double previous = -1;
    for (int i=0; i<track->getNumEvents(); ++i)
    {
        const auto& m=track->getEventPointer(i)->message;
        CHECK(m.getTimeStamp() >= previous); previous=m.getTimeStamp();
        CHECK(!m.isTempoMetaEvent());
        if(m.isTimeSignatureMetaEvent()) { ++result.meterCount; m.getTimeSignatureInfo(result.numerator,result.denominator); CHECK(m.getTimeStamp()==0); }
        else if(m.isEndOfTrackMetaEvent()) { ++result.ends; result.end=m.getTimeStamp(); CHECK(i==track->getNumEvents()-1); }
        else { CHECK(m.isNoteOn() || m.isNoteOff()); if(m.getRawDataSize()==3) result.events.push_back({int(m.getTimeStamp()),m.getRawData()[0],m.getNoteNumber(),m.getVelocity()}); }
    }
    CHECK(result.meterCount==1); CHECK(result.ends==1);
    return result;
}
juce::MemoryBlock midi(const PatternDocument& d) { juce::MemoryBlock b; CHECK(encodePatternMidi(d,b)); return b; }
struct RawReadback { bool valid=false; std::vector<Event> events; };
RawReadback readRawMidi(const juce::MemoryBlock& block)
{
    // Test-only reader for this exporter: type0/one track/960PPQ, notes and meta events.
    // Preserve serialized order; no JUCE parser, sorting, matching, or timing helper.
    if (block.getSize()<22 || block.getSize()>65536) return {};
    const auto b=bytes(block);
    const Bytes header {'M','T','h','d',0,0,0,6,0,0,0,1,3,192,'M','T','r','k'};
    if (!std::equal(header.begin(),header.end(),b.begin())) return {};
    const auto length=(std::uint32_t(b[18])<<24)|(std::uint32_t(b[19])<<16)
        |(std::uint32_t(b[20])<<8)|b[21];
    if (length!=b.size()-22) return {};
    std::size_t position=22;
    const auto vlq=[&](std::uint32_t& value)
    {
        value=0;
        for (int i=0;i<4;++i)
        {
            if (position==b.size()) return false;
            const auto part=b[position++];value=(value<<7)|(part&0x7f);
            if ((part&0x80)==0) return true;
        }
        return false;
    };
    RawReadback result;std::uint32_t tick=0;std::uint8_t running=0;
    while (position<b.size())
    {
        std::uint32_t delta=0;
        if (!vlq(delta) || delta>0x7fffffffU-tick || position==b.size()) return {};
        tick+=delta;
        auto status=b[position];
        if (status>=0x80) ++position;
        else {if (running==0) return {};status=running;}
        if (status==0xff)
        {
            running=0;
            if (position==b.size()) return {};
            const auto type=b[position++];std::uint32_t payload=0;
            if (!vlq(payload) || payload>b.size()-position) return {};
            position+=payload;
            if (type==0x2f)
            {
                if (payload!=0 || position!=b.size()) return {};
                result.valid=true;return result;
            }
            continue;
        }
        if ((status&0xf0)!=0x80 && (status&0xf0)!=0x90) return {};
        running=status;
        if (b.size()-position<2 || b[position]>=0x80 || b[position+1]>=0x80
            || result.events.size()>=512) return {};
        result.events.push_back({static_cast<int>(tick),status,b[position],b[position+1]});
        position+=2;
    }
    return {}; // A complete track must contain its final end-of-track meta event.
}
void serializedMidiOrder()
{
    // Catches a writer that emits note-ons before same-tick offs, even though JUCE hides it.
    // Literal valid SMF: running-status on60 precedes off60 at tick960; end at tick3840.
    const Bytes wire {'M','T','h','d',0,0,0,6,0,0,0,1,3,192,
        'M','T','r','k',0,0,0,30,
        0,0xff,0x58,4,4,2,24,8,
        0,0x90,60,100,
        0x87,0x40,60,90,
        0,0x80,60,0,
        0x81,0x70,0x80,60,0,
        0x94,0x50,0xff,0x2f,0};
    const juce::MemoryBlock counterexample(wire.data(),wire.size());
    const std::vector<Event> physical {{0,144,60,100},{960,144,60,90},{960,128,60,0},{1200,128,60,0}};
    const std::vector<Event> normalized {{0,144,60,100},{960,128,60,0},{960,144,60,90},{1200,128,60,0}};
    CHECK(readMidi(counterexample).events==normalized);
    const auto raw=readRawMidi(counterexample);
    CHECK(raw.valid); CHECK(raw.events==physical); CHECK(raw.events!=normalized);

    // Actual encoder: same-tick boundary, stored-order chord, and unequal identical-key overlap.
    PatternDocument d;d.noteCount=5;
    d.notes[0]={1,.25,67,91,2};d.notes[1]={0,1,60,100,1};
    d.notes[2]={1,1,60,80,1};d.notes[3]={1,.5,64,60,16};d.notes[4]={1.5,.25,60,70,1};
    const auto encoded=midi(d);const auto actual=readRawMidi(encoded);
    const std::vector<Event> expected {{0,144,60,100},{960,128,60,0},
        {960,145,67,91},{960,144,60,80},{960,159,64,60},
        {1200,129,67,0},{1440,143,64,0},{1440,144,60,70},{1680,128,60,0},{1920,128,60,0}};
    CHECK(actual.valid);CHECK(actual.events==expected);CHECK(readMidi(encoded).events==expected);
    // Same-channel chord forces running status on both on/off groups; input order is unsorted pitch.
    d={};d.noteCount=3;d.notes[0]={0,.25,72,99,3};d.notes[1]={0,.25,48,88,3};d.notes[2]={0,.25,60,77,3};
    const auto chord=readRawMidi(midi(d));CHECK(chord.valid);
    CHECK(chord.events==std::vector<Event>({{0,146,72,99},{0,146,48,88},{0,146,60,77},{240,130,72,0},{240,130,48,0},{240,130,60,0}}));
}
void rejected(const Bytes& b)
{
    auto output=fixture(); const auto before=output;
    CHECK(!decodePatternAsset(b.data(),b.size(),output)); CHECK(exact(output,before));
}

void assetBits()
{
    begin("asset_exact_bits_and_canonical_suffix");
    // Catches reordered fields, lossy numbers, omitted channels, and noncanonical unused notes.
    const auto d=fixture(); const auto source=d; juce::MemoryBlock encoded;
    CHECK(encodePatternAsset(d,encoded)); CHECK(exact(d,source));
    // Literal little endian header and both note payloads, independently stated.
    Bytes expected {'A','I','P','T',1,0,0,0,2,7,8,1, 0,0,0,0,0,0,224,63,2,0,0,0,
        0,0,0,0,0,0,0,128, 0,0,0,0,0,0,208,63,60,99,1,
        0,0,0,0,0,0,248,63, 0,0,0,0,0,0,224,63,127,1,16};
    const auto sha=juce::SHA256(expected.data(),expected.size()).getRawData();
    const auto hashBytes=bytes(sha); expected.insert(expected.end(),hashBytes.begin(),hashBytes.end());
    CHECK(bytes(encoded)==expected); CHECK(encoded.getSize()==94);
    PatternDocument output; output.notes[200]={1,1,1,1,1};
    CHECK(decodePatternAsset(expected.data(),expected.size(),output)); CHECK(exact(output,d));
    for(double swing : {-0.0, .123456789012345, 1.0})
    {
        auto special=d; special.swing=swing; special.notes[1].startBeats=std::nextafter(1.5,2.0);
        special.notes[0].durationBeats=std::numeric_limits<double>::denorm_min();
        CHECK(encodePatternAsset(special,encoded)); CHECK(decodePatternAsset(encoded.getData(),encoded.getSize(),output)); CHECK(exact(output,special));
    }
    for(int bars=1;bars<=8;++bars) for(int numerator=1;numerator<=32;++numerator)
        for(int denominator : {1,2,4,8,16,32}) for(int grid=0;grid<5;++grid)
        {
            PatternDocument a; a.bars=static_cast<std::uint8_t>(bars); a.meter={static_cast<std::uint8_t>(numerator),static_cast<std::uint8_t>(denominator)}; a.grid=static_cast<PatternGrid>(grid);
            CHECK(encodePatternAsset(a,encoded)); CHECK(encoded.getSize()==56);
            CHECK(decodePatternAsset(encoded.getData(),encoded.getSize(),output)); CHECK(exact(output,a));
        }
}
void assetRejection()
{
    begin("asset_strict_rejection_preserves_output");
    // Catches trusting digest alone, length/count overflow, and modifying last-good output on failure.
    juce::MemoryBlock encoded; CHECK(encodePatternAsset(fixture(),encoded));
    auto valid=bytes(encoded); if(valid.size()!=94) valid.resize(94);
    for(std::size_t n=0;n<valid.size();++n) rejected(Bytes(valid.begin(),valid.begin()+n));
    auto extra=valid; extra.push_back(0); rejected(extra); rejected(Bytes(4921,0));
    auto out=fixture(); auto before=out; CHECK(!decodePatternAsset(nullptr,94,out)); CHECK(exact(out,before));
    for(auto index : {0,4,5,6,7,22,23}) { auto bad=valid; bad[index]^=0x80; digest(bad); rejected(bad); }
    for(auto count : {0,1,3,256,257,65535}) {auto bad=valid; bad[20]=std::uint8_t(count);bad[21]=std::uint8_t(count>>8);digest(bad);rejected(bad);}
    for(std::size_t index=0;index<valid.size();++index) {auto bad=valid;bad[index]^=1;rejected(bad);}
    for(const auto [index,value] : {std::pair{8,0},{8,9},{9,0},{9,33},{10,3},{11,5},{40,128},{41,0},{41,128},{42,0},{42,17}})
    {auto bad=valid;bad[index]=std::uint8_t(value);digest(bad);rejected(bad);}
    for(double value : {-1.0,1.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {auto bad=valid;putDouble(bad,12,value);digest(bad);rejected(bad);}
    for(double value : {-1.0,7.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {auto bad=valid;putDouble(bad,24,value);digest(bad);rejected(bad);}
    for(double value : {-1.0,0.0,8.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {auto bad=valid;putDouble(bad,32,value);digest(bad);rejected(bad);}
    std::vector<PatternDocument> invalid;
    auto a=fixture();a.noteCount=257;invalid.push_back(a);a=fixture();a.bars=0;invalid.push_back(a);
    a=fixture();a.notes[200].pitch=1;invalid.push_back(a);a=fixture();a.swing=std::numeric_limits<double>::quiet_NaN();invalid.push_back(a);
    a=fixture();a.notes[0].durationBeats=0;invalid.push_back(a);
    for(const auto& d:invalid) {juce::MemoryBlock b("keep",4);const auto old=b;CHECK(!encodePatternAsset(d,b));CHECK(b==old);CHECK(!encodePatternMidi(d,b));CHECK(b==old);}
}
void emptyMidi()
{
    begin("midi_empty_meter_phrase_end_and_counts");
    // Catches guessed tempo/meter, missing empty phrase end, and wrong SMF type/division.
    for(int bars=1;bars<=8;++bars) for(int n=1;n<=32;++n) for(int den : {1,2,4,8,16,32}) for(int grid=0;grid<5;++grid)
    {
        PatternDocument d;d.bars=std::uint8_t(bars);d.meter={std::uint8_t(n),std::uint8_t(den)};d.grid=static_cast<PatternGrid>(grid);
        const auto r=readMidi(midi(d));CHECK(r.events.empty());CHECK(r.numerator==n);CHECK(r.denominator==den);CHECK(r.end==bars*n*3840/den);
    }
}
void timingMidi()
{
    begin("midi_shared_swing_rounding_order_and_overlap");
    serializedMidiOrder();
    // Catches independent/wrong swing, nearest-even ties, reordered chord starts or repaired overlaps.
    PatternDocument d;d.grid=PatternGrid::eighth;d.swing=1;d.noteCount=5;
    d.notes[0]={.5,.25,67,91,2};d.notes[1]={0,.75,60,100,1};d.notes[2]={.5,.5,60,80,1};d.notes[3]={1,.25,60,70,1};d.notes[4]={.5,.25,64,60,16};
    const std::vector<Event> expected {{0,0x90,60,100},{720,0x80,60,0},{720,0x91,67,91},{720,0x90,60,80},{720,0x9f,64,60},{960,0x81,67,0},{960,0x8f,64,0},{960,0x90,60,70},{1200,0x80,60,0},{1200,0x80,60,0}};
    auto before=d;const auto r=readMidi(midi(d));CHECK(r.events==expected);CHECK(exact(d,before));
    // Unequal overlapping same-key ends must remain four raw events, without pairing repair.
    d={};d.noteCount=2;d.notes[0]={0,2,60,77,1};d.notes[1]={1,.25,60,99,1};
    CHECK(readMidi(midi(d)).events==std::vector<Event>({{0,144,60,77},{960,144,60,99},{1200,128,60,0},{1920,128,60,0}}));
    d={};d.meter={3,8};d.grid=PatternGrid::quarter;d.swing=1;d.noteCount=3;
    d.notes[0]={.5,.125,61,90,1};d.notes[1]={1,.125,62,91,1};d.notes[2]={1.25,.25,63,92,1};
    CHECK(readMidi(midi(d)).events==std::vector<Event>({{600,144,61,90},{720,128,61,0},{1200,144,62,91},{1320,128,62,0},{1320,144,63,92},{1440,128,63,0}}));
    d={};d.meter={1,8};d.grid=PatternGrid::quarter;d.swing=1;d.noteCount=1;d.notes[0]={.25,.125,60,100,1};
    CHECK(readMidi(midi(d)).events==std::vector<Event>({{240,144,60,100},{360,128,60,0}}));
    for(int g=0;g<5;++g) for(double swing : {0.0,.5,1.0})
    {
        d={};d.grid=static_cast<PatternGrid>(g);d.swing=swing;d.noteCount=1;
        const double cell=1.0/std::pow(2.0,g);d.notes[0]={cell,cell*.25,60,100,1};
        const int on=int(cell*960*(1+swing*.5));const int off=on+int(cell*240);
        CHECK(readMidi(midi(d)).events==std::vector<Event>({{on,144,60,100},{off,128,60,0}}));
    }
    d={};d.noteCount=1;d.notes[0]={.5/960,1.0/960,60,100,1};
    CHECK(readMidi(midi(d)).events==std::vector<Event>({{1,144,60,100},{2,128,60,0}}));
    d.notes[0]={.123456,.345678,60,100,1};auto rounded=readMidi(midi(d));
    CHECK(rounded.events.size()==2);if(rounded.events.size()==2){CHECK(std::abs(rounded.events[0].tick-.123456*960)<=.5);CHECK(std::abs(rounded.events[1].tick-(.123456+.345678)*960)<=.5);}
}
void boundaries()
{
    begin("max256_and_subtick_boundaries");
    // Catches truncating capacity, dropped tiny holds and note-ons at/after phrase end.
    PatternDocument d;d.noteCount=256;
    for(int i=0;i<256;++i)d.notes[i]={0,.25,std::uint8_t(i%128),std::uint8_t(1+i%127),std::uint8_t(1+i%16)};
    juce::MemoryBlock b;CHECK(encodePatternAsset(d,b));CHECK(b.getSize()==4920);PatternDocument out;CHECK(decodePatternAsset(b.getData(),b.getSize(),out));CHECK(exact(out,d));
    auto r=readMidi(midi(d));CHECK(r.events.size()==512);
    if(r.events.size()==512)for(int i=0;i<256;++i){CHECK(r.events[i]==Event({0,0x90+i%16,i%128,1+i%127}));CHECK(r.events[256+i]==Event({240,0x80+i%16,i%128,0}));}
    d={};d.noteCount=3;d.notes[0]={0,std::numeric_limits<double>::denorm_min(),60,100,1};d.notes[1]={4-0.1/960,0.05/960,61,101,1};d.notes[2]={std::nextafter(4.0,0.0),std::numeric_limits<double>::denorm_min(),62,102,1};
    CHECK(readMidi(midi(d)).events==std::vector<Event>({{0,144,60,100},{1,128,60,0},{3839,144,61,101},{3839,144,62,102},{3840,128,61,0},{3840,128,62,0}}));
    d={};d.grid=PatternGrid::quarter;d.swing=1;d.noteCount=1;d.notes[0]={3,1,60,100,1};
    CHECK(readMidi(midi(d)).events==std::vector<Event>({{3360,144,60,100},{3840,128,60,0}}));
}
void files()
{
    begin("file_atomic_replacement_and_failed_destination");
    // Catches destructive prevalidation, failed-temp replacement and loading corrupt files into live state.
    const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/m3-pattern-scheduler-20261004/task-2-fix-1-files").getChildFile(juce::Uuid().toString());
    CHECK(root.createDirectory().wasOk());const auto asset=root.getChildFile("phrase.aipattern"), smf=root.getChildFile("phrase.mid");
    CHECK(asset.replaceWithText("old asset"));CHECK(smf.replaceWithText("old midi"));const auto d=fixture();
    auto invalid=d;invalid.bars=0;CHECK(!savePatternAsset(invalid,asset));CHECK(asset.loadFileAsString()=="old asset");CHECK(!exportPatternMidi(invalid,smf));CHECK(smf.loadFileAsString()=="old midi");
    CHECK(savePatternAsset(d,asset));CHECK(exportPatternMidi(d,smf));PatternDocument output;CHECK(loadPatternAsset(asset,output));CHECK(exact(output,d));
    juce::MemoryBlock disk, encoded;CHECK(asset.loadFileAsData(disk));CHECK(encodePatternAsset(d,encoded));CHECK(disk==encoded);
    disk.reset();CHECK(smf.loadFileAsData(disk));CHECK(encodePatternMidi(d,encoded));CHECK(disk==encoded);CHECK(readMidi(disk).events.size()==4);
    auto second=d;second.notes[0].pitch=17;CHECK(savePatternAsset(second,asset));CHECK(loadPatternAsset(asset,output));CHECK(exact(output,second));
    const auto old=output;const auto corrupt=root.getChildFile("corrupt.aipattern");CHECK(corrupt.replaceWithText("bad"));CHECK(!loadPatternAsset(corrupt,output));CHECK(exact(output,old));CHECK(!loadPatternAsset(root.getChildFile("absent.aipattern"),output));CHECK(exact(output,old));
    const auto huge=root.getChildFile("oversize.aipattern");juce::MemoryBlock oversized(4921,true);CHECK(huge.replaceWithData(oversized.getData(),oversized.getSize()));CHECK(!loadPatternAsset(huge,output));CHECK(exact(output,old));
    const auto directory=root.getChildFile("destination.aipattern");CHECK(directory.createDirectory().wasOk());const auto sentinel=directory.getChildFile("keep.txt");CHECK(sentinel.replaceWithText("preserve"));
    CHECK(!savePatternAsset(d,directory));CHECK(!exportPatternMidi(d,directory));CHECK(sentinel.loadFileAsString()=="preserve");CHECK(!loadPatternAsset(directory,output));CHECK(exact(output,old));
    const auto blocker=root.getChildFile("parent-is-file");CHECK(blocker.replaceWithText("keep"));CHECK(!savePatternAsset(d,blocker.getChildFile("x.aipattern")));CHECK(!exportPatternMidi(d,blocker.getChildFile("x.mid")));CHECK(blocker.loadFileAsString()=="keep");
#if JUCE_WINDOWS
    // Deny deletion and writes through an actual OS handle: atomic rename must fail without truncation.
    disk.reset();CHECK(asset.loadFileAsData(disk));const auto assetBefore=disk;
    auto handle=CreateFileW(asset.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(handle!=INVALID_HANDLE_VALUE);
    if(handle!=INVALID_HANDLE_VALUE){CHECK(!savePatternAsset(d,asset));CHECK(!exportPatternMidi(d,asset));CloseHandle(handle);}
    disk.reset();CHECK(asset.loadFileAsData(disk));CHECK(disk==assetBefore);
#endif
    std::cout << "file fixtures: " << root.getFullPathName() << '\n';
}
}
int main()
{
    assetBits();assetRejection();emptyMidi();timingMidi();boundaries();files();
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures==0?0:1;
}
