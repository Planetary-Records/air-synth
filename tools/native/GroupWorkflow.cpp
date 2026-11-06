#include "InstrumentProcessor.h"
#include "CompanionRuntimeServer.h"
#include "AiCatalog.h"
#include "RackLibrary.h"
#include "PatternIO.h"
#include "RackExamples.h"
#include "ScreenDriver.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_formats/juce_audio_formats.h>
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
#include <source_location>
#include <thread>

// Development-only native driver. --run never launches a helper; the labelled
// synthetic service is confined to the frozen --self-test region below.
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
constexpr const char* complementary="Create a complementary upper-register melodic pluck for the selected rolling bassline. Leave room for the bass, use A minor at 123 BPM, and create editable notes.";
constexpr const char* refinement="Make this complementary part darker and tighter; preserve its existing notes.";
std::atomic_bool interrupted=false;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
WireBytes bytes(const juce::MemoryBlock& b){return{static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& [k,v]:fields)o->setProperty(k,v);return o;}
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
juce::File evidenceRoot(){return workspace().getChildFile(".superpowers/sdd/2026-10-06-m5-technical-acceptance/task-1");}
struct Arguments {enum class Action{invalid,help,selfTest,run};Action action=Action::invalid;juce::File companion,peer,output;};
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
bool readProject(const juce::File& file,ProjectDocument& output);
bool parse(const std::vector<std::wstring>& args,Arguments& parsed)
{
 Arguments next;
 if(args.size()==1&&(args[0]==L"--help"||args[0]==L"--self-test")){next.action=args[0]==L"--help"?Arguments::Action::help:Arguments::Action::selfTest;parsed=next;return true;}
 if(args.size()!=7||args[0]!=L"--run")return false;
 std::map<std::wstring,std::wstring> fields;
 for(std::size_t i=1;i<args.size();i+=2)if((args[i]!=L"--companion"&&args[i]!=L"--peer"&&args[i]!=L"--output")||!fields.emplace(args[i],args[i+1]).second)return false;
 if(!fields.contains(L"--companion")||!fields.contains(L"--peer")||!fields.contains(L"--output")||!absolute(fields[L"--companion"],next.companion)||!absolute(fields[L"--peer"],next.peer)||!absolute(fields[L"--output"],next.output))return false;
 if(!next.companion.isAChildOf(workspace())||!plainPath(next.companion,false)||next.companion.getFileExtension().toLowerCase()!=".exe"||!next.peer.isAChildOf(workspace())||next.peer.getFileExtension().toLowerCase()!=".airackproject"||next.output.getParentDirectory()!=hostRoot()||!freshPath(next.output))return false;
 auto decoded=std::make_unique<ProjectDocument>();if(!readProject(next.peer,*decoded))return false;
 next.action=Arguments::Action::run;parsed=next;return true;
}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;Handle()=default;Handle(const Handle&)=delete;~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}};
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
  const juce::String filename(name);if(filename.isEmpty()||filename=="."||filename==".."||filename.containsAnyOf("/\\:*?\"<>|")||filename.endsWithChar('.')||filename.endsWithChar(' ')||rootLease.value==INVALID_HANDLE_VALUE||!plainPath(root,true)||data.getSize()>DWORD(-1))return false;
  const auto file=root.getChildFile(filename);if(file.getParentDirectory()!=root)return false;
  Handle handle;handle.value=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle.value==INVALID_HANDLE_VALUE)return false;DWORD written=0;
  const bool ok=WriteFile(handle.value,data.getData(),DWORD(data.getSize()),&written,nullptr)&&written==data.getSize()&&FlushFileBuffers(handle.value);
  CloseHandle(handle.value);handle.value=INVALID_HANDLE_VALUE;juce::MemoryBlock back;return ok&&plainPath(file,false)&&file.loadFileAsData(back)&&back==data;
 }
 bool text(const char* name,const juce::String& value){return save(name,juce::MemoryBlock(value.toRawUTF8(),value.getNumBytesAsUTF8()));}
};
bool readProject(const juce::File& file,ProjectDocument& output)
{
 if(!plainPath(file,false))return false;Handle h;h.value=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(h.value==INVALID_HANDLE_VALUE)return false;
 BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};if(!GetFileInformationByHandle(h.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(h.value,&size)||size.QuadPart<juce::int64(minimumProjectStateBytes)||size.QuadPart>juce::int64(maximumProjectStateBytes))return false;
 juce::MemoryBlock data(std::size_t(size.QuadPart));DWORD got=0;return ReadFile(h.value,data.getData(),DWORD(data.getSize()),&got,nullptr)&&got==data.getSize()&&plainPath(file,false)&&decodeProjectState(data.getData(),data.getSize(),output);
}
bool saveProject(Exports& output,const char* name,const ProjectDocument& d)
{juce::MemoryBlock data;auto decoded=std::make_unique<ProjectDocument>();return encodeProjectState(d,data)&&decodeProjectState(data.getData(),data.getSize(),*decoded)&&*decoded==d&&output.save(name,data);}
juce::Component* find(juce::Component& c,const juce::String& id)
{if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* r=find(*child,id))return r;return nullptr;}
template<class T>T& widget(juce::Component& root,const char* id)
{auto* r=dynamic_cast<T*>(find(root,id));require(r!=nullptr,"component_missing");return *r;}
bool click(juce::Component& root,const char* id)
{auto& b=widget<juce::Button>(root,id);if(!b.isEnabled()||!b.onClick)return false;b.onClick();return true;}
std::size_t index(const char* id)
{const auto r=parameterRegistry();const auto p=std::find_if(r.begin(),r.end(),[&](const auto& d){return d.id==id;});require(p!=r.end(),"control_missing");return std::size_t(p-r.begin());}
struct Head final:juce::AudioPlayHead
{
 std::int64_t sample=0;
 juce::Optional<PositionInfo> getPosition()const override
 {PositionInfo p;p.setIsPlaying(true);p.setBpm(123);p.setTimeSignature(TimeSignature{4,4});p.setTimeInSamples(sample);p.setPpqPosition(double(sample)/48000*123/60);return p;}
};
struct Node
{
 std::unique_ptr<InstrumentProcessor> p;std::unique_ptr<juce::AudioProcessorEditor> editor;
 Head head;juce::AudioBuffer<float> audio{2,512};juce::MidiBuffer midi;
 std::uint64_t callbacks=0,nonfinite=0;double peak=0,maximumCallbackUs=0;
 std::array<std::vector<float>,2> previewOutput;
 explicit Node(const ClientOptions& o):p(std::make_unique<InstrumentProcessor>(o))
 {p->setPlayHead(&head);p->prepareToPlay(48000,512);midi.ensureSize(128);reopen();}
 ~Node(){editor.reset();p->closeAiSession();p->releaseResources();p->setPlayHead(nullptr);}
 void reopen(){editor.reset(p->createEditor());editor->setSize(1080,680);}
 std::unique_ptr<ProjectDocument> snapshot(std::uint64_t* edit=nullptr)
 {auto d=std::make_unique<ProjectDocument>();std::uint64_t r;require(p->tryProjectSnapshot(*d,r),"snapshot_failed");if(edit)*edit=r;return d;}
 juce::MemoryBlock saved(){juce::MemoryBlock b;p->getStateInformation(b);return b;}
 bool recall(const ProjectDocument& d){juce::MemoryBlock b;return encodeProjectState(d,b)&&p->restoreProjectState(b.getData(),int(b.getSize()));}
 void callback()
 {midi.clear();midi.addEvent(juce::MidiMessage::noteOn(1,72,juce::uint8(55)),0);midi.addEvent(juce::MidiMessage::noteOff(1,72),256);const auto at=std::chrono::steady_clock::now();p->processBlock(audio,midi);maximumCallbackUs=std::max(maximumCallbackUs,std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-at).count());++callbacks;head.sample+=512;for(int c=0;c<2;++c)for(int n=0;n<512;++n){const auto v=audio.getSample(c,n);if(!std::isfinite(v))++nonfinite;else peak=std::max(peak,double(std::abs(v)));}}
};
bool wait(Node& a,Node* b,const std::function<bool()>& done,std::uint64_t timeout=clientGroupOperationTimeoutMs,const std::source_location at=std::source_location::current())
{const auto start=GetTickCount64(),end=start+timeout;std::uint64_t turns=0,longest=0;do{if(done())return true;if(interrupted.load())return false;const auto turn=GetTickCount64();
 std::array<std::uint64_t,5> phases{};
 auto phase=[&](std::size_t index,const auto& action){const auto began=GetTickCount64();action();phases[index]=GetTickCount64()-began;};
 phase(0,[&]{a.callback();});phase(1,[&]{a.p->pollAi();});if(b){phase(2,[&]{b->callback();});phase(3,[&]{b->p->pollAi();});}phase(4,[&]{juce::MessageManager::getInstance()->runDispatchLoopUntil(1);});
 const auto elapsed=GetTickCount64()-turn;longest=std::max(longest,elapsed);++turns;
 if(elapsed>=1000)std::cout<<"WORKFLOW_SLOW_TURN line="<<at.line()<<" a_audio_ms="<<phases[0]<<" a_poll_ms="<<phases[1]<<" b_audio_ms="<<phases[2]<<" b_poll_ms="<<phases[3]<<" dispatch_ms="<<phases[4]<<std::endl;
 }while(GetTickCount64()<end);
 if(done())return true;std::cout<<"  wait at line "<<at.line()<<" not done after "<<GetTickCount64()-start<<" ms, "<<turns<<" turns, the longest "<<longest<<" ms"<<std::endl;return false;}
