#pragma once
#include "GroupIntent.h"
#include <chrono>
#include <memory>
#include <span>

namespace airack::companion
{
struct GroupSpectrum
{
    GroupUuid runtime;
    std::uint64_t loadRevision=0,sequence=0,generation=0;
    std::uint8_t slot=0;
    double sampleRate=0;
    std::array<std::int16_t,128> postDb100{};
    bool operator==(const GroupSpectrum&) const=default;
};
inline constexpr std::size_t maximumSpectrumPayloadBytes=4096;
bool validateGroupSpectrum(const GroupSpectrum&) noexcept;
bool encodeGroupSpectrum(const GroupSpectrum&,juce::MemoryBlock&);
bool decodeGroupSpectrum(const void*,std::size_t,GroupSpectrum&);
// Control-thread cache. Only a new sequence starts a new arrival deadline.
class GroupSpectrumInbox final
{
public:
    struct Peer {GroupUuid runtime;std::uint64_t loadRevision=0;bool operator==(const Peer&) const=default;};
    using Time=std::chrono::steady_clock::time_point;
    GroupSpectrumInbox();
    ~GroupSpectrumInbox();
    bool configure(juce::Uuid,GroupUuid,std::uint64_t,std::span<const Peer>);
    bool receive(juce::Uuid,GroupUuid,std::uint64_t,std::span<const GroupSpectrum>,Time);
    const GroupSpectrum* current(GroupUuid,Time) const noexcept;
    void clear() noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
