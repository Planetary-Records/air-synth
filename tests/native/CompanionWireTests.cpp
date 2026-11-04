#include "FrozenRackExamples.h"
#include "CompanionWire.h"
#include "AiCatalog.h"
#include "PatternIO.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace airack;
using namespace airack::ai;
using namespace airack::companion;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value) { ++failures; if (failures < 100) std::cerr << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* name) { static const auto started=std::chrono::steady_clock::now();static auto previous=started;const auto now=std::chrono::steady_clock::now();std::cout<<"WIRE_SECTION_BOUNDARY previous="<<section<<" next="<<name<<" elapsed_ms="<<std::chrono::duration<double,std::milli>(now-started).count()<<" previous_ms="<<std::chrono::duration<double,std::milli>(now-previous).count()<<" checks="<<checks<<" failures="<<failures<<std::endl;previous=now;section = name; ++sections; std::cout << name << std::endl; }
WireBytes bytes(const juce::MemoryBlock& b) { return {static_cast<const std::byte*>(b.getData()), b.getSize()}; }
juce::MemoryBlock sentinel() { constexpr unsigned char data[] {0xde,0xad,0xbe,0xef}; return {data, sizeof data}; }
template<class T> bool bits(T a, T b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
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
bool accountEqual(const AccountReply& a, const AccountReply& b)
{
    if (a.status!=b.status || a.providerStatus!=b.providerStatus || a.httpStatus!=b.httpStatus || a.errorCode!=b.errorCode
        || a.selectedModel!=b.selectedModel || a.modelVerified!=b.modelVerified || a.models.size()!=b.models.size()
        || a.activeAccount!=b.activeAccount || a.accounts!=b.accounts) return false;
    for (std::size_t i=0;i<a.models.size();++i) if (a.models[i].slug!=b.models[i].slug || a.models[i].displayName!=b.models[i].displayName) return false;
    return true;
}
GenerationRequest request()
{
    GenerationRequest r; r.tag={juce::Uuid("11223344556677889900aabbccddeeff"),9007199254740993ull,UINT64_MAX,0x0102030405060708ull};
    r.model="account-selected_model:1.0"; r.instruction="Cr\xc3\xa8me \xf0\x9f\x8e\xb9"; r.role="lead"; r.key="D"; r.scale="minor";
    return r;
}
PatternDocument phrase()
{
    PatternDocument p; p.meter={3,8}; p.grid=PatternGrid::eighth; p.swing=.351234567891;
    p.noteCount=2; p.notes[0]={.123456789, .125, 65,87,2}; p.notes[1]={.75,.25,69,93,2}; return p;
}
std::size_t index(const char* id)
{
    const auto registry=parameterRegistry(); const auto it=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return p.id==id;});
    CHECK(it!=registry.end()); return it==registry.end()?0:std::size_t(it-registry.begin());
}
void u32(juce::MemoryBlock& b,std::size_t offset,std::uint32_t value)
{
    CHECK(offset+4<=b.getSize()); if(offset+4>b.getSize())return;
    auto* p=static_cast<unsigned char*>(b.getData())+offset; for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(value>>(8*i));
}
void byte(juce::MemoryBlock& b,std::size_t offset,unsigned char value)
{ CHECK(offset<b.getSize()); if(offset<b.getSize())static_cast<unsigned char*>(b.getData())[offset]=value; }
std::uint32_t at32(const juce::MemoryBlock& b,std::size_t offset)
{
    CHECK(offset+4<=b.getSize()); if(offset+4>b.getSize())return 0;
    const auto* p=static_cast<const unsigned char*>(b.getData())+offset;
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
std::size_t skipAsset(const juce::MemoryBlock& b,std::size_t offset)
{ const auto n=at32(b,offset); CHECK(n<=b.getSize() && offset+4+n<=b.getSize()); return offset+4+n<=b.getSize()?offset+4+n:b.getSize(); }
void appendByte(juce::MemoryBlock& b) { const unsigned char value=0x42;b.append(&value,1); }
GenerationRequest captured;
GenerationResult generated;
juce::MemoryBlock requestBytes,resultBytes;
void exact_request_and_cold_macro()
{
    begin(__func__); // A missing control-thread registry bootstrap rejects a valid cold saved macro.
    auto r=request(); r.source.sound.macros[0]={0x31924c5b,.25f};
    juce::MemoryBlock cold; const bool coldOk=encodeRequest(r,cold); CHECK(coldOk);
    if(coldOk){auto out=request();CHECK(decodeRequest(bytes(cold),out));CHECK(requestEqual(out,r));}
    // Exercise every native setting with valid nontrivial float bits, preserving discrete banks.
    const auto registry=parameterRegistry(); CHECK(registry.size()==parameterCount);
    for(std::size_t i=0;i<registry.size();++i){
        if(registry[i].kind==ParameterKind::continuous)r.source.sound.values[i]=.1234567f+float(i%7)*.0712345f;
        else r.source.sound.values[i]=float(i%registry[i].choices.size())/float(registry[i].choices.size()-1);
    }
    // Keep independently chosen route targets coherent with native bank/source dependencies.
    for(std::size_t n=252;n<572;n+=5){
        const int target=int(std::lround(r.source.sound.values[n+2]*16));
        r.source.sound.values[n+3]=target<=2?0.f:(target==9 || target==10)?.5f:(target==11 || target==12)?1.f:((n/5)%2?.5f:1.f);
        if(target==0)r.source.sound.values[n]=0;
        if(r.source.sound.values[n]==1 && r.source.sound.values[n+1]==0)r.source.sound.values[n+1]=1.f/6.f;
    }
    for(std::size_t base=dynamicEqDetectorParameterStart;base<dynamicEqSpectralParameterStart;base+=5){
        const auto mode=std::lround(r.source.sound.values[base+2]*3);
        if((mode==1||mode==3)&&r.source.sound.values[base+3]>r.source.sound.values[base+4])std::swap(r.source.sound.values[base+3],r.source.sound.values[base+4]);
    }
    // Random detector/shape banks are legal stored intent in Ordinary mode.
    for(std::size_t slot=0;slot<8;++slot)r.source.sound.values[dynamicEqSpectralParameterStart+slot*25]=0;
    CHECK(validateProjectDocument(r.source));
    // Numeric persistence retains every varied selector, including inactive Drum flags.
    juce::MemoryBlock numericProject;auto numericRead=std::make_unique<ProjectDocument>();
    CHECK(encodeProjectState(r.source,numericProject));CHECK(decodeProjectState(numericProject.getData(),numericProject.getSize(),*numericRead));CHECK(*numericRead==r.source);
    // This separate generation-positive witness must use admitted melodic sources.
    for(std::size_t bank=0;bank<4;++bank)r.source.sound.values[drumModeParameterStart+bank]=0;
    r.source.mode=PlaybackMode::internalPattern; r.source.pattern=phrase();
    CHECK(editRackLayout(r.source.sound.layout,{RackEditKind::addSynth,1,masterChain,1},r.source.sound.layout)==RackEditResult::applied);
    CHECK(editRackLayout(r.source.sound.layout,{RackEditKind::removeSynth,1},r.source.sound.layout)==RackEditResult::applied);
    r.source.sound.controlLocks[757]=true;r.source.sound.effectLocks[7]=true;r.source.sound.synthLocks[3]=true;
    r.capturedPhrase=phrase();r.capturedBpm=143.1234567890123;r.policy={RequestScope::both,true,true};
    juce::MemoryBlock b;const bool ok=encodeRequest(r,b);CHECK(ok);
    if(ok){auto out=request();const bool decoded=decodeRequest(bytes(b),out);CHECK(decoded);CHECK(requestEqual(out,r));
        if(decoded){for(std::size_t i=0;i<parameterCount;++i)CHECK(bits(out.source.sound.values[i],r.source.sound.values[i]));}}
    for(auto scope:{RequestScope::sound,RequestScope::pattern,RequestScope::both}){
        auto variant=r;variant.policy={scope,false,false};variant.capturedPhrase.reset();variant.capturedBpm.reset();
        CHECK(encodeRequest(variant,b));GenerationRequest out;CHECK(decodeRequest(bytes(b),out));CHECK(requestEqual(out,variant));
    }
    r.capturedPhrase=PatternDocument{};r.capturedBpm=20;CHECK(encodeRequest(r,b));GenerationRequest out;
    CHECK(decodeRequest(bytes(b),out));CHECK(requestEqual(out,r));
    auto reject=[&](const GenerationRequest& bad){auto target=sentinel();CHECK(!encodeRequest(bad,target));CHECK(target==sentinel());};
    auto bad=r;bad.tag.runtime=juce::Uuid::null();reject(bad);bad=r;bad.tag.requestId=0;reject(bad);
    bad=r;bad.policy.scope=static_cast<RequestScope>(3);reject(bad);bad=r;bad.model="https://secret.example/token";reject(bad);
    bad=r;bad.model=juce::String::repeatedString("x",257);reject(bad);bad=r;bad.instruction.clear();reject(bad);
    bad=r;bad.instruction=std::string(8193,'x');reject(bad);bad=r;bad.role=std::string(129,'x');reject(bad);
    bad=r;bad.instruction="\xc0\xaf";reject(bad);bad=r;bad.key=std::string("D\0minor",7);reject(bad);
    bad=r;bad.capturedBpm=std::numeric_limits<double>::quiet_NaN();reject(bad);bad=r;bad.capturedBpm=400.001;reject(bad);
    bad=r;bad.source.sound.values[0]=std::numeric_limits<float>::infinity();reject(bad);
}
void admitted_result_and_audio()
{
    begin(__func__); // Re-admission binds intended edits and exact tag; wire preserves every audio bit.
    // Copper Arc owns synth 1, so moving it is in reach.
    captured=request();captured.source.sound=copperArcSound();captured.policy={RequestScope::both,true,true};captured.capturedPhrase=phrase();captured.capturedBpm=400;
    const std::string text=R"({"version":1,"parameter_edits":[{"id":"output.gain_db","normalized":0.4234567}],"rack_edits":[{"kind":"move_synth","slot":0,"chain":4,"position":0,"effect":"drive"}],"macro_edits":[{"index":8,"target_id":"output.gain_db","amount":0.2512345}],"pattern":{"bars":1,"meter_numerator":3,"meter_denominator":8,"grid":"eighth","swing":0.35,"notes":[{"start_beats":0.25,"duration_beats":0.125,"pitch":65,"velocity":87,"channel":2}]},"characteristics":"Warm crème","explanation":"Explicit sparse edits"})";
    const std::string historicalMainText=R"({"version":1,"parameter_edits":[{"id":"output.gain_db","normalized":0.4234567}],"rack_edits":[{"kind":"move_synth","slot":0,"chain":4,"position":0,"effect":"drive"}],"macro_edits":[{"index":8,"target_id":"output.gain_db","amount":0.2512345}],"pattern":{"bars":1,"meter_numerator":3,"meter_denominator":8,"grid":"eighth","swing":0.35,"notes":[{"start_beats":0.25,"duration_beats":0.125,"pitch":65,"velocity":87,"channel":2}]},"characteristics":"Warm crÃ¨me","explanation":"Explicit sparse edits"})";
    ProposalEdits historicalMain;CHECK(decodeProposal(historicalMainText,historicalMain));
    CHECK(historicalMain.characteristics==juce::String::fromUTF8("Warm cr\xc3\x83\xc2\xa8me"));
    unsigned calls=0;
    generated=generateProposal(captured,[&](const juce::String& model,std::string_view prompt,const juce::var& schema){
        ++calls;CHECK(model==captured.model);CHECK(prompt.size()<=1048576);CHECK(schema.isObject());
        rack::oauth::StructuredResult r;r.result.status=rack::oauth::Status::connected;r.result.httpStatus=200;r.text=juce::String::fromUTF8(text.c_str());return r;
    });
    CHECK(calls==1);CHECK(generated.status==GenerationStatus::ready);CHECK(generated.edits.characteristics==juce::String::fromUTF8("Warm cr\xc3\xa8me"));CHECK(generated.candidate.has_value());CHECK(!generated.audio.left.empty());
    if(generated.status!=GenerationStatus::ready || !generated.candidate || generated.audio.left.empty())return;
    CHECK(generated.candidate->mode==captured.source.mode);CHECK(generated.edits.parameters.size()==1 && generated.edits.racks.size()==1 && generated.edits.macros.size()==1 && generated.edits.pattern);
    CHECK(encodeRequest(captured,requestBytes));const bool ok=encodeResult(captured,generated,resultBytes);CHECK(ok);
    if(ok){GenerationResult out;const bool decoded=decodeResult(bytes(resultBytes),captured,out);CHECK(decoded);CHECK(resultEqual(out,generated));
        if(decoded && out.audio.left.size()==generated.audio.left.size() && out.audio.right.size()==generated.audio.right.size())
            for(std::size_t i=0;i<out.audio.left.size();++i){CHECK(bits(out.audio.left[i],generated.audio.left[i]));CHECK(bits(out.audio.right[i],generated.audio.right[i]));}}
    auto reject=[&](const GenerationRequest& r,const GenerationResult& value){auto target=sentinel();CHECK(!encodeResult(r,value,target));CHECK(target==sentinel());};
    auto rounded=generated;rounded.measurements.decaySeconds=std::nextafter(4.,5.);juce::MemoryBlock b;CHECK(encodeResult(captured,rounded,b));
    GenerationResult out;CHECK(decodeResult(bytes(b),captured,out));CHECK(bits(out.measurements.decaySeconds,rounded.measurements.decaySeconds));
    rounded.measurements.decaySeconds=4.+1e-8;reject(captured,rounded);
    auto bad=generated;++bad.tag.requestId;reject(captured,bad);bad=generated;bad.candidate->sound.values[index("output.gain_db")]=.7f;reject(captured,bad);
    auto locked=captured;locked.source.sound.controlLocks[index("output.gain_db")]=true;reject(locked,generated);
    locked=captured;locked.policy.scope=RequestScope::sound;reject(locked,generated);
    bad=generated;bad.audio.sampleRate=44100;reject(captured,bad);bad=generated;bad.audio.right.pop_back();reject(captured,bad);
    bad=generated;bad.audio.left[0]=1;reject(captured,bad);bad=generated;bad.audio.right[0]=std::numeric_limits<float>::quiet_NaN();reject(captured,bad);
    bad=generated;bad.inferenceCalls=0;reject(captured,bad);bad=generated;bad.providerStatus=rack::oauth::Status::ready;reject(captured,bad);
    bad=generated;bad.measurements.peak=1;reject(captured,bad);bad=generated;bad.measurements.bandFractions={.2,.2,.2};reject(captured,bad);
    bad=generated;bad.measurements.onsetSeconds=-1e-12;reject(captured,bad);bad=generated;bad.audio.context.bpm=19.99;reject(captured,bad);
    bad=generated;bad.edits.parameters.resize(759);reject(captured,bad);bad=generated;bad.edits.racks.resize(65);reject(captured,bad);
    bad=generated;bad.edits.macros.resize(9);reject(captured,bad);
    for(auto status:{GenerationStatus::invalidRequest,GenerationStatus::cancelled,GenerationStatus::providerFailure,GenerationStatus::budgetExhausted,GenerationStatus::renderFailure}){
        GenerationResult failed;failed.tag=captured.tag;failed.status=status;failed.inferenceCalls=3;failed.httpStatus=429;failed.errorCode="rate_limit_exceeded";
        failed.measurements.finite=false;failed.measurements.clippedSamples=1152000;
        failed.measurements.peak=std::numeric_limits<double>::infinity();failed.measurements.rms=std::bit_cast<double>(0x7ff800000000002aull);
        failed.measurements.onsetSeconds=-std::numeric_limits<double>::infinity();failed.measurements.bandFractions[1]=std::numeric_limits<double>::quiet_NaN();
        CHECK(encodeResult(captured,failed,b));GenerationResult decoded;CHECK(decodeResult(bytes(b),captured,decoded));CHECK(resultEqual(failed,decoded));
        bad=failed;bad.candidate=captured.source;reject(captured,bad);bad=failed;bad.edits.explanation="hidden edit";reject(captured,bad);bad=failed;bad.audio.left={.1f};reject(captured,bad);
    }
}
void names_cross_the_wire()
{
    begin(__func__);CHECK(wireVersion==9);
    auto r=request();r.namesInUse={"Ethereal Rolling Bass",juce::String(juce::CharPointer_UTF8("Cr\xc3\xa8me Keys"))};
    juce::MemoryBlock b;CHECK(encodeRequest(r,b));auto out=request();CHECK(decodeRequest(bytes(b),out));CHECK(out.namesInUse==r.namesInUse);CHECK(requestEqual(out,r));
    auto rejectRequest=[&](const GenerationRequest& bad){auto target=sentinel();CHECK(!encodeRequest(bad,target));CHECK(target==sentinel());};
    auto bad=r;for(int i=0;i<199;++i)bad.namesInUse.push_back("Name "+juce::String(i));CHECK(bad.namesInUse.size()==201);rejectRequest(bad);
    bad=r;bad.namesInUse.push_back(juce::String::repeatedString("a",161));rejectRequest(bad);
    // The admitted result left by admitted_result_and_audio, named as admission names its candidate.
    if(generated.status!=GenerationStatus::ready||!generated.candidate)return;
    auto named=generated;named.edits.name="Warm Creme Lead";named.candidate->sound.name="Warm Creme Lead";
    const bool ok=encodeResult(captured,named,b);CHECK(ok);
    if(ok){GenerationResult decoded;CHECK(decodeResult(bytes(b),captured,decoded));CHECK(decoded.edits.name=="Warm Creme Lead");CHECK(resultEqual(decoded,named));}
    auto rejectResult=[&](const GenerationResult& value){auto target=sentinel();CHECK(!encodeResult(captured,value,target));CHECK(target==sentinel());};
    auto wrong=named;wrong.candidate->sound.name="Other Name";rejectResult(wrong);
    wrong=named;wrong.edits.name=juce::String::repeatedString("a",161);wrong.candidate->sound.name=wrong.edits.name;rejectResult(wrong);
}
void frame_and_control_bounds()
{
    begin(__func__); // Bad versions, session/control shapes or length arithmetic must fail before copying payloads.
    const auto runtime=request().tag.runtime;ControlMessage control;control.tag.runtime=runtime;
    juce::MemoryBlock payload;const bool ok=encodeControl(control,payload);CHECK(ok);CHECK(payload.getSize()==41);
    if(ok){constexpr unsigned char expectedTag[]{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0x00,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};CHECK(payload==juce::MemoryBlock(expectedTag,sizeof expectedTag));}
    control.tag=request().tag;CHECK(encodeControl(control,payload));
    if(payload.getSize()==41){constexpr unsigned char integerBytes[]{1,0,0,0,0,0,0x20,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,8,7,6,5,4,3,2,1};CHECK(std::memcmp(static_cast<const unsigned char*>(payload.getData())+16,integerBytes,sizeof integerBytes)==0);}
    for(auto kind:{MessageKind::hello,MessageKind::welcome,MessageKind::cancel,MessageKind::ping,MessageKind::pong,MessageKind::failure}){
        control={};control.tag.runtime=runtime;if(kind==MessageKind::cancel)control.tag=request().tag;if(kind==MessageKind::failure)control.status=RuntimeStatus::unavailable;
        CHECK(encodeControl(control,payload));Frame f{kind,kind==MessageKind::hello?juce::Uuid::null():runtime,payload};juce::MemoryBlock b;
        const bool framed=encodeFrame(f,b);CHECK(framed);Frame out;CHECK(decodeFrame(bytes(b),out));CHECK(out.kind==f.kind && out.session==f.session && out.payload==f.payload);
        FrameHeader h;CHECK(decodeFrameHeader(bytes(b).first(std::min<std::size_t>(28,b.getSize())),h));CHECK(h.kind==kind && h.session==f.session && h.payloadBytes==41);
        if(kind==MessageKind::hello && framed){constexpr unsigned char header[]{'A','R','S','4',static_cast<unsigned char>(wireVersion),0,1,0,41,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};CHECK(std::memcmp(b.getData(),header,28)==0);}
        auto reject=[&](const Frame& bad){auto target=sentinel();CHECK(!encodeFrame(bad,target));CHECK(target==sentinel());};
        auto bad=f;bad.session=kind==MessageKind::hello?runtime:juce::Uuid::null();reject(bad);
        auto c=control;if(kind==MessageKind::cancel)c.tag.requestId=0;else if(kind==MessageKind::failure)c.status=RuntimeStatus::ready;else c.tag.editRevision=1;
        CHECK(encodeControl(c,bad.payload));bad.session=f.session;reject(bad);
    }
    for(auto kind:{MessageKind::generate,MessageKind::result,MessageKind::account,MessageKind::accountResult}){
        const auto cap=kind==MessageKind::result?8388608:kind==MessageKind::generate?917504:65536;
        Frame f{kind,runtime,juce::MemoryBlock(std::size_t(cap),true)};juce::MemoryBlock b;CHECK(encodeFrame(f,b));Frame out;
        CHECK(decodeFrame(bytes(b),out));CHECK(out.payload==f.payload);appendByte(f.payload);auto target=sentinel();CHECK(!encodeFrame(f,target));CHECK(target==sentinel());
        if(b.getSize()>=28){u32(b,8,UINT32_MAX);FrameHeader h;h.payloadBytes=123;CHECK(!decodeFrameHeader(bytes(b).first(28),h));CHECK(h.payloadBytes==123);}
    }
    for(unsigned status=0;status<=9;++status){control.status=static_cast<RuntimeStatus>(status);CHECK(encodeControl(control,payload));ControlMessage out;CHECK(decodeControl(bytes(payload),out));CHECK(out.tag==control.tag && out.status==control.status);}
    control={};control.tag.runtime=runtime;control.status=RuntimeStatus::busy;CHECK(encodeControl(control,payload));
    Frame welcome{MessageKind::welcome,runtime,payload};juce::MemoryBlock framed;CHECK(encodeFrame(welcome,framed));
    auto shapeReject=[&](MessageKind kind,const ControlMessage& value){Frame f{kind,kind==MessageKind::hello?juce::Uuid::null():runtime,{}};CHECK(encodeControl(value,f.payload));auto out=sentinel();CHECK(!encodeFrame(f,out));CHECK(out==sentinel());};
    for(auto kind:{MessageKind::hello,MessageKind::ping,MessageKind::pong})shapeReject(kind,control);
    control.status=RuntimeStatus::cancelled;control.tag.editRevision=1;shapeReject(MessageKind::failure,control);
    control.tag.editRevision=0;control.tag.contextRevision=1;shapeReject(MessageKind::failure,control);
    control.tag=request().tag;control.status=static_cast<RuntimeStatus>(10);auto unknown=sentinel();CHECK(!encodeControl(control,unknown));CHECK(unknown==sentinel());
    control.tag.runtime=juce::Uuid::null();auto target=sentinel();CHECK(!encodeControl(control,target));CHECK(target==sentinel());
}
void malformed_transactional_decoding()
{
    begin(__func__); // Exhaustion, oversized count, invalid raw UTF8, stale authority and corrupt nested assets preserve sentinels.
    // A result owner may retire stale requests after inspecting only a bounded exact native tag prefix.
    constexpr unsigned char prefix[]{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
        1,0,0,0,0,0,0x20,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,8,7,6,5,4,3,2,1};
    juce::MemoryBlock tagOnly(prefix,sizeof prefix);RequestTag peeked;CHECK(peekResultTag(bytes(tagOnly),peeked));CHECK(peeked==request().tag);
    appendByte(tagOnly);CHECK(peekResultTag(bytes(tagOnly),peeked));CHECK(peeked==request().tag); // Remaining data has no admission authority.
    auto rejectTag=[&](const juce::MemoryBlock& bad){auto out=request().tag;out.requestId=17;const auto before=out;CHECK(!peekResultTag(bytes(bad),out));CHECK(out==before);};
    rejectTag(juce::MemoryBlock(prefix,39));auto invalid=tagOnly;std::memset(invalid.getData(),0,16);rejectTag(invalid);
    invalid=tagOnly;std::memset(static_cast<unsigned char*>(invalid.getData())+16,0,8);rejectTag(invalid);
    juce::MemoryBlock large(8388608,true);std::memcpy(large.getData(),prefix,sizeof prefix);CHECK(peekResultTag(bytes(large),peeked));CHECK(peeked==request().tag);appendByte(large);rejectTag(large);
    auto source=request();source.capturedPhrase=phrase();source.capturedBpm=137.123456789;
    juce::MemoryBlock b;const bool ok=encodeRequest(source,b);CHECK(ok);
    auto rejectRequest=[&](const juce::MemoryBlock& bad){auto out=request();out.instruction="sentinel";const auto before=out;CHECK(!decodeRequest(bytes(bad),out));CHECK(requestEqual(out,before));};
    if(ok){
        std::vector<std::size_t> boundaries{0,16,24,32,40,41,42,43,44};std::size_t p=52;
        for(int i=0;i<5;++i){boundaries.push_back(p);p=skipAsset(b,p);boundaries.push_back(p);}
        const auto projectLength=p;const auto projectStart=p+4;p=skipAsset(b,p);boundaries.push_back(p);
        const auto phraseFlag=p++;const auto phraseLength=p;p=skipAsset(b,p);boundaries.push_back(p);boundaries.push_back(p+1);boundaries.push_back(b.getSize()-1);
        for(auto n:boundaries){CHECK(n<b.getSize());if(n<b.getSize())rejectRequest(juce::MemoryBlock(b.getData(),n));}
        auto bad=b;appendByte(bad);rejectRequest(bad);
        for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned char>>{{40,3},{41,2},{42,255},{43,2},{43,255},{56,0},{56,0xff},{projectStart,'X'},{phraseFlag,2},{b.getSize()-13,2}}){bad=b;byte(bad,offset,value);rejectRequest(bad);}
        bad=b;u32(bad,52,UINT32_MAX);rejectRequest(bad);bad=b;u32(bad,projectLength,7006);rejectRequest(bad);bad=b;u32(bad,phraseLength,4921);rejectRequest(bad);
        bad=b;byte(bad,projectStart+at32(b,projectLength)-1,0);rejectRequest(bad);
        bad=b;const auto nan=std::bit_cast<std::array<unsigned char,8>>(std::numeric_limits<double>::infinity());std::memcpy(static_cast<unsigned char*>(bad.getData())+bad.getSize()-12,nan.data(),8);rejectRequest(bad);
    }
    if(!resultBytes.isEmpty()){
        auto rejectResult=[&](const juce::MemoryBlock& bad,const GenerationRequest& r){auto out=generated;out.errorCode="sentinel";const auto before=out;CHECK(!decodeResult(bytes(bad),r,out));CHECK(resultEqual(out,before));};
        // Actual fixture errorCode is empty and onset is present: metadata length is hand-derived from the fixed layout.
        CHECK(generated.measurements.onsetSeconds.has_value());const std::size_t candidateLength=49+4+1+4+16+1+(generated.measurements.onsetSeconds?8:0)+16+24+1;
        const auto candidateEnd=skipAsset(resultBytes,candidateLength);const auto parameterCountOffset=candidateEnd;
        auto p=candidateEnd+4;const auto parameterIdLength=p;p=skipAsset(resultBytes,p)+4;const auto rackCountOffset=p;p+=4+6;
        const auto macroCountOffset=p;p+=4+1;p=skipAsset(resultBytes,p)+4;p+=1;p=skipAsset(resultBytes,p);p=skipAsset(resultBytes,p);p=skipAsset(resultBytes,p);p=skipAsset(resultBytes,p);
        p+=9;const auto preview=p;p+=4+1+1+1+8;p=skipAsset(resultBytes,p);const auto frameCountOffset=p;const auto audioStart=p+4;
        CHECK(audioStart+generated.audio.left.size()*8==resultBytes.getSize());
        for(auto n:{std::size_t(16),std::size_t(40),std::size_t(44),std::size_t(49),candidateLength,candidateEnd,parameterCountOffset+4,parameterIdLength+4,rackCountOffset,macroCountOffset,preview,frameCountOffset,audioStart,resultBytes.getSize()-1}){
            if(n<resultBytes.getSize())rejectResult(juce::MemoryBlock(resultBytes.getData(),n),captured);
        }
        auto bad=resultBytes;appendByte(bad);rejectResult(bad,captured);auto stale=captured;++stale.tag.contextRevision;rejectResult(resultBytes,stale);
        for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned char>>{{40,6},{41,9},{42,6},{43,17},{44,4},{53,2},{preview+4,2},{preview+5,3},{preview+6,2}}){bad=resultBytes;byte(bad,offset,value);rejectResult(bad,captured);}
        for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,std::uint32_t>>{{parameterCountOffset,UINT32_MAX},{rackCountOffset,65},{macroCountOffset,9},{frameCountOffset,UINT32_MAX},{parameterIdLength,257}}){bad=resultBytes;u32(bad,offset,value);rejectResult(bad,captured);}
        bad=resultBytes;u32(bad,audioStart,0x7f800000u);rejectResult(bad,captured);
        bad=resultBytes;u32(bad,audioStart,0x3f800000u);rejectResult(bad,captured);
        bad=resultBytes;byte(bad,candidateLength+4+at32(resultBytes,candidateLength)-1,0);rejectResult(bad,captured);
    }
    ControlMessage control;control.tag=request().tag;CHECK(encodeControl(control,b));auto rejectControl=[&](const juce::MemoryBlock& bad){ControlMessage out;out.tag=request().tag;out.status=RuntimeStatus::queueFull;const auto before=out;CHECK(!decodeControl(bytes(bad),out));CHECK(out.tag==before.tag && out.status==before.status);};
    if(b.getSize()==41){auto bad=b;byte(bad,40,10);rejectControl(bad);bad=b;appendByte(bad);rejectControl(bad);rejectControl(juce::MemoryBlock(b.getData(),40));}
    Frame f{MessageKind::generate,request().tag.runtime,sentinel()};CHECK(encodeFrame(f,b));
    if(b.getSize()>=28){auto rejectFrame=[&](const juce::MemoryBlock& bad){Frame out{MessageKind::account,request().tag.runtime,sentinel()};CHECK(!decodeFrame(bytes(bad),out));CHECK(out.kind==MessageKind::account && out.session==request().tag.runtime && out.payload==sentinel());};
        for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned char>>{{0,'X'},{4,1},{6,0},{6,255}}){auto bad=b;byte(bad,offset,value);rejectFrame(bad);}
        auto bad=b;appendByte(bad);rejectFrame(bad);for(auto n:{std::size_t(0),std::size_t(7),std::size_t(11),std::size_t(27),b.getSize()-1})rejectFrame(juce::MemoryBlock(b.getData(),n));
    }
}
void account_messages()
{
    begin(__func__); // Closed account payloads cannot carry extra credentials, duplicate model identities or unbounded text/counts.
    for(auto action:{AccountAction::status,AccountAction::models,AccountAction::selectModel,AccountAction::signIn,AccountAction::addAccount,AccountAction::selectAccount}){
        const bool selection=action==AccountAction::selectModel||action==AccountAction::selectAccount;
        AccountCommand c{action,selection?juce::String("authorized:model-1.2"):juce::String{}};juce::MemoryBlock b;
        CHECK(encodeAccountCommand(c,b));AccountCommand out;CHECK(decodeAccountCommand(bytes(b),out));CHECK(out.action==c.action && out.model==c.model);
        c.model=selection?"":"token";auto target=sentinel();CHECK(!encodeAccountCommand(c,target));CHECK(target==sentinel());
    }
    AccountReply reply;reply.providerStatus=rack::oauth::Status::connected;reply.httpStatus=200;reply.selectedModel="authorized:model-1.2";reply.modelVerified=true;
    reply.models={{reply.selectedModel,juce::String::fromUTF8("Cr\xc3\xa8me \xf0\x9f\x8e\xb9")},{"other_model","Other"}};
    reply.accounts={{"client-a","Account 1"},{"client-b","Account 2"}};reply.activeAccount="client-b";
    juce::MemoryBlock b;const bool ok=encodeAccountReply(reply,b);CHECK(ok);AccountReply out;CHECK(decodeAccountReply(bytes(b),out));CHECK(accountEqual(out,reply));
    for(unsigned status=0;status<=16;++status){auto variant=reply;variant.providerStatus=static_cast<rack::oauth::Status>(status);variant.status=static_cast<RuntimeStatus>(status%10);CHECK(encodeAccountReply(variant,b));CHECK(decodeAccountReply(bytes(b),out));CHECK(accountEqual(out,variant));}
    auto reject=[&](const AccountReply& bad){auto target=sentinel();CHECK(!encodeAccountReply(bad,target));CHECK(target==sentinel());};
    auto bad=reply;bad.models.push_back(reply.models.front());reject(bad);bad=reply;bad.models.resize(129);reject(bad);
    bad=reply;bad.status=static_cast<RuntimeStatus>(10);reject(bad);bad=reply;bad.providerStatus=static_cast<rack::oauth::Status>(17);reject(bad);
    bad=reply;bad.httpStatus=99;reject(bad);bad=reply;bad.selectedModel.clear();reject(bad);bad=reply;bad.models[0].slug="secret/token";reject(bad);
    bad=reply;bad.models[0].displayName=juce::String::repeatedString("x",1025);reject(bad);bad=reply;bad.errorCode=juce::String::repeatedString("x",257);reject(bad);
    bad=reply;bad.activeAccount="unknown-client";reject(bad);bad=reply;bad.accounts[1].id=bad.accounts[0].id;reject(bad);
    bad=reply;bad.accounts[0].label.clear();reject(bad);bad=reply;bad.accounts[0].label=juce::String::repeatedString("x",513);reject(bad);
    bad=reply;bad.accounts.clear();for(unsigned i=0;i<=rack::oauth::maximumSavedAccounts;++i)bad.accounts.push_back({"client-"+juce::String(i),"Account"});reject(bad);
    // In-cap individual fields can still exceed the complete control payload.
    bad=reply;bad.models.clear();for(unsigned i=0;i<128;++i)bad.models.push_back({juce::String("model-")+juce::String(i),juce::String::repeatedString("x",1024)});reject(bad);
    if(ok){CHECK(encodeAccountReply(reply,b));auto rejectBytes=[&](const juce::MemoryBlock& corrupt){auto target=reply;target.errorCode="sentinel";const auto before=target;CHECK(!decodeAccountReply(bytes(corrupt),target));CHECK(accountEqual(target,before));};
        auto corrupt=b;appendByte(corrupt);const char extra[]="bearer=secret&authorization_url=https://example";corrupt.append(extra,sizeof extra);rejectBytes(corrupt);
        const auto selectedLength=10;const auto verified=skipAsset(b,selectedLength);const auto count=verified+1;const auto firstSlug=count+4;
        for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned char>>{{0,10},{1,17},{verified,2},{firstSlug+4,0},{firstSlug+4,0xff}}){corrupt=b;byte(corrupt,offset,value);rejectBytes(corrupt);}
        corrupt=b;u32(corrupt,count,UINT32_MAX);rejectBytes(corrupt);corrupt=b;u32(corrupt,firstSlug,257);rejectBytes(corrupt);
        for(auto n:{std::size_t(0),std::size_t(1),std::size_t(6),std::size_t(10),verified,count,firstSlug,b.getSize()-1})rejectBytes(juce::MemoryBlock(b.getData(),n));
    }
    AccountCommand c;CHECK(encodeAccountCommand(c,b));AccountCommand before{AccountAction::models,"sentinel"};auto target=before;
    if(b.getSize()>=5){byte(b,0,6);CHECK(!decodeAccountCommand(bytes(b),target));CHECK(target.action==before.action && target.model==before.model);}
    CHECK(!decodeAccountCommand({},target));CHECK(target.action==before.action && target.model==before.model);
}
void required_policy_request_wire()
{
    begin(__func__);
    for(auto scope:{RequestScope::pattern,RequestScope::both})for(bool required:{false,true}) {
        auto source=request();source.policy={scope,true,true,required};source.source.pattern=phrase();
        source.capturedPhrase=phrase();source.capturedBpm=143.1234567890123;
        juce::MemoryBlock b;const bool encoded=encodeRequest(source,b);CHECK(encoded);
        if(!encoded)continue;
        CHECK(b.getSize()>48);
        if(b.getSize()>48){const auto* p=static_cast<const unsigned char*>(b.getData());
            CHECK(p[40]==static_cast<unsigned char>(scope));CHECK(p[41]==1 && p[42]==1);
            CHECK(p[43]==(required?1:0));CHECK(at32(b,52)==source.model.getNumBytesAsUTF8());
            CHECK(std::memcmp(p+56,source.model.toRawUTF8(),source.model.getNumBytesAsUTF8())==0);
        }
        auto out=request();out.policy.requirePattern=!required;
        CHECK(decodeRequest(bytes(b),out));CHECK(requestEqual(out,source));
        CHECK(out.policy.requirePattern==required && out.source.pattern==source.source.pattern);
        auto reject=[&](const juce::MemoryBlock& bad){auto target=request();target.policy={RequestScope::both,true,true,true};target.instruction="sentinel";
            const auto before=target;CHECK(!decodeRequest(bytes(bad),target));CHECK(requestEqual(target,before));};
        for(std::size_t size=0;size<b.getSize();++size)reject(juce::MemoryBlock(b.getData(),size));
        for(unsigned char value:{2,255}){auto bad=b;byte(bad,43,value);reject(bad);}
        if(required){auto bad=b;byte(bad,40,static_cast<unsigned char>(RequestScope::sound));reject(bad);}
    }
    auto invalid=request();invalid.policy={RequestScope::sound,false,false,true};auto b=sentinel();
    CHECK(!encodeRequest(invalid,b));CHECK(b==sentinel());
}
void optional_result_required_readmission()
{
    begin(__func__);
    for(bool empty:{false,true}) {
        auto source=request();source.policy.scope=RequestScope::both;source.source.pattern=phrase();source.capturedBpm=400;
        const std::string pattern=empty?R"({"bars":1,"meter_numerator":4,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[]})":"null";
        const std::string text=R"({"version":1,"parameter_edits":[],"rack_edits":[],"macro_edits":[],"pattern":)"+pattern+R"(,"characteristics":"warm","explanation":"preserve or clear"})";
        const auto optional=generateProposal(source,[&](const juce::String&,std::string_view,const juce::var&){rack::oauth::StructuredResult r;
            r.result.status=rack::oauth::Status::connected;r.result.httpStatus=200;r.text=juce::String::fromUTF8(text.c_str());return r;});
        CHECK(optional.status==GenerationStatus::ready && optional.candidate);if(optional.status!=GenerationStatus::ready || !optional.candidate)continue;
        CHECK(empty?(optional.edits.pattern && optional.edits.pattern->noteCount==0):!optional.edits.pattern);
        CHECK(empty?optional.candidate->pattern.noteCount==0:optional.candidate->pattern==source.source.pattern);
        juce::MemoryBlock encoded;CHECK(encodeResult(source,optional,encoded));GenerationResult decoded;
        CHECK(decodeResult(bytes(encoded),source,decoded));CHECK(resultEqual(decoded,optional));
        auto required=source;required.policy.requirePattern=true;auto target=sentinel();
        CHECK(!encodeResult(required,optional,target));CHECK(target==sentinel());
        decoded=optional;decoded.errorCode="sentinel";const auto before=decoded;
        CHECK(!decodeResult(bytes(encoded),required,decoded));CHECK(resultEqual(decoded,before));
    }
    CHECK(generated.status==GenerationStatus::ready && generated.edits.pattern && generated.edits.pattern->noteCount>0);
    if(generated.status==GenerationStatus::ready && generated.edits.pattern && generated.edits.pattern->noteCount>0) {
        auto required=captured;required.policy.requirePattern=true;juce::MemoryBlock encoded;
        CHECK(encodeResult(required,generated,encoded));GenerationResult out;
        CHECK(decodeResult(bytes(encoded),required,out));CHECK(resultEqual(out,generated));
    }
}
void missing_pattern_failure_wire()
{
    begin(__func__);auto source=request();source.policy={RequestScope::both,false,false,true};source.source.pattern=phrase();
    GenerationResult failed;failed.tag=source.tag;failed.status=GenerationStatus::budgetExhausted;failed.inferenceCalls=3;
    failed.proposalStatus=ProposalStatus::requestedPatternMissing;failed.providerStatus=rack::oauth::Status::connected;
    failed.httpStatus=200;failed.errorCode="requested_pattern_missing";
    juce::MemoryBlock encoded;const bool ok=encodeResult(source,failed,encoded);CHECK(ok);
    if(ok){GenerationResult out;out.tag=request().tag;out.errorCode="sentinel";CHECK(decodeResult(bytes(encoded),source,out));CHECK(resultEqual(out,failed));
        CHECK(!out.candidate && !out.edits.pattern && out.edits.parameters.empty() && out.edits.racks.empty() && out.edits.macros.empty());
        CHECK(out.audio.left.empty() && out.audio.right.empty() && out.audio.bodyFrames==0);
        auto bad=encoded;byte(bad,41,static_cast<unsigned char>(ProposalStatus::outOfReach)+1);
        out.errorCode="sentinel";const auto before=out;CHECK(!decodeResult(bytes(bad),source,out));CHECK(resultEqual(out,before));
        auto reach=failed;reach.proposalStatus=ProposalStatus::outOfReach;reach.errorCode.clear();juce::MemoryBlock reached;
        CHECK(encodeResult(source,reach,reached));GenerationResult back;CHECK(decodeResult(bytes(reached),source,back)&&resultEqual(back,reach));
    }
    auto invalid=failed;invalid.proposalStatus=static_cast<ProposalStatus>(static_cast<unsigned char>(ProposalStatus::outOfReach)+1);
    auto target=sentinel();CHECK(!encodeResult(source,invalid,target));CHECK(target==sentinel());
    for(int payload=0;payload<3;++payload){auto invalid=failed;if(payload==0)invalid.candidate=source.source;
        if(payload==1)invalid.edits.pattern=phrase();if(payload==2)invalid.audio.left={.1f};
        target=sentinel();CHECK(!encodeResult(source,invalid,target));CHECK(target==sentinel());}
}
void v4_large_state_frames()
{
    begin(__func__);
    auto r=request();r.structuralGeneration=UINT64_MAX;
    juce::MemoryBlock b;CHECK(encodeRequest(r,b));GenerationRequest out;CHECK(decodeRequest(bytes(b),out));
    CHECK(out.structuralGeneration==UINT64_MAX);
    for(auto kind:{MessageKind::generate,MessageKind::generateGroup,MessageKind::groupCommand,MessageKind::result,MessageKind::account,MessageKind::groupReply}){
        const std::size_t cap=kind==MessageKind::generate?917504:kind==MessageKind::result?8388608:(kind==MessageKind::generateGroup||kind==MessageKind::groupCommand)?1048576:65536;
        Frame frame{kind,r.tag.runtime,juce::MemoryBlock(cap,true)};CHECK(encodeFrame(frame,b));FrameHeader h;CHECK(decodeFrameHeader(bytes(b).first(28),h));CHECK(h.payloadBytes==cap);
        u32(b,8,std::uint32_t(cap+1));const auto before=h;CHECK(!decodeFrameHeader(bytes(b).first(28),h));CHECK(h.payloadBytes==before.payloadBytes);
    }
    Frame frame{MessageKind::generate,r.tag.runtime,sentinel()};CHECK(encodeFrame(frame,b));CHECK(static_cast<const unsigned char*>(b.getData())[4]==wireVersion);
    for(auto version:{3,4,5,6,7,8}){auto bad=b;byte(bad,4,(unsigned char)version);FrameHeader h;CHECK(!decodeFrameHeader(bytes(bad).first(28),h));}
}

