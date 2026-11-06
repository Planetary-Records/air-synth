#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "PreparedTileSkin.h"
#include "LocalPreview.h"
#include "AiCatalog.h"
#include "UserReach.h"
#include "CompanionWire.h"
#include "RackExamples.h"
#include "RoutedSynthEngine.h"
#include "InstrumentProcessor.h"
#include "DrumImport.h"
#include "DrumBundle.h"
#include "RackStack.h"
#include "TileArrayFace.h"
#include "ScreenDriver.h"
#include "CompanionRuntimeServer.h"
#include "LocalPreviewExchange.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <climits>
#include <bit>
#include <string_view>
#include <cstring>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <windows.h>
#include <psapi.h>
#endif

namespace airack {
#ifndef NDEBUG
struct RoutedDrumTestAccess {static auto snapshot(const RoutedSynthEngine& engine){return engine.drumTestSnapshot();}};
#endif
struct InstrumentDrumTestAccess {
    static void observationReset(InstrumentProcessor& p,bool enabled){const std::lock_guard lock(p.restoreMutex);
        p.aiLocalRenderObservations={};p.aiLocalRenderObservationCount=0;p.aiLocalRenderObservationOverflow=false;
        p.aiLocalProfileCallTrace=enabled;p.aiLocalFirstCallCompleted=false;}
    // Read only on the control owner, including its CRT allocation/free hook; no locking or allocation there.
    static std::uint64_t observationReserved(const InstrumentProcessor& p) noexcept {return p.drumOwnership->reservedDecoded_;}
    struct ObservationSnapshot {unsigned count=0;bool overflow=false,first=false;std::uint64_t reserved=0,work=0;
        std::array<const ai::PreviewCallTrace*,3> traces{};std::array<ai::PreviewWorkDiagnostics,3> diagnostics{};};
    static ObservationSnapshot observationSnapshot(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);ObservationSnapshot out;
        out.count=p.aiLocalRenderObservationCount;out.overflow=p.aiLocalRenderObservationOverflow;out.first=p.aiLocalFirstCallCompleted.load(std::memory_order_acquire);
        out.reserved=p.drumOwnership->reservedDecoded_;out.work=p.aiLocal?p.aiLocal->charge.work:0;
        for(unsigned n=0;n<out.count&&n<out.traces.size();++n){out.traces[n]=p.aiLocalRenderObservations[n].callTrace.get();out.diagnostics[n]=p.aiLocalRenderObservations[n].work;}return out;}
#ifndef NDEBUG
    static const ai::PreviewCallTrace* observationCompletion(DrumImport& worker){const std::lock_guard lock(worker.mutex);
        return worker.completion&&worker.completion->local?worker.completion->local->callTrace.get():nullptr;}
#endif
    static auto observationSizes(){static_assert(sizeof(InstrumentProcessor)<=128*1024);
        return std::array<std::size_t,5>{sizeof(InstrumentProcessor),sizeof(InstrumentProcessor::AiLocalWork),sizeof(ai::RenderResult),sizeof(ai::PreviewCallTrace),sizeof(InstrumentProcessor::AiLocalRenderObservation)};}
#ifndef NDEBUG
    static void hook(DrumImport& worker,void (*callback)(void*,DrumImport::TestPoint) noexcept,void* context){worker.testHook=callback;worker.testContext=context;}
#endif
    struct LoadWaitState {
        std::uint64_t request=0,job=0,ticket=0;int phase=0,pending=0,active=0,completionOperation=-1,receiptStatus=-1;
        bool local=false,rendering=false,receipt=false,receiptSent=false,retiring=false,cancel=false;
        bool pendingLocal=false,completion=false,completionLocal=false,workerCancelled=false;
    };
    static LoadWaitState loadWaitState(InstrumentProcessor& p){LoadWaitState out;
        {const std::lock_guard lock(p.restoreMutex);out.phase=int(p.aiPhase);
            if(p.aiPayloads->request)out.request=p.aiPayloads->request->tag.requestId;
            if(p.aiLocal){const auto& local=*p.aiLocal;out.local=true;out.rendering=local.rendering;out.receipt=bool(local.receipt);
                out.receiptSent=local.receiptSent;out.retiring=local.retiring;out.cancel=local.cancel.load(std::memory_order_acquire);
                out.job=local.lastJob;out.ticket=local.lastTicket;if(local.receipt)out.receiptStatus=int(local.receipt->status);}}
        // Independent existing-owner locks: this is a bounded snapshot, not a globally atomic state.
        {auto& worker=p.drumImport();const std::lock_guard lock(worker.mutex);out.pending=int(worker.pending.operation);
            out.active=int(worker.activeOperation);out.pendingLocal=bool(worker.pending.local);out.completion=bool(worker.completion);
            if(worker.completion){out.completionOperation=int(worker.completion->operation);out.completionLocal=bool(worker.completion->local);}
            out.workerCancelled=worker.cancelled.load(std::memory_order_acquire);}
        return out;}
    static std::array<std::uint64_t,7> localState(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);std::uint64_t roles=0;
        for(std::size_t n=0;n<3;++n)if(p.drumOwnership->aiImages_[n])roles|=std::uint64_t(1)<<n;
        return {roles,p.drumOwnership->retainedBytes(),p.drumOwnership->reservedDecoded_,p.aiPreviewBytes,
            p.aiLocal?p.aiLocal->charge.parent:0,p.aiLocal?p.aiLocal->charge.work:0,p.aiLocal?p.aiLocal->charge.output:0};}
// PRIVATE DRAFT ONLY. Insert these members into the EXISTING InstrumentDrumTestAccess.
// No AssetPtr escapes C. Raw addresses below are identity observations only.
struct LoadSnapshot {
    drums::DrumSlots slots;
    std::array<const drums::SampleAsset*,64> owners{};
    std::array<std::uint64_t,3> tickets{};
    std::array<drums::AssetId,3> candidates{};
    std::uint64_t ledgerPcm=0,unique=0,intendedPcm=0,intendedUnique=0;
    std::uint64_t imageMetadata=0,retained=0,actualPlayer=0;
    std::array<std::uint64_t,3> inputIds{},history{};
    std::array<std::uint64_t,64> targets{};
    std::uint64_t sourceWord=0,edit=0,reset=0;
    std::uint64_t reserved=0,legacyWorkspace=0,legacyEncoded=0;
    std::uint64_t roles=0,base=0,work=0,parent=0,output=0,player=0;
    std::uint64_t requestCounter=0,publication=0;bool applying=false;
};
static std::unique_ptr<LoadSnapshot> loadSnapshot(InstrumentProcessor& p) {
    auto result=std::make_unique<LoadSnapshot>();const std::lock_guard lock(p.restoreMutex);auto& c=*p.drumOwnership;
    result->history={p.editHistory->count,p.editHistory->cursor,p.editHistory->storedUnits};
    result->sourceWord=p.structuralGeneration.load();result->edit=p.editRevision.load();result->reset=p.resetGeneration.load();
    for(unsigned n=0;n<64;++n)result->targets[n]=p.drumTargetTokens[n].load();
    result->slots=c.current_->document->slots();result->publication=c.current_->borrowed.generation;
    for(const auto& owner:c.ledger_)if(owner){++result->unique;result->ledgerPcm+=std::uint64_t(owner->frames())*owner->channels()*sizeof(float);}
    for(std::size_t n=0;n<64;++n){const auto* owner=c.current_->owners[n].get();result->owners[n]=owner;if(!owner)continue;
        bool prior=false;for(std::size_t k=0;k<n;++k)prior|=result->owners[k]==owner;
        if(!prior){++result->intendedUnique;result->intendedPcm+=std::uint64_t(owner->frames())*owner->channels()*sizeof(float);}}
    for(std::size_t n=0;n<3;++n){const auto& slot=c.preparations_[n];result->tickets[n]=slot.ticket;if(slot.asset)result->candidates[n]=slot.asset->id();
        result->inputIds[n]=slot.inputId;
        if(c.aiImages_[n]){result->roles|=std::uint64_t(1)<<n;result->imageMetadata+=sizeof(drums::DrumOwnership::AiImage);}}
    result->reserved=c.reservedDecoded_;result->legacyWorkspace=c.reservedWorkspace_;result->legacyEncoded=c.reservedEncoded_;
    result->retained=c.retainedBytes();result->actualPlayer=p.renderedPreview.retainedPcmBytes();
    result->player=p.aiPreviewBytes;result->requestCounter=p.aiNextRequestId;result->applying=p.aiApplying;
    if(p.aiLocal){result->base=p.aiLocal->charge.base;result->work=p.aiLocal->charge.work;result->parent=p.aiLocal->charge.parent;result->output=p.aiLocal->charge.output;}
    return result;
}
static drums::AssetId loadPreparedId(InstrumentProcessor& p,std::uint64_t ticket) {
    const std::lock_guard lock(p.restoreMutex);for(const auto& slot:p.drumOwnership->preparations_)if(slot.ticket==ticket&&slot.asset)return slot.asset->id();return {};
}
static BorrowedDrumAssignments loadBorrowed(InstrumentProcessor& p) {
    const std::lock_guard lock(p.restoreMutex);BorrowedDrumAssignments result;
    for(unsigned b=0;b<4;++b)for(unsigned pad=0;pad<16;++pad)result[b][pad]=p.drumOwnership->current_->owners[b*16+pad].get();return result;
}
static void loadClearHistory(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.clearHistoryOwned();p.drainDrumFeedbackOwned();}
static bool loadReserve(InstrumentProcessor& p,std::uint64_t bytes){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->reserveDecoded(bytes);}
static void loadRelease(InstrumentProcessor& p,std::uint64_t bytes){const std::lock_guard lock(p.restoreMutex);p.drumOwnership->releaseDecoded(bytes);}
static void loadReleaseRaw(InstrumentProcessor& p,std::uint64_t input){p.releaseDrumInputWork(input);}
// Read AFTER the existing worker barrier is released but BEFORE dispatching the message/pump owner.
// Values refer to a real completed render retained in the EXISTING one completion slot.
static std::array<std::uint64_t,4> loadCompletionCapacity(DrumImport& input) {
    const std::lock_guard lock(input.mutex);if(!input.completion||input.completion->operation!=DrumImport::Operation::localPreview||!input.completion->local)return {};
    const auto& audio=input.completion->local->audio;return {audio.left.size(),audio.right.size(),audio.left.capacity(),audio.right.capacity()};
}

#ifndef NDEBUG
static auto loadEngineHits(const InstrumentProcessor& p){return RoutedDrumTestAccess::snapshot(p.engine).hits;}
#endif
// Direct real SampleAsset PCM validation, borrowed inside C's restore lock. No extra owner copies.
static bool loadPcmValid(InstrumentProcessor& p) {
    const std::lock_guard lock(p.restoreMutex);
    for(unsigned n=0;n<64;++n){const auto* owner=p.drumOwnership->current_->owners[n].get();
        if(!owner||owner->frames()!=480000||owner->channels()!=2)return false;
        const auto pcm=owner->samples();const auto marker=float(n+1)*.000001f;
        for(std::size_t f=0;f<480000;++f)
            if(pcm[2*f]!=marker*(f%31?.5f:1.f)||pcm[2*f+1]!=-marker*(f%29?.25f:.75f))return false;
    }return true;
}
#if defined(_MSC_VER) && defined(_DEBUG)
struct LoadVectorRequest {std::uint64_t payload=0,requested=0,overhead=0;long request=0;bool valid=false;};
static LoadVectorRequest loadVectorRequest(std::span<const float> samples) {
    LoadVectorRequest out;out.payload=samples.size_bytes();if(samples.empty())return out;
    void* base=const_cast<float*>(samples.data());std::size_t extent=samples.size_bytes();
    // Same pinned MSVC allocator adjustment used when these exact-size vectors are destroyed.
    if(extent>=std::_Big_allocation_threshold)std::_Adjust_manually_vector_aligned(base,extent);
    const auto use=_CrtReportBlockType(base);if(use==-1)return out;
    out.requested=_msize_dbg(base,use);if(out.requested>UINT_MAX||out.requested!=extent)return out;
    out.valid=_CrtIsMemoryBlock(base,unsigned(out.requested),&out.request,nullptr,nullptr)!=0&&out.request>0;
    out.overhead=out.requested-out.payload;return out;
}
struct LoadPcmRequests {std::uint64_t payload=0,requested=0,overhead=0,count=0;bool valid=true;};
static LoadPcmRequests loadActualPcmRequests(InstrumentProcessor& p) {
    const std::lock_guard lock(p.restoreMutex);LoadPcmRequests out;
    for(const auto& owner:p.drumOwnership->ledger_)if(owner){const auto actual=loadVectorRequest(owner->samples());
        out.valid&=actual.valid&&actual.payload==std::uint64_t(owner->frames())*owner->channels()*sizeof(float);
        out.payload+=actual.payload;out.requested+=actual.requested;out.overhead+=actual.overhead;++out.count;
    }return out;
}
#endif

