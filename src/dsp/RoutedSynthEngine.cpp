#include <atomic>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <emmintrin.h>
#endif
#include "RoutedSynthEngine.h"
#include "space/SpaceActivation.h"
#include "FxMonitor.h"
#include "DspPipeline.h"
#include "SpectralEq.h"
#include "HistoryRing.h"
#include "ModulatedTaps.h"
#include "ConvolutionBankData.h"
#include <juce_cryptography/juce_cryptography.h>
#include <bit>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <vector>

namespace airack
{
namespace
{
bool inRange(float x, float lo, float hi) noexcept { return std::isfinite(x) && x >= lo && x <= hi; }
bool validEq(const EqSettings&) noexcept;
bool validFilter(const FilterSettings& f) noexcept
{
    return f.mode>=FilterFxMode::lowpass&&f.mode<=FilterFxMode::highpass
        &&inRange(f.cutoffHz,20,20000)&&inRange(f.resonance,.5f,10)
        &&inRange(f.driveDb,0,24)&&inRange(f.mix,0,1);
}
bool validChorus(const ChorusSettings& c) noexcept
{
    return inRange(c.rateHz,.1f,5)&&inRange(c.depth,0,1)&&inRange(c.baseDelayMs,12,40)
        &&inRange(c.feedback,-.8f,.8f)&&inRange(c.mix,0,1);
}
bool validPhaser(const PhaserSettings& p) noexcept
{
    return inRange(p.rateHz,.1f,5)&&inRange(p.depth,0,1)&&inRange(p.centreHz,20,20000)
        &&inRange(p.feedback,-.8f,.8f)&&inRange(p.mix,0,1);
}
bool validFlanger(const FlangerSettings& f) noexcept
{
    return inRange(f.rateHz,.05f,5)&&inRange(f.depth,0,1)&&inRange(f.baseDelayMs,1,8)
        &&inRange(f.feedback,-.8f,.8f)&&inRange(f.mix,0,1);
}
bool validStereo(const StereoSettings& s) noexcept
{
    return inRange(s.gainDb,-24,12)&&inRange(s.pan,-1,1)&&inRange(s.width,0,2)
        &&s.channelMode>=StereoChannelMode::stereo&&s.channelMode<=StereoChannelMode::mono;
}
bool validDimension(const DimensionSettings& d) noexcept
{
    return inRange(d.depth,0,1)&&inRange(d.rateHz,.05f,2)&&inRange(d.width,0,2)&&inRange(d.mix,0,1);
}
bool validFrequencyShift(const FrequencyShiftSettings& f) noexcept
{ return inRange(f.hz,-1000,1000)&&inRange(f.stereoOffsetHz,-100,100)&&inRange(f.mix,0,1); }
bool validConvolution(const ConvolutionSettings& c) noexcept
{ return c.ir>=ConvolutionIr::unitImpulse&&c.ir<=ConvolutionIr::shortRoom&&inRange(c.predelayMs,0,250)&&inRange(c.wetTrimDb,-24,12)&&inRange(c.mix,0,1); }
bool validFrequencySplit(const FrequencySplitSettings& s) noexcept
{ return inRange(s.crossoverHz,20,20000)&&inRange(s.lowGainDb,-24,12)&&inRange(s.highGainDb,-24,12)
    &&inRange(s.lowTone,-1,1)&&inRange(s.highTone,-1,1)&&inRange(s.mix,0,1); }
bool validMidSideSplit(const MidSideSplitSettings& s) noexcept
{ return inRange(s.midGainDb,-24,12)&&inRange(s.sideGainDb,-24,12)
    &&inRange(s.midTone,-1,1)&&inRange(s.sideTone,-1,1)&&inRange(s.mix,0,1); }
bool valid(const EffectSettings& e) noexcept
{
    const auto& d = e.drive; const auto& t = e.delay; const auto& r = e.reverb; const auto& c = e.compressor;
    return inRange(d.gainDb,0,24) && inRange(d.toneHz,200,20000) && inRange(d.mix,0,1)
        && (d.shape == DriveShape::tanh || d.shape == DriveShape::softSign)
        && inRange(t.seconds,.001f,4) && inRange(t.beats,.0625f,1) && inRange(t.feedback,0,.95f)
        && inRange(t.stereo,0,1) && inRange(t.filterHz,20,20000) && inRange(t.mix,0,1)
        && inRange(t.saturation,0,1) && inRange(t.lowCutHz,20,2000) && inRange(t.wobble,0,1) && inRange(t.wobbleRateHz,.1f,10)
        && inRange(t.rightRatio,.25f,4) && t.note >= 0 && t.note < 22
        && inRange(r.size,0,1) && inRange(r.decaySeconds,.1f,20) && inRange(r.damping,0,1)
        && inRange(r.predelayMs,0,250) && inRange(r.mix,0,1)
        && inRange(c.thresholdDb,-60,0) && inRange(c.ratio,1,20) && inRange(c.attackMs,.1f,100)
        && inRange(c.releaseMs,10,2000) && inRange(c.makeupDb,0,24) && inRange(c.mix,0,1) && validEq(e.eq) && validFilter(e.filter) && validChorus(e.chorus) && validPhaser(e.phaser) && validFlanger(e.flanger) && validStereo(e.stereo) && validDimension(e.dimension) && validFrequencyShift(e.frequencyShift) && validConvolution(e.convolution) && validFrequencySplit(e.frequencySplit) && validMidSideSplit(e.midSideSplit) && validateDynamicEq(e.dynamicEq) && validateSaturation(e.saturation) && space::valid(e.spaceReverb) && validateEraReverb(e.eraReverb);
}
using EqCoefficients = std::array<double,5>;
bool validEq(const EqSettings& e) noexcept
{
    const auto band=[](const EqBandSettings& b,float lo,float hi){
        return inRange(b.frequencyHz,lo,hi)&&inRange(b.gainDb,-18,18)&&inRange(b.q,.25f,4);
    };
    return band(e.lowShelf,20,2000)&&band(e.bell,20,20000)&&band(e.highShelf,1000,20000);
}
// Revision 2 follows the analog prototypes' magnitudes to Nyquist where the
// bilinear transform cramps every band toward the top of the spectrum. Poles
// map by impulse invariance; the numerator's squared magnitude meets the
// prototype exactly at DC and Nyquist and by least squares between.
double eqPrototypeSquared(bool bell,double ratio,double A,double q) noexcept
{
    const auto squared=[ratio](double s2,double s1,double s0){const double re=s0-s2*ratio*ratio,im=s1*ratio;return re*re+im*im;};
    if(bell)return squared(1,A/q,1)/squared(1,1/(A*q),1);
    const double r=std::sqrt(A)/q;
    return A*A*squared(1,r,A)/squared(A,r,1);
}
EqCoefficients matchedEqBoost(bool bell,double rate,double corner,double q,double gainDb) noexcept
{
    const double A=std::pow(10.,gainDb/40),w=2*std::numbers::pi*corner/rate;
    const double wn=bell?w:w/std::sqrt(A),zeta=1/(2*(bell?A*q:q)),r=std::exp(-zeta*wn);
    double a1,s2;
    if(zeta<=1){const double th=wn*std::sqrt(1-zeta*zeta),s=std::sin(th/2);a1=-2*r*std::cos(th);s2=s*s;}
    else{const double th=wn*std::sqrt(zeta*zeta-1),s=std::sinh(th/2);a1=-2*r*std::cosh(th);s2=-s*s;}
    // 1+a1+a2 and 1-a1+a2 without cancellation at low corners.
    const double a2=r*r,om=-std::expm1(-zeta*wn),sum=om*om+4*r*s2,difference=(1+r)*(1+r)-4*r*s2;
    const double A0=sum*sum,A1=difference*difference,A2=-4*a2;
    const auto prototype=[&](double hz){return eqPrototypeSquared(bell,hz/corner,A,q);};
    const double B0=prototype(0)*A0,B1=prototype(rate/2)*A1,start=std::min(corner/8,.2*rate);
    double sxx=0,sxy=0;
    for(int i=0;i<=32;++i){
        const double hz=start*std::pow(.499*rate/start,i/32.),s=std::sin(std::numbers::pi*hz/rate),p1=s*s,p0=1-p1,p2=4*p0*p1;
        const double target=prototype(hz)*(A0*p0+A1*p1+A2*p2),x=p2/target,y=(target-B0*p0-B1*p1)/target;
        sxx+=x*x;sxy+=x*y;
    }
    const double B2=sxy/sxx,s0=std::sqrt(B0),s1=std::sqrt(B1),W=.5*(s0+s1);
    // The minimum-phase root keeps the inverted cut stable.
    const double b0=.5*(W+std::sqrt(std::max(W*W+B2,0.)));
    return {b0,.5*(s0-s1),-B2/(4*b0),a1,a2};
}
// Cuts invert the boost of the same size; the high shelf is the gain over the
// low shelf, so every band's poles sit at or below its corner.
EqCoefficients matchedEqBand(size_t band,double rate,const EqBandSettings& b) noexcept
{
    const double corner=std::min(double(b.frequencyHz),.45*rate);
    auto c=matchedEqBoost(band==1,rate,corner,b.q,std::abs(double(b.gainDb)));
    if(band==2?b.gainDb>0:b.gainDb<0)c={1/c[0],c[3]/c[0],c[4]/c[0],c[1]/c[0],c[2]/c[0]};
    if(band==2){const double gain=std::pow(10.,double(b.gainDb)/20);for(size_t i=0;i<3;++i)c[i]*=gain;}
    return c;
}
bool eqCoefficients(const EqSettings& settings,double rate,AlgorithmRevision revision,std::array<EqCoefficients,3>& output) noexcept
{
    if(!validEq(settings)||!std::isfinite(rate)||rate<8000)return false;
    if(revision!=legacyAlgorithmRevision&&revision!=modernEqAlgorithmRevision)return false;
    const std::array<EqBandSettings,3> bands{settings.lowShelf,settings.bell,settings.highShelf};
    for(size_t n=0;n<bands.size();++n){
        const auto& band=bands[n];auto& c=output[n];
        // A zero-gain endpoint is exactly transparent, including its first
        // sample. Live endpoints still follow the same coefficient-space ramp.
        if(band.gainDb==0){c={1,0,0,0,0};continue;}
        if(revision==modernEqAlgorithmRevision)c=matchedEqBand(n,rate,band);
        else{
            const double frequency=std::min(double(band.frequencyHz),.45*rate),gain=std::pow(10.,double(band.gainDb)/20);
            using Factory=juce::dsp::IIR::ArrayCoefficients<double>;
            const auto raw=n==0?Factory::makeLowShelf(rate,frequency,band.q,gain)
                :n==1?Factory::makePeakFilter(rate,frequency,band.q,gain):Factory::makeHighShelf(rate,frequency,band.q,gain);
            if(!std::isfinite(raw[3])||raw[3]==0)return false;
            c={raw[0]/raw[3],raw[1]/raw[3],raw[2]/raw[3],raw[4]/raw[3],raw[5]/raw[3]};
        }
        for(double value:c)if(!std::isfinite(value))return false;
        // Normalized second-order Jury region is convex. Interpolating these
        // stable endpoints defines our five-ms coefficient-space transition;
        // observable live high-Q tests cover its time-varying behavior.
        if(!(1+c[3]+c[4]>0&&1-c[3]+c[4]>0&&1-c[4]>0))return false;
    }
    return true;
}
// Slots of every kind carry EQ settings; a revision the EQ lacks designs them as revision 1.
AlgorithmRevision eqRevisionOf(const EffectSettings& e) noexcept
{ return e.algorithmRevision==modernEqAlgorithmRevision?modernEqAlgorithmRevision:legacyAlgorithmRevision; }
double eqPoleRadius(const EqCoefficients& c) noexcept
{
    const double discriminant=c[3]*c[3]-4*c[4];
    if(discriminant<0)return std::sqrt(c[4]);
    const double root=std::sqrt(discriminant);
    return std::max(std::abs(( -c[3]+root)*.5),std::abs((-c[3]-root)*.5));
}
struct Ramp
{
    double value = 0, target = 0, increment = 0;
    int remaining = 0;
    void set(double next, int samples, bool snap = false) noexcept
    {
        if (snap) { value = target = next; increment = 0; remaining = 0; }
        else if (target != next) { target = next; remaining = samples; increment = (target-value)/samples; }
    }
    double next() noexcept
    { if (remaining > 0) { value += increment; if (--remaining == 0) value = target; } return value; }
    double advance(int samples) noexcept
    {
        // Keep each moving addition exact; settled frames have no state changes.
        for(int n=std::min(samples,remaining);n>0;--n)(void)next();
        return value;
    }
};
using Ring = detail::HistoryRing;
using Stereo = std::array<double,2>;
struct ConvolutionRecord
{
    int code=0,frames=0,channels=0;
    double sourceRate=0;
    const std::uint32_t* words=nullptr;
    const char* sha256=nullptr;
};
std::array<ConvolutionRecord,2> canonicalConvolutionRecords() noexcept
{
    static constexpr std::array<std::uint32_t,2> unit {0x3f800000,0x3f800000};
    return {{{0,1,2,detail::convolutionCanonicalRate,unit.data(),detail::convolutionUnitSha256},
             {1,int(detail::convolutionCanonicalFrames),2,detail::convolutionCanonicalRate,detail::convolutionRoomBits.data(),detail::convolutionRoomSha256}}};
}
bool validConvolutionRecord(const ConvolutionRecord& r)
{
    // Only the compiled bank is admitted. Hashing and all String/storage work
    // happen during preparation, never during a render/choice/clear callback.
    if(r.code<0||r.code>1||!r.words||!r.sha256||r.channels!=2||r.sourceRate!=detail::convolutionCanonicalRate
        ||r.frames!=(r.code==0?1:int(detail::convolutionCanonicalFrames)))return false;
    if constexpr(std::endian::native!=std::endian::little)return false;
    const auto expected=r.code==0?detail::convolutionUnitSha256:detail::convolutionRoomSha256;
    if(juce::String(r.sha256)!=expected)return false;
    for(int n=0;n<r.frames*2;++n)if(!std::isfinite(std::bit_cast<float>(r.words[n])))return false;
    return juce::SHA256(r.words,std::size_t(r.frames)*2*sizeof(std::uint32_t)).toHexString().equalsIgnoreCase(expected);
}
struct ConvolutionChoice
{
    static constexpr int headFrames=64;
    std::array<std::array<float,headFrames>,2> headCoefficients{};
    std::array<std::array<float,2*headFrames>,2> headHistory{};
    int headPosition=0;
    bool unitHead=false;
    std::array<int,2> nonFiniteReach{};
    std::unique_ptr<juce::dsp::Convolution> tail;
    std::array<std::array<float,headFrames>,2> tailInput{},tailOutput{};
    int tailIndex=0,irFrames=0;
    bool prepare(juce::AudioBuffer<float> pcm,double rate,juce::dsp::ConvolutionMessageQueue& queue)
    {
        if(pcm.getNumChannels()!=2||pcm.getNumSamples()<1)return false;
        const int count=pcm.getNumSamples();
        for(int ch=0;ch<2;++ch){headCoefficients[ch].fill(0);
            std::copy_n(pcm.getReadPointer(ch),std::min(headFrames,count),headCoefficients[ch].data());}
        unitHead=count==1&&std::bit_cast<std::uint32_t>(headCoefficients[0][0])==0x3f800000u
            &&std::bit_cast<std::uint32_t>(headCoefficients[1][0])==0x3f800000u;
        std::unique_ptr<juce::dsp::Convolution> readyTail;
        if(count>headFrames){juce::AudioBuffer<float> remainder(2,count-headFrames);
            for(int ch=0;ch<2;++ch)remainder.copyFrom(ch,0,pcm,ch,headFrames,count-headFrames);
            readyTail=std::make_unique<juce::dsp::Convolution>(juce::dsp::Convolution::NonUniform{512},queue);
            // Whole64 input blocks are processed only after every frame arrived.
            // The prepared output block is consumed in the following64 frames,
            // restoring the removed IR prefix without a second engine latency.
            readyTail->loadImpulseResponse(std::move(remainder),rate,juce::dsp::Convolution::Stereo::yes,
                juce::dsp::Convolution::Trim::no,juce::dsp::Convolution::Normalise::no);
            readyTail->prepare({rate,headFrames,2});
            if(readyTail->getLatency()!=0||readyTail->getCurrentIRSize()!=count-headFrames)return false;
        }
        tail=std::move(readyTail);irFrames=count;reset();return true;
    }
    void reset() noexcept {for(auto& channel:headHistory)channel.fill(0);headPosition=0;nonFiniteReach.fill(0);if(tail)tail->reset();for(auto& channel:tailInput)channel.fill(0);for(auto& channel:tailOutput)channel.fill(0);tailIndex=0;}
    void process(std::array<float,2>& frame) noexcept
    {
        // Fixed immutable64 taps retain JUCE's ascending float operation order.
        // Mirrored history replaces only its variable circular-loop split.
        std::array<float,2> immediate{};
        for(int ch=0;ch<2;++ch){auto& history=headHistory[ch];history[headPosition]=history[headPosition+headFrames]=frame[ch];
            if(unitHead){const auto exponent=std::bit_cast<std::uint32_t>(frame[ch])&0x7f800000u;
                if(exponent==0x7f800000u)nonFiniteReach[ch]=headFrames;else if(nonFiniteReach[ch]>0)--nonFiniteReach[ch];
                // Keep every exceptional historical zero-product until overwritten.
                // Zero/subnormal current samples retain the original full expression.
                if(exponent!=0&&exponent!=0x7f800000u&&nonFiniteReach[ch]==0){float out=0;out+=history[headPosition]*headCoefficients[ch][0];immediate[ch]=out;continue;}}
            float out=0;for(int k=0;k<headFrames;++k)out+=history[headPosition+k]*headCoefficients[ch][k];immediate[ch]=out;}
        headPosition=headPosition==0?headFrames-1:headPosition-1;
        if(tail){
            // Consume the old slot before a completed block overwrites it.
            for(int ch=0;ch<2;++ch){const float delayed=tailOutput[ch][tailIndex];tailInput[ch][tailIndex]=frame[ch];frame[ch]=immediate[ch]+delayed;}
            if(++tailIndex==headFrames){std::array<const float*,2> input{tailInput[0].data(),tailInput[1].data()};std::array<float*,2> output{tailOutput[0].data(),tailOutput[1].data()};
                juce::dsp::AudioBlock<const float> in(input.data(),2,headFrames);juce::dsp::AudioBlock<float> out(output.data(),2,headFrames);
                juce::dsp::ProcessContextNonReplacing<float> context(in,out);tail->process(context);tailIndex=0;}
        }
        else frame=immediate;
    }
    int getCurrentIRSize() const noexcept {return irFrames;}
    int getLatency() const noexcept {return 0;}
    int getTailLatency() const noexcept {return tail?headFrames:0;}
    int getTailEngineLatency() const noexcept {return tail?tail->getLatency():0;}
    int getTailIRSize() const noexcept {return tail?tail->getCurrentIRSize():0;}
};
struct PreparedConvolution
{
    // The queue outlives both choices. No new IR message is sent after prepare;
    // all FIR/tail histories are ready before publication and callback use.
    juce::dsp::ConvolutionMessageQueue queue {4};
    std::array<std::unique_ptr<ConvolutionChoice>,2> engines;
    juce::dsp::DelayLine<float,juce::dsp::DelayLineInterpolationTypes::Linear> predelay;
    std::array<float,2> frame {};
    double rate=0;
    bool prepare(double fs,const std::array<ConvolutionRecord,2>& records)
    {
        if(!std::isfinite(fs)||fs<8000||fs>768000||records[0].code!=0||records[1].code!=1
            ||!validConvolutionRecord(records[0])||!validConvolutionRecord(records[1]))return false;
        std::array<std::unique_ptr<ConvolutionChoice>,2> ready;
        for(std::size_t choice=0;choice<ready.size();++choice){const auto& r=records[choice];
            juce::AudioBuffer<float> pcm(2,r.frames);for(int ch=0;ch<2;++ch)for(int n=0;n<r.frames;++n)
                pcm.setSample(ch,n,std::bit_cast<float>(r.words[ch*r.frames+n]));
            const double sourceRate=choice==0?fs:r.sourceRate;
            // Match pinned JUCE SRC and its NormaliseOFF rate gain on the WHOLE
            // IR before splitting; the tail never resamples a truncated record.
            if(!juce::approximatelyEqual(sourceRate,fs)){const double ratio=sourceRate/fs;
                juce::AudioBuffer<float> original=pcm;juce::MemoryAudioSource memory(original,false);
                juce::ResamplingAudioSource src(&memory,false,2);const int count=juce::roundToInt(std::max(1.,double(r.frames)/ratio));
                src.setResamplingRatio(ratio);src.prepareToPlay(count,sourceRate);pcm.setSize(2,count);
                src.getNextAudioBlock({&pcm,0,count});}
            pcm.applyGain(float(sourceRate/fs));
            const int expected=choice==0?1:juce::roundToInt(double(r.frames)*fs/r.sourceRate);
            if(pcm.getNumSamples()!=expected)return false;
            for(int ch=0;ch<2;++ch)for(int n=0;n<expected;++n)if(!std::isfinite(pcm.getSample(ch,n)))return false;
            ready[choice]=std::make_unique<ConvolutionChoice>();
            if(!ready[choice]->prepare(std::move(pcm),fs,queue)||ready[choice]->getLatency()!=0)return false;
        }
        predelay.setMaximumDelayInSamples(int(std::ceil(.250*fs))+2);predelay.prepare({fs,1,2});
        engines=std::move(ready);rate=fs;clear();return true;
    }
    void clear() noexcept
    { for(auto& engine:engines)if(engine)engine->reset();predelay.reset();frame={}; }
    Stereo process(Stereo input,int choice,double seconds) noexcept
    {
        const float delaySamples=float(seconds*rate);
        if(std::bit_cast<std::uint32_t>(delaySamples)!=std::bit_cast<std::uint32_t>(predelay.getDelay()))predelay.setDelay(delaySamples);
        for(int ch=0;ch<2;++ch){predelay.pushSample(ch,float(input[ch]));frame[ch]=predelay.popSample(ch);}
        engines[std::size_t(choice)]->process(frame);
        return {double(frame[0]),double(frame[1])};
    }
};

double lowpassCoefficient(double hz, double rate) noexcept
{ return 1-std::exp(-2*std::numbers::pi*std::min(hz,rate*.49)/rate); }
double antiderivative(double x, DriveShape shape) noexcept
{
    const double a = std::abs(x);
    return shape == DriveShape::tanh ? a+std::log1p(std::exp(-2*a))-std::numbers::ln2 : a-std::log1p(a);
}
double waveshape(double x, DriveShape shape) noexcept
{ return shape == DriveShape::tanh ? std::tanh(x) : x/(1+std::abs(x)); }
// Drive revision 2 oversamples 4x through JUCE's minimum-phase polyphase IIR
// halfbands (designIIRLowpassHalfBandPolyphaseAllpassMethod), fixed here so no
// callback designs them: two branches of first-order allpass sections. The 2x
// stage is steep (up .05/-90 dB, down .06/-75 dB); the 4x stage only has to
// clear what lies above the 2x stage's passband (.2/-60 dB both ways).
struct Drive2Stage1Up
{
    static constexpr std::array<double,4> direct {0.045728148016233122,0.33250111173947311,0.66320202241939952,0.93385580374462795};
    static constexpr std::array<double,3> delayed {0.16808754571240941,0.50448575028293263,0.80378086791112258};
};
struct Drive2Stage1Down
{
    static constexpr std::array<double,3> direct {0.054217525755190787,0.38308732729094908,0.74872094443642634};
    static constexpr std::array<double,3> delayed {0.19679796981431272,0.57313641114809111,0.914293709685017};
};
struct Drive2Stage2
{
    static constexpr std::array<double,2> direct {0.081430023176616115,0.70977080010248506};
    static constexpr std::array<double,1> delayed {0.31565984021666094};
};
template<std::size_t N> double allpassCascade(double x, const std::array<double,N>& c, std::array<double,N>& z) noexcept
{ for (std::size_t n = 0; n < N; ++n) { const double y = c[n]*x+z[n]; z[n] = x-c[n]*y; x = y; } return x; }
template<class Coefficients> struct Halfband
{
    std::array<double,Coefficients::direct.size()> direct {};
    std::array<double,Coefficients::delayed.size()> delayed {};
    double held = 0;
    std::array<double,2> up(double x) noexcept
    { return {allpassCascade(x,Coefficients::direct,direct),allpassCascade(x,Coefficients::delayed,delayed)}; }
    double down(double even, double odd) noexcept
    {
        const double y = (held+allpassCascade(even,Coefficients::direct,direct))*.5;
        held = allpassCascade(odd,Coefficients::delayed,delayed); return y;
    }
};
// Samples, at the rate the halfband runs, for each section of its slower
// branch in turn to decay to 1e-6/9.
template<class Coefficients> double halfbandSupportSamples() noexcept
{
    const auto branch = [](const auto& c) { double n = 0; for (double a : c) n += std::log(9e6)/-std::log(a); return n; };
    return std::max(branch(Coefficients::direct),branch(Coefficients::delayed));
}
struct ModernDriveChannel
{
    Halfband<Drive2Stage1Up> up1; Halfband<Drive2Stage2> up2, down2; Halfband<Drive2Stage1Down> down1;
    double previousInput = 0, previousAntiderivative = 0, previousDry = 0, lowpass = 0;
};
// Filter revision 2 runs at four times the rate through the same halfbands.
struct ModernFilterChannel
{ Halfband<Drive2Stage1Up> up1; Halfband<Drive2Stage2> up2, down2; Halfband<Drive2Stage1Down> down1; };
// Revision 2's knee: one full-level voice reaches the drive near 1/45, and
// 1/16 there reaches full scale at the default master level and output gain.
constexpr double drive2Reference = 1.0/16;
// x with (1+x)exp(-x) = exp(-1): two equal one-pole stages of time constant
// tau reach 63.2% of a step at x tau, so each compressor release stage uses
// the release label over x.
constexpr double compressor2ReleaseSpan = 2.1461932;
enum Control : size_t { driveGain, driveTone, driveMix, delayTime, delayFeedback, delayStereo, delayFilter, delayMix,
    reverbSize, reverbDecay, reverbDamping, reverbPredelay, reverbMix,
    compressorThreshold, compressorRatio, compressorAttack, compressorRelease, compressorMakeup, compressorMix,
    delaySaturation, delayLowCut, delayWobble, delayWobbleRate, delayRightRatio, controlCount };
struct Effect
{
    std::array<Ramp,controlCount> controls;
    Ramp tempoWet;
    std::array<Ring,2> delay, predelay;
    std::array<Ring,4> diffusion;
    std::array<Ring,detail::reverb2DelaySeconds.size()> modernDelay;
    std::array<std::array<Ring,4>,2> modernDiffusion;
    std::array<double,detail::reverb2DelaySeconds.size()> modernDamping {}, modernPhase {}, modernFeedback {}, modernSeconds {};
    std::array<double,detail::reverb2DelaySeconds.size()> modernSine {}, modernCosine {}, modernPhaseStep {}, modernStepSine {}, modernStepCosine {};
    unsigned modernPhaseFrames=0;
    std::array<double,3> modernLastControls {-1,-1,-1};
    double modernLowpass=0;
    Stereo previousInput {}, previousDry {}, driveLowpass {}, delayLowpass {}, delaySaturationPrevious {}, delayLowCutEcho {}, delayLowCutLoop {};
    const detail::SincTapTable* delayTaps = nullptr;
    double driveToneKey=std::numeric_limits<double>::quiet_NaN(),driveRateKey=0,driveToneCoefficient=0;
    std::array<ModernDriveChannel,2> modernDrive {};
    double modernDriveToneKey=std::numeric_limits<double>::quiet_NaN(),modernDriveRateKey=0,modernDriveToneCoefficient=0;
    std::array<double,4> damping {};
    juce::dsp::Compressor<double> compressor;
    std::array<double,4> lastCompressor {-999,-999,-999,-999};
    // Revision 2's peak stage and smoothed reduction (dB), and its coefficients
    // with the controls and rate they came from.
    double modernCompressorPeak=0,modernCompressorReduction=0;
    std::array<double,6> modernCompressorKey {std::numeric_limits<double>::quiet_NaN()};
    double modernCompressorThreshold=1,modernCompressorSlope=0,modernCompressorAttack=0,modernCompressorRelease=0,modernCompressorMakeup=1;
    std::array<std::array<Ramp,5>,3> eqControls {};
    std::array<std::array<std::array<double,2>,3>,2> eqHistory {};
    juce::dsp::StateVariableTPTFilter<double> filter;
    std::array<Ramp,4> filterControls {};
    std::array<double,4> filterValues {};
    std::array<double,3> filterKeys {-1,-1,-1};
    double filterDriveGain=1;
    int filterMode=-1;
    juce::dsp::StateVariableTPTFilter<double> modernFilter;
    std::array<ModernFilterChannel,2> modernFilterChannels {};
    std::array<double,2> modernFilterKeys {-1,-1};
    int modernFilterMode=-1;
    juce::dsp::Chorus<double> chorus;
    detail::ModulatedChorus modernChorus;
    ChorusSettings chorusRequested;
    std::array<Ramp,2> chorusControls {};
    std::array<double,2> chorusValues {};
    std::array<double,3> chorusKeys {-1,-1,-2};
    void chorusTargets() noexcept
    {
        if(chorusKeys[0]!=chorusRequested.rateHz){chorus.setRate(chorusRequested.rateHz);modernChorus.setRate(chorusRequested.rateHz);chorusKeys[0]=chorusRequested.rateHz;}
        if(chorusKeys[1]!=chorusRequested.depth){chorus.setDepth(chorusRequested.depth);modernChorus.setDepth(chorusRequested.depth);chorusKeys[1]=chorusRequested.depth;}
        if(chorusKeys[2]!=chorusRequested.feedback){chorus.setFeedback(chorusRequested.feedback);modernChorus.setFeedback(chorusRequested.feedback);chorusKeys[2]=chorusRequested.feedback;}
    }
    juce::dsp::Phaser<double> phaser;
    PhaserSettings phaserRequested;
    std::array<Ramp,2> phaserControls {};
    std::array<double,2> phaserValues {};
    std::array<double,4> phaserKeys {-1,-1,-2,-1};
    void phaserTargets() noexcept
    {
        if(phaserKeys[0]!=phaserRequested.rateHz){phaser.setRate(phaserRequested.rateHz);phaserKeys[0]=phaserRequested.rateHz;}
        if(phaserKeys[1]!=phaserRequested.depth){phaser.setDepth(phaserRequested.depth);phaserKeys[1]=phaserRequested.depth;}
        if(phaserKeys[2]!=phaserRequested.feedback){phaser.setFeedback(phaserRequested.feedback);phaserKeys[2]=phaserRequested.feedback;}
    }
    void phaserCentre(double requested) noexcept
    {
        const double centre=std::clamp(requested,20.,std::min(20000.,.49*rate));
        if(phaserKeys[3]!=centre){phaser.setCentreFrequency(centre);phaserKeys[3]=centre;}
    }
    juce::dsp::DelayLine<double,juce::dsp::DelayLineInterpolationTypes::Linear> flangerDelay;
    std::array<detail::ModulatedTapRing,2> modernFlangerDelay;
    std::array<Ramp,5> flangerControls {};
    std::array<double,5> flangerValues {};
    Stereo flangerPrevious {};
    double flangerPhase=0;
    std::array<Ramp,4> stereoControls {};
    std::array<double,4> stereoValues {};
    double stereoGainKey=std::numeric_limits<double>::quiet_NaN(),stereoGainTarget=1;
    juce::dsp::DelayLine<double,juce::dsp::DelayLineInterpolationTypes::Linear> dimensionDelay;
    detail::ModulatedTapRing modernDimensionDelay;
    std::array<Ramp,4> dimensionControls {};
    std::array<double,4> dimensionValues {};
    double dimensionPhase=0;
    detail::FrequencyShiftPreparation frequencyShiftPrepared;
    std::array<Ramp,3> frequencyShiftControls {};
    std::array<double,3> frequencyShiftValues {};
    std::array<std::array<std::array<double,4>,24>,2> frequencyShiftHistory {};
    Stereo frequencyShiftDelayed {},frequencyShiftPhase {},frequencyShiftSine {},frequencyShiftCosine {1,1};
    Stereo frequencyShiftStepKeys {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()},frequencyShiftStepSine {},frequencyShiftStepCosine {1,1};
    unsigned frequencyShiftFrames=0;
    std::unique_ptr<PreparedConvolution> convolution;
    std::array<Ramp,3> convolutionControls {};
    std::array<double,3> convolutionValues {};
    double convolutionGainKey=std::numeric_limits<double>::quiet_NaN(),convolutionGainTarget=1;
    std::array<Ramp,8> frequencySplitControls {};
    std::array<double,8> frequencySplitValues {};
    std::array<double,3> frequencySplitKeys {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()};
    std::array<double,3> frequencySplitTargets {};
    std::array<Stereo,3> frequencySplitHistory {}; // crossover, low tone, high tone
    // Revision 2, per channel: the input's state-variable split, then the
    // lowpass lowpassed and the highpass highpassed; each holds its two integrators.
    std::array<std::array<std::array<double,2>,3>,2> modernFrequencySplit {};
    double frequencySplitToneA=0;
    std::array<Ramp,7> midSideSplitControls {};
    std::array<double,7> midSideSplitValues {};
    Stereo midSideSplitGainKeys {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()};
    Stereo midSideSplitGainTargets {1,1},midSideSplitHistory {};
    DynamicEq dynamicEq;
    Saturation saturation;
    EraReverb eraReverb;
    std::unique_ptr<SpectralEq> spectralEq;
    std::array<detail::HistoryRing,2> spectralDry;
    Ramp spectralGain;
    EqSettings eqRequested;
    AlgorithmRevision eqRevision=legacyAlgorithmRevision;
    bool eqReady=false;
    double rate = 0, resolvedDelay = .25, delayWobblePhase = 0, delayGlide = .25, delayGlideRight = .25, delayGlideCoefficient = 0;
    bool tempoInitialised = false, delayGlideOn = false, delayGlideReady = false;
    bool prepare(double sampleRate)
    {
        rate = sampleRate;eqReady=false;
        delayGlideCoefficient = 1-std::exp(-1/(.5*rate));
        if(!dynamicEq.prepare(rate)||!saturation.prepare(rate))return false;
        if(!eraReverb.prepare(rate))return false;
        const double toneG=std::tan(std::numbers::pi*1000/rate);frequencySplitToneA=toneG/(1+toneG);
        frequencySplitKeys.fill(std::numeric_limits<double>::quiet_NaN());
        midSideSplitGainKeys.fill(std::numeric_limits<double>::quiet_NaN());
        // Revision 2's sinc reads past the 4-s maximum; revision 1 never reaches the spare frames.
        for (auto& ring : delay) ring.prepare(static_cast<size_t>(std::ceil(rate*4))+detail::SincTapTable::points);
        delayTaps = &detail::SincTapTable::shared();
        for (auto& ring : predelay) ring.prepare(static_cast<size_t>(std::ceil(rate*.25)));
        for (auto& ring : diffusion) ring.prepare(static_cast<size_t>(std::ceil(rate*.1)));
        for (auto& ring : modernDelay) ring.prepare(static_cast<size_t>(std::ceil(rate*.2)));
        for (auto& channel : modernDiffusion) for(auto& ring:channel) ring.prepare(static_cast<size_t>(std::ceil(rate*.017)));
        for(size_t i=0;i<modernPhaseStep.size();++i) {
            modernPhaseStep[i]=2*std::numbers::pi*(.137+.017*double(i))/rate;
            modernStepSine[i]=std::sin(modernPhaseStep[i]); modernStepCosine[i]=std::cos(modernPhaseStep[i]);
        }
        compressor.prepare({rate,1,1});
        filter.prepare({rate,1,2});filterKeys={-1,-1,-1};filterMode=-1;
        modernFilter.prepare({4*rate,1,2});modernFilterChannels={};modernFilterKeys={-1,-1};modernFilterMode=-1;
        // The stock widget owns prepared stereo history and its active-frame
        // LFO/50-ms targets. Its mixer is fully wet; our external mix is the
        // only public dry/wet law. All allocation is in prepare.
        chorus.setMix(1);chorus.prepare({rate,1,2});chorusKeys={-1,-1,-2};
        // Admitted centres reach 40 ms and modulation adds up to 10 ms.
        modernChorus.prepare(rate,50);
        // Prepare first: stock centre normalization depends on the effective
        // rate and is deliberately republished after that rate is installed.
        phaser.setMix(1);phaser.prepare({rate,1,2});phaserKeys={-1,-1,-2,-1};
        phaserTargets();phaserCentre(phaserRequested.centreHz);phaser.reset();
        flangerDelay.setMaximumDelayInSamples(static_cast<int>(std::ceil(.014*rate))+2);
        flangerDelay.prepare({rate,1,2});
        for(auto& ring:modernFlangerDelay)ring.prepare(static_cast<size_t>(std::ceil(.014*rate))+2);
        dimensionDelay.setMaximumDelayInSamples(static_cast<int>(std::ceil(.016*rate))+2);
        dimensionDelay.prepare({rate,1,1});
        modernDimensionDelay.prepare(static_cast<size_t>(std::ceil(.016*rate))+2);
        auto ready=std::make_unique<PreparedConvolution>();
        if(!ready->prepare(rate,canonicalConvolutionRecords()))return false;
        convolution=std::move(ready);return true;
    }
    void clear() noexcept
    {
        dynamicEq.reset();
        saturation.reset();
        eraReverb.reset();
        if(spectralEq){spectralEq->reset();for(auto& ring:spectralDry)ring.clear();}
        for (auto& ring : delay) ring.clear();
        for (auto& ring : predelay) ring.clear();
        for (auto& ring : diffusion) ring.clear();
        for (auto& ring : modernDelay) ring.clear();
        for (auto& channel : modernDiffusion) for(auto& ring:channel) ring.clear();
        modernDamping={}; modernLastControls={-1,-1,-1};
        modernPhaseFrames=0;
        for(size_t i=0;i<modernPhase.size();++i) {
            modernPhase[i]=2*std::numbers::pi*double(i)/modernPhase.size();
            modernSine[i]=std::sin(modernPhase[i]); modernCosine[i]=std::cos(modernPhase[i]);
        }
        previousInput = previousDry = driveLowpass = delayLowpass = delaySaturationPrevious = delayLowCutEcho = delayLowCutLoop = {};
        delayWobblePhase = 0; delayGlideReady = false;
        modernDrive = {};
        damping = {}; compressor.reset(); modernCompressorPeak = modernCompressorReduction = 0; tempoInitialised = false;eqHistory={};
        filter.reset();filterKeys={-1,-1,-1};filterMode=-1;
        modernFilter.reset();modernFilterChannels={};modernFilterKeys={-1,-1};modernFilterMode=-1;
        chorusKeys={-1,-1,-2};chorusTargets();chorus.reset();modernChorus.reset();
        phaserKeys={-1,-1,-2,-1};phaserTargets();if(rate>0)phaserCentre(phaserControls[0].value);phaser.reset();
        flangerDelay.reset();for(auto& ring:modernFlangerDelay)ring.clear();flangerPrevious={};flangerPhase=0;
        // Stop reaches clear directly. Snap every owning target here, rather
        // than depending on reset's later settings(...,snap=true) call.
        for(size_t i=0;i<flangerControls.size();++i){auto& ramp=flangerControls[i];ramp.set(ramp.target,1,true);flangerValues[i]=ramp.value;}
        for(size_t i=0;i<stereoControls.size();++i){auto& ramp=stereoControls[i];ramp.set(ramp.target,1,true);stereoValues[i]=ramp.value;}
        frequencyShiftHistory={};frequencyShiftDelayed={};frequencyShiftPhase={};frequencyShiftSine={};frequencyShiftCosine={1,1};frequencyShiftFrames=0;
        frequencyShiftStepKeys={std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()};
        for(size_t i=0;i<frequencyShiftControls.size();++i){auto& ramp=frequencyShiftControls[i];ramp.set(ramp.target,1,true);frequencyShiftValues[i]=ramp.value;}
        frequencySplitHistory={};modernFrequencySplit={};
        for(size_t i=0;i<frequencySplitControls.size();++i){auto& ramp=frequencySplitControls[i];ramp.set(ramp.target,1,true);frequencySplitValues[i]=ramp.value;}
        midSideSplitHistory={};
        for(size_t i=0;i<midSideSplitControls.size();++i){auto& ramp=midSideSplitControls[i];ramp.set(ramp.target,1,true);midSideSplitValues[i]=ramp.value;}
        if(convolution)convolution->clear();
        for(size_t i=0;i<convolutionControls.size();++i){auto& ramp=convolutionControls[i];ramp.set(ramp.target,1,true);convolutionValues[i]=ramp.value;}
        dimensionDelay.reset();modernDimensionDelay.clear();dimensionPhase=0;
        for(size_t i=0;i<dimensionControls.size();++i){auto& ramp=dimensionControls[i];ramp.set(ramp.target,1,true);dimensionValues[i]=ramp.value;}
    }
    void settings(const EffectSettings& e, int samples, bool snap) noexcept
    {
        auto ordinary=e.dynamicEq;
        if(spectralEq){
            auto spectral=e.dynamicEq;
            // Keep the frequency-region audition available without applying the band's gain twice.
            for(auto& band:ordinary.bands)if(band.spectralEnabled){band.gainDb=0;band.rangeDb=0;}
            ordinary.outputGainDb=0;ordinary.autoGain=false;
            spectral.outputGainDb=0;spectral.autoGain=false;
            (void)spectralEq->setSettings(spectral);
            spectralGain.set(std::pow(10.,double(e.dynamicEq.outputGainDb+dynamicEqAutoGainDb(e.dynamicEq))/20),samples,snap);
        }
        dynamicEq.setSettings(ordinary);
        if(snap)dynamicEq.reset();
        (void)saturation.setSettings(e.saturation);
        if(snap)saturation.reset();
        (void)eraReverb.setSettings(e.eraReverb);
        if(snap)eraReverb.reset();
        const auto set = [&](Control i, double value) { controls[i].set(value,samples,snap); };
        set(driveGain,e.drive.gainDb); set(driveTone,e.drive.toneHz); set(driveMix,e.drive.mix);
        if (!e.delay.synced) { resolvedDelay = e.delay.seconds; set(delayTime,resolvedDelay); }
        else if (snap) set(delayTime,resolvedDelay);
        set(delayFeedback,e.delay.feedback); set(delayStereo,e.delay.stereo); set(delayFilter,e.delay.filterHz); set(delayMix,e.delay.mix);
        set(delaySaturation,e.delay.saturation); set(delayLowCut,e.delay.lowCutHz); set(delayWobble,e.delay.wobble); set(delayWobbleRate,e.delay.wobbleRateHz);
        set(delayRightRatio,e.delay.rightRatio);
        delayGlideOn = e.delay.glide; if (snap) delayGlideReady = false;
        set(reverbSize,e.reverb.size); set(reverbDecay,e.reverb.decaySeconds); set(reverbDamping,e.reverb.damping);
        set(reverbPredelay,e.reverb.predelayMs); set(reverbMix,e.reverb.mix);
        set(compressorThreshold,e.compressor.thresholdDb); set(compressorRatio,e.compressor.ratio);
        set(compressorAttack,e.compressor.attackMs); set(compressorRelease,e.compressor.releaseMs);
        set(compressorMakeup,e.compressor.makeupDb); set(compressorMix,e.compressor.mix);
        const std::array<double,5> flanger{e.flanger.rateHz,e.flanger.depth,e.flanger.baseDelayMs,e.flanger.feedback,e.flanger.mix};
        for(size_t i=0;i<flanger.size();++i)flangerControls[i].set(flanger[i],samples,snap);
        if(stereoGainKey!=e.stereo.gainDb){stereoGainKey=e.stereo.gainDb;stereoGainTarget=std::pow(10.,stereoGainKey/20);}
        const double q=e.stereo.channelMode==StereoChannelMode::stereo?1:e.stereo.channelMode==StereoChannelMode::swap?-1:0;
        const std::array<double,4> stereo{stereoGainTarget,e.stereo.pan,e.stereo.width,q};
        for(size_t i=0;i<stereo.size();++i)stereoControls[i].set(stereo[i],samples,snap);
        const std::array<double,3> shift{e.frequencyShift.hz,e.frequencyShift.stereoOffsetHz,e.frequencyShift.mix};
        for(size_t i=0;i<shift.size();++i)frequencyShiftControls[i].set(shift[i],samples,snap);
        const std::array<double,4> dimension{e.dimension.depth,e.dimension.rateHz,e.dimension.width,e.dimension.mix};
        for(size_t i=0;i<dimension.size();++i)dimensionControls[i].set(dimension[i],samples,snap);
        if(convolutionGainKey!=e.convolution.wetTrimDb){convolutionGainKey=e.convolution.wetTrimDb;convolutionGainTarget=std::pow(10.,convolutionGainKey/20);}
        const std::array<double,3> cv{double(e.convolution.predelayMs)*.001,convolutionGainTarget,e.convolution.mix};
        for(size_t i=0;i<cv.size();++i)convolutionControls[i].set(cv[i],samples,snap);
        const auto& split=e.frequencySplit;const std::array<double,3> keys{split.crossoverHz,split.lowGainDb,split.highGainDb};
        for(size_t i=0;i<keys.size();++i)if(frequencySplitKeys[i]!=keys[i]){
            frequencySplitKeys[i]=keys[i];
            if(i==0){const double fs=rate>0?rate:48000.,g=std::tan(std::numbers::pi*std::min(keys[i],.45*fs)/fs);frequencySplitTargets[i]=g/(1+g);}
            else frequencySplitTargets[i]=std::pow(10.,keys[i]/20);
        }
        const std::array<double,8> sv{frequencySplitTargets[0],frequencySplitTargets[1],split.lowMuted?0.:1.,split.lowTone,frequencySplitTargets[2],split.highMuted?0.:1.,split.highTone,split.mix};
        for(size_t i=0;i<sv.size();++i)frequencySplitControls[i].set(sv[i],samples,snap);
        const auto& ms=e.midSideSplit;const Stereo gainKeys{ms.midGainDb,ms.sideGainDb};
        for(size_t i=0;i<gainKeys.size();++i)if(midSideSplitGainKeys[i]!=gainKeys[i]){midSideSplitGainKeys[i]=gainKeys[i];midSideSplitGainTargets[i]=std::pow(10.,gainKeys[i]/20);}
        const std::array<double,7> mv{midSideSplitGainTargets[0],ms.midMuted?0.:1.,ms.midTone,midSideSplitGainTargets[1],ms.sideMuted?0.:1.,ms.sideTone,ms.mix};
        for(size_t i=0;i<mv.size();++i)midSideSplitControls[i].set(mv[i],samples,snap);
        phaserRequested=e.phaser;
        phaserControls[0].set(e.phaser.centreHz,samples,snap);
        phaserControls[1].set(e.phaser.mix,samples,snap);
        if(snap&&rate>0){phaserKeys={-1,-1,-2,-1};phaserTargets();phaserCentre(e.phaser.centreHz);phaser.reset();}
        chorusRequested=e.chorus;
        chorusControls[0].set(e.chorus.baseDelayMs,samples,snap);
        chorusControls[1].set(e.chorus.mix,samples,snap);
        if(snap&&rate>0){chorusKeys={-1,-1,-2};chorusTargets();chorus.setCentreDelay(e.chorus.baseDelayMs);chorus.reset();
            modernChorus.setCentreDelay(e.chorus.baseDelayMs);modernChorus.reset();}
        filterControls[0].set(e.filter.cutoffHz,samples,snap);
        filterControls[1].set(e.filter.resonance,samples,snap);
        filterControls[2].set(e.filter.driveDb,samples,snap);
        filterControls[3].set(e.filter.mix,samples,snap);
        const auto revision=eqRevisionOf(e);
        if(rate>0&&(snap||!eqReady||e.eq!=eqRequested||revision!=eqRevision)){
            std::array<EqCoefficients,3> coefficients;
            if(eqCoefficients(e.eq,rate,revision,coefficients)){
                for(size_t band=0;band<3;++band)for(size_t i=0;i<5;++i)
                    eqControls[band][i].set(coefficients[band][i],samples,snap||!eqReady);
                eqRequested=e.eq;eqRevision=revision;eqReady=true;
            }
        }
    }
    void tempo(const EffectSettings& e, std::optional<double> bpm, int samples) noexcept
    {
        const bool available = !e.delay.synced || bpm.has_value();
        if (available)
        {
            // Beats keeps its float product of `delay.beats`; a note's period is held at the line's 4 s.
            resolvedDelay = !e.delay.synced ? e.delay.seconds
                : std::min(4., e.delay.note == 0 ? 60*e.delay.beats / *bpm
                    : 60*detail::delayNoteBeats[static_cast<std::size_t>(e.delay.note)] / *bpm);
            controls[delayTime].set(resolvedDelay,samples,!tempoInitialised);
        }
        // Missing tempo never guesses a time. Feedback keeps its last resolved
        // time; the caller stops admitting fresh wet input immediately.
        tempoWet.set(available ? 1 : 0,samples,!tempoInitialised); tempoInitialised = true;
    }
    Stereo modernReverb(Stereo excitation,const std::array<double,controlCount>& c) noexcept
    {
        constexpr auto count=detail::reverb2DelaySeconds.size();
        const double normalization=1/std::sqrt(double(count));
        const std::array<double,3> parameters{c[reverbSize],c[reverbDecay],c[reverbDamping]};
        if(parameters!=modernLastControls) {
            modernLastControls=parameters;
            modernLowpass=lowpassCoefficient(20000*std::pow(.025,c[reverbDamping]),rate);
            for(size_t i=0;i<count;++i) {
                modernSeconds[i]=detail::reverb2DelaySeconds[i]*(.5+1.5*c[reverbSize]);
                modernFeedback[i]=std::exp(-std::log(1000.)*modernSeconds[i]/c[reverbDecay]);
            }
        }
        for(size_t ch=0;ch<2;++ch) for(size_t stage=0;stage<4;++stage) {
            auto& ring=modernDiffusion[ch][stage];
            const double delayed=ring.read(detail::reverb2AllpassSeconds[ch][stage]*(.75+.5*c[reverbSize])*rate);
            const double next=delayed-detail::reverb2AllpassFeedback*excitation[ch];
            ring.write(excitation[ch]+detail::reverb2AllpassFeedback*next); excitation[ch]=next;
        }
        std::array<double,count> values,mixed;
        for(size_t i=0;i<count;++i) {
            const double seconds=modernSeconds[i]+detail::reverb2ModulationSeconds*modernSine[i];
            modernDamping[i]+=modernLowpass*(modernDelay[i].read(seconds*rate)-modernDamping[i]);
            values[i]=modernDamping[i];
            const double sine=modernSine[i],cosine=modernCosine[i];
            modernSine[i]=sine*modernStepCosine[i]+cosine*modernStepSine[i];
            modernCosine[i]=cosine*modernStepCosine[i]-sine*modernStepSine[i];
            modernPhase[i]+=modernPhaseStep[i];
            if(modernPhase[i]>=2*std::numbers::pi) modernPhase[i]-=2*std::numbers::pi;
        }
        // Internal-frame cadence is independent of host partitions. The
        // canonical phase bounds recurrent roundoff without changing clocks.
        if(++modernPhaseFrames==256) {
            modernPhaseFrames=0;
            for(size_t i=0;i<count;++i) {
                modernSine[i]=std::sin(modernPhase[i]); modernCosine[i]=std::cos(modernPhase[i]);
            }
        }
        mixed=values;
        // In-place normalized Hadamard: fixed orthonormal feedback mixing.
        for(size_t stride=1;stride<count;stride*=2) for(size_t start=0;start<count;start+=2*stride)
            for(size_t j=0;j<stride;++j) {
                const auto a=mixed[start+j],b=mixed[start+j+stride];
                mixed[start+j]=a+b; mixed[start+j+stride]=a-b;
            }
        Stereo wet{};
        for(size_t i=0;i<count;++i) {
            const double sign=i%2?-1.:1.;
            modernDelay[i].write(.25*(excitation[0]+sign*excitation[1])+normalization*mixed[i]*modernFeedback[i]);
            wet[0]+=normalization*values[i]; wet[1]+=normalization*sign*values[i];
        }
        return wet;
    }
    // Advance once per valid internal frame even while another kind or bypass
    // owns this retained bank. Invalid/empty renders never enter this clock.
    void advanceFilter() noexcept
    {
        for(size_t i=0;i<filterValues.size();++i)filterValues[i]=filterControls[i].next();
        // Centre and public mix retain their physical five-ms clocks while
        // inactive/bypassed. Widget audio/LFO history resumes through clear.
        for(size_t i=0;i<chorusValues.size();++i)chorusValues[i]=chorusControls[i].next();
        for(size_t i=0;i<phaserValues.size();++i)phaserValues[i]=phaserControls[i].next();
        for(size_t i=0;i<flangerValues.size();++i)flangerValues[i]=flangerControls[i].next();
        for(size_t i=0;i<stereoValues.size();++i)stereoValues[i]=stereoControls[i].next();
        for(size_t i=0;i<dimensionValues.size();++i)dimensionValues[i]=dimensionControls[i].next();
        for(size_t i=0;i<frequencyShiftValues.size();++i)frequencyShiftValues[i]=frequencyShiftControls[i].next();
        for(size_t i=0;i<convolutionValues.size();++i)convolutionValues[i]=convolutionControls[i].next();
        // Split controls evaluate the current value, then advance one internal frame.
        for(size_t i=0;i<frequencySplitValues.size();++i){frequencySplitValues[i]=frequencySplitControls[i].value;frequencySplitControls[i].next();}
        for(size_t i=0;i<midSideSplitValues.size();++i){midSideSplitValues[i]=midSideSplitControls[i].value;midSideSplitControls[i].next();}
    }
    // Same retained banks and pre/post convention as advanceFilter(), for a
    // block that never reads their intermediate values.
    void advanceFilterBlock(int frames) noexcept
    {
        if(frames<=0)return;
        const auto bank=[frames](auto& controls,auto& values,bool before=false){
            for(size_t i=0;i<controls.size();++i){
                values[i]=controls[i].advance(frames-(before?1:0));
                if(before)(void)controls[i].next();
            }
        };
        bank(filterControls,filterValues);bank(chorusControls,chorusValues);
        bank(phaserControls,phaserValues);bank(flangerControls,flangerValues);
        bank(stereoControls,stereoValues);bank(dimensionControls,dimensionValues);
        bank(frequencyShiftControls,frequencyShiftValues);bank(convolutionControls,convolutionValues);
        bank(frequencySplitControls,frequencySplitValues,true);bank(midSideSplitControls,midSideSplitValues,true);
    }
    // Shaper, tone and dry/wet mix all run at four times the rate, so the
    // decimator removes both the shaper's aliases and the averaged dry's droop
    // equally. The previous antiderivative is kept rather than recomputed.
    Stereo modernDriveFrame(Stereo input, const std::array<double,controlCount>& c, DriveShape shape) noexcept
    {
        const double fast = 4*rate;
        if (modernDriveToneKey != c[driveTone] || modernDriveRateKey != fast)
        { modernDriveToneKey = c[driveTone]; modernDriveRateKey = fast; modernDriveToneCoefficient = lowpassCoefficient(modernDriveToneKey,fast); }
        const double gain = std::pow(10.0,c[driveGain]/20)/drive2Reference, mix = c[driveMix], a = modernDriveToneCoefficient;
        Stereo output {};
        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto& s = modernDrive[ch];
            const auto oversampled = [&](double u)
            {
                const double x = u*gain, old = s.previousInput, delta = x-old, integral = antiderivative(x,shape);
                const double shaped = std::abs(delta) < 1e-6*(1+std::max(std::abs(x),std::abs(old)))
                    ? waveshape((x+old)*.5,shape) : (integral-s.previousAntiderivative)/delta;
                s.lowpass += a*(shaped-s.lowpass);
                const double dry = (u+s.previousDry)*.5;
                s.previousInput = x; s.previousAntiderivative = integral; s.previousDry = u;
                return (1-mix)*dry+mix*s.lowpass*drive2Reference;
            };
            const auto half = s.up1.up(input[ch]);
            const auto first = s.up2.up(half[0]), second = s.up2.up(half[1]);
            const double e0 = oversampled(first[0]), o0 = oversampled(first[1]);
            const double e1 = oversampled(second[0]), o1 = oversampled(second[1]);
            const double even = s.down2.down(e0,o0);
            output[ch] = s.down1.down(even,s.down2.down(e1,o1));
        }
        return output;
    }
    // Pre-drive, state-variable filter and mix all run at four times the rate:
    // the decimator removes the pre-drive's aliases, the filter follows its
    // analog prototype up to the clamped corner, and dry and wet share one phase.
    Stereo modernFilterFrame(Stereo input, double cutoff, double q, double drive, double mix, FilterFxMode mode) noexcept
    {
        if(cutoff!=modernFilterKeys[0]){modernFilter.setCutoffFrequency(cutoff);modernFilterKeys[0]=cutoff;}
        if(q!=modernFilterKeys[1]){modernFilter.setResonance(q);modernFilterKeys[1]=q;}
        if(drive!=filterKeys[2]){filterDriveGain=std::pow(10.,drive/20);filterKeys[2]=drive;}
        if(int(mode)!=modernFilterMode){modernFilter.setType(mode==FilterFxMode::lowpass?juce::dsp::StateVariableTPTFilterType::lowpass:
            mode==FilterFxMode::bandpass?juce::dsp::StateVariableTPTFilterType::bandpass:juce::dsp::StateVariableTPTFilterType::highpass);modernFilterMode=int(mode);}
        Stereo output {};
        for(size_t ch=0;ch<2;++ch){
            auto& s=modernFilterChannels[ch];
            const auto oversampled=[&](double x){
                const double excitation=drive==0?x:x+(drive/24)*(std::tanh(filterDriveGain*x)/filterDriveGain-x);
                double wet=modernFilter.processSample(int(ch),excitation);
                if(mode==FilterFxMode::bandpass)wet*=1/q;
                return (1-mix)*x+mix*wet;
            };
            const auto half=s.up1.up(input[ch]);
            const auto first=s.up2.up(half[0]),second=s.up2.up(half[1]);
            const double e0=oversampled(first[0]),o0=oversampled(first[1]);
            const double e1=oversampled(second[0]),o1=oversampled(second[1]);
            const double even=s.down2.down(e0,o0);
            output[ch]=s.down1.down(even,s.down2.down(e1,o1));
        }
        return output;
    }
    // Compressor revision 2 smooths its hard-knee gain reduction in decibels,
    // from one max-absolute detector linking both channels. A peak stage holds
    // each cycle's reduction and releases toward the current one; the smoother
    // attacks toward the peak stage and releases with it, so a level step moves
    // the reduction 63.2% of the way within the attack or release label.
    Stereo modernCompressorFrame(Stereo input, const std::array<double,controlCount>& c) noexcept
    {
        const std::array<double,6> key {c[compressorThreshold],c[compressorRatio],c[compressorAttack],c[compressorRelease],c[compressorMakeup],rate};
        if (key != modernCompressorKey)
        {
            modernCompressorKey = key;
            modernCompressorThreshold = std::pow(10.0,key[0]/20); modernCompressorSlope = 1-1/key[1];
            modernCompressorAttack = std::exp(-1000/(rate*key[2]));
            modernCompressorRelease = std::exp(-1000*compressor2ReleaseSpan/(rate*key[3]));
            modernCompressorMakeup = std::pow(10.0,key[4]/20);
        }
        const double detector = std::max(std::abs(input[0]),std::abs(input[1]));
        const double target = detector > modernCompressorThreshold ? 20*std::log10(detector/modernCompressorThreshold)*modernCompressorSlope : 0;
        double& peak = modernCompressorPeak; double& reduction = modernCompressorReduction;
        peak = std::max(target,target+modernCompressorRelease*(peak-target));
        reduction = peak+(peak > reduction ? modernCompressorAttack : modernCompressorRelease)*(reduction-peak);
        // Reductions below 1e-9 dB are inaudible; flushing them keeps the decays out of denormals.
        if (peak < 1e-9) peak = 0;
        if (reduction < 1e-9) reduction = 0;
        const double gain = (reduction > 0 ? std::pow(10.0,-reduction/20) : 1)*modernCompressorMakeup;
        return {input[0]*gain,input[1]*gain};
    }
    Stereo process(Stereo input, EffectKind kind, AlgorithmRevision revision, DriveShape shape, bool admitWet, FilterFxMode mode, ConvolutionIr ir=ConvolutionIr::shortRoom, bool pingPong=false, Stereo external={}) noexcept
    {
        std::array<double,controlCount> c;
        for (size_t i = 0; i < c.size(); ++i) c[i] = controls[i].next();
        Stereo wet {}; double mix = 0;
        switch (kind)
        {
            case EffectKind::saturation: return saturation.process(input);
            case EffectKind::spaceReverb: return input; // Dedicated block owner below.
            case EffectKind::eraReverb: return eraReverb.process(input);
            case EffectKind::dynamicEq: {
                input=dynamicEq.process(input,external);
                if(spectralEq){spectralEq->processBlock(std::span(&input,1));const auto gain=spectralGain.next();for(auto& value:input)value*=gain;}
                return input;
            }
            case EffectKind::midSideSplit:
            {
                constexpr double q=0x1.6a09e667f3bcdp-1;
                const auto& s=midSideSplitValues;mix=s[6];
                const Stereo branch{(input[0]+input[1])*q,(input[0]-input[1])*q};Stereo colored{};
                for(size_t b=0;b<2;++b){
                    const double v=frequencySplitToneA*(branch[b]-midSideSplitHistory[b]),low=v+midSideSplitHistory[b];midSideSplitHistory[b]=low+v;
                    const double tone=s[3*b+2];colored[b]=(tone==0?branch[b]:branch[b]+tone*(branch[b]-2*low))*s[3*b]*s[3*b+1];
                }
                wet={(colored[0]+colored[1])*q,(colored[0]-colored[1])*q};
                // Clock both histories first, preserving exact original signed zeros.
                if(mix==0||(s[0]==1&&s[1]==1&&s[2]==0&&s[3]==1&&s[4]==1&&s[5]==0))return input;
                break;
            }
            case EffectKind::frequencySplit:
            {
                const auto& s=frequencySplitValues;mix=s[7];
                const auto lowpass=[](double x,double a,double& state){const double v=a*(x-state),low=v+state;state=low+v;return low;};
                if(revision==modernFrequencySplitAlgorithmRevision){
                    // Each band is a Butterworth section squared, so the bands sum to an
                    // allpass. That sum is the dry path: neutral bands and every mix stay flat.
                    const double g=s[0]/(1-s[0]),k=std::numbers::sqrt2,a1=1/(1+g*(g+k)),a2=g*a1,a3=g*a2;
                    const auto section=[&](double x,std::array<double,2>& z,double& high){
                        const double v3=x-z[1],v1=a1*z[0]+a2*v3,v2=z[1]+a2*z[0]+a3*v3;
                        z[0]=2*v1-z[0];z[1]=2*v2-z[1];high=x-k*v1-v2;return v2;};
                    Stereo output {};
                    for(size_t ch=0;ch<2;++ch){
                        auto& z=modernFrequencySplit[ch];double high=0,unused=0,highBand=0;
                        const double low=section(section(input[ch],z[0],high),z[1],unused);
                        section(high,z[2],highBand);
                        const double lowTone=lowpass(low,frequencySplitToneA,frequencySplitHistory[1][ch]);
                        const double highTone=lowpass(highBand,frequencySplitToneA,frequencySplitHistory[2][ch]);
                        const double wet=(low+s[3]*(low-2*lowTone))*s[1]*s[2]+(highBand+s[6]*(highBand-2*highTone))*s[4]*s[5];
                        output[ch]=(1-mix)*(low+highBand)+mix*wet;
                    }
                    return output;
                }
                for(size_t ch=0;ch<2;++ch){
                    const double low=lowpass(input[ch],s[0],frequencySplitHistory[0][ch]),high=input[ch]-low;
                    const double lowTone=lowpass(low,frequencySplitToneA,frequencySplitHistory[1][ch]);
                    const double highTone=lowpass(high,frequencySplitToneA,frequencySplitHistory[2][ch]);
                    const double coloredLow=s[3]==0?low:low+s[3]*(low-2*lowTone);
                    const double coloredHigh=s[6]==0?high:high+s[6]*(high-2*highTone);
                    wet[ch]=coloredLow*s[1]*s[2]+coloredHigh*s[4]*s[5];
                }
                // Preserve exact original signed zeros after all six histories clock.
                if(mix==0||(s[1]==1&&s[2]==1&&s[3]==0&&s[4]==1&&s[5]==1&&s[6]==0))return input;
                break;
            }
            case EffectKind::convolution:
            {
                wet=convolution->process(input,int(ir),convolutionValues[0]);
                // Zero public mix still clocks the active convolution and tap.
                mix=convolutionValues[2];if(mix==0)return input;
                for(auto& sample:wet)sample*=convolutionValues[1];break;
            }
            case EffectKind::drive:
            {
                if(revision==modernDriveAlgorithmRevision) return modernDriveFrame(input,c,shape);
                if(driveToneKey!=c[driveTone]||driveRateKey!=rate){
                    driveToneKey=c[driveTone];driveRateKey=rate;driveToneCoefficient=lowpassCoefficient(driveToneKey,driveRateKey);
                }
                mix = c[driveMix]; const double gain = std::pow(10.0,c[driveGain]/20), a = driveToneCoefficient;
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    const double x = input[ch]*gain, old = previousInput[ch], delta = x-old;
                    const double shaped = std::abs(delta) < 1e-6*(1+std::max(std::abs(x),std::abs(old)))
                        ? waveshape((x+old)*.5,shape) : (antiderivative(x,shape)-antiderivative(old,shape))/delta;
                    driveLowpass[ch] += a*(shaped-driveLowpass[ch]); wet[ch] = driveLowpass[ch];
                    const auto dry = input[ch]; input[ch] = (dry+previousDry[ch])*.5;
                    previousDry[ch] = dry; previousInput[ch] = x;
                }
                break;
            }
            case EffectKind::delay:
            {
                const double available = tempoWet.next(); mix = c[delayMix]*available;
                const double a = lowpassCoefficient(c[delayFilter],rate);
                const bool sinc = revision == modernDelayAlgorithmRevision;
                // The right side reads at TIME * RIGHT, held in 1 ms to 4 s. Glide follows
                // each side's time on its own 0.5 s one-pole, like a transport changing speed;
                // both start at their time after a clear, a structural change or preparation.
                const double timeLeft = c[delayTime];
                std::array<double,2> time { timeLeft, std::clamp(timeLeft*c[delayRightRatio],.001,4.) };
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    double& glide = ch ? delayGlideRight : delayGlide;
                    if (!delayGlideOn || !delayGlideReady) glide = time[ch];
                    else { glide += (time[ch]-glide)*delayGlideCoefficient; time[ch] = glide; }
                }
                delayGlideReady = true;
                // Wobble swings each side's read either side of its time on one shared sine,
                // only as far as that side's 1 ms to 4 s range allows.
                if (c[delayWobble] > 0)
                    for (auto& t : time)
                    {
                        const double swing = std::min({.001*c[delayWobble],t-.001,4-t});
                        if (swing > 0) t += swing*std::sin(delayWobblePhase);
                    }
                delayWobblePhase += 2*std::numbers::pi*c[delayWobbleRate]/rate;
                if (delayWobblePhase >= 2*std::numbers::pi) delayWobblePhase -= 2*std::numbers::pi;
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    const double read = sinc ? delay[ch].read(time[ch]*rate,*delayTaps) : delay[ch].read(time[ch]*rate);
                    delayLowpass[ch] += a*(read-delayLowpass[ch]); wet[ch] = delayLowpass[ch];
                }
                // The low cut is two equal bilinear one-pole high-passes, on the echo and
                // inside the loop, so the first repeat is 3 dB down at the key's value.
                const bool lowCut = c[delayLowCut] > 20;
                double cut = 0;
                if (lowCut) { const double g = std::tan(std::numbers::pi*std::min(c[delayLowCut]/detail::delayLowCutCornerRatio,.49*rate)/rate); cut = g/(1+g); }
                const auto highpass = [cut](double x, double& state) { const double v = cut*(x-state), low = v+state; state = low+v; return x-low; };
                Stereo echo = wet;
                if (lowCut) for (size_t ch = 0; ch < 2; ++ch) echo[ch] = highpass(wet[ch],delayLowCutEcho[ch]);
                else delayLowCutEcho = delayLowCutLoop = {};
                // Saturation bends the write under a ceiling referenced like revision 2's
                // drive, through the tanh antiderivative; at 0 the write is untouched.
                const double saturation = c[delaySaturation];
                const double ceiling = saturation > 0 ? drive2Reference*std::pow(10.0,(16-24*saturation)/20) : 1;
                // Ping-pong writes the mono sum into the left line only; the right line
                // takes the left's read and REPEATS feeds the right's back to the left,
                // so each hop crosses sides and CROSS has nothing to mix.
                Stereo writes;
                if (pingPong)
                {
                    constexpr double q = 0x1.6a09e667f3bcdp-1;
                    const double sum = admitWet ? (input[0]+input[1])*q*available : 0;
                    writes = { sum+c[delayFeedback]*wet[1], wet[0] };
                }
                else
                    for (size_t ch = 0; ch < 2; ++ch)
                        writes[ch] = (admitWet ? input[ch]*available : 0)
                            + c[delayFeedback]*((1-c[delayStereo])*wet[ch]+c[delayStereo]*wet[1-ch]);
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    double w = writes[ch];
                    if (lowCut) w = highpass(w,delayLowCutLoop[ch]);
                    const double old = delaySaturationPrevious[ch]; delaySaturationPrevious[ch] = w;
                    if (saturation > 0)
                    {
                        const double x = w/ceiling, x0 = old/ceiling, delta = x-x0;
                        w = ceiling*(std::abs(delta) < 1e-6*(1+std::max(std::abs(x),std::abs(x0)))
                            ? std::tanh((x+x0)*.5) : (antiderivative(x,DriveShape::tanh)-antiderivative(x0,DriveShape::tanh))/delta);
                    }
                    delay[ch].write(w);
                }
                wet = echo;
                break;
            }
            case EffectKind::reverb:
            {
                mix = c[reverbMix]; Stereo excitation;
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    const double samples = c[reverbPredelay]*rate*.001;
                    excitation[ch] = samples < 1 ? input[ch]*(1-samples)+predelay[ch].read(1)*samples : predelay[ch].read(samples);
                    predelay[ch].write(input[ch]);
                }
                if(revision==2) { wet=modernReverb(excitation,c); break; }
                constexpr std::array<double,4> times {.0297,.0371,.0411,.0437};
                std::array<double,4> values, feedback;
                const double a = lowpassCoefficient(20000*std::pow(.025,c[reverbDamping]),rate);
                for (size_t i = 0; i < 4; ++i)
                {
                    const double seconds = times[i]*(.5+1.5*c[reverbSize]);
                    damping[i] += a*(diffusion[i].read(seconds*rate)-damping[i]); values[i] = damping[i];
                    feedback[i] = std::pow(10.0,-3*seconds/c[reverbDecay]);
                }
                // Orthonormal Hadamard mixing with independently computed RT60.
                const std::array<double,4> h {(values[0]+values[1]+values[2]+values[3])*.5,
                    (values[0]-values[1]+values[2]-values[3])*.5,
                    (values[0]+values[1]-values[2]-values[3])*.5,
                    (values[0]-values[1]-values[2]+values[3])*.5};
                const std::array<double,4> feed {excitation[0],excitation[1],excitation[0],-excitation[1]};
                for (size_t i = 0; i < 4; ++i) diffusion[i].write(feed[i]*.5+h[i]*feedback[i]);
                wet = {(values[0]+values[2])*.5,(values[1]+values[3])*.5};
                break;
            }
            case EffectKind::frequencyShift:
            {
                const auto& f=frequencyShiftValues;mix=f[2];
                for(size_t ch=0;ch<2;++ch){double d=input[ch],q=d;
                    const auto parity=frequencyShiftFrames&1u;
                    for(size_t n=0;n<frequencyShiftPrepared.count;++n){auto& history=frequencyShiftHistory[ch][n];
                        const double x=n%2?q:d,a=frequencyShiftPrepared.coefficients[n],y=(a*x-history[parity])+a*history[2+parity];
                        history[parity]=x;history[2+parity]=y;if(n%2)q=y;else d=y;}
                    wet[ch]=d*frequencyShiftCosine[ch]-frequencyShiftDelayed[ch]*frequencyShiftSine[ch];frequencyShiftDelayed[ch]=q;
                    const double step=2*std::numbers::pi*(f[0]+(ch?.5:-.5)*f[1])/rate;
                    if(step!=frequencyShiftStepKeys[ch]){frequencyShiftStepKeys[ch]=step;frequencyShiftStepSine[ch]=std::sin(step);frequencyShiftStepCosine[ch]=std::cos(step);}
                    const double sine=frequencyShiftSine[ch],cosine=frequencyShiftCosine[ch];
                    frequencyShiftSine[ch]=sine*frequencyShiftStepCosine[ch]+cosine*frequencyShiftStepSine[ch];
                    frequencyShiftCosine[ch]=cosine*frequencyShiftStepCosine[ch]-sine*frequencyShiftStepSine[ch];
                    frequencyShiftPhase[ch]+=step;
                    if(frequencyShiftPhase[ch]>std::numbers::pi)frequencyShiftPhase[ch]-=2*std::numbers::pi;
                    else if(frequencyShiftPhase[ch]<-std::numbers::pi)frequencyShiftPhase[ch]+=2*std::numbers::pi;
                }
                if((++frequencyShiftFrames&255u)==0)for(size_t ch=0;ch<2;++ch){frequencyShiftSine[ch]=std::sin(frequencyShiftPhase[ch]);frequencyShiftCosine[ch]=std::cos(frequencyShiftPhase[ch]);}
                // Active dry mix keeps quadrature histories and both phases.
                if(mix==0)return input;
                break;
            }
            case EffectKind::dimension:
            {
                const auto& d=dimensionValues;
                const double offset=.004*d[0]*std::sin(dimensionPhase);
                double a=0,b=0;
                if(revision==modernModulatedTapAlgorithmRevision){
                    modernDimensionDelay.push((input[0]+input[1])*.5);
                    a=modernDimensionDelay.read((.012+offset)*rate);b=modernDimensionDelay.read((.012-offset)*rate);
                }else{
                    dimensionDelay.pushSample(0,(input[0]+input[1])*.5);
                    a=dimensionDelay.popSample(0,(.012+offset)*rate,false);b=dimensionDelay.popSample(0,(.012-offset)*rate,true);
                }
                const double delta=d[3]*d[2]*.5*((a-b)*.5);
                dimensionPhase+=2*std::numbers::pi*d[1]/rate;
                if(dimensionPhase>=2*std::numbers::pi)dimensionPhase-=2*std::numbers::pi;
                // Dry endpoints retain exact inputs while active history and
                // physical phase still advance once per internal frame.
                if(d[0]==0||d[2]==0||d[3]==0||delta==0)return input;
                return {input[0]+delta,input[1]-delta};
            }
            case EffectKind::stereo:
            {
                const auto& s=stereoValues;const double gain=s[0],pan=s[1],width=s[2],q=s[3];
                // Direct neutral and swap endpoints preserve input signed zeros.
                if(gain==1&&pan==0&&width==1){if(q==1)return input;if(q==-1)return {input[1],input[0]};}
                if(width==1&&q==1)wet=input;
                else if(width==1&&q==-1)wet={input[1],input[0]};
                else{const double mid=(input[0]+input[1])*.5;
                    if(width==0||q==0)wet={mid,mid};
                    else{const double side=(input[0]-input[1])*.5*width*q;wet={mid+side,mid-side};}}
                const double left=1-std::max(pan,0.),right=1+std::min(pan,0.);
                return {gain*left*wet[0],gain*right*wet[1]};
            }
            case EffectKind::flanger:
            {
                const auto& f=flangerValues;mix=f[4];
                const double delay=f[2]*.001*(1+.75*f[1]*std::sin(flangerPhase));
                if(revision==modernModulatedTapAlgorithmRevision)
                    for(size_t ch=0;ch<2;++ch){modernFlangerDelay[ch].push(input[ch]-f[3]*flangerPrevious[ch]);
                        wet[ch]=modernFlangerDelay[ch].read(delay*rate);flangerPrevious[ch]=wet[ch];}
                else{
                    flangerDelay.setDelay(delay*rate);
                    for(size_t ch=0;ch<2;++ch){flangerDelay.pushSample(int(ch),input[ch]-f[3]*flangerPrevious[ch]);
                        wet[ch]=flangerDelay.popSample(int(ch));flangerPrevious[ch]=wet[ch];}
                }
                flangerPhase+=2*std::numbers::pi*f[0]/rate;
                if(flangerPhase>=2*std::numbers::pi)flangerPhase-=2*std::numbers::pi;
                break;
            }
            case EffectKind::phaser:
            {
                phaserTargets();phaserCentre(phaserValues[0]);mix=phaserValues[1];
                wet=input;std::array<double*,2> channels {&wet[0],&wet[1]};
                juce::dsp::AudioBlock<double> block(channels.data(),2,1);
                juce::dsp::ProcessContextReplacing<double> context(block);phaser.process(context);
                break;
            }
            case EffectKind::chorus:
            {
                chorusTargets();mix=chorusValues[1];
                if(revision==modernModulatedTapAlgorithmRevision){modernChorus.setCentreDelay(chorusValues[0]);wet=modernChorus.process(input);break;}
                chorus.setCentreDelay(chorusValues[0]);
                wet=input;std::array<double*,2> channels {&wet[0],&wet[1]};
                juce::dsp::AudioBlock<double> block(channels.data(),2,1);
                juce::dsp::ProcessContextReplacing<double> context(block);chorus.process(context);
                break;
            }
            case EffectKind::filter:
            {
                mix=filterValues[3];const double cutoff=std::min(filterValues[0],.45*rate),q=filterValues[1],drive=filterValues[2];
                if(revision==modernFilterAlgorithmRevision)return modernFilterFrame(input,cutoff,q,drive,mix,mode);
                if(cutoff!=filterKeys[0]){filter.setCutoffFrequency(cutoff);filterKeys[0]=cutoff;}
                if(q!=filterKeys[1]){filter.setResonance(q);filterKeys[1]=q;}
                if(drive!=filterKeys[2]){filterDriveGain=std::pow(10.,drive/20);filterKeys[2]=drive;}
                if(int(mode)!=filterMode){filter.setType(mode==FilterFxMode::lowpass?juce::dsp::StateVariableTPTFilterType::lowpass:
                    mode==FilterFxMode::bandpass?juce::dsp::StateVariableTPTFilterType::bandpass:juce::dsp::StateVariableTPTFilterType::highpass);filterMode=int(mode);}
                for(size_t ch=0;ch<2;++ch){const auto x=input[ch];
                    const double excitation=drive==0?x:x+(drive/24)*(std::tanh(filterDriveGain*x)/filterDriveGain-x);
                    wet[ch]=filter.processSample(int(ch),excitation);
                    if(mode==FilterFxMode::bandpass)wet[ch]*=1/q;
                }
                break;
            }
            case EffectKind::eq:
            {
                Stereo result=input;
                for(size_t band=0;band<3;++band){
                    EqCoefficients coefficients;
                    for(size_t i=0;i<5;++i)coefficients[i]=eqControls[band][i].next();
                    for(size_t ch=0;ch<2;++ch){
                        auto& history=eqHistory[ch][band];const auto x=result[ch];
                        const double y=coefficients[0]*x+history[0];
                        history[0]=coefficients[1]*x-coefficients[3]*y+history[1];
                        history[1]=coefficients[2]*x-coefficients[4]*y;result[ch]=y;
                    }
                }
                return result;
            }
            case EffectKind::compressor:
            {
                mix = c[compressorMix];
                if (revision == modernCompressorAlgorithmRevision) { wet = modernCompressorFrame(input,c); break; }
                const std::array<double,4> parameters {c[compressorThreshold],c[compressorRatio],c[compressorAttack],c[compressorRelease]};
                if (parameters != lastCompressor)
                {
                    compressor.setThreshold(parameters[0]); compressor.setRatio(parameters[1]);
                    compressor.setAttack(parameters[2]); compressor.setRelease(parameters[3]); lastCompressor = parameters;
                }
                // One JUCE max-absolute detector controls both audio channels.
                const double detector = std::max(std::abs(input[0]),std::abs(input[1]));
                const double compressed = compressor.processSample(0,detector);
                const double gain = (detector > 0 ? compressed/detector : 1)*std::pow(10.0,c[compressorMakeup]/20);
                wet = {input[0]*gain,input[1]*gain};
                break;
            }
        }
        return {input[0]*(1-mix)+wet[0]*mix,input[1]*(1-mix)+wet[1]*mix};
    }
};
static_assert(alignof(Effect)<=alignof(detail::DspPipeline));
bool structuralEqual(const RoutedPatch& a, const RoutedPatch& b) noexcept
{
    if (a.layout != b.layout) return false;
    for (size_t i = 0; i < 8; ++i) if (a.layout.effects[i].use == SlotUse::present)
    {
        if (a.effects[i].bypassed != b.effects[i].bypassed) return false;
        if (a.effects[i].algorithmRevision != b.effects[i].algorithmRevision) return false;
        const auto kind = a.layout.effects[i].kind;
        if (kind == EffectKind::drive && a.effects[i].drive.shape != b.effects[i].drive.shape) return false;
        if (kind == EffectKind::delay && (a.effects[i].delay.synced != b.effects[i].delay.synced
            || a.effects[i].delay.pingPong != b.effects[i].delay.pingPong)) return false;
        if (kind == EffectKind::filter && a.effects[i].filter.mode != b.effects[i].filter.mode) return false;
        if (kind == EffectKind::convolution && a.effects[i].convolution.ir != b.effects[i].convolution.ir) return false;
    }
    return true;
}
}

