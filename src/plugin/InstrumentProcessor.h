#pragma once
#include "SoundState.h"
#include "ProjectDocument.h"
#include "PerformanceMidi.h"
#include "InstrumentAi.h"
#include "RenderedPreview.h"
#include "GroupLinkState.h"
#include "ObservedMidi.h"
#include "DrumOwnership.h"
#include "RackReconciliation.h"
#include "RackLibrary.h"
#include "RackInventory.h"
#include "RackLibraryClient.h"
#include "FxMonitor.h"
#include "InstrumentMcp.h"
#include "InstrumentProposal.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <mutex>

namespace airack
{
class DrumImport;
namespace studio { class AiUnit; struct MakerSkin; }
struct SourceAssignmentTrace;
enum class DrumCommitError : std::uint8_t {
    none,invalid,staleTarget,cancelled,locked,busy,preparationFull,publicationFull,
    decodedBudget,workspaceBudget,missingAsset,allocationFailed,authorityExhausted,audioNotReady
};
struct DrumPadTarget final {
    juce::Uuid rack=juce::Uuid::null();
    std::uint8_t unit=0,bank=0,pad=0;
    std::uint64_t token=0,reset=0;
    bool operator==(const DrumPadTarget&) const=default;
};
struct DrumPadReadback final {
    drums::DrumPad metadata;
    std::uint32_t frames=0,channels=0;
};
struct ParameterGestureValue { std::size_t index; float value; };
struct GroupWorkflowView
{
    GroupLinkView link;
    companion::ClientStatus connection;
    std::vector<companion::GroupMemberSummary> members;
    GroupSelection selection;
    bool busy=false,operationPending=false,conflicted=false,generationReady=false,generationWaits=false,peerContextCurrent=false;
    juce::String message;
};
struct EqSpectrumView
{
    bool enabled=false,capable=false,admitted=false;
    std::uint8_t sendSlot=7,peerCount=0;
    std::array<GroupUuid,4> peers{};
    bool operator==(const EqSpectrumView&) const=default;
};
class InstrumentProcessor final : public juce::AudioProcessor, private juce::AsyncUpdater, private juce::Timer, private juce::VST3ClientExtensions
{
    friend struct InstrumentMcpTestAccess;
public:
    // Control/UI publication only; preparation uses the supplied snapshot off-thread, never scans installations.
    void prepareRackSkins(RackInventoryPtr,std::span<const RackInstance>);
    void requestMakerSkin(const RackInstance&);
    std::shared_ptr<const studio::MakerSkin> preparedMakerSkin(const RackInstance&) const;
    InstrumentProcessor();
    explicit InstrumentProcessor(std::shared_ptr<RackLibraryRuntime>);
    explicit InstrumentProcessor(const companion::ClientOptions&);
    InstrumentProcessor(const companion::ClientOptions&,std::shared_ptr<RackLibraryRuntime>);
    std::shared_ptr<RackLibraryRuntime> rackLibraryRuntime() const {return libraryRuntime;}
    // Native callers, like VST3 hosts, serialize destruction on the message/UI
    // thread and stop concurrent calls before teardown begins.
    ~InstrumentProcessor() override;
    McpView mcpView() const;
    void setMcpEnabled(bool);
    mcp::ToolResult executeMcp(const mcp::Command&);
    void pollMcp();
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    RenderQuality selectedRenderQuality() const noexcept;
    RenderQuality effectiveRenderQuality() const noexcept;
    // Render owner, or lifecycle owner with processing stopped.
    RoutedSynthEngine::SpaceRenderStats spaceRenderStats() const noexcept;
    bool renderQualityPending() const noexcept;
    DynamicEqProcessingMode effectiveEqMode(std::size_t slot) const noexcept;
    bool eqPreparationPending(std::size_t slot) const noexcept;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void* data, int size) override { restoreSoundState(data, size); }
    bool restoreSoundState(const void*, int);
    bool restoreProjectState(const void*, int);
    bool captureDrumPadTarget(juce::Uuid,std::uint8_t,std::uint8_t,DrumPadTarget&,DrumCommitError&) noexcept;
    bool captureDrumPerformanceTarget(juce::Uuid,std::uint8_t,std::uint8_t,DrumPadTarget&,DrumCommitError&) noexcept;
    bool enqueueDrumPad(const DrumPadTarget&,std::uint8_t velocity,std::uint8_t channel=1) noexcept;
    DrumPerformanceStatus drumPerformanceStatus() const noexcept;
    DrumImport& drumImport() noexcept;
    bool beginDrumGeneration(const DrumPadTarget&,const juce::String&,
        const std::array<std::uint64_t,3>&,DrumCommitError&) noexcept;
    void cancelDrumGeneration(std::uint64_t session);
    bool selectDrumGenerationCandidate(std::uint64_t session,std::size_t ordinal);
    bool readDrumPad(const DrumPadTarget&,DrumPadReadback&,DrumCommitError&) noexcept;
    bool beginDrumInput(std::uint64_t&,DrumCommitError&) noexcept;
    bool reserveDrumInputPreparation(std::uint64_t,std::uint32_t,std::uint32_t,DrumCommitError&) noexcept;
    bool finishDrumInput(std::uint64_t,std::span<const float>,std::uint32_t,const drums::AssetId&,std::uint64_t&,DrumCommitError&) noexcept;
    void cancelDrumInput(std::uint64_t) noexcept;
    bool prepareDrumAsset(std::span<const float>,std::uint32_t,std::uint64_t&,DrumCommitError&) noexcept;
    void discardPreparedDrumAsset(std::uint64_t) noexcept;
    bool assignPreparedDrumPad(const DrumPadTarget&,std::uint64_t,const drums::DrumPad&,
        std::span<const ParameterGestureValue>,DrumCommitError&) noexcept;
    bool clearDrumPad(const DrumPadTarget&,DrumCommitError&) noexcept;
    bool tryProjectSnapshot(ProjectDocument&, std::uint64_t&) const noexcept;
    bool tryProjectSnapshot(ProjectDocument&, std::uint64_t&, std::uint64_t&) const noexcept;
    bool applyPatternEdit(const PatternDocument&, std::uint64_t);
    bool setPlaybackMode(PlaybackMode, std::uint64_t);
    bool createPattern(std::uint8_t, std::uint64_t);
    bool savePattern(const juce::File&);
    bool loadPattern(const juce::File&);
    bool exportPattern(const juce::File&);
    std::optional<PhraseMeter> currentHostMeter() const noexcept;
    PatternPlaybackStatus patternPlaybackStatus() const noexcept;
    /** While the Internal pattern plays: its phrase position, in quarter beats, at the latest audio block's first sample. */
    std::optional<double> patternPlaybackPosition() const noexcept;
    PerformanceInputStatus performanceInputStatus() const noexcept;
    // One serialized control consumer; audio/lifecycle owns publication.
    void captureObservedMidi(ObservedMidiSnapshot&) noexcept;
    bool enqueueKeyboardNote(bool, std::uint8_t, std::uint8_t, std::uint8_t) noexcept;
    void releaseKeyboardNotes() noexcept;
    bool trySoundSnapshot(SoundDocument&, std::uint64_t&) const noexcept;
    // Coherent document/edit/reset read; failure preserves every output.
    bool trySoundSnapshot(SoundDocument&, std::uint64_t&, std::uint64_t& reset) const noexcept;
    bool applySoundEdit(const SoundDocument&, SoundEditOrigin, std::uint64_t);
    bool beginParameterGesture(std::size_t);
    bool beginParameterGesture(std::size_t,std::span<const ParameterGestureValue>,const SoundDocument&,std::uint64_t expectedRevision,std::uint64_t expectedReset);
    bool setParameterGestureValue(std::size_t,float);
    bool setParameterGestureValues(std::size_t,std::span<const ParameterGestureValue>);
    // Discrete multi-field commits may require the exact captured raw sound.
    // Recheck after host begin callbacks as well as before announcing fields.
    bool setParameterGestureValues(std::size_t,std::span<const ParameterGestureValue>,const SoundDocument&,std::uint64_t expectedRevision,std::uint64_t expectedReset);
    bool endParameterGesture(std::size_t);
    RackEditResult applyRackEdit(const RackEdit&,std::uint64_t);
    RackCandidateStatus previewRackOperation(const RackOperation&,std::uint64_t,SoundDocument&,RackDifference&);
    RackCandidateStatus applyRackOperation(const RackOperation&,std::uint64_t);
    RackCandidateStatus previewRackOperations(std::span<const RackOperation>,std::uint64_t,SoundDocument&,RackDifference&);
    RackCandidateStatus applyRackOperations(std::span<const RackOperation>,std::uint64_t,
        std::optional<std::uint64_t> expectedReset={},const RackLibraryPlacementPreparationPtr& ={},
        const std::shared_ptr<const RackLibraryPlacementView>& ={});
    bool undoSoundEdit();
    bool redoSoundEdit();
    bool saveSoundPreset(const juce::File&);
    bool captureRackDesign(juce::Uuid,RackDesign&) const;
    bool captureAvailableRackCatalog(std::shared_ptr<const RackCatalogSnapshot>&,RackCatalogStatus* =nullptr);
    bool captureAvailableRackInventory(RackInventoryPtr&,RackLibraryStatus* =nullptr);
    void retryAvailableRackInventory();
    bool loadSoundPreset(const juce::File&);
    bool loadFactorySound(std::size_t);
    juce::File soundFolder() const;
    juce::String soundPresetPath() const;
    void setSoundPresetPath(const juce::String&);
    std::vector<juce::String> soundNamesInUse() const;
    juce::String soundName() const;
    bool tempoUnavailable() const noexcept;
    // Message-thread handle; capture storage remains valid after the editor
    // retires or a stopped processor is destroyed. Never serialize this handle.
    std::shared_ptr<FxMonitor> fxMonitorHandle() const noexcept;
    std::optional<FxMonitor::SaturationToken> acquireSaturationAudition(std::uint8_t,const juce::Uuid&,std::uint64_t revision,std::uint64_t resetEpoch) noexcept;
    bool requestSaturationAudition(FxMonitor::SaturationToken,std::optional<SaturationLane>,const juce::Uuid&,std::uint64_t revision,std::uint64_t resetEpoch) noexcept;
    void retireSaturationAudition(FxMonitor::SaturationToken) noexcept;
    // Read on the render owner, or while stopped; not a host/state parameter.
    RoutedSynthEngine::DspWorkerStats dspWorkerStats() const noexcept { return engine.dspWorkerStats(); }
    RoutedSynthEngine::SpectralStats spectralStats() const noexcept { return engine.spectralStats(); }

