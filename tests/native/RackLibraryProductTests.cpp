#include "InstrumentProcessor.h"
#include "RackExamples.h"
#include "RackPackageFixtures.h"
#include "RackReview.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include "MakerFace.h"
#include "RackStack.h"
#include "RackPanel.h"
#include "StudioSkin.h"
#include "IsolatedRackLibrary.h"
#include "DrumBundle.h"
#include "LibraryUnit.h"
#include "BlankInstance.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <future>
#include <iostream>
#include <thread>
#include <cmath>
#include <map>
#include <cstring>

using namespace airack;
namespace airack {struct InstrumentDrumTestAccess {static auto history(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return std::pair(p.editHistory->count,p.editHistory->cursor);}};}
namespace {
int checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){ ++failures; std::cout<<"FAIL "<<__LINE__<<": "<<#x<<std::endl; } } while(false)
std::vector<RackDesign> frozenCurrent();
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}
template<class F> bool wait(F f){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(60);while(!f()&&std::chrono::steady_clock::now()<end){pump();std::this_thread::sleep_for(std::chrono::milliseconds(2));}return f();}
companion::ClientOptions offline(){companion::ClientOptions c;c.allowLaunch=false;c.launch.pipeSuffix=juce::Uuid().toString();return c;}
juce::Component* find(juce::Component& c,const juce::String& id){if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* result=find(*child,id))return result;return nullptr;}
SoundDocument snapshot(InstrumentProcessor& p){SoundDocument d;std::uint64_t revision=0;CHECK(p.trySoundSnapshot(d,revision));return d;}
RackInventoryPtr inventory(InstrumentProcessor& p){RackInventoryPtr result;CHECK(testing::readyRackInventory(p,result));return result;}
std::vector<float> render(juce::AudioProcessor& p,bool tile=false){
    p.setNonRealtime(true);p.prepareToPlay(48000,128);std::vector<float> result;result.reserve(96000);
    for(int block=0;block<375;++block){juce::AudioBuffer<float> audio(2,128);audio.clear();juce::MidiBuffer midi;
        if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,tile?36:48,juce::uint8(100)),0);
        if(block==187)midi.addEvent(juce::MidiMessage::noteOff(1,48),0);
        p.processBlock(audio,midi);for(int channel=0;channel<2;++channel)result.insert(result.end(),audio.getReadPointer(channel),audio.getReadPointer(channel)+128);
    }p.releaseResources();return result;
}
void compareAudio(juce::AudioProcessor& p,const juce::File& baseline,bool tile){
    juce::MemoryBlock bytes;CHECK(baseline.loadFileAsData(bytes));const auto actual=render(p,tile);CHECK(bytes.getSize()==actual.size()*sizeof(float));
    double maximum=0,peak=0;const auto* expected=static_cast<const float*>(bytes.getData());
    for(std::size_t n=0;n<actual.size()&&n<bytes.getSize()/sizeof(float);++n){CHECK(std::isfinite(actual[n]));maximum=std::max(maximum,std::abs(double(actual[n])-expected[n]));peak=std::max(peak,std::abs(double(actual[n])));}
    CHECK(maximum<=1e-6);std::cout<<"AUDIO "<<baseline.getParentDirectory().getFileName()<<" max="<<maximum<<" peak="<<peak<<" samples="<<actual.size()<<std::endl;
}
struct Fixture {
    juce::File releases,root,updateArchive;juce::var catalog;std::atomic<int> gets{0},archiveGets{0};std::atomic<bool> update{false},hold{false},entered{false},disconnected{false};
    std::shared_ptr<RackLibraryRuntime> runtime;
    Fixture(const juce::File& packages,const juce::File& directory):releases(packages),root(directory),catalog(juce::JSON::parse(packages.getChildFile("catalog.json"))){
        runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
        runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[this](const RackHttpRequest& request,const RackHttpSink& sink,const RackLibraryCancel& cancel,RackHttpResponse& response){
            ++gets;if(disconnected)return RackLibraryStatus::networkFailure;if(hold){entered=true;while(hold&&!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(2));if(cancel())return RackLibraryStatus::cancelled;}
            juce::MemoryBlock bytes;
            if(request.url==rackLibraryCatalogUrl){auto value=catalog.clone();if(!update)while(value["racks"][0]["releases"].size()>1)value["racks"][0]["releases"].getArray()->removeLast();const auto text=juce::JSON::toString(value,true);bytes.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());}
            else if(request.url==rackLibraryCountsUrl){juce::Array<juce::var> rows;
                for(const auto& rack:*catalog["racks"].getArray()){auto* release=new juce::DynamicObject;release->setProperty("tag_name",rack["id"].toString()+"-1.0.0");juce::Array<juce::var> assets;
                    for(const auto& r:*rack["releases"].getArray()){auto* asset=new juce::DynamicObject;asset->setProperty("id",r["archive"]["assetId"]);asset->setProperty("download_count",5);assets.add(juce::var(asset));}release->setProperty("assets",assets);rows.add(juce::var(release));}
                const auto text=juce::JSON::toString(rows,true);bytes.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());}
            else {++archiveGets;const auto name=request.url.fromLastOccurrenceOf("/",false,false);const auto file=name==updateArchive.getFileName()?updateArchive:releases.getChildFile(name);if(!file.loadFileAsData(bytes))return RackLibraryStatus::unavailable;}
            response.code=200;return sink(bytes.getData(),bytes.getSize())?RackLibraryStatus::ready:RackLibraryStatus::cancelled;
        });
    }
    RackPackageRef first()const{const auto r=catalog["racks"][0]["releases"][0]["ref"];return {r["id"].toString(),r["version"].toString(),r["sha256"].toString()};}
    bool done(std::uint64_t job){return wait([&]{const auto v=runtime->client->view();return !v->busy&&v->job==job;})&&runtime->client->view()->status==RackLibraryStatus::ready;}
    RackPackageRef makeUpdate(){
        const auto old=first();juce::ZipFile original(releases.getChildFile(old.id+"-"+old.version+".airrack"));juce::ZipFile::Builder zip;
        for(int n=0;n<original.getNumEntries();++n){const auto* entry=original.getEntry(n);auto stream=std::unique_ptr<juce::InputStream>(original.createStreamForEntry(n));juce::MemoryBlock bytes;CHECK(stream&&stream->readIntoMemoryBlock(bytes)>0);
            if(entry->filename=="package.json"){auto manifest=juce::JSON::parse(bytes.toString());manifest.getDynamicObject()->setProperty("version","1.1.0");const auto text=juce::JSON::toString(manifest,true);bytes.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());}
            zip.addEntry(new juce::MemoryInputStream(bytes,true),0,entry->filename,juce::Time(1980,0,1,0,0));}
        CHECK(root.createDirectory().wasOk());updateArchive=root.getChildFile(old.id+"-1.1.0.airrack");auto stream=updateArchive.createOutputStream();CHECK(stream&&zip.writeToStream(*stream,nullptr));stream.reset();juce::MemoryBlock bytes;CHECK(updateArchive.loadFileAsData(bytes));
        RackPackageRef ref{old.id,"1.1.0",juce::SHA256(bytes).toHexString()};auto newer=catalog["racks"][0]["releases"][0].clone();newer["ref"].getDynamicObject()->setProperty("version",ref.version);newer["ref"].getDynamicObject()->setProperty("sha256",ref.sha256);
        auto* archive=newer["archive"].getDynamicObject();archive->setProperty("assetId",100);archive->setProperty("bytes",juce::int64(bytes.getSize()));archive->setProperty("sha256",ref.sha256);archive->setProperty("url",newer["archive"]["url"].toString().upToLastOccurrenceOf("/",true,false)+updateArchive.getFileName());catalog["racks"][0]["releases"].getArray()->add(newer);return ref;
    }
};
// Runs before any other test in a separate process: no prepared/global helper owner exists.
void coldEditor(const juce::File& packages,const juce::File& output){
    Fixture f(packages,output.getChildFile("store-"+juce::Uuid().toString()));const auto old=f.first(),newer=f.makeUpdate();
    auto p=std::make_unique<InstrumentProcessor>(offline(),f.runtime);CHECK(blankInstance(*p));std::unique_ptr<juce::AudioProcessorEditor> editor;
    const auto openEditor=[&]{editor.reset(p->createEditor());editor->setSize(1080,680);editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);editor->setVisible(true);pump();};
    const auto press=[&](const char* id){auto* key=dynamic_cast<juce::Button*>(find(*editor,id));std::cout<<"COLD_ACTION "<<id<<" showing="<<(key&&key->isShowing())<<" enabled="<<(key&&key->isEnabled())<<std::endl;CHECK(key&&key->isShowing()&&key->isEnabled());if(key&&key->isShowing()&&key->isEnabled()){key->triggerClick();pump();}};
    const auto detail=[&]{press("rack.add");auto* lib=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(lib);if(!lib)return lib;CHECK(wait([&]{lib->refresh();return !f.runtime->client->view()->busy&&lib->matchingRows()>17;}));auto* search=dynamic_cast<juce::TextEditor*>(find(*lib,"library.search"));auto* gallery=dynamic_cast<juce::ListBox*>(find(*lib,"library.gallery"));CHECK(search&&gallery);if(search&&gallery){search->setText("Ember Reed",true);CHECK(wait([&]{lib->refresh();return lib->matchingRows()>=1&&lib->matchingRows()<=2;}));pump();gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));}return lib;};
    const auto face=[&](const RackPackageRef& ref){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);bool exact=false;while(std::chrono::steady_clock::now()<end){pump();const auto sound=snapshot(*p);if(sound.personalities&&!sound.personalities->instances.empty()){const auto& rack=sound.personalities->instances.back();const auto owner=p->preparedMakerSkin(rack);std::function<bool(juce::Component&)> shown=[&](auto& c){if(auto* unit=dynamic_cast<RackStackUnit*>(&c);unit&&unit->id()==rack.id&&unit->face())return true;for(auto* child:c.getChildren())if(shown(*child))return true;return false;};exact=owner&&owner->lease&&owner->lease->manifest().ref==ref&&shown(*editor);if(exact)break;}}CHECK(exact);return exact;};
    openEditor();auto* lib=detail();if(!lib)return;const auto empty=snapshot(*p);CHECK(f.runtime->store->active().empty());press("library.install");CHECK(wait([&]{lib->refresh();return dynamic_cast<juce::Button*>(find(*lib,"library.apply"))->isEnabled();}));CHECK(snapshot(*p)==empty);CHECK(f.runtime->store->active()==std::vector<RackPackageRef>{old});press("library.apply");if(!face(old))return;
    const auto placed=snapshot(*p);juce::MemoryBlock saved;p->getStateInformation(saved);editor.reset();openEditor();CHECK(face(old));editor.reset();p.reset();
    // A peer activates v2; offline processor/editor recreation must still recall pinned v1.
    RackPackageStore peer({f.runtime->store->paths().root});CHECK(peer.install(f.updateArchive,newer,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);f.disconnected=true;
    p=std::make_unique<InstrumentProcessor>(offline(),f.runtime);CHECK(p->restoreProjectState(saved.getData(),int(saved.getSize())));openEditor();CHECK(face(old));CHECK(snapshot(*p)==placed);editor.reset();p.reset();pump();
    CHECK(peer.remove(old,{})==RackLibraryStatus::ready);p=std::make_unique<InstrumentProcessor>(offline(),f.runtime);CHECK(p->restoreProjectState(saved.getData(),int(saved.getSize())));openEditor();pump();CHECK(!p->preparedMakerSkin(placed.personalities->instances.front()));CHECK(find(*editor,"rack.focus.0"));
    const auto history=InstrumentDrumTestAccess::history(*p);f.disconnected=false;f.update=true;lib=detail();if(!lib)return;
    auto* gallery=dynamic_cast<juce::ListBox*>(find(*lib,"library.gallery"));CHECK(wait([&]{lib->refresh();return lib->matchingRows()==2;}));gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    auto* restore=dynamic_cast<juce::Button*>(find(*lib,"library.install"));CHECK(restore&&restore->getButtonText()=="RESTORE");press("library.install");CHECK(wait([&]{lib->refresh();return !f.runtime->client->view()->busy;}));press("library.cancel");CHECK(face(old));CHECK(snapshot(*p)==placed&&InstrumentDrumTestAccess::history(*p)==history);CHECK(f.runtime->store->active()==std::vector<RackPackageRef>{newer});
    editor.reset();p.reset();pump();CHECK(peer.remove(old,{})==RackLibraryStatus::ready);
    // Peer installation is observed by the ordinary editor control refresh, without a Library job.
    p=std::make_unique<InstrumentProcessor>(offline(),f.runtime);CHECK(p->restoreProjectState(saved.getData(),int(saved.getSize())));openEditor();pump();CHECK(!p->preparedMakerSkin(placed.personalities->instances.front()));const auto job=f.runtime->client->view()->job;
    CHECK(peer.install(packages.getChildFile(old.id+"-"+old.version+".airrack"),old,RackPackageActivation::preserveActive,{})==RackLibraryStatus::ready);CHECK(face(old));CHECK(f.runtime->client->view()->job==job);CHECK(snapshot(*p)==placed);
    editor.reset();p.reset();p=std::make_unique<InstrumentProcessor>(offline(),f.runtime);CHECK(blankInstance(*p));openEditor();
    AiRequestOptions options;options.instruction="Place installed Ember Reed";options.model="fixture";options.policy.scope=ai::RequestScope::sound;
    std::optional<ai::GenerationRequest> request;CHECK(wait([&]{if(!request)request=p->beginAiRequest(options,120);return request.has_value();}));if(!request||!request->catalog)return;
    ai::GenerationResult selected;selected.tag=request->tag;selected.status=ai::GenerationStatus::catalogSelectionRequired;selected.inferenceCalls=1;selected.proposalStatus=ai::ProposalStatus::accepted;selected.providerStatus=rack::oauth::Status::connected;selected.httpStatus=200;
    selected.catalog=ai::CatalogResultBinding{request->catalog->root,request->catalog->authority->snapshot};const auto m=f.catalog["racks"][0]["releases"][0]["metadata"];
    selected.edits.name="Cold AI Ember";selected.edits.characteristics="warm";selected.edits.explanation="Installed native fixture";selected.edits.catalogSelections={{7,juce::Uuid(m["design"].toString()),std::uint64_t(m["revision"].toString().getLargeIntValue()),m["canonical_sha256"].toString(),{RackRole::synth,masterChain,0}}};
    CHECK(p->receiveAiResult(std::move(selected)));const auto* materialize=p->aiCurrentRequest();CHECK(materialize&&materialize->catalog&&materialize->catalog->phase==ai::CatalogPhase::materialize);if(!materialize||materialize->catalog->phase!=ai::CatalogPhase::materialize)return;
    auto result=ai::generateProposal(*materialize,[](const auto&,auto,const auto&){return rack::oauth::StructuredResult{};});CHECK(result.status==ai::GenerationStatus::ready);CHECK(p->receiveAiResult(std::move(result)));bool applied=false;CHECK(wait([&]{if(!applied)applied=p->applyAiProposal();return applied;}));CHECK(face(newer));CHECK(snapshot(*p).personalities->instances.back().package==newer);
    std::cout<<"COLD_EDITOR Install/Add/editor+processor offline pinned recall/Restore/peer generation/AI exact faces covered"<<std::endl;
}

