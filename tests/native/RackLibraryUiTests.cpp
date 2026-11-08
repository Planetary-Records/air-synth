#include "LibraryUnit.h"
#include "InstrumentProcessor.h"
#include "IsolatedRackLibrary.h"
#include "StudioSkin.h"
#include "RackStack.h"
#include "HouseUnit.h"
#include "BlankInstance.h"
#include "RackPackageFixtures.h"
#include "PostedClick.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <set>
namespace {
using namespace airack;
int checks=0,failures=0;
juce::File images;juce::String only;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<": "<<#x<<'\n';}}while(false)
void pump(int ms=5){juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);}
template<class F>bool wait(F f){auto end=juce::Time::getMillisecondCounter()+5000;while(!f()&&juce::Time::getMillisecondCounter()<end)pump();return f();}
juce::Component* find(juce::Component& c,const juce::String& id){if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* result=find(*child,id))return result;return nullptr;}
companion::ClientOptions offline(){companion::ClientOptions c;c.allowLaunch=false;c.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");return c;}
void clearFilters(){studio::RackLibraryBrowserState state;state.query.collection=RackLibraryCollection::installed;state.query.text="warm";state.query.role=RackRole::fx;state.query.tags={"saturator","analog"};studio::clearRackLibraryFilters(state);CHECK(state.query.text.isEmpty()&&!state.query.role&&state.query.tags.isEmpty());CHECK(state.query.collection==RackLibraryCollection::installed);}
void browserControls(){
    InstrumentProcessor p{offline(),testing::isolatedRackRuntime()};studio::LibraryUnit unit(p,*studio::studioSkin(),{},{});
    CHECK(find(unit,"library.search")!=nullptr);CHECK(find(unit,"library.back")!=nullptr);CHECK(find(unit,"library.clear")!=nullptr);
    CHECK(wait([&]{unit.refresh();return unit.matchingRows()==17;}));auto* gallery=dynamic_cast<juce::ListBox*>(find(unit,"library.gallery"));CHECK(gallery);if(!gallery)return;
    CHECK(gallery->getModel()->getNumRows()==9);unit.setSize(900,740);CHECK(gallery->getModel()->getNumRows()==17);CHECK(!gallery->getViewport()->getHorizontalScrollBar().isVisible());
}
void guardedAdd(){
    auto runtime=testing::isolatedRackRuntime();InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));RackInventoryPtr inventory;CHECK(testing::readyRackInventory(p,inventory));if(!inventory)return;
    const auto& row=inventory->rows().front();RackDesign design;std::optional<RackPackageRef> ref;CHECK(resolveInventoryDesign(*inventory,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,design,ref));
    auto prep=runtime->client->createPlacementPreparation();runtime->client->requestPlacement(prep,inventory,{});CHECK(wait([&]{return runtime->client->placement(prep)->status==RackLibraryStatus::ready;}));
    RackReviewRequest request;request.localDesign=design;request.position=0;const auto reviewed=reviewRack(p,request);CHECK(!reviewed.draft.empty());const auto result=applyRackReview(p,reviewed,prep,runtime->client->placement(prep));CHECK(result.applied&&result.placed.has_value());
}
bool writePng(const juce::File& file,const juce::Image& image){if(file.existsAsFile()&&!file.deleteFile())return false;auto out=file.createOutputStream();return out&&juce::PNGImageFormat().writeImageToStream(image,*out);}
void browse(){
    const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/ui/"+juce::Uuid().toString());
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});std::atomic<int> mode{0},gets{0};
    runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[&](const auto&,const auto&,const auto& cancel,auto&){++gets;while(mode==1&&!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(2));return cancel()?RackLibraryStatus::cancelled:mode==2?RackLibraryStatus::invalidData:RackLibraryStatus::networkFailure;});
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));ProjectDocument before;std::uint64_t beforeEdit;CHECK(p.tryProjectSnapshot(before,beforeEdit));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);editor->setSize(1080,680);
    const auto press=[&](const char* id){auto* b=dynamic_cast<juce::Button*>(find(*editor,id));const bool ready=wait([&]{return b&&b->isVisible()&&b->isEnabled();});if(!ready)std::cout<<"BROWSE_ACTION_UNAVAILABLE "<<id<<std::endl;CHECK(ready);if(ready){const auto point=editor->getLocalPoint(b,b->getLocalBounds().getCentre());CHECK(editor->getLocalBounds().contains(point));const auto* hit=editor->getComponentAt(point);CHECK(hit==b||b->isParentOf(hit));CHECK(clickPosted(*b));pump();}};
    press("rack.add");auto* library=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(library&&library->isShowing());if(!library)return;
    CHECK(wait([&]{library->refresh();return library->matchingRows()==17;}));
    auto* search=dynamic_cast<juce::TextEditor*>(find(*library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(*library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;
    const auto capture=[&](const char* name){if(images!=juce::File{})CHECK(writePng(images.getChildFile(name),editor->createComponentSnapshot(editor->getLocalBounds(),true,1.0f)));};
    RackInventoryPtr local;CHECK(testing::readyRackInventory(p,local));CHECK(wait([&]{library->refresh();for(std::size_t n=0;n<4;++n){const auto image=runtime->client->builtInThumbnail(local->rows()[n].metadata);if(!image||image->status!=RackLibraryStatus::ready)return false;}return true;}));capture("gallery-1080x680.png");
    search->setText("cOpPeR",true);CHECK(wait([&]{return library->matchingRows()==2;}));gallery->selectRow(0);gallery->grabKeyboardFocus();gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(library->browserState().detailOpen);
    auto* back=dynamic_cast<juce::Button*>(find(*library,"library.back"));CHECK(back&&back->hasKeyboardFocus(false));
    CHECK(wait([&]{library->refresh();return library->ownedImageBytes()>0;}));capture("detail-1080x680.png");
    auto* description=dynamic_cast<juce::TextEditor*>(find(*library,"library.details"));CHECK(description&&description->getText().removeCharacters(" ").contains("DOWNLOADS:Unknown")&&description->getText().contains("EXACT VERSION:")&&description->getText().contains("COMPATIBILITY:"));
    press("library.back");CHECK(!library->browserState().detailOpen&&library->browserState().query.text=="cOpPeR"&&library->matchingRows()==2);CHECK(gallery->hasKeyboardFocus(false));
    press("library.installed");press("library.filter");auto* role=dynamic_cast<juce::ComboBox*>(find(*library,"library.role"));CHECK(role);if(role)role->setSelectedId(3);pump();CHECK(library->matchingRows()==1);
    press("library.clear");CHECK(library->browserState().query.collection==RackLibraryCollection::installed&&library->matchingRows()==17);press("library.tag.analog");CHECK(library->matchingRows()>0&&library->matchingRows()<17);press("library.tag.delay");CHECK(library->matchingRows()==0);press("library.clear");press("library.back");
    gallery->getViewport()->setViewPosition(0,302);pump();const auto scroll=gallery->getViewport()->getViewPositionY();gallery->selectRow(1);gallery->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(library->browserState().detailOpen);const auto selected=library->browserState().design;
    press("library.back");CHECK(gallery->getViewport()->getViewPositionY()==scroll&&library->browserState().design==selected);press("library.cancel");CHECK(!library->isShowing());press("rack.add");CHECK(!library->browserState().detailOpen&&library->browserState().design==selected&&gallery->getViewport()->getViewPositionY()==scroll);
    editor->setSize(1600,1000);gallery->getViewport()->setViewPosition(0,0);pump();capture("gallery-1600x1000.png");gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::leftKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump();capture("detail-1600x1000.png");press("library.back");
    search->setText("NO SUCH RACK",true);pump();CHECK(library->matchingRows()==0);CHECK(find(*library,"library.empty")&&find(*library,"library.empty")->isShowing());capture("empty.png");press("library.clear");CHECK(wait([&]{return runtime->client->view()->offline;}));capture("offline.png");
    mode=1;press("library.refresh");CHECK(runtime->client->view()->busy);capture("progress.png");press("library.cancel");mode=0;CHECK(wait([&]{return !runtime->client->view()->busy;}));press("rack.add");mode=2;press("library.refresh");CHECK(wait([&]{return !runtime->client->view()->busy;}));library->refresh();capture("error.png");
    CHECK(library->ownedImageBytes()<=64ull*1024*1024);ProjectDocument after;std::uint64_t afterEdit;CHECK(p.tryProjectSnapshot(after,afterEdit));CHECK(before==after&&beforeEdit==afterEdit);CHECK(!p.undoSoundEdit());CHECK(gets>0);
    press("library.cancel");editor.reset();
}
struct UiFixture {
    juce::File root,oneZip,twoZip;
    RackPackageManifest first=test::packageFixture(),second=first;
    RackPackageRef one,two;
    juce::MemoryBlock png;
    std::atomic<bool> update{false},badImage{false};bool fullTags=false;std::atomic<int> scopeTags{-1};bool boundary=false;std::atomic<int> incompatible{0};std::atomic<bool> holdCatalog{false},catalogEntered{false};std::atomic<unsigned> gets{0},archives{0},catalogGets{0};
    explicit UiFixture(const juce::File& directory,bool contour=false,bool maximumMetadata=false):root(directory),boundary(maximumMetadata){
        CHECK(root.createDirectory().wasOk());auto canonical=test::packageFixtureDesign();
        if(contour){
            const auto source=juce::JSON::parse(juce::File(RACKSYNTH_WORKSPACE).getChildFile("racks/contour-dynamics/rack-source.json"))["manifest"];
            canonical.reset(); // loadFileAsData appends; the retained Ember bytes must not precede this exact recipe.
            CHECK(juce::File(RACKSYNTH_WORKSPACE).getChildFile("racks/contour-dynamics/rack.json").loadFileAsData(canonical));CHECK(juce::SHA256(canonical).toHexString()==source["metadata"]["canonical_sha256"].toString());
            const auto m=source["metadata"];first.ref.id="contour-dynamics";
            first.metadata={juce::Uuid(m["design"].toString()),std::uint64_t(m["revision"].toString().getLargeIntValue()),m["canonical_sha256"].toString(),m["name"].toString(),m["maker"].toString(),m["purpose"].toString(),m["description"].toString(),RackRole::fx,{"eq","dynamic","precision"}};
            first.capabilities=source["capabilities"];first.files.front()={"rack.json",juce::SHA256(canonical).toHexString(),"design",canonical.getSize()};
        }
        RackDesign design;CHECK(decodeRackDesign(canonical.getData(),canonical.getSize(),design,RackValidationMode::instanceRecall));
        if(boundary){
            first.metadata.name=juce::String::repeatedString("Boundary instrument name ",8).substring(0,128); // Remote metadata has a128-byte limit; canonical recipes retain their existing64-byte name limit.
            first.metadata.canonicalSha256=juce::SHA256(canonical).toHexString();first.files.front()={"rack.json",first.metadata.canonicalSha256,"design",canonical.getSize()};
            first.metadata.maker=juce::String::repeatedString("Boundary maker name ",8).substring(0,128);
            first.metadata.purpose=juce::String::repeatedString("Maximum purpose remains readable and wraps in native detail. ",12).substring(0,496)+" PURPOSE-END-512";
            first.metadata.tags.clear();for(int n=0;n<8;++n)first.metadata.tags.add("boundary-"+juce::String(n)+juce::String::repeatedString("x",22));
        }
        {
            InstrumentProcessor p{offline(),testing::isolatedRackRuntime()};CHECK(blankInstance(p));RackReviewRequest request;request.localDesign=design;request.package=RackPackageRef{first.ref.id,first.ref.version,juce::String::repeatedString("a",64)};request.position=0;
            const auto applied=applyRackReview(p,reviewRack(p,request));CHECK(applied.applied&&applied.placed);if(applied.placed){SoundDocument sound;std::uint64_t edit,reset;CHECK(p.trySoundSnapshot(sound,edit,reset));const auto& model=*studio::studioSkin();RackStackUnit unit(p,model,*applied.placed);unit.refresh(sound);unit.setSize(1547,274);juce::MemoryOutputStream bytes;CHECK(juce::PNGImageFormat().writeImageToStream(unit.createComponentSnapshot(unit.getLocalBounds(),true,1),bytes));png=bytes.getMemoryBlock();}
        }
        CHECK(root.getChildFile("native-fixture.png").replaceWithData(png.getData(),png.getSize()));
        for(auto& file:first.files)if(file.path==first.screenshotPath){file.bytes=png.getSize();file.sha256=juce::SHA256(png).toHexString();}
        first.metadata.description="Actual native fixture screenshot. Installing makes this exact recipe available; Add alone changes the sound. "+juce::String::repeatedString("Long readable description with native controls and no markup. ",30);
        if(boundary)first.metadata.description=juce::String::repeatedString("Maximum description wraps and scrolls without hiding fixed actions. ",70).substring(0,4075)+" DESCRIPTION-END-4096";
        second=first;second.ref.version="2.0.0";
        const auto zip=[&](const RackPackageManifest& manifest){const auto file=root.getChildFile(manifest.ref.id+"-"+manifest.ref.version+".zip");juce::ZipFile::Builder builder;const auto json=test::packageFixtureJson(manifest);const juce::Time stamp(1980,0,1,0,0);builder.addEntry(new juce::MemoryInputStream(json.toRawUTF8(),json.getNumBytesAsUTF8(),true),0,"package.json",stamp);builder.addEntry(new juce::MemoryInputStream(canonical,true),0,"rack.json",stamp);builder.addEntry(new juce::MemoryInputStream(png,true),0,"screenshot.png",stamp);auto output=file.createOutputStream();CHECK(output&&builder.writeToStream(*output,nullptr));return file;};
        oneZip=zip(first);twoZip=zip(second);one=first.ref;two=second.ref;one.sha256=juce::SHA256(*oneZip.createInputStream()).toHexString();two.sha256=juce::SHA256(*twoZip.createInputStream()).toHexString();
    }
    static juce::var object(std::initializer_list<std::pair<juce::Identifier,juce::var>> fields){auto* d=new juce::DynamicObject;for(const auto& [key,value]:fields)d->setProperty(key,value);return d;}
    juce::var catalog() const{
        const auto release=[&](const RackPackageManifest& m,const RackPackageRef& ref,const juce::File& file,int id){return object({{"ref",object({{"id",ref.id},{"version",ref.version},{"sha256",ref.sha256}})},{"metadata",juce::JSON::parse(test::packageFixtureJson(m))["metadata"]},{"capabilities",m.capabilities.clone()},{"archive",object({{"assetId",id},{"url","https://github.com/Planetary-Records/air-rack-library/releases/download/v1/"+file.getFileName()},{"bytes",juce::int64(file.getSize())},{"sha256",ref.sha256}})},{"screenshot",object({{"url","https://raw.githubusercontent.com/Planetary-Records/air-rack-library/main/native-fixture.png"},{"bytes",juce::int64(png.getSize())},{"sha256",juce::SHA256(png).toHexString()}})}});};
        juce::Array<juce::var> releases{release(first,one,oneZip,101)};if(update)releases.add(release(second,two,twoZip,102));
        if(incompatible)for(int n=0;n<releases.size();++n)if(incompatible==2||n>0)releases[n]["capabilities"].getDynamicObject()->setProperty("airMin","99.0.0");
        juce::Array<juce::var> dictionary;for(const auto& tag:first.metadata.tags)dictionary.add(object({{"id",tag},{"label",boundary?("Readable boundary tag "+juce::String(dictionary.size())+juce::String::repeatedString(" label",10)).substring(0,64):tag.toUpperCase()}}));
        if(fullTags)while(dictionary.size()<128){const auto id="catalog-tag-"+juce::String(dictionary.size());dictionary.add(object({{"id",id},{"label",id}}));}
        if(scopeTags>=0){
            dictionary.clear();juce::Array<juce::var> racks;const int count=scopeTags.load();
            for(int n=0;n<count;++n)dictionary.add(object({{"id","scope-tag-"+juce::String(n)},{"label","Readable scope label "+juce::String(n)}}));
            for(int n=0;n<count;n+=8){auto item=releases.getFirst().clone();const auto id="scope-rack-"+juce::String(n/8);item["ref"].getDynamicObject()->setProperty("id",id);item["archive"].getDynamicObject()->setProperty("assetId",200+n/8);
                auto* metadata=item["metadata"].getDynamicObject();metadata->setProperty("name","Scope Rack "+juce::String(n/8).paddedLeft('0',3));metadata->setProperty("purpose","Boundary dictionary fixture");metadata->setProperty("description","Bounded native filters");juce::Array<juce::var> used;for(int tag=n;tag<std::min(count,n+8);++tag)used.add("scope-tag-"+juce::String(tag));metadata->setProperty("tags",used);
                racks.add(object({{"id",id},{"order",n/8},{"releases",juce::Array<juce::var>{item}}}));}
            return object({{"schema",1},{"tags",dictionary},{"racks",racks}});
        }
        return object({{"schema",1},{"tags",dictionary},{"racks",juce::Array<juce::var>{object({{"id",one.id},{"order",0},{"releases",releases}})}}});
    }
    RackLibraryTransport transport(){return [this](const RackHttpRequest& request,const RackHttpSink& sink,const RackLibraryCancel& cancel,RackHttpResponse& response){++gets;if(cancel())return RackLibraryStatus::cancelled;response.code=200;juce::MemoryBlock bytes;
        if(request.url==rackLibraryCatalogUrl){++catalogGets;catalogEntered=true;while(holdCatalog&&!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(2));if(cancel())return RackLibraryStatus::cancelled;const auto text=juce::JSON::toString(catalog(),true);bytes=juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8());}
        else if(request.url.startsWith("https://api.github.com/")){juce::Array<juce::var> assets;if(scopeTags>=0){for(int n=0;n<scopeTags.load();n+=8)assets.add(object({{"id",200+n/8},{"download_count",12}}));}else{assets.add(object({{"id",101},{"download_count",12}}));assets.add(object({{"id",102},{"download_count",8}}));}const auto text=juce::JSON::toString(juce::Array<juce::var>{object({{"assets",assets}})},true);bytes=juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8());}
        else if(request.url.endsWith(".png")){if(badImage)return RackLibraryStatus::networkFailure;bytes=png;}
        else{++archives;CHECK((request.url.endsWith(oneZip.getFileName())?oneZip:twoZip).loadFileAsData(bytes));}
        return sink(bytes.getData(),bytes.getSize())?RackLibraryStatus::ready:RackLibraryStatus::byteLimit;
    };}
};
unsigned tagKeyCount(const juce::Component& c){unsigned count=c.getComponentID().startsWith("library.tag.")?1u:0u;for(const auto* child:c.getChildren())count+=tagKeyCount(*child);return count;}
void boundedTags(){
    UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/fix1/tag-fixtures/"+juce::Uuid().toString()));
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));RackInventoryPtr inventory;CHECK(testing::readyRackInventory(p,inventory));if(!inventory)return;std::set<juce::String> localTags;for(const auto& row:inventory->rows())for(const auto& tag:row.metadata.tags)localTags.insert(tag);
    CHECK(!localTags.empty()&&localTags.size()<128);fixture.scopeTags=128-int(localTags.size());
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);editor->setSize(1080,680);
    const auto press=[&](const char* id){auto* b=dynamic_cast<juce::Button*>(find(*editor,id));CHECK(b&&b->isShowing()&&b->isEnabled());if(b){CHECK(clickPosted(*b));pump();}};
    press("rack.add");auto* library=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(library);if(!library)return;
    CHECK(wait([&]{library->refresh();return !runtime->client->view()->busy&&runtime->client->view()->status==RackLibraryStatus::ready;}));CHECK(runtime->client->view()->tags.size()==std::size_t(fixture.scopeTags.load()));
    press("library.filter");CHECK(tagKeyCount(*library)==128);auto* search=dynamic_cast<juce::TextEditor*>(find(*library,"library.search"));CHECK(search);if(!search)return;
    search->setText("Scope Rack 000",true);pump();CHECK(library->matchingRows()==1&&tagKeyCount(*library)==8);press("library.tag.scope-tag-0");CHECK(library->browserState().query.tags.contains("scope-tag-0")&&tagKeyCount(*library)==8);
    search->setText({},true);pump();CHECK(library->matchingRows()==1&&tagKeyCount(*library)==128);fixture.scopeTags++;auto job=runtime->client->refresh();CHECK(wait([&]{library->refresh();return !runtime->client->view()->busy&&runtime->client->view()->job==job;}));
    CHECK(runtime->client->view()->status==RackLibraryStatus::ready);CHECK(tagKeyCount(*library)==0);auto* overflow=dynamic_cast<juce::Label*>(find(*library,"library.tag-overflow"));CHECK(overflow&&overflow->isShowing()&&overflow->getText().contains("128")&&overflow->getText().containsIgnoreCase("search"));CHECK(library->browserState().query.tags.contains("scope-tag-0")&&library->matchingRows()==1);
    if(images!=juce::File{})CHECK(writePng(images.getChildFile("tags-overflow-1080x680.png"),editor->createComponentSnapshot(editor->getLocalBounds(),true,1)));
    search->setText("Scope",true);pump();CHECK(tagKeyCount(*library)==unsigned(fixture.scopeTags.load()));CHECK(find(*library,"library.tag.scope-tag-8")!=nullptr&&library->matchingRows()==1);CHECK(!overflow||!overflow->isShowing());
    if(images!=juce::File{})CHECK(writePng(images.getChildFile("tags-refined-1080x680.png"),editor->createComponentSnapshot(editor->getLocalBounds(),true,1)));
    press("library.tag.scope-tag-1");CHECK(library->matchingRows()==1&&library->browserState().query.tags.size()==2);press("library.tag.scope-tag-8");CHECK(library->matchingRows()==0&&library->browserState().query.tags.size()==3);
    press("library.clear");search->setText("Readable scope label 0",true);pump();CHECK(library->matchingRows()==1);
    press("library.installed");press("library.clear");CHECK(library->browserState().query.collection==RackLibraryCollection::installed&&library->matchingRows()==17);
    CHECK(tagKeyCount(*library)==localTags.size());auto* role=dynamic_cast<juce::ComboBox*>(find(*library,"library.role"));CHECK(role);if(role){role->setSelectedId(3);pump();std::set<juce::String> fxTags;for(const auto& row:inventory->rows())if(row.metadata.role==RackRole::fx)for(const auto& tag:row.metadata.tags)fxTags.insert(tag);CHECK(tagKeyCount(*library)==fxTags.size());press("library.clear");CHECK(tagKeyCount(*library)==localTags.size()&&!library->browserState().query.role);}
    unsigned reachable=0;for(const auto& row:inventory->rows()){search->setText(row.metadata.name,true);pump();CHECK(library->matchingRows()>0&&tagKeyCount(*library)<=128);for(const auto& tag:row.metadata.tags){CHECK(find(*library,"library.tag."+tag)!=nullptr);++reachable;}}
    press("library.all");press("library.clear");fixture.scopeTags=-1;fixture.fullTags=true;job=runtime->client->refresh();CHECK(wait([&]{library->refresh();return !runtime->client->view()->busy&&runtime->client->view()->job==job;}));CHECK(runtime->client->view()->tags.size()==128);CHECK(tagKeyCount(*library)<=128&&find(*library,"library.tag.analog")!=nullptr&&find(*library,"library.tag.catalog-tag-127")==nullptr);
    ProjectDocument before,after;std::uint64_t edit,next;CHECK(p.tryProjectSnapshot(before,edit));press("library.clear");CHECK(p.tryProjectSnapshot(after,next)&&before==after&&edit==next&&!p.undoSoundEdit());CHECK(fixture.archives==0);
    std::cout<<"TAG_BOUNDARY local_distinct="<<localTags.size()<<" exact=128 overflow=129 refined="<<128-localTags.size()+1<<" local_tag_occurrences_reachable="<<reachable<<" unused_remote_dictionary=128\n";
}
void maximumMetadata(){
    UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/fix1/metadata-fixtures/"+juce::Uuid().toString()),false,true);
    CHECK(fixture.first.metadata.name.getNumBytesAsUTF8()==128&&fixture.first.metadata.maker.getNumBytesAsUTF8()==128);CHECK(fixture.first.metadata.purpose.getNumBytesAsUTF8()==512&&fixture.first.metadata.description.getNumBytesAsUTF8()==4096);CHECK(fixture.first.metadata.tags.size()==8);for(const auto& tag:fixture.first.metadata.tags)CHECK(tag.getNumBytesAsUTF8()==32);
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());CHECK(runtime->store->active().empty());
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));ProjectDocument before,after;std::uint64_t edit,next;CHECK(p.tryProjectSnapshot(before,edit));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);
    const auto press=[&](const char* id){auto* b=dynamic_cast<juce::Button*>(find(*editor,id));CHECK(b&&b->isShowing()&&b->isEnabled());if(b){const auto bounds=editor->getLocalArea(b,b->getLocalBounds());CHECK(editor->getLocalBounds().contains(bounds)&&bounds.getWidth()>=24&&bounds.getHeight()>=24);const auto* hit=editor->getComponentAt(bounds.getCentre());CHECK(hit==b||b->isParentOf(hit));CHECK(clickPosted(*b));pump();}};
    editor->setSize(1080,680);press("rack.add");auto* library=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(library);if(!library)return;
    CHECK(wait([&]{library->refresh();return library->matchingRows()==18&&!runtime->client->view()->busy&&runtime->client->view()->status==RackLibraryStatus::ready;}));CHECK(runtime->client->view()->tags.size()==8);for(const auto& tag:runtime->client->view()->tags)CHECK(tag.label.getNumBytesAsUTF8()==64);
    auto* search=dynamic_cast<juce::TextEditor*>(find(*library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(*library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;search->setText("Boundary instrument",true);pump();CHECK(library->matchingRows()==1);
    const auto capture=[&](const juce::String& name){if(images!=juce::File{})CHECK(writePng(images.getChildFile(name),editor->createComponentSnapshot(editor->getLocalBounds(),true,1)));};
    for(const auto size:{juce::Point<int>{1080,680},juce::Point<int>{1600,1000}}){editor->setSize(size.x,size.y);pump();CHECK(!gallery->getViewport()->getHorizontalScrollBar().isVisible());CHECK(wait([&]{library->refresh();const auto image=runtime->client->thumbnail(fixture.one);return image&&image->status==RackLibraryStatus::ready&&library->ownedImageBytes()>0;}));const auto suffix=juce::String(size.x)+"x"+juce::String(size.y)+".png";capture("maximum-gallery-"+suffix);
        gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::leftKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(library->browserState().detailOpen);CHECK(wait([&]{library->refresh();auto* install=dynamic_cast<juce::Button*>(find(*library,"library.install"));return install&&install->isEnabled()&&runtime->client->thumbnail(fixture.one,2048)&&library->ownedImageBytes()>0;}));
        auto* source=dynamic_cast<juce::HyperlinkButton*>(find(*library,"library.source"));auto* contribute=dynamic_cast<juce::HyperlinkButton*>(find(*library,"library.contribute"));
        CHECK(source&&source->isShowing()&&source->isEnabled()&&source->getURL().toString(false)==rackLibrarySourceUrl(fixture.one));
        CHECK(contribute&&contribute->isShowing()&&contribute->getURL().toString(false)=="https://github.com/Planetary-Records/air-rack-library/blob/main/CONTRIBUTING.md");
        if(source&&contribute){CHECK(source->getHeight()>=24&&contribute->getHeight()>=24);CHECK(!source->getBounds().intersects(contribute->getBounds()));CHECK(library->getLocalBounds().contains(source->getBounds()));}
        auto* description=dynamic_cast<juce::TextEditor*>(find(*library,"library.description"));auto* details=dynamic_cast<juce::TextEditor*>(find(*library,"library.details"));CHECK(description&&details);if(description&&details){CHECK(description->getText().contains(fixture.first.metadata.name)&&description->getText().contains(fixture.first.metadata.maker)&&description->getText().contains(fixture.first.metadata.purpose)&&description->getText().contains(fixture.first.metadata.description));for(const auto& tag:runtime->client->view()->tags)CHECK(details->getText().contains(tag.label));CHECK(details->getText().contains("EXACT VERSION:")&&details->getText().contains("INSTALLED:")&&details->getText().contains("LATEST:"));
            description->setCaretPosition(0);description->scrollEditorToPositionCaret(0,0);details->setCaretPosition(0);details->scrollEditorToPositionCaret(0,0);capture("maximum-detail-"+suffix);
            const auto scrollTo=[&](juce::TextEditor& text,const juce::String& marker,bool bottom){
                juce::Viewport* viewport=nullptr;for(auto* child:text.getChildren())if(auto* view=dynamic_cast<juce::Viewport*>(child))viewport=view;CHECK(viewport&&!viewport->getHorizontalScrollBar().isVisible());if(!viewport)return;
                auto& scrollbar=viewport->getVerticalScrollBar();CHECK(scrollbar.isVisible());text.grabKeyboardFocus();CHECK(text.hasKeyboardFocus(true));const auto index=text.getText().indexOf(marker);CHECK(index>=0);if(index<0)return;
                const auto before=text.getCaretRectangleForCharIndex(index);scrollbar.setCurrentRangeStart(bottom?scrollbar.getMaximumRangeLimit():scrollbar.getCurrentRangeStart()+before.getY()-text.getHeight()/4,juce::sendNotificationSync);pump();
                const auto visible=text.getCaretRectangleForCharIndex(index);CHECK(viewport->getViewPositionY()>0&&visible.getY()>=0&&visible.getBottom()<=text.getHeight());CHECK(!viewport->getHorizontalScrollBar().isVisible());
                std::cout<<"METADATA_VISIBLE size="<<size.x<<"x"<<size.y<<" panel="<<text.getComponentID()<<" marker="<<marker<<" viewport_y="<<viewport->getViewPositionY()<<" target_y="<<visible.getY()<<" target_bottom="<<visible.getBottom()<<" panel_height="<<text.getHeight()<<'\n';
            };
            scrollTo(*description,"DESCRIPTION-END-4096",true);scrollTo(*details,"Readable boundary tag 7",false);capture("maximum-detail-last-tag-"+suffix);
            scrollTo(*details,"DOWNLOAD SIZE:",true);for(const auto* label:{"EXACT VERSION:","INSTALLED:","LATEST:"}){const auto visible=details->getCaretRectangleForCharIndex(details->getText().indexOf(label));CHECK(visible.getY()>=0&&visible.getBottom()<=details->getHeight());}capture("maximum-detail-footer-"+suffix);
        }
        for(const auto* id:{"library.install","library.cancel","library.back"}){auto* action=dynamic_cast<juce::Button*>(find(*library,id));CHECK(action&&action->isShowing()&&action->isEnabled());if(action){const auto bounds=editor->getLocalArea(action,action->getLocalBounds());CHECK(editor->getLocalBounds().contains(bounds)&&bounds.getWidth()>=24&&bounds.getHeight()>=24);const auto* hit=editor->getComponentAt(bounds.getCentre());CHECK(hit==action||action->isParentOf(hit));action->grabKeyboardFocus();CHECK(action->hasKeyboardFocus(true));}}
        press("library.back");CHECK(library->browserState().query.text=="Boundary instrument");
    }
    CHECK(p.tryProjectSnapshot(after,next)&&before==after&&edit==next&&!p.undoSoundEdit());CHECK(fixture.archives==0);std::cout<<"MAXIMUM_METADATA name=128 maker=128 purpose=512 description=4096 tags=8 id_bytes=32 label_bytes=64 sizes=1080x680,1600x1000 fixed_actions=3 no_horizontal_scroll=1\n";
}
void guardedPackages(){
    UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/package-guard/"+juce::Uuid().toString()));
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());
    CHECK(runtime->store->install(fixture.oneZip,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));RackInventoryPtr root;CHECK(testing::readyRackInventory(p,root));if(!root)return;
    RackDesign design;std::optional<RackPackageRef> package;CHECK(resolveInventoryDesign(*root,fixture.first.metadata.design,fixture.first.metadata.revision,fixture.first.metadata.canonicalSha256,design,package)&&package==fixture.one);
    auto prep=runtime->client->createPlacementPreparation();runtime->client->requestPlacement(prep,root,std::span<const RackPackageRef>(&fixture.one,1));CHECK(wait([&]{return runtime->client->placement(prep)->status==RackLibraryStatus::ready;}));auto authority=runtime->client->placement(prep);
    RackReviewRequest request;request.localDesign=design;request.package=fixture.one;request.position=0;const auto reviewed=reviewRack(p,request);CHECK(!reviewed.draft.empty());
    ProjectDocument before,after;std::uint64_t edit,afterEdit;CHECK(p.tryProjectSnapshot(before,edit));
    CHECK(p.applyRackOperations(reviewed.draft,reviewed.expected,reviewed.reset,{},authority)!=RackCandidateStatus::ready);
    CHECK(!applyRackReview(p,reviewed,prep,std::make_shared<RackLibraryPlacementView>(*authority)).applied);
    auto resetMismatch=reviewed;++resetMismatch.reset;CHECK(!applyRackReview(p,resetMismatch,prep,authority).applied);
    auto changed=reviewed;changed.draft.front().design->name="Different canonical design";CHECK(!applyRackReview(p,changed,prep,authority).applied);
    CHECK(p.tryProjectSnapshot(after,afterEdit)&&after==before&&afterEdit==edit);
    CHECK(runtime->store->remove(fixture.one,{})==RackLibraryStatus::busy);
    CHECK(applyRackReview(p,reviewed,prep,authority).applied);CHECK(p.undoSoundEdit());
    CHECK(!applyRackReview(p,reviewed,prep,authority).applied);CHECK(p.tryProjectSnapshot(after,afterEdit)&&after==before);
    const auto liveReview=reviewRack(p,request);CHECK(applyRackReview(p,liveReview,prep,authority).applied);
    CHECK(runtime->store->install(fixture.twoZip,fixture.two,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    CHECK(!applyRackReview(p,reviewRack(p,request),prep,authority).applied);
    authority.reset();runtime->client->cancelPlacement(prep);CHECK(testing::readyRackInventory(p,root));
    package.reset();RackDesign current;CHECK(resolveInventoryDesign(*root,design.id,design.revision,fixture.first.metadata.canonicalSha256,current,package)&&current==design&&package==fixture.two);
    runtime->client->requestPlacement(prep,root,{});CHECK(wait([&]{return runtime->client->placement(prep)->status==RackLibraryStatus::ready;}));authority=runtime->client->placement(prep);CHECK(authority->leases.empty());
    CHECK(applyRackReview(p,reviewRack(p,request),prep,authority).applied);CHECK(p.undoSoundEdit());
    auto absent=request;absent.package=fixture.two;CHECK(!applyRackReview(p,reviewRack(p,absent),prep,authority).applied);
    auto unrelated=runtime->client->createPlacementPreparation();runtime->client->requestPlacement(unrelated,root,std::span<const RackPackageRef>(&fixture.two,1));CHECK(wait([&]{return runtime->client->placement(unrelated)->status==RackLibraryStatus::ready;}));CHECK(!applyRackReview(p,reviewRack(p,request),unrelated,runtime->client->placement(unrelated)).applied);runtime->client->cancelPlacement(unrelated);
    authority.reset();runtime->client->cancelPlacement(prep);const auto job=runtime->client->remove(fixture.one);CHECK(wait([&]{return runtime->client->view()->job==job&&!runtime->client->view()->busy;}));CHECK(runtime->client->view()->status==RackLibraryStatus::ready&&runtime->store->active()==std::vector<RackPackageRef>{fixture.two});
    CHECK(testing::readyRackInventory(p,root));runtime->client->requestPlacement(prep,root,{});CHECK(wait([&]{return runtime->client->placement(prep)->status==RackLibraryStatus::ready;}));CHECK(applyRackReview(p,reviewRack(p,request),prep,runtime->client->placement(prep)).applied);
}

void exactHighlight(){
    UiFixture f(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task9/final-highlight/"+juce::Uuid().toString()));f.update=true;
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{f.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,f.transport());
    CHECK(runtime->store->install(f.twoZip,f.two,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));RackDesign d;const auto bytes=test::packageFixtureDesign();CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),d,RackValidationMode::instanceRecall));RackReviewRequest request;request.localDesign=d;request.package=f.one;CHECK(applyRackReview(p,reviewRack(p,request)).applied);
    studio::LibraryUnit library(p,*studio::studioSkin(),{},{});library.addToDesktop(0);library.setVisible(false);library.setVisible(true);CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&library.matchingRows()==19;}));
    auto* search=dynamic_cast<juce::TextEditor*>(find(library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;search->setText("Ember Reed",true);CHECK(wait([&]{library.refresh();return library.matchingRows()==2;}));gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(library.browserState().release==f.one);
    juce::Image cards(juce::Image::ARGB,1000,374,true);{juce::Graphics g(cards);gallery->getModel()->paintListBoxItem(0,g,1000,374,false);}std::cout<<"HIGHLIGHT left="<<cards.getPixelAt(14,10).toString()<<" right="<<cards.getPixelAt(514,10).toString()<<" accent="<<studio::accent.toString()<<std::endl;CHECK((cards.getPixelAt(14,10)==studio::accent)!=(cards.getPixelAt(514,10)==studio::accent));
}
void compatibleOffer(){
    for(const int mode:{1,2}){
        UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task9/final-offer/"+juce::Uuid().toString()));fixture.update=true;fixture.incompatible=mode;
        auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());
        InstrumentProcessor p{offline(),runtime};studio::LibraryUnit library(p,*studio::studioSkin(),{},{});library.addToDesktop(0);library.setVisible(false);library.setVisible(true);
        CHECK(wait([&]{library.refresh();return library.matchingRows()==18&&!runtime->client->view()->busy;}));auto* search=dynamic_cast<juce::TextEditor*>(find(library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;
        search->setText("Ember Reed",true);CHECK(wait([&]{library.refresh();return library.matchingRows()==1;}));gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        auto* install=dynamic_cast<juce::Button*>(find(library,"library.install"));auto* details=dynamic_cast<juce::TextEditor*>(find(library,"library.details"));CHECK(install&&details);if(!install||!details)return;
        CHECK(details->getText().contains("newer AIR")&&details->getText().contains("2.0.0"));
        CHECK(library.browserState().release==(mode==1?fixture.one:fixture.two));CHECK(install->isVisible()==(mode==1));
        if(mode==1){CHECK(clickPosted(*install));CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&runtime->store->active()==std::vector<RackPackageRef>{fixture.one};}));}
        else CHECK(fixture.archives==0);
    }
}
void reopen(){
    UiFixture f(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task9/final-reopen/"+juce::Uuid().toString()));
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{f.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,f.transport());
    InstrumentProcessor p{offline(),runtime};studio::LibraryUnit library(p,*studio::studioSkin(),{},{});library.addToDesktop(0);library.setVisible(false);library.setVisible(true);
    CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&f.catalogGets>0;}));const auto initial=runtime->client->view()->job;const auto gets=f.catalogGets.load();
    library.setVisible(false);library.setVisible(true);CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&runtime->client->view()->job>initial;}));CHECK(f.catalogGets==gets);
    const auto age=[&]{for(const auto* name:{"catalog.json","counts.json"}){const auto file=f.root.getChildFile("store/cache").getChildFile(name);auto doc=juce::JSON::parse(file);doc.getDynamicObject()->setProperty("time",juce::String(juce::Time::currentTimeMillis()-16*60*1000));CHECK(file.replaceWithText(juce::JSON::toString(doc,true)));}};
    age();library.setVisible(false);library.setVisible(true);CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&f.catalogGets>gets;}));
    f.holdCatalog=true;f.catalogEntered=false;const auto busy=runtime->client->refresh();CHECK(wait([&]{return f.catalogEntered.load();}));library.setVisible(false);library.setVisible(true);CHECK(runtime->client->view()->job==busy&&runtime->client->view()->busy);
    f.holdCatalog=false;CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&runtime->client->view()->job>busy;}));
}

