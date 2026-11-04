#include "SoundState.h"
#include "SoundDocument.h"
#include "RackDesign.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <type_traits>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace airack;
int checks = 0, failures = 0;
void check(bool ok, const char* expression, int line)
{
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL line " << line << ": " << expression << '\n'; }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
bool near(float a, float b) { return std::abs(a - b) <= 2e-5f * std::max(1.0f, std::abs(b)); }
void section(const char* name) { std::cout << "SECTION " << name << std::endl; }
constexpr std::size_t layoutOffset=16+8*parameterCount, macroOffset=layoutOffset+78, controlLockOffset=macroOffset+64, synthLockOffset=controlLockOffset+parameterCount;
struct Field { const char* id; float low, high, initial, skew; int choices; const char* unit; };
constexpr Field analog[] {
    {"osc.1.waveform",0,3,2,1,4,""}, {"osc.1.tune",-24,24,0,1,0,"st"},
    {"osc.1.detune",-100,100,0,1,0,"cent"}, {"osc.1.level",0,1,.5f,1,0,""}, {"osc.1.pulse_width",.05f,.95f,.5f,1,0,""},
    {"osc.2.waveform",0,3,2,1,4,""}, {"osc.2.tune",-24,24,0,1,0,"st"},
    {"osc.2.detune",-100,100,0,1,0,"cent"}, {"osc.2.level",0,1,.5f,1,0,""}, {"osc.2.pulse_width",.05f,.95f,.5f,1,0,""}
};
constexpr Field wavetable[] {
    {"osc.1.table",0,1,0,1,2,""}, {"osc.1.position",0,1,0,1,0,""}, {"osc.1.tune",-24,24,0,1,0,"st"},
    {"osc.1.detune",-100,100,0,1,0,"cent"}, {"osc.1.level",0,1,.5f,1,0,""},
    {"osc.2.table",0,1,0,1,2,""}, {"osc.2.position",0,1,0,1,0,""}, {"osc.2.tune",-24,24,0,1,0,"st"},
    {"osc.2.detune",-100,100,0,1,0,"cent"}, {"osc.2.level",0,1,.5f,1,0,""}
};
constexpr Field tail[] {
    {"noise",0,1,0,1,0,""}, {"filter.cutoff",20,20000,4000,.25f,0,"Hz"},
    {"filter.resonance",0,.95f,.25f,1,0,""}, {"filter.mode",0,2,0,1,3,""},
    {"amp.attack",0,20,.005f,.3f,0,"s"}, {"amp.decay",0,20,.15f,.3f,0,"s"},
    {"amp.sustain",0,1,.65f,1,0,""}, {"amp.release",0,20,.15f,.3f,0,"s"},
    {"filter_env.attack",0,20,.005f,.3f,0,"s"}, {"filter_env.decay",0,20,.15f,.3f,0,"s"},
    {"filter_env.sustain",0,1,.65f,1,0,""}, {"filter_env.release",0,20,.15f,.3f,0,"s"},
    {"filter_env.octaves",-8,8,0,1,0,"oct"}
};
std::uint32_t fnv(const juce::String& id)
{
    auto h = std::uint32_t{2166136261u};
    for (const auto c : id.toStdString()) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h;
}
void write32(juce::MemoryBlock& b, std::size_t offset, std::uint32_t value)
{
    auto* bytes = static_cast<unsigned char*>(b.getData());
    for (int i = 0; i < 4; ++i) bytes[offset + static_cast<std::size_t>(i)] = static_cast<unsigned char>(value >> (8 * i));
}
std::uint32_t read32(const juce::MemoryBlock& b, std::size_t offset)
{
    const auto* p = static_cast<const unsigned char*>(b.getData()) + offset;
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
void sign(juce::MemoryBlock& b)
{
    const auto hash = juce::SHA256(b.getData(), b.getSize()-32).getRawData();
    std::memcpy(static_cast<char*>(b.getData()) + b.getSize()-32, hash.getData(), 32);
}
void expectEnvelope(const EnvelopeSettings& a, const EnvelopeSettings& b)
{ CHECK(near(a.attack,b.attack)); CHECK(near(a.decay,b.decay)); CHECK(near(a.sustain,b.sustain)); CHECK(near(a.release,b.release)); }
void expectPatch(const SynthPatch& a, const SynthPatch& b)
{
    CHECK(near(a.masterLevel,b.masterLevel));
    for (std::size_t i=0; i<4; ++i)
    {
        const auto& x=a.layers[i]; const auto& y=b.layers[i];
        CHECK(x.enabled==y.enabled); CHECK(x.engine==y.engine); CHECK(near(x.level,y.level)); CHECK(near(x.pan,y.pan));
        CHECK(near(x.noiseLevel,y.noiseLevel)); CHECK(near(x.cutoffHz,y.cutoffHz)); CHECK(near(x.resonance,y.resonance));
        CHECK(x.filterMode==y.filterMode); CHECK(near(x.filterEnvelopeOctaves,y.filterEnvelopeOctaves));
        expectEnvelope(x.ampEnvelope,y.ampEnvelope); expectEnvelope(x.filterEnvelope,y.filterEnvelope);
        for (std::size_t j=0; j<2; ++j)
        {
            const auto& m=x.oscillators[j]; const auto& n=y.oscillators[j];
            CHECK(m.waveform==n.waveform); CHECK(m.tableIndex==n.tableIndex); CHECK(near(m.position,n.position));
            CHECK(near(m.tune,n.tune)); CHECK(near(m.detune,n.detune)); CHECK(near(m.level,n.level)); CHECK(near(m.pulseWidth,n.pulseWidth));
        }
    }
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking = false;
thread_local int allocations = 0;
int __cdecl allocationHook(int kind, void*, size_t, int, long, const unsigned char*, int)
{ if (tracking && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)) ++allocations; return 1; }
#endif
void mapping_allocates_zero()
{
    section("mapping_allocates_zero (including first call before catalog initialization)");
    ParameterValues values{}; SynthPatch p;
    // All-zero normalized controls are valid, including discrete endpoints.
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous = _CrtSetAllocHook(allocationHook);
    tracking = true;
#endif
    bool ok = true;
    for (int i=0; i<1000; ++i) ok = makeSynthPatch(values,p) && validateParameterValues(values) && ok;
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking = false; _CrtSetAllocHook(previous); CHECK(allocations==0);
#endif
    CHECK(ok);
}
void registry_is_fixed_and_complete()
{
    section("registry_is_fixed_and_complete");
    const auto registry = parameterRegistry(); CHECK(registry.size()==parameterCount);
    if (registry.size()!=parameterCount) return;
    CHECK(parameterRegistry().data()==registry.data());
    const auto defaults=defaultParameterValues(); CHECK(validateParameterValues(defaults));
    std::set<juce::String> ids,names; std::set<std::uint32_t> keys;
    juce::MemoryBlock keyBytes;
    for (std::size_t ri=0; ri<registry.size(); ++ri)
    { const auto& d=registry[ri];
    {
        CHECK(ids.insert(d.id).second); CHECK(names.insert(d.name).second); CHECK(d.name.isNotEmpty());
        CHECK(keys.insert(d.stateKey).second); CHECK(d.stateKey==fnv(d.id));
        if (ri>=9 && (ri<12 || ri>=212)) continue;
        const auto offset=keyBytes.getSize(); keyBytes.setSize(offset+4); write32(keyBytes,offset,d.stateKey);
    }
    }
    // Independent FNV fixture over the exact version-1 ordered IDs; frozen wire-key mapping.
    CHECK(juce::SHA256(keyBytes).toHexString()=="1bcde2a2b5f6966d9c2a73c772785151e5ef3c11f965f917fe8f264ec3435d60");
    auto field=[&](std::size_t index, const juce::String& id, const Field& f, ParameterKind kind)
    {
        const auto actualIndex=index<9?index:index+3;
        const auto& d=registry[actualIndex];
        CHECK(d.id==id); CHECK(d.kind==kind); CHECK(d.minimum==f.low); CHECK(d.maximum==f.high);
        CHECK(d.defaultValue==f.initial); CHECK(d.skew==f.skew); CHECK(d.unit==f.unit);
        CHECK(d.choices.size()==static_cast<std::size_t>(f.choices));
        const auto normalized=std::pow((f.initial-f.low)/(f.high-f.low),f.skew);
        CHECK(near(defaults[actualIndex],normalized));
    };
    field(0,"global.master_level",{"",0,1,.5f,1,0,""},ParameterKind::continuous);
    for (int i=1; i<=8; ++i) field(static_cast<std::size_t>(i),"macro."+juce::String(i),{"",0,1,0,1,0,""},ParameterKind::continuous);
    for (int slot=0; slot<4; ++slot)
    {
        const auto prefix="synth."+juce::String(slot+1)+".";
        const auto common=static_cast<std::size_t>(9+slot*4);
        field(common,prefix+"enabled",{"",0,1,slot==0?1.0f:0.0f,1,2,""},ParameterKind::toggle);
        field(common+1,prefix+"engine",{"",0,1,0,1,2,""},ParameterKind::choice);
        field(common+2,prefix+"level",{"",0,1,.5f,1,0,""},ParameterKind::continuous);
        field(common+3,prefix+"pan",{"",-1,1,0,1,0,""},ParameterKind::continuous);
        CHECK((registry[common+4].choices==std::vector<juce::String>{"Analog","Wavetable"}));
        for (int engine=0; engine<2; ++engine)
            for (int j=0; j<23; ++j)
            {
                const auto& f=j>=10?tail[j-10]:(engine==0?analog[j]:wavetable[j]);
                const auto index=static_cast<std::size_t>(25+engine*92+slot*23+j);
                field(index,prefix+(engine==0?"analog.":"wavetable.")+f.id,f,f.choices?ParameterKind::choice:ParameterKind::continuous);
                if (j==13) CHECK((registry[index+3].choices==std::vector<juce::String>{"Lowpass","Bandpass","Highpass"}));
                if (j==0||j==5)
                {
                    if (engine==0) CHECK((registry[index+3].choices==std::vector<juce::String>{"Sine","Triangle","Saw","Pulse"}));
                    else CHECK((registry[index+3].choices==std::vector<juce::String>{"0","1"}));
                }
            }
    }
    SynthPatch patch; CHECK(makeSynthPatch(defaults,patch)); expectPatch(patch,SynthPatch{});
}
void all_core_controls_map()
{
    section("all_core_controls_map");
    auto values=defaultParameterValues(); values[0]=.8f;
    for (int engine=0; engine<2; ++engine)
    {
        SynthPatch expected; expected.masterLevel=.8f;
        for (std::size_t slot=0; slot<4; ++slot)
        {
            const auto c=12+4*slot, b=28+92*static_cast<std::size_t>(engine)+23*slot;
            auto& l=expected.layers[slot];
            values[c]=1; values[c+1]=static_cast<float>(engine); values[c+2]=.1f*static_cast<float>(slot+1); values[c+3]=.25f;
            l.enabled=true; l.engine=engine==0?EngineKind::analog:EngineKind::wavetable; l.level=values[c+2]; l.pan=-.5f;
            if (engine==0)
            {
                const float v[]={1,.75f,.25f,.2f,.7f, 1.0f/3,.25f,.75f,.8f,.3f};
                std::copy(std::begin(v),std::end(v),values.begin()+static_cast<std::ptrdiff_t>(b));
                l.oscillators[0]={Waveform::pulse,0,0,12,-50,.2f,.68f};
                l.oscillators[1]={Waveform::triangle,0,0,-12,50,.8f,.32f};
            }
            else
            {
                const float v[]={1,.3f,.75f,.25f,.2f, 0,.8f,.25f,.75f,.8f};
                std::copy(std::begin(v),std::end(v),values.begin()+static_cast<std::ptrdiff_t>(b));
                l.oscillators[0]={Waveform::saw,1,.3f,12,-50,.2f,.5f};
                l.oscillators[1]={Waveform::saw,0,.8f,-12,50,.8f,.5f};
            }
            const float v[]={.3f,.5f,.8f,1, .5f,.25f,.7f,.75f, .25f,.75f,.2f,.5f, .75f};
            std::copy(std::begin(v),std::end(v),values.begin()+static_cast<std::ptrdiff_t>(b+10));
            l.noiseLevel=.3f; l.cutoffHz=1268.75f; l.resonance=.76f; l.filterMode=FilterMode::highpass;
            l.ampEnvelope={1.984251315f,.196862664f,7.666031f,.7f};
            l.filterEnvelope={.196862664f,7.666031f,1.984251315f,.2f}; l.filterEnvelopeOctaves=4;
            // Distinct bank/slot cutoff catches wrong bank base or wrong slot offset.
            values[b+11]=slot==0?.5f:static_cast<float>(slot)/4;
            const float cutoffs[]={1268.75f,98.046875f,1268.75f,6341.796875f}; l.cutoffHz=cutoffs[slot];
        }
        SynthPatch actual; CHECK(makeSynthPatch(values,actual)); expectPatch(actual,expected);
        SynthEngine core; CHECK(core.setPatch(actual));
    }
    // Every legal endpoint is consumable by the reviewed core (including skew rounding).
    for (float end:{0.0f,1.0f})
    {
        values=defaultParameterValues();
        for (std::size_t i=0;i<212;++i) values[i]=end;
        SynthPatch p; CHECK(makeSynthPatch(values,p)); SynthEngine core; CHECK(core.setPatch(p));
    }
}
void bank_switch_preserves_inactive_values()
{
    section("bank_switch_preserves_inactive_values");
    auto values=defaultParameterValues(); values[1]=.73f; values[8]=.21f;
    values[28+3*23+1]=.75f; // fourth analog tune +12
    values[120+3*23+2]=.25f; values[211]=1; // fourth wavetable tune -12 / envelope +8
    const auto original=values; SynthPatch analogPatch, wavePatch;
    CHECK(makeSynthPatch(values,analogPatch)); CHECK(near(analogPatch.layers[3].oscillators[0].tune,12));
    CHECK(values==original);
    for (std::size_t slot=0; slot<4; ++slot) values[13+4*slot]=1;
    const auto switched=values;
    CHECK(makeSynthPatch(values,wavePatch)); CHECK(values==switched);
    CHECK(near(wavePatch.layers[3].oscillators[0].tune,-12)); CHECK(near(wavePatch.layers[3].filterEnvelopeOctaves,8));
    juce::MemoryBlock state; CHECK(encodeSoundState(values,state)); ParameterValues recalled{};
    CHECK(decodeSoundState(state.getData(),state.getSize(),recalled)); CHECK(recalled==values);
    for (std::size_t slot=0; slot<4; ++slot) recalled[13+4*slot]=0;
    SynthPatch restored; CHECK(makeSynthPatch(recalled,restored)); CHECK(restored==analogPatch);
}
void reordered_state_roundtrips()
{
    section("reordered_state_roundtrips");
    auto values=defaultParameterValues();
    const auto registry=parameterRegistry(); if (registry.size()!=parameterCount) { CHECK(false); return; }
    for (std::size_t i=0; i<parameterCount; ++i) if (registry[i].kind==ParameterKind::continuous) values[i]=static_cast<float>(i)/static_cast<float>(parameterCount-1);
    juce::MemoryBlock bytes; CHECK(encodeSoundState(values,bytes)); CHECK(bytes.getSize()==currentSoundStateSize);
    if (bytes.getSize()!=currentSoundStateSize) return;
    CHECK(std::memcmp(bytes.getData(),"ARSI",4)==0); CHECK(read32(bytes,4)==currentSoundStateVersion); CHECK(read32(bytes,8)==parameterCount); CHECK(read32(bytes,12)==0);
    for (std::size_t i=0; i<parameterCount; ++i)
    { CHECK(read32(bytes,16+8*i)==registry[i].stateKey); CHECK(read32(bytes,20+8*i)==std::bit_cast<std::uint32_t>(values[i])); }
    auto reordered=bytes;
    for (std::size_t i=0; i<parameterCount; ++i)
        std::memcpy(static_cast<char*>(reordered.getData())+16+8*i,static_cast<const char*>(bytes.getData())+16+8*(parameterCount-1-i),8);
    sign(reordered); ParameterValues output{};
    CHECK(decodeSoundState(reordered.getData(),reordered.getSize(),output)); CHECK(output==values);
    SoundDocument poisoned;poisoned.values.fill(std::numeric_limits<float>::quiet_NaN());
    CHECK(decodeSoundState(reordered.getData(),reordered.getSize(),poisoned));
    for(std::size_t i=0;i<parameterCount;++i)CHECK(std::bit_cast<std::uint32_t>(poisoned.values[i])==std::bit_cast<std::uint32_t>(values[i]));
    // Unaligned caller storage is permitted.
    juce::MemoryBlock unaligned(currentSoundStateSize+1); std::memcpy(static_cast<char*>(unaligned.getData())+1,reordered.getData(),currentSoundStateSize);
    CHECK(decodeSoundState(static_cast<char*>(unaligned.getData())+1,currentSoundStateSize,output)); CHECK(output==values);
    juce::MemoryBlock again; CHECK(encodeSoundState(output,again)); CHECK(again==bytes);
}
void corrupt_state_has_no_mutation()
{
    section("corrupt_state_has_no_mutation");
    const auto values=defaultParameterValues(); const auto registry=parameterRegistry();
    juce::MemoryBlock good; CHECK(encodeSoundState(values,good));
    if (good.getSize()!=currentSoundStateSize||registry.size()!=parameterCount) { CHECK(false); return; }
    ParameterValues destination; destination.fill(.123f); const auto prior=destination;
    auto reject=[&](const void* data,std::size_t size) { CHECK(!decodeSoundState(data,size,destination)); CHECK(destination==prior); };
    reject(nullptr,currentSoundStateSize); reject(nullptr,0);
    for (std::size_t size=0; size<currentSoundStateSize; ++size) reject(good.getData(),size);
    auto extra=good; extra.setSize(currentSoundStateSize+1); reject(extra.getData(),extra.getSize());
    for (std::size_t offset:{0u,4u,8u,12u,16u,20u,6992u,7023u})
    { auto bad=good; static_cast<unsigned char*>(bad.getData())[offset]^=1; reject(bad.getData(),bad.getSize()); }
    for (auto pair:{std::pair<std::size_t,std::uint32_t>{0,0}, {4,0}, {4,4}, {8,208}, {8,210}, {12,1}, {16,0}})
    { auto bad=good; write32(bad,pair.first,pair.second); sign(bad); reject(bad.getData(),bad.getSize()); }
    auto duplicate=good; write32(duplicate,16+8*757,registry[0].stateKey); sign(duplicate); reject(duplicate.getData(),duplicate.getSize());
    SynthPatch patch; patch.masterLevel=.23f; const auto oldPatch=patch;
    auto badValue=[&](std::size_t index,float v)
    {
        auto invalid=values; invalid[index]=v;
        CHECK(!validateParameterValues(invalid)); CHECK(!makeSynthPatch(invalid,patch)); CHECK(patch==oldPatch);
        juce::MemoryBlock encoded("keep",4); CHECK(!encodeSoundState(invalid,encoded)); CHECK(encoded==juce::MemoryBlock("keep",4));
        auto bad=good; write32(bad,20+8*index,std::bit_cast<std::uint32_t>(v)); sign(bad); reject(bad.getData(),bad.getSize());
    };
    for (std::size_t i=0; i<parameterCount; ++i)
    {
        for (float v:{-.001f,1.001f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) badValue(i,v);
        if (registry[i].kind==ParameterKind::continuous || (i>=252 && i<572)) continue;
        const auto steps=static_cast<float>(registry[i].choices.size()-1);
        badValue(i,.5f/steps);
        for (int j=0; j<=static_cast<int>(steps); ++j)
            for (float delta:{-0.5e-6f,0.0f,0.5e-6f})
            {
                const auto value=static_cast<float>(j)/steps+delta;
                if (value<0||value>1) continue;
                auto valid=values; valid[i]=value; CHECK(validateParameterValues(valid));
                SynthPatch p; CHECK(makeSynthPatch(valid,p)); SynthEngine core; CHECK(core.setPatch(p));
                auto canonical=values; canonical[i]=static_cast<float>(j)/steps;
                SynthPatch exact; CHECK(makeSynthPatch(canonical,exact)); CHECK(p==exact);
                juce::MemoryBlock encoded; CHECK(encodeSoundState(valid,encoded)); ParameterValues decoded{};
                CHECK(decodeSoundState(encoded.getData(),encoded.getSize(),decoded)); CHECK(decoded==valid);
            }
        badValue(i,2e-6f); badValue(i,1-2e-6f);
    }
}
std::size_t indexOf(const juce::String& id)
{
    const auto r=parameterRegistry();
    const auto it=std::find_if(r.begin(),r.end(),[&](const auto& d){return d.id==id;});
    CHECK(it!=r.end()); return it==r.end()?0:static_cast<std::size_t>(it-r.begin());
}
void put(SoundDocument& d,const juce::String& id,float physical)
{
    const auto i=indexOf(id); const auto& f=parameterRegistry()[i];
    d.values[i]=juce::NormalisableRange<float>(f.minimum,f.maximum,0,f.skew).convertTo0to1(physical);
}
std::array<float,23> effectNumbers(const EffectSettings& e)
{
    return {float(e.bypassed),e.drive.gainDb,float(e.drive.shape),e.drive.toneHz,e.drive.mix,
        float(e.delay.synced),e.delay.seconds,e.delay.beats,e.delay.feedback,e.delay.stereo,e.delay.filterHz,e.delay.mix,
        e.reverb.size,e.reverb.decaySeconds,e.reverb.damping,e.reverb.predelayMs,e.reverb.mix,
        e.compressor.thresholdDb,e.compressor.ratio,e.compressor.attackMs,e.compressor.releaseMs,e.compressor.makeupDb,e.compressor.mix};
}
constexpr Field effectFields[] {
    {"bypassed",0,1,0,1,2,""}, {"drive.gain_db",0,24,6,1,0,"dB"}, {"drive.shape",0,1,0,1,2,""},
    {"drive.tone_hz",200,20000,12000,1,0,"Hz"},{"drive.mix",0,1,.5f,1,0,""},
    {"delay.synced",0,1,0,1,2,""},{"delay.seconds",.001f,4,.25f,1,0,"s"},{"delay.beats",.0625f,1,.5f,1,0,"beats"},
    {"delay.feedback",0,.95f,.35f,1,0,""},{"delay.stereo",0,1,0,1,0,""},{"delay.filter_hz",20,20000,12000,1,0,"Hz"},{"delay.mix",0,1,.25f,1,0,""},
    {"reverb.size",0,1,.5f,1,0,""},{"reverb.decay_seconds",.1f,20,2,1,0,"s"},{"reverb.damping",0,1,.5f,1,0,""},{"reverb.predelay_ms",0,250,0,1,0,"ms"},{"reverb.mix",0,1,.25f,1,0,""},
    {"compressor.threshold_db",-60,0,-18,1,0,"dB"},{"compressor.ratio",1,20,4,1,0,""},{"compressor.attack_ms",.1f,100,10,1,0,"ms"},
    {"compressor.release_ms",10,2000,100,1,0,"ms"},{"compressor.makeup_db",0,24,0,1,0,"dB"},{"compressor.mix",0,1,1,1,0,""}
};
void completeCatalogAndMapping()
{
    section("completeCatalogAndMapping"); CHECK(parameterCount==4519);
    const auto r=parameterRegistry(); SoundDocument d; RoutedPatch patch;
    CHECK(validateSoundDocument(d)); CHECK(makeRoutedPatch(d,patch));
    CHECK(patch.layout.synthCount==1);
    // Appended skewed physical defaults pass through normalized float
    // controls; frequency skew can round that inverse mapping by a few ULPs.
    // Retain exact equality for every old effect field and explicit physical
    // default fences for the new fields rather than require inverse bit parity.
    auto expectedEffects=RoutedPatch{}.effects;
    for(std::size_t slot=0;slot<8;++slot){
        CHECK(std::abs(patch.effects[slot].saturation.lowCrossoverHz-180.f)<.001f);
        expectedEffects[slot].saturation.lowCrossoverHz=patch.effects[slot].saturation.lowCrossoverHz;
        const auto& ms=patch.effects[slot].midSideSplit;
        CHECK(ms.midGainDb==0&&!ms.midMuted&&ms.midTone==0&&ms.sideGainDb==0&&!ms.sideMuted&&ms.sideTone==0&&ms.mix==1);
        const auto& split=patch.effects[slot].frequencySplit;expectedEffects[slot].frequencySplit=split;
        CHECK(std::abs(split.crossoverHz-1000.f)<.001f&&split.lowGainDb==0&&!split.lowMuted&&split.lowTone==0
            &&split.highGainDb==0&&!split.highMuted&&split.highTone==0&&split.mix==1);
        expectedEffects[slot].dynamicEq=patch.effects[slot].dynamicEq;
        const auto& era=patch.effects[slot].eraReverb;expectedEffects[slot].eraReverb=era;
        CHECK(era.voicing==EraVoicing::hall&&era.age==EraAge::dark&&near(era.decaySeconds,4)&&near(era.predelayMs,20)&&near(era.size,1)&&near(era.attack,.5f)
            &&near(era.diffusion,1)&&near(era.wobble,.38f)&&near(era.wobbleRateHz,2.53f)&&era.grit==0&&near(era.bassMultiplier,1.5f)
            &&near(era.dampingHz,6000)&&near(era.lowCutHz,10)&&near(era.highCutHz,8000)&&near(era.mix,.25f));
        const auto& spaceSettings=patch.effects[slot].spaceReverb;expectedEffects[slot].spaceReverb=spaceSettings;
        CHECK(near(float(spaceSettings.size),.5f)&&near(float(spaceSettings.decay.decaySeconds),2)&&near(float(spaceSettings.predelayMs),20)
            &&near(float(spaceSettings.decay.brightness),.5f)&&near(float(spaceSettings.character),.25f)&&near(float(spaceSettings.distance),.5f)
            &&near(float(spaceSettings.width),1)&&near(float(spaceSettings.mix),.25f)&&spaceSettings.duckRangeDb==0
            &&near(float(spaceSettings.duckThresholdDb),-24)&&near(float(spaceSettings.duckAttackMs),10)&&near(float(spaceSettings.duckReleaseMs),250));
        for(std::size_t band=0;band<6;++band){const auto& decay=spaceSettings.decay.bands[band];const auto& post=spaceSettings.post[band];
            CHECK(!decay.active&&!decay.bypassed&&decay.shape==space::DecayShape::bell
                &&near(float(decay.frequencyHz),1000)&&near(float(decay.multiplier),1)&&near(float(decay.q),1));
            CHECK(!post.active&&!post.bypassed&&post.shape==space::PostShape::bell&&post.slope==space::CutSlope::db12
                &&near(float(post.frequencyHz),1000)&&post.gainDb==0&&near(float(post.bellQ),1)&&near(float(post.shelfQ),1));
        }
        const auto& eq=patch.effects[slot].eq;expectedEffects[slot].eq=eq;
        const auto& filter=patch.effects[slot].filter;expectedEffects[slot].filter=filter;
        const auto& chorus=patch.effects[slot].chorus;expectedEffects[slot].chorus=chorus;
        const auto& flanger=patch.effects[slot].flanger;expectedEffects[slot].flanger=flanger;
        CHECK(near(flanger.rateHz,.25f)&&near(flanger.depth,.6f)&&near(flanger.baseDelayMs,2)&&near(flanger.feedback,.35f)&&near(flanger.mix,.5f));
        const auto& phaser=patch.effects[slot].phaser;expectedEffects[slot].phaser=phaser;
        CHECK(near(phaser.rateHz,.35f)&&near(phaser.depth,.6f)&&near(phaser.centreHz,1300)&&near(phaser.feedback,.2f)&&near(phaser.mix,.5f));
        const auto& delay=patch.effects[slot].delay;expectedEffects[slot].delay.wobbleRateHz=delay.wobbleRateHz;
        CHECK(delay.saturation==0&&delay.lowCutHz==20&&delay.wobble==0&&near(delay.wobbleRateHz,.8f)&&!delay.glide);
        CHECK(near(chorus.rateHz,.35f)&&near(chorus.depth,.25f)&&near(chorus.baseDelayMs,18)&&near(chorus.feedback,.1f)&&near(chorus.mix,.25f));
        CHECK(filter.mode==FilterFxMode::lowpass&&std::abs(filter.cutoffHz-4000.f)<.01f&&std::abs(filter.resonance-.70710678f)<1e-6f&&filter.driveDb==0&&filter.mix==1);
        CHECK(std::abs(eq.lowShelf.frequencyHz-160.f)<.001f&&eq.lowShelf.gainDb==0&&std::abs(eq.lowShelf.q-.70710678f)<1e-6f);
        CHECK(std::abs(eq.bell.frequencyHz-1000.f)<.01f&&eq.bell.gainDb==0&&eq.bell.q==1);
        CHECK(std::abs(eq.highShelf.frequencyHz-8000.f)<.01f&&eq.highShelf.gainDb==0&&std::abs(eq.highShelf.q-.70710678f)<1e-6f);
    }
    CHECK(patch.effects==expectedEffects);
    std::cout<<"EQ_DEFAULT_PHYSICAL "<<patch.effects[0].eq.lowShelf.frequencyHz<<' '<<patch.effects[0].eq.bell.frequencyHz<<' '<<patch.effects[0].eq.highShelf.frequencyHz<<' '<<patch.effects[0].eq.lowShelf.q<<'\n';
    CHECK(!parameterIndexForKey(0)); CHECK(!parameterIndexForKey(0xffffffffu));
    std::set<std::uint32_t> keys;
    for(const auto& field:r)keys.insert(field.stateKey);
    for(const auto& field:r)for(const auto probe:{field.stateKey-1,field.stateKey+1})
        if(!keys.contains(probe))CHECK(!parameterIndexForKey(probe));
    auto descriptor=[&](std::size_t i,const juce::String& id,const Field& f,bool toggle=false)
    {
        const auto& x=r[i]; CHECK(x.id==id); CHECK(x.stateKey==fnv(id)); CHECK(parameterIndexForKey(x.stateKey)==i);
        CHECK(x.minimum==f.low); CHECK(x.maximum==f.high); CHECK(x.defaultValue==f.initial); CHECK(x.skew==f.skew); CHECK(x.unit==f.unit);
        CHECK(x.choices.size()==std::size_t(f.choices)); CHECK(x.kind==(toggle?ParameterKind::toggle:f.choices?ParameterKind::choice:ParameterKind::continuous));
        if(toggle) CHECK((x.choices==std::vector<juce::String>{"Off","On"}));
    };
    descriptor(9,"global.voice_mode",{"",0,1,0,1,2,""}); descriptor(10,"global.glide_seconds",{"",0,2,0,1,0,"s"}); descriptor(11,"global.legato",{"",0,1,0,1,2,""},true);
    CHECK((r[9].choices==std::vector<juce::String>{"Polyphonic","Monophonic"}));
    for(float end:{0.f,1.f}) {
        d.values[9]=d.values[10]=d.values[11]=end; CHECK(makeRoutedPatch(d,patch));
        CHECK(patch.synth.performance.mode==static_cast<VoiceMode>(int(end))); CHECK(patch.synth.performance.glideSeconds==2*end); CHECK(patch.synth.performance.legato==bool(end));
    }
    constexpr Field lf[]={{"shape",0,3,0,1,4,""},{"synced",0,1,0,1,2,""},{"rate_hz",.05f,40,1,1,0,"Hz"},{"beats",.0625f,16,1,1,0,"beats"}};
    constexpr Field mf[]={{"enabled",0,1,0,1,2,""},{"source",0,6,0,1,7,""},{"target",0,16,0,1,17,""},{"bank",0,2,0,1,3,""},{"amount",-1,1,0,1,0,""}};
    for(std::size_t s=0;s<4;++s) {
        const auto prefix="synth."+juce::String(s+1)+".";
        for(std::size_t k=0;k<2;++k) {
            const auto i=212+s*2+k; descriptor(i,prefix+(k==0?"mute":"solo"),{"",0,1,0,1,2,""},true);
            d=SoundDocument{}; d.values[i]=1; CHECK(makeRoutedPatch(d,patch)); CHECK((k==0?patch.mute[s]:patch.solo[s]));
            for(std::size_t f=0;f<4;++f) {
                const auto n=220+s*8+k*4+f; descriptor(n,prefix+"lfo."+juce::String(k+1)+"."+lf[f].id,lf[f],f==1);
                for(float end:{0.f,1.f}) {d=SoundDocument{}; d.values[n]=end; CHECK(makeRoutedPatch(d,patch)); const auto& l=patch.synth.layers[s].lfos[k];
                    const float actual[]={float(l.shape),float(l.synced),l.rateHz,l.beats}; CHECK(near(actual[f],end==0?lf[f].low:lf[f].high));}
            }
        }
        for(std::size_t k=0;k<16;++k) {
            const auto base=252+s*80+k*5;
            for(std::size_t f=0;f<5;++f) descriptor(base+f,prefix+"modulation."+juce::String(k+1)+"."+mf[f].id,mf[f],f==0);
            // All bank/target/source combinations, including disabled invalid banks.
            for(int target=0;target<=16;++target) for(int bank=0;bank<3;++bank) for(int source=0;source<=6;++source) {
                d=SoundDocument{}; d.values[base+1]=float(source)/6; d.values[base+2]=float(target)/16; d.values[base+3]=float(bank)/2;
                const bool bankOK=target<=2?bank==0:target==9||target==10?bank==1:target==11||target==12?bank==2:bank!=0;
                for(int enabled=0;enabled<2;++enabled) {d.values[base]=float(enabled); const bool valid=bankOK&&(!enabled||(source!=0&&target!=0));
                    const auto before=patch; CHECK(makeRoutedPatch(d,patch)==valid); if(!valid)CHECK(patch==before);
                    else {const auto& m=patch.synth.layers[s].modulation[k]; CHECK(m.enabled==bool(enabled));CHECK(int(m.source)==source);CHECK(int(m.target)==target);CHECK(int(m.bank)==bank);}
                }
            }
            for(float end:{0.f,1.f}) {d=SoundDocument{};d.values[base+4]=end;CHECK(makeRoutedPatch(d,patch));CHECK(patch.synth.layers[s].modulation[k].amount==2*end-1);}
        }
    }
    for(std::size_t e=0;e<8;++e)for(std::size_t f=0;f<23;++f) {
        const auto i=572+e*23+f; const auto& expected=effectFields[f]; descriptor(i,"effect."+juce::String(e+1)+"."+expected.id,expected,f==0||f==5);
        for(float end:{0.f,1.f}) {d=SoundDocument{};d.values[i]=end;CHECK(makeRoutedPatch(d,patch));CHECK(near(effectNumbers(patch.effects[e])[f],end==0?expected.low:expected.high));}
    }
}
void strictDocumentCodec()
{
    section("strictDocumentCodec"); SoundDocument d;juce::MemoryBlock good; CHECK(encodeSoundState(d,good)); CHECK(good.getSize()==currentSoundStateSize); if(good.getSize()!=currentSoundStateSize)return;
    CHECK(read32(good,4)==currentSoundStateVersion);CHECK(read32(good,8)==parameterCount);
    SoundDocument out;out.values[0]=.17f;const auto before=out;
    auto reject=[&](const juce::MemoryBlock& b){CHECK(!decodeSoundState(b.getData(),b.getSize(),out));CHECK(out==before);};
    for(std::size_t n=0;n<currentSoundStateSize;++n) {CHECK(!decodeSoundState(good.getData(),n,out));CHECK(out==before);}
    for(std::size_t n=0;n<currentSoundStateSize;++n) {auto bad=good;static_cast<unsigned char*>(bad.getData())[n]^=1;reject(bad);}
    auto badByte=[&](std::size_t n,unsigned char v){auto bad=good;static_cast<unsigned char*>(bad.getData())[n]=v;sign(bad);reject(bad);};
    // Every explicit boolean and enum wire field rejects out-of-domain bytes even with a correct digest.
    for(std::size_t n=controlLockOffset;n<currentSoundNumericBytes;++n)badByte(n,2);
    for(std::size_t n=layoutOffset;n<layoutOffset+4;++n)badByte(n,3);
    for(std::size_t e=0;e<8;++e) {badByte(layoutOffset+9+e*3,3);badByte(layoutOffset+10+e*3,255);badByte(layoutOffset+11+e*3,5);}
    badByte(layoutOffset+8,5);badByte(layoutOffset+4,4);badByte(layoutOffset+73,9);badByte(layoutOffset+33,8);
    for(auto [n,v]:{std::pair<std::size_t,std::uint32_t>{4,1},{4,4},{8,757},{12,1},{16,0},{16+8*757,read32(good,16)},{macroOffset,0xffffffffu},{macroOffset+4,0x7fc00000u},{macroOffset+4,0x3f800000u}}) {auto bad=good;write32(bad,n,v);sign(bad);reject(bad);}
    auto extra=good;extra.setSize(currentSoundStateSize+1);reject(extra);
    // Decode into memory which also backs the input: publication happens only after a complete parse.
    CHECK(decodeSoundState(good.getData(),good.getSize(),out));CHECK(out==d);
    juce::MemoryBlock bytes;CHECK(encodeSoundState(d,bytes));CHECK(bytes==good);
    section("ownership_safe_codec_aliases");
    struct CodecAliasState : RackPersonalityState { juce::MemoryBlock wire; };
    auto owner=std::make_shared<CodecAliasState>();owner->wire=good;SoundDocument aliased=d;aliased.personalities=owner;
    std::weak_ptr<CodecAliasState> weak=owner;const auto* input=owner->wire.getData();owner.reset();
    CHECK(decodeSoundState(input,good.getSize(),aliased));CHECK(weak.expired());CHECK(aliased==d);
    owner=std::make_shared<CodecAliasState>();owner->wire=good;static_cast<char*>(owner->wire.getData())[0]^=1;const auto sentinel=owner->wire;
    aliased=d;aliased.personalities=owner;weak=owner;input=owner->wire.getData();owner.reset();
    CHECK(!decodeSoundState(input,sentinel.getSize(),aliased));CHECK(!weak.expired());CHECK(weak.lock()->wire==sentinel);CHECK(aliased==d);
    for(auto key:{fnv("macro.1"),fnv("synth.1.engine"),fnv("effect.1.bypassed")}) {
        auto bad=good;write32(bad,macroOffset,key);sign(bad);out=before;reject(bad);
    }
    for(float amount:{-1.01f,1.01f,-std::numeric_limits<float>::infinity()}) {
        auto bad=good;write32(bad,macroOffset,fnv("global.master_level"));write32(bad,macroOffset+4,std::bit_cast<std::uint32_t>(amount));sign(bad);out=before;reject(bad);
    }
}
void named_sound_state()
{
    section("named_sound_state");
    SoundDocument unnamed;juce::MemoryBlock plain;CHECK(encodeSoundState(unnamed,plain));
    CHECK(plain.getSize()==currentSoundStateSize);CHECK(read32(plain,4)==currentSoundStateVersion);
    SoundDocument named;named.name="Ethereal Rolling Bass";juce::MemoryBlock bytes;CHECK(encodeSoundState(named,bytes));
    CHECK(bytes.getSize()==currentSoundStateSize+21);
    SoundDocument back;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),back));CHECK(back.name=="Ethereal Rolling Bass");CHECK(back==named);CHECK(!(back==unnamed));
    // Forty four-byte letters are the longest name the format holds.
    juce::String longest;for(int i=0;i<40;++i)longest+=juce::String(juce::CharPointer_UTF8("\xf0\x9d\x90\x80"));
    named.name=longest;CHECK(encodeSoundState(named,bytes));CHECK(bytes.getSize()==currentSoundStateSize+160);CHECK(bytes.getSize()<=maximumSoundStateBytes);
    CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),back));CHECK(back.name==longest);
    named.name=longest+"a";CHECK(!encodeSoundState(named,bytes));
    named.name="Ab";CHECK(encodeSoundState(named,bytes));const auto length=bytes.getSize()-32-2-4;
    for(std::uint32_t wrong:{1u,3u,161u}){auto bad=bytes;write32(bad,length,wrong);sign(bad);back=unnamed;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),back));CHECK(back==unnamed);}
    {auto bad=bytes;static_cast<unsigned char*>(bad.getData())[length+4]=0xff;sign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),back));}
    // Project the former Pro-R registry, excluding the separately published spectral bank.
    if(plain.getSize()!=currentSoundStateSize)return;
    const auto* raw=static_cast<const char*>(plain.getData());juce::MemoryOutputStream prior;
    const auto mapped=[](std::size_t i){return i<2535?i:spaceReverbParameterStart+i-2535;};
    prior.write(raw,16);for(std::size_t i=0;i<3303;++i)prior.write(raw+16+8*mapped(i),8);
    const auto layout=16+8*parameterCount,locks=layout+78+64;prior.write(raw+layout,78+64);
    for(std::size_t i=0;i<3303;++i)prior.write(raw+locks+mapped(i),1);
    prior.write(raw+currentSoundNumericBytes-12,12);
    prior.writeInt(16+8*17);prior.writeShort(1);prior.writeByte(4);prior.writeByte(2);prior.writeByte(8);prior.writeByte(17);prior.writeShort(0);
    prior.write(raw+currentSoundNumericBytes+12,8);for(std::size_t slot=0;slot<8;++slot)prior.write(raw+currentSoundNumericBytes+20+18*slot,17);
    const auto personality=currentSoundNumericBytes+4+kernelCompatibilitySectionBytes;
    CHECK(read32(plain,personality)==0);prior.writeInt(0);
    prior.write(juce::MemoryBlock(32,true).getData(),32);
    auto old=prior.getMemoryBlock();write32(old,4,24);write32(old,8,3303);sign(old);
    CHECK(old.getSize()==proRSound24StateSize);
    SoundDocument recalled;recalled.name="Previous";CHECK(decodeSoundState(old.getData(),old.getSize(),recalled));CHECK(recalled.name.isEmpty());CHECK(recalled==unnamed);
}
void historicalM1Migration()
{
    section("historicalM1Migration");
    const char* names[]={"m1-analog-pluck.soundstate","m1-wavetable-stab.soundstate"};
    const char* hashes[]={"6fbc457ffb3bbd821a458244aa5135857b5356c946a16924b3c494ad529698f4","e2fdfa26e60ca64797e34d7434a67c854ff6ab73e3d87b1c44a940ae42133209"};
    for(int fixture=0;fixture<2;++fixture) {
        juce::MemoryBlock raw; CHECK(juce::File(RACKSYNTH_FIXTURE_DIR).getChildFile(names[fixture]).loadFileAsData(raw));CHECK(raw.getSize()==1720);CHECK(juce::SHA256(raw).toHexString()==hashes[fixture]);
        SoundDocument d;const bool ok=decodeSoundState(raw.getData(),raw.getSize(),d);CHECK(ok);if(!ok)continue;
        {CHECK(near(parameterPhysicalValue(d.values,756),0));CHECK(d.values[757]==0);CHECK(!d.controlLocks[756]&&!d.controlLocks[757]);}
        CHECK(d.layout.synthCount==4);CHECK((d.layout.synthOrder==std::array<std::uint8_t,4>{0,1,2,3}));
        auto oldSign=[](juce::MemoryBlock& b){const auto hash=juce::SHA256(b.getData(),1688).getRawData();std::memcpy(static_cast<char*>(b.getData())+1688,hash.getData(),32);};
        const auto fresh=SoundDocument{};
        for(std::size_t i=0;i<209;++i) {const auto index=parameterIndexForKey(read32(raw,16+8*i));CHECK(index.has_value());if(index)CHECK(d.values[*index]==std::bit_cast<float>(read32(raw,20+8*i)));}
        CHECK(d.macros==fresh.macros);CHECK(d.controlLocks==fresh.controlLocks);CHECK(d.synthLocks==fresh.synthLocks);CHECK(d.effectLocks==fresh.effectLocks);
        for(std::size_t i=0;i<756;++i)if((i>=9&&i<12)||i>=212)CHECK(d.values[i]==fresh.values[i]);
        auto reversed=raw;for(std::size_t i=0;i<209;++i)std::memcpy(static_cast<char*>(reversed.getData())+16+8*i,static_cast<const char*>(raw.getData())+16+8*(208-i),8);oldSign(reversed);
        SoundDocument reversedDoc;CHECK(decodeSoundState(reversed.getData(),1720,reversedDoc));CHECK(reversedDoc==d);
        for(std::size_t slot=1;slot<4;++slot)for(float enabled:{0.f,1.f}) {
            auto legacy=raw;for(std::size_t i=0;i<209;++i)if(read32(legacy,16+8*i)==fnv("synth."+juce::String(slot+1)+".enabled"))write32(legacy,20+8*i,std::bit_cast<std::uint32_t>(enabled));oldSign(legacy);
            SoundDocument migrated;CHECK(decodeSoundState(legacy.getData(),1720,migrated));RoutedPatch p;CHECK(makeRoutedPatch(migrated,p));CHECK(p.layout.synthUse[slot]==SlotUse::present);CHECK(p.synth.layers[slot].enabled==bool(enabled));
            put(migrated,"synth."+juce::String(slot+1)+".enabled",1);CHECK(makeRoutedPatch(migrated,p));CHECK(p.synth.layers[slot].enabled);
        }
        juce::MemoryBlock v2;CHECK(encodeSoundState(d,v2));CHECK(v2.getSize()==currentSoundStateSize);SoundDocument restored;CHECK(decodeSoundState(v2.getData(),v2.getSize(),restored));CHECK(restored==d);
        const auto keep=d;for(std::size_t n=0;n<1720;++n){CHECK(!decodeSoundState(raw.getData(),n,d));CHECK(d==keep);}
        for(auto [n,v]:{std::pair<std::size_t,std::uint32_t>{4,2},{8,210},{12,1},{16,0},{16+8*208,read32(raw,16)},{20,0x7fc00000u},{20,0xbf800000u}}){auto bad=raw;write32(bad,n,v);oldSign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),d));CHECK(d==keep);}
    }
}
void retainedBanksAndMacros()
{
    section("retainedBanksAndMacros");SoundDocument d;CHECK(validateSoundDocument(d));
    put(d,"synth.4.analog.osc.1.tune",12);put(d,"effect.8.reverb.mix",.73f);put(d,"synth.3.modulation.16.amount",-.7f);
    CHECK(editRackLayout(d.layout,{RackEditKind::addSynth},d.layout)==RackEditResult::applied);
    CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,noSlot,1,0,EffectKind::delay},d.layout)==RackEditResult::applied);
    CHECK(editRackLayout(d.layout,{RackEditKind::removeEffect,0},d.layout)==RackEditResult::applied);
    const auto target=indexOf("synth.4.analog.osc.1.tune");d.macros[0]={parameterRegistry()[target].stateKey,.5f};d.macros[1]={parameterRegistry()[target].stateKey,-.25f};d.values[1]=.8f;d.values[2]=.4f;
    const auto base=d;RoutedPatch p;CHECK(makeRoutedPatch(d,p));CHECK(near(p.synth.layers[3].oscillators[0].tune,24));CHECK(d==base);
    d.macros[0].amount=-.5f;CHECK(makeRoutedPatch(d,p));CHECK(near(p.synth.layers[3].oscillators[0].tune,-12));
    d.macros={};CHECK(makeRoutedPatch(d,p));CHECK(near(p.synth.layers[3].oscillators[0].tune,12));CHECK(d.values==base.values);
    d=base;d.controlLocks[target]=true;d.synthLocks[1]=true;d.effectLocks[0]=true;
    juce::MemoryBlock bytes;CHECK(encodeSoundState(d,bytes));SoundDocument round;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),round));CHECK(round==d);
    for(auto binding:{MacroBinding{0,.1f},MacroBinding{fnv("macro.1"),.2f},MacroBinding{fnv("synth.1.engine"),.2f},MacroBinding{0xffffffffu,0},MacroBinding{fnv("global.master_level"),1.01f},MacroBinding{fnv("global.master_level"),std::numeric_limits<float>::quiet_NaN()}}) {
        auto bad=d;bad.macros[7]=binding;const auto before=p;CHECK(!validateSoundDocument(bad));CHECK(!makeRoutedPatch(bad,p));CHECK(p==before);juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(bad,keep));CHECK(keep==juce::MemoryBlock("keep",4));
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous=_CrtSetAllocHook(allocationHook); allocations=0;tracking=true;
#endif
    bool ok=true;for(int i=0;i<1000;++i)ok=validateSoundDocument(d)&&makeRoutedPatch(d,p)&&validateAiSoundEdit(d,d)&&parameterIndexForKey(d.macros[0].targetKey).has_value()&&!parameterIndexForKey(0).has_value()&&ok;
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(previous);CHECK(allocations==0);std::cout<<"prepared document allocations "<<allocations<<'\n';
#endif
    CHECK(ok);
}
void localLockPolicy()
{
    section("localLockPolicy");SoundDocument d;CHECK(validateAiSoundEdit(d,d));
    CHECK(editRackLayout(d.layout,{RackEditKind::addSynth},d.layout)==RackEditResult::applied);
    CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,noSlot,0,0,EffectKind::drive},d.layout)==RackEditResult::applied);
    CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,noSlot,0,1,EffectKind::delay},d.layout)==RackEditResult::applied);
    const auto cutoff=indexOf("synth.1.analog.filter.cutoff"),fx=indexOf("effect.1.delay.feedback");
    d.controlLocks[cutoff]=true;d.controlLocks[fx]=true;
    int lockCase=0;
    auto reject=[&](const SoundDocument& candidate){++lockCase; if(!validateSoundDocument(candidate)||validateAiSoundEdit(d,candidate))std::cout<<"lock case "<<lockCase<<'\n';CHECK(validateSoundDocument(candidate));CHECK(!validateAiSoundEdit(d,candidate));RoutedPatch manual;CHECK(makeRoutedPatch(candidate,manual));};
    auto c=d;c.values[cutoff]=.3f;reject(c);c=d;c.values[fx]=.9f;reject(c);
    c=d;put(c,"synth.1.engine",1);reject(c);
    c=d;CHECK(editRackLayout(c.layout,{RackEditKind::removeSynth,0},c.layout)==RackEditResult::applied);reject(c);
    c=d;CHECK(editRackLayout(c.layout,{RackEditKind::replaceEffect,0,masterChain,0,EffectKind::reverb,true},c.layout)==RackEditResult::applied);reject(c);
    c=d;CHECK(editRackLayout(c.layout,{RackEditKind::removeEffect,0},c.layout)==RackEditResult::applied);reject(c);
    c=d;c.controlLocks[cutoff]=false;reject(c);c=d;c.synthLocks[2]=true;reject(c);c=d;c.effectLocks[2]=true;reject(c);
    // Base lock permits modulation/automation; rack locks additionally preserve all banks, order and relevant assignments.
    c=d;c.macros[0]={parameterRegistry()[cutoff].stateKey,.4f};CHECK(validateAiSoundEdit(d,c));
    d.synthLocks[0]=true;d.effectLocks[0]=true;
    for(const auto& id:{"synth.1.wavetable.osc.2.position","synth.1.modulation.16.amount","synth.1.solo","effect.1.reverb.mix"}){c=d;c.values[indexOf(id)]=1;reject(c);}
    c=d;CHECK(editRackLayout(c.layout,{RackEditKind::moveSynth,0,masterChain,0},c.layout)==RackEditResult::applied);reject(c);
    c=d;CHECK(editRackLayout(c.layout,{RackEditKind::moveEffect,0,masterChain,0},c.layout)==RackEditResult::applied);reject(c);
    c=d;CHECK(editRackLayout(c.layout,{RackEditKind::moveEffect,0,0,1},c.layout)==RackEditResult::applied);reject(c);
    c=d;c.macros[3]={parameterRegistry()[cutoff].stateKey,.2f};reject(c);
    c=d;c.values[indexOf("synth.2.level")]=.2f;CHECK(validateAiSoundEdit(d,c));
    d.controlLocks[1]=true;c=d;c.macros[0]={fnv("global.master_level"),.3f};reject(c);
    c=d;c.values[1]=.2f;reject(c);
    // Existing assignment cannot be retargeted away from a locked rack either.
    d.macros[3]={parameterRegistry()[fx].stateKey,.2f};c=d;c.macros[3]={};reject(c);
}

