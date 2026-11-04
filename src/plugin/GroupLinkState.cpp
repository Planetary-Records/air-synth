#include "GroupLinkState.h"
#include <limits>

namespace airack
{
GroupLinkState::GroupLinkState(GroupUuid runtime)
{
    state.runtime = runtime;
    state.exhausted = runtime.isNull();
}
GroupLinkView GroupLinkState::view() const noexcept { return state; }
bool GroupLinkState::eligible() const noexcept
{
    return !state.runtime.isNull() && !state.exhausted && state.intent && validateGroupIntent(*state.intent)
        && (state.phase == GroupLinkPhase::authorized || state.phase == GroupLinkPhase::admitted);
}
bool GroupLinkState::advance() noexcept
{
    if (state.exhausted || state.loadRevision == std::numeric_limits<std::uint64_t>::max())
    {
        state.exhausted = true;
        state.phase = state.intent ? GroupLinkPhase::pending : GroupLinkPhase::unlinked;
        return false;
    }
    ++state.loadRevision;
    return true;
}
GroupLinkStatus GroupLinkState::restore(const std::optional<GroupIntent>& intent) noexcept
{
    if (intent && !validateGroupIntent(*intent)) return GroupLinkStatus::invalidIntent;
    const auto advanced = advance();
    state.intent = intent;
    state.phase = intent ? GroupLinkPhase::pending : GroupLinkPhase::unlinked;
    return advanced ? GroupLinkStatus::ready : GroupLinkStatus::revisionExhausted;
}
GroupLinkStatus GroupLinkState::link(const GroupIntent& intent) noexcept
{
    if (!validateGroupIntent(intent)) return GroupLinkStatus::invalidIntent;
    if (eligible() && state.intent == intent) return GroupLinkStatus::ready;
    if (!advance()) return GroupLinkStatus::revisionExhausted;
    state.intent = intent;
    state.phase = GroupLinkPhase::authorized;
    return GroupLinkStatus::ready;
}
GroupLinkStatus GroupLinkState::resume() noexcept
{
    if (eligible()) return GroupLinkStatus::ready;
    if (!state.intent) return GroupLinkStatus::notLinked;
    if (!advance()) return GroupLinkStatus::revisionExhausted;
    state.phase = GroupLinkPhase::authorized;
    return GroupLinkStatus::ready;
}
void GroupLinkState::leave() noexcept
{
    advance();
    state.intent.reset();
    state.phase = GroupLinkPhase::unlinked;
}
void GroupLinkState::disconnected() noexcept
{
    if (state.phase == GroupLinkPhase::admitted) state.phase = GroupLinkPhase::authorized;
}
GroupLinkStatus GroupLinkState::acknowledge(GroupUuid runtime, std::uint64_t load,
                                          const GroupIntent& requested, const GroupIntent& normalized) noexcept
{
    if (!validateGroupIntent(requested) || !validateGroupIntent(normalized)) return GroupLinkStatus::invalidIntent;
    if (!eligible() || runtime != state.runtime || load != state.loadRevision || requested != *state.intent)
        return GroupLinkStatus::staleReply;
    auto permitted = requested;
    permitted.member = normalized.member;
    if (permitted != normalized) return GroupLinkStatus::invalidIntent;
    state.intent = normalized;
    state.phase = GroupLinkPhase::admitted;
    return GroupLinkStatus::ready;
}
void detail::GroupLinkStateTestAccess::setLoadRevision(GroupLinkState& link, std::uint64_t revision) noexcept
{ link.state.loadRevision = revision; }
}
