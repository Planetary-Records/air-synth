#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "LocalPreview.h"
#include "DrumImport.h"
#include "OwnedFixture.h"
#include "IsolatedRackLibrary.h"
#include <bit>
#include <cmath>
#include <crtdbg.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <Windows.h>
#include <winioctl.h>
#include <iostream>
namespace airack {struct InstrumentDrumTestAccess {
    static void holdRestore(InstrumentProcessor& p,std::atomic_bool& entered,const std::atomic_bool& release){const std::lock_guard lock(p.restoreMutex);entered=true;while(!release.load())std::this_thread::yield();}
    static bool externalPlayerPlaying(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.previewOwner==InstrumentProcessor::PreviewOwner::external&&p.renderedPreview.playing();}
    static auto capture(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->captureMcpImage();}
    static std::uint64_t reserved(const InstrumentProcessor& p){return p.drumOwnership->reservedDecoded_;}
    static std::uint64_t externalPlayerBytes(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.mcpPreviewBytes;}
    static std::uint64_t pcm(const InstrumentProcessor& p){return p.drumOwnership->retainedBytes()-p.drumOwnership->reservedDecoded_;}
};}
using namespace airack;
namespace {
int checks=0,failures=0;thread_local bool callbackGuard=false;thread_local unsigned callbackAllocations=0,callbackFrees=0;
int __cdecl allocationHook(int kind,void*,size_t,int,long,const unsigned char*,int){if(callbackGuard){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++callbackAllocations;if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++callbackFrees;}return 1;}
void check(bool b,const char* s,int l){++checks;if(!b){++failures;std::cerr<<l<<" "<<s<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
auto object(std::initializer_list<std::pair<const char*,juce::var>> v){return mcp::ipc::object(v);}
mcp::ToolResult rawCall(InstrumentProcessor& p,mcp::Caller c,mcp::Tool t,juce::var a){mcp::Command command;command.serial=17;command.deadlineMs=GetTickCount64()+2000;command.caller=c;command.call={t,p.mcpView().instance,a};return p.executeMcp(command);}
mcp::ToolResult call(InstrumentProcessor& p,mcp::Caller c,mcp::Tool t,juce::var a){
    if(t==mcp::Tool::getState||t==mcp::Tool::prepareEdit)CHECK(testing::readyRackInventory(p));
    return rawCall(p,c,t,a);
}
void coldInventoryReadiness(){
    std::cout<<"mcp_preview_cold_inventory_readiness\n";auto runtime=testing::isolatedRackRuntime();auto p=std::make_unique<InstrumentProcessor>(runtime);
    CHECK(copperArcInstance(*p));p->pollMcp();const mcp::Caller caller{juce::Uuid(),p->mcpView().epoch};
    const auto first=rawCall(*p,caller,mcp::Tool::getState,object({}));
    CHECK(first.status==mcp::Status::unavailable||first.status==mcp::Status::ready);
    std::cout<<"cold inventory initial status="<<int(first.status)<<std::endl;
    CHECK(testing::readyRackInventory(*p));const auto ready=rawCall(*p,caller,mcp::Tool::getState,object({}));
    CHECK(ready.status==mcp::Status::ready&&ready.data["snapshot"].isString());CHECK(runtime->gets->load()==0);
}
void callback(InstrumentProcessor& p,juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi){callbackGuard=true;p.processBlock(audio,midi);callbackGuard=false;}
void renderBoundary(bool staged,bool drum=false){
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());
    if(!drum)CHECK(copperArcInstance(*p));
    else{
        CHECK(blankInstance(*p));auto source=std::make_unique<ProjectDocument>();std::uint64_t revision=0;CHECK(p->tryProjectSnapshot(*source,revision));
        RackDesign design;design.name="MCP Drum fixture";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
        RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,0}};CHECK(p->applyRackOperation(op,revision)==RackCandidateStatus::ready);
        std::vector<float> samples(2400);for(std::size_t n=0;n<samples.size();++n)samples[n]=.1f*std::sin(float(n)*.17f)*float(samples.size()-n)/samples.size();
        std::uint64_t ticket=0;DrumCommitError error;CHECK(p->prepareDrumAsset(samples,1,ticket,error));drums::AssetPtr known;drums::AssetError assetError;CHECK(drums::makeSampleAsset(samples,1,known,assetError));
        drums::DrumPad metadata;metadata.asset=known->id();metadata.name="Owned preview";DrumPadTarget target;CHECK(p->captureDrumPadTarget(op.newInstance,0,0,target,error));CHECK(p->assignPreparedDrumPad(target,ticket,metadata,{},error));
        CHECK(p->tryProjectSnapshot(*source,revision));auto phrase=source->pattern;phrase.noteCount=1;const auto note=std::uint8_t(std::lround(parameterPhysicalValue(source->sound.values,drumParameterStart+7)));phrase.notes[0]={0,.25,note,96,1};CHECK(p->applyPatternEdit(phrase,revision));
    }
    p->pollMcp();mcp::Caller c{juce::Uuid(),p->mcpView().epoch};
    auto state=call(*p,c,mcp::Tool::getState,object({}));CHECK(state.status==mcp::Status::ready);CHECK(state.data["snapshot"].isString());if(state.status!=mcp::Status::ready||!state.data["snapshot"].isString())return;
    auto before=std::make_unique<ProjectDocument>();std::uint64_t edit=0,reset=0;CHECK(p->tryProjectSnapshot(*before,edit,reset));
    auto image=InstrumentDrumTestAccess::capture(*p);CHECK(image);BorrowedDrumAssignments borrowed;if(image)for(std::size_t n=0;n<64;++n)borrowed[n/16][n%16]=image->owners[n].get();
    const auto baseline=InstrumentDrumTestAccess::reserved(*p),pcm=InstrumentDrumTestAccess::pcm(*p);
    auto reference=std::make_unique<ProjectDocument>(*before);
    auto args=object({{"snapshot",state.data["snapshot"]}});
    if(staged){auto prepared=call(*p,c,mcp::Tool::prepareEdit,object({{"snapshot",state.data["snapshot"]},{"policy",object({})},{"edits",juce::JSON::parse(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})")}}));CHECK(prepared.status==mcp::Status::ready);CHECK(prepared.data["edit_id"].isString());if(prepared.status!=mcp::Status::ready)return;auto viewed=call(*p,c,mcp::Tool::getEdit,object({{"edit_id",prepared.data["edit_id"]}}));CHECK(viewed.status==mcp::Status::ready);args=object({{"edit_id",prepared.data["edit_id"]}});const auto registry=parameterRegistry();for(std::size_t n=0;n<registry.size();++n)if(registry[n].id=="global.master_level")reference->sound.values[n]=.23f;}
    auto rendered=call(*p,c,mcp::Tool::renderPreview,args);std::cout<<"render boundary staged="<<staged<<" status="<<int(rendered.status)<<'\n';CHECK(rendered.status==mcp::Status::ready);CHECK(rendered.data["job_id"].isString());
    if(rendered.status!=mcp::Status::ready||!rendered.data["job_id"].isString())return;
    p->closeAiPanel();auto builtIn=testing::fixtureValue([&]{return p->beginAiRequest({"Preview owns this instance","model",{}});});CHECK(!builtIn->has_value());CHECK(InstrumentDrumTestAccess::pcm(*p)==pcm);
    const auto jobArgs=object({{"job_id",rendered.data["job_id"]}});mcp::ToolResult complete;
    const auto deadline=GetTickCount64()+60000;
    do{juce::MessageManager::getInstance()->runDispatchLoopUntil(1);p->pollMcp();complete=call(*p,c,mcp::Tool::getJob,jobArgs);if(complete.status!=mcp::Status::ready)break;}while(complete.data["job_status"].toString()=="running"&&GetTickCount64()<deadline);
    std::cout<<"job lookup status="<<int(complete.status)<<" epoch-before="<<c.epoch.toDashedString()<<" epoch-after="<<p->mcpView().epoch.toDashedString()<<" connected="<<p->mcpView().connected<<"\n";std::cout<<"actual job="<<juce::JSON::toString(complete.data,true)<<'\n';CHECK(complete.status==mcp::Status::ready);CHECK(complete.data["job_status"].toString()=="ready");CHECK(complete.data["resource_uri"].isString());if(staged)CHECK(InstrumentDrumTestAccess::reserved(*p)>=baseline+RackCatalogCaptureLimits{}.maximumPreparedBytes);if(complete.data["job_status"].toString()!="ready")return;
    const auto uri=complete.data["resource_uri"].toString();const auto parts=juce::StringArray::fromTokens(uri.substring(14),"/","");
    const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});CHECK(parts.size()==3&&juce::File::isAbsolutePath(root));
    if(parts.size()==3){const auto file=juce::File(root).getChildFile("MCP Previews").getChildFile(parts[1]).getChildFile(parts[0]).getChildFile(parts[2]+".wav");
        HANDLE h=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);CHECK(h!=INVALID_HANDLE_VALUE);
        HANDLE replaced=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);CHECK(replaced==INVALID_HANDLE_VALUE);if(replaced!=INVALID_HANDLE_VALUE)CloseHandle(replaced);
        if(h!=INVALID_HANDLE_VALUE){LARGE_INTEGER size{};CHECK(GetFileSizeEx(h,&size));CHECK(size.QuadPart==44+juce::int64(complete.data["frames"])*8);juce::MemoryBlock bytes(std::size_t(size.QuadPart));DWORD count=0;CHECK(ReadFile(h,bytes.getData(),DWORD(bytes.getSize()),&count,nullptr)&&count==bytes.getSize());CloseHandle(h);
            CHECK(std::memcmp(bytes.getData(),"RIFF",4)==0);const auto capture=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK5_RED_ARTIFACT_ROOT",{});if(capture.isNotEmpty()){CHECK(juce::File(capture).getChildFile(staged?"staged.wav":"committed.wav").replaceWithData(bytes.getData(),bytes.getSize()));}}
    }
    auto request=std::make_unique<ai::GenerationRequest>();request->source=*before;auto context=ai::previewContextFor(*request);
    auto native=testing::fixtureValue([&]{return ai::renderPreview(*reference,context,borrowed);});CHECK(native->status==ai::RenderStatus::ready);CHECK(int(native->audio.left.size())==int(complete.data["frames"]));
    auto alternate=testing::fixtureValue([&]{return ai::renderPreview(*reference,context,borrowed,{},128);});CHECK(alternate->status==ai::RenderStatus::ready);CHECK(alternate->audio.left==native->audio.left&&alternate->audio.right==native->audio.right);
    CHECK(std::abs(native->measurements.peak-double(complete.data["measurements"]["peak"]))<1e-12);CHECK(std::abs(native->measurements.rms-double(complete.data["measurements"]["rms"]))<1e-12);
    CHECK(bool(complete.data["measurements"]["finite"])==native->measurements.finite);CHECK(complete.data["measurements"]["clipped_samples"].toString()==mcp::ipc::decimal(native->measurements.clippedSamples));CHECK(int(complete.data["body_frames"])==native->audio.bodyFrames);CHECK(bool(complete.data["phrase_truncated"])==native->audio.phraseTruncated);
    CHECK(double(complete.data["measurements"]["activity_end_seconds"])==native->measurements.activityEndSeconds);CHECK(double(complete.data["measurements"]["decay_seconds"])==native->measurements.decaySeconds);CHECK(bool(complete.data["measurements"]["tail_active_at_end"])==native->measurements.tailActiveAtEnd);CHECK(native->measurements.onsetSeconds?double(complete.data["measurements"]["onset_seconds"])==*native->measurements.onsetSeconds:complete.data["measurements"]["onset_seconds"].isVoid());
    for(int band=0;band<3;++band)CHECK(double(complete.data["measurements"]["band_fractions"][band])==native->measurements.bandFractions[band]);
    auto read=call(*p,c,mcp::Tool::resourceRead,object({{"uri",complete.data["resource_uri"]}}));std::cout<<"actual resource read status="<<int(read.status)<<'\n';CHECK(read.status==mcp::Status::ready);
    if(read.status==mcp::Status::ready){const auto contents=read.data["contents"];CHECK(contents.isArray()&&contents.size()==1);CHECK(contents[0]["mimeType"].toString()=="audio/wav");juce::MemoryOutputStream decoded;CHECK(juce::Base64::convertFromBase64(decoded,contents[0]["blob"].toString()));CHECK(decoded.getDataSize()==44+native->audio.left.size()*8);
        bool exact=true;const auto* bytes=static_cast<const std::byte*>(decoded.getData());for(std::size_t n=0;n<native->audio.left.size();++n){float left=0,right=0;std::memcpy(&left,bytes+44+n*8,4);std::memcpy(&right,bytes+48+n*8,4);exact&=std::bit_cast<std::uint32_t>(left)==std::bit_cast<std::uint32_t>(native->audio.left[n])&&std::bit_cast<std::uint32_t>(right)==std::bit_cast<std::uint32_t>(native->audio.right[n]);}CHECK(exact);}
    auto foreign=mcp::Caller{juce::Uuid(),c.epoch};CHECK(call(*p,foreign,mcp::Tool::resourceList,object({})).data["resources"].size()==0);CHECK(call(*p,foreign,mcp::Tool::resourceRead,object({{"uri",uri}})).status==mcp::Status::unauthorized);
    CHECK(call(*p,foreign,mcp::Tool::playPreview,jobArgs).status==mcp::Status::unauthorized);
    CHECK(call(*p,c,mcp::Tool::resourceRead,object({{"uri",uri+"/../file"}})).status==mcp::Status::invalidInput);
    CHECK(call(*p,c,mcp::Tool::resourceRead,object({{"uri","C:/Windows/win.ini"}})).status==mcp::Status::invalidInput);
    auto play=call(*p,c,mcp::Tool::playPreview,jobArgs);std::cout<<"actual suspended play status="<<int(play.status)<<'\n';CHECK(play.status==mcp::Status::audioUnavailable);
    p->prepareToPlay(48000,128);play=call(*p,c,mcp::Tool::playPreview,jobArgs);std::cout<<"actual prepared-without-callback play status="<<int(play.status)<<'\n';CHECK(play.status==mcp::Status::audioUnavailable);
    juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
    if(!staged&&!drum){juce::AudioBuffer<float> large(2,65536);callback(*p,large,midi);const auto grace=GetTickCount64()+350;while(GetTickCount64()<grace)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);CHECK(call(*p,c,mcp::Tool::playPreview,jobArgs).status==mcp::Status::ready);CHECK(call(*p,c,mcp::Tool::stopPreview,jobArgs).status==mcp::Status::ready);for(int n=0;n<5;++n)callback(*p,audio,midi);p->pollMcp();}
    audio.clear();callback(*p,audio,midi);
    CHECK(call(*p,c,mcp::Tool::playPreview,jobArgs).status==mcp::Status::ready);
    for(int n=0;n<5;++n)callback(*p,audio,midi);CHECK(audio.getMagnitude(0,128)>0);
    if(staged)CHECK(call(*p,c,mcp::Tool::applyEdit,args).status==mcp::Status::busy);
    CHECK(call(*p,c,mcp::Tool::stopPreview,jobArgs).status==mcp::Status::ready);for(int n=0;n<5;++n)callback(*p,audio,midi);p->pollMcp();
    p->suspendProcessing(true);CHECK(call(*p,c,mcp::Tool::playPreview,jobArgs).status==mcp::Status::audioUnavailable);p->suspendProcessing(false);
    const auto expiry=GetTickCount64()+300;while(GetTickCount64()<expiry)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    CHECK(call(*p,c,mcp::Tool::playPreview,jobArgs).status==mcp::Status::audioUnavailable);
    auto after=std::make_unique<ProjectDocument>();std::uint64_t observed=0,observedReset=0;CHECK(p->tryProjectSnapshot(*after,observed,observedReset));CHECK(*after==*before&&observed==edit&&observedReset==reset);
    CHECK(call(*p,c,mcp::Tool::cancelJob,jobArgs).status==mcp::Status::ready);CHECK(read.resourceRevoked&&read.resourceRevoked->load());read={};p->pollMcp();CHECK(call(*p,c,mcp::Tool::getJob,jobArgs).status==mcp::Status::stale);
    if(staged)CHECK(call(*p,c,mcp::Tool::getEdit,args).status==mcp::Status::ready);
    CHECK(InstrumentDrumTestAccess::reserved(*p)==baseline);CHECK(InstrumentDrumTestAccess::pcm(*p)==pcm);
}
juce::var startJob(InstrumentProcessor& p,mcp::Caller caller){const auto state=call(p,caller,mcp::Tool::getState,object({}));CHECK(state.status==mcp::Status::ready);const auto started=call(p,caller,mcp::Tool::renderPreview,object({{"snapshot",state.data["snapshot"]}}));CHECK(started.status==mcp::Status::ready);return object({{"job_id",started.data["job_id"]}});}
mcp::ToolResult waitJob(InstrumentProcessor& p,mcp::Caller caller,const juce::var& job){mcp::ToolResult result;const auto end=GetTickCount64()+60000;do{p.pollMcp();result=call(p,caller,mcp::Tool::getJob,job);if(result.status!=mcp::Status::ready||result.data["job_status"].toString()!="running")break;juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}while(GetTickCount64()<end);CHECK(result.status==mcp::Status::ready&&result.data["job_status"].toString()=="ready");return result;}
void lifetimes(){
    for(int mode=0;mode<6;++mode){std::cout<<"native_preview_retirement mode="<<mode<<'\n';auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));p->pollMcp();const auto view=p->mcpView();mcp::Caller caller{juce::Uuid(),view.epoch};
        const auto baseline=InstrumentDrumTestAccess::reserved(*p);const auto job=startJob(*p,caller);CHECK(job["job_id"].isString());if(!job["job_id"].isString())continue;
        if(mode==0){CHECK(call(*p,caller,mcp::Tool::getJob,job).data["job_status"].toString()=="running");CHECK(call(*p,caller,mcp::Tool::cancelJob,job).status==mcp::Status::ready);}
        if(mode==1||mode==2){const auto ready=waitJob(*p,caller,job);auto read=call(*p,caller,mcp::Tool::resourceRead,object({{"uri",ready.data["resource_uri"]}}));CHECK(read.status==mcp::Status::ready);CHECK(read.resourceLifetime);
            if(mode==1){mcp::Command control;control.control=mcp::CommandControl::ownerDisconnected;control.caller=caller;p->executeMcp(control);}else{p->setMcpEnabled(false);p->setMcpEnabled(true);}
            CHECK(read.resourceRevoked&&read.resourceRevoked->load());CHECK(InstrumentDrumTestAccess::reserved(*p)>baseline);read={};}
        if(mode==3)CHECK(copperArcInstance(*p));
        if(mode==4){const auto ready=waitJob(*p,caller,job);auto peer=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());juce::MemoryBlock state;p->getStateInformation(state);CHECK(peer->restoreProjectState(state.getData(),int(state.getSize())));peer->pollMcp();CHECK(peer->mcpView().instance!=view.instance);mcp::Caller other{juce::Uuid(),peer->mcpView().epoch};CHECK(call(*peer,other,mcp::Tool::resourceRead,object({{"uri",ready.data["resource_uri"]}})).status==mcp::Status::stale);CHECK(call(*p,caller,mcp::Tool::getJob,job).status==mcp::Status::ready);CHECK(call(*p,caller,mcp::Tool::cancelJob,job).status==mcp::Status::ready);}
        const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("MCP Previews").getChildFile(view.epoch.toDashedString()).getChildFile(view.instance.toDashedString());
        if(mode==5){p.reset();CHECK(root.findChildFiles(juce::File::findFiles,false,"*.wav").isEmpty());continue;}
        const auto deadline=GetTickCount64()+60000;while(InstrumentDrumTestAccess::reserved(*p)!=baseline&&GetTickCount64()<deadline){p->pollMcp();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}CHECK(InstrumentDrumTestAccess::reserved(*p)==baseline);CHECK(root.findChildFiles(juce::File::findFiles,false,"*.wav").isEmpty());
    }
}