void calibratedGlobalsAndLegacyV2()
{
    section("calibratedGlobalsAndLegacyV2");
    const auto r=parameterRegistry();CHECK(r.size()==parameterCount);if(r.size()!=parameterCount)return;
    CHECK(r[755].id=="effect.8.compressor.mix");
    const auto& gain=r[756];const auto& guard=r[757];
    CHECK(gain.id=="output.gain_db"&&gain.unit=="dB"&&gain.kind==ParameterKind::continuous);
    CHECK(gain.minimum==-24&&gain.maximum==36&&gain.defaultValue==30&&gain.skew==1);
    CHECK(guard.id=="output.protection"&&guard.unit.isEmpty()&&guard.kind==ParameterKind::toggle);
    CHECK(guard.minimum==0&&guard.maximum==1&&guard.defaultValue==1&&guard.skew==1);
    CHECK((guard.choices==std::vector<juce::String>{"Off","On"}));
    CHECK(isContinuousParameter(756)&&!isContinuousParameter(757));
    SoundDocument d;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));CHECK(near(patch.outputGainDb,30)&&patch.outputProtection);
    for(float db:{-24.f,0.f,30.f,36.f})for(float on:{0.f,1.f}){
        put(d,"output.gain_db",db);put(d,"output.protection",on);CHECK(makeRoutedPatch(d,patch));CHECK(near(patch.outputGainDb,db)&&patch.outputProtection==bool(on));
    }
    d=SoundDocument{};d.synthLocks.fill(true);d.effectLocks.fill(true);auto c=d;c.values[756]=.5f;c.values[757]=0;
    CHECK(validateAiSoundEdit(d,c)); // New globals have no physical rack owner.
    d.controlLocks[756]=d.controlLocks[757]=true;CHECK(!validateAiSoundEdit(d,c));CHECK(validateSoundDocument(c));
    c=d;c.macros[0]={gain.stateKey,.2f};CHECK(!validateAiSoundEdit(d,c));CHECK(validateSoundDocument(c));
    d.macros[0]={gain.stateKey,.2f};c=d;c.macros[0]={};CHECK(!validateAiSoundEdit(d,c));
    c=d;c.macros[0].amount=.3f;CHECK(!validateAiSoundEdit(d,c));
    c=d;c.macros[0]={guard.stateKey,.2f};CHECK(!validateSoundDocument(c));
    d=SoundDocument{};put(d,"output.gain_db",0);d.macros[0]={gain.stateKey,.5f};d.values[1]=.5f;
    CHECK(makeRoutedPatch(d,patch));CHECK(near(patch.outputGainDb,15));
    for(const char* name:{"m2-analog-pluck.v2.soundstate","m2-compound-mono-modulated-tempo.v2.soundstate","m2-compound-mono-modulated-no-tempo.v2.soundstate"}){
        juce::MemoryBlock raw;CHECK(juce::File(RACKSYNTH_FIXTURE_DIR).getChildFile(name).loadFileAsData(raw));CHECK(raw.getSize()==7006);if(raw.getSize()!=7006)continue;
        SoundDocument migrated;CHECK(decodeSoundState(raw.getData(),raw.getSize(),migrated));
        CHECK(makeRoutedPatch(migrated,patch));CHECK(near(patch.outputGainDb,0)&&!patch.outputProtection);
        CHECK(!migrated.controlLocks[756]&&!migrated.controlLocks[757]);
        for(std::size_t i=0;i<756;++i){CHECK(read32(raw,16+8*i)==r[i].stateKey);CHECK(migrated.values[i]==std::bit_cast<float>(read32(raw,20+8*i)));CHECK(migrated.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[6206+i]!=0));}
        juce::MemoryBlock current;CHECK(encodeSoundState(migrated,current));CHECK(current.getSize()==currentSoundStateSize&&read32(current,4)==currentSoundStateVersion);
        // The complete legacy metadata block is preserved byte for byte in the current format.
        CHECK(std::memcmp(static_cast<const char*>(raw.getData())+6064,static_cast<const char*>(current.getData())+layoutOffset,78+64+756)==0);
        CHECK(std::memcmp(static_cast<const char*>(raw.getData())+6962,static_cast<const char*>(current.getData())+synthLockOffset,12)==0);
        auto reversed=raw;for(std::size_t i=0;i<756;++i)std::memcpy(static_cast<char*>(reversed.getData())+16+8*i,static_cast<const char*>(raw.getData())+16+8*(755-i),8);sign(reversed);
        SoundDocument out;CHECK(decodeSoundState(reversed.getData(),reversed.getSize(),out));CHECK(out==migrated);
        out.values[0]=.123f;const auto before=out;
        auto reject=[&](juce::MemoryBlock bad){sign(bad);CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out));CHECK(out==before);};
        for(auto key:{gain.stateKey,guard.stateKey}){auto bad=raw;write32(bad,16,key);reject(bad);bad=raw;write32(bad,6142,key);write32(bad,6146,std::bit_cast<std::uint32_t>(.2f));reject(bad);}
        for(auto [offset,value]:{std::pair<std::size_t,std::uint32_t>{4,3},{8,758},{12,1},{16,0},{16+8*755,read32(raw,16)}}){auto bad=raw;write32(bad,offset,value);reject(bad);}
        for(std::size_t n=0;n<7006;++n){CHECK(!decodeSoundState(raw.getData(),n,out));CHECK(out==before);}
        auto extra=raw;extra.append("x",1);CHECK(!decodeSoundState(extra.getData(),extra.getSize(),out));CHECK(out==before);
        for(std::size_t n=6206;n<6974;++n){auto bad=raw;static_cast<unsigned char*>(bad.getData())[n]=2;reject(bad);}
    }
}

}
SoundDocument twelvePersonalities()
{
    SoundDocument d;
    for(std::uint8_t n=1;n<4;++n)CHECK(editRackLayout(d.layout,{RackEditKind::addSynth,n,masterChain,n},d.layout)==RackEditResult::applied);
    for(std::uint8_t n=0;n<8;++n)CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,n,masterChain,n},d.layout)==RackEditResult::applied);
    auto state=std::make_shared<RackPersonalityState>();
    constexpr std::uint64_t revisions[]={9223372036854775807ULL,9223372036854775808ULL,18446744073709551615ULL};
    for(std::uint8_t n=0;n<12;++n){
        RackInstance i;i.edited=n%2!=0;i.design.name="Embedded "+juce::String(n);i.design.intent="Original native recipe";
        i.design.revision=revisions[n%3];i.design.role=n<4?RackRole::synth:RackRole::fx;i.design.panel.sections={"Tone"};
        i.design.units={{0,n<4?NativeModule::analog:NativeModule::drive,{{0,n<4?"analog.filter.cutoff":"drive.gain_db",.5f}}}};
        i.bindings={{0,std::uint8_t(n<4?n:n-4)}};
        if(n<4)i.design.panel.controls={{"Retained",0,0,"wavetable.osc.1.position",FocusWidget::rotary,FocusResponse::linear,0,1}};
        state->instances.push_back(i);
    }
    d.personalities=state;CHECK(validateSoundDocument(d));return d;
}
void nested_state_bounds()
{
    section("nested_state_bounds");const auto d=twelvePersonalities();juce::MemoryBlock wire;CHECK(encodeSoundState(d,wire));
    CHECK(wire.getSize()>currentSoundStateSize);CHECK(read32(wire,4)==currentSoundStateVersion);SoundDocument decoded;
    CHECK(decodeSoundState(wire.getData(),wire.getSize(),decoded));CHECK(decoded==d);
    if(wire.getSize()<=currentSoundStateSize)return;
    const auto reject=[&](juce::MemoryBlock b){auto out=d;CHECK(!decodeSoundState(b.getData(),b.getSize(),out));CHECK(out==d);};
    for(const auto size:{std::size_t(maximumSoundStateBytes),std::size_t(maximumSoundStateBytes+1)}){juce::MemoryBlock b(size,true);reject(b);}
    auto b=wire;b.setSize(wire.getSize()-1);reject(b);b=wire;static_cast<char*>(b.getData())[wire.getSize()-1]^=1;reject(b);
    for(const auto value:{0u,786917u,0xffffffffu}){b=wire;write32(b,currentPersonalityLengthOffset,value);sign(b);reject(b);}
    for(const auto offset:{std::size_t(currentPersonalityLengthOffset+4),std::size_t(currentPersonalityLengthOffset+7),std::size_t(currentPersonalityLengthOffset+24)}){b=wire;static_cast<unsigned char*>(b.getData())[offset]=255;sign(b);reject(b);}
    for(const auto count:{0,13}){b=wire;static_cast<unsigned char*>(b.getData())[currentPersonalityLengthOffset+6]=static_cast<unsigned char>(count);sign(b);reject(b);}
    b=wire;static_cast<unsigned char*>(b.getData())[currentPersonalityLengthOffset+25]=10;sign(b);reject(b);
    b=wire;static_cast<unsigned char*>(b.getData())[currentPersonalityLengthOffset+26]=9;sign(b);reject(b); // Unknown local unit.
    b=wire;write32(b,currentPersonalityLengthOffset+28,65537);sign(b);reject(b);
    const auto nextRecord=currentPersonalityLengthOffset+33+read32(wire,currentPersonalityLengthOffset+28);
    b=wire;std::memcpy(static_cast<char*>(b.getData())+nextRecord,static_cast<const char*>(wire.getData())+currentPersonalityLengthOffset+8,16);sign(b);reject(b);
    b=wire;b.setSize(b.getSize()+1);sign(b);reject(b);
    // Wire belongs to the very shared owner overwritten by a successful decode.
    struct Owner : RackPersonalityState {juce::MemoryBlock wire;};
    auto owner=std::make_shared<Owner>();owner->instances=d.personalities->instances;owner->wire=wire;auto alias=d;alias.personalities=owner;
    std::weak_ptr<Owner> weak=owner;const auto* input=owner->wire.getData();owner.reset();
    CHECK(decodeSoundState(input,wire.getSize(),alias));CHECK(alias==d);CHECK(weak.expired());
    owner=std::make_shared<Owner>();owner->instances=d.personalities->instances;owner->wire=b;alias.personalities=owner;input=owner->wire.getData();weak=owner;owner.reset();
    CHECK(!decodeSoundState(input,b.getSize(),alias));CHECK(alias==d);CHECK(!weak.expired());
    auto writable=std::make_shared<Owner>();writable->instances=d.personalities->instances;alias.personalities=writable;
    CHECK(encodeSoundState(alias,writable->wire));CHECK(writable->wire==wire);
    // Physically possible count-bound case: four single synths/eight single FX,
    // each valid recipe padded to the full allowed JSON byte length.
    juce::MemoryBlock nearCap;juce::MemoryOutputStream s(nearCap,false);s.write(wire.getData(),currentPersonalityLengthOffset);s.writeInt(4+12*(16+1+1+2+4+65536+1));
    s.writeShort(2);s.writeByte(12);s.writeByte(0);
    for(const auto& i:d.personalities->instances){juce::MemoryBlock design;CHECK(encodeRackDesign(i.design,design,RackValidationMode::instanceRecall));
        s.write(i.id.getRawData(),16);s.writeByte(i.edited?1:0);s.writeByte(1);s.writeByte(0);s.writeByte(static_cast<char>(i.bindings[0].slot));s.writeInt(65536);
        s.write(design.getData(),design.getSize());const std::string padding(65536-design.getSize(),' ');s.write(padding.data(),padding.size());s.writeByte(0);}
    s.writeInt(0);s.write(juce::MemoryBlock(32,true).getData(),32);s.flush();sign(nearCap);
    CHECK(nearCap.getSize()==maximumSoundStateBytes-192-12*172-maximumSoundNameBytes);CHECK(decodeSoundState(nearCap.getData(),nearCap.getSize(),decoded));CHECK(decoded==d);
    juce::MemoryBlock generic;CHECK(encodeSoundState(SoundDocument{},generic));CHECK(generic.getSize()==currentSoundStateSize);CHECK(read32(generic,4)==currentSoundStateVersion);
    CHECK(decodeSoundState(generic.getData(),generic.getSize(),decoded));CHECK(!decoded.personalities);juce::MemoryBlock again;CHECK(encodeSoundState(decoded,again));CHECK(again==generic);
}
// Exact captured native Sound5 input; independent of the current writer and ignored build directories.
juce::MemoryBlock capturedOldSound5()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwAAAAAQAEAggEAAABAQEBAQEB"
        "AQEBAgEBAQIBAQECAQEBAgEBAQIBAQECAQEBAgEBAQIBAAAAAJ8POhhDmBaGVEi5ZsXleCkCAFhanUdpbr5UqhybXVUR"));
    auto wire=out.getMemoryBlock();
    CHECK(wire.getSize()==7089);
    CHECK(juce::SHA256(wire).toHexString()=="83841f0d056855d71aa42d28089c5f84e3880b8cc7159d950918e3a0bbe545c0");
    return wire;
}
void compatibility_format_admission()
{
    section("legacy_kernel_and_mode_migration: appended mode and current format admission");
    const auto registry=parameterRegistry();
    CHECK(registry.size()==4519);
    if(registry.size()>758){const auto& quality=registry[758];CHECK(quality.id=="output.render_quality");CHECK(quality.kind==ParameterKind::choice);CHECK(quality.minimum==0&&quality.maximum==1&&quality.defaultValue==0);}
    juce::MemoryBlock legacyIdentity;juce::MemoryOutputStream identity(legacyIdentity,false);
    for(std::size_t i=0;i<758&&i<registry.size();++i){const auto& d=registry[i];identity.writeString(d.id);identity.writeString(d.name);identity.writeString(d.unit);identity.writeInt(int(d.kind));identity.writeFloat(d.minimum);identity.writeFloat(d.maximum);identity.writeFloat(d.defaultValue);identity.writeFloat(d.skew);identity.writeInt(std::bit_cast<std::int32_t>(d.stateKey));identity.writeInt(int(d.choices.size()));for(const auto& choice:d.choices)identity.writeString(choice);}
    identity.flush();CHECK(juce::SHA256(legacyIdentity).toHexString()=="d19c0a7b4b1ddc4833f7b67e95203db6caeffa7059a801c3d140972aaf3bf9f4");std::cout<<"LEGACY_758_DESCRIPTOR_SHA256 "<<juce::SHA256(legacyIdentity).toHexString()<<'\n';
    {const auto& d=registry[758];identity.writeString(d.id);identity.writeString(d.name);identity.writeString(d.unit);identity.writeInt(int(d.kind));identity.writeFloat(d.minimum);identity.writeFloat(d.maximum);identity.writeFloat(d.defaultValue);identity.writeFloat(d.skew);identity.writeInt(std::bit_cast<std::int32_t>(d.stateKey));identity.writeInt(int(d.choices.size()));for(const auto& choice:d.choices)identity.writeString(choice);identity.flush();}
    CHECK(juce::SHA256(legacyIdentity).toHexString()=="0c870368444ed93a353c013b7e2534145a156ed17c360090b042ba4449422a7e");
    juce::MemoryBlock wire;CHECK(encodeSoundState(SoundDocument{},wire));CHECK(read32(wire,4)==currentSoundStateVersion);
}
void algorithm_revision_transaction()
{
    section("algorithm_revision_transaction: every retained bank and raw rejection");
    static_assert(std::is_trivially_copyable_v<KernelCompatibility> && std::is_trivially_copyable_v<SoundAudioState>);
    SoundDocument original;
    CHECK(original.layout.synthUse[0]==SlotUse::present&&original.layout.synthCount==1);
    RoutedPatch accepted;CHECK(makeRoutedPatch(original,accepted));
    CHECK(original.values[758]==0&&accepted.compatibility==KernelCompatibility{});
    for(const auto& layer:accepted.synth.layers)CHECK(layer.algorithmRevision==1);
    for(const auto& effect:accepted.effects)CHECK(effect.algorithmRevision==1);
    auto higher=original;higher.values[758]=1;RoutedPatch selected;CHECK(makeRoutedPatch(higher,selected));CHECK(selected==accepted);
    higher.controlLocks[758]=true;juce::MemoryBlock encoded;CHECK(encodeSoundState(higher,encoded));
    SoundDocument recalled;CHECK(decodeSoundState(encoded.getData(),encoded.getSize(),recalled));CHECK(recalled==higher);
    auto blocked=higher;blocked.values[758]=0;CHECK(!validateAiSoundEdit(higher,blocked));
    const auto reject=[&](const SoundDocument& bad){
        auto patch=accepted;CHECK(!validateSoundDocument(bad));CHECK(!makeRoutedPatch(bad,patch));CHECK(patch==accepted);
        juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(bad,keep));CHECK(keep==juce::MemoryBlock("keep",4));
    };
    for(auto unsupported:{AlgorithmRevision(0),AlgorithmRevision(2),AlgorithmRevision(255)}){
        for(std::size_t s=0;s<4;++s)for(std::size_t b=0;b<2;++b){auto bad=original;bad.compatibility.synth[s][b]=unsupported==2?3:unsupported;reject(bad);}
        for(std::size_t s=0;s<8;++s)for(std::size_t b=0;b<effectBankCount;++b){auto bad=original;bad.compatibility.effects[s][b]=(b<=5||b==6||b==8||b==10||b==13)&&unsupported==2?3:unsupported;reject(bad);}
    }
    {
        auto modern=original;for(auto& banks:modern.compatibility.synth)banks.fill(2);
        RoutedPatch patch;CHECK(validateSoundDocument(modern)&&makeRoutedPatch(modern,patch));
        for(const auto& layer:patch.synth.layers)CHECK(layer.algorithmRevision==2);
        juce::MemoryBlock bytes;CHECK(encodeSoundState(modern,bytes));SoundDocument recalledModern;
        CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalledModern)&&recalledModern==modern);
    }
    for(std::size_t n=currentSoundNumericBytes;n<currentPersonalityLengthOffset;++n){
        auto bad=encoded;static_cast<unsigned char*>(bad.getData())[n]=255;sign(bad);
        auto out=higher;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out));CHECK(out==higher);
    }
    SynthEngine synth,control;CHECK(synth.prepare(48000,128));CHECK(control.prepare(48000,128));
    auto valid=accepted.synth;CHECK(synth.setPatch(valid));CHECK(control.setPatch(valid));
    juce::AudioBuffer<float> a(2,128),b(2,128);juce::MidiBuffer note,empty;note.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(90)),0);
    synth.render(a,note);control.render(b,note);
    auto invalid=valid;invalid.layers[3].algorithmRevision=3;CHECK(!validateSynthPatch(invalid));CHECK(!synth.setPatch(invalid));CHECK(synth.patch()==valid);
    synth.render(a,empty);control.render(b,empty);
    for(int ch=0;ch<2;++ch)for(int n=0;n<128;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));
    RoutedSynthEngine routed,routedControl;CHECK(routed.prepare(48000,128));CHECK(routedControl.prepare(48000,128));
    CHECK(routed.setPatch(accepted));CHECK(routedControl.setPatch(accepted));routed.render(a,note);routedControl.render(b,note);
    CHECK(a.getRMSLevel(0,0,128)>0&&a.getRMSLevel(1,0,128)>0);
    auto bad=accepted;bad.effects[7].algorithmRevision=2;CHECK(!routed.setPatch(bad));CHECK(routed.patch()==accepted);
    bad=accepted;bad.compatibility.effects[7][3]=3;CHECK(!routed.setPatch(bad));CHECK(routed.patch()==accepted);
    routed.render(a,empty);routedControl.render(b,empty);
    for(int ch=0;ch<2;++ch)for(int n=0;n<128;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));
    for(const char* name:{"m1-analog-pluck.soundstate","m1-wavetable-stab.soundstate","m2-analog-pluck.v2.soundstate","m2-compound-mono-modulated-tempo.v2.soundstate","m2-compound-mono-modulated-no-tempo.v2.soundstate","m2-calibrated-pluck.v3.soundstate"}){
        juce::MemoryBlock legacy;CHECK(juce::File(RACKSYNTH_FIXTURE_DIR).getChildFile(name).loadFileAsData(legacy));
        SoundDocument d;CHECK(decodeSoundState(legacy.getData(),legacy.getSize(),d));CHECK(d.values[758]==0&&d.compatibility==KernelCompatibility{});
        juce::MemoryBlock modern;CHECK(encodeSoundState(d,modern));CHECK(read32(modern,4)==currentSoundStateVersion);
        auto out=higher;CHECK(decodeSoundState(modern.getData(),modern.getSize(),out));CHECK(out==d);
    }
}
void modern_reverb_numeric_banks()
{
    section("modern_reverb_numeric_banks: Reverb2 only, active and inactive recall");
    SoundDocument modern;
    for(auto& bank:modern.compatibility.effects) bank[2]=2;
    CHECK(validateSoundDocument(modern));
    if(!validateSoundDocument(modern)) return;
    juce::MemoryBlock encoded; CHECK(encodeSoundState(modern,encoded));
    CHECK(read32(encoded,4)==currentSoundStateVersion);
    SoundDocument recalled; CHECK(decodeSoundState(encoded.getData(),encoded.getSize(),recalled)); CHECK(recalled==modern);
    for(std::size_t slot=0;slot<8;++slot) for(std::size_t kind=0;kind<effectBankCount;++kind) {
        auto bad=modern; bad.compatibility.effects[slot][kind]=kind<=5||kind==6||kind==8||kind==10||kind==13?3:2;
        CHECK(!validateSoundDocument(bad)); juce::MemoryBlock keep("keep",4);
        CHECK(!encodeSoundState(bad,keep)&&keep==juce::MemoryBlock("keep",4));
    }
}
void eq_catalog_and_retained_state()
{
    section("eq_catalog_and_retained_state: appended ordinary fields and all logical banks");
    constexpr Field fields[] {
        {"eq.low_shelf.frequency_hz",20,2000,160,.35f,0,"Hz"}, {"eq.low_shelf.gain_db",-18,18,0,1,0,"dB"}, {"eq.low_shelf.q",.25f,4,.70710678f,1,0,""},
        {"eq.bell.frequency_hz",20,20000,1000,.35f,0,"Hz"}, {"eq.bell.gain_db",-18,18,0,1,0,"dB"}, {"eq.bell.q",.25f,4,1,1,0,""},
        {"eq.high_shelf.frequency_hz",1000,20000,8000,.35f,0,"Hz"}, {"eq.high_shelf.gain_db",-18,18,0,1,0,"dB"}, {"eq.high_shelf.q",.25f,4,.70710678f,1,0,""}
    };
    CHECK(eqParameterStart==759&&eqParametersPerSlot==9&&filterParameterStart==831&&filterParametersPerSlot==5&&parameterCount==4519&&effectBankCount==19);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<9;++field){
        const auto index=759+9*slot+field;const auto& actual=registry[index];const auto& expected=fields[field];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id);
        CHECK(actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew&&actual.unit==expected.unit);
        CHECK(actual.kind==ParameterKind::continuous&&actual.choices.empty()&&isContinuousParameter(index));
        CHECK(rackParameterIndex(NativeModule::eq,slot,expected.id)==index&&rackParameterActive(NativeModule::eq,expected.id));
        CHECK(near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){
            SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));
            const auto& eq=patch.effects[slot].eq;const float mapped[]={eq.lowShelf.frequencyHz,eq.lowShelf.gainDb,eq.lowShelf.q,eq.bell.frequencyHz,eq.bell.gainDb,eq.bell.q,eq.highShelf.frequencyHz,eq.highShelf.gainDb,eq.highShelf.q};
            CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));
        }
    }
    SoundDocument d;
    for(std::uint8_t slot=0;slot<8;++slot){
        CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot%2?EffectKind::drive:EffectKind::eq},d.layout)==RackEditResult::applied);
        d.compatibility.effects[slot][2]=slot%2?1:2;
        for(std::size_t field=0;field<9;++field){const auto index=759+9*slot+field;d.values[index]=float(index-758)/73;d.controlLocks[index]=(slot+field)%2!=0;}
    }
    d.macros[0]={registry[759].stateKey,.23f};d.values[1]=.41f;d.controlLocks[759]=true;d.effectLocks[7]=true;
    auto metadata=std::make_shared<RackPersonalityState>();RackInstance instance;
    instance.design.name="Embedded retained EQ";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
    instance.design.units={{0,NativeModule::eq,{}}};instance.bindings={{0,0}};
    for(std::size_t field=0;field<9;++field)instance.design.units[0].defaults.push_back({0,fields[field].id,defaults[759+field]});
    instance.design.units[0].defaults.push_back({0,"reverb.mix",.32f});
    instance.design.panel.controls={{"Bell",0,0,"eq.bell.gain_db",FocusWidget::rotary,FocusResponse::linear,-18,18}};
    metadata->instances={instance};d.personalities=metadata;CHECK(validateSoundDocument(d));
    juce::MemoryBlock wire;CHECK(encodeSoundState(d,wire));CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
    SoundDocument recalled;CHECK(decodeSoundState(wire.getData(),wire.getSize(),recalled));CHECK(recalled==d);
    CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
    // Numeric live tweaks do not overwrite the embedded original defaults.
    CHECK(recalled.values[759]!=instance.design.units[0].defaults[0].normalized);
    for(std::size_t slot=0;slot<8;++slot)CHECK(recalled.compatibility.effects[slot]==d.compatibility.effects[slot]);
    auto reject=[&](juce::MemoryBlock bad){sign(bad);auto output=d;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),output));CHECK(output==d);};
    for(std::size_t slot=0;slot<8;++slot)for(unsigned char revision:{0,3,255}){
        auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+4]=revision;reject(bad);
        auto invalid=d;invalid.compatibility.effects[slot][4]=revision;juce::MemoryBlock keep("keep",4);
        CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));
    }
    for(const auto offset:{currentSoundNumericBytes+9,currentSoundNumericBytes+19}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[offset]=4;reject(bad);}
    auto wrongModule=d;auto changed=std::make_shared<RackPersonalityState>(*metadata);changed->instances[0].design.units[0].algorithmRevision=2;wrongModule.personalities=changed;
    CHECK(!validateSoundDocument(wrongModule));
    auto locked=d;locked.values[759]=.8f;CHECK(!validateAiSoundEdit(d,locked));
    locked=d;locked.values[759+7*9+8]=.8f;CHECK(!validateAiSoundEdit(d,locked));
}
void exact_old_sound5_migration_and_domain()
{
    section("exact_old_sound5_migration_and_domain: frozen bytes, explicit four-to-five rows");
    const auto raw=capturedOldSound5();const auto registry=parameterRegistry();SoundDocument migrated;
    CHECK(decodeSoundState(raw.getData(),raw.getSize(),migrated));
    for(std::size_t index=0;index<759;++index){
        CHECK(read32(raw,16+8*index)==registry[index].stateKey);
        CHECK(migrated.values[index]==std::bit_cast<float>(read32(raw,20+8*index)));
        CHECK(migrated.controlLocks[index]==(static_cast<const unsigned char*>(raw.getData())[6230+index]!=0));
    }
    const auto fresh=SoundDocument{};
    for(std::size_t index=759;index<parameterCount;++index)CHECK(migrated.values[index]==fresh.values[index]&&!migrated.controlLocks[index]);
    for(std::size_t slot=0;slot<8;++slot)CHECK((migrated.compatibility.effects[slot]==std::array<AlgorithmRevision,effectBankCount>{1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}));
    // A second valid old input varies retained revisions by row, exposing a flat expanded-stride copy.
    auto varied=raw;for(std::size_t slot=0;slot<8;++slot)static_cast<unsigned char*>(varied.getData())[7021+4*slot+2]=slot%2?1:2;sign(varied);
    SoundDocument rows;CHECK(decodeSoundState(varied.getData(),varied.getSize(),rows));
    for(std::size_t slot=0;slot<8;++slot){CHECK(rows.compatibility.effects[slot][2]==(slot%2?1:2));CHECK(rows.compatibility.effects[slot][0]==1&&rows.compatibility.effects[slot][1]==1&&rows.compatibility.effects[slot][3]==1&&rows.compatibility.effects[slot][4]==1);}
    juce::MemoryBlock current;CHECK(encodeSoundState(migrated,current));CHECK(read32(current,4)==currentSoundStateVersion&&current.getSize()==currentSoundStateSize);
    SoundDocument recalled;CHECK(decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==migrated);
    auto reject=[&](juce::MemoryBlock bad){sign(bad);auto output=migrated;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),output));CHECK(output==migrated);};
    for(const auto index:{std::size_t(759),std::size_t(830)}){
        auto bad=raw;write32(bad,16,registry[index].stateKey);reject(bad);
        bad=raw;write32(bad,6166,registry[index].stateKey);write32(bad,6170,std::bit_cast<std::uint32_t>(.2f));reject(bad);
    }
    for(const auto mutation:std::array<std::pair<std::size_t,unsigned char>,5>{{{6098,4},{7010,5},{7021,3},{7023,3},{7052,3}}}){
        auto bad=raw;static_cast<unsigned char*>(bad.getData())[mutation.first]=mutation.second;reject(bad);
    }
    // Re-signed old recall must still reject newly known retained suffixes/defaults/control bindings.
    RackInstance instance;instance.design.name="Old-domain retention";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};instance.design.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};instance.bindings={{0,0}};
    auto withPersonality=[&](const RackInstance& i){
        juce::MemoryBlock design;CHECK(encodeRackDesign(i.design,design,RackValidationMode::instanceRecall));
        juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(i.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),7053);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();
        auto* b=static_cast<unsigned char*>(result.getData());b[6097]=1;b[6098]=static_cast<unsigned char>(i.design.units[0].module==NativeModule::eq?EffectKind::eq:EffectKind::drive);b[6153]=0;b[6165]=1;sign(result);return result;
    };
    const auto validOld=withPersonality(instance);CHECK(decodeSoundState(validOld.getData(),validOld.getSize(),recalled));
    auto retained=instance;retained.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.6f});reject(withPersonality(retained));
    retained=instance;retained.design.panel.controls={{"Future retained",0,0,"eq.bell.gain_db",FocusWidget::rotary,FocusResponse::linear,-18,18}};reject(withPersonality(retained));
    retained=instance;retained.design.units[0].module=NativeModule::eq;retained.design.units[0].defaults={{0,"eq.bell.gain_db",.5f}};reject(withPersonality(retained));
    retained=instance;retained.design.units[0].defaults.clear();
    for(std::size_t index=572;index<595;++index)retained.design.units[0].defaults.push_back({0,registry[index].id.substring(9),fresh.values[index]});
    retained.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});CHECK(retained.design.units[0].defaults.size()==24);reject(withPersonality(retained));
    struct Owner:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Owner>();owner->wire=raw;auto* b=static_cast<unsigned char*>(owner->wire.getData());b[6098]=4;sign(owner->wire);auto alias=migrated;alias.personalities=owner;std::weak_ptr<Owner> weak=owner;const auto* data=owner->wire.getData();owner.reset();
    CHECK(!decodeSoundState(data,7089,alias)&&!weak.expired());CHECK(alias.personalities==weak.lock());
}

