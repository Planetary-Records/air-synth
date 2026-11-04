#pragma once
#include "PatternTiming.h"
#include <optional>
namespace airack {
enum class PlaybackMode : std::uint8_t { externalMidi, internalPattern };
struct PatternLoopRange { double startBeats=0, endBeats=4; };
struct HostPatternTiming {
    bool playing=false;
    std::optional<double> bpm, ppqPosition;
    bool looping=false;
    std::optional<PatternLoopRange> loop;
    std::optional<std::int64_t> samplePosition;
};
inline constexpr std::size_t maximumPatternEvents=4096;
struct PatternEvent {
    int sampleOffset=0;
    bool noteOn=true;
    std::uint8_t pitch=60, velocity=100, channel=1;
    bool operator==(const PatternEvent&) const = default;
};
struct PatternEventBuffer {
    std::array<PatternEvent, maximumPatternEvents> events {};
    std::size_t count=0;
    bool drumStopAtStart=false;
};
enum class PatternPlaybackStatus : std::uint8_t {
    externalMidi, playing, stopped, missingTiming, invalidTiming, eventCapacity, unprepared, invalidBlock
};
// Render-owned, fixed storage. Serialize every call, including reads and publication.
class PatternScheduler {
public:
    bool prepare(double,int) noexcept;
    bool setPattern(const PatternDocument&) noexcept;
    const PatternDocument& pattern() const noexcept;
    PatternPlaybackStatus processBlock(const HostPatternTiming&,int,PlaybackMode,PatternEventBuffer&) noexcept;
    void release(PatternEventBuffer&) noexcept;
    std::size_t heldNoteCount() const noexcept;
private:
    bool append(PatternEventBuffer&,int,bool,std::size_t) noexcept;
    bool releaseKeys(PatternEventBuffer&,int) noexcept;
    bool schedule(const HostPatternTiming&,int,PatternEventBuffer&) noexcept;
    bool drumClockContinues(const HostPatternTiming&,double,double) const noexcept;
    PatternDocument document {};
    PatternPlaybackNotes playback {};
    std::array<bool,maximumPatternNotes> active {};
    std::array<std::uint16_t,16*128> references {};
    double rate=0, samplesPerBeat=0, anchorBeat=0, anchorSample=0, expectedBeat=0;
    std::int64_t cursor=0, phrase=0;
    int maximumBlock=0;
    std::optional<std::int64_t> expectedSample;
    PatternLoopRange loop {};
    bool continuous=false, pendingRelease=false, phraseKnown=false, looping=false;
};
}
