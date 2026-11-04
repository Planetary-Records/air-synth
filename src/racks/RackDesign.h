#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>
#include <optional>
#include <vector>
#include "DspCompatibility.h"
#include "RackLayout.h"

namespace airack
{
enum class RackRole : std::uint8_t { synth, fx };
enum class NativeModule : std::uint8_t { analog, wavetable, drive, delay, reverb, compressor, eq, filter, chorus, phaser, flanger, stereo, dimension, frequencyShift, convolution, frequencySplit, midSideSplit, dynamicEq, spaceReverb, eraReverb, drum, saturation };
constexpr bool isSourceModule(NativeModule m) noexcept { return m==NativeModule::analog||m==NativeModule::wavetable||m==NativeModule::drum; }
constexpr bool isEffectModule(NativeModule m) noexcept { return m>=NativeModule::drive&&(m<=NativeModule::eraReverb||m==NativeModule::saturation); }
static_assert(unsigned(EffectKind::saturation)==saturationEffectBank && unsigned(NativeModule::saturation)==21);
constexpr NativeModule nativeModuleForEffect(EffectKind kind) noexcept
{ return kind==EffectKind::saturation?NativeModule::saturation:NativeModule(unsigned(kind)+2); }
constexpr EffectKind effectKindForModule(NativeModule module) noexcept
{ return module==NativeModule::saturation?EffectKind::saturation:EffectKind(unsigned(module)-2); }

enum class PanelLayout : std::uint8_t { instrumentSections, performanceControls, orderedFxStrip };
enum class FocusWidget : std::uint8_t { rotary, verticalSlider, toggle, choice };
enum class FocusResponse : std::uint8_t { linear, logarithmic };
enum class RackValidationMode : std::uint8_t { portableDesign, instanceRecall };
inline constexpr int rackParameterKeyBytes = 38;
// Exact catalog/recall capacity; portable legacy FX keep their existing23 limit.
inline constexpr std::size_t rackUnitParameterCapacity(NativeModule module) noexcept
{
    return isSourceModule(module)?293:!isEffectModule(module)?0:module==NativeModule::spaceReverb?97:module==NativeModule::dynamicEq?181:module==NativeModule::eraReverb?16:module==NativeModule::saturation?36:82;
}
struct LocalParameter {
    std::uint8_t unit {}; juce::String key; float normalized {};
    bool operator==(const LocalParameter&) const = default;
};
struct RackUnit {
    std::uint8_t id {}; NativeModule module = NativeModule::analog; std::vector<LocalParameter> defaults;
    AlgorithmRevision algorithmRevision = legacyAlgorithmRevision;
    bool operator==(const RackUnit&) const = default;
};
struct FocusedControl {
    juce::String label; std::uint8_t unit {}, section {}; juce::String key;
    FocusWidget widget = FocusWidget::rotary; FocusResponse response = FocusResponse::linear;
    float minimum {}, maximum = 1; // Registry physical/display units.
    bool operator==(const FocusedControl&) const = default;
};
struct RackPanel {
    PanelLayout layout = PanelLayout::instrumentSections;
    std::vector<juce::String> sections; std::vector<FocusedControl> controls;
    std::uint8_t surface {}, accent = 2, frame {}, emphasis {}, spacing {}, density {};
    bool operator==(const RackPanel&) const = default;
};
struct RackDesign {
    juce::Uuid id; std::uint64_t revision = 1; juce::String name, intent;
    RackRole role = RackRole::synth; std::vector<RackUnit> units; RackPanel panel;
    bool operator==(const RackDesign&) const = default;
};
struct UnitBinding {
    std::uint8_t unit {}, slot {};
    bool operator==(const UnitBinding&) const = default;
};
struct RackPackageRef {
    juce::String id, version, sha256;
    bool operator==(const RackPackageRef&) const = default;
};
bool validRackPackageRef(const RackPackageRef&);
bool validRackPackageId(const juce::String&);
bool validRackPackageVersion(const juce::String&);
bool validRackPackageHash(const juce::String&);
struct RackInstance {
    juce::Uuid id; RackDesign design; std::vector<UnitBinding> bindings; bool edited = false;
    std::optional<RackPackageRef> package;
    bool operator==(const RackInstance&) const = default;
};
struct RackPersonalityState {
    std::vector<RackInstance> instances;
    bool operator==(const RackPersonalityState&) const = default;
};
// Bindings are keyed by local unit ID; persistence preserves legal vector permutations.
const UnitBinding* rackUnitBinding(const RackInstance&, std::uint8_t unit) noexcept;
bool validateRackDesign(const RackDesign&, RackValidationMode);
bool supportsAlgorithm(NativeModule, AlgorithmRevision) noexcept;
AlgorithmRevision rackAlgorithmRevision(const KernelCompatibility&,NativeModule,std::uint8_t) noexcept;
bool setRackAlgorithmRevision(KernelCompatibility&,NativeModule,std::uint8_t,AlgorithmRevision) noexcept;
// Active local-bank compatibility; callers resolve keys through the registry.
bool rackParameterActive(NativeModule, const juce::String& key);
// Slots are zero-based existing physical banks. Retained suffixes resolve too;
// portable compatibility is checked by validateRackDesign.
std::optional<std::size_t> rackParameterIndex(NativeModule, std::uint8_t slot, const juce::String& key);
bool encodeRackDesign(const RackDesign&, juce::MemoryBlock&, RackValidationMode = RackValidationMode::portableDesign);
bool decodeRackDesign(const void*, std::size_t, RackDesign&, RackValidationMode = RackValidationMode::portableDesign);
bool encodeRackDesignObject(const RackDesign&, juce::var&, RackValidationMode);
bool decodeRackDesignObject(const juce::var&, RackDesign&, RackValidationMode);
juce::var rackDesignSchema(bool includeIdentity = true);
// The name every screen gives a module or effect kind; "Unknown" outside the enums.
juce::String moduleDisplayName(NativeModule);
juce::String effectDisplayName(EffectKind);
}