juce::MemoryBlock capturedOldSound6()
{
    // Actual accepted native831 export, not re-encoded from the current registry.
    const juce::String encoded=
        "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"
        "HU8MEUjh+j50cETrUrgeP/yEUvgK1yM9vfpcgHsULj5UAtDpmpmZPtIIchv2KNw+f8NboylcDz92fEr716MwPwEAAAAA////AQAA"
        "BAAABAAABAAABAAABAAABAAABAAABP////////////////////////////////////////////////////8AAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA4"
        "AAAAAQAEAggFAAABAQEBAQEBAQEBAgEBAQECAQEBAQIBAQEBAgEBAQECAQEBAQIBAQEBAgEBAQECAQEAAAAAMMQHiAGoAlLNBHTG"
        "OY+DgrnyRUj19DXA+RaHZIUuZUU=";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));
    auto wire=out.getMemoryBlock();CHECK(wire.getSize()==7745);
    CHECK(juce::SHA256(wire).toHexString()=="dbb4df56b15e393cdf7cbae07d2a43da8287476173673153e1bf7439b5976585");return wire;
}
void filter_state_and_old_sound6_authority()
{
    section("filter_state_and_old_sound6_authority: actual831 prefix, six logical banks and old domain");
    CHECK(filterParameterStart==831&&filterParametersPerSlot==5&&parameterCount==4519&&effectBankCount==19);
    CHECK(oldSound6ParameterCount==831&&oldSound6NumericBytes==7649&&oldSound6CompatibilityBytes==56&&oldSound6StateSize==7745&&oldSound6MaximumBytes==794661);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    // Same native serialization as the frozen old831 descriptor export.
    juce::Array<juce::var> rows;
    for(const auto& d:registry.first(831)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);
    CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="31ead8d1cf87eb74eedef172c1985c9f00d55d958504482e64cdd02f5186c880");
    constexpr Field fields[]{
        {"filter_fx.mode",0,2,0,1,3,""},{"filter_fx.cutoff_hz",20,20000,4000,.35f,0,"Hz"},
        {"filter_fx.resonance",.5f,10,.70710678f,1,0,"Q"},{"filter_fx.drive_db",0,24,0,1,0,"dB"},{"filter_fx.mix",0,1,1,1,0,""}
    };
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t f=0;f<5;++f){
        const auto index=831+5*slot+f;const auto& expected=fields[f];const auto& actual=registry[index];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id&&actual.stateKey==fnv(actual.id));
        CHECK(actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew&&actual.unit==expected.unit);
        CHECK(actual.kind==(f==0?ParameterKind::choice:ParameterKind::continuous)&&actual.choices.size()==std::size_t(expected.choices));
        CHECK(rackParameterIndex(NativeModule::filter,slot,expected.id)==index&&rackParameterActive(NativeModule::filter,expected.id));
        CHECK(near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){
            SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));
            const auto& filter=patch.effects[slot].filter;const float mapped[]{float(filter.mode),filter.cutoffHz,filter.resonance,filter.driveDb,filter.mix};
            CHECK(near(mapped[f],endpoint==0?expected.low:expected.high));
        }
    }
    SoundDocument modern;
    for(std::uint8_t slot=0;slot<8;++slot){
        CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot%2?EffectKind::drive:EffectKind::filter},modern.layout)==RackEditResult::applied);
        modern.compatibility.effects[slot][2]=slot%2?1:2;
        for(std::size_t f=0;f<5;++f){const auto i=831+5*slot+f;modern.values[i]=f==0?float(slot%3)/2:.1f+.07f*float((slot+f)%8);modern.controlLocks[i]=(slot+f)%3==0;}
    }
    modern.macros[0]={registry[832].stateKey,.17f};modern.values[1]=.3f;modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Filter immutable defaults";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
    instance.design.units={{0,NativeModule::filter,{}}};instance.bindings={{0,0}};
    for(std::size_t f=0;f<5;++f)instance.design.units[0].defaults.push_back({0,fields[f].id,defaults[831+f]});
    instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.6f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
    instance.design.panel.controls={{"Cutoff",0,0,"filter_fx.cutoff_hz",FocusWidget::rotary,FocusResponse::logarithmic,20,20000}};
    for(bool embedded:{false,true}){
        auto source=modern;if(embedded){auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.personalities=state;}
        const bool valid=validateSoundDocument(source);CHECK(valid);if(!valid)continue;juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
        SoundDocument recalled;CHECK(decodeSoundState(wire.getData(),wire.getSize(),recalled)&&recalled==source);
        if(embedded)CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
        auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out));CHECK(out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){
            auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+5]=revision;reject(bad);
            auto invalid=source;invalid.compatibility.effects[slot][5]=revision;juce::MemoryBlock keep("keep",4);
            CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));
        }
        auto mode=wire;write32(mode,20+8*831,std::bit_cast<std::uint32_t>(.25f));reject(mode);
        auto macro=wire;write32(macro,macroOffset,registry[831].stateKey);write32(macro,macroOffset+4,std::bit_cast<std::uint32_t>(.2f));reject(macro);
        if(embedded){auto invalid=source;auto changed=std::make_shared<RackPersonalityState>(*source.personalities);changed->instances[0].design.units[0].algorithmRevision=3;invalid.personalities=changed;CHECK(!validateSoundDocument(invalid));}
        auto edited=source;edited.values[831+7*5+4]=.93f;CHECK(!validateAiSoundEdit(source,edited));
    }
    const auto raw=capturedOldSound6();SoundDocument old;const bool oldDecoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(oldDecoded);if(!oldDecoded)return;
    for(std::size_t i=0;i<831;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey);CHECK(std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[6806+i]!=0));}
    for(std::size_t i=831;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<5;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[7669+5*slot+bank]);CHECK(old.compatibility.effects[slot][5]==1);}
    auto varied=raw;for(std::size_t slot=0;slot<8;++slot)static_cast<unsigned char*>(varied.getData())[7669+5*slot+2]=slot%2?1:2;sign(varied);SoundDocument migrated;
    CHECK(decodeSoundState(varied.getData(),varied.getSize(),migrated));for(std::size_t slot=0;slot<8;++slot)CHECK(migrated.compatibility.effects[slot][2]==(slot%2?1:2)&&migrated.compatibility.effects[slot][4]==1&&migrated.compatibility.effects[slot][5]==1);
    juce::MemoryBlock saved;const bool upgraded=encodeSoundState(old,saved);CHECK(upgraded);if(!upgraded)return;CHECK(read32(saved,4)==currentSoundStateVersion&&saved.getSize()==currentSoundStateSize);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out));CHECK(out==old);};
    for(std::size_t index:{std::size_t(831),std::size_t(870)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,6742,registry[index].stateKey);write32(bad,6746,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[6674]=5;rejectOld(bad);
    RackInstance eq;eq.design.name="Old6 retained EQ";eq.design.role=RackRole::fx;eq.design.panel.sections={"Tone"};eq.design.units={{0,NativeModule::eq,{{0,"eq.bell.gain_db",.5f}}}};eq.bindings={{0,0}};
    // Re-signed embedded cases use the actual captured numeric/revision prefix.
    const auto withPersonality=[&](const RackInstance& owner){
        juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));
        juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),7709);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());
        bytes[6673]=1;bytes[6674]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[6729]=0;bytes[6741]=1;sign(result);return result;
    };
    const auto embedded=withPersonality(eq);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled));CHECK(recalled.personalities&&recalled.personalities->instances[0].design==eq.design);
    auto retained=eq;retained.design.units[0].defaults.push_back({0,"filter_fx.mix",.5f});rejectOld(withPersonality(retained));
    retained=eq;retained.design.panel.controls={{"Future cutoff",0,0,"filter_fx.cutoff_hz",FocusWidget::rotary,FocusResponse::linear,20,20000}};rejectOld(withPersonality(retained));
    retained=eq;retained.design.units[0].module=NativeModule::filter;retained.design.units[0].defaults={{0,"filter_fx.mix",.5f}};rejectOld(withPersonality(retained));
    retained=eq;retained.design.units[0].defaults.clear();for(std::size_t i=0;i<registry.size();++i)if(registry[i].id.startsWith("effect.1.")&&i<831)retained.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});
    CHECK(retained.design.units[0].defaults.size()==32);const auto allOld=withPersonality(retained);CHECK(decodeSoundState(allOld.getData(),allOld.getSize(),recalled));
    retained.design.units[0].defaults.push_back({0,"filter_fx.mode",0});CHECK(retained.design.units[0].defaults.size()==33);rejectOld(withPersonality(retained));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[6674]=5;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();
    CHECK(!decodeSoundState(data,7745,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

// Actual accepted native871 bytes; independent of the current writer.
juce::MemoryBlock capturedOldSound7()
{
    const char* encoded=
        "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"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAABAAAAAAQAEAggGAAABAQEB"
        "AQEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQAAAABlRIQxu1veK5/QAVqPm4+6jcQS"
        "k3WFRIvVjSdaFggn7g==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();
    CHECK(wire.getSize()==8113&&juce::SHA256(wire).toHexString()=="a10734a80350ac97b6e4e1b070b629d21004a57dbf6c3e8b7252a4789380757a");return wire;
}
void chorus_state_and_old_sound7_authority()
{
    section("chorus_state_and_old_sound7_authority: native871 identity and logical six-to-seven rows");
    CHECK(chorusParameterStart==871&&chorusParametersPerSlot==5&&parameterCount==4519&&effectBankCount==19);
    CHECK(oldSound7ParameterCount==871&&oldSound7NumericBytes==8009&&oldSound7CompatibilityBytes==64&&oldSound7StateSize==8113&&oldSound7MaximumBytes==795029);
    CHECK(currentSoundNumericBytes==40841&&kernelCompatibilitySectionBytes==168&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    juce::Array<juce::var> rows;for(const auto& d:registry.first(871)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="fa2cc23c8ebbf163f70e17654cce353eeb1bfbca5e83e067aafd6172d9e353f8");
    constexpr Field fields[]{{"chorus.rate_hz",.1f,5,.35f,1,0,"Hz"},{"chorus.depth",0,1,.25f,1,0,""},{"chorus.base_delay_ms",12,40,18,1,0,"ms"},{"chorus.feedback",-.8f,.8f,.1f,1,0,""},{"chorus.mix",0,1,.25f,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field){const auto index=871+5*slot+field;const auto& expected=fields[field];const auto& actual=registry[index];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id&&actual.stateKey==fnv(actual.id));
        CHECK(actual.kind==ParameterKind::continuous&&actual.choices.empty()&&actual.unit==expected.unit&&actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew);
        CHECK(rackParameterIndex(NativeModule::chorus,slot,expected.id)==index&&rackParameterActive(NativeModule::chorus,expected.id)&&near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto& c=patch.effects[slot].chorus;const float mapped[]{c.rateHz,c.depth,c.baseDelayMs,c.feedback,c.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}
    }
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::chorus},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=2;
        for(std::size_t field=0;field<5;++field){const auto i=871+5*slot+field;modern.values[i]=.11f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[872].stateKey,.17f};modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Chorus immutable defaults";instance.design.role=RackRole::fx;instance.design.panel.sections={"Motion"};instance.design.units={{0,NativeModule::chorus,{}}};instance.bindings={{0,0}};
    for(std::size_t field=0;field<5;++field)instance.design.units[0].defaults.push_back({0,fields[field].id,defaults[871+field]});
    instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
    instance.design.panel.controls={{"Rate",0,0,"chorus.rate_hz",FocusWidget::rotary,FocusResponse::linear,.1f,5}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto metadata=std::make_shared<RackPersonalityState>();metadata->instances={instance};source.personalities=metadata;}
        const bool valid=validateSoundDocument(source);CHECK(valid);if(!valid)continue;juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
        SoundDocument recalled;const bool decoded=decodeSoundState(wire.getData(),wire.getSize(),recalled);CHECK(decoded&&recalled==source);if(embedded&&decoded)CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+6]=revision;reject(bad);
            auto invalid=source;invalid.compatibility.effects[slot][6]=revision;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        if(embedded){auto invalid=source;auto changed=std::make_shared<RackPersonalityState>(*source.personalities);changed->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=changed;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto edited=source;edited.values[910]=.93f;CHECK(!validateAiSoundEdit(source,edited));
    }
    const auto raw=capturedOldSound7();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    constexpr std::size_t oldLayout=16+8*871,oldMacro=oldLayout+78,oldLocks=oldMacro+64,oldBanks=8009+20,oldPersonality=8009+4+64;
    for(std::size_t i=0;i<871;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+i]!=0));}
    for(std::size_t i=871;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<6;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[oldBanks+6*slot+bank]);CHECK(old.compatibility.effects[slot][6]==1);}
    juce::MemoryBlock saved;const bool encoded=encodeSoundState(old,saved);CHECK(encoded);if(!encoded)return;CHECK(read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(871),std::size_t(910)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,oldMacro,registry[index].stateKey);write32(bad,oldMacro+4,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[oldLayout+10]=6;rejectOld(bad);bad=raw;write32(bad,8,911);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[8009+9]=7;rejectOld(bad);
    RackInstance filter;filter.design.name="Old7 retained Filter";filter.design.role=RackRole::fx;filter.design.panel.sections={"Tone"};filter.design.units={{0,NativeModule::filter,{{0,"filter_fx.mix",.5f}}}};filter.bindings={{0,0}};
    // Deliberately re-signed embedded forgeries reuse the actual native numeric prefix.
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),oldPersonality);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[oldLayout+9]=1;bytes[oldLayout+10]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[oldLayout+65]=0;bytes[oldLayout+77]=1;sign(result);return result;};
    const auto embedded=withPersonality(filter);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities&&recalled.personalities->instances[0].design==filter.design);
    auto forged=filter;forged.design.units[0].module=NativeModule::chorus;forged.design.units[0].defaults={{0,"chorus.mix",.5f}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.push_back({0,"chorus.mix",.5f});rejectOld(withPersonality(forged));
    forged=filter;forged.design.panel.controls={{"Rate",0,0,"chorus.rate_hz",FocusWidget::rotary,FocusResponse::linear,.1f,5}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.clear();for(std::size_t i=0;i<871;++i)if(registry[i].id.startsWith("effect.1."))forged.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});
    CHECK(forged.design.units[0].defaults.size()==37);const auto allOld=withPersonality(forged);CHECK(decodeSoundState(allOld.getData(),allOld.getSize(),recalled));forged.design.units[0].defaults.push_back({0,"chorus.mix",.25f});CHECK(forged.design.units[0].defaults.size()==38);rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[oldLayout+10]=6;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,8113,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