void version_two_rejects_legacy_frames()
{
    begin(__func__);CHECK(wireVersion==9);
    Frame source{MessageKind::generate,request().tag.runtime,sentinel()};juce::MemoryBlock b;CHECK(encodeFrame(source,b));
    CHECK(b.getSize()==wireHeaderBytes+4);if(b.getSize()!=wireHeaderBytes+4)return;
    auto current=b;byte(current,4,wireVersion);byte(current,5,0);Frame out;FrameHeader header;
    CHECK(decodeFrameHeader(bytes(current).first(wireHeaderBytes),header));CHECK(header.kind==source.kind && header.session==source.session && header.payloadBytes==4);
    CHECK(decodeFrame(bytes(current),out));CHECK(out.kind==source.kind && out.session==source.session && out.payload==source.payload);
    auto reject=[&](const juce::MemoryBlock& bad){Frame target{MessageKind::account,source.session,sentinel()};
        CHECK(!decodeFrame(bytes(bad),target));CHECK(target.kind==MessageKind::account && target.session==source.session && target.payload==sentinel());};
    auto legacy=b;byte(legacy,4,1);byte(legacy,5,0);
    header={MessageKind::account,source.session,123};const auto before=header;
    CHECK(!decodeFrameHeader(bytes(legacy).first(wireHeaderBytes),header));CHECK(header.kind==before.kind && header.session==before.session && header.payloadBytes==before.payloadBytes);
    reject(legacy);for(std::size_t size=0;size<current.getSize();++size)reject(juce::MemoryBlock(current.getData(),size));
}
std::uint32_t read32(const juce::MemoryBlock& b,std::size_t pos){const auto* p=static_cast<const unsigned char*>(b.getData())+pos;return p[0]|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;}
void resign(juce::MemoryBlock& b){const auto hash=juce::SHA256(b.getData(),b.getSize()-32).getRawData();std::memcpy(static_cast<char*>(b.getData())+b.getSize()-32,hash.getData(),32);}
juce::MemoryBlock replaceBlock(const juce::MemoryBlock& b,std::size_t offset,std::size_t count,const juce::MemoryBlock& inserted){juce::MemoryBlock out(b.getData(),offset);out.append(inserted.getData(),inserted.getSize());out.append(static_cast<const char*>(b.getData())+offset+count,b.getSize()-offset-count);return out;}
void v4_nested_personality_and_count_bounds()
{
    // The 320 neutral controls add 2880 saved bytes; transport caps stay fixed.
    static_assert(843396<=maximumGenerationRequestBytes);
    // Keep the many independent maximum-size request/result witnesses off the
    // default Windows stack; codec calls retain their production stack budget.
    begin(__func__);auto rOwner=std::make_unique<GenerationRequest>(request());auto& r=*rOwner;r.model=juce::String::repeatedString("m",256);r.instruction=std::string(8192,'"');r.role=r.key=r.scale=std::string(128,'\\');r.structuralGeneration=UINT64_MAX;
    for(std::uint8_t n=1;n<4;++n)CHECK(editRackLayout(r.source.sound.layout,{RackEditKind::addSynth,n,masterChain,n},r.source.sound.layout)==RackEditResult::applied);
    for(std::uint8_t n=0;n<8;++n)CHECK(editRackLayout(r.source.sound.layout,{RackEditKind::addEffect,n,masterChain,n},r.source.sound.layout)==RackEditResult::applied);
    auto instances=std::make_shared<RackPersonalityState>();for(std::uint8_t n=0;n<12;++n){RackInstance i;i.design.name=juce::String::repeatedString("N",64);i.design.intent=juce::String::repeatedString("\\\"",256);i.design.revision=UINT64_MAX;i.design.role=n<4?RackRole::synth:RackRole::fx;i.design.panel.sections={juce::String::repeatedString("S",64),juce::String::repeatedString("S",64),juce::String::repeatedString("S",64),juce::String::repeatedString("S",64)};i.design.units={{0,n<4?NativeModule::analog:NativeModule::drive,{}}};i.bindings={{0,std::uint8_t(n<4?n:n-4)}};
        const auto prefix=n<4?juce::String("synth.1."):juce::String("effect.1.");for(const auto& d:parameterRegistry())if(d.id.startsWith(prefix)&&d.kind==ParameterKind::continuous&&i.design.panel.controls.size()<8){const auto key=d.id.substring(prefix.length());if(n<4&&!rackParameterActive(NativeModule::analog,key))continue;i.design.panel.controls.push_back({juce::String::repeatedString("L",64),0,std::uint8_t(i.design.panel.controls.size()%4),key,FocusWidget::rotary,FocusResponse::linear,d.minimum,d.maximum});}CHECK(i.design.panel.controls.size()==8);instances->instances.push_back(i);}
    for(std::uint8_t n=0;n<4;++n){auto& unit=instances->instances[n].design.units[0];const auto prefix="synth."+juce::String(n+1)+".";for(const auto& descriptor:parameterRegistry().first(drumParameterStart))if(descriptor.id.startsWith(prefix)){const auto key=descriptor.id.substring(prefix.length());unit.defaults.push_back({0,key,r.source.sound.values[*rackParameterIndex(NativeModule::analog,n,key)]});}CHECK(unit.defaults.size()==149);}
    for(std::uint8_t n=0;n<8;++n){CHECK(editRackLayout(r.source.sound.layout,{RackEditKind::replaceEffect,n,masterChain,n,EffectKind::convolution,true},r.source.sound.layout)==RackEditResult::applied);auto& unit=instances->instances[4+n].design.units[0];unit.module=NativeModule::convolution;const auto prefix="effect."+juce::String(n+1)+".";for(std::size_t index=0;index<dynamicEqParameterStart;++index)if(parameterRegistry()[index].id.startsWith(prefix))unit.defaults.push_back({0,parameterRegistry()[index].id.substring(prefix.length()),r.source.sound.values[index]});CHECK(unit.defaults.size()==82);}
    r.source.sound.personalities=instances;CHECK(validateProjectDocument(r.source));
    r.policy.scope=RequestScope::both;r.capturedPhrase=phrase();r.capturedPhrase->noteCount=256;for(unsigned n=0;n<256;++n)r.capturedPhrase->notes[n]={double(n)/256,.01,60,90,1};r.source.pattern=*r.capturedPhrase;r.capturedBpm=400;
    GroupIntent intent;intent.group=juce::Uuid();intent.member=juce::Uuid();for(auto* text:{&intent.name,&intent.role,&intent.key,&intent.scale})CHECK(setGroupText(std::string(128,'"'),*text));intent.bpm=400;r.source.group=intent;
    juce::MemoryBlock wire;CHECK(encodeRequest(r,wire));if(wire.getSize()<56)return;CHECK(40+4+8+4+256+4+8192+3*(4+128)+4+maximumProjectStateBytes+1+5368+1+8<=850316);
    std::size_t projectOffset=52;for(unsigned n=0;n<5;++n)projectOffset+=4+read32(wire,projectOffset);juce::MemoryBlock canonical;CHECK(encodeProjectState(r.source,canonical));CHECK(read32(wire,projectOffset)==canonical.getSize());
    // Retain the legacy Sound30/Project29 maximum v1 personality envelope.
    juce::MemoryBlock sound;CHECK(encodeSoundState(r.source.sound,sound));juce::MemoryBlock padded;juce::MemoryOutputStream out(padded,false);out.write(sound.getData(),currentPersonalityLengthOffset);out.writeInt(4+12*(16+1+1+2+4+65536));out.writeShort(1);out.writeByte(12);out.writeByte(0);
    for(const auto& i:instances->instances){juce::MemoryBlock design;CHECK(encodeRackDesign(i.design,design,RackValidationMode::instanceRecall));out.write(i.id.getRawData(),16);out.writeByte(0);out.writeByte(1);out.writeByte(0);out.writeByte(i.bindings[0].slot);out.writeInt(65536);out.write(design.getData(),design.getSize());const std::string padding(65536-design.getSize(),' ');out.write(padding.data(),padding.size());}out.writeInt(0);out.write(juce::MemoryBlock(32,true).getData(),32);out.flush();u32(padded,4,30);resign(padded);CHECK(padded.getSize()==oldSound30MaximumBytes-192-maximumSoundNameBytes);
    auto largeProject=replaceBlock(canonical,20,sound.getSize(),padded);u32(largeProject,12,std::uint32_t(padded.getSize()));byte(largeProject,4,29);resign(largeProject);CHECK(largeProject.getSize()==maximumProjectStateBytes-12*maximumRackPackageRecordBytes-192-maximumSoundNameBytes);ProjectDocument decoded;CHECK(decodeProjectState(largeProject.getData(),largeProject.getSize(),decoded)&&decoded==r.source);
    auto largeRequest=replaceBlock(wire,projectOffset+4,canonical.getSize(),largeProject);u32(largeRequest,projectOffset,std::uint32_t(largeProject.getSize()));GenerationRequest read;CHECK(decodeRequest(bytes(largeRequest),read));CHECK(read.source==r.source&&read.structuralGeneration==UINT64_MAX);CHECK(requestEqual(read,r));CHECK(largeRequest.getSize()<=850316);
    auto groupOwner=std::make_unique<GroupGenerationCommand>();auto& group=*groupOwner;group.request=r;group.selection.loadRevision=1;group.selection.peerCount=4;for(auto& member:group.selection.members)member=juce::Uuid();group.parentProposal=r.tag;--group.parentProposal->requestId;juce::MemoryBlock grouped;CHECK(encodeGroupGeneration(group,grouped));auto readGroupOwner=std::make_unique<GroupGenerationCommand>();auto& readGroup=*readGroupOwner;CHECK(decodeGroupGeneration(bytes(grouped),readGroup)&&readGroup.request.source==r.source);CHECK(grouped.getSize()<=839203);
    auto reject=[&](juce::MemoryBlock bad){GenerationRequest value=request(),before=value;CHECK(!decodeRequest(bytes(bad),value));CHECK(requestEqual(value,before));};for(auto cap:{std::uint32_t(maximumProjectStateBytes+1),UINT32_MAX}){auto bad=largeRequest;u32(bad,projectOffset,cap);reject(bad);}
    auto resultOwner=std::make_unique<GenerationResult>();auto& result=*resultOwner;result.tag=r.tag;result.status=GenerationStatus::ready;result.inferenceCalls=1;result.proposalStatus=ProposalStatus::accepted;result.renderStatus=RenderStatus::ready;result.providerStatus=rack::oauth::Status::connected;result.candidate=r.source;result.audio.bodyFrames=maximumPreviewBodyFrames;result.audio.left.resize(maximumPreviewFrames);result.audio.right.resize(maximumPreviewFrames);juce::MemoryBlock resultWire;CHECK(encodeResult(r,result,resultWire));CHECK(resultWire.getSize()<=5913482);auto decodedResultOwner=std::make_unique<GenerationResult>();auto& decodedResult=*decodedResultOwner;CHECK(decodeResult(bytes(resultWire),r,decodedResult)&&decodedResult.candidate==result.candidate&&decodedResult.audio.left.size()==576000);

    // All1055 distinct ordinary edits are actually admitted; appended values cannot disappear at the wire seam.
    auto allEditsOwner=std::make_unique<GenerationResult>(result);auto& allEdits=*allEditsOwner;for(std::size_t index=0;index<parameterCount;++index)allEdits.edits.parameters.push_back({parameterRegistry()[index].id,r.source.sound.values[index]});
    ProjectDocument allAdmitted;CHECK(admitProposal(r.source,r.policy,allEdits.edits,allAdmitted)==ProposalStatus::accepted);allEdits.candidate=allAdmitted;
    juce::MemoryBlock allWire;CHECK(encodeResult(r,allEdits,allWire));auto allReadOwner=std::make_unique<GenerationResult>();auto& allRead=*allReadOwner;CHECK(decodeResult(bytes(allWire),r,allRead)&&resultEqual(allRead,allEdits));CHECK(allRead.edits.parameters.size()==4519);
    const auto* first=static_cast<const unsigned char*>(allWire.getData());const auto* projectStart=static_cast<const unsigned char*>(canonical.getData());const auto* found=std::search(first,first+allWire.getSize(),projectStart,projectStart+canonical.getSize());CHECK(found!=first+allWire.getSize());
    if(found!=first+allWire.getSize()){const auto offset=std::size_t(found-first);auto full=replaceBlock(allWire,offset,canonical.getSize(),largeProject);u32(full,offset-4,std::uint32_t(largeProject.getSize()));auto readAllOwner=std::make_unique<GenerationResult>();auto& readAll=*readAllOwner;CHECK(full.getSize()<=5913482&&decodeResult(bytes(full),r,readAll)&&resultEqual(readAll,allEdits));}
    auto overEditsOwner=std::make_unique<GenerationResult>(allEdits);auto& overEdits=*overEditsOwner;overEdits.edits.parameters.push_back(allEdits.edits.parameters.back());auto unchangedEdits=sentinel();CHECK(!encodeResult(r,overEdits,unchangedEdits)&&unchangedEdits==sentinel());
    GroupPublication publication;publication.runtime=r.tag.runtime;publication.loadRevision=1;publication.editRevision=r.tag.editRevision;publication.project=r.source;GroupCommand command{1,GroupAction::publish,publication.runtime,1,publication,{}};juce::MemoryBlock published;CHECK(encodeGroupCommand(command,published));GroupCommand readPublication;CHECK(decodeGroupCommand(bytes(published),readPublication)&&readPublication.publication->project==r.source);CHECK(readPublication.publication->project.sound.personalities->instances.size()==12);constexpr std::size_t publicationProjectOffset=66;auto fullPublication=replaceBlock(published,publicationProjectOffset+4,read32(published,publicationProjectOffset),largeProject);u32(fullPublication,publicationProjectOffset,std::uint32_t(largeProject.getSize()));GroupCommand fullCommand;CHECK(decodeGroupCommand(bytes(fullPublication),fullCommand)&&fullCommand.publication->project==r.source);CHECK(fullPublication.getSize()<=1048576);std::cout<<"v5 full publication bytes="<<fullPublication.getSize()<<"\n";
    std::cout<<"v5 actual request="<<largeRequest.getSize()<<" project="<<largeProject.getSize()<<" result="<<resultWire.getSize()<<" instances=12 peers=4 preview=576000 original-projects-on-wire=1\n";
}