bool detail::prepareFrequencyShift(double rate,FrequencyShiftPreparation& output) noexcept
{
    if(!std::isfinite(rate)||rate<8000||rate>768000)return false;
    const auto agm=[](double a,double b,double& result){
        for(int n=0;n<64;++n){const double arithmetic=(a+b)*.5,geometric=std::sqrt(a*b);
            if(!std::isfinite(arithmetic)||!std::isfinite(geometric))return false;
            if(std::abs(arithmetic-geometric)<=4*std::numeric_limits<double>::epsilon()*arithmetic){result=arithmetic;return true;}
            a=arithmetic;b=geometric;}
        return false;
    };
    // Local double form of the pinned JUCE elliptic design. The vendor's
    // four-term nome approximation is replaced solely by the exact AGM ratio.
    const double width=40/rate,wt=2*std::numbers::pi*width,k=std::pow(std::tan((std::numbers::pi-wt)/4),2);
    double numeratorAgm,denominatorAgm;
    if(!agm(1,std::sqrt(1-k*k),numeratorAgm)||!agm(1,k,denominatorAgm))return false;
    const double q=std::exp(-std::numbers::pi*numeratorAgm/denominatorAgm),ds=std::pow(10.,-90./20),k1=ds*ds/(1-ds*ds);
    if(!(q>0&&q<1))return false;
    int order=int(std::ceil(std::log(k1*k1/16)/std::log(q)));if(order%2==0)++order;if(order==1)order=3;
    if(order<3||(order-1)/2>24)return false;
    FrequencyShiftPreparation prepared;prepared.count=std::size_t((order-1)/2);
    for(std::size_t index=0;index<prepared.count;++index){const double i=double(index+1);double num=0,den=0;bool numDone=false,denDone=false;
        for(int m=0;m<1000;++m){const double delta=(m%2?-1.:1.)*std::pow(q,double(m)*(m+1))*std::sin((2*m+1)*std::numbers::pi*i/order);
            num+=delta;if(std::abs(delta)<=1e-100){numDone=true;break;}}
        for(int m=1;m<=1000;++m){const double delta=(m%2?-1.:1.)*std::pow(q,double(m)*m)*std::cos(2*m*std::numbers::pi*i/order);
            den+=delta;if(std::abs(delta)<=1e-100){denDone=true;break;}}
        if(!numDone||!denDone)return false;num*=2*std::pow(q,.25);
        const double wi=num/(1+2*den),radicand=(1-wi*wi*k)*(1-wi*wi/k);
        if(!std::isfinite(wi)||radicand<0)return false;
        const double api=std::sqrt(radicand)/(1+wi*wi),a=(1-api)/(1+api);
        if(!std::isfinite(a)||!(a>0&&a<1))return false;prepared.coefficients[index]=a;
    }
    const double pole=std::sqrt(*std::max_element(prepared.coefficients.begin(),prepared.coefficients.begin()+prepared.count)),rho=(1+pole)*.5;
    double d=1,qBound=1/rho;
    for(std::size_t n=0;n<prepared.count;++n){const double a=prepared.coefficients[n],bound=a+(1-a*a)/(rho*rho-a);if(n%2)qBound*=bound;else d*=bound;}
    const double frames=std::ceil(std::log((d+qBound)/1e-6)/(-std::log(rho)));
    prepared.tailSeconds=.005+frames/rate;
    if(!std::isfinite(prepared.tailSeconds)||prepared.tailSeconds<=0)return false;
    output=prepared;return true;
}
double detail::frequencyShiftTailSeconds(const FrequencyShiftSettings& settings,double rate) noexcept
{
    FrequencyShiftPreparation prepared;
    if(!validFrequencyShift(settings)||!prepareFrequencyShift(rate,prepared))return std::numeric_limits<double>::quiet_NaN();
    // Complete two-cascade weighted l1 bound includes Q's one-frame delay.
    // Oscillator roundoff allowance and actual moving residuals are tested;
    // the chain owner separately includes source release and quality FIR.
    return prepared.tailSeconds;
}

