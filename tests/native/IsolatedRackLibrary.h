#pragma once
#include "InstrumentProcessor.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace airack::testing {
inline juce::var catalogRowId(const juce::var& row){return row.isArray()?row[0]:row["id"];}
inline juce::var catalogRowRevision(const juce::var& row){return row.isArray()?row[1]:row["revision"];}
inline juce::var catalogRowHash(const juce::var& row){return row.isArray()?row[2]:row["content_sha256"];}
struct IsolatedRackRuntime final:RackLibraryRuntime {
    juce::File root;
    std::shared_ptr<std::atomic<unsigned>> gets=std::make_shared<std::atomic<unsigned>>(0);
    IsolatedRackRuntime(){
        const auto base=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("build/rack-library-native/task6/isolated");
        root=base.getChildFile(juce::Uuid().toString());
        store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});
        client=std::make_shared<RackLibraryClient>(store,[count=gets](const RackHttpRequest&,const RackHttpSink&,const RackLibraryCancel&,RackHttpResponse&){++*count;return RackLibraryStatus::networkFailure;});
    }
    ~IsolatedRackRuntime(){client.reset();store.reset();if(root.getParentDirectory().getFileName()=="isolated")root.deleteRecursively();}
};
inline auto isolatedRackRuntime(){return std::make_shared<IsolatedRackRuntime>();}
inline bool readyRackInventory(InstrumentProcessor& processor,RackInventoryPtr& inventory,unsigned timeoutMs=120000){
    const auto limit=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    while(std::chrono::steady_clock::now()<limit){RackLibraryStatus status;if(processor.captureAvailableRackInventory(inventory,&status))return true;
        if(status!=RackLibraryStatus::busy&&status!=RackLibraryStatus::unavailable)return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    return false;
}
inline bool readyRackInventory(InstrumentProcessor& processor){RackInventoryPtr inventory;return readyRackInventory(processor,inventory);}
inline auto beginAiRequest(InstrumentProcessor& processor,const AiRequestOptions& options,std::optional<double> bpm={}){
    RackInventoryPtr inventory;if(options.policy.scope!=ai::RequestScope::pattern&&!readyRackInventory(processor,inventory))return std::optional<ai::GenerationRequest>{};
    return processor.beginAiRequest(options,bpm);
}
inline bool generateAi(InstrumentProcessor& processor,const AiRequestOptions& options){
    return (options.policy.scope==ai::RequestScope::pattern||readyRackInventory(processor))&&processor.generateAi(options);
}
}