void lockDelayedPreview(){
    for(auto action:{mcp::Tool::playPreview,mcp::Tool::stopPreview,mcp::Tool::cancelJob}){
        std::cout<<"actual_lock_delayed_preview action="<<mcp::toolName(action)<<std::endl;auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));p->pollMcp();mcp::Caller caller{juce::Uuid(),p->mcpView().epoch};const auto baseline=InstrumentDrumTestAccess::reserved(*p);const auto job=startJob(*p,caller);const auto ready=waitJob(*p,caller,job);if(ready.data["job_status"].toString()!="ready")continue;
        p->prepareToPlay(48000,128);juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;callback(*p,audio,midi);if(action!=mcp::Tool::playPreview){CHECK(call(*p,caller,mcp::Tool::playPreview,job).status==mcp::Status::ready);for(int n=0;n<5;++n)callback(*p,audio,midi);CHECK(audio.getMagnitude(0,128)>0);}
        const auto bytes=InstrumentDrumTestAccess::externalPlayerBytes(*p),reserved=InstrumentDrumTestAccess::reserved(*p),pcm=InstrumentDrumTestAccess::pcm(*p);const bool playing=InstrumentDrumTestAccess::externalPlayerPlaying(*p);CHECK(playing==(action!=mcp::Tool::playPreview));
        mcp::Command command;command.serial=700;command.caller=caller;command.call={action,p->mcpView().instance,job};std::atomic_bool entered=false,release=false,invoking=false,done=false;std::thread holder([&]{InstrumentDrumTestAccess::holdRestore(*p,entered,release);});while(!entered.load())std::this_thread::yield();command.deadlineMs=GetTickCount64()+2000;mcp::ToolResult result;
        std::thread dispatch([&]{invoking=true;result=p->executeMcp(command);done=true;});while(!invoking.load())std::this_thread::yield();unsigned blocks=0;auto next=GetTickCount64();while(GetTickCount64()<command.deadlineMs+10){if(GetTickCount64()>=next){callback(*p,audio,midi);++blocks;next=GetTickCount64()+5;}else std::this_thread::yield();}
        CHECK(!done.load());release=true;holder.join();dispatch.join();std::cout<<"lock-delayed result="<<int(result.status)<<" deadline="<<command.deadlineMs<<" finished="<<GetTickCount64()<<" real_callback_blocks="<<blocks<<std::endl;CHECK(blocks>100);CHECK(result.status==mcp::Status::hostControlUnavailable);CHECK(InstrumentDrumTestAccess::externalPlayerBytes(*p)==bytes);CHECK(InstrumentDrumTestAccess::externalPlayerPlaying(*p)==playing);CHECK(InstrumentDrumTestAccess::reserved(*p)==reserved&&InstrumentDrumTestAccess::pcm(*p)==pcm);CHECK(call(*p,caller,mcp::Tool::getJob,job).data["job_status"].toString()=="ready");for(int n=0;n<5;++n)callback(*p,audio,midi);CHECK(InstrumentDrumTestAccess::externalPlayerPlaying(*p)==playing);CHECK(InstrumentDrumTestAccess::externalPlayerBytes(*p)==bytes);
        (void)call(*p,caller,mcp::Tool::cancelJob,job);for(int n=0;n<5;++n)callback(*p,audio,midi);p->releaseResources();p->pollMcp();CHECK(InstrumentDrumTestAccess::externalPlayerBytes(*p)==0);CHECK(InstrumentDrumTestAccess::reserved(*p)==baseline);
    }
}