juce::MemoryBlock capturedOldSound8()
{
    // Exact native old Sound8 capture, including nonuniform inactive Chorus/Filter/EQ/Reverb2 banks.
    const juce::String encoded=
        "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"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAABIAAAAAQAEAggHAAABAQEBAQEBAQEBAgEBAQEBAQIB"
        "AQEBAQECAQEBAQEBAgEBAQEBAQIBAQEBAQECAQEBAQEBAgEBAQEBAQIBAQEBAAAAABeeYxRBjzSVNk+CloDHQ/D+ytzRc1wV+7n0"
        "aacrrgR5";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();
    CHECK(wire.getSize()==8481&&juce::SHA256(wire).toHexString()=="863fdbf7b0df3d5479c642b97946f1a514edde985dc054ad098e2f85ff475584");return wire;
}
void phaser_state_and_old_sound8_authority()
{
    section("phaser_state_and_old_sound8_authority: native911 identity and logical seven-to-eight rows");
    CHECK(phaserParameterStart==911&&phaserParametersPerSlot==5&&parameterCount==4519&&effectBankCount==19);
    CHECK(oldSound8ParameterCount==911&&oldSound8NumericBytes==8369&&oldSound8CompatibilityBytes==72&&oldSound8StateSize==8481&&oldSound8MaximumBytes==795397);
    CHECK(currentSoundNumericBytes==40841&&kernelCompatibilitySectionBytes==168&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    juce::Array<juce::var> rows;for(const auto& d:registry.first(911)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="51e57daf1b0ede215a282ad01bef18d22f4ce703473911fe0c295a45bf6a86d0");
    constexpr Field fields[]{{"phaser.rate_hz",.1f,5,.35f,1,0,"Hz"},{"phaser.depth",0,1,.6f,1,0,""},{"phaser.centre_hz",20,20000,1300,.35f,0,"Hz"},{"phaser.feedback",-.8f,.8f,.2f,1,0,""},{"phaser.mix",0,1,.5f,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field){const auto index=911+5*slot+field;const auto& expected=fields[field];const auto& actual=registry[index];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id&&actual.stateKey==fnv(actual.id));
        CHECK(actual.kind==ParameterKind::continuous&&actual.choices.empty()&&actual.unit==expected.unit&&actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew);
        CHECK(rackParameterIndex(NativeModule::phaser,slot,expected.id)==index&&rackParameterActive(NativeModule::phaser,expected.id)&&near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto& c=patch.effects[slot].phaser;const float mapped[]{c.rateHz,c.depth,c.centreHz,c.feedback,c.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}
    }
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::phaser},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=2;
        for(std::size_t field=0;field<5;++field){const auto i=911+5*slot+field;modern.values[i]=.11f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[912].stateKey,.17f};modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Phaser immutable defaults";instance.design.role=RackRole::fx;instance.design.panel.sections={"Motion"};instance.design.units={{0,NativeModule::phaser,{}}};instance.bindings={{0,0}};
    for(std::size_t field=0;field<5;++field)instance.design.units[0].defaults.push_back({0,fields[field].id,defaults[911+field]});
    instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
    instance.design.panel.controls={{"Rate",0,0,"phaser.rate_hz",FocusWidget::rotary,FocusResponse::linear,.1f,5}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto metadata=std::make_shared<RackPersonalityState>();metadata->instances={instance};source.personalities=metadata;}
        const bool valid=validateSoundDocument(source);CHECK(valid);if(!valid)continue;juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
        SoundDocument recalled;const bool decoded=decodeSoundState(wire.getData(),wire.getSize(),recalled);CHECK(decoded&&recalled==source);if(embedded&&decoded)CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,2,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+7]=revision;reject(bad);
            auto invalid=source;invalid.compatibility.effects[slot][7]=revision;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        if(embedded){auto invalid=source;auto changed=std::make_shared<RackPersonalityState>(*source.personalities);changed->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=changed;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto edited=source;edited.values[950]=.93f;CHECK(!validateAiSoundEdit(source,edited));
    }
    const auto raw=capturedOldSound8();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    constexpr std::size_t oldLayout=16+8*911,oldMacro=oldLayout+78,oldLocks=oldMacro+64,oldBanks=8369+20,oldPersonality=8369+4+72;
    for(std::size_t i=0;i<911;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+i]!=0));}
    for(std::size_t i=911;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,oldMacro+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,oldMacro+8*macro+4));
    for(std::size_t slot=0;slot<4;++slot)CHECK(old.synthLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+911+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot)CHECK(old.effectLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+911+4+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<7;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[oldBanks+7*slot+bank]);CHECK(old.compatibility.effects[slot][7]==1);}
    juce::MemoryBlock saved;const bool encoded=encodeSoundState(old,saved);CHECK(encoded);if(!encoded)return;CHECK(read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(911),std::size_t(950)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,oldMacro,registry[index].stateKey);write32(bad,oldMacro+4,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[oldLayout+10]=7;rejectOld(bad);bad=raw;write32(bad,8,951);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[8369+9]=8;rejectOld(bad);
    RackInstance filter;filter.design.name="Old8 retained Chorus";filter.design.role=RackRole::fx;filter.design.panel.sections={"Tone"};filter.design.units={{0,NativeModule::chorus,{{0,"chorus.mix",.5f}}}};filter.bindings={{0,0}};
    // Deliberately re-signed embedded forgeries reuse the actual native numeric prefix.
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),oldPersonality);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[oldLayout+9]=1;bytes[oldLayout+10]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[oldLayout+65]=0;bytes[oldLayout+77]=1;sign(result);return result;};
    const auto embedded=withPersonality(filter);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities&&recalled.personalities->instances[0].design==filter.design);
    auto forged=filter;forged.design.units[0].module=NativeModule::phaser;forged.design.units[0].defaults={{0,"phaser.mix",.5f}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.push_back({0,"phaser.mix",.5f});rejectOld(withPersonality(forged));
    forged=filter;forged.design.panel.controls={{"Rate",0,0,"phaser.rate_hz",FocusWidget::rotary,FocusResponse::linear,.1f,5}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.clear();for(std::size_t i=0;i<911;++i)if(registry[i].id.startsWith("effect.1."))forged.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});
    CHECK(forged.design.units[0].defaults.size()==42);const auto allOld=withPersonality(forged);CHECK(decodeSoundState(allOld.getData(),allOld.getSize(),recalled));forged.design.units[0].defaults.push_back({0,"phaser.mix",.25f});CHECK(forged.design.units[0].defaults.size()==43);rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[oldLayout+10]=7;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,8481,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

juce::MemoryBlock capturedOldSound9()
{
    // Exact pre-append native Sound9 bytes, including inactive retained banks.
    constexpr const char* encoded=
        "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"
        "DBXlVtej8D4pqRz1mpkZP1zWbg0K16M8eLTTppqZGT44HlemKVyPPttWibCF69E+xDdttXE9Cj8v2RoMH4UrPwEAAAAA////AQAABAAABAAABAAA"
        "BAAABAAABAAABAAABP////////////////////////////////////////////////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABQAAAAAQAE"
        "AggIAAABAQEBAQEBAQEBAgEBAQEBAQECAQEBAQEBAQIBAQEBAQEBAgEBAQEBAQECAQEBAQEBAQIBAQEBAQEBAgEBAQEBAQECAQEBAQEAAAAA4vnT"
        "e+dtdzGAGuVA2MkztyIkoYrS44d6efp3g8GX6fE=";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();
    CHECK(wire.getSize()==8849&&juce::SHA256(wire).toHexString()=="8841e71897cc3ac43fd031095fe21b5a688976a0a300e10b5dede89934447ffb");return wire;
}
void flanger_state_and_old_sound9_authority()
{
    section("flanger_state_and_old_sound9_authority: native951 identity and logical eight-to-nine rows");
    CHECK(flangerParameterStart==951&&flangerParametersPerSlot==5&&parameterCount==4519&&effectBankCount==19);
    CHECK(oldSound9ParameterCount==951&&oldSound9NumericBytes==8729&&oldSound9CompatibilityBytes==80&&oldSound9StateSize==8849&&oldSound9MaximumBytes==795765);
    CHECK(currentSoundNumericBytes==40841&&kernelCompatibilitySectionBytes==168&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    juce::Array<juce::var> rows;for(const auto& d:registry.first(951)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="028e189218ddc1e2d8adca11b959ecc91a327105bf4b5da6c67dff879125f322");
    constexpr Field fields[]{{"flanger.rate_hz",.05f,5,.25f,1,0,"Hz"},{"flanger.depth",0,1,.6f,1,0,""},{"flanger.base_delay_ms",1,8,2,1,0,"ms"},{"flanger.feedback",-.8f,.8f,.35f,1,0,""},{"flanger.mix",0,1,.5f,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field){const auto index=951+5*slot+field;const auto& expected=fields[field];const auto& actual=registry[index];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id&&actual.stateKey==fnv(actual.id));
        CHECK(actual.kind==ParameterKind::continuous&&actual.choices.empty()&&actual.unit==expected.unit&&actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew);
        CHECK(rackParameterIndex(NativeModule::flanger,slot,expected.id)==index&&rackParameterActive(NativeModule::flanger,expected.id)&&near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto& c=patch.effects[slot].flanger;const float mapped[]{c.rateHz,c.depth,c.baseDelayMs,c.feedback,c.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}
    }
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::flanger},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=2;
        for(std::size_t field=0;field<5;++field){const auto i=951+5*slot+field;modern.values[i]=.11f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[952].stateKey,.17f};modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Flanger immutable defaults";instance.design.role=RackRole::fx;instance.design.panel.sections={"Motion"};instance.design.units={{0,NativeModule::flanger,{}}};instance.bindings={{0,0}};
    for(std::size_t field=0;field<5;++field)instance.design.units[0].defaults.push_back({0,fields[field].id,defaults[951+field]});
    instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
    instance.design.panel.controls={{"Rate",0,0,"flanger.rate_hz",FocusWidget::rotary,FocusResponse::linear,.05f,5}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto metadata=std::make_shared<RackPersonalityState>();metadata->instances={instance};source.personalities=metadata;}
        const bool valid=validateSoundDocument(source);CHECK(valid);if(!valid)continue;juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
        SoundDocument recalled;const bool decoded=decodeSoundState(wire.getData(),wire.getSize(),recalled);CHECK(decoded&&recalled==source);if(embedded&&decoded)CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+8]=revision;reject(bad);
            auto invalid=source;invalid.compatibility.effects[slot][8]=revision;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        if(embedded){auto invalid=source;auto changed=std::make_shared<RackPersonalityState>(*source.personalities);changed->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=changed;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto edited=source;edited.values[990]=.93f;CHECK(!validateAiSoundEdit(source,edited));
    }
    const auto raw=capturedOldSound9();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    constexpr std::size_t oldLayout=16+8*951,oldMacro=oldLayout+78,oldLocks=oldMacro+64,oldBanks=8729+20,oldPersonality=8729+4+80;
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[8729+12+2*slot+bank]);
    for(std::size_t i=0;i<951;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+i]!=0));}
    for(std::size_t i=951;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,oldMacro+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,oldMacro+8*macro+4));
    for(std::size_t slot=0;slot<4;++slot)CHECK(old.synthLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+951+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot)CHECK(old.effectLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+951+4+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<8;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[oldBanks+8*slot+bank]);CHECK(old.compatibility.effects[slot][8]==1);}
    juce::MemoryBlock saved;const bool encoded=encodeSoundState(old,saved);CHECK(encoded);if(!encoded)return;CHECK(read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(951),std::size_t(990)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,oldMacro,registry[index].stateKey);write32(bad,oldMacro+4,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[oldLayout+10]=8;rejectOld(bad);bad=raw;write32(bad,8,991);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[8729+9]=9;rejectOld(bad);
    RackInstance filter;filter.design.name="Old9 retained Phaser";filter.design.role=RackRole::fx;filter.design.panel.sections={"Tone"};filter.design.units={{0,NativeModule::phaser,{{0,"phaser.mix",.5f}}}};filter.bindings={{0,0}};
    // Deliberately re-signed embedded forgeries reuse the actual native numeric prefix.
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),oldPersonality);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[oldLayout+9]=1;bytes[oldLayout+10]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[oldLayout+65]=0;bytes[oldLayout+77]=1;sign(result);return result;};
    const auto embedded=withPersonality(filter);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities&&recalled.personalities->instances[0].design==filter.design);
    auto forged=filter;forged.design.units[0].module=NativeModule::flanger;forged.design.units[0].defaults={{0,"flanger.mix",.5f}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.push_back({0,"flanger.mix",.5f});rejectOld(withPersonality(forged));
    forged=filter;forged.design.panel.controls={{"Rate",0,0,"flanger.rate_hz",FocusWidget::rotary,FocusResponse::linear,.05f,5}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.clear();for(std::size_t i=0;i<951;++i)if(registry[i].id.startsWith("effect.1."))forged.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});
    CHECK(forged.design.units[0].defaults.size()==47);const auto allOld=withPersonality(forged);CHECK(decodeSoundState(allOld.getData(),allOld.getSize(),recalled));forged.design.units[0].defaults.push_back({0,"flanger.mix",.25f});CHECK(forged.design.units[0].defaults.size()==48);rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[oldLayout+10]=8;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,8849,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

juce::MemoryBlock capturedOldSound10()
{
    const juce::String encoded=
        "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"
        "DBXlVtej8D4pqRz1mpkZP1zWbg0K16M8eLTTppqZGT44HlemKVyPPttWibCF69E+xDdttXE9Cj8v2RoMH4UrP+VCIC/sUbg9ReSRla5HYT5S7avQ"
        "MzOzPnepKDGPwvU+RkPyPvYoHD+A/em2j8L1PEDdoNcK1yM+20pm6+F6lD68K3pMPQrXPpe22rXNzAw/g6FhyHsULj+/81N+zczMPcAw3V8fhWs+"
        "XfQyo+xRuD4woTD1SOH6Pj45znFSuB4/musUZgrXIz1hchOYexQuPtIzAAuamZk+Mc2vafYo3D6pnzVOKVwPPwncI1jXozA/Jt7R4q5H4T07oYlK"
        "j8J1PsqLNcykcL0+VA7ACAAAAD/UYA2MrkchP09VUtLNzEw9IPdH+uxROD475U70UriePsfgUKKuR+E+Myzx04XrET+07PEgMzMzP4Hh6nWPwvU9"
        "BKJrhwAAgD4SF/NpXI/CPj6hMqVcjwI/RTM8+grXIz9W3Tsej8J1PVXDMZhcj0I+AQAAAAD///8BAAAEAAAEAAAEAAAEAAAEAAAEAAAEAAAE////"
        "/////////////////////////////////////////////////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAABYAAAAAQAEAggJAAABAQEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEB"
        "AgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQAAAADRpihiyrX1U/ex/SU/mfpg4zyVFWi89rYbRNZU4Tywyw==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();
    CHECK(wire.getSize()==9217&&juce::SHA256(wire).toHexString()=="785c4bb7d42bdaf55b16b6ccb3dd77c7fcbb2d03c67ab8090f7a719db52f757b");return wire;
}
void stereo_state_and_old_sound10_authority()
{
    section("stereo_state_and_old_sound10_authority: native991 identity and logical nine-to-ten rows");
    CHECK(stereoParameterStart==991&&stereoParametersPerSlot==4&&parameterCount==4519&&effectBankCount==19);
    CHECK(oldSound10ParameterCount==991&&oldSound10NumericBytes==9089&&oldSound10CompatibilityBytes==88&&oldSound10StateSize==9217&&oldSound10MaximumBytes==796133);
    CHECK(currentSoundNumericBytes==40841&&kernelCompatibilitySectionBytes==168&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    juce::Array<juce::var> rows;for(const auto& d:registry.first(991)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="d4c520ffc4a7f1e81f761c263308fcc4441c38592ea3cb589bf609ae36b08399");
    constexpr Field fields[]{{"stereo.gain_db",-24,12,0,1,0,"dB"},{"stereo.pan",-1,1,0,1,0,""},{"stereo.width",0,2,1,1,0,""},{"stereo.channel_mode",0,2,0,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<4;++field){const auto index=991+4*slot+field;const auto& expected=fields[field];const auto& actual=registry[index];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id&&actual.stateKey==fnv(actual.id));
        if(field==3)CHECK((actual.choices==std::vector<juce::String>{"Stereo","Swap","Mono"}));
        CHECK(actual.kind==(field==3?ParameterKind::choice:ParameterKind::continuous)&&(field==3?actual.choices.size()==3:actual.choices.empty())&&actual.unit==expected.unit&&actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew);
        CHECK(rackParameterIndex(NativeModule::stereo,slot,expected.id)==index&&rackParameterActive(NativeModule::stereo,expected.id)&&near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto& c=patch.effects[slot].stereo;const float mapped[]{c.gainDb,c.pan,c.width,float(c.channelMode)};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}
    }
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::stereo},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=2;
        for(std::size_t field=0;field<4;++field){const auto i=991+4*slot+field;modern.values[i]=field==3?float(slot%3)/2:.11f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[992].stateKey,.17f};modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Stereo immutable defaults";instance.design.role=RackRole::fx;instance.design.panel.sections={"Motion"};instance.design.units={{0,NativeModule::stereo,{}}};instance.bindings={{0,0}};
    for(std::size_t field=0;field<4;++field)instance.design.units[0].defaults.push_back({0,fields[field].id,defaults[991+field]});
    instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
    instance.design.panel.controls={{"Rate",0,0,"stereo.gain_db",FocusWidget::rotary,FocusResponse::linear,-24,12}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto metadata=std::make_shared<RackPersonalityState>();metadata->instances={instance};source.personalities=metadata;}
        const bool valid=validateSoundDocument(source);CHECK(valid);if(!valid)continue;juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
        SoundDocument recalled;const bool decoded=decodeSoundState(wire.getData(),wire.getSize(),recalled);CHECK(decoded&&recalled==source);if(embedded&&decoded)CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,2,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+9]=revision;reject(bad);
            auto invalid=source;invalid.compatibility.effects[slot][9]=revision;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        if(embedded){auto invalid=source;auto changed=std::make_shared<RackPersonalityState>(*source.personalities);changed->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=changed;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto edited=source;edited.values[1022]=.93f;CHECK(!validateAiSoundEdit(source,edited));
    }
    const auto raw=capturedOldSound10();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    constexpr std::size_t oldLayout=16+8*991,oldMacro=oldLayout+78,oldLocks=oldMacro+64,oldBanks=9089+20,oldPersonality=9089+4+88;
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[9089+12+2*slot+bank]);
    for(std::size_t i=0;i<991;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+i]!=0));}
    for(std::size_t i=991;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,oldMacro+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,oldMacro+8*macro+4));
    for(std::size_t slot=0;slot<4;++slot)CHECK(old.synthLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+991+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot)CHECK(old.effectLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+991+4+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<9;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[oldBanks+9*slot+bank]);CHECK(old.compatibility.effects[slot][9]==1);}
    juce::MemoryBlock saved;const bool encoded=encodeSoundState(old,saved);CHECK(encoded);if(!encoded)return;CHECK(read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(991),std::size_t(1022)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,oldMacro,registry[index].stateKey);write32(bad,oldMacro+4,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[oldLayout+10]=9;rejectOld(bad);bad=raw;write32(bad,8,1023);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[9089+9]=10;rejectOld(bad);
    RackInstance filter;filter.design.name="Old10 retained Flanger";filter.design.role=RackRole::fx;filter.design.panel.sections={"Tone"};filter.design.units={{0,NativeModule::flanger,{{0,"flanger.mix",.5f}}}};filter.bindings={{0,0}};
    // Deliberately re-signed embedded forgeries reuse the actual native numeric prefix.
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),oldPersonality);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[oldLayout+9]=1;bytes[oldLayout+10]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[oldLayout+65]=0;bytes[oldLayout+77]=1;sign(result);return result;};
    const auto embedded=withPersonality(filter);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities&&recalled.personalities->instances[0].design==filter.design);
    auto forged=filter;forged.design.units[0].module=NativeModule::stereo;forged.design.units[0].defaults={{0,"stereo.channel_mode",.5f}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.push_back({0,"stereo.channel_mode",.5f});rejectOld(withPersonality(forged));
    forged=filter;forged.design.panel.controls={{"Rate",0,0,"stereo.gain_db",FocusWidget::rotary,FocusResponse::linear,-24,12}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.clear();for(std::size_t i=0;i<991;++i)if(registry[i].id.startsWith("effect.1."))forged.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});
    CHECK(forged.design.units[0].defaults.size()==52);const auto allOld=withPersonality(forged);CHECK(decodeSoundState(allOld.getData(),allOld.getSize(),recalled));forged.design.units[0].defaults.push_back({0,"stereo.channel_mode",.5f});CHECK(forged.design.units[0].defaults.size()==53);rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[oldLayout+10]=9;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,9217,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

juce::MemoryBlock capturedDimensionOldSound11()
{
    // Immutable actual predecessor export, independent of the current writer.
    constexpr const char* encoded=
        "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"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABgAAAAAQAEAggKAAABAQEBAQEBAQEBAgEBAQEBAQEB"
        "AQIBAQEBAQEBAQECAQEBAQEBAQEBAgEBAQEBAQEBAQIBAQEBAQEBAQECAQEBAQEBAQEBAgEBAQEBAQEBAQIBAQEBAQEBAAAAAPwIvDli5DDtPaPN"
        "rVH3mFLFqaCWoipRFL96FrPfPn73";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==9513&&juce::SHA256(wire).toHexString()=="07e03ad18e85b60778eca8e954387a7110fcd6340097029cdeec719bd3da5a6c");return wire;
}

void dimension_state_and_old_sound11_authority()
{
    section("dimension_state_and_old_sound11_authority: native1023 identity and logical ten-to-eleven rows");
    CHECK(dimensionParameterStart==1023&&dimensionParametersPerSlot==4&&parameterCount==4519&&effectBankCount==19);
    CHECK(oldSound11ParameterCount==1023&&oldSound11NumericBytes==9377&&oldSound11CompatibilityBytes==96&&oldSound11StateSize==9513&&oldSound11MaximumBytes==796429);
    CHECK(currentSoundNumericBytes==40841&&kernelCompatibilitySectionBytes==168&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    juce::Array<juce::var> rows;for(const auto& d:registry.first(1023)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="c28052fa7893956a2fd96f7bd0bbb0fff60af3bdc915f21101447e35b8e6582a");
    constexpr Field fields[]{{"dimension.depth",0,1,.6f,1,0,""},{"dimension.rate_hz",.05f,2,.35f,1,0,"Hz"},{"dimension.width",0,2,1,1,0,""},{"dimension.mix",0,1,.5f,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<4;++field){const auto index=1023+4*slot+field;const auto& expected=fields[field];const auto& actual=registry[index];
        CHECK(actual.id=="effect."+juce::String(slot+1)+"."+expected.id&&actual.stateKey==fnv(actual.id));
        CHECK(actual.kind==ParameterKind::continuous&&actual.choices.empty()&&actual.unit==expected.unit&&actual.minimum==expected.low&&actual.maximum==expected.high&&actual.defaultValue==expected.initial&&actual.skew==expected.skew);
        CHECK(rackParameterIndex(NativeModule::dimension,slot,expected.id)==index&&rackParameterActive(NativeModule::dimension,expected.id)&&near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){SoundDocument d;d.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto& c=patch.effects[slot].dimension;const float mapped[]{c.depth,c.rateHz,c.width,c.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}
    }
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::dimension},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=2;
        for(std::size_t field=0;field<4;++field){const auto i=1023+4*slot+field;modern.values[i]=.11f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[1024].stateKey,.17f};modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Dimension immutable defaults";instance.design.role=RackRole::fx;instance.design.panel.sections={"Motion"};instance.design.units={{0,NativeModule::dimension,{}}};instance.bindings={{0,0}};
    for(std::size_t field=0;field<4;++field)instance.design.units[0].defaults.push_back({0,fields[field].id,defaults[1023+field]});
    instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
    instance.design.panel.controls={{"Rate",0,0,"dimension.depth",FocusWidget::rotary,FocusResponse::linear,0,1}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto metadata=std::make_shared<RackPersonalityState>();metadata->instances={instance};source.personalities=metadata;}
        const bool valid=validateSoundDocument(source);CHECK(valid);if(!valid)continue;juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);
        SoundDocument recalled;const bool decoded=decodeSoundState(wire.getData(),wire.getSize(),recalled);CHECK(decoded&&recalled==source);if(embedded&&decoded)CHECK(recalled.personalities&&recalled.personalities->instances[0].design==instance.design);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+10]=revision;reject(bad);
            auto invalid=source;invalid.compatibility.effects[slot][10]=revision;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        if(embedded){auto invalid=source;auto changed=std::make_shared<RackPersonalityState>(*source.personalities);changed->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=changed;juce::MemoryBlock keep("keep",4);CHECK(!validateSoundDocument(invalid)&&!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto edited=source;edited.values[1053]=.93f;CHECK(!validateAiSoundEdit(source,edited));
    }
    const auto raw=capturedDimensionOldSound11();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    constexpr std::size_t oldLayout=16+8*1023,oldMacro=oldLayout+78,oldLocks=oldMacro+64,oldBanks=9377+20,oldPersonality=9377+4+96;
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[9377+12+2*slot+bank]);
    for(std::size_t i=0;i<1023;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+i]!=0));}
    for(std::size_t i=1023;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,oldMacro+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,oldMacro+8*macro+4));
    for(std::size_t slot=0;slot<4;++slot)CHECK(old.synthLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+1023+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot)CHECK(old.effectLocks[slot]==(static_cast<const unsigned char*>(raw.getData())[oldLocks+1023+4+slot]!=0));
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<10;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[oldBanks+10*slot+bank]);CHECK(old.compatibility.effects[slot][10]==1);}
    juce::MemoryBlock saved;const bool encoded=encodeSoundState(old,saved);CHECK(encoded);if(!encoded)return;CHECK(read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(1023),std::size_t(1054)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,oldMacro,registry[index].stateKey);write32(bad,oldMacro+4,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[oldLayout+10]=10;rejectOld(bad);bad=raw;write32(bad,8,1055);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[9377+9]=11;rejectOld(bad);
    RackInstance filter;filter.design.name="Old11 retained Stereo";filter.design.role=RackRole::fx;filter.design.panel.sections={"Tone"};filter.design.units={{0,NativeModule::stereo,{{0,"stereo.width",.5f}}}};filter.bindings={{0,0}};
    // Deliberately re-signed embedded forgeries reuse the actual native numeric prefix.
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());
        juce::MemoryOutputStream out;out.write(raw.getData(),oldPersonality);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[oldLayout+9]=1;bytes[oldLayout+10]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[oldLayout+65]=0;bytes[oldLayout+77]=1;sign(result);return result;};
    const auto embedded=withPersonality(filter);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities&&recalled.personalities->instances[0].design==filter.design);
    auto forged=filter;forged.design.units[0].module=NativeModule::dimension;forged.design.units[0].defaults={{0,"dimension.mix",.5f}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.push_back({0,"dimension.mix",.5f});rejectOld(withPersonality(forged));
    forged=filter;forged.design.panel.controls={{"Rate",0,0,"dimension.depth",FocusWidget::rotary,FocusResponse::linear,0,1}};rejectOld(withPersonality(forged));
    forged=filter;forged.design.units[0].defaults.clear();for(std::size_t i=0;i<1023;++i)if(registry[i].id.startsWith("effect.1."))forged.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});
    CHECK(forged.design.units[0].defaults.size()==56);const auto allOld=withPersonality(forged);CHECK(decodeSoundState(allOld.getData(),allOld.getSize(),recalled));forged.design.units[0].defaults.push_back({0,"dimension.mix",.5f});CHECK(forged.design.units[0].defaults.size()==57);rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[oldLayout+10]=10;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,9513,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

juce::MemoryBlock capturedFrequencyShiftOldSound12()
{
    constexpr const char* encoded=
        "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"
        "rsfqZ65HIT+/EG3CzcxMPQHl/M3sUTg+m9HssFK4nj7PpIfNrkfhPp6TkbyF6xE/LOWuRzMzMz8BAAAAAP///wEAAAQAAAQAAAQAAAQAAAQAAAQA"
        "AAQAAAT/////////////////////////////////////////////////////AAAAAAC8zYMBMzMzvzxevF/MzAy/JSrr1czMzL4jMCo7/v9/vpVU"
        "e8HIzMy99RDw59DMTD18DG1B0MxMPvbKzHE2M7M+AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAABoAAAAAQAEAggLAAABAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQIB"
        "AQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEAAAAAWZmE4+jH7i3pTqxlbSNKF8ROg2LUIHZIDJVPwnl+x2o="
        ;
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto result=out.getMemoryBlock();CHECK(result.getSize()==9809&&juce::SHA256(result).toHexString()=="6ca035de2b3a535ad417cffab3a0ad9aa7aa3b54bcb682ad3f695b734c758fc6");return result;
}
void frequency_shift_state_and_old_sound12_authority()
{
    section("frequency_shift_state_and_old_sound12_authority");const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    CHECK(parameterCount==4519&&frequencyShiftParameterStart==1055&&frequencyShiftParametersPerSlot==3&&effectBankCount==19);
    CHECK(oldSound12ParameterCount==1055&&oldSound12NumericBytes==9665&&oldSound12StateSize==9809&&oldSound12CompatibilityBytes==104&&oldSound12MaximumBytes==796725);
    juce::Array<juce::var> rows;for(const auto& d:registry.first(1055)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="ecf6eea82e5f156a874dfa01a67e29aa48f0ec6dd08df8c816d42a5a597d4d18");
    constexpr Field fields[]{{"frequency_shift.hz",-1000,1000,7,1,0,"Hz"},{"frequency_shift.stereo_offset_hz",-100,100,0,1,0,"Hz"},{"frequency_shift.mix",0,1,.25f,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<3;++field){const auto index=1055+3*slot+field;const auto& expected=fields[field];const auto& d=registry[index];CHECK(d.id=="effect."+juce::String(slot+1)+"."+expected.id&&d.stateKey==fnv(d.id));CHECK(d.unit==expected.unit&&d.kind==ParameterKind::continuous&&d.choices.empty()&&d.minimum==expected.low&&d.maximum==expected.high&&d.defaultValue==expected.initial&&d.skew==1);
        CHECK(rackParameterIndex(NativeModule::frequencyShift,slot,expected.id)==index&&near(parameterPhysicalValue(defaults,index),expected.initial));for(float endpoint:{0.f,1.f}){SoundDocument source;source.values[index]=endpoint;RoutedPatch p;CHECK(makeRoutedPatch(source,p));const float mapped[]{p.effects[slot].frequencyShift.hz,p.effects[slot].frequencyShift.stereoOffsetHz,p.effects[slot].frequencyShift.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}}
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::frequencyShift},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=1+slot%2;for(std::size_t field=0;field<3;++field){const auto i=1055+3*slot+field;modern.values[i]=.13f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[1055].stateKey,.17f};modern.effectLocks[7]=true;
    RackInstance instance;instance.design.name="Frequency shift retained63";instance.design.role=RackRole::fx;instance.design.panel.sections={"Translation"};instance.design.units={{0,NativeModule::frequencyShift,{}}};instance.bindings={{0,0}};
    for(std::size_t i=0;i<dynamicEqParameterStart;++i)if(registry[i].id.startsWith("effect.1."))instance.design.units[0].defaults.push_back({0,registry[i].id.substring(9),modern.values[i]});CHECK(instance.design.units[0].defaults.size()==82);
    instance.design.panel.controls={{"Shift",0,0,"frequency_shift.hz",FocusWidget::rotary,FocusResponse::linear,-1000,1000}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.personalities=state;}CHECK(validateSoundDocument(source));juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);SoundDocument out;CHECK(decodeSoundState(wire.getData(),wire.getSize(),out)&&out==source);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,2,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+11]=revision;reject(bad);auto invalid=source;invalid.compatibility.effects[slot][11]=revision;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        if(embedded){auto invalid=source;auto state=std::make_shared<RackPersonalityState>(*source.personalities);state->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=state;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
    }
    const auto raw=capturedFrequencyShiftOldSound12();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    for(std::size_t i=0;i<1055;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[8598+i]!=0));}
    for(std::size_t i=1055;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<11;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[9685+11*slot+bank]);CHECK(old.compatibility.effects[slot][11]==1);}
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[9677+2*slot+bank]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,8534+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,8538+8*macro));
    juce::MemoryBlock saved;CHECK(encodeSoundState(old,saved)&&read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(1055),std::size_t(1078)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,8534,registry[index].stateKey);write32(bad,8538,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[8466]=11;rejectOld(bad);bad=raw;write32(bad,8,1079);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[9674]=12;rejectOld(bad);
    RackInstance retained;retained.design.name="Old12 retained Dimension60";retained.design.role=RackRole::fx;retained.design.panel.sections={"Motion"};retained.design.units={{0,NativeModule::dimension,{}}};retained.bindings={{0,0}};
    for(std::size_t i=0;i<1055;++i)if(registry[i].id.startsWith("effect.1."))retained.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});CHECK(retained.design.units[0].defaults.size()==60);
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());juce::MemoryOutputStream out;out.write(raw.getData(),9773);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[8465]=1;bytes[8466]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[8521]=0;bytes[8533]=1;sign(result);return result;};
    const auto embedded=withPersonality(retained);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities);
    auto forged=retained;forged.design.units[0].module=NativeModule::frequencyShift;forged.design.units[0].defaults={{0,"frequency_shift.mix",.5f}};rejectOld(withPersonality(forged));forged=retained;forged.design.units[0].defaults.push_back({0,"frequency_shift.mix",.5f});rejectOld(withPersonality(forged));forged=retained;forged.design.panel.controls={{"Shift",0,0,"frequency_shift.hz",FocusWidget::rotary,FocusResponse::linear,-1000,1000}};rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[8466]=11;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,9809,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

