#include "PerformanceMidi.h"
#include <algorithm>
namespace airack {
static_assert(std::atomic<PerformanceInputStatus>::is_always_lock_free);
static_assert(std::atomic<DrumPerformanceStatus>::is_always_lock_free);
bool PerformanceMidi::enqueueDrum(const DrumPadPress& press) noexcept {
    int first=0,count=0,second=0,secondCount=0;drumFifo.prepareToWrite(1,first,count,second,secondCount);
    if(!count){drumFeedback.store(DrumPerformanceStatus::queueFull);return false;}
    drumQueue[static_cast<std::size_t>(first)]=press;drumFeedback.store(DrumPerformanceStatus::ready);drumFifo.finishedWrite(1);return true;
}
void PerformanceMidi::reportDrumStatus(DrumPerformanceStatus status) noexcept {
    auto previous=DrumPerformanceStatus::ready;drumFeedback.compare_exchange_strong(previous,status);
}
void PerformanceMidi::beginDrumCallback() noexcept {
    auto previous=DrumPerformanceStatus::deferred;drumFeedback.compare_exchange_strong(previous,DrumPerformanceStatus::ready);
    drumCount=0;int first=0,count=0,second=0,secondCount=0;drumFifo.prepareToRead(64,first,count,second,secondCount);
    const auto copy=[&](int start,int length){for(int n=0;n<length;++n){const auto slot=static_cast<std::size_t>(start+n);drumSlots[drumCount]=static_cast<std::uint8_t>(slot);drumCutoff[drumCount++]=drumQueue[slot];}};
    copy(first,count);copy(second,secondCount);
}
void PerformanceMidi::deferDrumCallback() noexcept {
    for(std::size_t n=0;n<drumCount;++n)if(drumCutoff[n].hostOffset)drumExpired.set(drumSlots[n]);
    if(drumCount)reportDrumStatus(DrumPerformanceStatus::deferred);
}
void PerformanceMidi::consumeDrumCallback() noexcept {
    for(std::size_t n=0;n<drumCount;++n)drumExpired.reset(drumSlots[n]);
    drumFifo.finishedRead(static_cast<int>(drumCount));drumCount=0;
}
bool PerformanceMidi::enqueue(bool on,std::uint8_t pitch,std::uint8_t velocity,std::uint8_t channel) noexcept {
    if(pitch>127||velocity>127||channel<1||channel>16)return false;
    int start=0,count=0,second=0,secondCount=0;fifo.prepareToWrite(1,start,count,second,secondCount);
    if(count==0){generation.fetch_add(1);feedback.store(PerformanceInputStatus::keyboardOverflow);return false;}
    queue[static_cast<std::size_t>(start)]={generation.load(),on&&velocity!=0,pitch,velocity,channel};fifo.finishedWrite(1);
    if(on&&velocity!=0)feedback.store(PerformanceInputStatus::ready);
    return true;
}
void PerformanceMidi::cancelKeyboard() noexcept {generation.fetch_add(1);}
void PerformanceMidi::prepare(){output.ensureSize(capacity*9+64);beginDrumCallback();if(drumCount)reportDrumStatus(DrumPerformanceStatus::staleTarget);consumeDrumCallback();clearOwners();cancelDeferredReset();}
void PerformanceMidi::clearDeferredNotes() noexcept {for(auto& source:deferredNotes)source.fill(0);}
void PerformanceMidi::clearOwners(bool preserveDeferred) noexcept {for(auto& source:owners)source.fill(false);if(!preserveDeferred&&deferredReset)clearDeferredNotes();}
void PerformanceMidi::deferReset(std::uint64_t token,bool externalMidi) noexcept {
    if(!deferredReset||deferredToken!=token)clearDeferredNotes();
    else if(deferredExternal&&!externalMidi)deferredNotes[0].fill(0);
    deferredToken=token;deferredReset=true;deferredExternal=externalMidi;
}
void PerformanceMidi::cancelDeferredReset() noexcept {if(deferredReset){deferredReset=false;clearDeferredNotes();}}
void PerformanceMidi::stageDeferredReset(std::uint64_t token,bool includeHost) noexcept {
    if(deferredReset&&deferredToken==token)for(std::size_t source=0;source<deferredNotes.size();++source)
        if((source!=0||includeHost)&&(source!=1||!keyboardRelease))for(std::size_t key=0;key<deferredNotes[source].size();++key)
            if(const auto velocity=deferredNotes[source][key]){
                const juce::uint8 raw[]={static_cast<juce::uint8>(0x90+key/128),static_cast<juce::uint8>(key%128),velocity};
                stage(source==0?Source::host:Source::keyboard,0,1,raw,3);
                if(!overflow)staged[stagedCount-1].kind=Kind::replayedMidi;
            }
    cancelDeferredReset();
}
void PerformanceMidi::beginCallback() noexcept {
    // Never overwrite a concurrent producer overflow while clearing last
    // callback's capacity warning. Only an accepted new note clears overflow.
    auto previous=PerformanceInputStatus::midiCapacity;
    feedback.compare_exchange_strong(previous,PerformanceInputStatus::ready);
    const auto current=generation.load();keyboardRelease=current!=observedGeneration;observedGeneration=current;keyboardCount=0;
    int first=0,count=0,second=0,secondCount=0;fifo.prepareToRead(256,first,count,second,secondCount);
    const auto copy=[&](int start,int length){for(int i=0;i<length;++i){const auto e=queue[static_cast<std::size_t>(start+i)];if(e.generation==current)keyboard[keyboardCount++]=e;}};
    copy(first,count);copy(second,secondCount);fifo.finishedRead(count+secondCount);
}
void PerformanceMidi::beginChunk() noexcept {stagedCount=0;emitted=0;overflow=false;output.clear();drumOutputCount=0;drumStopRequested=false;drumSuppressed=false;drumEmergency=false;}
void PerformanceMidi::stage(Source source,int offset,std::uint8_t phase,const juce::uint8* data,std::uint8_t size,bool deferredOnly) noexcept {
    if(stagedCount==capacity){overflow=true;return;}
    auto& e=staged[stagedCount];e={};e.offset=offset;e.ordinal=static_cast<std::uint32_t>(stagedCount);e.source=source;e.phase=phase;e.size=size;e.deferredOnly=deferredOnly;std::copy_n(data,size,e.data.begin());++stagedCount;
}
void PerformanceMidi::stageExactDrum(const DrumPadPress& press,int offset) noexcept {
    const juce::uint8 raw[]{static_cast<juce::uint8>(0x90+press.channel-1),press.pad,press.velocity};stage(Source::keyboard,offset,3,raw,3);
    if(!overflow){auto& e=staged[stagedCount-1];e.kind=Kind::exactPad;e.token=press.token;e.bank=press.bank;}
}
void PerformanceMidi::requestDrumStop() noexcept {
    if(drumStopRequested)return;drumStopRequested=true;
    const juce::uint8 raw[]{0};stage(Source::pattern,0,0,raw,1);if(!overflow)staged[stagedCount-1].kind=Kind::drumStop;
}
void PerformanceMidi::stageKeyboard() noexcept {
    if(keyboardRelease)releaseSource(Source::keyboard);
    for(std::size_t n=0;n<keyboardCount;++n){const auto& e=keyboard[n];const juce::uint8 raw[]={static_cast<juce::uint8>((e.on?0x90:0x80)+(e.channel-1)),e.pitch,e.velocity};stage(Source::keyboard,0,3,raw,3);}
    keyboardCount=0;keyboardRelease=false;
}
void PerformanceMidi::releaseSource(Source source) noexcept {
    const auto& mask=owners[static_cast<std::size_t>(source)];
    for(std::size_t k=0;k<mask.size();++k)if(mask[k]){const juce::uint8 raw[]={static_cast<juce::uint8>(0x80+k/128),static_cast<juce::uint8>(k%128),0};stage(source,0,0,raw,3);}
}
void PerformanceMidi::releaseAll() noexcept {releaseSource(Source::pattern);releaseSource(Source::host);releaseSource(Source::keyboard);}
void PerformanceMidi::stagePattern(const PatternEventBuffer& events,std::uint8_t phase) noexcept {
    for(std::size_t n=0;n<events.count;++n){const auto& e=events.events[n];const juce::uint8 raw[]={static_cast<juce::uint8>((e.noteOn?0x90:0x80)+e.channel-1),e.pitch,e.velocity};stage(Source::pattern,e.sampleOffset,phase,raw,3);}
}
void PerformanceMidi::stageHost(const juce::MidiMessageMetadata& e,int offset,bool deferredOnly) noexcept {
    const bool drumControl=e.numBytes==3&&(e.data[0]&0xf0)==0xb0&&(e.data[1]==120||e.data[1]==123)&&e.data[2]<=127;
    if(drumControl&&deferredOnly&&(!deferredReset||!deferredExternal)){
        stage(Source::host,offset,2,e.data,3);if(!overflow)staged[stagedCount-1].kind=Kind::drumOnlyMidi;return;
    }
    if(deferredOnly&&(!deferredReset||!deferredExternal))return;
    if(e.numBytes==1&&e.data[0]==0xfc){stage(Source::host,offset,2,e.data,1,deferredOnly);return;}
    if(e.numBytes!=3||e.data[1]>127||e.data[2]>127)return;
    const auto status=e.data[0]&0xf0;
    if(status==0x80||status==0x90||(status==0xb0&&(e.data[1]==120||e.data[1]==123)))stage(Source::host,offset,2,e.data,3,deferredOnly);
}
bool PerformanceMidi::emit(const juce::uint8* data,int size,int offset) noexcept {
    if(emitted==capacity)return false;
    output.addEvent(data,size,offset);++emitted;return true;
}
bool PerformanceMidi::off(Source source,std::size_t key,int offset) noexcept {
    if(deferredReset&&source!=Source::pattern)deferredNotes[static_cast<std::size_t>(source)-1][key]=0;
    auto& held=owners[static_cast<std::size_t>(source)][key];if(!held)return true;held=false;
    if(owners[0][key]||owners[1][key]||owners[2][key])return true;
    const juce::uint8 raw[]={static_cast<juce::uint8>(0x80+key/128),static_cast<juce::uint8>(key%128),0};return emit(raw,3,offset);
}
bool PerformanceMidi::apply(const Event& e) noexcept {
    if(e.kind==Kind::exactPad||e.kind==Kind::drumStop||e.kind==Kind::drumOnlyMidi)return true;
    if(e.deferredOnly){
        if(e.data[0]==0xfc){clearDeferredNotes();return true;}
        const auto status=e.data[0]&0xf0;const auto channel=e.data[0]&15;const auto key=static_cast<std::size_t>(channel*128+e.data[1]);
        if(status==0x90&&e.data[2]!=0)deferredNotes[0][key]=e.data[2];
        else if(status==0x80||status==0x90)deferredNotes[0][key]=0;
        else if(e.data[1]==123)std::fill(deferredNotes[0].begin()+channel*128,deferredNotes[0].begin()+(channel+1)*128,0);
        else for(auto& notes:deferredNotes)std::fill(notes.begin()+channel*128,notes.begin()+(channel+1)*128,0);
        return true;
    }
    if(e.data[0]==0xfc){clearOwners();return emit(e.data.data(),1,e.offset);}
    const auto status=e.data[0]&0xf0;const auto channel=e.data[0]&15;const auto key=static_cast<std::size_t>(channel*128+e.data[1]);
    if(status==0x90&&e.data[2]!=0){owners[static_cast<std::size_t>(e.source)][key]=true;if(deferredReset&&e.source!=Source::pattern&&(e.source!=Source::host||deferredExternal))deferredNotes[static_cast<std::size_t>(e.source)-1][key]=e.data[2];return emit(e.data.data(),3,e.offset);}
    if(status==0x80||status==0x90)return off(e.source,key,e.offset);
    if(e.data[1]==123){for(std::size_t k=channel*128;k<(channel+1u)*128;++k)if(!off(Source::host,k,e.offset))return false;return true;}
    // CC120 retains the engine's channel kill, including keyboard ownership.
    for(auto& mask:owners)std::fill(mask.begin()+channel*128,mask.begin()+(channel+1)*128,false);
    if(deferredReset)for(auto& notes:deferredNotes)std::fill(notes.begin()+channel*128,notes.begin()+(channel+1)*128,0);
    return emit(e.data.data(),3,e.offset);
}
void PerformanceMidi::projectDrum(const Event& e) noexcept {
    using drums::RenderEventKind;
    drums::RenderEvent out;out.hostOffset=e.offset;
    if(e.kind==Kind::exactPad){out.kind=RenderEventKind::exactPad;out.token=e.token;out.bank=e.bank;out.padOrNote=e.data[1];out.velocity=e.data[2];out.channel=(e.data[0]&15)+1;}
    else if(e.kind==Kind::drumStop)out.kind=RenderEventKind::stopAll;
    else if(e.kind==Kind::replayedMidi)return;
    else if(e.size==3&&(e.data[0]&0xf0)==0xb0&&(e.data[1]==120||e.data[1]==123)){out.kind=RenderEventKind::stopChannel;out.channel=(e.data[0]&15)+1;}
    else if(e.deferredOnly)return;
    else if(e.size==1&&e.data[0]==0xfc)out.kind=RenderEventKind::stopAll;
    else if(e.size==3&&(e.data[0]&0xf0)==0x90&&e.data[2]){out.kind=RenderEventKind::noteOn;out.padOrNote=e.data[1];out.velocity=e.data[2];out.channel=(e.data[0]&15)+1;}
    else return;
    drumOutput[drumOutputCount++]=out;
}
void PerformanceMidi::emergencyDrumStop() noexcept {
    drumOutputCount=1;drumOutput[0]={};drumOutput[0].kind=drums::RenderEventKind::stopAll;reportDrumStatus(DrumPerformanceStatus::eventCapacity);
}
void PerformanceMidi::capacityFailure() noexcept {
    emergencyDrumStop();
    output.clear();emitted=0;
    for(std::size_t k=0;k<16*128;++k)if(owners[0][k]||owners[1][k]||owners[2][k]){const juce::uint8 raw[]={static_cast<juce::uint8>(0x80+k/128),static_cast<juce::uint8>(k%128),0};emit(raw,3,0);}
    clearOwners();
    auto previous=PerformanceInputStatus::ready;
    feedback.compare_exchange_strong(previous,PerformanceInputStatus::midiCapacity);
}
bool PerformanceMidi::commit() noexcept {
    if(overflow){capacityFailure();return false;}
    std::sort(staged.begin(),staged.begin()+static_cast<std::ptrdiff_t>(stagedCount),[](const Event& a,const Event& b){if(a.offset!=b.offset)return a.offset<b.offset;if(a.phase!=b.phase)return a.phase<b.phase;return a.ordinal<b.ordinal;});
    const auto before=owners;
    for(std::size_t i=0;i<stagedCount;++i){if(!drumSuppressed)projectDrum(staged[i]);if(!apply(staged[i])){owners=before;capacityFailure();return false;}}
    if(drumEmergency)emergencyDrumStop();
    return true;
}
}
