#include "../../tests/native/FrozenRackExamples.h"
#include "InstrumentProcessor.h"
#include "CompanionRuntimeServer.h"
#include "CompanionBackend.h"
#include "AiCatalog.h"
#include "RackLibrary.h"
#include "RackExamples.h"
#include "StrictJson.h"
#include "PatternIO.h"
#include "ScreenDriver.h"
#include "../../tests/native/OwnedFixture.h"
#include "../../tests/native/IsolatedRackLibrary.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

// Development-only public workflow driver. Synthetic service exists solely in
// --self-test; --run attaches to an existing helper with allowLaunch=false.
namespace airack {
// Development exporter observation only: copies real current context and accepted receipt under the owner lock.
struct InstrumentAiTestAccess {
    struct LocalExport {ai::LocalGenerationRequest input;std::optional<ai::PreviewReceipt> accepted;};
    static std::unique_ptr<LocalExport> localExport(InstrumentProcessor& p)
    {
        const std::lock_guard lock(p.restoreMutex);
        if(!p.aiLocal||!p.aiLocal->current||!p.aiPayloads->request||!p.aiLocalCurrentOwned())return {};
        auto observed=std::make_unique<LocalExport>();observed->input.request=*p.aiPayloads->request;observed->input.local=*p.aiLocal->current;
        if(p.aiPhase==AiProposalPhase::proposed&&p.aiPayloads->result&&p.aiPayloads->result->tag==observed->input.request.tag
            &&p.aiLocal->hasParent&&p.aiLocal->parentReceipt&&p.aiLocal->parentReceipt->binding.request==observed->input.request.tag)
            observed->accepted=p.aiLocal->parentReceipt;
        return observed;
    }
};
}
namespace
{
using namespace airack;
using namespace airack::ai;
using namespace airack::companion;
constexpr const char* pluck="a soft pluck";
constexpr const char* bass="a rolling bassline in the style of John Summit";
constexpr const char* child="Make it darker and tighter. Add a delay effect rack.";
std::atomic_bool interrupted{false};
BOOL WINAPI consoleControl(DWORD event)
{if(event<=CTRL_CLOSE_EVENT||event==CTRL_LOGOFF_EVENT||event==CTRL_SHUTDOWN_EVENT){interrupted.store(true);return TRUE;}return FALSE;}
void require(bool pass,const char* reason){if(!pass)throw std::runtime_error(reason);}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& [key,value]:fields)o->setProperty(key,value);return o;}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
template<class T> bool bits(T a,T b){return std::memcmp(&a,&b,sizeof(T))==0;}
bool choicesEqual(const RackReconcileChoices& a,const RackReconcileChoices& b)
{
    if(a.detach!=b.detach||a.remove!=b.remove||a.replace!=b.replace||a.controls.size()!=b.controls.size())return false;
    for(std::size_t n=0;n<a.controls.size();++n){const auto& x=a.controls[n];const auto& y=b.controls[n];if(x.instance!=y.instance||x.control!=y.control||x.replacement.has_value()!=y.replacement.has_value())return false;
        if(x.replacement&&(*x.replacement!=*y.replacement||!bits(x.replacement->minimum,y.replacement->minimum)||!bits(x.replacement->maximum,y.replacement->maximum)))return false;}
    return true;
}
bool editsEqual(const ProposalEdits& a, const ProposalEdits& b)
{
    if (!choicesEqual(a.reconciliation,b.reconciliation) || a.parameters != b.parameters || a.macros != b.macros || a.pattern != b.pattern
        || a.characteristics != b.characteristics || a.explanation != b.explanation || a.name != b.name || a.racks.size() != b.racks.size()
        || a.catalogSelections != b.catalogSelections || a.catalogParameters != b.catalogParameters) return false;
    for (std::size_t i=0; i<a.racks.size(); ++i) {
        const auto& x=a.racks[i]; const auto& y=b.racks[i];
        if (x.kind!=y.kind || x.slot!=y.slot || x.destination!=y.destination || x.position!=y.position || x.effect!=y.effect || x.allowReuse!=y.allowReuse) return false;
    }
    for(std::size_t i=0;i<a.catalogParameters.size();++i)if(!bits(a.catalogParameters[i].normalized,b.catalogParameters[i].normalized))return false;
    return true;
}
bool catalogContextEqual(const std::optional<CatalogRequestContext>& a,const std::optional<CatalogRequestContext>& b)
{
    if(a.has_value()!=b.has_value())return false;if(!a)return true;const auto& x=*a;const auto& y=*b;
    return x.version==y.version&&x.phase==y.phase&&x.root==y.root&&x.selectionParent==y.selectionParent
        && bool(x.authority)==bool(y.authority)&&(!x.authority||*x.authority==*y.authority)&&editsEqual(x.baseIntent,y.baseIntent)
        && x.inferenceCallsUsed==y.inferenceCallsUsed&&x.providerStatus==y.providerStatus&&x.httpStatus==y.httpStatus;
}
bool requestEqual(const GenerationRequest& a, const GenerationRequest& b)
{
    return a.structuralGeneration==b.structuralGeneration && a.tag==b.tag && a.source==b.source && a.policy.scope==b.policy.scope && a.policy.allowMacroChanges==b.policy.allowMacroChanges
        && a.policy.allowSlotReuse==b.policy.allowSlotReuse && a.policy.requirePattern==b.policy.requirePattern && a.model==b.model && a.instruction==b.instruction && a.role==b.role
        && a.key==b.key && a.scale==b.scale && a.capturedPhrase==b.capturedPhrase && a.capturedBpm.has_value()==b.capturedBpm.has_value()
        && (!a.capturedBpm || bits(*a.capturedBpm,*b.capturedBpm)) && catalogContextEqual(a.catalog,b.catalog) && a.namesInUse==b.namesInUse;
}
using LocalExport=InstrumentAiTestAccess::LocalExport;
bool usesOwnedPreview(const GenerationRequest& r)
{
    if(r.catalog&&r.catalog->authority&&r.catalog->authority->rendering==CatalogRendering::ownedLocal)return true;
    for(std::uint8_t bank=0;bank<4;++bank)if(r.source.sound.layout.synthUse[bank]==SlotUse::present&&sourceModule(r.source.sound,bank)==NativeModule::drum)return true;
    return false;
}
bool encodeRequestArtifact(const GenerationRequest& r,const LocalExport* local,juce::MemoryBlock& out)
{
    if(!usesOwnedPreview(r))return !local&&encodeRequest(r,out);
    return local&&requestEqual(r,local->input.request)&&encodeLocalRequest(local->input,out);
}
bool decodeRequestArtifact(WireBytes body,const LocalExport* local,GenerationRequest& out)
{
    if(!local)return decodeRequest(body,out);
    auto decoded=std::make_unique<LocalGenerationRequest>();juce::MemoryBlock again;
    if(!decodeLocalRequest(body,*decoded)||decoded->local!=local->input.local||!encodeLocalRequest(*decoded,again)
        ||again.getSize()!=body.size()||std::memcmp(again.getData(),body.data(),body.size()))return false;
    out=std::move(decoded->request);return true;
}
bool encodeResultArtifact(const GenerationRequest& r,const GenerationResult& result,const LocalExport* local,juce::MemoryBlock& out)
{
    if(!usesOwnedPreview(r))return !local&&encodeResult(r,result,out);
    if(!local||!local->accepted||!requestEqual(r,local->input.request))return false;
    auto terminal=std::make_unique<LocalGenerationResult>();terminal->numeric=result;terminal->numeric.audio={};terminal->rendered=local->accepted;
    return encodeLocalResult(local->input,*terminal,out);
}
bool decodeResultArtifact(WireBytes body,const GenerationRequest& r,const LocalExport* local,const PreviewAudio& pcm,GenerationResult& out)
{
    if(!usesOwnedPreview(r))return !local&&decodeResult(body,r,out);
    if(!local||!local->accepted||!requestEqual(r,local->input.request))return false;
    auto terminal=std::make_unique<LocalGenerationResult>();
    if(!decodeLocalResult(body,local->input,*terminal)||!terminal->numeric.audio.left.empty()||!terminal->numeric.audio.right.empty())return false;
    auto audio=std::make_unique<PreviewAudio>(pcm);
    return joinLocalPreview(local->input,*terminal,*local->accepted,std::move(*audio),out);
}
juce::String json(const juce::var& v)
{
    // Reuse AccountProbe's decimal-point conversion: the pinned JUCE reader's
    // fractional-leading-zero limit otherwise loses tiny native measurements.
    const auto formatted=juce::JSON::toString(v,true,22).toStdString();std::string out;
    for(std::size_t i=0;i<formatted.size();)
    {
        const auto begin=i;const auto c=formatted[i];
        if(c=='"'){++i;while(i<formatted.size()){if(formatted[i]=='\\'){i=std::min(i+2,formatted.size());continue;}if(formatted[i++]=='"')break;}out.append(formatted,begin,i-begin);continue;}
        if(c=='-'||(c>='0'&&c<='9'))
        {
            ++i;while(i<formatted.size()&&std::string_view("0123456789.eE+-").find(formatted[i])!=std::string_view::npos)++i;
            const auto token=formatted.substr(begin,i-begin);const std::size_t sign=token[0]=='-'?1:0;
            if(token.compare(sign,2,"0.")==0&&token.find_first_of("eE")==std::string::npos)
            {auto first=sign+2;while(first<token.size()&&token[first]=='0')++first;if(first<token.size()){out.append(token,0,sign);out+=token[first];if(first+1<token.size()){out+='.';out.append(token,first+1,std::string::npos);}out+="e-";out+=std::to_string(first-sign-1);continue;}}
            out+=token;continue;
        }
        out+=c;++i;
    }
    return juce::String::fromUTF8(out.data(),int(out.size()));
}
juce::File workspace(){return juce::File(RACKSYNTH_WORKSPACE);}
juce::File hostRoot(){return workspace().getChildFile("build/host-fixtures");}
juce::File evidenceRoot(){return workspace().getChildFile(".superpowers/sdd/2026-10-06-explicit-pattern-output/task-3");}
struct Arguments
{enum class Action{invalid,help,selfTest,run,runOwned};Action action=Action::invalid;juce::File companion,output;};
bool absolute(const std::wstring& raw,juce::File& file)
{
    if(raw.size()<3||raw.size()>=32767||!((raw[0]>=L'A'&&raw[0]<=L'Z')||(raw[0]>=L'a'&&raw[0]<=L'z'))||raw[1]!=L':'||(raw[2]!=L'\\'&&raw[2]!=L'/'))return false;
    std::vector<wchar_t> full(32768);const auto size=GetFullPathNameW(raw.c_str(),DWORD(full.size()),full.data(),nullptr);if(!size||size>=full.size())return false;
    const std::filesystem::path path(full.data());
    for(const auto& part:path.relative_path())
    {
        const auto component=part.wstring();if(component.empty()||component.back()==L'.'||component.back()==L' '||component.find_first_of(L":*?\"")!=std::wstring::npos)return false;
        const auto stem=juce::String(component.c_str()).upToFirstOccurrenceOf(".",false,false).toUpperCase();
        if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||(stem.length()==4&&(stem.startsWith("COM")||stem.startsWith("LPT"))&&stem[3]>='1'&&stem[3]<='9'))return false;
    }
    file=juce::File(juce::String(full.data()));return true;
}
bool plainPath(const juce::File& file,bool directory)
{
    const std::filesystem::path path(file.getFullPathName().toWideCharPointer());auto current=path.root_path();
    for(const auto& part:path.relative_path())
    {current/=part;const auto attributes=GetFileAttributesW(current.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||(current!=path&&!(attributes&FILE_ATTRIBUTE_DIRECTORY)))return false;}
    const auto attributes=GetFileAttributesW(path.c_str());return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(attributes&FILE_ATTRIBUTE_DIRECTORY)==directory;
}
bool freshPath(const juce::File& file)
{return plainPath(file.getParentDirectory(),true)&&GetFileAttributesW(file.getFullPathName().toWideCharPointer())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND;}
bool parse(const std::vector<std::wstring>& args,Arguments& parsed)
{
    Arguments next;
    if(args.size()==1&&(args[0]==L"--help"||args[0]==L"--self-test")){next.action=args[0]==L"--help"?Arguments::Action::help:Arguments::Action::selfTest;parsed=next;return true;}
    if(args.size()==3&&args[0]==L"--run-owned"&&args[1]==L"--output"){
        if(!absolute(args[2],next.output)||next.output.getParentDirectory()!=hostRoot()||!freshPath(next.output))return false;
        next.action=Arguments::Action::runOwned;parsed=next;return true;
    }
    if(args.size()!=5||args[0]!=L"--run")return false;
    std::map<std::wstring,std::wstring> fields;
    for(std::size_t i=1;i<args.size();i+=2)if((args[i]!=L"--companion"&&args[i]!=L"--output")||!fields.emplace(args[i],args[i+1]).second)return false;
    if(!fields.contains(L"--companion")||!fields.contains(L"--output")||!absolute(fields[L"--companion"],next.companion)||!absolute(fields[L"--output"],next.output))return false;
    if(!next.companion.isAChildOf(workspace())||!plainPath(next.companion,false)||next.companion.getFileExtension().toLowerCase()!=".exe"||next.output.getParentDirectory()!=hostRoot()||!freshPath(next.output))return false;
    next.action=Arguments::Action::run;parsed=next;return true;
}
struct Handle
{
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
bool exactFile(const juce::File& file,const juce::MemoryBlock& data)
{juce::MemoryBlock physical;return plainPath(file,false)&&file.loadFileAsData(physical)&&physical==data;}
struct Exports
{
    juce::File root;Handle parentLease,rootLease;
    bool create(const juce::File& file)
    {
        if(!root.getFullPathName().isEmpty()||!file.isAChildOf(workspace())||!freshPath(file))return false;
        parentLease.value=CreateFileW(file.getParentDirectory().getFullPathName().toWideCharPointer(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(parentLease.value==INVALID_HANDLE_VALUE||!freshPath(file)||!CreateDirectoryW(file.getFullPathName().toWideCharPointer(),nullptr))return false;
        root=file;rootLease.value=CreateFileW(root.getFullPathName().toWideCharPointer(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        return rootLease.value!=INVALID_HANDLE_VALUE&&plainPath(root,true);
    }
    bool save(const char* name,const juce::MemoryBlock& data)
    {
        if(rootLease.value==INVALID_HANDLE_VALUE||!plainPath(root,true)||data.getSize()>DWORD(-1))return false;
        const auto file=root.getChildFile(name);Handle handle;handle.value=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle.value==INVALID_HANDLE_VALUE)return false;DWORD written=0;
        const bool writtenExactly=WriteFile(handle.value,data.getData(),DWORD(data.getSize()),&written,nullptr)&&written==data.getSize()&&FlushFileBuffers(handle.value);
        CloseHandle(handle.value);handle.value=INVALID_HANDLE_VALUE;return writtenExactly&&exactFile(file,data);
    }
    bool text(const char* name,const juce::String& value){return save(name,juce::MemoryBlock(value.toRawUTF8(),value.getNumBytesAsUTF8()));}
};
std::unique_ptr<ProjectDocument> snapshotOwned(InstrumentProcessor& p)
{auto d=std::make_unique<ProjectDocument>();std::uint64_t revision;require(p.tryProjectSnapshot(*d,revision),"snapshot_failed");return d;}
std::size_t index(const char* id)
{const auto r=parameterRegistry();const auto it=std::find_if(r.begin(),r.end(),[&](const auto& d){return d.id==id;});require(it!=r.end(),"parameter_missing");return std::size_t(it-r.begin());}
juce::Component* find(juce::Component& c,const juce::String& id)
{if(c.getComponentID()==id)return &c;for(auto* childComponent:c.getChildren())if(auto* found=find(*childComponent,id))return found;return nullptr;}
template<class T>T& widget(juce::Component& editor,const char* id)
{auto* c=dynamic_cast<T*>(find(editor,id));require(c!=nullptr,"editor_component_missing");return *c;}
struct Workflow
{
    std::unique_ptr<InstrumentProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::AudioBuffer<float> audio{2,512};juce::MidiBuffer midi;
    std::array<std::vector<float>,2> previewOutput;
    std::uint64_t callbacks=0,nonfinite=0;double peak=0,maximumCallbackUs=0;
    explicit Workflow(const ClientOptions& options,std::shared_ptr<RackLibraryRuntime> library={})
    {processor=std::make_unique<InstrumentProcessor>(options,std::move(library));processor->prepareToPlay(48000,512);editor.reset(processor->createEditor());editor->setSize(1080,680);}
    ~Workflow(){editor.reset();processor->releaseResources();processor.reset();}
    void callback();void pump();
    bool wait(const std::function<bool()>& done,std::uint64_t timeout=clientOperationTimeoutMs);
    bool open();void context(const char*,int);
    bool submit(const char*,bool=false);bool complete(const GenerationRequest&);
    void requirePattern(bool);bool requiresPattern();void cancel();
    bool previewApply();bool recall(const ProjectDocument&);
    void report(const char*,std::uint64_t) const;
    studio::Screen& screen();void compose();
};
void Workflow::callback()
{
    midi.clear();midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(60)),0);midi.addEvent(juce::MidiMessage::noteOff(1,60),256);
    const auto begin=std::chrono::steady_clock::now();processor->processBlock(audio,midi);
    maximumCallbackUs=std::max(maximumCallbackUs,std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count());++callbacks;
    for(int c=0;c<2;++c)for(int n=0;n<512;++n){const auto v=audio.getSample(c,n);if(!std::isfinite(v))++nonfinite;else peak=std::max(peak,double(std::abs(v)));}
}
void Workflow::pump(){processor->pollAi();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}
bool Workflow::wait(const std::function<bool()>& done,std::uint64_t timeout)
{const auto end=GetTickCount64()+timeout;do{if(done())return true;if(interrupted.load())return false;callback();pump();}while(GetTickCount64()<end);return done();}
bool click(juce::Component& editor,const char* id)
{auto& button=widget<juce::Button>(editor,id);if(!button.isEnabled()||!button.onClick)return false;button.onClick();return true;}
// The AI unit's screen once its timer has drawn the processor's latest state there.
studio::Screen& Workflow::screen()
{juce::MessageManager::getInstance()->runDispatchLoopUntil(60);auto* s=screenOf(*editor,"ai.screen");require(s!=nullptr,"editor_component_missing");return *s;}
// Back on the mode's page by its button, from the account, linked or join page.
void Workflow::compose()
{auto& s=screen();if(s.page=="connected")click(*editor,"ai.account");else if(s.page=="linked"||s.page=="join")click(*editor,"ai.linked");}
bool Workflow::open()
{
    auto& toggle=widget<juce::Button>(*editor,"ai.toggle");if(!toggle.getToggleState())toggle.onClick();
    if(!wait([&]{const auto s=processor->aiWorkflowStatus();return s.connection.phase==ClientPhase::failed||(s.connection.phase==ClientPhase::ready&&!s.accountPending);},clientStartupTimeoutMs+clientOperationTimeoutMs))return false;
    const auto* account=processor->aiAccountReply();
    if(!account||account->status!=RuntimeStatus::ready||account->providerStatus!=rack::oauth::Status::connected||account->models.empty())return false;
    if(std::none_of(account->models.begin(),account->models.end(),[&](const auto& m){return m.slug==account->selectedModel;}))
    {
        // The first model's row, without opening the account page, which asks for the status again.
        screen().onCommand("ai.model",0);
        if(!wait([&]{return !processor->aiWorkflowStatus().accountPending;}))return false;
    }
    account=processor->aiAccountReply();return account&&account->selectedModel.isNotEmpty()&&processor->aiWorkflowStatus().connection.phase==ClientPhase::ready;
}
void Workflow::context(const char* role,int scope)
{
    compose();auto& s=screen();
    require(enterText(s,"ai.role",role)&&enterText(s,"ai.key","A")&&enterText(s,"ai.scale","minor")&&enterText(s,"ai.tempo","123"),"context_entry_refused");
    require(stepTo(s,"ai.scope",scope-1),"scope_option_refused");
}
void Workflow::requirePattern(bool on){require(stepTo(screen(),"ai.requirePattern",on?1:0),"notes_option_refused");}
bool Workflow::requiresPattern()
{const auto& options=screen().shown.options;const auto at=options.find("ai.requirePattern");return at!=options.end()&&at->second!=0;}
void Workflow::cancel(){require(pressItem(screen(),"ai.cancel"),"cancel_refused");}
bool Workflow::submit(const char* instruction,bool refinement)
{
    const auto* old=processor->aiCurrentRequest();const auto id=old?old->tag.requestId:0;
    compose();
    if(!enterText(screen(),"ai.prompt",juce::String::fromUTF8(instruction))){report("submit_prompt_refused",id);return false;}
    const char* button=refinement?"ai.refine":"ai.generate";
    if(!click(*editor,button)){report(widget<juce::Button>(*editor,button).isEnabled()?"submit_refused":"submit_disabled",id);return false;}
    const auto* request=processor->aiCurrentRequest();
    const bool pending=request&&request->tag.requestId>id&&request->instruction==instruction&&processor->aiProposalPhase()==AiProposalPhase::pending;
    if(!pending)report("submit_not_pending",id);
    return pending;
}
bool completedRequest(const GenerationRequest& initial,const GenerationRequest& current)
{
    juce::MemoryBlock a,b;const bool owned=usesOwnedPreview(initial)||usesOwnedPreview(current);
    if(owned){if(!usesOwnedPreview(initial)||!usesOwnedPreview(current)||!ai::detail::catalogRequestValid(initial)||!ai::detail::catalogRequestValid(current))return false;}
    else if(!encodeRequest(initial,a)||!encodeRequest(current,b))return false;
    auto normalized=current;
    if(current.tag!=initial.tag){if(!initial.catalog||!current.catalog||!initial.catalog->authority||!current.catalog->authority||initial.catalog->phase!=CatalogPhase::select||current.catalog->phase!=CatalogPhase::materialize
        ||current.tag.requestId<=initial.tag.requestId||current.catalog->selectionParent!=initial.tag||current.catalog->root!=initial.catalog->root
        ||current.catalog->authority->root!=initial.catalog->authority->root||current.catalog->authority->snapshot!=initial.catalog->authority->snapshot
        ||current.catalog->authority->descriptorJson!=initial.catalog->authority->descriptorJson
        ||current.catalog->authority->rendering!=initial.catalog->authority->rendering)return false;
        const auto& before=initial.catalog->baseIntent;const auto& after=current.catalog->baseIntent;
        const auto retains=[](const auto& old,const auto& next,auto same){return std::all_of(old.begin(),old.end(),[&](const auto& row){return std::any_of(next.begin(),next.end(),[&](const auto& value){return same(row,value);});});};
        const auto prefix=[](const auto& old,const auto& next,auto same){return old.size()<=next.size()&&std::equal(old.begin(),old.end(),next.begin(),same);};
        const auto exact=[](const auto& x,const auto& y){return x==y;};
        if(!retains(initial.catalog->authority->definitions,current.catalog->authority->definitions,exact)
            ||!retains(before.catalogSelections,after.catalogSelections,exact)
            ||!retains(before.catalogParameters,after.catalogParameters,[](const auto& x,const auto& y){return x.selection==y.selection&&x.unit==y.unit&&x.key==y.key;})
            ||!retains(before.parameters,after.parameters,[](const auto& x,const auto& y){return x.id==y.id;})
            ||!retains(before.macros,after.macros,[](const auto& x,const auto& y){return x.index==y.index;})
            ||(before.pattern&&!after.pattern)
            ||!prefix(before.racks,after.racks,[](const auto& x,const auto& y){return x.kind==y.kind&&x.slot==y.slot&&x.destination==y.destination&&x.position==y.position&&x.effect==y.effect&&x.allowReuse==y.allowReuse;})
            ||!prefix(before.reconciliation.detach,after.reconciliation.detach,exact)||!prefix(before.reconciliation.remove,after.reconciliation.remove,exact)
            ||!prefix(before.reconciliation.replace,after.reconciliation.replace,exact)
            ||!prefix(before.reconciliation.controls,after.reconciliation.controls,[](const auto& x,const auto& y){return x.instance==y.instance&&x.control==y.control&&x.replacement==y.replacement;}))return false;
        normalized.tag=initial.tag;normalized.catalog=initial.catalog;}
    return owned?requestEqual(initial,normalized):encodeRequest(normalized,b)&&a==b;
}
bool Workflow::complete(const GenerationRequest& request)
{
    if(!wait([&]{return processor->aiProposalPhase()!=AiProposalPhase::pending;})){report("complete_timeout",request.tag.requestId);return false;}
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    const auto* current=processor->aiCurrentRequest();
    const bool proposed=current&&completedRequest(request,*current)&&processor->aiProposalPhase()==AiProposalPhase::proposed&&processor->aiProposal();
    if(!proposed)report("complete_not_proposed",request.tag.requestId);
    return proposed;
}
void Workflow::report(const char* step,std::uint64_t requestId) const
{
    static constexpr const char* phases[]{"idle","pending","proposed","stale","failed"};
    static constexpr const char* connections[]{"closed","connecting","ready","failed"};
    const auto status=processor->aiWorkflowStatus();const auto* current=processor->aiCurrentRequest();
    std::cout<<"DIAG "<<step<<" request="<<requestId<<" current="<<(current?current->tag.requestId:0)<<" phase="<<phases[int(processor->aiProposalPhase())]
        <<" proposal="<<(processor->aiProposal()!=nullptr)<<" connection="<<connections[int(status.connection.phase)]<<'/'<<int(status.connection.status)
        <<' '<<status.connection.errorCode<<" message="<<status.message<<std::endl;
}
bool Workflow::previewApply()
{
    const auto beforeStorage=snapshotOwned(*processor);const auto& before=*beforeStorage;const auto* proposal=processor->aiProposal();if(!proposal||!proposal->candidate)return false;const auto expectedStorage=std::make_unique<ProjectDocument>(*proposal->candidate);const auto& expected=*expectedStorage;
    if(!click(*editor,"ai.preview"))return false;callback();
    for(int c=0;c<2;++c)previewOutput[c].assign(audio.getReadPointer(c),audio.getReadPointer(c)+512);
    pump();if(!processor->aiPreviewPlaying()||(*snapshotOwned(*processor))!=before)return false;
    if(!click(*editor,"ai.stop")||processor->aiPreviewStopped()||(*snapshotOwned(*processor))!=before)return false;
    if(!click(*editor,"ai.apply")||!processor->aiWorkflowStatus().applyPending||(*snapshotOwned(*processor))!=before)return false;
    // No control pumping before the audio acknowledgement. The ordinary panel
    // timer/poll owns the single delayed transaction once stop is acknowledged.
    callback();if(!processor->aiPreviewStopped()||(*snapshotOwned(*processor))!=before)return false;
    if(!wait([&]{return !processor->aiWorkflowStatus().applyPending;},clientStartupTimeoutMs))return false;
    return (*snapshotOwned(*processor))==expected&&nonfinite==0;
}
bool Workflow::recall(const ProjectDocument& d)
{juce::MemoryBlock state;if(!encodeProjectState(d,state)||!processor->restoreProjectState(state.getData(),int(state.getSize())))return false;pump();return (*snapshotOwned(*processor))==d;}
struct Stage
{
    Exports output;GenerationRequest request;std::unique_ptr<LocalExport> submittedLocal,finalLocal;std::optional<GenerationResult> result;
    std::optional<ProjectDocument> original;std::optional<ProposalEdits> intent;
    bool captureRequest(Workflow&);bool captureResult(Workflow&);
};
bool saveProject(Exports& output,const char* name,const ProjectDocument& d)
{juce::MemoryBlock state;auto decodedStorage=std::make_unique<ProjectDocument>();auto& decoded=*decodedStorage;return encodeProjectState(d,state)&&decodeProjectState(state.getData(),state.getSize(),decoded)&&decoded==d&&output.save(name,state);}
juce::var phraseJson(const PatternDocument& p)
{
    constexpr const char* grids[]{"quarter","eighth","sixteenth","thirty_second","sixty_fourth"};juce::Array<juce::var> notes;
    for(std::size_t n=0;n<p.noteCount;++n){const auto& note=p.notes[n];notes.add(object({{"start_beats",note.startBeats},{"duration_beats",note.durationBeats},{"pitch",int(note.pitch)},{"velocity",int(note.velocity)},{"channel",int(note.channel)}}));}
    return object({{"bars",int(p.bars)},{"meter_numerator",int(p.meter.numerator)},{"meter_denominator",int(p.meter.denominator)},{"grid",grids[int(p.grid)]},{"swing",p.swing},{"notes",notes}});
}
juce::var editsJson(const ProposalEdits& edits,bool catalog=false)
{
    constexpr const char* kinds[]{"add_synth","remove_synth","move_synth","add_effect","remove_effect","move_effect","replace_effect"};constexpr const char* effects[]{"drive","delay","reverb","compressor","eq","filter_fx","chorus","phaser","flanger","stereo","dimension","frequency_shift","convolution","frequency_split","mid_side_split"};
    juce::Array<juce::var> parameters,racks,macros;
    for(const auto& e:edits.parameters)parameters.add(object({{"id",e.id},{"normalized",e.normalized}}));
    for(const auto& e:edits.racks)racks.add(object({{"kind",kinds[int(e.kind)]},{"slot",int(e.slot)},{"chain",int(e.destination)},{"position",int(e.position)},{"effect",effects[int(e.effect)]}}));
    for(const auto& e:edits.macros)macros.add(object({{"index",int(e.index)},{"target_id",e.targetId.isEmpty()?juce::var{}:juce::var(e.targetId)},{"amount",e.amount}}));
    auto proposal=object({{"version",1},{"parameter_edits",parameters},{"rack_edits",racks},{"macro_edits",macros},{"pattern",edits.pattern?phraseJson(*edits.pattern):juce::var{}},{"characteristics",edits.characteristics},{"explanation",edits.explanation}});
    if(catalog||!edits.reconciliation.detach.empty()||!edits.reconciliation.remove.empty()||!edits.reconciliation.replace.empty()||!edits.reconciliation.controls.empty()){
        auto* o=proposal.getDynamicObject();o->setProperty("version",2);o->setProperty("name",edits.name.isNotEmpty()?edits.name:juce::String("Workflow Test Sound"));
        juce::Array<juce::var> detach,remove,replace,controls;for(const auto& id:edits.reconciliation.detach)detach.add(id.toDashedString());for(const auto& id:edits.reconciliation.remove)remove.add(id.toDashedString());for(const auto& id:edits.reconciliation.replace)replace.add(id.toDashedString());
        constexpr const char* widgets[]{"rotary","verticalSlider","toggle","choice"};constexpr const char* responses[]{"linear","logarithmic"};
        for(const auto& r:edits.reconciliation.controls){juce::var replacement;if(r.replacement){const auto& f=*r.replacement;replacement=object({{"label",f.label},{"unit",int(f.unit)},{"section",int(f.section)},{"key",f.key},{"widget",widgets[int(f.widget)]},{"response",responses[int(f.response)]},{"minimum",f.minimum},{"maximum",f.maximum}});}controls.add(object({{"instance",r.instance.toDashedString()},{"control",int(r.control)},{"replacement",replacement}}));}
        o->setProperty("reconciliation",object({{"detach",detach},{"remove",remove},{"replace",replace},{"controls",controls}}));
        if(catalog){juce::Array<juce::var> selected,tweaks;for(const auto& s:edits.catalogSelections)selected.add(object({{"local_id",int(s.localId)},{"design",s.design.toDashedString()},{"revision",juce::String(std::to_string(s.revision))},{"content_sha256",s.contentSha256},{"destination",object({{"role",s.destination.role==RackRole::synth?"synth":"fx"},{"chain",int(s.destination.chain)},{"position",int(s.destination.position)}})}}));
            for(const auto& p:edits.catalogParameters)tweaks.add(object({{"selection",int(p.selection)},{"unit",int(p.unit)},{"key",p.key},{"normalized",p.normalized}}));o->setProperty("catalog_selections",selected);o->setProperty("catalog_parameter_edits",tweaks);}}
    return proposal;
}
juce::var tagJson(const RequestTag& t)
{return object({{"runtime",t.runtime.toString()},{"requestId",juce::int64(t.requestId)},{"editRevision",juce::int64(t.editRevision)},{"contextRevision",juce::int64(t.contextRevision)}});}
juce::var catalogContextJson(const CatalogRequestContext& c)
{
    auto exactTag=[](const RequestTag& t){return object({{"runtime",t.runtime.toString()},{"requestId",juce::String(std::to_string(t.requestId))},{"editRevision",juce::String(std::to_string(t.editRevision))},{"contextRevision",juce::String(std::to_string(t.contextRevision))}});};
    const auto& a=*c.authority;juce::Array<juce::var> definitions;for(const auto& d:a.definitions)definitions.add(object({{"design",d.design.toDashedString()},{"revision",juce::String(std::to_string(d.revision))},{"contentSha256",d.contentSha256},{"canonicalBytes",juce::int64(d.canonical.getSize())},{"canonicalBase64",d.canonical.toBase64Encoding()}}));
    return object({{"version",int(c.version)},{"phase",c.phase==CatalogPhase::select?"select":"materialize"},{"root",exactTag(c.root)},{"selectionParent",c.selectionParent?exactTag(*c.selectionParent):juce::var{}},{"snapshot",a.snapshot.toString()},
        {"descriptorJson",juce::String::fromUTF8(a.descriptorJson.data(),int(a.descriptorJson.size()))},{"definitions",definitions},{"baseIntent",editsJson(c.baseIntent,true)},
        {"inferenceCallsUsed",int(c.inferenceCallsUsed)},{"providerStatus",int(c.providerStatus)},{"httpStatus",c.httpStatus}});
}
ProposalStatus capturedAdmission(const ProjectDocument& source,const GenerationRequest& request,const ProposalEdits& edits,ProjectDocument& output)
{return request.catalog?admitProposal(source,request.policy,edits,*request.catalog->authority,output):admitProposal(source,request.policy,edits,output);}
juce::var exactRacksJson(const ProposalEdits& edits)
{
    juce::Array<juce::var> operations;
    for(const auto& edit:edits.racks)operations.add(object({{"kind",int(edit.kind)},{"slot",int(edit.slot)},{"destination",int(edit.destination)},{"position",int(edit.position)},{"effect",int(edit.effect)},{"allowReuse",edit.allowReuse}}));
    return operations;
}
juce::var metricsJson(const AudioMeasurements& m)
{juce::Array<juce::var> bands;for(auto v:m.bandFractions)bands.add(v);return object({{"finite",m.finite},{"clippedSamples",juce::int64(m.clippedSamples)},{"peak",m.peak},{"rms",m.rms},{"onsetSeconds",m.onsetSeconds?juce::var(*m.onsetSeconds):juce::var{}},{"activityEndSeconds",m.activityEndSeconds},{"decaySeconds",m.decaySeconds},{"bandFractions",bands},{"tailActiveAtEnd",m.tailActiveAtEnd}});}
juce::var callbacksJson(const Workflow& w)
{return object({{"count",juce::int64(w.callbacks)},{"nonfinite",juce::int64(w.nonfinite)},{"peak",w.peak},{"maximumCallbackUs",w.maximumCallbackUs},{"qualification","diagnostic contention observation; no realtime/DAW deadline claim"}});}
bool image(Workflow& w,Exports& output,const char* name)
{juce::MemoryBlock physical;juce::MemoryOutputStream stream(physical,false);const auto picture=w.editor->createComponentSnapshot(w.editor->getLocalBounds(),true);return picture.isValid()&&juce::PNGImageFormat().writeImageToStream(picture,stream)&&output.save(name,physical);}
bool hasGeneratedBassPattern(const GenerationResult& result)
{return result.candidate&&result.edits.pattern&&result.edits.pattern->noteCount>0&&result.candidate->pattern==*result.edits.pattern;}
bool Stage::captureRequest(Workflow& w)
{
    const auto* current=w.processor->aiCurrentRequest();const auto* source=w.processor->aiOriginalSource();if(!current||!source)return false;request=*current;original=*source;
    juce::MemoryBlock wire,again;auto decodedStorage=std::make_unique<GenerationRequest>();auto& decoded=*decodedStorage;juce::var catalog;
    submittedLocal=InstrumentAiTestAccess::localExport(*w.processor);
    if(!encodeRequestArtifact(request,submittedLocal.get(),wire)||!decodeRequestArtifact(bytes(wire),submittedLocal.get(),decoded)
        ||!encodeRequestArtifact(decoded,submittedLocal.get(),again)||wire!=again
        ||!catalogFor(request.source,usesOwnedPreview(request)?CatalogRendering::ownedLocal:CatalogRendering::companionPcm,catalog))return false;
    const auto* controls=catalog["controls"].getArray();if(!controls||controls->size()!=int(parameterCount)||int(catalog["limits"]["parameters"])!=int(parameterCount))return false;
    if(request.catalog){juce::var inventory;if(!parseStrictJson(request.catalog->authority->descriptorJson,inventory,maximumCatalogDescriptorBytes)||!bool(inventory["complete"]))return false;catalog.getDynamicObject()->setProperty("rack_catalog",inventory);}
    constexpr const char* scopes[]{"sound","pattern","both"};
    auto metadata=object({{"version",1},{"tag",tagJson(request.tag)},{"instruction",juce::String::fromUTF8(request.instruction.data(),int(request.instruction.size()))},{"model",request.model},
        {"policy",object({{"scope",scopes[int(request.policy.scope)]},{"allowMacroChanges",request.policy.allowMacroChanges},{"allowSlotReuse",request.policy.allowSlotReuse},{"requirePattern",request.policy.requirePattern}})},
        {"context",object({{"role",juce::String(request.role)},{"key",juce::String(request.key)},{"scale",juce::String(request.scale)},{"capturedBpm",request.capturedBpm?juce::var(*request.capturedBpm):juce::var{}},{"capturedPhrase",request.capturedPhrase?phraseJson(*request.capturedPhrase):juce::var{}}})}});
    metadata.getDynamicObject()->setProperty("wireMode",submittedLocal?"owned_local":"companion_pcm");
    if(request.catalog)metadata.getDynamicObject()->setProperty("catalog",catalogContextJson(*request.catalog));
    return output.text("instruction.txt",juce::String::fromUTF8(request.instruction.data(),int(request.instruction.size())))&&output.save("request.bin",wire)&&output.text("request.json",json(metadata))&&output.text("catalog.json",json(catalog))&&output.text("schema.json",json(request.catalog?proposalSchema(request.policy,*request.catalog->authority):proposalSchema(request.policy)))&&saveProject(output,"source.airackproject",request.source)&&saveProject(output,"original.airackproject",*original)&&output.text("stage-pending.json",json(object({{"version",1},{"tag",tagJson(request.tag)},{"success",false},{"state","pending"}})));
}
bool wav(Exports& output,const PreviewAudio& audio)
{
    juce::MemoryBlock physical;std::unique_ptr<juce::OutputStream> stream=std::make_unique<juce::MemoryOutputStream>(physical,false);juce::WavAudioFormat format;
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(audio.sampleRate).withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    const float* channels[]{audio.left.data(),audio.right.data()};if(!writer||!writer->writeFromFloatArrays(channels,2,int(audio.left.size()))||!writer->flush())return false;writer.reset();
    if(!output.save("preview.wav",physical))return false;auto input=output.root.getChildFile("preview.wav").createInputStream();if(!input)return false;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(input.release(),true));
    if(!reader||!reader->usesFloatingPointData||reader->bitsPerSample!=32||reader->numChannels!=2||reader->sampleRate!=audio.sampleRate||reader->lengthInSamples!=juce::int64(audio.left.size()))return false;
    juce::AudioBuffer<float> back(2,int(audio.left.size()));return reader->read(&back,0,back.getNumSamples(),0,true,true)&&!std::memcmp(back.getReadPointer(0),audio.left.data(),audio.left.size()*4)&&!std::memcmp(back.getReadPointer(1),audio.right.data(),audio.right.size()*4);
}
bool Stage::captureResult(Workflow& w)
{
    const auto* current=w.processor->aiCurrentRequest();const auto* proposal=w.processor->aiProposal();const auto* cumulative=w.processor->aiIntendedEdits();const auto* source=w.processor->aiOriginalSource();
    if(!current||!completedRequest(request,*current)||!proposal||!cumulative||!source||!original||*source!=*original||!proposal->candidate)return false;
    request=*current;finalLocal=InstrumentAiTestAccess::localExport(*w.processor);
    if(usesOwnedPreview(request)&&(!finalLocal||!finalLocal->accepted))return false;
    result=*proposal;intent=*cumulative;auto childCandidateStorage=std::make_unique<ProjectDocument>();auto& childCandidate=*childCandidateStorage;auto aggregateStorage=std::make_unique<ProjectDocument>();auto& aggregate=*aggregateStorage;auto decodedStorage=std::make_unique<GenerationResult>();auto& decoded=*decodedStorage;juce::MemoryBlock wire,again;AudioMeasurements measured;
    if(capturedAdmission(request.source,request,result->edits,childCandidate)!=ProposalStatus::accepted||childCandidate!=*result->candidate||capturedAdmission(*original,request,*intent,aggregate)!=ProposalStatus::accepted||aggregate!=*result->candidate||!encodeResultArtifact(request,*result,finalLocal.get(),wire)||!decodeResultArtifact(bytes(wire),request,finalLocal.get(),result->audio,decoded)||!encodeResultArtifact(request,decoded,finalLocal.get(),again)||wire!=again)return false;
    const auto& audio=result->audio;if(measurePreview(audio.left,audio.right,audio.bodyFrames,measured)!=RenderStatus::ready||json(metricsJson(measured))!=json(metricsJson(result->measurements)))return false;
    ProposalEdits parsed;const auto composed=json(editsJson(*intent,request.catalog.has_value()));const bool decodedIntent=request.catalog?decodeProposal(composed.toStdString(),request.policy,*request.catalog->authority,parsed):decodeProposal(composed.toStdString(),request.policy,parsed);
    if(!decodedIntent||capturedAdmission(*original,request,parsed,aggregate)!=ProposalStatus::accepted||aggregate!=*result->candidate)return false;
    if(request.catalog){juce::MemoryBlock finalWire,finalAgain;auto finalDecodedStorage=std::make_unique<GenerationRequest>();auto& finalDecoded=*finalDecodedStorage;
        if(!encodeRequestArtifact(request,finalLocal.get(),finalWire)||!decodeRequestArtifact(bytes(finalWire),finalLocal.get(),finalDecoded)||!encodeRequestArtifact(finalDecoded,finalLocal.get(),finalAgain)||finalAgain!=finalWire
            ||!output.save("final-request.bin",finalWire)||!output.text("final-catalog-authority.json",json(catalogContextJson(*request.catalog))))return false;}
    const auto native=object({{"version",1},{"wireMode",finalLocal?"owned_local":"companion_pcm"},{"pcmInResultWire",finalLocal==nullptr},{"tag",tagJson(result->tag)},{"generationStatus",int(result->status)},{"proposalStatus",int(result->proposalStatus)},{"renderStatus",int(result->renderStatus)},{"providerStatus",int(result->providerStatus)},{"httpStatus",result->httpStatus},{"inferenceCalls",int(result->inferenceCalls)},{"sampleRate",audio.sampleRate},{"frames",juce::int64(audio.left.size())},{"bodyFrames",juce::int64(audio.bodyFrames)},{"tailFrames",juce::int64(previewTailFrames)},{"phraseTruncated",audio.phraseTruncated},{"measurements",metricsJson(measured)},
        {"auditionContext",object({{"bpm",audio.context.bpm},{"phraseSource",int(audio.context.phraseSource)},{"tempoSource",int(audio.context.tempoSource)},{"phrase",phraseJson(audio.context.phrase)}})},{"callbacks",callbacksJson(w)},{"childAdmissionAgainst","source.airackproject"},{"cumulativeAdmissionAgainst","original.airackproject"}});
    auto& diff=widget<juce::TextEditor>(*w.editor,"ai.diff");
    return diff.isReadOnly()&&output.save("result.bin",wire)&&saveProject(output,"candidate.airackproject",*result->candidate)&&output.text("result-edits.json",json(editsJson(result->edits,request.catalog.has_value())))&&output.text("cumulative-intent.json",composed)&&output.text("rack-operations.json",json(object({{"version",1},{"result",exactRacksJson(result->edits)},{"cumulative",exactRacksJson(*intent)},{"policyAllowSlotReuse",request.policy.allowSlotReuse}})))&&output.save("preview-left.f32",juce::MemoryBlock(audio.left.data(),audio.left.size()*4))&&output.save("preview-right.f32",juce::MemoryBlock(audio.right.data(),audio.right.size()*4))&&wav(output,audio)&&output.text("native.json",json(native))&&output.text("ai.diff.txt",diff.getText())&&image(w,output,"proposal.png");
}
bool committedArtifacts(Workflow& w,Exports& output,const ProjectDocument& original,const ProjectDocument& candidate,bool manual)
{
    auto& p=*w.processor;if((*snapshotOwned(p))!=candidate)return false;
    const auto control=index("output.gain_db");const auto expectedGain=p.control(control).convertFrom0to1(candidate.sound.values[control]);
    const auto refreshStart=GetTickCount64();double displayed=0;bool matched=false;
    do
    {
        displayed=widget<juce::Slider>(*w.editor,"output.gain_db").getValue();matched=std::isfinite(displayed)&&std::abs(displayed-expectedGain)<.000001;
        if(matched||GetTickCount64()-refreshStart>=250)break;w.pump();
    }while(true);
    const auto refreshMs=GetTickCount64()-refreshStart;
    if(!matched||refreshMs>250||!saveProject(output,"committed.airackproject",candidate)||!image(w,output,"committed.png"))return false;
    if(w.previewOutput[0].size()!=512||w.previewOutput[1].size()!=512||!output.save("preview-through-processor-left.f32",juce::MemoryBlock(w.previewOutput[0].data(),512*4))||!output.save("preview-through-processor-right.f32",juce::MemoryBlock(w.previewOutput[1].data(),512*4)))return false;
    juce::MemoryBlock state,again;p.getStateInformation(state);auto decodedStorage=std::make_unique<ProjectDocument>();auto& decoded=*decodedStorage;
    if(!decodeProjectState(state.getData(),state.getSize(),decoded)||decoded!=candidate||!encodeProjectState(decoded,again)||state!=again||!output.save("state.bin",state))return false;
    ClientOptions offline;offline.allowLaunch=false;auto recalled=std::make_unique<InstrumentProcessor>(offline);
    if(!recalled->restoreProjectState(state.getData(),int(state.getSize()))||(*snapshotOwned(*recalled))!=candidate||!saveProject(output,"reloaded.airackproject",(*snapshotOwned(*recalled))))return false;
    bool manualUndo=false,notesEditable=false,midiExact=false;
    if(manual)
    {
        const auto n=index("output.gain_db");const auto value=candidate.sound.values[n]==.42f?.41f:.42f;
        if(!p.beginParameterGesture(n)||!p.setParameterGestureValue(n,value)||!p.endParameterGesture(n)||(*snapshotOwned(p)).sound.values[n]!=value||!saveProject(output,"manual.airackproject",(*snapshotOwned(p)))||!p.undoSoundEdit()||(*snapshotOwned(p))!=candidate||!saveProject(output,"manual-undone.airackproject",(*snapshotOwned(p))))return false;
        manualUndo=true;
    }
    else
    {
        if(candidate.pattern.noteCount==0)return false;
        const auto file=output.root.getChildFile("committed.mid");juce::MemoryBlock expected,physical;
        if(file.exists()||!p.exportPattern(file)||!encodePatternMidi(candidate.pattern,expected)||!file.loadFileAsData(physical)||physical!=expected)return false;midiExact=true;
        auto pattern=candidate.pattern;
        pattern.notes[0].velocity=pattern.notes[0].velocity==90?91:90;
        auto currentStorage=std::make_unique<ProjectDocument>();auto& current=*currentStorage;std::uint64_t revision;
        if(!recalled->tryProjectSnapshot(current,revision)||!recalled->applyPatternEdit(pattern,revision)||(*snapshotOwned(*recalled)).pattern!=pattern||!saveProject(output,"manual-notes.airackproject",(*snapshotOwned(*recalled)))||!recalled->undoSoundEdit()||(*snapshotOwned(*recalled))!=candidate)return false;notesEditable=true;
    }
    if(!p.undoSoundEdit()||(*snapshotOwned(p))!=original||!saveProject(output,"undo.airackproject",(*snapshotOwned(p))))return false;
    return output.text("apply-report.json",json(object({{"version",1},{"committedMatchesCandidate",true},{"applyBeforeAudioAckCommitted",false},{"previewStopAcknowledged",true},{"previewProcessorCapturedFrames",512},{"delayedApplyCount",1},{"stateReloadExact",true},{"manualTweakOwnUndo",manualUndo},{"notesEditableOnIndependentReload",notesEditable},{"physicalMidiExact",midiExact},{"oneAiUndoReturnsOriginal",true},{"committedControlId","output.gain_db"},{"committedControlExpected",expectedGain},{"committedControlDisplayed",displayed},{"committedControlMatched",matched},{"committedControlRefreshMs",juce::int64(refreshMs)},{"callbacks",callbacksJson(w)}})));
}
juce::var statusJson(Workflow& w)
{
    const auto status=w.processor->aiWorkflowStatus();const auto* account=w.processor->aiAccountReply();
    return object({{"connectionPhase",int(status.connection.phase)},{"runtimeStatus",int(status.connection.status)},{"errorCode",status.connection.errorCode},{"proposalPhase",int(w.processor->aiProposalPhase())},{"accountPending",status.accountPending},{"applyPending",status.applyPending},{"message",status.message},
        {"providerStatus",account?juce::var(int(account->providerStatus)):juce::var{}},{"selectedModel",account?juce::var(account->selectedModel):juce::var{}},{"modelVerified",account?account->modelVerified:false},{"callbacks",callbacksJson(w)}});
}
// A private endpoint exercises current backend code without replacing an incumbent helper.
struct OwnedBackend
{
    std::atomic_bool stop{false};juce::String suffix=juce::Uuid().toString();
    std::shared_ptr<CompanionBackend> backend;std::unique_ptr<RuntimeServer> server;std::thread worker;
    OwnedBackend(){
        const auto dirs=companionDirectories();require(dirs.credentials.getFullPathName().isNotEmpty()&&dirs.preferences.getFullPathName().isNotEmpty(),"account_directories_unavailable");
        BackendOptions options;options.credentialDirectory=dirs.credentials;options.preferenceDirectory=dirs.preferences;
        options.openBrowser=[](const juce::String&){return false;};backend=std::make_shared<CompanionBackend>(std::move(options));
        server=std::make_unique<RuntimeServer>([b=backend](const RuntimeJob& j){return b->run(j);},[b=backend](const RuntimeJob& j){b->cancel(j);});
        worker=std::thread([this]{server->run(stop,suffix);});
    }
    ~OwnedBackend(){stop.store(true);if(worker.joinable())worker.join();}
};
int run(const Arguments& arguments)
{
    const bool owned=arguments.action==Arguments::Action::runOwned;
    Exports workflowOutput;if((!owned&&!plainPath(arguments.companion,false))||arguments.output.getParentDirectory()!=hostRoot()||!workflowOutput.create(arguments.output)){std::cerr<<"output_creation_failed\n";return 1;}
    const bool handler=SetConsoleCtrlHandler(consoleControl,TRUE)!=FALSE;
    const auto backend=owned?std::make_unique<OwnedBackend>():nullptr;
    ClientOptions options{{arguments.companion,owned?backend->suffix:juce::String{},owned?arguments.output:juce::File{}},false};Workflow w(options);const auto originalStorage=snapshotOwned(*w.processor);const auto& original=*originalStorage;int completed=0;juce::String failure;Stage* active=nullptr;
    std::array<std::unique_ptr<Stage>,3> stages;constexpr const char* names[]{"pluck","bass","refinement"};constexpr const char* instructions[]{pluck,bass,child};
    try
    {
        require(handler,"console_handler_failed");require(saveProject(workflowOutput,"initial.airackproject",original),"initial_export_failed");require(w.open(),"account_or_helper_unavailable");
        const auto* account=w.processor->aiAccountReply();juce::MemoryBlock accountWire;require(account&&encodeAccountReply(*account,accountWire)&&workflowOutput.save("account.bin",accountWire),"public_account_export_failed");
        juce::Array<juce::var> models;for(const auto& m:account->models)models.add(object({{"slug",m.slug},{"displayName",m.displayName}}));require(workflowOutput.text("account.json",json(object({{"version",1},{"selectedModel",account->selectedModel},{"modelVerified",account->modelVerified},{"models",models},{"allowLaunch",false}}))),"public_account_export_failed");
        for(int n=0;n<3;++n)
        {
            if(n==0)w.context("lead",1);else if(n==1){require(w.recall(original),"original_restore_failed");w.context("bass",3);}
            stages[n]=std::make_unique<Stage>();active=stages[n].get();require(active->output.create(arguments.output.getChildFile(names[n])),"stage_directory_failed");
            const bool submitted=w.submit(instructions[n],n==2);
            if(!submitted){require(active->output.text("instruction.txt",instructions[n]),"instruction_export_failed");throw std::runtime_error("submission_failed");}
            require(active->captureRequest(w),"request_export_failed");require(w.complete(active->request),interrupted.load()?"cancelled":"generation_failed_or_timeout");require(active->captureResult(w),"result_export_failed");
            if(n==1)require(hasGeneratedBassPattern(*active->result),"requested_pattern_missing");
            if(n==2)require(active->result->candidate->pattern.noteCount>0,"requested_pattern_missing");
            if(n!=1)
            {
                require(w.previewApply(),"preview_or_delayed_apply_failed");require(committedArtifacts(w,active->output,*active->original,*active->result->candidate,n==0),"committed_recall_or_undo_failed");
            }
            require(active->output.text("stage-report.json",json(object({{"version",1},{"success",true},{"stage",names[n]},{"tag",tagJson(active->request.tag)},{"resultAvailable",true},{"committed",n!=1},{"status",statusJson(w)},{"listeningAccepted",false}}))),"stage_report_failed");++completed;active=nullptr;
        }
    }
    catch(const std::exception& e)
    {
        failure=e.what();if(active&&!active->output.root.getFullPathName().isEmpty())active->output.text("stage-report.json",json(object({{"version",1},{"success",false},{"tag",tagJson(active->request.tag)},{"reason",failure},{"resultAvailable",active->result.has_value()},{"failedResultPubliclyRetained",false},{"status",statusJson(w)},{"currentRequestTag",w.processor->aiCurrentRequest()?tagJson(w.processor->aiCurrentRequest()->tag):juce::var{}},{"limit","Failed processor result is not publicly retained; captured child request is authoritative. Restored parent is never exported as child success."}})));
    }
    w.processor->cancelAi();w.callback();w.pump();const bool manualPreserved=(*snapshotOwned(*w.processor))==original;
    const bool finalSaved=saveProject(workflowOutput,"final.airackproject",(*snapshotOwned(*w.processor)));
    const auto report=object({{"version",1},{"success",failure.isEmpty()&&completed==3&&manualPreserved},{"completedStages",completed},{"failure",failure},{"allowLaunch",false},{"output",arguments.output.getFullPathName()},{"companion",arguments.companion.getFullPathName()},{"ownedBackend",owned},{"initialRestored",manualPreserved},{"status",statusJson(w)},{"helperAttribution",owned?"Current backend on owned private endpoint; browser opening disabled":"External helper; driver does not inspect credentials or helper process/private client state"},{"listeningAccepted",false}});
    const bool saved=workflowOutput.text("workflow-report.json",json(report));if(handler)SetConsoleCtrlHandler(consoleControl,FALSE);std::cout<<json(report).toStdString()<<std::endl;
    return !saved||!finalSaved||!failure.isEmpty()||completed!=3||!manualPreserved?interrupted.load()?130:1:0;
}

// Isolated synthetic fixture: no provider, account store, browser, production
// pipe, helper launch, private client API or request retagging.
Frame localSyntheticResult(const RuntimeJob& job,const GenerationRequest& request,const StructuredCall& provider,const CancelCheck& cancelled)
{
    require(job.localPreview&&job.previewExchange,"synthetic_local_pair_missing");
    auto input=std::make_unique<LocalGenerationRequest>();input->request=request;input->local=*job.localPreview;
    unsigned attempts=request.catalog&&request.catalog->phase==CatalogPhase::materialize?request.catalog->inferenceCallsUsed:0;
    std::uint8_t ordinal=0;
    auto generated=std::unique_ptr<LocalGenerationResult>(new LocalGenerationResult(generateLocalProposal(*input,
        [&](const auto& model,auto prompt,const auto& schema){++attempts;return provider(model,prompt,schema);},
        [&](const auto& candidate,const auto& edits,const auto& audition,const auto& trace,const auto& stop){
            auto value=std::make_unique<PreviewEvaluate>();value->edits=edits;value->context=audition;auto& binding=value->binding;
            binding.request=request.tag;binding.root=input->local.root;binding.catalogSnapshot=input->local.catalogSnapshot;binding.job=job.jobId;
            binding.structuralGeneration=request.structuralGeneration;binding.reset=input->local.reset;binding.sourceWord=input->local.sourceWord;
            binding.phase=request.catalog?request.catalog->phase:CatalogPhase::select;binding.providerAttempt=std::uint8_t(attempts);binding.renderOrdinal=++ordinal;
            binding.sourceMap=input->local.sourceMapDigest;PreviewSampleMap staged;
            require(stagePreviewMap(input->local.sourceMap,trace,staged)&&digestPreviewMap(staged,binding.stagedMap)
                &&digestPreviewCandidate(candidate,binding.candidate)&&digestPreviewContext(audition,binding.context),"synthetic_local_binding_failed");
            return job.previewExchange->evaluate(*value,stop);
        },cancelled,job.groupContext.get())));
    Frame frame{MessageKind::resultLocal,job.session,{}};
    require(encodeLocalResult(*input,*generated,frame.payload),"synthetic_local_result_codec_failed");return frame;
}
struct Synthetic
{
    struct State {std::atomic_bool stop{false};std::atomic_int generations{0},accounts{0},cancels{0},providerCalls{0};
        std::mutex mutex;std::vector<GenerationRequest> requests;};
    std::shared_ptr<State> backend=std::make_shared<State>();
    std::atomic_int& generations=backend->generations;std::atomic_int& accounts=backend->accounts;
    std::atomic_int& cancels=backend->cancels;std::atomic_int& providerCalls=backend->providerCalls;
    juce::String suffix=juce::Uuid().toString().removeCharacters("-");
    std::shared_ptr<RuntimeServer> server;std::thread thread;
    explicit Synthetic()
    {
        server=std::make_shared<RuntimeServer>([state=backend](const RuntimeJob& job)
        {
            if(const auto* request=std::get_if<GenerationRequest>(&job.input))
            {
                ++state->generations;{std::lock_guard lock(state->mutex);state->requests.push_back(*request);}
                if(request->instruction=="hold")while(!job.cancellation->cancelled()&&!state->stop.load())Sleep(1);
                const StructuredCall provider=[&,state](const juce::String&,std::string_view,const juce::var&){
                    ++state->providerCalls;rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::connected;response.result.httpStatus=200;
                    if(request->instruction=="failed child"){response.result.status=rack::oauth::Status::networkFailure;response.result.errorCode="synthetic_failure";return response;}
                    ProposalEdits edits;edits.parameters.push_back({"output.gain_db",request->instruction==child?.30f:.36f});
                    const auto rowName=[](const juce::var& row){return row.isArray()?row[3]:row["name"];};
                    edits.characteristics="Labelled synthetic candidate";edits.explanation="Synthetic native self-test only";
                    if(request->policy.scope==RequestScope::both&&request->instruction!=child&&request->instruction!="missing bass")
                    {PatternDocument p;p.meter={1,4};p.noteCount=2;p.notes[0]={0,.125,36,90,1};p.notes[1]={.5,.125,36,95,1};edits.pattern=p;}
                    // The delay comes with the library's Echo Thread rack.
                    if(request->instruction==child){
                        require(request->catalog.has_value(),"synthetic_catalog_capture_missing");juce::var descriptor;require(parseStrictJson(request->catalog->authority->descriptorJson,descriptor,maximumCatalogDescriptorBytes),"synthetic_catalog_descriptor");
                        edits.catalogSelections=request->catalog->baseIntent.catalogSelections;
                        if(const auto* entries=descriptor["entries"].getArray())for(const auto& entry:*entries)if(rowName(entry).toString()=="Echo Thread")
                        {edits.catalogSelections.push_back({7,juce::Uuid(testing::catalogRowId(entry).toString()),std::uint64_t(testing::catalogRowRevision(entry).toString().getLargeIntValue()),testing::catalogRowHash(entry).toString(),{RackRole::fx,masterChain,0}});break;}
                        require(edits.catalogSelections.size()==1,"synthetic_catalog_selection_missing");}
                    if(request->instruction=="catalog export"||request->instruction=="catalog export refinement"){
                        require(request->catalog.has_value(),"synthetic_catalog_capture_missing");juce::var descriptor;require(parseStrictJson(request->catalog->authority->descriptorJson,descriptor,maximumCatalogDescriptorBytes),"synthetic_catalog_descriptor");
                        const auto inventory=descriptor["entries"];const auto* entries=inventory.getArray();require(entries!=nullptr,"synthetic_catalog_inventory");
                        const bool refinement=request->instruction=="catalog export refinement";if(refinement)edits.catalogSelections=request->catalog->baseIntent.catalogSelections;
                        for(const auto& entry:*entries)if(rowName(entry).toString()==(refinement?"Anchor Press":"Lumen Drift")){
                            edits.catalogSelections.push_back({std::uint8_t(refinement?8:7),juce::Uuid(testing::catalogRowId(entry).toString()),std::uint64_t(testing::catalogRowRevision(entry).toString().getLargeIntValue()),testing::catalogRowHash(entry).toString(),{RackRole::fx,masterChain,std::uint8_t(refinement?1:0)}});
                            if(refinement){edits.parameters[0].normalized=.28f;edits.parameters.push_back({"global.master_level",.45f});edits.catalogParameters.push_back({7,0,"reverb.mix",.20f});edits.catalogParameters.push_back({8,0,"compressor.mix",.65f});}
                            else edits.catalogParameters.push_back({7,0,"reverb.mix",.12f});break;}
                        require(edits.catalogSelections.size()==(refinement?2u:1u),"synthetic_catalog_selection_missing");}
                    response.text=json(editsJson(edits,request->catalog.has_value()));auto parsed=std::make_unique<ProposalEdits>();const auto reply=response.text.toStdString();const bool validMock=request->catalog?decodeProposal(reply,request->policy,*request->catalog->authority,*parsed):decodeProposal(reply,request->policy,*parsed);if(request->instruction=="catalog export refinement")std::cout<<"EXPORTER_MOCK_PREFLIGHT valid="<<validMock<<" selections="<<edits.catalogSelections.size()<<" semantic="<<edits.catalogParameters.size()<<std::endl;require(validMock,"synthetic_mock_decode_preflight");return response;
                };
                const CancelCheck cancelled=[token=job.cancellation]{return token&&token->cancelled();};
                if(job.localPreview||job.previewExchange)return localSyntheticResult(job,*request,provider,cancelled);
                const auto r=generateProposal(*request,provider,cancelled);
                if(request->instruction=="catalog export refinement")std::cout<<"EXPORTER_REFINEMENT_RESULT phase="<<int(request->catalog->phase)<<" status="<<int(r.status)<<" proposal="<<int(r.proposalStatus)<<" render="<<int(r.renderStatus)<<" error="<<r.errorCode.toStdString()<<" calls="<<r.inferenceCalls<<std::endl;Frame f{MessageKind::result,job.session,{}};require(encodeResult(*request,r,f.payload),"synthetic_result_codec_failed");return f;
            }
            ++state->accounts;const auto command=std::get<AccountCommand>(job.input);require(command.action!=AccountAction::signIn,"self_test_sign_in_forbidden");
            AccountReply reply;reply.providerStatus=rack::oauth::Status::connected;reply.selectedModel="synthetic-public";reply.modelVerified=false;
            if(command.action==AccountAction::models)reply.models={{"synthetic-public","Synthetic public model"}};
            Frame f{MessageKind::accountResult,job.session,{}};require(encodeAccountReply(reply,f.payload),"synthetic_account_codec_failed");return f;
        },[state=backend](const RuntimeJob& job){++state->cancels;if(job.previewExchange)job.previewExchange->cancel();});
        thread=std::thread([state=backend,server=server,suffix=suffix]{const auto result=server->run(state->stop,suffix);if(result.workerRetained)std::terminate();});
    }
    ~Synthetic(){backend->stop.store(true);if(thread.joinable())thread.join();}
    ClientOptions options(const juce::File& root){return {{juce::File{},suffix,root},false};}
};

// BEGIN FROZEN SELF_TEST
int checks=0,failures=0;
void check(bool ok,const char* expression,int line)
{++checks;if(!ok){++failures;std::cout<<"FAIL line="<<line<<" "<<expression<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void policyExportChecks(Stage& stage,bool required)
{
    CHECK(stage.request.policy.requirePattern==required);
    const auto metadata=juce::JSON::parse(stage.output.root.getChildFile("request.json"));
    CHECK(metadata["policy"]["requirePattern"].isBool());CHECK(bool(metadata["policy"]["requirePattern"])==required);
    const auto schema=juce::JSON::parse(stage.output.root.getChildFile("schema.json"));
    CHECK(json(schema)==json(stage.request.catalog?proposalSchema(stage.request.policy,*stage.request.catalog->authority):proposalSchema(stage.request.policy)));
    if(required){CHECK(schema["properties"]["pattern"]["type"].toString()=="object");CHECK(int(schema["properties"]["pattern"]["properties"]["notes"]["minItems"])==1);}
    else CHECK(schema["properties"]["pattern"]["anyOf"].isArray());
}
void requiredBassChecks(Stage& parent)
{
    CHECK(hasGeneratedBassPattern(*parent.result));
    auto missingStorage=std::make_unique<GenerationResult>(*parent.result);auto& missing=*missingStorage;missing.candidate.reset();CHECK(!hasGeneratedBassPattern(missing));
    missing=*parent.result;missing.edits.pattern.reset();CHECK(!hasGeneratedBassPattern(missing));
    missing=*parent.result;missing.edits.pattern=PatternDocument{};CHECK(!hasGeneratedBassPattern(missing));
    missing=*parent.result;missing.candidate->pattern.notes[0].pitch=37;CHECK(!hasGeneratedBassPattern(missing));
}
void committedEvidenceChecks(Stage& refined,const ProjectDocument& cumulative,InstrumentProcessor& p)
{
    const auto applied=juce::JSON::parse(refined.output.root.getChildFile("apply-report.json"));
    CHECK(applied["committedControlId"].toString()=="output.gain_db"&&bool(applied["committedControlMatched"]));
    const auto expectedGain=p.control(index("output.gain_db")).convertFrom0to1(cumulative.sound.values[index("output.gain_db")]);
    CHECK(std::abs(double(applied["committedControlExpected"])-expectedGain)<.000001&&std::abs(double(applied["committedControlDisplayed"])-expectedGain)<.000001);
    CHECK(applied.hasProperty("committedControlRefreshMs")&&double(applied["committedControlRefreshMs"])>=0&&double(applied["committedControlRefreshMs"])<=250);
    auto manualStorage=std::make_unique<ProjectDocument>();auto& manuallyEdited=*manualStorage;juce::MemoryBlock manualState;
    CHECK(refined.output.root.getChildFile("manual-notes.airackproject").loadFileAsData(manualState)&&decodeProjectState(manualState.getData(),manualState.getSize(),manuallyEdited));
    CHECK(cumulative.pattern.noteCount>0&&manuallyEdited.pattern.noteCount==cumulative.pattern.noteCount);
    auto expectedManualStorage=std::make_unique<ProjectDocument>(cumulative);auto& expectedManual=*expectedManualStorage;expectedManual.pattern.notes[0].velocity=91;CHECK(manuallyEdited==expectedManual);
}
void missingBassChecks(const juce::File& root,Synthetic& missingService)
{
    std::cout<<"GROUP required_bass_refusal"<<std::endl;
    const auto priorGenerations=missingService.generations.load();
    Workflow missingWorkflow(missingService.options(root),testing::isolatedRackRuntime());auto& local=*missingWorkflow.processor;const auto beforeStorage=snapshotOwned(local);const auto& before=*beforeStorage;
    CHECK(missingWorkflow.open());missingWorkflow.context("bass",3);
    missingWorkflow.requirePattern(false);
    CHECK(!missingWorkflow.requiresPattern());CHECK(missingWorkflow.submit("missing bass"));
    auto missingStage=std::make_unique<Stage>();auto& missing=*missingStage;CHECK(missing.output.create(root.getChildFile("missing-bass")));CHECK(missing.captureRequest(missingWorkflow));CHECK(missingWorkflow.complete(missing.request));CHECK(missing.captureResult(missingWorkflow));
    if(missing.result)
    {
        CHECK(missing.request.policy.scope==RequestScope::both&&!missing.result->edits.pattern&&missing.result->candidate->pattern.noteCount==0);
        const bool accepted=hasGeneratedBassPattern(*missing.result);
        if(accepted)
        {
            CHECK(missingWorkflow.submit(child,true));const auto nextStorage=std::make_unique<GenerationRequest>(*local.aiCurrentRequest());const auto& next=*nextStorage;CHECK(missingWorkflow.complete(next));CHECK(missingWorkflow.previewApply());
        }
        CHECK(!accepted);CHECK(missingService.generations.load()==priorGenerations+1);CHECK((*snapshotOwned(local))==before);
        CHECK(missingWorkflow.previewOutput[0].empty()&&missingWorkflow.previewOutput[1].empty()&&!local.aiPreviewPlaying()&&!local.aiWorkflowStatus().applyPending);
        CHECK(local.aiCurrentRequest()&&local.aiCurrentRequest()->tag==missing.request.tag);
        CHECK(missing.output.root.getChildFile("result.bin").exists()&&missing.output.root.getChildFile("candidate.airackproject").exists());
    }
}
void catalogRefinementRetentionChecks(Workflow& workflow,Synthetic& service,const juce::File& root,const ProjectDocument& original)
{
    std::cout<<"GROUP catalog_refinement_retains_captured_intent"<<std::endl;
    const auto transports=service.generations.load(),calls=service.providerCalls.load();CHECK(workflow.submit("catalog export refinement",true));
    auto refinedStorage=std::make_unique<Stage>();auto& refined=*refinedStorage;CHECK(refined.output.create(root.getChildFile("catalog-refinement")));CHECK(refined.captureRequest(workflow));
    auto initialStorage=std::make_unique<GenerationRequest>(refined.request);const auto& initial=*initialStorage;CHECK(initial.catalog&&initial.catalog->phase==CatalogPhase::select);
    if(!initial.catalog||!initial.catalog->authority)return;
    CHECK(initial.catalog->baseIntent.catalogSelections.size()==1&&initial.catalog->baseIntent.catalogSelections[0].localId==7);
    CHECK(initial.catalog->baseIntent.catalogParameters.size()==1&&initial.catalog->baseIntent.catalogParameters[0].normalized==.12f&&initial.catalog->authority->definitions.size()==1);
    CHECK(workflow.complete(initial));CHECK(refined.captureResult(workflow));CHECK(service.generations.load()==transports+2&&service.providerCalls.load()==calls+1);
    if(!refined.result||!refined.intent||!refined.request.catalog)return;
    auto finalStorage=std::make_unique<GenerationRequest>(refined.request);const auto& final=*finalStorage;CHECK(completedRequest(initial,final));CHECK(final.catalog->root==initial.catalog->root&&final.catalog->selectionParent==initial.tag);
    CHECK(final.catalog->baseIntent.catalogSelections.size()==2&&final.catalog->authority->definitions.size()==2);
    const auto& aggregate=final.catalog->baseIntent;
    CHECK(aggregate.catalogSelections[0]==initial.catalog->baseIntent.catalogSelections[0]);
    CHECK(aggregate.catalogParameters.size()==2&&aggregate.catalogParameters[0].selection==7&&aggregate.catalogParameters[0].key=="reverb.mix"&&aggregate.catalogParameters[0].normalized==.20f);
    CHECK(aggregate.parameters.size()==2&&aggregate.parameters[0].id=="output.gain_db"&&aggregate.parameters[0].normalized==.28f&&aggregate.parameters[1].id=="global.master_level");
    // Standalone numeric exporter oracles retain their genuine pre-Tile capture at construction.
    auto historicalLibrary=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
    std::erase_if(historicalLibrary,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
    CHECK(historicalLibrary.size()==26&&std::count(historicalLibrary.begin(),historicalLibrary.end(),tile)==1);
    std::erase(historicalLibrary,tile);CHECK(historicalLibrary.size()==25);
    std::vector<RackDesign> embedded;if(initial.source.sound.personalities)for(const auto& instance:initial.source.sound.personalities->instances)embedded.push_back(instance.design);
    auto historical=std::make_shared<RackCatalogSnapshot>();CHECK(captureRackCatalog(historicalLibrary,embedded,{},*historical));
    juce::var descriptor;CHECK(catalogInventoryFor(*historical,descriptor));
    const auto legacyRequest=[&](const GenerationRequest& seed){
        auto r=std::make_unique<GenerationRequest>();r->tag=seed.tag;r->source=seed.source;r->policy=seed.policy;r->model=seed.model;r->instruction=seed.instruction;
        r->role=seed.role;r->key=seed.key;r->scale=seed.scale;r->capturedPhrase=seed.capturedPhrase;r->capturedBpm=seed.capturedBpm;
        r->structuralGeneration=seed.structuralGeneration;r->namesInUse=seed.namesInUse;
        const auto& captured=*seed.catalog;auto authority=std::make_shared<CatalogAuthority>();authority->root=captured.root;authority->snapshot=historical->id;
        authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
        for(const auto& definition:captured.authority->definitions){RackDesign resolved;CatalogDefinition hydrated;
            require(resolveCatalogDesign(*historical,definition.design,definition.revision,definition.contentSha256,resolved),"historical_definition_not_captured");
            hydrated.design=resolved.id;hydrated.revision=resolved.revision;hydrated.contentSha256=definition.contentSha256;
            require(encodeRackDesign(resolved,hydrated.canonical,RackValidationMode::instanceRecall),"historical_definition_encode_failed");authority->definitions.push_back(std::move(hydrated));}
        CatalogRequestContext context;context.version=captured.version;context.phase=captured.phase;context.root=captured.root;context.selectionParent=captured.selectionParent;
        context.authority=std::move(authority);context.baseIntent=captured.baseIntent;context.inferenceCallsUsed=captured.inferenceCallsUsed;
        context.providerStatus=captured.providerStatus;context.httpStatus=captured.httpStatus;r->catalog=std::move(context);return r;};
    const auto historicalInitial=legacyRequest(initial),historicalFinal=legacyRequest(final);const auto& legacyInitial=*historicalInitial;const auto& legacyFinal=*historicalFinal;
    const auto rejected=[&](const char* name,const GenerationRequest& substituted){std::cout<<"CASE "<<name<<std::endl;juce::MemoryBlock wire;CHECK(encodeRequest(substituted,wire));CHECK(!completedRequest(legacyInitial,substituted));};
    auto reducedStorage=std::make_unique<GenerationRequest>(legacyFinal);auto& reduced=*reducedStorage;auto& reducedIntent=reduced.catalog->baseIntent;
    std::erase_if(reducedIntent.catalogSelections,[](const auto& s){return s.localId==7;});std::erase_if(reducedIntent.catalogParameters,[](const auto& p){return p.selection==7;});
    CHECK(reducedIntent.catalogSelections.size()==1&&reducedIntent.catalogSelections[0].localId==8);
    reducedIntent.catalogSelections[0].destination.position=0;
    auto reducedAuthority=std::make_shared<CatalogAuthority>(*reduced.catalog->authority);std::erase_if(reducedAuthority->definitions,[&](const auto& d){return d.design==legacyInitial.catalog->baseIntent.catalogSelections[0].design;});reduced.catalog->authority=reducedAuthority;
    rejected("dropped retained selection with internally consistent ready result",reduced);
    auto reducedResultStorage=testing::fixtureValue([&]{return generateProposal(reduced,{});});const auto& reducedResult=*reducedResultStorage;CHECK(reducedResult.status==GenerationStatus::ready&&reducedResult.candidate&&reducedResult.inferenceCalls==legacyFinal.catalog->inferenceCallsUsed);
    juce::MemoryBlock reducedWire;auto reducedReadbackStorage=std::make_unique<GenerationResult>();auto& reducedReadback=*reducedReadbackStorage;CHECK(encodeResult(reduced,reducedResult,reducedWire)&&decodeResult(bytes(reducedWire),reduced,reducedReadback)&&reducedReadback.candidate==reducedResult.candidate);
    auto substitutedStorage=std::make_unique<GenerationRequest>(legacyFinal);auto& substituted=*substitutedStorage;std::erase_if(substituted.catalog->baseIntent.catalogParameters,[](const auto& p){return p.selection==7;});rejected("dropped retained semantic tweak",substituted);
    substituted=legacyFinal;auto& moved=substituted.catalog->baseIntent.catalogSelections[0];moved.destination.position=2;rejected("substituted retained destination",substituted);
    substituted=legacyFinal;auto& identity=substituted.catalog->baseIntent.catalogSelections[0];const auto& added=substituted.catalog->baseIntent.catalogSelections[1];identity.design=added.design;identity.revision=added.revision;identity.contentSha256=added.contentSha256;
    substituted.catalog->baseIntent.catalogParameters[0].key="compressor.mix";rejected("substituted retained canonical identity",substituted);
    substituted=legacyFinal;auto missingDefinition=std::make_shared<CatalogAuthority>(*substituted.catalog->authority);missingDefinition->definitions.erase(missingDefinition->definitions.begin());substituted.catalog->authority=missingDefinition;rejected("dropped retained hydrated canonical definition",substituted);
    substituted=legacyFinal;std::erase_if(substituted.catalog->baseIntent.parameters,[](const auto& p){return p.id=="output.gain_db";});rejected("dropped retained ordinary parameter key",substituted);
    auto withMacroStorage=std::make_unique<GenerationRequest>(legacyInitial);auto& withMacro=*withMacroStorage;withMacro.policy.allowMacroChanges=true;withMacro.catalog->baseIntent.macros.push_back({1,"output.gain_db",.1f});
    auto updatedMacroStorage=std::make_unique<GenerationRequest>(legacyFinal);auto& updatedMacro=*updatedMacroStorage;updatedMacro.policy.allowMacroChanges=true;updatedMacro.catalog->baseIntent.macros.push_back({1,"global.master_level",.2f});
    juce::MemoryBlock valid;CHECK(encodeRequest(withMacro,valid)&&encodeRequest(updatedMacro,valid)&&completedRequest(withMacro,updatedMacro));
    updatedMacro.catalog->baseIntent.macros.clear();CHECK(encodeRequest(updatedMacro,valid));CHECK(!completedRequest(withMacro,updatedMacro));
    auto withPatternStorage=std::make_unique<GenerationRequest>(legacyInitial);auto& withPattern=*withPatternStorage;withPattern.policy.scope=RequestScope::both;PatternDocument phrase;phrase.meter={1,4};phrase.noteCount=1;phrase.notes[0]={0,.125,36,90,1};withPattern.catalog->baseIntent.pattern=phrase;
    auto updatedPatternStorage=std::make_unique<GenerationRequest>(legacyFinal);auto& updatedPattern=*updatedPatternStorage;updatedPattern.policy.scope=RequestScope::both;phrase.notes[0].pitch=38;updatedPattern.catalog->baseIntent.pattern=phrase;
    CHECK(encodeRequest(withPattern,valid)&&encodeRequest(updatedPattern,valid)&&completedRequest(withPattern,updatedPattern));updatedPattern.catalog->baseIntent.pattern.reset();CHECK(encodeRequest(updatedPattern,valid));CHECK(!completedRequest(withPattern,updatedPattern));
    auto withRackStorage=std::make_unique<GenerationRequest>(legacyInitial);auto& withRack=*withRackStorage;withRack.catalog->baseIntent.racks.push_back({RackEditKind::addEffect,noSlot,masterChain,2,EffectKind::delay});
    auto appendedRackStorage=std::make_unique<GenerationRequest>(legacyFinal);auto& appendedRack=*appendedRackStorage;appendedRack.catalog->baseIntent.racks=withRack.catalog->baseIntent.racks;appendedRack.catalog->baseIntent.racks.push_back({RackEditKind::addEffect,noSlot,masterChain,3,EffectKind::drive});
    CHECK(encodeRequest(withRack,valid)&&encodeRequest(appendedRack,valid)&&completedRequest(withRack,appendedRack));appendedRack.catalog->baseIntent.racks.erase(appendedRack.catalog->baseIntent.racks.begin());CHECK(encodeRequest(appendedRack,valid));CHECK(!completedRequest(withRack,appendedRack));
    const auto retainedInstance=juce::Uuid("653b4e7b-3f8b-490f-ad29-1dff22d3f601");auto withChoicesStorage=std::make_unique<GenerationRequest>(legacyInitial);auto& withChoices=*withChoicesStorage;withChoices.catalog->baseIntent.reconciliation.detach.push_back(retainedInstance);withChoices.catalog->baseIntent.reconciliation.remove.push_back(retainedInstance);withChoices.catalog->baseIntent.reconciliation.replace.push_back(retainedInstance);withChoices.catalog->baseIntent.reconciliation.controls.push_back({retainedInstance,0,{}});
    auto appendedChoicesStorage=std::make_unique<GenerationRequest>(legacyFinal);auto& appendedChoices=*appendedChoicesStorage;appendedChoices.catalog->baseIntent.reconciliation=withChoices.catalog->baseIntent.reconciliation;
    CHECK(encodeRequest(withChoices,valid)&&encodeRequest(appendedChoices,valid)&&completedRequest(withChoices,appendedChoices));
    for(int choice=0;choice<4;++choice){auto lostStorage=std::make_unique<GenerationRequest>(appendedChoices);auto& lost=*lostStorage;auto& c=lost.catalog->baseIntent.reconciliation;if(choice==0)c.detach.clear();else if(choice==1)c.remove.clear();else if(choice==2)c.replace.clear();else c.controls.clear();CHECK(encodeRequest(lost,valid));CHECK(!completedRequest(withChoices,lost));}
    auto droppedCaptureStorage=std::make_unique<Stage>();auto& droppedCapture=*droppedCaptureStorage;CHECK(droppedCapture.output.create(root.getChildFile("catalog-dropped-capture")));droppedCapture.request=initial;droppedCapture.original=original;droppedCapture.request.catalog->baseIntent.catalogParameters.push_back({7,0,"reverb.size",.17f});
    if(refined.submittedLocal){auto altered=std::make_unique<LocalGenerationRequest>(refined.submittedLocal->input);altered->request=droppedCapture.request;CHECK(encodeLocalRequest(*altered,valid));}
    else CHECK(encodeRequest(droppedCapture.request,valid));CHECK(!droppedCapture.captureResult(workflow));
    CHECK(!droppedCapture.output.root.getChildFile("final-request.bin").exists()&&!droppedCapture.output.root.getChildFile("result.bin").exists());
    CHECK((*snapshotOwned(*workflow.processor))==original);CHECK(workflow.previewApply());CHECK(committedArtifacts(workflow,refined.output,original,*refined.result->candidate,true));CHECK((*snapshotOwned(*workflow.processor))==original);
}
void catalogExporterChecks(Workflow& workflow,Synthetic& service,const juce::File& root)
{
    std::cout<<"GROUP catalog_root_export_and_materialize"<<std::endl;auto originalStorage=snapshotOwned(*workflow.processor);const auto& original=*originalStorage;
    const auto transports=service.generations.load(),calls=service.providerCalls.load();workflow.context("lead",1);std::cout<<"TASK13_BOUNDARY 48 exporter_submit.before"<<std::endl;CHECK(workflow.submit("catalog export"));std::cout<<"TASK13_BOUNDARY 48 exporter_submit.after"<<std::endl;
    auto stageStorage=std::make_unique<Stage>();auto& stage=*stageStorage;CHECK(stage.output.create(root.getChildFile("catalog-export")));std::cout<<"TASK13_BOUNDARY 48 exporter_request_capture.before"<<std::endl;CHECK(stage.captureRequest(workflow));std::cout<<"TASK13_BOUNDARY 48 exporter_request_capture.after"<<std::endl;
    const auto rootTag=stage.request.tag;CHECK(stage.request.catalog&&stage.request.catalog->root==rootTag);
    CHECK(workflow.complete(stage.request));std::cout<<"TASK13_BOUNDARY 48 exporter_result_capture.before"<<std::endl;CHECK(stage.captureResult(workflow));std::cout<<"TASK13_BOUNDARY 48 exporter_result_capture.after"<<std::endl;CHECK(service.generations.load()==transports+2&&service.providerCalls.load()==calls+1);
    if(!stage.result||!stage.intent||!stage.request.catalog)return;
    CHECK(stage.request.tag.requestId>rootTag.requestId&&stage.request.catalog->root==rootTag&&stage.request.catalog->selectionParent==rootTag);
    CHECK(stage.request.catalog->authority->definitions.size()==1&&stage.result->catalog&&stage.result->catalog->root==rootTag);
    CHECK(stage.result->inferenceCalls==1&&stage.intent->catalogSelections.size()==1&&stage.intent->catalogParameters.size()==1);
    auto initialStorage=std::make_unique<GenerationRequest>();auto& initial=*initialStorage;auto finalStorage=std::make_unique<GenerationRequest>();auto& final=*finalStorage;auto recalledStorage=std::make_unique<GenerationResult>();auto& recalled=*recalledStorage;juce::MemoryBlock initialBytes,finalBytes,resultBytes;
    CHECK(stage.output.root.getChildFile("request.bin").loadFileAsData(initialBytes)&&decodeRequestArtifact(bytes(initialBytes),stage.submittedLocal.get(),initial));
    CHECK(stage.output.root.getChildFile("final-request.bin").loadFileAsData(finalBytes)&&decodeRequestArtifact(bytes(finalBytes),stage.finalLocal.get(),final));
    CHECK(initial.tag==rootTag&&final.tag==stage.request.tag&&initial.source==original&&final.source==original);
    auto substitutedStorage=std::make_unique<GenerationRequest>(final);auto& substituted=*substitutedStorage;substituted.catalog->root=final.tag;CHECK(!completedRequest(initial,substituted));substituted=final;substituted.model="substituted-model";CHECK(!completedRequest(initial,substituted));
    CHECK(stage.output.root.getChildFile("result.bin").loadFileAsData(resultBytes)&&decodeResultArtifact(bytes(resultBytes),final,stage.finalLocal.get(),stage.result->audio,recalled));
    auto replayStorage=std::make_unique<ProjectDocument>();auto& replay=*replayStorage;CHECK(admitProposal(original,final.policy,recalled.edits,*final.catalog->authority,replay)==ProposalStatus::accepted&&recalled.candidate&&replay==*recalled.candidate);
    CHECK(!decodeResultArtifact(bytes(resultBytes),initial,stage.submittedLocal.get(),stage.result->audio,recalled));const auto authority=juce::JSON::parse(stage.output.root.getChildFile("final-catalog-authority.json").loadFileAsString());
    CHECK(authority["root"]["requestId"].toString().toStdString()==std::to_string(rootTag.requestId)&&authority["definitions"].getArray()&&authority["definitions"].getArray()->size()==1);
    CHECK((*snapshotOwned(*workflow.processor))==original);catalogRefinementRetentionChecks(workflow,service,root,original);
}
int selfTest()
{
    const auto root=evidenceRoot().getChildFile("synthetic-"+juce::Uuid().toString());
    require(root.createDirectory().wasOk(),"synthetic_fixture_directory_failed");
    std::cout<<"SYNTHETIC_FIXTURE "<<root.getFullPathName()<<std::endl;
    std::cout<<"GROUP arguments_and_artifacts"<<std::endl;
    Arguments a;CHECK(parse({L"--help"},a)&&a.action==Arguments::Action::help);
    CHECK(parse({L"--self-test"},a)&&a.action==Arguments::Action::selfTest);
    CHECK(!parse({L"--help",L"--self-test"},a));CHECK(!parse({L"--run",L"--unknown"},a));
    const auto exe=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    // A clean checkout has no host root until a test creates one.
    CHECK(hostRoot().createDirectory().wasOk());
    const auto fresh=hostRoot().getChildFile(juce::String::fromUTF8("workflow-UTF8-\xc3\xa9-")+juce::Uuid().toString());
    auto args=std::vector<std::wstring>{L"--run",L"--companion",exe.getFullPathName().toWideCharPointer(),L"--output",fresh.getFullPathName().toWideCharPointer()};
    CHECK(parse(args,a)&&a.output==fresh);auto duplicate=args;duplicate.insert(duplicate.end(),{L"--output",fresh.getFullPathName().toWideCharPointer()});CHECK(!parse(duplicate,a));
    CHECK(parse({L"--run-owned",L"--output",fresh.getFullPathName().toWideCharPointer()},a)&&a.action==Arguments::Action::runOwned);
    CHECK(!parse({L"--run-owned",L"--output",L"relative"},a));
    CHECK(!parse({L"--run-owned",L"--output",root.getFullPathName().toWideCharPointer()},a));
    auto relative=args;relative[4]=L"relative";CHECK(!parse(relative,a));auto outside=args;outside[4]=L"C:\\outside-workspace";CHECK(!parse(outside,a));
    auto missing=args;missing[2]=root.getChildFile("absent.exe").getFullPathName().toWideCharPointer();CHECK(!parse(missing,a));
    auto existing=args;existing[4]=hostRoot().getFullPathName().toWideCharPointer();CHECK(!parse(existing,a));
    Exports artifact;const auto unicode=root.getChildFile(juce::String::fromUTF8("artefact-\xc3\xa9"));CHECK(artifact.create(unicode));
    if(artifact.root.exists())
    {const juce::MemoryBlock original("utf8\xc3\xa9",6);CHECK(artifact.save("exact.bin",original));CHECK(!artifact.save("exact.bin",juce::MemoryBlock("changed",7)));juce::MemoryBlock physical;CHECK(unicode.getChildFile("exact.bin").loadFileAsData(physical)&&physical==original);Exports second;CHECK(!second.create(unicode));}
    const auto junction=root.getChildFile("reparse");
    if(CreateSymbolicLinkW(junction.getFullPathName().toWideCharPointer(),hostRoot().getFullPathName().toWideCharPointer(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
    {Exports escape;CHECK(!escape.create(junction.getChildFile("escape")));}
    else std::cout<<"LIMIT reparse creation unavailable; parser walks FILE_ATTRIBUTE_REPARSE_POINT"<<std::endl;

    std::cout<<"GROUP public_editor_workflow"<<std::endl;
    Synthetic service;Workflow workflow(service.options(root),testing::isolatedRackRuntime());auto& p=*workflow.processor;const auto originalStorage=snapshotOwned(p);const auto& original=*originalStorage;
    std::shared_ptr<const RackCatalogSnapshot> available;CHECK(p.captureAvailableRackCatalog(available)&&available&&available->entries.size()==17);
    const auto tile=airack::test::frozenTileArrayRackDesign();if(available){CHECK(std::count_if(available->entries.begin(),available->entries.end(),[&](const auto& row){return row.definition==tile;})==0);
        CHECK(std::count_if(available->entries.begin(),available->entries.end(),[&](const auto& row){return row.definition.id==tile.id;})==0);
        for(const auto& design:factoryRackDesigns())CHECK(std::count_if(available->entries.begin(),available->entries.end(),[&](const auto& row){return row.definition==design;})==1);}
    CHECK(service.accounts.load()==0&&service.generations.load()==0);CHECK(workflow.open());
std::cout<<"TASK13_BOUNDARY 48 catalogExporterChecks.before"<<std::endl;    catalogExporterChecks(workflow,service,root);std::cout<<"TASK13_BOUNDARY 48 catalogExporterChecks.after"<<std::endl;
    if(p.aiAccountReply())
    {
        CHECK(p.aiAccountReply()->models.size()==1&&!p.aiAccountReply()->modelVerified);CHECK(service.accounts.load()==2);
        workflow.context("lead",1);CHECK(workflow.submit(pluck));auto task13Next1136_stageStorage=std::make_unique<Stage>();auto& stage=*task13Next1136_stageStorage;CHECK(stage.output.create(root.getChildFile("01-pluck")));CHECK(stage.captureRequest(workflow));
        CHECK(stage.request.instruction==pluck&&stage.request.policy.scope==RequestScope::sound);CHECK(stage.request.role=="lead"&&stage.request.key=="A"&&stage.request.scale=="minor"&&stage.request.capturedBpm==123);
        policyExportChecks(stage,false);
        CHECK((*snapshotOwned(p))==original);CHECK(workflow.complete(stage.request));CHECK(stage.captureResult(workflow));
        if(stage.result)
        {CHECK(stage.result->tag==stage.request.tag&&stage.original==original);CHECK((*snapshotOwned(p))==original);CHECK(widget<juce::TextEditor>(*workflow.editor,"ai.diff").isReadOnly());CHECK(workflow.previewApply());CHECK((*snapshotOwned(p))==*stage.result->candidate);CHECK(committedArtifacts(workflow,stage.output,original,*stage.result->candidate,true));CHECK((*snapshotOwned(p))==original);}
    }

    std::cout<<"GROUP refinement_apply_and_recall"<<std::endl;
    CHECK(workflow.recall(original));workflow.context("bass",3);CHECK(workflow.submit(bass));auto task13Next1136_parentStorage=std::make_unique<Stage>();auto& parent=*task13Next1136_parentStorage;CHECK(parent.output.create(root.getChildFile("02-bass")));CHECK(parent.captureRequest(workflow));CHECK(workflow.complete(parent.request));CHECK(parent.captureResult(workflow));
    if(parent.result)
    {
        CHECK(parent.request.instruction==bass&&parent.request.policy.scope==RequestScope::both);CHECK((*snapshotOwned(p))==original);const auto parentCandidateStorage=std::make_unique<ProjectDocument>(*parent.result->candidate);const auto& parentCandidate=*parentCandidateStorage;
        policyExportChecks(parent,true);
        requiredBassChecks(parent);
        CHECK(workflow.submit("failed child",true));auto task13Next1136_failedStorage=std::make_unique<Stage>();auto& failed=*task13Next1136_failedStorage;CHECK(failed.output.create(root.getChildFile("failed-child")));CHECK(failed.captureRequest(workflow));CHECK(failed.request.source==(failed.request.catalog?original:parentCandidate));CHECK(!workflow.complete(failed.request));CHECK(!failed.captureResult(workflow));CHECK(!failed.output.root.getChildFile("result.bin").exists());CHECK(p.aiProposal()&&p.aiCurrentRequest()->tag==parent.request.tag);CHECK((*snapshotOwned(p))==original);
        CHECK(workflow.submit(child,true));auto task13Next1136_refinedStorage=std::make_unique<Stage>();auto& refined=*task13Next1136_refinedStorage;CHECK(refined.output.create(root.getChildFile("03-refinement")));CHECK(refined.captureRequest(workflow));CHECK(refined.request.source==(refined.request.catalog?original:parentCandidate)&&refined.request.tag.requestId>parent.request.tag.requestId);CHECK(refined.request.capturedBpm==123);if(refined.request.catalog)CHECK(refined.request.catalog->root==parent.request.catalog->root&&refined.request.catalog->baseIntent.pattern==parentCandidate.pattern);else CHECK(refined.request.capturedPhrase==parentCandidate.pattern);CHECK((*snapshotOwned(p))==original);CHECK(workflow.complete(refined.request));CHECK(refined.captureResult(workflow));
        policyExportChecks(refined,false);
        if(refined.result&&refined.intent)
        {
            CHECK(refined.request.instruction==child&&refined.original==original);CHECK(refined.intent->racks.empty()&&refined.intent->catalogSelections.size()==1);
            const auto& placed=refined.result->candidate->sound.personalities;CHECK(placed&&std::any_of(placed->instances.begin(),placed->instances.end(),[](const auto& i){return i.design.name=="Echo Thread";}));
            CHECK((refined.request.catalog?refined.result->edits.pattern==parentCandidate.pattern:!refined.result->edits.pattern)&&refined.result->candidate->pattern==parentCandidate.pattern&&refined.intent->pattern==parentCandidate.pattern);
            auto cumulativeStorage=std::make_unique<ProjectDocument>();auto& cumulative=*cumulativeStorage;CHECK(capturedAdmission(original,refined.request,*refined.intent,cumulative)==ProposalStatus::accepted&&cumulative==*refined.result->candidate);
            auto physicalStorage=std::make_unique<GenerationResult>();auto& physical=*physicalStorage;juce::MemoryBlock b;CHECK(refined.output.root.getChildFile("result.bin").loadFileAsData(b)&&decodeResultArtifact(bytes(b),refined.request,refined.finalLocal.get(),refined.result->audio,physical));CHECK(physical.edits.catalogSelections.size()==1&&physical.candidate==refined.result->candidate);
            CHECK(workflow.previewApply());CHECK((*snapshotOwned(p))==cumulative);CHECK(committedArtifacts(workflow,refined.output,original,cumulative,false));CHECK((*snapshotOwned(p))==original);
            committedEvidenceChecks(refined,cumulative,p);
            juce::MidiFile midi;auto stream=refined.output.root.getChildFile("committed.mid").createInputStream();CHECK(stream&&midi.readFrom(*stream));CHECK(midi.getNumTracks()==1&&midi.getTimeFormat()==patternMidiTicksPerQuarter);if(midi.getNumTracks()==1){int noteOns=0;for(int n=0;n<midi.getTrack(0)->getNumEvents();++n)if(midi.getTrack(0)->getEventPointer(n)->message.isNoteOn())++noteOns;CHECK(noteOns==cumulative.pattern.noteCount);}
        }
    }

    std::cout<<"GROUP manual_unavailable_and_cancel"<<std::endl;
    CHECK(workflow.recall(original));workflow.context("lead",1);CHECK(workflow.submit("hold"));auto task13Next1136_pendingStorage=std::make_unique<Stage>();auto& pending=*task13Next1136_pendingStorage;CHECK(pending.output.create(root.getChildFile("cancelled")));CHECK(pending.captureRequest(workflow));CHECK(workflow.wait([&]{return service.generations.load()>=5;},clientStartupTimeoutMs));
    workflow.cancel();workflow.callback();workflow.pump();CHECK((*snapshotOwned(p))==original);CHECK(p.aiProposalPhase()==AiProposalPhase::idle);CHECK(!pending.captureResult(workflow));
    const auto calls=workflow.callbacks;workflow.callback();CHECK(workflow.callbacks>calls&&workflow.nonfinite==0);
    ClientOptions absent{{juce::File{},juce::Uuid().toString().removeCharacters("-"),root},false};Workflow offline(absent,testing::isolatedRackRuntime());const auto manualStorage=snapshotOwned(*offline.processor);const auto& manual=*manualStorage;CHECK(!offline.open());
    const auto parameter=index("output.gain_db");CHECK(offline.processor->beginParameterGesture(parameter));CHECK(offline.processor->setParameterGestureValue(parameter,.42f));CHECK(offline.processor->endParameterGesture(parameter));offline.callback();CHECK((*snapshotOwned(*offline.processor)).sound.values[parameter]==.42f&&offline.nonfinite==0);CHECK(offline.processor->undoSoundEdit());CHECK((*snapshotOwned(*offline.processor))==manual);
    missingBassChecks(root,service);
    std::cout<<"RESULT checks="<<checks<<" groups=5 failures="<<failures<<std::endl;return failures?1:0;
}
// END FROZEN SELF_TEST
}
int main()
{
    try
    {
        int count=0;auto** native=CommandLineToArgvW(GetCommandLineW(),&count);require(native!=nullptr,"arguments_unavailable");std::vector<std::wstring> args;for(int n=1;n<count;++n)args.emplace_back(native[n]);LocalFree(native);
        Arguments parsed;if(!parse(args,parsed)){std::cerr<<"invalid_arguments\n";return 2;}
        if(parsed.action==Arguments::Action::selfTest){juce::ScopedJuceInitialiser_GUI gui;return selfTest();}
        if(parsed.action==Arguments::Action::help){std::cout<<"RackSynthAiWorkflow\n--self-test (isolated synthetic only)\n--run --companion ABS_EXISTING_EXE --output ABS_FRESH_WORKSPACE_FIXTURE_DIR\n--run-owned --output ABS_FRESH_WORKSPACE_FIXTURE_DIR (current backend, private endpoint)\nNo sign-in, browser opening or request retry.\n";return 0;}
        juce::ScopedJuceInitialiser_GUI gui;return run(parsed);
    }
    catch(const std::exception& e){std::cerr<<"native_failure "<<e.what()<<std::endl;return 1;}
}
