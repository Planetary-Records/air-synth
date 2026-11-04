#pragma once
#include "PatternScheduler.h"
#include "DrumEngine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <bitset>
namespace airack {
enum class PerformanceInputStatus : std::uint8_t { ready, keyboardOverflow, midiCapacity };
enum class DrumPerformanceStatus : std::uint8_t {ready,invalid,staleTarget,unavailable,queueFull,deferred,missedOffset,eventCapacity};
struct DrumPadPress final {
    std::uint64_t token=0;
    std::uint32_t hostOffset=0;
    std::uint8_t bank=0,pad=0,velocity=0,channel=1;
};
static_assert(sizeof(DrumPadPress)==16);
static_assert(std::is_trivially_copyable_v<DrumPadPress>);
// One UI producer, one audio consumer. Fixed storage is allocated with the
// processor, never retired/replaced while callbacks run.
class PerformanceMidi final {
public:
    enum class Source : std::uint8_t { pattern, host, keyboard };
    static constexpr std::size_t capacity=8192;
    bool enqueue(bool,std::uint8_t,std::uint8_t,std::uint8_t) noexcept;
    bool enqueueDrum(const DrumPadPress&) noexcept;
    void beginDrumCallback() noexcept;
    std::span<const DrumPadPress> drumPresses() const noexcept {return std::span(drumCutoff).first(drumCount);}
    bool drumPressExpired(std::size_t index) const noexcept {return drumExpired[drumSlots[index]];}
    void deferDrumCallback() noexcept;
    void consumeDrumCallback() noexcept;
    void stageExactDrum(const DrumPadPress&,int chunkOffset) noexcept;
    void requestDrumStop() noexcept;
    void suppressDrums(bool stopAtStart) noexcept {drumSuppressed=true;drumEmergency|=stopAtStart;}
    std::span<const drums::RenderEvent> drumEvents() const noexcept {return std::span(drumOutput).first(drumOutputCount);}
    DrumPerformanceStatus drumStatus() const noexcept {return drumFeedback.load();}
    void reportDrumStatus(DrumPerformanceStatus) noexcept;
    void cancelKeyboard() noexcept;
    PerformanceInputStatus status() const noexcept {return feedback.load();}
    void prepare();
    void clearOwners(bool preserveDeferred=false) noexcept;
    void deferReset(std::uint64_t token,bool externalMidi=true) noexcept;
    void cancelDeferredReset() noexcept;
    void stageDeferredReset(std::uint64_t token,bool includeHost) noexcept;
    void beginCallback() noexcept;
    void beginChunk() noexcept;
    void stageKeyboard() noexcept;
    void releaseSource(Source) noexcept;
    void releaseAll() noexcept;
    void stagePattern(const PatternEventBuffer&,std::uint8_t phase=1) noexcept;
    void stageHost(const juce::MidiMessageMetadata&,int offset,bool deferredOnly=false) noexcept;
    bool commit() noexcept;
    const juce::MidiBuffer& midi() const noexcept {return output;}
    PatternScheduler scheduler;
    PatternEventBuffer patternEvents;
private:
    friend struct InstrumentDrumTestAccess;
    struct KeyEvent {std::uint64_t generation=0;bool on=false;std::uint8_t pitch=0,velocity=0,channel=1;};
    enum class Kind : std::uint8_t {midi,exactPad,drumStop,drumOnlyMidi,replayedMidi};
    struct Event {std::uint64_t token=0;int offset=0;std::uint32_t ordinal=0;Source source=Source::host;std::uint8_t phase=0,size=3;std::array<juce::uint8,3> data{};bool deferredOnly=false;Kind kind=Kind::midi;std::uint8_t bank=0;};
    void stage(Source,int,std::uint8_t,const juce::uint8*,std::uint8_t,bool deferredOnly=false) noexcept;
    bool emit(const juce::uint8*,int,int) noexcept;
    bool off(Source,std::size_t,int) noexcept;
    bool apply(const Event&) noexcept;
    void projectDrum(const Event&) noexcept;
    void capacityFailure() noexcept;
    void emergencyDrumStop() noexcept;
    void clearDeferredNotes() noexcept;
    juce::AbstractFifo fifo {257};
    juce::AbstractFifo drumFifo {65};
    std::array<DrumPadPress,65> drumQueue{};
    std::array<DrumPadPress,64> drumCutoff{};
    std::array<std::uint8_t,64> drumSlots{};
    std::bitset<65> drumExpired;
    std::size_t drumCount=0;
    std::atomic<DrumPerformanceStatus> drumFeedback{DrumPerformanceStatus::ready};
    std::array<KeyEvent,257> queue {};
    std::atomic<std::uint64_t> generation {0};
    std::atomic<PerformanceInputStatus> feedback {PerformanceInputStatus::ready};
    std::uint64_t observedGeneration=0;
    std::array<KeyEvent,256> keyboard {};
    std::size_t keyboardCount=0;
    bool keyboardRelease=false;
    std::array<std::array<bool,16*128>,3> owners {};
    std::array<std::array<std::uint8_t,16*128>,2> deferredNotes {};
    std::uint64_t deferredToken=0;
    bool deferredReset=false,deferredExternal=false;
    std::array<Event,capacity> staged {};
    std::size_t stagedCount=0, emitted=0;
    bool overflow=false;
    juce::MidiBuffer output;
    std::array<drums::RenderEvent,capacity> drumOutput{};
    std::size_t drumOutputCount=0;
    bool drumStopRequested=false;
    bool drumSuppressed=false,drumEmergency=false;
};
}