namespace
{
// Frames the revision-2 sinc reads beyond the linear read's older tap.
double modulatedTapSupport(AlgorithmRevision revision) noexcept
{ return revision==modernModulatedTapAlgorithmRevision?double(detail::SincTapTable::points/2):0; }
}

double detail::dimensionTailSeconds(const DimensionSettings& settings,double rate,AlgorithmRevision revision) noexcept
{
    if(!validDimension(settings)||!std::isfinite(rate)||rate<8000)return std::numeric_limits<double>::quiet_NaN();
    // Global historical tap support, independent of later target decreases.
    // Source release and the quality FIR support belong to the chain owner.
    return .005+(std::ceil(.016*rate)+2+modulatedTapSupport(revision))/rate;
}

double detail::flangerTailSeconds(const FlangerSettings& settings,double rate,AlgorithmRevision revision) noexcept
{
    if(!validFlanger(settings)||!std::isfinite(rate)||rate<8000)return std::numeric_limits<double>::quiet_NaN();
    // A global stationary loop envelope preserves history from earlier
    // feedback/delay targets. Moving extrema have separate empirical checks.
    const double feedback=double(float(.8)),repeats=std::ceil(std::log(9e6/(1-feedback))/(-std::log(feedback)));
    return .005+(repeats+1)*(std::ceil(.014*rate)+3+modulatedTapSupport(revision))/rate;
}