struct Workflow
{
 std::unique_ptr<Node> peer,requester;std::unique_ptr<ProjectDocument> imported,baseline,original;
 juce::MemoryBlock peerBytes;std::uint64_t peerEdit=0;GroupSelection selection;
 GroupLinkView peerIdentity;
 std::unique_ptr<ProjectDocument> submittedSource;std::uint64_t submittedEdit=0;GroupSelection submittedSelection;
 explicit Workflow(const ClientOptions& o):peer(std::make_unique<Node>(o)),requester(std::make_unique<Node>(o)){}
 bool setup(const ProjectDocument&,const juce::String&,Exports* =nullptr);
 bool peerUnchanged();bool account();bool submit(const char*,bool=false);bool complete(const GenerationRequest&);
 bool previewStop();bool applyAndUndo(Exports&,const ProjectDocument&);
};
struct Stage
{
 Exports output;GroupGenerationCommand command;std::unique_ptr<GroupGenerationCommand> submitted;std::unique_ptr<LocalExport> submittedLocal,finalLocal;std::unique_ptr<ProjectDocument> original;
 std::unique_ptr<GenerationResult> result;std::unique_ptr<ProposalEdits> cumulative;
 explicit Stage(const GroupSelection& s,std::optional<RequestTag> parent={}){command.selection=s;command.parentProposal=parent;}
 bool captureRequest(Workflow&);bool captureResult(Workflow&);
};
bool image(Node&,Exports&,const char*);
bool audioArtifacts(Exports&,const PreviewAudio&);
// IMPLEMENTATION
juce::String uuid(GroupUuid id){return juce::Uuid(id.getRawData()).toString();}
bool expanded(Node& node)
{auto& toggle=widget<juce::Button>(*node.editor,"ai.toggle");if(!toggle.getToggleState()&&toggle.onClick)toggle.onClick();return true;}
// The node's AI unit's screen once its timer has drawn the processor's latest state there.
studio::Screen& screen(Node& node)
{juce::MessageManager::getInstance()->runDispatchLoopUntil(60);auto* s=screenOf(*node.editor,"ai.screen");require(s!=nullptr,"component_missing");return *s;}
// Back on the create page by the toggle of the account, linked or join page.
void compose(Node& node)
{auto& s=screen(node);if(s.page=="connected")click(*node.editor,"ai.account");else if(s.page=="linked"||s.page=="join")click(*node.editor,"ai.linked");}
// LINKED opens the linked page; from the join page it goes there by the mode's page.
bool linked(Node& node)
{auto& s=screen(node);if(s.page=="linked")return true;if(s.page=="join")click(*node.editor,"ai.linked");return click(*node.editor,"ai.linked")&&s.page=="linked";}
void context(Node& node,const char* role)
{compose(node);auto& s=screen(node);require(enterText(s,"ai.role",role)&&enterText(s,"ai.key","A")&&enterText(s,"ai.scale","minor")&&enterText(s,"ai.tempo","123"),"context_entry_refused");}
bool Workflow::setup(const ProjectDocument& input,const juce::String& name,Exports* output)
{
 if(!validateProjectDocument(input)||input.pattern.noteCount==0||!peer->recall(input))return false;
 imported=std::make_unique<ProjectDocument>(input);if(output&&(!saveProject(*output,"imported-peer.airackproject",input)||!saveProject(*output,"setup-before.airackproject",*peer->snapshot())))return false;
 std::uint64_t revision=0;peer->snapshot(&revision);if(!peer->p->setPlaybackMode(PlaybackMode::internalPattern,revision))return false;
 expanded(*peer);expanded(*requester);context(*peer,"bass");context(*requester,"lead");auto& options=screen(*requester);if(!stepTo(options,"ai.scope",2)||!stepTo(options,"ai.requirePattern",1))return false;
 if(!wait(*requester,peer.get(),[&]{return !peer->p->aiWorkflowStatus().accountPending&&!requester->p->aiWorkflowStatus().accountPending&&peer->p->aiWorkflowStatus().connection.phase==ClientPhase::ready&&requester->p->aiWorkflowStatus().connection.phase==ClientPhase::ready;},clientStartupTimeoutMs+clientOperationTimeoutMs))return false;
 if(!linked(*peer)||!enterText(screen(*peer),"group.name",name)||!wait(*requester,peer.get(),[&]{return peer->p->groupWorkflowView().generationReady;}))return false;
 const auto link=peer->p->groupLinkView();if(!link.intent)return false;const auto group=link.intent->group;
 if(!linked(*requester)||!pressItem(screen(*requester),"group.refresh")||!wait(*requester,peer.get(),[&]{const auto view=requester->p->groupWorkflowView();return !view.operationPending&&std::any_of(view.members.begin(),view.members.end(),[&](const auto& m){return m.intent.group==group&&m.intent.member==link.intent->member;});}))return false;
 auto& groups=screen(*requester);if(!pressItem(groups,"ai.page.join")||groups.page!="join")return false;int row=-1,count=0;const auto label=uuid(group).substring(0,8);
 for(int n=0;n<groups.shown.list.size();++n)if(groups.shown.list[n].contains(label)){row=n;++count;}if(count!=1)return false;
 if(!pressItem(groups,"group.join",row)||!wait(*requester,peer.get(),[&]{return requester->p->groupWorkflowView().generationReady&&peer->p->groupWorkflowView().generationReady;}))return false;
 const auto joined=requester->p->groupLinkView();if(!joined.intent||joined.intent->group!=group||joined.intent->member==link.intent->member)return false;
 if(!linked(*requester)||!pressItem(screen(*requester),"group.refresh")||!wait(*requester,peer.get(),[&]{const auto v=requester->p->groupWorkflowView();return !v.operationPending&&std::any_of(v.members.begin(),v.members.end(),[&](const auto& m){return m.intent.member==link.intent->member&&m.runtime==link.runtime;});}))return false;
 auto& peers=screen(*requester);if(!clickHit(peers,hitWithTip(peers,"group.peer",uuid(link.intent->member))))return false;
 selection=requester->p->groupWorkflowView().selection;if(selection.peerCount!=1||selection.members[0]!=link.intent->member||selection.loadRevision!=joined.loadRevision)return false;
 if(!wait(*requester,peer.get(),[&]{return requester->p->groupWorkflowView().generationReady&&!requester->p->groupWorkflowView().operationPending&&!peer->p->groupWorkflowView().operationPending;}))return false;
 baseline=peer->snapshot(&peerEdit);peerBytes=peer->saved();peerIdentity=peer->p->groupLinkView();original=requester->snapshot();if(baseline->sound!=input.sound||baseline->pattern!=input.pattern||baseline->mode!=PlaybackMode::internalPattern)return false;
 if(output){if(!saveProject(*output,"peer-baseline.airackproject",*baseline)||!saveProject(*output,"original-requester.airackproject",*original)||!output->text("setup.json",json(object({{"version",1},{"peerRole","bass"},{"requesterRole","lead"},{"key","A"},{"scale","minor"},{"bpm",123},{"peerMode","internalPattern"},{"soundAndNotesPreserved",true},{"peerRuntime",uuid(link.runtime)},{"peerLoadRevision",juce::int64(link.loadRevision)},{"peerEditRevision",juce::int64(peerEdit)},{"selectedMember",uuid(link.intent->member)},{"group",uuid(group)}}))))return false;}
 return peerUnchanged();
}
bool Workflow::peerUnchanged()
{if(!baseline)return false;std::uint64_t edit=0;const auto now=peer->snapshot(&edit);return *now==*baseline&&edit==peerEdit&&peer->saved()==peerBytes;}
bool Workflow::account()
{
 if(!click(*requester->editor,"ai.account")||!wait(*requester,peer.get(),[&]{const auto* a=requester->p->aiAccountReply();return !requester->p->aiWorkflowStatus().accountPending&&a;} ,clientStartupTimeoutMs+clientOperationTimeoutMs))return false;
 const auto* a=requester->p->aiAccountReply();if(!a||a->status!=RuntimeStatus::ready||a->providerStatus!=rack::oauth::Status::connected||a->models.empty())return false;
 if(std::none_of(a->models.begin(),a->models.end(),[&](const auto& m){return m.slug==a->selectedModel;})){if(!pressItem(screen(*requester),"ai.model",0)||!wait(*requester,peer.get(),[&]{return !requester->p->aiWorkflowStatus().accountPending;},clientOperationTimeoutMs))return false;}
 a=requester->p->aiAccountReply();auto& models=screen(*requester);return a&&a->selectedModel.isNotEmpty()&&wait(*requester,peer.get(),[&]{return requester->p->groupWorkflowView().generationReady&&models.shown.chosen>=0;});
}
bool Workflow::submit(const char* instruction,bool child)
{
 auto& p=*requester->p;const auto producer=peer->p->groupWorkflowView();if(!peerUnchanged()||producer.connection.phase!=ClientPhase::ready||producer.link.phase!=GroupLinkPhase::admitted||producer.link.runtime!=peerIdentity.runtime||producer.link.loadRevision!=peerIdentity.loadRevision||producer.busy||producer.conflicted)return false;
 // The prompt goes in first: a heartbeat starting between the readiness wait and the press would hold the press.
 compose(*requester);if(!enterText(screen(*requester),"ai.prompt",juce::String::fromUTF8(instruction)))return false;
 if(!wait(*requester,peer.get(),[&]{const auto v=p.groupWorkflowView();return v.selection==selection&&v.generationReady&&!v.conflicted&&!v.busy&&(!child||v.peerContextCurrent);} )||!peerUnchanged())return false;
 const auto view=p.groupWorkflowView();submittedSelection=view.selection;auto local=requester->snapshot(&submittedEdit);
 if(child){const auto* proposal=p.aiProposal();if(!proposal||!proposal->candidate)return false;const auto* parent=p.aiCurrentRequest();const auto* initial=p.aiOriginalSource();if(!parent||(parent->catalog&&!initial))return false;submittedSource=std::make_unique<ProjectDocument>(parent->catalog?*initial:*proposal->candidate);}else submittedSource=std::move(local);
 const auto* previous=p.aiCurrentRequest();const auto previousId=previous?previous->tag.requestId:0;
 if(!click(*requester->editor,child?"ai.refine":"ai.generate"))return false;
 const auto* current=p.aiCurrentRequest();return current&&current->tag.runtime==p.aiRuntime()&&current->tag.editRevision==submittedEdit&&current->tag.requestId>previousId&&current->instruction==instruction&&current->source==*submittedSource&&p.aiProposalPhase()==AiProposalPhase::pending;
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
{if(!wait(*requester,peer.get(),[&]{return requester->p->aiProposalPhase()!=AiProposalPhase::pending;},clientOperationTimeoutMs))return false;const auto* r=requester->p->aiCurrentRequest();return r&&completedRequest(request,*r)&&requester->p->aiProposalPhase()==AiProposalPhase::proposed&&requester->p->aiProposal()&&wait(*requester,peer.get(),[&]{return requester->p->groupWorkflowView().peerContextCurrent;});}
bool Workflow::previewStop()
{
 auto before=requester->snapshot();if(!click(*requester->editor,"ai.preview"))return false;requester->callback();for(int c=0;c<2;++c)requester->previewOutput[c].assign(requester->audio.getReadPointer(c),requester->audio.getReadPointer(c)+512);
 if(!requester->p->aiPreviewPlaying()||*requester->snapshot()!=*before||!peerUnchanged()||!click(*requester->editor,"ai.stop"))return false;
 if(!wait(*requester,peer.get(),[&]{return requester->p->aiPreviewStopped();}))return false;return *requester->snapshot()==*before&&peerUnchanged();
}
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
juce::var metricsJson(const AudioMeasurements& m)
{juce::Array<juce::var> bands;for(auto v:m.bandFractions)bands.add(v);return object({{"finite",m.finite},{"clippedSamples",juce::int64(m.clippedSamples)},{"peak",m.peak},{"rms",m.rms},{"onsetSeconds",m.onsetSeconds?juce::var(*m.onsetSeconds):juce::var{}},{"activityEndSeconds",m.activityEndSeconds},{"decaySeconds",m.decaySeconds},{"bandFractions",bands},{"tailActiveAtEnd",m.tailActiveAtEnd}});}
juce::var callbacksJson(const Node& n)
{return object({{"count",juce::int64(n.callbacks)},{"nonfinite",juce::int64(n.nonfinite)},{"peak",n.peak},{"maximumCallbackUs",n.maximumCallbackUs},{"qualification","diagnostic callback observation; no realtime/DAW deadline claim"}});}
juce::var selectionJson(const GroupSelection& s)
{juce::Array<juce::var> members;for(std::size_t n=0;n<s.peerCount;++n)members.add(uuid(s.members[n]));return object({{"loadRevision",juce::int64(s.loadRevision)},{"peerCount",int(s.peerCount)},{"members",members}});}
bool Stage::captureRequest(Workflow& w)
{
 const auto* r=w.requester->p->aiCurrentRequest();const auto* initial=w.requester->p->aiOriginalSource();std::cout<<"WORKFLOW_CAPTURE_REQUEST request_present="<<(r!=nullptr)<<" catalog_present="<<(r&&r->catalog.has_value())<<std::endl;if(!r||!initial||!w.original||!w.submittedSource||r->source!=*w.submittedSource||r->tag.editRevision!=w.submittedEdit||command.selection!=w.submittedSelection||*initial!=*w.original||w.requester->p->groupWorkflowView().selection!=command.selection||!w.peerUnchanged())return false;
 command.request=*r;submitted=std::make_unique<GroupGenerationCommand>(command);submittedLocal=InstrumentAiTestAccess::localExport(*w.requester->p);original=std::make_unique<ProjectDocument>(*initial);juce::MemoryBlock request,group,again;auto decoded=std::make_unique<GroupGenerationCommand>();juce::var catalog;
 auto gate=[&](bool ok,const char* name){if(!ok)std::cout<<"WORKFLOW_CAPTURE_GATE failed="<<name<<" catalog_present="<<r->catalog.has_value()<<" controls="<<(catalog["controls"].isArray()?catalog["controls"].getArray()->size():-1)<<std::endl;return ok;};
 if(!gate(encodeRequestArtifact(*r,submittedLocal.get(),request),"encodeRequest"))return false;
 if(submittedLocal){auto local=std::make_unique<LocalGroupGenerationCommand>();local->input=submittedLocal->input;local->selection=command.selection;local->parentProposal=command.parentProposal;
  auto roundtrip=std::make_unique<LocalGroupGenerationCommand>();
  if(!gate(encodeLocalGroupGeneration(*local,group),"encodeLocalGroupGeneration")||!gate(decodeLocalGroupGeneration(bytes(group),*roundtrip),"decodeLocalGroupGeneration")
   ||!gate(encodeLocalGroupGeneration(*roundtrip,again),"reencodeLocalGroupGeneration")||!gate(roundtrip->input.local==local->input.local&&requestEqual(roundtrip->input.request,command.request)&&roundtrip->selection==command.selection&&roundtrip->parentProposal==command.parentProposal,"localGroupFields"))return false;
 }else if(!gate(encodeGroupGeneration(command,group),"encodeGroupGeneration")||!gate(decodeGroupGeneration(bytes(group),*decoded),"decodeGroupGeneration")||!gate(encodeGroupGeneration(*decoded,again),"reencodeGroupGeneration"))return false;
 if(!gate(again==group,"groupRoundtrip")||!gate(catalogFor(r->source,submittedLocal?CatalogRendering::ownedLocal:CatalogRendering::companionPcm,catalog),"catalogFor")||!gate(catalog["controls"].isArray(),"controlsArray")||!gate(catalog["controls"].getArray()->size()==int(parameterCount),"controlsExtentCurrent"))return false;
 constexpr const char* scopes[]{"sound","pattern","both"};const auto link=w.peer->p->groupLinkView();
 const auto meta=object({{"version",1},{"wireMode",submittedLocal?"owned_local":"companion_pcm"},{"tag",tagJson(r->tag)},{"instruction",juce::String::fromUTF8(r->instruction.data(),int(r->instruction.size()))},{"model",r->model},{"policy",object({{"scope",scopes[int(r->policy.scope)]},{"allowMacroChanges",r->policy.allowMacroChanges},{"allowSlotReuse",r->policy.allowSlotReuse},{"requirePattern",r->policy.requirePattern}})},
  {"context",object({{"role",juce::String(r->role)},{"key",juce::String(r->key)},{"scale",juce::String(r->scale)},{"capturedBpm",r->capturedBpm?juce::var(*r->capturedBpm):juce::var{}},{"capturedPhrase",r->capturedPhrase?phraseJson(*r->capturedPhrase):juce::var{}}})},{"selection",selectionJson(command.selection)},{"parentProposal",command.parentProposal?tagJson(*command.parentProposal):juce::var{}},
  {"preSubmit",object({{"editRevision",juce::int64(w.submittedEdit)},{"selection",selectionJson(w.submittedSelection)},{"sourceMatchesSubmitted",true},{"peerBaselineUnchanged",true}})},{"peer",object({{"runtime",uuid(link.runtime)},{"loadRevision",juce::int64(link.loadRevision)},{"editRevision",juce::int64(w.peerEdit)},{"member",uuid(w.selection.members[0])}})},{"groupEnvelopeAttribution","Canonical reconstruction of public immutable request and selected IDs/explicit parent; not live pipe interception"}});
 return output.text("instruction.txt",juce::String::fromUTF8(r->instruction.data(),int(r->instruction.size())))&&output.save("request.bin",request)&&output.save("group-request.bin",group)&&output.text("request.json",json(meta))&&output.text("catalog.json",json(catalog))&&output.text("schema.json",json(r->catalog?proposalSchema(r->policy,*r->catalog->authority):proposalSchema(r->policy)))&&saveProject(output,"source.airackproject",r->source)&&saveProject(output,"original.airackproject",*initial)&&saveProject(output,"peer-baseline.airackproject",*w.baseline)&&output.text("stage-pending.json",json(object({{"version",1},{"tag",tagJson(r->tag)},{"success",false},{"state","pending"}})));
}
bool Stage::captureResult(Workflow& w)
{
 const auto* current=w.requester->p->aiCurrentRequest();const auto* proposal=w.requester->p->aiProposal();const auto* edits=w.requester->p->aiIntendedEdits();const auto* initial=w.requester->p->aiOriginalSource();
 if(!current||!completedRequest(command.request,*current)||!proposal||proposal->tag!=current->tag||!proposal->candidate||!edits||!initial||!original||*initial!=*original||!w.peerUnchanged())return false;
 command.request=*current;finalLocal=InstrumentAiTestAccess::localExport(*w.requester->p);
 if(usesOwnedPreview(command.request)&&(!finalLocal||!finalLocal->accepted))return false;
 result=std::make_unique<GenerationResult>(*proposal);cumulative=std::make_unique<ProposalEdits>(*edits);auto admitted=std::make_unique<ProjectDocument>();juce::MemoryBlock wire,again;auto decoded=std::make_unique<GenerationResult>();AudioMeasurements measured;
 if((command.request.catalog?admitProposal(command.request.source,command.request.policy,result->edits,*command.request.catalog->authority,*admitted):admitProposal(command.request.source,command.request.policy,result->edits,*admitted))!=ProposalStatus::accepted||*admitted!=*result->candidate||(command.request.catalog?admitProposal(*original,command.request.policy,*cumulative,*command.request.catalog->authority,*admitted):admitProposal(*original,command.request.policy,*cumulative,*admitted))!=ProposalStatus::accepted||*admitted!=*result->candidate||!encodeResultArtifact(command.request,*result,finalLocal.get(),wire)||!decodeResultArtifact(bytes(wire),command.request,finalLocal.get(),result->audio,*decoded)||!encodeResultArtifact(command.request,*decoded,finalLocal.get(),again)||again!=wire||measurePreview(result->audio.left,result->audio.right,result->audio.bodyFrames,measured)!=RenderStatus::ready||json(metricsJson(measured))!=json(metricsJson(result->measurements)))return false;
 ProposalEdits parsed;if(!(command.request.catalog?decodeProposal(json(editsJson(*cumulative,true)).toStdString(),command.request.policy,*command.request.catalog->authority,parsed):decodeProposal(json(editsJson(*cumulative)).toStdString(),parsed))||(command.request.catalog?admitProposal(*original,command.request.policy,parsed,*command.request.catalog->authority,*admitted):admitProposal(*original,command.request.policy,parsed,*admitted))!=ProposalStatus::accepted||*admitted!=*result->candidate)return false;
 if(finalLocal){juce::MemoryBlock finalWire,finalAgain;auto finalDecoded=std::make_unique<GenerationRequest>();
  if(!encodeRequestArtifact(command.request,finalLocal.get(),finalWire)||!decodeRequestArtifact(bytes(finalWire),finalLocal.get(),*finalDecoded)
   ||!encodeRequestArtifact(*finalDecoded,finalLocal.get(),finalAgain)||finalWire!=finalAgain||!output.save("final-request.bin",finalWire))return false;}
 const auto& audio=result->audio;const auto native=object({{"version",1},{"wireMode",finalLocal?"owned_local":"companion_pcm"},{"pcmInResultWire",finalLocal==nullptr},{"tag",tagJson(result->tag)},{"generationStatus",int(result->status)},{"proposalStatus",int(result->proposalStatus)},{"renderStatus",int(result->renderStatus)},{"providerStatus",int(result->providerStatus)},{"httpStatus",result->httpStatus},{"inferenceCalls",int(result->inferenceCalls)},{"sampleRate",audio.sampleRate},{"frames",juce::int64(audio.left.size())},{"bodyFrames",juce::int64(audio.bodyFrames)},{"tailFrames",juce::int64(previewTailFrames)},{"phraseTruncated",audio.phraseTruncated},{"measurements",metricsJson(measured)},
  {"auditionContext",object({{"bpm",audio.context.bpm},{"phraseSource",int(audio.context.phraseSource)},{"tempoSource",int(audio.context.tempoSource)},{"phrase",phraseJson(audio.context.phrase)}})},{"callbacksRequester",callbacksJson(*w.requester)},{"callbacksPeer",callbacksJson(*w.peer)},{"childAdmissionAgainst","source.airackproject"},{"cumulativeAdmissionAgainst","original.airackproject"},{"liveHelperCaptureExported",false}});
 auto& diff=widget<juce::TextEditor>(*w.requester->editor,"ai.diff");return diff.isReadOnly()&&output.save("result.bin",wire)&&saveProject(output,"candidate.airackproject",*result->candidate)&&output.text("result-edits.json",json(editsJson(result->edits,command.request.catalog.has_value())))&&output.text("cumulative-intent.json",json(editsJson(*cumulative,command.request.catalog.has_value())))&&output.text("native.json",json(native))&&output.text("ai.diff.txt",diff.getText())&&audioArtifacts(output,audio)&&image(*w.requester,output,"proposal.png");
}
bool image(Node& n,Exports& output,const char* name)
{if(!n.editor)return false;juce::MemoryBlock data;juce::MemoryOutputStream stream(data,false);const auto picture=n.editor->createComponentSnapshot(n.editor->getLocalBounds(),true);return picture.isValid()&&juce::PNGImageFormat().writeImageToStream(picture,stream)&&output.save(name,data);}
bool audioArtifacts(Exports& output,const PreviewAudio& audio)
{
 if(audio.left.empty()||audio.left.size()!=audio.right.size()||audio.left.size()>std::size_t(INT_MAX)||!output.save("preview-left.f32",juce::MemoryBlock(audio.left.data(),audio.left.size()*4))||!output.save("preview-right.f32",juce::MemoryBlock(audio.right.data(),audio.right.size()*4)))return false;
 juce::MemoryBlock data;std::unique_ptr<juce::OutputStream> stream=std::make_unique<juce::MemoryOutputStream>(data,false);juce::WavAudioFormat format;auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(audio.sampleRate).withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));const float* channels[]{audio.left.data(),audio.right.data()};if(!writer||!writer->writeFromFloatArrays(channels,2,int(audio.left.size()))||!writer->flush())return false;writer.reset();if(!output.save("preview.wav",data))return false;
 auto input=output.root.getChildFile("preview.wav").createInputStream();std::unique_ptr<juce::AudioFormatReader> reader(input?format.createReaderFor(input.release(),true):nullptr);if(!reader||!reader->usesFloatingPointData||reader->bitsPerSample!=32||reader->sampleRate!=audio.sampleRate||reader->numChannels!=2||reader->lengthInSamples!=juce::int64(audio.left.size()))return false;
 juce::AudioBuffer<float> back(2,int(audio.left.size()));return reader->read(&back,0,back.getNumSamples(),0,true,true)&&!std::memcmp(back.getReadPointer(0),audio.left.data(),audio.left.size()*4)&&!std::memcmp(back.getReadPointer(1),audio.right.data(),audio.right.size()*4);
}
bool Workflow::applyAndUndo(Exports& output,const ProjectDocument& candidate)
{
 auto& p=*requester->p;if(!original||!peerUnchanged()||!p.aiPreviewStopped()||*requester->snapshot()!=*original||!click(*requester->editor,"ai.apply")||!p.aiWorkflowStatus().applyPending)return false;
 if(!wait(*requester,peer.get(),[&]{return !p.aiWorkflowStatus().applyPending&&p.aiProposalPhase()==AiProposalPhase::idle;} )||*requester->snapshot()!=candidate||!peerUnchanged()||!saveProject(output,"committed.airackproject",candidate)||!image(*requester,output,"committed.png"))return false;
 const auto gain=index("output.gain_db");const auto expected=p.control(gain).convertFrom0to1(candidate.sound.values[gain]);double displayed=0;const auto began=GetTickCount64();const bool displayedMatches=wait(*requester,peer.get(),[&]{displayed=widget<juce::Slider>(*requester->editor,"output.gain_db").getValue();return std::isfinite(displayed)&&std::abs(displayed-expected)<.000001;},250);const auto refresh=GetTickCount64()-began;if(!displayedMatches||refresh>250)return false;
 juce::MemoryBlock state,again;p.getStateInformation(state);auto decoded=std::make_unique<ProjectDocument>();if(!decodeProjectState(state.getData(),state.getSize(),*decoded)||*decoded!=candidate||!encodeProjectState(*decoded,again)||again!=state||!output.save("state.bin",state))return false;
 if(requester->previewOutput[0].size()!=512||requester->previewOutput[1].size()!=512||!output.save("preview-through-processor-left.f32",juce::MemoryBlock(requester->previewOutput[0].data(),512*4))||!output.save("preview-through-processor-right.f32",juce::MemoryBlock(requester->previewOutput[1].data(),512*4)))return false;
 juce::MemoryBlock midi,physical;const auto midiFile=output.root.getChildFile("committed.mid");if(candidate.pattern.noteCount==0||midiFile.exists()||!p.exportPattern(midiFile)||!encodePatternMidi(candidate.pattern,midi)||!midiFile.loadFileAsData(physical)||physical!=midi||!output.text("applied-notes.json",json(phraseJson(candidate.pattern))))return false;
 const float tweak=candidate.sound.values[gain]==.42f?.41f:.42f;if(!p.beginParameterGesture(gain)||!p.setParameterGestureValue(gain,tweak)||!p.endParameterGesture(gain)||requester->snapshot()->sound.values[gain]!=tweak||!saveProject(output,"manual.airackproject",*requester->snapshot())||!p.undoSoundEdit()||*requester->snapshot()!=candidate||!saveProject(output,"manual-undone.airackproject",*requester->snapshot())||!p.undoSoundEdit()||*requester->snapshot()!=*original||!saveProject(output,"undo.airackproject",*requester->snapshot())||!peerUnchanged()||!saveProject(output,"peer-after.airackproject",*peer->snapshot()))return false;
 auto clone=std::make_unique<Node>(ClientOptions{{juce::File{},juce::Uuid().toString().removeCharacters("-"),output.root},false});if(!clone->recall(candidate)||clone->p->groupLinkView().phase!=GroupLinkPhase::pending||clone->p->groupWorkflowView().connection.phase!=ClientPhase::closed||clone->snapshot()->sound!=candidate.sound||clone->snapshot()->pattern!=candidate.pattern)return false;
 return output.text("apply-report.json",json(object({{"version",1},{"requesterApplyCount",1},{"freshRemoteValidation",true},{"manualTweakOwnUndo",true},{"oneAiUndoReturnsOriginal",true},{"peerEncodedStateExact",true},{"peerEditRevision",juce::int64(peerEdit)},{"recalledGroupPending",true},{"committedControlId","output.gain_db"},{"committedControlExpected",expected},{"committedControlDisplayed",displayed},{"committedControlMatched",displayedMatches},{"committedControlRefreshMs",juce::int64(refresh)},{"callbacksRequester",callbacksJson(*requester)},{"callbacksPeer",callbacksJson(*peer)}})));
}
juce::var statusJson(Workflow& w)
{const auto ai=w.requester->p->aiWorkflowStatus();const auto g=w.requester->p->groupWorkflowView();return object({{"connectionPhase",int(ai.connection.phase)},{"runtimeStatus",int(ai.connection.status)},{"errorCode",ai.connection.errorCode},{"accountPending",ai.accountPending},{"applyPending",ai.applyPending},{"proposalPhase",int(w.requester->p->aiProposalPhase())},{"message",ai.message},{"groupMessage",g.message},{"groupPhase",int(g.link.phase)},{"generationReady",g.generationReady},{"peerContextCurrent",g.peerContextCurrent},{"selection",selectionJson(g.selection)},{"callbacksRequester",callbacksJson(*w.requester)},{"callbacksPeer",callbacksJson(*w.peer)}});}
BOOL WINAPI consoleControl(DWORD event)
{if(event<=CTRL_CLOSE_EVENT||event==CTRL_LOGOFF_EVENT||event==CTRL_SHUTDOWN_EVENT){interrupted=true;return TRUE;}return FALSE;}
int run(const Arguments& a)
{
 auto input=std::make_unique<ProjectDocument>();if(!plainPath(a.companion,false)||!readProject(a.peer,*input)||a.output.getParentDirectory()!=hostRoot()){std::cerr<<"invalid_run_input\n";return 2;}Exports output;if(!output.create(a.output)){std::cerr<<"fresh_output_creation_failed\n";return 1;}
 const bool handler=SetConsoleCtrlHandler(consoleControl,TRUE)!=FALSE;auto w=std::make_unique<Workflow>(ClientOptions{{a.companion,{},{}},false});std::array<std::unique_ptr<Stage>,2> stages;Stage* active=nullptr;int completed=0;juce::String failure;
 try{
  require(handler,"console_handler_failed");require(w->setup(*input,"Complementary "+juce::Uuid().toString(),&output),"group_setup_failed");require(w->account(),"explicit_account_or_model_unavailable");const auto* account=w->requester->p->aiAccountReply();juce::MemoryBlock accountBytes;require(account&&encodeAccountReply(*account,accountBytes)&&output.save("account.bin",accountBytes),"account_export_failed");require(output.text("account.json",json(object({{"version",1},{"selectedModel",account->selectedModel},{"modelVerified",account->modelVerified},{"allowLaunch",false}}))),"account_export_failed");
  constexpr const char* names[]{"01-complementary","02-refinement"};constexpr const char* instructions[]{complementary,refinement};
  for(int n=0;n<2;++n){std::optional<RequestTag> parent;if(n)parent=stages[0]->command.request.tag;stages[n]=std::make_unique<Stage>(w->selection,parent);active=stages[n].get();require(active->output.create(a.output.getChildFile(names[n])),"stage_directory_failed");require(w->peerUnchanged(),"peer_changed_before_submit");require(w->submit(instructions[n],n==1),"submission_failed");require(active->captureRequest(*w),"request_export_failed");require(w->complete(active->command.request),interrupted.load()?"cancelled":"generation_failed_or_timeout");require(active->captureResult(*w),"result_export_failed");require(active->result&&active->result->candidate&&active->result->candidate->pattern.noteCount>0,"requested_pattern_missing");if(n){require(active->command.request.source==(active->command.request.catalog?*w->original:*stages[0]->result->candidate)&&active->command.parentProposal==stages[0]->command.request.tag&&!active->command.request.policy.requirePattern&&*active->original==*stages[0]->original,"refinement_authority_changed");require(active->result->candidate->pattern==stages[0]->result->candidate->pattern,"refinement_did_not_preserve_notes");}require(w->previewStop(),"preview_or_stop_failed");if(!n)w->peer->editor.reset();else{require(w->applyAndUndo(active->output,*active->result->candidate),"fresh_apply_or_undo_failed");w->peer->reopen();require(image(*w->peer,active->output,"peer-reopened.png"),"peer_reopen_export_failed");auto clone=std::make_unique<Node>(ClientOptions{{juce::File{},juce::Uuid().toString().removeCharacters("-"),a.output},false});require(clone->recall(*active->result->candidate)&&clone->p->groupLinkView().phase==GroupLinkPhase::pending,"recall_permission_failed");require(saveProject(active->output,"recalled-pending.airackproject",*clone->snapshot())&&image(*clone,active->output,"recalled-pending.png"),"recall_export_failed");}require(active->output.text("stage-report.json",json(object({{"version",1},{"success",true},{"stage",names[n]},{"tag",tagJson(active->command.request.tag)},{"committed",n==1},{"status",statusJson(*w)},{"listeningAccepted",false}}))),"stage_report_failed");++completed;active=nullptr;}
 }catch(const std::exception& e){failure=e.what();if(active&&!active->output.root.getFullPathName().isEmpty())active->output.text("stage-report.json",json(object({{"version",1},{"success",false},{"tag",tagJson(active->command.request.tag)},{"reason",failure},{"resultAvailable",active->result!=nullptr},{"status",statusJson(*w)},{"limit","Failed processor result is not publicly retained; submitted request is authoritative. Restored parent is never exported as successful child."}})));}
 w->requester->p->cancelAi();w->requester->callback();w->requester->p->pollAi();const bool peerExact=w->peerUnchanged();const bool restored=w->original&&*w->requester->snapshot()==*w->original;const bool finalSaved=saveProject(output,"final.airackproject",*w->requester->snapshot());const auto report=object({{"version",1},{"success",failure.isEmpty()&&completed==2&&peerExact&&restored},{"completedStages",completed},{"failure",failure},{"allowLaunch",false},{"companion",a.companion.getFullPathName()},{"peerInput",a.peer.getFullPathName()},{"output",a.output.getFullPathName()},{"peerEncodedStateExact",peerExact},{"requesterOriginalRestored",restored},{"status",statusJson(*w)},{"helperAttribution","Parent owned PID/time/path/hash observation; no helper process/private credential access"},{"liveHelperPromptOrCaptureExported",false},{"listeningAccepted",false}});const bool saved=output.text("workflow-report.json",json(report));if(handler)SetConsoleCtrlHandler(consoleControl,FALSE);std::cout<<json(report).toStdString()<<'\n';return !saved||!finalSaved||!failure.isEmpty()||completed!=2||!peerExact||!restored?interrupted.load()?130:1:0;
}


