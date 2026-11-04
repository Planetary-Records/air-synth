#include "FrozenRackExamples.h"
#include "RackInventory.h"
#include "RackPackageFixtures.h"
#include "RackExamples.h"
#include "ParameterRegistry.h"
#include "AiCatalog.h"
#include "ProposalDocument.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <chrono>
#include <set>
#include <type_traits>

using namespace airack;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<line<<": "<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
juce::var object(std::initializer_list<std::pair<juce::Identifier,juce::var>> fields){auto* o=new juce::DynamicObject;for(const auto& [k,v]:fields)o->setProperty(k,v);return o;}
struct Package {
    RackDesign design;
    RackPackageManifest manifest;
    juce::MemoryBlock recipe;
    explicit Package(int n){
        design=factoryRackDesigns()[0];design.id=juce::Uuid("10000000-0000-4000-8000-"+juce::String(n).paddedLeft('0',12));
        design.revision=18446744073709549567ULL+std::uint64_t(n);design.name="Fixture "+juce::String(n);
        manifest=test::packageFixture("fixture-"+juce::String(n));refresh();
    }
    void refresh(){
        CHECK(encodeRackDesign(design,recipe,RackValidationMode::instanceRecall));
        manifest.metadata={design.id,design.revision,juce::SHA256(recipe).toHexString(),design.name,"Fixture Maker","Native purpose","Cached description",design.role,{"analog","warm"}};
        const auto png=test::packageFixturePng();
        manifest.files={{"rack.json",manifest.metadata.canonicalSha256,"design",recipe.getSize()},{"screenshot.png",juce::SHA256(png).toHexString(),"png",png.getSize()}};
        juce::Array<juce::var> modules;std::set<std::pair<int,int>> seen;
        for(const auto& u:design.units)if(seen.emplace(int(u.module),int(u.algorithmRevision)).second)modules.add(object({{"module",int(u.module)},{"algorithmRevision",int(u.algorithmRevision)}}));
        manifest.capabilities=object({{"airMin","0.1.0"},{"modules",modules},{"nativeEditor",""}});
    }
    RackPackageRef archive(const juce::File& root){
        CHECK(root.createDirectory().wasOk());const auto path=root.getChildFile(manifest.ref.id+"-"+manifest.ref.version+".zip");
        juce::ZipFile::Builder zip;const auto json=test::packageFixtureJson(manifest);const auto png=test::packageFixturePng();const juce::Time stamp(1980,0,1,0,0);
        zip.addEntry(new juce::MemoryInputStream(json.toRawUTF8(),json.getNumBytesAsUTF8(),true),0,"package.json",stamp);
        zip.addEntry(new juce::MemoryInputStream(recipe,true),0,"rack.json",stamp);zip.addEntry(new juce::MemoryInputStream(png,true),0,"screenshot.png",stamp);
        {auto out=path.createOutputStream();CHECK(out!=nullptr);if(out)CHECK(zip.writeToStream(*out,nullptr));}
        auto ref=manifest.ref;juce::FileInputStream input(path);ref.sha256=juce::SHA256(input).toHexString();
        return ref;
    }
    RackPackageRef install(RackPackageStore& store,const juce::File& root){
        const auto ref=archive(root);CHECK(store.install(root.getChildFile(manifest.ref.id+"-"+manifest.ref.version+".zip"),ref,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);return ref;
    }
    RackPackageRef commit(const juce::File& root,const juce::File& archives){
        const auto ref=archive(archives);const auto release=root.getChildFile("packages").getChildFile(ref.id).getChildFile(ref.version);
        CHECK(release.createDirectory().wasOk());CHECK(root.getChildFile("locks").createDirectory().wasOk());
        const auto write=[&](const char* name,const void* bytes,std::size_t size){juce::FileOutputStream file(release.getChildFile(name));CHECK(file.openedOk());CHECK(file.write(bytes,size));file.flush();};
        const auto json=test::packageFixtureJson(manifest);write("package.json",json.toRawUTF8(),json.getNumBytesAsUTF8());write("rack.json",recipe.getData(),recipe.getSize());const auto png=test::packageFixturePng();
        write("screenshot.png",png.getData(),png.getSize());write(".archive-sha256",ref.sha256.toRawUTF8(),ref.sha256.getNumBytesAsUTF8());return ref;
    }
};
void index(const juce::File& root,const std::vector<RackPackageRef>& releases,const std::vector<RackPackageRef>& active,int generation){
    juce::Array<juce::var> r,a;const auto entry=[](const auto& ref){return object({{"id",ref.id},{"version",ref.version},{"sha256",ref.sha256}});};
    for(const auto& ref:releases)r.add(entry(ref));for(const auto& ref:active)a.add(entry(ref));
    CHECK(root.createDirectory().wasOk());CHECK(root.getChildFile("installed.json").replaceWithText(juce::JSON::toString(object({{"schema",1},{"generation",juce::String(generation)},{"releases",r},{"active",a}}),true)));
}
void queries(){
    std::vector<RackInventoryRow> rows(4);
    rows[0].metadata.name="Thread Press";rows[0].metadata.role=RackRole::fx;rows[0].metadata.tags={"compressor","saturator"};rows[0].installed=true;
    rows[1].metadata.name="Tile Array";rows[1].metadata.role=RackRole::synth;rows[1].metadata.tags={"drums"};rows[1].installed=true;rows[1].updateAvailable=true;
    rows[2].metadata.name="Copper Arc";rows[2].metadata.maker="AIR";rows[2].metadata.purpose="Bright pulse";rows[2].builtIn=true;
    rows[3].metadata.name="Remote only";rows[3].metadata.tags={"compressor"};rows[3].updateAvailable=true;
    CHECK(queryRackRows(rows,{}).size()==4);
    RackLibraryQuery q;q.role=RackRole::fx;q.tags={"compressor","saturator"};CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{0});
    q.tags={"drums"};CHECK(queryRackRows(rows,q).empty());q.role=RackRole::synth;CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{1});
    q={};q.collection=RackLibraryCollection::installed;CHECK(queryRackRows(rows,q)==std::vector<std::size_t>({0,1,2}));
    q.collection=RackLibraryCollection::updates;CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{1});q.text="absent";CHECK(queryRackRows(rows,q).empty());
    q.text={};q.role.reset();q.tags.clear();CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{1});
    q={};q.text="tHrEaD";CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{0});
    q.text="air";CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{2});q.text="pulse";CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{2});
    q.text="SATURATOR";CHECK(queryRackRows(rows,q)==std::vector<std::size_t>{0});
    std::vector<RackTag> tags{{"saturator","Harmonic density"}};q.text="DENSITY";CHECK(queryRackRows(rows,q,tags)==std::vector<std::size_t>{0});CHECK(queryRackRows(rows,q).empty());
    q.text={};q.tags={"unknown"};CHECK(queryRackRows(rows,q).empty());q.tags={"compressor","unknown"};CHECK(queryRackRows(rows,q).empty());
    q={};q.role=RackRole(2);CHECK(queryRackRows(rows,q).empty());q={};q.collection=RackLibraryCollection(3);CHECK(queryRackRows(rows,q).empty());
    q.collection=RackLibraryCollection(-1);CHECK(queryRackRows(rows,q).empty());
}
void inventory(const juce::File& trial){
    const auto root=trial.getChildFile("store");RackInventoryPtr captured;RackLibraryStatus status=RackLibraryStatus::invalidData;
    auto store=std::make_unique<RackPackageStore>(RackLibraryPaths{root});
    CHECK(captureRackInventory(*store,{},captured,&status));CHECK(status==RackLibraryStatus::ready);
    if(captured){CHECK(captured->rows().size()==17);CHECK(!captured->id().isNull());for(const auto& row:captured->rows())CHECK(row.builtIn&&row.installed&&!row.package);}
    std::vector<Package> packages;std::vector<RackPackageRef> refs;
    for(int n=0;n<96;++n){packages.emplace_back(n);refs.push_back(packages.back().install(*store,trial.getChildFile("archives")));}
    CHECK(captureRackInventory(*store,{},captured,&status));CHECK(status==RackLibraryStatus::ready);
    if(!captured)return;
    CHECK(captured->rows().size()==113);CHECK(queryRackInventory(*captured,{}).size()==113);
    RackLibraryQuery q;q.text="FIXTURE MAKER";CHECK(queryRackInventory(*captured,q).size()==96);
    q={};q.collection=RackLibraryCollection::updates;CHECK(queryRackInventory(*captured,q).empty());
    for(std::size_t n=0;n<packages.size();++n){RackDesign resolved;std::optional<RackPackageRef> ref;
        CHECK(resolveInventoryDesign(*captured,packages[n].design.id,packages[n].design.revision,packages[n].manifest.metadata.canonicalSha256,resolved,ref));
        CHECK(resolved==packages[n].design);CHECK(ref==refs[n]);}
    const auto descriptor=rackDiscoveryDescriptor(*captured,ai::CatalogRendering::ownedLocal);CHECK(int(descriptor["format"])==2);CHECK(bool(descriptor["complete"]));
    CHECK(descriptor["entries"].isArray()&&descriptor["entries"].size()==113);CHECK(descriptor["columns"].size()==9);
    const auto discoveryJson=juce::JSON::toString(descriptor,true);CHECK(discoveryJson.getNumBytesAsUTF8()<=917504);CHECK(!discoveryJson.contains("parameters")&&!discoveryJson.contains("defaults"));
    std::set<juce::String> identities;
    if(const auto* entries=descriptor["entries"].getArray())for(const auto& row:*entries){CHECK(row.isArray()&&row.size()==9);CHECK(row[1].isString());CHECK(identities.insert(row[0].toString()+"/"+row[1].toString()).second);
        CHECK(row[5].isArray()&&row[8].isArray());for(const auto& tag:*row[5].getArray())CHECK(tag.isInt()&&int(tag)>=0&&int(tag)<descriptor["tags"].size());}
    RackDesign sentinel=factoryRackDesigns()[1],result=sentinel;std::optional<RackPackageRef> kept=refs[0];
    CHECK(!resolveInventoryDesign(*captured,packages[0].design.id,packages[0].design.revision+1,packages[0].manifest.metadata.canonicalSha256,result,kept));CHECK(result==sentinel&&kept==refs[0]);
    CHECK(!resolveInventoryDesign(*captured,packages[0].design.id,packages[0].design.revision,juce::String::repeatedString("0",64),result,kept));CHECK(result==sentinel&&kept==refs[0]);
    auto lease=captured->openPackage(refs[0],&status);CHECK(lease!=nullptr);CHECK(store->remove(refs[0],{})==RackLibraryStatus::busy);lease.reset();
    auto wrongRef=refs[0];wrongRef.sha256=juce::String::repeatedString("0",64);CHECK(!captured->openPackage(wrongRef,&status));CHECK(status==RackLibraryStatus::identityConflict);
    CHECK(store->remove(refs[0],{})==RackLibraryStatus::ready);CHECK(!captured->openPackage(refs[0],&status));CHECK(status==RackLibraryStatus::unavailable);
    CHECK(resolveInventoryDesign(*captured,packages[0].design.id,packages[0].design.revision,packages[0].manifest.metadata.canonicalSha256,result,kept));CHECK(result==packages[0].design&&kept==refs[0]);
    const auto preserved=captured;
    auto embedded=RackInstance{};embedded.design=packages[1].design;embedded.design.intent="Conflicting same revision";
    CHECK(!captureRackInventory(*store,std::span(&embedded,1),captured,&status));CHECK(status==RackLibraryStatus::identityConflict);CHECK(captured==preserved);
    std::vector<RackInstance> tooMany(13,embedded);CHECK(!captureRackInventory(*store,tooMany,captured,&status));CHECK(status==RackLibraryStatus::countLimit);CHECK(captured==preserved);
    store.reset();lease=captured->openPackage(refs[1],&status);CHECK(lease!=nullptr);CHECK(status==RackLibraryStatus::ready);lease.reset();
    CHECK(root.getChildFile("installed.json").replaceWithText("broken"));RackPackageStore broken({root});
    CHECK(!captureRackInventory(broken,{},captured,&status));CHECK(status==RackLibraryStatus::unavailable);CHECK(captured==preserved);
}
void embeddedReferences(const juce::File& trial){
    const auto root=trial.getChildFile("store"),archives=trial.getChildFile("archives");RackPackageStore store({root});Package p(200);
    const auto v1=p.install(store,archives);p.manifest.ref.version="2.0.0";const auto v2=p.install(store,archives);
    RackInstance pinned;pinned.design=p.design;pinned.package=v1;const auto original=pinned;RackInventoryPtr inventory;RackLibraryStatus status;
    CHECK(captureRackInventory(store,std::span(&pinned,1),inventory,&status));CHECK(pinned==original);
    if(!inventory)return;CHECK(inventory->rows().size()==18);RackDesign result;std::optional<RackPackageRef> selected;
    CHECK(resolveInventoryDesign(*inventory,p.design.id,p.design.revision,p.manifest.metadata.canonicalSha256,result,selected));CHECK(selected==v2);
    auto lease=inventory->openPackage(v1,&status);CHECK(lease!=nullptr);CHECK(lease&&lease->manifest().ref==v1);lease.reset();
    CHECK(store.remove(v1,{})==RackLibraryStatus::ready);CHECK(!inventory->openPackage(v1,&status));CHECK(status==RackLibraryStatus::unavailable);
    CHECK(captureRackInventory(store,std::span(&pinned,1),inventory,&status));CHECK(pinned.package==v1);
    CHECK(store.remove(v2,{})==RackLibraryStatus::ready);CHECK(captureRackInventory(store,std::span(&pinned,1),inventory,&status));
    CHECK(resolveInventoryDesign(*inventory,p.design.id,p.design.revision,p.manifest.metadata.canonicalSha256,result,selected));CHECK(selected==v1);
    CHECK(!inventory->openPackage(v1,&status));CHECK(status==RackLibraryStatus::unavailable);
    auto second=pinned;second.package=v2;auto absent=pinned;absent.package.reset();std::array<RackInstance,3> embedded{pinned,second,absent};
    CHECK(captureRackInventory(store,embedded,inventory,&status));CHECK(inventory->rows().size()==18);
    CHECK(resolveInventoryDesign(*inventory,p.design.id,p.design.revision,p.manifest.metadata.canonicalSha256,result,selected));CHECK(!selected);
    CHECK(embedded[0].package==v1&&embedded[1].package==v2&&!embedded[2].package);
    embedded[1].design.revision++;CHECK(captureRackInventory(store,embedded,inventory,&status));CHECK(inventory->rows().size()==19);
    const auto kept=inventory;embedded[0].package->sha256="bad";CHECK(!captureRackInventory(store,embedded,inventory,&status));CHECK(status==RackLibraryStatus::invalidData);CHECK(inventory==kept);
    RackInstance drum;drum.design=airack::test::frozenTileArrayRackDesign();CHECK(captureRackInventory(store,std::span(&drum,1),inventory,&status));
    const auto local=rackDiscoveryDescriptor(*inventory,ai::CatalogRendering::ownedLocal),remote=rackDiscoveryDescriptor(*inventory,ai::CatalogRendering::companionPcm);
    CHECK(local["entries"].size()==18&&remote["entries"].size()==17);CHECK(rackDiscoveryDescriptor(*inventory,ai::CatalogRendering(2)).isVoid());
    p.manifest.ref.version="3.0.0";p.install(store,archives);Package duplicate=p;duplicate.manifest.ref.id="competing-active";duplicate.install(store,archives);
    const auto previous=inventory;CHECK(!captureRackInventory(store,{},inventory,&status));CHECK(status==RackLibraryStatus::identityConflict);CHECK(inventory==previous);
}
RackDesign recipeOfSize(RackDesign design,std::size_t target){
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto effect=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.units[0].module==NativeModule::spaceReverb;});
    design.intent={};design.panel={};design.panel.sections={"Main"};design.units={{0,NativeModule::analog,{}}};
    for(std::uint8_t n=1;n<=8;++n)design.units.push_back({n,NativeModule::spaceReverb,{}});
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(design,bytes,RackValidationMode::instanceRecall));
    std::vector<LocalParameter> sourceDefaults;for(const auto& p:parameterRegistry())if(p.id.startsWith("synth.1.")&&p.id.substring(8)!="drum_mode"&&rackParameterIndex(NativeModule::analog,0,p.id.substring(8)))sourceDefaults.push_back({0,p.id.substring(8),0});
    std::vector<LocalParameter> available;
    for(std::size_t n=0;n<design.units.size();++n)for(auto param:n?effect->units[0].defaults:sourceDefaults){param.unit=std::uint8_t(n);available.push_back(param);}
    const auto withDefaults=[&](std::size_t count){auto d=design;for(std::size_t n=0;n<count;++n)d.units[available[n].unit].defaults.push_back(available[n]);return d;};
    std::size_t low=0,high=available.size();
    while(low<high){const auto middle=low+(high-low+1)/2;juce::MemoryBlock candidate;
        if(encodeRackDesign(withDefaults(middle),candidate,RackValidationMode::instanceRecall)&&candidate.getSize()<=target)low=middle;else high=middle-1;}
    design=withDefaults(low);CHECK(encodeRackDesign(design,bytes,RackValidationMode::instanceRecall));
    CHECK(bytes.getSize()<=target);const auto gap=target-bytes.getSize();CHECK(gap<=512);std::cout<<"RECIPE target="<<target<<" before="<<bytes.getSize()<<" gap="<<gap<<std::endl;if(gap>512)return design;design.intent=juce::String::repeatedString("x",int(gap));
    CHECK(encodeRackDesign(design,bytes,RackValidationMode::instanceRecall));CHECK(bytes.getSize()==target);return design;
}
void boundaries(const juce::File& trial,bool overflowOnly){
    const auto start=std::chrono::steady_clock::now();
    const auto phase=[&](const char* label,int count=-1){std::cout<<"INVENTORY_PHASE mode="<<(overflowOnly?"overflow":"boundaries")<<" phase="<<label<<" count="<<count<<" elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<std::endl;};
    phase("begin");
    const auto root=trial.getChildFile("store"),archives=trial.getChildFile("archives");RackPackageStore store({root});RackInventoryPtr inventory;RackLibraryStatus status;
    CHECK(captureRackInventory(store,{},inventory,&status));if(!inventory)return;phase("initial_capture");
    std::size_t fixedBytes=0;for(const auto& row:inventory->rows()){RackDesign d;std::optional<RackPackageRef> ref;CHECK(resolveInventoryDesign(*inventory,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,d,ref));juce::MemoryBlock bytes;CHECK(encodeRackDesign(d,bytes,RackValidationMode::instanceRecall));fixedBytes+=bytes.getSize();}
    std::vector<RackInstance> embedded(12);for(int n=0;n<12;++n){embedded[std::size_t(n)].design=Package(n+1000).design;juce::MemoryBlock bytes;CHECK(encodeRackDesign(embedded[std::size_t(n)].design,bytes,RackValidationMode::instanceRecall));fixedBytes+=bytes.getSize();}
    Package seed(0);seed.design.name=juce::String::repeatedString("N",64);const auto large=recipeOfSize(seed.design,65536);
    phase("large_recipe");
    const auto fullCount=(rackPackageLimits::canonicalInventoryBytes-fixedBytes)/65536;
    const auto remaining=(rackPackageLimits::canonicalInventoryBytes-fixedBytes)%65536;CHECK(fullCount<512&&fullCount>=500);CHECK(remaining>(512-fullCount)*2048);
    std::vector<RackPackageRef> refs;std::vector<Package> packages;refs.reserve(512);packages.reserve(512);std::size_t total=fixedBytes;
    for(int n=0;n<512;++n){Package p=seed;p.design=large;p.design.id=juce::Uuid("10000000-0000-4000-8000-"+juce::String(n).paddedLeft('0',12));p.design.revision=18446744073709549567ULL+std::uint64_t(n);
        // Spread the remaining bytes over all smaller legal recipes, with one exact residual.
        const auto smallCount=512-fullCount;
        if(std::size_t(n)>=fullCount){const auto size=std::size_t(n)==511?remaining-(smallCount-1)*2048:2048;p.design=recipeOfSize(p.design,size);}
        if(overflowOnly&&n==511)p.design.intent+="x";
        p.manifest.ref.id=("fixture-"+juce::String(n)).paddedRight('z',64);p.manifest.ref.version="65535.65535.65535";p.refresh();
        p.manifest.metadata.maker=juce::String("Fixture Maker").paddedRight('m',128);p.manifest.metadata.purpose=juce::String::repeatedString("p",512);p.manifest.metadata.description=juce::String::repeatedString("d",4096);p.manifest.metadata.tags.clear();
        for(int t=0;t<8;++t)p.manifest.metadata.tags.add(("tag-"+juce::String(t)).paddedRight('t',32));
        total+=p.recipe.getSize();refs.push_back(p.commit(root,archives));packages.push_back(std::move(p));if((n+1)%64==0)phase("package_commit",n+1);}
    phase("commits_complete");CHECK(total==rackPackageLimits::canonicalInventoryBytes+(overflowOnly?1:0));index(root,refs,refs,1);phase("index_complete");
    auto firstLease=store.open(refs.front(),&status);CHECK(firstLease!=nullptr);CHECK(status==RackLibraryStatus::ready);firstLease.reset();phase("first_open");
    if(overflowOnly){const auto preserved=inventory;CHECK(!captureRackInventory(store,embedded,inventory,&status));CHECK(status==RackLibraryStatus::byteLimit);CHECK(inventory==preserved);phase("overflow_capture");std::cout<<"CANONICAL_OVERFLOW bytes="<<total<<'\n';return;}
    const bool admitted=captureRackInventory(store,embedded,inventory,&status);CHECK(admitted);CHECK(status==RackLibraryStatus::ready);if(!admitted){std::cerr<<"BOUNDARY_CAPTURE_STATUS "<<int(status)<<'\n';return;}
    phase("boundary_capture");CHECK(inventory->rows().size()==541);CHECK(queryRackInventory(*inventory,{}).size()==541);
    for(const auto& row:inventory->rows())if(row.package){CHECK(row.metadata.maker.getNumBytesAsUTF8()==128);CHECK(row.metadata.purpose.getNumBytesAsUTF8()==512);CHECK(row.metadata.description.getNumBytesAsUTF8()==4096);CHECK(row.metadata.tags.size()==8);}
    const auto descriptor=rackDiscoveryDescriptor(*inventory,ai::CatalogRendering::ownedLocal);CHECK(descriptor["entries"].size()==541);const auto serialized=juce::JSON::toString(descriptor,true,22);CHECK(serialized.getNumBytesAsUTF8()<=917504);
    ai::CatalogAuthority authority;authority.root={juce::Uuid(),1,0,0};authority.snapshot=inventory->id();authority.rendering=ai::CatalogRendering::ownedLocal;authority.descriptorJson=serialized.toStdString();
    CHECK(ai::detail::catalogAuthorityValid(authority));std::size_t exact=0;CHECK(rackCatalogEscapedSize(descriptor,917504,exact)&&exact==serialized.getNumBytesAsUTF8());
    authority.descriptorJson.resize(917504,' ');CHECK(ai::detail::catalogAuthorityValid(authority));authority.descriptorJson+=' ';CHECK(!ai::detail::catalogAuthorityValid(authority));
    if(const auto* entries=descriptor["entries"].getArray())for(const auto& row:*entries)if(row[0].toString()==packages.front().design.id.toString()){CHECK(int(row[6])==1&&int(row[7])==8);CHECK(row[8].size()==9);for(int n=1;n<9;++n)CHECK(int(row[8][n])==int(NativeModule::spaceReverb));}
    RackDesign resolved;std::optional<RackPackageRef> ref;const auto& last=packages.back();CHECK(resolveInventoryDesign(*inventory,last.design.id,last.design.revision,last.manifest.metadata.canonicalSha256,resolved,ref));CHECK(resolved==last.design&&ref==refs.back());
    const auto preserved=inventory;
    embedded.push_back(embedded.back());CHECK(!captureRackInventory(store,embedded,inventory,&status));CHECK(status==RackLibraryStatus::countLimit);CHECK(inventory==preserved);embedded.pop_back();
    Package extra(512);const auto extraRef=extra.commit(root,archives);CHECK(store.install(archives.getChildFile(extra.manifest.ref.id+"-"+extra.manifest.ref.version+".zip"),extraRef,RackPackageActivation::makeActive,{})==RackLibraryStatus::countLimit);
    refs.push_back(extraRef);index(root,refs,refs,2);CHECK(!captureRackInventory(store,embedded,inventory,&status));CHECK(status==RackLibraryStatus::unavailable);CHECK(inventory==preserved);
    phase("boundary_checks_complete");std::cout<<"BOUNDARIES rows=541 canonical="<<total<<" metadata=512*(128+512+4096+8*32)\n";
}
void builtinConflicts(const juce::File& trial){
    const auto root=trial.getChildFile("store"),archives=trial.getChildFile("archives");Package first(700);first.design=factoryRackDesigns()[0];first.refresh();
    auto second=first;second.manifest.ref.id="builtin-copy-2";const auto a=first.commit(root,archives),b=second.commit(root,archives);
    index(root,{a,b},{a},1);RackPackageStore store({root});RackInventoryPtr inventory;RackLibraryStatus status;
    CHECK(captureRackInventory(store,{},inventory,&status));CHECK(inventory&&inventory->rows().size()==17);
    if(inventory){
        std::vector<RackInventoryRow> rows(inventory->rows().begin(),inventory->rows().end());RackLibraryQuery updates;updates.collection=RackLibraryCollection::updates;
        CHECK(queryRackRows(rows,updates).empty());
        const auto mirror=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.package==a;});CHECK(mirror!=rows.end());
        if(mirror!=rows.end()){CHECK(mirror->builtIn&&mirror->installed);mirror->updateAvailable=true;
            CHECK(queryRackRows(rows,updates)==std::vector<std::size_t>{std::size_t(mirror-rows.begin())});
            mirror->updateAvailable=false;CHECK(queryRackRows(rows,updates).empty());}
    }
    const auto preserved=inventory;index(root,{a,b},{a,b},2);CHECK(!captureRackInventory(store,{},inventory,&status));CHECK(status==RackLibraryStatus::identityConflict);CHECK(inventory==preserved);
}
void fixtureGuards(const juce::File& trial){
    const auto root=trial.getChildFile("store"),archives=trial.getChildFile("archives");std::vector<Package> packages;std::vector<RackPackageRef> refs;
    for(int n=0;n<20;++n){Package p(n);p.manifest.metadata.tags.clear();for(int t=0;t<8;++t)p.manifest.metadata.tags.add("tag-"+juce::String(8*n+t));refs.push_back(p.commit(root,archives));packages.push_back(std::move(p));}
    index(root,refs,refs,1);RackPackageStore store({root});RackInventoryPtr inventory;RackLibraryStatus status;
    const bool admitted=captureRackInventory(store,{},inventory,&status);CHECK(admitted);if(!admitted){std::cerr<<"GUARD_CAPTURE_STATUS "<<int(status)<<'\n';return;}CHECK(inventory->rows().size()==37);CHECK(rackDiscoveryDescriptor(*inventory,ai::CatalogRendering::ownedLocal).isVoid());
    std::vector<juce::Uuid> order;for(const auto& row:inventory->rows())order.push_back(row.metadata.design);
    std::reverse(refs.begin(),refs.end());index(root,refs,refs,2);CHECK(captureRackInventory(store,{},inventory,&status));CHECK(inventory->rows().size()==order.size());for(std::size_t n=0;n<order.size();++n)CHECK(inventory->rows()[n].metadata.design==order[n]);
    const auto preserved=inventory;const auto ref=refs.back();const auto receipt=root.getChildFile("packages").getChildFile(ref.id).getChildFile(ref.version).getChildFile(".archive-sha256");
    CHECK(receipt.replaceWithText(juce::String::repeatedString("0",64)));CHECK(!captureRackInventory(store,{},inventory,&status));CHECK(status==RackLibraryStatus::identityConflict);CHECK(inventory==preserved);
    RackDesign resolved;std::optional<RackPackageRef> selected;CHECK(resolveInventoryDesign(*inventory,packages[0].design.id,packages[0].design.revision,packages[0].manifest.metadata.canonicalSha256,resolved,selected));CHECK(resolved==packages[0].design);
    const auto fileRoot=trial.getChildFile("root-is-file");CHECK(fileRoot.replaceWithText("unavailable"));RackPackageStore unavailable({fileRoot});CHECK(!captureRackInventory(unavailable,{},inventory,&status));CHECK(status==RackLibraryStatus::unavailable);CHECK(inventory==preserved);
}
void nativeInventoryAdmission(const juce::File& trial){
    Package package(900);package.design.role=RackRole::fx;package.design.units={{0,NativeModule::drive,{}}};package.design.panel.sections={"Drive"};package.design.panel.controls.clear();
    const auto values=defaultParameterValues();const auto registry=parameterRegistry();for(std::size_t n=0;n<dynamicEqParameterStart;++n)if(registry[n].id.startsWith("effect.1."))package.design.units[0].defaults.push_back({0,registry[n].id.substring(9),values[n]});
    CHECK(package.design.units[0].defaults.size()==82);CHECK(!validateRackDesign(package.design,RackValidationMode::portableDesign));package.refresh();RackPackageStore store({trial.getChildFile("store")});const auto ref=package.install(store,trial.getChildFile("archives"));
    RackInventoryPtr inventory;RackLibraryStatus status;CHECK(captureRackInventory(store,{},inventory,&status));if(!inventory)return;const auto retained=inventory;
    CHECK(!captureRackInventory(store,{},inventory,&status,[]{return true;}));CHECK(status==RackLibraryStatus::cancelled&&inventory==retained);CHECK(inventory->storeGeneration()==store.observedGeneration());
    ai::CatalogAuthority authority;authority.root={juce::Uuid(),1,0,0};authority.snapshot=inventory->id();authority.rendering=ai::CatalogRendering::ownedLocal;authority.descriptorJson=juce::JSON::toString(rackDiscoveryDescriptor(*inventory,authority.rendering),true,22).toStdString();
    authority.definitions.push_back({package.design.id,package.design.revision,package.manifest.metadata.canonicalSha256,package.recipe});
    ai::ProposalEdits edits;edits.catalogSelections={{7,package.design.id,package.design.revision,package.manifest.metadata.canonicalSha256,{RackRole::fx,masterChain,0}}};
    auto source=std::make_unique<ProjectDocument>(),output=std::make_unique<ProjectDocument>();const auto original=*output;
    CHECK(ai::admitProposal(*source,{},edits,authority,*output)!=ai::ProposalStatus::accepted);CHECK(*output==original);
    CHECK(ai::admitProposal(*source,{},edits,authority,*inventory,*output)==ai::ProposalStatus::accepted);CHECK(output->sound.personalities&&output->sound.personalities->instances.front().package==ref);
    auto bad=authority;bad.snapshot=juce::Uuid();const auto accepted=*output;CHECK(ai::admitProposal(*source,{},edits,bad,*inventory,*output)!=ai::ProposalStatus::accepted);CHECK(*output==accepted);
    source->sound.synthLocks.fill(true);source->sound.effectLocks.fill(true);CHECK(ai::admitProposal(*source,{},edits,authority,*inventory,*output)!=ai::ProposalStatus::accepted);CHECK(*output==accepted);
    *source=ProjectDocument{};for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(source->sound.layout,{RackEditKind::addEffect,slot,masterChain,slot},source->sound.layout)==RackEditResult::applied);
    CHECK(ai::admitProposal(*source,{},edits,authority,*inventory,*output)==ai::ProposalStatus::capacityReached);CHECK(*output==accepted);
    *source=ProjectDocument{};auto remote=package.design;remote.id=juce::Uuid();remote.units[0].defaults.clear();juce::MemoryBlock bytes;CHECK(encodeRackDesign(remote,bytes));const auto hash=juce::SHA256(bytes).toHexString();
    auto forged=authority;forged.definitions={{remote.id,remote.revision,hash,bytes}};auto descriptor=rackDiscoveryDescriptor(*inventory,authority.rendering);auto row=descriptor["entries"].getArray()->getLast().clone();row.getArray()->set(0,remote.id.toString());row.getArray()->set(2,hash);descriptor["entries"].getArray()->add(row);forged.descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
    auto remoteIntent=edits;remoteIntent.catalogSelections[0].design=remote.id;remoteIntent.catalogSelections[0].contentSha256=hash;CHECK(ai::detail::catalogAuthorityValid(forged));
    CHECK(ai::admitProposal(*source,{},remoteIntent,forged,*inventory,*output)!=ai::ProposalStatus::accepted);CHECK(*output==accepted);
}
}
int main(int argc,char** argv){
    static_assert(!std::is_copy_constructible_v<RackPackageStore>);
    if(argc!=2&&argc!=3)return 2;const auto trial=juce::File(argv[1]).getChildFile(juce::Uuid().toString());CHECK(trial.createDirectory().wasOk());
    std::cout<<"TRIAL "<<trial.getFullPathName()<<std::endl;
    if(argc==3&&std::string_view(argv[2])=="--boundaries"){boundaries(trial,false);fixtureGuards(trial.getChildFile("guards"));}
    else if(argc==3&&std::string_view(argv[2])=="--canonical-overflow")boundaries(trial,true);
    else if(argc==3&&std::string_view(argv[2])=="--identity-conflict")builtinConflicts(trial);
    else{queries();inventory(trial);embeddedReferences(trial.getChildFile("refs"));builtinConflicts(trial.getChildFile("builtin"));nativeInventoryAdmission(trial.getChildFile("native"));}
    std::cout<<checks<<" CHECKs, "<<failures<<" failures\n";return failures?1:0;
}