double detail::phaserTailSeconds(const PhaserSettings& settings,double rate) noexcept
{
    if(!validPhaser(settings)||!std::isfinite(rate)||rate<8000)return std::numeric_limits<double>::quiet_NaN();
    const auto pole=[&](double cutoff){const double g=std::tan(std::numbers::pi*cutoff/rate);return std::abs(2*g/(1+g)-1);};
    const double p=std::max(pole(20),pole(std::min(20000.,.49*rate))),feedback=std::abs(double(settings.feedback));
    double rho=(1+p)*.5;
    if(feedback>0){const double t=std::pow((1+feedback)/(2*feedback),1./7);
        rho=std::nextafter(std::max(1/t,(1+t*p)/(t+p)),1.);}
    const double b=(1-p*rho)/(rho-p),cascade=std::pow(b,6),loop=feedback*cascade/rho;
    if(!(p<rho&&rho<1&&loop<1))return std::numeric_limits<double>::quiet_NaN();
    const double bound=cascade/(1-loop),frames=std::ceil(std::log(9e6*bound/(1-rho))/(-std::log(rho)));
    // Stationary complete six-AP/one-frame feedback Cauchy support. Moving
    // admitted controls have separate empirical residual/stability evidence.
    return .055+(frames+4)/rate;
}

double detail::chorusTailSeconds(const ChorusSettings& c,double rate,AlgorithmRevision revision) noexcept
{
    if(!validChorus(c)||!std::isfinite(rate)||rate<8000)return std::numeric_limits<double>::quiet_NaN();
    const double delay=std::ceil((double(c.baseDelayMs)+10*double(c.depth))*rate/1000)+2+modulatedTapSupport(revision);
    const double feedback=std::abs(double(c.feedback));
    const double repeats=feedback==0?0:std::ceil(std::log(9e6)/(-std::log(feedback)));
    // Relative geometric engineering support includes modulation extrema,
    // interpolation, one feedback-memory frame and stock smoothing support.
    return .050+(repeats+1)*(delay+1)/rate;
}

