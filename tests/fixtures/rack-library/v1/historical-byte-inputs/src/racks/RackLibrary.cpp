#include "RackLibrary.h"
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include <algorithm>
#include <juce_cryptography/juce_cryptography.h>
#include <limits>

namespace airack {
bool saveRackDesign(const RackDesign& design,const juce::File& target)
{
    try {
        juce::MemoryBlock encoded;if(!encodeRackDesign(design,encoded))return false;
        juce::TemporaryFile temporary(target);
        {
            auto stream=temporary.getFile().createOutputStream();
            if(!stream||!stream->write(encoded.getData(),encoded.getSize()))return false;
            stream->flush();if(stream->getStatus().failed())return false;
        }
        return temporary.overwriteTargetFileWithTemporary();
    }catch(...){return false;}
}
bool loadRackDesign(const juce::File& file,RackDesign& output)
{
    try {
        juce::FileInputStream stream(file);if(!stream.openedOk())return false;
        const auto size=stream.getTotalLength();if(size<1||size>65536)return false;
        juce::MemoryBlock bytes(static_cast<std::size_t>(size)+1);
        const auto read=stream.read(bytes.getData(),static_cast<int>(bytes.getSize()));
        return read==size&&decodeRackDesign(bytes.getData(),static_cast<std::size_t>(size),output);
    }catch(...){return false;}
}
bool captureGenericRackDesign(const SoundDocument& sound,RackRole role,std::span<const UnitBinding> selected,
    const RackPanel& panel,juce::Uuid id,const juce::String& name,const juce::String& intent,RackDesign& output)
{
    try {
        if(!validateSoundDocument(sound)||selected.empty()||selected.size()>9||role>RackRole::fx)return false;
        RackInstance instance;auto& design=instance.design;design.id=id;design.name=name;design.intent=intent;design.role=role;design.panel=panel;
        instance.bindings.assign(selected.begin(),selected.end());
        for(std::size_t n=0;n<selected.size();++n){
            const auto& b=selected[n];const bool isSynth=role==RackRole::synth&&n==0;
            if(b.unit>8||b.slot>=(isSynth?4:8))return false;
            if(isSynth?sound.layout.synthUse[b.slot]!=SlotUse::present:sound.layout.effects[b.slot].use!=SlotUse::present)return false;
            const auto module=isSynth?sourceModule(sound,b.slot)
                :nativeModuleForEffect(sound.layout.effects[b.slot].kind);
            RackUnit unit;unit.id=b.unit;unit.module=module;
            unit.algorithmRevision=rackAlgorithmRevision(sound.compatibility,module,b.slot);
            const auto prefix=juce::String(isSynth?"synth.":"effect.")+juce::String(b.slot+1)+".";
            const auto registry=parameterRegistry();
            for(std::size_t p=0;p<registry.size();++p)if(registry[p].id.startsWith(prefix)){
                const auto key=registry[p].id.substring(prefix.length());
                if(rackParameterActive(module,key))unit.defaults.push_back({b.unit,key,sound.values[p]});
            }
            design.units.push_back(std::move(unit));
        }
        if(!validateRackDesign(design,RackValidationMode::portableDesign))return false;
        // One common ownership validator enforces presence, uniqueness,
        // contiguity/prefix and unowned selection without changing live sound.
        auto state=std::make_shared<RackPersonalityState>();if(sound.personalities)*state=*sound.personalities;
        state->instances.push_back(instance);auto candidate=sound;candidate.personalities=std::move(state);
        if(!validateRackInstances(candidate))return false;
        output=std::move(design);return true;
    }catch(...){return false;}
}
}