    bool stateLoadFailed() const noexcept { return loadFailed.load(std::memory_order_relaxed); }
    juce::RangedAudioParameter& control(std::size_t);
    // Message/control-thread only. Proposal pointers last until the next AI mutation.
    juce::Uuid aiRuntime() const noexcept;
    AiMusicalContext aiMusicalContext() const;
    bool setAiMusicalContext(const AiMusicalContext&);
    GroupLinkView groupLinkView() const;
    GroupWorkflowView groupWorkflowView() const;
    bool requestGroupConnection();
    bool browseGroups();
    bool selectGroupPeers(std::span<const GroupUuid>);
    std::uint64_t openEqSpectrumView();
    bool configureEqSpectrumView(std::uint64_t,bool,std::uint8_t,std::span<const GroupUuid>);
    void closeEqSpectrumView(std::uint64_t);
    EqSpectrumView eqSpectrumView(std::uint64_t) const;
    bool captureEqSpectrum(std::uint64_t,std::uint8_t,std::uint64_t generation,std::uint64_t windows,double,const std::array<std::int16_t,128>&);
    const companion::GroupSpectrum* peerEqSpectrum(std::uint64_t,GroupUuid) const;
    void closeAiPanel();
    bool setGroupIntent(const GroupIntent&);
    bool resumeGroupIntent();
    bool leaveGroupIntent();
    bool acknowledgeGroupIntent(GroupUuid, std::uint64_t, const GroupIntent&, const GroupIntent&);
    void groupCompanionDisconnected();
    std::optional<ai::GenerationRequest> beginAiRequest(const AiRequestOptions&, std::optional<double> observedBpm = {});
    bool receiveAiResult(ai::GenerationResult&&);
    const ai::GenerationResult* aiProposal() const noexcept;
    AiProposalPhase aiProposalPhase() const noexcept;
    void refreshAiProposal();
    void discardAiProposal();
    bool applyAiProposal();
    bool startAiPreview();
    void stopAiPreview() noexcept;
    bool aiPreviewStopped() const noexcept;
    bool aiPreviewPlaying() const noexcept;
    std::optional<double> currentHostBpm() const noexcept;
    const companion::AccountReply* aiAccountReply() const noexcept;
    AiWorkflowStatus aiWorkflowStatus() const;
    bool requestAiAccount(const companion::AccountCommand&);
    bool generateAi(const AiRequestOptions&);
    bool refineAi(const std::string&);
    void pollAi();
    void cancelAi();
    void closeAiSession();
    bool applyAiWhenStopped();
    std::optional<ai::GenerationRequest> beginAiRefinement(const std::string&,const juce::String&);
    const ai::GenerationRequest* aiCurrentRequest() const noexcept;
    const ProjectDocument* aiOriginalSource() const noexcept;
    const ai::ProposalEdits* aiIntendedEdits() const noexcept;
    bool tryParameterSnapshot(ParameterValues&, std::uint64_t&) const noexcept;
    const juce::String getName() const override { return "air"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
    bool getPluginHasMainInput() const override { return false; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
private:
    std::shared_ptr<RackLibraryRuntime> libraryRuntime;
    bool sessionTracked = false;
    RackLibrarySkinPreparationPtr makerPreparation;
    RackLibraryInventoryPreparationPtr inventoryPreparation;
    RackLibraryPlacementPreparationPtr placementPreparation;
    ProposalCommitResult commitProposal(const ProposalCommitInput&,const std::function<bool()>&);
    ProposalCommitResult commitMcpEdit(const mcp::Command&);
    bool refreshMcpSnapshot(ProjectDocument&);
    juce::var mcpLinkedContextOwned() const;
    bool mcpBindingCurrentOwned(const McpState::Binding&,ProjectDocument* = nullptr);
    void retireMcpStageOwned(std::uint64_t=0);
    mcp::ToolResult executeMcpPreview(const mcp::Command&);
    void pollMcpPreviewOwned();
    bool mcpPreviewOwnedByOther(const mcp::Command&) const;
    void controlMcpPreviewOwned(const mcp::Command&);
    void releaseMcpPreviewChargeOwned() noexcept;
    std::uint64_t mcpPreviewBytes=0;
    std::atomic<std::uint64_t> mcpAudioValidUntil{0};
    void retireMcpPreviewOwned(bool force=false) noexcept;
    bool saturationAuditionContext(std::uint8_t,const juce::Uuid&,std::uint64_t,std::uint64_t) const noexcept;
    std::optional<ai::GenerationRequest> beginAiRequestWithCatalog(const AiRequestOptions&,
        std::optional<double>, std::shared_ptr<const RackCatalogSnapshot>,RackInventoryPtr ={});
    friend class studio::AiUnit;
    friend struct InstrumentAiTestAccess;
    friend struct InstrumentGroupTestAccess;
    friend struct InstrumentStructuralTestAccess;
    friend struct InstrumentDrumTestAccess;
    friend class DrumImport;
    struct DrumAudioState {
        drums::BorrowedSamples samples {};
        drums::PublicationIdentity identity {};
        std::uint64_t source=0;
        std::uint64_t acknowledgedSource=0;
    };
#ifndef NDEBUG
    enum class DrumTestPoint {afterBorrowedLoad,afterSourceCas,beforeReportPublish,afterPreparationReservation,afterCoreInstall,afterSaveCapture,afterRestoreReservation,beforeReadValidation,afterPresetFileCommit,beforeControlPumpWork,beforeMessageCallbackStop,beforeCallbackStorageRelease,beforeOwnerClear,afterDrumCutoff,afterDrumAuthority,beforeDrumReadAttempt};
    void (*drumTestHook)(void*,DrumTestPoint)=nullptr;
    void* drumTestContext=nullptr;
    void drumTestPoint(DrumTestPoint point) const {if(drumTestHook)drumTestHook(drumTestContext,point);}
    struct DrumHistoryDebugView {
        std::size_t count=0,cursor=0;std::uint64_t storedUnits=0;
        std::array<const juce::UndoableAction*,64> identities {};
        std::array<std::uint32_t,64> units {};
        std::array<std::uint8_t,64> kinds {},rootCounts {};
        std::array<std::uint64_t,64> masks {};
        std::array<std::array<const drums::SampleAsset*,128>,64> roots {};
        std::array<std::array<drums::AssetId,128>,64> assetIds {};
        std::array<std::array<std::uint64_t,8>,64> chargeTerms {};
        bool operator==(const DrumHistoryDebugView&) const=default;
    };
    void drumHistoryDebugSnapshot(DrumHistoryDebugView&) const noexcept;
    bool replayRetainedOwnedAction(const juce::UndoableAction*,bool);
    static std::array<std::size_t,2> legacyHistorySizes() noexcept;
#endif
    using DrumOwners=std::array<drums::AssetPtr,64>;
    struct OwnedPadImage {std::uint8_t bank=0,pad=0;drums::DrumPad metadata;drums::AssetPtr owner;};
    struct BankOwnerBinding {
        SlotUse use=SlotUse::neverUsed;juce::Uuid rack=juce::Uuid::null();std::uint8_t unit=0;
        bool operator==(const BankOwnerBinding&) const=default;
    };
    struct OwnedAssignmentScope {
        std::uint64_t pads=0;
        std::array<BankOwnerBinding,4> beforeOwners {},afterOwners {};
        std::uint8_t guardedBanks=0,invalidateBanks=0;
        std::vector<OwnedPadImage> before,after;
    };
    struct AssignmentStage {drums::DrumSlots slots;DrumOwners owners {};OwnedAssignmentScope scope;std::uint8_t fullNumericBanks=0;};
    struct PadNumericDelta {std::size_t index=0;float before=0,after=0;};
    enum class OwnedDocumentKind : std::uint8_t {rack,soundPreset};
    struct ChangedProjectFields {
        std::array<bool,parameterCount> values {};
        std::array<bool,8> macros {};
        std::array<std::array<bool,2>,4> synthRevisions {};
        std::array<std::array<bool,effectBankCount>,8> effectRevisions {};
        bool layout=false,pattern=false,named=false;
        std::vector<juce::Uuid> personalities;
    };
    struct DrumPadAction;
    struct OwnedDocumentAction;
    struct PreparedControlCommit;
    static std::size_t historyPersonalityBytes(const SoundDocument&);
    DrumCommitError drumCommitStatus=DrumCommitError::none;
    struct EditHistory {
        std::array<std::unique_ptr<juce::UndoableAction>,64> actions {};
        std::array<std::uint32_t,64> units {};
        std::array<std::uint64_t,64> actionIds {};
        std::uint64_t nextActionId=1;
        std::size_t count=0,cursor=0;std::uint64_t storedUnits=0;
    };
    bool performHistoryAction(std::unique_ptr<juce::UndoableAction>);
    bool undoHistoryAction();
    bool redoHistoryAction();
    void clearHistoryOwned() noexcept;
    void refreshDrumHistoryRootsOwned() noexcept;
    void drainDrumFeedbackOwned() noexcept;
    void publishDrumFeedback() noexcept;
    bool invalidateDrumTargetsOwned(std::uint64_t,std::uint64_t) noexcept;
    bool resolveDrumTargetOwned(const ProjectDocument&,const DrumPadTarget&,bool,DrumCommitError&,bool requireUnlocked=true) const noexcept;
    bool captureDrumTarget(juce::Uuid,std::uint8_t,std::uint8_t,DrumPadTarget&,DrumCommitError&,bool requireUnlocked) noexcept;
    bool enqueueDrumPadAtOffset(const DrumPadTarget&,std::uint8_t,std::uint8_t,std::uint32_t) noexcept;
    bool finishDrumPreparation(std::size_t,std::uint64_t,std::uint64_t,std::span<const float>,std::uint32_t,const drums::AssetId*,std::uint64_t&,DrumCommitError&) noexcept;
    void releaseDrumInputPreparationWork(std::uint64_t) noexcept;
    void releaseDrumInputWork(std::uint64_t) noexcept;
    bool beginDrumGenerationInputs(std::array<std::uint64_t,3>&,std::uint64_t,DrumCommitError&) noexcept;
    bool drumInputAiBusyOwned() const noexcept;
    void revokeDrumInputPreparation(std::uint64_t) noexcept;
    bool startManualDrumPreviewOwned(const DrumPadTarget&,std::uint64_t,std::uint64_t,DrumCommitError&) noexcept;
    bool startManualDrumPreview(const DrumPadTarget&,std::uint64_t,std::uint64_t,DrumCommitError&) noexcept;
    void stopManualDrumPreview() noexcept;
    bool manualDrumPreviewPlaying() const noexcept;
    void clearStoppedPreviewOwned() noexcept;
    void releaseManualPreviewChargeOwned() noexcept;
    void stopAiPreviewOwned() noexcept;
    static BankOwnerBinding drumBankOwner(const SoundDocument&,std::uint8_t) noexcept;
    bool validateFreshSourceOwnerAdmissionOwned(const SoundDocument&,const SoundDocument&,
        const SourceAssignmentTrace*,DrumCommitError&) const noexcept;
    bool performOwnedPadEdit(const DrumPadTarget&,const OwnedPadImage&,std::span<const ParameterGestureValue>,DrumCommitError&);
    bool performOwnedDocumentEdit(const ProjectDocument&,const ProjectDocument&,OwnedAssignmentScope,
        ChangedProjectFields,OwnedDocumentKind,bool,DrumCommitError&);
    bool replayOwnedPad(const DrumPadAction&,bool,DrumCommitError&);
    bool replayOwnedDocument(const OwnedDocumentAction&,bool,DrumCommitError&);
    bool publishDrumCommit(const ProjectDocument&,bool,bool,bool,const std::array<bool,parameterCount>*,bool,
        std::unique_ptr<drums::DrumOwnership::PublicationRecord>,std::uint64_t,const juce::String* = nullptr) noexcept;
    static ChangedProjectFields changedProjectFields(const ProjectDocument&,const ProjectDocument&);
    bool mergeChangedProjectFieldsOwned(const ProjectDocument&,const ChangedProjectFields&,ProjectDocument&) const;
    bool performOwnedRackEdit(const ProjectDocument&,const ProjectDocument&,const SourceAssignmentTrace&,DrumCommitError&);
    bool buildAssignmentStageOwned(const SoundDocument&,const SourceAssignmentTrace&,AssignmentStage&,DrumCommitError&) const;
    bool buildAssignmentStageOwned(const SoundDocument&,const SourceAssignmentTrace&,const drums::DrumSlots&,const DrumOwners&,AssignmentStage&,DrumCommitError&) const;
    bool beginDrumSaveOwned(ProjectDocument&);
    void endDrumSaveOwned() noexcept;
    bool encodeOwnedDrumState(const ProjectDocument&,bool,juce::MemoryBlock&);
    bool restoreOwnedDrumState(const void*,int,bool,const juce::File*,bool& groupChanged);
    struct DrumControlPump final : juce::Timer {
        InstrumentProcessor& owner;
        explicit DrumControlPump(InstrumentProcessor&);
        ~DrumControlPump() override;
        void timerCallback() override;
    };

#ifndef NDEBUG
    static void setStructuralTestHook(void (*)(int)) noexcept;
    static void sourceCaptureTestPoint() noexcept;
#endif
    const juce::Uuid aiRuntimeIdentity;
    GroupLinkState groupLinkState { GroupUuid(aiRuntimeIdentity) };
    bool setGroupIntentOwned(const GroupIntent&);
    bool publishGroupOwned(const std::optional<GroupIntent>&);
    void groupMusicalContextOwned(AiMusicalContext&&) noexcept;
    void invalidateAiContextOwned() noexcept;
    struct AtomicGroup
    {
        std::atomic<bool> present {false}, hasBpm {false};
        std::array<std::atomic<std::uint8_t>,32> identities {};
        std::array<std::atomic<std::uint16_t>,4> lengths {};
        std::array<std::array<std::atomic<char>,maximumGroupTextBytes>,4> texts {};
        std::atomic<double> bpm {0};
        void store(const std::optional<GroupIntent>&) noexcept;
        void load(ProjectDocument&) const noexcept;
    } groupMetadata;
    companion::CompanionClient aiClient {aiRuntimeIdentity};
    struct GroupControl
    {
        ProjectDocument committed;
        std::uint64_t committedEdit=0,nextOperation=1;
        GroupLinkView processed;
        juce::Uuid session=juce::Uuid::null();
        std::vector<companion::GroupMemberSummary> members;
        GroupSelection selection;
        enum class Purpose { admit,publish,browse,check,apply,spectrumPublish,spectrumRead,spectrumWithdraw };
        struct Spectrum
        {
            std::uint64_t nextOwner=1,owner=0,pendingOwner=0,sequence=0,submitted=0,publishTime=0,readTime=0;
            EqSpectrumView view,pendingView;
            bool withdraw=false;
            struct Window {std::uint8_t slot=0;std::uint64_t generation=0,windows=0;bool operator==(const Window&) const=default;};
            std::optional<Window> observed;
            std::optional<companion::GroupSpectrum> latest;
            std::unique_ptr<companion::GroupSpectrumInbox> inbox;
        } spectrum;
        std::unique_ptr<companion::GroupCommand> pending;
        Purpose purpose=Purpose::browse;
        juce::Uuid pendingSession=juce::Uuid::null();
        bool explicitConnection=false,discovery=false,admitted=false,dirty=true,publishedBusy=false;
        bool browseRequested=false,conflicted=false,pumping=false,stale=false;
        std::uint64_t observationTime=0,browseTime=0,checkTime=0,lastTime=0;
        struct Marker
        {
            ai::RequestTag tag;
            juce::Uuid session;
            GroupSelection selection;
            const ai::GenerationResult* proposal=nullptr;
            bool current=false;
        };
        std::optional<Marker> marker,parent,applyToken;
        // Selection result whose materialize request waits for the pending group reply.
        std::optional<ai::RequestTag> followupParent;
        struct WaitingPress
        {
            bool refine=false;
            AiRequestOptions options;
        };
        std::optional<WaitingPress> waiting;
        juce::String message;
        const void* panelOwner=nullptr;
        std::function<void()> refreshPanel;
    };
    std::unique_ptr<GroupControl> groupControl;
    bool groupBusyOwned() const noexcept;
    void sealGroupCommitOwned() noexcept;
    bool groupMarkerCurrentOwned(const GroupControl::Marker&) const noexcept;
    void invalidateGroupedProposal();
    void resetGroupConnection();
    void reconcileGroupConnection();
    void pollGroup();
    bool settleGroupPresses();
    void refuseGroupPresses();
    bool submitGroup(GroupControl::Purpose,std::uint64_t);
    void resetEqSpectrumConnection();
    void configureEqSpectrumInbox();
    void receiveEqSpectrumReply(const companion::GroupCommand&,const companion::GroupReply&);
    bool submitEqSpectrum(std::uint64_t,std::uint64_t before);
    void submitCatalogFollowup();
    void timerCallback() override;
    companion::ClientOptions aiClientOptions;
    std::shared_ptr<const RackCatalogSnapshot> availableRackCatalog;
    std::optional<companion::AccountReply> aiAccount;
    std::optional<companion::AccountCommand> aiStartupAccount,aiActiveAccount;
    juce::Uuid aiAccountSession=juce::Uuid::null();
    bool aiModelsDiscovered=false;
    juce::String aiWorkflowMessage,aiVerifiedModel;
    AiMusicalContext aiContext;
    std::uint64_t aiNextRequestId = 1, aiContextRevision = 0;
    // Fixed control-owned proposal storage stays off native callers' stacks.
    // Construction owns this allocation; prepared audio never touches it.
    struct AiPayloadStorage
    {
        std::optional<ai::GenerationRequest> request;
        std::optional<ai::GenerationResult> result;
    };
    std::unique_ptr<AiPayloadStorage> aiPayloads = std::make_unique<AiPayloadStorage>();
    RackInventoryPtr aiRootCatalogSnapshot;
    std::shared_ptr<const RackCatalogSnapshot> aiRootLegacyCatalogSnapshot;
    // Large proposal storage is allocated only by explicit control-thread work.
    // Keep ordinary stack-owned native processors within their existing footprint.
    std::unique_ptr<ProjectDocument> aiSourceOriginal;
    std::optional<ai::ProposalPolicy> aiOriginalPolicy;
    std::unique_ptr<ai::ProposalEdits> aiIntent;
    struct RefinementBackup
    {
        ai::GenerationRequest request;
        ai::GenerationResult result;
        ai::ProposalEdits intent;
    };
    std::unique_ptr<RefinementBackup> aiRefinementBackup;
    struct AiLocalCharge final {
        drums::DrumOwnership* owner=nullptr;
        std::uint64_t base=0,work=0,output=0,parent=0;
        ~AiLocalCharge();
    };
    static void deleteAiLocalTrace(drums::DrumOwnership*,ai::PreviewCallTrace*) noexcept;
    struct AiLocalTraceDelete final {
        drums::DrumOwnership* owner=nullptr;
        void operator()(ai::PreviewCallTrace* trace) const noexcept {InstrumentProcessor::deleteAiLocalTrace(owner,trace);}
    };
    using AiLocalCallTrace=std::unique_ptr<ai::PreviewCallTrace,AiLocalTraceDelete>;
    struct AiLocalWork final {
        ProjectDocument candidate;
        ai::PreviewEvaluate evaluation;
        BorrowedDrumAssignments borrowed;
        const std::atomic_bool* cancel=nullptr;
        ai::PreviewReceipt receipt;
        ai::PreviewAudio audio;
        ai::PreviewWorkDiagnostics renderWork;
        AiLocalCallTrace callTrace;std::atomic_bool* firstCallCompleted=nullptr;
    };
    struct AiLocalState final {
        // First member: release reservations only after every later buffer destroys.
        AiLocalCharge charge;
        std::unique_ptr<ai::LocalPreviewContext> current,backupContext;
        ai::PreviewSampleMap rootMap,parentMap,stagedMap;
        drums::TargetTokens targets{};
        std::uint64_t publicationGeneration=0,lastJob=0,lastTicket=0;
        std::uint8_t lastOrdinal=0,lastAttempt=0;
        std::atomic_bool cancel{false};
        bool rendering=false,retiring=false,joining=false,receiptSent=false,hasParent=false,bound=false;
        juce::Uuid session=juce::Uuid::null();companion::PipePeer peer;
        std::optional<ai::PreviewReceipt> receipt,parentReceipt;
        ai::PreviewAudio audio;
    };
    // Bounded numeric evidence survives stale/cancel drain; existing Tool friend reads it.
    struct AiLocalRenderObservation final {
        std::uint64_t requestId=0,job=0,ticket=0;
        std::uint8_t ordinal=0;
        ai::RenderStatus status=ai::RenderStatus::invalidInput;
        ai::PreviewWorkDiagnostics work;
        AiLocalCallTrace callTrace;
    };
    std::unique_ptr<AiLocalState> aiLocal;
    std::uint64_t aiPreviewBytes=0,aiGroupPublicationBytes=0;
    static bool aiRequestUsesLocalPreview(const ai::GenerationRequest&) noexcept;
    bool buildAiPreviewMapOwned(const SoundDocument&,const drums::DrumDocument&,const DrumOwners&,std::uint64_t,ai::PreviewSampleMap&) const;
    std::unique_ptr<AiLocalState> allocateAiLocalStateOwned();
    std::unique_ptr<AiLocalState> prepareAiLocalRootOwned(const ai::GenerationRequest&,std::uint64_t,std::unique_ptr<AiLocalState>);
    void replaceAiLocalRootOwned(std::unique_ptr<AiLocalState>) noexcept;
    bool aiLocalCurrentOwned() const noexcept;
    bool submitAiGeneration(const ai::GenerationRequest&,const GroupSelection* =nullptr,const ai::RequestTag* =nullptr);
    void retireAiLocalOwned() noexcept;
    void drainAiLocalOwned(bool force=false) noexcept;
    void restoreAiLocalParentOwned();
    static void renderAiLocalWork(AiLocalWork&) noexcept;
    void finishAiLocalWork(std::unique_ptr<AiLocalWork>);
    void pollAiLocal();
    bool receiveAiLocalResult(ai::LocalGenerationResult&&);
    bool receiveAiResultOwned(ai::GenerationResult&&,bool localValidated=false);
    bool validateAiLocalParentOwned(const ProjectDocument&,const SourceAssignmentTrace&);
    void acceptAiLocalParentOwned() noexcept;
    bool validateAiLocalApplyOwned(const SoundDocument&,const SourceAssignmentTrace&);
    void releaseAiPreviewChargeOwned() noexcept;
    void releaseAiGroupPublicationChargeOwned() noexcept;

    struct DelayedApply
    {
        ai::RequestTag tag;
        const ai::GenerationResult* proposal=nullptr;
    };
    std::optional<DelayedApply> aiDelayedApply;
    bool aiAccountReady(const juce::String&) const;
    void failAiOwned(const juce::String&);
    AiProposalPhase aiPhase = AiProposalPhase::idle;
    bool aiApplying = false;
    bool aiHistoryRejected = false;
    // Control-only ownership, retained until every AI gesture end callback returns.
    std::array<bool,parameterCount> aiGestureReserved {};
    struct DocumentAction;
    struct AiDocumentAction;
    struct AtomicMetadata
    {
        std::array<std::atomic<std::uint8_t>,78> layout {};
        std::array<std::atomic<std::uint32_t>,8> keys {};
        std::array<std::atomic<float>,8> amounts {};
        std::array<std::atomic<bool>,parameterCount> controls {};
        std::array<std::atomic<bool>,4> synths {};
        std::array<std::atomic<bool>,8> effects {};
        std::array<std::array<std::atomic<AlgorithmRevision>,2>,4> synthRevisions {};
        std::array<std::array<std::atomic<AlgorithmRevision>,effectBankCount>,8> effectRevisions {};
        void store(const SoundAudioState&) noexcept;
        void load(SoundAudioState&) const noexcept;
    } metadata;
    struct AtomicPattern
    {
        struct Note {std::atomic<double> start{0},duration{.25};std::atomic<std::uint8_t> pitch{60},velocity{100},channel{1};};
        std::array<Note,maximumPatternNotes> notes {};
        std::atomic<std::uint8_t> numerator{4},denominator{4},bars{1},grid{2},mode{0};
        std::atomic<double> swing{0};
        std::atomic<std::uint16_t> count{0};
        void store(const ProjectDocument&) noexcept;
        void load(AudioProjectState&) const noexcept;
    } patternMetadata;
    bool readProject(ProjectDocument&,std::uint64_t&,std::uint64_t&,std::uint64_t&,std::uint64_t&,std::uint64_t* modeReset=nullptr,bool includeGroup=true) const noexcept;
    bool readAudioProject(AudioProjectState&,std::uint64_t&,std::uint64_t&,std::uint64_t&,std::uint64_t&,std::uint64_t* modeReset=nullptr,DrumAudioState* drums=nullptr) const noexcept;
    bool tryProjectSnapshotOwned(ProjectDocument&,std::uint64_t&) const noexcept;
    bool tryProjectSnapshotOwned(ProjectDocument&,std::uint64_t&,std::uint64_t&) const noexcept;
    bool trySoundSnapshotOwned(SoundDocument&,std::uint64_t&) const noexcept;
    bool trySoundSnapshotOwned(SoundDocument&,std::uint64_t&,std::uint64_t&) const noexcept;
    static constexpr std::size_t guardedGestureCapacity=rackUnitParameterCapacity(NativeModule::dynamicEq);
    static_assert(guardedGestureCapacity>=147);
    static_assert(guardedGestureCapacity>=rackUnitParameterCapacity(NativeModule::spaceReverb));
    static_assert(guardedGestureCapacity>=rackUnitParameterCapacity(NativeModule::eraReverb));
    bool normalizeGestureValues(std::span<const ParameterGestureValue>,std::span<ParameterGestureValue>) const;
    bool setParameterGestureValuesImpl(std::size_t,std::span<const ParameterGestureValue>,const SoundDocument*,std::uint64_t,std::uint64_t);
    RackCandidateStatus rackOperationsOwned(std::span<const RackOperation>,std::uint64_t,SoundDocument&,RackDifference&,SourceAssignmentTrace* trace=nullptr);
    bool publish(const ProjectDocument&,bool,bool advanceEdit=true,bool restartPattern=false,
                 const std::array<bool,parameterCount>* valueMask=nullptr,bool recall=false) noexcept;
    bool performAiEdit(const ProjectDocument&,const ProjectDocument&,bool alreadyPublished=false);
    bool performProjectEdit(const ProjectDocument&,const ProjectDocument&,bool=false,bool=false);
    bool readDocument(SoundDocument&,std::uint64_t&,std::uint64_t&,std::uint64_t&) const noexcept;
    bool publish(const SoundDocument&,bool reset,bool advanceEdit = true) noexcept;
    bool performEdit(const SoundDocument&,const SoundDocument&,bool reset = false,bool alreadyPublished = false);
    bool performSoundNameEdit(const SoundDocument&,const juce::String&);
    void handleAsyncUpdate() override;
    std::array<juce::RangedAudioParameter*, parameterCount> controls {};
    std::atomic<std::uint64_t> revision {0}, editRevision {0}, resetGeneration {0};
    // Low eight bits are four engine and four Drum choices; upper56 bits are their shared epoch.
    // One lock-free CAS publishes selectors and epoch, including change-and-return authority.
    // Independent numeric host atomics are outside this source-word contract.
    std::atomic<std::uint64_t> structuralGeneration {0};
    std::uint64_t observedStructuralGeneration=0;
    std::shared_ptr<const RackPersonalityState> controlPersonalities;
    std::atomic<double> tailSeconds {.15};
    RenderedPreview renderedPreview;
    enum class PreviewOwner : std::uint8_t {none,ai,manual,external};
    PreviewOwner previewOwner=PreviewOwner::none;
    std::uint64_t manualPreviewBytes=0;
    std::atomic<double> hostBpm {0};
    std::atomic<bool> missingTempo {false};
    mutable std::mutex restoreMutex;
    std::mutex drumRestoreOperationMutex;
    // Control-thread valid intent, protected by restoreMutex; never read by audio.
    // Invalid independent automation is not a usable history before-image.
    std::unique_ptr<ProjectDocument> recoveryDocument = std::make_unique<ProjectDocument>();
    // The last loaded or saved preset's folder and SESSION's preset path, protected by restoreMutex.
    juce::File presetFolder;
    juce::String presetPath;
    juce::File soundFolderOwned() const;
    std::unique_ptr<EditHistory> editHistory=std::make_unique<EditHistory>();
    struct Gesture
    {
        std::size_t index = parameterCount;
        SoundDocument before, lastValid;
        std::array<std::size_t,guardedGestureCapacity> notified {};
        std::size_t notifiedCount = 0;
        bool changed = false, cancelled = false, notifying = false, grouped = false;
    };
    // Heap-owned so sizeof(InstrumentProcessor) stays within 128 KiB as the registry grows.
    const std::unique_ptr<Gesture> gesture = std::make_unique<Gesture>();
    // Owners and the independent pump outlive the engine's raw borrowers.
    std::unique_ptr<drums::DrumOwnership> drumOwnership=std::make_unique<drums::DrumOwnership>();
    // Declared AFTER its C owner: history destroys first, after importer joined.
    std::array<AiLocalRenderObservation,3> aiLocalRenderObservations{};
    unsigned aiLocalRenderObservationCount=0;bool aiLocalRenderObservationOverflow=false;
    bool aiLocalProfileCallTrace=false;std::atomic_bool aiLocalFirstCallCompleted{false};
    std::unique_ptr<DrumControlPump> drumControlPump;
    std::unique_ptr<McpState> mcpState;
    void startMcp();void closeMcp() noexcept;void rotateMcpEpoch();
    std::unique_ptr<DrumImport> drumImporter;
    std::array<std::atomic<std::uint64_t>,64> drumTargetTokens {};
    std::uint64_t drumTargetEpoch=1;
    std::atomic<std::uint64_t> drumAcknowledgedSource {0};
    drums::PublicationIdentity renderDrumIdentity {};
    const juce::String* stagedAiPresetPath=nullptr;
    const juce::File* stagedPresetFolder=nullptr;
    // Declared before the engine so its borrowed monitor outlives destruction.
    std::shared_ptr<FxMonitor> fxMonitoring;
    RoutedSynthEngine engine;
    std::atomic<RenderQuality> preparedQuality {RenderQuality::realtime};
    std::array<std::atomic<DynamicEqProcessingMode>,8> preparedEqModes {};
    std::atomic<bool> pendingEqRouting {false};
    std::unique_ptr<PerformanceMidi> performance;
    std::unique_ptr<ObservedMidi> observedMidi;
    AudioProjectState renderProject;
    // Separate lifecycle counters retain edits coalesced back to the same
    // value between callbacks. They are read under the project revision.
    std::atomic<std::uint64_t> patternGeneration{0};
    std::uint64_t renderPatternGeneration=0;
    std::atomic<std::uint64_t> modeGeneration{0};
    std::uint64_t renderModeGeneration=0;
    std::atomic<std::uint32_t> hostMeter{0};
    std::atomic<PatternPlaybackStatus> playbackFeedback{PatternPlaybackStatus::unprepared};
    std::atomic<double> playbackPosition{0};
    double preparedRate=0,preparedFrequencyShiftTail=0,preparedMidSideSplitTail=0;
    std::array<double,2> preparedFrequencySplitTail {}; // revisions 1 and 2
    int preparedBlock=0;
    bool invalidPatternBlock=false;
    std::uint64_t renderResetGeneration = 0;
    bool prepared = false, wasPlaying = false;
    // Runtime feedback only: never part of the sound, automation or undo history.
    std::atomic<bool> loadFailed {false};
    // Render-owner cache validity; failed/contended preparation is conservative.
    bool renderSoundCurrent = false;
};
}
