#include "FxMonitor.h"
#include "SpectralEq.h"
#include "DynamicEq.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace airack
{
namespace {
constexpr std::uint64_t saturationKind=std::uint64_t{1}<<63;
constexpr std::uint64_t saturationGenerationMask=(std::uint64_t{1}<<60)-1;
bool saturationToken(FxMonitor::SaturationToken token) noexcept {
    return token.slot<FxMonitor::slots && (token.id&saturationGenerationMask)!=0
        && (token.id&~saturationGenerationMask)==saturationKind;
}
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
struct FxMonitor::State
{
    struct Slot
    {
        juce::AbstractFifo fifo{int(packetCapacity+1)};
        std::array<Packet,packetCapacity+1> packets{};
        Packet staging;
        std::atomic<std::uint64_t> subscription{0},visibleGeneration{1};
        std::atomic<std::uint64_t> captured{0},published{0},dropped{0},clipped{0};
        std::atomic<std::uint64_t> audition{0};
        std::atomic<bool> auditionActive{false};
        // Only the render owner touches the remaining fields.
        std::uint64_t blockToken=0,streamGeneration=1,nextSample=0,totalCaptured=0,totalClipped=0;
        std::size_t staged=0;
        double rate=0;
        bool active=false;
        void invalidate() noexcept
        {
            staged=0;nextSample=0;
            // Exhaustion disables this stream rather than reusing an epoch.
            if(streamGeneration!=0)streamGeneration=streamGeneration==std::numeric_limits<std::uint64_t>::max()?0:streamGeneration+1;
            visibleGeneration.store(streamGeneration,std::memory_order_release);
            captured.store(totalCaptured,std::memory_order_relaxed);clipped.store(totalClipped,std::memory_order_relaxed);
        }
        float displaySample(double v) noexcept
        {
            constexpr double limit=1e12;
            if(!std::isfinite(v)){++totalClipped;return 0;}
            if(std::abs(v)>limit){++totalClipped;v=std::clamp(v,-limit,limit);}
            return float(v);
        }
    };
    std::array<Slot,slots> streams;
    struct SaturationCommand {
        std::atomic<std::uint64_t> word{0};
        std::atomic<bool> running{false};
        std::uint64_t blockGeneration=0;
    };
    std::array<SaturationCommand,slots> saturation;
    // Message-thread only, never wrapped or reused.
    std::uint64_t nextToken=1;
    std::atomic<double> preparedRate{0};
    std::atomic<bool> externalInput{false};
};
FxMonitor::FxMonitor():state(std::make_unique<State>()){}
FxMonitor::~FxMonitor()=default;
std::optional<FxMonitor::Token> FxMonitor::subscribe(std::uint8_t slot) noexcept
{
    if(slot>=slots||state->nextToken==0)return {};
    std::uint64_t empty=0;
    const auto token=state->nextToken;
    if(!state->streams[slot].subscription.compare_exchange_strong(empty,token,std::memory_order_release,std::memory_order_relaxed))return {};
    state->nextToken=token==((std::uint64_t(1)<<60)-1)?0:token+1;
    return Token{token,slot};
}
bool FxMonitor::retire(Token token) noexcept
{
    if(token.slot>=slots||token.id==0)return false;
    auto& s=state->streams[token.slot];
    if(!s.subscription.compare_exchange_strong(token.id,0,std::memory_order_acq_rel,std::memory_order_relaxed))return false;
    s.audition.store(0,std::memory_order_release);return true;
}
std::uint64_t FxMonitor::generation(Token token) const noexcept
{
    if(token.slot>=slots||token.id==0)return 0;
    const auto& s=state->streams[token.slot];
    return s.subscription.load(std::memory_order_acquire)==token.id?s.visibleGeneration.load(std::memory_order_acquire):0;
}
bool FxMonitor::requestAudition(Token token,std::optional<std::size_t> band,bool detector) noexcept
{
    if(token.slot>=slots || token.id==0 || (band && *band>=8))return false;
    auto& s=state->streams[token.slot];
    if(s.subscription.load(std::memory_order_acquire)!=token.id)return false;
    s.audition.store(band?(token.id<<4)|*band|(detector?8u:0u):0,std::memory_order_release);return true;
}
std::optional<std::size_t> FxMonitor::requestedAudition(Token token) const noexcept
{
    if(token.slot>=slots || token.id==0)return {};
    const auto& s=state->streams[token.slot];const auto packed=s.audition.load(std::memory_order_acquire);
    if(packed==0 || (packed>>4)!=token.id || s.subscription.load(std::memory_order_acquire)!=token.id)return {};
    return std::size_t(packed&7);
}
std::optional<std::size_t> FxMonitor::auditionBand(std::uint8_t slot) const noexcept
{
    const auto request=auditionRequest(slot);return request?std::optional<std::size_t>(request->band):std::nullopt;
}
std::optional<FxMonitor::Audition> FxMonitor::auditionRequest(std::uint8_t slot) const noexcept
{
    if(slot>=slots)return {};
    const auto& s=state->streams[slot];if(!s.active||s.blockToken==0)return {};
    const auto packed=s.audition.load(std::memory_order_acquire);
    if(packed==0 || (packed>>4)!=s.blockToken || s.subscription.load(std::memory_order_acquire)!=s.blockToken)return {};
    return Audition{std::size_t(packed&7),(packed&8)!=0};
}
bool FxMonitor::externalAvailable() const noexcept {return state->externalInput.load(std::memory_order_acquire);}
void FxMonitor::cancelAudition(std::uint8_t slot) noexcept
{if(slot<slots)state->streams[slot].audition.store(0,std::memory_order_release);}
void FxMonitor::auditionRunning(std::uint8_t slot,bool active) noexcept
{if(slot<slots)state->streams[slot].auditionActive.store(active,std::memory_order_release);}
double FxMonitor::auditionTailSeconds() const noexcept
{
    double seconds=0;
    for(const auto& s:state->streams)if(s.audition.load(std::memory_order_acquire)!=0 || s.auditionActive.load(std::memory_order_acquire))seconds+=8;
    for(const auto& s:state->saturation)if((s.word.load(std::memory_order_acquire)&3)!=0 || s.running.load(std::memory_order_acquire))seconds+=2.02;
    return seconds;
}
std::optional<FxMonitor::SaturationToken> FxMonitor::acquireSaturationAudition(std::uint8_t slot) noexcept
{
    if(slot>=slots || nextSaturationGeneration==0)return {};
    std::uint64_t empty=0;const auto generation=nextSaturationGeneration;
    if(!state->saturation[slot].word.compare_exchange_strong(empty,generation<<2,std::memory_order_acq_rel,std::memory_order_relaxed))return {};
    nextSaturationGeneration=generation==saturationGenerationMask?0:generation+1;
    return SaturationToken{saturationKind|generation,slot};
}
bool FxMonitor::requestSaturationAudition(SaturationToken token,std::optional<SaturationLane> lane) noexcept
{
    if(!saturationToken(token) || (lane&&(*lane==SaturationLane::full || static_cast<unsigned>(*lane)>static_cast<unsigned>(SaturationLane::high))))return false;
    auto& word=state->saturation[token.slot].word;auto expected=word.load(std::memory_order_acquire);
    const auto generation=token.id&saturationGenerationMask;
    if((expected>>2)!=generation)return false;
    const auto next=(generation<<2)|(lane?static_cast<std::uint64_t>(*lane):0);
    return word.compare_exchange_strong(expected,next,std::memory_order_acq_rel,std::memory_order_relaxed);
}
bool FxMonitor::retireSaturationAudition(SaturationToken token) noexcept
{
    if(!saturationToken(token))return false;
    auto& word=state->saturation[token.slot].word;auto expected=word.load(std::memory_order_acquire);
    if((expected>>2)!=(token.id&saturationGenerationMask))return false;
    return word.compare_exchange_strong(expected,0,std::memory_order_acq_rel,std::memory_order_relaxed);
}
std::optional<SaturationLane> FxMonitor::saturationAuditionLane(std::uint8_t slot) const noexcept
{
    if(slot>=slots)return {};
    const auto& s=state->saturation[slot];const auto word=s.word.load(std::memory_order_acquire);
    if(s.blockGeneration==0 || (word>>2)!=s.blockGeneration || (word&3)==0)return {};
    return SaturationLane(word&3);
}
void FxMonitor::cancelSaturationAudition(std::uint8_t slot) noexcept
{if(slot<slots)state->saturation[slot].word.store(0,std::memory_order_release);}
void FxMonitor::saturationAuditionRunning(std::uint8_t slot,bool running) noexcept
{if(slot<slots)state->saturation[slot].running.store(running,std::memory_order_release);}
void FxMonitor::beginSaturationBlock(const ActiveSlots& active) noexcept
{
    for(std::size_t slot=0;slot<slots;++slot){auto& s=state->saturation[slot];
        if(!active[slot]){s.word.store(0,std::memory_order_release);s.running.store(false,std::memory_order_release);}
        s.blockGeneration=active[slot]?s.word.load(std::memory_order_acquire)>>2:0;
    }
}
double FxMonitor::sampleRate() const noexcept{return state->preparedRate.load(std::memory_order_acquire);}
bool FxMonitor::publishSampleRate(double rate) noexcept
{
    if(!std::isfinite(rate)||(rate!=0&&(rate<8000||rate>1536000)))return false;
    if(rate==0)state->externalInput.store(false,std::memory_order_release);
    if(state->preparedRate.load(std::memory_order_relaxed)!=rate){
        for(auto& s:state->saturation)s.word.store(0,std::memory_order_release);
        state->preparedRate.store(rate,std::memory_order_release);
    }
    if(rate==0)for(auto& s:state->saturation){s.word.store(0,std::memory_order_release);s.running.store(false,std::memory_order_release);}
    return true;
}
FxMonitor::Stats FxMonitor::stats(std::uint8_t slot) const noexcept
{
    if(slot>=slots)return {};
    const auto& s=state->streams[slot];
    return {s.captured.load(std::memory_order_relaxed),s.published.load(std::memory_order_relaxed),s.dropped.load(std::memory_order_relaxed),s.clipped.load(std::memory_order_relaxed),s.fifo.getNumReady()};
}
bool FxMonitor::read(Token token,Packet& output) noexcept
{std::size_t budget=packetCapacity;return read(token,output,budget);}
bool FxMonitor::read(Token token,Packet& output,std::size_t& budget) noexcept
{
    if(generation(token)==0)return false;
    auto& s=state->streams[token.slot];
    while(budget>0){
        int start=0,count=0,second=0,secondCount=0;s.fifo.prepareToRead(1,start,count,second,secondCount);
        if(count==0)return false;
        --budget;
        const auto& packet=s.packets[std::size_t(start)];
        const bool current=packet.token==token.id&&packet.generation==s.visibleGeneration.load(std::memory_order_acquire);
        if(current)output=packet;
        s.fifo.finishedRead(1);
        if(current)return true;
    }
    return false;
}
bool FxMonitor::beginBlock(double rate,const ActiveSlots& active,bool external) noexcept
{
    if(!std::isfinite(rate)||rate<8000||rate>1536000)return false;
    if(state->externalInput.load(std::memory_order_relaxed)!=external)state->externalInput.store(external,std::memory_order_release);
    (void)publishSampleRate(rate);
    for(std::size_t i=0;i<slots;++i){
        auto& s=state->streams[i];const auto token=active[i]?s.subscription.load(std::memory_order_acquire):0;
        if(!active[i] || (s.rate!=0 && s.rate!=rate))s.audition.store(0,std::memory_order_release);
        if(!active[i])s.auditionActive.store(false,std::memory_order_release);
        if(s.rate!=rate||s.active!=active[i]||s.blockToken!=token)s.invalidate();
        s.rate=rate;s.active=active[i];s.blockToken=token;
    }
    return true;
}
std::size_t FxMonitor::framesUntilPacket(std::uint8_t slot) const noexcept
{
    if(slot>=slots)return 0;
    const auto& s=state->streams[slot];
    return s.blockToken==0||s.streamGeneration==0?0:packetFrames-s.staged;
}
void FxMonitor::capture(std::uint8_t slot,Frame input,Frame output,const DynamicEq* meter,const SpectralEq* spectral) noexcept
{
    if(slot>=slots)return;
    auto& s=state->streams[slot];if(s.blockToken==0||s.streamGeneration==0)return;
    if(s.staged==0){s.staging.token=s.blockToken;s.staging.generation=s.streamGeneration;s.staging.firstSample=s.nextSample;s.staging.sampleRate=s.rate;}
    s.staging.samples[s.staged++]={s.displaySample(input[0]),s.displaySample(input[1]),s.displaySample(output[0]),s.displaySample(output[1])};
    ++s.nextSample;++s.totalCaptured;
    if(s.staged<packetFrames)return;
    s.staged=0;s.captured.store(s.totalCaptured,std::memory_order_relaxed);s.clipped.store(s.totalClipped,std::memory_order_relaxed);
    if(s.subscription.load(std::memory_order_acquire)!=s.blockToken){s.dropped.fetch_add(1,std::memory_order_relaxed);return;}
    int start=0,count=0,second=0,secondCount=0;s.fifo.prepareToWrite(1,start,count,second,secondCount);
    if(count==0){s.dropped.fetch_add(1,std::memory_order_relaxed);return;}
    s.staging.movementDb=meter?meter->dynamicGainsDb():std::array<float,8>{};
    s.staging.spectralMovementDb=spectral?spectral->dynamicGainsDb():std::array<float,8>{};
    s.staging.detectorReadbacks=meter?meter->detectorReadbacks():std::array<DynamicEqDetectorReadback,8>{};
    s.staging.detectorReadbacksValid=meter!=nullptr;s.staging.externalAvailable=state->externalInput.load(std::memory_order_relaxed);
    s.packets[std::size_t(start)]=s.staging;s.fifo.finishedWrite(1);s.published.fetch_add(1,std::memory_order_relaxed);
}
void FxMonitor::invalidate() noexcept
{
    for(auto& s:state->streams){s.audition.store(0,std::memory_order_release);s.auditionActive.store(false,std::memory_order_release);s.invalidate();}
    for(auto& s:state->saturation){s.word.store(0,std::memory_order_release);s.running.store(false,std::memory_order_release);}
}
}
