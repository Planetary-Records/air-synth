#pragma once
#include "IsolatedRackLibrary.h"
#include "MakerFace.h"
#include "RackExamples.h"
#include <algorithm>
#include <future>
#include <iostream>
#include <stdexcept>

namespace airack::testing {
// Each GUI test owns a real Tile package and its prepared artwork for the editor lifetime.
struct PreparedTileSkin {
    juce::Image device{juce::Image::ARGB,1,1,true};
    studio::MakerSkinPtr owner;
};
inline std::unique_ptr<PreparedTileSkin> prepareTileSkin(const std::shared_ptr<IsolatedRackRuntime>& runtime,
                                                          InstrumentProcessor& processor,juce::Uuid rackId) try {
    ProjectDocument document;std::uint64_t edit=0;
    if(!processor.tryProjectSnapshot(document,edit)||!document.sound.personalities)throw std::runtime_error("Tile fixture snapshot");
    const auto& racks=document.sound.personalities->instances;
    const auto found=std::find_if(racks.begin(),racks.end(),[&](const auto& rack){return rack.id==rackId;});
    if(found==racks.end()||found->package)throw std::runtime_error("Tile fixture instance");
    const auto reference=legacyRackPackageRef(found->design);
    if(!reference||reference->id!="tile-array")throw std::runtime_error("Tile fixture identity");
    const auto rack=*found;
    return std::async(std::launch::async,[runtime,rack,reference]{
        auto result=std::make_unique<PreparedTileSkin>();
        if(!result->device.isValid())throw std::runtime_error("Tile fixture native device");
        const auto workspace=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory();
        const auto archive=workspace.getChildFile("build/rack-library-native/releases/"+reference->id+"-"+reference->version+".airrack");
        const auto installed=runtime->store->install(archive,*reference,RackPackageActivation::makeActive,{});
        if(installed!=RackLibraryStatus::ready)throw std::runtime_error("Tile package installation status "+std::to_string(int(installed)));
        RackInventoryPtr inventory;RackLibraryStatus status{};
        if(!captureRackInventory(*runtime->store,std::span(&rack,1),inventory,&status)||!inventory||!inventory->openPackage(*reference,&status))
            throw std::runtime_error("Tile package inventory/lease status "+std::to_string(int(status)));
        result->owner=studio::prepareMakerSkin(rack,inventory.get());
        if(!result->owner)throw std::runtime_error("Tile package skin preparation");
        return result;
    }).get();
} catch(const std::exception& error) {
    std::cerr<<"TILE_FIXTURE_ERROR "<<error.what()<<std::endl;
    return {};
}
}
