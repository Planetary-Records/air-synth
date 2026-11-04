#include "SaturationAnalysis.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace
{
using namespace airack::probe::analysis;
int checks=0,failures=0;
void check(bool good,const char* expression,int line){++checks;if(!good){++failures;if(failures<40)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)

std::vector<double> tone(std::size_t frames,std::size_t bin,double amplitude)
{
    std::vector<double> samples(frames);
    for(std::size_t i=0;i<frames;++i)
        samples[i]=amplitude*std::sin(2*std::numbers::pi*static_cast<double>(bin)*static_cast<double>(i)/static_cast<double>(frames));
    return samples;
}

void addTone(std::vector<double>& samples,std::size_t bin,double amplitude)
{
    const auto component=tone(samples.size(),bin,amplitude);
    for(std::size_t i=0;i<samples.size();++i) samples[i]+=component[i];
}

// An analytic bandlimited 64x fixture calibrates the mask independently of a
// candidate or resampler; the execution probe separately qualifies its resampler.
std::vector<double> analytic64xReference(std::size_t frames,std::size_t fundamental,double h1,double h2=0)
{
    auto high=tone(frames*64,fundamental,h1);
    addTone(high,fundamental*2,h2);
    std::vector<double> native(frames);
    for(std::size_t i=0;i<frames;++i) native[i]=high[i*64];
    return native;
}

bool hasBin(const AliasMeasurement& result,std::size_t bin)
{
    return std::find(result.harmonicBins.begin(),result.harmonicBins.end(),bin)!=result.harmonicBins.end();
}

void signalAndPowerCalibration()
{
    std::cout<<"SECTION rectangular_one_sided_power_and_signal_calibration\n";
    constexpr std::size_t frames=1024,bin=37;
    const auto sine=tone(frames,bin,.8);
    const auto summary=summarizeSignal(sine);
    CHECK(summary.valid);
    CHECK(std::abs(summary.mean)<1e-12);
    CHECK(std::abs(summary.rms-.8/std::sqrt(2.))<1e-12);
    const auto spectrum=rectangularPowerSpectrum(sine);
    CHECK(spectrum.valid);
    CHECK(spectrum.frames==frames);
    CHECK(spectrum.oneSidedPower.size()==frames/2+1);
    if(spectrum.oneSidedPower.size()!=frames/2+1) return;
    CHECK(std::abs(spectrum.oneSidedPower[bin]-.32)<1e-6);
    double total=0;
    for(const auto power:spectrum.oneSidedPower) total+=power;
    CHECK(std::abs(total-summary.rms*summary.rms)<1e-6);

    auto mixed=sine;
    for(std::size_t i=0;i<frames;++i) mixed[i]+=.1+(i%2==0?.2:-.2);
    const auto mixedSummary=summarizeSignal(mixed);
    const auto mixedSpectrum=rectangularPowerSpectrum(mixed);
    CHECK(mixedSummary.valid&&mixedSpectrum.valid);
    CHECK(std::abs(mixedSummary.mean-.1)<1e-12);
    CHECK(std::abs(mixedSummary.rms-std::sqrt(.37))<1e-12);
    if(mixedSpectrum.oneSidedPower.size()!=frames/2+1){CHECK(false);return;}
    CHECK(std::abs(mixedSpectrum.oneSidedPower[0]-.01)<1e-6);
    CHECK(std::abs(mixedSpectrum.oneSidedPower[frames/2]-.04)<1e-6);
    CHECK(std::abs(mixedSpectrum.oneSidedPower[bin]-.32)<1e-6);

    CHECK(!summarizeSignal(std::span<const double>{}).valid);
    CHECK(!rectangularPowerSpectrum(std::span<const double>{}).valid);
    auto nonfinite=sine;
    nonfinite[10]=std::numeric_limits<double>::quiet_NaN();
    CHECK(!summarizeSignal(nonfinite).valid);
    CHECK(!rectangularPowerSpectrum(nonfinite).valid);
}

void coherentBins()
{
    std::cout<<"SECTION nearest_odd_coherent_bin_ties_lower\n";
    constexpr double rate=48000;
    constexpr std::size_t frames=1024;
    const auto exact=nearestOddCoherentBin(5*rate/frames,rate,frames);
    const auto tie=nearestOddCoherentBin(6*rate/frames,rate,frames);
    const auto aboveTie=nearestOddCoherentBin(6.01*rate/frames,rate,frames);
    CHECK(exact&&*exact==5);
    CHECK(tie&&*tie==5);
    CHECK(aboveTie&&*aboveTie==7);
    CHECK(!nearestOddCoherentBin(0,rate,frames));
    CHECK(!nearestOddCoherentBin(rate/2,rate,frames));
    CHECK(!nearestOddCoherentBin(1000,0,frames));
    CHECK(!nearestOddCoherentBin(std::numeric_limits<double>::infinity(),rate,frames));
    for(double sampleRate:{44100.,48000.,96000.}) for(double requested:{1760.,3520.,7040.})
    {
        const auto bin=nearestOddCoherentBin(requested,sampleRate,65536);
        CHECK(bin&&*bin%2==1);
        if(bin) CHECK(std::abs(sampleRate*static_cast<double>(*bin)/65536-requested)<=sampleRate/65536+1e-12);
    }
}

void legitimateHarmonicsAndKnownAliasPower()
{
    std::cout<<"SECTION_reference_harmonic_mask_and_known_nonharmonic_power\n";
    constexpr std::size_t frames=4096,fundamental=101,injectedBin=317;
    constexpr double rate=48000;
    const auto reference=rectangularPowerSpectrum(analytic64xReference(frames,fundamental,.4,.2));
    const auto clean=measureAlias(reference,reference,rate,fundamental);
    CHECK(clean.valid);
    CHECK(clean.referenceUnwantedDbc<=-100);
    CHECK(clean.unwantedDbc<=-100);
    CHECK(hasBin(clean,fundamental));
    CHECK(hasBin(clean,2*fundamental));
    CHECK(!hasBin(clean,injectedBin));
    CHECK(std::abs(clean.fundamentalPower-.08)<1e-6);

    auto candidate=analytic64xReference(frames,fundamental,.4,.2);
    addTone(candidate,injectedBin,.04);
    const auto contaminated=measureAlias(rectangularPowerSpectrum(candidate),reference,rate,fundamental);
    CHECK(contaminated.valid);
    CHECK(std::abs(contaminated.unwantedDbc-(-20.))<.002);
    CHECK(std::abs(contaminated.unwantedPower-.0008)<1e-7);
    CHECK(!passesAliasGate(contaminated,contaminated));

    auto quietHarmonic=analytic64xReference(frames,fundamental,.4,4e-8);
    const auto belowMaskFloor=measureAlias(rectangularPowerSpectrum(candidate),rectangularPowerSpectrum(quietHarmonic),rate,fundamental);
    CHECK(belowMaskFloor.valid);
    CHECK(!hasBin(belowMaskFloor,2*fundamental));
    CHECK(belowMaskFloor.unwantedDbc>-7);

    const auto dryReference=rectangularPowerSpectrum(analytic64xReference(frames,fundamental,.4));
    const auto mismatched=measureAlias(reference,dryReference,rate,fundamental);
    CHECK(mismatched.valid);
    CHECK(mismatched.unwantedDbc>-7);
    CHECK(!passesAliasGate(mismatched,contaminated));
}

void invalidReferenceAndSilentFundamental()
{
    std::cout<<"SECTION_bad_reference_and_missing_fundamental_invalidate_measurement\n";
    constexpr std::size_t frames=4096,fundamental=101;
    constexpr double rate=48000;
    const auto clean=rectangularPowerSpectrum(tone(frames,fundamental,.4));
    auto dirtyReference=tone(frames,fundamental,.4);
    addTone(dirtyReference,317,4e-5); // -80 dBc, twenty dB above the reference limit.
    const auto invalid=measureAlias(clean,rectangularPowerSpectrum(dirtyReference),rate,fundamental);
    CHECK(!invalid.valid);
    CHECK(std::abs(invalid.referenceUnwantedDbc-(-80.))<.02);
    const auto silent=rectangularPowerSpectrum(std::vector<double>(frames));
    const auto silentCandidate=measureAlias(silent,clean,rate,fundamental);
    const auto silentReference=measureAlias(clean,silent,rate,fundamental);
    CHECK(!silentCandidate.valid);
    CHECK(!silentReference.valid);
    CHECK(!passesAliasGate(silentCandidate,invalid));
    CHECK(!measureAlias(clean,clean,rate,0).valid);
    CHECK(!measureAlias(clean,clean,0,fundamental).valid);
    CHECK(!measureAlias(clean,rectangularPowerSpectrum(tone(1024,11,.4)),rate,fundamental).valid);
}

void cutoffAndNegativeAliasedControl()
{
    std::cout<<"SECTION_cutoff_and_deliberately_unoversampled_control\n";
    constexpr std::size_t frames=4096,fundamental=601;
    constexpr double rate=48000;
    const auto reference=rectangularPowerSpectrum(tone(frames,fundamental,.4));
    auto unoversampled=tone(frames,fundamental,.4);
    for(auto& sample:unoversampled) sample=std::tanh(40*sample);
    const auto negative=measureAlias(rectangularPowerSpectrum(unoversampled),reference,rate,fundamental);
    CHECK(negative.valid);
    CHECK(negative.unwantedDbc>-50);
    CHECK(!passesAliasGate(negative,negative));

    auto smallAlias=tone(frames,fundamental,.4);
    addTone(smallAlias,317,.0004); // -60 dBc and comfortably ten dB below the control.
    const auto passing=measureAlias(rectangularPowerSpectrum(smallAlias),reference,rate,fundamental);
    CHECK(passing.valid);
    CHECK(std::abs(passing.unwantedDbc-(-60.))<.02);
    CHECK(passesAliasGate(passing,negative));
    CHECK(!passesAliasGate(passing,passing));

    for(double sampleRate:{32000.,48000.,96000.})
    {
        auto outside=tone(frames,101,.4);
        const auto cutoff=std::min(16000.,.4*sampleRate);
        const auto outsideBin=static_cast<std::size_t>(std::ceil(cutoff*frames/sampleRate));
        addTone(outside,outsideBin,.04);
        const auto cleanReference=rectangularPowerSpectrum(tone(frames,101,.4));
        const auto result=measureAlias(rectangularPowerSpectrum(outside),cleanReference,sampleRate,101);
        CHECK(result.valid);
        CHECK(result.unwantedDbc<=-100);
    }
}

void nearestRankTiming()
{
    std::cout<<"SECTION_timing_mean_max_and_nearest_rank_p99\n";
    std::vector<double> callbacks;
    for(int i=100;i>=1;--i) callbacks.push_back(static_cast<double>(i)*1e-6);
    const auto hundred=summarizeTiming(callbacks);
    CHECK(hundred.valid);
    CHECK(hundred.count==100);
    CHECK(std::abs(hundred.meanSeconds-50.5e-6)<1e-15);
    CHECK(std::abs(hundred.p99Seconds-99e-6)<1e-15);
    CHECK(std::abs(hundred.maximumSeconds-100e-6)<1e-15);
    CHECK(std::abs(callbacks.front()-100e-6)<1e-15); // Analysis must not reorder the raw evidence.
    callbacks.push_back(101e-6);
    const auto hundredOne=summarizeTiming(callbacks);
    CHECK(hundredOne.valid);
    CHECK(std::abs(hundredOne.p99Seconds-100e-6)<1e-15);
    const std::vector<double> three{3e-6,1e-6,2e-6};
    CHECK(std::abs(summarizeTiming(three).p99Seconds-3e-6)<1e-15);
    CHECK(!summarizeTiming(std::span<const double>{}).valid);
    const std::vector<double> negative{-1e-6,1e-6};
    const std::vector<double> nonfinite{1e-6,std::numeric_limits<double>::infinity()};
    CHECK(!summarizeTiming(negative).valid);
    CHECK(!summarizeTiming(nonfinite).valid);
}

void zeroAliasBaselineCannotProveImprovement()
{
    std::cout<<"SECTION_exact_zero_alias_baseline_has_no_measurable_improvement\n";
    PowerSpectrum exact;
    exact.valid=true;exact.frames=1024;exact.oneSidedPower.resize(513);
    exact.oneSidedPower[37]=1;
    const auto clean=measureAlias(exact,exact,48000,37);
    CHECK(clean.valid);
    CHECK(std::isinf(clean.unwantedDbc)&&clean.unwantedDbc<0);
    CHECK(!passesAliasGate(clean,clean));
    auto noisy=exact;noisy.oneSidedPower[113]=.01;
    const auto positiveBaseline=measureAlias(noisy,exact,48000,37);
    CHECK(positiveBaseline.valid);
    CHECK(passesAliasGate(clean,positiveBaseline));
}
}

int main()
{
    signalAndPowerCalibration();coherentBins();legitimateHarmonicsAndKnownAliasPower();
    invalidReferenceAndSilentFundamental();cutoffAndNegativeAliasedControl();nearestRankTiming();zeroAliasBaselineCannotProveImprovement();
    std::cout<<"Saturation analysis: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
