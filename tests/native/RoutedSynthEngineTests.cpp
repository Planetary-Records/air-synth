#include "RoutedSynthEngine.h"
#include "DspPipeline.h"
#include "space/SpaceActivation.h"
#if !defined(_DEBUG)
#include "../../src/dsp/RoutedSynthEngine.cpp"
#endif
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <limits>
#include <numbers>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace airack;
int checks = 0, failures = 0;
void check(bool good, const char* what, int line)
{ ++checks; if (!good) { ++failures; std::cout << "FAIL " << line << " " << what << '\n'; } }
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
void section(const char* name)
{
    if(std::getenv("AIRACK_ROUTED_SECTION_TIMING")){
        static const auto start=std::chrono::steady_clock::now();
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"SECTION_TIME ns="<<elapsed<<" name="<<name<<'\n'<<std::flush;
    }else std::cout<<"SECTION "<<name<<'\n';
}
using Event = std::pair<int, juce::MidiMessage>;
using Audio = std::array<std::vector<float>, 2>;
juce::MidiMessage on(int note = 69, int channel = 1, float velocity = 1)
{ return juce::MidiMessage::noteOn(channel, note, velocity); }
juce::MidiMessage off(int note = 69, int channel = 1) { return juce::MidiMessage::noteOff(channel, note); }
RoutedPatch flat()
{
    RoutedPatch p;
    auto& l = p.synth.layers[0];
    l.oscillators[0].waveform = Waveform::sine; l.oscillators[0].level = 1;
    l.oscillators[1].level = 0; l.level = 1; l.cutoffHz = 20000; l.resonance = 0;
    l.ampEnvelope = l.filterEnvelope = {0, 0, 0, 1}; p.synth.masterLevel = 1;
    return p;
}
RoutedPatch impulsePatch()
{ auto p = flat(); p.synth.layers[0].oscillators[0].level = 0; p.synth.layers[0].noiseLevel = 1; return p; }
const std::vector<Event> impulseEvents {{0, on()}, {1, off()}};
void add(RoutedPatch& p, EffectKind kind, int chain = masterChain, int slot = noSlot)
{
    RackEdit edit; edit.kind = RackEditKind::addEffect; edit.effect = kind;
    edit.slot = static_cast<std::uint8_t>(slot); edit.destination = static_cast<std::uint8_t>(chain);
    edit.position = p.layout.effectCount[static_cast<size_t>(chain)];
    CHECK(editRackLayout(p.layout, edit, p.layout) == RackEditResult::applied);
}
void addLayer(RoutedPatch& p)
{
    RackEdit edit; edit.kind = RackEditKind::addSynth; edit.position = p.layout.synthCount;
    CHECK(editRackLayout(p.layout, edit, p.layout) == RackEditResult::applied);
    const auto slot = p.layout.synthOrder[edit.position]; p.synth.layers[slot] = p.synth.layers[0];
}
Audio render(RoutedSynthEngine& e, int count, const std::vector<Event>& events = {},
             int partition = 512, std::optional<double> tempo = std::nullopt)
{
    Audio result; for (auto& channel : result) channel.resize(static_cast<size_t>(count));
    for (int start = 0; start < count; start += partition)
    {
        const auto n = std::min(partition, count - start);
        juce::AudioBuffer<float> out(2, n); juce::MidiBuffer midi;
        for (const auto& [offset, message] : events)
            if (offset >= start && offset < start + n) midi.addEvent(message, offset - start);
        e.render(out, midi, tempo);
        for (int c = 0; c < 2; ++c) std::copy_n(out.getReadPointer(c), n, result[static_cast<size_t>(c)].data() + start);
    }
    return result;
}
Audio sound(const RoutedPatch& p, int count, const std::vector<Event>& events = {{0, on()}},
            double rate = 48000, int partition = 512, std::optional<double> tempo = std::nullopt)
{ RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(rate, 512)); return render(e, count, events, partition, tempo); }
double energy(const Audio& a, int from = 0, int to = -1)
{
    if (to < 0) to = static_cast<int>(a[0].size());
    double sum = 0; for (auto& c : a) for (int i = from; i < to; ++i) sum += double(c[static_cast<size_t>(i)]) * c[static_cast<size_t>(i)];
    return sum / std::max(1, 2 * (to - from));
}
double difference(const Audio& a, const Audio& b)
{
    double d = 0;
    for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < a[c].size(); ++i) d = std::max(d, std::abs(double(a[c][i]) - b[c][i]));
    return d;
}
bool finite(const Audio& a) { for (auto& c : a) for (auto x : c) if (!std::isfinite(x)) return false; return true; }
int arrival(const Audio& a)
{ for (size_t i = 0; i < a[0].size(); ++i) if (std::max(std::abs(a[0][i]), std::abs(a[1][i])) > 1e-8f) return static_cast<int>(i); return -1; }
double peak(const Audio& a) { double p = 0; for (auto& c : a) for (auto x : c) p = std::max(p, std::abs(double(x))); return p; }

void outputLegacySamples()
{
    section("outputLegacySamples");
    auto p=flat(); addLayer(p); p.synth.layers[0].pan=-.4f; p.synth.layers[1].pan=.7f;
    add(p,EffectKind::drive,0); add(p,EffectKind::delay); add(p,EffectKind::reverb); add(p,EffectKind::compressor);
    // Captured from the pre-stage Windows Debug routed engine, including both
    // channels and all four effect kinds. Protect the exact old float boundary.
    const std::array<std::uint64_t,3> expected {11875746573281970778ull,6121427366471511569ull,9759203172101311434ull};
    size_t index=0;
    for(double rate : {44100.0,48000.0,96000.0})
    {
        const auto a=sound(p,4096,{{0,on(59)},{83,on(72)},{903,off(59)}},rate,64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_OUTPUT rate="<<rate<<" hash="<<hash<<'\n';
        CHECK(hash==expected[index++]);
    }
}

void outputValidationAndGain()
{
    section("outputValidationAndGain");
    auto p=flat(); p.outputGainDb=30; p.outputProtection=true;
    RoutedSynthEngine e,reference; CHECK(e.setPatch(p));CHECK(reference.setPatch(p));
    CHECK(e.prepare(48000,512));CHECK(reference.prepare(48000,512));
    render(e,321,{{0,on()}});render(reference,321,{{0,on()}});
    for(float invalid : {-24.01f,36.01f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
    {auto q=p;q.outputGainDb=invalid;CHECK(!validateRoutedPatch(q));CHECK(!e.setPatch(q));CHECK(e.patch()==p);}
    CHECK(difference(render(e,1000),render(reference,1000))==0);CHECK(e.activeNoteCount()==1);
    // Quiet actual routed output exposes gain after inserts, master effects,
    // pan and master level without triggering the guard.
    p=flat();p.synth.layers[0].level=.001f;p.synth.layers[0].pan=.4f;p.synth.masterLevel=.7f;
    add(p,EffectKind::drive,0);add(p,EffectKind::delay);p.effects[1].delay.seconds=.003f;
    for(double rate : {44100.0,48000.0,96000.0})
    {
        const auto dry=sound(p,4096,{{0,on()}},rate);
        for(float db : {-24.f,0.f,30.f,36.f})
        {
            auto q=p;q.outputGainDb=db;const auto gained=sound(q,4096,{{0,on()}},rate);
            const double gain=std::pow(10.,double(db)/20);double error=0;
            for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<dry[ch].size();++i)
                error=std::max(error,std::abs(double(gained[ch][i])-double(dry[ch][i])*gain));
            CHECK(error<1e-8);CHECK(energy(gained)>0);CHECK(peak(gained)<.891);
            q.outputProtection=true;CHECK(difference(gained,sound(q,4096,{{0,on()}},rate))==0);
        }
    }
}

// Compare protection with the actual unprotected routed samples. This is an
// observable sample-peak contract, not a second oscillator/effect renderer.
double protectionError(const Audio& raw,const Audio& guarded,double rate,double initialEnvelope=0)
{
    double envelope=initialEnvelope,error=0;const double ceiling=std::pow(10.,-1./20),release=std::exp(-1./(.050*rate));
    for(size_t i=0;i<raw[0].size();++i)
    {
        envelope=std::max({std::abs(double(raw[0][i])),std::abs(double(raw[1][i])),envelope*release});
        const double gain=envelope>ceiling?ceiling/envelope:1;
        for(size_t ch=0;ch<2;++ch)error=std::max(error,std::abs(double(guarded[ch][i])-raw[ch][i]*gain));
    }
    return error;
}

void outputProtectionAndRelease()
{
    section("outputProtectionAndRelease");
    auto p=flat();p.outputGainDb=36;p.synth.layers[0].pan=.35f;
    for(int i=0;i<3;++i)addLayer(p);
    std::vector<Event> notes;for(int n=48;n<64;++n)notes.emplace_back(0,on(n));
    for(double rate : {44100.0,48000.0,96000.0})
    {
        const auto raw=sound(p,8192,notes,rate);auto q=p;q.outputProtection=true;
        const auto guarded=sound(q,8192,notes,rate,64);
        CHECK(peak(raw)>1);CHECK(peak(guarded)<=double(float(std::pow(10.,-1./20))));
        CHECK(protectionError(raw,guarded,rate)<2e-7);CHECK(finite(guarded));
        bool negative=false;double ratioError=0;
        for(size_t i=0;i<raw[0].size();++i)
        {negative|=guarded[0][i]<-.1f;const double scale=std::max({1e-20,std::abs(double(raw[0][i])),std::abs(double(raw[1][i]))});
            ratioError=std::max(ratioError,std::abs(double(guarded[0][i])*raw[1][i]-double(guarded[1][i])*raw[0][i])/scale);}
        CHECK(negative);CHECK(ratioError<2e-7);CHECK(energy(sound(q,512,{},rate))==0);
        // Bypass history is observed. Enabling during a held overload takes
        // effect on this sample, preserves notes and is bounded immediately.
        RoutedSynthEngine enabled,rawEngine;CHECK(enabled.setPatch(p));CHECK(rawEngine.setPatch(p));
        CHECK(enabled.prepare(rate,512));CHECK(rawEngine.prepare(rate,512));
        const auto before=render(enabled,777,notes),rawBefore=render(rawEngine,777,notes);CHECK(difference(before,rawBefore)==0);
        double history=0;for(size_t i=0;i<before[0].size();++i)history=std::max({std::abs(double(before[0][i])),std::abs(double(before[1][i])),history*std::exp(-1./(.050*rate))});
        CHECK(enabled.setPatch(q));const auto after=render(enabled,1024),rawAfter=render(rawEngine,1024);
        CHECK(enabled.activeNoteCount()==16);CHECK(peak(after)<=double(float(std::pow(10.,-1./20))));CHECK(protectionError(rawAfter,after,rate,history)<2e-7);
        // One loud noise impulse followed by a very quiet one measures the
        // 50 ms release directly at 50/100 ms, including bypass history.
        auto impulse=impulsePatch();impulse.outputGainDb=36;impulse.synth.layers[0].pan=-.4f;
        add(impulse,EffectKind::compressor);impulse.effects[0].compressor.ratio=1;impulse.effects[0].compressor.makeupDb=24;
        for(int i=0;i<3;++i)addLayer(impulse);
        for(double seconds : {.05,.10})
        {
            const int offset=static_cast<int>(rate*seconds);
            const std::vector<Event> events {{0,on()},{1,off()},{offset,on(69,1,1.f/127)},{offset+1,off()}};
            const auto r=sound(impulse,offset+2,events,rate);CHECK(peak(r)>3);
            auto protect=impulse;protect.outputProtection=true;const auto g=sound(protect,offset+2,events,rate);
            CHECK(protectionError(r,g,rate)<2e-7);
            const double initialPeak=std::max(std::abs(double(r[0][0])),std::abs(double(r[1][0])));
            const double expected=std::min(1.,std::pow(10.,-1./20)/(initialPeak*std::exp(-double(offset)/(.05*rate))));
            CHECK(std::abs(double(g[0][size_t(offset)])/r[0][size_t(offset)]-expected)<1e-7);CHECK(expected<1);
            CHECK(energy(g,offset)>0);CHECK(energy(g,offset)<energy(r,offset));
            RoutedSynthEngine bypass;CHECK(bypass.setPatch(impulse));CHECK(bypass.prepare(rate,512));
            render(bypass,offset,impulseEvents);CHECK(bypass.setPatch(protect));
            const auto admitted=render(bypass,2,{{0,on(69,1,1.f/127)},{1,off()}});
            CHECK(admitted[0][0]==g[0][size_t(offset)]);CHECK(admitted[1][0]==g[1][size_t(offset)]);
        }
    }
}

void higherOutputProtection()
{
    section("quality_final_host_protection: ceiling, stereo link and 50ms release after FIR");
    auto p=flat();p.outputGainDb=36;p.synth.layers[0].pan=.35f;
    for(int i=0;i<3;++i)addLayer(p);
    std::vector<Event> notes;for(int n=48;n<64;++n)notes.emplace_back(0,on(n));
    for(double rate:{44100.,48000.,96000.})
    {
        RoutedSynthEngine raw,guarded;auto q=p;q.outputProtection=true;
        CHECK(raw.setPatch(p)&&guarded.setPatch(q));
        CHECK(raw.prepare(rate,512,RenderQuality::higher)&&guarded.prepare(rate,512,RenderQuality::higher));
        const auto a=render(raw,8192,notes),b=render(guarded,8192,notes);
        CHECK(peak(a)>1 && finite(b));CHECK(peak(b)<=double(float(std::pow(10.,-1./20))));
        CHECK(protectionError(a,b,rate)<2e-7);
        raw.reset();guarded.setPatch(p);guarded.reset();
        const auto before=render(raw,777,notes);CHECK(difference(before,render(guarded,777,notes))==0);
        double history=0;for(size_t i=0;i<before[0].size();++i)history=std::max({std::abs(double(before[0][i])),std::abs(double(before[1][i])),history*std::exp(-1./(.05*rate))});
        guarded.setPatch(q);const auto after=render(guarded,4096),rawAfter=render(raw,4096);
        CHECK(guarded.activeNoteCount()==16);CHECK(peak(after)<=double(float(std::pow(10.,-1./20))));
        CHECK(protectionError(rawAfter,after,rate,history)<2e-7);
    }
}

void higherStopProtectionHistory()
{
    section("quality_higher_stop_clears_overload_after_fir_history");
    for(double rate:{44100.,48000.,96000.}){
        auto p=flat();p.outputGainDb=36;p.outputProtection=true;for(int n=0;n<3;++n)addLayer(p);
        Audio coarse;
        for(int partition:{512,64}){RoutedSynthEngine e,fresh;CHECK(e.setPatch(p)&&fresh.setPatch(p));CHECK(e.prepare(rate,512,RenderQuality::higher)&&fresh.prepare(rate,512,RenderQuality::higher));
            CHECK(peak(render(e,2048,{{0,on()}}))>.8);
            const auto restarted=render(e,625,{{113,juce::MidiMessage::midiStop()},{113,on(69,1,1.f/127)}},partition);
            const auto quiet=render(fresh,512,{{0,on(69,1,1.f/127)}},partition);const int flush=2*e.latencySamples();double error=0;
            for(size_t ch=0;ch<2;++ch)for(int n=flush;n<512;++n)error=std::max(error,std::abs(double(restarted[ch][size_t(n+113)])-quiet[ch][size_t(n)]));
            CHECK(energy(quiet)>1e-12);CHECK(error<=2e-7);if(partition==512)coarse=restarted;else CHECK(difference(coarse,restarted)==0);
            std::cout<<"QUALITY_STOP rate="<<rate<<" partition="<<partition<<" retained_fir_history_frames="<<flush<<" quiet_restart_max_error="<<error<<'\n';}
    }
}
void outputEditsResetAndInvalidBuffers()
{
    section("outputEditsResetAndInvalidBuffers");
    for(double rate : {44100.0,48000.0,96000.0})
    {
        auto p=flat();p.synth.layers[0].level=.01f;add(p,EffectKind::delay);p.effects[0].delay.seconds=.003f;
        RoutedSynthEngine e,reference;CHECK(e.setPatch(p));CHECK(reference.setPatch(p));CHECK(e.prepare(rate,512));CHECK(reference.prepare(rate,512));
        render(e,997,{{0,on()}});render(reference,997,{{0,on()}});
        p.outputGainDb=36;CHECK(e.setPatch(p));
        const int ramp=static_cast<int>(std::ceil(rate*.005));Audio edited;
        for(int i=0;i<ramp+31;++i)
        {CHECK(e.setPatch(p));const auto a=render(e,1);for(size_t ch=0;ch<2;++ch)edited[ch].push_back(a[ch][0]);}
        const auto dry=render(reference,ramp+31);double error=0;
        for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<dry[ch].size();++i)
        {const double gain=1+(std::pow(10.,36./20)-1)*std::min(1.,double(i+1)/ramp);error=std::max(error,std::abs(edited[ch][i]-dry[ch][i]*gain));}
        CHECK(error<1e-8);CHECK(e.activeNoteCount()==1);
        // A fresh protected recall/reprepare snaps gain and clears loud history.
        p=flat();p.outputGainDb=36;p.outputProtection=true;for(int i=0;i<3;++i)addLayer(p);
        CHECK(e.setPatch(p));e.reset();const auto first=render(e,2048,{{0,on()}});
        CHECK(peak(first)>.8);e.reset();CHECK(difference(first,render(e,2048,{{0,on()}},64))==0);
        CHECK(e.prepare(rate,512));CHECK(difference(first,render(e,2048,{{0,on()}}))==0);
        CHECK(reference.setPatch(p));reference.reset();render(reference,2048,{{0,on()}});
        // Invalid/empty/aliased buffers and MIDI STOP cannot advance either
        // a running gain ramp or the nonzero release history.
        p.outputGainDb=-24;CHECK(e.setPatch(p));CHECK(reference.setPatch(p));
        juce::MidiBuffer stop;stop.addEvent(juce::MidiMessage::midiStop(),0);
        juce::AudioBuffer<float> bad(1,64),empty(2,0),large(2,513);
        std::array<float,100> storage{};std::array<float*,2> channels{storage.data(),storage.data()+3};juce::AudioBuffer<float> alias(channels.data(),2,64);
        e.render(bad,stop);e.render(empty,stop);e.render(large,stop);e.render(alias,stop);
        CHECK(difference(render(e,512),render(reference,512))==0);CHECK(e.activeNoteCount()==1);
        // STOP clears envelope at its exact offset; a quiet restart must equal
        // a fresh prepared instance, not inherit the old overload attenuation.
        p.outputGainDb=36;CHECK(e.setPatch(p));e.reset();render(e,1024,{{0,on()}});
        const auto restarted=render(e,401,{{113,juce::MidiMessage::midiStop()},{113,on(69,1,1.f/127)}},64);
        const auto fresh=sound(p,288,{{0,on(69,1,1.f/127)}},rate);
        CHECK(difference(Audio{std::vector<float>(restarted[0].begin()+113,restarted[0].end()),std::vector<float>(restarted[1].begin()+113,restarted[1].end())},fresh)==0);
        // Guard/gain edits preserve a ringing effect tail and are independent
        // of host partitions, including gain reversal before a ramp ends.
        const auto sequence=[&](int partition)
        {
            auto q=impulsePatch();q.outputGainDb=36;add(q,EffectKind::delay);q.effects[0].delay.seconds=.003f;q.effects[0].delay.feedback=.8f;
            RoutedSynthEngine engine;CHECK(engine.setPatch(q));CHECK(engine.prepare(rate,512));Audio result;
            const auto append=[&](Audio a){for(size_t ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),a[ch].begin(),a[ch].end());};
            append(render(engine,997,impulseEvents,partition));q.outputProtection=true;CHECK(engine.setPatch(q));append(render(engine,173,{},partition));
            q.outputGainDb=-24;CHECK(engine.setPatch(q));append(render(engine,71,{},partition));q.outputGainDb=30;CHECK(engine.setPatch(q));append(render(engine,2000,{},partition));
            CHECK(engine.activeNoteCount()==0);CHECK(energy(result,1241)>0);return result;
        };
        CHECK(difference(sequence(64),sequence(512))==0);
    }
}

void validationIsTransactional()
{
    section("validationIsTransactional");
    RoutedSynthEngine e, reference; const auto p = flat();
    CHECK(e.setPatch(p)); CHECK(reference.setPatch(p)); CHECK(e.prepare(48000, 512)); CHECK(reference.prepare(48000, 512));
    CHECK(energy(render(e, 300, {{0, on()}})) > 1e-7); render(reference, 300, {{0, on()}});
    const auto reject = [&](const RoutedPatch& q) { CHECK(!e.setPatch(q)); CHECK(e.patch() == p); };
    // Every field, including every inactive slot and bank, rejects NaN and both bounds.
    const auto fields = [&](auto bank, const auto& ranges) {
        for (size_t slot = 0; slot < 8; ++slot) for (const auto& range : ranges)
            for (float value : {range.second.first - 0.01f, range.second.second + 0.01f, std::numeric_limits<float>::quiet_NaN()})
            { auto q = p; ((q.effects[slot].*bank).*range.first) = value; reject(q); }
    };
    fields(&EffectSettings::drive, std::vector<std::pair<float DriveSettings::*, std::pair<float,float>>> {
        {&DriveSettings::gainDb,{0.f,24.f}}, {&DriveSettings::toneHz,{200.f,20000.f}}, {&DriveSettings::mix,{0.f,1.f}}});
    fields(&EffectSettings::delay, std::vector<std::pair<float DelaySettings::*, std::pair<float,float>>> {
        {&DelaySettings::seconds,{.001f,4.f}}, {&DelaySettings::beats,{.0625f,1.f}}, {&DelaySettings::feedback,{0.f,.95f}},
        {&DelaySettings::stereo,{0.f,1.f}}, {&DelaySettings::filterHz,{20.f,20000.f}}, {&DelaySettings::mix,{0.f,1.f}}});
    fields(&EffectSettings::reverb, std::vector<std::pair<float ReverbSettings::*, std::pair<float,float>>> {
        {&ReverbSettings::size,{0.f,1.f}}, {&ReverbSettings::decaySeconds,{.1f,20.f}}, {&ReverbSettings::damping,{0.f,1.f}},
        {&ReverbSettings::predelayMs,{0.f,250.f}}, {&ReverbSettings::mix,{0.f,1.f}}});
    fields(&EffectSettings::compressor, std::vector<std::pair<float CompressorSettings::*, std::pair<float,float>>> {
        {&CompressorSettings::thresholdDb,{-60.f,0.f}}, {&CompressorSettings::ratio,{1.f,20.f}}, {&CompressorSettings::attackMs,{.1f,100.f}},
        {&CompressorSettings::releaseMs,{10.f,2000.f}}, {&CompressorSettings::makeupDb,{0.f,24.f}}, {&CompressorSettings::mix,{0.f,1.f}}});
    for (size_t slot = 0; slot < 8; ++slot) { auto q = p; q.effects[slot].drive.shape = static_cast<DriveShape>(99); reject(q); }
    auto q = p; q.synth.layers[3].cutoffHz = std::numeric_limits<float>::quiet_NaN(); reject(q);
    q = p; q.layout.effects[0] = {SlotUse::present, EffectKind::delay, 3}; reject(q);
    q = p; q.layout.synthOrder[1] = 0; reject(q);
    for (auto rate : {7999.0, 384001.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) CHECK(!e.prepare(rate,512));
    CHECK(!e.prepare(48000,0)); CHECK(!e.prepare(48000,65537));
    CHECK(difference(render(e,512), render(reference,512)) == 0);
    CHECK(e.activeNoteCount() == 1);
}

void parallelInsertsAndMasterOrder()
{
    section("parallelInsertsAndMasterOrder");
    auto p = flat();
    const auto dry = sound(p,2048);
    SynthEngine core; CHECK(core.setPatch(p.synth)); CHECK(core.prepare(48000,2048));
    juce::AudioBuffer<float> buffer(2,2048); juce::MidiBuffer midi; midi.addEvent(on(),0); core.render(buffer,midi);
    double error = 0; for (int c=0;c<2;++c) for (int i=0;i<2048;++i) error=std::max(error,std::abs(double(buffer.getSample(c,i))-dry[static_cast<size_t>(c)][static_cast<size_t>(i)]));
    CHECK(error < 1e-7); CHECK(energy(dry)>1e-7);
    auto empty = p; empty.layout.synthUse[0] = SlotUse::retired; empty.layout.synthCount=0; empty.layout.synthOrder[0]=noSlot;
    CHECK(energy(sound(empty,2048)) == 0);
    addLayer(p); p.synth.layers[0].enabled=false;
    auto isolated = sound(p,2048); add(p,EffectKind::drive,0); p.effects[0].drive.gainDb=24;
    CHECK(difference(isolated,sound(p,2048))==0);
    p = flat(); add(p,EffectKind::drive); add(p,EffectKind::compressor);
    p.effects[0].drive.gainDb=24; p.effects[0].drive.mix=1;
    p.effects[1].compressor.thresholdDb=-50; p.effects[1].compressor.ratio=20;
    const auto dc = sound(p,8192); std::swap(p.layout.effectOrder[4][0],p.layout.effectOrder[4][1]);
    CHECK(difference(dc,sound(p,8192))>1e-3);
    p=flat(); for(int i=0;i<3;++i) addLayer(p);
    RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(48000,512));
    std::vector<Event> notes; for(int i=0;i<17;++i) notes.emplace_back(i,on(40+i));
    CHECK(energy(render(e,512,notes))>1e-6); CHECK(e.activeNoteCount()==16);
}

double binEnergy(const std::vector<float>& x, double cycles)
{
    double re=0,im=0; for(size_t i=0;i<x.size();++i) {const double phase=2*std::numbers::pi*cycles*double(i)/double(x.size()); re+=x[i]*std::cos(phase); im+=x[i]*std::sin(phase);}
    return 2*(re*re+im*im)/double(x.size()*x.size());
}
double foldedEnergy(const std::vector<float>& x)
{
    // 7031.25Hz, bin600/4096. Odd harmonics 5,7,9,11 fold to known bins.
    double sum=0; for(int harmonic : {5,7,9,11}) {int bin=(600*harmonic)%4096; bin=std::min(bin,4096-bin); sum+=binEnergy(x,bin);} return sum;
}
void driveToneCoefficientParity()
{
#if !defined(_DEBUG)
    section("driveToneCoefficientParity");
    juce::ScopedNoDenormals noDenormals;
    const auto same=[](double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);};
    for(double rate:{8000.,48000.,96000.,192000.})for(auto shape:{DriveShape::tanh,DriveShape::softSign}){
        auto cached=std::make_unique<airack::Effect>(),forced=std::make_unique<airack::Effect>();
        const bool ready=cached->prepare(rate)&&forced->prepare(rate);CHECK(ready);if(!ready)return;
        const int ramp=int(std::ceil(.005*rate)),frames=3*ramp+512;
        EffectSettings setting;setting.drive={24,shape,200,1};
        cached->settings(setting,ramp,true);forced->settings(setting,ramp,true);cached->clear();forced->clear();
        bool coefficientBits=true,outputBits=true,historyBits=true;int firstMismatch=-1;
        for(int n=0;n<frames;++n){
            bool publish=false,snap=false;
            if(n==32){setting.drive.toneHz=20000;publish=true;}
            if(n==32+ramp/2){setting.drive.toneHz=400;publish=true;}
            if(n==64+3*ramp/2){setting.drive.toneHz=20000;publish=true;}
            if(n==96+5*ramp/2){publish=true;} // Identical settled target.
            if(n==128+5*ramp/2){cached->clear();forced->clear();}
            if(n==160+5*ramp/2){setting.drive.toneHz=200;publish=true;snap=true;}
            if(publish){cached->settings(setting,ramp,snap);forced->settings(setting,ramp,snap);}
            auto nextTone=forced->controls[airack::driveTone];
            const double originalCoefficient=airack::lowpassCoefficient(nextTone.next(),forced->rate);
            // Force the predecessor's exact calculation on every reference frame.
            forced->driveToneKey=std::numeric_limits<double>::quiet_NaN();
            const airack::Stereo input=n%97==0?airack::Stereo{0.,-0.}:n%101==0?airack::Stereo{std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min()}:airack::Stereo{.11*std::sin(.13*n),-.07*std::cos(.17*n)};
            const auto a=cached->process(input,EffectKind::drive,1,shape,true,FilterFxMode::lowpass);
            const auto b=forced->process(input,EffectKind::drive,1,shape,true,FilterFxMode::lowpass);
            coefficientBits&=same(cached->driveToneCoefficient,originalCoefficient)&&same(forced->driveToneCoefficient,originalCoefficient);
            for(int ch=0;ch<2;++ch){outputBits&=same(a[ch],b[ch]);historyBits&=same(cached->previousInput[ch],forced->previousInput[ch])&&same(cached->previousDry[ch],forced->previousDry[ch])&&same(cached->driveLowpass[ch],forced->driveLowpass[ch]);}
            if(!(coefficientBits&&outputBits&&historyBits)&&firstMismatch<0)firstMismatch=n;
        }
        CHECK(coefficientBits&&outputBits&&historyBits);
        const double nextRate=rate==8000?96000:8000;
        const bool reprepared=cached->prepare(nextRate)&&forced->prepare(nextRate);CHECK(reprepared);if(!reprepared)return;
        cached->settings(setting,ramp,true);forced->settings(setting,ramp,true);cached->clear();forced->clear();
        forced->driveToneKey=std::numeric_limits<double>::quiet_NaN();
        const auto a=cached->process({.13,-.09},EffectKind::drive,1,shape,true,FilterFxMode::lowpass);
        const auto b=forced->process({.13,-.09},EffectKind::drive,1,shape,true,FilterFxMode::lowpass);
        bool rateBits=same(cached->driveToneCoefficient,airack::lowpassCoefficient(setting.drive.toneHz,nextRate));
        for(int ch=0;ch<2;++ch)rateBits&=same(a[ch],b[ch])&&same(cached->previousInput[ch],forced->previousInput[ch])&&same(cached->previousDry[ch],forced->previousDry[ch])&&same(cached->driveLowpass[ch],forced->driveLowpass[ch]);
        CHECK(rateBits);
        std::cout<<"DRIVE_TONE_PARITY rate="<<rate<<" shape="<<int(shape)<<" frames="<<frames<<" coefficient_bits="<<coefficientBits<<" output_bits="<<outputBits<<" history_bits="<<historyBits<<" reprepare_bits="<<rateBits<<" first_mismatch="<<firstMismatch<<" forced_original_per_frame=1 interrupted_ramp=1 clear=1 snap=1"<<'\n';
    }
#endif
    auto patch=flat();add(patch,EffectKind::drive);patch.effects[0].drive={24,DriveShape::softSign,12000,.75f};
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine reused,fresh;CHECK(reused.setPatch(patch)&&fresh.setPatch(patch));
        const bool started=reused.prepare(8000,512,quality);CHECK(started);if(!started)return;
        render(reused,97,{{0,on(57)}},73);
        const bool prepared=reused.prepare(96000,512,quality)&&fresh.prepare(96000,512,quality);CHECK(prepared);if(!prepared)return;
        const auto a=render(reused,4096,{{0,on(57)}},73),b=render(fresh,4096,{{0,on(57)}},73);
        bool bits=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<a[ch].size();++n)bits&=std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(b[ch][n]);
        CHECK(finite(a)&&energy(a)>0&&bits&&reused.patch()==patch&&reused.effectiveQuality()==quality);
        auto edited=patch;edited.effects[0].drive.toneHz=200;CHECK(reused.setPatch(edited)&&fresh.setPatch(edited));
        const auto c=render(reused,4096,{},73),d=render(fresh,4096,{},73);
        bool editBits=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<c[ch].size();++n)editBits&=std::bit_cast<std::uint32_t>(c[ch][n])==std::bit_cast<std::uint32_t>(d[ch][n]);
        CHECK(finite(c)&&editBits);reused.reset();fresh.reset();
        const auto e=render(reused,4096,{{0,on(57)}},73),f=render(fresh,4096,{{0,on(57)}},73);
        bool resetBits=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<e[ch].size();++n)resetBits&=std::bit_cast<std::uint32_t>(e[ch][n])==std::bit_cast<std::uint32_t>(f[ch][n]);
        CHECK(finite(e)&&energy(e)>0&&resetBits);
        std::cout<<"DRIVE_TONE_PUBLIC from=8000 to=96000 quality="<<int(quality)<<" frames=4096 bits="<<bits<<" edit_bits="<<editBits<<" reset_bits="<<resetBits<<'\n';
    }
}

void driveControlsAndAlias()
{
    section("driveControlsAndAlias");
    driveToneCoefficientParity();
    auto p=flat(); add(p,EffectKind::drive); p.effects[0].drive.mix=1; p.effects[0].drive.gainDb=24;
    const auto driven=sound(p,8192); CHECK(energy(driven)>1e-5);
    auto q=p; q.effects[0].drive.gainDb=0; CHECK(energy(driven)>energy(sound(q,8192))*10);
    q=p; q.effects[0].drive.toneHz=200; CHECK(energy(sound(q,8192))<energy(driven)*.6);
    q=p; q.effects[0].drive.mix=0; CHECK(difference(sound(q,8192),driven)>1e-3);
    q=p; q.effects[0].drive.shape=DriveShape::softSign; CHECK(difference(sound(q,8192),driven)>1e-3);
    for (auto shape : {DriveShape::tanh,DriveShape::softSign})
    {
        auto clean=flat(); clean.synth.layers[0].oscillators[0].tune=static_cast<float>(12*std::log2(7031.25/7040.0));
        std::vector<Event> events; for(int c=1;c<=16;++c) events.emplace_back(0,on(117,c));
        const auto source=sound(clean,8192,events);
        auto wet=clean; add(wet,EffectKind::drive); wet.effects[0].drive={24,shape,20000,1};
        const auto processed=sound(wet,8192,events);
        std::vector<float> naive(4096),aa(4096); double tone=0;
        const double coefficient=1-std::exp(-2*std::numbers::pi*20000/48000);
        for(size_t i=0;i<8192;++i)
        {
            const double x=source[0][i]*std::pow(10.0,24.0/20);
            const double direct=shape==DriveShape::tanh?std::tanh(x):x/(1+std::abs(x));
            tone+=coefficient*(direct-tone);
            if(i>=4096) {naive[i-4096]=static_cast<float>(tone); aa[i-4096]=processed[0][i];}
        }
        const double direct=foldedEnergy(naive),filtered=foldedEnergy(aa);
        double directTotal=0,aaTotal=0; for(size_t i=0;i<4096;++i) {directTotal+=double(naive[i])*naive[i]/4096; aaTotal+=double(aa[i])*aa[i]/4096;}
        std::cout<<"ALIAS shape="<<static_cast<int>(shape)<<" direct="<<direct<<" ADAA="<<filtered<<" ratio="<<filtered/std::max(direct,1e-30)
                 <<" directFraction="<<direct/std::max(directTotal,1e-30)<<" aaFraction="<<filtered/std::max(aaTotal,1e-30)<<'\n';
        CHECK(direct>1e-8); CHECK(filtered<direct*.75);
    }
}

void delayTempoStereoAndTail()
{
    section("delayTempoStereoAndTail");
    auto p=impulsePatch(); const auto dry=sound(p,8192,impulseEvents);
    CHECK(arrival(dry)==0); CHECK(energy(dry,1)==0); CHECK(std::abs(dry[0][0])>1e-5);
    add(p,EffectKind::delay); auto& d=p.effects[0].delay; d.seconds=.01f; d.feedback=.6f; d.mix=1; d.filterHz=20000;
    const auto echo=sound(p,48000,impulseEvents); CHECK(std::abs(arrival(echo)-480)<=2); CHECK(finite(echo));
    auto slower=p; slower.effects[0].delay.seconds=.025f;
    CHECK(std::abs(arrival(sound(slower,8192,impulseEvents))-1200)<=2);
    CHECK(energy(echo,480,1000)>energy(echo,24000)); CHECK(energy(echo,900,3000)>1e-10);
    auto q=p; q.effects[0].delay.feedback=0; CHECK(energy(sound(q,8192,impulseEvents),1000)<energy(echo,1000,8192)*.01);
    q=p; q.effects[0].delay.filterHz=200; CHECK(energy(sound(q,8192,impulseEvents))<energy(echo,0,8192)*.1);
    q=p; q.effects[0].delay.mix=0; CHECK(difference(dry,sound(q,8192,impulseEvents))==0);
    q=p; q.synth.layers[0].pan=-1; q.effects[0].delay.stereo=0;
    const auto mono=sound(q,8192,impulseEvents); q.effects[0].delay.stereo=1; const auto cross=sound(q,8192,impulseEvents);
    double right=0; for(auto v:cross[1]) right+=v*v; CHECK(right>1e-9); CHECK(difference(mono,cross)>1e-5);
    d.synced=true; d.beats=.0625f;
    CHECK(std::abs(arrival(sound(p,8192,impulseEvents,48000,512,120))-1500)<=2);
    CHECK(std::abs(arrival(sound(p,8192,impulseEvents,48000,512,60))-3000)<=2);
    auto beats=p; beats.effects[0].delay.beats=.125f;
    CHECK(std::abs(arrival(sound(beats,8192,impulseEvents,48000,512,120))-3000)<=2);
    std::cout<<"DELAY freeArrival="<<arrival(echo)<<" earlyEnergy="<<energy(echo,480,1000)<<" lateEnergy="<<energy(echo,24000)<<'\n';
    for(auto bpm : {std::optional<double>{},std::optional<double>{0},std::optional<double>{401},std::optional<double>{std::numeric_limits<double>::quiet_NaN()}})
    {
        RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(48000,512));
        const auto missing=render(e,8192,impulseEvents,512,bpm); CHECK(e.tempoUnavailable()); CHECK(difference(missing,dry)<1e-7);
        render(e,512,{},512,120); CHECK(!e.tempoUnavailable());
        CHECK(energy(render(e,8192,impulseEvents,512,120),1000)>1e-10);
    }
    RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(48000,512)); render(e,1600,impulseEvents,512,120);
    CHECK(energy(render(e,4096,{},512,120))>1e-12); // Empty MIDI continues feedback.
    render(e,512); CHECK(e.tempoUnavailable()); CHECK(energy(render(e,4096))==0);
    e.reset(); CHECK(energy(render(e,8192,{},512,120))==0);
}

void reverbIndependentControlsAndTail()
{
    section("reverbIndependentControlsAndTail");
    auto p=impulsePatch(); add(p,EffectKind::reverb); auto& r=p.effects[0].reverb;
    r.mix=1; r.size=.3f; r.decaySeconds=1; r.damping=.2f;
    const auto base=sound(p,96000,impulseEvents); CHECK(arrival(base)>500); CHECK(finite(base));
    CHECK(energy(base,4800,12000)>energy(base,72000)); CHECK(energy(base,4800,12000)>1e-12);
    auto q=p; q.effects[0].reverb.size=.9f; const auto big=sound(q,96000,impulseEvents);
    CHECK(arrival(big)>arrival(base)); CHECK(difference(base,big)>1e-5);
    q=p; q.effects[0].reverb.decaySeconds=4; const auto longTail=sound(q,96000,impulseEvents);
    CHECK(arrival(longTail)==arrival(base)); CHECK(energy(longTail,48000)>energy(base,48000)*4);
    q=p; q.effects[0].reverb.damping=.9f; const auto damp=sound(q,96000,impulseEvents);
    CHECK(energy(damp,12000)>0); CHECK(energy(damp,12000)<energy(base,12000));
    q=p; q.effects[0].reverb.predelayMs=50; const auto predelayed=sound(q,96000,impulseEvents);
    CHECK(std::abs(arrival(predelayed)-arrival(base)-2400)<=2);
    q=p; q.effects[0].reverb.mix=0; CHECK(difference(sound(q,8192,impulseEvents),sound(impulsePatch(),8192,impulseEvents))==0);
    double decorrelation=0; for(size_t i=0;i<base[0].size();++i) decorrelation+=std::abs(base[0][i]-base[1][i]); CHECK(decorrelation>1e-3);
    std::cout<<"REVERB arrival="<<arrival(base)<<" sizeArrival="<<arrival(big)<<" late1s="<<energy(base,48000)<<" late4s="<<energy(longTail,48000)<<" decorrelation="<<decorrelation<<'\n';
    RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(48000,512)); render(e,4096,impulseEvents); e.reset(); CHECK(energy(render(e,8192))==0);
}

void compressorLinkedDynamics()
{
    section("compressorLinkedDynamics");
    auto p=flat(); p.synth.layers[0].pan=.4f; add(p,EffectKind::compressor);
    auto& c=p.effects[0].compressor; c.thresholdDb=-50; c.ratio=10; c.attackMs=.1f; c.releaseMs=100;
    const auto compressed=sound(p,12000); auto q=p; q.effects[0].compressor.mix=0; const auto dry=sound(q,12000);
    CHECK(energy(compressed,4096)<energy(dry,4096)*.1);
    double ratioError=0; for(size_t i=0;i<dry[0].size();++i) if(std::abs(dry[0][i])>1e-4f && std::abs(compressed[0][i])>1e-7f)
        ratioError=std::max(ratioError,std::abs(double(compressed[1][i]/compressed[0][i])-double(dry[1][i]/dry[0][i])));
    CHECK(ratioError<1e-5);
    q=p; q.effects[0].compressor.ratio=1; CHECK(difference(sound(q,12000),dry)<1e-7);
    q=p; q.effects[0].compressor.thresholdDb=0; CHECK(difference(sound(q,12000),dry)<1e-7);
    q=p; q.effects[0].compressor.attackMs=100; CHECK(energy(sound(q,12000),0,1000)>energy(compressed,0,1000)*2);
    q=p; q.effects[0].compressor.makeupDb=6; CHECK(energy(sound(q,12000),4096)>energy(compressed,4096)*3.9);
    const std::vector<Event> step {{0,on()}, {4800,off()}, {4801,on(69,1,.2f)}};
    q=p; q.effects[0].compressor.releaseMs=10; const auto fast=sound(q,24000,step);
    q.effects[0].compressor.releaseMs=2000; const auto slow=sound(q,24000,step);
    CHECK(energy(fast,6000,12000)>energy(slow,6000,12000)*1.1);
    CHECK(energy(sound(p,8192,{}))==0); CHECK(finite(compressed));
    std::cout<<"COMPRESSOR ratioError="<<ratioError<<" compressed="<<energy(compressed,4096)<<" dry="<<energy(dry,4096)<<'\n';
}

void muteSoloAndMasterTail()
{
    section("muteSoloAndMasterTail"); auto p=flat(); const auto dry=sound(p,2048);
    p.solo[3]=true; CHECK(difference(dry,sound(p,2048))==0);
    addLayer(p); p.synth.layers[1].enabled=false; p.solo[1]=true; CHECK(difference(dry,sound(p,2048))==0);
    p.synth.layers[1].enabled=true; p.solo[1]=false; p.mute[1]=true; CHECK(difference(dry,sound(p,2048))==0);
    p.solo[1]=true; CHECK(energy(sound(p,2048))==0);
    p=impulsePatch(); add(p,EffectKind::delay,0); p.effects[0].delay.seconds=.01f; p.effects[0].delay.mix=1; p.effects[0].delay.feedback=.8f;
    RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(48000,512)); render(e,600,impulseEvents);
    p.synth.masterLevel=0; CHECK(e.setPatch(p)); const auto fade=render(e,512); CHECK(energy(fade,240)==0); CHECK(energy(render(e,4096))==0);
    p.synth.masterLevel=1; CHECK(e.setPatch(p)); CHECK(energy(render(e,4096))>1e-12);
    p.mute[0]=true; CHECK(e.setPatch(p)); render(e,512); CHECK(energy(render(e,1024))==0);
    p.mute[0]=false; CHECK(e.setPatch(p)); CHECK(energy(render(e,2048))>1e-14);
}

Audio editSequence(double rate, int partition)
{
    auto p=flat(); addLayer(p); p.synth.layers[1].enabled=false; add(p,EffectKind::delay,0);
    p.effects[0].delay.seconds=.01f; p.effects[0].delay.feedback=.8f; p.effects[0].delay.mix=.7f;
    p.synth.layers[0].ampEnvelope.release=.1f;
    RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(rate,512));
    Audio result; const auto append=[&](Audio a){for(size_t c=0;c<2;++c)result[c].insert(result[c].end(),a[c].begin(),a[c].end());};
    append(render(e,900,{{0,on()}},partition));
    RackEdit move; move.kind=RackEditKind::moveEffect; move.slot=0; move.destination=4; move.position=0;
    CHECK(editRackLayout(p.layout,move,p.layout)==RackEditResult::applied); CHECK(e.setPatch(p)); append(render(e,71,{},partition));
    p.layout.effects[0].kind=EffectKind::reverb; CHECK(e.setPatch(p)); append(render(e,83,{},partition));
    p.layout.effects[0].kind=EffectKind::drive; p.effects[0].drive.gainDb=24; CHECK(e.setPatch(p)); append(render(e,800,{},partition));
    p.effects[0].bypassed=true; CHECK(e.setPatch(p)); append(render(e,600,{{17,off()}},partition));
    p.effects[0].drive.mix=.2f; CHECK(e.setPatch(p));
    for(int i=0;i<300;++i) {CHECK(e.setPatch(p)); append(render(e,1,{},partition));}
    CHECK(e.patch()==p); CHECK(e.patch().effects[0].delay.feedback==.8f);
    append(render(e,800,{},partition)); return result;
}
void editsRecallAndPartitions()
{
    section("editsRecallAndPartitions");
    for(double rate : {44100.0,48000.0,96000.0})
    {
        const auto single=editSequence(rate,1);
        for(int partition : {64,512}) {const auto blocked=editSequence(rate,partition); CHECK(difference(single,blocked)<1e-7); CHECK(finite(blocked)); CHECK(peak(blocked)<1);}
        std::cout<<"PARTITION rate="<<rate<<" peak="<<peak(single)<<'\n';
    }
    auto p=impulsePatch(); add(p,EffectKind::delay,0); p.effects[0].delay.seconds=.01f; p.effects[0].delay.mix=1; p.effects[0].delay.feedback=.9f;
    RoutedSynthEngine e, ref; CHECK(e.setPatch(p)); CHECK(ref.setPatch(p)); CHECK(e.prepare(48000,512)); CHECK(ref.prepare(48000,512));
    const auto first=render(e,8192,impulseEvents); CHECK(energy(first)>1e-10); e.reset(); CHECK(difference(first,render(e,8192,impulseEvents))==0);
    CHECK(e.prepare(48000,512)); CHECK(difference(first,render(e,8192,impulseEvents))==0);
    render(ref,8192,impulseEvents);
    juce::MidiBuffer stop; stop.addEvent(juce::MidiMessage::midiStop(),0);
    for(int channels : {1,3}) {juce::AudioBuffer<float> bad(channels,64); e.render(bad,stop,120); CHECK(bad.getMagnitude(0,64)==0);}
    juce::AudioBuffer<float> oversized(2,513),empty(2,0); e.render(oversized,stop); e.render(empty,stop);
    CHECK(difference(render(e,512),render(ref,512))==0);
    // Kill in a block with already-ringing wet state; no tail survives the offset.
    const auto stopped=render(e,1024,{{211,juce::MidiMessage::midiStop()}}); CHECK(energy(stopped,211)==0);
    e.reset(); render(e,600,impulseEvents); auto removed=p;
    RackEdit remove; remove.kind=RackEditKind::removeEffect; remove.slot=0;
    CHECK(editRackLayout(removed.layout,remove,removed.layout)==RackEditResult::applied); CHECK(e.setPatch(removed));
    const auto fade=render(e,512); CHECK(energy(fade,240)==0); CHECK(e.setPatch(p)); render(e,1024); CHECK(energy(render(e,4096))==0);
    // Bypassed recall cannot resurrect the old ringing delay.
    e.reset(); render(e,600,impulseEvents); p.effects[0].bypassed=true; CHECK(e.setPatch(p)); render(e,1024);
    p.effects[0].bypassed=false; CHECK(e.setPatch(p)); render(e,1024); CHECK(energy(render(e,4096))==0);
    // Identical settings preserve envelope release and running tail exactly.
    p=flat(); p.synth.layers[0].ampEnvelope.release=.2f; add(p,EffectKind::delay); p.effects[0].delay.seconds=.01f;
    CHECK(e.setPatch(p)); CHECK(ref.setPatch(p)); e.reset(); ref.reset();
    render(e,1000,{{0,on()},{500,off()}}); render(ref,1000,{{0,on()},{500,off()}});
    Audio repeated; for(int i=0;i<500;++i) {CHECK(e.setPatch(p)); const auto a=render(e,1); for(size_t c=0;c<2;++c)repeated[c].push_back(a[c][0]);}
    CHECK(energy(repeated)>1e-8); CHECK(difference(repeated,render(ref,500))==0);
}

void controlRampsAndBoundaryCases()
{
    section("controlRampsAndBoundaryCases");
    // Each bank's mix is linear, including a partial mix, against measured
    // native source and wet output. Drive's dry reference is its midpoint.
    for(auto kind : {EffectKind::drive,EffectKind::delay,EffectKind::reverb,EffectKind::compressor})
    {
        auto p=flat(); add(p,kind);
        const auto setMix=[&](float value) {p.effects[0].drive.mix=p.effects[0].delay.mix=p.effects[0].reverb.mix=p.effects[0].compressor.mix=value;};
        p.effects[0].compressor.thresholdDb=-50; p.effects[0].delay.seconds=.003f;
        setMix(0); const auto dry=sound(p,4096); setMix(1); const auto wet=sound(p,4096);
        setMix(.3f); const auto partial=sound(p,4096);
        double error=0; for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<4096;++i)
            error=std::max(error,std::abs(double(partial[ch][i])-(.7*dry[ch][i]+.3*wet[ch][i])));
        CHECK(error<1e-7); CHECK(difference(dry,wet)>1e-5);
        // Ramp only mix from dry to wet. Engines have identical note/history
        // states, so each frame independently exposes the exact ramp gain.
        RoutedSynthEngine transitioning,dryEngine,wetEngine;
        setMix(0); CHECK(transitioning.setPatch(p)); CHECK(dryEngine.setPatch(p));
        setMix(1); CHECK(wetEngine.setPatch(p));
        CHECK(transitioning.prepare(48000,512)); CHECK(dryEngine.prepare(48000,512)); CHECK(wetEngine.prepare(48000,512));
        render(transitioning,2048,{{0,on()}}); render(dryEngine,2048,{{0,on()}}); render(wetEngine,2048,{{0,on()}});
        CHECK(transitioning.setPatch(p));
        Audio ramped; for(int i=0;i<300;++i) {CHECK(transitioning.setPatch(p));const auto a=render(transitioning,1);for(size_t ch=0;ch<2;++ch)ramped[ch].push_back(a[ch][0]);}
        const auto d=render(dryEngine,300),w=render(wetEngine,300);
        error=0; for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<300;++i)
        {const double t=std::min(1.0,double(i+1)/240);error=std::max(error,std::abs(double(ramped[ch][i])-(d[ch][i]*(1-t)+w[ch][i]*t)));}
        std::cout<<"MIX_RAMP kind="<<static_cast<int>(kind)<<" error="<<error<<'\n'; CHECK(error<1e-7);
    }
    auto p=impulsePatch(); add(p,EffectKind::delay); p.effects[0].delay.synced=true; p.effects[0].delay.beats=.0625f;
    p.effects[0].delay.mix=1; p.effects[0].delay.feedback=.9f;
    RoutedSynthEngine e, reference; CHECK(e.setPatch(p)); CHECK(reference.setPatch(p)); CHECK(e.prepare(48000,512)); CHECK(reference.prepare(48000,512));
    render(e,1600,impulseEvents,512,120); render(reference,1600,impulseEvents,512,120);
    CHECK(!e.tempoUnavailable()); juce::MidiBuffer stop; stop.addEvent(juce::MidiMessage::midiStop(),0);
    juce::AudioBuffer<float> empty(2,0),oversized(2,513); e.render(empty,stop); e.render(oversized,stop);
    std::array<float,100> shared {}; std::array<float*,2> channels {shared.data(),shared.data()+3};
    juce::AudioBuffer<float> aliased(channels.data(),2,64); e.render(aliased,stop);
    CHECK(!e.tempoUnavailable()); CHECK(difference(render(e,512,{},512,120),render(reference,512,{},512,120))==0);
    // A rejected patch while a feedback tail is ringing cannot touch its state.
    auto bad=p; bad.effects[7].reverb.predelayMs=std::numeric_limits<float>::infinity(); CHECK(!e.setPatch(bad));
    CHECK(difference(render(e,512,{},512,120),render(reference,512,{},512,120))==0);
    // No fresh wet input while tempo is missing: let both histories decay for
    // the same duration, but inject an impulse into only the unavailable one.
    render(e,600,{},512); render(reference,600,{},512);
    render(e,4000,impulseEvents,512); render(reference,4000,{},512);
    CHECK(difference(render(e,4000,{},512,120),render(reference,4000,{},512,120))==0);
    // Synced boundary times, including the slowest tempo, and reprepare rates.
    p.effects[0].delay.beats=1;
    for(double rate : {8000.0,44100.0,96000.0})
    {
        const auto a=sound(p,static_cast<int>(rate*3.01),impulseEvents,rate,512,20);
        CHECK(std::abs(arrival(a)-static_cast<int>(rate*3))<=2); CHECK(finite(a));
    }
    p.effects[0].delay.synced=false; p.effects[0].delay.seconds=4;
    CHECK(std::abs(arrival(sound(p,32020,impulseEvents,8000))-32000)<=2);
    // Endpoint MIDI Stop must be ignored; in-range Stop clears exactly at sample.
    auto continuous=flat(); add(continuous,EffectKind::reverb); CHECK(e.setPatch(continuous)); e.reset();
    juce::AudioBuffer<float> block(2,64); juce::MidiBuffer note; note.addEvent(on(),0); e.render(block,note);
    juce::MidiBuffer outside; outside.addEvent(juce::MidiMessage::midiStop(),-1); outside.addEvent(juce::MidiMessage::midiStop(),64);
    e.render(block,outside); CHECK(e.activeNoteCount()==1); CHECK(block.getMagnitude(0,64)>0);
    juce::MidiBuffer inside; inside.addEvent(juce::MidiMessage::midiStop(),31); e.render(block,inside); CHECK(e.activeNoteCount()==0); CHECK(block.getMagnitude(31,33)==0);
    // Unprepared/reset and invalid prepare preserve the public retained patch.
    RoutedSynthEngine unprepared; CHECK(unprepared.setPatch(continuous)); unprepared.reset(); unprepared.render(block,note,120);
    CHECK(block.getMagnitude(0,64)==0); CHECK(unprepared.activeNoteCount()==0); CHECK(unprepared.patch()==continuous);
}

void structuralTailAndFadeEvidence()
{
    section("structuralTailAndFadeEvidence");
    for(double rate : {44100.0,48000.0,96000.0})
    {
        const int ramp=static_cast<int>(std::ceil(rate*.005));
        for(auto kind : {RackEditKind::moveEffect,RackEditKind::removeEffect,RackEditKind::replaceEffect})
        {
            Audio single;
            for(int partition : {1,64,512})
            {
                auto p=impulsePatch(); add(p,EffectKind::delay,0); p.effects[0].delay.seconds=.01f;
                p.effects[0].delay.feedback=.95f; p.effects[0].delay.mix=1;
                RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(rate,512));
                const auto tail=render(e,static_cast<int>(rate*.031),impulseEvents,partition);
                CHECK(energy(tail)>1e-10); CHECK(e.activeNoteCount()==0);
                auto q=p; RackEdit edit; edit.kind=kind; edit.slot=0; edit.destination=4; edit.effect=EffectKind::reverb; edit.allowReuse=true;
                CHECK(editRackLayout(q.layout,edit,q.layout)==RackEditResult::applied); CHECK(e.setPatch(q));
                const auto transition=render(e,ramp*3,{},partition);
                CHECK(energy(transition,ramp)==0); CHECK(finite(transition)); CHECK(peak(transition)<=peak(tail));
                CHECK(e.patch().effects==p.effects);
                if(partition==1) single=transition; else CHECK(difference(single,transition)==0);
                CHECK(e.setPatch(p)); render(e,ramp*3,{},partition); CHECK(energy(render(e,4096,{},partition))==0);
            }
        }
        // A transparent compressor moved from insert to master isolates the
        // fade itself: measured source * exact down/up gain, with no DSP change.
        auto p=flat(); add(p,EffectKind::compressor,0); p.effects[0].compressor.ratio=1;
        RoutedSynthEngine e,dry; CHECK(e.setPatch(p)); CHECK(dry.setPatch(flat())); CHECK(e.prepare(rate,512)); CHECK(dry.prepare(rate,512));
        render(e,1000,{{0,on()}}); render(dry,1000,{{0,on()}});
        RackEdit move; move.kind=RackEditKind::moveEffect; move.slot=0; move.destination=4;
        CHECK(editRackLayout(p.layout,move,p.layout)==RackEditResult::applied); CHECK(e.setPatch(p));
        Audio actual; for(int i=0;i<ramp*2+32;++i) {CHECK(e.setPatch(p));const auto a=render(e,1);for(size_t ch=0;ch<2;++ch)actual[ch].push_back(a[ch][0]);}
        const auto source=render(dry,ramp*2+32); double error=0;
        for(size_t ch=0;ch<2;++ch)for(int i=0;i<ramp*2+32;++i)
        {const double gain=i<ramp ? 1-double(i+1)/ramp : std::min(1.0,double(i-ramp+1)/ramp);error=std::max(error,std::abs(double(actual[ch][static_cast<size_t>(i)])-source[ch][static_cast<size_t>(i)]*gain));}
        std::cout<<"STRUCTURAL_FADE rate="<<rate<<" samples="<<ramp<<" error="<<error<<'\n'; CHECK(error<1e-7);
    }
    // Editing one insert preserves an independent rack's existing tail.
    auto p=impulsePatch(); addLayer(p); p.synth.layers[0].noiseLevel=0;
    add(p,EffectKind::delay,0); add(p,EffectKind::delay,1);
    for(auto& settings:p.effects) {settings.delay.seconds=.01f;settings.delay.feedback=.95f;settings.delay.mix=1;}
    RoutedSynthEngine e,ref; CHECK(e.setPatch(p));CHECK(ref.setPatch(p));CHECK(e.prepare(48000,512));CHECK(ref.prepare(48000,512));
    render(e,600,impulseEvents);render(ref,600,impulseEvents);
    p.layout.effects[0].kind=EffectKind::reverb;CHECK(e.setPatch(p));render(e,600);render(ref,600);
    const auto retained=render(e,4096),expected=render(ref,4096);CHECK(energy(retained)>1e-10);CHECK(difference(retained,expected)==0);
}

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking=false; thread_local int allocations=0,frees=0;
int __cdecl allocationHook(int kind,void*,size_t,int,long,const unsigned char*,int)
{ if(tracking && (kind==_HOOK_ALLOC || kind==_HOOK_REALLOC))++allocations; if(tracking&&kind==_HOOK_FREE)++frees; return 1; }
#endif
void routedModulationClockAndCapacity()
{
    section("routedModulationClockAndCapacity");
    auto p=flat();p.synth.layers[0].level=.5f;p.synth.layers[0].lfos[0]={LfoShape::sine,true,1,1};
    p.synth.layers[0].modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};
    auto carrier=p;carrier.synth.layers[0].modulation={};carrier.synth.layers[0].level=1;
    for(double rate:{44100.,48000.,96000.})for(int partition:{1,64,512})
    {
        const auto a=sound(p,9000,{{0,on()}},rate,partition,120),b=sound(carrier,9000,{{0,on()}},rate,partition);
        double error=0;for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<a[ch].size();++i)error=std::max(error,std::abs(a[ch][i]-b[ch][i]*(.5+.25*std::sin(2*std::numbers::pi*i*2/rate))));
        CHECK(error<4e-9);CHECK(energy(a)>1e-7);
    }
    RoutedSynthEngine e,reference;e.setPatch(p);reference.setPatch(carrier);e.prepare(48000,512);reference.prepare(48000,512);
    render(e,3000,{{0,on()}},512,120);render(reference,3000,{{0,on()}});
    const auto frozen=render(e,3000),dry=render(reference,3000);double error=0;
    for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<dry[ch].size();++i)error=std::max(error,std::abs(frozen[ch][i]-dry[ch][i]*(.5+.25*std::sin(std::numbers::pi/4))));
    CHECK(error<4e-9);CHECK(e.tempoUnavailable());CHECK(energy(frozen)>1e-7);
    juce::AudioBuffer<float> invalid(1,100),emptyBuffer(2,0);juce::MidiBuffer midi;e.render(invalid,midi,120);e.render(emptyBuffer,midi,120);CHECK(e.tempoUnavailable());
    render(e,100,{},64,120);CHECK(!e.tempoUnavailable());
    // Stop clears both effect histories and clock phase at the event offset.
    auto delay=p;add(delay,EffectKind::delay);delay.effects[0].delay.seconds=.01f;delay.effects[0].delay.mix=.7f;delay.effects[0].delay.feedback=.5f;
    const auto stopped=sound(delay,4096,{{0,on()},{913,juce::MidiMessage::midiStop()},{913,on()}},48000,64,120);
    const auto fresh=sound(delay,3183,{{0,on()}},48000,512,120);
    CHECK(difference(Audio{std::vector<float>(stopped[0].begin()+913,stopped[0].end()),std::vector<float>(stopped[1].begin()+913,stopped[1].end())},fresh)==0);
    // Clock status and delay status combine independently. A clock timing loss
    // does not kill a free delay tail; mute gates fresh notes, master tail lasts.
    e.setPatch(delay);e.reset();render(e,3000,{{0,on()},{1000,off()}},512,120);CHECK(e.activeNoteCount()==0);
    const auto tail=render(e,4096);CHECK(e.tempoUnavailable());CHECK(energy(tail)>1e-12);
    delay.synth.layers[0].lfos[0].synced=false;delay.effects[0].delay.synced=true;e.setPatch(delay);render(e,512);CHECK(e.tempoUnavailable());render(e,512,{},512,120);CHECK(!e.tempoUnavailable());
    delay.effects[0].delay.synced=false;delay.synth.layers[0].lfos[0].synced=true;e.setPatch(delay);e.reset();render(e,2000,{{0,on()}},512,120);delay.mute[0]=true;e.setPatch(delay);render(e,512,{},512,120);CHECK(energy(render(e,4096))>1e-10);CHECK(e.tempoUnavailable());
    // All four racks, sixteen routes each, sixteen notes and all eight effects.
    p=flat();for(int i=0;i<3;++i)addLayer(p);for(int i=0;i<8;++i)add(p,static_cast<EffectKind>(i%4),i%5);
    for(auto& l:p.synth.layers){l.level=.4f;l.lfos[1].synced=true;for(size_t r=0;r<16;++r)l.modulation[r]={true,static_cast<ModSource>(r%6+1),r%2?ModTarget::cutoff:ModTarget::level,r%2?ModBank::analog:ModBank::common,r%2?.125f:-.125f};}
    e.setPatch(p);e.reset();juce::AudioBuffer<float> out(2,512);juce::MidiBuffer notes,stop;for(int n=48;n<64;++n)notes.addEvent(on(n),0);stop.addEvent(juce::MidiMessage::midiStop(),71);
    bool accepted=true,allFinite=true;
#if defined(_MSC_VER) && defined(_DEBUG)
    auto oldHook=_CrtSetAllocHook(allocationHook);allocations=0;tracking=true;
#endif
    e.render(out,notes,120);const auto count=e.activeNoteCount();
    for(int i=0;i<40;++i){p.synth.layers[size_t(i%4)].lfos[0].rateHz=float(i+1);p.outputGainDb=i%2?36.f:-24.f;p.outputProtection=i%2==0;accepted&=e.setPatch(p);e.render(out,midi,i%2?std::optional<double>{90}:std::nullopt);for(int s=0;s<512;++s)allFinite&=std::isfinite(out.getSample(0,s));}
    e.render(out,stop);e.reset();e.render(out,notes,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(oldHook);CHECK(allocations==0);std::cout<<"ROUTED_MODULATION allocations="<<allocations<<'\n';
#endif
    CHECK(accepted);CHECK(allFinite);CHECK(count==16);CHECK(e.activeNoteCount()==16);
}

void preparedOperationsAllocateNothing()
{
    section("preparedOperationsAllocateNothing");
    auto p=flat(); for(int i=0;i<3;++i)addLayer(p); for(int i=0;i<8;++i)add(p,static_cast<EffectKind>(i%4),i%5);
    RoutedSynthEngine e; CHECK(e.setPatch(p)); CHECK(e.prepare(48000,512));
    juce::AudioBuffer<float> out(2,512); juce::MidiBuffer midi,empty,stop; midi.addEvent(on(),0); stop.addEvent(juce::MidiMessage::midiStop(),113);
#if defined(_MSC_VER) && defined(_DEBUG)
    auto old=_CrtSetAllocHook(allocationHook); allocations=0; tracking=true;
#endif
    bool accepted=true; e.render(out,midi,120);
    for(int i=0;i<40;++i)
    {
        p.effects[0].drive.gainDb=static_cast<float>(i%25); p.effects[1].delay.synced=(i%2==0);
        p.effects[3].bypassed=(i%3==0); p.layout.effects[4].kind=static_cast<EffectKind>(i%4);
        accepted &= e.setPatch(p); e.render(out,empty,i%2?std::optional<double>{90}:std::nullopt);
        if(i%7==0) {e.reset();e.render(out,midi,120);}
    }
    e.render(out,stop,90);
    e.reset();
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false; _CrtSetAllocHook(old); CHECK(allocations==0); std::cout<<"ALLOCATIONS "<<allocations<<'\n';
#else
    std::cout<<"ALLOCATIONS instrumentation unavailable\n";
#endif
    CHECK(accepted); CHECK(out.getMagnitude(0,512)<4);
}
void qualityEffectStereoTail()
{
    section("quality_effect_stereo_tail");
    // Actual excitation is the retained filtered one-sample noise note, not
    // an ideal delta or a promise about the reverb's impulse response.
    for(double rate:{44100.,48000.,96000.}){
        auto p=impulsePatch();const int frames=int(rate*2);
        const auto dry=sound(p,frames,impulseEvents,rate,64);CHECK(energy(dry)>1e-12);
        add(p,EffectKind::reverb);p.effects[0].reverb={.3f,4,.2f,0,1};
        const auto wet=sound(p,frames,impulseEvents,rate,512),fine=sound(p,frames,impulseEvents,rate,64);
        CHECK(finite(wet)&&peak(wet)<1);CHECK(difference(wet,fine)<=1e-6);CHECK(energy(wet,int(rate*.5))>1e-16);
        double side=0;for(std::size_t i=0;i<wet[0].size();++i)side+=std::pow(double(wet[0][i])-wet[1][i],2);
        CHECK(side>1e-10);auto bypass=p;bypass.effects[0].bypassed=true;CHECK(difference(dry,sound(bypass,frames,impulseEvents,rate,64))<=1e-6);
        std::cout<<"QUALITY_EFFECT rate="<<rate<<" excitation=filtered_single_sample_noise_note late_energy="<<energy(wet,int(rate*.5))<<" side_squared_sum="<<side<<" partition_error="<<difference(wet,fine)<<'\n';
    }
}
void qualityCallbackSafety()
{
    section("quality_callback_allocations_frees");
    auto p=flat();for(int n=0;n<3;++n)addLayer(p);
    for(int n=0;n<8;++n)add(p,static_cast<EffectKind>(n%4),n<4?n:masterChain);
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));
        juce::AudioBuffer<float> buffer(2,512);juce::MidiBuffer held,released,empty;held.ensureSize(4096);released.ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(on(n),0);released.addEvent(juce::MidiMessage::allNotesOff(1),17);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto old=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;
        for(int block=0;block<96;++block)e.render(buffer,block==0?held:block==32?released:empty,120);
        e.reset();tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
        std::cout<<"QUALITY_CALLBACK rate="<<rate<<" quality="<<int(quality)<<" layers=4 notes=16 fx=8 callbacks=96 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false); // Same supported MSVC Debug acceptance contract as Core.
#endif
    }
}
}
template<class Engine>
void preparedQualityContract()
{
    section("quality_prepared_mode_truth");
    if constexpr (requires(Engine& e) { e.effectiveQuality(); e.latencySamples(); })
    {
        using Quality = decltype(std::declval<Engine&>().effectiveQuality());
        const auto realtime=static_cast<Quality>(0), higher=static_cast<Quality>(1);
        auto p=flat(); add(p,EffectKind::delay); p.effects[0].delay.mix=.4f;
        Engine a, control; CHECK(a.setPatch(p) && control.setPatch(p));
        CHECK(a.prepare(48000,512) && control.prepare(48000,512));
        CHECK(a.effectiveQuality()==realtime && a.latencySamples()==0);
        CHECK(difference(render(a,2048,{{17,on()}}),render(control,2048,{{17,on()}}))==0);
        CHECK(a.activeNoteCount()==1);
        CHECK(!a.prepare(48000,512,static_cast<Quality>(255)));
        CHECK(!a.prepare(384001,512,higher) && !a.prepare(48000,0,higher));
        CHECK(a.effectiveQuality()==realtime && a.activeNoteCount()==1);
        CHECK(difference(render(a,2048),render(control,2048))==0);
        CHECK(a.prepare(48000,512,higher));
        CHECK(a.effectiveQuality()==higher && a.latencySamples()>0 && a.activeNoteCount()==0);
        section("quality_latency_and_partition");
        Engine b; CHECK(b.setPatch(p) && b.prepare(48000,512,higher));
        const std::vector<Event> events{{17,on()},{2047,off()}};
        const auto coarse=render(a,32768,events,512,120),fine=render(b,32768,events,64,120);
        CHECK(energy(coarse)>1e-8 && difference(coarse,fine)<=1e-6);
        CHECK(a.prepare(48000,512) && a.effectiveQuality()==realtime && a.latencySamples()==0);
    }
    else CHECK(false);
}
void modernReverbAdmissionAndTransition()
{
    section("modernReverbAdmissionAndTransition");
    auto legacy=impulsePatch(); add(legacy,EffectKind::reverb);
    legacy.effects[0].reverb={.3f,4,.2f,0,1};
    auto modern=legacy; modern.effects[0].algorithmRevision=2;
    modern.compatibility.effects[0][static_cast<std::size_t>(EffectKind::reverb)]=2;
    CHECK(validateRoutedPatch(modern));
    if(!validateRoutedPatch(modern)) return;
    auto mismatched=modern; mismatched.compatibility.effects[0][2]=1; CHECK(!validateRoutedPatch(mismatched));
    mismatched=modern; mismatched.effects[0].algorithmRevision=1; CHECK(!validateRoutedPatch(mismatched));
    for(auto kind:{EffectKind::stereo,EffectKind::phaser}) {
        auto bad=legacy; bad.effects[0].algorithmRevision=2;
        bad.layout.effects[0].kind=kind; bad.compatibility.effects[0][static_cast<std::size_t>(kind)]=2;
        CHECK(!validateRoutedPatch(bad));
    }
    for(double rate:{44100.,48000.,96000.}) for(auto quality:{RenderQuality::realtime,RenderQuality::higher}) {
        RoutedSynthEngine a,b; CHECK(a.setPatch(modern)&&b.setPatch(modern));
        CHECK(a.prepare(rate,512,quality)&&b.prepare(rate,512,quality));
        auto coarse=render(a,int(rate*.5),impulseEvents,512),fine=render(b,int(rate*.5),impulseEvents,64);
        CHECK(finite(coarse)&&energy(coarse)>0&&difference(coarse,fine)==0);
        CHECK(a.setPatch(legacy)); render(a,int(rate*.05));
        CHECK(a.setPatch(modern)); render(a,int(rate*.05));
        CHECK(energy(render(a,int(rate*.1)))==0);
        a.reset(); CHECK(difference(render(a,int(rate*.5),impulseEvents),coarse)==0);
    }
}
void modernReverbClockAndStatePreservation()
{
    section("modern_reverb_clock_controls_invalid_prepare_buffers_stop_and_reset");
    auto patch=flat();patch.synth.layers[0].level=.2f;add(patch,EffectKind::reverb);
    patch.effects[0].algorithmRevision=2;patch.compatibility.effects[0][2]=2;patch.effects[0].reverb={.4f,4,.3f,13,.75f};
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine a,b;CHECK(a.setPatch(patch)&&b.setPatch(patch));CHECK(a.prepare(rate,512,quality)&&b.prepare(rate,512,quality));
        const std::vector<Event> phrase{{17,on()},{2203,off()}};
        const auto original=render(a,8191,phrase,73),reference=render(b,8191,phrase,512);
        CHECK(finite(original)&&energy(original)>1e-12&&difference(original,reference)==0);
        auto controlled=patch;controlled.effects[0].reverb={.83f,.65f,.72f,17,.4f};CHECK(a.setPatch(controlled)&&b.setPatch(controlled));
        const auto moved=render(a,int(rate*2),{},64),movedReference=render(b,int(rate*2),{},511);
        CHECK(finite(moved)&&energy(moved)>0&&difference(moved,movedReference)==0);
        a.reset();b.reset();CHECK(difference(render(a,4093,{{23,on()}},73),render(b,4093,{{23,on()}},512))==0);
        const auto heldNotes=a.activeNoteCount();CHECK(heldNotes==1);
        CHECK(!a.prepare(0,512,quality)&&!a.prepare(rate,0,quality)&&!a.prepare(384001,512,quality));
        CHECK(!a.prepare(rate,512,static_cast<RenderQuality>(255)));
        CHECK(a.patch()==controlled&&a.effectiveQuality()==quality&&a.activeNoteCount()==heldNotes);
        auto unsupported=controlled;unsupported.compatibility.effects[7][3]=3;CHECK(!a.setPatch(unsupported));CHECK(a.patch()==controlled);
        unsupported=controlled;unsupported.compatibility.effects[0][2]=1;CHECK(!a.setPatch(unsupported));CHECK(a.patch()==controlled);
        juce::MidiBuffer stop;stop.addEvent(juce::MidiMessage::midiStop(),0);
        juce::AudioBuffer<float> empty(2,0),mono(1,64),oversized(2,513);
        a.render(empty,stop);a.render(mono,stop);a.render(oversized,stop);
        std::array<float,100> storage{};std::array<float*,2> channels{storage.data(),storage.data()+3};juce::AudioBuffer<float> aliased(channels.data(),2,64);a.render(aliased,stop);
        CHECK(a.activeNoteCount()==heldNotes);CHECK(difference(render(a,2048,{},73),render(b,2048,{},512))==0);
        // The actual Stop offset resets the modern wet clock. Higher mode keeps
        // its real FIR support; only samples after that support match a fresh note.
        const int total=8192,offset=913,support=quality==RenderQuality::higher?42:0;
        const auto stopped=render(a,total,{{offset,juce::MidiMessage::midiStop()},{offset,on()},{offset+1420,off()}},73);
        RoutedSynthEngine fresh;CHECK(fresh.setPatch(controlled)&&fresh.prepare(rate,512,quality));
        const auto restarted=render(fresh,total-offset,{{0,on()},{1420,off()}},512);
        double postSupportError=0;for(std::size_t ch=0;ch<2;++ch)for(int sample=support;sample<total-offset;++sample)
            postSupportError=std::max(postSupportError,std::abs(double(stopped[ch][std::size_t(offset+sample)])-restarted[ch][std::size_t(sample)]));
        CHECK(finite(stopped)&&energy(stopped,0,offset)>0&&postSupportError<=1e-6);
        a.reset();fresh.reset();const std::vector<Event> resetPhrase{{17,on()},{1403,off()}};
        const auto reset=render(a,8192,resetPhrase,73),resetReference=render(fresh,8192,resetPhrase,512);
        CHECK(finite(reset)&&energy(reset)>0&&difference(reset,resetReference)==0);
        std::cout<<"MODERN_STATE rate="<<rate<<" quality="<<int(quality)<<" stop_offset="<<offset<<" fir_support="<<support<<" post_support_error="<<postSupportError<<'\n';
    }
}
void modernReverbPreparedCallbacks()
{
    section("modern_reverb_prepared_first_and_subsequent_callbacks_allocations_frees");
    auto patch=flat();for(int layer=0;layer<3;++layer)addLayer(patch);patch.synth.masterLevel=.25f;patch.outputProtection=true;
    for(auto& layer:patch.synth.layers){layer.level=.2f;for(std::size_t route=0;route<16;++route)
        layer.modulation[route]={true,route%2?ModSource::lfo1:ModSource::lfo2,route%2?ModTarget::cutoff:ModTarget::level,route%2?ModBank::analog:ModBank::common,.001f};}
    for(int slot=0;slot<8;++slot){add(patch,EffectKind::reverb,slot%5);patch.effects[std::size_t(slot)].algorithmRevision=2;patch.compatibility.effects[std::size_t(slot)][2]=2;}
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto current=patch;RoutedSynthEngine engine;CHECK(engine.setPatch(current)&&engine.prepare(geometry.first,geometry.second,quality));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        bool bounded=true,accepted=true;const auto inspect=[&]{for(int ch=0;ch<2;++ch)for(int sample=0;sample<geometry.second;++sample){const auto value=audio.getSample(ch,sample);bounded&=std::isfinite(value)&&std::abs(value)<=.891252f;}};
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
        engine.render(audio,held,120);tracking=false;inspect();CHECK(allocations==0&&frees==0);CHECK(engine.activeNoteCount()==16&&audio.getMagnitude(0,geometry.second)>0);
        std::cout<<"MODERN_ROUTED_FIRST rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<int(quality)<<" callbacks=1 allocations="<<allocations<<" frees="<<frees<<'\n';
        allocations=0;frees=0;tracking=true;
        for(int block=0;block<96;++block){current.effects[std::size_t(block%8)].reverb={.1f+.005f*float(block%30),.25f+.01f*float(block%30),.2f,17,.4f};
            accepted&=engine.setPatch(current);engine.render(audio,block==32?released:block==64?stop:empty,120);inspect();}
        tracking=false;CHECK(allocations==0&&frees==0);
        std::cout<<"MODERN_ROUTED_SUBSEQUENT rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<int(quality)<<" layers=4 notes=16 effects=8 routes=64 callbacks=96 allocations="<<allocations<<" frees="<<frees<<'\n';
        for(auto algorithm:{AlgorithmRevision(1),AlgorithmRevision(2)}){
            for(std::size_t slot=0;slot<8;++slot){current.effects[slot].algorithmRevision=algorithm;current.compatibility.effects[slot][2]=algorithm;}
            allocations=0;frees=0;tracking=true;accepted&=engine.setPatch(current);engine.render(audio,held,120);inspect();
            for(int block=0;block<4;++block){engine.render(audio,empty,120);inspect();}
            engine.reset();engine.render(audio,held,120);inspect();tracking=false;CHECK(allocations==0&&frees==0);
            std::cout<<"MODERN_ROUTED_REVISION_RESET rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<int(quality)<<" algorithm="<<int(algorithm)<<" callbacks=6 allocations="<<allocations<<" frees="<<frees<<'\n';
        }
        _CrtSetAllocHook(old);
#else
        CHECK(false);
#endif
        CHECK(accepted&&bounded);
    }
}
void modernReverbFingerprint()
{
    section("modern_reverb_fingerprint");
    // New instances and saved projects replay this kernel; protect its exact float output.
    auto patch=impulsePatch();add(patch,EffectKind::reverb);
    patch.effects[0].algorithmRevision=modernReverbAlgorithmRevision;patch.compatibility.effects[0][reverbEffectBank]=modernReverbAlgorithmRevision;
    patch.effects[0].reverb={.4f,4,.3f,13,.75f};
    // Realtime and higher quality at 44.1, 48 and 96 kHz, from the Task6 kernel on Windows Debug.
    const std::array<std::uint64_t,6> expected{4566920730788388553ull,13040251213137879045ull,13473279118433671066ull,
        14101591089778019939ull,16433755890688262934ull,11487533303018599445ull};
    std::size_t index=0;
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,16384,impulseEvents,64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"MODERN_REVERB_FINGERPRINT rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a,8192)>0&&hash==expected[index++]);
    }
}
// Drive revision 2 is measured through the public engine with clean wavetable
// sines at exact frequencies. Effects run near 1/32 of a voice's output, so one
// full-level voice peaks near .022 at the drive; revision 2's shaper is
// referenced to 1/16, which reaches 0 dBFS at the default master level and gain.
constexpr double modernDriveReference=1.0/16;
RoutedPatch driveStimulus(double hz,float level,int& key)
{
    RoutedPatch p;p.synth.masterLevel=1;
    auto& l=p.synth.layers[0];l.enabled=true;l.engine=EngineKind::wavetable;l.algorithmRevision=2;l.level=level;
    p.compatibility.synth[0][static_cast<std::size_t>(EngineKind::wavetable)]=2;
    const double pitch=69+12*std::log2(hz/440);const int octaves=pitch>127.4?int(std::ceil((pitch-127.4)/12)):0;
    key=std::clamp(int(std::lround(pitch))-12*octaves,0,127);
    l.oscillators[0]={Waveform::sine,0,0,float(12*octaves),float((pitch-12*octaves-key)*100),1.0f,.5f};
    l.oscillators[1].level=0;l.noiseLevel=0;l.cutoffHz=20000;l.resonance=0;l.filterEnvelopeOctaves=0;l.ampEnvelope={0,0,0,1};
    return p;
}
// The stimulus, an optional exact-gain stereo lift, then a drive at the revision.
RoutedPatch driven(RoutedPatch p,const DriveSettings& d,AlgorithmRevision revision,float liftDb=0)
{
    std::size_t slot=0;
    if(liftDb>0){add(p,EffectKind::stereo);p.effects[0].stereo={liftDb,0,1,StereoChannelMode::stereo};slot=1;}
    add(p,EffectKind::drive);p.effects[slot].drive=d;p.effects[slot].algorithmRevision=revision;
    p.compatibility.effects[slot][static_cast<std::size_t>(EffectKind::drive)]=revision;
    return p;
}
std::vector<double> driveRender(const RoutedPatch& p,int key,double rate,int from,int count)
{
    const auto a=sound(p,from+count,{{0,on(key)}},rate);
    return std::vector<double>(a[0].begin()+from,a[0].begin()+from+count);
}
double blackmanHarris(std::size_t n,std::size_t size)
{
    const double t=double(n)/double(size-1),pi=std::numbers::pi;
    return .35875-.48829*std::cos(2*pi*t)+.14128*std::cos(4*pi*t)-.01168*std::cos(6*pi*t);
}
double toneAmplitude(const std::vector<double>& x,double hz,double rate)
{
    std::complex<double> sum;double weight=0;
    for(std::size_t n=0;n<x.size();++n){const double w=blackmanHarris(n,x.size());sum+=x[n]*w*std::polar(1.0,-2*std::numbers::pi*hz*double(n)/rate);weight+=w;}
    return 2*std::abs(sum)/weight;
}
// Layer level that gives the stimulus sine the requested peak at the drive.
float driveLevel(double hz,double target,double rate)
{
    int key=0;double unit=0;const auto stimulus=driveStimulus(hz,1,key);
    for(double v:driveRender(stimulus,key,rate,int(.1*rate),int(.3*rate)))unit=std::max(unit,std::abs(v));
    return float(target/unit);
}
double driveDistortionDb(const std::vector<double>& x,double f0,double rate)
{
    const double fundamental=toneAmplitude(x,f0,rate);double harmonics=0;
    for(int h=2;h*f0<rate/2;++h){const double a=toneAmplitude(x,h*f0,rate);harmonics+=a*a;}
    return 10*std::log10(std::max(harmonics,1e-30)/(fundamental*fundamental));
}
// Inharmonic energy below 16 kHz relative to the total over 2^16 samples:
// aliases that land in the audible band, excluding fold-back near Nyquist.
double driveAliasDb(const std::vector<double>& x,double f0,double rate)
{
    constexpr int order=16,size=1<<order;
    juce::dsp::FFT fft(order);std::vector<float> data(2*std::size_t(size));
    for(std::size_t i=0;i<std::size_t(size);++i)data[i]=float(x[i]*blackmanHarris(i,std::size_t(size)));
    fft.performFrequencyOnlyForwardTransform(data.data(),true);
    const double bin=rate/size;std::vector<bool> harmonic(std::size_t(size/2+1),false);
    for(int h=1;h*f0<rate/2;++h){const double centre=h*f0/bin;const int spread=6+int(std::ceil(centre*.0005));
        for(int k=std::max(0,int(centre)-spread);k<=std::min(size/2,int(centre)+spread);++k)harmonic[std::size_t(k)]=true;}
    double total=0,alias=0;
    for(int k=4;k<=size/2;++k){const double power=double(data[std::size_t(k)])*data[std::size_t(k)];total+=power;if(!harmonic[std::size_t(k)]&&k*bin<=16000)alias+=power;}
    return 10*std::log10(std::max(alias,1e-30)/std::max(total,1e-30));
}
void modernDriveAdmission()
{
    section("modern_drive_admission_and_fresh_instance_default");
    CHECK(supportsEffectAlgorithmRevision(static_cast<std::size_t>(EffectKind::drive),2));
    CHECK(!supportsEffectAlgorithmRevision(static_cast<std::size_t>(EffectKind::drive),3));
    for(const auto& banks:newInstanceKernelCompatibility().effects)CHECK(banks[static_cast<std::size_t>(EffectKind::drive)]==2&&banks[reverbEffectBank]==2);
    int key=0;const auto base=driveStimulus(440,.5f,key);
    for(auto shape:{DriveShape::tanh,DriveShape::softSign})CHECK(validateRoutedPatch(driven(base,{6,shape,12000,.5f},2)));
    auto mismatched=driven(base,{6,DriveShape::tanh,12000,.5f},2);mismatched.compatibility.effects[0][0]=1;CHECK(!validateRoutedPatch(mismatched));
    mismatched=driven(base,{6,DriveShape::tanh,12000,.5f},2);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
    CHECK(!validateRoutedPatch(driven(base,{6,DriveShape::tanh,12000,.5f},3)));
}
void modernDriveResponse()
{
    section("modern_drive_small_signal_response");
    struct Case{const char* name;DriveSettings d;};
    const Case cases[]{{"mix0",{0,DriveShape::tanh,20000,0}},{"mix1_tone20k",{0,DriveShape::tanh,20000,1}},
                       {"mix1_tone6500",{0,DriveShape::tanh,6500,1}},{"cinder_coil",{6,DriveShape::tanh,6500,.25f}}};
    for(double rate:{44100.,48000.})for(double hz:{1000.,5000.,10000.,15000.,18000.}){
        int key=0;const auto stimulus=driveStimulus(hz,driveLevel(hz,.001,rate),key);
        const int from=4096,count=16384;const double reference=toneAmplitude(driveRender(stimulus,key,rate,from,count),hz,rate);
        for(const auto& c:cases){
            const double measured=20*std::log10(toneAmplitude(driveRender(driven(stimulus,c.d,2),key,rate,from,count),hz,rate)/reference);
            const double analog=20*std::log10(std::abs((1.0-c.d.mix)+c.d.mix*std::pow(10.0,c.d.gainDb/20)/std::complex<double>(1,hz/c.d.toneHz)));
            const double tolerance=hz<=15000||c.d.mix==0?.5:1.;
            CHECK(std::abs(measured-analog)<=tolerance);
            std::cout<<"MODERN_DRIVE_RESPONSE rate="<<rate<<" case="<<c.name<<" hz="<<hz<<" measured_db="<<measured<<" analog_db="<<analog<<" error_db="<<measured-analog<<'\n';
        }
    }
}
void modernDriveSaturation()
{
    section("modern_drive_saturation_alias_and_peak");
    const DriveSettings heavy{24,DriveShape::tanh,20000,1};
    for(double rate:{44100.,48000.}){
        const int from=int(.25*rate),count=1<<16;
        int key=0;const auto voice=driveStimulus(1760,1,key);
        const double modern=driveDistortionDb(driveRender(driven(voice,{6,DriveShape::tanh,20000,1},2),key,rate,from,count),1760,rate);
        const double legacy=driveDistortionDb(driveRender(driven(voice,{6,DriveShape::tanh,20000,1},1),key,rate,from,count),1760,rate);
        CHECK(modern>=-35&&modern<=-20);
        std::cout<<"MODERN_DRIVE_DISTORTION rate="<<rate<<" f0=1760 full_voice drive=6 modern_db="<<modern<<" legacy_db="<<legacy<<'\n';
        for(double f0:{1760.,3520.,7040.}){
            const float level=driveLevel(f0,.02,rate);const auto lifted=driveStimulus(f0,level,key);
            const double hard=driveAliasDb(driveRender(driven(lifted,heavy,2,12),key,rate,from,count),f0,rate);
            CHECK(hard<=-50);
            std::cout<<"MODERN_DRIVE_ALIAS rate="<<rate<<" f0="<<f0<<" peak=.02 lift_db=12 drive=24 alias16k_db="<<hard<<'\n';
            if(f0==7040){
                // Revision 2's input 24 dB lower gives its shaper revision 1's signal.
                const double old=driveAliasDb(driveRender(driven(lifted,heavy,1,12),key,rate,from,count),f0,rate);
                const auto quiet=driveStimulus(f0,level/4,key);
                const double matched=driveAliasDb(driveRender(driven(quiet,heavy,2),key,rate,from,count),f0,rate);
                std::cout<<"MODERN_DRIVE_MATCHED_ALIAS rate="<<rate<<" f0="<<f0<<" legacy_alias16k_db="<<old<<" matched_alias16k_db="<<matched<<'\n';
                CHECK(matched<=old-10);
            }
        }
        for(double f0:{55.,220.}){
            const auto low=driveStimulus(f0,driveLevel(f0,.02,rate),key);
            const auto out=driveRender(driven(low,heavy,2,12),key,rate,from,int(rate));
            double peak=0;for(double v:out)peak=std::max(peak,std::abs(v));
            const double overDb=20*std::log10(peak/modernDriveReference);
            CHECK(std::abs(overDb)<=1);
            std::cout<<"MODERN_DRIVE_PEAK rate="<<rate<<" f0="<<f0<<" peak=.02 lift_db=12 drive=24 over_reference_db="<<overDb<<'\n';
        }
    }
}
void modernDrivePartitionResetAndCallbacks()
{
    section("modern_drive_partition_reset_quality_callbacks");
    int key=0;const auto patch=driven(driveStimulus(220,1,key),{18,DriveShape::softSign,7000,.6f},2,6);
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine a,b;CHECK(a.setPatch(patch)&&b.setPatch(patch));CHECK(a.prepare(rate,512,quality)&&b.prepare(rate,512,quality));
        const std::vector<Event> phrase{{17,on(key)},{4001,off(key)}};
        const auto coarse=render(a,8192,phrase,512),fine=render(b,8192,phrase,61);
        CHECK(finite(coarse)&&energy(coarse)>1e-10&&difference(coarse,fine)==0);
        a.reset();CHECK(energy(render(a,2048))==0);
        a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
        std::cout<<"MODERN_DRIVE_STATE rate="<<rate<<" quality="<<int(quality)<<" energy="<<energy(coarse)<<" partition_error="<<difference(coarse,fine)<<'\n';
    }
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){add(busy,EffectKind::drive,slot%5);busy.effects[std::size_t(slot)].algorithmRevision=2;busy.compatibility.effects[std::size_t(slot)][0]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    bool accepted=true;engine.render(audio,held,120);
    for(int block=0;block<48;++block){busy.effects[std::size_t(block%8)].drive.gainDb=float(block%25);accepted&=engine.setPatch(busy);engine.render(audio,empty,120);}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_DRIVE_CALLBACKS layers=4 notes=16 drives=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_DRIVE_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&audio.getMagnitude(0,256)>0);
}
void legacyDriveFingerprint()
{
    section("legacy_drive_fingerprint");
    // Recalled sounds replay drive revision 1; protect its exact float output.
    auto tanhPatch=flat();tanhPatch.synth.layers[0].level=.8f;add(tanhPatch,EffectKind::drive);tanhPatch.effects[0].drive={24,DriveShape::tanh,6500,.6f};
    auto softPatch=tanhPatch;softPatch.effects[0].drive={12,DriveShape::softSign,3000,1};
    // Tanh then soft sign; realtime and higher quality at 44.1, 48 and 96 kHz, from the pre-F12.19 build.
    const std::array<std::uint64_t,12> expected{1967487982301102889ull,8618886097395471589ull,17525267997054802829ull,14545132586926545469ull,
        15167069316862938753ull,11838112099876776277ull,11082641870874130741ull,17087415942726810621ull,14017209359213559329ull,15646567810854802269ull,
        720294369250989689ull,8729916736262110177ull};
    std::size_t index=0;
    for(const auto* patch:{&tanhPatch,&softPatch})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_DRIVE_FINGERPRINT shape="<<int(patch->effects[0].drive.shape)<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
// Compressor revision 2 is measured through the public engine on the drive's
// clean wavetable sines, with thresholds set from the stimulus peak at the
// compressor. Its steady gain reduction follows the hard-knee law at the peak
// level, and its attack and release labels are a level step's 63.2% times.
RoutedPatch compressed(RoutedPatch p,const CompressorSettings& c,AlgorithmRevision revision)
{
    add(p,EffectKind::compressor);p.effects[0].compressor=c;p.effects[0].algorithmRevision=revision;
    p.compatibility.effects[0][static_cast<std::size_t>(EffectKind::compressor)]=revision;
    return p;
}
std::vector<double> compressorRender(const RoutedPatch& p,const std::vector<Event>& events,double rate,RenderQuality quality,int count)
{
    RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));
    const auto a=render(e,count,events,512);
    return std::vector<double>(a[0].begin(),a[0].end());
}
double compressorLawDb(double overDb,double ratio){return std::max(0.0,overDb)*(1-1/ratio);}
// Gain reduction in dB where the dry signal is at least half its largest
// magnitude within one period either side; NaN elsewhere.
std::vector<double> reductionTrack(const std::vector<double>& dry,const std::vector<double>& wet,std::size_t period)
{
    std::vector<double> r(dry.size(),std::numeric_limits<double>::quiet_NaN());
    for(std::size_t n=0;n<dry.size();++n){
        double local=0;
        for(std::size_t m=n>period?n-period:0;m<std::min(dry.size(),n+period+1);++m)local=std::max(local,std::abs(dry[m]));
        if(local>0&&std::abs(dry[n])>=.5*local&&wet[n]/dry[n]>0)r[n]=-20*std::log10(wet[n]/dry[n]);
    }
    return r;
}
// Milliseconds after a step at sample start (reduction initial) until the
// track first crosses goal, interpolated between valid samples; sample n has
// seen n-start+1 samples of the new level.
double crossingMs(const std::vector<double>& r,std::size_t start,double initial,double goal,double rate)
{
    const bool rising=goal>initial;double time=0,value=initial;
    for(std::size_t n=start;n<r.size();++n){
        if(std::isnan(r[n]))continue;
        const double t=double(n-start+1)/rate;
        if(rising?r[n]>=goal:r[n]<=goal)return 1000*(time+(goal-value)/(r[n]-value)*(t-time));
        time=t;value=r[n];
    }
    return std::numeric_limits<double>::infinity();
}
double compressorDistortionDb(const std::vector<double>& x,double f0,double rate)
{
    const double fundamental=toneAmplitude(x,f0,rate);double harmonics=0;
    for(int h=2;h<=16&&h*f0<rate/2;++h){const double a=toneAmplitude(x,h*f0,rate);harmonics+=a*a;}
    return 10*std::log10(std::max(harmonics,1e-30)/(fundamental*fundamental));
}
void modernCompressorAdmission()
{
    section("modern_compressor_admission_and_fresh_instance_default");
    constexpr auto bank=static_cast<std::size_t>(EffectKind::compressor);
    CHECK(supportsEffectAlgorithmRevision(bank,2));CHECK(!supportsEffectAlgorithmRevision(bank,3));
    for(const auto& banks:newInstanceKernelCompatibility().effects)
        CHECK(banks[bank]==2&&banks[static_cast<std::size_t>(EffectKind::drive)]==2&&banks[reverbEffectBank]==2);
    int key=0;const auto base=driveStimulus(440,.5f,key);const CompressorSettings settings{-30,4,10,100,0,1};
    CHECK(validateRoutedPatch(compressed(base,settings,2)));
    auto mismatched=compressed(base,settings,2);mismatched.compatibility.effects[0][bank]=1;CHECK(!validateRoutedPatch(mismatched));
    mismatched=compressed(base,settings,2);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
    CHECK(!validateRoutedPatch(compressed(base,settings,3)));
}
void modernCompressorBallistics()
{
    section("modern_compressor_label_ballistics");
    // A 1 kHz sine 12 dB over the threshold starts from silence (attack), then a
    // retrigger at velocity .2 drops it 14 dB, below the threshold (release).
    const double peakLevel=.02,overDb=12;
    struct Case{const char* name;float ratio,attackMs,releaseMs;};
    const Case cases[]{{"default",4,10,100},{"anchor_press",3,15,150},{"attack_over_release",4,50,20},{"slowest",4,100,2000}};
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        int key=0;const auto stimulus=driveStimulus(1000,driveLevel(1000,peakLevel,rate),key);
        const auto period=static_cast<std::size_t>(std::ceil(rate/1000));
        for(const auto& c:cases){
            const int step=int(rate*std::max(.5,.012*c.attackMs)),count=step+int(rate*(.002*c.releaseMs+.05));
            const std::vector<Event> events{{0,on(key)},{step,off(key)},{step+1,on(key,1,.2f)}};
            const CompressorSettings settings{float(20*std::log10(peakLevel)-overDb),c.ratio,c.attackMs,c.releaseMs,0,1};
            const auto dry=compressorRender(stimulus,events,rate,quality,count);
            // Times run from the stimulus's arrival at half its peak, so the higher
            // quality's resampling latency is not counted against the labels.
            std::size_t arrival=0;
            {double top=0;for(int n=0;n<step;++n)top=std::max(top,std::abs(dry[std::size_t(n)]));while(std::abs(dry[arrival])<.5*top)++arrival;}
            for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
                if(revision==1&&(rate!=48000||quality!=RenderQuality::realtime))continue;
                const auto track=reductionTrack(dry,compressorRender(compressed(stimulus,settings,revision),events,rate,quality,count),period);
                double settled=0;int samples=0;
                for(int n=step-int(.05*rate);n<step;++n)if(!std::isnan(track[std::size_t(n)])){settled+=track[std::size_t(n)];++samples;}
                settled/=std::max(1,samples);
                const double attack=crossingMs(track,arrival,0,(1-std::exp(-1.))*settled,rate)/c.attackMs;
                const double release=crossingMs(track,std::size_t(step+1)+arrival,settled,std::exp(-1.)*settled,rate)/c.releaseMs;
                if(revision==2)CHECK(std::abs(attack-1)<=.1&&std::abs(release-1)<=.1&&std::abs(settled-compressorLawDb(overDb,c.ratio))<=.2);
                else CHECK(attack<.5&&release<.5);
                std::cout<<"MODERN_COMPRESSOR_BALLISTICS revision="<<int(revision)<<" rate="<<rate<<" quality="<<int(quality)<<" case="<<c.name
                         <<" arrival_samples="<<arrival<<" attack_over_label="<<attack<<" release_over_label="<<release<<" settled_db="<<settled<<" law_db="<<compressorLawDb(overDb,c.ratio)<<'\n';
            }
        }
    }
}
void modernCompressorStaticCurveAndDistortion()
{
    section("modern_compressor_static_curve_and_low_frequency_distortion");
    constexpr double rate=48000;const double peakLevel=.02;
    int key=0;const auto tone=driveStimulus(1000,driveLevel(1000,peakLevel,rate),key);
    const auto window=[&](const RoutedPatch& p,int from,int count){
        const auto x=compressorRender(p,{{0,on(key)}},rate,RenderQuality::realtime,from+count);return std::vector<double>(x.begin()+from,x.end());};
    const double reference=toneAmplitude(window(tone,int(.5*rate),16384),1000,rate);
    for(const float ratio:{1.5f,4.f,20.f})for(const double over:{-6.,0.,6.,12.,24.})for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
        if(revision==1&&ratio!=4)continue;
        const CompressorSettings settings{float(20*std::log10(peakLevel)-over),ratio,10,100,0,1};
        const double reduction=20*std::log10(reference/toneAmplitude(window(compressed(tone,settings,revision),int(.5*rate),16384),1000,rate));
        const double error=reduction-compressorLawDb(over,ratio);
        if(revision==2)CHECK(std::abs(error)<=.1);
        std::cout<<"MODERN_COMPRESSOR_STATIC revision="<<int(revision)<<" f0=1000 ratio="<<ratio<<" over_db="<<over<<" reduction_db="<<reduction<<" law_error_db="<<error<<'\n';
    }
    struct Case{const char* name;float ratio,attackMs,releaseMs;};
    for(const double f0:{55.,110.}){
        const auto low=driveStimulus(f0,driveLevel(f0,peakLevel,rate),key);
        const int from=int(2*rate),size=1<<16;const double fundamental=toneAmplitude(window(low,from,size),f0,rate);
        for(const auto& c:{Case{"anchor_press",3,15,150},Case{"default",4,10,100}}){
            const CompressorSettings settings{float(20*std::log10(peakLevel)-12),c.ratio,c.attackMs,c.releaseMs,0,1};
            const auto modern=window(compressed(low,settings,2),from,size);
            const double modernDb=compressorDistortionDb(modern,f0,rate),legacyDb=compressorDistortionDb(window(compressed(low,settings,1),from,size),f0,rate);
            const double error=20*std::log10(fundamental/toneAmplitude(modern,f0,rate))-compressorLawDb(12,c.ratio);
            CHECK(modernDb<=legacyDb-10&&modernDb<=-50&&std::abs(error)<=.3);
            std::cout<<"MODERN_COMPRESSOR_LOW_FREQUENCY f0="<<f0<<" case="<<c.name<<" modern_thd_db="<<modernDb<<" legacy_thd_db="<<legacyDb<<" modern_law_error_db="<<error<<'\n';
        }
    }
}
void modernCompressorLinkMakeupMixBypass()
{
    section("modern_compressor_link_makeup_mix_bypass_tail");
    constexpr double rate=48000;int key=0;
    auto stimulus=driveStimulus(330,driveLevel(330,.02,rate),key);stimulus.synth.layers[0].pan=.4f;
    const CompressorSettings settings{float(20*std::log10(.02)-12),4,5,80,0,1};
    const int count=int(.5*rate),stop=int(.4*rate);const std::vector<Event> events{{0,on(key)},{stop,off(key)}};
    const auto stereo=[&](const RoutedPatch& p,const std::vector<Event>& e){RoutedSynthEngine engine;CHECK(engine.setPatch(p)&&engine.prepare(rate,512));return render(engine,count,e,512);};
    const auto dry=stereo(stimulus,events),wet=stereo(compressed(stimulus,settings,2),events);
    double linkError=0;
    for(std::size_t i=0;i<dry[0].size();++i)if(std::abs(dry[0][i])>1e-4f&&std::abs(dry[1][i])>1e-4f)
        linkError=std::max(linkError,std::abs(double(wet[0][i])/dry[0][i]-double(wet[1][i])/dry[1][i]));
    CHECK(linkError<1e-5&&energy(wet,stop/2,stop)<energy(dry,stop/2,stop)*.5);
    const auto scaledError=[](const Audio& a,const Audio& b,double scale,const Audio* plus=nullptr){
        double e=0;for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<a[ch].size();++i)
            e=std::max(e,std::abs(double(a[ch][i])-(scale*b[ch][i]+(plus?(1-scale)*(*plus)[ch][i]:0.0))));
        return e;};
    auto lifted=settings;lifted.makeupDb=6;
    const double makeupError=scaledError(stereo(compressed(stimulus,lifted,2),events),wet,std::pow(10.,6./20));
    auto half=settings;half.mix=.5f;
    const double mixError=scaledError(stereo(compressed(stimulus,half,2),events),wet,.5,&dry);
    CHECK(makeupError<1e-7&&mixError<1e-7);
    auto bypassed=compressed(stimulus,settings,2);bypassed.effects[0].bypassed=true;CHECK(difference(stereo(bypassed,events),dry)==0);
    auto unity=settings;unity.ratio=1;CHECK(difference(stereo(compressed(stimulus,unity,2),events),dry)==0);
    auto above=settings;above.thresholdDb=0;CHECK(difference(stereo(compressed(stimulus,above,2),events),dry)==0);
    CHECK(energy(dry,stop+256,count)==0&&energy(wet,stop+256,count)==0);
    CHECK(energy(stereo(compressed(stimulus,settings,2),{}))==0&&finite(wet));
    std::cout<<"MODERN_COMPRESSOR_SIGNAL link_error="<<linkError<<" makeup_error="<<makeupError<<" mix_error="<<mixError
             <<" wet_over_dry_db="<<10*std::log10(energy(wet,stop/2,stop)/energy(dry,stop/2,stop))<<'\n';
}
void modernCompressorPartitionResetAndCallbacks()
{
    section("modern_compressor_partition_reset_quality_callbacks");
    int key=0;const auto patch=compressed(driveStimulus(220,1,key),{-45,6,5,80,3,.8f},2);
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine a,b;CHECK(a.setPatch(patch)&&b.setPatch(patch));CHECK(a.prepare(rate,512,quality)&&b.prepare(rate,512,quality));
        const std::vector<Event> phrase{{17,on(key)},{4001,off(key)},{4400,on(key,1,.5f)},{7000,off(key)}};
        const auto coarse=render(a,8192,phrase,512),fine=render(b,8192,phrase,61);
        CHECK(finite(coarse)&&energy(coarse)>1e-10&&difference(coarse,fine)==0);
        a.reset();CHECK(energy(render(a,2048))==0);
        a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
        std::cout<<"MODERN_COMPRESSOR_STATE rate="<<rate<<" quality="<<int(quality)<<" energy="<<energy(coarse)<<" partition_error="<<difference(coarse,fine)<<'\n';
    }
    constexpr auto bank=static_cast<std::size_t>(EffectKind::compressor);
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){add(busy,EffectKind::compressor,slot%5);auto& e=busy.effects[std::size_t(slot)];
        e.algorithmRevision=2;e.compressor.thresholdDb=-40;busy.compatibility.effects[std::size_t(slot)][bank]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    bool accepted=true;engine.render(audio,held,120);
    for(int block=0;block<48;++block){auto& c=busy.effects[std::size_t(block%8)].compressor;
        c.thresholdDb=float(-60+block%50);c.attackMs=float(.1+block%10);c.releaseMs=float(10+40*(block%7));
        accepted&=engine.setPatch(busy);engine.render(audio,empty,120);}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_COMPRESSOR_CALLBACKS layers=4 notes=16 compressors=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_COMPRESSOR_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&audio.getMagnitude(0,256)>0);
}
void legacyCompressorFingerprint()
{
    section("legacy_compressor_fingerprint");
    // Recalled sounds replay compressor revision 1; protect its exact float output.
    auto fast=flat();fast.synth.layers[0].level=.8f;add(fast,EffectKind::compressor);fast.effects[0].compressor={-40,8,1,50,6,1};
    auto slow=fast;slow.effects[0].compressor={-30,2,30,500,0,.5f};
    // Fast then slow; realtime and higher quality at 44.1, 48 and 96 kHz, from the pre-F12.21 build.
    const std::array<std::uint64_t,12> expected{9610832556691144141ull,16138785664159386753ull,1494244022292220829ull,17674769376587059989ull,
        3400026688387417925ull,8020861601683606613ull,6706442207602541393ull,12471642634429790633ull,15226103658657012281ull,11730659596543116337ull,
        6273359746623270929ull,206940111177946029ull};
    std::size_t index=0;
    for(const auto* patch:{&fast,&slow})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_COMPRESSOR_FINGERPRINT ratio="<<patch->effects[0].compressor.ratio<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
// F12.23 modulated taps. Probes follow the F12.18 audit: steady sines through
// one effect at the audit's settings, wet only, feedback 0.
constexpr std::array<EffectKind,3> modulatedKinds{EffectKind::chorus,EffectKind::flanger,EffectKind::dimension};
template<typename Configure>
RoutedPatch modulated(RoutedPatch p,EffectKind kind,AlgorithmRevision revision,Configure&& configure)
{
    add(p,kind);configure(p.effects[0]);p.effects[0].algorithmRevision=revision;
    p.compatibility.effects[0][static_cast<std::size_t>(kind)]=revision;
    return p;
}
void auditSettings(EffectSettings& e,EffectKind kind)
{
    if(kind==EffectKind::chorus)e.chorus={.35f,.25f,18,0,1};
    else if(kind==EffectKind::flanger)e.flanger={.25f,.6f,2,0,1};
    else e.dimension={.6f,.35f,1,.5f};
}
constexpr int sideChannel=2;
std::vector<double> modulatedRender(const RoutedPatch& p,int key,double rate,RenderQuality quality,int from,int count,int channel=0)
{
    RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));
    const auto a=render(e,from+count,{{0,on(key)}},512);
    std::vector<double> x(static_cast<std::size_t>(count));
    for(std::size_t n=0;n<x.size();++n){const auto i=std::size_t(from)+n;
        x[n]=channel==sideChannel?.5*(double(a[0][i])-a[1][i]):double(a[std::size_t(channel)][i]);}
    return x;
}
// Largest over smallest instantaneous amplitude of the band f0 +- 2 kHz across
// the central 80% of 2^17 samples (analytic signal of a Tukey-tapered window).
double tapFlutterDb(const std::vector<double>& x,double f0,double rate)
{
    constexpr int order=17,size=1<<order;
    juce::dsp::FFT fft(order);std::vector<juce::dsp::Complex<float>> wave(static_cast<std::size_t>(size)),spectrum(wave.size());
    const int taper=size/20;
    for(int i=0;i<size;++i){const int edge=std::min(i,size-1-i);const double w=edge<taper?.5-.5*std::cos(std::numbers::pi*edge/taper):1.;
        wave[std::size_t(i)]={float(x[std::size_t(i)]*w),0.f};}
    fft.perform(wave.data(),spectrum.data(),false);
    const double bin=rate/size;const int low=std::max(1,int((f0-2000)/bin)),high=std::min(size/2-1,int((f0+2000)/bin));
    for(int k=0;k<size;++k)if(k<low||k>high)spectrum[std::size_t(k)]={};
    fft.perform(spectrum.data(),wave.data(),true);
    double largest=0,smallest=std::numeric_limits<double>::max();
    for(int i=size/10;i<size-size/10;++i){const double m=std::abs(wave[std::size_t(i)]);largest=std::max(largest,m);smallest=std::min(smallest,m);}
    return largest>0?20*std::log10(largest/std::max(smallest,1e-30)):std::numeric_limits<double>::infinity();
}
// Energy outside f0 +- 150 Hz over the total in 2^16 samples (Blackman-Harris);
// the dimension sweep's own sidebands stay inside.
double tapProductsDb(const std::vector<double>& x,double f0,double rate)
{
    constexpr int order=16,size=1<<order;
    juce::dsp::FFT fft(order);std::vector<float> data(2*std::size_t(size));
    for(std::size_t i=0;i<std::size_t(size);++i)data[i]=float(x[i]*blackmanHarris(i,std::size_t(size)));
    fft.performFrequencyOnlyForwardTransform(data.data(),true);
    const double bin=rate/size;double total=0,inside=0;
    for(int k=4;k<=size/2;++k){const double power=double(data[std::size_t(k)])*data[std::size_t(k)];total+=power;if(std::abs(k*bin-f0)<=150)inside+=power;}
    return 10*std::log10(std::max(total-inside,1e-30)/std::max(total,1e-30));
}
void modernModulatedTapsAdmission()
{
    section("modern_modulated_taps_admission_and_fresh_instance_default");
    for(std::size_t bank=0;bank<effectBankCount;++bank){
        CHECK(supportsEffectAlgorithmRevision(bank,2)==(bank<=5||bank==6||bank==8||bank==10||bank==13));
        CHECK(!supportsEffectAlgorithmRevision(bank,3));}
    for(const auto& banks:newInstanceKernelCompatibility().effects)for(auto kind:modulatedKinds)CHECK(banks[static_cast<std::size_t>(kind)]==2);
    int key=0;const auto base=driveStimulus(440,.5f,key);
    for(auto kind:modulatedKinds){
        const auto bank=static_cast<std::size_t>(kind);const auto audit=[kind](EffectSettings& e){auditSettings(e,kind);};
        CHECK(validateRoutedPatch(modulated(base,kind,2,audit)));
        auto mismatched=modulated(base,kind,2,audit);mismatched.compatibility.effects[0][bank]=1;CHECK(!validateRoutedPatch(mismatched));
        mismatched=modulated(base,kind,2,audit);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
        CHECK(!validateRoutedPatch(modulated(base,kind,3,audit)));
    }
}
void modernModulatedTapsFixedTap()
{
    section("modern_modulated_taps_fixed_fractional_tap_level");
    // Depth 0 holds the tap still about half a sample off the grid at 44.1 kHz:
    // revision 1's linear read cuts the treble there, revision 2 keeps the level.
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(double f0:{10000.,15000.}){
        int key=0;const auto tone=driveStimulus(f0,1,key);const int from=int(.3*rate),count=1<<15;
        const double dry=toneAmplitude(modulatedRender(tone,key,rate,quality,from,count),f0,rate);
        for(auto kind:{EffectKind::chorus,EffectKind::flanger})for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
            if(revision==1&&(rate!=44100||quality!=RenderQuality::realtime))continue;
            const auto patch=modulated(tone,kind,revision,[kind](EffectSettings& e){
                if(kind==EffectKind::chorus)e.chorus={.35f,0,12.0068f,0,1};else e.flanger={.25f,0,2.0068f,0,1};});
            const double levelDb=20*std::log10(toneAmplitude(modulatedRender(patch,key,rate,quality,from,count),f0,rate)/dry);
            CHECK(revision==2?std::abs(levelDb)<=.05:levelDb<-1);
            std::cout<<"MODERN_TAPS_FIXED revision="<<int(revision)<<" rate="<<rate<<" quality="<<int(quality)<<" kind="<<int(kind)<<" f0="<<f0<<" level_db="<<levelDb<<'\n';
        }
    }
}
void modernModulatedTapsFlutterAndProducts()
{
    section("modern_modulated_taps_flutter_and_products");
    // Targets: revision 1's 44.1 kHz higher-quality flutter, and dimension's
    // products 12 dB under its 44.1 kHz realtime figures.
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(double f0:{10000.,15000.}){
        int key=0;const auto tone=driveStimulus(f0,1,key);const int from=int(.3*rate);
        const double flutterLimit=f0==10000?.57:1.31,productLimit=(f0==10000?-25.5:-21.5)-12;
        for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
            if(revision==1&&(rate!=44100||quality!=RenderQuality::realtime))continue;
            const auto through=[&](EffectKind kind){return modulated(tone,kind,revision,[kind](EffectSettings& e){auditSettings(e,kind);});};
            const double chorus=tapFlutterDb(modulatedRender(through(EffectKind::chorus),key,rate,quality,from,1<<17),f0,rate);
            const double flanger=tapFlutterDb(modulatedRender(through(EffectKind::flanger),key,rate,quality,from,1<<17),f0,rate);
            const double products=tapProductsDb(modulatedRender(through(EffectKind::dimension),key,rate,quality,from,1<<16,sideChannel),f0,rate);
            if(revision==2)CHECK(chorus<=flutterLimit&&flanger<=flutterLimit&&products<=productLimit);
            else CHECK(chorus>flutterLimit&&flanger>flutterLimit&&products>productLimit);
            std::cout<<"MODERN_TAPS_FLUTTER revision="<<int(revision)<<" rate="<<rate<<" quality="<<int(quality)<<" f0="<<f0
                     <<" chorus_db="<<chorus<<" flanger_db="<<flanger<<" dimension_products_db="<<products<<'\n';
        }
    }
}
void modernModulatedTapsKeepLaw()
{
    section("modern_modulated_taps_keep_revision1_law");
    // At 200 Hz both reads are accurate, so revision 2 follows revision 1 within
    // linear interpolation's error through feedback, mix and a live edit; a
    // different delay law would move the output far more. The attack keeps the
    // onset narrow-band. Whole-sample taps read exactly in both revisions.
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        int key=0;auto tone=driveStimulus(200,1,key);tone.synth.layers[0].ampEnvelope.attack=.02f;const int count=int(.6*rate),edit=int(.25*rate);
        for(auto kind:modulatedKinds){
            const auto configure=[kind](EffectSettings& e,bool later){
                if(kind==EffectKind::chorus)e.chorus=later?ChorusSettings{2.5f,.9f,31,-.6f,.7f}:ChorusSettings{.8f,.6f,15,.5f,.8f};
                else if(kind==EffectKind::flanger)e.flanger=later?FlangerSettings{3,1,1.5f,-.7f,.6f}:FlangerSettings{.6f,.8f,4,.6f,.9f};
                else e.dimension=later?DimensionSettings{1,1.5f,2,1}:DimensionSettings{.7f,.4f,1.3f,.6f};};
            const auto run=[&](AlgorithmRevision revision){
                auto patch=modulated(tone,kind,revision,[&](EffectSettings& e){configure(e,false);});
                RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));
                auto output=render(engine,edit,{{0,on(key)}},173);
                configure(patch.effects[0],true);CHECK(engine.setPatch(patch));const auto rest=render(engine,count-edit,{},173);
                for(std::size_t ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),rest[ch].begin(),rest[ch].end());
                return output;};
            const auto modern=run(2),legacy=run(1);const double error=difference(modern,legacy)/std::max(peak(legacy),1e-30);
            CHECK(finite(modern)&&peak(modern)>1e-4&&error<=1e-3);
            std::cout<<"MODERN_TAPS_LAW rate="<<rate<<" quality="<<int(quality)<<" kind="<<int(kind)<<" relative_error="<<error<<'\n';
        }
    }
    for(double rate:{48000.,96000.})for(float feedback:{-.6f,.6f}){
        auto noise=impulsePatch();const auto whole=[&](EffectKind kind,AlgorithmRevision revision){
            return modulated(noise,kind,revision,[&](EffectSettings& e){
                if(kind==EffectKind::chorus)e.chorus={.35f,0,20,feedback,.7f};else e.flanger={.25f,0,2,feedback,.7f};});};
        for(auto kind:{EffectKind::chorus,EffectKind::flanger}){
            const auto modern=sound(whole(kind,2),4096,impulseEvents,rate,64),legacy=sound(whole(kind,1),4096,impulseEvents,rate,64);
            const double error=difference(modern,legacy);
            CHECK(peak(modern)>1e-6&&error<=1e-6*peak(legacy));
            std::cout<<"MODERN_TAPS_WHOLE_SAMPLE rate="<<rate<<" kind="<<int(kind)<<" feedback="<<feedback<<" error="<<error<<'\n';
        }
    }
}
void modernModulatedTapsEngine()
{
    section("modern_modulated_taps_partition_reset_bypass_tail_callbacks");
    for(auto kind:modulatedKinds){
        auto patch=modulated(impulsePatch(),kind,2,[kind](EffectSettings& e){auditSettings(e,kind);
            if(kind==EffectKind::chorus)e.chorus.feedback=.7f;else if(kind==EffectKind::flanger)e.flanger={2,1,1,-.8f,.8f};});
        const auto dry=impulsePatch();
        for(double rate:{8000.,44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
            const std::vector<Event> phrase{{17,on()},{18,off()},{4400,on(60)},{4401,off(60)}};
            const auto capture=[&](const RoutedPatch& p,int partition){RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));return render(e,8192,phrase,partition);};
            RoutedSynthEngine a;CHECK(a.setPatch(patch)&&a.prepare(rate,512,quality));
            const auto coarse=render(a,8192,phrase,512),fine=capture(patch,61);
            CHECK(finite(coarse)&&energy(coarse)>1e-12&&peak(coarse)<4&&difference(coarse,fine)==0);
            a.reset();CHECK(energy(render(a,2048))==0);
            a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
            const auto input=capture(dry,512);
            auto bypassed=patch;bypassed.effects[0].bypassed=true;CHECK(difference(capture(bypassed,512),input)==0);
            auto zero=patch;if(kind==EffectKind::chorus)zero.effects[0].chorus.mix=0;else if(kind==EffectKind::flanger)zero.effects[0].flanger.mix=0;else zero.effects[0].dimension.mix=0;
            CHECK(difference(capture(zero,512),input)==0);
            std::cout<<"MODERN_TAPS_STATE rate="<<rate<<" quality="<<int(quality)<<" kind="<<int(kind)<<" energy="<<energy(coarse)<<" peak="<<peak(coarse)<<'\n';
        }
    }
    // The longest feedback tails at the lowest rate, where each pass's eight
    // extra samples of kernel support weigh most.
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(auto kind:modulatedKinds){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);
        auto patch=modulated(impulsePatch(),kind,2,[kind](EffectSettings& e){
            if(kind==EffectKind::chorus)e.chorus={.35f,1,40,.8f,1};else if(kind==EffectKind::flanger)e.flanger={.05f,1,8,.8f,1};else e.dimension={1,.05f,2,1};});
        const auto& e=patch.effects[0];
        const double seconds=kind==EffectKind::chorus?detail::chorusTailSeconds(e.chorus,internal,2):kind==EffectKind::flanger?detail::flangerTailSeconds(e.flanger,internal,2)
            :detail::dimensionTailSeconds(e.dimension,internal,2);
        CHECK(std::isfinite(seconds)&&seconds>0&&seconds<60);if(!std::isfinite(seconds)||seconds<=0||seconds>=60)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+800,impulseEvents,73);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[std::size_t(n)])));
        CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6);
        std::cout<<"MODERN_TAPS_TAIL quality="<<int(quality)<<" kind="<<int(kind)<<" seconds="<<seconds<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){const auto kind=modulatedKinds[std::size_t(slot%3)];add(busy,kind,slot%5);auto& e=busy.effects[std::size_t(slot)];
        auditSettings(e,kind);e.algorithmRevision=2;busy.compatibility.effects[std::size_t(slot)][static_cast<std::size_t>(kind)]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256,RenderQuality::higher);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    // Wet-only taps delay the first output past a single callback; take the loudest.
    bool accepted=true;float loudest=0;engine.render(audio,held,120);
    for(int block=0;block<48;++block){auto& e=busy.effects[std::size_t(block%8)];
        e.chorus={block%2?.1f:5.f,block%2?0.f:1.f,block%2?12.f:40.f,block%2?-.8f:.8f,block%2?.1f:1.f};
        e.flanger={block%2?.05f:5.f,block%2?0.f:1.f,block%2?1.f:8.f,block%2?-.8f:.8f,block%2?.1f:1.f};
        e.dimension={block%2?0.f:1.f,block%2?.05f:2.f,block%2?0.f:2.f,block%2?.1f:1.f};
        accepted&=engine.setPatch(busy);engine.render(audio,empty,120);loudest=std::max(loudest,audio.getMagnitude(0,256));if(block%17==0)engine.reset();}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_TAPS_CALLBACKS layers=4 notes=16 effects=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_TAPS_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&loudest>0);
}
void legacyModulatedTapsFingerprint()
{
    section("legacy_modulated_taps_fingerprint");
    // Recalled sounds replay revision 1's linear taps; protect its exact float output.
    auto chorus=flat();chorus.synth.layers[0].level=.8f;add(chorus,EffectKind::chorus);chorus.effects[0].chorus={1.3f,.7f,14,.4f,.6f};
    auto flanger=flat();flanger.synth.layers[0].level=.8f;add(flanger,EffectKind::flanger);flanger.effects[0].flanger={.9f,.8f,3,-.5f,.7f};
    auto dimension=flat();dimension.synth.layers[0].level=.8f;add(dimension,EffectKind::dimension);dimension.effects[0].dimension={.8f,.6f,1.4f,.7f};
    // Chorus, flanger then dimension; realtime and higher quality at 44.1 and 96 kHz, from the pre-F12.23 build.
    const std::array<std::uint64_t,12> expected{15293937218295352813ull,1532470833359306257ull,2452767860477874541ull,9321004266085989333ull,
        11681473165328669065ull,7395270378933342393ull,12812879323455566357ull,9370692885695943501ull,
        9280128663900666253ull,13789178171142556189ull,7401589102499407784ull,6588616280368900628ull};
    std::size_t index=0;
    for(const auto* patch:{&chorus,&flanger,&dimension})for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_MODULATED_TAPS_FINGERPRINT kind="<<int(patch->layout.effects[0].kind)<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
// F12.25 delay. Probes follow the F12.18 audit: steady sines through one
// wet-only delay with feedback 0.
RoutedPatch delayed(RoutedPatch p,const DelaySettings& d,AlgorithmRevision revision)
{
    add(p,EffectKind::delay);p.effects[0].delay=d;p.effects[0].algorithmRevision=revision;
    p.compatibility.effects[0][delayEffectBank]=revision;
    return p;
}
// Channel 0 from `from` while the free time rises by `slope` seconds per
// second, retargeted every 64 samples so the read fraction sweeps steadily.
std::vector<double> glideRender(RoutedPatch p,int key,double rate,RenderQuality quality,int from,int count,double slope)
{
    RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));
    const double start=p.effects[0].delay.seconds;bool accepted=true;
    std::vector<double> x;x.reserve(std::size_t(count));juce::AudioBuffer<float> audio(2,64);
    for(int offset=0;offset<from+count;offset+=64){
        juce::MidiBuffer midi;if(offset==0)midi.addEvent(on(key),0);
        p.effects[0].delay.seconds=float(start+slope*offset/rate);accepted&=e.setPatch(p);e.render(audio,midi,std::nullopt);
        for(int i=0;i<64;++i)if(offset+i>=from&&offset+i<from+count)x.push_back(audio.getSample(0,i));
    }
    CHECK(accepted&&x.size()==std::size_t(count));
    return x;
}
void modernDelayAdmission()
{
    section("modern_delay_admission_and_fresh_instance_default");
    CHECK(supportsEffectAlgorithmRevision(delayEffectBank,modernDelayAlgorithmRevision)&&!supportsEffectAlgorithmRevision(delayEffectBank,3));
    for(const auto& banks:newInstanceKernelCompatibility().effects)CHECK(banks[delayEffectBank]==modernDelayAlgorithmRevision);
    const DelaySettings echo{false,.0137f,.5f,.4f,.3f,9000,.5f};
    CHECK(validateRoutedPatch(delayed(impulsePatch(),echo,2)));
    auto mismatched=delayed(impulsePatch(),echo,2);mismatched.compatibility.effects[0][delayEffectBank]=1;CHECK(!validateRoutedPatch(mismatched));
    mismatched=delayed(impulsePatch(),echo,2);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
    CHECK(!validateRoutedPatch(delayed(impulsePatch(),echo,3)));
}
void modernDelayFractionalRepeat()
{
    section("modern_delay_fractional_repeat_level");
    // One repeat half a sample off the internal grid against the whole length
    // below it: revision 1's linear read cuts the treble there, revision 2 keeps
    // it. The 20 kHz repeat filter is common to both lengths.
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(double f0:{5000.,10000.,15000.,18000.}){
        const double internal=rate*(quality==RenderQuality::higher?2:1),limit=f0==18000?1:.1;
        int key=0;const auto tone=driveStimulus(f0,1,key);const int from=int(.3*rate),count=1<<15;
        for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
            if(revision==1&&(rate!=44100||quality!=RenderQuality::realtime))continue;
            const auto level=[&](double samples){
                return toneAmplitude(modulatedRender(delayed(tone,{false,float(samples/internal),.5f,0,0,20000,1},revision),key,rate,quality,from,count),f0,rate);};
            const double lossDb=20*std::log10(level(441.5)/level(441));
            CHECK(revision==2?std::abs(lossDb)<=limit:lossDb<-limit);
            std::cout<<"MODERN_DELAY_REPEAT revision="<<int(revision)<<" rate="<<rate<<" quality="<<int(quality)<<" f0="<<f0<<" loss_db="<<lossDb<<'\n';
        }
    }
}
void modernDelayGlide()
{
    section("modern_delay_glide_flutter");
    // The time rises 2 ms per second, so the read fraction sweeps the grid at
    // 88 Hz at 44.1 kHz realtime and linear reads make the treble flutter.
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(double f0:{10000.,15000.}){
        int key=0;const auto tone=driveStimulus(f0,1,key);const int from=int(.3*rate);
        for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
            if(revision==1&&(rate!=44100||quality!=RenderQuality::realtime))continue;
            const double flutter=tapFlutterDb(glideRender(delayed(tone,{false,.01f,.5f,0,0,20000,1},revision),key,rate,quality,from,1<<17,.002),f0,rate);
            CHECK(revision==2?flutter<=.1:flutter>1);
            std::cout<<"MODERN_DELAY_GLIDE revision="<<int(revision)<<" rate="<<rate<<" quality="<<int(quality)<<" f0="<<f0<<" flutter_db="<<flutter<<'\n';
        }
    }
}
void modernDelayKeepLaw()
{
    section("modern_delay_keep_revision1_law");
    // At 200 Hz both reads are accurate, so revision 2 follows revision 1 within
    // linear interpolation's error through feedback, crossfeed, the repeat
    // filter, mix and a live time edit.
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        int key=0;auto tone=driveStimulus(200,1,key);tone.synth.layers[0].ampEnvelope.attack=.02f;const int count=int(.8*rate),edit=int(.3*rate);
        const auto run=[&](AlgorithmRevision revision){
            auto patch=delayed(tone,{false,.0137f,.5f,.6f,.4f,6000,.7f},revision);
            RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));
            auto output=render(engine,edit,{{0,on(key)}},173);
            patch.effects[0].delay={false,.0163f,.5f,.45f,.9f,3000,.4f};CHECK(engine.setPatch(patch));const auto rest=render(engine,count-edit,{},173);
            for(std::size_t ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),rest[ch].begin(),rest[ch].end());
            return output;};
        const auto modern=run(2),legacy=run(1);const double error=difference(modern,legacy)/std::max(peak(legacy),1e-30);
        CHECK(finite(modern)&&peak(modern)>1e-4&&error<=1e-3);
        std::cout<<"MODERN_DELAY_LAW rate="<<rate<<" quality="<<int(quality)<<" relative_error="<<error<<'\n';
    }
    // Whole-sample lengths read exactly in both revisions, free and synced.
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(bool synced:{false,true}){
        const DelaySettings d{synced,.25f,.5f,.6f,.4f,7000,.7f};
        const auto capture=[&](AlgorithmRevision revision){
            RoutedSynthEngine e;CHECK(e.setPatch(delayed(impulsePatch(),d,revision))&&e.prepare(rate,512,quality));
            return render(e,int(1.1*rate),impulseEvents,512,120.0);};
        const auto modern=capture(2),legacy=capture(1);const double error=difference(modern,legacy);
        CHECK(peak(modern)>1e-6&&error==0);
        std::cout<<"MODERN_DELAY_WHOLE_SAMPLE rate="<<rate<<" quality="<<int(quality)<<" synced="<<synced<<" error="<<error<<'\n';
    }
}
void modernDelayEngine()
{
    section("modern_delay_partition_reset_bypass_tempo_tail_callbacks");
    const auto patch=delayed(impulsePatch(),{false,.0137f,.5f,.7f,.5f,9000,.8f},2);const auto dry=impulsePatch();
    for(double rate:{8000.,44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const std::vector<Event> phrase{{17,on()},{18,off()},{4400,on(60)},{4401,off(60)}};
        const auto capture=[&](const RoutedPatch& p,int partition){RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));return render(e,8192,phrase,partition);};
        RoutedSynthEngine a;CHECK(a.setPatch(patch)&&a.prepare(rate,512,quality));
        const auto coarse=render(a,8192,phrase,512),fine=capture(patch,61);
        CHECK(finite(coarse)&&energy(coarse)>1e-12&&peak(coarse)<4&&difference(coarse,fine)==0);
        a.reset();CHECK(energy(render(a,2048))==0);
        a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
        const auto input=capture(dry,512);
        auto bypassed=patch;bypassed.effects[0].bypassed=true;CHECK(difference(capture(bypassed,512),input)==0);
        auto zero=patch;zero.effects[0].delay.mix=0;CHECK(difference(capture(zero,512),input)==0);
        std::cout<<"MODERN_DELAY_STATE rate="<<rate<<" quality="<<int(quality)<<" energy="<<energy(coarse)<<" peak="<<peak(coarse)<<'\n';
    }
    // Synced repeats follow the tempo and drop out without one, as in revision 1.
    for(double rate:{44100.,48000.}){
        const auto synced=delayed(impulsePatch(),{true,.25f,.0625f,0,0,20000,1},2);const double bpm=130,expected=60*.0625/bpm*rate;
        const auto loudest=[](const Audio& a){std::size_t at=0;for(std::size_t i=0;i<a[0].size();++i)if(std::abs(a[0][i])>std::abs(a[0][at]))at=i;return double(at);};
        const auto echo=sound(synced,8192,impulseEvents,rate,512,bpm),source=sound(impulsePatch(),8192,impulseEvents,rate,512,bpm);
        CHECK(peak(echo)>1e-6&&std::abs(loudest(echo)-loudest(source)-expected)<=2);
        CHECK(difference(sound(synced,8192,impulseEvents,rate,512,std::nullopt),sound(impulsePatch(),8192,impulseEvents,rate,512,std::nullopt))==0);
        std::cout<<"MODERN_DELAY_TEMPO rate="<<rate<<" bpm="<<bpm<<" expected="<<expected<<" loudest="<<loudest(echo)-loudest(source)<<'\n';
    }
    // The longest feedback tails at the lowest rate, where each pass's extra
    // samples of kernel support weigh most: the reported bound with 8 frames
    // added per pass.
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);
        const DelaySettings longest{false,.0013f,.5f,.95f,.5f,20000,1};
        const double threshold=std::log(1e-6/9),repeats=std::ceil(threshold/std::log(double(longest.feedback)));
        const double seconds=(repeats+1)*(double(longest.seconds)-threshold/(2*std::numbers::pi*longest.filterHz)+8/internal);
        RoutedSynthEngine engine;CHECK(engine.setPatch(delayed(impulsePatch(),longest,2))&&engine.prepare(rate,512,quality));
        const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+800,impulseEvents,73);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[std::size_t(n)])));
        CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6);
        std::cout<<"MODERN_DELAY_TAIL quality="<<int(quality)<<" seconds="<<seconds<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){add(busy,EffectKind::delay,slot%5);auto& e=busy.effects[std::size_t(slot)];
        e.delay={false,.011f+.003f*float(slot),.5f,.5f,.3f,8000,.5f};e.algorithmRevision=2;busy.compatibility.effects[std::size_t(slot)][delayEffectBank]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256,RenderQuality::higher);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    bool accepted=true;float loudest=0;engine.render(audio,held,120);
    for(int block=0;block<48;++block){auto& e=busy.effects[std::size_t(block%8)];
        e.delay={block%5==0,block%2?.0013f:3.9f,.5f,block%2?0.f:.95f,block%2?0.f:1.f,block%2?20.f:20000.f,block%2?.1f:1.f,
                 block%2?1.f:0.f,block%2?2000.f:20.f,block%2?0.f:1.f,block%2?10.f:.1f,block%3==0,block%2?4.f:.25f,block%4<2,block%22};
        accepted&=engine.setPatch(busy);engine.render(audio,empty,120);loudest=std::max(loudest,audio.getMagnitude(0,256));if(block%17==0)engine.reset();}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_DELAY_CALLBACKS layers=4 notes=16 effects=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_DELAY_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&loudest>0);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local std::size_t allocatedBytes=0;
int __cdecl byteHook(int kind,void*,size_t size,int,long,const unsigned char*,int)
{ if(tracking&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC))allocatedBytes+=size; return 1; }
#endif
void modernDelayStorage()
{
    section("modern_delay_prepare_storage");
    // Revision 2 reads the existing delay history: preparing at 96 kHz stays
    // within 8 KiB of the pre-F12.25 build plus separately owned fixed states.
    // A second 4 s line per channel would add over 3 MB for each of the 16.
#if defined(_MSC_VER) && defined(_DEBUG)
    allocatedBytes=0;const auto savedHook=_CrtSetAllocHook(byteHook);tracking=true;
    { DynamicEq fixedState; }
    tracking=false;_CrtSetAllocHook(savedHook);const auto dynamicEqBytes=allocatedBytes;
    CHECK(dynamicEqBytes>0&&dynamicEqBytes<=16384);
    // The output float-overflow guard designs one fixed FIR bound at prepare.
    // Account for that separate transient allocation, retaining the delay's8KiB gate.
    allocatedBytes=0;const auto firHook=_CrtSetAllocHook(byteHook);tracking=true;
    {const auto fir=juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod(.08f,-100.f);(void)fir;}
    tracking=false;_CrtSetAllocHook(firHook);const auto headroomFirBytes=allocatedBytes;
    CHECK(headroomFirBytes>0&&headroomFirBytes<=16384);
    // The routed state retains requested and active settings for every slot.
    const auto dynamicEqSettingsBytes=2*8*sizeof(DynamicEqSettings);
    // Per layer-voice: three exact keys, eight doubles, validity/alignment word.
    const auto modernFilterReuseBytes=4*16*(3*sizeof(std::uint64_t)+8*sizeof(double)+sizeof(std::uint64_t));
    // Per layer-voice wavetable unison: six more phases per oscillator and the right side's two filter states. Per
    // layer: the cached voice math, the spread and width ramps, the mode and pending counts and the frame's pointer.
    struct UnisonMathStorage {struct Oscillator {int count;std::array<double,7> ratio,left,right;};std::array<Oscillator,2> oscillators;
        std::array<int,2> counts;std::array<double,2> spread;double width;bool valid;};
    struct RampStorage {double current,target,step;int remaining;};
    const auto wavetableUnisonBytes=4*16*(2*6*sizeof(double)+2*sizeof(double))
        +4*(sizeof(UnisonMathStorage)+3*sizeof(RampStorage)+2*2*sizeof(int)+sizeof(void*))+sizeof(std::uint32_t);
    // Per layer-voice wavetable drift: two fixed offsets. Per layer: the drift ramp and the parallel frame's wander and offset.
    const auto wavetableDriftBytes=4*16*2*sizeof(double)+4*(sizeof(RampStorage)+2*sizeof(double));
    // Per layer-voice wavetable ladders: the right side's four stages. Per layer: the mode and pending filter type, each
    // padded to the mode's size_t alignment.
    const auto wavetableLadderBytes=4*16*4*sizeof(double)+4*2*alignof(std::size_t);
    // Per layer-voice wavetable vowels: two sides' three formant histories of two. Per layer: the size ramp and the
    // parallel frame's size.
    const auto wavetableVowelBytes=4*16*12*sizeof(double)+4*(sizeof(RampStorage)+sizeof(double));
    // Separate fixed context/pool/thread-start CRT allowance. Platform-owned
    // thread stacks are not CRT bytes; the old delay allowance remains8KiB.
    constexpr std::size_t dspPipelineStorageAllowance=8192;
    // Space changes no delay history. Reconstruct only the pre-Space value
    // layout, then account for its two new patch copies and owning members.
    const auto alignBytes=[](std::size_t bytes,std::size_t alignment){return (bytes+alignment-1)/alignment*alignment;};
    constexpr auto oldEffectAlignment=std::max({alignof(DriveSettings),alignof(DelaySettings),alignof(ReverbSettings),
        alignof(CompressorSettings),alignof(EqSettings),alignof(FilterSettings),alignof(ChorusSettings),alignof(PhaserSettings),
        alignof(FlangerSettings),alignof(StereoSettings),alignof(DimensionSettings),alignof(FrequencyShiftSettings),
        alignof(ConvolutionSettings),alignof(FrequencySplitSettings),alignof(MidSideSplitSettings),alignof(DynamicEqSettings)});
    constexpr auto oldEffectEnd=offsetof(EffectSettings,dynamicEq)+sizeof(DynamicEqSettings)+sizeof(AlgorithmRevision);
    struct alignas(oldEffectAlignment) PreSpaceEffectStorage {std::array<std::byte,oldEffectEnd> bytes;};
    struct PreSpacePatchStorage {
        SynthPatch synth;RackLayout layout;std::array<PreSpaceEffectStorage,8> effects;
        std::array<bool,4> mute,solo;float outputGainDb;bool outputProtection;
        std::array<AlgorithmRevision,4*2+8*16> compatibility;
    };
    struct SpaceRoutingStorage {
        space::SpaceActivation activation;space::runtime::ModalSlotIdentities identities;
        space::SpaceActivationRequest request;RoutedPatch pending,previous;
        std::uint64_t key,recallToken,pendingRecallToken;bool hasBundle,pendingInstall,installPending,preparationFailed;
        std::array<std::array<double,2>,512> frames;std::array<std::uint64_t,8> renderedFrames;
    };
    static_assert(effectBankCount==19&&alignof(PreSpacePatchStorage)<=alignof(RoutedPatch)
        &&sizeof(PreSpacePatchStorage)<sizeof(RoutedPatch));
    static_assert(offsetof(EffectSettings,spaceReverb)>=oldEffectEnd-sizeof(AlgorithmRevision));
    const auto spaceExistingPatchExpansionBytes=2*(sizeof(RoutedPatch)-sizeof(PreSpacePatchStorage));
    const auto spaceRoutingMembersBytes=sizeof(SpaceRoutingStorage);
    // State already has 64-byte pipeline alignment; only its resulting padding
    // can extend the sum of these explicit added members.
    const auto spaceInlineBytes=alignBytes(spaceExistingPatchExpansionBytes+spaceRoutingMembersBytes,alignof(detail::DspPipeline));
    // The existing four patches already include Drum's numeric fields. Its
    // three borrowed transaction views and four fixed renderers are separate.
    struct DrumSourceViewStorage {RoutedPatch patch;BorrowedDrumAssignments assignments;std::uint64_t token;drums::PublicationIdentity identity;};
    struct DrumSourceRuntimeStorage {bool drum,targetDrum,zeroFirst;enum Phase {steady,down,up} phase;RampStorage gain;};
    struct DrumRoutingStorage {
        std::array<DrumSourceViewStorage,3> views;std::uint64_t passedGeneration;bool pendingValid,previousValid;
        std::array<DrumSourceRuntimeStorage,4> sources;std::array<drums::DrumEngine,4> drums;
        bool sourceAware,borrowedSegment;int segmentBase;
#ifndef NDEBUG
        void* sourceBorrowTestContext;void (*sourceBorrowTestHook)(void*,unsigned);
#endif
    };
    const auto drumRoutingInlineBytes=alignBytes(sizeof(DrumRoutingStorage),alignof(detail::DspPipeline));
    const auto drumSynthSourceBytes=2*sizeof(unsigned);
    // Realtime then higher quality, from the pre-F12.25 build.
    const std::array<std::size_t,2> before{108193848,202222039};
    for(std::size_t index=0;index<2;++index){
        const auto quality=index?RenderQuality::higher:RenderQuality::realtime;
        const auto masterEqScratchBytes=2*sizeof(double)*512*(index?2:1);
        // Independent prepared sidechain frames; retain every historical allowance.
        const auto externalDetectorScratchBytes=sizeof(DynamicEq::Frame)*512*(index?2:1);
        CHECK(externalDetectorScratchBytes<=16384);
        // Every FX slot's Era Reverb owns its buffers for the internal rate, used or not.
        allocatedBytes=0;const auto eraHook=_CrtSetAllocHook(byteHook);tracking=true;bool eraPrepared=false;
        {EraReverb era;eraPrepared=era.prepare(96000.*(index?2:1));}
        tracking=false;_CrtSetAllocHook(eraHook);const auto eraBytes=allocatedBytes;CHECK(eraPrepared&&eraBytes>0);
        RoutedSynthEngine warm;CHECK(warm.prepare(96000,512,quality));
        std::array<std::size_t,2> saturationConstructor{},saturationPrepare{};
        for(std::size_t trial=0;trial<2;++trial){
            allocatedBytes=0;const auto saturationHook=_CrtSetAllocHook(byteHook);tracking=true;bool prepared=false;
            {Saturation saturation;saturationConstructor[trial]=allocatedBytes;allocatedBytes=0;prepared=saturation.prepare(96000.*(index?2:1));saturationPrepare[trial]=allocatedBytes;}
            tracking=false;_CrtSetAllocHook(saturationHook);CHECK(prepared);
            CHECK(saturationConstructor[trial]>0&&saturationConstructor[trial]<200000&&saturationPrepare[trial]>0&&saturationPrepare[trial]<200000);
        }
        CHECK(saturationConstructor[0]==saturationConstructor[1]&&saturationPrepare[0]==saturationPrepare[1]);
        // One handle per Effect, rounded at State's fixed64 alignment, bounds
        // both each Effect's insertion padding and the aggregate State padding.
        const auto saturationInlineBytes=8*alignBytes(sizeof(Saturation),alignof(detail::DspPipeline));
        const auto saturationOwnerBytes=8*(saturationConstructor[0]+saturationPrepare[0]);
        std::array<std::size_t,2> spaceOwnerBytes{};
        for(auto& measured:spaceOwnerBytes){
            space::SpaceActivation activation;space::SpaceActivationRequest inactive;
            inactive.key=1;inactive.activeMask=0;inactive.identities={1,2,3,4,5,6,7,8};
            allocatedBytes=0;const auto activationHook=_CrtSetAllocHook(byteHook);tracking=true;
            const bool prepared=activation.prepare(96000,index?2U:1U,inactive);
            tracking=false;_CrtSetAllocHook(activationHook);measured=allocatedBytes;CHECK(prepared);
            if(prepared)CHECK(!activation.service().workersRunning()&&activation.service().modeCount()==0);
        }
        CHECK(spaceOwnerBytes[0]>0&&spaceOwnerBytes[0]==spaceOwnerBytes[1]);
        std::array<std::size_t,2> bytes{};
        for(auto& measured:bytes){
            RoutedSynthEngine engine;CHECK(engine.setPatch(delayed(impulsePatch(),{false,.0137f,.5f,.5f,.3f,9000,.5f},1)));
            allocatedBytes=0;auto old=_CrtSetAllocHook(byteHook);tracking=true;
            const bool prepared=engine.prepare(96000,512,quality);
            tracking=false;_CrtSetAllocHook(old);measured=allocatedBytes;CHECK(prepared);
        }
        const auto maximumBytes=before[index]+8*dynamicEqBytes+dynamicEqSettingsBytes+modernFilterReuseBytes+masterEqScratchBytes+externalDetectorScratchBytes+dspPipelineStorageAllowance+(index?headroomFirBytes:0)+spaceOwnerBytes[0]+spaceInlineBytes+drumRoutingInlineBytes+drumSynthSourceBytes+wavetableUnisonBytes+wavetableDriftBytes+wavetableLadderBytes+wavetableVowelBytes+8*eraBytes+saturationOwnerBytes+saturationInlineBytes+8192;
        CHECK(bytes[0]==bytes[1]&&bytes[0]<=maximumBytes);
        std::cout<<"MODERN_DELAY_PREPARE_BYTES rate=96000 quality="<<int(quality)<<" bytes="<<bytes[0]<<" repeat="<<bytes[1]
                 <<" fixed_dynamic_eq_bytes_each="<<dynamicEqBytes<<" fixed_dynamic_eq_settings_bytes="<<dynamicEqSettingsBytes
                 <<" fixed_modern_filter_reuse_bytes="<<modernFilterReuseBytes<<" master_eq_scratch_bytes="<<masterEqScratchBytes
                 <<" external_detector_scratch_bytes="<<externalDetectorScratchBytes
                 <<" dsp_pipeline_fixed_allowance="<<dspPipelineStorageAllowance
                 <<" inactive_space_prepare_bytes="<<spaceOwnerBytes[0]<<" inactive_space_prepare_repeat="<<spaceOwnerBytes[1]
                 <<" space_existing_patch_expansion_bytes="<<spaceExistingPatchExpansionBytes
                 <<" space_two_new_patch_bytes="<<2*sizeof(RoutedPatch)<<" space_settings_in_four_patches_bytes="<<4*8*sizeof(space::Settings)
                 <<" space_routing_member_bytes="<<spaceRoutingMembersBytes<<" space_inline_aligned_bytes="<<spaceInlineBytes
                 <<" drum_source_view_bytes_each="<<sizeof(DrumSourceViewStorage)<<" drum_source_runtime_bytes_each="<<sizeof(DrumSourceRuntimeStorage)
                 <<" drum_publication_identity_bytes_each="<<sizeof(drums::PublicationIdentity)<<" drum_passed_generation_bytes="<<sizeof(std::uint64_t)
                 <<" drum_engine_bytes_each="<<sizeof(drums::DrumEngine)<<" drum_routing_member_bytes="<<sizeof(DrumRoutingStorage)
                 <<" drum_routing_inline_aligned_bytes="<<drumRoutingInlineBytes<<" drum_synth_source_bytes="<<drumSynthSourceBytes
                 <<" era_reverb_prepare_bytes_each="<<eraBytes
                 <<" saturation_constructor_bytes_each="<<saturationConstructor[0]<<" saturation_prepare_bytes_each="<<saturationPrepare[0]
                 <<" saturation_owner_gross_bytes="<<saturationOwnerBytes<<" saturation_inline_aligned_bound_bytes="<<saturationInlineBytes
                 <<" wavetable_unison_bytes="<<wavetableUnisonBytes<<" wavetable_drift_bytes="<<wavetableDriftBytes<<" wavetable_ladder_bytes="<<wavetableLadderBytes<<" wavetable_vowel_bytes="<<wavetableVowelBytes<<" historical_baseline_bytes="<<before[index]<<" maximum_accounted_bytes="<<maximumBytes<<" delay_history_allowance="<<8192<<'\n';
    }
#else
    std::cout<<"MODERN_DELAY_PREPARE_BYTES instrumentation unavailable\n";
#endif
}
void legacyDelayFingerprint()
{
    section("legacy_delay_fingerprint");
    // Recalled sounds replay revision 1's linear reads; protect its exact float output.
    auto unsynced=flat();unsynced.synth.layers[0].level=.8f;add(unsynced,EffectKind::delay);unsynced.effects[0].delay={false,.0137f,.5f,.6f,.4f,7000,.6f};
    auto synced=unsynced;synced.effects[0].delay={true,.25f,.0625f,.75f,.8f,12000,.5f};
    // Free then synced at 137 BPM; realtime and higher quality at 44.1, 48 and 96 kHz, from the pre-F12.25 build.
    const std::array<std::uint64_t,12> expected{
        13875076186519456901ull,5513499406365821693ull,9872654237128488753ull,11442804469266527253ull,10556000153551454737ull,6627329905026061781ull,
        4540396878032363605ull,17005973299736533229ull,14002099772452406109ull,8209805836777237201ull,17790520079419535605ull,10229095691584754713ull};
    std::size_t index=0;
    for(const auto* patch:{&unsynced,&synced})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64,137.0);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_DELAY_FINGERPRINT synced="<<patch->effects[0].delay.synced<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
void modernDelayNeutralFingerprint()
{
    section("modern_delay_neutral_fingerprint");
    // Revision 2 at the tape keys' neutral values; protect its exact float output.
    // Free, synced at 137 BPM, then free with crossfeed and a time edit mid-note.
    const auto modern=[](RoutedPatch p){p.effects[0].algorithmRevision=2;p.compatibility.effects[0][delayEffectBank]=2;return p;};
    auto unsynced=flat();unsynced.synth.layers[0].level=.8f;add(unsynced,EffectKind::delay);unsynced.effects[0].delay={false,.0137f,.5f,.6f,.4f,7000,.6f};
    auto synced=unsynced;synced.effects[0].delay={true,.25f,.0625f,.75f,.8f,12000,.5f};
    auto crossed=unsynced;crossed.effects[0].delay={false,.0211f,.5f,.55f,.7f,9000,.5f};
    unsynced=modern(unsynced);synced=modern(synced);crossed=modern(crossed);
    // Realtime and higher quality at 44.1, 48 and 96 kHz, from the build before the tape keys.
    const std::array<std::uint64_t,18> expected{
        10273895205105027697ull,6714467093114018349ull,9109324630518444853ull,6310214086492437093ull,1490729120670038093ull,6681855512289556605ull,
        8541624943859907377ull,5686299779066194157ull,11535542908765251045ull,14301146642958815553ull,11576235557248786637ull,8217354678499233785ull,
        13057840675240696869ull,5800728158001177981ull,12679801534973762117ull,10437490549025435821ull,4481243609095741285ull,12898167256150846409ull};
    std::size_t index=0;
    for(const auto* patch:{&unsynced,&synced,&crossed})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        Audio a;
        if(patch==&crossed){
            a=render(engine,4096,{{0,on()}},64,137.0);
            auto edited=crossed;edited.effects[0].delay.seconds=.0089f;CHECK(engine.setPatch(edited));
            const auto rest=render(engine,4096,{{1904,off()}},64,137.0);
            for(std::size_t ch=0;ch<2;++ch)a[ch].insert(a[ch].end(),rest[ch].begin(),rest[ch].end());
        }
        else a=render(engine,8192,{{0,on()},{6000,off()}},64,137.0);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"MODERN_DELAY_FINGERPRINT patch="<<(patch==&unsynced?"free":patch==&synced?"synced":"crossed")<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
void modernDelayTapeFingerprint()
{
    section("modern_delay_tape_fingerprint");
    // Revision 2 with the tape keys moved, before the stereo keys; protect its exact float output.
    // Oxide Return's tape settings at a short TIME, free then synced at 137 BPM, then a worn tape with
    // crossfeed and a gliding time edit mid-note.
    const auto modern=[](RoutedPatch p){p.effects[0].algorithmRevision=2;p.compatibility.effects[0][delayEffectBank]=2;return p;};
    auto oxide=flat();oxide.synth.layers[0].level=.8f;add(oxide,EffectKind::delay);
    oxide.effects[0].delay={false,.031f,.0625f,.38f,.3f,5000,.25f,.3f,100,.2f,.8f,true};
    auto synced=oxide;synced.effects[0].delay.synced=true;
    auto worn=oxide;worn.effects[0].delay={false,.0211f,.5f,.55f,.7f,9000,.5f,1,400,1,3,true};
    oxide=modern(oxide);synced=modern(synced);worn=modern(worn);
    // Realtime and higher quality at 44.1, 48 and 96 kHz, from the build before the stereo keys.
    const std::array<std::uint64_t,18> expected{
        17513810810437341721ull,15425186657514368317ull,10643521226743668401ull,8376432680404508289ull,2347035926935947789ull,13139604452498546061ull,
        12407295925020885565ull,1368708358288703565ull,9425509597132024469ull,10429161732880146541ull,4402805215000811445ull,9614001788427236021ull,
        4112560599751019013ull,6083058919917873277ull,14406490148674638817ull,16651096346189104153ull,11775672023786566917ull,15486237191297423589ull};
    std::size_t index=0;
    for(const auto* patch:{&oxide,&synced,&worn})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        Audio a;
        if(patch==&worn){
            a=render(engine,4096,{{0,on()}},64,137.0);
            auto edited=worn;edited.effects[0].delay.seconds=.0089f;CHECK(engine.setPatch(edited));
            const auto rest=render(engine,4096,{{1904,off()}},64,137.0);
            for(std::size_t ch=0;ch<2;++ch)a[ch].insert(a[ch].end(),rest[ch].begin(),rest[ch].end());
        }
        else a=render(engine,8192,{{0,on()},{6000,off()}},64,137.0);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"MODERN_DELAY_TAPE_FINGERPRINT patch="<<(patch==&oxide?"oxide":patch==&synced?"synced":"worn")<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
// The delay's tape keys on a wet-only revision 2 delay. Levels are output dBFS:
// 1/16 at an effect reaches full scale at the default master level and output gain.
constexpr double tapeReference=1.0/16;
RoutedPatch taped(RoutedPatch p,const DelaySettings& d,float liftDb=0)
{
    std::size_t slot=0;
    if(liftDb>0){add(p,EffectKind::stereo);p.effects[0].stereo={liftDb,0,1,StereoChannelMode::stereo};slot=1;}
    add(p,EffectKind::delay);p.effects[slot].delay=d;p.effects[slot].algorithmRevision=2;
    p.compatibility.effects[slot][delayEffectBank]=2;
    return p;
}
DelaySettings wetDelay(float seconds,float feedback=0) { return {false,seconds,.5f,feedback,0,20000,1}; }
void delayTapeSaturation()
{
    section("delay_tape_saturation");
    const double rate=48000,f0=1000;
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int from=int(.25*rate),count=1<<14;
        struct Point{double dbfs,thd,h2,h3,gain;};
        const auto measure=[&](double target,float saturation,double hz){
            int key=0;const auto source=driveStimulus(hz,driveLevel(hz,tapeReference*std::pow(10.,target/20),rate),key);
            auto d=wetDelay(.1f);d.saturation=saturation;
            const auto dry=modulatedRender(source,key,rate,quality,from,count),wet=modulatedRender(taped(source,d),key,rate,quality,from,count);
            const double input=toneAmplitude(dry,hz,rate),h1=toneAmplitude(wet,hz,rate);
            double harmonics=0;for(int h=2;h<=5;++h){const double a=toneAmplitude(wet,h*hz,rate);harmonics+=a*a;}
            const auto db=[&](double a){return 20*std::log10(std::max(a,1e-300)/h1);};
            return Point{20*std::log10(input/tapeReference),10*std::log10(std::max(harmonics,1e-300)/(h1*h1)),db(toneAmplitude(wet,2*hz,rate)),db(toneAmplitude(wet,3*hz,rate)),20*std::log10(h1/input)};
        };
        // The level where the echo's H2 to H5 reach 0.1 % of H1, between 1 dB steps.
        Point previous{},nearest{0,1e9,0,0,0};double crossing=std::numeric_limits<double>::quiet_NaN();
        for(int level=-34;level<=-20;++level){
            const auto p=measure(level,1,f0);
            if(std::abs(p.thd+60)<std::abs(nearest.thd+60))nearest=p;
            if(level>-34&&std::isnan(crossing)&&previous.thd<-60&&p.thd>=-60)crossing=previous.dbfs+(-60-previous.thd)*(p.dbfs-previous.dbfs)/(p.thd-previous.thd);
            previous=p;
            std::cout<<"DELAY_TAPE_THD quality="<<int(quality)<<" dbfs="<<p.dbfs<<" thd_db="<<p.thd<<" h2_db="<<p.h2<<" h3_db="<<p.h3<<'\n';
        }
        CHECK(std::abs(crossing+27)<=1&&nearest.h3-nearest.h2>=60);
        const auto quiet=measure(-60,1,f0);CHECK(std::abs(quiet.gain)<=.1);
        std::cout<<"DELAY_TAPE_SATURATION quality="<<int(quality)<<" crossing_dbfs="<<crossing<<" h3_over_h2_db="<<nearest.h3-nearest.h2<<" small_signal_gain_db="<<quiet.gain<<'\n';
        // The antiderivative's two-sample average costs each pass cos(pi f / internal rate) while saturation is on.
        const double internal=rate*(quality==RenderQuality::higher?2:1);
        for(double hz:{5000.,10000.}){
            const double loss=measure(-60,1,hz).gain-measure(-60,0,hz).gain,expected=20*std::log10(std::cos(std::numbers::pi*hz/internal));
            CHECK(std::abs(loss-expected)<=.1);
            std::cout<<"DELAY_TAPE_SATURATION_TREBLE quality="<<int(quality)<<" hz="<<hz<<" loss_db="<<loss<<" expected_db="<<expected<<'\n';
        }
        int key=0;const auto source=driveStimulus(f0,driveLevel(f0,tapeReference*std::pow(10.,-12./20),rate),key);
        auto silent=wetDelay(.1f,.5f);silent.saturation=1;silent.mix=0;
        const auto full=[&](const RoutedPatch& p){RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));return render(e,int(.5*rate),{{0,on(key)}},512);};
        CHECK(difference(full(taped(source,silent)),full(source))==0);
        // A 0 dBFS burst into REPEATS 0.95: the curve holds every pass under its -8 dBFS ceiling and the repeats still die away.
        const float lift=12;int burstKey=0;const auto burst=driveStimulus(f0,driveLevel(f0,tapeReference/std::pow(10.,lift/20),rate),burstKey);
        auto loud=wetDelay(.1f,.95f);loud.saturation=1;
        RoutedSynthEngine e;CHECK(e.setPatch(taped(burst,loud,lift))&&e.prepare(rate,512,quality));
        const auto a=render(e,int(4*rate),{{0,on(burstKey)},{int(.05*rate),off(burstKey)}},512);
        double first=0,last=0,loudest=0;
        for(const auto& channel:a)for(std::size_t n=0;n<channel.size();++n){const double v=std::abs(double(channel[n]));loudest=std::max(loudest,v);
            if(n<std::size_t(.2*rate))first=std::max(first,v);if(n>=std::size_t(3*rate))last=std::max(last,v);}
        CHECK(finite(a)&&first>0&&loudest<=tapeReference*std::pow(10.,-8./20)&&last<first);
        std::cout<<"DELAY_TAPE_RUNAWAY quality="<<int(quality)<<" loudest_dbfs="<<20*std::log10(loudest/tapeReference)<<" first_dbfs="<<20*std::log10(first/tapeReference)
                 <<" last_second_dbfs="<<20*std::log10(std::max(last,1e-300)/tapeReference)<<'\n';
    }
}
// Magnitude at hz of x over [from, from + length), rectangular window.
double windowMagnitude(const std::vector<float>& x,std::size_t from,std::size_t length,double hz,double rate)
{
    std::complex<double> sum;
    for(std::size_t n=0;n<length;++n)sum+=double(x[from+n])*std::polar(1.0,-2*std::numbers::pi*hz*double(n)/rate);
    return std::abs(sum);
}
void delayTapeLowCut()
{
    section("delay_tape_low_cut");
    // Impulse repeats 200 ms apart at REPEATS 0.5. The first repeat passes the
    // echo and loop sections, each later one the loop section once more.
    constexpr double cornerRatio=1.5537739740300374;
    const double rate=48000;
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto capture=[&](float lowCutHz){auto d=wetDelay(.2f,.5f);d.lowCutHz=lowCutHz;
            RoutedSynthEngine e;CHECK(e.setPatch(taped(impulsePatch(),d))&&e.prepare(rate,512,quality));return render(e,int(.7*rate),impulseEvents,512);};
        const auto neutral=capture(20);
        const std::size_t first=std::size_t(.19*rate),second=std::size_t(.39*rate),length=std::size_t(.2*rate);
        const auto repeat=[&](const Audio& a,std::size_t from,double hz){return windowMagnitude(a[0],from,length,hz,rate);};
        for(float key:{100.f,500.f,1557.f}){
            const auto cut=capture(key);
            const double firstDb=20*std::log10(repeat(cut,first,key)/repeat(neutral,first,key));
            double worst=0;
            for(double hz=50;hz<=5001;hz*=std::pow(2.,1./6)){
                const double measured=20*std::log10(repeat(cut,second,hz)/repeat(cut,first,hz)*repeat(neutral,first,hz)/repeat(neutral,second,hz));
                const double w=hz*cornerRatio/key;worst=std::max(worst,std::abs(measured-10*std::log10(w*w/(1+w*w))));
            }
            CHECK(finite(cut)&&std::abs(firstDb+3)<=.5&&worst<=.3);
            std::cout<<"DELAY_TAPE_LOW_CUT quality="<<int(quality)<<" key_hz="<<key<<" first_repeat_db="<<firstDb<<" loop_section_worst_error_db="<<worst<<'\n';
        }
        // Back at 20 Hz both sections drop out: after a reset the render is the neutral one.
        auto moved=wetDelay(.2f,.5f);moved.lowCutHz=500;auto patch=taped(impulsePatch(),moved);
        RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));render(e,int(.1*rate),impulseEvents,512);
        patch.effects[0].delay.lowCutHz=20;CHECK(e.setPatch(patch));render(e,512);e.reset();
        CHECK(difference(render(e,int(.7*rate),impulseEvents,512),neutral)==0);
    }
}
// Frequency of the band f0 +- 2 kHz across the central 80% of 2^17 samples,
// from the analytic signal's phase advance over 1 ms (single-precision FFT
// phase is too noisy sample to sample).
std::vector<double> instantaneousHz(const std::vector<double>& x,double f0,double rate)
{
    constexpr int order=17,size=1<<order;
    juce::dsp::FFT fft(order);std::vector<juce::dsp::Complex<float>> wave(static_cast<std::size_t>(size)),spectrum(wave.size());
    const int taper=size/20;
    for(int i=0;i<size;++i){const int edge=std::min(i,size-1-i);const double w=edge<taper?.5-.5*std::cos(std::numbers::pi*edge/taper):1.;
        wave[std::size_t(i)]={float(x[std::size_t(i)]*w),0.f};}
    fft.perform(wave.data(),spectrum.data(),false);
    const double bin=rate/size;const int low=std::max(1,int((f0-2000)/bin)),high=std::min(size/2-1,int((f0+2000)/bin));
    for(int k=0;k<size;++k)if(k<low||k>high)spectrum[std::size_t(k)]={};
    fft.perform(spectrum.data(),wave.data(),true);
    std::vector<double> phase;phase.reserve(std::size_t(size));double unwrapped=0,previous=std::arg(wave[std::size_t(size/10)]);
    for(int i=size/10;i<size-size/10;++i){const double p=std::arg(wave[std::size_t(i)]);double step=p-previous;previous=p;
        step-=2*std::numbers::pi*std::round(step/(2*std::numbers::pi));unwrapped+=step;phase.push_back(unwrapped);}
    const std::size_t lag=std::size_t(std::lround(rate/1000));std::vector<double> hz;
    for(std::size_t n=0;n+lag<phase.size();++n)hz.push_back((phase[n+lag]-phase[n])*rate/(2*std::numbers::pi*double(lag)));
    return hz;
}
// Least-squares mean and amplitude of f at a known modulation rate; the read's
// fractional-position jitter rides above it and stays out of the fit.
std::pair<double,double> sineFit(const std::vector<double>& f,double hz,double rate)
{
    std::array<std::array<double,4>,3> m{};
    for(std::size_t n=0;n<f.size();++n){const double t=2*std::numbers::pi*hz*double(n)/rate;const std::array<double,3> r{1,std::cos(t),std::sin(t)};
        for(int i=0;i<3;++i){for(int j=0;j<3;++j)m[i][j]+=r[i]*r[j];m[i][3]+=r[i]*f[n];}}
    for(int i=0;i<3;++i)for(int k=i+1;k<3;++k){const double q=m[k][i]/m[i][i];for(int j=i;j<4;++j)m[k][j]-=q*m[i][j];}
    std::array<double,3> x{};
    for(int i=2;i>=0;--i){double s=m[i][3];for(int j=i+1;j<3;++j)s-=m[i][j]*x[j];x[i]=s/m[i][i];}
    return {x[0],std::hypot(x[1],x[2])};
}
void delayTapeWobble()
{
    section("delay_tape_wobble");
    const double rate=48000,f0=1000;
    int key=0;const auto source=driveStimulus(f0,driveLevel(f0,tapeReference*std::pow(10.,-12./20),rate),key);
    const int from=int(.3*rate),count=1<<17;
    const auto wobbled=[&](float seconds,float wobble,float hz){auto d=wetDelay(seconds);d.wobble=wobble;d.wobbleRateHz=hz;return taped(source,d);};
    const auto swing=[&](const std::vector<double>& f){const auto [low,high]=std::minmax_element(f.begin(),f.end());return std::pair{*high,(*high-*low)/2};};
    const auto allFinite=[](const std::vector<double>& x){return std::all_of(x.begin(),x.end(),[](double v){return std::isfinite(v);});};
    // The read swings 1 ms * wobble either side of TIME: the pitch peaks at 1 + pi * rate * 2 ms * wobble.
    for(const auto [wobble,hz]:std::initializer_list<std::pair<float,float>>{{1.f,.8f},{.4f,5.5f},{1.f,10.f}}){
        const auto echo=modulatedRender(wobbled(.2f,wobble,hz),key,rate,RenderQuality::realtime,from,count);
        const auto [mean,deviation]=sineFit(instantaneousHz(echo,f0,rate),hz,rate);
        const double cents=1200*std::log2(1+std::numbers::pi*hz*.002*wobble),expected=f0*std::pow(2.,cents/1200);
        CHECK(allFinite(echo)&&std::abs(mean+deviation-expected)<=.05*(expected-f0));
        std::cout<<"DELAY_TAPE_WOBBLE wobble="<<wobble<<" rate_hz="<<hz<<" peak_hz="<<mean+deviation<<" expected_hz="<<expected<<" deviation_hz="<<deviation<<" cents="<<cents<<'\n';
    }
    const auto [unitMean,unitDeviation]=sineFit(instantaneousHz(modulatedRender(wobbled(.2f,1,1),key,rate,RenderQuality::realtime,from,count),f0,rate),1,rate);
    const double excursion=unitDeviation/(std::numbers::pi*1*f0);
    CHECK(std::abs(excursion-.002)<=.002*.01);
    // At TIME 1 ms the swing has no room and the echo holds its pitch.
    const auto shortest=modulatedRender(wobbled(.001f,1,10),key,rate,RenderQuality::realtime,from,count);
    const auto [shortPeak,shortDeviation]=swing(instantaneousHz(shortest,f0,rate));
    CHECK(allFinite(shortest)&&shortDeviation<=.01);
    std::cout<<"DELAY_TAPE_WOBBLE_EXCURSION peak_to_peak_ms="<<excursion*1000<<" unit_mean_hz="<<unitMean<<" time_1ms_deviation_hz="<<shortDeviation<<'\n';
    // The rate moves every block from 0.1 to 10 Hz: the phase runs on, so no
    // step outgrows the steady echo's by more than the 3 % pitch rise.
    const auto steps=[&](float wobble,bool sweep){
        auto patch=wobbled(.2f,wobble,.1f);RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,RenderQuality::realtime));
        double largest=0,previous=0;juce::AudioBuffer<float> audio(2,512);
        for(int block=0;block<int(2*rate)/512;++block){
            if(sweep){patch.effects[0].delay.wobbleRateHz=float(.1+9.9*block/(2*rate/512));CHECK(e.setPatch(patch));}
            juce::MidiBuffer midi;if(block==0)midi.addEvent(on(key),0);e.render(audio,midi,std::nullopt);
            for(int i=0;i<512;++i){const double v=audio.getSample(0,i);if(block*512+i>=int(.25*rate))largest=std::max(largest,std::abs(v-previous));previous=v;}
        }
        return largest;};
    const double steady=steps(0,false),swept=steps(.5f,true);
    CHECK(steady>0&&swept<=steady*1.05);
    std::cout<<"DELAY_TAPE_WOBBLE_SWEEP steady_step="<<steady<<" swept_step="<<swept<<" ratio="<<swept/steady<<'\n';
    // Back at 0 the read is TIME exactly: after a reset the render is the neutral one.
    const auto capture=[&](RoutedSynthEngine& e){return render(e,int(.5*rate),{{0,on(key)}},512);};
    RoutedSynthEngine neutral;CHECK(neutral.setPatch(wobbled(.2f,0,.8f))&&neutral.prepare(rate,512,RenderQuality::realtime));
    auto patch=wobbled(.2f,1,3);RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,RenderQuality::realtime));capture(e);
    patch.effects[0].delay.wobble=0;CHECK(e.setPatch(patch));render(e,512);e.reset();
    CHECK(difference(capture(e),capture(neutral))==0);
}
// The transport's 0.5 s one-pole from `from` to `to` seconds: an impulse
// written `written` seconds after the change is read when t_r - t_w = g(t_r).
double glideArrival(double from,double to,double written)
{
    const auto late=[&](double t){return t-written-(to+(from-to)*std::exp(-t/.5));};
    double low=written,high=written+5;
    for(int i=0;i<100;++i){const double middle=.5*(low+high);(late(middle)<0?low:high)=middle;}
    return .5*(low+high);
}
void delayTapeGlide()
{
    section("delay_tape_glide");
    const double rate=48000;
    const auto glided=[](float seconds,bool glide){auto d=wetDelay(seconds);d.glide=glide;return d;};
    // Seconds from the change to the echo's loudest sample, for an impulse
    // written `written` seconds after TIME (or the synced beats) moves to `target`.
    const auto arrival=[&](const DelaySettings& d,float target,double written,RenderQuality quality,std::optional<double> tempo){
        auto patch=taped(impulsePatch(),d);RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));
        render(e,int(rate),{},512,tempo);
        (d.synced?patch.effects[0].delay.beats:patch.effects[0].delay.seconds)=target;CHECK(e.setPatch(patch));
        const int at=int(std::lround(written*rate));const auto a=render(e,at+int(2.5*rate),{{at,on()},{at+1,off()}},512,tempo);
        std::size_t loudest=std::size_t(at)+1;
        for(std::size_t n=loudest;n<a[0].size();++n)if(std::abs(a[0][n])>std::abs(a[0][loudest]))loudest=n;
        return std::pair{double(loudest)/rate,finite(a)};};
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})
        for(const auto [from,to]:std::initializer_list<std::pair<float,float>>{{.25f,1.f},{1.f,.25f}})
            for(double written:{.2,.6,1.,1.6}){
                const auto [measured,clean]=arrival(glided(from,true),to,written,quality,std::nullopt);
                const double expected=glideArrival(from,to,written);
                CHECK(clean&&std::abs(measured-expected)<=.015);
                std::cout<<"DELAY_TAPE_GLIDE quality="<<int(quality)<<" from_s="<<from<<" to_s="<<to<<" written_s="<<written
                         <<" arrival_s="<<measured<<" expected_s="<<expected<<'\n';
            }
    // Synced at 120 BPM the transport glides between the resolved times the same way.
    auto synced=glided(.25f,true);synced.synced=true;synced.beats=.25f;
    const auto [syncArrival,syncClean]=arrival(synced,.5f,.3,RenderQuality::realtime,120.0);
    const double syncExpected=glideArrival(.125,.25,.3),syncDelay=syncArrival-.3;
    CHECK(syncClean&&syncDelay>.125&&syncDelay<.25&&std::abs(syncArrival-syncExpected)<=.005);
    std::cout<<"DELAY_TAPE_GLIDE_SYNCED arrival_s="<<syncArrival<<" expected_s="<<syncExpected<<" echo_delay_s="<<syncDelay<<'\n';
    // A fresh instance and a reset start the transport at TIME: the echo is
    // the glide-off render's, sample for sample, and glide off is neutral.
    const auto capture=[&](RoutedSynthEngine& e){return render(e,int(.5*rate),impulseEvents,512);};
    RoutedSynthEngine neutral;CHECK(neutral.setPatch(taped(impulsePatch(),glided(.25f,false)))&&neutral.prepare(rate,512,RenderQuality::realtime));
    const auto reference=capture(neutral);
    RoutedSynthEngine fresh;CHECK(fresh.setPatch(taped(impulsePatch(),glided(.25f,true)))&&fresh.prepare(rate,512,RenderQuality::realtime));
    const auto first=capture(fresh);
    std::size_t loudest=0;for(std::size_t n=0;n<first[0].size();++n)if(std::abs(first[0][n])>std::abs(first[0][loudest]))loudest=n;
    auto patch=taped(impulsePatch(),glided(1,true));RoutedSynthEngine moving;CHECK(moving.setPatch(patch)&&moving.prepare(rate,512,RenderQuality::realtime));
    render(moving,int(rate));patch.effects[0].delay.seconds=.25f;CHECK(moving.setPatch(patch));render(moving,int(.2*rate));moving.reset();
    const auto afterReset=capture(moving);
    patch.effects[0].delay.seconds=1;CHECK(moving.setPatch(patch));render(moving,int(.2*rate));
    patch.effects[0].delay.seconds=.25f;patch.effects[0].delay.glide=false;CHECK(moving.setPatch(patch));render(moving,512);moving.reset();
    CHECK(difference(first,reference)==0&&difference(afterReset,reference)==0&&difference(capture(moving),reference)==0
          &&std::abs(double(loudest)-.25*rate)<=2);
    std::cout<<"DELAY_TAPE_GLIDE_START echo_sample="<<loudest<<" time_sample="<<.25*rate<<'\n';
}
// The reported tail at a steady TIME, as InstrumentProcessor adds the tape
// keys: per pass the wobble's 1 ms, the saturation's sample and the loop
// low cut's decay, then the echo low cut's decay once. The glide's lag is the caller's.
double delayTapeTailSeconds(const DelaySettings& d,double internal)
{
    const double threshold=std::log(1e-6/9),repeats=d.feedback>0?std::ceil(threshold/std::log(double(d.feedback))):1;
    const auto decay=[&](double hz){return -threshold/(2*std::numbers::pi*hz);};
    const double lowCut=d.lowCutHz>20?decay(d.lowCutHz/1.5537739740300374):0;
    return (repeats+1)*(d.seconds+decay(d.filterHz)+8/internal+lowCut+(d.wobble>0?.001:0)+(d.saturation>0?1/internal:0))+lowCut;
}
void delayTapeTail()
{
    section("delay_tape_tail");
    const double rate=8000;
    const auto residualAfter=[](const Audio& a,int boundary){double r=0;
        for(const auto& channel:a)for(int n=boundary;n<int(channel.size());++n)r=std::max(r,std::abs(double(channel[std::size_t(n)])));return r;};
    // Every key moved at REPEATS 0.95 and 1 ms, where the per-pass terms weigh most.
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double internal=rate*(quality==RenderQuality::higher?2:1);
        const DelaySettings d{false,.001f,.5f,.95f,.5f,20000,1,1,20.5f,1,10,true};
        const double seconds=delayTapeTailSeconds(d,internal)+.5*std::log(4/.001);
        RoutedSynthEngine e;CHECK(e.setPatch(taped(impulsePatch(),d))&&e.prepare(rate,512,quality));
        const int boundary=int(std::ceil(seconds*rate))+2*e.latencySamples();
        const auto tail=render(e,boundary+800,impulseEvents,73);const double residual=residualAfter(tail,boundary);
        CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6);
        std::cout<<"DELAY_TAPE_TAIL quality="<<int(quality)<<" seconds="<<seconds<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    // TIME glides from 4 s to 1 ms as the last input arrives, and the repeats
    // bunch up behind the slowing transport: N passes end by N TIME + 0.5 ln(4 s / TIME).
    // Five time constants (2.5 s) end too early.
    auto d=wetDelay(4,.95f);d.glide=true;auto patch=taped(impulsePatch(),d);
    RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,RenderQuality::realtime));
    render(e,512);patch.effects[0].delay.seconds=.001f;CHECK(e.setPatch(patch));
    const double steady=delayTapeTailSeconds(patch.effects[0].delay,rate),lag=.5*std::log(4/.001);
    const int boundary=int(std::ceil((steady+lag)*rate)),early=int(std::ceil((steady+2.5)*rate));
    const auto tail=render(e,boundary+800,impulseEvents,73);
    const double residual=residualAfter(tail,boundary),fiveTimeConstants=residualAfter(tail,early);
    CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6&&fiveTimeConstants>peak(tail)*1e-6);
    std::cout<<"DELAY_TAPE_GLIDE_TAIL seconds="<<steady+lag<<" residual="<<residual<<" five_time_constants_residual="<<fiveTimeConstants<<" peak="<<peak(tail)<<'\n';
}
// The delay's stereo keys on a wet-only revision 2 delay fed from one side: a
// stereo effect before it pans the voice hard left (-1) or right (1), so the
// other side's input is exactly zero.
RoutedPatch sided(RoutedPatch p,const DelaySettings& d,float pan)
{
    add(p,EffectKind::stereo);p.effects[0].stereo={0,pan,1,StereoChannelMode::stereo};
    add(p,EffectKind::delay);p.effects[1].delay=d;p.effects[1].algorithmRevision=2;
    p.compatibility.effects[1][delayEffectBank]=2;
    return p;
}
std::size_t loudestSample(const std::vector<float>& x,std::size_t from=0)
{ std::size_t at=from;for(std::size_t n=from;n<x.size();++n)if(std::abs(x[n])>std::abs(x[at]))at=n;return at; }
bool silent(const std::vector<float>& x) { return std::all_of(x.begin(),x.end(),[](float v){return v==0;}); }
void delayStereoRightTime()
{
    section("delay_stereo_right_time");
    const double rate=48000;
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto capture=[&](const DelaySettings& d,float pan,double seconds,std::optional<double> tempo){
            RoutedSynthEngine e;CHECK(e.setPatch(sided(impulsePatch(),d,pan))&&e.prepare(rate,512,quality));
            return render(e,int(seconds*rate),impulseEvents,512,tempo);};
        auto dry=wetDelay(.2f);dry.mix=0;
        const double zeroLeft=double(loudestSample(capture(dry,-1,.1,std::nullopt)[0])),zeroRight=double(loudestSample(capture(dry,1,.1,std::nullopt)[1]));
        // A left-only impulse echoes on the left at TIME and a right-only one on the right at TIME * RIGHT.
        const auto sides=[&](const DelaySettings& d,double left,double right,std::optional<double> tempo){
            const double seconds=std::max(left,right)+.1;
            const auto l=capture(d,-1,seconds,tempo),r=capture(d,1,seconds,tempo);
            const double leftAt=double(loudestSample(l[0]))-zeroLeft,rightAt=double(loudestSample(r[1]))-zeroRight;
            CHECK(finite(l)&&finite(r)&&peak(l)>0&&peak(r)>0&&silent(l[1])&&silent(r[0]));
            CHECK(std::abs(leftAt-left*rate)<=1&&std::abs(rightAt-right*rate)<=1);
            std::cout<<"DELAY_STEREO_RIGHT_TIME quality="<<int(quality)<<" seconds="<<d.seconds<<" ratio="<<d.rightRatio<<" synced="<<d.synced
                     <<" left_samples="<<leftAt<<" expected="<<left*rate<<" right_samples="<<rightAt<<" expected="<<right*rate<<'\n';};
        for(float ratio:{.25f,.5f,1.5f,4.f}){auto d=wetDelay(.2f);d.rightRatio=ratio;sides(d,.2,.2*ratio,std::nullopt);}
        // The right time is held between 1 ms and 4 s.
        auto longest=wetDelay(1.5f);longest.rightRatio=4;sides(longest,1.5,4,std::nullopt);
        auto shortest=wetDelay(.002f);shortest.rightRatio=.25f;sides(shortest,.002,.001,std::nullopt);
        // Synced, RIGHT scales the resolved time: a quarter note at 120 BPM.
        auto synced=wetDelay(.2f);synced.synced=true;synced.beats=1;synced.rightRatio=.75f;sides(synced,.5,.375,120.0);
        // With glide on each side's transport follows its own time on the 0.5 s one-pole.
        const auto arrival=[&](float pan,std::size_t ch,double written){
            auto d=wetDelay(.25f);d.glide=true;d.rightRatio=.5f;auto patch=sided(impulsePatch(),d,pan);
            RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));render(e,int(rate),{},512);
            patch.effects[1].delay.seconds=1;CHECK(e.setPatch(patch));
            const int at=int(std::lround(written*rate));const auto a=render(e,at+int(2.5*rate),{{at,on()},{at+1,off()}},512);
            return std::pair{double(loudestSample(a[ch],std::size_t(at)+1))/rate,finite(a)&&silent(a[1-ch])};};
        for(double written:{.2,.6,1.}){
            const auto [left,leftClean]=arrival(-1,0,written);const auto [right,rightClean]=arrival(1,1,written);
            const double leftExpected=glideArrival(.25,1,written),rightExpected=glideArrival(.125,.5,written);
            CHECK(leftClean&&rightClean&&std::abs(left-leftExpected)<=.015&&std::abs(right-rightExpected)<=.015);
            std::cout<<"DELAY_STEREO_RIGHT_GLIDE quality="<<int(quality)<<" written_s="<<written<<" left_s="<<left<<" expected_s="<<leftExpected
                     <<" right_s="<<right<<" expected_s="<<rightExpected<<'\n';
        }
    }
    // Wobble 1 at TIME 1.5 ms: the left swings its 0.5 ms of room; the right, held at 1 ms, has none and holds its pitch.
    const double f0=1000,wobbleHz=10;
    int key=0;const auto source=driveStimulus(f0,driveLevel(f0,tapeReference*std::pow(10.,-12./20),rate),key);
    const int from=int(.3*rate),count=1<<17;
    auto w=wetDelay(.0015f);w.wobble=1;w.wobbleRateHz=float(wobbleHz);w.rightRatio=.25f;
    const auto left=modulatedRender(taped(source,w),key,rate,RenderQuality::realtime,from,count,0);
    const auto right=modulatedRender(taped(source,w),key,rate,RenderQuality::realtime,from,count,1);
    const auto leftDeviation=sineFit(instantaneousHz(left,f0,rate),wobbleHz,rate).second;
    const auto rightHz=instantaneousHz(right,f0,rate);const auto [low,high]=std::minmax_element(rightHz.begin(),rightHz.end());
    const double expected=2*std::numbers::pi*wobbleHz*.0005*f0,rightDeviation=(*high-*low)/2;
    const auto allFinite=[](const std::vector<double>& x){return std::all_of(x.begin(),x.end(),[](double v){return std::isfinite(v);});};
    CHECK(allFinite(left)&&allFinite(right)&&std::abs(leftDeviation-expected)<=.05*expected&&rightDeviation<=.01);
    std::cout<<"DELAY_STEREO_RIGHT_WOBBLE left_deviation_hz="<<leftDeviation<<" expected_hz="<<expected<<" right_deviation_hz="<<rightDeviation<<'\n';
    // RIGHT moves every block from 1 to 2: the right read ramps, so no step outgrows the steady echo's.
    const auto steps=[&](bool sweep){
        auto patch=taped(source,wetDelay(.2f));RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,RenderQuality::realtime));
        double largest=0,previous=0;juce::AudioBuffer<float> audio(2,512);const int blocks=int(2*rate)/512;
        for(int block=0;block<blocks;++block){
            if(sweep){patch.effects[0].delay.rightRatio=float(1+double(block)/blocks);CHECK(e.setPatch(patch));}
            juce::MidiBuffer midi;if(block==0)midi.addEvent(on(key),0);e.render(audio,midi,std::nullopt);
            for(int i=0;i<512;++i){const double v=audio.getSample(1,i);if(block*512+i>=int(.25*rate))largest=std::max(largest,std::abs(v-previous));previous=v;}
        }
        return largest;};
    const double steady=steps(false),swept=steps(true);
    CHECK(steady>0&&swept<=steady*1.05);
    std::cout<<"DELAY_STEREO_RIGHT_SWEEP steady_step="<<steady<<" swept_step="<<swept<<" ratio="<<swept/steady<<'\n';
}
// Ping-pong at TIME 100 ms and RIGHT 1.5: a left hop at 100 ms, then a right
// hop 150 ms later. Levels compare each hop's content at `hz` over a window
// from 10 ms before it.
void delayStereoPingPong()
{
    section("delay_stereo_ping_pong");
    const double rate=48000;
    const auto bounced=[](float feedback){auto d=wetDelay(.1f,feedback);d.rightRatio=1.5f;d.pingPong=true;return d;};
    const auto hop=[&](const std::vector<float>& x,double at,double length=.15,double hz=1000){
        return windowMagnitude(x,std::size_t((at-.01)*rate),std::size_t(length*rate),hz,rate);};
    const auto quietAt=[&](const std::vector<float>& x,double at){
        return std::all_of(x.begin()+std::ptrdiff_t((at-.005)*rate),x.begin()+std::ptrdiff_t((at+.005)*rate),[](float v){return v==0;});};
    const auto db=[](double ratio){return 20*std::log10(ratio);};
    const auto perQuality=[&](RenderQuality quality){
        const auto capture=[&](const RoutedPatch& p,double seconds){
            RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));return render(e,int(seconds*rate),impulseEvents,512);};
        // Feedback 0: the mono sum's left hop at 1/sqrt 2 of a normal echo, then the right hop at its level.
        auto normal=bounced(0);normal.pingPong=false;
        const auto reference=capture(sided(impulsePatch(),normal,-1),.5),left=capture(sided(impulsePatch(),bounced(0),-1),.5);
        auto dry=normal;dry.mix=0;const auto zero=double(loudestSample(capture(sided(impulsePatch(),dry,-1),.1)[0]));
        const double leftAt=double(loudestSample(left[0]))-zero,rightAt=double(loudestSample(left[1]))-zero;
        const double levelDb=db(hop(left[0],.1)/hop(reference[0],.1)),hopDb=db(hop(left[1],.25)/hop(left[0],.1));
        double stray=0;
        for(std::size_t ch=0;ch<2;++ch){const double at=ch?.25:.1;
            for(std::size_t n=0;n<left[ch].size();++n)if(double(n)<(at-.005)*rate||double(n)>(at+.01)*rate)stray=std::max(stray,std::abs(double(left[ch][n])));}
        CHECK(finite(left)&&std::abs(leftAt-.1*rate)<=1&&std::abs(rightAt-.25*rate)<=1);
        CHECK(std::abs(levelDb+db(std::numbers::sqrt2))<=.1&&std::abs(hopDb)<=.1&&quietAt(left[1],.1)&&quietAt(left[0],.25)&&stray<=peak(left)*1e-6);
        std::cout<<"DELAY_STEREO_PING_PONG quality="<<int(quality)<<" left_samples="<<leftAt<<" right_samples="<<rightAt<<" left_vs_normal_db="<<levelDb
                 <<" right_vs_left_db="<<hopDb<<" stray_peak="<<stray<<'\n';
        // The right side alone gives the same output, both sides 6.02 dB more, opposite sides silence.
        const auto right=capture(sided(impulsePatch(),bounced(0),1),.5),both=capture(sided(impulsePatch(),bounced(0),0),.5);
        auto opposed=impulsePatch();add(opposed,EffectKind::stereo);opposed.effects[0].stereo={0,-1,1,StereoChannelMode::stereo};
        add(opposed,EffectKind::midSideSplit);opposed.effects[1].midSideSplit.midMuted=true;
        add(opposed,EffectKind::delay);opposed.effects[2].delay=normal;opposed.effects[2].algorithmRevision=2;opposed.compatibility.effects[2][delayEffectBank]=2;
        const auto opposedNormal=capture(opposed,.5);opposed.effects[2].delay=bounced(0);const auto opposedBounced=capture(opposed,.5);
        const double bothDb=db(hop(both[0],.1)/hop(left[0],.1));
        CHECK(difference(right,left)==0&&std::abs(bothDb-db(2))<=.1&&peak(opposedNormal)>0&&peak(opposedBounced)==0);
        std::cout<<"DELAY_STEREO_PING_PONG_SUM quality="<<int(quality)<<" right_only_difference="<<difference(right,left)<<" both_db="<<bothDb
                 <<" opposed_peak="<<peak(opposedBounced)<<'\n';
        // Feedback 0.5: hops alternate left at 100 + 250 k ms and right at 250 + 250 k ms, each round trip 6.02 dB down.
        const auto bouncing=capture(sided(impulsePatch(),bounced(.5f),-1),1.25);
        double worst=0;bool quiet=true;
        for(int k=0;k<4;++k){
            const double l=.1+.25*k,r=.25+.25*k;quiet=quiet&&quietAt(bouncing[1],l)&&quietAt(bouncing[0],r);
            if(k<3)worst=std::max({worst,std::abs(db(hop(bouncing[0],l+.25)/hop(bouncing[0],l))+db(2)),std::abs(db(hop(bouncing[1],r+.25)/hop(bouncing[1],r))+db(2))});
        }
        CHECK(finite(bouncing)&&quiet&&worst<=.1);
        // CROSS has no effect.
        auto crossed=bounced(.5f);crossed.stereo=.5f;const auto half=capture(sided(impulsePatch(),crossed,-1),1.25);
        crossed.stereo=1;const auto full=capture(sided(impulsePatch(),crossed,-1),1.25);
        CHECK(difference(half,bouncing)==0&&difference(full,bouncing)==0);
        std::cout<<"DELAY_STEREO_PING_PONG_FEEDBACK quality="<<int(quality)<<" round_trip_worst_error_db="<<worst<<" quiet="<<quiet
                 <<" cross_difference="<<std::max(difference(half,bouncing),difference(full,bouncing))<<'\n';
        // Low cut: each hop passes one more loop section than the last, as the tape law predicts.
        constexpr double cornerRatio=1.5537739740300374;const float key=400;
        auto cutting=bounced(0);cutting.lowCutHz=key;const auto cut=capture(sided(impulsePatch(),cutting,-1),.5);
        const double firstDb=db(hop(cut[0],.1,.2,key)/hop(left[0],.1,.2,key));double loopError=0;
        for(double hz=50;hz<=5001;hz*=std::pow(2.,1./6)){
            const double measured=db(hop(cut[1],.25,.2,hz)/hop(cut[0],.1,.2,hz)*hop(left[0],.1,.2,hz)/hop(left[1],.25,.2,hz));
            const double w=hz*cornerRatio/key,error=std::abs(measured-10*std::log10(w*w/(1+w*w)));if(!(error<=loopError))loopError=error;
        }
        CHECK(finite(cut)&&std::abs(firstDb+3)<=.5&&loopError<=.3);
        std::cout<<"DELAY_STEREO_PING_PONG_LOW_CUT quality="<<int(quality)<<" first_hop_db="<<firstDb<<" loop_section_worst_error_db="<<loopError<<'\n';
    };
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})perQuality(quality);
    [&]{
    // The switch is structural: the lines clear, and the next phrase renders as on a fresh instance.
    const int ramp=int(std::ceil(rate*.005));
    auto patch=sided(impulsePatch(),bounced(.5f),-1);patch.effects[1].delay.pingPong=false;
    RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,RenderQuality::realtime));
    const auto before=render(e,int(.3*rate),impulseEvents,512);
    patch.effects[1].delay.pingPong=true;CHECK(e.setPatch(patch));render(e,3*ramp);
    const auto cleared=render(e,int(.1*rate));const auto after=render(e,int(.8*rate),impulseEvents,512);
    RoutedSynthEngine fresh;CHECK(fresh.setPatch(patch)&&fresh.prepare(rate,512,RenderQuality::realtime));
    CHECK(peak(before)>0&&peak(cleared)==0&&difference(after,render(fresh,int(.8*rate),impulseEvents,512))==0);
    // A switch mid-tail clicks no louder than a delay.synced switch.
    const double f0=1000;int key=0;const auto source=driveStimulus(f0,driveLevel(f0,tapeReference*std::pow(10.,-12./20),rate),key);
    const auto click=[&](bool pingPong){
        auto p=taped(source,wetDelay(.1f,.5f));p.effects[0].delay.rightRatio=1.5f;
        RoutedSynthEngine s;CHECK(s.setPatch(p)&&s.prepare(rate,512,RenderQuality::realtime));render(s,int(.4*rate),{{0,on(key)}},512,120.0);
        (pingPong?p.effects[0].delay.pingPong:p.effects[0].delay.synced)=true;CHECK(s.setPatch(p));
        const auto a=render(s,3*ramp,{},512,120.0);double largest=0;
        for(const auto& channel:a)for(std::size_t n=1;n<channel.size();++n)largest=std::max(largest,std::abs(double(channel[n])-channel[n-1]));
        return largest;};
    const double pingPongClick=click(true),syncedClick=click(false);
    CHECK(syncedClick>0&&pingPongClick<=syncedClick*1.05);
    std::cout<<"DELAY_STEREO_PING_PONG_SWITCH cleared_peak="<<peak(cleared)<<" ping_pong_step="<<pingPongClick<<" synced_step="<<syncedClick<<'\n';
    }();
}
// Synced, wet-only, feedback 0. NOTE's steps after Beats, from 1/64 triplet to
// 1/1 dotted, in quarter-note beats: 4 / denominator, times 2/3 for a triplet
// and 3/2 for a dotted note.
DelaySettings syncedDelay(int note,float beats=.5f) { auto d=wetDelay(.1f);d.synced=true;d.beats=beats;d.note=note;return d; }
void delayStereoNote()
{
    section("delay_stereo_note");
    const double rate=48000;
    constexpr double triplet=2./3,straight=1,dotted=1.5;
    const std::array<std::pair<int,double>,21> notes{{{64,triplet},{64,straight},{32,triplet},{64,dotted},{32,straight},{16,triplet},{32,dotted},
        {16,straight},{8,triplet},{16,dotted},{8,straight},{4,triplet},{8,dotted},{4,straight},{2,triplet},{4,dotted},{2,straight},
        {1,triplet},{2,dotted},{1,straight},{1,dotted}}};
    const auto noteBeats=[&](int note){const auto [denominator,kind]=notes[std::size_t(note-1)];return 4.*kind/denominator;};
    const auto capture=[&](const DelaySettings& d,double seconds,std::optional<double> tempo,RenderQuality quality=RenderQuality::realtime){
        RoutedSynthEngine e;CHECK(e.setPatch(sided(impulsePatch(),d,-1))&&e.prepare(rate,512,quality));
        return render(e,int(seconds*rate),impulseEvents,512,tempo);};
    auto dry=wetDelay(.1f);dry.mix=0;const double zero=double(loudestSample(capture(dry,.1,std::nullopt)[0]));
    // Each step's first echo lands at 60 * beats / BPM; a step ignores `delay.beats`.
    double worst=0;
    for(double bpm:{120.,97.})for(int note=1;note<=21;++note){
        const double period=60*noteBeats(note)/bpm;
        worst=std::max(worst,std::abs(double(loudestSample(capture(syncedDelay(note),period+.02,bpm)[0]))-zero-period*rate));
    }
    const auto quarterShort=capture(syncedDelay(14,.1f),.55,120.),quarterLong=capture(syncedDelay(14,1),.55,120.);
    CHECK(worst<=1&&peak(quarterShort)>0&&difference(quarterShort,quarterLong)==0);
    std::cout<<"DELAY_STEREO_NOTE placement_worst_samples="<<worst<<" beats_ignored_difference="<<difference(quarterShort,quarterLong)<<'\n';
    // Beats keeps today's float product of `delay.beats`: 0.37 at 97 BPM, from the build before NOTE.
    const std::array<std::uint64_t,2> expected{6701492305304001289ull,3175403813586833461ull};std::size_t index=0;
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto d=syncedDelay(0,.37f);d.feedback=.4f;const auto a=capture(d,.8,97.,quality);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"DELAY_STEREO_NOTE_BEATS_FINGERPRINT quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
    // At 60 BPM a 1/1 dotted note, 6 s, is held at the line's 4 s. Without a host tempo the wet stops
    // and only the dry voice passes, as at Beats.
    const double held=double(loudestSample(capture(syncedDelay(21),4.05,60.)[0]))-zero;
    const auto untimed=capture(syncedDelay(21),.3,std::nullopt),dryOnly=capture(dry,.3,std::nullopt);
    CHECK(std::abs(held-4*rate)<=1&&peak(dryOnly)>0&&difference(untimed,dryOnly)==0);
    std::cout<<"DELAY_STEREO_NOTE_HELD samples="<<held<<" untimed_vs_dry_difference="<<difference(untimed,dryOnly)<<'\n';
    // Free time ignores NOTE.
    double unsynced=0;const auto plain=capture(wetDelay(.1f),.15,120.);
    for(int note=1;note<=21;++note){auto d=wetDelay(.1f);d.note=note;unsynced=std::max(unsynced,difference(capture(d,.15,120.),plain));}
    CHECK(peak(plain)>0&&unsynced==0);
    // A NOTE change across blocks moves the time as the same Beats change does, sample for sample.
    const double f0=1000;int key=0;const auto source=driveStimulus(f0,driveLevel(f0,tapeReference*std::pow(10.,-12./20),rate),key);
    const auto moved=[&](bool byNote){
        const auto start=syncedDelay(byNote?11:0);auto p=taped(source,start);
        RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,RenderQuality::realtime));
        auto a=render(e,int(.4*rate),{{0,on(key)}},512,120.0);
        if(byNote)p.effects[0].delay.note=14;else p.effects[0].delay.beats=1;
        CHECK(e.setPatch(p));const auto b=render(e,int(.6*rate),{},512,120.0);
        for(std::size_t ch=0;ch<2;++ch)a[ch].insert(a[ch].end(),b[ch].begin(),b[ch].end());
        return a;};
    const auto byNote=moved(true),byBeats=moved(false);
    double step=0;for(const auto& channel:byNote)for(std::size_t n=std::size_t(.4*rate);n<channel.size();++n)step=std::max(step,std::abs(double(channel[n])-channel[n-1]));
    CHECK(peak(byNote)>0&&difference(byNote,byBeats)==0);
    std::cout<<"DELAY_STEREO_NOTE_FREE difference="<<unsynced<<" note_change_largest_step="<<step<<" note_vs_beats_difference="<<difference(byNote,byBeats)<<'\n';
}
// F12.26 EQ. The grid is the F12.26 candidate comparison's: every band type
// over its corner range, gains and Q, against the RBJ analog prototypes.
RoutedPatch equalized(RoutedPatch p,const EqSettings& eq,AlgorithmRevision revision)
{
    add(p,EffectKind::eq);p.effects[0].eq=eq;p.effects[0].algorithmRevision=revision;
    p.compatibility.effects[0][eqEffectBank]=revision;
    return p;
}
enum class EqBand { lowShelf, bell, highShelf };
EqSettings oneBand(EqBand band,const EqBandSettings& b)
{ EqSettings eq;(band==EqBand::lowShelf?eq.lowShelf:band==EqBand::bell?eq.bell:eq.highShelf)=b;return eq; }
double analogEqDb(EqBand band,const EqBandSettings& b,double hz)
{
    const double A=std::pow(10.,double(b.gainDb)/40),q=b.q,r=std::sqrt(A)/q;
    const std::complex<double> s(0,hz/double(b.frequencyHz));
    const auto h=band==EqBand::bell?(s*s+s*(A/q)+1.)/(s*s+s/(A*q)+1.)
        :band==EqBand::highShelf?A*(A*s*s+r*s+1.)/(s*s+r*s+A):A*(s*s+r*s+A)/(A*s*s+r*s+1.);
    return 20*std::log10(std::abs(h));
}
double eqBiquadDb(const std::array<double,5>& c,double hz,double rate)
{
    const auto z=std::polar(1.0,-2*std::numbers::pi*hz/rate);
    return 20*std::log10(std::abs((c[0]+z*(c[1]+z*c[2]))/(1.0+z*(c[3]+z*c[4]))));
}
bool juryStable(const std::array<double,5>& c)
{ return std::all_of(c.begin(),c.end(),[](double v){return std::isfinite(v);})&&1+c[3]+c[4]>0&&1-c[3]+c[4]>0&&1-c[4]>0; }
struct EqRange { EqBand band; float lo,hi; };
constexpr EqRange eqRanges[]{{EqBand::lowShelf,20,2000},{EqBand::bell,20,20000},{EqBand::highShelf,1000,20000}};
constexpr float eqGains[]{-18,-12,-6,-3,-1,1,3,6,12,18},eqQs[]{.25f,.5f,.70710678f,1,2,4};
std::vector<float> eqCorners(const EqRange& r)
{
    std::vector<float> f;for(int i=0;i<=24;++i)f.push_back(float(r.lo*std::pow(double(r.hi)/r.lo,i/24.)));
    for(float extra:{8000.f,10000.f,12000.f,15000.f,18000.f})if(extra>=r.lo&&extra<=r.hi)f.push_back(extra);
    return f;
}
void modernEqAdmission()
{
    section("modern_eq_admission_and_fresh_instance_default");
    CHECK(supportsEffectAlgorithmRevision(eqEffectBank,modernEqAlgorithmRevision)&&!supportsEffectAlgorithmRevision(eqEffectBank,3));
    for(const auto& banks:newInstanceKernelCompatibility().effects)CHECK(banks[eqEffectBank]==modernEqAlgorithmRevision);
    const EqSettings shaped{{120,6,.70710678f},{3000,-9,2},{12000,9,.70710678f}};
    CHECK(validateRoutedPatch(equalized(impulsePatch(),shaped,2)));
    auto mismatched=equalized(impulsePatch(),shaped,2);mismatched.compatibility.effects[0][eqEffectBank]=1;CHECK(!validateRoutedPatch(mismatched));
    mismatched=equalized(impulsePatch(),shaped,2);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
    CHECK(!validateRoutedPatch(equalized(impulsePatch(),shaped,3)));
    std::array<std::array<double,5>,3> c{};CHECK(!detail::eqBandCoefficients(shaped,44100,3,c));
    // Zero-gain bands are exact identities in both revisions.
    for(double rate:{8000.,44100.,96000.,768000.})for(const AlgorithmRevision revision:{AlgorithmRevision{1},AlgorithmRevision{2}}){
        CHECK(detail::eqBandCoefficients(EqSettings{},rate,revision,c));
        for(const auto& band:c)CHECK((band==std::array<double,5>{1,0,0,0,0}));
    }
}
void modernEqGrid()
{
    section("modern_eq_grid_against_analog_prototypes");
    // Each setting's worst error to 20 kHz against its prototype, revision 2
    // beside revision 1, at the realtime and higher-quality internal rates.
    for(double rate:{44100.,48000.,88200.,96000.}){
        const double top=std::min(20000.,.49*rate);std::vector<double> grid;for(int i=0;i<=300;++i)grid.push_back(20*std::pow(top/20,i/300.));
        for(const auto& range:eqRanges){
            const auto band=std::size_t(range.band);
            double worst=0,practical=0,practical16=0,legacyWorst=0,regression=-1e9;int undesigned=0;std::string regressedAt;
            for(float f0:eqCorners(range))for(float g:eqGains)for(float q:eqQs){
                const EqBandSettings b{f0,g,q};std::array<std::array<double,5>,3> modern{},legacy{};
                if(!detail::eqBandCoefficients(oneBand(range.band,b),rate,modernEqAlgorithmRevision,modern)
                    ||!detail::eqBandCoefficients(oneBand(range.band,b),rate,legacyAlgorithmRevision,legacy)){++undesigned;continue;}
                double e2=0,e2at16=0,e1=0;
                for(double hz:grid){const double analog=analogEqDb(range.band,b,hz),m=std::abs(eqBiquadDb(modern[band],hz,rate)-analog);
                    e2=std::max(e2,m);e1=std::max(e1,std::abs(eqBiquadDb(legacy[band],hz,rate)-analog));if(hz<=16000)e2at16=std::max(e2at16,m);}
                worst=std::max(worst,e2);legacyWorst=std::max(legacyWorst,e1);
                if(e2-e1>regression){regression=e2-e1;regressedAt="f0="+std::to_string(f0)+" g="+std::to_string(g)+" q="+std::to_string(q);}
                if(std::abs(g)<=12&&q<=2){practical=std::max(practical,e2);practical16=std::max(practical16,e2at16);}
            }
            CHECK(undesigned==0);CHECK(regression<=.05);CHECK(practical<=1.5);CHECK(practical16<=1.25);CHECK(worst<=3.5);
            std::cout<<"MODERN_EQ_GRID rate="<<rate<<" band="<<int(band)<<" worst_db="<<worst<<" practical_db="<<practical<<" practical16_db="<<practical16
                <<" regression_db="<<regression<<" at "<<regressedAt<<" legacy_worst_db="<<legacyWorst<<" undesigned="<<undesigned<<'\n';
        }
    }
    for(double rate:{8000.,22050.,44100.,48000.,88200.,96000.,192000.,384000.,768000.}){
        int count=0,unstable=0;
        for(const auto& range:eqRanges)for(float f0:eqCorners(range))for(float g:eqGains)for(float q:eqQs){
            std::array<std::array<double,5>,3> c{};++count;
            bool stable=detail::eqBandCoefficients(oneBand(range.band,{f0,g,q}),rate,modernEqAlgorithmRevision,c);
            for(const auto& band:c)stable&=juryStable(band);unstable+=!stable;
        }
        CHECK(unstable==0);std::cout<<"MODERN_EQ_STABLE rate="<<rate<<" settings="<<count<<" unstable="<<unstable<<'\n';
    }
}
void modernEqResponseThroughEngine()
{
    section("modern_eq_engine_response");
    // Steady sines through the F12.18 audit's three cases match the revision-2
    // coefficients; the measurement first shows revision 1's cramping.
    struct Case { const char* name; EqBand band; EqBandSettings b; };
    const Case cases[]{{"bell_10k_+12_q1",EqBand::bell,{10000,12,1}},{"bell_3k_+12_q2",EqBand::bell,{3000,12,2}},
                       {"high_shelf_8k_+12_q0.71",EqBand::highShelf,{8000,12,.70710678f}}};
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double internal=rate*(quality==RenderQuality::higher?2:1);const int from=int(.3*rate),count=1<<14;
        const float level=driveLevel(1000,.02,rate);
        for(double f0:{1000.,5000.,10000.,15000.,18000.}){
            int key=0;const auto tone=driveStimulus(f0,level,key);
            const double dry=toneAmplitude(modulatedRender(tone,key,rate,quality,from,count),f0,rate);CHECK(dry>1e-4);
            for(const auto& c:cases){
                const auto eq=oneBand(c.band,c.b);std::array<std::array<double,5>,3> coefficients{};
                const bool designed=detail::eqBandCoefficients(eq,internal,modernEqAlgorithmRevision,coefficients);CHECK(designed);
                const bool audited=rate==44100&&quality==RenderQuality::realtime;
                for(const AlgorithmRevision revision:{AlgorithmRevision{2},AlgorithmRevision{1}}){
                    if(revision==1&&!audited)continue;
                    if(revision==2&&!designed)continue;
                    const double measured=20*std::log10(toneAmplitude(modulatedRender(equalized(tone,eq,revision),key,rate,quality,from,count),f0,rate)/dry);
                    const double predicted=revision==2?eqBiquadDb(coefficients[std::size_t(c.band)],f0,internal):std::numeric_limits<double>::quiet_NaN();
                    if(revision==2)CHECK(std::abs(measured-predicted)<=(quality==RenderQuality::higher?.1:.05));
                    if(audited&&c.b.frequencyHz==10000&&f0>=15000)CHECK(revision==2?measured>=(f0==15000?5.5:3.5):measured<(f0==15000?4.:2.));
                    std::cout<<"MODERN_EQ_ENGINE revision="<<int(revision)<<" rate="<<rate<<" quality="<<int(quality)<<" case="<<c.name<<" f0="<<f0
                        <<" measured_db="<<measured<<" predicted_db="<<predicted<<" analog_db="<<analogEqDb(c.band,c.b,f0)<<'\n';
                }
            }
        }
    }
}
void modernEqKeepLaw()
{
    section("modern_eq_keep_revision1_ramp_and_law");
    // Below 2 kHz both designs meet the prototypes closely, so revision 2
    // follows revision 1 through the same coefficient ramp and a live edit.
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        int key=0;auto tone=driveStimulus(200,driveLevel(200,.02,rate),key);tone.synth.layers[0].ampEnvelope.attack=.02f;const int count=int(.6*rate),edit=int(.3*rate);
        const auto run=[&](AlgorithmRevision revision){
            auto patch=equalized(tone,{{100,6,.70710678f},{400,-6,1},{2000,3,.70710678f}},revision);
            RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));
            auto output=render(engine,edit,{{0,on(key)}},173);
            patch.effects[0].eq={{150,-6,1},{300,9,2},{1500,-3,.5f}};CHECK(engine.setPatch(patch));const auto rest=render(engine,count-edit,{},173);
            for(std::size_t ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),rest[ch].begin(),rest[ch].end());
            return output;};
        const auto modern=run(2),legacy=run(1);const double error=difference(modern,legacy)/std::max(peak(legacy),1e-30);
        CHECK(finite(modern)&&peak(modern)>1e-4&&error<=1e-2);
        std::cout<<"MODERN_EQ_LAW rate="<<rate<<" quality="<<int(quality)<<" relative_error="<<error<<'\n';
    }
}
void modernEqEngine()
{
    section("modern_eq_live_partition_reset_bypass_tail_callbacks");
    const EqSettings shaped{{120,6,.70710678f},{3000,-9,2},{12000,9,.70710678f}};
    for(double rate:{8000.,44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto dry=impulsePatch();const std::vector<Event> phrase{{17,on()},{4400,off()}};
        const auto capture=[&](const RoutedPatch& p,int partition){RoutedSynthEngine e;CHECK(e.setPatch(p)&&e.prepare(rate,512,quality));return render(e,8192,phrase,partition);};
        const auto input=capture(dry,512);
        CHECK(difference(capture(equalized(dry,EqSettings{},2),512),input)==0);
        const auto patch=equalized(dry,shaped,2);
        RoutedSynthEngine a;CHECK(a.setPatch(patch)&&a.prepare(rate,512,quality));
        const auto coarse=render(a,8192,phrase,512),fine=capture(patch,61);
        CHECK(finite(coarse)&&energy(coarse)>1e-12&&difference(coarse,fine)==0&&difference(coarse,input)>0);
        a.reset();CHECK(energy(render(a,2048))==0);
        a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
        auto bypassed=patch;bypassed.effects[0].bypassed=true;CHECK(difference(capture(bypassed,512),input)==0);
        const auto live=[&](int partition){
            RoutedSynthEngine engine;auto p=equalized(flat(),{{20,18,4},{440,18,4},{20000,-18,4}},2);
            CHECK(engine.setPatch(p)&&engine.prepare(rate,512,quality));Audio result;
            const auto append=[&](int samples,const std::vector<Event>& events={}){const auto x=render(engine,samples,events,partition);
                for(int ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),x[ch].begin(),x[ch].end());};
            append(int(rate*.04),{{0,on(69)},{73,on(93)}});
            for(int n=0;n<12;++n){auto changed=p;
                changed.effects[0].eq={{n%2?2000.f:20.f,n%2?-18.f:18.f,n%2?.25f:4.f},{n%2?20000.f:20.f,n%2?18.f:-18.f,4},{n%2?1000.f:20000.f,n%2?-18.f:18.f,4}};
                CHECK(engine.setPatch(changed));p=changed;append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(changed));append(37);}
            engine.reset();append(int(rate*.03),{{0,on(65)},{201,off(65)}});
            CHECK(finite(result)&&peak(result)<10&&energy(result)>1e-8);return result;};
        const auto x=live(512),y=live(64),z=live(73);CHECK(difference(x,y)==0&&difference(x,z)==0);
        std::cout<<"MODERN_EQ_STATE rate="<<rate<<" quality="<<int(quality)<<" energy="<<energy(coarse)<<" live_peak="<<peak(x)<<'\n';
    }
    // The slowest pole at the lowest rate: the revision-2 bound.
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);
        const EqSettings slowest{{160,0,.70710678f},{20,18,4},{8000,0,.70710678f}};
        const double seconds=detail::eqTailSeconds(slowest,internal,modernEqAlgorithmRevision);
        CHECK(std::isfinite(seconds)&&seconds>.1&&seconds<20);if(!std::isfinite(seconds)||seconds<=0||seconds>=20)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(equalized(impulsePatch(),slowest,2))&&engine.prepare(rate,512,quality));
        const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+int(rate*.02),impulseEvents,73);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[std::size_t(n)])));
        CHECK(finite(tail)&&peak(tail)>1e-6&&residual<=peak(tail)*1e-6);
        std::cout<<"MODERN_EQ_TAIL quality="<<int(quality)<<" seconds="<<seconds<<" legacy_seconds="<<detail::eqTailSeconds(slowest,internal)<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){add(busy,EffectKind::eq,slot%5);auto& e=busy.effects[std::size_t(slot)];
        e.eq=shaped;e.algorithmRevision=2;busy.compatibility.effects[std::size_t(slot)][eqEffectBank]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256,RenderQuality::higher);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    bool accepted=true;float loudest=0;engine.render(audio,held,120);
    for(int block=0;block<48;++block){auto& e=busy.effects[std::size_t(block%8)];
        e.eq={{block%2?20.f:2000.f,block%2?-18.f:18.f,block%2?.25f:4.f},{block%2?20000.f:20.f,block%2?18.f:-18.f,4},{block%2?1000.f:20000.f,block%2?-18.f:18.f,block%2?4.f:.25f}};
        accepted&=engine.setPatch(busy);engine.render(audio,empty,120);loudest=std::max(loudest,audio.getMagnitude(0,256));if(block%17==0)engine.reset();}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_EQ_CALLBACKS layers=4 notes=16 effects=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_EQ_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&loudest>0);
}
void legacyEqFingerprint()
{
    section("legacy_eq_fingerprint");
    // Recalled sounds replay revision 1's bilinear bands; protect its exact float output.
    auto shaped=impulsePatch();shaped.synth.layers[0].level=.1f;add(shaped,EffectKind::eq);shaped.effects[0].eq={{120,6,.70710678f},{3000,-9,2},{12000,9,.70710678f}};
    auto extreme=shaped;extreme.effects[0].eq={{1500,-12,4},{16000,15,.5f},{18000,-6,1.5f}};
    // Shaped then extreme; realtime and higher quality at 44.1, 48 and 96 kHz, from the pre-F12.26 build.
    const std::array<std::uint64_t,12> expected{
        17724625065297019489ull,17000352507522860025ull,12695983303460682285ull,8125539879198778493ull,13405678699194679593ull,5317880079973262437ull,
        7611757905012166553ull,4202602372109127973ull,17929877366420448485ull,14143203202764304717ull,4880794200695136541ull,17025692226675390037ull};
    std::size_t index=0;
    for(const auto* patch:{&shaped,&extreme})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_EQ_FINGERPRINT extreme="<<(patch==&extreme)<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
// Filter revision 2 is measured through the public engine on the drive's
// clean wavetable sines. It runs its pre-drive, state-variable filter and mix
// at four times the rate through drive revision 2's halfbands, so steady sines
// follow the analog prototype at the clamped corner and the pre-drive's
// aliases fall outside the band.
RoutedPatch filtered(RoutedPatch p,const FilterSettings& f,AlgorithmRevision revision,float liftDb=0)
{
    std::size_t slot=0;
    if(liftDb>0){add(p,EffectKind::stereo);p.effects[0].stereo={liftDb,0,1,StereoChannelMode::stereo};slot=1;}
    add(p,EffectKind::filter);p.effects[slot].filter=f;p.effects[slot].algorithmRevision=revision;
    p.compatibility.effects[slot][filterEffectBank]=revision;
    return p;
}
double analogFilterDb(const FilterSettings& f,double hz,double rate)
{
    const double corner=std::min(double(f.cutoffHz),.45*rate),q=f.resonance;const std::complex<double> s(0,hz/corner),d=s*s+s/q+1.;
    const auto h=f.mode==FilterFxMode::lowpass?1./d:f.mode==FilterFxMode::highpass?s*s/d:(s/q)/d;
    return 20*std::log10(std::abs((1.0-f.mix)+double(f.mix)*h));
}
void modernFilterAdmission()
{
    section("modern_filter_admission_and_fresh_instance_default");
    CHECK(supportsEffectAlgorithmRevision(filterEffectBank,modernFilterAlgorithmRevision)&&!supportsEffectAlgorithmRevision(filterEffectBank,3));
    for(const auto& banks:newInstanceKernelCompatibility().effects)CHECK(banks[filterEffectBank]==modernFilterAlgorithmRevision);
    int key=0;const auto base=driveStimulus(440,.5f,key);
    for(auto mode:{FilterFxMode::lowpass,FilterFxMode::bandpass,FilterFxMode::highpass})CHECK(validateRoutedPatch(filtered(base,{mode,2000,2,6,.5f},2)));
    auto mismatched=filtered(base,{FilterFxMode::lowpass,2000,2,6,.5f},2);mismatched.compatibility.effects[0][filterEffectBank]=1;CHECK(!validateRoutedPatch(mismatched));
    mismatched=filtered(base,{FilterFxMode::lowpass,2000,2,6,.5f},2);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
    CHECK(!validateRoutedPatch(filtered(base,{FilterFxMode::lowpass,2000,2,6,.5f},3)));
}
void modernFilterResponse()
{
    section("modern_filter_small_signal_response");
    // The mix runs inside the oversampled domain, so partial mixes follow
    // (1-mix)+mix*H as well and mix 0 is the halfbands' flat, delayed path,
    // not the input itself; revision 1 shows the bilinear cramping first.
    struct Case{const char* name;FilterSettings f;};
    const Case cases[]{{"lowpass_10k",{FilterFxMode::lowpass,10000,.70710678f,0,1}},{"lowpass_4k_q2",{FilterFxMode::lowpass,4000,2,0,1}},
                       {"bandpass_8k_q2",{FilterFxMode::bandpass,8000,2,0,1}},{"highpass_12k",{FilterFxMode::highpass,12000,.70710678f,0,1}},
                       {"highpass_1k_mix_half",{FilterFxMode::highpass,1000,.70710678f,0,.5f}},{"lowpass_20k_q5_clamped",{FilterFxMode::lowpass,20000,5,0,1}},
                       {"mix0",{FilterFxMode::lowpass,1000,.70710678f,0,0}}};
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double internal=rate*(quality==RenderQuality::higher?2:1);const int from=int(.3*rate),count=1<<14;
        for(double hz:{1000.,5000.,10000.,15000.,18000.}){
            int key=0;const auto tone=driveStimulus(hz,driveLevel(hz,.001,rate),key);
            const double dry=toneAmplitude(modulatedRender(tone,key,rate,quality,from,count),hz,rate);CHECK(dry>1e-5);
            for(const auto& c:cases){
                const double analog=analogFilterDb(c.f,hz,internal);if(analog<-30)continue;
                const double measured=20*std::log10(toneAmplitude(modulatedRender(filtered(tone,c.f,2),key,rate,quality,from,count),hz,rate)/dry);
                CHECK(std::abs(measured-analog)<=(c.f.mix==0?.01:hz<=15000?.5:1.));
                const bool audited=rate==44100&&quality==RenderQuality::realtime&&std::string_view(c.name)=="lowpass_10k"&&hz>=15000;
                double legacy=std::numeric_limits<double>::quiet_NaN();
                if(audited){legacy=20*std::log10(toneAmplitude(modulatedRender(filtered(tone,c.f,1),key,rate,quality,from,count),hz,rate)/dry);CHECK(legacy<analog-3);}
                std::cout<<"MODERN_FILTER_RESPONSE rate="<<rate<<" quality="<<int(quality)<<" case="<<c.name<<" hz="<<hz<<" measured_db="<<measured
                         <<" analog_db="<<analog<<" error_db="<<measured-analog<<" legacy_db="<<legacy<<'\n';
            }
        }
    }
}
void modernFilterSaturation()
{
    section("modern_filter_saturation_alias_and_law");
    for(double rate:{44100.,48000.}){
        const int from=int(.25*rate),count=1<<16;int key=0;
        const FilterSettings heavy{FilterFxMode::lowpass,20000,.70710678f,24,1};
        for(double f0:{1760.,3520.,7040.}){
            const auto lifted=driveStimulus(f0,driveLevel(f0,.02,rate),key);
            const double modern=driveAliasDb(driveRender(filtered(lifted,heavy,2,12),key,rate,from,count),f0,rate);
            const double legacy=driveAliasDb(driveRender(filtered(lifted,heavy,1,12),key,rate,from,count),f0,rate);
            CHECK(modern<=-80);if(f0==7040)CHECK(legacy>-60);
            std::cout<<"MODERN_FILTER_ALIAS rate="<<rate<<" f0="<<f0<<" peak=.02 lift_db=12 drive=24 alias16k_db="<<modern<<" legacy_alias16k_db="<<legacy<<'\n';
        }
        // The pre-drive law is unchanged: every harmonic of 1760 Hz that
        // carries energy lies well below the corner, where both revisions agree.
        const auto voice=driveStimulus(1760,driveLevel(1760,.02,rate),key);
        for(float drive:{6.f,12.f,24.f}){
            const FilterSettings f{FilterFxMode::lowpass,20000,.70710678f,drive,1};
            const double modern=driveDistortionDb(driveRender(filtered(voice,f,2,12),key,rate,from,count),1760,rate);
            const double legacy=driveDistortionDb(driveRender(filtered(voice,f,1,12),key,rate,from,count),1760,rate);
            CHECK(std::abs(modern-legacy)<=.5);
            std::cout<<"MODERN_FILTER_DISTORTION rate="<<rate<<" f0=1760 peak=.02 lift_db=12 drive="<<drive<<" modern_db="<<modern<<" legacy_db="<<legacy<<'\n';
        }
    }
}
void modernFilterPartitionResetAndCallbacks()
{
    section("modern_filter_partition_reset_quality_tail_callbacks");
    int key=0;const auto patch=filtered(driveStimulus(220,1,key),{FilterFxMode::bandpass,1800,4,18,.6f},2,6);
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine a,b;CHECK(a.setPatch(patch)&&b.setPatch(patch));CHECK(a.prepare(rate,512,quality)&&b.prepare(rate,512,quality));
        const std::vector<Event> phrase{{17,on(key)},{4001,off(key)}};
        const auto coarse=render(a,8192,phrase,512),fine=render(b,8192,phrase,61);
        CHECK(finite(coarse)&&energy(coarse)>1e-10&&difference(coarse,fine)==0);
        a.reset();CHECK(energy(render(a,2048))==0);
        a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
        std::cout<<"MODERN_FILTER_STATE rate="<<rate<<" quality="<<int(quality)<<" energy="<<energy(coarse)<<" partition_error="<<difference(coarse,fine)<<'\n';
    }
    // The slowest pole at the lowest rate: the revision-2 bound covers the
    // filter at four times the rate and the halfbands.
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);
        const FilterSettings slowest{FilterFxMode::lowpass,20,10,0,1};
        const double seconds=detail::filterTailSeconds(slowest,internal,modernFilterAlgorithmRevision);
        CHECK(std::isfinite(seconds)&&seconds>.1&&seconds<20);if(!std::isfinite(seconds)||seconds<=0||seconds>=20)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(filtered(impulsePatch(),slowest,2))&&engine.prepare(rate,512,quality));
        const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+int(rate*.02),impulseEvents,73);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[std::size_t(n)])));
        CHECK(finite(tail)&&peak(tail)>1e-6&&residual<=peak(tail)*1e-6);
        std::cout<<"MODERN_FILTER_TAIL quality="<<int(quality)<<" seconds="<<seconds<<" legacy_seconds="<<detail::filterTailSeconds(slowest,internal)<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){add(busy,EffectKind::filter,slot%5);auto& e=busy.effects[std::size_t(slot)];
        e.filter={FilterFxMode::lowpass,3000,2,12,.8f};e.algorithmRevision=2;busy.compatibility.effects[std::size_t(slot)][filterEffectBank]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256,RenderQuality::higher);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    bool accepted=true;float loudest=0;engine.render(audio,held,120);
    for(int block=0;block<48;++block){auto& e=busy.effects[std::size_t(block%8)];
        e.filter={FilterFxMode(block%3),block%2?20.f:20000.f,block%2?10.f:.5f,float(block%25),block%2?1.f:.3f};
        accepted&=engine.setPatch(busy);engine.render(audio,empty,120);loudest=std::max(loudest,audio.getMagnitude(0,256));if(block%17==0)engine.reset();}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_FILTER_CALLBACKS layers=4 notes=16 effects=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_FILTER_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&loudest>0);
}
void legacyFilterFingerprint()
{
    section("legacy_filter_fingerprint");
    // Recalled sounds replay revision 1's bilinear filter and direct pre-drive; protect its exact float output.
    auto veil=flat();veil.synth.layers[0].level=.8f;add(veil,EffectKind::filter);veil.effects[0].filter={FilterFxMode::lowpass,4000,.70710678f,0,1};
    auto heavy=veil;heavy.effects[0].filter={FilterFxMode::bandpass,9000,6,18,.7f};
    // Veil Current's settings then a driven resonant bandpass; realtime and higher quality at 44.1, 48 and 96 kHz, from the pre-F12.29 build.
    const std::array<std::uint64_t,12> expected{
        7454854989477531533ull,4150990048452541005ull,17679721396148677957ull,2349563176838502541ull,6052026990133580733ull,11663401562202857961ull,
        11474792940000388941ull,2963992548226553025ull,1470163638766153797ull,2942098700237583473ull,13432636869374406869ull,14412462221985256681ull};
    std::size_t index=0;
    for(const auto* patch:{&veil,&heavy})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_FILTER_FINGERPRINT heavy="<<(patch==&heavy)<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
// Frequency-splitter revision 2 is a fourth-order Linkwitz-Riley crossover: each
// band is a Butterworth section squared, the bands sum to an allpass, and that
// sum is the dry path, so neutral bands and partial mixes stay flat. Measured
// through the public engine on the drive's clean wavetable sines.
RoutedPatch split(RoutedPatch p,const FrequencySplitSettings& s,AlgorithmRevision revision)
{
    add(p,EffectKind::frequencySplit);p.effects[0].frequencySplit=s;p.effects[0].algorithmRevision=revision;
    p.compatibility.effects[0][frequencySplitEffectBank]=revision;
    return p;
}
void modernFrequencySplitAdmission()
{
    section("modern_frequency_split_admission_and_fresh_instance_default");
    CHECK(supportsEffectAlgorithmRevision(frequencySplitEffectBank,modernFrequencySplitAlgorithmRevision)&&!supportsEffectAlgorithmRevision(frequencySplitEffectBank,3));
    for(const auto& banks:newInstanceKernelCompatibility().effects)CHECK(banks[frequencySplitEffectBank]==modernFrequencySplitAlgorithmRevision);
    int key=0;const auto base=driveStimulus(440,.5f,key);const FrequencySplitSettings s{1000,-6,false,.3f,3,false,-.2f,.7f};
    CHECK(validateRoutedPatch(split(base,s,2)));
    auto mismatched=split(base,s,2);mismatched.compatibility.effects[0][frequencySplitEffectBank]=1;CHECK(!validateRoutedPatch(mismatched));
    mismatched=split(base,s,2);mismatched.effects[0].algorithmRevision=1;CHECK(!validateRoutedPatch(mismatched));
    CHECK(!validateRoutedPatch(split(base,s,3)));
}
void modernFrequencySplitResponse()
{
    section("modern_frequency_split_isolation_sum_and_law");
    for(double rate:{44100.,48000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double internal=rate*(quality==RenderQuality::higher?2:1);const int from=int(.3*rate),count=1<<14;
        const auto level=[&](double hz,const FrequencySplitSettings& s,AlgorithmRevision revision){
            int key=0;const auto tone=driveStimulus(hz,driveLevel(hz,.01,rate),key);
            const double dry=toneAmplitude(modulatedRender(tone,key,rate,quality,from,count),hz,rate);CHECK(dry>1e-5);
            return 20*std::log10(toneAmplitude(modulatedRender(split(tone,s,revision),key,rate,quality,from,count),hz,rate)/dry);};
        const FrequencySplitSettings lowMuted{1000,0,true,0,0,false,0,1},highMuted{1000,0,false,0,0,true,0,1};
        const double at500=level(500,lowMuted,2),at250=level(250,lowMuted,2),at2k=level(2000,highMuted,2),legacy500=level(500,lowMuted,1);
        CHECK(at500<=-24&&at250<=-46&&at2k<=-24&&legacy500>-8);
        const double lowAlone=level(1000,highMuted,2),highAlone=level(1000,lowMuted,2);
        CHECK(std::abs(lowAlone+6.0206)<=.1&&std::abs(highAlone+6.0206)<=.1);
        std::cout<<"MODERN_FREQUENCY_SPLIT_ISOLATION rate="<<rate<<" quality="<<int(quality)<<" low_muted_500_db="<<at500<<" low_muted_250_db="<<at250
                 <<" high_muted_2000_db="<<at2k<<" legacy_low_muted_500_db="<<legacy500<<" low_alone_1000_db="<<lowAlone<<" high_alone_1000_db="<<highAlone<<'\n';
        for(double hz:{100.,1000.,5000.,10000.,18000.})for(float mix:{1.f,.5f}){
            const double neutral=level(hz,{1000,0,false,0,0,false,0,mix},2);CHECK(std::abs(neutral)<=.01);
            std::cout<<"MODERN_FREQUENCY_SPLIT_FLAT rate="<<rate<<" quality="<<int(quality)<<" hz="<<hz<<" mix="<<mix<<" level_db="<<neutral<<'\n';
        }
        // Gain and tone keep revision 1's law on the LR4 band: both prewarped at 1 kHz.
        for(double hz:{100.,400.}){
            const std::complex<double> s(0,std::tan(std::numbers::pi*hz/internal)/std::tan(std::numbers::pi*1000/internal)),b=s*s+std::sqrt(2.)*s+1.;
            const double expected=20*std::log10(std::abs(std::pow(10.,6./20)/(b*b)*(1.+.5*(1.-2./(1.+s)))));
            const double measured=level(hz,{1000,6,false,.5f,0,true,0,1},2);CHECK(std::abs(measured-expected)<=.1);
            std::cout<<"MODERN_FREQUENCY_SPLIT_LAW rate="<<rate<<" quality="<<int(quality)<<" hz="<<hz<<" measured_db="<<measured<<" expected_db="<<expected<<'\n';
        }
    }
}
void modernFrequencySplitPartitionResetAndCallbacks()
{
    section("modern_frequency_split_partition_reset_quality_tail_callbacks");
    int key=0;const auto patch=split(driveStimulus(220,1,key),{700,-4,false,.6f,5,false,-.4f,.8f},2);
    for(double rate:{44100.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine a,b;CHECK(a.setPatch(patch)&&b.setPatch(patch));CHECK(a.prepare(rate,512,quality)&&b.prepare(rate,512,quality));
        const std::vector<Event> phrase{{17,on(key)},{4001,off(key)}};
        const auto coarse=render(a,8192,phrase,512),fine=render(b,8192,phrase,61);
        CHECK(finite(coarse)&&energy(coarse)>1e-10&&difference(coarse,fine)==0);
        a.reset();CHECK(energy(render(a,2048))==0);
        a.reset();CHECK(difference(render(a,8192,phrase,512),coarse)==0);
        std::cout<<"MODERN_FREQUENCY_SPLIT_STATE rate="<<rate<<" quality="<<int(quality)<<" energy="<<energy(coarse)<<" partition_error="<<difference(coarse,fine)<<'\n';
    }
    // The slowest sections at the lowest rate, both bands at full gain and either tone.
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(float tone:{-1.f,1.f}){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);
        const double seconds=detail::frequencySplitTailSeconds(internal,modernFrequencySplitAlgorithmRevision);
        CHECK(std::isfinite(seconds)&&seconds>.1&&seconds<20);if(!std::isfinite(seconds)||seconds<=0||seconds>=20)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(split(impulsePatch(),{20,12,false,tone,12,false,tone,1},2))&&engine.prepare(rate,512,quality));
        const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+int(rate*.02),impulseEvents,73);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[std::size_t(n)])));
        CHECK(finite(tail)&&peak(tail)>1e-6&&residual<=peak(tail)*1e-6);
        std::cout<<"MODERN_FREQUENCY_SPLIT_TAIL quality="<<int(quality)<<" tone="<<tone<<" seconds="<<seconds<<" legacy_seconds="<<detail::frequencySplitTailSeconds(internal)
                 <<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    auto busy=flat();for(int layer=0;layer<3;++layer)addLayer(busy);
    for(int slot=0;slot<8;++slot){add(busy,EffectKind::frequencySplit,slot%5);auto& e=busy.effects[std::size_t(slot)];
        e.frequencySplit={300.f+131.f*float(slot),-3,false,-.2f,2,false,.3f,.8f};e.algorithmRevision=2;busy.compatibility.effects[std::size_t(slot)][frequencySplitEffectBank]=2;}
    RoutedSynthEngine engine;const bool ready=engine.setPatch(busy)&&engine.prepare(48000,256,RenderQuality::higher);CHECK(ready);if(!ready)return;
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer held,empty;held.ensureSize(4096);for(int note=48;note<64;++note)held.addEvent(on(note),note-48);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
    bool accepted=true;float loudest=0;engine.render(audio,held,120);
    for(int block=0;block<48;++block){auto& e=busy.effects[std::size_t(block%8)];
        e.frequencySplit={block%2?20.f:20000.f,block%2?-24.f:12.f,block%3==0,block%2?-1.f:1.f,block%2?12.f:-24.f,block%3==1,block%2?1.f:-1.f,block%2?0.f:1.f};
        accepted&=engine.setPatch(busy);engine.render(audio,empty,120);loudest=std::max(loudest,audio.getMagnitude(0,256));if(block%17==0)engine.reset();}
    engine.reset();engine.render(audio,held,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
    std::cout<<"MODERN_FREQUENCY_SPLIT_CALLBACKS layers=4 notes=16 effects=8 callbacks=50 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
    std::cout<<"MODERN_FREQUENCY_SPLIT_CALLBACKS instrumentation unavailable\n";
#endif
    CHECK(accepted&&loudest>0);
}
void legacyFrequencySplitFingerprint()
{
    section("legacy_frequency_split_fingerprint");
    // Recalled sounds replay revision 1's complementary one-pole split; protect its exact float output.
    auto balance=flat();auto& voice=balance.synth.layers[0];voice.level=.8f;voice.oscillators[0].waveform=Waveform::saw;
    add(balance,EffectKind::frequencySplit);balance.effects[0].frequencySplit={300,-3,false,-.2f,2,false,.3f,.8f};
    auto muted=balance;muted.effects[0].frequencySplit={1000,0,true,0,12,false,1,1};
    // Band Balance's ordinary edit then a muted low band; realtime and higher quality at 44.1, 48 and 96 kHz, from the pre-F13.34 build.
    const std::array<std::uint64_t,12> expected{6100176385658614433ull,10391054687330573437ull,6305747857827587405ull,10028335272190445877ull,
        18424228757283954529ull,10738412035180696521ull,17449449914886246061ull,9149987486295501365ull,9018026194164897965ull,2259105153064335109ull,
        2291208225708350197ull,9555029550910623981ull};
    std::size_t index=0;
    for(const auto* patch:{&balance,&muted})for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(*patch)&&engine.prepare(rate,512,quality));
        const auto a=render(engine,8192,{{0,on()},{6000,off()}},64);
        std::uint64_t hash=14695981039346656037ull;
        for(const auto& channel:a)for(float x:channel)
        {const auto bits=std::bit_cast<std::uint32_t>(x);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255u;hash*=1099511628211ull;}}
        std::cout<<"LEGACY_FREQUENCY_SPLIT_FINGERPRINT muted="<<(patch==&muted)<<" rate="<<rate<<" quality="<<int(quality)<<" fnv="<<hash<<'\n';
        CHECK(finite(a)&&energy(a)>0&&hash==expected[index++]);
    }
}
void baseFrameMathLiveArguments()
{
    section("base_frame_math_nonzero_defaults_live_arguments_gain_stop_reset_characterization");
    auto initial=flat();for(int layer=0;layer<3;++layer)addLayer(initial);
    initial.outputProtection=false;initial.synth.masterLevel=.4f;
    for(std::size_t i=0;i<4;++i){auto& l=initial.synth.layers[i];l.level=.13f;l.pan=float(i)*.45f-.7f;
        l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.ampEnvelope.attack=.001f;l.ampEnvelope.decay=.01f;l.ampEnvelope.sustain=.8f;l.ampEnvelope.release=.03f;
        for(std::size_t j=0;j<2;++j){auto& o=l.oscillators[j];o.level=j?.23f:.7f;o.waveform=j?Waveform::pulse:Waveform::saw;o.tune=float(int(i)*3-int(j)*7);o.detune=float(int(i)*11-int(j)*17);o.position=.3f;}}
    add(initial,EffectKind::reverb);initial.effects[0].algorithmRevision=2;initial.compatibility.effects[0][2]=2;
    initial.effects[0].reverb={.4f,.5f,.3f,0,.2f};
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto phrase=[&](RoutedSynthEngine& engine,int partition){
            CHECK(engine.setPatch(initial)&&engine.prepare(rate,512,quality));Audio result;
            auto append=[&](int frames,const std::vector<Event>& events){const auto part=render(engine,frames,events,partition,120);for(std::size_t ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),part[ch].begin(),part[ch].end());};
            std::vector<Event> held;for(int note=48;note<64;++note)held.emplace_back(0,on(note,1,.6f));
            append(int(rate*.012),held);
            auto moved=initial;
            for(std::size_t i=0;i<4;++i){auto& l=moved.synth.layers[i];l.pan=-initial.synth.layers[i].pan;l.level=.06f+float(i)*.02f;
                for(std::size_t j=0;j<2;++j){l.oscillators[j].tune=initial.synth.layers[i].oscillators[j].tune+5;l.oscillators[j].detune=-initial.synth.layers[i].oscillators[j].detune;}}
            moved.synth.layers[3].enabled=false;CHECK(engine.setPatch(moved));append(int(rate*.0017),{});
            // Interrupt all live keys before their five millisecond ramp ends.
            auto reversed=initial;reversed.synth.masterLevel=.19f;for(auto& l:reversed.synth.layers){l.pan*=.4f;l.level=.21f;for(auto& o:l.oscillators){o.tune=-3;o.detune=25;}}
            CHECK(engine.setPatch(reversed));append(int(rate*.009),{});
            // Fixed pan coefficients with changed level/enable/master products.
            auto gainOnly=reversed;gainOnly.synth.masterLevel=.7f;for(std::size_t i=0;i<4;++i)gainOnly.synth.layers[i].level=.05f+float(i)*.06f;
            gainOnly.synth.layers[1].enabled=false;CHECK(engine.setPatch(gainOnly));append(int(rate*.009),{});
            // Equal derived pitch arguments have different ordinary inputs.
            auto equalArgument=gainOnly;for(auto& l:equalArgument.synth.layers)for(auto& o:l.oscillators){o.tune=.5f;o.detune=50;}
            CHECK(engine.setPatch(equalArgument));append(int(rate*.008),{});
            for(auto& l:equalArgument.synth.layers)for(auto& o:l.oscillators){o.tune=1;o.detune=0;}
            CHECK(engine.setPatch(equalArgument));append(int(rate*.008),{});
            // Repeated unchanged setPatch must not restart ramps or clocks.
            for(int n=0;n<11;++n){CHECK(engine.setPatch(equalArgument));append(37,{});}
            auto structural=equalArgument;structural.synth.layers[0].oscillators[0].waveform=Waveform::triangle;
            CHECK(engine.setPatch(structural));append(int(rate*.012),{});
            std::vector<Event> stopped{{17,juce::MidiMessage::midiStop()}};
            for(int note=55;note<59;++note){stopped.emplace_back(73,on(note,1,.5f));stopped.emplace_back(int(rate*.012),off(note));}
            append(int(rate*.045),stopped);
            engine.reset();CHECK(engine.setPatch(initial));engine.reset();append(int(rate*.016),{{23,on(64)},{int(rate*.009),off(64)}});
            return result;
        };
        RoutedSynthEngine fineEngine,coarseEngine,replayEngine;
        const auto fine=phrase(fineEngine,64),coarse=phrase(coarseEngine,512),replay=phrase(replayEngine,73);
        CHECK(finite(fine)&&energy(fine)>1e-9&&difference(fine,coarse)==0&&difference(fine,replay)==0);
        bool exact=true;std::uint64_t hash=14695981039346656037ull;
        for(std::size_t ch=0;ch<2;++ch)for(std::size_t n=0;n<fine[ch].size();++n){const auto bits=std::bit_cast<std::uint32_t>(fine[ch][n]);
            exact&=bits==std::bit_cast<std::uint32_t>(coarse[ch][n])&&bits==std::bit_cast<std::uint32_t>(replay[ch][n]);
            for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255;hash*=1099511628211ull;}}
        CHECK(exact);
        if(const auto* directory=std::getenv("AIRACK_BASE_MATH_AUDIO_DIR")){
            const juce::File folder{juce::String(directory)};CHECK(folder.createDirectory().wasOk());
            const auto file=folder.getChildFile("base-math-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");
            std::vector<float> raw;for(const auto& ch:fine)raw.insert(raw.end(),ch.begin(),ch.end());CHECK(file.replaceWithData(raw.data(),raw.size()*sizeof(float)));
        }
        std::cout<<"BASE_MATH_RAW rate="<<rate<<" quality="<<int(quality)<<" frames="<<fine[0].size()<<" fnv="<<hash<<" partition_bits_equal="<<exact<<" energy="<<energy(fine)<<'\n';
    }
}
void effect_eq_kernel_availability()
{
    section("effect_eq_kernel_availability");
    auto patch=flat();patch.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(4),masterChain};
    patch.layout.effectCount[masterChain]=1;patch.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(patch));
    if(!validateRoutedPatch(patch))return;
    RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(48000,512));
    const auto output=render(engine,2048,{{0,on()}});CHECK(finite(output)&&energy(output)>1e-8);
}

void effect_eq_transfer_and_live_clock()
{
    section("effect_eq_transfer_and_live_clock");
    auto p=flat();add(p,EffectKind::eq);
    if(!validateRoutedPatch(p))return; // Availability has its own causal RED.
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto capture=[&](RoutedPatch patch,int note,int partition=512){
            RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));
            return render(engine,int(rate*.45),{{0,on(note)}},partition);
        };
        auto dry=p;dry.layout=RackLayout{};
        const auto clean=capture(dry,69),transparent=capture(p,69);
        CHECK(finite(transparent)&&difference(clean,transparent)==0);
        // Actual stationary production sine notes, unnormalized and with the
        // same physical clock. Center bell gain and shelf asymptotes are
        // observable transfer laws, not a copied coefficient implementation.
        for(float gain:{-9.f,9.f}){
            auto bell=p;bell.effects[0].eq.bell={440,gain,1};
            const auto wet=capture(bell,69);
            const double measured=10*std::log10(energy(wet,int(rate*.25))/energy(clean,int(rate*.25)));
            CHECK(std::abs(measured-gain)<.04);
            auto narrower=bell;narrower.effects[0].eq.bell.q=4;
            const auto broad=capture(bell,81),narrow=capture(narrower,81),atOctave=capture(dry,81);
            CHECK(std::abs(std::log(energy(narrow,int(rate*.25))/energy(atOctave,int(rate*.25))))
                <std::abs(std::log(energy(broad,int(rate*.25))/energy(atOctave,int(rate*.25)))));
        }
        auto shelves=p;shelves.effects[0].eq.lowShelf={1000,6,.70710678f};
        const auto lowDry=capture(dry,33),lowWet=capture(shelves,33);
        CHECK(std::abs(10*std::log10(energy(lowWet,int(rate*.25))/energy(lowDry,int(rate*.25)))-6)<.08);
        shelves=p;shelves.effects[0].eq.highShelf={1000,-6,.70710678f};
        const auto highDry=capture(dry,117),highWet=capture(shelves,117);
        CHECK(std::abs(10*std::log10(energy(highWet,int(rate*.25))/energy(highDry,int(rate*.25)))+6)<.08);
        auto live=[&](int partition){
            RoutedSynthEngine engine;auto patch=p;patch.effects[0].eq={{20,18,4},{440,18,4},{20000,-18,4}};
            CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));Audio result;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(engine,count,events,partition);
                for(int ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),a[ch].begin(),a[ch].end());};
            append(int(rate*.04),{{0,on(69)},{73,on(93)}});
            for(int n=0;n<12;++n){auto changed=patch;
                changed.effects[0].eq={{n%2?2000.f:20.f,n%2?-18.f:18.f,n%2?.25f:4.f},
                    {n%2?20000.f:20.f,n%2?18.f:-18.f,4},{n%2?1000.f:20000.f,n%2?-18.f:18.f,4}};
                CHECK(engine.setPatch(changed));patch=changed;
                append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(changed));append(37);
            }
            // Invalid retained EQ fields must leave the held voice/control
            // state unchanged, even when the selected effect is another kind.
            const auto before=engine.patch();auto bad=before;bad.effects[7].eq.bell.q=4.01f;
            CHECK(!engine.setPatch(bad)&&engine.patch()==before);
            append(int(rate*.02),{{17,juce::MidiMessage::midiStop()},{113,on(57)}});
            engine.reset();append(int(rate*.03),{{0,on(65)},{201,off(65)}});
            CHECK(finite(result)&&peak(result)<10&&energy(result)>1e-8);return result;
        };
        const auto a=live(512),b=live(64),c=live(73);CHECK(difference(a,b)==0&&difference(a,c)==0);
        bool bits=true;for(int ch=0;ch<2;++ch)for(std::size_t i=0;i<a[ch].size();++i)
            bits&=std::bit_cast<std::uint32_t>(a[ch][i])==std::bit_cast<std::uint32_t>(b[ch][i])
                &&std::bit_cast<std::uint32_t>(a[ch][i])==std::bit_cast<std::uint32_t>(c[ch][i]);
        CHECK(bits);std::cout<<"EQ_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits="<<bits<<" peak="<<peak(a)<<'\n';
    }
    section("effect_eq_relative_pole_tail");
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double rate=8000,internalRate=rate*(quality==RenderQuality::higher?2:1);
        auto impulse=impulsePatch();add(impulse,EffectKind::eq);
        impulse.effects[0].eq.bell={20,18,4};
        const double seconds=detail::eqTailSeconds(impulse.effects[0].eq,internalRate);
        CHECK(std::isfinite(seconds)&&seconds>.1&&seconds<20);
        if(!std::isfinite(seconds)||seconds<=0||seconds>=20)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(impulse)&&engine.prepare(rate,512,quality));
        const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+int(rate*.02),impulseEvents);
        double residual=0;for(const auto& ch:tail)for(int n=boundary;n<int(ch.size());++n)residual=std::max(residual,std::abs(double(ch[n])));
        CHECK(finite(tail)&&peak(tail)>1e-6&&residual<=peak(tail)*1e-6);
        auto extreme=impulse;extreme.effects[0].eq={{2000,18,4},{20000,-18,4},{20000,18,4}};
        CHECK(engine.setPatch(extreme));const auto clamped=render(engine,8192,{{0,on(117)}});
        CHECK(finite(clamped)&&peak(clamped)<10);
        std::cout<<"EQ_TAIL rate="<<rate<<" quality="<<int(quality)<<" relative_seconds="<<seconds<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    section("effect_eq_prepared_first_and_live_callbacks");
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::eq);
    for(auto& l:full.synth.layers)for(auto& route:l.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        RoutedSynthEngine engine;CHECK(engine.setPatch(full)&&engine.prepare(rate,512,quality));
        juce::AudioBuffer<float> out(2,512);juce::MidiBuffer notes,empty;notes.ensureSize(4096);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;
        bool accepted=true;engine.render(out,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& effect:next.effects)effect.eq.bell={float(n%2?20:20000),float(n%2?-18:18),4};
            accepted&=engine.setPatch(next);engine.render(out,empty,120);if(n%19==0)engine.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);
        std::cout<<"EQ_CALLBACK rate="<<rate<<" quality="<<int(quality)<<" first_plus_live=97 layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
}

void effect_filter_transfer_and_live_clock()
{
    section("effect_filter_transfer_and_live_clock");auto p=flat();add(p,EffectKind::filter);
    if(!validateRoutedPatch(p))return;
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto capture=[&](RoutedPatch patch,int note,int partition=512){RoutedSynthEngine engine;
            CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));return render(engine,int(rate*.4),{{0,on(note)}},partition);};
        auto dry=p;dry.layout=RackLayout{};const auto clean=capture(dry,69);
        auto mix0=p;mix0.effects[0].filter.mix=0;CHECK(difference(clean,capture(mix0,69))==0);
        auto bypass=p;bypass.effects[0].bypassed=true;CHECK(difference(clean,capture(bypass,69))==0);
        for(auto mode:{FilterFxMode::lowpass,FilterFxMode::bandpass,FilterFxMode::highpass}){
            auto at=p;at.effects[0].filter={mode,440,.70710678f,0,1};const auto wet=capture(at,69);
            const double measured=10*std::log10(energy(wet,int(rate*.25))/energy(clean,int(rate*.25)));
            const double expected=mode==FilterFxMode::bandpass?0:-3.010299956639812;
            CHECK(finite(wet)&&std::abs(measured-expected)<.15);
            at.effects[0].filter.resonance=10;const auto resonance=capture(at,69);
            const double qGain=10*std::log10(energy(resonance,int(rate*.25))/energy(clean,int(rate*.25)));
            CHECK(std::abs(qGain-(mode==FilterFxMode::bandpass?0:20))<.15);
            // Source dry is common: octave response distinguishes actual
            // low/high cut and BP Q narrowing from a neutral copied recipe.
            const auto octaveDry=capture(dry,81),normalOctave=capture(p,81);
            at.effects[0].filter.resonance=.5f;const auto broad=capture(at,81);
            at.effects[0].filter.resonance=10;const auto narrow=capture(at,81);
            if(mode==FilterFxMode::bandpass)CHECK(energy(narrow,int(rate*.25))<energy(broad,int(rate*.25))*.05);
            auto lo=at;lo.effects[0].filter.cutoffHz=20;auto hi=at;hi.effects[0].filter.cutoffHz=20000;
            const auto low=capture(lo,69),high=capture(hi,69);CHECK(finite(low)&&finite(high)&&peak(low)<10&&peak(high)<10);
            if(mode==FilterFxMode::lowpass)CHECK(energy(low,int(rate*.25))<energy(high,int(rate*.25))*.01);
            if(mode==FilterFxMode::highpass)CHECK(energy(high,int(rate*.25))<energy(low,int(rate*.25))*.01);
            (void)octaveDry;(void)normalOctave;
            std::cout<<"FILTER_TRANSFER rate="<<rate<<" quality="<<int(quality)<<" mode="<<int(mode)<<" center_db="<<measured<<" q10_center_db="<<qGain<<'\n';
        }
        const auto live=[&](int partition){RoutedSynthEngine engine;auto next=p;next.effects[0].filter={FilterFxMode::lowpass,20,10,24,1};
            CHECK(engine.setPatch(next)&&engine.prepare(rate,512,quality));Audio result;
            const auto append=[&](int n,const std::vector<Event>& events={}){auto a=render(engine,n,events,partition);for(int ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),a[ch].begin(),a[ch].end());};
            append(int(rate*.03),{{0,on(69)},{73,on(93)}});
            for(int n=0;n<12;++n){next.effects[0].filter={FilterFxMode::bandpass,n%2?20.f:20000.f,n%2?.5f:10.f,n%2?0.f:24.f,n%2?.1f:1.f};
                CHECK(engine.setPatch(next));append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(next));append(37);}
            const auto before=engine.patch();auto bad=before;bad.effects[7].filter.resonance=10.01f;CHECK(!engine.setPatch(bad)&&engine.patch()==before);
            bad=before;bad.effects[7].filter.mode=static_cast<FilterFxMode>(3);CHECK(!engine.setPatch(bad)&&engine.patch()==before);
            append(int(rate*.02),{{17,juce::MidiMessage::midiStop()},{113,on(57)}});engine.reset();append(int(rate*.03),{{0,on(65)},{201,off(65)}});
            CHECK(finite(result)&&peak(result)<10&&energy(result)>1e-9);return result;};
        const auto a=live(512),b=live(64),c=live(73);CHECK(difference(a,b)==0&&difference(a,c)==0);
        bool exact=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<a[ch].size();++n)
            exact&=std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(b[ch][n])&&std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(c[ch][n]);
        CHECK(exact);std::cout<<"FILTER_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits="<<exact<<" peak="<<peak(a)<<'\n';
    }
    section("effect_filter_relative_pole_tail");
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double rate=8000,internalRate=rate*(quality==RenderQuality::higher?2:1);auto impulse=impulsePatch();add(impulse,EffectKind::filter);
        impulse.effects[0].filter={FilterFxMode::lowpass,20,10,0,1};const double seconds=detail::filterTailSeconds(impulse.effects[0].filter,internalRate);
        CHECK(std::isfinite(seconds)&&seconds>10&&seconds<10.3);if(!std::isfinite(seconds)||seconds<=0||seconds>=11)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(impulse)&&engine.prepare(rate,512,quality));const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+int(rate*.02),impulseEvents);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[n])));
        CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6);
        std::cout<<"FILTER_TAIL rate="<<rate<<" quality="<<int(quality)<<" seconds="<<seconds<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
    section("effect_filter_prepared_first_and_live_callbacks");auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::filter);
    for(auto& layer:full.synth.layers)for(auto& route:layer.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine engine;CHECK(engine.setPatch(full)&&engine.prepare(rate,block,quality));
        juce::AudioBuffer<float> out(2,block);juce::MidiBuffer notes,empty;notes.ensureSize(4096);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;engine.render(out,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& e:next.effects)e.filter={n%12?FilterFxMode::bandpass:FilterFxMode::highpass,n%2?20.f:20000.f,n%2?.5f:10.f,n%2?0.f:24.f,n%2?.25f:1.f};
            accepted&=engine.setPatch(next);engine.render(out,empty,120);if(n%19==0)engine.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);
        std::cout<<"FILTER_CALLBACK rate="<<rate<<" block="<<block<<" quality="<<int(quality)<<" first_plus_live=97 layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
}

void effect_filter_kernel_availability()
{
    section("effect_filter_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(5),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(p));
}

void effect_chorus_delay_feedback_and_stereo()
{
    section("effect_chorus_delay_feedback_and_stereo");auto p=impulsePatch();add(p,EffectKind::chorus);
    p.effects[0].chorus={.35f,0,12.125f,0,1};auto dry=p;dry.layout=RackLayout{};
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto capture=[&](RoutedPatch patch,int count=4096){RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));return render(engine,count,impulseEvents,64);};
        const auto input=capture(dry),wet=capture(p);CHECK(finite(wet)&&energy(wet)>0);
        auto bypass=p;bypass.effects[0].bypassed=true;CHECK(difference(input,capture(bypass))==0);
        auto zero=p;zero.effects[0].chorus.mix=0;CHECK(difference(input,capture(zero))==0);
        // Constant delay is a measured linear law from the actual production
        // excitation. Realtime fractional interpolation includes both taps;
        // integer host delay commutes with the Higher FIR, including its edge.
        auto integer=p;integer.effects[0].chorus.baseDelayMs=20;const auto delayed=capture(integer);
        const int frames=int(rate*.020);double integerError=0,fractionalError=0;
        const double delay=double(p.effects[0].chorus.baseDelayMs)*rate/1000;const int whole=int(delay);const double fraction=delay-whole;
        for(int ch=0;ch<2;++ch)for(int n=0;n<4096;++n){
            const double expected=n>=frames?input[ch][n-frames]:0;integerError=std::max(integerError,std::abs(double(delayed[ch][n])-expected));
            if(quality==RenderQuality::realtime){const double a=n>=whole?input[ch][n-whole]:0,b=n>whole?input[ch][n-whole-1]:0;
                fractionalError=std::max(fractionalError,std::abs(double(wet[ch][n])-(a*(1-fraction)+b*fraction)));}
        }
        CHECK(integerError<2e-8&&fractionalError<2e-8);CHECK(wet[0]==wet[1]);
        auto left=p;left.synth.layers[0].pan=-1;const auto one=capture(left);double rightPeak=0;
        for(float value:one[1])rightPeak=std::max(rightPeak,std::abs(double(value)));CHECK(rightPeak<1e-17&&energy(one)>0);
        auto asym=p;asym.synth.layers[0].pan=.4f;const auto asymmetric=capture(asym);CHECK(asymmetric[0]!=asymmetric[1]);
        auto partial=p;partial.effects[0].chorus.mix=.37f;const auto blend=capture(partial);double mixError=0;
        for(int ch=0;ch<2;++ch)for(int n=0;n<4096;++n)mixError=std::max(mixError,std::abs(double(blend[ch][n])-(double(input[ch][n])*(1-double(.37f))+double(wet[ch][n])*double(.37f))));
        CHECK(mixError<2e-8);
        if(quality==RenderQuality::realtime){for(float feedback:{-.6f,.6f}){
            auto echo=integer;echo.effects[0].chorus.feedback=feedback;const auto repeats=capture(echo);double error=0;
            std::array<std::vector<double>,2> expected;for(auto& channel:expected)channel.resize(4096);
            for(int ch=0;ch<2;++ch)for(int n=0;n<4096;++n){expected[ch][n]=n>=frames?double(input[ch][n-frames]):0;
                if(n>=frames+1)expected[ch][n]-=double(feedback)*expected[ch][n-frames-1];
                error=std::max(error,std::abs(double(repeats[ch][n])-expected[ch][n]));}
            CHECK(error<2e-8);CHECK(repeats[0][2*frames+1]*repeats[0][frames]*(feedback>0?-1:1)>0);
        }}
        std::cout<<"CHORUS_DELAY rate="<<rate<<" quality="<<int(quality)<<" integer_error="<<integerError<<" fractional_error="<<fractionalError<<" mix_error="<<mixError<<" right_peak="<<rightPeak<<'\n';
    }
    section("effect_chorus_physical_motion_and_tail");
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto motion=flat();add(motion,EffectKind::chorus);motion.effects[0].chorus={.7f,1,18,0,1};
        auto constant=motion;constant.effects[0].chorus.depth=0;auto faster=motion;faster.effects[0].chorus.rateHz=2;
        const auto capture=[&](RoutedPatch patch){RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));return render(engine,int(rate*.3),{{0,on()}},73);};
        const auto moving=capture(motion),stationary=capture(constant),fast=capture(faster);
        CHECK(finite(moving)&&energy(moving)>0&&difference(moving,stationary)>1e-4&&difference(moving,fast)>1e-4);
        CHECK(moving[0]==moving[1]);
    }
    for(float feedback:{-.8f,.8f})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);auto tailPatch=impulsePatch();add(tailPatch,EffectKind::chorus);
        tailPatch.effects[0].chorus={.35f,1,40,feedback,1};const double seconds=detail::chorusTailSeconds(tailPatch.effects[0].chorus,internal);
        CHECK(std::isfinite(seconds)&&seconds>3.7&&seconds<3.8);if(!std::isfinite(seconds)||seconds<=0||seconds>=4)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(tailPatch)&&engine.prepare(rate,512,quality));const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+800,impulseEvents,73);double residual=0;
        for(const auto& channel:tail)for(int n=boundary;n<int(channel.size());++n)residual=std::max(residual,std::abs(double(channel[n])));
        CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6);
        std::cout<<"CHORUS_TAIL feedback="<<feedback<<" quality="<<int(quality)<<" seconds="<<seconds<<" residual="<<residual<<" peak="<<peak(tail)<<'\n';
    }
}
void effect_chorus_live_clock_and_callbacks()
{
    section("effect_chorus_live_clock_and_callbacks");auto p=flat();add(p,EffectKind::chorus);
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto live=[&](int partition){auto patch=p;RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(engine,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(int(rate*.09),{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].chorus={n%2?.1f:5.f,n%2?0.f:1.f,n%2?12.f:40.f,n%2?-.8f:.8f,n%2?.1f:1.f};
                CHECK(engine.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(patch));append(37);}
            const auto held=engine.patch();auto invalid=held;invalid.effects[7].chorus.feedback=.8001f;CHECK(!engine.setPatch(invalid)&&engine.patch()==held);
            CHECK(!engine.prepare(7999,512));append(401);patch.effects[0].bypassed=true;CHECK(engine.setPatch(patch));append(int(rate*.02));
            patch.effects[0].chorus={2,.9f,38,-.7f,.8f};CHECK(engine.setPatch(patch));append(int(rate*.02));patch.effects[0].bypassed=false;CHECK(engine.setPatch(patch));append(int(rate*.1));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});engine.reset();append(1501,{{0,on(65)},{801,off(65)}});
            CHECK(finite(output)&&energy(output)>1e-9&&peak(output)<2);return output;};
        const auto a=live(512),b=live(64),c=live(73);bool bits=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<a[ch].size();++n)
            bits&=std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(b[ch][n])&&std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(c[ch][n]);
        CHECK(bits);std::cout<<"CHORUS_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits="<<bits<<" peak="<<peak(a)<<'\n';
        RoutedSynthEngine reset,fresh;auto control=p;control.effects[0].chorus={3,.8f,35,-.6f,1};CHECK(reset.setPatch(control)&&fresh.setPatch(control)&&reset.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));
        render(reset,2401,{{0,on()}});reset.reset();CHECK(difference(render(reset,4096,{{0,on()}}),render(fresh,4096,{{0,on()}}))==0);
        render(reset,2000);const auto stopped=render(reset,4096,{{913,juce::MidiMessage::midiStop()},{1100,on(57)}}),reference=render(fresh,4096,{{913,juce::MidiMessage::midiStop()},{1100,on(57)}});
        double stopError=0;for(int ch=0;ch<2;++ch)for(int n=913+2*reset.latencySamples();n<4096;++n)stopError=std::max(stopError,std::abs(double(stopped[ch][n])-reference[ch][n]));CHECK(stopError==0);
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::chorus);
    for(auto& layer:full.synth.layers)for(auto& route:layer.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine engine;CHECK(engine.setPatch(full)&&engine.prepare(rate,block,quality));
        juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;engine.render(output,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& e:next.effects)e.chorus={n%2?.1f:5.f,n%2?0.f:1.f,n%2?12.f:40.f,n%2?-.8f:.8f,n%2?.25f:1.f};
            accepted&=engine.setPatch(next);engine.render(output,n%19==0?stop:empty,120);if(n%23==0)engine.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);
        std::cout<<"CHORUS_CALLBACK rate="<<rate<<" block="<<block<<" quality="<<int(quality)<<" first_plus_live=97 layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
}


void chorus_actual_phase_and_centre_ramp()
{
    section("chorus_actual_physical_phase_and_centre_ramp");
    for(double rate:{8000.,44100.,48000.,96000.}){
        auto p=flat();add(p,EffectKind::chorus);p.effects[0].chorus={.7f,.8f,18,0,1};auto dry=p;dry.layout=RackLayout{};
        const int change=int(rate*.13),count=int(rate*.4);const auto source=sound(dry,count,{{0,on()}},rate,73);
        RoutedSynthEngine engine;CHECK(engine.setPatch(p)&&engine.prepare(rate,512));const auto before=render(engine,change,{{0,on()}},64);
        auto next=p;next.effects[0].chorus.baseDelayMs=40;CHECK(engine.setPatch(next));const auto after=render(engine,count-change,{},73);Audio actual=before;
        for(int ch=0;ch<2;++ch)actual[ch].insert(actual[ch].end(),after[ch].begin(),after[ch].end());
        double error=0;const int ramp=int(std::ceil(rate*.005));
        for(int n=0;n<count;++n){const double centre=n<change?18:18+22*std::min(1.,double(n-change+1)/ramp);
            // Stock shared oscillator begins at -pi. This independent elapsed
            // physical-time phase predicts actual moving fractional tap output
            // from the captured excitation, not another Chorus instance.
            const double phase=2*std::numbers::pi*double(p.effects[0].chorus.rateHz)*n/rate-std::numbers::pi;
            const double delay=(centre+10*double(p.effects[0].chorus.depth)*std::sin(phase))*rate/1000;const int whole=int(delay);const double fraction=delay-whole;
            for(int ch=0;ch<2;++ch){const double a=n>=whole?source[ch][n-whole]:0,b=n>whole?source[ch][n-whole-1]:0;
                error=std::max(error,std::abs(double(actual[ch][n])-(a*(1-fraction)+b*fraction)));}}
        CHECK(error<3e-8&&energy(actual)>1e-8&&finite(actual));
        std::cout<<"CHORUS_PHYSICAL_ORACLE rate="<<rate<<" phase_hz="<<p.effects[0].chorus.rateHz<<" centre_ramp_frames="<<ramp<<" actual_source_fractional_max_error="<<error<<'\n';
    }
}

void effect_chorus_kernel_availability()
{
    section("effect_chorus_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(6),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(p));
}

// Independent complete six-allpass/one-frame feedback transfer. The oracle
// uses captured dry excitation, not another stock effect instance.
std::complex<double> phaserTransfer(double hz,double centre,double feedback,double rate)
{
    const double g=std::tan(std::numbers::pi*std::min(centre,.49*rate)/rate),a=2*g/(1+g)-1;
    const auto z=std::polar(1.,-2*std::numbers::pi*hz/rate),stage=(a+z)/(1.+a*z);
    const auto h=std::pow(stage,6);return h/(1.+feedback*z*h);
}
std::complex<double> phaserTone(const std::vector<float>& channel,double hz,double rate,int from)
{
    double cc=0,ss=0,cs=0,yc=0,ys=0;
    for(int n=from;n<int(channel.size());++n){const double angle=2*std::numbers::pi*hz*n/rate,c=std::cos(angle),s=std::sin(angle);
        cc+=c*c;ss+=s*s;cs+=c*s;yc+=channel[n]*c;ys+=channel[n]*s;}
    const double determinant=cc*ss-cs*cs;return {(yc*ss-ys*cs)/determinant,-(ys*cc-yc*cs)/determinant};
}
void effect_phaser_transfer_and_motion()
{
    section("effect_phaser_complete_transfer_and_stereo");
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto p=flat();add(p,EffectKind::phaser);p.effects[0].phaser={.7f,0,1300,0,1};auto dry=p;dry.layout=RackLayout{};
        const int count=int(rate*.5);const auto capture=[&](RoutedPatch patch){RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));return render(engine,count,{{0,on()}},73);};
        const auto source=capture(dry),wet=capture(p);CHECK(finite(wet)&&energy(wet)>1e-9&&wet[0]==wet[1]);
        auto bypass=p;bypass.effects[0].bypassed=true;CHECK(difference(source,capture(bypass))==0);
        auto zero=p;zero.effects[0].phaser.mix=0;CHECK(difference(source,capture(zero))==0);
        auto left=p;left.synth.layers[0].pan=-1;const auto one=capture(left);double right=0;for(float x:one[1])right=std::max(right,std::abs(double(x)));CHECK(right<1e-17&&energy(one)>0);
        auto asymmetric=p;asymmetric.synth.layers[0].pan=.4f;const auto stereo=capture(asymmetric);CHECK(stereo[0]!=stereo[1]);
        double maximumError=0;double notchMinimum=1;
        for(float centre:{220.f,1300.f,7000.f})for(float feedback:{-.6f,0.f,.6f}){
            auto q=p;q.effects[0].phaser.centreHz=centre;q.effects[0].phaser.feedback=feedback;
            const auto actual=capture(q);const auto measured=phaserTone(actual[0],440,rate,int(rate*.3))/phaserTone(source[0],440,rate,int(rate*.3));
            const auto expected=phaserTransfer(440,centre,feedback,rate*(quality==RenderQuality::higher?2:1));
            maximumError=std::max(maximumError,std::abs(measured-expected));CHECK(std::abs(measured-expected)<2e-4);
            if(feedback==0){CHECK(std::abs(std::abs(measured)-1)<2e-4);auto blend=q;blend.effects[0].phaser.mix=.5f;const auto mixed=capture(blend);
                const auto mixedRatio=phaserTone(mixed[0],440,rate,int(rate*.3))/phaserTone(source[0],440,rate,int(rate*.3));
                CHECK(std::abs(mixedRatio-(.5+.5*expected))<2e-4);notchMinimum=std::min(notchMinimum,std::abs(mixedRatio));}
        }
        CHECK(notchMinimum<.8);std::cout<<"PHASER_TRANSFER rate="<<rate<<" quality="<<int(quality)<<" closed_loop_max_error="<<maximumError<<" blend_notch_min="<<notchMinimum<<'\n';
    }
    section("effect_phaser_actual_physical_clock");
    for(double rate:{8000.,44100.,48000.,96000.}){
        auto p=flat();add(p,EffectKind::phaser);p.effects[0].phaser={.7f,.8f,1300,0,1};auto dry=p;dry.layout=RackLayout{};
        const int change=int(rate*.13),count=int(rate*.4),ramp=int(std::ceil(rate*.005));const auto source=sound(dry,count,{{0,on()}},rate,73);
        RoutedSynthEngine engine;CHECK(engine.setPatch(p)&&engine.prepare(rate,512));auto actual=render(engine,change,{{0,on()}},64);
        auto next=p;next.effects[0].phaser.centreHz=4000;CHECK(engine.setPatch(next));const auto after=render(engine,count-change,{},73);for(int ch=0;ch<2;++ch)actual[ch].insert(actual[ch].end(),after[ch].begin(),after[ch].end());
        std::array<std::array<double,6>,2> history{};double coefficient=0,error=0;const double maximum=std::min(20000.,.49*rate);
        for(int n=0;n<count;++n){if(n%4==0){const double centre=n<change?1300:1300+2700*std::min(1.,double(n-change+1)/ramp);
                const double normalized=std::log10(std::clamp(centre,20.,maximum)/20)/std::log10(maximum/20);
                const double phase=2*std::numbers::pi*double(p.effects[0].phaser.rateHz)*n/rate-std::numbers::pi;
                const double cutoff=20*std::pow(maximum/20,std::clamp(normalized+.5*double(p.effects[0].phaser.depth)*std::sin(phase),0.,1.));
                const double g=std::tan(std::numbers::pi*cutoff/rate);coefficient=g/(1+g);}
            for(int ch=0;ch<2;++ch){double x=source[ch][n];for(double& state:history[ch]){const double v=coefficient*(x-state),low=v+state;state=low+v;x=2*low-x;}
                error=std::max(error,std::abs(double(actual[ch][n])-x));}}
        CHECK(error<8e-8&&energy(actual)>1e-9&&finite(actual));std::cout<<"PHASER_PHYSICAL_ORACLE rate="<<rate<<" rate_hz="<<p.effects[0].phaser.rateHz<<" update_internal_frames=4 centre_ramp_frames="<<ramp<<" error="<<error<<'\n';
    }
}
void effect_phaser_tail_residual()
{
    section("effect_phaser_stationary_and_moving_tail_residual");
    for(float feedback:{-.8f,.8f})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(int scenario=0;scenario<3;++scenario){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);auto p=impulsePatch();add(p,EffectKind::phaser);
        p.effects[0].phaser={scenario==2?5.f:.1f,scenario==2?1.f:0.f,scenario==0?20.f:scenario==1?20000.f:1300.f,feedback,1};
        const double seconds=detail::phaserTailSeconds(p.effects[0].phaser,internal);CHECK(std::isfinite(seconds)&&seconds>25&&seconds<30);if(!std::isfinite(seconds)||seconds<25||seconds>30)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(p)&&engine.prepare(rate,512,quality));const int boundary=int(std::ceil(seconds*rate))+2*engine.latencySamples();
        const auto tail=render(engine,boundary+800,impulseEvents,73);double residual=0;for(const auto& ch:tail)for(int n=boundary;n<int(ch.size());++n)residual=std::max(residual,std::abs(double(ch[n])));
        CHECK(finite(tail)&&peak(tail)>1e-7&&residual<=peak(tail)*1e-6);
        std::cout<<"PHASER_TAIL feedback="<<feedback<<" quality="<<int(quality)<<" scenario="<<scenario<<" seconds="<<seconds<<" peak="<<peak(tail)<<" residual="<<residual<<" relative_limit=1e-6 moving_is_empirical=1\n";
    }
    PhaserSettings valid;CHECK(std::isfinite(detail::phaserTailSeconds(valid,48000)));valid.feedback=.8001f;CHECK(!std::isfinite(detail::phaserTailSeconds(valid,48000)));
}

void effect_phaser_live_clock_and_callbacks()
{
    section("effect_phaser_live_clock_and_callbacks");auto p=flat();add(p,EffectKind::phaser);
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto live=[&](int partition){auto patch=p;RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(engine,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(int(rate*.09),{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].phaser={n%2?.1f:5.f,n%2?0.f:1.f,n%2?20.f:20000.f,n%2?-.8f:.8f,n%2?.1f:1.f};
                CHECK(engine.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(patch));append(37);}
            const auto held=engine.patch();auto invalid=held;invalid.effects[7].phaser.feedback=.8001f;CHECK(!engine.setPatch(invalid)&&engine.patch()==held);
            CHECK(!engine.prepare(7999,512));append(401);patch.effects[0].bypassed=true;CHECK(engine.setPatch(patch));append(int(rate*.02));
            patch.effects[0].phaser={2,.9f,18000,-.7f,.8f};CHECK(engine.setPatch(patch));append(int(rate*.02));patch.effects[0].bypassed=false;CHECK(engine.setPatch(patch));append(int(rate*.1));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});engine.reset();append(1501,{{0,on(65)},{801,off(65)}});
            CHECK(finite(output)&&energy(output)>1e-9&&peak(output)<2);return output;};
        const auto a=live(512),b=live(64),c=live(73);bool bits=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<a[ch].size();++n)
            bits&=std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(b[ch][n])&&std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(c[ch][n]);
        CHECK(bits);std::cout<<"PHASER_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits="<<bits<<" peak="<<peak(a)<<'\n';
        RoutedSynthEngine reset,fresh;auto control=p;control.effects[0].phaser={3,.8f,17000,-.6f,1};CHECK(reset.setPatch(control)&&fresh.setPatch(control)&&reset.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));
        render(reset,2401,{{0,on()}});reset.reset();CHECK(difference(render(reset,4096,{{0,on()}}),render(fresh,4096,{{0,on()}}))==0);
        render(reset,2000);const auto stopped=render(reset,4096,{{913,juce::MidiMessage::midiStop()},{1100,on(57)}}),reference=render(fresh,4096,{{913,juce::MidiMessage::midiStop()},{1100,on(57)}});
        double stopError=0;for(int ch=0;ch<2;++ch)for(int n=913+2*reset.latencySamples();n<4096;++n)stopError=std::max(stopError,std::abs(double(stopped[ch][n])-reference[ch][n]));CHECK(stopError==0);
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::phaser);
    for(auto& layer:full.synth.layers)for(auto& route:layer.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine engine;CHECK(engine.setPatch(full)&&engine.prepare(rate,block,quality));
        juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;engine.render(output,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& e:next.effects)e.phaser={n%2?.1f:5.f,n%2?0.f:1.f,n%2?20.f:20000.f,n%2?-.8f:.8f,n%2?.25f:1.f};
            accepted&=engine.setPatch(next);engine.render(output,n%19==0?stop:empty,120);if(n%23==0)engine.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);
        std::cout<<"PHASER_CALLBACK rate="<<rate<<" block="<<block<<" quality="<<int(quality)<<" first_plus_live=97 layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
}

void effect_phaser_kernel_availability()
{
    section("effect_phaser_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(7),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(p));
}
std::complex<double> flangerTransfer(double hz,double milliseconds,double feedback,double rate)
{
    const double delay=milliseconds*.001*rate;const int k=int(std::floor(delay));const double u=delay-k;
    const auto z=std::polar(1.,-2*std::numbers::pi*hz/rate),p=(1-u)*std::pow(z,k)+u*std::pow(z,k+1);
    return p/(1.+feedback*z*p);
}
void effect_flanger_transfer_and_interpolation()
{
    section("effect_flanger_complete_comb_and_fractional_interpolation");
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto p=flat();add(p,EffectKind::flanger);p.effects[0].flanger={.7f,0,2,0,1};auto dry=p;dry.layout=RackLayout{};
        const int count=int(rate*.5);const auto capture=[&](const RoutedPatch& patch){RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));return render(engine,count,{{0,on()}},73);};
        const auto source=capture(dry),wet=capture(p);CHECK(finite(wet)&&energy(wet)>1e-9&&wet[0]==wet[1]);
        auto zero=p;zero.effects[0].flanger.mix=0;CHECK(difference(source,capture(zero))==0);
        auto bypass=p;bypass.effects[0].bypassed=true;CHECK(difference(source,capture(bypass))==0);
        auto left=p;left.synth.layers[0].pan=-1;const auto one=capture(left);double right=0;for(float x:one[1])right=std::max(right,std::abs(double(x)));CHECK(right<1e-17&&energy(one)>0);
        auto asymmetric=p;asymmetric.synth.layers[0].pan=.4f;CHECK(capture(asymmetric)[0]!=capture(asymmetric)[1]);
        double maximumError=0,notchMinimum=1;
        for(float delay:{1.f,2.125f,8.f})for(float feedback:{-.6f,0.f,.6f}){
            auto q=p;q.effects[0].flanger.baseDelayMs=delay;q.effects[0].flanger.feedback=feedback;
            const auto actual=capture(q);const auto measured=phaserTone(actual[0],440,rate,int(rate*.3))/phaserTone(source[0],440,rate,int(rate*.3));
            const auto expected=flangerTransfer(440,delay,feedback,rate*(quality==RenderQuality::higher?2:1));
            maximumError=std::max(maximumError,std::abs(measured-expected));CHECK(std::abs(measured-expected)<2e-4);
            if(feedback==0){auto blend=q;blend.effects[0].flanger.mix=.5f;const auto mixed=capture(blend);
                const auto ratio=phaserTone(mixed[0],440,rate,int(rate*.3))/phaserTone(source[0],440,rate,int(rate*.3));
                CHECK(std::abs(ratio-(.5+.5*expected))<2e-4);notchMinimum=std::min(notchMinimum,std::abs(ratio));}
        }
        CHECK(notchMinimum<.8);std::cout<<"FLANGER_TRANSFER rate="<<rate<<" quality="<<int(quality)<<" max_error="<<maximumError<<" blend_notch_min="<<notchMinimum<<'\n';
    }
}
void effect_flanger_independent_moving_oracle()
{
    section("effect_flanger_actual_physical_interrupted_clock");
    for(double rate:{8000.,44100.,48000.,96000.}){
        auto p=flat();add(p,EffectKind::flanger);p.effects[0].flanger={.25f,.6f,2,.35f,.5f};auto dry=p;dry.layout=RackLayout{};
        const int count=int(rate*.3),ramp=int(std::ceil(.005*rate));
        const std::array<int,4> changes{int(rate*.071)+3,int(rate*.073)+1,int(rate*.129)+7,int(rate*.212)+5};
        const std::array<FlangerSettings,4> targets{{{5,1,8,-.8f,1},{.05f,0,1,.8f,.1f},{3,.9f,1,-.7f,.8f},{.1f,.4f,7,.6f,.7f}}};
        const auto source=sound(dry,count,{{0,on()}},rate,73);RoutedSynthEngine engine;CHECK(engine.setPatch(p)&&engine.prepare(rate,512));Audio actual;
        int from=0;for(size_t i=0;i<=changes.size();++i){const int end=i<changes.size()?changes[i]:count;const auto segment=render(engine,end-from,from==0?std::vector<Event>{{0,on()}}:std::vector<Event>{},i%2?64:73);
            for(int ch=0;ch<2;++ch)actual[ch].insert(actual[ch].end(),segment[ch].begin(),segment[ch].end());from=end;
            if(i<targets.size()){p.effects[0].flanger=targets[i];CHECK(engine.setPatch(p));}}
        std::array<double,5> values{.25,double(float(.6)),2,double(float(.35)),.5},ends=values,steps{};int remaining=0;size_t change=0;
        std::array<std::vector<double>,2> excitation;for(auto& c:excitation)c.resize(count);std::array<double,2> previous{};double phase=0,error=0,minimum=1,maximum=0;
        for(int n=0;n<count;++n){if(change<changes.size()&&n==changes[change]){const auto& t=targets[change++];ends={t.rateHz,t.depth,t.baseDelayMs,t.feedback,t.mix};for(size_t i=0;i<5;++i)steps[i]=(ends[i]-values[i])/ramp;remaining=ramp;}
            if(remaining>0){for(size_t i=0;i<5;++i)values[i]+=steps[i];if(--remaining==0)values=ends;}
            const double seconds=values[2]*.001*(1+.75*values[1]*std::sin(phase)),delay=seconds*rate;minimum=std::min(minimum,seconds);maximum=std::max(maximum,seconds);
            const int k=int(std::floor(delay));const double u=delay-k;CHECK(delay>=2&&delay<=.014*rate);
            for(int ch=0;ch<2;++ch){excitation[ch][n]=source[ch][n]-values[3]*previous[ch];
                const double a=n>=k?excitation[ch][n-k]:0,b=n>=k+1?excitation[ch][n-k-1]:0,wet=a+(b-a)*u;previous[ch]=wet;
                const double expected=source[ch][n]*(1-values[4])+wet*values[4];error=std::max(error,std::abs(expected-double(actual[ch][n])));}
            phase+=2*std::numbers::pi*values[0]/rate;if(phase>=2*std::numbers::pi)phase-=2*std::numbers::pi;
        }
        CHECK(error<1e-7&&finite(actual)&&energy(actual)>1e-9&&minimum>=.00025&&maximum<=.014);
        std::cout<<"FLANGER_PHYSICAL rate="<<rate<<" five_ramp_frames="<<ramp<<" minimum_ms="<<minimum*1000<<" maximum_ms="<<maximum*1000<<" independent_error="<<error<<'\n';
    }
}
void effect_flanger_mid_ramp_stop_and_tail()
{
    section("effect_flanger_direct_clear_mid_ramp_Stop_target_snap");
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(int partition:{64,73,512}){
        auto p=flat();add(p,EffectKind::flanger);p.effects[0].flanger={.05f,0,8,.8f,.1f};auto target=p;target.effects[0].flanger={5,1,1,-.8f,1};
        RoutedSynthEngine live,fresh;CHECK(live.setPatch(p)&&live.prepare(rate,512,quality)&&fresh.setPatch(target)&&fresh.prepare(rate,512,quality));
        render(live,int(rate*.01),{{0,on()}},partition);CHECK(live.setPatch(target));render(live,int(rate*.001),{},partition);
        const auto stopped=render(live,4096,{{0,juce::MidiMessage::midiStop()},{0,on(57)}},partition),reference=render(fresh,4096,{{0,on(57)}},partition);
        double postSupport=0;int lastDifferent=-1;const int support=2*live.latencySamples();
        for(int ch=0;ch<2;++ch)for(int n=0;n<4096;++n){
            if(std::bit_cast<std::uint32_t>(stopped[ch][n])!=std::bit_cast<std::uint32_t>(reference[ch][n]))lastDifferent=std::max(lastDifferent,n);
            if(n>=support)postSupport=std::max(postSupport,std::abs(double(stopped[ch][n])-reference[ch][n]));}
        if(const auto* directory=std::getenv("AIRACK_FLANGER_STOP_AUDIO_DIR")){
            const juce::File folder{juce::String(directory)};CHECK(folder.createDirectory().wasOk());
            const auto stem="flanger-stop-"+juce::String(int(rate))+"-"+juce::String(int(quality))+"-"+juce::String(partition);
            for(int variant=0;variant<2;++variant){const auto& audio=variant?reference:stopped;std::vector<float> raw;
                for(const auto& ch:audio)raw.insert(raw.end(),ch.begin(),ch.end());
                CHECK(folder.getChildFile(stem+(variant?"-fresh.f32":"-stopped.f32")).replaceWithData(raw.data(),raw.size()*sizeof(float)));}}
        std::cout<<"FLANGER_STOP_SUPPORT rate="<<rate<<" quality="<<int(quality)<<" partition="<<partition<<" support_frames="<<support<<" last_bit_difference="<<lastDifferent<<" post_support_error="<<postSupport<<'\n';
        // Stop preserves the prepared quality FIR history. Compare exact bits
        // after its declared finite support; the original prefix is retained
        // in the diagnostic raw-pair receipt.
        CHECK(lastDifferent<support&&postSupport==0&&finite(stopped)&&energy(stopped)>1e-9);
        live.reset();fresh.reset();CHECK(difference(render(live,4096,{{0,on(65)}},partition),render(fresh,4096,{{0,on(65)}},partition))==0);
        std::cout<<"FLANGER_STOP rate="<<rate<<" quality="<<int(quality)<<" partition="<<partition<<" mid_ramp_all5_targets=1 whole_prefix_error="<<difference(stopped,reference)<<'\n';
    }
    if(std::getenv("AIRACK_FLANGER_STOP_ONLY"))return;
    section("effect_flanger_stationary_and_moving_relative_tail");
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(float feedback:{-.8f,.8f})for(int scenario=0;scenario<3;++scenario){
        const double rate=8000,internal=rate*(quality==RenderQuality::higher?2:1);auto p=impulsePatch();add(p,EffectKind::flanger);p.effects[0].flanger={scenario==2?5.f:.05f,scenario==2?1.f:0.f,scenario==0?1.f:8.f,feedback,1};
        const double bound=detail::flangerTailSeconds(p.effects[0].flanger,internal);CHECK(std::isfinite(bound)&&bound>=1.1&&bound<=1.16);if(!std::isfinite(bound)||bound<1.1||bound>1.16)continue;
        RoutedSynthEngine engine;CHECK(engine.setPatch(p)&&engine.prepare(rate,512,quality));const int boundary=int(std::ceil(bound*rate))+2*engine.latencySamples();
        auto output=render(engine,boundary+800,impulseEvents,73);double residual=0;for(const auto& ch:output)for(int n=boundary;n<int(ch.size());++n)residual=std::max(residual,std::abs(double(ch[n])));
        CHECK(finite(output)&&peak(output)>1e-7&&residual<=peak(output)*1e-6);
        std::cout<<"FLANGER_TAIL quality="<<int(quality)<<" feedback="<<feedback<<" scenario="<<scenario<<" bound="<<bound<<" peak="<<peak(output)<<" residual="<<residual<<" moving_is_empirical=1\n";
    }
    FlangerSettings f;CHECK(std::abs(detail::flangerTailSeconds(f,8000)-1.155)<1e-12);f.feedback=.8001f;CHECK(!std::isfinite(detail::flangerTailSeconds(f,48000)));
    auto maximum=flat();add(maximum,EffectKind::flanger);maximum.effects[0].flanger={5,1,8,.8f,.5f};RoutedSynthEngine engine;
    CHECK(engine.setPatch(maximum)&&engine.prepare(384000,512,RenderQuality::higher));CHECK(finite(render(engine,1024,{{0,on()}},73)));
}
void effect_flanger_live_clock_and_callbacks()
{
    section("effect_flanger_live_clock_and_callbacks");auto p=flat();add(p,EffectKind::flanger);
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto live=[&](int partition){auto patch=p;RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(engine,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(int(rate*.09),{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].flanger={n%2?.05f:5.f,n%2?0.f:1.f,n%2?1.f:8.f,n%2?-.8f:.8f,n%2?.1f:1.f};
                CHECK(engine.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(patch));append(37);}
            const auto held=engine.patch();auto invalid=held;invalid.effects[7].flanger.feedback=.8001f;CHECK(!engine.setPatch(invalid)&&engine.patch()==held);
            CHECK(!engine.prepare(7999,512));append(401);patch.effects[0].bypassed=true;CHECK(engine.setPatch(patch));append(int(rate*.02));
            patch.effects[0].flanger={2,.9f,8,-.7f,.8f};CHECK(engine.setPatch(patch));append(int(rate*.02));patch.effects[0].bypassed=false;CHECK(engine.setPatch(patch));append(int(rate*.1));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});engine.reset();append(1501,{{0,on(65)},{801,off(65)}});
            CHECK(finite(output)&&energy(output)>1e-9&&peak(output)<2);return output;};
        const auto a=live(512),b=live(64),c=live(73);bool bits=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<a[ch].size();++n)
            bits&=std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(b[ch][n])&&std::bit_cast<std::uint32_t>(a[ch][n])==std::bit_cast<std::uint32_t>(c[ch][n]);
        CHECK(bits);std::cout<<"FLANGER_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits="<<bits<<" peak="<<peak(a)<<'\n';
        RoutedSynthEngine reset,fresh;auto control=p;control.effects[0].flanger={3,.8f,7,-.6f,1};CHECK(reset.setPatch(control)&&fresh.setPatch(control)&&reset.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));
        render(reset,2401,{{0,on()}});reset.reset();CHECK(difference(render(reset,4096,{{0,on()}}),render(fresh,4096,{{0,on()}}))==0);
        render(reset,2000);const auto stopped=render(reset,4096,{{913,juce::MidiMessage::midiStop()},{1100,on(57)}}),reference=render(fresh,4096,{{913,juce::MidiMessage::midiStop()},{1100,on(57)}});
        double stopError=0;for(int ch=0;ch<2;++ch)for(int n=913+2*reset.latencySamples();n<4096;++n)stopError=std::max(stopError,std::abs(double(stopped[ch][n])-reference[ch][n]));CHECK(stopError==0);
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::flanger);
    for(auto& layer:full.synth.layers)for(auto& route:layer.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine engine;CHECK(engine.setPatch(full)&&engine.prepare(rate,block,quality));
        juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;engine.render(output,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& e:next.effects)e.flanger={n%2?.05f:5.f,n%2?0.f:1.f,n%2?1.f:8.f,n%2?-.8f:.8f,n%2?.25f:1.f};
            accepted&=engine.setPatch(next);engine.render(output,n%19==0?stop:empty,120);if(n%23==0)engine.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);
        std::cout<<"FLANGER_CALLBACK rate="<<rate<<" block="<<block<<" quality="<<int(quality)<<" first_plus_live=97 layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
}


void effect_flanger_kernel_availability()
{
    section("effect_flanger_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(8),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(p));
}

bool stereoBits(const Audio& a,const Audio& b,int start=0)
{
    if(a[0].size()!=b[0].size())return false;
    for(int ch=0;ch<2;++ch)for(std::size_t n=std::size_t(start);n<a[ch].size();++n)
        if(std::bit_cast<std::uint32_t>(a[ch][n])!=std::bit_cast<std::uint32_t>(b[ch][n]))return false;
    return true;
}
void effect_stereo_algebra_and_neutral()
{
    section("effect_stereo_algebra_and_neutral");
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto dry=flat();dry.synth.layers[0].pan=.43f;
        const auto capture=[&](const RoutedPatch& patch){RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));return render(e,2048,{{0,on(57)},{911,off(57)}},73);};
        const auto reference=capture(dry);CHECK(finite(reference)&&energy(reference)>1e-9);
        auto patch=dry;add(patch,EffectKind::stereo);CHECK(stereoBits(reference,capture(patch)));
        patch.effects[0].bypassed=true;patch.effects[0].stereo={12,.9f,2,StereoChannelMode::mono};CHECK(stereoBits(reference,capture(patch)));patch.effects[0].bypassed=false;
        patch.effects[0].stereo={0,0,1,StereoChannelMode::swap};const auto swapped=capture(patch);
        bool exactSwap=true;for(int ch=0;ch<2;++ch)for(std::size_t n=0;n<reference[ch].size();++n)exactSwap&=std::bit_cast<std::uint32_t>(swapped[ch][n])==std::bit_cast<std::uint32_t>(reference[1-ch][n]);CHECK(exactSwap);
        add(patch,EffectKind::stereo);patch.effects[1].stereo=patch.effects[0].stereo;CHECK(stereoBits(reference,capture(patch)));
        const std::array<StereoSettings,9> laws{{{0,0,0,StereoChannelMode::stereo},{0,0,1,StereoChannelMode::mono},{0,0,2,StereoChannelMode::stereo},
            {0,.5f,1,StereoChannelMode::stereo},{0,-1,1,StereoChannelMode::stereo},{0,1,1,StereoChannelMode::stereo},
            {-6,0,.5f,StereoChannelMode::swap},{12,-.25f,2,StereoChannelMode::stereo},{-24,0,1,StereoChannelMode::stereo}}};
        for(const auto& settings:laws){auto one=dry;add(one,EffectKind::stereo);one.effects[0].stereo=settings;const auto out=capture(one);
            const double g=std::pow(10.,double(settings.gainDb)/20),q=settings.channelMode==StereoChannelMode::mono?0:settings.channelMode==StereoChannelMode::swap?-1:1;
            const double a=(1+settings.width*q)*.5,b=(1-settings.width*q)*.5;
            const double left=g*(settings.pan>0?1-settings.pan:1),right=g*(settings.pan<0?1+settings.pan:1);
            double error=0;for(std::size_t n=0;n<out[0].size();++n){error=std::max(error,std::abs(double(out[0][n])-left*(a*reference[0][n]+b*reference[1][n])));
                error=std::max(error,std::abs(double(out[1][n])-right*(b*reference[0][n]+a*reference[1][n])));}
            CHECK(finite(out)&&error<=std::max(1e-8,peak(reference)*8e-7));CHECK(peak(out)<=7.962144*peak(reference)+1e-8);
        }
        for(bool gainPair:{false,true}){auto inverse=dry;add(inverse,EffectKind::stereo);add(inverse,EffectKind::stereo);
            inverse.effects[0].stereo=gainPair?StereoSettings{-6,0,1,StereoChannelMode::stereo}:StereoSettings{0,0,.5f,StereoChannelMode::stereo};
            inverse.effects[1].stereo=gainPair?StereoSettings{6,0,1,StereoChannelMode::stereo}:StereoSettings{0,0,2,StereoChannelMode::stereo};
            CHECK(difference(reference,capture(inverse))<=std::max(1e-8,peak(reference)*8e-7));}
        std::cout<<"STEREO_ALGEBRA rate="<<rate<<" quality="<<int(quality)<<" identity_bits=1 swap_twice_bits=1 source_peak="<<peak(reference)<<'\n';
    }
    auto mono=flat();add(mono,EffectKind::stereo);mono.effects[0].stereo.width=2;const auto output=sound(mono,2048);
    double sides=0;for(std::size_t n=0;n<output[0].size();++n)sides=std::max(sides,std::abs(double(output[0][n])-output[1][n]));CHECK(sides<1e-7);
    auto valid=mono;for(int field=0;field<4;++field)for(float bad:{-100.f,100.f,std::numeric_limits<float>::quiet_NaN()}){
        auto invalid=valid;auto& s=invalid.effects[7].stereo;
        if(field==0)s.gainDb=bad;else if(field==1)s.pan=bad;else if(field==2)s.width=bad;else{s.channelMode=static_cast<StereoChannelMode>(field==3?255:3);}
        CHECK(!validateRoutedPatch(invalid));}
}
void effect_stereo_independent_ramp_and_history()
{
    section("effect_stereo_independent_ramp_and_history");
    for(double rate:{8000.,44100.,48000.,96000.}){
        auto dry=flat();dry.synth.layers[0].pan=-.38f;auto patch=dry;add(patch,EffectKind::stereo);
        RoutedSynthEngine original,live;CHECK(original.setPatch(dry)&&live.setPatch(patch)&&original.prepare(rate,512)&&live.prepare(rate,512));
        std::array<double,4> values{1,0,1,1},targets=values,steps{};std::array<int,4> left{};double error=0;
        const int duration=int(std::ceil(.005*rate));
        for(int edit=0;edit<26;++edit){if(edit){patch.effects[0].stereo={edit%2?-12.f:6.f,edit%3==0?-.7f:.8f,edit%2?.3f:2.f,edit%3==0?StereoChannelMode::mono:edit%3==1?StereoChannelMode::swap:StereoChannelMode::stereo};}
            CHECK(live.setPatch(patch));const auto& s=patch.effects[0].stereo;
            const std::array<double,4> next{std::pow(10.,double(s.gainDb)/20),s.pan,s.width,s.channelMode==StereoChannelMode::stereo?1.:s.channelMode==StereoChannelMode::swap?-1.:0.};
            for(int k=0;k<4;++k)if(next[k]!=targets[k]){targets[k]=next[k];left[k]=duration;steps[k]=(next[k]-values[k])/duration;}
            // Re-arm immediately around completion, and after a long idle span.
            // Keep the original interrupted/settled cases before these additions.
            const std::array<int,8> boundaries{duration-1,duration,duration+1,4*duration+71,1,2,duration+1,3*duration+19};
            const int count=edit<18?(edit%3==0?std::max(7,duration/3):duration+37):boundaries[std::size_t(edit-18)];
            const auto input=render(original,count,edit?std::vector<Event>{}:std::vector<Event>{{0,on(61)}},73),output=render(live,count,edit?std::vector<Event>{}:std::vector<Event>{{0,on(61)}},73);
            for(int n=0;n<count;++n){for(int k=0;k<4;++k)if(left[k]>0){values[k]+=steps[k];if(--left[k]==0)values[k]=targets[k];}
                const double a=(1+values[2]*values[3])*.5,b=(1-values[2]*values[3])*.5;
                error=std::max(error,std::abs(double(output[0][n])-values[0]*(1-std::max(values[1],0.))*(a*input[0][n]+b*input[1][n])));
                error=std::max(error,std::abs(double(output[1][n])-values[0]*(1+std::min(values[1],0.))*(b*input[0][n]+a*input[1][n])));}
            CHECK(live.setPatch(patch)); // Repeated targets must not restart the interrupted clock.
        }
        CHECK(error<2e-7);std::cout<<"STEREO_RAMP rate="<<rate<<" internal_frames="<<duration<<" all4_error="<<error<<'\n';
    }
    for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto patch=impulsePatch();add(patch,EffectKind::stereo);add(patch,EffectKind::delay);patch.effects[1].delay={false,.05f,.5f,0,0,20000,1};
        RoutedSynthEngine live,control;CHECK(live.setPatch(patch)&&control.setPatch(patch)&&live.prepare(48000,512,quality)&&control.prepare(48000,512,quality));
        render(live,97,impulseEvents);render(control,97,impulseEvents);patch.effects[0].stereo.channelMode=StereoChannelMode::swap;CHECK(live.setPatch(patch));
        const auto observed=render(live,4096,{},73),expected=render(control,4096,{},73);
        CHECK(peak(expected)>1e-7&&finite(observed)&&difference(observed,expected)<1e-10);
        std::cout<<"STEREO_DOWNSTREAM_HISTORY quality="<<int(quality)<<" delayed_peak="<<peak(expected)<<" preserved_error="<<difference(observed,expected)<<'\n';
    }
}
void effect_stereo_clock_stop_and_callbacks()
{
    section("effect_stereo_clock_stop_and_callbacks");auto p=flat();p.synth.layers[0].pan=.3f;add(p,EffectKind::stereo);
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto run=[&](int partition){auto patch=p;RoutedSynthEngine engine;CHECK(engine.setPatch(patch)&&engine.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(engine,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(1001,{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].stereo={n%2?-24.f:12.f,n%2?-1.f:1.f,n%2?0.f:2.f,static_cast<StereoChannelMode>(n%3)};CHECK(engine.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(engine.setPatch(patch));append(37);}
            const auto held=engine.patch();auto invalid=held;invalid.effects[7].stereo.width=2.01f;CHECK(!engine.setPatch(invalid)&&engine.patch()==held);CHECK(!engine.prepare(7999,512));
            juce::AudioBuffer<float> empty(2,0);juce::MidiBuffer midi;engine.render(empty,midi);
            patch.effects[0].bypassed=true;CHECK(engine.setPatch(patch));append(int(rate*.02));patch.effects[0].stereo={6,.4f,.5f,StereoChannelMode::swap};CHECK(engine.setPatch(patch));append(int(rate*.02));patch.effects[0].bypassed=false;CHECK(engine.setPatch(patch));append(int(rate*.04));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});engine.reset();append(1501,{{0,on(65)},{801,off(65)}});CHECK(finite(output)&&energy(output)>1e-9);return output;};
        const auto a=run(512),b=run(64),c=run(73);CHECK(stereoBits(a,b)&&stereoBits(a,c));
        RoutedSynthEngine live,fresh;auto changed=p;changed.effects[0].stereo={6,.7f,2,StereoChannelMode::swap};
        CHECK(live.setPatch(p)&&fresh.setPatch(changed)&&live.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));render(live,2001,{{0,on(69)}});
        CHECK(live.setPatch(changed));render(live,std::max(1,int(rate*.0017)));const auto stopped=render(live,4096,{{0,juce::MidiMessage::midiStop()},{113,on(57)}},73),reference=render(fresh,4096,{{113,on(57)}},73);
        CHECK(stereoBits(stopped,reference,2*live.latencySamples()));live.reset();fresh.reset();CHECK(stereoBits(render(live,4096,{{0,on(65)}}),render(fresh,4096,{{0,on(65)}})));
        std::cout<<"STEREO_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits=1 Stop_snap_all4=1 FIR_support="<<2*live.latencySamples()<<'\n';
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::stereo);
    for(auto& layer:full.synth.layers)for(auto& route:layer.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine engine;CHECK(engine.setPatch(full)&&engine.prepare(rate,block,quality));
        juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;engine.render(output,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& e:next.effects)e.stereo={n%2?-6.f:6.f,n%2?-.7f:.8f,n%2?.5f:2.f,static_cast<StereoChannelMode>(n%3)};
            accepted&=engine.setPatch(next);engine.render(output,n%19==0?stop:empty,120);if(n%23==0)engine.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);
        std::cout<<"STEREO_CALLBACK rate="<<rate<<" block="<<block<<" quality="<<int(quality)<<" first_plus_live=97 layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
    auto maximum=p;maximum.effects[0].stereo={12,-1,2,StereoChannelMode::mono};RoutedSynthEngine engine;CHECK(engine.setPatch(maximum)&&engine.prepare(384000,512,RenderQuality::higher));CHECK(finite(render(engine,1024,{{0,on()}},73)));
}

void effect_stereo_kernel_availability()
{
    section("effect_stereo_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(9),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(p));
}

struct DimensionOracle
{
    std::vector<double> history;
    std::array<double,4> current{},target{},increment{};
    std::array<int,4> remaining{};
    double phase=0,rate;
    explicit DimensionOracle(double fs,const DimensionSettings& d):rate(fs){current=target={d.depth,d.rateHz,d.width,d.mix};}
    void set(const DimensionSettings& d){const std::array<double,4> next{d.depth,d.rateHz,d.width,d.mix};
        const int frames=int(std::ceil(.005*rate));for(int k=0;k<4;++k)if(next[k]!=target[k]){target[k]=next[k];remaining[k]=frames;increment[k]=(target[k]-current[k])/frames;}}
    double tap(double delay)const{const double position=double(history.size()-1)-delay;const auto lower=static_cast<std::int64_t>(std::floor(position));
        const auto sample=[&](std::int64_t n){return n<0?0.:history[static_cast<std::size_t>(n)];};return sample(lower)+(position-lower)*(sample(lower+1)-sample(lower));}
    std::array<double,2> sample(double l,double r){for(int k=0;k<4;++k)if(remaining[k]>0){current[k]+=increment[k];if(--remaining[k]==0)current[k]=target[k];}
        history.push_back((l+r)*.5);const double offset=.004*current[0]*std::sin(phase);
        const double side=(tap((.012+offset)*rate)-tap((.012-offset)*rate))*.5;
        const double delta=current[3]*current[2]*.5*side;
        phase=std::fmod(phase+2*std::numbers::pi*current[1]/rate,2*std::numbers::pi);
        return {l+delta,r-delta};}
};
void effect_dimension_response_and_fold()
{
    section("effect_dimension_response_and_fold");
    for(double rate:{8000.,44100.,48000.,96000.})for(float pan:{0.f,-1.f,.43f}){
        auto source=flat();source.synth.layers[0].pan=pan;auto patch=source;add(patch,EffectKind::dimension);patch.effects[0].dimension={1,2,2,1};
        const auto capture=[&](const RoutedPatch& q){RoutedSynthEngine e;CHECK(e.setPatch(q)&&e.prepare(rate,512));return render(e,4096,{{0,on(57)}},73);};
        const auto dry=capture(source),wet=capture(patch);DimensionOracle oracle(rate,patch.effects[0].dimension);double error=0,fold=0,side=0;
        for(std::size_t n=0;n<dry[0].size();++n){const auto expected=oracle.sample(dry[0][n],dry[1][n]);for(int ch=0;ch<2;++ch)error=std::max(error,std::abs(double(wet[ch][n])-expected[ch]));
            fold=std::max(fold,std::abs((double(wet[0][n])+wet[1][n])-(double(dry[0][n])+dry[1][n])));
            side=std::max(side,std::abs((double(wet[0][n])-wet[1][n])-(double(dry[0][n])-dry[1][n])));}
        CHECK(finite(wet)&&error<std::max(1e-8,peak(dry)*5e-7)&&fold<=peak(dry)*1e-6&&side>1e-5&&peak(wet)<=2*peak(dry)+1e-8);
        std::cout<<"DIMENSION_ORACLE rate="<<rate<<" pan="<<pan<<" independent_delay_error="<<error<<" fold_error="<<fold<<" generated_side="<<side<<'\n';
    }
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto source=flat();source.synth.layers[0].pan=.37f;const auto capture=[&](const RoutedPatch& patch,bool notes=true){RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));return render(e,2048,notes?std::vector<Event>{{0,on(61)}}:std::vector<Event>{},73);};
        const auto dry=capture(source);for(int zero=0;zero<3;++zero){auto patch=source;add(patch,EffectKind::dimension);auto& d=patch.effects[0].dimension;
            if(zero==0)d.depth=0;else if(zero==1)d.width=0;else d.mix=0;CHECK(stereoBits(dry,capture(patch)));}
        auto patch=source;add(patch,EffectKind::dimension);patch.effects[0].bypassed=true;CHECK(stereoBits(dry,capture(patch)));patch.effects[0].bypassed=false;
        const auto silent=capture(patch,false);CHECK(peak(silent)==0&&finite(silent));
        const auto wet=capture(patch);double fold=0;for(std::size_t n=0;n<wet[0].size();++n)fold=std::max(fold,std::abs((double(wet[0][n])+wet[1][n])-(double(dry[0][n])+dry[1][n])));
        CHECK(fold<=peak(dry)*1e-6&&finite(wet));
        auto impulse=impulsePatch();add(impulse,EffectKind::dimension);impulse.effects[0].dimension={1,2,2,1};RoutedSynthEngine e;CHECK(e.setPatch(impulse)&&e.prepare(rate,512,quality));
        const int support=int(std::ceil(detail::dimensionTailSeconds(impulse.effects[0].dimension,rate*(quality==RenderQuality::higher?2:1))*rate))+2*e.latencySamples();
        const auto tail=render(e,support+2048,impulseEvents,73);double residual=0;for(int ch=0;ch<2;++ch)for(std::size_t n=std::size_t(support);n<tail[ch].size();++n)residual=std::max(residual,std::abs(double(tail[ch][n])));CHECK(energy(tail)>1e-10&&residual==0);
    }
    auto valid=flat();add(valid,EffectKind::dimension);for(int field=0;field<4;++field)for(float bad:{-100.f,100.f,std::numeric_limits<float>::quiet_NaN()}){
        auto invalid=valid;auto& d=invalid.effects[7].dimension;if(field==0)d.depth=bad;else if(field==1)d.rateHz=bad;else if(field==2)d.width=bad;else d.mix=bad;CHECK(!validateRoutedPatch(invalid));}
    auto left=flat();left.synth.layers[0].pan=-1;add(left,EffectKind::dimension);const auto cross=sound(left,4096);CHECK(energy(cross)>1e-9);double right=0;for(float x:cross[1])right+=double(x)*x;CHECK(right>1e-8);
}
void effect_dimension_interrupted_control_and_clock()
{
    section("effect_dimension_interrupted_control_and_clock");
    for(double rate:{8000.,44100.,48000.,96000.}){
        auto dry=flat();dry.synth.layers[0].pan=-.38f;auto patch=dry;add(patch,EffectKind::dimension);patch.effects[0].dimension={.6f,.35f,1,0};
        RoutedSynthEngine original,live;CHECK(original.setPatch(dry)&&live.setPatch(patch)&&original.prepare(rate,512)&&live.prepare(rate,512));DimensionOracle oracle(rate,patch.effects[0].dimension);double error=0;
        for(int edit=0;edit<18;++edit){if(edit)patch.effects[0].dimension={edit%2?0.f:1.f,edit%2?.05f:2.f,edit%3==0?0.f:2.f,edit%3==1?0.f:1.f};
            CHECK(live.setPatch(patch));oracle.set(patch.effects[0].dimension);const int count=edit%3==0?std::max(7,int(rate*.0017)):int(rate*.0073)+37;
            const auto input=render(original,count,edit?std::vector<Event>{}:std::vector<Event>{{0,on(61)}},73),output=render(live,count,edit?std::vector<Event>{}:std::vector<Event>{{0,on(61)}},73);
            for(int n=0;n<count;++n){const auto expected=oracle.sample(input[0][n],input[1][n]);for(int ch=0;ch<2;++ch)error=std::max(error,std::abs(double(output[ch][n])-expected[ch]));}
            CHECK(live.setPatch(patch));oracle.set(patch.effects[0].dimension);
        }
        CHECK(error<2e-7);std::cout<<"DIMENSION_MOVING rate="<<rate<<" all4_ramp_error="<<error<<" activeMix0_history_oracle=1\n";
    }
    auto p=flat();add(p,EffectKind::dimension);
    for(double rate:{8000.,44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto run=[&](int partition){auto patch=p;RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));Audio result;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(e,count,events,partition);for(int ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),a[ch].begin(),a[ch].end());};
            append(1701,{{0,on(57)},{31,on(81)}});for(int n=0;n<12;++n){patch.effects[0].dimension={n%2?0.f:1.f,n%2?.05f:2.f,n%3?2.f:0.f,n%3==1?0.f:1.f};CHECK(e.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(e.setPatch(patch));append(37);}
            const auto held=e.patch();auto invalid=held;invalid.effects[7].dimension.depth=1.01f;CHECK(!e.setPatch(invalid)&&e.patch()==held);CHECK(!e.prepare(7999,512));juce::AudioBuffer<float> empty(2,0);juce::MidiBuffer midi;e.render(empty,midi);
            patch.effects[0].bypassed=true;CHECK(e.setPatch(patch));append(int(rate*.02));patch.effects[0].dimension={.8f,1.7f,1.8f,.7f};CHECK(e.setPatch(patch));append(int(rate*.02));patch.effects[0].bypassed=false;CHECK(e.setPatch(patch));append(int(rate*.04));append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});e.reset();append(1501,{{0,on(65)},{801,off(65)}});CHECK(finite(result));return result;};
        const auto a=run(512),b=run(64),c=run(73);CHECK(stereoBits(a,b)&&stereoBits(a,c));
        auto changed=p;changed.effects[0].dimension={1,2,2,1};RoutedSynthEngine live,fresh;CHECK(live.setPatch(p)&&fresh.setPatch(changed)&&live.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));render(live,2001,{{0,on(69)}});CHECK(live.setPatch(changed));render(live,std::max(1,int(rate*.0017)));
        CHECK(stereoBits(render(live,4096,{{0,juce::MidiMessage::midiStop()},{113,on(57)}},73),render(fresh,4096,{{113,on(57)}},73),2*live.latencySamples()));live.reset();fresh.reset();CHECK(stereoBits(render(live,4096,{{0,on(65)}}),render(fresh,4096,{{0,on(65)}})));
        std::cout<<"DIMENSION_CLOCK rate="<<rate<<" quality="<<int(quality)<<" partition_bits=1 Stop_all4_snap=1 FIR_support="<<2*live.latencySamples()<<'\n';
    }
}
void effect_dimension_first_live_allocation()
{
    section("effect_dimension_first_live_allocation");auto full=flat();for(int n=0;n<3;++n)addLayer(full);for(int n=0;n<8;++n)add(full,EffectKind::dimension);
    for(int i=0;i<4;++i){full.synth.layers[i].engine=i%2?EngineKind::wavetable:EngineKind::analog;for(auto& r:full.synth.layers[i].modulation){r.enabled=true;r.source=ModSource::lfo1;r.target=ModTarget::pan;r.bank=ModBank::common;r.amount=.005f;}}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine e;CHECK(e.setPatch(full)&&e.prepare(rate,block,quality));juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;e.render(output,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(int s=0;s<8;++s)next.effects[s].dimension={n%2?.1f:.9f,.1f+.2f*s,n%2?.5f:2.f,n%2?0.f:.8f};accepted&=e.setPatch(next);e.render(output,n%19==0?stop:empty,120);if(n%23==0)e.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);std::cout<<"DIMENSION_CALLBACK rate="<<rate<<" block="<<block<<" quality="<<int(quality)<<" mixed_layers=4 notes=16 fx=8 routes=64 callbacks=97 alloc="<<allocations<<" frees="<<frees<<'\n';
#else
        CHECK(false);
#endif
    }
    RoutedSynthEngine e;CHECK(e.setPatch(full)&&e.prepare(384000,512,RenderQuality::higher));CHECK(finite(render(e,1024,{{0,on()}},73)));
}

void effect_dimension_kernel_availability()
{
    section("effect_dimension_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(10),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;
    CHECK(validateRoutedPatch(p));
}
void sharedModulationPrechangeGoldens()
{
    section("sharedModulationPrechangeGoldens");
    // Captured from the ordinary, unoptimized protected preimage. These are
    // complete float-bit boundaries, never recomputed by the candidate engine.
#if defined(_DEBUG)
    const std::array<std::uint64_t,6> expected {12604871295369152542ull,14383237287056653774ull,17247480399641137088ull,13288251697001036892ull,960090315365237036ull,15615377690878284221ull};
#else
    const std::array<std::uint64_t,6> expected {12604871295369152542ull,14383237287056653774ull,17247480399641137088ull,13288251697001036892ull,960090315365237036ull,15615377690878284221ull};
#endif
    const char* capture=std::getenv("AIRACK_SHARED_MODULATION_GOLDEN_CAPTURE");
    std::size_t index=0;
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto initial=flat();for(int i=1;i<4;++i)addLayer(initial);
        initial.outputProtection=false;initial.synth.masterLevel=.47f;
        const auto routes=[](LayerSettings& l,int sources,bool zero=false){
            for(std::size_t i=0;i<l.modulation.size();++i){auto& r=l.modulation[i];r.enabled=true;r.target=static_cast<ModTarget>(i+1);
                r.source=sources==0?(i%2?ModSource::lfo2:ModSource::lfo1):sources==1?static_cast<ModSource>(i%4==0?1:i%4==1?2:i%4==2?5:6):sources==2?(i%3?ModSource::lfo1:ModSource::velocity):ModSource::lfo2;
                r.bank=i<2?ModBank::common:(r.target==ModTarget::osc1Width||r.target==ModTarget::osc2Width)?ModBank::analog:(r.target==ModTarget::osc1Position||r.target==ModTarget::osc2Position)?ModBank::wavetable:l.engine==EngineKind::analog?ModBank::analog:ModBank::wavetable;
                r.amount=zero?0.f:float(i%3==0?-.021:.013)*(1.f+float(i)*.02f);}
        };
        for(std::size_t i=0;i<4;++i){auto& l=initial.synth.layers[i];l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.level=.17f;l.pan=float(i)*.43f-.65f;
            l.cutoffHz=750+float(i)*1100;l.resonance=.17f+float(i)*.1f;l.filterEnvelopeOctaves=1.3f;
            l.ampEnvelope={.003f+float(i)*.002f,.027f+float(i)*.011f,.043f+float(i)*.017f,.58f};
            l.filterEnvelope={.005f,.036f,.041f,.34f+float(i)*.07f};
            l.lfos[0]={LfoShape::sine,false,.37f+float(i)*.21f,1};l.lfos[1]={LfoShape::triangle,false,.79f+float(i)*.17f,.5f};
            for(std::size_t j=0;j<2;++j){auto& o=l.oscillators[j];o.level=j?.31f:.43f;o.waveform=j?Waveform::pulse:Waveform::saw;o.position=.2f+float(i)*.1f;o.tune=float(i)-float(j)*5;o.detune=float(i)*9-float(j)*17;}
            routes(l,0);
        }
        const auto phrase=[&](RoutedSynthEngine& e,int partition){
            CHECK(e.setPatch(initial)&&e.prepare(rate,512,quality));Audio result;
            const auto append=[&](int count,const std::vector<Event>& events={},std::optional<double> tempo=120.){const auto part=render(e,count,events,partition,tempo);for(int ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),part[ch].begin(),part[ch].end());};
            std::vector<Event> notes;for(int n=0;n<16;++n)notes.emplace_back(n*11,on(43+n,1+n%3,.15f+float(n)*.047f));
            append(int(rate*.019)+197,notes);CHECK(e.activeNoteCount()==16);auto patch=initial;
            append(197,{{17,on(84,8,.79f)},{73,on(86,9,.33f)}});CHECK(e.activeNoteCount()==16);
            auto invalid=patch;invalid.synth.layers[0].modulation[0].source=ModSource::none;CHECK(!e.setPatch(invalid)&&e.patch().synth==patch.synth);
            // All four voice sources and mixed routes have diverse note/velocity
            // and envelope states. These MUST retain the original voice path.
            for(auto& l:patch.synth.layers)routes(l,1);CHECK(e.setPatch(patch));append(int(rate*.012)+37);
            for(auto& l:patch.synth.layers)routes(l,2);CHECK(e.setPatch(patch));append(int(rate*.0013)+13);
            // Interrupt the pending route mode, then return to shared LFOs.
            for(auto& l:patch.synth.layers)routes(l,3);CHECK(e.setPatch(patch));append(int(rate*.014)+31);
            for(auto& l:patch.synth.layers)routes(l,0,true);CHECK(e.setPatch(patch));append(int(rate*.009)+19);
            for(auto& l:patch.synth.layers){for(auto& r:l.modulation)r.enabled=false;}CHECK(e.setPatch(patch));append(int(rate*.011)+23);
            // Engine and bank edits classify the actual audible mode, including
            // the fade interval where requested and current routes differ.
            for(auto& l:patch.synth.layers){l.engine=l.engine==EngineKind::analog?EngineKind::wavetable:EngineKind::analog;routes(l,2);}CHECK(e.setPatch(patch));append(int(rate*.0017)+17);
            for(auto& l:patch.synth.layers){routes(l,0);l.lfos[0].synced=true;l.lfos[1].synced=true;}CHECK(e.setPatch(patch));append(int(rate*.013)+29,{},std::nullopt);
            append(int(rate*.009)+7,{},97.);CHECK(e.setPatch(patch));append(71);
            patch.synth.performance={VoiceMode::monophonic,.023f,true};CHECK(e.setPatch(patch));append(int(rate*.014)+37,{{11,on(74,4,.83f)},{73,on(50,7,.22f)}});
            for(auto& l:patch.synth.layers)routes(l,1);CHECK(e.setPatch(patch));append(int(rate*.013)+41,{{17,on(67,2,.61f)},{117,off(50,7)}});
            patch.synth.performance.mode=VoiceMode::polyphonic;for(auto& l:patch.synth.layers)routes(l,0);CHECK(e.setPatch(patch));append(int(rate*.014)+13,{{7,on(82,1,.77f)},{71,off(43,1)}});
            append(int(rate*.016)+89,{{17,juce::MidiMessage::midiStop()},{73,on(63,5,.48f)}});
            e.reset();CHECK(e.setPatch(initial));e.reset();append(int(rate*.017)+97,{{23,on(64,2,.52f)},{int(rate*.008)+41,off(64,2)}});
            CHECK(e.prepare(rate,512,quality));append(int(rate*.019)+53,{{17,on(56,3,.71f)}});
            return result;
        };
        RoutedSynthEngine first,coarse,other;const auto a=phrase(first,64),b=phrase(coarse,512),c=phrase(other,73);
        CHECK(finite(a)&&energy(a)>1e-9&&stereoBits(a,b)&&stereoBits(a,c));
        std::uint64_t hash=14695981039346656037ull;for(const auto& channel:a)for(float value:channel){const auto bits=std::bit_cast<std::uint32_t>(value);for(int byte=0;byte<4;++byte){hash^=(bits>>(8*byte))&255;hash*=1099511628211ull;}}
        if(capture){const juce::File folder{juce::String(capture)};const bool ready=folder.createDirectory().wasOk();CHECK(ready);if(ready){const auto file=folder.getChildFile("shared-modulation-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");const bool fresh=!file.existsAsFile();CHECK(fresh);if(fresh){std::vector<float> raw;for(const auto& channel:a)raw.insert(raw.end(),channel.begin(),channel.end());CHECK(file.replaceWithData(raw.data(),raw.size()*sizeof(float)));}}}
        else CHECK(hash==expected[index]);
        std::cout<<"SHARED_MODULATION_GOLDEN rate="<<rate<<" quality="<<int(quality)<<" frames="<<a[0].size()<<" fnv="<<hash<<" partition_bits=1 diverse_voice_sources=1 transitions=1 stop_reset=1\n";++index;
    }
}

struct FrequencyShiftReference {double rate,tail;std::size_t count;std::array<double,24> coefficients;};
const std::array<FrequencyShiftReference,9> frequencyShiftReference {{
    {8000.0,1.4206249999999998,12,{{0.04754668869241305,0.17332290294099867,0.33827071314461693,0.5037203966218702,0.6457318105293272,0.756079409350159,0.8365033548042374,0.8928757546318834,0.9316740006661889,0.9585032859042403,0.9777921794716119,0.9930057142854922}}},
    {16000.0,1.5396249999999998,14,{{0.0448108863617278,0.1642000973877088,0.3228206400999307,0.4846222234763798,0.6260974811388617,0.7380653010392375,0.821024808139148,0.879886932284676,0.920526701094129,0.9481748186262331,0.9669436266171393,0.9798820311841674,0.989213518679212,0.9966035669521143}}},
    {44100.0,1.5874036281179138,16,{{0.046716557461946334,0.17055849706305817,0.33359014572416723,0.4979097205587107,0.6396676957941205,0.7503070794622282,0.831146363366513,0.887701955670285,0.9261649162591433,0.9518581120906665,0.9688438494766906,0.9800319956737561,0.9874371671278721,0.9924356991146793,0.9959759840578175,0.9987391859842653}}},
    {48000.0,1.5703958333333332,16,{{0.047780126321606,0.17408750410934865,0.33951988576238007,0.5051608636994737,0.6470122583982464,0.7568958838771531,0.8365964162051477,0.8919662372201248,0.9293731575423823,0.954202739688341,0.9705169630404279,0.9811977509617276,0.9882237675940876,0.9929363763546603,0.9962529928460793,0.9988278089892215}}},
    {88200.0,1.7019727891156462,18,{{0.044673645051845345,0.16373829968399978,0.32202620810337024,0.48361575206586743,0.6250211486956877,0.7370102884169182,0.8200107640944602,0.8788686101975945,0.9194029162516018,0.9467871258648619,0.9650598722443905,0.9771614732912048,0.9851478575685324,0.990423375007294,0.9939364663311955,0.9963279018239828,0.998038069906365,0.9993839909177621}}},
    {96000.0,1.6853125,18,{{0.0456028837480649,0.16684719211432275,0.32731396054837686,0.4901755424717115,0.6317673805159241,0.743154905732569,0.8251689097546955,0.8829627464043136,0.9225275467558612,0.949105661329484,0.9667445005864842,0.9783649957904348,0.9859942845518377,0.9910078014083034,0.9943286619737028,0.9965767421444789,0.9981754837313247,0.9994278949102464}}},
    {192000.0,1.7978802083333332,20,{{0.04388394677437089,0.16108728760109514,0.3174945616692689,0.47796015170502737,0.6191671507866383,0.7316428399142934,0.8154740796832404,0.8752407647355714,0.916608756392984,0.9446864946418859,0.9634997849447791,0.9760004891342651,0.9842633999985086,0.9897097961021749,0.9932982335274979,0.9956697221225042,0.9972520095256773,0.9983323207646341,0.9991075996026306,0.9997196017918106}}},
    {384000.0,1.7898515625,21,{{0.04641160327449679,0.1695436304858677,0.331877084461135,0.4958024397957329,0.63751704175646,0.7483574772577672,0.8295070051367874,0.8863812061765527,0.9251138954908602,0.9510011360006143,0.9680929395543779,0.9792888770321941,0.9865859887484633,0.9913276160198883,0.9944043456537149,0.9964015221186877,0.9977022527756696,0.9985573823660536,0.9991323948176993,0.9995388216694444,0.999855618988906}}},
    {768000.0,1.9005677083333332,23,{{0.044830308793486984,0.16426316987244247,0.3229206895132277,0.48472774989654804,0.6261667009929053,0.7380542375892017,0.8208853455270687,0.8795576454424922,0.919918796858605,0.9471527851294094,0.9652976584358098,0.9772869958932069,0.9851663639228412,0.9903266874088485,0.9936989662415227,0.9959001997022758,0.9973367955262183,0.998275550695678,0.9988914726608732,0.9992996710930918,0.9995765592640568,0.9997740263906201,0.9999291061044602}}},
}};
// Frozen scalar coefficients below came from the independently held exact-AGM
// tool, before native preparation existed; the signal oracle uses exact trig.
void effect_frequency_shift_preparation_and_physical_oracle()
{
    section("effect_frequency_shift_preparation_and_physical_oracle");
    for(const auto& row:frequencyShiftReference){
        detail::FrequencyShiftPreparation prepared;CHECK(detail::prepareFrequencyShift(row.rate,prepared));
        CHECK(prepared.count==row.count&&prepared.count<=24&&std::abs(prepared.tailSeconds-row.tail)<1e-10);
        for(std::size_t n=0;n<row.count;++n)CHECK(std::abs(prepared.coefficients[n]-row.coefficients[n])<1e-12);
        double worst=0;
        for(int n=0;n<=1000;++n){const double hz=20+(.5*row.rate-40)*n/1000;
            const auto z=std::polar(1.,-2*std::numbers::pi*hz/row.rate),z2=z*z;
            std::complex<double> d=1,q=z;
            for(std::size_t k=0;k<prepared.count;++k){const double a=prepared.coefficients[k];const auto h=(a-z2)/(1.-a*z2);if(k%2)q*=h;else d*=h;}
            const double wanted=std::abs(d+std::complex<double>(0,1)*q),image=std::abs(d-std::complex<double>(0,1)*q);
            CHECK(wanted>1.9999);worst=std::max(worst,image/wanted);
        }
        CHECK(worst<=1e-4);std::cout<<"FREQUENCY_SHIFT_TRANSFER rate="<<row.rate<<" sections="<<prepared.count<<" worst_image_ratio="<<worst<<" finite_grid=1001 tail="<<prepared.tailSeconds<<'\n';
    }
    detail::FrequencyShiftPreparation held;held.count=13;CHECK(!detail::prepareFrequencyShift(7999,held)&&held.count==13);
    CHECK(!detail::prepareFrequencyShift(768001,held)&&held.count==13);
    for(double rate:{8000.,44100.,48000.,96000.})for(float hz:{-1000.f,-7.f,0.f,7.f,1000.f}){
        auto dry=flat();dry.synth.layers[0].pan=.27f;auto wet=dry;add(wet,EffectKind::frequencyShift);wet.effects[0].frequencyShift={hz,hz==7?37.f:0.f,1};
        const auto reference=sound(dry,12001,{{0,on(57)},{9011,off(57)}},rate,73),output=sound(wet,12001,{{0,on(57)},{9011,off(57)}},rate,73);
        const auto& row=*std::find_if(frequencyShiftReference.begin(),frequencyShiftReference.end(),[&](const auto& r){return r.rate==rate;});
        std::array<std::array<std::array<double,4>,24>,2> histories{};std::array<double,2> delayed{},phase{};double error=0;
        for(std::size_t n=0;n<reference[0].size();++n)for(int ch=0;ch<2;++ch){
            double d=reference[ch][n],q=d;
            for(std::size_t k=0;k<row.count;++k){auto& history=histories[ch][k];const auto parity=n%2;double& x2=history[parity];double& y2=history[2+parity];const double x=k%2?q:d,y=(row.coefficients[k]*x-x2)+row.coefficients[k]*y2;x2=x;y2=y;if(k%2)q=y;else d=y;}
            const double sample=d*std::cos(phase[ch])-delayed[ch]*std::sin(phase[ch]);delayed[ch]=q;
            phase[ch]=std::remainder(phase[ch]+2*std::numbers::pi*(hz+(ch?.5:-.5)*wet.effects[0].frequencyShift.stereoOffsetHz)/rate,2*std::numbers::pi);
            error=std::max(error,std::abs(double(output[ch][n])-sample));
        }
        CHECK(finite(output)&&energy(output)>1e-9&&error<1e-6);
        if(hz==0)CHECK(difference(output,reference)>1e-4); // Held phase still colours through the D cascade.
        std::cout<<"FREQUENCY_SHIFT_STARTUP rate="<<rate<<" hz="<<hz<<" exact_trig_error="<<error<<" source_start=0\n";
    }
}
void effect_frequency_shift_live_tail_stop_and_callbacks()
{
    section("effect_frequency_shift_live_tail_stop_and_callbacks");auto p=flat();add(p,EffectKind::frequencyShift);
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto run=[&](int partition){auto patch=p;RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(e,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(1001,{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].frequencyShift={n%2?-1000.f:1000.f,n%3?-100.f:100.f,n%2?0.f:1.f};CHECK(e.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(e.setPatch(patch));append(37);}
            const auto held=e.patch();auto bad=held;bad.effects[7].frequencyShift.hz=1001;CHECK(!e.setPatch(bad)&&e.patch()==held);CHECK(!e.prepare(7999,512));
            patch.effects[0].bypassed=true;CHECK(e.setPatch(patch));append(int(rate*.02));patch.effects[0].frequencyShift={7,37,.7f};CHECK(e.setPatch(patch));append(91);patch.effects[0].bypassed=false;CHECK(e.setPatch(patch));append(int(rate*.04));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});e.reset();append(1501,{{0,on(65)},{801,off(65)}});CHECK(finite(output)&&energy(output)>1e-9);return output;};
        const auto a=run(512),b=run(64),c=run(73);CHECK(stereoBits(a,b)&&stereoBits(a,c));
        auto changed=p;changed.effects[0].frequencyShift={-71,53,1};RoutedSynthEngine live,fresh;
        CHECK(live.setPatch(p)&&fresh.setPatch(changed)&&live.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));render(live,2001,{{0,on(69)}});CHECK(live.setPatch(changed));render(live,std::max(1,int(rate*.0017)));
        CHECK(stereoBits(render(live,4096,{{0,juce::MidiMessage::midiStop()},{113,on(57)}},73),render(fresh,4096,{{113,on(57)}},73),2*live.latencySamples()));live.reset();fresh.reset();CHECK(stereoBits(render(live,4096,{{0,on(65)}}),render(fresh,4096,{{0,on(65)}})));
        auto impulse=impulsePatch();add(impulse,EffectKind::frequencyShift);impulse.effects[0].frequencyShift={-1000,100,1};
        const double internal=rate*(quality==RenderQuality::higher?2:1),tail=detail::frequencyShiftTailSeconds(impulse.effects[0].frequencyShift,internal);CHECK(std::isfinite(tail)&&tail>1.4&&tail<2);
        RoutedSynthEngine e;CHECK(e.setPatch(impulse)&&e.prepare(rate,512,quality));const int boundary=int(std::ceil(tail*rate))+2*e.latencySamples();const auto result=render(e,boundary+800,impulseEvents,73);double residual=0;for(const auto& ch:result)for(int n=boundary;n<int(ch.size());++n)residual=std::max(residual,std::abs(double(ch[n])));CHECK(finite(result)&&peak(result)>1e-7&&residual<=peak(result)*1e-6);
        std::cout<<"FREQUENCY_SHIFT_LIVE rate="<<rate<<" quality="<<int(quality)<<" partition_bits=1 Stop_snap3=1 tail="<<tail<<" residual="<<residual<<'\n';
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::frequencyShift);
    for(auto& layer:full.synth.layers)for(auto& route:layer.modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=ModBank::analog;route.amount=.005f;}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;RoutedSynthEngine e;CHECK(e.setPatch(full)&&e.prepare(rate,block,quality));juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;e.render(output,notes,120);
        for(int n=0;n<96;++n){auto next=full;for(auto& fx:next.effects)fx.frequencyShift={n%2?-1000.f:1000.f,n%2?-100.f:100.f,n%2?0.f:1.f};accepted&=e.setPatch(next);e.render(output,n%19==0?stop:empty,120);if(n%23==0)e.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);std::cout<<"FREQUENCY_SHIFT_CALLBACK rate="<<rate<<" quality="<<int(quality)<<" layers=4 notes=16 fx=8 routes=64 alloc="<<allocations<<" frees="<<frees<<'\n';
#endif
    }
}

void effect_frequency_shift_kernel_availability()
{
    section("effect_frequency_shift_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(11),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;CHECK(validateRoutedPatch(p));
}
// Private Release test translation-unit access uses the exact owned production
// CPP. Static-library resolution supplies its Synth dependencies; no shipping
// API, build flag, vendor source or product tone injector is introduced.
void effect_frequency_shift_native_tones_and_edges()
{
#if !defined(_DEBUG)
    section("effect_frequency_shift_actual_native_internal_tone_projection");
    const auto exportPath=juce::SystemStats::getEnvironmentVariable("AIRACK_FREQUENCY_SHIFT_NATIVE_EVIDENCE",{});
    const juce::File folder(exportPath);if(exportPath.isNotEmpty()){CHECK(!folder.exists());if(folder.exists()||!folder.createDirectory().wasOk()){CHECK(false);return;}}
    for(double rate:{8000.,48000.,768000.}){
        auto effect=std::make_unique<airack::Effect>();CHECK(detail::prepareFrequencyShift(rate,effect->frequencyShiftPrepared));effect->prepare(rate);
        EffectSettings settings;settings.frequencyShift.mix=1;const int settle=int(std::ceil(effect->frequencyShiftPrepared.tailSeconds*rate)),count=int(rate);
        for(double frequency:{20.,440.,rate*.5-20}){
            std::array<std::array<std::complex<double>,3>,3> measured{};std::array<double,3> residual{};
            for(int variant=0;variant<3;++variant){const double shift=variant==0?0:variant==1?7:-7;settings.frequencyShift={float(shift),0,1};effect->settings(settings,int(.005*rate),true);effect->clear();
                std::vector<double> raw;raw.reserve(std::size_t(count)*2);std::array<std::complex<double>,3> projections{};double square=0,maxMono=0;bool allFinite=true;
                for(int frame=0;frame<settle+count;++frame){const double phase=2*std::numbers::pi*frequency*frame/rate,input=.1*std::cos(phase);effect->advanceFilter();const auto output=effect->process({input,input},EffectKind::frequencyShift,1,DriveShape::tanh,true,FilterFxMode::lowpass);
                    allFinite&=std::isfinite(output[0])&&std::isfinite(output[1]);maxMono=std::max(maxMono,std::abs(output[0]-output[1]));if(frame>=settle){raw.push_back(output[0]);raw.push_back(output[1]);square+=output[0]*output[0];for(int k=0;k<3;++k)projections[k]+=output[0]*std::polar(1.,-2*std::numbers::pi*(frequency+(k==0?0:k==1?7:-7))*frame/rate);}}
                CHECK(allFinite&&maxMono==0);for(int k=0;k<3;++k)measured[variant][k]=projections[k]*(2./count);double components=0;for(const auto& value:measured[variant])components+=.5*std::norm(value);residual[variant]=std::sqrt(std::max(0.,square/count-components));
                if(exportPath.isNotEmpty()){const auto file=folder.getChildFile("internal-"+juce::String(int(rate))+"-tone-"+juce::String(int(frequency))+"-shift-"+juce::String(int(shift))+".f64");const bool fresh=!file.exists();CHECK(fresh);if(fresh)CHECK(file.replaceWithData(raw.data(),raw.size()*sizeof(double)));}
            }
            const auto z=std::polar(1.,-2*std::numbers::pi*frequency/rate),z2=z*z;std::complex<double> d=1,q=z;
            for(std::size_t n=0;n<effect->frequencyShiftPrepared.count;++n){const double a=effect->frequencyShiftPrepared.coefficients[n];const auto h=(a-z2)/(1.-a*z2);if(n%2)q*=h;else d*=h;}
            CHECK(std::abs(measured[0][0]-.1*d)<1e-6);
            const double floor=std::max({std::abs(measured[0][1]),std::abs(measured[0][2]),residual[0],1e-13});
            for(int variant=1;variant<3;++variant){const int wanted=variant==1?1:2,image=variant==1?2:1;const double wa=std::abs(measured[variant][wanted]),ia=std::abs(measured[variant][image]);const auto expected=.05*(d+std::complex<double>(0,1)*q);CHECK(wa>.09&&std::abs(measured[variant][wanted]-expected)<1e-6&&((ia+floor)/wa)<=1e-4);
                std::cout.precision(17);std::cout<<"FREQUENCY_SHIFT_NATIVE internal_rate="<<rate<<" source_hz="<<frequency<<" shift="<<(variant==1?7:-7)<<" settle_frames="<<settle<<" measured_frames="<<count<<" wanted_real="<<measured[variant][wanted].real()<<" wanted_imag="<<measured[variant][wanted].imag()<<" image_real="<<measured[variant][image].real()<<" image_imag="<<measured[variant][image].imag()<<" common_floor="<<floor<<" residual="<<residual[variant]<<" image_below_floor="<<(ia<=floor)<<" phase_error="<<std::abs(measured[variant][wanted]-expected)<<" qualified_upper_ratio="<<((ia+floor)/wa)<<'\n';}
        }
        // Outside the physical source guard these are descriptive finite folding
        // probes, never image-rejection acceptance points. Offset verifies both
        // actual independent signed channel clocks through the same native stage.
        for(const auto probe:std::array<std::array<double,3>,3>{{{0,7,0},{rate*.5-3,7,0},{440,7,37}}}){settings.frequencyShift={float(probe[1]),float(probe[2]),1};effect->settings(settings,1,true);effect->clear();double peak=0,side=0;bool finite=true;std::array<std::complex<double>,2> wanted{};for(int frame=0;frame<settle+count;++frame){const double input=.1*std::cos(2*std::numbers::pi*probe[0]*frame/rate);effect->advanceFilter();const auto output=effect->process({input,input},EffectKind::frequencyShift,1,DriveShape::tanh,true,FilterFxMode::lowpass);for(int ch=0;ch<2;++ch){finite&=std::isfinite(output[ch]);peak=std::max(peak,std::abs(output[ch]));if(frame>=settle)wanted[ch]+=output[ch]*std::polar(1.,-2*std::numbers::pi*(probe[0]+probe[1]+(ch?.5:-.5)*probe[2])*frame/rate);}side=std::max(side,std::abs(output[0]-output[1]));}CHECK(finite&&peak>1e-3&&peak<.3);if(probe[2]!=0)CHECK(side>.01&&std::abs(wanted[0]*(2./count))>.08&&std::abs(wanted[1]*(2./count))>.08);std::cout<<"FREQUENCY_SHIFT_EDGE internal_rate="<<rate<<" source_hz="<<probe[0]<<" hz="<<probe[1]<<" offset="<<probe[2]<<" peak="<<peak<<" side="<<side<<" descriptive_only="<<(probe[2]==0)<<'\n';}
    }
#endif
}

void effect_convolution_kernel_availability()
{
    section("effect_convolution_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(12),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;CHECK(validateRoutedPatch(p));
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool convolutionDenyAllocation=false;
int __cdecl convolutionAllocationFailure(int kind,void*,size_t,int,long,const unsigned char*,int)
{ if(convolutionDenyAllocation&&kind==_HOOK_ALLOC){convolutionDenyAllocation=false;return 0;}return 1; }
#endif
void effect_convolution_resource_and_atomic_preparation()
{
    section("effect_convolution_resource_and_atomic_preparation");
#if !defined(_DEBUG) && defined(_WIN32)
    // Fresh owning process runs this entry before any impulse/whole tests.
    // Private committed memory includes allocator retention, FFT/queue/thread
    // overhead and SRC transients. Peak-minus-current-baseline is conservative
    // even when startup already established the process high-water mark.
    PROCESS_MEMORY_COUNTERS_EX before{},after{};before.cb=after.cb=sizeof(before);
    CHECK(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),sizeof(before))!=0);
    const auto start=juce::Time::getHighResolutionTicks();std::array<std::unique_ptr<airack::PreparedConvolution>,8> stages;
    for(std::size_t slot=0;slot<stages.size();++slot){auto& stage=stages[slot];stage=std::make_unique<airack::PreparedConvolution>();CHECK(stage->prepare(768000,airack::canonicalConvolutionRecords()));
        if(slot==0){PROCESS_MEMORY_COUNTERS_EX one{};one.cb=sizeof(one);CHECK(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&one),sizeof(one))!=0);
            const auto retained=one.PrivateUsage>before.PrivateUsage?one.PrivateUsage-before.PrivateUsage:0;const auto peak=one.PeakPagefileUsage>before.PrivateUsage?one.PeakPagefileUsage-before.PrivateUsage:0;
            CHECK(retained>0&&retained<=32*1024*1024&&peak>=retained&&peak<=64*1024*1024);std::cout<<"CONVOLUTION_RESOURCE stages=1 new_only_private_commit_increment="<<retained<<" conservative_process_peak_increment="<<peak<<'\n';}}
    CHECK(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after),sizeof(after))!=0);
    const auto retained=after.PrivateUsage>before.PrivateUsage?after.PrivateUsage-before.PrivateUsage:0;
    const auto peak=after.PeakPagefileUsage>before.PrivateUsage?after.PeakPagefileUsage-before.PrivateUsage:0;
    CHECK(retained>0&&retained<=8*32*1024*1024&&peak>=retained&&peak<=8*64*1024*1024);
    std::cout<<"CONVOLUTION_RESOURCE effective_rate=768000 stages=8 new_only_private_commit_increment="<<retained<<" conservative_process_peak_increment="<<peak<<" total_process_private_commit="<<after.PrivateUsage<<" total_process_peak_commit="<<after.PeakPagefileUsage<<" prepare_seconds="<<juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks()-start)<<'\n';
    for(auto& stage:stages){auto invalid=airack::canonicalConvolutionRecords();invalid[1].sha256="corrupt";CHECK(!stage->prepare(768000,invalid));CHECK(stage->engines[1]->getCurrentIRSize()==192000&&stage->engines[0]->getCurrentIRSize()==1);}
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
    auto p=flat();add(p,EffectKind::convolution);RoutedSynthEngine live,control;CHECK(live.setPatch(p)&&control.setPatch(p)&&live.prepare(48000,512)&&control.prepare(48000,512));
    CHECK(stereoBits(render(live,1901,{{0,on(57)}}),render(control,1901,{{0,on(57)}})));
    const auto old=_CrtSetAllocHook(convolutionAllocationFailure);convolutionDenyAllocation=true;const bool rejected=!live.prepare(96000,512,RenderQuality::higher);convolutionDenyAllocation=false;_CrtSetAllocHook(old);
    CHECK(rejected&&live.patch()==p&&live.effectiveQuality()==RenderQuality::realtime&&live.latencySamples()==0);
    CHECK(stereoBits(render(live,4096),render(control,4096)));
#endif
}

void effect_convolution_fixed_head_juce_bits()
{
#if !defined(_DEBUG)
    section("effect_convolution_fixed_head_juce_bits");
    // Independent installed JUCE64 reference, including padding and ring resets.
    // Both complete streams are retained; exact bits include signed zeros.
    std::vector<float> actual,expected;actual.reserve(3*1537*2);expected.reserve(3*1537*2);
    std::size_t mismatches=0;int resets=0;bool finite=true;
    for(int count:{1,23,64}){
        juce::AudioBuffer<float> pcm(2,count);std::array<std::array<float,64>,2> coefficients{};
        for(int ch=0;ch<2;++ch)for(int k=0;k<count;++k){const float c=count==1?(ch==0?1.f:-.5f):float(((k*17+ch*11)%29)-14)/128.f;
            pcm.setSample(ch,k,c);coefficients[ch][k]=c;}
        juce::dsp::ConvolutionMessageQueue queue{4};airack::ConvolutionChoice choice;
        CHECK(choice.prepare(pcm,48000,queue)&&choice.getCurrentIRSize()==count&&choice.getTailIRSize()==0&&choice.getLatency()==0);
        std::array<juce::dsp::FIR::Filter<float>,2> reference;
        for(int ch=0;ch<2;++ch){reference[ch].coefficients=new juce::dsp::FIR::Coefficients<float>(coefficients[ch].data(),64);reference[ch].prepare({48000,1,1});}
        for(int n=0;n<1537;++n){
            if(n==0||n==1||n==17||n==63||n==64||n==65||n==129||n==513||n==1024){choice.reset();for(auto& f:reference)f.reset();++resets;}
            std::array<float,2> input{};
            for(int ch=0;ch<2;++ch){const auto word=0x3f000000u|((std::uint32_t(n)*1664525u+std::uint32_t(ch)*1013904223u)&0x007fffffu);
                input[ch]=std::bit_cast<float>(word);if((n+ch)%3==0)input[ch]=-input[ch];if((n+ch)%11==0)input[ch]=std::bit_cast<float>(ch==0?0u:0x80000000u);}
            auto output=input;choice.process(output);
            for(int ch=0;ch<2;++ch){const float value=reference[ch].processSample(input[ch]);actual.push_back(output[ch]);expected.push_back(value);
                finite=finite&&std::isfinite(output[ch])&&std::isfinite(value);mismatches+=std::bit_cast<std::uint32_t>(output[ch])!=std::bit_cast<std::uint32_t>(value);}
        }
    }
    CHECK(finite&&actual.size()==9222&&expected.size()==actual.size()&&resets==27&&mismatches==0);
    const auto folder=juce::String::fromUTF8(std::getenv("AIRACK_CONVOLUTION_NATIVE_OUTPUT"));
    if(folder.isNotEmpty()){const juce::File directory(folder);const bool ready=directory.createDirectory().wasOk();CHECK(ready);
        for(int stream=0;stream<2;++stream){const auto file=directory.getChildFile(stream==0?"fixed-head-actual.f32":"fixed-head-juce-reference.f32");const bool fresh=!file.exists();CHECK(fresh);
            const auto& samples=stream==0?actual:expected;if(ready&&fresh)CHECK(file.replaceWithData(samples.data(),samples.size()*sizeof(float)));}}
    std::cout<<"CONVOLUTION_FIXED_HEAD samples="<<actual.size()<<" resets="<<resets<<" finite="<<finite<<" float_bit_mismatches="<<mismatches<<'\n';
#endif
}

void effect_convolution_unit_head_juce_bits()
{
#if !defined(_DEBUG)
    section("effect_convolution_unit_head_juce_bits");
    // JUCE64 proves finite/Inf bits and NaN class; compiled loops can select
    // different NaN payloads. Pin every original L output bit separately.
    juce::ScopedNoDenormals noDenormals;
    constexpr std::array<std::uint32_t,16> edges{0,0x80000000u,1,0x80000001u,0x007fffffu,0x807fffffu,0x00800000u,0x80800000u,
        0x3f800000u,0xbf800000u,0x7f7fffffu,0xff7fffffu,0x3f000000u,0xbf000000u,0x3eaaaaabu,0xbeaaaaabu};
    std::vector<std::uint32_t> actual,expected;actual.reserve(3*2049*2);expected.reserve(3*2049*2);
    std::size_t mismatches=0,payloadDifferences=0,finite=0,exceptional=0;int resets=0;
    for(double rate:{8000.,48000.,768000.}){
        juce::AudioBuffer<float> pcm(2,1);pcm.setSample(0,0,1);pcm.setSample(1,0,1);
        juce::dsp::ConvolutionMessageQueue queue{4};airack::ConvolutionChoice choice;
        CHECK(choice.prepare(pcm,rate,queue)&&choice.getCurrentIRSize()==1&&choice.getTailIRSize()==0&&choice.getLatency()==0);
        std::array<float,64> coefficients{};coefficients[0]=1;
        std::array<juce::dsp::FIR::Filter<float>,2> reference;
        for(auto& f:reference){f.coefficients=new juce::dsp::FIR::Coefficients<float>(coefficients.data(),64);f.prepare({rate,1,1});}
        for(int n=0;n<2049;++n){
            if(n==0||n==1||n==63||n==64||n==65||n==129||n==513||n==1024||n==1500){choice.reset();for(auto& f:reference)f.reset();++resets;}
            std::array<float,2> input{};
            for(int ch=0;ch<2;++ch){std::uint32_t word=0x3e000000u|((std::uint32_t(n)*1664525u+std::uint32_t(ch)*1013904223u)&0x007fffffu);
                if((n+ch)%3==0)word|=0x80000000u;if((n+ch)%7==0)word=edges[(n/7+ch)%edges.size()];
                if((ch==0&&(n==70||n==200||n==510||n==1010||n==1500))||(ch==1&&(n==71||n==201||n==511)))word=0x7fc12345u;
                if(ch==0&&n==210)word=0xff800000u;if(ch==1&&n==211)word=0x7f800000u;
                input[ch]=std::bit_cast<float>(word);}
            auto output=input;choice.process(output);
            for(int ch=0;ch<2;++ch){const float value=reference[ch].processSample(input[ch]);const auto a=std::bit_cast<std::uint32_t>(output[ch]),b=std::bit_cast<std::uint32_t>(value);
                const auto nan=[](std::uint32_t word){return (word&0x7f800000u)==0x7f800000u&&(word&0x007fffffu)!=0;};
                actual.push_back(a);expected.push_back(b);mismatches+=a!=b&&!(nan(a)&&nan(b));payloadDifferences+=a!=b&&nan(a)&&nan(b);
                if(std::isfinite(value))++finite;else ++exceptional;}
        }
    }
    std::uint64_t originalBits=14695981039346656037ull;for(auto word:actual)for(int byte=0;byte<4;++byte){originalBits^=(word>>(8*byte))&255u;originalBits*=1099511628211ull;}
    CHECK(actual.size()==12294&&expected.size()==actual.size()&&resets==27&&finite==11250&&exceptional==1044&&mismatches==0);
    CHECK(originalBits==13916387702478906974ull&&payloadDifferences==18);
    const auto folder=juce::String::fromUTF8(std::getenv("AIRACK_CONVOLUTION_NATIVE_OUTPUT"));
    if(folder.isNotEmpty()){const juce::File directory(folder);const bool ready=directory.createDirectory().wasOk();CHECK(ready);
        for(int stream=0;stream<2;++stream){const auto file=directory.getChildFile(stream==0?"unit-head-actual.u32":"unit-head-juce-reference.u32");const bool fresh=!file.exists();CHECK(fresh);
            const auto& samples=stream==0?actual:expected;if(ready&&fresh)CHECK(file.replaceWithData(samples.data(),samples.size()*sizeof(std::uint32_t)));}}
    std::cout<<"CONVOLUTION_UNIT_HEAD samples="<<actual.size()<<" resets="<<resets<<" finite_reference_samples="<<finite<<" exceptional_reference_samples="<<exceptional<<" juce_reference_mismatches="<<mismatches<<" juce_nan_payload_differences="<<payloadDifferences<<" original_l_bits_fnv="<<originalBits<<" context=ScopedNoDenormals"<<'\n';
#endif
}

void effect_convolution_predelay_setter_parity()
{
#if !defined(_DEBUG)
    section("effect_convolution_predelay_setter_parity");
    const auto bank=airack::canonicalConvolutionRecords();
    for(double rate:{8000.,48000.,96000.,192000.}){
        auto changed=std::make_unique<airack::PreparedConvolution>();
        auto forced=std::make_unique<airack::PreparedConvolution>();
        const bool prepared=changed->prepare(rate,bank)&&forced->prepare(rate,bank);CHECK(prepared);if(!prepared)return;
        const int ramp=int(std::ceil(.005*rate)),frames=int(std::ceil(.310*rate))+128;
        bool bits=true,delayBits=true;int firstMismatch=-1;
        for(int n=0;n<frames;++n){
            double seconds=0.;
            if(n>=32&&n<64)seconds=-0.;
            else if(n>=64&&n<96)seconds=n%2?123.25/rate:std::nextafter(123.25/rate,std::numeric_limits<double>::infinity());
            else if(n>=96&&n<96+ramp)seconds=.250*double(n-96)/ramp;
            else if(n>=96+ramp)seconds=.250;
            if(n==96+ramp+7){changed->clear();forced->clear();}
            const int choice=n<frames/2?0:1;
            const std::array<double,2> input=n%97==0?std::array<double,2>{0.,-0.}:n%101==0?std::array<double,2>{double(std::numeric_limits<float>::denorm_min()),-double(std::numeric_limits<float>::denorm_min())}:std::array<double,2>{.11*std::sin(.13*n),-.07*std::cos(.17*n)};
            // Exactly the predecessor's unconditional setter before the same prepared process path.
            forced->predelay.setDelay(float(seconds*rate));
            const auto a=changed->process(input,choice,seconds),b=forced->process(input,choice,seconds);
            for(int ch=0;ch<2;++ch){const bool same=std::bit_cast<std::uint32_t>(float(a[ch]))==std::bit_cast<std::uint32_t>(float(b[ch]));bits&=same;if(!same&&firstMismatch<0)firstMismatch=n;}
            delayBits&=std::bit_cast<std::uint32_t>(changed->predelay.getDelay())==std::bit_cast<std::uint32_t>(forced->predelay.getDelay());
        }
        CHECK(bits&&delayBits);CHECK(changed->engines[0]->getLatency()==0&&changed->engines[1]->getLatency()==0);
        CHECK(!changed->prepare(7999,bank)&&!forced->prepare(7999,bank));
        forced->predelay.setDelay(float(.250*rate));
        const auto a=changed->process({.13,-.09},1,.250),b=forced->process({.13,-.09},1,.250);
        CHECK(std::bit_cast<std::uint32_t>(float(a[0]))==std::bit_cast<std::uint32_t>(float(b[0]))&&std::bit_cast<std::uint32_t>(float(a[1]))==std::bit_cast<std::uint32_t>(float(b[1])));
        std::cout<<"CONVOLUTION_PREDELAY_PARITY rate="<<rate<<" frames="<<frames<<" bits="<<bits<<" delay_bits="<<delayBits<<" first_mismatch="<<firstMismatch<<" choices=2 clear=1 signed_zero=1 quantized_repeat=1 rejected_prepare=1 forced_original_setter=1"<<'\n';
    }
    // Valid cross-rate reprepare uses the public engine's replacement State ownership.
    auto patch=flat();add(patch,EffectKind::convolution);
    patch.effects[0].convolution={ConvolutionIr::shortRoom,17.25f,0,1};
    RoutedSynthEngine reused,fresh;
    const bool started=reused.setPatch(patch)&&fresh.setPatch(patch)&&reused.prepare(8000,512);
    CHECK(started);if(!started)return;
    render(reused,97,{{0,on(57)}},73);
    const bool reprepared=reused.prepare(96000,512)&&fresh.prepare(96000,512);
    CHECK(reprepared);if(!reprepared)return;
    CHECK(reused.patch()==patch&&fresh.patch()==patch&&reused.effectiveQuality()==RenderQuality::realtime&&reused.latencySamples()==0&&fresh.latencySamples()==0);
    const auto actual=render(reused,4096,{{0,on(57)}},73),expected=render(fresh,4096,{{0,on(57)}},73);
    const bool publicBits=stereoBits(actual,expected);
    CHECK(finite(actual)&&energy(actual)>0&&publicBits);
    std::cout<<"CONVOLUTION_PREDELAY_PUBLIC_REPREPARE from=8000 to=96000 frames=4096 bits="<<publicBits<<" quality=rt"<<'\n';
#endif
}

void effect_convolution_native_impulses_src_and_controls()
{
#if !defined(_DEBUG)
    section("effect_convolution_native_impulses_src_and_controls");
    effect_convolution_fixed_head_juce_bits();
    effect_convolution_unit_head_juce_bits();
    effect_convolution_predelay_setter_parity();
    const auto canonical=airack::canonicalConvolutionRecords();
    CHECK(airack::validConvolutionRecord(canonical[0])&&airack::validConvolutionRecord(canonical[1]));
    for(int variant=0;variant<7;++variant){auto bad=canonical[1];std::vector<std::uint32_t> corrupt(bad.words,bad.words+bad.frames*2);
        if(variant==0)bad.words=nullptr;if(variant==1)bad.frames=0;if(variant==2)bad.frames=12001;
        if(variant==3)bad.channels=1;if(variant==4)bad.sourceRate=44100;if(variant==5)bad.sha256="0000000000000000000000000000000000000000000000000000000000000000";
        if(variant==6){corrupt[77]^=1;bad.words=corrupt.data();}CHECK(!airack::validConvolutionRecord(bad));}
    for(double rate:{8000.,44100.,48000.,96000.,768000.}){
        // Independent source-policy oracle: decode canonical words, then use
        // installed SRC with the declared rate gain, no fitted lag or gain.
        juce::AudioBuffer<float> original(2,12000);for(int ch=0;ch<2;++ch)for(int n=0;n<12000;++n)
            original.setSample(ch,n,std::bit_cast<float>(canonical[1].words[ch*12000+n]));
        juce::AudioBuffer<float> reference;
        if(rate==48000)reference=original;else{juce::MemoryAudioSource memory(original,false);juce::ResamplingAudioSource src(&memory,false,2);
            const int frames=juce::roundToInt(12000*rate/48000);src.setResamplingRatio(48000/rate);src.prepareToPlay(frames,48000);
            reference.setSize(2,frames);src.getNextAudioBlock({&reference,0,frames});reference.applyGain(float(48000/rate));}
        // The private marker queue is fresh and outlives the substituted FIR/tail choice.
        juce::dsp::ConvolutionMessageQueue markerQueue{4};airack::PreparedConvolution stage;CHECK(stage.prepare(rate,canonical));
        CHECK(stage.engines[0]->getLatency()==0&&stage.engines[1]->getLatency()==0);
        CHECK(stage.engines[0]->getCurrentIRSize()==1&&stage.engines[1]->getCurrentIRSize()==reference.getNumSamples());
        CHECK(stage.engines[0]->getTailLatency()==0&&stage.engines[0]->getTailEngineLatency()==0&&stage.engines[0]->getTailIRSize()==0);
        CHECK(stage.engines[1]->getTailLatency()==64&&stage.engines[1]->getTailEngineLatency()==0&&stage.engines[1]->getTailIRSize()==reference.getNumSamples()-64);
        const int count=reference.getNumSamples()+133;double roomError=0,unitError=0,crossfeed=0,late=0;
        std::array<std::vector<float>,2> capture;for(auto& channel:capture)channel.resize(count);
        for(int n=0;n<count;++n){const auto out=stage.process({n==0?.25:0,n==0?-.125:0},0,0);
            unitError=std::max({unitError,std::abs(out[0]-(n==0?.25:0)),std::abs(out[1]-(n==0?-.125:0))});}
        stage.clear();for(int n=0;n<count;++n){const auto out=stage.process({n==0?1.:0,0},1,0);
            const double expected=n<reference.getNumSamples()?reference.getSample(0,n):0;roomError=std::max(roomError,std::abs(out[0]-expected));crossfeed=std::max(crossfeed,std::abs(out[1]));}
        stage.clear();for(int n=0;n<count;++n){const auto out=stage.process({0,n==0?1.:0},1,0);
            const double expected=n<reference.getNumSamples()?reference.getSample(1,n):0;roomError=std::max(roomError,std::abs(out[1]-expected));crossfeed=std::max(crossfeed,std::abs(out[0]));
            if(n>=reference.getNumSamples())late=std::max(late,std::abs(out[1]));capture[0][n]=float(out[0]);capture[1][n]=float(out[1]);}
        CHECK(unitError==0&&crossfeed==0&&roomError<=1e-6&&late<=1e-6);
        const auto folder=juce::String::fromUTF8(std::getenv("AIRACK_CONVOLUTION_NATIVE_OUTPUT"));if(folder.isNotEmpty()){
            const juce::File directory(folder);CHECK(directory.createDirectory().wasOk());const auto file=directory.getChildFile("impulse-"+juce::String(int(rate))+".f32");
            const bool fresh=!file.exists();CHECK(fresh);if(fresh){std::vector<float> planar=capture[0];planar.insert(planar.end(),capture[1].begin(),capture[1].end());CHECK(file.replaceWithData(planar.data(),planar.size()*sizeof(float)));}}
        std::cout.precision(17);std::cout<<"CONVOLUTION_NATIVE rate="<<rate<<" ir_frames="<<reference.getNumSamples()<<" latency=0 unit_error="<<unitError<<" room_error="<<roomError<<" crossfeed="<<crossfeed<<" late="<<late<<'\n';
        // Marker at the exact FIR/tail split, through the same one-frame adapter.
        // This private injected record is never a bank admission path.
        juce::AudioBuffer<float> marker(2,129);marker.clear();for(int ch=0;ch<2;++ch){marker.setSample(ch,63,.2f);marker.setSample(ch,64,-.3f);marker.setSample(ch,65,.4f);}
        stage.engines[1]=std::make_unique<airack::ConvolutionChoice>();CHECK(stage.engines[1]->prepare(std::move(marker),rate,markerQueue));
        CHECK(stage.engines[1]->getCurrentIRSize()==129&&stage.engines[1]->getLatency()==0);
        CHECK(stage.engines[1]->getTailLatency()==64&&stage.engines[1]->getTailEngineLatency()==0&&stage.engines[1]->getTailIRSize()==65);stage.clear();double markerError=0;const int markerIrBefore=stage.engines[1]->getCurrentIRSize();int markerMaxFrame=-1;std::array<std::array<float,260>,2> markerCapture{};
        for(int n=0;n<260;++n){const auto out=stage.process({n==0?1.:0,n==0?-.5:0},1,0);const double expected=n==63?double(.2f):n==64?double(-.3f):n==65?double(.4f):0;
            const double error=std::max(std::abs(out[0]-expected),std::abs(out[1]+.5*expected));if(error>markerError){markerError=error;markerMaxFrame=n;}for(int ch=0;ch<2;++ch)markerCapture[ch][n]=float(out[ch]);}
        std::cout<<"CONVOLUTION_MARKER rate="<<rate<<" ir_before="<<markerIrBefore<<" ir_after="<<stage.engines[1]->getCurrentIRSize()<<" error="<<markerError<<" max_frame="<<markerMaxFrame<<" left63="<<markerCapture[0][63]<<" left64="<<markerCapture[0][64]<<" left65="<<markerCapture[0][65]<<'\n';
        if(folder.isNotEmpty()){const juce::File file=juce::File(folder).getChildFile("marker-"+juce::String(int(rate))+".f32");const bool fresh=!file.exists();CHECK(fresh);if(fresh)CHECK(file.replaceWithData(markerCapture.data(),sizeof(markerCapture)));}
        CHECK(markerError<=1e-6);
        // Inputs on both sides of completed64-frame batches, after discarding
        // a partial nonzero block. Independent fixed impulse superposition.
        for(int n=0;n<33;++n)stage.process({.5,-.25},1,0);stage.clear();
        constexpr std::array<int,8> positions{0,62,63,64,65,126,127,128};
        constexpr std::array<float,8> amplitudes{1,.25f,-.5f,.75f,-.125f,.4f,-.6f,.3f};
        std::array<std::array<float,400>,2> boundaryCapture{};double boundaryError=0;
        for(int n=0;n<400;++n){float excitation=0;double expected=0;
            for(std::size_t i=0;i<positions.size();++i){if(n==positions[i])excitation=amplitudes[i];
                const int offset=n-positions[i];const float coefficient=offset==63?.2f:offset==64?-.3f:offset==65?.4f:0;
                expected+=double(amplitudes[i])*double(coefficient);}
            const auto out=stage.process({double(excitation),-.5*double(excitation)},1,0);
            boundaryError=std::max({boundaryError,std::abs(out[0]-expected),std::abs(out[1]+.5*expected)});
            for(int ch=0;ch<2;++ch)boundaryCapture[ch][n]=float(out[ch]);}
        CHECK(boundaryError<=1e-6);
        std::cout<<"CONVOLUTION_BATCH_BOUNDARY rate="<<rate<<" underlying_latency="<<stage.engines[1]->getTailEngineLatency()<<" external_latency="<<stage.engines[1]->getTailLatency()<<" complete_frames=400 reset_partial_frames=33 error="<<boundaryError<<'\n';
        if(folder.isNotEmpty()){const juce::File file=juce::File(folder).getChildFile("batch-boundary-"+juce::String(int(rate))+".f32");const bool fresh=!file.exists();CHECK(fresh);if(fresh)CHECK(file.replaceWithData(boundaryCapture.data(),sizeof(boundaryCapture)));}
        // Independent complete sparse-IR transfer at both the outer64 and
        // inner512 boundary. A fresh choice/queue precedes first preparation.
        constexpr std::array<int,8> wideTaps{0,63,64,65,575,576,577,1024};
        constexpr std::array<float,8> wideCoefficients{.5f,.125f,-.25f,.375f,-.0625f,.03125f,-.015625f,.0078125f};
        juce::AudioBuffer<float> wideIr(2,1025);wideIr.clear();
        for(std::size_t k=0;k<wideTaps.size();++k){wideIr.setSample(0,wideTaps[k],wideCoefficients[k]);wideIr.setSample(1,wideTaps[k],-.75f*wideCoefficients[k]);}
        juce::dsp::ConvolutionMessageQueue wideQueue{4};airack::ConvolutionChoice wideChoice;
        CHECK(wideChoice.prepare(std::move(wideIr),rate,wideQueue));
        CHECK(wideChoice.getCurrentIRSize()==1025&&wideChoice.getLatency()==0&&wideChoice.getTailIRSize()==961);
        CHECK(wideChoice.getTailLatency()==64&&wideChoice.getTailEngineLatency()==0);
        for(int n=0;n<517;++n){std::array<float,2> input{.2f,-.1f};wideChoice.process(input);}wideChoice.reset();
        constexpr std::array<int,8> widePositions{0,62,63,64,65,510,511,512};
        std::array<std::array<float,1664>,2> wideCapture{};double wideError=0,wideLate=0;bool wideFinite=true;
        for(int n=0;n<1664;++n){float excitation=0;double expected=0;
            for(std::size_t i=0;i<widePositions.size();++i){if(n==widePositions[i])excitation=amplitudes[i];
                for(std::size_t k=0;k<wideTaps.size();++k)if(n-widePositions[i]==wideTaps[k])expected+=double(amplitudes[i])*double(wideCoefficients[k]);}
            std::array<float,2> out{excitation,-.5f*excitation};wideChoice.process(out);
            wideError=std::max({wideError,std::abs(double(out[0])-expected),std::abs(double(out[1])-.375*expected)});
            for(int ch=0;ch<2;++ch){wideCapture[ch][n]=out[ch];wideFinite=wideFinite&&std::isfinite(out[ch]);if(n>1536)wideLate=std::max(wideLate,std::abs(double(out[ch])));}}
        if(folder.isNotEmpty()){const auto file=juce::File(folder).getChildFile("nonuniform-boundary-"+juce::String(int(rate))+".f32");const bool fresh=!file.exists();CHECK(fresh);if(fresh)CHECK(file.replaceWithData(wideCapture.data(),sizeof(wideCapture)));}
        CHECK(wideFinite&&wideError<=1e-6&&wideLate<=1e-6);
        std::cout<<"CONVOLUTION_NONUNIFORM_BOUNDARY rate="<<rate<<" ir_frames=1025 tail_frames=961 underlying_latency="<<wideChoice.getTailEngineLatency()<<" external_latency="<<wideChoice.getTailLatency()<<" complete_frames=1664 reset_after_frames=517 error="<<wideError<<" terminal_residual="<<wideLate<<'\n';
        auto effect=std::make_unique<airack::Effect>();CHECK(effect->prepare(rate));EffectSettings settings;settings.convolution={ConvolutionIr::unitImpulse,0,0,1};effect->settings(settings,1,true);effect->clear();
        settings.convolution={ConvolutionIr::unitImpulse,13.3f,-12,.7f};const int ramp=int(std::ceil(.005*rate));effect->settings(settings,ramp,false);
        std::array<double,3> current{0,1,1},target{13.3*.001,std::pow(10.,-12./20),.7f};std::vector<std::array<float,2>> history;double movingError=0;
        for(int n=0;n<ramp+99;++n){for(int k=0;k<3;++k)if(n<ramp)current[k]+=(target[k]-std::array<double,3>{0,1,1}[k])/ramp;if(n==ramp-1)current=target;
            const std::array<double,2> input{.13*std::sin(.19*n),-.07*std::cos(.23*n)};history.push_back({float(input[0]),float(input[1])});effect->advanceFilter();const auto out=effect->process(input,EffectKind::convolution,1,DriveShape::tanh,true,FilterFxMode::lowpass,settings.convolution.ir);
            const double position=n-current[0]*rate;const int base=int(std::floor(position));const auto sample=[&](int ch,int at){return at>=0&&std::size_t(at)<history.size()?double(history[std::size_t(at)][ch]):0.;};
            for(int ch=0;ch<2;++ch){const double delayed=float(sample(ch,base)+(position-base)*(sample(ch,base+1)-sample(ch,base)));const double expected=input[ch]*(1-current[2])+delayed*current[1]*current[2];movingError=std::max(movingError,std::abs(out[ch]-expected));}}
        CHECK(movingError<=1e-6);settings.convolution={ConvolutionIr::shortRoom,1,6,0};effect->settings(settings,ramp,false);effect->advanceFilter();effect->clear();
        CHECK(effect->convolutionValues[0]==double(settings.convolution.predelayMs)*.001&&effect->convolutionValues[1]==std::pow(10.,6./20)&&effect->convolutionValues[2]==0);
        for(int n=0;n<90;++n){effect->advanceFilter();const std::array<double,2> input{.1*std::sin(n),-.05*std::cos(n)};CHECK(effect->process(input,EffectKind::convolution,1,DriveShape::tanh,true,FilterFxMode::lowpass,settings.convolution.ir)==input);}
        CHECK(effect->convolution->engines[1]->getCurrentIRSize()==reference.getNumSamples());
    }
#endif
}
void effect_convolution_live_partition_stop_and_callbacks()
{
    section("effect_convolution_live_partition_stop_and_callbacks");auto base=flat();add(base,EffectKind::convolution);
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto run=[&](int block){auto patch=base;RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));Audio result;
            const auto append=[&](int n,const std::vector<Event>& events={}){const auto a=render(e,n,events,block);for(int ch=0;ch<2;++ch)result[ch].insert(result[ch].end(),a[ch].begin(),a[ch].end());};
            append(2001,{{0,on(57)},{77,on(81)}});for(int n=0;n<8;++n){patch.effects[0].convolution={n%2?ConvolutionIr::unitImpulse:ConvolutionIr::shortRoom,n%2?250.f:0.f,n%2?-24.f:12.f,n%3?1.f:0.f};CHECK(e.setPatch(patch));append(int(rate*.021));}
            auto bad=patch;bad.effects[7].convolution.ir=static_cast<ConvolutionIr>(255);CHECK(!e.setPatch(bad)&&e.patch()==patch);bad=patch;bad.effects[7].convolution.predelayMs=251;CHECK(!e.setPatch(bad));CHECK(!e.prepare(7999,512));
            patch.effects[0].bypassed=true;CHECK(e.setPatch(patch));append(511);patch.effects[0].bypassed=false;CHECK(e.setPatch(patch));append(1701,{{0,juce::MidiMessage::midiStop()},{113,on(60)}});e.reset();append(901,{{0,on(65)}});CHECK(finite(result)&&energy(result)>1e-9);return result;};
        const auto a=run(512),b=run(64),c=run(73);CHECK(difference(a,b)<=1e-6&&difference(a,c)<=1e-6);
        auto changed=base;changed.effects[0].convolution={ConvolutionIr::shortRoom,17,6,.8f};RoutedSynthEngine live,fresh;CHECK(live.setPatch(base)&&fresh.setPatch(changed)&&live.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));
        render(live,1701,{{0,on(69)}});CHECK(live.setPatch(changed));render(live,std::max(1,int(rate*.0017)));CHECK(stereoBits(render(live,4096,{{0,juce::MidiMessage::midiStop()},{113,on(57)}},73),render(fresh,4096,{{113,on(57)}},73),2*live.latencySamples()));
        const double internal=rate*(quality==RenderQuality::higher?2:1);CHECK(detail::convolutionTailSeconds(base.effects[0].convolution,internal)>=.25+.255);CHECK(live.latencySamples()==(quality==RenderQuality::higher?21:0));
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    auto full=base;for(int layer=1;layer<4;++layer)addLayer(full);for(int slot=1;slot<8;++slot)add(full,EffectKind::convolution);
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){RoutedSynthEngine e;CHECK(e.setPatch(full)&&e.prepare(rate,512,quality));juce::AudioBuffer<float> output(2,512);juce::MidiBuffer held,stop,empty;held.ensureSize(4096);stop.ensureSize(128);for(int note=48;note<64;++note)held.addEvent(on(note),0);stop.addEvent(juce::MidiMessage::midiStop(),17);
        auto previous=_CrtSetAllocHook(allocationHook);allocations=frees=0;tracking=true;bool ok=true;e.render(output,held,120);
        for(int n=0;n<32;++n){auto next=full;for(auto& s:next.effects)s.convolution={n%2?ConvolutionIr::unitImpulse:ConvolutionIr::shortRoom,n%2?250.f:0.f,n%2?-24.f:12.f,n%2?0.f:1.f};ok&=e.setPatch(next);e.render(output,n%7?empty:stop,120);if(n%11==0)e.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(ok&&allocations==0&&frees==0);std::cout<<"CONVOLUTION_CALLBACK rate="<<rate<<" quality="<<int(quality)<<" first_plus_live=33 layers=4 notes=16 stages=8 allocations="<<allocations<<" frees="<<frees<<'\n';}
#endif
}

void effect_frequency_split_native_transfer_and_tail()
{
    section("effect_frequency_split_native_transfer_and_tail");
#if !defined(_DEBUG)
    // Exercise the actual private Effect at its prepared INTERNAL rate. An
    // independent direct-form transfer oracle uses the static bilinear pole,
    // rather than copying the production trapezoidal state recurrence.
    juce::ScopedNoDenormals noDenormals;
    const juce::String exportPath=std::getenv("AIRACK_FREQUENCY_SPLIT_NATIVE_EXPORT_DIR")?std::getenv("AIRACK_FREQUENCY_SPLIT_NATIVE_EXPORT_DIR"):"";
    const juce::File exportFolder(exportPath);if(exportPath.isNotEmpty())CHECK(exportFolder.createDirectory().wasOk());
    for(double rate:{8000.,44100.,48000.,96000.,768000.}){
        Effect stage;CHECK(stage.prepare(rate));
        const int support=int(std::ceil(detail::frequencySplitTailSeconds(rate)*rate));
        CHECK(support>0&&support<=192160);
        const double toneG=std::tan(std::numbers::pi*1000/rate),toneA=toneG/(1+toneG),toneR=1-2*toneA;
        double rateError=0,rateTail=0;
        for(double cutoff:{20.,1000.,std::min(20000.,.45*rate)})for(int branch=0;branch<2;++branch)for(float tone:{-1.f,0.f,1.f}){
            EffectSettings settings;auto& s=settings.frequencySplit;s.crossoverHz=float(cutoff);s.lowMuted=branch==1;s.highMuted=branch==0;
            s.lowTone=s.highTone=tone;s.lowGainDb=s.highGainDb=12;s.mix=1;
            stage.settings(settings,std::max(1,int(std::ceil(.005*rate))),true);stage.clear();
            const double g=std::tan(std::numbers::pi*std::min(double(s.crossoverHz),.45*rate)/rate),a=g/(1+g),r=1-2*a,gain=std::pow(10.,.6);
            std::array<double,2> previousInput{},previousLow{},previousBranch{},previousTone{};
            std::array<double,3> real{},imag{};
            const std::array<double,3> frequencies{std::min(cutoff*.1,.04*rate),cutoff,std::min(cutoff*4,.45*rate)};
            double error=0,residual=0;
            std::vector<double> raw;if(exportPath.isNotEmpty())raw.resize(size_t(support+257)*2);
            for(int n=0;n<support+257;++n){const Stereo input{n==0?1.:0.,n==0?-.37:0.};
                stage.advanceFilter();const auto output=stage.process(input,EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);
                for(int ch=0;ch<2;++ch){const double low=a*input[ch]+a*previousInput[ch]+r*previousLow[ch];previousInput[ch]=input[ch];previousLow[ch]=low;
                    const double selected=branch==0?low:input[ch]-low;
                    const double colored=toneA*selected+toneA*previousBranch[ch]+toneR*previousTone[ch];previousBranch[ch]=selected;previousTone[ch]=colored;
                    const double expected=(selected+double(tone)*(selected-2*colored))*gain;
                    error=std::max(error,std::abs(output[ch]-expected));CHECK(std::isfinite(output[ch]));
                    if(!raw.empty())raw[size_t(n)*2+size_t(ch)]=output[ch];
                    if(n>=support)residual=std::max(residual,std::abs(output[ch]));
                }
                for(size_t f=0;f<3;++f){const double angle=2*std::numbers::pi*frequencies[f]*n/rate;real[f]+=output[0]*std::cos(angle);imag[f]-=output[0]*std::sin(angle);}
            }
            CHECK(error<1e-11&&residual<=1e-6);rateError=std::max(rateError,error);rateTail=std::max(rateTail,residual);
            if(!raw.empty()){const auto file=exportFolder.getChildFile("split-"+juce::String(int(rate))+"-cutoff-"+juce::String(int(cutoff))+"-branch-"+juce::String(branch)+"-tone-"+juce::String(int(tone))+".f64");const bool fresh=!file.exists();CHECK(fresh);if(fresh)CHECK(file.replaceWithData(raw.data(),raw.size()*sizeof(double)));}
            for(size_t f=0;f<3;++f){const auto z=std::polar(1.,-2*std::numbers::pi*frequencies[f]/rate);
                const auto low=a*(1.+z)/(1.-r*z),selected=branch==0?low:1.-low;
                const auto color=1.+double(tone)*(1.-2.*toneA*(1.+z)/(1.-toneR*z));
                const auto wanted=selected*color*gain,measured=std::complex<double>(real[f],imag[f]);CHECK(std::abs(wanted-measured)<1e-6);
                if(tone==0&&f==1)CHECK(std::abs(std::abs(wanted)/gain-std::sqrt(.5))<1e-12);
            }
        }
        // The complete admitted cutoff grid includes negative poles. A
        // transparent current state clocks all histories and preserves zeros.
        for(int point=0;point<=128;++point){EffectSettings s;s.frequencySplit.crossoverHz=float(20*std::pow(1000.,double(point)/128));
            stage.settings(s,int(std::ceil(.005*rate)),true);stage.clear();
            for(int n=0;n<97;++n){const Stereo input{n%2?-.23:.71,n%3?0.:-0.};stage.advanceFilter();const auto output=stage.process(input,EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);
                CHECK(std::bit_cast<std::uint64_t>(output[0])==std::bit_cast<std::uint64_t>(input[0])&&std::bit_cast<std::uint64_t>(output[1])==std::bit_cast<std::uint64_t>(input[1]));}
        }
        // A mute-gate edit has an observable, independent first/mid/endpoint
        // law when the two complementary neutral branches share the same gate.
        EffectSettings live;stage.settings(live,int(std::ceil(.005*rate)),true);stage.clear();
        const int ramp=int(std::ceil(.005*rate));live.frequencySplit.lowMuted=live.frequencySplit.highMuted=true;stage.settings(live,ramp,false);
        for(int n=0;n<=ramp+1;++n){stage.advanceFilter();const auto output=stage.process({.25,-.5},EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);
            const double gate=1-std::min(n,ramp)/double(ramp);CHECK(std::abs(output[0]-.25*gate)<1e-12&&std::abs(output[1]+.5*gate)<1e-12);
            if(n==ramp/2)stage.settings(live,ramp,false);}
        live.frequencySplit.lowMuted=live.frequencySplit.highMuted=false;stage.settings(live,ramp,false);double gate=0;
        for(int n=0;n<ramp/3;++n){stage.advanceFilter();const auto out=stage.process({.25,-.5},EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);CHECK(std::abs(out[0]-.25*gate)<1e-12);gate+=1./ramp;}
        live.frequencySplit.lowMuted=live.frequencySplit.highMuted=true;stage.settings(live,ramp,false);const double decrement=gate/ramp;
        for(int n=0;n<=ramp;++n){stage.advanceFilter();const auto out=stage.process({.25,-.5},EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);CHECK(std::abs(out[0]-.25*std::max(0.,gate))<1e-12);gate-=decrement;}
        // Mix0 clocks the actual two tone histories. A later nonneutral target
        // must agree with the independently running wet stage without a clear.
        Effect wet;CHECK(wet.prepare(rate));EffectSettings muted;muted.frequencySplit={1973,-6,false,.7f,3,false,-.4f,0};auto audible=muted;audible.frequencySplit.mix=1;
        stage.settings(muted,ramp,true);stage.clear();wet.settings(audible,ramp,true);wet.clear();
        for(int n=0;n<2301;++n){const Stereo in{std::sin(.137*n),std::cos(.073*n)};stage.advanceFilter();wet.advanceFilter();const auto dry=stage.process(in,EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);wet.process(in,EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);CHECK(dry==in);}
        stage.settings(audible,ramp,true);stage.advanceFilter();wet.advanceFilter();CHECK(stage.process({.3,-.7},EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass)==wet.process({.3,-.7},EffectKind::frequencySplit,1,DriveShape::tanh,true,FilterFxMode::lowpass));
        std::cout<<"FREQUENCY_SPLIT_TRANSFER internal_rate="<<rate<<" support_frames="<<support<<" independent_DF1_error="<<rateError<<" complete_cascade_residual="<<rateTail<<" negative_pole_admitted=1\n";
    }
#endif
    auto valid=flat();add(valid,EffectKind::frequencySplit);
    for(int field=0;field<6;++field)for(float bad:{-100000.f,100000.f,std::numeric_limits<float>::quiet_NaN()}){auto invalid=valid;auto& s=invalid.effects[7].frequencySplit;
        if(field==0)s.crossoverHz=bad;else if(field==1)s.lowGainDb=bad;else if(field==2)s.lowTone=bad;else if(field==3)s.highGainDb=bad;else if(field==4)s.highTone=bad;else s.mix=bad;CHECK(!validateRoutedPatch(invalid));}
}
void effect_frequency_split_live_clock_and_callbacks()
{
    section("effect_frequency_split_live_clock_and_callbacks");auto p=flat();add(p,EffectKind::frequencySplit);
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto run=[&](int partition){auto patch=p;RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(e,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(1001,{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].frequencySplit={n%2?20.f:20000.f,n%2?-24.f:12.f,n%3==0,n%2?-1.f:1.f,n%2?12.f:-24.f,n%3==1,n%2?1.f:-1.f,n%2?0.f:1.f};CHECK(e.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(e.setPatch(patch));append(37);}
            const auto held=e.patch();auto bad=held;bad.effects[7].frequencySplit.lowTone=1.01f;CHECK(!e.setPatch(bad)&&e.patch()==held);CHECK(!e.prepare(7999,512));juce::AudioBuffer<float> empty(2,0);juce::MidiBuffer midi;e.render(empty,midi);
            patch.effects[0].bypassed=true;CHECK(e.setPatch(patch));append(int(rate*.02));patch.effects[0].frequencySplit={1317,-3,false,-.7f,6,true,.3f,.8f};CHECK(e.setPatch(patch));append(91);patch.effects[0].bypassed=false;CHECK(e.setPatch(patch));append(int(rate*.04));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});e.reset();append(1501,{{0,on(65)},{801,off(65)}});CHECK(finite(output)&&energy(output)>1e-9);return output;};
        const auto a=run(512),b=run(64),c=run(73);CHECK(stereoBits(a,b)&&stereoBits(a,c));
        auto changed=p;changed.effects[0].frequencySplit={197,-7,true,.37f,3,false,-.49f,.71f};RoutedSynthEngine live,fresh;
        CHECK(live.setPatch(p)&&fresh.setPatch(changed)&&live.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));render(live,2001,{{0,on(69)}});CHECK(live.setPatch(changed));render(live,std::max(1,int(rate*.0017)));
        CHECK(stereoBits(render(live,4096,{{0,juce::MidiMessage::midiStop()},{113,on(57)}},73),render(fresh,4096,{{113,on(57)}},73),2*live.latencySamples()));live.reset();fresh.reset();CHECK(stereoBits(render(live,4096,{{0,on(65)}}),render(fresh,4096,{{0,on(65)}})));
        auto neutral=p;const auto capture=[&](const RoutedPatch& patch){RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));return render(e,4096,{{0,on(59)},{31,on(71)}},73);};CHECK(stereoBits(capture(flat()),capture(neutral)));
        std::cout<<"FREQUENCY_SPLIT_CLOCK rate="<<rate<<" quality="<<int(quality)<<" partition_bits=1 Stop_snap8=1 neutral_bits=1 FIR_support="<<2*live.latencySamples()<<'\n';
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::frequencySplit);
    for(size_t i=0;i<4;++i){full.synth.layers[i].engine=i%2?EngineKind::wavetable:EngineKind::analog;for(auto& route:full.synth.layers[i].modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=i%2?ModBank::wavetable:ModBank::analog;route.amount=.005f;}}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;for(int s=0;s<8;++s)full.effects[s].frequencySplit={300.f+131.f*s,-3,false,-.2f,2,false,.3f,.8f};
        RoutedSynthEngine e;CHECK(e.setPatch(full)&&e.prepare(rate,block,quality));juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;e.render(output,notes,120);
        for(int n=0;n<32;++n){auto next=full;for(int s=0;s<8;++s)next.effects[s].frequencySplit={n%2?20.f:20000.f,n%2?-24.f:12.f,n%2==0,n%2?-1.f:1.f,n%2?12.f:-24.f,n%2!=0,n%2?1.f:-1.f,n%2?0.f:1.f};accepted&=e.setPatch(next);e.render(output,n%19==0?stop:empty,120);if(n%23==0)e.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);std::cout<<"FREQUENCY_SPLIT_CALLBACK rate="<<rate<<" quality="<<int(quality)<<" layers=4 notes=16 fx=8 routes=64 edits=64 callbacks=33 alloc="<<allocations<<" frees="<<frees<<'\n';
#endif
    }
}

void effect_frequency_split_kernel_availability()
{
    section("effect_frequency_split_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(13),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;CHECK(validateRoutedPatch(p));
}
// External source-first slice. UNCOMPILED / UNEXECUTED.
// Merge inside the existing RoutedSynthEngineTests.cpp anonymous namespace.
// The existing Release-only inclusion of RoutedSynthEngine.cpp supplies Effect
// and Stereo. Add this function to ordinary main after actual M/S availability
// succeeds. Debug's host/lifecycle/allocation tests remain separate.
void effect_mid_side_split_native_transfer_and_tail()
{
    section("effect_mid_side_split_native_transfer_and_tail");
#if !defined(_DEBUG)
    // The nearest double to mathematical 1/sqrt(2), pinned for both matrices.
    constexpr double inverseSqrt2 = 0x1.6a09e667f3bcdp-1;
    constexpr double sampleTolerance = 2e-11;
    constexpr double transferTolerance = 2e-9;
    constexpr double residualTolerance = 1e-6;
    constexpr int maximumSupport = 5870;
    constexpr int suffixFrames = 257;
    constexpr int warmFrames = 513, continuationFrames = 257;
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    static_assert(sizeof(Stereo) == 2 * sizeof(double));
    static_assert(std::endian::native == std::endian::little);
    juce::ScopedNoDenormals noDenormals;

    // Case order and input order are fixed export coordinates. Settings retain
    // their real physical float values; no oracle fits gain, phase or delay.
    const std::array<MidSideSplitSettings, 10> cases {{
        { 12, false, -1, -24, true,   1, 1 }, // 0: loud dark Mid only
        {-24, false,  1,  12, true,  -1, 1 }, // 1: quiet bright Mid only
        {-24, true,   1,  12, false, -1, 1 }, // 2: loud dark Side only
        { 12, true,  -1, -24, false,  1, 1 }, // 3: quiet bright Side only
        { -6, false, .7f,  3, false, -.4f, 1 }, // 4: independent varied branches
        { 12, false, -1, -24, false,  1, .37f }, // 5: actual partial dry/wet
        {  0, false,  0,   0, true,   0, 1 }, // 6: unity Mid / fresh mono
        { 12, true,   1, -24, true,  -1, 1 }, // 7: both branches muted
        {  0, false,  0,   0, false,  0, 1 }, // 8: transparent nominal
        { 12, false, .75f, -24, false, -.4f, 0 } // 9: dry while histories clock
    }};
    const std::array<Stereo, 5> impulses {{
        {1, 0}, {0, 1}, {.75, .75}, {.75, -.75}, {1, -.37}
    }}; // left column, right column, mono, opposite, asymmetric

    const char* rawExport = std::getenv("AIRACK_MID_SIDE_SPLIT_NATIVE_EXPORT_DIR");
    const juce::String exportPath = rawExport ? rawExport : "";
    const juce::File exportFolder(exportPath);
    if (exportPath.isNotEmpty()) {
        const bool absolute = juce::File::isAbsolutePath(exportPath);
        CHECK(absolute);
        if (!absolute) return; // Setup failure stays a failure, never acceptance.
        const bool ready = exportFolder.createDirectory().wasOk();
        CHECK(ready);
        if (!ready) return;
    }
    const auto exportTrace = [&](const juce::String& name, const std::vector<Stereo>& trace) {
        if (exportPath.isEmpty()) return;
        const auto file = exportFolder.getChildFile(name);
        const bool fresh = !file.exists();
        CHECK(fresh);
        if (fresh) CHECK(file.replaceWithData(trace.data(), trace.size() * sizeof(Stereo)));
    };

    for (double rate : {8000., 44100., 48000., 96000., 768000.}) {
        Effect stage; // Exactly one actual Effect prepare per admitted rate.
        const bool prepared = stage.prepare(rate);
        CHECK(prepared);
        if (!prepared) continue;
        const double publishedSeconds = detail::midSideSplitTailSeconds(rate);
        const double publishedFrames = std::ceil(publishedSeconds * rate);
        const bool boundedSupport = std::isfinite(publishedSeconds)
            && std::isfinite(publishedFrames) && publishedFrames > 0
            && publishedFrames <= maximumSupport;
        CHECK(boundedSupport);
        if (!boundedSupport) continue; // Prevent an unbounded allocation/window.
        const int support = int(publishedFrames);
        const int count = support + suffixFrames;
        const int ramp = int(std::ceil(.005 * rate));

        const double g = std::tan(std::numbers::pi * 1000 / rate);
        const double a = g / (1 + g), pole = (1 - g) / (1 + g);
        CHECK(a > 0 && a <= .292894 && pole > 0 && pole < 1);
        // Independent geometric support and integer minimality. One additional
        // frame permits ceil((integer/rate)*rate) representation rounding only.
        int residualFrame = 1 + int(std::ceil(std::log(residualTolerance / 16) / std::log(pole)));
        const auto envelope = [&](int n) { return 16 * std::pow(pole, n - 1); };
        while (envelope(residualFrame) > residualTolerance) ++residualFrame;
        while (residualFrame > 1 && envelope(residualFrame - 1) <= residualTolerance) --residualFrame;
        CHECK(envelope(residualFrame) <= residualTolerance);
        CHECK(residualFrame == 1 || envelope(residualFrame - 1) > residualTolerance);
        const int independentSupport = residualFrame + ramp + 1;
        CHECK(support >= independentSupport && support <= independentSupport + 1);

        // Local static oracle data: gains, gates, tones, mix. This is test-only
        // arithmetic, with two DF1 input/output pairs and no production state
        // member access. y=a*x+a*x_previous+pole*y_previous is independent of
        // the production TPT pre-state representation.
        const auto coefficients = [](const MidSideSplitSettings& s) {
            return std::array<double, 7>{std::pow(10., double(s.midGainDb) / 20),
                s.midMuted ? 0. : 1., double(s.midTone),
                std::pow(10., double(s.sideGainDb) / 20),
                s.sideMuted ? 0. : 1., double(s.sideTone), double(s.mix)};
        };
        const auto oracleStep = [&](Stereo input, const std::array<double, 7>& c,
                                    Stereo& previousInput, Stereo& previousLow) {
            const Stereo branch {(input[0] + input[1]) * inverseSqrt2,
                                 (input[0] - input[1]) * inverseSqrt2};
            Stereo colored {};
            for (int b = 0; b < 2; ++b) {
                const double low = a * branch[b] + a * previousInput[b] + pole * previousLow[b];
                previousInput[b] = branch[b]; previousLow[b] = low;
                colored[b] = (branch[b] + c[3 * b + 2] * (branch[b] - 2 * low))
                    * c[3 * b] * c[3 * b + 1];
            }
            const Stereo wet {(colored[0] + colored[1]) * inverseSqrt2,
                              (colored[0] - colored[1]) * inverseSqrt2};
            return Stereo {(1 - c[6]) * input[0] + c[6] * wet[0],
                           (1 - c[6]) * input[1] + c[6] * wet[1]};
        };
        double maximumError = 0, maximumMatrixError = 0, maximumTail = 0;
        double maximumSuperpositionError = 0, maximumWarmError = 0;
        int completeImpulseTraces = 0;
        bool allDirectBits = true;

        for (int caseIndex = 0; caseIndex < int(cases.size()); ++caseIndex) {
            EffectSettings settings; settings.midSideSplit = cases[size_t(caseIndex)];
            const auto c = coefficients(settings.midSideSplit);
            std::array<std::vector<Stereo>, 2> columns;
            for (int inputIndex = 0; inputIndex < int(impulses.size()); ++inputIndex) {
                stage.settings(settings, ramp, true); stage.clear();
                Stereo previousInput {}, previousLow {};
                std::vector<Stereo> trace(size_t(count), Stereo{});
                const Stereo impulse = impulses[size_t(inputIndex)];
                const double inputPeak = std::max(std::abs(impulse[0]), std::abs(impulse[1]));
                double error = 0, expectedPeak = 0, measuredPeak = 0, tail = 0;
                double superpositionError = 0;
                bool finite = true;
                for (int n = 0; n < count; ++n) {
                    const Stereo input = n == 0 ? impulse : Stereo{};
                    stage.advanceFilter();
                    const auto output = stage.process(input, EffectKind::midSideSplit, 1,
                        DriveShape::tanh, true, FilterFxMode::lowpass);
                    const auto expected = oracleStep(input, c, previousInput, previousLow);
                    trace[size_t(n)] = output;
                    for (int ch = 0; ch < 2; ++ch) {
                        finite = finite && std::isfinite(output[ch]) && std::isfinite(expected[ch]);
                        error = std::max(error, std::abs(output[ch] - expected[ch]));
                        expectedPeak = std::max(expectedPeak, std::abs(expected[ch]));
                        measuredPeak = std::max(measuredPeak, std::abs(output[ch]));
                        if (n >= support) tail = std::max(tail, std::abs(output[ch]));
                        if (n >= 1) CHECK(std::abs(output[ch]) <= inputPeak * envelope(n)
                            + sampleTolerance * std::max(1., inputPeak));
                        if (inputIndex >= 2) {
                            const double linear = impulse[0] * columns[0][size_t(n)][ch]
                                                + impulse[1] * columns[1][size_t(n)][ch];
                            superpositionError = std::max(superpositionError, std::abs(output[ch] - linear));
                        }
                        if (caseIndex == 6 && inputIndex == 2) CHECK(std::abs(output[0] - output[1]) <= sampleTolerance);
                        if ((caseIndex == 2 || caseIndex == 3) && inputIndex == 2) CHECK(output[ch] == 0);
                        if ((caseIndex == 0 || caseIndex == 1 || caseIndex == 6) && inputIndex == 3) CHECK(output[ch] == 0);
                    }
                }
                CHECK(finite);
                CHECK(error <= sampleTolerance * std::max(1., expectedPeak));
                CHECK(superpositionError <= sampleTolerance * std::max(1., expectedPeak));
                CHECK(measuredPeak <= 32 * inputPeak + sampleTolerance);
                CHECK(tail <= residualTolerance * inputPeak);
                maximumError = std::max(maximumError, error);
                maximumTail = std::max(maximumTail, tail);
                maximumSuperpositionError = std::max(maximumSuperpositionError, superpositionError);
                if (inputIndex < 2) columns[size_t(inputIndex)] = trace;
                exportTrace("mid-side-split-native-" + juce::String(int(rate)) + "-case-"
                    + juce::String(caseIndex) + "-input-" + juce::String(inputIndex) + ".f64", trace);
                ++completeImpulseTraces;
            }

            // Both full impulse columns, both output rows, DC/pivot/Nyquist.
            // The expected orthonormal matrix is explicitly .5*(Gm +/- Gs).
            const std::array<double, 3> frequencies {0., 1000., rate * .5};
            for (int f = 0; f < int(frequencies.size()); ++f) {
                const double omega = 2 * std::numbers::pi * frequencies[size_t(f)] / rate;
                const std::complex<double> z = f == 0 ? std::complex<double>{1, 0}
                    : f == 2 ? std::complex<double>{-1, 0} : std::polar(1., -omega);
                const auto low = a * (1. + z) / (1. - pole * z);
                std::array<std::complex<double>, 2> branchTransfer;
                for (int b = 0; b < 2; ++b)
                    branchTransfer[size_t(b)] = c[3 * b] * c[3 * b + 1]
                        * (1. + c[3 * b + 2] * (1. - 2. * low));
                std::array<std::array<std::complex<double>, 2>, 2> measured {};
                for (int column = 0; column < 2; ++column) for (int n = 0; n < count; ++n) {
                    const auto phase = f == 0 ? std::complex<double>{1, 0}
                        : f == 2 ? std::complex<double>{n % 2 ? -1. : 1., 0}
                        : std::polar(1., -omega * n);
                    for (int row = 0; row < 2; ++row)
                        measured[size_t(column)][size_t(row)] += columns[size_t(column)][size_t(n)][row] * phase;
                }
                for (int column = 0; column < 2; ++column) for (int row = 0; row < 2; ++row) {
                    const auto wet = .5 * (branchTransfer[0]
                        + (column == row ? branchTransfer[1] : -branchTransfer[1]));
                    const auto wanted = c[6] * wet + (column == row ? 1 - c[6] : 0);
                    const double matrixError = std::abs(measured[size_t(column)][size_t(row)] - wanted);
                    CHECK(matrixError <= transferTolerance * std::max(1., std::abs(wanted)));
                    maximumMatrixError = std::max(maximumMatrixError, matrixError);
                }
                if (c[6] > 0) {
                    // Invert the known static dry/wet law, not a fitted scale.
                    const std::array<std::complex<double>, 2> actualBranches {
                        (measured[0][0] + measured[0][1] - (1 - c[6])) / c[6],
                        (measured[0][0] - measured[0][1] - (1 - c[6])) / c[6]};
                    for (int b = 0; b < 2; ++b) {
                        const double gain = c[3 * b] * c[3 * b + 1], tone = c[3 * b + 2];
                        const double magnitude = gain * (f == 0 ? 1 - tone
                            : f == 2 ? 1 + tone : std::sqrt(1 + tone * tone));
                        CHECK(std::abs(std::abs(actualBranches[size_t(b)]) - magnitude)
                            <= transferTolerance * std::max(1., magnitude));
                        if (f != 1) CHECK(std::abs(actualBranches[size_t(b)].imag()) <= transferTolerance);
                    }
                }
            }
        }

        // Warm default/Mix0 output must retain original signed-zero bits, yet
        // its hidden tone histories must continue. The later nonneutral output
        // is compared to independent DF1 histories without any intervening clear.
        for (int dryMode = 0; dryMode < 2; ++dryMode) {
            EffectSettings warm, audible;
            audible.midSideSplit = {-6, false, .7f, 3, false, -.4f, 1};
            if (dryMode == 1) { warm = audible; warm.midSideSplit.mix = 0; }
            stage.settings(warm, ramp, true); stage.clear();
            const auto warmCoefficients = coefficients(warm.midSideSplit);
            const auto audibleCoefficients = coefficients(audible.midSideSplit);
            Stereo previousInput {}, previousLow {};
            std::vector<Stereo> trace(size_t(warmFrames + continuationFrames), Stereo{});
            double error = 0, peak = 0;
            bool finite = true;
            for (int n = 0; n < warmFrames; ++n) {
                const int zero = n % 23;
                const Stereo input = zero < 4
                    ? Stereo {zero & 1 ? -0. : 0., zero & 2 ? -0. : 0.}
                    : Stereo {.13 + .05 * std::sin(.137 * n), -.07 + .03 * std::cos(.073 * n)};
                // Check pinned neutral algebra and orthonormal energy directly.
                const double mid = (input[0] + input[1]) * inverseSqrt2;
                const double side = (input[0] - input[1]) * inverseSqrt2;
                CHECK(std::abs(mid * mid + side * side - input[0] * input[0] - input[1] * input[1]) <= 2e-15);
                oracleStep(input, warmCoefficients, previousInput, previousLow);
                stage.advanceFilter();
                const auto output = stage.process(input, EffectKind::midSideSplit, 1,
                    DriveShape::tanh, true, FilterFxMode::lowpass);
                trace[size_t(n)] = output;
                for (int ch = 0; ch < 2; ++ch) {
                    const bool originalBits = std::bit_cast<std::uint64_t>(output[ch])
                        == std::bit_cast<std::uint64_t>(input[ch]);
                    CHECK(originalBits);
                    allDirectBits = allDirectBits && originalBits;
                    finite = finite && std::isfinite(output[ch]);
                }
            }
            stage.settings(audible, ramp, true); // Intentionally NO clear.
            for (int n = 0; n < continuationFrames; ++n) {
                const Stereo input {n == 0 ? .3 : .11 * std::sin(.19 * n),
                                    n == 0 ? -.7 : -.07 * std::cos(.23 * n)};
                const auto expected = oracleStep(input, audibleCoefficients, previousInput, previousLow);
                stage.advanceFilter();
                const auto output = stage.process(input, EffectKind::midSideSplit, 1,
                    DriveShape::tanh, true, FilterFxMode::lowpass);
                trace[size_t(warmFrames + n)] = output;
                for (int ch = 0; ch < 2; ++ch) {
                    finite = finite && std::isfinite(output[ch]) && std::isfinite(expected[ch]);
                    error = std::max(error, std::abs(output[ch] - expected[ch]));
                    peak = std::max(peak, std::abs(expected[ch]));
                }
            }
            CHECK(finite && error <= sampleTolerance * std::max(1., peak));
            maximumWarmError = std::max(maximumWarmError, error);
            exportTrace("mid-side-split-warm-" + juce::String(int(rate))
                + (dryMode == 0 ? "-neutral.f64" : "-mix0.f64"), trace);
        }
        CHECK(completeImpulseTraces == 50);
        const auto precision = std::cout.precision(); std::cout.precision(17);
        std::cout << "MID_SIDE_SPLIT_TRANSFER internal_rate=" << rate
            << " support_frames=" << support << " independent_support_frames=" << independentSupport
            << " complete_impulse_traces=" << completeImpulseTraces << " channels=2"
            << " frames_per_impulse_trace=" << count << " complete_warm_traces=2 warm_trace_frames=770"
            << " independent_DF1_error=" << maximumError << " full_complex_matrix_error=" << maximumMatrixError
            << " superposition_error=" << maximumSuperpositionError << " post_support_residual=" << maximumTail
            << " warm_history_continuation_error=" << maximumWarmError
            << " neutral_mix0_original_signed_zero_bits=" << int(allDirectBits) << '\n';
        std::cout.precision(precision);
    }
#endif
}

void effect_mid_side_split_independent_ramp_and_admission()
{
    section("effect_mid_side_split_independent_ramp_and_admission");
    auto p=flat();add(p,EffectKind::midSideSplit);
    for(int field=0;field<5;++field)for(float value:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),field<2?13.f:field<4?1.01f:1.01f}){
        auto bad=p;auto& s=bad.effects[7].midSideSplit;
        if(field==0)s.midGainDb=value;else if(field==1)s.sideGainDb=value;else if(field==2)s.midTone=value;else if(field==3)s.sideTone=value;else s.mix=value;
        CHECK(!validateRoutedPatch(bad));RoutedSynthEngine engine;CHECK(engine.setPatch(p));CHECK(!engine.setPatch(bad)&&engine.patch()==p);
    }
    for(float value:{-24.01f,12.01f}){auto bad=p;bad.effects[7].midSideSplit.midGainDb=value;CHECK(!validateRoutedPatch(bad));bad=p;bad.effects[7].midSideSplit.sideGainDb=value;CHECK(!validateRoutedPatch(bad));}
    for(float value:{-1.01f,1.01f}){auto bad=p;bad.effects[7].midSideSplit.midTone=value;CHECK(!validateRoutedPatch(bad));bad=p;bad.effects[7].midSideSplit.sideTone=value;CHECK(!validateRoutedPatch(bad));}
    auto bad=p;bad.effects[7].midSideSplit.mix=-.01f;CHECK(!validateRoutedPatch(bad));
    for(double rate:{7999.,768001.,std::numeric_limits<double>::quiet_NaN()})CHECK(std::isnan(detail::midSideSplitTailSeconds(rate)));
#if !defined(_DEBUG)
    Effect stage;CHECK(stage.prepare(48000));juce::ScopedNoDenormals noDenormals;
    constexpr double q=0x1.6a09e667f3bcdp-1;const double g=std::tan(std::numbers::pi*1000/48000),a=g/(1+g),pole=(1-g)/(1+g);
    std::array<double,7> current{1,1,0,1,1,0,1},target=current,increment{};int remaining=0;
    Stereo previousInput{},previousLow{};EffectSettings settings;stage.settings(settings,240,true);stage.clear();double error=0;
    const auto change=[&](const MidSideSplitSettings& s){settings.midSideSplit=s;target={std::pow(10.,double(s.midGainDb)/20),s.midMuted?0.:1.,s.midTone,std::pow(10.,double(s.sideGainDb)/20),s.sideMuted?0.:1.,s.sideTone,s.mix};for(size_t k=0;k<7;++k)increment[k]=(target[k]-current[k])/240;remaining=240;stage.settings(settings,240,false);};
    for(int n=0;n<800;++n){if(n==0)change({12,true,-1,-24,false,1,.37f});if(n==83)change({-6,false,.7f,3,true,-.4f,.81f});if(n==417)change({0,false,0,0,false,0,1});
        const Stereo input{.11*std::sin(.19*n)+.03,-.07*std::cos(.23*n)};const Stereo branch{(input[0]+input[1])*q,(input[0]-input[1])*q};Stereo colored{};
        for(int b=0;b<2;++b){const double low=a*branch[b]+a*previousInput[b]+pole*previousLow[b];previousInput[b]=branch[b];previousLow[b]=low;colored[b]=(branch[b]+current[3*b+2]*(branch[b]-2*low))*current[3*b]*current[3*b+1];}
        const Stereo wet{(colored[0]+colored[1])*q,(colored[0]-colored[1])*q};const Stereo expected{input[0]+current[6]*(wet[0]-input[0]),input[1]+current[6]*(wet[1]-input[1])};
        stage.advanceFilter();const auto out=stage.process(input,EffectKind::midSideSplit,1,DriveShape::tanh,true,FilterFxMode::lowpass);
        for(int b=0;b<2;++b)error=std::max(error,std::abs(out[b]-expected[b]));
        if(remaining){for(size_t k=0;k<7;++k)current[k]+=increment[k];if(--remaining==0)current=target;}
    }
    CHECK(error<2e-11);std::cout<<"MID_SIDE_SPLIT_RAMP independent_DF1_error="<<error<<" ramps=7 interrupted=1 current_before_increment=1\n";
#endif
}

void effect_mid_side_split_live_clock_and_callbacks()
{
    section("effect_mid_side_split_live_clock_and_callbacks");auto p=flat();p.synth.layers[0].pan=-.35f;add(p,EffectKind::midSideSplit);
    for(double rate:{8000.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const auto run=[&](int partition){auto patch=p;RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));Audio output;
            const auto append=[&](int count,const std::vector<Event>& events={}){const auto a=render(e,count,events,partition);for(int ch=0;ch<2;++ch)output[ch].insert(output[ch].end(),a[ch].begin(),a[ch].end());};
            append(1001,{{0,on(57)},{31,on(81)}});
            for(int n=0;n<12;++n){patch.effects[0].midSideSplit={n%2?-24.f:12.f,n%3==0,n%2?-1.f:1.f,n%2?12.f:-24.f,n%3==1,n%2?1.f:-1.f,n%2?0.f:1.f};CHECK(e.setPatch(patch));append(int(rate*(n%3==0?.0017:.0073)));CHECK(e.setPatch(patch));append(37);}
            const auto held=e.patch();auto bad=held;bad.effects[7].midSideSplit.midTone=1.01f;CHECK(!e.setPatch(bad)&&e.patch()==held);CHECK(!e.prepare(7999,512));juce::AudioBuffer<float> empty(2,0);juce::MidiBuffer midi;e.render(empty,midi);
            patch.effects[0].bypassed=true;CHECK(e.setPatch(patch));append(int(rate*.02));patch.effects[0].midSideSplit={-3,false,-.7f,6,true,.3f,.8f};CHECK(e.setPatch(patch));append(91);patch.effects[0].bypassed=false;CHECK(e.setPatch(patch));append(int(rate*.04));
            append(1901,{{913,juce::MidiMessage::midiStop()},{1027,on(69)}});e.reset();append(1501,{{0,on(65)},{801,off(65)}});CHECK(finite(output)&&energy(output)>1e-9);return output;};
        const auto a=run(512),b=run(64),c=run(73);CHECK(stereoBits(a,b)&&stereoBits(a,c));
        auto changed=p;changed.effects[0].midSideSplit={-7,true,.37f,3,false,-.49f,.71f};RoutedSynthEngine live,fresh;
        CHECK(live.setPatch(p)&&fresh.setPatch(changed)&&live.prepare(rate,512,quality)&&fresh.prepare(rate,512,quality));render(live,2001,{{0,on(69)}});CHECK(live.setPatch(changed));render(live,std::max(1,int(rate*.0017)));
        CHECK(stereoBits(render(live,4096,{{0,juce::MidiMessage::midiStop()},{113,on(57)}},73),render(fresh,4096,{{113,on(57)}},73),2*live.latencySamples()));live.reset();fresh.reset();CHECK(stereoBits(render(live,4096,{{0,on(65)}}),render(fresh,4096,{{0,on(65)}})));
        auto neutral=p;const auto capture=[&](const RoutedPatch& patch){RoutedSynthEngine e;CHECK(e.setPatch(patch)&&e.prepare(rate,512,quality));return render(e,4096,{{0,on(59)},{31,on(71)}},73);};CHECK(stereoBits(capture([]{auto dry=flat();dry.synth.layers[0].pan=-.35f;return dry;}()),capture(neutral)));
        std::cout<<"MID_SIDE_SPLIT_CLOCK rate="<<rate<<" quality="<<int(quality)<<" partition_bits=1 Stop_snap7=1 neutral_bits=1 FIR_support="<<2*live.latencySamples()<<'\n';
    }
    auto full=p;for(int n=0;n<3;++n)addLayer(full);for(int n=1;n<8;++n)add(full,EffectKind::midSideSplit);
    for(size_t i=0;i<4;++i){full.synth.layers[i].pan=i%2?.35f:-.35f;full.synth.layers[i].engine=i%2?EngineKind::wavetable:EngineKind::analog;for(auto& route:full.synth.layers[i].modulation){route.enabled=true;route.source=ModSource::lfo1;route.target=ModTarget::cutoff;route.bank=i%2?ModBank::wavetable:ModBank::analog;route.amount=.005f;}}
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        const int block=rate==48000?128:512;for(int s=0;s<8;++s)full.effects[s].midSideSplit={-3,false,-.2f,2,false,.3f,.8f};
        RoutedSynthEngine e;CHECK(e.setPatch(full)&&e.prepare(rate,block,quality));juce::AudioBuffer<float> output(2,block);juce::MidiBuffer notes,empty,stop;notes.ensureSize(4096);stop.ensureSize(128);stop.addEvent(juce::MidiMessage::midiStop(),17);for(int n=48;n<64;++n)notes.addEvent(on(n),0);
#if defined(_MSC_VER) && defined(_DEBUG)
        auto previous=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;bool accepted=true;e.render(output,notes,120);
        for(int n=0;n<32;++n){auto next=full;for(int s=0;s<8;++s)next.effects[s].midSideSplit={n%2?-24.f:12.f,n%2==0,n%2?-1.f:1.f,n%2?12.f:-24.f,n%2!=0,n%2?1.f:-1.f,n%2?0.f:1.f};accepted&=e.setPatch(next);e.render(output,n%19==0?stop:empty,120);if(n%23==0)e.reset();}
        tracking=false;_CrtSetAllocHook(previous);CHECK(accepted&&allocations==0&&frees==0);std::cout<<"MID_SIDE_SPLIT_CALLBACK rate="<<rate<<" quality="<<int(quality)<<" layers=4 notes=16 fx=8 routes=64 edits=56 callbacks=33 alloc="<<allocations<<" frees="<<frees<<'\n';
#endif
    }
}

void effect_mid_side_split_kernel_availability()
{
    section("effect_mid_side_split_kernel_availability");auto p=flat();
    p.layout.effects[0]={SlotUse::present,static_cast<EffectKind>(14),masterChain};
    p.layout.effectCount[masterChain]=1;p.layout.effectOrder[masterChain][0]=0;CHECK(validateRoutedPatch(p));
}
#if !defined(_DEBUG)
// The unchanged per-frame clocks are the oracle; timing is development-only.
#if defined(_MSC_VER)
__declspec(noinline)
#endif
void eqRetainedClockReference(airack::Effect& effect,int frames)
{
    for(int n=0;n<frames;++n)effect.advanceFilter();
    for(int n=0;n<frames;++n)for(auto& control:effect.controls)(void)control.next();
}
#if defined(_MSC_VER)
__declspec(noinline)
#endif
void eqRetainedClockBatch(airack::Effect& effect,int frames)
{
    if(frames<=0)return;
    effect.advanceFilterBlock(frames);
    for(auto& control:effect.controls)(void)control.advance(frames);
}
void eqRetainedClockChecks(bool benchmark)
{
    section("eq_retained_clock_block_exact_private_state");
    juce::ScopedNoDenormals noDenormals;
    const auto seed=[](airack::Effect& effect,int variant,bool settled){
        std::size_t index=0;
        const auto ramp=[&](auto& control){
            constexpr std::array<double,6> values{-.75,.125,0.,-0.,1.e-200,-.375};
            constexpr std::array<int,8> lengths{0,1,2,15,16,17,240,960};
            control.value=values[index%values.size()];control.target=values[(index+3)%values.size()];
            control.remaining=settled?0:lengths[(index+std::size_t(variant))%lengths.size()];
            if(control.remaining==0)control.value=control.target;
            control.increment=control.remaining?(control.target-control.value)/control.remaining:.003;
            ++index;
        };
        for(auto& control:effect.controls)ramp(control);
        const auto bank=[&](auto& controls,auto& values){
            for(std::size_t i=0;i<controls.size();++i){ramp(controls[i]);values[i]=-.0625*double(i+1);}
        };
        bank(effect.filterControls,effect.filterValues);bank(effect.chorusControls,effect.chorusValues);
        bank(effect.phaserControls,effect.phaserValues);bank(effect.flangerControls,effect.flangerValues);
        bank(effect.stereoControls,effect.stereoValues);bank(effect.dimensionControls,effect.dimensionValues);
        bank(effect.frequencyShiftControls,effect.frequencyShiftValues);bank(effect.convolutionControls,effect.convolutionValues);
        bank(effect.frequencySplitControls,effect.frequencySplitValues);bank(effect.midSideSplitControls,effect.midSideSplitValues);
    };
    const auto equal=[](const airack::Effect& a,const airack::Effect& b){
        const auto same=[](double x,double y){return std::bit_cast<std::uint64_t>(x)==std::bit_cast<std::uint64_t>(y);};
        const auto ramps=[&](const auto& x,const auto& y){
            for(std::size_t i=0;i<x.size();++i){CHECK(same(x[i].value,y[i].value));CHECK(same(x[i].target,y[i].target));
                CHECK(same(x[i].increment,y[i].increment));CHECK(x[i].remaining==y[i].remaining);}
        };
        ramps(a.controls,b.controls);
        const auto bank=[&](const auto& x,const auto& xv,const auto& y,const auto& yv){
            ramps(x,y);for(std::size_t i=0;i<xv.size();++i)CHECK(same(xv[i],yv[i]));
        };
        bank(a.filterControls,a.filterValues,b.filterControls,b.filterValues);
        bank(a.chorusControls,a.chorusValues,b.chorusControls,b.chorusValues);
        bank(a.phaserControls,a.phaserValues,b.phaserControls,b.phaserValues);
        bank(a.flangerControls,a.flangerValues,b.flangerControls,b.flangerValues);
        bank(a.stereoControls,a.stereoValues,b.stereoControls,b.stereoValues);
        bank(a.dimensionControls,a.dimensionValues,b.dimensionControls,b.dimensionValues);
        bank(a.frequencyShiftControls,a.frequencyShiftValues,b.frequencyShiftControls,b.frequencyShiftValues);
        bank(a.convolutionControls,a.convolutionValues,b.convolutionControls,b.convolutionValues);
        bank(a.frequencySplitControls,a.frequencySplitValues,b.frequencySplitControls,b.frequencySplitValues);
        bank(a.midSideSplitControls,a.midSideSplitValues,b.midSideSplitControls,b.midSideSplitValues);
    };
    auto reference=std::make_unique<airack::Effect>(),candidate=std::make_unique<airack::Effect>();
    for(int variant=0;variant<8;++variant)for(int count:{0,1,2,15,16,17,63,64,65,127,128,239,240,241,959,960,961}){
        seed(*reference,variant,false);seed(*candidate,variant,false);
        eqRetainedClockReference(*reference,count);eqRetainedClockBatch(*candidate,count);equal(*reference,*candidate);
        // A following packet also covers the pre-advance value after a ramp settles.
        eqRetainedClockReference(*reference,16);eqRetainedClockBatch(*candidate,16);equal(*reference,*candidate);
    }
    if(failures||!benchmark)return;
    std::array<std::unique_ptr<airack::Effect>,8> referenceBank,candidateBank;
    for(std::size_t i=0;i<8;++i){referenceBank[i]=std::make_unique<airack::Effect>();candidateBank[i]=std::make_unique<airack::Effect>();}
    for(int frames:{16,128})for(int pair=0;pair<5;++pair){
        for(std::size_t i=0;i<8;++i){seed(*referenceBank[i],int(i),true);seed(*candidateBank[i],int(i),true);}
        const int iterations=frames==16?4000:500;
        const auto measure=[&](auto& bank,auto clock){
            const auto before=std::chrono::steady_clock::now();
            for(int n=0;n<iterations;++n)for(auto& effect:bank)clock(*effect,frames);
            return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-before).count()/iterations*(128./frames);
        };
        double oldNs,newNs;
        if(pair%2){newNs=measure(candidateBank,eqRetainedClockBatch);oldNs=measure(referenceBank,eqRetainedClockReference);}
        else{oldNs=measure(referenceBank,eqRetainedClockReference);newNs=measure(candidateBank,eqRetainedClockBatch);}
        for(std::size_t i=0;i<8;++i)equal(*referenceBank[i],*candidateBank[i]);
        std::cout<<"RETAINED_CLOCK_HYPOTHESIS frames="<<frames<<" pair="<<pair<<" original_ns_per_128_frames="<<oldNs
                 <<" prototype_ns_per_128_frames="<<newNs<<" saved_ns="<<oldNs-newNs<<" diagnostic_only=1\n";
    }
}
#endif

int main()
{
    if(!std::getenv("AIRACK_ROUTED_TEST_GROUP")&&std::getenv("AIRACK_EQ_RETAINED_CLOCK_PROBE")){
#if !defined(_DEBUG)
        eqRetainedClockChecks(true);std::cout<<"Retained clock checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
#else
        std::cerr<<"Retained clock hypothesis requires Release private-state oracle\n";return 2;
#endif
    }
    if(const auto* group=std::getenv("AIRACK_ROUTED_TEST_GROUP")){
        const std::string_view name(group);
        for(const char* flag : {
            "AIRACK_EQ_RETAINED_CLOCK_PROBE",
            "AIRACK_BASE_MATH_AUDIO_DIR",
            "AIRACK_CHORUS_PHYSICAL_ONLY",
            "AIRACK_CONVOLUTION_NATIVE_ONLY",
            "AIRACK_CONVOLUTION_NATIVE_OUTPUT",
            "AIRACK_CONVOLUTION_RESOURCE_ONLY",
            "AIRACK_DIMENSION_ONLY",
            "AIRACK_FLANGER_ONLY",
            "AIRACK_FLANGER_STOP_AUDIO_DIR",
            "AIRACK_FLANGER_STOP_ONLY",
            "AIRACK_FREQUENCY_SHIFT_NATIVE_ONLY",
            "AIRACK_FREQUENCY_SPLIT_NATIVE_EXPORT_DIR",
            "AIRACK_FREQUENCY_SPLIT_NATIVE_ONLY",
            "AIRACK_MID_SIDE_SPLIT_NATIVE_EXPORT_DIR",
            "AIRACK_MID_SIDE_SPLIT_NATIVE_ONLY",
            "AIRACK_PHASER_ONLY",
            "AIRACK_SHARED_MODULATION_GOLDEN_CAPTURE",
            "AIRACK_SHARED_MODULATION_ONLY",
            "AIRACK_STEREO_ONLY"
        }) if(std::getenv(flag)){
            std::cout<<"Routed group selector conflict="<<flag<<'\n'<<std::flush;return 2;
        }
        if(name=="prepare_storage"){
            modernDelayStorage();
        }
        else if(name=="native_prefix"){
            effect_mid_side_split_kernel_availability();if(failures){std::cout<<"Mid side split availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_mid_side_split_native_transfer_and_tail();
            effect_mid_side_split_independent_ramp_and_admission();
            effect_mid_side_split_live_clock_and_callbacks();
            effect_frequency_split_kernel_availability();if(failures){std::cout<<"Frequency split availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_frequency_split_native_transfer_and_tail();
            effect_frequency_split_live_clock_and_callbacks();
            effect_convolution_kernel_availability();if(failures){std::cout<<"Convolution availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_convolution_resource_and_atomic_preparation();
            effect_convolution_native_impulses_src_and_controls();
            effect_convolution_live_partition_stop_and_callbacks();
            effect_frequency_shift_native_tones_and_edges();
            effect_frequency_shift_kernel_availability();if(failures){std::cout<<"Frequency shift availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_frequency_shift_preparation_and_physical_oracle();
            effect_frequency_shift_live_tail_stop_and_callbacks();
        }
        else if(name=="native_prefix_head"){
            effect_mid_side_split_kernel_availability();if(failures){std::cout<<"Mid side split availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_mid_side_split_native_transfer_and_tail();
            effect_mid_side_split_independent_ramp_and_admission();
            effect_mid_side_split_live_clock_and_callbacks();
            effect_frequency_split_kernel_availability();if(failures){std::cout<<"Frequency split availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_frequency_split_native_transfer_and_tail();
            effect_frequency_split_live_clock_and_callbacks();
            effect_convolution_kernel_availability();if(failures){std::cout<<"Convolution availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_convolution_resource_and_atomic_preparation();
            effect_convolution_native_impulses_src_and_controls();
            effect_convolution_live_partition_stop_and_callbacks();
            effect_frequency_shift_native_tones_and_edges();
        }
        else if(name=="native_prefix_tail"){
            effect_frequency_shift_kernel_availability();if(failures){std::cout<<"Frequency shift availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_frequency_shift_preparation_and_physical_oracle();
            effect_frequency_shift_live_tail_stop_and_callbacks();
        }
        else if(name=="shared_dimension"){
            sharedModulationPrechangeGoldens();
            effect_dimension_kernel_availability();if(failures){std::cout<<"Dimension availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_dimension_response_and_fold();
            effect_dimension_interrupted_control_and_clock();
            effect_dimension_first_live_allocation();
        }
        else if(name=="stereo"){
            effect_stereo_kernel_availability();if(failures){std::cout<<"Stereo availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_stereo_algebra_and_neutral();
            effect_stereo_independent_ramp_and_history();
            effect_stereo_clock_stop_and_callbacks();
        }
        else if(name=="flanger"){
            effect_flanger_kernel_availability();if(failures){std::cout<<"Flanger availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_flanger_transfer_and_interpolation();
            effect_flanger_independent_moving_oracle();
            effect_flanger_live_clock_and_callbacks();
            effect_flanger_mid_ramp_stop_and_tail();
        }
        else if(name=="phaser"){
            effect_phaser_kernel_availability();if(failures){std::cout<<"Phaser availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_phaser_transfer_and_motion();
            effect_phaser_live_clock_and_callbacks();
            effect_phaser_tail_residual();
        }
        else if(name=="chorus_eq_filter"){
            effect_chorus_kernel_availability();if(failures){std::cout<<"Routed availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
            effect_chorus_delay_feedback_and_stereo();
            effect_chorus_live_clock_and_callbacks();
            effect_filter_kernel_availability();
            effect_filter_transfer_and_live_clock();
            effect_eq_kernel_availability();
            effect_eq_transfer_and_live_clock();
        }
        else if(name=="drive_coefficient_parity"){driveControlsAndAlias();}
        else if(name=="legacy_core"){
#if !defined(_DEBUG)
            eqRetainedClockChecks(false);
#endif
            outputLegacySamples();
            outputValidationAndGain();
            outputProtectionAndRelease();
            outputEditsResetAndInvalidBuffers();
            validationIsTransactional();
            parallelInsertsAndMasterOrder();
            driveControlsAndAlias();
            delayTempoStereoAndTail();
            reverbIndependentControlsAndTail();
            compressorLinkedDynamics();
            muteSoloAndMasterTail();
            editsRecallAndPartitions();
            controlRampsAndBoundaryCases();
            structuralTailAndFadeEvidence();
            preparedOperationsAllocateNothing();
            routedModulationClockAndCapacity();
            qualityEffectStereoTail();
            qualityCallbackSafety();
            preparedQualityContract<RoutedSynthEngine>();
            higherOutputProtection();
            higherStopProtectionHistory();
            baseFrameMathLiveArguments();
        }
        else if(name=="modern_reverb"){
            modernReverbAdmissionAndTransition();
            modernReverbClockAndStatePreservation();
            modernReverbPreparedCallbacks();
            modernReverbFingerprint();
        }
        else if(name=="modern_drive"){
            modernDriveAdmission();
            modernDriveResponse();
            modernDriveSaturation();
            modernDrivePartitionResetAndCallbacks();
            legacyDriveFingerprint();
        }
        else if(name=="modern_compressor"){
            modernCompressorAdmission();
            modernCompressorBallistics();
            modernCompressorStaticCurveAndDistortion();
            modernCompressorLinkMakeupMixBypass();
            modernCompressorPartitionResetAndCallbacks();
            legacyCompressorFingerprint();
        }
        else if(name=="modern_modulated_taps"){
            modernModulatedTapsAdmission();
            modernModulatedTapsFixedTap();
            modernModulatedTapsFlutterAndProducts();
            modernModulatedTapsKeepLaw();
            modernModulatedTapsEngine();
            legacyModulatedTapsFingerprint();
        }
        else if(name=="modern_delay"){
            modernDelayAdmission();
            modernDelayFractionalRepeat();
            modernDelayGlide();
            modernDelayKeepLaw();
            modernDelayEngine();
            modernDelayStorage();
            legacyDelayFingerprint();
            modernDelayNeutralFingerprint();
            modernDelayTapeFingerprint();
            delayTapeSaturation();
            delayTapeLowCut();
            delayTapeWobble();
            delayTapeGlide();
            delayTapeTail();
            delayStereoRightTime();
            delayStereoPingPong();
            delayStereoNote();
        }
        else if(name=="delay_stereo"){
            modernDelayNeutralFingerprint();
            modernDelayTapeFingerprint();
            delayStereoRightTime();
            delayStereoPingPong();
            delayStereoNote();
        }
        else if(name=="delay_tape"){
            delayTapeSaturation();
            delayTapeLowCut();
            delayTapeWobble();
            delayTapeGlide();
            delayTapeTail();
        }
        else if(name=="modern_eq"){
            modernEqAdmission();
            modernEqGrid();
            modernEqResponseThroughEngine();
            modernEqKeepLaw();
            modernEqEngine();
            legacyEqFingerprint();
        }
        else if(name=="modern_filter"){
            modernFilterAdmission();
            modernFilterResponse();
            modernFilterSaturation();
            modernFilterPartitionResetAndCallbacks();
            legacyFilterFingerprint();
        }
        else if(name=="modern_frequency_split"){
            modernFrequencySplitAdmission();
            modernFrequencySplitResponse();
            modernFrequencySplitPartitionResetAndCallbacks();
            legacyFrequencySplitFingerprint();
        }
        else{std::cout<<"Routed group selector invalid="<<name<<'\n'<<std::flush;return 2;}
        std::cout<<"Routed group="<<name<<" checks="<<checks<<" failures="<<failures<<'\n'<<std::flush;
        return failures?1:0;
    }
    // CTest runs the groups, not this default run; tools/routed_groups.py holds them to it, each section once.
    if(!std::getenv("AIRACK_MID_SIDE_SPLIT_NATIVE_ONLY")&&!std::getenv("AIRACK_FREQUENCY_SPLIT_NATIVE_ONLY")){
        effect_convolution_kernel_availability();if(failures){std::cout<<"Convolution availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
        effect_convolution_resource_and_atomic_preparation();
    }
    effect_mid_side_split_kernel_availability();if(failures){std::cout<<"Mid side split availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_mid_side_split_native_transfer_and_tail();effect_mid_side_split_independent_ramp_and_admission();effect_mid_side_split_live_clock_and_callbacks();if(std::getenv("AIRACK_MID_SIDE_SPLIT_NATIVE_ONLY")){std::cout<<"Mid side split native checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_frequency_split_kernel_availability();if(failures){std::cout<<"Frequency split availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_frequency_split_native_transfer_and_tail();effect_frequency_split_live_clock_and_callbacks();if(std::getenv("AIRACK_FREQUENCY_SPLIT_NATIVE_ONLY")){std::cout<<"Frequency split native checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(std::getenv("AIRACK_CONVOLUTION_RESOURCE_ONLY")){std::cout<<"Convolution resource checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_convolution_native_impulses_src_and_controls();if(std::getenv("AIRACK_CONVOLUTION_NATIVE_ONLY")){std::cout<<"Convolution native checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}effect_convolution_live_partition_stop_and_callbacks();
    effect_frequency_shift_native_tones_and_edges();if(std::getenv("AIRACK_FREQUENCY_SHIFT_NATIVE_ONLY")){std::cout<<"Frequency shift native checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;} effect_frequency_shift_kernel_availability();if(failures){std::cout<<"Frequency shift availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_frequency_shift_preparation_and_physical_oracle();effect_frequency_shift_live_tail_stop_and_callbacks();
    sharedModulationPrechangeGoldens();if(std::getenv("AIRACK_SHARED_MODULATION_ONLY")){std::cout<<"Shared modulation checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_dimension_kernel_availability();if(failures){std::cout<<"Dimension availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_dimension_response_and_fold();effect_dimension_interrupted_control_and_clock();effect_dimension_first_live_allocation();
    if(std::getenv("AIRACK_DIMENSION_ONLY")){std::cout<<"Dimension checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_stereo_kernel_availability();if(failures){std::cout<<"Stereo availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_stereo_algebra_and_neutral();effect_stereo_independent_ramp_and_history();effect_stereo_clock_stop_and_callbacks();
    if(std::getenv("AIRACK_STEREO_ONLY")){std::cout<<"Stereo checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_flanger_kernel_availability();if(failures){std::cout<<"Flanger availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    if(std::getenv("AIRACK_FLANGER_STOP_ONLY")){effect_flanger_mid_ramp_stop_and_tail();std::cout<<"Flanger Stop diagnosis checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_flanger_transfer_and_interpolation();effect_flanger_independent_moving_oracle();effect_flanger_live_clock_and_callbacks();effect_flanger_mid_ramp_stop_and_tail();
    if(std::getenv("AIRACK_FLANGER_ONLY")){std::cout<<"Flanger checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_phaser_kernel_availability();if(failures){std::cout<<"Phaser availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_phaser_transfer_and_motion();effect_phaser_live_clock_and_callbacks();effect_phaser_tail_residual();
    if(std::getenv("AIRACK_PHASER_ONLY")){std::cout<<"Phaser checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(std::getenv("AIRACK_CHORUS_PHYSICAL_ONLY")){chorus_actual_phase_and_centre_ramp();std::cout<<"Chorus physical checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    effect_chorus_kernel_availability();
    // A missing family is a complete scoped availability RED. The full owning
    // suite runs once it is admitted; don't rerender unchanged predecessors here.
    if(failures){std::cout<<"Routed availability checks="<<checks<<" failures="<<failures<<'\n';return 1;}
    effect_chorus_delay_feedback_and_stereo();effect_chorus_live_clock_and_callbacks();
    effect_filter_kernel_availability();effect_filter_transfer_and_live_clock();effect_eq_kernel_availability();effect_eq_transfer_and_live_clock();
#if !defined(_DEBUG)
    eqRetainedClockChecks(false);
#endif
    outputLegacySamples();
    outputValidationAndGain();outputProtectionAndRelease();outputEditsResetAndInvalidBuffers();
    validationIsTransactional(); parallelInsertsAndMasterOrder(); driveControlsAndAlias(); delayTempoStereoAndTail();
    reverbIndependentControlsAndTail(); compressorLinkedDynamics(); muteSoloAndMasterTail(); editsRecallAndPartitions();
    controlRampsAndBoundaryCases(); structuralTailAndFadeEvidence(); preparedOperationsAllocateNothing();
    routedModulationClockAndCapacity();
    qualityEffectStereoTail();
    qualityCallbackSafety(); preparedQualityContract<RoutedSynthEngine>(); higherOutputProtection();higherStopProtectionHistory();
    baseFrameMathLiveArguments();
    std::cout<<"Routed checks="<<checks<<" failures="<<failures<<'\n'; return failures?1:0;
}
