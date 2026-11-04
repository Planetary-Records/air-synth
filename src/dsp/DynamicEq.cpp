#include "DynamicEq.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace airack
{
namespace
{
using Coefficients = std::array<double, 5>;
using Chain = std::array<Coefficients, 2>;
using StereoRegister = juce::dsp::SIMDRegister<double>;
constexpr Coefficients identity {1, 0, 0, 0, 0};
constexpr Chain identityChain {identity, identity};
bool range(float value, float lo, float hi) noexcept
{ return std::isfinite(value) && value >= lo && value <= hi; }
bool validRate(double rate) noexcept { return std::isfinite(rate) && rate >= 8000 && rate <= 1536000; }
bool freeDetector(DynamicEqDetectorSource source) noexcept
{ return source==DynamicEqDetectorSource::internalFree||source==DynamicEqDetectorSource::externalFree; }
bool externalDetector(DynamicEqDetectorSource source) noexcept
{ return source==DynamicEqDetectorSource::externalBand||source==DynamicEqDetectorSource::externalFree; }
double detectorAttackMs(const DynamicEqBand& b) noexcept
{ return b.adaptiveTiming?std::clamp(double(b.attackMs)*std::sqrt(1000./b.frequencyHz),.1,200.):b.attackMs; }
double detectorReleaseMs(const DynamicEqBand& b,double movement) noexcept
{ return b.adaptiveTiming?std::clamp(double(b.releaseMs)*(1+std::min(std::abs(movement)/12,1.)),10.,2000.):b.releaseMs; }
float learnedThresholdDb(double power) noexcept
{ return float(power>0?std::clamp(4.3429448190325182765*std::log(power)+6.,-60.,0.):-60.); }
bool validBand(const DynamicEqBand& b) noexcept
{
    return b.shape <= DynamicEqShape::flatTilt && b.slope <= DynamicEqSlope::db96 && b.placement <= DynamicEqPlacement::side
        && range(b.frequencyHz, 20, 20000) && range(b.gainDb, -24, 24) && range(b.q, .25f, 18)
        && range(b.rangeDb, -24, 24) && range(b.thresholdDb, -60, 0)
        && range(b.attackMs, .1f, 200) && range(b.releaseMs, 10, 2000)
        && b.detectorSource<=DynamicEqDetectorSource::externalFree
        && range(b.detectorLowHz,20,20000)&&range(b.detectorHighHz,20,20000)
        && range(b.spectralDensity,0,1)&&range(b.spectralTiltDbOct,-6,6)
        && (!freeDetector(b.detectorSource)||b.detectorLowHz<b.detectorHighHz);
}
bool enabled(const DynamicEqBand& b) noexcept { return b.active && !b.bypassed; }
bool dynamicShape(const DynamicEqBand& b) noexcept { return b.shape <= DynamicEqShape::highShelf; }
bool gainShape(const DynamicEqBand& b) noexcept
{ return dynamicShape(b) || b.shape==DynamicEqShape::tiltShelf || b.shape==DynamicEqShape::flatTilt; }
bool neutralTilt(const DynamicEqBand& b) noexcept
{ return (b.shape==DynamicEqShape::tiltShelf||b.shape==DynamicEqShape::flatTilt)&&b.gainDb==0; }
bool extendedShape(const DynamicEqBand& b) noexcept
{
    return b.shape >= DynamicEqShape::bandPass
        || ((b.shape == DynamicEqShape::lowCut || b.shape == DynamicEqShape::highCut) && b.slope >= DynamicEqSlope::db6);
}
DynamicEqBand scaledBand(const DynamicEqBand& raw, float scale) noexcept
{
    auto band = raw;
    if (scale != 1 && gainShape(band)) band.gainDb = std::clamp(band.gainDb * scale, -24.f, 24.f);
    return band;
}
double gainWeight(const DynamicEqBand& b) noexcept
{
    constexpr double logSpan = 6.9077552789821370521; // ln(20000/20)
    switch (b.shape) {
        case DynamicEqShape::bell: return std::clamp(2*std::asinh(1/(2*double(b.q)))/logSpan, 0., 1.);
        case DynamicEqShape::lowShelf: return std::clamp(std::log(double(b.frequencyHz)/20)/logSpan, 0., 1.);
        case DynamicEqShape::highShelf: return 1-std::clamp(std::log(double(b.frequencyHz)/20)/logSpan, 0., 1.);
        case DynamicEqShape::tiltShelf: return .5-std::clamp(std::log(double(b.frequencyHz)/20)/logSpan, 0., 1.);
        case DynamicEqShape::flatTilt: {
            double weight=-.5;
            for(double octave:{-2.5,-1.5,-.5,.5,1.5,2.5})
                weight+=(1-std::clamp(std::log(double(b.frequencyHz)*std::exp2(octave)/20)/logSpan,0.,1.))/6;
            return weight;
        }
        default: return 0;
    }
}
double endpointGain(const DynamicEqBand& b) noexcept
{ return dynamicShape(b) ? std::clamp(double(b.gainDb)+b.rangeDb, -24., 24.) : b.gainDb; }
double amplitude(double db) noexcept { return std::exp(db * .1151292546497022842); }
double interpolation(double base, double endpoint, double dynamicGain) noexcept
{
    const auto a0 = amplitude(base), a1 = amplitude(endpoint);
    return std::abs(a1-a0) < 1e-12 ? 0. : std::clamp((amplitude(base+dynamicGain)-a0)/(a1-a0), 0., 1.);
}
Coefficients normalize(const std::array<double, 6>& c) noexcept
{ return {c[0]/c[3], c[1]/c[3], c[2]/c[3], c[4]/c[3], c[5]/c[3]}; }
Chain detectorCoefficients(const DynamicEqBand& b,double rate) noexcept
{
    using Factory=juce::dsp::IIR::ArrayCoefficients<double>;
    Chain result=identityChain;
    if(freeDetector(b.detectorSource)){
        result[0]=normalize(Factory::makeHighPass(rate,std::min(double(b.detectorLowHz),.45*rate),std::sqrt(.5)));
        result[1]=normalize(Factory::makeLowPass(rate,std::min(double(b.detectorHighHz),.45*rate),std::sqrt(.5)));
    }else{
        const auto hz=std::min(double(b.frequencyHz),.45*rate);
        if(b.shape==DynamicEqShape::bell)result[0]=normalize(Factory::makeBandPass(rate,hz,b.q));
        else if(b.shape==DynamicEqShape::lowShelf)result[0]=normalize(Factory::makeLowPass(rate,hz,std::sqrt(.5)));
        else if(b.shape==DynamicEqShape::highShelf)result[0]=normalize(Factory::makeHighPass(rate,hz,std::sqrt(.5)));
    }
    return result;
}
Chain coefficients(const DynamicEqBand& b, double rate, double gain) noexcept
{
    using Factory = juce::dsp::IIR::ArrayCoefficients<double>;
    Chain result = identityChain;
    const double hz = std::min(double(b.frequencyHz), .45 * rate), q = b.q;
    switch (b.shape) {
        case DynamicEqShape::bell:
            if (gain != 0) result[0] = normalize(Factory::makePeakFilter(rate, hz, q, amplitude(gain)));
            break;
        case DynamicEqShape::lowShelf:
            if (gain != 0) result[0] = normalize(Factory::makeLowShelf(rate, hz, std::min(q, 4.), amplitude(gain)));
            break;
        case DynamicEqShape::highShelf:
            if (gain != 0) result[0] = normalize(Factory::makeHighShelf(rate, hz, std::min(q, 4.), amplitude(gain)));
            break;
        case DynamicEqShape::lowCut:
        case DynamicEqShape::highCut: {
            const auto cut = [&](double quality) {
                return normalize(b.shape == DynamicEqShape::lowCut ? Factory::makeHighPass(rate, hz, quality)
                                                                  : Factory::makeLowPass(rate, hz, quality));
            };
            result[0] = cut(b.slope == DynamicEqSlope::db12 ? std::sqrt(.5) : .541196100146197);
            if (b.slope == DynamicEqSlope::db24) result[1] = cut(1.306562964876377);
            break;
        }
        case DynamicEqShape::notch: result[0] = normalize(Factory::makeNotch(rate, hz, q)); break;
        default: break;
    }
    return result;
}
struct ExtendedChain
{
    std::array<Coefficients,8> sections;
    int count = 0;
    double trim = 1;
};
ExtendedChain extendedCoefficients(const DynamicEqBand& b,double rate) noexcept
{
    using Factory=juce::dsp::IIR::ArrayCoefficients<double>;
    ExtendedChain result;result.sections.fill(identity);
    const double hz=std::min(double(b.frequencyHz),.45*rate);
    if(b.shape==DynamicEqShape::bandPass){
        result.sections[0]=normalize(Factory::makeBandPass(rate,hz,b.q));result.count=1;
    }else if(b.shape==DynamicEqShape::tiltShelf){
        const auto q=std::min(double(b.q),4.);
        result.sections[0]=normalize(Factory::makeLowShelf(rate,hz,q,amplitude(-b.gainDb*.5)));
        result.sections[1]=normalize(Factory::makeHighShelf(rate,hz,q,amplitude(b.gainDb*.5)));
        result.count=2;
    }else if(b.shape==DynamicEqShape::flatTilt){
        for(double octave:{-2.5,-1.5,-.5,.5,1.5,2.5})
            result.sections[result.count++]=normalize(Factory::makeHighShelf(rate,std::min(double(b.frequencyHz)*std::exp2(octave),.49*rate),std::sqrt(.5),amplitude(b.gainDb/6.)));
        result.count=6;
        result.trim=amplitude(-b.gainDb*.5);
    }else{
        const bool high=b.shape==DynamicEqShape::lowCut;
        if(b.slope==DynamicEqSlope::db6){
            const double t=std::tan(std::numbers::pi*hz/rate),norm=1./(1.+t);
            result.sections[0]=high?Coefficients{norm,-norm,0,(t-1)*norm,0}:Coefficients{t*norm,t*norm,0,(t-1)*norm,0};result.count=1;
        }else{
            const int order=b.slope==DynamicEqSlope::db36?6:b.slope==DynamicEqSlope::db48?8:16;
            for(int k=0;k<order/2;++k){
                const double q=1./(2.*std::cos((2.*k+1.)*std::numbers::pi/(2.*order)));
                result.sections[result.count++]=normalize(high?Factory::makeHighPass(rate,hz,q):Factory::makeLowPass(rate,hz,q));
            }
        }
    }
    return result;
}
std::complex<double> response(const Chain& c, double rate, double hz) noexcept
{
    const auto z = std::polar(1., -2 * std::numbers::pi * hz / rate);
    std::complex<double> result = 1.;
    for (const auto& s : c) result *= (s[0] + s[1]*z + s[2]*z*z) / (1. + s[3]*z + s[4]*z*z);
    return result;
}
double poleRadius(const Coefficients& c) noexcept
{
    const auto discriminant = c[3]*c[3] - 4*c[4];
    if (discriminant < 0) return std::sqrt(c[4]);
    const auto root = std::sqrt(discriminant);
    return std::max(std::abs((-c[3]+root)*.5), std::abs((-c[3]-root)*.5));
}
struct Ramp
{
    double value = 0, target = 0, step = 0;
    int left = 0;
    void set(double next, int samples, bool snap = false) noexcept
    {
        if (snap) { value = target = next; left = 0; step = 0; }
        else if (next != target) { target = next; left = samples; step = (next-value)/samples; }
    }
    double tick() noexcept
    {
        if (left > 0) { value += step; if (--left == 0) value = target; }
        return value;
    }
};
struct Filter
{
    Chain current = identityChain, target = identityChain, step {};
    // Direct form I keeps physical histories during coefficient ramps.
    struct History { StereoRegister x1, x2, y1, y2; };
    std::array<History, 2> history {};
    int left = 0, stages = 1;
    bool transparent = true;
    void set(const Chain& next, int samples, bool snap = false) noexcept
    {
        stages = next[1] == identity && (snap || current[1] == identity) ? 1 : 2;
        transparent = next == identityChain && (snap || current == identityChain);
        if (snap) { current = target = next; left = 0; history = {}; }
        else if (next == current) { target = next; left = 0; }
        else if (next != target) {
            target = next; left = samples;
            for (std::size_t s = 0; s < 2; ++s)
                for (std::size_t i = 0; i < 5; ++i) step[s][i] = (target[s][i]-current[s][i])/samples;
        }
    }
   #if defined(_MSC_VER) && !defined(_DEBUG)
    __forceinline
   #endif
    StereoRegister process(StereoRegister x) noexcept
    {
        if (transparent) {
            // Preserve the physical input/output histories so a later
            // continuous edit can start its ramp without a history reset.
            auto& h = history[0]; h.x2 = h.x1; h.y2 = h.y1; h.x1 = h.y1 = x;
            return x;
        }
        if (left > 0) {
            for (std::size_t s = 0; s < 2; ++s)
                for (std::size_t i = 0; i < 5; ++i) current[s][i] += step[s][i];
            if (--left == 0) { current = target; transparent = current == identityChain; stages = current[1] == identity ? 1 : 2; }
        }
        for (int s = 0; s < stages; ++s) {
            const auto& c = current[s];
            auto& h = history[s];
            const auto y = StereoRegister(c[0])*x + StereoRegister(c[1])*h.x1 + StereoRegister(c[2])*h.x2
                         - StereoRegister(c[3])*h.y1 - StereoRegister(c[4])*h.y2;
            h.x2 = h.x1; h.x1 = x; h.y2 = h.y1; h.y1 = y; x = y;
        }
        return x;
    }

    // Caller proves a settled section. Keep the original DF-I expression
    // and physical histories while avoiding a state round trip for each sample.
    template<bool mono=false,std::size_t section=0>
    void settledBlock(std::span<const DynamicEq::Frame> input, std::span<DynamicEq::Frame> output) noexcept
    {
        auto& h=history[section];auto x1=h.x1,x2=h.x2,y1=h.y1,y2=h.y2;
        alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
        const auto load=[&](const DynamicEq::Frame& frame){
            if constexpr(mono)return StereoRegister(frame[0]);
            else{lanes[0]=frame[0];lanes[1]=frame[1];return StereoRegister::fromRawArray(lanes.data());}
        };
        if(transparent){
            for(std::size_t n=0;n<input.size();++n){const auto x=load(input[n]);x2=x1;y2=y1;x1=y1=x;output[n]=input[n];}
        }else{
            const auto& c=current[section];const StereoRegister c0(c[0]),c1(c[1]),c2(c[2]),c3(c[3]),c4(c[4]);
            for(std::size_t n=0;n<input.size();++n){
                const auto x=load(input[n]);
                const auto y=c0*x+c1*x1+c2*x2-c3*y1-c4*y2;
                x2=x1;x1=x;y2=y1;y1=y;y.copyToRawArray(lanes.data());output[n]={lanes[0],lanes[1]};
            }
        }
        h.x1=x1;h.x2=x2;h.y1=y1;h.y2=y2;
    }
};
bool sameStructure(const DynamicEqBand& a, const DynamicEqBand& b) noexcept
{ return enabled(a) == enabled(b) && a.shape == b.shape && a.slope == b.slope && a.placement == b.placement
    &&(!dynamicShape(a)||a.detectorSource==b.detectorSource); }
double selectedComponent(StereoRegister input, DynamicEqPlacement placement) noexcept
{
    const auto l=input.get(0),r=input.get(1);
    switch(placement){
        case DynamicEqPlacement::left:return l;
        case DynamicEqPlacement::right:return r;
        case DynamicEqPlacement::mid:return .5*(l+r);
        case DynamicEqPlacement::side:return .5*(l-r);
        default:return 0;
    }
}
StereoRegister componentFrame(double value, DynamicEqPlacement placement) noexcept
{
    alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
    switch(placement){
        case DynamicEqPlacement::left:lanes[0]=value;break;
        case DynamicEqPlacement::right:lanes[1]=value;break;
        case DynamicEqPlacement::mid:lanes[0]=lanes[1]=value;break;
        case DynamicEqPlacement::side:lanes[0]=value;lanes[1]=-value;break;
        default:break;
    }
    return StereoRegister::fromRawArray(lanes.data());
}
struct ExtendedFilter
{
    std::array<Filter,4> pairs;
    Ramp trim {1,1};
    int count = 1;
    static bool unity(const Chain& chain) noexcept
    {
        for(const auto& c:chain)if(c[0]!=1||c[1]!=c[3]||c[2]!=c[4])return false;
        return true;
    }
    void set(const DynamicEqBand& b,double rate,int samples,bool snap) noexcept
    {
        const auto next=extendedCoefficients(b,rate);
        count=(next.count+1)/2;
        for(std::size_t i=0;i<pairs.size();++i)pairs[i].set({next.sections[i*2],next.sections[i*2+1]},samples,snap);
        trim.set(next.trim,samples,snap);
    }
    StereoRegister process(StereoRegister input) noexcept
    {
        for(int i=0;i<count;++i){
            auto& pair=pairs[static_cast<std::size_t>(i)];
            const auto original=input,previous=pair.history[0].x1;input=pair.process(input);
            // Preserve unit pole geometry and physical histories for continuous gain edits.
            if(unity(pair.current)){
                input=original;
                for(auto& h:pair.history){h.x2=h.y2=previous;h.x1=h.y1=original;}
            }
        }
        return input*StereoRegister(trim.tick());
    }
    template<bool mono>
    void settledBlock(std::span<DynamicEq::Frame> frames) noexcept
    {
        for(int i=0;i<count;++i){
            auto& pair=pairs[static_cast<std::size_t>(i)];
            pair.settledBlock<mono>(frames,frames);
            if(pair.stages==2)pair.settledBlock<mono,1>(frames,frames);
        }
    }
    bool transparent() const noexcept
    {
        if(trim.value!=1)return false;
        for(int i=0;i<count;++i)if(!unity(pairs[static_cast<std::size_t>(i)].current))return false;
        return true;
    }
    bool targetTransparent() const noexcept
    {
        if(trim.target!=1)return false;
        for(int i=0;i<count;++i)if(!unity(pairs[static_cast<std::size_t>(i)].target))return false;
        return true;
    }
};
struct BandState
{
    DynamicEqBand installed;
    struct LegacyFilters { Filter filter, endpoint, detector; };
    union Filters {
        LegacyFilters legacy;
        ExtendedFilter extended;
        Filters() noexcept : legacy{} {}
    } filters;
    Ramp wet, baseGain, rangeEndpoint;
    double dynamicGain = 0, attack = 0, release = 0;
    double baseAmplitude = 1, endpointAmplitude = 1;
    double kneeFloorAmplitude = 0, saturationAmplitude = 0;
    double previousDynamicGain = 0, cachedMix = 0;
    double learnedPower = 0, powerCoefficient = 0;
    bool pending = false;
    void configure(const DynamicEqBand& b, double rate, int samples, bool snap) noexcept
    {
        if(extendedShape(b)){
            filters.extended.set(b,rate,samples,snap);
            if(snap){previousDynamicGain=0;cachedMix=0;}
            return;
        }
        auto& [filter,endpoint,detector]=filters.legacy;
        filter.set(coefficients(b, rate, b.gainDb), samples, snap);
        endpoint.set(coefficients(b, rate, endpointGain(b)), samples, snap);
        detector.set(detectorCoefficients(b,rate), samples, snap);
        baseGain.set(b.gainDb, samples, snap); rangeEndpoint.set(endpointGain(b), samples, snap);
        if (snap) { baseAmplitude = amplitude(baseGain.value); endpointAmplitude = amplitude(rangeEndpoint.value); }
        attack = std::exp(-1./(rate*detectorAttackMs(b)*.001)); release = std::exp(-1./(rate*b.releaseMs*.001));
        if(b.autoThreshold)powerCoefficient=std::exp(-1./(.5*rate));
        kneeFloorAmplitude = amplitude(b.autoThreshold?-3.:b.thresholdDb-3.);
        const auto rangeMagnitude = std::abs(double(b.rangeDb));
        const auto saturationOver = rangeMagnitude <= 2.25 ? -3.+4.*std::sqrt(rangeMagnitude) : rangeMagnitude/.75;
        saturationAmplitude = amplitude((b.autoThreshold?0.:b.thresholdDb)+saturationOver);
        if (snap) { previousDynamicGain = 0; cachedMix = 0; }
    }
    void install(const DynamicEqBand& b, double rate, int samples, bool snap) noexcept
    {
        // Reuse fixed storage; live structural edits reach install only after the wet fade.
        if(extendedShape(b)!=extendedShape(installed)){
            if(extendedShape(b))std::construct_at(&filters.extended);
            else std::construct_at(&filters.legacy);
        }
        installed = b; pending = false;
        configure(b, rate, samples, true); dynamicGain = 0;learnedPower=0;
        wet.set(enabled(b) ? 1 : 0, samples, snap);
    }
    void publish(const DynamicEqBand& b, double rate, int samples) noexcept
    {
        if (!sameStructure(installed, b)) { pending = true; wet.set(0, samples); }
        else {
            pending = false; installed = b;
            wet.set(enabled(b) ? 1 : 0, samples);
            configure(b, rate, samples, false);
        }
    }
    StereoRegister process(StereoRegister input, const DynamicEqBand& b, double rate, int samples,StereoRegister external=StereoRegister(0)) noexcept
    {
        if (pending && wet.value == 0) install(b, rate, samples, false);
        const auto blend = wet.tick();
        if (blend == 0 && !pending) return input;
        const auto original=input;
        if(installed.placement!=DynamicEqPlacement::stereo)
            input=StereoRegister(selectedComponent(input,installed.placement));
        StereoRegister y;
        if(extendedShape(installed)){
            y=filters.extended.process(input);
            if(filters.extended.transparent())return original;
        }
        else {
        auto& [filter,endpoint,detector]=filters.legacy;
        const bool referenceMoving = baseGain.left > 0 || rangeEndpoint.left > 0;
        const auto base = baseGain.tick(), limit = rangeEndpoint.tick();
        if (referenceMoving) { baseAmplitude = amplitude(base); endpointAmplitude = amplitude(limit); }
        double target = 0;
        if (dynamicShape(installed) && (installed.rangeDb != 0||installed.autoThreshold)) {
            const auto trigger=externalDetector(installed.detectorSource)
                ?(installed.placement==DynamicEqPlacement::stereo?external:StereoRegister(selectedComponent(external,installed.placement))):input;
            const auto detected = detector.process(trigger);
            const auto detectedAmplitude = std::max(std::abs(detected.get(0)),std::abs(detected.get(1)));
            if(installed.autoThreshold){
                learnedPower=powerCoefficient*learnedPower+(detectedAmplitude>3.162277660168379e-5?(1-powerCoefficient)*detectedAmplitude*detectedAmplitude:0.);
                if(learnedPower<1e-24)learnedPower=0;
                const auto threshold=std::clamp(std::sqrt(learnedPower)*1.9952623149688795,.001,1.);
                if(detectedAmplitude>=threshold*saturationAmplitude)target=installed.rangeDb;
                else if(detectedAmplitude>threshold*kneeFloorAmplitude)
                    target=dynamicEqTargetGainDb(float(8.685889638065036553*std::log(detectedAmplitude/threshold)),0,installed.rangeDb);
            }else if (detectedAmplitude >= saturationAmplitude) target = installed.rangeDb;
            else if (detectedAmplitude > kneeFloorAmplitude) {
                const auto level = 8.685889638065036553*std::log(std::max(detectedAmplitude,1e-12));
                target = dynamicEqTargetGainDb(float(level), installed.thresholdDb, installed.rangeDb);
            }
            target = std::clamp(base+target,-24.,24.)-base;
        }
        auto timing = std::abs(target) > std::abs(dynamicGain) ? attack : release;
        if(installed.adaptiveTiming&&dynamicGain!=0&&std::abs(target)<=std::abs(dynamicGain))
            timing=std::exp(-1./(rate*detectorReleaseMs(installed,dynamicGain)*.001));
        dynamicGain = target + timing*(dynamicGain-target);
        dynamicGain = std::clamp(base+dynamicGain,-24.,24.)-base;
        const auto difference = endpointAmplitude-baseAmplitude;
        if (referenceMoving || dynamicGain != previousDynamicGain) {
            cachedMix = dynamicGain == 0 || std::abs(difference) < 1e-12 ? 0.
                : std::clamp(baseAmplitude*(amplitude(dynamicGain)-1.)/difference,0.,1.);
            previousDynamicGain = dynamicGain;
        }
        const auto mix = cachedMix;
        const auto filtered = filter.process(input), ranged = endpoint.process(input);
        y = filtered + StereoRegister(mix)*(ranged-filtered);
        }
        if(installed.placement!=DynamicEqPlacement::stereo){
            const auto delta=blend*(y.get(0)-input.get(0));
            if(delta==0)return original;
            if(installed.placement==DynamicEqPlacement::left||installed.placement==DynamicEqPlacement::right){
                alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
                original.copyToRawArray(lanes.data());
                lanes[installed.placement==DynamicEqPlacement::left?0:1]+=delta;
                return StereoRegister::fromRawArray(lanes.data());
            }
            return original+componentFrame(delta,installed.placement);
        }
        return input + StereoRegister(blend)*(y-input);
    }

    bool processBlock(std::span<DynamicEq::Frame> frames,double rate,std::span<const DynamicEq::Frame> external = {}) noexcept
    {
        if(extendedShape(installed)){
            auto& extended=filters.extended;
            if(pending||wet.left||wet.value<=0||extended.trim.left)return false;
            // Unity pairs have special history repair in the scalar path.
            for(int i=0;i<extended.count;++i){const auto& pair=extended.pairs[static_cast<std::size_t>(i)];if(pair.left||ExtendedFilter::unity(pair.current))return false;}
            std::array<DynamicEq::Frame,32> scratch{};const auto size=frames.size();auto filtered=std::span(scratch).first(size);
            const auto placement=installed.placement;const bool component=placement!=DynamicEqPlacement::stereo;
            alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
            for(std::size_t n=0;n<size;++n){
                if(component){lanes[0]=frames[n][0];lanes[1]=frames[n][1];const auto selected=selectedComponent(StereoRegister::fromRawArray(lanes.data()),placement);filtered[n]={selected,selected};}
                else filtered[n]=frames[n];
            }
            if(component)extended.settledBlock<true>(filtered);else extended.settledBlock<false>(filtered);
            for(std::size_t n=0;n<size;++n){
                lanes[0]=frames[n][0];lanes[1]=frames[n][1];const auto input=StereoRegister::fromRawArray(lanes.data());
                const auto load=[&]{if(component)return StereoRegister(filtered[n][0]);lanes[0]=filtered[n][0];lanes[1]=filtered[n][1];return StereoRegister::fromRawArray(lanes.data());};
                const auto y=load()*StereoRegister(extended.trim.value);
                if(component){const auto delta=wet.value*(y.get(0)-selectedComponent(input,placement));if(delta==0)continue;
                    if(placement==DynamicEqPlacement::left||placement==DynamicEqPlacement::right)frames[n][placement==DynamicEqPlacement::left?0:1]+=delta;
                    else{const auto signal=input+componentFrame(delta,placement);signal.copyToRawArray(lanes.data());frames[n]={lanes[0],lanes[1]};}
                }else{const auto signal=input+StereoRegister(wet.value)*(y-input);signal.copyToRawArray(lanes.data());frames[n]={lanes[0],lanes[1]};}
            }
            return true;
        }
        if(!dynamicShape(installed) || (installed.rangeDb==0&&!installed.autoThreshold))return false;
        auto& [filter,endpoint,detector]=filters.legacy;
        if(pending || wet.left || baseGain.left || rangeEndpoint.left || wet.value<=0
            || filter.left || endpoint.left || detector.left || filter.stages!=1 || endpoint.stages!=1)return false;
        constexpr std::size_t capacity=32;
        std::array<DynamicEq::Frame,capacity> detected{},filtered{},ranged{};
        const auto size=frames.size();
        const auto placement=installed.placement;const bool component=placement!=DynamicEqPlacement::stereo;
        const bool useExternal=externalDetector(installed.detectorSource);
        alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
        if(component){
            for(std::size_t n=0;n<size;++n){lanes[0]=frames[n][0];lanes[1]=frames[n][1];
                const auto selected=selectedComponent(StereoRegister::fromRawArray(lanes.data()),placement);detected[n]={selected,selected};}
            const auto selected=std::span(detected).first(size);
            filter.settledBlock<true>(selected,std::span(filtered).first(size));
            endpoint.settledBlock<true>(selected,std::span(ranged).first(size));
            if(useExternal)for(std::size_t n=0;n<size;++n){
                if(external.empty())detected[n]={};
                else{lanes[0]=external[n][0];lanes[1]=external[n][1];const auto trigger=selectedComponent(StereoRegister::fromRawArray(lanes.data()),placement);detected[n]={trigger,trigger};}
            }
            // Independent filter states permit scratch reuse; mono loads retain every SIMD lane.
            detector.settledBlock<true>(selected,selected);
            if(detector.stages==2)detector.settledBlock<true,1>(selected,selected);
        }else{
            const auto trigger=useExternal?(external.empty()?std::span<const DynamicEq::Frame>(detected).first(size):external):std::span<const DynamicEq::Frame>(frames);
            detector.settledBlock(trigger,std::span(detected).first(size));
            if(detector.stages==2)detector.settledBlock<false,1>(std::span(detected).first(size),std::span(detected).first(size));
            filter.settledBlock(frames,std::span(filtered).first(size));
            endpoint.settledBlock(frames,std::span(ranged).first(size));
        }
        const auto base=baseGain.value,blend=wet.value,difference=endpointAmplitude-baseAmplitude;
        std::array<double,capacity> targets{},exponents{},amplitudes{};
        std::array<int,capacity> amplitudeIndex{};std::size_t exponentCount=0;
        // These original logarithms are independent; the smoothing recurrence
        // follows in its own pass and still consumes every sample in order.
        for(std::size_t n=0;n<size;++n){
            const auto detectedAmplitude=std::max(std::abs(detected[n][0]),std::abs(detected[n][1]));double target=0;
            if(installed.autoThreshold){
                learnedPower=powerCoefficient*learnedPower+(detectedAmplitude>3.162277660168379e-5?(1-powerCoefficient)*detectedAmplitude*detectedAmplitude:0.);
                if(learnedPower<1e-24)learnedPower=0;
                const auto threshold=std::clamp(std::sqrt(learnedPower)*1.9952623149688795,.001,1.);
                if(detectedAmplitude>=threshold*saturationAmplitude)target=installed.rangeDb;
                else if(detectedAmplitude>threshold*kneeFloorAmplitude)target=dynamicEqTargetGainDb(float(8.685889638065036553*std::log(detectedAmplitude/threshold)),0,installed.rangeDb);
            }else if(detectedAmplitude>=saturationAmplitude)target=installed.rangeDb;
            else if(detectedAmplitude>kneeFloorAmplitude){const auto level=8.685889638065036553*std::log(std::max(detectedAmplitude,1e-12));target=dynamicEqTargetGainDb(float(level),installed.thresholdDb,installed.rangeDb);}
            targets[n]=std::clamp(base+target,-24.,24.)-base;
        }
        auto previous=previousDynamicGain;
        for(std::size_t n=0;n<size;++n){
            const auto target=targets[n];auto timing=std::abs(target)>std::abs(dynamicGain)?attack:release;
            if(installed.adaptiveTiming&&dynamicGain!=0&&std::abs(target)<=std::abs(dynamicGain))timing=std::exp(-1./(rate*detectorReleaseMs(installed,dynamicGain)*.001));
            dynamicGain=target+timing*(dynamicGain-target);dynamicGain=std::clamp(base+dynamicGain,-24.,24.)-base;
            amplitudeIndex[n]=-1;
            if(dynamicGain!=previous){
                amplitudeIndex[n]=-2;
                if(dynamicGain!=0 && std::abs(difference)>=1e-12){amplitudeIndex[n]=int(exponentCount);exponents[exponentCount++]=dynamicGain*.1151292546497022842;}
                previous=dynamicGain;
            }
        }
        for(std::size_t n=0;n<exponentCount;++n)amplitudes[n]=std::exp(exponents[n]);
        previousDynamicGain=previous;
        for(std::size_t n=0;n<size;++n){
            const auto index=amplitudeIndex[n];if(index==-2)cachedMix=0;else if(index>=0)cachedMix=std::clamp(baseAmplitude*(amplitudes[std::size_t(index)]-1.)/difference,0.,1.);
            lanes[0]=frames[n][0];lanes[1]=frames[n][1];const auto input=StereoRegister::fromRawArray(lanes.data());
            lanes[0]=filtered[n][0];lanes[1]=filtered[n][1];const auto staticSignal=StereoRegister::fromRawArray(lanes.data());
            lanes[0]=ranged[n][0];lanes[1]=ranged[n][1];const auto endpointSignal=StereoRegister::fromRawArray(lanes.data());
            const auto y=staticSignal+StereoRegister(cachedMix)*(endpointSignal-staticSignal);
            if(component){
                const auto delta=blend*(y.get(0)-selectedComponent(input,placement));if(delta==0)continue;
                if(placement==DynamicEqPlacement::left||placement==DynamicEqPlacement::right)frames[n][placement==DynamicEqPlacement::left?0:1]+=delta;
                else{const auto signal=input+componentFrame(delta,placement);signal.copyToRawArray(lanes.data());frames[n]={lanes[0],lanes[1]};}
            }else{const auto signal=input+StereoRegister(blend)*(y-input);signal.copyToRawArray(lanes.data());frames[n]={lanes[0],lanes[1]};}
        }
        return true;
    }
};
}
struct DynamicEq::State
{
    DynamicEqSettings settings;
    std::array<DynamicEqBand, dynamicEqBandCount> renderBands {};
    std::array<double, dynamicEqBandCount> compensationWeights {};
    std::array<BandState, dynamicEqBandCount> bands;
    double rate = 0;
    int rampSamples = 1;
    Ramp trim {1, 1};
    Ramp compensation {1, 1};
    Filter auditionFilter;
    Ramp auditionWet;
    std::optional<std::size_t> auditionRequested, auditionInstalled;
    DynamicEqPlacement auditionPlacement=DynamicEqPlacement::stereo;
    float auditionHz=0, auditionQ=0;
    float auditionLow=0,auditionHigh=0;
    DynamicEqDetectorSource auditionSource=DynamicEqDetectorSource::internalBand;
    DynamicEqShape auditionShape=DynamicEqShape::bell;
    bool detectorAuditionRequested=false,detectorAuditionInstalled=false;
    StereoRegister monitor(StereoRegister input,StereoRegister normal,StereoRegister external) noexcept
    {
        if(auditionRequested && (!settings.bands[*auditionRequested].active
            ||(detectorAuditionRequested&&!dynamicShape(settings.bands[*auditionRequested]))))auditionRequested.reset();
        const auto placement=auditionRequested?settings.bands[*auditionRequested].placement:DynamicEqPlacement::stereo;
        const bool triggerChanged=auditionRequested&&detectorAuditionRequested
            &&(settings.bands[*auditionRequested].detectorSource!=auditionSource||settings.bands[*auditionRequested].shape!=auditionShape);
        if(auditionRequested!=auditionInstalled || (auditionRequested && placement!=auditionPlacement)
            ||detectorAuditionRequested!=detectorAuditionInstalled||triggerChanged){
            auditionWet.set(0,rampSamples);
            if(auditionWet.value==0){
                auditionInstalled=auditionRequested;auditionFilter=Filter{};auditionHz=auditionQ=0;
                auditionPlacement=placement;
                detectorAuditionInstalled=detectorAuditionRequested;
                if(auditionInstalled){auditionSource=settings.bands[*auditionInstalled].detectorSource;auditionShape=settings.bands[*auditionInstalled].shape;}
                auditionLow=auditionHigh=0;
                auditionWet.set(auditionInstalled?1:0,rampSamples);
            }
        }
        if(!auditionInstalled)return normal;
        const auto& band=settings.bands[*auditionInstalled];
        if(band.frequencyHz!=auditionHz || band.q!=auditionQ
            ||(detectorAuditionInstalled&&(band.detectorLowHz!=auditionLow||band.detectorHighHz!=auditionHigh))){
            Chain c=identityChain;
            if(detectorAuditionInstalled){auto detectorBand=band;detectorBand.detectorSource=auditionSource;detectorBand.shape=auditionShape;c=detectorCoefficients(detectorBand,rate);}
            else c[0]=normalize(juce::dsp::IIR::ArrayCoefficients<double>::makeBandPass(rate,std::min(double(band.frequencyHz),.45*rate),double(band.q)));
            auditionFilter.set(c,rampSamples,auditionHz==0);auditionHz=band.frequencyHz;auditionQ=band.q;
            auditionLow=band.detectorLowHz;auditionHigh=band.detectorHighHz;
        }
        const auto trigger=detectorAuditionInstalled&&externalDetector(auditionSource)?external:input;
        const auto selected=auditionPlacement==DynamicEqPlacement::stereo?trigger:StereoRegister(selectedComponent(trigger,auditionPlacement));
        auto isolated=auditionFilter.process(selected);
        if(auditionPlacement!=DynamicEqPlacement::stereo)isolated=componentFrame(isolated.get(0),auditionPlacement);
        const auto wet=auditionWet.tick();
        return wet==0?normal:normal+StereoRegister(wet)*(isolated-normal);
    }
};
bool validateDynamicEq(const DynamicEqSettings& s) noexcept
{
    return s.processingMode<=DynamicEqProcessingMode::spectral && range(s.outputGainDb, -24, 12) && range(s.gainScale, 0, 2)
        && std::all_of(s.bands.begin(), s.bands.end(), validBand)
        && (s.processingMode==DynamicEqProcessingMode::ordinary || std::all_of(s.bands.begin(),s.bands.end(),[](const auto& b){
            return !b.spectralEnabled || (b.shape<=DynamicEqShape::highShelf && b.detectorSource==DynamicEqDetectorSource::internalBand && !b.autoThreshold && !b.adaptiveTiming);
        }));
}
float dynamicEqAutoGainDb(const DynamicEqSettings& s) noexcept
{
    if (!validateDynamicEq(s)) return std::numeric_limits<float>::quiet_NaN();
    if (!s.autoGain) return 0;
    double sum = 0;
    for (const auto& raw : s.bands) if (enabled(raw)) sum += gainWeight(raw)*scaledBand(raw, s.gainScale).gainDb;
    return float(std::clamp(-sum, -12., 12.));
}
double dynamicEqTailSeconds(const DynamicEqSettings& settings, double rate) noexcept
{
    if (!validateDynamicEq(settings) || !validRate(rate)) return std::numeric_limits<double>::quiet_NaN();
    double seconds = 0;
    for (const auto& raw : settings.bands) if (enabled(raw)) {
        const auto band = scaledBand(raw, settings.gainScale);
        if(extendedShape(band)){
            if(neutralTilt(band))continue;
            const auto chain=extendedCoefficients(band,rate);
            for(int i=0;i<chain.count;++i){
                const auto radius=poleRadius(chain.sections[static_cast<std::size_t>(i)]);
                if(!std::isfinite(radius)||radius>=1)return std::numeric_limits<double>::quiet_NaN();
                if(radius>0)seconds+=std::log(1e-12)/std::log(radius)/rate;
            }
            continue;
        }
        const auto first = coefficients(band,rate,band.gainDb), last = coefficients(band,rate,endpointGain(band));
        for (std::size_t i = 0; i < first.size(); ++i) {
            const auto radius = std::max(poleRadius(first[i]),poleRadius(last[i]));
            if (!std::isfinite(radius) || radius >= 1) return std::numeric_limits<double>::quiet_NaN();
            if (radius > 0) seconds += std::log(1e-12)/std::log(radius)/rate;
        }
    }
    return seconds == 0 ? 0 : seconds+.01;
}
float dynamicEqTargetGainDb(float level, float threshold, float rangeDb) noexcept
{
    const auto x = level-threshold;
    const auto over = x <= -3 ? 0 : x >= 3 ? x : (x+3)*(x+3)/12;
    return std::copysign(std::min(std::abs(rangeDb),.75f*over),rangeDb);
}
std::optional<std::complex<double>> dynamicEqBandResponse(const DynamicEqBand& raw, double rate, double hz, float dynamicGain, float gainScale) noexcept
{
    if (!validBand(raw) || !range(gainScale, 0, 2) || !validRate(rate) || !std::isfinite(hz) || hz < 0 || hz > rate*.5 || !std::isfinite(dynamicGain)) return {};
    const auto b = scaledBand(raw, gainScale);
    if (!enabled(b)) return std::complex<double>(1., 0.);
    if(extendedShape(b)){
        if(neutralTilt(b))return std::complex<double>(1.,0.);
        const auto chain=extendedCoefficients(b,rate);
        const auto z=std::polar(1.,-2*std::numbers::pi*hz/rate);
        std::complex<double> result=chain.trim;
        for(int i=0;i<chain.count;++i){const auto& c=chain.sections[static_cast<std::size_t>(i)];result*=(c[0]+c[1]*z+c[2]*z*z)/(1.+c[3]*z+c[4]*z*z);}
        return result;
    }
    const auto first = response(coefficients(b, rate, b.gainDb), rate, hz);
    if (!dynamicShape(b) || b.rangeDb == 0) return first;
    const auto last = response(coefficients(b, rate, endpointGain(b)), rate, hz);
    return first + interpolation(b.gainDb, endpointGain(b), dynamicGain)*(last-first);
}
std::optional<std::complex<double>> dynamicEqResponse(const DynamicEqSettings& s, double rate, double hz,
    const std::array<float, dynamicEqBandCount>& dynamics) noexcept
{
    if (!validateDynamicEq(s)) return {};
    std::complex<double> result = amplitude(s.outputGainDb);
    if (s.autoGain) result *= amplitude(dynamicEqAutoGainDb(s));
    for (std::size_t i = 0; i < dynamicEqBandCount; ++i) {
        const auto value = dynamicEqBandResponse(s.bands[i], rate, hz, dynamics[i], s.gainScale);
        if (!value) return {};
        result *= *value;
    }
    return result;
}
std::optional<std::array<std::complex<double>,4>> dynamicEqMatrixResponse(const DynamicEqSettings& settings,double rate,double hz,
    const std::array<float,dynamicEqBandCount>& dynamics) noexcept
{
    if(!validateDynamicEq(settings)||!validRate(rate)||!std::isfinite(hz)||hz<0||hz>rate*.5)return {};
    using Matrix=std::array<std::complex<double>,4>;
    Matrix result{1.,0.,0.,1.};
    for(std::size_t i=0;i<dynamicEqBandCount;++i){
        const auto& band=settings.bands[i];
        const auto h=dynamicEqBandResponse(band,rate,hz,dynamics[i],settings.gainScale);if(!h)return {};
        Matrix next{1.,0.,0.,1.};
        switch(band.placement){
            case DynamicEqPlacement::stereo:next={*h,0.,0.,*h};break;
            case DynamicEqPlacement::left:next[0]=*h;break;
            case DynamicEqPlacement::right:next[3]=*h;break;
            case DynamicEqPlacement::mid:next={(*h+1.)*.5,(*h-1.)*.5,(*h-1.)*.5,(*h+1.)*.5};break;
            case DynamicEqPlacement::side:next={(1.+*h)*.5,(1.-*h)*.5,(1.-*h)*.5,(1.+*h)*.5};break;
        }
        result={next[0]*result[0]+next[1]*result[2],next[0]*result[1]+next[1]*result[3],
                next[2]*result[0]+next[3]*result[2],next[2]*result[1]+next[3]*result[3]};
    }
    auto gain=amplitude(settings.outputGainDb);
    if(settings.autoGain)gain*=amplitude(dynamicEqAutoGainDb(settings));
    for(auto& value:result)value*=gain;
    return result;
}
std::optional<double> dynamicEqEnergyResponse(const DynamicEqSettings& settings,double rate,double hz,
    const std::array<float,dynamicEqBandCount>& dynamics) noexcept
{
    const bool stereo=std::all_of(settings.bands.begin(),settings.bands.end(),[](const auto& b){
        return !enabled(b)||b.placement==DynamicEqPlacement::stereo;
    });
    if(stereo){
        const auto response=dynamicEqResponse(settings,rate,hz,dynamics);
        if(!response)return {};
        const auto magnitude=std::abs(*response);
        if(!std::isfinite(magnitude))return {};
        return magnitude;
    }
    const auto matrix=dynamicEqMatrixResponse(settings,rate,hz,dynamics);
    if(!matrix)return {};
    double energy=0;for(const auto& entry:*matrix)energy+=std::norm(entry);
    if(!std::isfinite(energy))return {};
    return std::sqrt(energy*.5);
}
struct PreparedDynamicEqResponse::State
{
    struct Band
    {
        Chain first{},last{};
        ExtendedChain extended;
        double base=0,endpoint=0,mix=0;
        DynamicEqPlacement placement=DynamicEqPlacement::stereo;
        bool identity=true,extension=false,dynamic=false;
        std::complex<double> at(double rate,double hz) const noexcept
        {
            if(identity)return {1.,0.};
            if(extension){
                const auto z=std::polar(1.,-2*std::numbers::pi*hz/rate);
                std::complex<double> result=extended.trim;
                for(int i=0;i<extended.count;++i){const auto& c=extended.sections[static_cast<std::size_t>(i)];result*=(c[0]+c[1]*z+c[2]*z*z)/(1.+c[3]*z+c[4]*z*z);}
                return result;
            }
            const auto value=response(first,rate,hz);
            return dynamic?value+mix*(response(last,rate,hz)-value):value;
        }
    };
    std::array<Band,dynamicEqBandCount> bands;
    DynamicEqSettings settings;
    double rate=0,gain=1;
    bool haveCoefficients=false,valid=false,stereo=true;
};
PreparedDynamicEqResponse::PreparedDynamicEqResponse():state(std::make_unique<State>()){}
PreparedDynamicEqResponse::~PreparedDynamicEqResponse()=default;
bool PreparedDynamicEqResponse::prepare(const DynamicEqSettings& settings,double rate,const std::array<float,dynamicEqBandCount>& dynamics) noexcept
{
    auto& s=*state;s.valid=false;
    if(!validateDynamicEq(settings)||!validRate(rate)||!std::all_of(dynamics.begin(),dynamics.end(),[](float value){return std::isfinite(value);}))return false;
    if(!s.haveCoefficients||s.settings!=settings||s.rate!=rate){
        for(std::size_t i=0;i<s.bands.size();++i){
            const auto b=scaledBand(settings.bands[i],settings.gainScale);auto& prepared=s.bands[i];
            prepared.placement=b.placement;prepared.extension=extendedShape(b);
            prepared.identity=!enabled(b)||(prepared.extension&&neutralTilt(b));
            prepared.dynamic=!prepared.identity&&!prepared.extension&&dynamicShape(b)&&b.rangeDb!=0;
            prepared.base=b.gainDb;prepared.endpoint=endpointGain(b);
            if(!prepared.identity){
                if(prepared.extension)prepared.extended=extendedCoefficients(b,rate);
                else{prepared.first=coefficients(b,rate,b.gainDb);if(prepared.dynamic)prepared.last=coefficients(b,rate,prepared.endpoint);}
            }
        }
        s.gain=amplitude(settings.outputGainDb);if(settings.autoGain)s.gain*=amplitude(dynamicEqAutoGainDb(settings));
        s.stereo=std::all_of(settings.bands.begin(),settings.bands.end(),[](const auto& b){return !enabled(b)||b.placement==DynamicEqPlacement::stereo;});
        s.settings=settings;s.rate=rate;s.haveCoefficients=true;
    }
    for(std::size_t i=0;i<s.bands.size();++i){auto& b=s.bands[i];b.mix=b.dynamic?interpolation(b.base,b.endpoint,dynamics[i]):0.;}
    s.valid=true;return true;
}
std::optional<double> PreparedDynamicEqResponse::energy(double hz) const noexcept
{
    const auto& s=*state;if(!s.valid||!std::isfinite(hz)||hz<0||hz>s.rate*.5)return {};
    if(s.stereo){
        std::complex<double> result=s.gain;for(const auto& b:s.bands)result*=b.at(s.rate,hz);
        const auto magnitude=std::abs(result);if(!std::isfinite(magnitude))return {};return magnitude;
    }
    using Matrix=std::array<std::complex<double>,4>;
    Matrix result{1.,0.,0.,1.};
    for(const auto& b:s.bands){
        const auto h=b.at(s.rate,hz);Matrix next{1.,0.,0.,1.};
        switch(b.placement){
            case DynamicEqPlacement::stereo:next={h,0.,0.,h};break;
            case DynamicEqPlacement::left:next[0]=h;break;
            case DynamicEqPlacement::right:next[3]=h;break;
            case DynamicEqPlacement::mid:next={(h+1.)*.5,(h-1.)*.5,(h-1.)*.5,(h+1.)*.5};break;
            case DynamicEqPlacement::side:next={(1.+h)*.5,(1.-h)*.5,(1.-h)*.5,(1.+h)*.5};break;
        }
        result={next[0]*result[0]+next[1]*result[2],next[0]*result[1]+next[1]*result[3],next[2]*result[0]+next[3]*result[2],next[2]*result[1]+next[3]*result[3]};
    }
    for(auto& value:result)value*=s.gain;
    double energy=0;for(const auto& value:result)energy+=std::norm(value);
    if(!std::isfinite(energy))return {};return std::sqrt(energy*.5);
}
DynamicEq::DynamicEq() : state(std::make_unique<State>()) {}
DynamicEq::~DynamicEq() = default;
bool DynamicEq::prepare(double rate) noexcept
{
    if (!validRate(rate)) return false;
    state->rate = rate; state->rampSamples = int(std::ceil(rate * .005)); reset(); return true;
}
bool DynamicEq::setSettings(const DynamicEqSettings& s) noexcept
{
    if (!validateDynamicEq(s)) return false;
    if (s == state->settings) return true;
    double compensationDb = 0;
    for (std::size_t i = 0; i < dynamicEqBandCount; ++i) {
        const auto& band = s.bands[i];
        const auto& previous = state->settings.bands[i];
        state->renderBands[i] = scaledBand(band, s.gainScale);
        if (s.autoGain) {
            if (!state->settings.autoGain || band.shape != previous.shape || band.frequencyHz != previous.frequencyHz || band.q != previous.q)
                state->compensationWeights[i] = gainWeight(band);
            if (enabled(band)) compensationDb += state->compensationWeights[i]*state->renderBands[i].gainDb;
        }
    }
    state->settings = s;
    if (state->rate > 0) {
        for (std::size_t i = 0; i < dynamicEqBandCount; ++i) state->bands[i].publish(state->renderBands[i], state->rate, state->rampSamples);
        state->compensation.set(amplitude(float(std::clamp(-compensationDb, -12., 12.))), state->rampSamples);
        state->trim.set(amplitude(s.outputGainDb), state->rampSamples);
    }
    return true;
}
const DynamicEqSettings& DynamicEq::settings() const noexcept { return state->settings; }
std::array<float, dynamicEqBandCount> DynamicEq::dynamicGainsDb() const noexcept
{
    std::array<float, dynamicEqBandCount> result {};
    for (std::size_t i = 0; i < dynamicEqBandCount; ++i) {
        const auto& band = state->bands[i];
        if (!dynamicShape(band.installed) || band.wet.value == 0 || band.cachedMix == 0) continue;
        // Report the applied interpolation coordinate, rather than a detector
        // command that can outlive a smaller, zero or reversed range. Include
        // the structural wet fade relative to its corresponding static path.
        const auto base = 1.+band.wet.value*(band.baseAmplitude-1.);
        const auto actual = base+band.wet.value*band.cachedMix*(band.endpointAmplitude-band.baseAmplitude);
        result[i] = float(8.685889638065036553*std::log(actual/base));
    }
    return result;
}
DynamicEq::Frame DynamicEq::process(Frame input) noexcept
{ return process(input,{}); }
DynamicEq::Frame DynamicEq::process(Frame input,Frame external) noexcept
{
    if (state->rate == 0) return input;
    alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes {};
    lanes[0]=external[0];lanes[1]=external[1];const auto externalSignal=StereoRegister::fromRawArray(lanes.data());
    lanes[0] = input[0]; lanes[1] = input[1];
    const auto original = StereoRegister::fromRawArray(lanes.data());
    auto signal = original;
    for (std::size_t i = 0; i < dynamicEqBandCount; ++i)
        signal = state->bands[i].process(signal, state->renderBands[i], state->rate, state->rampSamples,externalSignal);
    if (state->compensation.left || state->compensation.value != 1) signal *= StereoRegister(state->compensation.tick());
    const auto trim = state->trim.tick();
    signal *= StereoRegister(trim);
    if(state->auditionRequested || state->auditionInstalled)signal=state->monitor(original,signal,externalSignal);
    signal.copyToRawArray(lanes.data());
    return {lanes[0],lanes[1]};
}
bool DynamicEq::processBlock(std::span<Frame> frames,std::span<const Frame> external) noexcept
{
    if(frames.size()!=external.size())return false;
    const auto audioAddress=reinterpret_cast<std::uintptr_t>(frames.data()),externalAddress=reinterpret_cast<std::uintptr_t>(external.data());
    const auto distance=audioAddress>externalAddress?audioAddress-externalAddress:externalAddress-audioAddress;
    if(distance<frames.size_bytes()){
        // Preserve the existing sample order when callers share trigger/output storage.
        for(std::size_t i=0;i<frames.size();++i)frames[i]=process(frames[i],external[i]);
    }else processBlockImpl(frames,external);
    return true;
}
std::array<DynamicEqDetectorReadback,dynamicEqBandCount> DynamicEq::detectorReadbacks() const noexcept
{
    std::array<DynamicEqDetectorReadback,dynamicEqBandCount> result{};
    for(std::size_t i=0;i<result.size();++i){const auto& rendered=state->bands[i];const auto& b=state->rate>0?rendered.installed:state->settings.bands[i];
        result[i]={b.autoThreshold?learnedThresholdDb(rendered.learnedPower):b.thresholdDb,float(detectorAttackMs(b)),float(detectorReleaseMs(b,rendered.dynamicGain))};}
    return result;
}
bool DynamicEq::setDetectorAudition(std::optional<std::size_t> band) noexcept
{
    if(band&&(*band>=dynamicEqBandCount||!state->settings.bands[*band].active||!dynamicShape(state->settings.bands[*band])))return false;
    state->detectorAuditionRequested=band.has_value();state->auditionRequested=band;return true;
}
void DynamicEq::processBlock(std::span<Frame> frames) noexcept
{processBlockImpl(frames,{});}
void DynamicEq::processBlockImpl(std::span<Frame> frames,std::span<const Frame> external) noexcept
{
    if (state->rate == 0) return;
    if (state->auditionRequested || state->auditionInstalled) {
        for(std::size_t i=0;i<frames.size();++i)frames[i]=process(frames[i],external.empty()?Frame{}:external[i]);
        return;
    }
    alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes {};
    while(!frames.empty()){
        const auto chunk=frames.first(std::min<std::size_t>(32,frames.size()));
        const auto trigger=external.empty()?external:external.first(chunk.size());
        for (std::size_t band = 0; band < dynamicEqBandCount; ++band) {
            if(state->bands[band].processBlock(chunk,state->rate,trigger))continue;
            for(std::size_t n=0;n<chunk.size();++n){
                auto& frame=chunk[n];StereoRegister detector(0);
                if(!trigger.empty()){lanes[0]=trigger[n][0];lanes[1]=trigger[n][1];detector=StereoRegister::fromRawArray(lanes.data());}
                lanes[0] = frame[0]; lanes[1] = frame[1];
                const auto signal = state->bands[band].process(StereoRegister::fromRawArray(lanes.data()),
                    state->renderBands[band], state->rate, state->rampSamples,detector);
                signal.copyToRawArray(lanes.data());frame = {lanes[0],lanes[1]};
            }
        }
        if (!state->compensation.left && state->compensation.value == 1) {
            // Keep the original neutral trim loop free of per-sample
            // compensation checks. No gain arithmetic is added in this path.
            for(auto& frame:chunk){const auto trim=state->trim.tick();frame[0]*=trim;frame[1]*=trim;}
        } else {
            for(auto& frame:chunk){
                if (state->compensation.left || state->compensation.value != 1) {
                    const auto compensation = state->compensation.tick(); frame[0]*=compensation; frame[1]*=compensation;
                }
                const auto trim=state->trim.tick();frame[0]*=trim;frame[1]*=trim;
            }
        }
        frames=frames.subspan(chunk.size());
        if(!external.empty())external=external.subspan(chunk.size());
    }
}
bool DynamicEq::setAudition(std::optional<std::size_t> band) noexcept
{
    if(band && (*band>=dynamicEqBandCount || !state->settings.bands[*band].active))return false;
    state->detectorAuditionRequested=false;
    state->auditionRequested=band;return true;
}
bool DynamicEq::auditioning() const noexcept
{return state->auditionRequested.has_value() || state->auditionWet.value>0;}
void DynamicEq::cancelAudition() noexcept
{state->auditionRequested.reset();state->auditionInstalled.reset();state->auditionWet={};state->auditionFilter=Filter{};state->auditionHz=state->auditionQ=0;state->auditionLow=state->auditionHigh=0;state->detectorAuditionRequested=state->detectorAuditionInstalled=false;}
void DynamicEq::reset() noexcept
{
    cancelAudition();
    if (state->rate == 0) return;
    for (std::size_t i = 0; i < dynamicEqBandCount; ++i) state->bands[i].install(state->renderBands[i], state->rate, state->rampSamples, true);
    state->compensation.set(amplitude(dynamicEqAutoGainDb(state->settings)), state->rampSamples, true);
    state->trim.set(amplitude(state->settings.outputGainDb), state->rampSamples, true);
}
double DynamicEq::tailSeconds() const noexcept
{
    if (state->rate == 0) return 0;
    double seconds = 0;
    bool transitioning = state->trim.left > 0 || state->compensation.left > 0;
    for (std::size_t band = 0; band < dynamicEqBandCount; ++band) {
        const auto& b = state->bands[band];
        if (!enabled(state->settings.bands[band]) && b.wet.value == 0 && !b.pending) continue;
        transitioning = transitioning || b.pending || b.wet.left > 0;
        if(extendedShape(b.installed)){
            const auto& extra=b.filters.extended;
            if(extra.transparent()&&extra.targetTransparent())continue;
            transitioning=transitioning||extra.trim.left>0;
            for(const auto& pair:extra.pairs){
                transitioning=transitioning||pair.left>0;
                for(std::size_t i=0;i<2;++i){
                    const auto radius=std::max(poleRadius(pair.current[i]),poleRadius(pair.target[i]));
                    if(radius>0&&radius<1)seconds+=std::log(1e-12)/std::log(radius)/state->rate;
                }
            }
            continue;
        }
        const auto& [filter,endpoint,detector]=b.filters.legacy;
        transitioning = transitioning || filter.left > 0 || endpoint.left > 0;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto radius = std::max({poleRadius(filter.current[i]),poleRadius(filter.target[i]),
                                          poleRadius(endpoint.current[i]),poleRadius(endpoint.target[i])});
            if (radius > 0 && radius < 1) seconds += std::log(1e-12) / std::log(radius) / state->rate;
        }
    }
    // Summed section decay allows twelve orders of pole attenuation before
    // the engineering -120 dB impulse test, including cascades and ramps.
    const auto normal=seconds == 0 && !transitioning ? 0 : seconds + .01;
    return auditioning()?normal+8:normal;
}
}