void reconciliation_oracles()
{
    begin(__func__);FocusedControl control{"Original",0,0,"analog.filter.resonance",FocusWidget::rotary,FocusResponse::linear,0.f,1.f};
    ProposalEdits e;e.reconciliation.controls={{juce::Uuid(),0,control}};auto next=e;next.reconciliation.controls[0].replacement->label="Changed";CHECK(!editsEqual(e,next));
    next=e;next.reconciliation.controls[0].replacement->maximum=.5f;CHECK(!editsEqual(e,next));next=e;next.reconciliation.controls[0].replacement->minimum=-0.f;CHECK(!editsEqual(e,next));
}
void modern_reverb_preview_and_wire_roundtrip()
{
    begin(__func__); // Real native preview and wire codecs; no provider or IPC fixture.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::reverb},source.source.sound.layout)==RackEditResult::applied);
        CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,1,masterChain,1,EffectKind::drive},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        source.source.sound.values[index("effect.1.reverb.mix")]=.27f;
        source.source.sound.values[index("effect.1.reverb.decay_seconds")]=.025f;
        source.source.sound.values[index("effect.8.reverb.mix")]=.73f;
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842d604");
        instance.design.id=juce::Uuid("9138771513d34c57a47912251842d605");instance.design.revision=11;
        instance.design.name="Modern wire";instance.design.role=RackRole::fx;instance.design.panel.sections={"Space"};
        instance.design.units={{0,NativeModule::reverb,{{0,"reverb.mix",.27f},{0,"reverb.decay_seconds",.025f}},2}};
        instance.design.panel.controls={{"Mix",0,0,"reverb.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        instance.bindings={{0,0}};
        if(embedded){auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.source.sound.personalities=state;}
        CHECK(validateProjectDocument(source.source));
        juce::MemoryBlock opaque;CHECK(encodeSoundState(source.source.sound,opaque));
        ProjectDocument opaqueProject;CHECK(decodeProjectState(opaque.getData(),opaque.getSize(),opaqueProject));CHECK(opaqueProject==source.source);
        juce::MemoryBlock requestWire;const bool requestEncoded=encodeRequest(source,requestWire);CHECK(requestEncoded);if(!requestEncoded)continue;GenerationRequest read=request();
        CHECK(decodeRequest(bytes(requestWire),read));CHECK(requestEqual(read,source));
        for(std::size_t slot=0;slot<8;++slot)CHECK((read.source.sound.compatibility.effects[slot]==std::array<AlgorithmRevision,effectBankCount>{1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,legacyAlgorithmRevision}));
        juce::MemoryBlock project;const bool projectEncoded=encodeProjectState(source.source,project);CHECK(projectEncoded);if(!projectEncoded)continue;
        auto projectLength=std::size_t(52);for(int text=0;text<5;++text)projectLength=skipAsset(requestWire,projectLength);
        CHECK(at32(requestWire,projectLength)==project.getSize());
        const auto corruptedProject=[&](const juce::MemoryBlock& original,std::size_t revisionOffset,unsigned char revision){
            auto changed=original;const auto soundSize=at32(changed,12);
            juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,soundSize);
            byte(sound,revisionOffset,revision);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;
        };
        for(const auto mutation:std::array<std::pair<std::size_t,unsigned char>,5>{{{currentSoundNumericBytes+20+7*effectBankCount,3},{currentSoundNumericBytes+21+7*effectBankCount,3},{currentSoundNumericBytes+23+7*effectBankCount,3},{currentSoundNumericBytes+22+7*effectBankCount,3},{currentSoundNumericBytes+19,3}}}){
            const auto invalidProject=corruptedProject(project,mutation.first,mutation.second);
            const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),invalidProject);
            auto output=request();output.instruction="retained";const auto before=output;
            CHECK(!decodeRequest(bytes(bad),output));CHECK(requestEqual(output,before));
        }
        auto modernDrive=source;modernDrive.source.sound.compatibility.effects[1][0]=2;juce::MemoryBlock driveWire;
        CHECK(encodeRequest(modernDrive,driveWire));GenerationRequest driveRead=request();CHECK(decodeRequest(bytes(driveWire),driveRead)&&requestEqual(driveRead,modernDrive));
        auto badSource=source;badSource.source.sound.compatibility.effects[7][0]=3;
        auto unchanged=sentinel();CHECK(!encodeRequest(badSource,unchanged)&&unchanged==sentinel());
        if(embedded){
            const auto invalidProject=corruptedProject(project,currentSoundNumericBytes+22,1);
            const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),invalidProject);auto output=read;
            CHECK(!decodeRequest(bytes(bad),output));CHECK(requestEqual(output,read));
        }
        ProposalEdits edits;edits.parameters={{"effect.1.reverb.mix",.41f}};ProjectDocument candidate;
        CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.compatibility==source.source.sound.compatibility&&candidate.sound.values[index("effect.1.reverb.mix")]==.41f);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto mode=quality==0?RenderQuality::realtime:RenderQuality::higher;
        const auto rendered=renderPreview(candidate,context,{},128,mode);
        CHECK(rendered.status==RenderStatus::ready&&rendered.quality==mode&&rendered.latencySamples==(quality==0?0:21));
        CHECK(rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);
        if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;
        ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;GenerationResult result;
        const bool resultDecoded=decodeResult(bytes(resultWire),source,result);CHECK(resultDecoded);CHECK(resultEqual(result,ready));if(!resultDecoded||!result.candidate)continue;
        CHECK(result.candidate->sound.compatibility==source.source.sound.compatibility);
        if(embedded){CHECK(result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==1);
            if(result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==1)CHECK(result.candidate->sound.personalities->instances[0].design==instance.design);}
        juce::MemoryBlock candidateProject;const bool candidateEncoded=encodeProjectState(candidate,candidateProject);CHECK(candidateEncoded);if(!candidateEncoded)continue;
        const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* nested=static_cast<const unsigned char*>(candidateProject.getData());
        const auto* found=std::search(first,first+resultWire.getSize(),nested,nested+candidateProject.getSize());
        CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){
            const auto offset=std::size_t(found-first);CHECK(offset>=4&&at32(resultWire,offset-4)==candidateProject.getSize());
            const auto invalidProject=corruptedProject(candidateProject,currentSoundNumericBytes+20+7*effectBankCount,3);
            const auto bad=replaceBlock(resultWire,offset,candidateProject.getSize(),invalidProject);auto output=ready;
            CHECK(!decodeResult(bytes(bad),source,output));CHECK(resultEqual(output,ready));
        }
        auto invalidReady=ready;invalidReady.candidate->sound.compatibility.effects[7][1]=3;unchanged=sentinel();
        CHECK(!encodeResult(source,invalidReady,unchanged)&&unchanged==sentinel());
        for(const auto kind:{MessageKind::generate,MessageKind::result}){
            const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock framed;
            const bool frameEncoded=encodeFrame({kind,source.tag.runtime,payload},framed);CHECK(frameEncoded);if(!frameEncoded)continue;CHECK(static_cast<const unsigned char*>(framed.getData())[4]==wireVersion);
            Frame decoded;CHECK(decodeFrame(bytes(framed),decoded));CHECK(decoded.kind==kind&&decoded.session==source.tag.runtime&&decoded.payload==payload);
        }
    }
}
void eq_request_result_and_readmission()
{
    begin(__func__); // Actual EQ admission, native preview, nested recall and unchanged Wire5 framing.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::eq},source.source.sound.layout)==RackEditResult::applied);
        CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,1,masterChain,1,EffectKind::drive},source.source.sound.layout)==RackEditResult::applied);
        CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,2,masterChain,2,EffectKind::eq},source.source.sound.layout)==RackEditResult::applied);
        for(auto& bank:source.source.sound.compatibility.effects)bank[2]=2;
        source.source.sound.values[759+4]=.61f;source.source.sound.values[830]=.42f;
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842a804");instance.design.id=juce::Uuid("9138771513d34c57a47912251842a805");
        instance.design.name="EQ wire original";instance.design.role=RackRole::fx;instance.design.panel.sections={"Contour"};
        instance.design.units={{0,NativeModule::eq,{{0,"eq.bell.gain_db",.5f},{0,"reverb.mix",.22f}},1}};instance.bindings={{0,0}};
        instance.design.panel.controls={{"Bell",0,0,"eq.bell.gain_db",FocusWidget::rotary,FocusResponse::linear,-18,18}};
        if(embedded){auto metadata=std::make_shared<RackPersonalityState>();metadata->instances={instance};source.source.sound.personalities=metadata;}
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const auto encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read));CHECK(requestEqual(read,source));
        CHECK(read.source.sound.values[830]==.42f&&read.source.sound.compatibility.effects[7][4]==1&&read.source.sound.compatibility.effects[7][2]==2);
        auto projectLength=std::size_t(52);for(int n=0;n<5;++n)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(requestWire,projectLength)==project.getSize());
        CHECK(at32(project,24)==currentSoundStateVersion&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(const auto slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+4,revision);
            auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;output.instruction="retained";const auto before=output;
            CHECK(!decodeRequest(bytes(bad),output));CHECK(requestEqual(output,before));
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.eq.bell.gain_db",.58f},{"effect.3.eq.high_shelf.q",.31f}};
        if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::eq,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::eq&&candidate.sound.values[index("effect.3.eq.high_shelf.q")]==.31f&&candidate.sound.compatibility.effects[2][4]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const auto resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded)CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances[0].design==instance.design);
        // Walk actual sized fields to the rack enum and reject an unknown next family without publication.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);
        std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto invalid=ready;invalid.candidate->sound.compatibility.effects[7][4]=3;auto keep=sentinel();CHECK(!encodeResult(source,invalid,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+24,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
}

juce::MemoryBlock capturedWireSound6()
{
    // Captured native831 numeric/bank bytes; no current-writer reconstruction.
    const juce::String encoded=
        "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"
        "HU8MEUjh+j50cETrUrgeP/yEUvgK1yM9vfpcgHsULj5UAtDpmpmZPtIIchv2KNw+f8NboylcDz92fEr716MwPwEAAAAA////AQAA"
        "BAAABAAABAAABAAABAAABAAABAAABP////////////////////////////////////////////////////8AAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA4"
        "AAAAAQAEAggFAAABAQEBAQEBAQEBAgEBAQECAQEBAQIBAQEBAgEBAQECAQEBAQIBAQEBAgEBAQECAQEAAAAAMMQHiAGoAlLNBHTG"
        "OY+DgrnyRUj19DXA+RaHZIUuZUU=";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto result=out.getMemoryBlock();
    CHECK(result.getSize()==7745&&juce::SHA256(result).toHexString()=="dbb4df56b15e393cdf7cbae07d2a43da8287476173673153e1bf7439b5976585");return result;
}
void filter_request_result_and_old_nested_authority()
{
    begin(__func__); // Actual admission/native preview/wire; no provider, IPC or listening claim.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        for(std::uint8_t slot=0;slot<3;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::filter},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        source.source.sound.values[832]=.44f;source.source.sound.values[837]=.49f;source.source.sound.values[870]=.42f;
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842a811");instance.design.id=juce::Uuid("9138771513d34c57a47912251842a812");
        instance.design.name="Filter wire originals";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
        instance.design.units={{0,NativeModule::filter,{{0,"filter_fx.mode",0},{0,"filter_fx.cutoff_hz",.4f},{0,"eq.bell.gain_db",.5f},{0,"reverb.mix",.2f}},1}};
        instance.bindings={{0,0}};instance.design.panel.controls={{"Cutoff",0,0,"filter_fx.cutoff_hz",FocusWidget::rotary,FocusResponse::logarithmic,20,20000}};
        if(embedded){
            auto second=instance;second.id=juce::Uuid("9138771513d34c57a47912251842a813");second.bindings={{0,1}};
            auto state=std::make_shared<RackPersonalityState>();state->instances={instance,second};source.source.sound.personalities=state;
        }
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const bool encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read)&&requestEqual(read,source));
        if(embedded)CHECK(read.source.sound.personalities&&read.source.sound.personalities->instances.size()==2&&read.source.sound.personalities->instances[0].id!=read.source.sound.personalities->instances[1].id&&read.source.sound.personalities->instances[0].design==read.source.sound.personalities->instances[1].design);
        auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(project,24)==currentSoundStateVersion&&at32(project,28)==4519&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        const auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+5,revision);const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;
            CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,source));
            auto invalid=source;invalid.source.sound.compatibility.effects[slot][5]=revision;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.filter_fx.cutoff_hz",.58f},{"effect.3.filter_fx.mix",.31f}};if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::filter,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::filter&&candidate.sound.values[index("effect.3.filter_fx.mix")]==.31f&&candidate.sound.compatibility.effects[2][5]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded){CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==2);if(result.candidate&&result.candidate->sound.personalities)CHECK(result.candidate->sound.personalities->instances==candidate.sound.personalities->instances);}
        // Walk genuine bounded fields to the actual rack-edit enum seam.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto mismatched=ready;mismatched.edits.parameters[1].normalized=.2f;auto keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+25+effectBankCount*7,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
    // Reader::project admits real old Sound6 both raw and inside explicit Project5.
    auto oldRequest=request();const auto raw=capturedWireSound6();const bool migrated=decodeSoundState(raw.getData(),raw.getSize(),oldRequest.source.sound);CHECK(migrated);if(!migrated)return;
    juce::MemoryBlock wire,project,sound;const bool requestEncoded=encodeRequest(oldRequest,wire),projectEncoded=encodeProjectState(oldRequest.source,project),soundEncoded=encodeSoundState(oldRequest.source.sound,sound);CHECK(requestEncoded&&projectEncoded&&soundEncoded);if(!requestEncoded||!projectEncoded||!soundEncoded)return;
    auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    auto oldProject=replaceBlock(project,20,sound.getSize(),raw);u32(oldProject,12,std::uint32_t(raw.getSize()));byte(oldProject,4,5);resign(oldProject);CHECK(oldProject.getSize()==7853);
    for(const auto& legacy:{raw,oldProject}){
        auto older=replaceBlock(wire,projectLength+4,project.getSize(),legacy);u32(older,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest read;
        CHECK(decodeRequest(bytes(older),read)&&requestEqual(read,oldRequest));for(const auto& banks:read.source.sound.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1);
        auto forged=raw;u32(forged,16,parameterRegistry()[831].stateKey);resign(forged);auto bad=replaceBlock(wire,projectLength+4,project.getSize(),forged);u32(bad,projectLength,std::uint32_t(forged.getSize()));auto output=oldRequest;
        CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,oldRequest));
    }
}

juce::MemoryBlock capturedWireSound7()
{
    // Immutable actual accepted native871 input, not a current-writer projection.
    const char* encoded=
        "QVJTSQcAAABnAwAAAAAAAFtMkjEAAAAAwh3lyrgeBT4vHOXJuB6FPpwa5cgUrsc+CRnlx7geBT92F+XGZmYmP+MV5cUpXI89UBTl"
        "xM3MTD7tK+XTw/WoPjiie5oAAAAAdwl20j0KFz/yhYvsAAAAADP2odUAAIA/lPYd4AAAAAD06IjNzczMPjUdWWMUrgc/GgK6/AAA"
        "AACveblyAAAAAPmT9Co9Clc+qJCnDXsUrj6VapRMAAAAAN7nbvkAAAAASs1MlwrXozy7xbclmpkZPlyl62MAAAAA2UZVLgAAAAD3"
        "ThLRcT0KP9ai7/AfhSs/Y38fPKuqKj/2ohIZrkdhPl3+nBozM7M+bnYrX4/C9T4eia3y9igcP8iUT9Krqio/MTFrjgrXIz4i284D"
        "4XqUPndXiHA9Ctc+p6PM883MDD9mBeeRexQuP01och/NzMw9eBICRR+Faz57257zAAAAADDyI0BI4fo+ImPAgVK4Hj8dFLWCCtcj"
        "PdkmJEV7FC4+XAlNeJqZmT7m8CTa9ijcPgnMxRopXA8/vcrFDtejMD8pFmaurkfhPdogILSrqio/61XQiqRwvT4E0PYBAAAAP9Uh"
        "O7muRyE/HWg7FM3MTD3xsg2Dq6oqP+yGz4lSuJ4+Qxdw565H4T5QVXyShesRPxCRBSEzMzM/FaDPFY/C9T16TqqPAACAPl0LCtZc"
        "j8I+aHXALgAAAABhf3+rCtcjP7kmfIyPwnU9brF4a1yPQj7aBt4JCtejPpf1+ddmZuY+1/v4qeF6FD9chmUDAAAAAPQO5Ni4HgU+"
        "iBlGibgehT4NavN7q6oqP7hk63i4HgU/bx3c2GZmJj98GxxeKVyPPQzoPoLNzEw+VjhamquqKj/3AZfaH4XrPsDijQM9Chc/QQpP"
        "DgrXIzyJKD9xKVwPPtAKGahxPYo+dwkD2c3MzD7OEvdhFK4HP+E9mbgAAAAA3hOyTArXoz2wdW87PQpXPoNVujt7FK4+k8lUhtej"
        "8D5yoV04mpkZP8xTBcwK16M8t5gq/5qZGT5vPgXyKVyPPltm4f6F69E+1L/VxauqKj/t17YmH4UrPx6SZp7sUbg9g6hT2q5HYT4T"
        "hnGrMzOzPt966sirqio/cueFkvYoHD8570Gdj8L1PPpy6ZIK1yM+itzzdeF6lD7/fsx5PQrXPpzouEDNzAw/g4qdD3sULj+WHhRe"
        "AAAAAI+7XTkfhWs+vxjGlOxRuD60ak3HSOH6PvwwFq5SuB4/nYsAkArXIz19/XTnexQuPqpI+EeamZk+nuyewvYo3D7ql5v7KVwP"
        "P3M+2P4AAAAA7rBls65H4T2HOJxBj8J1PhA6bFCkcL0+EXUuLQAAAD9CPq2RAAAAAOmERRfNzEw9iDKVFOxROD5/9hGaUriePswe"
        "gmyuR+E+KTnqpYXrET+Wgcr9MzMzP9EttEiPwvU9HBeMrAAAAAAVQi5bXI/CPiXho2FcjwI/YtU+xwrXIz+G8sLtj8J1PTvubVVc"
        "j0I+A0oNjArXoz4QCVgXZmbmPtD2+cvhehQ/FCZT7AAAAADSunAZAAAAANka6Si4HoU+2HTxDxSuxz6PDOj5uB4FPxwImw9mZiY/"
        "g5S4dQAAAABelYMQzcxMPpeXgfHD9ag+YPpIIB+F6z5hxx1/PQoXP/BVy10K1yM8l0qYhClcDz5uLkkDcT2KPgGeNe8AAAAAfjDj"
        "HBSuBz/Q/nPPw/UoP6MAClkK16M9syVtvz0KVz7SAcG6exSuPqwYh5TXo/A+lxCO9JqZGT9PTe7kCtejPDuambeamRk+7fjnaQAA"
        "AAAIN5MhhevRPvUcZJNxPQo/JuW3Xh+FKz/Lqqq37FG4PZB5jXoAAAAAn7bWazMzsz56aE2Xj8L1PqEU8G32KBw/YhocW4/C9TzH"
        "9epwCtcjPvQw+17hepQ+y+dpMT0K1z6+X5N9AAAAAGctD817FC4/55S7rc3MzD0M+Kz1H4VrPmRdJQ/sUbg+lUCE/kjh+j6l+E60"
        "UrgeP+J4nmcK1yM9BpYijnsULj4y/E+ympmZPhykDkcAAAAAK2gBjylcDz9WGXLo16MwP709BaWuR+E9TjRzco/CdT651QhTAAAA"
        "AJQ0gW0AAAA/EYXqs65HIT8C8CCPzcxMPddpB4XsUTg+RhvrzlK4nj6tcv4arkfhPlgO1u6F6xE/2/bdpgAAAACQFt3mj8L1PQKN"
        "Q9gAAIA+/StOAVyPwj65vPrvXI8CP3w7SSsK1yM/hkjljY/CdT2pbY/ZXI9CPl0oz8oK16M+yVyw+GZm5j4XKXh4AAAAABF9vlEA"
        "AAAAQHFVNwAAAAA+z4ZgAAAAAA3f3Y4AAAAAm/qiiQAAAACuV1/wAAAAAFA3KeEAAAAAZ8A8JgAAAAD4qq60AAAAADcZxCQfhes+"
        "k+esHT0KFz9WUo6xAAAAADuFO4YAAAAAvhlzB3E9ij5yxiC2zczMPspEGiIAAAAAV1gengAAAABSQ9sGCtejPb7nXic9Clc+SwP1"
        "CwAAAABU9q4oAAAAACthRTCamRk/D3GtRQrXozyd0yJ1AAAAAMIcbc4AAAAArStYX4Xr0T7dEwmOcT0KP8DGLfoAAAAAsZs92wAA"
        "AAAw2CAgrkdhPphwRmYzM7M+gFX3JAAAAABxeXyJAAAAAPCl2YCPwvU8WKNQvQrXIz5drF8nAAAAAILVB7sAAAAAbZU5aM3MDD+d"
        "8Mk9exQuP54r+C0AAAAA2NFjdwAAAABIMPWNAAAAAJeWX4cAAAAASXYaU1K4Hj9X9UfrAAAAAEfyWZMAAAAAe58qYgAAAAA8Og64"
        "AAAAALYbPEspXA8/7ELfSAAAAADCH0jNAAAAAFriqWcAAAAARZguHQAAAABzSpLFAAAAPyXjWu4AAAAAoXlfIQAAAADtFZwTAAAA"
        "ACq6a/AAAAAAaN/7+q5H4T5qNpbUAAAAADzMdUEAAAAAVCMggAAAAACbGHDuAAAAAG2nZRdcj8I+wxXJjAAAAAD75pS7AAAAAEfb"
        "lOYAAAAAIFzbhAAAAADqvOHmCtejPghligcAAAAAhjptmQAAAAAWGwpUAAAAACmmTMUAAAAAtzj/PrgehT5B7JzIAAAAAAUGEWUA"
        "AAAACXq0kwAAAAAOJHyrAAAAAPwNDRzNzEw+5rQFegAAAADgsa+uAAAAAGDwSc0AAAAAL18afQAAAABxjQEJKVwPPhQ8RuMAAAAA"
        "qo/Z0QAAAAByRFCIAAAAAF2GyS0AAAAAG297NQrXoz3fVkGtAAAAAK/57zcAAAAAUzxQpAAAAAAUSbZ6AAAAAP4h8RwK16M8piyM"
        "UgAAAACgJe6qAAAAACCvO7AAAAAA7zrywwAAAAAxufHrH4UrPwGo46MAAAAAxYdK7wAAAADJdXQNAAAAAM5LJgkAAAAAvOeBfPYo"
        "HD8wwl76AAAAAE6ks6sAAAAA7nBH7gAAAADBqmqMAAAAAB+0YfrNzAw/q60DSwAAAADD10qfAAAAAB+suQMAAAAAeNosZwAAAAAy"
        "pjK7SOH6PpL+/+AAAAAAJMuzRQAAAADsYkk3AAAAABOM1FUAAAAAlWlFzPYo3D7hwgtbAAAAACVXZE8AAAAAKe1SlwAAAAAugYIS"
        "AAAAABzX9MakcL0+dDvVbQAAAACKWHqJAAAAAFJVP1kAAAAAPevADwAAAAD7yh3tUriePj8SJfsAAAAAj3SZagAAAAAzWzoDAAAA"
        "APRi+nUAAAAA3kDbewAAgD7ybzAWAAAAAATxpU4AAAAAzOc+sgAAAADz34wLAAAAAHVEhO5cj0I+jaF4hQAAAACJLfUDAAAAAAXD"
        "eYsAAAAAwp0MgAAAAADwfSxsuB4FPhAO8vgAAAAArkBMNQAAAABOwiyRAAAAACHxBcIAAAAAf1D6gylcjz0LSbt9AAAAAKP9PKgA"
        "AAAA/w5kZQAAAABY4zE2AAAAABLMJMQK1yM8PldRKQAAAAD4RAJ7AAAAAGiSh6QAAAAAtz4Z1QAAAABpNGw9w/UoP5ls22EAAAAA"
        "3eoRqwAAAABxqev5AAAAANbXbegAAAAAdFI7npqZGT9NOdXjAAAAAEmbQzQAAAAAxSxEvgAAAACC1O4WAAAAALCuwkNxPQo/sujF"
        "igAAAADEV+zUAAAAAIykzt8AAAAAswaC5QAAAAA114wej8L1Pssjw0oAAAAAYwjEWgAAAAC/7T6sAAAAABgrU/kAAAAA0vj2jz0K"
        "1z7Qnrl9AAAAAG7WmAMAAAAADiXvGQAAAADh7XOeAAAAAD9sjSPsUbg+ofkFOwAAAADlr/jEAAAAAOl/W8cAAAAA7n3w7gAAAADc"
        "t/R7mpmZPsb0BuMAAAAAQBuhPAAAAADAHIMCAAAAAI8inlMAAAAA0WPxlo/CdT7/0whXAAAAAE/+vIcAAAAA8zEDNgAAAAC0LN4M"
        "AAAAAJ4XpK7sUTg+YOYGTAAAAAC+p9XQAAAAAF4+QdAAAAAA0W/voAAAAADvQ3Mdj8L1PZ3k6ugAAAAAWfrCnQAAAABVjlzjAAAA"
        "API2Fm8AAAAA4POoSI/CdT2CYNYaAAAAAFQTtLEAAAAAXLYGQQAAAABjrmI+AAAAAGXLiJUAAAAAjyC/5gAAAAAfRmpmAAAAAIMj"
        "QJAAAAAA5J7T6gAAAABOU//jZmYmP8Q8SRQAAAAAWuMqTwAAAAAiCXbQAAAAAG0VbqYAAAAAK7Nzqj0KFz/xZiq8AAAAADXJhawA"
        "AAAAea5QegAAAACedEoeAAAAAMwgK8wUrgc/VrmKdQAAAABQWxXMAAAAABBshWcAAAAAf63MhQAAAAABc4ot16PwPvNeSPIAAAAA"
        "6+lIAAAAAAD35qd7AAAAADC36xoAAAAAOiB9EYXr0T64jXsIAAAAADZv9IkAAAAAxpJ0tgAAAAB5i4TLAAAAAAfthPczM7M+mqyL"
        "YwAAAACsXWPQAAAAAISAuMMAAAAAa6MJpgAAAAD984sR4XqUPhUVZrMAAAAA0ThFkgAAAADdb/dBAAAAAHozjZwAAAAA+KL6xR+F"
        "az54r5ZoAAAAAPZiQIIAAAAAhi5SmQAAAAA5U0xFAAAAAMeVHQl7FC4+s6BzPAAAAACryS9jAAAAALd4gRsAAAAA8ExIfwAAAAD6"
        "kmV0rkfhPQ72vSEAAAAAyKA4TgAAAAA4pqiOAAAAAKdNam4AAAAA2cK8Ks3MTD1pyVNKAAAAAC2iL7IAAAAAAdbn3AAAAAAG56SM"
        "AAAAAKRBTsEzMzM/3E+9ygAAAAAyM1XhAAAAAMq+VmsAAAAAFZnvsAAAAADD4TPSCtcjP3tcxY4AAAAAkw1TuAAAAACvO2XMAAAA"
        "AGgIpUEAAAAAYidqeuF6FD9i8yzQAAAAALSAolwAAAAAvNhBBQAAAADDP8zlAAAAAMXtw1m4HgU/fXzkSAAAAAC527fyAAAAALUk"
        "nlEAAAAAUix7zgAAAABAlBjKH4XrPiQ/8BgAAAAAOi8qowAAAAAC67rCAAAAAE0Q5+AAAAAAC6OzKs3MzD5vBvBcAAAAAH9K3g4A"
        "AAAA4ye0OAAAAABEwz6SAAAAAK7A7Y57FK4+NpizQQAAAACwcrrPAAAAAHDZcxIAAAAA3yBvEQAAAABhzzmsKVyPPlHbcmsAAAAA"
        "Fa+OqgAAAABZpZikAAAAAH7xhSwAAAAArAY0yq5HYT6YMd6qAAAAAJYZm40AAAAAJpnajQAAAADZ+qJZAAAAAGeXK/sK1yM+0/Ge"
        "pwAAAABLDITEAAAAAFeRTn8AAAAAkHLcvwAAAACajWu8zczMPUPI/7wAAAAAe/ocugAAAADH7hzlAAAAAKBj/Z4AAAAAatBp5QrX"
        "Iz2IF8E3AAAAAAZO9ZcAAAAAli6SUgAAAACprW7fAAAAADdMhz3XozA/pZWRHgAAAAAhjecfAAAAAG0pJBIAAAAAqsGNCgAAAADo"
        "8oP5rkchP+rozAQAAAAAvN/9PwAAAADUNqh+AAAAABsgkggAAAAA7brtFYXrET/Xp34bAAAAAMcF4pEAAAAA+7KyYAAAAAC8QTDS"
        "AAAAADYvxElcjwI/bPUVeQAAAABCM9DLAAAAANr1MWYAAAAAxZ9QNwAAAADzXRrEZmbmPvl7gwIAAAAAvaY3eAAAAABR8NbiAAAA"
        "ALZS7sIAAAAAVCwohxSuxz7Yj8g6AAAAAAOYm+xmZiY/qAS+VgAAAABiU5c+zcxMPq/BjKfD9ag+fO1dpwAAAABDOQx6PQoXP4eE"
        "2Z8K1yM8bUD+TylcDz7WMv4UcT2KPq/WBBbNzMw+QBa/khSuBz+kmQJow/UoP6vAUkwK16M9PfKw5z0KVz6iqeYzexSuPklsD37X"
        "o/A+NNFVepqZGT+5F524CtejPAOSzY2amRk+UOhxVilcjz68chiOhevRPlQ9e9hxPQo/+9vEyQAAAACSFQSs7FG4PZknya0AAAAA"
        "k1IYYjMzsz7aVlGGj8L1PqOTWJsAAAAAUieGQo/C9Txqu50kCtcjPkqVdMjhepQ+VQo8nj0K1z42/NBqzcwMP7mlxqF7FC4/LUDj"
        "8s3MzD2EecFXH4VrPmJO0DrsUbg+2VVQJUjh+j5K9eKBUrgePznh9VYK1yM9ega7D3sULj5Ulyk9mpmZPqWB23P2KNw+s3TENylc"
        "Dz8vgCGB16MwP+ZssXMAAAAA7S+ZMY/CdT5+035hAAAAABy7LiMAAAA/ea5Te65HIT/OlhLsAAAAANHadRzsUTg+qYsGlFK4nj7D"
        "nIvVrkfhPjgHS4SF6xE/0Qs/rDMzMz8CgVETj8L1Pa5HowgAAIA+TQAhsFyPwj5zs8BKXI8CP4wCs7cK1yM/54ieqY/CdT2e3lnO"
        "XI9CPuti+uIK16M+pYVPP2Zm5j5esyyH4XoUP/o4UssAAAAArpjdlbgeBT7BeWykAAAAABRRB1QUrsc+XzVwJAAAAADd6UxSZmYm"
        "P0y9pdspXI89Ve2ooQAAAAA494wHw/WoPvwt/skfhes++NHB7j0KFz8fC+xMCtcjPFChtQopXA8+Y8G3pHE9ij5vv8zkzczMPv5+"
        "l5UUrgc/2J/RosP1KD/zaORfCtejPVgN33s9Clc+S9/nKXsUrj7spq3H16PwPt6tYHaamRk/c0v8gArXozxRAI4PmpkZPtm/fNgp"
        "XI8+nHrFHAAAAACvkIqucT0KP3zPA1cAAAAAlkzmM+xRuD2Dv8+erkdhPth7DLgAAAAAn807xI/C9T7z3nW79igcP3FeaNiPwvU8"
        "egXvHgrXIz6D+U2W4XqUPjQf0l89Ctc+WHXvbs3MDD9nIDpLexQuP2E8PtDNzMw99hdZ9h+Faz6F6pEm7FG4PnBS7GRI4fo+RSBX"
        "zlK4Hj/fGPt1CtcjPYRZCa57FC4+MJCmn5qZmT6IAujQ9ijcPi8nC0EAAAAAXkKwNdejMD+9K5nSAAAAAGejHBCPwnU+bqz8IqRw"
        "vT7v8hG1AAAAAH6c/teuRyE/BkLaUc3MTD0euB6Q7FE4PvldIohSuJ4+6homNK5H4T7NVai+hesRP0FHEw4zMzM/4HkF54/C9T12"
        "g/BcAACAPi3RgNxcj8I+Nr/LllyPAj+1mb6WCtcjP3YH4qSPwnU98M8vQVyPQj4J0qYDCtejPgeVNIBmZuY+QzOMI+F6FD9abvWZ"
        "AAAAAOn/usO4HgU+MrfILAAAAADQ8PE+FK7HPj2uNg+4HgU/2uF74QAAAAA9FcCpKVyPPYX0FKDNzEw+l9aCIMP1qD5cBohEH4Xr"
        "PvVbM4I9Chc/5nBmbQrXIzwisLx/KVwPPkmtIVVxPYo+x5O+Nc3MzD5wwBvQFK4HP7MGavnD9Sg/CqO+AgrXoz2Xw8CgPQpXPnEa"
        "reZ7FK4+wp9OW9ej8D7+yMQlmpkZP5LAi10K16M8dQdVUgAAAABA4zx9KVyPPsMz+xMAAAAAQfWK7XE9Cj8wtbNjH4UrP2HuBOsA"
        "AAAAlDn91a5HYT4oDSLEMzOzPjwMgFmPwvU+IwrLOPYoHD/kfYnDj8L1PBfMb8oK1yM+U1wpgOF6lD6aantfPQrXPoxmjLnNzAw/"
        "ZxbcC3sULj80zFFezczMPQfTIfgfhWs+mIDcI+xRuD5qK7HWSOH6PicRGsdSuB4/FV/5+ArXIz2tDwA7exQuPjjN8YSamZk+nZLh"
        "qAAAgD88bkE8AAAAAIM6VsrXozA/RN21AK5H4T05yCDGj8J1PhU4AlOkcL0+Ap+dOAAAAD8feHwerkchP188BnDNzEw9yK6KTOxR"
        "OD5NgcbNUriePq699pmuR+E+YyBe1IXrET+K4lxrMzMzP0oE37mPwvU9x8tHMwAAgD6O5ThrXI/CPnT74OhcjwI/ZZrG3wrXIz8M"
        "B2Ucj8J1PUWJLh5cj0I+Mo+FQQrXoz4P5eFqZmbmPhtXOWbhehQ/LBqnuwAAAABRuPfkuB4FPhEVhUS4HoU+hozMtBSuxz6zI4Qq"
        "uB4FPyBja+pmZiY/afkvlSlcjz3QaKcOzcxMPvgC9MvD9ag+8Z31xR+F6z4oih5YPQoXPz6eBs8K1yM883u7XClcDz4aICfGcT2K"
        "Ph9RB7LNzMw+iGCVMRSuBz8NohLpw/UoP2lGKD0K16M9jt+wXT0KVz6r7A2yexSuPpMmykTXo/A+FIhrsZqZGT/JD7ZJCtejPBqk"
        "XYOamRk+F6RMFSlcjz4eXUJyhevRPm7tjqBxPQo/I8+ZtB+FKz9K47DL7FG4PRjrp1GuR2E+kScJUzMzsz6IIkkJj8L1PvGGpZX2"
        "KBw/5ik724/C9TxT0bbpCtcjPp/lnsnhepQ+CLJqfT0K1z6N362yzcwMPzVUeth7FC4/IjfiRc3MzD3/Mp25H4VrPlwiubPsUbg+"
        "HU8MEUjh+j50cETrUrgeP/yEUvgK1yM9vfpcgHsULj5UAtDpmpmZPtIIchv2KNw+f8NboylcDz92fEr716MwPwd1638AAAAA9KEh"
        "Eo/CdT6Uz1ROpHC9Pm+DSBQAAAA/WlPl0a5HIT+wLX0SAAAAANU8WdjsUTg+5Xu2+1K4nj4MULDFrkfhPudnPLGF6xE/AevgfQAA"
        "AADe1AvIj8L1PW7VH20AAIA+VZSslVyPwj7EFztnXI8CP2p+yAAAAAAAZxhtg4/CdT3X5HrgXI9CPpqLYAcK16M+WbSBvWZm5j4T"
        "0NTEAAAAAAjfGxoAAAAAcJvjprgeBT6jUnK1uB6FPgYfwnYUrsc+zGR6EwAAAAAJhLRjZmYmP+E/kYspXI89QFOsns3MTD5TiCzW"
        "w/WoPl0Fq8AAAAAAUkInnD0KFz/6AUKqCtcjPOnwxwUpXA8+oOiVmnE9ij5Wt/P8AAAAAHs8gZsUrgc/QyhwCsP1KD/+9gUeCtej"
        "PdUCsXo9Clc+AQAAAAD///8BAAAEAAAEAAAEAAAEAAAEAAAEAAAEAAAE////////////////////////////////////////////"
        "/////////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAABAAAAAAQAEAggGAAABAQEB"
        "AQEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQEBAgEBAQAAAABlRIQxu1veK5/QAVqPm4+6jcQS"
        "k3WFRIvVjSdaFggn7g==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==8113&&juce::SHA256(wire).toHexString()=="a10734a80350ac97b6e4e1b070b629d21004a57dbf6c3e8b7252a4789380757a");return wire;
}
juce::MemoryBlock capturedWireProject6()
{
    // Captured native header/trailer plus the exact captured Sound7 bytes above.
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,"QUlQSgYAAAAAAAAAsR8AADgAAAA="));const auto sound=capturedWireSound7();out.write(sound.getData(),sound.getSize());CHECK(juce::Base64::convertFromBase64(out,"QUlQVAEAAAABBAQCAAAAAAAAAAAAAAAAiCr/0GkLwzzRJG7hFlHpNqneT4Z3LJQfg8h6NnmXl6CPD2gEGirM7QIw+2O/3hmHk/FJ9smRJJlZwUy+4IzJIw=="));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==8221&&juce::SHA256(wire).toHexString()=="313f4cf3225eabafa714c0ab780c504e2caf37f3c018e39e0e58a8f9789ddb5e");return wire;
}
void chorus_request_result_and_old_nested_authority()
{
    begin(__func__); // Actual admission/native preview/wire; no provider, IPC or listening claim.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        for(std::uint8_t slot=0;slot<3;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::chorus},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field)source.source.sound.values[871+5*slot+field]=.11f+.05f*float((slot+field)%8);
        source.source.sound.controlLocks[899]=true;source.source.sound.macros[0]={parameterRegistry()[892].stateKey,.17f};
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842c811");instance.design.id=juce::Uuid("9138771513d34c57a47912251842c812");
        instance.design.name="Chorus wire originals";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
        instance.design.units={{0,NativeModule::chorus,{}}};const auto defaults=defaultParameterValues();constexpr std::array<const char*,5> keys{"chorus.rate_hz","chorus.depth","chorus.base_delay_ms","chorus.feedback","chorus.mix"};for(std::size_t field=0;field<5;++field)instance.design.units[0].defaults.push_back({0,keys[field],defaults[871+field]});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
        instance.bindings={{0,0}};instance.design.panel.controls={{"Rate",0,0,"chorus.rate_hz",FocusWidget::rotary,FocusResponse::linear,.1f,5}};
        if(embedded){
            auto second=instance;second.id=juce::Uuid("9138771513d34c57a47912251842c813");second.bindings={{0,1}};
            auto state=std::make_shared<RackPersonalityState>();state->instances={instance,second};source.source.sound.personalities=state;
        }
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const bool encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read)&&requestEqual(read,source));
        if(embedded)CHECK(read.source.sound.personalities&&read.source.sound.personalities->instances.size()==2&&read.source.sound.personalities->instances[0].id!=read.source.sound.personalities->instances[1].id&&read.source.sound.personalities->instances[0].design==read.source.sound.personalities->instances[1].design);
        auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(project,24)==currentSoundStateVersion&&at32(project,28)==4519&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        const auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+6,revision);const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;
            CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,source));
            auto invalid=source;invalid.source.sound.compatibility.effects[slot][6]=revision;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.chorus.rate_hz",.58f},{"effect.3.chorus.mix",.31f}};if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::chorus,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::chorus&&candidate.sound.values[index("effect.3.chorus.mix")]==.31f&&candidate.sound.compatibility.effects[2][6]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded){CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==2);if(result.candidate&&result.candidate->sound.personalities)CHECK(result.candidate->sound.personalities->instances==candidate.sound.personalities->instances);}
        // Walk genuine bounded fields to the actual rack-edit enum seam.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto mismatched=ready;mismatched.edits.parameters[1].normalized=.2f;auto keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+26+effectBankCount*7,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
    auto oldRequest=request();const auto raw=capturedWireSound7(),oldProject=capturedWireProject6();const bool migrated=decodeProjectState(oldProject.getData(),oldProject.getSize(),oldRequest.source);CHECK(migrated);if(!migrated)return;
    juce::MemoryBlock wire,project;const bool requestEncoded=encodeRequest(oldRequest,wire),projectEncoded=encodeProjectState(oldRequest.source,project);CHECK(requestEncoded&&projectEncoded);if(!requestEncoded||!projectEncoded)return;
    auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    for(bool framed:{false,true}){const auto legacy=framed?oldProject:raw;auto older=replaceBlock(wire,projectLength+4,project.getSize(),legacy);u32(older,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest read;
        CHECK(decodeRequest(bytes(older),read)&&requestEqual(read,oldRequest));for(const auto& banks:read.source.sound.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1);
        auto forged=raw;u32(forged,16,parameterRegistry()[871].stateKey);resign(forged);auto badLegacy=framed?replaceBlock(oldProject,20,raw.getSize(),forged):forged;if(framed)resign(badLegacy);
        auto bad=replaceBlock(wire,projectLength+4,project.getSize(),badLegacy);u32(bad,projectLength,std::uint32_t(badLegacy.getSize()));auto output=oldRequest;CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,oldRequest));
    }
}