double detail::filterTailSeconds(const FilterSettings& f,double rate,AlgorithmRevision revision) noexcept
{
    if(!validFilter(f)||!std::isfinite(rate)||rate<8000)return std::numeric_limits<double>::quiet_NaN();
    // Revision 2's pole runs at four times the rate, behind the halfbands.
    const bool modern=revision==modernFilterAlgorithmRevision;const double fast=modern?4*rate:rate;
    const double g=std::tan(std::numbers::pi*std::min(double(f.cutoffHz),.45*rate)/fast);
    const double d=1+g/double(f.resonance)+g*g,a1=2*(g*g-1)/d,a2=(1-g/double(f.resonance)+g*g)/d;
    const double radius=eqPoleRadius({0,0,0,a1,a2});
    if(!std::isfinite(radius)||radius>=1)return std::numeric_limits<double>::quiet_NaN();
    const double seconds=.005+4/rate+(radius>0?4*std::log(9e6)/(-fast*std::log(radius)):0);
    // The halfband bound carries its own 5-ms guard.
    return modern?seconds+drive2OversamplingTailSeconds(rate)-.005:seconds;
}

double detail::midSideSplitTailSeconds(double rate) noexcept
{
    if(!std::isfinite(rate)||rate<8000||rate>768000)return std::numeric_limits<double>::quiet_NaN();
    const double g=std::tan(std::numbers::pi*1000/rate),a=g/(1+g),rho=1-2*a;
    if(!(rho>0&&rho<1))return std::numeric_limits<double>::quiet_NaN();
    const double logRho=std::log(rho),threshold=std::log(1e-6),logScale=std::log(16.);
    const auto admitted=[&](std::int64_t n){return logScale+double(n-1)*logRho<=threshold;};
    const double estimate=1+std::ceil((threshold-logScale)/logRho);
    if(!std::isfinite(estimate)||estimate<1||estimate>1000000)return std::numeric_limits<double>::quiet_NaN();
    auto n=static_cast<std::int64_t>(estimate);while(!admitted(n))++n;while(n>1&&admitted(n-1))--n;
    return double(n+static_cast<std::int64_t>(std::ceil(.005*rate))+1)/rate;
}
double detail::frequencySplitTailSeconds(double rate,AlgorithmRevision revision) noexcept
{
    if(!std::isfinite(rate)||rate<8000||rate>768000)return std::numeric_limits<double>::quiet_NaN();
    if(revision==modernFrequencySplitAlgorithmRevision){
        // Each band squares a Butterworth section; the slowest is the 20-Hz one.
        const auto radius=[&](double hz){const double g=std::tan(std::numbers::pi*std::min(hz,.45*rate)/rate),k=std::numbers::sqrt2,d=1+g*k+g*g;
            return eqPoleRadius({0,0,0,2*(g*g-1)/d,(1-g*k+g*g)/d});};
        const double toneG=std::tan(std::numbers::pi*1000/rate),r=std::max({radius(20),radius(20000),std::abs(1-2*toneG/(1+toneG))});
        if(!(r>0&&r<1))return std::numeric_limits<double>::quiet_NaN();
        return .005+4/rate+2*std::log(9e6)/(-rate*std::log(r));
    }
    const auto pole=[&](double hz){const double g=std::tan(std::numbers::pi*std::min(hz,.45*rate)/rate),a=g/(1+g);return std::abs(1-2*a);};
    const double rho=std::max({pole(20),pole(20000),pole(1000)});
    if(!(rho>0&&rho<1))return std::numeric_limits<double>::quiet_NaN();
    const double logRho=std::log(rho),threshold=std::log(1e-6);
    const auto admitted=[&](std::int64_t n){return std::log(320.+128.*double(n))+double(n-1)*logRho<=threshold;};
    std::int64_t low=static_cast<std::int64_t>(std::ceil(1/(1-rho))),high=low;
    while(!admitted(high)){if(high>1000000)return std::numeric_limits<double>::quiet_NaN();high*=2;}
    while(low<high){const auto middle=low+(high-low)/2;if(admitted(middle))high=middle;else low=middle+1;}
    return double(low+static_cast<std::int64_t>(std::ceil(.005*rate))+1)/rate;
}
double detail::convolutionTailSeconds(const ConvolutionSettings& settings,double rate) noexcept
{
    if(!validConvolution(settings)||!std::isfinite(rate)||rate<8000||rate>768000)return std::numeric_limits<double>::quiet_NaN();
    const int frames=settings.ir==ConvolutionIr::unitImpulse?1:juce::roundToInt(double(detail::convolutionCanonicalFrames)*rate/detail::convolutionCanonicalRate);
    // Max admitted tap covers decreasing predelay and interrupted ramps. This
    // conservative support is separate from physical Unit-at-zero support.
    return double(frames-1)/rate+.250+.005+2/rate;
}
bool detail::eqBandCoefficients(const EqSettings& settings,double effectiveSampleRate,AlgorithmRevision revision,std::array<EqCoefficients,3>& output) noexcept
{ return eqCoefficients(settings,effectiveSampleRate,revision,output); }
double detail::eqTailSeconds(const EqSettings& settings,double effectiveSampleRate,AlgorithmRevision revision) noexcept
{
    std::array<EqCoefficients,3> coefficients;
    if(!eqCoefficients(settings,effectiveSampleRate,revision,coefficients))return std::numeric_limits<double>::quiet_NaN();
    double seconds=.005+6/effectiveSampleRate;
    for(const auto& band:coefficients){
        const double radius=eqPoleRadius(band);
        if(!std::isfinite(radius)||radius>=1)return std::numeric_limits<double>::quiet_NaN();
        if(radius>0)seconds+=4*std::log(9e6)/(-effectiveSampleRate*std::log(radius));
    }
    return seconds;
}
double detail::delayReadSupportSeconds(double rate,AlgorithmRevision revision) noexcept
{ return revision==modernDelayAlgorithmRevision?double(detail::SincTapTable::points/2)/rate:0; }
double detail::drive2OversamplingTailSeconds(double rate) noexcept
{
    if(!std::isfinite(rate)||rate<8000||rate>768000)return std::numeric_limits<double>::quiet_NaN();
    // The first-stage halfbands run at the base rate; the two second-stage ones
    // run at twice it, so together they span one second-stage support.
    const double samples=halfbandSupportSamples<Drive2Stage1Up>()+halfbandSupportSamples<Drive2Stage1Down>()+halfbandSupportSamples<Drive2Stage2>();
    return .005+(samples+2)/rate;
}

namespace {
unsigned spacePresence(const RoutedPatch& p) noexcept {
    unsigned mask=0;for(unsigned i=0;i<8;++i)if(p.layout.effects[i].use==SlotUse::present&&p.layout.effects[i].kind==EffectKind::spaceReverb)mask|=1U<<i;return mask;
}
space::SpaceActivationRequest spaceRequestFor(const RoutedPatch& p,const space::runtime::ModalSlotIdentities& ids,std::uint64_t key,unsigned installMask) noexcept {
    space::SpaceActivationRequest request;request.key=key;request.identities=ids;request.activeMask=spacePresence(p);request.installMask=installMask;
    for(unsigned i=0;i<8;++i)request.settings[i]=p.effects[i].spaceReverb;return request;
}
}

struct RoutedSynthEngine::State
{
    FxMonitor* monitor=nullptr;
    SynthEngine synth;
    RoutedPatch requested, active;
    struct SourceView {
        RoutedPatch patch;
        BorrowedDrumAssignments assignments{};
        std::uint64_t token=0;
        drums::PublicationIdentity identity{};
        bool operator==(const SourceView&) const = default;
    };
    SourceView committedSource,pendingSpaceSource,spaceInstallPreviousSource;
    std::uint64_t passedGeneration=0;
    bool pendingSpaceSourceValid=false,spaceInstallPreviousSourceValid=false;
    struct SourceRuntime {
        bool drum=false,targetDrum=false,zeroFirst=false;
        enum Phase {steady,down,up} phase=steady;
        Ramp gain;
    };
    std::array<SourceRuntime,4> sources;
    std::array<drums::DrumEngine,4> drums;
    bool sourceAware=false,borrowedSegment=false;
    bool authoritativeDrums=false;
    std::array<bool,4> drumUnavailable{};
    int sourceSegmentBase=0;
#ifndef NDEBUG
    void* sourceBorrowTestContext=nullptr;
    void (*sourceBorrowTestHook)(void*,unsigned)=nullptr;
#endif
    space::SpaceActivation spaceActivation;
    space::runtime::ModalSlotIdentities spaceIdentities{1,2,3,4,5,6,7,8};
    space::SpaceActivationRequest pendingSpaceRequest;
    RoutedPatch pendingSpacePatch,spaceInstallPreviousPatch;
    std::uint64_t spaceKey=1,recallToken=0,pendingRecallToken=0;
    bool spaceHasBundle=false,spacePending=false,spaceInstallPending=false,spacePreparationFailed=false;
    std::array<Stereo,512> spaceFrames{};
    std::array<std::uint64_t,8> renderedSpaceFrames{};

