#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "OwnedFixture.h"
#include "InstrumentProcessor.h"
#include "AiUnit.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "AiCatalog.h"
#include "StrictJson.h"
#include "GroupPerformance.h"
#include <bit>
#include <limits>
#include <set>
#include "CompanionRuntimeServer.h"
#include "PatternControls.h"
#include "ProgrammerUnit.h"
#include "RackStack.h"
#include "MakerFace.h"
#include "ScreenDriver.h"
#include "StudioSkin.h"
#include "PostedClick.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <future>
#include <mutex>
#include <thread>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

// Labelled synthetic native RuntimeServer/AI unit/editor fixtures. No browser, account
// store, actual provider, inference, helper launch or production pipe is used.
namespace airack {
struct InstrumentGroupTestAccess {
 // Only UI refresh runs while synthetic validation is installed. Preserve the
 // real tracked command so its queued reply keeps the original authority.
 static std::unique_ptr<companion::GroupCommand> pendingValidation(InstrumentProcessor& p,bool pending,std::unique_ptr<companion::GroupCommand> previous={}){const std::lock_guard lock(p.restoreMutex);auto original=std::move(p.groupControl->pending);if(pending){p.groupControl->pending=std::make_unique<companion::GroupCommand>();p.groupControl->pending->action=companion::GroupAction::validateProposal;p.groupControl->pending->operationId=UINT64_MAX;}else p.groupControl->pending=std::move(previous);return original;}
 static std::uint64_t pauseWithPublication(InstrumentProcessor& p){p.stopTimer();if(!p.groupControl->pending)p.submitGroup(InstrumentProcessor::GroupControl::Purpose::publish,GetTickCount64());const std::lock_guard lock(p.restoreMutex);return p.groupControl->pending?p.groupControl->pending->operationId:0;}
 static std::uint64_t pendingOperation(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.groupControl->pending?p.groupControl->pending->operationId:0;}
 static void resumePolling(InstrumentProcessor& p){p.startTimer(25);}
 static bool applyToken(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.groupControl->applyToken.has_value();}
};
}
namespace
{
using namespace airack;
using namespace airack::ai;
using namespace airack::companion;
int checks=0,failures=0,groups=0,groupChecks=0,groupFailures=0;
const char* group="";
void check(bool pass,const char* expression,int line)
{++checks;if(!pass){++failures;std::cout<<"FAIL "<<group<<':'<<line<<' '<<expression<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void finish(){if(groups)std::cout<<"GROUP_RESULT "<<group<<" checks="<<checks-groupChecks<<" failures="<<failures-groupFailures<<std::endl;}
void begin(const char* name){finish();group=name;++groups;groupChecks=checks;groupFailures=failures;std::cout<<"GROUP "<<name<<std::endl;}
void pump(int ms=30){juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);}
template<class F> bool wait(F f,unsigned milliseconds=5000){const auto end=GetTickCount64()+milliseconds;do{if(f())return true;pump(2);}while(GetTickCount64()<end);return f();}
juce::Component* find(juce::Component& root,const juce::String& id)
{if(root.getComponentID()==id)return &root;for(auto* child:root.getChildren())if(auto* result=find(*child,id))return result;return nullptr;}
template<class T> T* widget(juce::Component& root,const char* id)
{auto* c=dynamic_cast<T*>(find(root,id));CHECK(c!=nullptr);return c;}
void click(juce::Button& button){CHECK(clickPosted(button));pump();}
bool click(juce::Component& root,const char* id)
{if(auto* c=widget<juce::Button>(root,id)){CHECK(c->isEnabled());click(*c);return true;}return false;}
void postedClickBehindSlowMessage(){begin(__func__);CHECK(clickRunsBehindSlowMessage([](juce::Button& button){click(button);}));}
juce::String status(juce::Component& root)
{if(auto* c=widget<juce::Label>(root,"ai.status"))return c->getText();return {};}
juce::String diff(juce::Component& root)
{if(auto* c=widget<juce::TextEditor>(root,"ai.diff")){CHECK(c->isReadOnly());return c->getText();}return {};}
// The AI unit's screen, a text on its page as drawn, an option's index, and whether the page offers an item.
studio::Screen& screenIn(juce::Component& unit)
{auto* s=screenOf(unit,"ai.screen");CHECK(s!=nullptr);return *s;}
juce::String drawn(studio::Screen& s,const juce::String& key)
{const auto t=s.texts.find(key);return t==s.texts.end()?juce::String():t->second.text;}
int option(studio::Screen& s,const char* bind)
{const auto o=s.shown.options.find(bind);return o==s.shown.options.end()?-1:o->second;}
bool offered(studio::Screen& s,const char* command)
{return screendriver::reveal(s,[&](const screendriver::Hit& h){return h.command==command||h.entry==command;})!=nullptr;}
std::size_t index(const juce::String& id)
{const auto r=parameterRegistry();const auto i=std::find_if(r.begin(),r.end(),[&](const auto& d){return d.id==id;});CHECK(i!=r.end());return i==r.end()?0:std::size_t(i-r.begin());}
ProjectDocument project(InstrumentProcessor& p)
{ProjectDocument d;std::uint64_t r;CHECK(p.tryProjectSnapshot(d,r));return d;}
std::uint64_t revision(InstrumentProcessor& p)
{ProjectDocument d;std::uint64_t r=0;CHECK(p.tryProjectSnapshot(d,r));return r;}
void recall(InstrumentProcessor& p,const ProjectDocument& d)
{juce::MemoryBlock b;CHECK(encodeProjectState(d,b));CHECK(p.restoreProjectState(b.getData(),int(b.getSize())));pump();}
ProjectDocument setup(InstrumentProcessor& p,bool phrase=true)
{ProjectDocument d;d.sound.values[index("output.gain_db")]=.4f;if(phrase){d.pattern.meter={1,4};d.pattern.noteCount=1;d.pattern.notes[0]={0,.05,60,80,1};}recall(p,d);return d;}
// The user's one-unit rack over a laid-out bank.
void adopt(SoundDocument& sound,RackRole role,NativeModule module,std::uint8_t slot)
{
    RackDesign design;design.id=juce::Uuid();design.name="User rack";design.role=role;design.panel.sections={"Native"};design.units.push_back({0,module,{}});
    RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.destination.role=role;op.allocation.push_back({0,slot});op.design=design;
    SoundDocument next;RackDifference delta;CHECK(makeRackCandidate(sound,op,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);sound=std::move(next);
}
// setup()'s document with the user's racks on the banks "parent" moves and removes: synth 1, and Drive and Reverb on
// the master, each its own rack.
ProjectDocument racked(InstrumentProcessor& p)
{
    ProjectDocument d;d.sound.values[index("output.gain_db")]=.4f;d.pattern.meter={1,4};d.pattern.noteCount=1;d.pattern.notes[0]={0,.05,60,80,1};
    for(const auto& e:{RackEdit{RackEditKind::addEffect,0,masterChain,0,EffectKind::drive},RackEdit{RackEditKind::addEffect,1,masterChain,1,EffectKind::reverb}})
    {RackLayout next;CHECK(editRackLayout(d.sound.layout,e,next)==RackEditResult::applied);d.sound.layout=next;}
    adopt(d.sound,RackRole::synth,NativeModule::analog,0);adopt(d.sound,RackRole::fx,NativeModule(unsigned(EffectKind::drive)+2),0);adopt(d.sound,RackRole::fx,NativeModule(unsigned(EffectKind::reverb)+2),1);
    recall(p,d);return d;
}
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
bool measurementsEqual(const AudioMeasurements& a, const AudioMeasurements& b)
{
    if (a.finite!=b.finite || a.clippedSamples!=b.clippedSamples || !bits(a.peak,b.peak) || !bits(a.rms,b.rms)
        || a.onsetSeconds.has_value()!=b.onsetSeconds.has_value() || !bits(a.activityEndSeconds,b.activityEndSeconds)
        || !bits(a.decaySeconds,b.decaySeconds) || a.tailActiveAtEnd!=b.tailActiveAtEnd) return false;
    if (a.onsetSeconds && !bits(*a.onsetSeconds,*b.onsetSeconds)) return false;
    for (std::size_t i=0;i<3;++i) if (!bits(a.bandFractions[i],b.bandFractions[i])) return false;
    return true;
}
bool audioEqual(const PreviewAudio& a, const PreviewAudio& b)
{
    return a.left.size()==b.left.size() && a.right.size()==b.right.size()
        && (a.left.empty() || std::memcmp(a.left.data(),b.left.data(),a.left.size()*sizeof(float))==0)
        && (a.right.empty() || std::memcmp(a.right.data(),b.right.data(),a.right.size()*sizeof(float))==0)
        && bits(a.sampleRate,b.sampleRate) && a.bodyFrames==b.bodyFrames && a.phraseTruncated==b.phraseTruncated
        && a.context.phrase==b.context.phrase && bits(a.context.bpm,b.context.bpm)
        && a.context.phraseSource==b.context.phraseSource && a.context.tempoSource==b.context.tempoSource;
}
bool resultEqual(const GenerationResult& a, const GenerationResult& b)
{
    return a.tag==b.tag && a.status==b.status && a.inferenceCalls==b.inferenceCalls && a.proposalStatus==b.proposalStatus
        && a.renderStatus==b.renderStatus && a.providerStatus==b.providerStatus && a.httpStatus==b.httpStatus
        && a.errorCode==b.errorCode && a.candidate==b.candidate && editsEqual(a.edits,b.edits)
        && audioEqual(a.audio,b.audio) && measurementsEqual(a.measurements,b.measurements) && a.catalog==b.catalog;
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
bool usesOwnedPreview(const GenerationRequest& r)
{
    if(r.catalog&&r.catalog->authority&&r.catalog->authority->rendering==CatalogRendering::ownedLocal)return true;
    return r.source.sound.personalities&&std::any_of(r.source.sound.personalities->instances.begin(),r.source.sound.personalities->instances.end(),[](const auto& rack){
        return std::any_of(rack.design.units.begin(),rack.design.units.end(),[](const auto& unit){return unit.module==NativeModule::drum;});});
}
bool sameRequest(const GenerationRequest& a,const GenerationRequest& b)
{
    if(usesOwnedPreview(a)||usesOwnedPreview(b))return requestEqual(a,b);
    juce::MemoryBlock x,y;return encodeRequest(a,x)&&encodeRequest(b,y)&&x==y;
}
bool sameResult(const GenerationRequest& request,const GenerationResult& a,const GenerationResult& b)
{
    if(usesOwnedPreview(request))return validateGenerationResult(request,a)&&validateGenerationResult(request,b)&&resultEqual(a,b);
    juce::MemoryBlock x,y;return encodeResult(request,a,x)&&encodeResult(request,b,y)&&x==y;
}
thread_local bool audioGuard=false;
thread_local unsigned allocations=0,deallocations=0;
unsigned guardedCalls=0;
#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl heapHook(int kind,void*,size_t,int,long,const unsigned char*,int)
{if(audioGuard){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++deallocations;}return 1;}
#endif
void callback(InstrumentProcessor& p,juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi)
{const auto a=allocations,d=deallocations;audioGuard=true;p.processBlock(audio,midi);audioGuard=false;++guardedCalls;CHECK(allocations==a);CHECK(deallocations==d);}
PatternDocument phrase(int pitch)
{PatternDocument p;p.meter={1,4};p.noteCount=1;p.notes[0]={0,.05,static_cast<std::uint8_t>(pitch),90,1};return p;}
// The parameter that control k of the placed rack n drives.
std::optional<std::size_t> rackControl(const SoundDocument& d,std::size_t n,std::size_t k)
{
    if(!d.personalities||d.personalities->instances.size()<=n)return {};
    const auto& i=d.personalities->instances[n];if(i.design.panel.controls.size()<=k)return {};
    const auto& c=i.design.panel.controls[k];
    const auto u=std::find_if(i.design.units.begin(),i.design.units.end(),[&](const auto& v){return v.id==c.unit;});
    const auto b=std::find_if(i.bindings.begin(),i.bindings.end(),[&](auto v){return v.unit==c.unit;});
    if(u==i.design.units.end()||b==i.bindings.end())return {};
    return rackParameterIndex(u->module,b->slot,c.key);
}
ProposalEdits editsFor(const GenerationRequest& request)
{
    ProposalEdits e;e.characteristics="Synthetic native measured candidate";e.explanation="Synthetic service; existing DAW automation remains attached.";
    const auto& instruction=request.instruction;
    if(instruction.starts_with("catalog-preview")){
        if(instruction=="catalog-preview-existing")if(const auto n=rackControl(request.source.sound,0,0))
            e.parameters.push_back({parameterRegistry()[*n].id,.65f});
        return e;
    }
    // The first rack's first control 0.3 from its live value, and its second repeated at its live value: the one the
    // test locks.
    if(instruction=="marks")
    {
        const auto& sound=request.source.sound;const auto first=rackControl(sound,0,0),second=rackControl(sound,0,1);
        if(first&&second){const auto v=sound.values[*first];e.parameters={{parameterRegistry()[*first].id,v<.5f?v+.3f:v-.3f},{parameterRegistry()[*second].id,sound.values[*second]}};}
        return e;
    }
    if(instruction=="rack63"||instruction=="rack2"||instruction=="rack1")
    {for(int n=0;n<(instruction=="rack63"?63:instruction=="rack2"?2:1);++n)e.racks.push_back({RackEditKind::moveSynth,0,masterChain,0});return e;}
    // Four parameter changes, the third an explicit equal value on a control the test locks.
    if(instruction=="four")
    {const auto glide=index("global.glide_seconds");e.parameters={{"output.gain_db",.31f},{"global.master_level",.23f},{"global.glide_seconds",request.source.sound.values[glide]},{parameterRegistry()[39].id,.6f}};return e;}
    if(instruction=="named"){e.name="Ethereal Rolling Bass";e.parameters={{"output.gain_db",.31f}};return e;}
    if(request.policy.scope!=RequestScope::pattern)
    {
        e.parameters.push_back({"output.gain_db",instruction=="child-one"?.55f:instruction=="child-two"?.4f:.31f});
        e.parameters.push_back({"global.master_level",.23f});
        if(request.policy.allowMacroChanges)e.macros.push_back({1,"output.gain_db",instruction=="child-one"?.4f:instruction=="child-two"?.6f:.2f});
        // On racked()'s document: the Reverb rack removed, and the Drive rack moved to synth 1's inserts, back to the
        // master, and to the inserts again.
        if(instruction=="parent")
        {
            const auto& sound=request.source.sound;
            if(sound.personalities)for(const auto& i:sound.personalities->instances)
                if(i.design.role==RackRole::fx&&std::any_of(i.bindings.begin(),i.bindings.end(),[](auto b){return b.slot==1;}))e.reconciliation.remove.push_back(i.id);
            e.racks.push_back({RackEditKind::removeEffect,1});
            e.racks.push_back({RackEditKind::moveEffect,0,0,0});
            const auto registry=parameterRegistry();
            e.parameters.push_back({registry[252].id,1});e.parameters.push_back({registry[253].id,.5f});e.parameters.push_back({registry[254].id,1.f/16});
        }
        if(instruction=="child-one")e.racks.push_back({RackEditKind::moveEffect,0,masterChain,0});
        if(instruction=="child-two")e.racks.push_back({RackEditKind::moveEffect,0,0,0});
    }
    if(request.policy.scope!=RequestScope::sound&&instruction!="empty-child")e.pattern=instruction=="empty"?PatternDocument{}:phrase(instruction=="child-one"?68:instruction=="child-two"?72:64);
    return e;
}
// Emit the fixture's intended edits through the same strict provider parser as ordinary generation.
juce::String syntheticResponse(const GenerationRequest& request,const juce::var& schema)
{
    const auto object=[](std::initializer_list<std::pair<const char*,juce::var>> fields){auto* value=new juce::DynamicObject;
        for(const auto& field:fields)value->setProperty(field.first,field.second);return juce::var(value);};
    const auto edits=editsFor(request);juce::Array<juce::var> parameters,racks,macros,remove;
    for(const auto& p:edits.parameters)parameters.add(object({{"id",p.id},{"normalized",double(p.normalized)}}));
    const auto rackKinds=schema["properties"]["rack_edits"]["items"]["properties"]["kind"]["enum"];
    const auto effectKinds=schema["properties"]["rack_edits"]["items"]["properties"]["effect"]["enum"];
    for(const auto& r:edits.racks)racks.add(object({{"kind",rackKinds[int(r.kind)]},{"slot",int(r.slot)},
        {"chain",int(r.destination)},{"position",int(r.position)},{"effect",effectKinds[int(r.effect)]}}));
    for(const auto& m:edits.macros)macros.add(object({{"index",int(m.index)},{"target_id",m.targetId.isEmpty()?juce::var():juce::var(m.targetId)},{"amount",double(m.amount)}}));
    for(const auto& id:edits.reconciliation.remove)remove.add(id.toDashedString());
    if(!edits.reconciliation.detach.empty()||!edits.reconciliation.replace.empty()||!edits.reconciliation.controls.empty()
        ||!edits.catalogSelections.empty()||!edits.catalogParameters.empty())throw std::runtime_error("unexpected synthetic panel intent");
    juce::var pattern;
    if(edits.pattern){const auto& p=*edits.pattern;juce::Array<juce::var> notes;
        for(std::size_t n=0;n<p.noteCount;++n){const auto& note=p.notes[n];notes.add(object({{"start_beats",note.startBeats},{"duration_beats",note.durationBeats},
            {"pitch",int(note.pitch)},{"velocity",int(note.velocity)},{"channel",int(note.channel)}}));}
        constexpr const char* gridNames[]{"quarter","eighth","sixteenth","thirty_second","sixty_fourth"};
        pattern=object({{"bars",int(p.bars)},{"meter_numerator",int(p.meter.numerator)},{"meter_denominator",int(p.meter.denominator)},
            {"grid",gridNames[int(p.grid)]},{"swing",p.swing},{"notes",notes}});}
    auto response=object({{"version",2},{"parameter_edits",parameters},{"rack_edits",racks},{"macro_edits",macros},{"pattern",pattern},
        {"characteristics",edits.characteristics},{"explanation",edits.explanation},{"name",edits.name.isEmpty()?"Native Sound "+juce::String(request.tag.requestId):edits.name},
        {"reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",remove},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}})},
        {"catalog_selections",juce::Array<juce::var>()},{"catalog_parameter_edits",juce::Array<juce::var>()}});
    if(request.instruction.starts_with("catalog-preview")){
        if(!request.catalog||!request.catalog->authority)throw std::runtime_error("catalog preview authority missing");
        juce::var inventory;if(!parseStrictJson(request.catalog->authority->descriptorJson,inventory,maximumCatalogDescriptorBytes))
            throw std::runtime_error("catalog preview inventory invalid");
        bool found=false;
        if(const auto* entries=inventory["entries"].getArray())for(const auto& row:*entries)
            if(juce::Uuid(testing::catalogRowId(row).toString())==juce::Uuid("91387715-13d3-4c57-a479-12251842a402")){
                response.getDynamicObject()->setProperty("catalog_selections",juce::Array<juce::var>{object({{"local_id",1},
                    {"design",testing::catalogRowId(row)},{"revision",testing::catalogRowRevision(row)},
                    {"content_sha256",testing::catalogRowHash(row)},
                    {"destination",object({{"role","synth"},{"chain",4},{"position",0}})}})});
                found=true;break;
            }
        if(!found)throw std::runtime_error("catalog preview Copper Arc missing");
        response.getDynamicObject()->setProperty("catalog_parameter_edits",juce::Array<juce::var>{
            object({{"selection",1},{"unit",0},{"key","analog.filter.cutoff"},{"normalized",.25}}),
            object({{"selection",1},{"unit",0},{"key","analog.osc.1.waveform"},{"normalized",1.0/3}})});
        if(request.instruction=="catalog-preview-pair"){
            auto second=(*response["catalog_selections"].getArray())[0].clone();
            second.getDynamicObject()->setProperty("local_id",2);
            second["destination"].getDynamicObject()->setProperty("position",1);
            response["catalog_selections"].getArray()->add(second);
            response["catalog_parameter_edits"].getArray()->add(object({{"selection",2},{"unit",0},{"key","analog.filter.cutoff"},{"normalized",.75}}));
        }
    }
    if(!request.catalog){auto* value=response.getDynamicObject();value->removeProperty("catalog_selections");value->removeProperty("catalog_parameter_edits");}
    return juce::JSON::toString(response,true,22);
}
GenerationResult syntheticResult(const GenerationRequest& request)
{
    if(request.instruction.starts_with("catalog-preview"))return generateProposal(request,[&](const auto&,auto,const auto& schema){
        rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;
        reply.text=syntheticResponse(request,schema);return reply;
    });
    GenerationResult r;r.tag=request.tag;r.inferenceCalls=1;r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;
    if(request.instruction=="fail")
    {r.status=GenerationStatus::providerFailure;r.providerStatus=rack::oauth::Status::networkFailure;r.errorCode="server_error";return r;}
    r.edits=editsFor(request);
    if(request.catalog)
    {
        auto combined=request.catalog->baseIntent;
        const auto merge=[](auto& destination,const auto& input,auto same)
        {for(const auto& row:input){auto found=std::find_if(destination.begin(),destination.end(),[&](const auto& old){return same(old,row);});if(found==destination.end())destination.push_back(row);else *found=row;}};
        merge(combined.parameters,r.edits.parameters,[](const auto& a,const auto& b){return a.id==b.id;});
        merge(combined.macros,r.edits.macros,[](const auto& a,const auto& b){return a.index==b.index;});
        combined.racks.insert(combined.racks.end(),r.edits.racks.begin(),r.edits.racks.end());
        merge(combined.reconciliation.remove,r.edits.reconciliation.remove,[](const auto& a,const auto& b){return a==b;});
        if(r.edits.pattern)combined.pattern=r.edits.pattern;
        combined.characteristics=r.edits.characteristics;combined.explanation=r.edits.explanation;combined.name=r.edits.name;
        r.edits=std::move(combined);
    }
    ProjectDocument candidate;
    const auto admitted=request.catalog?admitProposal(request.source,request.policy,r.edits,*request.catalog->authority,candidate):admitProposal(request.source,request.policy,r.edits,candidate);
    if(admitted!=ProposalStatus::accepted){r.edits={};r.status=GenerationStatus::budgetExhausted;r.proposalStatus=admitted;return r;}
    r.candidate=candidate;AuditionContext context;context.bpm=request.capturedBpm.value_or(120);context.tempoSource=request.capturedBpm?TempoSource::captured:TempoSource::defaultTempo;
    if(r.edits.pattern){context.phrase=*r.edits.pattern;context.phraseSource=PhraseSource::proposed;}
    else if(request.capturedPhrase)context.phrase=*request.capturedPhrase;
    else if(request.source.pattern.noteCount)context.phrase=request.source.pattern;
    else{context.phraseSource=PhraseSource::defaultNotes;context.phrase.noteCount=4;for(std::size_t n=0;n<4;++n)context.phrase.notes[n]={double(n),.25,60,96,1};}
    auto render=renderPreview(candidate,context);if(render.status!=RenderStatus::ready)throw std::runtime_error("synthetic native render failed");
    r.audio=std::move(render.audio);r.measurements=render.measurements;r.status=GenerationStatus::ready;r.proposalStatus=ProposalStatus::accepted;r.renderStatus=RenderStatus::ready;if(request.catalog)r.catalog=CatalogResultBinding{request.catalog->root,request.catalog->authority->snapshot};
    return r;
}
struct Backend
{
    std::atomic_int generationCalls{0},cancelCalls{0};std::array<std::atomic_int,6> accountCalls{};
    // holdAccount keeps a sign-in waiting, as the browser does until the user finishes.
    std::atomic_bool release{false},holdAccount{false},rejectNewSignIn{false};std::mutex mutex;
    // expired: the saved access token has expired. Status says accessExpired; the next models or selectModel renews it,
    // as the companion's ensureAccess does, or, with refuseRenewal, answers tokenFailure/invalid_grant and leaves the
    // sign-in cleared, as a refused refresh token does. holdRenewal keeps that renewal waiting.
    std::atomic_bool expired{false},refuseRenewal{false},holdRenewal{false};
    rack::oauth::Status provider=rack::oauth::Status::connected;RuntimeStatus runtime=RuntimeStatus::ready;
    juce::String selected="synthetic-a";bool omitModels=false;bool verified=false;
    std::vector<rack::oauth::SavedAccount> accounts;juce::String activeAccount;
    std::vector<GenerationRequest> requests;std::vector<GenerationResult> results;
    void state(rack::oauth::Status p,RuntimeStatus r=RuntimeStatus::ready,const juce::String& model="synthetic-a")
    {std::lock_guard lock(mutex);provider=p;runtime=r;selected=model;}
    Frame run(const RuntimeJob& job)
    {
        if(const auto* request=std::get_if<GenerationRequest>(&job.input))
        {
            generationCalls.fetch_add(1);{std::lock_guard lock(mutex);requests.push_back(*request);}
            if(request->instruction=="hold")while(!release.load()&&!job.cancellation->cancelled())Sleep(1);
            if(job.localPreview||job.previewExchange)return runLocal(job,*request);
            const auto renderStart=GetTickCount64();auto result=syntheticResult(*request);std::cout<<"NATIVE_RENDER request="<<request->tag.requestId<<" elapsed_ms="<<GetTickCount64()-renderStart<<std::endl;{std::lock_guard lock(mutex);results.push_back(result);}
            Frame f{MessageKind::result,job.session,{}};if(!encodeResult(*request,result,f.payload))throw std::runtime_error("synthetic native result wire failed");return f;
        }
        const auto command=std::get<AccountCommand>(job.input);accountCalls[static_cast<std::size_t>(command.action)].fetch_add(1);
        if(command.action==AccountAction::signIn||command.action==AccountAction::addAccount)while(holdAccount.load()&&!(job.cancellation&&job.cancellation->cancelled()))Sleep(1);
        const bool renewal=expired.load()&&(command.action==AccountAction::models||command.action==AccountAction::selectModel);
        if(renewal)while(holdRenewal.load()&&!(job.cancellation&&job.cancellation->cancelled()))Sleep(1);
        AccountReply reply;
        {std::lock_guard lock(mutex);if(command.action==AccountAction::signIn){provider=rack::oauth::Status::connected;expired.store(false);}if(command.action==AccountAction::selectModel)selected=command.model;
            if(command.action==AccountAction::addAccount&&!rejectNewSignIn.load()&&!(job.cancellation&&job.cancellation->cancelled())){provider=rack::oauth::Status::connected;activeAccount="saved-b";if(accounts.size()==1)accounts.insert(accounts.begin(),{"saved-b","Account 2 - b@example.test"});}
            if(command.action==AccountAction::selectAccount)activeAccount=command.model;
            if(renewal){expired.store(false);if(refuseRenewal.load())provider=rack::oauth::Status::signedOut;}
            reply.providerStatus=expired.load()?rack::oauth::Status::accessExpired:provider;reply.status=runtime;reply.selectedModel=selected;reply.modelVerified=verified;
            reply.accounts=accounts;reply.activeAccount=activeAccount;
            if(command.action==AccountAction::models&&!omitModels)reply.models={{"synthetic-a","Synthetic A"},{"synthetic-b","Synthetic B"}};
            if(command.action==AccountAction::models&&!omitModels&&activeAccount=="saved-b")reply.models={{"second-only","Second account choice"}};
            if(renewal&&refuseRenewal.load()){reply.providerStatus=rack::oauth::Status::tokenFailure;reply.errorCode="invalid_grant";reply.models.clear();}
            if(command.action==AccountAction::addAccount&&rejectNewSignIn.load()){reply.providerStatus=rack::oauth::Status::tokenFailure;reply.errorCode="invalid_grant";reply.httpStatus=400;reply.models.clear();}}
        Frame f{MessageKind::accountResult,job.session,{}};if(!encodeAccountReply(reply,f.payload))throw std::runtime_error("synthetic public account wire failed");return f;
    }
    Frame runLocal(const RuntimeJob& job,const GenerationRequest& request)
    {
        if(!job.localPreview||!job.previewExchange)throw std::runtime_error("synthetic local preview pair missing");
        auto input=std::make_unique<LocalGenerationRequest>();input->request=request;input->local=*job.localPreview;
        if(request.catalog){const auto& authority=*request.catalog->authority;juce::var descriptor;
            if(authority.rendering!=CatalogRendering::ownedLocal||!parseStrictJson(authority.descriptorJson,descriptor,maximumCatalogDescriptorBytes))
                throw std::runtime_error("synthetic local catalog mode invalid");
            const auto* entries=descriptor["entries"].getArray();const auto tile=airack::test::frozenTileArrayRackDesign();juce::MemoryBlock canonical;
            if(!entries||!encodeRackDesign(tile,canonical,RackValidationMode::instanceRecall))throw std::runtime_error("synthetic local Tile grounding missing");
            const auto hash=juce::SHA256(canonical).toHexString();unsigned copies=0;
            for(const auto& row:*entries)if(juce::Uuid(testing::catalogRowId(row).toString())==tile.id){++copies;
                if(testing::catalogRowRevision(row).toString()!=juce::String(tile.revision)||testing::catalogRowHash(row).toString()!=hash)
                    throw std::runtime_error("synthetic local Tile identity differs");}
            const bool embedded=request.source.sound.personalities&&std::any_of(request.source.sound.personalities->instances.begin(),request.source.sound.personalities->instances.end(),[&](const auto& rack){return rack.design==tile;});
            if(copies>1||(embedded&&copies!=1))throw std::runtime_error("synthetic local Tile identity count differs");}
        unsigned attempts=request.catalog&&request.catalog->phase==CatalogPhase::materialize?request.catalog->inferenceCallsUsed:0;
        std::uint8_t ordinal=0;std::optional<AuditionContext> evaluatedContext;
        const CancelCheck cancelled=[&]{return job.cancellation->cancelled();};
        const auto renderStart=GetTickCount64();
        auto local=std::unique_ptr<LocalGenerationResult>(new LocalGenerationResult(generateLocalProposal(*input,
            [&](const auto&,auto,const auto& schema){++attempts;rack::oauth::StructuredResult reply;
                if(request.instruction=="fail"){reply.result.status=rack::oauth::Status::networkFailure;reply.result.errorCode="server_error";return reply;}
                reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=syntheticResponse(request,schema);return reply;},
            [&](const auto& candidate,const auto& edits,const auto& audition,const auto& trace,const auto& stop){
                auto value=std::make_unique<PreviewEvaluate>();value->edits=edits;value->context=audition;auto& binding=value->binding;
                binding.request=request.tag;binding.root=input->local.root;binding.catalogSnapshot=input->local.catalogSnapshot;binding.job=job.jobId;
                binding.structuralGeneration=request.structuralGeneration;binding.reset=input->local.reset;binding.sourceWord=input->local.sourceWord;
                binding.phase=request.catalog?request.catalog->phase:CatalogPhase::select;binding.providerAttempt=std::uint8_t(attempts);binding.renderOrdinal=++ordinal;
                binding.sourceMap=input->local.sourceMapDigest;PreviewSampleMap staged;
                if(!stagePreviewMap(input->local.sourceMap,trace,staged)||!digestPreviewMap(staged,binding.stagedMap)
                    ||!digestPreviewCandidate(candidate,binding.candidate)||!digestPreviewContext(audition,binding.context))throw std::runtime_error("synthetic local binding failed");
                evaluatedContext=audition;return job.previewExchange->evaluate(*value,stop);
            },cancelled,job.groupContext.get())));
        std::cout<<"NATIVE_LOCAL_RENDER request="<<request.tag.requestId<<" elapsed_ms="<<GetTickCount64()-renderStart<<std::endl;
        Frame frame{MessageKind::resultLocal,job.session,{}};
        if(!encodeLocalResult(*input,*local,frame.payload))throw std::runtime_error("synthetic local terminal wire failed");
        auto expected=std::make_unique<GenerationResult>(local->numeric);
        if(local->numeric.status==GenerationStatus::ready){
            if(!local->numeric.candidate||!local->rendered||!evaluatedContext)throw std::runtime_error("synthetic local ready authority missing");
            if(local->numeric.candidate->sound.personalities)for(const auto& rack:local->numeric.candidate->sound.personalities->instances)
                for(const auto& unit:rack.design.units)if(unit.module==NativeModule::drum)throw std::runtime_error("panel direct reference requires a melodic fixture");
            auto reference=renderPreview(*local->numeric.candidate,*evaluatedContext);
            if(reference.status!=RenderStatus::ready||!measurementsEqual(reference.measurements,local->rendered->measurements)
                ||!joinLocalPreview(*input,*local,*local->rendered,std::move(reference.audio),*expected))throw std::runtime_error("synthetic local independent reference mismatch");
        }
        {std::lock_guard lock(mutex);results.push_back(std::move(*expected));}return frame;
    }
    GenerationRequest request(std::size_t n){std::lock_guard lock(mutex);return requests.at(n);}
    GenerationResult result(std::size_t n){std::lock_guard lock(mutex);return results.at(n);}
};
struct Service
{
    std::shared_ptr<Backend> backend=std::make_shared<Backend>();std::atomic_bool stop{false};juce::String name=juce::Uuid().toString().removeCharacters("-");
    std::unique_ptr<RuntimeServer> server;std::thread io;
    Service(){start();}
    void start(){stop.store(false);server=std::make_unique<RuntimeServer>([b=backend](const auto& j){return b->run(j);},[b=backend](const auto& job){b->cancelCalls.fetch_add(1);if(job.previewExchange)job.previewExchange->cancel();});io=std::thread([this]{const auto r=server->run(stop,name);if(r.workerRetained)std::terminate();});}
    void halt(){backend->release.store(true);backend->holdAccount.store(false);backend->holdRenewal.store(false);stop.store(true);if(io.joinable())io.join();server.reset();}
    ~Service(){halt();}
};
// The AI unit on the desktop over a prepared processor and the synthetic service, hidden until open() shows it as
// AI on the session bar does; its dialogs answer from `files`. With `inEditor` the unit is the one in the editor, at
// 1080 x 680, and open() presses AI.
struct Fixture
{
    const juce::File root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures").getChildFile("ai-unit-synthetic-"+juce::Uuid().toString());
    Service service;ClientOptions options;std::unique_ptr<InstrumentProcessor> processor;FakeServices files;std::unique_ptr<studio::AiUnit> unit;std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::AudioBuffer<float> audio{2,512};juce::MidiBuffer midi;
    explicit Fixture(bool inEditor=false,bool empty=false)
    {
        CHECK(root.createDirectory().wasOk());options={{juce::File{},service.name,root},false};processor=std::make_unique<InstrumentProcessor>(options, airack::testing::isolatedRackRuntime());if(!empty)setup(*processor);CHECK(airack::testing::readyRackInventory(*processor));processor->prepareToPlay(48000,512);
        if(inEditor){editor.reset(processor->createEditor());editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);pump();}else make();
        std::cout<<"SYNTHETIC_FIXTURE "<<root.getFullPathName()<<std::endl;
    }
    ~Fixture(){unit.reset();editor.reset();processor->releaseResources();processor.reset();}
    void make(){unit=std::make_unique<studio::AiUnit>(*processor,*studio::studioSkin(),files.make());unit->addToDesktop(0);pump();}
    juce::Component& holder(){if(editor)return *editor;return *unit;}
    studio::Screen& screen(){return screenIn(holder());}
    bool open()
    {
        if(editor){const bool clicked=click(*editor,"ai.toggle");CHECK(clicked);if(!clicked)return false;}else unit->setVisible(true);
        const bool models=wait([&]{const auto* a=processor->aiAccountReply();return a&&a->models.size()==2&&!processor->aiWorkflowStatus().accountPending;});CHECK(models);pump();return models;
    }
    bool ready(){const auto* current=processor->aiCurrentRequest();const auto id=current?current->tag.requestId:0;const auto started=GetTickCount64();const bool rendered=wait([&]{std::lock_guard lock(service.backend->mutex);return std::any_of(service.backend->results.begin(),service.backend->results.end(),[&](const auto& r){return r.tag.requestId==id;});},15000);std::cout<<"NATIVE_RENDER_READY request="<<id<<" observed_ms="<<GetTickCount64()-started<<" ready="<<rendered<<std::endl;CHECK(rendered);if(!rendered)return false;const bool ok=wait([&]{return processor->aiProposalPhase()==AiProposalPhase::proposed||processor->aiProposalPhase()==AiProposalPhase::failed;});CHECK(ok);CHECK(processor->aiProposalPhase()==AiProposalPhase::proposed);pump();return ok&&processor->aiProposal();}
    // Back on CREATE SOUND's page by its button, from the account, linked or join page.
    void compose(){auto& s=screen();if(s.page=="connected")CHECK(click(holder(),"ai.account"));else if(s.page=="linked"||s.page=="join")CHECK(click(holder(),"ai.linked"));}
    // The options as asked (a standing proposal goes first: its page has no option rows), the prompt, then
    // GENERATE until a request is taken, as today's form waited for Generate to enable.
    bool generate(const char* instruction="sound",int scope=1,bool macros=false,bool reuse=false,std::optional<bool> createNotes={})
    {
        compose();auto& s=screen();auto& p=*processor;
        const int notes=createNotes?int(*createNotes):option(s,"ai.scope")!=scope-1?int(scope>1):option(s,"ai.requirePattern");
        const std::vector<std::pair<const char*,int>> wanted{{"ai.scope",scope-1},{"ai.requirePattern",notes},{"ai.allowMacros",int(macros)},{"ai.allowReuse",int(reuse)}};
        if(s.page=="proposal"&&std::any_of(wanted.begin(),wanted.end(),[&](const auto& w){return option(s,w.first)!=w.second;}))
        {if(p.aiProposalPhase()==AiProposalPhase::pending)p.cancelAi();else CHECK(click(holder(),"ai.discard"));pump();}
        for(const auto& [bind,value]:wanted)if(option(s,bind)!=value)CHECK(stepTo(s,bind,value));
        CHECK(enterText(s,"ai.prompt",instruction));
        const auto calls=service.backend->generationCalls.load();const auto* current=p.aiCurrentRequest();const auto prior=current?current->tag.requestId:0;
        const bool taken=wait([&]{const auto* r=p.aiCurrentRequest();if((r&&r->tag.requestId!=prior)||p.aiWorkflowStatus().generationWaiting)return true;click(holder(),"ai.generate");return false;});
        if(!taken)std::cout<<"NATIVE_GENERATE_REFUSED phase="<<int(p.aiProposalPhase())<<" shown="<<s.shown.status<<" status="<<status(holder())<<std::endl;
        CHECK(taken);if(!taken)return false;
        const bool arrived=wait([&]{return juce::String(instruction).startsWith("catalog-preview")?service.backend->generationCalls.load()>=calls+1:service.backend->generationCalls.load()==calls+1;});
        if(!arrived){const auto workflow=p.aiWorkflowStatus();const auto group=p.groupWorkflowView();std::cout<<"NATIVE_ARRIVAL_FAILURE expected_calls="<<calls+1<<" actual_calls="<<service.backend->generationCalls.load()<<" phase="<<int(p.aiProposalPhase())<<" connection="<<int(workflow.connection.phase)<<" group_ready="<<group.generationReady<<" group_busy="<<group.busy<<" group_conflicted="<<group.conflicted<<" status="<<status(holder())<<std::endl;}
        CHECK(arrived);return arrived&&ready();
    }
    bool refine(const char* instruction)
    {compose();CHECK(enterText(screen(),"ai.prompt",instruction));const auto calls=service.backend->generationCalls.load();if(!click(holder(),"ai.refine"))return false;const bool arrived=wait([&]{return service.backend->generationCalls.load()==calls+1;});CHECK(arrived);return arrived&&ready();}
    void process(){callback(*processor,audio,midi);pump();}
    void image(const char* name)
    {const auto file=root.getChildFile(juce::String(name)+".png");auto stream=file.createOutputStream();const auto picture=holder().createComponentSnapshot(holder().getLocalBounds(),true);CHECK(stream&&picture.isValid());if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(picture,*stream));}
};
void account_models()
{
    begin(__func__);Fixture f;auto& p=*f.processor;const auto manual=project(p);
    CHECK(f.service.backend->generationCalls.load()==0);for(const auto& n:f.service.backend->accountCalls)CHECK(n.load()==0);
    CHECK(p.aiWorkflowStatus().connection.phase==ClientPhase::closed);f.process();CHECK(project(p)==manual);
    if(!f.open())return;auto& u=*f.unit;auto& s=f.screen();
    CHECK(f.service.backend->accountCalls[0]==1&&f.service.backend->accountCalls[1]==1&&f.service.backend->accountCalls[3]==0);
    CHECK(p.aiAccountReply()&&!p.aiAccountReply()->modelVerified);CHECK(status(u).containsIgnoreCase("unverified"));
    // The model rows are on the account page, which ACCOUNT opens.
    CHECK(click(u,"ai.account"));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(s.page=="connected"&&s.shown.list.size()==2);if(s.shown.list.size()==2)CHECK(s.shown.list[0].contains("Synthetic A"));CHECK(pressItem(s,"ai.model",1));
    CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->selectedModel=="synthetic-b"&&!p.aiWorkflowStatus().accountPending;}));
    CHECK(f.service.backend->accountCalls[2]==1);CHECK(p.aiAccountReply()->models.size()==2);
    CHECK(!p.generateAi({"wrong model","undiscovered",{}}));CHECK(!p.generateAi({"wrong selection","synthetic-a",{}}));CHECK(!p.generateAi({"","synthetic-b",{}}));
    CHECK(f.generate());if(p.aiCurrentRequest())CHECK(p.aiCurrentRequest()->model=="synthetic-b");CHECK(f.service.backend->generationCalls==1);
    CHECK(project(p)==manual);CHECK(status(u).containsIgnoreCase("verified"));
    const auto pinned=*p.aiCurrentRequest();CHECK(!p.generateAi({"","synthetic-b",{}}));CHECK(sameRequest(*p.aiCurrentRequest(),pinned));
    for(auto state:{rack::oauth::Status::signedOut,rack::oauth::Status::accessExpired,rack::oauth::Status::busy})
    {
        p.cancelAi();
        CHECK(wait([&]{const auto connection=p.aiWorkflowStatus().connection;return connection.phase==ClientPhase::ready&&connection.status==RuntimeStatus::ready;}));
        f.service.backend->state(state);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));
        CHECK(!p.generateAi({"blocked","synthetic-b",{}}));pump();
        // GENERATE is refused, and says why.
        const auto generations=f.service.backend->generationCalls.load();CHECK(click(u,"ai.generate"));CHECK(f.service.backend->generationCalls==generations&&p.aiProposalPhase()==AiProposalPhase::idle);
        CHECK(s.shown.status=="Sign in and connect to generate");CHECK(status(u).isNotEmpty());
        if(state==rack::oauth::Status::signedOut||state==rack::oauth::Status::accessExpired){const auto calls=f.service.backend->accountCalls[3].load();CHECK(drawn(s,"button")=="SIGN IN");CHECK(pressItem(s,"ai.account"));CHECK(wait([&]{return f.service.backend->accountCalls[3]==calls+1&&!p.aiWorkflowStatus().accountPending;}));}
    }
    for(const auto& selected:{juce::String{},juce::String("undiscovered")})
    {f.service.backend->state(rack::oauth::Status::connected,RuntimeStatus::ready,selected);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));CHECK(!p.generateAi({"blocked",selected,{}}));}
    f.service.backend->state(rack::oauth::Status::connected,RuntimeStatus::busy);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));CHECK(!p.generateAi({"blocked","synthetic-a",{}}));
    CHECK(project(p)==manual);
}
void generation_diff()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& p=*f.processor;auto& u=*f.unit;auto& s=f.screen();racked(p);auto committed=project(p);
    CHECK(f.generate());auto request=f.service.backend->request(0);CHECK(request.source==committed&&request.policy.scope==RequestScope::sound&&!request.policy.allowMacroChanges&&!request.policy.allowSlotReuse);
    CHECK(request.capturedPhrase&&*request.capturedPhrase==committed.pattern);CHECK(!request.capturedBpm);CHECK(project(p)==committed);
    CHECK(status(u).containsIgnoreCase("captured")&&status(u).contains("120"));
    auto d=diff(u);CHECK(d.contains(parameterRegistry()[index("output.gain_db")].name));CHECK(d.contains("dB"));CHECK(d.containsIgnoreCase("automation"));CHECK(d.containsIgnoreCase("RMS"));CHECK(d.contains("Synthetic native"));
    CHECK(f.generate("parent",3,true,true));request=f.service.backend->request(1);CHECK(request.policy.scope==RequestScope::both&&request.policy.allowMacroChanges&&request.policy.allowSlotReuse);CHECK(project(p)==committed);
    d=diff(u);for(const char* term:{"Remove","Move","Synth 1","Macro 1","depth","64","60","automation","bank","route"})CHECK(d.containsIgnoreCase(term));
    CHECK(d.containsIgnoreCase("proposed"));
    // The proposal page has no option rows; the proposal keeps its captured policy.
    CHECK(s.page=="proposal"&&!offered(s,"ai.scope"));CHECK(diff(u).containsIgnoreCase("both"));
    // An invalid tempo leaves the context, and GENERATE and APPLY say why.
    const auto context=p.aiMusicalContext();CHECK(enterText(s,"ai.tempo","garbage"));CHECK(p.aiMusicalContext()==context);
    const auto calls=f.service.backend->generationCalls.load();CHECK(click(u,"ai.generate"));CHECK(f.service.backend->generationCalls==calls);CHECK(s.shown.status.containsIgnoreCase("tempo"));
    CHECK(click(u,"ai.apply"));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed);CHECK(s.shown.status.containsIgnoreCase("tempo"));CHECK(status(u).containsIgnoreCase("tempo"));
    CHECK(enterText(s,"ai.tempo","137"));CHECK(p.aiMusicalContext().auditionBpm==137);CHECK(!p.aiProposal());CHECK(enterText(s,"ai.role","bass"));CHECK(enterText(s,"ai.key","F#"));CHECK(enterText(s,"ai.scale","minor"));
    CHECK(f.generate("pattern",2));request=f.service.backend->request(2);CHECK(request.policy.scope==RequestScope::pattern&&request.capturedBpm==137&&request.role=="bass"&&request.key=="F#"&&request.scale=="minor");CHECK(p.aiProposal()->edits.parameters.empty());CHECK(status(u).contains("137"));
    p.cancelAi();committed=setup(p,false);CHECK(enterText(s,"ai.tempo",""));CHECK(f.generate());const auto* defaultProposal=p.aiProposal();CHECK(defaultProposal);if(!defaultProposal)return;CHECK(p.aiProposal()->audio.context.phraseSource==PhraseSource::defaultNotes&&p.aiProposal()->audio.context.tempoSource==TempoSource::defaultTempo);CHECK(status(u).containsIgnoreCase("default"));
    p.cancelAi();committed=setup(p);committed.pattern.meter={32,4};committed.pattern.bars=8;recall(p,committed);CHECK(enterText(s,"ai.tempo","20"));const bool rendered=f.generate();CHECK(rendered);if(!rendered)return;CHECK(p.aiProposal()->audio.phraseTruncated);CHECK(p.aiProposal()->audio.bodyFrames==maximumPreviewBodyFrames);CHECK(status(u).containsIgnoreCase("truncat"));
    p.cancelAi();committed.sound.controlLocks[index("output.gain_db")]=true;recall(p,committed);CHECK(airack::testing::generateAi(p,{"sound","synthetic-a",{}}));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::failed;}));CHECK(project(p)==committed);CHECK(!p.aiProposal());
}
void refinement()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& p=*f.processor;auto& u=*f.unit;auto& s=f.screen();racked(p);auto ownedFixture1=airack::testing::fixtureValue([&]{return project(p);});const auto& committed=*ownedFixture1;
    CHECK(enterText(s,"ai.tempo","400"));CHECK(enterText(s,"ai.role","bass"));CHECK(f.generate("parent",3,true,true));if(!p.aiProposal())return;
    auto ownedFixture2=airack::testing::fixtureValue([&]{return *p.aiProposal()->candidate;});const auto& parentCandidate=*ownedFixture2;auto ownedFixture3=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});const auto& parentRequest=*ownedFixture3;auto ownedFixture4=airack::testing::fixtureValue([&]{return *p.aiProposal();});const auto& parentResult=*ownedFixture4;
    // The refinement keeps the captured policy; the proposal page has no option rows to change.
    CHECK(f.refine("child-one"));if(!p.aiProposal())return;auto ownedFixture5=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});const auto& childOne=*ownedFixture5;auto ownedFixture6=airack::testing::fixtureValue([&]{return *p.aiProposal()->candidate;});const auto& candidateOne=*ownedFixture6;
    if(childOne.catalog){CHECK(parentRequest.catalog&&childOne.catalog->root==parentRequest.catalog->root&&childOne.source==committed&&childOne.tag.requestId>parentRequest.tag.requestId);CHECK(childOne.catalog->baseIntent.pattern==parentCandidate.pattern);}else CHECK(childOne.source==parentCandidate&&childOne.tag.requestId>parentRequest.tag.requestId);CHECK(childOne.policy.scope==parentRequest.policy.scope&&childOne.policy.allowMacroChanges&&childOne.policy.allowSlotReuse);CHECK(childOne.capturedBpm==parentRequest.capturedBpm&&childOne.role==parentRequest.role&&childOne.capturedPhrase&&*childOne.capturedPhrase==parentCandidate.pattern);
    CHECK(sameRequest(childOne,f.service.backend->request(1)));CHECK(sameResult(childOne,*p.aiProposal(),f.service.backend->result(1)));CHECK(p.aiOriginalSource()&&*p.aiOriginalSource()==committed);CHECK(p.aiIntendedEdits()&&p.aiIntendedEdits()->racks.size()==3);
    CHECK(f.refine("child-two"));if(!p.aiProposal())return;auto ownedFixture7=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});const auto& childTwo=*ownedFixture7;if(childTwo.catalog){CHECK(childOne.catalog&&childTwo.catalog->root==childOne.catalog->root&&childTwo.source==committed&&childTwo.tag.requestId>childOne.tag.requestId);CHECK(childTwo.catalog->baseIntent.pattern==candidateOne.pattern);}else CHECK(childTwo.source==candidateOne&&childTwo.tag.requestId>childOne.tag.requestId);CHECK(sameResult(childTwo,*p.aiProposal(),f.service.backend->result(2)));CHECK(project(p)==committed);
    const auto* intent=p.aiIntendedEdits();CHECK(intent);if(intent){const auto gain=std::find_if(intent->parameters.begin(),intent->parameters.end(),[](const auto& e){return e.id=="output.gain_db";});CHECK(gain!=intent->parameters.end());if(gain!=intent->parameters.end())CHECK(gain->normalized==.4f);CHECK(intent->macros.size()==1&&intent->macros[0].amount==.6f);CHECK(intent->racks.size()==4&&intent->reconciliation.remove.size()==1);CHECK(intent->pattern&&intent->pattern->notes[0].pitch==72);auto ownedFixture17=airack::testing::fixtureValue([&]{return ProjectDocument{};});auto& aggregate=*ownedFixture17;CHECK(admitProposal(committed,parentRequest.policy,*intent,aggregate)==ProposalStatus::accepted);CHECK(aggregate==*p.aiProposal()->candidate);}
    CHECK(diff(u).contains("Remove effect bank 2")&&diff(u).contains("Move effect bank 1"));CHECK(diff(u).contains(parameterRegistry()[index("output.gain_db")].name));
    auto ownedFixture8=airack::testing::fixtureValue([&]{return parentResult;});auto& foreign=*ownedFixture8;CHECK(!p.receiveAiResult(std::move(foreign)));CHECK(sameRequest(*p.aiCurrentRequest(),childTwo));
    auto ownedFixture9=airack::testing::fixtureValue([&]{return *p.aiProposal();});const auto& priorResult=*ownedFixture9;auto ownedFixture10=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});const auto& priorRequest=*ownedFixture10;CHECK(f.refine("fail"));CHECK(status(u).containsIgnoreCase("previous proposal retained"));CHECK(sameRequest(*p.aiCurrentRequest(),priorRequest));CHECK(sameResult(priorRequest,*p.aiProposal(),priorResult));CHECK(project(p)==committed);
    p.control(index("output.gain_db")).setValue(.8f);p.control(8).setValue(.7f);auto ownedFixture11=airack::testing::fixtureValue([&]{return project(p);});const auto& beforeApply=*ownedFixture11;CHECK(click(u,"ai.apply"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::idle;}));CHECK(project(p).sound.values[index("output.gain_db")]==.4f);CHECK(project(p).sound.values[8]==.7f);CHECK(p.undoSoundEdit());CHECK(project(p)==beforeApply);
    CHECK(f.generate("empty",3,false,false,false));CHECK(p.aiProposal()->candidate->pattern.noteCount==0);CHECK(f.refine("empty-child"));if(p.aiCurrentRequest()->catalog){CHECK(p.aiCurrentRequest()->capturedPhrase&&p.aiCurrentRequest()->capturedPhrase->noteCount==0);CHECK(p.aiCurrentRequest()->source.pattern==beforeApply.pattern);CHECK(p.aiProposal()->edits.pattern&&p.aiProposal()->edits.pattern->noteCount==0&&p.aiIntendedEdits()->pattern&&p.aiIntendedEdits()->pattern->noteCount==0);CHECK(p.aiProposal()->audio.context.phraseSource==PhraseSource::proposed&&p.aiProposal()->audio.context.phrase.noteCount==0);CHECK(status(u).containsIgnoreCase("proposed"));}else{CHECK(!p.aiCurrentRequest()->capturedPhrase);CHECK(p.aiCurrentRequest()->source.pattern.noteCount==0);CHECK(p.aiProposal()->audio.context.phraseSource==PhraseSource::defaultNotes);CHECK(status(u).containsIgnoreCase("default"));}CHECK(project(p)==beforeApply);
    CHECK(f.generate("rack63",1));const auto* rack63Proposal=p.aiProposal();const auto* rack63Request=p.aiCurrentRequest();CHECK(rack63Proposal&&rack63Request);if(!rack63Proposal||!rack63Request)return;auto ownedFixture12=airack::testing::fixtureValue([&]{return *p.aiProposal();});const auto& rackPrior=*ownedFixture12;auto ownedFixture13=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});const auto& rackRequest=*ownedFixture13;CHECK(f.refine("rack2"));CHECK(sameRequest(*p.aiCurrentRequest(),rackRequest));CHECK(sameResult(rackRequest,*p.aiProposal(),rackPrior));CHECK(status(u).containsIgnoreCase("previous proposal retained"));
    // The existing63/+2 case stays intact.64 is legal; a further edit65 must retain it.
    CHECK(f.refine("rack1"));CHECK(p.aiCurrentRequest()&&p.aiProposal()&&p.aiIntendedEdits()&&p.aiIntendedEdits()->racks.size()==64);
    if(!p.aiCurrentRequest()||!p.aiProposal())return;
    auto ownedFixture14=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});const auto& limitRequest=*ownedFixture14;auto ownedFixture15=airack::testing::fixtureValue([&]{return *p.aiProposal();});const auto& limitResult=*ownedFixture15;
    CHECK(limitRequest.tag.requestId>rackRequest.tag.requestId);CHECK(limitResult.edits.racks.size()==(limitRequest.catalog?64:1));
    CHECK(f.refine("rack1"));CHECK(sameRequest(*p.aiCurrentRequest(),limitRequest));CHECK(sameResult(limitRequest,*p.aiProposal(),limitResult));
    CHECK(p.aiIntendedEdits()&&p.aiIntendedEdits()->racks.size()==64);CHECK(status(u).containsIgnoreCase("previous proposal retained"));
    CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.refine"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::pending;}));CHECK(!p.aiProposal());p.cancelAi();f.service.backend->release.store(true);CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump(80);CHECK(p.aiProposalPhase()==AiProposalPhase::idle&&!p.aiProposal()&&!p.aiIntendedEdits());
    f.service.backend->release.store(false);CHECK(f.generate());CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.refine"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::pending;}));auto ownedFixture16=airack::testing::fixtureValue([&]{return project(p);});auto& recalled=*ownedFixture16;recalled.sound.values[index("output.gain_db")]=.6f;recall(p,recalled);f.service.backend->release.store(true);pump(100);p.pollAi();CHECK(!p.aiProposal()&&!p.aiIntendedEdits());CHECK(project(p)==recalled);
}
struct ReentrantPreview final:juce::AudioProcessorParameter::Listener
{
    InstrumentProcessor& p;bool started=false;explicit ReentrantPreview(InstrumentProcessor& owner):p(owner){}
    void parameterValueChanged(int,float) override {}
    void parameterGestureChanged(int,bool beginning) override {if(beginning&&!started)started=p.startAiPreview();}
};
void preview_apply()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& p=*f.processor;const auto committed=project(p);CHECK(f.generate());if(!p.aiProposal())return;
    CHECK(click(*f.unit,"ai.preview"));CHECK(p.aiPreviewPlaying());f.process();CHECK(click(*f.unit,"ai.apply"));CHECK(p.aiWorkflowStatus().applyPending);CHECK(project(p)==committed);pump(80);CHECK(project(p)==committed);CHECK(status(*f.unit).containsIgnoreCase("waiting for audio"));
    f.process();CHECK(wait([&]{return !p.aiWorkflowStatus().applyPending;}));CHECK(p.aiProposalPhase()==AiProposalPhase::idle);auto applied=project(p);CHECK(applied!=committed);pump(80);CHECK(project(p)==applied);CHECK(p.undoSoundEdit());CHECK(project(p)==committed);CHECK(!p.undoSoundEdit());
    for(const char* intervening:{"manual","context","discard","generate","cancel","closure"})
    {
        CHECK(f.generate());CHECK(p.startAiPreview());f.process();CHECK(p.applyAiWhenStopped());CHECK(p.aiWorkflowStatus().applyPending);auto expected=project(p);
        if(std::string(intervening)=="manual"){expected.sound.values[index("output.gain_db")]=.6f;recall(p,expected);}
        else if(std::string(intervening)=="context"){auto c=p.aiMusicalContext();c.role+="x";CHECK(p.setAiMusicalContext(c));}
        else if(std::string(intervening)=="discard")p.discardAiProposal();
        else if(std::string(intervening)=="generate")
        {
            const auto* previous=p.aiCurrentRequest();const auto prior=previous?previous->tag.requestId:0;
            CHECK(airack::testing::generateAi(p,{"sound","synthetic-a",{}}));
            const auto* current=p.aiCurrentRequest();CHECK(current&&current->tag.requestId>prior);
            CHECK(!p.aiWorkflowStatus().applyPending);CHECK(!p.aiWorkflowStatus().generationWaiting);
            CHECK(f.ready());
        }
        else if(std::string(intervening)=="cancel")p.cancelAi();
        else {f.unit.reset();CHECK(p.aiWorkflowStatus().connection.phase==ClientPhase::closed);}
        f.process();p.pollAi();CHECK(!p.aiWorkflowStatus().applyPending);CHECK(project(p)==expected);
        if(!f.unit){f.make();CHECK(f.open());}
    }
    CHECK(f.generate());ReentrantPreview listener(p);const auto field=index("output.gain_db");p.control(field).addListener(&listener);const auto before=project(p);CHECK(p.applyAiWhenStopped());p.pollAi();p.control(field).removeListener(&listener);CHECK(listener.started);CHECK(!p.aiWorkflowStatus().applyPending);CHECK(project(p)==before);CHECK(p.aiProposal());CHECK(p.aiWorkflowStatus().message.containsIgnoreCase("retry"));p.stopAiPreview();f.process();CHECK(p.applyAiWhenStopped());CHECK(wait([&]{return !p.aiWorkflowStatus().applyPending;}));
    CHECK(f.generate());CHECK(p.aiProposal()&&p.aiProposal()->candidate);if(!p.aiProposal()||!p.aiProposal()->candidate)return;
    const auto releaseExpected=std::make_unique<ProjectDocument>(*p.aiProposal()->candidate);
    CHECK(p.startAiPreview());f.process();CHECK(p.applyAiWhenStopped());p.releaseResources();p.pollAi();CHECK(!p.aiWorkflowStatus().applyPending);CHECK(p.aiProposalPhase()==AiProposalPhase::idle);CHECK(project(p)==*releaseExpected);
}
void client_lifecycle()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& p=*f.processor;const auto committed=project(p);
    CHECK(enterText(f.screen(),"ai.prompt","hold"));CHECK(click(*f.unit,"ai.generate"));CHECK(wait([&]{return f.service.backend->generationCalls==1;}));CHECK(p.aiProposalPhase()==AiProposalPhase::pending);f.process();CHECK(project(p)==committed);
    f.unit.reset();CHECK(p.aiWorkflowStatus().connection.phase==ClientPhase::closed&&!p.aiProposal()&&!p.aiCurrentRequest());f.service.backend->release.store(true);CHECK(wait([&]{return f.service.backend->cancelCalls>0;}));
    CompanionClient other;CHECK(other.start(f.options));CHECK(wait([&]{other.poll();return other.status().phase==ClientPhase::ready;}));CHECK(other.account({AccountAction::status,{}}));AccountReply reply;CHECK(wait([&]{other.poll();return other.takeAccount(reply);}));CHECK(reply.providerStatus==rack::oauth::Status::connected);other.close();
    // A new unit asks nothing until it shows.
    const auto calls=f.service.backend->generationCalls.load();const auto accounts=f.service.backend->accountCalls[0].load();f.make();pump(80);CHECK(f.service.backend->generationCalls==calls&&f.service.backend->accountCalls[0]==accounts);CHECK(p.aiWorkflowStatus().connection.phase==ClientPhase::closed);CHECK(project(p)==committed);CHECK(f.open());
    CHECK(f.generate());CHECK(p.startAiPreview());f.process();f.service.halt();CHECK(wait([&]{return p.aiWorkflowStatus().connection.phase==ClientPhase::failed;}));CHECK(!p.aiProposal()&&!p.aiCurrentRequest()&&!p.aiIntendedEdits());f.process();CHECK(p.aiPreviewStopped());CHECK(project(p)==committed);CHECK(status(*f.unit).containsIgnoreCase("unavailable"));
    // RETRY on the offline page.
    f.service.start();pump();CHECK(f.screen().page=="alert");CHECK(pressItem(f.screen(),"ai.account"));CHECK(wait([&]{return p.aiAccountReply()&&p.aiAccountReply()->models.size()==2&&!p.aiWorkflowStatus().accountPending;}));CHECK(f.generate());f.process();CHECK(project(p)==committed);
    CHECK(enterText(f.screen(),"ai.prompt","hold"));f.service.backend->release.store(false);CHECK(click(*f.unit,"ai.generate"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::pending;}));pump();CHECK(pressItem(f.screen(),"ai.cancel"));CHECK(p.aiProposalPhase()==AiProposalPhase::idle);f.service.backend->release.store(true);pump(100);CHECK(!p.aiProposal());
    p.releaseResources();p.prepareToPlay(48000,512);juce::MidiBuffer notes;notes.ensureSize(128);notes.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);callback(p,f.audio,notes);CHECK(f.audio.getMagnitude(0,512)>0);CHECK(project(p)==committed);
    auto sound=committed.sound;sound.controlLocks[index("output.gain_db")]=true;CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,revision(p)));CHECK(p.saveSoundPreset(f.root.getChildFile("manual.airack")));CHECK(p.loadSoundPreset(f.root.getChildFile("manual.airack")));CHECK(project(p).sound.controlLocks[index("output.gain_db")]);
    const auto audioPath=juce::File(RACKSYNTH_WORKSPACE).getChildFile("src/plugin/InstrumentProcessor.cpp").loadFileAsString();const auto block=audioPath.fromFirstOccurrenceOf("void InstrumentProcessor::processBlock",false,false).upToFirstOccurrenceOf("void InstrumentProcessor::",false,false);CHECK(!block.contains("pollAi")&&!block.contains("aiClient")&&!block.contains("CompanionClient")&&!block.contains("requestAiAccount"));
}
bool reachable(juce::Component& root,juce::Component& child)
{const auto r=root.getLocalArea(&child,child.getLocalBounds());return child.isShowing()&&r.getWidth()>0&&r.getHeight()>0&&root.getLocalBounds().contains(r.getCentre());}
namespace task11_envelope_draft
{
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{
    auto* value=new juce::DynamicObject;
    for(const auto& field:fields)value->setProperty(field.first,field.second);
    return value;
}
std::string escaped(std::size_t bytes)
{
    std::string result;result.reserve(bytes);
    for(std::size_t n=0;n<bytes;++n)result.push_back(n%3==0?'\x01':n%3==1?'"':'\\');
    return result;
}
juce::String string(const std::string& text)
{return juce::String::fromUTF8(text.data(),int(text.size()));}

// Use the product JSON emitter with the EXACT precision17 used by
// GenerationRequest.cpp:315. Count into a sink before allocating output backing.
// This does not reconstruct the provider object or count unescaped characters.
struct Count final:juce::OutputStream
{
    std::size_t count=0,ceiling=0;bool valid=true;
    explicit Count(std::size_t limit):ceiling(limit){}
    juce::int64 getPosition() override{return juce::int64(count);}
    bool setPosition(juce::int64) override{return false;}
    void flush() override{}
    bool write(const void*,std::size_t bytes) override
    {
        if(!valid)return false;
        if(bytes>ceiling-count){valid=false;return false;}
        count+=bytes;return true;
    }
};
bool bytes(const juce::var& value,std::size_t ceiling,std::size_t& output)
{
    Count sink(ceiling);juce::JSON::writeToStream(sink,value,true,17);
    if(!sink.valid)return false;output=sink.count;return true;
}

// Large permitted text/control metadata and deliberately long finite native
// values. Only one saved default per unit keeps this complete 61-entry inventory
// within the adopted 65536 page fence; controls need not duplicate defaults.
RackDesign recipe(NativeModule module,bool large)
{
    RackDesign d;d.role=module<NativeModule::drive?RackRole::synth:RackRole::fx;
    d.name=large?string(escaped(64)):juce::String("Quoted \"library\"");
    d.intent=large?string(escaped(512)):juce::String("Explicit \\ native path-independent recipe");
    d.revision=UINT64_MAX;d.units={{0,module,{}}};
    d.panel.layout=d.role==RackRole::synth?PanelLayout::instrumentSections:PanelLayout::orderedFxStrip;
    d.panel.sections=large?std::vector<juce::String>{d.name,d.name,d.name,d.name}:std::vector<juce::String>{"Native"};
    const auto prefix=juce::String(d.role==RackRole::synth?"synth.1.":"effect.1.");
    const auto key=juce::String(d.role==RackRole::synth?"enabled":"bypassed");
    const auto defaultIndex=rackParameterIndex(module,0,key);CHECK(defaultIndex.has_value());
    if(defaultIndex)d.units[0].defaults.push_back({0,key,defaultParameterValues()[*defaultIndex]});
    for(const auto& p:parameterRegistry())if(p.id.startsWith(prefix))
    {
        const auto local=p.id.substring(prefix.length());
        if(!large||!rackParameterActive(module,local)||p.minimum>=p.maximum||d.panel.controls.size()==8)continue;
        d.panel.controls.push_back({d.name,0,std::uint8_t(d.panel.controls.size()%4),local,
            p.kind==ParameterKind::continuous?FocusWidget::rotary:p.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice,
            FocusResponse::linear,p.minimum,p.maximum});
    }
    CHECK(!large||d.panel.controls.size()==8);
    CHECK(validateRackDesign(d,RackValidationMode::portableDesign));return d;
}
ProjectDocument source()
{
    ProjectDocument d;SoundDocument next;RackDifference difference;
    for(std::uint8_t slot=0;slot<12;++slot)
    {
        const bool synth=slot<4;RackOperation operation;
        operation.kind=slot==0?RackOperationKind::adopt:RackOperationKind::place;
        operation.newInstance=juce::Uuid();operation.design=recipe(synth?NativeModule::analog:NativeModule::frequencySplit,true);
        operation.allocation={{0,std::uint8_t(synth?slot:slot-4)}};
        operation.destination={synth?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(synth?slot:slot-4)};
        const auto result=makeRackCandidate(d.sound,operation,SoundEditOrigin::manual,next,difference);
        CHECK(result==RackCandidateStatus::ready);if(result!=RackCandidateStatus::ready)return d;d.sound=next;
    }
    for(std::size_t n=0;n<parameterCount;++n)
    {
        if(parameterRegistry()[n].kind==ParameterKind::continuous)
            d.sound.values[n]=std::bit_cast<float>(std::uint32_t(0x3e800001u+n));
        d.sound.controlLocks[n]=n%7==0;
    }
    d.sound.values[index("output.gain_db")]=.05f;
    for(std::size_t n=0;n<8;++n)d.sound.macros[n]={parameterRegistry()[0].stateKey,float(int(n)-4)/8};
    d.pattern.bars=8;d.pattern.noteCount=256;
    for(std::size_t n=0;n<256;++n)d.pattern.notes[n]={
        (double(n)+.123456789012345)/8,.03123456789012345,std::uint8_t(n%128),std::uint8_t(1+n%127),std::uint8_t(1+n%16)};
    CHECK(d.sound.personalities&&d.sound.personalities->instances.size()==12);
    CHECK(validateProjectDocument(d));return d;
}
GroupUuid id(unsigned n)
{GroupUuid value;value.bytes[0]=std::uint8_t(n);value.bytes[15]=0x91;return value;}
void intent(ProjectDocument& d,unsigned member)
{
    GroupIntent group;group.group=id(90);group.member=id(member);group.bpm=400;
    for(auto* field:{&group.name,&group.role,&group.key,&group.scale})CHECK(setGroupText(escaped(128),*field));
    d.group=group;CHECK(validateProjectDocument(d));
}
std::unique_ptr<GroupPerformanceCapture> peers(const GenerationRequest& r)
{
    auto c=std::make_unique<GroupPerformanceCapture>();c->ownership.registry=id(99);
    c->ownership.requesterRuntime=GroupUuid(r.tag.runtime);c->ownership.loadRevision=1;c->ownership.revision=432;
    c->ownership.requester=*r.source.group;c->requesterEditRevision=r.tag.editRevision;c->peerCount=4;
    for(unsigned n=0;n<4;++n)
    {
        auto peer=r.source;intent(peer,n+2);c->ownership.peers.push_back({id(n+50),1,n+20,peer});
        auto& p=c->playback[n];p.controls.values=peer.sound.values;
        p.controls.availability=ObservedMidiAvailability::stale;p.controls.reason=LiveControlReason::clockRollback;p.controls.ageMs=UINT64_MAX;
        p.notes.meter=PhraseMeter{4,4};p.notes.bpm=400;p.notes.windowBeats=16;p.notes.ageMs=UINT64_MAX;
        p.notes.availability=ObservedMidiAvailability::stale;p.notes.reason=ObservedMidiReason::clockRollback;
        p.notes.truncated=true;p.notes.noteCount=256;
        for(std::size_t k=0;k<256;++k)p.notes.notes[k]={
            (double(k)+.123456789012345)/32,k%2?.01561234567890123:0.,
            std::uint8_t(k%128),std::uint8_t(1+k%127),std::uint8_t(1+k%16),k%2==0};
    }
    CHECK(validateGroupPerformanceCapture(*c));return c;
}
struct Directory
{
    juce::File root=juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("Task11-native-envelope","",false);
    Directory(){CHECK(root.createDirectory().wasOk());}
    ~Directory(){CHECK(root.deleteRecursively());}
};
}

