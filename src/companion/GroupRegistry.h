#pragma once
#include "GroupContext.h"
#include "CompanionPipe.h"
#include "GroupSpectrum.h"
#include <chrono>
#include <memory>
#include <span>

namespace airack::companion
{
// Exact complete-project authority, including numeric bits; shared by registry/runtime.
bool exactProject(const ProjectDocument&,const ProjectDocument&) noexcept;
inline constexpr std::size_t maximumGroupMembers = 16;
enum class GroupStatus : std::uint8_t
{ ready, invalidInput, notAdmitted, capacity, duplicateRuntime, staleRegistration, invalidSelection, conflict, revisionExhausted };
struct GroupRegistration
{
    std::uint64_t connectionId = 0;
    PipePeer host {};
    GroupUuid runtime {};
    std::uint64_t loadRevision = 0, editRevision = 0;
    ProjectDocument project {};
};
struct GroupAdmission
{
    GroupIntent intent {};
    std::uint64_t revision = 0;
    bool conflicted = false;
};
namespace detail { struct GroupRegistryTestAccess; }
// One trusted control owner supplies verified hosts and explicit admission.
class GroupRegistry
{
public:
    GroupRegistry();
    ~GroupRegistry();
    GroupRegistry(const GroupRegistry&) = delete;
    GroupRegistry& operator=(const GroupRegistry&) = delete;
    GroupStatus admit(const GroupRegistration&, GroupAdmission&);
    GroupStatus updateIntent(std::uint64_t, GroupUuid, std::uint64_t, const GroupIntent&, GroupAdmission&);
    GroupStatus publish(std::uint64_t, GroupUuid, std::uint64_t, std::uint64_t, const ProjectDocument&);
    GroupStatus updatePublication(std::uint64_t, GroupUuid, std::uint64_t, std::uint64_t, const ProjectDocument&, GroupAdmission&);
    bool withdraw(std::uint64_t) noexcept;
    GroupStatus publishSpectrum(std::uint64_t,const GroupSpectrum&,std::chrono::steady_clock::time_point);
    GroupStatus readSpectra(std::uint64_t,GroupUuid,std::uint64_t,std::span<const GroupUuid>,std::chrono::steady_clock::time_point,std::vector<GroupSpectrum>&);
    GroupStatus withdrawSpectrum(std::uint64_t,GroupUuid,std::uint64_t) noexcept;
    GroupStatus capture(std::uint64_t, std::span<const GroupUuid>, GroupContext&);
    bool current(std::uint64_t, const GroupContext&) const noexcept;
    std::size_t memberCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend struct detail::GroupRegistryTestAccess;
};
namespace detail
{
struct GroupRegistryTestAccess
{ static void setNextRevision(GroupRegistry&, std::uint64_t) noexcept; };
}
}