    std::array<Effect,8> effects;
    std::array<juce::AudioBuffer<float>,4> buffers;
    std::vector<float> masterGains;
    std::vector<Stereo> masterEqFrames;
    std::vector<Stereo> externalFrames;
    std::span<const Stereo> externalView;
    std::array<Ramp,4> gates;
    Ramp transition, outputGain;
    double outputEnvelope = 0, outputRelease = 0, outputCeiling = 1;
    double hostOutputEnvelope = 0, hostOutputRelease = 0;
    double floatLimit = std::numeric_limits<float>::max();
    int protectionClearRemaining = -1;
    double rate = 0;
    int maximumBlock = 0, rampSamples = 1;
    int factor = 1, latency = 0;
    EqPreparationModes eqModes;
    int spectralStageDelay=0, spectralDelay=0, outputPad=0;
    std::size_t spectralBytes=0;
    std::array<int,4> insertPad {};
    std::array<std::array<detail::HistoryRing,2>,5> alignment;
    std::array<int,5> spectralCounts(const RackLayout& layout) const noexcept
    {
        std::array<int,5> counts {};
        for(std::size_t slot=0;slot<8;++slot)
            if(layout.effects[slot].use==SlotUse::present && layout.effects[slot].kind==EffectKind::dynamicEq
                && eqModes.slots[slot]==DynamicEqProcessingMode::spectral)++counts[layout.effects[slot].chain];
        return counts;
    }
    int requiredSpectralDelay(const RackLayout& layout) const noexcept
    {
        const auto counts=spectralCounts(layout);int longest=0;
        for(std::size_t layer=0;layer<4;++layer)if(layout.synthUse[layer]==SlotUse::present)longest=std::max(longest,counts[layer]);
        return (longest+counts[masterChain])*spectralStageDelay;
    }
    void alignRouting() noexcept
    {
        const auto counts=spectralCounts(active.layout);int longest=0;
        for(std::size_t layer=0;layer<4;++layer)if(active.layout.synthUse[layer]==SlotUse::present)longest=std::max(longest,counts[layer]);
        for(std::size_t layer=0;layer<4;++layer)insertPad[layer]=(longest-counts[layer])*spectralStageDelay;
        outputPad=spectralDelay-(longest+counts[masterChain])*spectralStageDelay;
        for(auto& pair:alignment)for(auto& ring:pair)ring.clear();
    }
    Stereo pad(Stereo input,std::size_t path,int delay) noexcept
    {
        if(delay>0)for(std::size_t ch=0;ch<2;++ch){const auto value=alignment[path][ch].read(delay);alignment[path][ch].write(input[ch]);input[ch]=value;}
        return input;
    }
    RenderQuality quality = RenderQuality::realtime;
    std::unique_ptr<juce::dsp::Oversampling<float>> resampler;
    std::array<float,2> lastHigh {};
    bool delayHighSample = false;
    bool switching = false, missingTempo = false;
    struct alignas(64) ReadyStream {std::atomic<unsigned> token{0};};
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    std::array<ReadyStream,6> readyStreams;
    int parallelCount = 0;
    bool parallelSynthOnly=false;
    bool parallelSpaceFx=false,parallelSpaceFxOk=true;
    int parallelSpaceOffset=0;
    unsigned parallelSpaceAdmissions=0;
    std::optional<double> parallelTempo;
    int parallelPacketFrames=16;
    static_assert(std::atomic<unsigned>::is_always_lock_free&&sizeof(std::atomic<unsigned>)==sizeof(unsigned));
    std::uint64_t parallelBlocks = 0, parallelFrames = 0;
    // Smaller packets reduce the final synth/EQ pipeline bubble. Producer and
    // consumer share this bound; equations and per-slot sample order stay exact.
    static constexpr int pipelineChunkFrames = 16;
    // Last member: retire all borrowed jobs before any DSP storage is destroyed.
    detail::DspPipeline pipeline;
    bool holdsOlderSourceView(std::uint64_t generation) const noexcept {
        return (pendingSpaceSourceValid&&pendingSpaceSource.identity.generation<generation)
            ||(spaceInstallPreviousSourceValid&&spaceInstallPreviousSource.identity.generation<generation);
    }
    bool sourceInstallComplete() const noexcept {
        if(spaceInstallPending||switching||transition.remaining!=0)return false;
        for(const auto& source:sources)if(source.phase!=SourceRuntime::steady)return false;
        return !holdsOlderSourceView(committedSource.identity.generation);
    }
    static bool usesSources(const SourceView& view) noexcept {
        for(std::size_t bank=0;bank<4;++bank){if(view.patch.drumSource[bank])return true;for(auto* asset:view.assignments[bank])if(asset)return true;}return false;
    }
    static drums::RackSettings drumSettings(const SourceView& view,std::size_t bank) noexcept {
        drums::RackSettings out;const auto& common=view.patch.synth.layers[bank];out.enabled=common.enabled;out.level=common.level;out.pan=common.pan;
        for(std::size_t pad=0;pad<16;++pad){const auto& p=view.patch.drumPads[bank][pad];out.pads[pad]={view.assignments[bank][pad],p.gainDb,p.pan,p.tuneSemitones,p.trimStart,p.trimEnd,p.attackSeconds,p.decaySeconds,p.midiNote,p.chokeGroup};}return out;
    }
    static bool validSource(const SourceView& view) noexcept {
        for(std::size_t bank=0;bank<4;++bank)if(view.patch.drumSource[bank]&&view.patch.layout.synthUse[bank]==SlotUse::present)
            if(!drums::DrumEngine::validSettings(drumSettings(view,bank)))return false;
        return true;
    }
    unsigned excludedSources() const noexcept {unsigned mask=0;for(std::size_t i=0;i<4;++i)if(sources[i].drum)mask|=1u<<i;return mask;}
    void melodicSettings() noexcept {
        const auto& p=sourceAware?committedSource.patch:requested;auto effective=p.synth;
        for(std::size_t i=0;i<4;++i)effective.layers[i].enabled&=p.layout.synthUse[i]==SlotUse::present&&!sources[i].drum;
        const bool ok=synth.setPatch(effective);jassert(ok);(void)ok;
    }
    void configureDrum(std::size_t bank,bool snap=false) noexcept {
        if(!sources[bank].drum)return;
        const auto& p=committedSource.patch;const bool selected=p.drumSource[bank]&&p.layout.synthUse[bank]==SlotUse::present;
        auto settings=selected?drumSettings(committedSource,bank):drums[bank].settings_;
        if(!selected){settings.enabled=p.synth.layers[bank].enabled;settings.level=p.synth.layers[bank].level;settings.pan=p.synth.layers[bank].pan;}
        if(p.layout.synthUse[bank]!=SlotUse::present)settings.enabled=false;
        if(!settings.enabled&&gates[bank].remaining>0&&gates[bank].value!=0)settings.enabled=true;
        const bool ok=drums[bank].setSettings(settings);jassert(ok);(void)ok;if(snap)drums[bank].reset();
    }
    void commitSource(const SourceView& view,bool snap=false) noexcept {
        sourceAware|=usesSources(view);committedSource=view;
        for(std::size_t bank=0;bank<4;++bank){auto& s=sources[bank];const bool target=view.patch.drumSource[bank]&&view.patch.layout.synthUse[bank]==SlotUse::present;
            s.targetDrum=target;
            if(snap){s.drum=target;s.phase=SourceRuntime::steady;s.zeroFirst=false;s.gain.set(1,rampSamples,true);drums[bank].reset();if(!target){drums::RackSettings empty;empty.enabled=false;(void)drums[bank].setSettings(empty);}}
            else if(s.drum!=target){s.phase=SourceRuntime::down;s.gain.set(0,rampSamples);}
            else if(s.phase==SourceRuntime::down)s.gain.set(0,rampSamples);
        }
        melodicSettings();gatesToTarget(snap);
        for(std::size_t bank=0;bank<4;++bank)configureDrum(bank,snap);
    }
    void sourceBoundaries() noexcept {
        bool changed=false;
        for(std::size_t bank=0;bank<4;++bank){auto& s=sources[bank];
            if(s.phase==SourceRuntime::down&&s.gain.remaining<=1){(void)s.gain.next();
                if(s.drum!=s.targetDrum){drums[bank].reset();synth.retireSourceLayer(bank);s.drum=s.targetDrum;if(!s.drum){drums::RackSettings empty;empty.enabled=false;(void)drums[bank].setSettings(empty);}changed=true;}
                s.phase=SourceRuntime::up;s.zeroFirst=true;s.gain.set(1,rampSamples);configureDrum(bank,true);
            }
            if(s.phase==SourceRuntime::up&&s.gain.remaining==0)s.phase=SourceRuntime::steady;
            if(s.drum&&gates[bank].target==0&&gates[bank].remaining<=1&&!committedSource.patch.synth.layers[bank].enabled){drums[bank].reset();auto settings=drums[bank].settings_;settings.enabled=false;const bool ok=drums[bank].setSettings(settings);jassert(ok);(void)ok;}
        }
        if(changed)melodicSettings();
    }
    int sourceSpan(int first,int count) const noexcept {
        int length=count-first;
        for(std::size_t bank=0;bank<4;++bank){const auto& s=sources[bank];if(s.phase==SourceRuntime::down&&s.gain.remaining>1)length=std::min(length,s.gain.remaining-1);
            if(s.drum&&gates[bank].target==0&&gates[bank].remaining>1&&!committedSource.patch.synth.layers[bank].enabled)length=std::min(length,gates[bank].remaining-1);}
        if(switching&&transition.remaining>1)length=std::min(length,transition.remaining-1);
        return length;
    }
    void sourceRange(std::size_t begin,std::size_t end,int first,int count) noexcept {
        const int absolute=sourceSegmentBase+first;
        for(std::size_t bank=begin;bank<end;++bank){auto& s=sources[bank];
#ifndef NDEBUG
            if(sourceBorrowTestHook&&detail::DspPipeline::renderingOnThisThread()){
                const drums::SampleAsset* borrowed=nullptr;
                for(const auto& pad:drums[bank].settings_.pads)if(pad.sample){borrowed=pad.sample;break;}
                if(!borrowed)for(const auto& hit:drums[bank].hits_)if(hit.sample){borrowed=hit.sample;break;}
                if(borrowed)sourceBorrowTestHook(sourceBorrowTestContext,static_cast<unsigned>(bank));
            }
#endif
            const bool admit=s.drum&&committedSource.patch.layout.synthUse[bank]==SlotUse::present&&committedSource.patch.synth.layers[bank].enabled;
            if(authoritativeDrums){
                static constexpr std::array<std::uint64_t,16> zero{};
                const auto targets=s.drum&&committedSource.patch.drumSource[bank]&&committedSource.patch.layout.synthUse[bank]==SlotUse::present
                    ?std::span<const std::uint64_t,16>(committedSource.identity.targets.data()+bank*16,16):std::span<const std::uint64_t,16>(zero);
                (void)drums[bank].renderRange(absolute,count,admit,s.drum,targets,drumUnavailable[bank]);
            }else (void)drums[bank].renderRange(absolute,count,admit,s.drum);
            for(int n=absolute;n<absolute+count;++n){const double gain=s.zeroFirst?(s.zeroFirst=false,0.):s.gain.next();for(int ch=0;ch<2;++ch)buffers[bank].getWritePointer(ch)[n]=static_cast<float>(buffers[bank].getSample(ch,n)*gain);}
        }
    }
    void rollbackSpaceInstall() noexcept {
        requested=spaceInstallPreviousPatch;
        melodicSettings();pendingSpaceSourceValid=false;spaceInstallPreviousSourceValid=false;
        outputGain.set(std::pow(10.0,double(requested.outputGainDb)/20),rampSamples);
        for(std::size_t i=0;i<8;++i)effects[i].settings(requested.effects[i],rampSamples,false);
        gatesToTarget(false);switching=!structuralEqual(active,requested);
        transition.set(switching?0:1,rampSamples);
    }
    bool admitSpace(const RoutedPatch& p,std::uint64_t token,const SourceView* source=nullptr) noexcept {
        const unsigned present=spacePresence(p);
        if(spaceInstallPending){
            if(source?(pendingSpaceSourceValid&&*source==pendingSpaceSource):(token==recallToken&&structuralEqual(requested,p)))return true;
            bool installedOnly=(present&~spacePresence(active))==0;
            for(unsigned i=0;i<8;++i)if((present&(1U<<i))&&pendingSpaceRequest.identities[i]!=spaceIdentities[i])installedOnly=false;
            if(installedOnly){
                auto cancel=spaceRequestFor(active,spaceIdentities,++spaceKey,255);cancel.activeMask=0;
                spaceActivation.request(cancel);spaceInstallPending=false;rollbackSpaceInstall();
            }else{
                // Hold this immutable view until its bounded fade-zero install.
                return false;
            }
        }
        const unsigned added=present&~spacePresence(active);
        const bool fresh=token!=recallToken;
        if(!present||(!fresh&&added==0)){
            if(spacePending){auto cancel=spaceRequestFor(requested,spaceIdentities,++spaceKey,255);cancel.activeMask=0;spaceActivation.request(cancel);spacePending=false;spacePreparationFailed=false;pendingSpaceSourceValid=false;}
            return true;
        }
        bool changedAfterFailure=false;
        if(spacePreparationFailed)for(unsigned i=0;i<8;++i)if(pendingSpaceRequest.settings[i]!=p.effects[i].spaceReverb)changedAfterFailure=true;
        if(!spacePending||pendingRecallToken!=token||(source?(!pendingSpaceSourceValid||*source!=pendingSpaceSource):!structuralEqual(pendingSpacePatch,p))||changedAfterFailure){
            auto ids=spaceIdentities;for(unsigned i=0;i<8;++i)if(added&(1U<<i))++ids[i];
            pendingSpaceRequest=spaceRequestFor(p,ids,++spaceKey,fresh||!spaceHasBundle?255:added);
            pendingSpacePatch=p;pendingRecallToken=token;spacePending=true;spacePreparationFailed=false;spaceActivation.request(pendingSpaceRequest);
            if(source){pendingSpaceSource=*source;pendingSpaceSourceValid=true;}
        }
        const auto ready=spaceActivation.pollReady(pendingSpaceRequest);
        if(ready!=space::SpaceActivation::Result::ready){spacePreparationFailed=ready==space::SpaceActivation::Result::failed;return false;}
        if(fresh){if(!spaceActivation.installReady(pendingSpaceRequest))return false;spaceIdentities=pendingSpaceRequest.identities;spaceHasBundle=true;}
        else {spaceInstallPreviousPatch=requested;spaceInstallPending=true;if(source){spaceInstallPreviousSource=committedSource;spaceInstallPreviousSourceValid=true;}}
        spacePending=false;return true;
    }
    bool finishSpaceInstall() noexcept {
        if(!spaceInstallPending)return true;
        const bool installed=spaceActivation.installReady(pendingSpaceRequest);jassert(installed);if(!installed)return false;
        spaceIdentities=pendingSpaceRequest.identities;spaceHasBundle=true;spaceInstallPending=false;
        if(pendingSpaceSourceValid){
            if(sourceAware||usesSources(pendingSpaceSource))commitSource(pendingSpaceSource);
            else committedSource=pendingSpaceSource;
            pendingSpaceSourceValid=false;spaceInstallPreviousSourceValid=false;
        }return true;
    }
    void clearSpace(std::size_t slot) noexcept {if(spaceHasBundle)(void)spaceActivation.service().resetSlot(slot);}
    void clearAllSpace() noexcept {if(spaceHasBundle)for(unsigned i=0;i<8;++i)(void)spaceActivation.service().resetSlot(i);}
    bool spaceChain(std::span<Stereo> frames,std::size_t chainIndex,std::optional<double> tempo,unsigned& clocked,std::span<const Stereo> external) noexcept {
        for(std::size_t position=0;position<active.layout.effectCount[chainIndex];++position){
            const auto slot=active.layout.effectOrder[chainIndex][position];clocked|=1U<<slot;
            const auto& configuration=active.effects[slot];
            if(active.layout.effects[slot].kind==EffectKind::spaceReverb&&!configuration.bypassed){
                auto& effect=effects[slot];const auto count=static_cast<int>(frames.size());
                effect.advanceFilterBlock(count);
                for(auto& control:effect.controls)(void)control.advance(count);
                if(!spaceActivation.service().renderSlot(slot,frames))return false;
                renderedSpaceFrames[slot]+=frames.size();
            }else for(std::size_t n=0;n<frames.size();++n){effects[slot].advanceFilter();frames[n]=slotFrame(frames[n],slot,tempo,external.empty()?Stereo{}:external[n]);}
        }
        return true;
    }
    bool spaceSegment(int first,int length,std::optional<double> tempo,unsigned& admissions,bool allowAdmission=true) noexcept {
        auto mixed=std::span(masterEqFrames).subspan(static_cast<std::size_t>(first),static_cast<std::size_t>(length));std::fill(mixed.begin(),mixed.end(),Stereo{});
        const auto external=externalView.empty()?std::span<const Stereo>{}:externalView.subspan(static_cast<std::size_t>(first),static_cast<std::size_t>(length));
        unsigned activeMask=spacePresence(active);for(unsigned i=0;i<8;++i)if(active.effects[i].bypassed)activeMask&=~(1U<<i);
        space::runtime::ModalSlotSettings settings;for(unsigned i=0;i<8;++i)settings[i]=requested.effects[i].spaceReverb;
        auto& service=spaceActivation.service();
        if(activeMask&&!service.beginParent(settings,activeMask,spaceIdentities,static_cast<std::size_t>(length),admissions==0&&allowAdmission))return false;
        unsigned clocked=0;
        for(std::size_t layer=0;layer<4;++layer){
            auto frames=std::span(spaceFrames).first(static_cast<std::size_t>(length));
            for(int n=0;n<length;++n)frames[static_cast<std::size_t>(n)]=active.layout.synthUse[layer]==SlotUse::present?Stereo{buffers[layer].getSample(0,first+n),buffers[layer].getSample(1,first+n)}:Stereo{};
            if(!spaceChain(frames,layer,tempo,clocked,external))return false;
            if(spectralDelay && active.layout.synthUse[layer]==SlotUse::present)
                for(auto& frame:frames)frame=pad(frame,layer,insertPad[layer]);
            for(std::size_t n=0;n<frames.size();++n){const double gate=gates[layer].next();for(std::size_t ch=0;ch<2;++ch)mixed[n][ch]+=frames[n][ch]*gate;}
        }
        if(!spaceChain(mixed,masterChain,tempo,clocked,external))return false;
        for(unsigned i=0;i<8;++i)if(!(clocked&(1U<<i)))for(int n=0;n<length;++n)effects[i].advanceFilter();
        if(activeMask){if(!service.endParent())return false;admissions+=service.admissionsThisParent();}
        return true;
    }
    void mixMasterEqFrames(int first, int count) noexcept
    {
        for (int frame = first; frame < first + count; ++frame)
        {
            Stereo mixed {};
            for (size_t layer = 0; layer < 4; ++layer)
            {
                Stereo input {buffers[layer].getSample(0, frame), buffers[layer].getSample(1, frame)};
                if (active.layout.synthUse[layer] != SlotUse::present) input = {};
                const double gate = gates[layer].next();
                for (size_t channel = 0; channel < 2; ++channel) mixed[channel] += input[channel] * gate;
            }
            masterEqFrames[static_cast<size_t>(frame)] = mixed;
        }
    }
    void masterEqRange(int first, int count, size_t begin, size_t end) noexcept
    {
        const auto frames = std::span(masterEqFrames).subspan(static_cast<size_t>(first), static_cast<size_t>(count));
        const auto external=externalView.empty()?std::span<const Stereo>{}:externalView.subspan(static_cast<size_t>(first),static_cast<size_t>(count));
        for (size_t position = begin; position < end; ++position)
            masterEqSlotBlock(active.layout.effectOrder[masterChain][position], frames,external);
    }
    static void relaxPacketWait() noexcept
    {
#if defined(_WIN32)
        _mm_pause();
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }
    // Low bit claims the next packet; high bits publish completed packets.
    // Acquiring each stream token also transfers its DSP histories to its next owner.
    unsigned readyPackets(unsigned stream) const noexcept
    { return readyStreams[stream].token.load(std::memory_order_acquire) >> 1; }
    // Two-slot stages retain parallel work without per-slot claim overhead.
    unsigned parallelStreams() const noexcept
    { return parallelSynthOnly?(parallelSpaceFx?5:4):2+std::max(1u,(unsigned(active.layout.effectCount[masterChain])+1)/2); }
    bool parallelComplete(unsigned packets) const noexcept
    {
        if(parallelSynthOnly)for(unsigned stream=0;stream<3;++stream)if(readyPackets(stream)<packets)return false;
        return readyPackets(parallelStreams()-1)>=packets;
    }
    bool tryReadyPacket(unsigned& cursor,bool allowSpaceFx=true) noexcept
    {
        const unsigned packets=static_cast<unsigned>((parallelCount+parallelPacketFrames-1)/parallelPacketFrames);
        const unsigned streams=parallelSpaceFx&&!allowSpaceFx?4:parallelStreams();
        for(unsigned trial=0;trial<streams;++trial)
        {
            unsigned stream=cursor+trial;
            if(stream>=streams)stream-=streams;
            auto token=readyStreams[stream].token.load(std::memory_order_acquire);
            const unsigned done=token>>1;
            if((token&1u)!=0 || done>=packets)continue;
            const bool spaceFx=parallelSynthOnly&&parallelSpaceFx&&stream==4;
            const unsigned advance=spaceFx?std::min(2u,packets-done):1u;
            if(spaceFx){bool inputsReady=true;for(unsigned layer=0;layer<4;++layer)inputsReady&=readyPackets(layer)>=done+advance;if(!inputsReady)continue;}
            if(!parallelSynthOnly && stream==2 && (readyPackets(0)<=done || readyPackets(1)<=done))continue;
            if(!parallelSynthOnly && stream>2 && readyPackets(stream-1)<=done)continue;
            if(!readyStreams[stream].token.compare_exchange_strong(token,token|1u,
                std::memory_order_acq_rel,std::memory_order_relaxed))continue;
            const int first=static_cast<int>(done)*parallelPacketFrames;
            const int count=std::min(parallelPacketFrames*int(advance),parallelCount-first);
            if(spaceFx){
                // One claimed FX stream owns the service, gates and scratch across packet handoffs.
                if(parallelSpaceFxOk)for(int offset=0;offset<count;){
                    const int absolute=parallelSpaceOffset+first+offset,length=std::min(count-offset,512-absolute%512);
                    if(!spaceSegment(absolute,length,parallelTempo,parallelSpaceAdmissions,absolute%512==0)){parallelSpaceFxOk=false;break;}
                    offset+=length;
                }
            }
            else if(parallelSynthOnly){synth.renderParallelLayers(stream,stream+1,first,count);if(borrowedSegment)sourceRange(stream,stream+1,first,count);}
            else if(stream<2){synth.renderParallelLayers(stream*2,stream*2+2,first,count);if(borrowedSegment)sourceRange(stream*2,stream*2+2,first,count);}
            else
            {
                const int absolute=borrowedSegment?sourceSegmentBase+first:first;
                if(stream==2)mixMasterEqFrames(absolute,count);
                const size_t slot=(stream-2)*2;
                masterEqRange(absolute,count,slot,std::min(slot+2,size_t(active.layout.effectCount[masterChain])));
            }
            readyStreams[stream].token.store(token+2u*advance,std::memory_order_release);
            cursor=stream+1==streams?0:stream+1;
            return true;
        }
        return false;
    }
    static void parallelWork(void* owner, unsigned stage) noexcept
    {
        auto& state = *static_cast<State*>(owner);
        unsigned cursor=stage;
        const unsigned packets=static_cast<unsigned>((state.parallelCount+state.parallelPacketFrames-1)/state.parallelPacketFrames);
        while(!state.parallelComplete(packets))
            // A descheduled worker must not hold the dependent Space stream while the caller waits.
            if(!state.tryReadyPacket(cursor,false))return;
    }
    void gatesToTarget(bool snap) noexcept
    {
        const auto& target=sourceAware?committedSource.patch:requested;
        bool solo = false;
        for (size_t i = 0; i < 4; ++i)
            solo |= target.layout.synthUse[i] == SlotUse::present && target.synth.layers[i].enabled && target.solo[i];
        for (size_t i = 0; i < 4; ++i)
        {
            const bool audible = target.layout.synthUse[i] == SlotUse::present && target.synth.layers[i].enabled
                && !target.mute[i] && (!solo || target.solo[i]);
            gates[i].set(audible ? 1 : 0,rampSamples,snap);
        }
    }
    void switchRouting() noexcept
    {
        if(!finishSpaceInstall())return;
        if(monitor)monitor->invalidate();
        std::array<bool,5> affected {};
        for (size_t chain = 0; chain < 5; ++chain)
            affected[chain] = active.layout.effectOrder[chain] != requested.layout.effectOrder[chain]
                || (chain < 4 && active.layout.synthUse[chain] != requested.layout.synthUse[chain]);
        for (size_t i = 0; i < 8; ++i)
        {
            const auto& before = active.layout.effects[i]; const auto& after = requested.layout.effects[i];
            if (before != after || active.effects[i].bypassed != requested.effects[i].bypassed
                || active.effects[i].algorithmRevision != requested.effects[i].algorithmRevision
                || (after.kind == EffectKind::drive && active.effects[i].drive.shape != requested.effects[i].drive.shape)
                || (after.kind == EffectKind::delay && (active.effects[i].delay.synced != requested.effects[i].delay.synced
                    || active.effects[i].delay.pingPong != requested.effects[i].delay.pingPong))
                || (after.kind == EffectKind::filter && active.effects[i].filter.mode != requested.effects[i].filter.mode)
                || (after.kind == EffectKind::convolution && active.effects[i].convolution.ir != requested.effects[i].convolution.ir))
            { affected[before.chain] = true; affected[after.chain] = true; }
        }
        // A changed insert may already have excited the downstream master tail.
        for (size_t chain = 0; chain < 4; ++chain) affected[4] |= affected[chain];
        for (size_t i = 0; i < 8; ++i)
            if (affected[active.layout.effects[i].chain] || affected[requested.layout.effects[i].chain]) {effects[i].clear();clearSpace(i);}
        active = requested;
        if(!spacePresence(active))(void)spaceActivation.acknowledgeInactive();
        alignRouting();
    }
    Stereo slotFrame(Stereo input, size_t slot, std::optional<double> tempo,Stereo external) noexcept
    {
        const auto& configuration = active.effects[slot];
        const auto before=input;
        if (!configuration.bypassed)
            input = effects[slot].process(input,active.layout.effects[slot].kind,configuration.algorithmRevision,configuration.drive.shape,
                !configuration.delay.synced || tempo.has_value(),configuration.filter.mode,configuration.convolution.ir,configuration.delay.pingPong,external);
        else if(active.layout.effects[slot].kind==EffectKind::dynamicEq && effects[slot].spectralEq)
            for(std::size_t ch=0;ch<2;++ch){const auto value=effects[slot].spectralDry[ch].read(spectralStageDelay);effects[slot].spectralDry[ch].write(input[ch]);input[ch]=value;}
        if(monitor&&active.layout.effects[slot].kind==EffectKind::dynamicEq)
            monitor->capture(slot,before,input,configuration.bypassed?nullptr:&effects[slot].dynamicEq,configuration.bypassed?nullptr:effects[slot].spectralEq.get());
        if(monitor&&active.layout.effects[slot].kind==EffectKind::saturation)
            monitor->saturationAuditionRunning(static_cast<std::uint8_t>(slot),!configuration.bypassed&&effects[slot].saturation.auditioning());
        return input;
    }
    void masterEqSlotBlock(std::size_t slot, std::span<Stereo> frames,std::span<const Stereo> external) noexcept
    {
        auto& effect=effects[slot];
        if(active.effects[slot].bypassed){
            for(auto& frame:frames){const auto before=frame;
                if(effect.spectralEq)for(std::size_t ch=0;ch<2;++ch){const auto value=effect.spectralDry[ch].read(spectralStageDelay);effect.spectralDry[ch].write(frame[ch]);frame[ch]=value;}
                if(monitor)monitor->capture(static_cast<std::uint8_t>(slot),before,frame);
            }
            return;
        }
        std::array<Stereo,FxMonitor::packetFrames> input;
        while(!frames.empty()){
            const auto untilPacket=monitor?monitor->framesUntilPacket(static_cast<std::uint8_t>(slot)):0;
            if(untilPacket==0){if(external.empty())effect.dynamicEq.processBlock(frames);else (void)effect.dynamicEq.processBlock(frames,external);
                if(effect.spectralEq){effect.spectralEq->processBlock(frames);for(auto& frame:frames){const auto gain=effect.spectralGain.next();for(auto& value:frame)value*=gain;}}return;}
            const auto count=std::min(untilPacket,frames.size());
            std::copy_n(frames.begin(),count,input.begin());
            const auto chunk=frames.first(count);
            if(external.empty())effect.dynamicEq.processBlock(chunk);else (void)effect.dynamicEq.processBlock(chunk,external.first(count));
            if(effect.spectralEq){effect.spectralEq->processBlock(chunk);for(auto& frame:chunk){const auto gain=effect.spectralGain.next();for(auto& value:frame)value*=gain;}}
            for(std::size_t i=0;i<count;++i)monitor->capture(static_cast<std::uint8_t>(slot),input[i],chunk[i],&effect.dynamicEq,effect.spectralEq.get());
            frames=frames.subspan(count);
            if(!external.empty())external=external.subspan(count);
        }
    }
    Stereo chain(Stereo input, size_t chainIndex, std::optional<double> tempo,Stereo external) noexcept
    {
        for (size_t position = 0; position < active.layout.effectCount[chainIndex]; ++position)
        {
            const auto slot = active.layout.effectOrder[chainIndex][position];
            input = slotFrame(input,slot,tempo,external);
        }
        return input;
    }
    bool masterEqBlock(const juce::MidiBuffer& midi) const noexcept
    {
        if(switching || transition.remaining!=0 || transition.value!=1 || active.layout.effectCount[masterChain]==0)return false;
        for(size_t chainIndex=0;chainIndex<masterChain;++chainIndex)if(active.layout.effectCount[chainIndex]!=0)return false;
        for(size_t position=0;position<active.layout.effectCount[masterChain];++position)
            if(active.layout.effects[active.layout.effectOrder[masterChain][position]].kind!=EffectKind::dynamicEq)return false;
        for(const auto metadata:midi)if(metadata.numBytes==1&&metadata.data[0]==0xfc)return false;
        return true;
    }
    void updateTempo(std::optional<double> bpm) noexcept
    {
        for (size_t i = 0; i < 8; ++i)
        {
            auto settings = requested.effects[i]; settings.delay.synced = active.effects[i].delay.synced;
            if (active.layout.effects[i].use == SlotUse::present && active.layout.effects[i].kind == EffectKind::delay)
                effects[i].tempo(settings,bpm,rampSamples);
        }
    }
};
RoutedSynthEngine::RoutedSynthEngine() : state(std::make_unique<State>()) {}
EqPreparationModes eqPreparationModesFor(const RoutedPatch& patch) noexcept
{
    EqPreparationModes modes;
    for(std::size_t slot=0;slot<8;++slot)modes.slots[slot]=patch.effects[slot].dynamicEq.processingMode;
    return modes;
}
RoutedSynthEngine::~RoutedSynthEngine(){if(state&&state->monitor){state->monitor->invalidate();(void)state->monitor->publishSampleRate(0);}}
RoutedSynthEngine::RoutedSynthEngine(RoutedSynthEngine&&) noexcept = default;
RoutedSynthEngine& RoutedSynthEngine::operator=(RoutedSynthEngine&& other) noexcept
{
    if(this!=&other){if(state&&state->monitor){state->monitor->invalidate();(void)state->monitor->publishSampleRate(0);}state=std::move(other.state);if(state&&state->monitor)(void)state->monitor->publishSampleRate(state->rate);}
    return *this;
}
bool RoutedSynthEngine::prepare(double sampleRate, int maximumBlockSize)
{ return prepare(sampleRate,maximumBlockSize,RenderQuality::realtime); }
bool RoutedSynthEngine::prepare(double sampleRate, int maximumBlockSize, RenderQuality quality) try
{ return prepare(sampleRate,maximumBlockSize,quality,{}); }
catch(const std::bad_alloc&) { return false; }
bool RoutedSynthEngine::prepare(double sampleRate,int maximumBlockSize,RenderQuality quality,const EqPreparationModes& modes)
{return prepareWithPipelineLaunch(sampleRate,maximumBlockSize,quality,modes,nullptr);}
bool RoutedSynthEngine::prepareWithPipelineLaunch(double sampleRate,int maximumBlockSize,RenderQuality quality,const EqPreparationModes& modes,detail::DspPipeline::Launch launch,const space::SpaceActivationOptions& spaceOptions) try
{
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000 || maximumBlockSize < 1 || maximumBlockSize > 65536
        || (quality!=RenderQuality::realtime && quality!=RenderQuality::higher)) return false;
    // Build the entire replacement before touching the last working state.
    auto candidate = std::make_unique<State>();candidate->sourceAware=state->sourceAware;candidate->committedSource=state->committedSource;
    candidate->passedGeneration=state->passedGeneration;
    const bool committedView=state->sourceAware||state->committedSource.identity.generation!=0;
    candidate->requested=committedView?state->committedSource.patch:state->requested;candidate->monitor=state->monitor;candidate->recallToken=committedView?state->committedSource.token:state->recallToken;
#ifndef NDEBUG
    candidate->sourceBorrowTestContext=state->sourceBorrowTestContext;candidate->sourceBorrowTestHook=state->sourceBorrowTestHook;
