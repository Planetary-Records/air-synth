#include "SoundDocument.h"
#include "RackReconciliation.h"
#include <algorithm>
#include <cmath>

namespace airack
{
namespace detail {bool validateRoutedPatchForStorage(const RoutedPatch&) noexcept;}
namespace
{
// Canonical numeric catalog ranges; identities on disk and in bindings are keys.
int synthOwner(std::size_t i) noexcept
{
    if(i>=12 && i<28)return int((i-12)/4);
    if(i>=28 && i<120)return int((i-28)/23);
    if(i>=120 && i<212)return int((i-120)/23);
    if(i>=212 && i<220)return int((i-212)/2);
    if(i>=220 && i<252)return int((i-220)/8);
    if(i>=252 && i<572)return int((i-252)/80);
    if(i>=drumModeParameterStart && i<saturationParameterStart)return int(i-drumModeParameterStart);
    if(i>=drumParameterStart && i<drumModeParameterStart)return int((i-drumParameterStart)/drumParametersPerSlot);
    if(i>=wavetableWarmthParameterStart && i<eraReverbParameterStart)return int((i-wavetableWarmthParameterStart)/wavetableWarmthParametersPerSlot);
    return -1;
}
int effectOwner(std::size_t i) noexcept
{ return i>=saturationParameterStart && i<parameterCount?int((i-saturationParameterStart)/saturationParametersPerSlot)
    :i>=eraReverbParameterStart && i<drumParameterStart?int((i-eraReverbParameterStart)/eraReverbParametersPerSlot)
    :i>=spaceReverbParameterStart && i<wavetableWarmthParameterStart?int((i-spaceReverbParameterStart)/spaceReverbParametersPerSlot)
    :i>=dynamicEqSpectralParameterStart && i<spaceReverbParameterStart?int((i-dynamicEqSpectralParameterStart)/dynamicEqSpectralParametersPerSlot)
    :i>=dynamicEqDetectorParameterStart && i<dynamicEqSpectralParameterStart?int((i-dynamicEqDetectorParameterStart)/dynamicEqDetectorParametersPerSlot)
    :i>=delayStereoParameterStart && i<dynamicEqDetectorParameterStart?int((i-delayStereoParameterStart)/delayStereoParametersPerSlot)
    :i>=dynamicEqExtensionParameterStart && i<delayStereoParameterStart?int((i-dynamicEqExtensionParameterStart)/dynamicEqExtensionParametersPerSlot)
    :i>=delayTapeParameterStart && i<dynamicEqExtensionParameterStart?int((i-delayTapeParameterStart)/delayTapeParametersPerSlot)
    :i>=dynamicEqPlacementParameterStart && i<delayTapeParameterStart?int((i-dynamicEqPlacementParameterStart)/dynamicEqPlacementParametersPerSlot)
    :i>=dynamicEqGainParameterStart && i<dynamicEqPlacementParameterStart?int((i-dynamicEqGainParameterStart)/dynamicEqGainParametersPerSlot)
    :i>=dynamicEqParameterStart && i<dynamicEqGainParameterStart?int((i-dynamicEqParameterStart)/dynamicEqParametersPerSlot)
    :i>=midSideSplitParameterStart && i<dynamicEqParameterStart?int((i-midSideSplitParameterStart)/midSideSplitParametersPerSlot)
    :i>=frequencySplitParameterStart && i<midSideSplitParameterStart?int((i-frequencySplitParameterStart)/frequencySplitParametersPerSlot)
    :i>=convolutionParameterStart && i<frequencySplitParameterStart?int((i-convolutionParameterStart)/convolutionParametersPerSlot)
    :i>=frequencyShiftParameterStart && i<convolutionParameterStart?int((i-frequencyShiftParameterStart)/frequencyShiftParametersPerSlot)
    :i>=dimensionParameterStart && i<frequencyShiftParameterStart?int((i-dimensionParameterStart)/dimensionParametersPerSlot)
    :i>=stereoParameterStart && i<dimensionParameterStart?int((i-stereoParameterStart)/stereoParametersPerSlot)
    :i>=flangerParameterStart && i<stereoParameterStart?int((i-flangerParameterStart)/flangerParametersPerSlot)
    :i>=phaserParameterStart && i<flangerParameterStart?int((i-phaserParameterStart)/phaserParametersPerSlot)
    :i>=chorusParameterStart && i<phaserParameterStart?int((i-chorusParameterStart)/chorusParametersPerSlot)
    :i>=filterParameterStart && i<chorusParameterStart?int((i-filterParameterStart)/filterParametersPerSlot)
    :i>=eqParameterStart && i<filterParameterStart?int((i-eqParameterStart)/eqParametersPerSlot):i>=572 && i<756?int((i-572)/23):-1; }
bool validateMapped(const RoutedPatch& p,bool playable) noexcept
{
    if(playable)return validateRoutedPatch(p);
    return detail::validateRoutedPatchForStorage(p);
}
bool mapValues(const SoundAudioState& document,const ParameterValues& values, const RackLayout& layout, RoutedPatch& p,bool playable) noexcept
{
    if(!makeSynthPatch(values,p.synth))return false;
    p.layout=layout;
    p.outputGainDb=parameterPhysicalValue(values,756);
    p.outputProtection=parameterPhysicalValue(values,757)==1;
    for(std::size_t s=0;s<4;++s) {
        p.mute[s]=parameterPhysicalValue(values,212+s*2)==1;
        p.solo[s]=parameterPhysicalValue(values,213+s*2)==1;
        p.drumSource[s]=sourceModule(document,static_cast<std::uint8_t>(s))==NativeModule::drum;
        for(std::size_t pad=0;pad<16;++pad){
            const auto base=drumParameterStart+s*drumParametersPerSlot+pad*9;
            auto v=[&](std::size_t field){return parameterPhysicalValue(values,base+field);};
            p.drumPads[s][pad]={v(0),v(1),v(2),v(3),v(4),v(5),v(6),int(v(7)),int(v(8))};
        }
    }
    for(std::size_t s=0;s<8;++s) {
        const auto base=572+s*23;
        auto v=[&](std::size_t f){return parameterPhysicalValue(values,base+f);};
        auto& e=p.effects[s];
        e.bypassed=v(0)==1;
        e.drive={v(1),static_cast<DriveShape>(int(v(2))),v(3),v(4)};
        e.delay={v(5)==1,v(6),v(7),v(8),v(9),v(10),v(11)};
        e.reverb={v(12),v(13),v(14),v(15),v(16)};
        e.compressor={v(17),v(18),v(19),v(20),v(21),v(22)};
        const auto eqBase=eqParameterStart+s*eqParametersPerSlot;
        auto q=[&](std::size_t field){return parameterPhysicalValue(values,eqBase+field);};
        e.eq={{q(0),q(1),q(2)},{q(3),q(4),q(5)},{q(6),q(7),q(8)}};
        const auto filterBase=filterParameterStart+s*filterParametersPerSlot;
        auto f=[&](std::size_t field){return parameterPhysicalValue(values,filterBase+field);};
        e.filter={static_cast<FilterFxMode>(int(f(0))),f(1),f(2),f(3),f(4)};
        const auto chorusBase=chorusParameterStart+s*chorusParametersPerSlot;
        auto c=[&](std::size_t field){return parameterPhysicalValue(values,chorusBase+field);};
        e.chorus={c(0),c(1),c(2),c(3),c(4)};
        const auto phaserBase=phaserParameterStart+s*phaserParametersPerSlot;
        auto ph=[&](std::size_t field){return parameterPhysicalValue(values,phaserBase+field);};
        e.phaser={ph(0),ph(1),ph(2),ph(3),ph(4)};
        const auto flangerBase=flangerParameterStart+s*flangerParametersPerSlot;
        auto fl=[&](std::size_t field){return parameterPhysicalValue(values,flangerBase+field);};
        e.flanger={fl(0),fl(1),fl(2),fl(3),fl(4)};
        const auto stereoBase=stereoParameterStart+s*stereoParametersPerSlot;
        auto st=[&](std::size_t field){return parameterPhysicalValue(values,stereoBase+field);};
        e.stereo={st(0),st(1),st(2),static_cast<StereoChannelMode>(int(st(3)))};
        const auto dimensionBase=dimensionParameterStart+s*dimensionParametersPerSlot;
        auto di=[&](std::size_t field){return parameterPhysicalValue(values,dimensionBase+field);};
        e.dimension={di(0),di(1),di(2),di(3)};
        const auto shiftBase=frequencyShiftParameterStart+s*frequencyShiftParametersPerSlot;
        auto fs=[&](std::size_t field){return parameterPhysicalValue(values,shiftBase+field);};
        e.frequencyShift={fs(0),fs(1),fs(2)};
        const auto convolutionBase=convolutionParameterStart+s*convolutionParametersPerSlot;
        auto cv=[&](std::size_t field){return parameterPhysicalValue(values,convolutionBase+field);};
        e.convolution={static_cast<ConvolutionIr>(int(cv(0))),cv(1),cv(2),cv(3)};
        const auto splitBase=frequencySplitParameterStart+s*frequencySplitParametersPerSlot;
        auto sp=[&](std::size_t field){return parameterPhysicalValue(values,splitBase+field);};
        e.frequencySplit={sp(0),sp(1),sp(2)==1,sp(3),sp(4),sp(5)==1,sp(6),sp(7)};
        const auto msBase=midSideSplitParameterStart+s*midSideSplitParametersPerSlot;
        auto ms=[&](std::size_t field){return parameterPhysicalValue(values,msBase+field);};
        e.midSideSplit={ms(0),ms(1)==1,ms(2),ms(3),ms(4)==1,ms(5),ms(6)};
        const auto dynamicBase=dynamicEqParameterStart+s*dynamicEqParametersPerSlot;
        for(std::size_t band=0;band<dynamicEqBandCount;++band){
            auto v=[&](std::size_t field){return parameterPhysicalValue(values,dynamicBase+band*dynamicEqParametersPerBand+field);};
            auto& b=e.dynamicEq.bands[band];b.active=v(0)==1;b.bypassed=v(1)==1;b.shape=DynamicEqShape(int(v(2)));
            b.frequencyHz=v(3);b.gainDb=v(4);b.q=v(5);b.slope=DynamicEqSlope(int(v(6)));
            b.rangeDb=v(7);b.thresholdDb=v(8);b.attackMs=v(9);b.releaseMs=v(10);
            b.placement=DynamicEqPlacement(int(parameterPhysicalValue(values,dynamicEqPlacementParameterStart+s*dynamicEqPlacementParametersPerSlot+band)));
            const auto extensionBase=dynamicEqExtensionParameterStart+s*dynamicEqExtensionParametersPerSlot+band*dynamicEqExtensionParametersPerBand;
            const auto shapeExtension=int(parameterPhysicalValue(values,extensionBase));
            const auto slopeExtension=int(parameterPhysicalValue(values,extensionBase+1));
            constexpr std::array shapes{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt};
            constexpr std::array slopes{DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96};
            if(shapeExtension)b.shape=shapes[std::size_t(shapeExtension-1)];
            if(slopeExtension)b.slope=slopes[std::size_t(slopeExtension-1)];
            const auto detectorBase=dynamicEqDetectorParameterStart+s*dynamicEqDetectorParametersPerSlot+band*dynamicEqDetectorParametersPerBand;
            b.autoThreshold=parameterPhysicalValue(values,detectorBase)==1;
            b.adaptiveTiming=parameterPhysicalValue(values,detectorBase+1)==1;
            b.detectorSource=DynamicEqDetectorSource(int(parameterPhysicalValue(values,detectorBase+2)));
            b.detectorLowHz=parameterPhysicalValue(values,detectorBase+3);b.detectorHighHz=parameterPhysicalValue(values,detectorBase+4);
            const auto spectralBase=dynamicEqSpectralParameterStart+s*dynamicEqSpectralParametersPerSlot+1+band*dynamicEqSpectralParametersPerBand;
            b.spectralEnabled=parameterPhysicalValue(values,spectralBase)==1;
            b.spectralDensity=parameterPhysicalValue(values,spectralBase+1);b.spectralTiltDbOct=parameterPhysicalValue(values,spectralBase+2);
        }
        e.dynamicEq.outputGainDb=parameterPhysicalValue(values,dynamicBase+88);
        const auto saturationBase=saturationParameterStart+s*saturationParametersPerSlot;
        const auto sat=[&](std::size_t n){return parameterPhysicalValue(values,saturationBase+n);};
        auto& a=e.saturation;
        a.mode=static_cast<SaturationMode>(int(sat(0)));
        a.inputTrimDb=sat(1);a.outputTrimDb=sat(2);a.mix=sat(3);a.driveCompensation=sat(4)==1;
        a.lowCrossoverHz=sat(5);a.highCrossoverHz=sat(6);
        a.preLowCutEnabled=sat(7)==1;a.preLowCutHz=sat(8);a.postHighCutEnabled=sat(9)==1;a.postHighCutHz=sat(10);
        for(std::size_t lane=0;lane<4;++lane){const auto first=11+6*lane;auto& l=a.lanes[lane];
            l.bypassed=sat(first)==1;l.character=static_cast<SaturationCharacter>(int(sat(first+1)));
            l.driveDb=sat(first+2);l.preTiltDb=sat(first+3);l.mix=sat(first+4);l.trimDb=sat(first+5);}
        const auto gainBase=dynamicEqGainParameterStart+s*dynamicEqGainParametersPerSlot;
        e.dynamicEq.autoGain=parameterPhysicalValue(values,gainBase)==1;
        e.dynamicEq.gainScale=parameterPhysicalValue(values,gainBase+1);
        const auto spaceBase=spaceReverbParameterStart+s*spaceReverbParametersPerSlot;
        auto sr=[&](std::size_t field){return parameterPhysicalValue(values,spaceBase+field);};
        auto& reverb=e.spaceReverb;
        reverb.size=sr(0);reverb.decay.decaySeconds=sr(1);reverb.predelayMs=sr(2);reverb.decay.brightness=sr(3);
        reverb.character=sr(4);reverb.distance=sr(5);reverb.width=sr(6);reverb.mix=sr(7);
        reverb.duckRangeDb=sr(8);reverb.duckThresholdDb=sr(9);reverb.duckAttackMs=sr(10);reverb.duckReleaseMs=sr(11);
        for(std::size_t band=0;band<6;++band){
            const auto decay=12+6*band,post=48+8*band;
            auto& d=reverb.decay.bands[band];d.active=sr(decay)==1;d.bypassed=sr(decay+1)==1;
            d.shape=space::DecayShape(int(sr(decay+2)));d.frequencyHz=sr(decay+3);d.multiplier=sr(decay+4);d.q=sr(decay+5);
            auto& p=reverb.post[band];p.active=sr(post)==1;p.bypassed=sr(post+1)==1;
            p.shape=space::PostShape(int(sr(post+2)));p.frequencyHz=sr(post+3);p.gainDb=sr(post+4);
            p.bellQ=sr(post+5);p.shelfQ=sr(post+6);p.slope=space::CutSlope(int(sr(post+7)));
        }
        const auto eraBase=eraReverbParameterStart+s*eraReverbParametersPerSlot;
        auto er=[&](std::size_t field){return parameterPhysicalValue(values,eraBase+field);};
        e.eraReverb={EraVoicing(int(er(0))),EraAge(int(er(1))),er(2),er(3),er(4),er(5),er(6),er(7),er(8),er(9),er(10),er(11),er(12),er(13),er(14)};
        e.dynamicEq.processingMode=DynamicEqProcessingMode(int(parameterPhysicalValue(values,dynamicEqSpectralParameterStart+s*dynamicEqSpectralParametersPerSlot)));
        const auto tapeBase=delayTapeParameterStart+s*delayTapeParametersPerSlot;
        auto t=[&](std::size_t field){return parameterPhysicalValue(values,tapeBase+field);};
        e.delay.saturation=t(0);e.delay.lowCutHz=t(1);e.delay.wobble=t(2);e.delay.wobbleRateHz=t(3);e.delay.glide=t(4)==1;
        const auto modesBase=delayStereoParameterStart+s*delayStereoParametersPerSlot;
        auto m=[&](std::size_t field){return parameterPhysicalValue(values,modesBase+field);};
        e.delay.rightRatio=m(0);e.delay.pingPong=m(1)==1;e.delay.note=int(m(2));
    }
    return validateMapped(p,playable);
}
bool macroTargets(const SoundAudioState& d,std::array<std::optional<std::size_t>,8>& targets) noexcept
{
    for(std::size_t i=0;i<8;++i) {
        const auto& b=d.macros[i];
        if(!std::isfinite(b.amount)||b.amount < -1||b.amount > 1)return false;
        if(b.targetKey==0) {if(b.amount!=0)return false;continue;}
        const auto target=parameterIndexForKey(b.targetKey);
        if(!target || (*target>=1&&*target<=8) || !isContinuousParameter(*target))return false;
        targets[i]=target;
    }
    return true;
}
template<std::size_t N>
std::size_t position(const std::array<std::uint8_t,N>& order,std::size_t slot) noexcept
{ return static_cast<std::size_t>(std::find(order.begin(),order.end(),slot)-order.begin()); }
}

namespace {
bool mapDocument(const SoundAudioState& document,RoutedPatch& output,bool playable,ParameterValues* effectiveOutput=nullptr) noexcept
{
    RoutedPatch patch;
    // Validate stored base values as well as the effective values; a macro must
    // never hide an invalid inactive bank or a malformed route.
    if(!validateKernelCompatibility(document.compatibility)||!mapValues(document,document.values,document.layout,patch,playable))return false;
    std::array<std::optional<std::size_t>,8> targets {};
    if(!macroTargets(document,targets))return false;
    auto effective=document.values;
    std::array<float,parameterCount> offsets {};
    bool bound=false;
    for(std::size_t i=0;i<8;++i)if(targets[i]) {
        offsets[*targets[i]]+=document.values[i+1]*document.macros[i].amount;
        bound=true;
    }
    if(bound) {
        for(std::size_t i=0;i<parameterCount;++i)effective[i]=std::clamp(effective[i]+offsets[i],0.0f,1.0f);
        if(!mapValues(document,effective,document.layout,patch,playable))return false;
    }
    patch.compatibility=document.compatibility;
    for(std::size_t s=0;s<patch.synth.layers.size();++s)
        patch.synth.layers[s].algorithmRevision=document.compatibility.synth[s][static_cast<std::size_t>(patch.synth.layers[s].engine)];
    for(std::size_t s=0;s<patch.effects.size();++s)
        patch.effects[s].algorithmRevision=document.compatibility.effects[s][static_cast<std::size_t>(patch.layout.effects[s].kind)];
    if(!validateMapped(patch,playable))return false;
    if(effectiveOutput)*effectiveOutput=effective;
    output=patch;
    return true;
}
}
bool makeEffectiveParameterValues(const SoundAudioState& document,ParameterValues& output) noexcept
{
    RoutedPatch patch;
    ParameterValues effective;
    if(!mapDocument(document,patch,true,&effective))return false;
    output=effective;
    return true;
}
bool makeRoutedPatch(const SoundAudioState& document,RoutedPatch& output) noexcept
{return mapDocument(document,output,true);}
NativeModule sourceModule(const SoundAudioState& d,std::uint8_t bank) noexcept
{
    if(bank>=4)return static_cast<NativeModule>(255);
    if(parameterPhysicalValue(d.values,drumModeParameterStart+bank)==1)return NativeModule::drum;
    return parameterPhysicalValue(d.values,13+4*bank)==0?NativeModule::analog:NativeModule::wavetable;
}
bool validateSoundDocument(const SoundDocument& document) noexcept
{ try {RoutedPatch patch;return document.name.getNumBytesAsUTF8()<=maximumSoundNameBytes&&mapDocument(static_cast<const SoundAudioState&>(document),patch,false)&&validateRackInstances(document);}catch(...){return false;} }
bool makeRoutedPatch(const SoundDocument& d,RoutedPatch& output) noexcept
{return makeRoutedPatch(static_cast<const SoundAudioState&>(d),output);}
bool SoundDocument::operator==(const SoundDocument& other) const noexcept
{
    if(static_cast<const SoundAudioState&>(*this)!=static_cast<const SoundAudioState&>(other)||name!=other.name)return false;
    const auto a=personalities?personalities->instances.size():0,b=other.personalities?other.personalities->instances.size():0;
    return a==b&&(a==0||*personalities==*other.personalities);
}

bool validateAiSoundEdit(const SoundDocument& current,const SoundDocument& candidate) noexcept
{
    if(!validateSoundDocument(current)||!validateSoundDocument(candidate)
        ||current.controlLocks!=candidate.controlLocks||current.synthLocks!=candidate.synthLocks
        ||current.effectLocks!=candidate.effectLocks)return false;
    auto protectedSynth=current.synthLocks;
    auto protectedEffect=current.effectLocks;
    for(std::size_t i=0;i<parameterCount;++i) {
        const auto synth=synthOwner(i),effect=effectOwner(i);
        if(current.controlLocks[i]) {
            if(current.values[i]!=candidate.values[i])return false;
            if(synth>=0)protectedSynth[std::size_t(synth)]=true;
            if(effect>=0)protectedEffect[std::size_t(effect)]=true;
        }
        if((synth>=0&&current.synthLocks[std::size_t(synth)])||(effect>=0&&current.effectLocks[std::size_t(effect)]))
            if(current.values[i]!=candidate.values[i])return false;
    }
    for(std::size_t s=0;s<4;++s) {
        if(protectedSynth[s]&&current.compatibility.synth[s]!=candidate.compatibility.synth[s])return false;
        if(protectedSynth[s] && (current.layout.synthUse[s]!=candidate.layout.synthUse[s]
            ||parameterPhysicalValue(current.values,13+s*4)!=parameterPhysicalValue(candidate.values,13+s*4)
            ||current.values[drumModeParameterStart+s]!=candidate.values[drumModeParameterStart+s]))return false;
        if(current.synthLocks[s] && position(current.layout.synthOrder,s)!=position(candidate.layout.synthOrder,s))return false;
    }
    for(std::size_t s=0;s<8;++s) {
        if(protectedEffect[s]&&current.compatibility.effects[s]!=candidate.compatibility.effects[s])return false;
        const auto& a=current.layout.effects[s];const auto& b=candidate.layout.effects[s];
        if(protectedEffect[s]&&(a.use!=b.use||a.kind!=b.kind))return false;
        if(current.effectLocks[s] && (a.chain!=b.chain
            ||position(current.layout.effectOrder[a.chain],s)!=position(candidate.layout.effectOrder[b.chain],s)))return false;
    }
    auto targetsLockedRack=[&](std::uint32_t key){
        const auto i=parameterIndexForKey(key);if(!i)return false;
        const auto synth=synthOwner(*i),effect=effectOwner(*i);
        return (*i>=756&&current.controlLocks[*i]) || (synth>=0&&current.synthLocks[std::size_t(synth)])||(effect>=0&&current.effectLocks[std::size_t(effect)]);
    };
    for(std::size_t i=0;i<8;++i)
        if(current.macros[i]!=candidate.macros[i] && (current.controlLocks[i+1]
            ||targetsLockedRack(current.macros[i].targetKey)||targetsLockedRack(candidate.macros[i].targetKey)))return false;
    if(current.personalities)for(const auto& instance:current.personalities->instances){
        const RackInstance* after=nullptr;if(candidate.personalities)for(const auto& i:candidate.personalities->instances)if(i.id==instance.id){after=&i;break;}
        if(after&&*after==instance)continue;
        for(const auto& unit:instance.design.units)for(const auto& b:instance.bindings)if(b.unit==unit.id){
            if(isSourceModule(unit.module)?protectedSynth[b.slot]:protectedEffect[b.slot])return false;
        }
    }
    return true;
}
}
