#include "RackInventory.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "RackReconciliation.h"
#include "AiCatalog.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <string>

namespace airack {
namespace {
namespace limits=rackPackageLimits;
using Status=RackLibraryStatus;
constexpr std::size_t builtinCount=17,maximumEntries=builtinCount+limits::installedCount+limits::embeddedCount;
struct Definition {
    std::string canonical;
    std::array<NativeModule,9> modules{};
    std::uint8_t count=0,synths=0,effects=0;
    bool ambiguousEmbeddedPackage=false;
};
void statusTo(Status* out,Status value){if(out)*out=value;}
bool fail(Status value,Status* out){statusTo(out,value);return false;}
bool add(std::size_t& total,std::size_t bytes,std::size_t ceiling){if(total>ceiling||bytes>ceiling-total)return false;total+=bytes;return true;}
juce::String fresh(const juce::String& text){return juce::String::fromUTF8(text.toRawUTF8(),int(text.getNumBytesAsUTF8()));}
bool validRef(const RackPackageRef& ref){return validRackPackageId(ref.id)&&validRackPackageVersion(ref.version)&&ref.sha256.length()==64&&ref.sha256.containsOnly("0123456789abcdef");}
std::size_t stringBytes(const juce::String& text){return 23+((std::size_t(text.getNumBytesAsUTF8())+4)&~std::size_t(3));}
std::size_t refBytes(const RackPackageRef& ref){return stringBytes(ref.id)+stringBytes(ref.version)+stringBytes(ref.sha256);}
RackPackageRef owned(const RackPackageRef& ref){return {fresh(ref.id),fresh(ref.version),fresh(ref.sha256)};}
std::size_t metadataBytes(const RackMetadata& m){
    // JUCE's Array allocation rounds tag storage to eight elements.
    std::size_t bytes=8*sizeof(juce::String)+32;
    for(const auto* s:{&m.canonicalSha256,&m.name,&m.maker,&m.purpose,&m.description})bytes+=stringBytes(*s);
    for(const auto& tag:m.tags)bytes+=stringBytes(tag);return bytes;
}
RackMetadata owned(const RackMetadata& m){
    RackMetadata copy=m;copy.canonicalSha256=fresh(m.canonicalSha256);copy.name=fresh(m.name);copy.maker=fresh(m.maker);
    copy.purpose=fresh(m.purpose);copy.description=fresh(m.description);copy.tags.clear();
    for(const auto& tag:m.tags)copy.tags.add(fresh(tag));return copy;
}
juce::String tagFor(NativeModule module){
    constexpr const char* tags[]{"analog","wavetable","saturator","delay","reverb","compressor","eq","filter","chorus","phaser","flanger","stereo","dimension","frequency-shift","convolution","multiband","mid-side","dynamic","space","reverb","drums","saturator"};
    return tags[unsigned(module)];
}
RackMetadata metadata(const RackDesign& d,const juce::MemoryBlock& bytes,bool builtin){
    RackMetadata m{d.id,d.revision,juce::SHA256(bytes).toHexString(),d.name,builtin?"AIR":"Project",d.intent,{},d.role,{}};
    m.tags.add(tagFor(d.units.front().module));return m;
}
bool summarize(std::span<const NativeModule> modules,Definition& out){
    if(modules.empty()||modules.size()>out.modules.size())return false;
    for(const auto m:modules){if(m>NativeModule::saturation)return false;out.modules[out.count++]=m;isSourceModule(m)?++out.synths:++out.effects;}
    return true;
}
std::vector<NativeModule> modules(const RackDesign& design){std::vector<NativeModule> result;result.reserve(design.units.size());for(const auto& u:design.units)result.push_back(u.module);return result;}
juce::var object(std::initializer_list<std::pair<juce::Identifier,juce::var>> fields){auto* o=new juce::DynamicObject;for(const auto& [k,v]:fields)o->setProperty(k,v);return o;}
}
struct RackInventorySnapshot::Impl {
    juce::Uuid identity;
    std::vector<RackInventoryRow> rows;
    std::vector<Definition> definitions;
    std::vector<RackPackageRef> packages;
    std::unique_ptr<RackPackageStore> store;
    std::size_t canonicalBytes=0,backingBytes=0;
    std::uint64_t generation=0;
};
RackInventorySnapshot::RackInventorySnapshot(std::unique_ptr<Impl> value,const RackPackageStore& store):impl([&]{value->store.reset(new RackPackageStore(store.impl));return std::move(value);}()) {}
RackInventorySnapshot::~RackInventorySnapshot()=default;
std::span<const RackInventoryRow> RackInventorySnapshot::rows() const{return impl->rows;}
juce::Uuid RackInventorySnapshot::id() const{return impl->identity;}
std::uint64_t RackInventorySnapshot::storeGeneration() const{return impl->generation;}
RackPackageLeasePtr RackInventorySnapshot::openPackage(const RackPackageRef& ref,Status* status,const RackLibraryCancel& cancel) const{
    if(!validRef(ref)){statusTo(status,Status::invalidData);return {};}
    if(std::find(impl->packages.begin(),impl->packages.end(),ref)==impl->packages.end()){
        const auto identity=std::find_if(impl->packages.begin(),impl->packages.end(),[&](const auto& r){return r.id==ref.id&&r.version==ref.version;});
        statusTo(status,identity==impl->packages.end()?Status::unavailable:Status::identityConflict);return {};}
    return impl->store->open(ref,status,cancel);
}
bool captureRackInventory(const RackPackageStore& store,std::span<const RackInstance> embedded,RackInventoryPtr& output,Status* status,const RackLibraryCancel& cancel){
    try{
        const auto cancelled=[&]{return cancel&&cancel();};if(cancelled())return fail(Status::cancelled,status);
        if(embedded.size()>limits::embeddedCount)return fail(Status::countLimit,status);
        Status state;const auto generation=store.generation(&state,cancel);if(state!=Status::ready)return fail(state,status);auto active=store.active(&state,cancel);
        if(state!=Status::ready)return fail(state,status);if(active.size()>limits::installedCount)return fail(Status::countLimit,status);
        std::sort(active.begin(),active.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        auto candidate=std::make_unique<RackInventorySnapshot::Impl>();
        candidate->generation=generation;
        // Exact reserves and allocator/debug-container overhead are charged before allocation.
        static_assert(sizeof(RackInventorySnapshot)<=16&&sizeof(RackInventorySnapshot::Impl)<=256&&sizeof(RackPackageStore)<=32);
        static_assert(sizeof(RackInventoryRow)<=256&&sizeof(Definition)<=80&&sizeof(RackPackageRef)<=32);
        candidate->backingBytes=sizeof(RackInventorySnapshot)+sizeof(RackInventorySnapshot::Impl)+sizeof(RackPackageStore)+320
            +maximumEntries*(sizeof(RackInventoryRow)+sizeof(Definition)+sizeof(RackPackageRef));
        if(candidate->backingBytes>limits::inventoryBytes)return fail(Status::byteLimit,status);
        candidate->rows.reserve(maximumEntries);candidate->definitions.reserve(maximumEntries);candidate->packages.reserve(maximumEntries);
        const auto rememberPackage=[&](const std::optional<RackPackageRef>& ref){
            if(!ref)return true;if(!validRef(*ref))return fail(Status::invalidData,status);
            if(std::find(candidate->packages.begin(),candidate->packages.end(),*ref)==candidate->packages.end()){
                if(!add(candidate->backingBytes,refBytes(*ref),limits::inventoryBytes))return fail(Status::byteLimit,status);
                candidate->packages.push_back(owned(*ref));}return true;
        };
        const auto append=[&](const RackMetadata& meta,std::string_view canonical,std::span<const NativeModule> unitModules,const std::optional<RackPackageRef>& package,bool builtin,bool installed){
            if(canonical.empty()||canonical.size()>limits::designBytes)return fail(Status::byteLimit,status);
            if(juce::SHA256(canonical.data(),canonical.size()).toHexString()!=meta.canonicalSha256)return fail(Status::identityConflict,status);
            if(!rememberPackage(package))return false;
            for(std::size_t n=0;n<candidate->rows.size();++n){auto& row=candidate->rows[n];auto& definition=candidate->definitions[n];
                if(row.metadata.design!=meta.design||row.metadata.revision!=meta.revision)continue;
                if(row.metadata.canonicalSha256!=meta.canonicalSha256||definition.canonical!=canonical)return fail(Status::identityConflict,status);
                if(installed&&row.package&&row.package!=package)return fail(Status::identityConflict,status);
                if(installed){
                    if(package&&(!row.package||row.package!=package)){if(!add(candidate->backingBytes,refBytes(*package),limits::inventoryBytes))return fail(Status::byteLimit,status);row.package=owned(*package);}
                    row.installed=true;
                }else if(!row.installed&&package){
                    if(row.package&&row.package!=package){row.package.reset();definition.ambiguousEmbeddedPackage=true;}
                    else if(!row.package&&!definition.ambiguousEmbeddedPackage){if(!add(candidate->backingBytes,refBytes(*package),limits::inventoryBytes))return fail(Status::byteLimit,status);row.package=owned(*package);}
                }return true;
            }
            if(candidate->rows.size()>=maximumEntries)return fail(Status::countLimit,status);
            Definition definition;if(!summarize(unitModules,definition))return fail(Status::invalidData,status);
            const auto ownedBytes=metadataBytes(meta)+(package?refBytes(*package):0)+std::max<std::size_t>(15,canonical.size())+(canonical.size()>=4096?63:32);
            if(!add(candidate->canonicalBytes,canonical.size(),limits::canonicalInventoryBytes)||!add(candidate->backingBytes,ownedBytes,limits::inventoryBytes))return fail(Status::byteLimit,status);
            definition.canonical.assign(canonical);candidate->rows.push_back({owned(meta),package?std::optional(owned(*package)):std::nullopt,builtin,installed,false});
            candidate->definitions.push_back(std::move(definition));return true;
        };
        // Frozen retained AIR identities in curated order; shipping detachment remains Task8.
        constexpr const char* builtinIds[]{"91387715-13d3-4c57-a479-12251842a402","91387715-13d3-4c57-a479-12251842b402","91387715-13d3-4c57-a479-12251842c401",
            "91387715-13d3-4c57-a479-12251842d402","91387715-13d3-4c57-a479-12251842e402","91387715-13d3-4c57-a479-12251842f402","91387715-13d3-4c57-a479-12251842a802",
            "91387715-13d3-4c57-a479-12251842b802","91387715-13d3-4c57-a479-12251842c802","91387715-13d3-4c57-a479-12251842d901","91387715-13d3-4c57-a479-12251842e902",
            "91387715-13d3-4c57-a479-12251842f901","91387715-13d3-4c57-a479-12251842aa02","91387715-13d3-4c57-a479-12251842bb01","91387715-13d3-4c57-a479-12251842bc01",
            "91387715-13d3-4c57-a479-12251842bd02","91387715-13d3-4c57-a479-12251842be01"};
        static_assert(std::size(builtinIds)==builtinCount);const auto factories=factoryRackDesigns();
        for(const auto* id:builtinIds){const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==juce::Uuid(id);});
            if(cancelled())return fail(Status::cancelled,status);
            if(found==factories.end())return fail(Status::invalidData,status);juce::MemoryBlock bytes;if(!encodeRackDesign(*found,bytes,RackValidationMode::instanceRecall))return fail(Status::invalidData,status);
            if(!append(metadata(*found,bytes,true),{static_cast<const char*>(bytes.getData()),bytes.getSize()},modules(*found),{},true,true))return false;}
        for(const auto& ref:active){if(cancelled())return fail(Status::cancelled,status);auto lease=store.open(ref,&state,cancel);if(!lease)return fail(state,status);
            if(lease->manifest().ref!=ref)return fail(Status::identityConflict,status);
            if(!append(lease->manifest().metadata,lease->canonicalRecipe(),lease->unitModules(),ref,false,true))return false;}
        std::vector<const RackInstance*> ordered;ordered.reserve(embedded.size());for(const auto& in:embedded)ordered.push_back(&in);
        std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->design.id==b->design.id?a->design.revision<b->design.revision:a->design.id.toString()<b->design.id.toString();});
        for(const auto* in:ordered){juce::MemoryBlock bytes;if(!encodeRackDesign(in->design,bytes,RackValidationMode::instanceRecall))return fail(Status::invalidData,status);
            if(cancelled())return fail(Status::cancelled,status);
            if(!append(metadata(in->design,bytes,false),{static_cast<const char*>(bytes.getData()),bytes.getSize()},modules(in->design),in->package,false,false))return false;}
        const auto current=store.generation(&state,cancel);if(state!=Status::ready)return fail(state,status);if(current!=generation)return fail(Status::busy,status);
        active=store.active(&state,cancel);if(state!=Status::ready)return fail(state,status);
        const auto final=store.generation(&state,cancel);if(state!=Status::ready)return fail(state,status);if(final!=generation)return fail(Status::busy,status);
        output=RackInventoryPtr(new RackInventorySnapshot(std::move(candidate),store));statusTo(status,Status::ready);return true;
    }catch(const std::bad_alloc&){return fail(Status::byteLimit,status);}catch(const std::exception&){return fail(Status::invalidData,status);}
}
std::vector<std::size_t> queryRackRows(std::span<const RackInventoryRow> rows,const RackLibraryQuery& q,std::span<const RackTag> tags){
    std::vector<std::size_t> result;if((q.collection!=RackLibraryCollection::all&&q.collection!=RackLibraryCollection::installed&&q.collection!=RackLibraryCollection::updates)||(q.role&&*q.role>RackRole::fx))return result;const auto text=q.text.trim();
    for(std::size_t n=0;n<rows.size();++n){const auto& row=rows[n];const auto& m=row.metadata;const bool installed=row.builtIn||row.installed;
        if((q.collection==RackLibraryCollection::installed&&!installed)||(q.collection==RackLibraryCollection::updates&&(!row.installed||!row.updateAvailable))||(q.role&&m.role!=*q.role))continue;
        if(std::any_of(q.tags.begin(),q.tags.end(),[&](const auto& tag){return !m.tags.contains(tag);}))continue;
        bool matches=text.isEmpty()||m.name.containsIgnoreCase(text)||m.maker.containsIgnoreCase(text)||m.purpose.containsIgnoreCase(text);
        for(const auto& tag:m.tags){if(matches)break;matches=tag.containsIgnoreCase(text);for(const auto& label:tags)if(label.id==tag&&label.label.containsIgnoreCase(text)){matches=true;break;}}
        if(matches)result.push_back(n);
    }return result;
}
std::vector<std::size_t> queryRackInventory(const RackInventorySnapshot& snapshot,const RackLibraryQuery& q,std::span<const RackTag> tags){return queryRackRows(snapshot.rows(),q,tags);}
bool resolveInventoryDesign(const RackInventorySnapshot& snapshot,juce::Uuid id,std::uint64_t revision,const juce::String& hash,RackDesign& output,std::optional<RackPackageRef>& package){
    try{for(std::size_t n=0;n<snapshot.impl->rows.size();++n){const auto& row=snapshot.impl->rows[n];if(row.metadata.design!=id||row.metadata.revision!=revision||row.metadata.canonicalSha256!=hash)continue;
            const auto& bytes=snapshot.impl->definitions[n].canonical;RackDesign candidate;if(!decodeRackDesign(bytes.data(),bytes.size(),candidate,RackValidationMode::instanceRecall))return false;
            output=std::move(candidate);package=row.package;return true;}
    }catch(const std::exception&){}return false;
}
RackCandidateStatus makeCatalogRackCandidate(const SoundDocument& before,const RackInventorySnapshot& snapshot,juce::Uuid id,std::uint64_t revision,const juce::String& hash,
    const RackOperation& operation,SoundEditOrigin origin,SoundDocument& output,RackDifference& difference,SourceAssignmentTrace* trace){
    try{
        if(operation.design||operation.package)return RackCandidateStatus::invalid;
        RackDesign design;std::optional<RackPackageRef> package;if(!resolveInventoryDesign(snapshot,id,revision,hash,design,package))return RackCandidateStatus::invalid;
        RackCatalogSnapshot selected;if(!captureRackCatalog(std::span(&design,1),{},{},selected))return RackCandidateStatus::invalid;
        auto local=operation;local.package=std::move(package);
        return makeCatalogRackCandidate(before,selected,id,revision,hash,local,origin,output,difference,trace);
    }catch(const std::exception&){return RackCandidateStatus::invalid;}
}
juce::var rackDiscoveryDescriptor(const RackInventorySnapshot& snapshot,ai::CatalogRendering rendering){
    try{
        if(rendering>ai::CatalogRendering::ownedLocal||snapshot.impl->rows.size()>maximumEntries)return {};
        juce::StringArray ids;juce::Array<juce::var> dictionary,entries;
        for(std::size_t n=0;n<snapshot.impl->rows.size();++n){const auto& d=snapshot.impl->definitions[n];
            if(rendering==ai::CatalogRendering::companionPcm&&std::find(d.modules.begin(),d.modules.begin()+d.count,NativeModule::drum)!=d.modules.begin()+d.count)continue;
            const auto& m=snapshot.impl->rows[n].metadata;juce::Array<juce::var> tags,units;
            for(const auto& tag:m.tags){auto index=ids.indexOf(tag);if(index<0){if(ids.size()>=int(limits::tagDictionaryCount))return {};index=ids.size();ids.add(tag);dictionary.add(object({{"id",tag},{"label",tag}}));}tags.add(index);}
            for(std::size_t u=0;u<d.count;++u)units.add(int(d.modules[u]));
            entries.add(juce::Array<juce::var>{m.design.toString(),juce::String(std::to_string(m.revision)),m.canonicalSha256,m.name,m.role==RackRole::synth?"synth":"fx",tags,int(d.synths),int(d.effects),units});
        }
        auto descriptor=object({{"format",2},{"complete",true},{"columns",juce::Array<juce::var>{"id","revision","sha256","name","role","tags","synthUnits","fxUnits","modules"}},{"tags",dictionary},{"entries",entries}});
        std::size_t bytes;if(!rackCatalogEscapedSize(descriptor,917504,bytes))return {};return descriptor;
    }catch(const std::exception&){return {};}
}
}