juce::MemoryBlock capturedConvolutionOldSound13()
{
    constexpr const char* encoded=
        "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"
        "rsfqZ65HIT+/EG3CzcxMPQHl/M3sUTg+m9HssFK4nj7PpIfNrkfhPp6TkbyF6xE/LOWuRzMzMz8WyN7Vj8L1PVlreo8AAIA+FuXNNFyPwj7xmPkd"
        "XI8CP3oAETwK1yM/s59g/o/CdT103F/jXI9CPgMr4hwK16M+VFHjHGZm5j6/Etrf4XoUP/z2SJkAAAAASc1dBLgeBT5SkYd4uB6FPh2iHsYUrsc+"
        "cmWbqbgeBT99XPUwZmYmP07GhnYpXI89b4RF9s3MTD6Qbspjw/WoPtcaR3Mfhes+MBc3uj0KFz+7cfTwCtcjPHDCMLkpXA8+1U6dYXE9ij4BAAAA"
        "AP///wEAAAQAAAQAAAQAAAQAAAQAAAQAAAQAAAT/////////////////////////////////////////////////////AAAAAAAWyN7VMzMzv1lr"
        "eo/MzAy/FuXNNMzMzL7xmPkd/v9/vnoAETzIzMy9s59g/tDMTD103F/j0MxMPgMr4hw2M7M+AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAABwAAAAAQAEAggMAAABAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEB"
        "AQAAAACsDe6OuEf+ZYgD0x4pIvW+bQzZZnZRP4E3IKHQJoycNg==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==10033&&juce::SHA256(wire).toHexString()=="c881c7691dfebbf6f239aff48f8cad12210e0f85afd9ca07b592f56f34c698d9");return wire;
}
void convolution_state_and_old_sound13_authority()
{
    section("convolution_state_and_old_sound13_authority");const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    CHECK(parameterCount==4519&&convolutionParameterStart==1079&&convolutionParametersPerSlot==4&&effectBankCount==19);
    CHECK(oldSound13ParameterCount==1079&&oldSound13NumericBytes==9881&&oldSound13StateSize==10033&&oldSound13CompatibilityBytes==112&&oldSound13MaximumBytes==796949);
    juce::Array<juce::var> rows;for(const auto& d:registry.first(1079)){juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});}
    const auto text=juce::JSON::toString(rows,true,17);CHECK(juce::SHA256(text.toRawUTF8(),std::size_t(text.getNumBytesAsUTF8())).toHexString()=="43d34a69e051022769b92ca214678b509cad45a21cd3d2c074e229f36b642a11");
    constexpr Field fields[]{{"convolution.ir",0,1,1,1,2,""},{"convolution.predelay_ms",0,250,0,1,0,"ms"},{"convolution.wet_trim_db",-24,12,0,1,0,"dB"},{"convolution.mix",0,1,.25f,1,0,""}};
    for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<4;++field){const auto index=1079+4*slot+field;const auto& expected=fields[field];const auto& d=registry[index];
        CHECK(d.id=="effect."+juce::String(slot+1)+"."+expected.id&&d.stateKey==fnv(d.id)&&d.unit==expected.unit&&d.minimum==expected.low&&d.maximum==expected.high&&d.defaultValue==expected.initial&&d.skew==1);
        CHECK(d.kind==(field==0?ParameterKind::choice:ParameterKind::continuous));if(field==0)CHECK(d.choices==std::vector<juce::String>({"Unit Impulse","Short Room"}));else CHECK(d.choices.empty());
        CHECK(rackParameterIndex(NativeModule::convolution,slot,expected.id)==index&&near(parameterPhysicalValue(defaults,index),expected.initial));for(float endpoint:{0.f,1.f}){SoundDocument source;source.values[index]=endpoint;RoutedPatch p;CHECK(makeRoutedPatch(source,p));const float mapped[]{float(p.effects[slot].convolution.ir),p.effects[slot].convolution.predelayMs,p.effects[slot].convolution.wetTrimDb,p.effects[slot].convolution.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}}
    SoundDocument modern;for(std::uint8_t slot=0;slot<8;++slot){CHECK(editRackLayout(modern.layout,{RackEditKind::addEffect,slot,masterChain,slot,slot==7?EffectKind::drive:EffectKind::convolution},modern.layout)==RackEditResult::applied);modern.compatibility.effects[slot][2]=1+slot%2;for(std::size_t field=0;field<4;++field){const auto i=1079+4*slot+field;modern.values[i]=field==0?float(slot%2):.13f+.07f*float((slot+field)%8);modern.controlLocks[i]=(slot+field)%3==0;}}
    modern.macros[0]={registry[1082].stateKey,.17f};modern.effectLocks[7]=true;RackInstance instance;instance.design.name="Convolution retained67";instance.design.role=RackRole::fx;instance.design.panel.sections={"Room"};instance.design.units={{0,NativeModule::convolution,{}}};instance.bindings={{0,0}};
    for(std::size_t i=0;i<dynamicEqParameterStart;++i)if(registry[i].id.startsWith("effect.1."))instance.design.units[0].defaults.push_back({0,registry[i].id.substring(9),modern.values[i]});CHECK(instance.design.units[0].defaults.size()==82);
    instance.design.panel.controls={{"Mix",0,0,"convolution.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
    for(bool embedded:{false,true}){auto source=modern;if(embedded){auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.personalities=state;}CHECK(validateSoundDocument(source));juce::MemoryBlock wire;const bool encoded=encodeSoundState(source,wire);CHECK(encoded);if(!encoded)continue;CHECK(read32(wire,4)==currentSoundStateVersion&&read32(wire,8)==4519);SoundDocument out;CHECK(decodeSoundState(wire.getData(),wire.getSize(),out)&&out==source);
        const auto reject=[&](juce::MemoryBlock bad){sign(bad);out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,2,255}){auto bad=wire;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+12]=revision;reject(bad);auto invalid=source;invalid.compatibility.effects[slot][12]=revision;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto invalid=source;invalid.values[1079]=.5f;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));
        if(embedded){invalid=source;auto state=std::make_shared<RackPersonalityState>(*source.personalities);state->instances[0].design.units[0].algorithmRevision=2;invalid.personalities=state;CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
    }
    const auto raw=capturedConvolutionOldSound13();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    for(std::size_t i=0;i<1079;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[8790+i]!=0));}
    for(std::size_t i=1079;i<parameterCount;++i)CHECK(old.values[i]==defaults[i]&&!old.controlLocks[i]);for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<12;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[9901+12*slot+bank]);CHECK(old.compatibility.effects[slot][12]==1);}
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[9893+2*slot+bank]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,8726+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,8730+8*macro));
    juce::MemoryBlock saved;CHECK(encodeSoundState(old,saved)&&read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(1079),std::size_t(1110)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,8726,registry[index].stateKey);write32(bad,8730,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto bad=raw;static_cast<unsigned char*>(bad.getData())[8658]=12;rejectOld(bad);bad=raw;write32(bad,8,1111);rejectOld(bad);bad=raw;static_cast<unsigned char*>(bad.getData())[9890]=13;rejectOld(bad);
    RackInstance retained;retained.design.name="Old13 retained Shifter63";retained.design.role=RackRole::fx;retained.design.panel.sections={"Shift"};retained.design.units={{0,NativeModule::frequencyShift,{}}};retained.bindings={{0,0}};
    for(std::size_t i=0;i<1079;++i)if(registry[i].id.startsWith("effect.1."))retained.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});CHECK(retained.design.units[0].defaults.size()==63);
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());juce::MemoryOutputStream out;out.write(raw.getData(),9997);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* bytes=static_cast<unsigned char*>(result.getData());bytes[8657]=1;bytes[8658]=static_cast<unsigned char>(static_cast<unsigned>(owner.design.units[0].module)-2);bytes[8713]=0;bytes[8725]=1;sign(result);return result;};
    const auto embedded=withPersonality(retained);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities);
    auto forged=retained;forged.design.units[0].module=NativeModule::convolution;forged.design.units[0].defaults={{0,"convolution.mix",.5f}};rejectOld(withPersonality(forged));forged=retained;forged.design.units[0].defaults.push_back({0,"convolution.mix",.5f});rejectOld(withPersonality(forged));forged=retained;forged.design.panel.controls={{"Mix",0,0,"convolution.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};rejectOld(withPersonality(forged));
    struct Alias:RackPersonalityState{juce::MemoryBlock wire;};auto owner=std::make_shared<Alias>();owner->wire=raw;static_cast<unsigned char*>(owner->wire.getData())[8658]=12;sign(owner->wire);auto alias=old;alias.personalities=owner;std::weak_ptr<Alias> weak=owner;const auto* data=owner->wire.getData();owner.reset();CHECK(!decodeSoundState(data,10033,alias)&&!weak.expired()&&alias.personalities==weak.lock());
}

// Frozen accepted native bytes; this fixture never invokes the current writer.
juce::MemoryBlock capturedFrequencySplitOldSound14()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "DjLMzMy+93if5f7/f75WXOD8yMzMvcSezCPQzEw9rNO7q9DMTD5J8XU2NjOzPgEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAB4"
        "AAAAAQAEAggNAAABAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEB"
        "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAAAAAC82frDbOBYpC2AmOxhTb3mxpc4AumC7e7gRQ4cWjZw6"));
    const auto raw=out.getMemoryBlock();CHECK(raw.getSize()==10329&&juce::SHA256(raw).toHexString()=="b4dd78f22d27f3e9465ca1ed8ca5b9bdfd3c091e0598d4316c23fdf1a3ebfa72");return raw;
}

void frequency_split_state_and_old_sound14_authority()
{
    section("frequency_split_state_and_old_sound14_authority");
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    CHECK(parameterCount==4519&&effectBankCount==19&&frequencySplitParameterStart==1111&&frequencySplitParametersPerSlot==8);
    const auto raw=capturedFrequencySplitOldSound14();SoundDocument old;
    const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    for(std::size_t i=0;i<1111;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[9046+i]!=0));}
    for(std::size_t i=1111;i<1231;++i)CHECK(std::bit_cast<std::uint32_t>(old.values[i])==std::bit_cast<std::uint32_t>(defaults[i])&&!old.controlLocks[i]);
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<13;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[10189+13*slot+bank]);CHECK(old.compatibility.effects[slot][13]==1);}
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[10181+2*slot+bank]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,8982+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,8986+8*macro));
    juce::MemoryBlock upgraded;CHECK(encodeSoundState(old,upgraded)&&read32(upgraded,4)==currentSoundStateVersion&&upgraded.getSize()==41053);
    SoundDocument recalled;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(1111),std::size_t(1174)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,8982,registry[index].stateKey);write32(bad,8986,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto forged=raw;static_cast<unsigned char*>(forged.getData())[8914]=13;rejectOld(forged);forged=raw;write32(forged,8,1175);rejectOld(forged);
    RackInstance retained;retained.design.name="Old14 retained Convolution67";retained.design.role=RackRole::fx;retained.design.panel.sections={"Room"};retained.design.units={{0,NativeModule::convolution,{}}};retained.bindings={{0,0}};
    for(std::size_t i=0;i<1111;++i)if(registry[i].id.startsWith("effect.1."))retained.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});CHECK(retained.design.units[0].defaults.size()==67);
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());juce::MemoryOutputStream out;out.write(raw.getData(),10293);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* b=static_cast<unsigned char*>(result.getData());b[8913]=1;b[8914]=12;b[8969]=0;b[8981]=1;sign(result);return result;};
    const auto embedded=withPersonality(retained);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities);
    auto invalidOwner=retained;invalidOwner.design.units[0].module=NativeModule::frequencySplit;invalidOwner.design.units[0].defaults={{0,"frequency_split.mix",.5f}};rejectOld(withPersonality(invalidOwner));
    invalidOwner=retained;invalidOwner.design.units[0].defaults.push_back({0,"frequency_split.mix",.5f});rejectOld(withPersonality(invalidOwner));
    invalidOwner=retained;invalidOwner.design.panel.controls={{"Mix",0,0,"frequency_split.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};rejectOld(withPersonality(invalidOwner));
    for(int quality:{0,1})for(bool active:{false,true}){
        SoundDocument source;source.values[758]=float(quality);
        for(std::size_t slot=0;slot<8;++slot){source.compatibility.effects[slot][2]=1+slot%2;for(std::size_t field=0;field<8;++field){const auto index=1111+8*slot+field;source.values[index]=(field==2||field==5)?float((slot+field)%2):.11f+.07f*float((slot+field)%8);source.controlLocks[index]=(slot+field)%3==0;}}
        if(active)CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::frequencySplit},source.layout)==RackEditResult::applied);
        source.macros[0]={registry[1111].stateKey,.17f};juce::MemoryBlock current;const bool encoded=encodeSoundState(source,current);CHECK(encoded);if(!encoded)continue;
        CHECK(read32(current,4)==currentSoundStateVersion&&read32(current,8)==4519&&current.getSize()==41053);CHECK(decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==source);
        for(std::size_t slot:{std::size_t(0),std::size_t(7)}){auto bad=current;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+13]=255;sign(bad);recalled=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),recalled)&&recalled==source);auto invalid=source;invalid.compatibility.effects[slot][13]=255;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto bad=current;static_cast<unsigned char*>(bad.getData())[layoutOffset+10]=255;sign(bad);recalled=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),recalled)&&recalled==source);
        auto invalid=source;invalid.values[1113]=.5f;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));
    }
}

// Frozen accepted native bytes; this fixture never invokes the current writer.
juce::MemoryBlock capturedMidSideOldSound15()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAACAAAAAAQAEAggOAAABAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEAAAAAeikTU4Stlv2E"
        "6wa48EPugrKTjeEd6MeKnTCzcijgxxw="));
    const auto raw=out.getMemoryBlock();CHECK(raw.getSize()==10913&&juce::SHA256(raw).toHexString()=="76c73af81a289a1eb5f0706a549f8f08bc6ad78af54bf066c69c085f51fbf094");return raw;
}

void mid_side_state_and_old_sound15_authority()
{
    section("mid_side_state_and_old_sound15_authority");const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    CHECK(parameterCount==4519&&effectBankCount==19&&midSideSplitParameterStart==1175&&midSideSplitParametersPerSlot==7);
    constexpr float normalized[]{2.f/3.f,0,.5f,2.f/3.f,0,.5f,1};
    constexpr Field fields[]{
        {"mid_side_split.mid_gain_db",-24,12,0,1,0,"dB"},{"mid_side_split.mid_mute",0,1,0,1,2,""},
        {"mid_side_split.mid_tone",-1,1,0,1,0,""},{"mid_side_split.side_gain_db",-24,12,0,1,0,"dB"},
        {"mid_side_split.side_mute",0,1,0,1,2,""},{"mid_side_split.side_tone",-1,1,0,1,0,""},{"mid_side_split.mix",0,1,1,1,0,""}};
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<7;++field){const auto index=1175+7*slot+field;const auto& d=registry[index];const auto& expected=fields[field];
        CHECK(d.id=="effect."+juce::String(slot+1)+"."+expected.id&&d.stateKey==fnv(d.id)&&parameterIndexForKey(d.stateKey)==index);
        CHECK(d.minimum==expected.low&&d.maximum==expected.high&&d.defaultValue==expected.initial&&d.skew==1&&d.unit==expected.unit);
        CHECK(std::bit_cast<std::uint32_t>(defaults[index])==std::bit_cast<std::uint32_t>(normalized[field]));
        CHECK(d.kind==((field==1||field==4)?ParameterKind::toggle:ParameterKind::continuous));if(field==1||field==4)CHECK(d.choices==std::vector<juce::String>({"Off","On"}));else CHECK(d.choices.empty());
        CHECK(rackParameterIndex(NativeModule::midSideSplit,slot,expected.id)==index&&near(parameterPhysicalValue(defaults,index),expected.initial));
        for(float endpoint:{0.f,1.f}){SoundDocument source;source.values[index]=endpoint;RoutedPatch patch;CHECK(makeRoutedPatch(source,patch));const auto& ms=patch.effects[slot].midSideSplit;const float mapped[]{ms.midGainDb,float(ms.midMuted),ms.midTone,ms.sideGainDb,float(ms.sideMuted),ms.sideTone,ms.mix};CHECK(near(mapped[field],endpoint==0?expected.low:expected.high));}
    }
    const auto raw=capturedMidSideOldSound15();SoundDocument old;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),old);CHECK(decoded);if(!decoded)return;
    unsigned continuous=0;for(std::size_t i=0;i<1175;++i){CHECK(read32(raw,16+8*i)==registry[i].stateKey&&std::bit_cast<std::uint32_t>(old.values[i])==read32(raw,20+8*i));CHECK(old.controlLocks[i]==(static_cast<const unsigned char*>(raw.getData())[9558+i]!=0));if(registry[i].kind==ParameterKind::continuous)++continuous;}CHECK(continuous==795);
    for(std::size_t i=1175;i<1231;++i)CHECK(std::bit_cast<std::uint32_t>(old.values[i])==std::bit_cast<std::uint32_t>(defaults[i])&&!old.controlLocks[i]);
    for(std::size_t slot=0;slot<8;++slot){for(std::size_t bank=0;bank<14;++bank)CHECK(old.compatibility.effects[slot][bank]==static_cast<const unsigned char*>(raw.getData())[10765+14*slot+bank]);CHECK(old.compatibility.effects[slot][14]==1);}
    for(std::size_t slot=0;slot<4;++slot)for(std::size_t bank=0;bank<2;++bank)CHECK(old.compatibility.synth[slot][bank]==static_cast<const unsigned char*>(raw.getData())[10757+2*slot+bank]);
    for(std::size_t macro=0;macro<8;++macro)CHECK(old.macros[macro].targetKey==read32(raw,9494+8*macro)&&std::bit_cast<std::uint32_t>(old.macros[macro].amount)==read32(raw,9498+8*macro));
    SoundDocument expected;for(std::size_t i=0;i<1175;++i){if(registry[i].kind==ParameterKind::continuous)expected.values[i]=float((i*13)%71)/100;expected.controlLocks[i]=(i%17)==0;}
    constexpr std::size_t targets[]{0,1,3,4,6,7};for(std::size_t i=0;i<8;++i){const auto index=1111+8*(i/6)+targets[i%6];CHECK(registry[index].kind==ParameterKind::continuous);expected.macros[i]={registry[index].stateKey,-.7f+.15f*float(i)};}for(std::size_t slot=0;slot<8;++slot)expected.compatibility.effects[slot][2]=1+slot%2;CHECK(old==expected);
    juce::MemoryBlock saved;CHECK(encodeSoundState(old,saved)&&read32(saved,4)==currentSoundStateVersion&&saved.getSize()==41053);SoundDocument recalled;CHECK(decodeSoundState(saved.getData(),saved.getSize(),recalled)&&recalled==old);
    const auto rejectOld=[&](juce::MemoryBlock bad){sign(bad);auto out=old;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==old);};
    for(std::size_t index:{std::size_t(1175),std::size_t(1230)}){auto bad=raw;write32(bad,16,registry[index].stateKey);rejectOld(bad);bad=raw;write32(bad,9494,registry[index].stateKey);write32(bad,9498,std::bit_cast<std::uint32_t>(.2f));rejectOld(bad);}
    auto forged=raw;static_cast<unsigned char*>(forged.getData())[9426]=14;rejectOld(forged);forged=raw;write32(forged,8,1231);rejectOld(forged);
    RackInstance retained;retained.design.name="Old15 retained Frequency75";retained.design.role=RackRole::fx;retained.design.panel.sections={"Bands"};retained.design.units={{0,NativeModule::frequencySplit,{}}};retained.bindings={{0,0}};
    for(std::size_t i=0;i<1175;++i)if(registry[i].id.startsWith("effect.1."))retained.design.units[0].defaults.push_back({0,registry[i].id.substring(9),defaults[i]});CHECK(retained.design.units[0].defaults.size()==75);
    const auto withPersonality=[&](const RackInstance& owner){juce::MemoryBlock design;CHECK(encodeRackDesign(owner.design,design,RackValidationMode::instanceRecall));juce::MemoryOutputStream section;section.writeShort(1);section.writeByte(1);section.writeByte(0);section.write(owner.id.getRawData(),16);section.writeByte(0);section.writeByte(1);section.writeByte(0);section.writeByte(0);section.writeInt(int(design.getSize()));section.write(design.getData(),design.getSize());juce::MemoryOutputStream out;out.write(raw.getData(),10877);out.writeInt(int(section.getDataSize()));out.write(section.getData(),section.getDataSize());out.write(juce::MemoryBlock(32,true).getData(),32);auto result=out.getMemoryBlock();auto* b=static_cast<unsigned char*>(result.getData());b[9425]=1;b[9426]=13;b[9481]=0;b[9493]=1;sign(result);return result;};
    const auto embedded=withPersonality(retained);CHECK(decodeSoundState(embedded.getData(),embedded.getSize(),recalled)&&recalled.personalities);
    auto owner=retained;owner.design.units[0].module=NativeModule::midSideSplit;owner.design.units[0].defaults={{0,"mid_side_split.mix",.5f}};rejectOld(withPersonality(owner));owner=retained;owner.design.units[0].defaults.push_back({0,"mid_side_split.mix",.5f});rejectOld(withPersonality(owner));owner=retained;owner.design.panel.controls={{"Mix",0,0,"mid_side_split.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};rejectOld(withPersonality(owner));
    for(int quality:{0,1})for(bool active:{false,true}){SoundDocument source;source.values[758]=float(quality);for(std::size_t slot=0;slot<8;++slot){source.compatibility.effects[slot][2]=1+slot%2;for(std::size_t field=0;field<7;++field){const auto index=1175+7*slot+field;source.values[index]=(field==1||field==4)?float((slot+field)%2):.13f+.07f*float((slot+field)%8);source.controlLocks[index]=(slot+field)%3==0;}}
        if(active)CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::midSideSplit},source.layout)==RackEditResult::applied);source.macros[0]={registry[1175].stateKey,.17f};juce::MemoryBlock current;const bool encoded=encodeSoundState(source,current);CHECK(encoded);if(!encoded)continue;CHECK(read32(current,4)==currentSoundStateVersion&&read32(current,8)==4519&&current.getSize()==41053);CHECK(decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==source);
        for(std::size_t slot:{std::size_t(0),std::size_t(7)}){auto bad=current;static_cast<unsigned char*>(bad.getData())[currentSoundNumericBytes+20+effectBankCount*slot+14]=255;sign(bad);recalled=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),recalled)&&recalled==source);auto invalid=source;invalid.compatibility.effects[slot][14]=255;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));}
        auto bad=current;static_cast<unsigned char*>(bad.getData())[layoutOffset+10]=255;sign(bad);recalled=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),recalled)&&recalled==source);auto invalid=source;invalid.values[1176]=.5f;juce::MemoryBlock keep("keep",4);CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));
        invalid=source;invalid.macros[0]={registry[1176].stateKey,.17f};CHECK(!encodeSoundState(invalid,keep)&&keep==juce::MemoryBlock("keep",4));
    }
}

namespace {
void shape_extension_sound20_and_frozen_sound19()
{
    {
        section("shared_effective_projection_macro_sum_clamp_and_refusal_atomicity");
        SoundDocument d;const auto i=saturationParameterStart+5;
        d.values[i]=.2f;d.values[1]=.5f;d.values[2]=.25f;
        d.macros[0]={parameterRegistry()[i].stateKey,.8f};d.macros[1]={parameterRegistry()[i].stateKey,-.4f};
        ParameterValues projected;projected.fill(.123f);CHECK(makeEffectiveParameterValues(d,projected));CHECK(near(projected[i],.5f));CHECK(d.values[i]==.2f);
        d.macros[1].amount=1;d.values[i]=.8f;CHECK(makeEffectiveParameterValues(d,projected));CHECK(projected[i]==1);
        d.macros[0].amount=-1;d.macros[1].amount=-1;d.values[i]=.1f;CHECK(makeEffectiveParameterValues(d,projected));CHECK(projected[i]==0);
        const auto sentinel=projected;d.macros[1].amount=2;CHECK(!makeEffectiveParameterValues(d,projected));CHECK(projected==sentinel);
        d.macros={};d.values[i]=std::numeric_limits<float>::quiet_NaN();CHECK(!makeEffectiveParameterValues(d,projected));CHECK(projected==sentinel);
        d.values=defaultParameterValues();d.compatibility.effects[0][0]=999;CHECK(!makeEffectiveParameterValues(d,projected));CHECK(projected==sentinel);
    }
    section("shape_extension_sound20_and_frozen_sound19");
    CHECK(currentSoundStateVersion==31&&parameterCount==4519);
    CHECK(currentSoundNumericBytes==40841&&currentSoundStateSize==41053&&maximumSoundStateBytes==830205);
    const auto directory=juce::File(RACKSYNTH_FIXTURE_DIR).getChildFile("eq-shapes-legacy");
    for(const auto* name:{"default-v19.soundstate","rich-v19.soundstate","embedded-v19.soundstate"}){
        juce::MemoryBlock raw;CHECK(directory.getChildFile(name).loadFileAsData(raw));
        CHECK(read32(raw,4)==19&&read32(raw,8)==2023);SoundDocument old;CHECK(decodeSoundState(raw.getData(),raw.getSize(),old));
        for(std::size_t i=2023;i<parameterCount;++i)CHECK(old.values[i]==SoundDocument{}.values[i]&&!old.controlLocks[i]);
        RoutedPatch patch;CHECK(makeRoutedPatch(old,patch));
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto base=1231+slot*89+band*11;
            CHECK(patch.effects[slot].dynamicEq.bands[band].shape==DynamicEqShape(int(parameterPhysicalValue(old.values,base+2))));
            CHECK(patch.effects[slot].dynamicEq.bands[band].slope==DynamicEqSlope(int(parameterPhysicalValue(old.values,base+6))));}
        juce::MemoryBlock upgraded;CHECK(encodeSoundState(old,upgraded));CHECK(read32(upgraded,4)==currentSoundStateVersion&&read32(upgraded,8)==4519);
        SoundDocument recalled;CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),recalled)&&recalled==old);
        if(parameterCount>=2191){auto forged=raw;write32(forged,16,parameterRegistry()[dynamicEqExtensionParameterStart].stateKey);sign(forged);recalled=old;CHECK(!decodeSoundState(forged.getData(),forged.getSize(),recalled)&&recalled==old);}
    }
    if(parameterCount<2191)return;
    for(int selection=0;selection<5;++selection){
        SoundDocument sound;for(std::size_t i=dynamicEqExtensionParameterStart;i<delayStereoParameterStart;++i){const int count=int(parameterRegistry()[i].choices.size());sound.values[i]=float(selection%count)/float(count-1);sound.controlLocks[i]=i%2==0;}
        juce::MemoryBlock encoded;CHECK(encodeSoundState(sound,encoded));CHECK(encoded.getSize()==41053&&read32(encoded,4)==currentSoundStateVersion);
        SoundDocument recalled;CHECK(decodeSoundState(encoded.getData(),encoded.getSize(),recalled)&&recalled==sound);
    }
}
}