#endif
    for(auto mode:modes.slots)if(mode>DynamicEqProcessingMode::spectral)return false;
    for(std::size_t slot=0;slot<8;++slot)if(candidate->requested.layout.effects[slot].use==SlotUse::present
        && candidate->requested.layout.effects[slot].kind==EffectKind::dynamicEq
        && candidate->requested.effects[slot].dynamicEq.processingMode!=modes.slots[slot])return false;
    candidate->eqModes=modes;
    candidate->factor=quality==RenderQuality::higher?2:1; candidate->quality=quality;
    const double internalRate=sampleRate*candidate->factor;
    const int internalBlock=maximumBlockSize*candidate->factor;
    candidate->rate = internalRate; candidate->maximumBlock = maximumBlockSize;
    candidate->rampSamples = static_cast<int>(std::ceil(internalRate*.005));
    candidate->outputRelease = std::exp(-1/(.050*internalRate));
    candidate->hostOutputRelease = std::exp(-1/(.050*sampleRate));
    candidate->outputCeiling = std::pow(10.0,-1.0/20);
    for (auto& buffer : candidate->buffers) buffer.setSize(2,internalBlock);
    for(auto& drum:candidate->drums)if(!drum.prepare(internalRate,internalBlock))return false;
    candidate->masterGains.resize(static_cast<size_t>(internalBlock));
    candidate->masterEqFrames.resize(static_cast<size_t>(internalBlock));
    candidate->externalFrames.resize(static_cast<size_t>(internalBlock));
    for (const auto& settings:candidate->requested.effects){std::array<EqCoefficients,3> coefficients;if(!eqCoefficients(settings.eq,internalRate,eqRevisionOf(settings),coefficients))return false;}
    detail::FrequencyShiftPreparation shiftPreparation;
    if(!detail::prepareFrequencyShift(internalRate,shiftPreparation))return false;
    for (auto& effect : candidate->effects){effect.frequencyShiftPrepared=shiftPreparation;if(!effect.prepare(internalRate))return false;}
    for(std::size_t slot=0;slot<8;++slot)if(modes.slots[slot]==DynamicEqProcessingMode::spectral){
        auto& effect=candidate->effects[slot];effect.spectralEq=std::make_unique<SpectralEq>();
        if(!effect.spectralEq->prepare(internalRate,internalBlock,4096,0,SpectralEqWork::bounded))return false;
        candidate->spectralStageDelay=effect.spectralEq->latencySamples();
        for(auto& ring:effect.spectralDry)ring.prepare(std::size_t(candidate->spectralStageDelay));
        candidate->spectralBytes+=effect.spectralEq->preparedBytes()+2*(std::size_t(candidate->spectralStageDelay)+2)*sizeof(double);
    }
    candidate->spectralDelay=candidate->requiredSpectralDelay(candidate->requested.layout);
    if(candidate->spectralStageDelay)for(auto& pair:candidate->alignment)for(auto& ring:pair)ring.prepare(std::size_t(8*candidate->spectralStageDelay));
    if(candidate->spectralStageDelay)candidate->spectralBytes+=10*(std::size_t(8*candidate->spectralStageDelay)+2)*sizeof(double);
    if(candidate->spectralBytes>64*1024*1024)return false;
    if(candidate->factor==2)
    {
        // Match the actual JUCE downsampler below. Its symmetric convolution
        // first adds sample pairs, then accumulates weighted pairs and a centre
        // tap. Fourfold headroom covers both intermediate sums and float rounding.
        // This limit is only relevant to extreme output near float overflow.
        const auto down=juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod(.08f,-100.f);
        const auto taps=down->getFilterOrder()+1;const auto* fir=down->getRawCoefficients();
        double norm=std::abs(double(fir[taps/2]));
        for(std::size_t k=0;k<taps/2;k+=2)norm+=2*std::abs(double(fir[k]));
        if(!std::isfinite(norm)||norm<=0)return false;
        candidate->floatLimit=std::numeric_limits<float>::max()/(4*std::max(1.,norm));
        candidate->resampler=std::make_unique<juce::dsp::Oversampling<float>>(2);
        candidate->resampler->clearOversamplingStages();
        candidate->resampler->addOversamplingStage(juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,.08f,-100.f,.08f,-100.f);
        candidate->resampler->initProcessing(static_cast<size_t>(maximumBlockSize));
        const auto delay=candidate->resampler->getLatencyInSamples()*.5f;
        if(delay!=std::floor(delay) && delay-std::floor(delay)!=.5f)return false;
        candidate->delayHighSample=delay!=std::floor(delay);
        candidate->latency=static_cast<int>(std::ceil(delay));
    }
    candidate->latency+=candidate->spectralDelay/candidate->factor;
    auto effective = candidate->requested.synth;
    for (size_t i = 0; i < 4; ++i) effective.layers[i].enabled &= candidate->requested.layout.synthUse[i] == SlotUse::present;
    if (!candidate->synth.setPatch(effective) || !candidate->synth.prepareInternal(internalRate,internalBlock,candidate->factor)) return false;
    const auto initialSpace=spaceRequestFor(candidate->requested,candidate->spaceIdentities,candidate->spaceKey,255);
    if(!candidate->spaceActivation.prepare(sampleRate,static_cast<unsigned>(candidate->factor),initialSpace,spaceOptions))return false;
    candidate->spaceHasBundle=initialSpace.activeMask!=0;
    // Optional resource: a startup failure leaves the exact serial renderer usable.
    if(launch)(void)candidate->pipeline.prepare(candidate.get(), State::parallelWork,launch);
    else (void)candidate->pipeline.prepare(candidate.get(), State::parallelWork);
    state.swap(candidate); reset(); return true;
}
catch(const std::bad_alloc&) { return false; }
RenderQuality RoutedSynthEngine::effectiveQuality() const noexcept { return state->quality; }
DynamicEqProcessingMode RoutedSynthEngine::effectiveEqMode(std::size_t slot) const noexcept
{ return slot<8?state->eqModes.slots[slot]:DynamicEqProcessingMode::ordinary; }
int RoutedSynthEngine::latencySamples() const noexcept { return state->latency; }
RoutedSynthEngine::SpectralStats RoutedSynthEngine::spectralStats() const noexcept
{
    SpectralStats result;result.preparedBytes=state->spectralBytes;
    for(const auto& effect:state->effects)if(effect.spectralEq){const auto work=effect.spectralEq->workStats();
        result.framesStarted+=work.framesStarted;result.framesCompleted+=work.framesCompleted;result.deadlineMisses+=work.deadlineMisses;
        result.maximumSliceWork=std::max(result.maximumSliceWork,work.maximumSliceWork);result.maximumSliceAllowance=std::max(result.maximumSliceAllowance,work.maximumSliceAllowance);
    }
    return result;
}
namespace {
bool validateRoutedValues(const RoutedPatch& p,bool playable) noexcept
{
    if (!validateRackLayout(p.layout) || !validateSynthPatch(p.synth) || !inRange(p.outputGainDb,-24,36)
        || !validateKernelCompatibility(p.compatibility)) return false;
    for(std::size_t bank=0;bank<4;++bank)for(const auto& pad:p.drumPads[bank]){
        if(!inRange(pad.gainDb,-60,12)||!inRange(pad.pan,-1,1)||!inRange(pad.tuneSemitones,-24,24)
            ||!inRange(pad.trimStart,0,1)||!inRange(pad.trimEnd,0,1)||!inRange(pad.attackSeconds,0,1)||!inRange(pad.decaySeconds,0,10)
            ||pad.midiNote<0||pad.midiNote>127||pad.chokeGroup<0||pad.chokeGroup>16
            ||(playable&&p.drumSource[bank]&&p.layout.synthUse[bank]==SlotUse::present&&pad.trimStart>=pad.trimEnd))return false;
    }
    for (size_t slot=0;slot<p.effects.size();++slot) {
        const auto kind=static_cast<size_t>(p.layout.effects[slot].kind);
        const auto& effect=p.effects[slot];
        if (!supportsEffectAlgorithmRevision(kind,effect.algorithmRevision) || !valid(effect)
            || effect.algorithmRevision!=p.compatibility.effects[slot][kind]) return false;
    }
    return true;
}
}
bool validateRoutedPatch(const RoutedPatch& p) noexcept{return validateRoutedValues(p,true);}
namespace detail {bool validateRoutedPatchForStorage(const RoutedPatch& p) noexcept{return validateRoutedValues(p,false);}}
bool RoutedSynthEngine::setPatch(const RoutedPatch& p,std::uint64_t recallToken) noexcept
{const auto assignments=state->committedSource.assignments;const auto identity=state->committedSource.identity;return setPatch(p,assignments,identity,recallToken);}
bool RoutedSynthEngine::setPatch(const RoutedPatch& p,const BorrowedDrumAssignments& assignments,std::uint64_t recallToken) noexcept
{const auto identity=state->committedSource.identity;return setPatch(p,assignments,identity,recallToken);}
bool RoutedSynthEngine::setPatch(const RoutedPatch& p,const BorrowedDrumAssignments& assignments,const drums::PublicationIdentity& identity,std::uint64_t recallToken) noexcept
{
    if (!validateRoutedPatch(p)||identity.generation<state->passedGeneration) return false;
    const State::SourceView view{p,assignments,recallToken,identity};if(!State::validSource(view))return false;
    const bool sources=state->sourceAware||State::usesSources(view);
    const bool completeView=sources||identity.generation!=0||state->committedSource.identity.generation!=0;
    if(state->rate>0){
        for(std::size_t slot=0;slot<8;++slot)if(p.layout.effects[slot].use==SlotUse::present && p.layout.effects[slot].kind==EffectKind::dynamicEq
            && p.effects[slot].dynamicEq.processingMode!=state->eqModes.slots[slot])return false;
        if(state->requiredSpectralDelay(p.layout)>state->spectralDelay)return false;
    }
    if(state->rate>0)for(size_t i=0;i<p.effects.size();++i)
        if(!state->effects[i].eqReady||p.effects[i].eq!=state->effects[i].eqRequested||eqRevisionOf(p.effects[i])!=state->effects[i].eqRevision){
            std::array<EqCoefficients,3> coefficients;if(!eqCoefficients(p.effects[i].eq,state->rate,eqRevisionOf(p.effects[i]),coefficients))return false;
        }
    if(state->rate>0&&!state->admitSpace(p,recallToken,completeView?&view:nullptr))return false;
    if(sources)state->sourceAware=true;
    if(completeView&&!state->spaceInstallPending){
        if(sources)state->commitSource(view,state->rate==0);
        else state->committedSource=view;
        state->pendingSpaceSourceValid=false;state->spaceInstallPreviousSourceValid=false;
    }
    auto effective = p.synth;
    for (size_t i = 0; i < 4; ++i) effective.layers[i].enabled &= p.layout.synthUse[i] == SlotUse::present;
    if (!sources&&!state->synth.setPatch(effective)) return false;
    state->requested = p;state->recallToken=recallToken;
    if(state->rate>0&&!spacePresence(state->active)&&!spacePresence(p))(void)state->spaceActivation.acknowledgeInactive();
    const bool snap = state->rate == 0;
    state->outputGain.set(std::pow(10.0,double(p.outputGainDb)/20),state->rampSamples,snap);
    for (size_t i = 0; i < 8; ++i) state->effects[i].settings(p.effects[i],state->rampSamples,snap);
    state->gatesToTarget(snap);
    if (snap) { state->active = p; state->transition.set(1,1,true); }
    else
    {
        state->switching = !structuralEqual(state->active,p);
        state->transition.set(state->switching ? 0 : 1,state->rampSamples);
    }
    return true;
}
bool RoutedSynthEngine::setPatchQuiescent(const RoutedPatch& p,std::uint64_t token) {
    const auto assignments=state->committedSource.assignments;const auto identity=state->committedSource.identity;
    return setPatchQuiescent(p,assignments,identity,token);
}
bool RoutedSynthEngine::setPatchQuiescent(const RoutedPatch& p,const BorrowedDrumAssignments& assignments,std::uint64_t token) {
    const auto identity=state->committedSource.identity;return setPatchQuiescent(p,assignments,identity,token);
}
bool RoutedSynthEngine::setPatchQuiescent(const RoutedPatch& p,const BorrowedDrumAssignments& assignments,const drums::PublicationIdentity& identity,std::uint64_t token) {
    if(identity.generation<state->passedGeneration)return false;
    RoutedSynthEngine candidate;if(!candidate.setPatch(p,assignments,identity,token))return false;
    candidate.state->passedGeneration=state->passedGeneration;
#ifndef NDEBUG
    candidate.state->sourceBorrowTestContext=state->sourceBorrowTestContext;candidate.state->sourceBorrowTestHook=state->sourceBorrowTestHook;
#endif
    if(state->rate>0&&!candidate.prepare(state->rate/state->factor,state->maximumBlock,state->quality,state->eqModes))return false;
    candidate.attachFxMonitor(state->monitor);*this=std::move(candidate);return true;
}
drums::PublicationIdentity RoutedSynthEngine::installedDrumIdentity() const noexcept
{
    auto out=state->committedSource.identity;
    const bool complete=!state->spaceInstallPending&&!state->switching&&state->transition.remaining==0
        &&!state->holdsOlderSourceView(out.generation);
    for(std::size_t bank=0;bank<4;++bank)
        if(!complete||state->sources[bank].phase!=State::SourceRuntime::steady
            ||!state->sources[bank].drum||!state->committedSource.patch.drumSource[bank]
            ||state->committedSource.patch.layout.synthUse[bank]!=SlotUse::present)
            for(std::size_t pad=0;pad<16;++pad)out.targets[bank*16+pad]=0;
    return out;
}
drums::ReferenceReport RoutedSynthEngine::drumReferenceReport(std::uint64_t lifecycle) const noexcept
{
    drums::ReferenceReport out;out.lifecycle=lifecycle;
    // render() ends every session only after all claimed and late-woken jobs finish.
    if(state->borrowedSegment||state->parallelCount!=0)return out;
    for(const auto& drum:state->drums)if(drum.rendering_)return out;
    const auto generation=state->committedSource.identity.generation;
    if(state->rate>0&&generation!=0&&state->sourceInstallComplete())
        state->passedGeneration=std::max(state->passedGeneration,generation);
    out.adoptedGeneration=state->passedGeneration;
    for(std::size_t bank=0;bank<4;++bank){
        std::array<const drums::SampleAsset*,48> references{};state->drums[bank].borrowedReferences(references);
        std::copy(references.begin(),references.end(),out.references.begin()+bank*references.size());
    }
    out.complete=true;return out;
}
void RoutedSynthEngine::clearDrumBorrowersQuiescent() noexcept
{
    // The lifecycle caller excludes callbacks before fencing the prepared job owners.
    (void)state->pipeline.finishUnstarted();
    deactivateSpace();
    for(auto& drum:state->drums)drum.clearBorrowedSamplesQuiescent();
    for(auto* view:{&state->committedSource,&state->pendingSpaceSource,&state->spaceInstallPreviousSource}){
        view->assignments={};view->identity={};
    }
    state->pendingSpaceSourceValid=false;state->spaceInstallPreviousSourceValid=false;
    state->passedGeneration=0;state->borrowedSegment=false;state->parallelCount=0;state->externalView={};
}
std::array<double,4> RoutedSynthEngine::sourceTailSeconds() const noexcept {
    std::array<double,4> result{};for(std::size_t bank=0;bank<4;++bank)if(state->sources[bank].drum)result[bank]=state->drums[bank].tailSeconds();return result;
}
double RoutedSynthEngine::spaceTailSeconds() const noexcept {
    if(!state->spaceHasBundle)return 0;double seconds=0;
    for(unsigned layer=0;layer<4;++layer){std::array<std::size_t,8> indices{};std::size_t count=0;
        for(unsigned chain:std::array<unsigned,2>{layer,masterChain})for(unsigned position=0;position<state->active.layout.effectCount[chain];++position){const auto slot=state->active.layout.effectOrder[chain][position];if(state->active.layout.effects[slot].kind==EffectKind::spaceReverb&&!state->active.effects[slot].bypassed)indices[count++]=slot;}
        if(count){space::runtime::ModalTailReport report;if(state->spaceActivation.service().composeTail(std::span(indices).first(count),4,report))seconds=std::max(seconds,report.seconds);}
    }
    return seconds;
}
void RoutedSynthEngine::deactivateSpace() noexcept {
    if(state->rate==0)return;
    state->spacePending=false;state->spaceInstallPending=false;
    state->pendingSpaceSourceValid=false;state->spaceInstallPreviousSourceValid=false;
    auto cancel=spaceRequestFor(state->requested,state->spaceIdentities,++state->spaceKey,255);cancel.activeMask=0;state->spaceActivation.request(cancel);
    (void)state->spaceActivation.acknowledgeInactive();
}
std::uint64_t RoutedSynthEngine::spacePreparationFailures() const noexcept {return state->spaceActivation.failureCount();}
RoutedSynthEngine::SpaceRenderStats RoutedSynthEngine::spaceRenderStats() const noexcept {
    SpaceRenderStats out;out.renderedPhysicalFrames=state->renderedSpaceFrames;out.physicalFactor=static_cast<unsigned>(state->factor);out.ready=state->spaceHasBundle;
    out.activeMask=spacePresence(state->active);for(unsigned i=0;i<8;++i)if(state->active.effects[i].bypassed)out.activeMask&=~(1U<<i);
    if(out.ready){out.modeCount=state->spaceActivation.service().modeCount();out.converterBackend=state->spaceActivation.service().converterBackend();}return out;
}
const RoutedPatch& RoutedSynthEngine::patch() const noexcept { return state->requested; }
void RoutedSynthEngine::render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, std::optional<double> tempoBpm) noexcept
{ render(out,midi,tempoBpm,{}); }
void RoutedSynthEngine::render(juce::AudioBuffer<float>& out,const juce::MidiBuffer& midi,std::optional<double> tempoBpm,std::span<const DynamicEq::Frame> external) noexcept
{renderImpl(out,midi,tempoBpm,external,{},false);}
void RoutedSynthEngine::render(juce::AudioBuffer<float>& out,const juce::MidiBuffer& midi,std::optional<double> tempoBpm,std::span<const DynamicEq::Frame> external,std::span<const drums::RenderEvent> events) noexcept
{renderImpl(out,midi,tempoBpm,external,events,true);}
bool RoutedSynthEngine::drumDispatchUnavailable() const noexcept
{return std::any_of(state->drumUnavailable.begin(),state->drumUnavailable.end(),[](bool value){return value;});}
void RoutedSynthEngine::renderImpl(juce::AudioBuffer<float>& out,const juce::MidiBuffer& midi,std::optional<double> tempoBpm,std::span<const DynamicEq::Frame> external,std::span<const drums::RenderEvent> events,bool authoritativeDrums) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    state->drumUnavailable={};state->authoritativeDrums=authoritativeDrums;
    out.clear(); int count = out.getNumSamples();
    if (state->rate == 0 || out.getNumChannels() != 2 || count > state->maximumBlock || count == 0
        || (!external.empty()&&external.size()!=static_cast<std::size_t>(count))) return;
    const auto left = reinterpret_cast<uintptr_t>(out.getWritePointer(0)), right = reinterpret_cast<uintptr_t>(out.getWritePointer(1));
    if ((left > right ? left-right : right-left) < static_cast<uintptr_t>(count)*sizeof(float)) return;
    if(!external.empty()){
        const auto start=reinterpret_cast<std::uintptr_t>(external.data()),bytes=external.size_bytes();
        for(auto channel:{left,right})if(start<channel+static_cast<std::uintptr_t>(count)*sizeof(float)&&channel<start+bytes)return;
    }
    if(authoritativeDrums){
        if(events.size()>8192)return;
        int previous=-1;
        for(const auto& event:events){
            if(event.hostOffset<previous||event.hostOffset<0||event.hostOffset>=count)return;
            previous=event.hostOffset;
            switch(event.kind){
            case drums::RenderEventKind::noteOn:
                if(event.padOrNote>127||event.velocity>127||event.channel<1||event.channel>16)return;break;
            case drums::RenderEventKind::exactPad:
                if(event.bank>=4||event.padOrNote>=16||event.velocity<1||event.velocity>127||event.channel<1||event.channel>16||!event.token)return;break;
            case drums::RenderEventKind::stopChannel:if(event.channel<1||event.channel>16)return;break;
            case drums::RenderEventKind::stopAll:break;
            default:return;
            }
        }
    }
    state->externalView={};
    if(!external.empty()){
        // Host frame n drives internal frames n*factor..(n+1)*factor-1.
        for(int n=0;n<count;++n){const auto& x=external[static_cast<std::size_t>(n)];
            const Stereo finite{std::isfinite(x[0])?x[0]:0.,std::isfinite(x[1])?x[1]:0.};
            for(int phase=0;phase<state->factor;++phase)state->externalFrames[static_cast<std::size_t>(n*state->factor+phase)]=finite;
        }
        state->externalView=std::span<const Stereo>(state->externalFrames).first(static_cast<std::size_t>(count*state->factor));
    }
    if(state->monitor){FxMonitor::ActiveSlots active{};
        for(std::size_t i=0;i<active.size();++i)active[i]=state->active.layout.effects[i].use==SlotUse::present&&state->active.layout.effects[i].kind==EffectKind::dynamicEq;
        (void)state->monitor->beginBlock(state->rate,active,!external.empty());
        for(std::uint8_t i=0;i<8;++i){
            auto& eq=state->effects[i].dynamicEq;
            if(!active[i] || state->active.effects[i].bypassed){state->monitor->cancelAudition(i);eq.cancelAudition();}
            else {
                const auto request=state->monitor->auditionRequest(i);
                const auto band=request?std::optional<std::size_t>(request->band):std::nullopt;
                bool admitted=false;
                if(request&&request->detector){
                    const auto source=eq.settings().bands[request->band].detectorSource;
                    const bool unavailable=external.empty()&&(source==DynamicEqDetectorSource::externalBand||source==DynamicEqDetectorSource::externalFree);
                    admitted=!unavailable&&eq.setDetectorAudition(band);
                }else admitted=eq.setAudition(band);
                if(!admitted){state->monitor->cancelAudition(i);(void)eq.setAudition(std::nullopt);}
            }
        }
        FxMonitor::ActiveSlots saturationActive{};
        for(std::size_t i=0;i<saturationActive.size();++i)saturationActive[i]=state->requested.layout.effects[i].use==SlotUse::present
            &&state->requested.layout.effects[i].kind==EffectKind::saturation&&!state->requested.effects[i].bypassed
            &&state->requested.effects[i].saturation.mode==SaturationMode::threeBand;
        state->monitor->beginSaturationBlock(saturationActive);
        for(std::uint8_t i=0;i<8;++i){auto& saturation=state->effects[i].saturation;
            if(!saturationActive[i]||!saturation.setAudition(state->monitor->saturationAuditionLane(i))){
                state->monitor->cancelSaturationAudition(i);saturation.cancelAudition();
            }
        }
    }
    auto host=juce::dsp::AudioBlock<float>(out);
    juce::AudioBuffer<float> internal;
    auto* output=&out;
    if(state->resampler)
    {
        auto high=state->resampler->processSamplesUp(host);
        std::array<float*,2> channels{high.getChannelPointer(0),high.getChannelPointer(1)};
        count*=state->factor; internal.setDataToReferTo(channels.data(),2,count); output=&internal;
    }
    if (tempoBpm && (!std::isfinite(*tempoBpm) || *tempoBpm < 20 || *tempoBpm > 400)) tempoBpm.reset();
    state->missingTempo = false;
    for (size_t i = 0; i < 8; ++i)
        state->missingTempo |= state->requested.layout.effects[i].use == SlotUse::present
            && state->requested.layout.effects[i].kind == EffectKind::delay && !state->requested.effects[i].bypassed
            && state->requested.effects[i].delay.synced && !tempoBpm;
    state->updateTempo(tempoBpm);
    std::array<juce::AudioBuffer<float>,4> views;
    std::array<juce::AudioBuffer<float>*,4> pointers;
    for (size_t i = 0; i < 4; ++i)
    {
        views[i].setDataToReferTo(state->buffers[i].getArrayOfWritePointers(),2,count);
        pointers[i] = &views[i];
    }
    const auto writeFrame=[&](Stereo mixed,int frame,double fade){
        if(state->spectralDelay)mixed=state->pad(mixed,masterChain,state->outputPad);
        const double outputGain = state->outputGain.next();
        for (size_t ch = 0; ch < 2; ++ch)
            mixed[ch] = mixed[ch]*state->masterGains[static_cast<size_t>(frame)]*fade*outputGain;
        state->outputEnvelope = std::max({std::abs(mixed[0]),std::abs(mixed[1]),state->outputEnvelope*state->outputRelease});
        const double protectionGain = !state->resampler && state->requested.outputProtection && state->outputEnvelope > state->outputCeiling
            ? state->outputCeiling/state->outputEnvelope : 1;
        for (int ch = 0; ch < 2; ++ch) {
            const auto value=mixed[static_cast<size_t>(ch)]*protectionGain;
            // The double EQ cascade has much greater headroom than a host float
            // or the Higher FIR's float intermediates. Preserve ordinary samples
            // exactly; overload is finite saturation, with the existing final
            // protection still owned by the host-rate stage.
            const auto bounded=std::isnan(value)?0.:std::clamp(value,-state->floatLimit,state->floatLimit);
            output->setSample(ch,frame,static_cast<float>(bounded));
        }
    };
    const auto end=midi.cend();
    if(state->sourceAware){
        for(std::size_t bank=0;bank<4;++bank){const bool ok=authoritativeDrums
            ?state->drums[bank].beginRender(views[bank],events,static_cast<std::uint8_t>(bank),state->factor)
            :state->drums[bank].beginRender(views[bank],midi.cbegin(),end,state->factor);jassert(ok);(void)ok;}
        auto next=midi.cbegin();std::size_t nextDrum=0;int first=0;unsigned admissions=0;bool worked=false;
        while(first<count){
            bool boundaryFade=false;double zeroFade=0;
            if(state->switching&&state->transition.remaining<=1){zeroFade=state->transition.next();
                state->switchRouting();if(state->spaceInstallPending)break;
                state->switching=false;state->transition.set(1,state->rampSamples);state->updateTempo(tempoBpm);boundaryFade=true;
            }
            state->sourceBoundaries();
            const auto group=next;while(next!=end&&std::int64_t((*next).samplePosition)*state->factor<=first)++next;
            while(nextDrum<events.size()&&std::int64_t(events[nextDrum].hostOffset)*state->factor<=first)++nextDrum;
            int length=std::min(state->sourceSpan(first,count),512-first%512);
            if(next!=end){const auto offset=std::int64_t((*next).samplePosition)*state->factor;if(offset<count)length=std::min(length,static_cast<int>(offset)-first);}
            if(nextDrum<events.size())length=std::min(length,events[nextDrum].hostOffset*state->factor-first);
            jassert(length>0);if(length<=0)break;
            for(auto it=group;it!=next;++it){const auto event=*it;if(event.samplePosition>=0&&event.numBytes==1&&event.data[0]==0xfc){for(auto& effect:state->effects)effect.clear();state->clearAllSpace();if(state->monitor)state->monitor->invalidate();state->outputEnvelope=0;state->updateTempo(tempoBpm);}}
            for(std::size_t bank=0;bank<4;++bank)views[bank].setDataToReferTo(state->buffers[bank].getArrayOfWritePointers(),2,first,length);
            const auto gains=std::span(state->masterGains).subspan(static_cast<std::size_t>(first),static_cast<std::size_t>(length));
            const bool space=spacePresence(state->active)||spacePresence(state->requested);
            const bool eq=!space&&state->masterEqBlock(midi);
            if(eq){for(auto& effect:state->effects)effect.advanceFilterBlock(length);
                for(std::size_t pos=0;pos<state->active.layout.effectCount[masterChain];++pos){const auto slot=state->active.layout.effectOrder[masterChain][pos];if(!state->active.effects[slot].bypassed)for(auto& control:state->effects[slot].controls)(void)control.advance(length);}}
            state->sourceSegmentBase=first;state->borrowedSegment=true;state->parallelSynthOnly=space;state->parallelSpaceFx=false;state->parallelPacketFrames=State::pipelineChunkFrames;
            const bool parallel=state->pipeline.available()&&count>=64&&(space||(eq&&state->active.layout.effectCount[masterChain]>=2))
                &&state->synth.beginParallelLayerSegment(pointers,gains,group,next,tempoBpm,true,state->excludedSources());
            bool streamed=false;
            if(parallel){
                bool streamFx=space&&!state->switching&&state->active.layout==state->requested.layout;
                for(std::size_t bank=0;bank<4;++bank)streamFx&=state->active.layout.effectCount[bank]==0;
                for(std::size_t pos=0;pos<state->active.layout.effectCount[masterChain];++pos){const auto slot=state->active.layout.effectOrder[masterChain][pos];streamFx&=state->active.layout.effects[slot].kind==EffectKind::spaceReverb&&state->requested.layout.effects[slot].kind==EffectKind::spaceReverb&&state->active.effects[slot].bypassed==state->requested.effects[slot].bypassed;}
                state->parallelSpaceFx=streamFx;state->parallelSpaceFxOk=true;state->parallelSpaceAdmissions=admissions;state->parallelTempo=tempoBpm;state->parallelSpaceOffset=first;
                state->parallelCount=length;for(auto& ready:state->readyStreams)ready.token.store(0,std::memory_order_relaxed);
                state->pipeline.begin();unsigned cursor=3;const auto packets=static_cast<unsigned>((length+State::pipelineChunkFrames-1)/State::pipelineChunkFrames);
                while(!state->parallelComplete(packets))if(!state->tryReadyPacket(cursor))State::relaxPacketWait();
                state->pipeline.finishUnstarted();state->synth.endParallelLayers();state->parallelCount=0;
                if(!state->parallelSpaceFxOk){jassertfalse;break;}admissions=state->parallelSpaceAdmissions;streamed=streamFx;worked=true;
            }else{
                state->synth.renderLayerSegment(pointers,gains,group,next,first,tempoBpm,state->excludedSources());state->sourceRange(0,4,0,length);
                if(eq){state->mixMasterEqFrames(first,length);state->masterEqRange(first,length,0,state->active.layout.effectCount[masterChain]);}
            }
            if(space&&!streamed&&!state->spaceSegment(first,length,tempoBpm,admissions,first%512==0)){jassertfalse;break;}
            for(int frame=first;frame<first+length;++frame){
                Stereo mixed{};
                if(space||eq)mixed=state->masterEqFrames[static_cast<std::size_t>(frame)];
                else{for(auto& effect:state->effects)effect.advanceFilter();
                    for(std::size_t bank=0;bank<4;++bank){Stereo input{state->buffers[bank].getSample(0,frame),state->buffers[bank].getSample(1,frame)};if(state->active.layout.synthUse[bank]!=SlotUse::present)input={};
                        auto processed=state->chain(input,bank,tempoBpm,state->externalView.empty()?Stereo{}:state->externalView[static_cast<std::size_t>(frame)]);
                        if(state->spectralDelay&&state->active.layout.synthUse[bank]==SlotUse::present)processed=state->pad(processed,bank,state->insertPad[bank]);
                        const double gate=state->gates[bank].next();for(std::size_t ch=0;ch<2;++ch)mixed[ch]+=processed[ch]*gate;}
                    mixed=state->chain(mixed,masterChain,tempoBpm,state->externalView.empty()?Stereo{}:state->externalView[static_cast<std::size_t>(frame)]);
                }
                writeFrame(mixed,frame,boundaryFade&&frame==first?zeroFade:state->transition.next());
            }
            first+=length;
        }
        if(worked){++state->parallelBlocks;state->parallelFrames+=static_cast<std::uint64_t>(count);}
        state->borrowedSegment=false;for(auto& drum:state->drums)drum.endRender();
    }else{
    if(authoritativeDrums)for(const auto& event:events)if(event.kind==drums::RenderEventKind::exactPad)state->drumUnavailable[event.bank]=true;
    const bool spaceBlock=spacePresence(state->active)||spacePresence(state->requested);
    const bool masterEqBlock = !spaceBlock&&state->masterEqBlock(midi);
    if(masterEqBlock){
        // No EQ sample reads these retained banks. Advance their exact end state
        // once, before workers borrow the block; ordinary FX keep per-frame clocks.
        for(auto& effect:state->effects)effect.advanceFilterBlock(count);
        for(size_t position=0;position<state->active.layout.effectCount[masterChain];++position){
            const auto slot=state->active.layout.effectOrder[masterChain][position];
            if(!state->active.effects[slot].bypassed)
                for(auto& control:state->effects[slot].controls)(void)control.advance(count);
        }
    }
    state->parallelSynthOnly=spaceBlock;
    state->parallelSpaceFx=false;
    state->parallelPacketFrames=State::pipelineChunkFrames;
    bool noteSegments=true;
    if(spaceBlock)for(const auto event:midi)if(event.samplePosition<0||event.numBytes!=3||event.data[1]>127||event.data[2]>127
        ||((event.data[0]&0xf0)!=0x80&&(event.data[0]&0xf0)!=0x90)){noteSegments=false;break;}
    auto nextNote=midi.cbegin();const auto notesEnd=midi.cend();int segmentLength=count;
    const auto beginSegment=[&](int first){
        const auto groupFirst=nextNote;
        while(nextNote!=notesEnd&&std::int64_t((*nextNote).samplePosition)*state->factor<=first)++nextNote;
        segmentLength=count-first;
        if(nextNote!=notesEnd){const auto offset=std::int64_t((*nextNote).samplePosition)*state->factor;
            if(offset< count)segmentLength=static_cast<int>(offset)-first;}
        for(size_t layer=0;layer<4;++layer)views[layer].setDataToReferTo(state->buffers[layer].getArrayOfWritePointers(),2,first,segmentLength);
        return state->synth.beginParallelLayerSegment(pointers,std::span(state->masterGains).subspan(static_cast<size_t>(first),static_cast<size_t>(segmentLength)),groupFirst,nextNote,tempoBpm,true);
    };
    const bool parallel = state->pipeline.available() && (spaceBlock||masterEqBlock) && count >= 64
        && (spaceBlock||state->active.layout.effectCount[masterChain] >= 2)
        && (spaceBlock ? noteSegments&&beginSegment(0)
                       : state->synth.beginParallelLayers(pointers,std::span(state->masterGains).first(static_cast<size_t>(count)),midi,tempoBpm));
    bool streamedSpace=false;
    if (!parallel){
        if(spaceBlock)for(size_t layer=0;layer<4;++layer)views[layer].setDataToReferTo(state->buffers[layer].getArrayOfWritePointers(),2,count);
        state->synth.renderLayers(pointers,std::span<float>(state->masterGains.data(),static_cast<size_t>(count)),midi,tempoBpm);
    }
    else if(spaceBlock)
    {
        bool streamFx=!state->switching&&state->active.layout==state->requested.layout;
        for(std::size_t layer=0;layer<masterChain;++layer)streamFx=streamFx&&state->active.layout.effectCount[layer]==0;
        for(std::size_t n=0;n<state->active.layout.effectCount[masterChain];++n){const auto slot=state->active.layout.effectOrder[masterChain][n];
            streamFx=streamFx&&state->active.layout.effects[slot].kind==EffectKind::spaceReverb&&state->requested.layout.effects[slot].kind==EffectKind::spaceReverb
                &&state->active.effects[slot].bypassed==state->requested.effects[slot].bypassed;
        }
        state->parallelSpaceFx=streamFx;state->parallelSpaceFxOk=true;state->parallelSpaceAdmissions=0;state->parallelTempo=tempoBpm;
        int first=0;
        for(;;){
            state->parallelCount=segmentLength;
            state->parallelSpaceOffset=first;
            for(auto& stream:state->readyStreams)stream.token.store(0,std::memory_order_relaxed);
            state->pipeline.begin();
            const auto packets=static_cast<unsigned>((state->parallelCount+state->parallelPacketFrames-1)/state->parallelPacketFrames);
            unsigned readyCursor=3;
            while(!state->parallelComplete(packets))
                if(!state->tryReadyPacket(readyCursor))State::relaxPacketWait();
            state->pipeline.finishUnstarted();
            state->synth.endParallelLayers();
            if(!state->parallelSpaceFxOk){state->parallelCount=0;jassertfalse;out.clear();return;}
            first+=segmentLength;if(first==count)break;
            if(!beginSegment(first)){jassertfalse;out.clear();return;}
        }
        state->parallelCount=0;streamedSpace=streamFx;
        ++state->parallelBlocks;state->parallelFrames+=static_cast<std::uint64_t>(count);
    }
    auto event = midi.cbegin();
    if(spaceBlock)
    {
        unsigned admissions=0;int first=0;
        while(first<count){
            while(event!=end&&std::int64_t((*event).samplePosition)*state->factor<=first){
                const auto metadata=*event;
                if(metadata.samplePosition>=0&&metadata.numBytes==1&&metadata.data[0]==0xfc){for(auto& effect:state->effects)effect.clear();state->clearAllSpace();if(state->monitor)state->monitor->invalidate();state->outputEnvelope=0;state->updateTempo(tempoBpm);}
                ++event;
            }
            bool boundaryFade=false;double firstFade=0;
            if(state->switching&&state->transition.remaining<=1){firstFade=state->transition.next();boundaryFade=true;state->switchRouting();state->switching=false;state->transition.set(1,state->rampSamples);state->updateTempo(tempoBpm);}
            int length=std::min(512,count-first);
            if(state->switching)length=std::min(length,state->transition.remaining-1);
            for(auto next=event;next!=end;++next){const auto metadata=*next;if(metadata.numBytes==1&&metadata.data[0]==0xfc){const auto offset=std::int64_t(metadata.samplePosition)*state->factor;if(offset>first&&offset<first+length)length=static_cast<int>(offset-first);break;}}
            if(!streamedSpace&&!state->spaceSegment(first,length,tempoBpm,admissions)){jassertfalse;break;}
            for(int n=0;n<length;++n)writeFrame(state->masterEqFrames[static_cast<std::size_t>(first+n)],first+n,boundaryFade&&n==0?firstFade:state->transition.next());
            first+=length;
        }
    }
    else if (parallel)
    {
        state->parallelCount = count;
        for(auto& stream:state->readyStreams)stream.token.store(0,std::memory_order_relaxed);
        state->pipeline.begin();
        unsigned readyCursor=3;
        for (int first = 0, packet = 1; first < count; first += State::pipelineChunkFrames, ++packet)
        {
            const int length = std::min(State::pipelineChunkFrames, count - first);
            // Render any ready packet while preserving original per-stream sample order.
            while(state->readyPackets(state->parallelStreams()-1)<static_cast<unsigned>(packet))
                if(!state->tryReadyPacket(readyCursor))State::relaxPacketWait();
            for (int frame = first; frame < first + length; ++frame)
                writeFrame(state->masterEqFrames[static_cast<size_t>(frame)], frame, state->transition.next());
        }
        state->pipeline.finishUnstarted();
        state->synth.endParallelLayers(); state->parallelCount = 0;
        ++state->parallelBlocks; state->parallelFrames += static_cast<std::uint64_t>(count);
    }
    else if (masterEqBlock)
    {
        state->mixMasterEqFrames(0, count);
        state->masterEqRange(0, count, 0, state->active.layout.effectCount[masterChain]);
        for(int frame=0;frame<count;++frame)writeFrame(state->masterEqFrames[static_cast<size_t>(frame)],frame,state->transition.next());
    }
    else
    for (int frame = 0; frame < count; ++frame)
    {
        while (event != end && std::int64_t((*event).samplePosition)*state->factor <= frame)
        {
            const auto metadata = *event;
            if (metadata.samplePosition >= 0 && metadata.numBytes == 1 && metadata.data[0] == 0xfc)
            { for (auto& effect : state->effects) effect.clear();if(state->monitor)state->monitor->invalidate(); state->outputEnvelope = 0; state->updateTempo(tempoBpm); }
            ++event;
        }
        const double fade = state->transition.next();
        if (state->switching && state->transition.remaining == 0)
        { state->switchRouting(); state->switching = false; state->transition.set(1,state->rampSamples); state->updateTempo(tempoBpm); }
        for(auto& effect:state->effects)effect.advanceFilter();
        Stereo mixed {};
        for (size_t layer = 0; layer < 4; ++layer)
        {
            Stereo input {views[layer].getSample(0,frame),views[layer].getSample(1,frame)};
            if (state->active.layout.synthUse[layer] != SlotUse::present) input = {};
            auto processed = state->chain(input,layer,tempoBpm,state->externalView.empty()?Stereo{}:state->externalView[static_cast<std::size_t>(frame)]);
            if(state->spectralDelay && state->active.layout.synthUse[layer]==SlotUse::present)processed=state->pad(processed,layer,state->insertPad[layer]);
            const double gate = state->gates[layer].next();
            for (size_t ch = 0; ch < 2; ++ch) mixed[ch] += processed[ch]*gate;
        }
        mixed = state->chain(mixed,4,tempoBpm,state->externalView.empty()?Stereo{}:state->externalView[static_cast<std::size_t>(frame)]);
        writeFrame(mixed,frame,fade);
    }
    }
    if(state->resampler)
    {
        // One high-rate sample completes an exact integer host-frame delay;
        // no fractional interpolation or frequency-dependent phase fitting.
        if(state->delayHighSample)for(size_t ch=0;ch<2;++ch)for(int n=0;n<count;++n)
            std::swap(state->lastHigh[ch],output->getWritePointer(static_cast<int>(ch))[n]);
        state->resampler->processSamplesDown(host);
        // The accepted sample ceiling belongs to the final host stream. FIR
        // ringing is included in its linked detector and 50ms release history.
        auto stop = midi.cbegin();
        for(int n=0;n<out.getNumSamples();++n)
        {
            while(stop!=end && (*stop).samplePosition<=n)
            {
                const auto metadata=*stop;
                if(metadata.samplePosition>=0 && metadata.numBytes==1 && metadata.data[0]==0xfc)
                    state->protectionClearRemaining=2*state->latency;
                ++stop;
            }
            // Stop retains the real FIR history. Once its finite support has
            // drained, the new quiet note must not inherit prior overload.
            if(state->protectionClearRemaining==0){state->hostOutputEnvelope=0;state->protectionClearRemaining=-1;}
            else if(state->protectionClearRemaining>0)--state->protectionClearRemaining;
            state->hostOutputEnvelope=std::max({std::abs(double(out.getSample(0,n))),std::abs(double(out.getSample(1,n))),state->hostOutputEnvelope*state->hostOutputRelease});
            const double gain=state->requested.outputProtection && state->hostOutputEnvelope>state->outputCeiling ? state->outputCeiling/state->hostOutputEnvelope : 1;
            for(int ch=0;ch<2;++ch)out.setSample(ch,n,static_cast<float>(out.getSample(ch,n)*gain));
        }
    }
    if(state->monitor)for(std::uint8_t i=0;i<8;++i)state->monitor->auditionRunning(i,state->effects[i].dynamicEq.auditioning());
}
void RoutedSynthEngine::reset() noexcept
{
    if(!state->finishSpaceInstall())return;
    if(state->spacePending){
        auto cancel=spaceRequestFor(state->requested,state->spaceIdentities,++state->spaceKey,255);cancel.activeMask=0;
        state->spaceActivation.request(cancel);state->spacePending=false;
        state->pendingSpaceSourceValid=false;state->spaceInstallPreviousSourceValid=false;
    }
    if(state->monitor){state->monitor->invalidate();(void)state->monitor->publishSampleRate(state->rate);}
    if(state->resampler)state->resampler->reset();
    state->lastHigh={};
    state->clearAllSpace();
    state->externalView={};
    state->synth.reset(); state->active = state->requested; state->switching = false; state->missingTempo = false;
    if(state->sourceAware)state->commitSource(state->committedSource,true);
    state->alignRouting();
    state->transition.set(1,state->rampSamples,true); state->gatesToTarget(true);
    if(!spacePresence(state->active))(void)state->spaceActivation.acknowledgeInactive();
    state->outputEnvelope = 0;
    state->hostOutputEnvelope = 0;
    state->protectionClearRemaining = -1;
    state->outputGain.set(std::pow(10.0,double(state->requested.outputGainDb)/20),state->rampSamples,true);
    for (size_t i = 0; i < 8; ++i) {
        // Space owns separate histories; switching to any legacy kind clears its banks before use.
        if(state->requested.layout.effects[i].kind!=EffectKind::spaceReverb)state->effects[i].clear();
        state->effects[i].settings(state->requested.effects[i],state->rampSamples,true);
    }
}
RoutedSynthEngine::DspWorkerStats RoutedSynthEngine::dspWorkerStats() const noexcept
{ return {state->pipeline.workerCount(), state->parallelBlocks, state->parallelFrames}; }
#ifndef NDEBUG
RoutedSynthEngine::DrumTestSnapshot RoutedSynthEngine::drumTestSnapshot() const noexcept {
    DrumTestSnapshot out;out.committed=&state->committedSource.patch;out.pending=&state->pendingSpaceSource.patch;out.previous=&state->spaceInstallPreviousSource.patch;
    out.committedAssignments=state->committedSource.assignments;out.pendingAssignments=state->pendingSpaceSource.assignments;out.previousAssignments=state->spaceInstallPreviousSource.assignments;
    out.committedIdentity=state->committedSource.identity;out.pendingIdentity=state->pendingSpaceSource.identity;out.previousIdentity=state->spaceInstallPreviousSource.identity;
    out.passedGeneration=state->passedGeneration;out.installedIdentity=installedDrumIdentity();
    out.pendingValid=state->pendingSpaceSourceValid;out.previousValid=state->spaceInstallPreviousSourceValid;out.installPending=state->spaceInstallPending;out.preparing=state->spacePending;
    out.transitionRemaining=state->transition.remaining;
    out.streams=state->parallelStreams();for(unsigned stream=0;stream<6;++stream)out.ready[stream]=state->readyPackets(stream);
    for(std::size_t bank=0;bank<4;++bank){out.installed[bank]=state->sources[bank].drum;out.target[bank]=state->sources[bank].targetDrum;out.sourceRemaining[bank]=state->sources[bank].gain.remaining;out.gateRemaining[bank]=state->gates[bank].remaining;out.hits[bank]=state->drums[bank].activeHitCount();
        out.configuredEnabled[bank]=state->drums[bank].settings_.enabled;
        for(std::size_t pad=0;pad<16;++pad)out.references[bank][pad]=state->drums[bank].settings_.pads[pad].sample;
        for(std::size_t hit=0;hit<32;++hit)out.references[bank][16+hit]=state->drums[bank].hits_[hit].sample;
    }return out;
}
void RoutedSynthEngine::setDrumSourceBorrowTestHook(void* context,void (*hook)(void*,unsigned)) noexcept
{state->sourceBorrowTestContext=context;state->sourceBorrowTestHook=hook;}
#endif
std::size_t RoutedSynthEngine::activeNoteCount() const noexcept { return state->synth.activeNoteCount(); }
void RoutedSynthEngine::attachFxMonitor(FxMonitor* monitor) noexcept
{
    if(state->monitor==monitor)return;
    if(state->monitor){state->monitor->invalidate();(void)state->monitor->publishSampleRate(0);}
    for(auto& effect:state->effects){effect.dynamicEq.cancelAudition();effect.saturation.cancelAudition();}
    state->monitor=monitor;
    if(monitor){monitor->invalidate();(void)monitor->publishSampleRate(state->rate);}
}
bool RoutedSynthEngine::tempoUnavailable() const noexcept { return state->missingTempo || state->synth.tempoUnavailable(); }
}
