#pragma once
#include "SoundDocument.h"
#include "PatternDocument.h"
#include "PatternScheduler.h"
#include "GroupIntent.h"

namespace airack
{
struct AudioProjectState
{
    SoundAudioState sound {};
    PatternDocument pattern {};
    PlaybackMode mode = PlaybackMode::externalMidi;
};
struct ProjectDocument
{
    SoundDocument sound {};
    PatternDocument pattern {};
    PlaybackMode mode = PlaybackMode::externalMidi;
    std::optional<GroupIntent> group;
    bool operator==(const ProjectDocument&) const = default;
};
inline constexpr std::size_t projectStateHeaderBytes = 20;
inline constexpr std::size_t minimumProjectStateBytes = 7132;
inline constexpr std::size_t oldProject4MinimumBytes = 7197, oldProject4MaximumBytes = 799538;
inline constexpr std::size_t oldProject5MinimumBytes = 7853, oldProject5MaximumBytes = 800194;
inline constexpr std::size_t oldProject6MinimumBytes = 8221, oldProject6MaximumBytes = 800562;
inline constexpr std::size_t oldProject7MinimumBytes = 8589, oldProject7MaximumBytes = 800930;
inline constexpr std::size_t oldProject8MinimumBytes = 8957, oldProject8MaximumBytes = 801298;
inline constexpr std::size_t oldProject9MinimumBytes = 9325, oldProject9MaximumBytes = 801666;
inline constexpr std::size_t oldProject10MinimumBytes = 9621, oldProject10MaximumBytes = 801962;
inline constexpr std::size_t oldProject11MinimumBytes = 9917, oldProject11MaximumBytes = 802258;
inline constexpr std::size_t oldProject12MinimumBytes = 10141, oldProject12MaximumBytes = 802482;
inline constexpr std::size_t oldProject13MinimumBytes = 10437, oldProject13MaximumBytes = 802778;
inline constexpr std::size_t oldProject14MinimumBytes = 11021, oldProject14MaximumBytes = 803362;
inline constexpr std::size_t oldProject15MinimumBytes = 11533, oldProject15MaximumBytes = 803874;
inline constexpr std::size_t oldProject16MinimumBytes = 17949, oldProject16MaximumBytes = 810290;
inline constexpr std::size_t oldProject17MinimumBytes = 18093, oldProject17MaximumBytes = 810434;
inline constexpr std::size_t oldProject18MinimumBytes = 18669, oldProject18MaximumBytes = 811010;
inline constexpr std::size_t oldProject19MinimumBytes = 19029, oldProject19MaximumBytes = 811370;
inline constexpr std::size_t draftEqProject19MinimumBytes = 19821, draftEqProject19MaximumBytes = 812162;
inline constexpr std::size_t oldProject20MinimumBytes = 20181, oldProject20MaximumBytes = 812522;
inline constexpr std::size_t oldProject21MinimumBytes = 20397, oldProject21MaximumBytes = 812738;
inline constexpr std::size_t proRProject20MinimumBytes = 25949, proRProject20MaximumBytes = 818290;
inline constexpr std::size_t oldProject22MinimumBytes = 23277, oldProject22MaximumBytes = 815618;
inline constexpr std::size_t proRProject22MinimumBytes = 27317, proRProject22MaximumBytes = 819658;
inline constexpr std::size_t oldProject23MinimumBytes = 23281, oldProject23MaximumBytes = 815782;
inline constexpr std::size_t proRProject23MinimumBytes = 30197, proRProject23MaximumBytes = 822538;
inline constexpr std::size_t spectralProject24MinimumBytes = 25081, spectralProject24MaximumBytes = 817582;
inline constexpr std::size_t proRProject24MinimumBytes = 30201, proRProject24MaximumBytes = 822702;
inline constexpr std::size_t oldProject25MinimumBytes = 32001, oldProject25MaximumBytes = 824502;
inline constexpr std::size_t oldProject26MinimumBytes = 32325, oldProject26MaximumBytes = 824826;
inline constexpr std::size_t privateDrumProject26MinimumBytes=37221,privateDrumProject26MaximumBytes=829722;
inline constexpr std::size_t oldProject27MinimumBytes=33413,oldProject27MaximumBytes=825914;
inline constexpr std::size_t oldSaturationProject19MinimumBytes=21197, oldSaturationProject19MaximumBytes=813538;
inline constexpr std::size_t oldSaturationProject21MinimumBytes=22709, oldSaturationProject21MaximumBytes=815050;
inline constexpr std::size_t oldSaturationProject23MinimumBytes=25805, oldSaturationProject23MaximumBytes=818146;
inline constexpr std::size_t oldProject28MinimumBytes=38633, oldProject28MaximumBytes=831134;
inline constexpr std::size_t oldProject29MinimumBytes=41161,oldProject29MaximumBytes=833662;
inline constexpr int currentProjectStateVersion = 30;
inline constexpr std::size_t currentMinimumProjectStateBytes = 41161;
inline constexpr std::size_t groupIntentStateBytes = 561;
inline constexpr std::size_t maximumProjectStateBytes = 835738;
bool validateProjectDocument(const ProjectDocument&) noexcept;
// Control-thread operations. Rejection preserves output, including aliases.
bool encodeProjectState(const ProjectDocument&, juce::MemoryBlock&);
bool decodeProjectState(const void*, std::size_t, ProjectDocument&);
}
