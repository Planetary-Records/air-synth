#include "InstrumentProcessor.h"
#include "RackLibrary.h"
#include "HistoryRing.h"
#include "../../tests/native/OwnedFixture.h"
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
#include "CompanionRuntimeServer.h"
#include "PreviewRender.h"
#include "GenerationRequest.h"
#include "LocalPreview.h"
#include "LocalPreviewExchange.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>
#if JUCE_WINDOWS
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#endif

namespace airack {
// Tool-only observation on the control/render owner; never exports an asset owner.
struct InstrumentDrumTestAccess {
    static drums::AssetId preparedId(InstrumentProcessor& p,std::uint64_t ticket) {
        const std::lock_guard lock(p.restoreMutex);
        for(const auto& ready:p.drumOwnership->preparations_)
            if(ready.ticket==ticket&&!ready.running&&!ready.cancelled&&ready.asset)return ready.asset->id();
        throw std::runtime_error("ready drum preparation ticket missing");
    }
    static drums::ReferenceReport references(const InstrumentProcessor& p) {return p.engine.drumReferenceReport(0);}
    static void resetLocalObservations(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);
        p.aiLocalRenderObservationCount=0;p.aiLocalRenderObservationOverflow=false;p.aiLocalRenderObservations={};
        p.aiLocalProfileCallTrace=true;p.aiLocalFirstCallCompleted=false;}
    static bool firstLocalCallCompleted(const InstrumentProcessor& p){return p.aiLocalFirstCallCompleted.load(std::memory_order_acquire);}
    static unsigned localObservationCount(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.aiLocalRenderObservationCount;}
    template<class Visitor>static bool visitLocalObservations(const InstrumentProcessor& p,Visitor visitor){const std::lock_guard lock(p.restoreMutex);
        for(unsigned n=0;n<p.aiLocalRenderObservationCount;++n)visitor(p.aiLocalRenderObservations[n]);return p.aiLocalRenderObservationOverflow;}
    static auto compiledLocalSizes(){return std::array<std::size_t,5>{sizeof(InstrumentProcessor),sizeof(InstrumentProcessor::AiLocalWork),sizeof(ai::RenderResult),sizeof(ai::PreviewCallTrace),sizeof(InstrumentProcessor::AiLocalRenderObservation)};}
};
}

namespace
{
using namespace airack;
using Clock = std::chrono::steady_clock;
using Ns = std::int64_t;
using J = juce::var;
const std::array<juce::String,6> names {"single-analog","single-wavetable","full-analog-mixed","full-wavetable-mixed","full-delay","full-reverb"};
struct Stats { double minimum=0,mean=0,median=0,p95=0,p99=0,maximum=0,deadline=0; std::size_t misses=0; };
struct AudioResult { bool finite=true; double peak=0; std::uint64_t excursions=0,nonzero=0,checksum=14695981039346656037ull; };
struct Event { juce::String name; Ns requested=0,actual=0; bool boundary=false; };
struct Geometry { juce::String workload,kind; int rate=48000,block=128,count=1,trial=1; };
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
J object() { return J(new juce::DynamicObject); }
void put(J& o,const char* key,const J& value) { o.getDynamicObject()->setProperty(key,value); }
J integer(Ns n) { return J(juce::int64(n)); }
Ns elapsed(Clock::time_point start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(); }
std::size_t index(const juce::String&);
void set(SoundDocument& d,const juce::String& id,float physical)
{
    const auto i=index(id);const auto& p=parameterRegistry()[i];
    require(physical>=p.minimum&&physical<=p.maximum,"workload parameter outside descriptor range");
    d.values[i]=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(physical);
}
Stats statistics(const std::vector<Ns>& ns,double deadline)
{
    require(!ns.empty()&&std::isfinite(deadline)&&deadline>0,"invalid observations/deadline");
    require(std::all_of(ns.begin(),ns.end(),[](Ns n){return n>=0;}),"negative observation");
    auto sorted=ns;std::sort(sorted.begin(),sorted.end());
    const auto rank=[&](double q){return double(sorted[std::size_t(std::ceil(q*double(sorted.size())))-1]);};
    return {double(sorted.front()),std::accumulate(ns.begin(),ns.end(),0.0)/double(ns.size()),(sorted.size()%2?rank(.5):(double(sorted[sorted.size()/2-1])+double(sorted[sorted.size()/2]))/2),rank(.95),rank(.99),double(sorted.back()),deadline,
        std::size_t(std::count_if(ns.begin(),ns.end(),[&](Ns n){return double(n)>deadline;}))};
}
double deadlineNs(int rate,int block) { require(rate>0&&block>0,"invalid deadline geometry");return double(block)*1e9/double(rate); }
J summary(const std::vector<Ns>& ns,double deadline)
{
    const auto s=statistics(ns,deadline);auto j=object();
    put(j,"min_ns",s.minimum);put(j,"mean_ns",s.mean);put(j,"median_ns",s.median);put(j,"p95_ns",s.p95);put(j,"p99_ns",s.p99);put(j,"max_ns",s.maximum);
    put(j,"deadline_ns",s.deadline);put(j,"deadline_miss_count",integer(Ns(s.misses)));put(j,"mean_callback_load",s.mean/deadline);put(j,"max_callback_load",s.maximum/deadline);return j;
}
void scan(AudioResult& r,const juce::AudioBuffer<float>& audio)
{
    for(int c=0;c<audio.getNumChannels();++c)for(int i=0;i<audio.getNumSamples();++i){
        const float v=audio.getSample(c,i);const auto bits=std::bit_cast<std::uint32_t>(v);
        for(int b=0;b<4;++b)r.checksum=(r.checksum^((bits>>(8*b))&255u))*1099511628211ull;
        if(!std::isfinite(v)){r.finite=false;continue;}
        r.peak=std::max(r.peak,std::abs(double(v)));r.excursions+=std::abs(v)>1;r.nonzero+=v!=0;
    }
}
J audioJson(const AudioResult& a)
{
    auto j=object();put(j,"finite",a.finite);put(j,"peak",a.peak);put(j,"full_scale_excursions",integer(Ns(a.excursions)));put(j,"nonzero_samples",integer(Ns(a.nonzero)));
    put(j,"checksum_fnv1a64_float_bits",juce::String::toHexString(juce::int64(a.checksum)));return j;
}
void routes(SoundDocument& d,int rack,bool wt)
{
    const auto prefix="synth."+juce::String(rack+1);
    const std::array<ModTarget,8> targets{ModTarget::level,ModTarget::pan,ModTarget::cutoff,ModTarget::osc1Tune,ModTarget::osc2Detune,ModTarget::osc1Level,ModTarget::resonance,wt?ModTarget::osc1Position:ModTarget::osc1Width};
    for(int k=0;k<16;++k){const auto route=prefix+".modulation."+juce::String(k+1)+".";
        set(d,route+"enabled",1);set(d,route+"source",float(1+k%6));set(d,route+"target",float(targets[std::size_t(k%8)]));
        set(d,route+"bank",float(k%8<2?ModBank::common:(wt?ModBank::wavetable:ModBank::analog)));set(d,route+"amount",k%2?-.05f:.05f);
    }
}
void layoutEdit(SoundDocument& d,const RackEdit& edit) { require(editRackLayout(d.layout,edit,d.layout)==RackEditResult::applied,"invalid workload layout edit"); }
void eqControls(SoundDocument& d,bool edited=false)
{
    constexpr std::array<const char*,9> keys {"eq.low_shelf.frequency_hz","eq.low_shelf.gain_db","eq.low_shelf.q","eq.bell.frequency_hz","eq.bell.gain_db","eq.bell.q","eq.high_shelf.frequency_hz","eq.high_shelf.gain_db","eq.high_shelf.q"};
    for(int slot=0;slot<8;++slot){const float sign=slot%2?-1.f:1.f;
        const std::array<float,9> values {float((edited?35:20)+5*slot),sign*(edited?5.f:6.f),(edited?3.5f:4.f)-.2f*slot,float((edited?720:600)+180*slot),-sign*(edited?3.f:4.f),(edited?1.7f:1.5f)+.3f*slot,float((edited?6600:6000)+600*slot),sign*(edited?2.f:3.f),(edited?1.f:.8f)+.2f*slot};
        for(std::size_t field=0;field<keys.size();++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);
    }
}
void filterControls(SoundDocument& d,bool edited=false)
{
    // Distinct low-cutoff/high-Q stages retain a useful post-release pole tail;
    // this is a capacity workload, separate from the untouched factory default.
    constexpr std::array<const char*,5> keys {"filter_fx.mode","filter_fx.cutoff_hz","filter_fx.resonance","filter_fx.drive_db","filter_fx.mix"};
    for(int slot=0;slot<8;++slot){
        const std::array<float,5> values {edited?float(1+slot%2):0.f,float((edited?35:20)+5*slot),(edited?8.5f:9.f)-(edited?.2f:.25f)*slot,float((edited?5:6)+2*slot),(edited?.65f:.7f)+.03f*slot};
        for(std::size_t field=0;field<keys.size();++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);
    }
}
void chorusControls(SoundDocument& d,bool edited=false)
{
    constexpr std::array<const char*,5> keys {"chorus.rate_hz","chorus.depth","chorus.base_delay_ms","chorus.feedback","chorus.mix"};
    for(int slot=0;slot<8;++slot){const float sign=slot%2?-1.f:1.f;
        const std::array<float,5> values {edited?.35f+.22f*slot:.2f+.2f*slot,edited?.3f+.06f*slot:.2f+.05f*slot,float(edited?40-4*slot:12+4*slot),(edited?-sign:sign)*(.45f+.05f*slot),edited?.25f+.04f*slot:.3f+.05f*slot};
        for(std::size_t field=0;field<keys.size();++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);
    }
}
void phaserControls(SoundDocument& d,bool edited=false)
{
    constexpr std::array<const char*,5> keys {"phaser.rate_hz","phaser.depth","phaser.centre_hz","phaser.feedback","phaser.mix"};
    constexpr std::array<float,8> centres {20,80,250,650,1300,3500,9000,20000};
    for(int slot=0;slot<8;++slot){const float sign=slot%2?-1.f:1.f;
        const std::array<float,5> values {edited?.35f+.22f*slot:.2f+.2f*slot,edited?.3f+.06f*slot:.2f+.05f*slot,centres[std::size_t(edited?7-slot:slot)],(edited?-sign:sign)*(.45f+.05f*slot),edited?.25f+.04f*slot:.3f+.05f*slot};
        for(std::size_t field=0;field<keys.size();++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);
    }
}
void flangerControls(SoundDocument& d,bool edited=false)
{
    constexpr std::array<const char*,5> keys {"flanger.rate_hz","flanger.depth","flanger.base_delay_ms","flanger.feedback","flanger.mix"};
    for(int slot=0;slot<8;++slot){const float sign=slot%2?-1.f:1.f;
        const std::array<float,5> values {edited?.35f+.22f*slot:.2f+.3f*slot,edited?.3f+.06f*slot:.2f+.1f*slot,float(edited?8-slot:1+slot),(edited?-sign:sign)*(.45f+.05f*slot),edited?.25f+.04f*slot:.3f+.05f*slot};
        for(std::size_t field=0;field<keys.size();++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);
    }
}
void stereoControls(SoundDocument& d,bool edited=false)
{
    constexpr std::array<const char*,4> keys {"stereo.gain_db","stereo.pan","stereo.width","stereo.channel_mode"};
    for(int slot=0;slot<8;++slot){const float sign=slot%2?-1.f:1.f;
        const std::array<float,4> values {float((edited?-4:-6)+slot),(edited?-sign:sign)*(.1f+.08f*slot),edited?1.8f-.15f*slot:.5f+.1f*slot,float((slot+(edited?1:0))%3)};
        for(std::size_t field=0;field<keys.size();++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);
    }
}
void dimensionControls(SoundDocument& d,bool edited=false)
{
    constexpr const char* keys[]{"dimension.depth","dimension.rate_hz","dimension.width","dimension.mix"};
    for(int slot=0;slot<8;++slot){const std::array<float,4> values{edited?.85f-.05f*slot:.2f+.08f*slot,edited?1.7f-.15f*slot:.15f+.2f*slot,edited?1.8f-.1f*slot:.6f+.15f*slot,edited?.7f-.04f*slot:.25f+.06f*slot};for(std::size_t field=0;field<4;++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
}
void frequencyShiftControls(SoundDocument& d,bool edited=false)
{
    constexpr const char* keys[]{"frequency_shift.hz","frequency_shift.stereo_offset_hz","frequency_shift.mix"};
    for(int slot=0;slot<8;++slot){const float sign=slot%2?-1.f:1.f;const std::array<float,3> values{sign*(edited?-21.f-3*slot:7.f+.75f*slot),sign*(edited?-55.f+3*slot:12.f+4*slot),edited?.7f-.04f*slot:.25f+.06f*slot};for(std::size_t field=0;field<3;++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
}
void midSideSplitControls(SoundDocument& d,bool edited=false)
{
    constexpr const char* keys[]{"mid_side_split.mid_gain_db","mid_side_split.mid_mute","mid_side_split.mid_tone","mid_side_split.side_gain_db","mid_side_split.side_mute","mid_side_split.side_tone","mid_side_split.mix"};
    for(int slot=0;slot<8;++slot){const std::array<float,7> values{edited?1.f-.2f*slot:-3.f+.1f*slot,float((slot%3==0)!=edited),edited?.6f-.04f*slot:-.2f+.03f*slot,edited?-6.f+.2f*slot:2.f+.1f*slot,float((slot%3==1)!=edited),edited?-.5f+.02f*slot:.3f-.02f*slot,edited?.2f+.03f*slot:.8f-.04f*slot};for(std::size_t field=0;field<7;++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
}
void frequencySplitControls(SoundDocument& d,bool edited=false)
{
    constexpr const char* keys[]{"frequency_split.crossover_hz","frequency_split.low_gain_db","frequency_split.low_mute","frequency_split.low_tone","frequency_split.high_gain_db","frequency_split.high_mute","frequency_split.high_tone","frequency_split.mix"};
    for(int slot=0;slot<8;++slot){const std::array<float,8> values{edited?6000.f-173*slot:300.f+131*slot,edited?1.f-.2f*slot:-3.f+.1f*slot,float((slot%3==0)!=edited),edited?.6f-.04f*slot:-.2f+.03f*slot,edited?-6.f+.2f*slot:2.f+.1f*slot,float((slot%3==1)!=edited),edited?-.5f+.02f*slot:.3f-.02f*slot,edited?.2f+.03f*slot:.8f-.04f*slot};for(std::size_t field=0;field<8;++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
}
void convolutionControls(SoundDocument& d,bool edited=false)
{
    constexpr const char* keys[]{"convolution.ir","convolution.predelay_ms","convolution.wet_trim_db","convolution.mix"};
    for(int slot=0;slot<8;++slot){const std::array<float,4> values{float((slot+int(edited))%2),edited?float(140-slot*13):float(slot*17),edited?float(5-slot):float(-6+slot),edited?.7f-.04f*slot:.25f+.06f*slot};for(std::size_t field=0;field<4;++field)set(d,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
}
// Set once per run from --synth-algorithm, --drive-algorithm, --compressor-algorithm, --modulation-algorithm, --delay-algorithm, --eq-algorithm, --filter-algorithm and --frequency-split-algorithm; every workload document uses them.
AlgorithmRevision workloadSynthRevision=1,workloadDriveRevision=1,workloadCompressorRevision=1,workloadModulatedTapRevision=1,workloadDelayRevision=1,workloadEqRevision=1,workloadFilterRevision=1,workloadFrequencySplitRevision=1;
SoundDocument workload(const juce::String& name,AlgorithmRevision reverbRevision=1)
{
    const bool eqOnly=name=="full-eq",filterOnly=name=="full-filter",chorusOnly=name=="full-chorus",phaserOnly=name=="full-phaser",flangerOnly=name=="full-flanger",stereoOnly=name=="full-stereo",dimensionOnly=name=="full-dimension",frequencyShiftOnly=name=="full-frequency-shift",convolutionOnly=name=="full-convolution",frequencySplitOnly=name=="full-frequency-split",midSideSplitOnly=name=="full-mid-side-split",wavetableUnisonOnly=name=="full-wavetable-unison";const auto found=std::find(names.begin(),names.end(),name);require(eqOnly||filterOnly||chorusOnly||phaserOnly||flangerOnly||stereoOnly||dimensionOnly||frequencyShiftOnly||convolutionOnly||frequencySplitOnly||midSideSplitOnly||wavetableUnisonOnly||found!=names.end(),"unknown workload");
    // The unison workload is full-wavetable-mixed with revision-2 voices at their warmest.
    const auto n=wavetableUnisonOnly?std::size_t(3):midSideSplitOnly?std::size_t(16):frequencySplitOnly?std::size_t(15):convolutionOnly?std::size_t(14):frequencyShiftOnly?std::size_t(13):dimensionOnly?std::size_t(12):stereoOnly?std::size_t(11):flangerOnly?std::size_t(10):phaserOnly?std::size_t(9):chorusOnly?std::size_t(8):filterOnly?std::size_t(7):eqOnly?std::size_t(6):std::size_t(found-names.begin());const bool full=n>=2;SoundDocument d;
    for(auto& banks:d.compatibility.synth)banks.fill(workloadSynthRevision);
    if(full)for(std::uint8_t i=1;i<4;++i)layoutEdit(d,{RackEditKind::addSynth,i,masterChain,i});
    set(d,"global.voice_mode",float(VoiceMode::polyphonic));set(d,"global.master_level",.5f);set(d,"output.gain_db",full?12.0f:30.0f);set(d,"output.protection",1);
    for(int r=0;r<4;++r){const auto p="synth."+juce::String(r+1);const bool wt=n==1||n==3||(n>=4&&r%2==1);
        set(d,p+".enabled",full||r==0?1.0f:0.0f);set(d,p+".engine",wt?1.0f:0.0f);
        for(const auto& bank:{".analog.",".wavetable."}){
            const auto b=p+bank;set(d,b+"filter.cutoff",4000);set(d,b+"filter.resonance",.25f);set(d,b+"amp.attack",.005f);set(d,b+"amp.decay",.15f);set(d,b+"amp.sustain",1);set(d,b+"amp.release",.15f);
            for(int o=1;o<=2;++o){const auto osc=b+"osc."+juce::String(o)+".";if(juce::String(bank)==".analog.")set(d,osc+"waveform",float(Waveform::saw));else {set(d,osc+"table",0);set(d,osc+"position",.7f);}}
        }
        if(full){set(d,p+".lfo.1.shape",float(LfoShape::sine));set(d,p+".lfo.1.synced",0);set(d,p+".lfo.1.rate_hz",3);set(d,p+".lfo.2.shape",float(LfoShape::triangle));set(d,p+".lfo.2.synced",1);set(d,p+".lfo.2.beats",.25f);routes(d,r,wt);}
    }
    if(full)for(std::uint8_t f=0;f<8;++f){const auto kind=midSideSplitOnly?EffectKind::midSideSplit:frequencySplitOnly?EffectKind::frequencySplit:convolutionOnly?EffectKind::convolution:frequencyShiftOnly?EffectKind::frequencyShift:dimensionOnly?EffectKind::dimension:stereoOnly?EffectKind::stereo:flangerOnly?EffectKind::flanger:phaserOnly?EffectKind::phaser:chorusOnly?EffectKind::chorus:filterOnly?EffectKind::filter:eqOnly?EffectKind::eq:n==4?EffectKind::delay:n==5?EffectKind::reverb:static_cast<EffectKind>(f%4);
        const auto chain=std::uint8_t(n<4&&f<4?f:masterChain);layoutEdit(d,{RackEditKind::addEffect,f,chain,d.layout.effectCount[chain],kind});
        const auto p="effect."+juce::String(f+1)+".";
        if(n==4){set(d,p+"delay.seconds",4);set(d,p+"delay.feedback",.95f);set(d,p+"delay.mix",.35f);set(d,p+"delay.filter_hz",12000);set(d,p+"delay.stereo",.5f);set(d,p+"delay.synced",0);}
        if(n==5){set(d,p+"reverb.size",1);set(d,p+"reverb.decay_seconds",20);set(d,p+"reverb.damping",.5f);set(d,p+"reverb.predelay_ms",250);set(d,p+"reverb.mix",.35f);}
        if(kind==EffectKind::reverb)d.compatibility.effects[f][2]=reverbRevision;
        if(kind==EffectKind::drive)d.compatibility.effects[f][driveEffectBank]=workloadDriveRevision;
        if(kind==EffectKind::compressor)d.compatibility.effects[f][compressorEffectBank]=workloadCompressorRevision;
        if(kind==EffectKind::chorus||kind==EffectKind::flanger||kind==EffectKind::dimension)d.compatibility.effects[f][static_cast<std::size_t>(kind)]=workloadModulatedTapRevision;
        if(phaserOnly)d.compatibility.effects[f][7]=1;
        if(stereoOnly)d.compatibility.effects[f][9]=1;
    }
    if(eqOnly)eqControls(d);
    if(filterOnly)filterControls(d);
    if(chorusOnly)chorusControls(d);
    if(phaserOnly)phaserControls(d);
    if(flangerOnly)flangerControls(d);
    if(stereoOnly)stereoControls(d);
    if(midSideSplitOnly)for(int slot=0;slot<4;++slot)set(d,"synth."+juce::String(slot+1)+".pan",slot%2?.35f:-.35f);
    if(midSideSplitOnly){midSideSplitControls(d);for(auto& banks:d.compatibility.effects)banks[14]=1;}
    if(frequencySplitOnly){frequencySplitControls(d);for(auto& banks:d.compatibility.effects)banks[frequencySplitEffectBank]=workloadFrequencySplitRevision;}
    if(convolutionOnly){convolutionControls(d);for(auto& banks:d.compatibility.effects)banks[12]=1;}
    if(frequencyShiftOnly){frequencyShiftControls(d);for(auto& banks:d.compatibility.effects)banks[11]=1;}
    if(dimensionOnly){dimensionControls(d);for(auto& banks:d.compatibility.effects)banks[10]=workloadModulatedTapRevision;}
    if(wavetableUnisonOnly)for(int r=0;r<4;++r){
        d.compatibility.synth[std::size_t(r)][std::size_t(EngineKind::wavetable)]=modernSynthAlgorithmRevision;const auto w="synth."+juce::String(r+1)+".wavetable.";
        for(int o=1;o<=2;++o){const auto osc=w+"osc."+juce::String(o)+".";set(d,osc+"unison",parameterRegistry()[index(osc+"unison")].maximum);set(d,osc+"unison_cents",o==1?25.f:40.f);}
        set(d,w+"unison_width",1);set(d,w+"drift",1);set(d,w+"filter.type",float(r<2?FilterType::classic:FilterType::vowel));
    }
    // Every slot, as for a fresh instance, so lifecycle steps that switch slots to delays keep the selection.
    for(auto& banks:d.compatibility.effects){banks[delayEffectBank]=workloadDelayRevision;banks[eqEffectBank]=workloadEqRevision;banks[filterEffectBank]=workloadFilterRevision;}
    require(validateSoundDocument(d),"invalid workload document");return d;
}
constexpr std::pair<const char*,float> eraRecipe[]{{"voicing",1.f},{"age",0.f},{"decay_seconds",8.f},{"predelay_ms",25.f},{"size",.85f},{"attack",.6f},{"diffusion",1.f},{"wobble",.4f},
    {"wobble_rate_hz",.7f},{"grit",.15f},{"bass_multiplier",1.2f},{"damping_hz",5000.f},{"low_cut_hz",150.f},{"high_cut_hz",9000.f},{"mix",.3f}};
// full-reverb with its eight master slots switched to Era Reverb at the Small Hours recipe; everything else unchanged.
SoundDocument eraWorkload()
{
    auto d=workload("full-reverb");
    for(std::size_t f=0;f<8;++f){d.layout.effects[f].kind=EffectKind::eraReverb;for(const auto& [key,value]:eraRecipe)set(d,"effect."+juce::String(f+1)+".era_reverb."+key,value);}
    require(validateSoundDocument(d),"invalid Era Reverb workload document");return d;
}
std::vector<Event> events(int rate,int block)
{
    require(rate>0&&block>0,"invalid event geometry");std::vector<Event> result;
    const std::array<int,10> hundredths{10,25,40,70,100,125,150,160,170,185};
    const std::array<const char*,10> labels{"retrigger-four","automate-gain-cutoffs","replace-engines-effects","reverse-chains","restore-retrigger-sixteen","release-sixteen","midi-stop","resume-sixteen","sync-delays-remove-tempo","restore-tempo"};
    for(std::size_t i=0;i<10;++i){const Ns requested=(Ns(rate)*hundredths[i]+99)/100;const bool boundary=i==1||i==2||i==3||i==4||i==8||i==9;
        result.push_back({labels[i],requested,boundary?((requested+block-1)/block)*block:requested,boundary});}return result;
}
J documentJson(const SoundDocument& d)
{
    juce::MemoryBlock bytes;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;require(encodeSoundState(d,bytes)&&decodeSoundState(bytes.getData(),bytes.getSize(),decoded),"document export roundtrip failed");
    auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;require(makeRoutedPatch(decoded,patch),"exported document patch invalid");auto j=object();
    put(j,"encoded_base64",juce::Base64::toBase64(bytes.getData(),bytes.getSize()));put(j,"bytes",integer(Ns(bytes.getSize())));put(j,"sha256",juce::SHA256(bytes).toHexString());
    J values(juce::Array<J>{}),layers(juce::Array<J>{}),fx(juce::Array<J>{}),chains(juce::Array<J>{}),synthOrder(juce::Array<J>{});
    for(std::size_t i=0;i<parameterCount;++i){const auto& p=parameterRegistry()[i];auto v=object();put(v,"id",p.id);put(v,"key",integer(p.stateKey));put(v,"normalized",double(decoded.values[i]));put(v,"physical",double(parameterPhysicalValue(decoded.values,i)));put(v,"minimum",double(p.minimum));put(v,"maximum",double(p.maximum));values.append(v);}
    int activeRoutes=0;
    for(std::size_t r=0;r<4;++r){const auto& l=patch.synth.layers[r];auto layer=object();put(layer,"slot",int(r));put(layer,"use",int(decoded.layout.synthUse[r]));put(layer,"enabled",l.enabled);put(layer,"engine",int(l.engine));J list(juce::Array<J>{});
        for(const auto& route:l.modulation){const bool active=route.enabled&&route.amount!=0&&route.source!=ModSource::none&&route.target!=ModTarget::none&&(route.bank==ModBank::common||route.bank==(l.engine==EngineKind::analog?ModBank::analog:ModBank::wavetable));activeRoutes+=l.enabled&&active;
            auto v=object();put(v,"enabled",route.enabled);put(v,"source",int(route.source));put(v,"target",int(route.target));put(v,"bank",int(route.bank));put(v,"amount",double(route.amount));put(v,"active_bank_nonzero",active);list.append(v);}
        put(layer,"routes",list);layers.append(layer);synthOrder.append(int(decoded.layout.synthOrder[r]));}
    for(std::size_t f=0;f<8;++f){auto e=object();put(e,"slot",int(f));put(e,"use",int(decoded.layout.effects[f].use));put(e,"kind",int(decoded.layout.effects[f].kind));put(e,"chain",int(decoded.layout.effects[f].chain));put(e,"bypassed",patch.effects[f].bypassed);put(e,"algorithm_revision",int(patch.effects[f].algorithmRevision));J banks(juce::Array<J>{});for(auto revision:decoded.compatibility.effects[f])banks.append(int(revision));put(e,"bank_algorithm_revisions",banks);
        if(decoded.layout.effects[f].kind==EffectKind::eq){J bands(juce::Array<J>{});for(const auto& band:{patch.effects[f].eq.lowShelf,patch.effects[f].eq.bell,patch.effects[f].eq.highShelf}){auto b=object();put(b,"frequency_hz",double(band.frequencyHz));put(b,"gain_db",double(band.gainDb));put(b,"q",double(band.q));bands.append(b);}put(e,"eq_low_shelf_bell_high_shelf",bands);}
        if(decoded.layout.effects[f].kind==EffectKind::filter){const auto& filter=patch.effects[f].filter;auto settings=object();put(settings,"mode",int(filter.mode));put(settings,"cutoff_hz",double(filter.cutoffHz));put(settings,"resonance_q",double(filter.resonance));put(settings,"drive_db",double(filter.driveDb));put(settings,"mix",double(filter.mix));put(e,"filter_fx",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::chorus){const auto& chorus=patch.effects[f].chorus;auto settings=object();put(settings,"rate_hz",double(chorus.rateHz));put(settings,"depth",double(chorus.depth));put(settings,"base_delay_ms",double(chorus.baseDelayMs));put(settings,"feedback",double(chorus.feedback));put(settings,"mix",double(chorus.mix));put(e,"chorus",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::phaser){const auto& phaser=patch.effects[f].phaser;auto settings=object();put(settings,"rate_hz",double(phaser.rateHz));put(settings,"depth",double(phaser.depth));put(settings,"centre_hz",double(phaser.centreHz));put(settings,"feedback",double(phaser.feedback));put(settings,"mix",double(phaser.mix));put(e,"phaser",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::flanger){const auto& flanger=patch.effects[f].flanger;auto settings=object();put(settings,"rate_hz",double(flanger.rateHz));put(settings,"depth",double(flanger.depth));put(settings,"base_delay_ms",double(flanger.baseDelayMs));put(settings,"feedback",double(flanger.feedback));put(settings,"mix",double(flanger.mix));put(e,"flanger",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::stereo){const auto& stereo=patch.effects[f].stereo;auto settings=object();put(settings,"gain_db",double(stereo.gainDb));put(settings,"pan",double(stereo.pan));put(settings,"width",double(stereo.width));put(settings,"channel_mode",int(stereo.channelMode));put(e,"stereo",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::dimension){const auto& v=patch.effects[f].dimension;auto settings=object();put(settings,"depth",double(v.depth));put(settings,"rate_hz",double(v.rateHz));put(settings,"width",double(v.width));put(settings,"mix",double(v.mix));put(e,"dimension",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::frequencyShift){const auto& v=patch.effects[f].frequencyShift;auto settings=object();put(settings,"hz",double(v.hz));put(settings,"stereo_offset_hz",double(v.stereoOffsetHz));put(settings,"mix",double(v.mix));put(e,"frequency_shift",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::midSideSplit){const auto& v=patch.effects[f].midSideSplit;auto settings=object();put(settings,"mid_gain_db",double(v.midGainDb));put(settings,"mid_mute",v.midMuted);put(settings,"mid_tone",double(v.midTone));put(settings,"side_gain_db",double(v.sideGainDb));put(settings,"side_mute",v.sideMuted);put(settings,"side_tone",double(v.sideTone));put(settings,"mix",double(v.mix));put(e,"mid_side_split",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::frequencySplit){const auto& v=patch.effects[f].frequencySplit;auto settings=object();put(settings,"crossover_hz",double(v.crossoverHz));put(settings,"low_gain_db",double(v.lowGainDb));put(settings,"low_mute",v.lowMuted);put(settings,"low_tone",double(v.lowTone));put(settings,"high_gain_db",double(v.highGainDb));put(settings,"high_mute",v.highMuted);put(settings,"high_tone",double(v.highTone));put(settings,"mix",double(v.mix));put(e,"frequency_split",settings);}
        if(decoded.layout.effects[f].kind==EffectKind::convolution){const auto& v=patch.effects[f].convolution;auto settings=object();put(settings,"ir",int(v.ir));put(settings,"predelay_ms",double(v.predelayMs));put(settings,"wet_trim_db",double(v.wetTrimDb));put(settings,"mix",double(v.mix));put(e,"convolution",settings);}
        fx.append(e);}
    for(std::size_t c=0;c<5;++c){J order(juce::Array<J>{});for(int k=0;k<decoded.layout.effectCount[c];++k)order.append(int(decoded.layout.effectOrder[c][std::size_t(k)]));chains.append(order);}
    put(j,"descriptor_values",values);put(j,"synths",layers);put(j,"synth_order",synthOrder);put(j,"synth_count",int(decoded.layout.synthCount));put(j,"effects",fx);put(j,"chains",chains);put(j,"active_nonzero_routes",activeRoutes);return j;
}
struct Playhead final:juce::AudioPlayHead
{
    bool tempo=true,playing=true;
    juce::Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setIsPlaying(playing);if(tempo)p.setBpm(120);return p;}
};
struct Observation {Ns ns=0;AudioResult audio;bool missingTempo=false;};
struct Instance
{
    Playhead playhead;std::unique_ptr<InstrumentProcessor> processor;
    juce::AudioBuffer<float> audio;juce::MidiBuffer midi;std::vector<Observation> observations;
    Ns construct=0,restore=0,prepare=0,release=0,destroy=0;
    Instance(int block,std::size_t blocks):audio(2,block),observations(blocks) {midi.ensureSize(4096);auto t=Clock::now();processor=std::make_unique<InstrumentProcessor>();construct=elapsed(t);processor->setPlayHead(&playhead);}
};
struct EventRecord {Event event;Ns operation=0;J midi{juce::Array<J>{}},document;};
void notes(juce::MidiBuffer& midi,int count,int offset,bool on,J* trace=nullptr)
{
    for(int k=0;k<count;++k){midi.addEvent(on?juce::MidiMessage::noteOn(1,48+k,juce::uint8(100)):juce::MidiMessage::noteOff(1,48+k),offset);
        if(trace){auto j=object();put(j,"type",on?"note_on":"note_off");put(j,"channel",1);put(j,"note",48+k);put(j,"velocity",on?100:0);put(j,"sample_offset",offset);trace->append(j);}}
}
SoundDocument snapshot(InstrumentProcessor& p,std::uint64_t& revision) {SoundDocument d;require(p.trySoundSnapshot(d,revision),"snapshot unavailable");return d;}
void manualEdit(InstrumentProcessor& p,const SoundDocument& d,std::uint64_t revision) {require(p.applySoundEdit(d,SoundEditOrigin::manual,revision),"manual edit rejected");}
void performEvent(Instance& instance,EventRecord& record,Ns frame,const juce::MemoryBlock& baseline,bool reverbEvidence=false,bool eqEvidence=false,bool filterEvidence=false,bool chorusEvidence=false,bool phaserEvidence=false,bool flangerEvidence=false,bool stereoEvidence=false,bool dimensionEvidence=false,bool frequencyShiftEvidence=false,bool convolutionEvidence=false,bool frequencySplitEvidence=false,bool midSideSplitEvidence=false,const SoundDocument* supplied=nullptr)
{
    auto& p=*instance.processor;auto& midi=instance.midi;const auto& e=record.event;const int offset=int(e.actual-frame);
    const auto t=Clock::now();
    if(e.name=="retrigger-four"){notes(midi,4,offset,false,&record.midi);notes(midi,4,offset,true,&record.midi);}
    else if(e.name=="release-sixteen")notes(midi,16,offset,false,&record.midi);
    else if(e.name=="resume-sixteen")notes(midi,16,offset,true,&record.midi);
    else if(e.name=="midi-stop"){midi.addEvent(juce::MidiMessage::midiStop(),offset);auto j=object();put(j,"type","midi_stop");put(j,"sample_offset",offset);record.midi.append(j);}
    else if(e.name=="restore-retrigger-sixteen"){require(p.restoreSoundState(baseline.getData(),int(baseline.getSize())),"baseline recall failed");notes(midi,16,0,true,&record.midi);}
    else if(e.name=="restore-tempo"){
        instance.playhead.tempo=true;
        if(supplied){std::uint64_t revision=0;auto d=snapshot(p,revision);for(std::size_t n=0;n<8;++n)d.layout.effects[n].kind=supplied->layout.effects[n].kind;manualEdit(p,d,revision);}
        else
        if(reverbEvidence||eqEvidence||filterEvidence||chorusEvidence||phaserEvidence||flangerEvidence||stereoEvidence||dimensionEvidence||frequencyShiftEvidence||convolutionEvidence||frequencySplitEvidence||midSideSplitEvidence){std::uint64_t revision=0;auto d=snapshot(p,revision);for(auto& fx:d.layout.effects)fx.kind=midSideSplitEvidence?EffectKind::midSideSplit:frequencySplitEvidence?EffectKind::frequencySplit:convolutionEvidence?EffectKind::convolution:frequencyShiftEvidence?EffectKind::frequencyShift:dimensionEvidence?EffectKind::dimension:stereoEvidence?EffectKind::stereo:flangerEvidence?EffectKind::flanger:phaserEvidence?EffectKind::phaser:chorusEvidence?EffectKind::chorus:filterEvidence?EffectKind::filter:eqEvidence?EffectKind::eq:EffectKind::reverb;manualEdit(p,d,revision);}
    }
    else {
        std::uint64_t revision=0;auto d=snapshot(p,revision);
        if(e.name=="automate-gain-cutoffs"){
            set(d,"output.gain_db",6);const auto gain=index("output.gain_db");p.control(gain).setValueNotifyingHost(d.values[gain]);
            for(int r=0;r<4;++r){const auto prefix="synth."+juce::String(r+1);const bool wt=parameterPhysicalValue(d.values,index(prefix+".engine"))==1;const auto id=prefix+(wt?".wavetable.":".analog.")+"filter.cutoff";set(d,id,2000);const auto i=index(id);p.control(i).setValueNotifyingHost(d.values[i]);}
            if(eqEvidence){eqControls(d,true);for(std::size_t i=eqParameterStart;i<eqParameterStart+8*eqParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(filterEvidence){filterControls(d,true);for(std::size_t i=filterParameterStart;i<filterParameterStart+8*filterParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(chorusEvidence){chorusControls(d,true);for(std::size_t i=chorusParameterStart;i<chorusParameterStart+8*chorusParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(phaserEvidence){phaserControls(d,true);for(std::size_t i=phaserParameterStart;i<phaserParameterStart+8*phaserParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(flangerEvidence){flangerControls(d,true);for(std::size_t i=flangerParameterStart;i<flangerParameterStart+8*flangerParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(stereoEvidence){stereoControls(d,true);for(std::size_t i=stereoParameterStart;i<stereoParameterStart+8*stereoParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(dimensionEvidence){dimensionControls(d,true);for(std::size_t i=dimensionParameterStart;i<dimensionParameterStart+8*dimensionParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(frequencyShiftEvidence){frequencyShiftControls(d,true);for(std::size_t i=frequencyShiftParameterStart;i<frequencyShiftParameterStart+8*frequencyShiftParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(midSideSplitEvidence){midSideSplitControls(d,true);for(std::size_t i=midSideSplitParameterStart;i<parameterCount;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(frequencySplitEvidence){frequencySplitControls(d,true);for(std::size_t i=frequencySplitParameterStart;i<parameterCount;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
            if(convolutionEvidence){convolutionControls(d,true);for(std::size_t i=convolutionParameterStart;i<convolutionParameterStart+8*convolutionParametersPerSlot;++i)p.control(i).setValueNotifyingHost(d.values[i]);}
        }else if(e.name=="replace-engines-effects"){
            for(int r=0;r<4;++r){const auto id="synth."+juce::String(r+1)+".engine";const bool wt=parameterPhysicalValue(d.values,index(id))==0;set(d,id,wt?1.0f:0.0f);routes(d,r,wt);}
            for(auto& fx:d.layout.effects)fx.kind=(eqEvidence||filterEvidence||chorusEvidence||phaserEvidence||flangerEvidence||stereoEvidence||dimensionEvidence||frequencyShiftEvidence||convolutionEvidence||frequencySplitEvidence||midSideSplitEvidence)?EffectKind::compressor:static_cast<EffectKind>((int(fx.kind)+1)%4);manualEdit(p,d,revision);
        }else if(e.name=="reverse-chains"){
            for(std::size_t c=0;c<5;++c)std::reverse(d.layout.effectOrder[c].begin(),d.layout.effectOrder[c].begin()+d.layout.effectCount[c]);manualEdit(p,d,revision);
        }else if(e.name=="sync-delays-remove-tempo"){
            // Dedicated family workloads select delay banks for the unavailable
            // tempo oracle, then restore their selected retained family.
            if(supplied||reverbEvidence||eqEvidence||filterEvidence||chorusEvidence||phaserEvidence||flangerEvidence||stereoEvidence||dimensionEvidence||frequencyShiftEvidence||convolutionEvidence||frequencySplitEvidence||midSideSplitEvidence)for(auto& fx:d.layout.effects)fx.kind=EffectKind::delay;
            for(int f=0;f<8;++f)set(d,"effect."+juce::String(f+1)+".delay.synced",1);manualEdit(p,d,revision);instance.playhead.tempo=false;
        }else throw std::runtime_error("unrecognized lifecycle event");
    }
    record.operation=elapsed(t);
    if(e.boundary){std::uint64_t revision=0;const auto current=snapshot(p,revision);record.document=documentJson(current);require(int(record.document["active_nonzero_routes"])==64,"lifecycle lost active route capacity");
        if(eqEvidence||filterEvidence||chorusEvidence||phaserEvidence||flangerEvidence||stereoEvidence||dimensionEvidence||frequencyShiftEvidence||convolutionEvidence||frequencySplitEvidence||midSideSplitEvidence){const auto expected=e.name=="replace-engines-effects"||e.name=="reverse-chains"?EffectKind::compressor:e.name=="sync-delays-remove-tempo"?EffectKind::delay:midSideSplitEvidence?EffectKind::midSideSplit:frequencySplitEvidence?EffectKind::frequencySplit:convolutionEvidence?EffectKind::convolution:frequencyShiftEvidence?EffectKind::frequencyShift:dimensionEvidence?EffectKind::dimension:stereoEvidence?EffectKind::stereo:flangerEvidence?EffectKind::flanger:phaserEvidence?EffectKind::phaser:chorusEvidence?EffectKind::chorus:filterEvidence?EffectKind::filter:EffectKind::eq;const auto bank=midSideSplitEvidence?14:frequencySplitEvidence?13:convolutionEvidence?12:frequencyShiftEvidence?11:dimensionEvidence?10:stereoEvidence?9:flangerEvidence?8:phaserEvidence?7:chorusEvidence?6:filterEvidence?5:4;const AlgorithmRevision retained=modulatedTapEffectBank(std::size_t(bank))?workloadModulatedTapRevision:std::size_t(bank)==eqEffectBank?workloadEqRevision:std::size_t(bank)==filterEffectBank?workloadFilterRevision:std::size_t(bank)==frequencySplitEffectBank?workloadFrequencySplitRevision:legacyAlgorithmRevision;RoutedPatch patch;require(makeRoutedPatch(current,patch),"selected-family lifecycle snapshot invalid");for(std::size_t slot=0;slot<8;++slot)require(current.layout.effects[slot].use==SlotUse::present&&current.layout.effects[slot].kind==expected&&!patch.effects[slot].bypassed&&patch.effects[slot].algorithmRevision==(expected==EffectKind::delay?workloadDelayRevision:expected==EffectKind::compressor?legacyAlgorithmRevision:retained)&&current.compatibility.effects[slot][std::size_t(bank)]==retained,"lifecycle lost selected/restored family or retained revision");}
        if(chorusEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Chorus lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")chorusControls(*expected,true);for(std::size_t i=chorusParameterStart;i<chorusParameterStart+8*chorusParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored Chorus settings");}
        if(phaserEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Phaser lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")phaserControls(*expected,true);for(std::size_t i=phaserParameterStart;i<phaserParameterStart+8*phaserParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored Phaser settings");}
        if(flangerEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Flanger lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")flangerControls(*expected,true);for(std::size_t i=flangerParameterStart;i<flangerParameterStart+8*flangerParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored Flanger settings");}
        if(stereoEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Stereo lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")stereoControls(*expected,true);for(std::size_t i=stereoParameterStart;i<stereoParameterStart+8*stereoParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored Stereo settings");}
        if(dimensionEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Dimension lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")dimensionControls(*expected,true);for(std::size_t i=dimensionParameterStart;i<dimensionParameterStart+8*dimensionParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored Dimension settings");}
        if(frequencyShiftEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"FrequencyShift lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")frequencyShiftControls(*expected,true);for(std::size_t i=frequencyShiftParameterStart;i<frequencyShiftParameterStart+8*frequencyShiftParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored FrequencyShift settings");}
        if(midSideSplitEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Mid side split lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")midSideSplitControls(*expected,true);for(std::size_t i=midSideSplitParameterStart;i<parameterCount;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored mid side split settings");}
        if(frequencySplitEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Frequency split lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")frequencySplitControls(*expected,true);for(std::size_t i=frequencySplitParameterStart;i<parameterCount;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored frequency split settings");}
        if(convolutionEvidence){auto expected=std::make_unique<SoundDocument>();require(decodeSoundState(baseline.getData(),baseline.getSize(),*expected),"Convolution lifecycle baseline decode failed");if(e.name=="automate-gain-cutoffs"||e.name=="replace-engines-effects"||e.name=="reverse-chains")convolutionControls(*expected,true);for(std::size_t i=convolutionParameterStart;i<convolutionParameterStart+8*convolutionParametersPerSlot;++i)require(current.values[i]==expected->values[i],"lifecycle changed selected/inactive/restored Convolution settings");}
    }
}

// Task13 development-only callback scope. No helper priority/affinity change.
// Release wall measurements are separate from matching Debug CRT evidence.
thread_local bool step7Enabled=false,step7Tracking=false;
thread_local std::uint64_t step7Calls=0,step7Allocations=0,step7Frees=0;
#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl step7AllocationHook(int kind,void*,size_t,int,long,const unsigned char*,int)
{if(step7Tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++step7Allocations;if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++step7Frees;}return 1;}
#endif
struct Step7AuditScope
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CRT_ALLOC_HOOK previous=nullptr;
#endif
    Step7AuditScope(){require(!step7Enabled,"nested Step7 callback audit");step7Enabled=true;
#if defined(_MSC_VER) && defined(_DEBUG)
        previous=_CrtSetAllocHook(step7AllocationHook);
#endif
    }
    ~Step7AuditScope(){step7Tracking=false;
#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtSetAllocHook(previous);
#endif
        step7Enabled=false;
    }
};
void step7Process(InstrumentProcessor& p,juce::AudioBuffer<float>& a,juce::MidiBuffer& m)
{
    if(!step7Enabled){p.processBlock(a,m);return;}
    ++step7Calls;
#if defined(_MSC_VER) && defined(_DEBUG)
    require(!step7Tracking,"nested audio callback");step7Tracking=true;
    try{p.processBlock(a,m);}catch(...){step7Tracking=false;throw;}
    step7Tracking=false;
#else
    p.processBlock(a,m);
#endif
}
void step7ResetAudit(){step7Calls=step7Allocations=step7Frees=0;}
J step7AuditResult()
{
    auto j=object();put(j,"callback_invocations",integer(Ns(step7Calls)));
    put(j,"scope","Prepared processBlock only, including first/warmup, timed, lifecycle/recall/Stop, continuous release/tail and return-to-RT/oracle calls. Setup/control/report/IO/background threads excluded by TLS.");
#if defined(_MSC_VER) && defined(_DEBUG)
    put(j,"status","OBSERVED_DEBUG_CRT");put(j,"allocations",integer(Ns(step7Allocations)));put(j,"frees",integer(Ns(step7Frees)));put(j,"zero",step7Calls>0&&step7Allocations==0&&step7Frees==0);
#else
    put(j,"status","UNAVAILABLE_RELEASE_TIMING_ONLY");put(j,"zero",false);
#endif
    return j;
}

Ns framesFor(double seconds,int rate,int block) {return Ns(std::ceil(seconds*rate/block))*block;}
J runTrial(const Geometry& g,RenderQuality quality=RenderQuality::realtime,AlgorithmRevision reverbRevision=0,bool bypassEffects=false,const SoundDocument* supplied=nullptr)
{
    const auto setupStart=Clock::now();
    require((g.kind=="steady"||g.kind=="lifecycle"||g.kind=="multi")&&g.count>0&&g.count<=8,"invalid trial kind/count");
    auto baseline=supplied?*supplied:workload(g.workload,reverbRevision?reverbRevision:1);baseline.values[758]=quality==RenderQuality::higher?1.f:0.f;
    if(bypassEffects)for(int slot=1;slot<=8;++slot)set(baseline,"effect."+juce::String(slot)+".bypassed",1);
    const auto document=documentJson(baseline);const bool full=g.workload.startsWith("full-");
    require(int(document["synth_count"])==(full?4:1)&&int(document["active_nonzero_routes"])==(full?64:0),"decoded capacity failure");
    const int noteCount=full?16:1;const Ns measured=framesFor(g.kind=="lifecycle"?2:1,g.rate,g.block),warmup=framesFor(g.workload=="full-delay"?4.25:.25,g.rate,g.block);
    const auto blocks=std::size_t(measured/g.block);std::vector<std::unique_ptr<Instance>> instances;instances.reserve(std::size_t(g.count));
    std::vector<Ns> serial(g.kind=="multi"?blocks:0);std::vector<EventRecord> trace;
    if(g.kind=="lifecycle")for(const auto& e:events(g.rate,g.block))trace.push_back({e});
    juce::MemoryBlock encoded;require(encodeSoundState(baseline,encoded),"baseline encoding failed");
    J initialMidi(juce::Array<J>{});
    for(int i=0;i<g.count;++i){auto instance=std::make_unique<Instance>(g.block,blocks);auto t=Clock::now();require(instance->processor->restoreSoundState(encoded.getData(),int(encoded.getSize())),"setup restore failed");instance->restore=elapsed(t);
        instance->processor->setNonRealtime(quality==RenderQuality::higher);
        t=Clock::now();instance->processor->prepareToPlay(g.rate,g.block);instance->prepare=elapsed(t);
        require(instance->processor->selectedRenderQuality()==quality&&instance->processor->effectiveRenderQuality()==quality&&!instance->processor->renderQualityPending(),"Profile effective mode differs from request");
        notes(instance->midi,noteCount,0,true,i==0?&initialMidi:nullptr);instances.push_back(std::move(instance));}
    const auto setupWall=elapsed(setupStart);auto warmStart=Clock::now();
    for(Ns frame=0;frame<warmup;frame+=g.block)for(auto& p:instances){step7Process(*p->processor,p->audio,p->midi);p->midi.clear();}
    const auto warmWall=elapsed(warmStart);
    // Prepared buffers/MIDI/observations only in this loop. Audio inspection follows all timed callbacks.
    std::size_t nextEvent=0;
    for(std::size_t b=0;b<blocks;++b){const Ns frame=Ns(b)*g.block;
        for(auto& p:instances)p->midi.clear();
        while(nextEvent<trace.size()&&trace[nextEvent].event.actual<frame+g.block){performEvent(*instances[0],trace[nextEvent],frame,encoded,reverbRevision!=0,g.workload=="full-eq",g.workload=="full-filter",g.workload=="full-chorus",g.workload=="full-phaser",g.workload=="full-flanger",g.workload=="full-stereo",g.workload=="full-dimension",g.workload=="full-frequency-shift",g.workload=="full-convolution",g.workload=="full-frequency-split",g.workload=="full-mid-side-split",supplied);++nextEvent;}
        if(g.kind=="multi"){
            const auto start=Clock::now();
            for(auto& p:instances){const auto t=Clock::now();step7Process(*p->processor,p->audio,p->midi);p->observations[b].ns=elapsed(t);}
            serial[b]=elapsed(start);
        }else {auto& p=*instances[0];const auto t=Clock::now();step7Process(*p.processor,p.audio,p.midi);p.observations[b].ns=elapsed(t);}
        for(auto& p:instances){scan(p->observations[b].audio,p->audio);p->observations[b].missingTempo=p->processor->tempoUnavailable();}
    }
    require(nextEvent==trace.size(),"unexecuted lifecycle event");
    auto result=object();put(result,"workload",g.workload);put(result,"kind",g.kind);put(result,"sample_rate",g.rate);put(result,"block_size",g.block);put(result,"instance_count",g.count);put(result,"trial",g.trial);
    put(result,"measured_frames",integer(measured));put(result,"measured_seconds",double(measured)/g.rate);put(result,"warmup_frames",integer(warmup));put(result,"warmup_seconds",double(warmup)/g.rate);put(result,"warmup_wall_ns",integer(warmWall));put(result,"document",document);put(result,"initial_note_count",noteCount);put(result,"initial_midi",initialMidi);
    put(result,"setup_before_warmup_ns",integer(setupWall));
    put(result,"selected_quality",int(quality));put(result,"effective_quality",int(instances[0]->processor->effectiveRenderQuality()));put(result,"host_latency_samples",instances[0]->processor->getLatencySamples());put(result,"host_non_realtime",instances[0]->processor->isNonRealtime());
    J callbacks(juce::Array<J>{}),audio(juce::Array<J>{}),summaries(juce::Array<J>{}),lifecycleTimes(juce::Array<J>{}),tempo(juce::Array<J>{});
    bool finite=true,missing=false,recovered=false;double peak=0;Ns excursions=0,nonzero=0;
    for(auto& p:instances){J raw(juce::Array<J>{}),rawAudio(juce::Array<J>{}),missingFlags(juce::Array<J>{});std::vector<Ns> ns;ns.reserve(blocks);
        for(std::size_t b=0;b<blocks;++b){const auto& o=p->observations[b];ns.push_back(o.ns);raw.append(integer(o.ns));rawAudio.append(audioJson(o.audio));missingFlags.append(o.missingTempo);
            finite=finite&&o.audio.finite;peak=std::max(peak,o.audio.peak);excursions+=Ns(o.audio.excursions);nonzero+=Ns(o.audio.nonzero);
            if(g.kind=="lifecycle"){const Ns frame=Ns(b)*g.block;if(frame>=trace[8].event.actual&&frame<trace[9].event.actual)missing=missing||o.missingTempo;if(frame>=trace[9].event.actual)recovered=recovered||!o.missingTempo;}
        }
        callbacks.append(raw);audio.append(rawAudio);tempo.append(missingFlags);summaries.append(summary(ns,deadlineNs(g.rate,g.block)));
        auto t=Clock::now();p->processor->releaseResources();p->release=elapsed(t);t=Clock::now();p->processor.reset();p->destroy=elapsed(t);
        auto times=object();put(times,"construct_ns",integer(p->construct));put(times,"setup_restore_ns",integer(p->restore));put(times,"prepare_ns",integer(p->prepare));put(times,"release_ns",integer(p->release));put(times,"destroy_ns",integer(p->destroy));lifecycleTimes.append(times);
    }
    put(result,"callback_ns",callbacks);put(result,"callback_summaries",summaries);put(result,"audio_observations",audio);put(result,"tempo_unavailable",tempo);put(result,"lifecycle_times",lifecycleTimes);
    J serialJson(juce::Array<J>{});for(const auto ns:serial)serialJson.append(integer(ns));put(result,"serial_ns",serialJson);if(!serial.empty())put(result,"serial_summary",summary(serial,deadlineNs(g.rate,g.block)));
    J eventJson(juce::Array<J>{}),labels(juce::Array<J>{});
    for(const auto& record:trace){auto e=object();put(e,"name",record.event.name);put(e,"requested_frame",integer(record.event.requested));put(e,"actual_frame",integer(record.event.actual));put(e,"block_index",integer(record.event.actual/g.block));put(e,"sample_offset",integer(record.event.actual%g.block));put(e,"control_at_boundary",record.event.boundary);put(e,"operation_ns",integer(record.operation));put(e,"midi",record.midi);if(!record.document.isVoid())put(e,"document_after",record.document);
        const auto immediate=std::size_t(record.event.actual/g.block);Ns maximum=0;const Ns end=record.event.actual+Ns(std::ceil(.02*g.rate));
        for(std::size_t b=immediate;b<blocks&&Ns(b)*g.block<end;++b)maximum=std::max(maximum,instances[0]->observations[b].ns);
        put(e,"window_end_frame_exclusive",integer(end));put(e,"immediate_callback_ns",integer(instances[0]->observations[immediate].ns));put(e,"transition_window_max_ns",integer(maximum));eventJson.append(e);}
    for(std::size_t b=0;b<blocks;++b){J tag(juce::Array<J>{});for(const auto& record:trace)if(Ns(b)*g.block<record.event.actual+Ns(std::ceil(.02*g.rate))&&(Ns(b)+1)*g.block>record.event.actual)tag.append(record.event.name);labels.append(tag);}
    put(result,"events",eventJson);put(result,"event_count",int(trace.size()));put(result,"event_window_labels",labels);put(result,"raw_finite",finite);put(result,"raw_peak",peak);put(result,"raw_full_scale_excursions",integer(excursions));put(result,"raw_nonzero_samples",integer(nonzero));put(result,"missing_tempo_observed",missing);put(result,"recovered_tempo_observed",recovered);
    put(result,"integrity",finite&&peak>0&&peak<=std::pow(10.0,-1.0/20.0)+1e-6&&nonzero>0&&(g.kind!="lifecycle"||(missing&&recovered)));
    const auto drain=Clock::now();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);put(result,"message_drain_ns",integer(elapsed(drain)));
    return result;
}
void validateTrial(const J& report)
{
    require(report.isObject(),"report must be object");const int rate=int(report["sample_rate"]),block=int(report["block_size"]),count=int(report["instance_count"]);const auto kind=report["kind"].toString();
    require((rate==44100||rate==48000||rate==96000)&&(block==64||block==128||block==256||block==512||block==1024),"bad report geometry");
    require((kind=="steady"||kind=="lifecycle"||kind=="multi")&&(count==1||(kind=="multi"&&(count==4||count==8))),"bad kind/count");
    const auto name=report["workload"].toString();require(name=="full-era-reverb"||name=="full-eq"||name=="full-filter"||name=="full-chorus"||name=="full-phaser"||name=="full-flanger"||name=="full-stereo"||name=="full-dimension"||name=="full-frequency-shift"||name=="full-convolution"||name=="full-frequency-split"||name=="full-mid-side-split"||name=="full-wavetable-unison"||std::find(names.begin(),names.end(),name)!=names.end(),"bad report workload");
    const Ns frames=framesFor(kind=="lifecycle"?2:1,rate,block),warmup=framesFor(name=="full-delay"?4.25:.25,rate,block);const int blocks=int(frames/block);
    require(juce::int64(report["measured_frames"])==frames&&juce::int64(report["warmup_frames"])==warmup&&report["callback_ns"].size()==count&&report["audio_observations"].size()==count,"inconsistent observations/frames");
    for(int i=0;i<count;++i){require(report["callback_ns"][i].size()==blocks&&report["audio_observations"][i].size()==blocks,"inconsistent callback count");std::vector<Ns> ns;
        for(int b=0;b<blocks;++b){const auto n=report["callback_ns"][i][b];require(n.isInt64()||n.isInt(),"noninteger nanoseconds");ns.push_back(juce::int64(n));const auto a=report["audio_observations"][i][b];require(bool(a["finite"])&&std::isfinite(double(a["peak"])),"nonfinite audio observation");}statistics(ns,deadlineNs(rate,block));}
    require(report["serial_ns"].size()==(kind=="multi"?blocks:0)&&report["event_window_labels"].size()==blocks,"inconsistent serial/tag geometry");
    for(int b=0;b<report["serial_ns"].size();++b){const auto n=report["serial_ns"][b];require((n.isInt64()||n.isInt())&&juce::int64(n)>=0,"invalid serial nanoseconds");}
    require(bool(report["raw_finite"])&&std::isfinite(double(report["raw_peak"]))&&bool(report["integrity"]),"audio/measurement integrity failure");
    auto checkNote=[](const J& midi,int pitch,bool on,int offset){require(midi["type"].toString()==(on?"note_on":"note_off")&&int(midi["note"])==pitch&&int(midi["channel"])==1&&int(midi["velocity"])==(on?100:0)&&int(midi["sample_offset"])==offset,"incorrect MIDI note trace");};
    const int held=name.startsWith("full-")?16:1;require(int(report["initial_note_count"])==held&&report["initial_midi"].size()==held,"incorrect held note capacity");
    for(int k=0;k<held;++k)checkNote(report["initial_midi"][k],48+k,true,0);
    if(kind=="lifecycle"){
        require(report["events"].size()==10&&bool(report["missing_tempo_observed"])&&bool(report["recovered_tempo_observed"]),"incomplete lifecycle report");
        const auto script=events(rate,block);const std::array<int,10> midiCounts{8,0,0,0,16,16,1,16,0,0};
        for(int e=0;e<10;++e){const auto trace=report["events"][e];const auto& wanted=script[std::size_t(e)];
            require(trace["name"].toString()==wanted.name&&juce::int64(trace["requested_frame"])==wanted.requested&&juce::int64(trace["actual_frame"])==wanted.actual&&int(trace["sample_offset"])==wanted.actual%block&&trace["midi"].size()==midiCounts[std::size_t(e)],"incorrect event schedule/trace");
            for(int k=0;k<midiCounts[std::size_t(e)];++k){if(e==6)require(trace["midi"][k]["type"].toString()=="midi_stop"&&int(trace["midi"][k]["sample_offset"])==wanted.actual%block,"incorrect MIDI Stop");else checkNote(trace["midi"][k],48+(e==0?k%4:k),e==0?k>=4:e!=5,int(wanted.actual%block));}
        }
        require(report["tempo_unavailable"].size()==count,"missing tempo observations");
        for(int b=0;b<blocks;++b){const Ns frame=Ns(b)*block;require(bool(report["tempo_unavailable"][0][b])==(frame>=script[8].actual&&frame<script[9].actual),"missing/recovered tempo timing mismatch");}
    }
}
bool acceptTrial(J& trial)
{
    try {validateTrial(trial);return true;}
    catch(const std::exception& e){put(trial,"integrity",false);put(trial,"validation_error",e.what());return false;}
}
bool validInventoryObject(const juce::MemoryBlock& bytes)
{
    // JUCE's JSON reader permits trailing text/commas and treats arrays as objects.
    // Validate the complete bounded UTF-8 grammar iteratively; metadata need not be decoded.
    const auto* data=static_cast<const char*>(bytes.getData());const auto size=bytes.getSize();
    if(size==0||!juce::CharPointer_UTF8::isValidString(data,int(size)))return false;
    const std::string_view text(data,size);std::size_t i=text.starts_with("\xef\xbb\xbf")?3:0;
    auto whitespace=[&]{while(i<size&&(text[i]==' '||text[i]=='\t'||text[i]=='\r'||text[i]=='\n'))++i;};
    auto digit=[](char c){return c>='0'&&c<='9';};
    auto quoted=[&]{
        if(i>=size||text[i++]!='"')return false;
        while(i<size){const auto c=static_cast<unsigned char>(text[i++]);if(c=='"')return true;if(c<0x20)return false;
            if(c=='\\'){if(i==size)return false;const auto escape=text[i++];
                if(escape=='u'){for(int k=0;k<4;++k){if(i==size||!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')||(text[i]>='A'&&text[i]<='F')))return false;++i;}}
                else if(std::string_view("\"\\/bfnrt").find(escape)==std::string_view::npos)return false;}}
        return false;
    };
    whitespace();if(i==size||text[i++]!='{')return false;
    enum class Expected {keyOrEnd,key,colon,value,arrayValueOrEnd,arrayValue,objectCommaOrEnd,arrayCommaOrEnd};
    std::vector<Expected> stack{Expected::keyOrEnd};
    while(!stack.empty()){
        whitespace();if(i==size)return false;const auto state=stack.back();const auto c=text[i];
        if(state==Expected::keyOrEnd||state==Expected::key){
            if(state==Expected::keyOrEnd&&c=='}'){++i;stack.pop_back();continue;}
            if(!quoted())return false;stack.back()=Expected::colon;continue;
        }
        if(state==Expected::colon){if(c!=':')return false;++i;stack.back()=Expected::value;continue;}
        if(state==Expected::objectCommaOrEnd||state==Expected::arrayCommaOrEnd){
            const bool objectState=state==Expected::objectCommaOrEnd;
            if(c==(objectState?'}':']')){++i;stack.pop_back();continue;}
            if(c!=',')return false;++i;stack.back()=objectState?Expected::key:Expected::arrayValue;continue;
        }
        if(state==Expected::arrayValueOrEnd&&c==']'){++i;stack.pop_back();continue;}
        stack.back()=state==Expected::value?Expected::objectCommaOrEnd:Expected::arrayCommaOrEnd;
        if(c=='{'||c=='['){++i;stack.push_back(c=='{'?Expected::keyOrEnd:Expected::arrayValueOrEnd);continue;}
        if(c=='"'){if(!quoted())return false;continue;}
        bool literal=false;for(const auto word:{std::string_view("true"),std::string_view("false"),std::string_view("null")})
            if(text.substr(i).starts_with(word)){i+=word.size();literal=true;break;}
        if(literal)continue;
        if(c=='-')++i;if(i==size||!digit(text[i]))return false;
        if(text[i]=='0')++i;else while(i<size&&digit(text[i]))++i;
        if(i<size&&text[i]=='.'){++i;const auto start=i;while(i<size&&digit(text[i]))++i;if(i==start)return false;}
        if(i<size&&(text[i]=='e'||text[i]=='E')){++i;if(i<size&&(text[i]=='+'||text[i]=='-'))++i;const auto start=i;while(i<size&&digit(text[i]))++i;if(i==start)return false;}
    }
    whitespace();return i==size;
}
J inventoryMetadata(const juce::File& supplied)
{
    const bool explicitInput=supplied!=juce::File();
    const juce::File root(RACKSYNTH_WORKSPACE);
    const auto file=explicitInput?supplied:root.getChildFile(".superpowers/sdd/m2-profile-20261004/task-1-inventory.json");
    auto result=object();put(result,"inventory_source",explicitInput?"explicit":"legacy_snapshot");
    put(result,"inventory_path",file.getFullPathName());put(result,"inventory_available",file.existsAsFile());
    if(explicitInput){
        namespace fs=std::filesystem;
        const auto workspace=fs::canonical(fs::path(root.getFullPathName().toWideCharPointer()));
        const auto path=fs::canonical(fs::path(file.getFullPathName().toWideCharPointer()));const auto relative=path.lexically_relative(workspace);
        require(!relative.empty()&&relative!="."&&!relative.is_absolute()&&*relative.begin()!="..","inventory outside workspace");
        require(fs::is_regular_file(path)&&file.existsAsFile(),"inventory must be a regular file");
        constexpr int maximum=256*1024;
        require(file.getSize()<=maximum,"inventory exceeds256KiB");
        juce::FileInputStream input(file);require(input.openedOk(),"inventory unreadable");juce::MemoryBlock bytes;
        input.readIntoMemoryBlock(bytes,maximum+1);
        require(input.getStatus().wasOk()&&input.isExhausted()&&bytes.getSize()<=maximum,"inventory unreadable or exceeds256KiB");
        require(validInventoryObject(bytes),"inventory must be a valid JSON object");
        put(result,"inventory_sha256",juce::SHA256(bytes).toHexString());
    }else if(file.existsAsFile()){
        juce::FileInputStream input(file);require(input.openedOk(),"legacy inventory unreadable");put(result,"inventory_sha256",juce::SHA256(input).toHexString());
    }
    return result;
}
struct Arguments {juce::String mode,suite;juce::File output,inventory;RenderQuality quality=RenderQuality::realtime;AlgorithmRevision reverbRevision=0;bool eqOnly=false,filterOnly=false,chorusOnly=false,phaserOnly=false,flangerOnly=false,stereoOnly=false,dimensionOnly=false,frequencyShiftOnly=false,convolutionOnly=false,frequencySplitOnly=false,midSideSplitOnly=false;AlgorithmRevision synthRevision=1,driveRevision=1,compressorRevision=1,modulatedTapRevision=1,delayRevision=1,eqRevision=1,filterRevision=1,frequencySplitRevision=1;bool wavetableUnisonOnly=false;bool eraReverb=false;juce::String drumCapacityPoint;bool drumLocalPreview=false;};
Arguments parseArguments(juce::StringArray args)
{
    if(args.contains("--drum-local-preview")){
require(std::count(args.begin(),args.end(),juce::String("--drum-local-preview"))==1,"Duplicate local preview selector");
        require(args.contains("--drum-capacity-point"),"Local preview requires drum capacity point");
        args.remove(args.indexOf("--drum-local-preview"));auto parsed=parseArguments(args);parsed.drumLocalPreview=true;return parsed;
    }
    if(args.contains("--drum-capacity-point")){
        require(args.size()>=7&&args[0]=="--profile"&&args[1]=="--suite"&&args[2]=="sound-quality"&&args[3]=="--output","Drum capacity requires sound-quality and output");
        juce::String point,quality,inventory;bool pointSeen=false,qualitySeen=false,inventorySeen=false;
        for(int n=5;n<args.size();n+=2){require(n+1<args.size(),"Incomplete drum capacity selector");
            if(args[n]=="--drum-capacity-point"){require(!pointSeen,"Duplicate drum capacity point");pointSeen=true;point=args[n+1];require(point=="44100/128"||point=="48000/128"||point=="96000/512","Unknown drum capacity point");}
            else if(args[n]=="--quality"){require(!qualitySeen&&(args[n+1]=="realtime"||args[n+1]=="higher"),"Invalid/duplicate drum capacity quality");qualitySeen=true;quality=args[n+1];}
            else if(args[n]=="--inventory"){require(!inventorySeen,"Duplicate drum inventory");inventorySeen=true;inventory=args[n+1];}
            else require(false,"Drum capacity cannot mix family/algorithm/unknown selectors");
        }
        require(pointSeen,"Missing drum capacity point");juce::StringArray base{args[0],args[1],args[2],args[3],args[4]};
        if(inventorySeen){base.add("--inventory");base.add(inventory);}auto parsed=parseArguments(base);
        parsed.drumCapacityPoint=point;parsed.quality=quality=="higher"?RenderQuality::higher:RenderQuality::realtime;
#if JUCE_WINDOWS
        for(auto path=parsed.output.getParentDirectory();path!=juce::File(RACKSYNTH_WORKSPACE).getParentDirectory();path=path.getParentDirectory()){
            const auto attributes=GetFileAttributesW(path.getFullPathName().toWideCharPointer());
            require(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)==0,"Drum output parent is a reparse point");
            require(path!=path.getParentDirectory(),"Drum output parent escapes workspace");
        }
#endif
        return parsed;
    }
    if(args.size()==1&&args[0]=="--task13-step7-self-test")return {args[0],{}, {}};
    if(args.size()>=3&&args[0]=="--profile"&&args[1]=="--suite"&&args[2]=="task13-drive-background-diagnostic"){
        require(args.size()==5||args.size()==7,"drive diagnostic requires only output and optional inventory");
        auto legacy=args;legacy.set(2,"production");auto parsed=parseArguments(legacy);parsed.suite="task13-drive-background-diagnostic";return parsed;
    }
    if(args.size()>=3&&args[0]=="--profile"&&args[1]=="--suite"&&args[2]=="task13-production"){
        require(args.size()==5||args.size()==7,"Task13 requires only output and optional inventory");
        auto legacy=args;legacy.set(2,"production");auto parsed=parseArguments(legacy);parsed.suite="task13-production";return parsed;
    }
    if(args.size()==1&&(args[0]=="--self-test"||args[0]=="--help"))return {args[0],{}, {}};
    auto quality=RenderQuality::realtime;bool selected=false,reverbSelected=false,eqOnly=false,filterOnly=false,chorusOnly=false,phaserOnly=false,flangerOnly=false,stereoOnly=false,dimensionOnly=false,frequencyShiftOnly=false,convolutionOnly=false,frequencySplitOnly=false,midSideSplitOnly=false,wavetableUnisonOnly=false;AlgorithmRevision reverbRevision=0;
    bool synthSelected=false,driveSelected=false,compressorSelected=false,modulatedTapSelected=false,delaySelected=false,eqSelected=false,filterSelected=false,frequencySplitSelected=false,eraReverb=false;AlgorithmRevision synthRevision=1,driveRevision=1,compressorRevision=1,modulatedTapRevision=1,delayRevision=1,eqRevision=1,filterRevision=1,frequencySplitRevision=1;
    for(int n=5;n<args.size();)
    {
        if(args[n]=="--wavetable-unison-only"){require(args[2]=="sound-quality"&&!wavetableUnisonOnly,"Wavetable unison selection applies once to sound-quality only");wavetableUnisonOnly=true;args.remove(n);continue;}
        if(args[n]=="--era-reverb"){require(args[2]=="sound-quality"&&!eraReverb,"Era Reverb selection applies once to sound-quality only");eraReverb=true;args.remove(n);continue;}
        if(args[n]=="--mid-side-split-only"){require(args[2]=="sound-quality"&&!midSideSplitOnly,"Mid side split selection applies once to sound-quality only");midSideSplitOnly=true;args.remove(n);continue;}
        if(args[n]=="--frequency-split-only"){require(args[2]=="sound-quality"&&!frequencySplitOnly,"Frequency split selection applies once to sound-quality only");frequencySplitOnly=true;args.remove(n);continue;}
        if(args[n]=="--convolution-only"){require(args[2]=="sound-quality"&&!convolutionOnly,"Convolution selection applies once to sound-quality only");convolutionOnly=true;args.remove(n);continue;}
        if(args[n]=="--frequency-shift-only"){require(args[2]=="sound-quality"&&!frequencyShiftOnly,"Frequency-shift selection applies once to sound-quality only");frequencyShiftOnly=true;args.remove(n);continue;}
        if(args[n]=="--dimension-only"){require(args[2]=="sound-quality"&&!dimensionOnly,"Dimension selection applies once to sound-quality only");dimensionOnly=true;args.remove(n);continue;}
        if(args[n]=="--stereo-only"){require(args[2]=="sound-quality"&&!stereoOnly,"Stereo selection applies once to sound-quality only");stereoOnly=true;args.remove(n);continue;}
        if(args[n]=="--flanger-only"){require(args[2]=="sound-quality"&&!flangerOnly,"Flanger selection applies once to sound-quality only");flangerOnly=true;args.remove(n);continue;}
        if(args[n]=="--phaser-only"){require(args[2]=="sound-quality"&&!phaserOnly,"Phaser selection applies once to sound-quality only");phaserOnly=true;args.remove(n);continue;}
        if(args[n]=="--chorus-only"){require(args[2]=="sound-quality"&&!chorusOnly,"Chorus selection applies once to sound-quality only");chorusOnly=true;args.remove(n);continue;}
        if(args[n]=="--filter-only"){require(args[2]=="sound-quality"&&!filterOnly,"Filter selection applies once to sound-quality only");filterOnly=true;args.remove(n);continue;}
        if(args[n]=="--eq-only"){require(args[2]=="sound-quality"&&!eqOnly,"EQ selection applies once to sound-quality only");eqOnly=true;args.remove(n);continue;}
        if(args[n]!="--quality"&&args[n]!="--reverb-algorithm"&&args[n]!="--synth-algorithm"&&args[n]!="--drive-algorithm"&&args[n]!="--compressor-algorithm"&&args[n]!="--modulation-algorithm"&&args[n]!="--delay-algorithm"&&args[n]!="--eq-algorithm"&&args[n]!="--filter-algorithm"&&args[n]!="--frequency-split-algorithm"){n+=2;continue;}
        require(n+1<args.size()&&args[2]=="sound-quality","Quality/reverb/synth/drive/compressor/modulation/delay/EQ/filter/frequency split selection applies to sound-quality only");
        if(args[n]=="--quality"){
            require(!selected,"Duplicate profile quality");selected=true;
            require(args[n+1]=="realtime"||args[n+1]=="higher","Unknown profile quality");
            quality=args[n+1]=="higher"?RenderQuality::higher:RenderQuality::realtime;
        }else if(args[n]=="--synth-algorithm"){
            require(!synthSelected,"Duplicate synth algorithm");synthSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown synth algorithm");
            synthRevision=args[n+1]=="2"?modernSynthAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--drive-algorithm"){
            require(!driveSelected,"Duplicate drive algorithm");driveSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown drive algorithm");
            driveRevision=args[n+1]=="2"?modernDriveAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--compressor-algorithm"){
            require(!compressorSelected,"Duplicate compressor algorithm");compressorSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown compressor algorithm");
            compressorRevision=args[n+1]=="2"?modernCompressorAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--modulation-algorithm"){
            require(!modulatedTapSelected,"Duplicate modulation algorithm");modulatedTapSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown modulation algorithm");
            modulatedTapRevision=args[n+1]=="2"?modernModulatedTapAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--delay-algorithm"){
            require(!delaySelected,"Duplicate delay algorithm");delaySelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown delay algorithm");
            delayRevision=args[n+1]=="2"?modernDelayAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--eq-algorithm"){
            require(!eqSelected,"Duplicate EQ algorithm");eqSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown EQ algorithm");
            eqRevision=args[n+1]=="2"?modernEqAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--filter-algorithm"){
            require(!filterSelected,"Duplicate filter algorithm");filterSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown filter algorithm");
            filterRevision=args[n+1]=="2"?modernFilterAlgorithmRevision:legacyAlgorithmRevision;
        }else if(args[n]=="--frequency-split-algorithm"){
            require(!frequencySplitSelected,"Duplicate frequency split algorithm");frequencySplitSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown frequency split algorithm");
            frequencySplitRevision=args[n+1]=="2"?modernFrequencySplitAlgorithmRevision:legacyAlgorithmRevision;
        }else{
            require(!reverbSelected,"Duplicate reverb algorithm");reverbSelected=true;
            require(args[n+1]=="1"||args[n+1]=="2","Unknown reverb algorithm");
            reverbRevision=args[n+1]=="2"?2:1;
            require(supportsEffectAlgorithmRevision(2,reverbRevision),"Reverb profile algorithm not implemented");
        }
        args.removeRange(n,2);
    }
    require(!eqOnly||!reverbSelected,"EQ and reverb selections cannot be mixed");
    require(!filterOnly||(!eqOnly&&!reverbSelected),"Filter, EQ and reverb selections cannot be mixed");
    require(!chorusOnly||(!filterOnly&&!eqOnly&&!reverbSelected),"Chorus, filter, EQ and reverb selections cannot be mixed");
    require(!phaserOnly||(!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Phaser, chorus, filter, EQ and reverb selections cannot be mixed");
    require(!flangerOnly||(!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Flanger, phaser, chorus, filter, EQ and reverb selections cannot be mixed");
    require(!wavetableUnisonOnly||(!midSideSplitOnly&&!frequencySplitOnly&&!convolutionOnly&&!frequencyShiftOnly&&!dimensionOnly&&!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Wavetable unison and other selections cannot be mixed");
    require(!midSideSplitOnly||(!frequencySplitOnly&&!convolutionOnly&&!frequencyShiftOnly&&!dimensionOnly&&!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Mid side split and other selections cannot be mixed");
    require(!frequencySplitOnly||(!convolutionOnly&&!frequencyShiftOnly&&!dimensionOnly&&!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Frequency split and other selections cannot be mixed");
    require(!convolutionOnly||(!frequencyShiftOnly&&!dimensionOnly&&!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Convolution and other selections cannot be mixed");
    require(!frequencyShiftOnly||(!dimensionOnly&&!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Frequency-shift and other selections cannot be mixed");
    require(!dimensionOnly||(!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Dimension and other selections cannot be mixed");
    require(!stereoOnly||(!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Stereo, flanger, phaser, chorus, filter, EQ and reverb selections cannot be mixed");
    require(!eraReverb||(!wavetableUnisonOnly&&!midSideSplitOnly&&!frequencySplitOnly&&!convolutionOnly&&!frequencyShiftOnly&&!dimensionOnly&&!stereoOnly&&!flangerOnly&&!phaserOnly&&!chorusOnly&&!filterOnly&&!eqOnly&&!reverbSelected),"Era Reverb and other selections cannot be mixed");
    require((args.size()==5||(args.size()==7&&args[5]=="--inventory"))&&args[0]=="--profile"&&args[1]=="--suite"&&args[3]=="--output","Usage: --profile --suite full|smoke|pattern|production|rack-personalities|sound-quality --output FRESH_ABSOLUTE_WORKSPACE_DIRECTORY [--inventory ABSOLUTE_WORKSPACE_JSON]; --self-test; --help");
    require(args[2]=="full"||args[2]=="smoke"||args[2]=="pattern"||args[2]=="production"||args[2]=="rack-personalities"||args[2]=="sound-quality"||args[2]=="reverb-cost","invalid suite");require(juce::File::isAbsolutePath(args[4]),"output must be absolute");
    const juce::File output(args[4]);namespace fs=std::filesystem;const auto workspace=fs::weakly_canonical(fs::path(juce::File(RACKSYNTH_WORKSPACE).getFullPathName().toWideCharPointer()));const auto destination=fs::weakly_canonical(fs::path(output.getFullPathName().toWideCharPointer()));const auto relative=destination.lexically_relative(workspace);
    require(!relative.empty()&&relative!="."&&!relative.is_absolute()&&*relative.begin()!="..","output outside workspace");require(!output.exists()&&!fs::exists(fs::symlink_status(fs::path(output.getFullPathName().toWideCharPointer()))),"output already exists");
    juce::File inventory;if(args.size()==7){require(juce::File::isAbsolutePath(args[6]),"inventory must be absolute");inventory=juce::File(args[6]);inventoryMetadata(inventory);}return {args[0],args[2],output,inventory,quality,reverbRevision,eqOnly,filterOnly,chorusOnly,phaserOnly,flangerOnly,stereoOnly,dimensionOnly,frequencyShiftOnly,convolutionOnly,frequencySplitOnly,midSideSplitOnly,synthRevision,driveRevision,compressorRevision,modulatedTapRevision,delayRevision,eqRevision,filterRevision,frequencySplitRevision,wavetableUnisonOnly,eraReverb};
}

J identities()
{
    const juce::File root(RACKSYNTH_WORKSPACE);J entries(juce::Array<J>{});
    auto add=[&](const juce::File& file,const char* role){auto e=object();put(e,"role",role);put(e,"path",file.getFullPathName());put(e,"available",file.existsAsFile());if(file.existsAsFile()){put(e,"bytes",integer(file.getSize()));juce::FileInputStream stream(file);require(stream.openedOk(),"identity file unreadable");put(e,"sha256",juce::SHA256(stream).toHexString());}entries.append(e);};
    add(root.getChildFile("tools/native/ProfileInstrument.cpp"),"profiler-source");add(juce::File::getSpecialLocation(juce::File::currentExecutableFile),"profiler-executable");
    add(root.getChildFile("src/dsp/HistoryRing.h"),"history-source");add(root.getChildFile("tests/native/HistoryRingTests.cpp"),"history-test");
    add(root.getChildFile("build/windows-debug/RackSynthInstrument_artefacts/" RACKSYNTH_CONFIGURATION "/AI Rack Synth_SharedCode.lib"),"linked-instrument");
    add(root.getChildFile("build/windows-debug/RackSynthInstrument_artefacts/" RACKSYNTH_CONFIGURATION "/VST3/AI Rack Synth.vst3/Contents/x86_64-win/AI Rack Synth.vst3"),"product-vst3");
    add(root.getChildFile("build/windows-debug/CMakeCache.txt"),"compiler-build-cache");
    add(root.getChildFile("build/windows-debug/_deps/juce-src/modules/juce_core/juce_core.h"),"juce-header");
    add(root.getChildFile("build/windows-debug/_deps/juce-src/.git/HEAD"),"juce-revision");
    auto current=root.getChildFile("src").findChildFiles(juce::File::findFiles,true,"*.cpp;*.h");current.sort();for(const auto& file:current)add(file,"current-product-source");
    add(root.getChildFile("CMakeLists.txt"),"current-cmake");
    add(root.getChildFile("tools/native/RenderFixtures.cpp"),"current-render-source");
    add(root.getChildFile("tests/native/SynthEngineTests.cpp"),"current-core-test");
    add(root.getChildFile("tests/native/RoutedSynthEngineTests.cpp"),"current-routed-test");
    return entries;
}
struct CpuTime {bool available=false;std::uint64_t user=0,kernel=0;};
CpuTime cpuTime()
{
#if JUCE_WINDOWS
    FILETIME created,exited,kernel,user;
    if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return {true,(std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime,(std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime};
#endif
    return {};
}
void writeJson(const juce::File& file,const J& data) {require(!file.exists(),"refusing report overwrite");require(file.replaceWithText(juce::JSON::toString(data,false)),"cannot write report");}
int profile(const juce::String& suite,const juce::File& output,const juce::File& suppliedInventory)
{
    require(suite=="full"||suite=="smoke","unknown suite");
    require(suite!="full"||juce::String(RACKSYNTH_CONFIGURATION)=="Release","full measurements require Release");
    require(std::filesystem::create_directory(std::filesystem::path(output.getFullPathName().toWideCharPointer())),"fresh output directory creation failed");
    const auto start=Clock::now();const auto cpuBefore=cpuTime();const auto before=identities();auto report=object();
    put(report,"schema_version",1);put(report,"suite",suite);put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"scope","native InstrumentProcessor; multiple instances are a serial scheduling model, not DAW multicore measurements");
    put(report,"timing_limits","steady_clock nanoseconds; clock overhead not deducted; multi serial interval includes sequential callbacks and nested clock calls/stores only; no audio scan, JSON, state edits, sleeps or pacing timed inside callbacks; scheduling outliers retained; deadline excursions are not observed DAW underruns");
    put(report,"audio_limits","Untouched stereo float samples; no normalization. FNV-1a64 hashes all channel-major float bits per block including signed zero. Tail interval is short observation, not settled silence. No listening acceptance.");
    put(report,"actual_host_underruns",J());put(report,"actual_host_underruns_reason","No actual host underrun counter exists in this harness");put(report,"performance_acceptance",false);
    put(report,"juce_pinned_revision","be29c81492b6151c8ea8d14c840e1311963b3a83");put(report,"compiler_msc_full_ver",integer(_MSC_FULL_VER));put(report,"compiler_msc_ver",_MSC_VER);put(report,"sdk_version","10.0.26100.0");put(report,"started_utc",juce::Time::getCurrentTime().toISO8601(true));
    const auto inventory=inventoryMetadata(suppliedInventory);
    for(const auto& property:inventory.getDynamicObject()->getProperties())report.getDynamicObject()->setProperty(property.name,property.value);
    put(report,"identities_before",before);std::vector<Geometry> matrix;
    if(suite=="smoke")matrix={{"full-wavetable-mixed","steady",48000,128,1,1},{"full-wavetable-mixed","lifecycle",48000,128,1,1},{"full-wavetable-mixed","multi",48000,128,4,1}};
    else {
        for(const auto& name:names)for(const int rate:{44100,48000,96000})for(const int block:{64,128,256,512,1024})for(int trial=1;trial<=3;++trial)matrix.push_back({name,"steady",rate,block,1,trial});
        for(const auto& name:{"full-wavetable-mixed","full-delay","full-reverb"})for(const int rate:{44100,48000,96000})for(const int block:{64,128,256,512,1024})for(int trial=1;trial<=2;++trial)matrix.push_back({name,"lifecycle",rate,block,1,trial});
        for(const int rate:{48000,96000})for(const int block:{128,512})for(const int count:{1,4,8})for(int trial=1;trial<=3;++trial)matrix.push_back({"full-wavetable-mixed","multi",rate,block,count,trial});
    }
    require(matrix.size()==std::size_t(suite=="full"?396:3),"incorrect fixed matrix");put(report,"expected_trial_count",int(matrix.size()));J trials(juce::Array<J>{});bool integrity=true;int number=0;
    for(const auto& g:matrix){auto trial=runTrial(g);if(!acceptTrial(trial))integrity=false;const auto filename="trial-"+juce::String(++number).paddedLeft('0',3)+".json";writeJson(output.getChildFile(filename),trial);put(trial,"trial_file",filename);trials.append(trial);
        std::cout<<number<<'/'<<matrix.size()<<' '<<g.kind<<' '<<g.workload<<' '<<g.rate<<'/'<<g.block<<" instances="<<g.count<<" integrity="<<bool(trial["integrity"])<<std::endl;
    }
    put(report,"trials",trials);put(report,"completed_trial_count",number);const auto after=identities();put(report,"identities_after",after);const bool stable=juce::JSON::toString(before)==juce::JSON::toString(after);put(report,"identities_unchanged",stable);integrity=integrity&&stable;
    const auto cpuAfter=cpuTime();if(cpuBefore.available&&cpuAfter.available){auto cpu=object();const auto user=(cpuAfter.user-cpuBefore.user)*100,kernel=(cpuAfter.kernel-cpuBefore.kernel)*100;put(cpu,"user_ns",integer(Ns(user)));put(cpu,"kernel_ns",integer(Ns(kernel)));put(cpu,"total_ns",integer(Ns(user+kernel)));put(cpu,"scope","Windows GetProcessTimes tool-process delta, all threads, including setup/control/warmup/audio checks/statistics/trial JSON/identity work; excludes final root report serialization");put(report,"tool_process_cpu",cpu);}else {put(report,"tool_process_cpu",J());put(report,"tool_process_cpu_reason","GetProcessTimes unavailable");}
    put(report,"wall_ns_before_final_serialization",integer(elapsed(start)));put(report,"completed_utc",juce::Time::getCurrentTime().toISO8601(true));put(report,"measurement_integrity",integrity?"passed":"failed");writeJson(output.getChildFile("profile-report.json"),report);
    std::cout<<"measurement_integrity="<<(integrity?"passed":"failed")<<" trials="<<number<<std::endl;return integrity?0:1;
}

std::size_t index(const juce::String& id)
{
    const auto registry=parameterRegistry();
    for(std::size_t i=0;i<registry.size();++i) if(registry[i].id==id)return i;
    throw std::runtime_error("unknown parameter: "+id.toStdString());
}
// BEGIN SELF TEST -- frozen behavioral oracle, shared by RED and GREEN.
int selfTest()
{
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* what){++checks;if(!ok){++failed;std::cerr<<"FAIL: "<<what<<'\n';}};
    auto rejects=[&](auto action,const char* what){bool caught=false;try{action();}catch(const std::exception&){caught=true;}check(caught,what);};
    const auto fresh=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/m2-profile-task1-cli-selftest-absent").getFullPathName();
    try {const auto synth=juce::File(RACKSYNTH_WORKSPACE).getChildFile("src/dsp/SynthEngine.cpp");const auto ids=identities();bool listed=false;for(const auto& e:*ids.getArray())listed=listed||(e["role"].toString()=="current-product-source"&&juce::File(e["path"].toString())==synth&&bool(e["available"]));check(listed,"identities hash the current product sources");}
    catch(const std::exception&) {check(false,"identities hash the current product sources");}
    check(parseArguments({"--profile","--suite","smoke","--output",fresh}).suite=="smoke","valid bounded CLI");
    try {check(parseArguments({"--profile","--suite","reverb-cost","--output",fresh}).suite=="reverb-cost","bounded reverb component diagnosis admitted");}
    catch(const std::exception&) {check(false,"bounded reverb component diagnosis admitted");}
    try { check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).suite=="sound-quality","sound-quality suite admits fresh output"); }
    catch(const std::exception&) { check(false,"sound-quality suite admits fresh output"); }
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--quality","higher"}).quality==RenderQuality::higher,"explicit higher quality profile admitted");}
    catch(const std::exception&) {check(false,"explicit higher quality profile admitted");}
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","2"}).suite=="sound-quality","explicit modern all-eight-reverb workload admitted");}
    catch(const std::exception&) {check(false,"explicit modern all-eight-reverb workload admitted");}
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--quality","higher","--reverb-algorithm","2"}).reverbRevision==2,"modern profile selects revision2 independently of higher mode");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","3"});},"unsupported reverb profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","2","--reverb-algorithm","2"});},"duplicate reverb profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--reverb-algorithm","2"});},"reverb profile selection is bounded to sound-quality");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-reverb",2);});const auto& d=*dStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;check(makeRoutedPatch(d,p)&&d.layout.synthCount==4,"actual modern full workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"modern workload keeps64 active routes");
        bool all=true;for(std::size_t slot=0;slot<8;++slot)all&=p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::reverb&&!p.effects[slot].bypassed&&p.effects[slot].algorithmRevision==2&&p.compatibility.effects[slot][2]==2;
        check(all,"all8 actual active and retained Reverb2 banks");
    }
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).synthRevision==legacyAlgorithmRevision,"profile voices default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","2","--synth-algorithm","2"}).synthRevision==modernSynthAlgorithmRevision,"fresh-instance profile selects revision2 voices with Reverb2");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--synth-algorithm","3"});},"unsupported synth profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--synth-algorithm","2","--synth-algorithm","2"});},"duplicate synth profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--synth-algorithm","2"});},"synth profile selection is bounded to sound-quality");
    {
        workloadSynthRevision=modernSynthAlgorithmRevision;const auto dStorage=testing::fixtureValue([&]{return workload("full-reverb",2);});const auto& d=*dStorage;workloadSynthRevision=legacyAlgorithmRevision;
        auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p)&&d.layout.synthCount==4;for(const auto& layer:p.synth.layers)all&=layer.algorithmRevision==modernSynthAlgorithmRevision;
        check(all,"fresh-instance workload runs all4 layers on revision2 voices");
    }
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).driveRevision==legacyAlgorithmRevision,"profile drives default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--synth-algorithm","2","--drive-algorithm","2"}).driveRevision==modernDriveAlgorithmRevision,"fresh-instance profile selects revision2 drives");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--drive-algorithm","3"});},"unsupported drive profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--drive-algorithm","2","--drive-algorithm","2"});},"duplicate drive profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--drive-algorithm","2"});},"drive profile selection is bounded to sound-quality");
    {
        workloadDriveRevision=modernDriveAlgorithmRevision;const auto dStorage=testing::fixtureValue([&]{return workload("full-wavetable-mixed");});const auto& d=*dStorage;workloadDriveRevision=legacyAlgorithmRevision;
        auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int drives=0;
        for(std::size_t slot=0;slot<8;++slot)if(p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::drive){++drives;all&=p.effects[slot].algorithmRevision==modernDriveAlgorithmRevision;}
        check(all&&drives==2,"fresh-instance workload runs both drives on revision2");
    }
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).compressorRevision==legacyAlgorithmRevision,"profile compressors default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--drive-algorithm","2","--compressor-algorithm","2"}).compressorRevision==modernCompressorAlgorithmRevision,"fresh-instance profile selects revision2 compressors");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--compressor-algorithm","3"});},"unsupported compressor profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--compressor-algorithm","2","--compressor-algorithm","2"});},"duplicate compressor profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--compressor-algorithm","2"});},"compressor profile selection is bounded to sound-quality");
    {
        workloadCompressorRevision=modernCompressorAlgorithmRevision;const auto dStorage=testing::fixtureValue([&]{return workload("full-wavetable-mixed");});const auto& d=*dStorage;workloadCompressorRevision=legacyAlgorithmRevision;
        auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int compressors=0;
        for(std::size_t slot=0;slot<8;++slot)if(p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::compressor){++compressors;all&=p.effects[slot].algorithmRevision==modernCompressorAlgorithmRevision;}
        check(all&&compressors==2,"fresh-instance workload runs both compressors on revision2");
    }
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).modulatedTapRevision==legacyAlgorithmRevision,"profile modulated taps default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--modulation-algorithm","2"}).modulatedTapRevision==modernModulatedTapAlgorithmRevision,"fresh-instance profile selects revision2 modulated taps");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--modulation-algorithm","3"});},"unsupported modulation profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--modulation-algorithm","2","--modulation-algorithm","2"});},"duplicate modulation profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--modulation-algorithm","2"});},"modulation profile selection is bounded to sound-quality");
    workloadModulatedTapRevision=modernModulatedTapAlgorithmRevision;
    for(const auto* name:{"full-chorus","full-flanger","full-dimension"}){
        const auto dStorage=testing::fixtureValue([&]{return workload(name);});const auto& d=*dStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int taps=0;
        for(std::size_t slot=0;slot<8;++slot)if(p.layout.effects[slot].use==SlotUse::present&&modulatedTapEffectBank(static_cast<std::size_t>(p.layout.effects[slot].kind))){++taps;all&=p.effects[slot].algorithmRevision==modernModulatedTapAlgorithmRevision;}
        check(all&&taps==8,"fresh-instance workload runs all eight modulated-tap stages on revision2");
    }
    workloadModulatedTapRevision=legacyAlgorithmRevision;
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).delayRevision==legacyAlgorithmRevision,"profile delays default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--delay-algorithm","2"}).delayRevision==modernDelayAlgorithmRevision,"fresh-instance profile selects revision2 delays");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--delay-algorithm","3"});},"unsupported delay profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--delay-algorithm","2","--delay-algorithm","2"});},"duplicate delay profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--delay-algorithm","2"});},"delay profile selection is bounded to sound-quality");
    workloadDelayRevision=modernDelayAlgorithmRevision;
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-wavetable-mixed");});const auto& d=*dStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int delays=0;
        for(std::size_t slot=0;slot<8;++slot){all&=d.compatibility.effects[slot][delayEffectBank]==modernDelayAlgorithmRevision;
            if(p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::delay){++delays;all&=p.effects[slot].algorithmRevision==modernDelayAlgorithmRevision;}}
        check(all&&delays==2,"fresh-instance workload runs both delays on revision2 and retains it in every slot");
    }
    workloadDelayRevision=legacyAlgorithmRevision;
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).eqRevision==legacyAlgorithmRevision,"profile EQs default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--eq-algorithm","2"}).eqRevision==modernEqAlgorithmRevision,"fresh-instance profile selects revision2 EQs");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--eq-algorithm","3"});},"unsupported EQ profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--eq-algorithm","2","--eq-algorithm","2"});},"duplicate EQ profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--eq-algorithm","2"});},"EQ profile selection is bounded to sound-quality");
    workloadEqRevision=modernEqAlgorithmRevision;
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-eq");});const auto& d=*dStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int eqs=0;double tail=0;
        for(std::size_t slot=0;slot<8;++slot){all&=d.compatibility.effects[slot][eqEffectBank]==modernEqAlgorithmRevision;
            if(p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::eq){++eqs;all&=p.effects[slot].algorithmRevision==modernEqAlgorithmRevision;
                tail+=detail::eqTailSeconds(p.effects[slot].eq,96000.0,p.effects[slot].algorithmRevision);}}
        check(all&&eqs==8&&std::isfinite(tail)&&tail>0,"fresh-instance workload runs all eight EQ stages on revision2 with a finite engineering tail");
    }
    workloadEqRevision=legacyAlgorithmRevision;
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--eq-only"}).eqOnly,"singleton EQ profile selection admitted");}
    catch(const std::exception&) {check(false,"singleton EQ profile selection admitted");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--quality","higher"});check(selected.eqOnly&&selected.quality==RenderQuality::higher,"EQ profile selection and higher mode are independent");}
    catch(const std::exception&) {check(false,"EQ profile selection and higher mode are independent");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--reverb-algorithm","2"},
        {"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","1","--eq-only"},
        {"--profile","--suite","smoke","--output",fresh,"--eq-only"},
        {"--self-test","--eq-only"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite EQ selector rejects");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-eq");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4,"actual full EQ workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"EQ workload keeps64 active nonzero routes");
        bool all=true;for(std::size_t slot=0;slot<8;++slot){const auto& effect=patch.effects[slot];const auto& eq=effect.eq;
            all&=patch.layout.effects[slot].use==SlotUse::present&&patch.layout.effects[slot].kind==EffectKind::eq&&patch.layout.effects[slot].chain==masterChain&&!effect.bypassed&&effect.algorithmRevision==1&&patch.compatibility.effects[slot][4]==1;
            all&=eq.lowShelf.gainDb!=0&&eq.bell.gainDb!=0&&eq.highShelf.gainDb!=0&&eq.lowShelf.q>0&&eq.bell.q>0&&eq.highShelf.q>0;
            if(slot)all&=eq.lowShelf.frequencyHz!=patch.effects[slot-1].eq.lowShelf.frequencyHz&&eq.bell.frequencyHz!=patch.effects[slot-1].eq.bell.frequencyHz&&eq.highShelf.frequencyHz!=patch.effects[slot-1].eq.highShelf.frequencyHz;
        }
        check(all,"all8 independent nonzero EQ1 stages and retained native1 bank");
        juce::MemoryBlock wire;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),decoded)&&decoded==d,"actual EQ workload state roundtrip");
        const auto effectiveRate=96000.0;double tail=0;for(const auto& effect:patch.effects)tail+=detail::eqTailSeconds(effect.eq,effectiveRate,effect.algorithmRevision);check(std::isfinite(tail)&&tail>0,"EQ engineering tail uses actual effective internal rate");
    }
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).filterRevision==legacyAlgorithmRevision,"profile filters default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--filter-algorithm","2"}).filterRevision==modernFilterAlgorithmRevision,"fresh-instance profile selects revision2 filters");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--filter-algorithm","3"});},"unsupported filter profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--filter-algorithm","2","--filter-algorithm","2"});},"duplicate filter profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--filter-algorithm","2"});},"filter profile selection is bounded to sound-quality");
    workloadFilterRevision=modernFilterAlgorithmRevision;
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-filter");});const auto& d=*dStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int filters=0;double tail=0;
        for(std::size_t slot=0;slot<8;++slot){all&=d.compatibility.effects[slot][filterEffectBank]==modernFilterAlgorithmRevision;
            if(p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::filter){++filters;all&=p.effects[slot].algorithmRevision==modernFilterAlgorithmRevision;
                tail+=detail::filterTailSeconds(p.effects[slot].filter,96000.0,p.effects[slot].algorithmRevision);}}
        check(all&&filters==8&&std::isfinite(tail)&&tail>0,"fresh-instance workload runs all eight filter stages on revision2 with a finite engineering tail");
    }
    workloadFilterRevision=legacyAlgorithmRevision;
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--filter-only"}).filterOnly,"singleton filter profile selection admitted");}
    catch(const std::exception&) {check(false,"singleton filter profile selection admitted");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--quality","higher"});check(selected.filterOnly&&selected.quality==RenderQuality::higher,"filter profile selection and higher mode are independent");}
    catch(const std::exception&) {check(false,"filter profile selection and higher mode are independent");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--filter-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--filter-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--reverb-algorithm","2"},
        {"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","1","--filter-only"},
        {"--profile","--suite","smoke","--output",fresh,"--filter-only"},
        {"--self-test","--filter-only"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite filter selector rejects");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-filter");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4,"actual full filter workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"filter workload keeps64 active nonzero routes");
        bool all=true;for(std::size_t layer=0;layer<4;++layer)all&=patch.synth.layers[layer].enabled&&patch.synth.layers[layer].engine==(layer%2?EngineKind::wavetable:EngineKind::analog);
        for(std::size_t slot=0;slot<8;++slot){const auto& effect=patch.effects[slot];const auto& filter=effect.filter;
            all&=patch.layout.effects[slot].use==SlotUse::present&&patch.layout.effects[slot].kind==EffectKind::filter&&patch.layout.effects[slot].chain==masterChain&&!effect.bypassed&&effect.algorithmRevision==1&&patch.compatibility.effects[slot][5]==1;
            all&=filter.mode==FilterFxMode::lowpass&&filter.cutoffHz>=20&&filter.resonance>0&&filter.driveDb>0&&filter.mix>0;
            if(slot)all&=filter.cutoffHz!=patch.effects[slot-1].filter.cutoffHz&&filter.resonance!=patch.effects[slot-1].filter.resonance&&filter.driveDb!=patch.effects[slot-1].filter.driveDb&&filter.mix!=patch.effects[slot-1].filter.mix;
        }
        check(all,"all4 mixed layers and8 independent nonzero Filter1 stages with retained native1 bank");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;filterControls(edited,true);std::size_t changed=0;for(std::size_t i=filterParameterStart;i<filterParameterStart+8*filterParametersPerSlot;++i)changed+=edited.values[i]!=d.values[i];
        check(changed==40&&validateSoundDocument(edited),"filter automation changes all40 ordinary controls within registry bounds");
        juce::MemoryBlock wire;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),decoded)&&decoded==d,"actual filter workload state roundtrip");
        for(const auto effectiveRate:{48000.0,96000.0,192000.0}){double tail=0;for(const auto& effect:patch.effects)tail+=detail::filterTailSeconds(effect.filter,effectiveRate,effect.algorithmRevision);check(std::isfinite(tail)&&tail>0,"filter engineering tail uses actual effective internal rate");}
    }
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--chorus-only"}).chorusOnly,"singleton chorus profile selection admitted");}
    catch(const std::exception&) {check(false,"singleton chorus profile selection admitted");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--quality","higher"});check(selected.chorusOnly&&selected.quality==RenderQuality::higher,"chorus profile selection and higher mode are independent");}
    catch(const std::exception&) {check(false,"chorus profile selection and higher mode are independent");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--chorus-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--chorus-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--filter-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--chorus-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--reverb-algorithm","2"},
        {"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","1","--chorus-only"},
        {"--profile","--suite","smoke","--output",fresh,"--chorus-only"},
        {"--self-test","--chorus-only"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite chorus selector rejects");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-chorus");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4,"actual full chorus workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"chorus workload keeps64 active nonzero routes");
        bool all=true;for(std::size_t layer=0;layer<4;++layer)all&=patch.synth.layers[layer].enabled&&patch.synth.layers[layer].engine==(layer%2?EngineKind::wavetable:EngineKind::analog);
        for(std::size_t slot=0;slot<8;++slot){const auto& effect=patch.effects[slot];const auto& c=effect.chorus;
            all&=patch.layout.effects[slot].use==SlotUse::present&&patch.layout.effects[slot].kind==EffectKind::chorus&&patch.layout.effects[slot].chain==masterChain&&!effect.bypassed&&effect.algorithmRevision==1&&patch.compatibility.effects[slot][6]==1;
            all&=c.rateHz>0&&c.depth>0&&c.baseDelayMs>=12&&c.baseDelayMs<=40&&c.feedback!=0&&c.mix>0&&(slot%2?c.feedback<0:c.feedback>0);
            if(slot)all&=c.rateHz!=patch.effects[slot-1].chorus.rateHz&&c.depth!=patch.effects[slot-1].chorus.depth&&c.baseDelayMs!=patch.effects[slot-1].chorus.baseDelayMs&&c.mix!=patch.effects[slot-1].chorus.mix;
        }
        check(all&&patch.effects[0].chorus.baseDelayMs==12&&patch.effects[7].chorus.baseDelayMs==40,"all4 mixed layers and8 independent nonzero Chorus1 stages with centre endpoints and signed feedback");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;chorusControls(edited,true);std::size_t changed=0;for(std::size_t i=chorusParameterStart;i<chorusParameterStart+8*chorusParametersPerSlot;++i)changed+=edited.values[i]!=d.values[i];
        check(changed==40&&validateSoundDocument(edited),"chorus automation changes all40 ordinary controls within registry bounds");
        auto editedPatchStorage=std::make_unique<RoutedPatch>();auto& editedPatch=*editedPatchStorage;check(makeRoutedPatch(edited,editedPatch)&&editedPatch.effects[7].chorus.feedback>0&&editedPatch.effects[0].chorus.feedback<0,"chorus ordinary edits reverse actual feedback signs");
        juce::MemoryBlock wire;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),decoded)&&decoded==d,"actual chorus workload state roundtrip");
        for(const auto effectiveRate:{48000.0,96000.0,192000.0}){double tail=0;for(const auto& effect:patch.effects)tail+=detail::chorusTailSeconds(effect.chorus,effectiveRate);check(std::isfinite(tail)&&tail>0,"chorus engineering tail uses actual effective internal rate");}
    }
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--phaser-only"}).phaserOnly,"singleton phaser profile selection admitted");}
    catch(const std::exception&) {check(false,"singleton phaser profile selection admitted");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--quality","higher"});check(selected.phaserOnly&&selected.quality==RenderQuality::higher,"phaser profile selection and higher mode are independent");}
    catch(const std::exception&) {check(false,"phaser profile selection and higher mode are independent");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--phaser-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--phaser-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--filter-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--phaser-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--chorus-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--phaser-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--reverb-algorithm","2"},
        {"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","1","--phaser-only"},
        {"--profile","--suite","smoke","--output",fresh,"--phaser-only"},
        {"--self-test","--phaser-only"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite phaser selector rejects");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-phaser");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4,"actual full phaser workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"phaser workload keeps64 active nonzero routes");
        bool all=true;for(std::size_t layer=0;layer<4;++layer)all&=patch.synth.layers[layer].enabled&&patch.synth.layers[layer].engine==(layer%2?EngineKind::wavetable:EngineKind::analog);
        for(std::size_t slot=0;slot<8;++slot){const auto& effect=patch.effects[slot];const auto& phaser=effect.phaser;
            all&=patch.layout.effects[slot].use==SlotUse::present&&patch.layout.effects[slot].kind==EffectKind::phaser&&patch.layout.effects[slot].chain==masterChain&&!effect.bypassed&&effect.algorithmRevision==1&&patch.compatibility.effects[slot][7]==1;
            all&=phaser.rateHz>0&&phaser.depth>0&&phaser.centreHz>=20&&phaser.centreHz<=20000&&phaser.feedback!=0&&phaser.mix>0&&(slot%2?phaser.feedback<0:phaser.feedback>0);
            if(slot)all&=phaser.rateHz!=patch.effects[slot-1].phaser.rateHz&&phaser.depth!=patch.effects[slot-1].phaser.depth&&phaser.centreHz!=patch.effects[slot-1].phaser.centreHz&&phaser.mix!=patch.effects[slot-1].phaser.mix;
        }
        check(all&&patch.effects[0].phaser.centreHz==20&&patch.effects[7].phaser.centreHz==20000,"all4 mixed layers and8 independent nonzero Phaser1 stages with centre endpoints and signed feedback");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;phaserControls(edited,true);std::size_t changed=0;for(std::size_t i=phaserParameterStart;i<phaserParameterStart+8*phaserParametersPerSlot;++i)changed+=edited.values[i]!=d.values[i];
        check(changed==40&&validateSoundDocument(edited),"phaser automation changes all40 ordinary controls within registry bounds");
        auto editedPatchStorage=std::make_unique<RoutedPatch>();auto& editedPatch=*editedPatchStorage;check(makeRoutedPatch(edited,editedPatch)&&editedPatch.effects[7].phaser.feedback>0&&editedPatch.effects[0].phaser.feedback<0&&editedPatch.effects[0].phaser.centreHz==20000&&editedPatch.effects[7].phaser.centreHz==20,"phaser ordinary edits reverse actual feedback signs and centre endpoints");
        juce::MemoryBlock wire;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),decoded)&&decoded==d,"actual phaser workload state roundtrip");
        for(const auto effectiveRate:{48000.0,96000.0,192000.0}){double tail=0;for(const auto& effect:patch.effects)tail+=detail::phaserTailSeconds(effect.phaser,effectiveRate);check(std::isfinite(tail)&&tail>0,"phaser complete-loop stationary engineering tail uses actual effective internal rate");}
    }
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--flanger-only"}).flangerOnly,"singleton flanger profile selection admitted");}
    catch(const std::exception&) {check(false,"singleton flanger profile selection admitted");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--quality","higher"});check(selected.flangerOnly&&selected.quality==RenderQuality::higher,"flanger profile selection and higher mode are independent");}
    catch(const std::exception&) {check(false,"flanger profile selection and higher mode are independent");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--flanger-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--flanger-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--filter-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--flanger-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--phaser-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--flanger-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--chorus-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--flanger-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--reverb-algorithm","2"},
        {"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","1","--flanger-only"},
        {"--profile","--suite","smoke","--output",fresh,"--flanger-only"},
        {"--self-test","--flanger-only"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite flanger selector rejects");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-flanger");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4,"actual full flanger workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"flanger workload keeps64 active nonzero routes");
        bool all=true;for(std::size_t layer=0;layer<4;++layer)all&=patch.synth.layers[layer].enabled&&patch.synth.layers[layer].engine==(layer%2?EngineKind::wavetable:EngineKind::analog);
        for(std::size_t slot=0;slot<8;++slot){const auto& effect=patch.effects[slot];const auto& flanger=effect.flanger;
            all&=patch.layout.effects[slot].use==SlotUse::present&&patch.layout.effects[slot].kind==EffectKind::flanger&&patch.layout.effects[slot].chain==masterChain&&!effect.bypassed&&effect.algorithmRevision==1&&patch.compatibility.effects[slot][8]==1;
            all&=flanger.rateHz>0&&flanger.depth>0&&flanger.baseDelayMs>=1&&flanger.baseDelayMs<=8&&flanger.feedback!=0&&flanger.mix>0&&(slot%2?flanger.feedback<0:flanger.feedback>0);
            if(slot)all&=flanger.rateHz!=patch.effects[slot-1].flanger.rateHz&&flanger.depth!=patch.effects[slot-1].flanger.depth&&flanger.baseDelayMs!=patch.effects[slot-1].flanger.baseDelayMs&&flanger.mix!=patch.effects[slot-1].flanger.mix;
        }
        check(all&&patch.effects[0].flanger.baseDelayMs==1&&patch.effects[7].flanger.baseDelayMs==8,"all4 mixed layers and8 independent nonzero Flanger1 stages with base-delay endpoints and signed feedback");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;flangerControls(edited,true);std::size_t changed=0;for(std::size_t i=flangerParameterStart;i<flangerParameterStart+8*flangerParametersPerSlot;++i)changed+=edited.values[i]!=d.values[i];
        check(changed==40&&validateSoundDocument(edited),"flanger automation changes all40 ordinary controls within registry bounds");
        auto editedPatchStorage=std::make_unique<RoutedPatch>();auto& editedPatch=*editedPatchStorage;check(makeRoutedPatch(edited,editedPatch)&&editedPatch.effects[7].flanger.feedback>0&&editedPatch.effects[0].flanger.feedback<0&&editedPatch.effects[0].flanger.baseDelayMs==8&&editedPatch.effects[7].flanger.baseDelayMs==1,"flanger ordinary edits reverse actual feedback signs and base-delay endpoints");
        juce::MemoryBlock wire;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),decoded)&&decoded==d,"actual flanger workload state roundtrip");
        for(const auto effectiveRate:{48000.0,96000.0,192000.0}){double tail=0;for(const auto& effect:patch.effects)tail+=detail::flangerTailSeconds(effect.flanger,effectiveRate);check(std::isfinite(tail)&&tail>0,"flanger complete-loop stationary engineering tail uses actual effective internal rate");}
    }
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--mid-side-split-only"}).midSideSplitOnly,"singleton mid side split profile");}catch(const std::exception&){check(false,"singleton mid side split profile");}
    for(const auto* other:{"--mid-side-split-only","--frequency-split-only","--convolution-only","--frequency-shift-only","--dimension-only","--stereo-only","--flanger-only","--phaser-only","--chorus-only","--filter-only","--eq-only"})for(bool reverse:{false,true}){
        rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,reverse?other:"--mid-side-split-only",reverse?"--mid-side-split-only":other});},"mixed mid side split profile rejects");}
    {const auto dStorage=testing::fixtureValue([&]{return workload("full-mid-side-split");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4&&int(documentJson(d)["active_nonzero_routes"])==64,"actual mid side split4/8/64");
        for(std::size_t slot=0;slot<8;++slot)check(patch.layout.effects[slot].kind==EffectKind::midSideSplit&&patch.compatibility.effects[slot][14]==1&&patch.effects[slot].midSideSplit.mix>0,"independent nonneutral MidSideSplit1 stages");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;midSideSplitControls(edited,true);int count=0;for(std::size_t i=midSideSplitParameterStart;i<parameterCount;++i)count+=d.values[i]!=edited.values[i];check(count==56,"all56 mid side split edits genuinely change distinct values");}
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--wavetable-unison-only"}).wavetableUnisonOnly,"singleton wavetable unison profile");}catch(const std::exception&){check(false,"singleton wavetable unison profile");}
    for(const auto* other:{"--wavetable-unison-only","--mid-side-split-only","--frequency-split-only","--convolution-only","--frequency-shift-only","--dimension-only","--stereo-only","--flanger-only","--phaser-only","--chorus-only","--filter-only","--eq-only"})for(bool reverse:{false,true}){
        rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,reverse?other:"--wavetable-unison-only",reverse?"--wavetable-unison-only":other});},"mixed wavetable unison profile rejects");}
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--wavetable-unison-only","--reverb-algorithm","2"});},"wavetable unison and reverb profile rejects");
    {const auto dStorage=testing::fixtureValue([&]{return workload("full-wavetable-unison");});const auto& d=*dStorage;const auto mixed=testing::fixtureValue([&]{return workload("full-wavetable-mixed");});auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;
        check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4&&int(documentJson(d)["active_nonzero_routes"])==64&&d.layout==mixed->layout,"actual wavetable unison 4/8/64 with the mixed effects");
        for(std::size_t r=0;r<4;++r){const auto& l=patch.synth.layers[r];check(l.enabled&&l.engine==EngineKind::wavetable&&l.algorithmRevision==modernSynthAlgorithmRevision&&l.oscillators[0].unison==7&&l.oscillators[1].unison==7&&l.unisonWidth==1&&l.drift==1&&l.filterType==(r<2?FilterType::classic:FilterType::vowel),"four revision2 wavetable layers at seven voices, Classic then Vowel");}}
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--frequency-split-only"}).frequencySplitOnly,"singleton frequency split profile");}catch(const std::exception&){check(false,"singleton frequency split profile");}
    for(const auto* other:{"--frequency-split-only","--convolution-only","--frequency-shift-only","--dimension-only","--stereo-only","--flanger-only","--phaser-only","--chorus-only","--filter-only","--eq-only"})for(bool reverse:{false,true}){
        rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,reverse?other:"--frequency-split-only",reverse?"--frequency-split-only":other});},"mixed frequency split profile rejects");}
    {const auto dStorage=testing::fixtureValue([&]{return workload("full-frequency-split");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4&&int(documentJson(d)["active_nonzero_routes"])==64,"actual frequency split4/8/64");
        for(std::size_t slot=0;slot<8;++slot)check(patch.layout.effects[slot].kind==EffectKind::frequencySplit&&patch.compatibility.effects[slot][13]==1&&patch.effects[slot].frequencySplit.mix>0,"independent nonneutral FrequencySplit1 stages");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;frequencySplitControls(edited,true);int count=0;for(std::size_t i=frequencySplitParameterStart;i<parameterCount;++i)count+=d.values[i]!=edited.values[i];check(count==64,"all64 frequency split edits genuinely change distinct values");}
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh}).frequencySplitRevision==legacyAlgorithmRevision,"profile frequency splits default to revision1");
    check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--frequency-split-only","--frequency-split-algorithm","2"}).frequencySplitRevision==modernFrequencySplitAlgorithmRevision,"fresh-instance profile selects revision2 frequency splits");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--frequency-split-algorithm","3"});},"unsupported frequency split profile rejects");
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--frequency-split-algorithm","2","--frequency-split-algorithm","2"});},"duplicate frequency split profile selection rejects");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--frequency-split-algorithm","2"});},"frequency split profile selection is bounded to sound-quality");
    workloadFrequencySplitRevision=modernFrequencySplitAlgorithmRevision;
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-frequency-split");});const auto& d=*dStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;bool all=makeRoutedPatch(d,p);int splits=0;
        for(std::size_t slot=0;slot<8;++slot){all&=d.compatibility.effects[slot][frequencySplitEffectBank]==modernFrequencySplitAlgorithmRevision;
            if(p.layout.effects[slot].use==SlotUse::present&&p.layout.effects[slot].kind==EffectKind::frequencySplit){++splits;all&=p.effects[slot].algorithmRevision==modernFrequencySplitAlgorithmRevision;}}
        const double tail=detail::frequencySplitTailSeconds(96000.0,modernFrequencySplitAlgorithmRevision);
        check(all&&splits==8&&std::isfinite(tail)&&tail>0,"fresh-instance workload runs all eight frequency split stages on revision2 with a finite engineering tail");
    }
    workloadFrequencySplitRevision=legacyAlgorithmRevision;
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--convolution-only"}).convolutionOnly,"singleton convolution profile");}catch(const std::exception&){check(false,"singleton convolution profile");}
    for(const auto* other:{"--convolution-only","--frequency-shift-only","--dimension-only","--stereo-only","--flanger-only","--phaser-only","--chorus-only","--filter-only","--eq-only"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,reverse?other:"--convolution-only",reverse?"--convolution-only":other});},"duplicate/mixed convolution selection");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--convolution-only"});},"convolution selector wrong suite");
    {const auto dStorage=testing::fixtureValue([&]{return workload("full-convolution");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4&&int(documentJson(d)["active_nonzero_routes"])==64,"actual convolution4/8/64");
        for(std::size_t slot=0;slot<8;++slot)check(patch.layout.effects[slot].kind==EffectKind::convolution&&patch.compatibility.effects[slot][12]==1&&patch.effects[slot].convolution.ir==(slot%2?ConvolutionIr::shortRoom:ConvolutionIr::unitImpulse)&&patch.effects[slot].convolution.predelayMs>=0&&patch.effects[slot].convolution.mix>0,"independent actual Convolution1 stages and both prepared choices");
        auto changedStorage=std::make_unique<SoundDocument>(d);auto& changed=*changedStorage;convolutionControls(changed,true);std::size_t count=0;for(std::size_t i=convolutionParameterStart;i<convolutionParameterStart+32;++i)count+=changed.values[i]!=d.values[i];check(count==32&&validateSoundDocument(changed),"all32 convolution ordinary host fields change");
        juce::MemoryBlock wire;auto outStorage=std::make_unique<SoundDocument>();auto& out=*outStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),out)&&out==d,"convolution workload actual state roundtrip");}
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--frequency-shift-only"}).frequencyShiftOnly,"singleton frequency-shift profile");}catch(const std::exception&){check(false,"singleton frequency-shift profile");}
    for(const auto* other:{"--frequency-shift-only","--dimension-only","--stereo-only","--flanger-only","--phaser-only","--chorus-only","--filter-only","--eq-only"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,reverse?other:"--frequency-shift-only",reverse?"--frequency-shift-only":other});},"duplicate/mixed frequency-shift selection");
    rejects([&]{parseArguments({"--profile","--suite","smoke","--output",fresh,"--frequency-shift-only"});},"frequency-shift selector wrong suite");
    {const auto dStorage=testing::fixtureValue([&]{return workload("full-frequency-shift");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4&&int(documentJson(d)["active_nonzero_routes"])==64,"actual frequency-shift4/8/64");
        for(std::size_t slot=0;slot<8;++slot)check(patch.layout.effects[slot].kind==EffectKind::frequencyShift&&patch.compatibility.effects[slot][11]==1&&patch.effects[slot].frequencyShift.hz!=0&&patch.effects[slot].frequencyShift.stereoOffsetHz!=0&&patch.effects[slot].frequencyShift.mix>0,"independent actual nonzero FrequencyShift1 stages");
        auto changedStorage=std::make_unique<SoundDocument>(d);auto& changed=*changedStorage;frequencyShiftControls(changed,true);std::size_t count=0;for(std::size_t i=frequencyShiftParameterStart;i<frequencyShiftParameterStart+24;++i)count+=changed.values[i]!=d.values[i];check(count==24&&validateSoundDocument(changed),"all24 frequency-shift ordinary host fields change");
        juce::MemoryBlock wire;auto outStorage=std::make_unique<SoundDocument>();auto& out=*outStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),out)&&out==d,"frequency-shift workload actual state roundtrip");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--dimension-only"});check(selected.dimensionOnly,"singleton Dimension selection");}catch(const std::exception&){check(false,"singleton Dimension selection");}
    for(const auto& other:{"--dimension-only","--stereo-only","--flanger-only","--phaser-only","--chorus-only","--filter-only","--eq-only"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,reverse?other:"--dimension-only",reverse?"--dimension-only":other});},"duplicate/mixed Dimension selector rejects");
    {const auto dStorage=testing::fixtureValue([&]{return workload("full-dimension");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4&&int(documentJson(d)["active_nonzero_routes"])==64,"actual Dimension full4/8/64");
        for(std::size_t slot=0;slot<8;++slot)check(patch.layout.effects[slot].kind==EffectKind::dimension&&patch.compatibility.effects[slot][10]==1&&patch.effects[slot].dimension.depth>0&&patch.effects[slot].dimension.width>0&&patch.effects[slot].dimension.mix>0,"independent active native Dimension banks");
        auto changedStorage=std::make_unique<SoundDocument>(d);auto& changed=*changedStorage;dimensionControls(changed,true);std::size_t count=0;for(std::size_t i=dimensionParameterStart;i<dimensionParameterStart+32;++i)count+=changed.values[i]!=d.values[i];check(count==32&&validateSoundDocument(changed),"all32 ordinary Dimension host controls change");
        juce::MemoryBlock wire;auto outStorage=std::make_unique<SoundDocument>();auto& out=*outStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),out)&&out==d,"Dimension profile state roundtrip");}
    try {check(parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--stereo-only"}).stereoOnly,"singleton stereo profile selection admitted");}
    catch(const std::exception&) {check(false,"singleton stereo profile selection admitted");}
    try {const auto selected=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--quality","higher"});check(selected.stereoOnly&&selected.quality==RenderQuality::higher,"stereo profile selection and higher mode are independent");}
    catch(const std::exception&) {check(false,"stereo profile selection and higher mode are independent");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--eq-only","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--filter-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--filter-only","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--chorus-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--chorus-only","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--phaser-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--phaser-only","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--flanger-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--flanger-only","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--stereo-only","--reverb-algorithm","2"},
        {"--profile","--suite","sound-quality","--output",fresh,"--reverb-algorithm","1","--stereo-only"},
        {"--profile","--suite","smoke","--output",fresh,"--stereo-only"},
        {"--self-test","--stereo-only"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite stereo selector rejects");
    {
        const auto dStorage=testing::fixtureValue([&]{return workload("full-stereo");});const auto& d=*dStorage;auto patchStorage=std::make_unique<RoutedPatch>();auto& patch=*patchStorage;check(makeRoutedPatch(d,patch)&&d.layout.synthCount==4,"actual full stereo workload patch");
        const auto evidence=documentJson(d);check(int(evidence["active_nonzero_routes"])==64,"stereo workload keeps64 active nonzero routes");
        bool all=true;for(std::size_t layer=0;layer<4;++layer)all&=patch.synth.layers[layer].enabled&&patch.synth.layers[layer].engine==(layer%2?EngineKind::wavetable:EngineKind::analog);
        for(std::size_t slot=0;slot<8;++slot){const auto& effect=patch.effects[slot];const auto& stereo=effect.stereo;
            all&=patch.layout.effects[slot].use==SlotUse::present&&patch.layout.effects[slot].kind==EffectKind::stereo&&patch.layout.effects[slot].chain==masterChain&&!effect.bypassed&&effect.algorithmRevision==1&&patch.compatibility.effects[slot][9]==1;
            all&=std::abs(stereo.gainDb-(-6.f+float(slot)))<1e-5f&&stereo.pan!=0&&(slot%2?stereo.pan<0:stereo.pan>0)&&std::abs(stereo.width-(.5f+.1f*float(slot)))<1e-5f&&stereo.channelMode==static_cast<StereoChannelMode>(slot%3);
            if(slot)all&=stereo.gainDb!=patch.effects[slot-1].stereo.gainDb&&stereo.pan!=patch.effects[slot-1].stereo.pan&&stereo.width!=patch.effects[slot-1].stereo.width;
        }
        check(all,"all4 mixed layers and8 independent nonneutral Stereo1 stages with gain balance width and all3 modes");
        auto editedStorage=std::make_unique<SoundDocument>(d);auto& edited=*editedStorage;stereoControls(edited,true);std::size_t changed=0;for(std::size_t i=stereoParameterStart;i<stereoParameterStart+8*stereoParametersPerSlot;++i)changed+=edited.values[i]!=d.values[i];
        check(stereoParametersPerSlot==4&&changed==32&&validateSoundDocument(edited),"stereo automation changes exactly32 ordinary controls within registry bounds");
        auto editedPatchStorage=std::make_unique<RoutedPatch>();auto& editedPatch=*editedPatchStorage;check(makeRoutedPatch(edited,editedPatch),"edited stereo patch valid");
        for(std::size_t slot=0;slot<8;++slot){const auto& stereo=editedPatch.effects[slot].stereo;check(std::abs(stereo.gainDb-(-4.f+float(slot)))<1e-5f&&(slot%2?stereo.pan>0:stereo.pan<0)&&std::abs(stereo.width-(1.8f-.15f*float(slot)))<1e-5f&&stereo.channelMode==static_cast<StereoChannelMode>((slot+1)%3),"stereo ordinary edits change physical gains balance widths and mode targets");}
        juce::MemoryBlock wire;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;check(encodeSoundState(d,wire)&&decodeSoundState(wire.getData(),wire.getSize(),decoded)&&decoded==d,"actual stereo workload state roundtrip");
        check(evidence["effects"].isArray()&&evidence["effects"].getArray()->size()==8&&!(*evidence["effects"].getArray())[0]["stereo"].isVoid(),"stereo document provenance includes physical four-field settings");
    }
    rejects([&]{parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--quality","unknown"});},"unknown profile quality rejected before output creation");
    try { check(parseArguments({"--profile","--suite","rack-personalities","--output",fresh}).suite=="rack-personalities","personality suite admits fresh output"); }
    catch(const std::exception&) { check(false,"personality suite admits fresh output"); }
    for(const auto& args:std::vector<juce::StringArray>{{},{"--unknown"},{"--profile","--suite","bad","--output",fresh},{"--profile","--suite","smoke"},{"--self-test","--self-test"},{"--profile","--suite","smoke","--output",fresh,"--suite","full"},{"--profile","--suite","full","--output",RACKSYNTH_WORKSPACE},{"--profile","--suite","full","--output","C:/outside-profile"}})
        rejects([&]{parseArguments(args);},"reject invalid/duplicate/incomplete/existing/outside CLI");
    const auto s=statistics({1,2,3,4,100},4);
    check(s.minimum==1&&s.mean==22&&s.median==3&&s.p95==100&&s.p99==100&&s.maximum==100,"independent nanosecond/nearest-rank statistics oracle");
    const auto even=statistics({1,2,3,4},4);check(even.mean==2.5&&even.median==2.5,"independent even median oracle");
    check(s.misses==1&&statistics({1,2,3,4,100},100).misses==0,"deadline excursions are strictly greater");
    check(std::abs(deadlineNs(48000,128)/1e9-128.0/48000.0)<1e-15,"48k/128 seconds conversion");
    rejects([&]{statistics({},4);},"reject empty observations");
    rejects([&]{statistics({-1},4);},"reject negative observations");
    rejects([&]{statistics({1},std::numeric_limits<double>::infinity());},"reject nonfinite deadline");
    rejects([&]{deadlineNs(0,128);},"reject invalid sample rate");
    juce::AudioBuffer<float> audio(2,2); audio.clear();AudioResult zero;scan(zero,audio);
    audio.setSample(0,0,-0.0f);AudioResult minus;scan(minus,audio);
    check(zero.checksum!=minus.checksum,"float-bit checksum distinguishes signed zero");
    audio.setSample(0,0,.125f);AudioResult changed;scan(changed,audio);
    check(changed.checksum!=zero.checksum&&changed.peak==.125&&changed.nonzero==1,"raw changed sample checksum/peak");
    audio.setSample(1,1,std::numeric_limits<float>::quiet_NaN());AudioResult invalid;scan(invalid,audio);
    check(!invalid.finite,"nonfinite raw audio detected");
    for(std::size_t n=0;n<names.size();++n) {
        try {
            const auto dStorage=testing::fixtureValue([&]{return workload(names[n]);});const auto& d=*dStorage;juce::MemoryBlock encoded;auto decodedStorage=std::make_unique<SoundDocument>();auto& decoded=*decodedStorage;auto pStorage=std::make_unique<RoutedPatch>();auto& p=*pStorage;
            check(encodeSoundState(d,encoded)&&encoded.getSize()==currentSoundStateSize&&decodeSoundState(encoded.getData(),encoded.getSize(),decoded)&&makeRoutedPatch(decoded,p),"current workload codec and revision1 patch");
            const bool full=n>=2;
            check(decoded.layout.synthCount==(full?4:1)&&p.synth.performance.mode==VoiceMode::polyphonic&&p.synth.masterLevel==.5f,"decoded rack capacity and ordinary poly/master");
            check(std::abs(p.outputGainDb-(full?12:30))<.0001&&p.outputProtection,"decoded calibrated gain/protection");
            const int fx=std::accumulate(decoded.layout.effectCount.begin(),decoded.layout.effectCount.end(),0);
            check(fx==(full?8:0),"decoded eight-effect capacity");
            for(int r=0;r<(full?4:1);++r) {
                const auto& l=p.synth.layers[std::size_t(r)];
                const bool wt=n==1||n==3||(n>=4&&r%2==1);
                check(l.enabled&&l.engine==(wt?EngineKind::wavetable:EngineKind::analog)&&!p.mute[std::size_t(r)],"all claimed layers active with prescribed engine");
                check(std::abs(l.cutoffHz-4000)<.02&&std::abs(l.resonance-.25f)<1e-6&&std::abs(l.ampEnvelope.attack-.005f)<1e-5&&std::abs(l.ampEnvelope.decay-.15f)<1e-5&&l.ampEnvelope.sustain==1&&std::abs(l.ampEnvelope.release-.15f)<1e-5,"ordinary filter and nondecaying held envelope");
                for(const auto& o:l.oscillators)check(wt?std::abs(o.position-.7f)<1e-6:o.waveform==Waveform::saw,"saw banks and active wavetable position");
                if(full) {
                    check(l.lfos[0].shape==LfoShape::sine&&!l.lfos[0].synced&&std::abs(l.lfos[0].rateHz-3)<1e-6&&l.lfos[1].shape==LfoShape::triangle&&l.lfos[1].synced&&std::abs(l.lfos[1].beats-.25f)<1e-6,"both prescribed LFOs");
                    const std::array<ModTarget,8> targets{ModTarget::level,ModTarget::pan,ModTarget::cutoff,ModTarget::osc1Tune,ModTarget::osc2Detune,ModTarget::osc1Level,ModTarget::resonance,wt?ModTarget::osc1Position:ModTarget::osc1Width};
                    for(int k=0;k<16;++k){const auto& route=l.modulation[std::size_t(k)];check(route.enabled&&route.source==static_cast<ModSource>(1+k%6)&&route.target==targets[std::size_t(k%8)]&&route.bank==(k%8<2?ModBank::common:(wt?ModBank::wavetable:ModBank::analog))&&std::abs(route.amount-(k%2?-.05f:.05f))<1e-6,"all64 routes active with prescribed source target bank depth");}
                }
            }
            for(int f=0;f<fx;++f){const auto& e=p.effects[std::size_t(f)];check(!e.bypassed,"all effects unbypassed");
                if(n==4)check(decoded.layout.effects[std::size_t(f)].kind==EffectKind::delay&&decoded.layout.effects[std::size_t(f)].chain==masterChain&&!e.delay.synced&&e.delay.seconds==4&&std::abs(e.delay.feedback-.95f)<1e-6&&std::abs(e.delay.mix-.35f)<1e-6&&std::abs(e.delay.filterHz-12000)<.01&&e.delay.stereo==.5f,"eight maximum-history delays");
                else if(n==5)check(decoded.layout.effects[std::size_t(f)].kind==EffectKind::reverb&&decoded.layout.effects[std::size_t(f)].chain==masterChain&&e.reverb.size==1&&e.reverb.decaySeconds==20&&e.reverb.predelayMs==250&&e.reverb.damping==.5f&&std::abs(e.reverb.mix-.35f)<1e-6,"eight maximum reverbs");
                else check(decoded.layout.effects[std::size_t(f)].kind==static_cast<EffectKind>(f%4)&&decoded.layout.effects[std::size_t(f)].chain==(f<4?f:4)&&std::abs(e.delay.seconds-.25f)<1e-6&&std::abs(e.delay.feedback-.35f)<1e-6&&std::abs(e.delay.mix-.25f)<1e-6&&std::abs(e.reverb.decaySeconds-2)<1e-5&&std::abs(e.reverb.mix-.25f)<1e-6,"mixed insert and master ordinary defaults");
            }
        }catch(const std::exception& e){check(false,e.what());}
    }
    const auto script=events(48000,128);
    check(script.size()==10,"all ten lifecycle event groups");
    const std::array<Ns,10> requested{4800,12000,19200,33600,48000,60000,72000,76800,81600,88800};
    const std::array<Ns,10> actual{4800,12032,19200,33664,48000,60000,72000,76800,81664,88832};
    for(std::size_t i=0;i<script.size()&&i<10;++i)check(script[i].requested==requested[i]&&script[i].actual==actual[i],"independent MIDI offsets and control boundary schedule");
    for(const auto& kind:{"steady","lifecycle","multi"}) {
        try {
            const auto report=runTrial({"full-wavetable-mixed",kind,48000,128,juce::String(kind)=="multi"?4:1,1});
            check(bool(report["integrity"]),"actual processor smoke report integrity");
            check(int(report["measured_frames"])==(juce::String(kind)=="lifecycle"?96000:48000)&&int(report["warmup_frames"])==12032,"actual rounded smoke geometry");
            check(int(report["initial_note_count"])==16&&double(report["raw_peak"])>0&&bool(report["raw_finite"]),"actual sixteen-note finite nonzero audio");
            validateTrial(report);
            auto bad=juce::JSON::parse(juce::JSON::toString(report));if(auto* obj=bad.getDynamicObject())obj->setProperty("measured_frames",1);
            rejects([&]{validateTrial(bad);},"reject inconsistent report geometry");
            auto badAudio=juce::JSON::parse(juce::JSON::toString(report));if(auto* obj=badAudio.getDynamicObject())obj->setProperty("raw_finite",false);
            rejects([&]{validateTrial(badAudio);},"reject nonfinite audio report");
            auto badNotes=juce::JSON::parse(juce::JSON::toString(report));if(auto* obj=badNotes["initial_midi"][0].getDynamicObject())obj->setProperty("note",47);
            rejects([&]{validateTrial(badNotes);},"reject wrong initial held pitch");
            if(juce::String(kind)=="lifecycle"){
                auto badEvent=juce::JSON::parse(juce::JSON::toString(report));if(auto* obj=badEvent["events"][4].getDynamicObject())obj->setProperty("actual_frame",48001);
                rejects([&]{validateTrial(badEvent);},"reject mistimed baseline recall");
                auto badMidi=juce::JSON::parse(juce::JSON::toString(report));if(auto* obj=badMidi["events"][4]["midi"][0].getDynamicObject())obj->setProperty("type","note_off");
                rejects([&]{validateTrial(badMidi);},"reject missing recall retrigger");
            }
            if(juce::String(kind)=="lifecycle")check(bool(report["missing_tempo_observed"])&&bool(report["recovered_tempo_observed"])&&int(report["event_count"])==10,"actual missing/recovered tempo and exact lifecycle events");
            if(juce::String(kind)=="multi")check(int(report["instance_count"])==4&&report["serial_ns"].size()==375,"actual four-instance serial observations");
        }catch(const std::exception& e){check(false,e.what());}
    }
    std::cout<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}
// END SELF TEST
// BEGIN HISTORY PROFILE SELF TEST -- identical compiled body for RED and GREEN.
int historyProfileSelfTest()
{
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* what){++checks;if(!ok){++failed;std::cerr<<"FAIL: "<<what<<'\n';}};
    auto rejects=[&](auto action,const char* what){bool caught=false;try{action();}catch(const std::exception&){caught=true;}check(caught,what);};
    const auto root=juce::File(RACKSYNTH_WORKSPACE);
    const auto fixture=root.getChildFile("build/m2-history-task2-cli-selftest-"+juce::Uuid().toString());
    require(fixture.createDirectory().wasOk(),"self-test fixture directory failed");
    const auto output=fixture.getChildFile("absent-output").getFullPathName();
    const auto metadata=fixture.getChildFile("inventory.json");
    auto cli=[&](const juce::String& path){return juce::StringArray{"--profile","--suite","smoke","--output",output,"--inventory",path};};
    require(metadata.replaceWithText("{\"marker\":\"current\"}"),"self-test metadata write failed");
    try {const auto parsed=parseArguments(cli(metadata.getFullPathName()));check(parsed.inventory==metadata,"accept explicit object inventory and retain exact supplied file");}
    catch(const std::exception&){check(false,"accept explicit object inventory and retain exact supplied file");}
    auto missing=cli(metadata.getFullPathName());missing.remove(missing.size()-1);
    rejects([&]{parseArguments(missing);},"reject missing inventory value");
    auto duplicate=cli(metadata.getFullPathName());duplicate.add("--inventory");duplicate.add(metadata.getFullPathName());
    rejects([&]{parseArguments(duplicate);},"reject duplicate inventory option");
    rejects([&]{parseArguments(cli("inventory.json"));},"reject relative inventory");
    rejects([&]{parseArguments(cli(fixture.getFullPathName()));},"reject inventory directory");
    rejects([&]{parseArguments(cli(fixture.getChildFile("missing.json").getFullPathName()));},"reject missing inventory file");
    rejects([&]{parseArguments(cli(juce::File::getSpecialLocation(juce::File::windowsSystemDirectory).getChildFile("kernel32.dll").getFullPathName()));},"reject outside workspace inventory");
    for(const auto& invalid:{"not json","[]","null","42","true","\"text\"","{} garbage","{}{}","{\"x\":1,}","{\"x\":[1,]}","{\"x\":01}","{\"x\":.5}","{\"x\":+1}","{\"x\":1.}","{\"x\":\"a\n b\"}","{\"x\":\"\\q\"}","{\"x\":\"\\u00zz\"}"}){
        require(metadata.replaceWithText(invalid),"self-test metadata rewrite failed");
        rejects([&]{parseArguments(cli(metadata.getFullPathName()));},"reject invalid or nonobject JSON inventory");
    }
    require(metadata.replaceWithText("{\"\": [{\"nested\":true},null,-1.25e+3,\"escaped\\\"brace}\\u00e9\"],\"object\":{}}"),"self-test nested metadata write failed");
    try {check(parseArguments(cli(metadata.getFullPathName())).inventory==metadata,"accept complete nested JSON including empty property name");}
    catch(const std::exception&){check(false,"accept complete nested JSON including empty property name");}
    require(metadata.replaceWithText("{}"+juce::String::repeatedString(" ",256*1024-2)),"self-test bound write failed");
    try {check(parseArguments(cli(metadata.getFullPathName())).inventory==metadata,"accept exactly256KiB object inventory");}
    catch(const std::exception&){check(false,"accept exactly256KiB object inventory");}
    require(metadata.appendText(" "),"self-test excess write failed");
    rejects([&]{parseArguments(cli(metadata.getFullPathName()));},"reject inventory above256KiB");
    require(metadata.replaceWithText("{}"),"self-test readable write failed");
#if JUCE_WINDOWS
    const auto handle=CreateFileW(metadata.getFullPathName().toWideCharPointer(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"self-test unreadable fixture lock failed");
    rejects([&]{parseArguments(cli(metadata.getFullPathName()));},"reject unreadable inventory");
    CloseHandle(handle);
#endif
    const auto valid=runTrial({"full-wavetable-mixed","multi",48000,128,4,1});
    for(const auto& invalid:std::vector<J>{integer(-1),J("not-a-number"),J(1.5),J(true),J()}){
        auto mutated=juce::JSON::parse(juce::JSON::toString(valid));mutated["serial_ns"].getArray()->set(0,invalid);
        rejects([&]{validateTrial(mutated);},"reject negative or noninteger serial timing");
    }
    auto zero=juce::JSON::parse(juce::JSON::toString(valid));zero["serial_ns"].getArray()->set(0,integer(0));
    try {validateTrial(zero);check(true,"zero integer serial timing is valid");}catch(const std::exception&){check(false,"zero integer serial timing is valid");}
    auto rejected=juce::JSON::parse(juce::JSON::toString(valid));put(rejected,"measured_frames",1);
    check(!acceptTrial(rejected),"trial rejection returns false for root failure");
    check(!bool(rejected["integrity"])&&rejected["validation_error"].toString().isNotEmpty(),"rejected trial integrity false before reporting");
    const auto rejectedFile=fixture.getChildFile("rejected.json");writeJson(rejectedFile,rejected);
    check(!bool(juce::JSON::parse(rejectedFile)["integrity"]),"standalone rejected trial persists false integrity");
    auto accepted=juce::JSON::parse(juce::JSON::toString(valid));
    check(acceptTrial(accepted)&&bool(accepted["integrity"]),"valid trial still accepted");
    std::cout<<"history profile "<<checks<<" checks, "<<failed<<" failures; fixtures="<<fixture.getFullPathName()<<'\n';
    return failed?1:0;
}
// END HISTORY PROFILE SELF TEST
}
namespace {
ProjectDocument patternWorkload(bool dense)
{
    ProjectDocument result;
    result.sound=workload("full-wavetable-mixed");
    result.mode=PlaybackMode::internalPattern;
    result.pattern.noteCount=dense?256:16;
    result.pattern.grid=dense?PatternGrid::sixtyFourth:PatternGrid::sixteenth;
    result.pattern.swing=dense?.7:0;
    for(int k=0;k<result.pattern.noteCount;++k) {
        const double start=dense?k/64.0:0;
        result.pattern.notes[std::size_t(k)]={start,dense?std::min(.5,4-start):4,static_cast<std::uint8_t>(48+k%16),100,1};
    }
    require(validateProjectDocument(result),"Invalid Internal pattern workload");
    return result;
}
std::vector<Geometry> patternMatrix()
{
    std::vector<Geometry> result;
    for(const auto* name:{"typical","dense"})
        for(const auto geometry:std::array<std::pair<int,int>,3>{{{48000,64},{48000,512},{96000,512}}})
            for(int trial=1;trial<=3;++trial) result.push_back({name,"pattern",geometry.first,geometry.second,1,trial});
    return result;
}
J patternTrial(const Geometry& g,const ProjectDocument& document)
{
    struct PatternPlayhead final:juce::AudioPlayHead {
        PositionInfo position;
        juce::Optional<PositionInfo> getPosition() const override {return position;}
    } playhead;
    const auto warmup=framesFor(.25,g.rate,g.block),frames=framesFor(1,g.rate,g.block);
    const auto count=std::size_t(frames/g.block);
    std::vector<Ns> raw(count); std::vector<AudioResult> audio(count);
    juce::AudioBuffer<float> buffer(2,g.block); juce::MidiBuffer midi; midi.ensureSize(65536);
    InstrumentProcessor processor; processor.setPlayHead(&playhead);
    juce::MemoryBlock state; require(encodeProjectState(document,state) && processor.restoreProjectState(state.getData(),int(state.getSize())),"Pattern trial restore failed");
    processor.prepareToPlay(g.rate,g.block);
    playhead.position.setBpm(120); playhead.position.setIsPlaying(true);
    for(Ns frame=0;frame<warmup+frames;frame+=g.block) {
        playhead.position.setTimeInSamples(frame); playhead.position.setPpqPosition(double(frame)*2/g.rate);
        midi.clear(); buffer.clear();
        if(frame<warmup) { processor.processBlock(buffer,midi); continue; }
        const auto n=std::size_t((frame-warmup)/g.block);
        const auto start=Clock::now(); processor.processBlock(buffer,midi); raw[n]=elapsed(start);
        scan(audio[n],buffer);
    }
    processor.releaseResources(); processor.setPlayHead(nullptr);
    auto report=object(); put(report,"pattern",g.workload); put(report,"sample_rate",g.rate); put(report,"block_size",g.block); put(report,"trial",g.trial);
    put(report,"warmup_frames",integer(warmup)); put(report,"measured_frames",integer(frames)); put(report,"sound",documentJson(document.sound));
    put(report,"mode","Internal Pattern"); put(report,"note_count",int(document.pattern.noteCount)); put(report,"fixture_sha256",juce::SHA256(state).toHexString());
    J durations(juce::Array<J>{}),observations(juce::Array<J>{}); bool finite=true; double peak=0;
    for(std::size_t n=0;n<count;++n) { durations.append(integer(raw[n])); observations.append(audioJson(audio[n])); finite=finite&&audio[n].finite; peak=std::max(peak,audio[n].peak); }
    put(report,"callback_ns",durations); put(report,"audio_observations",observations); put(report,"summary",summary(raw,deadlineNs(g.rate,g.block)));
    const auto stats=statistics(raw,deadlineNs(g.rate,g.block));
    put(report,"raw_finite",finite); put(report,"raw_peak",peak); put(report,"integrity",finite&&peak>0&&peak<1);
    put(report,"target_met",stats.mean<=.75*stats.deadline && stats.p99<=stats.deadline);
    return report;
}
int profilePatterns(const juce::File& output,const juce::File& suppliedInventory)
{
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Pattern timing requires Release");
    const auto inventory=inventoryMetadata(suppliedInventory);
    require(std::filesystem::create_directory(std::filesystem::path(output.getFullPathName().toWideCharPointer())),"Fresh pattern output creation failed");
    auto report=object(); put(report,"schema",1); put(report,"suite","pattern"); put(report,"configuration",RACKSYNTH_CONFIGURATION);
    put(report,"scope","18 native prepared InstrumentProcessor trials only; no host underrun or universal deadline claim; all outliers retained");
    put(report,"timed_work","processBlock only plus clock overhead; sample/PPQ preparation, clearing, scans, JSON and state outside measured intervals");
    put(report,"inventory",inventory); const auto before=identities(); put(report,"identities_before",before);
    J fixtures(juce::Array<J>{});
    for(bool dense:{false,true}) {
        const auto document=patternWorkload(dense); juce::MemoryBlock bytes;
        require(encodeProjectState(document,bytes),"Pattern fixture encode failed");
        const auto file=output.getChildFile(dense?"dense.aiproject":"typical.aiproject");
        require(!file.exists()&&file.replaceWithData(bytes.getData(),bytes.getSize()),"Pattern fixture write failed");
        juce::MemoryBlock readback; ProjectDocument decoded;
        require(file.loadFileAsData(readback)&&readback==bytes&&decodeProjectState(readback.getData(),readback.getSize(),decoded)&&decoded==document,"Lossless fixture readback failed");
        auto entry=object(); put(entry,"path",file.getFullPathName()); put(entry,"bytes",integer(Ns(bytes.getSize()))); put(entry,"sha256",juce::SHA256(bytes).toHexString()); fixtures.append(entry);
    }
    put(report,"fixtures",fixtures); J trials(juce::Array<J>{}); bool integrity=true,target=true; int number=0;
    for(const auto& geometry:patternMatrix()) {
        auto trial=patternTrial(geometry,patternWorkload(geometry.workload=="dense"));
        integrity=integrity&&bool(trial["integrity"]); target=target&&bool(trial["target_met"]);
        writeJson(output.getChildFile("trial-"+juce::String(++number).paddedLeft('0',2)+".json"),trial); trials.append(trial);
        std::cout<<number<<"/18 native "<<geometry.workload<<' '<<geometry.rate<<'/'<<geometry.block<<" target="<<bool(trial["target_met"])<<std::endl;
    }
    const auto after=identities(); put(report,"identities_after",after); const bool stable=juce::JSON::toString(before)==juce::JSON::toString(after);
    put(report,"identities_unchanged",stable); put(report,"trials",trials); put(report,"integrity",integrity&&stable); put(report,"targets_met",target);
    writeJson(output.getChildFile("pattern-profile-report.json"),report);
    return integrity&&stable&&target?0:1;
}
// BEGIN M3 PROFILE SELF TEST -- freeze whole region for RED/GREEN.
int patternProfileSelfTest()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL M3: " << label << '\n'; } };
    const auto fresh = juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/m3-pattern-cli-selftest-absent").getFullPathName();
    try { check(parseArguments({"--profile","--suite","pattern","--output",fresh}).suite == "pattern","bounded pattern CLI admitted"); }
    catch (const std::exception&) { check(false,"bounded pattern CLI admitted"); }
    for (bool dense : {false,true}) {
        try {
            const auto d = patternWorkload(dense);
            check(d.mode == PlaybackMode::internalPattern && d.pattern.bars == 1 && d.pattern.meter == PhraseMeter{4,4}, "explicit internal 4/4 one-bar fixture");
            check(d.sound == workload("full-wavetable-mixed"), "exact existing full four-rack/eight-effect/64-route sound reused");
            check(d.pattern.noteCount == (dense ? 256 : 16) && d.pattern.swing == (dense ? .7 : 0), "fixed typical/dense capacities");
            for (int k = 0; k < d.pattern.noteCount; ++k) {
                const auto& n = d.pattern.notes[std::size_t(k)];
                check(n.startBeats == (dense ? k/64.0 : 0) && n.durationBeats == (dense ? std::min(.5,4-k/64.0) : 4)
                    && n.pitch == 48+k%16 && n.velocity == 100 && n.channel == 1,"literal sustained sixteen-voice fixture geometry");
            }
            juce::MemoryBlock bytes; ProjectDocument readback;
            check(encodeProjectState(d,bytes) && decodeProjectState(bytes.getData(),bytes.getSize(),readback) && readback == d,"exact AIPJ fixture roundtrip");
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; check(false,"pattern workload implemented"); }
    }
    const auto matrix = patternMatrix(); check(matrix.size() == 18,"exact eighteen trials");
    if (matrix.size() == 18) for (std::size_t k = 0; k < matrix.size(); ++k) {
        const auto& g = matrix[k]; const int geometry = int(k%9)/3;
        check(g.rate == (geometry == 2 ? 96000 : 48000) && g.block == (geometry == 0 ? 64 : 512)
            && g.trial == int(k%3)+1 && g.count == 1,"literal timing geometries and repetitions");
        check(framesFor(.25,g.rate,g.block) == (geometry == 0 ? 12032 : geometry == 1 ? 12288 : 24064)
            && framesFor(1,g.rate,g.block) == (geometry == 0 ? 48000 : geometry == 1 ? 48128 : 96256),"literal rounded warmup/measurement counts");
    }
    const auto known = statistics({100,200,300,400},250);
    check(known.mean == 250 && known.p99 == 400 && known.maximum == 400 && known.misses == 2,"raw timing statistics preserve excursions");
    std::cout << "M3 profile helper " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
// END M3 PROFILE SELF TEST
}

namespace {
std::vector<Geometry> productionMatrix()
{
    std::vector<Geometry> result;
    for(const auto* name:{"full-wavetable-mixed","full-reverb"})for(const int rate:{44100,48000,96000})for(const int block:{64,128,256,512,1024})result.push_back({name,"steady",rate,block,1,1});
    result.insert(result.end(),{{"full-wavetable-mixed","lifecycle",48000,64,1,1},{"full-wavetable-mixed","lifecycle",48000,512,1,1},{"full-wavetable-mixed","lifecycle",96000,512,1,1},
        {"full-wavetable-mixed","multi",48000,128,4,1},{"full-wavetable-mixed","multi",96000,512,4,1}});
    for(const auto pair:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int trial=1;trial<=3;++trial)result.push_back({"full-wavetable-mixed","linked-contention",pair.first,pair.second,4,trial});
    return result;
}
juce::String productionClassification(const Geometry& g)
{return (g.kind=="steady"||g.kind=="lifecycle")&&!(g.rate==96000&&g.block<256)?"historical-qualified":"stress";}
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
struct ProductionHead final:juce::AudioPlayHead
{
    PositionInfo position;
    juce::Optional<PositionInfo> getPosition() const override{return position;}
    void advance(Ns frame,int rate){position.setTimeInSamples(frame);position.setPpqPosition(double(frame)*2/rate);position.setBpm(120);position.setTimeSignature(TimeSignature{4,4});position.setIsPlaying(true);}
};
struct ProductionObservation
{
    Ns ns=0,start=0,end=0;AudioResult audio;
    int activeBefore=0,activeAfter=0,startedBefore=0,startedAfter=0,completedBefore=0,completedAfter=0;
};
struct ProductionInstance
{
    ProductionHead head;std::unique_ptr<InstrumentProcessor> processor;juce::AudioBuffer<float> audio;juce::MidiBuffer midi;std::vector<ProductionObservation> observations;
    ProductionInstance(const companion::ClientOptions& options,int block,std::size_t blocks):processor(std::make_unique<InstrumentProcessor>(options)),audio(2,block),observations(blocks){midi.ensureSize(65536);processor->setPlayHead(&head);}
};
struct ProductionRenderJob
{Ns start=0,end=0;ai::RenderStatus status=ai::RenderStatus::renderFailure;std::size_t body=0,tail=0;bool finite=false;};
struct ProductionRenderer
{
    std::atomic_bool ended=false,cancel=false,done=false,priorityAttempted=false,prioritySet=false;
    std::atomic_int started=0,active=0,completed=0,ready=0,cancellationChecks=0;
    std::mutex starts,completion;std::condition_variable finished;std::array<ProductionRenderJob,3> jobs{};std::thread thread;
    ~ProductionRenderer(){stopStarting();cancel=true;if(thread.joinable())thread.join();}
    void stopStarting(){std::lock_guard lock(starts);ended=true;}
};
J productionPublication(const ProjectDocument& d,std::uint64_t edit)
{
    juce::MemoryBlock bytes;require(encodeProjectState(d,bytes),"production publication codec failed");auto result=object();
    put(result,"edit_revision",integer(Ns(edit)));put(result,"native_aipj_base64",juce::Base64::toBase64(bytes.getData(),bytes.getSize()));put(result,"bytes",integer(Ns(bytes.getSize())));put(result,"sha256",juce::SHA256(bytes).toHexString());return result;
}
SoundDocument profilePersonalityState();

// The RuntimeServer worker receives requests submitted by public generateAi.
// A labelled structured fixture replaces inference only; native admission,
// renderPreview, request/result wire codecs and public result readmission run.
struct Step7NativeAi
{
    const Clock::time_point origin;
    std::atomic_int accounts=0,jobs=0,providerCalls=0,ready=0;
    std::atomic_bool active=false;
    std::atomic<Ns> start=0,end=0,generationStart=0,generationEnd=0;
    std::mutex mutex;J records{juce::Array<J>{}};
    explicit Step7NativeAi(Clock::time_point o):origin(o){}
    companion::Frame run(const companion::RuntimeJob& job)
    {
        using namespace companion;
        if(const auto* r=std::get_if<ai::GenerationRequest>(&job.input)){
            require(++jobs==1,"Step7 fixture permits one generation record");start=elapsed(origin);active=true;
            auto record=object();put(record,"job_id",juce::String(std::to_string(job.jobId)));put(record,"session",job.session.toDashedString());
            juce::MemoryBlock requestBytes,projectBytes;
            require(r->catalog&&r->catalog->authority&&ai::detail::catalogRequestValid(*r)&&job.groupContext,"Step7 missing captured authority");
            std::unique_ptr<ai::LocalGenerationRequest> local;
            if(job.localPreview){
                require(job.previewExchange!=nullptr,"Step7 owned preview exchange missing");
                local=std::make_unique<ai::LocalGenerationRequest>();local->request=*r;local->local=*job.localPreview;
                require(encodeLocalRequest(*local,requestBytes),"Step7 owned request codec");
            }else require(encodeRequest(*r,requestBytes),"Step7 legacy request codec");
            require(encodeProjectState(r->source,projectBytes),"Step7 source codec");
            put(record,"request_codec",local?"owned_local":"legacy_pcm");
            put(record,"request_sha256",juce::SHA256(requestBytes).toHexString());put(record,"request_base64",juce::Base64::toBase64(requestBytes.getData(),requestBytes.getSize()));put(record,"source_project_sha256",juce::SHA256(projectBytes).toHexString());
            put(record,"source_project_base64",juce::Base64::toBase64(projectBytes.getData(),projectBytes.getSize()));
            put(record,"runtime",r->tag.runtime.toDashedString());put(record,"request_id",juce::String(std::to_string(r->tag.requestId)));put(record,"edit_revision",juce::String(std::to_string(r->tag.editRevision)));put(record,"context_revision",juce::String(std::to_string(r->tag.contextRevision)));
            put(record,"catalog_snapshot",r->catalog->authority->snapshot.toDashedString());put(record,"catalog_root_request_id",juce::String(std::to_string(r->catalog->root.requestId)));put(record,"catalog_descriptor_sha256",juce::SHA256(r->catalog->authority->descriptorJson.data(),r->catalog->authority->descriptorJson.size()).toHexString());
            put(record,"catalog_phase",int(r->catalog->phase));put(record,"captured_bpm",r->capturedBpm?*r->capturedBpm:0);put(record,"captured_peer_count",int(job.groupContext->peerCount));put(record,"source_document",documentJson(r->source.sound));
            generationStart=elapsed(origin);
            unsigned providerAttempt=r->catalog&&r->catalog->phase==ai::CatalogPhase::materialize?r->catalog->inferenceCallsUsed:0;
            std::uint8_t renderOrdinal=0;
            const ai::StructuredCall provider=[&](const juce::String&,std::string_view,const J&){
                ++providerCalls;++providerAttempt;require(r->catalog->baseIntent.catalogSelections.empty(),"Step7 fresh request unexpectedly carries selections");
                rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::connected;response.result.httpStatus=200;
                response.text=R"json({"version":2,"parameter_edits":[{"id":"output.gain_db","normalized":0.4}],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"Task13 synthetic technical full-capacity candidate","explanation":"Structured fixture only; actual public queue, wire, captured authority, native admission and rendering","name":"Profile Test Sound","reconciliation":{"detach":[],"remove":[],"replace":[],"controls":[]},"catalog_selections":[],"catalog_parameter_edits":[]})json";
                ai::ProposalEdits parsed;require(ai::decodeProposal(response.text.toStdString(),r->policy,*r->catalog->authority,parsed),"Step7 synthetic response invalid against captured authority");return response;
            };
            const ai::CancelCheck cancelled=[token=job.cancellation]{return token&&token->cancelled();};
            std::unique_ptr<ai::LocalGenerationResult> localGenerated;
            std::unique_ptr<ai::GenerationResult> legacyGenerated;
            if(local){
                const ai::LocalPreviewEvaluator evaluate=[&](const auto& candidate,const auto& edits,const auto& context,const auto& trace,const auto& stop){
                    auto value=std::make_unique<ai::PreviewEvaluate>();value->edits=edits;value->context=context;auto& b=value->binding;
                    b.request=r->tag;b.root=local->local.root;b.catalogSnapshot=local->local.catalogSnapshot;b.job=job.jobId;
                    b.structuralGeneration=r->structuralGeneration;b.reset=local->local.reset;b.sourceWord=local->local.sourceWord;
                    b.phase=r->catalog?r->catalog->phase:ai::CatalogPhase::select;b.providerAttempt=std::uint8_t(providerAttempt);
                    b.renderOrdinal=++renderOrdinal;b.sourceMap=local->local.sourceMapDigest;ai::PreviewSampleMap staged;
                    require(ai::stagePreviewMap(local->local.sourceMap,trace,staged)&&ai::digestPreviewMap(staged,b.stagedMap)
                        &&ai::digestPreviewCandidate(candidate,b.candidate)&&ai::digestPreviewContext(context,b.context),"Step7 owned candidate binding");
                    return job.previewExchange->evaluate(*value,stop);
                };
                localGenerated.reset(new ai::LocalGenerationResult(ai::generateLocalProposal(*local,provider,evaluate,cancelled,job.groupContext.get())));
            }else legacyGenerated.reset(new ai::GenerationResult(ai::generateProposal(*r,provider,cancelled,job.groupContext.get())));
            const auto& generated=localGenerated?localGenerated->numeric:*legacyGenerated;
            const auto* receipt=localGenerated&&localGenerated->rendered?&*localGenerated->rendered:nullptr;
            const auto bodyFrames=localGenerated?(receipt?receipt->bodyFrames:0):generated.audio.bodyFrames;
            const auto totalFrames=localGenerated?(receipt?receipt->frames:0):generated.audio.left.size();
            const bool terminalGeometry=localGenerated
                ?receipt&&receipt->status==ai::RenderStatus::ready&&receipt->failure==ai::PreviewFailure::none
                    &&ai::validPreviewReceipt(*receipt)&&ai::validateLocalGenerationResult(*local,*localGenerated)
                    &&generated.audio.bodyFrames==0&&generated.audio.left.empty()&&generated.audio.right.empty()
                    &&bodyFrames>0&&totalFrames==bodyFrames+ai::previewTailFrames
                :generated.audio.bodyFrames>0&&generated.audio.left.size()==generated.audio.bodyFrames+ai::previewTailFrames
                    &&generated.audio.right.size()==generated.audio.left.size();
            put(record,"owned_local_preview",bool(localGenerated));
            put(record,"terminal_pcm_frames",integer(Ns(generated.audio.left.size()+generated.audio.right.size())));
            put(record,"native_generation_interval_scope","Existing full generation interval, including IO/receipt waiting; not per-render-call coverage");
            if(receipt){put(record,"receipt_status",int(receipt->status));put(record,"receipt_failure",int(receipt->failure));}
            generationEnd=elapsed(origin);put(record,"native_generation_start_ns",integer(generationStart.load()));put(record,"native_generation_end_ns",integer(generationEnd.load()));put(record,"generation_status",int(generated.status));put(record,"proposal_status",int(generated.proposalStatus));put(record,"render_status",int(generated.renderStatus));put(record,"inference_calls",int(generated.inferenceCalls));put(record,"native_preview_quality",localGenerated?"realtime: actual owned local renderer, receipt measurements; independent of selected source quality":"realtime: current generateProposal default, independently of selected source quality");put(record,"body_frames",integer(Ns(bodyFrames)));put(record,"total_frames",integer(Ns(totalFrames)));put(record,"native_audio_finite",generated.measurements.finite);put(record,"native_audio_peak",generated.measurements.peak);put(record,"error",generated.errorCode);
            bool candidateValid=false;if(generated.candidate){juce::MemoryBlock state;auto decoded=std::make_unique<ProjectDocument>();candidateValid=validateProjectDocument(*generated.candidate)&&encodeProjectState(*generated.candidate,state)&&decodeProjectState(state.getData(),state.getSize(),*decoded)&&*decoded==*generated.candidate;put(record,"candidate_project_sha256",juce::SHA256(state).toHexString());put(record,"candidate_project_base64",juce::Base64::toBase64(state.getData(),state.getSize()));put(record,"candidate_document",documentJson(generated.candidate->sound));}
            put(record,"candidate_project_valid",candidateValid);
            const bool valid=generated.status==ai::GenerationStatus::ready&&generated.proposalStatus==ai::ProposalStatus::accepted&&generated.renderStatus==ai::RenderStatus::ready&&candidateValid&&generated.measurements.finite&&terminalGeometry&&generated.catalog&&generated.catalog->root==r->catalog->root&&generated.catalog->snapshot==r->catalog->authority->snapshot;
            put(record,"integrity",valid);if(valid)++ready;
            Frame frame{localGenerated?MessageKind::resultLocal:MessageKind::result,job.session,{}};
            if(localGenerated)require(encodeLocalResult(*local,*localGenerated,frame.payload),"Step7 owned result wire");
            else require(encodeResult(*r,generated,frame.payload),"Step7 native result wire");put(record,"result_wire_sha256",juce::SHA256(frame.payload).toHexString());put(record,"result_wire_bytes",integer(Ns(frame.payload.getSize())));
            end=elapsed(origin);active=false;put(record,"start_ns",integer(start.load()));put(record,"end_ns",integer(end.load()));
            {std::lock_guard lock(mutex);records.append(record);}return frame;
        }
        const auto command=std::get<AccountCommand>(job.input);require(command.action==AccountAction::status||command.action==AccountAction::models,"Step7 fixture cannot mutate account/model");++accounts;
        AccountReply reply;reply.providerStatus=rack::oauth::Status::connected;reply.httpStatus=200;reply.selectedModel="task13-synthetic-technical";reply.modelVerified=true;reply.models={{"task13-synthetic-technical","Task13 technical fixture only"}};
        Frame frame{MessageKind::accountResult,job.session,{}};require(encodeAccountReply(reply,frame.payload),"Step7 account fixture wire");return frame;
    }
    J report(){std::lock_guard lock(mutex);auto j=object();put(j,"label","Synthetic structured response and isolated synthetic discovered account/model; not configured account or real inference");put(j,"account_jobs",accounts.load());put(j,"generation_jobs",jobs.load());put(j,"structured_calls",providerCalls.load());put(j,"ready",ready.load());put(j,"active",active.load());put(j,"jobs",juce::JSON::parse(juce::JSON::toString(records)));return j;}
};

J linkedTrial(const Geometry& g,std::size_t reducedBlocks,bool personalities=false,const SoundDocument* supplied=nullptr,RenderQuality quality=RenderQuality::realtime,bool nativeAi=false)
{
    require(g.kind=="linked-contention"&&g.workload=="full-wavetable-mixed"&&g.count==4&&((g.rate==48000&&g.block==128)||(g.rate==96000&&g.block==512)),"invalid linked geometry");
    require(reducedBlocks==0||reducedBlocks==32,"invalid bounded selftest size");
    const auto origin=Clock::now();const auto cpuBefore=cpuTime();const Ns warmup=framesFor(.25,g.rate,g.block),frames=reducedBlocks?Ns(reducedBlocks)*g.block:framesFor(2,g.rate,g.block);
    const auto blocks=std::size_t(frames/g.block);auto result=object();J controls(juce::Array<J>{}),before(juce::Array<J>{}),after(juce::Array<J>{});
    std::array<std::unique_ptr<ProductionInstance>,4> instances;std::array<std::unique_ptr<ProjectDocument>,4> initial;std::array<std::uint64_t,4> initialEdits{};
    std::vector<Ns> serial(blocks);std::size_t completedBlocks=0;std::array<GroupUuid,3> selection{};GroupSelection capturedSelection;
    auto backendOwner=std::make_shared<Step7NativeAi>(origin);auto& backend=*backendOwner;bool aiSubmitted=false,aiDelivered=false,requestAuthority=false;Ns aiOverlap=0,jointOverlap=0;
    ProductionRenderer renderer;std::atomic_bool stopServer=false,serverReturned=false,workerRetained=false;std::atomic_int serverStatus=-1;auto unexpectedJobsOwner=std::make_shared<std::atomic_int>(0);auto& unexpectedJobs=*unexpectedJobsOwner;
    const auto suffix="prod-"+juce::Uuid().toString().removeCharacters("-").substring(0,27);
    auto server=std::make_unique<companion::RuntimeServer>(
        [owned=backendOwner,enabled=nativeAi,unexpected=unexpectedJobsOwner](const companion::RuntimeJob& job){
            if(enabled)return owned->run(job);++*unexpected;return companion::Frame{companion::MessageKind::failure,job.session,{}};
        },[](const auto& job){if(job.previewExchange)job.previewExchange->cancel();});
    std::thread io;
    bool admitted=false,publication=false,peersUnchanged=false,selectionPreserved=false,rosterProgress=false,priority=false;
    std::uint64_t editBefore=0,editAfter=0;Ns measuredWall=0,graceWall=0,manualCost=0,measurementEnd=0;J preparedModes(juce::Array<J>{});juce::String failure;
    auto pump=[&](const char* phase,int block){const auto t=Clock::now();for(auto& i:instances)if(i)i->processor->pollAi();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);const auto cost=elapsed(t);
        auto record=object();put(record,"phase",phase);put(record,"block_index",block);put(record,"operation_ns",integer(cost));
        if(instances[0]){const auto v=instances[0]->processor->groupWorkflowView();put(record,"admitted",v.link.phase==GroupLinkPhase::admitted);put(record,"operation_pending",v.operationPending);put(record,"generation_ready",v.generationReady);put(record,"roster_count",int(v.members.size()));put(record,"selected_peer_count",int(v.selection.peerCount));}controls.append(record);};
    auto wait=[&](auto predicate,const char* label){const auto end=Clock::now()+std::chrono::seconds(5);do{pump(label,-1);require(!serverReturned,"synthetic server exited during control work");if(predicate())return;}while(Clock::now()<end);throw std::runtime_error(label);};
    auto take=[&](std::size_t n,std::uint64_t& edit){auto d=std::make_unique<ProjectDocument>();require(instances[n]->processor->tryProjectSnapshot(*d,edit),"linked project snapshot unavailable");return d;};
    auto rosterMatches=[&](const GroupWorkflowView& view,const std::array<std::uint64_t,4>& edits){if(view.members.size()!=4||view.conflicted||view.busy||view.operationPending)return false;
        for(std::size_t n=0;n<4;++n){const auto local=instances[n]->processor->groupLinkView();const auto found=std::find_if(view.members.begin(),view.members.end(),[&](const auto& member){return local.intent&&member.intent.member==local.intent->member;});
            if(found==view.members.end()||found->runtime!=GroupUuid(instances[n]->processor->aiRuntime())||found->editRevision!=edits[n]||found->busy||found->conflicted||found->noteCount!=16||found->noteAvailability!=ObservedMidiAvailability::fresh||found->controlAvailability!=ObservedMidiAvailability::fresh)return false;}return true;};
    try{
        io=std::thread([&]{const auto r=server->run(stopServer,suffix);serverStatus=int(r.status);workerRetained=r.workerRetained;serverReturned=true;});
        const auto fixtureRoot=nativeAi?juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/task13-step7-isolated-technical-fixtures").getChildFile(suffix):juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-06-m6-production-profile/task-1");
        if(nativeAi)require(fixtureRoot.createDirectory().wasOk(),"Step7 isolated technical context creation");
        const companion::ClientOptions options{{juce::File{},suffix,fixtureRoot},false};const GroupUuid group=juce::Uuid();
        auto full=std::make_unique<ProjectDocument>();const auto sound=testing::fixtureValue([&]{return supplied?*supplied:personalities?profilePersonalityState():workload(g.workload);});full->sound=*sound;full->sound.values[758]=quality==RenderQuality::higher?1.f:0.f;full->pattern.noteCount=16;for(std::size_t n=0;n<16;++n)full->pattern.notes[n]={0,4,std::uint8_t(48+n),100,1};
        juce::MemoryBlock bytes;require(encodeProjectState(*full,bytes),"linked baseline encoding failed");put(result,"document",documentJson(full->sound));
        for(std::size_t n=0;n<4;++n){instances[n]=std::make_unique<ProductionInstance>(options,g.block,blocks);auto& p=*instances[n]->processor;
            require(p.restoreProjectState(bytes.getData(),int(bytes.getSize())),"linked full source restore failed");p.setNonRealtime(quality==RenderQuality::higher);p.prepareToPlay(g.rate,g.block);require(p.effectiveRenderQuality()==quality&&!p.renderQualityPending(),"Linked mode not prepared");
            auto mode=object();put(mode,"selected",int(p.selectedRenderQuality()));put(mode,"effective",int(p.effectiveRenderQuality()));put(mode,"host_non_realtime",p.isNonRealtime());put(mode,"latency_samples",p.getLatencySamples());preparedModes.append(mode);
            GroupIntent intent;intent.group=group;intent.member=juce::Uuid();require(setGroupText("Synthetic production profile",intent.name)&&setGroupText(n==0?"lead":"peer",intent.role)&&setGroupText("A",intent.key)&&setGroupText("minor",intent.scale),"linked intent text invalid");intent.bpm=120;
            require(p.setGroupIntent(intent)&&p.requestGroupConnection(),"explicit linked connection rejected");notes(instances[n]->midi,16,0,true);}
        wait([&]{for(const auto& i:instances){const auto v=i->processor->groupWorkflowView();if(v.link.phase!=GroupLinkPhase::admitted||v.connection.phase!=companion::ClientPhase::ready||v.operationPending)return false;}return true;},"public welcome/admission timeout");admitted=true;
        for(Ns frame=0;frame<warmup;frame+=g.block){for(auto& i:instances){i->head.advance(frame,g.rate);i->audio.clear();step7Process(*i->processor,i->audio,i->midi);i->midi.clear();}if((frame/g.block)%8==0)pump("warmup",int(frame/g.block));}
        for(std::size_t n=0;n<4;++n){initial[n]=take(n,initialEdits[n]);require(initial[n]->sound==full->sound&&initial[n]->pattern==full->pattern&&initial[n]->group&&initial[n]->group->group==group,"linked baseline lost full source");before.append(productionPublication(*initial[n],initialEdits[n]));if(n>0)selection[n-1]=initial[n]->group->member;}
        wait([&]{return !instances[0]->processor->groupWorkflowView().operationPending;},"initial publisher wait");require(instances[0]->processor->browseGroups(),"initial browse rejected");
        wait([&]{return rosterMatches(instances[0]->processor->groupWorkflowView(),initialEdits);},"full coherent sixteen-held-note roster timeout");
        require(instances[0]->processor->selectGroupPeers(selection),"explicit ordered selection rejected");wait([&]{return instances[0]->processor->groupWorkflowView().generationReady;},"selected peer readiness timeout");
        capturedSelection=instances[0]->processor->groupWorkflowView().selection;editBefore=initialEdits[0];

        if(nativeAi){
            // Publish/settle the same midpoint edit before capture. Changing it
            // during the request would stale the captured authority by design.
            auto edited=std::make_unique<ProjectDocument>(*initial[0]);set(edited->sound,"synth.1.wavetable.filter.cutoff",2000);require(instances[0]->processor->applySoundEdit(edited->sound,SoundEditOrigin::manual,editBefore),"Step7 pre-capture manual edit");
            wait([&]{std::uint64_t revision;auto current=take(0,revision);return revision==editBefore+1&&current->sound==edited->sound;},"Step7 pre-capture publication");
            editAfter=editBefore+1;publication=true;
            for(std::size_t peer=1;peer<4;++peer){require(instances[peer]->processor->browseGroups(),"Step7 peer refresh");wait([&]{return !instances[peer]->processor->groupWorkflowView().operationPending;},"Step7 peer publication settle");}
            rosterProgress=true;
            require(instances[0]->processor->requestAiAccount({}),"Step7 technical Account submission");wait([&]{const auto* a=instances[0]->processor->aiAccountReply();return a&&a->models.size()==1&&!instances[0]->processor->aiWorkflowStatus().accountPending&&instances[0]->processor->groupWorkflowView().generationReady;},"Step7 technical Account and captured peers");
            AiRequestOptions request{"Task13 full capacity synthetic technical candidate","task13-synthetic-technical",{ai::RequestScope::sound,false,false,false}};
            aiSubmitted=instances[0]->processor->generateAi(request);require(aiSubmitted,"Step7 public generateAi submission");const auto* capture=instances[0]->processor->aiCurrentRequest();require(capture&&capture->catalog&&ai::detail::catalogRequestValid(*capture),"Step7 public captured catalog");
            requestAuthority=validateProjectDocument(capture->source)&&capture->catalog->authority->root==capture->catalog->root&&capture->catalog->root==capture->tag&&capture->model=="task13-synthetic-technical"&&capture->capturedBpm&&*capture->capturedBpm==120&&int(documentJson(capture->source.sound)["active_nonzero_routes"])==64;
            require(requestAuthority,"Step7 current captured request authority");wait([&]{return backend.jobs.load()>0;},"Step7 native worker entry");
        }

        renderer.thread=std::thread([&,full=std::move(full)]{
            renderer.prioritySet=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL)!=0;renderer.priorityAttempted=true;
            if(renderer.prioritySet)try{ai::AuditionContext context;context.phrase=full->pattern;
                for(std::size_t n=0;n<3;++n){auto& job=renderer.jobs[n];{std::lock_guard lock(renderer.starts);if(renderer.ended||renderer.cancel)break;job.start=elapsed(origin);renderer.active=1;++renderer.started;}
                    const auto rendered=ai::renderPreview(*full,context,[&]{++renderer.cancellationChecks;return renderer.cancel.load();},512,quality);job.end=elapsed(origin);job.status=rendered.status;job.body=rendered.audio.bodyFrames;job.tail=rendered.audio.left.size()>=job.body?rendered.audio.left.size()-job.body:0;job.finite=rendered.measurements.finite;
                    if(job.status==ai::RenderStatus::ready)++renderer.ready;++renderer.completed;renderer.active=0;
                }
            }catch(...){renderer.active=0;}
            {std::lock_guard lock(renderer.completion);renderer.done=true;}renderer.finished.notify_all();
        });
        wait([&]{return renderer.priorityAttempted.load();},"renderer priority setup timeout");priority=renderer.prioritySet;require(priority,"below-normal renderer priority unavailable");
        wait([&]{return renderer.started.load()>0||renderer.done.load();},"renderer job startup timeout");require(renderer.started>0,"renderer never became active");
        const auto loopStart=Clock::now();
        for(std::size_t b=0;b<blocks;++b){
            if(!nativeAi&&b==blocks/2){auto changed=take(0,editAfter);set(changed->sound,"synth.1.wavetable.filter.cutoff",2000);const auto controlStart=Clock::now();manualEdit(*instances[0]->processor,changed->sound,editAfter);manualCost=elapsed(controlStart);
                auto updated=take(0,editAfter);require(editAfter==editBefore+1&&updated->sound==changed->sound&&updated->pattern==initial[0]->pattern&&updated->group==initial[0]->group,"requester manual edit not coherent");
                auto expectedEdits=initialEdits;expectedEdits[0]=editAfter;wait([&]{return !instances[1]->processor->groupWorkflowView().operationPending;},"midpoint publisher wait");require(instances[1]->processor->browseGroups(),"midpoint browse rejected");
                wait([&]{return rosterMatches(instances[1]->processor->groupWorkflowView(),expectedEdits);},"manual publication revision timeout");publication=true;rosterProgress=true;
            }
            const Ns frame=warmup+Ns(b)*g.block;for(auto& i:instances){i->head.advance(frame,g.rate);i->midi.clear();i->audio.clear();}
            const auto serialStart=Clock::now();
            for(auto& i:instances){auto& o=i->observations[b];o.activeBefore=renderer.active;o.startedBefore=renderer.started;o.completedBefore=renderer.completed;
                const auto start=Clock::now();step7Process(*i->processor,i->audio,i->midi);const auto end=Clock::now();o.ns=std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();o.start=std::chrono::duration_cast<std::chrono::nanoseconds>(start-origin).count();o.end=std::chrono::duration_cast<std::chrono::nanoseconds>(end-origin).count();
                o.activeAfter=renderer.active;o.startedAfter=renderer.started;o.completedAfter=renderer.completed;}
            serial[b]=elapsed(serialStart);completedBlocks=b+1;for(auto& i:instances)scan(i->observations[b].audio,i->audio);
            if(b%8==0)pump("measured-periodic",int(b));
        }
        measuredWall=elapsed(loopStart);renderer.stopStarting();measurementEnd=elapsed(origin);
        if(renderer.ready>0)renderer.cancel=true;else{const auto graceStart=Clock::now();std::unique_lock lock(renderer.completion);renderer.finished.wait_until(lock,graceStart+std::chrono::seconds(30),[&]{return renderer.done.load();});graceWall=elapsed(graceStart);renderer.cancel=true;}
        renderer.thread.join();

        if(nativeAi){const auto grace=Clock::now()+std::chrono::milliseconds(companion::runtimeWorkerDrainMs);while(Clock::now()<grace&&instances[0]->processor->aiProposalPhase()==AiProposalPhase::pending){pump("native-grace",-1);std::this_thread::yield();}
            const auto* proposal=instances[0]->processor->aiProposal();aiDelivered=proposal&&instances[0]->processor->aiProposalPhase()==AiProposalPhase::proposed&&proposal->status==ai::GenerationStatus::ready&&proposal->candidate&&validateProjectDocument(*proposal->candidate)&&backend.ready.load()>0;require(aiDelivered,"Step7 native result not publicly readmitted within drain bound");}
        peersUnchanged=true;for(std::size_t n=0;n<4;++n){std::uint64_t edit;auto final=take(n,edit);after.append(productionPublication(*final,edit));if(n>0)peersUnchanged=peersUnchanged&&*final==*initial[n]&&edit==initialEdits[n];}
        const auto finalSelection=instances[0]->processor->groupWorkflowView().selection;selectionPreserved=finalSelection==capturedSelection&&finalSelection.peerCount==3;
    }catch(const std::exception& e){failure=e.what();}
    renderer.stopStarting();if(measurementEnd==0)measurementEnd=elapsed(origin);renderer.cancel=true;if(renderer.thread.joinable())renderer.thread.join();
    pump("shutdown",-1);for(auto& i:instances)if(i){i->processor->releaseResources();i->processor->closeAiSession();i->processor->setPlayHead(nullptr);}stopServer=true;if(io.joinable())io.join();
    J callbacks(juce::Array<J>{}),audio(juce::Array<J>{}),summaries(juce::Array<J>{}),activity(juce::Array<J>{}),rawSerial(juce::Array<J>{}),jobs(juce::Array<J>{}),overlap(juce::Array<J>{});
    bool finite=true;double peak=0;int overlapBlocks=0;std::vector<Ns> serialSamples;serialSamples.reserve(completedBlocks);
    for(std::size_t b=0;b<completedBlocks;++b){bool overlaps=false;for(const auto& i:instances)for(int n=0;n<renderer.started;++n){const auto& job=renderer.jobs[std::size_t(n)];const auto& o=i->observations[b];overlaps=overlaps||(job.start<o.end&&job.end>o.start);}overlap.append(overlaps);overlapBlocks+=overlaps;rawSerial.append(integer(serial[b]));serialSamples.push_back(serial[b]);}
    for(const auto& i:instances){J raw(juce::Array<J>{}),observations(juce::Array<J>{}),states(juce::Array<J>{});std::vector<Ns> ns;ns.reserve(completedBlocks);
        if(i)for(std::size_t b=0;b<completedBlocks;++b){const auto& o=i->observations[b];raw.append(integer(o.ns));ns.push_back(o.ns);observations.append(audioJson(o.audio));finite=finite&&o.audio.finite;peak=std::max(peak,o.audio.peak);
            auto counters=object();put(counters,"start_ns",integer(o.start));put(counters,"end_ns",integer(o.end));put(counters,"active_before",o.activeBefore);put(counters,"active_after",o.activeAfter);put(counters,"started_before",o.startedBefore);put(counters,"started_after",o.startedAfter);put(counters,"completed_before",o.completedBefore);put(counters,"completed_after",o.completedAfter);states.append(counters);}
        callbacks.append(raw);audio.append(observations);activity.append(states);summaries.append(ns.empty()?J():summary(ns,deadlineNs(g.rate,g.block)));}
    bool allRenderJobsValid=renderer.started==renderer.completed,noJobsAfterMeasurement=true;
    for(int n=0;n<renderer.started;++n){const auto& job=renderer.jobs[std::size_t(n)];allRenderJobsValid=allRenderJobsValid&&job.end>=job.start&&(job.status==ai::RenderStatus::ready||job.status==ai::RenderStatus::cancelled)&&job.finite;noJobsAfterMeasurement=noJobsAfterMeasurement&&job.start<=measurementEnd;
        auto record=object();put(record,"start_ns",integer(job.start));put(record,"end_ns",integer(job.end));put(record,"status",int(job.status));put(record,"body_frames",integer(Ns(job.body)));put(record,"tail_frames",integer(Ns(job.tail)));put(record,"finite",job.finite);jobs.append(record);}

    if(nativeAi){const auto begun=backend.generationStart.load(),ended=backend.generationEnd.load();for(std::size_t b=0;b<completedBlocks;++b){bool native=false,joint=false;for(const auto& instance:instances)if(instance){const auto& o=instance->observations[b];native=native||(begun>0&&ended>begun&&begun<o.end&&ended>o.start);for(const auto& job:renderer.jobs)joint=joint||(begun>0&&ended>begun&&job.end>job.start&&std::max({begun,job.start,o.start})<std::min({ended,job.end,o.end}));}aiOverlap+=native;jointOverlap+=joint;}}
    put(result,"requested_quality",int(quality));put(result,"prepared_instance_modes",preparedModes);put(result,"workload",g.workload);put(result,"kind",g.kind);put(result,"sample_rate",g.rate);put(result,"block_size",g.block);put(result,"instance_count",4);put(result,"trial",g.trial);put(result,"acceptance_class",productionClassification(g));
    put(result,"selftest_reduced",reducedBlocks!=0);put(result,"warmup_frames",integer(warmup));put(result,"measured_frames",integer(frames));put(result,"completed_blocks",int(completedBlocks));put(result,"callback_ns",callbacks);put(result,"callback_summaries",summaries);put(result,"audio_observations",audio);put(result,"render_activity_around_callbacks",activity);put(result,"serial_ns",rawSerial);if(!serialSamples.empty())put(result,"serial_summary",summary(serialSamples,deadlineNs(g.rate,g.block)));
    put(result,"raw_finite",finite);put(result,"raw_peak",peak);put(result,"control_observations",controls);put(result,"manual_edit_operation_ns",integer(manualCost));put(result,"measured_loop_wall_ns",integer(measuredWall));put(result,"untimed_render_completion_grace_ns",integer(graceWall));
    put(result,"publications_before",before);put(result,"publications_after",after);put(result,"all_admitted",admitted);put(result,"selected_peer_count",int(capturedSelection.peerCount));put(result,"selection_preserved",selectionPreserved);put(result,"manual_publication_observed",publication);put(result,"peer_musical_state_unchanged",peersUnchanged);put(result,"requester_edit_before",integer(Ns(editBefore)));put(result,"requester_edit_after",integer(Ns(editAfter)));put(result,"roster_progress_observed",rosterProgress);
    put(result,"renderer_priority_set",priority);put(result,"render_jobs",jobs);put(result,"render_jobs_started",renderer.started.load());put(result,"render_jobs_completed",renderer.completed.load());put(result,"render_ready_count",renderer.ready.load());put(result,"render_overlap_blocks",overlapBlocks);put(result,"render_overlap_flags",overlap);put(result,"measurement_end_ns",integer(measurementEnd));put(result,"sticky_cancellation_requested",renderer.cancel.load());put(result,"cancellation_check_count",renderer.cancellationChecks.load());put(result,"no_jobs_started_after_measurement",noJobsAfterMeasurement);put(result,"renderer_joined",!renderer.thread.joinable());
    J selected(juce::Array<J>{});for(const auto member:selection)selected.append(juce::Uuid(member.getRawData()).toString());put(result,"selected_members",selected);
    const bool shutdown=serverReturned&&serverStatus==int(companion::RuntimeStatus::ready)&&!workerRetained;put(result,"shutdown_complete",shutdown);put(result,"server_return_status",serverStatus.load());put(result,"server_worker_retained",workerRetained.load());put(result,"unexpected_backend_jobs",unexpectedJobs.load());if(nativeAi){put(result,"public_generate_ai",aiSubmitted);put(result,"captured_request_authority",requestAuthority);put(result,"public_result_readmitted",aiDelivered);put(result,"native_ai_overlap_blocks",integer(aiOverlap));put(result,"joint_native_ai_background_overlap_blocks",integer(jointOverlap));put(result,"native_ai",backend.report());put(result,"manual_publication_scope","Same coherent edit performed and peers settled before capture; original default linkedTrial retains midpoint publication");}put(result,"synthetic_pipe_suffix",suffix);put(result,"failure",failure);
    put(result,"timing_scope","callback start/end span contains processBlock and clock overhead only; serial interval adds atomic worker observations/stores/nested clocks; controls, scans, manual edit/waits and grace are untimed callback work");
    put(result,"integrity",failure.isEmpty()&&completedBlocks==blocks&&admitted&&publication&&peersUnchanged&&selectionPreserved&&rosterProgress&&priority&&renderer.ready>0&&allRenderJobsValid&&noJobsAfterMeasurement&&overlapBlocks>=8&&finite&&peak>0&&shutdown&&unexpectedJobs==0&&(!nativeAi||(aiSubmitted&&requestAuthority&&aiDelivered&&backend.ready.load()>0&&aiOverlap>=8&&jointOverlap>=8)));
    const auto cpuAfter=cpuTime();if(cpuBefore.available&&cpuAfter.available){auto cpu=object();put(cpu,"user_ns",integer(Ns((cpuAfter.user-cpuBefore.user)*100)));put(cpu,"kernel_ns",integer(Ns((cpuAfter.kernel-cpuBefore.kernel)*100)));put(cpu,"scope","tool process/all threads, includes setup/warmup/control/scans/renderer/untimed grace and trial serialization so far");put(result,"process_cpu",cpu);}put(result,"trial_wall_ns_before_return",integer(elapsed(origin)));return result;
}
#else
J linkedTrial(const Geometry&,std::size_t){throw std::runtime_error("linked production profiling requires Windows development probes");}
#endif
J linkedProductionTrial(const Geometry& g) { return linkedTrial(g,0); }
void validateLinkedTrial(const J& report)
{
    require(report.isObject(),"linked report must be an object");
    auto number=[](const J& value){require((value.isInt64()||value.isInt())&&juce::int64(value)>=0,"linked integer extent/timing invalid");return Ns(juce::int64(value));};
    for(const auto* key:{"integrity","all_admitted","selection_preserved","manual_publication_observed","peer_musical_state_unchanged","roster_progress_observed","renderer_priority_set","sticky_cancellation_requested","no_jobs_started_after_measurement","renderer_joined","shutdown_complete","raw_finite"})require(report[key].isBool()&&bool(report[key]),"linked workload integrity flag absent/false");
    const int rate=int(number(report["sample_rate"])),block=int(number(report["block_size"]));const bool reduced=bool(report["selftest_reduced"]);
    require(report["selftest_reduced"].isBool()&&report["kind"].toString()=="linked-contention"&&report["workload"].toString()=="full-wavetable-mixed"&&((rate==48000&&block==128)||(rate==96000&&block==512))&&number(report["instance_count"])==4,"linked geometry invalid");
    const Ns frames=reduced?Ns(32)*block:framesFor(2,rate,block);const int blocks=int(frames/block);
    require(number(report["measured_frames"])==frames&&number(report["warmup_frames"])==framesFor(.25,rate,block)&&number(report["completed_blocks"])==blocks,"linked frame extent invalid");
    require(report["callback_ns"].size()==4&&report["audio_observations"].size()==4&&report["render_activity_around_callbacks"].size()==4&&report["callback_summaries"].size()==4&&report["serial_ns"].size()==blocks&&report["render_overlap_flags"].size()==blocks,"linked raw array extent invalid");
    require(std::isfinite(double(report["raw_peak"]))&&double(report["raw_peak"])>0&&report["control_observations"].size()>=4,"linked finite signal/control evidence missing");
    for(int i=0;i<4;++i){require(report["callback_ns"][i].size()==blocks&&report["audio_observations"][i].size()==blocks&&report["render_activity_around_callbacks"][i].size()==blocks,"missing linked callback observations");std::vector<Ns> raw;
        for(int b=0;b<blocks;++b){const auto ns=number(report["callback_ns"][i][b]);raw.push_back(ns);const auto audio=report["audio_observations"][i][b];const auto timing=report["render_activity_around_callbacks"][i][b];
            require(audio["finite"].isBool()&&bool(audio["finite"])&&std::isfinite(double(audio["peak"])),"linked raw audio nonfinite");require(number(timing["end_ns"])-number(timing["start_ns"])==ns,"callback span differs from raw nanoseconds");
            for(const auto* key:{"active_before","active_after"})require(number(timing[key])<=1,"worker active counter invalid");for(const auto* key:{"started_before","started_after","completed_before","completed_after"})require(number(timing[key])<=3,"worker job counter invalid");
        }
        const auto expected=summary(raw,deadlineNs(rate,block));for(const auto* key:{"min_ns","mean_ns","median_ns","p95_ns","p99_ns","max_ns","deadline_ns","deadline_miss_count"})require(double(report["callback_summaries"][i][key])==double(expected[key]),"linked summary does not retain raw distribution");
    }
    std::vector<Ns> serial;for(int b=0;b<blocks;++b)serial.push_back(number(report["serial_ns"][b]));const auto serialSummary=summary(serial,deadlineNs(rate,block));for(const auto* key:{"mean_ns","p99_ns","max_ns","deadline_miss_count"})require(double(report["serial_summary"][key])==double(serialSummary[key]),"serial summary differs from raw stress samples");
    const int started=int(number(report["render_jobs_started"]));require(started>=1&&started<=3&&number(report["render_jobs_completed"])==started&&report["render_jobs"].size()==started,"render job extent invalid");
    int ready=0;for(int n=0;n<started;++n){const auto job=report["render_jobs"][n];const auto start=number(job["start_ns"]),end=number(job["end_ns"]);const auto status=number(job["status"]);
        require(start<=number(report["measurement_end_ns"])&&end>=start&&(n==0||start>=number(report["render_jobs"][n-1]["end_ns"])),"render jobs not sequential/bounded to measured startup");
        require(status==0||status==2,"render job failed");require(job["finite"].isBool()&&bool(job["finite"]),"render job nonfinite");
        if(status==0){++ready;require(number(job["body_frames"])==96000&&number(job["tail_frames"])==192000,"renderer changed explicit16-note2s body/fixed4s tail");}
    }
    require(ready>=1&&number(report["render_ready_count"])==ready,"no completed ready render");
    int overlapping=0;for(int b=0;b<blocks;++b){bool overlap=false;for(int i=0;i<4;++i)for(int n=0;n<started;++n){const auto span=report["render_activity_around_callbacks"][i][b];const auto job=report["render_jobs"][n];overlap=overlap||(number(job["start_ns"])<number(span["end_ns"])&&number(job["end_ns"])>number(span["start_ns"]));}
        require(report["render_overlap_flags"][b].isBool()&&bool(report["render_overlap_flags"][b])==overlap,"overlap flag not bound to actual callback/job spans");overlapping+=overlap;}
    require(overlapping>=8&&number(report["render_overlap_blocks"])==overlapping,"inactive background workload cannot pass");
    require(number(report["unexpected_backend_jobs"])==0&&number(report["server_return_status"])==0&&!bool(report["server_worker_retained"])&&number(report["selected_peer_count"])==3&&report["selected_members"].size()==3,"native server/selection integrity failed");
    require(report["publications_before"].size()==4&&report["publications_after"].size()==4,"missing coherent full publications");
    std::array<std::unique_ptr<ProjectDocument>,4> before,after;
    for(int n=0;n<4;++n)for(int phase=0;phase<2;++phase){const auto pub=report[phase?"publications_after":"publications_before"][n];juce::MemoryBlock bytes;juce::MemoryOutputStream out(bytes,false);require(juce::Base64::convertFromBase64(out,pub["native_aipj_base64"].toString()),"publication native bytes missing");out.flush();auto d=std::make_unique<ProjectDocument>();
        require(bytes.getSize()==std::size_t(number(pub["bytes"]))&&juce::SHA256(bytes).toHexString()==pub["sha256"].toString()&&decodeProjectState(bytes.getData(),bytes.getSize(),*d)&&d->group,"publication codec/hash missing");
        const auto capacity=documentJson(d->sound);require(int(capacity["synth_count"])==4&&capacity["effects"].size()==8&&int(capacity["active_nonzero_routes"])==64&&d->pattern.noteCount==16&&d->group->bpm==120,"publication lost full-capacity source");if(phase)after[std::size_t(n)]=std::move(d);else before[std::size_t(n)]=std::move(d);
    }
    const auto editBefore=number(report["requester_edit_before"]),editAfter=number(report["requester_edit_after"]);require(editAfter==editBefore+1&&number(report["publications_before"][0]["edit_revision"])==editBefore&&number(report["publications_after"][0]["edit_revision"])==editAfter,"requester publication revision inconsistent");
    auto changed=std::make_unique<ProjectDocument>(*before[0]);set(changed->sound,"synth.1.wavetable.filter.cutoff",2000);require(*after[0]==*changed,"requester manual publication is not the single cutoff edit");
    for(int n=1;n<4;++n){require(*before[std::size_t(n)]==*after[std::size_t(n)]&&number(report["publications_before"][n]["edit_revision"])==number(report["publications_after"][n]["edit_revision"]),"peer musical authority changed");
        require(juce::Uuid(report["selected_members"][n-1].toString())==juce::Uuid(before[std::size_t(n)]->group->member.getRawData())&&before[std::size_t(n)]->group->group==before[0]->group->group,"selected original peer order/group incorrect");}
    for(const auto& control:*report["control_observations"].getArray())number(control["operation_ns"]);
}
bool acceptLinkedTrial(J& trial)
{try{validateLinkedTrial(trial);return true;}catch(const std::exception& e){put(trial,"integrity",false);put(trial,"validation_error",e.what());return false;}}
int profileProduction(const juce::File& output,const juce::File& suppliedInventory)
{
#if !defined(RACKSYNTH_PRODUCTION_PROFILE)
    throw std::runtime_error("production profiling requires Windows development probes");
#else
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Production timings require Release");const auto inventory=inventoryMetadata(suppliedInventory);
    require(std::filesystem::create_directory(std::filesystem::path(output.getFullPathName().toWideCharPointer())),"fresh production output creation failed");
    const auto start=Clock::now();const auto cpuBefore=cpuTime();const auto before=identities();auto report=object();put(report,"schema_version",1);put(report,"suite","production");put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"expected_trial_count",41);put(report,"inventory",inventory);put(report,"identities_before",before);
    put(report,"scope","fixed41 native trials; four-instance serial/linked sums are stress observations, not observed DAW multicore scheduling or underruns; synthetic runtime has no provider/account/model actions");put(report,"budget_mean_callback_load",.75);put(report,"budget_p99_deadline_load",1.0);put(report,"performance_acceptance",false);put(report,"actual_host_underruns",J());
    put(report,"timing_limits","processBlock-only callback intervals plus clocks; audio scans/JSON/setup/control waits/grace excluded; serial sums include instrumentation; process CPU/wall include all threads and untimed work; every raw sample/outlier retained");
    J trials(juce::Array<J>{});bool integrity=true,qualifiedTargets=true;int number=0;
    for(const auto& geometry:productionMatrix()){
        auto trial=geometry.kind=="linked-contention"?linkedProductionTrial(geometry):runTrial(geometry);const bool accepted=geometry.kind=="linked-contention"?acceptLinkedTrial(trial):acceptTrial(trial);
        put(trial,"acceptance_class",productionClassification(geometry));bool target=true;for(const auto& summary:*trial["callback_summaries"].getArray())target=target&&double(summary["mean_callback_load"])<=.75&&double(summary["p99_ns"])<=double(summary["deadline_ns"]);put(trial,"callback_targets_met",accepted&&target);
        if(productionClassification(geometry)=="historical-qualified")qualifiedTargets=qualifiedTargets&&target;
        const auto name="trial-"+juce::String(++number).paddedLeft('0',3)+".json";writeJson(output.getChildFile(name),trial);put(trial,"trial_file",name);trials.append(trial);std::cout<<number<<"/41 "<<geometry.kind<<' '<<geometry.rate<<'/'<<geometry.block<<" integrity="<<accepted<<" callback_targets="<<target<<std::endl;
        if(!accepted){integrity=false;break;}
    }
    const auto after=identities();const bool stable=juce::JSON::toString(before)==juce::JSON::toString(after);put(report,"identities_after",after);put(report,"identities_unchanged",stable);put(report,"trials",trials);put(report,"completed_trial_count",number);put(report,"measurement_integrity",integrity&&stable&&number==41?"passed":"failed");put(report,"historical_qualified_targets_met",qualifiedTargets);
    const auto cpuAfter=cpuTime();if(cpuBefore.available&&cpuAfter.available){auto cpu=object();put(cpu,"user_ns",integer(Ns((cpuAfter.user-cpuBefore.user)*100)));put(cpu,"kernel_ns",integer(Ns((cpuAfter.kernel-cpuBefore.kernel)*100)));put(cpu,"scope","whole tool/all threads including setup/control/scans/background/grace/trial JSON/identities; excludes final serialization");put(report,"tool_process_cpu",cpu);}put(report,"wall_ns_before_final_serialization",integer(elapsed(start)));writeJson(output.getChildFile("production-profile-report.json"),report);
    return integrity&&stable&&number==41?0:1;
#endif
}
// BEGIN PRODUCTION PROFILE SELF TEST -- complete immutable Task2 oracle.
int productionProfileSelfTest()
{
    int checks=0,failures=0,sections=0;
#if JUCE_WINDOWS
    FILETIME created{},exited{},kernel{},user{};const bool processTimes=GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=0;
    std::cout<<"ProductionProfile NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<((std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime)<<" timeAvailable="<<processTimes<<std::endl;
#endif
    auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL production: "<<label<<'\n';}};
    auto rejects=[&](auto action,const char* label){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,label);};
    const auto fixture=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-06-m6-production-profile/task-1/task-2-fixtures").getChildFile(juce::Uuid().toString());
    require(fixture.createDirectory().wasOk(),"production selftest fixture creation failed");
    const auto fresh=fixture.getChildFile("never-profiled").getFullPathName();
    ++sections;
    try{check(parseArguments({"--profile","--suite","production","--output",fresh}).suite=="production","new production CLI admitted before any work");}
    catch(const std::exception&){check(false,"new production CLI admitted before any work");}
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","unknown","--output",fresh},
        {"--profile","--suite","production","--output","relative-output"},
        {"--profile","--suite","production","--output",fixture.getFullPathName()},
        {"--profile","--suite","production","--output","C:/outside-production-profile"},
        {"--profile","--suite","production","--output",fresh,"--suite","production"}})
        rejects([&]{parseArguments(args);},"unknown/relative/existing/outside/duplicate production CLI rejected");
    check(!juce::File(fresh).exists(),"parser creates no output or measured workload");

    ++sections;
    const std::array<Geometry,41> expected{{
        {"full-wavetable-mixed","steady",44100,64,1,1},{"full-wavetable-mixed","steady",44100,128,1,1},{"full-wavetable-mixed","steady",44100,256,1,1},{"full-wavetable-mixed","steady",44100,512,1,1},{"full-wavetable-mixed","steady",44100,1024,1,1},
        {"full-wavetable-mixed","steady",48000,64,1,1},{"full-wavetable-mixed","steady",48000,128,1,1},{"full-wavetable-mixed","steady",48000,256,1,1},{"full-wavetable-mixed","steady",48000,512,1,1},{"full-wavetable-mixed","steady",48000,1024,1,1},
        {"full-wavetable-mixed","steady",96000,64,1,1},{"full-wavetable-mixed","steady",96000,128,1,1},{"full-wavetable-mixed","steady",96000,256,1,1},{"full-wavetable-mixed","steady",96000,512,1,1},{"full-wavetable-mixed","steady",96000,1024,1,1},
        {"full-reverb","steady",44100,64,1,1},{"full-reverb","steady",44100,128,1,1},{"full-reverb","steady",44100,256,1,1},{"full-reverb","steady",44100,512,1,1},{"full-reverb","steady",44100,1024,1,1},
        {"full-reverb","steady",48000,64,1,1},{"full-reverb","steady",48000,128,1,1},{"full-reverb","steady",48000,256,1,1},{"full-reverb","steady",48000,512,1,1},{"full-reverb","steady",48000,1024,1,1},
        {"full-reverb","steady",96000,64,1,1},{"full-reverb","steady",96000,128,1,1},{"full-reverb","steady",96000,256,1,1},{"full-reverb","steady",96000,512,1,1},{"full-reverb","steady",96000,1024,1,1},
        {"full-wavetable-mixed","lifecycle",48000,64,1,1},{"full-wavetable-mixed","lifecycle",48000,512,1,1},{"full-wavetable-mixed","lifecycle",96000,512,1,1},
        {"full-wavetable-mixed","multi",48000,128,4,1},{"full-wavetable-mixed","multi",96000,512,4,1},
        {"full-wavetable-mixed","linked-contention",48000,128,4,1},{"full-wavetable-mixed","linked-contention",48000,128,4,2},{"full-wavetable-mixed","linked-contention",48000,128,4,3},
        {"full-wavetable-mixed","linked-contention",96000,512,4,1},{"full-wavetable-mixed","linked-contention",96000,512,4,2},{"full-wavetable-mixed","linked-contention",96000,512,4,3}
    }};
    const std::array<Ns,15> warm{{11072,11136,11264,11264,11264,12032,12032,12032,12288,12288,24000,24064,24064,24064,24576}};
    const std::array<Ns,15> measured{{44160,44160,44288,44544,45056,48000,48000,48128,48128,48128,96000,96000,96000,96256,96256}};
    const auto matrix=productionMatrix();check(matrix.size()==41,"exact literal41 production matrix");
    if(matrix.size()==expected.size())for(std::size_t n=0;n<expected.size();++n){const auto& a=matrix[n];const auto& e=expected[n];
        check(a.workload==e.workload&&a.kind==e.kind&&a.rate==e.rate&&a.block==e.block&&a.count==e.count&&a.trial==e.trial,"literal matrix order/workload/geometry/count/repetition");
        const bool qualified=n<30?(n%15!=10&&n%15!=11):n<33;
        check(productionClassification(a)==(qualified?"historical-qualified":"stress"),"qualified26 steady geometries plus lifecycle versus declared stress");
        if(n<30)check(framesFor(.25,a.rate,a.block)==warm[n%15]&&framesFor(1,a.rate,a.block)==measured[n%15],"literal steady rounded warmup/measured frames");
        if(n>=30&&n<33)check(framesFor(2,a.rate,a.block)==(n==30?96000:n==31?96256:192000),"literal ten-event lifecycle two-second frames");
        if(n>=35)check(framesFor(2,a.rate,a.block)==(n<38?96000:192000)&&framesFor(.25,a.rate,a.block)==(n<38?12032:24064),"literal linked2s/.25s frames and repetitions");
    }
    const auto full=std::make_unique<ProjectDocument>();const auto sound=testing::fixtureValue([]{return workload("full-wavetable-mixed");});full->sound=*sound;
    full->pattern.noteCount=16;for(std::size_t n=0;n<16;++n)full->pattern.notes[n]={0,4,std::uint8_t(48+n),100,1};
    check(validateProjectDocument(*full),"unchanged full-capacity project/explicit16-note one-bar phrase");
    juce::MemoryBlock encoded;const auto decoded=std::make_unique<ProjectDocument>();
    check(encodeProjectState(*full,encoded)&&decodeProjectState(encoded.getData(),encoded.getSize(),*decoded)&&*decoded==*full,"exact full project native codec readback");
    const auto capacity=documentJson(full->sound);check(int(capacity["synth_count"])==4&&capacity["effects"].size()==8&&int(capacity["active_nonzero_routes"])==64,"full4-rack/8-effect/64-route source capacity");

    ++sections;
    const std::vector<Ns> raw{2,8,99,1};const auto known=statistics(raw,8);
    check(raw==std::vector<Ns>{2,8,99,1}&&known.mean==27.5&&known.median==5&&known.p99==99&&known.maximum==99&&known.misses==1,"raw outlier/strict deadline miss independently retained");
    rejects([&]{validateLinkedTrial(object());},"malformed/incomplete report never accepted");
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
    ++sections;
    auto linked=linkedTrial({"full-wavetable-mixed","linked-contention",48000,128,4,1},32);
    check(linked.isObject()&&bool(linked["integrity"]),"reduced public native linked helper establishes full workload integrity");
    const auto file=fixture.getChildFile("linked-reduced.json");writeJson(file,linked);
    if(bool(linked["integrity"])){
        try{validateLinkedTrial(linked);check(true,"independent linked report admission");}catch(const std::exception& e){std::cerr<<e.what()<<'\n';check(false,"independent linked report admission");}
        check(linked["callback_ns"].size()==4&&linked["callback_ns"][0].size()==32&&linked["serial_ns"].size()==32,"reduced fixed4x32 callback/serial raw observations");
        check(juce::int64(linked["measured_frames"])==4096&&juce::int64(linked["warmup_frames"])==12032&&bool(linked["selftest_reduced"]),"selftest geometry differs explicitly from production duration");
        check(bool(linked["all_admitted"])&&int(linked["selected_peer_count"])==3&&bool(linked["selection_preserved"]),"actual authenticated four-instance admission/ordered explicit three-peer selection");
        check(linked["publications_before"].size()==4&&linked["publications_after"].size()==4&&bool(linked["manual_publication_observed"])&&bool(linked["peer_musical_state_unchanged"]),"manual requester revision actually published while peer documents remain exact");
        check(juce::int64(linked["requester_edit_after"])==juce::int64(linked["requester_edit_before"])+1,"exactly one manual requester cutoff edit");
        check(bool(linked["renderer_priority_set"])&&int(linked["render_jobs_started"])>=1&&int(linked["render_jobs_started"])<=3&&int(linked["render_ready_count"])>=1,"real below-normal bounded sequential worker/ready result");
        check(int(linked["render_overlap_blocks"])>=8&&linked["render_overlap_flags"].size()==32&&bool(linked["renderer_joined"]),"actual measured callback/render overlap and joined worker");
        check(bool(linked["sticky_cancellation_requested"])&&bool(linked["no_jobs_started_after_measurement"])&&bool(linked["shutdown_complete"])&&int(linked["unexpected_backend_jobs"])==0,"sticky cancellation/end-boundary/no-provider orderly shutdown");
        check(linked["control_observations"].size()>=4&&bool(linked["roster_progress_observed"]),"real control/publication progress captured separately from callbacks");
        for(const auto* key:{"renderer_priority_set","manual_publication_observed","all_admitted","peer_musical_state_unchanged","selection_preserved","renderer_joined","shutdown_complete","roster_progress_observed"}){
            auto bad=juce::JSON::parse(juce::JSON::toString(linked));put(bad,key,false);check(!acceptLinkedTrial(bad),"missing public publication/active worker/state integrity cannot be accepted");
        }
        for(const auto* key:{"render_overlap_blocks","render_ready_count"}){auto bad=juce::JSON::parse(juce::JSON::toString(linked));put(bad,key,0);check(!acceptLinkedTrial(bad),"inactive or no-ready render workload rejected");}
        auto missing=juce::JSON::parse(juce::JSON::toString(linked));missing["callback_ns"][0].getArray()->removeLast();check(!acceptLinkedTrial(missing),"missing raw callback extent rejected");
        auto badTime=juce::JSON::parse(juce::JSON::toString(linked));badTime["callback_ns"][0].getArray()->set(0,J(1.5));check(!acceptLinkedTrial(badTime),"noninteger callback timing rejected");
        auto badAudio=juce::JSON::parse(juce::JSON::toString(linked));badAudio["audio_observations"][0][0].getDynamicObject()->setProperty("finite",false);check(!acceptLinkedTrial(badAudio),"nonfinite raw audio cannot be hidden by aggregate integrity");
        auto badPublication=juce::JSON::parse(juce::JSON::toString(linked));badPublication["publications_after"].getArray()->removeLast();check(!acceptLinkedTrial(badPublication),"missing published instance extent rejected");
    }
    ai::AuditionContext context;context.phrase=full->pattern;const auto cancelled=ai::renderPreview(*full,context,[]{return true;});
    check(cancelled.status==ai::RenderStatus::cancelled,"existing renderer honors immediate sticky cancellation without provider");
#endif
    check(!juce::File(fresh).exists(),"selftest never launches actual production41 profiling output");
    std::cout<<"ProductionProfile checks="<<checks<<" sections="<<sections<<" failures="<<failures<<" fixtures="<<fixture.getFullPathName()<<'\n';
    return failures?1:0;
}
// END PRODUCTION PROFILE SELF TEST
}

namespace {
SoundDocument profilePersonalityState()
{
    auto d=workload("full-wavetable-mixed");
    for(int n=0;n<12;++n){const bool synth=n<4;const int slot=synth?n:n-4;const auto module=synth?NativeModule::wavetable:NativeModule(unsigned(d.layout.effects[std::size_t(slot)].kind)+2);
        RackDesign design;design.id=juce::Uuid();design.name="Capacity metadata "+juce::String(n);design.role=synth?RackRole::synth:RackRole::fx;design.units={{0,module,{}}};design.panel.sections={"Native"};
        const auto key=synth?"wavetable.filter.cutoff":module==NativeModule::drive?"drive.mix":module==NativeModule::delay?"delay.mix":module==NativeModule::reverb?"reverb.mix":"compressor.mix";
        const auto i=*rackParameterIndex(module,std::uint8_t(slot),key);const auto& p=parameterRegistry()[i];design.panel.controls={{"Response",0,0,key,FocusWidget::rotary,FocusResponse::linear,p.minimum,p.maximum}};
        RackOperation op;op.kind=RackOperationKind::adopt;op.design=design;op.newInstance=juce::Uuid();op.destination.role=design.role;op.allocation={{0,std::uint8_t(slot)}};SoundDocument next;RackDifference diff;require(makeRackCandidate(d,op,SoundEditOrigin::manual,next,diff)==RackCandidateStatus::ready,"Full metadata adoption failed");d=std::move(next);
    }
    require(validateSoundDocument(d)&&d.personalities&&d.personalities->instances.size()==12,"Full12 metadata lost");return d;
}
J personalityControlTrial(const juce::File& output,int rate,int block,std::size_t reducedBlocks=0)
{
    const auto origin=Clock::now();const auto beforeCpu=cpuTime();auto d=profilePersonalityState();juce::MemoryBlock bytes;require(encodeSoundState(d,bytes),"Full metadata encoding failed");
    companion::ClientOptions offline;offline.allowLaunch=false;InstrumentProcessor p(offline);require(p.restoreSoundState(bytes.getData(),int(bytes.getSize())),"Full metadata recall failed");p.prepareToPlay(rate,block);
    juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(4096);notes(midi,16,0,true);for(Ns n=0;n<framesFor(.25,rate,block);n+=block){p.processBlock(audio,midi);midi.clear();}
    // A swap appends its replacement; vector position no longer identifies the captured synth.
    const auto selected=d.personalities->instances[0].id;
    const auto library=output.getChildFile("library-"+juce::String(rate)+".rack");RackDesign captured;require(p.captureRackDesign(selected,captured),"Live numeric design capture failed");require(saveRackDesign(captured,library),"Native library preparation save failed");
    std::atomic_bool stop=false,started=false;std::atomic<unsigned> completed=0;std::atomic_bool good=true;
    std::thread control([&]{started=true;try{while(!stop){RackDesign loaded;if(!loadRackDesign(library,loaded)){good=false;break;}std::uint64_t edit;SoundDocument current;if(!p.trySoundSnapshot(current,edit))continue;const auto found=std::find_if(current.personalities->instances.begin(),current.personalities->instances.end(),[&](const RackInstance& instance){return instance.id==selected;});if(found==current.personalities->instances.end()){good=false;break;}RackOperation swap;swap.kind=RackOperationKind::swap;swap.allowSlotReuse=true;swap.selected=selected;swap.design=loaded;swap.design->panel.accent=std::uint8_t(completed%3);swap.design->revision=found->design.revision+1;swap.allocation=found->bindings;SoundDocument next;RackDifference difference;if(p.previewRackOperation(swap,edit,next,difference)!=RackCandidateStatus::ready){good=false;break;}if(static_cast<const SoundAudioState&>(next)!=static_cast<const SoundAudioState&>(current)){good=false;break;}if(p.applyRackOperation(swap,edit)==RackCandidateStatus::ready)++completed;}}catch(...){good=false;}});
    while(!started)std::this_thread::yield();const auto blocks=reducedBlocks?reducedBlocks:std::size_t(framesFor(2,rate,block)/block);std::vector<Ns> raw(blocks);std::vector<AudioResult> observed(blocks);std::vector<unsigned> progress(blocks);J changes(juce::Array<J>{});
    try{for(std::size_t n=0;n<blocks;++n){
        if(n==blocks/3||n==2*blocks/3){stop=true;if(control.joinable())control.join();const auto t=Clock::now();require(p.restoreSoundState(bytes.getData(),int(bytes.getSize())),"Lifecycle full metadata recall failed");p.control(13).setValue(n==blocks/3?0.f:1.f);std::uint64_t edit;SoundDocument actual;require(p.trySoundSnapshot(actual,edit),"Engine reconciliation snapshot unavailable");auto item=object();put(item,"block",int(n));put(item,"control_ns",integer(elapsed(t)));put(item,"document",documentJson(actual));changes.append(item);notes(midi,16,0,true);}
        const auto t=Clock::now();p.processBlock(audio,midi);raw[n]=elapsed(t);midi.clear();scan(observed[n],audio);progress[n]=completed;}
    }catch(...){stop=true;if(control.joinable())control.join();p.releaseResources();throw;}
    stop=true;if(control.joinable())control.join();p.releaseResources();require(good,"Native library/metadata swap preparation failed validation");require(completed>0,"Concurrent metadata/library preparation never progressed");
    auto result=object();put(result,"rate",rate);put(result,"block",block);put(result,"document",documentJson(d));put(result,"personalities",12);put(result,"native_library",library.getFullPathName());put(result,"library_sha256",juce::SHA256(library).toHexString());put(result,"control_completed",int(completed));put(result,"lifecycle",changes);J times(juce::Array<J>{}),signal(juce::Array<J>{}),counts(juce::Array<J>{});
    for(std::size_t n=0;n<blocks;++n){require(observed[n].finite&&observed[n].peak<1,"Full metadata callback audio invalid");times.append(integer(raw[n]));signal.append(audioJson(observed[n]));counts.append(int(progress[n]));}
    const auto s=statistics(raw,deadlineNs(rate,block));put(result,"raw_callback_ns",times);put(result,"raw_audio",signal);put(result,"control_progress_after_callback",counts);put(result,"summary",summary(raw,deadlineNs(rate,block)));put(result,"target_met",s.mean<=.75*s.deadline&&s.p99<=s.deadline);put(result,"scope","processBlock plus clock overhead; scans/library/control/lifecycle excluded from callback timing; callback allocations/frees separately Debug CRT");const auto afterCpu=cpuTime();if(beforeCpu.available&&afterCpu.available){put(result,"process_user_ns",integer(Ns((afterCpu.user-beforeCpu.user)*100)));put(result,"process_kernel_ns",integer(Ns((afterCpu.kernel-beforeCpu.kernel)*100)));}put(result,"wall_ns",integer(elapsed(origin)));return result;
}
int personalityProfileSelfTest()
{
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
    try{const auto output=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-personalities-profile-selftest-"+juce::Uuid().toString());require(!output.exists()&&output.createDirectory().wasOk(),"Fresh profile self-test output required");
        for(const auto pair:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}}){const auto trial=personalityControlTrial(output,pair.first,pair.second,120);require(trial["lifecycle"].size()==2&&trial["raw_callback_ns"].size()==120&&int(trial["control_completed"])>0,"Profile self-test lost concurrent work or two lifecycle checkpoints");writeJson(output.getChildFile("control-"+juce::String(pair.first)+".json"),trial);}
        std::cout<<"personality profile lifecycle: two geometries, two checkpoints each, concurrent library/metadata progress; fixtures="<<output.getFullPathName()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"personality profile lifecycle FAIL: "<<e.what()<<'\n';return 1;}
#else
    return 0;
#endif
}
int profileRackPersonalities(const juce::File& output,const juce::File& inventory)
{
#if !defined(RACKSYNTH_PRODUCTION_PROFILE)
    throw std::runtime_error("rack-personalities requires existing native development profiling facilities");
#else
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Personality timings require Release");require(!output.exists()&&output.createDirectory().wasOk(),"Fresh personality profile output required");
    auto report=object();put(report,"suite","rack-personalities");put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"inventory",inventoryMetadata(inventory));const auto before=identities();put(report,"identities_before",before);J trials(juce::Array<J>{});bool targets=true,integrity=true;
    for(const auto pair:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}}){auto control=personalityControlTrial(output,pair.first,pair.second);targets=targets&&bool(control["target_met"]);writeJson(output.getChildFile("control-"+juce::String(pair.first)+".json"),control);trials.append(control);
        for(int repeat=1;repeat<=3;++repeat){auto linked=linkedTrial({"full-wavetable-mixed","linked-contention",pair.first,pair.second,4,repeat},0,true);const bool accepted=acceptLinkedTrial(linked);integrity=integrity&&accepted;bool target=accepted;for(const auto& s:*linked["callback_summaries"].getArray())target=target&&double(s["mean_callback_load"])<=.75&&double(s["p99_ns"])<=double(s["deadline_ns"]);put(linked,"target_met",target);targets=targets&&target;writeJson(output.getChildFile("linked-"+juce::String(pair.first)+"-"+juce::String(repeat)+".json"),linked);trials.append(linked);std::cout<<"PERSONALITY_PROFILE linked "<<pair.first<<'/'<<pair.second<<" repetition="<<repeat<<" target="<<target<<'\n';}}
    const auto after=identities();require(juce::JSON::toString(before)==juce::JSON::toString(after),"Profile identities changed during measurement");put(report,"identities_after",after);put(report,"trials",trials);put(report,"targets_met",targets);put(report,"integrity",integrity);put(report,"historical_limits","M6 outliers/support unchanged; these two points only; native serial sums are stress evidence, no DAW underrun or listening claim");writeJson(output.getChildFile("rack-personalities-profile-report.json"),report);std::cout<<(targets&&integrity?"SUCCESS":"UNRESOLVED")<<" rack-personalities profile raw callback targets\n";return targets&&integrity?0:1;
#endif
}
J qualityTailTrial(int rate,int block,int repeat,RenderQuality quality=RenderQuality::realtime,AlgorithmRevision reverbRevision=1,bool eqOnly=false,bool filterOnly=false,bool chorusOnly=false,bool phaserOnly=false,bool flangerOnly=false,bool stereoOnly=false,bool dimensionOnly=false,bool frequencyShiftOnly=false,bool convolutionOnly=false,bool frequencySplitOnly=false,bool midSideSplitOnly=false,bool wavetableUnisonOnly=false,const SoundDocument* supplied=nullptr)
{
    // A continuously running full-capacity FX chain after real note releases;
    // no MIDI Stop/reset/recall erases tails during this observation.
    auto d=supplied?*supplied:workload(wavetableUnisonOnly?"full-wavetable-unison":midSideSplitOnly?"full-mid-side-split":frequencySplitOnly?"full-frequency-split":convolutionOnly?"full-convolution":frequencyShiftOnly?"full-frequency-shift":dimensionOnly?"full-dimension":stereoOnly?"full-stereo":flangerOnly?"full-flanger":phaserOnly?"full-phaser":chorusOnly?"full-chorus":filterOnly?"full-filter":eqOnly?"full-eq":"full-reverb",reverbRevision);d.values[758]=quality==RenderQuality::higher?1.f:0.f;juce::MemoryBlock state;require(encodeSoundState(d,state),"Tail state encode failed");
    const Ns measured=framesFor(22,rate,block),releaseFrame=framesFor(.5,rate,block),warmup=framesFor(.25,rate,block);const auto blocks=std::size_t(measured/block);
    Instance p(block,blocks);require(p.processor->restoreSoundState(state.getData(),int(state.getSize())),"Tail state restore failed");p.processor->setNonRealtime(quality==RenderQuality::higher);p.processor->prepareToPlay(rate,block);
    require(p.processor->effectiveRenderQuality()==quality&&!p.processor->renderQualityPending(),"Tail effective mode differs from request");const auto latency=p.processor->getLatencySamples();const bool hostNonRealtime=p.processor->isNonRealtime();notes(p.midi,16,0,true);
    const auto reportedTail=p.processor->getTailLengthSeconds();double eqTail=0,filterTail=0,chorusTail=0,phaserTail=0,flangerTail=0,dimensionTail=0,frequencyShiftTail=0,convolutionTail=0;
    if(eqOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"EQ tail patch invalid");for(const auto& effect:patch.effects)eqTail+=detail::eqTailSeconds(effect.eq,rate*(quality==RenderQuality::higher?2.0:1.0),effect.algorithmRevision);require(std::isfinite(eqTail)&&eqTail>0&&std::isfinite(reportedTail)&&reportedTail>=eqTail,"EQ prepared tail omits effective-rate pole bound");}
    if(filterOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Filter tail patch invalid");for(const auto& effect:patch.effects)filterTail+=detail::filterTailSeconds(effect.filter,rate*(quality==RenderQuality::higher?2.0:1.0),effect.algorithmRevision);require(std::isfinite(filterTail)&&filterTail>0&&std::isfinite(reportedTail)&&reportedTail>=filterTail,"Filter prepared tail omits effective-rate pole bound");}
    if(chorusOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Chorus tail patch invalid");for(const auto& effect:patch.effects)chorusTail+=detail::chorusTailSeconds(effect.chorus,rate*(quality==RenderQuality::higher?2.0:1.0),effect.algorithmRevision);require(std::isfinite(chorusTail)&&chorusTail>0&&std::isfinite(reportedTail)&&reportedTail>=chorusTail,"Chorus prepared tail omits effective-rate feedback support");}
    if(phaserOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Phaser tail patch invalid");for(const auto& effect:patch.effects)phaserTail+=detail::phaserTailSeconds(effect.phaser,rate*(quality==RenderQuality::higher?2.0:1.0));require(std::isfinite(phaserTail)&&phaserTail>0&&std::isfinite(reportedTail)&&reportedTail>=phaserTail,"Phaser prepared tail omits effective-rate complete-loop stationary support");}
    if(flangerOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Flanger tail patch invalid");const auto effectiveRate=rate*(p.processor->effectiveRenderQuality()==RenderQuality::higher?2.0:1.0);for(const auto& effect:patch.effects)flangerTail+=detail::flangerTailSeconds(effect.flanger,effectiveRate,effect.algorithmRevision);require(std::isfinite(flangerTail)&&flangerTail>0&&std::isfinite(reportedTail)&&reportedTail>=flangerTail,"Flanger prepared tail omits effective-rate qualified complete-loop stationary support");}
    if(midSideSplitOnly){const double effectiveRate=rate*(quality==RenderQuality::higher?2.:1.),support=8*detail::midSideSplitTailSeconds(effectiveRate);require(std::isfinite(support)&&support>0&&std::isfinite(reportedTail)&&reportedTail>=support,"Mid side split prepared tail omits complete admitted-control cascade support");}
    if(frequencySplitOnly){const double effectiveRate=rate*(quality==RenderQuality::higher?2.:1.),support=8*detail::frequencySplitTailSeconds(effectiveRate,workloadFrequencySplitRevision);require(std::isfinite(support)&&support>0&&std::isfinite(reportedTail)&&reportedTail>=support,"Frequency split prepared tail omits complete admitted-control cascade support");}
    if(convolutionOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Convolution tail patch invalid");const double effectiveRate=rate*(quality==RenderQuality::higher?2.:1.);for(const auto& effect:patch.effects)convolutionTail+=detail::convolutionTailSeconds(effect.convolution,effectiveRate);require(std::isfinite(convolutionTail)&&convolutionTail>0&&std::isfinite(reportedTail)&&reportedTail>=convolutionTail,"Convolution prepared tail omits actual resampled finite IR plus maximum predelay/ramp/interpolation support");}
    if(frequencyShiftOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Frequency-shift tail patch invalid");const double effectiveRate=rate*(quality==RenderQuality::higher?2.:1.);for(const auto& effect:patch.effects)frequencyShiftTail+=detail::frequencyShiftTailSeconds(effect.frequencyShift,effectiveRate);require(std::isfinite(frequencyShiftTail)&&frequencyShiftTail>0&&std::isfinite(reportedTail)&&reportedTail>=frequencyShiftTail,"Frequency-shift prepared tail omits both cascades and Q delay at effective rate");}
    if(dimensionOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Dimension tail patch invalid");const auto effectiveRate=rate*(p.processor->effectiveRenderQuality()==RenderQuality::higher?2.0:1.0);for(const auto& effect:patch.effects)dimensionTail+=detail::dimensionTailSeconds(effect.dimension,effectiveRate,effect.algorithmRevision);require(std::isfinite(dimensionTail)&&dimensionTail>0&&std::isfinite(reportedTail)&&reportedTail>=dimensionTail,"Dimension prepared tail omits effective-rate finite-history support");}
    if(stereoOnly){RoutedPatch patch;require(makeRoutedPatch(d,patch),"Stereo source-tail patch invalid");for(std::size_t slot=0;slot<8;++slot)require(patch.layout.effects[slot].kind==EffectKind::stereo&&patch.effects[slot].algorithmRevision==1&&patch.compatibility.effects[slot][9]==1,"Stereo source-tail lost active family/revision");require(std::isfinite(reportedTail)&&reportedTail>=2.0*latency/rate,"Memoryless utility erased prepared source/quality support");}
    for(Ns frame=0;frame<warmup;frame+=block){step7Process(*p.processor,p.audio,p.midi);p.midi.clear();}
    for(std::size_t b=0;b<blocks;++b){if(Ns(b)*block==releaseFrame)notes(p.midi,16,0,false);const auto t=Clock::now();step7Process(*p.processor,p.audio,p.midi);p.observations[b].ns=elapsed(t);p.midi.clear();scan(p.observations[b].audio,p.audio);}
    J raw(juce::Array<J>{}),audio(juce::Array<J>{});std::vector<Ns> times;times.reserve(blocks);bool integrity=true;double latePeak=0;Ns lateNonzero=0,observedNonzero=0;
    for(std::size_t b=0;b<blocks;++b){const auto& o=p.observations[b];times.push_back(o.ns);raw.append(integer(o.ns));audio.append(audioJson(o.audio));observedNonzero+=Ns(o.audio.nonzero);integrity=integrity&&o.audio.finite&&o.audio.excursions==0;if(Ns(b)*block>=releaseFrame+rate){latePeak=std::max(latePeak,o.audio.peak);lateNonzero+=Ns(o.audio.nonzero);}}
    p.processor->releaseResources();auto result=object();put(result,"kind","continuous-long-tail");put(result,"tail_scope",wavetableUnisonOnly?"Fixed22s continuous active-tail window after real releases through the full-wavetable-mixed effects; not proved quiescence or full declared-tail coverage":midSideSplitOnly?"Fixed22s real release/source observation; complete fixed1k Mid/Side engineering support relative to input1e-6 is separate, no exact silence claim":frequencySplitOnly?"Fixed22s real release/source observation; complete negative-pole crossover/tone cascade engineering support relative to input1e-6 is separate, no exact silence claim":convolutionOnly?"Fixed22s real release/source observation; finite IR/max-predelay/ramp/interpolation support separately from source and Higher FIR, no whole-patch quiescence claim":frequencyShiftOnly?"Fixed22s real release/source observation, separate from both-cascade qualified relative support; no whole-patch quiescence claim":dimensionOnly?"Fixed22s source/downstream engineering observation; Dimension contributes finite history support, not late ringing or whole-patch quiescence":stereoOnly?"Fixed22s continuous source/downstream observation after real releases; Stereo utility contributes zero audio tail and smoothing emits no zero-input ringing. Prepared source and Higher FIR support remain separate; no late nonzero or whole-patch silence claim":flangerOnly?"Fixed 22s continuous active flanger-tail engineering observation after real releases; qualified complete-loop stationary support is reported separately, without whole-tail quiescence or arbitrary time-varying stability proof":phaserOnly?"Fixed 22s continuous active phaser-tail engineering observation after real releases; complete-loop stationary support is reported separately, without whole-tail quiescence or arbitrary time-varying stability proof":chorusOnly?"Fixed 22s continuous active chorus-tail engineering observation after real releases; not absolute silence, whole-tail quiescence or universal time-varying stability":filterOnly?"Fixed 22s continuous active filter-tail engineering observation after real releases; not absolute silence, whole-pole quiescence or general LTV stability":eqOnly?"Fixed 22s continuous active EQ-tail engineering observation after real releases; not absolute silence or whole-pole quiescence":"Fixed22s continuous active-tail window after real releases; not proved quiescence or full declared-tail coverage for eight serial maximum-decay reverbs");put(result,"selected_quality",int(quality));put(result,"effective_quality",int(quality));put(result,"host_latency_samples",latency);put(result,"host_non_realtime",hostNonRealtime);put(result,"workload",wavetableUnisonOnly?"full-wavetable-unison":midSideSplitOnly?"full-mid-side-split":frequencySplitOnly?"full-frequency-split":convolutionOnly?"full-convolution":frequencyShiftOnly?"full-frequency-shift":dimensionOnly?"full-dimension":stereoOnly?"full-stereo":flangerOnly?"full-flanger":phaserOnly?"full-phaser":chorusOnly?"full-chorus":filterOnly?"full-filter":eqOnly?"full-eq":"full-reverb");put(result,"sample_rate",rate);put(result,"block_size",block);put(result,"trial",repeat);put(result,"document",documentJson(d));put(result,"layers",4);put(result,"notes",16);put(result,"midi","Notes48..63, channel1 velocity100 at first warmup block; all16 note-offs at release_frame; no Stop");put(result,"effects",8);put(result,"warmup_frames",integer(warmup));put(result,"measured_frames",integer(measured));put(result,"release_frame",integer(releaseFrame));put(result,"tail_resets",0);put(result,"raw_callback_ns",raw);put(result,"audio_observations",audio);put(result,"late_peak",latePeak);put(result,"late_nonzero_samples",integer(lateNonzero));put(result,"summary",summary(times,deadlineNs(rate,block)));put(result,"integrity",integrity&&((stereoOnly||dimensionOnly||frequencyShiftOnly||convolutionOnly||frequencySplitOnly||midSideSplitOnly)?observedNonzero>0:latePeak>0&&lateNonzero>0));
    if(eqOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"eq_relative_engineering_tail_seconds",eqTail);put(result,"prepared_plugin_tail_seconds",reportedTail);}
    if(filterOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"filter_relative_engineering_tail_seconds",filterTail);put(result,"prepared_plugin_tail_seconds",reportedTail);}
    if(chorusOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"chorus_relative_engineering_tail_seconds",chorusTail);put(result,"prepared_plugin_tail_seconds",reportedTail);}
    if(phaserOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"phaser_complete_loop_stationary_engineering_tail_seconds",phaserTail);put(result,"prepared_plugin_tail_seconds",reportedTail);}
    if(flangerOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"flanger_qualified_complete_loop_stationary_engineering_tail_seconds",flangerTail);put(result,"prepared_plugin_tail_seconds",reportedTail);}
    if(midSideSplitOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"mid_side_split_engineering_support_seconds",8*detail::midSideSplitTailSeconds(rate*(quality==RenderQuality::higher?2:1)));put(result,"prepared_plugin_tail_seconds",reportedTail);put(result,"observed_source_nonzero_samples",integer(observedNonzero));}
    if(frequencySplitOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"frequency_split_engineering_support_seconds",8*detail::frequencySplitTailSeconds(rate*(quality==RenderQuality::higher?2:1),workloadFrequencySplitRevision));put(result,"prepared_plugin_tail_seconds",reportedTail);put(result,"observed_source_nonzero_samples",integer(observedNonzero));}
    if(convolutionOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"convolution_finite_history_support_seconds",convolutionTail);put(result,"prepared_plugin_tail_seconds",reportedTail);put(result,"observed_source_nonzero_samples",integer(observedNonzero));}
    if(frequencyShiftOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"frequency_shift_both_cascade_relative_support_seconds",frequencyShiftTail);put(result,"prepared_plugin_tail_seconds",reportedTail);put(result,"observed_source_nonzero_samples",integer(observedNonzero));}
    if(dimensionOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"dimension_finite_history_support_seconds",dimensionTail);put(result,"prepared_plugin_tail_seconds",reportedTail);put(result,"observed_source_nonzero_samples",integer(observedNonzero));}
    if(stereoOnly){put(result,"effective_internal_sample_rate",rate*(quality==RenderQuality::higher?2:1));put(result,"stereo_stage_audio_tail_seconds",0.0);put(result,"prepared_plugin_tail_seconds",reportedTail);put(result,"observed_source_nonzero_samples",integer(observedNonzero));}return result;
}
J processMemory()
{
    auto result=object();put(result,"scope","Whole process cumulative peak, includes previous trials and retained JUCE allocations; not isolated job RSS");
#if JUCE_WINDOWS
    using Query=BOOL(WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX counters {};counters.cb=sizeof(counters);
    if(query&&query(GetCurrentProcess(),reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),sizeof(counters))){put(result,"peak_working_set_bytes",integer(Ns(counters.PeakWorkingSetSize)));put(result,"working_set_bytes",integer(Ns(counters.WorkingSetSize)));put(result,"private_commit_bytes",integer(Ns(counters.PrivateUsage)));}else put(result,"status","UNAVAILABLE");
#endif
    return result;
}
J qualityReturnTrial(int rate,int block,AlgorithmRevision reverbRevision=0,bool eqOnly=false,bool filterOnly=false,bool chorusOnly=false,bool phaserOnly=false,bool flangerOnly=false,bool stereoOnly=false,bool dimensionOnly=false,bool frequencyShiftOnly=false,bool convolutionOnly=false,bool frequencySplitOnly=false,bool midSideSplitOnly=false,bool wavetableUnisonOnly=false,const SoundDocument* supplied=nullptr)
{
    auto d=supplied?*supplied:workload(wavetableUnisonOnly?"full-wavetable-unison":midSideSplitOnly?"full-mid-side-split":frequencySplitOnly?"full-frequency-split":convolutionOnly?"full-convolution":frequencyShiftOnly?"full-frequency-shift":dimensionOnly?"full-dimension":stereoOnly?"full-stereo":flangerOnly?"full-flanger":phaserOnly?"full-phaser":chorusOnly?"full-chorus":filterOnly?"full-filter":eqOnly?"full-eq":reverbRevision?"full-reverb":"full-wavetable-mixed",reverbRevision?reverbRevision:1);juce::MemoryBlock state;require(encodeSoundState(d,state),"Return state encode failed");
    Instance tested(block,0),control(block,0);for(auto* p:{&tested,&control}){require(p->processor->restoreSoundState(state.getData(),int(state.getSize())),"Return restore failed");p->processor->prepareToPlay(rate,block);}
    notes(tested.midi,16,0,true);step7Process(*tested.processor,tested.audio,tested.midi);tested.midi.clear();
    tested.processor->control(758).setValueNotifyingHost(1);tested.processor->setNonRealtime(true);require(tested.processor->renderQualityPending()&&tested.processor->effectiveRenderQuality()==RenderQuality::realtime,"Selection unexpectedly prepared higher");
    const auto begin=Clock::now();tested.processor->prepareToPlay(rate,block);const auto higherPrepare=elapsed(begin);require(tested.processor->effectiveRenderQuality()==RenderQuality::higher&&tested.processor->getLatencySamples()>0,"Higher transition not prepared");
    notes(tested.midi,16,0,true);step7Process(*tested.processor,tested.audio,tested.midi);tested.midi.clear();
    tested.processor->control(758).setValueNotifyingHost(0);tested.processor->setNonRealtime(false);require(tested.processor->renderQualityPending(),"Return mismatch not pending");
    const auto back=Clock::now();tested.processor->prepareToPlay(rate,block);const auto returnPrepare=elapsed(back);control.processor->prepareToPlay(rate,block);
    require(tested.processor->effectiveRenderQuality()==RenderQuality::realtime&&tested.processor->getLatencySamples()==0&&!tested.processor->renderQualityPending(),"Return realtime preparation failed");
    notes(tested.midi,16,0,true);notes(control.midi,16,0,true);
    const auto frames=framesFor(2,rate,block);std::vector<Ns> times;times.reserve(std::size_t(frames/block));J raw(juce::Array<J>{});AudioResult audio;double error=0;
    for(Ns frame=0;frame<frames;frame+=block){const auto t=Clock::now();step7Process(*tested.processor,tested.audio,tested.midi);const auto ns=elapsed(t);times.push_back(ns);raw.append(integer(ns));step7Process(*control.processor,control.audio,control.midi);for(int c=0;c<2;++c)for(int n=0;n<block;++n)error=std::max(error,std::abs(double(tested.audio.getSample(c,n))-control.audio.getSample(c,n)));scan(audio,tested.audio);tested.midi.clear();control.midi.clear();}
    auto result=object();put(result,"kind","same-owner realtime-higher-realtime");put(result,"document",documentJson(d));put(result,"sample_rate",rate);put(result,"block_size",block);put(result,"frames",integer(frames));put(result,"higher_prepare_ns",integer(higherPrepare));put(result,"return_prepare_ns",integer(returnPrepare));put(result,"latency_after_return",tested.processor->getLatencySamples());put(result,"fresh_realtime_control_max_difference",error);put(result,"raw_callback_ns",raw);put(result,"summary",summary(times,deadlineNs(rate,block)));put(result,"audio",audioJson(audio));put(result,"integrity",error==0&&audio.finite&&audio.nonzero>0&&audio.excursions==0);tested.processor->releaseResources();control.processor->releaseResources();return result;
}
J qualityPreviewTrial(RenderQuality quality,AlgorithmRevision reverbRevision=0,bool eqOnly=false,bool filterOnly=false,bool chorusOnly=false,bool phaserOnly=false,bool flangerOnly=false,bool stereoOnly=false,bool dimensionOnly=false,bool frequencyShiftOnly=false,bool convolutionOnly=false,bool frequencySplitOnly=false,bool midSideSplitOnly=false,bool wavetableUnisonOnly=false,const SoundDocument* supplied=nullptr)
{
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
    auto d=std::make_unique<ProjectDocument>();d->sound=supplied?*supplied:workload(wavetableUnisonOnly?"full-wavetable-unison":midSideSplitOnly?"full-mid-side-split":frequencySplitOnly?"full-frequency-split":convolutionOnly?"full-convolution":frequencyShiftOnly?"full-frequency-shift":dimensionOnly?"full-dimension":stereoOnly?"full-stereo":flangerOnly?"full-flanger":phaserOnly?"full-phaser":chorusOnly?"full-chorus":filterOnly?"full-filter":eqOnly?"full-eq":reverbRevision?"full-reverb":"full-wavetable-mixed",reverbRevision?reverbRevision:1);if(supplied||wavetableUnisonOnly||eqOnly||filterOnly||chorusOnly||phaserOnly||flangerOnly||stereoOnly||dimensionOnly||frequencyShiftOnly||convolutionOnly||frequencySplitOnly||midSideSplitOnly)d->sound.values[758]=quality==RenderQuality::higher?1.f:0.f;d->pattern.noteCount=16;for(std::size_t n=0;n<16;++n)d->pattern.notes[n]={0,4,std::uint8_t(48+n),100,1};ai::AuditionContext context;context.phrase=d->pattern;
    const auto begin=Clock::now();const auto rendered=ai::renderPreview(*d,context,{},512,quality);const auto ns=elapsed(begin);const auto fine=ai::renderPreview(*d,context,{},64,quality);int checks=0;const auto cancelBegin=Clock::now();const auto cancelled=ai::renderPreview(*d,context,[&]{return ++checks==3;},512,quality);const auto cancelNs=elapsed(cancelBegin);
    require(rendered.status==ai::RenderStatus::ready&&fine.status==ai::RenderStatus::ready&&rendered.quality==quality&&rendered.audio.left==fine.audio.left&&rendered.audio.right==fine.audio.right,"Native preview mode/partition failed");require(cancelled.status==ai::RenderStatus::cancelled&&cancelled.audio.left.empty()&&cancelled.audio.right.empty(),"Cancellation retained partial preview");
    auto result=object();put(result,"quality",int(rendered.quality));put(result,"document",documentJson(d->sound));put(result,"latency_samples",rendered.latencySamples);put(result,"latency_compensated",rendered.latencyCompensated);put(result,"body_frames",integer(Ns(rendered.audio.bodyFrames)));put(result,"total_frames",integer(Ns(rendered.audio.left.size())));put(result,"wall_ns",integer(ns));put(result,"audio_seconds_per_wall_second",double(rendered.audio.left.size())/ai::previewSampleRate/(double(ns)/1e9));put(result,"partition_exact",true);put(result,"cancel_checks",checks);put(result,"cancel_status",int(cancelled.status));put(result,"cancel_wall_ns",integer(cancelNs));put(result,"process_memory_after_preview",processMemory());return result;
#else
    throw std::runtime_error("Native preview facilities required for quality evidence");
#endif
}

// BEGIN TASK13 STEP7 DEVELOPMENT HELPER
constexpr std::array<const char*,4> step7Profiles{"eight-modern-reverb2","eight-convolution1","costly-mixed-forward","costly-mixed-reversed"};
SoundDocument step7Stimulus(const juce::String& profile)
{
    require(std::any_of(step7Profiles.begin(),step7Profiles.end(),[&profile](const char* name){return profile.compare(name)==0;}),"unknown Step7 stimulus");
    auto d=workload(profile==step7Profiles[0]?"full-reverb":profile==step7Profiles[1]?"full-convolution":"full-wavetable-mixed",2);d.compatibility=newInstanceKernelCompatibility();
    // Exact current supported controls; no new synth features or ceilings.
    for(int slot=0;slot<4;++slot){const auto prefix="synth."+juce::String(slot+1);set(d,prefix+".lfo.1.rate_hz",40);set(d,prefix+".lfo.2.beats",.0625f);for(const auto* bank:{".analog.",".wavetable."}){set(d,prefix+bank+"filter.resonance",.95f);set(d,prefix+bank+"filter_env.octaves",8);set(d,prefix+bank+"amp.release",20);}}
    const std::array<EffectKind,8> kinds{EffectKind::filter,EffectKind::drive,EffectKind::convolution,EffectKind::frequencyShift,EffectKind::chorus,EffectKind::flanger,EffectKind::reverb,EffectKind::compressor};
    if(profile==step7Profiles[2]||profile==step7Profiles[3])for(std::size_t n=0;n<8;++n)d.layout.effects[n].kind=kinds[n];
    // Set retained inactive banks too. Lifecycle replacement/recall witnesses
    // retain their exact defaults and the original ten-event ordering.
    for(int slot=0;slot<8;++slot){const auto p="effect."+juce::String(slot+1)+".";
        set(d,p+"drive.gain_db",24);set(d,p+"drive.shape",1);set(d,p+"drive.mix",1);
        set(d,p+"filter_fx.cutoff_hz",slot%2?20000:20);set(d,p+"filter_fx.resonance",10);set(d,p+"filter_fx.drive_db",24);set(d,p+"filter_fx.mix",1);
        set(d,p+"convolution.ir",float(slot%2));set(d,p+"convolution.predelay_ms",250);set(d,p+"convolution.wet_trim_db",12);set(d,p+"convolution.mix",.5f);
        set(d,p+"frequency_shift.hz",slot%2?1000:-1000);set(d,p+"frequency_shift.stereo_offset_hz",100);set(d,p+"frequency_shift.mix",.5f);
        set(d,p+"chorus.rate_hz",5);set(d,p+"chorus.depth",1);set(d,p+"chorus.feedback",.8f);set(d,p+"chorus.mix",.5f);
        set(d,p+"flanger.rate_hz",5);set(d,p+"flanger.depth",1);set(d,p+"flanger.feedback",.8f);set(d,p+"flanger.mix",.5f);
        set(d,p+"reverb.size",1);set(d,p+"reverb.decay_seconds",20);set(d,p+"reverb.predelay_ms",250);set(d,p+"reverb.mix",.35f);
    }
    if(profile==step7Profiles[3])for(std::size_t chain=0;chain<5;++chain)std::reverse(d.layout.effectOrder[chain].begin(),d.layout.effectOrder[chain].begin()+d.layout.effectCount[chain]);
    require(validateSoundDocument(d),"Step7 stimulus document");return d;
}
bool step7CallbackTargets(const J& row)
{
    if(row.hasProperty("summary"))return double(row["summary"]["mean_callback_load"])<=.75&&double(row["summary"]["p99_ns"])<=double(row["summary"]["deadline_ns"]);
    const auto* summaries=row["callback_summaries"].getArray();if(!summaries||summaries->isEmpty())return false;
    for(const auto& summary:*summaries)if(double(summary["mean_callback_load"])>.75||double(summary["p99_ns"])>double(summary["deadline_ns"]))return false;return true;
}
void step7FrameEvidence(J& row,int rate,int block)
{
    J actualFrames(juce::Array<J>{}),actualMisses(juce::Array<J>{});
    auto add=[&](const J& raw){const auto* samples=raw.getArray();require(samples&&!samples->isEmpty(),"Step7 raw callback samples absent");J frames(juce::Array<J>{});Ns missed=0;for(const auto& ns:*samples){require((ns.isInt64()||ns.isInt())&&juce::int64(ns)>=0,"Step7 raw callback time invalid");frames.append(block);missed+=double(juce::int64(ns))>deadlineNs(rate,block);}actualFrames.append(frames);actualMisses.append(integer(missed));};
    if(row.hasProperty("raw_callback_ns"))add(row["raw_callback_ns"]);else {const auto* callbacks=row["callback_ns"].getArray();require(callbacks&&!callbacks->isEmpty(),"Step7 callback rows absent");for(const auto& raw:*callbacks)add(raw);}
    put(row,"raw_actual_callback_frames",actualFrames);put(row,"deadline_misses_actual_callback_frames",actualMisses);
    put(row,"actual_frame_scope","These Profile loops process full blocks and framesFor rounds observation windows upward; every actual callback is block_size. Loaded Render partial-final callbacks remain separate actual-frame receipts; do not compare them to this full-block period or erase excursions.");
}
bool step7Integrity(const J& row,bool native)
{
    if(!bool(row["integrity"]))return false;
    if(native&&(!bool(row["public_generate_ai"])||!bool(row["captured_request_authority"])||!bool(row["public_result_readmitted"])||juce::int64(row["native_ai_overlap_blocks"])<8||juce::int64(row["joint_native_ai_background_overlap_blocks"])<8||int(row["native_ai"]["ready"])<=0))return false;
    const auto* arrays=row["raw_actual_callback_frames"].getArray();if(!arrays||arrays->isEmpty()||row["deadline_misses_actual_callback_frames"].size()!=arrays->size())return false;
    for(int n=0;n<arrays->size();++n){const auto* values=arrays->getReference(n).getArray();const auto raw=row.hasProperty("raw_callback_ns")?row["raw_callback_ns"]:row["callback_ns"][n];if(!values||values->isEmpty()||raw.size()!=values->size())return false;for(const auto& f:*values)if(!f.isInt()||int(f)!=int(row["block_size"]))return false;}
    return true;
}
J step7Tail(const SoundDocument& d,int rate,int block,int repeat,RenderQuality q)
{return qualityTailTrial(rate,block,repeat,q,2,false,false,false,false,false,false,false,false,false,false,false,false,&d);}
J step7Return(const SoundDocument& d,int rate,int block)
{return qualityReturnTrial(rate,block,2,false,false,false,false,false,false,false,false,false,false,false,false,&d);}
J step7Preview(const SoundDocument& d,RenderQuality q)
{return qualityPreviewTrial(q,2,false,false,false,false,false,false,false,false,false,false,false,false,&d);}

int profileTask13Step7(const juce::File& output,const juce::File& inventory)
{
#if JUCE_WINDOWS && defined(RACKSYNTH_PRODUCTION_PROFILE)
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release"||juce::String(RACKSYNTH_CONFIGURATION)=="Debug","Step7 meaningful configurations only");
    require(std::filesystem::create_directory(std::filesystem::path(output.getFullPathName().toWideCharPointer())),"Step7 fresh output creation");
    Step7AuditScope audit;const auto identitiesBefore=identities();auto report=object();put(report,"suite","task13-production");put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"identities_before",identitiesBefore);put(report,"inventory",inventoryMetadata(inventory));
    put(report,"required_realtime_budget","mean <= .75 * callback period; nearest-rank p99 <= callback period; every actual-frame excursion retained. Mean/p99 pass does not mean zero misses.");
    put(report,"historical_failures","F11.7 45 misses / four p99 failures UNWAIVED; every previous required failure/timeout retained separately. This is a new required production observation, not a waiver/retry of old evidence.");
    put(report,"method","Existing full-capacity steady/lifecycle/22s continuous tail/return/preview loops plus opt-in public native AI RuntimeServer background lane. Normal caller thread, original BELOW_NORMAL renderer and RuntimeServer worker. No affinity/MMCSS/priority/environment correction or silent scheduling substitution.");
    J rows(juce::Array<J>{}),returns(juce::Array<J>{}),previews(juce::Array<J>{});int ordinal=0;bool integrity=true,realtimeTargets=true,allocationTargets=true;
    auto persist=[&](J row,const juce::String& profile,const char* lane,const char* phase,RenderQuality q,int rate,int block,int repeat,bool native){
        step7FrameEvidence(row,rate,block);put(row,"ordinal",++ordinal);put(row,"step7_profile",profile);put(row,"step7_lane",lane);put(row,"step7_phase",phase);put(row,"step7_quality",int(q));put(row,"step7_repeat",repeat);const auto counters=step7AuditResult();put(row,"callback_allocation_audit",counters);
        const bool valid=step7Integrity(row,native);const bool target=valid&&step7CallbackTargets(row);put(row,"step7_integrity",valid);put(row,"realtime_comparison_target_met",target);put(row,"target_is_required",q==RenderQuality::realtime);integrity=integrity&&valid;if(q==RenderQuality::realtime)realtimeTargets=realtimeTargets&&target;
#if defined(_MSC_VER) && defined(_DEBUG)
        allocationTargets=allocationTargets&&bool(counters["zero"]);
#endif
        writeJson(output.getChildFile("trial-"+juce::String(ordinal).paddedLeft('0',3)+".json"),row);rows.append(row);std::cout<<"STEP7 "<<ordinal<<' '<<profile<<' '<<phase<<" integrity="<<valid<<" target="<<target<<std::endl;
    };
    for(const auto* profile:step7Profiles){auto d=step7Stimulus(profile);writeJson(output.getChildFile(juce::String(profile)+"-stimulus.json"),documentJson(d));for(const auto point:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(const auto q:{RenderQuality::realtime,RenderQuality::higher})for(int repeat=1;repeat<=3;++repeat){
        for(const auto* phase:{"steady","lifecycle"}){step7ResetAudit();auto row=runTrial({"full-wavetable-mixed",phase,point.first,point.second,1,repeat},q,0,false,&d);persist(row,profile,"isolated-audio",phase,q,point.first,point.second,repeat,false);}
        step7ResetAudit();persist(step7Tail(d,point.first,point.second,repeat,q),profile,"isolated-audio","continuous-22-second-tail",q,point.first,point.second,repeat,false);
    }}
    require(ordinal==144,"Step7 isolated matrix extent");
    for(const auto* profile:{step7Profiles[2],step7Profiles[3]}){const auto d=step7Stimulus(profile);for(const auto point:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(const auto q:{RenderQuality::realtime,RenderQuality::higher})for(int repeat=1;repeat<=3;++repeat){
        step7ResetAudit();auto row=linkedTrial({"full-wavetable-mixed","linked-contention",point.first,point.second,4,repeat},0,false,&d,q,true);persist(row,profile,"public-native-AI-and-background-render","four-linked-processor-callback-overlap",q,point.first,point.second,repeat,true);
    }}
    require(ordinal==168,"Step7 public-native/background matrix extent");
    for(const auto* profile:step7Profiles){const auto d=step7Stimulus(profile);for(const auto point:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}}){step7ResetAudit();auto row=step7Return(d,point.first,point.second);step7FrameEvidence(row,point.first,point.second);put(row,"step7_profile",profile);put(row,"callback_allocation_audit",step7AuditResult());const bool valid=step7Integrity(row,false);integrity=integrity&&valid;realtimeTargets=realtimeTargets&&valid&&step7CallbackTargets(row);
#if defined(_MSC_VER) && defined(_DEBUG)
        allocationTargets=allocationTargets&&bool(row["callback_allocation_audit"]["zero"]);
#endif
        writeJson(output.getChildFile(juce::String(profile)+"-return-"+juce::String(point.first)+".json"),row);returns.append(row);}
        for(const auto q:{RenderQuality::realtime,RenderQuality::higher}){auto row=step7Preview(d,q);put(row,"step7_profile",profile);put(row,"memory_scope","Whole process current RSS/private commit and cumulative peak; not isolated render/job-local peak");writeJson(output.getChildFile(juce::String(profile)+"-preview-"+juce::String(int(q))+".json"),row);previews.append(row);}
    }
    require(returns.size()==8&&previews.size()==8,"Step7 oracle matrix extent");put(report,"callback_trials",rows);put(report,"return_oracles",returns);put(report,"native_preview_oracles",previews);put(report,"expected_observations",184);put(report,"callback_integrity",integrity);put(report,"realtime_callback_targets_met",realtimeTargets);put(report,"process_memory",processMemory());put(report,"identities_after",identities());
#if defined(_MSC_VER) && defined(_DEBUG)
    put(report,"allocation_gate_status",allocationTargets?"OBSERVED_ZERO":"FAIL");put(report,"timing_gate_status","DEBUG_INSTRUMENTED_DISTRIBUTIONS_NOT_RELEASE_TIMING_ACCEPTANCE");
#else
    put(report,"allocation_gate_status","PENDING_MATCHED_DEBUG_EVIDENCE");put(report,"timing_gate_status",realtimeTargets?"PASS_WITH_RAW_EXCURSIONS_RETAINED":"FAIL");
#endif
    writeJson(output.getChildFile("task13-production-profile-report.json"),report);
    return integrity&&allocationTargets&&(juce::String(RACKSYNTH_CONFIGURATION)=="Debug"||realtimeTargets)?0:1;
#else
    throw std::runtime_error("Step7 needs Windows development probes");
#endif
}
int profileTask13DriveBackgroundDiagnostic(const juce::File& output,const juce::File& inventory)
{
#if JUCE_WINDOWS && defined(RACKSYNTH_PRODUCTION_PROFILE)
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","drive diagnostic requires Release");
    require(std::filesystem::create_directory(std::filesystem::path(output.getFullPathName().toWideCharPointer())),"drive diagnostic fresh output creation");
    Step7AuditScope audit;const auto identitiesBefore=identities();auto report=object();
    put(report,"suite","task13-drive-background-diagnostic");put(report,"configuration",RACKSYNTH_CONFIGURATION);
    put(report,"diagnostic_only",true);put(report,"campaign_pass",false);put(report,"identities_before",identitiesBefore);put(report,"inventory",inventoryMetadata(inventory));
    put(report,"required_realtime_budget","Unchanged mean <= .75 * full callback period; nearest-rank p99 <= callback period; every raw excursion retained");
    put(report,"scope","Original four required mixed/public-native-AI/background workloads151/153/163/165 only. No component cost attribution, full184/Debug allocation/cross-configuration or whole campaign acceptance");
    put(report,"method","Existing step7Stimulus/linkedTrial full work reducedBlocks0, RT96k512/four linked processors, normal caller and original BELOW_NORMAL workers; same public account/catalog/generateAi/native readmission/joint background overlap requirements");
    J rows(juce::Array<J>{});int ordinal=144;bool integrity=true,realtimeTargets=true;
    for(const auto* profile:{step7Profiles[2],step7Profiles[3]}){const auto d=step7Stimulus(profile);for(const auto point:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(const auto q:{RenderQuality::realtime,RenderQuality::higher})for(int repeat=1;repeat<=3;++repeat){
        ++ordinal;if(ordinal!=151&&ordinal!=153&&ordinal!=163&&ordinal!=165)continue;
        step7ResetAudit();auto row=linkedTrial({"full-wavetable-mixed","linked-contention",point.first,point.second,4,repeat},0,false,&d,q,true);
        step7FrameEvidence(row,point.first,point.second);put(row,"ordinal",ordinal);put(row,"step7_profile",profile);
        put(row,"step7_lane","public-native-AI-and-background-render");put(row,"step7_phase","four-linked-processor-callback-overlap");
        put(row,"step7_quality",int(q));put(row,"step7_repeat",repeat);put(row,"callback_allocation_audit",step7AuditResult());
        const bool valid=step7Integrity(row,true),target=valid&&step7CallbackTargets(row);put(row,"step7_integrity",valid);put(row,"realtime_comparison_target_met",target);put(row,"target_is_required",q==RenderQuality::realtime);
        integrity=integrity&&valid;realtimeTargets=realtimeTargets&&target;
        writeJson(output.getChildFile("trial-"+juce::String(ordinal).paddedLeft('0',3)+".json"),row);rows.append(row);
        std::cout<<"DRIVE_DIAGNOSTIC "<<ordinal<<' '<<profile<<" integrity="<<valid<<" target="<<target<<" diagnostic_only=1 campaign_pass=0"<<std::endl;
    }}
    require(ordinal==168&&rows.size()==4,"drive diagnostic exact four original workloads");
    put(report,"callback_trials",rows);put(report,"expected_observations",4);put(report,"callback_integrity",integrity);put(report,"realtime_callback_targets_met",realtimeTargets);
    put(report,"timing_gate_status",realtimeTargets?"DIAGNOSTIC_TARGETS_MET_ONLY":"DIAGNOSTIC_TIMING_FAILURE");put(report,"allocation_gate_status","PENDING_REQUIRED_COMPLETE_DEBUG");
    put(report,"full184_status","PENDING_REQUIRED_COMPLETE_RELEASE_AND_DEBUG_ORIGINAL_PAIRED_VALIDATION");put(report,"process_memory",processMemory());put(report,"identities_after",identities());
    writeJson(output.getChildFile("task13-drive-background-diagnostic-report.json"),report);
    std::cout<<"Task13 drive background diagnostic observations="<<rows.size()<<" integrity="<<integrity<<" target="<<realtimeTargets<<" diagnostic_only=1 campaign_pass=0"<<std::endl;
    return integrity&&realtimeTargets?0:1;
#else
    throw std::runtime_error("drive diagnostic needs Windows development probes");
#endif
}

int task13Step7SelfTest()
{
    int failures=0;auto check=[&](bool ok,const char* label){if(!ok){++failures;std::cerr<<"FAIL Step7: "<<label<<'\n';}};
    auto rejects=[&](auto f,const char* label){try{f();check(false,label);}catch(const std::exception&) {check(true,label);}};
    const auto fresh=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/task13-step7-parser-selftest-"+juce::Uuid().toString()).getFullPathName();
    check(parseArguments({"--profile","--suite","task13-production","--output",fresh}).suite=="task13-production","opt-in suite admitted without filesystem work");
    check(parseArguments({"--profile","--suite","task13-drive-background-diagnostic","--output",fresh}).suite=="task13-drive-background-diagnostic","diagnostic selector admitted without filesystem work");
    for(const auto& args:std::vector<juce::StringArray>{{"--profile","--suite","task13-drive-background-diagnostic","--output","relative"},{"--profile","--suite","task13-drive-background-diagnostic","--output",fresh,"--quality","higher"},{"--profile","--suite","task13-drive-background-diagnostic","--output",fresh,"--eq-only"}})rejects([&]{parseArguments(args);},"diagnostic wrong path/workload-changing selector rejected");
    for(const auto& args:std::vector<juce::StringArray>{{"--profile","--suite","task13-production","--output","relative"},{"--profile","--suite","task13-production","--output",fresh,"--quality","higher"},{"--profile","--suite","task13-production","--output",fresh,"--eq-only"}})rejects([&]{parseArguments(args);},"Step7 wrong path/matrix-changing selector rejected");
    check(!juce::File(fresh).exists(),"Step7 parser creates no measured workload");
    auto forward=step7Stimulus(step7Profiles[2]),reverse=step7Stimulus(step7Profiles[3]);const auto f=documentJson(forward),r=documentJson(reverse);check(int(f["synth_count"])==4&&int(f["active_nonzero_routes"])==64&&f["effects"].size()==8,"actual full capacity admitted through existing codec");
    check(f["effects"][0]["kind"].equals(J(int(EffectKind::filter)))&&f["effects"][2]["kind"].equals(J(int(EffectKind::convolution)))&&f["effects"][6]["algorithm_revision"].equals(J(2)),"mixed expensive families and actual active revision");
    check(reverse.layout.effectOrder[4][0]==7&&forward.layout.effectOrder[4][0]==4&&forward.values==reverse.values&&forward.compatibility==reverse.compatibility,"reordered pair changes master order only");
    for(const auto* name:step7Profiles){auto d=step7Stimulus(name);juce::MemoryBlock state;SoundDocument decoded;check(encodeSoundState(d,state)&&decodeSoundState(state.getData(),state.getSize(),decoded)&&decoded==d,"every exact workload codec roundtrip");}
    for(const auto* name:step7Profiles)check(step7Stimulus(name).compatibility==newInstanceKernelCompatibility(),"stimulus plays a fresh instance's kernels");
    rejects([&]{step7Stimulus("unknown");},"unsupported workload rejected");
#if JUCE_WINDOWS && defined(RACKSYNTH_PRODUCTION_PROFILE)
    Step7AuditScope audit;step7ResetAudit();auto live=linkedTrial({"full-wavetable-mixed","linked-contention",48000,128,4,1},32,false,&forward,RenderQuality::realtime,true);step7FrameEvidence(live,48000,128);check(step7Integrity(live,true),"public generateAi/account/catalog/native render/wire/readmission and joint callback overlap");
    for(const auto* field:{"public_generate_ai","captured_request_authority","public_result_readmitted"}){auto bad=juce::JSON::parse(juce::JSON::toString(live));put(bad,field,false);check(!step7Integrity(bad,true),"missing actual public path cannot accept");}
    for(const auto* field:{"native_ai_overlap_blocks","joint_native_ai_background_overlap_blocks"}){auto bad=juce::JSON::parse(juce::JSON::toString(live));put(bad,field,0);check(!step7Integrity(bad,true),"absent native or joint background callback overlap cannot accept");}
    auto bad=juce::JSON::parse(juce::JSON::toString(live));bad["raw_actual_callback_frames"][0].getArray()->clear();check(!step7Integrity(bad,true),"missing raw actual callback frames cannot accept");
#if defined(_MSC_VER) && defined(_DEBUG)
    check(bool(step7AuditResult()["zero"]),"public-contention prepared callbacks allocate and free zero");
#endif
#endif
    std::cout<<"Task13 Step7 self-test failures="<<failures<<'\n';return failures?1:0;
}
// END TASK13 STEP7 DEVELOPMENT HELPER

// Tool-only equivalence candidate: production HistoryRing and all legacy
// consumers are unchanged. This measures index arithmetic, not DSP quality.
struct BranchIndexProbeRing
{
    std::vector<double> data;std::size_t head=0,written=0;
    void prepare(std::size_t capacity){data.assign(capacity+2,0.0);clear();}
    void clear(){head=written=0;}
    double read(double delay)const noexcept{
        delay=std::clamp(delay,1.0,double(data.size()-2));const auto integral=std::size_t(delay);
        const auto a=head>=integral?head-integral:head+data.size()-integral;
        const auto b=a==0?data.size()-1:a-1;
        const double recent=integral<=written?data[a]:0.0,older=integral+1<=written?data[b]:0.0;
        return recent+(older-recent)*(delay-double(integral));
    }
    void write(double x){data[head]=x;if(++head==data.size())head=0;if(written<data.size())++written;}
};
J ringCostProbe()
{
    constexpr int rate=96000,frames=96000;constexpr std::size_t lines=detail::reverb2DelaySeconds.size();
    std::size_t checks=0;
    for(const std::size_t capacity:{3u,17u,1632u,19200u}){
        detail::HistoryRing original;BranchIndexProbeRing candidate;original.prepare(capacity);candidate.prepare(capacity);
        for(std::size_t frame=0;frame<3*(capacity+2);++frame){
            if(frame==capacity+1){original.clear();candidate.clear();}
            for(double delay:{0.0,1.0,1.25,2.75,double(capacity)-.5,double(capacity),double(capacity)+.75}){
                require(std::bit_cast<std::uint64_t>(original.read(delay))==std::bit_cast<std::uint64_t>(candidate.read(delay)),"Tool ring indexing equivalence failed");++checks;
            }
            const double input=double(int(frame%23)-11)*.03125;original.write(input);candidate.write(input);
        }
    }
    std::array<std::vector<double>,lines> delays;
    for(std::size_t line=0;line<lines;++line){delays[line].resize(frames);double phase=2*std::numbers::pi*double(line)/lines;
        const double step=2*std::numbers::pi*(.137+.017*double(line))/rate;
        for(int frame=0;frame<frames;++frame){delays[line][std::size_t(frame)]=(2*detail::reverb2DelaySeconds[line]+detail::reverb2ModulationSeconds*std::sin(phase))*rate;phase+=step;if(phase>=2*std::numbers::pi)phase-=2*std::numbers::pi;}}
    std::vector<double> input(frames);for(int frame=0;frame<frames;++frame)input[std::size_t(frame)]=.125*std::sin(.013*frame);
    auto pass=[&]<class Ring>(){
        std::array<std::array<Ring,lines+8>,8> rings;
        for(auto& effect:rings)for(std::size_t line=0;line<lines+8;++line){const auto capacity=std::size_t(std::ceil(rate*(line<lines?.2:.017)));effect[line].prepare(capacity);
            for(std::size_t k=0;k<capacity+2;++k)effect[line].write(double(int(k%31)-15)*.001);}
        double checksum=0;const auto begin=Clock::now();
        for(int frame=0;frame<frames;++frame)for(auto& effect:rings)for(std::size_t line=0;line<lines+8;++line){
            const double delay=line<lines?delays[line][std::size_t(frame)]:detail::reverb2AllpassSeconds[(line-lines)/4][(line-lines)%4]*1.25*rate;
            const double value=effect[line].read(delay);effect[line].write(input[std::size_t(frame)]+.97*value);checksum+=value;
        }
        return std::pair{elapsed(begin),checksum};
    };
    auto result=object();J trials(juce::Array<J>{});std::vector<Ns> originalTimes,candidateTimes;
    for(int repeat=0;repeat<3;++repeat){std::pair<Ns,double> original,candidate;
        if(repeat%2==0){original=pass.template operator()<detail::HistoryRing>();candidate=pass.template operator()<BranchIndexProbeRing>();}
        else{candidate=pass.template operator()<BranchIndexProbeRing>();original=pass.template operator()<detail::HistoryRing>();}
        require(std::bit_cast<std::uint64_t>(original.second)==std::bit_cast<std::uint64_t>(candidate.second)&&std::isfinite(original.second)&&original.second!=0,"Timed ring checksums differ/invalid");
        originalTimes.push_back(original.first);candidateTimes.push_back(candidate.first);auto row=object();put(row,"repeat",repeat+1);put(row,"original_first",repeat%2==0);put(row,"original_ns",integer(original.first));put(row,"candidate_ns",integer(candidate.first));put(row,"checksum",original.second);put(row,"checksum_bit_exact",true);trials.append(row);
    }
    put(result,"scope","Actual HistoryRing versus tool-local branch arithmetic; same prepared storage geometry/read-write sequence, synthetic nonzero input. Not sampled owning-callback attribution or product audio acceptance.");
    put(result,"sample_rate",rate);put(result,"frames",frames);put(result,"effects",8);put(result,"network_reads_per_effect_frame",int(lines));put(result,"allpass_reads_per_effect_frame",8);put(result,"reads_per_trial",integer(Ns(frames)*8*Ns(lines+8)));put(result,"equivalence_checks",integer(Ns(checks)));put(result,"fixed_size_control",1);put(result,"precomputed_delay_note","Same canonical phases/frequencies/depth at physical96k; sine generation excluded from both ring timings. No per-candidate delay/gain fitting.");put(result,"trials",trials);
    put(result,"original_timing",summary(originalTimes,1e9));put(result,"candidate_timing",summary(candidateTimes,1e9));put(result,"integrity",true);return result;
}
int diagnoseReverbCost(const juce::File& output)
{
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Component timings require Release");require(!output.exists()&&output.createDirectory().wasOk(),"Fresh component output required");
    auto result=object();const auto before=identities();put(result,"suite","reverb-cost");put(result,"configuration",RACKSYNTH_CONFIGURATION);put(result,"identities_before",before);
    put(result,"scope","Development-only rotated96k/512 full4layer16note64route modern2/legacy1/ordinarybypass isolation. Same controls/MIDI; not quality-adoption or substitute for full workload gates.");
    J trials(juce::Array<J>{});for(int repeat=0;repeat<3;++repeat)for(int position=0;position<3;++position){const int variant=(position+repeat)%3;
        auto trial=runTrial({"full-reverb","steady",96000,512,1,repeat+1},RenderQuality::realtime,variant==1?1:2,variant==2);
        require(acceptTrial(trial),"Component wrapper integrity failure");put(trial,"component_variant",variant==0?"modern2":variant==1?"legacy1":"modern2-all8bypassed");put(trial,"order_in_round",position+1);
        const auto filename="trial-"+juce::String(repeat+1)+"-"+juce::String(position+1)+".json";writeJson(output.getChildFile(filename),trial);put(trial,"trial_file",filename);trials.append(trial);
        std::cout<<"COMPONENT "<<trial["component_variant"].toString()<<" repeat="<<repeat+1<<std::endl;
    }
    put(result,"trials",trials);put(result,"ring_probe",ringCostProbe());const auto after=identities();put(result,"identities_after",after);put(result,"identities_unchanged",juce::JSON::toString(before)==juce::JSON::toString(after));require(bool(result["identities_unchanged"]),"Component identities changed");put(result,"integrity",true);writeJson(output.getChildFile("reverb-cost-report.json"),result);std::cout<<"SUCCESS component diagnosis; no product adoption\n";return 0;
}
int profileSoundQuality(const juce::File& output,const juce::File& inventory,RenderQuality quality,AlgorithmRevision reverbRevision=0,bool eqOnly=false,bool filterOnly=false,bool chorusOnly=false,bool phaserOnly=false,bool flangerOnly=false,bool stereoOnly=false,bool dimensionOnly=false,bool frequencyShiftOnly=false,bool convolutionOnly=false,bool frequencySplitOnly=false,bool midSideSplitOnly=false,bool wavetableUnisonOnly=false,bool eraReverb=false)
{
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Sound-quality timings require Release");
    std::unique_ptr<SoundDocument> era;if(eraReverb)era=std::make_unique<SoundDocument>(eraWorkload());require(!output.exists()&&output.createDirectory().wasOk(),"Fresh quality profile output required");
    auto report=object();const auto before=identities();put(report,"suite","sound-quality");put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"identities_before",before);put(report,"inventory",inventoryMetadata(inventory));put(report,"scope","Native InstrumentProcessor; actual loaded VST3 audio is the separate render packet. Callback clocks include clock/store overhead, scans/control/report work excluded; excursions are not measured DAW underruns.");
    put(report,"selected_quality",int(quality));put(report,"effective_quality","Verified per trial from prepared processor");put(report,"deadline_policy",quality==RenderQuality::higher?"Offline cost only; unchanged realtime target reported separately":"Unchanged mean<=.75 and p99<=deadline gates");put(report,"algorithm_provenance","Current legacy revision1 kernels, explicit retained-bank revision metadata");put(report,"raw_engine_allocations_frees","Separate MSVC Debug quality_callback_allocations_frees tests instrument RoutedSynthEngine::render, not this wrapper");put(report,"wrapper_allocations_frees","Release InstrumentProcessor timings are uninstrumented; fresh existing Debug InstrumentTests boundary evidence is reported separately; no inference from raw-engine zero counts");put(report,"listening","NOT_RUN");put(report,"historical_failures","Original F11.7 45misses/fourp99failures are UNWAIVED; this independent baseline cannot replace them.");
#if JUCE_WINDOWS
    MEMORYSTATUSEX memory {};memory.dwLength=sizeof(memory);if(GlobalMemoryStatusEx(&memory)){put(report,"physical_memory_bytes",integer(Ns(memory.ullTotalPhys)));put(report,"available_memory_bytes_at_start",integer(Ns(memory.ullAvailPhys)));}
    SYSTEM_INFO cpu {};GetNativeSystemInfo(&cpu);put(report,"logical_processors",int(cpu.dwNumberOfProcessors));put(report,"processor_identifier",juce::SystemStats::getEnvironmentVariable("PROCESSOR_IDENTIFIER","unavailable"));
#endif
    J trials(juce::Array<J>{});bool integrity=true,targets=true;int number=0;
    put(report,"synth_algorithm_revision",int(workloadSynthRevision));put(report,"drive_algorithm_revision",int(workloadDriveRevision));
    put(report,"compressor_algorithm_revision",int(workloadCompressorRevision));
    put(report,"modulated_tap_algorithm_revision",int(workloadModulatedTapRevision));
    put(report,"delay_algorithm_revision",int(workloadDelayRevision));
    put(report,"eq_algorithm_revision",int(workloadEqRevision));
    put(report,"filter_algorithm_revision",int(workloadFilterRevision));
    const auto tapRevision=juce::String(int(workloadModulatedTapRevision)),eqRevision=juce::String(int(workloadEqRevision)),filterRevision=juce::String(int(workloadFilterRevision));
    const auto tapRead=juce::String(workloadModulatedTapRevision==modernModulatedTapAlgorithmRevision?"16-point windowed-sinc":"linear");
    juce::StringArray modern;
    if(workloadSynthRevision==modernSynthAlgorithmRevision)modern.add("synth voices");
    if(workloadDriveRevision==modernDriveAlgorithmRevision)modern.add("drives");
    if(workloadCompressorRevision==modernCompressorAlgorithmRevision)modern.add("compressors");
    if(workloadModulatedTapRevision==modernModulatedTapAlgorithmRevision)modern.add("chorus, flanger and dimension taps");
    if(workloadDelayRevision==modernDelayAlgorithmRevision)modern.add("delays");
    if(workloadEqRevision==modernEqAlgorithmRevision)modern.add("EQs");
    if(workloadFilterRevision==modernFilterAlgorithmRevision)modern.add("filters");
    if(workloadFrequencySplitRevision==modernFrequencySplitAlgorithmRevision)modern.add("frequency splits");
    if(!modern.isEmpty())put(report,"algorithm_provenance","Revision2 "+modern.joinIntoString(", ")+"; other kernels legacy revision1, explicit retained-bank revision metadata");
    if(reverbRevision){put(report,"algorithm_provenance",reverbRevision==2?(workloadSynthRevision==modernSynthAlgorithmRevision?"Actual all-eight Reverb2 workload with revision2 synth voices; other retained kernels remain revision1":"Actual all-eight Reverb2 workload; other retained/synth kernels remain revision1"):"Explicit all-eight legacy Reverb1 workload");put(report,"reverb_algorithm_revision",int(reverbRevision));put(report,"reverb_lines",int(reverbRevision==2?detail::reverb2DelaySeconds.size():4));put(report,"lifecycle_note","All8 selected reverbs at start/recall/Stop/resume; existing replacement switches to compressor and unavailable-tempo case temporarily selects delays, then restores selected reverbs. Every active/retained bank is captured.");}
    if(eqOnly){put(report,"eq_only",true);put(report,"algorithm_provenance","Actual 4 mixed analog/digital layers, 16 notes, 64 active nonzero routes and 8 independent nonzero EQ"+eqRevision+" stages; all active/retained banks are captured");put(report,"lifecycle_note","Actual all 9 ordinary EQ controls per slot edited at the automation boundary; temporary compressor replacement and missing-tempo Delay oracle are captured separately, then baseline recall and tempo restore return all 8 active families to EQ"+eqRevision+". Stop/resume retain EQ; active tails are a fixed 22s engineering observation, not whole-pole quiescence.");}
    if(filterOnly){put(report,"filter_only",true);put(report,"algorithm_provenance","Actual 4 mixed analog/digital layers, 16 notes, 64 active nonzero routes and 8 independent nonzero Filter"+filterRevision+" stages; all active/retained banks and physical settings are captured");put(report,"lifecycle_note","Actual all 5 ordinary filter controls per slot edited at the automation boundary; temporary compressor replacement and missing-tempo Delay oracle are captured separately, then baseline recall and tempo restore return all 8 active families to Filter"+filterRevision+". Stop/resume retain Filter"+filterRevision+"; active tails are a fixed 22s engineering observation, not whole-pole quiescence or general LTV stability.");}
    if(chorusOnly){put(report,"chorus_only",true);put(report,"algorithm_provenance","Actual 4 mixed analog/digital layers, 16 notes, 64 active nonzero routes and 8 independent nonzero Chorus"+tapRevision+" stages; all active/retained banks and physical settings are captured");put(report,"chorus_algorithm_revision",int(workloadModulatedTapRevision));put(report,"lifecycle_note","Actual all 5 ordinary chorus controls per slot edited at the automation boundary, including centre12..40ms and positive/negative feedback; temporary compressor replacement and missing-tempo Delay oracle are captured separately, then baseline recall and tempo restore return all 8 active families to Chorus"+tapRevision+". Stop/resume retain Chorus"+tapRevision+"; active tails are a fixed 22s engineering observation, not whole-tail quiescence or universal time-varying stability.");put(report,"chorus_smoothing_history_policy","Stock rate/depth/feedback smooth over50ms of active internal frames; external centre/mix ramps advance over5ms on valid internal frames including inactive/bypassed banks. Structural reactivation resets audio history/phase and snaps stock pending targets; no continuous bypassed LFO claim.");put(report,"chorus_channel_policy","Shared LFO with independent channel histories; dual-mono modulation preserves mono and left-only input, without a stereo widening/crossfeed claim.");}
    if(phaserOnly){put(report,"phaser_only",true);put(report,"algorithm_provenance","Actual 4 mixed analog/digital layers, 16 notes, 64 active nonzero routes and 8 independent nonzero Phaser1 stages; all active/retained banks and physical settings are captured");put(report,"phaser_algorithm_revision",1);put(report,"lifecycle_note","Actual all 5 ordinary phaser controls per slot edited at the automation boundary, including centre20..20000Hz and positive/negative feedback; temporary compressor replacement and missing-tempo Delay oracle are captured separately, then baseline recall and tempo restore return all 8 active families and exact retained settings to Phaser1. Stop/resume retain Phaser1; active tails are a fixed 22s engineering observation separate from complete-loop stationary support, without whole-tail quiescence or arbitrary time-varying stability proof.");put(report,"phaser_smoothing_history_policy","Stock rate/depth/feedback smooth over50ms of active internal frames; external centre/mix ramps advance over5ms on valid internal frames including inactive/bypassed banks. Structural reactivation resets audio history/phase and snaps stock pending targets; no continuous bypassed LFO claim.");put(report,"phaser_clock_centre_policy","Stock coefficient updates every4 internal frames with persistent counter; LFO rate is physical Hz at internalRate/4. Centre clamps to20..min(20000,.49*internalRate); log-normalized depth sweep clips at0..1. Endpoint fixtures do not imply unclipped or linear-Hz modulation.");put(report,"phaser_channel_tail_policy","Shared LFO and six allpasses with independent channel histories and one-frame signed feedback; dual-mono processing without widening/crossfeed. Tail helper uses the complete six-allpass/feedback stationary loop and global admitted cutoff envelope at actual effective internal rate; moving residual/stability evidence is separate.");}
    if(flangerOnly){put(report,"flanger_only",true);put(report,"algorithm_provenance","Actual 4 mixed analog/digital layers, 16 notes, 64 active nonzero routes and 8 independent nonzero Flanger"+tapRevision+" stages; all active/retained banks and physical settings are captured");put(report,"flanger_algorithm_revision",int(workloadModulatedTapRevision));put(report,"lifecycle_note","Actual all 5 ordinary flanger controls per slot edited at the automation boundary, including base delay1..8ms and positive/negative feedback; temporary compressor replacement and missing-tempo Delay oracle are captured separately, then baseline recall and tempo restore return all 8 active families and exact retained settings to Flanger"+tapRevision+". Stop/resume retain Flanger"+tapRevision+"; active tails are a fixed 22s engineering observation separate from qualified complete-loop stationary support, without whole-tail quiescence or arbitrary time-varying stability proof.");put(report,"flanger_smoothing_history_policy","All5 owning physical controls ramp over5ms of valid internal frames including inactive/bypassed banks; shared phase advances only during active Flanger processing. Structural reactivation and direct Stop clear both channel histories/previous wet/phase0 and snap all5 targets; no continuous bypassed LFO claim.");put(report,"flanger_channel_tail_policy","Shared phase with independent channel histories; dual-mono "+tapRead+" fractional-delay processing without widening/crossfeed. D=B*(1+.75*depth*sin(phase)) spans.25..14ms; excitation=x-feedback*previousWet gives stationary complete wet loop P(z)/(1+f*z^-1*P(z)). Tail helper uses global admitted14ms/abs(float(.8)) at actual effective internal rate; fractional droop, moving residual and subjective quality remain separate evidence.");}
    if(wavetableUnisonOnly){put(report,"wavetable_unison_only",true);put(report,"algorithm_provenance","Actual4 revision2 wavetable layers/16notes/64 nonzero routes, every oscillator at seven unison voices (25 and 40 cents, width1, drift1), Classic filter types in layers1-2 and Vowel in3-4, with the full-wavetable-mixed effects. Drift and filter types cost what this build implements of them.");}
    if(midSideSplitOnly){put(report,"mid_side_split_only",true);put(report,"mid_side_split_algorithm_revision",1);put(report,"algorithm_provenance","Actual4 mixed layers/16notes/64 nonzero routes/eight independently nonneutral MidSideSplit1 master stages;1231 controls/15 retained banks. Orthonormal Mid/Side matrix with two fixed1k TPT branch tilt histories, gain/gate/color/mix; ordinary mixed-layer pan retains measurable Side.");put(report,"lifecycle_note","Exactly7 physical fields per slot/all56 genuinely changed values; temporary compressor/Delay followed by full inactive/active recall restoration.");put(report,"mid_side_split_tail_policy","Seven5ms current-before-increment actual-internal-rate ramps; cached changed pow settings boundary may run on callback outside sample loops, no allocation. Mix0/neutral dry bits after history clock; clear/reset/Stop snaps7/clears2states. Prepared complete admitted-control engineering support has positive fixed1k pole and 16P zero-input residual qualification; source/Higher42 support and22s window separate.");}
    if(frequencySplitOnly){const bool modernSplit=workloadFrequencySplitRevision==modernFrequencySplitAlgorithmRevision;const auto splitRevision=juce::String(int(workloadFrequencySplitRevision));
        put(report,"frequency_split_only",true);put(report,"frequency_split_algorithm_revision",int(workloadFrequencySplitRevision));put(report,"algorithm_provenance","Actual4 mixed layers/16notes/64 nonzero routes/eight independently nonneutral FrequencySplit"+splitRevision+" master stages;1175 controls/14 retained banks. "+juce::String(modernSplit?"Fourth-order Linkwitz-Riley TPT crossover whose bands sum to an allpass, the dry path, with independent fixed1k TPT branch tilt histories, gain/gate/color/mix.":"Complementary correct TPT crossover and independent fixed1k TPT branch tilt histories, gain/gate/color/mix."));put(report,"lifecycle_note","Exactly8 physical fields per slot/all64 genuinely changed values; temporary compressor/Delay followed by full inactive/active recall restoration.");put(report,"frequency_split_tail_policy","Eight5ms current-before-increment actual-internal-rate ramps; cached changed tan/pow settings boundary may run on callback outside sample loops, no allocation. "+juce::String(modernSplit?"Mix0/neutral return the allpass band sum; clear/reset/Stop snaps8/clears all section and tone states. Prepared complete admitted-control engineering support covers the squared20Hz/20kHz sections and tone cascade;":"Mix0/neutral dry bits after history clock; clear/reset/Stop snaps8/clears6states. Prepared complete admitted-control engineering support includes negative poles and tone cascade;")+" source/Higher42 support and22s window separate.");}
    if(convolutionOnly){put(report,"convolution_only",true);put(report,"convolution_algorithm_revision",1);put(report,"algorithm_provenance","Actual4 mixed layers/16notes/64 nonzero routes/eight independently varied Convolution1 master stages;1111 controls/13 retained banks, both canonical choices ready before callback. Fixed64 ascending-float mirrored-history immediate head with prepared one-tap Unit normal-finite shortcut/original exceptional-history fallback plus prepared NonUniform512 remainder in64-frame blocks/external64 prefix restoration in the samplewise adapter, TrimOFF NormaliseOFF canonical48000 SRC+rate gain.");put(report,"lifecycle_note","Exactly4 physical IR/predelay/wet-trim/mix fields per slot, all32 edits including prepared choice switches. Temporary compressor/Delay then recall/tempo restoration verifies exact active and inactive retained settings.");put(report,"convolution_tail_policy","Three5ms actual-internal-rate ramps for predelay seconds/trim amplitude/mix; activeMix0 exact dry while selected ready IR/predelay clocks; clear/reset/Stop snaps3/clears both engines. IR choice structural reset uses ready banks. Finite actual IR plus maximum predelay/ramp/interpolation support distinct from22s source observation and Higher42 history. No load/free/hash/SRC in callback; actual allocation proof separate.");}
    if(frequencyShiftOnly){put(report,"frequency_shift_only",true);put(report,"frequency_shift_algorithm_revision",1);put(report,"algorithm_provenance","Actual4 mixed layers/16notes/64 nonzero routes/eight independently varied FrequencyShift1 master stages;1079 controls/12 retained banks. Local exact-AGM fixed24 preparation, no vendor mutation.");put(report,"lifecycle_note","Exactly3 physical Hz/stereo-offset/mix fields per slot, all24 edits, signed motion and symmetric channel translations. Temporary compressor/Delay then recall/tempo restoration verifies exact active and inactive retained settings.");put(report,"frequency_shift_clock_tail_policy","Three physical5ms ramps; evenD/oddQ plus one internal-frame Q delay; simultaneous complex rotation and exact256-frame resync. Active Mix0 is exact dry with ongoing phase/history. clear/reset/Stop/reactivation snaps3 and clears histories/phase. ZeroHz retains common-D coloration; offset0 preserves mono; relative both-cascade support differs from22s engineering observation and Higher42 host-frame history.");}
    if(dimensionOnly){put(report,"dimension_only",true);put(report,"dimension_algorithm_revision",int(workloadModulatedTapRevision));put(report,"algorithm_provenance","Actual4 mixed analog/digital layers,16notes,64 active nonzero routes and8 independent Dimension"+tapRevision+" master stages; all1055 current controls and11 retained banks captured");put(report,"lifecycle_note","Exactly4 ordinary depth/rate/generated-side-width/mix fields per slot, all32 host edits. Temporary compressor and missing-tempo Delay are separate then baseline recall/tempo restore return all8 Dimension"+tapRevision+" families and exact retained settings. Stop/resume retain Dimension"+tapRevision+". Fixed22s source/downstream observation is separate from finite-history support, without late ringing or whole-patch quiescence claim.");put(report,"dimension_smoothing_history_policy","All4 physical targets advance over5ms of valid internal frames. Active Mix0 preserves phase/history; clear/reset/Stop/reactivation snaps targets and clears ring/phase. One prepared mono "+tapRead+" ring push and two reads per internal frame at positive12ms opposed4ms-depth excursion.");put(report,"dimension_channel_tail_policy","Generated side is added oppositely to original L/R, preserving original fold within declared float tolerance. Nonconstant mono can acquire side. Depth/width/mix0 take exact dry endpoints while active clocks/history advance. Stage support includes5ms ramp plus maximum16ms tap and2 interpolation frames at effective internal rate; prepared source/downstream and Higher42 host-frame FIR support remain separate.");}
    if(stereoOnly){put(report,"stereo_only",true);put(report,"algorithm_provenance","Actual4 mixed analog/digital layers,16notes,64 active nonzero routes and8 independent nonneutral Stereo1 master stages; current1023-key Sound11/Project10 controls and all active/retained banks are captured");put(report,"stereo_algorithm_revision",1);put(report,"lifecycle_note","Exactly4 ordinary stereo fields per slot/all32 host edits at the automation boundary. Temporary compressor and missing-tempo Delay paths are captured separately then baseline recall/tempo restore return all8 active Stereo1 families and exact retained settings. Stop/resume retain Stereo1. The22s source/downstream observation does not invent utility ringing or require late nonzero audio.");put(report,"stereo_smoothing_history_policy","Gain amplitude, pan, width and derived mode-side coefficient advance over5ms of valid internal frames, including inactive/bypassed banks. Mode-only edits preserve downstream history; reset/Stop/reactivation snap all4 targets. No audio history, phase or stage latency is added.");put(report,"stereo_channel_tail_policy","Unity-centre balance and width scaling of existing side; Stereo/Swap/Mono targets derive side coefficient+1/-1/0. Utility does not generate width from mono. Utility stage audio tail is exactly0seconds; prepared source/downstream tails and Higher42 host-frame FIR support remain separate.");}
    if(era){put(report,"era_reverb_only",true);auto recipe=object();for(const auto& [key,value]:eraRecipe)put(recipe,key,value);put(report,"era_reverb_recipe",recipe);
        put(report,"algorithm_provenance","Actual 4 mixed analog/digital layers, 16 notes, 64 active nonzero routes; the full-reverb workload with its eight master slots switched to Era Reverb at the Small Hours recipe. Every other control and retained bank is unchanged.");
        put(report,"lifecycle_note","Supplied-document lifecycle: the replacement event selects the next legacy family and the missing-tempo oracle selects delays, then recall and tempo restore return all eight slots to Era Reverb.");
        put(report,"stress_policy","96 kHz / 64 steady rows are stress observations, recorded with target_met but outside targets_met and success, as the existing policy keeps them.");}
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int repeat=1;repeat<=3;++repeat){
        for(const auto* kind:{"steady","lifecycle"}){auto trial=runTrial({era?"full-era-reverb":wavetableUnisonOnly?"full-wavetable-unison":midSideSplitOnly?"full-mid-side-split":frequencySplitOnly?"full-frequency-split":convolutionOnly?"full-convolution":frequencyShiftOnly?"full-frequency-shift":dimensionOnly?"full-dimension":stereoOnly?"full-stereo":flangerOnly?"full-flanger":phaserOnly?"full-phaser":chorusOnly?"full-chorus":filterOnly?"full-filter":eqOnly?"full-eq":reverbRevision?"full-reverb":"full-wavetable-mixed",kind,geometry.first,geometry.second,1,repeat},quality,era?0:reverbRevision,false,era.get());const bool accepted=acceptTrial(trial);bool target=accepted;for(const auto& s:*trial["callback_summaries"].getArray())target=target&&double(s["mean_callback_load"])<=.75&&double(s["p99_ns"])<=double(s["deadline_ns"]);put(trial,"target_met",target);integrity=integrity&&accepted;targets=targets&&target;writeJson(output.getChildFile("trial-"+juce::String(++number).paddedLeft('0',2)+".json"),trial);trials.append(trial);std::cout<<"QUALITY_PROFILE "<<kind<<' '<<geometry.first<<'/'<<geometry.second<<" repeat="<<repeat<<" target="<<target<<std::endl;}
        auto tail=era?qualityTailTrial(geometry.first,geometry.second,repeat,quality,1,false,false,false,false,false,false,false,false,false,false,false,false,era.get())
                     :qualityTailTrial(geometry.first,geometry.second,repeat,quality,reverbRevision?reverbRevision:1,eqOnly,filterOnly,chorusOnly,phaserOnly,flangerOnly,stereoOnly,dimensionOnly,frequencyShiftOnly,convolutionOnly,frequencySplitOnly,midSideSplitOnly,wavetableUnisonOnly);
        if(era){put(tail,"workload","full-era-reverb");put(tail,"tail_scope","Fixed 22s continuous Era Reverb tail observation after real releases; the plugin's declared tail is checked separately, with no whole-patch quiescence claim");}const auto& s=tail["summary"];const bool accepted=bool(tail["integrity"]);const bool target=accepted&&double(s["mean_callback_load"])<=.75&&double(s["p99_ns"])<=double(s["deadline_ns"]);put(tail,"target_met",target);integrity=integrity&&accepted;targets=targets&&target;writeJson(output.getChildFile("trial-"+juce::String(++number).paddedLeft('0',2)+".json"),tail);trials.append(tail);std::cout<<"QUALITY_PROFILE long-tail "<<geometry.first<<'/'<<geometry.second<<" repeat="<<repeat<<" target="<<target<<std::endl;
    }
    bool returnTargets=true;J returns(juce::Array<J>{});for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}}){auto returned=era?qualityReturnTrial(geometry.first,geometry.second,0,false,false,false,false,false,false,false,false,false,false,false,false,era.get()):qualityReturnTrial(geometry.first,geometry.second,reverbRevision,eqOnly,filterOnly,chorusOnly,phaserOnly,flangerOnly,stereoOnly,dimensionOnly,frequencyShiftOnly,convolutionOnly,frequencySplitOnly,midSideSplitOnly,wavetableUnisonOnly);integrity=integrity&&bool(returned["integrity"]);const auto& rs=returned["summary"];const bool target=double(rs["mean_callback_load"])<=.75&&double(rs["p99_ns"])<=double(rs["deadline_ns"]);returnTargets=returnTargets&&target;put(returned,"target_met",target);writeJson(output.getChildFile("return-live-"+juce::String(geometry.first)+".json"),returned);returns.append(returned);}put(report,"return_to_live",returns);
    if(era){J stress(juce::Array<J>{});
        for(int repeat=1;repeat<=3;++repeat){auto trial=runTrial({"full-era-reverb","steady",96000,64,1,repeat},quality,0,false,era.get());const bool accepted=acceptTrial(trial);bool target=accepted;
            for(const auto& s:*trial["callback_summaries"].getArray())target=target&&double(s["mean_callback_load"])<=.75&&double(s["p99_ns"])<=double(s["deadline_ns"]);
            put(trial,"target_met",target);put(trial,"stress",true);integrity=integrity&&accepted;writeJson(output.getChildFile("stress-"+juce::String(repeat)+".json"),trial);stress.append(trial);std::cout<<"QUALITY_PROFILE stress steady 96000/64 repeat="<<repeat<<" target="<<target<<std::endl;}
        put(report,"stress",stress);}
    put(report,"preview",era?qualityPreviewTrial(quality,0,false,false,false,false,false,false,false,false,false,false,false,false,era.get()):qualityPreviewTrial(quality,reverbRevision,eqOnly,filterOnly,chorusOnly,phaserOnly,flangerOnly,stereoOnly,dimensionOnly,frequencyShiftOnly,convolutionOnly,frequencySplitOnly,midSideSplitOnly,wavetableUnisonOnly));put(report,"process_memory",processMemory());
    const auto after=identities();const bool stable=juce::JSON::toString(before)==juce::JSON::toString(after);integrity=integrity&&stable;put(report,"identities_after",after);put(report,"identities_unchanged",stable);put(report,"trials",trials);put(report,"targets_met",targets);put(report,"integrity",integrity);const bool accepted=integrity&&returnTargets&&(quality==RenderQuality::higher||targets);put(report,"success",accepted);writeJson(output.getChildFile("sound-quality-profile-report.json"),report);std::cout<<(accepted?"SUCCESS":"UNRESOLVED")<<" sound-quality profile; all original outcomes retained\n";return accepted?0:1;
}
int eraProfileSelfTest()
{
    int checks=0,failures=0;const auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<name<<'\n';}};
    const auto rejects=[&](auto operation,const char* name){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}check(rejected,name);};
    const auto fresh=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/era-profile-self-test-"+juce::Uuid().toString()).getFullPathName();
    try{const auto parsed=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--era-reverb"});check(parsed.eraReverb&&parsed.quality==RenderQuality::realtime,"Era Reverb profile selection admitted");}
    catch(const std::exception&){check(false,"Era Reverb profile selection admitted");}
    try{const auto parsed=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--quality","higher","--era-reverb"});check(parsed.eraReverb&&parsed.quality==RenderQuality::higher,"Era Reverb selection and higher mode are independent");}
    catch(const std::exception&){check(false,"Era Reverb selection and higher mode are independent");}
    check(!parseArguments({"--profile","--suite","sound-quality","--output",fresh}).eraReverb,"Era Reverb workload is off by default");
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","sound-quality","--output",fresh,"--era-reverb","--era-reverb"},
        {"--profile","--suite","sound-quality","--output",fresh,"--era-reverb","--stereo-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--mid-side-split-only","--era-reverb"},
        {"--profile","--suite","sound-quality","--output",fresh,"--era-reverb","--reverb-algorithm","2"},
        {"--profile","--suite","smoke","--output",fresh,"--era-reverb"},
        {"--self-test","--era-reverb"}})rejects([&]{parseArguments(args);},"duplicate/mixed/wrong-suite Era Reverb selector rejects");
    const auto era=std::make_unique<SoundDocument>(eraWorkload()),base=std::make_unique<SoundDocument>(workload("full-reverb"));auto patch=std::make_unique<RoutedPatch>();
    check(makeRoutedPatch(*era,*patch)&&int(documentJson(*era)["active_nonzero_routes"])==64,"Era workload patch keeps 64 active nonzero routes");
    bool placed=true;
    for(std::size_t slot=0;slot<8;++slot){const auto& e=patch->effects[slot].eraReverb;
        placed&=patch->layout.effects[slot].use==SlotUse::present&&patch->layout.effects[slot].kind==EffectKind::eraReverb&&patch->layout.effects[slot].chain==masterChain&&!patch->effects[slot].bypassed
            &&e.voicing==EraVoicing::space&&e.age==EraAge::dark&&std::abs(e.decaySeconds-8)<1e-3f&&std::abs(e.grit-.15f)<1e-5f&&std::abs(e.wobble-.4f)<1e-5f&&std::abs(e.mix-.3f)<1e-5f&&std::abs(e.highCutHz-9000)<.5f;}
    check(placed,"Era workload has Era Reverb at the Small Hours recipe in all eight master slots");
    bool unchanged=era->compatibility.synth==base->compatibility.synth&&era->compatibility.effects==base->compatibility.effects;
    for(std::size_t i=0;i<eraReverbParameterStart;++i)unchanged&=era->values[i]==base->values[i];
    auto layout=era->layout;for(auto& fx:layout.effects)if(fx.kind==EffectKind::eraReverb)fx.kind=EffectKind::reverb;unchanged&=layout==base->layout;
    check(unchanged,"Era workload leaves every other control, retained bank and route of full-reverb unchanged");
    std::cout<<"Era Reverb profile self-test "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
}
namespace {
struct DrumCapacityPoint {const char* name;int rate,block;Ns warm,steady,lifecycle;};
constexpr std::array<DrumCapacityPoint,3> drumCapacityPoints{{
    {"44100/128",44100,128,11136,44160,88320},
    {"48000/128",48000,128,12032,48000,96000},
    {"96000/512",96000,512,24064,96256,192000}}};
constexpr std::array<const char*,3> drumCapacityPhases{"stereo-steady","mixed-steady","mixed-lifecycle"};
J drumCapacityDescriptor(const DrumCapacityPoint& point,RenderQuality quality) {
    auto row=object();put(row,"point",point.name);put(row,"sample_rate",point.rate);put(row,"block_size",point.block);
    put(row,"quality",int(quality));put(row,"physical_factor",quality==RenderQuality::higher?2:1);
    put(row,"warmup_frames",integer(point.warm));put(row,"steady_frames",integer(point.steady));put(row,"lifecycle_frames",integer(point.lifecycle));
    put(row,"repeats",3);put(row,"windows_per_repeat",3);put(row,"canonical_frames",480000);put(row,"source_rate",48000);
    put(row,"stereo_tune",quality==RenderQuality::higher&&point.rate==96000?23.99f:24.f);
    put(row,"steady_captured_per_bank",32);put(row,"required_workers",3);put(row,"space_slot",6);
    put(row,"mean_period_limit",.75);put(row,"p99_period_limit",1.);put(row,"higher_offline_descriptive",quality==RenderQuality::higher);
    return row;
}
J drumCaptureObservation(const InstrumentProcessor& p) {
    const auto report=InstrumentDrumTestAccess::references(p);auto row=object();J captured(juce::Array<J>{}),configured(juce::Array<J>{}),pads(juce::Array<J>{}),ids(juce::Array<J>{});
    for(std::size_t bank=0;bank<4;++bank){int c=0,a=0;std::array<int,16> padCounts{};
        for(std::size_t n=0;n<16;++n){const auto* asset=report.references[bank*48+n];a+=asset!=nullptr;ids.append(asset?juce::String(drums::assetIdText(asset->id())):juce::String());}
        for(std::size_t n=0;n<32;++n)if(const auto* asset=report.references[bank*48+16+n]){++c;bool found=false;
            for(std::size_t pad=0;pad<16;++pad)if(asset==report.references[bank*48+pad]){++padCounts[pad];found=true;break;}
            require(found,"captured drum asset outside unchanged configured fixture");}
        J counts(juce::Array<J>{});for(const auto value:padCounts)counts.append(value);pads.append(counts);captured.append(c);configured.append(a);}
    put(row,"complete",report.complete);put(row,"reference_capacity",int(report.references.size()));put(row,"adopted_generation",integer(Ns(report.adoptedGeneration)));
    put(row,"captured_per_bank",captured);put(row,"configured_per_bank",configured);put(row,"captured_pad_counts",pads);put(row,"configured_asset_ids",ids);return row;
}
juce::Uuid drumFixtureId(unsigned bank,bool design) {
    return juce::Uuid(juce::String(design?"dca00000-0000-4000-8000-00000000000":"dcb00000-0000-4000-8000-00000000000")+juce::String(bank+1));
}
void drumPcm(std::vector<float>& scratch,unsigned bank,unsigned pad,unsigned channels,unsigned frames) {
    scratch.resize(std::size_t(frames)*channels);
    std::uint32_t state=0x9e3779b9u^((bank*16+pad+1)*0x85ebca6bu)^(channels<<24);
    for(auto& sample:scratch){state^=state<<13;state^=state>>17;state^=state<<5;sample=(float(std::int32_t(state>>8)-8388608)/8388608.f)*.0001f;}
}
std::array<drums::PadSettings,16> drumFixturePads(const DrumCapacityPoint& point,RenderQuality quality,const juce::String& phase) {
    std::array<drums::PadSettings,16> pads{};const bool stereo=phase==drumCapacityPhases[0];
    constexpr std::array<float,4> tunes{-24,-7.25f,0,24};
    for(unsigned pad=0;pad<16;++pad){auto& p=pads[pad];p.gainDb=-12;p.pan=pad%2?.25f:-.25f;
        p.tuneSemitones=stereo?(quality==RenderQuality::higher&&point.rate==96000?23.99f:24.f):tunes[pad%4];
        p.trimStart=stereo?.0625f:.125f;p.trimEnd=stereo?.9375f:.875f;p.attackSeconds=.001f;p.decaySeconds=10;p.midiNote=36+int(pad);}
    return pads;
}
constexpr std::array<const char*,9> drumPadFields{"gain_db","pan","tune_semitones","trim_start","trim_end","attack_seconds","decay_seconds","midi_note","choke_group"};
std::array<float,9> drumPadValues(const drums::PadSettings& p) {return {p.gainDb,p.pan,p.tuneSemitones,p.trimStart,p.trimEnd,p.attackSeconds,p.decaySeconds,float(p.midiNote),float(p.chokeGroup)};}
SoundDocument drumFixtureSound(const DrumCapacityPoint& point,RenderQuality quality,const juce::String& phase) {
    auto d=step7Stimulus("costly-mixed-forward");d.name="Drum playback capacity";set(d,"global.master_level",1);set(d,"output.gain_db",0);set(d,"output.protection",1);
    const auto pads=drumFixturePads(point,quality,phase);
    for(unsigned bank=0;bank<4;++bank){const auto prefix="synth."+juce::String(bank+1)+".";
        set(d,prefix+"enabled",1);set(d,prefix+"level",1);set(d,prefix+"pan",0);set(d,prefix+"mute",0);set(d,prefix+"solo",0);set(d,prefix+"drum_mode",1);
        for(unsigned pad=0;pad<16;++pad){const auto values=drumPadValues(pads[pad]);for(unsigned field=0;field<9;++field)set(d,prefix+"drum.pad."+juce::String(pad+1)+"."+drumPadFields[field],values[field]);}}
    d.layout.effects[6].kind=EffectKind::spaceReverb;
    for(unsigned slot=0;slot<8;++slot)set(d,"effect."+juce::String(slot+1)+".bypassed",0);
    constexpr std::pair<const char*,float> spaceValues[]{{"size",.5f},{"decay_seconds",2.f},{"predelay_ms",20.f},{"brightness",.5f},{"character",.25f},{"distance",.5f},{"width",1.f},{"mix",.25f},{"duck_range_db",0.f},{"duck_threshold_db",-24.f},{"duck_attack_ms",10.f},{"duck_release_ms",250.f}};
    for(const auto& [field,value]:spaceValues)set(d,"effect.7.space_reverb."+juce::String(field),value);
    for(unsigned band=1;band<=6;++band)for(const auto* family:{"decay.","post."})for(const auto* field:{"active","bypassed"})set(d,"effect.7.space_reverb."+juce::String(family)+juce::String(band)+"."+field,0);
    d.values[758]=quality==RenderQuality::higher?1.f:0.f;return d;
}
J drumSourceSettings(const SoundDocument& sound,unsigned frames,const juce::String& phase) {
    J rows(juce::Array<J>{});for(unsigned bank=0;bank<4;++bank)for(unsigned pad=0;pad<16;++pad){auto row=object();
        put(row,"bank",int(bank));put(row,"pad",int(pad));put(row,"frames",int(frames));put(row,"channels",phase==drumCapacityPhases[0]||pad>=8?2:1);
        put(row,"rack_instance",drumFixtureId(bank,false).toString());put(row,"rack_design",drumFixtureId(bank,true).toString());put(row,"unit",0);put(row,"algorithm",1);
        put(row,"name","capacity-b"+juce::String(bank)+"-p"+juce::String(pad));put(row,"source","");
        for(const auto* field:drumPadFields)put(row,field,double(parameterPhysicalValue(sound.values,index("synth."+juce::String(bank+1)+".drum.pad."+juce::String(pad+1)+"."+field))));
        rows.append(row);}return rows;
}
J drumSpaceJson(const RoutedSynthEngine::SpaceRenderStats& stats) {
    auto row=object();put(row,"ready",stats.ready);put(row,"mode_count",int(stats.modeCount));put(row,"physical_factor",int(stats.physicalFactor));put(row,"active_mask",int(stats.activeMask));
    put(row,"converter_backend",stats.converterBackend?J(int(*stats.converterBackend)):J());J counters(juce::Array<J>{});for(const auto frames:stats.renderedPhysicalFrames)counters.append(integer(Ns(frames)));put(row,"rendered_physical_frames",counters);return row;
}
J drumWorkersJson(const RoutedSynthEngine::DspWorkerStats& stats) {auto row=object();put(row,"prepared",int(stats.prepared));put(row,"blocks",integer(Ns(stats.blocks)));put(row,"internal_frames",integer(Ns(stats.internalFrames)));return row;}
void validateDrumSpace(const J& row,int factor) {
    require(row.isObject()&&bool(row["ready"])&&int(row["mode_count"])==128&&int(row["physical_factor"])==factor&&int(row["active_mask"])==(1<<6)&&!row["converter_backend"].isVoid()&&row["rendered_physical_frames"].size()==8,"Space must be real ready128/factor/slot6/backend");
    for(int slot=0;slot<8;++slot)require((row["rendered_physical_frames"][slot].isInt64()||row["rendered_physical_frames"][slot].isInt())&&juce::int64(row["rendered_physical_frames"][slot])>=0,"invalid Space frame counter");
}
void validateDrumSpaceAdvance(const J& before,const J& after,int frames,int factor) {
    validateDrumSpace(before,factor);validateDrumSpace(after,factor);require(before["converter_backend"]==after["converter_backend"],"Space backend changed inside window");
    for(int slot=0;slot<8;++slot)require(juce::int64(after["rendered_physical_frames"][slot])-juce::int64(before["rendered_physical_frames"][slot])==(slot==6?Ns(frames)*factor:0),"Space physical work missing or unexpected slot advanced");
}
std::vector<Event> drumCapacityEvents(int rate,int block,bool reduced) {
    const std::array<int,10> sixteenths{2,8,10,12,16,17,18,24,26,30};
    const std::array<const char*,10> labels{"matching-note-offs","choke1-trigger14","trigger15","trigger14","cc123-channel1","restore-choke0","refill-channel1","cc120-channel2","refill-channel2","raw-midi-stop"};
    std::vector<Event> result;for(unsigned n=0;n<labels.size();++n){const Ns requested=(Ns(reduced?512:rate)*sixteenths[n]+15)/16;const bool control=n==1||n==5;
        result.push_back({labels[n],requested,control?((requested+block-1)/block)*block:requested,control});}return result;
}
void drumMidi(juce::MidiBuffer& midi,const juce::MidiMessage& message,int offset,J& trace) {
    midi.addEvent(message,offset);auto row=object();put(row,"sample_offset",offset);put(row,"bytes_base64",juce::Base64::toBase64(message.getRawData(),std::size_t(message.getRawDataSize())));trace.append(row);
}
void drumRound(juce::MidiBuffer& midi,int channel,bool on,int offset,J& trace) {
    for(int pad=0;pad<16;++pad)drumMidi(midi,on?juce::MidiMessage::noteOn(channel,36+pad,juce::uint8(channel==1?100:80)):juce::MidiMessage::noteOff(channel,36+pad),offset,trace);
}
void drumEventMidi(unsigned event,juce::MidiBuffer& midi,int offset,J& trace) {
    if(event==0){drumRound(midi,1,false,offset,trace);drumRound(midi,2,false,offset,trace);}
    else if(event==1||event==2||event==3)drumMidi(midi,juce::MidiMessage::noteOn(1,event==2?51:50,juce::uint8(100)),offset,trace);
    else if(event==4||event==7)drumMidi(midi,juce::MidiMessage::controllerEvent(event==4?1:2,event==4?123:120,0),offset,trace);
    else if(event==6||event==8)drumRound(midi,event==6?1:2,true,offset,trace);
    else if(event==9)drumMidi(midi,juce::MidiMessage::midiStop(),offset,trace);
}
// Independent lifetime/rank/choke oracle; no production hit or reader helper is used.
struct DrumCapacityModel {
    struct Hit {bool active=false;int pad=0,channel=0,group=0;Ns age=0,support=0,release=0;std::uint64_t order=0;};
    std::array<Hit,32> hits{};std::array<drums::PadSettings,16> pads{};int factor=1,rate=48000,frames=4096;std::uint64_t order=0;
    DrumCapacityModel(const J& settings,int hostRate,int physicalFactor,int sourceFrames):factor(physicalFactor),rate(hostRate*physicalFactor),frames(sourceFrames) {
        for(int pad=0;pad<16;++pad){auto& p=pads[std::size_t(pad)];const auto row=settings[pad];p.tuneSemitones=float(double(row["tune_semitones"]));p.trimStart=float(double(row["trim_start"]));p.trimEnd=float(double(row["trim_end"]));p.attackSeconds=float(double(row["attack_seconds"]));p.decaySeconds=float(double(row["decay_seconds"]));}
    }
    void choke(int group){pads[14].chokeGroup=pads[15].chokeGroup=group;}
    void event(const juce::MidiMessageMetadata& midi){const auto* data=midi.data;if(midi.numBytes==1&&data[0]==0xfc){hits={};return;}if(midi.numBytes!=3)return;
        const int status=data[0]&0xf0,channel=(data[0]&15)+1;
        if(status==0xb0&&(data[1]==120||data[1]==123)){for(auto& h:hits)if(h.active&&h.channel==channel)h={};return;}
        if(status!=0x90||data[2]==0||data[1]<36||data[1]>51)return;const int pad=data[1]-36;const auto& p=pads[std::size_t(pad)];
        if(p.chokeGroup)for(auto& h:hits)if(h.active&&h.group==p.chokeGroup&&h.release==0)h.release=Ns(std::ceil(.005*rate));
        auto slot=std::find_if(hits.begin(),hits.end(),[](const Hit& h){return !h.active;});if(slot==hits.end())slot=std::min_element(hits.begin(),hits.end(),[](const Hit& a,const Hit& b){return a.order<b.order;});
        const auto begin=std::min(Ns(frames-1),Ns(std::floor(double(p.trimStart)*frames))),end=std::clamp(Ns(std::ceil(double(p.trimEnd)*frames)),begin+1,Ns(frames));
        Ns support=Ns(std::ceil(double(end-begin)*rate/(48000*std::exp2(double(p.tuneSemitones)/12))));
        if(p.decaySeconds>0)support=std::min(support,Ns(std::ceil(double(p.attackSeconds)*rate))+Ns(std::ceil(double(p.decaySeconds)*rate)));
        *slot={true,pad,channel,p.chokeGroup,0,support,0,++order};
    }
    void advance(const juce::MidiBuffer& midi,int hostFrames){auto next=midi.cbegin();const auto end=midi.cend();
        for(int frame=0;frame<hostFrames*factor;++frame){while(next!=end&&(*next).samplePosition*factor<=frame){event(*next);++next;}
            for(auto& h:hits)if(h.active){++h.age;if(h.age>=h.support||(h.release>0&&--h.release==0))h={};}}
    }
    std::array<int,16> counts() const {std::array<int,16> counts{};for(const auto& hit:hits)if(hit.active)++counts[std::size_t(hit.pad)];return counts;}
};
struct DrumCapacityHead final:juce::AudioPlayHead {
    Ns frame=0;int rate=48000;
    juce::Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setIsPlaying(true);p.setIsLooping(false);p.setTimeInSamples(frame);p.setPpqPosition(double(frame)*2/rate);p.setBpm(120);p.setTimeSignature(TimeSignature{4,4});return p;}
};
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
// Callback state is finite and independently owned by RuntimeServer's callable.
// Retained backend work holds this shared owner after fixture/server/IO destroy.
struct DrumLocalCallbackState final {
    static constexpr const char* model="drum-local-profile-fixture";
    std::atomic_uint accountCalls{0},generationCalls{0},providerCalls{0},evaluationCalls{0};
    std::mutex mutex;J records{juce::Array<J>{}};
    companion::Frame run(const companion::RuntimeJob& job){using namespace ai;using namespace companion;
        if(std::holds_alternative<AccountCommand>(job.input)){
            require(++accountCalls<=2,"bounded local account fixture");const auto action=std::get<AccountCommand>(job.input).action;
            require(action==AccountAction::status||action==AccountAction::models,"local fixture cannot mutate accounts");
            AccountReply reply;reply.providerStatus=rack::oauth::Status::connected;reply.httpStatus=200;reply.modelVerified=true;
            reply.selectedModel=model;reply.models={{model,"Isolated structured fixture; no inference network"}};
            Frame frame{MessageKind::accountResult,job.session,{}};require(encodeAccountReply(reply,frame.payload),"local account codec");return frame;
        }
        require(++generationCalls==1,"one bounded local fixture generation");require(job.localPreview&&job.previewExchange,"public Generate did not use owned IO exchange");
        auto input=std::make_unique<LocalGenerationRequest>();input->request=std::get<GenerationRequest>(job.input);input->local=*job.localPreview;
        require(!input->request.catalog&&!job.groupContext&&input->local.sourceMap.assets.size()==64,"local profile root geometry");
        juce::MemoryBlock encoded;require(encodeLocalRequest(*input,encoded),"local request codec");
        auto record=object();put(record,"request_sha256",juce::SHA256(encoded).toHexString());put(record,"request_base64",juce::Base64::toBase64(encoded.getData(),encoded.getSize()));
        unsigned attempt=0;std::uint8_t ordinal=0;
        auto result=std::unique_ptr<LocalGenerationResult>(new LocalGenerationResult(generateLocalProposal(*input,
            [&](const auto&,auto,const auto&){++providerCalls;++attempt;
                // Four bars at120BPM:384000 body+192000 tail. Exactly the original
                // sixteen36..51 notes on each channel,100/80 velocities, one onset.
                auto reply=juce::JSON::parse(R"({"version":1,"parameter_edits":[],"rack_edits":[],"macro_edits":[],"pattern":{"bars":4,"meter_numerator":4,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[]},"characteristics":"Capacity fixture","explanation":"Structured response only; real local IO and rendering"})");
                auto* notes=reply["pattern"]["notes"].getArray();
                for(int channel=1;channel<=2;++channel)for(int pad=0;pad<16;++pad){auto note=object();put(note,"start_beats",0);put(note,"duration_beats",.25);put(note,"pitch",36+pad);put(note,"velocity",channel==1?100:80);put(note,"channel",channel);notes->add(note);}
                rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::connected;response.result.httpStatus=200;response.text=juce::JSON::toString(reply,true,22);return response;},
            [&](const auto& candidate,const auto& edits,const auto& context,const auto& trace,const auto& cancelled){++evaluationCalls;
                auto value=std::make_unique<PreviewEvaluate>();value->edits=edits;value->context=context;auto& b=value->binding;b.request=input->request.tag;b.root=input->local.root;
                b.catalogSnapshot=input->local.catalogSnapshot;b.job=job.jobId;b.structuralGeneration=input->request.structuralGeneration;b.reset=input->local.reset;b.sourceWord=input->local.sourceWord;
                b.providerAttempt=std::uint8_t(attempt);b.renderOrdinal=++ordinal;b.sourceMap=input->local.sourceMapDigest;PreviewSampleMap staged;
                require(stagePreviewMap(input->local.sourceMap,trace,staged)&&digestPreviewMap(staged,b.stagedMap)&&digestPreviewCandidate(candidate,b.candidate)&&digestPreviewContext(context,b.context),"local candidate binding");
                return job.previewExchange->evaluate(*value,cancelled);
            },[&]{return job.cancellation&&job.cancellation->cancelled();})));
        put(record,"status",int(result->numeric.status));put(record,"render_status",int(result->numeric.renderStatus));put(record,"inference_calls",int(result->numeric.inferenceCalls));
        put(record,"terminal_pcm_frames",integer(Ns(result->numeric.audio.left.size()+result->numeric.audio.right.size())));
        Frame frame{MessageKind::resultLocal,job.session,{}};require(encodeLocalResult(*input,*result,frame.payload),"local terminal codec");
        put(record,"terminal_sha256",juce::SHA256(frame.payload).toHexString());put(record,"terminal_bytes",integer(Ns(frame.payload.getSize())));
        {const std::lock_guard lock(mutex);records.append(record);}return frame;
    }
};
// One canonical binding for the workload and its isolated lifetime proof.
struct DrumLocalRetentionBarrier final {
    std::mutex mutex;std::condition_variable condition;bool released=false;
    std::atomic_bool entered{false},completed{false},peerAuthenticated{false},statusJob{false},priorityGood{false};
    std::atomic_bool fixtureGone{false},fixtureGoneWhenCalled{false},accountReplyValid{false};
    void wait(const companion::RuntimeJob& job){
        FILETIME created{},exited{},kernel{},user{};
        const bool observed=GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=0;
        const auto incarnation=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
        peerAuthenticated=observed&&job.peer.processId==GetCurrentProcessId()&&incarnation!=0&&job.peer.creationTime==incarnation;
        const auto* account=std::get_if<companion::AccountCommand>(&job.input);
        statusJob=account&&account->action==companion::AccountAction::status;
        priorityGood=GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_BELOW_NORMAL;
        std::unique_lock lock(mutex);entered.store(true,std::memory_order_release);
        condition.wait(lock,[&]{return released;});
    }
    void release(){ {const std::lock_guard lock(mutex);released=true;}condition.notify_all(); }
};
companion::RuntimeRunJob bindDrumLocalCallback(std::shared_ptr<DrumLocalCallbackState> owned,
    std::shared_ptr<DrumLocalRetentionBarrier> barrier={}){
    companion::RuntimeRunJob actual=[owned=std::move(owned)](const companion::RuntimeJob& job){return owned->run(job);};
    if(!barrier)return actual; // Normal workload has exactly this actual callable, no barrier owner/check.
    return [actual=std::move(actual),barrier=std::move(barrier)](const companion::RuntimeJob& job){
        barrier->wait(job);barrier->fixtureGoneWhenCalled=barrier->fixtureGone.load();
        struct Finished {DrumLocalRetentionBarrier& value;~Finished(){value.completed.store(true,std::memory_order_release);}} finished{*barrier};
        auto frame=actual(job);companion::AccountReply reply;
        const companion::WireBytes bytes{static_cast<const std::byte*>(frame.payload.getData()),frame.payload.getSize()};
        barrier->accountReplyValid=frame.kind==companion::MessageKind::accountResult&&frame.session==job.session
            &&companion::decodeAccountReply(bytes,reply)&&reply.status==companion::RuntimeStatus::ready
            &&reply.providerStatus==rack::oauth::Status::connected&&reply.httpStatus==200
            &&reply.selectedModel==DrumLocalCallbackState::model&&reply.models.size()==1;
        return frame;
    };
}
struct DrumLocalProfile final {
    static constexpr const char* model=DrumLocalCallbackState::model;
    std::shared_ptr<DrumLocalCallbackState> callback=std::make_shared<DrumLocalCallbackState>();
    std::atomic_bool stop{false},retained{false};std::atomic_int serverStatus{-1};
    const CpuTime cpuBefore=cpuTime();
    juce::String suffix="drum-prof-"+juce::Uuid().toString().removeCharacters("-").substring(0,20);
    companion::RuntimeServer server;std::thread io;
    Ns coordinationStart=0,coordinationEnd=0;bool coordinated=false;
    static Ns clockNs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
    explicit DrumLocalProfile(std::shared_ptr<DrumLocalRetentionBarrier> barrier={}):server(bindDrumLocalCallback(callback,std::move(barrier)),[](const auto& job){if(job.previewExchange)job.previewExchange->cancel();}),
        io([this]{const auto result=server.run(stop,suffix);retained=result.workerRetained;serverStatus=int(result.status);}){}
    ~DrumLocalProfile(){stop=true;if(io.joinable())io.join();}
    companion::ClientOptions options()const {companion::ClientOptions result;result.allowLaunch=false;result.negotiateSpectrum=false;
        result.launch.pipeSuffix=suffix;result.launch.fixtureRoot=juce::File::getSpecialLocation(juce::File::tempDirectory);return result;}
    static void pump(InstrumentProcessor& p){
        // Nonblocking ordinary message dispatch: actual 25ms control pump owns
        // completion delivery. No importer.poll, owner resolver or synthetic ACK.
#if JUCE_WINDOWS
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
#endif
        p.pollAi();
    }
    void connect(InstrumentProcessor& p){
        require(p.setAiMusicalContext({"drums","D","minor",120}),"local fixture context");
        require(p.requestAiAccount({}),"local fixture public account submission");
        const auto deadline=Clock::now()+std::chrono::seconds(4);
        while(Clock::now()<deadline){pump(p);const auto* account=p.aiAccountReply();
            if(account&&account->selectedModel==model&&!p.aiWorkflowStatus().accountPending&&!account->models.empty())return;
            std::this_thread::yield();}
        require(false,"local fixture account setup incomplete");
    }
    void generate(InstrumentProcessor& p){
        InstrumentDrumTestAccess::resetLocalObservations(p);
        AiRequestOptions request{"Use the original two-channel32-note capacity phrase",model,{ai::RequestScope::pattern,false,false,false}};
        require(p.generateAi(request),"local profile public Generate refused");
        coordinationStart=clockNs();const auto deadline=Clock::now()+std::chrono::milliseconds(companion::runtimeWorkerStartupMs);
        while(Clock::now()<deadline&&!InstrumentDrumTestAccess::firstLocalCallCompleted(p)){pump(p);std::this_thread::yield();}
        coordinated=Clock::now()<=deadline&&InstrumentDrumTestAccess::firstLocalCallCompleted(p);coordinationEnd=clockNs();
        // No preview pause and no host callback until now. Always retain the
        // original measured callbacks even when this bounded coordination misses.
    }
    J finish(InstrumentProcessor& p){
        // Original live frame extent is over. This grace adds no callback samples
        // or overlap and never restarts Generate. Normal lifecycle edits may have
        // already staled/cancelled the request. Preserve that actual outcome.
        const auto deadline=Clock::now()+std::chrono::milliseconds(companion::runtimeWorkerDrainMs);
        while(Clock::now()<deadline){pump(p);const auto count=InstrumentDrumTestAccess::localObservationCount(p);
            if(count&&p.aiProposalPhase()!=AiProposalPhase::pending)break;std::this_thread::yield();}
        auto report=object();J observations(juce::Array<J>{});
        const bool overflow=InstrumentDrumTestAccess::visitLocalObservations(p,[&](const auto& o){const auto& d=o.work;auto row=object();
            put(row,"request_id",integer(Ns(o.requestId)));put(row,"job",integer(Ns(o.job)));put(row,"ticket",integer(Ns(o.ticket)));put(row,"ordinal",int(o.ordinal));put(row,"status",int(o.status));
            put(row,"start_ns",integer(d.startNs));put(row,"end_ns",integer(d.endNs));put(row,"loop_lifetime_start_ns",integer(d.loopLifetimeStartNs));put(row,"loop_lifetime_end_ns",integer(d.loopLifetimeEndNs));
            put(row,"prepared",d.prepared);put(row,"prepared_workers",int(d.preparedWorkers));put(row,"worker_blocks",integer(Ns(d.workerBlocks)));put(row,"worker_frames",integer(Ns(d.workerFrames)));
            put(row,"rendered_blocks",integer(Ns(d.renderedBlocks)));put(row,"rendered_frames",integer(Ns(d.renderedFrames)));put(row,"space_ready",d.spaceReady);put(row,"space_modes",int(d.spaceModes));
            put(row,"space_mask",int(d.spaceMask));put(row,"physical_factor",int(d.physicalFactor));put(row,"space_frames",integer(Ns(d.spaceFrames)));put(row,"effective_quality",int(d.effectiveQuality));put(row,"latency_samples",d.latencySamples);
            J captured(juce::Array<J>{});for(auto value:d.firstCaptured)captured.append(int(value));put(row,"first_captured_per_bank",captured);put(row,"references_complete",d.referencesComplete);
            if(o.callTrace){const auto& trace=*o.callTrace;J calls(juce::Array<J>{});
                for(unsigned n=0;n<trace.count&&n<trace.calls.size();++n){auto call=object();put(call,"start_ns",integer(trace.calls[n].startNs));put(call,"end_ns",integer(trace.calls[n].endNs));calls.append(call);}
                put(row,"render_calls",calls);put(row,"trace_count",int(trace.count));put(row,"trace_capacity",int(trace.calls.size()));put(row,"trace_overflow",trace.overflow);put(row,"trace_charge_bytes",integer(Ns(sizeof(ai::PreviewCallTrace))));}
            observations.append(row);});
        put(report,"observations",observations);put(report,"observation_overflow",overflow);put(report,"proposal_phase",int(p.aiProposalPhase()));
        const auto* proposal=p.aiProposal();put(report,"public_ready",proposal&&p.aiProposalPhase()==AiProposalPhase::proposed&&proposal->status==ai::GenerationStatus::ready);
        put(report,"public_body_frames",proposal?integer(Ns(proposal->audio.bodyFrames)):J());put(report,"public_frames",proposal?integer(Ns(proposal->audio.left.size())):J());
        p.closeAiSession();stop=true;if(io.joinable())io.join();
        put(report,"generation_calls",int(callback->generationCalls));put(report,"provider_calls",int(callback->providerCalls));put(report,"evaluation_calls",int(callback->evaluationCalls));put(report,"account_calls",int(callback->accountCalls));
        put(report,"server_status",serverStatus.load());put(report,"worker_retained",bool(retained));put(report,"fixture_label","Synthetic discovered account/model and structured response only; real typed IO, local engine, no provider/HTTP network");
        const auto cpuAfter=cpuTime();if(cpuBefore.available&&cpuAfter.available){auto cpu=object();put(cpu,"user_ns",integer(Ns((cpuAfter.user-cpuBefore.user)*100)));put(cpu,"kernel_ns",integer(Ns((cpuAfter.kernel-cpuBefore.kernel)*100)));put(cpu,"scope","tool process/all threads; local fixture construction, unchanged setup/warm/live frames, observations and untimed drain; not isolated render CPU");put(report,"process_cpu",cpu);}
        {const std::lock_guard lock(callback->mutex);put(report,"terminals",juce::JSON::parse(juce::JSON::toString(callback->records)));} // independent snapshot even if retained backend resumes
        put(report,"coordination_start_ns",integer(coordinationStart));put(report,"coordination_end_ns",integer(coordinationEnd));put(report,"coordination_bound_ms",integer(companion::runtimeWorkerStartupMs));put(report,"first_real_call_completed",coordinated);
        const auto sizes=InstrumentDrumTestAccess::compiledLocalSizes();J sizeRows(juce::Array<J>{});for(auto value:sizes)sizeRows.append(integer(Ns(value)));put(report,"compiled_sizes_processor_work_result_trace_observation",sizeRows);
        static_assert(sizeof(InstrumentProcessor)<=128*1024,"existing processor128KiB footprint limit");return report;
    }
};
// Opt-in only: the normal profiler self-test and all timing dispatch stay unchanged.
int drumLocalCallbackRetentionChild(){
    using Access=companion::detail::RuntimeServerTestAccess;
    std::cout<<"DRUM_LOCAL_RETENTION phase=entry drain_ms="<<companion::runtimeWorkerDrainMs<<std::endl;
    require(Access::retainedWorkers()==0&&Access::admissionAvailable(),"local retention needs a fresh isolated process");
    auto barrier=std::make_shared<DrumLocalRetentionBarrier>();std::unique_ptr<DrumLocalProfile> fixture;
    struct Cleanup {
        std::shared_ptr<DrumLocalRetentionBarrier> barrier;std::unique_ptr<DrumLocalProfile>& fixture;
        ~Cleanup(){
            barrier->release(); // Always release before any IO join, including setup/exception failure.
            if(fixture){fixture->stop=true;if(fixture->io.joinable())fixture->io.join();fixture.reset();}
            const auto deadline=Clock::now()+std::chrono::milliseconds(3000);
            while(Access::retainedWorkers()&&Clock::now()<deadline){Access::reapRetained();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        }
    } cleanup{barrier,fixture};
    fixture=std::make_unique<DrumLocalProfile>(barrier);
    auto callerOwned=fixture->callback;std::weak_ptr<DrumLocalCallbackState> lifetime=callerOwned;
    companion::CompanionClient client;
    const auto wait=[](auto&& predicate,const Clock::time_point deadline){
        do{if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<deadline);return predicate();};
    // One shared startup bound, not two successive5s allowances.
    const auto setupDeadline=Clock::now()+std::chrono::milliseconds(companion::runtimeWorkerStartupMs);
    require(client.start(fixture->options()),"local retention client startup refused");
    require(wait([&]{client.poll();return client.status().phase==companion::ClientPhase::ready;},setupDeadline)
        &&!client.session().isNull(),"local retention authenticated hello incomplete");
    require(client.account({companion::AccountAction::status,{}}),"local retention real Account status submission refused");
    require(wait([&]{client.poll();return barrier->entered.load(std::memory_order_acquire);},setupDeadline),"local retention actual backend pre-call barrier not entered");
    require(barrier->peerAuthenticated&&barrier->statusJob&&barrier->priorityGood,"local retention native peer/action/worker setup invalid");
    require(callerOwned->accountCalls==0&&!barrier->completed,"local retention actual callable ran before barrier release");
    client.close();const auto stoppingAt=GetTickCount64();fixture->stop=true;fixture->io.join();const auto elapsed=GetTickCount64()-stoppingAt;
    require(elapsed>=companion::runtimeWorkerDrainMs-100&&elapsed<companion::runtimeWorkerDrainMs+5000,"local retention did not use unchanged drain bound");
    require(fixture->retained&&fixture->serverStatus==int(companion::RuntimeStatus::unavailable)
        &&Access::retainedWorkers()==1&&!Access::admissionAvailable(),"local retention actual workerRetained/admission outcome missing");
    fixture.reset();callerOwned.reset();barrier->fixtureGone=true;
    require(!lifetime.expired(),"actual callback state died with fixture/server/caller");
    Access::reapRetained();require(Access::retainedWorkers()==1&&!lifetime.expired(),"blocked native worker was prematurely reaped");
    std::cout<<"DRUM_LOCAL_RETENTION phase=fixture_destroyed worker_retained=1 weak_alive=1 elapsed_ms="<<elapsed<<std::endl;
    barrier->release();
    require(wait([&]{return barrier->completed.load(std::memory_order_acquire);},Clock::now()+std::chrono::milliseconds(3000)),"actual callback did not finish after fixture destruction");
    require(barrier->fixtureGoneWhenCalled&&barrier->accountReplyValid,"actual post-destruction Account callback/result failed");
    {auto observed=lifetime.lock();require(bool(observed),"callback state expired before native reaping");
        require(observed->accountCalls==1&&observed->generationCalls==0&&observed->providerCalls==0&&observed->evaluationCalls==0,"actual callback counters differ from one Account status");
        const std::lock_guard lock(observed->mutex);require(observed->records.size()==0,"Account callback unexpectedly created a generation record");}
    require(!lifetime.expired()&&Access::retainedWorkers()==1,"actual callable owner vanished before native completion/reap");
    require(wait([&]{Access::reapRetained();return Access::retainedWorkers()==0;},Clock::now()+std::chrono::milliseconds(3000)),"actual retained worker native completion/reap missing");
    require(lifetime.expired()&&!Access::admissionAvailable(),"callback state did not expire after native reaping or poisoned admission changed");
    std::cout<<"DRUM_LOCAL_RETENTION phase=complete actual_account_calls=1 actual_reply_valid=1 native_reaped=1 weak_expired=1"<<std::endl;return 0;
}
#else
// Preserve existing non-production builds. Opt-in explicitly fails unavailable.
struct DrumLocalProfile final {
    DrumLocalProfile(){throw std::runtime_error("Local capacity preview requires existing Windows production-profile build");}
    companion::ClientOptions options()const{return {};}
    void connect(InstrumentProcessor&){}void generate(InstrumentProcessor&){}void pump(InstrumentProcessor&){}
    J finish(InstrumentProcessor&){return {};}
};
#endif
// Call intervals are retained raw; this canonical union is fixture-report work,
// never a product allocation/callback or a source of admission authority.
Ns drumCallOverlapNs(const J& intervals,Ns begin,Ns end){
    Ns total=0;for(int n=0;n<intervals.size();++n){const auto call=intervals[n];
        total+=std::max<Ns>(0,std::min(end,Ns(juce::int64(call["end_ns"])))-std::max(begin,Ns(juce::int64(call["start_ns"]))));}
    return total;
}
void drumLocalCoverageSelfTest(){
    J calls(juce::Array<J>{});for(const auto pair:{std::pair<Ns,Ns>{100,120},{180,200}}){auto call=object();put(call,"start_ns",integer(pair.first));put(call,"end_ns",integer(pair.second));calls.append(call);}
    require(drumCallOverlapNs(calls,150,160)==0,"gap-only envelope overlap must not qualify");
    require(drumCallOverlapNs(calls,205,220)==0,"cleanup-only envelope overlap must not qualify");
    require(drumCallOverlapNs(calls,110,190)==20,"actual call intersections required");
}
J drumLocalCoverage(const J& row){
    const auto integerValue=[](const J& v){require((v.isInt()||v.isInt64())&&juce::int64(v)>=0,"coverage raw integer missing");return Ns(juce::int64(v));};
    const int block=int(row["block_size"]),rate=int(row["sample_rate"]);require(block>0&&rate>0,"coverage host geometry missing");
    const bool lifecycle=row["phase"].toString()==drumCapacityPhases[2];const auto events=drumCapacityEvents(rate,block,false);
    const Ns boundary=lifecycle?events[1].actual:integerValue(row["measured_frames"]);const int required=int(boundary/block);
    auto report=object();put(report,"contract","STRICT_A_ALL_STEADY_OR_LIFECYCLE_PRE_FIRST_INVALIDATING_EDIT");
    put(report,"required_prefix_callbacks",required);put(report,"required_start_index",0);put(report,"required_end_index_exclusive",required);put(report,"required_frame_end_exclusive",integer(boundary));
    put(report,"all_callback_count",row["observations"].size());put(report,"first_invalidating_event_index",lifecycle?J(1):J());
    put(report,"edit_callback_excluded",lifecycle);put(report,"strict_complete",false);
    const auto p=row["local_preview"];J intervals(juce::Array<J>{}),perCallback(juce::Array<J>{}),gaps(juce::Array<J>{});
    bool traceValid=p["observations"].size()==1;
    if(traceValid){const auto work=p["observations"][0];const auto calls=work["render_calls"];traceValid=calls.isArray()&&!bool(work["trace_overflow"])&&int(work["trace_count"])==calls.size();
        Ns previousEnd=0;for(int n=0;n<calls.size();++n){const auto a=integerValue(calls[n]["start_ns"]),b=integerValue(calls[n]["end_ns"]);require(a>0&&b>a&&a>=previousEnd,"real call span ordering/extent wrong");
            if(intervals.size()&&a==previousEnd){auto last=intervals[intervals.size()-1];put(last,"end_ns",integer(b));}
            else{auto interval=object();put(interval,"start_ns",integer(a));put(interval,"end_ns",integer(b));intervals.append(interval);}previousEnd=b;}}
    int covered=0,first=-1,last=-1;Ns overlapNs=0;bool callbacksValid=true;
    for(int n=0;n<row["observations"].size();++n){const auto callback=row["observations"][n];const auto a=integerValue(callback["callback_start_ns"]),b=integerValue(callback["callback_end_ns"]);
        callbacksValid=callbacksValid&&a<b&&b-a==integerValue(callback["callback_ns"])&&integerValue(callback["frame"])==Ns(n)*block;
        const auto overlap=drumCallOverlapNs(intervals,a,b);const bool intersects=overlap>0,needed=n<required;auto evidence=object();
        put(evidence,"index",n);put(evidence,"required",needed);put(evidence,"intersects_real_render_call",intersects);put(evidence,"intersection_ns",integer(overlap));perCallback.append(evidence);
        if(needed){overlapNs+=overlap;if(intersects){++covered;if(first<0)first=n;last=n;}else gaps.append(n);}}
    put(report,"actual_call_union",intervals);put(report,"callback_intersections",perCallback);put(report,"gap_indices",gaps);
    put(report,"covered_required_callbacks",covered);put(report,"first_covered_required_index",first);put(report,"last_covered_required_index",last);put(report,"required_intersection_ns",integer(overlapNs));
    put(report,"trace_valid",traceValid);put(report,"callback_spans_valid",callbacksValid);
    put(report,"strict_complete",traceValid&&callbacksValid&&required>0&&required<=row["observations"].size()&&covered==required&&first==0&&last==required-1&&gaps.size()==0);return report;
}
void validateDrumLocalProfile(const J& row){
    const auto p=row["local_preview"];require(p.isObject()&&int(p["generation_calls"])==1&&int(p["provider_calls"])==1&&int(p["evaluation_calls"])==1,"one actual local Generate/evaluation required");
    require(p["process_cpu"].isObject(),"actual all-thread process CPU observation required");
    require(!bool(p["observation_overflow"])&&p["observations"].size()==1&&int(p["server_status"])==int(companion::RuntimeStatus::ready)&&!bool(p["worker_retained"]),"local observation/drain incomplete");
    const auto d=p["observations"][0];const auto integerValue=[](const J& v){require((v.isInt()||v.isInt64())&&juce::int64(v)>=0,"local raw integer missing");return Ns(juce::int64(v));};
    const auto start=integerValue(d["start_ns"]),end=integerValue(d["end_ns"]);
    require(start>0&&end>start&&bool(d["prepared"])&&int(d["prepared_workers"])==3&&integerValue(d["worker_blocks"])>0&&integerValue(d["worker_frames"])>0,"actual local engine/three-worker work missing");
    require(bool(d["space_ready"])&&int(d["space_modes"])==128&&int(d["space_mask"])==(1<<6)&&int(d["physical_factor"])==1&&int(d["effective_quality"])==int(RenderQuality::realtime)&&int(d["latency_samples"])==0,"local ordinary48k/realtime active Space differs");
    require(integerValue(d["space_frames"])==integerValue(d["rendered_frames"])&&bool(d["references_complete"])&&d["first_captured_per_bank"].size()==4,"local physical/reference progress missing");
    for(int bank=0;bank<4;++bank)require(int(d["first_captured_per_bank"][bank])==32,"local first render must capture128 real hits");
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
    require(bool(p["first_real_call_completed"])&&int(p["coordination_bound_ms"])==int(companion::runtimeWorkerStartupMs),"bounded real-start coordination incomplete");
#else
    require(false,"local profile requires existing production build");
#endif
    require(bool(row["local_preview_coverage"]["strict_complete"]),"strict actual-render-call coverage incomplete; no retry");
    require(!bool(d["trace_overflow"])&&int(d["trace_count"])==d["render_calls"].size()&&integerValue(d["rendered_blocks"])==integerValue(d["trace_count"]),"actual call trace incomplete/overflow");
    require(integerValue(d["trace_charge_bytes"])==integerValue(p["compiled_sizes_processor_work_result_trace_observation"][3]),"real trace allocation/C reservation mismatch");
    const bool lifecycle=row["phase"].toString()==drumCapacityPhases[2];
    if(!lifecycle)require(bool(p["public_ready"])&&integerValue(p["public_body_frames"])==384000&&integerValue(p["public_frames"])==576000&&integerValue(d["rendered_frames"])==576000,"steady full local proposal incomplete");
    else require(int(d["status"])==int(ai::RenderStatus::ready)||int(d["status"])==int(ai::RenderStatus::cancelled),"lifecycle preview must ready or naturally cancel");
    require(p["terminals"].size()==1&&integerValue(p["terminals"][0]["terminal_pcm_frames"])==0,"local terminal absent or carried PCM");
}

J drumCapacityWindow(const DrumCapacityPoint& point,RenderQuality quality,const juce::String& phase,int repeat,bool reduced,bool localPreview=false) {
    auto result=object();const auto windowStart=Clock::now();std::unique_ptr<DrumLocalProfile> local;std::unique_ptr<InstrumentProcessor> processor;
    try {
        Step7AuditScope audit;step7ResetAudit();
        require(std::any_of(drumCapacityPhases.begin(),drumCapacityPhases.end(),[&](const char* name){return phase==name;}),"unknown drum window");
        const int rate=reduced?48000:point.rate,block=reduced?64:point.block,factor=quality==RenderQuality::higher?2:1;const unsigned frames=reduced?4096u:480000u;
        const bool lifecycle=phase==drumCapacityPhases[2];const Ns warm=reduced?64:point.warm,measured=reduced?(lifecycle?1024:256):(lifecycle?point.lifecycle:point.steady);
        put(result,"point",point.name);put(result,"phase",phase);put(result,"repeat",repeat);put(result,"selftest_reduced",reduced);put(result,"sample_rate",rate);put(result,"block_size",block);put(result,"canonical_frames",int(frames));put(result,"source_rate",48000);put(result,"warmup_frames",integer(warm));put(result,"measured_frames",integer(measured));put(result,"selected_quality",int(quality));
        put(result,"fresh_processor",true);put(result,"host_bpm",120);put(result,"host_meter_numerator",4);put(result,"host_meter_denominator",4);put(result,"initial_note_rounds",2);put(result,"initial_host_note_count",32);put(result,"memory_before",processMemory());
        companion::ClientOptions options;options.allowLaunch=false;if(localPreview){require(!reduced,"No reduced local timing proxy");local=std::make_unique<DrumLocalProfile>();options=local->options();}const auto construct=Clock::now();processor=std::make_unique<InstrumentProcessor>(options);put(result,"construct_ns",integer(elapsed(construct)));auto& p=*processor;
        DrumCapacityHead head;head.rate=rate;p.setPlayHead(&head);juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(8192);J setup(juce::Array<J>{});
        auto progress=[&](const char* label){audio.clear();midi.clear();step7Process(p,audio,midi);head.frame+=block;auto row=drumCaptureObservation(p);put(row,"label",label);put(row,"host_frame_after",integer(head.frame));put(row,"space",drumSpaceJson(p.spaceRenderStats()));setup.append(row);juce::MessageManager::getInstance()->runDispatchLoopUntil(1);};
        for(unsigned bank=0;bank<4;++bank){std::uint64_t revision=0;const auto current=snapshot(p,revision);RackDesign design;design.id=drumFixtureId(bank,true);design.name="Drum capacity bank "+juce::String(bank);design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};RackOperation operation;operation.design=design;
            operation.newInstance=drumFixtureId(bank,false);operation.allowSlotReuse=true;operation.allocation={{0,std::uint8_t(bank)}};operation.destination={RackRole::synth,masterChain,std::uint8_t(bank)};require(p.applyRackOperation(operation,revision)==RackCandidateStatus::ready,"public drum rack placement rejected");}
        auto desired=drumFixtureSound(point,quality,phase);std::uint64_t revision=0;const auto placed=snapshot(p,revision);desired.personalities=placed.personalities;manualEdit(p,desired,revision);
        p.setNonRealtime(quality==RenderQuality::higher);auto prepare=Clock::now();p.prepareToPlay(rate,block);put(result,"initial_prepare_ns",integer(elapsed(prepare)));for(int n=0;n<8;++n)progress("initial-source-settle");
        J assetIds(juce::Array<J>{});std::vector<float> scratch;scratch.reserve(std::size_t(frames)*2);const auto padSettings=drumFixturePads(point,quality,phase);
        for(unsigned bank=0;bank<4;++bank)for(unsigned pad=0;pad<16;++pad){const unsigned channels=phase==drumCapacityPhases[0]||pad>=8?2u:1u;drumPcm(scratch,bank,pad,channels,frames);
            std::uint64_t ticket=0;DrumCommitError error=DrumCommitError::none;
            if(!p.prepareDrumAsset(scratch,channels,ticket,error))throw std::runtime_error("public prepareDrumAsset error="+std::to_string(int(error)));
            try {drums::DrumPad metadata;metadata.asset=InstrumentDrumTestAccess::preparedId(p,ticket);metadata.name=("capacity-b"+juce::String(bank)+"-p"+juce::String(pad)).toStdString();DrumPadTarget target;
                if(!p.captureDrumPadTarget(drumFixtureId(bank,false),0,std::uint8_t(pad),target,error))throw std::runtime_error("public captureDrumPadTarget error="+std::to_string(int(error)));
                const auto values=drumPadValues(padSettings[pad]);std::array<ParameterGestureValue,9> changes{};
                for(unsigned field=0;field<9;++field){const auto id="synth."+juce::String(bank+1)+".drum.pad."+juce::String(pad+1)+"."+drumPadFields[field];const auto n=index(id);const auto& descriptor=parameterRegistry()[n];changes[field]={n,juce::NormalisableRange<float>(descriptor.minimum,descriptor.maximum,0,descriptor.skew).convertTo0to1(values[field])};}
                if(!p.assignPreparedDrumPad(target,ticket,metadata,changes,error))throw std::runtime_error("public assignPreparedDrumPad error="+std::to_string(int(error)));
                assetIds.append(juce::String(drums::assetIdText(*metadata.asset)));
            }catch(...){p.discardPreparedDrumAsset(ticket);throw;}
            if(pad==15)for(int n=0;n<4;++n)progress("assignment-batch16");}
        scratch.clear();scratch.shrink_to_fit();put(result,"asset_ids",assetIds);put(result,"assignment_batch_limit",16);put(result,"setup_observations",setup);
        prepare=Clock::now();p.prepareToPlay(rate,block);put(result,"final_prepare_ns",integer(elapsed(prepare)));for(int n=0;n<8;++n)progress("final-source-settle");put(result,"setup_observations",setup);
        const auto actual=snapshot(p,revision);ProjectDocument project;std::uint64_t projectRevision=0;require(p.tryProjectSnapshot(project,projectRevision),"drum project snapshot failed");juce::MemoryBlock encodedProject;require(encodeProjectState(project,encodedProject),"drum project encode failed");
        auto projectJson=object();put(projectJson,"encoded_base64",juce::Base64::toBase64(encodedProject.getData(),encodedProject.getSize()));put(projectJson,"sha256",juce::SHA256(encodedProject).toHexString());put(projectJson,"document",documentJson(actual));put(projectJson,"playback_mode",int(project.mode));put(result,"project",projectJson);
        const auto settings=drumSourceSettings(actual,frames,phase);put(result,"source_settings",settings);DrumCapacityModel model(settings,rate,factor,int(frames));
        put(result,"effective_quality",int(p.effectiveRenderQuality()));put(result,"host_non_realtime",p.isNonRealtime());put(result,"host_latency_samples",p.getLatencySamples());require(p.selectedRenderQuality()==quality&&!p.renderQualityPending(),"drum selected quality pending");
        put(result,"workers_before",drumWorkersJson(p.dspWorkerStats()));put(result,"space_before",drumSpaceJson(p.spaceRenderStats()));validateDrumSpace(result["space_before"],factor);
        require(p.dspWorkerStats().prepared==3,"drum fixture requires prepared three workers");if(local)local->connect(p);put(result,"setup_before_warmup_ns",integer(elapsed(windowStart)));const auto warmStart=Clock::now();J initial(juce::Array<J>{}),warmRows(juce::Array<J>{});
        drumRound(midi,1,true,0,initial);drumRound(midi,2,true,0,initial);put(result,"initial_midi",initial);
        for(Ns frame=0;frame<warm;frame+=block){const auto before=drumSpaceJson(p.spaceRenderStats());step7Process(p,audio,midi);model.advance(midi,block);midi.clear();head.frame+=block;auto row=drumCaptureObservation(p);const auto after=drumSpaceJson(p.spaceRenderStats());validateDrumSpaceAdvance(before,after,block,factor);put(row,"space_before",before);put(row,"space_after",after);put(row,"frame",integer(frame));warmRows.append(row);}
        put(result,"warmup_wall_ns",integer(elapsed(warmStart)));put(result,"warmup_observations",warmRows);const auto captured=drumCaptureObservation(p);for(const auto& property:captured.getDynamicObject()->getProperties())result.getDynamicObject()->setProperty(property.name,property.value);
        if(local)local->generate(p);
        const auto script=lifecycle?drumCapacityEvents(rate,block,reduced):std::vector<Event>{};J eventRows(juce::Array<J>{}),observations(juce::Array<J>{}),raw(juce::Array<J>{});std::vector<Ns> times;times.reserve(std::size_t(measured/block));std::size_t next=0;AudioResult total;Ns nonzero=0,tailNonzero=0;const auto measureStart=Clock::now();
        for(Ns frame=0;frame<measured;frame+=block){if(local)local->pump(p);midi.clear();
            while(next<script.size()&&script[next].actual<frame+block){const auto& event=script[next];auto row=object();put(row,"name",event.name);put(row,"requested_frame",integer(event.requested));put(row,"actual_frame",integer(event.actual));put(row,"sample_offset",int(event.actual-frame));put(row,"control_at_boundary",event.boundary);
                const auto operation=Clock::now();if(next==1||next==5){std::uint64_t edit=0;auto updated=snapshot(p,edit);const int group=next==1?1:0;
                    for(unsigned bank=0;bank<4;++bank)for(unsigned pad:{14u,15u})set(updated,"synth."+juce::String(bank+1)+".drum.pad."+juce::String(pad+1)+".choke_group",float(group));manualEdit(p,updated,edit);model.choke(group);put(row,"document_after",documentJson(snapshot(p,edit)));}
                J eventMidi(juce::Array<J>{});drumEventMidi(unsigned(next),midi,int(event.actual-frame),eventMidi);put(row,"midi",eventMidi);put(row,"operation_ns",integer(elapsed(operation)));eventRows.append(row);++next;}
            const auto spaceBefore=drumSpaceJson(p.spaceRenderStats()),workersBefore=drumWorkersJson(p.dspWorkerStats());
            const auto t=Clock::now();step7Process(p,audio,midi);const auto ns=elapsed(t);times.push_back(ns);raw.append(integer(ns));
            model.advance(midi,block);auto row=drumCaptureObservation(p);put(row,"frame",integer(frame));put(row,"absolute_host_frame",integer(head.frame));put(row,"host_frames",block);put(row,"physical_frames",block*factor);put(row,"callback_ns",integer(ns));if(local){const auto start=std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();put(row,"callback_start_ns",integer(start));put(row,"callback_end_ns",integer(start+ns));}
            const auto spaceAfter=drumSpaceJson(p.spaceRenderStats());validateDrumSpaceAdvance(spaceBefore,spaceAfter,block,factor);put(row,"space_before",spaceBefore);put(row,"space_after",spaceAfter);put(row,"workers_before",workersBefore);put(row,"workers_after",drumWorkersJson(p.dspWorkerStats()));
            AudioResult observed;scan(observed,audio);put(row,"audio",audioJson(observed));nonzero+=Ns(observed.nonzero);if(lifecycle&&frame>=script.back().actual)tailNonzero+=Ns(observed.nonzero);scan(total,audio);observations.append(row);head.frame+=block;
        }
        require(next==script.size(),"drum lifecycle event unexecuted");const auto measureEnd=Clock::now();put(result,"measure_wall_ns",integer(std::chrono::duration_cast<std::chrono::nanoseconds>(measureEnd-measureStart).count()));put(result,"measure_start_steady_ns",integer(std::chrono::duration_cast<std::chrono::nanoseconds>(measureStart.time_since_epoch()).count()));put(result,"measure_end_steady_ns",integer(std::chrono::duration_cast<std::chrono::nanoseconds>(measureEnd.time_since_epoch()).count()));put(result,"events",eventRows);put(result,"raw_callback_ns",raw);put(result,"observations",observations);put(result,"summary",summary(times,deadlineNs(rate,block)));
        put(result,"raw_finite",total.finite);put(result,"raw_peak",total.peak);put(result,"raw_nonzero_samples",integer(nonzero));put(result,"raw_nonzero_after_stop",integer(tailNonzero));put(result,"raw_audio",audioJson(total));put(result,"space_after",drumSpaceJson(p.spaceRenderStats()));put(result,"workers_after",drumWorkersJson(p.dspWorkerStats()));put(result,"host_frame_end",integer(head.frame));put(result,"callback_allocation_audit",step7AuditResult());
        put(result,"intended_canonical_decoded_bytes",integer(Ns(frames)*(phase==drumCapacityPhases[0]?128:96)*4));put(result,"intended_decoded_limit_bytes",integer(Ns(drums::intendedDecodedLimit)));put(result,"retained_decoded_limit_bytes",integer(Ns(drums::retainedDecodedLimit)));put(result,"compiled_drum_engine_sizeof",integer(Ns(sizeof(drums::DrumEngine))));put(result,"memory_after",processMemory());
        if(local)put(result,"local_preview",local->finish(p));
        auto t=Clock::now();p.setPlayHead(nullptr);p.releaseResources();put(result,"release_ns",integer(elapsed(t)));t=Clock::now();processor.reset();put(result,"destroy_ns",integer(elapsed(t)));put(result,"window_wall_ns",integer(elapsed(windowStart)));put(result,"integrity",true);
    }catch(const std::exception& e){put(result,"integrity",false);put(result,"error",e.what());if(processor){processor->setPlayHead(nullptr);processor->releaseResources();processor.reset();}put(result,"window_wall_ns",integer(elapsed(windowStart)));}
    return result;
}
void validateDrumCapacityWindow(const J& row) {
    require(bool(row["complete"])&&int(row["reference_capacity"])==192,"drum report must be complete with192 references");
    require(row["captured_per_bank"].size()==4,"drum capture report missing");
    for(int bank=0;bank<4;++bank)require(int(row["captured_per_bank"][bank])==32,"drum full capacity requires captured32 per bank/128 total");
    for(const auto* field:{"raw_callback_ns","observations","events","asset_ids","source_settings","project","space_before","workers_before"})require(row.hasProperty(field),"drum report missing required field");
    require(bool(row["integrity"])&&!row.hasProperty("error")&&bool(row["fresh_processor"])&&int(row["repeat"])>=1&&int(row["repeat"])<=3,"drum fixture failed or invalid repeat");
    const auto point=std::find_if(drumCapacityPoints.begin(),drumCapacityPoints.end(),[&](const DrumCapacityPoint& p){return row["point"].toString()==p.name;});require(point!=drumCapacityPoints.end(),"drum report point unknown");
    const auto phase=row["phase"].toString();require(std::any_of(drumCapacityPhases.begin(),drumCapacityPhases.end(),[&](const char* value){return phase==value;}),"drum report phase unknown");
    const int selected=int(row["selected_quality"]);require(selected==int(RenderQuality::realtime)||selected==int(RenderQuality::higher),"drum report quality unknown");const auto quality=static_cast<RenderQuality>(selected);
    const bool reduced=bool(row["selftest_reduced"]),lifecycle=phase==drumCapacityPhases[2];const int rate=reduced?48000:point->rate,block=reduced?64:point->block,factor=quality==RenderQuality::higher?2:1,frames=reduced?4096:480000;
    const Ns warm=reduced?64:point->warm,measured=reduced?(lifecycle?1024:256):(lifecycle?point->lifecycle:point->steady);const int blocks=int(measured/block);
    require(int(row["sample_rate"])==rate&&int(row["block_size"])==block&&int(row["canonical_frames"])==frames&&int(row["source_rate"])==48000&&juce::int64(row["warmup_frames"])==warm&&juce::int64(row["measured_frames"])==measured,"drum report physical geometry/frame extent incorrect");
    require(int(row["effective_quality"])==selected&&bool(row["host_non_realtime"])==(quality==RenderQuality::higher)&&int(row["host_bpm"])==120&&int(row["host_meter_numerator"])==4&&int(row["host_meter_denominator"])==4,"drum effective quality/nonRealtime/transport metadata wrong");
    int latency=0;if(quality==RenderQuality::higher){juce::dsp::Oversampling<float> fir(2);fir.clearOversamplingStages();fir.addOversamplingStage(juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,.08f,-100.f,.08f,-100.f);fir.initProcessing(std::size_t(block));latency=int(std::ceil(fir.getLatencyInSamples()*.5f));}
    require(int(row["host_latency_samples"])==latency&&int(row["initial_note_rounds"])==2&&int(row["initial_host_note_count"])==32&&int(row["assignment_batch_limit"])==16,"drum latency/round/batch contract wrong");
    const auto same=[](const J& a,const J& b){return juce::JSON::toString(a)==juce::JSON::toString(b);};
    const auto ns=[](const J& value){require((value.isInt64()||value.isInt())&&juce::int64(value)>=0,"drum nanoseconds/counter must be nonnegative integer");return Ns(juce::int64(value));};
    require(ns(row["adopted_generation"])>0,"stale drum adopted generation");
    juce::MemoryBlock bytes;juce::MemoryOutputStream decodedStream(bytes,false);require(juce::Base64::convertFromBase64(decodedStream,row["project"]["encoded_base64"].toString()),"drum project bytes missing");decodedStream.flush();
    require(juce::SHA256(bytes).toHexString()==row["project"]["sha256"].toString(),"drum project hash mismatch");auto project=std::make_unique<ProjectDocument>();require(decodeProjectState(bytes.getData(),bytes.getSize(),*project),"drum project decode failed");
    auto expected=drumFixtureSound(*point,quality,phase);require(project->sound.personalities&&project->sound.personalities->instances.size()==4,"drum fixed rack instances missing");
    for(unsigned bank=0;bank<4;++bank){const auto& racks=project->sound.personalities->instances;const auto found=std::find_if(racks.begin(),racks.end(),[&](const RackInstance& r){return r.id==drumFixtureId(bank,false);});require(found!=racks.end(),"drum deterministic instance missing");
        require(found->design.id==drumFixtureId(bank,true)&&found->design.revision==1&&found->design.name=="Drum capacity bank "+juce::String(bank)&&found->design.units.size()==1&&found->design.units[0].module==NativeModule::drum&&found->design.units[0].algorithmRevision==1&&found->design.units[0].id==0&&found->bindings==std::vector<UnitBinding>{{0,std::uint8_t(bank)}},"drum deterministic design/unit/physical allocation wrong");}
    expected.personalities=project->sound.personalities;require(project->sound==expected&&project->mode==PlaybackMode::externalMidi&&project->pattern.noteCount==0&&!project->group&&int(row["project"]["playback_mode"])==int(PlaybackMode::externalMidi),"drum complete sound/project/FX contract differs");
    require(same(row["project"]["document"],documentJson(project->sound)),"drum complete physical document readback differs");
    auto patch=std::make_unique<RoutedPatch>();require(makeRoutedPatch(project->sound,*patch),"drum routed patch invalid");for(unsigned bank=0;bank<4;++bank)require(patch->drumSource[bank]&&patch->synth.layers[bank].enabled&&patch->synth.layers[bank].level==1&&patch->synth.layers[bank].pan==0,"drum source family/common bank controls wrong");
    const auto settings=drumSourceSettings(project->sound,unsigned(frames),phase);require(same(settings,row["source_settings"]),"drum complete physical source metadata/settings differ");
    require(row["asset_ids"].isArray()&&row["asset_ids"].size()==64,"drum canonical IDs missing/truncated");std::vector<float> scratch;
    for(unsigned bank=0;bank<4;++bank)for(unsigned pad=0;pad<16;++pad){const unsigned channels=phase==drumCapacityPhases[0]||pad>=8?2u:1u;drumPcm(scratch,bank,pad,channels,unsigned(frames));drums::AssetPtr asset;drums::AssetError error;
        require(drums::makeSampleAsset(scratch,channels,asset,error)&&error==drums::AssetError::none,"drum expected canonical source generation failed");require(row["asset_ids"][int(bank*16+pad)].toString()==juce::String(drums::assetIdText(asset->id())),"drum deterministic canonical ID/hash differs");}
    require(same(row["configured_asset_ids"],row["asset_ids"]),"drum actual configured IDs differ from admitted assets");
    require(row["configured_per_bank"].size()==4&&row["setup_observations"].size()==32&&row["warmup_observations"].size()==warm/block,"drum setup/warmup observation extent wrong");
    for(int bank=0;bank<4;++bank)require(int(row["configured_per_bank"][bank])==16,"drum configured pad capacity wrong");
    for(int n=0;n<32;++n){const auto setup=row["setup_observations"][n];require(ns(setup["host_frame_after"])==Ns(n+1)*block,"drum setup host timeline wrong");
        require(setup["label"].toString()==(n<8?"initial-source-settle":n<24?"assignment-batch16":"final-source-settle"),"drum setup batch order wrong");}
    validateDrumSpace(row["space_before"],factor);validateDrumSpace(row["space_after"],factor);
    const auto worker=[](const J& value){require(value.isObject()&&int(value["prepared"])==3&&(value["blocks"].isInt64()||value["blocks"].isInt())&&(value["internal_frames"].isInt64()||value["internal_frames"].isInt())&&juce::int64(value["blocks"])>=0&&juce::int64(value["internal_frames"])>=0,"drum prepared three worker/progress metadata wrong");};
    worker(row["workers_before"]);worker(row["workers_after"]);require(ns(row["workers_after"]["blocks"])>ns(row["workers_before"]["blocks"])&&ns(row["workers_after"]["internal_frames"])>ns(row["workers_before"]["internal_frames"]),"drum workers made no actual progress");
    DrumCapacityModel model(settings,rate,factor,frames);juce::MidiBuffer midi;J initial(juce::Array<J>{});drumRound(midi,1,true,0,initial);drumRound(midi,2,true,0,initial);require(same(initial,row["initial_midi"]),"drum initial MIDI channels/rounds/order/velocities differ");
    auto validateReferences=[&](const J& observation){require(bool(observation["complete"])&&int(observation["reference_capacity"])==192&&ns(observation["adopted_generation"])==ns(row["adopted_generation"])&&same(observation["configured_asset_ids"],row["asset_ids"]),"drum incomplete/stale/configured reference report");const auto counts=model.counts();const int total=std::accumulate(counts.begin(),counts.end(),0);
        require(observation["captured_per_bank"].size()==4&&observation["configured_per_bank"].size()==4&&observation["captured_pad_counts"].size()==4,"drum reference rows truncated");
        for(int bank=0;bank<4;++bank){require(int(observation["configured_per_bank"][bank])==16&&int(observation["captured_per_bank"][bank])==total&&observation["captured_pad_counts"][bank].size()==16,"drum independent captured count/lifetime mismatch");
            for(int pad=0;pad<16;++pad)require(int(observation["captured_pad_counts"][bank][pad])==counts[std::size_t(pad)],"drum independent per-pad natural/choke/channel/stop/refill lifetime mismatch");}
        if(!lifecycle)require(total==32,"drum steady callback lost full128 capacity");};
    J previousSpace=row["space_before"];
    for(int n=0;n<warm/block;++n){const auto observation=row["warmup_observations"][n];model.advance(midi,block);midi.clear();validateReferences(observation);require(ns(observation["frame"])==Ns(n)*block&&same(observation["space_before"],previousSpace),"drum warmup timeline/Space continuity wrong");validateDrumSpaceAdvance(observation["space_before"],observation["space_after"],block,factor);previousSpace=observation["space_after"];}
    validateReferences(row);
    const auto script=lifecycle?drumCapacityEvents(rate,block,reduced):std::vector<Event>{};require(row["events"].isArray()&&row["events"].size()==int(script.size()),"drum lifecycle events missing/truncated");
    require(row["raw_callback_ns"].isArray()&&row["raw_callback_ns"].size()==blocks&&row["observations"].size()==blocks,"drum callback distribution missing/truncated");
    std::vector<Ns> times;times.reserve(std::size_t(blocks));std::size_t next=0;Ns nonzero=0,tailNonzero=0;double peak=0;auto controlSound=project->sound;J previousWorkers;
    for(int n=0;n<blocks;++n){const Ns frame=Ns(n)*block;midi.clear();while(next<script.size()&&script[next].actual<frame+block){const auto& expectedEvent=script[next];const auto event=row["events"][int(next)];
            require(event["name"].toString()==expectedEvent.name&&ns(event["requested_frame"])==expectedEvent.requested&&ns(event["actual_frame"])==expectedEvent.actual&&int(event["sample_offset"])==expectedEvent.actual-frame&&bool(event["control_at_boundary"])==expectedEvent.boundary,"drum event requested/actual frame/offset/order wrong");ns(event["operation_ns"]);
            if(next==1||next==5){const int group=next==1?1:0;model.choke(group);for(unsigned bank=0;bank<4;++bank)for(unsigned pad:{14u,15u})set(controlSound,"synth."+juce::String(bank+1)+".drum.pad."+juce::String(pad+1)+".choke_group",float(group));require(same(event["document_after"],documentJson(controlSound)),"drum choke control physical readback differs");}
            J expectedMidi(juce::Array<J>{});drumEventMidi(unsigned(next),midi,int(expectedEvent.actual-frame),expectedMidi);require(same(expectedMidi,event["midi"]),"drum lifecycle MIDI bytes/offset/order wrong");++next;}
        const auto observation=row["observations"][n];const auto duration=ns(row["raw_callback_ns"][n]);require(ns(observation["callback_ns"])==duration&&ns(observation["frame"])==frame&&ns(observation["absolute_host_frame"])==32*Ns(block)+warm+frame&&int(observation["host_frames"])==block&&int(observation["physical_frames"])==block*factor,"drum raw-ns/frame/physical callback record wrong");times.push_back(duration);
        model.advance(midi,block);validateReferences(observation);require(same(observation["space_before"],previousSpace),"drum measured Space counter continuity wrong");validateDrumSpaceAdvance(observation["space_before"],observation["space_after"],block,factor);previousSpace=observation["space_after"];
        worker(observation["workers_before"]);worker(observation["workers_after"]);if(n)require(same(previousWorkers,observation["workers_before"]),"drum worker progress discontinuity");previousWorkers=observation["workers_after"];
        const auto blockProgress=ns(observation["workers_after"]["blocks"])-ns(observation["workers_before"]["blocks"]),frameProgress=ns(observation["workers_after"]["internal_frames"])-ns(observation["workers_before"]["internal_frames"]);
        // Accepted CC/Stop segments can render serially; actual progress is required for each window.
        require(blockProgress>=0&&frameProgress>=0&&(lifecycle||(blockProgress>0&&frameProgress>0)),"drum worker counters regressed or full128 steady callback lost parallel progress");
        const auto audio=observation["audio"];require(bool(audio["finite"])&&std::isfinite(double(audio["peak"]))&&double(audio["peak"])>=0&&double(audio["peak"])<=std::pow(10.,-1./20)+1e-6&&ns(audio["full_scale_excursions"])==0&&!audio["checksum_fnv1a64_float_bits"].toString().isEmpty(),"drum raw audio finite/protected/checksum record wrong");nonzero+=ns(audio["nonzero_samples"]);if(lifecycle&&frame>=script.back().actual)tailNonzero+=ns(audio["nonzero_samples"]);peak=std::max(peak,double(audio["peak"]));}
    require(next==script.size()&&same(previousSpace,row["space_after"])&&same(previousWorkers,row["workers_after"])&&ns(row["host_frame_end"])==32*Ns(block)+warm+measured,"drum final event/worker/Space/host timeline wrong");
    require(bool(row["raw_finite"])&&std::isfinite(double(row["raw_peak"]))&&double(row["raw_peak"])==peak&&peak>0&&ns(row["raw_nonzero_samples"])==nonzero&&nonzero>0,"drum raw output must be finite nonzero and complete");
    require(ns(row["raw_nonzero_after_stop"])==tailNonzero,"drum post-Stop audio record differs (raw Stop retains its accepted global history reset)");
    const auto expectedSummary=summary(times,deadlineNs(rate,block));for(const auto& property:expectedSummary.getDynamicObject()->getProperties()){const auto a=double(row["summary"][property.name]),b=double(property.value);require(std::isfinite(a)&&std::abs(a-b)<=1e-12*std::max(1.,std::abs(b)),"drum independently recomputed raw-ns statistics differ");}
    require(ns(row["intended_canonical_decoded_bytes"])==Ns(frames)*(phase==drumCapacityPhases[0]?128:96)*4&&ns(row["intended_decoded_limit_bytes"])==Ns(drums::intendedDecodedLimit)&&ns(row["retained_decoded_limit_bytes"])==Ns(drums::retainedDecodedLimit)&&ns(row["compiled_drum_engine_sizeof"])==Ns(sizeof(drums::DrumEngine)),"drum canonical/fixed compiled DSP memory accounting wrong");
    const auto audit=row["callback_allocation_audit"];require(ns(audit["callback_invocations"])==32+warm/block+measured/block,"drum callback-only audit count differs");
#if defined(_MSC_VER) && defined(_DEBUG)
    require(audit["status"].toString()=="OBSERVED_DEBUG_CRT"&&ns(audit["allocations"])==0&&ns(audit["frees"])==0&&bool(audit["zero"]),"drum callback-only Debug heap audit failed (worker heap gate remains separate)");
#else
    require(audit["status"].toString()=="UNAVAILABLE_RELEASE_TIMING_ONLY"&&!bool(audit["zero"]),"drum Release must not claim callback/worker heap evidence");
#endif
}
int profileDrumCapacity(const Arguments& args) {
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","drum capacity timings require Release");if(args.drumLocalPreview)drumLocalCoverageSelfTest();
    const auto point=std::find_if(drumCapacityPoints.begin(),drumCapacityPoints.end(),[&](const DrumCapacityPoint& p){return args.drumCapacityPoint==p.name;});require(point!=drumCapacityPoints.end(),"unknown drum capacity point");
    require(std::filesystem::create_directory(std::filesystem::path(args.output.getFullPathName().toWideCharPointer())),"fresh drum capacity output directory required");
    const auto start=Clock::now();const auto before=identities();auto report=object();put(report,"suite","sound-quality");put(report,"selector",args.drumLocalPreview?"drum-local-preview":"drum-capacity-point");put(report,"descriptor",drumCapacityDescriptor(*point,args.quality));put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"inventory",inventoryMetadata(args.inventory));put(report,"identities_before",before);
    put(report,"identity_source_role","identities scans current workspace files. Compiled source authority is the separately sealed source/object/compiler/link/image archive; deferred baseline replay may scan stable candidate workspace files.");
    put(report,"timing_method","steady_clock integer ns around prepared processBlock only. Source generation, asset/parameter preparation, control edits, MIDI construction, observation/worker/reference/Space reads, model, audio scan, JSON and IO are outside clocks. Every raw duration retained; no pacing, filtering, pooling or fastest selection.");
    put(report,"scope","Release practical44.1/48/96k pilot: four banks x32 hits, three prepared workers, eight mixed FX including one nominal active Space. Three fresh processors/windows per repeat; three repeats. Higher descriptive offline. No DAW underrun, worker-heap, eight-Space, universal-capacity or listening claim.");
    put(report,"memory_scope","Canonical256MiB intended/512MiB retained plus reservations remain unchanged. Actual compiled DrumEngine sizeof is reported per window; parent attributes its delta x4 and the separately verified196616-byte quality table. Process peaks are whole-process observations, without an invented per-instance budget.");
    put(report,"started_utc",juce::Time::getCurrentTime().toISO8601(true));put(report,"compiler_msc_full_ver",integer(_MSC_FULL_VER));put(report,"sdk_version","10.0.26100.0");put(report,"juce_pinned_revision","be29c81492b6151c8ea8d14c840e1311963b3a83");
    J rows(juce::Array<J>{}),workloads(juce::Array<J>{});bool integrity=true,targets=true;int number=0;
    for(int repeat=1;repeat<=3;++repeat)for(const auto* phase:drumCapacityPhases){auto row=drumCapacityWindow(*point,args.quality,phase,repeat,false,args.drumLocalPreview);bool valid=true;
        try{validateDrumCapacityWindow(row);if(args.drumLocalPreview){put(row,"local_preview_coverage",drumLocalCoverage(row));validateDrumLocalProfile(row);}}catch(const std::exception& e){valid=false;put(row,"validation_error",e.what());put(row,"integrity",false);}
        const bool target=valid&&double(row["summary"]["mean_callback_load"])<=.75&&double(row["summary"]["p99_ns"])<=double(row["summary"]["deadline_ns"]);put(row,"target_met",target);put(row,"realtime_target_required",args.quality==RenderQuality::realtime);integrity=integrity&&valid;targets=targets&&target;
        auto workload=object();put(workload,"point",point->name);put(workload,"phase",phase);put(workload,"quality",int(args.quality));put(workload,"asset_ids",row["asset_ids"]);put(workload,"source_settings",row["source_settings"]);put(workload,"project_sha256",row["project"]["sha256"]);put(workload,"initial_midi",row["initial_midi"]);
        J eventContract(juce::Array<J>{});for(int n=0;n<row["events"].size();++n){auto event=object();for(const auto* key:{"name","requested_frame","actual_frame","sample_offset","control_at_boundary","midi"})put(event,key,row["events"][n][key]);eventContract.append(event);}put(workload,"events",eventContract);
        const auto serialized=juce::JSON::toString(workload);put(row,"workload_sha256",juce::SHA256(serialized.toRawUTF8(),std::size_t(serialized.getNumBytesAsUTF8())).toHexString());workloads.append(workload);
        const auto filename="drum-"+juce::String(++number).paddedLeft('0',2)+"-"+phase+".json";writeJson(args.output.getChildFile(filename),row);put(row,"trial_file",filename);rows.append(row);put(report,"windows",rows);put(report,"integrity",integrity);writeJson(args.output.getChildFile("progress-"+juce::String(number).paddedLeft('0',2)+".json"),report);
        std::cout<<"DRUM_CAPACITY "<<point->name<<" quality="<<int(args.quality)<<" repeat="<<repeat<<" phase="<<phase<<" integrity="<<valid<<" target="<<target<<std::endl;
    }
    require(number==9,"drum cohort extent differs");const auto after=identities();const bool stable=juce::JSON::toString(before)==juce::JSON::toString(after);integrity=integrity&&stable;
    put(report,"identities_after",after);put(report,"identities_unchanged",stable);put(report,"windows",rows);put(report,"workloads",workloads);put(report,"realtime_targets_met",targets);put(report,"integrity",integrity);put(report,"process_memory",processMemory());put(report,"wall_ns",integer(elapsed(start)));
    const bool success=integrity&&(args.quality==RenderQuality::higher||targets);put(report,"success",success);writeJson(args.output.getChildFile("drum-capacity-profile-report.json"),report);std::cout<<(success?"SUCCESS":"UNRESOLVED")<<" drum capacity pilot; all windows/raw records retained\n";return success?0:1;
}
int drumCapacitySelfTest() {
    int checks=0,failures=0;auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cerr<<"FAIL drum-capacity "<<name<<'\n';}};
    auto rejects=[&](auto&& f,const char* name){try{f();check(false,name);}catch(const std::exception&){check(true,name);}};
    const auto fresh=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/drum-capacity-self-test-"+juce::Uuid().toString()).getFullPathName();
    int descriptors=0;
    for(const auto& point:drumCapacityPoints)for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        ++descriptors;const auto descriptor=drumCapacityDescriptor(point,quality);
        check(int(descriptor["sample_rate"])==point.rate&&int(descriptor["block_size"])==point.block&&int(descriptor["quality"])==int(quality),"six exact point/quality descriptors");
        check(juce::int64(descriptor["warmup_frames"])==Ns(std::ceil(.25*point.rate/point.block))*point.block&&juce::int64(descriptor["steady_frames"])==Ns(std::ceil(double(point.rate)/point.block))*point.block&&juce::int64(descriptor["lifecycle_frames"])==Ns(std::ceil(2.*point.rate/point.block))*point.block,"literal rounded warmup/steady/lifecycle frames");
        check(int(descriptor["repeats"])==3&&int(descriptor["windows_per_repeat"])==3&&int(descriptor["canonical_frames"])==480000&&int(descriptor["source_rate"])==48000,"three independent repeats/windows and full canonical source");
        check(int(descriptor["physical_factor"])==(quality==RenderQuality::higher?2:1)&&int(descriptor["required_workers"])==3&&int(descriptor["steady_captured_per_bank"])==32&&int(descriptor["space_slot"])==6,"physical geometry/full128/three workers/one Space");
        check(std::abs(double(descriptor["stereo_tune"])-(quality==RenderQuality::higher&&point.rate==96000?double(23.99f):24.))<1e-8&&double(descriptor["mean_period_limit"])==.75&&double(descriptor["p99_period_limit"])==1.&&bool(descriptor["higher_offline_descriptive"])==(quality==RenderQuality::higher),"frozen tune exception and unwaived realtime/offline budgets");
        try{const auto parsed=parseArguments({"--profile","--suite","sound-quality","--output",fresh,"--quality",quality==RenderQuality::higher?"higher":"realtime","--drum-capacity-point",point.name});
            check(parsed.suite=="sound-quality"&&parsed.quality==quality,"real parser accepts every drum point in both qualities");}
        catch(const std::exception&){check(false,"real parser accepts every drum point in both qualities");}
    }
    check(descriptors==6,"exactly six full descriptors");
    for(const auto& args:std::vector<juce::StringArray>{
        {"--profile","--suite","smoke","--output",fresh,"--drum-capacity-point","48000/128"},
        {"--profile","--suite","sound-quality","--output",fresh,"--drum-capacity-point","96000/128"},
        {"--profile","--suite","sound-quality","--output",fresh,"--drum-capacity-point","48000/128","--drum-capacity-point","48000/128"},
        {"--profile","--suite","sound-quality","--output",fresh,"--drum-capacity-point","48000/128","--eq-only"},
        {"--profile","--suite","sound-quality","--output",fresh,"--drum-capacity-point","48000/128","--synth-algorithm","1"},
        {"--profile","--suite","sound-quality","--output",fresh,"--drum-capacity-point","48000/128","--era-reverb"},
        {"--profile","--suite","sound-quality","--output",fresh,"--drum-capacity-point"},
        {"--self-test","--drum-capacity-point","48000/128"}})
        rejects([&]{parseArguments(args);},"real parser rejects wrong-suite/unknown/duplicate/incomplete/mixed selectors");
    const auto& point=drumCapacityPoints[1];
    for(const auto* phase:drumCapacityPhases){
        try{const auto report=drumCapacityWindow(point,RenderQuality::realtime,phase,1,true);
            check(report["captured_per_bank"].size()==4&&int(report["captured_per_bank"][0])==32&&int(report["captured_per_bank"][1])==32&&int(report["captured_per_bank"][2])==32&&int(report["captured_per_bank"][3])==32,"actual complete reference report captured32x4/128 after two rounds");
            bool valid=true;try{validateDrumCapacityWindow(report);}catch(const std::exception& e){valid=false;std::cerr<<"drum fixture validation: "<<e.what()<<'\n';}
            check(valid,"real reduced4096-frame/64-host-callback fixture validates full physical/settings/events/FX/count/worker/audio records");
            check(bool(report["selftest_reduced"])&&int(report["canonical_frames"])==4096&&int(report["block_size"])==64&&int(report["sample_rate"])==48000,"reduced geometry is explicitly distinguished from production capacity");
            for(const auto* field:{"raw_callback_ns","observations","events","asset_ids","source_settings","project","space_before","workers_before"}){
                auto broken=juce::JSON::parse(juce::JSON::toString(report));broken.getDynamicObject()->removeProperty(field);
                rejects([&]{validateDrumCapacityWindow(broken);},"missing required real report data rejected");}
            const auto negative=[&](const char* field,const J& value,const char* label){auto broken=juce::JSON::parse(juce::JSON::toString(report));put(broken,field,value);rejects([&]{validateDrumCapacityWindow(broken);},label);};
            negative("captured_per_bank",J(juce::Array<J>{31,32,32,32}),"wrong captured count rejected");
            negative("adopted_generation",integer(-1),"stale generation rejected");
            negative("raw_callback_ns",J(juce::Array<J>{integer(1)}),"truncated callback distribution rejected");
            negative("raw_callback_ns",J(juce::Array<J>{"1"}),"noninteger raw nanoseconds rejected");
            negative("events",J(juce::Array<J>{object()}),"wrong event record rejected");
            negative("asset_ids",J(juce::Array<J>{"bad-hash"}),"wrong canonical hash rejected");
            negative("workers_before",object(),"missing prepared three workers rejected");
            negative("space_before",object(),"missing real ready mode/mask/backend Space rejected");
            negative("raw_finite",false,"nonfinite raw audio rejected");
            negative("raw_nonzero_samples",integer(0),"silent raw audio rejected");
            negative("measured_frames",integer(1),"wrong callback frame extent rejected");
        }catch(const std::exception& e){std::cerr<<"drum reduced fixture: "<<e.what()<<'\n';check(false,"real reduced fixture setup and processing");}
    }
    check(!juce::File(fresh).exists(),"self-test creates no production cohort output");
    std::cout<<"Drum capacity self-test "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}

}
int main(int argc,char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    try {
#if defined(RACKSYNTH_PRODUCTION_PROFILE)
        if(argc==2&&juce::String::fromUTF8(argv[1])=="--drum-local-callback-retention-child")return drumLocalCallbackRetentionChild();
        if(argc==2&&juce::String::fromUTF8(argv[1])=="--drum-local-coverage-self-test"){drumLocalCoverageSelfTest();std::cout<<"Drum local coverage self-test 3 checks, 0 failures\n";return 0;}
#endif
        juce::StringArray args;for(int i=1;i<argc;++i)args.add(argv[i]);const auto options=parseArguments(args);workloadSynthRevision=options.synthRevision;workloadDriveRevision=options.driveRevision;workloadCompressorRevision=options.compressorRevision;workloadModulatedTapRevision=options.modulatedTapRevision;workloadDelayRevision=options.delayRevision;workloadEqRevision=options.eqRevision;workloadFilterRevision=options.filterRevision;workloadFrequencySplitRevision=options.frequencySplitRevision;if(options.mode=="--task13-step7-self-test")return task13Step7SelfTest();if(options.suite=="task13-drive-background-diagnostic")return profileTask13DriveBackgroundDiagnostic(options.output,options.inventory);if(options.suite=="task13-production")return profileTask13Step7(options.output,options.inventory);if(options.mode=="--self-test"){const auto production=productionProfileSelfTest();const auto patterns=patternProfileSelfTest();const auto old=selfTest();const auto history=historyProfileSelfTest();const auto personalities=personalityProfileSelfTest();const auto era=eraProfileSelfTest();const auto drum=drumCapacitySelfTest();return production||patterns||history||old||personalities||era||drum;}if(options.mode=="--help"){std::cout<<"RackSynthProfile --profile --suite full|smoke|pattern|production|rack-personalities|sound-quality|reverb-cost --output FRESH_ABSOLUTE_WORKSPACE_DIRECTORY [--inventory ABSOLUTE_WORKSPACE_JSON] [sound-quality: --quality realtime|higher, --synth-algorithm 1|2, --drive-algorithm 1|2, --compressor-algorithm 1|2, --modulation-algorithm 1|2, --delay-algorithm 1|2, --eq-algorithm 1|2, --filter-algorithm 1|2, --reverb-algorithm 1|2 OR --eq-only OR --filter-only OR --chorus-only OR --phaser-only OR --flanger-only OR --stereo-only OR --dimension-only OR --frequency-shift-only OR --convolution-only OR --frequency-split-only OR --mid-side-split-only OR --wavetable-unison-only OR --era-reverb OR --drum-capacity-point 44100/128|48000/128|96000/512]\nRackSynthProfile --self-test\n";return 0;}if(options.suite=="reverb-cost")return diagnoseReverbCost(options.output);if(options.suite=="rack-personalities")return profileRackPersonalities(options.output,options.inventory);if(!options.drumCapacityPoint.isEmpty())return profileDrumCapacity(options);if(options.suite=="sound-quality")return profileSoundQuality(options.output,options.inventory,options.quality,options.reverbRevision,options.eqOnly,options.filterOnly,options.chorusOnly,options.phaserOnly,options.flangerOnly,options.stereoOnly,options.dimensionOnly,options.frequencyShiftOnly,options.convolutionOnly,options.frequencySplitOnly,options.midSideSplitOnly,options.wavetableUnisonOnly,options.eraReverb);return options.suite=="production"?profileProduction(options.output,options.inventory):options.suite=="pattern"?profilePatterns(options.output,options.inventory):profile(options.suite,options.output,options.inventory);}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

