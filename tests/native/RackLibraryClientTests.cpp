#include "RackLibraryClient.h"
#include "RackPackageFixtures.h"
#include "RackExamples.h"
#include <juce_cryptography/juce_cryptography.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <windows.h>
#include <winioctl.h>
using namespace airack;
namespace airack {void setRackLibraryCacheTestHook(std::function<void(const juce::File&,const juce::String&)>);
void setRackLibraryInventoryTestHook(std::function<void(std::uint64_t,unsigned,bool,bool,bool)>);
void setRackLibraryThumbnailAcquisitionTestHook(std::function<void(unsigned,bool,bool,bool)>);}
namespace {
int checks=0,failures=0;
void check(bool b,const char* s,int n){++checks;if(!b){++failures;std::cerr<<n<<": "<<s<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
bool waitForTerminal(const RackLibraryClient& c,std::uint64_t job){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(std::chrono::steady_clock::now()<deadline){const auto v=c.view();if(v->job==job&&!v->busy)return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;}
juce::var object(std::initializer_list<std::pair<juce::Identifier,juce::var>> f){auto* d=new juce::DynamicObject;for(const auto& [k,v]:f)d->setProperty(k,v);return d;}
struct NetworkFixture {
    juce::File root;
    RackPackageManifest first=test::packageFixture(),second=first;
    RackPackageRef one,two;
    juce::File zipOne,zipTwo;
    juce::var doc;
    juce::MemoryBlock image=test::packageFixturePng();
    std::atomic<int> catalogGets{0},countGets{0},archiveGets{0},imageGets{0},mode{0};
    std::atomic<bool> entered{false};
    std::mutex mutex;juce::String lastEtag;
    explicit NetworkFixture(juce::File path):root(std::move(path)){
        CHECK(root.createDirectory().wasOk());second.ref.version="2.0.0";zipOne=test::writePackageZip(root,first);zipTwo=test::writePackageZip(root,second);
        one=first.ref;two=second.ref;{juce::FileInputStream in(zipOne);one.sha256=juce::SHA256(in).toHexString();}{juce::FileInputStream in(zipTwo);two.sha256=juce::SHA256(in).toHexString();}
        const auto release=[&](const RackPackageManifest& m,const RackPackageRef& ref,int asset,const juce::File& file){return object({
            {"ref",object({{"id",ref.id},{"version",ref.version},{"sha256",ref.sha256}})},{"metadata",juce::JSON::parse(test::packageFixtureJson(m))["metadata"]},{"capabilities",m.capabilities},
            {"archive",object({{"assetId",asset},{"url","https://github.com/Planetary-Records/air-rack-library/releases/download/v1/"+file.getFileName()},{"bytes",juce::int64(file.getSize())},{"sha256",ref.sha256}})},
            {"screenshot",object({{"url","https://raw.githubusercontent.com/Planetary-Records/air-rack-library/main/screenshot.png"},{"bytes",juce::int64(test::packageFixturePng().getSize())},{"sha256",juce::SHA256(test::packageFixturePng()).toHexString()}})}});};
        doc=object({{"schema",1},{"tags",juce::Array<juce::var>{object({{"id","analog"},{"label","Analog"}}),object({{"id","warm"},{"label","Warm"}})}},{"racks",juce::Array<juce::var>{object({{"id",one.id},{"order",0},{"releases",juce::Array<juce::var>{release(first,one,101,zipOne),release(second,two,102,zipTwo)}}})}}});
    }
    RackLibraryTransport transport(){return [this](const RackHttpRequest& request,const RackHttpSink& sink,const RackLibraryCancel& cancel,RackHttpResponse& response){
        const auto current=mode.load();response.code=200;response.etag="fixture-etag";
        if(request.url==rackLibraryCatalogUrl){++catalogGets;{const std::lock_guard lock(mutex);lastEtag=request.etag;}entered=true;
            if(current==1)return RackLibraryStatus::networkFailure;
            if(current==2){response.code=304;return RackLibraryStatus::ready;}
            if(current==3){response.code=429;response.retryAfter="7200";response.rateLimitRemaining="0";response.rateLimitReset=juce::String(std::to_string(juce::Time::currentTimeMillis()/1000+8000));return RackLibraryStatus::rateLimited;}
            if(current==4){while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));return RackLibraryStatus::cancelled;}
            if(current==5){const auto bytes=juce::String::repeatedString("x",int(rackPackageLimits::catalogBytes+1));return sink(bytes.toRawUTF8(),bytes.getNumBytesAsUTF8())?RackLibraryStatus::ready:RackLibraryStatus::byteLimit;}
            const auto body=current==6?juce::String("{"):juce::JSON::toString(doc,true);return sink(body.toRawUTF8(),body.getNumBytesAsUTF8())?RackLibraryStatus::ready:RackLibraryStatus::byteLimit;
        }
        if(request.url.startsWith("https://api.github.com/")){++countGets;
            if(current==2){response.code=304;return RackLibraryStatus::ready;}
            juce::String body=juce::File(RACKSYNTH_CLIENT_FIXTURE_DIR).getChildFile("counts.json").loadFileAsString();
            if(current==7)body="[{\"assets\":[{\"id\":101,\"download_count\":1}]}]";
            if(current==8)body="[{\"assets\":[{\"id\":101,\"download_count\":0},{\"id\":102,\"download_count\":0}]}]";
            if(current==9){response.code=403;return RackLibraryStatus::rateLimited;}
            if(current==10){response.link="<https://api.github.com/repos/Other/private/releases?per_page=100&page=2>; rel=\"next\"";}
            if(current==11){body="[{\"assets\":[{\"id\":101,\"download_count\":9223372036854775807},{\"id\":102,\"download_count\":9223372036854775807},{\"id\":103,\"download_count\":9223372036854775807}]}]";}
            if(current==12){response.link="<https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=100&page="+juce::String(request.url.fromLastOccurrenceOf("=",false,false).getIntValue()+1)+">; rel=\"next\"";}
            if(current==13&&request.url.endsWith("page=1")){body="[{\"assets\":[{\"id\":101,\"download_count\":20}]}]";response.link="<https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=100&page=2>; rel=\"next\"";}
            return sink(body.toRawUTF8(),body.getNumBytesAsUTF8())?RackLibraryStatus::ready:RackLibraryStatus::byteLimit;
        }
        if(request.url.endsWith(".png")){++imageGets;const auto data=current==14?juce::MemoryBlock("broken",6):image;return sink(data.getData(),data.getSize())?RackLibraryStatus::ready:RackLibraryStatus::byteLimit;}
        ++archiveGets;if(current==15){auto input=zipOne.createInputStream();char prefix[8]{};if(input){const auto n=input->read(prefix,8);if(!sink(prefix,std::size_t(n)))return RackLibraryStatus::byteLimit;}entered=true;while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));return RackLibraryStatus::cancelled;}entered=true;
        const auto file=request.url.endsWith(zipOne.getFileName())?zipOne:zipTwo;auto input=file.createInputStream();if(!input)return RackLibraryStatus::storageFailure;char buffer[511];for(;;){if(cancel())return RackLibraryStatus::cancelled;const auto got=input->read(buffer,sizeof(buffer));if(!got)break;if(!sink(buffer,std::size_t(got)))return RackLibraryStatus::byteLimit;}return RackLibraryStatus::ready;
    };}
};
bool waitUntil(const std::function<bool()>& predicate){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(std::chrono::steady_clock::now()<deadline){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;}
void extended(const juce::File& root){
    NetworkFixture f(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});RackLibraryClient client(store,f.transport());
    auto run=[&](std::uint64_t job){CHECK(waitForTerminal(client,job));return client.view()->status;};
    CHECK(run(client.refresh())==RackLibraryStatus::ready);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});CHECK(client.view()->releases.front().ref==f.two);CHECK(store->active().empty());
    const auto snapshot=client.view();const auto cache=root.getChildFile("store/cache/catalog.json").loadFileAsString();const auto requests=f.catalogGets.load();
    CHECK(run(client.openLibrary())==RackLibraryStatus::ready);CHECK(f.catalogGets==requests);
    f.mode=2;CHECK(run(client.refresh())==RackLibraryStatus::ready);CHECK(client.view()->cached);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});{const std::lock_guard lock(f.mutex);CHECK(f.lastEtag=="fixture-etag");}
    f.mode=1;CHECK(run(client.refresh())==RackLibraryStatus::networkFailure);CHECK(client.view()->offline&&client.view()->cached);CHECK(client.view()->releases.size()==2);
    for(const int mode:{5,6}){f.mode=mode;const auto status=run(client.refresh());CHECK(status==RackLibraryStatus::byteLimit||status==RackLibraryStatus::invalidData);CHECK(client.view()->releases.size()==2);}
    auto original=f.doc.clone();auto releases=f.doc["racks"][0]["releases"].getArray();
    (*releases)[0]["metadata"].getDynamicObject()->setProperty("tags",juce::Array<juce::var>{"unknown"});f.mode=0;CHECK(run(client.refresh())==RackLibraryStatus::invalidData);f.doc=original.clone();
    f.doc.getDynamicObject()->setProperty("unknown",1);CHECK(run(client.refresh())==RackLibraryStatus::invalidData);f.doc=original.clone();
    releases=f.doc["racks"][0]["releases"].getArray();(*releases)[0]["ref"].getDynamicObject()->setProperty("sha256",juce::String::repeatedString("c",64));(*releases)[0]["archive"].getDynamicObject()->setProperty("sha256",juce::String::repeatedString("c",64));CHECK(run(client.refresh())==RackLibraryStatus::identityConflict);f.doc=original.clone();
    CHECK(root.getChildFile("store/cache/catalog.json").loadFileAsString()!=juce::String());
    CHECK(run(client.install(f.one))==RackLibraryStatus::ready);CHECK(store->active()==std::vector<RackPackageRef>{f.one});const auto generation=client.view()->generation;
    CHECK(run(client.install(f.two))==RackLibraryStatus::ready);CHECK(store->active()==std::vector<RackPackageRef>{f.two});CHECK(client.view()->generation>generation);
    CHECK(run(client.restore(f.one))==RackLibraryStatus::ready);CHECK(store->active()==std::vector<RackPackageRef>{f.two});
    auto lease=store->open(f.one);CHECK(lease!=nullptr);CHECK(run(client.remove(f.one))==RackLibraryStatus::busy);lease.reset();CHECK(run(client.remove(f.one))==RackLibraryStatus::ready);CHECK(store->active()==std::vector<RackPackageRef>{f.two});
    f.mode=15;f.entered=false;const auto cancelJob=client.install(f.one);CHECK(waitUntil([&]{return f.entered.load();}));client.cancel(cancelJob);CHECK(run(cancelJob)==RackLibraryStatus::cancelled);CHECK(store->active()==std::vector<RackPackageRef>{f.two});CHECK(root.getChildFile("store/cache").findChildFiles(juce::File::findFiles,false,"download-*.zip").isEmpty());
    f.mode=4;f.entered=false;const auto old=client.refresh();CHECK(waitUntil([&]{return f.entered.load();}));f.mode=0;const auto newer=client.refresh();CHECK(newer>old);CHECK(run(newer)==RackLibraryStatus::ready);CHECK(client.view()->job==newer);
    client.requestThumbnail(f.one);CHECK(waitUntil([&]{return bool(client.thumbnail(f.one));}));CHECK(client.thumbnail(f.one)->status==RackLibraryStatus::ready);CHECK(client.thumbnail(f.one)->image.getWidth()==1);const auto imageGets=f.imageGets.load();client.requestThumbnail(f.one,2048);CHECK(waitUntil([&]{return bool(client.thumbnail(f.one,2048));}));CHECK(f.imageGets==imageGets);
    f.mode=7;CHECK(run(client.refresh())==RackLibraryStatus::invalidData);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});
    f.mode=10;CHECK(run(client.refresh())==RackLibraryStatus::invalidData);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});
    f.mode=13;CHECK(run(client.refresh())==RackLibraryStatus::ready);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});
    f.mode=8;CHECK(run(client.refresh())==RackLibraryStatus::ready);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{0});
    f.mode=3;CHECK(run(client.refresh())==RackLibraryStatus::rateLimited);const auto rateGets=f.catalogGets.load();f.mode=0;CHECK(run(client.refresh())==RackLibraryStatus::rateLimited);CHECK(f.catalogGets==rateGets);
    const auto rate=juce::JSON::parse(root.getChildFile("store/cache/rate.json").loadFileAsString());CHECK(rate["until"].toString().getLargeIntValue()>=juce::Time::currentTimeMillis()+7900000);const auto archiveGets=f.archiveGets.load();CHECK(run(client.install(f.one))==RackLibraryStatus::rateLimited);CHECK(f.archiveGets==archiveGets);CHECK(store->active()==std::vector<RackPackageRef>{f.two});
    {RackLibraryClient other(store,f.transport());const auto job=other.refresh();CHECK(waitForTerminal(other,job));CHECK(other.view()->status==RackLibraryStatus::rateLimited);CHECK(f.catalogGets==rateGets);}
    NetworkFixture offline(root.getChildFile("offline"));offline.mode=1;auto empty=std::make_shared<RackPackageStore>(RackLibraryPaths{offline.root.getChildFile("store")});RackLibraryClient disconnected(empty,offline.transport());const auto offlineJob=disconnected.refresh();CHECK(waitForTerminal(disconnected,offlineJob));CHECK(disconnected.view()->releases.empty());CHECK(disconnected.view()->counts.empty());CHECK(disconnected.view()->offline);CHECK(empty->active().empty());
    NetworkFixture missing(root.getChildFile("unknown"));missing.mode=7;auto missingStore=std::make_shared<RackPackageStore>(RackLibraryPaths{missing.root.getChildFile("store")});RackLibraryClient unknown(missingStore,missing.transport());const auto unknownJob=unknown.refresh();CHECK(waitForTerminal(unknown,unknownJob));CHECK(unknown.view()->counts.size()==1);CHECK(!unknown.view()->counts.front().value.has_value());
    CHECK(snapshot->releases.size()==2&&snapshot->status==RackLibraryStatus::ready);
}