namespace {
void merged_main_and_pro_r_state_lineages()
{
    section("merged_main_and_pro_r_state_lineages");
    CHECK(parameterCount==4519&&spaceReverbParameterStart==2735&&currentSoundStateVersion==31);
    // Frozen pre-merge F capture: technical-render-packet.zip / ducking-higher-48000.soundstate.
    // Its bytes remain independent of the current writer and of future review-packet replacement.
    juce::MemoryOutputStream captured;
    const char* const capturedParts[] = {
        "QVJTSRUAAAAPCwAAAAAAAFtMkjHNzEw/wh3lygAAAAAvHOXJAAAAAJwa5cgAAAAACRnlxwAAAAB2F+XGAAAAAOMV5cUAAAAAUBTlxAAAAADtK+XTAAAAADii",
        "e5oAAAAAdwl20gAAAADyhYvsAAAAADP2odUAAIA/lPYd4AAAAAD06IjNAAAAPzUdWWMAAAA/GgK6/AAAAACveblyAAAAAPmT9CoAAAA/qJCnDQAAAD+VapRM",
        "AAAAAN7nbvkAAAAASs1MlwAAAD+7xbclAAAAP1yl62MAAAAA2UZVLgAAAAD3ThLRAAAAP9ai7/AAAAA/Y38fPKuqKj/2ohIZAAAAP13+nBoAAAA/bnYrXwAA",
        "AD8eia3yAAAAP8iUT9Krqio/MTFrjgAAAD8i284DAAAAP3dXiHAAAAA/p6PM8wAAAD9mBeeRAAAAAE1och/vm1E/eBICRaK8hj57257zAAAAADDyI0D56QA+",
        "ImPAgcirXD4dFLWCMzMzP9kmJEWjZUM+XAlNeFwaqj3m8CTa+PJrPgnMxRpmZiY/vcrFDvjyaz4pFmauAAAAP9ogILSrqio/61XQigAAAD8E0PYBAAAAP9Uh",
        "O7kAAAA/HWg7FAAAAD/xsg2Dq6oqP+yGz4kAAAA/Qxdw5wAAAD9QVXySAAAAPxCRBSEAAAA/FaDPFQAAAAB6TqqPoQYrP10LCtaivIY+aHXALgAAAABhf3+r",
        "XBqqPbkmfIz48ms+brF4a2ZmJj/aBt4J+PJrPpf1+ddcGqo91/v4qfjyaz5chmUDZmYmP/QO5Nj48ms+iBlGiQAAAD8NavN7q6oqP7hk63gAAAA/bx3c2AAA",
        "AD98GxxeAAAAPwzoPoIAAAA/VjhamquqKj/3AZfaAAAAP8DijQMAAAA/QQpPDgAAAD+JKD9xAAAAP9AKGagAAAAAdwkD2aEGKz/OEvdhoryGPuE9mbgAAAAA",
        "3hOyTFwaqj2wdW87+PJrPoNVujtmZiY/k8lUhvjyaz5yoV04XBqqPcxTBcz48ms+t5gq/2ZmJj9vPgXy+PJrPltm4f4AAAA/1L/VxauqKj/t17YmAAAAPx6S",
        "Zp4AAAA/g6hT2gAAAD8ThnGrAAAAP9966sirqio/cueFkgAAAD8570GdAAAAP/py6ZIAAAA/itzzdQAAAD//fsx5AAAAAJzouEChBis/g4qdD6K8hj6WHhRe",
        "AAAAAI+7XTlcGqo9vxjGlPjyaz60ak3HZmYmP/wwFq748ms+nYsAkFwaqj19/XTn+PJrPqpI+EdmZiY/nuyewvjyaz7ql5v7AAAAP3M+2P4AAAAA7rBlswAA",
        "AACHOJxBAAAAPxA6bFAAAAA/EXUuLQAAAD9CPq2RAAAAAOmERRcAAAAAiDKVFAAAAD9/9hGaAAAAP8wegmwAAAA/KTnqpQAAAACWgcr9oQYrP9EttEiivIY+",
        "HBeMrAAAAAAVQi5bXBqqPSXho2H48ms+YtU+x2ZmJj+G8sLt+PJrPjvubVVcGqo9A0oNjPjyaz4QCVgXZmYmP9D2+cv48ms+FCZT7AAAAD/SunAZAAAAANka",
        "6SgAAAAA2HTxDwAAAD+PDOj5AAAAPxwImw8AAAA/g5S4dQAAAABelYMQAAAAAJeXgfEAAAA/YPpIIAAAAD9hxx1/AAAAP/BVy10AAAAAl0qYhKEGKz9uLkkD",
        "oryGPgGeNe8AAAAAfjDjHFwaqj3Q/nPP+PJrPqMACllmZiY/syVtv/jyaz7SAcG6XBqqPawYh5T48ms+lxCO9GZmJj9PTe7k+PJrPjuambcAAAA/7fjnaQAA",
        "AAAIN5MhAAAAAPUcZJMAAAA/JuW3XgAAAD/Lqqq3AAAAP5B5jXoAAAAAn7bWawAAAAB6aE2XAAAAP6EU8G0AAAA/YhocWwAAAD/H9epwAAAAAPQw+16hBis/",
        "y+dpMaK8hj6+X5N9AAAAAGctD81cGqo955S7rfjyaz4M+Kz1ZmYmP2RdJQ/48ms+lUCE/lwaqj2l+E60+PJrPuJ4nmdmZiY/BpYijvjyaz4y/E+yAAAAPxyk",
        "DkcAAAAAK2gBjwAAAABWGXLoAAAAP709BaUAAAA/TjRzcgAAAD+51QhTAAAAAJQ0gW0AAAAAEYXqswAAAD8C8CCPAAAAP9dpB4UAAAA/RhvrzgAAAACtcv4a",
        "oQYrP1gO1u6ivIY+2/bdpgAAAACQFt3mXBqqPQKNQ9j48ms+/StOAWZmJj+5vPrv+PJrPnw7SStcGqo9hkjljfjyaz6pbY/ZZmYmP10oz8r48ms+yVyw+AAA",
        "AD8XKXh4AAAAABF9vlEAAAAAQHFVNwAAAAA+z4ZgAAAAAA3f3Y4AAAAAm/qiiQAAAACuV1/wAAAAAFA3KeEAAAAAZ8A8JgAAAAD4qq60AAAAADcZxCSyzcI8",
        "k+esHfHwcD1WUo6xAAAAADuFO4YAAAAAvhlzB7LNwjxyxiC28fBwPcpEGiIAAAAAV1gengAAAABSQ9sGss3CPL7nXifx8HA9SwP1CwAAAABU9q4oAAAAACth",
        "RTCyzcI8D3GtRfHwcD2d0yJ1AAAAAMIcbc4AAAAArStYX7LNwjzdEwmO8fBwPcDGLfoAAAAAsZs92wAAAAAw2CAgss3CPJhwRmbx8HA9gFX3JAAAAABxeXyJ",
        "AAAAAPCl2YCyzcI8WKNQvfHwcD1drF8nAAAAAILVB7sAAAAAbZU5aLLNwjyd8Mk98fBwPZ4r+C0AAAAA2NFjdwAAAABIMPWNAAAAAJeWX4cAAAAASXYaUwAA",
        "AD9X9UfrAAAAAEfyWZMAAAAAe58qYgAAAAA8Og64AAAAALYbPEsAAAA/7ELfSAAAAADCH0jNAAAAAFriqWcAAAAARZguHQAAAABzSpLFAAAAPyXjWu4AAAAA",
        "oXlfIQAAAADtFZwTAAAAACq6a/AAAAAAaN/7+gAAAD9qNpbUAAAAADzMdUEAAAAAVCMggAAAAACbGHDuAAAAAG2nZRcAAAA/wxXJjAAAAAD75pS7AAAAAEfb",
        "lOYAAAAAIFzbhAAAAADqvOHmAAAAPwhligcAAAAAhjptmQAAAAAWGwpUAAAAACmmTMUAAAAAtzj/PgAAAD9B7JzIAAAAAAUGEWUAAAAACXq0kwAAAAAOJHyr",
        "AAAAAPwNDRwAAAA/5rQFegAAAADgsa+uAAAAAGDwSc0AAAAAL18afQAAAABxjQEJAAAAPxQ8RuMAAAAAqo/Z0QAAAAByRFCIAAAAAF2GyS0AAAAAG297NQAA",
        "AD/fVkGtAAAAAK/57zcAAAAAUzxQpAAAAAAUSbZ6AAAAAP4h8RwAAAA/piyMUgAAAACgJe6qAAAAACCvO7AAAAAA7zrywwAAAAAxufHrAAAAPwGo46MAAAAA",
        "xYdK7wAAAADJdXQNAAAAAM5LJgkAAAAAvOeBfAAAAD8wwl76AAAAAE6ks6sAAAAA7nBH7gAAAADBqmqMAAAAAB+0YfoAAAA/q60DSwAAAADD10qfAAAAAB+s",
        "uQMAAAAAeNosZwAAAAAypjK7AAAAP5L+/+AAAAAAJMuzRQAAAADsYkk3AAAAABOM1FUAAAAAlWlFzAAAAD/hwgtbAAAAACVXZE8AAAAAKe1SlwAAAAAugYIS",
        "AAAAABzX9MYAAAA/dDvVbQAAAACKWHqJAAAAAFJVP1kAAAAAPevADwAAAAD7yh3tAAAAPz8SJfsAAAAAj3SZagAAAAAzWzoDAAAAAPRi+nUAAAAA3kDbewAA",
        "AD/ybzAWAAAAAATxpU4AAAAAzOc+sgAAAADz34wLAAAAAHVEhO4AAAA/jaF4hQAAAACJLfUDAAAAAAXDeYsAAAAAwp0MgAAAAADwfSxsAAAAPxAO8vgAAAAA",
        "rkBMNQAAAABOwiyRAAAAACHxBcIAAAAAf1D6gwAAAD8LSbt9AAAAAKP9PKgAAAAA/w5kZQAAAABY4zE2AAAAABLMJMQAAAA/PldRKQAAAAD4RAJ7AAAAAGiS",
        "h6QAAAAAtz4Z1QAAAABpNGw9AAAAP5ls22EAAAAA3eoRqwAAAABxqev5AAAAANbXbegAAAAAdFI7ngAAAD9NOdXjAAAAAEmbQzQAAAAAxSxEvgAAAACC1O4W",
        "AAAAALCuwkMAAAA/sujFigAAAADEV+zUAAAAAIykzt8AAAAAswaC5QAAAAA114weAAAAP8sjw0oAAAAAYwjEWgAAAAC/7T6sAAAAABgrU/kAAAAA0vj2jwAA",
        "AD/Qnrl9AAAAAG7WmAMAAAAADiXvGQAAAADh7XOeAAAAAD9sjSMAAAA/ofkFOwAAAADlr/jEAAAAAOl/W8cAAAAA7n3w7gAAAADct/R7AAAAP8b0BuMAAAAA",
        "QBuhPAAAAADAHIMCAAAAAI8inlMAAAAA0WPxlgAAAD//0whXAAAAAE/+vIcAAAAA8zEDNgAAAAC0LN4MAAAAAJ4XpK4AAAA/YOYGTAAAAAC+p9XQAAAAAF4+",
        "QdAAAAAA0W/voAAAAADvQ3MdAAAAP53k6ugAAAAAWfrCnQAAAABVjlzjAAAAAPI2Fm8AAAAA4POoSAAAAD+CYNYaAAAAAFQTtLEAAAAAXLYGQQAAAABjrmI+",
        "AAAAAGXLiJUAAAA/jyC/5gAAAAAfRmpmAAAAAIMjQJAAAAAA5J7T6gAAAABOU//jAAAAP8Q8SRQAAAAAWuMqTwAAAAAiCXbQAAAAAG0VbqYAAAAAK7NzqgAA",
        "AD/xZiq8AAAAADXJhawAAAAAea5QegAAAACedEoeAAAAAMwgK8wAAAA/VrmKdQAAAABQWxXMAAAAABBshWcAAAAAf63MhQAAAAABc4otAAAAP/NeSPIAAAAA",
        "6+lIAAAAAAD35qd7AAAAADC36xoAAAAAOiB9EQAAAD+4jXsIAAAAADZv9IkAAAAAxpJ0tgAAAAB5i4TLAAAAAAfthPcAAAA/mqyLYwAAAACsXWPQAAAAAISA",
        "uMMAAAAAa6MJpgAAAAD984sRAAAAPxUVZrMAAAAA0ThFkgAAAADdb/dBAAAAAHozjZwAAAAA+KL6xQAAAD94r5ZoAAAAAPZiQIIAAAAAhi5SmQAAAAA5U0xF",
        "AAAAAMeVHQkAAAA/s6BzPAAAAACryS9jAAAAALd4gRsAAAAA8ExIfwAAAAD6kmV0AAAAPw72vSEAAAAAyKA4TgAAAAA4pqiOAAAAAKdNam4AAAAA2cK8KgAA",
        "AD9pyVNKAAAAAC2iL7IAAAAAAdbn3AAAAAAG56SMAAAAAKRBTsEAAAA/3E+9ygAAAAAyM1XhAAAAAMq+VmsAAAAAFZnvsAAAAADD4TPSAAAAP3tcxY4AAAAA",
        "kw1TuAAAAACvO2XMAAAAAGgIpUEAAAAAYidqegAAAD9i8yzQAAAAALSAolwAAAAAvNhBBQAAAADDP8zlAAAAAMXtw1kAAAA/fXzkSAAAAAC527fyAAAAALUk",
        "nlEAAAAAUix7zgAAAABAlBjKAAAAPyQ/8BgAAAAAOi8qowAAAAAC67rCAAAAAE0Q5+AAAAAAC6OzKgAAAD9vBvBcAAAAAH9K3g4AAAAA4ye0OAAAAABEwz6S",
        "AAAAAK7A7Y4AAAA/NpizQQAAAACwcrrPAAAAAHDZcxIAAAAA3yBvEQAAAABhzzmsAAAAP1HbcmsAAAAAFa+OqgAAAABZpZikAAAAAH7xhSwAAAAArAY0ygAA",
        "AD+YMd6qAAAAAJYZm40AAAAAJpnajQAAAADZ+qJZAAAAAGeXK/sAAAA/0/GepwAAAABLDITEAAAAAFeRTn8AAAAAkHLcvwAAAACajWu8AAAAP0PI/7wAAAAA",
        "e/ocugAAAADH7hzlAAAAAKBj/Z4AAAAAatBp5QAAAD+IF8E3AAAAAAZO9ZcAAAAAli6SUgAAAACprW7fAAAAADdMhz0AAAA/pZWRHgAAAAAhjecfAAAAAG0p",
        "JBIAAAAAqsGNCgAAAADo8oP5AAAAP+rozAQAAAAAvN/9PwAAAADUNqh+AAAAABsgkggAAAAA7brtFQAAAD/Xp34bAAAAAMcF4pEAAAAA+7KyYAAAAAC8QTDS",
        "AAAAADYvxEkAAAA/bPUVeQAAAABCM9DLAAAAANr1MWYAAAAAxZ9QNwAAAADzXRrEAAAAP/l7gwIAAAAAvaY3eAAAAABR8NbiAAAAALZS7sIAAAAAVCwohwAA",
        "AD/Yj8g6AAAAAAOYm+wAAIA+qAS+VgAAAABiU5c+z5AYP6/BjKcAAAA/fO1dpwAAAABDOQx6LQp/PYeE2Z/v7u4+bUD+T6+hvD7WMv4UAAAAAK/WBBZcfxk/",
        "QBa/kgAAgD6kmQJoAAAAP6vAUkymicM9PfKw5wAAAD+iqeYzAAAAAElsD34AAIA+NNFVejMzMz+5F524KK8hPgOSzY139Mo9UOhxVgk/OT28chiOAAAAAFQ9",
        "e9gAAIA/+9vEyQAAAACSFQSsAACAPpknya0AAAAAk1IYYs+QGD/aVlGGAAAAP6OTWJsAAAAAUieGQi0Kfz1qu50k7+7uPkqVdMivobw+VQo8ngAAAAA2/NBq",
        "XH8ZP7mlxqEAAIA+LUDj8gAAAD+EecFXponDPWJO0DoAAAA/2VVQJQAAAABK9eKBAACAPjnh9VYzMzM/ega7DyivIT5Ulyk9d/TKPaWB23MJPzk9s3TENwAA",
        "AAAvgCGBAACAP+ZssXMAAAAA7S+ZMQAAgD5+035hAAAAABy7LiPPkBg/ea5TewAAAD/OlhLsAAAAANHadRwtCn89qYsGlO/u7j7DnIvVr6G8PjgHS4QAAAAA",
        "0Qs/rFx/GT8CgVETAACAPq5HowgAAAA/TQAhsKaJwz1zs8BKAAAAP4wCs7cAAAAA54ieqQAAgD6e3lnOMzMzP+ti+uIoryE+pYVPP3f0yj1esyyHCT85Pfo4",
        "UssAAAAArpjdlQAAgD/BeWykAAAAABRRB1QAAIA+XzVwJAAAAADd6UxSz5AYP0y9pdsAAAA/Ve2ooQAAAAA494wHLQp/Pfwt/snv7u4++NHB7q+hvD4fC+xM",
        "AAAAAFChtQpcfxk/Y8G3pAAAgD5vv8zkAAAAP/5+l5WmicM92J/RogAAAD/zaORfAAAAAFgN33sAAIA+S9/nKTMzMz/spq3HKK8hPt6tYHZ39Mo9c0v8gAk/",
        "OT1RAI4PAAAAANm/fNgAAIA/nHrFHAAAAACvkIquAACAPnzPA1cAAAAAlkzmM8+QGD+Dv8+eAAAAP9h7DLgAAAAAn807xC0Kfz3z3nW77+7uPnFeaNivobw+",
        "egXvHgAAAACD+U2WXH8ZPzQf0l8AAIA+WHXvbgAAAD9nIDpLponDPWE8PtAAAAA/9hdZ9gAAAACF6pEmAACAPnBS7GQzMzM/RSBXziivIT7fGPt1d/TKPYRZ",
        "Ca4JPzk9MJCmnwAAAACIAujQAACAPy8nC0EAAAAAXkKwNQAAgD69K5nSAAAAAGejHBDPkBg/bqz8IgAAAD/v8hG1AAAAAH6c/tctCn89BkLaUe/u7j4euB6Q",
        "r6G8PvldIogAAAAA6homNFx/GT/NVai+AACAPkFHEw4AAAA/4HkF56aJwz12g/BcAAAAPy3RgNwAAAAANr/LlgAAgD61mb6WMzMzP3YH4qQoryE+8M8vQXf0",
        "yj0J0qYDCT85PQeVNIAAAAAAQzOMIwAAgD9abvWZAAAAAOn/usMAAIA+MrfILAAAAADQ8PE+z5AYPz2uNg8AAAA/2uF74QAAAAA9FcCpLQp/PYX0FKDv7u4+",
        "l9aCIK+hvD5cBohEAAAAAPVbM4Jcfxk/5nBmbQAAgD4isLx/AAAAP0mtIVWmicM9x5O+NQAAAD9wwBvQAAAAALMGavkAAIA+CqO+AjMzMz+Xw8CgKK8hPnEa",
        "reZ39Mo9wp9OWwk/OT3+yMQlAAAAAJLAi10AAIA/dQdVUgAAAABA4zx9AACAPsMz+xMAAAAAQfWK7c+QGD8wtbNjAAAAP2HuBOsAAAAAlDn91S0Kfz0oDSLE",
        "7+7uPjwMgFmvobw+IwrLOAAAAADkfYnDXH8ZPxfMb8oAAIA+U1wpgAAAAD+aantfponDPYxmjLkAAAA/ZxbcCwAAAAA0zFFeAACAPgfTIfgzMzM/mIDcIyiv",
        "IT5qK7HWd/TKPScRGscJPzk9FV/5+AAAAACtDwA7AACAPzjN8YTNzMw+nZLhqAAAAAA8bkE8AACAP4M6Vsq8kso+RN21AAAAAD85yCDGKaT5PRU4AlPrO7I+",
        "Ap+dOAAAAD8feHwezcxMPl88BnA6fjQ/yK6KTAAAAD9NgcbNKaT5Pa699pm8kso+YyBe1AAAAD+K4lxrKaT5PUoE37nrO7I+x8tHMwAAAD+O5ThrzcxMPnT7",
        "4Og6fjQ/ZZrG3wAAAD8MB2UcKaT5PUWJLh68kso+Mo+FQQAAAD8P5eFqKaT5PRtXOWbrO7I+LBqnuwAAAD9RuPfkzcxMPhEVhUQ6fjQ/hozMtAAAAD+zI4Qq",
        "KaT5PSBja+q8kso+afkvlQAAAD/QaKcOKaT5PfgC9MvrO7I+8Z31xQAAAD8oih5YzcxMPj6eBs86fjQ/83u7XAAAAD8aICfGKaT5PR9RB7K8kso+iGCVMQAA",
        "AD8NohLpKaT5PWlGKD3rO7I+jt+wXQAAAD+r7A2yzcxMPpMmykQ6fjQ/FIhrsQAAAD/JD7ZJKaT5PRqkXYO8kso+F6RMFQAAAD8eXUJyKaT5PW7tjqDrO7I+",
        "I8+ZtAAAAD9K47DLzcxMPhjrp1E6fjQ/kScJUwAAAD+IIkkJKaT5PfGGpZW8kso+5ik72wAAAD9T0bbpKaT5PZ/lnsnrO7I+CLJqfQAAAD+N362yzcxMPjVU",
        "etg6fjQ/IjfiRQAAAD//Mp25KaT5PVwiubO8kso+HU8MEQAAAD90cETrKaT5PfyEUvjrO7I+vfpcgAAAAD9UAtDpzcxMPtIIchs6fjQ/f8NbowAAAD92fEr7",
        "KaT5PQd1638AAAAA9KEhEvOKET+Uz1ROaJeyPG+DSBQAAAAAWlPl0QAAgD+wLX0SAAAAANU8WdjzihE/5Xu2+2iXsjwMULDFAAAAAOdnPLEAAIA/AevgfQAA",
        "AADe1AvI84oRP27VH21ol7I8VZSslQAAAADEFztnAACAP2p+yAAAAAAAZxhtg/OKET/X5HrgaJeyPJqLYAcAAAAAWbSBvQAAgD8T0NTEAAAAAAjfGxrzihE/",
        "cJvjpmiXsjyjUnK1AAAAAAYfwnYAAIA/zGR6EwAAAAAJhLRj84oRP+E/kYtol7I8QFOsngAAAABTiCzWAACAP10Fq8AAAAAAUkInnPOKET/6AUKqaJeyPOnw",
        "xwUAAAAAoOiVmgAAgD9Wt/P8AAAAAHs8gZvzihE/QyhwCmiXsjz+9gUeAAAAANUCsXoAAIA/NJGSIsb6UD10W7RTAACAPu+ap5+3bVs+QHLlqgAAED/bnV05",
        "AACAPr8FCGzG+lA9C7hI1QAAgD7sw7Uut21bPsnZxjwAABA/fGZsyQAAgD5mMqBkxvpQPWLQ8nkAAIA+CRARA7dtWz4KekbVAAAQP3mdZgIAAIA+6WIUEcb6",
        "UD1JR2VZAACAPmby6X+3bVs+e/T5eAAAED8K2ZguAACAPhiNt5bG+lA9mCGmYgAAgD4TM3ott21bPqS9AOAAABA/j3ZplwAAgD5DmNtIxvpQPX/vWcgAAIA+",
        "gJw9uLdtWz4dj5uUAAAQP/CiJqQAAIA+enH0ysb6UD1G5wPiAACAPs0j0x23bVs+jkQuUwAAED99ZyomAACAPm2Mp93G+lA9rU33PQAAgD66g0KAt21bPu9A",
        "YSgAABA/Dqt1QAAAgD5xPlXvxvpQPVF1oJaamRk/1sv434iywz6zAuo9AAAgPzIWznAAAAA/6iA+HMb6UD2255G5mpkZPxX5tMaIssM+HmuGngAAID/Npvni",
        "AAAAP8M8hMXG+lA9/9+bvJqZGT+wnELfiLLDPp2EFlIAACA/cP8VVAAAAD+sFhT8xvpQPSzb6xmamRk/HxPKWYiywz4Iu+AhAAAgP3PrTnYAAAA//UL2AMb6",
        "UD29u8S3mpkZPzIu5WKIssM+n71pTwAAID8+lUeQAAAAP9Y7nJbG+lA9Et8j2JqZGT+B2OGFiLLDPhokhQ0AACA/CVktUQAAAD/fqd06xvpQPesfhAmamRk/",
        "DBXlVoiywz4pqRz1AAAgP1zWbg0AAAA/eLTTpsb6UD04HlemmpkZP9tWibCIssM+xDdttQAAID8v2RoMAAAAP+VCIC+2fiU9ReSRlZqZGT9S7avQJUkSPnep",
        "KDEAADg/RkPyPgAAAD+A/em2tn4lPUDdoNeamRk/20pm6yVJEj68K3pMAAA4P5e22rUAAAA/g6FhyLZ+JT2/81N+mpkZP8Aw3V8lSRI+XfQyowAAOD8woTD1",
        "AAAAPz45znG2fiU9musUZpqZGT9hchOYJUkSPtIzAAsAADg/Mc2vaQAAAD+pnzVOtn4lPQncI1iamRk/Jt7R4iVJEj47oYlKAAA4P8qLNcwAAAA/VA7ACLZ+",
        "JT3UYA2MmpkZP09VUtIlSRI+IPdH+gAAOD875U70AAAAP8fgUKK2fiU9Myzx05qZGT+07PEgJUkSPoHh6nUAADg/BKJrhwAAAD8SF/Nptn4lPT6hMqWamRk/",
        "RTM8+iVJEj5W3TseAAA4P1XDMZgAAAA/HU7nGKuqKj+0Pf3IAAAAP7dfu4oAAAA/NvaFtAAAAACGWFCEq6oqPwfwpVUAAAA/XEIOSAAAAD+vmnKwAAAAAEe+",
        "EGarqio/ageS7gAAAD8Nru+eAAAAP6B5rv4AAAAA0O0Ve6uqKj/FZCuwAAAAPwJlU1UAAAA/0T0N2wAAAADJ5HqYq6oqP/jMHyQAAAA/S7J2MQAAAD9CFvQ4",
        "AAAAACJvo+2rqio/W3SQ8QAAAD/weKcEAAAAP9ti4FsAAAAA8+uLpquqKj9+wGmTAAAAP8HrvXoAAAA/LEyLAQAAAAC8BZgvq6oqP2m48F0AAAA/dshohQAA",
        "AD/tUXd4AAAAALzNgwGamRk/PF68X9iJHT4lKuvVAAAAPyMwKjsAAAA/lVR7wZqZGT/1EPDn2IkdPnwMbUEAAAA/9srMcQAAAD/erd7GmpkZP/K3grPYiR0+",
        "0yh87gAAAD/1+HpbAAAAP2/rEFSamRk/87iRy9iJHT5CJnEzAAAAP0A0wEUAAAA/+ObaJpqZGT84Ebb82IkdPvHLNRQAAAA/7xTmuwAAAD9RpbY7mpkZP3Hu",
        "TpjYiR0+6EwHqAAAAD8yxozwAAAAP4pSmleamRk/rsfqZ9iJHT6/EG3CAAAAPwHl/M0AAAA/m9HssJqZGT/PpIfN2IkdPp6TkbwAAAA/LOWuRwAAAD8WyN7V",
        "YOUAP1lreo8AAAA/FuXNNAAAgD7xmPkdYOUAP3oAETwAAAA/s59g/gAAgD503F/jYOUAPwMr4hwAAAA/VFHjHAAAgD6/EtrfYOUAP/z2SJkAAAA/Sc1dBAAA",
        "gD5SkYd4YOUAPx2iHsYAAAA/cmWbqQAAgD59XPUwYOUAP07GhnYAAAA/b4RF9gAAgD6QbspjYOUAP9caR3MAAAA/MBc3ugAAgD67cfTwYOUAP3DCMLkAAAA/",
        "1U6dYQAAgD5SMw4WAACAPzopP64AAAAA14TSXauqKj8RsA4yAACAPgGPdU0AAIA/93if5QAAAABWXOD8q6oqP8SezCMAAIA+2LuyeQAAgD+s07urAAAAAEnx",
        "dTarqio/x7KhiQAAgD43UpTNAACAPyGNcbMAAAAAQERwvquqKj+i1OI4AACAPgYemzYAAIA/ZvOssQAAAABTuZruq6oqP/XcGSsAAIA+dcsn2AAAgD8TxSWA",
        "AAAAAOLvvLGrqio/OIMplQAAgD6MGR8IAACAP/i0AfsAAAAAlUfsv6uqKj8r5HBrAACAPpssOGEAAIA/XTb90wAAAADcKdepq6oqP2aEup4AAIA+PZlUIUzz",
        "8D7TW5erq6oqP6Yv5dkAAAAAy+2kTQAAAD+pYAxkq6oqPzCKYbEAAAAA3eeXDgAAAD/ODs6zAACAP44BByJM8/A+hsNiP6uqKj85NW+vAAAAADDQluEAAAA/",
        "YlWAAquqKj9t+Dq+AAAAAISfveIAAAA/n57FDAAAgD+L5QceTPPwPuFKiM6rqio/+MbiUQAAAABVMlnSAAAAPweXu7Crqio/UrY37AAAAABHth6AAAAAPzBq",
        "oAQAAIA/7HuaUUzz8D5sSAWVq6oqP/uDWQQAAAAAEoHwUwAAAD8gNT2Cq6oqP3et6QwAAAAA/rle9gAAAD+hrqxHAACAP6Gbda9M8/A+9wDlWKuqKj9CZwJ1",
        "AAAAANfV2OYAAAA/xYZOZauqKj/kDSExAAAAAHFry+4AAAA/2oCEKAAAgD9S9d8hTPPwPiol6BGrqio/xWL/AQAAAACsnit+AAAAP165c/6rqio/QTJvbAAA",
        "AAAI4XqaAAAAP1uSTzEAAIA/XwOY2Ezz8D4lshOGq6oqPwSl+dYAAAAAkVHbeAAAAD+zieg5q6oqP8a6fpQAAAAA6+U8CAAAAD+chyDsAACAP5CELNxM8/A+",
        "8Jt6LKuqKj+H0O3pAAAAAO53RLgAAAA/LLbNjquqKj+7q85nAAAAANKoZbcAAAA/fcK2CwAAgD83c8B9q6oqP4JE6E4AAAAAF0bAwAAAAD+G0oRMq6oqPzkC",
        "vZUAAAAAMJ3kxwAAAD96deu2AACAP+R7+wyrqio/g1p0lgAAAABqaEgiAAAAP4PVkzSrqio/1hRwlwAAAABbph8pAAAAP40himkAAIA/kcZeHquqKj9oB5sx",
        "AAAAAGUtCm4AAAA/0AmqbquqKj8npr+CAAAAAI7gF1EAAAA/CLPk+QAAgD+2/5MZq6oqP6mmTlAAAAAAQEdvXwAAAD/t9TGdq6oqPxy+yAsAAAAAOWJFjAAA",
        "AD/bVyAtAACAP1vDYfWrqio/vqZHhQAAAADTwA92AAAAP4Ki/Nurqio/TZXnTQAAAABk0+9vAAAAPw6YvFMAAIA/iFrtVauqKj/PESPoAAAAAEZcEPoAAAA/",
        "L4VstquqKj+q2eSoAAAAAC8YEgMAAAA/gUUyPwAAgD+FnWWPq6oqP6QnMFAAAAAAMdQR8gAAAD8cyrlYq6oqP2vr2ccAAAAAotm1jQAAAD/s1fV2AACAP5qO",
        "xgOrqio/9aqUoQAAAAB8gM//AAAAP3nsiGCrqio/AJfouAAAAADNLA90AAAAPx9xNvkAAIA/HYANyQAAAADe5HUmAAAAABx/T08AAAAAYhiAc+s7sj6Pt0Iq",
        "AAAAP0bZHb5vfVI+YrezAwAAAACTisI7AAAAP1XiUSWamRk/EMOd8MzimT6p6RWQr2WUPmY9ESUAAAAANe2qvwAAAACZAiGjAAAAACXxNTHrO7I+kuhq4QAA",
        "AD8v1t4vb31SPlPKvO4AAAAAELjuaQAAAD9ijUpHmpkZP9lDnCXM4pk+uvkaXK9llD7zKUE0AAAAABBgTSkAAAAA0tAEFAAAAACA39Nh6zuyPkkysVQAAAA/",
        "8De7lG99Uj6ULv3WAAAAAGW2BUgAAAA/d7tvV5qZGT/6xUoTzOKZPlcxlEivZZQ+bBHbGQAAAABHHWiMAAAAAJc2An8AAAAAuz/vmes7sj5M5zEJAAAAPzGQ",
        "f65vfVI+vbc7MQAAAABSExssAAAAP0Q+4vuamRk/s7WBB8zimT4gK0mTr2WUPgkJwGoAAAAAEk8HhAAAAAAgGZnBAAAAALZBzEnrO7I+2zIkaAAAAD8yIb4S",
        "b31SPjZhfWUAAAAADyqWMwAAAD8BhKeKmpkZP6wl8XXM4pk+jeEEPK9llD5C4tHlAAAAALnRg1MAAAAA/Y4IPwAAAAB5ZI9G6zuyPp557okAAAA/e5uRg299",
        "Uj73Gc5xAAAAACxU/LwAAAA/nuLmj5qZGT+lEdDVzOKZPl4XiXOvZZQ+T80I/gAAAAAUY1LJAAAAAMZ7PkAAAAAApJIu7+s7sj7VO2smAAAAP/zNC+lvfVI+",
        "qJqCAQAAAAAxFf8BAAAAP0O/wbOamRk/BttMK8zimT7Luvn2r2WUPii+uugAAAAAu488GAAAAAALsRLUAAAAAN8Ddg3rO7I+SNH2kAAAAD/NV53Qb31SPiGM",
        "+HcAAAAAjn/QcgAAAD/Awn6QmpkZP09wYzPM4pk+lFbeF69llD5FlvzMq6oqP0LzHDcAAAAAucpnkwAAAAD92f/DAAAAAHnt+WrrO7I+njwagwAAAD97Ntqh",
        "b31SPvdkxfYAAAAALE3g/AAAAD+ea1G0mpkZP6UMsWnM4pk+Xji0Pq9llD4JGgu8AAAAABJI68MAAAAAIGSQRgAAAAC2yjZu6zuyPtv1T2EAAAA/MrwGMW99",
        "Uj42rHTqAAAAAA8jenMAAAA/AQ0Sr5qZGT+sINIJzOKZPo0CMAevZZQ+bCImawAAAABHFkzMAAAAAJeB+QMAAAAAu8hZvus7sj5Mql0CAAAAPzEryMxvfVI+",
        "vQIztgAAAABSDP9rAAAAP0THTCCamRk/s7Bim8zimT4gTHRer2WUPvM6jIUAAAAAEFkxaQAAAADSG/yYAAAAAIBoPobrO7I+SfXcTQAAAD/w0gOzb31SPpR5",
        "9FsAAAAAZa/phwAAAD93RNp7mpkZP/rAK6fM4pk+V1K/E69llD5mTlx2AAAAADXmjv8AAAAAmU0YKAAAAAAleqBV6zuyPpKrltoAAAA/L3EnTm99Uj5TFbRz",
        "AAAAABCx0qkAAAA/Yha1a5qZGT/ZPn25zOKZProaRievZZQ+HZFYGgAAAADe3VlmAAAAABzKRtQAAAAAYqHql+s7sj6Pem4jAAAAP0Z0ZtxvfVI+YgKriAAA",
        "AACTg6Z7AAAAP1VrvEmamRk/EL5+hMzimT6pCkFbr2WUPoBL5ooAAAAA8/YldgAAAABz6SIOAAAAABfvWbrrO7I+0GXijwAAAD9F4yd4b31SPplgO8QAAAAA",
        "lgexjwAAAD+4GN7UmpkZP1dtGInM4pk+jPDDpK9llD53YJz+AAAAAFwE1c8AAAAAfujH/gAAAADs7atK6zuyPu3MlLIAAAA/hAsUfm99Uj5wvPmlAAAAAAl0",
        "KQUAAAA/K/GRMpqZGT++ra8PzOKZPtNJBbivZZQ+QP55C6uqKj8HD1CkAAAAAOyMI0gAAAAALtUyawAAAAA81VEU6zuyPv3DK4cAAAA/lMBKZm99Uj6gJEKz",
        "AAAAANlyOPkAAAA/e5HIx5qZGT+u/wmwzOKZPmMwrBOvZZQ+ZISK5AAAAAA/YRZ9AAAAAB8itX8AAAAAU8PVT+s7sj7U+Pc0AAAAP4kxcAtvfVI+tSrgvAAA",
        "AAC6pzUfAAAAPxxl49KamRk/2/McPMzimT74Pegsr2WUPmEE0F8AAAAAqsZ+AQAAAACoU5e1AAAAAE4JWRLrO7I+A/9iAQAAAD/qYa2Pb31SPo7nbL0AAAAA",
        "V8e3AQAAAD+5gll+mpkZP9QLDjLM4pk+Jed+d69llD4edwOUAAAAAC2s3iUAAAAAAbdHlQAAAAD9RUS66zuyPnrlMKwAAAA/5xbOrG99Uj5rkFU8AAAAAJjk",
        "S+wAAAA/+t7Qf5qZGT9BtIZczOKZPvKu/YOvZZQ+y8u39QAAAACoFH3/AAAAAHrqMIgAAAAAeNLyb+s7sj5xGK5yAAAAP6h4qhFvfVI+zDCmuAAAAADt5qDU",
        "AAAAP2/q9xKamRk/IlOvyMzimT7PWVmtr2WUPrjfOsAAAAAAS+4VAgAAAAA7NO/CAAAAAO8ULMjrO7I+2NlaDQAAAD8d137Wb31SPpGq1CYAAAAAHswiGgAA",
        "AD8Qn8e5mpkZP/98mIbM4pk+pJHBnq9llD5VvYSHAAAAADawfJYAAAAAxCiQUwAAAADa9gFt6zuyPtf0uMUAAAA/3stcO299Uj5aLTeTAAAAAFsD0fQAAAA/",
        "7dcc9pqZGT8YsjBWzOKZPkE3nTivZZQ+kkImiwAAAAAJaYExAAAAAO15VWYAAAAASZxD9es7sj7urx/SAAAAP8sac4lvfVI+JzQwXgAAAAD89lGtAAAAP+6d",
        "jhSamRk/VXztDMzimT4uhv5Rr2WUPvuFdSurqio//NDINwAAAADXBNOaAAAAAMfZdQEAAAAAC0Cm3us7sj4c7leGAAAAP4EIfIlvfVI+rQET8QAAAABirAxe",
        "AAAAP9SG/x6amRk/I72ha8zimT5wEsSAr2WUPt9cyGsAAAAApEnsHwAAAAA2m40LAAAAAPRKiI7rO7I+5ZQsQgAAAD+MdWrDb31SPhjbCOEAAAAAAfqrLwAA",
        "AD8TneQQmpkZP/YTC1DM4pk+W7gxPK9llD6S7cDIAAAAAAmMb4EAAAAA7QJn/AAAAABJ77Di6zuyPu7gmswAAAA/yxOOXm99Uj4nvUH0AAAAAPwZQP0AAAA/",
        "7vD7AZqZGT9Vlc7gzOKZPi7hWN2vZZQ+bW2aswAAAABuL/L7AAAAAExzUl4AAAAAcrqjCes7sj6f7i0kAAAAP1YiuJlvfVI+UshzZAAAAACjlssUAAAAPyV3",
        "1weamRk/gHPnC8zimT75UiuBr2WUPhCMn8QAAAAAg99figAAAADjICf1AAAAAKeEKcnrO7I+ICnTTQAAAD+VwNs0b31SPskh+/wAAAAAJhfvfwAAAD9IW3i/",
        "mpkZP4eWFKvM4pk+3GhKH69llD5DuAbwAAAAAOC+8cYAAAAAQlMAgAAAAACQVFhp6zuyPllMCwoAAAA/QLC3b299Uj4E0i9XAAAAADXljMMAAAA/RwmgmZqZ",
        "GT+qgNfPzOKZPmd/KLGvZZQ+9lFAhQAAAAAFx7KBAAAAAAkQuhsAAAAA9a8GLOs7sj7iInJPAAAAP7/qOwpvfVI+A7uYFQAAAACgbmhlAAAAP3IsEiqamRk/",
        "ifSUt8zimT6K6ge7r2WUPmGvap0AAAAAqulsUQAAAACo3KhLAAAAAE5cxv/rO7I+AzDe+wAAAD/qWshkb31SPo5wflMAAAAAV+qlUQAAAD+51cZrmpkZP9Qk",
        "7wXM4pk+JULZAq9llD5emNA4q6oqP2nfoTMAAAAAcrPiHAAAAAAADTqcAAAAANby717rO7I+e5Oc+QAAAD8SxfWUb31SPhboH0AAAAAAL/D4BwAAAD8hRLgI",
        "mpkZP0x742DM4pk+LUkGJq9llD6i0PqEAAAAABmT/lIAAAAA3WOingAAAACZtZ316zuyPj61gsgAAAA/W6zHBW99Uj7Xhe3OAAAAAMxXKroAAAA/vvWTHpqZ",
        "GT/FtVbEzOKZPv5dGhKvZZQ+L8p8yAAAAAD0CuPqAAAAACaEIhwAAAAAxLUGj+s7sj71Pr5MAAAAP1w9BmpvfVI+CDZo3QAAAABR+EBqAAAAP+OSsIuamRk/",
        "JlXa1MzimT5ryiLsr2WUPmA3DwQAAAAA03u+KgAAAADTKNI4AAAAALe5eqbrO7I+cFJr2gAAAD+lt9nab31SPvnrvAUAAAAAthg9igAAAD/YKN5SmpkZP/dq",
        "/xLM4pk+rJO/+K9llD598Pg7AAAAAD64b7EAAAAA/MDnrgAAAACC7Ujj6zuyPi+eHz8AAAA/JupTQG99Uj5CI9NgAAAAADMSGAwAAAA/dVOkWZqZGT+wz8X5",
        "zOKZPslF+VWvZZQ+RjBObQAAAAAVhRusAAAAAPkmke4AAAAAxU3Uh+s7sj6y8GCyAAAAP4+y17BvfVI+s6JaIwAAAAAwRCHCAAAAPwKrwiCamRk/+WHug8zi",
        "mT7asz2Sr2WUPtPKwusAAAAA8N3JHQAAAAAyW6vDAAAAAKCixQHrO7I+6UPY8AAAAD9Qp7UVb31SPvRt8J8AAAAAhZlHsAAAAD8XAnkhmpkZP5p8ty3M4pk+",
        "dyU0Ca9llD40Mm5EAAAAAE84ifYAAAAAzyaeKAAAAABj2tr16zuyPqRtHt4AAAA/WfImh299Uj6lxkQoAAAAAMonU0EAAAA/7MNMo5qZGT+LujLfzOKZPkgN",
        "sNOvZZQ+kSGXMquqKj+ueYyhAAAAAP0g328AAAAAcQfpcwAAAABNUj166zuyPkp1sNYAAAA/N0Xn1m99Uj6bsvVaAAAAACjXhikAAAA/SsIIsZqZGT+xjGjy",
        "zOKZPgJSARevZZQ+5cx/ywAAAADGelnYAAAAAPQrGj8AAAAA6tfxAOs7sj5nQSnCAAAAP+7KE2ZvfVI+ymJGPAAAAABr9L0DAAAAP30nkHGamRk/iJvzKMzi",
        "mT6RrbOKr2WUPsiZYDYAAAAAW/EqNAAAAAArK1XbAAAAAD/20ybrO7I+KKwC4wAAAD/tzNYBb31SPkGdwHwAAAAAbqpqLQAAAD8gY7t8mpkZP69TTNvM4pk+",
        "9KX3569llD4Xi4zqAAAAAPwzOXQAAAAAnjYEvgAAAADMUk7N6zuyPs2Tzg4AAAA/pFIDkW99Uj6QKP2AAAAAAOmEgB4AAAA/C0gJRpqZGT+eDtyTzOKZPjOk",
        "waavZZQ+in4lggAAAACh1OyWAAAAAFWqyA8AAAAAQZgMj+s7sj62AckPAAAAP+M7OQtvfVI+P8/iawAAAABkc/TZAAAAP+ZsJoyamRk/XQGogMzimT5meLS0",
        "r2WUPvHjMLoAAAAAumefTQAAAABYhuIjAAAAAJ4OWdXrO7I+U0CF3wAAAD+6xAO7b31SPn6yR/MAAAAAJ2enYwAAAD9JgDDimpkZP4TEYO7M4pk+NeUCSa9l",
        "lD50yf+OAAAAAI95KQQAAAAAD5FqaAAAAACjQxMQ6zuyPuS9TgEAAAA/mTAnNm99Uj7l5V2BAAAAAAoLnDwAAAA/LBTCaJqZGT/L/TAwzOKZPoj5DayvZZQ+",
        "E0SNGgAAAAAwnqhSAAAAAHKLA0kAAAAA4DUDHOs7sj4pZMJsAAAAP5DltcRvfVI+NBODCQAAAADFNdQPAAAAP1f6ptCamRk/2mqLhMzimT63RP7xr2WUPpzv",
        "q6+rqio/Qwi0KQAAAADgjienAAAAAELDLzUAAAAAkCQbS+s7sj5ZPODVAAAAP0Ag0N9vfVI+BEJfDAAAAAA1tcKjAAAAP0fZYnuamRk/qvCNxMzimT5nz1rs",
        "r2WUPhDcTP4AAAAAg6+VagAAAADjkFaqAAAAAKdU7KrrO7I+IBmoGQAAAD+VMPSkb31SPsmRKrIAAAAAJuckYAAAAD9IKzuhmpkZP4cGy5/M4pk+3Lh8Wq9l",
        "lD5tvUftAAAAAG7/J9wAAAAATOOBEwAAAAByimbr6zuyPp/eAvAAAAA/VpLQCW99Uj5SOKMZAAAAAKNmAfUAAAA/JUea6ZqZGT+A450AzOKZPvmiXbyvZZQ+",
        "kj1uAgAAAAAJXKVhAAAAAO1ylrEAAAAASb9zxOs7sj7u0G+YAAAAP8uDps5vfVI+Jy1xqQAAAAD86XXdAAAAP+7AvuOamRk/VQWF1czimT4uMYsYr2WUPt+s",
        "daUAAAAApBkiAAAAAAA2C73AAAAAAPQaS3DrO7I+5YQBDgAAAD+M5YIzb31SPhhLOJYAAAAAAcrhDwAAAD8TbafympkZP/aDwUTM4pk+Wwhkd69llD78IHZx",
        "AAAAANfUCHsAAAAAx0mltgAAAAALEGnA6zuyPhzeLFIAAAA/gXiU+W99Uj6tcUKmAAAAAGJ8Qj4AAAA/1FbCAJqZGT8jLVhgzOKZPnBi9ruvZZQ+Weq/KgAA",
        "AACiWQuXAAAAANB1/qcAAAAAxox96us7sj4rQp7nAAAAP0Jtcl5vfVI+Jg4vJQAAAAAfgjCGAAAAP9F8pVaamRk/XCve6szimT5dAEHYr2WUPh5ySwsAAAAA",
        "LZ8CVgAAAAABsIjgAAAAAP1odInrO7I+egaBcgAAAD/nfwHyb31SPmuJlocAAAAAmNdvHAAAAD/6AQFPmpkZP0E9HiXM4pk+8lmKSq9llD4nbdYcq6oqP0hd",
        "cb8AAAAA2yA28AAAAACriWnuAAAAAL+dQo7rO7I+qG5mKAAAAD9tQ4qRb31SPsH71I8AAAAA7tl16QAAAD+gCirkmpkZPy8a6GDM4pk+dCExvK9llD5bwHpQ",
        "AAAAALik87kAAAAAKqlD6wAAAABIDNhA6zuyPgHJGcQAAAA/OIEWzG99Uj78Ru+vAAAAAL0kCYkAAAA/P6zLQZqZGT8SRTs1zOKZPt9k+cKvZZQ+Lj2dKgAA",
        "AAB9UOorAAAAAPFl/YYAAAAAzfmr4es7sj7KNxQcAAAAP7e7mmZvfVI+GxEKbgAAAACoBpLlAAAAP8ppdxiamRk/MVMEeMzimT6CzTrrr2WUPnGnQUMAAAAA",
        "OpeqCQAAAADY5PY2AAAAAB62xzzrO7I+0wLpJAAAAD86O7dKb31SPv4QXAYAAAAAp5ayHwAAAD/JJ59JmpkZPwSL/HPM4pk+tWA8Ha9llD70jBAYAAAAAA+p",
        "NMAAAAAAj+9+ewAAAAAj64F36zuyPmSAskYAAAA/GafaxW99Uj5lRHKUAAAAAIo6p/gAAAA/rLsw0JqZGT9LxMy1zOKZPgh1R4CvZZQ+l06dcwAAAAB8Y0Qw",
        "AAAAAB6VGNEAAAAATPq8NOs7sj5NVjJUAAAAPyTJtiBvfVI+EIcRlAAAAABptIvaAAAAP4vvd62amRk/HtV3GczimT6zH/t6r2WUPgpCNgsAAAAAIQT4UgAA",
        "AADVCN0iAAAAAME/e/brO7I+NsQsVQAAAD9jsuyab31SPr8t934AAAAA5KL/lQAAAD9mFJXzmpkZP93HQwbM4pk+5vPtiK9llD49MWIKAAAAAP55gR8AAAAA",
        "vCvkvwAAAABCmqtg6zuyPu+nTPEAAAA/5jEJf299Uj4CZEh0AAAAAPP7itwAAAA/NRYf/ZqZGT9w9WbDzOKZPonadoCvZZQ+GtKgNquqKj/o8ZKzAAAAAK9X",
        "b78AAAA/O4CDowAAAABenPkfAAAAPxLPPDgAAAAA8cyL4QAAAD8N/rf5AAAAALDTniwAAAA/zMnfJAAAAADDyq0PAAAAP19wvlYAAAAAwsntBQAAAD9WBI52",
        "AAAAAFXD3OcAAAA/gUojRQAAAADUjdF+AAAAP5pJikgAAAAAg85xVAAAAABk4a70AAAAAI3bqxwAAAAARhSOWAAAAADf029OAAAAACDbKykAAAAA2XTq4gAA",
        "AADfzlDiAAAAAEYPb+wAAAAAjdaMsAAAAABk3I+IAAAAAIPJUugAAAAAmkRr3AAAAAChlaeMAAAAAHhDucEAAAAAaPHy5wAAAADVQtwjAAAAAE5mQOAAAAAA",
        "i5WncAAAAADsOwrGAAAAAMmWCrAAAAAAAjs86wAAAACP+HETAAAAAH3ozZAAAAAAUJChZAAAAACPEVPnAAAAAIqj4PMAAAAA0Z3NcAAAAABUX0X/AAAAALNK",
        "1FkAAAAATn8htAAAAAAmjx71AAAAAL9Da5wAAAAAgNqbSQAAAAABOjLlAAAAAHreVFwAAAAA4417FgAAAADEOaxaAAAAAEXpQJkAAAAAO0pFdQAAAACyefMj",
        "AAAAAPnc6zEAAAAAmMK73gAAAAAXtxCIAAAAAL5roNQAAAAAhfz3TgAAAAAEqrRhAAAAAFTP+/MAAAAA0Q2EZQAAAACKE5foAAAAAI+BCdwAAAAAUABYWQAA",
        "AAB9WISFAAAAALYsKDIAAAAAix4/OQAAAAB5o4e3AAAAAFwACvkAAAAAuxDh+gAAAAA+MjxaAAAAAAXDk9QAAAAAGIlXZAAAAACXfawNAAAAADqWpQAAAAAA",
        "9sQOHwAAAABm6jYEAAAAALOg13oAAAAAmXud7d/OkD3XZeLtAAAAAJEVJdkAAAAA9dTmgAAAAABok5jfAAAAAP4niRTfzpA92s+3swAAAADciqR4AAAAABRr",
        "xigAAAAAmWrejwAAAAC/41Uk386QPT1dqPgAAAAA1zFTyQAAAADb4/iCAAAAAD434vMAAAAAHBpYJd/OkD14acpdAAAAAPKDTnIAAAAAMpiq5gAAAACneeck",
        "AAAAAO12CknfzpA9o1C0BAAAAADd3kPuAAAAAEFP7nIAAAAA3N7wUgAAAADSr3iS386QPSYPexgAAAAAOKdK1QAAAADg9zlcAAAAAE2dJt4AAAAAE6/U/d/O",
        "kD2ZQBi7AAAAAINKxeUAAAAAdxgj0AAAAACC7FX4AAAAACCTH+DfzpA9JAAGTAAAAAAx1OQdZmYmP/BVhpHxYwU/HbMxYArXIz2Xo8HcAAAAP//F6/mamZk+",
        "e6OlsJqZGT+MC7YdMzMTPyY9hMkzMzM/KjVrdwAAAD/MLO0gmpmZPmvwIIcYVjw+qNA078WI/D6hm4lgAACAP+qA+jQAAAAA6IpjCQAAgD9kyoSMs2sdPwSY",
        "6G3vN6c+KhKSmsIUxD462S1pAAAAAFHhO8UAAAAApd/LxAAAAAA94Z7V6zuyPicxQ8cu+eQ+E8dj6y755D5Ho8UGAAAAACyBEzYAAAAAboaFrgAAAABmU4a8",
        "6zuyPt4n6ZMu+eQ+1HAvcS755D74pjpoAAAAAIuAk5cAAAAAe9Q1GQAAAABfLkYq6zuyPqGitXQu+eQ+XYdj4S755D6VArnsAAAAAHbbu8kAAAAABPVP1gAA",
        "AABQ00VZ6zuyPuBffwsu+eQ+Huk/Ri755D5eTzn5AAAAAG0NzgcAAAAAQRqNFQAAAAAJ7x/M6zuyPlNlvTYu+eQ+JzSxty755D5FmsTXAACAPybc22EAAAAA",
        "1C5X9wAAQD9gq5Wjl08UPge4qL4AAAA/SrfVKMNagT51xPuwLvnkPqrO/fYAAAAADrBLsAAAAABdxBoHAAAAAFF3JCwAAAAAGW0zpes7sj7q/G7/AAAAP4XK",
        "+F3DWoE+pO8WXy755D57Hq0VAAAAALu8glUAAAAAGBAyVQAAAAAKGe7ZAAAAALJUkbPrO7I+oc/dWwAAAD+QGvDBw1qBPkuyzwIu+eQ+3CvUggAAAABUCFdE",
        "AAAAAG9MtY4AAAAAb7VyEQAAAAADcTR/6zuyPgTukdQAAAA/s5VlfMNagT5SlpYMLvnkPkXsng4AAAAA0d199AAAAACaPrVFAAAAALjkXocAAAAANBSVIes7",
        "sj7z4/V0AAAAP34+lP7DWoE+sSOIwy755D7ep0lUAAAAAOq77DoAAAAAAeKO6AAAAAA1Rn30AAAAAM3Ig8zrO7I+VhaWIgAAAD85tmjhw1qBPrCiqisu+eQ+",
        "H2cTUwAAAABgfPt/AAAAP78KajKbF8g+TtSMfArXoz1G9PLgAAAAP6QJiacAAIA+rrNP9wAAAD+bBFK7AAAAP419YAQAAIA+1axXOAAAAADnAiphmpkZP0o9",
        "MzDM4pk+h8oKXAWx2z7IonjRAAAAAFuiUFcAAAAAK55GoQAAAABPn1JA6zuyPjHrliwu+eQ+7Q9H/C755D7bBYJiAAAAADgmDiEAAAAAqr0gngAAAAAS3ltE",
        "6zuyPnodix8u+eQ+uE3TNi755D6ugqQ8AAAAAP3RBJMAAAAAcXraOQAAAAB5p9Cg6zuyPiOLvg0u+eQ+N4hX0S755D7x7EhVAAAAALoYxXAAAAAAWPnT6QAA",
        "AACU0kgE6zuyPhSo5oEu+eQ+ugd0tS755D500hcqAAAAAI8qTycAAAAADwRcLgAAAADjtTpa6zuyPiUC5EIu+eQ+mXOXMC755D4XlKSFAAAAAPzkXpcAAAAA",
        "nqn1gwAAAABW7D556zuyPu7q7h4u+eQ+pJVziy755D4WTp/dAAAAACVb0DcAAAAAqTe1GwAAAABxUwxO6zuyPkITcS8AAAA/7S0lWMNagT5cJdMQLvnkPqOX",
        "4C4AAAAADVxvSgAAAAAO46IvAAAAAGyYqMcAAAAA+IqH2+s7sj5/7wwEAAAAPzKPUPfDWoE+LXw7vC755D5yw0LQAAAAALBVA9kAAAAAI9CVngAAAAADau3l",
        "AAAAACeGcDjrO7I+gDqloQAAAD/XSOYJw1qBPl5Shm4u+eQ+6WrB7QAAAAD/YlXXAAAAAMS28UEAAAAA1maW+AAAAAAu/S3/6zuyPsXh9hwAAAA/jEMMzcNa",
        "gT5PVF5dLvnkPriExrQAAAAAsiOU2wAAAAApUxd9AAAAAI3frhUAAAAApdeSx+s7sj5OREsxAAAAP0Ef2IDDWoE+yBwJ3i755D7HLIsNAAAAAPlVe3cAAAAA",
        "QraujwAAAADw1Nu0AAAAAGyQHwfrO7I+i/2kGQAAAD8G+I4Zw1qBPikoK6Au+eQ+RgAOMgAAAABXzrXmAAAAP/ZwA9WbF8g+iylsYArXoz0dpgjcAAAAP4GV",
        "gYQAAIA+fXQeYwAAAD8i6SxYAAAAPyDLpRMAAIA+5F9pYgAAAABmoR0ImpkZP91OuNDM4pk+5nhePgWx2z6DfFTjAAAAACCvf6kAAAAAgrD8agAAAABKdwtE",
        "6zuyPkJmkNIu+eQ+gEvD6y755D5QKx7yAAAAAMNdS84AAAAAI0FrhwAAAAAHqKUp6zuyPjldXZgu+eQ+1e7osC755D6tDgsQAAAAAK5gOCoAAAAAjBVZxAAA",
        "AABYP3IU6zuyPniurxYu+eQ+llDFFS755D7SAEagAAAAAElGDacAAAAALasWzgAAAAAFACs16zuyPl8xMycu+eQ+C6+Z2i755D4fmEJyAAAAAOQZahgAAAAA",
        "dnpMygAAAACOGZ276zuyPhYsH8cu+eQ+zBB2Py755D48Xwg5AAAAABfxTlwAAAAAB0VtegAAAAC70AXo6zuyPi0sbq4u+eQ+wTaJBS755D6vYCF9AAAAAHTF",
        "HZ8AAAAApqPDaQAAAAD+TqEf6zuyPnUq1ysAAAA/vFdOP8NagT5/vdJeLvnkPohVCSsAAAAAzMY+sgAAAACntCKnAAAAAHdNSHwAAAAAKzb2aes7sj7s9xL4",
        "AAAAP+tc0DrDWoE+Ws2oOC755D6dzoEuAAAAAOl1RugAAAAA8m0d0QAAAACALNvpAAAAADy0Q+LrO7I++3612AAAAD82TXA5w1qBPhlk7yIu+eQ+lgfBjQAA",
        "AADGxvIhAAAAAJU/VmAAAAAAeUYyPAAAAADBxJIQ6zuyPjLceZEAAAA/3cOnucNagT6M0pYALvnkPjPC+3AAAAAAU2FnoAAAAABwmATSAAAAALJ6TBEAAAAA",
        "Oqarqus7sj5pL/HPAAAAP+hAoiTDWoE+UzCezy755D50jZHtAAAAAODNs7gAAAAAUzb53gAAAABTSHOGAAAAAPf91qLrO7I+8D2EuQAAAD9Hxr4+w1qBPo7Z",
        "VYUu+eQ+eQteUwAAAACuozlAAAAAP01U2hibF8g+jDYvzQrXoz0MBe9CAAAAPx4dDzQAAIA+kNi0pwAAAD+ZZNiKAAAAP+c8GFIAAIA+t1tg5gAAAACZvtcC",
        "mpkZPzR0EUDM4pk+hRsbgAWx2z5yNCk6AAAAAOmwNNQAAAAATdrb6QAAAABl0fHi6zuyPv9CDqEu+eQ+K2Iazi755D65oISQAAAAAALhX9YAAAAAsIRVogAA",
        "AAAsPIed6zuyPtwv35Uu+eQ+IhepXC755D5cvQHSAAAAADfNEq0AAAAAp3c2awAAAAAbaX/T6zuyPk3+Zn4u+eQ+YbXM9y755D6jDbqMAAAAAEC/RocAAAAA",
        "IqYNAwAAAACq714H6zuyPmJRxX0u+eQ+IF0I3i755D7WKCZKAAAAAOXckI4AAAAAaap2sAAAAAAxKgGj6zuyPmvSgO0u+eQ+H8zJeS755D7NHg0kAAAAAM6s",
        "ficAAAAALFlh4QAAAAC4JeOE6zuyPpjkrA0u+eQ+Ns8ICC755D4Q3juVAAAAAIOBkT0AAAAA48aUwQAAAADH1WYx6zuyPiA/6LMAAAA/d3t6U8NagT7+oS8t",
        "LvnkPsnHaMkAAAAAQwqjwAAAAADgYCN6AAAAAEL5bUwAAAAACuGudOs7sj5ZYiBwAAAAP9i6VP7DWoE+Q7cDSS755D4EeJ0jAAAAAPaj3FUAAAAABWnkNAAA",
        "AAAJtifoAAAAABHT73jrO7I+4jiHtQAAAD8NqZa8w1qBPnwhhXcu+eQ+A2EG4gAAAABZ7K7BAAAAAKIrB2oAAAAA0Ks8vwAAAACMoLLi6zuyPito3oEAAAA/",
        "JjOeusNagT7JyAeLLvnkPiZEbTwAAAAA/CJlCAAAAADXpgROAAAAAMd/480AAAAAe9/Vsus7sj4cBG3sAAAAP5s6kynDWoE+Cigu0y755D6tp4C9AAAAAN+u",
        "ZDwAAAAApOsd0wAAAAA2QfvXAAAAAE7Xt7brO7I+5apBqAAAAD8seyZyw1qBPm8MUMEu+eQ+GIF2rQAAAAB10i6jAAAAP/yejpybF8g+8aguhwrXoz1T6dmE",
        "AAAAP8OeE/4AAIA+B3P+PAAAAD/oXPHEAAAAPzJWRJAAAIA+btmITQAAAAAggk0gmpkZP69Q0GrM4pk+9Oy+1AWx2z7VwEjkAAAAALYfjRMAAAAAREcsMQAA",
        "AACQktkY6zuyPiD70lAu+eQ+XgKcWC755D6eesfwAAAAAK0b76IAAAAAgdXjcgAAAABJRTmJ6zuyPpPQl98u+eQ+Z00Nyi755D5Lz3tSAAAAACiEjXwAAAAA",
        "+gjNZQAAAABiY6eW6zuyPmrGQy0u+eQ+KK/pLi755D7kh05BAAAAAL/QJvoAAAAAn0BRXQAAAABzrNp16zuyPlW5nD8u+eQ+CWivKC755D7hB5S8AAAAACo2",
        "j34AAAAAKHIzkwAAAACk/O0w6zuyPkTFAK4u+eQ+apjsrC755D56efZEAAAAAJHO7uEAAAAA5Ynj9QAAAAB9ne726zuyPmekRMcu+eQ+U+C//S755D4xNqSQ",
        "AAAAAPqFesEAAAAAmHFVGAAAAADUOfW56zuyPpNryGUAAAA/Hn7OgMNagT7RRorRLvnkPr5/88sAAAAAyrVV6QAAAADhxzqWAAAAAJVpeVoAAAAA7SbmYus7",
        "sj7246F9AAAAP9n0f1PDWoE+UBOyby755D5/bEidAAAAAFeWEOYAAAAAPEeXXQAAAADemfU0AAAAAJZfgb3rO7I+DVe2PwAAAD8kvjrow1qBPtcLzVwu+eQ+",
        "0Nah3gAAAAAI71e5AAAAAJsKCWMAAAAAa9n5OAAAAACPdVwC6zuyPmiH06YAAAA/72SKDcNagT7mambPLvnkPoHWKvYAAAAAJf2niAAAAAAGKZ0SAAAAADSd",
        "BkQAAAAAgDk+4es7sj6nVCVyAAAAP2q2GdnDWoE+lZmGnS755D4KX/hcAAAAAO7+ycsAAAAAPZbvQgAAAACxIozRAAAAALmboX7rO7I+io1PMQAAAD+lJo5f",
        "w1qBPkSEyMgu+eQ+24Ll0QAAAABkut3qAAAAP2ugOAabF8g+YiZ0DQrXoz1itkXuAAAAP5j4IokAAIA+CiUngwAAAD8XUXmRAAAAPwnUcKIAAIA+iXNUbwAA",
        "AACraE2+mpkZPz7kX7HM4pk+UxVrwAWx2z4ceyH8AAAAAPcM5bcAAAAAZ1rVuwAAAADb8Xum6zuyPg3L6mAu+eQ+IRy32S755D5/i9cEAAAAAERToUQAAAAA",
        "VuwmRwAAAACuvhMj6zuyPnY3ms8u+eQ+rCrhFC755D4yTBYJAAAAAKnvxn8AAAAADWU/ZAAAAAAlmXjr6zuyPr9FJP4u+eQ+68gEsC755D6NhPF3AAAAAI5/",
        "UjIAAAAA7B05FgAAAAB4TG3/6zuyPljIWM8u+eQ+9jXz6S755D4wfoUGAAAAAKNsRaEAAAAAg+99NAAAAACnR1Zc6zuyPhngTDsu+eQ+NUEVhS755D5j8jpL",
        "AAAAAACRoUoAAAAA4iFXvwAAAABqi/kS6zuyPiK0ZgYu+eQ+4DDxvy755D7i1cmSAAAAAFm+bIIAAAAAHZXjaAAAAAA1lnvj6zuyPn5L87QAAAA/scvyWMNa",
        "gT54t2cqLvnkPhdo97QAAAAAqfqSqQAAAACycj4EAAAAAECI7u0AAAAA/Ix8l+s7sj67lKbmAAAAP/bCojzDWoE+2Sdlri755D5WHKWoAAAAAIzueTAAAAAA",
        "ZwT3wAAAAAA39S2XAAAAAOtEH9zrO7I+rCiHgwAAAD+repQow1qBPhpMFEku+eQ+Xd/hSwAAAAAT2x2hAAAAADCN7JkAAAAAcviqLgAAAAD6u9zG6zuyPikZ",
        "YEIAAAA/qH2gzMNagT4TnsqzLvnkPjSAKu8AAAAAhhqwOwAAAABVcXJ2AAAAADnBTLsAAAAAgRZd2Os7sj7yLURZAAAAP50GJqfDWoE+TFAjci755D7ziOgG",
        "AAAAAL0E4gcAAAAAfjm6qgAAAAA8OneSAAAAAIhlmVLrO7I+b5qGgQAAAD8iUDYHw1qBPt116A4u+eQ+gnLbRgAAAAC72+lIAAAAPxJURjabF8g+H1nr9grX",
        "oz3Zu3TiAAAAP+XjKVkAAIA+WZn5sQAAAD/OwznFAAAAP3zlB6MAAIA+SNPJ5QAAAADqKXnQmpkZP1GNUB7M4pk+IkM+AQWx2z6nlFqwAAAAAIxAkaIAAAAA",
        "TsPW3wAAAAAG9+826zuyPj5KokAu+eQ+tEKsjS755D4E/x0PAAAAAN+frasAAAAAP15QeQAAAABTP86H6zuyPnWSC0gu+eQ+qSDQMi755D6BEdXeAAAAAMrq",
        "NHUAAAAASHvvrQAAAAAE6uBE6zuyPuSOxyou+eQ+ioVKuC755D6+9Rq8AAAAAM2QqK8AAAAAIcBYSQAAAACpQM616zuyPrOd3TYu+eQ+BwYu1C755D5rDRfF",
        "AAAAAEhTFC4AAAAAmhWNVQAAAADCrDNI6zuyPopEM8Eu+eQ+yGcKOS755D7Y5TeVAAAAAGsEJY8AAAAA2y/OjgAAAAB/52XH6zuyPoG52LYu+eQ+vY0d/y75",
        "5D5bLghBAAAAALjCYaYAAAAAKkMdKgAAAACSn40R6zuyPgHzvXIAAAA/8ABU2sNagT4rwMbNLvnkPvzgyO4AAAAASMv+rwAAAADbPqTcAAAAAKsjQy0AAAAA",
        "z2CEDes7sj6omArXAAAAPy9O7DLDWoE+JpuOPC755D7Bla7OAAAAAGX+HUUAAAAARsjSgAAAAAB0JAiRAAAAAMABC/nrO7I+5y0xtgAAAD+qSpGVw1qBPtUW",
        "oM4u+eQ+Sls0jgAAAAAKsMP7AAAAACEiZj8AAAAA1aK2YQAAAAAtjQf86zuyPjbu0AMAAAA/GR8ez8NagT6QaJTcLvnkPr/H0L0AAAAAl7wqZAAAAAB8gbIc",
        "AAAAAB4v8g8AAAAA1q1wRus7sj5NgNYCAAAAP2SrIAvDWoE+F7AFjS755D4QIevSAAAAAPT6nQgAAAAAD8eirAAAAACPiVi6AAAAAGN3bCfrO7I+ZKpW9QAA",
        "AD+T1hW7w1qBPrK7Imou+eQ+Zd5L0wAAAADyOBd2AAAAP/lBHZGbF8g+ALK/EArXoz04CeZjAAAAP9LZpVoAAIA+bHGE6AAAAD/F94OEAAAAP8NZ6TgAAIA+",
        "C7VlcgAAAAAdpIW5mpkZPyjN5A7M4pk+sahAFQWx2z6Gsm0xAAAAAFXJpcsAAAAAOcl3gQAAAACBXtOx6zuyPhstdRcu+eQ+z6g+dS755D69nJ/9AAAAAH6R",
        "7f8AAAAAPEKiWAAAAACIrQ8s6zuyPogR13cu+eQ+ZuC6BC755D6gJDIqAAAAABOIt6YAAAAAE2WCZwAAAAC3om1z6zuyPkk3Fq0u+eQ+5Ro/ny755D5viVTl",
        "AAAAADShKJsAAAAAZlCQrAAAAAC+JeOW6zuyPibEv/0u+eQ+nDNtLi755D7ibYeIAAAAAFkWoNcAAAAAHZ0OLwAAAAA13vG86zuyPi8eBX0u+eQ+m6Iuyi75",
        "5D6pklCfAAAAALLKcVkAAAAAQJAZtAAAAAD81PJw6zuyPszUHKMu+eQ+UihbWS755D5MR2q4AAAAACdpN+sAAAAA9xrBlAAAAACrf3Gg6zuyPmxBji4AAAA/",
        "a937QMNagT7aFiRvLvnkPh2c+kYAAAAAL+FMgwAAAAD0eTLjAAAAACZxPIIAAAAAfpgcVus7sj71c1JiAAAAPzzYeUXDWoE+/wZOlS755D4II4JDAAAAAKLn",
        "yj8AAAAAGQJOSwAAAADdULwEAAAAAPVQK3zrO7I+PuoW3gAAAD9xcPUFw1qBPjhyF8Mu+eQ+13IHNQAAAAB9B8n2AAAAAD4nv6kAAAAA/K0BFQAAAABIBwEa",
        "6zuyPi/Ts1QAAAA/4nyk88NagT6dyVhRLvnkPkIQ7cYAAAAAYE7fvgAAAADT6g0jAAAAANMV7J4AAAAAd0dS2es7sj5wh//vAAAAP8dG6kTDWoE+DiPRuy75",
        "5D752NZrAAAAANPhkqYAAAAA8EwZFgAAAAAySMUpAAAAALrvJuHrO7I+6XhsBgAAAD9owc0qw1qBPtN5GQYu+eQ+9FoKBgAAAAABAAAAAP///wEBEAQAAAQA",
        "AAQAAAQAAAQAAAQAAAQAAAT//////////////////////////////////////////wD/////////AAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACY",
        "AAAAAQAEAggRAAACAgICAgICAgICAgICAgIBAgECAQECAQEBAgICAgICAgECAQIBAQIBAQECAgICAgICAQIBAgEBAgEBAQICAgICAgIBAgECAQECAQEBAgIC",
        "AgICAgECAQIBAQIBAQECAgICAgICAQIBAgEBAgEBAQICAgICAgIBAgECAQECAQEBAgICAgICAgECAQIBAQIBAQEwNwAAAQACAJE4dxUT00xXpHkSJRhCpPEA",
        "AQAA5hEAAHsidmVyc2lvbiI6IDIsICJpZCI6ICI5MTM4NzcxNS0xM2QzLTRjNTctYTQ3OS0xMjI1MTg0MmE0MDIiLCAicmV2aXNpb24iOiAiMSIsICJuYW1l",
        "IjogIkNvcHBlciBBcmMiLCAiaW50ZW50IjogIkEgZHJ5IHNhdyBhbmQgdHJpYW5nbGUgYW5hbG9nIHN0YXJ0aW5nIHZvaWNlIHdpdGggYSByb3VuZGVkIGZp",
        "bHRlciBjb250b3VyIGFuZCBzdWJ0bGUgc2xvdyBtb3Rpb24uIFNoYXBlIGJhc3MsIHBsdWNrLCBsZWFkLCBicmFzcyBhbmQgcGFkIHRocm91Z2ggb3JkaW5h",
        "cnkgbmF0aXZlIGNvbnRyb2xzLiIsICJyb2xlIjogInN5bnRoIiwgInVuaXRzIjogW3siaWQiOiAwLCAibW9kdWxlIjogImFuYWxvZyIsICJkZWZhdWx0cyI6",
        "IFt7InVuaXQiOiAwLCAia2V5IjogImVuYWJsZWQiLCAibm9ybWFsaXplZCI6IDEuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAiZW5naW5lIiwgIm5vcm1hbGl6",
        "ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogImxldmVsIiwgIm5vcm1hbGl6ZWQiOiAwLjY3Nzk5OTk3MzI5NzExOTE0MDYyNX0sIHsidW5pdCI6IDAs",
        "ICJrZXkiOiAicGFuIiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5vc2MuMS53YXZlZm9ybSIsICJub3JtYWxpemVk",
        "IjogMC42NjY2NjY2ODY1MzQ4ODE1OTE3OTY5fSwgeyJ1bml0IjogMCwgImtleSI6ICJhbmFsb2cub3NjLjEubGV2ZWwiLCAibm9ybWFsaXplZCI6IDAuNDYw",
        "MDAwMDA4MzQ0NjUwMjY4NTU0N30sIHsidW5pdCI6IDAsICJrZXkiOiAiYW5hbG9nLm9zYy4xLnR1bmUiLCAibm9ybWFsaXplZCI6IDAuNX0sIHsidW5pdCI6",
        "IDAsICJrZXkiOiAiYW5hbG9nLm9zYy4xLmRldHVuZSIsICJub3JtYWxpemVkIjogMC40ODUwMDAwMTQzMDUxMTQ3NDYwOTM4fSwgeyJ1bml0IjogMCwgImtl",
        "eSI6ICJhbmFsb2cub3NjLjEucHVsc2Vfd2lkdGgiLCAibm9ybWFsaXplZCI6IDAuNDU1NTU1NTU4MjA0NjUwODc4OTA2Mn0sIHsidW5pdCI6IDAsICJrZXki",
        "OiAiYW5hbG9nLm9zYy4yLndhdmVmb3JtIiwgIm5vcm1hbGl6ZWQiOiAwLjMzMzMzMzM0MzI2NzQ0MDc5NTg5ODR9LCB7InVuaXQiOiAwLCAia2V5IjogImFu",
        "YWxvZy5vc2MuMi5sZXZlbCIsICJub3JtYWxpemVkIjogMC4yMzk5OTk5OTQ2MzU1ODE5NzAyMTQ4fSwgeyJ1bml0IjogMCwgImtleSI6ICJhbmFsb2cub3Nj",
        "LjIudHVuZSIsICJub3JtYWxpemVkIjogMC41fSwgeyJ1bml0IjogMCwgImtleSI6ICJhbmFsb2cub3NjLjIuZGV0dW5lIiwgIm5vcm1hbGl6ZWQiOiAwLjUx",
        "NDk5OTk4NTY5NDg4NTI1MzkwNjJ9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5vc2MuMi5wdWxzZV93aWR0aCIsICJub3JtYWxpemVkIjogMC41NDQ0",
        "NDQ0NDE3OTUzNDkxMjEwOTM4fSwgeyJ1bml0IjogMCwgImtleSI6ICJhbmFsb2cubm9pc2UiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJr",
        "ZXkiOiAiYW5hbG9nLmZpbHRlci5tb2RlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5maWx0ZXIuY3V0b2ZmIiwg",
        "Im5vcm1hbGl6ZWQiOiAwLjU3NDczMTgyNjc4MjIyNjU2MjV9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5maWx0ZXIucmVzb25hbmNlIiwgIm5vcm1h",
        "bGl6ZWQiOiAwLjIzMTU3ODk0NjExMzU4NjQyNTc4MTJ9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5hbXAuYXR0YWNrIiwgIm5vcm1hbGl6ZWQiOiAw",
        "LjEwODAwNTM2NzIxOTQ0ODA4OTU5OTZ9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5hbXAuZGVjYXkiLCAibm9ybWFsaXplZCI6IDAuMjY4NTc5NTcy",
        "NDM5MTkzNzI1NTg1OX0sIHsidW5pdCI6IDAsICJrZXkiOiAiYW5hbG9nLmFtcC5zdXN0YWluIiwgIm5vcm1hbGl6ZWQiOiAwLjcyMDAwMDAyODYxMDIyOTQ5",
        "MjE4NzV9LCB7InVuaXQiOiAwLCAia2V5IjogImFuYWxvZy5hbXAucmVsZWFzZSIsICJub3JtYWxpemVkIjogMC4zMjAzNzIxMzQ0NDcwOTc3NzgzMjAzfSwg",
        "eyJ1bml0IjogMCwgImtleSI6ICJhbmFsb2cuZmlsdGVyX2Vudi5hdHRhY2siLCAibm9ybWFsaXplZCI6IDAuMTI1ODkyNTM0ODUyMDI3ODkzMDY2NH0sIHsi",
        "dW5pdCI6IDAsICJrZXkiOiAiYW5hbG9nLmZpbHRlcl9lbnYuZGVjYXkiLCAibm9ybWFsaXplZCI6IDAuMjgzNjc5MTI3NjkzMTc2MjY5NTMxMn0sIHsidW5p",
        "dCI6IDAsICJrZXkiOiAiYW5hbG9nLmZpbHRlcl9lbnYuc3VzdGFpbiIsICJub3JtYWxpemVkIjogMC4xODAwMDAwMDcxNTI1NTczNzMwNDY5fSwgeyJ1bml0",
        "IjogMCwgImtleSI6ICJhbmFsb2cuZmlsdGVyX2Vudi5yZWxlYXNlIiwgIm5vcm1hbGl6ZWQiOiAwLjMwOTI0OTQ5MDQ5OTQ5NjQ1OTk2MDl9LCB7InVuaXQi",
        "OiAwLCAia2V5IjogImFuYWxvZy5maWx0ZXJfZW52Lm9jdGF2ZXMiLCAibm9ybWFsaXplZCI6IDAuNTM3NTAwMDIzODQxODU3OTEwMTU2Mn0sIHsidW5pdCI6",
        "IDAsICJrZXkiOiAibGZvLjEuc2hhcGUiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAibGZvLjEuc3luY2VkIiwgIm5vcm1hbGl6",
        "ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogImxmby4xLnJhdGVfaHoiLCAibm9ybWFsaXplZCI6IDAuMDA1MDA2MjU3OTEwMjgxNDE5NzU0fSwgeyJ1",
        "bml0IjogMCwgImtleSI6ICJtb2R1bGF0aW9uLjEuZW5hYmxlZCIsICJub3JtYWxpemVkIjogMS4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJtb2R1bGF0aW9u",
        "LjEuc291cmNlIiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAia2V5IjogIm1vZHVsYXRpb24uMS50YXJnZXQiLCAibm9ybWFsaXplZCI6IDAu",
        "ODc1fSwgeyJ1bml0IjogMCwgImtleSI6ICJtb2R1bGF0aW9uLjEuYmFuayIsICJub3JtYWxpemVkIjogMC41fSwgeyJ1bml0IjogMCwgImtleSI6ICJtb2R1",
        "bGF0aW9uLjEuYW1vdW50IiwgIm5vcm1hbGl6ZWQiOiAwLjUxNzQ5OTk4MzMxMDY5OTQ2Mjg5MDZ9XSwgImFsZ29yaXRobV9yZXZpc2lvbiI6IDJ9XSwgInBh",
        "bmVsIjogeyJsYXlvdXQiOiAiaW5zdHJ1bWVudFNlY3Rpb25zIiwgInNlY3Rpb25zIjogWyJUb25lIiwgIkVudmVsb3BlIiwgIk1vdGlvbiJdLCAiY29udHJv",
        "bHMiOiBbeyJsYWJlbCI6ICJCcmlnaHRuZXNzIiwgInVuaXQiOiAwLCAic2VjdGlvbiI6IDAsICJrZXkiOiAiYW5hbG9nLmZpbHRlci5jdXRvZmYiLCAid2lk",
        "Z2V0IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsb2dhcml0aG1pYyIsICJtaW5pbXVtIjogMjAuMCwgIm1heGltdW0iOiAxNDAwMC4wfSwgeyJsYWJlbCI6",
        "ICJSZXNvbmFuY2UiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJhbmFsb2cuZmlsdGVyLnJlc29uYW5jZSIsICJ3aWRnZXQiOiAicm90YXJ5",
        "IiwgInJlc3BvbnNlIjogImxpbmVhciIsICJtaW5pbXVtIjogMC4wLCAibWF4aW11bSI6IDAuODUwMDAwMDIzODQxODU3OTEwMTU2Mn0sIHsibGFiZWwiOiAi",
        "Qm9keSIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAwLCAia2V5IjogImFuYWxvZy5vc2MuMi5sZXZlbCIsICJ3aWRnZXQiOiAicm90YXJ5IiwgInJlc3BvbnNl",
        "IjogImxpbmVhciIsICJtaW5pbXVtIjogMC4wLCAibWF4aW11bSI6IDAuNjQ5OTk5OTc2MTU4MTQyMDg5ODQzOH0sIHsibGFiZWwiOiAiQXR0YWNrIiwgInVu",
        "aXQiOiAwLCAic2VjdGlvbiI6IDEsICJrZXkiOiAiYW5hbG9nLmFtcC5hdHRhY2siLCAid2lkZ2V0IjogInZlcnRpY2FsU2xpZGVyIiwgInJlc3BvbnNlIjog",
        "ImxvZ2FyaXRobWljIiwgIm1pbmltdW0iOiAwLjAwMTAwMDAwMDA0NzQ5NzQ1MTMwNTQsICJtYXhpbXVtIjogMi4wfSwgeyJsYWJlbCI6ICJSZWxlYXNlIiwg",
        "InVuaXQiOiAwLCAic2VjdGlvbiI6IDEsICJrZXkiOiAiYW5hbG9nLmFtcC5yZWxlYXNlIiwgIndpZGdldCI6ICJ2ZXJ0aWNhbFNsaWRlciIsICJyZXNwb25z",
        "ZSI6ICJsb2dhcml0aG1pYyIsICJtaW5pbXVtIjogMC4wMTk5OTk5OTk1NTI5NjUxNjQxODQ2LCAibWF4aW11bSI6IDMuMH0sIHsibGFiZWwiOiAiQ29udG91",
        "ciIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAxLCAia2V5IjogImFuYWxvZy5maWx0ZXJfZW52LmRlY2F5IiwgIndpZGdldCI6ICJ2ZXJ0aWNhbFNsaWRlciIs",
        "ICJyZXNwb25zZSI6ICJsb2dhcml0aG1pYyIsICJtaW5pbXVtIjogMC4wMTk5OTk5OTk1NTI5NjUxNjQxODQ2LCAibWF4aW11bSI6IDIuMH0sIHsibGFiZWwi",
        "OiAiTW90aW9uIHJhdGUiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMiwgImtleSI6ICJsZm8uMS5yYXRlX2h6IiwgIndpZGdldCI6ICJyb3RhcnkiLCAicmVz",
        "cG9uc2UiOiAibG9nYXJpdGhtaWMiLCAibWluaW11bSI6IDAuMDUwMDAwMDAwNzQ1MDU4MDU5NjkyNCwgIm1heGltdW0iOiA1LjB9LCB7ImxhYmVsIjogIk1v",
        "dGlvbiIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAyLCAia2V5IjogIm1vZHVsYXRpb24uMS5hbW91bnQiLCAid2lkZ2V0IjogInJvdGFyeSIsICJyZXNwb25z",
        "ZSI6ICJsaW5lYXIiLCAibWluaW11bSI6IC0wLjI1LCAibWF4aW11bSI6IDAuMjV9XSwgInN1cmZhY2UiOiAibGlnaHQiLCAiYWNjZW50IjogIm5ldXRyYWwi",
        "LCAiZnJhbWUiOiAicm91bmRlZCIsICJlbXBoYXNpcyI6ICJiYWxhbmNlZCIsICJzcGFjaW5nIjogImNvbXBhY3QiLCAiZGVuc2l0eSI6ICJzcGFyc2UifX3M",
        "c8NqaW5LCZzz3NTfrBbxAAEAABYlAAB7InZlcnNpb24iOiAyLCAiaWQiOiAiY2M3M2MzNmEtNjk2ZS00YjA5LTljZjMtZGNkNGRmYWMxNmYwIiwgInJldmlz",
        "aW9uIjogIjEiLCAibmFtZSI6ICJMdW1lbiBGaWVsZCIsICJpbnRlbnQiOiAiQW4gb3JpZ2luYWwgc3BhY2lvdXMgc3RlcmVvIGhhbGwgd2l0aCBzb2Z0ZW5l",
        "ZCBoaWdoLWZyZXF1ZW5jeSBkZWNheSBhbmQgYSBnZW50bGUgd2V0IGxvdyBjdXQuIFNoYXBlIHRpbWUgd2l0aCBEZWNheSwgdG9uZSB3aXRoIFBvc3QgRVEg",
        "YW5kIGJ1c3kgcGFydHMgd2l0aCBkdWNraW5nLiIsICJyb2xlIjogImZ4IiwgInVuaXRzIjogW3siaWQiOiAwLCAibW9kdWxlIjogInNwYWNlX3JldmVyYiIs",
        "ICJkZWZhdWx0cyI6IFt7InVuaXQiOiAwLCAia2V5IjogImJ5cGFzc2VkIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNl",
        "X3JldmVyYi5zaXplIiwgIm5vcm1hbGl6ZWQiOiAwLjY0OTk5OTk3NjE1ODE0MjA4OTg0Mzh9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5k",
        "ZWNheV9zZWNvbmRzIiwgIm5vcm1hbGl6ZWQiOiAwLjQ3NTMzODk5NTQ1NjY5NTU1NjY0MDZ9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5w",
        "cmVkZWxheV9tcyIsICJub3JtYWxpemVkIjogMC4wOTYwMDAwMDA4MzQ0NjUwMjY4NTU1fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuYnJp",
        "Z2h0bmVzcyIsICJub3JtYWxpemVkIjogMC41fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuY2hhcmFjdGVyIiwgIm5vcm1hbGl6ZWQiOiAw",
        "LjMwMDAwMDAxMTkyMDkyODk1NTA3ODF9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kaXN0YW5jZSIsICJub3JtYWxpemVkIjogMC42MDAw",
        "MDAwMjM4NDE4NTc5MTAxNTYyfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIud2lkdGgiLCAibm9ybWFsaXplZCI6IDAuNTc0OTk5OTg4MDc5",
        "MDcxMDQ0OTIxOX0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLm1peCIsICJub3JtYWxpemVkIjogMC4yMTk5OTk5OTg4MDc5MDcxMDQ0OTIy",
        "fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZHVja19yYW5nZV9kYiIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6",
        "ICJzcGFjZV9yZXZlcmIuZHVja190aHJlc2hvbGRfZGIiLCAibm9ybWFsaXplZCI6IDAuNjAwMDAwMDIzODQxODU3OTEwMTU2Mn0sIHsidW5pdCI6IDAsICJr",
        "ZXkiOiAic3BhY2VfcmV2ZXJiLmR1Y2tfYXR0YWNrX21zIiwgIm5vcm1hbGl6ZWQiOiAwLjMwMDU1ODQ0NzgzNzgyOTU4OTg0Mzh9LCB7InVuaXQiOiAwLCAi",
        "a2V5IjogInNwYWNlX3JldmVyYi5kdWNrX3JlbGVhc2VfbXMiLCAibm9ybWFsaXplZCI6IDAuNDI5MDg0OTI2ODQzNjQzMTg4NDc2Nn0sIHsidW5pdCI6IDAs",
        "ICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjEuYWN0aXZlIiwgIm5vcm1hbGl6ZWQiOiAxLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVy",
        "Yi5kZWNheS4xLmJ5cGFzc2VkIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS4xLnNoYXBlIiwg",
        "Im5vcm1hbGl6ZWQiOiAxLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS4xLmZyZXFfaHoiLCAibm9ybWFsaXplZCI6IDAuNjE0",
        "OTI0NjA5NjYxMTAyMjk0OTIxOX0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjEubXVsdGlwbGllciIsICJub3JtYWxpemVkIjog",
        "MC4zMjY1OTg2MTQ0NTQyNjk0MDkxNzk3fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuMS5xIiwgIm5vcm1hbGl6ZWQiOiAwLjM4",
        "Mjk3MDg2OTU0MTE2ODIxMjg5MDZ9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS4yLmFjdGl2ZSIsICJub3JtYWxpemVkIjogMC4w",
        "fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuMi5ieXBhc3NlZCIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtl",
        "eSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuMi5zaGFwZSIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVj",
        "YXkuMi5mcmVxX2h6IiwgIm5vcm1hbGl6ZWQiOiAwLjM0ODExMzM4NzgyMzEwNDg1ODM5ODR9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5k",
        "ZWNheS4yLm11bHRpcGxpZXIiLCAibm9ybWFsaXplZCI6IDAuNDQ3MjEzNTkwMTQ1MTExMDgzOTg0NH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2",
        "ZXJiLmRlY2F5LjIucSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIu",
        "ZGVjYXkuMy5hY3RpdmUiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjMuYnlwYXNzZWQiLCAi",
        "bm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjMuc2hhcGUiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsi",
        "dW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjMuZnJlcV9oeiIsICJub3JtYWxpemVkIjogMC4zNDgxMTMzODc4MjMxMDQ4NTgzOTg0fSwg",
        "eyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuMy5tdWx0aXBsaWVyIiwgIm5vcm1hbGl6ZWQiOiAwLjQ0NzIxMzU5MDE0NTExMTA4Mzk4",
        "NDR9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS4zLnEiLCAibm9ybWFsaXplZCI6IDAuNDQ3MjEzNTkwMTQ1MTExMDgzOTg0NH0s",
        "IHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjQuYWN0aXZlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5Ijog",
        "InNwYWNlX3JldmVyYi5kZWNheS40LmJ5cGFzc2VkIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNh",
        "eS40LnNoYXBlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS40LmZyZXFfaHoiLCAibm9ybWFs",
        "aXplZCI6IDAuMzQ4MTEzMzg3ODIzMTA0ODU4Mzk4NH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjQubXVsdGlwbGllciIsICJu",
        "b3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuNC5xIiwgIm5vcm1h",
        "bGl6ZWQiOiAwLjQ0NzIxMzU5MDE0NTExMTA4Mzk4NDR9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS41LmFjdGl2ZSIsICJub3Jt",
        "YWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuNS5ieXBhc3NlZCIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1",
        "bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuNS5zaGFwZSIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFj",
        "ZV9yZXZlcmIuZGVjYXkuNS5mcmVxX2h6IiwgIm5vcm1hbGl6ZWQiOiAwLjM0ODExMzM4NzgyMzEwNDg1ODM5ODR9LCB7InVuaXQiOiAwLCAia2V5IjogInNw",
        "YWNlX3JldmVyYi5kZWNheS41Lm11bHRpcGxpZXIiLCAibm9ybWFsaXplZCI6IDAuNDQ3MjEzNTkwMTQ1MTExMDgzOTg0NH0sIHsidW5pdCI6IDAsICJrZXki",
        "OiAic3BhY2VfcmV2ZXJiLmRlY2F5LjUucSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJz",
        "cGFjZV9yZXZlcmIuZGVjYXkuNi5hY3RpdmUiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjYu",
        "YnlwYXNzZWQiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjYuc2hhcGUiLCAibm9ybWFsaXpl",
        "ZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLmRlY2F5LjYuZnJlcV9oeiIsICJub3JtYWxpemVkIjogMC4zNDgxMTMzODc4MjMx",
        "MDQ4NTgzOTg0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXkuNi5tdWx0aXBsaWVyIiwgIm5vcm1hbGl6ZWQiOiAwLjQ0NzIxMzU5",
        "MDE0NTExMTA4Mzk4NDR9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5kZWNheS42LnEiLCAibm9ybWFsaXplZCI6IDAuNDQ3MjEzNTkwMTQ1",
        "MTExMDgzOTg0NH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuMS5hY3RpdmUiLCAibm9ybWFsaXplZCI6IDEuMH0sIHsidW5pdCI6",
        "IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuMS5ieXBhc3NlZCIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9y",
        "ZXZlcmIucG9zdC4xLnNoYXBlIiwgIm5vcm1hbGl6ZWQiOiAwLjc1fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC4xLmZyZXFfaHoi",
        "LCAibm9ybWFsaXplZCI6IDAuMTQ0ODM0ODYxMTU5MzI0NjQ1OTk2MX0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuMS5nYWluX2Ri",
        "IiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjEuYmVsbF9xIiwgIm5vcm1hbGl6ZWQiOiAwLjI1",
        "MjY0NTU4MTk2MDY3ODEwMDU4NTl9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjEuc2hlbGZfcSIsICJub3JtYWxpemVkIjogMC40",
        "NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC4xLnNsb3BlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9",
        "LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjIuYWN0aXZlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5Ijog",
        "InNwYWNlX3JldmVyYi5wb3N0LjIuYnlwYXNzZWQiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3Qu",
        "Mi5zaGFwZSIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC4yLmZyZXFfaHoiLCAibm9ybWFsaXpl",
        "ZCI6IDAuMzQ4MTEzMzg3ODIzMTA0ODU4Mzk4NH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuMi5nYWluX2RiIiwgIm5vcm1hbGl6",
        "ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjIuYmVsbF9xIiwgIm5vcm1hbGl6ZWQiOiAwLjI1MjY0NTU4MTk2MDY3",
        "ODEwMDU4NTl9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjIuc2hlbGZfcSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUx",
        "MTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC4yLnNsb3BlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAw",
        "LCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjMuYWN0aXZlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVy",
        "Yi5wb3N0LjMuYnlwYXNzZWQiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuMy5zaGFwZSIsICJu",
        "b3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC4zLmZyZXFfaHoiLCAibm9ybWFsaXplZCI6IDAuMzQ4MTEz",
        "Mzg3ODIzMTA0ODU4Mzk4NH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuMy5nYWluX2RiIiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7",
        "InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjMuYmVsbF9xIiwgIm5vcm1hbGl6ZWQiOiAwLjI1MjY0NTU4MTk2MDY3ODEwMDU4NTl9LCB7",
        "InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjMuc2hlbGZfcSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwg",
        "eyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC4zLnNsb3BlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNw",
        "YWNlX3JldmVyYi5wb3N0LjQuYWN0aXZlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjQuYnlw",
        "YXNzZWQiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuNC5zaGFwZSIsICJub3JtYWxpemVkIjog",
        "MC4wfSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC40LmZyZXFfaHoiLCAibm9ybWFsaXplZCI6IDAuMzQ4MTEzMzg3ODIzMTA0ODU4",
        "Mzk4NH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuNC5nYWluX2RiIiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAi",
        "a2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjQuYmVsbF9xIiwgIm5vcm1hbGl6ZWQiOiAwLjI1MjY0NTU4MTk2MDY3ODEwMDU4NTl9LCB7InVuaXQiOiAwLCAi",
        "a2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjQuc2hlbGZfcSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwg",
        "ImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC40LnNsb3BlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5w",
        "b3N0LjUuYWN0aXZlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjUuYnlwYXNzZWQiLCAibm9y",
        "bWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuNS5zaGFwZSIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0",
        "IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9zdC41LmZyZXFfaHoiLCAibm9ybWFsaXplZCI6IDAuMzQ4MTEzMzg3ODIzMTA0ODU4Mzk4NH0sIHsidW5p",
        "dCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuNS5nYWluX2RiIiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNl",
        "X3JldmVyYi5wb3N0LjUuYmVsbF9xIiwgIm5vcm1hbGl6ZWQiOiAwLjI1MjY0NTU4MTk2MDY3ODEwMDU4NTl9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNl",
        "X3JldmVyYi5wb3N0LjUuc2hlbGZfcSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFj",
        "ZV9yZXZlcmIucG9zdC41LnNsb3BlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjYuYWN0aXZl",
        "IiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0LjYuYnlwYXNzZWQiLCAibm9ybWFsaXplZCI6IDAu",
        "MH0sIHsidW5pdCI6IDAsICJrZXkiOiAic3BhY2VfcmV2ZXJiLnBvc3QuNi5zaGFwZSIsICJub3JtYWxpemVkIjogMC4wfSwgeyJ1bml0IjogMCwgImtleSI6",
        "ICJzcGFjZV9yZXZlcmIucG9zdC42LmZyZXFfaHoiLCAibm9ybWFsaXplZCI6IDAuMzQ4MTEzMzg3ODIzMTA0ODU4Mzk4NH0sIHsidW5pdCI6IDAsICJrZXki",
        "OiAic3BhY2VfcmV2ZXJiLnBvc3QuNi5nYWluX2RiIiwgIm5vcm1hbGl6ZWQiOiAwLjV9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0",
        "LjYuYmVsbF9xIiwgIm5vcm1hbGl6ZWQiOiAwLjI1MjY0NTU4MTk2MDY3ODEwMDU4NTl9LCB7InVuaXQiOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5wb3N0",
        "LjYuc2hlbGZfcSIsICJub3JtYWxpemVkIjogMC40NDcyMTM1OTAxNDUxMTEwODM5ODQ0fSwgeyJ1bml0IjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucG9z",
        "dC42LnNsb3BlIiwgIm5vcm1hbGl6ZWQiOiAwLjB9XSwgImFsZ29yaXRobV9yZXZpc2lvbiI6IDF9XSwgInBhbmVsIjogeyJsYXlvdXQiOiAib3JkZXJlZEZ4",
        "U3RyaXAiLCAic2VjdGlvbnMiOiBbIlNwYWNlIl0sICJjb250cm9scyI6IFt7ImxhYmVsIjogIlNpemUiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtl",
        "eSI6ICJzcGFjZV9yZXZlcmIuc2l6ZSIsICJ3aWRnZXQiOiAicm90YXJ5IiwgInJlc3BvbnNlIjogImxpbmVhciIsICJtaW5pbXVtIjogMC4wLCAibWF4aW11",
        "bSI6IDEuMH0sIHsibGFiZWwiOiAiRGVjYXkiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuZGVjYXlfc2Vjb25kcyIs",
        "ICJ3aWRnZXQiOiAicm90YXJ5IiwgInJlc3BvbnNlIjogImxvZ2FyaXRobWljIiwgIm1pbmltdW0iOiAwLjEwMDAwMDAwMTQ5MDExNjExOTM4NDgsICJtYXhp",
        "bXVtIjogMjAuMH0sIHsibGFiZWwiOiAiUHJlZGVsYXkiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIucHJlZGVsYXlf",
        "bXMiLCAid2lkZ2V0IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsaW5lYXIiLCAibWluaW11bSI6IDAuMCwgIm1heGltdW0iOiAyNTAuMH0sIHsibGFiZWwi",
        "OiAiQnJpZ2h0bmVzcyIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5icmlnaHRuZXNzIiwgIndpZGdldCI6ICJyb3Rh",
        "cnkiLCAicmVzcG9uc2UiOiAibGluZWFyIiwgIm1pbmltdW0iOiAwLjAsICJtYXhpbXVtIjogMS4wfSwgeyJsYWJlbCI6ICJDaGFyYWN0ZXIiLCAidW5pdCI6",
        "IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIuY2hhcmFjdGVyIiwgIndpZGdldCI6ICJyb3RhcnkiLCAicmVzcG9uc2UiOiAibGluZWFy",
        "IiwgIm1pbmltdW0iOiAwLjAsICJtYXhpbXVtIjogMS4wfSwgeyJsYWJlbCI6ICJEaXN0YW5jZSIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAwLCAia2V5Ijog",
        "InNwYWNlX3JldmVyYi5kaXN0YW5jZSIsICJ3aWRnZXQiOiAicm90YXJ5IiwgInJlc3BvbnNlIjogImxpbmVhciIsICJtaW5pbXVtIjogMC4wLCAibWF4aW11",
        "bSI6IDEuMH0sIHsibGFiZWwiOiAiV2lkdGgiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJzcGFjZV9yZXZlcmIud2lkdGgiLCAid2lkZ2V0",
        "IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsaW5lYXIiLCAibWluaW11bSI6IDAuMCwgIm1heGltdW0iOiAyLjB9LCB7ImxhYmVsIjogIk1peCIsICJ1bml0",
        "IjogMCwgInNlY3Rpb24iOiAwLCAia2V5IjogInNwYWNlX3JldmVyYi5taXgiLCAid2lkZ2V0IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsaW5lYXIiLCAi",
        "bWluaW11bSI6IDAuMCwgIm1heGltdW0iOiAxLjB9XSwgInN1cmZhY2UiOiAiZGFyayIsICJhY2NlbnQiOiAiY29vbCIsICJmcmFtZSI6ICJyb3VuZGVkIiwg",
        "ImVtcGhhc2lzIjogImJhbGFuY2VkIiwgInNwYWNpbmciOiAiY29tcGFjdCIsICJkZW5zaXR5IjogInNwYXJzZSJ9fcm7WH3XXrcg3WDZn1weEWnyFbm3wZjZ",
        "jttubCsU03x0",
    };
    juce::String capturedBase64;
    for(const auto* part:capturedParts)capturedBase64+=part;
    CHECK(juce::Base64::convertFromBase64(captured,capturedBase64));
    const auto raw=captured.getMemoryBlock();
    CHECK(raw.getSize()==39969&&juce::SHA256(raw).toHexString()=="c1510e461f1b62686010e8ca5375a31c9313f6bc34f7e6eda47dc229f868a667");
    CHECK(read32(raw,4)==21&&read32(raw,8)==2831);
    SoundDocument recalled;const bool decoded=decodeSoundState(raw.getData(),raw.getSize(),recalled);CHECK(decoded);if(!decoded)return;
    const auto registry=parameterRegistry();const auto defaults=defaultParameterValues();
    const auto* original=static_cast<const unsigned char*>(raw.getData());
    constexpr std::size_t oldLocks=16+8*2831+78+64;
    for(std::size_t i=0;i<2831;++i){
        const auto target=i<2063?i:spaceReverbParameterStart+i-2063;
        CHECK(registry[target].stateKey==read32(raw,16+8*i));
        CHECK(std::bit_cast<std::uint32_t>(recalled.values[target])==read32(raw,20+8*i));
        CHECK(recalled.controlLocks[target]==(original[oldLocks+i]!=0));
    }
    for(std::size_t i=2063;i<spaceReverbParameterStart;++i)CHECK(recalled.values[i]==defaults[i]&&!recalled.controlLocks[i]);
    CHECK(recalled.personalities&&std::any_of(recalled.personalities->instances.begin(),recalled.personalities->instances.end(),[](const auto& instance){return std::any_of(instance.design.units.begin(),instance.design.units.end(),[](const auto& unit){return unit.module==NativeModule::spaceReverb;});}));
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t bank=0;bank<17;++bank)CHECK(recalled.compatibility.effects[slot][bank]==original[25649+20+17*slot+bank]);
    auto locked=raw;auto* changed=static_cast<unsigned char*>(locked.getData());changed[oldLocks+2063]=1;changed[oldLocks+2830]=1;sign(locked);
    SoundDocument withLocks;CHECK(decodeSoundState(locked.getData(),locked.getSize(),withLocks));
    CHECK(withLocks.controlLocks[spaceReverbParameterStart]&&withLocks.controlLocks[wavetableWarmthParameterStart-1]);
    for(std::size_t i=2063;i<spaceReverbParameterStart;++i)CHECK(!withLocks.controlLocks[i]);
    auto foreign=raw;write32(foreign,16,registry[dynamicEqExtensionParameterStart].stateKey);sign(foreign);withLocks=recalled;
    CHECK(!decodeSoundState(foreign.getData(),foreign.getSize(),withLocks)&&withLocks==recalled);
    juce::MemoryBlock upgraded;CHECK(encodeSoundState(recalled,upgraded)&&read32(upgraded,4)==currentSoundStateVersion&&read32(upgraded,8)==4519);
    CHECK(decodeSoundState(upgraded.getData(),upgraded.getSize(),withLocks)&&withLocks==recalled);
    // Independently project the accepted main layouts, including their 16-bank stride.
    for(const auto count:{std::size_t(2191),std::size_t(2215),std::size_t(2535)}){
        SoundDocument source;if(count==2535)source.name="Accepted named detector";for(std::size_t i=2063;i<std::min(count,std::size_t(2215));++i){source.values[i]=registry[i].kind==ParameterKind::continuous?.375f:1.f;source.controlLocks[i]=i%2==0;}
        juce::MemoryBlock current;const bool encoded=encodeSoundState(source,current);CHECK(encoded);if(!encoded)continue;
        const auto* bytes=static_cast<const char*>(current.getData());juce::MemoryOutputStream old;
        old.write(bytes,16+8*count);old.write(bytes+layoutOffset,78+64+count);old.write(bytes+currentSoundNumericBytes-12,12);
        old.writeInt(144);old.writeShort(1);old.writeByte(4);old.writeByte(2);old.writeByte(8);old.writeByte(16);old.writeShort(0);
        old.write(bytes+currentSoundNumericBytes+12,8);
        for(std::size_t slot=0;slot<8;++slot)old.write(bytes+currentSoundNumericBytes+20+18*slot,16);
        const auto personalities=currentSoundNumericBytes+4+kernelCompatibilitySectionBytes;
        old.write(bytes+personalities,current.getSize()-32-personalities-(count==2535?0:4));old.write(juce::MemoryBlock(32,true).getData(),32);
        auto accepted=old.getMemoryBlock();write32(accepted,4,count==2535?24:count==2191?21:22);write32(accepted,8,std::uint32_t(count));sign(accepted);
        CHECK(accepted.getSize()==(count==2535?oldSound24StateSize+source.name.getNumBytesAsUTF8():count==2191?oldSound21StateSize:oldSound22StateSize));
        SoundDocument out;CHECK(decodeSoundState(accepted.getData(),accepted.getSize(),out)&&out==source);
        auto bad=accepted;write32(bad,16,registry[spaceReverbParameterStart].stateKey);sign(bad);out=source;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out)&&out==source);
    }
}
}

