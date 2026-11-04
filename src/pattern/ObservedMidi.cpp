#include "ObservedMidi.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace airack
{
bool validateObservedMidiSnapshot(const ObservedMidiSnapshot& s) noexcept
{
    if(s.availability>ObservedMidiAvailability::stale||s.reason>ObservedMidiReason::clockRollback||s.noteCount>maximumObservedMidiNotes
        ||s.meter.has_value()!=s.bpm.has_value())return false;
    if(s.meter)
    {
        if(!validatePhraseMeter(*s.meter)||!std::isfinite(*s.bpm)||*s.bpm<20||*s.bpm>400
            ||s.windowBeats!=observedMidiBars*s.meter->numerator*4.0/s.meter->denominator)return false;
    }
    else if(s.windowBeats!=0||s.noteCount!=0)return false;
    if(s.availability==ObservedMidiAvailability::fresh)
    {if(!s.noteCount||s.reason!=ObservedMidiReason::none||s.ageMs>observedMidiFreshnessMs)return false;}
    else if(s.availability==ObservedMidiAvailability::stale)
    {if(!s.noteCount||(s.reason!=ObservedMidiReason::stopped&&s.reason!=ObservedMidiReason::stalePublisher&&s.reason!=ObservedMidiReason::clockRollback))return false;}
    else if(s.noteCount||s.reason==ObservedMidiReason::none)return false;
    for(std::size_t i=0;i<maximumObservedMidiNotes;++i)
    {
        const auto& n=s.notes[i];
        if(i>=s.noteCount){if(n!=ObservedMidiNote{})return false;continue;}
        if(!std::isfinite(n.startBeats)||!std::isfinite(n.durationBeats)||n.startBeats<0||n.startBeats>s.windowBeats
            ||n.durationBeats<0||n.durationBeats>s.windowBeats-n.startBeats
            ||n.pitch>127||n.velocity<1||n.velocity>127||n.channel<1||n.channel>16)return false;
    }
    return true;
}
void ObservedMidi::publish(const ObservedMidiSnapshot& s,std::uint64_t timestamp) noexcept
{
    slots[back]={s,timestamp};publishedTimestamp=timestamp;
    const auto previous=middle.exchange(static_cast<std::uint8_t>(back|dirty),std::memory_order_acq_rel)&indexMask;
    if(previous<slots.size())back=previous;
}
void ObservedMidi::publishEmpty(ObservedMidiReason reason,std::uint64_t timestamp) noexcept
{ObservedMidiSnapshot s;s.reason=reason;publish(s,timestamp);}
void ObservedMidi::resetHistory() noexcept
{first=0;count=0;keys.fill(-1);truncated=false;continuous=false;lastWindow={};}
bool ObservedMidi::prepare(double sampleRate) noexcept
{
    if(!std::isfinite(sampleRate)||sampleRate<8000||sampleRate>384000){release();return false;}
    rate=sampleRate;inBlock=false;resetHistory();publishEmpty(ObservedMidiReason::noNotes,publishedTimestamp);return true;
}
void ObservedMidi::release() noexcept
{rate=0;inBlock=false;resetHistory();publishEmpty(ObservedMidiReason::unprepared,publishedTimestamp);}
bool ObservedMidi::beginBlock(const HostPatternTiming& host,std::optional<PhraseMeter> meter,int size,PlaybackMode mode,std::uint64_t nowMs,bool hostPositionAvailable) noexcept
{
    if(inBlock){resetHistory();publishEmpty(ObservedMidiReason::invalidBlock,nowMs);}
    const bool rollback=clockKnown&&nowMs<previousNow;previousNow=nowMs;clockKnown=true;
    inBlock=true;accepting=false;stopped=false;eventCalls=0;blockTimestamp=nowMs;frames=size;
    const auto reject=[&](ObservedMidiReason reason){resetHistory();publishEmpty(reason,nowMs);return false;};
    if(rate==0)return reject(ObservedMidiReason::unprepared);
    if(size<1||size>65536)return reject(ObservedMidiReason::invalidBlock);
    if(mode==PlaybackMode::internalPattern)return reject(ObservedMidiReason::internalMode);
    if(mode!=PlaybackMode::externalMidi)return reject(ObservedMidiReason::invalidBlock);
    if(!hostPositionAvailable)return reject(ObservedMidiReason::missingTiming);
    if(!host.playing){stopped=true;return false;}
    if(!host.bpm||!host.ppqPosition||!meter)return reject(ObservedMidiReason::missingTiming);
    if(!std::isfinite(*host.bpm)||*host.bpm<20||*host.bpm>400||!std::isfinite(*host.ppqPosition)
        ||std::abs(*host.ppqPosition)>16777216||!validatePhraseMeter(*meter)
        ||(host.samplePosition&&*host.samplePosition>std::numeric_limits<std::int64_t>::max()-size))return reject(ObservedMidiReason::invalidTiming);
    const auto tolerance=1.5*std::max(previousBpm,*host.bpm)/(60*rate);
    if(!continuous||rollback||*meter!=previousMeter||std::abs(*host.ppqPosition-expectedBeat)>tolerance
        ||(host.samplePosition&&expectedSample&&*host.samplePosition!=*expectedSample))resetHistory();
    origin=*host.ppqPosition;beatPerSample=*host.bpm/(60*rate);blockMeter=*meter;blockSample=host.samplePosition;
    previousBpm=*host.bpm;accepting=true;return true;
}
void ObservedMidi::closeKey(std::size_t key,double at) noexcept
{
    if(keys[key]<0)return;auto& entry=history[keys[key]];
    entry.end=std::max(entry.start,at);entry.held=false;keys[key]=-1;
}
void ObservedMidi::event(std::span<const std::uint8_t> raw,int offset) noexcept
{
    if(!inBlock)return;
    if(eventCalls<=maximumObservedMidiEvents)++eventCalls;
    if(!accepting)return;
    if(eventCalls>maximumObservedMidiEvents){accepting=false;resetHistory();publishEmpty(ObservedMidiReason::inputCapacity,blockTimestamp);return;}
    if(offset<0||offset>=frames||raw.empty())return;
    const auto at=origin+offset*beatPerSample;
    if(raw[0]==0xfc&&raw.size()==1){for(std::size_t key=0;key<keys.size();++key)closeKey(key,at);return;}
    if(raw.size()!=3||raw[1]>127||raw[2]>127)return;
    const auto kind=raw[0]&0xf0;const auto channel=raw[0]&15;
    if(kind==0xb0&&(raw[1]==120||raw[1]==123))
    {for(std::size_t pitch=0;pitch<128;++pitch)closeKey(channel*128+pitch,at);return;}
    if(kind!=0x80&&kind!=0x90)return;
    const auto key=channel*128+raw[1];closeKey(key,at);
    if(kind==0x80||raw[2]==0)return;
    if(count==maximumObservedMidiNotes)
    {
        const auto& old=history[first];const auto oldKey=(old.channel-1)*128+old.pitch;
        if(keys[oldKey]==static_cast<std::int16_t>(first))keys[oldKey]=-1;
        first=(first+1)%maximumObservedMidiNotes;--count;truncated=true;
    }
    const auto index=(first+count)%maximumObservedMidiNotes;
    history[index]={at,at,raw[1],raw[2],static_cast<std::uint8_t>(channel+1),true};keys[key]=static_cast<std::int16_t>(index);++count;
}
void ObservedMidi::endBlock(bool inputAccepted) noexcept
{
    if(!inBlock){resetHistory();publishEmpty(ObservedMidiReason::invalidBlock,publishedTimestamp);return;}
    inBlock=false;
    if(!inputAccepted){resetHistory();publishEmpty(ObservedMidiReason::inputCapacity,blockTimestamp);return;}
    if(stopped)
    {
        for(std::size_t key=0;key<keys.size();++key)closeKey(key,boundary);
        auto s=lastWindow;for(std::size_t i=0;i<s.noteCount;++i)s.notes[i].held=false;
        s.availability=s.noteCount?ObservedMidiAvailability::stale:ObservedMidiAvailability::unavailable;
        s.reason=s.noteCount?ObservedMidiReason::stopped:ObservedMidiReason::noNotes;
        lastWindow=s;publish(s,s.meter?windowTimestamp:publishedTimestamp);return;
    }
    if(!accepting)return;
    boundary=origin+frames*beatPerSample;expectedBeat=boundary;expectedSample=blockSample?std::optional<std::int64_t>(*blockSample+frames):std::nullopt;
    previousMeter=blockMeter;continuous=true;
    ObservedMidiSnapshot s;s.meter=blockMeter;s.bpm=previousBpm;s.windowBeats=observedMidiBars*blockMeter.numerator*4.0/blockMeter.denominator;
    const auto left=boundary-s.windowBeats;std::array<Entry,maximumObservedMidiNotes> retained{};std::size_t kept=0;
    for(std::size_t i=0;i<count;++i)
    {
        const auto& entry=history[(first+i)%maximumObservedMidiNotes];
        if(!entry.held&&entry.end<left)continue;
        retained[kept]=entry;const auto start=std::clamp(entry.start,left,boundary),end=std::clamp(entry.held?boundary:entry.end,start,boundary);
        s.notes[kept]={start-left,std::min(end-start,s.windowBeats-(start-left)),entry.pitch,entry.velocity,entry.channel,entry.held};++kept;
    }
    history=retained;count=kept;first=0;keys.fill(-1);
    for(std::size_t i=0;i<count;++i)if(history[i].held)keys[(history[i].channel-1)*128+history[i].pitch]=static_cast<std::int16_t>(i);
    if(count==0)truncated=false;s.noteCount=static_cast<std::uint16_t>(count);s.truncated=truncated;
    if(count){s.availability=ObservedMidiAvailability::fresh;s.reason=ObservedMidiReason::none;}
    windowTimestamp=blockTimestamp;lastWindow=s;publish(s,blockTimestamp);
}
void ObservedMidi::capture(std::uint64_t nowMs,ObservedMidiSnapshot& output) noexcept
{
    if(middle.load(std::memory_order_acquire)&dirty)
    {
        const auto newest=middle.exchange(front,std::memory_order_acq_rel)&indexMask;
        if(newest<slots.size())front=newest;
    }
    output=slots[front].snapshot;const auto timestamp=slots[front].timestamp;
    output.ageMs=nowMs<timestamp?std::numeric_limits<std::uint64_t>::max():nowMs-timestamp;
    if(output.noteCount)
    {
        if(nowMs<timestamp){output.availability=ObservedMidiAvailability::stale;output.reason=ObservedMidiReason::clockRollback;}
        else if(output.availability==ObservedMidiAvailability::fresh&&output.ageMs>observedMidiFreshnessMs)
        {output.availability=ObservedMidiAvailability::stale;output.reason=ObservedMidiReason::stalePublisher;}
    }
}
}
