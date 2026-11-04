#pragma once
#include "PatternScheduler.h"
#include <atomic>
#include <span>

namespace airack
{
inline constexpr std::size_t maximumObservedMidiNotes=256,maximumObservedMidiEvents=8192;
inline constexpr std::uint64_t observedMidiFreshnessMs=2000;
inline constexpr std::uint8_t observedMidiBars=4;
enum class ObservedMidiAvailability : std::uint8_t { unavailable,fresh,stale };
enum class ObservedMidiReason : std::uint8_t
{ none,noNotes,stopped,missingTiming,invalidTiming,internalMode,unprepared,invalidBlock,inputCapacity,stalePublisher,clockRollback };
struct ObservedMidiNote
{
    double startBeats=0,durationBeats=0;
    std::uint8_t pitch=60,velocity=100,channel=1;
    bool held=false;
    bool operator==(const ObservedMidiNote&) const = default;
};
struct ObservedMidiSnapshot
{
    std::optional<PhraseMeter> meter;
    std::optional<double> bpm;
    double windowBeats=0;
    std::uint64_t ageMs=0;
    ObservedMidiAvailability availability=ObservedMidiAvailability::unavailable;
    ObservedMidiReason reason=ObservedMidiReason::noNotes;
    bool truncated=false;
    std::uint16_t noteCount=0;
    std::array<ObservedMidiNote,maximumObservedMidiNotes> notes{};
    bool operator==(const ObservedMidiSnapshot&) const = default;
};
bool validateObservedMidiSnapshot(const ObservedMidiSnapshot&) noexcept;
class ObservedMidi final
{
public:
    ObservedMidi() = default;
    ObservedMidi(const ObservedMidi&) = delete;
    ObservedMidi& operator=(const ObservedMidi&) = delete;
    bool prepare(double sampleRate) noexcept;
    void release() noexcept;
    bool beginBlock(const HostPatternTiming&,std::optional<PhraseMeter>,int frames,PlaybackMode,std::uint64_t nowMs,bool hostPositionAvailable=true) noexcept;
    void event(std::span<const std::uint8_t> raw,int sampleOffset) noexcept;
    void endBlock(bool inputAccepted=true) noexcept;
    void capture(std::uint64_t nowMs,ObservedMidiSnapshot&) noexcept;
private:
    struct Publication {ObservedMidiSnapshot snapshot{};std::uint64_t timestamp=0;};
    std::array<Publication,3> slots{};
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    static constexpr std::uint8_t indexMask=3,dirty=4;
    std::atomic<std::uint8_t> middle{1};
    std::uint8_t front=0,back=2;
    struct Entry
    {
        double start=0,end=0;
        std::uint8_t pitch=60,velocity=100,channel=1;
        bool held=false;
    };
    std::array<Entry,maximumObservedMidiNotes> history{};
    std::array<std::int16_t,16*128> keys{};
    std::size_t first=0,count=0,eventCalls=0;
    double rate=0,origin=0,beatPerSample=0,boundary=0,expectedBeat=0,previousBpm=0;
    int frames=0;
    std::optional<std::int64_t> blockSample,expectedSample;
    PhraseMeter blockMeter{},previousMeter{};
    std::uint64_t blockTimestamp=0,previousNow=0,publishedTimestamp=0,windowTimestamp=0;
    bool inBlock=false,accepting=false,stopped=false,continuous=false,clockKnown=false,truncated=false;
    ObservedMidiSnapshot lastWindow{};
    void publish(const ObservedMidiSnapshot&,std::uint64_t) noexcept;
    void publishEmpty(ObservedMidiReason,std::uint64_t) noexcept;
    void resetHistory() noexcept;
    void closeKey(std::size_t,double) noexcept;
};
}
