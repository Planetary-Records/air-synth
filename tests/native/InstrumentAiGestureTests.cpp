#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "InstrumentProcessor.h"
#include "CompanionWire.h"
#include "RackExamples.h"
#include <stdexcept>
#include <juce_gui_extra/juce_gui_extra.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack
{
struct InstrumentAiTestAccess
{
    static void requestCounter(InstrumentProcessor& p, std::uint64_t v) { p.aiNextRequestId = v; }
    static void contextCounter(InstrumentProcessor& p, std::uint64_t v) { p.aiContextRevision = v; }
    static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p, const AiRequestOptions& options,
        std::optional<double> bpm = {}, std::shared_ptr<const RackCatalogSnapshot>* captured = nullptr)
    try {
        if(options.policy.scope==ai::RequestScope::pattern)return airack::testing::beginAiRequest(p,options,bpm);
        p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
        std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
        std::erase_if(factory,[&](const auto& design){return design.id==tile;});
        if(factory.size()!=25)throw std::runtime_error("historical25 library fixture");
        auto inventory=std::make_shared<RackCatalogSnapshot>();
        {
            const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit;
            if(!p.tryProjectSnapshotOwned(*source,edit))return {};
            std::vector<RackDesign> embedded;
            if(source->sound.personalities)for(const auto& instance:source->sound.personalities->instances)embedded.push_back(instance.design);
            if(!captureRackCatalog(factory,embedded,{},*inventory))return {};
        }
        if(captured)*captured=inventory;
        return p.beginAiRequestWithCatalog(options,bpm,std::move(inventory));
    }catch(const std::bad_alloc&){return {};}
};
}
namespace
{
using namespace airack;
using namespace airack::ai;
int checks=0,failures=0,groups=0;
const char* group="";
void check(bool pass,const char* expression,int line)
{++checks;if(!pass){++failures;std::cout<<"FAIL "<<group<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){group=name;++groups;std::cout<<"GROUP "<<name<<'\n';}
std::size_t index(const char* id)
{
    const auto registry=parameterRegistry();
    const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return p.id==id;});
    CHECK(found!=registry.end());return found==registry.end()?0:std::size_t(found-registry.begin());
}
ProjectDocument project(InstrumentProcessor& p)
{ProjectDocument d;std::uint64_t edit,reset;CHECK(p.tryProjectSnapshot(d,edit,reset));return d;}
std::uint64_t revision(InstrumentProcessor& p)
{ProjectDocument d;std::uint64_t edit=0,reset;CHECK(p.tryProjectSnapshot(d,edit,reset));return edit;}
void setup(InstrumentProcessor& p)
{
    ProjectDocument d;d.pattern.meter={1,4};d.pattern.noteCount=1;d.pattern.notes[0]={0,.05,60,70,1};
    d.sound.values[index("output.gain_db")]=.4f;
    juce::MemoryBlock state;CHECK(encodeProjectState(d,state));CHECK(p.restoreProjectState(state.getData(),int(state.getSize())));
}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& f:fields)o->setProperty(f.first,f.second);return o;}
bool propose(InstrumentProcessor& p,std::initializer_list<ParameterEdit> edits)
{
    const AiRequestOptions options {"Synthetic gesture ownership regression","synthetic-selected_model:1.0",{RequestScope::sound,false,false}};
    auto request=InstrumentAiTestAccess::beginHistorical25(p,options,400);CHECK(request.has_value());if(!request)return false;
    juce::Array<juce::var> params;
    for(const auto& e:edits)params.add(object({{"id",e.id},{"normalized",double(e.normalized)}}));
    auto value=object({{"version",request->catalog?2:1},{"parameter_edits",params},
        {"rack_edits",juce::Array<juce::var>()},{"macro_edits",juce::Array<juce::var>()},{"pattern",juce::var()},
        {"characteristics","synthetic"},{"explanation","native gesture regression fixture"}});
    if(request->catalog){
        value.getDynamicObject()->setProperty("name","Gesture Test Sound");
        value.getDynamicObject()->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>()},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));
        value.getDynamicObject()->setProperty("catalog_selections",juce::Array<juce::var>());
        value.getDynamicObject()->setProperty("catalog_parameter_edits",juce::Array<juce::var>());}
    const auto text=juce::JSON::toString(value,true);
    auto result=generateProposal(*request,[&](const juce::String& model,std::string_view,const juce::var&)
    {
        CHECK(model==request->model);rack::oauth::StructuredResult reply;
        reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=text;return reply;
    });
    CHECK(result.status==GenerationStatus::ready&&result.inferenceCalls==1&&result.candidate);
    juce::MemoryBlock wire;CHECK(companion::encodeResult(*request,result,wire));
    CHECK(p.receiveAiResult(std::move(result)));
    return p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal();
}
struct Events
{
    struct Listener final : juce::AudioProcessorParameter::Listener
    {
        Events* owner=nullptr;std::size_t field=0;
        void parameterValueChanged(int,float) override {++owner->values[field];owner->notify(field,'V');}
        void parameterGestureChanged(int,bool started) override
        {if(started)++owner->starts[field];else ++owner->ends[field];owner->notify(field,started?'B':'E');}
    };
    InstrumentProcessor& processor;
    std::array<Listener,3> listeners;
    std::size_t count=0;
    std::array<int,parameterCount> starts {},values {},ends {};
    std::array<std::string,parameterCount> order;
    std::function<void(std::size_t,char)> callback;
    Events(InstrumentProcessor& p,std::initializer_list<std::size_t> fields):processor(p)
    {for(const auto field:fields){auto& listener=listeners[count++];listener.owner=this;listener.field=field;p.control(field).addListener(&listener);}}
    ~Events(){for(std::size_t n=0;n<count;++n)processor.control(listeners[n].field).removeListener(&listeners[n]);}
    void notify(std::size_t field,char phase){order[field]+=phase;if(callback)callback(field,phase);}
    void reset(){starts.fill(0);values.fill(0);ends.fill(0);for(auto& s:order)s.clear();}
    void sequence(std::size_t field,const char* expected)
    {CHECK(order[field]==expected);CHECK(starts[field]==ends[field]);}
};
void same_parameter_begin()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;setup(p);
    const auto owned=index("global.master_level");if(!propose(p,{{"global.master_level",.23f}}))return;
    Events events(p,{owned});bool fired=false,accepted=false;
    events.callback=[&](std::size_t field,char phase)
    {if(field==owned&&phase=='B'&&!fired){fired=true;accepted=p.beginParameterGesture(owned);CHECK(!accepted);}};
    CHECK(p.applyAiProposal());CHECK(fired);CHECK(!accepted);
    const bool lateEnd=p.endParameterGesture(owned);CHECK(!lateEnd); // Also cleans an incorrectly accepted old manual slot.
    events.sequence(owned,"BVE");CHECK(!p.aiProposal()&&p.aiProposalPhase()==AiProposalPhase::idle);
    events.reset();CHECK(p.beginParameterGesture(owned));CHECK(p.setParameterGestureValue(owned,.31f));CHECK(p.endParameterGesture(owned));
    events.sequence(owned,"BVE");CHECK(!p.endParameterGesture(owned));
}
void reserved_later_parameter()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;setup(p);
    const auto a=index("global.master_level"),b=index("output.gain_db");
    const auto first=std::min(a,b),later=std::max(a,b);CHECK(first!=later);
    if(!propose(p,{{"global.master_level",.23f},{"output.gain_db",.67f}}))return;
    Events events(p,{first,later});bool fired=false,accepted=false;
    events.callback=[&](std::size_t field,char phase)
    {if(field==first&&phase=='B'&&!fired){fired=true;CHECK(events.starts[later]==0);accepted=p.beginParameterGesture(later);CHECK(!accepted);}};
    CHECK(p.applyAiProposal());CHECK(fired);CHECK(!accepted);
    const bool lateEnd=p.endParameterGesture(later);CHECK(!lateEnd);
    events.sequence(first,"BVE");events.sequence(later,"BVE");
    events.reset();CHECK(p.beginParameterGesture(later));CHECK(p.endParameterGesture(later));events.sequence(later,"BE");
}
void coupled_manual_collision()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;setup(p);
    const auto owned=index("global.master_level"),unrelated=index("output.gain_db");
    const auto before=project(p);const auto edit=revision(p);if(!propose(p,{{"global.master_level",.23f}}))return;
    Events events(p,{owned,unrelated});bool fired=false,manual=false,coupled=false;
    events.callback=[&](std::size_t field,char phase)
    {
        if(field!=owned||phase!='B'||fired)return;fired=true;
        manual=p.beginParameterGesture(unrelated);CHECK(manual);if(!manual)return;
        const auto live=project(p);const auto rev=revision(p);
        const auto starts=events.starts,values=events.values,ends=events.ends;
        const std::array<ParameterGestureValue,2> edits {{{unrelated,.51f},{owned,.34f}}};
        coupled=p.setParameterGestureValues(unrelated,edits);CHECK(!coupled);
        CHECK(project(p)==live);CHECK(revision(p)==rev);
        CHECK(events.starts==starts&&events.values==values&&events.ends==ends);
    };
    CHECK(!p.applyAiProposal());CHECK(fired&&manual);CHECK(!coupled);
    CHECK(project(p)==before);CHECK(revision(p)==edit);
    const bool ended=p.endParameterGesture(unrelated);CHECK(ended); // Normalize an accepted old coupled gesture only after observing RED.
    events.sequence(owned,"BE");events.sequence(unrelated,"BE");CHECK(!p.endParameterGesture(owned));
    events.reset();CHECK(p.applyAiProposal());events.sequence(owned,"BVE");
    events.reset();CHECK(p.beginParameterGesture(unrelated));
    const std::array<ParameterGestureValue,2> ordinary {{{unrelated,.52f},{owned,.36f}}};
    CHECK(p.setParameterGestureValues(unrelated,ordinary));CHECK(p.endParameterGesture(unrelated));
    events.sequence(owned,"BVE");events.sequence(unrelated,"BVE");
}
void guarded_group_reservation()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;setup(p);
    const auto owned=index("global.master_level"),leader=index("output.gain_db");
    if(!propose(p,{{"global.master_level",.23f}}))return;
    Events events(p,{owned,leader});bool fired=false,opened=false;
    const std::array<ParameterGestureValue,2> edits {{{leader,.51f},{owned,.34f}}};
    events.callback=[&](std::size_t field,char phase){
        if(field!=owned||phase!='B'||fired)return;fired=true;SoundDocument before;std::uint64_t edit,reset;
        CHECK(p.trySoundSnapshot(before,edit,reset));const auto starts=events.starts;
        opened=p.beginParameterGesture(leader,edits,before,edit,reset);CHECK(!opened);CHECK(events.starts==starts);
        if(opened)CHECK(p.endParameterGesture(leader));
    };
    CHECK(p.applyAiProposal());CHECK(fired&&!opened);events.sequence(owned,"BVE");events.sequence(leader,"");
    events.callback=nullptr;events.reset();SoundDocument before;std::uint64_t edit,reset;CHECK(p.trySoundSnapshot(before,edit,reset));
    const bool began=p.beginParameterGesture(leader,edits,before,edit,reset);CHECK(began);
    if(began){CHECK(p.setParameterGestureValues(leader,edits,before,edit,reset));CHECK(p.endParameterGesture(leader));
        events.sequence(owned,"BVE");events.sequence(leader,"BVE");CHECK(p.undoSoundEdit());CHECK(project(p).sound==before);}
}
void callbacks_and_reuse()
{
    begin(__func__);
    for(const char stage:{'V','E'})
    {
        auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;setup(p);
        const auto a=index("global.master_level"),b=index("output.gain_db");const auto first=std::min(a,b),later=std::max(a,b);
        if(!propose(p,{{"global.master_level",.23f},{"output.gain_db",.67f}}))continue;
        Events events(p,{first,later});bool fired=false,accepted=false;
        events.callback=[&](std::size_t field,char phase)
        {
            if(phase!=stage||field!=(stage=='V'?first:later)||fired)return;fired=true;
            if(stage=='E')CHECK(events.ends[first]==1); // Earlier AI field remains owned through the last end callback.
            accepted=p.beginParameterGesture(first);CHECK(!accepted);
        };
        CHECK(p.applyAiProposal());CHECK(fired);CHECK(!accepted);
        const bool lateEnd=p.endParameterGesture(first);CHECK(!lateEnd);
        events.sequence(first,"BVE");events.sequence(later,"BVE");
        events.reset();CHECK(p.beginParameterGesture(first));CHECK(p.setParameterGestureValue(first,.32f));CHECK(p.endParameterGesture(first));
        events.sequence(first,"BVE");
        events.reset();
        if(propose(p,{{"global.master_level",.41f},{"output.gain_db",.53f}}))
        {CHECK(p.applyAiProposal());events.sequence(first,"BVE");events.sequence(later,"BVE");}
        events.reset();const auto current=project(p);const auto rev=revision(p);
        if(propose(p,{{"global.master_level",current.sound.values[a]},{"output.gain_db",current.sound.values[b]}}))
        {CHECK(p.applyAiProposal());CHECK(revision(p)==rev);CHECK(project(p)==current);events.sequence(first,"");events.sequence(later,"");}
        CHECK(p.beginParameterGesture(first));CHECK(p.endParameterGesture(first));CHECK(p.beginParameterGesture(later));CHECK(p.endParameterGesture(later));
    }
}

