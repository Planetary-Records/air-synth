#pragma once
#include "ParameterRegistry.h"
#include "RoutedSynthEngine.h"
#include "RackDesign.h"
#include <memory>

namespace airack
{
struct MacroBinding
{
    std::uint32_t targetKey = 0;
    float amount = 0;
    bool operator==(const MacroBinding&) const = default;
};
struct SoundAudioState
{
    ParameterValues values = defaultParameterValues();
    RackLayout layout {};
    std::array<MacroBinding, 8> macros {};
    std::array<bool, parameterCount> controlLocks {};
    std::array<bool, 4> synthLocks {};
    std::array<bool, 8> effectLocks {};
    KernelCompatibility compatibility {};
    bool operator==(const SoundAudioState&) const = default;
};
inline constexpr std::size_t maximumSoundNameBytes = 160;
// Immutable personality snapshots belong only to the serialized control path.
struct SoundDocument : SoundAudioState
{
    std::shared_ptr<const RackPersonalityState> personalities;
    juce::String name;
    bool operator==(const SoundDocument&) const noexcept;
};
NativeModule sourceModule(const SoundAudioState&,std::uint8_t) noexcept;
bool validateSoundDocument(const SoundDocument&) noexcept;
bool makeRoutedPatch(const SoundDocument&, RoutedPatch&) noexcept;
bool makeRoutedPatch(const SoundAudioState&, RoutedPatch&) noexcept;
bool makeEffectiveParameterValues(const SoundAudioState&, ParameterValues&) noexcept;
bool validateAiSoundEdit(const SoundDocument&, const SoundDocument&) noexcept;
bool encodeSoundState(const SoundDocument&, juce::MemoryBlock&);
bool decodeSoundState(const void*, std::size_t, SoundDocument&);
}