void native_catalog_provider_envelope_preflight()
{
    begin(__func__);using namespace task11_envelope_draft;
    CHECK(maximumGenerationRequestBytes==917504);
    CHECK(rack::oauth::maximumStructuredPromptBytes==1048576);
    CHECK(rack::oauth::maximumStructuredRequestBytes==2097152);
    Directory directory;GenerationRequest r;r.tag={juce::Uuid(id(40).getRawData()),917,813,719};
    r.source=source();if(!r.source.sound.personalities||r.source.sound.personalities->instances.size()!=12)return;
    intent(r.source,1);r.model=juce::String::repeatedString("m",256);r.instruction=escaped(8192);
    r.role=std::string(groupTextView(r.source.group->role));r.key=std::string(groupTextView(r.source.group->key));
    r.scale=std::string(groupTextView(r.source.group->scale));r.capturedPhrase=r.source.pattern;r.capturedBpm=400;
    r.policy.scope=RequestScope::both;
    const auto original=r;auto linked=peers(r);const auto originalLinked=*linked;

    // The binary wire contains no catalog. This proves the same real rich source
    // is accepted; do not append JSON descriptors or infer combined Wire admission.
    juce::MemoryBlock wire;CHECK(encodeRequest(r,wire));CHECK(wire.getSize()<=maximumGenerationRequestBytes);
    GenerationRequest decoded;CHECK(decodeRequest({static_cast<const std::byte*>(wire.getData()),wire.getSize()},decoded));
    CHECK(sameRequest(decoded,r));CHECK(decoded.source==r.source);

    std::vector<RackDesign> embedded;for(const auto& i:r.source.sound.personalities->instances)embedded.push_back(i.design);
    for(unsigned n=0;n<32;++n)
    {
        auto d=recipe(NativeModule::delay,false);d.name="Quoted \"library\" "+juce::String(int(n));
        embedded.push_back(d);
    }
    RackCatalogSnapshot snapshot;RackCatalogStatus status=RackCatalogStatus::unavailable;
    auto factory=airack::test::frozenLibraryRackDesigns();const auto historicalTile=airack::test::frozenTileArrayRackDesign();
    CHECK(factory.size()==28&&std::count(factory.begin(),factory.end(),historicalTile)==1);
    std::erase(factory,historicalTile);CHECK(factory.size()==27);
    CHECK(captureRackCatalog(factory,embedded,{},snapshot,&status));CHECK(status==RackCatalogStatus::ready);
    CHECK(snapshot.entries.size()==71);
    const auto character=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[](const auto& e){return e.definition.name=="Character Drive";});
    CHECK(character!=snapshot.entries.end());
    const auto bandColor=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[](const auto& e){return e.definition.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf02");});
    CHECK(bandColor!=snapshot.entries.end());
    juce::var complete;CHECK(catalogInventoryFor(snapshot,complete));
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=snapshot.id;
    authority->descriptorJson=juce::JSON::toString(complete,true,22).toStdString();
    auto current=r;CatalogRequestContext context;context.root=r.tag;context.authority=authority;current.catalog=context;
    juce::MemoryBlock currentWire;CHECK(encodeRequest(current,currentWire)&&currentWire.getSize()<=maximumGenerationRequestBytes);
    GenerationRequest currentDecoded;CHECK(decodeRequest({static_cast<const std::byte*>(currentWire.getData()),currentWire.getSize()},currentDecoded)&&sameRequest(current,currentDecoded));
    std::cout<<"TASK2_COMPLETE_CATALOG entries="<<snapshot.entries.size()<<" descriptor="<<authority->descriptorJson.size()<<" request="<<currentWire.getSize()<<std::endl;
    // Keep the64KiB page fence while collecting the current71-entry inventory.
    std::size_t catalogCursor=0,catalogEntries=0;bool catalogComplete=false;
    for(std::size_t pageIndex=0;pageIndex<71&&!catalogComplete;++pageIndex){RackCatalogPage page;
        CHECK(catalogPage(snapshot,{},catalogCursor,65536,page,&status));
        CHECK(page.next>catalogCursor&&!page.entries.empty());if(page.next<=catalogCursor||page.entries.empty())return;
        std::size_t escaped=0;CHECK(rackCatalogEscapedSize(rackCatalogPageDescriptor(page),65536,escaped));
        catalogEntries+=page.entries.size();catalogCursor=page.next;catalogComplete=page.complete;
    }
    CHECK(catalogComplete&&catalogEntries==71&&catalogCursor==71);
    if(!catalogComplete||catalogEntries!=71)return;
    juce::var native;CHECK(providerCatalogFor(r.source,snapshot,native));
    CHECK(native["rack_catalog"]["snapshot"].toString()==snapshot.id.toString());
    const auto* inventory=native["rack_catalog"]["entries"].getArray();CHECK(inventory&&inventory->size()==71);
    if(!inventory||inventory->size()!=71)return;

    // Obtain the real protected GenerationRequest envelope through its existing
    // callback, including all policy/context/schema/linked source fields.
    juce::var actual,schema;std::string actualPrompt;unsigned generatedCalls=0;
    const auto generated=generateProposal(r,[&](const juce::String& model,std::string_view prompt,const juce::var& outputSchema)
    {
        ++generatedCalls;CHECK(model==r.model);actualPrompt.assign(prompt);schema=outputSchema.clone();
        CHECK(parseStrictJson(prompt,actual,rack::oauth::maximumStructuredPromptBytes));
        rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::networkFailure;
        response.result.httpStatus=429;response.result.errorCode="rate_limit_exceeded";return response;
    },{},linked.get());
    CHECK(generatedCalls==1&&generated.inferenceCalls==1&&generated.status==GenerationStatus::providerFailure);
    CHECK(actual.getDynamicObject()!=nullptr);CHECK(sameRequest(r,original)&&*linked==originalLinked);
    if(generatedCalls!=1||!actual.getDynamicObject())return;
    CHECK(actual["instruction"].toString().toStdString()==r.instruction);
    CHECK(actual["linked_context"]["peers"].getArray()&&actual["linked_context"]["peers"].getArray()->size()==4);

    // Prospective replacement only: no Task12 backend snapshot injection exists.
    auto full=actual.clone();full.getDynamicObject()->setProperty("catalog",native);
    auto base=full.clone();RackCatalogPage empty;empty.snapshot=snapshot.id;
    const auto emptyDescriptor=rackCatalogPageDescriptor(empty);
    base["catalog"].getDynamicObject()->setProperty("rack_catalog",emptyDescriptor);
    std::size_t fullBytes=0,baseBytes=0,emptyBytes=0;
    const bool fullCounted=bytes(full,3997696,fullBytes),baseCounted=bytes(base,3997696,baseBytes),emptyCounted=bytes(emptyDescriptor,65536,emptyBytes);
    CHECK(fullCounted&&baseCounted&&emptyCounted);if(!fullCounted||!baseCounted||!emptyCounted)return;
    CHECK(baseBytes>=emptyBytes&&baseBytes-emptyBytes<rack::oauth::maximumStructuredPromptBytes);
    if(baseBytes<emptyBytes||baseBytes-emptyBytes>=rack::oauth::maximumStructuredPromptBytes)return;
    const auto remaining=rack::oauth::maximumStructuredPromptBytes-(baseBytes-emptyBytes);
    const bool prospectiveRawOverflow=fullBytes>rack::oauth::maximumStructuredPromptBytes;
    std::cout<<"TASK11_PROVIDER_PREFLIGHT wire="<<wire.getSize()<<" current_prompt="<<actualPrompt.size()
        <<" complete_catalog_prompt="<<fullBytes<<" envelope_prefix="<<baseBytes-emptyBytes
        <<" exact_remaining_raw="<<remaining<<" prospective_raw_overflow="<<prospectiveRawOverflow<<std::endl;

    // Use the REAL pinned final provider emitter and an isolated synthetic store.
    const auto storeDirectory=directory.root.getChildFile("synthetic-store");CHECK(storeDirectory.createDirectory().wasOk());
    {rack::oauth::detail::Store store(storeDirectory);std::atomic_bool cancelled=false;CHECK(store.lock(cancelled));
        CHECK(store.save(object({{"access_token","synthetic-local-test"},{"scope","chatgpt.tokens.use.direct resource.invoke"},
            {"expires_at",juce::var(juce::int64(2000))}})));}
    unsigned transportCalls=0;std::size_t emittedBodyBytes=0;std::string expected;
    rack::oauth::OAuthProbe probe(storeDirectory,[&](const rack::oauth::HttpRequest& request,const std::atomic_bool&)
    {
        ++transportCalls;CHECK(request.method=="POST"&&request.url.endsWith("/responses"));
        emittedBodyBytes=request.body.getNumBytesAsUTF8();CHECK(emittedBodyBytes<=rack::oauth::maximumStructuredRequestBytes);
        const auto body=juce::JSON::parse(request.body);const auto* input=body["input"].getArray();CHECK(input&&input->size()==1);
        if(input&&input->size()==1)CHECK((*input)[0]["content"].toString().toStdString()==expected);
        CHECK(juce::JSON::toString(body["text"]["format"]["schema"],true,17)==juce::JSON::toString(schema,true,17));
        return rack::oauth::HttpResponse{429,"{\"error\":{\"code\":\"rate_limit_exceeded\"}}"};
    },[]{return int64_t(1000);});
    // Use the current request's actual catalog-aware generator and final provider emitter.
    unsigned currentProviderCalls=0,currentHttpCalls=0;std::size_t currentPromptBytes=0,currentBodyBytes=0,currentSchemaBytes=0;
    std::string currentPrompt;juce::var currentSchema;
    rack::oauth::OAuthProbe currentProbe(storeDirectory,[&](const rack::oauth::HttpRequest& http,const std::atomic_bool&){
        ++currentHttpCalls;currentBodyBytes=http.body.getNumBytesAsUTF8();
        CHECK(currentBodyBytes<=rack::oauth::maximumStructuredRequestBytes);
        const auto body=juce::JSON::parse(http.body);const auto* input=body["input"].getArray();
        CHECK(input&&input->size()==1&&(*input)[0]["content"].toString().toStdString()==currentPrompt);
        CHECK(juce::JSON::toString(body["text"]["format"]["schema"],true,17)==juce::JSON::toString(currentSchema,true,17));
        return rack::oauth::HttpResponse{429,"{\"error\":{\"code\":\"rate_limit_exceeded\"}}"};
    },[]{return int64_t(1000);});
    const auto currentResult=generateProposal(current,[&](const juce::String& model,std::string_view prompt,const juce::var& outputSchema){
        ++currentProviderCalls;CHECK(model==current.model);currentPrompt.assign(prompt);currentSchema=outputSchema.clone();
        currentPromptBytes=prompt.size();currentSchemaBytes=juce::JSON::toString(outputSchema,true,17).getNumBytesAsUTF8();
        CHECK(currentPromptBytes<=rack::oauth::maximumStructuredPromptBytes&&currentSchemaBytes<=rack::oauth::maximumStructuredSchemaBytes);
        juce::var payload;CHECK(parseStrictJson(prompt,payload,rack::oauth::maximumStructuredPromptBytes));
        CHECK(juce::JSON::toString(payload["catalog"]["rack_catalog"],true,22)==juce::JSON::toString(complete,true,22));
        CHECK(payload["linked_context"]["peers"].isArray()&&payload["linked_context"]["peers"].getArray()->size()==4);
        return currentProbe.requestStructured(model,prompt,outputSchema);
    },{},linked.get());
    CHECK(currentProviderCalls==1&&currentHttpCalls==1&&currentResult.inferenceCalls==1&&currentResult.status==GenerationStatus::providerFailure);
    CHECK(!currentResult.candidate&&currentResult.audio.left.empty()&&currentResult.audio.right.empty());
    CHECK(sameRequest(current,currentDecoded));
    std::cout<<"TASK2_CURRENT_PROVIDER entries="<<snapshot.entries.size()<<" prompt="<<currentPromptBytes<<" schema="<<currentSchemaBytes<<" body="<<currentBodyBytes<<" calls="<<currentProviderCalls<<'/'<<currentHttpCalls<<std::endl;
    bool finalOverflow=false;
    if(!prospectiveRawOverflow)
    {
        expected=juce::JSON::toString(full,true,17).toStdString();CHECK(expected.size()==fullBytes);
        const auto result=probe.requestStructured(r.model,expected,schema);
        finalOverflow=result.result.errorCode=="structured_request_limit";
        CHECK(finalOverflow?transportCalls==0:transportCalls==1&&result.result.httpStatus==429);
        std::cout<<"TASK11_PROVIDER_FINAL complete_body="<<emittedBodyBytes<<" final_overflow="<<finalOverflow<<std::endl;
    }
    std::cout<<"TASK11_FALLBACK_WITNESS observed="<<(prospectiveRawOverflow||finalOverflow)
        <<" scope=prospective_catalog_replacement"<<std::endl;
    // If neither crosses a pinned product cap, retain this NO-TRIGGER result.
    // It cannot authorize adding a provider fallback branch.

    // A caller may lower the output allowance. Use the smallest bounded page
    // that accommodates every individual descriptor, within the exact remaining
    // real envelope budget; this gives multiple pages without inventing an outer
    // provider cap. catalogPage counts precision22 conservatively before backing.
    std::size_t pageBudget=0;
    for(std::size_t cursor=0;cursor<snapshot.entries.size();++cursor)
    {
        RackCatalogPage one;one.snapshot=snapshot.id;one.entries={snapshot.entries[cursor]};
        one.next=cursor+1;one.complete=one.next==snapshot.entries.size();std::size_t size=0;
        CHECK(rackCatalogEscapedSize(rackCatalogPageDescriptor(one),65536,size));pageBudget=(std::max)(pageBudget,size);
    }
    CHECK(pageBudget<=65536&&pageBudget<=remaining);if(pageBudget>remaining||!pageBudget)return;
    std::set<std::string> visited;std::size_t cursor=0,pages=0;
    while(cursor<snapshot.entries.size()&&pages<snapshot.entries.size())
    {
        RackCatalogPage page;CHECK(catalogPage(snapshot,{},cursor,pageBudget,page,&status));
        CHECK(status==RackCatalogStatus::ready&&page.snapshot==snapshot.id&&!page.entries.empty()&&page.next>cursor);
        if(page.entries.empty()||page.next<=cursor)return;
        const auto descriptor=rackCatalogPageDescriptor(page);std::size_t size=0;
        CHECK(rackCatalogEscapedSize(descriptor,pageBudget,size));CHECK(size<=remaining);
        auto envelope=base.clone();envelope["catalog"].getDynamicObject()->setProperty("rack_catalog",descriptor);
        std::size_t promptBytes=0;const bool counted=bytes(envelope,rack::oauth::maximumStructuredPromptBytes,promptBytes);
        CHECK(counted);if(!counted)return;
        expected=juce::JSON::toString(envelope,true,17).toStdString();CHECK(expected.size()==promptBytes);
        const auto calls=transportCalls;const auto result=probe.requestStructured(r.model,expected,schema);
        CHECK(transportCalls==calls+1&&result.result.httpStatus==429);
        CHECK(juce::JSON::toString(envelope["context"],true,17)==juce::JSON::toString(actual["context"],true,17));
        CHECK(juce::JSON::toString(envelope["linked_context"],true,17)==juce::JSON::toString(actual["linked_context"],true,17));
        for(const auto& entry:page.entries)
        {
            const auto token=entry.definition.id.toString().toStdString()+"/"+std::to_string(entry.definition.revision)+"/"+entry.contentSha256.toStdString();
            CHECK(visited.insert(token).second);RackDesign resolved;
            CHECK(resolveCatalogDesign(snapshot,entry.definition.id,entry.definition.revision,entry.contentSha256,resolved));CHECK(resolved==entry.definition);
        }
        cursor=page.next;++pages;CHECK(page.complete==(cursor==snapshot.entries.size()));
    }
    CHECK(cursor==snapshot.entries.size()&&visited.size()==71&&pages>1);
    std::cout<<"TASK11_PROVIDER_PAGES count="<<pages<<" page_budget="<<pageBudget<<" reachable="<<visited.size()<<std::endl;
    CHECK(sameRequest(r,original)&&*linked==originalLinked);
}