juce::MemoryBlock capturedWireSound8()
{
    // Exact native old Sound8 capture with nonuniform retained banks and locks.
    const juce::String encoded=
        "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"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAABIAAAAAQAEAggHAAABAQEBAQEBAQEBAgEBAQEBAQIB"
        "AQEBAQECAQEBAQEBAgEBAQEBAQIBAQEBAQECAQEBAQEBAgEBAQEBAQIBAQEBAAAAABeeYxRBjzSVNk+CloDHQ/D+ytzRc1wV+7n0"
        "aacrrgR5";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==8481&&juce::SHA256(wire).toHexString()=="863fdbf7b0df3d5479c642b97946f1a514edde985dc054ad098e2f85ff475584");return wire;
}
juce::MemoryBlock capturedWireProject7()
{
    // Native header/trailer around the exact captured old Sound8, never a reconstructed current stride.
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,"QUlQSgcAAAAAAAAAISEAADgAAAA="));const auto sound=capturedWireSound8();out.write(sound.getData(),sound.getSize());CHECK(juce::Base64::convertFromBase64(out,"QUlQVAEAAAABBAQCAAAAAAAAAAAAAAAAiCr/0GkLwzzRJG7hFlHpNqneT4Z3LJQfg8h6NnmXl6AEhOJ8XHVh3Z582Iqaiig0KmzuLQFW8GOzTVHH/MCmeA=="));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==8589&&juce::SHA256(wire).toHexString()=="dc0d65e82d72c1d5cb984d651cf0f21f7dbbaffce318c30e7a39bcad422ea291");return wire;
}
void phaser_request_result_and_old_nested_authority()
{
    begin(__func__); // Actual admission/native preview/wire; no provider, IPC or listening claim.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        for(std::uint8_t slot=0;slot<3;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::phaser},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field)source.source.sound.values[911+5*slot+field]=.11f+.05f*float((slot+field)%8);
        source.source.sound.controlLocks[939]=true;source.source.sound.macros[0]={parameterRegistry()[932].stateKey,.17f};
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842d911");instance.design.id=juce::Uuid("9138771513d34c57a47912251842d912");
        instance.design.name="Phaser wire originals";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
        instance.design.units={{0,NativeModule::phaser,{}}};const auto defaults=defaultParameterValues();constexpr std::array<const char*,5> keys{"phaser.rate_hz","phaser.depth","phaser.centre_hz","phaser.feedback","phaser.mix"};for(std::size_t field=0;field<5;++field)instance.design.units[0].defaults.push_back({0,keys[field],defaults[911+field]});instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
        instance.bindings={{0,0}};instance.design.panel.controls={{"Rate",0,0,"phaser.rate_hz",FocusWidget::rotary,FocusResponse::linear,.1f,5}};
        if(embedded){
            auto second=instance;second.id=juce::Uuid("9138771513d34c57a47912251842d913");second.bindings={{0,1}};
            auto state=std::make_shared<RackPersonalityState>();state->instances={instance,second};source.source.sound.personalities=state;
        }
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const bool encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read)&&requestEqual(read,source));
        if(embedded)CHECK(read.source.sound.personalities&&read.source.sound.personalities->instances.size()==2&&read.source.sound.personalities->instances[0].id!=read.source.sound.personalities->instances[1].id&&read.source.sound.personalities->instances[0].design==read.source.sound.personalities->instances[1].design);
        auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(project,24)==currentSoundStateVersion&&at32(project,28)==4519&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        const auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,2,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+7,revision);const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;
            CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,source));
            auto invalid=source;invalid.source.sound.compatibility.effects[slot][7]=revision;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.phaser.rate_hz",.58f},{"effect.3.phaser.mix",.31f}};if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::phaser,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::phaser&&candidate.sound.values[index("effect.3.phaser.mix")]==.31f&&candidate.sound.compatibility.effects[2][7]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded){CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==2);if(result.candidate&&result.candidate->sound.personalities)CHECK(result.candidate->sound.personalities->instances==candidate.sound.personalities->instances);}
        // Walk genuine bounded fields to the actual rack-edit enum seam.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto mismatched=ready;mismatched.edits.parameters[1].normalized=.2f;auto keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+27+effectBankCount*7,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
    auto oldRequest=request();const auto raw=capturedWireSound8(),oldProject=capturedWireProject7();const bool migrated=decodeProjectState(oldProject.getData(),oldProject.getSize(),oldRequest.source);CHECK(migrated);if(!migrated)return;
    juce::MemoryBlock wire,project;const bool requestEncoded=encodeRequest(oldRequest,wire),projectEncoded=encodeProjectState(oldRequest.source,project);CHECK(requestEncoded&&projectEncoded);if(!requestEncoded||!projectEncoded)return;
    auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    for(bool framed:{false,true}){const auto legacy=framed?oldProject:raw;auto older=replaceBlock(wire,projectLength+4,project.getSize(),legacy);u32(older,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest read;
        CHECK(decodeRequest(bytes(older),read)&&requestEqual(read,oldRequest));for(const auto& banks:read.source.sound.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1);
        auto forged=raw;u32(forged,16,parameterRegistry()[911].stateKey);resign(forged);auto badLegacy=framed?replaceBlock(oldProject,20,raw.getSize(),forged):forged;if(framed)resign(badLegacy);
        auto bad=replaceBlock(wire,projectLength+4,project.getSize(),badLegacy);u32(bad,projectLength,std::uint32_t(badLegacy.getSize()));auto output=oldRequest;CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,oldRequest));
    }
}

juce::MemoryBlock capturedWireSound9()
{
    // Exact pre-append native Sound9 bytes, including inactive retained banks.
    constexpr const char* encoded=
        "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"
        "DBXlVtej8D4pqRz1mpkZP1zWbg0K16M8eLTTppqZGT44HlemKVyPPttWibCF69E+xDdttXE9Cj8v2RoMH4UrPwEAAAAA////AQAABAAABAAABAAA"
        "BAAABAAABAAABAAABP////////////////////////////////////////////////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABQAAAAAQAE"
        "AggIAAABAQEBAQEBAQEBAgEBAQEBAQECAQEBAQEBAQIBAQEBAQEBAgEBAQEBAQECAQEBAQEBAQIBAQEBAQEBAgEBAQEBAQECAQEBAQEAAAAA4vnT"
        "e+dtdzGAGuVA2MkztyIkoYrS44d6efp3g8GX6fE=";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();
    CHECK(wire.getSize()==8849&&juce::SHA256(wire).toHexString()=="8841e71897cc3ac43fd031095fe21b5a688976a0a300e10b5dede89934447ffb");return wire;
}
juce::MemoryBlock capturedWireProject8()
{
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,"QUlQSggAAAAAAAAAkSIAADgAAAA="));const auto sound=capturedWireSound9();out.write(sound.getData(),sound.getSize());CHECK(juce::Base64::convertFromBase64(out,"QUlQVAEAAAABBAQCAAAAAAAAAAAAAAAAiCr/0GkLwzzRJG7hFlHpNqneT4Z3LJQfg8h6NnmXl6C+VT3sbRLbK/4VPF31jvxwbxTq9MMAV2sRQOTz8IXfYw=="));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==8957&&juce::SHA256(wire).toHexString()=="95268db96749993b9fd275a35a487cf830b1d747449d89d84abe6dce895beb32");return wire;
}
void flanger_request_result_and_old_nested_authority()
{
    begin(__func__); // Actual admission/native preview/wire; no provider, IPC or listening claim.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        for(std::uint8_t slot=0;slot<3;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::flanger},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field)source.source.sound.values[951+5*slot+field]=.11f+.05f*float((slot+field)%8);
        source.source.sound.controlLocks[979]=true;source.source.sound.macros[0]={parameterRegistry()[972].stateKey,.17f};
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842d911");instance.design.id=juce::Uuid("9138771513d34c57a47912251842d912");
        instance.design.name="Flanger wire originals";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
        instance.design.units={{0,NativeModule::flanger,{}}};const auto defaults=defaultParameterValues();constexpr std::array<const char*,5> keys{"flanger.rate_hz","flanger.depth","flanger.base_delay_ms","flanger.feedback","flanger.mix"};for(std::size_t field=0;field<5;++field)instance.design.units[0].defaults.push_back({0,keys[field],defaults[951+field]});instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
        instance.bindings={{0,0}};instance.design.panel.controls={{"Rate",0,0,"flanger.rate_hz",FocusWidget::rotary,FocusResponse::linear,.05f,5}};
        if(embedded){
            auto second=instance;second.id=juce::Uuid("9138771513d34c57a47912251842d913");second.bindings={{0,1}};
            auto state=std::make_shared<RackPersonalityState>();state->instances={instance,second};source.source.sound.personalities=state;
        }
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const bool encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read)&&requestEqual(read,source));
        if(embedded)CHECK(read.source.sound.personalities&&read.source.sound.personalities->instances.size()==2&&read.source.sound.personalities->instances[0].id!=read.source.sound.personalities->instances[1].id&&read.source.sound.personalities->instances[0].design==read.source.sound.personalities->instances[1].design);
        auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(project,24)==currentSoundStateVersion&&at32(project,28)==4519&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        const auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+8,revision);const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;
            CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,source));
            auto invalid=source;invalid.source.sound.compatibility.effects[slot][8]=revision;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.flanger.rate_hz",.58f},{"effect.3.flanger.mix",.31f}};if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::flanger,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::flanger&&candidate.sound.values[index("effect.3.flanger.mix")]==.31f&&candidate.sound.compatibility.effects[2][8]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded){CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==2);if(result.candidate&&result.candidate->sound.personalities)CHECK(result.candidate->sound.personalities->instances==candidate.sound.personalities->instances);}
        // Walk genuine bounded fields to the actual rack-edit enum seam.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto mismatched=ready;mismatched.edits.parameters[1].normalized=.2f;auto keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+28+effectBankCount*7,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
    auto oldRequest=request();const auto raw=capturedWireSound9(),oldProject=capturedWireProject8();const bool migrated=decodeProjectState(oldProject.getData(),oldProject.getSize(),oldRequest.source);CHECK(migrated);if(!migrated)return;
    juce::MemoryBlock wire,project;const bool requestEncoded=encodeRequest(oldRequest,wire),projectEncoded=encodeProjectState(oldRequest.source,project);CHECK(requestEncoded&&projectEncoded);if(!requestEncoded||!projectEncoded)return;
    auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    for(bool framed:{false,true}){const auto legacy=framed?oldProject:raw;auto older=replaceBlock(wire,projectLength+4,project.getSize(),legacy);u32(older,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest read;
        CHECK(decodeRequest(bytes(older),read)&&requestEqual(read,oldRequest));for(const auto& banks:read.source.sound.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1&&banks[8]==1);
        auto forged=raw;u32(forged,16,parameterRegistry()[951].stateKey);resign(forged);auto badLegacy=framed?replaceBlock(oldProject,20,raw.getSize(),forged):forged;if(framed)resign(badLegacy);
        auto bad=replaceBlock(wire,projectLength+4,project.getSize(),badLegacy);u32(bad,projectLength,std::uint32_t(badLegacy.getSize()));auto output=oldRequest;CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,oldRequest));
    }
}

juce::MemoryBlock capturedWireSound10()
{
    const juce::String encoded=
        "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"
        "DBXlVtej8D4pqRz1mpkZP1zWbg0K16M8eLTTppqZGT44HlemKVyPPttWibCF69E+xDdttXE9Cj8v2RoMH4UrP+VCIC/sUbg9ReSRla5HYT5S7avQ"
        "MzOzPnepKDGPwvU+RkPyPvYoHD+A/em2j8L1PEDdoNcK1yM+20pm6+F6lD68K3pMPQrXPpe22rXNzAw/g6FhyHsULj+/81N+zczMPcAw3V8fhWs+"
        "XfQyo+xRuD4woTD1SOH6Pj45znFSuB4/musUZgrXIz1hchOYexQuPtIzAAuamZk+Mc2vafYo3D6pnzVOKVwPPwncI1jXozA/Jt7R4q5H4T07oYlK"
        "j8J1PsqLNcykcL0+VA7ACAAAAD/UYA2MrkchP09VUtLNzEw9IPdH+uxROD475U70UriePsfgUKKuR+E+Myzx04XrET+07PEgMzMzP4Hh6nWPwvU9"
        "BKJrhwAAgD4SF/NpXI/CPj6hMqVcjwI/RTM8+grXIz9W3Tsej8J1PVXDMZhcj0I+AQAAAAD///8BAAAEAAAEAAAEAAAEAAAEAAAEAAAEAAAE////"
        "/////////////////////////////////////////////////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAABYAAAAAQAEAggJAAABAQEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQEB"
        "AgEBAQEBAQEBAgEBAQEBAQEBAgEBAQEBAQAAAADRpihiyrX1U/ex/SU/mfpg4zyVFWi89rYbRNZU4Tywyw==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==9217&&juce::SHA256(wire).toHexString()=="785c4bb7d42bdaf55b16b6ccb3dd77c7fcbb2d03c67ab8090f7a719db52f757b");return wire;
}
juce::MemoryBlock capturedWireProject9()
{
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,"QUlQSgkAAAAAAAAAASQAADgAAAA="));const auto sound=capturedWireSound10();out.write(sound.getData(),sound.getSize());CHECK(juce::Base64::convertFromBase64(out,"QUlQVAEAAAABBAQCAAAAAAAAAAAAAAAAiCr/0GkLwzzRJG7hFlHpNqneT4Z3LJQfg8h6NnmXl6DFzO1uc5U9CIel7/NP1M3vjW/tNAePSMlTNummn/1HUA=="));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==9325&&juce::SHA256(wire).toHexString()=="46bae8d0a7f644d67bca218e896bee30a17357294ccc233b36ca2685b83d4d90");return wire;
}
void stereo_request_result_and_old_nested_authority()
{
    begin(__func__); // Actual admission/native preview/wire; no provider, IPC or listening claim.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        for(std::uint8_t slot=0;slot<3;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::stereo},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<4;++field)source.source.sound.values[991+4*slot+field]=field==3?float(slot%3)/2:.11f+.05f*float((slot+field)%8);
        source.source.sound.controlLocks[1012]=true;source.source.sound.macros[0]={parameterRegistry()[1008].stateKey,.17f};
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842f911");instance.design.id=juce::Uuid("9138771513d34c57a47912251842f912");
        instance.design.name="Stereo wire originals";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
        instance.design.units={{0,NativeModule::stereo,{}}};const auto defaults=defaultParameterValues();constexpr std::array<const char*,4> keys{"stereo.gain_db","stereo.pan","stereo.width","stereo.channel_mode"};for(std::size_t field=0;field<4;++field)instance.design.units[0].defaults.push_back({0,keys[field],defaults[991+field]});instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
        instance.bindings={{0,0}};instance.design.panel.controls={{"Rate",0,0,"stereo.gain_db",FocusWidget::rotary,FocusResponse::linear,-24,12}};
        if(embedded){
            auto second=instance;second.id=juce::Uuid("9138771513d34c57a47912251842f913");second.bindings={{0,1}};
            auto state=std::make_shared<RackPersonalityState>();state->instances={instance,second};source.source.sound.personalities=state;
        }
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const bool encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read)&&requestEqual(read,source));
        if(embedded)CHECK(read.source.sound.personalities&&read.source.sound.personalities->instances.size()==2&&read.source.sound.personalities->instances[0].id!=read.source.sound.personalities->instances[1].id&&read.source.sound.personalities->instances[0].design==read.source.sound.personalities->instances[1].design);
        auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(project,24)==currentSoundStateVersion&&at32(project,28)==4519&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        const auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,2,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+9,revision);const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;
            CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,source));
            auto invalid=source;invalid.source.sound.compatibility.effects[slot][9]=revision;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.stereo.gain_db",.58f},{"effect.3.stereo.width",.31f}};if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::stereo,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::stereo&&candidate.sound.values[index("effect.3.stereo.width")]==.31f&&candidate.sound.compatibility.effects[2][9]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded){CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==2);if(result.candidate&&result.candidate->sound.personalities)CHECK(result.candidate->sound.personalities->instances==candidate.sound.personalities->instances);}
        // Walk genuine bounded fields to the actual rack-edit enum seam.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto mismatched=ready;mismatched.edits.parameters[1].normalized=.2f;auto keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+29+effectBankCount*7,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
    auto oldRequest=request();const auto raw=capturedWireSound10(),oldProject=capturedWireProject9();const bool migrated=decodeProjectState(oldProject.getData(),oldProject.getSize(),oldRequest.source);CHECK(migrated);if(!migrated)return;
    juce::MemoryBlock wire,project;const bool requestEncoded=encodeRequest(oldRequest,wire),projectEncoded=encodeProjectState(oldRequest.source,project);CHECK(requestEncoded&&projectEncoded);if(!requestEncoded||!projectEncoded)return;
    auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    for(bool framed:{false,true}){const auto legacy=framed?oldProject:raw;auto older=replaceBlock(wire,projectLength+4,project.getSize(),legacy);u32(older,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest read;
        CHECK(decodeRequest(bytes(older),read)&&requestEqual(read,oldRequest));for(const auto& banks:read.source.sound.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1&&banks[8]==1&&banks[9]==1);
        auto forged=raw;u32(forged,16,parameterRegistry()[991].stateKey);resign(forged);auto badLegacy=framed?replaceBlock(oldProject,20,raw.getSize(),forged):forged;if(framed)resign(badLegacy);
        auto bad=replaceBlock(wire,projectLength+4,project.getSize(),badLegacy);u32(bad,projectLength,std::uint32_t(badLegacy.getSize()));auto output=oldRequest;CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,oldRequest));
    }
}