void manualPreviewTransition(){
    for(int mode=0;mode<2;++mode){std::cout<<"actual_external_to_manual_preview mode="<<mode<<std::endl;
        auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));auto project=std::make_unique<ProjectDocument>();std::uint64_t revision=0;CHECK(p->tryProjectSnapshot(*project,revision));
        RackDesign design;design.name="Manual transition drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
        RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,1}};op.destination.position=1;CHECK(p->applyRackOperation(op,revision)==RackCandidateStatus::ready);
        DrumPadTarget target;DrumCommitError error;CHECK(p->captureDrumPadTarget(op.newInstance,0,0,target,error));
        const auto file=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("manual-transition-"+juce::String(mode)+".wav");juce::MemoryOutputStream bytes;
        bytes.write("RIFF",4);bytes.writeInt(36+4800*2);bytes.write("WAVEfmt ",8);bytes.writeInt(16);bytes.writeShort(1);bytes.writeShort(1);bytes.writeInt(48000);bytes.writeInt(96000);bytes.writeShort(2);bytes.writeShort(16);bytes.write("data",4);bytes.writeInt(4800*2);for(int n=0;n<4800;++n)bytes.writeShort(short(8192*std::sin(n*.05)));CHECK(file.replaceWithData(bytes.getData(),bytes.getDataSize()));
        auto& importer=p->drumImport();CHECK(importer.open(target));importer.choose(file);const auto end=GetTickCount64()+15000;
        while(!importer.view().inspected&&GetTickCount64()<end)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);CHECK(importer.view().inspected);importer.edit(0,4800,1,"Manual transition");importer.prepare();
        while(importer.view().phase!=DrumImportPhase::ready&&GetTickCount64()<end)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);CHECK(importer.view().phase==DrumImportPhase::ready);if(importer.view().phase!=DrumImportPhase::ready)continue;
        p->pollMcp();mcp::Caller caller{juce::Uuid(),p->mcpView().epoch};const auto job=startJob(*p,caller);const auto ready=waitJob(*p,caller,job);if(ready.data["job_status"].toString()!="ready")continue;
        p->prepareToPlay(48000,128);juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;callback(*p,audio,midi);CHECK(call(*p,caller,mcp::Tool::playPreview,job).status==mcp::Status::ready);for(int n=0;n<5;++n)callback(*p,audio,midi);CHECK(audio.getMagnitude(0,128)>0);CHECK(InstrumentDrumTestAccess::externalPlayerBytes(*p)>0);
        CHECK(call(*p,caller,mcp::Tool::stopPreview,job).status==mcp::Status::ready);for(int n=0;n<5;++n)callback(*p,audio,midi);
        if(mode)juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
        importer.audition();CHECK(importer.view().phase==DrumImportPhase::auditioning);std::cout<<"external player charge after real manual start="<<InstrumentDrumTestAccess::externalPlayerBytes(*p)<<std::endl;CHECK(InstrumentDrumTestAccess::externalPlayerBytes(*p)==0);
        for(int n=0;n<5;++n)callback(*p,audio,midi);CHECK(audio.getMagnitude(0,128)>0);p->pollMcp();CHECK(InstrumentDrumTestAccess::externalPlayerBytes(*p)==0);CHECK(call(*p,caller,mcp::Tool::getJob,job).data["job_status"].toString()=="ready");auto read=call(*p,caller,mcp::Tool::resourceRead,object({{"uri",ready.data["resource_uri"]}}));CHECK(read.status==mcp::Status::ready);CHECK(call(*p,caller,mcp::Tool::cancelJob,job).status==mcp::Status::ready);read={};importer.cancel();for(int n=0;n<5;++n)callback(*p,audio,midi);p->releaseResources();
        const auto drain=GetTickCount64()+15000;while(InstrumentDrumTestAccess::reserved(*p)&&GetTickCount64()<drain){p->pollMcp();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}CHECK(InstrumentDrumTestAccess::reserved(*p)==0);CHECK(file.deleteFile());
    }
}

