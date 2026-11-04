#pragma once
#include "GroupIntent.h"

namespace airack
{
enum class GroupLinkPhase : std::uint8_t { unlinked, pending, authorized, admitted };
enum class GroupLinkStatus : std::uint8_t { ready, invalidIntent, notLinked, staleReply, revisionExhausted };
struct GroupLinkView
{
    GroupUuid runtime {};
    std::uint64_t loadRevision = 1;
    std::optional<GroupIntent> intent;
    GroupLinkPhase phase = GroupLinkPhase::unlinked;
    bool exhausted = false;
    bool operator==(const GroupLinkView&) const = default;
};
namespace detail { struct GroupLinkStateTestAccess; }
// Control-owned local consent only; the caller authenticates companion replies.
class GroupLinkState
{
public:
    explicit GroupLinkState(GroupUuid runtime);
    GroupLinkState(const GroupLinkState&) = delete;
    GroupLinkState& operator=(const GroupLinkState&) = delete;
    GroupLinkView view() const noexcept;
    bool eligible() const noexcept;
    GroupLinkStatus restore(const std::optional<GroupIntent>&) noexcept;
    GroupLinkStatus link(const GroupIntent&) noexcept;
    GroupLinkStatus resume() noexcept;
    void leave() noexcept;
    void disconnected() noexcept;
    GroupLinkStatus acknowledge(GroupUuid runtime, std::uint64_t loadRevision,
                                const GroupIntent& requested, const GroupIntent& normalized) noexcept;
private:
    GroupLinkView state {};
    bool advance() noexcept;
    friend struct detail::GroupLinkStateTestAccess;
};
namespace detail
{
struct GroupLinkStateTestAccess
{ static void setLoadRevision(GroupLinkState&, std::uint64_t) noexcept; };
}
}