juce::MemoryBlock capturedDimensionWireSound11()
{
    // Immutable actual predecessor export, independent of the current writer.
    constexpr const char* encoded=
        "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"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABgAAAAAQAEAggKAAABAQEBAQEBAQEBAgEBAQEBAQEB"
        "AQIBAQEBAQEBAQECAQEBAQEBAQEBAgEBAQEBAQEBAQIBAQEBAQEBAQECAQEBAQEBAQEBAgEBAQEBAQEBAQIBAQEBAQEBAAAAAPwIvDli5DDtPaPN"
        "rVH3mFLFqaCWoipRFL96FrPfPn73";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==9513&&juce::SHA256(wire).toHexString()=="07e03ad18e85b60778eca8e954387a7110fcd6340097029cdeec719bd3da5a6c");return wire;
}

juce::MemoryBlock capturedDimensionWireProject10()
{
    // Immutable actual predecessor export, independent of the current writer.
    constexpr const char* encoded=
        "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"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAGAAAAABAAQCCAoA"
        "AAEBAQEBAQEBAQECAQEBAQEBAQEBAgEBAQEBAQEBAQIBAQEBAQEBAQECAQEBAQEBAQEBAgEBAQEBAQEBAQIBAQEBAQEBAQECAQEBAQEBAQEBAgEB"
        "AQEBAQEAAAAA/Ai8OWLkMO09o82tUfeYUsWpoJaiKlEUv3oWs98+fvdBSVBUAQAAAAEEBAIAAAAAAAAAAAAAAACIKv/QaQvDPNEkbuEWUek2qd5P"
        "hncslB+DyHo2eZeXoJNBfI/7UL9mkr8HLEHmR6EgNWw0L4ulcKAekOzGj0kK";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==9621&&juce::SHA256(wire).toHexString()=="7226fcee2e9cd7539de7d374adcd9ca490c957a2e5bb5f4c909d14c758033fd7");return wire;
}

void dimension_request_result_and_old_nested_authority()
{
    begin(__func__); // Actual admission/native preview/wire; no provider, IPC or listening claim.
    for(bool embedded:{false,true})for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);
        for(std::uint8_t slot=0;slot<3;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::dimension},source.source.sound.layout)==RackEditResult::applied);
        for(auto& banks:source.source.sound.compatibility.effects)banks[2]=2;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<4;++field)source.source.sound.values[1023+4*slot+field]=.11f+.05f*float((slot+field)%8);
        source.source.sound.controlLocks[1044]=true;source.source.sound.macros[0]={parameterRegistry()[1040].stateKey,.17f};
        RackInstance instance;instance.id=juce::Uuid("9138771513d34c57a47912251842f911");instance.design.id=juce::Uuid("9138771513d34c57a47912251842f912");
        instance.design.name="Stereo wire originals";instance.design.role=RackRole::fx;instance.design.panel.sections={"Tone"};
        instance.design.units={{0,NativeModule::dimension,{}}};const auto defaults=defaultParameterValues();constexpr std::array<const char*,4> keys{"dimension.depth","dimension.rate_hz","dimension.width","dimension.mix"};for(std::size_t field=0;field<4;++field)instance.design.units[0].defaults.push_back({0,keys[field],defaults[1023+field]});instance.design.units[0].defaults.push_back({0,"chorus.mix",.3f});instance.design.units[0].defaults.push_back({0,"filter_fx.mix",.4f});instance.design.units[0].defaults.push_back({0,"eq.bell.gain_db",.5f});instance.design.units[0].defaults.push_back({0,"reverb.mix",.2f});
        instance.bindings={{0,0}};instance.design.panel.controls={{"Rate",0,0,"dimension.depth",FocusWidget::rotary,FocusResponse::linear,0,1}};
        if(embedded){
            auto second=instance;second.id=juce::Uuid("9138771513d34c57a47912251842f913");second.bindings={{0,1}};
            auto state=std::make_shared<RackPersonalityState>();state->instances={instance,second};source.source.sound.personalities=state;
        }
        CHECK(validateProjectDocument(source.source));juce::MemoryBlock requestWire;const bool encoded=encodeRequest(source,requestWire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest read;CHECK(decodeRequest(bytes(requestWire),read)&&requestEqual(read,source));
        if(embedded)CHECK(read.source.sound.personalities&&read.source.sound.personalities->instances.size()==2&&read.source.sound.personalities->instances[0].id!=read.source.sound.personalities->instances[1].id&&read.source.sound.personalities->instances[0].design==read.source.sound.personalities->instances[1].design);
        auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(requestWire,projectLength);
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));CHECK(at32(project,24)==currentSoundStateVersion&&at32(project,28)==4519&&static_cast<const unsigned char*>(project.getData())[4]==currentProjectStateVersion);
        const auto corruptProject=[&](const juce::MemoryBlock& original,std::size_t offset,unsigned char value){auto changed=original;juce::MemoryBlock sound(static_cast<const char*>(changed.getData())+20,at32(changed,12));byte(sound,offset,value);resign(sound);std::memcpy(static_cast<char*>(changed.getData())+20,sound.getData(),sound.getSize());resign(changed);return changed;};
        for(std::size_t slot:{std::size_t(0),std::size_t(7)})for(unsigned char revision:{0,3,255}){
            const auto badProject=corruptProject(project,currentSoundNumericBytes+20+effectBankCount*slot+10,revision);const auto bad=replaceBlock(requestWire,projectLength+4,project.getSize(),badProject);auto output=source;
            CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,source));
            auto invalid=source;invalid.source.sound.compatibility.effects[slot][10]=revision;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());
        }
        // Effect 3 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.dimension.depth",.58f},{"effect.3.dimension.width",.31f}};if(embedded)edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::dimension,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        CHECK(candidate.sound.layout.effects[2].kind==EffectKind::dimension&&candidate.sound.values[index("effect.3.dimension.width")]==.31f&&candidate.sound.compatibility.effects[2][10]==1);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
        const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);
        CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0&&rendered.measurements.clippedSamples==0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;
        ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;const bool resultEncoded=encodeResult(source,ready,resultWire);CHECK(resultEncoded);if(!resultEncoded)continue;
        GenerationResult result;CHECK(decodeResult(bytes(resultWire),source,result)&&resultEqual(result,ready));
        if(embedded){CHECK(result.candidate&&result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.size()==2);if(result.candidate&&result.candidate->sound.personalities)CHECK(result.candidate->sound.personalities->instances==candidate.sound.personalities->instances);}
        // Walk genuine bounded fields to the actual rack-edit enum seam.
        const auto candidateOffset=std::size_t(116)+(ready.measurements.onsetSeconds?8:0);std::size_t cursor=skipAsset(resultWire,candidateOffset);const auto count=at32(resultWire,cursor);cursor+=4;
        CHECK(count==2);for(std::size_t n=0;n<count;++n)cursor=skipAsset(resultWire,cursor)+4;CHECK(at32(resultWire,cursor)==(embedded?1u:0u));
        auto bad=resultWire;auto output=ready;if(embedded){byte(bad,cursor+8,255);CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        auto mismatched=ready;mismatched.edits.parameters[1].normalized=.2f;auto keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        juce::MemoryBlock nested;CHECK(encodeProjectState(candidate,nested));const auto* first=static_cast<const unsigned char*>(resultWire.getData());const auto* p=static_cast<const unsigned char*>(nested.getData());const auto* found=std::search(first,first+resultWire.getSize(),p,p+nested.getSize());CHECK(found!=first+resultWire.getSize());
        if(found!=first+resultWire.getSize()){bad=replaceBlock(resultWire,std::size_t(found-first),nested.getSize(),corruptProject(nested,currentSoundNumericBytes+30+effectBankCount*7,2));output=ready;CHECK(!decodeResult(bytes(bad),source,output)&&resultEqual(output,ready));}
        for(const auto kind:{MessageKind::generate,MessageKind::result}){const auto& payload=kind==MessageKind::generate?requestWire:resultWire;juce::MemoryBlock frame;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));CHECK(static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);Frame framed;CHECK(decodeFrame(bytes(frame),framed)&&framed.payload==payload&&framed.kind==kind);}
    }
    auto oldRequest=request();const auto raw=capturedDimensionWireSound11(),oldProject=capturedDimensionWireProject10();const bool migrated=decodeProjectState(oldProject.getData(),oldProject.getSize(),oldRequest.source);CHECK(migrated);if(!migrated)return;
    juce::MemoryBlock wire,project;const bool requestEncoded=encodeRequest(oldRequest,wire),projectEncoded=encodeProjectState(oldRequest.source,project);CHECK(requestEncoded&&projectEncoded);if(!requestEncoded||!projectEncoded)return;
    auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    for(bool framed:{false,true}){const auto legacy=framed?oldProject:raw;auto older=replaceBlock(wire,projectLength+4,project.getSize(),legacy);u32(older,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest read;
        CHECK(decodeRequest(bytes(older),read)&&requestEqual(read,oldRequest));for(const auto& banks:read.source.sound.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1&&banks[8]==1&&banks[10]==1);
        auto forged=raw;u32(forged,16,parameterRegistry()[1023].stateKey);resign(forged);auto badLegacy=framed?replaceBlock(oldProject,20,raw.getSize(),forged):forged;if(framed)resign(badLegacy);
        auto bad=replaceBlock(wire,projectLength+4,project.getSize(),badLegacy);u32(bad,projectLength,std::uint32_t(badLegacy.getSize()));auto output=oldRequest;CHECK(!decodeRequest(bytes(bad),output)&&requestEqual(output,oldRequest));
    }
}

juce::MemoryBlock capturedFrequencyShiftWireSound12()
{
    constexpr const char* encoded=
        "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"
        "rsfqZ65HIT+/EG3CzcxMPQHl/M3sUTg+m9HssFK4nj7PpIfNrkfhPp6TkbyF6xE/LOWuRzMzMz8BAAAAAP///wEAAAQAAAQAAAQAAAQAAAQAAAQA"
        "AAQAAAT/////////////////////////////////////////////////////AAAAAAC8zYMBMzMzvzxevF/MzAy/JSrr1czMzL4jMCo7/v9/vpVU"
        "e8HIzMy99RDw59DMTD18DG1B0MxMPvbKzHE2M7M+AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAABoAAAAAQAEAggLAAABAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQIB"
        "AQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEAAAAAWZmE4+jH7i3pTqxlbSNKF8ROg2LUIHZIDJVPwnl+x2o="
        ;
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto result=out.getMemoryBlock();CHECK(result.getSize()==9809&&juce::SHA256(result).toHexString()=="6ca035de2b3a535ad417cffab3a0ad9aa7aa3b54bcb682ad3f695b734c758fc6");return result;
}
juce::MemoryBlock capturedFrequencyShiftWireProject11()
{
    constexpr const char* encoded=
        "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"
        "j8J1PjLGjPCkcL0+ilKaVwAAAD+ux+pnrkchP78QbcLNzEw9AeX8zexROD6b0eywUriePs+kh82uR+E+npORvIXrET8s5a5HMzMzPwEAAAAA////"
        "AQAABAAABAAABAAABAAABAAABAAABAAABP////////////////////////////////////////////////////8AAAAAALzNgwEzMzO/PF68X8zM"
        "DL8lKuvVzMzMviMwKjv+/3++lVR7wcjMzL31EPDn0MxMPXwMbUHQzEw+9srMcTYzsz4BAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAGgAAAABAAQCCAsAAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEB"
        "AQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQAAAABZmYTj6MfuLelOrGVtI0oXxE6DYtQgdkgMlU/CeX7H"
        "akFJUFQBAAAAAQQEAgAAAAAAAAAAAAAAAIgq/9BpC8M80SRu4RZR6Tap3k+GdyyUH4PIejZ5l5egFjrCyp0pvc6rTuB7uxWh9vCbmiLSL1yH9hkm"
        "2LlqLqE="
        ;
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto result=out.getMemoryBlock();CHECK(result.getSize()==9917&&juce::SHA256(result).toHexString()=="9aef91b1df651598e7df9b2e12f55afab9304945d791c6082f1821691569fcd0");return result;
}
void frequency_shift_request_result_and_old_authority()
{
    begin("frequency_shift_request_result_and_old_authority");
    for(int quality=0;quality<2;++quality){auto source=request();source.source.sound.values[758]=float(quality);for(std::uint8_t slot=0;slot<2;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::frequencyShift},source.source.sound.layout)==RackEditResult::applied);source.source.sound.values[1055]=.53f;source.source.sound.values[1056]=.67f;source.source.sound.values[1057]=.41f;
        RackInstance instance;instance.design.name="Wire shifter";instance.design.role=RackRole::fx;instance.design.panel.sections={"Translation"};instance.design.units={{0,NativeModule::frequencyShift,{{0,"frequency_shift.hz",.53f},{0,"frequency_shift.stereo_offset_hz",.67f},{0,"frequency_shift.mix",.41f}}}};instance.bindings={{0,0}};instance.design.panel.controls={{"Shift",0,0,"frequency_shift.hz",FocusWidget::rotary,FocusResponse::linear,-1000,1000}};auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.source.sound.personalities=state;
        juce::MemoryBlock wire;CHECK(encodeRequest(source,wire));GenerationRequest decoded;CHECK(decodeRequest(bytes(wire),decoded)&&requestEqual(decoded,source));
        // Effect 2 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.frequency_shift.hz",.55f},{"effect.2.frequency_shift.mix",.31f}};edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::frequencyShift,false}};ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;juce::MemoryBlock resultWire;CHECK(encodeResult(source,ready,resultWire));GenerationResult out;CHECK(decodeResult(bytes(resultWire),source,out)&&resultEqual(out,ready));
        auto bad=source;bad.source.sound.compatibility.effects[7][11]=2;auto keep=sentinel();CHECK(!encodeRequest(bad,keep)&&keep==sentinel());auto mismatched=ready;mismatched.edits.racks[0].effect=static_cast<EffectKind>(255);keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        for(auto kind:{MessageKind::generate,MessageKind::result}){juce::MemoryBlock frame;const auto& payload=kind==MessageKind::generate?wire:resultWire;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));Frame recalled;CHECK(decodeFrame(bytes(frame),recalled)&&recalled.payload==payload&&static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);}
    }
    auto oldRequest=request();const auto project=capturedFrequencyShiftWireProject11();CHECK(decodeProjectState(project.getData(),project.getSize(),oldRequest.source));juce::MemoryBlock wire,current;CHECK(encodeRequest(oldRequest,wire)&&encodeProjectState(oldRequest.source,current));auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    const auto raw=capturedFrequencyShiftWireSound12();for(bool framed:{false,true}){const auto& legacy=framed?project:raw;auto payload=replaceBlock(wire,projectLength+4,current.getSize(),legacy);u32(payload,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest out;CHECK(decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));for(const auto& bank:out.source.sound.compatibility.effects)CHECK(bank[11]==1);
        auto forged=legacy;if(framed){juce::MemoryBlock sound(static_cast<const char*>(legacy.getData())+20,9809);u32(sound,16,parameterRegistry()[1055].stateKey);resign(sound);std::memcpy(static_cast<char*>(forged.getData())+20,sound.getData(),sound.getSize());resign(forged);}else{u32(forged,16,parameterRegistry()[1055].stateKey);resign(forged);}payload=replaceBlock(wire,projectLength+4,current.getSize(),forged);u32(payload,projectLength,std::uint32_t(forged.getSize()));out=oldRequest;CHECK(!decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));}
}

juce::MemoryBlock capturedConvolutionWireSound13()
{
    constexpr const char* encoded=
        "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"
        "rsfqZ65HIT+/EG3CzcxMPQHl/M3sUTg+m9HssFK4nj7PpIfNrkfhPp6TkbyF6xE/LOWuRzMzMz8WyN7Vj8L1PVlreo8AAIA+FuXNNFyPwj7xmPkd"
        "XI8CP3oAETwK1yM/s59g/o/CdT103F/jXI9CPgMr4hwK16M+VFHjHGZm5j6/Etrf4XoUP/z2SJkAAAAASc1dBLgeBT5SkYd4uB6FPh2iHsYUrsc+"
        "cmWbqbgeBT99XPUwZmYmP07GhnYpXI89b4RF9s3MTD6Qbspjw/WoPtcaR3Mfhes+MBc3uj0KFz+7cfTwCtcjPHDCMLkpXA8+1U6dYXE9ij4BAAAA"
        "AP///wEAAAQAAAQAAAQAAAQAAAQAAAQAAAQAAAT/////////////////////////////////////////////////////AAAAAAAWyN7VMzMzv1lr"
        "eo/MzAy/FuXNNMzMzL7xmPkd/v9/vnoAETzIzMy9s59g/tDMTD103F/j0MxMPgMr4hw2M7M+AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAABwAAAAAQAEAggMAAABAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEB"
        "AQAAAACsDe6OuEf+ZYgD0x4pIvW+bQzZZnZRP4E3IKHQJoycNg==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==10033&&juce::SHA256(wire).toHexString()=="c881c7691dfebbf6f239aff48f8cad12210e0f85afd9ca07b592f56f34c698d9");return wire;
}
juce::MemoryBlock capturedConvolutionWireProject12()
{
    constexpr const char* encoded=
        "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"
        "j8J1PjLGjPCkcL0+ilKaVwAAAD+ux+pnrkchP78QbcLNzEw9AeX8zexROD6b0eywUriePs+kh82uR+E+npORvIXrET8s5a5HMzMzPxbI3tWPwvU9"
        "WWt6jwAAgD4W5c00XI/CPvGY+R1cjwI/egARPArXIz+zn2D+j8J1PXTcX+Ncj0I+AyviHArXoz5UUeMcZmbmPr8S2t/hehQ//PZImQAAAABJzV0E"
        "uB4FPlKRh3i4HoU+HaIexhSuxz5yZZupuB4FP31c9TBmZiY/TsaGdilcjz1vhEX2zcxMPpBuymPD9ag+1xpHcx+F6z4wFze6PQoXP7tx9PAK1yM8"
        "cMIwuSlcDz7VTp1hcT2KPgEAAAAA////AQAABAAABAAABAAABAAABAAABAAABAAABP//////////////////////////////////////////////"
        "//////8AAAAAABbI3tUzMzO/WWt6j8zMDL8W5c00zMzMvvGY+R3+/3++egARPMjMzL2zn2D+0MxMPXTcX+PQzEw+AyviHDYzsz4BAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAHAAAAABAAQCCAwA"
        "AAEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEB"
        "AQEBAQEBAQEBAQECAQEBAQEBAQEBAAAAAKwN7o64R/5liAPTHiki9b5tDNlmdlE/gTcgodAmjJw2QUlQVAEAAAABBAQCAAAAAAAAAAAAAAAAiCr/"
        "0GkLwzzRJG7hFlHpNqneT4Z3LJQfg8h6NnmXl6B1sY9770Ika3EXPSNfyjm6SJLS7Kxpc9IwIKdL558znA==";
    juce::MemoryOutputStream out;CHECK(juce::Base64::convertFromBase64(out,encoded));const auto wire=out.getMemoryBlock();CHECK(wire.getSize()==10141&&juce::SHA256(wire).toHexString()=="64d59272b26e168c74916a26e427c991bc5996f64fc111612e27197007eb8479");return wire;
}
void convolution_request_result_and_old_authority()
{
    begin("convolution_request_result_and_old_authority");
    for(int quality=0;quality<2;++quality){auto source=request();source.source.sound.values[758]=float(quality);for(std::uint8_t slot=0;slot<2;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::convolution},source.source.sound.layout)==RackEditResult::applied);source.source.sound.values[1080]=.53f;source.source.sound.values[1081]=.67f;source.source.sound.values[1082]=.41f;
        RackInstance instance;instance.design.name="Wire shifter";instance.design.role=RackRole::fx;instance.design.panel.sections={"Translation"};instance.design.units={{0,NativeModule::convolution,{{0,"convolution.predelay_ms",.53f},{0,"convolution.wet_trim_db",.67f},{0,"convolution.mix",.41f}}}};instance.bindings={{0,0}};instance.design.panel.controls={{"Shift",0,0,"convolution.predelay_ms",FocusWidget::rotary,FocusResponse::linear,0,250}};auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.source.sound.personalities=state;
        juce::MemoryBlock wire;CHECK(encodeRequest(source,wire));GenerationRequest decoded;CHECK(decodeRequest(bytes(wire),decoded)&&requestEqual(decoded,source));
        // Effect 2 is the user's bare bank; the AI moves only a placed rack.
        ProposalEdits edits;edits.parameters={{"effect.1.convolution.predelay_ms",.55f},{"effect.2.convolution.mix",.31f}};edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::convolution,false}};ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;juce::MemoryBlock resultWire;CHECK(encodeResult(source,ready,resultWire));GenerationResult out;CHECK(decodeResult(bytes(resultWire),source,out)&&resultEqual(out,ready));
        auto bad=source;bad.source.sound.compatibility.effects[7][12]=2;auto keep=sentinel();CHECK(!encodeRequest(bad,keep)&&keep==sentinel());auto mismatched=ready;mismatched.edits.racks[0].effect=static_cast<EffectKind>(255);keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        for(auto kind:{MessageKind::generate,MessageKind::result}){juce::MemoryBlock frame;const auto& payload=kind==MessageKind::generate?wire:resultWire;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));Frame recalled;CHECK(decodeFrame(bytes(frame),recalled)&&recalled.payload==payload&&static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);}
    }
    auto oldRequest=request();const auto project=capturedConvolutionWireProject12();CHECK(decodeProjectState(project.getData(),project.getSize(),oldRequest.source));juce::MemoryBlock wire,current;CHECK(encodeRequest(oldRequest,wire)&&encodeProjectState(oldRequest.source,current));auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    const auto raw=capturedConvolutionWireSound13();for(bool framed:{false,true}){const auto& legacy=framed?project:raw;auto payload=replaceBlock(wire,projectLength+4,current.getSize(),legacy);u32(payload,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest out;CHECK(decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));for(const auto& bank:out.source.sound.compatibility.effects)CHECK(bank[12]==1);
        auto forged=legacy;if(framed){juce::MemoryBlock sound(static_cast<const char*>(legacy.getData())+20,10033);u32(sound,16,parameterRegistry()[1080].stateKey);resign(sound);std::memcpy(static_cast<char*>(forged.getData())+20,sound.getData(),sound.getSize());resign(forged);}else{u32(forged,16,parameterRegistry()[1080].stateKey);resign(forged);}payload=replaceBlock(wire,projectLength+4,current.getSize(),forged);u32(payload,projectLength,std::uint32_t(forged.getSize()));out=oldRequest;CHECK(!decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));}
}

// Frozen accepted native bytes; this fixture never invokes the current writer.
juce::MemoryBlock capturedFrequencySplitWireSound14()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "DjLMzMy+93if5f7/f75WXOD8yMzMvcSezCPQzEw9rNO7q9DMTD5J8XU2NjOzPgEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAB4"
        "AAAAAQAEAggNAAABAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEB"
        "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAAAAAC82frDbOBYpC2AmOxhTb3mxpc4AumC7e7gRQ4cWjZw6"));
    const auto raw=out.getMemoryBlock();CHECK(raw.getSize()==10329&&juce::SHA256(raw).toHexString()=="b4dd78f22d27f3e9465ca1ed8ca5b9bdfd3c091e0598d4316c23fdf1a3ebfa72");return raw;
}
// Frozen accepted native bytes; this fixture never invokes the current writer.
juce::MemoryBlock capturedFrequencySplitWireProject13()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "AAA6KT+uMzMzv9eE0l3MzAy/EbAOMszMzL73eJ/l/v9/vlZc4PzIzMy9xJ7MI9DMTD2s07ur0MxMPknxdTY2M7M+AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAB"
        "AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
        "AAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA"
        "AAEAAAAAAAAAAAAAAAAAAAAAAHgAAAABAAQCCA0AAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEB"
        "AQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEAAAAALzZ+sNs4FikLYCY7GFNvebGlzgC6YLt7uBFDhxaNnDpB"
        "SVBUAQAAAAEEBAIAAAAAAAAAAAAAAACIKv/QaQvDPNEkbuEWUek2qd5PhncslB+DyHo2eZeXoAzxfTnmaxaG/0qdAdvpnZ5wbGZIvB9kxwgyFNIiZvf5"));
    const auto raw=out.getMemoryBlock();CHECK(raw.getSize()==10437&&juce::SHA256(raw).toHexString()=="84f73ff313862bf75ec8e601c1843a42fdb8a42e57c064d337e7c887658e35b0");return raw;
}

void frequency_split_request_result_and_old_authority()
{
    begin("frequency_split_request_result_and_old_authority");CHECK(wireVersion==9&&wireHeaderBytes==28&&maximumGenerationRequestBytes==917504&&maximumResultPayloadBytes==8388608);
    for(int quality:{0,1}){
        auto source=request();source.source.sound.values[758]=float(quality);for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::frequencySplit},source.source.sound.layout)==RackEditResult::applied);
        RackInstance instance;instance.design.name="Wire split";instance.design.role=RackRole::fx;instance.design.panel.sections={"Bands"};instance.design.units={{0,NativeModule::frequencySplit,{{0,"frequency_split.crossover_hz",.53f},{0,"frequency_split.low_tone",.67f},{0,"frequency_split.mix",.41f}}}};instance.bindings={{0,0}};instance.design.panel.controls={{"Crossover",0,0,"frequency_split.crossover_hz",FocusWidget::rotary,FocusResponse::logarithmic,20,20000}};
        auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.source.sound.personalities=state;
        juce::MemoryBlock wire;const bool encoded=encodeRequest(source,wire);CHECK(encoded);if(!encoded)continue;GenerationRequest decoded;CHECK(decodeRequest(bytes(wire),decoded)&&requestEqual(decoded,source));
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
        const auto soundSize=at32(project,12);juce::MemoryBlock damagedSound(static_cast<const char*>(project.getData())+20,soundSize);byte(damagedSound,11269+15*7+13,255);resign(damagedSound);auto damagedProject=project;std::memcpy(static_cast<char*>(damagedProject.getData())+20,damagedSound.getData(),damagedSound.getSize());resign(damagedProject);
        auto damagedRequest=replaceBlock(wire,projectLength+4,project.getSize(),damagedProject);auto unchanged=source;CHECK(!decodeRequest(bytes(damagedRequest),unchanged)&&requestEqual(unchanged,source));
        ProposalEdits edits;for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<8;++field){const auto i=1111+8*slot+field;edits.parameters.push_back({parameterRegistry()[i].id,(field==2||field==5)?float((slot+field)%2):.31f+.03f*float((slot+field)%8)});}CHECK(edits.parameters.size()==64);
        // Effects 2 to 8 are the user's bare banks; the AI moves only a placed rack.
        edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::frequencySplit,false}};
        ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;
        juce::MemoryBlock resultWire;CHECK(encodeResult(source,ready,resultWire));GenerationResult out;CHECK(decodeResult(bytes(resultWire),source,out)&&resultEqual(out,ready));
        auto invalid=source;invalid.source.sound.compatibility.effects[7][13]=255;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());auto mismatched=ready;mismatched.edits.racks[0].effect=static_cast<EffectKind>(255);keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        for(auto kind:{MessageKind::generate,MessageKind::result}){juce::MemoryBlock frame;const auto& payload=kind==MessageKind::generate?wire:resultWire;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));Frame recalled;CHECK(decodeFrame(bytes(frame),recalled)&&recalled.payload==payload&&static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);}
    }
    auto oldRequest=request();const auto project=capturedFrequencySplitWireProject13();CHECK(decodeProjectState(project.getData(),project.getSize(),oldRequest.source));juce::MemoryBlock wire,current;CHECK(encodeRequest(oldRequest,wire)&&encodeProjectState(oldRequest.source,current));auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);
    const auto raw=capturedFrequencySplitWireSound14();for(bool framed:{false,true}){const auto& legacy=framed?project:raw;auto payload=replaceBlock(wire,projectLength+4,current.getSize(),legacy);u32(payload,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest out;CHECK(decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));for(const auto& bank:out.source.sound.compatibility.effects)CHECK(bank[13]==1);
        auto forged=legacy;if(framed){juce::MemoryBlock sound(static_cast<const char*>(legacy.getData())+20,10329);u32(sound,16,parameterRegistry()[1111].stateKey);resign(sound);std::memcpy(static_cast<char*>(forged.getData())+20,sound.getData(),sound.getSize());resign(forged);}else{u32(forged,16,parameterRegistry()[1111].stateKey);resign(forged);}payload=replaceBlock(wire,projectLength+4,current.getSize(),forged);u32(payload,projectLength,std::uint32_t(forged.getSize()));out=oldRequest;CHECK(!decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));}
}