void installAndAdd(){
    UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/package-ui/"+juce::Uuid().toString()));
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);auto* entry=dynamic_cast<juce::Button*>(find(*editor,"rack.add"));CHECK(entry&&clickPosted(*entry));auto* panel=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(panel);if(!panel)return;auto& library=*panel;library.refresh();
    const auto captureManagement=[&](const char* state){if(images==juce::File{}||(only=="install-busy"&&juce::String(state)!="busy"))return;for(const auto size:{juce::Point<int>(1080,680),juce::Point<int>(1600,1000)}){editor->setSize(size.x,size.y);library.refresh();pump();auto* selector=find(library,"library.releases");CHECK(selector&&selector->isShowing());if(selector){const auto bounds=editor->getLocalArea(selector,selector->getLocalBounds());CHECK(editor->getLocalBounds().contains(bounds)&&bounds.getWidth()>100&&bounds.getHeight()>=24);}CHECK(writePng(images.getChildFile("management-"+juce::String(state)+"-"+juce::String(size.x)+"x"+juce::String(size.y)+".png"),editor->createComponentSnapshot(editor->getLocalBounds(),true,1)));}editor->setSize(1080,680);library.refresh();};
    const auto press=[&](const char* id){auto* b=dynamic_cast<juce::Button*>(find(library,id));CHECK(b&&b->isVisible()&&b->isEnabled());if(b){CHECK(clickPosted(*b));library.refresh();pump();}};
    CHECK(wait([&]{library.refresh();return library.matchingRows()==18&&!runtime->client->view()->busy;}));auto* search=dynamic_cast<juce::TextEditor*>(find(library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;
    ProjectDocument before;std::uint64_t revision;CHECK(p.tryProjectSnapshot(before,revision));search->setText("Ember Reed",true);CHECK(wait([&]{return library.matchingRows()==1;}));gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(!dynamic_cast<juce::Button*>(find(library,"library.apply"))->isEnabled());CHECK(fixture.archives==0);ProjectDocument after;std::uint64_t afterRevision;CHECK(p.tryProjectSnapshot(after,afterRevision)&&before==after&&revision==afterRevision);
    press("library.install");CHECK(wait([&]{library.refresh();return dynamic_cast<juce::Button*>(find(library,"library.apply"))->isEnabled();}));CHECK(fixture.archives==1&&runtime->store->active()==std::vector<RackPackageRef>{fixture.one});CHECK(p.tryProjectSnapshot(after,afterRevision)&&before==after&&revision==afterRevision);CHECK(!p.undoSoundEdit());
    auto* firstManagement=find(library,"library.releases");auto* firstDescription=find(library,"library.description");CHECK(firstManagement&&firstManagement->isShowing()&&firstDescription&&!firstManagement->getBounds().intersects(firstDescription->getBounds()));
    auto* details=dynamic_cast<juce::TextEditor*>(find(library,"library.details"));CHECK(details&&details->getText().removeCharacters(" ").contains("DOWNLOADS:12")&&details->getText().contains("1.0.0"));auto* description=dynamic_cast<juce::TextEditor*>(find(library,"library.description"));bool scrolls=false;if(description)for(auto* child:description->getChildren())if(auto* viewport=dynamic_cast<juce::Viewport*>(child))scrolls=viewport->getVerticalScrollBar().isVisible();CHECK(scrolls);
    int done=0;juce::Uuid selected=juce::Uuid::null();library.onDone=[&]{++done;};library.onPlaced=[&](juce::Uuid id,const auto&){selected=id;};press("library.apply");CHECK(done==1&&!selected.isNull());CHECK(p.tryProjectSnapshot(after,afterRevision)&&after!=before&&after.sound.personalities->instances.back().package==fixture.one);const auto withOld=after;CHECK(p.undoSoundEdit());library.refresh();CHECK(p.tryProjectSnapshot(after,afterRevision)&&after==before);
    fixture.update=true;press("library.refresh");CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy&&dynamic_cast<juce::Button*>(find(library,"library.install"))->getButtonText()=="UPDATE";}));press("library.install");CHECK(wait([&]{library.refresh();return library.browserState().release==fixture.two&&dynamic_cast<juce::Button*>(find(library,"library.apply"))->isEnabled();}));CHECK(runtime->store->open(fixture.one)&&runtime->store->active()==std::vector<RackPackageRef>{fixture.two});CHECK(p.tryProjectSnapshot(after,afterRevision)&&after==before);
    auto* releases=dynamic_cast<juce::ComboBox*>(find(library,"library.releases"));CHECK(releases&&releases->isShowing()&&releases->getNumItems()==2);if(!releases)return;
    int oldItem=0;for(int n=0;n<releases->getNumItems();++n)if(releases->getItemText(n).contains("1.0.0"))oldItem=releases->getItemId(n);CHECK(oldItem>0);releases->setSelectedId(oldItem,juce::sendNotificationSync);captureManagement("old");
    auto oldReader=runtime->store->open(fixture.one);CHECK(oldReader);press("library.uninstall");CHECK(description->getText().contains("ember-reed 1.0.0")&&description->getText().contains("exact artwork restored"));captureManagement("confirm");press("library.uninstall");
    CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy;}));CHECK(runtime->client->view()->status==RackLibraryStatus::busy&&runtime->store->open(fixture.one));captureManagement("busy");oldReader.reset();
    press("library.uninstall");press("library.uninstall");CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy;}));CHECK(runtime->client->view()->status==RackLibraryStatus::ready&&!runtime->store->open(fixture.one));
    CHECK(runtime->store->active()==std::vector<RackPackageRef>{fixture.two});CHECK(p.tryProjectSnapshot(after,afterRevision)&&after==before);RackInventoryPtr activeInventory;CHECK(testing::readyRackInventory(p,activeInventory));if(activeInventory)CHECK(std::none_of(activeInventory->rows().begin(),activeInventory->rows().end(),[&](const auto& row){return row.package==fixture.one;}));
    juce::MemoryBlock recalled;CHECK(encodeProjectState(withOld,recalled));CHECK(p.restoreProjectState(recalled.getData(),int(recalled.getSize())));
    CHECK(wait([&]{library.refresh();return library.matchingRows()==2;}));press("library.back");gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(library.browserState().release==fixture.one);
    {juce::Image cards(juce::Image::ARGB,1000,374,true);{juce::Graphics g(cards);gallery->getModel()->paintListBoxItem(0,g,1000,374,false);}std::cout<<"HIGHLIGHT left="<<cards.getPixelAt(14,10).toString()<<" right="<<cards.getPixelAt(514,10).toString()<<" accent="<<studio::accent.toString()<<std::endl;CHECK((cards.getPixelAt(14,10)==studio::accent)!=(cards.getPixelAt(514,10)==studio::accent));}
    CHECK(wait([&]{library.refresh();return dynamic_cast<juce::Button*>(find(library,"library.apply"))->isEnabled();}));CHECK(dynamic_cast<juce::Button*>(find(library,"library.install"))->getButtonText()=="RESTORE");
    press("library.install");CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy;}));CHECK(runtime->client->view()->status==RackLibraryStatus::ready&&runtime->store->active()==std::vector<RackPackageRef>{fixture.two});CHECK(p.tryProjectSnapshot(after,afterRevision)&&after==withOld);CHECK(!p.undoSoundEdit()&&done==1);
    press("library.back");gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::leftKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(library.browserState().release==fixture.two);
    press("library.uninstall");CHECK(description->getText().contains("ember-reed 2.0.0")&&description->getText().contains("Saved racks remain"));press("library.uninstall");CHECK(wait([&]{library.refresh();return !runtime->client->view()->busy;}));CHECK(runtime->client->view()->status==RackLibraryStatus::ready);CHECK(runtime->store->active().empty());CHECK(p.tryProjectSnapshot(after,afterRevision)&&after==withOld);
}