// The editor's AI unit at 1080 x 680 and 1600 x 1000: on the stage right of the rack area and the narrowed strip, at
// the stage's scale, its controls reachable. AI again hides it, and the rack panel spans the rack area's full width.
void editor_layout()
{
    begin(__func__);Fixture f(true);auto& editor=*f.editor;auto* aiToggle=widget<juce::Button>(editor,"ai.toggle");if(aiToggle)CHECK(reachable(editor,*aiToggle));f.image("collapsed-1080");if(!f.open())return;
    const char* ids[]{"ai.account","ai.model","ai.screen","ai.generate","ai.refine","ai.preview","ai.stop","ai.apply","ai.discard","ai.linked"};
    for(const auto* id:ids)CHECK(find(editor,id));
    auto* pattern=widget<juce::Component>(editor,"pattern.panel");auto* keyboard=widget<juce::Component>(editor,"pattern.keyboard");auto* pages=widget<studio::ProgrammerUnit>(editor,"pages");
    auto* unit=dynamic_cast<studio::AiUnit*>(find(editor,"ai.panel"));auto* stage=find(editor,"stage");CHECK(unit&&stage&&unit->getParentComponent()==stage);
    for(const auto size:{juce::Point<int>{1080,680},juce::Point<int>{1600,1000}})
    {
        editor.setSize(size.x,size.y);pump();if(pattern)CHECK(!pattern->isShowing());
        // The folded unit keeps its place: the keyboard's bounds are its drawn rect on the stage, below the unit's top.
        auto* membrane=dynamic_cast<studio::Keyboard*>(keyboard);auto* skin=studio::studioSkin();CHECK(membrane&&stage&&skin);
        if(membrane&&stage&&skin)
        {
            const auto want=editor.getLocalArea(stage,membrane->element.rect.translated(0.0f,(float)(int)skin->layout["pattern"]["y"]));
            const auto got=editor.getLocalArea(keyboard,keyboard->getLocalBounds().toFloat());
            CHECK(std::abs(got.getX()-want.getX())<=1&&std::abs(got.getY()-want.getY())<=1&&std::abs(got.getRight()-want.getRight())<=1&&std::abs(got.getBottom()-want.getBottom())<=1);
        }
        auto* strip=find(editor,"strip");auto* house=find(editor,"house.panel");CHECK(strip&&house&&reachable(editor,*strip));
        if(strip&&house&&unit&&stage)
        {
            const auto scale=editor.getLocalArea(stage,stage->getLocalBounds().toFloat()).getWidth()/(float)stage->getWidth();
            const auto shown=editor.getLocalArea(strip,strip->getLocalBounds().toFloat()),rack=editor.getLocalArea(house,house->getLocalBounds().toFloat()),side=editor.getLocalArea(unit,unit->getLocalBounds().toFloat());
            CHECK(std::abs(side.getWidth()-401.0f*scale)<=1.0f&&std::abs(shown.getWidth()-1143.0f*scale)<=1.0f);
            CHECK(side.getX()>=shown.getRight()&&side.getX()>=rack.getRight()&&side.getBottom()<=(float)editor.getHeight()+1.0f);
        }
        for(const auto* id:ids)if(auto* c=find(editor,id))CHECK(reachable(editor,*c));
        // ADVANCED: RACK LAYOUT's row selects its bank, whose controls show, and SYNTH 1's knobs show.
        if(pages){CHECK(click(editor,"rack.toggle"));CHECK(pages->isShowing()&&unit&&!unit->isShowing());pages->showPage("layout");pump();
            CHECK(click(editor,"effect.1.row"));for(const auto* id:{"effect.1.row","effect.1.move","effect.1.kind"}){auto* c=find(editor,id);CHECK(c&&reachable(editor,*c));}
            pages->showPage("synth1");pump();CHECK(!pages->shownKnobs().empty());for(auto* k:pages->shownKnobs())CHECK(reachable(editor,*k));pages->showPage("global");pump();
            CHECK(click(editor,"rack.toggle"));CHECK(click(editor,"ai.toggle"));CHECK(unit&&unit->isShowing());}
        f.image(size.x==1080?"expanded-1080":"expanded-1600");
    }
    editor.setSize(1080,680);racked(*f.processor);pump();CHECK(f.generate("parent",3,true,true));f.image("proposed-1080");CHECK(f.screen().page=="proposal");
    // A pending request shows CANCEL on the screen, and closing the unit leaves it running.
    CHECK(enterText(f.screen(),"ai.prompt","hold"));f.service.backend->release.store(false);CHECK(click(editor,"ai.refine"));CHECK(wait([&]{return f.processor->aiProposalPhase()==AiProposalPhase::pending;}));pump();f.image("pending-1080");
    CHECK(f.screen().shown.action=="CANCEL"&&f.screen().hitFor("ai.cancel")!=nullptr);f.process();
    const auto calls=f.service.backend->generationCalls.load();const auto accountCalls=f.service.backend->accountCalls[0].load();CHECK(click(editor,"ai.toggle"));if(unit)CHECK(!unit->isVisible());f.service.backend->release.store(true);CHECK(f.ready());CHECK(f.service.backend->generationCalls==calls&&f.service.backend->accountCalls[0]==accountCalls);CHECK(click(editor,"ai.toggle"));
    // An invalid tempo's error is in ai.status, and GENERATE refuses.
    CHECK(click(editor,"ai.discard"));pump(80);CHECK(enterText(f.screen(),"ai.tempo","nan"));f.image("error-1080");CHECK(status(editor).containsIgnoreCase("tempo"));
    const auto refused=f.service.backend->generationCalls.load();CHECK(click(editor,"ai.generate"));CHECK(f.service.backend->generationCalls==refused&&f.screen().shown.status.containsIgnoreCase("tempo"));f.process();
    CHECK(click(editor,"ai.toggle"));auto* rack=find(editor,"rack.stack");CHECK(rack&&std::abs(editor.getLocalArea(rack,rack->getLocalBounds()).getWidth()-editor.getWidth())<=1);if(unit)CHECK(!unit->isVisible());
}
void required_pattern_options()
{
    std::cout<<"CASE required_pattern_options (generation_diff)"<<std::endl;
    Fixture f;if(!f.open())return;auto& p=*f.processor;auto& u=*f.unit;auto& s=f.screen();const auto committed=project(p);
    // EDITABLE NOTES: dim and off for SOUND, on (and offered) as PATTERN or BOTH is chosen.
    CHECK(option(s,"ai.scope")==0&&option(s,"ai.requirePattern")==0&&s.shown.dim.contains("ai.requirePattern")&&!offered(s,"ai.requirePattern"));
    CHECK(stepTo(s,"ai.scope",1));CHECK(option(s,"ai.requirePattern")==1&&offered(s,"ai.requirePattern"));
    CHECK(stepTo(s,"ai.requirePattern",0));CHECK(option(s,"ai.requirePattern")==0);pump(80);CHECK(option(s,"ai.requirePattern")==0);
    CHECK(stepTo(s,"ai.scope",2));CHECK(option(s,"ai.requirePattern")==1);
    CHECK(f.generate("empty",3,false,false,false));if(!p.aiProposal())return;
    CHECK(p.aiCurrentRequest()&&!p.aiCurrentRequest()->policy.requirePattern);CHECK(p.aiProposal()->candidate->pattern.noteCount==0);CHECK(project(p)==committed);
    p.discardAiProposal();CHECK(wait([&]{return s.page=="create";}));CHECK(stepTo(s,"ai.scope",0));CHECK(option(s,"ai.requirePattern")==0&&!offered(s,"ai.requirePattern"));
    CHECK(stepTo(s,"ai.scope",2));CHECK(option(s,"ai.requirePattern")==1);
    CHECK(stepTo(s,"ai.allowMacros",1));CHECK(stepTo(s,"ai.allowReuse",1));CHECK(enterText(s,"ai.prompt","hold"));
    const auto calls=f.service.backend->generationCalls.load();CHECK(click(u,"ai.generate"));
    CHECK(wait([&]{return f.service.backend->generationCalls.load()==calls+1;}));
    CHECK(p.aiProposalPhase()==AiProposalPhase::pending&&p.aiCurrentRequest());if(!p.aiCurrentRequest())return;
    const auto captured=*p.aiCurrentRequest();CHECK(captured.policy.requirePattern&&captured.policy.scope==RequestScope::both);
    CHECK(captured.policy.allowMacroChanges&&captured.policy.allowSlotReuse);
    // GENERATING has no option rows; the request keeps what it captured.
    pump();CHECK(s.page=="proposal"&&!offered(s,"ai.scope"));
    CHECK(sameRequest(*p.aiCurrentRequest(),captured));CHECK(sameRequest(f.service.backend->request(std::size_t(calls)),captured));CHECK(project(p)==committed);
    f.service.backend->release.store(true);CHECK(f.ready());if(!p.aiProposal())return;
    const auto parent=*p.aiProposal()->candidate;CHECK(parent.pattern.noteCount>0&&p.aiIntendedEdits()->pattern==parent.pattern);
    CHECK(diff(u).contains("notes required"));CHECK(f.refine("empty-child"));if(!p.aiProposal())return;
    const auto child=*p.aiCurrentRequest();CHECK(!child.policy.requirePattern&&child.policy.scope==captured.policy.scope);
    CHECK(child.policy.allowMacroChanges==captured.policy.allowMacroChanges&&child.policy.allowSlotReuse==captured.policy.allowSlotReuse);
    if(child.catalog){CHECK(captured.catalog&&child.catalog->root==captured.catalog->root&&child.source==committed&&child.capturedPhrase==parent.pattern&&p.aiOriginalSource()&&*p.aiOriginalSource()==committed);CHECK(child.catalog->baseIntent.pattern==parent.pattern);}else CHECK(child.source==parent&&child.capturedPhrase==parent.pattern&&p.aiOriginalSource()&&*p.aiOriginalSource()==committed);
    if(child.catalog)CHECK(p.aiProposal()->edits.pattern==parent.pattern&&p.aiProposal()->candidate->pattern==parent.pattern&&p.aiIntendedEdits()->pattern==parent.pattern);else CHECK(!p.aiProposal()->edits.pattern&&p.aiProposal()->candidate->pattern==parent.pattern&&p.aiIntendedEdits()->pattern==parent.pattern);
    const auto candidate=*p.aiProposal()->candidate;CHECK(click(u,"ai.apply"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::idle;}));
    CHECK(project(p)==candidate);CHECK(p.undoSoundEdit());CHECK(project(p)==committed);CHECK(!p.undoSoundEdit());
}
// The AI's marks in the editor (the moves spec: The AI's marks): a CREATE SOUND proposal marks Ember Reed's changed
// control on its faceplate, AIR, and not its locked one, BITE, with the proposal in the control's description; DISCARD
// clears them; a turn of the control, an edit, makes the proposal out of date and clears them.
void editor_proposal_marks()
{
    begin(__func__);Fixture f(true);if(!f.open())return;auto& p=*f.processor;auto& e=*f.editor;
    const RackPackageRef exact{"ember-reed","1.0.0","14b4f5f907054a35a9d6f95513cc1f5ae57b6ad4e4a383b2975bd17700d8f328"};
    const auto fixtureRuntime=airack::testing::isolatedRackRuntime();
    const auto archive=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/releases/ember-reed-1.0.0.airrack");
    const auto owner=std::async(std::launch::async,[&]{
        const auto status=fixtureRuntime->store->install(archive,exact,RackPackageActivation::makeActive,{});
        std::cout<<"MARKS_FIXTURE install_status="<<int(status)<<std::endl;if(status!=RackLibraryStatus::ready)return studio::MakerSkinPtr{};
        RackInventoryPtr inventory;if(!captureRackInventory(*fixtureRuntime->store,{},inventory))return studio::MakerSkinPtr{};
        RackInstance rack;rack.design=airack::test::frozenExampleRackDesigns()[0];return studio::prepareMakerSkin(rack,inventory.get());
    }).get();
    CHECK(owner&&owner->lease&&owner->lease->manifest().ref==exact);if(!owner)return;
    RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[0];op.newInstance=juce::Uuid();op.destination.role=RackRole::synth;op.destination.position=1;
    SoundDocument d;std::uint64_t edit=0;CHECK(p.trySoundSnapshot(d,edit)&&p.applyRackOperation(op,edit)==RackCandidateStatus::ready);
    CHECK(p.trySoundSnapshot(d,edit));const auto air=rackControl(d,0,0),bite=rackControl(d,0,1);CHECK(air&&bite);if(!air||!bite)return;
    d.controlLocks[*bite]=true;CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,edit));
    RackStackUnit* ember=nullptr;
    const bool faceReady=wait([&]{ember=dynamic_cast<RackStackUnit*>(find(e,"rack.unit.0"));return ember!=nullptr&&ember->isShowing()&&ember->face()!=nullptr;});
    std::cout<<"MARKS_FACE component="<<(ember!=nullptr)<<" showing="<<(ember&&ember->isShowing())<<" face="<<(ember&&ember->face()!=nullptr)<<std::endl;CHECK(faceReady);
    if(ember==nullptr||ember->face()==nullptr)return;
    auto& face=*ember->face();
    const auto described=[&](const char* id){auto* c=find(*ember,id);return c!=nullptr?c->getDescription():juce::String("(none)");};
    CHECK(f.generate("marks"));if(!p.aiProposal())return;
    CHECK(wait([&]{return face.marks().size()==1;}));
    if(face.marks().size()==1)CHECK(face.marks()[0].element=="face.air");
    CHECK(described("face.air").startsWith("AI proposes ")&&!described("face.bite").contains("AI proposes"));
    f.image("editor-proposal-marks");
    CHECK(click(e,"ai.discard"));CHECK(wait([&]{return face.marks().empty();}));CHECK(!described("face.air").contains("AI proposes"));
    CHECK(f.generate("marks"));if(!p.aiProposal())return;CHECK(wait([&]{return face.marks().size()==1;}));
    auto* knob=dynamic_cast<juce::Slider*>(find(*ember,"face.air"));CHECK(knob!=nullptr);if(knob==nullptr)return;
    knob->onDragStart();knob->setValue(knob->proportionOfLengthToValue(knob->valueToProportionOfLength(knob->getValue())<.5?.8:.2),juce::sendNotificationSync);knob->onDragEnd();
    CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::stale&&face.marks().empty();}));CHECK(!described("face.air").contains("AI proposes"));
}
// GENERATE and REFINE blink while their request runs, AUDITION is lit while the proposal plays, and APPLY is lit while
// a proposal with audio is ready and blinks while its Apply waits; STOP and DISCARD stay dark. A blink is 250 ms on,
// 250 ms off, by the unit's clock. With --side-snapshots, the unit idle, generating lit and dark, and ready is saved
// for the user's review.
juce::File sideSnapshots;
void side_leds()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& u=*f.unit;auto& p=*f.processor;auto& s=f.screen();auto& b=*f.service.backend;
    const auto save=[&](const char* name)
    {
        if(sideSnapshots==juce::File{})return;
        const auto file=sideSnapshots.getChildFile(juce::String(name)+".png");file.deleteFile();auto stream=file.createOutputStream();
        const auto picture=u.createComponentSnapshot(u.getLocalBounds(),true);CHECK(stream&&picture.isValid());if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(picture,*stream));
    };
    std::vector<std::pair<const char*,studio::SkinButton*>> leds;
    for(const char* id:{"ai.apply","ai.discard","ai.generate","ai.preview","ai.refine","ai.stop"}){leds.push_back({id,dynamic_cast<studio::SkinButton*>(find(u,id))});CHECK(leds.back().second!=nullptr);if(!leds.back().second)return;}
    const auto litAt=[&](std::int64_t now){f.files.now=now;pump(60);juce::StringArray lit;for(const auto& [id,button]:leds)if(button->lit())lit.add(id);CHECK(!lit.contains("ai.stop")&&!lit.contains("ai.discard"));return lit;};
    using Lit=juce::StringArray;
    CHECK(litAt(0).isEmpty()&&litAt(250).isEmpty());save("ai-idle");
    CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.generate"));CHECK(wait([&]{return b.generationCalls.load()==1;}));
    CHECK(litAt(0)==Lit{"ai.generate"});save("ai-generating-lit");CHECK(litAt(250).isEmpty());save("ai-generating-dark");
    b.release.store(true);CHECK(f.ready());
    CHECK(litAt(0)==Lit{"ai.apply"});save("ai-proposal-ready");CHECK(litAt(250)==Lit{"ai.apply"});
    b.release.store(false);CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.refine"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::pending;}));
    CHECK(litAt(0)==Lit{"ai.refine"});CHECK(litAt(250).isEmpty());
    b.release.store(true);CHECK(f.ready());
    CHECK(click(u,"ai.preview"));CHECK(p.aiPreviewPlaying());
    CHECK(litAt(0)==Lit({"ai.apply","ai.preview"}));CHECK(litAt(250)==Lit({"ai.apply","ai.preview"}));
    CHECK(click(u,"ai.stop"));f.process();CHECK(wait([&]{return p.aiPreviewStopped();}));
    CHECK(litAt(0)==Lit{"ai.apply"});
    CHECK(click(u,"ai.preview"));CHECK(p.aiPreviewPlaying());CHECK(click(u,"ai.apply"));CHECK(p.aiWorkflowStatus().applyPending);
    CHECK(litAt(0).contains("ai.apply"));CHECK(!litAt(250).contains("ai.apply"));
    CHECK(wait([&]{f.process();return !p.aiWorkflowStatus().applyPending;}));
    CHECK(litAt(0).isEmpty());
}
// ACCOUNT's LED: dark signed out and while signing in, green signed in, amber when the sign-in has expired or the
// companion fails.
void side_account_states()
{
    begin(__func__);Fixture f;auto& u=*f.unit;auto& p=*f.processor;auto& b=*f.service.backend;auto* account=dynamic_cast<studio::Target*>(find(u,"ai.account"));CHECK(account!=nullptr);if(!account)return;
    CHECK(account->litLayer().isEmpty());if(!f.open())return;CHECK(account->litLayer()=="green"&&account->state()=="connected");
    const auto reply=[&](rack::oauth::Status state){b.state(state);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();};
    reply(rack::oauth::Status::accessExpired);CHECK(account->litLayer()=="amber"&&account->state()=="sign-in expired");
    reply(rack::oauth::Status::signedOut);CHECK(account->litLayer().isEmpty()&&account->state()=="signed out");
    b.holdAccount.store(true);CHECK(pressItem(f.screen(),"ai.account"));CHECK(wait([&]{return b.accountCalls[3].load()==1;}));pump();CHECK(account->litLayer().isEmpty()&&account->state()=="signing in");
    b.holdAccount.store(false);CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->providerStatus==rack::oauth::Status::connected&&!p.aiWorkflowStatus().accountPending;}));pump();CHECK(account->litLayer()=="green");
    f.service.halt();CHECK(wait([&]{return p.aiWorkflowStatus().connection.phase==ClientPhase::failed;}));pump();CHECK(account->litLayer()=="amber"&&account->state()=="companion offline");
}
// The connected page lists the models with the chosen one marked; a row chooses its model, which the model display
// then shows, and the page stays.
void side_model_rows()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& u=*f.unit;auto& p=*f.processor;auto& s=f.screen();auto* model=dynamic_cast<studio::Readout*>(find(u,"ai.model"));CHECK(model!=nullptr);if(!model)return;
    CHECK(model->text.text=="SYNTHETIC A");
    CHECK(click(u,"ai.model"));CHECK(s.page=="connected");CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(s.shown.list.size()==2&&s.shown.chosen==0&&drawn(s,"row.1")=="SYNTHETIC B (SYNTHETIC-B)");
    CHECK(pressItem(s,"ai.model",1));CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->selectedModel=="synthetic-b"&&!p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(s.page=="connected"&&s.shown.chosen==1&&model->text.text=="SYNTHETIC B");
    CHECK(click(u,"ai.model"));CHECK(s.page=="create");
}
// GENERATE shows GENERATING with CANCEL, which returns to the mode's page. The finished proposal lists every change,
// the locked one KEPT: LOCKED, and the wheel brings the fourth onto the glass; AUDITION, STOP and APPLY act, and with
// no model chosen GENERATE says why.
void side_generation()
{
    begin(__func__);Fixture f;auto& p=*f.processor;auto locked=project(p);locked.sound.controlLocks[index("global.glide_seconds")]=true;recall(p,locked);
    if(!f.open())return;auto& u=*f.unit;auto& s=f.screen();auto& b=*f.service.backend;
    CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.generate"));CHECK(wait([&]{return b.generationCalls.load()==1;}));pump();
    CHECK(s.page=="proposal"&&drawn(s,"title")=="GENERATING"&&drawn(s,"cancel")=="CANCEL");
    CHECK(pressItem(s,"ai.cancel"));CHECK(p.aiProposalPhase()==AiProposalPhase::idle&&s.page=="create");b.release.store(true);
    CHECK(f.generate("four"));
    CHECK(s.page=="proposal"&&drawn(s,"title")=="PROPOSAL READY"&&s.shown.changes.size()==4);CHECK(drawn(s,"change.2.kept")=="KEPT: LOCKED");
    if(s.shown.changes.size()==4){CHECK(s.shown.changes[2].locked&&!s.shown.changes[3].locked);const auto fourth=s.shown.changes[3].name;CHECK(drawn(s,"change.2.name")!=fourth);screendriver::wheel(s,-1.0f);CHECK(drawn(s,"change.2.name")==fourth);}
    CHECK(click(u,"ai.preview"));CHECK(p.aiPreviewPlaying());pump();CHECK(drawn(s,"title")=="PROPOSAL: AUDITIONING");
    CHECK(click(u,"ai.stop"));f.process();CHECK(wait([&]{return p.aiPreviewStopped();}));pump();CHECK(drawn(s,"title")=="PROPOSAL READY");
    CHECK(click(u,"ai.apply"));CHECK(wait([&]{f.process();return p.aiProposalPhase()==AiProposalPhase::idle;}));pump();CHECK(s.page=="create");
    CHECK(click(u,"ai.discard"));CHECK(drawn(s,"status")=="NOTHING TO DISCARD");
    b.state(rack::oauth::Status::connected,RuntimeStatus::ready,"");CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();
    const auto calls=b.generationCalls.load();CHECK(click(u,"ai.generate"));CHECK(b.generationCalls.load()==calls&&drawn(s,"status")=="CHOOSE A MODEL TO GENERATE");
}
// The context fields take what is written and draw it in capitals, on the create page and on a proposal.
void side_context_fields()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& u=*f.unit;auto& p=*f.processor;auto& s=f.screen();
    CHECK(enterText(s,"ai.role","lead"));CHECK(enterText(s,"ai.key","D"));CHECK(enterText(s,"ai.scale","minor"));
    CHECK(p.aiMusicalContext().role=="lead"&&p.aiMusicalContext().key=="D"&&p.aiMusicalContext().scale=="minor");
    CHECK(drawn(s,"context.ROLE")=="ROLE LEAD"&&drawn(s,"context.KEY")=="KEY D"&&drawn(s,"context.SCALE")=="SCALE MINOR"&&drawn(s,"context.BPM")=="HOST BPM");
    CHECK(enterText(s,"ai.tempo","96"));CHECK(p.aiMusicalContext().auditionBpm==96.0);CHECK(drawn(s,"context.BPM").startsWith("96")&&drawn(s,"context.BPM").endsWith(" BPM"));
    CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.generate"));CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::pending;}));pump();
    CHECK(s.page=="proposal"&&drawn(s,"context.ROLE")=="ROLE LEAD"&&drawn(s,"context.SCALE")=="SCALE MINOR");
    CHECK(pressItem(s,"ai.cancel"));f.service.backend->release.store(true);
    pump();CHECK(s.page=="create"&&drawn(s,"context.ROLE")=="ROLE LEAD");
    // The entry opens with the text as written.
    CHECK(pressItem(s,"ai.scale"));CHECK(s.entry.isVisible()&&s.entry.getText()=="minor");
}
// A press that cannot act says why on the status line and does nothing else; the next press clears it.
void side_refusals()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& u=*f.unit;auto& p=*f.processor;auto& s=f.screen();auto& b=*f.service.backend;
    const auto calls=[&]{return std::array<int,5>{b.generationCalls.load(),b.accountCalls[0].load(),b.accountCalls[1].load(),b.accountCalls[2].load(),b.accountCalls[3].load()};};
    const auto refused=[&](const char* id,const juce::String& reason)
    {
        const auto before=calls();const auto phase=p.aiProposalPhase();const auto applying=p.aiWorkflowStatus().applyPending;const auto playing=p.aiPreviewPlaying();
        CHECK(click(u,id));const bool said=s.shown.status==reason&&status(u).contains(reason);CHECK(said);
        if(!said)std::cout<<"REFUSAL "<<id<<" shown="<<s.shown.status<<" status="<<status(u)<<std::endl;
        CHECK(calls()==before&&p.aiProposalPhase()==phase&&p.aiWorkflowStatus().applyPending==applying&&p.aiPreviewPlaying()==playing);
    };
    refused("ai.stop","Nothing is auditioning");refused("ai.discard","Nothing to discard");refused("ai.preview","Nothing to audition yet");refused("ai.apply","Nothing to apply");
    refused("ai.generate","Describe a sound or a change to generate");refused("ai.refine","Describe a change to refine");
    CHECK(enterText(s,"ai.prompt","sound"));refused("ai.refine","Refine needs a proposal: Generate first");
    CHECK(enterText(s,"ai.prompt","sound"));CHECK(s.shown.status!="Refine needs a proposal: Generate first"&&!status(u).contains("Refine needs a proposal"));
    CHECK(enterText(s,"ai.tempo","fast"));for(const char* id:{"ai.generate","ai.refine","ai.preview","ai.apply"})refused(id,"Invalid tempo: enter 20 to 400 BPM, or leave blank");CHECK(enterText(s,"ai.tempo",""));
    const auto account=[&](rack::oauth::Status state,const juce::String& model){b.state(state,RuntimeStatus::ready,model);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();};
    account(rack::oauth::Status::signedOut,"synthetic-a");refused("ai.generate","Sign in and connect to generate");refused("ai.refine","Sign in and connect to refine");
    account(rack::oauth::Status::connected,"");refused("ai.generate","Choose a model to generate");refused("ai.refine","Choose a model to refine");
    account(rack::oauth::Status::connected,"synthetic-a");
    // While a request is pending: GENERATE, and a model row on the account page (whose line says so).
    CHECK(enterText(s,"ai.prompt","hold"));CHECK(click(u,"ai.generate"));CHECK(wait([&]{return b.generationCalls.load()==1;}));
    refused("ai.generate","Already generating; Cancel stops it");
    CHECK(click(u,"ai.account"));CHECK(s.page=="connected");const auto before=calls();CHECK(pressItem(s,"ai.model",1));
    CHECK(s.shown.status=="The model stays while a request is pending; Cancel stops it"&&drawn(s,"body.0")==s.shown.status&&calls()==before);
    CHECK(click(u,"ai.account"));CHECK(s.page=="proposal");CHECK(pressItem(s,"ai.cancel"));b.release.store(true);
    CHECK(f.generate());CHECK(click(u,"ai.preview"));refused("ai.preview","Already auditioning; Stop returns to the live sound");
    CHECK(click(u,"ai.apply"));CHECK(p.aiWorkflowStatus().applyPending);refused("ai.refine","Apply is waiting; Discard cancels it");
    CHECK(wait([&]{f.process();return !p.aiWorkflowStatus().applyPending;}));
}
// ACCOUNT opens the account page and asks the account's status unless signed out; pressed again it returns. SIGN IN
// signs in, CANCEL while it waits is today's Cancel, and RETRY on the offline page asks again.
void side_account_page()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& u=*f.unit;auto& p=*f.processor;auto& s=f.screen();auto& b=*f.service.backend;
    const auto asked=b.accountCalls[0].load();CHECK(click(u,"ai.account"));CHECK(s.page=="connected"&&drawn(s,"heading")=="CONNECTED");
    CHECK(wait([&]{return b.accountCalls[0].load()==asked+1&&!p.aiWorkflowStatus().accountPending;}));
    CHECK(click(u,"ai.account"));CHECK(s.page=="create"&&b.accountCalls[0].load()==asked+1);
    b.state(rack::oauth::Status::signedOut);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(s.page=="signed_out"&&drawn(s,"heading")=="SIGNED OUT"&&drawn(s,"button")=="SIGN IN");
    CHECK(drawn(s,"body.0")=="Sound generation is experimental.");
    CHECK(drawn(s,"body.1")=="Account access and limits may vary.");
    CHECK(drawn(s,"body.2")=="Manual controls stay available.");
    const auto statuses=b.accountCalls[0].load();CHECK(click(u,"ai.account"));pump();CHECK(s.page=="signed_out"&&b.accountCalls[0].load()==statuses&&!p.aiWorkflowStatus().accountPending);
    b.holdAccount.store(true);CHECK(pressItem(s,"ai.account"));CHECK(wait([&]{return b.accountCalls[3].load()==1;}));pump();
    CHECK(p.aiWorkflowStatus().accountPending&&drawn(s,"heading")=="SIGNING IN"&&drawn(s,"button")=="CANCEL");
    CHECK(pressItem(s,"ai.account"));CHECK(!p.aiWorkflowStatus().accountPending);b.holdAccount.store(false);pump();CHECK(drawn(s,"heading")=="SIGNED OUT"&&drawn(s,"button")=="SIGN IN");
    CHECK(pressItem(s,"ai.account"));CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->providerStatus==rack::oauth::Status::connected&&!p.aiWorkflowStatus().accountPending;}));pump();CHECK(s.page=="connected");
    f.service.halt();CHECK(wait([&]{return p.aiWorkflowStatus().connection.phase==ClientPhase::failed;}));pump();
    CHECK(s.page=="alert"&&drawn(s,"heading")=="AI OFFLINE"&&drawn(s,"button")=="RETRY");
    f.service.start();CHECK(pressItem(s,"ai.account"));CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->models.size()==2&&!p.aiWorkflowStatus().accountPending;}));pump();CHECK(s.page=="connected");
}
// The change rows read today's difference: each parameter by its knob's name, with its values as the difference
// writes them, and the locked one kept.
void side_change_rows()
{
    begin(__func__);Fixture f;auto& p=*f.processor;auto locked=project(p);locked.sound.controlLocks[index("global.glide_seconds")]=true;recall(p,locked);
    if(!f.open())return;auto& s=f.screen();CHECK(f.generate("four"));
    const auto& registry=parameterRegistry();const auto& model=*studio::studioSkin();const auto d=diff(*f.unit);
    const std::array<std::size_t,4> rows{index("output.gain_db"),index("global.master_level"),index("global.glide_seconds"),39};
    CHECK(s.shown.changes.size()==4);
    for(std::size_t i=0;i<s.shown.changes.size()&&i<rows.size();++i)
    {
        const auto n=rows[i];const auto& row=s.shown.changes[i];
        CHECK(row.name==(model.knobNames[n].isNotEmpty()?model.knobNames[n]:registry[n].name.toUpperCase()));CHECK(row.locked==(i==2));
        const auto line=d.fromFirstOccurrenceOf("["+registry[n].id+"]: ",false,false).upToFirstOccurrenceOf("\n",false,false);
        CHECK(line.startsWith(row.before+" -> "+row.after));
    }
    if(s.shown.changes.size()==4)CHECK(s.shown.changes[2].before==s.shown.changes[2].after);
    f.image("unit-proposal");
}
// A named proposal's header reads its name in capitals, and the details say it as written; an unnamed one keeps
// CREATE SOUND.
void catalog_proposal_review()
{
    begin(__func__);Fixture f(true,true);auto& p=*f.processor;
    const auto committed=project(p);CHECK(committed.sound.layout.synthCount==0);
    if(!f.open()||!f.generate("catalog-preview"))return;
    const auto* result=p.aiProposal();CHECK(result&&result->candidate);
    if(!result||!result->candidate)return;
    CHECK(result->candidate->sound.layout.synthCount==1);
    CHECK(project(p)==committed);
    auto& s=f.screen();CHECK(s.page=="proposal");
    CHECK(std::any_of(s.shown.changes.begin(),s.shown.changes.end(),[](const auto& row){return row.name.containsIgnoreCase("Copper Arc");}));
    CHECK(std::any_of(s.shown.changes.begin(),s.shown.changes.end(),[](const auto& row){return row.name.containsIgnoreCase("Brightness")||row.name.containsIgnoreCase("Cutoff");}));
    CHECK(drawn(s,"change.0.name")=="Copper Arc"&&drawn(s,"change.0.before")=="Add");
    CHECK(drawn(s,"change.1.name")=="Brightness"&&drawn(s,"change.1.before")=="-");
    CHECK(drawn(s,"change.1.after").contains("Hz"));
    const auto& waveLabel=studio::studioSkin()->knobNames[index("synth.1.analog.osc.1.waveform")];
    CHECK(waveLabel.isNotEmpty()&&drawn(s,"change.2.name")==waveLabel);
    CHECK(drawn(s,"change.2.after")=="Triangle");
    f.image("editor-catalog-proposal");
    CHECK(click(*f.editor,"ai.discard"));pump();CHECK(project(p)==committed);CHECK(s.shown.changes.empty());
}
void catalog_rack_ghosts()
{
    begin(__func__);Fixture f(true,true);auto& p=*f.processor;auto& editor=*f.editor;
    const auto committed=project(p);
    if(!f.open()||!f.generate("catalog-preview-pair"))return;
    const auto* result=p.aiProposal();CHECK(result&&result->candidate&&result->candidate->sound.personalities);
    if(!result||!result->candidate||!result->candidate->sound.personalities)return;
    const auto candidate=*result->candidate;
    CHECK(candidate.sound.personalities->instances.size()==2&&project(p)==committed);
    std::vector<juce::String> ids;
    for(const auto& rack:candidate.sound.personalities->instances)ids.push_back("rack.proposal."+rack.id.toString());
    CHECK(wait([&]{return std::all_of(ids.begin(),ids.end(),[&](const auto& id){auto* c=find(editor,id);return c&&c->isShowing()&&c->getHeight()>0;});}));
    for(std::size_t i=0;i<ids.size();++i){
        auto* ghost=find(editor,ids[i]);CHECK(ghost!=nullptr);if(!ghost)return;
        CHECK(!ghost->hitTest(ghost->getWidth()/2,ghost->getHeight()/2)&&!ghost->getWantsKeyboardFocus());
        CHECK(ghost->getTitle().startsWith("PREVIEW:")&&ghost->getDescription()=="Copper Arc");
        const auto face=std::find_if(ghost->getChildren().begin(),ghost->getChildren().end(),[](const auto* c){return std::abs(c->getAlpha()-.5f)<.01f;});
        CHECK(face!=ghost->getChildren().end());
        auto* brightness=dynamic_cast<juce::Slider*>(find(*ghost,"rack.focus.0"));CHECK(brightness&&!brightness->getWantsKeyboardFocus()&&!brightness->onValueChange);
        const auto& rack=candidate.sound.personalities->instances[i];const auto* binding=rackUnitBinding(rack,0);CHECK(binding);
        if(brightness&&binding){const auto n=rackParameterIndex(NativeModule::analog,binding->slot,"analog.filter.cutoff");CHECK(n);if(n)CHECK(std::abs(brightness->getValue()-p.control(*n).convertFrom0to1(candidate.sound.values[*n]))<.01);}
    }
    CHECK(find(editor,ids[0])->getBottom()<=find(editor,ids[1])->getY());
    f.image("editor-catalog-rack-ghosts");CHECK(project(p)==committed);
    CHECK(click(editor,"ai.discard"));CHECK(wait([&]{return std::none_of(ids.begin(),ids.end(),[&](const auto& id){return find(editor,id)!=nullptr;});}));CHECK(project(p)==committed);
    if(!f.generate("catalog-preview-pair"))return;
    const auto staleId="rack.proposal."+p.aiProposal()->candidate->sound.personalities->instances[0].id.toString();
    auto changed=committed.sound;changed.values[index("output.gain_db")]=.89f;CHECK(p.applySoundEdit(changed,SoundEditOrigin::manual,revision(p)));
    CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::stale&&find(editor,staleId)==nullptr;}));CHECK(p.undoSoundEdit());
    if(!f.generate("catalog-preview-pair"))return;
    const auto applied=*p.aiProposal()->candidate;const auto appliedId="rack.proposal."+applied.sound.personalities->instances[0].id.toString();
    CHECK(click(editor,"ai.apply"));f.process();CHECK(wait([&]{return p.aiProposalPhase()==AiProposalPhase::idle;}));pump(100);
    CHECK(project(p)==applied&&find(editor,appliedId)==nullptr&&find(editor,"rack.unit.0")!=nullptr);
    CHECK(p.undoSoundEdit());CHECK(project(p)==committed);
    RackOperation add;add.design=factoryRackDesigns()[0];add.newInstance=juce::Uuid();add.destination={RackRole::synth,masterChain,0};
    CHECK(p.applyRackOperation(add,revision(p))==RackCandidateStatus::ready);pump(100);
    const auto beforeExisting=project(p);
    if(!f.generate("catalog-preview-existing"))return;
    auto* existing=dynamic_cast<RackStackUnit*>(find(editor,"rack.unit.0"));CHECK(existing&&existing->id()==add.newInstance);
    CHECK(wait([&]{return existing&&existing->row.marks().size()==1;}));CHECK(project(p)==beforeExisting);
    if(existing){auto* knob=find(*existing,"rack.focus.0");CHECK(knob&&knob->getDescription().startsWith("AI proposes "));}
    const auto& mixed=p.aiProposal()->candidate->sound.personalities->instances;
    const auto added=std::find_if(mixed.begin(),mixed.end(),[&](const auto& rack){return rack.id!=add.newInstance;});CHECK(added!=mixed.end());
    const auto mixedGhost=added!=mixed.end()?"rack.proposal."+added->id.toString():juce::String();
    CHECK(wait([&]{return find(editor,mixedGhost)!=nullptr;}));f.image("editor-existing-rack-and-ghost");
    CHECK(click(editor,"ai.discard"));CHECK(wait([&]{return find(editor,mixedGhost)==nullptr&&existing&&existing->row.marks().empty();}));CHECK(project(p)==beforeExisting);
}
void side_proposal_name()
{
    begin(__func__);Fixture f;if(!f.open())return;auto& s=f.screen();
    // Pattern-only generation retains the genuine legacy unnamed proposal contract.
    CHECK(f.generate("unnamed",2));CHECK(s.page=="proposal"&&drawn(s,"header.title")=="CREATE SOUND");
    CHECK(f.generate("named"));CHECK(s.page=="proposal"&&drawn(s,"header.title")=="ETHEREAL ROLLING BASS");
    CHECK(diff(*f.unit).contains("Name: Ethereal Rolling Bass"));
    f.image("unit-proposal-named");
}
}
// A sign-in whose access token expired while the host was closed renews by itself when the unit first shows: one
// status, one renewal, no sign-in, RENEWING while it runs. A refused refresh token ends at SIGN IN after one more
// status, with no loop.
void renewal_after_restart()
{
    begin(__func__);
    {
        Fixture f;auto& p=*f.processor;auto& b=*f.service.backend;auto& s=f.screen();
        auto* account=dynamic_cast<studio::Target*>(find(*f.unit,"ai.account"));CHECK(account!=nullptr);if(!account)return;
        b.expired.store(true);b.holdRenewal.store(true);f.unit->setVisible(true);
        CHECK(wait([&]{return b.accountCalls[1].load()==1;}));pump();
        CHECK(p.aiWorkflowStatus().accountPending&&p.aiWorkflowStatus().message=="Renewing sign-in");
        CHECK(s.page=="signed_out"&&drawn(s,"heading")=="RENEWING");CHECK(account->litLayer().isEmpty()&&account->state()=="renewing");
        b.holdRenewal.store(false);
        CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->providerStatus==rack::oauth::Status::connected&&a->models.size()==2&&!p.aiWorkflowStatus().accountPending;}));pump(200);
        CHECK(b.accountCalls[0]==1&&b.accountCalls[1]==1&&b.accountCalls[3]==0);
        CHECK(account->litLayer()=="green"&&account->state()=="connected");CHECK(s.page=="create");
    }
    {
        Fixture f;auto& p=*f.processor;auto& b=*f.service.backend;auto& s=f.screen();
        b.expired.store(true);b.refuseRenewal.store(true);f.unit->setVisible(true);
        CHECK(wait([&]{const auto* a=p.aiAccountReply();return b.accountCalls[0].load()==2&&a&&a->providerStatus==rack::oauth::Status::signedOut&&!p.aiWorkflowStatus().accountPending;}));pump(200);
        CHECK(b.accountCalls[0]==2&&b.accountCalls[1]==1&&b.accountCalls[3]==0);CHECK(s.page=="signed_out"&&drawn(s,"button")=="SIGN IN");
    }
}
void account_switch_ui()
{
    begin(__func__);Fixture f;auto& p=*f.processor;auto& b=*f.service.backend;
    {const std::lock_guard lock(b.mutex);b.accounts={{"saved-a","Account 1 - a@example.test"}};b.activeAccount="saved-a";}
    if(!f.open())return;const auto original=project(p);const auto priorModels=b.accountCalls[1].load();
    f.screen().onCommand("ai.account.add",0);
    CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->activeAccount=="saved-b"&&a->models.size()==1&&!p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(b.accountCalls[4]==1&&b.accountCalls[1]>priorModels);CHECK(p.aiAccountReply()->accounts.size()==2);
    CHECK(p.aiAccountReply()->models[0].slug=="second-only");
    CHECK(drawn(f.screen(),"body.0")=="Account 2 - b@example.test");
    CHECK(click(f.holder(),"ai.generate"));pump();CHECK(f.screen().shown.status=="Choose a model to generate");
    CHECK(!p.generateAi({"stale account choice","synthetic-a",{}}));
    f.screen().onCommand("ai.account.choose",1); // Current account list keeps the first saved registration.
    CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->activeAccount=="saved-a"&&a->models.size()==2&&!p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(b.accountCalls[5]==1);CHECK(drawn(f.screen(),"body.0")=="Account 1 - a@example.test");
    CHECK(project(p)==original&&b.generationCalls==0);
}
void account_sign_in_failure()
{
    begin(__func__);Fixture f;auto& p=*f.processor;auto& b=*f.service.backend;
    if(!f.open())return;const auto original=project(p);
    b.state(rack::oauth::Status::signedOut);b.rejectNewSignIn.store(true);
    f.screen().onCommand("ai.account.add",0);
    CHECK(wait([&]{const auto* a=p.aiAccountReply();return a&&a->providerStatus==rack::oauth::Status::tokenFailure&&!p.aiWorkflowStatus().accountPending;}));pump();
    CHECK(p.aiAccountReply()->errorCode=="invalid_grant"&&p.aiAccountReply()->httpStatus==400);
    CHECK(p.aiWorkflowStatus().message=="Air could not complete sign-in. Check your browser, then choose an account.");
    CHECK(drawn(f.screen(),"button")=="SIGN IN");
    CHECK(pressItem(f.screen(),"ai.account"));
    CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));
    CHECK(b.accountCalls[3]==1);CHECK(b.accountCalls[4]==1);
    CHECK(project(p)==original&&b.generationCalls==0);
    for(const auto permission:{rack::oauth::Status::inferenceNotGranted,rack::oauth::Status::denied,rack::oauth::Status::expiredAttempt}){
        b.state(permission);CHECK(p.requestAiAccount({AccountAction::status,{}}));CHECK(wait([&]{return !p.aiWorkflowStatus().accountPending;}));pump();
        const bool unavailable=permission==rack::oauth::Status::inferenceNotGranted;
        CHECK(drawn(f.screen(),"heading")== (unavailable?"GENERATION UNAVAILABLE":permission==rack::oauth::Status::denied?"PERMISSION NOT GRANTED":"SIGN-IN INCOMPLETE"));
        CHECK(drawn(f.screen(),"body.0")== (unavailable?"Sound generation is unavailable for this account. Manual controls stay available.":permission==rack::oauth::Status::denied?"Permission was not granted. Choose an account or continue manually.":"Sign-in did not return to air. Check your browser, or choose an account."));
        CHECK(drawn(f.screen(),"button")=="CHOOSE ACCOUNT");
        const auto signIns=b.accountCalls[3].load();CHECK(pressItem(f.screen(),"ai.account"));pump();
        CHECK(b.accountCalls[3]==signIns);juce::PopupMenu::dismissAllActiveMenus();pump();
        CHECK(project(p)==original&&b.generationCalls==0);
    }
}
int main(int argc,char** argv)
{
    if(argc==2&&juce::String(argv[1])=="--catalog-envelope"){
        try{native_catalog_provider_envelope_preflight();}catch(const std::exception& e){CHECK(false);std::cout<<"FIXTURE_EXCEPTION "<<e.what()<<std::endl;}
        finish();std::cout<<"Catalog envelope checks="<<checks<<" groups="<<groups<<" failures="<<failures<<std::endl;return failures?1:0;}
    juce::ScopedJuceInitialiser_GUI gui;
    if(argc==2&&juce::String(argv[1])=="--account-errors"){account_sign_in_failure();finish();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--account-switch"){account_switch_ui();finish();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--catalog-ghosts"){catalog_rack_ghosts();finish();std::cout<<"Catalog ghost checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--catalog-review"){catalog_proposal_review();finish();std::cout<<"Catalog review checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--editor-marks"){editor_proposal_marks();finish();std::cout<<"Editor marks checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
    for(int i=1;i+1<argc;++i)if(juce::String(argv[i])=="--side-snapshots"){sideSnapshots=juce::File(argv[i+1]);sideSnapshots.createDirectory();}
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto oldHook=_CrtSetAllocHook(heapHook);
#endif
    std::cout<<"Labelled synthetic native service/AI unit/editor fixtures; PID="<<GetCurrentProcessId()<<std::endl;
    try{native_catalog_provider_envelope_preflight();account_models();generation_diff();required_pattern_options();refinement();preview_apply();client_lifecycle();editor_layout();editor_proposal_marks();
        side_leds();side_account_states();side_model_rows();side_generation();side_context_fields();side_refusals();side_account_page();side_change_rows();side_proposal_name();catalog_proposal_review();catalog_rack_ghosts();renewal_after_restart();postedClickBehindSlowMessage();account_switch_ui();account_sign_in_failure();}
    catch(const std::exception& e){CHECK(false);std::cout<<"FIXTURE_EXCEPTION "<<e.what()<<std::endl;}
    finish();CHECK(groups==23);CHECK(allocations==0&&deallocations==0);std::cout<<"AiPanel checks="<<checks<<" groups="<<groups<<" failures="<<failures<<" guardedAudio="<<guardedCalls<<" allocations="<<allocations<<" frees="<<deallocations<<std::endl;
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(oldHook);
#endif
    return failures?1:0;
}