// Frozen accepted native bytes; this fixture never invokes the current writer.
juce::MemoryBlock capturedMidSideWireSound15()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAA"
        "AAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAA"
        "AAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAA"
        "AAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAA"
        "AAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAACAAAAAAQAEAggOAAABAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEAAAAAeikTU4Stlv2E"
        "6wa48EPugrKTjeEd6MeKnTCzcijgxxw="));
    const auto raw=out.getMemoryBlock();CHECK(raw.getSize()==10913&&juce::SHA256(raw).toHexString()=="76c73af81a289a1eb5f0706a549f8f08bc6ad78af54bf066c69c085f51fbf094");return raw;
}
// Frozen accepted native bytes; this fixture never invokes the current writer.
juce::MemoryBlock capturedMidSideWireProject14()
{
    juce::MemoryOutputStream out;
    CHECK(juce::Base64::convertFromBase64(out,
        "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"
        "AAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAA"
        "AAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
        "AAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA"
        "AAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEA"
        "AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
        "AAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAA"
        "AAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAA"
        "AQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
        "AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAIAAAAABAAQCCA4AAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQIBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQIBAQEB"
        "AQEBAQEBAQAAAAB6KRNThK2W/YTrBrjwQ+6CspON4R3ox4qdMLNyKODHHEFJUFQBAAAAAQQEAgAAAAAAAAAAAAAAAIgq/9BpC8M80SRu4RZR6Tap3k+GdyyU"
        "H4PIejZ5l5egGjRkZTO1H/JdkSbhN5nQ8+KjYbUOt9BXZA7a4U4YEu8="));
    const auto raw=out.getMemoryBlock();CHECK(raw.getSize()==11021&&juce::SHA256(raw).toHexString()=="1746796fa2ab4c8cfc4016ae6a779f87d68a81bd0d5a646d4dc632a802088bca");return raw;
}

void mid_side_request_result_and_old_authority()
{
    begin("mid_side_request_result_and_old_authority");CHECK(wireVersion==9&&wireHeaderBytes==28&&maximumGenerationRequestBytes==917504&&maximumResultPayloadBytes==8388608);
    for(int quality:{0,1}){auto source=request();source.source.sound.values[758]=float(quality);for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(source.source.sound.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::midSideSplit},source.source.sound.layout)==RackEditResult::applied);
        // A plain source with an explicit ordinary pan edit carries both branches.
        source.source.sound.values[index("synth.1.pan")]=.325f;
        RackInstance instance;instance.design.name="Wire field";instance.design.role=RackRole::fx;instance.design.panel.sections={"Field"};instance.design.units={{0,NativeModule::midSideSplit,{{0,"mid_side_split.mid_gain_db",.53f},{0,"mid_side_split.side_tone",.67f},{0,"mid_side_split.mix",.41f}}}};instance.bindings={{0,0}};instance.design.panel.controls={{"Mid gain",0,0,"mid_side_split.mid_gain_db",FocusWidget::rotary,FocusResponse::linear,-24,12}};auto state=std::make_shared<RackPersonalityState>();state->instances={instance};source.source.sound.personalities=state;
        juce::MemoryBlock wire;const bool encoded=encodeRequest(source,wire);CHECK(encoded);if(!encoded)continue;GenerationRequest decoded;CHECK(decodeRequest(bytes(wire),decoded)&&requestEqual(decoded,source));
        juce::MemoryBlock project;CHECK(encodeProjectState(source.source,project));auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);const auto soundSize=at32(project,12);juce::MemoryBlock damagedSound(static_cast<const char*>(project.getData())+20,soundSize);byte(damagedSound,currentSoundNumericBytes+20+effectBankCount*7+14,255);resign(damagedSound);auto damagedProject=project;std::memcpy(static_cast<char*>(damagedProject.getData())+20,damagedSound.getData(),damagedSound.getSize());resign(damagedProject);auto damagedRequest=replaceBlock(wire,projectLength+4,project.getSize(),damagedProject);auto unchanged=source;CHECK(!decodeRequest(bytes(damagedRequest),unchanged)&&requestEqual(unchanged,source));
        ProposalEdits edits;for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<7;++field){const auto i=1175+7*slot+field;edits.parameters.push_back({parameterRegistry()[i].id,(field==1||field==4)?0.f:.31f+.03f*float((slot+field)%8)});}CHECK(edits.parameters.size()==56);
        // Effects 2 to 8 are the user's bare banks; the AI moves only a placed rack.
        edits.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind::midSideSplit,false}};ProjectDocument candidate;CHECK(admitProposal(source.source,source.policy,edits,candidate)==ProposalStatus::accepted);
        AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};const auto rendered=renderPreview(candidate,context,{},128,quality==0?RenderQuality::realtime:RenderQuality::higher);CHECK(rendered.status==RenderStatus::ready&&rendered.measurements.finite&&rendered.measurements.peak>0);if(rendered.status!=RenderStatus::ready)continue;
        GenerationResult ready;ready.tag=source.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;ready.providerStatus=rack::oauth::Status::connected;ready.candidate=candidate;ready.edits=edits;ready.audio=rendered.audio;ready.measurements=rendered.measurements;juce::MemoryBlock resultWire;CHECK(encodeResult(source,ready,resultWire));GenerationResult out;CHECK(decodeResult(bytes(resultWire),source,out)&&resultEqual(out,ready));
        auto invalid=source;invalid.source.sound.compatibility.effects[7][14]=255;auto keep=sentinel();CHECK(!encodeRequest(invalid,keep)&&keep==sentinel());auto mismatched=ready;mismatched.edits.racks[0].effect=static_cast<EffectKind>(255);keep=sentinel();CHECK(!encodeResult(source,mismatched,keep)&&keep==sentinel());
        for(auto kind:{MessageKind::generate,MessageKind::result}){juce::MemoryBlock frame;const auto& payload=kind==MessageKind::generate?wire:resultWire;CHECK(encodeFrame({kind,source.tag.runtime,payload},frame));Frame recalled;CHECK(decodeFrame(bytes(frame),recalled)&&recalled.payload==payload&&static_cast<const unsigned char*>(frame.getData())[4]==wireVersion);}
    }
    auto oldRequest=request();const auto project=capturedMidSideWireProject14();CHECK(decodeProjectState(project.getData(),project.getSize(),oldRequest.source));juce::MemoryBlock wire,current;CHECK(encodeRequest(oldRequest,wire)&&encodeProjectState(oldRequest.source,current));auto projectLength=std::size_t(52);for(int field=0;field<5;++field)projectLength=skipAsset(wire,projectLength);const auto raw=capturedMidSideWireSound15();
    for(bool framed:{false,true}){const auto& legacy=framed?project:raw;auto payload=replaceBlock(wire,projectLength+4,current.getSize(),legacy);u32(payload,projectLength,std::uint32_t(legacy.getSize()));GenerationRequest out;CHECK(decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));for(const auto& bank:out.source.sound.compatibility.effects)CHECK(bank[14]==1);auto forged=legacy;if(framed){juce::MemoryBlock sound(static_cast<const char*>(legacy.getData())+20,10913);u32(sound,16,parameterRegistry()[1175].stateKey);resign(sound);std::memcpy(static_cast<char*>(forged.getData())+20,sound.getData(),sound.getSize());resign(forged);}else{u32(forged,16,parameterRegistry()[1175].stateKey);resign(forged);}payload=replaceBlock(wire,projectLength+4,current.getSize(),forged);u32(payload,projectLength,std::uint32_t(forged.getSize()));out=oldRequest;CHECK(!decodeRequest(bytes(payload),out)&&requestEqual(out,oldRequest));}
}

GenerationRequest catalogWireRequest(bool hydrated=false)
{
    auto r=request();
    r.policy.allowMacroChanges=true;r.source.sound.values[index("output.gain_db")]=.4f;
    const auto factory=airack::test::frozenFactoryRackDesigns();RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(factory,{},{},snapshot));
    RackCatalogPage page;CHECK(catalogPage(snapshot,{},0,65536,page));CHECK(page.complete&&page.entries.size()==21);
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=snapshot.id;
    const auto descriptor=rackCatalogPageDescriptor(page);
    static bool roundingObserved=false;
    if(!roundingObserved){
        const auto firstRoundedBound=[&](int precision){
            const auto read=juce::JSON::parse(juce::JSON::toString(descriptor,true,precision));
            for(const auto& entry:*read["entries"].getArray())for(const auto& unit:*entry["units"].getArray())
                for(const auto& parameter:*unit["parameters"].getArray()){
                    const auto& tuple=*parameter.getArray();
                    const auto i=rackParameterIndex(NativeModule(int(unit["module"])),0,tuple[0].toString());
                    CHECK(i);if(!i)return false;const auto& field=parameterRegistry()[*i];
                    for(int bound=2;bound<=3;++bound){const auto expected=double(bound==2?field.minimum:field.maximum);
                        const auto actual=double(tuple[bound]);if(actual==expected)continue;
                        std::cout<<"CATALOG_DESCRIPTOR_ROUNDING precision="<<precision<<" key="<<tuple[0].toString()
                            <<" bound="<<(bound==2?"minimum":"maximum")<<" parsed="<<std::hexfloat<<actual
                            <<" expected="<<expected<<std::defaultfloat<<'\n';return true;
                    }
                }
            return false;
        };
        CHECK(firstRoundedBound(15));CHECK(firstRoundedBound(17));roundingObserved=true;
    }
    authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
    CHECK(airack::ai::detail::catalogAuthorityValid(*authority));
    CatalogRequestContext context;context.root=r.tag;context.authority=authority;r.catalog=std::move(context);
    const auto selected=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[](const auto& e){return e.definition.name=="Lumen Drift";});
    CHECK(selected!=snapshot.entries.end());if(selected==snapshot.entries.end())return r;
    if(hydrated){CatalogDefinition d;d.design=selected->definition.id;d.revision=selected->definition.revision;d.contentSha256=selected->contentSha256;
        CHECK(encodeRackDesign(selected->definition,d.canonical,RackValidationMode::instanceRecall));authority->definitions.push_back(std::move(d));}
    return r;
}
ProposalEdits catalogWireIntent(const GenerationRequest& r)
{
    ProposalEdits edits;const auto descriptor=juce::JSON::parse(juce::String::fromUTF8(r.catalog->authority->descriptorJson.c_str()));
    const auto entryList=descriptor.getProperty("entries",{});const auto* entries=entryList.getArray();CHECK(entries!=nullptr);if(!entries)return edits;
    for(const auto& entry:*entries)if(entry.getProperty("name",{}).toString()=="Lumen Drift"){
        const auto design=juce::Uuid(entry.getProperty("id",{}).toString());const auto revision=std::uint64_t(entry.getProperty("revision",{}).toString().getLargeIntValue());
        const auto hash=entry.getProperty("content_sha256",{}).toString();
        edits.catalogSelections={{7,design,revision,hash,{RackRole::fx,masterChain,0}},{255,design,revision,hash,{RackRole::fx,masterChain,1}}};
        edits.catalogParameters={{7,0,"reverb.mix",.34f},{255,0,"reverb.mix",.17f}};
        edits.macros={{2,"catalog:7:0:reverb.mix",.25f}};edits.parameters={{"output.gain_db",.4f}};
        edits.characteristics="Two independent spaces";edits.explanation="Captured native identity, local mix and macro edits";break;}
    CHECK(edits.catalogSelections.size()==2);return edits;
}
GenerationResult catalogWireSelection(const GenerationRequest& r)
{
    GenerationResult value;value.tag=r.tag;value.status=GenerationStatus::catalogSelectionRequired;value.inferenceCalls=2;
    value.providerStatus=rack::oauth::Status::connected;value.httpStatus=200;value.proposalStatus=ProposalStatus::accepted;
    value.edits=catalogWireIntent(r);value.catalog=CatalogResultBinding{r.catalog->root,r.catalog->authority->snapshot};return value;
}
void saturation_result_and_base_intent_roundtrip()
{
    begin(__func__);
    for(const auto* name:{"Character Drive","Band Color"}){
        auto initial=std::make_unique<GenerationRequest>(catalogWireRequest());
        const auto descriptor=juce::JSON::parse(juce::String::fromUTF8(initial->catalog->authority->descriptorJson.c_str()));
        const auto* entries=descriptor["entries"].getArray();CHECK(entries);if(!entries)continue;
        const auto entry=std::find_if(entries->begin(),entries->end(),[&](const auto& row){return row["name"].toString()==name;});CHECK(entry!=entries->end());if(entry==entries->end())continue;
        auto selection=std::make_unique<GenerationResult>(catalogWireSelection(*initial));selection->edits={};
        selection->edits.catalogSelections={{7,juce::Uuid((*entry)["id"].toString()),std::uint64_t((*entry)["revision"].toString().getLargeIntValue()),(*entry)["content_sha256"].toString(),{RackRole::fx,masterChain,0}}};
        const auto key=juce::String(name)=="Character Drive"?"saturation.full.drive_db":"saturation.low.drive_db";
        selection->edits.catalogParameters={{7,0,key,.4f},{7,0,"saturation.mix",.7f}};
        juce::MemoryBlock wire;const bool resultEncoded=encodeResult(*initial,*selection,wire);CHECK(resultEncoded);
        auto resultRead=std::make_unique<GenerationResult>();if(resultEncoded)CHECK(decodeResult(bytes(wire),*initial,*resultRead)&&resultEqual(*selection,*resultRead));
        auto materialized=std::make_unique<GenerationRequest>(*initial);auto authority=std::make_shared<CatalogAuthority>(*initial->catalog->authority);
        const auto factories=airack::test::frozenFactoryRackDesigns();const auto design=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.name==name;});CHECK(design!=factories.end());if(design==factories.end())continue;
        CatalogDefinition definition;definition.design=design->id;definition.revision=design->revision;definition.contentSha256=(*entry)["content_sha256"].toString();
        CHECK(encodeRackDesign(*design,definition.canonical,RackValidationMode::instanceRecall));authority->definitions.push_back(std::move(definition));materialized->catalog->authority=authority;
        materialized->catalog->phase=CatalogPhase::materialize;materialized->catalog->selectionParent=initial->tag;materialized->catalog->inferenceCallsUsed=2;
        materialized->catalog->providerStatus=rack::oauth::Status::connected;materialized->catalog->httpStatus=200;materialized->catalog->baseIntent=selection->edits;++materialized->tag.requestId;
        const bool requestEncoded=encodeRequest(*materialized,wire);CHECK(requestEncoded);auto requestRead=std::make_unique<GenerationRequest>();
        if(requestEncoded)CHECK(decodeRequest(bytes(wire),*requestRead)&&requestEqual(*materialized,*requestRead));
        auto candidate=std::make_unique<ProjectDocument>();CHECK(admitProposal(materialized->source,materialized->policy,selection->edits,*authority,*candidate)==ProposalStatus::accepted);
        // Wire6 retains catalog placement; raw AI bank creation/replacement is outside the placed-rack contract.
        for(auto kind:{RackEditKind::addEffect,RackEditKind::replaceEffect}){ProposalEdits raw;raw.racks={{kind,7,masterChain,0,EffectKind::saturation,false}};
            auto unchanged=std::make_unique<ProjectDocument>(*candidate);CHECK(admitProposal(initial->source,initial->policy,raw,*unchanged)==ProposalStatus::outOfReach&&*unchanged==*candidate);}
        auto invalid=*selection;invalid.edits.catalogParameters[0].key+="x";auto output=sentinel();CHECK(!encodeResult(*initial,invalid,output)&&output==sentinel());
    }
}
void catalog_request_extension_and_atomic_bounds()
{
    begin(__func__);CHECK(wireVersion==9&&wireHeaderBytes==28&&maximumGenerationRequestBytes==917504&&maximumControlPayloadBytes==1048576&&maximumResultPayloadBytes==8388608);
    const auto initial=catalogWireRequest();auto legacy=initial;legacy.catalog.reset();juce::MemoryBlock oldWire,wire;
    CHECK(encodeRequest(legacy,oldWire));const bool encoded=encodeRequest(initial,wire);CHECK(encoded);if(!encoded)return;
    // Legacy bytes are an exact prefix; optional extension never changes Wire6 or source layout.
    CHECK(wire.getSize()>oldWire.getSize());if(wire.getSize()<=oldWire.getSize())return;
    CHECK(std::memcmp(wire.getData(),oldWire.getData(),oldWire.getSize())==0);
    const auto initialBytes=wire.getSize();const auto ext=oldWire.getSize();const auto* raw=static_cast<const unsigned char*>(wire.getData());CHECK(raw[ext]==1&&raw[ext+1]==0&&raw[ext+2]==0);
    CHECK(std::memcmp(raw+ext+3,initial.tag.runtime.getRawData(),16)==0);CHECK(raw[ext+43]==0);
    GenerationRequest read;CHECK(decodeRequest(bytes(wire),read)&&requestEqual(read,initial));
    CHECK(read.catalog&&read.catalog->authority&&read.catalog->authority->root==initial.tag);
    juce::MemoryBlock reencoded;CHECK(encodeRequest(read,reencoded)&&reencoded==wire);
    auto rejectWire=[&](const juce::MemoryBlock& bad){auto out=initial;out.instruction="sentinel";const auto before=out;CHECK(!decodeRequest(bytes(bad),out)&&requestEqual(out,before));};
    for(unsigned value:{0u,2u,255u}){auto bad=wire;byte(bad,ext,static_cast<unsigned char>(value));rejectWire(bad);}
    auto bad=wire;byte(bad,ext+2,2);rejectWire(bad);bad=wire;byte(bad,ext+43,2);rejectWire(bad);
    bad=wire;byte(bad,ext+3,raw[ext+3]^1);rejectWire(bad);bad=wire;byte(bad,ext+60,1);rejectWire(bad);
    bad=wire;byte(bad,ext+61,255);rejectWire(bad);bad=wire;u32(bad,ext+62,600);rejectWire(bad);
    const auto descriptor=ext+66;for(auto n:{std::uint32_t(maximumCatalogDescriptorBytes+1),UINT32_MAX}){bad=wire;u32(bad,descriptor,n);rejectWire(bad);}
    bad=wire;byte(bad,descriptor+4,0xff);rejectWire(bad);const auto definitions=skipAsset(wire,descriptor);
    for(auto n:{13u,UINT32_MAX}){bad=wire;u32(bad,definitions,n);rejectWire(bad);}
    for(std::size_t cut:{ext,ext+1,ext+43,ext+65,definitions+3,wire.getSize()-1}){if(cut==ext)continue;bad=wire;bad.setSize(cut);rejectWire(bad);}
    bad=wire;appendByte(bad);rejectWire(bad);juce::MemoryBlock capPlusOne(maximumGenerationRequestBytes+1,true);rejectWire(capPlusOne);
    auto rejectRequest=[&](const GenerationRequest& invalid){auto out=sentinel();CHECK(!encodeRequest(invalid,out)&&out==sentinel());};
    auto invalid=initial;invalid.catalog->version=2;rejectRequest(invalid);invalid=initial;invalid.catalog->root.requestId++;rejectRequest(invalid);
    invalid=initial;invalid.catalog->authority.reset();rejectRequest(invalid);invalid=initial;
    auto forged=std::make_shared<CatalogAuthority>(*initial.catalog->authority);forged->root.requestId++;invalid.catalog->authority=forged;rejectRequest(invalid);
    invalid=initial;invalid.catalog->baseIntent=catalogWireIntent(initial);invalid.catalog->baseIntent.catalogSelections[1].localId=7;rejectRequest(invalid);
    invalid=initial;forged=std::make_shared<CatalogAuthority>(*initial.catalog->authority);forged->descriptorJson.resize(maximumCatalogDescriptorBytes+1,' ');invalid.catalog->authority=forged;rejectRequest(invalid);
    auto materialized=catalogWireRequest(true);materialized.catalog->phase=CatalogPhase::materialize;materialized.catalog->selectionParent=initial.tag;
    materialized.catalog->inferenceCallsUsed=2;materialized.catalog->providerStatus=rack::oauth::Status::connected;materialized.catalog->httpStatus=200;
    materialized.catalog->baseIntent=catalogWireIntent(initial);++materialized.tag.requestId;
    CHECK(encodeRequest(materialized,wire));GenerationRequest materializedRead;CHECK(decodeRequest(bytes(wire),materializedRead)&&requestEqual(materializedRead,materialized));
    CHECK(materializedRead.catalog&&materializedRead.catalog->authority->definitions.size()==1&&materializedRead.catalog->authority->root==initial.tag);
    CHECK(encodeRequest(materializedRead,reencoded)&&reencoded==wire);
    auto hydratedLegacy=materialized;hydratedLegacy.catalog.reset();CHECK(encodeRequest(hydratedLegacy,oldWire));const auto hydratedExt=oldWire.getSize();
    const auto hydratedDescriptor=hydratedExt+106;const auto hydratedCount=skipAsset(wire,hydratedDescriptor);const auto record=hydratedCount+4;
    CHECK(at32(wire,hydratedCount)==1);const auto hashLength=record+24;CHECK(at32(wire,hashLength)==64);const auto canonicalLength=skipAsset(wire,hashLength);
    for(auto n:{65537u,UINT32_MAX}){bad=wire;u32(bad,canonicalLength,n);rejectWire(bad);}
    bad=wire;byte(bad,hashLength+4,'A');rejectWire(bad);bad=wire;byte(bad,canonicalLength+4,0xff);rejectWire(bad);
    invalid=materialized;invalid.catalog->inferenceCallsUsed=0;rejectRequest(invalid);invalid=materialized;invalid.catalog->selectionParent.reset();rejectRequest(invalid);
    invalid=materialized;invalid.catalog->selectionParent=materialized.tag;rejectRequest(invalid);
    invalid=materialized;forged=std::make_shared<CatalogAuthority>(*materialized.catalog->authority);forged->definitions.push_back(forged->definitions[0]);invalid.catalog->authority=forged;rejectRequest(invalid);
    invalid=materialized;forged=std::make_shared<CatalogAuthority>(*materialized.catalog->authority);forged->definitions[0].canonical.setSize(65537);invalid.catalog->authority=forged;rejectRequest(invalid);
    std::cout<<"catalog wire initial="<<initialBytes<<" materialize="<<wire.getSize()<<" full source and complete inventory retained\n";
}
void catalog_selection_result_and_ready_readmission()
{
    begin(__func__);const auto initial=catalogWireRequest();const auto selection=catalogWireSelection(initial);juce::MemoryBlock wire;
    const bool encoded=encodeResult(initial,selection,wire);CHECK(encoded);if(!encoded)return;
    GenerationStatus status=GenerationStatus::invalidRequest;CHECK(peekGenerationResultStatus(bytes(wire),status)&&status==GenerationStatus::catalogSelectionRequired);
    GenerationResult read;CHECK(decodeResult(bytes(wire),initial,read)&&resultEqual(read,selection));
    CHECK(!read.candidate&&read.audio.left.empty()&&read.audio.right.empty()&&read.audio.bodyFrames==0);
    CHECK(read.edits.catalogSelections.size()==2&&read.edits.catalogParameters.size()==2&&read.inferenceCalls==2);
    juce::MemoryBlock reencoded;CHECK(encodeResult(initial,read,reencoded)&&reencoded==wire);
    auto rejectResult=[&](const GenerationRequest& capture,const GenerationResult& invalid){auto out=sentinel();CHECK(!encodeResult(capture,invalid,out)&&out==sentinel());};
    auto invalid=selection;invalid.catalog.reset();rejectResult(initial,invalid);invalid=selection;invalid.catalog->root.requestId++;rejectResult(initial,invalid);
    invalid=selection;invalid.catalog->snapshot=juce::Uuid();rejectResult(initial,invalid);invalid=selection;invalid.candidate=initial.source;rejectResult(initial,invalid);
    invalid=selection;invalid.audio.left={.1f};rejectResult(initial,invalid);invalid=selection;invalid.measurements.rms=.1;rejectResult(initial,invalid);
    invalid=selection;invalid.measurements.peak=-0.;rejectResult(initial,invalid);
    invalid=selection;invalid.inferenceCalls=0;rejectResult(initial,invalid);invalid=selection;invalid.edits.catalogParameters[0].normalized=std::numeric_limits<float>::quiet_NaN();rejectResult(initial,invalid);
    auto legacy=initial;legacy.catalog.reset();rejectResult(legacy,selection);
    auto rejectWire=[&](const juce::MemoryBlock& bad,const GenerationRequest& capture){auto out=selection;out.errorCode="sentinel";const auto before=out;CHECK(!decodeResult(bytes(bad),capture,out)&&resultEqual(out,before));};
    // Selection header is53 bytes with empty error code; no measurements/design/candidate/audio branch precedes the typed intent.
    constexpr std::size_t extension=53;auto bad=wire;byte(bad,extension,2);rejectWire(bad,initial);
    bad=wire;byte(bad,extension+2,static_cast<const unsigned char*>(wire.getData())[extension+2]^1);rejectWire(bad,initial);
    bad=wire;appendByte(bad);rejectWire(bad,initial);for(std::size_t cut:{extension,extension+1,extension+57,wire.getSize()-1}){bad=wire;bad.setSize(cut);rejectWire(bad,initial);}
    auto changedCapture=initial;changedCapture.tag.contextRevision++;rejectWire(wire,changedCapture);rejectWire(wire,legacy);
    juce::MemoryBlock capPlusOne(maximumResultPayloadBytes+1,true);rejectWire(capPlusOne,initial);
    auto materialized=catalogWireRequest(true);materialized.catalog->phase=CatalogPhase::materialize;materialized.catalog->selectionParent=initial.tag;
    materialized.catalog->inferenceCallsUsed=selection.inferenceCalls;materialized.catalog->providerStatus=selection.providerStatus;materialized.catalog->httpStatus=selection.httpStatus;
    materialized.catalog->baseIntent=selection.edits;++materialized.tag.requestId;ProjectDocument candidate;
    const auto admitted=admitProposal(materialized.source,materialized.policy,selection.edits,*materialized.catalog->authority,candidate);CHECK(admitted==ProposalStatus::accepted);
    if(admitted!=ProposalStatus::accepted)return;AuditionContext context;context.phrase.meter={1,32};context.phrase.noteCount=1;context.phrase.notes[0]={0,.0625,64,86,1};
    const auto rendered=renderPreview(candidate,context,{},128);CHECK(rendered.status==RenderStatus::ready);if(rendered.status!=RenderStatus::ready)return;
    GenerationResult ready;ready.tag=materialized.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=2;ready.proposalStatus=ProposalStatus::accepted;
    ready.renderStatus=RenderStatus::ready;ready.providerStatus=selection.providerStatus;ready.httpStatus=selection.httpStatus;ready.edits=selection.edits;ready.candidate=candidate;
    ready.audio=rendered.audio;ready.measurements=rendered.measurements;ready.catalog=selection.catalog;
    CHECK(encodeResult(materialized,ready,wire));CHECK(decodeResult(bytes(wire),materialized,read)&&resultEqual(read,ready));
    CHECK(read.candidate&&read.candidate->sound.personalities&&read.candidate->sound.personalities->instances.size()==2);
    ProjectDocument repeated;CHECK(admitProposal(materialized.source,materialized.policy,read.edits,*materialized.catalog->authority,repeated)==ProposalStatus::accepted&&repeated==candidate);
    invalid=ready;invalid.catalog.reset();rejectResult(materialized,invalid);invalid=ready;invalid.edits.catalogParameters[0].normalized=.7f;rejectResult(materialized,invalid);
    invalid=ready;invalid.inferenceCalls=1;rejectResult(materialized,invalid);invalid=ready;invalid.catalog->root=materialized.tag;rejectResult(materialized,invalid);
    invalid=ready;invalid.edits.parameters[0].normalized=.35f;ProjectDocument alteredCandidate;
    CHECK(admitProposal(materialized.source,materialized.policy,invalid.edits,*materialized.catalog->authority,alteredCandidate)==ProposalStatus::accepted);
    invalid.candidate=alteredCandidate;rejectResult(materialized,invalid);
    auto alteredAuthority=materialized;auto forged=std::make_shared<CatalogAuthority>(*materialized.catalog->authority);forged->definitions[0].contentSha256=juce::String::repeatedString("0",64);alteredAuthority.catalog->authority=forged;rejectWire(wire,alteredAuthority);
    invalid=ready;invalid.status=GenerationStatus::providerFailure;invalid.candidate.reset();invalid.edits={};invalid.audio={};rejectResult(materialized,invalid);
    GenerationResult failed;failed.tag=materialized.tag;failed.status=GenerationStatus::renderFailure;failed.inferenceCalls=2;
    failed.providerStatus=selection.providerStatus;failed.httpStatus=selection.httpStatus;failed.errorCode="catalog_render_failure";
    CHECK(encodeResult(materialized,failed,wire));CHECK(decodeResult(bytes(wire),materialized,read)&&resultEqual(read,failed));
    invalid=failed;invalid.inferenceCalls=0;rejectResult(materialized,invalid);invalid=failed;invalid.providerStatus=rack::oauth::Status::ready;rejectResult(materialized,invalid);
    invalid=failed;invalid.httpStatus=0;rejectResult(materialized,invalid);
}
int invalid_rack_source(unsigned which)
{
    std::cout<<"isolated invalid rack source case="<<which<<std::endl;auto r=request();auto state=std::make_shared<RackPersonalityState>();RackInstance i;i.design.name="Invalid ownership";i.design.panel.sections={"Tone"};i.design.units={{0,which?NativeModule::drive:NativeModule::analog,{}}};i.design.role=which?RackRole::fx:RackRole::synth;i.bindings=which?std::vector<UnitBinding>{{0,0}}:std::vector<UnitBinding>{};state->instances.push_back(i);r.source.sound.personalities=state;
    if(which){r.source.sound.layout.effects[0].use=SlotUse::present;r.source.sound.layout.effects[0].chain=255;}
    auto output=sentinel();const bool rejected=!encodeRequest(r,output),unchanged=output==sentinel();std::cout<<"rejected="<<rejected<<" sentinelUnchanged="<<unchanged<<std::endl;return rejected&&unchanged?0:1;
}
// AD's exact legal geometry/text through the existing owning Wire APIs.