void sequence(const juce::File& packages,const juce::File& output){
    Fixture fixture(packages,output.getChildFile("sequence"));const auto old=fixture.first(),newer=fixture.makeUpdate();auto& client=*fixture.runtime->client;
    auto p=std::make_unique<InstrumentProcessor>(offline(),fixture.runtime);const auto empty=snapshot(*p);CHECK(fixture.gets==0&&fixture.runtime->store->active().empty());
    CHECK(fixture.done(client.refresh()));CHECK(client.view()->releases.size()==11);CHECK(fixture.archiveGets==0&&fixture.runtime->store->active().empty());CHECK(snapshot(*p)==empty);
    CHECK(fixture.done(client.install(old)));CHECK(snapshot(*p)==empty);CHECK(fixture.archiveGets==1);
    auto local=inventory(*p);CHECK(local&&local->rows().size()==18);if(!local)return;
    const auto row=std::find_if(local->rows().begin(),local->rows().end(),[&](const auto& r){return r.package==old;});CHECK(row!=local->rows().end());if(row==local->rows().end())return;
    RackDesign design;std::optional<RackPackageRef> ref;CHECK(resolveInventoryDesign(*local,row->metadata.design,row->metadata.revision,row->metadata.canonicalSha256,design,ref)&&ref==old);
    auto prep=client.createPlacementPreparation();client.requestPlacement(prep,local,std::span(&old,1));CHECK(wait([&]{return client.placement(prep)->status==RackLibraryStatus::ready;}));
    RackReviewRequest request;request.localDesign=design;request.package=old;request.position=0;auto reviewed=reviewRack(*p,request);CHECK(applyRackReview(*p,reviewed,prep,client.placement(prep)).applied);
    client.cancelPlacement(prep);prep.reset();local.reset();const auto placed=snapshot(*p);CHECK(placed.personalities->instances[0].package==old);juce::MemoryBlock saved;p->getStateInformation(saved);
    auto owner=std::async(std::launch::async,[&]{RackInventoryPtr current;captureRackInventory(*fixture.runtime->store,placed.personalities->instances,current);return studio::prepareMakerSkin(placed.personalities->instances[0],current.get());}).get();CHECK(owner&&owner->lease->manifest().ref==old);
    fixture.update=true;CHECK(fixture.done(client.refresh()));CHECK(fixture.done(client.install(newer)));CHECK(fixture.runtime->store->active()==std::vector<RackPackageRef>{newer});CHECK(snapshot(*p)==placed);
    p.reset();p=std::make_unique<InstrumentProcessor>(offline(),fixture.runtime);CHECK(p->restoreProjectState(saved.getData(),int(saved.getSize())));CHECK(snapshot(*p)==placed);CHECK(fixture.runtime->store->open(old)!=nullptr);
    CHECK(fixture.runtime->store->remove(old,{})==RackLibraryStatus::busy);owner.reset();p.reset();pump();CHECK(fixture.done(client.remove(old)));CHECK(fixture.runtime->store->open(old)==nullptr);
    p=std::make_unique<InstrumentProcessor>(offline(),fixture.runtime);CHECK(p->restoreProjectState(saved.getData(),int(saved.getSize())));const auto fallback=snapshot(*p);CHECK(fallback==placed);
    {auto current=inventory(*p);CHECK(!std::async(std::launch::async,[&]{return studio::prepareMakerSkin(fallback.personalities->instances[0],current.get());}).get());RackStackUnit unit(*p,*studio::studioSkin(),fallback.personalities->instances[0].id);unit.refresh(fallback);CHECK(unit.face()==nullptr);CHECK(find(unit,"rack.focus.0")!=nullptr);}
    CHECK(fixture.done(client.restore(old)));CHECK(fixture.runtime->store->active()==std::vector<RackPackageRef>{newer});CHECK(snapshot(*p)==placed);
    auto restored=std::async(std::launch::async,[&]{RackInventoryPtr current;captureRackInventory(*fixture.runtime->store,placed.personalities->instances,current);return studio::prepareMakerSkin(placed.personalities->instances[0],current.get());}).get();CHECK(restored&&restored->lease->manifest().ref==old);
    CHECK(fixture.archiveGets==3);std::cout<<"SEQUENCE explicit catalog/download/Install/Add/save/update/pinned-old/remove/fallback/exact-Restore\n";
}
void allPackages(const juce::File& packages,const juce::File& output){
    Fixture f(packages,output.getChildFile("all11"));InstrumentProcessor p(offline(),f.runtime);
    for(const auto& row:*f.catalog["racks"].getArray()){const auto r=row["releases"][0]["ref"];RackPackageRef ref{r["id"].toString(),r["version"].toString(),r["sha256"].toString()};
        const auto installed=std::async(std::launch::async,[&]{return f.runtime->store->install(packages.getChildFile(ref.id+"-"+ref.version+".airrack"),ref,RackPackageActivation::makeActive,{});}).get();
        std::cout<<"INSTALL "<<ref.id<<" status="<<int(installed)<<std::endl;CHECK(installed==RackLibraryStatus::ready);
    }
    CHECK(f.runtime->store->active().size()==11);auto local=inventory(p);CHECK(local&&local->rows().size()==28);if(!local)return;
    int makers=0,skins=0;for(const auto& row:local->rows())if(row.package){++makers;RackInstance rack;std::optional<RackPackageRef> ref;
        CHECK(resolveInventoryDesign(*local,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,rack.design,ref)&&ref==row.package);rack.package=ref;
        auto owner=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(rack,local.get());}).get();
        CHECK(bool(owner)==(ref->id!="contour-dynamics"));if(owner){++skins;CHECK(owner->lease&&owner->lease->manifest().ref==ref);
            if(ref->id=="ember-reed"||ref->id=="night-dial"){const auto id=ref->id=="ember-reed"?"face.vel-air":"face.sweep";const auto source=ref->id=="ember-reed"?"Velocity":"LFO 1";const auto found=std::find_if(owner->skin.elements.begin(),owner->skin.elements.end(),[&](const auto& e){return e.id==id;});CHECK(found!=owner->skin.elements.end()&&found->printedRoute.source==source);}
            for(const auto& e:owner->skin.elements)if(e.rackUnit>=0){const auto unit=std::find_if(rack.design.units.begin(),rack.design.units.end(),[&](const auto& u){return u.id==e.rackUnit;});CHECK(unit!=rack.design.units.end());if(unit!=rack.design.units.end())CHECK(rackParameterIndex(unit->module,0,e.key).has_value());}
        }
    }
    CHECK(makers==11&&skins==10);CHECK(f.gets==0);
    std::cout<<"INVENTORY 17 AIR/11 installed/10 maker skins/Contour house, no GET\n";
}
void stagedDestruction(const juce::File& packages,const juce::File& output){
    Fixture f(packages,output.getChildFile("destruction"));
    auto p=std::make_unique<InstrumentProcessor>(offline(),f.runtime);auto peer=std::make_unique<InstrumentProcessor>(offline(),f.runtime);
    auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(f.gets==0);f.hold=true;const auto job=f.runtime->client->refresh();CHECK(wait([&]{return f.entered.load();}));
    editor.reset();p.reset();CHECK(peer->rackLibraryRuntime()==f.runtime);f.runtime->client->cancel(job);f.hold=false;CHECK(wait([&]{return !f.runtime->client->view()->busy;}));peer.reset();CHECK(f.runtime->store->active().empty());
    // Two processors share one committed index; the second writer never publishes a competing partial tree.
    auto first=std::make_unique<InstrumentProcessor>(offline(),f.runtime),second=std::make_unique<InstrumentProcessor>(offline(),f.runtime);const auto ref=f.first();
    auto one=std::async(std::launch::async,[&]{return f.runtime->store->install(packages.getChildFile(ref.id+"-"+ref.version+".airrack"),ref,RackPackageActivation::makeActive,{});});
    CHECK(std::async(std::launch::async,[&]{RackPackageStore writer(f.runtime->store->paths());const auto status=writer.install(packages.getChildFile(ref.id+"-"+ref.version+".airrack"),ref,RackPackageActivation::makeActive,{});return status==RackLibraryStatus::ready||status==RackLibraryStatus::busy;}).get());
    const auto status=one.get();CHECK(status==RackLibraryStatus::ready||status==RackLibraryStatus::busy);CHECK(f.runtime->store->active()==std::vector<RackPackageRef>{ref});
    CHECK(inventory(*first)->rows().size()==18&&inventory(*second)->rows().size()==18);CHECK(snapshot(*first)==snapshot(*second));
    Fixture last(packages,output.getChildFile("last-owner"));auto finalProcessor=std::make_unique<InstrumentProcessor>(offline(),last.runtime);std::weak_ptr<RackLibraryRuntime> weak=last.runtime;
    last.hold=true;last.runtime->client->refresh();CHECK(wait([&]{return last.entered.load();}));last.runtime.reset();finalProcessor.reset();CHECK(weak.expired());
}
void baselineCore(const juce::File& output){
    const auto base=juce::File(RACKSYNTH_WORKSPACE).getChildFile("tests/fixtures/rack-library-product/v1");
    const auto map=juce::JSON::parse(base.getChildFile("capture-identities.json"));
    for(const auto& row:*map["captures"].getArray()){
        const auto id=row["id"].toString();auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{output.getChildFile("absent-"+id)});
        std::atomic<int> requests{0};runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[&](const auto&,const auto&,const auto&,auto&){++requests;return RackLibraryStatus::networkFailure;});InstrumentProcessor p(offline(),runtime);
        juce::MemoryBlock state;CHECK(base.getChildFile(id+"/project.state").loadFileAsData(state));CHECK(juce::SHA256(state).toHexString()==row["state_sha256"].toString());CHECK(p.restoreProjectState(state.getData(),int(state.getSize())));
        const auto restored=snapshot(p);CHECK(restored.personalities&&!restored.personalities->instances.empty());compareAudio(p,base.getChildFile(id+"/baseline.f32"),id=="tile-array");CHECK(requests==0&&runtime->store->active().empty());
    }
}
void guardedSwap(const juce::File& packages,const juce::File& output){
    Fixture f(packages,output.getChildFile("swap"));const auto ref=f.first();CHECK(f.done(f.runtime->client->refresh()));CHECK(f.done(f.runtime->client->install(ref)));InstrumentProcessor p(offline(),f.runtime);auto local=inventory(p);
    RackDesign design;std::optional<RackPackageRef> selected;const auto& row=*std::find_if(local->rows().begin(),local->rows().end(),[&](const auto& r){return r.package==ref;});CHECK(resolveInventoryDesign(*local,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,design,selected));
    auto prep=f.runtime->client->createPlacementPreparation();f.runtime->client->requestPlacement(prep,local,std::span(&ref,1));CHECK(wait([&]{return f.runtime->client->placement(prep)->status==RackLibraryStatus::ready;}));
    RackReviewRequest request;request.localDesign=design;request.package=ref;request.position=0;auto add=applyRackReview(p,reviewRack(p,request),prep,f.runtime->client->placement(prep));CHECK(add.applied&&add.placed);if(!add.placed)return;
    auto edited=snapshot(p);edited.values[*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff")]=.7f;std::uint64_t edit=0;SoundDocument observed;CHECK(p.trySoundSnapshot(observed,edit));CHECK(p.applySoundEdit(edited,SoundEditOrigin::manual,edit));
    const auto before=snapshot(p);request.operation=4;request.selected=*add.placed;request.reuse=true;const auto reviewed=reviewRack(p,request);CHECK(!reviewed.draft.empty());auto result=applyRackReview(p,reviewed,prep,f.runtime->client->placement(prep));CHECK(result.applied);CHECK(!result.placed);
    if(result.applied){CHECK(snapshot(p).personalities->instances[0].id==*add.placed);CHECK(p.undoSoundEdit());CHECK(snapshot(p)==before);}
    const auto stale=reviewRack(p,request);juce::MemoryBlock state;CHECK(encodeSoundState(before,state));CHECK(p.restoreSoundState(state.getData(),int(state.getSize())));const auto history=InstrumentDrumTestAccess::history(p);
    CHECK(!applyRackReview(p,stale,prep,f.runtime->client->placement(prep)).applied);CHECK(snapshot(p)==before&&InstrumentDrumTestAccess::history(p)==history);
    const auto old=f.runtime->client->placement(prep);const auto stable=reviewRack(p,request);const auto r=f.catalog["racks"][1]["releases"][0]["ref"];RackPackageRef other{r["id"].toString(),r["version"].toString(),r["sha256"].toString()};
    CHECK(std::async(std::launch::async,[&]{return f.runtime->store->install(packages.getChildFile(other.id+"-"+other.version+".airrack"),other,RackPackageActivation::makeActive,{});}).get()==RackLibraryStatus::ready);
    CHECK(!applyRackReview(p,stable,prep,old).applied);CHECK(snapshot(p)==before&&InstrumentDrumTestAccess::history(p)==history);
}
void panelReadiness(const juce::File& packages,const juce::File& output){
    Fixture f(packages,output.getChildFile("panel-readiness"));const auto ref=f.first();CHECK(f.done(f.runtime->client->refresh()));CHECK(f.done(f.runtime->client->install(ref)));InstrumentProcessor p(offline(),f.runtime);auto local=inventory(p);
    RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);auto* choices=dynamic_cast<juce::ComboBox*>(find(panel,"rack.examples"));auto* apply=dynamic_cast<juce::Button*>(find(panel,"rack.apply"));CHECK(choices&&apply);if(!choices||!apply)return;
    int selected=0;CHECK(wait([&]{for(int n=0;n<choices->getNumItems();++n)if(choices->getItemText(n)=="Ember Reed")selected=choices->getItemId(n);return selected!=0;}));
    f.hold=true;const auto job=f.runtime->client->refresh();CHECK(wait([&]{return f.entered.load();}));choices->setSelectedId(selected,juce::sendNotificationSync);auto* position=dynamic_cast<juce::ComboBox*>(find(panel,"rack.position"));CHECK(position);if(position)position->setSelectedId(1,juce::sendNotificationSync);pump();pump();const auto before=snapshot(p);
    CHECK(!apply->isEnabled());CHECK(snapshot(p)==before);f.hold=false;CHECK(f.done(job));CHECK(wait([&]{return apply->isEnabled();}));apply->triggerClick();pump();pump();
    const auto placed=snapshot(p);CHECK(placed.personalities&&placed.personalities->instances.size()==1&&placed.personalities->instances[0].package==ref);
}
void focusTransition(const juce::File& packages,const juce::File& output){
    Fixture f(packages,output.getChildFile("transition"));const auto ref=f.first();CHECK(std::async(std::launch::async,[&]{return f.runtime->store->install(packages.getChildFile(ref.id+"-"+ref.version+".airrack"),ref,RackPackageActivation::makeActive,{});}).get()==RackLibraryStatus::ready);
    CHECK(f.done(f.runtime->client->refresh()));f.hold=true;const auto job=f.runtime->client->refresh();CHECK(wait([&]{return f.entered.load();}));InstrumentProcessor p(offline(),f.runtime);
    RackOperation op;op.design=frozenCurrent()[0];op.package=ref;op.newInstance=juce::Uuid();op.destination.position=0;CHECK(p.applyRackOperation(op,0)==RackCandidateStatus::ready);const auto before=snapshot(p);const auto history=InstrumentDrumTestAccess::history(p);
    RackInventoryPtr local;CHECK(std::async(std::launch::async,[&]{return captureRackInventory(*f.runtime->store,before.personalities->instances,local);}).get());p.prepareRackSkins(local,before.personalities->instances);
    RackStackUnit unit(p,*studio::studioSkin(),op.newInstance);unit.refresh(before);unit.setBounds(0,0,1547,411);unit.addToDesktop(juce::ComponentPeer::windowIsTemporary);unit.setVisible(true);pump();
    CHECK(unit.face()==nullptr);auto* control=find(unit,"rack.focus.0");CHECK(control&&control->isShowing());if(control){control->grabKeyboardFocus();CHECK(control->hasKeyboardFocus(true));}
    f.hold=false;CHECK(f.done(job));CHECK(wait([&]{unit.refresh(before);return unit.face()!=nullptr;}));
    auto* focused=juce::Component::getCurrentlyFocusedComponent();std::cout<<"FOCUS_TRANSITION actual="<<(focused?focused->getComponentID():"none")<<std::endl;
    CHECK(focused&&focused->isShowing()&&(focused==&unit.fold||(unit.face()&&unit.face()->isParentOf(focused))));
    CHECK(snapshot(p)==before&&InstrumentDrumTestAccess::history(p)==history);
}
void galleryCaptures(const juce::File& packages,const juce::File& output,const juce::File& fixtures){
    Fixture f(packages,fixtures.getChildFile("gallery-store"));InstrumentProcessor p(offline(),f.runtime);const auto before=snapshot(p);
    auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());editor->setSize(1080,680);editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);editor->setVisible(true);
    auto* add=dynamic_cast<juce::Button*>(find(*editor,"rack.add"));CHECK(add);if(!add)return;add->triggerClick();pump();
    auto* library=dynamic_cast<studio::LibraryUnit*>(find(*editor,"library.panel"));CHECK(library);if(!library)return;
    CHECK(wait([&]{library->refresh();return library->matchingRows()==28&&!f.runtime->client->view()->busy;}));CHECK(f.runtime->store->active().empty());
    auto* gallery=dynamic_cast<juce::ListBox*>(find(*library,"library.gallery"));auto* search=dynamic_cast<juce::TextEditor*>(find(*library,"library.search"));CHECK(gallery&&search);if(!gallery||!search)return;
    const auto capture=[&](const juce::String& name){auto stream=output.getChildFile(name).createOutputStream();CHECK(stream&&juce::PNGImageFormat().writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds(),true,1.f),*stream));};
    for(const auto size:{juce::Point<int>(1080,680),juce::Point<int>(1600,1000)}){
        editor->setSize(size.x,size.y);search->setText({},true);CHECK(wait([&]{library->refresh();return library->matchingRows()==28;}));gallery->getViewport()->setViewPosition(0,100000);pump();
        for(const auto& row:*f.catalog["racks"].getArray()){const auto r=row["releases"][0]["ref"];f.runtime->client->requestThumbnail({r["id"].toString(),r["version"].toString(),r["sha256"].toString()});}
        CHECK(wait([&]{library->refresh();const auto image=f.runtime->client->thumbnail(f.first());return image&&image->status==RackLibraryStatus::ready;}));
        capture("gallery-"+juce::String(size.x)+"x"+juce::String(size.y)+".png");search->setText("Ember Reed",true);CHECK(wait([&]{library->refresh();return library->matchingRows()==1;}));gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump();
        CHECK(library->browserState().detailOpen);CHECK(wait([&]{const auto image=f.runtime->client->thumbnail(f.first(),2048);return image&&image->status==RackLibraryStatus::ready;}));library->refresh();capture("detail-"+juce::String(size.x)+"x"+juce::String(size.y)+".png");auto* back=dynamic_cast<juce::Button*>(find(*library,"library.back"));CHECK(back);if(back){back->triggerClick();pump();}
    }
    CHECK(snapshot(p)==before&&f.runtime->store->active().empty());CHECK(output.getChildFile("capture-scope.json").replaceWithText("{\"catalog\":\"UNPUBLISHED private candidate\",\"counts\":\"injected fixture5, not observed public downloads\",\"screenshots\":\"real11 native package captures\",\"installed\":0}"));
}
int bundleChild(const juce::File& bundle,const juce::File& output){
    // This path deliberately has no injected root, catalog, editor, transfer or provider request.
    const auto production=RackLibraryPaths::forCurrentUser().root;juce::MemoryBlock before;const bool hadIndex=production.getChildFile("installed.json").existsAsFile();production.getChildFile("installed.json").loadFileAsData(before);
    const auto installed=juce::JSON::parse(before.toString());std::cout<<"PRODUCTION_STORE read-only index_exists="<<hadIndex<<" active_records="<<installed["active"].size()<<" (no clean-store assumption; no Store operation/recovery)\n";
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;format.findAllTypesForFile(descriptions,bundle.getFullPathName());CHECK(descriptions.size()==1);if(descriptions.size()!=1)return 1;
    const auto base=juce::File(RACKSYNTH_WORKSPACE).getChildFile("tests/fixtures/rack-library-product/v1");const auto map=juce::JSON::parse(base.getChildFile("capture-identities.json"));
    for(const auto& row:*map["captures"].getArray())for(int missing=0;missing<2;++missing){
        const auto id=row["id"].toString();juce::String error;auto plugin=format.createInstanceFromDescription(*descriptions[0],48000,128,error);CHECK(plugin!=nullptr);if(!plugin)return 1;
        // The unchanged JUCE VST3 wrapper adds bypass plus 16 channels of130 MIDI controllers.
        CHECK(plugin->getParameters().size()==4519+1+16*130);CHECK(plugin->getTotalNumInputChannels()==0);CHECK(plugin->getLatencySamples()==0);
        if(id=="ember-reed"&&missing==0){std::map<juce::String,juce::AudioProcessorParameter*> hosted;for(auto* parameter:plugin->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter))CHECK(hosted.emplace(named->getParameterID(),parameter).second);
            juce::StringArray ids;for(const auto& descriptor:parameterRegistry()){const auto key=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));const auto found=hosted.find(key);CHECK(found!=hosted.end());if(found!=hosted.end()){CHECK(found->second->getName(256)==descriptor.name);CHECK(found->second->getLabel()==descriptor.unit);CHECK(found->second->isAutomatable()==descriptor.automatable);ids.add(key);}}
            CHECK(ids.size()==4519);CHECK(output.getChildFile("vst3-parameter-ids.txt").replaceWithText(ids.joinIntoString("\n")));std::cout<<"HOST owned_controls="<<ids.size()<<" hosted_parameters="<<plugin->getParameters().size()<<" (4519 AIR + bypass +2080 JUCE MIDI controls)\n";}
        juce::MemoryBlock state;CHECK(base.getChildFile(id+"/project.state").loadFileAsData(state));
        if(missing){ProjectDocument project;drums::DrumBundlePtr drum;drums::DrumError drumError;
            if(drums::decodeDrumBundle({static_cast<const std::uint8_t*>(state.getData()),state.getSize()},drum,drumError)){const auto compact=drum->compactDocument();CHECK(decodeProjectState(compact.data(),compact.size(),project));}
            else CHECK(decodeProjectState(state.getData(),state.getSize(),project));
            auto personalities=std::make_shared<RackPersonalityState>(*project.sound.personalities);for(auto& rack:personalities->instances)if(rack.design.name==row["id"].toString().replaceCharacter('-',' ')||legacyRackPackageRef(rack.design))rack.package=RackPackageRef{id,"99.0.0",juce::String::repeatedString("e",64)};project.sound.personalities=personalities;
            juce::MemoryBlock compact;CHECK(encodeProjectState(project,compact));if(drum){drums::DrumDocumentPtr document;CHECK(drums::makeDrumDocument(drum->document().slots(),document,drumError));drums::DrumBundlePtr changed;CHECK(drums::makeDrumBundle(document,{static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()},drum->assets(),changed,drumError));CHECK(drums::encodeDrumBundle(*changed,state,drumError));}else state=std::move(compact);
        }
        juce::XmlElement xml("VST3PluginState");xml.createNewChildElement("IComponent")->addTextElement(state.toBase64Encoding());juce::MemoryBlock envelope;juce::AudioProcessor::copyXmlToBinary(xml,envelope);
        plugin->setStateInformation(envelope.getData(),int(envelope.getSize()));juce::MemoryBlock returned;plugin->getStateInformation(returned);
        const auto recalled=juce::AudioProcessor::getXmlFromBinary(returned.getData(),int(returned.getSize()));CHECK(recalled&&recalled->hasTagName("VST3PluginState"));
        juce::MemoryBlock component;const auto* child=recalled?recalled->getChildByName("IComponent"):nullptr;CHECK(child&&component.fromBase64Encoding(child->getAllSubText()));
        CHECK(component.getSize()>=state.getSize()&&std::memcmp(component.getData(),state.getData(),state.getSize())==0);
        compareAudio(*plugin,base.getChildFile(id+"/baseline.f32"),id=="tile-array");
    }
    juce::MemoryBlock after;production.getChildFile("installed.json").loadFileAsData(after);CHECK(production.getChildFile("installed.json").existsAsFile()==hadIndex&&before==after);
    return failures?1:0;
}
std::vector<RackDesign> frozenCurrent() {
    std::vector<RackDesign> result;
    const auto manifest=juce::JSON::parse(juce::File(RACKSYNTH_PACKAGE_FIXTURE_DIR).getChildFile("manifest.json"));
    if(const auto* entries=manifest["current"].getArray())for(const auto& entry:*entries){
        juce::MemoryBlock bytes;RackDesign design;
        CHECK(juce::File(RACKSYNTH_PACKAGE_FIXTURE_DIR).getChildFile(entry["path"].toString()).loadFileAsData(bytes));
        CHECK(juce::SHA256(bytes).toHexString()==entry["sha256"].toString());
        CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),design,RackValidationMode::instanceRecall));
        result.push_back(std::move(design));
    }
    return result;
}
void fresh(const juce::File& root) {
    std::atomic<int> requests{0};
    auto runtime=std::make_shared<RackLibraryRuntime>();
    runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});
    runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[&](const auto&,const auto&,const auto&,auto&){++requests;return RackLibraryStatus::networkFailure;});
    auto& freshStore=*runtime->store;InstrumentProcessor freshProcessor(runtime);
    CHECK(factoryRackDesigns().size()==17);
    CHECK(libraryRackDesigns().size()==17);
    CHECK(freshStore.active().empty());
    CHECK(freshProcessor.getTotalNumInputChannels()==0);
    CHECK(freshProcessor.getParameters().size()==4519);
    const auto frozen=frozenCurrent();CHECK(frozen.size()==28);
    for(const auto& actual:factoryRackDesigns()){const auto original=std::find_if(frozen.begin(),frozen.end(),[&](const auto& d){return d.id==actual.id;});CHECK(original!=frozen.end()&&actual==*original);}
    // Historical files are25 unique preexisting recipes, referenced by68 frozen source records.
    CHECK(test::historicalRackFixtures().size()==25);
    SoundDocument sound;std::uint64_t revision=0;
    CHECK(freshProcessor.trySoundSnapshot(sound,revision));
    CHECK(!sound.personalities||sound.personalities->instances.empty());
    CHECK(sound.layout.synthCount==0);
    CHECK(requests==0);
    juce::MemoryBlock old;
    CHECK(juce::File(RACKSYNTH_WORKSPACE).getChildFile("tests/fixtures/rack-package-recall/sound30.soundstate").loadFileAsData(old));
    SoundDocument legacy;CHECK(decodeSoundState(old.getData(),old.getSize(),legacy));
    CHECK(legacy.personalities&&legacy.personalities->instances[0].package.has_value());
    auto changed=legacy.personalities->instances[0].design;changed.name="Edited legacy recipe";CHECK(!legacyRackPackageRef(changed));
    changed=legacy.personalities->instances[0].design;++changed.revision;CHECK(!legacyRackPackageRef(changed));
    auto noRef=legacy;auto plain=std::make_shared<RackPersonalityState>(*legacy.personalities);plain->instances[0].package.reset();noRef.personalities=plain;
    juce::MemoryBlock current;CHECK(encodeSoundState(noRef,current));SoundDocument roundTrip;
    CHECK(decodeSoundState(current.getData(),current.getSize(),roundTrip)&&roundTrip==noRef);
    auto explicitRef=noRef;plain=std::make_shared<RackPersonalityState>(*plain);plain->instances[0].package=RackPackageRef{"ember-reed","9.0.0",juce::String::repeatedString("d",64)};explicitRef.personalities=plain;
    CHECK(encodeSoundState(explicitRef,current));CHECK(decodeSoundState(current.getData(),current.getSize(),roundTrip)&&roundTrip==explicitRef);
    CHECK(freshProcessor.restoreSoundState(old.getData(),int(old.getSize())));
    CHECK(freshProcessor.trySoundSnapshot(sound,revision));
    CHECK(sound.personalities&&sound.personalities->instances.size()==1);
    juce::MemoryBlock plainState;CHECK(encodeSoundState(noRef,plainState));InstrumentProcessor plainProcessor(offline(),runtime);CHECK(plainProcessor.restoreSoundState(plainState.getData(),int(plainState.getSize())));
    CHECK(render(freshProcessor)==render(plainProcessor));
    auto edited=sound;edited.values[756]=.41f;
    CHECK(freshProcessor.applySoundEdit(edited,SoundEditOrigin::manual,revision));
    CHECK(freshProcessor.trySoundSnapshot(sound,revision)&&sound==edited);
    CHECK(freshStore.active().empty());CHECK(requests==0);
}
}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const bool onlyPanel=argc==6&&juce::String(argv[1])=="--only-panel";
    const bool onlyPackages=argc==6&&juce::String(argv[1])=="--only-packages";
    const bool onlyGallery=argc==6&&juce::String(argv[1])=="--only-gallery";
    const bool onlySwap=argc==6&&juce::String(argv[1])=="--only-swap",onlyFocus=argc==6&&juce::String(argv[1])=="--only-focus";
    const bool cold=argc==6&&(juce::String(argv[1])=="--cold-editor"||juce::String(argv[1])=="--cold-editor-child");
    const bool core=argc==6&&(cold||juce::String(argv[1])=="--core"||onlySwap||onlyFocus||onlyGallery||onlyPackages||onlyPanel);
    const bool hosted=argc==7&&(juce::String(argv[1])=="--bundle"||juce::String(argv[1])=="--bundle-child");
    const int at=core?2:3;
    if((!core&&!hosted)||juce::String(argv[at])!="--fixtures"||juce::String(argv[at+2])!="--output"
       ||!juce::File::isAbsolutePath(argv[at+1])||!juce::File::isAbsolutePath(argv[at+3])||!juce::File(argv[at+1]).getChildFile("catalog.json").existsAsFile())return 2;
    const juce::File packages(argv[at+1]),output(argv[at+3]);CHECK(output.createDirectory().wasOk());
    const auto runId=juce::Uuid().toString();
    const auto fixtures=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("airack-rack-library-core-"+runId);
    if(core){CHECK(!fixtures.exists());CHECK(fixtures.createDirectory().wasOk());if(!fixtures.isDirectory())return 2;
        CHECK(output.getChildFile("core-run-"+runId+".txt").replaceWithText("mutable_fixture_root="+fixtures.getFullPathName()+"\n"));
        std::cout<<"CORE_FIXTURE "<<fixtures.getFullPathName()<<std::endl;}
    if(cold){if(juce::String(argv[1])=="--cold-editor-child")coldEditor(packages,fixtures);
        else{juce::ChildProcess child;CHECK(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--cold-editor-child","--fixtures",packages.getFullPathName(),"--output",output.getFullPathName()}));CHECK(child.waitForProcessToFinish(240000));const auto log=child.readAllProcessOutput();std::cout<<log;CHECK(output.getChildFile("cold-child-"+runId+".log").replaceWithText(log));CHECK(child.getExitCode()==0);}}
    else if(onlyPanel)panelReadiness(packages,fixtures);
    else if(onlyPackages)allPackages(packages,fixtures);
    else if(onlyGallery)galleryCaptures(packages,output,fixtures);
    else if(onlySwap)guardedSwap(packages,fixtures);
    else if(onlyFocus)focusTransition(packages,fixtures);
    else if(core){fresh(fixtures.getChildFile("fresh"));focusTransition(packages,fixtures);sequence(packages,fixtures.getChildFile("lifecycle"));guardedSwap(packages,fixtures);allPackages(packages,fixtures);stagedDestruction(packages,fixtures);baselineCore(fixtures);}
    else if(juce::String(argv[1])=="--bundle-child"){bundleChild(juce::File(argv[2]),output);}
    else{CHECK(juce::File::isAbsolutePath(argv[2])&&juce::File(argv[2]).isDirectory());juce::ChildProcess child;
        CHECK(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--bundle-child",argv[2],"--fixtures",packages.getFullPathName(),"--output",output.getFullPathName()}));
        CHECK(child.waitForProcessToFinish(500000));const auto log=child.readAllProcessOutput();std::cout<<log;CHECK(output.getChildFile("bundle-child.log").replaceWithText(log));CHECK(child.getExitCode()==0);
    }
    if(core&&failures==0)CHECK(fixtures.deleteRecursively());
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