void automation_during_apply()
{
    begin(__func__);InstrumentProcessor p{airack::testing::isolatedRackRuntime()};setup(p);const auto engine=index("synth.1.engine");
    if(propose(p,{{"output.gain_db",.51f}})){
        p.control(engine).setValue(1);p.control(engine).setValue(0);
        CHECK(p.aiProposalPhase()==AiProposalPhase::stale);CHECK(!p.applyAiProposal());
    }
    p.discardAiProposal();
    if(propose(p,{{"output.gain_db",.52f}})){
        Events events(p,{index("output.gain_db")});events.callback=[&](std::size_t,char phase){if(phase=='B'){p.control(engine).setValue(1);p.control(engine).setValue(0);}};
        const auto before=project(p);CHECK(!p.applyAiProposal());CHECK(project(p).sound.values==before.sound.values);CHECK(events.starts[index("output.gain_db")]==events.ends[index("output.gain_db")]);
    }
}

void processIdentity(int argc,char** argv)
{
    FILETIME created {},exited {},kernel {},user {};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    ULARGE_INTEGER time {};time.LowPart=created.dwLowDateTime;time.HighPart=created.dwHighDateTime;
    wchar_t module[32768] {};GetModuleFileNameW(nullptr,module,32768);
    std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(module)
        <<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<'\n';
    for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
}
}
int main(int argc,char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    std::cout.setf(std::ios::unitbuf);processIdentity(argc,argv);juce::ScopedJuceInitialiser_GUI gui;
    automation_during_apply();same_parameter_begin();reserved_later_parameter();coupled_manual_collision();guarded_group_reservation();callbacks_and_reuse();
    std::cout<<checks<<" checks, "<<groups<<" groups, "<<failures<<" failures\n";return failures?1:0;
}
