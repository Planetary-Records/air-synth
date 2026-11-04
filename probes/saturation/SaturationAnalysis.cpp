#include "SaturationAnalysis.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace airack::probe::analysis
{
SignalSummary summarizeSignal(std::span<const double> samples) noexcept
{
    if(samples.empty()) return {};
    long double sum=0,squares=0;
    for(double sample:samples)
    {
        if(!std::isfinite(sample)) return {};
        sum+=sample;squares+=static_cast<long double>(sample)*sample;
    }
    SignalSummary result;
    result.mean=static_cast<double>(sum/samples.size());
    result.rms=std::sqrt(static_cast<double>(squares/samples.size()));
    result.valid=std::isfinite(result.mean)&&std::isfinite(result.rms);
    return result;
}

std::optional<std::size_t> nearestOddCoherentBin(double requested,double rate,std::size_t frames) noexcept
{
    if(!std::isfinite(requested)||!std::isfinite(rate)||rate<=0||requested<=0||requested>=rate/2||frames<4||frames%2) return {};
    const double desired=requested*static_cast<double>(frames)/rate;
    const auto highest=(frames/2-1)%2?frames/2-1:frames/2-2;
    auto lower=static_cast<std::size_t>(std::max(1.,2*std::floor((desired-1)/2)+1));
    lower=std::min(lower,highest);
    const auto upper=std::min(lower+2,highest);
    return desired-static_cast<double>(lower)<=static_cast<double>(upper)-desired?lower:upper;
}

PowerSpectrum rectangularPowerSpectrum(std::span<const double> samples)
{
    // Double precision keeps the -120 dBc harmonic-mask calibration above FFT roundoff.
    const auto count=samples.size();
    if(count<2||(count&(count-1))) return {};
    std::vector<std::complex<double>> data(count);
    for(std::size_t i=0;i<count;++i)
    {
        if(!std::isfinite(samples[i])) return {};
        data[i]=samples[i];
    }
    for(std::size_t i=1,j=0;i<count;++i)
    {
        auto bit=count>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) std::swap(data[i],data[j]);
    }
    for(std::size_t width=2;width<=count;width*=2)
    {
        const auto turn=std::polar(1.,-2*std::numbers::pi/static_cast<double>(width));
        for(std::size_t start=0;start<count;start+=width)
        {
            std::complex<double> phase=1;
            for(std::size_t offset=0;offset<width/2;++offset)
            {
                const auto even=data[start+offset],odd=phase*data[start+offset+width/2];
                data[start+offset]=even+odd;data[start+offset+width/2]=even-odd;phase*=turn;
            }
        }
        if(width==count) break;
    }
    PowerSpectrum result;
    result.frames=count;result.oneSidedPower.resize(count/2+1);
    const double normalizer=1./static_cast<double>(count)/static_cast<double>(count);
    for(std::size_t bin=0;bin<=count/2;++bin)
    {
        result.oneSidedPower[bin]=std::norm(data[bin])*normalizer*(bin==0||bin==count/2?1:2);
        if(!std::isfinite(result.oneSidedPower[bin])) return {};
    }
    result.valid=true;
    return result;
}

namespace
{
double dbc(double power,double fundamental) noexcept
{
    return power==0?-std::numeric_limits<double>::infinity():10*std::log10(power/fundamental);
}
bool usable(const PowerSpectrum& spectrum)
{
    return spectrum.valid&&spectrum.frames>=2&&spectrum.frames%2==0&&spectrum.oneSidedPower.size()==spectrum.frames/2+1
        &&std::all_of(spectrum.oneSidedPower.begin(),spectrum.oneSidedPower.end(),[](double value){return std::isfinite(value)&&value>=0;});
}
}

AliasMeasurement measureAlias(const PowerSpectrum& candidate,const PowerSpectrum& reference,double rate,std::size_t fundamental)
{
    AliasMeasurement result;
    if(!usable(candidate)||!usable(reference)||candidate.frames!=reference.frames||!std::isfinite(rate)||rate<=0||fundamental==0||fundamental>=candidate.frames/2) return result;
    result.fundamentalPower=candidate.oneSidedPower[fundamental];
    const auto referenceFundamental=reference.oneSidedPower[fundamental];
    if(result.fundamentalPower<=0||referenceFundamental<=0) return result;
    std::vector<bool> mask(candidate.frames/2+1,false);
    for(std::size_t bin=fundamental;bin<candidate.frames/2;bin+=fundamental)
        if(dbc(reference.oneSidedPower[bin],referenceFundamental)>-120)
        {
            mask[bin]=true;result.harmonicBins.push_back(bin);
        }
    double referenceUnwanted=0;
    const auto cutoff=std::min(16000.,.4*rate);
    for(std::size_t bin=0;bin<candidate.oneSidedPower.size()&&static_cast<double>(bin)*rate/static_cast<double>(candidate.frames)<cutoff;++bin)
        if(!mask[bin])
        {
            result.unwantedPower+=candidate.oneSidedPower[bin];
            referenceUnwanted+=reference.oneSidedPower[bin];
        }
    result.unwantedDbc=dbc(result.unwantedPower,result.fundamentalPower);
    result.referenceUnwantedDbc=dbc(referenceUnwanted,referenceFundamental);
    result.valid=result.referenceUnwantedDbc<=-100&&!std::isnan(result.unwantedDbc);
    return result;
}

bool passesAliasGate(const AliasMeasurement& candidate,const AliasMeasurement& oneX) noexcept
{
    return candidate.valid&&oneX.valid&&std::isfinite(oneX.unwantedDbc)&&oneX.unwantedPower>0
        &&candidate.unwantedDbc<=-50&&candidate.unwantedDbc<=oneX.unwantedDbc-10;
}

TimingSummary summarizeTiming(std::span<const double> seconds)
{
    if(seconds.empty()||!std::all_of(seconds.begin(),seconds.end(),[](double value){return std::isfinite(value)&&value>=0;})) return {};
    TimingSummary result;
    result.count=seconds.size();
    long double sum=0;
    for(const auto value:seconds) sum+=value;
    result.meanSeconds=static_cast<double>(sum/seconds.size());
    std::vector<double> ordered(seconds.begin(),seconds.end());
    std::sort(ordered.begin(),ordered.end());
    result.p99Seconds=ordered[static_cast<std::size_t>(std::ceil(.99*static_cast<double>(ordered.size())))-1];
    result.maximumSeconds=ordered.back();result.valid=true;
    return result;
}
}