// BEGIN FROZEN SELF_TEST
std::atomic_int checks=0,failures=0;int sections=0;std::atomic<const char*> currentSection="";
void check(bool ok,const char* expression,int line)
{++checks;if(!ok){++failures;std::cerr<<currentSection.load()<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){currentSection=name;++sections;std::cout<<name<<'\n';}
std::uint64_t creation(){FILETIME a{},b{},c{},d{};CHECK(GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d));return(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
struct Fixture
{
 juce::File root=evidenceRoot().getChildFile("fixtures").getChildFile(juce::Uuid().toString());
 Fixture(){CHECK(root.isAChildOf(evidenceRoot()));CHECK(root.createDirectory().wasOk());std::cout<<"SYNTHETIC_FIXTURE "<<root.getFullPathName()<<'\n';}
};
std::unique_ptr<ProjectDocument> bassDocument()
{auto n=std::make_unique<Node>(ClientOptions{{juce::File{},"unused",evidenceRoot()},false});auto d=n->snapshot();d->pattern.meter={1,4};d->pattern.noteCount=2;d->pattern.notes[0]={0,.125,36,90,1};d->pattern.notes[1]={.5,.125,43,95,1};d->mode=PlaybackMode::internalPattern;CHECK(validateProjectDocument(*d));return d;}
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
 struct State {std::atomic_bool stop=false,returned=false;std::atomic_int calls=0,accounts=0,providerCalls=0,held=0;
  std::mutex mutex;std::vector<RuntimeJob> jobs;};
 std::shared_ptr<State> backend=std::make_shared<State>();
 std::atomic_bool& returned=backend->returned;std::atomic_int& calls=backend->calls;std::atomic_int& accounts=backend->accounts;
 std::atomic_int& providerCalls=backend->providerCalls;std::atomic_int& held=backend->held;
 Fixture fixture;juce::String suffix=juce::Uuid().toString().removeCharacters("-");
 std::shared_ptr<RuntimeServer> server;std::unique_ptr<CompanionClient> probe=std::make_unique<CompanionClient>();std::thread io;
 Synthetic()
 {
  server=std::make_shared<RuntimeServer>([state=backend](const RuntimeJob& job){
   if(const auto* request=std::get_if<GenerationRequest>(&job.input)){
    ++state->calls;{std::lock_guard lock(state->mutex);state->jobs.push_back(job);}CHECK(job.groupContext!=nullptr);
    if(request->instruction=="synthetic-hold"){++state->held;while(!job.cancellation->cancelled()&&!state->stop.load())std::this_thread::yield();}
    const StructuredCall provider=[&,state](const juce::String&,std::string_view prompt,const juce::var& schema){
     ++state->providerCalls;CHECK(job.groupContext&&validateGroupPerformanceCapture(*job.groupContext));CHECK(prompt.find("linked_context")!=std::string_view::npos);CHECK(json(schema)==json(request->catalog?proposalSchema(request->policy,*request->catalog->authority):proposalSchema(request->policy)));if(json(schema)!=json(request->catalog?proposalSchema(request->policy,*request->catalog->authority):proposalSchema(request->policy)))std::cout<<"WORKFLOW_PROVIDER_SCHEMA_FAILURE catalog_present="<<request->catalog.has_value()<<" legacy_schema_equal=0"<<std::endl;
     rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;
     reply.text=juce::String("{\"version\":1,\"parameter_edits\":[{\"id\":\"output.gain_db\",\"normalized\":")+(request->instruction==refinement?"0.30":"0.36")+"}],\"rack_edits\":[],\"macro_edits\":[],\"pattern\":"+(request->instruction==refinement?juce::String("null"):juce::String("{\"bars\":1,\"meter_numerator\":1,\"meter_denominator\":4,\"grid\":\"sixteenth\",\"swing\":0,\"notes\":[{\"start_beats\":0,\"duration_beats\":0.125,\"pitch\":76,\"velocity\":90,\"channel\":1},{\"start_beats\":0.5,\"duration_beats\":0.125,\"pitch\":79,\"velocity\":95,\"channel\":1}]}"))+",\"characteristics\":\"Synthetic pluck\",\"explanation\":\"Labelled synthetic only\"}";if(request->catalog){auto current=juce::JSON::parse(reply.text);auto* fields=current.getDynamicObject();require(fields!=nullptr,"synthetic_reply_invalid");fields->setProperty("version",2);fields->setProperty("name","Synthetic Pluck");fields->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>()},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));fields->setProperty("catalog_selections",juce::Array<juce::var>());fields->setProperty("catalog_parameter_edits",juce::Array<juce::var>());reply.text=json(current);}return reply;
    };
    const CancelCheck cancelled=[token=job.cancellation]{return token&&token->cancelled();};
    if(job.localPreview||job.previewExchange)return localSyntheticResult(job,*request,provider,cancelled);
    auto result=std::make_unique<GenerationResult>(generateProposal(*request,provider,cancelled,job.groupContext.get()));
    if(result->status!=GenerationStatus::ready)std::cout<<"WORKFLOW_GENERATION_FAILURE status="<<int(result->status)<<" proposal="<<int(result->proposalStatus)<<" render="<<int(result->renderStatus)<<" calls="<<result->inferenceCalls<<std::endl;Frame f{MessageKind::result,job.session,{}};CHECK(encodeResult(*request,*result,f.payload));return f;
   }
   ++state->accounts;const auto command=std::get<AccountCommand>(job.input);CHECK(command.action!=AccountAction::signIn);
   AccountReply r;r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;r.selectedModel="synthetic-public";r.modelVerified=true;r.models={{"synthetic-public","Synthetic native model"}};Frame f{MessageKind::accountResult,job.session,{}};CHECK(encodeAccountReply(r,f.payload));return f;
  },[](const RuntimeJob& job){if(job.previewExchange)job.previewExchange->cancel();});
  io=std::thread([state=backend,server=server,suffix=suffix]{const auto r=server->run(state->stop,suffix);state->returned=true;std::cout<<"SERVER_RETURN status="<<int(r.status)<<" retained="<<r.workerRetained<<" stop="<<state->stop.load()<<'\n';CHECK(!r.workerRetained);CHECK(state->stop.load()&&r.status==RuntimeStatus::ready);});
  CHECK(probe->start(options()));const auto end=GetTickCount64()+clientStartupTimeoutMs;do{probe->poll();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}while(probe->status().phase!=ClientPhase::ready&&!returned.load()&&GetTickCount64()<end);
  // Keep the fixture's service alive across editor construction and client replacement.
  CHECK(probe->status().phase==ClientPhase::ready);CHECK(!probe->session().isNull());CHECK(probe->peer().processId==GetCurrentProcessId()&&probe->peer().creationTime==creation());std::cout<<"STARTUP_WELCOME ready="<<(probe->status().phase==ClientPhase::ready)<<'\n';
 }
 ~Synthetic(){backend->stop=true;probe->close();if(io.joinable())io.join();}
 ClientOptions options()const{return{{juce::File{},suffix,fixture.root},false};}
 std::vector<RuntimeJob> captured(){std::lock_guard lock(backend->mutex);return backend->jobs;}
};
void projectChecks(const juce::File& f,const ProjectDocument& expected)
{auto d=std::make_unique<ProjectDocument>();juce::MemoryBlock b,again;CHECK(f.loadFileAsData(b));CHECK(decodeProjectState(b.getData(),b.getSize(),*d));CHECK(*d==expected);CHECK(encodeProjectState(*d,again)&&again==b);for(std::size_t n=0;n<parameterCount;++n)CHECK(d->sound.values[n]==expected.sound.values[n]);for(std::size_t n=0;n<expected.pattern.noteCount;++n)CHECK(d->pattern.notes[n]==expected.pattern.notes[n]);}
void stageChecks(Stage& s,Workflow& w,bool child)
{
 auto decoded=std::make_unique<GroupGenerationCommand>();auto request=std::make_unique<GenerationRequest>();auto result=std::make_unique<GenerationResult>();juce::MemoryBlock b,again;
 CHECK(s.submitted!=nullptr);if(!s.submitted)return;
 CHECK(s.output.root.getChildFile("request.bin").loadFileAsData(b));CHECK(decodeRequestArtifact(bytes(b),s.submittedLocal.get(),*request));
 CHECK(requestEqual(*request,s.submitted->request));CHECK(encodeRequestArtifact(*request,s.submittedLocal.get(),again)&&again==b);
 b.reset();CHECK(s.output.root.getChildFile("group-request.bin").loadFileAsData(b));
 if(s.submittedLocal){auto local=std::make_unique<LocalGroupGenerationCommand>();CHECK(decodeLocalGroupGeneration(bytes(b),*local));
  CHECK(local->input.local==s.submittedLocal->input.local&&requestEqual(local->input.request,s.submitted->request));
  CHECK(local->selection==w.selection&&local->parentProposal==s.submitted->parentProposal);CHECK(encodeLocalGroupGeneration(*local,again)&&again==b);
 }else{CHECK(decodeGroupGeneration(bytes(b),*decoded));CHECK(decoded->selection==w.selection&&decoded->parentProposal==s.submitted->parentProposal);
  CHECK(decoded->request.tag==s.submitted->request.tag);CHECK(encodeGroupGeneration(*decoded,again)&&again==b);}
 b.reset();CHECK(s.output.root.getChildFile("result.bin").loadFileAsData(b));
 CHECK(decodeResultArtifact(bytes(b),s.command.request,s.finalLocal.get(),s.result->audio,*result));CHECK(result->candidate==s.result->candidate);
 CHECK(encodeResultArtifact(s.command.request,*result,s.finalLocal.get(),again)&&again==b);
 const auto meta=juce::JSON::parse(s.output.root.getChildFile("request.json"));CHECK(meta["policy"]["requirePattern"].isBool()&&bool(meta["policy"]["requirePattern"])==!child);CHECK(int(meta["selection"]["peerCount"])==1);CHECK(meta["parentProposal"].isObject()==child);CHECK(meta["tag"]["runtime"].toString()==w.requester->p->aiRuntime().toString());
 const auto catalog=juce::JSON::parse(s.output.root.getChildFile("catalog.json"));CHECK(catalog["controls"].isArray()&&catalog["controls"].getArray()->size()==int(parameterCount));CHECK(json(juce::JSON::parse(s.output.root.getChildFile("schema.json")))==json(s.command.request.catalog?proposalSchema(s.command.request.policy,*s.command.request.catalog->authority):proposalSchema(s.command.request.policy)));
 projectChecks(s.output.root.getChildFile("source.airackproject"),s.command.request.source);projectChecks(s.output.root.getChildFile("original.airackproject"),*w.original);projectChecks(s.output.root.getChildFile("candidate.airackproject"),*s.result->candidate);
 CHECK(s.result->audio.left.size()==s.result->audio.right.size());juce::MemoryBlock left,right;CHECK(s.output.root.getChildFile("preview-left.f32").loadFileAsData(left));CHECK(s.output.root.getChildFile("preview-right.f32").loadFileAsData(right));CHECK(left.getSize()==s.result->audio.left.size()*4&&right.getSize()==left.getSize());if(left.getSize()==s.result->audio.left.size()*4)for(std::size_t i=0;i<s.result->audio.left.size();++i){CHECK(!std::memcmp(static_cast<const char*>(left.getData())+i*4,&s.result->audio.left[i],4));CHECK(!std::memcmp(static_cast<const char*>(right.getData())+i*4,&s.result->audio.right[i],4));}
 juce::WavAudioFormat format;auto stream=s.output.root.getChildFile("preview.wav").createInputStream();std::unique_ptr<juce::AudioFormatReader> reader(stream?format.createReaderFor(stream.release(),true):nullptr);CHECK(reader&&reader->usesFloatingPointData&&reader->bitsPerSample==32&&reader->numChannels==2&&reader->sampleRate==48000&&reader->lengthInSamples==juce::int64(s.result->audio.left.size()));if(reader){juce::AudioBuffer<float> back(2,int(s.result->audio.left.size()));CHECK(reader->read(&back,0,back.getNumSamples(),0,true,true));for(int c=0;c<2;++c){const auto& expected=c?s.result->audio.right:s.result->audio.left;for(std::size_t i=0;i<expected.size();++i)CHECK(!std::memcmp(back.getReadPointer(c)+i,&expected[i],4));}}
 auto admitted=std::make_unique<ProjectDocument>();CHECK((s.command.request.catalog?admitProposal(s.command.request.source,s.command.request.policy,s.result->edits,*s.command.request.catalog->authority,*admitted):admitProposal(s.command.request.source,s.command.request.policy,s.result->edits,*admitted))==ProposalStatus::accepted&&*admitted==*s.result->candidate);CHECK((s.command.request.catalog?admitProposal(*s.original,s.command.request.policy,*s.cumulative,*s.command.request.catalog->authority,*admitted):admitProposal(*s.original,s.command.request.policy,*s.cumulative,*admitted))==ProposalStatus::accepted&&*admitted==*s.result->candidate);
 CHECK(s.output.root.getChildFile("proposal.png").getSize()>0);CHECK(s.output.root.getChildFile("ai.diff.txt").loadFileAsString().contains("output.gain_db"));CHECK(w.peerUnchanged());
}
void arguments_and_group_exports()
{
 begin(__func__);Fixture fixture;Arguments a;CHECK(parse({L"--help"},a)&&a.action==Arguments::Action::help);CHECK(parse({L"--self-test"},a)&&a.action==Arguments::Action::selfTest);CHECK(!parse({L"--help",L"--self-test"},a));CHECK(!parse({L"--run",L"--unknown"},a));
 auto project=bassDocument();juce::MemoryBlock state;CHECK(encodeProjectState(*project,state));const auto peer=fixture.root.getChildFile("peer.airackproject");CHECK(peer.replaceWithData(state.getData(),state.getSize()));const auto exe=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
 // A clean checkout has no host root until a test creates one.
 CHECK(hostRoot().createDirectory().wasOk());const auto fresh=hostRoot().getChildFile("group-workflow-"+juce::Uuid().toString());
 const std::vector<std::wstring> args{L"--run",L"--companion",exe.getFullPathName().toWideCharPointer(),L"--peer",peer.getFullPathName().toWideCharPointer(),L"--output",fresh.getFullPathName().toWideCharPointer()};CHECK(parse(args,a)&&a.peer==peer&&a.output==fresh);auto duplicate=args;duplicate[3]=L"--companion";CHECK(!parse(duplicate,a));auto relative=args;relative[6]=L"relative";CHECK(!parse(relative,a));auto absent=args;absent[2]=fixture.root.getChildFile("missing.exe").getFullPathName().toWideCharPointer();CHECK(!parse(absent,a));auto missingPeer=args;missingPeer[4]=fixture.root.getChildFile("missing.airackproject").getFullPathName().toWideCharPointer();CHECK(!parse(missingPeer,a));auto existing=args;existing[6]=hostRoot().getFullPathName().toWideCharPointer();CHECK(!parse(existing,a));auto outside=args;outside[6]=L"C:\\outside-workspace";CHECK(!parse(outside,a));
 auto decoded=std::make_unique<ProjectDocument>();CHECK(readProject(peer,*decoded)&&*decoded==*project);const auto corrupt=fixture.root.getChildFile("corrupt.airackproject");state.append("tail",4);CHECK(corrupt.replaceWithData(state.getData(),state.getSize()));CHECK(!readProject(corrupt,*decoded));auto invalidPeer=args;invalidPeer[4]=corrupt.getFullPathName().toWideCharPointer();CHECK(!parse(invalidPeer,a));
 Exports output;CHECK(output.create(fixture.root.getChildFile("exports")));const juce::MemoryBlock payload("utf8\xc3\xa9",6);CHECK(output.save("exact.bin",payload));CHECK(!output.save("exact.bin",juce::MemoryBlock("changed",7)));juce::MemoryBlock physical;CHECK(output.root.getChildFile("exact.bin").loadFileAsData(physical)&&physical==payload);Exports second;CHECK(!second.create(output.root));CHECK(!output.save("../escape.bin",payload));CHECK(saveProject(output,"project.airackproject",*project));if(output.root.exists())projectChecks(output.root.getChildFile("project.airackproject"),*project);
 const auto junction=fixture.root.getChildFile("reparse");if(CreateSymbolicLinkW(junction.getFullPathName().toWideCharPointer(),hostRoot().getFullPathName().toWideCharPointer(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)){Exports escape;CHECK(!escape.create(junction.getChildFile("escape")));}else std::cout<<"LIMIT synthetic symlink creation unavailable; every real path uses reparse walk\n";
}
struct Cases
{
 std::unique_ptr<Synthetic> service=std::make_unique<Synthetic>();std::unique_ptr<Workflow> workflow=std::make_unique<Workflow>(service->options());std::unique_ptr<Stage> parent,child;
};
void two_instance_capture_and_refinement(Cases& c)
{
 begin(__func__);auto input=bassDocument();auto& w=*c.workflow;CHECK(w.setup(*input,"Synthetic two-instance band"));if(!w.baseline||!w.original)return;
 CHECK(w.baseline->sound==input->sound&&w.baseline->pattern==input->pattern&&w.baseline->mode==PlaybackMode::internalPattern);CHECK(w.peer->p->groupLinkView().phase==GroupLinkPhase::admitted);CHECK(w.requester->p->groupLinkView().phase==GroupLinkPhase::admitted);CHECK(w.selection.peerCount==1&&w.selection.members[0]==w.baseline->group->member);CHECK(w.requester->p->groupLinkView().intent->member!=w.baseline->group->member);CHECK(c.service->calls==0);CHECK(w.account());
 c.parent=std::make_unique<Stage>(w.selection);auto& parent=*c.parent;CHECK(parent.output.create(c.service->fixture.root.getChildFile("01-complementary")));CHECK(w.submit(complementary));CHECK(parent.captureRequest(w));CHECK(parent.command.request.instruction==complementary&&parent.command.request.policy.scope==RequestScope::both&&parent.command.request.policy.requirePattern);CHECK(w.requester->saved()!=juce::MemoryBlock{}&&*w.requester->snapshot()==*w.original);CHECK(w.complete(parent.command.request));CHECK(parent.captureResult(w));if(!parent.result)return;
 CHECK(parent.result->candidate&&parent.result->edits.pattern&&parent.result->candidate->pattern.noteCount==2);stageChecks(parent,w,false);CHECK(w.previewStop());CHECK(*w.requester->snapshot()==*w.original);CHECK(w.peerUnchanged());w.peer->editor.reset();CHECK(w.peer->p->groupLinkView().phase==GroupLinkPhase::admitted);
 c.child=std::make_unique<Stage>(w.selection,parent.command.request.tag);auto& child=*c.child;CHECK(child.output.create(c.service->fixture.root.getChildFile("02-refinement")));CHECK(w.submit(refinement,true));CHECK(child.captureRequest(w));CHECK(child.command.request.source==(child.command.request.catalog?*w.original:*parent.result->candidate));if(child.command.request.catalog)CHECK(parent.command.request.catalog&&child.command.request.catalog->root==parent.command.request.catalog->root&&child.command.request.catalog->baseIntent.pattern==parent.result->candidate->pattern);CHECK(child.command.request.policy.scope==RequestScope::both&&!child.command.request.policy.requirePattern);CHECK(child.command.parentProposal==parent.command.request.tag);CHECK(w.complete(child.command.request));CHECK(child.captureResult(w));if(!child.result)return;
 CHECK((child.command.request.catalog?child.result->edits.pattern==parent.result->candidate->pattern:!child.result->edits.pattern)&&child.result->candidate->pattern==parent.result->candidate->pattern);CHECK(child.cumulative->pattern==parent.result->candidate->pattern);CHECK(*child.original==*w.original);CHECK(*w.requester->snapshot()==*w.original);stageChecks(child,w,true);
 const auto jobs=c.service->captured();CHECK(jobs.size()==2&&c.service->calls==2&&c.service->providerCalls==2);if(jobs.size()==2){const auto* capture=jobs[0].groupContext.get();CHECK(capture&&capture->peerCount==1&&capture->ownership.peers.size()==1);if(capture){CHECK(capture->ownership.peers[0].project==*w.baseline);CHECK(capture->ownership.peers[0].editRevision==w.peerEdit);CHECK(capture->ownership.peers[0].project.group->member==w.selection.members[0]);}CHECK(jobs[1].groupContext&&jobs[0].groupContext&&jobs[1].groupContext->ownership==jobs[0].groupContext->ownership&&jobs[1].groupContext->requesterEditRevision==jobs[0].groupContext->requesterEditRevision&&jobs[1].groupContext->peerCount==jobs[0].groupContext->peerCount);CHECK(std::get<GenerationRequest>(jobs[1].input).tag==child.command.request.tag);}
 CHECK(w.peerUnchanged());w.peer->reopen();CHECK(image(*w.peer,child.output,"peer-reopened.png"));
}
void requester_apply_peer_preservation_and_recall(Cases& c)
{
 begin(__func__);auto& w=*c.workflow;CHECK(c.child&&c.child->result);if(!c.child||!c.child->result)return;auto& stage=*c.child;CHECK(w.previewStop());CHECK(*w.requester->snapshot()==*w.original);CHECK(!w.requester->p->applyAiProposal());CHECK(w.applyAndUndo(stage.output,*stage.result->candidate));CHECK(w.peerUnchanged());CHECK(*w.requester->snapshot()==*w.original);
 projectChecks(stage.output.root.getChildFile("committed.airackproject"),*stage.result->candidate);projectChecks(stage.output.root.getChildFile("manual-undone.airackproject"),*stage.result->candidate);projectChecks(stage.output.root.getChildFile("undo.airackproject"),*w.original);projectChecks(stage.output.root.getChildFile("peer-after.airackproject"),*w.baseline);
 const auto report=juce::JSON::parse(stage.output.root.getChildFile("apply-report.json"));CHECK(int(report["requesterApplyCount"])==1&&bool(report["freshRemoteValidation"]));CHECK(bool(report["manualTweakOwnUndo"])&&bool(report["oneAiUndoReturnsOriginal"]));CHECK(bool(report["peerEncodedStateExact"])&&juce::int64(report["peerEditRevision"])==juce::int64(w.peerEdit));
 juce::MemoryBlock smf,expected;CHECK(stage.output.root.getChildFile("committed.mid").loadFileAsData(smf));CHECK(encodePatternMidi(stage.result->candidate->pattern,expected)&&smf==expected);juce::MidiFile midi;auto stream=stage.output.root.getChildFile("committed.mid").createInputStream();CHECK(stream&&midi.readFrom(*stream));CHECK(midi.getNumTracks()==1&&midi.getTimeFormat()==patternMidiTicksPerQuarter);if(midi.getNumTracks()==1){int notes=0;for(int n=0;n<midi.getTrack(0)->getNumEvents();++n)if(midi.getTrack(0)->getEventPointer(n)->message.isNoteOn())++notes;CHECK(notes==stage.result->candidate->pattern.noteCount);}
 auto clone=std::make_unique<Node>(c.service->options());CHECK(clone->recall(*stage.result->candidate));CHECK(clone->p->groupLinkView().phase==GroupLinkPhase::pending&&clone->p->groupWorkflowView().connection.phase==ClientPhase::closed);CHECK(clone->snapshot()->sound==stage.result->candidate->sound&&clone->snapshot()->pattern==stage.result->candidate->pattern);CHECK(saveProject(stage.output,"recalled-pending.airackproject",*clone->snapshot()));CHECK(image(*clone,stage.output,"recalled-pending.png"));
}
void closed_editor_failure_and_manual_playback(Cases& c)
{
 begin(__func__);auto& w=*c.workflow;const auto calls=w.peer->callbacks;w.peer->editor.reset();CHECK(wait(*w.requester,w.peer.get(),[&]{return w.peer->callbacks>calls+2;}));CHECK(w.peer->p->groupLinkView().phase==GroupLinkPhase::admitted);CHECK(w.peerUnchanged());
 CHECK(wait(*w.requester,w.peer.get(),[&]{return w.requester->p->groupWorkflowView().generationReady;}));CHECK(w.submit("synthetic-hold"));CHECK(wait(*w.requester,w.peer.get(),[&]{return c.service->held.load()>0;}));CHECK(w.requester->p->aiProposalPhase()==AiProposalPhase::pending);const auto gain=index("output.gain_db");CHECK(w.peer->p->beginParameterGesture(gain));CHECK(w.peer->p->setParameterGestureValue(gain,.41f));CHECK(w.peer->p->endParameterGesture(gain));CHECK(wait(*w.requester,w.peer.get(),[&]{return w.requester->p->aiProposalPhase()!=AiProposalPhase::pending;}));CHECK(w.requester->p->aiProposalPhase()!=AiProposalPhase::proposed);CHECK(*w.requester->snapshot()==*w.original);CHECK(!w.requester->p->applyAiWhenStopped());CHECK(w.peer->p->undoSoundEdit());CHECK(w.peer->snapshot()->sound==w.baseline->sound&&w.peer->snapshot()->pattern==w.baseline->pattern);
 const auto providerBefore=c.service->providerCalls.load();w.peer->p->closeAiSession();CHECK(wait(*w.requester,nullptr,[&]{return !w.requester->p->groupWorkflowView().generationReady;}));CHECK(!w.submit(complementary));CHECK(c.service->providerCalls==providerBefore);CHECK(*w.requester->snapshot()==*w.original);
 ClientOptions absent{{juce::File{},juce::Uuid().toString().removeCharacters("-"),c.service->fixture.root},false};auto offline=std::make_unique<Node>(absent);ProjectDocument copper;copper.sound=copperArcSound();CHECK(offline->recall(copper));auto before=offline->snapshot();CHECK(offline->p->beginParameterGesture(gain));CHECK(offline->p->setParameterGestureValue(gain,.42f));CHECK(offline->p->endParameterGesture(gain));for(int n=0;n<8;++n)offline->callback();CHECK(offline->callbacks==8&&offline->nonfinite==0&&offline->peak>0);CHECK(offline->snapshot()->sound.values[gain]==.42f);CHECK(offline->p->undoSoundEdit());CHECK(*offline->snapshot()==*before);CHECK(w.peer->nonfinite==0&&w.requester->nonfinite==0);CHECK(airack::companion::detail::RuntimeServerTestAccess::retainedWorkers()==0);
}
int selfTest()
{std::cout<<std::unitbuf;std::cerr<<std::unitbuf;std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<creation()<<'\n';arguments_and_group_exports();{auto c=std::make_unique<Cases>();two_instance_capture_and_refinement(*c);requester_apply_peer_preservation_and_recall(*c);if(c->workflow->baseline)closed_editor_failure_and_manual_playback(*c);else{begin("closed_editor_failure_and_manual_playback");CHECK(c->workflow->baseline!=nullptr);}}CHECK(sections==4);std::cout<<"GroupWorkflow checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;}
// END FROZEN SELF_TEST
}
int main()
{
 try{int count=0;auto** raw=CommandLineToArgvW(GetCommandLineW(),&count);require(raw,"arguments_unavailable");std::vector<std::wstring> args;for(int n=1;n<count;++n)args.emplace_back(raw[n]);LocalFree(raw);
 Arguments a;if(!parse(args,a)){std::cerr<<"invalid_arguments\n";return 2;}if(a.action==Arguments::Action::selfTest){juce::ScopedJuceInitialiser_GUI gui;return selfTest();}if(a.action==Arguments::Action::help){std::cout<<"RackSynthGroupWorkflow\n--self-test\n--run --companion ABS_EXISTING_WORKSPACE_EXE --peer ABS_EXISTING_WORKSPACE_PROJECT --output ABS_FRESH_HOST_FIXTURE_DIR\n";return 0;}juce::ScopedJuceInitialiser_GUI gui;return run(a);
 }catch(const std::exception& e){std::cerr<<"native_failure "<<e.what()<<'\n';return 1;}
}
