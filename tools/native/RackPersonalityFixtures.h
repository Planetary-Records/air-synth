#include "../../tests/native/FrozenRackExamples.h"
#pragma once
// Development-only producer shared by native checks/captures and hosted rendering.
// Golden endpoint values and retained project identities deliberately stay in callers.
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include "ParameterRegistry.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace airack::rackPersonalityFixtures {
template<class Check>
inline SoundDocument fixture(int example, int setting, bool master,Check check)
{
    const auto designs=airack::test::frozenExampleRackDesigns(); SoundDocument d; RackDifference delta; SoundDocument next;
    RackOperation op; op.design=designs[example==2?0:example]; op.newInstance=juce::Uuid("d83dab6a1c4e48c183c480d9212ca601"); op.destination.position=1;
    check(makeRackCandidate(d,op,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready); d=next;
    d.values[12]=0; // Generic bank is retained, silent and unowned.
    if(example==2){op={};op.design=designs[2];op.newInstance=juce::Uuid("d83dab6a1c4e48c183c480d9212ca603");op.destination={RackRole::fx,std::uint8_t(master?masterChain:1),1};
        if(master)op.destination.position=0;check(makeRackCandidate(d,op,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);d=next;}
    if(example==2){
        // The calibrated +30dB output lies AFTER FX. Use explicit fixed upstream
        // Ember Drive excitation for Thread so its useful -36dB threshold is reached.
        d.values[*rackParameterIndex(NativeModule::drive,0,"drive.gain_db")]=1;
        d.values[*rackParameterIndex(NativeModule::drive,0,"drive.mix")]=1;
        const auto& output=parameterRegistry()[756];d.values[756]=juce::NormalisableRange<float>(output.minimum,output.maximum,0,output.skew).convertTo0to1(0);
    }
    const auto& instance=d.personalities->instances.back(); const auto& focus=instance.design.panel.controls[example==1?3:0];
    const auto binding=std::find_if(instance.bindings.begin(),instance.bindings.end(),[&](auto b){return b.unit==focus.unit;});
    const auto unit=std::find_if(instance.design.units.begin(),instance.design.units.end(),[&](const auto& u){return u.id==focus.unit;});
    const auto i=*rackParameterIndex(unit->module,binding->slot,focus.key);const auto& p=parameterRegistry()[i];
    if(setting!=0)d.values[i]=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(setting<0?focus.minimum:focus.maximum);
    check(validateSoundDocument(d));return d;
}
inline double energy(const juce::AudioBuffer<float>& a,int begin,int end)
{double sum=0;for(int c=0;c<2;++c)for(int s=begin;s<end;++s)sum+=double(a.getSample(c,s))*a.getSample(c,s);return sum/(2*(end-begin));}
template<class Check>
inline double highBand(const juce::AudioBuffer<float>& a,Check check)
{
    constexpr int n=8192;std::array<float,2*n> bins{};
    for(int sample=0;sample<n;++sample)bins[std::size_t(sample)]=float(.5*(1-std::cos(2*juce::MathConstants<double>::pi*sample/(n-1))))*a.getSample(0,12000+sample);
    juce::dsp::FFT fft(13);fft.performFrequencyOnlyForwardTransform(bins.data());double total=0,high=0;
    for(int k=1;k<n/2;++k){const double power=double(bins[std::size_t(k)])*bins[std::size_t(k)];total+=power;if(double(k)*48000/n>=4000)high+=power;}
    check(total>0);return high/total;
}

template<class Check>
inline double responseMetric(int example,const juce::AudioBuffer<float>& audio,Check check)
{
    return example==0?highBand(audio,check):example==1?energy(audio,96000,192000):energy(audio,12000,36000);
}
}
