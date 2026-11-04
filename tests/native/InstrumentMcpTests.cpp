#include "InstrumentProcessor.h"
#include "InstrumentMcp.h"
#include "BlankInstance.h"
#include "OwnedFixture.h"
#include "IsolatedRackLibrary.h"
#include "FrozenRackExamples.h"
#include <chrono>
#include <future>
#include <thread>
#include <juce_gui_basics/juce_gui_basics.h>
#include <windows.h>
#include <iostream>
#include <set>
namespace airack {struct InstrumentDrumTestAccess {
    static auto capture(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->captureMcpImage();}
    static std::uint64_t reserved(InstrumentProcessor& p){return p.drumOwnership->reservedDecoded_;}
    static std::uint64_t retained(InstrumentProcessor& p){return p.drumOwnership->retainedBytes();}
    static const drums::SampleAsset* asset(InstrumentProcessor& p){return p.drumOwnership->current_->owners[0].get();}
    static bool rooted(InstrumentProcessor& p,const drums::SampleAsset* asset){return std::any_of(p.drumOwnership->ledger_.begin(),p.drumOwnership->ledger_.end(),[&](const auto& a){return a.get()==asset;});}
};}
namespace airack {struct InstrumentMcpTestAccess {static auto inventory(InstrumentProcessor& p){return p.libraryRuntime->client->inventory(p.inventoryPreparation);}static auto apply(InstrumentProcessor& p,const mcp::Command& c){return p.commitMcpEdit(c);}static void expire(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.retireMcpStageOwned(GetTickCount64()+300000);}static std::unique_ptr<ProjectDocument> candidate(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.mcpState&&p.mcpState->stage&&p.mcpState->stage->candidate?std::make_unique<ProjectDocument>(*p.mcpState->stage->candidate):nullptr;}};}
using namespace airack;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<line<<" "<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
auto object(std::initializer_list<std::pair<const char*,juce::var>> v){return mcp::ipc::object(v);}
auto json(const char* text){return juce::JSON::parse(text);}
std::size_t index(const char* id){const auto r=parameterRegistry();auto i=std::find_if(r.begin(),r.end(),[&](const auto& p){return p.id==id;});CHECK(i!=r.end());return std::size_t(i-r.begin());}
std::uint64_t revision(InstrumentProcessor& p){auto d=std::make_unique<ProjectDocument>();std::uint64_t e=0,r=0;CHECK(p.tryProjectSnapshot(*d,e,r));return e;}
mcp::ToolResult call(InstrumentProcessor& p,mcp::Caller caller,mcp::Tool tool,juce::var args=object({}),mcp::Status expected=mcp::Status::ready){
    if(tool==mcp::Tool::getState||tool==mcp::Tool::getCatalog||tool==mcp::Tool::prepareEdit)CHECK(testing::readyRackInventory(p));
    mcp::Command c;c.serial=1;c.deadlineMs=GetTickCount64()+(tool==mcp::Tool::renderPreview?60000:2000);c.caller=caller;c.call={tool,p.mcpView().instance,args};
    auto result=p.executeMcp(c);if(result.status!=expected)std::cerr<<"tool="<<int(tool)<<" actual="<<int(result.status)<<" expected="<<int(expected)<<" data="<<juce::JSON::toString(result.data,true)<<"\n";CHECK(result.status==expected);return result;
}
juce::var prepare(InstrumentProcessor& p,mcp::Caller c,const juce::var& edits,mcp::Status expected=mcp::Status::ready,juce::var policy=object({})){
    auto state=call(p,c,mcp::Tool::getState);
    return call(p,c,mcp::Tool::prepareEdit,object({{"snapshot",state.data["snapshot"]},{"policy",policy},{"edits",edits}}),expected).data;
}
void staging(){
    std::cout<<"staging_no_live_change_and_panel_independence\n";
    auto processor=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*processor));auto& p=*processor;
    const auto view=p.mcpView();const auto caller=mcp::Caller{juce::Uuid(),view.epoch};
    const auto before=p.control(index("global.master_level")).getValue();const auto edit=revision(p);const auto path=p.soundPresetPath();
    auto staged=prepare(p,caller,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})"));
    CHECK(staged["edit_id"].isString());CHECK(staged["diff"].getDynamicObject());const auto* diff=staged["diff"]["parameters"].getArray();CHECK(diff&&diff->size()==1&&(*diff)[0]["id"].toString()=="global.master_level"&&float((*diff)[0]["after"])==.23f);
    CHECK(staged["diff"]["automation_warning"].isString()&&staged["diff"]["automation_warning"].toString().isEmpty());
    CHECK(call(p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}})).data["diff"]==staged["diff"]);
    CHECK(p.control(index("global.master_level")).getValue()==before);CHECK(revision(p)==edit);CHECK(p.soundPresetPath()==path);
    p.closeAiPanel();call(p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}}));
    call(p,{juce::Uuid(),view.epoch},mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}}),mcp::Status::unauthorized);
    prepare(p,{juce::Uuid(),view.epoch},json("{}"),mcp::Status::busy);
    CHECK(!p.beginAiRequest({"An edit","model",{}}));
    call(p,caller,mcp::Tool::discardEdit,object({{"edit_id",staged["edit_id"]}}));
    staged=prepare(p,caller,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.24}]})"));
    CHECK(p.beginParameterGesture(index("global.master_level")));CHECK(p.setParameterGestureValue(index("global.master_level"),.41f));CHECK(p.endParameterGesture(index("global.master_level")));
    call(p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}}),mcp::Status::stale);
}
void admission(){
    std::cout<<"native_admission_and_authority\n";auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));
    auto caller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};
    prepare(*p,caller,json(R"({"parameter_edits":[{"id":"no.such.parameter","normalized":0.2}]})"),mcp::Status::invalidInput);
    prepare(*p,caller,json(R"({"parameter_edits":[{"id":"synth.2.analog.filter.cutoff","normalized":0.2}]})"),mcp::Status::invalidInput);
    prepare(*p,caller,json(R"({"macro_edits":[{"index":1,"target_id":"global.master_level","amount":0.2}]})"),mcp::Status::invalidInput);
    prepare(*p,caller,json("{}"),mcp::Status::invalidInput,object({{"scope","both"},{"require_pattern",true}}));
    auto macroStage=prepare(*p,caller,json(R"({"macro_edits":[{"index":1,"target_id":"global.master_level","amount":0.2}]})"),mcp::Status::ready,object({{"allow_macro_changes",true}}));call(*p,caller,mcp::Tool::discardEdit,object({{"edit_id",macroStage["edit_id"]}}));
    auto source=std::make_unique<SoundDocument>();std::uint64_t e;CHECK(p->trySoundSnapshot(*source,e));source->controlLocks[index("global.master_level")]=true;source->synthLocks[1]=true;
    CHECK(p->applySoundEdit(*source,SoundEditOrigin::manual,e));
    prepare(*p,caller,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.2}]})"),mcp::Status::invalidInput);
    prepare(*p,caller,json(R"({"parameter_edits":[{"id":"synth.2.enabled","normalized":1}]})"),mcp::Status::invalidInput);
    auto staged=prepare(*p,caller,json(R"({"parameter_edits":[{"id":"output.gain_db","normalized":0.2}]})"));
    mcp::Command disconnected;disconnected.control=mcp::CommandControl::ownerDisconnected;disconnected.caller=caller;p->executeMcp(disconnected);
    call(*p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}}),mcp::Status::stale);
    staged=prepare(*p,caller,json("{}"));p->setMcpEnabled(false);
    call(*p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}}),mcp::Status::unauthorized);
}
struct Listener final:juce::AudioProcessorParameter::Listener {
    int starts=0,ends=0,values=0;std::function<void()> begin;
    void parameterValueChanged(int,float)override{++values;}
    void parameterGestureChanged(int,bool b)override{if(b){++starts;if(begin)begin();}else ++ends;}
};
bool rename(InstrumentProcessor& p,const juce::String& name){auto source=std::make_unique<SoundDocument>();std::uint64_t edit;if(!p.trySoundSnapshot(*source,edit))return false;source->name=name;return p.applySoundEdit(*source,SoundEditOrigin::manual,edit);}
void named_stage(){
    std::cout<<"named_stage_review_matches_apply\n";
    for(int mode=0;mode<5;++mode){
        std::cout<<"NAME_CASE "<<mode<<'\n';auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime()),peer=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));CHECK(copperArcInstance(*peer));
        const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});CHECK(juce::File::isAbsolutePath(root));if(!juce::File::isAbsolutePath(root))return;
        const auto folder=juce::File(root).getChildFile("mcp-name-"+juce::Uuid().toString());CHECK(folder.createDirectory());CHECK(p->saveSoundPreset(folder.getChildFile("Base.airack")));
        const juce::String requested="Warm Pad";
        if(mode==0||mode==4)CHECK(rename(*peer,requested));if(mode==0)CHECK(folder.getChildFile("warm pad 2.airack").create());
        p->pollMcp(); // Drain the endpoint startup lifecycle event before binding this direct caller.
        auto before=std::make_unique<ProjectDocument>();std::uint64_t edit=0,reset=0;CHECK(p->tryProjectSnapshot(*before,edit,reset));const auto path=p->soundPresetPath();
        const auto caller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};auto staged=prepare(*p,caller,object({{"name",requested},{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.23}})}}}));
        CHECK(staged["snapshot"].isString());CHECK(staged["edit_id"].isString());if(!staged["edit_id"].isString())continue;
        call(*p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}}));
        const auto reviewed=staged["diff"]["name"]["after"].toString();CHECK(reviewed==(mode==0?juce::String("Warm Pad 3"):mode==4?juce::String("Warm Pad 2"):requested));
        auto afterPrepare=std::make_unique<ProjectDocument>();std::uint64_t observed=0;CHECK(p->tryProjectSnapshot(*afterPrepare,observed));CHECK(*afterPrepare==*before&&observed==edit&&p->soundPresetPath()==path);
        if(mode==0){
            const auto candidate=InstrumentMcpTestAccess::candidate(*p);CHECK(candidate&&candidate->sound.name==reviewed);
            const auto reviewedDiff=staged["diff"].clone();const auto started=call(*p,caller,mcp::Tool::renderPreview,object({{"edit_id",staged["edit_id"]}}));CHECK(started.data["job_id"].isString());
            if(started.data["job_id"].isString()){
                const auto job=object({{"job_id",started.data["job_id"]}});const auto deadline=GetTickCount64()+60000;juce::var status;
                do{p->pollMcp();const auto result=call(*p,caller,mcp::Tool::getJob,job);status=result.data["job_status"];if(status.toString()!="running")break;juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}while(GetTickCount64()<deadline);
                CHECK(status.toString()=="ready");auto afterPreview=std::make_unique<ProjectDocument>();CHECK(p->tryProjectSnapshot(*afterPreview,observed));CHECK(*afterPreview==*before&&observed==edit&&p->soundPresetPath()==path);
                CHECK(call(*p,caller,mcp::Tool::getEdit,object({{"edit_id",staged["edit_id"]}})).data["diff"]==reviewedDiff);
                call(*p,caller,mcp::Tool::cancelJob,job);p->pollMcp();
            }
        }
        if(mode==1)CHECK(rename(*peer,reviewed));
        if(mode==4)CHECK(rename(*peer,"Changed peer name"));
        Listener listener;bool fired=false;listener.begin=[&]{if(fired)return;fired=true;if(mode==2)CHECK(rename(*peer,reviewed));};
        const auto field=index("global.master_level");p->control(field).addListener(&listener);
        mcp::Command command;command.deadlineMs=GetTickCount64()+2000;command.caller=caller;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",staged["edit_id"]}})};
        const auto result=InstrumentMcpTestAccess::apply(*p,command);const bool accepted=mode==0||mode==3||mode==4;
        CHECK(result.status==(accepted?mcp::Status::ready:mcp::Status::stale));CHECK(listener.starts==listener.ends);
        auto committed=std::make_unique<ProjectDocument>();CHECK(p->tryProjectSnapshot(*committed,observed));
        if(accepted){CHECK(committed->sound.name==reviewed);CHECK(p->soundPresetPath()==folder.getChildFile(reviewed+".airack").getFullPathName());CHECK(observed==edit+1);}
        else{CHECK(*committed==*before&&observed==edit&&p->soundPresetPath()==path);CHECK(listener.values==0);}
        if(mode==2)CHECK(fired&&listener.starts==1);p->control(field).removeListener(&listener);
    }
}
void protected_dependency(){
    std::cout<<"external_stage_preserves_independent_fx_dependency\n";auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));
    auto source=std::make_unique<ProjectDocument>();std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(*source,edit));CHECK(source->sound.personalities&&!source->sound.personalities->instances.empty());if(!source->sound.personalities)return;
    const auto synth=source->sound.personalities->instances.front();CHECK(synth.design.role==RackRole::synth);const auto bank=synth.bindings.front().slot;
    RackDesign design;design.name="Independent drive";design.role=RackRole::fx;design.units={{0,NativeModule::drive,{}}};design.panel.sections={"Drive"};
    RackOperation fx;fx.design=design;fx.newInstance=juce::Uuid();fx.destination={RackRole::fx,bank,0};CHECK(p->applyRackOperation(fx,edit)==RackCandidateStatus::ready);
    auto before=std::make_unique<ProjectDocument>();CHECK(p->tryProjectSnapshot(*before,edit));CHECK(before->sound.personalities->instances.size()==2);const auto path=p->soundPresetPath();
    const auto caller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};auto result=prepare(*p,caller,object({{"reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>{synth.id.toDashedString()}},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}})}}),mcp::Status::invalidInput);
    CHECK(result["proposal_status"].isInt());CHECK(!result["edit_id"].isString());auto after=std::make_unique<ProjectDocument>();std::uint64_t now=0;CHECK(p->tryProjectSnapshot(*after,now));CHECK(*after==*before&&now==edit&&p->soundPresetPath()==path);
    // A failed dependent removal reserves no stage: a real ordinary edit can immediately prepare.
    auto stage=prepare(*p,caller,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})"));CHECK(stage["edit_id"].isString());call(*p,caller,mcp::Tool::discardEdit,object({{"edit_id",stage["edit_id"]}}));
}
void native_commit(){
    std::cout<<"native_commit_and_host_reentry\n";
    for(int mode=0;mode<4;++mode){auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));auto c=mcp::Caller{juce::Uuid(),p->mcpView().epoch};
        auto staged=prepare(*p,c,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})"));
        Listener listener;p->control(index("global.master_level")).addListener(&listener);
        bool fired=false;listener.begin=[&]{if(fired)return;fired=true;
            if(mode==1)p->control(index("output.gain_db")).setValue(.63f);
            if(mode==2){auto d=std::make_unique<SoundDocument>();std::uint64_t e;CHECK(p->trySoundSnapshot(*d,e));d->controlLocks[index("global.master_level")]=true;CHECK(p->applySoundEdit(*d,SoundEditOrigin::manual,e));}
            if(mode==3)CHECK(copperArcInstance(*p));
        };
        mcp::Command command;command.deadlineMs=GetTickCount64()+2000;command.caller=c;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",staged["edit_id"]}})};
        auto result=InstrumentMcpTestAccess::apply(*p,command);CHECK(result.status==(mode==0?mcp::Status::ready:mcp::Status::stale));CHECK(fired);CHECK(listener.starts==listener.ends);
        CHECK(listener.starts==1);if(mode==0){CHECK(result.changed);CHECK(p->control(index("global.master_level")).getValue()==.23f);CHECK(p->undoSoundEdit());}
        if(mode==1)CHECK(p->control(index("output.gain_db")).getValue()==.63f);
        p->control(index("global.master_level")).removeListener(&listener);
    }
}
void owned_drum_reentry(){
    std::cout<<"owned_drum_capture_survives_reentrant_restore\n";
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(blankInstance(*p));
    RackDesign design;design.name="MCP Drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
    RackOperation operation;operation.design=design;operation.newInstance=juce::Uuid();operation.allowSlotReuse=true;operation.allocation={{0,0}};
    CHECK(p->applyRackOperation(operation,revision(*p))==RackCandidateStatus::ready);
    const auto melodic=factoryRackDesigns()[0];
    for(std::uint8_t bank=1;bank<4;++bank){RackOperation placed;placed.design=melodic;placed.newInstance=juce::Uuid();placed.allowSlotReuse=true;placed.allocation={{0,bank}};placed.destination={RackRole::synth,masterChain,bank};CHECK(p->applyRackOperation(placed,revision(*p))==RackCandidateStatus::ready);}
    const std::array<float,4> samples{.2f,-.2f,.1f,0};std::uint64_t ticket=0;DrumCommitError error;
    CHECK(p->prepareDrumAsset(samples,1,ticket,error));drums::AssetPtr known;drums::AssetError assetError;CHECK(drums::makeSampleAsset(samples,1,known,assetError));
    drums::DrumPad metadata;metadata.asset=known->id();metadata.name="Captured";DrumPadTarget target;
    CHECK(p->captureDrumPadTarget(operation.newInstance,0,0,target,error));CHECK(p->assignPreparedDrumPad(target,ticket,metadata,{},error));
    const auto asset=InstrumentDrumTestAccess::asset(*p);CHECK(asset);
    auto firstCaller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};auto firstStage=prepare(*p,firstCaller,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.29}]})"));
    mcp::Command firstCommit;firstCommit.deadlineMs=GetTickCount64()+2000;firstCommit.caller=firstCaller;firstCommit.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",firstStage["edit_id"]}})};
    CHECK(InstrumentMcpTestAccess::apply(*p,firstCommit).status==mcp::Status::ready);CHECK(InstrumentDrumTestAccess::asset(*p)==asset);
    CHECK(p->captureDrumPadTarget(operation.newInstance,0,0,target,error));DrumPadReadback readback;CHECK(p->readDrumPad(target,readback,error));CHECK(readback.metadata==metadata&&readback.frames==4&&readback.channels==1);
    auto removal=object({{"reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>{operation.newInstance.toDashedString()}},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}})}});
    auto removalStage=prepare(*p,firstCaller,removal);const auto* pads=removalStage["diff"]["source_assignments"]["pads"].getArray();
    CHECK(pads&&pads->isEmpty());const auto* preserve=removalStage["diff"]["source_assignments"]["effects"].getArray();CHECK(preserve&&!preserve->isEmpty()&&(*preserve)[0]["kind"].toString()=="preserve");
    CHECK(InstrumentDrumTestAccess::asset(*p)==asset);call(*p,firstCaller,mcp::Tool::discardEdit,object({{"edit_id",removalStage["edit_id"]}}));p->pollMcp();
    RackInventoryPtr catalog;CHECK(testing::readyRackInventory(*p,catalog));if(!catalog)return;const auto rows=catalog->rows();const auto copper=std::find_if(rows.begin(),rows.end(),[](const auto& e){return e.metadata.name=="Copper Arc";});CHECK(copper!=rows.end());if(copper==rows.end())return;
    auto swap=removal.clone();swap.getDynamicObject()->setProperty("catalog_selections",juce::Array<juce::var>{object({{"local_id",1},{"design",copper->metadata.design.toDashedString()},{"revision",mcp::ipc::decimal(copper->metadata.revision)},{"content_sha256",copper->metadata.canonicalSha256},{"destination",object({{"role","synth"},{"chain",4},{"position",0}})}})});
    const auto replaceRevision=revision(*p);auto liveBefore=std::make_unique<ProjectDocument>();std::uint64_t liveEdit;CHECK(p->tryProjectSnapshot(*liveBefore,liveEdit));
    auto swapped=prepare(*p,firstCaller,swap,mcp::Status::ready,object({{"allow_slot_reuse",true}}));pads=swapped["diff"]["source_assignments"]["pads"].getArray();CHECK(pads&&pads->size()==16&&int((*pads)[0]["bank"])==0);
    const auto* clears=swapped["diff"]["source_assignments"]["effects"].getArray();CHECK(clears&&std::any_of(clears->begin(),clears->end(),[](const auto& effect){return effect["kind"].toString()=="clear"&&int(effect["bank"])==0;}));
    CHECK(swapped["diff"]["automation_warning"].toString().contains("Reuse bank 1"));
    CHECK(call(*p,firstCaller,mcp::Tool::getEdit,object({{"edit_id",swapped["edit_id"]}})).data["diff"]==swapped["diff"]);
    auto liveAfter=std::make_unique<ProjectDocument>();CHECK(p->tryProjectSnapshot(*liveAfter,liveEdit));CHECK(*liveBefore==*liveAfter&&revision(*p)==replaceRevision);
    if(pads&&!pads->isEmpty()){CHECK((*pads)[0]["before"]["asset"].toString()==juce::String(drums::assetIdText(known->id())));CHECK((*pads)[0]["after"]["asset"].isVoid());}
    CHECK(InstrumentDrumTestAccess::asset(*p)==asset);call(*p,firstCaller,mcp::Tool::discardEdit,object({{"edit_id",swapped["edit_id"]}}));p->pollMcp();
    const auto beforeReserved=InstrumentDrumTestAccess::reserved(*p),beforeRetained=InstrumentDrumTestAccess::retained(*p);
    auto caller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};auto stage=prepare(*p,caller,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})"));
    CHECK(InstrumentDrumTestAccess::reserved(*p)==beforeReserved+sizeof(drums::DrumOwnership::McpImage));
    CHECK(InstrumentDrumTestAccess::retained(*p)==beforeRetained+sizeof(drums::DrumOwnership::McpImage));
    {auto first=InstrumentDrumTestAccess::capture(*p),second=InstrumentDrumTestAccess::capture(*p);CHECK(first&&first==second&&first->owners[0].get()==asset);}
    Listener listener;bool fired=false;listener.begin=[&]{if(fired)return;fired=true;CHECK(blankInstance(*p));CHECK(InstrumentDrumTestAccess::rooted(*p,asset));CHECK(!InstrumentDrumTestAccess::capture(*p));};
    p->control(index("global.master_level")).addListener(&listener);
    mcp::Command command;command.deadlineMs=GetTickCount64()+2000;command.caller=caller;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",stage["edit_id"]}})};
    CHECK(InstrumentMcpTestAccess::apply(*p,command).status==mcp::Status::stale);CHECK(fired);CHECK(listener.starts==listener.ends);
    p->control(index("global.master_level")).removeListener(&listener);
    p->pollMcp();CHECK(!InstrumentDrumTestAccess::rooted(*p,asset));
}
void catalog_placement(){
    std::cout<<"captured_catalog_placement_and_reconciliation\n";
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));auto c=mcp::Caller{juce::Uuid(),p->mcpView().epoch};
    RackInventoryPtr expected;CHECK(testing::readyRackInventory(*p,expected));if(!expected)return;juce::Array<juce::var> all;juce::var cursor,snapshot;std::set<juce::String> seen; mcp::ToolResult page;
    do{auto args=object({{"kind","racks"},{"limit",65536}});if(!cursor.isVoid()){args.getDynamicObject()->setProperty("cursor",cursor);args.getDynamicObject()->setProperty("snapshot",snapshot);}
        page=call(*p,c,mcp::Tool::getCatalog,args);const auto* entries=page.data["entries"].getArray();CHECK(entries);if(!entries)return;
        for(const auto& row:*entries){CHECK(seen.insert(row["id"].toString()+":"+row["revision"].toString()).second);all.add(row);}cursor=page.data["cursor"];snapshot=page.data["snapshot"];
    }while(!cursor.isVoid());CHECK(all.size()==int(expected->rows().size()));auto* rows=&all;CHECK(!rows->isEmpty());if(rows->isEmpty())return;
    const auto found=std::find_if(rows->begin(),rows->end(),[](const auto& row){return row["role"].toString()=="synth"&&row["name"].toString()=="Copper Arc";});CHECK(found!=rows->end());if(found==rows->end())return;const auto row=*found;
    call(*p,c,mcp::Tool::getCatalog,object({{"snapshot",page.data["snapshot"]},{"kind","definition"},{"design_id",row["id"]},{"design_revision",row["revision"]},{"content_sha256",row["content_sha256"]}}));
    auto selection=object({{"local_id",1},{"design",row["id"]},{"revision",row["revision"]},{"content_sha256",row["content_sha256"]},{"destination",object({{"role","synth"},{"chain",4},{"position",1}})}});
    auto edits=object({{"catalog_selections",juce::Array<juce::var>{selection}}});auto stage=prepare(*p,c,edits);CHECK(stage["diff"]["instances"]["after"].getArray()&&stage["diff"]["instances"]["after"].getArray()->size()==2);const auto* effects=stage["diff"]["source_assignments"]["effects"].getArray();CHECK(effects&&!effects->isEmpty());
    mcp::Command command;command.deadlineMs=GetTickCount64()+2000;command.caller=c;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",stage["edit_id"]}})};
    CHECK(InstrumentMcpTestAccess::apply(*p,command).status==mcp::Status::ready);
    auto source=std::make_unique<ProjectDocument>();std::uint64_t e;CHECK(p->tryProjectSnapshot(*source,e));CHECK(source->sound.personalities->instances.size()==2);
    source->sound.macros[0]={parameterRegistry()[index("synth.2.analog.filter.cutoff")].stateKey,.3f};
    CHECK(p->applySoundEdit(source->sound,SoundEditOrigin::manual,e));CHECK(p->tryProjectSnapshot(*source,e));
    const auto id=source->sound.personalities->instances.back().id.toDashedString();
    auto remove=object({{"reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>{id}},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}})}});
    RackOperation native;native.kind=RackOperationKind::remove;native.selected=source->sound.personalities->instances.back().id;
    auto nativeCandidate=std::make_unique<SoundDocument>();RackDifference nativeDiff;
    CHECK(makeRackCandidate(source->sound,native,SoundEditOrigin::aiCandidate,*nativeCandidate,nativeDiff)==RackCandidateStatus::ready);
    CHECK(nativeDiff.automationWarning.contains("Global macro 1 target affected"));
    stage=prepare(*p,c,remove);CHECK(stage["diff"]["instances"]["after"].getArray()->size()==1);
    CHECK(stage["diff"]["automation_warning"].toString()==nativeDiff.automationWarning);
    CHECK(call(*p,c,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}})).data["diff"]==stage["diff"]);
    auto unchanged=std::make_unique<ProjectDocument>();std::uint64_t unchangedEdit;CHECK(p->tryProjectSnapshot(*unchanged,unchangedEdit));CHECK(*unchanged==*source&&unchangedEdit==e);
    command.call.arguments=object({{"edit_id",stage["edit_id"]}});command.deadlineMs=GetTickCount64()+2000;
    CHECK(InstrumentMcpTestAccess::apply(*p,command).status==mcp::Status::ready);
    CHECK(p->mcpView().lastEdit.contains(nativeDiff.automationWarning));
    selection.getDynamicObject()->setProperty("content_sha256",juce::String::repeatedString("0",64));prepare(*p,c,edits,mcp::Status::invalidInput);
}
void inventory_transient_failure_recovery(){
    std::cout<<"mcp_inventory_transient_failure_recovery\n";
    auto runtime=testing::isolatedRackRuntime();CHECK(runtime->root.createDirectory().wasOk());
    const auto index=runtime->root.getChildFile("installed.json");CHECK(!index.exists());
    auto p=std::make_unique<InstrumentProcessor>(runtime);p->pollMcp();const auto generation=runtime->store->observedGeneration();
    mcp::Command command;command.serial=1;command.deadlineMs=GetTickCount64()+30000;
    command.caller={juce::Uuid(),p->mcpView().epoch};command.call={mcp::Tool::getState,p->mcpView().instance,object({})};
    const auto initialDeadline=GetTickCount64()+10000;bool initiallyReady=false;
    while(GetTickCount64()<initialDeadline){if(p->executeMcp(command).status==mcp::Status::ready){initiallyReady=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    CHECK(initiallyReady);const auto initialView=InstrumentMcpTestAccess::inventory(*p);CHECK(initialView&&initialView->snapshot);
    if(!initiallyReady||!initialView||!initialView->snapshot)return;
    CHECK(index.replaceWithText("broken"));
    const auto failedDeadline=GetTickCount64()+10000;bool failed=false;
    while(GetTickCount64()<failedDeadline){const auto view=InstrumentMcpTestAccess::inventory(*p);
        if(view&&!view->busy&&view->status==RackLibraryStatus::unavailable){failed=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    CHECK(failed);CHECK(p->executeMcp(command).status==mcp::Status::unavailable);CHECK(index.deleteFile());CHECK(runtime->store->observedGeneration()==generation);
    const auto readyDeadline=GetTickCount64()+10000;bool ready=false;
    while(GetTickCount64()<readyDeadline){if(p->executeMcp(command).status==mcp::Status::ready){ready=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    CHECK(ready);CHECK(runtime->store->observedGeneration()==generation);CHECK(runtime->gets->load()==0);
}
void package_inventory_authority(){
    std::cout<<"mcp_package_inventory_authority_and_pinned_offline\n";
    const auto& designs=test::frozenCurrentRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.name=="Lumen Field";});
    CHECK(found!=designs.end());if(found==designs.end())return;const auto design=*found;const auto ref=legacyRackPackageRef(design);CHECK(ref&&ref->id=="lumen-field");if(!ref)return;
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(design,bytes,RackValidationMode::instanceRecall));const auto hash=juce::SHA256(bytes).toHexString();
    const auto selection=object({{"local_id",1},{"design",design.id.toDashedString()},{"revision",mcp::ipc::decimal(design.revision)},{"content_sha256",hash},{"destination",object({{"role","fx"},{"chain",4},{"position",0}})}});
    const auto edits=object({{"catalog_selections",juce::Array<juce::var>{selection}}});
    auto runtime=testing::isolatedRackRuntime();auto p=std::make_unique<InstrumentProcessor>(runtime);CHECK(copperArcInstance(*p));p->pollMcp();const auto caller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};
    auto state=call(*p,caller,mcp::Tool::getState);const auto oldSnapshot=state.data["snapshot"];
    auto absent=call(*p,caller,mcp::Tool::getCatalog,object({{"snapshot",oldSnapshot},{"kind","definition"},{"design_id",design.id.toDashedString()},{"design_revision",mcp::ipc::decimal(design.revision)},{"content_sha256",hash}}),mcp::Status::stale);(void)absent;
    prepare(*p,caller,edits,mcp::Status::invalidInput);CHECK(runtime->store->active().empty());CHECK(runtime->gets->load()==0);
    const auto workspace=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory();const auto archive=workspace.getChildFile("build/rack-library-native/releases/"+ref->id+"-"+ref->version+".airrack");
    CHECK(archive.existsAsFile());if(!archive.existsAsFile())return;
    const auto installed=std::async(std::launch::async,[&]{return runtime->store->install(archive,*ref,RackPackageActivation::makeActive,{});}).get();CHECK(installed==RackLibraryStatus::ready);if(installed!=RackLibraryStatus::ready)return;
    CHECK(testing::readyRackInventory(*p));call(*p,caller,mcp::Tool::getCatalog,object({{"snapshot",oldSnapshot},{"kind","racks"}}),mcp::Status::stale);
    state=call(*p,caller,mcp::Tool::getState);const auto installedSnapshot=state.data["snapshot"];std::set<juce::String> seen;juce::var cursor;unsigned pages=0;
    do{auto args=object({{"snapshot",installedSnapshot},{"kind","racks"},{"limit",8192}});if(!cursor.isVoid())args.getDynamicObject()->setProperty("cursor",cursor);
        auto page=call(*p,caller,mcp::Tool::getCatalog,args);const auto* rows=page.data["entries"].getArray();CHECK(rows);if(!rows)return;
        for(const auto& row:*rows)CHECK(seen.insert(row["id"].toString()+":"+row["revision"].toString()).second);
        cursor=page.data["cursor"];++pages;
    }while(!cursor.isVoid()&&pages<200);
    CHECK(cursor.isVoid());CHECK(pages>1);CHECK(seen.count(design.id.toString()+":"+mcp::ipc::decimal(design.revision))==1);
    call(*p,caller,mcp::Tool::getCatalog,object({{"snapshot",installedSnapshot},{"kind","definition"},{"design_id",design.id.toDashedString()},{"design_revision",mcp::ipc::decimal(design.revision)},{"content_sha256",hash}}));
    auto stage=prepare(*p,caller,edits);CHECK(stage["edit_id"].isString());if(!stage["edit_id"].isString())return;
    call(*p,{juce::Uuid(),caller.epoch},mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}}),mcp::Status::unauthorized);
    const auto candidate=InstrumentMcpTestAccess::candidate(*p);CHECK(candidate&&candidate->sound.personalities);
    if(candidate&&candidate->sound.personalities){const auto& racks=candidate->sound.personalities->instances;const auto stagedRack=std::find_if(racks.begin(),racks.end(),[&](const auto& rack){return rack.design==design;});CHECK(stagedRack!=racks.end());if(stagedRack!=racks.end())CHECK(stagedRack->package==ref);}
    juce::var previewJob;const auto liveBeforePreview=revision(*p);const auto started=call(*p,caller,mcp::Tool::renderPreview,object({{"edit_id",stage["edit_id"]}}));CHECK(started.data["job_id"].isString());
    if(started.data["job_id"].isString()){
        const auto job=object({{"job_id",started.data["job_id"]}});previewJob=job;const auto deadline=GetTickCount64()+60000;juce::var status;
        do{p->pollMcp();const auto view=call(*p,caller,mcp::Tool::getJob,job);status=view.data["job_status"];if(status.toString()!="running")break;juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}while(GetTickCount64()<deadline);
        CHECK(status.toString()=="ready");CHECK(revision(*p)==liveBeforePreview);
        CHECK(call(*p,caller,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}})).data["diff"]==stage["diff"]);
    }
    const auto removed=std::async(std::launch::async,[&]{return runtime->store->remove(*ref,{});}).get();CHECK(removed==RackLibraryStatus::ready);if(removed!=RackLibraryStatus::ready)return;
    if(!previewJob.isVoid()){p->pollMcp();call(*p,caller,mcp::Tool::getJob,previewJob,mcp::Status::stale);}
    call(*p,caller,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}}),mcp::Status::stale);CHECK(runtime->gets->load()==0);
    CHECK(std::async(std::launch::async,[&]{return runtime->store->install(archive,*ref,RackPackageActivation::makeActive,{});}).get()==RackLibraryStatus::ready);
    CHECK(testing::readyRackInventory(*p));stage=prepare(*p,caller,edits);CHECK(stage["edit_id"].isString());if(!stage["edit_id"].isString())return;
    const auto before=revision(*p);mcp::Command command;command.caller=caller;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",stage["edit_id"]}})};
    auto outcome=ProposalCommitResult{};const auto end=GetTickCount64()+60000;unsigned busy=0;
    do{command.deadlineMs=GetTickCount64()+2000;outcome=InstrumentMcpTestAccess::apply(*p,command);if(outcome.status!=mcp::Status::busy)break;++busy;CHECK(revision(*p)==before);std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(GetTickCount64()<end);
    std::cout<<"mcp package preparation busy replies="<<busy<<std::endl;CHECK(outcome.status==mcp::Status::ready);
    auto committed=std::make_unique<ProjectDocument>();std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(*committed,edit));CHECK(committed->sound.personalities);
    if(committed->sound.personalities){const auto& racks=committed->sound.personalities->instances;const auto pinned=std::find_if(racks.begin(),racks.end(),[&](const auto& rack){return rack.design==design;});CHECK(pinned!=racks.end());if(pinned!=racks.end())CHECK(pinned->package==ref);}
    CHECK(runtime->gets->load()==0);
    // A project can retain the exact recipe and ref without an installed package.
    auto offline=testing::isolatedRackRuntime();auto project=std::make_unique<InstrumentProcessor>(offline);CHECK(copperArcInstance(*project));RackOperation op;op.design=design;op.package=ref;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};
    CHECK(project->applyRackOperation(op,revision(*project))==RackCandidateStatus::ready);CHECK(offline->store->active().empty());
    project->pollMcp();const auto projectCaller=mcp::Caller{juce::Uuid(),project->mcpView().epoch};auto projectState=call(*project,projectCaller,mcp::Tool::getState);
    call(*project,projectCaller,mcp::Tool::getCatalog,object({{"snapshot",projectState.data["snapshot"]},{"kind","definition"},{"design_id",design.id.toDashedString()},{"design_revision",mcp::ipc::decimal(design.revision)},{"content_sha256",hash}}));
    auto second=selection.clone();second["destination"].getDynamicObject()->setProperty("position",1);auto embeddedEdits=object({{"catalog_selections",juce::Array<juce::var>{second}}});auto embeddedStage=prepare(*project,projectCaller,embeddedEdits);
    CHECK(embeddedStage["edit_id"].isString());if(embeddedStage["edit_id"].isString()){
        mcp::Command c;c.deadlineMs=GetTickCount64()+2000;c.caller=projectCaller;c.call={mcp::Tool::applyEdit,project->mcpView().instance,object({{"edit_id",embeddedStage["edit_id"]}})};
        CHECK(InstrumentMcpTestAccess::apply(*project,c).status==mcp::Status::ready);
        auto after=std::make_unique<ProjectDocument>();CHECK(project->tryProjectSnapshot(*after,edit));if(after->sound.personalities){unsigned pinned=0;for(const auto& rack:after->sound.personalities->instances)if(rack.design==design&&rack.package==ref)++pinned;CHECK(pinned==2);}else CHECK(false);
    }
    CHECK(offline->store->active().empty());CHECK(offline->gets->load()==0);
}
// Own 56 small, distinct packages so the public MCP paging path crosses the old 72-entry limit.
// Recipes are derived from the retained native Copper Arc design; UUIDs and names are test-only.
void inventory_scale(){
    std::cout<<"mcp_inventory_more_than_72_installed_designs\n";
    auto runtime=testing::isolatedRackRuntime();auto p=std::make_unique<InstrumentProcessor>(runtime);CHECK(copperArcInstance(*p));
    const auto directory=runtime->root.getChildFile("owned-test-archives");CHECK(directory.createDirectory().wasOk());
    constexpr unsigned char pngBytes[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,96,96,96,96,0,0,0,5,0,1,165,246,69,64,0,0,0,0,73,69,78,68,174,66,96,130};
    const juce::MemoryBlock png(pngBytes,sizeof(pngBytes));std::set<juce::String> expectedIds;
    const auto built=factoryRackDesigns();CHECK(!built.empty());if(built.empty())return;
    for(int n=0;n<56;++n){
        auto design=built.front();design.id=juce::Uuid();design.name="MCP scale fixture "+juce::String(n);juce::MemoryBlock recipe;
        CHECK(encodeRackDesign(design,recipe,RackValidationMode::instanceRecall));if(recipe.isEmpty())return;
        RackPackageManifest manifest;manifest.ref={"mcp-scale-"+juce::String(n),"1.0.0",{}};
        manifest.metadata={design.id,design.revision,juce::SHA256(recipe).toHexString(),design.name,"Test fixture","Inventory scale","Isolated MCP paging test",design.role,{"analog"}};
        manifest.designPath="rack.json";manifest.screenshotPath="screenshot.png";
        manifest.files={{"rack.json",juce::SHA256(recipe).toHexString(),"design",recipe.getSize()},{"screenshot.png",juce::SHA256(png).toHexString(),"png",png.getSize()}};
        juce::Array<juce::var> modules;std::set<std::pair<int,int>> distinct;
        for(const auto& unit:design.units)if(distinct.emplace(int(unit.module),int(unit.algorithmRevision)).second)modules.add(object({{"module",int(unit.module)},{"algorithmRevision",int(unit.algorithmRevision)}}));
        manifest.capabilities=object({{"airMin","0.1.0"},{"modules",modules},{"nativeEditor",""}});juce::String text;CHECK(encodeRackPackageManifest(manifest,text));if(text.isEmpty())return;
        const auto file=directory.getChildFile(manifest.ref.id+".airrack");juce::ZipFile::Builder zip;const juce::Time stamp(1980,0,1,0,0);
        zip.addEntry(new juce::MemoryInputStream(text.toRawUTF8(),text.getNumBytesAsUTF8(),true),0,"package.json",stamp);
        zip.addEntry(new juce::MemoryInputStream(recipe,true),0,"rack.json",stamp);zip.addEntry(new juce::MemoryInputStream(png,true),0,"screenshot.png",stamp);
        {auto output=file.createOutputStream();CHECK(output&&zip.writeToStream(*output,nullptr));if(!output)return;}
        {juce::FileInputStream input(file);CHECK(input.openedOk());if(!input.openedOk())return;manifest.ref.sha256=juce::SHA256(input).toHexString();}
        const auto status=std::async(std::launch::async,[&]{return runtime->store->install(file,manifest.ref,RackPackageActivation::makeActive,{});}).get();
        CHECK(status==RackLibraryStatus::ready);if(status!=RackLibraryStatus::ready)return;expectedIds.insert(design.id.toString()+":"+mcp::ipc::decimal(design.revision));
    }
    RackInventoryPtr inventory;CHECK(testing::readyRackInventory(*p,inventory));if(!inventory)return;CHECK(inventory->rows().size()>72);
    p->pollMcp();const auto caller=mcp::Caller{juce::Uuid(),p->mcpView().epoch};const auto state=call(*p,caller,mcp::Tool::getState);const auto snapshot=state.data["snapshot"];
    std::set<juce::String> found;juce::var cursor,lateRow;unsigned pages=0;std::size_t rowNumber=0;
    do{auto args=object({{"snapshot",snapshot},{"kind","racks"},{"limit",16384}});if(!cursor.isVoid())args.getDynamicObject()->setProperty("cursor",cursor);
        const auto page=call(*p,caller,mcp::Tool::getCatalog,args);const auto* rows=page.data["entries"].getArray();CHECK(rows);if(!rows)return;
        for(const auto& row:*rows){CHECK(found.insert(row["id"].toString()+":"+row["revision"].toString()).second);if(rowNumber++==72)lateRow=row;}
        cursor=page.data["cursor"];++pages;
    }while(!cursor.isVoid()&&pages<200);
    CHECK(cursor.isVoid());CHECK(pages>1);CHECK(found.size()==inventory->rows().size());CHECK(found.size()>72);
    for(const auto& id:expectedIds)CHECK(found.count(id)==1);CHECK(runtime->gets->load()==0);
    CHECK(lateRow.getDynamicObject());if(!lateRow.getDynamicObject())return;
    const auto& target=inventory->rows()[72];CHECK(target.installed&&target.package.has_value());if(!target.package)return;
    CHECK(juce::Uuid(lateRow["id"].toString())==target.metadata.design);CHECK(lateRow["revision"].toString()==mcp::ipc::decimal(target.metadata.revision));CHECK(lateRow["content_sha256"].toString()==target.metadata.canonicalSha256);
    const auto definition=call(*p,caller,mcp::Tool::getCatalog,object({{"snapshot",snapshot},{"kind","definition"},{"design_id",lateRow["id"]},{"design_revision",lateRow["revision"]},{"content_sha256",lateRow["content_sha256"]}}));CHECK(definition.data["definition"].getDynamicObject());
    const auto selection=object({{"local_id",1},{"design",lateRow["id"]},{"revision",lateRow["revision"]},{"content_sha256",lateRow["content_sha256"]},{"destination",object({{"role","synth"},{"chain",4},{"position",1}})}});
    const auto edits=object({{"catalog_selections",juce::Array<juce::var>{selection}}});const auto stage=prepare(*p,caller,edits);CHECK(stage["edit_id"].isString());if(!stage["edit_id"].isString())return;
    const auto candidate=InstrumentMcpTestAccess::candidate(*p);CHECK(candidate&&candidate->sound.personalities);
    if(candidate&&candidate->sound.personalities){const auto& racks=candidate->sound.personalities->instances;const auto selected=std::find_if(racks.begin(),racks.end(),[&](const auto& rack){return rack.design.id==target.metadata.design&&rack.design.revision==target.metadata.revision;});CHECK(selected!=racks.end());if(selected!=racks.end())CHECK(selected->package==target.package);}
    const auto before=revision(*p);auto beforeProject=std::make_unique<ProjectDocument>();std::uint64_t beforeEdit=0;CHECK(p->tryProjectSnapshot(*beforeProject,beforeEdit));CHECK(beforeEdit==before&&beforeProject->sound.personalities);
    mcp::Command command;command.caller=caller;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",stage["edit_id"]}})};
    ProposalCommitResult outcome;const auto end=GetTickCount64()+60000;unsigned busy=0;
    do{command.deadlineMs=GetTickCount64()+2000;outcome=InstrumentMcpTestAccess::apply(*p,command);if(outcome.status!=mcp::Status::busy)break;++busy;CHECK(revision(*p)==before);std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(GetTickCount64()<end);
    CHECK(outcome.status==mcp::Status::ready);std::cout<<"mcp scale placement busy replies="<<busy<<std::endl;
    auto after=std::make_unique<ProjectDocument>();std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(*after,edit));CHECK(after->sound.personalities);
    CHECK(edit==before+1);
    if(after->sound.personalities&&beforeProject->sound.personalities){
        const auto& racks=after->sound.personalities->instances;const auto& original=beforeProject->sound.personalities->instances;CHECK(racks.size()==original.size()+1);
        for(const auto& prior:original)CHECK(std::find(racks.begin(),racks.end(),prior)!=racks.end());
        const auto selected=std::find_if(racks.begin(),racks.end(),[&](const auto& rack){return rack.design.id==target.metadata.design&&rack.design.revision==target.metadata.revision;});
        CHECK(selected!=racks.end());if(selected!=racks.end()){CHECK(selected->package==target.package);juce::MemoryBlock canonical;CHECK(encodeRackDesign(selected->design,canonical,RackValidationMode::instanceRecall));CHECK(juce::SHA256(canonical).toHexString()==target.metadata.canonicalSha256);}
    }
    CHECK(runtime->gets->load()==0);
}
void stage_lifecycle(){
    std::cout<<"staged_expiry_deadline_epoch_and_native_busy\n";auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));auto c=mcp::Caller{juce::Uuid(),p->mcpView().epoch};
    auto stage=prepare(*p,c,json("{}"));InstrumentMcpTestAccess::expire(*p);call(*p,c,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}}),mcp::Status::stale);
    stage=prepare(*p,c,json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})"));
    mcp::Command command;command.deadlineMs=GetTickCount64();command.caller=c;command.call={mcp::Tool::applyEdit,p->mcpView().instance,object({{"edit_id",stage["edit_id"]}})};
    CHECK(InstrumentMcpTestAccess::apply(*p,command).status==mcp::Status::hostControlUnavailable);call(*p,c,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}}));
    command.deadlineMs=GetTickCount64()+2000;command.caller.owner=juce::Uuid();CHECK(InstrumentMcpTestAccess::apply(*p,command).status==mcp::Status::unauthorized);
    call(*p,c,mcp::Tool::discardEdit,object({{"edit_id",stage["edit_id"]}}));
    auto request=testing::fixtureValue([&]{return p->beginAiRequest({"Native pending","model",{}});});CHECK(request->has_value());
    prepare(*p,c,json("{}"),mcp::Status::busy);p->discardAiProposal();
    stage=prepare(*p,c,json("{}"));p->control(index("output.gain_db")).setValue(.37f);
    call(*p,c,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}}),mcp::Status::stale);
    stage=prepare(*p,c,json("{}"));CHECK(copperArcInstance(*p));
    call(*p,c,mcp::Tool::getEdit,object({{"edit_id",stage["edit_id"]}}),mcp::Status::stale);
}
void maximum_catalog(){
    std::cout<<"native_maximum_72_catalog_pages_and_limits\n";
    auto designs=libraryRackDesigns();while(designs.size()<maximumRackCatalogEntries){auto copy=designs.front();copy.id=juce::Uuid();designs.push_back(std::move(copy));}
    RackCatalogSnapshot snapshot;RackCatalogStatus status;CHECK(captureRackCatalog(designs,{}, {},snapshot,&status));CHECK(snapshot.entries.size()==72);
    std::size_t next=0,total=0;do{RackCatalogPage page;CHECK(catalogPage(snapshot,{},next,65536,page,&status));CHECK(page.next>next||page.complete);total+=page.entries.size();next=page.next;if(page.complete)break;}while(next<72);CHECK(total==72);
    RackCatalogPage page;CHECK(!catalogPage(snapshot,{},73,65536,page,&status));
    RackCatalogCaptureLimits limits;limits.maximumDefinitionBytes=1;CHECK(!captureRackCatalog(designs,{},limits,snapshot,&status));CHECK(status==RackCatalogStatus::byteLimit);
    limits={};limits.maximumPreparedBytes=1;CHECK(!captureRackCatalog(designs,{},limits,snapshot,&status));CHECK(status==RackCatalogStatus::preparedLimit);
}
void pagination(){
    std::cout<<"bounded_catalog_and_cursor_binding\n";auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));auto c=mcp::Caller{juce::Uuid(),p->mcpView().epoch};
    auto state=call(*p,c,mcp::Tool::getState);auto snapshot=state.data["snapshot"];CHECK(snapshot.isString());
    CHECK(!juce::JSON::toString(state.data).contains("credential"));
    std::set<juce::String> ids;juce::var cursor;unsigned pages=0;
    do{auto args=object({{"snapshot",snapshot},{"kind","parameters"},{"limit",256}});if(!cursor.isVoid())args.getDynamicObject()->setProperty("cursor",cursor);
        auto page=call(*p,c,mcp::Tool::getCatalog,args);auto* entries=page.data["entries"].getArray();CHECK(entries);if(!entries)break;
        CHECK(entries->size()<=256);for(const auto& e:*entries)CHECK(ids.insert(e["id"].toString()).second);
        cursor=page.data["cursor"];snapshot=page.data["snapshot"];++pages;
    }while(!cursor.isVoid()&&pages<200);
    CHECK(ids.size()==parameterCount);CHECK(pages>1);
    call(*p,c,mcp::Tool::getCatalog,object({{"snapshot",snapshot},{"kind","parameters"},{"cursor","forged"}}),mcp::Status::stale);
    auto page=call(*p,c,mcp::Tool::getCatalog,object({{"snapshot",snapshot},{"kind","parameters"},{"limit",1}}));cursor=page.data["cursor"];
    CHECK(p->setAiMusicalContext({"bass","C","minor",120}));
    call(*p,c,mcp::Tool::getCatalog,object({{"snapshot",snapshot},{"kind","parameters"},{"cursor",cursor}}),mcp::Status::stale);
}
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;if(argc>1&&std::string_view(argv[1])=="--inventory-recovery"){inventory_transient_failure_recovery();}else if(argc>1&&std::string_view(argv[1])=="--inventory-scale"){inventory_scale();}else if(argc>1&&std::string_view(argv[1])=="--names"){named_stage();}else if(argc>1&&std::string_view(argv[1])=="--dependency"){protected_dependency();}else if(argc>1&&std::string_view(argv[1])=="--owned-drum"){owned_drum_reentry();}else{staging();admission();pagination();named_stage();protected_dependency();native_commit();catalog_placement();inventory_transient_failure_recovery();package_inventory_authority();owned_drum_reentry();maximum_catalog();stage_lifecycle();}std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