static std::uint64_t loadRendererTransientAndOutputAllowance(std::uint64_t work) noexcept {
    constexpr auto control=sizeof(InstrumentProcessor::AiLocalWork)+2*sizeof(InstrumentProcessor::AssignmentStage)+sizeof(SourceAssignmentTrace)+2*65536;
    return work>=control?work-control:0;
}

};
}
using namespace airack;
namespace {
int checks=0,failures=0;
void check(bool passed,const char* expression,int line){++checks;if(!passed){++failures;std::cerr<<line<<": "<<expression<<'\n';}}
#define CHECK(...) check(bool(__VA_ARGS__),#__VA_ARGS__,__LINE__)
ai::PreviewSampleMap fullMap(){
    ai::PreviewSampleMap map;
    for(unsigned i=0;i<64;++i){ai::PreviewAssetRef ref;ref.id[31]=std::uint8_t(i+1);ref.channels=2;ref.frames=480000;map.assets.push_back(ref);
        map.pads[i].asset=std::uint8_t(i+1);map.pads[i].name=std::string(64,char('a'+i%26));map.pads[i].metadata[0]=std::uint8_t(i);}
    return map;
}
void map_codec_and_transaction(){
    auto map=fullMap();juce::MemoryBlock bytes;CHECK(ai::validPreviewMap(map));CHECK(ai::encodePreviewMap(map,bytes));CHECK(bytes.getSize()==8935);
    const auto span=[](const juce::MemoryBlock& b){return std::span(static_cast<const std::byte*>(b.getData()),b.getSize());};
    ai::PreviewSampleMap result;CHECK(ai::decodePreviewMap(span(bytes),result));CHECK(result==map);
    ai::PreviewDigest first{},second{};CHECK(ai::digestPreviewMap(map,first));CHECK(ai::digestPreviewMap(result,second));CHECK(first==second);
    for(unsigned kind=0;kind<9;++kind){auto invalid=map;
        switch(kind){case 0:invalid.assets.push_back(invalid.assets.back());break;case 1:invalid.assets[1].id=invalid.assets[0].id;break;
        case 2:invalid.assets[0].channels=0;break;case 3:invalid.assets[0].frames=480001;break;case 4:invalid.pads[0].asset=65;break;
        case 5:invalid.pads[0].name=std::string(65,'x');break;case 6:invalid.pads[0].name="bad\nname";break;case 7:invalid.pads[0].name=std::string("\xc0\xaf",2);break;case 8:invalid.pads[0].asset=0;break;}
        juce::MemoryBlock output("keep",4);const auto before=output;CHECK(!ai::encodePreviewMap(invalid,output));CHECK(output==before);}
    for(unsigned kind=0;kind<4;++kind){auto bad=bytes;
        if(kind==0)static_cast<std::uint8_t*>(bad.getData())[0]=2;
        if(kind==1)bad.append("x",1);if(kind==2)bad.setSize(bad.getSize()-1);if(kind==3)static_cast<std::uint8_t*>(bad.getData())[102]=65;
        auto output=map;CHECK(!ai::decodePreviewMap(span(bad),output));CHECK(output==map);}
    ai::PreviewSampleMap empty;CHECK(ai::encodePreviewMap(empty,bytes));CHECK(ai::decodePreviewMap(span(bytes),result));CHECK(result==empty);
}
void grouped_source_stage(){
    auto source=fullMap();for(auto& bank:source.banks){bank.rack=juce::Uuid();bank.generation=7;}
    SourceAssignmentTrace trace;trace.count=2;
    for(std::uint8_t bank=0;bank<2;++bank){auto& e=trace.effects[bank];e.kind=SourceAssignmentEffectKind::copy;e.bank=bank;e.copyFrom=1-bank;
        e.beforeOwner=e.afterOwner=source.banks[bank].rack;e.operation=1;}
    ai::PreviewSampleMap output;CHECK(ai::stagePreviewMap(source,trace,output));
    CHECK(output.banks[0].generation==0&&output.banks[1].generation==0);
    CHECK(output.banks[2].generation==7&&output.banks[3].generation==7);
    for(unsigned i=0;i<16;++i){CHECK(output.pads[i]==source.pads[16+i]);CHECK(output.pads[16+i]==source.pads[i]);}
    trace.count=1;trace.effects[0].kind=SourceAssignmentEffectKind::clear;CHECK(ai::stagePreviewMap(source,trace,output));
    CHECK(output.assets.size()==48);for(unsigned i=0;i<16;++i)CHECK(output.pads[i].asset==0&&output.pads[i].name.empty());
    CHECK(output.banks[0].generation==0);
    trace.effects[0].kind=SourceAssignmentEffectKind::preserve;CHECK(ai::stagePreviewMap(source,trace,output));
    CHECK(output==source);
    const auto before=output;trace.effects[0].beforeOwner=juce::Uuid();CHECK(!ai::stagePreviewMap(source,trace,output));CHECK(output==before);
}
ProjectDocument drumProject(){
    ProjectDocument project;project.sound=newInstanceSound();RackOperation op;op.kind=RackOperationKind::place;op.newInstance=juce::Uuid();op.design=airack::test::frozenTileArrayRackDesign();
    SoundDocument candidate;RackDifference difference;CHECK(makeRackCandidate(project.sound,op,SoundEditOrigin::manual,candidate,difference)==RackCandidateStatus::ready);
    project.sound=std::move(candidate);CHECK(validateProjectDocument(project));CHECK(sourceModule(project.sound,0)==NativeModule::drum);return project;
}
void borrowed_renderer(){
    const auto project=drumProject();ai::AuditionContext context;context.phrase.noteCount=1;context.phrase.notes[0]={0,.125,36,96,1};
    CHECK(ai::renderPreview(project,context).status==ai::RenderStatus::invalidInput);
    BorrowedDrumAssignments assignments;auto silent=ai::renderPreview(project,context,assignments,{},64);CHECK(silent.status==ai::RenderStatus::ready);
    CHECK(!silent.audio.left.empty()&&std::all_of(silent.audio.left.begin(),silent.audio.left.end(),[](float n){return n==0;}));
    std::vector<float> pcm(4800);for(std::size_t i=0;i<pcm.size();++i)pcm[i]=float(.1*std::exp(-double(i)/700)*std::sin(double(i)*.1));
    drums::AssetPtr asset;drums::AssetError error;CHECK(drums::makeSampleAsset(pcm,1,asset,error));if(!asset)return;assignments[0][0]=asset.get();
    const auto a=ai::renderPreview(project,context,assignments,{},64);const auto b=ai::renderPreview(project,context,assignments,{},512);
    CHECK(a.status==ai::RenderStatus::ready&&b.status==ai::RenderStatus::ready);CHECK(a.audio.left==b.audio.left&&a.audio.right==b.audio.right);
    CHECK(a.audio.left.size()==a.audio.bodyFrames+ai::previewTailFrames);CHECK(a.measurements.peak>0);
}
void receipt_and_context(){
    ai::LocalGenerationRequest input;input.request.tag={juce::Uuid(),1,2,3};input.local.root=input.request.tag;
    CHECK(ai::digestPreviewMap(input.local.sourceMap,input.local.sourceMapDigest));CHECK(ai::validLocalPreviewContext(input));
    auto altered=input;altered.local.sourceMapDigest[0]^=1;CHECK(!ai::validLocalPreviewContext(altered));
    ai::PreviewReceipt receipt;auto& binding=receipt.binding;binding.request=binding.root=input.request.tag;binding.job=1;binding.ticket=1;binding.providerAttempt=binding.renderOrdinal=1;
    receipt.status=ai::RenderStatus::ready;receipt.bodyFrames=1;receipt.frames=1+ai::previewTailFrames;CHECK(ai::validPreviewReceipt(receipt));CHECK(ai::samePreviewReceipt(receipt,receipt));
    auto other=receipt;other.measurements.rms=std::numeric_limits<double>::quiet_NaN();CHECK(!ai::validPreviewReceipt(other));
    other=receipt;other.failure=ai::PreviewFailure::missingOwner;CHECK(!ai::validPreviewReceipt(other));
    other=receipt;other.frames++;CHECK(!ai::validPreviewReceipt(other));other=receipt;other.binding.ticket=0;CHECK(!ai::validPreviewReceipt(other));
    other=receipt;other.status=ai::RenderStatus::nonFinite;other.measurements.finite=false;other.measurements.rms=std::numeric_limits<double>::quiet_NaN();
    CHECK(ai::validPreviewReceipt(other));CHECK(ai::samePreviewReceipt(other,other));
    ai::AuditionContext context;ai::PreviewDigest a{},b{};CHECK(ai::digestPreviewContext(context,a));context.bpm=121;CHECK(ai::digestPreviewContext(context,b));CHECK(a!=b);
}
void local_wire_and_real_audio_join(){
    using namespace ai;using namespace companion;
    LocalGenerationRequest input;auto& request=input.request;request.source=drumProject();request.tag={juce::Uuid(),7,2,3};
    request.model="test-model";request.instruction="Keep the owned drum kit";input.local.root=request.tag;
    for(const auto& instance:request.source.sound.personalities->instances)for(const auto& unit:instance.design.units)
        if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(instance,unit.id))input.local.sourceMap.banks[binding->slot]={instance.id,unit.id,1};
    CHECK(digestPreviewMap(input.local.sourceMap,input.local.sourceMapDigest));CHECK(validLocalPreviewContext(input));
    const auto bytes=[](const juce::MemoryBlock& b){return std::span(static_cast<const std::byte*>(b.getData()),b.getSize());};
    juce::MemoryBlock encoded("keep",4),sentinel=encoded;CHECK(!encodeRequest(request,encoded));CHECK(encoded==sentinel);
    CHECK(encodeLocalRequest(input,encoded));LocalGenerationRequest decoded;CHECK(decodeLocalRequest(bytes(encoded),decoded));
    CHECK(decoded.request.tag==request.tag&&decoded.request.source==request.source&&decoded.local==input.local);
    juce::MemoryBlock repeated;CHECK(encodeLocalRequest(decoded,repeated));CHECK(repeated==encoded);
    for(unsigned kind=0;kind<4;++kind){auto bad=encoded;
        if(kind==0)static_cast<std::uint8_t*>(bad.getData())[0]=2;if(kind==1)bad.append("x",1);
        if(kind==2)static_cast<std::uint8_t*>(bad.getData())[2]^=1;if(kind==3)static_cast<std::uint8_t*>(bad.getData())[bad.getSize()-1]^=1;
        auto destination=decoded;CHECK(!decodeLocalRequest(bytes(bad),destination));juce::MemoryBlock preserved;CHECK(encodeLocalRequest(destination,preserved));CHECK(preserved==encoded);}
    auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();ProposalEdits edits;
    CHECK(admitProposal(request.source,request.policy,edits,*candidate,trace.get())==ProposalStatus::accepted);
    const auto context=previewContextFor(request,{},&input.local.sourceMap,&candidate->sound);BorrowedDrumAssignments samples;
    auto rendered=renderPreview(*candidate,context,samples);CHECK(rendered.status==RenderStatus::ready);if(rendered.status!=RenderStatus::ready)return;
    LocalGenerationResult local;auto& numeric=local.numeric;numeric.tag=request.tag;numeric.status=GenerationStatus::ready;numeric.proposalStatus=ProposalStatus::accepted;
    numeric.renderStatus=RenderStatus::ready;numeric.providerStatus=rack::oauth::Status::connected;numeric.httpStatus=200;numeric.inferenceCalls=1;
    numeric.candidate=*candidate;numeric.measurements=rendered.measurements;
    PreviewReceipt receipt;auto& binding=receipt.binding;binding.request=request.tag;binding.root=input.local.root;
    binding.job=binding.ticket=1;binding.providerAttempt=binding.renderOrdinal=1;binding.sourceMap=input.local.sourceMapDigest;
    PreviewSampleMap staged;CHECK(stagePreviewMap(input.local.sourceMap,*trace,staged));CHECK(digestPreviewMap(staged,binding.stagedMap));
    CHECK(digestPreviewCandidate(*candidate,binding.candidate));CHECK(digestPreviewContext(context,binding.context));
    receipt.status=rendered.status;receipt.measurements=rendered.measurements;receipt.bodyFrames=std::uint32_t(rendered.audio.bodyFrames);
    receipt.frames=std::uint32_t(rendered.audio.left.size());receipt.phraseTruncated=rendered.audio.phraseTruncated;local.rendered=receipt;
    bool allocationFailed=true;CHECK(validateLocalGenerationResult(input,local,&allocationFailed)&&!allocationFailed);CHECK(!encodeResult(request,numeric,sentinel));
    PreviewEvaluate evaluation{binding,edits,context},evaluationRead;CHECK(encodePreviewEvaluate(evaluation,repeated));CHECK(decodePreviewEvaluate(bytes(repeated),evaluationRead));
    CHECK(evaluationRead.binding==binding&&samePreviewContext(evaluationRead.context,context));
    CHECK(encodePreviewReceipt(receipt,repeated));PreviewReceipt receiptRead;CHECK(decodePreviewReceipt(bytes(repeated),receiptRead));CHECK(samePreviewReceipt(receipt,receiptRead));
    CHECK(encodeLocalResult(input,local,repeated));LocalGenerationResult result;CHECK(decodeLocalResult(bytes(repeated),input,result));
    RequestTag tag;CHECK(peekLocalResultTag(bytes(repeated),tag)&&tag==request.tag);CHECK(result.numeric.audio.left.empty()&&result.numeric.audio.right.empty());
    auto forged=result;forged.rendered->binding.context[0]^=1;allocationFailed=true;CHECK(!validateLocalGenerationResult(input,forged,&allocationFailed)&&!allocationFailed);
    GenerationResult joined;auto foreign=receipt;foreign.binding.ticket++;const auto retainedFrames=rendered.audio.left.size();
    CHECK(!joinLocalPreview(input,result,foreign,std::move(rendered.audio),joined));CHECK(rendered.audio.left.size()==retainedFrames&&joined.status==GenerationStatus::invalidRequest);
    CHECK(joinLocalPreview(input,result,receipt,std::move(rendered.audio),joined));CHECK(joined.audio.left.size()==retainedFrames&&rendered.audio.left.empty());CHECK(validateGenerationResult(request,joined));
    Frame frame{MessageKind::generateLocal,request.tag.runtime,encoded};CHECK(encodeFrame(frame,repeated));Frame read;CHECK(decodeFrame(bytes(repeated),read));
    auto older=repeated;static_cast<std::uint8_t*>(older.getData())[4]=8;FrameHeader header;CHECK(!decodeFrameHeader(bytes(older).first(wireHeaderBytes),header));
    auto publication=std::make_unique<GroupCommand>();publication->operationId=1;publication->action=GroupAction::admit;publication->runtime=request.tag.runtime;publication->loadRevision=1;
    publication->publication.emplace();auto& published=*publication->publication;published.runtime=publication->runtime;published.loadRevision=1;published.editRevision=request.tag.editRevision;published.project=request.source;
    GroupIntent intent;intent.group=juce::Uuid();intent.member=juce::Uuid();CHECK(setGroupText("Local codec",intent.name));published.project.group=intent;
    CHECK(encodeLocalGroupPublication(*publication,input.local.sourceMap,repeated));auto publicationRead=std::make_unique<GroupCommand>();PreviewSampleMap publicationMap;
    CHECK(decodeLocalGroupPublication(bytes(repeated),*publicationRead,publicationMap));CHECK(*publicationRead==*publication&&publicationMap==input.local.sourceMap);
    auto unrelated=input.local.sourceMap;unrelated.banks[0].rack=juce::Uuid();sentinel=juce::MemoryBlock("keep",4);
    CHECK(!encodeLocalGroupPublication(*publication,unrelated,sentinel));CHECK(sentinel==juce::MemoryBlock("keep",4));
    auto wrongLogical=repeated;auto* raw=static_cast<std::uint8_t*>(wrongLogical.getData());std::size_t nested=0;for(unsigned i=0;i<4;++i)nested|=std::size_t(raw[2+i])<<(8*i);
    const auto firstRack=2+4+nested+4+2;CHECK(firstRack<wrongLogical.getSize());if(firstRack<wrongLogical.getSize())raw[firstRack]^=1;
    CHECK(!decodeLocalGroupPublication(bytes(wrongLogical),*publicationRead,publicationMap));CHECK(*publicationRead==*publication&&publicationMap==input.local.sourceMap);
    ControlMessage hello{{request.tag.runtime,0,0,0},RuntimeStatus::ready,knownCapabilityMask};CHECK(encodeControl(hello,repeated));ControlMessage welcomed;
    CHECK(decodeControl(bytes(repeated),welcomed)&&welcomed.capabilities==3);hello.capabilities=4;sentinel=juce::MemoryBlock("keep",4);CHECK(!encodeControl(hello,sentinel));CHECK(sentinel==juce::MemoryBlock("keep",4));
}
void owned_default_audition(){
    using namespace ai;auto input=std::make_unique<LocalGenerationRequest>();auto& request=input->request;
    request.source=drumProject();request.tag={juce::Uuid(),2,7,11};request.model="test-model";request.instruction="Keep the assigned drum";
    input->local.root=request.tag;input->local.root.requestId=1;
    const std::array<RackDesign,1> designs{airack::test::frozenTileArrayRackDesign()};RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(designs,{}, {},snapshot));
    juce::var inventory;CHECK(catalogInventoryFor(snapshot,CatalogRendering::ownedLocal,inventory));auto authority=std::make_shared<CatalogAuthority>();
    authority->root=input->local.root;authority->snapshot=snapshot.id;authority->rendering=CatalogRendering::ownedLocal;authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();
    request.catalog.emplace();auto& catalog=*request.catalog;catalog.phase=CatalogPhase::materialize;catalog.root=input->local.root;catalog.selectionParent=input->local.root;
    catalog.authority=authority;catalog.inferenceCallsUsed=1;catalog.providerStatus=rack::oauth::Status::connected;catalog.httpStatus=200;input->local.catalogSnapshot=snapshot.id;
    for(const auto& instance:request.source.sound.personalities->instances)for(const auto& unit:instance.design.units)
        if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(instance,unit.id))input->local.sourceMap.banks[binding->slot]={instance.id,unit.id,1};
    std::vector<float> pcm(4800);for(std::size_t n=0;n<pcm.size();++n)pcm[n]=float(.1*std::exp(-double(n)/700)*std::sin(double(n)*.1));
    drums::AssetPtr asset;drums::AssetError error;CHECK(drums::makeSampleAsset(pcm,1,asset,error));if(!asset)return;
    input->local.sourceMap.assets.push_back({asset->id(),1,asset->frames()});input->local.sourceMap.pads[0].asset=1;
    CHECK(digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest));CHECK(validLocalPreviewContext(*input));
    juce::MemoryBlock encoded;CHECK(companion::encodeLocalRequest(*input,encoded));unsigned calls=0,renders=0;
    const auto result=generateLocalProposal(*input,[&](const auto&,auto,const auto&){++calls;return rack::oauth::StructuredResult{};},
        [&](const auto& candidate,const auto& edits,const auto& context,const auto& trace,const auto& stop){++renders;
            CHECK(context.phraseSource==PhraseSource::defaultNotes);CHECK(context.phrase.notes[0].pitch==36);
            BorrowedDrumAssignments owners;owners[0][0]=asset.get();auto rendered=renderPreview(candidate,context,owners,stop);CHECK(rendered.status==RenderStatus::ready);CHECK(rendered.measurements.peak>0);
            PreviewReceipt receipt;auto& b=receipt.binding;b.request=request.tag;b.root=input->local.root;b.catalogSnapshot=snapshot.id;b.job=b.ticket=1;
            b.phase=CatalogPhase::materialize;b.providerAttempt=b.renderOrdinal=1;b.sourceMap=input->local.sourceMapDigest;
            PreviewSampleMap staged;CHECK(stagePreviewMap(input->local.sourceMap,trace,staged));CHECK(digestPreviewMap(staged,b.stagedMap));CHECK(digestPreviewCandidate(candidate,b.candidate));CHECK(digestPreviewContext(context,b.context));
            receipt.status=rendered.status;receipt.measurements=rendered.measurements;receipt.bodyFrames=std::uint32_t(rendered.audio.bodyFrames);receipt.frames=std::uint32_t(rendered.audio.left.size());receipt.phraseTruncated=rendered.audio.phraseTruncated;return receipt;
        });
    CHECK(calls==0&&renders==1);CHECK(result.numeric.status==GenerationStatus::ready&&result.rendered);
}
void owned_default_context_mapping(){
    using namespace ai;auto request=std::make_unique<GenerationRequest>();request->source=drumProject();
    PreviewSampleMap root;for(const auto& instance:request->source.sound.personalities->instances)for(const auto& unit:instance.design.units)
        if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(instance,unit.id))root.banks[binding->slot]={instance.id,unit.id,7};
    PreviewAssetRef asset;asset.id[0]=1;asset.channels=1;asset.frames=4800;root.assets.push_back(asset);root.pads[0].asset=1;CHECK(validPreviewMap(root));
    const auto legacy=previewContextFor(*request);CHECK(legacy.phrase.notes[0].pitch==60&&legacy.phrase.noteCount==4);
    auto context=previewContextFor(*request,{},&root,&request->source.sound);CHECK(context.phrase.noteCount==1&&context.phrase.notes[0].pitch==36);
    const auto original=request->source.sound.personalities->instances.front().id;RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=original;duplicate.newInstance=juce::Uuid();duplicate.destination.position=1;
    auto copied=std::make_unique<SoundDocument>();RackDifference difference;auto trace=std::make_unique<SourceAssignmentTrace>();
    CHECK(makeRackCandidate(request->source.sound,duplicate,SoundEditOrigin::manual,*copied,difference,trace.get())==RackCandidateStatus::ready);
    PreviewSampleMap copyMap;CHECK(stagePreviewMap(root,*trace,copyMap));CHECK(copyMap.pads[16].asset!=0&&copyMap.banks[1].generation==0);
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=original;auto after=std::make_unique<SoundDocument>();
    CHECK(makeRackCandidate(*copied,remove,SoundEditOrigin::manual,*after,difference,trace.get())==RackCandidateStatus::ready);
    PreviewSampleMap removedMap;CHECK(stagePreviewMap(root,*trace,removedMap));CHECK(removedMap.pads[0].asset!=0&&removedMap.pads[16].asset!=0);
    RackOperation fresh;fresh.kind=RackOperationKind::place;fresh.newInstance=juce::Uuid();fresh.allowSlotReuse=true;fresh.allocation={{0,0}};
    fresh.design=RackDesign{};fresh.design->name="Fresh analog source";fresh.design->panel.sections={"Source"};fresh.design->units={{0,NativeModule::analog,{}}};
    auto replaced=std::make_unique<SoundDocument>();CHECK(makeRackCandidate(*after,fresh,SoundEditOrigin::manual,*replaced,difference,trace.get())==RackCandidateStatus::ready);after=std::move(replaced);
    PreviewSampleMap finalMap;CHECK(stagePreviewMap(root,*trace,finalMap));CHECK(finalMap.pads[0].asset==0&&finalMap.pads[16].asset!=0);
    after->values[drumParameterStart+drumParametersPerSlot+7]=77.f/127.f;
    context=previewContextFor(*request,{},&finalMap,after.get());CHECK(context.phrase.noteCount==1&&context.phrase.notes[0].pitch==77);
    PatternDocument explicitPhrase;explicitPhrase.noteCount=1;explicitPhrase.notes[0]={.5,.25,42,100,2};
    request->capturedPhrase=explicitPhrase;context=previewContextFor(*request,{},&finalMap,after.get());CHECK(context.phrase==explicitPhrase&&context.phraseSource==PhraseSource::captured);
    PatternDocument explicitEmpty;context=previewContextFor(*request,explicitEmpty,&finalMap,after.get());CHECK(context.phrase==explicitEmpty&&context.phraseSource==PhraseSource::proposed);
    RackOperation removeMelodic;removeMelodic.kind=RackOperationKind::remove;removeMelodic.selected=fresh.newInstance;
    auto drumOnly=std::make_unique<SoundDocument>();auto removalTrace=std::make_unique<SourceAssignmentTrace>();
    CHECK(makeRackCandidate(*after,removeMelodic,SoundEditOrigin::manual,*drumOnly,difference,removalTrace.get())==RackCandidateStatus::ready);
    request->capturedPhrase.reset();PreviewSampleMap empty;CHECK(stagePreviewMap(finalMap,*removalTrace,empty));empty.assets.clear();for(auto& pad:empty.pads)pad.asset=0;
    context=previewContextFor(*request,{},&empty,drumOnly.get());CHECK(context.phrase.noteCount==4&&context.phrase.notes[0].pitch==77);
    ProjectDocument silent;silent.sound=*drumOnly;BorrowedDrumAssignments none;auto rendered=renderPreview(silent,context,none);
    CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.peak==0);
}
struct OwnedPreviewService {
    std::atomic_bool stop{false},protocolFailure{false},workerRetained{false};
    std::atomic_uint providerCalls{0},renderCalls{0},generationCalls{0},terminalPcmFrames{0},selectionTerminals{0},materializeCalls{0},materializeProviders{0},materializeRenders{0};
    std::atomic_uint terminalEncodesStarted{0},terminalEncodesCompleted{0},terminalEncodingFailures{0};
    struct LoadEncodeTiming {std::atomic<std::uint64_t> start{0},end{0};std::atomic_int result{-1};};
    std::atomic_bool observeLoadTiming{false};std::array<LoadEncodeTiming,2> loadEncodes;
    void printLoadEncodes(const char* point) const {
        for(std::size_t i=0;i<loadEncodes.size();++i){const auto& t=loadEncodes[i];
            std::cout<<"LOCAL_LOAD_ENCODE point="<<point<<" request="<<(i?4:1)<<" start_ns="<<t.start.load()
                <<" end_ns="<<t.end.load()<<" result="<<t.result.load()<<'\n';}
    }
    juce::String suffix=juce::Uuid().toString().removeCharacters("-");
    std::atomic_bool captureRelease{true},captureArrived{false};
    std::string structuredResponse;
    companion::RuntimeServer server;companion::CompanionClient fixtureClient;std::thread io;
    explicit OwnedPreviewService(std::string response={}):structuredResponse(std::move(response)),server([this](const auto& job){return run(job);},[](const auto& job){if(job.previewExchange)job.previewExchange->cancel();}),
        io([this]{const auto result=server.run(stop,suffix);workerRetained=result.workerRetained;}){
        // Keep the fixture available while full-kit preparation precedes the processor's first connection.
        CHECK(fixtureClient.start(options()));const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(companion::clientStartupTimeoutMs);
        do{fixtureClient.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        while(fixtureClient.status().phase==companion::ClientPhase::connecting&&std::chrono::steady_clock::now()<end);
        CHECK(fixtureClient.status().phase==companion::ClientPhase::ready);
    }
    ~OwnedPreviewService(){stop=true;fixtureClient.close();if(io.joinable())io.join();}
    companion::ClientOptions options() const {companion::ClientOptions value;value.allowLaunch=false;value.negotiateSpectrum=false;
        value.launch.pipeSuffix=suffix;value.launch.fixtureRoot=juce::File::getSpecialLocation(juce::File::tempDirectory);return value;}
    companion::Frame run(const companion::RuntimeJob& job){using namespace ai;using namespace companion;
        if(std::holds_alternative<AccountCommand>(job.input)){AccountReply result;result.providerStatus=rack::oauth::Status::connected;result.selectedModel="local-test-model";
            result.models={{"local-test-model","Local Test Model"}};Frame frame{MessageKind::accountResult,job.session,{}};if(!encodeAccountReply(result,frame.payload))protocolFailure=true;return frame;}
        ++generationCalls;const auto& request=std::get<GenerationRequest>(job.input);
        if(request.instruction=="Capture held"){captureArrived=true;while(!captureRelease&&!stop&&!job.cancellation->cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        const bool materialize=request.catalog&&request.catalog->phase==CatalogPhase::materialize;if(materialize)++materializeCalls;
        if(!job.localPreview||!job.previewExchange){protocolFailure=true;Frame frame{MessageKind::failure,job.session,{}};encodeControl({request.tag,RuntimeStatus::invalidMessage},frame.payload);return frame;}
        auto input=std::make_unique<LocalGenerationRequest>();input->request=request;input->local=*job.localPreview;unsigned attempts=0;std::uint8_t ordinal=0;
        auto result=std::unique_ptr<LocalGenerationResult>(new LocalGenerationResult(generateLocalProposal(*input,
            [&](const auto&,auto,const auto&){++providerCalls;++attempts;if(materialize)++materializeProviders;rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;
                reply.text=structuredResponse.empty()?juce::String(R"({"version":1,"parameter_edits":[],"rack_edits":[],"macro_edits":[],"pattern":{"bars":1,"meter_numerator":4,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[{"start_beats":0,"duration_beats":0.125,"pitch":36,"velocity":96,"channel":1}]},"characteristics":"Owned drum phrase","explanation":"Use the existing sampled pad"})"):juce::String::fromUTF8(structuredResponse.data(),int(structuredResponse.size()));
                if(request.catalog){auto json=juce::JSON::parse(reply.text);auto* fields=json.getDynamicObject();fields->setProperty("version",2);fields->setProperty("name","Owned kit preview");
                    const auto object=[](std::initializer_list<std::pair<const char*,juce::var>> values){auto* result=new juce::DynamicObject;for(const auto& value:values)result->setProperty(value.first,value.second);return juce::var(result);};
                    fields->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>()},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));
                    fields->setProperty("catalog_selections",juce::Array<juce::var>());fields->setProperty("catalog_parameter_edits",juce::Array<juce::var>());
                    if(request.policy.scope==RequestScope::sound||request.instruction=="Retain the explicit empty phrase")fields->setProperty("pattern",juce::var());
                    else if(request.instruction=="Use an explicit empty phrase")json["pattern"].getDynamicObject()->setProperty("notes",juce::Array<juce::var>());
                    if(request.instruction.starts_with("Capture ")){
                        std::optional<std::uint8_t> bank;
                        if(request.source.sound.personalities)for(const auto& rack:request.source.sound.personalities->instances)
                            for(const auto& unit:rack.design.units)if(unit.module==NativeModule::drum)
                                if(const auto* binding=rackUnitBinding(rack,unit.id))bank=binding->slot;
                        if(!bank){protocolFailure=true;return reply;}
                        const auto gain=rackParameterIndex(NativeModule::drum,*bank,"drum.pad.16.gain_db");
                        const auto tune=rackParameterIndex(NativeModule::drum,*bank,"drum.pad.16.tune_semitones");
                        if(!gain||!tune){protocolFailure=true;return reply;}
                        if(request.policy.scope!=RequestScope::pattern)fields->setProperty("parameter_edits",juce::Array<juce::var>{
                            object({{"id",parameterRegistry()[*gain].id},{"normalized",double(request.source.sound.values[*gain])}}),
                            object({{"id",parameterRegistry()[*tune].id},{"normalized",request.instruction=="Capture refined"?.6:.55}})});
                    }
                    if(request.instruction=="Replace original physical source"||request.instruction=="Refine captured root physical source"){
                        fields->setProperty("name","Captured Physical Kit");
                        if(request.instruction=="Replace original physical source"){
                            auto descriptor=juce::JSON::parse(juce::String::fromUTF8(request.catalog->authority->descriptorJson.c_str()));
                            const auto* entries=descriptor["entries"].getArray();const auto tile=airack::test::frozenTileArrayRackDesign();
                            if(!entries){protocolFailure=true;return reply;}const auto selected=std::find_if(entries->begin(),entries->end(),[&](const auto& row){return juce::Uuid(testing::catalogRowId(row).toString())==tile.id;});
                            if(selected==entries->end()){protocolFailure=true;return reply;}
                            juce::Uuid removed=juce::Uuid::null();for(const auto& instance:request.source.sound.personalities->instances)
                                for(const auto& unit:instance.design.units)if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(instance,unit.id);binding&&binding->slot==0)removed=instance.id;
                            if(!removed.isNull())fields->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>{removed.toDashedString()}},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));
                            fields->setProperty("catalog_selections",juce::Array<juce::var>{object({{"local_id",7},{"design",testing::catalogRowId(*selected)},{"revision",testing::catalogRowRevision(*selected)},{"content_sha256",testing::catalogRowHash(*selected)},
                                {"destination",object({{"role","synth"},{"chain",int(masterChain)},{"position",0}})}})});
                        }else{
                            // Semantic references must declare their captured selection before the existing aggregate merge.
                            juce::Array<juce::var> selections;for(const auto& selected:request.catalog->baseIntent.catalogSelections)
                                selections.add(object({{"local_id",int(selected.localId)},{"design",selected.design.toDashedString()},{"revision",juce::String(std::to_string(selected.revision))},{"content_sha256",selected.contentSha256},
                                    {"destination",object({{"role",selected.destination.role==RackRole::synth?"synth":"fx"},{"chain",int(selected.destination.chain)},{"position",int(selected.destination.position)}})}}));
                            fields->setProperty("catalog_selections",selections);fields->setProperty("catalog_parameter_edits",juce::Array<juce::var>{object({{"selection",7},{"unit",0},{"key","level"},{"normalized",.61}})});
                        }
                    }
                    reply.text=juce::JSON::toString(json,true,22);
                }
                if(request.instruction.starts_with("Capture ")&&request.policy.scope!=RequestScope::sound){
                    auto json=juce::JSON::parse(reply.text);json["pattern"]["notes"].getArray()->getReference(0).getDynamicObject()->setProperty("pitch",51);
                    reply.text=juce::JSON::toString(json,true,22);
                }
                if(request.instruction=="Distinct child"){auto json=juce::JSON::parse(reply.text);
                    json["pattern"]["notes"].getArray()->getReference(0).getDynamicObject()->setProperty("start_beats",.5);
                    reply.text=juce::JSON::toString(json,true,22);}
                return reply;},
            [&](const auto& candidate,const auto& edits,const auto& context,const auto& trace,const auto& cancelled){++renderCalls;if(materialize)++materializeRenders;
                auto value=std::make_unique<PreviewEvaluate>();value->edits=edits;value->context=context;auto& b=value->binding;b.request=request.tag;b.root=input->local.root;
                b.catalogSnapshot=input->local.catalogSnapshot;b.job=job.jobId;b.structuralGeneration=request.structuralGeneration;b.reset=input->local.reset;b.sourceWord=input->local.sourceWord;b.phase=request.catalog?request.catalog->phase:CatalogPhase::select;
                b.providerAttempt=materialize?request.catalog->inferenceCallsUsed:std::uint8_t(attempts);b.renderOrdinal=++ordinal;b.sourceMap=input->local.sourceMapDigest;
                PreviewSampleMap staged;if(!stagePreviewMap(input->local.sourceMap,trace,staged)||!digestPreviewMap(staged,b.stagedMap)||!digestPreviewCandidate(candidate,b.candidate)||!digestPreviewContext(context,b.context)){protocolFailure=true;return PreviewReceipt{};}
                return job.previewExchange->evaluate(*value,cancelled);
            },[&]{return job.cancellation->cancelled();})));
        if(result->numeric.status==GenerationStatus::catalogSelectionRequired)++selectionTerminals;
        terminalPcmFrames=unsigned(result->numeric.audio.left.size()+result->numeric.audio.right.size());Frame frame{MessageKind::resultLocal,job.session,{}};
        const bool physical=request.instruction=="Replace original physical source"||request.instruction=="Refine captured root physical source";
        const auto encodingStarted=std::chrono::steady_clock::now();++terminalEncodesStarted;
        LoadEncodeTiming* loadTiming=nullptr;
        if(observeLoadTiming.load()&&(request.tag.requestId==1||request.tag.requestId==4)){
            loadTiming=&loadEncodes[request.tag.requestId==1?0:1];
            loadTiming->start=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(encodingStarted.time_since_epoch()).count());
        }
        if(physical)std::cout<<"PHYSICAL_TERMINAL_ENCODE_START request="<<request.tag.requestId<<" phase="<<int(request.catalog->phase)<<" result="<<int(result->numeric.status)
            <<" clock_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(encodingStarted.time_since_epoch()).count()<<std::endl;
        const bool encoded=encodeLocalResult(*input,*result,frame.payload);++terminalEncodesCompleted;
        if(loadTiming){loadTiming->result=encoded?1:0;loadTiming->end=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
        if(!encoded){++terminalEncodingFailures;protocolFailure=true;}
        if(physical)std::cout<<"PHYSICAL_TERMINAL_ENCODE_END request="<<request.tag.requestId<<" encoded="<<encoded<<" bytes="<<frame.payload.getSize()
            <<" elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-encodingStarted).count()<<std::endl;
        return frame;
    }
};
template<class F> bool waitOwned(F&& ready,unsigned milliseconds=10000){const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<end){if(ready())return true;juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}return ready();}
std::unique_ptr<ProjectDocument> ownedProject(InstrumentProcessor& p){auto result=std::make_unique<ProjectDocument>();std::uint64_t edit=0,reset=0;CHECK(p.tryProjectSnapshot(*result,edit,reset));return result;}
std::optional<DrumPadTarget> installOwnedHit(InstrumentProcessor& p,drums::DrumPad& metadata){
    RackOperation place;place.kind=RackOperationKind::place;place.newInstance=juce::Uuid();place.design=airack::test::frozenTileArrayRackDesign();std::uint64_t edit=0,reset=0;
    auto before=ownedProject(p);CHECK(p.tryProjectSnapshot(*before,edit,reset));const auto placed=p.applyRackOperation(place,edit);CHECK(placed==RackCandidateStatus::ready);if(placed!=RackCandidateStatus::ready)return {};
    DrumPadTarget target;DrumCommitError error;const bool captured=p.captureDrumPadTarget(place.newInstance,0,0,target,error);CHECK(captured);if(!captured)return {};
    std::vector<float> pcm(4800);for(std::size_t n=0;n<pcm.size();++n)pcm[n]=float(.1*std::exp(-double(n)/700)*std::sin(double(n)*.1));
    drums::AssetPtr expected;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,expected,assetError));if(!expected)return {};metadata.asset=expected->id();metadata.name="Owned kick";expected.reset();
    std::uint64_t ticket=0;const bool prepared=p.prepareDrumAsset(pcm,1,ticket,error);CHECK(prepared);if(!prepared)return {};
    const bool assigned=p.assignPreparedDrumPad(target,ticket,metadata,{},error);CHECK(assigned);if(!assigned)return {};return target;
}
bool connectOwnedAccount(InstrumentProcessor& p){
    CHECK(p.setAiMusicalContext({"drums","D","minor",400}));CHECK(p.requestAiAccount({companion::AccountAction::status,{}}));
    const auto ready=waitOwned([&]{p.pollAi();const auto* account=p.aiAccountReply();return account&&!p.aiWorkflowStatus().accountPending&&account->selectedModel=="local-test-model"&&!account->models.empty();});CHECK(ready);return ready;
}
void owned_plugin_workflow(){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    drums::DrumPad metadata;const auto captured=installOwnedHit(p,metadata);if(!captured)return;auto original=ownedProject(p);p.prepareToPlay(48000,512);
    if(!connectOwnedAccount(p))return;DrumCommitError error;
    AiRequestOptions options{"Use this sampled pad in a short phrase","local-test-model",{ai::RequestScope::pattern,false,false,false}};
    const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted)return;CHECK(p.aiProposalPhase()==AiProposalPhase::pending);CHECK(!p.startAiPreview()&&!p.applyAiProposal());
    const auto finished=waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000);CHECK(finished);CHECK(p.aiProposalPhase()==AiProposalPhase::proposed);const auto* proposal=p.aiProposal();
    if(!proposal){std::cerr<<"Owned workflow status: "<<p.aiWorkflowStatus().message<<'\n';return;}
    CHECK(proposal->status==ai::GenerationStatus::ready&&proposal->inferenceCalls==1&&proposal->measurements.peak>0);CHECK(!proposal->audio.left.empty());
    CHECK(service.providerCalls==1&&service.renderCalls==1&&service.generationCalls==1&&!service.protocolFailure&&service.terminalPcmFrames==0);
    const auto retained=InstrumentDrumTestAccess::localState(p);CHECK(retained[0]==3&&retained[4]==proposal->audio.left.size()*8&&retained[5]==0&&retained[6]==0);
    CHECK(ownedProject(p)->pattern==original->pattern);CHECK(p.startAiPreview());const auto playing=InstrumentDrumTestAccess::localState(p);CHECK(playing[3]==retained[4]&&playing[2]>=retained[2]+playing[3]);
    juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);p.stopAiPreview();for(unsigned n=0;n<3;++n)p.processBlock(audio,midi);CHECK(p.aiPreviewStopped());
    const auto proposedPattern=proposal->candidate->pattern;CHECK(p.applyAiProposal());CHECK(ownedProject(p)->pattern==proposedPattern);CHECK(p.undoSoundEdit());CHECK(ownedProject(p)->pattern==original->pattern);CHECK(p.redoSoundEdit());CHECK(ownedProject(p)->pattern==proposedPattern);
    CHECK(waitOwned([&]{p.pollAi();return InstrumentDrumTestAccess::localState(p)[0]==0;}));
    DrumPadTarget current;DrumPadReadback readback;CHECK(p.captureDrumPadTarget(captured->rack,0,0,current,error));CHECK(p.readDrumPad(current,readback,error));CHECK(readback.metadata==metadata&&readback.frames==4800);
    juce::MemoryBlock saved;p.getStateInformation(saved);CHECK(saved.getSize()>4800*sizeof(float));p.releaseResources();processor.reset();
    auto fresh=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(fresh->restoreProjectState(saved.getData(),int(saved.getSize())));CHECK(ownedProject(*fresh)->pattern==proposedPattern);
    CHECK(fresh->captureDrumPadTarget(captured->rack,0,0,current,error));CHECK(fresh->readDrumPad(current,readback,error));CHECK(readback.metadata==metadata&&readback.frames==4800);CHECK(!service.workerRetained);
}
void owned_saturation_candidate_stale_and_apply(){
    juce::ScopedJuceInitialiser_GUI gui;CHECK(juce::ComponentPeer::getNumPeers()==0);
    const std::string response=R"({"version":2,"parameter_edits":[{"id":"effect.1.saturation.full.drive_db","normalized":0.75}],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"Saturation control fixture","explanation":"Edit the active Full drive only"})";
    OwnedPreviewService service(response);auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    auto setup=std::make_unique<ProjectDocument>();setup->sound=copperArcSound();
    {RackOperation tile;tile.design=airack::test::frozenTileArrayRackDesign();tile.newInstance=juce::Uuid();tile.destination={RackRole::synth,masterChain,1};SoundDocument next;RackDifference difference;CHECK(makeRackCandidate(setup->sound,tile,SoundEditOrigin::manual,next,difference)==RackCandidateStatus::ready);setup->sound=std::move(next);}
    for(const auto id:{"output.gain_db","output.protection"}){const auto descriptors=parameterRegistry();const auto found=std::find_if(descriptors.begin(),descriptors.end(),[&](const auto& d){return d.id==id;});CHECK(found!=descriptors.end());if(found!=descriptors.end()){const auto i=std::size_t(found-descriptors.begin());setup->sound.values[i]=p.control(i).convertTo0to1(0);}}
    juce::MemoryBlock bytes;CHECK(encodeProjectState(*setup,bytes)&&p.restoreProjectState(bytes.getData(),int(bytes.getSize())));
    auto before=ownedProject(p);std::uint64_t edit=0;CHECK(p.tryProjectSnapshot(*before,edit));
    RackOperation place;place.design=airack::test::frozenFactoryRackDesigns()[19];place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
    CHECK(p.applyRackOperation(place,edit)==RackCandidateStatus::ready);p.control(572).setValueNotifyingHost(0);
    const auto drive=rackParameterIndex(NativeModule::saturation,0,"saturation.full.drive_db");CHECK(drive);if(!drive)return;
    p.prepareToPlay(48000,64);if(!connectOwnedAccount(p))return;
    const auto original=ownedProject(p);AiRequestOptions options{"Edit the active saturation drive","local-test-model",{ai::RequestScope::sound,false,false,false}};
    for(const bool stale:{true,false}){
        const auto live=ownedProject(p);CHECK(airack::testing::generateAi(p,options));CHECK(p.aiProposalPhase()==AiProposalPhase::pending);
        const auto* request=p.aiCurrentRequest();CHECK(request&&request->source==*live&&request->catalog&&request->catalog->authority->rendering==ai::CatalogRendering::ownedLocal);
        if(request&&request->catalog){const auto descriptor=juce::JSON::parse(juce::String::fromUTF8(request->catalog->authority->descriptorJson.c_str()));const auto* entries=descriptor["entries"].getArray();bool saturation=false,drum=false;
            if(entries)for(const auto& entry:*entries){saturation|=juce::Uuid(testing::catalogRowId(entry).toString())==airack::test::frozenFactoryRackDesigns()[19].id;drum|=juce::Uuid(testing::catalogRowId(entry).toString())==airack::test::frozenTileArrayRackDesign().id;}CHECK(saturation&&drum);}
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
        CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());if(!p.aiProposal()){std::cerr<<"SATURATION_LOCAL status="<<p.aiWorkflowStatus().message<<'\n';return;}
        const auto candidate=std::make_unique<ProjectDocument>(*p.aiProposal()->candidate);CHECK(candidate->sound.values[*drive]==.75f);CHECK(*ownedProject(p)==*live);
        if(stale){
            // The real local preview uses a separate renderer: receiving it must
            // leave the initialized live instrument's samples unchanged too.
            auto reference=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(encodeProjectState(*live,bytes)&&reference->restoreProjectState(bytes.getData(),int(bytes.getSize())));reference->prepareToPlay(48000,64);
            juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),0);double error=0,energy=0;
            for(int block=0;block<16;++block){p.processBlock(a,midi);reference->processBlock(b,midi);midi.clear();for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){error=std::max(error,std::abs(double(a.getSample(ch,n))-b.getSample(ch,n)));energy+=std::abs(b.getSample(ch,n));}}
            CHECK(error==0&&energy>1e-6);
            CHECK(p.beginParameterGesture(*drive));CHECK(p.setParameterGestureValue(*drive,.25f));CHECK(p.endParameterGesture(*drive));const auto manuallyEdited=ownedProject(p);
            CHECK(manuallyEdited->sound.values[*drive]==.25f&&*manuallyEdited!=*live);CHECK(p.aiProposalPhase()==AiProposalPhase::stale);CHECK(!p.applyAiProposal());CHECK(*ownedProject(p)==*manuallyEdited);
            std::cout<<"SATURATION_LOCAL_STALE candidate_only_audio_error="<<error<<" live_energy="<<energy<<" manual_drive=0.25 apply_refused=1\n";
            CHECK(waitOwned([&]{p.pollAi();const auto connection=p.aiWorkflowStatus().connection;return connection.phase==companion::ClientPhase::ready&&connection.status==companion::RuntimeStatus::ready;}));
        }else{
            CHECK(p.applyAiProposal());CHECK(*ownedProject(p)==*candidate);CHECK(p.control(*drive).getValue()==.75f&&p.control(572).getValue()==0);
            std::cout<<"SATURATION_LOCAL_APPLY retained_drive="<<p.control(*drive).getValue()<<" full_catalog=1 actual_local_render_receipt=1\n";
        }
    }
    CHECK(original->sound.values[*drive]!=.75f);CHECK(service.providerCalls==2&&service.generationCalls==2&&service.renderCalls==2&&service.terminalPcmFrames==0&&!service.protocolFailure);
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();CHECK(!service.workerRetained);CHECK(juce::ComponentPeer::getNumPeers()==0);
    std::cout<<"SATURATION_LOCAL_COMPLETE requests="<<service.generationCalls<<" real_owned_renders="<<service.renderCalls<<" fixture_responses="<<service.providerCalls<<" external_provider_calls=0 native_peers=0 installed_client_launch=0\n";
}
void owned_catalog_refinement_phrase_authority(){
    juce::ScopedJuceInitialiser_GUI gui;
    for(const bool explicitEmpty:{false,true}){
        OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
        drums::DrumPad metadata;const auto target=installOwnedHit(p,metadata);if(!target)return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
        AiRequestOptions options{explicitEmpty?"Use an explicit empty phrase":"Keep the owned sampled kit","local-test-model",{explicitEmpty?ai::RequestScope::both:ai::RequestScope::sound,false,false,false}};
        const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted)return;
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());
        if(!p.aiProposal()){p.closeAiSession();return;}const auto parent=p.aiProposal()->tag;
        CHECK(p.aiProposal()->candidate->pattern.noteCount==0);CHECK(service.providerCalls==1&&service.renderCalls==1&&service.generationCalls==1&&!service.protocolFailure);
        if(explicitEmpty)CHECK(p.aiProposal()->audio.context.phrase.noteCount==0&&p.aiProposal()->measurements.peak==0);
        else CHECK(p.aiProposal()->audio.context.phrase.notes[0].pitch==36&&p.aiProposal()->measurements.peak>0);
        const bool started=p.refineAi(explicitEmpty?"Retain the explicit empty phrase":"Keep the owned kit again");CHECK(started);if(!started){p.closeAiSession();return;}
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
        const bool refined=p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag.requestId>parent.requestId;
        CHECK(refined);if(!refined){std::cerr<<"CATALOG_REFINE_REJECTED explicit_empty="<<explicitEmpty<<" status="<<p.aiWorkflowStatus().message<<'\n';p.closeAiSession();continue;}
        CHECK(service.providerCalls==2&&service.renderCalls==2&&service.generationCalls==2&&!service.protocolFailure&&service.terminalPcmFrames==0);
        if(explicitEmpty)CHECK(p.aiProposal()->audio.context.phrase.noteCount==0&&p.aiProposal()->measurements.peak==0);
        else CHECK(p.aiProposal()->audio.context.phrase.notes[0].pitch==36&&p.aiProposal()->measurements.peak>0);
        CHECK(p.applyAiProposal());DrumPadTarget current;DrumPadReadback readback;DrumCommitError error;
        CHECK(p.captureDrumPadTarget(target->rack,0,0,current,error));CHECK(p.readDrumPad(current,readback,error));CHECK(readback.metadata==metadata&&readback.frames==4800);
        p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();CHECK(!service.workerRetained);
    }
}
#ifndef NDEBUG
struct OwnedWorkBarrier {
    DrumImport::TestPoint point;std::atomic_bool observed{false};std::mutex mutex;std::condition_variable wake;bool released=false;
    explicit OwnedWorkBarrier(DrumImport::TestPoint value):point(value){}
    static void hook(void* context,DrumImport::TestPoint point) noexcept {auto& self=*static_cast<OwnedWorkBarrier*>(context);if(point!=self.point)return;
        self.observed=true;std::unique_lock lock(self.mutex);self.wake.wait(lock,[&]{return self.released;});}
    void release() noexcept {{const std::lock_guard lock(mutex);released=true;}wake.notify_all();}
    ~OwnedWorkBarrier(){release();}
};
void owned_lifetime_and_independent_pump(){
    juce::ScopedJuceInitialiser_GUI gui;
    for(const auto point:{DrumImport::TestPoint::beforeLocalRender,DrumImport::TestPoint::afterLocalRender}){
        OwnedWorkBarrier barrier(point);OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
        struct ReleaseBeforeJoin {OwnedWorkBarrier& barrier;~ReleaseBeforeJoin(){barrier.release();}} release{barrier};
        drums::DrumPad metadata;const auto target=installOwnedHit(p,metadata);if(!target)return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
        const auto base=InstrumentDrumTestAccess::localState(p);InstrumentDrumTestAccess::hook(p.drumImport(),OwnedWorkBarrier::hook,&barrier);
        AiRequestOptions options{"Use the existing pad","local-test-model",{ai::RequestScope::pattern,false,false,false}};const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted){barrier.release();return;}
        const bool entered=waitOwned([&]{p.pollAi();return barrier.observed.load();});CHECK(entered);if(!entered){barrier.release();return;}
        const auto held=InstrumentDrumTestAccess::localState(p);CHECK(held[0]==5&&held[5]>ai::maximumPreviewFrames&&held[2]>held[5]);
        p.closeAiSession();CHECK(p.aiProposalPhase()==AiProposalPhase::idle);CHECK(!p.startAiPreview()&&!p.applyAiProposal());
        const auto cancelled=InstrumentDrumTestAccess::localState(p);CHECK(cancelled[0]==held[0]&&cancelled[1]==held[1]&&cancelled[2]==held[2]&&cancelled[5]==held[5]);
        std::uint64_t input=0;DrumCommitError error;CHECK(!p.beginDrumInput(input,error)&&error==DrumCommitError::busy&&input==0);
        barrier.release();
        // closeAiSession stopped the AI timer: only the existing independent25ms pump returns this slot.
        CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));const auto drained=InstrumentDrumTestAccess::localState(p);
        CHECK(drained[1]==base[1]&&drained[2]==base[2]&&drained[5]==0);InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        DrumPadTarget current;CHECK(p.captureDrumPadTarget(target->rack,0,0,current,error));CHECK(p.drumImport().open(current));p.drumImport().cancel();
        CHECK(waitOwned([&]{return p.drumImport().view().phase==DrumImportPhase::closed;}));p.releaseResources();processor.reset();CHECK(!service.protocolFailure&&!service.workerRetained);
    }
}
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
// Existing accepted ownershipHeap probe, extended in the same hook with owner attribution and callback scope calibration.
namespace ownershipHeap {
struct Row {long request=0;std::size_t bytes=0;bool owner=false;};
struct Result {std::size_t live=0,peak=0,overlap=0,allocated=0,freed=0,baselineFreed=0,allocations=0,reallocations=0,ownerLive=0,ownerPeak=0,ownerOverlap=0,callbackAllocationCount=0,callbackFreeCount=0,callbackRequested=0,ledgerProbes=0,maximumLedgerProbe=0,deletionProbes=0,maximumDeletionProbe=0,activeRows=0,maximumActiveRows=0,insertionFailures=0,baselineFreeRequests=0,firstOverflowBytes=0;long firstOverflowRequest=0;bool firstOverflowOwner=false,valid=true;};
static constexpr std::size_t ledgerCapacity=131072;
static_assert(std::has_single_bit(ledgerCapacity));
static std::array<Row,ledgerCapacity> rows;
Result result;
thread_local bool callbackScope=false;
struct CallbackScope {bool previous=callbackScope;CallbackScope(){callbackScope=true;}~CallbackScope(){callbackScope=previous;}};
std::atomic<std::size_t> callbackAllocations{0},callbackFrees{0},callbackRequestedBytes{0};
std::atomic<bool> recording{false};thread_local bool inside=false;
std::atomic_flag ledgerGate=ATOMIC_FLAG_INIT;std::atomic<DWORD> ownerThread{0};unsigned observation=0;
std::atomic<std::size_t> otherAllocations{0},otherFrees{0},otherAllocatedBytes{0},otherFreedBytes{0};
struct Boundary {bool start;_CrtMemState& heap;Result& measured;bool taken=false;};
thread_local Boundary* pendingBoundary=nullptr;
std::size_t bucket(long request) noexcept {return (std::uint64_t(std::uint32_t(request))*11400714819323198485ull)>>(64-std::countr_zero(ledgerCapacity));}
Row* locate(long request,bool insertion=false) noexcept {
    const auto first=bucket(request);
    for(std::size_t probe=0;probe<rows.size();++probe){auto& row=rows[(first+probe)&(rows.size()-1)];
        ++result.ledgerProbes;result.maximumLedgerProbe=std::max(result.maximumLedgerProbe,probe+1);
        if(row.request==request)return &row;
        if(row.request==0)return insertion?&row:nullptr;
    }return nullptr;
}
Row* add(long request,std::size_t bytes,bool owner) noexcept {
    auto* row=request>0?locate(request,true):nullptr;
    if(!row||row->request!=0){result.valid=false;++result.insertionFailures;
        if(result.insertionFailures==1){result.firstOverflowRequest=request;result.firstOverflowBytes=bytes;result.firstOverflowOwner=owner;}return nullptr;}
    *row={request,bytes,owner};++result.activeRows;result.maximumActiveRows=std::max(result.maximumActiveRows,result.activeRows);return row;
}
void remove(long request,std::size_t size) noexcept {
    auto* found=locate(request);
    if(!found){result.baselineFreed+=size;++result.baselineFreeRequests;return;}
    if(found->bytes>result.live||result.activeRows==0){result.valid=false;return;}
    if(found->owner){if(found->bytes>result.ownerLive){result.valid=false;return;}result.ownerLive-=found->bytes;}
    result.live-=found->bytes;result.freed+=found->bytes;--result.activeRows;
    auto hole=std::size_t(found-rows.data());*found={};const auto mask=rows.size()-1;
    // Close the probe chain: permanent tombstones made baseline frees scan the entire recorder after long churn.
    for(std::size_t distance=1;distance<rows.size();++distance){const auto at=(std::size_t(found-rows.data())+distance)&mask;
        ++result.deletionProbes;result.maximumDeletionProbe=std::max(result.maximumDeletionProbe,distance);
        if(rows[at].request==0)break;const auto home=bucket(rows[at].request);
        if(((at-home)&mask)>=((at-hole)&mask)){rows[hole]=rows[at];rows[at]={};hole=at;}
    }
}
int __cdecl hook(int kind,void* data,std::size_t bytes,int use,long request,const unsigned char*,int){
    // The CRT invokes allocation hooks under its recursive heap lock. A zero
    // byte CRT marker lets the global snapshot and recorder transition share
    // that lock; CRT blocks are excluded from our NORMAL/CLIENT accounting.
    if(pendingBoundary&&kind==_HOOK_ALLOC&&(use&0xffff)==_CRT_BLOCK&&bytes==0){
        auto& boundary=*pendingBoundary;pendingBoundary=nullptr;
        if(boundary.start){_CrtMemCheckpoint(&boundary.heap);recording.store(true,std::memory_order_release);}
        else{
            recording.store(false,std::memory_order_release);
            while(ledgerGate.test_and_set(std::memory_order_acquire)){}
            boundary.measured=result;boundary.measured.callbackAllocationCount=callbackAllocations.load();
            boundary.measured.callbackFreeCount=callbackFrees.load();boundary.measured.callbackRequested=callbackRequestedBytes.load();
            ledgerGate.clear(std::memory_order_release);_CrtMemCheckpoint(&boundary.heap);
        }
        boundary.taken=true;return 1;
    }
    if(!recording.load(std::memory_order_acquire)||inside)return 1;inside=true;struct Exit{~Exit(){inside=false;}}exit;
    long oldRequest=0;std::size_t oldBytes=0;bool valid=true;
    // Resolve CRT block identity before the private ledger gate. The gate's
    // critical section performs only fixed-array/scalar operations.
    if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){use=_CrtReportBlockType(data);if(use==-1)valid=false;
        if(valid){oldBytes=_msize_dbg(data,use);valid=oldBytes<=UINT_MAX&&_CrtIsMemoryBlock(data,unsigned(oldBytes),&oldRequest,nullptr,nullptr);}}
    use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK)return 1;
    while(ledgerGate.test_and_set(std::memory_order_acquire)){}
    struct Unlock{~Unlock(){ledgerGate.clear(std::memory_order_release);}}unlock;
    if(!recording.load(std::memory_order_acquire))return 1;if(!valid){result.valid=false;return 1;}
    if(callbackScope){
        if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){++callbackAllocations;callbackRequestedBytes+=bytes;}
        if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC))++callbackFrees;
    }
    if(GetCurrentThreadId()!=ownerThread.load()){
        if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){++otherAllocations;otherAllocatedBytes+=bytes;}
        if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){++otherFrees;otherFreedBytes+=oldBytes;}}
    if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){
        const bool onOwner=GetCurrentThreadId()==ownerThread.load();
        if(kind==_HOOK_REALLOC){++result.reallocations;result.overlap=std::max(result.overlap,result.live+bytes);if(onOwner)result.ownerOverlap=std::max(result.ownerOverlap,result.ownerLive+bytes);if(data)remove(oldRequest,oldBytes);}
        if(!add(request,bytes,onOwner))return 1;
        if(onOwner){result.ownerLive+=bytes;result.ownerPeak=std::max(result.ownerPeak,result.ownerLive);}
        result.live+=bytes;result.allocated+=bytes;++result.allocations;result.peak=std::max(result.peak,result.live);
    }else if(kind==_HOOK_FREE&&data)remove(oldRequest,oldBytes);return 1;
}
bool takeBoundary(bool start,_CrtMemState& heap,Result& measured){
    Boundary boundary{start,heap,measured};pendingBoundary=&boundary;
    auto* marker=_malloc_dbg(0,_CRT_BLOCK,__FILE__,__LINE__);pendingBoundary=nullptr;
    if(marker)_free_dbg(marker,_CRT_BLOCK);
    CHECK(marker&&boundary.taken);return marker&&boundary.taken;
}
template<class F> Result observe(F&& work,void(*boundaryProbe)(bool,void*)=nullptr,void* probeContext=nullptr){
    rows.fill({});result={};inside=false;callbackAllocations=callbackFrees=callbackRequestedBytes=0;otherAllocations=0;otherFrees=0;otherAllocatedBytes=0;otherFreedBytes=0;ownerThread=GetCurrentThreadId();_CrtMemState before{},after{};Result out;
    const auto old=_CrtSetAllocHook(hook);
    try{
        if(!takeBoundary(true,before,out)){recording=false;_CrtSetAllocHook(old);out.valid=false;return out;}
        if(boundaryProbe)boundaryProbe(true,probeContext);
        work();
        if(!takeBoundary(false,after,out)){recording=false;_CrtSetAllocHook(old);out.valid=false;return out;}
        if(boundaryProbe)boundaryProbe(false,probeContext);
    }catch(...){recording=false;pendingBoundary=nullptr;_CrtSetAllocHook(old);throw;}
    _CrtSetAllocHook(old);
    const auto delta=std::int64_t(after.lSizes[_NORMAL_BLOCK])-std::int64_t(before.lSizes[_NORMAL_BLOCK])
        +std::int64_t(after.lSizes[_CLIENT_BLOCK])-std::int64_t(before.lSizes[_CLIENT_BLOCK]);
    std::cout<<"REQUESTED_HEAP_OBSERVATION number="<<++observation<<" live="<<out.live<<" baseline_freed="<<out.baselineFreed<<" checkpoint_delta="<<delta
        <<" callback_allocations="<<out.callbackAllocationCount<<" callback_frees="<<out.callbackFreeCount<<" callback_requested_bytes="<<out.callbackRequested
        <<" owner_live="<<out.ownerLive<<" owner_peak="<<out.ownerPeak<<" owner_realloc_overlap="<<out.ownerOverlap
        <<" peak="<<out.peak<<" ledger_probes="<<out.ledgerProbes<<" maximum_ledger_probe="<<out.maximumLedgerProbe
        <<" deletion_probes="<<out.deletionProbes<<" maximum_deletion_probe="<<out.maximumDeletionProbe<<" active_rows="<<out.activeRows<<" maximum_active_rows="<<out.maximumActiveRows
        <<" insertion_failures="<<out.insertionFailures<<" first_overflow_request="<<out.firstOverflowRequest<<" first_overflow_bytes="<<out.firstOverflowBytes<<" first_overflow_owner="<<out.firstOverflowOwner<<" baseline_free_requests="<<out.baselineFreeRequests<<" other_allocations="<<otherAllocations<<" other_frees="<<otherFrees<<" other_allocated_bytes="<<otherAllocatedBytes<<" other_freed_bytes="<<otherFreedBytes<<'\n';
    CHECK(out.valid);CHECK(std::int64_t(out.live)-std::int64_t(out.baselineFreed)==delta);return out;
}
void boundaryRegression(){
    // A real worker allocates/frees at each recorder transition. Keep the worker
    // alive outside both snapshots so its own startup/teardown is not measured.
    struct Worker {
        std::atomic_int command{0},completed{-1};void* block=nullptr;
        std::thread thread{[this]{completed=0;for(;;){const auto next=command.load();if(next==3)return;
            if(next==completed.load()){std::this_thread::yield();continue;}
            if(next==1)block=_malloc_dbg(216,_NORMAL_BLOCK,__FILE__,__LINE__);
            if(next==2){_free_dbg(block,_NORMAL_BLOCK);block=nullptr;}completed=next;}}};
        bool await(int value){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(completed.load()!=value&&std::chrono::steady_clock::now()<end)std::this_thread::yield();return completed.load()==value;}
        void perform(int value){command=value;CHECK(await(value));}
        ~Worker(){command=3;thread.join();if(block)_free_dbg(block,_NORMAL_BLOCK);}
    };
    {
        Worker worker;CHECK(worker.await(0));
        const auto measured=observe([]{},[](bool start,void* context){if(start)static_cast<Worker*>(context)->perform(1);},&worker);
        CHECK(worker.block!=nullptr&&measured.valid&&measured.live==216&&measured.allocated==216);worker.perform(2);
    }
    {
        Worker worker;CHECK(worker.await(0));
        const auto measured=observe([&]{worker.perform(1);CHECK(worker.block!=nullptr);},
            [](bool start,void* context){if(!start)static_cast<Worker*>(context)->perform(2);},&worker);
        CHECK(worker.block==nullptr&&measured.valid&&measured.live==216&&measured.allocated==216);
    }
}
void calibrate(){
    boundaryRegression();
    std::cout<<"LOCAL_LOAD_RECORDER row_capacity="<<rows.size()<<" row_bytes="<<sizeof(Row)<<" static_observer_bytes="<<sizeof(rows)<<" (test observer storage, not product C admission)\n";
    // Collision, wrap, full-table deletion and long churn precede the real CRT/checkpoint controls.
    for(const auto target:{bucket(1),rows.size()-1}){
        rows.fill({});result={};long first=0,collision=0;
        for(long request=1;request<long(rows.size()*32);++request)if(bucket(request)==target){if(!first)first=request;else{collision=request;break;}}
        CHECK(first>0&&collision>0);if(!first||!collision)continue;
        CHECK(add(first,11,true)&&add(collision,13,true));result.live=result.ownerLive=24;
        remove(first,11);auto* retained=locate(collision);CHECK(retained&&retained->bytes==13);remove(collision,13);
        CHECK(!locate(first)&&!locate(collision)&&result.live==0&&result.ownerLive==0&&result.freed==24&&result.activeRows==0&&result.valid);
    }
    rows.fill({});result={};bool filled=true;
    for(std::size_t n=0;n<rows.size();++n)filled=bool(add(long(n+1),1,false))&&filled;
    result.live=rows.size();CHECK(filled&&result.activeRows==rows.size()&&result.maximumActiveRows==rows.size());
    CHECK(!add(LONG_MAX,19,true)&&!result.valid&&result.insertionFailures==1&&result.firstOverflowRequest==LONG_MAX&&result.firstOverflowBytes==19&&result.firstOverflowOwner);
    remove(long(rows.size()/2),1);bool retained=true;
    for(std::size_t n=0;n<rows.size();++n)if(n+1!=rows.size()/2)retained=bool(locate(long(n+1)))&&retained;
    CHECK(retained&&result.activeRows==rows.size()-1);CHECK(add(LONG_MAX,1,false));++result.live;
    for(std::size_t n=0;n<rows.size();++n)if(n+1!=rows.size()/2)remove(long(n+1),1);remove(LONG_MAX,1);
    CHECK(result.live==0&&result.activeRows==0&&std::all_of(rows.begin(),rows.end(),[](const auto& row){return row.request==0;}));
    rows.fill({});result={};bool churn=true;
    for(long request=1;request<=long(rows.size()*3);++request){if(!add(request,1,false)){churn=false;break;}++result.live;remove(request,1);if(locate(request+long(rows.size()*3)))churn=false;}
    CHECK(churn&&result.valid&&result.activeRows==0&&result.maximumActiveRows==1&&result.maximumLedgerProbe==1&&result.maximumDeletionProbe==1);
    rows.fill({});result={};
    const auto a=observe([]{juce::MemoryBlock block(1024,false);});CHECK(a.peak==1024&&a.live==0&&a.freed==1024&&a.ownerPeak==1024&&a.ownerLive==0);
    auto baseline=_malloc_dbg(80,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(baseline!=nullptr);
    const auto b=observe([&]{auto p=_malloc_dbg(64,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(p!=nullptr);
        auto q=_realloc_dbg(p,128,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(q!=nullptr);_free_dbg(q?q:p,_NORMAL_BLOCK);_free_dbg(baseline,_NORMAL_BLOCK);});
    CHECK(b.ownerPeak==128&&b.ownerOverlap==192&&b.ownerLive==0);
    CHECK(b.peak==128&&b.overlap==192&&b.live==0&&b.allocated==192&&b.freed==192&&b.baselineFreed==80);
    const auto callbackPositive=observe([]{CallbackScope callback;auto p=_malloc_dbg(128,_NORMAL_BLOCK,__FILE__,__LINE__);if(p)_free_dbg(p,_NORMAL_BLOCK);});
    CHECK(callbackPositive.valid&&callbackPositive.peak==128&&callbackPositive.live==0&&callbackPositive.callbackAllocationCount==1&&callbackPositive.callbackFreeCount==1&&callbackPositive.callbackRequested==128);
    static_assert(std::_Non_user_size==47);
    for(const auto count:{1000u,960000u}){InstrumentDrumTestAccess::LoadVectorRequest actual;bool found=false,exactCapacity=false;
        const auto vectorObservation=observe([&]{std::vector<float> samples(count);exactCapacity=samples.capacity()==samples.size();
            actual=InstrumentDrumTestAccess::loadVectorRequest(samples);
            while(ledgerGate.test_and_set(std::memory_order_acquire)){}
            const auto* row=locate(actual.request);found=row&&row->bytes==actual.requested&&row->owner;
            ledgerGate.clear(std::memory_order_release);
        });
        CHECK(exactCapacity&&actual.valid&&found&&actual.payload==std::uint64_t(count)*sizeof(float));
        CHECK(actual.overhead==(actual.payload>=std::_Big_allocation_threshold?std::_Non_user_size:0));
        CHECK(vectorObservation.valid&&vectorObservation.live==0&&vectorObservation.allocated==vectorObservation.freed&&vectorObservation.peak>=actual.requested);
        std::cout<<"LOCAL_LOAD_VECTOR_CALIBRATION payload="<<actual.payload<<" crt_requested="<<actual.requested<<" alignment_metadata="<<actual.overhead<<" actual_request="<<actual.request<<" hook_match="<<found<<'\n';
    }
}
}

#endif
// PRIVATE DRAFT ONLY. Helpers for the existing DrumLocalPreviewTests anonymous namespace.
constexpr std::uint64_t loadAssetBytes=480000ull*2*sizeof(float);
constexpr std::uint64_t loadKitBytes=64*loadAssetBytes; //245760000, real PCM, not fabricated map geometry.
constexpr std::uint64_t loadOutputBytes=576000ull*2*sizeof(float);
static_assert(loadKitBytes==245760000&&loadOutputBytes==4608000);
struct LoadLease final {
    InstrumentProcessor& processor;std::uint64_t bytes=0;
    LoadLease(InstrumentProcessor& p,std::uint64_t request):processor(p){if(InstrumentDrumTestAccess::loadReserve(p,request))bytes=request;}
    ~LoadLease(){if(bytes)InstrumentDrumTestAccess::loadRelease(processor,bytes);}
    explicit operator bool()const noexcept{return bytes!=0;}
    LoadLease(const LoadLease&)=delete;
};
std::uint64_t loadEdit(InstrumentProcessor& p){std::uint64_t edit=0,reset=0;auto document=std::make_unique<ProjectDocument>();CHECK(p.tryProjectSnapshot(*document,edit,reset));return edit;}
void loadInput(std::vector<float>& input,unsigned ordinal) {
    // Every sample allocation is a full480000-frame stereo body. Quiet finite data avoids clipping repairs.
    input.resize(960000);const auto marker=float(ordinal+1)*.000001f;
    for(std::size_t f=0;f<480000;++f){input[2*f]=marker*(f%31? .5f:1.f);input[2*f+1]=-marker*(f%29? .25f:.75f);}
}
drums::DrumPad loadMetadata(drums::AssetId id,unsigned ordinal) {
    drums::DrumPad result;result.asset=id;result.name="Load pad "+std::to_string(ordinal+1);result.name.resize(64,'n');
    drums::SourceProvenance source;source.kind=drums::SourceKind::generated;source.sourceChannels=2;source.sourceFrames=480000;source.end=480000;
    source.generator=std::string(96,'g');source.condition=std::string(128,'c');source.seed=ordinal;source.promptId[31]=std::uint8_t(ordinal+1);result.source=std::move(source);return result;
}
struct LoadKit final {
    std::array<juce::Uuid,4> racks;
    std::array<std::uint64_t,3> candidates{};
    std::vector<float> input; // explicitly reported test caller input; never an ownership root.
    bool install(InstrumentProcessor& p) {
        for(unsigned bank=0;bank<4;++bank){RackOperation operation;operation.newInstance=racks[bank];operation.allowSlotReuse=true;operation.design=airack::test::frozenTileArrayRackDesign();
            operation.allocation={{0,std::uint8_t(bank)}};operation.destination={RackRole::synth,masterChain,std::uint8_t(bank)};
            const auto placed=p.applyRackOperation(operation,loadEdit(p));CHECK(placed==RackCandidateStatus::ready);if(placed!=RackCandidateStatus::ready)return false;}
        for(unsigned n=0;n<64;++n){loadInput(input,n);std::uint64_t ticket=0;DrumCommitError error;
            const bool prepared=p.prepareDrumAsset(input,2,ticket,error);CHECK(prepared);if(!prepared)return false;
            const auto id=InstrumentDrumTestAccess::loadPreparedId(p,ticket);CHECK(id!=drums::AssetId{});
            DrumPadTarget target;const bool captured=p.captureDrumPadTarget(racks[n/16],0,std::uint8_t(n%16),target,error);CHECK(captured);if(!captured)return false;
            const bool assigned=p.assignPreparedDrumPad(target,ticket,loadMetadata(id,n),{},error);CHECK(assigned);if(!assigned)return false;
        }
        InstrumentDrumTestAccess::loadClearHistory(p);const auto state=InstrumentDrumTestAccess::loadSnapshot(p);
        CHECK(state->unique==64&&state->intendedUnique==64&&state->ledgerPcm==loadKitBytes&&state->intendedPcm==loadKitBytes);
        CHECK(state->intendedPcm<=drums::intendedDecodedLimit&&state->reserved==0);return state->unique==64;
    }
    bool holdThreePrepared(InstrumentProcessor& p) {
        DrumCommitError error;for(unsigned n=0;n<3;++n){loadInput(input,64+n);const bool prepared=p.prepareDrumAsset(input,2,candidates[n],error);CHECK(prepared);if(!prepared)return false;}
        const auto state=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(state->unique==67&&state->ledgerPcm==67*loadAssetBytes&&state->tickets==candidates);
        std::uint64_t fourth=777;CHECK(!p.prepareDrumAsset(input,2,fourth,error)&&error==DrumCommitError::preparationFull&&fourth==777);return state->unique==67;
    }
    void dropCandidates(InstrumentProcessor& p){for(auto ticket:candidates)p.discardPreparedDrumAsset(ticket);candidates={};}
};
void loadPhysical(SoundDocument& sound,const juce::String& id,float value) {
    const auto registry=parameterRegistry();const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& parameter){return parameter.id==id;});CHECK(found!=registry.end());if(found==registry.end())return;
    const auto at=std::size_t(found-registry.begin());juce::NormalisableRange<float> range(found->minimum,found->maximum,found->kind==ParameterKind::continuous?0.f:1.f,found->skew);sound.values[at]=range.convertTo0to1(value);
}
bool loadMixed(InstrumentProcessor& p,const LoadKit& kit) {
    RackDesign melodic;melodic.name="Load mixed analog";melodic.panel.sections={"Tone"};melodic.units={{0,NativeModule::analog,{},legacyAlgorithmRevision}};
    RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=kit.racks[3];swap.design=melodic;swap.allowSlotReuse=true;swap.destination={RackRole::synth,masterChain,3};
    const auto swapped=p.applyRackOperation(swap,loadEdit(p));CHECK(swapped==RackCandidateStatus::ready);if(swapped!=RackCandidateStatus::ready)return false;
    CHECK(p.applyRackEdit({RackEditKind::addEffect,0,0,0,EffectKind::filter},loadEdit(p))==RackEditResult::applied);
    CHECK(p.applyRackEdit({RackEditKind::addEffect,1,masterChain,0,EffectKind::chorus},loadEdit(p))==RackEditResult::applied);
    auto sound=std::make_unique<SoundDocument>(ownedProject(p)->sound);for(unsigned b=0;b<4;++b){const auto prefix="synth."+juce::String(b+1)+".";loadPhysical(*sound,prefix+"enabled",1);loadPhysical(*sound,prefix+"level",b==3?.001f:1.f);loadPhysical(*sound,prefix+"pan",0);}
    loadPhysical(*sound,"output.gain_db",-18);
    // Pad2 duplicates Pad1; all16 pads still get a trigger, including Pad16 at51.
    sound->values[drumParameterStart+drumParametersPerPad+7]=36.f/127.f;
    CHECK(p.applySoundEdit(*sound,SoundEditOrigin::manual,loadEdit(p)));InstrumentDrumTestAccess::loadClearHistory(p);
    const auto state=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(state->intendedUnique==64&&state->ledgerPcm==loadKitBytes);return state->intendedUnique==64;
}
std::string loadPatternReply() {
    juce::Array<juce::var> notes;for(unsigned n=0;n<16;++n){auto* note=new juce::DynamicObject;note->setProperty("start_beats",double(n)*.5);note->setProperty("duration_beats",.125);
        note->setProperty("pitch",36+int(n));note->setProperty("velocity",96);note->setProperty("channel",1);notes.add(note);}
    auto* pattern=new juce::DynamicObject;pattern->setProperty("bars",4);pattern->setProperty("meter_numerator",4);pattern->setProperty("meter_denominator",4);pattern->setProperty("grid","sixteenth");pattern->setProperty("swing",0);pattern->setProperty("notes",notes);
    auto* root=new juce::DynamicObject;root->setProperty("version",1);root->setProperty("parameter_edits",juce::Array<juce::var>{});root->setProperty("rack_edits",juce::Array<juce::var>{});root->setProperty("macro_edits",juce::Array<juce::var>{});root->setProperty("pattern",pattern);
    root->setProperty("characteristics","Full owned kit");root->setProperty("explanation","Exercise all sixteen mappings");return juce::JSON::toString(root,true).toStdString();
}
template<class F> bool loadSpin(F&& ready,unsigned milliseconds=20000) {
    // Deliberately NO JUCE message dispatch: a stopped player's real Buffer must remain until the explicit ACK/clear boundary being measured.
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);do{if(ready())return true;std::this_thread::yield();}while(std::chrono::steady_clock::now()<end);return ready();
}
bool loadEqual(const ai::PreviewAudio& a,const ai::PreviewAudio& b) {
    return a.left.size()==b.left.size()&&a.right.size()==b.right.size()
        &&std::memcmp(a.left.data(),b.left.data(),a.left.size()*sizeof(float))==0
        &&std::memcmp(a.right.data(),b.right.data(),a.right.size()*sizeof(float))==0;
}

