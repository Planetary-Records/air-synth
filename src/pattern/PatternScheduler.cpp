#include "PatternScheduler.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace airack {
namespace {
constexpr double maximumBeat=16777216.;
bool validBeat(double value) noexcept { return std::isfinite(value)&&std::abs(value)<=maximumBeat; }
std::int64_t rounded(double value) noexcept { return static_cast<std::int64_t>(std::floor(value+.5)); }
std::size_t key(const PatternPlaybackNote& n) noexcept { return (n.channel-1)*128u+n.pitch; }
}
bool PatternScheduler::prepare(double sampleRate,int block) noexcept {
    if (!std::isfinite(sampleRate)||sampleRate<8000||sampleRate>384000||block<1||block>65536) return false;
    rate=sampleRate; maximumBlock=block; pendingRelease=true; continuous=false;
    return true;
}
bool PatternScheduler::setPattern(const PatternDocument& candidate) noexcept {
    PatternPlaybackNotes compiled;
    if (!makePatternPlaybackNotes(candidate,compiled)) return false;
    if (candidate!=document) {
        document=candidate; playback=compiled; pendingRelease=true; continuous=false;
    }
    return true;
}
const PatternDocument& PatternScheduler::pattern() const noexcept { return document; }
std::size_t PatternScheduler::heldNoteCount() const noexcept {
    return static_cast<std::size_t>(std::count(active.begin(),active.end(),true));
}
bool PatternScheduler::append(PatternEventBuffer& output,int offset,bool on,std::size_t index) noexcept {
    const auto& note=playback.notes[index];
    auto& refs=references[key(note)];
    if (on) {
        if (active[index]) return true;
        if (output.count==maximumPatternEvents) return false;
        active[index]=true; ++refs;
        output.events[output.count++]={offset,true,note.pitch,note.velocity,note.channel};
    } else if (active[index]) {
        active[index]=false; --refs;
        if (refs==0) {
            if (output.count==maximumPatternEvents) return false;
            output.events[output.count++]={offset,false,note.pitch,0,note.channel};
        }
    }
    return true;
}
bool PatternScheduler::releaseKeys(PatternEventBuffer& output,int offset) noexcept {
    for (std::size_t i=0;i<references.size();++i) if (references[i]!=0) {
        if (output.count==maximumPatternEvents) return false;
        output.events[output.count++]={offset,false,static_cast<std::uint8_t>(i%128),0,static_cast<std::uint8_t>(i/128+1)};
        references[i]=0;
    }
    active.fill(false); return true;
}
void PatternScheduler::release(PatternEventBuffer& output) noexcept {
    output.count=0;output.drumStopAtStart=continuous; releaseKeys(output,0);
    continuous=false; phraseKnown=false; pendingRelease=false; expectedSample.reset();
}
PatternPlaybackStatus PatternScheduler::processBlock(const HostPatternTiming& host,int frames,PlaybackMode mode,PatternEventBuffer& output) noexcept {
    output.drumStopAtStart=false;
    if (frames<1||frames>65536||(maximumBlock!=0&&frames>maximumBlock)) return PatternPlaybackStatus::invalidBlock;
    const auto pause=[&](PatternPlaybackStatus status) { release(output); return status; };
    if (maximumBlock==0) return pause(PatternPlaybackStatus::unprepared);
    if (mode==PlaybackMode::externalMidi) return pause(PatternPlaybackStatus::externalMidi);
    if (mode!=PlaybackMode::internalPattern) return pause(PatternPlaybackStatus::invalidTiming);
    if (!host.playing) return pause(PatternPlaybackStatus::stopped);
    if (!host.bpm||!host.ppqPosition||(host.looping&&!host.loop)) return pause(PatternPlaybackStatus::missingTiming);
    if (!std::isfinite(*host.bpm)||*host.bpm<20||*host.bpm>400||!validBeat(*host.ppqPosition)) return pause(PatternPlaybackStatus::invalidTiming);
    const auto spb=60*rate/ *host.bpm;
    if (host.looping && (!validBeat(host.loop->startBeats)||!validBeat(host.loop->endBeats)
        ||host.loop->endBeats<=host.loop->startBeats||(host.loop->endBeats-host.loop->startBeats)*spb<1)) return pause(PatternPlaybackStatus::invalidTiming);
    // Transactional bounded render: no speculative notes or clock survive overflow.
    const auto before=*this;
    output.count=0;
    if (!schedule(host,frames,output)) {
        *this=before; release(output);output.drumStopAtStart=true; return PatternPlaybackStatus::eventCapacity;
    }
    return PatternPlaybackStatus::playing;
}
bool PatternScheduler::schedule(const HostPatternTiming& host,int frames,PatternEventBuffer& output) noexcept {
    const double spb=60*rate/ *host.bpm;
    auto ppq=*host.ppqPosition;
    const auto hostLoop=host.loop.value_or(PatternLoopRange{});
    if (host.looping&&ppq>=hostLoop.endBeats)
        ppq=hostLoop.startBeats+std::fmod(ppq-hostLoop.startBeats,hostLoop.endBeats-hostLoop.startBeats);
    const bool sameLoop=looping==host.looping && (!looping || (loop.startBeats==hostLoop.startBeats&&loop.endBeats==hostLoop.endBeats));
    output.drumStopAtStart|=continuous&&!drumClockContinues(host,ppq,spb);
    bool entry=!continuous||pendingRelease||!sameLoop||std::abs(ppq-expectedBeat)*spb>.25
        ||(host.samplePosition&&expectedSample&&*host.samplePosition!=*expectedSample);
    if (entry) {
        if (!releaseKeys(output,0)) return false;
        phraseKnown=false;
    }
    // Integer elapsed samples plus one anchor prevents per-buffer PPQ rounding.
    // Rare large elapsed counts reanchor before double/int arithmetic loses precision.
    if (entry||spb!=samplesPerBeat||cursor>(std::int64_t{1}<<40)) {
        anchorBeat=ppq; anchorSample=0; cursor=0;
    }
    samplesPerBeat=spb; looping=host.looping; loop=hostLoop; pendingRelease=false; continuous=true;
    const auto blockStart=cursor, blockEnd=cursor+frames;
    const auto length=playback.lengthBeats;
    const auto at=[&](double beat) { return anchorSample+(beat-anchorBeat)*spb; };
    struct Edge { std::int64_t time; std::uint16_t index; bool on; };
    std::array<Edge,maximumPatternNotes*2> edges;
    while (cursor<blockEnd) {
        if (looping&&rounded(at(loop.endBeats))<=cursor) {
            const auto boundary=at(loop.endBeats);
            if (!releaseKeys(output,static_cast<int>(cursor-blockStart))) return false;
            anchorSample=boundary; anchorBeat=loop.startBeats; phraseKnown=false; entry=true;
        }
        const double beat=anchorBeat+(static_cast<double>(cursor)-anchorSample)/spb;
        auto currentPhrase=static_cast<std::int64_t>(std::floor(beat/length));
        // Sample-domain ownership also handles a rounded boundary just before/after its PPQ.
        if (rounded(at((currentPhrase+1)*length))<=cursor) ++currentPhrase;
        else if (rounded(at(currentPhrase*length))>cursor) --currentPhrase;
        if (phraseKnown&&phrase!=currentPhrase) {
            if (!releaseKeys(output,static_cast<int>(cursor-blockStart))) return false;
            entry=true;
        }
        phrase=currentPhrase; phraseKnown=true;
        const double phraseBeat=phrase*length;
        const auto phraseStart=rounded(at(phraseBeat)), phraseEnd=rounded(at(phraseBeat+length));
        auto end=std::min(blockEnd,phraseEnd);
        if (looping) end=std::min(end,rounded(at(loop.endBeats)));
        std::size_t count=0;
        for (std::uint16_t i=0;i<playback.noteCount;++i) {
            const auto& note=playback.notes[i];
            const auto on=std::clamp(rounded(at(phraseBeat+note.startBeats)),phraseStart,phraseEnd-1);
            const auto off=std::clamp(rounded(at(phraseBeat+note.endBeats)),on+1,phraseEnd);
            if (active[i]&&off<=cursor) edges[count++]={cursor,i,false};
            else if (off>=cursor&&off<end) edges[count++]={off,i,false};
            if (on>=cursor&&on<end) edges[count++]={on,i,true};
            else if (entry&&on<cursor&&off>cursor) edges[count++]={cursor,i,true};
        }
        std::sort(edges.begin(),edges.begin()+count,[](const Edge& a,const Edge& b) {
            if (a.time!=b.time) return a.time<b.time;
            if (a.on!=b.on) return !a.on;
            return a.index<b.index;
        });
        for (std::size_t i=0;i<count;++i)
            if (!append(output,static_cast<int>(edges[i].time-blockStart),edges[i].on,edges[i].index)) return false;
        cursor=end; entry=false;
    }
    expectedBeat=anchorBeat+(static_cast<double>(cursor)-anchorSample)/spb;
    if (looping&&expectedBeat>=loop.endBeats)
        expectedBeat=loop.startBeats+std::fmod(expectedBeat-loop.startBeats,loop.endBeats-loop.startBeats);
    expectedSample.reset();
    if (host.samplePosition&&*host.samplePosition<=std::numeric_limits<std::int64_t>::max()-frames)
        expectedSample=*host.samplePosition+frames;
    return true;
}
bool PatternScheduler::drumClockContinues(const HostPatternTiming& host,double ppq,double currentSpb) const noexcept
{
    const auto nextLoop=host.loop.value_or(PatternLoopRange{});
    const bool sameLoop=looping==host.looping&&(!looping
        ||(loop.startBeats==nextLoop.startBeats&&loop.endBeats==nextLoop.endBeats));
    if(!continuous||pendingRelease||!sameLoop)return false;
    double beatDistance=std::abs(ppq-expectedBeat);
    const bool phasesInLoop=host.looping&&ppq>=loop.startBeats&&ppq<loop.endBeats
        &&expectedBeat>=loop.startBeats&&expectedBeat<loop.endBeats;
    if(phasesInLoop){const double length=loop.endBeats-loop.startBeats;beatDistance=std::min(beatDistance,length-beatDistance);}
    if(beatDistance*currentSpb>.25)return false;
    if(!host.samplePosition||!expectedSample)return true;
    const auto current=*host.samplePosition,expected=*expectedSample;
    if(current==expected)return true;
    if(!phasesInLoop||current>expected)return false;
    const double loopFrames=(loop.endBeats-loop.startBeats)*samplesPerBeat;
    if(!std::isfinite(loopFrames)||loopFrames<1.)return false;
    const auto distance=static_cast<std::uint64_t>(expected)-static_cast<std::uint64_t>(current);
    const auto largestProvableDistance=static_cast<std::uint64_t>(std::ceil(loopFrames))+65536u;
    if(distance>largestProvableDistance)return false;
    const double wraps=std::floor(static_cast<double>(distance)/loopFrames+.5);
    if(wraps<1.||wraps>65537.)return false;
    const double accumulatedSpan=wraps*loopFrames;
    const auto lowerDistance=static_cast<std::uint64_t>(std::floor(accumulatedSpan));
    const auto upperDistance=static_cast<std::uint64_t>(std::ceil(accumulatedSpan));
    return distance==lowerDistance||distance==upperDistance;
}
}
