#include "GroupRegistry.h"
#include <algorithm>
#include <bit>
#include <limits>

namespace airack::companion
{
namespace
{
bool sameHost(PipePeer a, PipePeer b) noexcept
{ return a.processId == b.processId && a.creationTime == b.creationTime; }

}

// Value equality alone loses signed zero. Compare numeric bits as well, without
// comparing object padding or allocating a serialized document in current().
bool exactProject(const ProjectDocument& a, const ProjectDocument& b) noexcept
{
    if (a != b) return false;
    if(a.sound.personalities&&!a.sound.personalities->instances.empty()){const auto& left=a.sound.personalities->instances;const auto& right=b.sound.personalities->instances;
        for(std::size_t i=0;i<left.size();++i){const auto& x=left[i].design;const auto& y=right[i].design;
            for(std::size_t u=0;u<x.units.size();++u)for(std::size_t n=0;n<x.units[u].defaults.size();++n)if(std::bit_cast<std::uint32_t>(x.units[u].defaults[n].normalized)!=std::bit_cast<std::uint32_t>(y.units[u].defaults[n].normalized))return false;
            for(std::size_t n=0;n<x.panel.controls.size();++n)if(std::bit_cast<std::uint32_t>(x.panel.controls[n].minimum)!=std::bit_cast<std::uint32_t>(y.panel.controls[n].minimum)||std::bit_cast<std::uint32_t>(x.panel.controls[n].maximum)!=std::bit_cast<std::uint32_t>(y.panel.controls[n].maximum))return false;
        }
    }
    for (std::size_t i = 0; i < a.sound.values.size(); ++i)
        if (std::bit_cast<std::uint32_t>(a.sound.values[i]) != std::bit_cast<std::uint32_t>(b.sound.values[i])) return false;
    for (std::size_t i = 0; i < a.sound.macros.size(); ++i)
        if (std::bit_cast<std::uint32_t>(a.sound.macros[i].amount) != std::bit_cast<std::uint32_t>(b.sound.macros[i].amount)) return false;
    if (std::bit_cast<std::uint64_t>(a.pattern.swing) != std::bit_cast<std::uint64_t>(b.pattern.swing)) return false;
    for (std::size_t i = 0; i < a.pattern.notes.size(); ++i)
        if (std::bit_cast<std::uint64_t>(a.pattern.notes[i].startBeats) != std::bit_cast<std::uint64_t>(b.pattern.notes[i].startBeats)
            || std::bit_cast<std::uint64_t>(a.pattern.notes[i].durationBeats) != std::bit_cast<std::uint64_t>(b.pattern.notes[i].durationBeats)) return false;
    return true;
}
namespace
{
bool conflicting(const GroupIntent& a, const GroupIntent& b) noexcept
{
    return (a.key.length && b.key.length && a.key != b.key)
        || (a.scale.length && b.scale.length && a.scale != b.scale)
        || (a.bpm && b.bpm && a.bpm != b.bpm);
}
}

struct GroupRegistry::Impl
{
    struct Record {
        GroupRegistration registration;std::uint64_t epoch=0,spectrumSequence=0;
        std::optional<GroupSpectrum> spectrum;
        std::optional<std::chrono::steady_clock::time_point> spectrumTime,spectrumReadTime;
    };
    std::array<std::optional<Record>, maximumGroupMembers> records {};
    GroupUuid incarnation { juce::Uuid {} };
    std::uint64_t nextRevision = 1;