std::unique_ptr<ai::RenderResult> loadIrregularReference(const ProjectDocument& candidate,const ai::AuditionContext& context,const BorrowedDrumAssignments& borrowed) {
    auto result=std::make_unique<ai::RenderResult>();auto patch=std::make_unique<RoutedPatch>();
    if(!makeRoutedPatch(candidate.sound,*patch))return result;
    auto engine=std::make_unique<RoutedSynthEngine>();
    if(!engine->setPatch(*patch,borrowed)||!engine->prepare(48000,512,RenderQuality::realtime,eqPreparationModesFor(*patch)))return result;
    engine->reset();const auto latency=std::size_t(engine->latencySamples());
    auto scheduler=std::make_unique<PatternScheduler>();auto events=std::make_unique<PatternEventBuffer>();
    if(!scheduler->prepare(48000,512)||!scheduler->setPattern(context.phrase))return result;
    auto& audio=result->audio;audio.left.resize(576000);audio.right.resize(576000);audio.bodyFrames=384000;audio.context=context;
    juce::AudioBuffer<float> block(2,512);juce::MidiBuffer midi;midi.ensureSize(maximumPatternEvents*3);
    constexpr std::array<std::size_t,6> partitions{17,131,64,3,512,257};
    const auto renderFrames=audio.left.size()+latency;std::size_t frame=0,part=0;
    while(frame<renderFrames){const auto boundary=frame<audio.bodyFrames?audio.bodyFrames:renderFrames;
        const auto count=int(std::min(partitions[part++%partitions.size()],boundary-frame));HostPatternTiming clock;
        clock.playing=frame<audio.bodyFrames;clock.bpm=context.bpm;clock.samplePosition=std::int64_t(frame);clock.ppqPosition=double(frame)*context.bpm/(60*48000);
        const auto status=scheduler->processBlock(clock,count,PlaybackMode::internalPattern,*events);
        if(status!=(clock.playing?PatternPlaybackStatus::playing:PatternPlaybackStatus::stopped)){result->status=ai::RenderStatus::renderFailure;return result;}
        midi.clear();for(std::size_t n=0;n<events->count;++n){const auto& event=events->events[n];
            midi.addEvent(event.noteOn?juce::MidiMessage::noteOn(event.channel,event.pitch,event.velocity):juce::MidiMessage::noteOff(event.channel,event.pitch),event.sampleOffset);}
        block.setSize(2,count,false,false,true);engine->render(block,midi,context.bpm);
        const auto start=std::max(frame,latency),end=std::min(frame+std::size_t(count),renderFrames);
        if(end>start){const auto source=start-frame,destination=start-latency,length=end-start;
            std::copy_n(block.getReadPointer(0)+source,length,audio.left.begin()+destination);std::copy_n(block.getReadPointer(1)+source,length,audio.right.begin()+destination);}
        frame+=std::size_t(count);
    }
    result->quality=engine->effectiveQuality();result->latencySamples=engine->latencySamples();result->latencyCompensated=latency!=0;
    result->status=ai::measurePreview(audio.left,audio.right,audio.bodyFrames,result->measurements);return result;
}


