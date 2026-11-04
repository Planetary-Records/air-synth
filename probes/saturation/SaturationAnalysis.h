#pragma once
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace airack::probe::analysis
{
struct SignalSummary
{
    bool valid=false;
    double mean=0,rms=0;
};
SignalSummary summarizeSignal(std::span<const double>) noexcept;
std::optional<std::size_t> nearestOddCoherentBin(double requestedHz,double sampleRate,std::size_t frames) noexcept;

struct PowerSpectrum
{
    bool valid=false;
    std::size_t frames=0;
    std::vector<double> oneSidedPower;
};
PowerSpectrum rectangularPowerSpectrum(std::span<const double>);

struct AliasMeasurement
{
    bool valid=false;
    double fundamentalPower=0,unwantedPower=0;
    double unwantedDbc=std::numeric_limits<double>::quiet_NaN();
    double referenceUnwantedDbc=std::numeric_limits<double>::quiet_NaN();
    std::vector<std::size_t> harmonicBins;
};
AliasMeasurement measureAlias(const PowerSpectrum& candidate,const PowerSpectrum& reference,double sampleRate,std::size_t fundamentalBin);
bool passesAliasGate(const AliasMeasurement& candidate,const AliasMeasurement& oneX) noexcept;

struct TimingSummary
{
    bool valid=false;
    std::size_t count=0;
    double meanSeconds=0,p99Seconds=0,maximumSeconds=0;
};
TimingSummary summarizeTiming(std::span<const double>);
}
