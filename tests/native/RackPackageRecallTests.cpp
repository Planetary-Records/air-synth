#include "FrozenRackExamples.h"
#include "RackReconciliation.h"
#include "RackExamples.h"
#include "SoundDocument.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include "MakerFace.h"
#include "RackPackageFixtures.h"
#include "PreparedMakerSkins.h"
#include "InstrumentProcessor.h"
#include "RackReview.h"
#include "RackStack.h"
#include "StudioSkin.h"
#include "TileArrayFace.h"
#include "CompanionWire.h"
#include "DrumBundle.h"
#include "UserReach.h"
#include <future>
#include <map>
#include <set>
#include <thread>
#include <cstring>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
using namespace airack;
namespace airack {struct InstrumentDrumTestAccess {static auto history(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return std::pair(p.editHistory->count,p.editHistory->cursor);}};}
namespace {
int checks=0,failures=0;
#define CHECK(x) do { ++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<": "<<#x<<std::endl;} } while(false)
const RackPackageRef exact{"ember-reed","1.0.0",juce::String::repeatedString("a",64)};
juce::File fixtures(){return juce::File(RACKSYNTH_RECALL_FIXTURE_DIR);}
SoundDocument placed() {
    SoundDocument before,out;RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[0];op.newInstance=juce::Uuid("12345678-1234-4234-9234-123456789000");RackDifference diff;
    CHECK(makeRackCandidate(before,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);return out;
}
void references() {
    CHECK(validRackPackageRef(exact));
    auto sound=placed();auto state=std::make_shared<RackPersonalityState>(*sound.personalities);state->instances[0].package=exact;sound.personalities=state;
    juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));SoundDocument restored;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),restored));CHECK(restored==sound);
    CHECK(restored.values==sound.values&&restored.compatibility==sound.compatibility);
    ai::GenerationRequest request;request.tag={juce::Uuid(),1,1,1};request.model="account-selected_model:1.0";request.instruction="Keep this exact rack";request.source.sound=sound;CHECK(companion::encodeRequest(request,bytes));ai::GenerationRequest reply;CHECK(companion::decodeRequest({static_cast<const std::byte*>(bytes.getData()),bytes.getSize()},reply));CHECK(reply.source.sound==sound);
    ProjectDocument project;project.sound=sound;CHECK(encodeProjectState(project,bytes));ProjectDocument decoded;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),decoded));CHECK(decoded==project);
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=state->instances[0].id;duplicate.newInstance=juce::Uuid();RackDifference diff;SoundDocument copied;
    CHECK(makeRackCandidate(sound,duplicate,SoundEditOrigin::manual,copied,diff)==RackCandidateStatus::ready);CHECK(copied.personalities&&copied.personalities->instances.back().package==exact);
    RackOperation detach;detach.kind=RackOperationKind::detach;detach.selected=state->instances[0].id;SoundDocument detached;CHECK(makeRackCandidate(copied,detach,SoundEditOrigin::manual,detached,diff)==RackCandidateStatus::ready);CHECK(detached.personalities&&detached.personalities->instances.size()==1&&detached.personalities->instances[0].package==exact);
    auto bad=std::make_shared<RackPersonalityState>(*state);bad->instances[0].package->sha256="wrong";auto invalid=sound;invalid.personalities=bad;
    juce::MemoryBlock sentinel("keep",4),saved=sentinel;CHECK(!encodeSoundState(invalid,sentinel)&&sentinel==saved);
}
std::uint32_t u32(const juce::MemoryBlock& b,std::size_t at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(static_cast<const unsigned char*>(b.getData())[at+i])<<(8*i);return value;}
void put(juce::MemoryBlock& b,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)static_cast<unsigned char*>(b.getData())[at+i]=static_cast<unsigned char>(value>>(8*i));}
void sign(juce::MemoryBlock& b){const auto hash=juce::SHA256(b.getData(),b.getSize()-32).getRawData();std::memcpy(static_cast<char*>(b.getData())+b.getSize()-32,hash.getData(),32);}
void frozenAndInvalid() {
    juce::MemoryBlock old;CHECK(fixtures().getChildFile("sound30.soundstate").loadFileAsData(old));CHECK(juce::SHA256(old).toHexString()=="34686a43fd9a40aa17a4edc793f11ef05649b2b4a9790b8501e3c8e820746dae");
    SoundDocument sound;CHECK(decodeSoundState(old.getData(),old.getSize(),sound));CHECK(sound.personalities->instances[0].package==legacyRackPackageRef(sound.personalities->instances[0].design));
    juce::MemoryBlock project;CHECK(fixtures().getChildFile("project29.airackproject").loadFileAsData(project));CHECK(juce::SHA256(project).toHexString()=="31262efeb93c22751b8a396f282ccefaf80ccb0223db700c9e26737de354e149");ProjectDocument loaded;CHECK(decodeProjectState(project.getData(),project.getSize(),loaded));CHECK(loaded.sound==sound);
    auto state=std::make_shared<RackPersonalityState>(*sound.personalities);state->instances[0].package=exact;auto saved=sound;saved.personalities=state;juce::MemoryBlock bytes;CHECK(encodeSoundState(saved,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==saved);
    CHECK(std::memcmp(static_cast<const char*>(bytes.getData())+8,static_cast<const char*>(old.getData())+8,currentPersonalityLengthOffset-8)==0);
    CHECK(maximumSoundStateBytes-oldSound30MaximumBytes==12*173);CHECK(maximumProjectStateBytes-oldProject29MaximumBytes==12*173);
    const auto record=currentPersonalityLengthOffset+8;const auto designLength=record+18+2*state->instances[0].bindings.size();const auto ref=designLength+4+u32(bytes,designLength);
    const auto reject=[&](juce::MemoryBlock bad){sign(bad);auto out=saved;CHECK(!decodeSoundState(bad.getData(),bad.getSize(),out));CHECK(out==saved);};
    for(const auto length:{0u,65u,0xffffffffu}){auto bad=bytes;put(bad,ref+1,length);reject(bad);}
    {auto bad=bytes;static_cast<char*>(bad.getData())[ref]=2;reject(bad);}
    {auto bad=bytes;put(bad,4,30);reject(bad);}
    {auto bad=old;static_cast<char*>(bad.getData())[currentPersonalityLengthOffset+4]=2;reject(bad);}
    for(const auto& bad:std::vector<RackPackageRef>{{"../escape","1.0.0",exact.sha256},{"A","1.0.0",exact.sha256},{"ok","01.0.0",exact.sha256},{"ok","1.0.65536",exact.sha256},{"ok","1.0.0",exact.sha256.toUpperCase()},{"ok","1.0.0",""}}){
        CHECK(!validRackPackageRef(bad));state->instances[0].package=bad;juce::MemoryBlock output("keep",4),sentinel=output;CHECK(!encodeSoundState(saved,output)&&output==sentinel);
    }
}
struct Archive {
    RackPackageManifest manifest;
    RackDesign design;
    std::map<juce::String,juce::MemoryBlock> files;
    juce::File file;
    Archive(const juce::File& root,const juce::String& id,const juce::String& version,bool tile=false,bool changed=false,bool unknownEditor=false){
        manifest=test::packageFixture(id);manifest.ref.version=version;
        const auto source=juce::File(RACKSYNTH_WORKSPACE);juce::MemoryBlock recipe;CHECK(source.getChildFile("racks/"+id+"/rack.json").loadFileAsData(recipe));CHECK(decodeRackDesign(recipe.getData(),recipe.getSize(),design,RackValidationMode::instanceRecall));
        files["rack.json"]=recipe;files["screenshot.png"]=test::packageFixturePng();manifest.metadata.design=design.id;manifest.metadata.revision=design.revision;manifest.metadata.name=design.name;manifest.metadata.role=design.role;manifest.metadata.canonicalSha256=juce::SHA256(recipe).toHexString();manifest.skinPath="skin/elements.json";
        juce::Array<juce::var> modules;for(const auto& u:design.units){auto* o=new juce::DynamicObject;o->setProperty("module",int(u.module));o->setProperty("algorithmRevision",int(u.algorithmRevision));modules.add(juce::var(o));}manifest.capabilities.getDynamicObject()->setProperty("modules",modules);
        const auto skins=source.getChildFile("resources/maker-skins/"+id);
        const auto declarations=juce::JSON::parse(skins.getChildFile(id+"-elements.json").loadFileAsString());std::set<juce::String> paths{id+"-elements.json"};CHECK(declarations["assets"].isObject());
        if(const auto* assets=declarations["assets"].getDynamicObject())for(const auto& entry:assets->getProperties())if(entry.value["file"].isString())paths.insert(entry.value["file"].toString());
        for(const auto& listed:paths){CHECK(validRackPackagePath(listed));const auto input=skins.getChildFile(listed);juce::MemoryBlock data;CHECK(input.loadFileAsData(data));auto path=listed;if(path.endsWith("elements.json")){
                path="elements.json";auto doc=juce::JSON::parse(data.toString());if(changed)doc.getDynamicObject()->setProperty("model","EXACT VERSION TWO");
                if(tile){auto* records=doc["extension_records"].getArray();CHECK(records);if(records){(*records)[0]["geometry"].getDynamicObject()->setProperty("pivot",juce::Array<juce::var>{300.0,600.0});
                    for(const auto& record:*records)if(record["proposed_kind"]==juce::var("reveal-action")||record["proposed_kind"]==juce::var("reveal-close-action")){
                        auto* action=new juce::DynamicObject;for(const auto* key:{"id","view","label","geometry"})action->setProperty(key,record[key].clone());action->setProperty("kind","action");doc["elements"].getArray()->add(juce::var(action));
                    }
                }}
                if(unknownEditor)doc["reveal"].getDynamicObject()->setProperty("native_editor",juce::JSON::parse("{\"type\":\"future-editor\",\"theme\":\"amber_crt\",\"rack_unit\":0,\"rect\":[0,0,1,1]}"));
                const auto text=juce::JSON::toString(doc,true);data.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());
            }else if(changed&&path.endsWith(".png")){auto image=juce::ImageFileFormat::loadFrom(data.getData(),data.getSize());image.setPixelAt(0,0,juce::Colours::magenta);juce::MemoryOutputStream out;juce::PNGImageFormat png;CHECK(png.writeImageToStream(image,out));data=out.getMemoryBlock();}
            files["skin/"+path]=std::move(data);
        }
        manifest.files.clear();for(const auto& [path,data]:files)manifest.files.push_back({path,juce::SHA256(data).toHexString(),path=="rack.json"?"design":path.endsWith(".json")?"skin":"png",data.getSize()});
        const auto json=test::packageFixtureJson(manifest);juce::ZipFile::Builder zip;const juce::Time stamp(1980,0,1,0,0);zip.addEntry(new juce::MemoryInputStream(json.toRawUTF8(),json.getNumBytesAsUTF8(),true),0,"package.json",stamp);for(const auto& [path,data]:files)zip.addEntry(new juce::MemoryInputStream(data,true),0,path,stamp);
        file=root.getChildFile(id+"-"+version+".zip");auto stream=file.createOutputStream();CHECK(stream&&zip.writeToStream(*stream,nullptr));stream.reset();juce::MemoryBlock bytes;CHECK(file.loadFileAsData(bytes));manifest.ref.sha256=juce::SHA256(bytes).toHexString();std::cout<<"ARCHIVE_REF "<<id<<" "<<version<<" "<<manifest.ref.sha256<<std::endl;
    }
};
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(10);}
std::vector<float> render(const SoundDocument& sound){
    auto p=std::make_unique<InstrumentProcessor>();juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));CHECK(p->restoreSoundState(bytes.getData(),int(bytes.getSize())));p->prepareToPlay(48000,64);
    std::vector<float> result;for(int block=0;block<24;++block){juce::AudioBuffer<float> audio(2,64);audio.clear();juce::MidiBuffer midi;if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(block==16)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);p->processBlock(audio,midi);for(int ch=0;ch<2;++ch)result.insert(result.end(),audio.getReadPointer(ch),audio.getReadPointer(ch)+64);}p->releaseResources();return result;
}
int holdFace(const juce::File& root,const RackPackageRef& ref){
    RackPackageStore store({root.getChildFile("library")});RackInstance rack;rack.package=ref;RackInventoryPtr inventory;
    auto owner=std::async(std::launch::async,[&]{const auto lease=store.open(ref);if(!lease)return studio::MakerSkinPtr{};const auto recipe=lease->canonicalRecipe();if(!decodeRackDesign(recipe.data(),recipe.size(),rack.design,RackValidationMode::instanceRecall)||!captureRackInventory(store,std::span(&rack,1),inventory))return studio::MakerSkinPtr{};return studio::prepareMakerSkin(rack,inventory.get());}).get();
    if(!owner)return 2;auto p=std::make_unique<InstrumentProcessor>();RackOperation op;op.design=rack.design;op.package=ref;op.newInstance=juce::Uuid();SoundDocument placed;RackDifference difference;if(makeRackCandidate({},op,SoundEditOrigin::manual,placed,difference)!=RackCandidateStatus::ready)return 3;rack=placed.personalities->instances[0];juce::MemoryBlock bytes;if(!encodeSoundState(placed,bytes)||!p->restoreSoundState(bytes.getData(),int(bytes.getSize())))return 4;
    juce::Component holder;auto face=studio::MakerFace::forRack(*p,owner,rack,{});if(!face)return 5;holder.addAndMakeVisible(*face);holder.setBounds(0,0,1600,900);holder.addToDesktop(juce::ComponentPeer::windowIsTemporary);holder.setVisible(true);face->setOpen(true,false);root.getChildFile("child-ready").replaceWithText("ready");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);while(std::chrono::steady_clock::now()<deadline&&!root.getChildFile("child-release").existsAsFile()){pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));}face->retire();p.reset();face.reset();return 0;
}
void ownedDrumRecall(const Archive& tile,const juce::File& root){
    auto p=std::make_unique<InstrumentProcessor>();SoundDocument initial;std::uint64_t revision;CHECK(p->trySoundSnapshot(initial,revision));RackOperation op;op.design=tile.design;op.package=tile.manifest.ref;op.newInstance=juce::Uuid();CHECK(p->applyRackOperation(op,revision)==RackCandidateStatus::ready);p->prepareToPlay(48000,64);
    std::vector<float> pcm(480,.002f);drums::AssetPtr asset;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,asset,assetError));std::uint64_t ticket;DrumCommitError error;CHECK(p->prepareDrumAsset(pcm,1,ticket,error));DrumPadTarget target;CHECK(p->captureDrumPadTarget(op.newInstance,0,0,target,error));drums::DrumPad metadata;metadata.asset=asset->id();metadata.name="Owned independently of artwork";CHECK(p->assignPreparedDrumPad(target,ticket,metadata,{},error));
    juce::MemoryBlock host;p->getStateInformation(host);drums::DrumBundlePtr bundle;drums::DrumError bundleError;CHECK(drums::decodeDrumBundle({static_cast<const std::uint8_t*>(host.getData()),host.getSize()},bundle,bundleError));if(bundle){CHECK(bundle->assets().size()==1&&bundle->assets()[0]->id()==asset->id());ProjectDocument nested;const auto compact=bundle->compactDocument();CHECK(decodeProjectState(compact.data(),compact.size(),nested));CHECK(nested.sound.personalities->instances[0].package==tile.manifest.ref);}
    auto restored=std::make_unique<InstrumentProcessor>();CHECK(restored->restoreProjectState(host.getData(),int(host.getSize())));SoundDocument sound;CHECK(restored->trySoundSnapshot(sound,revision));CHECK(sound.personalities->instances[0].package==tile.manifest.ref);CHECK(restored->captureDrumPerformanceTarget(op.newInstance,0,0,target,error));DrumPadReadback readback;CHECK(restored->readDrumPad(target,readback,error)&&readback.metadata==metadata&&readback.frames==480);
    const auto preset=root.getChildFile("owned.soundstate");CHECK(std::async(std::launch::async,[&]{return p->saveSoundPreset(preset);}).get());auto loaded=std::make_unique<InstrumentProcessor>();CHECK(std::async(std::launch::async,[&]{return loaded->loadSoundPreset(preset);}).get());CHECK(loaded->trySoundSnapshot(sound,revision));CHECK(sound.personalities->instances[0].package==tile.manifest.ref);CHECK(loaded->captureDrumPerformanceTarget(op.newInstance,0,0,target,error));CHECK(loaded->readDrumPad(target,readback,error)&&readback.metadata==metadata&&readback.frames==480);p->releaseResources();
}
std::unique_ptr<Archive> tilePackage(RackPackageStore& store,const juce::File& root){
    auto tile=std::async(std::launch::async,[&]{return std::make_unique<Archive>(root,"tile-array","1.0.0",true);}).get();const auto status=std::async(std::launch::async,[&]{return store.install(tile->file,tile->manifest.ref,RackPackageActivation::makeActive,{});}).get();std::cout<<"TILE_INSTALL_STATUS "<<int(status)<<std::endl;CHECK(status==RackLibraryStatus::ready);
    RackInstance instance;instance.design=tile->design;instance.package=tile->manifest.ref;RackInventoryPtr inventory;CHECK(std::async(std::launch::async,[&]{return captureRackInventory(store,std::span(&instance,1),inventory);}).get());auto owner=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(instance,inventory.get());}).get();CHECK(owner);if(owner){CHECK(double((*owner->skin.nativeExtensionRecords.getArray())[0]["geometry"]["pivot"][0])==300.0);
        auto conflicting=owner->skin;const auto action=std::find_if(conflicting.elements.begin(),conflicting.elements.end(),[](const auto& e){return e.id=="face.programmer";});CHECK(action!=conflicting.elements.end());if(action!=conflicting.elements.end())action->rect.translate(1,0);
        bool rejected=false;try{studio::TileArrayFace::adaptModel(conflicting,juce::JSON::parse(tile->files.at("skin/elements.json").toString()));}catch(const std::exception&){rejected=true;}CHECK(rejected);
        for(int malformed=0;malformed<4;++malformed){auto doc=juce::JSON::parse(tile->files.at("skin/elements.json").toString());
            if(malformed==0){doc["reveal"].getDynamicObject()->setProperty("units_added",2147483647);CHECK(int(doc["reveal"]["units_added"])>4);}
            if(malformed==1){(*doc["extension_records"].getArray())[16].getDynamicObject()->setProperty("id","face.unknown");CHECK(doc["extension_records"][16]["id"].toString()=="face.unknown");}
            if(malformed==2){(*doc["extension_records"].getArray())[16]["geometry"].getDynamicObject()->removeProperty("pivot");CHECK(!doc["extension_records"][16]["geometry"].hasProperty("pivot"));}
            if(malformed==3){(*doc["extension_records"].getArray())[0]["geometry"].getDynamicObject()->setProperty("rect",juce::Array<juce::var>{144.0,532.0,-1.0,135.0});CHECK(double(doc["extension_records"][0]["geometry"]["rect"][2])<0);}
            auto model=owner->skin;rejected=false;try{studio::TileArrayFace::adaptModel(model,doc);}catch(const std::exception&){rejected=true;}if(!rejected)std::cout<<"TILE_REJECT_CASE "<<malformed<<std::endl;CHECK(rejected);
        }
    }return tile;
}
void saturationArtwork(const juce::File& root){
    auto archives=std::async(std::launch::async,[&]{return std::make_pair(std::make_unique<Archive>(root,"character-drive","1.0.0"),std::make_unique<Archive>(root,"character-drive","2.0.0",false,true));}).get();
    RackPackageStore store({root.getChildFile("library")});
    for(const auto* archive:{archives.first.get(),archives.second.get()})CHECK(std::async(std::launch::async,[&]{return store.install(archive->file,archive->manifest.ref,RackPackageActivation::makeActive,{});}).get()==RackLibraryStatus::ready);
    RackOperation op;op.destination.role=RackRole::fx;op.design=archives.first->design;op.package=archives.first->manifest.ref;op.newInstance=juce::Uuid();SoundDocument sound;RackDifference difference;CHECK(makeRackCandidate({},op,SoundEditOrigin::manual,sound,difference)==RackCandidateStatus::ready);
    if(!sound.personalities||sound.personalities->instances.empty())return;
    auto first=sound.personalities->instances[0],second=first;second.package=archives.second->manifest.ref;RackInventoryPtr inventory;
    CHECK(std::async(std::launch::async,[&]{return captureRackInventory(store,std::span(&first,1),inventory);}).get());
    auto older=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(first,inventory.get());}).get();auto newer=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(second,inventory.get());}).get();CHECK(older&&newer);if(!older||!newer)return;
    CHECK(older->skin.asset("patch-bay:cover").image.getPixelAt(0,0)!=newer->skin.asset("patch-bay:cover").image.getPixelAt(0,0));
    auto p=std::make_unique<InstrumentProcessor>();juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));CHECK(p->restoreSoundState(bytes.getData(),int(bytes.getSize())));p->prepareRackSkins(inventory,std::span(&first,1));for(int n=0;n<200&&!p->preparedMakerSkin(first);++n)pump();CHECK(p->preparedMakerSkin(first)==older);
    {studio::AllControlsPanel panel(*p,*studio::studioSkin(),{});const auto units=studio::panelUnits(first);panel.show(units);const auto knobs=panel.shownKnobs();const auto parameters=panel.parameters();
        auto next=sound;auto state=std::make_shared<RackPersonalityState>(*sound.personalities);state->instances[0]=second;next.personalities=state;SoundDocument current;std::uint64_t revision;CHECK(p->trySoundSnapshot(current,revision));CHECK(encodeSoundState(next,bytes));CHECK(p->restoreSoundState(bytes.getData(),int(bytes.getSize())));
        p->prepareRackSkins(inventory,std::span(&second,1));for(int n=0;n<200&&p->preparedMakerSkin(second)!=newer;++n)pump();CHECK(p->preparedMakerSkin(second)==newer);const auto history=InstrumentDrumTestAccess::history(*p);
        panel.show(units);CHECK(panel.shownKnobs()==knobs&&panel.parameters()==parameters);CHECK(p->trySoundSnapshot(current,revision)&&current==next);CHECK(InstrumentDrumTestAccess::history(*p)==history);
        older.reset();auto removal=RackLibraryStatus::busy;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(removal==RackLibraryStatus::busy&&std::chrono::steady_clock::now()<deadline){removal=store.remove(first.package.value(),{});if(removal==RackLibraryStatus::busy)pump();}CHECK(removal==RackLibraryStatus::ready);
    }
}
void packageSkins(const juce::File& root){
    CHECK(root.createDirectory().wasOk());
    auto setup=std::async(std::launch::async,[&]{return std::make_pair(std::make_unique<Archive>(root,"ember-reed","1.0.0"),std::make_unique<Archive>(root,"ember-reed","2.0.0",false,true));}).get();
    RackPackageStore store({root.getChildFile("library")});const auto installed=std::async(std::launch::async,[&]{return store.install(setup.first->file,setup.first->manifest.ref,RackPackageActivation::makeActive,{});}).get();std::cout<<"INSTALL_STATUS "<<int(installed)<<std::endl;CHECK(installed==RackLibraryStatus::ready);
    auto sound=placed();auto state=std::make_shared<RackPersonalityState>(*sound.personalities);state->instances[0].design=setup.first->design;state->instances[0].package=setup.first->manifest.ref;sound.personalities=state;
    RackInventoryPtr inventory;CHECK(std::async(std::launch::async,[&]{return captureRackInventory(store,state->instances,inventory);}).get());
    auto owner=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(state->instances[0],inventory.get());}).get();CHECK(owner&&owner->owner&&owner->lease);if(!owner)return;
    CHECK(studio::makerFaceFor(state->instances[0],*inventory)==owner);CHECK(&owner->skin==owner->owner.get());
    auto p=std::make_unique<InstrumentProcessor>();juce::MemoryBlock original;CHECK(encodeSoundState(sound,original));CHECK(p->restoreSoundState(original.getData(),int(original.getSize())));
    const auto pcm=render(sound);auto plain=sound;auto noRef=std::make_shared<RackPersonalityState>(*state);noRef->instances[0].package.reset();plain.personalities=noRef;CHECK(pcm==render(plain));CHECK(std::any_of(pcm.begin(),pcm.end(),[](float sample){return std::abs(sample)>1e-8f;}));std::cout<<"AUDIO_EXACT "<<juce::SHA256(pcm.data(),pcm.size()*sizeof(float)).toHexString()<<std::endl;
    std::uint64_t edit;SoundDocument current;CHECK(p->trySoundSnapshot(current,edit));RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=state->instances[0].id;duplicate.newInstance=juce::Uuid();CHECK(p->applyRackOperation(duplicate,edit)==RackCandidateStatus::ready);CHECK(p->trySoundSnapshot(current,edit));CHECK(current.personalities->instances.back().package==state->instances[0].package);CHECK(p->undoSoundEdit());CHECK(p->trySoundSnapshot(current,edit)&&current==sound);CHECK(p->redoSoundEdit());CHECK(p->trySoundSnapshot(current,edit)&&current.personalities->instances.back().package==state->instances[0].package);CHECK(p->undoSoundEdit());
    juce::ChildProcess child;const auto& held=*state->instances[0].package;CHECK(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--hold-face",root.getFullPathName(),held.id,held.version,held.sha256}));const auto childDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);while(std::chrono::steady_clock::now()<childDeadline&&child.isRunning()&&!root.getChildFile("child-ready").existsAsFile()){pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));}CHECK(root.getChildFile("child-ready").existsAsFile()&&child.isRunning());
    juce::Component holder;holder.setBounds(0,0,1600,900);holder.addToDesktop(juce::ComponentPeer::windowIsTemporary);holder.setVisible(true);
    auto face=studio::MakerFace::forRack(*p,owner,state->instances[0],{});CHECK(face);if(face){holder.addAndMakeVisible(*face);face->setOpen(true,false);pump();}
    const auto history=InstrumentDrumTestAccess::history(*p);
    p->prepareRackSkins(inventory,state->instances);for(int n=0;n<100&&!p->preparedMakerSkin(state->instances[0]);++n){std::this_thread::sleep_for(std::chrono::milliseconds(10));pump();}CHECK(p->preparedMakerSkin(state->instances[0])==owner);CHECK(InstrumentDrumTestAccess::history(*p)==history);
    CHECK(std::async(std::launch::async,[&]{return store.install(setup.second->file,setup.second->manifest.ref,RackPackageActivation::makeActive,{});}).get()==RackLibraryStatus::ready);
    RackInventoryPtr updated;CHECK(std::async(std::launch::async,[&]{return captureRackInventory(store,state->instances,updated);}).get());
    CHECK(InstrumentDrumTestAccess::history(*p)==history);
    RackDesign active;std::optional<RackPackageRef> ref;CHECK(resolveInventoryDesign(*updated,setup.second->design.id,setup.second->design.revision,setup.second->manifest.metadata.canonicalSha256,active,ref));CHECK(ref==setup.second->manifest.ref);
    RackReviewRequest request;request.localDesign=active;request.package=ref;auto review=reviewRack(*p,request);CHECK(review.state=="Ready"&&review.draft.size()==1&&review.draft[0].package==ref);CHECK(applyRackReview(*p,review).applied);CHECK(p->trySoundSnapshot(current,edit));CHECK(current.personalities->instances.front().package==state->instances[0].package&&current.personalities->instances.back().package==ref);CHECK(p->undoSoundEdit());CHECK(p->trySoundSnapshot(current,edit));auto edited=current;edited.values[756]=.4f;CHECK(p->applySoundEdit(edited,SoundEditOrigin::manual,edit));CHECK(p->trySoundSnapshot(current,edit)&&current.personalities->instances[0].package==state->instances[0].package);CHECK(p->undoSoundEdit());
    auto second=state->instances[0];second.package=ref;auto newer=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(second,updated.get());}).get();CHECK(newer&&newer!=owner&&newer->model=="EXACT VERSION TWO");if(newer)CHECK(newer->skin.asset("face:plate").image.getPixelAt(0,0)!=owner->skin.asset("face:plate").image.getPixelAt(0,0));
    CHECK(store.remove(*state->instances[0].package,{})==RackLibraryStatus::busy);
    root.getChildFile("child-release").replaceWithText("release");CHECK(child.waitForProcessToFinish(30000));CHECK(child.getExitCode()==0);std::cout<<"CHILD "<<child.readAllProcessOutput()<<std::endl;
    SoundDocument before;std::uint64_t revision;CHECK(p->trySoundSnapshot(before,revision));CHECK(before==sound);juce::MemoryBlock unchanged;CHECK(encodeSoundState(before,unchanged)&&unchanged==original);
    // Retirement leaves an inert queued face owning all images and its exact lease.
    face->retire();p.reset();owner.reset();inventory.reset();updated.reset();CHECK(store.remove(*state->instances[0].package,{})==RackLibraryStatus::busy);face.reset();pump();
    CHECK(store.remove(*state->instances[0].package,{})==RackLibraryStatus::ready);
    RackInventoryPtr absent;CHECK(std::async(std::launch::async,[&]{return captureRackInventory(store,state->instances,absent);}).get());CHECK(!std::async(std::launch::async,[&]{return studio::prepareMakerSkin(state->instances[0],absent.get());}).get());
    {auto fallback=std::make_unique<InstrumentProcessor>();CHECK(fallback->restoreSoundState(original.getData(),int(original.getSize())));fallback->prepareRackSkins(absent,state->instances);const auto* house=studio::studioSkin();CHECK(house);if(house){RackStackUnit rack(*fallback,*house,state->instances[0].id);rack.refresh(sound);CHECK(!rack.face());studio::AllControlsPanel controls(*fallback,*house,{});controls.show(studio::panelUnits(state->instances[0]));const auto reached=controls.parameters();for(const auto& unit:studio::panelUnits(state->instances[0]))for(const auto index:unitReach(unit.module,unit.bank,unit.revision))CHECK(std::binary_search(reached.begin(),reached.end(),index));}}
    CHECK(store.install(setup.first->file,*state->instances[0].package,RackPackageActivation::preserveActive,{})==RackLibraryStatus::ready);
    RackInventoryPtr restored;CHECK(std::async(std::launch::async,[&]{return captureRackInventory(store,state->instances,restored);}).get());auto restoredOwner=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(state->instances[0],restored.get());}).get();CHECK(restoredOwner&&restoredOwner->lease->manifest().ref==state->instances[0].package);CHECK(encodeSoundState(sound,unchanged)&&unchanged==original);
    auto background=std::make_unique<InstrumentProcessor>();background->prepareRackSkins(restored,state->instances);std::weak_ptr<const studio::MakerSkin> used=restoredOwner;for(int n=0;n<100&&used.use_count()<2;++n)pump();CHECK(used.use_count()>=2);restoredOwner.reset();background->prepareRackSkins(restored,{});RackLibraryStatus removal=RackLibraryStatus::busy;for(int n=0;n<100&&removal==RackLibraryStatus::busy;++n){pump();removal=store.remove(*state->instances[0].package,{});}CHECK(removal==RackLibraryStatus::ready);CHECK(used.expired());background.reset();
    auto unsupported=std::async(std::launch::async,[&]{return std::make_unique<Archive>(root,"ember-reed","3.0.0",false,false,true);}).get();
    CHECK(std::async(std::launch::async,[&]{return store.install(unsupported->file,unsupported->manifest.ref,RackPackageActivation::makeActive,{});}).get()==RackLibraryStatus::incompatible);
    auto unavailable=state->instances[0];unavailable.package=unsupported->manifest.ref;CHECK(!std::async(std::launch::async,[&]{return studio::prepareMakerSkin(unavailable,restored.get());}).get());
    auto tile=tilePackage(store,root);
    ownedDrumRecall(*tile,root);
    saturationArtwork(root);
}
}
void runtimeInjection(const juce::File& root){
    std::atomic<int> requests{0};auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[&](const auto&,const auto&,const auto&,auto&){++requests;return RackLibraryStatus::networkFailure;});
    std::weak_ptr<RackLibraryRuntime> weak=runtime;auto first=std::make_unique<InstrumentProcessor>(runtime),second=std::make_unique<InstrumentProcessor>(runtime);CHECK(first->rackLibraryRuntime()==runtime&&second->rackLibraryRuntime()==runtime);CHECK(first->getParameters().size()==parameterCount);auto editor=std::unique_ptr<juce::AudioProcessorEditor>(first->createEditor());CHECK(editor!=nullptr);editor.reset();first.reset();CHECK(requests==0);runtime.reset();CHECK(!weak.expired());CHECK(second->rackLibraryRuntime()->client->view()->status==RackLibraryStatus::unavailable);second.reset();CHECK(weak.expired());CHECK(requests==0);CHECK(!root.exists());
}
int main(int argc,char** argv){if(argc==2&&juce::String(argv[1])=="--freeze")return 2;juce::ScopedJuceInitialiser_GUI gui;
    if(argc==6&&juce::String(argv[1])=="--hold-face"){const juce::Image nativeDevice(juce::Image::ARGB,1,1,true);if(!nativeDevice.isValid())return 2;return holdFace(juce::File(argv[2]),{argv[3],argv[4],argv[5]});}
    const auto preparedSkins=prepareTestMakerSkins();
    if(argc==2&&juce::String(argv[1])=="--tile-fixture"){const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task4/trials").getNonexistentChildFile("tile-",{},false);root.createDirectory();RackPackageStore store({root.getChildFile("library")});tilePackage(store,root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--saturation-artwork"){const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task4/trials").getNonexistentChildFile("saturation-",{},false);root.createDirectory();saturationArtwork(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    references();frozenAndInvalid();
    const auto rack=placed().personalities->instances[0];
    auto skin=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(rack,nullptr);}).get();
    CHECK(skin&&skin->owner);if(skin)CHECK(&skin->skin==skin->owner.get());
    const auto base=argc==2?juce::File(argv[1]):juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task4/trials");CHECK(base.createDirectory().wasOk());runtimeInjection(base.getChildFile("runtime-"+juce::Uuid().toString()));const auto root=base.getNonexistentChildFile("recall-",{},false);std::cout<<"FIXTURE_ROOT "<<root.getFullPathName()<<std::endl;packageSkins(root);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
