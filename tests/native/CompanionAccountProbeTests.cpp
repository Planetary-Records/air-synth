#include "FrozenRackExamples.h"
#include "CompanionRuntimeServer.h"
#include "CompanionLaunch.h"
#include "AiCatalog.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "StrictJson.h"
#include "UserReach.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace airack;
using namespace airack::companion;
namespace
{
int checks=0,failures=0,groups=0,priorChecks=0,priorFailures=0;
const char* group="";
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<group<<':'<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void finish(){if(groups)std::cout<<group<<" checks="<<checks-priorChecks<<" failures="<<failures-priorFailures<<std::endl;}
void begin(const char* name){finish();group=name;++groups;priorChecks=checks;priorFailures=failures;std::cout<<name<<std::endl;}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields){auto* o=new juce::DynamicObject;for(const auto& p:fields)o->setProperty(p.first,p.second);return o;}
juce::String json(const juce::var& v){return juce::JSON::toString(v,true,17);}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::String suffix(){return juce::Uuid().toString().removeCharacters("-");}
template<class F> bool observe(F f,std::uint64_t ms=5000){const auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
const juce::File host=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures");
juce::File fresh(const char* prefix){return host.getChildFile(juce::String(prefix)+" "+suffix()+juce::String::fromUTF8(" 音 ü"));}
struct Temp{juce::File root=fresh("account-probe-fixture");Temp(){if(root.getParentDirectory()!=host || root.createDirectory().failed())throw std::runtime_error("fixture_setup");}};
struct Handle{HANDLE value=nullptr;~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle()=default;explicit Handle(HANDLE h):value(h){}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
std::uint64_t creation(HANDLE p){FILETIME a{},b{},c{},d{};return GetProcessTimes(p,&a,&b,&c,&d)?(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime:0;}
std::wstring quote(std::wstring_view text){std::wstring out=L"\"";unsigned slashes=0;for(auto c:text){if(c==L'\\'){++slashes;continue;}if(c==L'"')out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');out+=L'"';return out;}
struct ProcessResult{bool launched=false,timedOut=false;DWORD exit=999,totalProcessesBeforeResume=0,totalProcessesBeforeCleanup=0,companionMembers=0;std::string out,err;juce::var reply;};
juce::File logs;
int commandId=0;
juce::String acceptedHelperHash;
ProcessResult run(const juce::File& exe,const std::vector<juce::String>& args,bool fixtureTree=true,const std::function<void()>& whileRunning={})
{
    ProcessResult result;const auto log=logs.getChildFile(juce::String(++commandId));CHECK(log.createDirectory().wasOk());
    const auto utc=juce::Time::getCurrentTime().toISO8601(true);const auto before=juce::SHA256(exe).toHexString();
    std::wstring command=quote(exe.getFullPathName().toWideCharPointer());juce::Array<juce::var> argv;argv.add(exe.getFullPathName());for(const auto& arg:args){command+=L" "+quote(arg.toWideCharPointer());argv.add(arg);}
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle readOut,writeOut,readErr,writeErr;
    CHECK(CreatePipe(&readOut.value,&writeOut.value,&security,0)!=FALSE);CHECK(CreatePipe(&readErr.value,&writeErr.value,&security,0)!=FALSE);
    CHECK(SetHandleInformation(readOut.value,HANDLE_FLAG_INHERIT,0)!=FALSE);CHECK(SetHandleInformation(readErr.value,HANDLE_FLAG_INHERIT,0)!=FALSE);
    Handle job(CreateJobObjectW(nullptr,nullptr));Handle completion(CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1));
    CHECK(completion.value!=nullptr);JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};association.CompletionKey=job.value;association.CompletionPort=completion.value;CHECK(SetInformationJobObject(job.value,JobObjectAssociateCompletionPortInformation,&association,sizeof(association))!=FALSE);
    // Notifications supply positive identities only. Every lifetime association
    // must be accounted for below; missing notifications or identities fail.
    juce::Array<juce::var> memberRecords,jobEvents;std::vector<std::pair<DWORD,std::uint64_t>> memberKeys;std::vector<std::unique_ptr<Handle>> queriedMembers;bool identityComplete=true;DWORD returnedProbePid=0;std::uint64_t returnedProbeBorn=0;HANDLE returnedProbeHandle=nullptr;
    wchar_t systemPath[32768]{};const auto systemLength=GetSystemDirectoryW(systemPath,32768);CHECK(systemLength>0 && systemLength<32768);const auto consoleHost=juce::File(juce::String(systemPath)).getChildFile("conhost.exe");const auto expectedHelper=exe.getSiblingFile(companionExecutableName);
    auto drainEvents=[&](const char* phase){DWORD message=0;ULONG_PTR key=0;LPOVERLAPPED identity=nullptr;while(GetQueuedCompletionStatus(completion.value,&message,&key,&identity,0)){
        const auto pid=DWORD(reinterpret_cast<ULONG_PTR>(identity));const bool ownKey=key==reinterpret_cast<ULONG_PTR>(job.value);auto event=object({{"message",int(message)},{"processId",juce::int64(pid)},{"ownedKeyMatches",ownKey},{"phase",phase},{"observedUtc",juce::Time::getCurrentTime().toISO8601(true)}});if(!ownKey)identityComplete=false;
        if(message==JOB_OBJECT_MSG_NEW_PROCESS && ownKey){juce::String failure;DWORD error=0;HANDLE query=nullptr;
            if(memberRecords.size()>=16)failure="member_limit";else if(pid==returnedProbePid)query=returnedProbeHandle;else{auto handle=std::make_unique<Handle>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));query=handle->value;if(!query){failure="query_open_failed";error=GetLastError();}queriedMembers.push_back(std::move(handle));}
            BOOL member=FALSE;if(failure.isEmpty() && (!query || !IsProcessInJob(query,job.value,&member) || !member)){failure="owned_job_unverified";error=GetLastError();}const auto born=failure.isEmpty()?creation(query):0;if(failure.isEmpty() && (!born || !returnedProbeBorn || born<returnedProbeBorn))failure="incarnation_unverified";
            std::vector<wchar_t> image(32768);DWORD length=DWORD(image.size());if(failure.isEmpty() && !QueryFullProcessImageNameW(query,0,image.data(),&length)){failure="image_query_failed";error=GetLastError();}
            juce::String classification,sha;const juce::File loaded(failure.isEmpty()?juce::String(image.data(),int(length)):juce::String{});if(failure.isEmpty()){
                const auto incarnation=std::make_pair(pid,born);if(std::find(memberKeys.begin(),memberKeys.end(),incarnation)!=memberKeys.end())failure="duplicate_incarnation";else memberKeys.push_back(incarnation);
                if(pid==returnedProbePid && born==returnedProbeBorn && loaded==exe)classification="returnedProbe";else if(loaded==consoleHost)classification="systemConsoleHost";else if(loaded==expectedHelper){classification="companion";sha=juce::SHA256(loaded).toHexString();if(sha!=acceptedHelperHash)failure="helper_image_changed";++result.companionMembers;}else{classification="unexpected";failure="unexpected_image";}
            }
            if(failure.isNotEmpty())identityComplete=false;auto record=object({{"processId",juce::int64(pid)},{"creationTime",juce::String(born)},{"imagePath",failure=="query_open_failed" || failure=="owned_job_unverified" || failure=="incarnation_unverified" || failure=="image_query_failed" || failure=="member_limit"?juce::String{}:loaded.getFullPathName()},{"classification",classification},{"sha256",sha},{"identityFailure",failure},{"queryError",int(error)},{"observedUtc",juce::Time::getCurrentTime().toISO8601(true)}});memberRecords.add(record);
        }jobEvents.add(event);
    }if(GetLastError()!=WAIT_TIMEOUT)identityComplete=false;};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    CHECK(job.value && SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;startup.hStdOutput=writeOut.value;startup.hStdError=writeErr.value;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);
    result.launched=CreateProcessW(exe.getFullPathName().toWideCharPointer(),mutableCommand.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|DETACHED_PROCESS,nullptr,juce::String(RACKSYNTH_WORKSPACE).toWideCharPointer(),&startup,&pi)!=FALSE;
    CHECK(result.launched);Handle process(pi.hProcess),thread(pi.hThread);const auto born=result.launched?creation(process.value):0;returnedProbeBorn=born;returnedProbePid=pi.dwProcessId;returnedProbeHandle=process.value;
    if(result.launched){const bool assigned=AssignProcessToJobObject(job.value,process.value)!=FALSE;CHECK(assigned);if(assigned){JOBOBJECT_BASIC_ACCOUNTING_INFORMATION suspended{};CHECK(QueryInformationJobObject(job.value,JobObjectBasicAccountingInformation,&suspended,sizeof(suspended),nullptr)!=FALSE);result.totalProcessesBeforeResume=suspended.TotalProcesses;CHECK(result.totalProcessesBeforeResume>0);drainEvents("beforeResume");CHECK(ResumeThread(thread.value)!=DWORD(-1));}else CHECK(TerminateProcess(process.value,98)!=FALSE);}
    CloseHandle(writeOut.value);writeOut.value=nullptr;CloseHandle(writeErr.value);writeErr.value=nullptr;
    auto drain=[](HANDLE pipe,std::string& text){DWORD available=0;while(PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr) && available){char buffer[4096];DWORD n=0;if(!ReadFile(pipe,buffer,std::min<DWORD>(available,sizeof(buffer)),&n,nullptr) || !n)break;text.append(buffer,n);}};
    const auto end=GetTickCount64()+25000;
    if(result.launched){while(WaitForSingleObject(process.value,0)==WAIT_TIMEOUT && GetTickCount64()<end){drain(readOut.value,result.out);drain(readErr.value,result.err);drainEvents("whileRunning");if(whileRunning)whileRunning();Sleep(1);}result.timedOut=WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;if(result.timedOut){std::cout<<"  command "<<commandId<<' '<<(args.empty()?juce::String{}:args.front())<<" still running at its 25 s deadline"<<std::endl;CHECK(TerminateJobObject(job.value,98)!=FALSE);}CHECK(WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0);CHECK(GetExitCodeProcess(process.value,&result.exit)!=FALSE);}
    drain(readOut.value,result.out);drain(readErr.value,result.err);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION beforeCleanup{};CHECK(QueryInformationJobObject(job.value,JobObjectBasicAccountingInformation,&beforeCleanup,sizeof(beforeCleanup),nullptr)!=FALSE);result.totalProcessesBeforeCleanup=beforeCleanup.TotalProcesses;drainEvents("beforeCleanup");CHECK(identityComplete);CHECK(memberKeys.size()==result.totalProcessesBeforeCleanup);CHECK(memberRecords.size()==int(memberKeys.size()));
    // Only this suspended, test-created probe and its fixture-only descendants
    // enter the owned job. No PID adoption or production helper is possible.
    if(fixtureTree)CHECK(TerminateJobObject(job.value,97)!=FALSE);
    CHECK(observe([&]{JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};return QueryInformationJobObject(job.value,JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr) && info.ActiveProcesses==0;}));
    CHECK(!result.timedOut);CHECK(juce::SHA256(exe).toHexString()==before);
    CHECK(log.getChildFile("stdout.txt").replaceWithData(result.out.data(),result.out.size()));CHECK(log.getChildFile("stderr.txt").replaceWithData(result.err.data(),result.err.size()));
    const auto record=object({{"argv",argv},{"cwd",RACKSYNTH_WORKSPACE},{"startUtc",utc},{"endUtc",juce::Time::getCurrentTime().toISO8601(true)},{"launched",result.launched},{"exit",int(result.exit)},{"pid",int(pi.dwProcessId)},{"creationTime",juce::String(born)},{"sha256Before",before},{"sha256After",juce::SHA256(exe).toHexString()},{"timedOut",result.timedOut},{"ownedFixtureJob",true},{"memberIdentityComplete",identityComplete},{"boundUniqueMembers",int(memberKeys.size())},{"companionMembers",int(result.companionMembers)},{"members",memberRecords},{"jobEvents",jobEvents},{"totalProcessesBeforeResume",int(result.totalProcessesBeforeResume)},{"totalProcessesBeforeCleanup",int(result.totalProcessesBeforeCleanup)}});
    CHECK(log.getChildFile("command.json").replaceWithText(json(record)));
    if(!result.out.empty())juce::JSON::parse(juce::String::fromUTF8(result.out.data(),static_cast<int>(result.out.size())),result.reply);
    if(result.reply.isObject() && bool(result.reply["helper"]["owned"])){CHECK(result.companionMembers==1);for(const auto& member:memberRecords)if(member["classification"].toString()=="companion"){CHECK(juce::int64(member["processId"])==juce::int64(result.reply["helper"]["processId"]));CHECK(member["creationTime"].toString()==result.reply["helper"]["creationTime"].toString());CHECK(member["sha256"].toString()==result.reply["helper"]["sha256"].toString());}}else CHECK(result.companionMembers==0);
    return result;
}
std::vector<juce::String> fixtureArgs(const Temp& t,const juce::String& name,std::vector<juce::String> a){a.insert(a.end(),{"--fixture-root",t.root.getFullPathName(),"--pipe-suffix",name});return a;}
juce::String snapshot(const juce::File& root){juce::Array<juce::var> result;auto files=root.findChildFiles(juce::File::findFilesAndDirectories,true);files.sort();for(const auto& f:files)result.add(object({{"path",f.getRelativePathFrom(root)},{"directory",f.isDirectory()},{"sha256",f.existsAsFile()?juce::SHA256(f).toHexString():juce::String{}}}));return json(result);}
bool junction(const juce::File& link,const juce::File& target)
{
    if(link.createDirectory().failed())return false;
    Handle handle(CreateFileW(link.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(handle.value==INVALID_HANDLE_VALUE)return false;
    const std::wstring substitute=L"\\??\\"+std::wstring(target.getFullPathName().toWideCharPointer());const std::wstring print=target.getFullPathName().toWideCharPointer();
    struct Mount {DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;wchar_t paths[16376];} data{};
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.subLength=WORD(substitute.size()*2);data.printOffset=WORD((substitute.size()+1)*2);data.printLength=WORD(print.size()*2);
    std::copy(substitute.begin(),substitute.end(),data.paths);std::copy(print.begin(),print.end(),data.paths+substitute.size()+1);data.length=WORD(8+data.printOffset+data.printLength+2);DWORD returned=0;
    return DeviceIoControl(handle.value,FSCTL_SET_REPARSE_POINT,&data,DWORD(data.length+8),nullptr,0,&returned,nullptr)!=FALSE;
}
void publicReply(const ProcessResult& p,const char* action,bool generated=false)
{
    CHECK(p.err.empty());CHECK(p.out.size()<=65536);CHECK(p.reply.isObject());CHECK(int(p.reply["version"])==1);CHECK(p.reply["action"].toString()==action);CHECK(bool(p.reply["synthetic"]));
    for(const auto* key:{"runtime","session","runtimeStatus","providerStatus","httpStatus","errorCode","selectedModel","modelVerified","peer","helper"})CHECK(p.reply.hasProperty(key));
    CHECK(!juce::Uuid(p.reply["runtime"].toString()).isNull());CHECK(!juce::Uuid(p.reply["session"].toString()).isNull());
    for(const auto* secret:{"subject","email","token","access_token","refresh_token","browserUrl","authUrl","headers","providerBody"})CHECK(!p.reply.hasProperty(secret));
    CHECK(p.out.find("PRIVATE_SENTINEL")==std::string::npos);
    if(generated)for(const auto* key:{"generationStatus","proposalStatus","renderStatus","inferenceCalls"})CHECK(p.reply.hasProperty(key));else CHECK(p.reply["models"].isArray());
}
struct Captures
{
    std::mutex mutex;std::vector<RuntimeJob> jobs;std::vector<juce::var> prompts,schemas;std::optional<ai::GenerationRequest> request;std::optional<ai::GenerationResult> result,selection;
    rack::oauth::Status accountStatus=rack::oauth::Status::connected;int mode=0;std::atomic_bool heldEntered{false},releaseHeld{false};
    Frame run(const RuntimeJob& job)
    {
        {std::lock_guard lock(mutex);jobs.push_back(job);}
        if(const auto* r=std::get_if<ai::GenerationRequest>(&job.input)){
            if(mode==3){heldEntered.store(true);if(!observe([&]{return releaseHeld.load() || (job.cancellation && job.cancellation->cancelled());},5000))throw std::runtime_error("owned_proof_fixture_timeout");}
            if(mode==1){Frame f{MessageKind::failure,job.session,{}};if(!encodeControl({r->tag,RuntimeStatus::modelNotConfigured},f.payload))throw std::runtime_error("fixture_control");return f;}
            const auto generated=ai::generateProposal(*r,[this,r](const juce::String&,std::string_view prompt,const juce::var& schema){
                juce::var parsed;if(!ai::parseStrictJson(prompt,parsed,1024*1024))throw std::runtime_error("fixture_prompt");{std::lock_guard lock(mutex);prompts.push_back(parsed);schemas.push_back(schema);}
                rack::oauth::StructuredResult response;response.result.status=mode==2?rack::oauth::Status::denied:rack::oauth::Status::connected;response.result.httpStatus=mode==2?403:200;response.result.errorCode=mode==2?"access_denied":"";
                response.text=R"({"version":1,"parameter_edits":[{"id":"global.master_level","normalized":0.4}],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"pluck","explanation":"synthetic fixture"})";
                if(r->catalog){auto proposal=juce::JSON::parse(response.text);auto* o=proposal.getDynamicObject();o->setProperty("version",2);o->setProperty("name","Plucked Test Keys");
                    o->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>{}},{"remove",juce::Array<juce::var>{}},{"replace",juce::Array<juce::var>{}},{"controls",juce::Array<juce::var>{}}}));
                    o->setProperty("catalog_selections",juce::Array<juce::var>{});o->setProperty("catalog_parameter_edits",juce::Array<juce::var>{});response.text=json(proposal);}
                if(mode==4){juce::Array<juce::var> selected,tweaks;const auto inventory=parsed["catalog"]["rack_catalog"]["entries"];const auto* entries=inventory.getArray();if(!entries)throw std::runtime_error("fixture_inventory");
                    for(const auto* wanted:{"Thread Press","Lumen Drift"})for(const auto& entry:*entries){const auto name=entry["name"].toString();if(name!=wanted)continue;const bool custom=name=="Thread Press";const int local=custom?19:201;
                        selected.add(object({{"local_id",local},{"design",juce::Uuid(entry["id"].toString()).toDashedString()},{"revision",entry["revision"]},{"content_sha256",entry["content_sha256"]},{"destination",object({{"role","fx"},{"chain",4},{"position",custom?0:2}})}}));
                        tweaks.add(object({{"selection",local},{"unit",custom?1:0},{"key",custom?"drive.mix":"reverb.mix"},{"normalized",custom?.18:.11}}));}
                    if(selected.size()!=2)throw std::runtime_error("fixture_complete_inventory");
                    auto proposal=juce::JSON::parse(response.text);auto* o=proposal.getDynamicObject();o->setProperty("version",2);o->setProperty("name","Plucked Test Keys");
                    o->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>{}},{"remove",juce::Array<juce::var>{}},{"replace",juce::Array<juce::var>{}},{"controls",juce::Array<juce::var>{}}}));
                    o->setProperty("catalog_selections",selected);o->setProperty("catalog_parameter_edits",tweaks);response.text=json(proposal);}
                const auto responseUtf8=response.text.toStdString();ai::ProposalEdits checked;const bool valid=r->catalog?ai::decodeProposal(responseUtf8,r->policy,*r->catalog->authority,checked):ai::decodeProposal(responseUtf8,r->policy,checked);
                if(!valid)throw std::runtime_error("fixture_proposal_preflight");
                return response;
            },[token=job.cancellation]{return token && token->cancelled();});
            {std::lock_guard lock(mutex);request=*r;result=generated;if(generated.status==ai::GenerationStatus::catalogSelectionRequired)selection=generated;}
            Frame f{MessageKind::result,job.session,{}};if(!encodeResult(*r,generated,f.payload))throw std::runtime_error("fixture_result");return f;
        }
        const auto& command=std::get<AccountCommand>(job.input);AccountReply reply;reply.providerStatus=accountStatus;reply.httpStatus=accountStatus==rack::oauth::Status::connected?200:accountStatus==rack::oauth::Status::denied?403:0;
        reply.errorCode=accountStatus==rack::oauth::Status::denied?"access_denied":accountStatus==rack::oauth::Status::accessExpired?"access_expired":"";
        if(accountStatus==rack::oauth::Status::connected){reply.selectedModel=command.action==AccountAction::selectModel?command.model:"synthetic-model:1";reply.modelVerified=true;reply.models={{"synthetic-model:1","Synthetic model"},{"other_model","Other model"}};}
        Frame f{MessageKind::accountResult,job.session,{}};if(!encodeAccountReply(reply,f.payload))throw std::runtime_error("fixture_account");return f;
    }
};
struct Service
{
    std::shared_ptr<Captures> captures=std::make_shared<Captures>();std::atomic_bool stop{false};juce::String name=suffix();RuntimeServer server;std::thread io;
    explicit Service(int mode=0,rack::oauth::Status status=rack::oauth::Status::connected):server([c=captures](const auto& job){return c->run(job);},[](const auto&){}),io(){captures->mode=mode;captures->accountStatus=status;io=std::thread([this]{auto r=server.run(stop,name);if(r.workerRetained)std::terminate();});CHECK(observe([&]{return WaitNamedPipeW(currentUserPipeName(name).toWideCharPointer(),1)!=FALSE;}));}
    ~Service(){stop.store(true);if(io.joinable())io.join();CHECK(airack::companion::detail::RuntimeServerTestAccess::retainedWorkers()==0);}
};
void help_and_argument_isolation(const juce::File& probe)
{
    begin(__func__); // Removing preflight validation causes native launches/output/store effects here.
    Temp t;const auto instruction=t.root.getChildFile("prompt.txt");CHECK(instruction.replaceWithText("a pluck stab in the style of artbat"));const auto output=fresh("rejected-output");
    const auto invalidUtf8=t.root.getChildFile("invalid.txt");const char broken[]={char(0xc0),char(0xaf)};CHECK(invalidUtf8.replaceWithData(broken,2));
    const auto nul=t.root.getChildFile("embedded-nul-request.txt");const char zero[]={'a',0,'b'};CHECK(nul.replaceWithData(zero,3));
    const auto empty=t.root.getChildFile("empty.txt");CHECK(empty.replaceWithText(""));const auto large=t.root.getChildFile("large.txt");CHECK(large.replaceWithText(juce::String::repeatedString("x",8193)));
    const auto reparse=fresh("owned-junction");CHECK(junction(reparse,t.root));CHECK((GetFileAttributesW(reparse.getFullPathName().toWideCharPointer())&FILE_ATTRIBUTE_REPARSE_POINT)!=0);
    const auto helperObservation=t.root.getChildFile("launch-observation.json");const auto original=snapshot(t.root);
    auto generate=[&](juce::String input,juce::String destination){return std::vector<juce::String>{"--generate","--model","synthetic-model:1","--instruction-file",input,"--output",destination};};
    std::vector<std::vector<juce::String>> invalid={{},{"--unknown"},{"--help","--status"},{"--status","--models"},{"--status","--status"},{"--select-model"},{"--select-model","bad/model"},{"--select-model",""},{"--select-model",juce::String::repeatedString("a",257)},{"--status","--model","a"},{"--generate"},generate("relative.txt",output.getFullPathName()),generate(instruction.getFullPathName(),"rejected-relative "+suffix()),generate(instruction.getFullPathName(),t.root.getFullPathName()),generate(instruction.getFullPathName(),host.getFullPathName()),generate(instruction.getFullPathName(),juce::File(RACKSYNTH_WORKSPACE).getChildFile("rejected-outside "+suffix()).getFullPathName()),generate(juce::File(RACKSYNTH_WORKSPACE).getChildFile("AGENTS.md").getFullPathName(),output.getFullPathName()),generate(invalidUtf8.getFullPathName(),output.getFullPathName()),generate(nul.getFullPathName(),output.getFullPathName()),generate(empty.getFullPathName(),output.getFullPathName()),generate(large.getFullPathName(),output.getFullPathName()),generate(reparse.getChildFile("prompt.txt").getFullPathName(),output.getFullPathName()),generate(instruction.getFullPathName(),reparse.getFullPathName()),generate(instruction.getFullPathName(),host.getChildFile("rejected-nested "+suffix()+"/missing").getFullPathName()),{"--status","--fixture-root",t.root.getFullPathName()},{"--status","--pipe-suffix","only"},{"--status","--fixture-root","relative","--pipe-suffix","valid"},{"--status","--fixture-root",t.root.getFullPathName(),"--pipe-suffix","bad/suffix"},{"--status","--fixture-root",t.root.getFullPathName(),"--pipe-suffix",juce::String::repeatedString("s",33)},{"--status","--fixture-root",t.root.getFullPathName(),"--pipe-suffix",juce::String::fromUTF8("音")}};
    auto duplicate=generate(instruction.getFullPathName(),output.getFullPathName());duplicate.insert(duplicate.end(),{"--model","synthetic-model:1"});invalid.push_back(duplicate);
    invalid.push_back({"--status","--fixture-root",t.root.getFullPathName(),"--pipe-suffix","valid","--fixture-root",t.root.getFullPathName()});invalid.push_back({"--status","--fixture-root",t.root.getFullPathName(),"--pipe-suffix","valid","--pipe-suffix","valid"});
    auto destinations=[&](const std::vector<juce::String>& a){juce::Array<juce::var> records;auto record=[&](const juce::File& file){const auto attributes=GetFileAttributesW(file.getFullPathName().toWideCharPointer());records.add(object({{"path",file.getFullPathName()},{"attributes",juce::int64(attributes)},{"sha256",file.existsAsFile()?juce::SHA256(file).toHexString():juce::String{}}}));};
        for(std::size_t i=0;i+1<a.size();++i)if(a[i]=="--output"){const auto actual=juce::File(RACKSYNTH_WORKSPACE).getChildFile(a[i+1]);record(actual);record(actual.getParentDirectory());if(actual==host)for(const auto* f:{"source.airackproject","catalog.json","schema.json","instruction.txt","request.json","candidate.airackproject","result.bin","preview.wav","preview-left.f32","preview-right.f32","proof.json"})record(actual.getChildFile(f));}return json(records);};
    Service service;const auto help=run(probe,{"--help"});CHECK(help.exit==0);CHECK(help.err.empty());CHECK(help.companionMembers==0);for(const auto* flag:{"--status","--models","--select-model","--sign-in","--generate","--instruction-file","--brief","--output"})CHECK(help.out.find(flag)!=std::string::npos);
    for(auto a:invalid){const bool hasFixture=std::find(a.begin(),a.end(),"--fixture-root")!=a.end() || std::find(a.begin(),a.end(),"--pipe-suffix")!=a.end();if(!hasFixture){const auto pair=fixtureArgs(t,service.name,{});a.insert(a.begin(),pair.begin(),pair.end());}const auto beforeDestinations=destinations(a);const auto result=run(probe,a);CHECK(result.exit==2);CHECK(result.companionMembers==0);CHECK(bool(result.reply["synthetic"])==!hasFixture);CHECK(!bool(result.reply["success"]));CHECK(destinations(a)==beforeDestinations);CHECK(snapshot(t.root)==original);CHECK(!helperObservation.exists());CHECK(!t.root.getChildFile("OAuthProbe").exists());CHECK(!t.root.getChildFile("CompanionPreferences").exists());CHECK(!output.exists());std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.empty());}
    CHECK(snapshot(t.root)==original);CHECK(RemoveDirectoryW(reparse.getFullPathName().toWideCharPointer())!=FALSE);CHECK(instruction.existsAsFile());
}
void public_account_cli(const juce::File& probe,const juce::File& helper)
{
    begin(__func__); // Wrong action dispatch, forged success or leaking private fields breaks this group.
    {Temp t;auto p=run(probe,fixtureArgs(t,suffix(),{"--status"}));CHECK(p.exit==1);publicReply(p,"status");CHECK(int(p.reply["providerStatus"])==int(rack::oauth::Status::signedOut));CHECK(bool(p.reply["helper"]["owned"]));CHECK(bool(p.reply["helper"]["peerMatches"]));CHECK(p.reply["helper"]["imagePath"].toString()==probe.getSiblingFile(companionExecutableName).getFullPathName());CHECK(p.reply["helper"]["sha256"].toString()==juce::SHA256(helper).toHexString());const auto v=juce::JSON::parse(t.root.getChildFile("launch-observation.json").loadFileAsString());CHECK(int(v["transportCalls"])==0 && int(v["browserCalls"])==0);CHECK(!bool(v["consoleWindow"]));CHECK(v["creationTime"].toString()==p.reply["helper"]["creationTime"].toString());}
    {Temp t;auto p=run(probe,fixtureArgs(t,suffix(),{"--select-model","synthetic-model:1"}));CHECK(p.exit==1);publicReply(p,"selectModel");CHECK(p.reply["selectedModel"].toString().isEmpty());CHECK(int(p.reply["providerStatus"])==int(rack::oauth::Status::signedOut));CHECK(!bool(p.reply["modelVerified"]));CHECK(t.root.getChildFile("CompanionPreferences").isDirectory());}
    {Temp t;auto p=run(probe,fixtureArgs(t,suffix(),{"--models"}));CHECK(p.exit==1);publicReply(p,"models");CHECK(int(p.reply["providerStatus"])==int(rack::oauth::Status::signedOut));}
    for(const auto& action:std::vector<std::pair<std::vector<juce::String>,const char*>>{{{"--status"},"status"},{{"--models"},"models"},{{"--select-model","other_model"},"selectModel"}}){Service service;Temp t;auto p=run(probe,fixtureArgs(t,service.name,action.first));CHECK(p.exit==0);publicReply(p,action.second);CHECK(int(p.reply["providerStatus"])==int(rack::oauth::Status::connected));CHECK(int(p.reply["httpStatus"])==200);CHECK(bool(p.reply["modelVerified"]));CHECK(p.reply["models"].getArray() && p.reply["models"].getArray()->size()==2);CHECK(!bool(p.reply["helper"]["owned"]));CHECK(p.reply["helper"]["imagePath"].toString().isEmpty());std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.size()==1);if(!service.captures->jobs.empty()){const auto& c=std::get<AccountCommand>(service.captures->jobs[0].input);CHECK(int(c.action)==(action.first[0]=="--status"?0:action.first[0]=="--models"?1:2));CHECK(c.model==(c.action==AccountAction::selectModel?juce::String("other_model"):juce::String{}));}}
    for(auto status:{rack::oauth::Status::signedOut,rack::oauth::Status::accessExpired,rack::oauth::Status::denied,rack::oauth::Status::inferenceNotGranted,rack::oauth::Status::cancelled}){Service service(0,status);Temp t;const bool cancel=status==rack::oauth::Status::cancelled;auto p=run(probe,fixtureArgs(t,service.name,{cancel?"--sign-in":"--status"}));CHECK(p.exit==DWORD(cancel?130:1));publicReply(p,cancel?"signIn":"status");CHECK(int(p.reply["providerStatus"])==int(status));CHECK(!bool(p.reply["modelVerified"]));CHECK(p.reply["selectedModel"].toString().isEmpty());if(status==rack::oauth::Status::denied)CHECK(p.reply["errorCode"].toString()=="access_denied");if(status==rack::oauth::Status::accessExpired)CHECK(p.reply["errorCode"].toString()=="access_expired");std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.size()==1);}
}
struct CorruptService
{
    juce::String name=suffix();PipeListener listener;std::atomic_bool stop{false};std::thread io;int mode;
    explicit CorruptService(int m):mode(m){CHECK(listener.open(name)==PipeStatus::ready);io=std::thread([this]{PipeConnection p;auto wait=[&](auto f){return observe([&]{return stop.load() || f();},20000) && !stop.load();};if(!wait([&]{return listener.pollAccept(p)==PipeStatus::complete;}))return;auto read=[&](Frame& f){if(p.beginRead()!=PipeStatus::ready)return false;PipeStatus s=PipeStatus::pending;return wait([&]{s=p.pollRead(f);return s!=PipeStatus::pending;}) && s==PipeStatus::complete;};auto write=[&](const Frame& f){if(p.beginWrite(f)!=PipeStatus::ready)return false;PipeStatus s=PipeStatus::pending;return wait([&]{s=p.pollWrite();return s!=PipeStatus::pending;}) && s==PipeStatus::complete;};Frame f;ControlMessage hello;if(!read(f) || !decodeControl(bytes(f.payload),hello))return;const juce::Uuid session;Frame welcome{MessageKind::welcome,session,{}};if(!encodeControl({hello.tag,RuntimeStatus::ready},welcome.payload) || !write(welcome) || !read(f))return;ai::GenerationRequest request;if(!decodeRequest(bytes(f.payload),request))return;auto captures=std::make_shared<Captures>();RuntimeJob job;job.input=request;job.session=session;f=captures->run(job);if(mode==0)static_cast<unsigned char*>(f.payload.getData())[0]^=0x80;else {auto* data=static_cast<unsigned char*>(f.payload.getData());const auto n=f.payload.getSize();data[n-4]=1;data[n-3]=0;data[n-2]=0xc0;data[n-1]=0x7f;}write(f);wait([&]{return !p.connected();});});}
    ~CorruptService(){stop.store(true);if(io.joinable())io.join();}
};
void failedArtifacts(const juce::File& output)
{
    CHECK(output.isDirectory());const auto proof=juce::JSON::parse(output.getChildFile("proof.json").loadFileAsString());CHECK(proof.isObject());CHECK(!bool(proof["success"]));CHECK(bool(proof["synthetic"]));for(const auto* f:{"source.airackproject","catalog.json","schema.json","instruction.txt","request.json"})CHECK(output.getChildFile(f).existsAsFile());for(const auto* f:{"candidate.airackproject","preview.wav","preview-left.f32","preview-right.f32","result.bin"})CHECK(!output.getChildFile(f).exists());
}
void generation_and_artifact_integrity(const juce::File& probe)
{
    begin(__func__); // Missing full catalog, admission/readback or altered raw samples fails the complete physical oracle.
    Temp t;const auto instruction=t.root.getChildFile("prompt.txt");const juce::String original="a pluck stab in the style of artbat";CHECK(instruction.replaceWithText(original));
    auto args=[&](const juce::String& name,const juce::File& output){return fixtureArgs(t,name,{"--generate","--model","synthetic-model:1","--instruction-file",instruction.getFullPathName(),"--output",output.getFullPathName()});};
    for(const auto& text:{original,juce::String::fromUTF8("a pluck stab in the style of artbat · 音 🎹")}){CHECK(instruction.replaceWithText(text));Service service;const auto output=fresh("account-probe-ready");std::cout<<"retainedSyntheticMusic="<<output.getFullPathName()<<std::endl;auto p=run(probe,args(service.name,output));CHECK(p.exit==0);publicReply(p,"generate",true);CHECK(int(p.reply["generationStatus"])==int(ai::GenerationStatus::ready));CHECK(int(p.reply["proposalStatus"])==int(ai::ProposalStatus::accepted));CHECK(int(p.reply["renderStatus"])==int(ai::RenderStatus::ready));CHECK(int(p.reply["inferenceCalls"])==1);CHECK(int(p.reply["providerStatus"])==int(rack::oauth::Status::connected));CHECK(int(p.reply["httpStatus"])==200);
      std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.size()==1);CHECK(service.captures->request && service.captures->result);if(service.captures->request && service.captures->result){const auto& r=*service.captures->request;const auto& result=*service.captures->result;CHECK(r.model=="synthetic-model:1");CHECK(r.instruction==text.toStdString());CHECK(r.role=="lead" && r.key=="A" && r.scale=="minor");CHECK(r.capturedBpm==123 && !r.capturedPhrase);CHECK(r.tag.runtime==juce::Uuid(p.reply["runtime"].toString()) && r.tag.requestId==1 && r.tag.editRevision==0 && r.tag.contextRevision==0);CHECK(r.policy.scope==ai::RequestScope::sound && !r.policy.allowMacroChanges && !r.policy.allowSlotReuse);CHECK(r.source==ProjectDocument{});
        CHECK(r.catalog&&r.catalog->authority&&r.catalog->root==r.tag&&r.catalog->authority->root==r.tag);
        CHECK(service.captures->prompts.size()==1 && service.captures->schemas.size()==1);if(!service.captures->prompts.empty()){const auto& payload=service.captures->prompts[0];CHECK(payload["catalog"]["controls"].getArray() && payload["catalog"]["controls"].getArray()->size()==int(userReach(r.source.sound).count()));CHECK(int(payload["catalog"]["limits"]["parameters"])==int(parameterCount));CHECK(payload["instruction"].toString()==text);CHECK(payload["context"]["phrase_source"].toString()=="default_notes");CHECK(double(payload["context"]["bpm"])==123);CHECK(bool(payload["catalog"]["rack_catalog"]["complete"]));if(r.catalog)CHECK(json(service.captures->schemas[0])==json(ai::proposalSchema(r.policy,*r.catalog->authority)));}
        for(const auto* name:{"source.airackproject","candidate.airackproject","catalog.json","schema.json","instruction.txt","request.json","proof.json","result.bin","preview.wav","preview-left.f32","preview-right.f32"})CHECK(output.getChildFile(name).existsAsFile());
        juce::MemoryBlock source,candidate,encoded,resultBytes,left,right;CHECK(output.getChildFile("source.airackproject").loadFileAsData(source));CHECK(output.getChildFile("candidate.airackproject").loadFileAsData(candidate));ProjectDocument sourceRead,candidateRead;CHECK(decodeProjectState(source.getData(),source.getSize(),sourceRead) && sourceRead==r.source);CHECK(decodeProjectState(candidate.getData(),candidate.getSize(),candidateRead) && result.candidate && candidateRead==*result.candidate);CHECK(encodeProjectState(candidateRead,encoded) && encoded==candidate);ai::ProposalEdits edits=result.edits;ProjectDocument admitted;CHECK(ai::admitProposal(sourceRead,r.policy,edits,admitted)==ai::ProposalStatus::accepted && admitted==candidateRead);
        CHECK(output.getChildFile("result.bin").loadFileAsData(resultBytes));ai::GenerationResult decoded;CHECK(decodeResult(bytes(resultBytes),r,decoded));CHECK(encodeResult(r,decoded,encoded) && encoded==resultBytes);CHECK(decoded.tag==r.tag && decoded.candidate==result.candidate);
        CHECK(output.getChildFile("preview-left.f32").loadFileAsData(left));CHECK(output.getChildFile("preview-right.f32").loadFileAsData(right));CHECK(left.getSize()==result.audio.left.size()*4 && right.getSize()==result.audio.right.size()*4);CHECK(left.getSize()>0 && left.getSize()<=ai::maximumPreviewFrames*4);CHECK(left.getSize()==right.getSize());CHECK(left.getSize()==result.audio.left.size()*4 && std::memcmp(left.getData(),result.audio.left.data(),left.getSize())==0);CHECK(right.getSize()==result.audio.right.size()*4 && std::memcmp(right.getData(),result.audio.right.data(),right.getSize())==0);CHECK(decoded.audio.left==result.audio.left && decoded.audio.right==result.audio.right);
        juce::WavAudioFormat wav;auto wavInput=output.getChildFile("preview.wav").createInputStream();CHECK(wavInput);std::unique_ptr<juce::AudioFormatReader> reader(wavInput?wav.createReaderFor(wavInput.release(),true):nullptr);CHECK(reader && reader->sampleRate==48000 && reader->bitsPerSample==32 && reader->usesFloatingPointData && reader->numChannels==2 && reader->lengthInSamples==juce::int64(result.audio.left.size()));if(reader){juce::AudioBuffer<float> audio(2,int(result.audio.left.size()));CHECK(reader->read(&audio,0,audio.getNumSamples(),0,true,true));CHECK(std::memcmp(audio.getReadPointer(0),result.audio.left.data(),left.getSize())==0);CHECK(std::memcmp(audio.getReadPointer(1),result.audio.right.data(),right.getSize())==0);}
        ai::AudioMeasurements metrics;CHECK(ai::measurePreview(result.audio.left,result.audio.right,result.audio.bodyFrames,metrics)==ai::RenderStatus::ready);CHECK(metrics.finite && metrics.clippedSamples==0 && metrics.peak>0);const auto proof=juce::JSON::parse(output.getChildFile("proof.json").loadFileAsString());CHECK(bool(proof["success"]) && bool(proof["synthetic"]));CHECK(double(proof["measurements"]["peak"])==metrics.peak && double(proof["measurements"]["rms"])==metrics.rms);CHECK(double(proof["measurements"]["activityEndSeconds"])==metrics.activityEndSeconds && double(proof["measurements"]["decaySeconds"])==metrics.decaySeconds);CHECK(bool(proof["measurements"]["tailActiveAtEnd"])==metrics.tailActiveAtEnd);const auto* bands=proof["measurements"]["bandFractions"].getArray();CHECK(bands && bands->size()==3);if(bands && bands->size()==3)for(int i=0;i<3;++i)CHECK(double((*bands)[i])==metrics.bandFractions[std::size_t(i)]);CHECK(double(proof["durationSeconds"])==double(result.audio.left.size())/48000);CHECK(int(proof["frames"])==int(result.audio.left.size()));
        const auto* artifacts=proof["artifacts"].getArray();CHECK(artifacts && artifacts->size()==11);if(artifacts)for(const auto& a:*artifacts){const auto file=output.getChildFile(a["name"].toString());CHECK(file.existsAsFile());CHECK(a["sha256"].toString()==juce::SHA256(file).toHexString());CHECK(juce::int64(a["bytes"])==file.getSize());}
        const auto metadata=juce::JSON::parse(output.getChildFile("request.json").loadFileAsString());CHECK(metadata["model"].toString()==r.model);CHECK(metadata["tag"]["runtime"].toString()==r.tag.runtime.toString());CHECK(int(metadata["tag"]["requestId"])==1 && int(metadata["tag"]["editRevision"])==0 && int(metadata["tag"]["contextRevision"])==0);CHECK(metadata["policy"]["scope"].toString()=="sound" && !bool(metadata["policy"]["allowMacroChanges"]) && !bool(metadata["policy"]["allowSlotReuse"]));CHECK(metadata["context"]["role"].toString()=="lead" && metadata["context"]["key"].toString()=="A" && metadata["context"]["scale"].toString()=="minor" && double(metadata["context"]["capturedBpm"])==123 && metadata["context"]["capturedPhrase"].isVoid());juce::var catalog;CHECK(ai::catalogFor(r.source,catalog));if(r.catalog)catalog.getDynamicObject()->setProperty("rack_catalog",juce::JSON::parse(juce::String::fromUTF8(r.catalog->authority->descriptorJson.c_str())));CHECK(json(juce::JSON::parse(output.getChildFile("catalog.json").loadFileAsString()))==json(catalog));CHECK(json(juce::JSON::parse(output.getChildFile("schema.json").loadFileAsString()))==json(r.catalog?ai::proposalSchema(r.policy,*r.catalog->authority):ai::proposalSchema(r.policy)));CHECK(output.getChildFile("instruction.txt").loadFileAsString()==text);
      }}
    for(int mode:{1,2}){Service service(mode);const auto output=fresh("account-probe-failed");auto p=run(probe,args(service.name,output));CHECK(p.exit==1);publicReply(p,"generate",true);failedArtifacts(output);if(mode==1){CHECK(int(p.reply["runtimeStatus"])==int(RuntimeStatus::modelNotConfigured));CHECK(p.reply["errorCode"].toString()=="model_not_configured");}else{CHECK(int(p.reply["generationStatus"])==int(ai::GenerationStatus::providerFailure));CHECK(int(p.reply["providerStatus"])==int(rack::oauth::Status::denied));CHECK(int(p.reply["httpStatus"])==403);}std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.size()==1);}
    for(int mode:{0,1}){CorruptService service(mode);const auto output=fresh("account-probe-corrupt");auto p=run(probe,args(service.name,output));CHECK(p.exit==1);CHECK(int(p.reply["runtimeStatus"])==int(RuntimeStatus::invalidMessage));CHECK(p.reply["errorCode"].toString()=="invalid_message");failedArtifacts(output);}
    // Hold an actual public generation while attempting to replace its pending
    // proof with an unrelated owned regular file, then with an owned hard link.
    // Closing the CREATE_NEW handle before update makes these replacements win.
    for(bool hardLink:{false,true}){Service service(3);Temp unrelated;const auto target=unrelated.root.getChildFile("unrelated-sentinel.txt");const juce::String sentinel="owned unrelated bytes must remain exact";CHECK(target.replaceWithText(sentinel));const auto output=fresh("account-probe-proof-ownership");std::cout<<"retainedOwnedProofOutput="<<output.getFullPathName()<<std::endl;const auto proof=output.getChildFile("proof.json");bool attempted=false,moved=false,replacement=false;DWORD moveError=0;
        auto p=run(probe,args(service.name,output),true,[&]{if(attempted || !service.captures->heldEntered.load() || !proof.existsAsFile())return;const auto pending=juce::JSON::parse(proof.loadFileAsString());if(pending["reason"].toString()!="pending")return;attempted=true;moved=MoveFileW(proof.getFullPathName().toWideCharPointer(),unrelated.root.getChildFile("original-pending-proof.json").getFullPathName().toWideCharPointer())!=FALSE;moveError=moved?0:GetLastError();if(moved){replacement=hardLink?CreateHardLinkW(proof.getFullPathName().toWideCharPointer(),target.getFullPathName().toWideCharPointer(),nullptr)!=FALSE:CopyFileW(target.getFullPathName().toWideCharPointer(),proof.getFullPathName().toWideCharPointer(),TRUE)!=FALSE;CHECK(replacement);}service.captures->releaseHeld.store(true);});
        CHECK(attempted);CHECK(!moved);CHECK(moved || moveError==ERROR_SHARING_VIOLATION || moveError==ERROR_ACCESS_DENIED);CHECK(p.exit==0);CHECK(target.loadFileAsString()==sentinel);if(replacement)CHECK(proof.loadFileAsString()==sentinel);CHECK(bool(juce::JSON::parse(proof.loadFileAsString())["success"]));CHECK(p.companionMembers==0);CHECK(unrelated.root.getChildFile("proof-replacement-observation.json").replaceWithText(json(object({{"hardLink",hardLink},{"attempted",attempted},{"moved",moved},{"replacementCreated",replacement},{"moveError",int(moveError)},{"targetUnchanged",target.loadFileAsString()==sentinel},{"nativeExit",int(p.exit)}}))));std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.size()==1);
    }
    {Service service;const auto before=snapshot(t.root);auto p=run(probe,args(service.name,t.root));CHECK(p.exit==2);CHECK(bool(p.reply["synthetic"]));CHECK(p.companionMembers==0);CHECK(snapshot(t.root)==before);std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.empty());}
}
void catalog_materialize_authority_exports(const juce::File& probe)
{
    begin(__func__);Temp fixture;const auto instruction=fixture.root.getChildFile("catalog-prompt.txt");CHECK(instruction.replaceWithText("Select Thread Press and Lumen Drift, then tweak each mix."));
    Service service(4);const auto output=fresh("account-probe-catalog");
    auto p=run(probe,fixtureArgs(fixture,service.name,{"--generate","--model","synthetic-model:1","--instruction-file",instruction.getFullPathName(),"--output",output.getFullPathName()}));
    CHECK(p.exit==0);publicReply(p,"generate",true);CHECK(int(p.reply["inferenceCalls"])==1);
    std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.size()==2&&service.captures->prompts.size()==1&&service.captures->schemas.size()==1);
    CHECK(service.captures->request&&service.captures->result&&service.captures->selection);if(!service.captures->request||!service.captures->result||!service.captures->selection)return;
    const auto& materialized=*service.captures->request;const auto& result=*service.captures->result;const auto& selected=*service.captures->selection;
    CHECK(materialized.catalog&&materialized.catalog->phase==ai::CatalogPhase::materialize&&materialized.catalog->authority->definitions.size()==2);
    CHECK(materialized.tag.requestId==2&&materialized.catalog->root.requestId==1&&materialized.catalog->selectionParent==selected.tag);
    CHECK(materialized.catalog->inferenceCallsUsed==selected.inferenceCalls&&materialized.catalog->providerStatus==selected.providerStatus&&materialized.catalog->httpStatus==selected.httpStatus);
    CHECK(selected.status==ai::GenerationStatus::catalogSelectionRequired&&!selected.candidate&&selected.audio.left.empty());
    juce::MemoryBlock rootWire,materializeWire,resultWire,selectionWire;CHECK(output.getChildFile("request.bin").loadFileAsData(rootWire));CHECK(output.getChildFile("materialize-request.bin").loadFileAsData(materializeWire));
    CHECK(output.getChildFile("result.bin").loadFileAsData(resultWire));CHECK(output.getChildFile("selection.bin").loadFileAsData(selectionWire));
    ai::GenerationRequest rootRead,materializeRead;CHECK(decodeRequest(bytes(rootWire),rootRead));CHECK(decodeRequest(bytes(materializeWire),materializeRead));
    CHECK(rootRead.catalog&&rootRead.catalog->root==materializeRead.catalog->root&&rootRead.source==materializeRead.source&&rootRead.catalog->authority->descriptorJson==materializeRead.catalog->authority->descriptorJson);
    ai::GenerationResult selectionRead,resultRead;CHECK(decodeResult(bytes(selectionWire),rootRead,selectionRead));CHECK(decodeResult(bytes(resultWire),materializeRead,resultRead));
    CHECK(resultRead.catalog==selected.catalog&&resultRead.edits.catalogSelections.size()==2&&resultRead.edits.catalogParameters.size()==2&&resultRead.candidate==result.candidate);
    ProjectDocument replay;CHECK(ai::admitProposal(rootRead.source,materializeRead.policy,resultRead.edits,*materializeRead.catalog->authority,replay)==ai::ProposalStatus::accepted&&resultRead.candidate&&replay==*resultRead.candidate);
    auto forged=*materializeRead.catalog->authority;forged.root=materializeRead.tag;ProjectDocument unchanged;CHECK(ai::admitProposal(rootRead.source,materializeRead.policy,resultRead.edits,forged,unchanged)!=ai::ProposalStatus::accepted||unchanged!=replay);
    CHECK(replay.sound.personalities&&replay.sound.personalities->instances.size()==2);const auto metadata=juce::JSON::parse(output.getChildFile("materialize-request.json").loadFileAsString());
    CHECK(metadata["catalog"]["root"]["requestId"].toString()=="1"&&metadata["catalog"]["definitions"].getArray()&&metadata["catalog"]["definitions"].getArray()->size()==2);
    const auto catalog=juce::JSON::parse(output.getChildFile("catalog.json").loadFileAsString());CHECK(catalog["controls"].getArray()&&catalog["controls"].getArray()->size()==int(parameterCount));
    // The library's designs.
    // This export probe deliberately preserves the historical numeric PCM transport.
    auto melodic=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
    std::erase_if(melodic,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
    CHECK(melodic.size()==26&&std::count_if(melodic.begin(),melodic.end(),[&](const auto& d){return d.id==tile;})==1);
    std::erase_if(melodic,[&](const auto& d){return d.id==tile;});CHECK(melodic.size()==25);
    const auto* entries=catalog["rack_catalog"]["entries"].getArray();CHECK(entries&&entries->size()==int(melodic.size())&&bool(catalog["rack_catalog"]["complete"]));
    if(entries)for(const auto& design:melodic)CHECK(std::any_of(entries->begin(),entries->end(),[&](const auto& e){return juce::Uuid(e["id"].toString())==design.id;}));
    if(entries)CHECK(std::none_of(entries->begin(),entries->end(),[&](const auto& e){return juce::Uuid(e["id"].toString())==tile;}));
}
void brief_requests(const juce::File& probe)
{
    begin(__func__); // A brief's request carries its prompt, context, phrase, tempo and source.
    struct Expected{const char* id;const char* instruction;const char* role;double bpm;int bars;std::vector<PatternNote> notes;};
    const std::vector<Expected> expected{
        {"A","Create a warm evolving pad with a clear upper shimmer, soft attacks and broad stereo space; keep the low mids clear in mono.","pad",100,4,
            {{0,8,48,70,1},{0,8,51,70,1},{0,8,55,70,1},{0,8,58,70,1},{8,8,44,70,1},{8,8,48,70,1},{8,8,51,70,1},{8,8,55,70,1}}},
        {"B","Create a tight expressive pluck or lead that speaks clearly, preserves attacks and leaves a restrained stereo tail without washing out repeated notes.","pluck or lead",120,2,
            {{0,.25,60,90,1},{.5,.25,63,70,1},{1,.25,67,90,1},{1.5,.25,70,70,1},{2,.25,67,90,1},{2.5,.25,63,70,1},
             {4,.25,60,90,1},{4.5,.25,63,70,1},{5,.25,67,90,1},{5.5,.25,70,70,1},{6,.25,67,90,1},{6.5,.25,63,70,1}}}};
    const auto fresh24=[](const SoundDocument& s){return std::abs(parameterPhysicalValue(s.values,756)-24.0f)<1e-4f&&s.values[757]==1;};
    for(const auto& e:expected){
        Service service;Temp t;const auto output=fresh("account-probe-brief");std::cout<<"retainedBriefOutput="<<output.getFullPathName()<<std::endl;
        auto p=run(probe,fixtureArgs(t,service.name,{"--generate","--brief",e.id,"--model","synthetic-model:1","--output",output.getFullPathName()}));
        CHECK(p.exit==0);publicReply(p,"generate",true);CHECK(p.reply["explanation"].toString()=="synthetic fixture"&&p.reply["characteristics"].toString()=="pluck");
        const auto proof=juce::JSON::parse(output.getChildFile("proof.json").loadFileAsString());CHECK(proof["explanation"].toString()=="synthetic fixture");
        std::lock_guard lock(service.captures->mutex);CHECK(service.captures->request.has_value());if(!service.captures->request)continue;const auto& r=*service.captures->request;
        CHECK(r.instruction==e.instruction&&r.role==e.role&&r.key=="C"&&r.scale=="minor"&&r.capturedBpm==e.bpm);
        CHECK(r.policy.scope==ai::RequestScope::sound&&!r.policy.allowMacroChanges&&!r.policy.allowSlotReuse);
        const auto& phrase=r.source.pattern;CHECK(phrase.meter==(PhraseMeter{4,4})&&phrase.bars==e.bars&&phrase.grid==PatternGrid::sixteenth&&phrase.noteCount==e.notes.size());
        for(std::size_t n=0;n<e.notes.size()&&n<phrase.noteCount;++n)CHECK(phrase.notes[n]==e.notes[n]);
        CHECK(r.capturedPhrase&&*r.capturedPhrase==phrase);
        // The source is a new instance's sound but for the output gain and protection.
        auto sound=copperArcSound();CHECK(fresh24(r.source.sound)&&!fresh24(sound));
        sound.values[756]=r.source.sound.values[756];sound.values[757]=r.source.sound.values[757];CHECK(r.source.sound==sound);
        CHECK(service.captures->prompts.size()==1);if(!service.captures->prompts.empty()){const auto& context=service.captures->prompts[0]["context"];
            CHECK(double(context["bpm"])==e.bpm&&context["phrase_source"].toString()=="captured"&&context["tempo_source"].toString()=="captured");
            CHECK(context["phrase"]["notes"].getArray()&&context["phrase"]["notes"].getArray()->size()==int(e.notes.size()));}
        const auto metadata=juce::JSON::parse(output.getChildFile("request.json").loadFileAsString());CHECK(metadata["brief"].toString()==e.id);
        CHECK(metadata["context"]["role"].toString()==e.role&&double(metadata["context"]["capturedBpm"])==e.bpm);
        const auto* notes=metadata["context"]["capturedPhrase"]["notes"].getArray();CHECK(notes&&notes->size()==int(e.notes.size()));
        if(notes&&notes->size()==int(e.notes.size()))for(int n=0;n<notes->size();++n){const auto& w=(*notes)[n];const auto& x=e.notes[std::size_t(n)];
            CHECK(double(w["start_beats"])==x.startBeats&&double(w["duration_beats"])==x.durationBeats&&int(w["pitch"])==x.pitch&&int(w["velocity"])==x.velocity&&int(w["channel"])==x.channel);}
        CHECK(output.getChildFile("instruction.txt").loadFileAsString()==e.instruction);
    }
    // A brief replaces the instruction file and needs --generate; only A and B exist.
    {Service service;Temp t;const auto instruction=t.root.getChildFile("prompt.txt");CHECK(instruction.replaceWithText("a pad"));const auto output=fresh("rejected-brief");
        const std::vector<std::vector<juce::String>> invalid{
            {"--generate","--brief","C","--model","synthetic-model:1","--output",output.getFullPathName()},
            {"--generate","--brief","a","--model","synthetic-model:1","--output",output.getFullPathName()},
            {"--generate","--brief","A","--instruction-file",instruction.getFullPathName(),"--model","synthetic-model:1","--output",output.getFullPathName()},
            {"--generate","--brief","A","--brief","B","--model","synthetic-model:1","--output",output.getFullPathName()},
            {"--generate","--brief","A","--output",output.getFullPathName()},
            {"--status","--brief","A"}};
        for(const auto& a:invalid){const auto result=run(probe,fixtureArgs(t,service.name,a));CHECK(result.exit==2);CHECK(result.companionMembers==0);CHECK(!output.exists());}
        std::lock_guard lock(service.captures->mutex);CHECK(service.captures->jobs.empty());}
}
}
int main(int argc,char** argv)
{
    try{if(argc!=3)return 2;Temp logRoot;logs=logRoot.root;std::cout<<"retainedFixtureCommandLogs="<<logs.getFullPathName()<<std::endl;const juce::File probe=juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[1]));const juce::File helper=juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[2]));CHECK(probe.existsAsFile() && helper.existsAsFile());acceptedHelperHash=juce::SHA256(helper).toHexString();CHECK(juce::SHA256(probe.getSiblingFile(companionExecutableName)).toHexString()==acceptedHelperHash);help_and_argument_isolation(probe);public_account_cli(probe,helper);generation_and_artifact_integrity(probe);catalog_materialize_authority_exports(probe);brief_requests(probe);finish();std::cout<<"checks="<<checks<<" groups="<<groups<<" failures="<<failures<<" commands="<<commandId<<std::endl;return failures?1:0;}catch(...){std::cerr<<"fixture_setup_exception\n";return 2;}
}
