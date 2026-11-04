#pragma once
#include "DrumAsset.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace airack { class RoutedSynthEngine; }
namespace airack::drums
{
enum class RenderEventKind : std::uint8_t { noteOn,exactPad,stopChannel,stopAll };
struct RenderEvent final
{
    std::uint64_t token=0;
    int hostOffset=0;
    RenderEventKind kind=RenderEventKind::noteOn;
    std::uint8_t bank=0,padOrNote=0,velocity=0,channel=1;
};
static_assert(std::is_trivially_copyable_v<RenderEvent>);
struct PadSettings
{
    const SampleAsset* sample=nullptr;
    float gainDb=0,pan=0,tuneSemitones=0,trimStart=0,trimEnd=1;
    float attackSeconds=0,decaySeconds=0;
    int midiNote=36,chokeGroup=0;
};
struct RackSettings
{
    std::array<PadSettings,16> pads=[] {
        std::array<PadSettings,16> p{};for(std::size_t n=0;n<p.size();++n)p[n].midiNote=36+static_cast<int>(n);return p;
    }();
    bool enabled=true;
    float level=1,pan=0;
};
// One owning thread. Pointers are borrowed, including current settings and tails.
// Callers retain assets until references() is false; no concurrent reclamation.
class DrumEngine final
{
public:
    DrumEngine() noexcept=default;
    DrumEngine(const DrumEngine&)=delete;
    DrumEngine& operator=(const DrumEngine&)=delete;
    bool prepare(double physicalSampleRate,int maximumPhysicalBlock) noexcept;
    bool setSettings(const RackSettings&) noexcept;
    void render(juce::AudioBuffer<float>&,const juce::MidiBuffer&) noexcept;
    void reset() noexcept;
    bool references(const SampleAsset*) const noexcept;
    // Render/lifecycle owner only; configured16 followed by captured32, including nulls.
    void borrowedReferences(std::array<const SampleAsset*,48>&) const noexcept;
    // Callback exclusion and worker completion are required before this lifecycle clear.
    void clearBorrowedSamplesQuiescent() noexcept;
    std::size_t activeHitCount() const noexcept;
private:
    friend class airack::RoutedSynthEngine;
    friend struct DrumEngineTestAccess;
    static bool validSettings(const RackSettings&) noexcept;
    bool beginRender(juce::AudioBuffer<float>&,juce::MidiBufferIterator,juce::MidiBufferIterator,int factor) noexcept;
    bool beginRender(juce::AudioBuffer<float>&,std::span<const RenderEvent>,std::uint8_t bank,int factor) noexcept;
    bool renderRange(int first,int count,bool admitNoteOns=true,bool writeOutput=true) noexcept;
    bool renderRange(int first,int count,bool admitNoteOns,bool writeOutput,
        std::span<const std::uint64_t,16> installedTargets,bool& unavailable) noexcept;
    void endRender() noexcept;
    double tailSeconds() const noexcept;
    struct Ramp
    {
        double value=1,target=1,increment=0;
        int remaining=0;
        void set(double,int,bool) noexcept;
        double next() noexcept;
        void snap() noexcept;
    };
    struct ReadTable {
        static constexpr std::size_t cells=24*1024,count=cells+1;
        std::array<double,count> values{};
        ReadTable() noexcept;
        double at(double absoluteU) const noexcept;
    };
    static const ReadTable& preparedReadTable() noexcept;
    const ReadTable* readTable_=nullptr;
    struct ReadWeights {
        std::array<double,1153> values{};
        double readScale=0,readRadius=0,phase=0,normalization=0;
        int relativeFirst=0,relativeLast=0;
        bool valid=false;
    };
    ReadWeights readWeights_{};
#ifndef NDEBUG
    friend struct DrumQualityTestAccess;
#endif
    struct Hit
    {
        const SampleAsset* sample=nullptr;
        double step=1,velocity=1,readScale=1,readRadius=24;
        std::array<double,2> lastContribution{},stealFrom{};
        int stealIntervals=0,stealRemaining=0;
        bool hasRenderedContribution=false;
        std::uint32_t begin=0,end=0,elapsed=0,naturalFrames=0,attack=0,decay=0;
        int pad=0,channel=0,group=0,rank=0,release=0;
    };
    std::array<double,2> reconstructed(const Hit&,double position) noexcept;
    void gains(bool snap) noexcept;
    void retire(std::size_t) noexcept;
    void trigger(int pad,int channel,int velocity) noexcept;
    void event(const juce::MidiMessageMetadata&,bool admitNoteOns=true) noexcept;
    void event(const RenderEvent&,bool,std::span<const std::uint64_t,16>,bool&) noexcept;
    RackSettings settings_{};
    std::array<Hit,32> hits_{};
    std::array<std::array<Ramp,2>,16> padGains_{};
    std::array<Ramp,2> kitGains_{};
    double rate_=0;
    int maximumBlock_=0,rampFrames_=1;
    std::array<float*,2> output_{};
    juce::MidiBufferIterator next_{},end_{};
    std::int64_t nextOffset_=0;
    int sessionFrames_=0,cursor_=0,factor_=1;
    bool rendering_=false;
    std::span<const RenderEvent> events_;
    std::size_t eventCursor_=0;
    std::uint8_t bank_=0;
    bool authoritative_=false;
};
}
