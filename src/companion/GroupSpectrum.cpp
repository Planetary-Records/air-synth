#include "GroupSpectrum.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <thread>
namespace airack::companion
{
struct GroupSpectrumInbox::State
{
    const std::thread::id owner=std::this_thread::get_id();
    juce::Uuid session=juce::Uuid::null();GroupUuid requester;std::uint64_t load=0;
    struct Entry {Peer peer;GroupSpectrum value;Time arrival;bool present=false,available=false;};
    std::array<Entry,4> entries{};std::size_t count=0;std::optional<Time> lastTime;
    bool currentThread() const noexcept{return owner==std::this_thread::get_id();}
};
GroupSpectrumInbox::GroupSpectrumInbox():state(std::make_unique<State>()){}
GroupSpectrumInbox::~GroupSpectrumInbox()=default;
bool GroupSpectrumInbox::configure(juce::Uuid session,GroupUuid requester,std::uint64_t load,std::span<const Peer> peers)
{
    auto& s=*state;if(!s.currentThread()||session.isNull()||requester.isNull()||!load||peers.size()>4)return false;
    for(std::size_t i=0;i<peers.size();++i){if(peers[i].runtime.isNull()||peers[i].runtime==requester||!peers[i].loadRevision)return false;for(std::size_t j=0;j<i;++j)if(peers[i].runtime==peers[j].runtime)return false;}
    bool same=s.session==session&&s.requester==requester&&s.load==load&&s.count==peers.size();
    if(same)for(std::size_t i=0;i<peers.size();++i)same=same&&s.entries[i].peer==peers[i];
    if(same)return true;
    clear();s.session=session;s.requester=requester;s.load=load;s.count=peers.size();for(std::size_t i=0;i<peers.size();++i)s.entries[i].peer=peers[i];return true;
}
bool GroupSpectrumInbox::receive(juce::Uuid session,GroupUuid requester,std::uint64_t load,std::span<const GroupSpectrum> values,Time now)
{
    auto& s=*state;if(!s.currentThread()||!s.load||s.session!=session||s.requester!=requester||s.load!=load||values.size()>4||(s.lastTime&&now<*s.lastTime))return false;
    std::array<bool,4> seen{};auto entries=s.entries;for(auto& entry:entries)entry.available=false;
    for(const auto& value:values){
        if(!validateGroupSpectrum(value))return false;
        const auto found=std::find_if(s.entries.begin(),s.entries.begin()+s.count,[&](const auto& e){return e.peer.runtime==value.runtime&&e.peer.loadRevision==value.loadRevision;});
        if(found==s.entries.begin()+s.count)return false;const auto index=std::size_t(found-s.entries.begin());
        if(seen[index]||(found->present&&value.sequence<found->value.sequence))return false;seen[index]=true;
        auto& next=entries[index];
        if(found->present&&value.sequence==found->value.sequence){if(value!=found->value)return false;next.available=found->available;}
        else {next.value=value;next.arrival=now;next.present=next.available=true;}
    }
    s.entries=std::move(entries);s.lastTime=now;return true;
}
const GroupSpectrum* GroupSpectrumInbox::current(GroupUuid runtime,Time now) const noexcept
{
    const auto& s=*state;if(!s.currentThread()||!s.load||(s.lastTime&&now<*s.lastTime))return nullptr;
    for(std::size_t i=0;i<s.count;++i){const auto& entry=s.entries[i];if(entry.peer.runtime==runtime&&entry.present&&entry.available&&now>=entry.arrival&&now-entry.arrival<=std::chrono::seconds(2))return &entry.value;}
    return nullptr;
}
void GroupSpectrumInbox::clear() noexcept
{auto& s=*state;if(!s.currentThread())return;s.session=juce::Uuid::null();s.requester={};s.load=0;s.entries={};s.count=0;s.lastTime.reset();}
namespace {constexpr std::size_t encodedSize=4+16+3*8+1+8+128*2;}
bool validateGroupSpectrum(const GroupSpectrum& value) noexcept
{
    return !value.runtime.isNull()&&value.loadRevision&&value.sequence&&value.generation&&value.slot<8
        &&std::isfinite(value.sampleRate)&&value.sampleRate>=8000&&value.sampleRate<=1536000
        &&std::all_of(value.postDb100.begin(),value.postDb100.end(),[](auto level){return level>=-12000&&level<=2400;});
}
bool encodeGroupSpectrum(const GroupSpectrum& value,juce::MemoryBlock& output)
{
    if(!validateGroupSpectrum(value))return false;
    try {
        juce::MemoryOutputStream stream;
        stream.write("AGS1",4);stream.write(value.runtime.getRawData(),16);
        for(auto counter:{value.loadRevision,value.sequence,value.generation})stream.writeInt64(std::bit_cast<juce::int64>(counter));
        stream.writeByte(char(value.slot));stream.writeDouble(value.sampleRate);
        for(auto level:value.postDb100)stream.writeShort(level);
        if(stream.getDataSize()!=encodedSize)return false;
        juce::MemoryBlock wire(stream.getData(),stream.getDataSize());output.swapWith(wire);return true;
    }catch(const std::bad_alloc&){return false;}
}
bool decodeGroupSpectrum(const void* data,std::size_t size,GroupSpectrum& output)
{
    if(!data||size!=encodedSize||size>maximumSpectrumPayloadBytes||std::memcmp(data,"AGS1",4)!=0)return false;
    juce::MemoryInputStream stream(data,size,false);stream.setPosition(4);GroupSpectrum value;
    if(stream.read(value.runtime.bytes.data(),16)!=16)return false;
    value.loadRevision=std::bit_cast<std::uint64_t>(stream.readInt64());value.sequence=std::bit_cast<std::uint64_t>(stream.readInt64());value.generation=std::bit_cast<std::uint64_t>(stream.readInt64());
    value.slot=std::uint8_t(stream.readByte());value.sampleRate=stream.readDouble();for(auto& level:value.postDb100)level=stream.readShort();
    if(!validateGroupSpectrum(value)||!stream.isExhausted())return false;
    output=value;return true;
}
}