#if defined(_MSC_VER) && defined(_DEBUG)
// Direct production renderer calibration, isolated from inference and caller audio capture.
// The owning thread's engine allocations are measured separately; service/DSP worker totals remain in the all-thread report.
std::uint64_t loadPreparedEngineOwnerPeak(const ProjectDocument& candidate,const BorrowedDrumAssignments& borrowed) {
    auto patch=std::make_unique<RoutedPatch>();const bool valid=makeRoutedPatch(candidate.sound,*patch);CHECK(valid);if(!valid)return 0;
    std::unique_ptr<RoutedSynthEngine> engine;bool prepared=false;
    const auto heap=ownershipHeap::observe([&]{engine=std::make_unique<RoutedSynthEngine>();prepared=engine->setPatch(*patch,borrowed)
        &&engine->prepare(48000,512,RenderQuality::realtime,eqPreparationModesFor(*patch));if(prepared)engine->reset();});
    CHECK(prepared);std::cout<<"LOCAL_LOAD_ENGINE_PREPARED owner_live="<<heap.ownerLive<<" owner_peak="<<heap.ownerPeak
        <<" owner_realloc_overlap="<<heap.ownerOverlap<<" all_threads_live="<<heap.live<<" all_threads_peak="<<heap.peak
        <<" (prepared engine/DSP lifetime; separate from C512 decoded/transient; other-thread I/O separately reported)\n";
    engine.reset();return std::max(heap.ownerPeak,heap.ownerOverlap);
}
void loadRendererBound(const ownershipHeap::Result& heap,std::uint64_t engineOwnerPeak,std::uint64_t transientAndOutput) {
    const auto actual=std::max(heap.ownerPeak,heap.ownerOverlap),limit=engineOwnerPeak+transientAndOutput;
    if(actual>limit)std::cerr<<"LOCAL_LOAD_RENDER_TRANSIENT_UNDERRESERVED actual_owner_peak="<<actual<<" prepared_engine_owner_peak="<<engineOwnerPeak
        <<" explicit_transient_plus_output="<<transientAndOutput<<" IPC_reserve_not_used=1\n";
    CHECK(actual<=limit);
    std::cout<<"LOCAL_LOAD_RENDER_ALLOCATION owner_peak="<<actual<<" measured_engine_prepared_peak="<<engineOwnerPeak
        <<" explicit_transient_plus_output="<<transientAndOutput<<" actual_other_threads_in_separate_totals=1\n";
}
// Declare each same-C test-oracle lease before the real returned vectors so destruction precedes release.
void loadReferencePartitions(InstrumentProcessor& p,std::uint64_t calibratedWork,const ai::GenerationResult& admitted) {
    CHECK(admitted.candidate.has_value());if(!admitted.candidate)return;
    const auto borrowed=InstrumentDrumTestAccess::loadBorrowed(p); // Current C publication stays live and unchanged throughout.
    const auto enginePeak=loadPreparedEngineOwnerPeak(*admitted.candidate,borrowed);
    const auto transientAndOutput=InstrumentDrumTestAccess::loadRendererTransientAndOutputAllowance(calibratedWork);
    CHECK(enginePeak!=0&&transientAndOutput>loadOutputBytes);
    {
        LoadLease charge(p,calibratedWork);CHECK(charge);if(!charge)return;std::unique_ptr<ai::RenderResult> result;
        const auto heap=ownershipHeap::observe([&]{result.reset(new ai::RenderResult(ai::renderPreview(*admitted.candidate,admitted.audio.context,borrowed,{},64)));});
        CHECK(result->status==ai::RenderStatus::ready&&result->audio.bodyFrames==384000&&result->audio.left.size()==576000&&loadEqual(result->audio,admitted.audio));
        CHECK(result->audio.left.capacity()==576000&&result->audio.right.capacity()==576000);loadRendererBound(heap,enginePeak,transientAndOutput);
    }
    {
        LoadLease charge(p,calibratedWork);CHECK(charge);if(!charge)return;std::unique_ptr<ai::RenderResult> result;
        const auto heap=ownershipHeap::observe([&]{result.reset(new ai::RenderResult(ai::renderPreview(*admitted.candidate,admitted.audio.context,borrowed,{},512)));});
        CHECK(result->status==ai::RenderStatus::ready&&loadEqual(result->audio,admitted.audio));loadRendererBound(heap,enginePeak,transientAndOutput);
    }
    {
        LoadLease charge(p,calibratedWork);CHECK(charge);if(!charge)return;std::unique_ptr<ai::RenderResult> result;
        const auto heap=ownershipHeap::observe([&]{result=loadIrregularReference(*admitted.candidate,admitted.audio.context,borrowed);});
        CHECK(result->status==ai::RenderStatus::ready&&loadEqual(result->audio,admitted.audio));loadRendererBound(heap,enginePeak,transientAndOutput);
    }
}
#endif