namespace airack {
namespace {
constexpr std::size_t individualBytes=65536,aggregateBytes=maximumRackCatalogBytes,scratchBytes=20971520;
constexpr int decimals=std::numeric_limits<double>::max_digits10+5;
bool fail(RackCatalogStatus* status,RackCatalogStatus value){if(status)*status=value;return false;}
bool add(std::size_t& value,std::size_t extra){if(extra>std::numeric_limits<std::size_t>::max()-value)return false;value+=extra;return true;}
juce::String fresh(const juce::String& text){return juce::String::fromUTF8(text.toRawUTF8(),text.getNumBytesAsUTF8());}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields){auto* o=new juce::DynamicObject;for(const auto& f:fields)o->setProperty(f.first,f.second);return o;}
struct CountStream final : juce::OutputStream {
    explicit CountStream(std::size_t cap):ceiling(cap){}
    std::size_t count=0,ceiling;bool valid=true;
    void flush() override{} bool setPosition(juce::int64 n) override{return n==juce::int64(count);}
    juce::int64 getPosition() override{return juce::int64(count);}
    bool write(const void*,std::size_t n) override{if(n>ceiling-count){valid=false;return false;}count+=n;return true;}
};
bool canonicalValue(const RackDesign& d,RackValidationMode mode,juce::var& value,std::size_t& size){
    if(!encodeRackDesignObject(d,value,mode))return false;
    CountStream count(individualBytes);juce::JSON::writeToStream(count,value,true,decimals);if(!count.valid)return false;
    size=count.count;return true;
}
bool canonicalBacking(const juce::var& value,std::size_t size,juce::MemoryBlock& result){
    juce::MemoryBlock bytes(size);juce::MemoryOutputStream stream(bytes.getData(),bytes.getSize());
    juce::JSON::writeToStream(stream,value,true,decimals);if(stream.getDataSize()!=size)return false;result=std::move(bytes);return true;
}
bool canonical(const RackDesign& d,RackValidationMode mode,std::size_t budget,juce::MemoryBlock& result){
    juce::var value;std::size_t size;
    return canonicalValue(d,mode,value,size)&&size<=budget&&canonicalBacking(value,size,result);
}
RackDesign owned(const RackDesign& source){
    RackDesign d;d.id=source.id;d.revision=source.revision;d.name=fresh(source.name);d.intent=fresh(source.intent);d.role=source.role;
    d.units=std::vector<RackUnit>(source.units.size());
    for(std::size_t n=0;n<source.units.size();++n){const auto& in=source.units[n];auto& out=d.units[n];out.id=in.id;out.module=in.module;out.algorithmRevision=in.algorithmRevision;
        out.defaults=std::vector<LocalParameter>(in.defaults.size());for(std::size_t p=0;p<in.defaults.size();++p){out.defaults[p]=in.defaults[p];out.defaults[p].key=fresh(in.defaults[p].key);}}
    d.panel.layout=source.panel.layout;d.panel.surface=source.panel.surface;d.panel.accent=source.panel.accent;d.panel.frame=source.panel.frame;
    d.panel.emphasis=source.panel.emphasis;d.panel.spacing=source.panel.spacing;d.panel.density=source.panel.density;
    d.panel.sections=std::vector<juce::String>(source.panel.sections.size());
    for(std::size_t n=0;n<d.panel.sections.size();++n)d.panel.sections[n]=fresh(source.panel.sections[n]);
    d.panel.controls=std::vector<FocusedControl>(source.panel.controls.size());for(std::size_t n=0;n<d.panel.controls.size();++n){d.panel.controls[n]=source.panel.controls[n];d.panel.controls[n].label=fresh(source.panel.controls[n].label);d.panel.controls[n].key=fresh(source.panel.controls[n].key);}
    return d;
}
constexpr std::size_t proxyBytes=
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL == 2
    16;
#else
    0;
#endif
bool vectorCharge(std::size_t& charge,std::size_t capacity,std::size_t item){
    if(capacity>std::numeric_limits<std::size_t>::max()/item)return false;const auto bytes=capacity*item;
    return add(charge,bytes)&&add(charge,proxyBytes)&&add(charge,bytes>=4096?47:0);
}
bool stringCharge(std::size_t& charge,const juce::String& text){return add(charge,23+((std::size_t(text.getNumBytesAsUTF8())+4)&~std::size_t(3)));}
bool entryCharge(const RackDesign& d,std::size_t hashBytes,std::size_t& charge,bool normalized=false){
    const auto extent=[&](const auto& values){return normalized?values.size():values.capacity();};
    charge=sizeof(RackCatalogEntry);
    if(!vectorCharge(charge,extent(d.units),sizeof(RackUnit))||!vectorCharge(charge,extent(d.panel.sections),sizeof(juce::String))||!vectorCharge(charge,extent(d.panel.controls),sizeof(FocusedControl))
        ||!stringCharge(charge,d.name)||!stringCharge(charge,d.intent)||!add(charge,23+((hashBytes+4)&~std::size_t(3))))return false;
    for(const auto& u:d.units){if(!vectorCharge(charge,extent(u.defaults),sizeof(LocalParameter)))return false;for(const auto& p:u.defaults)if(!stringCharge(charge,p.key))return false;}
    for(const auto& s:d.panel.sections)if(!stringCharge(charge,s))return false;
    for(const auto& c:d.panel.controls)if(!stringCharge(charge,c.label)||!stringCharge(charge,c.key))return false;return true;
}
bool prepared(std::size_t entries,std::size_t descriptors,std::size_t ceiling,std::size_t factoryCredit=0,std::size_t reservedSources=0){
    // ponytail: one replacement/page, twelve resolved copies and serial scratch.
    // Admission reserves the reviewed no-sharing upper shape before ownership;
    // actual freshly normalised capacities are checked against it afterward.
    std::size_t total=scratchBytes+205+2048+2928+134368+131074+262144+12*71774;
    if(entries>maximumRackCatalogEntries||entries>std::numeric_limits<std::size_t>::max()/(3*72001))return false;
    // Retain the prior conservative reservation through71 entries. At72 only,
    // factory definitions have already been validated and charged at their owned shape.
    auto ownership=entries*72001;
    if(entries>71){
        if(factoryCredit>ownership)return false;
        ownership=std::max(ownership-factoryCredit,std::size_t(71)*72001);
    }
    return add(total,ownership*3)&&add(total,descriptors)&&add(total,(reservedSources?reservedSources:entries)*27)&&total<=ceiling;
}
juce::String seal(const RackCatalogSnapshot& s,std::span<const std::uint8_t> native){
    if(s.entries.size()>maximumRackCatalogEntries||native.size()!=s.entries.size())return {};
    for(std::size_t n=0;n<s.entries.size();++n)if(native[n]>1||s.entries[n].contentSha256.length()!=64
        ||!s.entries[n].contentSha256.containsOnly("0123456789abcdef"))return {};
    juce::MemoryOutputStream stream;stream.write(s.id.getRawData(),16);
    for(std::size_t n=0;n<s.entries.size();++n){const auto& e=s.entries[n];stream.write(e.definition.id.getRawData(),16);stream.writeInt64(juce::int64(e.definition.revision));
        stream.write(e.contentSha256.toRawUTF8(),64);stream.writeByte(char(native[n]));}
    return juce::SHA256(stream.getData(),stream.getDataSize()).toHexString();
}
}
bool rackCatalogEscapedSize(const juce::var& value,std::size_t ceiling,std::size_t& output){
    try{if(ceiling>aggregateBytes)return false;CountStream stream(ceiling);juce::JSON::writeToStream(stream,value,true,decimals);if(!stream.valid)return false;output=stream.count;return true;}catch(...){return false;}
}
juce::var rackCatalogDescriptor(const RackCatalogEntry& e){
    juce::Array<juce::var> units;int synths=0,effects=0;
    for(const auto& u:e.definition.units){juce::Array<juce::var> parameters;
        for(const auto& p:u.defaults){const auto index=rackParameterIndex(u.module,0,p.key);if(!index)return {};
            const auto& field=parameterRegistry()[*index];parameters.add(juce::Array<juce::var>{p.key,field.unit,double(field.minimum),double(field.maximum)});}
        isSourceModule(u.module)?++synths:++effects;
        units.add(object({{"id",int(u.id)},{"module",int(u.module)},{"algorithm_revision",int(u.algorithmRevision)},{"parameters",parameters}}));}
    return object({{"id",e.definition.id.toString()},{"revision",juce::String(std::to_string(e.definition.revision))},{"content_sha256",e.contentSha256},
        {"name",e.definition.name},{"intent",e.definition.intent},{"role",e.definition.role==RackRole::synth?"synth":"fx"},
        {"capacity",object({{"synths",synths},{"effects",effects}})},{"parameter_columns",juce::Array<juce::var>{"key","unit","minimum","maximum"}},{"units",units}});
}
juce::var rackCatalogPageDescriptor(const RackCatalogPage& page){
    juce::Array<juce::var> entries;for(const auto& entry:page.entries)entries.add(rackCatalogDescriptor(entry));
    return object({{"snapshot",page.snapshot.toString()},{"entries",entries},{"next",juce::String(std::to_string(page.next))},{"complete",page.complete}});
}
bool captureRackCatalog(std::span<const RackDesign> factory,std::span<const RackDesign> embedded,
    const RackCatalogCaptureLimits& limits,RackCatalogSnapshot& output,RackCatalogStatus* status){
    try{
        std::size_t sources=factory.size();if(!add(sources,embedded.size()))return fail(status,RackCatalogStatus::overflow);
        if(sources>limits.maximumSources||sources>maximumRackCatalogEntries||limits.maximumEntries>maximumRackCatalogEntries
            ||limits.maximumSources>maximumRackCatalogEntries)return fail(status,RackCatalogStatus::countLimit);
        if(limits.maximumDefinitionBytes>aggregateBytes||limits.maximumSourceBytes>aggregateBytes||limits.maximumDescriptorBytes>aggregateBytes)return fail(status,RackCatalogStatus::byteLimit);
        std::size_t factoryCredit=0;
        if(sources>71)for(const auto& design:factory){
            std::size_t charge;
            if(!validateRackDesign(design,RackValidationMode::instanceRecall))return fail(status,RackCatalogStatus::invalidSource);
            if(!entryCharge(design,64,charge,true)||charge>72001||!add(factoryCredit,72001-charge))return fail(status,RackCatalogStatus::preparedLimit);
        }
        if(sizeof(juce::var)>16||sizeof(juce::String)>8||sizeof(RackCatalogEntry)>304||sizeof(RackCatalogSnapshot)>512||sizeof(RackCatalogPage)>512||JUCE_STRING_UTF_TYPE!=8
            ||limits.maximumPreparedBytes>41943040||!prepared(std::min(sources,limits.maximumEntries),limits.maximumDescriptorBytes,limits.maximumPreparedBytes,factoryCredit,std::min(sources,limits.maximumEntries)))return fail(status,RackCatalogStatus::preparedLimit);
        RackCatalogSnapshot candidate;candidate.entries.reserve(std::min(sources,limits.maximumEntries));candidate.native.reserve(std::min(sources,limits.maximumEntries));
        std::size_t sourceBytes=0,definitionBytes=0,descriptorBytes=0;
        auto accept=[&](const RackDesign& input)->bool{
            constexpr auto mode=RackValidationMode::instanceRecall;
            if(!validateRackDesign(input,mode))return fail(status,RackCatalogStatus::invalidSource);
            if(!prepared(candidate.entries.size()+1,limits.maximumDescriptorBytes,limits.maximumPreparedBytes,factoryCredit,std::min(sources,limits.maximumEntries)))return fail(status,RackCatalogStatus::preparedLimit);
            const auto existing=std::find_if(candidate.entries.begin(),candidate.entries.end(),[&](const auto& e){return e.definition.id==input.id&&e.definition.revision==input.revision;});
            if(existing==candidate.entries.end()&&candidate.entries.size()==limits.maximumEntries)return fail(status,RackCatalogStatus::countLimit);
            juce::var value;std::size_t extent;if(!canonicalValue(input,mode,value,extent))return fail(status,RackCatalogStatus::invalidSource);
            if(extent>limits.maximumSourceBytes-sourceBytes)return fail(status,RackCatalogStatus::byteLimit);
            if(existing==candidate.entries.end()&&extent>limits.maximumDefinitionBytes-definitionBytes)return fail(status,RackCatalogStatus::byteLimit);
            juce::MemoryBlock bytes;if(!canonicalBacking(value,extent,bytes))return fail(status,RackCatalogStatus::invalidSource);
            sourceBytes+=extent;
            const auto hash=juce::SHA256(bytes).toHexString();
            if(existing!=candidate.entries.end()){
                if(existing->contentSha256!=hash)return fail(status,RackCatalogStatus::identityConflict);return true;}
            if(candidate.entries.size()==limits.maximumEntries)return fail(status,RackCatalogStatus::countLimit);
            if(bytes.getSize()>limits.maximumDefinitionBytes-definitionBytes)return fail(status,RackCatalogStatus::byteLimit);
            std::size_t precharge;
            if(!entryCharge(input,64,precharge,true)||precharge>72001)return fail(status,RackCatalogStatus::preparedLimit);
            RackCatalogEntry entry{owned(input),fresh(hash)};std::size_t charge;
            if(!entryCharge(entry.definition,64,charge)||charge>precharge||charge>72001)return fail(status,RackCatalogStatus::preparedLimit);
            std::size_t size;if(!rackCatalogEscapedSize(rackCatalogDescriptor(entry),limits.maximumDescriptorBytes-descriptorBytes,size))return fail(status,RackCatalogStatus::byteLimit);
            definitionBytes+=bytes.getSize();descriptorBytes+=size;candidate.entries.push_back(std::move(entry));candidate.native.push_back(1);return true;
        };
        for(const auto& d:factory)if(!accept(d))return false;
        for(const auto& d:embedded)if(!accept(d))return false;
        candidate.id=juce::Uuid::null();candidate.id=juce::Uuid(seal(candidate,candidate.native).substring(0,32));
        candidate.authority=seal(candidate,candidate.native);output=std::move(candidate);if(status)*status=RackCatalogStatus::ready;return true;
    }catch(...){return fail(status,RackCatalogStatus::allocationFailure);}
}
bool rackCatalogNativeEntry(const RackCatalogSnapshot& snapshot,std::size_t index){
    try{if(snapshot.entries.size()>maximumRackCatalogEntries||snapshot.native.size()!=snapshot.entries.size()||index>=snapshot.entries.size()
        ||snapshot.authority!=seal(snapshot,snapshot.native)||snapshot.native[index]==0)return false;
        const auto& entry=snapshot.entries[index];juce::MemoryBlock bytes;
        return canonical(entry.definition,RackValidationMode::instanceRecall,individualBytes,bytes)
            &&juce::SHA256(bytes).toHexString()==entry.contentSha256;}catch(...){return false;}
}
bool resolveCatalogDesign(const RackCatalogSnapshot& snapshot,juce::Uuid id,std::uint64_t revision,const juce::String& hash,RackDesign& output,RackCatalogStatus* status){
    try{if(snapshot.entries.size()>maximumRackCatalogEntries||snapshot.native.size()!=snapshot.entries.size()||snapshot.authority!=seal(snapshot,snapshot.native))return fail(status,RackCatalogStatus::invalidSource);
        const auto found=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[&](const auto& e){return e.definition.id==id&&e.definition.revision==revision&&e.contentSha256==hash;});
        if(found==snapshot.entries.end())return fail(status,RackCatalogStatus::unavailable);const auto n=std::size_t(found-snapshot.entries.begin());juce::MemoryBlock bytes;
        if(!canonical(found->definition,snapshot.native[n]?RackValidationMode::instanceRecall:RackValidationMode::portableDesign,individualBytes,bytes)||juce::SHA256(bytes).toHexString()!=hash)return fail(status,RackCatalogStatus::invalidSource);
        auto candidate=owned(found->definition);output=std::move(candidate);if(status)*status=RackCatalogStatus::ready;return true;
    }catch(...){return fail(status,RackCatalogStatus::allocationFailure);}
}
bool catalogPage(const RackCatalogSnapshot& snapshot,const RackCatalogQuery& query,std::size_t cursor,std::size_t byteBudget,RackCatalogPage& output,RackCatalogStatus* status){
    try{if(query.text.getNumBytesAsUTF8()>512||(query.role&&*query.role>RackRole::fx))return fail(status,RackCatalogStatus::invalidQuery);
        if(snapshot.entries.size()>maximumRackCatalogEntries||snapshot.native.size()!=snapshot.entries.size()||snapshot.authority!=seal(snapshot,snapshot.native))return fail(status,RackCatalogStatus::invalidSource);
        if(cursor>snapshot.entries.size())return fail(status,RackCatalogStatus::invalidCursor);byteBudget=std::min(byteBudget,individualBytes);
        RackCatalogPage candidate;candidate.snapshot=snapshot.id;candidate.next=cursor;std::size_t size;
        candidate.complete=cursor==snapshot.entries.size();candidate.entries.reserve(snapshot.entries.size()-cursor);
        if(!rackCatalogEscapedSize(rackCatalogPageDescriptor(candidate),byteBudget,size))return fail(status,RackCatalogStatus::singleEntryTooLarge);
        for(std::size_t n=cursor;n<snapshot.entries.size();++n){const auto& e=snapshot.entries[n];
            if((query.role&&e.definition.role!=*query.role)||(!e.definition.name.containsIgnoreCase(query.text)&&!e.definition.intent.containsIgnoreCase(query.text))){candidate.next=n+1;continue;}
            RackDesign d;if(!resolveCatalogDesign(snapshot,e.definition.id,e.definition.revision,e.contentSha256,d,status))return false;
            candidate.entries.push_back({std::move(d),fresh(e.contentSha256)});candidate.next=n+1;candidate.complete=n+1==snapshot.entries.size();
            if(!rackCatalogEscapedSize(rackCatalogPageDescriptor(candidate),byteBudget,size)){
                candidate.entries.pop_back();candidate.next=n;candidate.complete=false;if(candidate.entries.empty())return fail(status,RackCatalogStatus::singleEntryTooLarge);break;}
        }
        candidate.complete=candidate.next==snapshot.entries.size();if(!rackCatalogEscapedSize(rackCatalogPageDescriptor(candidate),byteBudget,size))return fail(status,RackCatalogStatus::singleEntryTooLarge);
        output=std::move(candidate);if(status)*status=RackCatalogStatus::ready;return true;
    }catch(...){return fail(status,RackCatalogStatus::allocationFailure);}
}
}
