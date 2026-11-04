#pragma once
#include "GenerationRequest.h"
#include "DrumAsset.h"
#include "GroupPerformance.h"
#include <array>

namespace airack { struct BorrowedDrumAssignments; namespace drums {struct DrumPad;} }
namespace airack::ai
{
using PreviewDigest = std::array<std::uint8_t,32>;
inline constexpr std::size_t maximumPreviewMapBytes=8935;
struct PreviewAssetRef {
    drums::AssetId id{}; std::uint8_t channels=1; std::uint32_t frames=0;
    bool operator==(const PreviewAssetRef&) const=default;
};
struct PreviewPadRef {
    std::uint8_t asset=0; std::string name; PreviewDigest metadata{};
    bool operator==(const PreviewPadRef&) const=default;
};
struct PreviewBankRef {
    juce::Uuid rack=juce::Uuid::null(); std::uint8_t unit=0; std::uint64_t generation=0;
    bool operator==(const PreviewBankRef&) const=default;
};
struct PreviewSampleMap {
    std::array<PreviewBankRef,4> banks{};
    std::array<PreviewPadRef,64> pads{};
    std::vector<PreviewAssetRef> assets;
    bool operator==(const PreviewSampleMap&) const=default;
};
struct LocalPreviewContext {
    RequestTag root;
    juce::Uuid catalogSnapshot=juce::Uuid::null();
    std::uint64_t reset=0,sourceWord=0;
    PreviewSampleMap sourceMap;
    PreviewDigest sourceMapDigest{};
    bool operator==(const LocalPreviewContext&) const=default;
};
struct LocalGenerationRequest { GenerationRequest request; LocalPreviewContext local; };
struct LocalGroupGenerationCommand {
    LocalGenerationRequest input; GroupSelection selection; std::optional<RequestTag> parentProposal;
};
struct PreviewBinding {
    RequestTag request,root;
    juce::Uuid catalogSnapshot=juce::Uuid::null();
    std::uint64_t job=0,ticket=0,structuralGeneration=0,reset=0,sourceWord=0;
    CatalogPhase phase=CatalogPhase::select;
    std::uint8_t providerAttempt=0,renderOrdinal=0;
    PreviewDigest sourceMap{},stagedMap{},candidate{},context{};
    bool operator==(const PreviewBinding&) const=default;
};
struct PreviewEvaluate { PreviewBinding binding; ProposalEdits edits; AuditionContext context; };
enum class PreviewFailure : std::uint8_t { none,busy,missingOwner,stale,cancelled,budget,allocation,invalidBinding };
struct PreviewReceipt {
    PreviewBinding binding;
    RenderStatus status=RenderStatus::invalidInput;
    PreviewFailure failure=PreviewFailure::none;
    AudioMeasurements measurements;
    std::uint32_t bodyFrames=0,frames=0;
    bool phraseTruncated=false;
};
struct LocalGenerationResult { GenerationResult numeric; std::optional<PreviewReceipt> rendered; };
using LocalPreviewEvaluator=std::function<PreviewReceipt(const ProjectDocument&,const ProposalEdits&,
    const AuditionContext&,const SourceAssignmentTrace&,const CancelCheck&)>;
LocalGenerationResult generateLocalProposal(const LocalGenerationRequest&,const StructuredCall&,
    const LocalPreviewEvaluator&,const CancelCheck& = {},const GroupPerformanceCapture* = nullptr);
AuditionContext previewContextFor(const GenerationRequest&,const std::optional<PatternDocument>& = {},
    const PreviewSampleMap* staged=nullptr,const SoundDocument* admitted=nullptr);
bool validPreviewMap(const PreviewSampleMap&) noexcept;
bool encodePreviewMap(const PreviewSampleMap&,juce::MemoryBlock&);
bool decodePreviewMap(std::span<const std::byte>,PreviewSampleMap&);
bool digestPreviewPad(const drums::DrumPad&,PreviewDigest&);
bool digestPreviewMap(const PreviewSampleMap&,PreviewDigest&);
bool digestPreviewCandidate(const ProjectDocument&,PreviewDigest&);
bool digestPreviewContext(const AuditionContext&,PreviewDigest&);
bool stagePreviewMap(const PreviewSampleMap&,const SourceAssignmentTrace&,PreviewSampleMap&);
bool validLocalPreviewContext(const LocalGenerationRequest&);
bool validPreviewBinding(const PreviewBinding&) noexcept;
bool validPreviewReceipt(const PreviewReceipt&) noexcept;
bool samePreviewReceipt(const PreviewReceipt&,const PreviewReceipt&) noexcept;
bool samePreviewContext(const AuditionContext&,const AuditionContext&) noexcept;
bool validateGenerationResult(const GenerationRequest&,const GenerationResult&);
bool validateLocalGenerationResult(const LocalGenerationRequest&,const LocalGenerationResult&,bool* allocationFailed=nullptr);
bool joinLocalPreview(const LocalGenerationRequest&,const LocalGenerationResult&,
    const PreviewReceipt&,PreviewAudio&&,GenerationResult&);
}