// PRIVATE SOURCE PROPOSAL: meaningful --load corpus; no run/acceptance claim.
#if defined(_MSC_VER) && defined(_DEBUG)
struct LoadWorkGate final {
    std::mutex mutex;std::condition_variable wake;
    std::atomic_uint before{0},after{0};bool allowBefore=false,allowAfter=false;
    void arm(){const std::lock_guard lock(mutex);before=after=0;allowBefore=allowAfter=false;}
    static void hook(void* raw,DrumImport::TestPoint point) noexcept {
        if(point!=DrumImport::TestPoint::beforeLocalRender&&point!=DrumImport::TestPoint::afterLocalRender)return;
        auto& gate=*static_cast<LoadWorkGate*>(raw);std::unique_lock lock(gate.mutex);
        const bool first=point==DrumImport::TestPoint::beforeLocalRender;
        if(first)++gate.before;else ++gate.after;
        gate.wake.wait(lock,[&]{return first?gate.allowBefore:gate.allowAfter;});
    }
    void releaseBefore(){const std::lock_guard lock(mutex);allowBefore=true;wake.notify_all();}
    void releaseAfter(){const std::lock_guard lock(mutex);allowAfter=true;wake.notify_all();}
    void release(){const std::lock_guard lock(mutex);allowBefore=allowAfter=true;wake.notify_all();}
    ~LoadWorkGate(){release();}
};
struct LoadMemoryView final {std::uint64_t privateBytes=0,workingSet=0,processStartPeakWorkingSet=0;};
LoadMemoryView loadMemoryView() {
    // Resolve Windows' existing process-memory query dynamically: no CMake/link dependency.
    using Query=BOOL (WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX state{};state.cb=sizeof(state);
    if(!query||!query(GetCurrentProcess(),reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&state),sizeof(state)))return {};
    return {state.PrivateUsage,state.WorkingSetSize,state.PeakWorkingSetSize};
}
void loadReport(InstrumentProcessor& p,const char* stage,std::uint64_t& ownerPeak,std::uint64_t callerBytes=0) {
    const auto state=InstrumentDrumTestAccess::loadSnapshot(p);ownerPeak=std::max(ownerPeak,state->retained);
    CHECK(state->retained==state->ledgerPcm+state->reserved);
    CHECK(state->intendedPcm<=drums::intendedDecodedLimit&&state->retained<=drums::retainedDecodedLimit);
    CHECK(state->actualPlayer==state->player);
    const auto pcmRequests=InstrumentDrumTestAccess::loadActualPcmRequests(p);
    CHECK(pcmRequests.valid&&pcmRequests.count==state->unique&&pcmRequests.payload==state->ledgerPcm);
    CHECK(pcmRequests.requested==pcmRequests.payload+pcmRequests.overhead&&pcmRequests.overhead==pcmRequests.count*std::_Non_user_size);
    const auto memory=loadMemoryView();CHECK(memory.privateBytes!=0&&memory.workingSet!=0);
    std::cout<<"LOCAL_LOAD stage="<<stage<<" physical_assets="<<state->unique<<" physical_pcm="<<state->ledgerPcm
        <<" pcm_crt_requested="<<pcmRequests.requested<<" pcm_alignment_metadata="<<pcmRequests.overhead
        <<" intended_pcm="<<state->intendedPcm<<" reserved_decoded="<<state->reserved<<" retained="<<state->retained
        <<" image_metadata="<<state->imageMetadata<<" roles="<<state->roles<<" base="<<state->base<<" work="<<state->work
        <<" parent="<<state->parent<<" joined_output="<<state->output<<" player="<<state->player
        <<" old_workspace="<<state->legacyWorkspace<<" old_encoded="<<state->legacyEncoded
        <<" explicit_test_caller_bytes="<<callerBytes<<" process_private="<<memory.privateBytes<<" rss="<<memory.workingSet
        <<" process_start_peak_rss="<<memory.processStartPeakWorkingSet<<'\n';
}
bool loadSameC(const InstrumentDrumTestAccess::LoadSnapshot& a,const InstrumentDrumTestAccess::LoadSnapshot& b) {
    return a.slots==b.slots&&a.owners==b.owners&&a.tickets==b.tickets&&a.candidates==b.candidates
        &&a.inputIds==b.inputIds&&a.publication==b.publication&&a.ledgerPcm==b.ledgerPcm&&a.unique==b.unique
        &&a.intendedPcm==b.intendedPcm&&a.intendedUnique==b.intendedUnique
        &&a.history==b.history&&a.targets==b.targets&&a.sourceWord==b.sourceWord&&a.edit==b.edit&&a.reset==b.reset;
}
std::uint64_t loadWaitTick() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct LoadWaitClockPair {std::uint64_t before=0,after=0;juce::int64 utcMs=0;};
LoadWaitClockPair loadWaitClockPair() {const auto before=loadWaitTick();const auto utc=juce::Time::currentTimeMillis();return {before,loadWaitTick(),utc};}
struct LoadWaitSpan {
    std::uint64_t count=0,firstBegin=0,firstEnd=0,maxBegin=0,maxEnd=0,lastBegin=0,lastEnd=0;bool overflow=false;
    void add(std::uint64_t begin,std::uint64_t end) noexcept {
        if(count==std::numeric_limits<std::uint64_t>::max()){overflow=true;return;}
        if(!count){firstBegin=begin;firstEnd=end;}if(!count||end-begin>maxEnd-maxBegin){maxBegin=begin;maxEnd=end;}
        ++count;lastBegin=begin;lastEnd=end;}
};
struct LoadWaitObservation {
    LoadWaitClockPair entry,exit,snapshotBegin,snapshotEnd;LoadWaitSpan drum,ai;
    std::uint64_t truePredicates=0,falsePredicates=0;bool predicateOverflow=false,settled=false,ready=false;
    void predicate(bool value) noexcept {auto& count=value?truePredicates:falsePredicates;
        if(count==std::numeric_limits<std::uint64_t>::max())predicateOverflow=true;else ++count;}
};
void loadPrintWait(const LoadWaitObservation& observation,const InstrumentDrumTestAccess::LoadWaitState& state,
                   unsigned before,unsigned after,const std::array<unsigned,5>& service,const char* tag="post_filler_retry") {
    const auto pair=[](const char* name,const LoadWaitClockPair& value){std::cout<<' '<<name<<"_steady_before_ns="<<value.before
        <<' '<<name<<"_utc_ms="<<value.utcMs<<' '<<name<<"_steady_after_ns="<<value.after;};
    std::cout<<"LOCAL_LOAD_WAIT tag="<<tag<<" request="<<state.request;pair("entry",observation.entry);pair("exit",observation.exit);
    std::cout<<" settled="<<observation.settled<<" ready="<<observation.ready<<" predicate_true="<<observation.truePredicates
        <<" predicate_false="<<observation.falsePredicates<<" overflow="<<observation.predicateOverflow<<'\n';
    for(const auto& item:{std::pair{"drum",&observation.drum},std::pair{"ai",&observation.ai}}){const auto& span=*item.second;
        std::cout<<"LOCAL_LOAD_WAIT_POLL owner="<<item.first<<" count="<<span.count<<" first_begin_ns="<<span.firstBegin<<" first_end_ns="<<span.firstEnd
            <<" max_begin_ns="<<span.maxBegin<<" max_end_ns="<<span.maxEnd<<" last_begin_ns="<<span.lastBegin<<" last_end_ns="<<span.lastEnd<<" overflow="<<span.overflow<<'\n';}
    std::cout<<"LOCAL_LOAD_WAIT_STATE";pair("begin",observation.snapshotBegin);pair("end",observation.snapshotEnd);
    std::cout<<" phase="<<state.phase<<" gate_before="<<before<<" gate_after="<<after<<" local="<<state.local<<" rendering="<<state.rendering
        <<" receipt="<<state.receipt<<" receipt_status="<<state.receiptStatus<<" receipt_sent="<<state.receiptSent<<" retiring="<<state.retiring<<" cancel="<<state.cancel
        <<" job="<<state.job<<" ticket="<<state.ticket<<" pending_op="<<state.pending<<" pending_local="<<state.pendingLocal<<" active_op="<<state.active
        <<" completion="<<state.completion<<" completion_op="<<state.completionOperation<<" completion_local="<<state.completionLocal<<" worker_cancelled="<<state.workerCancelled
        <<" generations="<<service[0]<<" renders="<<service[1]<<" encode_started="<<service[2]<<" encode_completed="<<service[3]<<" encode_failures="<<service[4]<<'\n';
}
bool loadPump(InstrumentProcessor& p,LoadWaitObservation* observation=nullptr) {
    // Calling these real control owners directly deliberately does not dispatch the25ms pump.
    const auto begin=observation?loadWaitTick():0;p.drumImport().poll();const auto drumEnd=observation?loadWaitTick():0;
    p.pollAi();const auto aiEnd=observation?loadWaitTick():0;const bool result=p.aiProposalPhase()!=AiProposalPhase::pending;
    if(observation){observation->drum.add(begin,drumEnd);observation->ai.add(drumEnd,aiEnd);observation->predicate(result);}return result;
}
bool loadReady(InstrumentProcessor& p,LoadWaitObservation* observation=nullptr) {
    if(observation)observation->entry=loadWaitClockPair();
    const bool settled=loadSpin([&]{return loadPump(p,observation);});if(observation)observation->exit=loadWaitClockPair();CHECK(settled);
    const bool ready=settled&&p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal();CHECK(ready);
    if(observation){observation->settled=settled;observation->ready=ready;}
    if(!ready)std::cerr<<"LOCAL_LOAD_REJECTED status="<<p.aiWorkflowStatus().message<<'\n';return ready;
}
bool loadCompleteCapacities(InstrumentProcessor& p,LoadWorkGate& gate,std::uint64_t& peak) {
    const bool rendered=loadSpin([&]{return gate.after.load()!=0;});CHECK(rendered);if(!rendered){gate.release();return false;}
    loadReport(p,"worker_after_render_held",peak);gate.releaseAfter();std::array<std::uint64_t,4> capacity{};
    const bool completed=loadSpin([&]{capacity=InstrumentDrumTestAccess::loadCompletionCapacity(p.drumImport());return capacity[0]!=0;});CHECK(completed);
    CHECK((capacity==std::array<std::uint64_t,4>{576000,576000,576000,576000}));
    std::cout<<"LOCAL_LOAD_REAL_COMPLETION sizes="<<capacity[0]<<','<<capacity[1]<<" capacities="<<capacity[2]<<','<<capacity[3]<<'\n';
    if(completed)loadReport(p,"actual_completion_PCM_parent_and_P_still_alive",peak);
    return completed&&loadReady(p);
}
void loadProcess(InstrumentProcessor& p,juce::AudioBuffer<float>& block,juce::MidiBuffer& midi) {
    // Only the actual callback enters this scope: no fixture allocations/snapshots inside it.
    ownershipHeap::CallbackScope callback;p.processBlock(block,midi);
}
// A complete deterministic host timeline. The duplicate36 mapping and pad16 at51 are included.
void loadMidi(juce::MidiBuffer& midi,std::size_t first,int count) {
    midi.clear();for(unsigned n=0;n<16;++n){const auto on=std::size_t(n)*12000,off=on+3000;
        if(on>=first&&on<first+std::size_t(count))midi.addEvent(juce::MidiMessage::noteOn(1,36+int(n),juce::uint8(96)),int(on-first));
        if(off>=first&&off<first+std::size_t(count))midi.addEvent(juce::MidiMessage::noteOff(1,36+int(n)),int(off-first));
    }
}
ai::PreviewAudio loadLive(InstrumentProcessor& p,LoadWorkGate* gate=nullptr) {
    constexpr std::array<int,6> partition{17,131,64,3,512,257};
    ai::PreviewAudio out;out.left.resize(576000);out.right.resize(576000);out.bodyFrames=384000;
    juce::AudioBuffer<float> block(2,512);juce::MidiBuffer midi;midi.ensureSize(4096);
    std::size_t first=0,index=0;bool released=false;
    while(first<out.left.size()){
        const auto count=int(std::min(std::size_t(partition[index++%partition.size()]),out.left.size()-first));
        block.setSize(2,count,false,false,true);block.clear();loadMidi(midi,first,count);loadProcess(p,block,midi);
        std::copy_n(block.getReadPointer(0),count,out.left.begin()+first);std::copy_n(block.getReadPointer(1),count,out.right.begin()+first);first+=count;
        if(gate){if(!released&&first>=out.left.size()/4){gate->releaseBefore();released=true;}p.pollAi();}
    }
    if(gate&&!released)gate->releaseBefore();return out;
}
std::array<juce::AudioBuffer<float>,2> loadFollowing(InstrumentProcessor& p) {
    std::array<juce::AudioBuffer<float>,2> result;juce::MidiBuffer midi;
    for(auto& block:result){block.setSize(2,512);block.clear();loadProcess(p,block,midi);}return result;
}
bool loadEqualBlock(const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b) {
    if(a.getNumChannels()!=b.getNumChannels()||a.getNumSamples()!=b.getNumSamples())return false;
    for(int c=0;c<a.getNumChannels();++c)if(std::memcmp(a.getReadPointer(c),b.getReadPointer(c),std::size_t(a.getNumSamples())*sizeof(float))!=0)return false;return true;
}
void loadAll64(InstrumentProcessor& p,const LoadKit& kit) {
    p.prepareToPlay(48000,512);DrumCommitError error;
    for(unsigned n=0;n<64;++n){DrumPadTarget target;const bool captured=p.captureDrumPerformanceTarget(kit.racks[n/16],0,std::uint8_t(n%16),target,error);CHECK(captured);
        if(captured)CHECK(p.enqueueDrumPad(target,96));}
    juce::AudioBuffer<float> block(2,64);juce::MidiBuffer midi;loadProcess(p,block,midi);
    const auto hits=InstrumentDrumTestAccess::loadEngineHits(p);CHECK((hits==std::array<std::size_t,4>{16,16,16,16}));CHECK(block.getMagnitude(0,64)>0);
    p.releaseResources();
}
void loadAudition(InstrumentProcessor& p,const std::array<juce::AudioBuffer<float>,2>& committed,std::uint64_t& peak) {
    CHECK(p.aiProposal());if(!p.aiProposal())return;
    // The test oracle player is separately charged and destroyed before that charge releases.
    LoadLease oracleCharge(p,loadOutputBytes);CHECK(oracleCharge);if(!oracleCharge)return;
    RenderedPreview oracle;oracle.prepare(48000);const auto revision=p.aiProposal()->tag.editRevision;
    CHECK(oracle.start(p.aiProposal()->audio,revision));CHECK(p.startAiPreview());
    CHECK(InstrumentDrumTestAccess::loadSnapshot(p)->player==loadOutputBytes);
    juce::MidiBuffer midi;juce::AudioBuffer<float> actual(2,512),expected;
    expected.makeCopyOf(committed[0]);loadProcess(p,actual,midi);oracle.mix(expected,revision);CHECK(loadEqualBlock(actual,expected));
    p.stopAiPreview();oracle.stop();expected.makeCopyOf(committed[1]);loadProcess(p,actual,midi);oracle.mix(expected,revision);CHECK(loadEqualBlock(actual,expected));
    CHECK(p.aiPreviewStopped()&&oracle.stopped());oracle.release();
    // No message dispatch: real P remains physically occupied and charged despite the audio ACK.
    const auto state=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(state->player==loadOutputBytes&&state->actualPlayer==loadOutputBytes);
    loadReport(p,"stopped_P_before_refine",peak,loadOutputBytes);
}
void loadRawClosure(InstrumentProcessor& p) {
    const auto baseline=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(baseline->roles==0);
    std::array<std::uint64_t,3> ids{};DrumCommitError error;
    for(auto& id:ids){const bool started=p.beginDrumInput(id,error);CHECK(started&&id!=0);if(!started)break;}
    const auto held=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(held->inputIds==ids&&held->ledgerPcm==baseline->ledgerPcm);
    CHECK(held->legacyWorkspace-baseline->legacyWorkspace==3ull*8*1024*1024+393224);
    CHECK(held->legacyEncoded==baseline->legacyEncoded&&held->reserved==baseline->reserved);
    std::uint64_t fourth=777;CHECK(!p.beginDrumInput(fourth,error)&&error==DrumCommitError::preparationFull&&fourth==777);
    // No source/decoder/worker exists in this low-level admission-only fixture. Cancel then real cleanup.
    for(auto id:ids)if(id){p.cancelDrumInput(id);InstrumentDrumTestAccess::loadReleaseRaw(p,id);}
    const auto drained=InstrumentDrumTestAccess::loadSnapshot(p);CHECK((drained->inputIds==std::array<std::uint64_t,3>{}));
    CHECK(drained->legacyWorkspace==baseline->legacyWorkspace+393224&&drained->legacyEncoded==baseline->legacyEncoded&&drained->reserved==baseline->reserved);
}
void owned_full_load() {
    juce::ScopedJuceInitialiser_GUI gui;ownershipHeap::calibrate();
    // This host bundle and oracle arrays are test-caller storage, not product ownership roots.
    juce::MemoryBlock saved;std::unique_ptr<ProjectDocument> savedProject;std::unique_ptr<drums::DrumSlots> savedSlots;
    ai::PreviewAudio expectedLive;std::array<juce::AudioBuffer<float>,2> expectedFollowing;
    const auto preset=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("local-load-"+juce::Uuid().toString()+".airack");
    const auto patternFile=preset.withFileExtension(".airpat");
    struct OwnedFiles {juce::File preset,pattern;~OwnedFiles(){preset.deleteFile();pattern.deleteFile();}} ownedFiles{preset,patternFile};
    std::uint64_t calibratedBase=0,calibratedWork=0,ownerPeak=0;
    {
        LoadWorkGate gate;auto service=std::make_unique<OwnedPreviewService>(loadPatternReply());
        auto processor=std::make_unique<InstrumentProcessor>(service->options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
        struct ReleaseBeforeJoin {LoadWorkGate& gate;~ReleaseBeforeJoin(){gate.release();}} releaseBeforeJoin{gate};
        LoadKit kit;if(!kit.install(p))return;loadAll64(p,kit);if(!loadMixed(p,kit))return;
        CHECK(p.setPlaybackMode(PlaybackMode::externalMidi,loadEdit(p)));InstrumentDrumTestAccess::loadClearHistory(p);
        CHECK(InstrumentDrumTestAccess::loadPcmValid(p));
        // Expected committed engine output is measured from the actual self-contained bundle,
        // before AI starts. Destroy this second C before the product peak observation.
        {
            juce::MemoryBlock referenceState;p.getStateInformation(referenceState);CHECK(referenceState.getSize()>loadKitBytes);
            auto reference=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const bool restored=reference->restoreProjectState(referenceState.getData(),int(referenceState.getSize()));CHECK(restored);if(!restored)return;
            reference->prepareToPlay(48000,512);expectedLive=loadLive(*reference);expectedFollowing=loadFollowing(*reference);reference->releaseResources();reference.reset();
        }
        if(!kit.holdThreePrepared(p))return;const auto kit67=InstrumentDrumTestAccess::loadSnapshot(p);
        const auto actual67=InstrumentDrumTestAccess::loadActualPcmRequests(p);
        CHECK(actual67.valid&&actual67.count==67&&actual67.payload==67*loadAssetBytes);
        CHECK(actual67.requested==67*(loadAssetBytes+std::_Non_user_size)&&actual67.overhead==67*std::_Non_user_size);
        p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;CHECK(p.setAiMusicalContext({"drums","D","minor",120}));
        InstrumentDrumTestAccess::hook(p.drumImport(),LoadWorkGate::hook,&gate);
        const auto callerBytes=std::uint64_t(kit.input.capacity())*sizeof(float)+std::uint64_t(expectedLive.left.capacity()+expectedLive.right.capacity())*sizeof(float)+2*2*512*sizeof(float);
        loadReport(p,"before_first_max_request",ownerPeak,callerBytes);
        bool calibrated=false;LoadWaitObservation firstGate;InstrumentDrumTestAccess::LoadWaitState firstGateState;
        unsigned firstGateBefore=0,firstGateAfter=0;std::array<unsigned,5> firstGateService{};
        service->observeLoadTiming=true;
        const auto requested=ownershipHeap::observe([&]{
            gate.arm();AiRequestOptions options{"Exercise all16 mappings at maximum preview geometry","local-test-model",{ai::RequestScope::pattern,false,false,false}};
            const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted){gate.release();return;}
            firstGate.entry=loadWaitClockPair();
            const bool before=loadSpin([&]{const auto begin=loadWaitTick();p.pollAi();const auto end=loadWaitTick();
                const bool value=gate.before.load()!=0;firstGate.ai.add(begin,end);firstGate.predicate(value);return value;});
            firstGate.exit=loadWaitClockPair();firstGate.settled=before;
            firstGate.snapshotBegin=loadWaitClockPair();firstGateState=InstrumentDrumTestAccess::loadWaitState(p);
            firstGateBefore=gate.before.load();firstGateAfter=gate.after.load();
            firstGateService={service->generationCalls.load(),service->renderCalls.load(),service->terminalEncodesStarted.load(),
                service->terminalEncodesCompleted.load(),service->terminalEncodingFailures.load()};firstGate.snapshotEnd=loadWaitClockPair();
            CHECK(before);if(!before){
                const auto status=p.aiWorkflowStatus();const auto state=InstrumentDrumTestAccess::loadSnapshot(p);
                std::cerr<<"LOCAL_LOAD_GATE_TIMEOUT ai_phase="<<int(p.aiProposalPhase())<<" client_phase="<<int(status.connection.phase)
                    <<" runtime_status="<<int(status.connection.status)<<" error="<<status.connection.errorCode<<" message="<<status.message
                    <<" provider="<<service->providerCalls<<" renders="<<service->renderCalls<<" generations="<<service->generationCalls
                    <<" protocol_failure="<<service->protocolFailure<<" roles="<<state->roles<<" base="<<state->base<<" work="<<state->work
                    <<" parent="<<state->parent<<" output="<<state->output<<" player="<<state->player<<'\n';gate.release();return;}
            const auto active=InstrumentDrumTestAccess::loadSnapshot(p);calibratedBase=active->base;calibratedWork=active->work;
            CHECK(active->roles==5&&active->parent==0&&active->player==0&&active->work>loadOutputBytes);
            CHECK(loadSameC(*kit67,*active));CHECK(active->reserved==active->base+active->work+active->imageMetadata);
            loadReport(p,"actual_first_worker_before_render",ownerPeak,callerBytes);
            // This actual audio engine progresses alongside the real local renderer after quarter1.
            // AI result is not applied and preview is never started during this entire timeline.
            {const auto actual=loadLive(p,&gate);CHECK(loadEqual(actual,expectedLive));CHECK(!p.aiPreviewPlaying());}
            if(!loadCompleteCapacities(p,gate,ownerPeak))return;
            const auto* proposal=p.aiProposal();CHECK(proposal&&proposal->candidate&&proposal->audio.bodyFrames==384000&&proposal->audio.left.size()==576000);
            if(!proposal||!proposal->candidate)return;
            CHECK(proposal->audio.left.capacity()==576000&&proposal->audio.right.capacity()==576000);
            CHECK(service->providerCalls==1&&service->renderCalls==1&&service->generationCalls==1&&!service->protocolFailure&&service->terminalPcmFrames==0);
            const auto parent=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(parent->roles==3&&parent->parent==loadOutputBytes&&parent->work==0&&parent->output==0);
            CHECK(parent->reserved==parent->base+parent->parent+parent->imageMetadata&&loadSameC(*kit67,*parent));
            loadAudition(p,expectedFollowing,ownerPeak);
            gate.arm();const bool refined=p.refineAi("Keep the full kit and16 mappings");CHECK(refined);if(!refined){gate.release();return;}
            const bool refining=loadSpin([&]{p.pollAi();return gate.before.load()!=0;});CHECK(refining);if(!refining){gate.release();return;}
            const auto maximum=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(maximum->roles==7&&maximum->parent==loadOutputBytes&&maximum->player==loadOutputBytes&&maximum->work==calibratedWork);
            CHECK(loadSameC(*kit67,*maximum));CHECK(maximum->reserved==maximum->base+maximum->parent+maximum->player+maximum->work+maximum->imageMetadata);
            CHECK(maximum->parent+maximum->player+loadOutputBytes==13824000);loadReport(p,"actual_parent_P_inflight_maximum",ownerPeak,callerBytes);
            DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(kit.racks[0],0,0,target,error));
            std::uint64_t deniedInput=777;CHECK(!p.beginDrumInput(deniedInput,error)&&error==DrumCommitError::busy&&deniedInput==777);
            CHECK(!p.drumImport().open(target)&&!p.drumImport().openGeneration(target));
            gate.releaseBefore();if(!loadCompleteCapacities(p,gate,ownerPeak))return;
            CHECK(service->providerCalls==2&&service->renderCalls==2&&service->generationCalls==2&&!service->protocolFailure&&service->terminalPcmFrames==0);
            CHECK(p.aiProposal()->audio.left.capacity()==576000&&p.aiProposal()->audio.right.capacity()==576000);
            calibrated=true;
        });
        CHECK(requested.callbackAllocationCount==0&&requested.callbackFreeCount==0&&requested.callbackRequested==0);
        loadPrintWait(firstGate,firstGateState,firstGateBefore,firstGateAfter,firstGateService,"initial_gate");service->printLoadEncodes("initial_gate_after_observe");
        gate.release();CHECK(calibrated);if(!calibrated){p.closeAiSession();return;}
        loadReferencePartitions(p,calibratedWork,*p.aiProposal());
        std::cout<<"LOCAL_LOAD_CALIBRATED owner_peak="<<ownerPeak<<" actual_request_heap_peak="<<requested.peak<<" realloc_overlap="<<requested.overlap
            <<" base="<<calibratedBase<<" work="<<calibratedWork<<" caller_bytes="<<callerBytes<<" (CRT includes service,DSP and caller; no whole-process512MiB claim)\n";
        // Genuine calibrated workload succeeds BEFORE the same work reservation is tested one byte short.
        {
            LoadLease referenceCharge(p,loadOutputBytes);CHECK(referenceCharge);if(!referenceCharge)return;
            const auto preserved=std::make_unique<ai::PreviewAudio>(p.aiProposal()->audio);const auto parentTag=p.aiProposal()->tag;
            const auto before=InstrumentDrumTestAccess::loadSnapshot(p);const auto committedBefore=ownedProject(p);CHECK(before->parent==loadOutputBytes&&before->player==loadOutputBytes);
            const auto room=drums::retainedDecodedLimit-before->retained;CHECK(room>=calibratedWork);if(room<calibratedWork)return;
            {
            LoadLease oneByteShort(p,room-calibratedWork+1);CHECK(oneByteShort);if(!oneByteShort)return;
            gate.arm();const auto evaluationsBefore=service->renderCalls.load();const bool started=p.refineAi("Calibrated local render one byte short");CHECK(started);
            if(!started){gate.release();return;}const bool settled=loadSpin([&]{return loadPump(p);});CHECK(settled);
            CHECK(gate.before==0&&gate.after==0);CHECK(service->renderCalls>evaluationsBefore);gate.release();
            CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag==parentTag);
            if(p.aiProposal())CHECK(loadEqual(p.aiProposal()->audio,*preserved));
            const auto after=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(*ownedProject(p)==*committedBefore);CHECK(loadSameC(*before,*after));CHECK(after->roles==3&&after->parent==loadOutputBytes&&after->work==0&&after->output==0&&after->player==loadOutputBytes);
            // Provider/evaluation/request counters may advance legitimately; retained C/tickets/audio may not.
            CHECK(after->reserved==before->reserved+oneByteShort.bytes);std::uint64_t refusalReservationPeak=0;loadReport(p,"render_one_byte_refused_with_test_filler",refusalReservationPeak,callerBytes+loadOutputBytes);
            }
            gate.arm();gate.release();CHECK(p.refineAi("Retry the same full workload after releasing filler"));LoadWaitObservation observation;
            const bool retryReady=loadReady(p,&observation);observation.snapshotBegin=loadWaitClockPair();
            const auto state=InstrumentDrumTestAccess::loadWaitState(p);const auto beforeCount=gate.before.load(),afterCount=gate.after.load();
            const std::array<unsigned,5> serviceCounts{service->generationCalls.load(),service->renderCalls.load(),service->terminalEncodesStarted.load(),
                service->terminalEncodesCompleted.load(),service->terminalEncodingFailures.load()};observation.snapshotEnd=loadWaitClockPair();
            loadPrintWait(observation,state,beforeCount,afterCount,serviceCounts);service->printLoadEncodes("post_filler_retry");if(!retryReady){p.closeAiSession();return;}
            CHECK(loadEqual(p.aiProposal()->audio,*preserved));CHECK(loadSameC(*kit67,*InstrumentDrumTestAccess::loadSnapshot(p)));
        }
        // The actual existing completion owns full PCM across close until its real control owner consumes it.
        gate.arm();const bool cancelSubmitted=p.refineAi("Complete real full output before closing");CHECK(cancelSubmitted);if(!cancelSubmitted)return;
        const bool cancelBefore=loadSpin([&]{p.pollAi();return gate.before.load()!=0;});CHECK(cancelBefore);if(!cancelBefore){gate.release();return;}
        gate.releaseBefore();const bool cancelRendered=loadSpin([&]{return gate.after.load()!=0;});CHECK(cancelRendered);if(!cancelRendered){gate.release();return;}
        gate.releaseAfter();std::array<std::uint64_t,4> heldCapacity{};
        const bool completionHeld=loadSpin([&]{heldCapacity=InstrumentDrumTestAccess::loadCompletionCapacity(p.drumImport());return heldCapacity[0]!=0;});CHECK(completionHeld);if(!completionHeld)return;
        CHECK((heldCapacity==std::array<std::uint64_t,4>{576000,576000,576000,576000}));const auto heldOutput=InstrumentDrumTestAccess::loadSnapshot(p);
        CHECK(heldOutput->work==calibratedWork&&heldOutput->roles==7);p.closeAiSession();
        const auto retainedCapacity=InstrumentDrumTestAccess::loadCompletionCapacity(p.drumImport());const auto closedOutput=InstrumentDrumTestAccess::loadSnapshot(p);
        // Assert charges only while this independently observed completion still physically owns the full arrays.
        CHECK(retainedCapacity==heldCapacity&&retainedCapacity[0]!=0);
        if(retainedCapacity[0])CHECK(closedOutput->work==heldOutput->work&&(closedOutput->roles&4)!=0&&closedOutput->ledgerPcm==heldOutput->ledgerPcm);
        std::cout<<"LOCAL_LOAD_CLOSED_ACTUAL_COMPLETION frames="<<retainedCapacity[0]<<" capacity="<<retainedCapacity[2]<<" work_charge="<<closedOutput->work<<" roles="<<closedOutput->roles<<'\n';
        loadReport(p,"closed_actual_completion_PCM_still_alive",ownerPeak,callerBytes);
        CHECK(waitOwned([&]{const auto state=InstrumentDrumTestAccess::loadSnapshot(p);return state->roles==0&&state->player==0;}));
        CHECK(InstrumentDrumTestAccess::loadSnapshot(p)->reserved==0);InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        if(!connectOwnedAccount(p))return;CHECK(p.setAiMusicalContext({"drums","D","minor",120}));
        {
            const auto before=InstrumentDrumTestAccess::loadSnapshot(p);const auto committedBefore=ownedProject(p);const auto providerBefore=service->providerCalls.load(),rendererBefore=service->renderCalls.load();
            const auto room=drums::retainedDecodedLimit-before->retained;CHECK(room>=calibratedBase);if(room<calibratedBase)return;
            LoadLease oneByteShort(p,room-calibratedBase+1);CHECK(oneByteShort);if(!oneByteShort)return;
            AiRequestOptions options{"Same full source root one byte short","local-test-model",{ai::RequestScope::pattern,false,false,false}};
            const auto rootHeap=ownershipHeap::observe([&]{CHECK(!p.generateAi(options));});
            const auto after=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(*ownedProject(p)==*committedBefore);CHECK(loadSameC(*before,*after));CHECK(after->roles==0&&after->requestCounter==before->requestCounter);
            CHECK(after->reserved==before->reserved+oneByteShort.bytes&&service->providerCalls==providerBefore&&service->renderCalls==rendererBefore);
            std::cout<<"LOCAL_LOAD_ROOT_REFUSAL actual_requested_peak="<<rootHeap.peak<<" unrelated_precapture_metadata_possible=1\n";
        }
        AiRequestOptions retry{"Same source successfully admitted after removing filler","local-test-model",{ai::RequestScope::pattern,false,false,false}};
        CHECK(airack::testing::generateAi(p,retry));if(!loadReady(p)){p.closeAiSession();return;}CHECK(p.aiProposal()->audio.left.size()==576000);
        CHECK(p.applyAiProposal());p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::loadSnapshot(p)->roles==0;}));
        savedSlots=std::make_unique<drums::DrumSlots>(InstrumentDrumTestAccess::loadSnapshot(p)->slots);
        kit.dropCandidates(p);loadRawClosure(p);p.releaseResources();InstrumentDrumTestAccess::loadClearHistory(p);
        CHECK(p.saveSoundPreset(preset));CHECK(p.savePattern(patternFile));p.getStateInformation(saved);CHECK(saved.getSize()>loadKitBytes);savedProject=ownedProject(p);
        CHECK(savedProject->pattern.noteCount==16&&savedProject->pattern.bars==4);CHECK(InstrumentDrumTestAccess::loadSnapshot(p)->unique==64);
        // Explicitly destroy source owner, worker and companion service. Saved bytes/metadata/audio contain no strong SampleAsset references.
        processor.reset();service.reset();
    }
    CHECK(savedProject&&savedSlots);if(!savedProject||!savedSlots)return;
    for(const bool presetRecall:{false,true}){
        companion::ClientOptions absent;absent.allowLaunch=false;auto fresh=std::make_unique<InstrumentProcessor>(absent, airack::testing::isolatedRackRuntime());
        const bool recalled=presetRecall?fresh->loadSoundPreset(preset):fresh->restoreProjectState(saved.getData(),int(saved.getSize()));CHECK(recalled);if(!recalled)continue;
        if(presetRecall)CHECK(fresh->loadPattern(patternFile));const auto project=ownedProject(*fresh);CHECK(project->sound==savedProject->sound&&project->pattern==savedProject->pattern);
        const auto state=InstrumentDrumTestAccess::loadSnapshot(*fresh);CHECK(state->slots==*savedSlots&&state->unique==64&&state->intendedUnique==64&&state->ledgerPcm==loadKitBytes&&state->roles==0);
        CHECK(InstrumentDrumTestAccess::loadPcmValid(*fresh));const auto actual64=InstrumentDrumTestAccess::loadActualPcmRequests(*fresh);
        CHECK(actual64.valid&&actual64.count==64&&actual64.payload==loadKitBytes);
        CHECK(actual64.requested==loadKitBytes+64*std::_Non_user_size&&actual64.overhead==64*std::_Non_user_size);
        CHECK(fresh->setPlaybackMode(PlaybackMode::externalMidi,loadEdit(*fresh)));fresh->prepareToPlay(48000,512);
        const auto recalledAudio=loadLive(*fresh);CHECK(loadEqual(recalledAudio,expectedLive));CHECK(!fresh->aiPreviewPlaying()&&!fresh->aiProposal());fresh->releaseResources();fresh.reset();
    }
    std::cout<<"LOCAL_LOAD_COMPLETE original139asset_retention_corpus_still_separately_required=1 native_timing_acceptance=0\n";
}
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool denyJoinProject=false,denyIdleEvaluation=false,observeTerminalInput=false,denyTerminalInput=false,denyApplyMap=false,observeRootMap=false,denyRootMap=false;
thread_local bool observeValidationProject=false,denyValidationProject=false;
thread_local unsigned terminalInputAllocations=0,rootMapAllocations=0,validationProjectAllocations=0;
thread_local unsigned idleEvaluationAllocations=0;
thread_local unsigned joinDenied=0,joinExpectedWarnings=0,joinPendingWarnings=0,joinUnexpectedReports=0;
std::array<void*,32> joinAllocationFrames{};unsigned short joinAllocationFrameCount=0;
int joinAllocationHook(int type,void*,size_t bytes,int use,long,const unsigned char*,int){
    const bool application=(use&0xffff)==_NORMAL_BLOCK||(use&0xffff)==_CLIENT_BLOCK;
    if(type==_HOOK_ALLOC&&application&&observeTerminalInput&&bytes==sizeof(ai::LocalGenerationRequest)){
        observeTerminalInput=false;++terminalInputAllocations;if(!denyTerminalInput)return 1;
        denyTerminalInput=false;++joinDenied;++joinPendingWarnings;joinAllocationFrameCount=RtlCaptureStackBackTrace(0,DWORD(joinAllocationFrames.size()),joinAllocationFrames.data(),nullptr);return 0;
    }
    if(type==_HOOK_ALLOC&&application&&observeRootMap&&bytes==sizeof(ai::PreviewSampleMap)){
        observeRootMap=false;++rootMapAllocations;if(!denyRootMap)return 1;
        denyRootMap=false;++joinDenied;++joinPendingWarnings;joinAllocationFrameCount=RtlCaptureStackBackTrace(0,DWORD(joinAllocationFrames.size()),joinAllocationFrames.data(),nullptr);return 0;
    }
    if(type==_HOOK_ALLOC&&application&&denyApplyMap&&bytes==sizeof(ai::PreviewSampleMap)){
        denyApplyMap=false;++joinDenied;++joinPendingWarnings;joinAllocationFrameCount=RtlCaptureStackBackTrace(0,DWORD(joinAllocationFrames.size()),joinAllocationFrames.data(),nullptr);return 0;
    }
    if(type==_HOOK_ALLOC&&application&&denyIdleEvaluation&&bytes==sizeof(ai::PreviewEvaluate)){
        denyIdleEvaluation=false;++idleEvaluationAllocations;++joinDenied;++joinPendingWarnings;
        joinAllocationFrameCount=RtlCaptureStackBackTrace(0,DWORD(joinAllocationFrames.size()),joinAllocationFrames.data(),nullptr);return 0;
    }
    if(type==_HOOK_ALLOC&&application&&observeValidationProject&&bytes==sizeof(ProjectDocument)){
        observeValidationProject=false;++validationProjectAllocations;if(!denyValidationProject)return 1;
        denyValidationProject=false;++joinDenied;++joinPendingWarnings;joinAllocationFrameCount=RtlCaptureStackBackTrace(0,DWORD(joinAllocationFrames.size()),joinAllocationFrames.data(),nullptr);return 0;
    }
    if(type==_HOOK_ALLOC&&application&&denyJoinProject&&bytes==sizeof(ProjectDocument)){
        denyJoinProject=false;++joinDenied;++joinPendingWarnings;
        joinAllocationFrameCount=RtlCaptureStackBackTrace(0,DWORD(joinAllocationFrames.size()),joinAllocationFrames.data(),nullptr);return 0;
    }return 1;
}
int __cdecl joinAllocationReport(int type,wchar_t* message,int* value){
    const std::wstring_view text(message?message:L"");const bool expected=text==L"Client hook allocation failure.\n"||text==L"Client hook allocation failure.\r\n"||text==L"Client hook allocation failure.";
    if(type==_CRT_WARN&&joinPendingWarnings&&expected){--joinPendingWarnings;++joinExpectedWarnings;}else ++joinUnexpectedReports;
    if(value)*value=0;return 1;
}
struct JoinAllocationFault {
    _CRT_ALLOC_HOOK previous;
    explicit JoinAllocationFault(){denyJoinProject=denyIdleEvaluation=observeTerminalInput=denyTerminalInput=denyApplyMap=observeRootMap=denyRootMap=false;terminalInputAllocations=rootMapAllocations=validationProjectAllocations=0;observeValidationProject=denyValidationProject=false;joinDenied=joinExpectedWarnings=joinPendingWarnings=joinUnexpectedReports=idleEvaluationAllocations=0;joinAllocationFrameCount=0;
        _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,joinAllocationReport);previous=_CrtSetAllocHook(joinAllocationHook);}
    ~JoinAllocationFault(){observeValidationProject=denyValidationProject=false;denyJoinProject=denyIdleEvaluation=observeTerminalInput=denyTerminalInput=denyApplyMap=observeRootMap=denyRootMap=false;_CrtSetAllocHook(previous);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,joinAllocationReport);}
    static void observe(void*,DrumImport::TestPoint point) noexcept {if(point==DrumImport::TestPoint::beforeLocalJoin)denyJoinProject=true;}
    static void terminal(void* context,DrumImport::TestPoint point) noexcept {if(point==DrumImport::TestPoint::beforeLocalTerminal){observeTerminalInput=true;denyTerminalInput=*static_cast<bool*>(context);}}
    static void validation(void* context,DrumImport::TestPoint point) noexcept {if(point==DrumImport::TestPoint::beforeLocalTerminal){observeValidationProject=true;denyValidationProject=*static_cast<bool*>(context);}}
    static void apply(void*,DrumImport::TestPoint point) noexcept {if(point==DrumImport::TestPoint::beforeLocalApplyValidation)denyApplyMap=true;}
    static void root(void* context,DrumImport::TestPoint point) noexcept {if(point==DrumImport::TestPoint::beforeLocalRootValidation){observeRootMap=true;denyRootMap=*static_cast<bool*>(context);}}
};
void owned_root_validation_allocation_preserves(){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    drums::DrumPad metadata;if(!installOwnedHit(p,metadata))return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
    const auto original=ownedProject(p);AiRequestOptions options{"Keep the original catalog phrase","local-test-model",{ai::RequestScope::both,false,false,false}};
    {
        JoinAllocationFault calibration;bool deny=false;InstrumentDrumTestAccess::hook(p.drumImport(),JoinAllocationFault::root,&deny);
        const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted){InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);return;}
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        CHECK(rootMapAllocations==1&&joinDenied==0&&joinExpectedWarnings==0&&joinUnexpectedReports==0);
    }
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());if(!p.aiProposal())return;
    const auto parent=std::make_unique<ai::GenerationResult>(*p.aiProposal());const auto before=InstrumentDrumTestAccess::loadSnapshot(p);
    {
        JoinAllocationFault fault;bool deny=true;InstrumentDrumTestAccess::hook(p.drumImport(),JoinAllocationFault::root,&deny);
        const bool submitted=p.refineAi("Distinct child");CHECK(submitted);if(!submitted){InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);return;}
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        CHECK(rootMapAllocations==1&&joinDenied==1&&joinExpectedWarnings==1&&joinPendingWarnings==0&&joinUnexpectedReports==0&&joinAllocationFrameCount>0);
        std::cout<<"LOCAL_SELECTED_ROOT_MAP_APPLICATION_DENIAL bytes="<<sizeof(ai::PreviewSampleMap)<<" pcs=";
        for(unsigned n=0;n<joinAllocationFrameCount;++n)std::cout<<joinAllocationFrames[n]<<',';std::cout<<'\n';
    }
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag==parent->tag);
    if(!p.aiProposal())return;CHECK(p.aiProposal()->audio.left==parent->audio.left&&p.aiProposal()->audio.right==parent->audio.right);
    CHECK(*ownedProject(p)==*original&&loadSameC(*before,*InstrumentDrumTestAccess::loadSnapshot(p)));
    CHECK(waitOwned([&]{p.pollAi();const auto status=p.aiWorkflowStatus();return status.connection.phase==companion::ClientPhase::ready&&status.connection.status==companion::RuntimeStatus::ready;}));
    const bool retried=p.refineAi("Distinct child");CHECK(retried);if(retried){CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
        CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag!=parent->tag);
        if(p.aiProposal()&&p.aiProposal()->candidate)CHECK(p.aiProposal()->candidate->pattern.notes[0].startBeats==.5);}
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();CHECK(!service.protocolFailure&&!service.workerRetained);
}
void idle_parent_poll_preserves(){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    drums::DrumPad metadata;if(!installOwnedHit(p,metadata))return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
    AiRequestOptions options{"Keep this existing pad","local-test-model",{ai::RequestScope::pattern,false,false,false}};
    const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted)return;
    CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));CHECK(p.aiProposal());if(!p.aiProposal())return;
    const auto tag=p.aiProposal()->tag;const auto frames=p.aiProposal()->audio.left.size();const auto retained=InstrumentDrumTestAccess::localState(p);
    {
        JoinAllocationFault fault;denyIdleEvaluation=true;p.pollAi();denyIdleEvaluation=false;
        CHECK(idleEvaluationAllocations==0);CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag==tag&&p.aiProposal()->audio.left.size()==frames);
        CHECK(joinDenied==joinExpectedWarnings&&joinPendingWarnings==0&&joinUnexpectedReports==0);
        CHECK(InstrumentDrumTestAccess::localState(p)==retained);
        if(joinDenied){std::cout<<"LOCAL_IDLE_EVALUATION_DENIAL bytes="<<sizeof(ai::PreviewEvaluate)<<" pcs=";
            for(unsigned n=0;n<joinAllocationFrameCount;++n)std::cout<<joinAllocationFrames[n]<<',';std::cout<<'\n';}
    }
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();CHECK(!service.protocolFailure&&!service.workerRetained);
}
void consumed_terminal_allocation_settles(){
    juce::ScopedJuceInitialiser_GUI gui;
    for(const bool parent:{false,true}){
        OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
        drums::DrumPad metadata;if(!installOwnedHit(p,metadata))return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
        const auto original=ownedProject(p);AiRequestOptions options{"Keep the owned sampled pad","local-test-model",{ai::RequestScope::pattern,false,false,false}};
        std::unique_ptr<ai::GenerationResult> retained;
        if(parent){const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted)return;
            CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));CHECK(p.aiProposal());if(!p.aiProposal())return;
            retained=std::make_unique<ai::GenerationResult>(*p.aiProposal());}
        {
            JoinAllocationFault fault;InstrumentDrumTestAccess::hook(p.drumImport(),JoinAllocationFault::observe,nullptr);
            const bool submitted=parent?p.refineAi("Retain this kit and phrase"):airack::testing::generateAi(p,options);CHECK(submitted);
            if(!submitted){InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);return;}
            const bool denied=waitOwned([&]{p.pollAi();return joinDenied!=0;},20000);CHECK(denied);
            InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
            CHECK(joinDenied==1&&joinExpectedWarnings==1&&joinPendingWarnings==0&&joinUnexpectedReports==0&&joinAllocationFrameCount>0);
            std::cout<<"LOCAL_JOIN_APPLICATION_DENIAL parent="<<parent<<" bytes="<<sizeof(ProjectDocument)<<" pcs=";
            for(unsigned n=0;n<joinAllocationFrameCount;++n)std::cout<<joinAllocationFrames[n]<<',';std::cout<<'\n';
            const bool settled=waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},2000);
            CHECK(settled);if(!settled){p.closeAiSession();return;}
        }
        if(parent){CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());if(p.aiProposal()){
            CHECK(p.aiProposal()->tag==retained->tag&&p.aiProposal()->candidate==retained->candidate);
            CHECK(p.aiProposal()->audio.left==retained->audio.left&&p.aiProposal()->audio.right==retained->audio.right);}}
        else CHECK(p.aiProposalPhase()==AiProposalPhase::failed&&!p.aiProposal()&&!p.startAiPreview()&&!p.applyAiProposal());
        CHECK(*ownedProject(p)==*original);p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();
        CHECK(!service.protocolFailure&&!service.workerRetained);
    }
}
void owned_refine_retry_after_terminal_allocation_failure(bool terminalInput=false,bool terminalValidation=false){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    drums::DrumPad metadata;if(!installOwnedHit(p,metadata))return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
    AiRequestOptions options{"Keep this original phrase","local-test-model",{ai::RequestScope::pattern,false,false,false}};
    {
        JoinAllocationFault calibration;bool deny=false;
        if(terminalInput||terminalValidation)InstrumentDrumTestAccess::hook(p.drumImport(),terminalValidation?JoinAllocationFault::validation:JoinAllocationFault::terminal,&deny);
        const bool submitted=airack::testing::generateAi(p,options);CHECK(submitted);if(!submitted){InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);return;}
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        if(terminalInput||terminalValidation)CHECK((terminalValidation?validationProjectAllocations:terminalInputAllocations)==1&&joinDenied==0&&joinExpectedWarnings==0&&joinUnexpectedReports==0);
        CHECK(p.aiProposal());if(!p.aiProposal())return;
    }
    const auto parent=p.aiProposal()->tag;const auto originalPattern=p.aiProposal()->candidate->pattern;
    CHECK(originalPattern.noteCount==1&&originalPattern.notes[0].startBeats==0);
    {
        JoinAllocationFault fault;bool deny=true;InstrumentDrumTestAccess::hook(p.drumImport(),terminalInput?JoinAllocationFault::terminal:terminalValidation?JoinAllocationFault::validation:JoinAllocationFault::observe,(terminalInput||terminalValidation)?&deny:nullptr);
        const bool started=p.refineAi("Distinct child");CHECK(started);if(!started){InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);return;}
        const bool denied=waitOwned([&]{p.pollAi();return joinDenied!=0;},20000);CHECK(denied);
        InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        CHECK(joinDenied==1&&joinExpectedWarnings==1&&joinPendingWarnings==0&&joinUnexpectedReports==0&&joinAllocationFrameCount>0);
        if(terminalInput||terminalValidation)CHECK((terminalValidation?validationProjectAllocations:terminalInputAllocations)==1);
        std::cout<<"LOCAL_REFINEMENT_TERMINAL_DENIAL input="<<terminalInput<<" nested_validation="<<terminalValidation<<" bytes="<<(terminalInput?sizeof(ai::LocalGenerationRequest):sizeof(ProjectDocument))<<" pcs=";
        for(unsigned n=0;n<joinAllocationFrameCount;++n)std::cout<<joinAllocationFrames[n]<<',';std::cout<<'\n';
        CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},2000));
    }
    const bool restored=p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag==parent;
    CHECK(restored);if(!restored){p.closeAiSession();return;}CHECK(p.aiProposal()->candidate->pattern==originalPattern);
    CHECK(service.providerCalls==2&&service.renderCalls==2&&service.generationCalls==2&&!service.protocolFailure);
    CHECK(waitOwned([&]{p.pollAi();const auto status=p.aiWorkflowStatus().connection;return status.phase==companion::ClientPhase::ready&&status.status==companion::RuntimeStatus::ready;}));
    const bool retry=p.refineAi("Retry the same admitted parent");CHECK(retry);if(!retry){p.closeAiSession();return;}
    CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
    const bool refined=p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag.requestId>parent.requestId;
    CHECK(refined);if(!refined)std::cerr<<"LOCAL_PARENT_RETRY_REJECTED status="<<p.aiWorkflowStatus().message<<'\n';
    if(refined){CHECK(service.providerCalls==3&&service.renderCalls==3&&service.generationCalls==3&&!service.protocolFailure&&service.terminalPcmFrames==0);
        CHECK(p.applyAiProposal());}
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();CHECK(!service.workerRetained);
}
void owned_apply_map_allocation_refusal_preserves_retry(){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    drums::DrumPad metadata;if(!installOwnedHit(p,metadata))return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
    const auto original=ownedProject(p);AiRequestOptions options{"Keep the owned pad with a phrase","local-test-model",{ai::RequestScope::pattern,false,false,false}};
    CHECK(airack::testing::generateAi(p,options));CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));CHECK(p.aiProposal());if(!p.aiProposal())return;
    const auto tag=p.aiProposal()->tag;const auto proposed=p.aiProposal()->candidate->pattern;const auto before=InstrumentDrumTestAccess::loadSnapshot(p);
    {
        JoinAllocationFault fault;InstrumentDrumTestAccess::hook(p.drumImport(),JoinAllocationFault::apply,nullptr);bool applied=false,threw=false;
        try{applied=p.applyAiProposal();}catch(const std::bad_alloc&){threw=true;}
        InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        CHECK(joinDenied==1&&joinExpectedWarnings==1&&joinPendingWarnings==0&&joinUnexpectedReports==0&&joinAllocationFrameCount>0);
        std::cout<<"LOCAL_APPLY_MAP_APPLICATION_DENIAL bytes="<<sizeof(ai::PreviewSampleMap)<<" pcs=";
        for(unsigned n=0;n<joinAllocationFrameCount;++n)std::cout<<joinAllocationFrames[n]<<',';std::cout<<'\n';
        CHECK(!threw&&!applied);
    }
    const auto refused=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(!refused->applying&&loadSameC(*before,*refused));
    CHECK(*ownedProject(p)==*original);CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag==tag);
    const bool retried=p.applyAiProposal();CHECK(retried);if(retried){CHECK(ownedProject(p)->pattern==proposed);CHECK(p.undoSoundEdit());CHECK(*ownedProject(p)==*original);}
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));p.releaseResources();processor.reset();CHECK(!service.protocolFailure&&!service.workerRetained);
}