std::vector<RackDesign> branchSaturationLibrary()
{
    const auto examples=airack::test::frozenExampleRackDesigns();const auto factory=airack::test::frozenFactoryRackDesigns();
    CHECK(examples.size()==6&&factory.size()==21);
    std::vector<RackDesign> result(examples.begin(),examples.begin()+4);
    for(const auto& design:factory)if(design.units[0].module!=NativeModule::spaceReverb)result.push_back(design);
    CHECK(result.size()==24);return result;
}
void catalog_worst_unique_61_request_envelope(int sourceEntries,bool mainHistory=false,bool acceptedMain=false,bool releasedMain=false,bool currentCatalog=false)
{
    begin(__func__);CHECK((sourceEntries>=65&&sourceEntries<=68)||(currentCatalog&&sourceEntries==71));
    const int externalEntries=releasedMain?42:44;acceptedMain=acceptedMain||releasedMain||currentCatalog;
    const bool beforeTape=sourceEntries<68&&!mainHistory&&!acceptedMain;
    const auto output=juce::File::getCurrentWorkingDirectory().getChildFile("build/task12-worst-unique-wire-ah-output-"+juce::Uuid().toString());
    CHECK(!output.exists());if(output.exists())return;CHECK(output.createDirectory().wasOk());
    auto stored=std::make_unique<GenerationRequest>(request());auto& r=*stored;
    r.tag={juce::Uuid("11223344556677889900aabbccddeeff"),917,813,719};r.instruction="Warm rounded sound";r.role.clear();r.key.clear();r.scale.clear();
    r.capturedPhrase=PatternDocument{};r.capturedBpm=400;
    auto maximumRecipe=[&](NativeModule module){
        RackDesign d;d.role=module==NativeModule::analog?RackRole::synth:RackRole::fx;d.revision=UINT64_MAX;d.units={{0,module,{}}};
        std::string name,intent;for(std::size_t n=0;n<64;++n)name+=n%3==0?'\x01':n%3==1?'"':'\\';
        for(std::size_t n=0;n<512;++n)intent+=n%3==0?'\x01':n%3==1?'"':'\\';
        d.name=juce::String::fromUTF8(name.data(),int(name.size()));d.intent=juce::String::fromUTF8(intent.data(),int(intent.size()));
        d.panel.sections={d.name,d.name,d.name,d.name};
        const bool synth=d.role==RackRole::synth;const auto prefix=juce::String(synth?"synth.1.":"effect.1.");
        for(const auto& p:parameterRegistry())if(p.id.startsWith(prefix)){
            auto key=p.id.substring(prefix.length());const bool compatible=rackParameterActive(module,key);
            const auto parameter=rackParameterIndex(module,0,key);if(!compatible||!parameter||(module==NativeModule::delay&&((beforeTape&&*parameter>=delayTapeParameterStart)||(!acceptedMain&&*parameter>=delayStereoParameterStart))))continue;
            d.units[0].defaults.push_back({0,key,defaultParameterValues()[*parameter]});
            if(d.panel.controls.size()<8&&p.minimum<p.maximum)d.panel.controls.push_back({d.name,0,std::uint8_t(d.panel.controls.size()%4),key,
                p.kind==ParameterKind::continuous?FocusWidget::rotary:p.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice,FocusResponse::linear,p.minimum,p.maximum});
        }
        if(module==NativeModule::delay)CHECK(d.units[0].defaults.size()==(beforeTape?8u:!acceptedMain?13u:16u));
        CHECK(d.panel.controls.size()==8&&validateRackDesign(d,RackValidationMode::portableDesign));return d;
    };
    std::vector<juce::String> names,intents;
    auto uniqueText=[&](RackDesign& d,unsigned ordinal){
        CHECK(ordinal<44);std::string name(64,'\x01'),intent(512,'\x01');
        for(unsigned bit=0;bit<6;++bit)if(ordinal&(1u<<bit)){name[bit]='\x02';intent[bit]='\x02';}
        d.name=juce::String::fromUTF8(name.data(),int(name.size()));d.intent=juce::String::fromUTF8(intent.data(),int(intent.size()));
        for(auto& section:d.panel.sections)section=d.name;for(auto& control:d.panel.controls)control.label=d.name;
        CHECK(d.name.getNumBytesAsUTF8()==64&&d.intent.getNumBytesAsUTF8()==512);
        CHECK(juce::JSON::toString(juce::var(d.name),true).getNumBytesAsUTF8()==386&&juce::JSON::toString(juce::var(d.intent),true).getNumBytesAsUTF8()==3074);
        CHECK(std::find(names.begin(),names.end(),d.name)==names.end()&&std::find(intents.begin(),intents.end(),d.intent)==intents.end());names.push_back(d.name);intents.push_back(d.intent);
        CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
    };
    for(std::uint8_t n=0;n<12;++n){RackOperation op;op.kind=n==0?RackOperationKind::adopt:RackOperationKind::place;op.newInstance=juce::Uuid();
        op.design=maximumRecipe(n<4?NativeModule::analog:NativeModule::delay);uniqueText(*op.design,n);op.allocation={{0,std::uint8_t(n<4?n:n-4)}};
        op.destination={n<4?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(n<4?n:n-4)};
        auto next=std::make_unique<SoundDocument>();RackDifference difference;const auto placed=makeRackCandidate(r.source.sound,op,SoundEditOrigin::manual,*next,difference);
        CHECK(placed==RackCandidateStatus::ready);if(placed!=RackCandidateStatus::ready)return;r.source.sound=std::move(*next);}
    for(std::size_t i=0;i<parameterCount;++i){if(parameterRegistry()[i].kind==ParameterKind::continuous)r.source.sound.values[i]=float(i%101)/100.f;r.source.sound.controlLocks[i]=i%7==0;}
    r.source.sound.values[index("output.gain_db")]=.05f;r.source.pattern.bars=8;r.source.pattern.noteCount=256;
    for(std::size_t n=0;n<8;++n)r.source.sound.macros[n]={parameterRegistry()[0].stateKey,float(int(n)-4)/8};
    for(std::size_t n=0;n<256;++n)r.source.pattern.notes[n]={double(n)/8,.03125,std::uint8_t(n%128),std::uint8_t(1+n%31),std::uint8_t(1+n%16)};
    const bool projectValid=validateProjectDocument(r.source);CHECK(r.source.sound.personalities&&r.source.sound.personalities->instances.size()==12&&projectValid);
    if(!r.source.sound.personalities||r.source.sound.personalities->instances.size()!=12)return;
    std::vector<RackDesign> embedded;for(const auto& i:r.source.sound.personalities->instances)embedded.push_back(i.design);
    const bool embeddedValid=std::all_of(embedded.begin(),embedded.end(),[](const auto& d){return validateRackDesign(d,RackValidationMode::instanceRecall);});CHECK(embeddedValid);
    auto factory=branchSaturationLibrary();
    if(releasedMain||currentCatalog){factory=airack::test::frozenLibraryRackDesigns();CHECK(factory.size()==28);
        std::erase(factory,airack::test::frozenTileArrayRackDesign());
        if(releasedMain)std::erase_if(factory,[](const auto& d){return d.units[0].module==NativeModule::saturation;});
        CHECK(factory.size()==std::size_t(currentCatalog?27:25));}
    if(beforeTape)std::erase_if(factory,[](const auto& d){return d.id==juce::Uuid("330cb53b-cd9b-443c-94d8-2700bc8b88f7");});
    // Keep the predecessor's exact recipes alongside the accepted main and integrated inventories.
    if(!acceptedMain)for(auto& d:factory)for(auto& u:d.units)if(u.module==NativeModule::delay)
        std::erase_if(u.defaults,[](const auto& p){return p.key=="delay.note"||p.key=="delay.right_ratio"||p.key=="delay.ping_pong";});
    if(factory.size()<std::size_t(sourceEntries-externalEntries))return;
    factory.resize(std::size_t(sourceEntries-externalEntries)); // Frozen65/66 prefixes and complete current68. Exact65 byte oracles remain below.
    auto rich=maximumRecipe(NativeModule::analog);const auto fx=maximumRecipe(NativeModule::delay);
    for(std::uint8_t unit=1;unit<=8;++unit){auto u=fx.units[0];u.id=unit;for(auto& value:u.defaults)value.unit=unit;rich.units.push_back(std::move(u));}
    CHECK(rich.units.size()==9&&validateRackDesign(rich,RackValidationMode::portableDesign));RackCatalogStatus status=RackCatalogStatus::unavailable;
    bool extraValid=true;for(unsigned n=0;n<unsigned(externalEntries-12);++n){auto d=rich;d.id=juce::Uuid();uniqueText(d,12+n);extraValid=extraValid&&validateRackDesign(d,RackValidationMode::portableDesign);embedded.push_back(std::move(d));}
    CHECK(names.size()==std::size_t(externalEntries)&&intents.size()==std::size_t(externalEntries)&&extraValid&&embedded.size()==std::size_t(externalEntries));
    RackCatalogSnapshot captured;const bool capturedOk=captureRackCatalog(factory,embedded,{},captured,&status);
    CHECK(capturedOk&&status==RackCatalogStatus::ready&&captured.entries.size()==std::size_t(sourceEntries));if(!capturedOk||captured.entries.size()!=std::size_t(sourceEntries))return;
    unsigned native=0;std::size_t canonicalMaximum=0,canonicalTotal=0;
    const auto definitions=output.getChildFile("canonical-sources");CHECK(definitions.createDirectory().wasOk());
    for(std::size_t n=0;n<captured.entries.size();++n){const auto& entry=captured.entries[n];const bool trusted=rackCatalogNativeEntry(captured,n);native+=trusted;
        for(const auto& unit:entry.definition.units)CHECK(unit.defaults.size()<=(unit.module==NativeModule::spaceReverb?97:unit.module==NativeModule::dynamicEq?(releasedMain||currentCatalog?116:90):unit.module==NativeModule::analog||unit.module==NativeModule::wavetable?140:trusted?82:23));
        juce::MemoryBlock canonical;CHECK(encodeRackDesign(entry.definition,canonical,trusted?RackValidationMode::instanceRecall:RackValidationMode::portableDesign));
        CHECK(canonical.getSize()<=65536&&juce::SHA256(canonical).toHexString()==entry.contentSha256);
        CHECK(definitions.getChildFile(juce::String(int(n))+"-"+entry.definition.id.toDashedString()+".airackrack").replaceWithData(canonical.getData(),canonical.getSize()));
        canonicalMaximum=std::max(canonicalMaximum,canonical.getSize());canonicalTotal+=canonical.getSize();}
    CHECK(native==unsigned(sourceEntries)&&canonicalTotal<=64*65536);
    juce::MemoryBlock project;CHECK(encodeProjectState(r.source,project));CHECK(output.getChildFile("requester.airackproject").replaceWithData(project.getData(),project.getSize()));
    // Full original capture is evidence for the representability attempt. The
    // bounded page is retained separately; it never substitutes a partial set.
    RackCatalogPage original;original.snapshot=captured.id;original.entries=captured.entries;original.next=std::size_t(sourceEntries);original.complete=true;
    const auto descriptor=juce::JSON::toString(rackCatalogPageDescriptor(original),true,22);
    CHECK(output.getChildFile("complete-inventory.json").replaceWithText(descriptor));
    RackCatalogPage bounded;const bool pageOk=catalogPage(captured,{},0,65536,bounded,&status);
    const auto boundedText=juce::JSON::toString(rackCatalogPageDescriptor(bounded),true,22);
    CHECK(output.getChildFile("bounded-page.json").replaceWithText(boundedText));
    CHECK(pageOk&&status==RackCatalogStatus::ready&&!bounded.complete&&boundedText.getNumBytesAsUTF8()<=65536);
    if(releasedMain){CHECK(sourceEntries==67);CHECK(bounded.entries.size()==28&&bounded.next==28&&boundedText.getNumBytesAsUTF8()==60827);CHECK(descriptor.getNumBytesAsUTF8()==526785);}
    if(mainHistory){CHECK(sourceEntries==66);CHECK(bounded.entries.size()==27&&bounded.next==27&&boundedText.getNumBytesAsUTF8()==64783);CHECK(descriptor.getNumBytesAsUTF8()==518218);}
    if(acceptedMain&&sourceEntries==66){CHECK(bounded.entries.size()==27&&bounded.next==27&&boundedText.getNumBytesAsUTF8()==64919);CHECK(descriptor.getNumBytesAsUTF8()==545969);}
    if(sourceEntries==65){CHECK(bounded.entries.size()==26&&bounded.next==26&&boundedText.getNumBytesAsUTF8()==63257);CHECK(descriptor.getNumBytesAsUTF8()==462777);}
    CHECK(maximumCatalogDescriptorBytes==maximumGenerationRequestBytes&&descriptor.getNumBytesAsUTF8()<=maximumCatalogDescriptorBytes);
    std::size_t cursor=0,seen=0,pages=0;
    while(cursor<captured.entries.size()){RackCatalogPage page;CHECK(catalogPage(captured,{},cursor,65536,page,&status));
        CHECK(page.next>cursor&&!page.entries.empty());if(page.next<=cursor)break;
        for(const auto& entry:page.entries){CHECK(entry==captured.entries[seen]);++seen;}cursor=page.next;++pages;
        CHECK(page.complete==(cursor==captured.entries.size()));}
    CHECK(seen==std::size_t(sourceEntries)&&pages>1);
    auto inventory=juce::JSON::parse("{\"sentinel\":true}");const auto inventorySentinel=juce::JSON::toString(inventory,true,22);
    const bool inventoryOk=catalogInventoryFor(captured,inventory);const auto inventoryText=juce::JSON::toString(inventory,true,22);
    if(!inventoryOk)CHECK(inventoryText==inventorySentinel);
    const auto* inventoryEntries=inventory["entries"].getArray();
    const bool inventoryComplete=inventoryOk&&inventoryEntries&&inventoryEntries->size()==sourceEntries&&bool(inventory["complete"])
        &&inventory["next"].toString()==juce::String(sourceEntries)&&inventory["snapshot"].toString()==captured.id.toString();
    CHECK(inventoryComplete&&inventoryText==descriptor);
    if(inventoryComplete)for(int n=0;n<sourceEntries;++n)CHECK(juce::JSON::toString((*inventoryEntries)[n],true,22)==juce::JSON::toString(rackCatalogDescriptor(captured.entries[std::size_t(n)]),true,22));
    CHECK(output.getChildFile("complete-helper.json").replaceWithText(inventoryText));
    std::shared_ptr<CatalogAuthority> authority;
    if(inventoryComplete){authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=captured.id;authority->descriptorJson=inventoryText.toStdString();
        CatalogRequestContext context;context.root=r.tag;context.authority=authority;r.catalog=context;}
    const bool authorityValid=inventoryComplete&&airack::ai::detail::catalogAuthorityValid(*authority);
    const bool contextValid=inventoryComplete&&airack::ai::detail::catalogRequestValid(r);
    auto payload=sentinel(),frame=sentinel();const bool encoded=inventoryComplete&&encodeRequest(r,payload);bool framed=false;juce::var requestSize,bodySize;
    if(!encoded)CHECK(payload==sentinel());
    else{requestSize=juce::var(juce::int64(payload.getSize()));CHECK(authorityValid&&contextValid&&payload.getSize()<=maximumGenerationRequestBytes);
        auto decoded=std::make_unique<GenerationRequest>();CHECK(decodeRequest(bytes(payload),*decoded)&&requestEqual(r,*decoded));
        CHECK(decoded->catalog&&decoded->catalog->authority&&decoded->catalog->authority->descriptorJson==authority->descriptorJson);
        CHECK(output.getChildFile("request.bin").replaceWithData(payload.getData(),payload.getSize()));
        framed=encodeFrame({MessageKind::generate,r.tag.runtime,payload},frame);
        if(!framed)CHECK(frame==sentinel());
        else{bodySize=juce::var(juce::int64(frame.getSize()));CHECK(frame.getSize()==wireHeaderBytes+payload.getSize());Frame read;
            CHECK(decodeFrame(bytes(frame),read)&&read.kind==MessageKind::generate&&read.payload==payload);
            CHECK(output.getChildFile("wire-frame.bin").replaceWithData(frame.getData(),frame.getSize()));}}
    if(!encoded)CHECK(!framed&&requestSize.isVoid()&&bodySize.isVoid());
    if(!framed)CHECK(frame==sentinel()&&bodySize.isVoid());
    CHECK(output.getChildFile("request-observation.bin").replaceWithData(payload.getData(),payload.getSize()));
    CHECK(output.getChildFile("frame-observation.bin").replaceWithData(frame.getData(),frame.getSize()));
    bool capRejected=false,overflowAuthorityValid=false,overflowContextValid=false,combinedRejected=false;
    juce::var overflowDescriptorBytes;
    if(authorityValid&&contextValid){
        auto invalid=std::make_unique<GenerationRequest>(r);auto oversized=std::make_shared<CatalogAuthority>(*authority);
        oversized->descriptorJson.resize(maximumCatalogDescriptorBytes+1,' ');invalid->catalog->authority=oversized;
        const auto before=std::make_unique<GenerationRequest>(*invalid);auto untouched=sentinel();
        CHECK(!airack::ai::detail::catalogAuthorityValid(*oversized)&&!airack::ai::detail::catalogRequestValid(*invalid));
        capRejected=!encodeRequest(*invalid,untouched);CHECK(capRejected&&untouched==sentinel()&&requestEqual(*invalid,*before));
        CHECK(inventoryText.getNumBytesAsUTF8()<maximumCatalogDescriptorBytes);
        auto combined=std::make_unique<GenerationRequest>(r);auto padded=std::make_shared<CatalogAuthority>(*authority);
        padded->descriptorJson.resize(maximumCatalogDescriptorBytes,' ');combined->catalog->authority=padded;
        overflowDescriptorBytes=juce::var(juce::int64(padded->descriptorJson.size()));
        overflowAuthorityValid=airack::ai::detail::catalogAuthorityValid(*padded);overflowContextValid=airack::ai::detail::catalogRequestValid(*combined);
        CHECK(overflowAuthorityValid&&overflowContextValid&&padded->descriptorJson.size()==maximumCatalogDescriptorBytes);
        const auto combinedBefore=std::make_unique<GenerationRequest>(*combined);untouched=sentinel();
        combinedRejected=!encodeRequest(*combined,untouched);CHECK(combinedRejected&&untouched==sentinel()&&requestEqual(*combined,*combinedBefore));
        CHECK(r.catalog->authority==authority&&authority->descriptorJson==inventoryText.toStdString());
        CHECK(output.getChildFile("combined-rejection-sentinel.bin").replaceWithData(untouched.getData(),untouched.getSize()));
    }
    auto* fields=new juce::DynamicObject;
    for(const auto& field:std::initializer_list<std::pair<const char*,juce::var>>{
        {"capture_ok",capturedOk},{"source_entries",sourceEntries},{"factory_entries",sourceEntries-externalEntries},{"embedded_entries",externalEntries},{"native_entries",int(native)},{"unique_maximum_text_entries",externalEntries},
        {"source_project_valid",projectValid},{"embedded_definitions_valid",embeddedValid},{"extra_definitions_valid",extraValid},
        {"canonical_maximum",juce::var(juce::int64(canonicalMaximum))},{"canonical_total",juce::var(juce::int64(canonicalTotal))},{"complete_descriptor_bytes",juce::var(juce::int64(descriptor.getNumBytesAsUTF8()))},
        {"page_ok",pageOk},{"page_status",int(status)},{"page_complete",bounded.complete},{"page_entries",int(bounded.entries.size())},{"page_next",int(bounded.next)},
        {"helper_ok",inventoryOk},{"helper_complete",inventoryComplete},{"helper_matches_full",inventoryOk&&inventoryText==descriptor},
        {"helper_descriptor_bytes",inventoryOk?juce::var(juce::int64(inventoryText.getNumBytesAsUTF8())):juce::var()},{"authority_valid",authorityValid},{"context_valid",contextValid},
        {"request_encoded",encoded},{"request_bytes",requestSize},{"request_sentinel_unchanged",!encoded&&payload==sentinel()},{"frame_attempted",encoded},{"full_wire_body_observed",framed},{"full_wire_body_bytes",bodySize},
        {"frame_sentinel_unchanged",!framed&&frame==sentinel()},{"descriptor_cap_plus_one_rejected",capRejected},
        {"overflow_descriptor_bytes",overflowDescriptorBytes},{"overflow_authority_valid",overflowAuthorityValid},{"overflow_context_valid",overflowContextValid},
        {"combined_request_rejected",combinedRejected},{"combined_body_observed",false},{"combined_body_bytes",juce::var()},
        {"encoder_api","encodeRequest/encodeFrame"},{"encodeJob_api_available",false}})fields->setProperty(field.first,field.second);
    const auto measured=juce::JSON::toString(juce::var(fields),true,22);CHECK(output.getChildFile("measurement.json").replaceWithText(measured));
    std::cout<<"Task12 worst-unique AH Wire envelope "<<measured<<std::endl;
    // Small positive boundary witness: legal complete61 must be representable,
    // with its original descriptor, under existing authority and Wire caps.
    CHECK(inventoryComplete&&inventoryText==descriptor&&authorityValid&&contextValid&&encoded&&framed);
    CHECK(capRejected&&overflowAuthorityValid&&overflowContextValid&&combinedRejected);
}

// Compare explicit and automatic allocation on the same genuinely removed physical slots.
void owned_local_retired_reuse_policy(const SoundDocument& full)
{
    auto retired=std::make_unique<SoundDocument>(full),next=std::make_unique<SoundDocument>();RackDifference difference;
    // Sequential removal forks the order of surviving racks. This policy control needs headroom; the independent maximum source retains UINT64_MAX.
    auto policyState=std::make_shared<RackPersonalityState>(*full.personalities);for(auto& instance:policyState->instances)instance.design.revision=1;
    retired->personalities=policyState;CHECK(validateSoundDocument(*retired));const auto instances=policyState->instances;
    for(const auto& instance:instances){RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=instance.id;
        const auto status=makeRackCandidate(*retired,remove,SoundEditOrigin::manual,*next,difference);
        CHECK(status==RackCandidateStatus::ready);if(status!=RackCandidateStatus::ready)return;*retired=std::move(*next);}
    CHECK(validateSoundDocument(*retired)&&(!retired->personalities||retired->personalities->instances.empty()));
    CHECK(std::all_of(retired->layout.synthUse.begin(),retired->layout.synthUse.end(),[](auto use){return use==SlotUse::retired;}));
    CHECK(std::all_of(retired->layout.effects.begin(),retired->layout.effects.end(),[](const auto& slot){return slot.use==SlotUse::retired;}));
    for(const bool synth:{true,false}){
        const auto old=std::find_if(instances.begin(),instances.end(),[&](const auto& instance){return (instance.design.role==RackRole::synth)==synth;});CHECK(old!=instances.end());if(old==instances.end())return;
        RackOperation operation;operation.design=old->design;operation.newInstance=juce::Uuid();operation.allocation={{0,0}};
        operation.destination={synth?RackRole::synth:RackRole::fx,masterChain,0};
        const auto refused=makeRackCandidate(*retired,operation,SoundEditOrigin::manual,*next,difference);CHECK(refused==RackCandidateStatus::replacementRequired);
        operation.allowSlotReuse=true;const auto explicitReuse=makeRackCandidate(*retired,operation,SoundEditOrigin::manual,*next,difference);
        CHECK(explicitReuse==RackCandidateStatus::ready);if(explicitReuse!=RackCandidateStatus::ready)return;
        CHECK(next->personalities->instances.size()==1&&next->personalities->instances[0].bindings==operation.allocation);
        operation.allocation.clear();operation.newInstance=juce::Uuid();
        auto available=std::make_unique<SoundDocument>(*retired);if(synth)available->layout.synthUse[3]=SlotUse::neverUsed;else available->layout.effects[7].use=SlotUse::neverUsed;
        CHECK(validateSoundDocument(*available));const auto unused=makeRackCandidate(*available,operation,SoundEditOrigin::manual,*next,difference);
        CHECK(unused==RackCandidateStatus::ready);if(unused==RackCandidateStatus::ready)CHECK(next->personalities->instances[0].bindings[0].slot==(synth?3:7));
        const auto automatic=makeRackCandidate(*retired,operation,SoundEditOrigin::manual,*next,difference);
        std::cout<<"OWNED_RETIRED_REUSE kind="<<(synth?"synth":"fx")<<" explicit_refused="<<int(refused)<<" explicit_allowed="<<int(explicitReuse)<<" unused_preferred="<<int(unused)<<" automatic_allowed="<<int(automatic)<<'\n';
        CHECK(automatic==RackCandidateStatus::ready);
    }
}

