#pragma once
#include "InstrumentProcessor.h"
#include "DrumPreparation.h"
#include "ComfyAudioJob.h"
#include <condition_variable>
#include <thread>

namespace airack {
enum class DrumImportPhase : std::uint8_t { closed,choosing,generating,inspecting,editing,preparing,ready,waitingPreview,auditioning,kept,error,cancelling };
enum class DrumGenerationStage : std::uint8_t { waiting,submitting,polling,downloading,inspecting,finished };
struct DrumGenerationCandidateView final {
 std::uint64_t seed=0,encodedBytes=0;
 drums::ComfyAttemptStatus status=drums::ComfyAttemptStatus::unrun;
 drums::AssetId fileId{};
 drums::DrumSourceInfo source;
 bool available=false;
};
struct DrumImportView final {
 DrumImportPhase phase=DrumImportPhase::closed;
 DrumPadTarget target;
 std::uint64_t session=0,draft=0,begin=0,end=0;
 float gain=1,preparedPeak=0;
 std::uint32_t preparedFrames=0;
 drums::DrumSourceInfo source;
 juce::String sourceName,keptName,message;
 bool inspected=false,canPrepare=false,canAudition=false,canKeep=false;
 bool generation=false;
 drums::DrumGenerationStatus generationStatus=drums::DrumGenerationStatus::invalidRequest;
 std::array<DrumGenerationCandidateView,3> candidates{};
 std::size_t selectedCandidate=0;
 std::size_t generationOrdinal=0;
 DrumGenerationStage generationStage=DrumGenerationStage::waiting;
};
// One processor-owned worker; its source and temporary audio never enter a UI callback.
class DrumImport final {
public:
#ifndef NDEBUG
 enum class TestPoint {beforeInspect,beforePrepare,afterHelper,beforeCompletion,beforeJoin,beforeGeneration,beforeCandidateInspect,afterGenerationDrain,beforeLocalRender,afterLocalRender,beforeLocalJoin,beforeLocalTerminal,beforeLocalApplyValidation,beforeLocalRootValidation};
#endif
 explicit DrumImport(InstrumentProcessor&);
 ~DrumImport();
 bool open(const DrumPadTarget&);
 bool openGeneration(const DrumPadTarget&);
 bool beginGeneration(const DrumPadTarget&,const juce::String&,const std::array<std::uint64_t,3>&,DrumCommitError&) noexcept;
 bool selectCandidate(std::uint64_t,std::size_t);
 bool busy() const;
 void choose(const juce::File&);
 void edit(std::uint64_t begin,std::uint64_t end,float gain,const juce::String& keptName);
 void prepare();
 void audition();
 bool keep();
 void stopPreview();
 void cancel();
 void closeAdmission();
 DrumImportView view() const;
 bool current(const DrumPadTarget&,std::uint64_t ticket) const;
 void poll();
private:
 friend struct InstrumentDrumTestAccess;
 friend class InstrumentProcessor;
#ifndef NDEBUG
 void (*testHook)(void*,TestPoint) noexcept=nullptr;
 void* testContext=nullptr;
 void testPoint(TestPoint point) noexcept {if(testHook)testHook(testContext,point);}
#endif
 enum class Operation { none,inspect,prepare,cancel,generate,select,localPreview };
 struct Command {
  Operation operation=Operation::none;std::uint64_t session=0,draft=0,input=0,begin=0,end=0;float gain=1;juce::File file;
  std::array<std::uint64_t,3> inputs{};std::size_t ordinal=0;DrumPadTarget target;
  std::optional<drums::ComfyAudioJobRequest> generation;
  std::unique_ptr<InstrumentProcessor::AiLocalWork> local;
 };
 struct Completion {
  Operation operation=Operation::none;std::uint64_t session=0,draft=0,input=0,ticket=0;
  drums::DrumSourceInfo source;drums::DrumPad metadata;
  std::uint32_t frames=0;float peak=0;bool success=false;
  DrumCommitError commitError=DrumCommitError::none;
  drums::PreparationError preparationError=drums::PreparationError::none;
  drums::DrumGenerationStatus generationStatus=drums::DrumGenerationStatus::invalidRequest;
  std::array<DrumGenerationCandidateView,3> candidates{};
  std::size_t ordinal=0;
  std::unique_ptr<InstrumentProcessor::AiLocalWork> local;
 };
 bool submitLocalPreview(std::unique_ptr<InstrumentProcessor::AiLocalWork>&);
 bool commandCurrent(const Command&,bool deadline=false) noexcept;
 void runGeneration(Command&,Completion&,std::array<std::unique_ptr<drums::DrumSource>,3>&,
                    std::array<std::uint64_t,3>&,std::uint64_t&);
 void run(std::stop_token);
 void enqueue(Command);
 InstrumentProcessor& processor;
 mutable std::mutex mutex;
 std::condition_variable_any wake;
 DrumImportView shown;
 Command pending;
 Operation activeOperation=Operation::none;
 std::optional<Completion> completion;
 drums::DrumPad preparedMetadata;
 std::atomic_bool cancelled{false};
 std::uint64_t inputId=0,preparedId=0,preparedDraft=0,nextSession=0;
 std::array<std::uint64_t,3> generationInputs{};
 juce::Uuid generationRuntime;
 std::uint64_t nextGeneration=0;
 drums::GenerationClock::time_point generationDeadline{};
 drums::ComfyAudioJob* activeJob=nullptr;
#ifndef NDEBUG
 std::unique_ptr<drums::ComfyAudioJob> (*testMakeJob)(void*,const juce::File&,const drums::ComfyAudioJobRequest&)=nullptr;
 void* testGenerationContext=nullptr;
#endif
 bool accepting=true,previewRequested=false;
 std::jthread worker;
};
}