void changedCatalogCounts(const juce::File& root){
    NetworkFixture f(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    {
        RackLibraryClient client(store,f.transport());CHECK(waitForTerminal(client,client.refresh()));CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});
        f.doc["tags"][0].getDynamicObject()->setProperty("label","Analog updated");f.mode=7;
        CHECK(waitForTerminal(client,client.refresh()));CHECK(client.view()->status==RackLibraryStatus::invalidData);
        CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});CHECK(client.view()->countsStale);
        f.mode=9;CHECK(waitForTerminal(client,client.refresh()));CHECK(client.view()->status==RackLibraryStatus::rateLimited);
        CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});
    }
    {
        RackLibraryClient restarted(store,f.transport());CHECK(waitForTerminal(restarted,restarted.openLibrary()));CHECK(restarted.view()->status==RackLibraryStatus::rateLimited);
        CHECK(restarted.view()->counts.size()==1&&restarted.view()->counts.front().value==std::optional<std::uint64_t>{47});CHECK(restarted.view()->countsStale);
    }
    CHECK(root.getChildFile("store/cache/rate.json").deleteFile());f.mode=8;
    RackLibraryClient replacement(store,f.transport());CHECK(waitForTerminal(replacement,replacement.refresh()));CHECK(replacement.view()->status==RackLibraryStatus::ready);
    CHECK(replacement.view()->counts.front().value==std::optional<std::uint64_t>{0});CHECK(!replacement.view()->countsStale);
    CHECK(store->install(f.zipOne,f.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    CHECK(store->install(f.zipTwo,f.two,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    const auto retained=store->committed();CHECK(retained.size()==2&&std::find(retained.begin(),retained.end(),f.one)!=retained.end());
}

void validationAndCache(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});RackLibraryClient client(store,fixture.transport());
    auto run=[&]{const auto job=client.refresh();CHECK(waitForTerminal(client,job));return client.view()->status;};CHECK(run()==RackLibraryStatus::ready);
    const auto original=fixture.doc.clone();const auto cacheFile=root.getChildFile("store/cache/catalog.json");const auto cacheBytes=cacheFile.loadFileAsString();
    const auto invalid=[&](const std::function<void(juce::var&)>& change){fixture.doc=original.clone();change(fixture.doc);CHECK(run()!=RackLibraryStatus::ready);CHECK(cacheFile.loadFileAsString()==cacheBytes);CHECK(client.view()->releases.size()==2);};
    invalid([](auto& doc){auto* array=doc["racks"][0]["releases"].getArray();array->add((*array)[0].clone());});
    invalid([](auto& doc){doc["racks"][0]["releases"][0]["archive"].getDynamicObject()->setProperty("url","https://github.com/Private/source/releases/download/v/a.zip");});
    invalid([](auto& doc){doc["racks"][0]["releases"][0]["capabilities"].getDynamicObject()->setProperty("bad",1);});
    invalid([](auto& doc){doc["racks"][0]["releases"][0]["capabilities"]["modules"].getArray()->add(doc["racks"][0]["releases"][0]["capabilities"]["modules"][0]);});
    invalid([](auto& doc){doc["racks"][0]["releases"][0]["capabilities"].getDynamicObject()->setProperty("nativeEditor","bad token");});
    invalid([](auto& doc){auto* array=doc["racks"][0]["releases"].getArray();const auto first=(*array)[0].clone();while(array->size()<65){auto next=first.clone();next["ref"].getDynamicObject()->setProperty("version",juce::String(array->size()+1)+".0.0");array->add(next);}});
    fixture.doc=original.clone();auto future=fixture.doc["racks"][0]["releases"][1].clone();future["ref"].getDynamicObject()->setProperty("version","3.0.0");future["capabilities"].getDynamicObject()->setProperty("airMin","9.0.0");fixture.doc["racks"][0]["releases"].getArray()->add(future);CHECK(run()==RackLibraryStatus::ready);CHECK(client.view()->releases.size()==3);CHECK(!client.view()->releases.front().compatible);const auto incompatible=client.view()->releases.front().ref;auto job=client.install(incompatible);CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::incompatible);CHECK(store->active().empty());
    fixture.doc=original.clone();CHECK(run()==RackLibraryStatus::ready);
    fixture.doc["racks"][0]["releases"][0]["capabilities"]["modules"].getArray()->removeLast();CHECK(run()==RackLibraryStatus::ready);auto mismatch=client.install(fixture.one);CHECK(waitForTerminal(client,mismatch));CHECK(client.view()->status==RackLibraryStatus::identityConflict);CHECK(store->active().empty());fixture.doc=original.clone();CHECK(run()==RackLibraryStatus::ready);
    fixture.mode=12;CHECK(run()!=RackLibraryStatus::ready);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});fixture.mode=0;
    auto third=fixture.doc["racks"][0]["releases"][1].clone();third["ref"].getDynamicObject()->setProperty("version","4.0.0");third["archive"].getDynamicObject()->setProperty("assetId",103);fixture.doc["racks"][0]["releases"].getArray()->add(third);fixture.mode=11;CHECK(run()==RackLibraryStatus::byteLimit);fixture.doc=original.clone();fixture.mode=0;CHECK(run()==RackLibraryStatus::ready);
    // Stale successful timestamps cause a conditional refresh; a fresh second process reuses bytes.
    auto state=juce::JSON::parse(cacheFile.loadFileAsString());state.getDynamicObject()->setProperty("time",juce::String(std::to_string(juce::Time::currentTimeMillis()-16*60*1000)));CHECK(cacheFile.replaceWithText(juce::JSON::toString(state,true)));const auto requests=fixture.catalogGets.load();job=client.openLibrary();CHECK(waitForTerminal(client,job));CHECK(fixture.catalogGets==requests+1);
    {RackLibraryClient second(store,fixture.transport());const auto open=second.openLibrary();CHECK(waitForTerminal(second,open));CHECK(second.view()->cached);CHECK(fixture.catalogGets==requests+1);}
    // A broken screenshot has a bounded unavailable image owner and leaves usable releases.
    NetworkFixture broken(root.getChildFile("broken"));broken.mode=14;auto brokenStore=std::make_shared<RackPackageStore>(RackLibraryPaths{broken.root.getChildFile("store")});RackLibraryClient brokenClient(brokenStore,broken.transport());job=brokenClient.refresh();CHECK(waitForTerminal(brokenClient,job));brokenClient.requestThumbnail(broken.one);CHECK(waitUntil([&]{return bool(brokenClient.thumbnail(broken.one));}));CHECK(brokenClient.thumbnail(broken.one)->status!=RackLibraryStatus::ready);CHECK(brokenClient.view()->releases.size()==2);CHECK(brokenStore->active().empty());
    // Lease contention waits for the committed cache; it does not duplicate requests.
    NetworkFixture concurrent(root.getChildFile("concurrent"));auto shared=std::make_shared<RackPackageStore>(RackLibraryPaths{concurrent.root.getChildFile("store")});std::atomic<bool> hold{true},entered{false};const auto base=concurrent.transport();auto slow=[&](const auto& r,const auto& sink,const auto& cancel,auto& response){if(r.url==rackLibraryCatalogUrl){entered=true;while(hold&&!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));}return base(r,sink,cancel,response);};RackLibraryClient first(shared,slow),second(shared,concurrent.transport());const auto firstJob=first.refresh();CHECK(waitUntil([&]{return entered.load();}));const auto secondJob=second.refresh();std::this_thread::sleep_for(std::chrono::milliseconds(50));hold=false;CHECK(waitForTerminal(first,firstJob));CHECK(waitForTerminal(second,secondJob));CHECK(first.view()->status==RackLibraryStatus::ready&&second.view()->status==RackLibraryStatus::ready);CHECK(concurrent.catalogGets==1&&concurrent.countGets==1);
    // The same worker owns transport and preparation; stale skin revisions never publish.
    std::thread::id networkThread,skinThread;std::mutex threadMutex;auto threadTransport=[&](const auto& r,const auto& sink,const auto& cancel,auto& response){{const std::lock_guard lock(threadMutex);networkThread=std::this_thread::get_id();}return base(r,sink,cancel,response);};RackLibraryClient sameWorker(shared,threadTransport);job=sameWorker.refresh();CHECK(waitForTerminal(sameWorker,job));std::atomic<int> prepared{0};auto skin=sameWorker.createSkinPreparation([&](const auto&,const auto*,const auto& cancel){skinThread=std::this_thread::get_id();if(!cancel())++prepared;return std::shared_ptr<const studio::MakerSkin>{};});RackInstance rack;rack.design=test::historicalRackFixtures().front();sameWorker.prepareSkins(skin,{},std::span(&rack,1));CHECK(waitUntil([&]{return prepared.load()==1;}));{const std::lock_guard lock(threadMutex);CHECK(networkThread==skinThread);}
    // Client destruction cancels and joins an injected stalled control operation.
    concurrent.mode=4;concurrent.entered=false;auto stalled=std::make_unique<RackLibraryClient>(shared,concurrent.transport());stalled->refresh();CHECK(waitUntil([&]{return concurrent.entered.load();}));const auto start=std::chrono::steady_clock::now();stalled.reset();CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(5));
}