void contourGallery(){
    UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/contour-ui/"+juce::Uuid().toString()),true);
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);
    auto* open=dynamic_cast<juce::Button*>(find(*editor,"rack.add"));CHECK(open&&clickPosted(*open));auto* library=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(library);if(!library)return;
    CHECK(wait([&]{library->refresh();return library->matchingRows()==18&&!runtime->client->view()->busy;}));auto* search=dynamic_cast<juce::TextEditor*>(find(*library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(*library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;
    search->setText("Contour Dynamics",true);CHECK(wait([&]{library->refresh();return library->matchingRows()==1&&library->ownedImageBytes()>0;}));
    if(images!=juce::File{}){CHECK(writePng(images.getChildFile("contour-gallery-1080x680.png"),editor->createComponentSnapshot(editor->getLocalBounds(),true,1)));CHECK(images.getChildFile("contour-dynamics-native-house.png").replaceWithData(fixture.png.getData(),fixture.png.getSize()));}
    gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(wait([&]{library->refresh();return library->ownedImageBytes()>0;}));CHECK(library->browserState().release==fixture.one&&fixture.archives==0);
    if(images!=juce::File{})CHECK(writePng(images.getChildFile("contour-detail-1080x680.png"),editor->createComponentSnapshot(editor->getLocalBounds(),true,1)));
    CHECK(!dynamic_cast<juce::Button*>(find(*library,"library.apply"))->isEnabled());CHECK(!p.undoSoundEdit());editor.reset();
}

void missingArtwork(){
    UiFixture fixture(juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/missing-artwork/"+juce::Uuid().toString()));fixture.badImage=true;fixture.fullTags=true;
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,fixture.transport());
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));RackDesign design;const auto canonical=test::packageFixtureDesign();CHECK(decodeRackDesign(canonical.getData(),canonical.getSize(),design,RackValidationMode::instanceRecall));
    RackReviewRequest request;request.localDesign=design;request.package=fixture.one;request.position=0;CHECK(applyRackReview(p,reviewRack(p,request)).applied);
    studio::LibraryUnit library(p,*studio::studioSkin(),{},{});library.addToDesktop(0);library.setVisible(false);library.setVisible(true);
    CHECK(wait([&]{library.refresh();return library.matchingRows()==18&&!runtime->client->view()->busy;}));auto* search=dynamic_cast<juce::TextEditor*>(find(library,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(library,"library.gallery"));CHECK(search&&gallery);if(!search||!gallery)return;
    CHECK(find(library,"library.tag.analog")!=nullptr); // A full remote dictionary cannot hide local used tags.
    search->setText("Ember Reed",true);CHECK(wait([&]{return library.matchingRows()==1;}));gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(wait([&]{library.refresh();const auto image=runtime->client->thumbnail(fixture.one,2048);return image&&image->status!=RackLibraryStatus::ready;}));
    CHECK(library.ownedImageBytes()==0);auto* metadata=dynamic_cast<juce::TextEditor*>(find(library,"library.details"));CHECK(metadata&&metadata->getText().contains("EXACT VERSION:"));auto* add=dynamic_cast<juce::Button*>(find(library,"library.apply"));CHECK(wait([&]{library.refresh();return add&&add->isEnabled();}));
    ProjectDocument before,after;std::uint64_t edit,next;CHECK(p.tryProjectSnapshot(before,edit));CHECK(fixture.archives==0);CHECK(clickPosted(*add));library.refresh();CHECK(p.tryProjectSnapshot(after,next)&&next!=edit&&after.sound.personalities->instances.size()==2);CHECK(p.undoSoundEdit());CHECK(p.tryProjectSnapshot(after,next)&&after==before);
}

void inventoryRetry(){
    const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task7/inventory-retry/"+juce::Uuid().toString());
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[](const auto&,const auto&,const auto&,auto&){return RackLibraryStatus::networkFailure;});
    InstrumentProcessor p{offline(),runtime};CHECK(blankInstance(p));studio::LibraryUnit library(p,*studio::studioSkin(),{},{});library.addToDesktop(0);library.setVisible(false);library.setVisible(true);
    RackInventoryPtr inventory;RackLibraryStatus status=RackLibraryStatus::busy;CHECK(wait([&]{library.refresh();return p.captureAvailableRackInventory(inventory)&&!runtime->client->view()->busy;}));const auto previous=inventory;CHECK(previous&&library.matchingRows()==17);
    const auto index=root.getChildFile("installed.json");const bool existed=index.existsAsFile();const auto saved=index.loadFileAsString();CHECK(index.replaceWithText("broken"));
    RackReviewRequest request;request.localDesign=factoryRackDesigns().front();request.position=0;CHECK(applyRackReview(p,reviewRack(p,request)).applied);
    CHECK(wait([&]{library.refresh();p.captureAvailableRackInventory(inventory,&status);return status==RackLibraryStatus::unavailable;}));CHECK(inventory==previous&&library.matchingRows()==17);
    if(existed)CHECK(index.replaceWithText(saved));else CHECK(index.deleteFile());pump(350);library.refresh();CHECK(!p.captureAvailableRackInventory(inventory)&&inventory==previous);
    ProjectDocument before,after;std::uint64_t edit,next;CHECK(p.tryProjectSnapshot(before,edit));auto* refresh=dynamic_cast<juce::Button*>(find(library,"library.refresh"));CHECK(wait([&]{library.refresh();return refresh&&refresh->isEnabled();}));CHECK(clickPosted(*refresh));CHECK(wait([&]{library.refresh();return p.captureAvailableRackInventory(inventory)&&inventory!=previous;}));CHECK(p.tryProjectSnapshot(after,next)&&after==before&&next==edit);
}