#endif

void owned_catalog_physical_root_and_apply(){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("local-catalog-physical-"+juce::Uuid().toString());
    CHECK(!folder.exists());if(folder.exists())return;CHECK(folder.createDirectory().wasOk());
    struct OwnedFolder {juce::File folder;~OwnedFolder(){folder.deleteRecursively();}} ownedFolder{folder};
    auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    drums::DrumPad firstMetadata,secondMetadata;const auto first=installOwnedHit(p,firstMetadata),second=installOwnedHit(p,secondMetadata);CHECK(first&&second);if(!first||!second)return;
    CHECK(first->bank==0&&second->bank==1);if(first->bank!=0||second->bank!=1)return;
    // Use the existing public dormant-bank recall contract: fresh allocation chooses bank0, whose real old samples must be cleared.
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=first->rack;
    CHECK(p.applyRackOperation(remove,loadEdit(p))==RackCandidateStatus::ready);
    const auto retired=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(retired->owners[0]&&retired->slots[0][0]==firstMetadata);
    auto numeric=ownedProject(p);numeric->sound.layout.synthUse[0]=SlotUse::neverUsed;CHECK(validateProjectDocument(*numeric));
    juce::MemoryBlock saved;p.getStateInformation(saved);drums::DrumBundlePtr existing;drums::DrumError error;
    const bool decoded=drums::decodeDrumBundle({static_cast<const std::uint8_t*>(saved.getData()),saved.getSize()},existing,error);CHECK(decoded);if(!decoded)return;
    juce::MemoryBlock compact;CHECK(encodeProjectState(*numeric,compact));drums::DrumDocumentPtr document;
    CHECK(drums::makeDrumDocument(existing->document().slots(),document,error));drums::DrumBundlePtr bundle;
    const bool bundled=drums::makeDrumBundle(document,{static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()},existing->assets(),bundle,error);CHECK(bundled);if(!bundled)return;
    juce::MemoryBlock encoded;CHECK(drums::encodeDrumBundle(*bundle,encoded,error));
    const bool restored=p.restoreProjectState(encoded.getData(),int(encoded.getSize()));CHECK(restored);if(!restored)return;
    const auto dormant=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(dormant->owners[0]&&dormant->slots[0][0]==firstMetadata&&dormant->slots[1][0]==secondMetadata);
    CHECK(ownedProject(p)->sound.layout.synthUse[0]==SlotUse::neverUsed&&ownedProject(p)->sound.layout.synthUse[1]==SlotUse::present);
    // No fixture bundle/asset owners may keep storage alive outside C during the actual workflow.
    existing.reset();bundle.reset();document.reset();
    const auto sameAssignments=[](const auto& a,const auto& b){return a.slots==b.slots&&a.owners==b.owners&&a.tickets==b.tickets&&a.candidates==b.candidates
        &&a.history==b.history&&a.targets==b.targets&&a.edit==b.edit&&a.reset==b.reset&&a.sourceWord==b.sourceWord;};
    p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
    CHECK(p.setAiMusicalContext({"drums","D","minor",120}));CHECK(p.saveSoundPreset(folder.getChildFile("Original.airack")));
    const auto rootProject=ownedProject(p);const auto root=InstrumentDrumTestAccess::loadSnapshot(p);
    CHECK(root->owners[0]&&root->owners[16]&&root->slots[0][0]==firstMetadata&&root->slots[1][0]==secondMetadata);
    AiRequestOptions options{"Replace original physical source","local-test-model",{ai::RequestScope::sound,false,true,false}};
    const bool generated=airack::testing::generateAi(p,options);CHECK(generated);if(!generated)return;
    const bool ready=waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000);CHECK(ready&&p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());
    if(!p.aiProposal()){
        const auto status=p.aiWorkflowStatus();const auto local=InstrumentDrumTestAccess::localState(p);const auto* current=p.aiCurrentRequest();
        std::cerr<<"CATALOG_PHYSICAL_ROOT_FAILED "<<status.message<<" request="<<(current?current->tag.requestId:0)
            <<" ready="<<ready<<" ai_phase="<<int(p.aiProposalPhase())<<" connection="<<int(status.connection.phase)<<" runtime="<<int(status.connection.status)
            <<" provider="<<service.providerCalls.load()<<" render="<<service.renderCalls.load()<<" generations="<<service.generationCalls.load()
            <<" selections="<<service.selectionTerminals.load()<<" materialize="<<service.materializeCalls.load()<<" materialize_provider="<<service.materializeProviders.load()
            <<" materialize_render="<<service.materializeRenders.load()<<" encode_started="<<service.terminalEncodesStarted.load()<<" encode_completed="<<service.terminalEncodesCompleted.load()
            <<" encode_failures="<<service.terminalEncodingFailures.load()<<" protocol_failure="<<service.protocolFailure.load()<<" terminal_pcm="<<service.terminalPcmFrames.load()
            <<" local_roles="<<local[0]<<" local_retained="<<local[1]<<" local_reserved="<<local[2]<<" local_player="<<local[3]
            <<" local_parent="<<local[4]<<" local_work="<<local[5]<<" local_output="<<local[6]<<" importer="<<int(p.drumImport().view().phase)<<std::endl;return;
    }
    CHECK(service.selectionTerminals==1&&service.materializeCalls==1&&service.materializeProviders==0&&service.materializeRenders==1);
    CHECK(service.providerCalls==1&&service.renderCalls==1&&service.generationCalls==2&&!service.protocolFailure&&service.terminalPcmFrames==0);
    CHECK(*ownedProject(p)==*rootProject&&sameAssignments(*root,*InstrumentDrumTestAccess::loadSnapshot(p)));
    CHECK(p.aiProposal()->candidate&&p.aiCurrentRequest()->catalog&&p.aiCurrentRequest()->catalog->authority->definitions.size()==1);
    CHECK(p.aiProposal()->measurements.peak>0);const auto parentTag=p.aiProposal()->tag;
    const bool refining=p.refineAi("Refine captured root physical source");CHECK(refining);if(!refining)return;
    const bool refined=waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000);CHECK(refined&&p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag!=parentTag);
    if(!refined||!p.aiProposal()||p.aiProposal()->tag==parentTag){std::cerr<<"CATALOG_PHYSICAL_REFINE_FAILED "<<p.aiWorkflowStatus().message<<'\n';return;}
    CHECK(service.selectionTerminals==1&&service.materializeCalls==1&&service.materializeProviders==0&&service.materializeRenders==1);
    CHECK(service.providerCalls==2&&service.renderCalls==2&&service.generationCalls==3&&!service.protocolFailure&&service.terminalPcmFrames==0);
    CHECK(*p.aiOriginalSource()==*rootProject&&*ownedProject(p)==*rootProject&&sameAssignments(*root,*InstrumentDrumTestAccess::loadSnapshot(p)));
    const auto expected=std::make_unique<ProjectDocument>(*p.aiProposal()->candidate);CHECK(expected->sound.values[*rackParameterIndex(NativeModule::drum,0,"level")]==.61f);
    CHECK(p.startAiPreview());juce::AudioBuffer<float> block(2,512);juce::MidiBuffer midi;p.processBlock(block,midi);CHECK(block.getMagnitude(0,512)>0);
    p.stopAiPreview();block.clear();p.processBlock(block,midi);CHECK(p.aiPreviewStopped());
    CHECK(p.applyAiProposal());const auto after=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(*ownedProject(p)==*expected);
    CHECK(!after->owners[0]&&!after->slots[0][0].asset&&after->owners[16]==root->owners[16]&&after->slots[1]==root->slots[1]);
    CHECK(after->history[0]==root->history[0]+1&&after->targets[0]!=root->targets[0]);
    CHECK(p.undoSoundEdit());const auto undone=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(*ownedProject(p)==*rootProject&&undone->owners==root->owners&&undone->slots==root->slots);
    CHECK(p.redoSoundEdit());const auto redone=InstrumentDrumTestAccess::loadSnapshot(p);CHECK(*ownedProject(p)==*expected&&redone->owners==after->owners&&redone->slots==after->slots);
    p.closeAiSession();p.releaseResources();
}


