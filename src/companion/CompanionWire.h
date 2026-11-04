#pragma once
#include "GenerationRequest.h"
#include "LocalPreview.h"
#include "GroupPerformance.h"
#include "GroupSpectrum.h"

namespace airack::companion
{
using WireBytes = std::span<const std::byte>;
inline constexpr std::uint16_t wireVersion = 9;
// Separate provider endpoints keep older helpers from reading the new account payloads.
inline constexpr std::uint16_t providerAccountVersion = 2;
inline constexpr std::size_t wireHeaderBytes = 28;
inline constexpr std::size_t maximumControlPayloadBytes = 1048576;
inline constexpr std::size_t maximumResultPayloadBytes = 8388608;
inline constexpr std::size_t maximumSmallPayloadBytes = 65536;
inline constexpr std::size_t maximumGenerationRequestBytes = 917504;
inline constexpr std::size_t maximumRackReconciliationBytes = 16384;
enum class MessageKind : std::uint16_t { hello=1, welcome, generate, cancel, result, account, accountResult, ping, pong, failure, groupCommand, groupReply, generateGroup, spectrumCommand, spectrumReply, generateLocal=16,generateGroupLocal=17,previewEvaluate=18,previewMeasured=19,resultLocal=20,groupCommandLocal=21,mcpHello=22,mcpRequest=23,mcpReply=24,mcpEvent=25 };
inline constexpr std::uint32_t spectrumCapability=1;
inline constexpr std::uint32_t ownedLocalPreviewCapability=2;
inline constexpr std::uint32_t knownCapabilityMask=spectrumCapability|ownedLocalPreviewCapability;
enum class RuntimeStatus : std::uint8_t { ready, busy, unavailable, unauthorized, invalidMessage, duplicateInstance, staleSession, cancelled, queueFull, modelNotConfigured };
struct FrameHeader
{
    MessageKind kind = MessageKind::hello;
    juce::Uuid session = juce::Uuid::null();
    std::uint32_t payloadBytes = 0;
};
struct Frame
{
    MessageKind kind = MessageKind::hello;
    juce::Uuid session = juce::Uuid::null();
    juce::MemoryBlock payload;
};
struct ControlMessage
{
    ai::RequestTag tag { juce::Uuid::null(), 0, 0, 0 };
    RuntimeStatus status = RuntimeStatus::ready;
    std::uint32_t capabilities=0;
};
enum class AccountAction : std::uint8_t { status, models, selectModel, signIn, addAccount, selectAccount };
// The historical text field carries the selected model or saved-account ID, by action.
struct AccountCommand { AccountAction action = AccountAction::status; juce::String model; };
struct AccountReply
{
    RuntimeStatus status = RuntimeStatus::ready;
    rack::oauth::Status providerStatus = rack::oauth::Status::ready;
    int httpStatus = 0;
    juce::String errorCode, selectedModel;
    bool modelVerified = false;
    std::vector<rack::oauth::Model> models;
    std::vector<rack::oauth::SavedAccount> accounts;
    juce::String activeAccount;
};
enum class GroupAction : std::uint8_t { admit,publish,withdraw,browse,validateProposal,spectrumPublish,spectrumRead,spectrumWithdraw };
constexpr bool spectrumAction(GroupAction action) noexcept{return action>=GroupAction::spectrumPublish&&action<=GroupAction::spectrumWithdraw;}
enum class GroupReplyStatus : std::uint8_t { ready,busy,notAdmitted,invalidInput,invalidSelection,conflict,staleContext,capacity,revisionExhausted };
struct GroupPublication
{
    GroupUuid runtime;
    std::uint64_t loadRevision=0,editRevision=0;
    ProjectDocument project;
    bool busy=false;
    PeerPlaybackObservation playback;
    bool operator==(const GroupPublication&) const = default;
};
struct GroupCommand
{
    std::uint64_t operationId=0;
    GroupAction action=GroupAction::browse;
    GroupUuid runtime;
    std::uint64_t loadRevision=0;
    std::optional<GroupPublication> publication;
    std::optional<ai::RequestTag> proposal;
    std::optional<GroupSpectrum> spectrum;
    std::vector<GroupUuid> spectrumPeers;
    bool operator==(const GroupCommand&) const = default;
};
struct GroupMemberSummary
{
    GroupUuid runtime;
    std::uint64_t loadRevision=0,editRevision=0;
    GroupIntent intent;
    PlaybackMode mode=PlaybackMode::externalMidi;
    bool busy=false,conflicted=false;
    ObservedMidiAvailability noteAvailability=ObservedMidiAvailability::unavailable;
    ObservedMidiReason noteReason=ObservedMidiReason::noNotes;
    std::uint16_t noteCount=0;
    std::uint64_t noteAgeMs=0;
    bool notesTruncated=false;
    ObservedMidiAvailability controlAvailability=ObservedMidiAvailability::unavailable;
    LiveControlReason controlReason=LiveControlReason::notCaptured;
    std::uint64_t controlAgeMs=0;
    bool operator==(const GroupMemberSummary&) const = default;
};
struct GroupReply
{
    std::uint64_t operationId=0;
    GroupAction action=GroupAction::browse;
    GroupUuid runtime;
    std::uint64_t loadRevision=0;
    GroupReplyStatus status=GroupReplyStatus::ready;
    std::optional<GroupIntent> normalized;
    std::uint64_t registryRevision=0;
    bool conflicted=false;
    std::vector<GroupMemberSummary> members;
    std::optional<ai::RequestTag> proposal;
    std::vector<GroupSpectrum> spectra;
    bool operator==(const GroupReply&) const = default;
};
struct GroupGenerationCommand { ai::GenerationRequest request;GroupSelection selection;std::optional<ai::RequestTag> parentProposal; };
std::size_t messagePayloadCap(MessageKind) noexcept;
bool encodeFrame(const Frame&, juce::MemoryBlock&);
bool decodeFrameHeader(WireBytes, FrameHeader&);
bool decodeFrame(WireBytes, Frame&);
bool encodeControl(const ControlMessage&, juce::MemoryBlock&);
bool decodeControl(WireBytes, ControlMessage&);
bool encodeRequest(const ai::GenerationRequest&, juce::MemoryBlock&);
bool decodeRequest(WireBytes, ai::GenerationRequest&);
bool peekResultTag(WireBytes, ai::RequestTag&);
bool encodeResult(const ai::GenerationRequest&, const ai::GenerationResult&, juce::MemoryBlock&);
bool decodeResult(WireBytes, const ai::GenerationRequest&, ai::GenerationResult&);
bool encodeAccountCommand(const AccountCommand&, juce::MemoryBlock&);
bool decodeAccountCommand(WireBytes, AccountCommand&);
bool encodeAccountReply(const AccountReply&, juce::MemoryBlock&);
bool decodeAccountReply(WireBytes, AccountReply&);
bool encodeGroupCommand(const GroupCommand&,juce::MemoryBlock&);
bool decodeGroupCommand(WireBytes,GroupCommand&);
bool encodeGroupReply(const GroupReply&,juce::MemoryBlock&);
bool decodeGroupReply(WireBytes,GroupReply&);
bool encodeGroupGeneration(const GroupGenerationCommand&,juce::MemoryBlock&);
bool decodeGroupGeneration(WireBytes,GroupGenerationCommand&);
bool peekGenerationResultStatus(WireBytes,ai::GenerationStatus&);
bool encodeLocalRequest(const ai::LocalGenerationRequest&,juce::MemoryBlock&);
bool decodeLocalRequest(WireBytes,ai::LocalGenerationRequest&);
bool encodeLocalGroupGeneration(const ai::LocalGroupGenerationCommand&,juce::MemoryBlock&);
bool decodeLocalGroupGeneration(WireBytes,ai::LocalGroupGenerationCommand&);
bool encodePreviewEvaluate(const ai::PreviewEvaluate&,juce::MemoryBlock&);
bool decodePreviewEvaluate(WireBytes,ai::PreviewEvaluate&);
bool encodePreviewReceipt(const ai::PreviewReceipt&,juce::MemoryBlock&);
bool decodePreviewReceipt(WireBytes,ai::PreviewReceipt&);
bool encodeLocalResult(const ai::LocalGenerationRequest&,const ai::LocalGenerationResult&,juce::MemoryBlock&);
bool decodeLocalResult(WireBytes,const ai::LocalGenerationRequest&,ai::LocalGenerationResult&);
bool encodeLocalGroupPublication(const GroupCommand&,const ai::PreviewSampleMap&,juce::MemoryBlock&);
bool decodeLocalGroupPublication(WireBytes,GroupCommand&,ai::PreviewSampleMap&);
bool peekLocalResultTag(WireBytes,ai::RequestTag&);
bool peekPreviewEvaluateTag(WireBytes,ai::RequestTag&);

}
