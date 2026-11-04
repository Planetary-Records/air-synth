#pragma once
#include "PreviewRender.h"
#include "ProposalDocument.h"
#include "OAuthProbe.h"
#include <string>

namespace airack { struct GroupPerformanceCapture; }
namespace airack::ai
{
enum class CatalogPhase : std::uint8_t { select, materialize };
struct CatalogRequestContext
{
    std::uint16_t version = 1;
    CatalogPhase phase = CatalogPhase::select;
    RequestTag root;
    std::optional<RequestTag> selectionParent;
    std::shared_ptr<const CatalogAuthority> authority;
    ProposalEdits baseIntent;
    std::uint8_t inferenceCallsUsed = 0;
    rack::oauth::Status providerStatus = rack::oauth::Status::ready;
    int httpStatus = 0;
};
struct CatalogResultBinding
{
    RequestTag root;
    juce::Uuid snapshot;
    bool operator==(const CatalogResultBinding&) const = default;
};
inline constexpr std::size_t maximumNamesInUse = 200;
struct GenerationRequest
{
    RequestTag tag;
    ProjectDocument source;
    ProposalPolicy policy;
    juce::String model;
    std::string instruction, role, key, scale;
    std::optional<PatternDocument> capturedPhrase;
    std::optional<double> capturedBpm;
    std::uint64_t structuralGeneration = 0;
    std::optional<CatalogRequestContext> catalog;
    // Other open instances' sound names, then the save folder's preset stems.
    std::vector<juce::String> namesInUse;
};
using StructuredCall = std::function<rack::oauth::StructuredResult(
    const juce::String& model, std::string_view utf8Prompt, const juce::var& schema)>;
enum class GenerationStatus : std::uint8_t { ready, invalidRequest, cancelled, providerFailure, budgetExhausted, renderFailure, catalogSelectionRequired };
struct GenerationResult
{
    RequestTag tag;
    GenerationStatus status = GenerationStatus::invalidRequest;
    unsigned inferenceCalls = 0;
    ProposalStatus proposalStatus = ProposalStatus::malformed;
    RenderStatus renderStatus = RenderStatus::invalidInput;
    rack::oauth::Status providerStatus = rack::oauth::Status::ready;
    int httpStatus = 0;
    juce::String errorCode;
    std::optional<ProjectDocument> candidate;
    ProposalEdits edits;
    PreviewAudio audio;
    AudioMeasurements measurements;
    std::optional<CatalogResultBinding> catalog;
};
GenerationResult generateProposal(const GenerationRequest&, const StructuredCall&,
    const CancelCheck& cancelled = {}, const GroupPerformanceCapture* linked = nullptr);
namespace detail
{
juce::var measurementsForFeedback(const AudioMeasurements&);
inline bool catalogRequestValid(const GenerationRequest& r)
{
    if (!r.catalog) return true;
    const auto& c = *r.catalog;
    if (c.version != 1 || c.phase > CatalogPhase::materialize || !c.authority || c.root != c.authority->root
        || c.root.runtime != r.tag.runtime || c.root.requestId == 0 || c.root.requestId > r.tag.requestId
        || c.root.editRevision != r.tag.editRevision || c.root.contextRevision != r.tag.contextRevision
        || !catalogIntentValid(c.baseIntent, r.policy, *c.authority) || c.inferenceCallsUsed > 3
        || c.providerStatus > rack::oauth::Status::busy || c.httpStatus < 0 || c.httpStatus > 599) return false;
    if (c.phase == CatalogPhase::select)
        return !c.selectionParent && c.inferenceCallsUsed == 0 && c.providerStatus == rack::oauth::Status::ready && c.httpStatus == 0;
    return c.selectionParent && c.selectionParent->runtime == c.root.runtime
        && c.selectionParent->editRevision == c.root.editRevision && c.selectionParent->contextRevision == c.root.contextRevision
        && c.selectionParent->requestId >= c.root.requestId && c.selectionParent->requestId < r.tag.requestId
        && c.inferenceCallsUsed > 0 && c.providerStatus == rack::oauth::Status::connected;
}

}
}