juce::Component* captureComponent(juce::Component& root,const juce::String& id){
    if(root.getComponentID()==id)return &root;
    for(auto* child:root.getChildren())if(auto* found=captureComponent(*child,id))return found;return nullptr;
}
bool capturePress(juce::Component& root,const char* id){
    auto* control=captureComponent(root,id);const bool reachable=control&&control->isShowing()&&control->isEnabled();CHECK(reachable);if(!reachable)return false;
    auto* handler=control->getAccessibilityHandler();const bool pressed=handler&&handler->getActions().invoke(juce::AccessibilityActionType::press);
    CHECK(pressed);juce::MessageManager::getInstance()->runDispatchLoopUntil(30);return pressed;
}
void owned_native_capture(const juce::File& destination){
    juce::ScopedJuceInitialiser_GUI gui;OwnedPreviewService service;auto runtime=airack::testing::isolatedRackRuntime();auto processor=std::make_unique<InstrumentProcessor>(service.options(),runtime);auto& p=*processor;
    drums::DrumPad metadata;const auto first=installOwnedHit(p,metadata);if(!first)return;const auto skin=airack::testing::prepareTileSkin(runtime,p,first->rack);CHECK(skin);if(!skin)return;
    DrumPadTarget pad16;DrumCommitError error;CHECK(p.captureDrumPadTarget(first->rack,0,15,pad16,error));
    std::vector<float> pcm(4800);for(std::size_t n=0;n<pcm.size();++n)pcm[n]=float(.1*std::exp(-double(n)/700)*std::sin(double(n)*.1));
    std::uint64_t ticket=0;CHECK(p.prepareDrumAsset(pcm,1,ticket,error));metadata.name="Capture pad sixteen";CHECK(p.assignPreparedDrumPad(pad16,ticket,metadata,{},error));
    auto live=ownedProject(p);CHECK(live->sound.layout.synthCount==1&&sourceModule(live->sound,pad16.bank)==NativeModule::drum);const auto gain=rackParameterIndex(NativeModule::drum,pad16.bank,"drum.pad.16.gain_db");
    const auto tune=rackParameterIndex(NativeModule::drum,pad16.bank,"drum.pad.16.tune_semitones");CHECK(gain&&tune);if(!gain||!tune)return;
    live->sound.controlLocks[*gain]=true;std::uint64_t revision=0;SoundDocument sound;CHECK(p.trySoundSnapshot(sound,revision));CHECK(p.applySoundEdit(live->sound,SoundEditOrigin::manual,revision));
    const auto original=ownedProject(p);std::shared_ptr<const RackCatalogSnapshot> inventory;CHECK(p.captureAvailableRackCatalog(inventory));CHECK(inventory);
    const auto tile=airack::test::frozenTileArrayRackDesign();const auto library=airack::test::frozenLibraryRackDesigns();CHECK(library.size()==28);
    CHECK(std::count_if(library.begin(),library.end(),[&](const auto& d){return d.id==tile.id;})==1);
    const RackCatalogEntry* canonical=nullptr;if(inventory)for(const auto& entry:inventory->entries)if(entry.definition.id==tile.id){CHECK(!canonical);canonical=&entry;}
    CHECK(canonical&&canonical->definition==tile&&!canonical->contentSha256.isEmpty());
    if(canonical){RackDesign resolved;CHECK(resolveCatalogDesign(*inventory,tile.id,tile.revision,canonical->contentSha256,resolved));CHECK(resolved==tile);}
    juce::var catalog;CHECK(ai::catalogFor(*original,ai::CatalogRendering::ownedLocal,catalog));const auto reach=unitReach(NativeModule::drum,pad16.bank);
    CHECK(reach.size()==147);CHECK(catalog["controls"].isArray());if(!catalog["controls"].isArray())return;for(const auto index:reach){const auto& row=(*catalog["controls"].getArray())[int(index)];CHECK(testing::catalogRowId(row).toString()==parameterRegistry()[index].id&&bool(row["active"]));}
    CHECK(bool((*catalog["controls"].getArray())[int(*gain)]["locked"]));
    p.prepareToPlay(48000,512);auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    const auto root=destination!=juce::File{}?destination:juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("build/drum-machine-native-20261022/local-preview-r1/native-capture-"+juce::Uuid().toString());
    CHECK(root.createDirectory().wasOk());juce::Array<juce::var> records;
    juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;
    const auto process=[&]{audio.clear();p.processBlock(audio,midi);p.pollAi();};
    const auto wait=[&](auto ready){return waitOwned([&]{process();return ready();},20000);};
    const auto shot=[&](const char* name){juce::MessageManager::getInstance()->runDispatchLoopUntil(60);auto* screen=screenOf(*editor,"ai.screen");
        auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true);auto file=root.getChildFile(juce::String(name)+".png");auto stream=file.createOutputStream();CHECK(stream&&image.isValid());if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(image,*stream));
        auto* record=new juce::DynamicObject;record->setProperty("state",name);record->setProperty("phase",int(p.aiProposalPhase()));record->setProperty("connection",int(p.aiWorkflowStatus().connection.phase));
        record->setProperty("workflow",p.aiWorkflowStatus().message);record->setProperty("shown",screen?screen->shown.status:juce::String());record->setProperty("image",file.getFullPathName());records.add(juce::var(record));CHECK(root.getChildFile("states.json").replaceWithText(juce::JSON::toString(juce::var(records),true,22)));};
    CHECK(capturePress(*editor,"ai.toggle"));CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&!p.aiWorkflowStatus().accountPending&&a->models.size()==1;}));
    auto* screen=screenOf(*editor,"ai.screen");CHECK(screen);if(!screen)return;CHECK(enterText(*screen,"ai.tempo","400"));
    auto* viewport=captureComponent(*editor,"rack.stack");auto* stack=viewport?dynamic_cast<RackStack*>(viewport->getParentComponent()):nullptr;
    CHECK(stack);auto* unit=stack?stack->unit(first->rack):nullptr;auto* face=unit?unit->face():nullptr;CHECK(face&&face->tileArray());
    if(face&&face->tileArray()){stack->select(first->rack);CHECK(capturePress(*editor,"face.pad.16"));CHECK(face->tileArray()->selectedPad()==15);process();CHECK(audio.getMagnitude(0,512)>0);face->setOpen(true,false);}
    shot("pad16-assigned-locked");
    const auto begin=[&](const char* instruction,ai::RequestScope scope){CHECK(stepTo(*screen,"ai.scope",int(scope)));if(scope!=ai::RequestScope::sound)CHECK(stepTo(*screen,"ai.requirePattern",0));
        CHECK(enterText(*screen,"ai.prompt",instruction));return capturePress(*editor,"ai.generate");};
    const auto discard=[&]{CHECK(capturePress(*editor,"ai.discard"));CHECK(wait([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));};
    DrumPadTarget manual;CHECK(p.captureDrumPadTarget(first->rack,0,0,manual,error));CHECK(p.drumImport().open(manual));const auto calls=service.generationCalls.load();
    CHECK(begin("Capture busy",ai::RequestScope::sound));CHECK(p.aiProposalPhase()==AiProposalPhase::idle&&service.generationCalls==calls&&p.drumImport().busy());CHECK(p.aiWorkflowStatus().message.containsIgnoreCase("busy"));shot("source-session-busy");
    p.drumImport().cancel();CHECK(wait([&]{return !p.drumImport().busy();}));
    CHECK(testing::readyRackInventory(p));
    service.captureRelease=false;CHECK(begin("Capture held",ai::RequestScope::sound));CHECK(wait([&]{return service.captureArrived.load();}));
    CHECK(p.aiProposalPhase()==AiProposalPhase::pending&&!p.applyAiProposal()&&!p.startAiPreview());CHECK((InstrumentDrumTestAccess::localState(p)[0]&1)!=0);
    CHECK(!p.drumImport().openGeneration(manual)&&!p.drumImport().open(manual));CHECK(screen->shown.title=="GENERATING");shot("generating");
    CHECK(pressItem(*screen,"ai.cancel"));service.captureRelease=true;CHECK(wait([&]{return p.aiProposalPhase()!=AiProposalPhase::pending&&InstrumentDrumTestAccess::localState(p)[0]==0;}));
    CHECK(*ownedProject(p)==*original);CHECK(p.drumImport().openGeneration(manual));p.drumImport().cancel();CHECK(wait([&]{return !p.drumImport().busy();}));shot("canceled");
    CHECK(begin("Capture sound",ai::RequestScope::sound));CHECK(wait([&]{return p.aiProposalPhase()!=AiProposalPhase::pending;}));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());
    if(!p.aiProposal()){std::cerr<<"CAPTURE_SOUND_FAILED "<<p.aiWorkflowStatus().message<<'\n';return;}
    CHECK(p.aiCurrentRequest()->catalog&&p.aiCurrentRequest()->catalog->authority->rendering==ai::CatalogRendering::ownedLocal);
    const auto descriptor=juce::JSON::parse(juce::String::fromUTF8(p.aiCurrentRequest()->catalog->authority->descriptorJson.c_str()));bool tileBound=false;
    CHECK(descriptor["entries"].isArray());if(!descriptor["entries"].isArray())return;for(const auto& row:*descriptor["entries"].getArray())if(juce::Uuid(testing::catalogRowId(row).toString())==tile.id)tileBound=canonical&&testing::catalogRowHash(row).toString()==canonical->contentSha256;
    CHECK(tileBound);CHECK(p.aiProposal()->candidate->pattern==original->pattern&&p.aiProposal()->candidate->sound.values[*gain]==original->sound.values[*gain]);
    CHECK(p.aiProposal()->candidate->sound.values[*tune]==.55f&&p.aiProposal()->measurements.peak>0&&service.terminalPcmFrames==0);CHECK(*ownedProject(p)==*original);shot("proposed-hidden-pad16");
    auto denied=p.aiProposal()->edits;denied.parameters={{parameterRegistry()[*gain].id,original->sound.values[*gain]<.5f?.8f:.2f}};auto refused=std::make_unique<ProjectDocument>(*original);
    CHECK(ai::admitProposal(p.aiCurrentRequest()->source,p.aiCurrentRequest()->policy,denied,*p.aiCurrentRequest()->catalog->authority,*refused)!=ai::ProposalStatus::accepted&&*refused==*original);
    CHECK(enterText(*screen,"ai.prompt","Capture refined"));CHECK(capturePress(*editor,"ai.refine"));CHECK(wait([&]{return p.aiProposalPhase()!=AiProposalPhase::pending;}));
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->candidate->sound.values[*tune]==.6f);if(!p.aiProposal())return;shot("refined");
    CHECK(capturePress(*editor,"ai.preview"));process();CHECK(p.aiPreviewPlaying()&&audio.getMagnitude(0,512)>0);CHECK(!p.drumImport().openGeneration(manual)&&!p.drumImport().open(manual));shot("auditioning");
    CHECK(capturePress(*editor,"ai.stop"));CHECK(wait([&]{return p.aiPreviewStopped();}));const auto expected=std::make_unique<ProjectDocument>(*p.aiProposal()->candidate);
    CHECK(capturePress(*editor,"ai.apply"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::idle;}));CHECK(*ownedProject(p)==*expected);shot("applied");
    CHECK(p.undoSoundEdit());CHECK(*ownedProject(p)==*original);shot("undo");CHECK(p.redoSoundEdit());CHECK(*ownedProject(p)==*expected);shot("redo");
    CHECK(wait([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));
    for(const auto scope:{ai::RequestScope::pattern,ai::RequestScope::both}){
        const auto before=ownedProject(p);CHECK(begin("Capture scopes",scope));CHECK(wait([&]{return p.aiProposalPhase()!=AiProposalPhase::pending;}));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());if(!p.aiProposal())return;
        CHECK(p.aiCurrentRequest()->policy.scope==scope&&p.aiProposal()->candidate->pattern.noteCount==1&&p.aiProposal()->candidate->pattern.notes[0].pitch==51);
        if(scope==ai::RequestScope::pattern)CHECK(p.aiProposal()->candidate->sound==before->sound);
        CHECK(p.aiProposal()->measurements.peak>0);shot(scope==ai::RequestScope::pattern?"pattern-scope":"sound-and-pattern-scope");discard();}
    CHECK(begin("Capture stale",ai::RequestScope::sound));CHECK(wait([&]{return p.aiProposalPhase()!=AiProposalPhase::pending;}));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed);
    const auto liveTune=p.control(*tune).getValue();CHECK(p.beginParameterGesture(*tune));CHECK(p.setParameterGestureValue(*tune,liveTune<.5f?.7f:.4f));CHECK(p.endParameterGesture(*tune));p.refreshAiProposal();
    CHECK(p.aiProposalPhase()==AiProposalPhase::stale&&!p.startAiPreview()&&!p.applyAiProposal());shot("stale");p.cancelAi();CHECK(wait([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));
    // Closing the existing source worker genuinely denies a new local render; playback remains prepared.
    p.drumImport().closeAdmission();CHECK(wait([&]{return !p.drumImport().busy();}));CHECK(begin("Capture unavailable",ai::RequestScope::sound));CHECK(wait([&]{return p.aiProposalPhase()!=AiProposalPhase::pending;}));
    CHECK(p.aiProposalPhase()==AiProposalPhase::failed&&!p.aiProposal()&&!p.startAiPreview()&&!p.applyAiProposal());CHECK(p.aiWorkflowStatus().message.contains("generation_local_busy"));
    shot("local-worker-unavailable");DrumPadTarget performance;CHECK(p.captureDrumPerformanceTarget(first->rack,0,15,performance,error));CHECK(p.enqueueDrumPad(performance,96));process();CHECK(audio.getMagnitude(0,512)>0);
    CHECK(!service.protocolFailure&&service.terminalPcmFrames==0);CHECK(root.getChildFile("states.json").replaceWithText(juce::JSON::toString(juce::var(records),true,22)));
    std::cout<<"DRUM_NATIVE_CAPTURE "<<root.getFullPathName()<<" records="<<records.size()<<" terminal_pcm="<<service.terminalPcmFrames<<'\n';editor.reset();p.closeAiSession();p.releaseResources();
}