int main(int argc,char** argv)
{
    if(argc==2&&juce::String(argv[1])=="--extensions"){shape_extension_sound20_and_frozen_sound19();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc!=1)return 2;
    shape_extension_sound20_and_frozen_sound19();
    mid_side_state_and_old_sound15_authority();
    frequency_split_state_and_old_sound14_authority();
    convolution_state_and_old_sound13_authority();
    frequency_shift_state_and_old_sound12_authority();
    dimension_state_and_old_sound11_authority();
    stereo_state_and_old_sound10_authority();
    flanger_state_and_old_sound9_authority();
    phaser_state_and_old_sound8_authority();
    chorus_state_and_old_sound7_authority();
    filter_state_and_old_sound6_authority();
    mapping_allocates_zero();
    compatibility_format_admission();
    algorithm_revision_transaction();
    modern_reverb_numeric_banks();
    eq_catalog_and_retained_state();exact_old_sound5_migration_and_domain();
    registry_is_fixed_and_complete();
    { all_core_controls_map(); bank_switch_preserves_inactive_values();
    reordered_state_roundtrips(); corrupt_state_has_no_mutation(); }
    calibratedGlobalsAndLegacyV2(); completeCatalogAndMapping(); strictDocumentCodec(); named_sound_state(); historicalM1Migration(); retainedBanksAndMacros(); localLockPolicy();
    nested_state_bounds();
    merged_main_and_pro_r_state_lineages();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures==0?0:1;
}
