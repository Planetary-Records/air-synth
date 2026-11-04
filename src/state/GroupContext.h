#pragma once
#include "ProjectDocument.h"
#include <vector>

namespace airack
{
inline constexpr std::size_t maximumSelectedGroupPeers = 4;
struct GroupPeerSnapshot
{
    GroupUuid runtime {};
    std::uint64_t loadRevision = 0, editRevision = 0;
    ProjectDocument project {};
    bool operator==(const GroupPeerSnapshot&) const = default;
};
// Owned transient context, never saved admission or a request authentication token.
struct GroupContext
{
    GroupUuid registry {}, requesterRuntime {};
    std::uint64_t loadRevision = 0, revision = 0;
    GroupIntent requester {};
    std::vector<GroupPeerSnapshot> peers;
    bool operator==(const GroupContext&) const = default;
};
}
