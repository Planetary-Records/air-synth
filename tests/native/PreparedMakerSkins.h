#pragma once
#include "MakerFace.h"
#include "FrozenRackExamples.h"
#include "RackLibraryClient.h"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

// The fixtures retain real prepared owners for the duration of each native GUI test.
struct PreparedTestMakerSkins {
    // Package validation creates temporary native images; retain their shared graphics device between files.
    juce::Image nativeDevice{juce::Image::ARGB,1,1,true};
    juce::File root;
    std::shared_ptr<airack::RackPackageStore> store;
    std::vector<airack::studio::MakerSkinPtr> owners;
    ~PreparedTestMakerSkins(){owners.clear();store.reset();if(root.getParentDirectory().getFileName()=="prepared-fixtures")root.deleteRecursively();}
};
inline std::weak_ptr<airack::RackPackageStore> preparedTestRackStore;
inline std::shared_ptr<airack::RackLibraryRuntime> preparedTestRackRuntime(){
    auto runtime=std::make_shared<airack::RackLibraryRuntime>();runtime->store=preparedTestRackStore.lock();
    if(!runtime->store)throw std::runtime_error("prepare explicit maker fixtures before a package-backed test");
    runtime->client=std::make_shared<airack::RackLibraryClient>(runtime->store,[](const auto&,const auto&,const auto&,auto&){return airack::RackLibraryStatus::networkFailure;});return runtime;
}
inline std::shared_ptr<PreparedTestMakerSkins> prepareTestMakerSkins() try {
    return std::async(std::launch::async,[]{
        const auto start=std::chrono::steady_clock::now();
        const auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();};
        auto result=std::make_shared<PreparedTestMakerSkins>();
        const bool native=result->nativeDevice.isValid()&&result->nativeDevice.getPixelData()->createType()->getTypeID()==juce::NativeImageType{}.getTypeID();
        std::cerr<<"MAKER_FIXTURE native_device="<<native<<" elapsed_ms="<<elapsed()<<std::endl;
        if(!native)throw std::runtime_error("private maker fixture native image device");
        const auto workspace=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory();
        result->root=workspace.getChildFile("build/rack-library-native/task8/prepared-fixtures/"+juce::Uuid().toString());
        result->store=std::make_shared<airack::RackPackageStore>(airack::RackLibraryPaths{result->root});
        const auto catalog=juce::JSON::parse(workspace.getChildFile("build/rack-library-native/releases/catalog.json"));
        const auto* racks=catalog["racks"].getArray();if(!racks||racks->size()!=11)throw std::runtime_error("build the11 private release fixtures first");
        for(const auto& row:*racks){const auto ref=row["releases"][0]["ref"];airack::RackPackageRef exact{ref["id"].toString(),ref["version"].toString(),ref["sha256"].toString()};
            const auto archive=workspace.getChildFile("build/rack-library-native/releases/"+exact.id+"-"+exact.version+".airrack");
            const auto status=result->store->install(archive,exact,airack::RackPackageActivation::makeActive,{});
            std::cerr<<"MAKER_FIXTURE install id="<<exact.id<<" status="<<int(status)<<" elapsed_ms="<<elapsed()<<std::endl;
            if(status!=airack::RackLibraryStatus::ready)throw std::runtime_error("private maker package fixture install: "+exact.id.toStdString()+" status="+std::to_string(int(status)));
        }
        airack::RackInventoryPtr inventory;if(!airack::captureRackInventory(*result->store,{},inventory))throw std::runtime_error("private maker package inventory");
        std::cerr<<"MAKER_FIXTURE inventory elapsed_ms="<<elapsed()<<std::endl;
        for(const auto& design:airack::test::frozenCurrentRackDesigns()){
            airack::RackInstance rack;rack.design=design;
            if(auto owner=airack::studio::prepareMakerSkin(rack,inventory.get()))result->owners.push_back(std::move(owner));
            else if(const auto ref=airack::legacyRackPackageRef(design);ref&&ref->id!="contour-dynamics")std::cerr<<"MAKER_FIXTURE skin unavailable id="<<ref->id<<std::endl;
        }
        std::cerr<<"MAKER_FIXTURE prepared owners="<<result->owners.size()<<" total_ms="<<elapsed()<<std::endl;
        if(result->owners.size()!=10)throw std::runtime_error("private maker skin coverage changed: "+std::to_string(result->owners.size()));
        preparedTestRackStore=result->store;
        return result;
    }).get();
} catch(const std::exception& error) {
    std::cerr<<"MAKER_FIXTURE_SETUP_ERROR "<<error.what()<<std::endl;
    std::exit(2);
}
