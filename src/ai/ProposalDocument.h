#pragma once
#include "ProjectDocument.h"
#include "AiCatalog.h"
#include "RackReconciliation.h"
#include <cstddef>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace airack::ai
{
// Descriptor-only fence; complete combined requests retain their independent limits.
inline constexpr std::size_t maximumCatalogDescriptorBytes = 917504;
inline constexpr std::size_t maximumCatalogParameterEdits = 12 *
    (rackUnitParameterCapacity(NativeModule::analog) + 8 * rackUnitParameterCapacity(NativeModule::dynamicEq));
static_assert(maximumCatalogParameterEdits >= 12 *
    (rackUnitParameterCapacity(NativeModule::analog) + 8 * rackUnitParameterCapacity(NativeModule::spaceReverb)));
static_assert(maximumCatalogParameterEdits==20892);
static_assert(4*rackUnitParameterCapacity(NativeModule::analog)+8*rackUnitParameterCapacity(NativeModule::dynamicEq)==2620);
struct RequestTag
{
    juce::Uuid runtime;
    std::uint64_t requestId = 0, editRevision = 0, contextRevision = 0;
    bool operator==(const RequestTag&) const = default;
};
enum class RequestScope : std::uint8_t { sound, pattern, both };
struct ProposalPolicy
{
    RequestScope scope = RequestScope::sound;
    bool allowMacroChanges = false;
    bool allowSlotReuse = false;
    bool requirePattern = false;
};
struct ParameterEdit
{
    juce::String id;
    float normalized = 0;
    bool operator==(const ParameterEdit&) const = default;
};
struct MacroEdit
{
    std::uint8_t index = 1;
    juce::String targetId;
    float amount = 0;
    bool operator==(const MacroEdit&) const = default;
};
struct CatalogDesignSelection
{
    std::uint8_t localId = 0;
    juce::Uuid design;
    std::uint64_t revision = 0;
    juce::String contentSha256;
    RackDestination destination;
    bool operator==(const CatalogDesignSelection& other) const
    {
        return localId == other.localId && design == other.design && revision == other.revision
            && contentSha256 == other.contentSha256 && destination.role == other.destination.role
            && destination.chain == other.destination.chain && destination.position == other.destination.position;
    }
};
// The same stable instance identity used when admitting a catalog selection.
juce::Uuid catalogInstanceId(const RequestTag& root, std::uint8_t localId);
struct CatalogParameterEdit
{
    std::uint8_t selection = 0, unit = 0;
    juce::String key;
    float normalized = 0;
    bool operator==(const CatalogParameterEdit&) const = default;
};
struct CatalogDefinition
{
    juce::Uuid design;
    std::uint64_t revision = 0;
    juce::String contentSha256;
    juce::MemoryBlock canonical;
    bool operator==(const CatalogDefinition&) const = default;
};
struct CatalogAuthority
{
    RequestTag root;
    juce::Uuid snapshot;
    std::string descriptorJson;
    std::vector<CatalogDefinition> definitions;
    CatalogRendering rendering=CatalogRendering::companionPcm;
    bool operator==(const CatalogAuthority&) const = default;
};
struct ProposalEdits
{
    std::vector<ParameterEdit> parameters;
    std::vector<RackEdit> racks;
    std::vector<MacroEdit> macros;
    std::optional<PatternDocument> pattern;
    juce::String characteristics, explanation, name;
    RackReconcileChoices reconciliation;
    std::vector<CatalogDesignSelection> catalogSelections;
    std::vector<CatalogParameterEdit> catalogParameters;
};
enum class ProposalStatus : std::uint8_t
{
    accepted, malformed, unsupported, scopeViolation, macroChangeNotRequested,
    replacementRequired, capacityReached, lockedDependency, invalidCandidate, requestedPatternMissing,
    // A parameter edit, macro target, rack edit or detach the user's controls could not make.
    outOfReach
};
enum class ProposalDecodeReason : std::uint8_t { accepted, strictJson, rootFields, parameterEdit, rackEdit, macroEdit, pattern, reconciliation, validity,
    unknownParameter, duplicateParameter, invalidRack, invalidText, invalidMacro, unknownMacroTarget,
    // A catalog selection's fields, a catalog parameter edit's fields, or what they refer to in the catalog.
    catalogSelection, catalogParameter, catalogIntent, invalidName };
const char* proposalDecodeReasonName(ProposalDecodeReason) noexcept;
// A name the AI may give a sound, once trimmed: 1 to 40 characters, at least one letter, and only letters, digits,
// spaces, hyphens and apostrophes.
bool soundNameValid(const juce::String& trimmed);
bool decodeProposal(std::string_view utf8, ProposalEdits& output, ProposalDecodeReason* reason=nullptr);
bool decodeProposal(std::string_view utf8, const ProposalPolicy&, ProposalEdits& output, ProposalDecodeReason* reason=nullptr);
bool decodeProposal(std::string_view utf8, const ProposalPolicy&, const CatalogAuthority&, ProposalEdits& output, ProposalDecodeReason* reason=nullptr);
bool decodeControlEdits(std::string_view, const ProposalPolicy&, const CatalogAuthority*, ProposalEdits&, ProposalDecodeReason* = nullptr);
juce::var controlEditsSchema(const ProposalPolicy&, const CatalogAuthority*);
// Optional native automation warnings are replaced only on accepted admission; omitted callers retain their behavior.
ProposalStatus admitProposal(const ProjectDocument& source, const ProposalPolicy&,
                            const ProposalEdits&, ProjectDocument& output, SourceAssignmentTrace* trace=nullptr, bool* allocationFailed=nullptr, juce::String* automationWarning=nullptr);
ProposalStatus admitProposal(const ProjectDocument& source, const ProposalPolicy&,
                            const ProposalEdits&, const CatalogAuthority&, ProjectDocument& output, SourceAssignmentTrace* trace=nullptr, bool* allocationFailed=nullptr, juce::String* automationWarning=nullptr);
ProposalStatus admitProposal(const ProjectDocument& source, const ProposalPolicy&,
                            const ProposalEdits&, const CatalogAuthority&, const RackInventorySnapshot&, ProjectDocument& output, SourceAssignmentTrace* trace=nullptr, bool* allocationFailed=nullptr, juce::String* automationWarning=nullptr);
juce::var proposalSchema(const ProposalPolicy& policy = {});
juce::var proposalSchema(const ProposalPolicy&, const CatalogAuthority&);
namespace detail
{
// Shared typed admission used by the existing wire and generation boundaries.
bool catalogAuthorityValid(const CatalogAuthority&);
bool catalogIntentValid(const ProposalEdits&, const ProposalPolicy&, const CatalogAuthority&);
// Whether a CREATE SOUND proposal's rack edits only move banks a rack owns in source, or remove those of a rack it
// names in reconciliation.remove, and it detaches nothing.
bool rackEditsInReach(const SoundDocument& source, const ProposalEdits&);
}
}