// Owned-local geometry is independent of the historical melodic25/67 fixtures above.
void owned_local_maximum_envelopes()
{
    begin(__func__);auto input=std::make_unique<LocalGenerationRequest>();auto& r=input->request;r=request();r.source.sound=newInstanceSound();
    r.tag={juce::Uuid(),917,813,719};r.structuralGeneration=UINT64_MAX;r.instruction=std::string(8192,'"');
    r.role=std::string(128,'\\');r.key=std::string(128,'"');r.scale=std::string(128,'\\');r.capturedBpm=400;
    r.namesInUse.assign(maximumNamesInUse,juce::String::repeatedString("\\",maximumSoundNameBytes));
    auto recipe=[&](NativeModule module,unsigned ordinal){
        RackDesign d;d.role=isSourceModule(module)?RackRole::synth:RackRole::fx;d.revision=UINT64_MAX;d.units={{0,module,{}}};
        std::string name(64,'\x01'),intent(512,'\x01');for(unsigned bit=0;bit<6;++bit)if(ordinal&(1u<<bit)){name[bit]='\x02';intent[bit]='\x02';}
        d.name=juce::String::fromUTF8(name.data(),int(name.size()));d.intent=juce::String::fromUTF8(intent.data(),int(intent.size()));
        d.panel.sections={d.name,d.name,d.name,d.name};const auto prefix=juce::String(isSourceModule(module)?"synth.1.":"effect.1.");
        for(const auto& parameter:parameterRegistry())if(parameter.id.startsWith(prefix)){
            const auto key=parameter.id.substring(prefix.length());const auto at=rackParameterIndex(module,0,key);if(!at||!rackParameterActive(module,key))continue;
            d.units[0].defaults.push_back({0,key,defaultParameterValues()[*at]});
            if(d.panel.controls.size()<8&&parameter.minimum<parameter.maximum)d.panel.controls.push_back({d.name,0,std::uint8_t(d.panel.controls.size()%4),key,
                parameter.kind==ParameterKind::continuous?FocusWidget::rotary:parameter.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice,
                FocusResponse::linear,parameter.minimum,parameter.maximum});
        }
        CHECK(d.name.getNumBytesAsUTF8()==64&&d.intent.getNumBytesAsUTF8()==512&&d.panel.controls.size()==8);
        CHECK(validateRackDesign(d,RackValidationMode::portableDesign));return d;
    };
    std::vector<RackDesign> embedded;
    for(std::uint8_t n=0;n<12;++n){RackOperation op;op.kind=RackOperationKind::place;op.allowSlotReuse=true;op.newInstance=juce::Uuid();
        op.design=recipe(n<4?NativeModule::drum:NativeModule::delay,n);op.allocation={{0,std::uint8_t(n<4?n:n-4)}};
        op.destination={n<4?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(n<4?n:n-4)};
        auto candidate=std::make_unique<SoundDocument>();RackDifference difference;const auto result=makeRackCandidate(r.source.sound,op,SoundEditOrigin::manual,*candidate,difference);
        CHECK(result==RackCandidateStatus::ready);if(result!=RackCandidateStatus::ready)return;r.source.sound=std::move(*candidate);embedded.push_back(*op.design);
    }
    owned_local_retired_reuse_policy(r.source.sound);
    r.source.pattern.bars=8;r.source.pattern.noteCount=256;
    for(std::size_t n=0;n<256;++n)r.source.pattern.notes[n]={double(n)/8,.03125,std::uint8_t(n%128),std::uint8_t(1+n%127),std::uint8_t(1+n%16)};
    r.capturedPhrase=r.source.pattern;CHECK(validateProjectDocument(r.source)&&r.source.sound.personalities->instances.size()==12);
    const auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
    CHECK(factory.size()==28&&std::count(factory.begin(),factory.end(),tile)==1
        &&std::count_if(factory.begin(),factory.end(),[&](const auto& d){return d.id==tile.id;})==1);
    for(unsigned n=12;n<41;++n){auto rich=recipe(NativeModule::drum,n);const auto fx=recipe(NativeModule::delay,n);
        for(std::uint8_t unit=1;unit<=8;++unit){auto add=fx.units[0];add.id=unit;for(auto& value:add.defaults)value.unit=unit;rich.units.push_back(std::move(add));}
        CHECK(validateRackDesign(rich,RackValidationMode::portableDesign));embedded.push_back(std::move(rich));}
    RackCatalogSnapshot snapshot;const bool captured=captureRackCatalog(factory,embedded,{},snapshot);CHECK(captured&&snapshot.entries.size()==69);if(!captured)return;
    juce::var inventory;CHECK(catalogInventoryFor(snapshot,CatalogRendering::ownedLocal,inventory));
    const auto* entries=inventory["entries"].getArray();CHECK(entries&&entries->size()==69&&bool(inventory["complete"]));if(!entries||entries->size()!=69)return;
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=snapshot.id;authority->rendering=CatalogRendering::ownedLocal;
    authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();r.catalog.emplace();r.catalog->root=r.tag;r.catalog->authority=authority;
    input->local.root=r.tag;input->local.catalogSnapshot=snapshot.id;input->local.reset=UINT64_MAX;input->local.sourceWord=r.structuralGeneration;
    for(const auto& instance:r.source.sound.personalities->instances)for(const auto& unit:instance.design.units)if(isSourceModule(unit.module)){
        const auto* binding=rackUnitBinding(instance,unit.id);CHECK(binding);if(binding)input->local.sourceMap.banks[binding->slot]={instance.id,unit.id,UINT64_MAX};}
    for(unsigned n=0;n<64;++n){PreviewAssetRef asset;asset.id[31]=std::uint8_t(n+1);asset.channels=2;asset.frames=480000;input->local.sourceMap.assets.push_back(asset);
        auto& pad=input->local.sourceMap.pads[n];pad.asset=std::uint8_t(n+1);pad.name=std::string(64,char('a'+n%26));pad.metadata.fill(std::uint8_t(n+1));}
    CHECK(digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest)&&validLocalPreviewContext(*input));
    juce::MemoryBlock map;CHECK(encodePreviewMap(input->local.sourceMap,map)&&map.getSize()==8935);
    const auto roundtrip=[&](const LocalGenerationRequest& value,juce::MemoryBlock& body){
        const bool encoded=encodeLocalRequest(value,body);CHECK(encoded);if(!encoded)return false;
        auto decoded=std::make_unique<LocalGenerationRequest>();CHECK(decodeLocalRequest(bytes(body),*decoded));
        CHECK(requestEqual(value.request,decoded->request)&&decoded->local==value.local);
        juce::MemoryBlock repeated;CHECK(encodeLocalRequest(*decoded,repeated)&&repeated==body);
        juce::MemoryBlock frame;CHECK(encodeFrame({MessageKind::generateLocal,value.request.tag.runtime,body},frame));
        Frame read;CHECK(decodeFrame(bytes(frame),read)&&read.kind==MessageKind::generateLocal&&read.payload==body);
        CHECK(frame.getSize()==body.getSize()+wireHeaderBytes);return true;
    };
    juce::MemoryBlock body;CHECK(roundtrip(*input,body));if(body.getSize()==0)return;
    std::cout<<"OWNED_MAX_WIRE phase=select factory=28 catalog="<<snapshot.entries.size()<<" embedded="<<embedded.size()
        <<" source_instances=12 sample_map="<<map.getSize()<<" descriptor="<<authority->descriptorJson.size()<<" payload="<<body.getSize()<<'\n';
    const auto originalBody=body;
    for(const auto target:{maximumGenerationRequestBytes-1,maximumGenerationRequestBytes}){
        CHECK(originalBody.getSize()<=target);if(originalBody.getSize()>target)return;
        auto padded=std::make_shared<CatalogAuthority>(*authority);padded->descriptorJson.append(target-originalBody.getSize(),' ');r.catalog->authority=padded;
        CHECK(roundtrip(*input,body)&&body.getSize()==target);
    }
    auto oversized=std::make_shared<CatalogAuthority>(*r.catalog->authority);oversized->descriptorJson+=' ';r.catalog->authority=oversized;
    auto untouched=sentinel();CHECK(!encodeLocalRequest(*input,untouched)&&untouched==sentinel());
    auto saved=std::make_unique<LocalGenerationRequest>();r.catalog->authority=authority;CHECK(decodeLocalRequest(bytes(originalBody),*saved));
    auto bad=body;bad.append(" ",1);auto before=std::make_unique<LocalGenerationRequest>(*saved);
    CHECK(!decodeLocalRequest(bytes(bad),*saved)&&requestEqual(saved->request,before->request)&&saved->local==before->local);
    r.tag.requestId++;r.catalog->phase=CatalogPhase::materialize;r.catalog->selectionParent=authority->root;
    r.catalog->inferenceCallsUsed=3;r.catalog->providerStatus=rack::oauth::Status::connected;r.catalog->httpStatus=200;r.policy.allowSlotReuse=true;
    for(const auto& instance:r.source.sound.personalities->instances)r.catalog->baseIntent.reconciliation.remove.push_back(instance.id);
    for(std::size_t n=0;n<12;++n){const auto& design=embedded[n];const auto found=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[&](const auto& item){return item.definition.id==design.id;});
        CHECK(found!=snapshot.entries.end());if(found==snapshot.entries.end())return;CatalogDefinition definition{design.id,design.revision,found->contentSha256,{}};
        CHECK(encodeRackDesign(design,definition.canonical,RackValidationMode::instanceRecall));authority->definitions.push_back(std::move(definition));
        r.catalog->baseIntent.catalogSelections.push_back({std::uint8_t(n),design.id,design.revision,found->contentSha256,
            {n<4?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(n<4?n:n-4)}});
        for(const auto& value:design.units[0].defaults)r.catalog->baseIntent.catalogParameters.push_back({std::uint8_t(n),value.unit,value.key,value.normalized});
    }
    CHECK(authority->definitions.size()==12&&r.catalog->baseIntent.catalogSelections.size()==12);
    CHECK(airack::ai::detail::catalogAuthorityValid(*authority)&&airack::ai::detail::catalogRequestValid(r)&&validLocalPreviewContext(*input));
    auto admitted=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
    const auto admittedStatus=admitProposal(r.source,r.policy,r.catalog->baseIntent,*authority,*admitted,trace.get());
    std::size_t canonicalTotal=0;for(const auto& definition:authority->definitions)canonicalTotal+=definition.canonical.getSize();
    // Each definition adds UUID16/revision8/hash68/length4; the parent tag adds40. All edits below add further bytes.
    const auto rawFloor=originalBody.getSize()+canonicalTotal+authority->definitions.size()*96+40;
    std::cout<<"OWNED_MAX_WIRE phase=materialize_admission status="<<int(admittedStatus)<<" definitions="<<authority->definitions.size()
        <<" selections="<<r.catalog->baseIntent.catalogSelections.size()<<" parameters="<<r.catalog->baseIntent.catalogParameters.size()
        <<" canonical_bytes="<<canonicalTotal<<" raw_lower_bound="<<rawFloor<<" request_cap="<<maximumGenerationRequestBytes<<'\n';
    CHECK(admittedStatus==ProposalStatus::accepted);
    // The retained all12/716 geometry cannot fit even its conservative floor. Refuse it transactionally.
    CHECK(rawFloor>maximumGenerationRequestBytes);body=sentinel();CHECK(!encodeLocalRequest(*input,body)&&body==sentinel());
    std::cout<<"OWNED_MAX_WIRE phase=materialize encoded=0 body_observed=0 no_body_transactional="<<(body==sentinel())<<'\n';
    std::unique_ptr<LocalGenerationRequest> largest;std::size_t largestBytes=0,largestCount=0;
    for(std::size_t count=1;count<12;++count){
        auto combination=std::make_unique<LocalGenerationRequest>(*input);auto captured=std::make_shared<CatalogAuthority>(*authority);
        captured->definitions.assign(authority->definitions.end()-count,authority->definitions.end());combination->request.catalog->authority=captured;
        auto& intent=combination->request.catalog->baseIntent;const auto first=12-count;
        intent.catalogSelections.erase(intent.catalogSelections.begin(),intent.catalogSelections.begin()+first);
        std::erase_if(intent.catalogParameters,[&](const auto& parameter){return parameter.selection<first;});
        // Replacing a suffix keeps every retained maximum-revision rack's order unchanged.
        intent.reconciliation.remove.erase(intent.reconciliation.remove.begin(),intent.reconciliation.remove.begin()+first);
        *trace={}; // Each combination is an independent root admission, not another cumulative operation.
        const auto admittedCombination=admitProposal(combination->request.source,combination->request.policy,intent,*captured,*admitted,trace.get());
        CHECK(admittedCombination==ProposalStatus::accepted);
        std::size_t exact=originalBody.getSize()+40+16*count+96*count;
        for(const auto& definition:captured->definitions)exact+=96+definition.canonical.getSize();
        for(const auto& parameter:intent.catalogParameters)exact+=10+std::size_t(parameter.key.getNumBytesAsUTF8());
        const auto parameterCount=intent.catalogParameters.size();auto encodedBody=sentinel();const bool encoded=encodeLocalRequest(*combination,encodedBody);
        CHECK(encoded==(exact<=maximumGenerationRequestBytes));
        if(encoded){CHECK(encodedBody.getSize()==exact);largestCount=count;largestBytes=encodedBody.getSize();largest=std::move(combination);}
        else CHECK(encodedBody==sentinel());
        std::cout<<"OWNED_MAX_WIRE phase=accepted_combination_attempt definitions="<<count<<" selections="<<count<<" parameters="<<parameterCount
            <<" admission="<<int(admittedCombination)<<" exact_field_bytes="<<exact<<" encoded="<<encoded;
        if(encoded)std::cout<<" observed_payload="<<encodedBody.getSize();else std::cout<<" body_unobserved=1 transactional="<<(encodedBody==sentinel());std::cout<<'\n';
    }
    CHECK(largest&&largestCount>0&&largestCount<12);if(largest){CHECK(roundtrip(*largest,body)&&body.getSize()==largestBytes);
        std::cout<<"OWNED_MAX_WIRE phase=largest_accepted_combination_of_this_full_source_and_catalog definitions="<<largestCount<<" selections="<<largestCount
            <<" parameters="<<largest->request.catalog->baseIntent.catalogParameters.size()<<" payload="<<largestBytes<<" source_instances=12 catalog=69 map="<<map.getSize()<<'\n';}
}

void extension_local_key_catalog_roundtrip()
{
    begin(__func__);CHECK(rackParameterKeyBytes==38);
    for(const juce::String key:{"dynamic_eq.band.8.cut_slope_extension","dynamic_eq.band.8.spectral_tilt_db_oct"}){
    CHECK(key.getNumBytesAsUTF8()<=rackParameterKeyBytes);
    auto initial=catalogWireRequest();const auto descriptor=juce::JSON::parse(juce::String::fromUTF8(initial.catalog->authority->descriptorJson.c_str()));const auto* entries=descriptor["entries"].getArray();CHECK(entries);if(!entries)return;
    const auto entry=std::find_if(entries->begin(),entries->end(),[](const auto& row){return row["name"].toString()=="Contour Dynamics";});CHECK(entry!=entries->end());if(entry==entries->end())return;
    GenerationResult selection;selection.tag=initial.tag;selection.status=GenerationStatus::catalogSelectionRequired;selection.inferenceCalls=2;selection.providerStatus=rack::oauth::Status::connected;selection.httpStatus=200;selection.proposalStatus=ProposalStatus::accepted;
    selection.catalog=CatalogResultBinding{initial.catalog->root,initial.catalog->authority->snapshot};
    selection.edits.catalogSelections={{7,juce::Uuid((*entry)["id"].toString()),std::uint64_t((*entry)["revision"].toString().getLargeIntValue()),(*entry)["content_sha256"].toString(),{RackRole::fx,masterChain,0}}};
    selection.edits.catalogParameters={{7,0,"dynamic_eq.band.8.shape_extension",1},{7,0,key,1}};
    juce::MemoryBlock wire;const bool encoded=encodeResult(initial,selection,wire);CHECK(encoded);if(encoded){GenerationResult read;CHECK(decodeResult(bytes(wire),initial,read)&&resultEqual(selection,read));}
    selection.edits.catalogParameters[1].key+=juce::String::repeatedString("x",rackParameterKeyBytes+1-key.getNumBytesAsUTF8());auto output=sentinel();CHECK(!encodeResult(initial,selection,output)&&output==sentinel());
    }
}

void present_drum_generation_boundary() {
    begin(__func__);
    for(const auto scope:{RequestScope::sound,RequestScope::pattern,RequestScope::both}){
        auto melodic=std::make_unique<GenerationRequest>(request());melodic->policy.scope=scope;
        juce::MemoryBlock valid;CHECK(encodeRequest(*melodic,valid));auto read=std::make_unique<GenerationRequest>();CHECK(decodeRequest(bytes(valid),*read)&&requestEqual(*read,*melodic));
        auto drum=std::make_unique<GenerationRequest>(*melodic);drum->source.sound.values[drumModeParameterStart]=1;
        CHECK(validateProjectDocument(drum->source));juce::MemoryBlock melodicState,drumState;CHECK(encodeProjectState(melodic->source,melodicState));CHECK(encodeProjectState(drum->source,drumState));CHECK(melodicState.getSize()==drumState.getSize());auto raw=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(drumState.getData(),drumState.getSize(),*raw)&&*raw==drum->source);
        auto output=sentinel();CHECK(!encodeRequest(*drum,output)&&output==sentinel());
        const auto* first=static_cast<const std::byte*>(valid.getData());const auto* last=first+valid.getSize();const auto* pattern=static_cast<const std::byte*>(melodicState.getData());const auto size=melodicState.getSize();const auto* match=std::search(first,last,pattern,pattern+size);CHECK(match!=last);if(match==last)continue;CHECK(std::search(match+size,last,pattern,pattern+size)==last);
        Frame framed{MessageKind::generate,melodic->tag.runtime,valid};juce::MemoryBlock framedBytes;CHECK(encodeFrame(framed,framedBytes));Frame unframed;CHECK(decodeFrame(bytes(framedBytes),unframed)&&unframed.payload==valid);
        auto injected=valid;std::memcpy(static_cast<std::byte*>(injected.getData())+(match-first),drumState.getData(),size);
        auto saved=std::make_unique<GenerationRequest>(*read);CHECK(!decodeRequest(bytes(injected),*read)&&requestEqual(*read,*saved));
    }
}
void saturation_embedded_control_and_remap_refusal()
{
    begin(__func__);auto source=std::make_unique<GenerationRequest>(request());
    CHECK(editRackLayout(source->source.sound.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::saturation},source->source.sound.layout)==RackEditResult::applied);
    RackInstance instance;instance.design.name="Wire saturation";instance.design.role=RackRole::fx;instance.design.units={{0,NativeModule::saturation,{}}};
    instance.design.panel.sections={"Drive"};instance.design.panel.controls={{"Full mix",0,0,"saturation.full.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};instance.bindings={{0,0}};
    auto personalities=std::make_shared<RackPersonalityState>();personalities->instances={instance};source->source.sound.personalities=personalities;
    // Embedded controls remain state; wire6 no longer carries authority to let AI remap them.
    for(const auto* key:{"bypassed","saturation.full.bypassed","saturation.mix","saturation.full.mix"}){
        auto& control=personalities->instances[0].design.panel.controls[0];control.key=key;
        const bool bypass=juce::String(key).endsWith("bypassed");control.label=bypass?"Bypass":"Mix";control.widget=bypass?FocusWidget::toggle:FocusWidget::rotary;
        CHECK(validateProjectDocument(source->source));juce::MemoryBlock wire;const bool encoded=encodeRequest(*source,wire);CHECK(encoded);
        auto requestRead=std::make_unique<GenerationRequest>();if(encoded)CHECK(decodeRequest(bytes(wire),*requestRead)&&requestEqual(*source,*requestRead));
        auto result=std::make_unique<GenerationResult>();result->tag=source->tag;result->status=GenerationStatus::ready;result->inferenceCalls=1;
        result->proposalStatus=ProposalStatus::accepted;result->renderStatus=RenderStatus::ready;result->providerStatus=rack::oauth::Status::connected;
        auto candidate=std::make_unique<ProjectDocument>();CHECK(admitProposal(source->source,source->policy,result->edits,*candidate)==ProposalStatus::accepted);result->candidate=*candidate;
        result->audio.bodyFrames=1;result->audio.left.resize(1+previewTailFrames);result->audio.right.resize(1+previewTailFrames);
        const bool resultEncoded=encodeResult(*source,*result,wire);CHECK(resultEncoded);auto resultRead=std::make_unique<GenerationResult>();
        if(resultEncoded)CHECK(decodeResult(bytes(wire),*source,*resultRead)&&resultEqual(*result,*resultRead));
        result->edits.reconciliation.controls={{instance.id,0,control}};auto unchanged=std::make_unique<ProjectDocument>(*candidate);
        CHECK(admitProposal(source->source,source->policy,result->edits,*unchanged)==ProposalStatus::scopeViolation&&*unchanged==*candidate);
        auto output=sentinel();CHECK(!encodeResult(*source,*result,output)&&output==sentinel());
        for(unsigned mutation=0;mutation<5;++mutation){
            auto invalid=std::make_unique<GenerationRequest>(*source);auto state=std::make_shared<RackPersonalityState>(*personalities);auto& changed=state->instances[0].design.panel.controls[0];
            if(mutation==0)changed.key="saturation.xyz";if(mutation==1)changed.unit=9;if(mutation==2)changed.section=8;
            if(mutation==3)changed.minimum=changed.maximum;if(mutation==4)changed.maximum=std::numeric_limits<float>::infinity();invalid->source.sound.personalities=state;
            output=sentinel();CHECK(!validateProjectDocument(invalid->source)&&!encodeRequest(*invalid,output)&&output==sentinel());
        }
    }
}
void invalid_source_before_capture()
{
    begin(__func__);for(unsigned which:{0u,1u}){juce::ChildProcess child;juce::StringArray args{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--invalid-rack-source",juce::String(which)};const bool started=child.start(args);CHECK(started);if(!started)continue;const bool ended=child.waitForProcessToFinish(5000);CHECK(ended);if(!ended){CHECK(child.kill());child.waitForProcessToFinish(1000);}std::cout<<child.readAllProcessOutput()<<"invalid source child exit="<<child.getExitCode()<<std::endl;CHECK(ended&&child.getExitCode()==0);}
}

}
static void compact_inventory_wire()
{
    const auto object=[](std::initializer_list<std::pair<juce::Identifier,juce::var>> fields){auto* value=new juce::DynamicObject;for(const auto& [key,item]:fields)value->setProperty(key,item);return juce::var(value);};
    begin(__func__);auto r=std::make_unique<GenerationRequest>(request());auto authority=std::make_shared<CatalogAuthority>();authority->root=r->tag;authority->snapshot=juce::Uuid();
    const auto design=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;CHECK(encodeRackDesign(design,canonical,RackValidationMode::instanceRecall));const auto hash=juce::SHA256(canonical).toHexString();
    juce::Array<juce::var> rows;for(int n=0;n<541;++n)rows.add(juce::Array<juce::var>{n?juce::Uuid().toString():design.id.toString(),juce::String(std::to_string(design.revision)),hash,n?juce::String("Installed ")+juce::String(n):design.name,"synth",juce::Array<juce::var>{0},1,0,juce::Array<juce::var>{0}});
    auto descriptor=object({{"format",2},{"complete",true},{"columns",juce::Array<juce::var>{"id","revision","sha256","name","role","tags","synthUnits","fxUnits","modules"}},
        {"tags",juce::Array<juce::var>{object({{"id","analog"},{"label","Analog"}})}},{"entries",rows}});
    authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();r->catalog.emplace();r->catalog->root=r->tag;r->catalog->authority=authority;
    juce::MemoryBlock body;CHECK(encodeRequest(*r,body));auto returned=std::make_unique<GenerationRequest>();CHECK(decodeRequest(bytes(body),*returned));
    CHECK(returned->catalog&&returned->catalog->version==1&&returned->catalog->authority->descriptorJson==authority->descriptorJson);CHECK(body.getSize()<=maximumGenerationRequestBytes);
    authority->definitions.push_back({design.id,design.revision,hash,canonical});r->catalog->phase=CatalogPhase::materialize;r->catalog->selectionParent=r->tag;r->tag.requestId++;r->catalog->inferenceCallsUsed=1;r->catalog->providerStatus=rack::oauth::Status::connected;
    r->catalog->baseIntent.catalogSelections={{7,design.id,design.revision,hash,{RackRole::synth,masterChain,1}}};CHECK(encodeRequest(*r,body));CHECK(decodeRequest(bytes(body),*returned));CHECK(returned->catalog->authority->definitions.front().canonical==canonical);
    auto invalid=descriptor.clone();invalid["columns"].getArray()->set(2,"package_path");authority->descriptorJson=juce::JSON::toString(invalid,true,22).toStdString();body=sentinel();CHECK(!encodeRequest(*r,body)&&body==sentinel());
    std::cout<<"COMPACT_WIRE rows=541 version=1 descriptor="<<juce::JSON::toString(descriptor,true,22).getNumBytesAsUTF8()<<" selected=1\n";
}
static void initial28_companion_materialization()
{
    begin(__func__);const auto designs=airack::test::frozenLibraryRackDesigns();CHECK(designs.size()==28);
    for(const auto& design:designs){
        auto input=std::make_unique<LocalGenerationRequest>();auto& r=input->request;r=request();r.source.sound=newInstanceSound();
        const bool local=std::any_of(design.units.begin(),design.units.end(),[](const auto& u){return u.module==NativeModule::drum;});
        RackCatalogSnapshot selected;CHECK(captureRackCatalog(std::span(&design,1),{},{},selected));juce::var descriptor;
        CHECK(catalogInventoryFor(selected,local?CatalogRendering::ownedLocal:CatalogRendering::companionPcm,descriptor));
        auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=selected.id;authority->rendering=local?CatalogRendering::ownedLocal:CatalogRendering::companionPcm;
        authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();CatalogDefinition definition{design.id,design.revision,selected.entries[0].contentSha256,{}};
        CHECK(encodeRackDesign(design,definition.canonical,RackValidationMode::instanceRecall));authority->definitions.push_back(definition);
        r.catalog.emplace();r.catalog->root=r.tag;r.catalog->phase=CatalogPhase::materialize;r.catalog->selectionParent=r.tag;r.tag.requestId++;
        r.catalog->authority=authority;r.catalog->inferenceCallsUsed=1;r.catalog->providerStatus=rack::oauth::Status::connected;
        r.catalog->baseIntent.catalogSelections={{7,design.id,design.revision,definition.contentSha256,{design.role,masterChain,0}}};
        juce::MemoryBlock body;bool encoded=false,decoded=false;
        if(local){input->local.root=authority->root;input->local.catalogSnapshot=selected.id;input->local.sourceWord=r.structuralGeneration;
            CHECK(digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest));encoded=encodeLocalRequest(*input,body);
            auto returned=std::make_unique<LocalGenerationRequest>();decoded=encoded&&decodeLocalRequest(bytes(body),*returned);if(decoded)r=std::move(returned->request);
        }else{encoded=encodeRequest(r,body);auto returned=std::make_unique<GenerationRequest>();decoded=encoded&&decodeRequest(bytes(body),*returned);if(decoded)r=std::move(*returned);}
        CHECK(encoded&&decoded);auto admitted=std::make_unique<ProjectDocument>();
        const auto status=admitProposal(r.source,r.policy,r.catalog->baseIntent,*r.catalog->authority,*admitted);CHECK(status==ProposalStatus::accepted);
        CHECK(admitted->sound.personalities&&admitted->sound.personalities->instances.size()==1);
        if(admitted->sound.personalities&&!admitted->sound.personalities->instances.empty())CHECK(admitted->sound.personalities->instances.front().design==design);
        std::cout<<"INITIAL28_COMPANION "<<design.name<<" local="<<local<<" portable="<<validateRackDesign(design,RackValidationMode::portableDesign)<<" admission="<<int(status)<<'\n';
    }
}

int main(int argc,char** argv)
{
    if(argc==2&&juce::String(argv[1])=="--account-messages"){account_messages();std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures?1:0;}
#if defined(_MSC_VER)
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);_CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    if(argc==2&&juce::String(argv[1])=="--initial28"){initial28_companion_materialization();std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--compact-inventory"){compact_inventory_wire();std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures?1:0;}
    compact_inventory_wire();
    initial28_companion_materialization();
    if(argc==2&&juce::String(argv[1])=="--owned-maximum"){owned_local_maximum_envelopes();std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--extension-keys"){extension_local_key_catalog_roundtrip();std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--saturation-wire"){saturation_result_and_base_intent_roundtrip();saturation_embedded_control_and_remap_refusal();std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[1])=="--invalid-rack-source")return invalid_rack_source(unsigned(juce::String(argv[2]).getIntValue()));
    owned_local_maximum_envelopes();present_drum_generation_boundary();extension_local_key_catalog_roundtrip();catalog_request_extension_and_atomic_bounds();catalog_selection_result_and_ready_readmission();catalog_worst_unique_61_request_envelope(65);catalog_worst_unique_61_request_envelope(66);catalog_worst_unique_61_request_envelope(67);catalog_worst_unique_61_request_envelope(68);catalog_worst_unique_61_request_envelope(66,true);catalog_worst_unique_61_request_envelope(66,false,true);catalog_worst_unique_61_request_envelope(68,false,true);catalog_worst_unique_61_request_envelope(67,false,true,true);catalog_worst_unique_61_request_envelope(71,false,true,false,true);
    mid_side_request_result_and_old_authority();
    frequency_split_request_result_and_old_authority();
    convolution_request_result_and_old_authority();
    frequency_shift_request_result_and_old_authority();
    dimension_request_result_and_old_nested_authority();
    stereo_request_result_and_old_nested_authority();
    flanger_request_result_and_old_nested_authority();phaser_request_result_and_old_nested_authority();chorus_request_result_and_old_nested_authority();filter_request_result_and_old_nested_authority();eq_request_result_and_readmission();modern_reverb_preview_and_wire_roundtrip();invalid_source_before_capture();reconciliation_oracles();v4_nested_personality_and_count_bounds();v4_large_state_frames();exact_request_and_cold_macro();admitted_result_and_audio();names_cross_the_wire();frame_and_control_bounds();malformed_transactional_decoding();account_messages();
    required_policy_request_wire();optional_result_required_readmission();missing_pattern_failure_wire();version_two_rejects_legacy_frames();
    std::cout<<checks<<" checks / "<<sections<<" sections / "<<failures<<" failures\n";return failures==0?0:1;
}