void storageRefusal(){
    std::cout<<"actual_reparse_storage_refusal_preserves_stage"<<std::endl;
    const auto diagnostic=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    const auto root=diagnostic.getChildFile("reparse-preview"),outside=diagnostic.getChildFile("outside-preview");CHECK(root.createDirectory().wasOk());CHECK(outside.createDirectory().wasOk());
    const auto sentinel=outside.getChildFile("sentinel.txt");CHECK(sentinel.replaceWithText("external marker"));const auto link=root.getChildFile("MCP Previews");CHECK(link.createDirectory().wasOk());
    HANDLE actor=CreateFileW(link.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);CHECK(actor!=INVALID_HANDLE_VALUE);if(actor==INVALID_HANDLE_VALUE)return;
    const std::wstring target(outside.getFullPathName().toWideCharPointer()),native=L"\\??\\"+target;
    struct Junction {DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;wchar_t names[1];};
    const auto extent=(native.size()+1+target.size()+1)*2;std::vector<std::byte> bytes(offsetof(Junction,names)+extent);auto* junction=reinterpret_cast<Junction*>(bytes.data());junction->tag=IO_REPARSE_TAG_MOUNT_POINT;junction->length=WORD(8+extent);junction->subLength=WORD(native.size()*2);junction->printOffset=WORD((native.size()+1)*2);junction->printLength=WORD(target.size()*2);std::memcpy(junction->names,native.c_str(),(native.size()+1)*2);std::memcpy(reinterpret_cast<std::byte*>(junction->names)+junction->printOffset,target.c_str(),(target.size()+1)*2);DWORD count=0;
    const bool installed=DeviceIoControl(actor,FSCTL_SET_REPARSE_POINT,bytes.data(),DWORD(bytes.size()),nullptr,0,&count,nullptr)!=FALSE;CHECK(installed);
    if(installed){const auto oldRoot=mcp::ipc::environment("AIRACK_MCP_FIXTURE_ROOT"),oldSuffix=mcp::ipc::environment("AIRACK_MCP_PIPE_SUFFIX");const auto suffix=juce::Uuid().toString().removeCharacters("-");SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",root.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",suffix.toWideCharPointer());
        {auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));p->pollMcp();mcp::Caller caller{juce::Uuid(),p->mcpView().epoch};const auto state=call(*p,caller,mcp::Tool::getState,object({}));CHECK(state.status==mcp::Status::ready);const auto prepared=call(*p,caller,mcp::Tool::prepareEdit,object({{"snapshot",state.data["snapshot"]},{"policy",object({})},{"edits",object({})}}));CHECK(prepared.status==mcp::Status::ready);const auto edit=object({{"edit_id",prepared.data["edit_id"]}});const auto baseline=InstrumentDrumTestAccess::reserved(*p);CHECK(call(*p,caller,mcp::Tool::renderPreview,edit).status==mcp::Status::failed);CHECK(call(*p,caller,mcp::Tool::getEdit,edit).status==mcp::Status::ready);CHECK(InstrumentDrumTestAccess::reserved(*p)==baseline);CHECK(sentinel.loadFileAsString()=="external marker");CHECK(outside.findChildFiles(juce::File::findFiles,true,"*.wav").isEmpty());}
        SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",oldRoot?oldRoot->toWideCharPointer():nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",oldSuffix?oldSuffix->toWideCharPointer():nullptr);
        struct {DWORD tag;WORD length,reserved;} remove{IO_REPARSE_TAG_MOUNT_POINT,0,0};CHECK(DeviceIoControl(actor,FSCTL_DELETE_REPARSE_POINT,&remove,sizeof(remove),nullptr,0,&count,nullptr));
    }
    CloseHandle(actor);CHECK(RemoveDirectoryW(link.getFullPathName().toWideCharPointer()));CHECK(sentinel.loadFileAsString()=="external marker");
}
void failedRenderKeepsStage(){
    std::cout<<"actual_clipped_render_preserves_stage"<<std::endl;auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));p->pollMcp();mcp::Caller caller{juce::Uuid(),p->mcpView().epoch};
    juce::Array<juce::var> notes;for(int pitch:{60,64,67,72})notes.add(object({{"start_beats",0.},{"duration_beats",1.},{"pitch",pitch},{"velocity",127},{"channel",1}}));const auto phrase=object({{"bars",1},{"meter_numerator",4},{"meter_denominator",4},{"grid","sixteenth"},{"swing",0.},{"notes",notes}});
    const auto state=call(*p,caller,mcp::Tool::getState,object({}));CHECK(state.status==mcp::Status::ready);const auto prepared=call(*p,caller,mcp::Tool::prepareEdit,object({{"snapshot",state.data["snapshot"]},{"policy",object({{"scope","both"}})},{"edits",object({{"pattern",phrase},{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",1.}}),object({{"id","output.gain_db"},{"normalized",1.}}),object({{"id","synth.1.level"},{"normalized",1.}}),object({{"id","output.protection"},{"normalized",0.}})}}})}}));CHECK(prepared.status==mcp::Status::ready);
    const auto edit=object({{"edit_id",prepared.data["edit_id"]}});const auto started=call(*p,caller,mcp::Tool::renderPreview,edit);CHECK(started.status==mcp::Status::ready);const auto job=object({{"job_id",started.data["job_id"]}});mcp::ToolResult ready;const auto end=GetTickCount64()+60000;do{p->pollMcp();ready=call(*p,caller,mcp::Tool::getJob,job);if(ready.data["job_status"].toString()!="running")break;juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}while(GetTickCount64()<end);
    std::cout<<"actual optional job "<<juce::JSON::toString(ready.data,true)<<std::endl;std::cout<<"actual prepared overload diff "<<juce::JSON::toString(prepared.data,true)<<std::endl;CHECK(ready.status==mcp::Status::ready&&ready.data["job_status"].toString()=="failed");CHECK(ready.data["render_status"].toString()=="clipped");CHECK(call(*p,caller,mcp::Tool::getEdit,edit).status==mcp::Status::ready);CHECK(call(*p,caller,mcp::Tool::cancelJob,job).status==mcp::Status::ready);CHECK(call(*p,caller,mcp::Tool::applyEdit,edit).status==mcp::Status::ready);
}
void nearCapacity(){
    std::cout<<"actual_owned_pcm_staged_preview_capacity"<<std::endl;
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(blankInstance(*p));std::array<juce::Uuid,4> racks;
    auto project=std::make_unique<ProjectDocument>();std::uint64_t revision=0;
    for(int bank=0;bank<4;++bank){CHECK(p->tryProjectSnapshot(*project,revision));RackDesign design;design.name="Capacity drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
        RackOperation op;op.design=design;op.newInstance=racks[bank]=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,std::uint8_t(bank)}};op.destination.position=std::uint8_t(bank);CHECK(p->applyRackOperation(op,revision)==RackCandidateStatus::ready);}
    std::vector<float> samples(960000);juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;
    for(int n=0;n<108;++n){std::fill(samples.begin(),samples.end(),float(n+1)/2048);std::uint64_t ticket=0;DrumCommitError error;CHECK(p->prepareDrumAsset(samples,2,ticket,error));
        drums::AssetPtr known;drums::AssetError assetError;CHECK(drums::makeSampleAsset(samples,2,known,assetError));drums::DrumPad metadata;metadata.asset=known->id();metadata.name="Actual capacity owner";
        const int slot=n<64?n:0;DrumPadTarget target;CHECK(p->captureDrumPadTarget(racks[slot/16],0,std::uint8_t(slot%16),target,error));CHECK(p->assignPreparedDrumPad(target,ticket,metadata,{},error));
        if(n==63)p->prepareToPlay(48000,256);
        if(n>=64&&n<80){midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(127)),0);for(int block=0;block<4;++block){callback(*p,audio,midi);midi.clear();}}
    }
    p->pollMcp();mcp::Caller caller{juce::Uuid(),p->mcpView().epoch};const auto pcm=InstrumentDrumTestAccess::pcm(*p);std::cout<<"actual unique native PCM="<<pcm<<" actual maximum-size assets="<<pcm/(480000*8)<<std::endl;CHECK(pcm+3*RackCatalogCaptureLimits{}.maximumPreparedBytes+ai::maximumPreviewFrames*8>drums::retainedDecodedLimit&&pcm<=drums::retainedDecodedLimit);if(pcm+3*RackCatalogCaptureLimits{}.maximumPreparedBytes+ai::maximumPreviewFrames*8<=drums::retainedDecodedLimit){p->releaseResources();return;}
    const auto state=call(*p,caller,mcp::Tool::getState,object({}));CHECK(state.status==mcp::Status::ready);
    const auto prepared=call(*p,caller,mcp::Tool::prepareEdit,object({{"snapshot",state.data["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.2}})}}})}}));CHECK(prepared.status==mcp::Status::ready);
    const auto edit=object({{"edit_id",prepared.data["edit_id"]}});const auto reserved=InstrumentDrumTestAccess::reserved(*p);
    CHECK(call(*p,caller,mcp::Tool::renderPreview,edit).status==mcp::Status::capacity);CHECK(call(*p,caller,mcp::Tool::getEdit,edit).status==mcp::Status::ready);
    CHECK(InstrumentDrumTestAccess::pcm(*p)==pcm&&InstrumentDrumTestAccess::reserved(*p)==reserved);CHECK(call(*p,caller,mcp::Tool::discardEdit,edit).status==mcp::Status::ready);
    CHECK(blankInstance(*p));p->prepareToPlay(48000,256);for(int n=0;n<8;++n)callback(*p,audio,midi);p->releaseResources();p->pollMcp();CHECK(InstrumentDrumTestAccess::reserved(*p)==0);CHECK(InstrumentDrumTestAccess::pcm(*p)==0);
}
void inactiveAudioRetirement(){
    for(int mode=0;mode<3;++mode){std::cout<<"actual_play_inactive_audio_retirement mode="<<mode<<std::endl;
        auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));p->pollMcp();const auto view=p->mcpView();mcp::Caller caller{juce::Uuid(),view.epoch};const auto baseline=InstrumentDrumTestAccess::reserved(*p);
        const auto job=startJob(*p,caller);const auto ready=waitJob(*p,caller,job);if(ready.data["job_status"].toString()!="ready")continue;
        p->prepareToPlay(48000,128);juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;callback(*p,audio,midi);CHECK(call(*p,caller,mcp::Tool::playPreview,job).status==mcp::Status::ready);
        for(int n=0;n<5;++n)callback(*p,audio,midi);CHECK(audio.getMagnitude(0,128)>0);
        if(mode==1)p->suspendProcessing(true);
        const auto expire=GetTickCount64()+300;while(GetTickCount64()<expire)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        CHECK(call(*p,caller,mcp::Tool::playPreview,job).status==mcp::Status::audioUnavailable);
        if(mode==0)CHECK(call(*p,caller,mcp::Tool::stopPreview,job).status==mcp::Status::ready);
        if(mode==2)p->setMcpEnabled(false);else CHECK(call(*p,caller,mcp::Tool::cancelJob,job).status==mcp::Status::ready);
        const auto end=GetTickCount64()+500;do{p->pollMcp();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}while(InstrumentDrumTestAccess::reserved(*p)!=baseline&&GetTickCount64()<end);
        std::cout<<"inactive retained="<<InstrumentDrumTestAccess::reserved(*p)<<" baseline="<<baseline<<std::endl;
        CHECK(InstrumentDrumTestAccess::reserved(*p)==baseline);
        p->suspendProcessing(false);for(int n=0;n<5;++n)callback(*p,audio,midi);p->pollMcp();CHECK(InstrumentDrumTestAccess::reserved(*p)==baseline);
    }
}

}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;if(argc==2&&juce::String(argv[1])=="--lock-deadline"){_CrtSetAllocHook(allocationHook);lockDelayedPreview();CHECK(callbackAllocations==0&&callbackFrees==0);std::cout<<"callback allocations="<<callbackAllocations<<" frees="<<callbackFrees<<"\n"<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--render"){_CrtSetAllocHook(allocationHook);coldInventoryReadiness();renderBoundary(false);renderBoundary(true);renderBoundary(false,true);renderBoundary(true,true);CHECK(callbackAllocations==0&&callbackFrees==0);std::cout<<"callback allocations="<<callbackAllocations<<" frees="<<callbackFrees<<"\n"<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--manual-transition"){manualPreviewTransition();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--refusals"){storageRefusal();failedRenderKeepsStage();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--capacity"){nearCapacity();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--inactive-audio"){inactiveAudioRetirement();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}_CrtSetAllocHook(allocationHook);renderBoundary(false);renderBoundary(true);renderBoundary(false,true);renderBoundary(true,true);lifetimes();lockDelayedPreview();manualPreviewTransition();nearCapacity();inactiveAudioRetirement();storageRefusal();failedRenderKeepsStage();CHECK(callbackAllocations==0&&callbackFrees==0);std::cout<<"callback allocations="<<callbackAllocations<<" frees="<<callbackFrees<<"\n";std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