void cancelled_materialize_keeps_spent_budget(){
    using namespace ai;using namespace companion;
    auto input=std::make_unique<LocalGenerationRequest>();auto& request=input->request;request.tag={juce::Uuid(),2,7,11};request.source=drumProject();
    request.model="test-model";request.instruction="Keep the kit";input->local.root=request.tag;input->local.root.requestId=1;
    const std::array<RackDesign,1> designs{airack::test::frozenTileArrayRackDesign()};RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(designs,{}, {},snapshot));
    juce::var inventory;CHECK(catalogInventoryFor(snapshot,CatalogRendering::ownedLocal,inventory));auto authority=std::make_shared<CatalogAuthority>();
    authority->root=input->local.root;authority->snapshot=snapshot.id;authority->rendering=CatalogRendering::ownedLocal;
    authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();CHECK(ai::detail::catalogAuthorityValid(*authority));
    request.catalog.emplace();auto& catalog=*request.catalog;catalog.phase=CatalogPhase::materialize;catalog.root=input->local.root;
    catalog.selectionParent=input->local.root;catalog.authority=authority;catalog.inferenceCallsUsed=3;catalog.providerStatus=rack::oauth::Status::connected;catalog.httpStatus=200;
    input->local.catalogSnapshot=snapshot.id;
    for(const auto& instance:request.source.sound.personalities->instances)for(const auto& unit:instance.design.units)
        if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(instance,unit.id))input->local.sourceMap.banks[binding->slot]={instance.id,unit.id,1};
    CHECK(digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest));juce::MemoryBlock encoded;CHECK(encodeLocalRequest(*input,encoded));
    unsigned calls=0,renders=0;const auto local=generateLocalProposal(*input,[&](const auto&,auto,const auto&){++calls;return rack::oauth::StructuredResult{};},
        [&](const auto&,const auto&,const auto&,const auto&,const auto&){++renders;return PreviewReceipt{};},[]{return true;});
    CHECK(local.numeric.status==GenerationStatus::cancelled&&local.numeric.inferenceCalls==3&&local.numeric.providerStatus==rack::oauth::Status::connected&&local.numeric.httpStatus==200);
    CHECK(calls==0&&renders==0&&!local.rendered&&!local.numeric.candidate);CHECK(encodeLocalResult(*input,local,encoded));
}

#if defined(_MSC_VER) && defined(_DEBUG)
// This opt-in gate uses the existing CRT fault/report discipline, independently of --load.
struct ObservationAllocation;
thread_local ObservationAllocation* observationAllocation=nullptr;
struct ObservationAllocation {
    InstrumentProcessor* processor;JoinAllocationFault reports;_CRT_ALLOC_HOOK previous;
    bool armed=false,deny=false;unsigned matches=0,denied=0;long request=0;
    std::uint64_t reservedAtAllocation=0;
    std::array<void*,8> site{},expectedSite{};unsigned short depth=0,expectedDepth=0;
    std::array<const ai::PreviewCallTrace*,4> watched{};
    std::array<unsigned,4> freed{};std::array<std::uint64_t,4> reservedAtFree{};
    explicit ObservationAllocation(InstrumentProcessor& p):processor(&p){CHECK(observationAllocation==nullptr);observationAllocation=this;previous=_CrtSetAllocHook(hook);CHECK(previous==joinAllocationHook);}
    ~ObservationAllocation(){_CrtSetAllocHook(previous);observationAllocation=nullptr;}
    static int __cdecl hook(int kind,void* data,std::size_t bytes,int use,long request,const unsigned char* file,int line){
        if(!observationAllocation)return joinAllocationHook(kind,data,bytes,use,request,file,line);
        auto& self=*observationAllocation;
        if(kind==_HOOK_FREE&&data)for(unsigned n=0;n<self.watched.size();++n)if(data==self.watched[n]){
            ++self.freed[n];self.reservedAtFree[n]=InstrumentDrumTestAccess::observationReserved(*self.processor);self.watched[n]=nullptr;}
        const bool application=(use&0xffff)==_NORMAL_BLOCK||(use&0xffff)==_CLIENT_BLOCK;
        if(kind==_HOOK_ALLOC&&application&&self.armed&&bytes==sizeof(ai::PreviewCallTrace)){
            self.depth=RtlCaptureStackBackTrace(0,DWORD(self.site.size()),self.site.data(),nullptr);
            // Calibration binds this size to the actual retained trace request. A denial additionally requires
            // the same first six native frames, so an earlier unrelated capture allocation cannot be denied.
            const bool sameSite=!self.deny||(self.expectedDepth>=6&&self.depth>=6&&std::equal(self.site.begin(),self.site.begin()+6,self.expectedSite.begin()));
            if(sameSite){self.armed=false;++self.matches;self.request=request;self.reservedAtAllocation=InstrumentDrumTestAccess::observationReserved(*self.processor);
                if(self.deny){++self.denied;++joinDenied;++joinPendingWarnings;return 0;}}
        }
        return self.previous?self.previous(kind,data,bytes,use,request,file,line):1;
    }
    void arm(bool fail=false){armed=true;deny=fail;matches=denied=0;request=0;reservedAtAllocation=0;site={};depth=0;}
    bool watch(unsigned index,const ai::PreviewCallTrace* trace){long actual=0;const bool valid=trace&&_CrtIsMemoryBlock(trace,unsigned(sizeof(*trace)),&actual,nullptr,nullptr)&&actual==request;
        CHECK(valid);if(valid){watched[index]=trace;freed[index]=0;reservedAtFree[index]=0;}return valid;}
};
bool observationTraceValid(const ai::PreviewCallTrace& trace,const ai::PreviewWorkDiagnostics& work){
    CHECK(!trace.overflow&&trace.count>0&&trace.count==work.renderedBlocks&&trace.count<=trace.calls.size());
    bool valid=!trace.overflow&&trace.count>0&&trace.count==work.renderedBlocks&&trace.count<=trace.calls.size();
    if(!valid)return false;
    for(unsigned n=0;n<trace.count;++n){const auto& call=trace.calls[n];const bool ordered=call.startNs>0&&call.endNs>call.startNs
            &&(!n||call.startNs>=trace.calls[n-1].endNs)&&call.startNs>=work.startNs&&call.endNs<=work.endNs;CHECK(ordered);valid&=ordered;}
    CHECK(work.renderedFrames==ai::maximumPreviewFrames&&trace.count==1125&&work.prepared&&work.effectiveQuality==RenderQuality::realtime);
    return valid&&work.renderedFrames==ai::maximumPreviewFrames&&trace.count==1125&&work.prepared;
}
void owned_observation_allocation_and_lifetime(){
    juce::ScopedJuceInitialiser_GUI gui;
    const auto sizes=InstrumentDrumTestAccess::observationSizes();std::cout<<"LOCAL_OBSERVATION_SIZES";
    for(auto size:sizes)std::cout<<' '<<size;std::cout<<'\n';
    constexpr auto bytes=std::uint64_t(sizeof(ai::PreviewCallTrace));
    // Four bars at120 BPM produce the existing maximum384000 body +192000 tail, at512/frame call geometry.
    const std::string response=R"({"version":1,"parameter_edits":[],"rack_edits":[],"macro_edits":[],"pattern":{"bars":4,"meter_numerator":4,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[{"start_beats":0,"duration_beats":0.125,"pitch":36,"velocity":96,"channel":1}]},"characteristics":"Owned drum phrase","explanation":"Use the existing sampled pad"})";
    LoadWorkGate gate;OwnedPreviewService service(response);auto processor=std::make_unique<InstrumentProcessor>(service.options(), airack::testing::isolatedRackRuntime());auto& p=*processor;
    struct ReleaseBeforeJoin{LoadWorkGate& gate;~ReleaseBeforeJoin(){gate.release();}} release{gate};
    drums::DrumPad metadata;if(!installOwnedHit(p,metadata))return;p.prepareToPlay(48000,512);if(!connectOwnedAccount(p))return;
    CHECK(p.setAiMusicalContext({"drums","D","minor",120}));const auto committed=ownedProject(p);
    InstrumentDrumTestAccess::observationReset(p,true);const auto baseline=InstrumentDrumTestAccess::localState(p);
    AiRequestOptions options{"Observe the owned sampled phrase","local-test-model",{ai::RequestScope::pattern,false,false,false}};
    ObservationAllocation probe(p);
    // Genuine joined success establishes actual trace ownership, PCM and the site used by the refusal case.
    CHECK(airack::testing::generateAi(p,options));probe.arm();
    const bool ready=waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000);CHECK(ready);
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->status==ai::GenerationStatus::ready);
    if(!ready||!p.aiProposal())return;
    auto observed=InstrumentDrumTestAccess::observationSnapshot(p);CHECK(observed.count==1&&!observed.overflow&&observed.first&&probe.matches==1&&probe.denied==0);
    if(observed.count!=1||!probe.watch(0,observed.traces[0]))return;
    CHECK(observationTraceValid(*observed.traces[0],observed.diagnostics[0]));CHECK(p.aiProposal()->audio.left.size()==ai::maximumPreviewFrames);
    probe.expectedSite=probe.site;probe.expectedDepth=probe.depth;CHECK(probe.expectedDepth>=6);
    std::cout<<"LOCAL_OBSERVATION_CALIBRATION request="<<probe.request<<" bytes="<<bytes<<" reserved_at_new="<<probe.reservedAtAllocation<<" frames="<<probe.depth;
    for(unsigned n=0;n<probe.depth;++n)std::cout<<' '<<probe.site[n];std::cout<<'\n';
    const auto parent=std::make_unique<ai::GenerationResult>(*p.aiProposal());const auto parentC=InstrumentDrumTestAccess::localState(p);
    // Arm AFTER public refinement construction; only the calibrated, real Trace new may be denied.
    CHECK(p.refineAi("Observe the owned sampled phrase"));probe.arm(true);
    CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
    CHECK(probe.matches==1&&probe.denied==1&&joinDenied==1&&joinExpectedWarnings==1&&joinPendingWarnings==0&&joinUnexpectedReports==0);
    std::cout<<"LOCAL_OBSERVATION_DENIAL matches="<<probe.matches<<" denied="<<probe.denied<<" warnings="<<joinExpectedWarnings<<" reserved_at_new="<<probe.reservedAtAllocation<<" frames="<<probe.depth;
    for(unsigned n=0;n<probe.depth;++n)std::cout<<' '<<probe.site[n];std::cout<<'\n';
    CHECK(InstrumentDrumTestAccess::observationSnapshot(p).count==1);
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag==parent->tag);
    if(!p.aiProposal())return;CHECK(p.aiProposal()->candidate==parent->candidate&&p.aiProposal()->audio.left==parent->audio.left&&p.aiProposal()->audio.right==parent->audio.right);
    CHECK(waitOwned([&]{p.pollAi();return InstrumentDrumTestAccess::localState(p)==parentC;}));
    // Actual same-parent retry succeeds; no injected result, receipt, PCM or terminal bypass.
    CHECK(p.refineAi("Observe the owned sampled phrase"));probe.arm();
    CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&p.aiProposal()->tag!=parent->tag);
    observed=InstrumentDrumTestAccess::observationSnapshot(p);CHECK(observed.count==2&&!observed.overflow&&probe.matches==1);
    if(observed.count!=2||!probe.watch(1,observed.traces[1]))return;CHECK(observationTraceValid(*observed.traces[1],observed.diagnostics[1]));
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));
    CHECK(InstrumentDrumTestAccess::localState(p)[2]==baseline[2]+2*bytes&&probe.freed[0]==0&&probe.freed[1]==0);
    // Actual full audio/Trace completion stays alive and charged across close before ordinary control consumption.
    for(unsigned iteration=2;iteration<4;++iteration){
        if(!connectOwnedAccount(p))return;
        CHECK(p.setAiMusicalContext({"drums","D","minor",120}));
        gate.arm();
        InstrumentDrumTestAccess::hook(p.drumImport(),LoadWorkGate::hook,&gate);
        CHECK(airack::testing::generateAi(p,options));const auto root=InstrumentDrumTestAccess::localState(p);probe.arm();
        const bool entered=waitOwned([&]{p.pollAi();return gate.before.load()!=0;},20000);CHECK(entered);if(!entered)return;
        const auto held=InstrumentDrumTestAccess::localState(p);CHECK(probe.matches==1&&probe.denied==0);
        CHECK(probe.reservedAtAllocation==root[2]+held[5]+bytes);
        gate.releaseBefore();const bool rendered=waitOwned([&]{return gate.after.load()!=0;},20000);CHECK(rendered);if(!rendered)return;
        gate.releaseAfter();const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        const ai::PreviewCallTrace* trace=nullptr;
        while(std::chrono::steady_clock::now()<deadline&&!(trace=InstrumentDrumTestAccess::observationCompletion(p.drumImport())))std::this_thread::yield();
        CHECK(trace!=nullptr);if(!trace||!probe.watch(iteration,trace))return;
        const auto capacity=InstrumentDrumTestAccess::loadCompletionCapacity(p.drumImport());CHECK((capacity==std::array<std::uint64_t,4>{ai::maximumPreviewFrames,ai::maximumPreviewFrames,ai::maximumPreviewFrames,ai::maximumPreviewFrames}));
        p.closeAiSession();const auto closed=InstrumentDrumTestAccess::localState(p);CHECK(closed==held&&probe.freed[iteration]==0);
        CHECK(InstrumentDrumTestAccess::observationCompletion(p.drumImport())==trace);
        CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));InstrumentDrumTestAccess::hook(p.drumImport(),nullptr,nullptr);
        observed=InstrumentDrumTestAccess::observationSnapshot(p);
        if(iteration==2){CHECK(observed.count==3&&!observed.overflow&&observed.traces[2]==trace&&probe.freed[2]==0);
            CHECK(InstrumentDrumTestAccess::localState(p)[2]==baseline[2]+3*bytes);}
        else {CHECK(observed.count==3&&observed.overflow&&probe.freed[3]==1);
            CHECK(probe.reservedAtFree[3]>=baseline[2]+4*bytes&&InstrumentDrumTestAccess::localState(p)[2]==baseline[2]+3*bytes);}
    }
    CHECK(*ownedProject(p)==*committed&&service.terminalPcmFrames==0&&!service.protocolFailure);
    // Reset destroys real history allocations before releasing their C charge; owner remains alive for each hook.
    InstrumentDrumTestAccess::observationReset(p,false);CHECK(InstrumentDrumTestAccess::localState(p)[2]==baseline[2]);
    for(unsigned n=0;n<3;++n)CHECK(probe.freed[n]==1&&probe.reservedAtFree[n]>=baseline[2]+bytes);
    // One more real owned render proves reset disables per-call Trace allocation on ordinary work.
    if(!connectOwnedAccount(p))return;
    CHECK(p.setAiMusicalContext({"drums","D","minor",120}));
    CHECK(airack::testing::generateAi(p,options));probe.arm();CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&probe.matches==0);probe.armed=false;
    observed=InstrumentDrumTestAccess::observationSnapshot(p);CHECK(observed.count==1&&!observed.traces[0]&&!observed.first);
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));CHECK(InstrumentDrumTestAccess::localState(p)[2]==baseline[2]);
    // Keep one new real trace in history through processor destruction, whose C owner must die later.
    InstrumentDrumTestAccess::observationReset(p,true);
    if(!connectOwnedAccount(p))return;
    CHECK(p.setAiMusicalContext({"drums","D","minor",120}));
    CHECK(airack::testing::generateAi(p,options));probe.arm();
    CHECK(waitOwned([&]{p.pollAi();return p.aiProposalPhase()!=AiProposalPhase::pending;},20000));
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal()&&probe.matches==1);
    observed=InstrumentDrumTestAccess::observationSnapshot(p);if(observed.count!=1||!probe.watch(0,observed.traces[0]))return;
    p.closeAiSession();CHECK(waitOwned([&]{return InstrumentDrumTestAccess::localState(p)[0]==0;}));
    CHECK(InstrumentDrumTestAccess::localState(p)[2]==baseline[2]+bytes&&probe.freed[0]==0);
    p.releaseResources();processor.reset();CHECK(probe.freed[0]==1&&probe.reservedAtFree[0]>=baseline[2]+bytes);
    CHECK(!service.workerRetained);std::cout<<"LOCAL_OBSERVATION_COMPLETE native_timing_acceptance=0\n";
}
#endif

}
int main(int argc,char** argv){
    const std::string_view mode=argc>1?argv[1]:"--protocol";
    if(mode=="--protocol"){map_codec_and_transaction();grouped_source_stage();receipt_and_context();local_wire_and_real_audio_join();cancelled_materialize_keeps_spent_budget();}
    else if(mode=="--capture")owned_native_capture(argc>2?juce::File(juce::String::fromUTF8(argv[2])):juce::File{});
    else if(mode=="--workflow"){borrowed_renderer();owned_plugin_workflow();}
    else if(mode=="--saturation")owned_saturation_candidate_stale_and_apply();
    else if(mode=="--audition-default"){owned_default_audition();owned_default_context_mapping();}
    else if(mode=="--refine"){owned_catalog_physical_root_and_apply();owned_catalog_refinement_phrase_authority();
#if defined(_MSC_VER) && defined(_DEBUG)
        owned_refine_retry_after_terminal_allocation_failure();owned_refine_retry_after_terminal_allocation_failure(true);owned_refine_retry_after_terminal_allocation_failure(false,true);
#endif
    }
#ifndef NDEBUG
    else if(mode=="--lifetime")owned_lifetime_and_independent_pump();
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
    else if(mode=="--observation")owned_observation_allocation_and_lifetime();
    else if(mode=="--load")owned_full_load();
    else if(mode=="--heap-boundary")ownershipHeap::boundaryRegression();
    else if(mode=="--oom-terminal"){idle_parent_poll_preserves();consumed_terminal_allocation_settles();owned_apply_map_allocation_refusal_preserves_retry();owned_root_validation_allocation_preserves();}
#endif
    else {std::cerr<<"Unsupported not-yet-implemented local preview test mode\n";return 2;}
    std::cout<<checks<<" CHECKs, "<<failures<<" failures\n";return failures?1:0;
}