bool junction(const juce::File& link,const juce::File& target,DWORD* error=nullptr){
    if(link.createDirectory().failed())return false;const auto print=std::wstring(target.getFullPathName().replaceCharacter('/','\\').toWideCharPointer()),substitute=L"\\??\\"+print;
    std::vector<unsigned char> bytes(16+(substitute.size()+print.size()+2)*sizeof(wchar_t));*reinterpret_cast<DWORD*>(bytes.data())=IO_REPARSE_TAG_MOUNT_POINT;auto* words=reinterpret_cast<WORD*>(bytes.data()+4);words[0]=WORD(bytes.size()-8);words[1]=0;words[2]=0;words[3]=WORD(substitute.size()*2);words[4]=WORD((substitute.size()+1)*2);words[5]=WORD(print.size()*2);std::memcpy(bytes.data()+16,substitute.data(),substitute.size()*2);std::memcpy(bytes.data()+16+(substitute.size()+1)*2,print.data(),print.size()*2);
    HANDLE h=CreateFileW(link.getFullPathName().toWideCharPointer(),FILE_WRITE_ATTRIBUTES|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);if(h==INVALID_HANDLE_VALUE){if(error)*error=GetLastError();return false;}DWORD returned=0;const auto result=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,bytes.data(),DWORD(bytes.size()),nullptr,0,&returned,nullptr);if(error)*error=result?0:GetLastError();BY_HANDLE_FILE_INFORMATION info{};const bool actual=result&&GetFileInformationByHandle(h,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);CloseHandle(h);return actual;
}
bool clearJunction(const juce::File& link){HANDLE h=CreateFileW(link.getFullPathName().toWideCharPointer(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);if(h==INVALID_HANDLE_VALUE)return false;std::array<unsigned char,16384> data{};DWORD returned=0;bool ok=DeviceIoControl(h,FSCTL_GET_REPARSE_POINT,nullptr,0,data.data(),DWORD(data.size()),&returned,nullptr)&&*reinterpret_cast<DWORD*>(data.data())==IO_REPARSE_TAG_MOUNT_POINT;struct {DWORD tag;WORD size,reserved;} clear{IO_REPARSE_TAG_MOUNT_POINT,0,0};ok=ok&&DeviceIoControl(h,FSCTL_DELETE_REPARSE_POINT,&clear,sizeof(clear),nullptr,0,&returned,nullptr);CloseHandle(h);return ok;}
bool guidReparse(const juce::File& file){
    constexpr DWORD tag=0x00000042;const GUID guid{0x438a6d38,0x1f0d,0x49d0,{0xa2,0x39,0x42,0x8d,0x93,0x11,0x70,0x15}};
    struct Buffer{DWORD tag;WORD size,reserved;GUID guid;char payload[16];}buffer{tag,16,0,guid,{"nonrouting-test"}};
    HANDLE h=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD returned=0;
    const auto created=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&buffer,sizeof(buffer),nullptr,0,&returned,nullptr);const auto error=GetLastError();BY_HANDLE_FILE_INFORMATION info{};const bool actual=created&&GetFileInformationByHandle(h,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
    unsigned char data[1024]{};const bool read=actual&&DeviceIoControl(h,FSCTL_GET_REPARSE_POINT,nullptr,0,data,sizeof(data),&returned,nullptr);const bool match=read&&*reinterpret_cast<DWORD*>(data)==tag&&std::memcmp(data+8,&guid,sizeof(guid))==0;CloseHandle(h);std::cout<<"guid_reparse_created="<<bool(created)<<" actual="<<actual<<" matched="<<match<<" api_error="<<(created?0:error)<<'\n';return match;
}
void clearGuidReparse(const juce::File& file){
    HANDLE h=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(h==INVALID_HANDLE_VALUE)return;unsigned char data[1024]{};DWORD returned=0;const GUID guid{0x438a6d38,0x1f0d,0x49d0,{0xa2,0x39,0x42,0x8d,0x93,0x11,0x70,0x15}};
    if(DeviceIoControl(h,FSCTL_GET_REPARSE_POINT,nullptr,0,data,sizeof(data),&returned,nullptr)&&returned>=24&&*reinterpret_cast<DWORD*>(data)==0x00000042&&std::memcmp(data+8,&guid,sizeof(guid))==0){*reinterpret_cast<WORD*>(data+4)=0;CHECK(DeviceIoControl(h,FSCTL_DELETE_REPARSE_POINT,data,24,nullptr,0,&returned,nullptr));}CloseHandle(h);
}
void hostileCache(const juce::File& root){
    CHECK(root.createDirectory().wasOk());const auto outside=root.getChildFile("outside");CHECK(outside.createDirectory().wasOk());const auto sentinel=outside.getChildFile("sentinel.txt");CHECK(sentinel.replaceWithText("outside bytes stay exact"));const auto initial=sentinel.loadFileAsString();
    {NetworkFixture fixture(root.getChildFile("junction"));const auto cache=fixture.root.getChildFile("store/cache");CHECK(cache.getParentDirectory().createDirectory().wasOk());CHECK(junction(cache,outside));auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});RackLibraryClient client(store,fixture.transport());const auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::storageFailure);CHECK(fixture.catalogGets==0);CHECK(!outside.getChildFile("catalog.json").exists());CHECK(sentinel.loadFileAsString()==initial);}
    {NetworkFixture fixture(root.getChildFile("hard-cache"));const auto cache=fixture.root.getChildFile("store/cache");CHECK(cache.createDirectory().wasOk());const auto linked=cache.getChildFile("catalog.json");CHECK(CreateHardLinkW(linked.getFullPathName().toWideCharPointer(),sentinel.getFullPathName().toWideCharPointer(),nullptr));fixture.mode=1;auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});RackLibraryClient client(store,fixture.transport());auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->releases.empty());CHECK(sentinel.loadFileAsString()==initial);fixture.mode=0;job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::ready);CHECK(sentinel.loadFileAsString()==initial);}
    for(const bool reparse:{false,true}){
        NetworkFixture fixture(root.getChildFile(reparse?"guid-image":"hard-image"));const auto images=fixture.root.getChildFile("store/cache/images");CHECK(images.createDirectory().wasOk());const auto png=outside.getChildFile(reparse?"reparse-sentinel.png":"hardlink-target.png");const auto data=test::packageFixturePng();CHECK(png.replaceWithData(data.getData(),data.getSize()));const auto stamp=juce::Time(1000000000);CHECK(png.setLastModificationTime(stamp));const auto hash=juce::SHA256(data).toHexString();const auto link=images.getChildFile(hash+".png");
        const bool created=reparse?guidReparse(link):CreateHardLinkW(link.getFullPathName().toWideCharPointer(),png.getFullPathName().toWideCharPointer(),nullptr)!=0;
        CHECK(created);if(!created)continue;
        auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});RackLibraryClient client(store,fixture.transport());const auto job=client.refresh();CHECK(waitForTerminal(client,job));client.requestThumbnail(fixture.one);CHECK(waitUntil([&]{return bool(client.thumbnail(fixture.one));}));CHECK(client.thumbnail(fixture.one)->status==RackLibraryStatus::ready);CHECK(fixture.imageGets==1);CHECK(juce::SHA256(*png.createInputStream()).toHexString()==hash);CHECK(png.getLastModificationTime()==stamp);
        // Eviction removes only the owned name; a hash-named outside link is never followed or touched.
        const auto evil=images.getChildFile(juce::String::repeatedString("d",64)+".png");CHECK(reparse?guidReparse(evil):CreateHardLinkW(evil.getFullPathName().toWideCharPointer(),png.getFullPathName().toWideCharPointer(),nullptr)!=0);
        juce::MemoryBlock fill(8*1024*1024,true);for(int i=1;i<=5;++i)CHECK(images.getChildFile(juce::String::repeatedString(juce::String(i),64)+".png").replaceWithData(fill.getData(),fill.getSize()));
        // Force a stale encoded-cache revalidation through a second client.
        images.getChildFile(hash+".json").deleteFile();RackLibraryClient second(store,fixture.transport());const auto open=second.openLibrary();CHECK(waitForTerminal(second,open));second.requestThumbnail(fixture.two);CHECK(waitUntil([&]{return bool(second.thumbnail(fixture.two));}));CHECK(second.thumbnail(fixture.two)->status==RackLibraryStatus::ready);std::uint64_t size=0;for(const auto& file:images.findChildFiles(juce::File::findFiles,false,"*.png"))size+=std::uint64_t(file.getSize());CHECK(size<=32ull*1024*1024);CHECK(png.existsAsFile());CHECK(juce::SHA256(*png.createInputStream()).toHexString()==hash);CHECK(png.getLastModificationTime()==stamp);if(reparse){clearGuidReparse(link);clearGuidReparse(evil);}
    }
}

}
void thumbnailBounds(const juce::File& root){
    NetworkFixture fixture(root);auto* releases=fixture.doc["racks"][0]["releases"].getArray();const auto first=(*releases)[0].clone();for(int i=3;i<=18;++i){auto next=first.clone();next["ref"].getDynamicObject()->setProperty("version",juce::String(i)+".0.0");releases->add(next);}
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});RackLibraryClient client(store,fixture.transport());auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->releases.size()==18);const auto refs=client.view()->releases;
    std::shared_ptr<const RackLibraryThumbnail> held;for(const auto& release:refs){client.requestThumbnail(release.ref);CHECK(waitUntil([&]{return bool(client.thumbnail(release.ref));}));if(!held)held=client.thumbnail(release.ref);}CHECK(!client.thumbnail(refs.front().ref));CHECK(held&&held->image.isValid());unsigned retained=0;for(const auto& release:refs)if(client.thumbnail(release.ref))++retained;CHECK(retained==16);
    NetworkFixture big(root.getChildFile("big"));juce::Image image(juce::Image::ARGB,2048,2048,true);image.clear(image.getBounds(),juce::Colours::orange);juce::MemoryOutputStream encoded;CHECK(juce::PNGImageFormat().writeImageToStream(image,encoded));big.image=encoded.getMemoryBlock();for(auto& release:*big.doc["racks"][0]["releases"].getArray()){release["screenshot"].getDynamicObject()->setProperty("bytes",juce::int64(big.image.getSize()));release["screenshot"].getDynamicObject()->setProperty("sha256",juce::SHA256(big.image).toHexString());}
    auto bigStore=std::make_shared<RackPackageStore>(RackLibraryPaths{big.root.getChildFile("store")});RackLibraryClient large(bigStore,big.transport());job=large.refresh();CHECK(waitForTerminal(large,job));large.requestThumbnail(big.one,2048);CHECK(waitUntil([&]{return bool(large.thumbnail(big.one,2048));}));const auto owner=large.thumbnail(big.one,2048);CHECK(owner&&owner->status==RackLibraryStatus::ready&&owner->image.getWidth()==2048);large.requestThumbnail(big.two,2048);CHECK(waitUntil([&]{return bool(large.thumbnail(big.two,2048));}));CHECK(!large.thumbnail(big.one,2048));CHECK(owner->image.getWidth()==2048);CHECK(big.imageGets==1);
    // At most12 used skins remain requested while the single explicit preview changes.
    std::mutex namesMutex;std::vector<juce::String> names;std::atomic<unsigned> completed{0};auto state=large.createSkinPreparation([&](const auto& rack,const auto*,const auto& cancel){if(!cancel()){const std::lock_guard lock(namesMutex);names.push_back(rack.design.name);++completed;}return std::shared_ptr<const studio::MakerSkin>{};});std::vector<RackInstance> used(12);for(unsigned i=0;i<used.size();++i){used[i].design.name="used-"+juce::String(i);}large.prepareSkins(state,{},used);CHECK(waitUntil([&]{return completed==12;}));RackInstance preview;preview.design.name="preview-A";large.requestSkin(state,preview);CHECK(waitUntil([&]{return completed==25;}));{const std::lock_guard lock(namesMutex);names.clear();}preview.design.name="preview-B";large.requestSkin(state,preview);CHECK(waitUntil([&]{return completed==38;}));{const std::lock_guard lock(namesMutex);CHECK(names.size()==13);CHECK(std::find(names.begin(),names.end(),"used-0")!=names.end());CHECK(std::find(names.begin(),names.end(),"preview-A")==names.end());CHECK(names.back()=="preview-B");}
}
void processReuse(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});std::atomic<bool> entered{false},hold{true};const auto transport=fixture.transport();RackLibraryClient client(store,[&](const auto& request,const auto& sink,const auto& cancel,auto& response){if(request.url==rackLibraryCatalogUrl){entered=true;while(hold&&!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));}return transport(request,sink,cancel,response);});const auto job=client.refresh();CHECK(waitUntil([&]{return entered.load();}));const auto marker=root.getChildFile("peer-ready");juce::ChildProcess child;CHECK(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--refresh-peer",root.getChildFile("store").getFullPathName(),marker.getFullPathName()}));CHECK(waitUntil([&]{return marker.existsAsFile();}));std::this_thread::sleep_for(std::chrono::milliseconds(100));hold=false;CHECK(waitForTerminal(client,job));const auto finished=child.waitForProcessToFinish(5000);CHECK(finished);if(!finished){child.kill();child.waitForProcessToFinish(5000);}std::cout<<child.readAllProcessOutput();CHECK(child.getExitCode()==0);CHECK(fixture.catalogGets==1&&fixture.countGets==1);
}
void fallbackRate(const juce::File& root){
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});const auto file=root.getChildFile("cache/rate.json");for(unsigned attempt=0;attempt<2;++attempt){
        if(attempt){auto doc=juce::JSON::parse(file.loadFileAsString());doc.getDynamicObject()->setProperty("until","0");CHECK(file.replaceWithText(juce::JSON::toString(doc,true)));}
        RackLibraryClient client(store,[](const auto&,const auto&,const auto&,RackHttpResponse& response){response.code=403;response.retryAfter="18446744073709551615";response.rateLimitRemaining="0";response.rateLimitReset="NaN";return RackLibraryStatus::rateLimited;});const auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::rateLimited);const auto doc=juce::JSON::parse(file.loadFileAsString());const auto remaining=doc["until"].toString().getLargeIntValue()-juce::Time::currentTimeMillis();CHECK(remaining>=(60ll<<attempt)*1000-1000);CHECK(remaining<=(60ll<<attempt)*1000);CHECK(client.view()->releases.empty());
    }
}
void successfulQuota(const juce::File& root){
    for(const bool intermediate:{false,true}){NetworkFixture fixture(root.getChildFile(intermediate?"intermediate":"complete"));auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{fixture.root.getChildFile("store")});const auto ordinary=fixture.transport();std::atomic<bool> exhaust{false};std::atomic<int> laterGets{0};const auto reset=juce::Time::currentTimeMillis()/1000+7200;
        auto transport=[&](const auto& request,const auto& sink,const auto& cancel,auto& response){if(exhaust&&request.url.endsWith("page=2"))++laterGets;const auto result=ordinary(request,sink,cancel,response);if(exhaust&&request.url==rackLibraryCountsUrl){response.rateLimitRemaining="0";response.rateLimitReset=juce::String(std::to_string(reset));if(intermediate)response.link="<https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=100&page=2>; rel=\"next\"";}return result;};
        RackLibraryClient client(store,transport);auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::ready);const auto countsFile=fixture.root.getChildFile("store/cache/counts.json");const auto oldCounts=countsFile.loadFileAsString();exhaust=true;job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==(intermediate?RackLibraryStatus::rateLimited:RackLibraryStatus::ready));CHECK(laterGets==0);CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});if(intermediate)CHECK(countsFile.loadFileAsString()==oldCounts);
        const auto rate=juce::JSON::parse(fixture.root.getChildFile("store/cache/rate.json").loadFileAsString());CHECK(rate["until"].toString().getLargeIntValue()>=reset*1000);CHECK(int(rate["failures"])==0);const auto catalogGets=fixture.catalogGets.load(),countGets=fixture.countGets.load();job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::rateLimited);CHECK(fixture.catalogGets==catalogGets&&fixture.countGets==countGets);
        RackLibraryClient second(store,transport);job=second.refresh();CHECK(waitForTerminal(second,job));CHECK(second.view()->status==RackLibraryStatus::rateLimited);CHECK(fixture.catalogGets==catalogGets&&fixture.countGets==countGets);second.requestThumbnail(fixture.one);CHECK(waitUntil([&]{return bool(second.thumbnail(fixture.one));}));CHECK(second.thumbnail(fixture.one)->status==RackLibraryStatus::rateLimited);CHECK(fixture.imageGets==0);job=second.install(fixture.one);CHECK(waitForTerminal(second,job));CHECK(second.view()->status==RackLibraryStatus::rateLimited);CHECK(fixture.archiveGets==0);
    }
}
void redirectQuota(const juce::File& root){
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});std::atomic<unsigned> requests{0};const auto reset=juce::Time::currentTimeMillis()/1000+7200;const auto transport=[&](const auto&,const auto&,const auto&,RackHttpResponse& response){++requests;response.code=302;response.rateLimitRemaining="0";response.rateLimitReset=juce::String(std::to_string(reset));return RackLibraryStatus::rateLimited;};RackLibraryClient client(store,transport);auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::rateLimited);const auto rate=juce::JSON::parse(root.getChildFile("cache/rate.json").loadFileAsString());CHECK(rate["until"].toString().getLargeIntValue()>=reset*1000);CHECK(int(rate["failures"])==0);job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(requests==1);RackLibraryClient second(store,transport);job=second.refresh();CHECK(waitForTerminal(second,job));CHECK(second.view()->status==RackLibraryStatus::rateLimited);CHECK(requests==1);
}
void cancellationAndRootPin(const juce::File& root){
    NetworkFixture fixture(root);fixture.mode=4;auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});RackLibraryClient client(store,fixture.transport());const auto job=client.refresh();CHECK(waitUntil([&]{return fixture.entered.load();}));
    const auto cache=root.getChildFile("store/cache"),outside=root.getChildFile("outside");CHECK(outside.createDirectory().wasOk());DWORD mutationError=0;CHECK(!junction(cache,outside,&mutationError));CHECK(mutationError==ERROR_DIR_NOT_EMPTY);CHECK(!MoveFileExW(cache.getFullPathName().toWideCharPointer(),root.getChildFile("moved-cache").getFullPathName().toWideCharPointer(),0));const auto moveError=GetLastError();CHECK(moveError==ERROR_SHARING_VIOLATION);std::cout<<"held_cache_reparse_error="<<mutationError<<" namespace_move_error="<<moveError<<'\n';client.cancel(job);CHECK(waitForTerminal(client,job));
    std::atomic<bool> started{false},cancelled{false};auto state=client.createSkinPreparation([&](const auto&,const auto*,const auto& cancel){started=true;while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));cancelled=true;return std::shared_ptr<const studio::MakerSkin>{};});RackInstance preview;preview.design.name="preview-only";client.requestSkin(state,preview);CHECK(waitUntil([&]{return started.load();}));client.prepareSkins(state,{},{});CHECK(waitUntil([&]{return cancelled.load();}));
}
void cacheBootstrap(const juce::File& root){
    for(const int stage:{0,1,2}){const bool guard=stage!=0;
        NetworkFixture fixture(root.getChildFile(stage==2?"before-images-guard":guard?"before-guard":"before-child"));const auto outside=fixture.root.getChildFile("outside"),storeRoot=fixture.root.getChildFile("store"),mutated=stage==2?storeRoot.getChildFile("cache/images"):guard?storeRoot.getChildFile("cache"):storeRoot;CHECK(outside.createDirectory().wasOk());std::atomic<bool> injected{false},created{false};DWORD setupError=0;
        setRackLibraryCacheTestHook([&](const juce::File& parent,const juce::String& name){if(parent==mutated&&name==(guard?".air-cache-guard":"cache")&&!injected.exchange(true))created=junction(parent,outside,&setupError);});
        auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{storeRoot});RackLibraryClient client(store,fixture.transport());const auto job=client.refresh();CHECK(waitForTerminal(client,job));if(stage==2){CHECK(client.view()->status==RackLibraryStatus::ready);client.requestThumbnail(fixture.one);CHECK(waitUntil([&]{return bool(client.thumbnail(fixture.one));}));}setRackLibraryCacheTestHook({});CHECK(injected&&created);CHECK(setupError==0);CHECK((stage==2?client.thumbnail(fixture.one)->status:client.view()->status)==RackLibraryStatus::storageFailure);CHECK(stage==2?fixture.imageGets==0:fixture.catalogGets==0);CHECK(outside.findChildFiles(juce::File::findFilesAndDirectories,false).isEmpty());if(created)CHECK(clearJunction(mutated));std::cout<<"bootstrap_stage="<<stage<<" actual_mutation="<<created<<" error="<<setupError<<" outside_entries="<<outside.findChildFiles(juce::File::findFilesAndDirectories,false).size()<<'\n';
    }
    NetworkFixture fixture(root.getChildFile("held-images"));const auto storeRoot=fixture.root.getChildFile("store"),images=storeRoot.getChildFile("cache/images"),outside=fixture.root.getChildFile("outside");CHECK(outside.createDirectory().wasOk());const auto transport=fixture.transport();std::atomic<bool> attempted{false};DWORD mutationError=0,moveError=0;bool mutated=false,moved=false;
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{storeRoot});RackLibraryClient client(store,[&](const auto& request,const auto& sink,const auto& cancel,auto& response){if(request.url.endsWith("screenshot.png")){mutated=junction(images,outside,&mutationError);moved=MoveFileExW(images.getFullPathName().toWideCharPointer(),storeRoot.getChildFile("moved-images").getFullPathName().toWideCharPointer(),0)!=0;moveError=GetLastError();attempted=true;}return transport(request,sink,cancel,response);});auto job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::ready);client.requestThumbnail(fixture.one);CHECK(waitUntil([&]{return bool(client.thumbnail(fixture.one));}));CHECK(attempted&&!mutated&&!moved);CHECK(mutationError==ERROR_DIR_NOT_EMPTY&&moveError==ERROR_SHARING_VIOLATION);CHECK(client.thumbnail(fixture.one)->status==RackLibraryStatus::ready);CHECK(outside.findChildFiles(juce::File::findFilesAndDirectories,false).isEmpty());fixture.mode=2;job=client.refresh();CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::ready&&fixture.catalogGets==2);CHECK(storeRoot.getChildFile("cache/catalog.json").existsAsFile());std::cout<<"held_images_reparse_error="<<mutationError<<" namespace_move_error="<<moveError<<" atomic_republication=ready\n";
}
void placementBusyRetry(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    RackInventoryPtr snapshot;CHECK(captureRackInventory(*store,{},snapshot));if(!snapshot)return;const auto generation=store->observedGeneration();
    std::atomic<unsigned> gets{0};RackLibraryClient client(store,[&](const auto&,const auto&,const auto&,auto&){++gets;return RackLibraryStatus::networkFailure;});
    struct WriterLock {HANDLE handle=INVALID_HANDLE_VALUE;~WriterLock(){release();}void release(){if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}} writer;
    writer.handle=CreateFileW(root.getChildFile("store/writer.lock").getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    OVERLAPPED offset{};const bool held=writer.handle!=INVALID_HANDLE_VALUE&&LockFileEx(writer.handle,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&offset);CHECK(held);if(!held)return;
    const auto state=client.createPlacementPreparation();client.requestPlacement(state,snapshot,std::span(&fixture.one,1));
    const bool terminalBusy=waitUntil([&]{const auto view=client.placement(state);return view->status==RackLibraryStatus::busy&&view->storeGeneration==generation;});CHECK(terminalBusy);if(!terminalBusy)return;
    const auto busy=client.placement(state);CHECK(busy->root==snapshot&&busy->leases.empty());writer.release();
    CHECK(store->generation()==generation&&store->observedGeneration()==generation);client.requestPlacement(state,snapshot,std::span(&fixture.one,1));
    const bool recovered=waitUntil([&]{const auto view=client.placement(state);return client.placementCurrent(state,*view);});CHECK(recovered);
    if(recovered){const auto ready=client.placement(state);CHECK(ready->root==snapshot&&ready->revision>busy->revision&&ready->storeGeneration==generation&&ready->leases.size()==1&&ready->leases.front()->manifest().ref==fixture.one);}
    CHECK(gets==0&&client.view()->job==0);std::cout<<"PLACEMENT_BUSY_RETRY same_root_ref_generation="<<generation<<" terminal_busy="<<terminalBusy<<" recovered="<<recovered<<" gets="<<gets<<" jobs="<<client.view()->job<<'\n';
}
struct StoreAcquisitionHold {
    std::mutex mutex;std::condition_variable changed;bool requested=false,released=false,stop=false;
    std::atomic<bool> holding{false};std::thread owner;
    explicit StoreAcquisitionHold(const std::shared_ptr<RackPackageStore>& store):owner([&,store]{
        {std::unique_lock lock(mutex);changed.wait(lock,[&]{return requested||stop;});if(stop)return;}
        unsigned checkpoints=0;store->generation(nullptr,[&]{if(++checkpoints==2){std::unique_lock lock(mutex);holding=true;changed.notify_all();changed.wait_for(lock,std::chrono::seconds(5),[&]{return released;});holding=false;}return false;});
    }){}
    bool acquire(){std::unique_lock lock(mutex);requested=true;changed.notify_all();return changed.wait_for(lock,std::chrono::seconds(5),[&]{return holding.load();});}
    void release(){const std::lock_guard lock(mutex);released=true;changed.notify_all();}
    ~StoreAcquisitionHold(){{const std::lock_guard lock(mutex);stop=true;released=true;changed.notify_all();}owner.join();}
};
struct InventoryAcquisitionWitness {
    struct Completion {std::uint64_t revision;unsigned checkpoints;bool cancelled,published;};
    StoreAcquisitionHold& hold;std::atomic<bool> entered{false},ownerAcquired{false};std::atomic<unsigned> enteredCheckpoint{0};std::atomic<std::uint64_t> acquisitionRevision{0};
    std::mutex mutex;std::vector<Completion> completions;
    explicit InventoryAcquisitionWitness(StoreAcquisitionHold& owner):hold(owner){
        setRackLibraryInventoryTestHook([this](std::uint64_t revision,unsigned checkpoint,bool cancelled,bool finished,bool published){
            std::uint64_t unset=0;if(!finished&&checkpoint==1&&acquisitionRevision.compare_exchange_strong(unset,revision))ownerAcquired=hold.acquire();
            // Capture's initial cancel is1;2 is Store's first wait check;3 follows a real timed mutex retry.
            if(!finished&&revision==acquisitionRevision&&checkpoint>=3&&hold.holding){enteredCheckpoint=checkpoint;entered=true;}
            if(finished){const std::lock_guard lock(mutex);completions.push_back({revision,checkpoint,cancelled,published});}
        });
    }
    bool completed(std::uint64_t revision,bool cancelled,bool published){const std::lock_guard lock(mutex);return std::any_of(completions.begin(),completions.end(),[&](const auto& event){return event.revision==revision&&event.cancelled==cancelled&&event.published==published;});}
    unsigned publications(){const std::lock_guard lock(mutex);return unsigned(std::count_if(completions.begin(),completions.end(),[](const auto& event){return event.published;}));}
    ~InventoryAcquisitionWitness(){setRackLibraryInventoryTestHook({});}
};
void thumbnailActiveRetirement(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    StoreAcquisitionHold hold(store);std::atomic<bool> entered{false},ownerAcquired{false},finished{false},cancelled{false},published{false};std::atomic<unsigned> enteredCheckpoint{0},gets{0};
    setRackLibraryThumbnailAcquisitionTestHook([&](unsigned checkpoint,bool stopped,bool done,bool publication){
        if(!done&&checkpoint==1)ownerAcquired=hold.acquire();
        // Store::open performs its first check before acquireStore, then checks each real mutex retry.
        if(!done&&checkpoint>=3&&hold.holding){enteredCheckpoint=checkpoint;entered=true;}
        if(done){cancelled=stopped;published=publication;finished=true;}
    });
    auto client=std::make_unique<RackLibraryClient>(store,[&](const auto&,const auto&,const auto&,auto&){++gets;return RackLibraryStatus::networkFailure;});
    client->requestThumbnail(fixture.one);const bool acquiring=waitUntil([&]{return entered.load();});CHECK(acquiring&&ownerAcquired&&hold.holding);
    if(acquiring){const auto checkpoint=enteredCheckpoint.load();client.reset();CHECK(hold.holding&&finished&&cancelled&&!published);CHECK(gets==0);
        std::cout<<"THUMBNAIL_ACTIVE_RETIREMENT entered_checkpoint="<<checkpoint<<" held_after_join="<<hold.holding<<" finished="<<finished<<" cancelled="<<cancelled<<" published="<<published<<" gets="<<gets<<'\n';}
    hold.release();client.reset();setRackLibraryThumbnailAcquisitionTestHook({});
}
void inventoryReplacement(const juce::File& root,bool replaceRef){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    RackInstance first;first.design=factoryRackDesigns().front();first.design.id=juce::Uuid();first.design.name="Acquiring canonical A";
    if(replaceRef){const auto bytes=test::packageFixtureDesign();CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),first.design,RackValidationMode::instanceRecall));
        CHECK(store->install(fixture.zipTwo,fixture.two,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
        CHECK(store->remove(fixture.one,{})==RackLibraryStatus::ready);CHECK(store->remove(fixture.two,{})==RackLibraryStatus::ready);first.package=fixture.one;}
    auto latest=first;if(replaceRef)latest.package=fixture.two;else{++latest.design.revision;latest.design.name="Replacement canonical B";}
    juce::MemoryBlock firstBytes,latestBytes;CHECK(encodeRackDesign(first.design,firstBytes,RackValidationMode::instanceRecall));CHECK(encodeRackDesign(latest.design,latestBytes,RackValidationMode::instanceRecall));
    CHECK(replaceRef?(firstBytes==latestBytes&&first.package!=latest.package):(firstBytes!=latestBytes&&first.package==latest.package));
    StoreAcquisitionHold hold(store);InventoryAcquisitionWitness witness(hold);std::atomic<unsigned> gets{0};
    RackLibraryClient client(store,[&](const auto&,const auto&,const auto&,auto&){++gets;return RackLibraryStatus::networkFailure;});
    const auto state=client.createInventoryPreparation();client.requestInventory(state,std::span(&first,1));
    const bool acquiring=waitUntil([&]{return witness.entered.load();});CHECK(acquiring&&witness.ownerAcquired&&hold.holding);if(!acquiring)return;
    const auto old=client.inventory(state);const auto oldRevision=witness.acquisitionRevision.load();for(unsigned n=0;n<96;++n)client.requestInventory(state,std::span(&first,1));CHECK(client.inventory(state)==old&&old->revision==oldRevision&&old->busy);
    client.requestInventory(state,std::span(&latest,1));const auto replacement=client.inventory(state);CHECK(replacement->revision==oldRevision+1&&replacement->busy&&replacement->snapshot==old->snapshot);
    CHECK(waitUntil([&]{return witness.completed(oldRevision,true,false);}));CHECK(hold.holding&&witness.publications()==0&&client.inventory(state)->revision==replacement->revision);
    hold.release();CHECK(waitUntil([&]{return client.inventory(state)->status==RackLibraryStatus::ready&&witness.completed(replacement->revision,false,true);}));
    const auto ready=client.inventory(state);CHECK(ready->revision==replacement->revision&&ready->snapshot&&witness.publications()==1);if(!ready->snapshot)return;
    RackDesign resolved;std::optional<RackPackageRef> package;CHECK(resolveInventoryDesign(*ready->snapshot,latest.design.id,latest.design.revision,juce::SHA256(latestBytes).toHexString(),resolved,package));CHECK(resolved==latest.design&&package==latest.package);
    if(!replaceRef)CHECK(!resolveInventoryDesign(*ready->snapshot,first.design.id,first.design.revision,juce::SHA256(firstBytes).toHexString(),resolved,package));
    CHECK(gets==0&&client.view()->job==0);std::cout<<"INVENTORY_REPLACE kind="<<(replaceRef?"exact_ref":"canonical_design")<<" entered_checkpoint="<<witness.enteredCheckpoint<<" old_cancelled=1 old_published=0 latest_revision="<<ready->revision<<" publications="<<witness.publications()<<" gets="<<gets<<" jobs="<<client.view()->job<<'\n';
}
void inventoryActiveRetirement(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    StoreAcquisitionHold hold(store);InventoryAcquisitionWitness witness(hold);std::atomic<unsigned> gets{0};
    auto client=std::make_unique<RackLibraryClient>(store,[&](const auto&,const auto&,const auto&,auto&){++gets;return RackLibraryStatus::networkFailure;});
    const auto state=client->createInventoryPreparation();client->requestInventory(state,{});
    const bool acquiring=waitUntil([&]{return witness.entered.load();});CHECK(acquiring&&witness.ownerAcquired&&hold.holding);if(!acquiring)return;
    const auto entered=witness.enteredCheckpoint.load();const auto revision=witness.acquisitionRevision.load();client.reset();CHECK(hold.holding&&witness.completed(revision,true,false)&&witness.publications()==0);CHECK(gets==0);
    std::cout<<"INVENTORY_ACTIVE_RETIREMENT entered_checkpoint="<<entered<<" held_after_join="<<hold.holding<<" cancelled=1 published="<<witness.publications()<<" gets="<<gets<<'\n';hold.release();
}
void inventoryCancellation(const juce::File& root){inventoryReplacement(root.getChildFile("design"),false);inventoryReplacement(root.getChildFile("ref"),true);inventoryActiveRetirement(root.getChildFile("retirement"));}
void inventoryPreparation(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    std::atomic<unsigned> gets{0};RackLibraryClient client(store,[&](const auto&,const auto&,const auto&,auto&){++gets;return RackLibraryStatus::networkFailure;});
    const auto state=client.createInventoryPreparation();CHECK(client.inventory(state)->status==RackLibraryStatus::unavailable);client.requestInventory(state,{});
    CHECK(waitUntil([&]{return client.inventory(state)->status==RackLibraryStatus::ready;}));const auto good=client.inventory(state);CHECK(good->snapshot&&good->snapshot->rows().size()==18);
    for(unsigned n=0;n<96;++n)client.requestInventory(state,{});CHECK(client.inventory(state)==good);
    const auto index=root.getChildFile("store/installed.json");const auto saved=index.loadFileAsString();CHECK(index.replaceWithText("broken"));
    RackInstance embedded;embedded.design=factoryRackDesigns().front();embedded.design.id=juce::Uuid();embedded.design.name="Embedded change";
    client.requestInventory(state,std::span(&embedded,1));CHECK(waitUntil([&]{const auto view=client.inventory(state);return !view->busy;}));auto failed=client.inventory(state);
    CHECK(failed->status==RackLibraryStatus::unavailable&&failed->snapshot==good->snapshot);client.requestInventory(state,std::span(&embedded,1));CHECK(client.inventory(state)==failed);
    CHECK(index.replaceWithText(saved));client.retryInventory(state);CHECK(waitUntil([&]{const auto view=client.inventory(state);return !view->busy&&view->status==RackLibraryStatus::ready;}));
    const auto captured=client.inventory(state);CHECK(captured->snapshot&&captured->snapshot->rows().size()==19);
    auto moved=embedded;moved.id=juce::Uuid();moved.bindings={{0,3}};client.requestInventory(state,std::span(&moved,1));CHECK(client.inventory(state)==captured);
    const auto placement=client.createPlacementPreparation();client.requestPlacement(placement,captured->snapshot,std::span(&fixture.one,1));
    CHECK(waitUntil([&]{return client.placement(placement)->status==RackLibraryStatus::ready;}));auto token=client.placement(placement);CHECK(token->leases.size()==1&&client.placementCurrent(placement,*token));
    CHECK(store->remove(fixture.one,{})==RackLibraryStatus::busy);
    CHECK(store->install(fixture.zipTwo,fixture.two,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);CHECK(!client.placementCurrent(placement,*token));
    client.requestPlacement(placement,captured->snapshot,std::span(&fixture.one,1));CHECK(waitUntil([&]{const auto view=client.placement(placement);return client.placementCurrent(placement,*view);}));
    token.reset();client.cancelPlacement(placement);CHECK(waitUntil([&]{return store->remove(fixture.one,{})==RackLibraryStatus::ready;}));
    client.requestPlacement(placement,captured->snapshot,std::span(&fixture.one,1));CHECK(waitUntil([&]{return client.placement(placement)->status==RackLibraryStatus::unavailable;}));
    client.cancelInventory(state);CHECK(client.inventory(state)->status==RackLibraryStatus::unavailable);CHECK(gets==0&&client.view()->job==0);
    std::atomic<bool> holding{false},release{false};std::thread owner([&]{unsigned points=0;store->generation(nullptr,[&]{if(++points==2){holding=true;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!release&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();holding=false;}return false;});});
    CHECK(waitUntil([&]{return holding.load();}));
    {auto retiring=std::make_unique<RackLibraryClient>(store,[&](const auto&,const auto&,const auto&,auto&){++gets;return RackLibraryStatus::networkFailure;});auto input=retiring->createInventoryPreparation();retiring->requestInventory(input,{});retiring.reset();CHECK(holding);}
    release=true;owner.join();CHECK(gets==0);
}
void installedScreenshotWithoutCatalog(const juce::File& root){
    NetworkFixture fixture(root);
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    std::atomic<unsigned> requests{0};
    RackLibraryClient client(store,[&](const auto&,const auto&,const auto&,auto&){++requests;return RackLibraryStatus::networkFailure;});
    CHECK(client.view()->releases.empty());client.requestThumbnail(fixture.one);
    CHECK(waitUntil([&]{return bool(client.thumbnail(fixture.one));}));
    const auto thumbnail=client.thumbnail(fixture.one);
    CHECK(thumbnail&&thumbnail->status==RackLibraryStatus::ready&&thumbnail->image.isValid());
    CHECK(requests==0);
}
void builtInScreenshots(const juce::File& root){
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});RackInventoryPtr inventory;
    CHECK(captureRackInventory(*store,{},inventory));if(!inventory)return;
    std::atomic<unsigned> requests{0};RackLibraryClient client(store,[&](const auto&,const auto&,const auto&,auto&){++requests;return RackLibraryStatus::networkFailure;});
    CHECK(inventory->rows().size()==17);
    for(const auto& row:inventory->rows()){
        CHECK(!client.builtInThumbnail(row.metadata));client.requestBuiltInThumbnail(row.metadata);
        CHECK(waitUntil([&]{return bool(client.builtInThumbnail(row.metadata));}));
        const auto thumbnail=client.builtInThumbnail(row.metadata);CHECK(thumbnail&&thumbnail->status==RackLibraryStatus::ready&&thumbnail->image.getWidth()==512);
    }
    CHECK(!client.builtInThumbnail(inventory->rows().front().metadata)); // Shared cache retains at most sixteen entries.
    auto stale=inventory->rows().back().metadata;++stale.revision;client.requestBuiltInThumbnail(stale);
    CHECK(waitUntil([&]{return bool(client.builtInThumbnail(stale));}));CHECK(client.builtInThumbnail(stale)->status!=RackLibraryStatus::ready);
    stale=inventory->rows().back().metadata;stale.canonicalSha256=juce::String::repeatedString("0",64);client.requestBuiltInThumbnail(stale);
    CHECK(waitUntil([&]{return bool(client.builtInThumbnail(stale));}));CHECK(client.builtInThumbnail(stale)->status!=RackLibraryStatus::ready);
    CHECK(requests==0);
}
void cancelledPlacementRemoval(const juce::File& root){
    NetworkFixture fixture(root);auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root.getChildFile("store")});
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    RackInventoryPtr inventory;CHECK(captureRackInventory(*store,{},inventory));RackLibraryClient client(store,fixture.transport());
    auto placement=client.createPlacementPreparation();client.requestPlacement(placement,inventory,std::span<const RackPackageRef>(&fixture.one,1));
    CHECK(waitUntil([&]{return client.placement(placement)->status==RackLibraryStatus::ready;}));
    std::mutex mutex;std::condition_variable changed;bool entered=false,release=false;
    auto skin=client.createSkinPreparation([&](const auto&,const auto*,const auto&){std::unique_lock lock(mutex);entered=true;changed.notify_all();changed.wait(lock,[&]{return release;});return std::shared_ptr<const studio::MakerSkin>{};});
    RackInstance request;request.design=factoryRackDesigns().front();client.requestSkin(skin,request);
    {std::unique_lock lock(mutex);CHECK(changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
    client.cancelPlacement(placement);auto job=client.remove(fixture.one);
    {const std::lock_guard lock(mutex);release=true;}changed.notify_all();
    CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::ready);CHECK(store->active().empty());
    CHECK(store->install(fixture.zipOne,fixture.one,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    auto live=store->open(fixture.one);CHECK(bool(live));job=client.remove(fixture.one);CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::busy);CHECK(store->active()==std::vector<RackPackageRef>{fixture.one});
}
void folderRevealIsolation(const juce::File& root){
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});std::atomic<unsigned> gets{0};std::atomic<bool> block{false},entered{false};
    RackLibraryClient client(store,[&](const auto&,const auto&,const auto& cancel,auto&){++gets;entered=true;while(block&&!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(2));return cancel()?RackLibraryStatus::cancelled:RackLibraryStatus::networkFailure;});
    auto job=client.revealLibraryFolder();CHECK(job!=0);if(job)CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::unavailable);CHECK(gets==0);CHECK(!root.exists());
    block=true;job=client.refresh();CHECK(waitUntil([&]{return entered.load();}));const auto view=client.view();CHECK(client.revealLibraryFolder()==0);CHECK(client.view()==view&&client.view()->job==job&&client.view()->busy);block=false;CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::networkFailure);CHECK(gets==1);
}
// Optional operator mode; default tests use only injected local transports.
int publicServiceProbe(const juce::String& receiptPath,RackLibraryTransport transport){
    if(!juce::File::isAbsolutePath(receiptPath))return 2;
    const juce::File receipt(receiptPath);if(!receipt.getParentDirectory().isDirectory())return 2;
    struct Receipt {HANDLE handle=INVALID_HANDLE_VALUE;~Receipt(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} output;
    output.handle=CreateFileW(receipt.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(output.handle==INVALID_HANDLE_VALUE)return 2;
    const auto base=juce::File::getSpecialLocation(juce::File::tempDirectory);const auto root=base.getChildFile("airack-public-service-"+juce::Uuid().toString());
    const auto executable=juce::File::getSpecialLocation(juce::File::currentExecutableFile);juce::FileInputStream image(executable);
    auto doc=object({{"phase","not_started"},{"sourceEntry","RackLibraryClientTests.cpp --public-service"},{"executableSha256",image.openedOk()?juce::SHA256(image).toHexString():juce::String()},{"catalogUrl",rackLibraryCatalogUrl},{"countsUrl",rackLibraryCountsUrl},{"temporaryStore",root.getFullPathName()},{"countsComplete",false}});
    const auto write=[&]{const auto text=juce::JSON::toString(doc,true);LARGE_INTEGER zero{};DWORD written=0;return SetFilePointerEx(output.handle,zero,nullptr,FILE_BEGIN)&&WriteFile(output.handle,text.toRawUTF8(),DWORD(text.getNumBytesAsUTF8()),&written,nullptr)&&written==text.getNumBytesAsUTF8()&&SetEndOfFile(output.handle)&&FlushFileBuffers(output.handle);};
    const auto fail=[&](const char* phase,RackLibraryStatus status){doc.getDynamicObject()->setProperty("phase","failed");doc.getDynamicObject()->setProperty("failedAt",phase);doc.getDynamicObject()->setProperty("status",int(status));write();return 1;};
    if(!write()||root.exists()||root.getParentDirectory()!=base||!CreateDirectoryW(root.getFullPathName().toWideCharPointer(),nullptr))return 2;
    {
        auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});RackLibraryClient client(store,std::move(transport));
        const auto done=[&](std::uint64_t job){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(600);while(std::chrono::steady_clock::now()<end){const auto view=client.view();if(view->job==job&&!view->busy)return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}client.cancel(job);return false;};
        doc.getDynamicObject()->setProperty("phase","catalog_and_counts");if(!write())return 2;
        if(!done(client.refresh()))return fail("catalog_and_counts",RackLibraryStatus::timeout);
        const auto view=client.view();juce::Array<juce::var> counts,refs;
        for(const auto& count:view->counts)counts.add(object({{"id",count.id},{"value",count.value?juce::var(juce::String(*count.value)):juce::var()}}));
        for(const auto& release:view->releases)refs.add(object({{"id",release.ref.id},{"version",release.ref.version},{"sha256",release.ref.sha256},{"archiveAssetId",juce::String(release.archiveAssetId)}}));
        doc.getDynamicObject()->setProperty("counts",counts);doc.getDynamicObject()->setProperty("releases",refs);
        const bool complete=view->status==RackLibraryStatus::ready&&!view->countsStale&&!view->counts.empty()&&std::all_of(view->counts.begin(),view->counts.end(),[](const auto& count){return count.value.has_value();});
        doc.getDynamicObject()->setProperty("countsComplete",complete);if(!complete)return fail("catalog_and_counts",view->status);
        const auto selected=std::find_if(view->releases.begin(),view->releases.end(),[](const auto& r){return r.compatible;});if(selected==view->releases.end())return fail("select_compatible_release",RackLibraryStatus::incompatible);
        const auto ref=selected->ref;doc.getDynamicObject()->setProperty("installed",object({{"id",ref.id},{"version",ref.version},{"sha256",ref.sha256}}));doc.getDynamicObject()->setProperty("phase","install_exact_archive");if(!write())return 2;
        if(!done(client.install(ref)))return fail("install_exact_archive",RackLibraryStatus::timeout);if(client.view()->status!=RackLibraryStatus::ready)return fail("install_exact_archive",client.view()->status);
        auto lease=store->open(ref);RackInventoryPtr inventory;if(!lease||lease->manifest().ref!=ref||store->active()!=std::vector<RackPackageRef>{ref}||!captureRackInventory(*store,{},inventory)||!inventory||std::none_of(inventory->rows().begin(),inventory->rows().end(),[&](const auto& row){return row.installed&&row.package==ref;}))return fail("verify_native_store",RackLibraryStatus::invalidData);
    }
    if(root.getParentDirectory()!=base||!root.deleteRecursively())return fail("cleanup_owned_temporary_store",RackLibraryStatus::storageFailure);
    doc.getDynamicObject()->setProperty("phase","complete");return write()?0:2;
}
void publicServiceProbeFixture(const juce::File& root){
    NetworkFixture f(root);const auto receipt=root.getChildFile("service-receipt.json");const auto before=f.catalogGets.load();
    CHECK(publicServiceProbe("relative-receipt.json",f.transport())==2);CHECK(f.catalogGets==before);
    const auto existing=root.getChildFile("existing.json");CHECK(existing.replaceWithText("keep"));CHECK(publicServiceProbe(existing.getFullPathName(),f.transport())==2);CHECK(existing.loadFileAsString()=="keep"&&f.catalogGets==before);
    const auto result=publicServiceProbe(receipt.getFullPathName(),f.transport());CHECK(result==0);if(result!=0)return;const auto doc=juce::JSON::parse(receipt);CHECK(doc["phase"].toString()=="complete"&&bool(doc["countsComplete"]));CHECK(doc["installed"]["sha256"].toString()==f.two.sha256);CHECK(!juce::File(doc["temporaryStore"].toString()).exists());f.mode=7;const auto failed=root.getChildFile("service-failed.json");CHECK(publicServiceProbe(failed.getFullPathName(),f.transport())==1);const auto partial=juce::JSON::parse(failed);CHECK(partial["phase"].toString()=="failed"&&!bool(partial["countsComplete"])&&partial["failedAt"].toString()=="catalog_and_counts");CHECK(f.archiveGets==1);
}
int main(int argc,char** argv){
    if(argc==3&&juce::String(argv[1])=="--public-service")return publicServiceProbe(argv[2],rackLibraryHttpWindows());
    if(argc==4&&juce::String(argv[1])=="--refresh-peer"){std::atomic<int> requests{0};auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{juce::File(argv[2])});RackLibraryClient client(store,[&](const auto&,const auto&,const auto&,auto&){++requests;return RackLibraryStatus::networkFailure;});const auto job=client.refresh();CHECK(juce::File(argv[3]).replaceWithText("ready"));CHECK(waitForTerminal(client,job));CHECK(client.view()->status==RackLibraryStatus::ready&&client.view()->cached);CHECK(requests==0);std::cout<<"peer_cache_requests="<<requests<<" checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc!=2&&argc!=3)return 2;const auto root=juce::File(argv[1]).getChildFile(juce::Uuid().toString());CHECK(root.createDirectory().wasOk());
    if(argc==3&&juce::String(argv[2])=="--folder-reveal"){folderRevealIsolation(root.getChildFile("folder"));std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[2])=="--thumbnail-retirement"){thumbnailActiveRetirement(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[2])=="--cancelled-placement-removal"){cancelledPlacementRemoval(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[2])=="--placement-retry"){placementBusyRetry(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[2])=="--inventory-cancellation"){inventoryCancellation(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[2])=="--inventory-preparation"){inventoryPreparation(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&juce::String(argv[2])=="--service-probe-fixture"){publicServiceProbeFixture(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    publicServiceProbeFixture(root.getChildFile("service-probe"));
    thumbnailActiveRetirement(root.getChildFile("thumbnail-retirement"));placementBusyRetry(root.getChildFile("placement-retry"));inventoryCancellation(root.getChildFile("inventory-cancellation"));inventoryPreparation(root.getChildFile("inventory"));
    auto m=test::packageFixture();const auto metadata=juce::JSON::parse(test::packageFixtureJson(m))["metadata"];
    auto release=object({{"ref",object({{"id",m.ref.id},{"version","1.0.0"},{"sha256",juce::String::repeatedString("a",64)}})},
        {"metadata",metadata},{"capabilities",m.capabilities},{"archive",object({{"assetId",101},{"url","https://github.com/Planetary-Records/air-rack-library/releases/download/v1/ember.zip"},{"bytes",1024},{"sha256",juce::String::repeatedString("a",64)}})},
        {"screenshot",object({{"url","https://raw.githubusercontent.com/Planetary-Records/air-rack-library/main/ember.png"},{"bytes",70},{"sha256",juce::String::repeatedString("b",64)}})}});
    auto second=release.clone();second["ref"].getDynamicObject()->setProperty("version","2.0.0");second["archive"].getDynamicObject()->setProperty("assetId",102);
    const auto catalog=juce::JSON::toString(object({{"schema",1},{"tags",juce::Array<juce::var>{object({{"id","analog"},{"label","Analog"}}),object({{"id","warm"},{"label","Warm"}})}},{"racks",juce::Array<juce::var>{object({{"id",m.ref.id},{"order",0},{"releases",juce::Array<juce::var>{release,second}}})}}}),true);
    auto store=std::make_shared<RackPackageStore>(RackLibraryPaths{root});
    const auto committed=juce::File(RACKSYNTH_CLIENT_FIXTURE_DIR).getChildFile("catalog.json").loadFileAsString();CHECK(committed.isNotEmpty());
    RackLibraryClient client(store,[&](const RackHttpRequest& request,const RackHttpSink& sink,const RackLibraryCancel&,RackHttpResponse& response){response.code=200;response.etag="fixture";const auto body=request.url==rackLibraryCatalogUrl?committed:juce::File(RACKSYNTH_CLIENT_FIXTURE_DIR).getChildFile("counts.json").loadFileAsString();return sink(body.toRawUTF8(),body.getNumBytesAsUTF8())?RackLibraryStatus::ready:RackLibraryStatus::byteLimit;});
    CHECK(client.view()->status==RackLibraryStatus::unavailable);const auto job=client.refresh();CHECK(waitForTerminal(client,job));
    CHECK(client.view()->releases.size()==2);CHECK(store->active().empty());CHECK(client.view()->status==RackLibraryStatus::ready);
    CHECK(client.view()->counts.size()==1);if(!client.view()->counts.empty())CHECK(client.view()->counts.front().value==std::optional<std::uint64_t>{47});
    if(client.view()->status!=RackLibraryStatus::ready||client.view()->releases.size()!=2||client.view()->counts.size()!=1){std::cerr<<"initial_cache_status="<<int(client.view()->status)<<" message="<<client.view()->message<<'\n';std::cout<<checks<<" checks, "<<failures<<" failures\n";return 1;}
    installedScreenshotWithoutCatalog(root.getChildFile("local-preview"));builtInScreenshots(root.getChildFile("builtin-preview"));cancelledPlacementRemoval(root.getChildFile("cancelled-placement-removal"));folderRevealIsolation(root.getChildFile("folder"));
    changedCatalogCounts(root.getChildFile("final-counts"));
    extended(root.getChildFile("extended"));
    validationAndCache(root.getChildFile("validation"));
    hostileCache(root.getChildFile("hostile"));
    thumbnailBounds(root.getChildFile("bounds"));
    processReuse(root.getChildFile("process"));
    fallbackRate(root.getChildFile("fallback-rate"));
    successfulQuota(root.getChildFile("successful-quota"));
    redirectQuota(root.getChildFile("redirect-quota"));
    cancellationAndRootPin(root.getChildFile("cancel-and-pin"));
    cacheBootstrap(root.getChildFile("bootstrap"));
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
