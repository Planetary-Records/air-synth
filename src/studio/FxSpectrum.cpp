#include "FxSpectrum.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <thread>
namespace airack::studio
{
struct FxSpectrum::State
{
    const std::thread::id owner=std::this_thread::get_id();
    static constexpr std::size_t ringSize=2*fftSize,hop=fftSize/2;
    juce::dsp::FFT fft{fftOrder};
    std::array<std::array<float,4>,ringSize> ring{};
    std::array<float,fftSize> window{};
    std::array<std::complex<float>,fftSize> input{},output{};
    std::array<double,bins> inputPower{},outputPower{};
    Result result;
    const FxMonitor* source=nullptr;
    FxMonitor::Token token;
    std::uint64_t total=0,nextWindow=fftSize,expected=0,lastWindow=0,continuity=0;
    std::size_t write=0;
    double windowSum=0;
    bool havePacket=false,spectrumEnabled=true;
    State()
    {
        juce::dsp::WindowingFunction<float>::fillWindowingTables(window.data(),window.size(),juce::dsp::WindowingFunction<float>::hann,false);
        windowSum=std::accumulate(window.begin(),window.end(),0.);
    }
    void reset(std::uint64_t generation=0,double rate=0) noexcept
    {
        result={};result.generation=generation;result.sampleRate=rate;result.continuity=++continuity;
        result.inputDb.fill(floorDb);result.outputDb.fill(floorDb);
        total=0;nextWindow=fftSize;expected=0;lastWindow=0;write=0;havePacket=false;
    }
    void analyse(std::uint64_t end) noexcept
    {
        const auto behind=std::size_t(total-end);
        const auto start=(write+ringSize-fftSize-behind)%ringSize;
        inputPower.fill(0);outputPower.fill(0);
        for(std::size_t channel=0;channel<4;++channel){
            for(std::size_t n=0;n<fftSize;++n)input[n]={ring[(start+n)%ringSize][channel]*window[n],0};
            fft.perform(input.data(),output.data(),false);
            auto& power=channel<2?inputPower:outputPower;
            for(std::size_t k=0;k<bins;++k){const auto real=double(output[k].real()),imaginary=double(output[k].imag());power[k]+=real*real+imaginary*imaginary;}
        }
        const auto seconds=double(end-lastWindow)/result.sampleRate;
        const auto smooth=[&](std::array<float,bins>& display,const std::array<double,bins>& power){
            for(std::size_t k=0;k<bins;++k){
                const auto scale=(k==0||k==bins-1?1.:2.)/windowSum;
                const auto energy=power[k]*.5*scale*scale;
                const auto db=float(std::clamp(10.*std::log10(std::max(energy,1e-12)),-120.,120.));
                // First complete window is exact; subsequent display attack is
                //20ms and release150ms, with real stream time between windows.
                const auto a=result.valid?std::exp(-seconds/(db>display[k]?.02:.15)):0.;
                display[k]=float(db+a*(display[k]-db));
            }
        };
        smooth(result.inputDb,inputPower);smooth(result.outputDb,outputPower);
        result.valid=true;++result.windows;result.endSample=expected-(total-end);lastWindow=end;
    }
};
FxSpectrum::FxSpectrum():state(std::make_unique<State>()){clear();}
FxSpectrum::~FxSpectrum()=default;
bool FxSpectrum::update(FxMonitor& monitor,FxMonitor::Token token,bool analyseSpectrum) noexcept
{std::size_t budget=FxMonitor::packetCapacity;return update(monitor,token,analyseSpectrum,budget);}
bool FxSpectrum::update(FxMonitor& monitor,FxMonitor::Token token,bool analyseSpectrum,std::size_t& budget) noexcept
{
    auto& s=*state;
    if(std::this_thread::get_id()!=s.owner)return false;
    if(s.source!=&monitor||s.token!=token||s.spectrumEnabled!=analyseSpectrum){s.reset();s.source=&monitor;s.token=token;s.spectrumEnabled=analyseSpectrum;}
    const auto generation=monitor.generation(token);
    if(generation!=s.result.generation)s.reset(generation);
    if(generation==0)return false;
    FxMonitor::Packet packet;
    while(budget>0&&monitor.read(token,packet,budget)){
        if(packet.generation!=s.result.generation||packet.sampleRate!=s.result.sampleRate||(s.havePacket&&packet.firstSample!=s.expected))
            s.reset(packet.generation,packet.sampleRate);
        s.havePacket=true;s.expected=packet.firstSample+FxMonitor::packetFrames;s.result.movementDb=packet.movementDb;
        s.result.spectralMovementDb=packet.spectralMovementDb;
        s.result.detectorReadbacks=packet.detectorReadbacks;s.result.detectorReadbacksValid=packet.detectorReadbacksValid;s.result.externalAvailable=packet.externalAvailable;
        if(analyseSpectrum)for(const auto& frame:packet.samples){s.ring[s.write]=frame;s.write=(s.write+1)%State::ringSize;++s.total;}
    }
    // A concurrent reset cannot publish the previous stream's spectrum.
    if(monitor.generation(token)!=s.result.generation){s.reset(monitor.generation(token));return false;}
    if(!analyseSpectrum)return false;
    if(s.total<s.nextWindow)return false;
    const auto due=1+(s.total-s.nextWindow)/State::hop;
    const auto end=s.nextWindow+(due-1)*State::hop;
    s.result.skipped+=due-1;s.analyse(end);s.nextWindow=end+State::hop;return true;
}
void FxSpectrum::clear() noexcept{state->reset();state->source=nullptr;state->token={};}
const FxSpectrum::Result& FxSpectrum::result() const noexcept{return state->result;}
float FxSpectrum::dbAt(double frequency,bool output) const noexcept
{return dbAt(state->result,frequency,output);}
float FxSpectrum::dbAt(const Result& r,double frequency,bool output) noexcept
{
    if(!r.valid||!std::isfinite(frequency)||frequency<0||frequency>r.sampleRate*.5)return floorDb;
    const auto position=frequency*fftSize/r.sampleRate;
    const auto lo=std::size_t(std::floor(position)),hi=std::min(lo+1,std::size_t(bins-1));
    const auto& levels=output?r.outputDb:r.inputDb;
    return float(std::lerp(double(levels[lo]),double(levels[hi]),position-lo));
}
}
