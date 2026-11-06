#include "ProjectDocument.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "RackPackageStore.h"
#include "McpTestRackRoot.h"
#include "McpService.h"
#include "CompanionLaunch.h"
#include "DrumBundle.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <future>
#include <set>
#include <cmath>
#include <cstring>
#include <crtdbg.h>

// Product acceptance uses AudioPluginInstance and public VST3 host state/parameters only.
namespace {
using namespace airack;
int checks=0,failures=0;
thread_local bool inCallback=false;
unsigned callbackAllocations=0,callbackFrees=0;
int allocationHook(int type,void*,size_t,int,long,const unsigned char*,int){if(inCallback){if(type==_HOOK_ALLOC||type==_HOOK_REALLOC)++callbackAllocations;if(type==_HOOK_FREE||type==_HOOK_REALLOC)++callbackFrees;}return TRUE;}
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cout<<"FAIL "<<line<<" "<<text<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
auto object(std::initializer_list<std::pair<const char*,juce::var>> v){return mcp::ipc::object(v);}
void pump(int ms=2){juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);}
template<class F> bool until(F f,unsigned ms=5000){const auto end=GetTickCount64()+ms;do{if(f())return true;pump();}while(GetTickCount64()<end);return f();}
// A unique installed design proves the loaded VST3 reads this process-owned store.
struct OwnedRackMarker { juce::String id, revision, hash; };
std::optional<OwnedRackMarker> installOwnedRackMarker(const juce::File& root) {
    const auto built=factoryRackDesigns(); if(built.empty())return {};
    auto design=built.front();design.id=juce::Uuid();design.name="Hosted MCP isolation "+design.id.toString();
    juce::MemoryBlock recipe;if(!encodeRackDesign(design,recipe,RackValidationMode::instanceRecall))return {};
    constexpr unsigned char bytes[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,96,96,96,96,0,0,0,5,0,1,165,246,69,64,0,0,0,0,73,69,78,68,174,66,96,130};
    const juce::MemoryBlock png(bytes,sizeof(bytes));RackPackageManifest manifest;
    manifest.ref={"mcp-host-isolation-"+design.id.toString().removeCharacters("-"),"1.0.0",{}};
    manifest.metadata={design.id,design.revision,juce::SHA256(recipe).toHexString(),design.name,"Test fixture","Host isolation","Owned MCP host fixture",design.role,{"analog"}};
    manifest.designPath="rack.json";manifest.screenshotPath="screenshot.png";
    manifest.files={{"rack.json",juce::SHA256(recipe).toHexString(),"design",recipe.getSize()},{"screenshot.png",juce::SHA256(png).toHexString(),"png",png.getSize()}};
    juce::Array<juce::var> modules;std::set<std::pair<int,int>> distinct;
    for(const auto& unit:design.units)if(distinct.emplace(int(unit.module),int(unit.algorithmRevision)).second)modules.add(object({{"module",int(unit.module)},{"algorithmRevision",int(unit.algorithmRevision)}}));
    manifest.capabilities=object({{"airMin","0.1.0"},{"modules",modules},{"nativeEditor",""}});juce::String text;if(!encodeRackPackageManifest(manifest,text))return {};
    const auto file=root.getChildFile("host-isolation.airrack");juce::ZipFile::Builder zip;const juce::Time stamp(1980,0,1,0,0);
    zip.addEntry(new juce::MemoryInputStream(text.toRawUTF8(),text.getNumBytesAsUTF8(),true),0,"package.json",stamp);
    zip.addEntry(new juce::MemoryInputStream(recipe,true),0,"rack.json",stamp);zip.addEntry(new juce::MemoryInputStream(png,true),0,"screenshot.png",stamp);
    {auto out=file.createOutputStream();if(!out||!zip.writeToStream(*out,nullptr))return {};}
    {juce::FileInputStream in(file);if(!in.openedOk())return {};manifest.ref.sha256=juce::SHA256(in).toHexString();}
    RackPackageStore store({root.getChildFile("Rack Library")});
    if(std::async(std::launch::async,[&]{return store.install(file,manifest.ref,RackPackageActivation::makeActive,{});}).get()!=RackLibraryStatus::ready)return {};
    return OwnedRackMarker{design.id.toString(),mcp::ipc::decimal(design.revision),manifest.metadata.canonicalSha256};
}
struct ScopedMcpRackRoot {
    juce::String previous;bool hadPrevious=false,installed=false;
    explicit ScopedMcpRackRoot(const juce::File& root) {
        SetLastError(ERROR_SUCCESS);const auto size=GetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",nullptr,0);
        hadPrevious=size>0 || GetLastError()!=ERROR_ENVVAR_NOT_FOUND;
        if(size>0){std::vector<wchar_t> value(size);GetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",value.data(),size);previous=juce::String(value.data());}
        installed=SetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",root.getFullPathName().toWideCharPointer())!=FALSE;
    }
    ~ScopedMcpRackRoot(){if(installed){if(hadPrevious)SetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",previous.toWideCharPointer());else SetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",nullptr);}}
};
struct Stdio {
    HANDLE process=nullptr,input=nullptr,output=nullptr;std::string buffered;unsigned serial=0;
    juce::File transcript;unsigned responseWorkDelayMs=0;std::atomic_bool frameReceived=false;
    ~Stdio(){close();}
    bool start(const juce::File& helper,const juce::File& root,const juce::String& suffix){
        transcript=root.getChildFile("raw-mcp-"+juce::Uuid().toString()+".jsonl");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read=nullptr,write=nullptr;
        if(!CreatePipe(&read,&input,&sa,4096)||!CreatePipe(&output,&write,&sa,4096))return false;
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        HANDLE error=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
        STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=read;si.StartupInfo.hStdOutput=write;si.StartupInfo.hStdError=error;
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<std::byte> storage(bytes);si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes);HANDLE inherited[]{read,write,error};UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr);
        const auto command="\""+helper.getFullPathName()+"\" --mcp-stdio --fixture-root \""+root.getFullPathName()+"\" --pipe-suffix "+suffix;
        std::wstring s(command.toWideCharPointer());std::vector<wchar_t> line(s.begin(),s.end());line.push_back(0);PROCESS_INFORMATION pi{};
        const bool ok=CreateProcessW(helper.getFullPathName().toWideCharPointer(),line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=FALSE;
        CloseHandle(read);CloseHandle(write);CloseHandle(error);DeleteProcThreadAttributeList(si.lpAttributeList);if(!ok)return false;process=pi.hProcess;CloseHandle(pi.hThread);return true;
    }
    bool send(juce::var value){const auto text=juce::JSON::toString(value,true)+"\n";CHECK(transcript.appendText(juce::JSON::toString(object({{"direction","stdin"},{"message",value}}),true)+"\n"));DWORD bytes=0;return WriteFile(input,text.toRawUTF8(),DWORD(text.getNumBytesAsUTF8()),&bytes,nullptr)&&bytes==text.getNumBytesAsUTF8();}
    juce::var receive(){const auto end=GetTickCount64()+5000;do{
        if(auto at=buffered.find('\n');at!=std::string::npos){const auto line=buffered.substr(0,at);buffered.erase(0,at+1);frameReceived=true;if(responseWorkDelayMs)Sleep(responseWorkDelayMs);juce::var value;CHECK(ai::parseStrictJson(line,value,mcp::maximumResultBytes));CHECK(transcript.appendText(juce::JSON::toString(object({{"direction","stdout"},{"message",value}}),true)+"\n"));return value;}
        DWORD available=0;if(!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr))break;
        if(available){char data[65536];DWORD bytes=0;if(!ReadFile(output,data,(std::min)(DWORD(sizeof(data)),available),&bytes,nullptr))break;buffered.append(data,bytes);if(buffered.size()>mcp::maximumResultBytes)return {};}else Sleep(1);
    }while(GetTickCount64()<end);return {};}
    juce::var rpc(const char* method,juce::var params){const auto id=++serial;CHECK(send(object({{"jsonrpc","2.0"},{"id",int(id)},{"method",method},{"params",params}})));
        juce::var reply;frameReceived=false;std::atomic_bool done=false;std::thread reader([&]{reply=receive();done=true;});
        // Bound transport, not this client's large JSON/transcript work; keep the hosted plugin pumping during both.
        CHECK(until([&]{return frameReceived.load()||done.load();},5500));while(!done.load())pump();reader.join();CHECK(int(reply["id"])==int(id));CHECK(!reply.hasProperty("error"));return reply["result"];}
    juce::var tool(const char* name,juce::var args,const char* expected="ready"){
        juce::var result;const bool waitForInventory=juce::String(name)=="get_state"&&juce::String(expected)=="ready";const auto deadline=GetTickCount64()+30000;
        do{result=rpc("tools/call",object({{"name",name},{"arguments",args}}))["structuredContent"];
            if(!waitForInventory||result["status"].toString()!="unavailable")break;
            Sleep(10);pump();
        }while(GetTickCount64()<deadline);
        std::cout<<"MCP "<<name<<" status="<<result["status"].toString()<<std::endl;CHECK(result["status"].toString()==expected);return result;}
    bool initialize(){const auto result=rpc("initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","actual-vst3-fixture"},{"version","1"}})}}));
        return result["protocolVersion"].toString()=="2025-11-25"&&send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}}));}
    void close(){if(input){CloseHandle(input);input=nullptr;}if(process){if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0){CHECK(false);TerminateProcess(process,97);WaitForSingleObject(process,5000);}else{DWORD code=99;GetExitCodeProcess(process,&code);CHECK(code==0);}CloseHandle(process);process=nullptr;}if(output){CloseHandle(output);output=nullptr;}}
};
void stdioClientWorkTest(){
    juce::TemporaryFile transcript(".jsonl");Stdio client;client.transcript=transcript.getFile();HANDLE requests=nullptr,replies=nullptr;
    CHECK(CreatePipe(&requests,&client.input,nullptr,0)!=FALSE);CHECK(CreatePipe(&client.output,&replies,nullptr,0)!=FALSE);
    if(!requests||!replies){if(requests)CloseHandle(requests);if(replies)CloseHandle(replies);return;}
    const std::string response="{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"fixture\":true}}\n";DWORD written=0;
    CHECK(WriteFile(replies,response.data(),DWORD(response.size()),&written,nullptr)&&written==response.size());
    client.responseWorkDelayMs=5600;CHECK(bool(client.rpc("fixture",object({}))["fixture"]));CloseHandle(requests);CloseHandle(replies);client.close();
    Stdio stalled;HANDLE heldWriter=nullptr;CHECK(CreatePipe(&stalled.output,&heldWriter,nullptr,0)!=FALSE);if(!heldWriter)return;
    const auto began=GetTickCount64();CHECK(stalled.receive().isVoid());const auto elapsed=GetTickCount64()-began;CHECK(elapsed>=5000&&elapsed<5500);std::cout<<"STDIO_READ_TIMEOUT elapsed_ms="<<elapsed<<std::endl;CloseHandle(heldWriter);
}
struct ServiceChild {
    juce::ChildProcess child;
    ~ServiceChild(){if(child.isRunning())child.kill();}
    bool start(const juce::File& helper,const juce::File& root,const juce::String& suffix){return child.start(juce::StringArray{helper.getFullPathName(),"--mcp-service","--fixture-root",root.getFullPathName(),"--pipe-suffix",suffix},juce::ChildProcess::wantStdOut);}
};
struct GestureListener final:juce::AudioProcessorParameter::Listener {
    bool fired=false;std::function<void()> action;
    void parameterValueChanged(int,float)override{}
    void parameterGestureChanged(int,bool starting)override{if(starting&&!fired){fired=true;if(action)action();}}
};
struct Clock final:juce::AudioPlayHead {
    int sample=0;bool available=true;
    juce::Optional<PositionInfo> getPosition()const override {if(!available)return {};PositionInfo p;p.setBpm(120);p.setTimeInSamples(sample);p.setTimeInSeconds(sample/48000.);p.setPpqPosition(sample/24000.);p.setIsPlaying(true);p.setTimeSignature(TimeSignature{4,4});return p;}
};
juce::MemoryBlock envelope(const juce::MemoryBlock& owned){juce::XmlElement xml("VST3PluginState");xml.createNewChildElement("IComponent")->addTextElement(owned.toBase64Encoding());juce::MemoryBlock result;juce::AudioProcessor::copyXmlToBinary(xml,result);return result;}
juce::MemoryBlock ownedState(juce::AudioPluginInstance& p){juce::MemoryBlock state,component;p.getStateInformation(state);auto xml=juce::AudioProcessor::getXmlFromBinary(state.getData(),int(state.getSize()));CHECK(xml&&xml->hasTagName("VST3PluginState"));
    const auto* child=xml?xml->getChildByName("IComponent"):nullptr;CHECK(child&&component.fromBase64Encoding(child->getAllSubText()));if(!child)return {};
    constexpr std::string_view magic="JUCEPrivateData";const auto size=component.getSize();const auto* data=static_cast<const char*>(component.getData());
    if(size>=magic.size()+16&&std::string_view(data+size-magic.size(),magic.size())==magic){juce::MemoryInputStream in(data+size-magic.size()-8,8,false);const auto length=in.readInt64();CHECK(length>=0&&std::uint64_t(length)<=size-magic.size()-16);if(length>=0&&std::uint64_t(length)<=size-magic.size()-16)component.setSize(size-magic.size()-16-std::size_t(length));}return component;}
