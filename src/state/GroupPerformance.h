#pragma once
#include "GroupContext.h"
#include "ObservedMidi.h"

namespace airack
{
enum class LiveControlReason : std::uint8_t { none,notCaptured,readConflict,invalidRouting,stalePublisher,clockRollback };
struct LiveControlObservation
{
    ObservedMidiAvailability availability=ObservedMidiAvailability::unavailable;
    LiveControlReason reason=LiveControlReason::notCaptured;
    std::uint64_t ageMs=0;
    ParameterValues values{};
    bool operator==(const LiveControlObservation&) const = default;
};
struct PeerPlaybackObservation
{
    LiveControlObservation controls;
    ObservedMidiSnapshot notes;
    bool operator==(const PeerPlaybackObservation&) const = default;
};
struct GroupSelection
{
    std::uint64_t loadRevision=0;
    std::uint8_t peerCount=0;
    std::array<GroupUuid,maximumSelectedGroupPeers> members{};
    bool operator==(const GroupSelection&) const = default;
};
struct GroupPerformanceCapture
{
    GroupContext ownership;
    std::uint64_t requesterEditRevision=0;
    std::uint8_t peerCount=0;
    std::array<PeerPlaybackObservation,maximumSelectedGroupPeers> playback{};
    bool operator==(const GroupPerformanceCapture&) const = default;
};
bool validateGroupSelection(const GroupSelection&) noexcept;
bool validatePeerPlaybackObservation(const PeerPlaybackObservation&,const ProjectDocument&);
bool validateGroupPerformanceCapture(const GroupPerformanceCapture&);
void agePeerPlaybackObservation(PeerPlaybackObservation&,std::uint64_t elapsedMs,bool clockRollback=false) noexcept;
}