    Record* find(std::uint64_t connection) noexcept
    {
        for (auto& record : records)
            if (record && record->registration.connectionId == connection) return &*record;
        return nullptr;
    }
    const Record* find(std::uint64_t connection) const noexcept
    {
        for (const auto& record : records)
            if (record && record->registration.connectionId == connection) return &*record;
        return nullptr;
    }
    static bool sameScope(const GroupRegistration& a, const GroupRegistration& b) noexcept
    { return sameHost(a.host, b.host) && a.project.group->group == b.project.group->group; }
    const Record* member(const GroupRegistration& requester, GroupUuid identity) const noexcept
    {
        for (const auto& record : records)
            if (record && sameScope(requester, record->registration)
                && record->registration.project.group->member == identity) return &*record;
        return nullptr;
    }
    bool conflict(const GroupRegistration& requester) const noexcept
    {
        for (const auto& a : records)
            if (a && sameScope(requester, a->registration))
                for (const auto& b : records)
                    if (b && sameScope(requester, b->registration)
                        && conflicting(*a->registration.project.group, *b->registration.project.group)) return true;
        return false;
    }
    std::uint64_t issue() noexcept
    {
        const auto result = nextRevision;
        if (nextRevision) nextRevision = nextRevision == std::numeric_limits<std::uint64_t>::max() ? 0 : nextRevision + 1;
        return result;
    }
    void advance(const GroupRegistration& requester, std::uint64_t epoch) noexcept
    {
        for (auto& record : records)
            if (record && sameScope(requester, record->registration)) record->epoch = epoch;
    }
    static bool owns(const Record& record, GroupUuid runtime, std::uint64_t load) noexcept
    { return record.registration.runtime == runtime && record.registration.loadRevision == load; }
    static bool eqSlot(const Record& record,std::uint8_t slot) noexcept
    {return slot<8&&record.registration.project.sound.layout.effects[slot].use==SlotUse::present&&record.registration.project.sound.layout.effects[slot].kind==EffectKind::dynamicEq;}
    static void clearRemovedSpectrum(Record& record) noexcept
    {if(record.spectrum&&!eqSlot(record,record.spectrum->slot))record.spectrum.reset();}
};
GroupRegistry::GroupRegistry() : impl(std::make_unique<Impl>()) {}
GroupRegistry::~GroupRegistry() = default;
GroupStatus GroupRegistry::publishSpectrum(std::uint64_t connection,const GroupSpectrum& value,std::chrono::steady_clock::time_point now)
{
    auto* record=impl->find(connection);if(!record)return GroupStatus::notAdmitted;
    if(!Impl::owns(*record,value.runtime,value.loadRevision))return GroupStatus::staleRegistration;
    if(!record->epoch)return GroupStatus::revisionExhausted;
    if(!validateGroupSpectrum(value)||!Impl::eqSlot(*record,value.slot))return GroupStatus::invalidInput;
    if(value.sequence<=record->spectrumSequence||(record->spectrumTime&&now<*record->spectrumTime))return GroupStatus::staleRegistration;
    if(record->spectrumTime&&now-*record->spectrumTime<std::chrono::milliseconds(100))return GroupStatus::capacity;
    record->spectrum=value;record->spectrumSequence=value.sequence;record->spectrumTime=now;return GroupStatus::ready;
}
GroupStatus GroupRegistry::readSpectra(std::uint64_t connection,GroupUuid runtime,std::uint64_t load,std::span<const GroupUuid> selected,std::chrono::steady_clock::time_point now,std::vector<GroupSpectrum>& output)
{
    auto* requester=impl->find(connection);if(!requester)return GroupStatus::notAdmitted;
    if(!Impl::owns(*requester,runtime,load))return GroupStatus::staleRegistration;
    if(!requester->epoch)return GroupStatus::revisionExhausted;
    if(selected.size()>4)return GroupStatus::invalidSelection;
    std::array<const Impl::Record*,4> peers{};
    for(std::size_t i=0;i<selected.size();++i){
        peers[i]=impl->member(requester->registration,selected[i]);
        if(!peers[i]||peers[i]==requester||std::find(selected.begin(),selected.begin()+i,selected[i])!=selected.begin()+i)return GroupStatus::invalidSelection;
    }
    if(requester->spectrumReadTime&&now<*requester->spectrumReadTime)return GroupStatus::staleRegistration;
    if(requester->spectrumReadTime&&now-*requester->spectrumReadTime<std::chrono::milliseconds(100))return GroupStatus::capacity;
    std::vector<GroupSpectrum> result;result.reserve(selected.size());
    for(std::size_t i=0;i<selected.size();++i){const auto& peer=*peers[i];
        if(peer.epoch&&peer.spectrum&&peer.spectrumTime&&now>=*peer.spectrumTime&&now-*peer.spectrumTime<=std::chrono::seconds(2)&&Impl::eqSlot(peer,peer.spectrum->slot))result.push_back(*peer.spectrum);
    }
    output=std::move(result);requester->spectrumReadTime=now;return GroupStatus::ready;
}
GroupStatus GroupRegistry::withdrawSpectrum(std::uint64_t connection,GroupUuid runtime,std::uint64_t load) noexcept
{
    auto* record=impl->find(connection);if(!record)return GroupStatus::notAdmitted;
    if(!Impl::owns(*record,runtime,load))return GroupStatus::staleRegistration;
    record->spectrum.reset();return GroupStatus::ready;
}
GroupStatus GroupRegistry::admit(const GroupRegistration& registration, GroupAdmission& output)
{
    if (!registration.connectionId || !registration.host.processId || !registration.host.creationTime
        || registration.runtime.isNull() || !registration.loadRevision || !registration.project.group
        || !validateProjectDocument(registration.project)) return GroupStatus::invalidInput;
    if (impl->find(registration.connectionId)) return GroupStatus::staleRegistration;
    for (const auto& record : impl->records)
        if (record && sameHost(registration.host, record->registration.host)
            && registration.runtime == record->registration.runtime) return GroupStatus::duplicateRuntime;
    const auto slot = std::find_if(impl->records.begin(), impl->records.end(), [](const auto& record) { return !record; });
    if (slot == impl->records.end()) return GroupStatus::capacity;
    if (!impl->nextRevision) return GroupStatus::revisionExhausted;
    auto normalized = registration;
    while (normalized.project.group->member.isNull() || impl->member(normalized, normalized.project.group->member))
        normalized.project.group->member = juce::Uuid {};
    const auto epoch = impl->issue();
    *slot = Impl::Record { normalized, epoch };
    impl->advance(normalized, epoch);
    output = { *normalized.project.group, epoch, impl->conflict(normalized) };
    return GroupStatus::ready;
}
GroupStatus GroupRegistry::updateIntent(std::uint64_t connection, GroupUuid runtime, std::uint64_t load,
                                      const GroupIntent& intent, GroupAdmission& output)
{
    auto* record = impl->find(connection);
    if (!record) return GroupStatus::notAdmitted;
    if (!Impl::owns(*record, runtime, load)) return GroupStatus::staleRegistration;
    const auto& saved = *record->registration.project.group;
    if (!validateGroupIntent(intent) || intent.group != saved.group || intent.member != saved.member) return GroupStatus::invalidInput;
    if (intent != saved)
    {
        if (!impl->nextRevision) return GroupStatus::revisionExhausted;
        record->registration.project.group = intent;
        impl->advance(record->registration, impl->issue());
    }
    output = { *record->registration.project.group, record->epoch, impl->conflict(record->registration) };
    return GroupStatus::ready;
}
GroupStatus GroupRegistry::publish(std::uint64_t connection, GroupUuid runtime, std::uint64_t load,
                                 std::uint64_t edit, const ProjectDocument& project)
{
    auto* record = impl->find(connection);
    if (!record) return GroupStatus::notAdmitted;
    if (!Impl::owns(*record, runtime, load)) return GroupStatus::staleRegistration;
    if (!validateProjectDocument(project) || project.group != record->registration.project.group) return GroupStatus::invalidInput;
    if (edit < record->registration.editRevision) return GroupStatus::staleRegistration;
    if (edit == record->registration.editRevision)
        return exactProject(project, record->registration.project) ? GroupStatus::ready : GroupStatus::staleRegistration;
    if (!impl->nextRevision) return GroupStatus::revisionExhausted;
    record->registration.project = project;
    record->registration.editRevision = edit;
    Impl::clearRemovedSpectrum(*record);
    impl->advance(record->registration, impl->issue());
    return GroupStatus::ready;
}
GroupStatus GroupRegistry::updatePublication(std::uint64_t connection, GroupUuid runtime, std::uint64_t load,
                                            std::uint64_t edit, const ProjectDocument& project, GroupAdmission& output)
{
    auto* record=impl->find(connection);
    if(!record)return GroupStatus::notAdmitted;
    if(!Impl::owns(*record,runtime,load))return GroupStatus::staleRegistration;
    const auto& incumbent=record->registration;
    if(!validateProjectDocument(project)||!project.group||project.group->group!=incumbent.project.group->group
        ||project.group->member!=incumbent.project.group->member)return GroupStatus::invalidInput;
    if(edit<incumbent.editRevision)return GroupStatus::staleRegistration;
    auto musical=project;musical.group=incumbent.project.group;
    if(edit==incumbent.editRevision&&!exactProject(musical,incumbent.project))return GroupStatus::staleRegistration;
    auto replacement=incumbent;replacement.project=project;replacement.editRevision=edit;
    if(edit!=incumbent.editRevision||project.group!=incumbent.project.group)
    {
        if(!impl->nextRevision)return GroupStatus::revisionExhausted;
        const auto epoch=impl->issue();record->registration=std::move(replacement);impl->advance(record->registration,epoch);
    }
    Impl::clearRemovedSpectrum(*record);
    output={*record->registration.project.group,record->epoch,impl->conflict(record->registration)};
    return GroupStatus::ready;
}
bool GroupRegistry::withdraw(std::uint64_t connection) noexcept
{
    for (auto& record : impl->records)
        if (record && record->registration.connectionId == connection)
        {
            const auto withdrawn = record->registration;
            record.reset();
            impl->advance(withdrawn, impl->issue());
            return true;
        }
    return false;
}
GroupStatus GroupRegistry::capture(std::uint64_t connection, std::span<const GroupUuid> selected, GroupContext& output)
{
    const auto* requester = impl->find(connection);
    if (!requester) return GroupStatus::notAdmitted;
    if (!requester->epoch) return GroupStatus::revisionExhausted;
    if (impl->conflict(requester->registration)) return GroupStatus::conflict;
    if (selected.size() > maximumSelectedGroupPeers) return GroupStatus::invalidSelection;
    std::array<const Impl::Record*, maximumSelectedGroupPeers> peers {};
    for (std::size_t i = 0; i < selected.size(); ++i)
    {
        peers[i] = impl->member(requester->registration, selected[i]);
        if (!peers[i] || peers[i] == requester || std::find(selected.begin(), selected.begin() + i, selected[i]) != selected.begin() + i)
            return GroupStatus::invalidSelection;
    }
    GroupContext context { impl->incarnation, requester->registration.runtime, requester->registration.loadRevision,
                           requester->epoch, *requester->registration.project.group, {} };
    context.peers.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i)
    {
        const auto& peer = peers[i]->registration;
        context.peers.push_back({ peer.runtime, peer.loadRevision, peer.editRevision, peer.project });
    }
    output = std::move(context);
    return GroupStatus::ready;
}
bool GroupRegistry::current(std::uint64_t connection, const GroupContext& context) const noexcept
{
    const auto* requester = impl->find(connection);
    if (!requester || !requester->epoch || context.registry != impl->incarnation
        || context.requesterRuntime != requester->registration.runtime || context.loadRevision != requester->registration.loadRevision
        || context.revision != requester->epoch || context.requester != *requester->registration.project.group
        || context.peers.size() > maximumSelectedGroupPeers || impl->conflict(requester->registration)) return false;
    for (std::size_t i = 0; i < context.peers.size(); ++i)
    {
        const auto& snapshot = context.peers[i];
        if (!snapshot.project.group) return false;
        const auto* peer = impl->member(requester->registration, snapshot.project.group->member);
        if (!peer || peer == requester || snapshot.runtime != peer->registration.runtime
            || snapshot.loadRevision != peer->registration.loadRevision || snapshot.editRevision != peer->registration.editRevision
            || !exactProject(snapshot.project, peer->registration.project)) return false;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (context.peers[prior].runtime == snapshot.runtime) return false;
    }
    return true;
}
std::size_t GroupRegistry::memberCount() const noexcept
{ return static_cast<std::size_t>(std::count_if(impl->records.begin(), impl->records.end(), [](const auto& record) { return bool(record); })); }
void detail::GroupRegistryTestAccess::setNextRevision(GroupRegistry& registry, std::uint64_t revision) noexcept
{ registry.impl->nextRevision = revision; }
}