juce::MemoryBlock fixture(const char* label,bool drum=false){auto project=std::make_unique<ProjectDocument>();project->sound=copperArcSound();project->sound.name=label;
    const auto registry=parameterRegistry();for(std::size_t n=0;n<registry.size();++n)if(registry[n].id.endsWith(".noise"))project->sound.values[n]=0;
    project->pattern.bars=4;project->pattern.noteCount=1;project->pattern.notes[0]={0,.25,std::uint8_t(drum?36:60),100,1};
    if(drum){project->sound=SoundDocument{};project->sound.name=label;project->sound.values[drumModeParameterStart]=1;}
    juce::MemoryBlock bytes;CHECK(encodeProjectState(*project,bytes));
    if(drum){using namespace drums;std::vector<float> pcm(4096);for(std::size_t n=0;n<pcm.size();++n)pcm[n]=float(.25*std::sin(n*.07)*(1.-double(n)/pcm.size()));
        AssetPtr asset;AssetError ae;CHECK(makeSampleAsset(pcm,1,asset,ae));DrumSlots slots{};slots[0][0].asset=asset->id();slots[0][0].name="Owned MCP fixture pad";DrumDocumentPtr document;DrumError error;
        CHECK(makeDrumDocument(slots,document,error));DrumBundlePtr bundle;const std::array<AssetPtr,1> owners{asset};CHECK(makeDrumBundle(document,{static_cast<const std::uint8_t*>(bytes.getData()),bytes.getSize()},owners,bundle,error));CHECK(encodeDrumBundle(*bundle,bytes,error));}
    return bytes;}