void captureBuiltins(const juce::File& dir){
    CHECK(dir.createDirectory().wasOk());auto runtime=testing::isolatedRackRuntime();InstrumentProcessor inventoryProcessor{offline(),runtime};RackInventoryPtr inventory;CHECK(testing::readyRackInventory(inventoryProcessor,inventory));if(!inventory)return;juce::Array<juce::var> provenance;
    for(const auto& row:inventory->rows())if(row.builtIn){
        RackDesign design;std::optional<RackPackageRef> ref;CHECK(resolveInventoryDesign(*inventory,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,design,ref));
        InstrumentProcessor p{offline(),testing::isolatedRackRuntime()};CHECK(blankInstance(p));RackReviewRequest request;request.localDesign=design;request.position=0;const auto applied=applyRackReview(p,reviewRack(p,request));CHECK(applied.applied&&applied.placed);if(!applied.placed)continue;
        SoundDocument sound;std::uint64_t edit,reset;CHECK(p.trySoundSnapshot(sound,edit,reset));const auto& model=*studio::studioSkin();RackStackUnit unit(p,model,*applied.placed);unit.refresh(sound);const auto scale=1547.0f/(2.0f*studio::HouseUnit::railW+model.house.panelWidth);unit.setSize(1547,juce::roundToInt(float(unit.units())*model.house.unitHeight*scale));
        auto image=unit.createComponentSnapshot(unit.getLocalBounds(),true,1.0f);const auto name=row.metadata.design.toString()+".png";const auto file=dir.getChildFile(name);CHECK(writePng(file,image));auto* data=new juce::DynamicObject;data->setProperty("design",row.metadata.design.toString());data->setProperty("revision",juce::String(row.metadata.revision));data->setProperty("canonicalSha256",row.metadata.canonicalSha256);data->setProperty("png",name);data->setProperty("pngSha256",juce::SHA256(*file.createInputStream()).toHexString());data->setProperty("width",image.getWidth());data->setProperty("height",image.getHeight());provenance.add(data);
    }
    CHECK(provenance.size()==17);CHECK(dir.getChildFile("provenance.json").replaceWithText(juce::JSON::toString(provenance,true)));
}
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;juce::File capture;for(int n=1;n<argc;n+=2){if(n+1>=argc)return 2;if(juce::String(argv[n])=="--only"){only=argv[n+1];continue;}if(!juce::File::isAbsolutePath(argv[n+1]))return 2;if(juce::String(argv[n])=="--capture-builtins")capture=juce::File(argv[n+1]);else if(juce::String(argv[n])=="--images"){images=juce::File(argv[n+1]);CHECK(images.createDirectory().wasOk());}else return 2;}if(capture!=juce::File{})captureBuiltins(capture);else{if(only.isEmpty()||only=="controls"){clearFilters();browserControls();}if(only.isEmpty()||only=="guard"){guardedAdd();guardedPackages();}if(only.isEmpty()||only=="browse")browse();if(only.isEmpty()||only=="highlight")exactHighlight();if(only.isEmpty()||only=="offer")compatibleOffer();if(only.isEmpty()||only=="reopen")reopen();if(only.isEmpty()||only=="install"||only=="install-busy")installAndAdd();if(only.isEmpty()||only=="contour")contourGallery();if(only.isEmpty()||only=="missing-artwork")missingArtwork();if(only.isEmpty()||only=="retry")inventoryRetry();if(only.isEmpty()||only=="tags")boundedTags();if(only.isEmpty()||only=="maximum-metadata")maximumMetadata();}std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
