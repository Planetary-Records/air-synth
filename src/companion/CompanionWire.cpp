#include "CompanionWire.h"
#include "PatternIO.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
namespace airack::companion
{
static_assert(ai::maximumCatalogDescriptorBytes == maximumGenerationRequestBytes);
namespace
{
using namespace ai;
template<class E> bool known(E value,E last) { return static_cast<std::uint64_t>(value)<=static_cast<std::uint64_t>(last); }
bool rawText(std::string_view s,std::size_t cap)
{ return s.size()<=cap && s.find('\0')==std::string_view::npos && (s.empty() || juce::CharPointer_UTF8::isValidString(s.data(),static_cast<int>(s.size()))); }
bool textValid(const juce::String& s,std::size_t cap) { return s.getNumBytesAsUTF8()<=cap && rawText(s.toStdString(),cap); }
bool modelValid(const juce::String& s,bool allowEmpty=false)
{ return (allowEmpty || s.isNotEmpty()) && s.length()<=256 && s.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-"); }
bool bpmValid(double v) { return std::isfinite(v) && v>=20 && v<=400; }
bool httpValid(int v) { return v==0 || (v>=100 && v<=599); }
bool tagValid(const RequestTag& t) { return !t.runtime.isNull() && t.requestId!=0; }
bool requestValid(const GenerationRequest& r,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    (void) parameterRegistry(); // Control-thread bootstrap before saved macro/ID lookup.
    if(rendering>CatalogRendering::ownedLocal)return false;
    if(r.catalog&&(!r.catalog->authority||r.catalog->authority->rendering!=rendering))return false;
    for(std::uint8_t bank=0;bank<4;++bank)if(rendering==CatalogRendering::companionPcm&&r.source.sound.layout.synthUse[bank]==SlotUse::present&&sourceModule(r.source.sound,bank)==NativeModule::drum)return false;
    return validateProjectDocument(r.source) && tagValid(r.tag) && known(r.policy.scope,RequestScope::both) && modelValid(r.model)
        && (!r.policy.requirePattern || r.policy.scope!=RequestScope::sound)
        && !r.instruction.empty() && rawText(r.instruction,8192) && rawText(r.role,128) && rawText(r.key,128) && rawText(r.scale,128)
        && (!r.capturedPhrase || validatePatternDocument(*r.capturedPhrase)) && (!r.capturedBpm || bpmValid(*r.capturedBpm))
        && r.namesInUse.size()<=ai::maximumNamesInUse
        && std::all_of(r.namesInUse.begin(),r.namesInUse.end(),[](const juce::String& n){return n.isNotEmpty()&&textValid(n,maximumSoundNameBytes);})
        && (!r.catalog || (r.catalog->version==1 && known(r.catalog->phase,CatalogPhase::materialize)
            && known(r.catalog->providerStatus,rack::oauth::Status::busy) && httpValid(r.catalog->httpStatus)
            && r.catalog->inferenceCallsUsed<=3 && ai::detail::catalogRequestValid(r)));
}
// Check lengths before conversion, nested decoding or allocation.
struct Reader
{
    WireBytes data; std::size_t position=0;
    std::size_t remaining() const { return data.size()-position; }
    bool take(std::size_t n,WireBytes& out) { if(n>remaining())return false;out=data.subspan(position,n);position+=n;return true; }
    template<class U> bool integer(U& out)
    {
        static_assert(std::is_unsigned_v<U>);WireBytes b;if(!take(sizeof(U),b))return false;
        U v=0;for(std::size_t i=0;i<sizeof(U);++i)v|=static_cast<U>(static_cast<U>(std::to_integer<unsigned char>(b[i]))<<(8*i));out=v;return true;
    }
    bool real(float& out) { std::uint32_t v;if(!integer(v))return false;out=std::bit_cast<float>(v);return true; }
    bool real(double& out) { std::uint64_t v;if(!integer(v))return false;out=std::bit_cast<double>(v);return true; }
    bool flag(bool& out) { std::uint8_t v;if(!integer(v) || v>1)return false;out=v!=0;return true; }
    template<class E> bool enumeration(E& out,E last) { std::uint8_t v;if(!integer(v) || !known(static_cast<E>(v),last))return false;out=static_cast<E>(v);return true; }
    bool uuid(juce::Uuid& out) { WireBytes b;if(!take(16,b))return false;out=juce::Uuid(reinterpret_cast<const std::uint8_t*>(b.data()));return true; }
    bool tag(RequestTag& out) { return uuid(out.runtime) && integer(out.requestId) && integer(out.editRevision) && integer(out.contextRevision); }
    bool raw(std::string& out,std::size_t cap)
    {
        std::uint32_t n;WireBytes b;if(!integer(n) || n>cap || !take(n,b))return false;
        const std::string_view s(reinterpret_cast<const char*>(b.data()),b.size());if(!rawText(s,cap))return false;out.assign(s);return true;
    }
    bool text(juce::String& out,std::size_t cap)
    {
        std::uint32_t n;WireBytes b;if(!integer(n) || n>cap || !take(n,b))return false;
        const std::string_view s(reinterpret_cast<const char*>(b.data()),b.size());if(!rawText(s,cap))return false;out=juce::String::fromUTF8(s.data(),static_cast<int>(s.size()));return true;
    }
    bool project(ProjectDocument& out)
    {
        std::uint32_t n;WireBytes b;return integer(n) && n>=minimumProjectStateBytes && n<=maximumProjectStateBytes && take(n,b) && decodeProjectState(b.data(),b.size(),out);
    }
    bool pattern(PatternDocument& out)
    { std::uint32_t n;WireBytes b;return integer(n) && n>=56 && n<=maximumPatternAssetBytes && take(n,b) && decodePatternAsset(b.data(),b.size(),out); }
    bool count(std::uint32_t& n,std::size_t cap,std::size_t minimumBytes) { return integer(n) && n<=cap && n<=remaining()/minimumBytes; }
    bool done() const { return position==data.size(); }
};
struct Writer
{
    juce::MemoryOutputStream stream; std::size_t cap; bool good=true;
    void raw(const void* p,std::size_t n) { if(!good)return;if(n>cap-stream.getDataSize()){good=false;return;}good=stream.write(p,n); }
    template<class U> void integer(U v)
    {
        static_assert(std::is_unsigned_v<U>);std::array<std::byte,sizeof(U)> b;
        for(std::size_t i=0;i<sizeof(U);++i)b[i]=std::byte(static_cast<unsigned char>(v>>(8*i)));raw(b.data(),b.size());
    }
    void real(float v) { integer(std::bit_cast<std::uint32_t>(v)); }
    void real(double v) { integer(std::bit_cast<std::uint64_t>(v)); }
    void flag(bool v) { integer<std::uint8_t>(v?1:0); }
    template<class E> void enumeration(E v) { integer(static_cast<std::uint8_t>(v)); }
    void uuid(const juce::Uuid& v) { raw(v.getRawData(),16); }
    void tag(const RequestTag& t) { uuid(t.runtime);integer(t.requestId);integer(t.editRevision);integer(t.contextRevision); }
    void text(std::string_view s) { integer(static_cast<std::uint32_t>(s.size()));raw(s.data(),s.size()); }
    void text(const juce::String& s) { const auto utf8=s.toStdString();text(std::string_view(utf8)); }
    bool project(const ProjectDocument& d)
    { juce::MemoryBlock b;if(!encodeProjectState(d,b))return false;integer(static_cast<std::uint32_t>(b.getSize()));raw(b.getData(),b.getSize());return good; }
    bool pattern(const PatternDocument& d)
    { juce::MemoryBlock b;if(!encodePatternAsset(d,b))return false;integer(static_cast<std::uint32_t>(b.getSize()));raw(b.getData(),b.getSize());return good; }
    bool publish(juce::MemoryBlock& out) { if(!good)return false;auto local=stream.getMemoryBlock();out.swapWith(local);return true; }
};

bool choicesValid(const RackReconcileChoices& c,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    for(const auto* list:{&c.detach,&c.remove,&c.replace}){
        if(list->size()>12)return false;
        for(std::size_t i=0;i<list->size();++i){if((*list)[i].isNull())return false;for(std::size_t j=0;j<i;++j)if((*list)[j]==(*list)[i])return false;}
    }
    if(c.controls.size()>96)return false;
    for(std::size_t i=0;i<c.controls.size();++i){const auto& r=c.controls[i];if(r.instance.isNull()||r.control>7)return false;
        for(std::size_t j=0;j<i;++j)if(c.controls[j].instance==r.instance&&c.controls[j].control==r.control)return false;
        if(r.replacement){const auto& f=*r.replacement;
            if(f.unit>8||f.section>3||!known(f.widget,FocusWidget::choice)||!known(f.response,FocusResponse::logarithmic)||!textValid(f.label,64)||f.label.isEmpty()
                ||f.key.isEmpty()||f.key.getNumBytesAsUTF8()>rackParameterKeyBytes||!f.key.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")
                ||!std::isfinite(f.minimum)||!std::isfinite(f.maximum)||f.minimum>=f.maximum||(f.response==FocusResponse::logarithmic&&f.minimum<=0))return false;
            bool knownKey=false;for(unsigned module=0;module<=static_cast<unsigned>(NativeModule::saturation);++module)if((rendering==CatalogRendering::ownedLocal||module!=unsigned(NativeModule::drum))&&rackParameterIndex(static_cast<NativeModule>(module),0,f.key))knownKey=true;if(!knownKey)return false;
        }
    }return true;
}
bool writeChoices(Writer& w,const RackReconcileChoices& c,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    if(!choicesValid(c,rendering))return false;Writer p{{},maximumRackReconciliationBytes-4};p.integer<std::uint8_t>(1);
    for(auto n:{c.detach.size(),c.remove.size(),c.replace.size(),c.controls.size()})p.integer(static_cast<std::uint8_t>(n));
    for(const auto* list:{&c.detach,&c.remove,&c.replace})for(const auto& id:*list)p.uuid(id);
    for(const auto& r:c.controls){p.uuid(r.instance);p.integer(r.control);p.flag(r.replacement.has_value());if(r.replacement){const auto& f=*r.replacement;
        p.integer(f.unit);p.integer(f.section);p.enumeration(f.widget);p.enumeration(f.response);p.real(f.minimum);p.real(f.maximum);
        for(const auto* text:{&f.label,&f.key}){const auto utf8=text->toStdString();p.integer(static_cast<std::uint8_t>(utf8.size()));p.raw(utf8.data(),utf8.size());}}}
    juce::MemoryBlock b;if(!p.publish(b))return false;w.integer(static_cast<std::uint32_t>(b.getSize()));w.raw(b.getData(),b.getSize());return w.good;
}
bool readChoices(Reader& r,RackReconcileChoices& c,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    std::uint32_t length;WireBytes b;if(!r.integer(length)||length<5||length>maximumRackReconciliationBytes-4||!r.take(length,b))return false;Reader p{b};std::uint8_t version;std::array<std::uint8_t,4> counts{};
    if(!p.integer(version)||version!=1)return false;for(auto& n:counts)if(!p.integer(n))return false;
    if(counts[0]>12||counts[1]>12||counts[2]>12||counts[3]>96||16u*(counts[0]+counts[1]+counts[2])+18u*counts[3]>p.remaining())return false;
    std::size_t i=0;for(auto* list:{&c.detach,&c.remove,&c.replace}){list->reserve(counts[i]);for(unsigned j=0;j<counts[i];++j){juce::Uuid id;if(!p.uuid(id)||id.isNull())return false;list->push_back(id);}++i;}
    c.controls.reserve(counts[3]);for(unsigned j=0;j<counts[3];++j){RackControlResolution v;bool present;if(!p.uuid(v.instance)||!p.integer(v.control)||!p.flag(present))return false;if(present){FocusedControl f;
        if(!p.integer(f.unit)||!p.integer(f.section)||!p.enumeration(f.widget,FocusWidget::choice)||!p.enumeration(f.response,FocusResponse::logarithmic)||!p.real(f.minimum)||!p.real(f.maximum))return false;
        for(auto* text:{&f.label,&f.key}){std::uint8_t n;WireBytes t;if(!p.integer(n)||n>(text==&f.label?64:rackParameterKeyBytes)||!p.take(n,t))return false;const std::string_view utf8(reinterpret_cast<const char*>(t.data()),t.size());if(!rawText(utf8,n))return false;*text=juce::String::fromUTF8(utf8.data(),int(n));}v.replacement=f;}c.controls.push_back(v);}
    return p.done()&&choicesValid(c,rendering);
}
template<class Function> bool recoverAllocation(Function&& f)
{ try{return f();}catch(const std::bad_alloc&){return false;}catch(const std::length_error&){return false;} }
WireBytes blockBytes(const juce::MemoryBlock& b) { return {static_cast<const std::byte*>(b.getData()),b.getSize()}; }
std::size_t payloadCap(MessageKind k) { return messagePayloadCap(k); }
bool frameHeaderValid(const FrameHeader& h)
{ return h.kind>=MessageKind::hello && known(h.kind,MessageKind::mcpEvent) && h.payloadBytes<=payloadCap(h.kind) && (h.kind==MessageKind::hello?h.session.isNull():!h.session.isNull()); }
bool controlShape(MessageKind k,WireBytes payload)
{
    if(k!=MessageKind::hello && k!=MessageKind::welcome && k!=MessageKind::cancel && k!=MessageKind::ping && k!=MessageKind::pong && k!=MessageKind::failure)return true;
    ControlMessage c;if(!decodeControl(payload,c))return false;const bool zero=c.tag.requestId==0 && c.tag.editRevision==0 && c.tag.contextRevision==0;
    if(k!=MessageKind::hello&&k!=MessageKind::welcome&&c.capabilities)return false;
    switch(k){
        case MessageKind::hello:case MessageKind::ping:case MessageKind::pong:return zero && c.status==RuntimeStatus::ready;
        case MessageKind::welcome:return zero;
        case MessageKind::cancel:return c.tag.requestId>0 && c.status==RuntimeStatus::ready;
        case MessageKind::failure:return c.status!=RuntimeStatus::ready && (c.tag.requestId!=0 || zero);
        default:return false;
    }
}
bool readMeasurements(Reader& r,AudioMeasurements& m)
{
    std::uint32_t clips;bool onset;
    if(!r.flag(m.finite) || !r.integer(clips) || clips>1152000 || !r.real(m.peak) || !r.real(m.rms) || !r.flag(onset))return false;
    m.clippedSamples=clips;if(onset){double v;if(!r.real(v))return false;m.onsetSeconds=v;}
    if(!r.real(m.activityEndSeconds) || !r.real(m.decaySeconds))return false;for(auto& band:m.bandFractions)if(!r.real(band))return false;return r.flag(m.tailActiveAtEnd);
}
void writeMeasurements(Writer& w,const AudioMeasurements& m)
{
    w.flag(m.finite);w.integer(static_cast<std::uint32_t>(m.clippedSamples));w.real(m.peak);w.real(m.rms);w.flag(m.onsetSeconds.has_value());if(m.onsetSeconds)w.real(*m.onsetSeconds);
    w.real(m.activityEndSeconds);w.real(m.decaySeconds);for(auto band:m.bandFractions)w.real(band);w.flag(m.tailActiveAtEnd);
}
bool measurementValid(const AudioMeasurements& m,std::size_t frames)
{
    const double duration=static_cast<double>(frames)/previewSampleRate;
    const auto time=[](double v,double upper){return std::isfinite(v) && v>=0 && v<=upper+1e-9;};
    if(!m.finite || m.clippedSamples!=0 || !std::isfinite(m.peak) || m.peak<0 || m.peak>=1 || !std::isfinite(m.rms) || m.rms<0 || m.rms>=1
        || (m.onsetSeconds && !time(*m.onsetSeconds,duration)) || !time(m.activityEndSeconds,duration) || !time(m.decaySeconds,4))return false;
    double sum=0;for(double v:m.bandFractions){if(!std::isfinite(v) || v<0 || v>1)return false;sum+=v;}return sum==0 || std::abs(sum-1)<=1e-6;
}
bool editsTextValid(const ProposalEdits& e)
{
    if(e.parameters.size()>parameterCount || e.racks.size()>64 || e.macros.size()>8 || e.characteristics.length()>2048 || e.explanation.length()>2048
        || !textValid(e.characteristics,8192) || !textValid(e.explanation,8192) || !textValid(e.name,maximumSoundNameBytes))return false;
    for(const auto& p:e.parameters)if(!textValid(p.id,256))return false;for(const auto& m:e.macros)if(!textValid(m.targetId,256))return false;return true;
}
bool readEdits(Reader& r,ProposalEdits& e,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    std::uint32_t n;if(!r.count(n,parameterCount,8))return false;e.parameters.reserve(n);
    for(std::uint32_t i=0;i<n;++i){ParameterEdit p;if(!r.text(p.id,256) || !r.real(p.normalized) || !std::isfinite(p.normalized) || p.normalized<0 || p.normalized>1)return false;e.parameters.push_back(std::move(p));}
    if(!r.count(n,64,6))return false;e.racks.reserve(n);
    for(std::uint32_t i=0;i<n;++i){RackEdit e0;if(!r.enumeration(e0.kind,RackEditKind::replaceEffect) || !r.integer(e0.slot) || !r.integer(e0.destination) || !r.integer(e0.position) || !r.enumeration(e0.effect,EffectKind::saturation) || !r.flag(e0.allowReuse))return false;e.racks.push_back(e0);}
    if(!r.count(n,8,9))return false;e.macros.reserve(n);
    for(std::uint32_t i=0;i<n;++i){MacroEdit m;if(!r.integer(m.index) || m.index<1 || m.index>8 || !r.text(m.targetId,256) || !r.real(m.amount) || !std::isfinite(m.amount) || m.amount < -1 || m.amount > 1)return false;e.macros.push_back(std::move(m));}
    bool pattern;if(!r.flag(pattern))return false;if(pattern){PatternDocument p;if(!r.pattern(p))return false;e.pattern=p;}
    return r.text(e.characteristics,8192) && r.text(e.explanation,8192) && r.text(e.name,maximumSoundNameBytes) && readChoices(r,e.reconciliation,rendering) && editsTextValid(e);
}
bool writeEdits(Writer& w,const ProposalEdits& e,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    w.integer(static_cast<std::uint32_t>(e.parameters.size()));for(const auto& p:e.parameters){w.text(p.id);w.real(p.normalized);}
    w.integer(static_cast<std::uint32_t>(e.racks.size()));for(const auto& r:e.racks){w.enumeration(r.kind);w.integer(r.slot);w.integer(r.destination);w.integer(r.position);w.enumeration(r.effect);w.flag(r.allowReuse);}
    w.integer(static_cast<std::uint32_t>(e.macros.size()));for(const auto& m:e.macros){w.integer(m.index);w.text(m.targetId);w.real(m.amount);}
    w.flag(e.pattern.has_value());if(e.pattern && !w.pattern(*e.pattern))return false;w.text(e.characteristics);w.text(e.explanation);w.text(e.name);return writeChoices(w,e.reconciliation,rendering)&&w.good;
}
constexpr std::size_t maximumCatalogSelections=12;
bool catalogHashValid(const juce::String& hash)
{ return hash.length()==64 && hash.containsOnly("0123456789abcdef"); }
bool catalogEditsTextValid(const ProposalEdits& e)
{
    if(e.catalogSelections.size()>maximumCatalogSelections || e.catalogParameters.size()>maximumCatalogParameterEdits)return false;
    for(const auto& s:e.catalogSelections)if(s.design.isNull() || !s.revision || !catalogHashValid(s.contentSha256)
        || !known(s.destination.role,RackRole::fx) || s.destination.chain>4 || s.destination.position>8)return false;
    for(const auto& p:e.catalogParameters)if(p.key.isEmpty() || !textValid(p.key,rackParameterKeyBytes) || !std::isfinite(p.normalized) || p.normalized<0 || p.normalized>1)return false;
    return true;
}
bool writeCatalogEdits(Writer& w,const ProposalEdits& e)
{
    if(!catalogEditsTextValid(e))return false;
    w.integer(static_cast<std::uint32_t>(e.catalogSelections.size()));
    for(const auto& s:e.catalogSelections){w.integer(s.localId);w.uuid(s.design);w.integer(s.revision);w.text(s.contentSha256);w.enumeration(s.destination.role);w.integer(s.destination.chain);w.integer(s.destination.position);}
    w.integer(static_cast<std::uint32_t>(e.catalogParameters.size()));
    for(const auto& p:e.catalogParameters){w.integer(p.selection);w.integer(p.unit);w.text(p.key);w.real(p.normalized);}return w.good;
}
bool readCatalogEdits(Reader& r,ProposalEdits& e)
{
    std::uint32_t count;if(!r.count(count,maximumCatalogSelections,96))return false;e.catalogSelections.reserve(count);
    for(std::uint32_t i=0;i<count;++i){CatalogDesignSelection s;
        if(!r.integer(s.localId) || !r.uuid(s.design) || !r.integer(s.revision) || !r.text(s.contentSha256,64)
            || !r.enumeration(s.destination.role,RackRole::fx) || !r.integer(s.destination.chain) || !r.integer(s.destination.position))return false;
        e.catalogSelections.push_back(std::move(s));}
    if(!r.count(count,maximumCatalogParameterEdits,10))return false;e.catalogParameters.reserve(count);
    for(std::uint32_t i=0;i<count;++i){CatalogParameterEdit p;if(!r.integer(p.selection) || !r.integer(p.unit) || !r.text(p.key,rackParameterKeyBytes) || !r.real(p.normalized))return false;e.catalogParameters.push_back(std::move(p));}
    return catalogEditsTextValid(e);
}
bool writeCatalogRequest(Writer& w,const std::optional<CatalogRequestContext>& optional)
{
    if(!optional)return true;const auto& c=*optional;const auto& a=*c.authority;
    w.integer(c.version);w.enumeration(c.phase);w.tag(c.root);w.flag(c.selectionParent.has_value());if(c.selectionParent)w.tag(*c.selectionParent);
    w.uuid(a.snapshot);w.integer(c.inferenceCallsUsed);w.enumeration(c.providerStatus);w.integer(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(c.httpStatus)));
    w.text(std::string_view(a.descriptorJson));w.integer(static_cast<std::uint32_t>(a.definitions.size()));
    for(const auto& d:a.definitions){w.uuid(d.design);w.integer(d.revision);w.text(d.contentSha256);w.integer(static_cast<std::uint32_t>(d.canonical.getSize()));w.raw(d.canonical.getData(),d.canonical.getSize());}
    return writeEdits(w,c.baseIntent,a.rendering)&&writeCatalogEdits(w,c.baseIntent)&&w.good;
}
bool readCatalogRequest(Reader& r,std::optional<CatalogRequestContext>& optional,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    if(r.done())return true;CatalogRequestContext c;CatalogAuthority a;a.rendering=rendering;bool parent;std::uint32_t http,count;
    if(!r.integer(c.version) || c.version!=1 || !r.enumeration(c.phase,CatalogPhase::materialize) || !r.tag(c.root) || !tagValid(c.root) || !r.flag(parent))return false;
    if(parent){RequestTag tag;if(!r.tag(tag) || !tagValid(tag))return false;c.selectionParent=tag;}
    a.root=c.root;
    if(!r.uuid(a.snapshot) || a.snapshot.isNull() || !r.integer(c.inferenceCallsUsed) || c.inferenceCallsUsed>3
        || !r.enumeration(c.providerStatus,rack::oauth::Status::busy) || !r.integer(http) || !r.raw(a.descriptorJson,ai::maximumCatalogDescriptorBytes)
        || !r.count(count,maximumCatalogSelections,96))return false;
    c.httpStatus=std::bit_cast<std::int32_t>(http);if(!httpValid(c.httpStatus))return false;a.definitions.reserve(count);
    for(std::uint32_t i=0;i<count;++i){CatalogDefinition d;std::uint32_t length;WireBytes canonical;
        if(!r.uuid(d.design) || d.design.isNull() || !r.integer(d.revision) || !d.revision || !r.text(d.contentSha256,64) || !catalogHashValid(d.contentSha256)
            || !r.integer(length) || !length || length>65536 || !r.take(length,canonical))return false;
        d.canonical=juce::MemoryBlock(canonical.data(),canonical.size());a.definitions.push_back(std::move(d));}
    if(!readEdits(r,c.baseIntent,rendering) || !readCatalogEdits(r,c.baseIntent) || !ai::detail::catalogAuthorityValid(a))return false;
    c.authority=std::make_shared<const CatalogAuthority>(std::move(a));optional=std::move(c);return true;
}
bool writeCatalogBinding(Writer& w,const CatalogResultBinding& c)
{ w.integer<std::uint16_t>(1);w.tag(c.root);w.uuid(c.snapshot);return w.good; }
bool readCatalogBinding(Reader& r,std::optional<CatalogResultBinding>& optional)
{
    std::uint16_t version;CatalogResultBinding c;if(!r.integer(version) || version!=1 || !r.tag(c.root) || !tagValid(c.root) || !r.uuid(c.snapshot) || c.snapshot.isNull())return false;optional=c;return true;
}
bool sameCatalogIntent(const ProposalEdits& a,const ProposalEdits& b,CatalogRendering rendering=CatalogRendering::companionPcm)
{
    if(!editsTextValid(a) || !editsTextValid(b) || !catalogEditsTextValid(a) || !catalogEditsTextValid(b))return false;
    Writer x{{},maximumGenerationRequestBytes},y{{},maximumGenerationRequestBytes};juce::MemoryBlock xb,yb;
    return writeEdits(x,a,rendering)&&writeCatalogEdits(x,a)&&x.publish(xb)&&writeEdits(y,b,rendering)&&writeCatalogEdits(y,b)&&y.publish(yb)&&xb==yb;
}
bool audioValid(const PreviewAudio& a)
{
    if(a.sampleRate!=previewSampleRate || a.bodyFrames<1 || a.bodyFrames>maximumPreviewBodyFrames || a.left.size()!=a.bodyFrames+previewTailFrames
        || a.left.size()>maximumPreviewFrames || a.right.size()!=a.left.size() || !known(a.context.phraseSource,PhraseSource::defaultNotes)
        || !known(a.context.tempoSource,TempoSource::defaultTempo) || !bpmValid(a.context.bpm) || !validatePatternDocument(a.context.phrase))return false;
    for(std::size_t i=0;i<a.left.size();++i)if(!std::isfinite(a.left[i]) || std::abs(a.left[i])>=1 || !std::isfinite(a.right[i]) || std::abs(a.right[i])>=1)return false;return true;
}
bool noPayload(const GenerationResult& r)
{
    const auto& e=r.edits;const auto& a=r.audio;
    return !r.catalog && e.catalogSelections.empty() && e.catalogParameters.empty() && e.reconciliation.detach.empty()&&e.reconciliation.remove.empty()&&e.reconciliation.replace.empty()&&e.reconciliation.controls.empty() && !r.candidate && e.parameters.empty() && e.racks.empty() && e.macros.empty() && !e.pattern && e.characteristics.isEmpty() && e.explanation.isEmpty()
        && a.left.empty() && a.right.empty() && a.bodyFrames==0 && !a.phraseTruncated && a.sampleRate==previewSampleRate && a.context.phrase==PatternDocument{}
        && a.context.bpm==120 && a.context.phraseSource==PhraseSource::captured && a.context.tempoSource==TempoSource::captured;
}
bool resultValid(const GenerationRequest& captured,const GenerationResult& r,bool requireAudio=true,bool* allocationFailed=nullptr)
{
    if(r.tag!=captured.tag || !known(r.status,GenerationStatus::catalogSelectionRequired) || !known(r.proposalStatus,ProposalStatus::outOfReach) || !known(r.renderStatus,RenderStatus::renderFailure)
        || !known(r.providerStatus,rack::oauth::Status::busy) || r.inferenceCalls>3 || !httpValid(r.httpStatus) || !textValid(r.errorCode,256) || r.measurements.clippedSamples>1152000)return false;
    if(captured.catalog && captured.catalog->phase==CatalogPhase::materialize){const auto& c=*captured.catalog;
        if(r.inferenceCalls!=c.inferenceCallsUsed || r.providerStatus!=c.providerStatus || r.httpStatus!=c.httpStatus)return false;}
    const bool catalogResult=r.status==GenerationStatus::catalogSelectionRequired || r.status==GenerationStatus::ready;
    if(catalogResult && captured.catalog){const auto& c=*captured.catalog;
        if(!r.catalog || r.catalog->root!=c.root || r.catalog->snapshot!=c.authority->snapshot
            || (c.phase==CatalogPhase::materialize && !sameCatalogIntent(r.edits,c.baseIntent,c.authority->rendering)))return false;}
    else if(r.catalog || !r.edits.catalogSelections.empty() || !r.edits.catalogParameters.empty())return false;
    if(r.status==GenerationStatus::catalogSelectionRequired){
        if(!captured.catalog || captured.catalog->phase!=CatalogPhase::select || !r.catalog || !r.inferenceCalls
            || r.proposalStatus!=ProposalStatus::accepted || r.renderStatus!=RenderStatus::invalidInput || r.providerStatus!=rack::oauth::Status::connected || r.errorCode.isNotEmpty()
            || !editsTextValid(r.edits) || !catalogEditsTextValid(r.edits) || !ai::detail::catalogIntentValid(r.edits,captured.policy,*captured.catalog->authority))return false;
        bool missing=false;for(const auto& s:r.edits.catalogSelections){const auto& definitions=captured.catalog->authority->definitions;
            missing=missing || std::none_of(definitions.begin(),definitions.end(),[&](const auto& d){return d.design==s.design && d.revision==s.revision && d.contentSha256==s.contentSha256;});}
        auto empty=r;empty.edits={};empty.catalog.reset();const auto& m=r.measurements;
        const auto zero=[](double v){return std::bit_cast<std::uint64_t>(v)==0;};
        return missing && noPayload(empty) && m.finite && m.clippedSamples==0 && zero(m.peak) && zero(m.rms) && !m.onsetSeconds
            && zero(m.activityEndSeconds) && zero(m.decaySeconds) && std::all_of(m.bandFractions.begin(),m.bandFractions.end(),zero) && !m.tailActiveAtEnd;
    }
    if(r.status!=GenerationStatus::ready)return noPayload(r);
    if(r.inferenceCalls==0 || r.proposalStatus!=ProposalStatus::accepted || r.renderStatus!=RenderStatus::ready || r.providerStatus!=rack::oauth::Status::connected
        || r.errorCode.isNotEmpty() || !r.candidate || !validateProjectDocument(*r.candidate) || !editsTextValid(r.edits) || (requireAudio?(!audioValid(r.audio)||!measurementValid(r.measurements,r.audio.left.size())):(!r.audio.left.empty()||!r.audio.right.empty()||r.audio.bodyFrames||r.audio.phraseTruncated||r.audio.sampleRate!=previewSampleRate||r.audio.context.phrase!=PatternDocument{}||r.audio.context.bpm!=120||r.audio.context.phraseSource!=PhraseSource::captured||r.audio.context.tempoSource!=TempoSource::captured)))return false;
    ProjectDocument admitted;const auto admission=captured.catalog?admitProposal(captured.source,captured.policy,r.edits,*captured.catalog->authority,admitted,nullptr,allocationFailed):admitProposal(captured.source,captured.policy,r.edits,admitted,nullptr,allocationFailed);
    return admission==ProposalStatus::accepted&&admitted==*r.candidate;
}
bool readAudio(Reader& r,PreviewAudio& a)
{
    std::uint32_t body,frames;
    if(!r.integer(body) || body<1 || body>maximumPreviewBodyFrames || !r.flag(a.phraseTruncated) || !r.enumeration(a.context.phraseSource,PhraseSource::defaultNotes)
        || !r.enumeration(a.context.tempoSource,TempoSource::defaultTempo) || !r.real(a.context.bpm) || !bpmValid(a.context.bpm) || !r.pattern(a.context.phrase)
        || !r.integer(frames) || frames!=body+previewTailFrames || frames>maximumPreviewFrames || frames>r.remaining()/8)return false;
    a.bodyFrames=body;a.left.resize(frames);a.right.resize(frames);
    for(std::size_t i=0;i<frames;++i)if(!r.real(a.left[i]) || !r.real(a.right[i]) || !std::isfinite(a.left[i]) || std::abs(a.left[i])>=1 || !std::isfinite(a.right[i]) || std::abs(a.right[i])>=1)return false;return true;
}
bool accountValid(const AccountReply& r)
{
    if(!known(r.status,RuntimeStatus::modelNotConfigured) || !known(r.providerStatus,rack::oauth::Status::busy) || !httpValid(r.httpStatus) || !textValid(r.errorCode,256)
        || !modelValid(r.selectedModel,true) || (r.modelVerified && r.selectedModel.isEmpty()) || r.models.size()>128)return false;
    for(std::size_t i=0;i<r.models.size();++i){const auto& m=r.models[i];if(!modelValid(m.slug) || !textValid(m.displayName,1024))return false;for(std::size_t j=0;j<i;++j)if(r.models[j].slug==m.slug)return false;}
    if(r.accounts.size()>rack::oauth::maximumSavedAccounts||!modelValid(r.activeAccount,true))return false;
    bool active=r.activeAccount.isEmpty();for(std::size_t i=0;i<r.accounts.size();++i){const auto& a=r.accounts[i];
        if(!modelValid(a.id)||!textValid(a.label,512)||a.label.isEmpty())return false;
        if(a.id==r.activeAccount)active=true;for(std::size_t j=0;j<i;++j)if(r.accounts[j].id==a.id)return false;}
    return active;
}
bool commandValid(const AccountCommand& c) { return known(c.action,AccountAction::selectAccount) && (c.action==AccountAction::selectModel||c.action==AccountAction::selectAccount?modelValid(c.model):c.model.isEmpty()); }
bool readGroupUuid(Reader& r,GroupUuid& id)
{WireBytes b;if(!r.take(16,b))return false;std::memcpy(id.bytes.data(),b.data(),16);return true;}
void writeGroupUuid(Writer& w,const GroupUuid& id) {w.raw(id.getRawData(),16);}
bool readGroupText(Reader& r,GroupText& text)
{std::uint16_t length;WireBytes b;return r.integer(length)&&length<=maximumGroupTextBytes&&r.take(length,b)&&setGroupText({reinterpret_cast<const char*>(b.data()),b.size()},text);}
void writeGroupText(Writer& w,const GroupText& text) {w.integer(text.length);w.raw(text.bytes.data(),text.length);}
bool readGroupIntent(Reader& r,GroupIntent& intent)
{
    bool bpm;if(!readGroupUuid(r,intent.group)||!readGroupUuid(r,intent.member)||!readGroupText(r,intent.name)||!readGroupText(r,intent.role)
        ||!readGroupText(r,intent.key)||!readGroupText(r,intent.scale)||!r.flag(bpm))return false;
    if(bpm){double v;if(!r.real(v))return false;intent.bpm=v;}return validateGroupIntent(intent);
}
void writeGroupIntent(Writer& w,const GroupIntent& intent)
{writeGroupUuid(w,intent.group);writeGroupUuid(w,intent.member);writeGroupText(w,intent.name);writeGroupText(w,intent.role);writeGroupText(w,intent.key);writeGroupText(w,intent.scale);w.flag(intent.bpm.has_value());if(intent.bpm)w.real(*intent.bpm);}
bool readPlayback(Reader& r,PeerPlaybackObservation& p)
{
    auto& c=p.controls;auto& n=p.notes;
    if(!r.enumeration(c.availability,ObservedMidiAvailability::stale)||!r.enumeration(c.reason,LiveControlReason::clockRollback)||!r.integer(c.ageMs))return false;
    if(c.availability!=ObservedMidiAvailability::unavailable)for(auto& v:c.values)if(!r.real(v))return false;
    bool present;if(!r.flag(present))return false;
    if(present){PhraseMeter m;if(!r.integer(m.numerator)||!r.integer(m.denominator))return false;n.meter=m;}
    if(!r.flag(present))return false;if(present){double bpm;if(!r.real(bpm))return false;n.bpm=bpm;}
    if(!r.real(n.windowBeats)||!r.integer(n.ageMs)||!r.enumeration(n.availability,ObservedMidiAvailability::stale)
        ||!r.enumeration(n.reason,ObservedMidiReason::clockRollback)||!r.flag(n.truncated)||!r.integer(n.noteCount)
        ||n.noteCount>maximumObservedMidiNotes||n.noteCount>r.remaining()/20)return false;
    for(std::size_t i=0;i<n.noteCount;++i){auto& note=n.notes[i];if(!r.real(note.startBeats)||!r.real(note.durationBeats)||!r.integer(note.pitch)||!r.integer(note.velocity)||!r.integer(note.channel)||!r.flag(note.held))return false;}
    return true;
}
void writePlayback(Writer& w,const PeerPlaybackObservation& p)
{
    const auto& c=p.controls;const auto& n=p.notes;w.enumeration(c.availability);w.enumeration(c.reason);w.integer(c.ageMs);
    if(c.availability!=ObservedMidiAvailability::unavailable)for(auto v:c.values)w.real(v);
    w.flag(n.meter.has_value());if(n.meter){w.integer(n.meter->numerator);w.integer(n.meter->denominator);}
    w.flag(n.bpm.has_value());if(n.bpm)w.real(*n.bpm);w.real(n.windowBeats);w.integer(n.ageMs);w.enumeration(n.availability);w.enumeration(n.reason);w.flag(n.truncated);w.integer(n.noteCount);
    for(std::size_t i=0;i<n.noteCount;++i){const auto& note=n.notes[i];w.real(note.startBeats);w.real(note.durationBeats);w.integer(note.pitch);w.integer(note.velocity);w.integer(note.channel);w.flag(note.held);}
}
bool groupCommandValid(const GroupCommand& c)
{
    if(!c.operationId||c.runtime.isNull()||!known(c.action,GroupAction::spectrumWithdraw)||(c.action==GroupAction::browse?c.loadRevision!=0:c.loadRevision==0))return false;
    const bool publish=c.action==GroupAction::admit||c.action==GroupAction::publish;
    if(publish!=c.publication.has_value()||(c.action==GroupAction::validateProposal)!=c.proposal.has_value())return false;
    if(c.publication){const auto& p=*c.publication;if(p.runtime!=c.runtime||p.loadRevision!=c.loadRevision||!p.project.group
        ||(c.action==GroupAction::admit&&p.busy)||!validatePeerPlaybackObservation(p.playback,p.project))return false;}
    if((c.action==GroupAction::spectrumPublish)!=c.spectrum.has_value())return false;
    if(c.spectrum&&(!validateGroupSpectrum(*c.spectrum)||c.spectrum->runtime!=c.runtime||c.spectrum->loadRevision!=c.loadRevision))return false;
    if(c.spectrumPeers.size()>4||(c.action!=GroupAction::spectrumRead&&!c.spectrumPeers.empty()))return false;
    for(std::size_t i=0;i<c.spectrumPeers.size();++i)if(c.spectrumPeers[i].isNull()||std::find(c.spectrumPeers.begin(),c.spectrumPeers.begin()+i,c.spectrumPeers[i])!=c.spectrumPeers.begin()+i)return false;
    return !c.proposal||(tagValid(*c.proposal)&&GroupUuid(c.proposal->runtime)==c.runtime);
}
bool summaryValid(const GroupMemberSummary& m)
{
    if(m.runtime.isNull()||!m.loadRevision||!validateGroupIntent(m.intent)||!known(m.mode,PlaybackMode::internalPattern)
        ||!known(m.noteAvailability,ObservedMidiAvailability::stale)||!known(m.noteReason,ObservedMidiReason::clockRollback)||m.noteCount>maximumObservedMidiNotes
        ||!known(m.controlAvailability,ObservedMidiAvailability::stale)||!known(m.controlReason,LiveControlReason::clockRollback))return false;
    if(m.noteAvailability==ObservedMidiAvailability::fresh){if(!m.noteCount||m.noteReason!=ObservedMidiReason::none||m.noteAgeMs>observedMidiFreshnessMs)return false;}
    else if(m.noteAvailability==ObservedMidiAvailability::stale){if(!m.noteCount||(m.noteReason!=ObservedMidiReason::stopped&&m.noteReason!=ObservedMidiReason::stalePublisher&&m.noteReason!=ObservedMidiReason::clockRollback))return false;}
    else if(m.noteCount||m.noteReason==ObservedMidiReason::none)return false;
    if(m.controlAvailability==ObservedMidiAvailability::fresh)return m.controlReason==LiveControlReason::none&&m.controlAgeMs<=observedMidiFreshnessMs;
    if(m.controlAvailability==ObservedMidiAvailability::stale)return m.controlReason==LiveControlReason::stalePublisher||m.controlReason==LiveControlReason::clockRollback;
    return m.controlReason==LiveControlReason::notCaptured||m.controlReason==LiveControlReason::readConflict||m.controlReason==LiveControlReason::invalidRouting;
}
bool groupReplyValid(const GroupReply& r)
{
    if(!r.operationId||r.runtime.isNull()||!known(r.action,GroupAction::spectrumWithdraw)||!known(r.status,GroupReplyStatus::revisionExhausted)
        ||(r.action==GroupAction::browse?r.loadRevision!=0:r.loadRevision==0)||r.members.size()>16)return false;
    if((r.action==GroupAction::validateProposal)!=r.proposal.has_value()||(r.proposal&&(!tagValid(*r.proposal)||GroupUuid(r.proposal->runtime)!=r.runtime)))return false;
    const bool normalized=r.status==GroupReplyStatus::ready&&(r.action==GroupAction::admit||r.action==GroupAction::publish);
    if(normalized!=r.normalized.has_value())return false;
    if(normalized){if(!r.registryRevision||!validateGroupIntent(*r.normalized))return false;}
    else if(r.registryRevision||r.conflicted)return false;
    if((r.action!=GroupAction::browse||r.status!=GroupReplyStatus::ready)&&!r.members.empty())return false;
    if(r.spectra.size()>4||((r.action!=GroupAction::spectrumRead||r.status!=GroupReplyStatus::ready)&&!r.spectra.empty()))return false;
    for(std::size_t i=0;i<r.spectra.size();++i){const auto& value=r.spectra[i];if(!validateGroupSpectrum(value)||value.runtime==r.runtime)return false;for(std::size_t j=0;j<i;++j)if(value.runtime==r.spectra[j].runtime)return false;}
    for(std::size_t i=0;i<r.members.size();++i)
    {const auto& m=r.members[i];if(!summaryValid(m))return false;for(std::size_t j=0;j<i;++j){const auto& previous=r.members[j];if(m.runtime==previous.runtime||(m.intent.group==previous.intent.group&&m.intent.member==previous.intent.member))return false;}}
    return true;
}
bool readSummary(Reader& r,GroupMemberSummary& m)
{
    return readGroupUuid(r,m.runtime)&&r.integer(m.loadRevision)&&r.integer(m.editRevision)&&readGroupIntent(r,m.intent)
        &&r.enumeration(m.mode,PlaybackMode::internalPattern)&&r.flag(m.busy)&&r.flag(m.conflicted)
        &&r.enumeration(m.noteAvailability,ObservedMidiAvailability::stale)&&r.enumeration(m.noteReason,ObservedMidiReason::clockRollback)
        &&r.integer(m.noteCount)&&r.integer(m.noteAgeMs)&&r.flag(m.notesTruncated)
        &&r.enumeration(m.controlAvailability,ObservedMidiAvailability::stale)&&r.enumeration(m.controlReason,LiveControlReason::clockRollback)&&r.integer(m.controlAgeMs)&&summaryValid(m);
}
void writeSummary(Writer& w,const GroupMemberSummary& m)
{
    writeGroupUuid(w,m.runtime);w.integer(m.loadRevision);w.integer(m.editRevision);writeGroupIntent(w,m.intent);w.enumeration(m.mode);w.flag(m.busy);w.flag(m.conflicted);
    w.enumeration(m.noteAvailability);w.enumeration(m.noteReason);w.integer(m.noteCount);w.integer(m.noteAgeMs);w.flag(m.notesTruncated);w.enumeration(m.controlAvailability);w.enumeration(m.controlReason);w.integer(m.controlAgeMs);
}
bool parentValid(const std::optional<RequestTag>& parent,const RequestTag& child)
{return !parent||(tagValid(*parent)&&parent->runtime==child.runtime&&parent->editRevision==child.editRevision&&parent->contextRevision==child.contextRevision&&parent->requestId<child.requestId);}
bool groupGenerationValid(const GroupGenerationCommand& c)
{
    if(!requestValid(c.request)||!validateGroupSelection(c.selection)||!c.request.source.group||!parentValid(c.parentProposal,c.request.tag))return false;
    for(std::size_t i=0;i<c.selection.peerCount;++i)if(c.selection.members[i]==c.request.source.group->member)return false;return true;
}
}

std::size_t messagePayloadCap(MessageKind k) noexcept
{if(k==MessageKind::mcpRequest)return maximumControlPayloadBytes;if(k==MessageKind::mcpReply)return maximumResultPayloadBytes;if(k==MessageKind::spectrumCommand||k==MessageKind::spectrumReply)return maximumSpectrumPayloadBytes;if(k==MessageKind::result||k==MessageKind::resultLocal)return maximumResultPayloadBytes;if(k==MessageKind::generate||k==MessageKind::generateLocal)return maximumGenerationRequestBytes;if(k==MessageKind::generateGroup||k==MessageKind::groupCommand||k==MessageKind::generateGroupLocal||k==MessageKind::previewEvaluate||k==MessageKind::groupCommandLocal)return maximumControlPayloadBytes;return maximumSmallPayloadBytes;}
bool encodeGroupCommand(const GroupCommand& c,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(!groupCommandValid(c))return false;Writer w{{},spectrumAction(c.action)?maximumSpectrumPayloadBytes:c.publication?maximumControlPayloadBytes:maximumSmallPayloadBytes};w.integer(c.operationId);w.enumeration(c.action);writeGroupUuid(w,c.runtime);w.integer(c.loadRevision);
        w.flag(c.publication.has_value());if(c.publication){const auto& p=*c.publication;writeGroupUuid(w,p.runtime);w.integer(p.loadRevision);w.integer(p.editRevision);if(!w.project(p.project))return false;w.flag(p.busy);writePlayback(w,p.playback);}
        w.flag(c.proposal.has_value());if(c.proposal)w.tag(*c.proposal);
        if(c.spectrum){juce::MemoryBlock nested;if(!encodeGroupSpectrum(*c.spectrum,nested))return false;w.integer(std::uint16_t(nested.getSize()));w.raw(nested.getData(),nested.getSize());}
        if(c.action==GroupAction::spectrumRead){w.integer(std::uint8_t(c.spectrumPeers.size()));for(auto peer:c.spectrumPeers)writeGroupUuid(w,peer);}return w.publish(out);});
}
bool decodeGroupCommand(WireBytes b,GroupCommand& out)
{
    return recoverAllocation([&]{if(b.size()>maximumControlPayloadBytes)return false;Reader r{b};GroupCommand c;bool present;
        if(!r.integer(c.operationId)||!r.enumeration(c.action,GroupAction::spectrumWithdraw)||!readGroupUuid(r,c.runtime)||!r.integer(c.loadRevision)||!r.flag(present))return false;
        if(spectrumAction(c.action)&&b.size()>maximumSpectrumPayloadBytes)return false;
        if(present){GroupPublication p;if(!readGroupUuid(r,p.runtime)||!r.integer(p.loadRevision)||!r.integer(p.editRevision)||!r.project(p.project)||!r.flag(p.busy)||!readPlayback(r,p.playback))return false;c.publication=std::move(p);}
        if(!r.flag(present))return false;if(present){RequestTag tag;if(!r.tag(tag))return false;c.proposal=tag;}
        if(c.action==GroupAction::spectrumPublish){std::uint16_t size;WireBytes nested;GroupSpectrum value;if(!r.integer(size)||!r.take(size,nested)||!decodeGroupSpectrum(nested.data(),nested.size(),value))return false;c.spectrum=value;}
        if(c.action==GroupAction::spectrumRead){std::uint8_t count;if(!r.integer(count)||count>4)return false;for(int i=0;i<count;++i){GroupUuid peer;if(!readGroupUuid(r,peer))return false;c.spectrumPeers.push_back(peer);}}
        if(!r.done()||!groupCommandValid(c)||(!c.publication&&b.size()>maximumSmallPayloadBytes))return false;out=std::move(c);return true;});
}
bool encodeGroupReply(const GroupReply& reply,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(!groupReplyValid(reply))return false;Writer w{{},spectrumAction(reply.action)?maximumSpectrumPayloadBytes:maximumSmallPayloadBytes};w.integer(reply.operationId);w.enumeration(reply.action);writeGroupUuid(w,reply.runtime);w.integer(reply.loadRevision);w.enumeration(reply.status);
        w.flag(reply.normalized.has_value());if(reply.normalized){writeGroupIntent(w,*reply.normalized);w.integer(reply.registryRevision);w.flag(reply.conflicted);}
        w.integer(static_cast<std::uint8_t>(reply.members.size()));for(const auto& member:reply.members)writeSummary(w,member);
        w.flag(reply.proposal.has_value());if(reply.proposal)w.tag(*reply.proposal);
        if(reply.action==GroupAction::spectrumRead){w.integer(std::uint8_t(reply.spectra.size()));for(const auto& value:reply.spectra){juce::MemoryBlock nested;if(!encodeGroupSpectrum(value,nested))return false;w.integer(std::uint16_t(nested.getSize()));w.raw(nested.getData(),nested.getSize());}}return w.publish(out);});
}
bool decodeGroupReply(WireBytes b,GroupReply& out)
{
    return recoverAllocation([&]{if(b.size()>maximumSmallPayloadBytes)return false;Reader r{b};GroupReply reply;bool present;std::uint8_t count;
        if(!r.integer(reply.operationId)||!r.enumeration(reply.action,GroupAction::spectrumWithdraw)||!readGroupUuid(r,reply.runtime)||!r.integer(reply.loadRevision)||!r.enumeration(reply.status,GroupReplyStatus::revisionExhausted)||!r.flag(present))return false;
        if(spectrumAction(reply.action)&&b.size()>maximumSpectrumPayloadBytes)return false;
        if(present){GroupIntent intent;if(!readGroupIntent(r,intent)||!r.integer(reply.registryRevision)||!r.flag(reply.conflicted))return false;reply.normalized=intent;}
        if(!r.integer(count)||count>16||count>r.remaining()/99)return false;reply.members.reserve(count);
        for(std::size_t i=0;i<count;++i){GroupMemberSummary m;if(!readSummary(r,m))return false;reply.members.push_back(std::move(m));}
        if(!r.flag(present))return false;if(present){RequestTag tag;if(!r.tag(tag))return false;reply.proposal=tag;}
        if(reply.action==GroupAction::spectrumRead){if(!r.integer(count)||count>4)return false;for(int i=0;i<count;++i){std::uint16_t size;WireBytes nested;GroupSpectrum value;if(!r.integer(size)||!r.take(size,nested)||!decodeGroupSpectrum(nested.data(),nested.size(),value))return false;reply.spectra.push_back(value);}}
        if(!r.done()||!groupReplyValid(reply))return false;out=std::move(reply);return true;});
}
bool encodeGroupGeneration(const GroupGenerationCommand& c,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(!groupGenerationValid(c))return false;juce::MemoryBlock nested;if(!encodeRequest(c.request,nested))return false;
        Writer w{{},maximumControlPayloadBytes};w.tag(c.request.tag);w.integer(static_cast<std::uint32_t>(nested.getSize()));w.raw(nested.getData(),nested.getSize());w.integer(c.selection.loadRevision);w.integer(c.selection.peerCount);
        for(std::size_t i=0;i<c.selection.peerCount;++i)writeGroupUuid(w,c.selection.members[i]);w.flag(c.parentProposal.has_value());if(c.parentProposal)w.tag(*c.parentProposal);return w.publish(out);});
}
bool decodeGroupGeneration(WireBytes b,GroupGenerationCommand& out)
{
    return recoverAllocation([&]{if(b.size()>maximumControlPayloadBytes)return false;Reader r{b};RequestTag leading,nestedTag;std::uint32_t size;WireBytes nested;
        if(!r.tag(leading)||!tagValid(leading)||!r.integer(size)||size>maximumGenerationRequestBytes||r.remaining()<10||size>r.remaining()-10||!r.take(size,nested)||!peekResultTag(nested,nestedTag)||nestedTag!=leading)return false;
        GroupGenerationCommand c;bool parent;if(!r.integer(c.selection.loadRevision)||!r.integer(c.selection.peerCount)||c.selection.peerCount>c.selection.members.size())return false;
        for(std::size_t i=0;i<c.selection.peerCount;++i)if(!readGroupUuid(r,c.selection.members[i]))return false;
        if(!r.flag(parent))return false;if(parent){RequestTag tag;if(!r.tag(tag))return false;c.parentProposal=tag;}
        if(!r.done()||!validateGroupSelection(c.selection)||!parentValid(c.parentProposal,leading)||!decodeRequest(nested,c.request)||!groupGenerationValid(c))return false;out=std::move(c);return true;});
}
bool peekGenerationResultStatus(WireBytes b,ai::GenerationStatus& out)
{
    return recoverAllocation([&]{RequestTag tag;GenerationStatus status;if(b.size()<41||!peekResultTag(b,tag))return false;Reader r{b,40};if(!r.enumeration(status,GenerationStatus::catalogSelectionRequired))return false;out=status;return true;});
}
bool encodeFrame(const Frame& f,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(f.payload.getSize()>payloadCap(f.kind))return false;
        const FrameHeader h{f.kind,f.session,static_cast<std::uint32_t>(f.payload.getSize())};if(!frameHeaderValid(h) || !controlShape(f.kind,blockBytes(f.payload)))return false;
        Writer w{{},payloadCap(f.kind)+wireHeaderBytes};w.raw("ARS4",4);w.integer(wireVersion);w.integer(static_cast<std::uint16_t>(f.kind));w.integer(h.payloadBytes);w.uuid(f.session);w.raw(f.payload.getData(),f.payload.getSize());return w.publish(out);});
}
bool decodeFrameHeader(WireBytes b,FrameHeader& out)
{
    return recoverAllocation([&]{if(b.size()!=wireHeaderBytes || std::memcmp(b.data(),"ARS4",4)!=0)return false;
        Reader r{b,4};std::uint16_t version,kind;FrameHeader h;if(!r.integer(version) || version!=wireVersion || !r.integer(kind) || !r.integer(h.payloadBytes) || !r.uuid(h.session))return false;
        h.kind=static_cast<MessageKind>(kind);if(!frameHeaderValid(h) || !r.done())return false;out=h;return true;});
}
bool decodeFrame(WireBytes b,Frame& out)
{
    return recoverAllocation([&]{FrameHeader h;if(b.size()<wireHeaderBytes || !decodeFrameHeader(b.first(wireHeaderBytes),h) || b.size()-wireHeaderBytes!=h.payloadBytes || !controlShape(h.kind,b.subspan(wireHeaderBytes)))return false;
        Frame f{h.kind,h.session,juce::MemoryBlock(b.data()+wireHeaderBytes,h.payloadBytes)};out=std::move(f);return true;});
}
bool encodeControl(const ControlMessage& c,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(c.tag.runtime.isNull() || !known(c.status,RuntimeStatus::modelNotConfigured)||(c.capabilities&~knownCapabilityMask))return false;Writer w{{},45};w.tag(c.tag);w.enumeration(c.status);if(c.capabilities)w.integer(c.capabilities);return w.publish(out);});
}
bool decodeControl(WireBytes b,ControlMessage& out)
{
    return recoverAllocation([&]{if(b.size()!=41&&b.size()!=45)return false;Reader r{b};ControlMessage c;if(!r.tag(c.tag) || c.tag.runtime.isNull() || !r.enumeration(c.status,RuntimeStatus::modelNotConfigured))return false;if(b.size()==45&&(!r.integer(c.capabilities)||!c.capabilities||(c.capabilities&~knownCapabilityMask)))return false;if(!r.done())return false;out=std::move(c);return true;});
}
static bool encodeRequestWithMode(const GenerationRequest& request,juce::MemoryBlock& out,CatalogRendering rendering)
{
    return recoverAllocation([&]{if(!requestValid(request,rendering))return false;Writer w{{},maximumGenerationRequestBytes};w.tag(request.tag);w.enumeration(request.policy.scope);w.flag(request.policy.allowMacroChanges);w.flag(request.policy.allowSlotReuse);w.flag(request.policy.requirePattern);w.integer(request.structuralGeneration);
        w.text(request.model);w.text(std::string_view(request.instruction));w.text(std::string_view(request.role));w.text(std::string_view(request.key));w.text(std::string_view(request.scale));if(!w.project(request.source))return false;
        w.flag(request.capturedPhrase.has_value());if(request.capturedPhrase && !w.pattern(*request.capturedPhrase))return false;w.flag(request.capturedBpm.has_value());if(request.capturedBpm)w.real(*request.capturedBpm);
        w.integer(static_cast<std::uint32_t>(request.namesInUse.size()));for(const auto& n:request.namesInUse)w.text(n);return writeCatalogRequest(w,request.catalog)&&w.publish(out);});
}
static bool decodeRequestWithMode(WireBytes b,GenerationRequest& out,CatalogRendering rendering)
{
    return recoverAllocation([&]{if(b.size()>maximumGenerationRequestBytes)return false;(void)parameterRegistry();Reader r{b};GenerationRequest request;
        if(!r.tag(request.tag) || !tagValid(request.tag) || !r.enumeration(request.policy.scope,RequestScope::both) || !r.flag(request.policy.allowMacroChanges) || !r.flag(request.policy.allowSlotReuse) || !r.flag(request.policy.requirePattern) || !r.integer(request.structuralGeneration)
            || !r.text(request.model,256) || !modelValid(request.model) || !r.raw(request.instruction,8192) || request.instruction.empty() || !r.raw(request.role,128) || !r.raw(request.key,128) || !r.raw(request.scale,128) || !r.project(request.source))return false;
        bool present;if(!r.flag(present))return false;if(present){PatternDocument p;if(!r.pattern(p))return false;request.capturedPhrase=p;}
        if(!r.flag(present))return false;if(present){double bpm;if(!r.real(bpm) || !bpmValid(bpm))return false;request.capturedBpm=bpm;}
        std::uint32_t names;if(!r.count(names,ai::maximumNamesInUse,5))return false;request.namesInUse.resize(names);
        for(auto& n:request.namesInUse)if(!r.text(n,maximumSoundNameBytes))return false;
        if(!readCatalogRequest(r,request.catalog,rendering) || !r.done() || !requestValid(request,rendering))return false;out=std::move(request);return true;});
}
bool encodeRequest(const GenerationRequest& r,juce::MemoryBlock& out){return encodeRequestWithMode(r,out,CatalogRendering::companionPcm);}
bool decodeRequest(WireBytes b,GenerationRequest& out){return decodeRequestWithMode(b,out,CatalogRendering::companionPcm);}
bool peekResultTag(WireBytes b,RequestTag& out)
{
    return recoverAllocation([&]{if(b.size()<40 || b.size()>maximumResultPayloadBytes)return false;Reader r{b.first(40)};RequestTag tag;if(!r.tag(tag) || !tagValid(tag))return false;out=std::move(tag);return true;});
}
bool encodeResult(const GenerationRequest& captured,const GenerationResult& result,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(!requestValid(captured) || !resultValid(captured,result))return false;Writer w{{},maximumResultPayloadBytes};w.tag(result.tag);w.enumeration(result.status);w.enumeration(result.proposalStatus);w.enumeration(result.renderStatus);w.enumeration(result.providerStatus);
        w.integer(static_cast<std::uint8_t>(result.inferenceCalls));w.integer(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(result.httpStatus)));w.text(result.errorCode);
        if(result.status==GenerationStatus::catalogSelectionRequired)return writeCatalogBinding(w,*result.catalog)&&writeEdits(w,result.edits)&&writeCatalogEdits(w,result.edits)&&w.publish(out);
        writeMeasurements(w,result.measurements);
        if(result.status==GenerationStatus::ready){if(!w.project(*result.candidate) || !writeEdits(w,result.edits))return false;
            if(result.catalog && (!writeCatalogBinding(w,*result.catalog) || !writeCatalogEdits(w,result.edits)))return false;
            const auto& a=result.audio;w.integer(static_cast<std::uint32_t>(a.bodyFrames));w.flag(a.phraseTruncated);w.enumeration(a.context.phraseSource);w.enumeration(a.context.tempoSource);w.real(a.context.bpm);
            if(!w.pattern(a.context.phrase))return false;w.integer(static_cast<std::uint32_t>(a.left.size()));for(std::size_t i=0;i<a.left.size();++i){w.real(a.left[i]);w.real(a.right[i]);}}return w.publish(out);});
}
bool decodeResult(WireBytes b,const GenerationRequest& captured,GenerationResult& out)
{
    return recoverAllocation([&]{RequestTag tag;if(!peekResultTag(b,tag) || tag!=captured.tag || !requestValid(captured))return false;Reader r{b,40};GenerationResult result;result.tag=tag;std::uint8_t calls;std::uint32_t http;
        if(!r.enumeration(result.status,GenerationStatus::catalogSelectionRequired) || !r.enumeration(result.proposalStatus,ProposalStatus::outOfReach) || !r.enumeration(result.renderStatus,RenderStatus::renderFailure) || !r.enumeration(result.providerStatus,rack::oauth::Status::busy)
            || !r.integer(calls) || calls>3 || !r.integer(http) || !r.text(result.errorCode,256))return false;
        result.inferenceCalls=calls;result.httpStatus=std::bit_cast<std::int32_t>(http);if(!httpValid(result.httpStatus))return false;
        if(result.status==GenerationStatus::catalogSelectionRequired){
            if(!captured.catalog || !readCatalogBinding(r,result.catalog) || result.catalog->root!=captured.catalog->root || result.catalog->snapshot!=captured.catalog->authority->snapshot
                || !readEdits(r,result.edits) || !readCatalogEdits(r,result.edits) || !r.done() || !resultValid(captured,result))return false;
            out=std::move(result);return true;}
        if(!readMeasurements(r,result.measurements))return false;
        if(result.status==GenerationStatus::ready){if(calls==0 || result.proposalStatus!=ProposalStatus::accepted || result.renderStatus!=RenderStatus::ready || result.providerStatus!=rack::oauth::Status::connected || result.errorCode.isNotEmpty())return false;
            ProjectDocument candidate;if(!r.project(candidate) || !readEdits(r,result.edits))return false;result.candidate=candidate;
            if(captured.catalog){const auto& context=*captured.catalog;
                if(!readCatalogBinding(r,result.catalog) || result.catalog->root!=context.root || result.catalog->snapshot!=context.authority->snapshot || !readCatalogEdits(r,result.edits))return false;}
            ProjectDocument admitted;const auto admission=captured.catalog?admitProposal(captured.source,captured.policy,result.edits,*captured.catalog->authority,admitted):admitProposal(captured.source,captured.policy,result.edits,admitted);
            if(admission!=ProposalStatus::accepted || admitted!=candidate || !readAudio(r,result.audio))return false;}
        if(!r.done() || !resultValid(captured,result))return false;out=std::move(result);return true;});
}
bool encodeAccountCommand(const AccountCommand& c,juce::MemoryBlock& out)
{ return recoverAllocation([&]{if(!commandValid(c))return false;Writer w{{},maximumSmallPayloadBytes};w.enumeration(c.action);w.text(c.model);return w.publish(out);}); }
bool decodeAccountCommand(WireBytes b,AccountCommand& out)
{
    return recoverAllocation([&]{if(b.size()>maximumSmallPayloadBytes)return false;Reader r{b};AccountCommand c;if(!r.enumeration(c.action,AccountAction::selectAccount) || !r.text(c.model,256) || !r.done() || !commandValid(c))return false;out=std::move(c);return true;});
}
bool encodeAccountReply(const AccountReply& reply,juce::MemoryBlock& out)
{
    return recoverAllocation([&]{if(!accountValid(reply))return false;Writer w{{},maximumSmallPayloadBytes};w.enumeration(reply.status);w.enumeration(reply.providerStatus);w.integer(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(reply.httpStatus)));w.text(reply.errorCode);w.text(reply.selectedModel);w.flag(reply.modelVerified);
        w.integer(static_cast<std::uint32_t>(reply.models.size()));for(const auto& m:reply.models){w.text(m.slug);w.text(m.displayName);}
        w.text(reply.activeAccount);w.integer(std::uint32_t(reply.accounts.size()));for(const auto& a:reply.accounts){w.text(a.id);w.text(a.label);}return w.publish(out);});
}
bool decodeAccountReply(WireBytes b,AccountReply& out)
{
    return recoverAllocation([&]{if(b.size()>maximumSmallPayloadBytes)return false;Reader r{b};AccountReply reply;std::uint32_t http,n;
        if(!r.enumeration(reply.status,RuntimeStatus::modelNotConfigured) || !r.enumeration(reply.providerStatus,rack::oauth::Status::busy) || !r.integer(http) || !r.text(reply.errorCode,256) || !r.text(reply.selectedModel,256) || !r.flag(reply.modelVerified) || !r.count(n,128,9))return false;
        reply.httpStatus=std::bit_cast<std::int32_t>(http);if(!httpValid(reply.httpStatus) || !modelValid(reply.selectedModel,true) || (reply.modelVerified && reply.selectedModel.isEmpty()))return false;
        reply.models.reserve(n);for(std::uint32_t i=0;i<n;++i){rack::oauth::Model m;if(!r.text(m.slug,256) || !modelValid(m.slug) || !r.text(m.displayName,1024))return false;for(const auto& previous:reply.models)if(previous.slug==m.slug)return false;reply.models.push_back(std::move(m));}
        if(!r.text(reply.activeAccount,256)||!r.count(n,rack::oauth::maximumSavedAccounts,9))return false;
        reply.accounts.reserve(n);for(std::uint32_t i=0;i<n;++i){rack::oauth::SavedAccount a;if(!r.text(a.id,256)||!r.text(a.label,512))return false;reply.accounts.push_back(std::move(a));}
        if(!r.done() || !accountValid(reply))return false;out=std::move(reply);return true;});
}
namespace {
bool writeNested(Writer& w,const juce::MemoryBlock& value){w.integer(std::uint32_t(value.getSize()));w.raw(value.getData(),value.getSize());return w.good;}
bool readNested(Reader& r,WireBytes& value,std::size_t cap){std::uint32_t size;return r.integer(size)&&size<=cap&&r.take(size,value);}
bool localVersion(Reader& r){std::uint16_t version;return r.integer(version)&&version==1;}
void writeDigest(Writer& w,const PreviewDigest& value){w.raw(value.data(),value.size());}
bool readDigest(Reader& r,PreviewDigest& value){WireBytes bytes;if(!r.take(value.size(),bytes))return false;std::memcpy(value.data(),bytes.data(),value.size());return true;}
void writePreviewBinding(Writer& w,const PreviewBinding& b){
    w.tag(b.request);w.tag(b.root);w.uuid(b.catalogSnapshot);w.integer(b.job);w.integer(b.ticket);w.integer(b.structuralGeneration);w.integer(b.reset);w.integer(b.sourceWord);
    w.enumeration(b.phase);w.integer(b.providerAttempt);w.integer(b.renderOrdinal);writeDigest(w,b.sourceMap);writeDigest(w,b.stagedMap);writeDigest(w,b.candidate);writeDigest(w,b.context);
}
bool readPreviewBinding(Reader& r,PreviewBinding& b){
    return r.tag(b.request)&&r.tag(b.root)&&r.uuid(b.catalogSnapshot)&&r.integer(b.job)&&r.integer(b.ticket)&&r.integer(b.structuralGeneration)&&r.integer(b.reset)&&r.integer(b.sourceWord)
        &&r.enumeration(b.phase,CatalogPhase::materialize)&&r.integer(b.providerAttempt)&&r.integer(b.renderOrdinal)&&readDigest(r,b.sourceMap)&&readDigest(r,b.stagedMap)&&readDigest(r,b.candidate)&&readDigest(r,b.context)&&validPreviewBinding(b);
}
bool writeLocalContext(Writer& w,const LocalPreviewContext& context){
    juce::MemoryBlock map;if(!encodePreviewMap(context.sourceMap,map))return false;
    w.tag(context.root);w.uuid(context.catalogSnapshot);w.integer(context.reset);w.integer(context.sourceWord);writeDigest(w,context.sourceMapDigest);return writeNested(w,map);
}
bool readLocalContext(Reader& r,LocalPreviewContext& context){
    WireBytes map;return r.tag(context.root)&&r.uuid(context.catalogSnapshot)&&r.integer(context.reset)&&r.integer(context.sourceWord)&&readDigest(r,context.sourceMapDigest)
        &&readNested(r,map,maximumPreviewMapBytes)&&decodePreviewMap(map,context.sourceMap);
}
bool writePreviewContext(Writer& w,const AuditionContext& context){
    PreviewDigest digest;if(!digestPreviewContext(context,digest))return false;
    w.enumeration(context.phraseSource);w.enumeration(context.tempoSource);w.real(context.bpm);return w.pattern(context.phrase);
}
bool readPreviewContext(Reader& r,AuditionContext& context){
    return r.enumeration(context.phraseSource,PhraseSource::defaultNotes)&&r.enumeration(context.tempoSource,TempoSource::defaultTempo)&&r.real(context.bpm)&&bpmValid(context.bpm)&&r.pattern(context.phrase);
}
bool localBodyTag(WireBytes bytes,RequestTag& output,std::size_t cap){
    if(bytes.size()<42||bytes.size()>cap)return false;Reader reader{bytes};RequestTag tag;
    if(!localVersion(reader)||!reader.tag(tag)||!tagValid(tag))return false;output=tag;return true;
}
bool logicalPreviewMap(const ProjectDocument& project,const PreviewSampleMap& map){
    for(std::uint8_t bank=0;bank<4;++bank){juce::Uuid rack=juce::Uuid::null();std::uint8_t unit=0;
        if(project.sound.personalities)for(const auto& instance:project.sound.personalities->instances)for(const auto& source:instance.design.units)
            if(isSourceModule(source.module))if(const auto* binding=rackUnitBinding(instance,source.id);binding&&binding->slot==bank){rack=instance.id;unit=source.id;}
        if(map.banks[bank].rack!=rack||map.banks[bank].unit!=unit)return false;
    }
    return true;
}
bool localResultValid(const LocalGenerationRequest& captured,const LocalGenerationResult& result,bool* allocationFailed=nullptr){
    const auto& n=result.numeric;
    if(!requestValid(captured.request,CatalogRendering::ownedLocal)||!validLocalPreviewContext(captured)||!resultValid(captured.request,n,false,allocationFailed))return false;
    if(n.status!=GenerationStatus::ready)return !result.rendered;
    if(!result.rendered||!n.candidate)return false;
    const auto& receipt=*result.rendered;const auto& binding=receipt.binding;const auto& request=captured.request;const auto& context=captured.local;
    if(!validPreviewReceipt(receipt)||receipt.status!=RenderStatus::ready||binding.request!=request.tag||binding.root!=context.root
        ||binding.catalogSnapshot!=context.catalogSnapshot||binding.sourceMap!=context.sourceMapDigest||binding.reset!=context.reset
        ||binding.sourceWord!=context.sourceWord||binding.structuralGeneration!=request.structuralGeneration
        ||binding.phase!=(request.catalog?request.catalog->phase:CatalogPhase::select)||binding.providerAttempt!=n.inferenceCalls)return false;
    auto measured=receipt;measured.measurements=n.measurements;if(!samePreviewReceipt(measured,receipt))return false;
    auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
    const auto status=request.catalog?admitProposal(request.source,request.policy,n.edits,*request.catalog->authority,*candidate,trace.get(),allocationFailed):admitProposal(request.source,request.policy,n.edits,*candidate,trace.get(),allocationFailed);
    if(status!=ProposalStatus::accepted||*candidate!=*n.candidate)return false;
    PreviewSampleMap staged;PreviewDigest digest;
    if(!stagePreviewMap(context.sourceMap,*trace,staged)||!digestPreviewMap(staged,digest)||digest!=binding.stagedMap
        ||!digestPreviewCandidate(*candidate,digest)||digest!=binding.candidate)return false;
    const auto audition=previewContextFor(request,n.edits.pattern,&staged,&candidate->sound);
    if(!digestPreviewContext(audition,digest)||digest!=binding.context)return false;
    const auto frames=std::floor(patternLengthBeats(audition.phrase)*60/audition.bpm*previewSampleRate+.5);
    return receipt.bodyFrames==std::uint32_t(std::max(1.,std::min(frames,double(maximumPreviewBodyFrames))))
        &&receipt.phraseTruncated==(frames>maximumPreviewBodyFrames);
}
}
bool peekLocalResultTag(WireBytes bytes,RequestTag& output){return recoverAllocation([&]{return localBodyTag(bytes,output,maximumResultPayloadBytes);});}
bool peekPreviewEvaluateTag(WireBytes bytes,RequestTag& output){return recoverAllocation([&]{return localBodyTag(bytes,output,maximumControlPayloadBytes);});}
bool encodeLocalRequest(const LocalGenerationRequest& input,juce::MemoryBlock& output){
    return recoverAllocation([&]{if(!validLocalPreviewContext(input))return false;juce::MemoryBlock nested;
        if(!encodeRequestWithMode(input.request,nested,CatalogRendering::ownedLocal))return false;
        Writer w{{},maximumGenerationRequestBytes};w.integer<std::uint16_t>(1);w.tag(input.request.tag);
        return writeNested(w,nested)&&writeLocalContext(w,input.local)&&w.publish(output);});
}
bool decodeLocalRequest(WireBytes bytes,LocalGenerationRequest& output){
    return recoverAllocation([&]{if(bytes.size()>maximumGenerationRequestBytes)return false;Reader r{bytes};LocalGenerationRequest input;RequestTag leading,nestedTag;WireBytes nested;
        if(!localVersion(r)||!r.tag(leading)||!tagValid(leading)||!readNested(r,nested,maximumGenerationRequestBytes)||!peekResultTag(nested,nestedTag)||nestedTag!=leading)return false;
        if(!decodeRequestWithMode(nested,input.request,CatalogRendering::ownedLocal)||!readLocalContext(r,input.local)||!r.done()||!validLocalPreviewContext(input))return false;
        output=std::move(input);return true;});
}
bool encodeLocalGroupGeneration(const LocalGroupGenerationCommand& input,juce::MemoryBlock& output){
    return recoverAllocation([&]{const auto& request=input.input.request;
        if(!validateGroupSelection(input.selection)||!request.source.group||!parentValid(input.parentProposal,request.tag))return false;
        for(std::size_t i=0;i<input.selection.peerCount;++i)if(input.selection.members[i]==request.source.group->member)return false;
        juce::MemoryBlock nested;if(!encodeLocalRequest(input.input,nested))return false;
        Writer w{{},maximumControlPayloadBytes};w.integer<std::uint16_t>(1);w.tag(request.tag);if(!writeNested(w,nested))return false;
        w.integer(input.selection.loadRevision);w.integer(input.selection.peerCount);for(std::size_t i=0;i<input.selection.peerCount;++i)writeGroupUuid(w,input.selection.members[i]);
        w.flag(input.parentProposal.has_value());if(input.parentProposal)w.tag(*input.parentProposal);return w.publish(output);});
}
bool decodeLocalGroupGeneration(WireBytes bytes,LocalGroupGenerationCommand& output){
    return recoverAllocation([&]{if(bytes.size()>maximumControlPayloadBytes)return false;Reader r{bytes};LocalGroupGenerationCommand input;RequestTag leading,nestedTag;WireBytes nested;bool parent;
        if(!localVersion(r)||!r.tag(leading)||!tagValid(leading)||!readNested(r,nested,maximumGenerationRequestBytes)||!localBodyTag(nested,nestedTag,maximumGenerationRequestBytes)||nestedTag!=leading
            ||!r.integer(input.selection.loadRevision)||!r.integer(input.selection.peerCount)||input.selection.peerCount>input.selection.members.size())return false;
        for(std::size_t i=0;i<input.selection.peerCount;++i)if(!readGroupUuid(r,input.selection.members[i]))return false;
        if(!r.flag(parent))return false;if(parent){RequestTag tag;if(!r.tag(tag))return false;input.parentProposal=tag;}
        if(!r.done()||!decodeLocalRequest(nested,input.input)||!validateGroupSelection(input.selection)||!input.input.request.source.group||!parentValid(input.parentProposal,leading))return false;
        for(std::size_t i=0;i<input.selection.peerCount;++i)if(input.selection.members[i]==input.input.request.source.group->member)return false;
        output=std::move(input);return true;});
}
bool encodePreviewEvaluate(const PreviewEvaluate& value,juce::MemoryBlock& output){
    return recoverAllocation([&]{PreviewDigest context;if(!validPreviewBinding(value.binding)||!digestPreviewContext(value.context,context)||context!=value.binding.context)return false;
        Writer w{{},maximumControlPayloadBytes};w.integer<std::uint16_t>(1);writePreviewBinding(w,value.binding);
        return writeEdits(w,value.edits,CatalogRendering::ownedLocal)&&writeCatalogEdits(w,value.edits)&&writePreviewContext(w,value.context)&&w.publish(output);});
}
bool decodePreviewEvaluate(WireBytes bytes,PreviewEvaluate& output){
    return recoverAllocation([&]{if(bytes.size()>maximumControlPayloadBytes)return false;Reader r{bytes};PreviewEvaluate value;PreviewDigest context;
        if(!localVersion(r)||!readPreviewBinding(r,value.binding)||!readEdits(r,value.edits,CatalogRendering::ownedLocal)||!readCatalogEdits(r,value.edits)
            ||!readPreviewContext(r,value.context)||!r.done()||!digestPreviewContext(value.context,context)||context!=value.binding.context)return false;
        output=std::move(value);return true;});
}
bool encodePreviewReceipt(const PreviewReceipt& value,juce::MemoryBlock& output){
    return recoverAllocation([&]{if(!validPreviewReceipt(value))return false;Writer w{{},maximumSmallPayloadBytes};w.integer<std::uint16_t>(1);writePreviewBinding(w,value.binding);
        w.enumeration(value.status);w.enumeration(value.failure);writeMeasurements(w,value.measurements);w.integer(value.bodyFrames);w.integer(value.frames);w.flag(value.phraseTruncated);return w.publish(output);});
}
bool decodePreviewReceipt(WireBytes bytes,PreviewReceipt& output){
    return recoverAllocation([&]{if(bytes.size()>maximumSmallPayloadBytes)return false;Reader r{bytes};PreviewReceipt value;
        if(!localVersion(r)||!readPreviewBinding(r,value.binding)||!r.enumeration(value.status,RenderStatus::renderFailure)||!r.enumeration(value.failure,PreviewFailure::invalidBinding)
            ||!readMeasurements(r,value.measurements)||!r.integer(value.bodyFrames)||!r.integer(value.frames)||!r.flag(value.phraseTruncated)||!r.done()||!validPreviewReceipt(value))return false;
        output=std::move(value);return true;});
}
bool encodeLocalResult(const LocalGenerationRequest& captured,const LocalGenerationResult& result,juce::MemoryBlock& output){
    return recoverAllocation([&]{if(!localResultValid(captured,result))return false;const auto& n=result.numeric;Writer w{{},maximumResultPayloadBytes};w.integer<std::uint16_t>(1);
        w.tag(n.tag);w.enumeration(n.status);w.enumeration(n.proposalStatus);w.enumeration(n.renderStatus);w.enumeration(n.providerStatus);w.integer(std::uint8_t(n.inferenceCalls));
        w.integer(std::bit_cast<std::uint32_t>(std::int32_t(n.httpStatus)));w.text(n.errorCode);writeMeasurements(w,n.measurements);
        w.flag(n.candidate.has_value());if(n.candidate&&!w.project(*n.candidate))return false;
        if(!writeEdits(w,n.edits,CatalogRendering::ownedLocal)||!writeCatalogEdits(w,n.edits))return false;
        w.flag(n.catalog.has_value());if(n.catalog&&!writeCatalogBinding(w,*n.catalog))return false;
        w.flag(result.rendered.has_value());if(result.rendered){juce::MemoryBlock nested;if(!encodePreviewReceipt(*result.rendered,nested)||!writeNested(w,nested))return false;}
        return w.publish(output);});
}
bool decodeLocalResult(WireBytes bytes,const LocalGenerationRequest& captured,LocalGenerationResult& output){
    return recoverAllocation([&]{RequestTag tag;if(!peekLocalResultTag(bytes,tag)||tag!=captured.request.tag||!requestValid(captured.request,CatalogRendering::ownedLocal)||!validLocalPreviewContext(captured))return false;
        Reader r{bytes,42};LocalGenerationResult result;auto& n=result.numeric;n.tag=tag;std::uint8_t calls;std::uint32_t http;bool present;
        if(!r.enumeration(n.status,GenerationStatus::catalogSelectionRequired)||!r.enumeration(n.proposalStatus,ProposalStatus::outOfReach)||!r.enumeration(n.renderStatus,RenderStatus::renderFailure)
            ||!r.enumeration(n.providerStatus,rack::oauth::Status::busy)||!r.integer(calls)||calls>3||!r.integer(http)||!r.text(n.errorCode,256)||!readMeasurements(r,n.measurements)||!r.flag(present))return false;
        n.inferenceCalls=calls;n.httpStatus=std::bit_cast<std::int32_t>(http);if(!httpValid(n.httpStatus))return false;
        if(present){if(n.status!=GenerationStatus::ready)return false;ProjectDocument candidate;if(!r.project(candidate))return false;n.candidate=std::move(candidate);}
        if(!readEdits(r,n.edits,CatalogRendering::ownedLocal)||!readCatalogEdits(r,n.edits)||!r.flag(present))return false;
        if(present&&!readCatalogBinding(r,n.catalog))return false;
        if(!r.flag(present))return false;if(present){WireBytes nested;PreviewReceipt receipt;if(!readNested(r,nested,maximumSmallPayloadBytes)||!decodePreviewReceipt(nested,receipt))return false;result.rendered=receipt;}
        if(!r.done()||!localResultValid(captured,result))return false;output=std::move(result);return true;});
}
bool encodeLocalGroupPublication(const GroupCommand& command,const PreviewSampleMap& map,juce::MemoryBlock& output){
    return recoverAllocation([&]{if((command.action!=GroupAction::admit&&command.action!=GroupAction::publish)||!command.publication||!logicalPreviewMap(command.publication->project,map))return false;
        juce::MemoryBlock nested,mapping;if(!encodeGroupCommand(command,nested)||!encodePreviewMap(map,mapping))return false;Writer w{{},maximumControlPayloadBytes};w.integer<std::uint16_t>(1);
        return writeNested(w,nested)&&writeNested(w,mapping)&&w.publish(output);});
}
bool decodeLocalGroupPublication(WireBytes bytes,GroupCommand& command,PreviewSampleMap& map){
    return recoverAllocation([&]{if(bytes.size()>maximumControlPayloadBytes)return false;Reader r{bytes};GroupCommand local;PreviewSampleMap mapping;WireBytes nested,encodedMap;
        if(!localVersion(r)||!readNested(r,nested,maximumControlPayloadBytes)||!readNested(r,encodedMap,maximumPreviewMapBytes)||!r.done()||!decodeGroupCommand(nested,local)
            ||(local.action!=GroupAction::admit&&local.action!=GroupAction::publish)||!local.publication||!decodePreviewMap(encodedMap,mapping)||!logicalPreviewMap(local.publication->project,mapping))return false;
        command=std::move(local);map=std::move(mapping);return true;});
}

}
namespace airack::ai {
bool validateGenerationResult(const GenerationRequest& request,const GenerationResult& result){
    return companion::recoverAllocation([&]{const auto mode=request.catalog&&request.catalog->authority?request.catalog->authority->rendering:CatalogRendering::ownedLocal;return companion::requestValid(request,mode)&&companion::resultValid(request,result);});
}
bool validateLocalGenerationResult(const LocalGenerationRequest& request,const LocalGenerationResult& result,bool* allocationFailed){
    if(allocationFailed)*allocationFailed=false;
    try{return companion::localResultValid(request,result,allocationFailed);}
    catch(const std::bad_alloc&){if(allocationFailed)*allocationFailed=true;return false;}
    catch(const std::length_error&){return false;}
}
bool joinLocalPreview(const LocalGenerationRequest& request,const LocalGenerationResult& result,const PreviewReceipt& retained,PreviewAudio&& audio,GenerationResult& destination){
    return companion::recoverAllocation([&]{PreviewDigest context;if(!companion::localResultValid(request,result)||result.numeric.status!=GenerationStatus::ready||!result.rendered
            ||!samePreviewReceipt(*result.rendered,retained)||!companion::audioValid(audio)||audio.bodyFrames!=retained.bodyFrames||audio.left.size()!=retained.frames||audio.phraseTruncated!=retained.phraseTruncated
            ||!digestPreviewContext(audio.context,context)||context!=retained.binding.context)return false;
        GenerationResult joined=result.numeric;joined.audio=std::move(audio);destination=std::move(joined);return true;});
}
}