std::vector<float> audio(juce::AudioPluginInstance& p,Clock& clock,int note=60){p.releaseResources();p.prepareToPlay(48000,128);clock.sample=0;juce::AudioBuffer<float> buffer(2,128);juce::MidiBuffer midi;std::vector<float> result;result.reserve(16384);
    for(int n=0;n<64;++n){buffer.clear();midi.clear();if(n==0)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);if(n==32)midi.addEvent(juce::MidiMessage::noteOff(1,note),0);inCallback=n>0;p.processBlock(buffer,midi);inCallback=false;for(int ch=0;ch<2;++ch)result.insert(result.end(),buffer.getReadPointer(ch),buffer.getReadPointer(ch)+128);clock.sample+=128;}return result;}
bool exact(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;}
float peak(const std::vector<float>& a){float result=0;for(auto f:a){CHECK(std::isfinite(f));result=(std::max)(result,std::abs(f));}return result;}
struct Host {
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;Clock clock;
    explicit Host(const juce::File& file){format.findAllTypesForFile(descriptions,file.getFullPathName());CHECK(descriptions.size()==1&&descriptions[0]->name=="air");}
    std::unique_ptr<juce::AudioPluginInstance> create(const juce::MemoryBlock& state){if(descriptions.isEmpty())return {};juce::String error;auto p=format.createInstanceFromDescription(*descriptions[0],48000,128,error);CHECK(p);if(!p)return {};
        CHECK(p->acceptsMidi()&&p->getTotalNumOutputChannels()==2);p->setPlayHead(&clock);p->setNonRealtime(true);const auto wrapped=envelope(state);p->setStateInformation(wrapped.getData(),int(wrapped.getSize()));p->prepareToPlay(48000,128);return p;}
};
void captureProduct(const juce::File& plugin,const juce::File& root){
    Host host(plugin);juce::Array<juce::var> rows;
    for(bool drum:{false,true}){const juce::String name=drum?"drum":"synth";const auto wanted=fixture(drum?"Task6 reference owned drum":"Task6 reference synth",drum);auto p=host.create(wanted);if(!p)continue;
        const auto rendered=audio(*p,host.clock,drum?36:60);CHECK(peak(rendered)>0);const auto actual=ownedState(*p);CHECK(actual==wanted);
        const auto pcm=root.getChildFile(name+".f32"),state=root.getChildFile(name+".state");CHECK(pcm.replaceWithData(rendered.data(),rendered.size()*sizeof(float)));CHECK(state.replaceWithData(actual.getData(),actual.getSize()));
        rows.add(object({{"name",name},{"pcm_sha256",juce::SHA256(pcm).toHexString()},{"state_sha256",juce::SHA256(state).toHexString()},{"frames",8192},{"channels",2},{"rate",48000},{"block",128},{"latency",p->getLatencySamples()}}));p->releaseResources();p.reset();}
    const auto modules=plugin.findChildFiles(juce::File::findFiles,true,"*.vst3");CHECK(modules.size()==1);
    CHECK(root.getChildFile("capture.json").replaceWithText(juce::JSON::toString(object({{"plugin",plugin.getFullPathName()},{"plugin_sha256",modules.size()==1?juce::SHA256(modules[0]).toHexString():juce::String()},{"rows",rows},{"callback_allocations",int(callbackAllocations)},{"callback_frees",int(callbackFrees)}}))));
}
juce::var target(const juce::var& id){return object({{"instance_id",id}});}
juce::var prepare(Stdio& bridge,const juce::var& id,double value){const auto state=bridge.tool("get_state",target(id));return bridge.tool("prepare_edit",object({{"instance_id",id},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",value}})}}})}}));}
bool verifyOwnedRackMarker(Stdio& bridge,const juce::var& id,const OwnedRackMarker& marker){
    const auto state=bridge.tool("get_state",target(id));
    if(state["status"].toString()!="ready")return false;
    const auto racks=bridge.tool("get_catalog",object({{"instance_id",id},{"kind","racks"},{"limit",65536}}));
    bool found=false;if(const auto* entries=racks["entries"].getArray())for(const auto& row:*entries)
        if(row["id"].toString()==marker.id && row["revision"].toString()==marker.revision && row["content_sha256"].toString()==marker.hash)found=true;
    CHECK(found);return found;
}
bool captureIsolationPreflight(const juce::File& plugin,const juce::File& helper,const juce::File& root,const juce::String& suffix,const OwnedRackMarker& marker){
    if(!SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",root.getFullPathName().toWideCharPointer()))return false;
    struct Cleanup { ~Cleanup(){SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);} } cleanup;
    if(!SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",suffix.toWideCharPointer()))return false;
    ServiceChild service;if(!service.start(helper,root,suffix))return false;
    Stdio bridge;if(!bridge.start(helper,root,suffix)||!bridge.initialize())return false;
    Host host(plugin);auto instance=host.create(fixture("Capture isolation preflight"));if(!instance)return false;
    juce::var list;if(!until([&]{list=bridge.tool("list_instances",object({}))["instances"];return list.size()==1;}))return false;
    const auto id=list[0]["instance_id"];const bool verified=id.isString()&&verifyOwnedRackMarker(bridge,id,marker);
    instance->releaseResources();instance.reset();bridge.close();return verified;
}
void warningProjection(const juce::File& plugin,const juce::File& helper,const juce::File& root,const juce::String& suffix,const OwnedRackMarker* marker){
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",root.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",suffix.toWideCharPointer());
    ServiceChild service;CHECK(service.start(helper,root,suffix));Stdio bridge;CHECK(bridge.start(helper,root,suffix));CHECK(bridge.initialize());
    auto project=std::make_unique<ProjectDocument>();auto candidate=std::make_unique<SoundDocument>();RackDifference difference;
    RackOperation drum;RackDesign design;design.name="Warning drum";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
    drum.design=design;drum.newInstance=juce::Uuid();drum.allowSlotReuse=true;drum.allocation={{0,0}};
    CHECK(makeRackCandidate(project->sound,drum,SoundEditOrigin::manual,*candidate,difference)==RackCandidateStatus::ready);project->sound=*candidate;
    for(std::uint8_t bank=1;bank<4;++bank){RackOperation op;op.design=factoryRackDesigns()[0];op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,bank}};op.destination={RackRole::synth,masterChain,bank};
        CHECK(makeRackCandidate(project->sound,op,SoundEditOrigin::manual,*candidate,difference)==RackCandidateStatus::ready);project->sound=*candidate;}
    project->sound.macros[0]={parameterRegistry()[drumParameterStart].stateKey,.3f};project->sound.name="Actual warning fixture";
    juce::MemoryBlock state;CHECK(encodeProjectState(*project,state));Host host(plugin);auto p=host.create(state);if(!p)return;
    juce::var list;CHECK(until([&]{list=bridge.tool("list_instances",object({}))["instances"];return list.size()==1;}));if(list.size()!=1)return;const auto id=list[0]["instance_id"];
    if(marker&&!verifyOwnedRackMarker(bridge,id,*marker))return;
    const auto ordinary=prepare(bridge,id,.23);CHECK(ordinary["diff"]["automation_warning"].isString()&&ordinary["diff"]["automation_warning"].toString().isEmpty());
    CHECK(bridge.tool("get_edit",object({{"instance_id",id},{"edit_id",ordinary["edit_id"]}}))["diff"]==ordinary["diff"]);bridge.tool("discard_edit",object({{"instance_id",id},{"edit_id",ordinary["edit_id"]}}));
    const auto before=bridge.tool("get_state",target(id));const auto catalog=bridge.tool("get_catalog",object({{"instance_id",id},{"kind","racks"},{"limit",65536}}));
    juce::var copper;for(const auto& row:*catalog["entries"].getArray())if(row["name"].toString()=="Copper Arc")copper=row;CHECK(copper.getDynamicObject());if(!copper.getDynamicObject())return;
    const auto selection=object({{"local_id",1},{"design",copper["id"]},{"revision",copper["revision"]},{"content_sha256",copper["content_sha256"]},{"destination",object({{"role","synth"},{"chain",4},{"position",0}})}});
    const auto edits=object({{"reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>{drum.newInstance.toDashedString()}},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}})},{"catalog_selections",juce::Array<juce::var>{selection}}});
    const auto stage=bridge.tool("prepare_edit",object({{"instance_id",id},{"snapshot",catalog["snapshot"]},{"policy",object({{"allow_slot_reuse",true}})},{"edits",edits}}));
    CHECK(stage["edit_id"].isString());CHECK(stage["diff"]["automation_warning"].toString().contains("Reuse bank 1"));CHECK(stage["diff"]["automation_warning"].toString().contains("Global macro 1 target affected"));
    const auto handle=object({{"instance_id",id},{"edit_id",stage["edit_id"]}});CHECK(bridge.tool("get_edit",handle)["diff"]==stage["diff"]);
    CHECK(ownedState(*p)==state);CHECK(bridge.tool("get_state",target(id))==before);bridge.tool("discard_edit",handle);CHECK(ownedState(*p)==state);
    p->releaseResources();p.reset();bridge.close();SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void workflow(const juce::File& plugin,const juce::File& helper,const juce::File& root,const juce::String& suffix,bool hold,const OwnedRackMarker* marker){
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",root.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",suffix.toWideCharPointer());
    ServiceChild service;CHECK(service.start(helper,root,suffix));
    Stdio bridge;CHECK(bridge.start(helper,root,suffix));CHECK(bridge.initialize());CHECK(bridge.tool("list_instances",object({}))["instances"].size()==0);
    Host host(plugin);const auto stateA=fixture("Task6 actual VST3 A"),stateB=fixture("Task6 actual VST3 B",true);auto a=host.create(stateA),b=host.create(stateB);if(!a||!b)return;
    juce::var list;CHECK(until([&]{list=bridge.tool("list_instances",object({}))["instances"];return list.size()==2;}));if(list.size()!=2)return;
    juce::var aid,bid;for(const auto& row:*list.getArray()){if(row["label"].toString()=="Task6 actual VST3 A")aid=row["instance_id"];if(row["label"].toString()=="Task6 actual VST3 B")bid=row["instance_id"];}CHECK(aid.isString()&&bid.isString()&&aid!=bid);if(!aid.isString()||!bid.isString())return;
    bridge.close();pump(50);
    const auto aBefore=audio(*a,host.clock),bBefore=audio(*b,host.clock,36);CHECK(peak(aBefore)>0);CHECK(peak(bBefore)>0);CHECK(ownedState(*a)==stateA);CHECK(ownedState(*b)==stateB);
    CHECK(bridge.start(helper,root,suffix));CHECK(bridge.initialize());
    const auto idleA=audio(*a,host.clock),idleB=audio(*b,host.clock,36);CHECK(exact(aBefore,idleA));CHECK(exact(bBefore,idleB));CHECK(ownedState(*a)==stateA);CHECK(ownedState(*b)==stateB);
    if(marker&&!verifyOwnedRackMarker(bridge,aid,*marker))return;
    if(failures){std::cout<<"ACTUAL_VST3_NOT_READY: prerequisite failure"<<std::endl;return;}
    if(hold){const auto modules=plugin.findChildFiles(juce::File::findFiles,true,"*.vst3");CHECK(modules.size()==1);if(modules.size()!=1)return;
        const auto ready=object({{"ready",true},{"scope","actual JUCE-hosted VST3; public host APIs only"},{"plugin",plugin.getFullPathName()},{"plugin_binary",modules[0].getFullPathName()},{"plugin_sha256",juce::SHA256(modules[0]).toHexString()},{"helper",helper.getFullPathName()},{"instances",list},{"suffix",suffix},{"root",root.getFullPathName()},{"editor_closed",true}});
        CHECK(root.getChildFile("workflow-ready.json").replaceWithText(juce::JSON::toString(ready)));std::cout<<"ACTUAL_VST3_READY "<<juce::JSON::toString(ready,true)<<std::endl;
        juce::AudioBuffer<float> buffer(2,128);juce::MidiBuffer midi;const auto deadline=GetTickCount64()+300000;
        while(!root.getChildFile("workflow.stop").existsAsFile()&&GetTickCount64()<deadline){buffer.clear();a->processBlock(buffer,midi);buffer.clear();b->processBlock(buffer,midi);host.clock.sample+=128;pump(5);}
        CHECK(root.getChildFile("workflow.stop").existsAsFile());a->releaseResources();b->releaseResources();a.reset();b.reset();pump(50);bridge.close();return;}
    const auto before=bridge.tool("get_state",target(aid)),other=bridge.tool("get_state",target(bid));
    CHECK(before["linked_context"].size()==0);CHECK(before["bpm"].isDouble()||before["bpm"].isInt());
    const auto catalog=bridge.tool("get_catalog",target(aid));CHECK(catalog["edits_schema"].getDynamicObject());
    const auto combined=juce::JSON::parse(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.35}],"macro_edits":[{"index":1,"target_id":"global.master_level","amount":0.2}],"pattern":{"bars":4,"meter_numerator":4,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[{"start_beats":0,"duration_beats":0.25,"pitch":64,"velocity":100,"channel":1}]}})");
    const auto composite=bridge.tool("prepare_edit",object({{"instance_id",aid},{"snapshot",before["snapshot"]},{"policy",object({{"scope","both"},{"allow_macro_changes",true}})},{"edits",combined}}));
    bridge.tool("discard_edit",object({{"instance_id",aid},{"edit_id",composite["edit_id"]}}));CHECK(ownedState(*a)==stateA);
    const auto racks=bridge.tool("get_catalog",object({{"instance_id",aid},{"kind","racks"},{"limit",65536}}));
    juce::var row;for(const auto& r:*racks["entries"].getArray())if(r["name"].toString()=="Copper Arc")row=r;CHECK(row.getDynamicObject());
    if(row.getDynamicObject()){bridge.tool("get_catalog",object({{"instance_id",aid},{"snapshot",racks["snapshot"]},{"kind","definition"},{"design_id",row["id"]},{"design_revision",row["revision"]},{"content_sha256",row["content_sha256"]}}));
        const auto selection=object({{"local_id",1},{"design",row["id"]},{"revision",row["revision"]},{"content_sha256",row["content_sha256"]},{"destination",object({{"role","synth"},{"chain",4},{"position",1}})}});
        const auto rackEdit=bridge.tool("prepare_edit",object({{"instance_id",aid},{"snapshot",racks["snapshot"]},{"policy",object({})},{"edits",object({{"catalog_selections",juce::Array<juce::var>{selection}}})}}));bridge.tool("discard_edit",object({{"instance_id",aid},{"edit_id",rackEdit["edit_id"]}}));}
    auto staged=prepare(bridge,aid,.23);const auto edit=object({{"instance_id",aid},{"edit_id",staged["edit_id"]}});CHECK(staged["diff"].getDynamicObject());
    std::unique_ptr<juce::AudioProcessorEditor> editor(a->createEditorIfNeeded());CHECK(editor);editor.reset();pump();bridge.tool("get_edit",edit);
    const auto first=bridge.tool("apply_edit",edit);CHECK(bool(first["changed"]));CHECK(bridge.tool("apply_edit",edit)==first);const auto committed=ownedState(*a);CHECK(committed!=stateA);CHECK(ownedState(*b)==stateB);
    CHECK(!exact(aBefore,audio(*a,host.clock)));CHECK(exact(bBefore,audio(*b,host.clock,36)));CHECK(bridge.tool("get_state",target(bid))==other);
    const auto undo=bridge.tool("undo_edit",object({{"instance_id",aid},{"transaction_id",first["transaction_id"]},{"expected_revision",first["revision"]}}));CHECK(ownedState(*a)==stateA);CHECK(exact(aBefore,audio(*a,host.clock)));
    bridge.tool("redo_edit",object({{"instance_id",aid},{"transaction_id",first["transaction_id"]},{"expected_revision",undo["revision"]}}));CHECK(ownedState(*a)==committed);
    staged=prepare(bridge,aid,.31);juce::AudioProcessorParameter* master=nullptr;for(auto* parameter:a->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==juce::String(juce::VST3ClientExtensions::convertJuceParameterId("global.master_level",true)))master=parameter;
    CHECK(master);if(master){master->setValueNotifyingHost(.47f);juce::AudioBuffer<float> block(2,128);juce::MidiBuffer midi;a->processBlock(block,midi);}pump();
    const auto manual=bridge.tool("get_state",target(aid));bool observedManual=false;for(const auto& value:*manual["values"].getArray())if(value["id"].toString()=="global.master_level")observedManual=std::abs(double(value["normalized"])-.47)<1e-6;CHECK(observedManual);CHECK(manual["revision"]!=first["revision"]);
    const auto manualState=ownedState(*a);bridge.tool("apply_edit",object({{"instance_id",aid},{"edit_id",staged["edit_id"]}}),"stale");CHECK(ownedState(*a)==manualState);
    staged=prepare(bridge,aid,.32);juce::AudioProcessorParameter* gain=nullptr;for(auto* parameter:a->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==juce::String(juce::VST3ClientExtensions::convertJuceParameterId("output.gain_db",true)))gain=parameter;
    CHECK(gain);juce::AudioBuffer<float> reentrantAudio(2,128);juce::MidiBuffer reentrantMidi;GestureListener listener;
    listener.action=[&]{gain->setValueNotifyingHost(.42f);a->processBlock(reentrantAudio,reentrantMidi);};
    if(master&&gain){master->addListener(&listener);bridge.tool("apply_edit",object({{"instance_id",aid},{"edit_id",staged["edit_id"]}}),"stale");master->removeListener(&listener);CHECK(listener.fired);
        const auto state=bridge.tool("get_state",target(aid));for(const auto& value:*state["values"].getArray())if(value["id"].toString()=="global.master_level")CHECK(std::abs(double(value["normalized"])-.47)<1e-6);}
    {Stdio outsider;CHECK(outsider.start(helper,root,suffix));CHECK(outsider.initialize());staged=prepare(bridge,aid,.34);const auto otherState=outsider.tool("get_state",target(aid));
        outsider.tool("prepare_edit",object({{"instance_id",aid},{"snapshot",otherState["snapshot"]},{"policy",object({})},{"edits",object({})}}),"busy");
        outsider.tool("apply_edit",object({{"instance_id",aid},{"edit_id",staged["edit_id"]}}),"unauthorized");bridge.close();pump(50);
        const auto replacement=prepare(outsider,aid,.36);outsider.tool("discard_edit",object({{"instance_id",aid},{"edit_id",replacement["edit_id"]}}));CHECK(bridge.start(helper,root,suffix));CHECK(bridge.initialize());outsider.close();}
    const auto snapshot=bridge.tool("get_state",target(aid));const auto job=bridge.tool("render_preview",object({{"instance_id",aid},{"snapshot",snapshot["snapshot"]}}));const auto jobArgs=object({{"instance_id",aid},{"job_id",job["job_id"]}});juce::var ready;
    CHECK(until([&]{ready=bridge.tool("get_job",jobArgs);return ready["job_status"].toString()=="ready"||ready["job_status"].toString()=="failed";},60000));CHECK(ready["job_status"].toString()=="ready");
    if(ready["job_status"].toString()=="ready"){auto resource=bridge.rpc("resources/read",object({{"uri",ready["resource_uri"]}}));CHECK(resource["contents"].size()==1);juce::MemoryBlock wav;juce::MemoryOutputStream stream(wav,false);CHECK(juce::Base64::convertFromBase64(stream,resource["contents"][0]["blob"].toString()));stream.flush();CHECK(wav.getSize()==44+std::size_t(int(ready["frames"]))*8);CHECK(int(ready["frames"])==576000);CHECK(root.getChildFile("actual-preview.wav").replaceWithData(wav.getData(),wav.getSize()));
        a->suspendProcessing(true);pump(300);bridge.tool("play_preview",jobArgs,"audio_unavailable");a->suspendProcessing(false);juce::AudioBuffer<float> block(2,128);juce::MidiBuffer midi;a->processBlock(block,midi);bridge.tool("play_preview",jobArgs);a->processBlock(block,midi);bridge.tool("stop_preview",jobArgs);a->processBlock(block,midi);pump();}
    bridge.tool("cancel_job",jobArgs);staged=prepare(bridge,aid,.32);const auto old=bridge.tool("list_instances",object({}))["instances"];
    const auto restore=envelope(stateA);a->setStateInformation(restore.getData(),int(restore.getSize()));pump(50);bridge.tool("apply_edit",object({{"instance_id",aid},{"edit_id",staged["edit_id"]}}),"stale");
    const auto fresh=bridge.tool("list_instances",object({}))["instances"];CHECK(fresh!=old);CHECK(ownedState(*a)==stateA);
    auto locked=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(stateA.getData(),stateA.getSize(),*locked));locked->sound.controlLocks[0]=true;juce::MemoryBlock lockedBytes;CHECK(encodeProjectState(*locked,lockedBytes));const auto lockedEnvelope=envelope(lockedBytes);a->setStateInformation(lockedEnvelope.getData(),int(lockedEnvelope.getSize()));pump(50);
    auto lockState=bridge.tool("get_state",target(aid));bridge.tool("prepare_edit",object({{"instance_id",aid},{"snapshot",lockState["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.2}})}}})}}),"invalid_input");
    bridge.tool("prepare_edit",object({{"instance_id",aid},{"snapshot",lockState["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","synth.2.analog.filter.cutoff"},{"normalized",.2}})}}})}}),"invalid_input");
    a->setStateInformation(restore.getData(),int(restore.getSize()));pump(50);
    juce::MemoryBlock opaque;a->getStateInformation(opaque);auto duplicate=host.create(stateA);CHECK(duplicate);if(duplicate){duplicate->setStateInformation(opaque.getData(),int(opaque.getSize()));juce::var duplicates;CHECK(until([&]{duplicates=bridge.tool("list_instances",object({}))["instances"];return duplicates.size()==3;}));
        juce::var duplicateId;for(const auto& row:*duplicates.getArray())if(row["instance_id"]!=aid&&row["instance_id"]!=bid)duplicateId=row["instance_id"];CHECK(duplicateId.isString());
        if(duplicateId.isString()){CHECK(ownedState(*duplicate)==stateA);bridge.tool("apply_edit",object({{"instance_id",duplicateId},{"edit_id",staged["edit_id"]}}),"stale");}duplicate->releaseResources();duplicate.reset();CHECK(until([&]{return bridge.tool("list_instances",object({}))["instances"].size()==2;}));}
    // Kill only this fixture's explicitly launched service. Ordinary observed reconnect is separate from pipe poison.
    staged=prepare(bridge,aid,.33);const auto beforeFailure=bridge.tool("list_instances",object({}))["instances"];const auto beforeFailureA=ownedState(*a),beforeFailureB=ownedState(*b);CHECK(service.child.kill());CHECK(service.child.waitForProcessToFinish(5000));CHECK(!service.child.isRunning());
    CHECK(exact(aBefore,audio(*a,host.clock)));CHECK(exact(bBefore,audio(*b,host.clock,36)));CHECK(ownedState(*a)==beforeFailureA&&ownedState(*b)==beforeFailureB);
    juce::var recovered;CHECK(until([&]{const auto result=bridge.rpc("tools/call",object({{"name","list_instances"},{"arguments",object({})}}))["structuredContent"];if(result["status"].toString()!="ready")return false;recovered=result["instances"];return recovered.size()==2&&recovered!=beforeFailure;}));
    bridge.tool("apply_edit",object({{"instance_id",aid},{"edit_id",staged["edit_id"]}}),"stale");CHECK(ownedState(*a)==beforeFailureA&&ownedState(*b)==beforeFailureB);
    a->releaseResources();a.reset();CHECK(until([&]{return bridge.tool("list_instances",object({}))["instances"].size()==1;}));bridge.tool("get_state",target(aid),"instance_gone");CHECK(ownedState(*b)==stateB);
    b->releaseResources();b.reset();CHECK(until([&]{return bridge.tool("list_instances",object({}))["instances"].size()==0;}));
    // A self-contained owned drum bundle is restored/previewed/recalled through the actual host.
    const auto drum=fixture("Task6 owned drum",true);auto d=host.create(drum);if(d){CHECK(ownedState(*d)==drum);CHECK(peak(audio(*d,host.clock,36))>0);d->releaseResources();d.reset();}
    bridge.close();SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;_CrtSetAllocHook(allocationHook);
    if(argc==2&&juce::String(argv[1])=="--stdio-self-test"){stdioClientWorkTest();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    juce::String plugin=RACKSYNTH_MCP_VST3,helper=RACKSYNTH_MCP_HELPER;juce::File root;bool hold=false,capture=false,warnings=false;
    for(int i=1;i<argc;++i){const juce::String a(argv[i]);if(a=="--hold")hold=true;else if(a=="--warnings")warnings=true;else if(a=="--capture-only")capture=true;else if(a=="--output"&&i+1<argc){const juce::String raw(argv[++i]);
#if defined(_DEBUG)
        if(!testing::fullyQualifiedMcpTestPath(raw))return 1;
#endif
        root=juce::File(raw);}else if(a=="--plugin"&&i+1<argc)plugin=argv[++i];}
    if(root==juce::File{}){const auto base=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
#if defined(_DEBUG)
        if(!testing::fullyQualifiedMcpTestPath(base))return 1;
#endif
        root=juce::File(base).getChildFile("task6-workflow-"+juce::Uuid().toString());}
#if defined(_DEBUG)
    // Establish and verify the absolute owned root before even scanning the VST3.
    if(!testing::fullyQualifiedMcpTestPath(root.getFullPathName()) || juce::File(plugin)!=juce::File(RACKSYNTH_MCP_VST3) || !root.createDirectory().wasOk())return 1;
    const auto ownedRoot=root.getChildFile("owned-plugin-"+juce::Uuid().toString());
    if(ownedRoot.exists() || !ownedRoot.createDirectory().wasOk())return 1;
    ScopedMcpRackRoot diagnostic(ownedRoot);if(!diagnostic.installed || juce::SystemStats::getEnvironmentVariable("AIRACK_MCP_TEST_RACK_ROOT",{})!=ownedRoot.getFullPathName())return 1;
    const auto ownedMarker=installOwnedRackMarker(ownedRoot);if(!ownedMarker)return 1;
    const OwnedRackMarker* marker=&*ownedMarker;
#else
    CHECK(root.createDirectory().wasOk());
    const OwnedRackMarker* marker=nullptr;
#endif
    const auto suffix=juce::SystemStats::getEnvironmentVariable("AIRACK_MCP_PIPE_SUFFIX",juce::Uuid().toString().removeCharacters("-"));
    try{if(capture){if(marker&&!captureIsolationPreflight(juce::File(plugin),juce::File(helper),root,suffix,*marker)){CHECK(false);return 1;}captureProduct(juce::File(plugin),root);}else if(warnings)warningProjection(juce::File(plugin),juce::File(helper),root,suffix,marker);else{stdioClientWorkTest();workflow(juce::File(plugin),juce::File(helper),root,suffix,hold,marker);}}catch(const std::exception& e){std::cout<<"EXCEPTION "<<e.what()<<std::endl;CHECK(false);}
    CHECK(callbackAllocations==0&&callbackFrees==0);std::cout<<"actual VST3 callback allocations="<<callbackAllocations<<" frees="<<callbackFrees<<std::endl;
    std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;
}
