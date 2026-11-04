#include "InstrumentProcessor.h"
#include "InstrumentMcp.h"
#include "CompanionWire.h"
#include "CompanionLaunch.h"
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
namespace airack {
namespace {
using mcp::ipc::object;
juce::String contentHash(const ProjectDocument& source){juce::MemoryBlock bytes;if(!encodeProjectState(source,bytes))return {};return juce::SHA256(bytes).toHexString();}
juce::var instances(const SoundDocument& sound){
    juce::Array<juce::var> entries;
    if(sound.personalities)for(const auto& rack:sound.personalities->instances){juce::MemoryBlock bytes;encodeRackDesign(rack.design,bytes,RackValidationMode::instanceRecall);
        juce::Array<juce::var> bindings;for(const auto& b:rack.bindings)bindings.add(object({{"unit",int(b.unit)},{"slot",int(b.slot)}}));
        entries.add(object({{"instance_id",rack.id.toDashedString()},{"design_id",rack.design.id.toDashedString()},
            {"design_revision",mcp::ipc::decimal(rack.design.revision)},{"content_sha256",juce::SHA256(bytes).toHexString()},{"bindings",bindings}}));}
    return entries;
}
juce::var difference(const ProjectDocument& before,const ProjectDocument& after){
    juce::var a,b;if(!ai::catalogSliceFor(before,ai::CatalogRendering::ownedLocal,0,0,a)||!ai::catalogSliceFor(after,ai::CatalogRendering::ownedLocal,0,0,b))return {};
    juce::Array<juce::var> values;const auto registry=parameterRegistry();
    for(std::size_t n=0;n<parameterCount;++n)if(before.sound.values[n]!=after.sound.values[n])values.add(object({{"id",registry[n].id},{"before",double(before.sound.values[n])},{"after",double(after.sound.values[n])}}));
    return object({{"parameters",values},{"racks",object({{"before",a["racks"]},{"after",b["racks"]}})},
        {"instances",object({{"before",instances(before.sound)},{"after",instances(after.sound)}})},
        {"macros",object({{"before",a["macro_bindings"]},{"after",b["macro_bindings"]}})},
        {"pattern",object({{"before",a["pattern"]},{"after",b["pattern"]}})},
        {"name",object({{"before",before.sound.name},{"after",after.sound.name}})}});
}
juce::var padDescription(const drums::DrumPad& pad){
    juce::var source;
    if(pad.source){const auto& p=*pad.source;source=object({{"kind",p.kind==drums::SourceKind::imported?"imported":"generated"},
        {"source_file_sha256",juce::String(drums::assetIdText(p.sourceFileId))},{"source_rate",int(p.sourceRate)},{"source_channels",int(p.sourceChannels)},
        {"source_frames",mcp::ipc::decimal(p.sourceFrames)},{"begin",mcp::ipc::decimal(p.begin)},{"end",mcp::ipc::decimal(p.end)},
        {"gain",double(p.gain)},{"generator",juce::String(p.generator)},{"condition",juce::String(p.condition)},
        {"seed",mcp::ipc::decimal(p.seed)},{"prompt_sha256",juce::String(drums::assetIdText(p.promptId))}});}
    return object({{"name",juce::String(pad.name)},{"asset",pad.asset?juce::var(juce::String(drums::assetIdText(*pad.asset))):juce::var()},{"source",source}});
}
bool policyFor(const juce::var& value,ai::ProposalPolicy& out){
    if(!mcp::ipc::fields(value,{"scope","allow_macro_changes","allow_slot_reuse","require_pattern"}))return false;
    if(value.hasProperty("scope")){if(!value["scope"].isString())return false;const auto scope=value["scope"].toString();
        if(scope=="sound")out.scope=ai::RequestScope::sound;else if(scope=="pattern")out.scope=ai::RequestScope::pattern;else if(scope=="both")out.scope=ai::RequestScope::both;else return false;}
    for(const auto& pair:{std::pair{"allow_macro_changes",&out.allowMacroChanges},std::pair{"allow_slot_reuse",&out.allowSlotReuse},std::pair{"require_pattern",&out.requirePattern}})
        if(value.hasProperty(pair.first)){if(!value[pair.first].isBool())return false;*pair.second=bool(value[pair.first]);}
    return true;
}
bool limitFor(const juce::var& args,std::size_t fallback,std::size_t maximum,std::size_t& out){
    out=fallback;if(!args.hasProperty("limit"))return true;const auto v=args["limit"];
    if((!v.isInt()&&!v.isInt64())||juce::int64(v)<1||juce::int64(v)>juce::int64(maximum))return false;out=std::size_t(juce::int64(v));return true;
}
bool owns(const McpState::Stage& stage,const mcp::Command& command){return stage.caller.owner==command.caller.owner&&stage.caller.epoch==command.caller.epoch;}
bool rackId(const juce::var& v,juce::Uuid& id){if(mcp::ipc::uuid(v,id))return true;if(!v.isString()||v.toString().length()!=32||!v.toString().containsOnly("0123456789abcdefABCDEF"))return false;id=juce::Uuid(v.toString());return !id.isNull();}
juce::var receiptView(const McpState::Receipt& r){return object({{"edit_id",r.edit.toDashedString()},
    {"transaction_id",r.changed?juce::var(mcp::ipc::decimal(r.historyId)):juce::var()},
    {"revision",mcp::ipc::decimal(r.revision)},{"changed",r.changed}});}
juce::var stageView(const McpState::Stage& stage){return object({{"edit_id",stage.id.toDashedString()},{"snapshot",stage.binding.token.toDashedString()},{"revision",mcp::ipc::decimal(stage.input.tag.editRevision)},{"diff",stage.diff}});}
}
juce::var InstrumentProcessor::mcpLinkedContextOwned() const {
    const auto link=groupLinkState.view();const auto& g=*groupControl;juce::Array<juce::var> peers;
    if(!link.intent||!groupLinkState.eligible()||!g.explicitConnection||!g.admitted||g.dirty||g.publishedBusy||g.conflicted
        ||g.pending||g.session!=aiClient.session()||aiClient.status().phase!=companion::ClientPhase::ready)return peers;
    for(std::size_t n=0;n<g.selection.peerCount;++n){
        const auto found=std::find_if(g.members.begin(),g.members.end(),[&](const auto& m){return m.intent.group==link.intent->group&&m.intent.member==g.selection.members[n]&&m.runtime!=link.runtime;});
        if(found==g.members.end()||found->busy||found->conflicted)return juce::Array<juce::var>();
        const auto text=[](const GroupText& t){const auto v=groupTextView(t);return juce::String::fromUTF8(v.data(),int(v.size()));};
        peers.add(object({{"runtime",juce::Uuid(found->runtime.getRawData()).toDashedString()},{"member",juce::Uuid(found->intent.member.getRawData()).toDashedString()},
            {"load_revision",mcp::ipc::decimal(found->loadRevision)},{"edit_revision",mcp::ipc::decimal(found->editRevision)},
            {"name",text(found->intent.name)},{"role",text(found->intent.role)},{"key",text(found->intent.key)},{"scale",text(found->intent.scale)},
            {"bpm",found->intent.bpm?juce::var(*found->intent.bpm):juce::var()},{"playback_mode",found->mode==PlaybackMode::externalMidi?"external_midi":"internal_pattern"},
            {"note_availability",int(found->noteAvailability)},{"note_count",int(found->noteCount)},{"control_availability",int(found->controlAvailability)}}));
    }
    return object({{"session",g.session.toDashedString()},{"load_revision",mcp::ipc::decimal(link.loadRevision)},{"peers",peers}});
}
bool InstrumentProcessor::mcpBindingCurrentOwned(const McpState::Binding& b,ProjectDocument* captured){
    const auto meta=mcpState->endpoint.metadata();if(!meta.enabled||meta.epoch!=b.epoch||b.token.isNull()||b.context!=aiContextRevision
        ||b.structure!=structuralGeneration.load()||b.reset!=resetGeneration.load()||b.edit!=editRevision.load()||b.source!=drumOwnership->current_->borrowed.generation
        ||!b.inventory||b.inventory->storeGeneration()!=libraryRuntime->store->observedGeneration())return false;
    auto storage=captured?nullptr:std::make_unique<ProjectDocument>();auto& source=captured?*captured:*storage;std::uint64_t edit,reset;
    return tryProjectSnapshotOwned(source,edit,reset)&&edit==b.edit&&reset==b.reset&&contentHash(source)+juce::JSON::toString(mcpLinkedContextOwned(),true)==b.content;
}
void InstrumentProcessor::retireMcpStageOwned(std::uint64_t now){
    if(now==0)now=GetTickCount64();
    if(!mcpState)return;
    const auto epoch=mcpState->endpoint.metadata().epoch;
    for(auto& r:mcpState->receipts)if(r&&(now-r->touched>=300000||r->caller.epoch!=epoch))r.reset();
    if(!mcpState->stage){drumOwnership->collect();return;}
    if(now-mcpState->stage->touched>=300000||!mcpBindingCurrentOwned(mcpState->stage->binding))mcpState->stage.reset();
    drumOwnership->collect();
}
bool InstrumentProcessor::refreshMcpSnapshot(ProjectDocument& source){
    RackInventoryPtr inventory;if(!captureAvailableRackInventory(inventory)){retryAvailableRackInventory();return false;}
    const std::lock_guard lock(restoreMutex);std::uint64_t edit,reset;
    if(!tryProjectSnapshotOwned(source,edit,reset))return false;
    if(source.sound.personalities)for(const auto& rack:source.sound.personalities->instances){
        juce::MemoryBlock bytes;RackDesign exact;std::optional<RackPackageRef> package;
        if(!encodeRackDesign(rack.design,bytes,RackValidationMode::instanceRecall)
            ||!resolveInventoryDesign(*inventory,rack.design.id,rack.design.revision,juce::SHA256(bytes).toHexString(),exact,package)||exact!=rack.design)return false;
    }
    auto& b=mcpState->snapshot;const auto meta=mcpState->endpoint.metadata();auto hash=contentHash(source);if(hash.isEmpty())return false;hash+=juce::JSON::toString(mcpLinkedContextOwned(),true);
    if(b.token.isNull()||b.epoch!=meta.epoch||b.edit!=edit||b.context!=aiContextRevision||b.structure!=structuralGeneration.load()
        ||b.reset!=reset||b.source!=drumOwnership->current_->borrowed.generation||b.content!=hash
        ||!b.inventory||b.inventory->id()!=inventory->id()){
        b={mcp::newRuntimeId(),meta.epoch,edit,aiContextRevision,structuralGeneration.load(),reset,drumOwnership->current_->borrowed.generation,hash,std::move(inventory)};
        mcpState->cursor.reset();
    }else b.inventory=std::move(inventory);
    retireMcpStageOwned(GetTickCount64());return true;
}
ProposalCommitResult InstrumentProcessor::commitMcpEdit(const mcp::Command& command){
    ProposalCommitResult result;std::shared_ptr<McpState::Stage> stage;
    if(command.control!=mcp::CommandControl::none||command.call.tool!=mcp::Tool::applyEdit
        ||!mcp::ipc::fields(command.call.arguments,{"instance_id","edit_id"})||!command.call.arguments["edit_id"].isString()){
        result.status=mcp::Status::invalidInput;return result;
    }
    {
        const std::lock_guard lock(restoreMutex);
        if(!mcpState||!mcpState->admission){result.status=mcp::Status::instanceGone;return result;}
        const auto meta=mcpState->endpoint.metadata();
        if(!meta.enabled){result.status=mcp::Status::unauthorized;return result;}
        if(command.call.instance!=meta.instance){result.status=mcp::Status::instanceGone;return result;}
        if(command.caller.epoch!=meta.epoch){result.status=mcp::Status::stale;return result;}
        if(mcp::commandExpired(command,GetTickCount64())){result.status=mcp::Status::hostControlUnavailable;return result;}
        retireMcpStageOwned(GetTickCount64());stage=mcpState->stage;
        if(!stage||command.call.arguments["edit_id"].toString()!=stage->id.toDashedString()){result.status=mcp::Status::stale;return result;}
        if(!owns(*stage,command)){result.status=mcp::Status::unauthorized;return result;}
        stage->touched=GetTickCount64();
    }
    // The input and actual owned source image survive callbacks which retire the public stage.
    ProposalCommitInput input=stage->input;
    if(!input.packageRefs.empty()){
        input.placement=libraryRuntime->client->placement(input.placementPreparation);
        if(!input.placement||input.placement->root!=input.inventory){
            libraryRuntime->client->requestPlacement(input.placementPreparation,input.inventory,input.packageRefs);
            input.placement=libraryRuntime->client->placement(input.placementPreparation);
        }
        if(!input.placement||input.placement->status==RackLibraryStatus::busy){result.status=mcp::Status::busy;return result;}
        if(input.placement->status!=RackLibraryStatus::ready||input.placement->root!=input.inventory
            ||!libraryRuntime->client->placementCurrent(input.placementPreparation,*input.placement)){
            result.status=mcp::Status::stale;
            const std::lock_guard lock(restoreMutex);
            if(mcpState&&mcpState->stage==stage)mcpState->stage.reset();
            return result;
        }
    }
    const auto diffText=juce::JSON::toString(stage->diff);
    result=commitProposal(input,[&]{const auto meta=mcpState->endpoint.metadata();return mcpState->admission&&meta.enabled
        &&meta.instance==command.call.instance&&meta.epoch==command.caller.epoch&&mcpState->stage==stage&&owns(*stage,command)
        &&GetTickCount64()-stage->touched<300000&&!mcp::commandExpired(command,GetTickCount64())
        &&mcpBindingCurrentOwned(stage->binding)&&stage->image&&stage->image->document==drumOwnership->current_->document&&stage->image->owners==drumOwnership->current_->owners&&stage->image->generation==drumOwnership->current_->borrowed.generation&&!mcp::commandExpired(command,GetTickCount64());});
    {const std::lock_guard lock(restoreMutex);
        if(result.status==mcp::Status::ready){const auto meta=mcpState->endpoint.metadata();
            if(meta.epoch==command.caller.epoch&&meta.enabled){auto& state=*mcpState;
                state.receipts[state.nextReceipt]=McpState::Receipt{stage->id,command.caller,GetTickCount64(),result.historyId,result.editRevision,result.changed};
                state.nextReceipt=(state.nextReceipt+1)%state.receipts.size();
                if(result.changed){state.lastEdit=diffText;state.lastEditHistory=result.historyId;state.lastEditRevision=result.editRevision;}}}
        else if(result.status!=mcp::Status::busy&&mcp::commandExpired(command,GetTickCount64()))result.status=mcp::Status::hostControlUnavailable;
        if(result.status!=mcp::Status::busy&&mcpState->stage==stage)mcpState->stage.reset();stage.reset();drumOwnership->collect();}
    return result;
}
void InstrumentProcessor::startMcp() {
    mcpState=std::make_unique<McpState>();mcpState->options=mcp::serviceOptionsForProcess();
    try{mcpState->admission=mcpState->options&&mcpState->endpoint.start(*mcpState->options,{mcp::newRuntimeId(),mcp::newRuntimeId(),soundName().isNotEmpty()?soundName():"AIR instance","0.1.0",true,mcp::ipc::object({})});}catch(...){mcpState->admission=false;mcpState->options.reset();}
}
void InstrumentProcessor::closeMcp() noexcept {
    if(mcpState){mcpState->admission=false;
        mcpState->pendingReply.reset();mcpState->priorityReplies={};mcpState->endpoint.close();
        {const std::lock_guard lock(restoreMutex);retireMcpPreviewOwned(true);mcpState->stage.reset();mcpState->receipts={};drumOwnership->collect();mcpState->snapshot={};mcpState->cursor.reset();}}
}
McpView InstrumentProcessor::mcpView() const {
    const std::lock_guard lock(restoreMutex);
    McpView view;if(!mcpState){view.enabled=false;view.status="unavailable";return view;}
    const auto meta=mcpState->endpoint.metadata();view.instance=meta.instance;view.epoch=meta.epoch;view.enabled=meta.enabled;
    const auto observation=mcpState->endpoint.observation();view.clientsAvailable=observation.available;view.clients=observation.clients;view.requests=observation.requests;
    view.label=recoveryDocument->sound.name.isNotEmpty()?recoveryDocument->sound.name:meta.label;
    if(mcpState->lastEditHistory)view.lastEdit="Transaction "+mcp::ipc::decimal(mcpState->lastEditHistory)+" · revision "+mcp::ipc::decimal(mcpState->lastEditRevision)+"\n"+mcpState->lastEdit;
    view.connected=mcpState->admission.load()&&mcpState->endpoint.connected();view.status=!meta.enabled?"disabled":view.connected?"connected":"unavailable";return view;
}
void InstrumentProcessor::setMcpEnabled(bool enabled) {
    if(!mcpState||!mcpState->admission.load())return;auto meta=mcpState->endpoint.metadata();if(meta.enabled==enabled)return;
    try{const std::lock_guard lock(restoreMutex);retireMcpPreviewOwned();mcpState->stage.reset();mcpState->receipts={};mcpState->snapshot={};mcpState->cursor.reset();meta.enabled=enabled;meta.epoch=mcp::newRuntimeId();mcpState->endpoint.update(meta);}
    catch(...){closeMcp();}
}
void InstrumentProcessor::rotateMcpEpoch() {
    if(!mcpState||!mcpState->admission.load())return;
    try{const std::lock_guard lock(restoreMutex);retireMcpPreviewOwned();mcpState->stage.reset();mcpState->receipts={};mcpState->snapshot={};mcpState->cursor.reset();auto meta=mcpState->endpoint.metadata();meta.epoch=mcp::newRuntimeId();meta.label=recoveryDocument->sound.name.isNotEmpty()?recoveryDocument->sound.name:"AIR instance";mcpState->endpoint.update(meta);}
    catch(...){closeMcp();}
}
mcp::ToolResult InstrumentProcessor::executeMcp(const mcp::Command& command) try {
    using mcp::Status;using mcp::Tool;
    if(!mcpState||!mcpState->admission.load())return {Status::instanceGone,{}};
    if(command.control!=mcp::CommandControl::none){
        const std::lock_guard lock(restoreMutex);
        controlMcpPreviewOwned(command);
        if(command.control==mcp::CommandControl::epochLost||command.control==mcp::CommandControl::revoke)mcpState->receipts={};
        else if(command.control==mcp::CommandControl::ownerDisconnected)
            for(auto& r:mcpState->receipts)if(r&&r->caller.owner==command.caller.owner&&r->caller.epoch==command.caller.epoch)r.reset();
        if(mcpState->stage){const auto& stage=*mcpState->stage;
            if(command.control==mcp::CommandControl::epochLost||command.control==mcp::CommandControl::revoke
                ||(command.control==mcp::CommandControl::ownerDisconnected&&stage.caller.owner==command.caller.owner&&stage.caller.epoch==command.caller.epoch)
                ||(command.control==mcp::CommandControl::cancel&&owns(stage,command)&&(stage.serial==command.cancelledSerial
                    ||(command.call.tool==mcp::Tool::applyEdit&&command.call.arguments["edit_id"].toString()==stage.id.toDashedString()))))mcpState->stage.reset();}
        return {Status::ready,{}};
    }
    const auto meta=mcpState->endpoint.metadata();
    if(command.call.instance!=meta.instance)return {Status::instanceGone,{}};
    if(!meta.enabled)return {Status::unauthorized,{}};
    if(command.caller.epoch!=meta.epoch)return {Status::stale,{}};
    if(command.caller.owner.isNull())return {Status::unauthorized,{}};
    const auto executeExpiredTick=GetTickCount64();if(mcp::commandExpired(command,executeExpiredTick)){mcp::detail::EndpointTestAccess::recordDiagnostic(mcpState->endpoint,mcp::detail::RequestStage::executeExpired,command,int(Status::hostControlUnavailable),executeExpiredTick);return {Status::hostControlUnavailable,{}};}
    const auto& args=command.call.arguments;const auto tool=command.call.tool;
    if(tool==Tool::renderPreview||tool==Tool::getJob||tool==Tool::cancelJob||tool==Tool::playPreview||tool==Tool::stopPreview||tool==Tool::resourceList||tool==Tool::resourceRead)return executeMcpPreview(command);
    if(tool==Tool::applyEdit){
        if(!mcp::ipc::fields(args,{"instance_id","edit_id"})||!args["edit_id"].isString())return {Status::invalidInput,{}};
        {
            const std::lock_guard lock(restoreMutex);retireMcpStageOwned(GetTickCount64());
            for(auto& r:mcpState->receipts)if(r&&args["edit_id"].toString()==r->edit.toDashedString()){
                if(r->caller.owner!=command.caller.owner||r->caller.epoch!=command.caller.epoch)return {Status::unauthorized,{}};
                r->touched=GetTickCount64();return {Status::ready,receiptView(*r)};}
        }
        const auto result=commitMcpEdit(command);
        if(result.status!=Status::ready)return {result.status,{}};
        // A reentrant lifecycle change may already have retired this connection's receipt.
        const std::lock_guard lock(restoreMutex);
        for(const auto& r:mcpState->receipts)if(r&&r->caller.owner==command.caller.owner&&r->caller.epoch==command.caller.epoch&&r->edit.toDashedString()==args["edit_id"].toString())return {Status::ready,receiptView(*r)};
        return {Status::stale,{}};
    }
    if(tool==Tool::undoEdit||tool==Tool::redoEdit){
        std::uint64_t transaction=0,expected=0;
        if(!mcp::ipc::fields(args,{"instance_id","transaction_id","expected_revision"})||!mcp::ipc::decimal(args["transaction_id"],transaction)||!transaction
            ||!mcp::ipc::decimal(args["expected_revision"],expected))return {Status::invalidInput,{}};
        std::uint64_t revision=0;
        {
            const std::lock_guard lock(restoreMutex);retireMcpStageOwned(GetTickCount64());
            auto found=std::find_if(mcpState->receipts.begin(),mcpState->receipts.end(),[&](const auto& r){return r&&r->changed&&r->historyId==transaction;});
            if(found==mcpState->receipts.end())return {Status::stale,{}};
            auto& receipt=**found;if(receipt.caller.owner!=command.caller.owner||receipt.caller.epoch!=command.caller.epoch)return {Status::unauthorized,{}};
            if(aiApplying||aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed||aiRefinementBackup||aiDelayedApply||groupControl->waiting
                ||gesture->index!=parameterCount||!renderedPreview.stopped())return {Status::busy,{}};
            if(mcpPreviewOwnedByOther(command))return {Status::busy,{}};
            if(mcpState->stage&&!owns(*mcpState->stage,command))return {Status::busy,{}};
            if(editRevision.load()!=expected)return {Status::stale,{}};
            const auto& history=*editHistory;const bool undo=tool==Tool::undoEdit;
            if((undo&&(history.cursor==0||history.actionIds[history.cursor-1]!=transaction))
                ||(!undo&&(history.cursor==history.count||history.actionIds[history.cursor]!=transaction)))return {Status::stale,{}};
            if(mcp::commandExpired(command,GetTickCount64()))return {Status::hostControlUnavailable,{}};
            aiHistoryRejected=false;
            if(!(undo?undoHistoryAction():redoHistoryAction())||aiHistoryRejected)return {Status::stale,{}};
            revision=editRevision.load();receipt.touched=GetTickCount64();
        }
        triggerAsyncUpdate();return {Status::ready,object({{"transaction_id",mcp::ipc::decimal(transaction)},{"revision",mcp::ipc::decimal(revision)}})};
    }
    if(tool==Tool::getEdit||tool==Tool::discardEdit){
        if(!mcp::ipc::fields(args,{"instance_id","edit_id"})||!args["edit_id"].isString())return {Status::invalidInput,{}};
        const std::lock_guard lock(restoreMutex);retireMcpStageOwned(GetTickCount64());const auto stage=mcpState->stage;
        if(!stage||args["edit_id"].toString()!=stage->id.toDashedString())return {Status::stale,{}};
        if(!owns(*stage,command))return {Status::unauthorized,{}};
        if(aiApplying)return {Status::busy,{}};stage->touched=GetTickCount64();
        if(tool==Tool::discardEdit){retireMcpPreviewOwned();mcpState->stage.reset();return {Status::ready,object({})};}
        return {Status::ready,stageView(*stage)};
    }
    if(tool!=Tool::getState&&tool!=Tool::getCatalog&&tool!=Tool::prepareEdit)return {Status::unavailable,{}};
    if(!mcp::ipc::fields(args,tool==Tool::getState?std::initializer_list<const char*>{"instance_id"}:
        tool==Tool::getCatalog?std::initializer_list<const char*>{"instance_id","snapshot","cursor","kind","limit","design_id","design_revision","content_sha256"}:
        std::initializer_list<const char*>{"instance_id","snapshot","policy","edits"}))return {Status::invalidInput,{}};
    std::vector<juce::String> names;if(tool==Tool::prepareEdit&&args["edits"]["name"].isString())names=soundNamesInUse();
    auto source=std::make_unique<ProjectDocument>();if(!refreshMcpSnapshot(*source))return {Status::unavailable,{}};
    const std::lock_guard lock(restoreMutex);auto& snapshot=mcpState->snapshot;
    if(!mcpBindingCurrentOwned(snapshot,source.get()))return {Status::stale,{}};
    if(tool==Tool::getState){
        juce::var state;if(!ai::catalogSliceFor(*source,ai::CatalogRendering::ownedLocal,0,0,state))return {Status::unavailable,{}};
        auto* out=state.getDynamicObject();out->removeProperty("controls");juce::Array<juce::var> values,locks;const auto registry=parameterRegistry();
        for(std::size_t n=0;n<parameterCount;++n)values.add(object({{"id",registry[n].id},{"normalized",double(source->sound.values[n])}}));for(auto v:source->sound.controlLocks)locks.add(v);out->setProperty("control_locks",locks);
        out->setProperty("values",values);out->setProperty("name",source->sound.name);out->setProperty("instances",instances(source->sound));
        out->setProperty("snapshot",snapshot.token.toDashedString());out->setProperty("revision",mcp::ipc::decimal(snapshot.edit));
        out->setProperty("context_revision",mcp::ipc::decimal(snapshot.context));
        const auto bpm=aiContext.auditionBpm?aiContext.auditionBpm:currentHostBpm();
        out->setProperty("musical_context",object({{"role",juce::String(aiContext.role)},{"key",juce::String(aiContext.key)},{"scale",juce::String(aiContext.scale)}}));
        out->setProperty("bpm",bpm?juce::var(*bpm):juce::var());
        out->setProperty("linked_context",mcpLinkedContextOwned());
        return {Status::ready,state};
    }
    if(args.hasProperty("snapshot")&&(!args["snapshot"].isString()||args["snapshot"].toString()!=snapshot.token.toDashedString()))return {Status::stale,{}};
    if(tool==Tool::getCatalog){
        const auto kind=args.hasProperty("kind")?args["kind"].toString():juce::String("parameters");std::size_t offset=0;
        if(args.hasProperty("cursor")){
            const auto& c=mcpState->cursor;if(!args["snapshot"].isString()||!args["cursor"].isString()||!c||args["cursor"].toString()!=c->id.toDashedString()
                ||c->snapshot!=snapshot.token||c->kind!=kind)return {Status::stale,{}};offset=c->next;
        }
        juce::var entries;std::size_t next=0;bool complete=true;
        if(kind=="parameters"){
            std::size_t limit;if(!limitFor(args,128,256,limit)||offset>parameterCount)return {Status::invalidInput,{}};
            juce::var page;const auto count=(std::min)(limit,parameterCount-offset);
            if(!ai::catalogSliceFor(*source,ai::CatalogRendering::ownedLocal,offset,count,page))return {Status::unavailable,{}};
            entries=page["controls"];next=offset+count;complete=next==parameterCount;
        }else if(kind=="racks"){
            std::size_t limit;if(!limitFor(args,65536,1048576,limit))return {Status::invalidInput,{}};
            const auto rows=snapshot.inventory->rows();if(offset>rows.size())return {Status::invalidInput,{}};
            RackCatalogPage page;page.snapshot=snapshot.inventory->id();page.next=offset;page.complete=offset==rows.size();
            for(std::size_t n=offset;n<rows.size();++n){
                const auto& metadata=rows[n].metadata;RackDesign design;std::optional<RackPackageRef> package;
                if(!resolveInventoryDesign(*snapshot.inventory,metadata.design,metadata.revision,metadata.canonicalSha256,design,package))return {Status::stale,{}};
                page.entries.push_back({std::move(design),metadata.canonicalSha256});page.next=n+1;page.complete=page.next==rows.size();
                std::size_t bytes=0;if(!rackCatalogEscapedSize(rackCatalogPageDescriptor(page),limit,bytes)){
                    page.entries.pop_back();page.next=n;page.complete=false;if(page.entries.empty())return {Status::capacity,{}};break;
                }
            }
            entries=rackCatalogPageDescriptor(page)["entries"];next=page.next;complete=page.complete;
        }else if(kind=="definition"){
            if(args.hasProperty("cursor")||args.hasProperty("limit"))return {Status::invalidInput,{}};
            juce::Uuid id;std::uint64_t revision;
            if(!rackId(args["design_id"],id)||!mcp::ipc::decimal(args["design_revision"],revision)||!args["content_sha256"].isString())return {Status::invalidInput,{}};
            RackDesign design;std::optional<RackPackageRef> package;if(!resolveInventoryDesign(*snapshot.inventory,id,revision,args["content_sha256"].toString(),design,package))return {Status::stale,{}};
            juce::MemoryBlock bytes;if(!encodeRackDesign(design,bytes,RackValidationMode::instanceRecall))return {Status::unavailable,{}};
            juce::var definition;if(!ai::parseStrictJson({static_cast<const char*>(bytes.getData()),bytes.getSize()},definition,maximumRackCatalogBytes))return {Status::unavailable,{}};
            return {Status::ready,object({{"snapshot",snapshot.token.toDashedString()},{"definition",definition}})};
        }else return {Status::invalidInput,{}};
        juce::var cursor;if(!complete){mcpState->cursor=McpState::Cursor{mcp::newRuntimeId(),snapshot.token,kind,next};cursor=mcpState->cursor->id.toDashedString();}
        else mcpState->cursor.reset();
        ai::CatalogAuthority authority;authority.root={meta.instance,1,snapshot.edit,snapshot.context};authority.snapshot=snapshot.inventory->id();authority.rendering=ai::CatalogRendering::ownedLocal;
        juce::var inventory;if(!ai::catalogInventoryFor(*snapshot.inventory,ai::CatalogRendering::ownedLocal,inventory))return {Status::capacity,{}};
        authority.descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();
        return {Status::ready,object({{"snapshot",snapshot.token.toDashedString()},{"kind",kind},{"entries",entries},{"cursor",cursor},{"edits_schema",ai::controlEditsSchema({},&authority)}})};
    }
    if(!args["snapshot"].isString()||args["snapshot"].toString()!=snapshot.token.toDashedString())return {Status::stale,{}};
    if(aiApplying||aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed||aiRefinementBackup||aiDelayedApply||groupControl->waiting)return {Status::busy,{}};
    if(mcpState->stage||mcpState->preview)return {Status::busy,{}};
    ai::ProposalPolicy policy;if(!policyFor(args["policy"],policy)||!args["edits"].getDynamicObject())return {Status::invalidInput,{}};
    auto authority=std::make_shared<ai::CatalogAuthority>();authority->root={snapshot.token,1,snapshot.edit,snapshot.context};
    authority->snapshot=snapshot.inventory->id();authority->rendering=ai::CatalogRendering::ownedLocal;
    juce::var inventory;if(!ai::catalogInventoryFor(*snapshot.inventory,ai::CatalogRendering::ownedLocal,inventory))return {Status::capacity,{}};
    authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();
    if(const auto* selections=args["edits"]["catalog_selections"].getArray()){
        if(selections->size()>12)return {Status::invalidInput,{}};
        for(const auto& selection:*selections){juce::Uuid id;std::uint64_t revision;
            if(!rackId(selection["design"],id)||!mcp::ipc::decimal(selection["revision"],revision)||!selection["content_sha256"].isString())return {Status::invalidInput,{}};
            if(std::any_of(authority->definitions.begin(),authority->definitions.end(),[&](const auto& d){return d.design==id&&d.revision==revision&&d.contentSha256==selection["content_sha256"].toString();}))continue;
            RackDesign design;std::optional<RackPackageRef> package;if(!resolveInventoryDesign(*snapshot.inventory,id,revision,selection["content_sha256"].toString(),design,package))return {Status::invalidInput,{}};
            ai::CatalogDefinition definition{id,revision,selection["content_sha256"].toString(),{}};
            if(!encodeRackDesign(design,definition.canonical,RackValidationMode::instanceRecall))return {Status::unavailable,{}};
            authority->definitions.push_back(std::move(definition));
        }
    }
    ai::ProposalEdits edits;ai::ProposalDecodeReason reason;
    if(!ai::decodeControlEdits(juce::JSON::toString(args["edits"],true,22).toStdString(),policy,authority.get(),edits,&reason))return {Status::invalidInput,object({{"reason",ai::proposalDecodeReasonName(reason)}})};
    auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
    juce::String automationWarning;
    const auto admission=ai::admitProposal(*source,policy,edits,*authority,*snapshot.inventory,*candidate,trace.get(),nullptr,&automationWarning);
    if(admission!=ai::ProposalStatus::accepted)return {Status::invalidInput,object({{"proposal_status",int(admission)}})};
    const auto reviewedName=resolveProposalName(edits.name,names);
    if(reviewedName.isNotEmpty())candidate->sound.name=reviewedName;
    auto image=drumOwnership->captureMcpImage();if(!image)return {Status::capacity,{}};
    auto assignments=std::make_unique<AssignmentStage>();DrumCommitError error;
    if(!validateFreshSourceOwnerAdmissionOwned(source->sound,candidate->sound,trace.get(),error)
        ||!buildAssignmentStageOwned(source->sound,*trace,image->document->slots(),image->owners,*assignments,error))return {Status::invalidInput,object({{"source_status",int(error)}})};
    if(!drumOwnership->intendedFits(assignments->owners))return {Status::capacity,{}};
    auto diff=difference(*source,*candidate);if(!diff.getDynamicObject())return {Status::capacity,{}};
    diff.getDynamicObject()->setProperty("automation_warning",automationWarning);
    juce::Array<juce::var> effects,pads;
    for(std::size_t n=0;n<trace->count;++n){const auto& e=trace->effects[n];effects.add(object({{"operation",int(e.operation)},
        {"kind",e.kind==SourceAssignmentEffectKind::clear?"clear":e.kind==SourceAssignmentEffectKind::copy?"copy":"preserve"},
        {"bank",int(e.bank)},{"copy_from",e.copyFrom==noSlot?juce::var():juce::var(int(e.copyFrom))},
        {"before_owner",e.beforeOwner.toDashedString()},{"after_owner",e.afterOwner.toDashedString()},{"before_unit",int(e.beforeUnit)},
        {"after_unit",int(e.afterUnit)},{"before_use",int(e.beforeUse)},{"after_use",int(e.afterUse)},
        {"invalidate_target",e.invalidateTarget},{"full_numeric_bank",e.fullNumericBank}}));}
    for(std::size_t n=0;n<assignments->scope.before.size();++n){const auto& before=assignments->scope.before[n];const auto& after=assignments->scope.after[n];
        pads.add(object({{"bank",int(before.bank)},{"pad",int(before.pad)},{"before",padDescription(before.metadata)},{"after",padDescription(after.metadata)}}));}
    diff.getDynamicObject()->setProperty("source_assignments",object({{"effects",effects},{"pads",pads}}));
    if(!mcp::ipc::encodedSize(diff,mcp::maximumResultBytes-4096))return {Status::capacity,{}};
    if(mcp::commandExpired(command,GetTickCount64()))return {Status::hostControlUnavailable,{}};
    if(!mcpBindingCurrentOwned(snapshot))return {Status::stale,{}};
    auto staged=std::make_shared<McpState::Stage>();staged->id=mcp::newRuntimeId();staged->caller=command.caller;staged->touched=GetTickCount64();staged->serial=command.serial;
    staged->binding=snapshot;staged->input={authority->root,snapshot.structure,snapshot.reset,policy,std::move(edits),std::move(authority),reviewedName};
    staged->input.inventory=snapshot.inventory;
    for(const auto& selection:staged->input.edits.catalogSelections){
        RackDesign design;std::optional<RackPackageRef> package;
        if(!resolveInventoryDesign(*snapshot.inventory,selection.design,selection.revision,selection.contentSha256,design,package))return {Status::stale,{}};
        const bool retained=source->sound.personalities&&std::any_of(source->sound.personalities->instances.begin(),source->sound.personalities->instances.end(),[&](const auto& rack){return rack.design==design;});
        if(package&&!retained&&std::find(staged->input.packageRefs.begin(),staged->input.packageRefs.end(),*package)==staged->input.packageRefs.end())staged->input.packageRefs.push_back(*package);
    }
    if(!staged->input.packageRefs.empty())staged->input.placementPreparation=libraryRuntime->client->createPlacementPreparation();
    staged->candidate=std::move(candidate);staged->image=std::move(image);staged->diff=std::move(diff);
    mcpState->stage=std::move(staged);
    return {Status::ready,stageView(*mcpState->stage)};
}catch(const std::bad_alloc&){return {mcp::Status::capacity,{}};}catch(...){closeMcp();return {mcp::Status::unavailable,{}};}
void InstrumentProcessor::pollMcp() {
    if(!mcpState||!mcpState->admission.load())return;
    const auto control=[&](const mcp::Command& command){
        if(mcpState->pendingReply){const auto& pending=*mcpState->pendingReply;
            if(command.control==mcp::CommandControl::epochLost||command.control==mcp::CommandControl::revoke
                ||(command.control==mcp::CommandControl::cancel&&command.cancelledSerial==pending.reply.serial)
                ||(command.control==mcp::CommandControl::ownerDisconnected&&command.caller.owner==pending.caller.owner))mcpState->pendingReply.reset();}
        for(auto& pending:mcpState->priorityReplies)if(pending&&(command.control==mcp::CommandControl::epochLost||command.control==mcp::CommandControl::revoke
            ||(command.control==mcp::CommandControl::cancel&&command.cancelledSerial==pending->reply.serial)
            ||(command.control==mcp::CommandControl::ownerDisconnected&&command.caller.owner==pending->caller.owner)))pending.reset();
        (void)executeMcp(command);
    };
    for(auto& pending:mcpState->priorityReplies)if(pending&&mcpState->endpoint.reply(std::move(pending->reply)))pending.reset();
    for(unsigned n=0;n<64;++n){mcp::Command command;if(!mcpState->endpoint.takeControl(command))break;
        if(command.control!=mcp::CommandControl::none){control(command);continue;}
        auto reply=mcp::Reply{command.serial,executeMcp(command)};
        if(!mcpState->endpoint.reply(std::move(reply))){const auto slot=std::find_if(mcpState->priorityReplies.begin(),mcpState->priorityReplies.end(),[](const auto& p){return !p;});
            if(slot!=mcpState->priorityReplies.end())*slot=McpState::PendingReply{std::move(reply),command.caller};}
    }
    {const std::lock_guard lock(restoreMutex);retireMcpStageOwned();pollMcpPreviewOwned();}
    const auto publish=[&]{
        if(!mcpState->pendingReply)return true;
        if(!mcpState->endpoint.reply(std::move(mcpState->pendingReply->reply)))return false;
        mcpState->pendingReply.reset();return true;
    };
    if(!publish())return;
    unsigned normal=0;for(unsigned n=0;n<66&&normal<2;++n){mcp::Command command;if(!mcpState->endpoint.take(command))break;
        if(command.control!=mcp::CommandControl::none){control(command);continue;}
        ++normal;mcp::detail::EndpointTestAccess::recordDiagnostic(mcpState->endpoint,mcp::detail::RequestStage::executeBegin,command);mcp::Reply reply{command.serial,executeMcp(command)};mcp::detail::EndpointTestAccess::recordDiagnostic(mcpState->endpoint,mcp::detail::RequestStage::executeEnd,command,int(reply.result.status));
        if(!mcp::ipc::encodedSize(mcp::ipc::reply(reply),mcp::maximumResultBytes))reply.result={mcp::Status::capacity,{}};
        mcpState->pendingReply=McpState::PendingReply{std::move(reply),command.caller};
        if(!publish())return;
    }
}
}
