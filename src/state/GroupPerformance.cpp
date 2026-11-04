#include "GroupPerformance.h"
#include <bit>
#include <limits>
namespace airack
{
namespace
{
bool conflict(const GroupIntent& a,const GroupIntent& b) noexcept
{return (a.key.length&&b.key.length&&a.key!=b.key)||(a.scale.length&&b.scale.length&&a.scale!=b.scale)||(a.bpm&&b.bpm&&a.bpm!=b.bpm);}
bool zeroControls(const ParameterValues& values) noexcept
{for(auto v:values)if(std::bit_cast<std::uint32_t>(v)!=0)return false;return true;}
std::uint64_t aged(std::uint64_t age,std::uint64_t elapsed) noexcept
{return elapsed>std::numeric_limits<std::uint64_t>::max()-age?std::numeric_limits<std::uint64_t>::max():age+elapsed;}
}
bool validateGroupSelection(const GroupSelection& s) noexcept
{
    if(!s.loadRevision||s.peerCount>s.members.size())return false;
    for(std::size_t i=0;i<s.members.size();++i)
    {
        if(i>=s.peerCount){if(!s.members[i].isNull())return false;continue;}
        if(s.members[i].isNull())return false;
        for(std::size_t j=0;j<i;++j)if(s.members[i]==s.members[j])return false;
    }
    return true;
}
bool validatePeerPlaybackObservation(const PeerPlaybackObservation& p,const ProjectDocument& committed)
{
    (void)parameterRegistry();
    if(!validateProjectDocument(committed)||!validateObservedMidiSnapshot(p.notes))return false;
    const auto& c=p.controls;
    if(c.availability>ObservedMidiAvailability::stale||c.reason>LiveControlReason::clockRollback)return false;
    if(c.availability==ObservedMidiAvailability::unavailable)
        return (c.reason==LiveControlReason::notCaptured||c.reason==LiveControlReason::readConflict||c.reason==LiveControlReason::invalidRouting)&&zeroControls(c.values);
    if(c.availability==ObservedMidiAvailability::fresh)
    {if(c.reason!=LiveControlReason::none||c.ageMs>observedMidiFreshnessMs)return false;}
    else if(c.reason!=LiveControlReason::stalePublisher&&c.reason!=LiveControlReason::clockRollback)return false;
    auto live=committed.sound;live.values=c.values;return validateSoundDocument(live);
}
bool validateGroupPerformanceCapture(const GroupPerformanceCapture& c)
{
    (void)parameterRegistry();const auto& o=c.ownership;
    if(o.registry.isNull()||o.requesterRuntime.isNull()||!o.loadRevision||!o.revision||!validateGroupIntent(o.requester)
       ||c.peerCount!=o.peers.size()||c.peerCount>c.playback.size())return false;
    for(std::size_t i=0;i<c.playback.size();++i)
    {
        if(i>=c.peerCount)
        {if(c.playback[i]!=PeerPlaybackObservation{}||!zeroControls(c.playback[i].controls.values))return false;continue;}
        const auto& peer=o.peers[i];
        if(peer.runtime.isNull()||peer.runtime==o.requesterRuntime||!peer.loadRevision||!peer.project.group
            ||peer.project.group->group!=o.requester.group||peer.project.group->member==o.requester.member
            ||!validatePeerPlaybackObservation(c.playback[i],peer.project)||conflict(o.requester,*peer.project.group))return false;
        for(std::size_t j=0;j<i;++j)
        {const auto& other=o.peers[j];if(peer.runtime==other.runtime||peer.project.group->member==other.project.group->member||conflict(*peer.project.group,*other.project.group))return false;}
    }
    return true;
}
void agePeerPlaybackObservation(PeerPlaybackObservation& p,std::uint64_t elapsed,bool rollback) noexcept
{
    auto& c=p.controls;auto& n=p.notes;c.ageMs=aged(c.ageMs,elapsed);n.ageMs=aged(n.ageMs,elapsed);
    if(rollback)
    {
        if(c.availability!=ObservedMidiAvailability::unavailable){c.availability=ObservedMidiAvailability::stale;c.reason=LiveControlReason::clockRollback;c.ageMs=std::numeric_limits<std::uint64_t>::max();}
        if(n.noteCount){n.availability=ObservedMidiAvailability::stale;n.reason=ObservedMidiReason::clockRollback;n.ageMs=std::numeric_limits<std::uint64_t>::max();}
    }
    else
    {
        if(c.availability==ObservedMidiAvailability::fresh&&c.ageMs>observedMidiFreshnessMs){c.availability=ObservedMidiAvailability::stale;c.reason=LiveControlReason::stalePublisher;}
        if(n.availability==ObservedMidiAvailability::fresh&&n.ageMs>observedMidiFreshnessMs){n.availability=ObservedMidiAvailability::stale;n.reason=ObservedMidiReason::stalePublisher;}
    }
}
}
