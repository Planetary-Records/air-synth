#include "RackExamples.h"
#include "RackLibrary.h"
#include "InstrumentProcessor.h"
#include "RackStack.h"
#include "StudioSkin.h"
#include "HouseUnit.h"
#include "ProjectDocument.h"
#include <future>
#include <cmath>
#include <thread>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <iostream>
#include <set>

using namespace airack;
namespace {
juce::var object(std::initializer_list<std::pair<juce::Identifier, juce::var>> fields) {
    auto* value = new juce::DynamicObject;
    for (const auto& [key, item] : fields) value->setProperty(key, item);
    return value;
}
struct Detached { const char* id; const char* uuid; };
// Package identities are explicit, independent of display names and skin heuristics.
constexpr Detached detached[] {
    {"ember-reed", "c83dab6a-1c4e-48c1-83c4-80d9212ca601"},
    {"glass-current", "c83dab6a-1c4e-48c1-83c4-80d9212ca602"},
    {"thread-press", "c83dab6a-1c4e-48c1-83c4-80d9212ca603"},
    {"oxide-return", "330cb53b-cd9b-443c-94d8-2700bc8b88f7"},
    {"small-hours", "22d80511-2e4f-4a41-b1dd-203eabbcf32e"},
    {"night-dial", "cdeafe3f-9c29-4c7c-9013-8284b505af10"},
    {"tile-array", "f97641c2-cd64-46a9-a3ab-0cc027a5f8d4"},
    {"contour-dynamics", "72b0ea6d-4c2d-4d93-a3d8-7a904b050001"},
    {"lumen-field", "cc73c36a-696e-4b09-9cf3-dcd4dfac16f0"},
    {"character-drive", "91387715-13d3-4c57-a479-12251842bf01"},
    {"band-color", "91387715-13d3-4c57-a479-12251842bf02"}
};
struct SourceIdentity { const char* path; const char* sha256; };
constexpr SourceIdentity baselineSources[] {
    {"src/racks/RackExamples.cpp","d704b4edaa4e3d826fdecf047c9663ea60761acb09598536ba4d50db5a20fc84"},
    {"src/racks/RackDesign.cpp","6196855ecbd4b35a03d739904ef41ffb642557f4effc78230b359552b2e412c0"},
    {"src/racks/RackLibrary.cpp","c37cfa0e93ff081a08a03997cb9c4de31f2aaab72c5ac62ff5963ccd6c0eb80f"},
    {"src/state/ParameterRegistry.cpp","49364f80083b272daba92901377bfc0d420c7b38d0dab05af1485a2718dbcdae"},
    {"src/dsp/DspCompatibility.h","2dd04f95786ac3aaf1759ae669d531846e105dbe1af94c73aaeda9980c3d98bf"}
};
bool write(const juce::File& file, const juce::MemoryBlock& bytes) {
    return file.getParentDirectory().createDirectory().wasOk()
        && file.replaceWithData(bytes.getData(), bytes.getSize());
}
bool writeJson(const juce::File& file, const juce::var& value) {
    const auto text = juce::JSON::toString(value, false);
    return write(file, juce::MemoryBlock(text.toRawUTF8(), text.getNumBytesAsUTF8()));
}
int releaseInputs(const juce::File& root,const juce::File& output) {
    if(output.exists()&&(!output.isDirectory()||output.getNumberOfChildFiles(juce::File::findFilesAndDirectories)!=0))return 2;
    const auto frozen=root.getChildFile("tests/fixtures/rack-library/v1");
    const auto manifest=juce::JSON::parse(frozen.getChildFile("manifest.json"));
    const auto* current=manifest["current"].getArray();if(!current||current->size()!=28)return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto* model=studio::studioSkin();if(!model)return 2;
    juce::Array<juce::var> captures;
    for(const auto& identity:detached){
        auto entry=std::find_if(current->begin(),current->end(),[&](const auto& row){return row["design"].toString()==identity.uuid;});
        if(entry==current->end())return 2;
        juce::MemoryBlock recipe;RackDesign design;
        if(!frozen.getChildFile((*entry)["path"].toString()).loadFileAsData(recipe)
           ||juce::SHA256(recipe).toHexString()!=(*entry)["sha256"].toString()
           ||!decodeRackDesign(recipe.getData(),recipe.getSize(),design,RackValidationMode::instanceRecall))return 2;
        companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString();
        auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{output.getChildFile("injected-unused-store")});
        runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[](const auto&,const auto&,const auto&,auto&){return RackLibraryStatus::networkFailure;});
        InstrumentProcessor processor(offline,runtime);
        RackOperation op;op.design=design;op.newInstance=juce::Uuid(identity.uuid);op.destination={design.role,masterChain,0};
        RackInventoryPtr inventory;
        if(const auto ref=legacyRackPackageRef(design)){
            const auto archive=root.getChildFile("build/rack-library-native/releases/"+ref->id+"-"+ref->version+".airrack");
            const auto status=std::async(std::launch::async,[&]{return runtime->store->install(archive,*ref,RackPackageActivation::makeActive,{});}).get();
            if(status!=RackLibraryStatus::ready){std::cerr<<"INSTALL "<<identity.id<<" status="<<int(status)<<std::endl;return 2;}
            if(!std::async(std::launch::async,[&]{return captureRackInventory(*runtime->store,{},inventory);}).get())return 2;
            op.package=ref;
        }
        SoundDocument sound;RackDifference difference;
        if(makeRackCandidate(design.role==RackRole::fx?copperArcSound():newInstanceSound(),op,SoundEditOrigin::manual,sound,difference)!=RackCandidateStatus::ready)return 2;
        juce::MemoryBlock state;if(!encodeSoundState(sound,state)||!processor.restoreSoundState(state.getData(),int(state.getSize())))return 2;
        const auto& rack=sound.personalities->instances.back();
        auto owner=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(rack,inventory.get());}).get();
        if(juce::String(identity.id)!="contour-dynamics"&&!owner)return 2;
        processor.prepareRackSkins(inventory,sound.personalities->instances);
        if(owner){const auto end=juce::Time::getMillisecondCounter()+60000;while(!processor.preparedMakerSkin(rack)&&juce::Time::getMillisecondCounter()<end){juce::MessageManager::getInstance()->runDispatchLoopUntil(5);std::this_thread::sleep_for(std::chrono::milliseconds(2));}if(!processor.preparedMakerSkin(rack))return 2;}
        RackStackUnit unit(processor,*model,rack.id);unit.refresh(sound);
        const int width=int(model->layout["stage"][0]);
        const auto scale=float(width)/(2.0f*studio::HouseUnit::railW+model->house.panelWidth);
        unit.setSize(width,juce::roundToInt(unit.units()*model->house.unitHeight*scale));
        unit.addToDesktop(juce::ComponentPeer::windowIsTemporary);unit.setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        const auto image=unit.createComponentSnapshot(unit.getLocalBounds(),true,1.0f);
        juce::MemoryOutputStream png;if(!juce::PNGImageFormat().writeImageToStream(image,png))return 2;
        if(!write(output.getChildFile(juce::String(identity.id)+"/screenshot.png"),png.getMemoryBlock())
           ||!write(output.getChildFile(juce::String(identity.id)+"/rack.json"),recipe))return 2;
        if(juce::String(identity.id)=="tile-array"){
            std::vector<float> pcm(480);for(std::size_t n=0;n<pcm.size();++n)pcm[n]=.1f*std::sin(float(n)*.1f)*float(pcm.size()-n)/float(pcm.size());
            std::uint64_t ticket=0;DrumCommitError error;DrumPadTarget target;
            if(!processor.prepareDrumAsset(pcm,1,ticket,error)||!processor.captureDrumPadTarget(rack.id,0,0,target,error))return 2;
            drums::AssetPtr asset;drums::AssetError assetError;if(!drums::makeSampleAsset(pcm,1,asset,assetError))return 2;
            drums::DrumPad metadata;metadata.name="Frozen owned sample";metadata.asset=asset->id();
            if(!processor.assignPreparedDrumPad(target,ticket,metadata,{},error))return 2;
        }
        processor.getStateInformation(state);if(!write(output.getChildFile(juce::String(identity.id)+"/project.state"),state))return 2;
        processor.setNonRealtime(true);processor.prepareToPlay(48000,128);
        juce::MemoryOutputStream pcm;
        for(int block=0;block<375;++block){
            juce::AudioBuffer<float> audio(2,128);audio.clear();juce::MidiBuffer midi;
            if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,juce::String(identity.id)=="tile-array"?36:48,juce::uint8(100)),0);
            if(block==187)midi.addEvent(juce::MidiMessage::noteOff(1,48),0);
            processor.processBlock(audio,midi);
            for(int channel=0;channel<2;++channel)pcm.write(audio.getReadPointer(channel),128*sizeof(float));
        }
        processor.releaseResources();if(!write(output.getChildFile(juce::String(identity.id)+"/baseline.f32"),pcm.getMemoryBlock()))return 2;
        captures.add(object({{"id",identity.id},{"canonical_sha256",juce::SHA256(recipe).toHexString()},
            {"screenshot_sha256",juce::SHA256(png.getMemoryBlock()).toHexString()},{"width",image.getWidth()},{"height",image.getHeight()},
            {"state_sha256",juce::SHA256(state).toHexString()},{"pcm_sha256",juce::SHA256(pcm.getMemoryBlock()).toHexString()}}));
        std::cout<<"CAPTURE "<<identity.id<<" "<<image.getWidth()<<"x"<<image.getHeight()<<std::endl;
    }
    if(!writeJson(output.getChildFile("capture-identities.json"),object({{"schema",1},{"captures",captures}})))return 2;
    std::cout<<"11 frozen definitions/native captures/owned project states/audio baselines\n";return 0;
}
}
int main(int argc, char** argv) {
    if(argc==4&&juce::String(argv[1])=="--release")return releaseInputs(juce::File(argv[2]),juce::File(argv[3]));
    if (argc != 3) { std::cerr << "usage: RackSynthRackPackageExport <source-root> <empty-output>\n"; return 2; }
    const juce::File root(juce::String::fromUTF8(argv[1])), output(juce::String::fromUTF8(argv[2]));
    if (!root.getChildFile("src/racks/RackExamples.cpp").existsAsFile()
        || (output.exists() && (!output.isDirectory() || output.getNumberOfChildFiles(juce::File::findFilesAndDirectories) != 0))) {
        std::cerr << "invalid source or nonempty export directory\n"; return 2;
    }
    // One-shot baseline export: refuse relabelling a different producer as the accepted source.
    for (const auto& source : baselineSources) {
        juce::MemoryBlock bytes;
        if (!root.getChildFile(source.path).loadFileAsData(bytes) || juce::SHA256(bytes).toHexString() != source.sha256) {
            std::cerr << "source does not match the frozen producer baseline\n"; return 2;
        }
    }
    RackCatalogSnapshot catalog;
    if (!captureRackCatalog(libraryRackDesigns(), {}, {}, catalog) || catalog.entries.size() != 28) return 1;
    juce::Array<juce::var> current, packages, historical, assets, sources;
    for (const auto& entry : catalog.entries) {
        juce::MemoryBlock bytes;
        if (!encodeRackDesign(entry.definition, bytes, RackValidationMode::instanceRecall)
            || juce::SHA256(bytes).toHexString() != entry.contentSha256) return 1;
        const auto file = "current/" + entry.definition.id.toString() + "-r" + juce::String(std::to_string(entry.definition.revision)) + ".json";
        if (!write(output.getChildFile(file), bytes)) return 1;
        current.add(object({{"design",entry.definition.id.toDashedString()}, {"revision",juce::String(std::to_string(entry.definition.revision))},
            {"name",entry.definition.name}, {"path",file}, {"bytes",juce::int64(bytes.getSize())}, {"sha256",entry.contentSha256}}));
    }
    for (const auto& identity : detached) {
        const auto found = std::find_if(catalog.entries.begin(), catalog.entries.end(), [&](const auto& entry) { return entry.definition.id == juce::Uuid(identity.uuid); });
        if (found == catalog.entries.end()) return 1;
        packages.add(object({{"id",identity.id}, {"version","1.0.0"}, {"design",identity.uuid},
            {"revision",juce::String(std::to_string(found->definition.revision))}, {"canonical_sha256",found->contentSha256}}));
    }
    auto files = root.getChildFile("tests/fixtures").findChildFiles(juce::File::findFiles, true);
    files.sort(); std::set<juce::String> hashes;
    for (const auto& file : files) {
        if (!file.hasFileExtension("rackdesign;airack;airackrack") || file.getSize() > 65536) continue;
        juce::MemoryBlock raw; RackDesign design;
        if (!file.loadFileAsData(raw) || !decodeRackDesign(raw.getData(),raw.getSize(),design,RackValidationMode::instanceRecall)) continue;
        const auto hash = juce::SHA256(raw).toHexString();
        const auto path = "historical/" + hash + ".json";
        if (hashes.insert(hash).second && !write(output.getChildFile(path),raw)) return 1;
        historical.add(object({{"source",file.getRelativePathFrom(root).replaceCharacter('\\','/')}, {"path",path},
            {"design",design.id.toDashedString()}, {"revision",juce::String(std::to_string(design.revision))},
            {"sha256",hash}, {"bytes",juce::int64(raw.getSize())}}));
    }
    auto resourceFiles = root.getChildFile("resources/maker-skins").findChildFiles(juce::File::findFiles,true); resourceFiles.sort();
    for (const auto& file : resourceFiles) {
        juce::MemoryBlock bytes; if (!file.loadFileAsData(bytes)) return 1;
        assets.add(object({{"path",file.getRelativePathFrom(root).replaceCharacter('\\','/')},
            {"bytes",juce::int64(bytes.getSize())}, {"sha256",juce::SHA256(bytes).toHexString()}}));
    }
    for (const auto& source : baselineSources) {
        sources.add(object({{"path",source.path},{"sha256",source.sha256}}));
    }
    if (!writeJson(output.getChildFile("manifest.json"),object({{"schema",1},
        {"source_baseline","9cbc6a89fea377b657d267a3c954afa5005c5fc2"},
        {"planning_source","6a285309931cc91f8f6e61586dea0e5cc2ccc5eb"}, {"sources",sources},
        {"current",current}, {"packages",packages}, {"historical",historical}, {"assets",assets}}))) return 1;
    std::cout << "exported " << current.size() << " current designs, " << packages.size()
        << " explicit packages, " << historical.size() << " historical inputs, " << assets.size() << " asset hashes\n";
    return 0;
}
