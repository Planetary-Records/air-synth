#include "RackLibraryClient.h"
#include <windows.h>
namespace airack {RackLibraryTransport rackLibraryHttpWindowsForTest(unsigned);}
namespace {struct Owner {std::shared_ptr<airack::RackPackageStore> store;std::unique_ptr<airack::RackLibraryClient> client;std::uint64_t job=0;};}
extern "C" __declspec(dllexport) void* rackLibraryOwnerStart(const wchar_t* path,unsigned port){
    try{auto owner=std::make_unique<Owner>();owner->store=std::make_shared<airack::RackPackageStore>(airack::RackLibraryPaths{juce::File(juce::String(path))});owner->client=std::make_unique<airack::RackLibraryClient>(owner->store,airack::rackLibraryHttpWindowsForTest(port));owner->job=owner->client->refresh();return owner.release();}catch(...){return nullptr;}
}
extern "C" __declspec(dllexport) void rackLibraryOwnerStop(void* value){auto* owner=static_cast<Owner*>(value);if(owner){owner->client->cancel(owner->job);delete owner;}}
