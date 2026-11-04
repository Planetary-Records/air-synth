#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "RackExamples.h"
#include "InstrumentProcessor.h"
#include "CompanionWire.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack {
// Explicit historical melodic unit-fixture authority, sealed before normal request construction.
struct InstrumentAiTestAccess {
    static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p,const AiRequestOptions& options,std::optional<double> bpm={})
    try {
        if(options.policy.scope==ai::RequestScope::pattern)return airack::testing::beginAiRequest(p,options,bpm);
        p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
        std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
        if(factory.size()!=26||std::count(factory.begin(),factory.end(),tile)!=1)return {};
        std::erase(factory,tile);if(factory.size()!=25)return {};
        auto captured=std::make_shared<RackCatalogSnapshot>();
        {const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit=0;
            if(!p.tryProjectSnapshotOwned(*source,edit))return {};
            std::vector<RackDesign> embedded;if(source->sound.personalities)for(const auto& rack:source->sound.personalities->instances)embedded.push_back(rack.design);
            if(!captureRackCatalog(factory,embedded,{},*captured))return {};}
        return p.beginAiRequestWithCatalog(options,bpm,std::move(captured));
    }catch(const std::bad_alloc&){return {};}
};
}

namespace
{
using namespace airack;
using namespace airack::ai;
int checks=0, failures=0, groups=0;
const char* group="";
void check(bool pass,const char* expression,int line)
{++checks;if(!pass){++failures;std::cout<<"FAIL "<<group<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){group=name;++groups;std::cout<<"GROUP "<<name<<'\n';}
std::uint32_t bits(float v){return std::bit_cast<std::uint32_t>(v);}
thread_local bool audioGuard=false;
thread_local unsigned allocations=0,deallocations=0;
unsigned guardedCalls=0;
#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl heapHook(int kind,void*,size_t,int,long,const unsigned char*,int)
{
    if(audioGuard){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++deallocations;}
    return 1;
}
#endif
template<class Call> void guarded(Call&& call)
{
    const auto a=allocations,d=deallocations;audioGuard=true;call();audioGuard=false;++guardedCalls;
    CHECK(allocations==a);CHECK(deallocations==d);
}
void mix(RenderedPreview& p,juce::AudioBuffer<float>& out,std::uint64_t rev=7)
{guarded([&]{p.mix(out,rev);});}
void callback(InstrumentProcessor& p,juce::AudioBuffer<float>& out,juce::MidiBuffer& midi)
{guarded([&]{p.processBlock(out,midi);});}
void fill(juce::AudioBuffer<float>& out,float left=.25f,float right=-.5f)
{for(int n=0;n<out.getNumSamples();++n){out.setSample(0,n,left);if(out.getNumChannels()>1)out.setSample(1,n,right);}}
PreviewAudio fixture(std::size_t frames=1024)
{
    PreviewAudio a;a.left.resize(frames);a.right.resize(frames);
    for(std::size_t n=0;n<frames;++n){a.left[n]=float(int(n%23)-11)/32;a.right[n]=float(int(n%19)-9)/64;}
    if(frames>401){a.left[400]=-0.0f;a.right[401]=-0.0f;}return a;
}
using Stereo=std::array<std::vector<float>,2>;
// Independent reference, with literal 48k ramp/end/steady anchors below.
Stereo reference(const PreviewAudio& source,double rate,std::optional<std::size_t> stopAt={})
{
    const auto count=std::size_t(std::ceil(source.left.size()*rate/48000.));
    const auto fade=std::size_t(std::max(1.,std::ceil(rate/200.)));
    Stereo out {std::vector<float>(count),std::vector<float>(count)};
    double last=0,stopLevel=0;
    for(std::size_t n=0;n<count;++n)
    {
        double gain=std::min({1.,double(n)/fade,double(count-1-n)/fade});
        if(stopAt&&n==*stopAt)stopLevel=last;
        if(stopAt&&n>=*stopAt)gain=std::min(gain,std::max(0.,stopLevel-double(n-*stopAt+1)/fade));
        for(int c=0;c<2;++c)
        {
            const auto& input=c==0?source.left:source.right;const float committed=c==0?.25f:-.5f;
            const double pos=n*48000./rate;const auto low=std::min(std::size_t(pos),input.size()-1),high=std::min(low+1,input.size()-1);
            const double part=pos-std::floor(pos);
            const float sample=part==0||low==high?input[low]:float(double(input[low])+(double(input[high])-input[low])*part);
            out[c][n]=gain==0?committed:gain==1?sample:float(committed*(1-gain)+sample*gain);
        }
        last=gain;
    }return out;
}
void same(const Stereo& a,const Stereo& b,float tolerance=0)
{
    CHECK(a[0].size()==b[0].size()&&a[1].size()==b[1].size());bool okay=true;
    for(int c=0;c<2;++c)for(std::size_t n=0;n<std::min(a[c].size(),b[c].size());++n)
        okay &= tolerance==0?bits(a[c][n])==bits(b[c][n]):std::isfinite(a[c][n])&&std::abs(a[c][n]-b[c][n])<=tolerance;
    CHECK(okay);
}
Stereo render(const PreviewAudio& a,double rate,std::span<const int> partitions,std::optional<std::size_t> stopAt={})
{
    RenderedPreview p;p.prepare(rate);const bool started=p.start(a,7);CHECK(started);
    const auto count=std::size_t(std::ceil(a.left.size()*rate/48000.));Stereo out {std::vector<float>(count),std::vector<float>(count)};
    if(!started)return out;
    std::size_t at=0,part=0;
    while(at<count)
    {
        if(stopAt&&at==*stopAt)p.stop();
        auto frames=std::min<std::size_t>(partitions[part++%partitions.size()],count-at);
        if(stopAt&&at<*stopAt)frames=std::min(frames,*stopAt-at);
        juce::AudioBuffer<float> block(2,int(frames));fill(block);mix(p,block);
        for(int c=0;c<2;++c)std::copy_n(block.getReadPointer(c),frames,out[c].begin()+at);at+=frames;
    }
    CHECK(p.stopped()&&!p.playing());return out;
}
void signal_and_partitions()
{
    begin(__func__);RenderedPreview idle;juce::AudioBuffer<float> out(2,1024);fill(out,-0.0f,.125f);
    mix(idle,out);bool untouched=true;for(int n=0;n<1024;++n)untouched &= bits(out.getSample(0,n))==0x80000000u&&bits(out.getSample(1,n))==bits(.125f);CHECK(untouched);
    const auto a=fixture();const std::array<int,1> whole {100000};const std::array<int,4> chunks {1,64,257,512};
    for(double rate:{44100.,48000.,96000.})
    {
        const auto actual=render(a,rate,whole),partitioned=render(a,rate,chunks);same(actual,partitioned);same(actual,reference(a,rate),1e-6f);
        CHECK(bits(actual[0].front())==bits(.25f)&&bits(actual[1].back())==bits(-.5f));
        if(rate==48000)
        {
            CHECK(std::abs(actual[0][1]-(.25f*239.f/240.f+a.left[1]/240.f))<1e-7f);
            CHECK(bits(actual[0][240])==bits(a.left[240])&&bits(actual[0][783])==bits(a.left[783]));
            CHECK(bits(actual[0][400])==0x80000000u&&bits(actual[1][401])==0x80000000u);
            bool steady=true;for(int c=0;c<2;++c)for(int n=240;n<=783;++n)steady &= bits(actual[c][n])==bits(c==0?a.left[n]:a.right[n]);CHECK(steady);
        }
    }
    for(std::size_t count:{1u,3u,240u,479u})for(double rate:{44100.,48000.,96000.})same(render(fixture(count),rate,chunks),reference(fixture(count),rate));
    PreviewAudio one;one.left={.75f};one.right={-.75f};const auto single=render(one,48000,whole);CHECK(single[0]==std::vector<float>{.25f}&&single[1]==std::vector<float>{-.5f});
    PreviewAudio triangle;triangle.left={1,1,1};triangle.right={0,0,0};auto shortOut=render(triangle,48000,whole);
    CHECK(std::abs(shortOut[0][1]-(.25f+ .75f/240.f))<1e-7f);CHECK(bits(shortOut[0][0])==bits(.25f)&&bits(shortOut[0][2])==bits(.25f));
}
void stop_and_lifecycle()
{
    begin(__func__);auto a=fixture();RenderedPreview p;CHECK(!p.start(a,7));p.prepare(48000);
    juce::AudioBuffer<float> block(2,64);
    for(std::size_t at:{0u,100u,512u,900u})
    {
        const std::array<int,4> partitions {1,64,257,512};same(render(a,48000,partitions,at),reference(a,48000,at));
    }
    const bool started=p.start(a,7);CHECK(started);
    if(started)
    {
        CHECK(p.playing()&&!p.stopped());p.stop();CHECK(!p.stopped()&&p.playing());CHECK(!p.start(a,7));fill(block);mix(p,block);
        CHECK(p.stopped()&&!p.playing());bool exact=true;for(int n=0;n<64;++n)exact &= bits(block.getSample(0,n))==bits(.25f)&&bits(block.getSample(1,n))==bits(-.5f);CHECK(exact);
    }
    auto invalid=a;invalid.right.pop_back();CHECK(!p.start(invalid,7));invalid=a;invalid.sampleRate=44100;CHECK(!p.start(invalid,7));
    invalid=a;invalid.left[2]=std::numeric_limits<float>::infinity();CHECK(!p.start(invalid,7));invalid=a;invalid.right[8]=std::numeric_limits<float>::quiet_NaN();CHECK(!p.start(invalid,7));
    invalid={};CHECK(!p.start(invalid,7));invalid.left.resize(576001);invalid.right.resize(576001);CHECK(!p.start(invalid,7));
    auto maximum=fixture(576000);const bool boundary=p.start(maximum,7);CHECK(boundary);if(boundary){p.stop();fill(block);mix(p,block);CHECK(p.stopped());}
    for(int iteration=0;iteration<12;++iteration)
    {
        const bool accepted=p.start(a,7);CHECK(accepted);if(!accepted)break;CHECK(!p.start(a,7));
        auto original=a;p.prepare(0);p.prepare(std::numeric_limits<double>::quiet_NaN());p.prepare(400000);CHECK(!p.stopped());
        a.left.assign(a.left.size(),.9f);a.right.assign(a.right.size(),.9f);
        juce::AudioBuffer<float> first(2,512);fill(first);mix(p,first);const auto wanted=reference(original,48000);
        bool copied=true;for(int c=0;c<2;++c)for(int n=0;n<512;++n)copied &= bits(first.getSample(c,n))==bits(wanted[c][n]);CHECK(copied);
        p.stop();CHECK(!p.stopped());fill(first);mix(p,first,8);CHECK(p.stopped());a=std::move(original);
    }
    CHECK(p.start(a,7));p.release();CHECK(p.stopped()&&!p.playing()&&!p.start(a,7));p.prepare(96000);CHECK(p.start(a,7));
    p.prepare(8000);CHECK(p.stopped());CHECK(p.start(a,7));p.prepare(384000);CHECK(p.stopped());CHECK(p.start(a,7));p.release();CHECK(p.stopped());
    p.prepare(48000);const bool manual=p.start(a,7);CHECK(manual);if(manual){fill(block);mix(p,block,8);CHECK(p.stopped());}
}
void canonical_copy_and_acknowledged_storage()
{
    begin(__func__);
    for(unsigned channels:{1u,2u})for(double rate:{44100.,48000.,96000.}){
        const auto source=fixture(1037);auto planar=source;if(channels==1)planar.right=planar.left;
        std::vector<float> pcm(source.left.size()*channels);
        for(std::size_t n=0;n<source.left.size();++n){pcm[n*channels]=source.left[n];if(channels==2)pcm[n*2+1]=source.right[n];}
        RenderedPreview canonical,ordinary;canonical.prepare(rate);ordinary.prepare(rate);
        CHECK(canonical.startCanonical(pcm,channels,7));CHECK(ordinary.start(planar,7));
        CHECK(canonical.retainedPcmBytes()==source.left.size()*2*sizeof(float));
        pcm.clear();pcm.shrink_to_fit();CHECK(!canonical.clearStopped());
        const std::array<int,4> partitions{1,64,257,512};std::size_t part=0;
        while(!canonical.stopped()){const int size=partitions[part++%partitions.size()];juce::AudioBuffer<float> a(2,size),b(2,size);fill(a);fill(b);mix(canonical,a);mix(ordinary,b);for(int c=0;c<2;++c)for(int n=0;n<size;++n)CHECK(bits(a.getSample(c,n))==bits(b.getSample(c,n)));}
        CHECK(canonical.retainedPcmBytes()==source.left.size()*8);CHECK(canonical.clearStopped());CHECK(canonical.retainedPcmBytes()==0);
        CHECK(canonical.startCanonical(std::array<float,2>{.1f,.2f},1,7));canonical.stop();CHECK(canonical.retainedPcmBytes()==16);CHECK(!canonical.clearStopped());juce::AudioBuffer<float> block(2,64);mix(canonical,block);CHECK(canonical.clearStopped());CHECK(canonical.retainedPcmBytes()==0);
    }
    RenderedPreview p;p.prepare(48000);const std::array<float,3> data{0,.25f,-.25f};
    CHECK(!p.startCanonical({},1,7));CHECK(!p.startCanonical(data,0,7));CHECK(!p.startCanonical(data,2,7));CHECK(!p.startCanonical(data,3,7));
    const std::array<float,1> nan{std::numeric_limits<float>::quiet_NaN()};CHECK(!p.startCanonical(nan,1,7));
    std::vector<float> maximum(480000*2,.125f);CHECK(p.startCanonical(maximum,2,7));CHECK(p.retainedPcmBytes()==3840000);p.release();CHECK(p.retainedPcmBytes()==0);p.prepare(48000);maximum.push_back(0);maximum.push_back(0);CHECK(!p.startCanonical(maximum,2,7));
}
std::size_t index(const char* id)
{
    const auto registry=parameterRegistry();const auto i=std::find_if(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;});
    CHECK(i!=registry.end());return i==registry.end()?0:std::size_t(i-registry.begin());
}
ProjectDocument project(InstrumentProcessor& p)
{ProjectDocument d;std::uint64_t edit=0,reset=0;CHECK(p.tryProjectSnapshot(d,edit,reset));return d;}
std::uint64_t revision(InstrumentProcessor& p)
{ProjectDocument d;std::uint64_t edit=0,reset=0;CHECK(p.tryProjectSnapshot(d,edit,reset));return edit;}
bool recall(InstrumentProcessor& p,const ProjectDocument& d)
{juce::MemoryBlock b;CHECK(encodeProjectState(d,b));return p.restoreProjectState(b.getData(),int(b.getSize()));}
ProjectDocument setup(InstrumentProcessor& p,PlaybackMode mode=PlaybackMode::externalMidi)
{
    ProjectDocument d;d.pattern.meter={1,4};d.pattern.noteCount=1;d.pattern.notes[0]={0,.05,60,70,1};d.mode=mode;
    d.sound.values[index("output.gain_db")]=.4f;CHECK(recall(p,d));return d;
}
AiRequestOptions options(){return {"Rendered audition synthetic native fixture","synthetic-selected_model:1.0",{RequestScope::sound,false,false}};}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& f:fields)o->setProperty(f.first,f.second);return o;}
GenerationResult generate(const GenerationRequest& request)
{
    juce::Array<juce::var> parameters;parameters.add(object({{"id","global.master_level"},{"normalized",.23}}));
    const auto legacyText=juce::JSON::toString(object({{"version",1},{"parameter_edits",parameters},{"rack_edits",juce::Array<juce::var>()},
        {"macro_edits",juce::Array<juce::var>()},{"pattern",juce::var()},{"characteristics","synthetic"},{"explanation","native rendered preview fixture"}}),true);
    auto text=legacyText;
    if(request.catalog){
        CHECK(request.catalog->authority);
        if(request.catalog->authority){
            auto preflight=std::make_unique<ProposalEdits>();ProposalDecodeReason reason;
            CHECK(!decodeProposal(legacyText.toStdString(),request.policy,*request.catalog->authority,*preflight,&reason)&&reason==ProposalDecodeReason::rootFields);
            auto value=juce::JSON::parse(legacyText);
            value.getDynamicObject()->setProperty("version",2);value.getDynamicObject()->setProperty("name","Rendered Test Sound");
            value.getDynamicObject()->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>()},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));
            value.getDynamicObject()->setProperty("catalog_selections",juce::Array<juce::var>());
            value.getDynamicObject()->setProperty("catalog_parameter_edits",juce::Array<juce::var>());
            text=juce::JSON::toString(value,true,22);
            CHECK(decodeProposal(text.toStdString(),request.policy,*request.catalog->authority,*preflight,&reason)&&reason==ProposalDecodeReason::accepted);
        }
    }
    auto r=generateProposal(request,[&](const juce::String& model,std::string_view,const juce::var&)
    {CHECK(model==request.model);rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=text;return reply;});
    CHECK(r.status==GenerationStatus::ready&&r.candidate&&r.inferenceCalls==1);juce::MemoryBlock wire;CHECK(companion::encodeResult(request,r,wire));return r;
}
bool propose(InstrumentProcessor& p)
{
    auto request=InstrumentAiTestAccess::beginHistorical25(p,options(),400);CHECK(request.has_value());if(!request)return false;
    // Reuse legacy renders only; catalog results belong to their captured authority.
    static std::optional<GenerationRequest> oldRequest;static std::optional<GenerationResult> oldResult;
    GenerationResult r;
    if(oldRequest&&oldResult&&oldRequest->source==request->source&&!oldRequest->catalog&&!request->catalog)r=*oldResult;
    else {r=generate(*request);oldRequest=*request;oldResult=r;}
    r.tag=request->tag;juce::MemoryBlock wire;CHECK(companion::encodeResult(*request,r,wire));CHECK(p.receiveAiResult(std::move(r)));
    CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());return p.aiProposal()!=nullptr;
}
struct Head final:juce::AudioPlayHead
{
    bool available=true,playing=false;std::optional<double> bpm=125;double ppq=0;std::int64_t samples=0;
    juce::Optional<PositionInfo> getPosition() const override
    {if(!available)return {};PositionInfo p;p.setIsPlaying(playing);if(bpm)p.setBpm(*bpm);p.setPpqPosition(ppq);p.setTimeInSamples(samples);return p;}
};
void equalAudio(const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b)
{
    bool equal=a.getNumSamples()==b.getNumSamples();for(int c=0;c<2;++c)for(int n=0;n<std::min(a.getNumSamples(),b.getNumSamples());++n)equal &= bits(a.getSample(c,n))==bits(b.getSample(c,n));CHECK(equal);
}
void drain(InstrumentProcessor& p,juce::AudioBuffer<float>& b,juce::MidiBuffer& m)
{for(int n=0;n<8&&!p.aiPreviewStopped();++n)callback(p,b,m);CHECK(p.aiPreviewStopped()&&!p.aiPreviewPlaying());}
void processor_authority()
{
    begin(__func__);
    for(int cause=0;cause<9;++cause)
    {
        auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto before=setup(p);p.prepareToPlay(48000,64);
        CHECK(!p.startAiPreview());if(!propose(p))continue;const auto captured=revision(p);juce::MemoryBlock saved;p.getStateInformation(saved);
        const bool started=p.startAiPreview();CHECK(started);if(!started)continue;
        CHECK(p.aiPreviewPlaying()&&!p.aiPreviewStopped());CHECK(!p.startAiPreview());CHECK(!p.applyAiProposal());CHECK(project(p)==before&&revision(p)==captured);
        juce::AudioBuffer<float> b(2,512);juce::MidiBuffer midi;callback(p,b,midi);
        const auto audio=p.aiProposal()->audio;bool correct=true;
        for(int n=0;n<512;++n){const double gain=std::min(1.,n/240.);correct &= std::abs(b.getSample(0,n)-audio.left[n]*gain)<1e-6f&&std::abs(b.getSample(1,n)-audio.right[n]*gain)<1e-6f;}CHECK(correct);
        juce::MemoryBlock during;p.getStateInformation(during);CHECK(during==saved);CHECK(!p.undoSoundEdit());
        if(cause==0)p.discardAiProposal();
        if(cause==1){const auto field=index("output.gain_db");CHECK(p.beginParameterGesture(field));CHECK(p.setParameterGestureValue(field,.5f));CHECK(p.endParameterGesture(field));}
        if(cause==2){auto pattern=before.pattern;pattern.notes[0].pitch=62;CHECK(p.applyPatternEdit(pattern,captured));}
        if(cause==3)CHECK(recall(p,before));
        if(cause==4)CHECK(p.setAiMusicalContext({"bass","D","minor",130}));
        if(cause==5){const auto request=InstrumentAiTestAccess::beginHistorical25(p,options(),400);CHECK(request.has_value());CHECK(p.aiProposalPhase()==AiProposalPhase::pending);}
        if(cause==6){auto sound=before.sound;sound.controlLocks[index("global.master_level")]=true;CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,captured));p.refreshAiProposal();}
        if(cause==7){auto request=InstrumentAiTestAccess::beginHistorical25(p,options(),400);CHECK(request.has_value());if(request){GenerationResult failed;failed.tag=request->tag;failed.status=GenerationStatus::providerFailure;CHECK(!p.receiveAiResult(std::move(failed)));CHECK(p.aiProposalPhase()==AiProposalPhase::failed);}}
        if(cause==8){auto request=InstrumentAiTestAccess::beginHistorical25(p,options(),400);CHECK(request.has_value());CHECK(recall(p,before));if(request){GenerationResult stale;stale.tag=request->tag;CHECK(!p.receiveAiResult(std::move(stale)));CHECK(p.aiProposalPhase()==AiProposalPhase::stale);}}
        CHECK(!p.aiPreviewStopped());drain(p,b,midi);callback(p,b,midi);CHECK(b.getMagnitude(0,512)==0);CHECK(!p.startAiPreview());
    }
    auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;setup(p);p.prepareToPlay(48000,64);if(!propose(p))return;
    const auto captured=revision(p);const auto tag=p.aiProposal()->tag;const bool started=p.startAiPreview();CHECK(started);if(!started)return;
    const auto unrelated=index("output.gain_db");p.control(unrelated).setValue(.55f);CHECK(revision(p)==captured);CHECK(p.aiProposal()->tag==tag);
    auto rejected=options();rejected.instruction.clear();CHECK(!p.beginAiRequest(rejected));CHECK(!p.setAiMusicalContext({"bad","C","minor",401}));
    CHECK(p.setAiMusicalContext(p.aiMusicalContext()));auto foreign=*p.aiProposal();foreign.tag.runtime=juce::Uuid();CHECK(!p.receiveAiResult(std::move(foreign)));
    p.refreshAiProposal();CHECK(p.aiPreviewPlaying()&&!p.aiPreviewStopped()&&p.aiProposal());
    juce::AudioBuffer<float> invalid(1,32),empty(2,0),huge(2,65537),b(2,512);juce::MidiBuffer midi;
    p.stopAiPreview();callback(p,invalid,midi);callback(p,empty,midi);callback(p,huge,midi);CHECK(!p.aiPreviewStopped());CHECK(!p.currentHostBpm());drain(p,b,midi);
    CHECK(p.applyAiProposal());CHECK(project(p).sound.values[unrelated]==.55f);CHECK(p.undoSoundEdit());CHECK(project(p).sound.values[unrelated]==.55f);
    CHECK(propose(p));CHECK(p.startAiPreview());p.stopAiPreview();p.releaseResources();CHECK(p.aiPreviewStopped()&&!p.startAiPreview());
    p.prepareToPlay(48000,64);CHECK(p.startAiPreview());p.prepareToPlay(0,64);CHECK(!p.aiPreviewStopped());p.prepareToPlay(48000,0);CHECK(!p.aiPreviewStopped());
    p.prepareToPlay(48000,64);CHECK(p.aiPreviewStopped());
    // The committed engines keep running under audition in both MIDI modes.
    for(auto mode:{PlaybackMode::externalMidi,PlaybackMode::internalPattern})
    {
        auto previewStorage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),referenceStorage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& preview=*previewStorage;auto& control=*referenceStorage;
        const auto d=setup(preview,mode);CHECK(recall(control,d));Head head;head.playing=true;head.bpm=400;preview.setPlayHead(&head);control.setPlayHead(&head);
        preview.prepareToPlay(48000,64);control.prepareToPlay(48000,64);if(!propose(preview))continue;const auto source=preview.aiProposal()->audio;
        const bool okay=preview.startAiPreview();CHECK(okay);if(!okay)continue;juce::AudioBuffer<float> a(2,512),expected(2,512);juce::MidiBuffer events;
        events.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);callback(preview,a,events);callback(control,expected,events);events.clear();
        bool weighted=true;for(int c=0;c<2;++c)for(int n=0;n<512;++n){const double gain=std::min(1.,n/240.);const float sample=c==0?source.left[n]:source.right[n];const auto wanted=gain==1?sample:float(expected.getSample(c,n)*(1-gain)+sample*gain);weighted &= bits(a.getSample(c,n))==bits(wanted);}CHECK(weighted);
        preview.stopAiPreview();for(int n=0;n<3;++n){head.samples+=512;head.ppq=head.samples*400./(60*48000);callback(preview,a,events);callback(control,expected,events);if(n>0)equalAudio(a,expected);}CHECK(preview.aiPreviewStopped());
        events.addEvent(juce::MidiMessage::noteOff(1,60),0);callback(preview,a,events);callback(control,expected,events);equalAudio(a,expected);preview.setPlayHead(nullptr);control.setPlayHead(nullptr);
    }
}
struct Listener final:juce::AudioProcessorParameter::Listener
{
    InstrumentProcessor& p;bool audition=false,started=false;int begins=0,ends=0,values=0;
    explicit Listener(InstrumentProcessor& v):p(v){}
    void parameterValueChanged(int,float) override{++values;}
    void parameterGestureChanged(int,bool begin) override{if(begin){++begins;if(audition)started=p.startAiPreview();}else ++ends;}
};
void apply_reentry_and_bpm()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto before=setup(p);Head head;p.setPlayHead(&head);p.prepareToPlay(48000,64);
    CHECK(!p.currentHostBpm());if(!propose(p))return;const auto captured=revision(p);const auto tag=p.aiProposal()->tag;
    juce::AudioBuffer<float> b(2,512);juce::MidiBuffer midi;
    for(auto bpm:{125.,20.,400.,19.,401.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
    {head.bpm=bpm;callback(p,b,midi);const auto observed=p.currentHostBpm();CHECK((std::isfinite(bpm)&&bpm>=20&&bpm<=400)?observed&&*observed==bpm:!observed);CHECK(revision(p)==captured&&p.aiProposal()->tag==tag);}
    head.bpm.reset();callback(p,b,midi);CHECK(!p.currentHostBpm());head.available=false;head.bpm=125;callback(p,b,midi);CHECK(!p.currentHostBpm());
    head.available=true;callback(p,b,midi);CHECK(p.currentHostBpm()==125);p.prepareToPlay(48000,64);CHECK(!p.currentHostBpm());callback(p,b,midi);p.releaseResources();CHECK(!p.currentHostBpm());
    callback(p,b,midi);CHECK(!p.currentHostBpm());p.prepareToPlay(48000,64);
    const auto field=index("global.master_level");Listener listener(p);p.control(field).addListener(&listener);
    const bool started=p.startAiPreview();CHECK(started);
    if(started){CHECK(!p.applyAiProposal());CHECK(listener.begins==0&&listener.ends==0&&listener.values==0);p.stopAiPreview();drain(p,b,midi);}
    listener.audition=true;CHECK(!p.applyAiProposal());CHECK(listener.started&&listener.begins==1&&listener.ends==1&&listener.values==0);
    CHECK(project(p)==before&&revision(p)==captured&&p.aiProposal());CHECK(!p.aiPreviewStopped());
    listener.audition=false;p.stopAiPreview();drain(p,b,midi);CHECK(p.applyAiProposal());CHECK(listener.values==1&&listener.begins==2&&listener.ends==2);
    CHECK(p.undoSoundEdit());CHECK(project(p)==before);p.control(field).removeListener(&listener);p.setPlayHead(nullptr);
}
void ownership_and_audio_safety()
{
    begin(__func__);RenderedPreview p;p.prepare(48000);std::atomic<int> command{-1},finished{-1},emitted{0};std::atomic<bool> abort{false};
    std::atomic<unsigned> audioAlloc{0},audioFree{0},signalFailures{0},completedCommands{0};
    std::thread audio([&]
    {
        juce::AudioBuffer<float> out(2,32);int observed=-1;
        while(!abort.load())
        {
            const int current=command.load();if(current==observed){std::this_thread::yield();continue;}observed=current;
            if(current<0)continue;int frames=0;double previous=0;bool stopping=false;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!p.stopped()&&!abort.load()&&std::chrono::steady_clock::now()<deadline)
            {
                out.clear();audioGuard=true;p.mix(out,7);audioGuard=false;
                for(int n=0;n<32;++n)
                {
                    const int k=frames+n;const float left=out.getSample(0,n),right=out.getSample(1,n);
                    const float source=float(current+1)/512.f+(current%2?float(k)/65536.f:0.f);
                    const double gain=source==0?0:left/source;const double scheduled=k<1024?std::min({1.,k/240.,(1023-k)/240.}):0.;
                    if(!std::isfinite(left)||!std::isfinite(right)||(left!=0&&bits(right)!=bits(-left*.5f))||(left==0&&right!=0)||gain<-.000001||gain>scheduled+.000002)++signalFailures;
                    if(current%3!=1&&k<1024&&std::abs(gain-scheduled)>.000002)++signalFailures;
                    if(current%3==1&&k>=512&&gain<scheduled-.000002)stopping=true;
                    if(stopping&&gain>previous+.000002)++signalFailures;previous=gain;
                }
                frames+=32;emitted.store(frames);std::this_thread::yield();
            }
            if(!p.stopped()){++signalFailures;abort=true;break;}++completedCommands;finished.store(current);
        }
        audioAlloc=allocations;audioFree=deallocations;
    });
    bool allStarted=true,allAcknowledged=true;
    for(int iteration=0;iteration<200&&!abort.load();++iteration)
    {
        PreviewAudio a;a.left.resize(1024);a.right.resize(1024);
        for(int n=0;n<1024;++n){a.left[n]=float(iteration+1)/512.f+(iteration%2?float(n)/65536.f:0.f);a.right[n]=-a.left[n]*.5f;}
        emitted=0;if(!p.start(a,7)){allStarted=false;abort=true;break;}a.left.assign(1024,.99f);a.right.assign(1024,.99f);command.store(iteration);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool stopped=false;
        while(finished.load()!=iteration&&!abort.load()&&std::chrono::steady_clock::now()<deadline)
        {if(iteration%3==1&&emitted.load()>=512&&!stopped){p.stop();stopped=true;}std::this_thread::yield();}
        if(finished.load()!=iteration){allAcknowledged=false;abort=true;break;}if(!p.stopped()){allAcknowledged=false;abort=true;break;}
        p.stop(); // A completed old stop must not poison the next command.
    }
    abort=true;audio.join();CHECK(allStarted);CHECK(allAcknowledged);CHECK(completedCommands==200);CHECK(signalFailures==0);CHECK(audioAlloc==0&&audioFree==0);
    p.release();CHECK(p.stopped());CHECK(allocations==0&&deallocations==0);
    std::cout<<"Audio allocations="<<allocations+audioAlloc<<" deallocations="<<deallocations+audioFree<<" guarded callbacks="<<guardedCalls<<" race commands="<<completedCommands<<'\n';
}
void inactive_stop_concurrency()
{
    begin("inactive stopped control retirement against real concurrent mix");
    auto project=std::make_unique<ProjectDocument>();project->sound=copperArcSound();AuditionContext context;context.phrase.noteCount=1;context.phrase.notes[0]={0,.25,60,96,1};
    auto rendered=std::make_unique<RenderResult>(renderPreview(*project,context));CHECK(rendered->status==RenderStatus::ready);if(rendered->status!=RenderStatus::ready)return;
    RenderedPreview player;player.prepare(48000);std::atomic_bool stop=false;std::atomic_uint mixed=0;unsigned audioAlloc=0,audioFree=0;
    std::thread audio([&]{juce::AudioBuffer<float> block(2,128);while(!stop.load()){block.clear();audioGuard=true;player.mix(block,7);audioGuard=false;++mixed;std::this_thread::yield();}audioAlloc=allocations;audioFree=deallocations;});
    bool allStarted=true,allRetired=true;
    for(int n=0;n<64;++n){if(!player.start(rendered->audio,7)){allStarted=false;break;}const auto before=mixed.load();
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(mixed.load()==before&&std::chrono::steady_clock::now()<end)std::this_thread::yield();
        player.stop();bool retired=false;do{retired=player.retireStoppedWithoutAudio();if(retired)break;std::this_thread::yield();}while(std::chrono::steady_clock::now()<end);
        if(!retired||!player.stopped()||player.retainedPcmBytes()!=0){allRetired=false;break;}
    }
    stop=true;audio.join();CHECK(allStarted&&allRetired);CHECK(mixed>0);CHECK(audioAlloc==0&&audioFree==0);
    const auto source=fixture();CHECK(player.start(source,7));CHECK(!player.retireStoppedWithoutAudio());CHECK(player.playing());
    player.stop();CHECK(player.retireStoppedWithoutAudio());CHECK(player.retainedPcmBytes()==0);CHECK(player.stopped());
}
void processIdentity(int argc,char** argv)
{
    FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);ULARGE_INTEGER time{};time.LowPart=created.dwLowDateTime;time.HighPart=created.dwHighDateTime;
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(module)<<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<'\n';
    for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
}
}
int main(int argc,char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);const auto oldHook=_CrtSetAllocHook(heapHook);
#endif
    std::cout.setf(std::ios::unitbuf);processIdentity(argc,argv);juce::ScopedJuceInitialiser_GUI gui;
    signal_and_partitions();stop_and_lifecycle();canonical_copy_and_acknowledged_storage();processor_authority();apply_reentry_and_bpm();ownership_and_audio_safety();inactive_stop_concurrency();
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(oldHook);
#else
    CHECK(false);
#endif
    std::cout<<checks<<" checks, "<<groups<<" groups, "<<failures<<" failures\n";return failures?1:0;
}
