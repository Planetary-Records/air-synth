#include "RackPackageStore.h"
#include "RackPackageFixtures.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>
#include <map>
#include <atomic>
#include <thread>
#include <windows.h>
#include <winioctl.h>

using namespace airack;
namespace {
int checks=0, failures=0;
void check(bool ok,const char* text,int line) { ++checks;if(!ok){++failures;std::cerr<<line<<": "<<text<<'\n';} }
#define CHECK(x) check(bool(x),#x,__LINE__)
const RackLibraryCancel never=[] { return false; };
struct Fixture {
    RackPackageManifest manifest=test::packageFixture();
    std::map<juce::String,juce::MemoryBlock> bytes{{"rack.json",test::packageFixtureDesign()},{"screenshot.png",test::packageFixturePng()}};
    void add(const juce::String& path,const juce::String& kind,const juce::MemoryBlock& data) {
        bytes[path]=data;
        auto it=std::find_if(manifest.files.begin(),manifest.files.end(),[&](const auto& f){return f.path==path;});
        RackPackageFile f{path,juce::SHA256(data).toHexString(),kind,data.getSize()};
        if(it==manifest.files.end())manifest.files.push_back(f);else *it=f;
    }
    juce::File write(const juce::File& parent,const juce::String& name="fixture.zip",const juce::String& extra={}) const {
        CHECK(parent.createDirectory().wasOk());auto file=parent.getChildFile(name);juce::ZipFile::Builder zip;
        const auto json=test::packageFixtureJson(manifest);const juce::Time stamp(1980,0,1,0,0);
        zip.addEntry(new juce::MemoryInputStream(json.toRawUTF8(),json.getNumBytesAsUTF8(),true),0,"package.json",stamp);
        for(const auto& [path,data]:bytes)zip.addEntry(new juce::MemoryInputStream(data,true),0,path,stamp);
        if(extra.isNotEmpty())zip.addEntry(new juce::MemoryInputStream("x",1,true),0,extra,stamp);
        auto out=file.createOutputStream();CHECK(out!=nullptr);if(out)CHECK(zip.writeToStream(*out,nullptr));return file;
    }
    RackPackageRef ref(const juce::File& zip) const { auto r=manifest.ref;juce::FileInputStream stream(zip);r.sha256=juce::SHA256(stream).toHexString();return r; }
};
juce::MemoryBlock textBytes(const juce::String& s) { return {s.toRawUTF8(),s.getNumBytesAsUTF8()}; }
juce::String skinJson(const Fixture& f) {
    return "{\"rack_pitch\":{\"panel_w\":100,\"u_px\":10},\"design_id\":\""+f.manifest.metadata.design.toDashedString()
        +"\",\"maker\":\"Fixture Maker\",\"model\":\"Fixture\",\"units\":1,\"assets\":{\"face:plate\":{\"file\":\"screenshot.png\"},\"setup:plate\":{\"file\":\"screenshot.png\"}},"
        "\"reveal\":{\"kind\":\"recess\",\"view\":\"setup\",\"opener\":\"face.open\",\"closers\":[\"setup.close\"]},"
        "\"elements\":[{\"id\":\"face.open\",\"view\":\"face\",\"kind\":\"action\",\"geometry\":{\"rect\":[0,0,1,1]}},{\"id\":\"setup.close\",\"view\":\"setup\",\"kind\":\"action\",\"geometry\":{\"rect\":[0,0,1,1]}}],\"groups\":[]}";
}
juce::File release(const juce::File& root,const RackPackageRef& ref) { return root.getChildFile("packages").getChildFile(ref.id).getChildFile(ref.version); }
juce::MemoryBlock data(const juce::File& file) { juce::MemoryBlock out;file.loadFileAsData(out);return out; }
void patchCentral(const juce::File& file,int field,std::uint32_t value,const juce::String& entry="rack.json") {
    auto b=data(file);auto* p=static_cast<unsigned char*>(b.getData());bool patched=false;
    for(size_t i=0;i+46<b.getSize();++i)if(juce::ByteOrder::littleEndianInt(p+i)==0x02014b50){
        const auto length=juce::ByteOrder::littleEndianShort(p+i+28);
        if(i+46+length<=b.getSize()&&juce::String::fromUTF8(reinterpret_cast<char*>(p+i+46),length)==entry){
            for(int n=0;n<4;++n)p[i+field+n]=static_cast<unsigned char>(value>>(8*n));patched=true;break;}
    }
    CHECK(patched);CHECK(file.replaceWithData(b.getData(),b.getSize()));
}
std::uint32_t crc(const unsigned char* bytes,size_t count) {
    std::uint32_t result=0xffffffff;for(size_t i=0;i<count;++i){result^=bytes[i];for(int n=0;n<8;++n)result=(result>>1)^(0xedb88320u&(0u-(result&1)));}return ~result;
}
bool junction(const juce::File& link,const juce::File& target) {
    if(link.createDirectory().failed())return false;
    const auto print=std::wstring(target.getFullPathName().replaceCharacter('/','\\').toWideCharPointer()),substitute=L"\\??\\"+print;
    std::vector<unsigned char> bytes(16+(substitute.size()+print.size()+2)*sizeof(wchar_t));
    auto* tag=reinterpret_cast<DWORD*>(bytes.data());*tag=IO_REPARSE_TAG_MOUNT_POINT;
    auto* words=reinterpret_cast<WORD*>(bytes.data()+4);words[0]=WORD(bytes.size()-8);words[1]=0;words[2]=0;words[3]=WORD(substitute.size()*2);words[4]=WORD((substitute.size()+1)*2);words[5]=WORD(print.size()*2);
    std::memcpy(bytes.data()+16,substitute.data(),substitute.size()*2);std::memcpy(bytes.data()+16+(substitute.size()+1)*2,print.data(),print.size()*2);
    HANDLE file=CreateFileW(link.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;DWORD returned=0;const bool result=DeviceIoControl(file,FSCTL_SET_REPARSE_POINT,bytes.data(),DWORD(bytes.size()),nullptr,0,&returned,nullptr)!=0;CloseHandle(file);return result;
}
void lifecycle(const juce::File& trial) {
    const auto root=trial.getChildFile("store");RackPackageStore store({root});RackLibraryStatus status=RackLibraryStatus::invalidData;
    CHECK(store.active(&status).empty());CHECK(status==RackLibraryStatus::ready);CHECK(store.generation()==0);
    Fixture f;const auto zip=f.write(trial);const auto v1=f.ref(zip);
    CHECK(store.install(zip,v1,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto oldIndex=data(root.getChildFile("installed.json"));CHECK(!oldIndex.isEmpty());CHECK(store.generation()==1);
    auto pinned=store.open(v1,&status);CHECK(pinned!=nullptr);CHECK(status==RackLibraryStatus::ready);
    if(pinned){CHECK(pinned->manifest().ref==v1);CHECK(pinned->canonicalRecipe()==std::string_view(static_cast<const char*>(f.bytes["rack.json"].getData()),f.bytes["rack.json"].getSize()));
        juce::MemoryBlock output("kept",4);CHECK(pinned->readListed("../outside",output)==RackLibraryStatus::invalidData);CHECK(output==juce::MemoryBlock("kept",4));
        CHECK(pinned->readListed("package.json",output)==RackLibraryStatus::invalidData);CHECK(pinned->readListed("screenshot.png",output)==RackLibraryStatus::ready);CHECK(output==f.bytes["screenshot.png"]);}
    CHECK(store.install(zip,v1,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    CHECK(store.generation()==1);CHECK(data(root.getChildFile("installed.json"))==oldIndex);
    CHECK(store.remove(v1,never)==RackLibraryStatus::busy);CHECK(data(root.getChildFile("installed.json"))==oldIndex);
    auto badHash=v1;badHash.sha256=juce::String::repeatedString("0",64);CHECK(store.open(badHash,&status)==nullptr);CHECK(status==RackLibraryStatus::identityConflict);
    auto different=f;different.manifest.metadata.description="Different release bytes";const auto other=different.write(trial,"different.zip");
    CHECK(store.install(other,different.ref(other),RackPackageActivation::makeActive,never)==RackLibraryStatus::identityConflict);
    CHECK(data(root.getChildFile("installed.json"))==oldIndex);
    f.manifest.ref.version="2.0.0";const auto zip2=f.write(trial,"v2.zip");const auto v2=f.ref(zip2);
    CHECK(store.install(zip2,v2,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);CHECK(store.generation()==2);
    CHECK(store.active()==std::vector<RackPackageRef>{v2});CHECK(release(root,v1).isDirectory());CHECK(store.open(v1)!=nullptr);
    CHECK(store.remove(v2,never)==RackLibraryStatus::ready);CHECK(store.generation()==3);CHECK(store.active().empty());CHECK(release(root,v1).isDirectory());
    f.manifest.ref.version="3.0.0";const auto zip3=f.write(trial,"v3.zip");const auto v3=f.ref(zip3);
    CHECK(store.install(zip3,v3,RackPackageActivation::preserveActive,never)==RackLibraryStatus::ready);CHECK(store.active().empty());CHECK(store.generation()==4);CHECK(store.open(v3)!=nullptr);
    pinned.reset();CHECK(store.remove(v1,never)==RackLibraryStatus::ready);CHECK(!release(root,v1).exists());CHECK(store.remove(v1,never)==RackLibraryStatus::ready);
    CHECK(store.remove(v3,[]{return true;})==RackLibraryStatus::cancelled);CHECK(store.open(v3)!=nullptr);
}
void refusals(const juce::File& trial) {
    RackPackageStore store({trial.getChildFile("store")});Fixture base;const auto good=base.write(trial);const auto ref=base.ref(good);
    CHECK(store.install(good,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto index=trial.getChildFile("store/installed.json");const auto old=data(index),oldRecipe=data(release(trial.getChildFile("store"),ref).getChildFile("rack.json"));
    auto reject=[&](const juce::File& zip,const RackPackageRef& r){CHECK(store.install(zip,r,RackPackageActivation::makeActive,never)!=RackLibraryStatus::ready);CHECK(data(index)==old);CHECK(data(release(trial.getChildFile("store"),ref).getChildFile("rack.json"))==oldRecipe);};
    for(const auto* path:{"../outside.txt","skin/CON.png","face.png:stream","face.png.","face.png ","FACE.png","skin/link.png"}) {
        auto f=base;f.manifest.ref.id="bad-path";f.add("face.png","png",test::packageFixturePng());const auto zip=f.write(trial,juce::Uuid().toString()+".zip",path);reject(zip,f.ref(zip));}
    for(const auto attrs:{0xa1ff0000u,0x61a40000u,0x81a40400u}){auto f=base;f.manifest.ref.id="bad-link";const auto zip=f.write(trial,juce::Uuid().toString()+".zip");patchCentral(zip,38,attrs);reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="huge";const auto zip=f.write(trial,"expanded.zip");patchCentral(zip,24,0xffffffffu);reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="many";for(int i=0;i<255;++i)f.bytes["extra"+juce::String(i)+".txt"]=textBytes("x");const auto zip=f.write(trial,"count.zip");reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="truncated";const auto zip=f.write(trial,"truncated.zip");auto b=data(zip);CHECK(zip.replaceWithData(b.getData(),b.getSize()/2));reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="malformed";const auto zip=trial.getChildFile("malformed.zip");CHECK(zip.replaceWithText("not a zip"));reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="bad-image";auto png=test::packageFixturePng();png.setSize(33,true);f.add("screenshot.png","png",png);const auto zip=f.write(trial,"image.zip");reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="bad-idat";auto png=test::packageFixturePng();auto* p=static_cast<unsigned char*>(png.getData());p[41]=0xff;
        const auto checksum=crc(p+37,17);for(int n=0;n<4;++n)p[54+n]=static_cast<unsigned char>(checksum>>(24-8*n));
        f.add("screenshot.png","png",png);const auto zip=f.write(trial,"bad-idat.zip");reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="image-cap";auto png=test::packageFixturePng();png.setSize(rackPackageLimits::pngBytes+1,true);f.add("screenshot.png","png",png);const auto zip=f.write(trial,"image-cap.zip");reject(zip,f.ref(zip));}
    {auto f=base;f.manifest.ref.id="cancelled";f.add("notice.txt","notice",textBytes(juce::String::repeatedString("x",200000)));const auto zip=f.write(trial,"cancel.zip");int calls=0;
        CHECK(store.install(zip,f.ref(zip),RackPackageActivation::makeActive,[&]{return ++calls==7;})==RackLibraryStatus::cancelled);CHECK(calls>=7);CHECK(data(index)==old);CHECK(!release(trial.getChildFile("store"),f.manifest.ref).exists());}
    {auto wrong=ref;wrong.sha256=juce::String::repeatedString("0",64);reject(good,wrong);}
}
void hostileFilesystem(const juce::File& trial) {
    Fixture f;const auto zip=f.write(trial);const auto ref=f.ref(zip);const auto root=trial.getChildFile("store");RackPackageStore store({root});
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);const auto old=data(root.getChildFile("installed.json"));
    const auto outside=trial.getChildFile("outside"),sentinel=outside.getChildFile("sentinel.txt");CHECK(outside.createDirectory().wasOk());CHECK(sentinel.replaceWithText("outside bytes stay owned"));const auto original=data(sentinel);
    const auto badRoot=trial.getChildFile("junction-store");CHECK(badRoot.createDirectory().wasOk());CHECK(junction(badRoot.getChildFile("staging"),outside));RackPackageStore linked({badRoot});
    CHECK(linked.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::storageFailure);CHECK(data(sentinel)==original);CHECK(!outside.getChildFile("installed.json").exists());
    f.manifest.ref.id="write-failure";auto failing=f.write(trial,"write-failure.zip");bool blocked=false;
    CHECK(store.install(failing,f.ref(failing),RackPackageActivation::makeActive,[&]{
        for(const auto& stage:root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*"))if(!blocked&&stage.getChildFile("package.json").exists()){
            CHECK(stage.getChildFile("screenshot.png").createDirectory().wasOk());blocked=true;}
        return false;})==RackLibraryStatus::storageFailure);
    CHECK(blocked);CHECK(data(root.getChildFile("installed.json"))==old);CHECK(data(sentinel)==original);
    f.manifest.ref.id="path-swap";f.add("nested/notice.txt","notice",textBytes("owned notice"));const auto swapZip=f.write(trial,"path-swap.zip");bool attempted=false;DWORD swapError=0;
    const auto candidate=trial.getChildFile("candidate-junction");CHECK(junction(candidate,outside));
    CHECK(store.install(swapZip,f.ref(swapZip),RackPackageActivation::makeActive,[&]{
        for(const auto& stage:root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*")){
            const auto nested=stage.getChildFile("nested");if(!attempted&&nested.getChildFile("notice.txt").exists()){
                attempted=true;
                HANDLE reparseWriter=CreateFileW(nested.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
                CHECK(reparseWriter==INVALID_HANDLE_VALUE);if(reparseWriter!=INVALID_HANDLE_VALUE)CloseHandle(reparseWriter);
                const auto moved=nested.getSiblingFile("nested-checked");const bool renamed=MoveFileExW(nested.getFullPathName().toWideCharPointer(),moved.getFullPathName().toWideCharPointer(),0)!=0;swapError=GetLastError();CHECK(!renamed);
                if(renamed)CHECK(MoveFileExW(candidate.getFullPathName().toWideCharPointer(),nested.getFullPathName().toWideCharPointer(),0));
            }
        }return false;})==RackLibraryStatus::ready);
    CHECK(attempted);CHECK(swapError==ERROR_SHARING_VIOLATION||swapError==ERROR_ACCESS_DENIED);CHECK(data(sentinel)==original);CHECK(!outside.getChildFile("notice.txt").exists());
    // The same open directory identity remains pinned for a consumer borrowing artwork.
    auto artwork=store.open(ref);CHECK(artwork!=nullptr);CHECK(!MoveFileExW(release(root,ref).getFullPathName().toWideCharPointer(),trial.getChildFile("release-swapped").getFullPathName().toWideCharPointer(),0));
    CHECK(data(sentinel)==original);
}
void receipts(const juce::File& trial) {
    Fixture f;const auto zip=f.write(trial);const auto ref=f.ref(zip);const auto root=trial.getChildFile("store");RackPackageStore store({root});
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);const auto old=data(root.getChildFile("installed.json"));
    const auto receipt=release(root,ref).getChildFile(".archive-sha256");CHECK(receipt.replaceWithText(juce::String::repeatedString("0",64)));CHECK(store.open(ref)==nullptr);
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::identityConflict);CHECK(data(root.getChildFile("installed.json"))==old);
    CHECK(receipt.deleteFile());CHECK(store.open(ref)==nullptr);CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)!=RackLibraryStatus::ready);CHECK(data(root.getChildFile("installed.json"))==old);
    CHECK(receipt.replaceWithText(ref.sha256));auto lease=store.open(ref);CHECK(lease!=nullptr);if(lease){juce::MemoryBlock output("unchanged",9);CHECK(lease->readListed(".archive-sha256",output)==RackLibraryStatus::invalidData);CHECK(output==juce::MemoryBlock("unchanged",9));}lease.reset();
    f.manifest.ref.version="2.0.0";const auto zip2=f.write(trial,"v2.zip");const auto v2=f.ref(zip2);const auto index=root.getChildFile("installed.json");
    HANDLE blocked=CreateFileW(index.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(blocked!=INVALID_HANDLE_VALUE);
    CHECK(store.install(zip2,v2,RackPackageActivation::makeActive,never)==RackLibraryStatus::storageFailure);if(blocked!=INVALID_HANDLE_VALUE)CloseHandle(blocked);
    auto alternate=data(zip2);alternate.append("same payload; alternate zip bytes",33);const auto other=trial.getChildFile("alternate.zip");CHECK(other.replaceWithData(alternate.getData(),alternate.getSize()));const auto alternateRef=f.ref(other);CHECK(alternateRef.sha256!=v2.sha256);
    CHECK(store.install(other,alternateRef,RackPackageActivation::makeActive,never)==RackLibraryStatus::identityConflict);CHECK(data(index)==old);CHECK(store.open(v2)==nullptr);
    CHECK(store.install(zip2,v2,RackPackageActivation::preserveActive,never)==RackLibraryStatus::ready);CHECK(store.active()==std::vector<RackPackageRef>{ref});CHECK(store.open(v2)!=nullptr);
}
void skins(const juce::File& trial) {
    int i=0;for(const auto* mutation:{"valid","missing-elements","wrong-groups","bad-binding","missing-asset","bad-narrow","global-param","tape"}){
        Fixture f;f.manifest.ref.id="skin-"+juce::String(i++);f.manifest.skinPath="skin.json";
        auto doc=juce::JSON::parse(skinJson(f));
        if(juce::String(mutation)=="missing-elements")doc.getDynamicObject()->removeProperty("elements");
        if(juce::String(mutation)=="wrong-groups")doc.getDynamicObject()->setProperty("groups",juce::var());
        if(juce::String(mutation)=="bad-narrow")doc.getDynamicObject()->setProperty("narrow",juce::JSON::parse("{\"view\":\"face\",\"cuts\":[[-20,9999]],\"width\":1,\"fill_at\":1,\"blend\":200}"));
        if(juce::String(mutation)=="missing-asset")doc["assets"]["face:plate"].getDynamicObject()->setProperty("file","unlisted.png");
        if(juce::String(mutation)=="bad-binding"||juce::String(mutation)=="global-param")doc["elements"].getArray()->add(juce::JSON::parse(juce::String(mutation)=="bad-binding"
            ?"{\"id\":\"face.x\",\"view\":\"face\",\"kind\":\"action\",\"geometry\":{\"rect\":[0,0,1,1]},\"binding\":{\"rack_unit\":255,\"key\":\"unknown\",\"type\":\"toggle\"}}"
            :"{\"id\":\"face.x\",\"view\":\"face\",\"kind\":\"action\",\"geometry\":{\"rect\":[0,0,1,1]},\"binding\":{\"param\":\"master.gain\",\"type\":\"float\"}}"));
        if(juce::String(mutation)=="tape")doc["assets"].getDynamicObject()->setProperty("unit:tape",juce::JSON::parse("{\"file\":\"screenshot.png\",\"text\":{\"cap_height_of_body\":{\"one line\":0.5,\"two lines\":0.3},\"line_pitch_of_cap\":1,\"max_width_of_body\":1,\"color\":\"#ffffff\",\"alpha\":1}}"));
        f.add("skin.json","skin",textBytes(juce::JSON::toString(doc,true)));const auto zip=f.write(trial,f.manifest.ref.id+".zip");RackPackageStore store({trial.getChildFile(f.manifest.ref.id)});
        CHECK((store.install(zip,f.ref(zip),RackPackageActivation::makeActive,never)==RackLibraryStatus::ready)==(juce::String(mutation)=="valid"||juce::String(mutation)=="tape"));
    }
}
void revealGeometry(const juce::File& trial) {
    const auto root=trial.getChildFile("store");RackPackageStore store({root});Fixture baseline;
    const auto originalZip=baseline.write(trial,"original.zip");const auto originalRef=baseline.ref(originalZip);
    CHECK(store.install(originalZip,originalRef,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto originalRecipe=data(release(root,originalRef).getChildFile("rack.json"));
    std::map<juce::String,juce::MemoryBlock> originalFiles;
    for(const auto* path:{"package.json","rack.json","screenshot.png",".archive-sha256"})originalFiles[path]=data(release(root,originalRef).getChildFile(path));
    const auto unchanged=[&]{for(const auto& [path,bytes]:originalFiles)CHECK(data(release(root,originalRef).getChildFile(path))==bytes);};
    const std::vector<std::pair<juce::String,juce::var>> cases{
        {"overflow",2147483647},{"product",2147483646},{"negative",-1},{"wide",juce::int64(4294967297ll)},
        {"fraction",1.5},{"text","1"},{"boolean",true},{"derived",2},{"zero",0},{"boundary",1},{"absent",juce::var()}};
    for(const auto& [name,value]:cases){
        Fixture f;f.manifest.ref.id="reveal-"+name;f.manifest.skinPath="skin.json";auto doc=juce::JSON::parse(skinJson(f));
        doc["reveal"].getDynamicObject()->setProperty("kind","door");if(name!="absent")doc["reveal"].getDynamicObject()->setProperty("units",value);
        f.add("skin.json","skin",textBytes(juce::JSON::toString(doc,true)));const auto archive=f.write(trial,f.manifest.ref.id+".zip");const auto previous=data(root.getChildFile("installed.json"));
        const auto result=store.install(archive,f.ref(archive),RackPackageActivation::preserveActive,never);
        const bool valid=name=="zero"||name=="boundary"||name=="absent";CHECK((result==RackLibraryStatus::ready)==valid);
        if(!valid){CHECK(data(root.getChildFile("installed.json"))==previous);CHECK(!release(root,f.manifest.ref).exists());unchanged();}
        CHECK(data(release(root,originalRef).getChildFile("rack.json"))==originalRecipe);CHECK(store.active()==std::vector<RackPackageRef>{originalRef});
    }
    for(const auto* mutation:{"face-product","drawer-sum","rgba","closed-type","pitch"}){
        Fixture f;f.manifest.ref.id=juce::String("reveal-")+mutation;f.manifest.skinPath="skin.json";auto doc=juce::JSON::parse(skinJson(f));
        doc["reveal"].getDynamicObject()->setProperty("kind",juce::String(mutation)=="drawer-sum"?"drawer":"door");doc["reveal"].getDynamicObject()->setProperty("units",1);
        if(juce::String(mutation)=="face-product")doc["assets"]["face:plate"].getDynamicObject()->setProperty("anchor",juce::JSON::parse("[0,2000000000]"));
        if(juce::String(mutation)=="drawer-sum"){
            doc["assets"]["setup:plate"].getDynamicObject()->setProperty("anchor",juce::JSON::parse("[0,1500000000]"));
            auto front=doc["assets"]["setup:plate"].clone();doc["assets"].getDynamicObject()->setProperty("front:plate",front);doc["reveal"].getDynamicObject()->setProperty("front","front");
        }
        if(juce::String(mutation)=="rgba")doc["assets"]["face:plate"].getDynamicObject()->setProperty("anchor",juce::JSON::parse("[8191,8191]"));
        if(juce::String(mutation)=="closed-type")doc.getDynamicObject()->setProperty("units",juce::int64(4294967297ll));
        if(juce::String(mutation)=="pitch")doc["rack_pitch"].getDynamicObject()->setProperty("u_px",1.0e300);
        f.add("skin.json","skin",textBytes(juce::JSON::toString(doc,true)));const auto archive=f.write(trial,f.manifest.ref.id+".zip");const auto previous=data(root.getChildFile("installed.json"));
        CHECK(store.install(archive,f.ref(archive),RackPackageActivation::preserveActive,never)==RackLibraryStatus::invalidData);CHECK(data(root.getChildFile("installed.json"))==previous);CHECK(data(release(root,originalRef).getChildFile("rack.json"))==originalRecipe);unchanged();
    }
    auto sources=juce::File(RACKSYNTH_PACKAGE_FIXTURE_DIR);for(int n=0;n<4;++n)sources=sources.getParentDirectory();sources=sources.getChildFile("resources/maker-skins");
    int current=0;
    for(const auto& folder:sources.findChildFiles(juce::File::findDirectories,false)){
        const auto files=folder.findChildFiles(juce::File::findFiles,false,"*elements.json");if(files.size()!=1)continue;++current;
        const auto actual=juce::JSON::parse(files[0]);Fixture f;f.manifest.ref.id="geometry-"+folder.getFileName();f.manifest.skinPath="skin.json";auto doc=juce::JSON::parse(skinJson(f));
        doc.getDynamicObject()->setProperty("units",actual["units"]);doc.getDynamicObject()->setProperty("rack_pitch",actual["rack_pitch"]);
        doc["reveal"].getDynamicObject()->setProperty("kind",actual["reveal"]["kind"]);
        if(actual["reveal"].hasProperty("units"))doc["reveal"].getDynamicObject()->setProperty("units",actual["reveal"]["units"]);
        const auto plate=[&](const juce::String& from,const juce::String& to){
            const auto original=actual["assets"][juce::Identifier(from+":plate")];auto copy=original.clone();const auto path="geometry-"+to+".png";const auto bytes=data(folder.getChildFile(original["file"].toString()));CHECK(!bytes.isEmpty());
            copy.getDynamicObject()->setProperty("file",path);doc["assets"].getDynamicObject()->setProperty(to+":plate",copy);f.add(path,"png",bytes);
        };
        plate("face","face");plate(actual["reveal"]["view"].toString(),"setup");
        for(const auto* key:{"front","hang"})if(actual["reveal"][key].isString()&&actual["reveal"][key].toString().isNotEmpty()){
            doc["reveal"].getDynamicObject()->setProperty(key,key);plate(actual["reveal"][key].toString(),key);
        }
        f.add("skin.json","skin",textBytes(juce::JSON::toString(doc,true)));const auto archive=f.write(trial,f.manifest.ref.id+".zip");
        CHECK(store.install(archive,f.ref(archive),RackPackageActivation::preserveActive,never)==RackLibraryStatus::ready);CHECK(store.active()==std::vector<RackPackageRef>{originalRef});
    }
    CHECK(current==10);
    unchanged();
}
void transientStageRename(const juce::File& trial) {
    const auto root=trial.getChildFile("store");RackPackageStore store({root});Fixture f;
    const auto firstZip=f.write(trial,"first.zip");const auto first=f.ref(firstZip);
    CHECK(store.install(firstZip,first,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto generation=store.generation();
    f.manifest.ref.version="2.0.0";const auto incoming=f.write(trial,"incoming.zip");const auto second=f.ref(incoming);
    // Simulate one transient ERROR_ACCESS_DENIED at the stage publication call.
    testing::RenameTrial control;control.errors[0]=ERROR_ACCESS_DENIED;control.virtualTime=true;testing::setRenameTrial(&control);
    CHECK(store.install(incoming,second,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);testing::setRenameTrial(nullptr);
    CHECK(control.attempts==2);CHECK(control.waits==10);
    CHECK(store.generation()==generation+1);CHECK(store.active()==std::vector<RackPackageRef>{second});CHECK(store.open(second)!=nullptr);
}
void stageRenameMatrix(const juce::File& base) {
    enum Mode { success,transient,persistent,deadline,cancelBefore,cancelDuring,cancelLast,cancelMoved,nonretry,collision,injectedOther,held,released,corrupt };
    const std::vector<Mode> modes{success,transient,persistent,deadline,cancelBefore,cancelDuring,cancelLast,cancelMoved,nonretry,collision,injectedOther,held,released,corrupt};
    for(const auto mode:modes){
        const auto trial=base.getChildFile(juce::String(int(mode)));CHECK(trial.createDirectory().wasOk());
        const auto root=trial.getChildFile("store");RackPackageStore store({root});Fixture f;f.add("nested/deep/notice.txt","notice",textBytes("owned notice"));
        const auto original=f.write(trial,"original.zip");const auto first=f.ref(original);CHECK(store.install(original,first,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
        const auto index=root.getChildFile("installed.json");const auto before=data(index);const auto generation=store.generation();const auto oldRecipe=data(release(root,first).getChildFile("rack.json"));
        const auto sentinel=trial.getChildFile("outside.txt");CHECK(sentinel.replaceWithText("outside"));const auto outside=data(sentinel);
        f.manifest.ref.version="2.0.0";const auto incoming=f.write(trial,"incoming.zip");const auto second=f.ref(incoming);
        testing::RenameTrial control;control.virtualTime=true;bool cancelledNow=false;
        if(mode!=success&&mode!=nonretry&&mode!=held&&mode!=released)control.errors[0]=ERROR_ACCESS_DENIED;
        if(mode==persistent)control.errors[1]=ERROR_ACCESS_DENIED;
        if(mode==nonretry)control.errors[0]=ERROR_SHARING_VIOLATION;
        if(mode==injectedOther)control.errors[1]=ERROR_ALREADY_EXISTS;
        control.holdChild=mode==held||mode==released;
        control.event=[&](int event,const juce::File& target){
            if((mode==cancelBefore&&event==0)||(mode==cancelDuring&&event==1)||(mode==cancelLast&&event==2)||(mode==cancelMoved&&event==3))cancelledNow=true;
            if(mode==deadline&&event==1)control.elapsedMs=100;
            if(mode==released&&event==1)testing::releaseStoreRenameChild();
            if(mode==collision&&event==2)CHECK(target.replaceWithText("target sentinel"));
            if(mode==corrupt&&event==3){
                const auto file=target.getChildFile("rack.json");const auto originalBytes=data(file);auto changed=originalBytes;
                CHECK(changed.getSize()>0);if(changed.getSize()>0)static_cast<unsigned char*>(changed.getData())[0]^=1;
                CHECK(changed.getSize()==originalBytes.getSize());CHECK(changed!=originalBytes);
                CHECK(file.replaceWithData(changed.getData(),changed.getSize()));const auto actual=data(file);
                CHECK(actual.getSize()==originalBytes.getSize());CHECK(actual==changed);
            }
        };
        testing::setRenameTrial(&control);const auto status=store.install(incoming,second,RackPackageActivation::makeActive,[&]{return cancelledNow;});testing::setRenameTrial(nullptr);
        if(mode==held){CHECK(testing::storeRenameChildHeld());testing::releaseStoreRenameChild();}
        const bool ok=mode==success||mode==transient||mode==released;
        const bool cancelledCase=mode==cancelBefore||mode==cancelDuring||mode==cancelLast||mode==cancelMoved;
        CHECK(status==(ok?RackLibraryStatus::ready:cancelledCase?RackLibraryStatus::cancelled:mode==corrupt?RackLibraryStatus::identityConflict:RackLibraryStatus::storageFailure));
        const int count=(mode==success||mode==deadline||mode==cancelBefore||mode==cancelDuring||mode==cancelLast||mode==nonretry)?1:2;
        CHECK(control.attempts==count);CHECK((control.waits==0)==(mode==success||mode==cancelBefore||mode==nonretry));
        CHECK(data(sentinel)==outside);CHECK(data(release(root,first).getChildFile("rack.json"))==oldRecipe);CHECK(store.open(first)!=nullptr);
        if(ok){CHECK(store.generation()==generation+1);CHECK(store.active()==std::vector<RackPackageRef>{second});CHECK(store.open(second)!=nullptr);}
        else {CHECK(data(index)==before);CHECK(store.generation()==generation);CHECK(store.active()==std::vector<RackPackageRef>{first});}
        if(mode==collision)CHECK(data(release(root,second))==textBytes("target sentinel"));
        const auto stages=root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*");
        if(mode==held||mode==cancelBefore||mode==cancelDuring||mode==cancelLast)CHECK(stages.size()==1);else CHECK(stages.isEmpty());
    }
}
void failedRenameCleanup(const juce::File& trial) {
    std::cout<<"INJECTED_STAGE_RENAME_FAILURE trial="<<trial.getFullPathName()<<std::endl;
    const auto root=trial.getChildFile("store");RackPackageStore store({root});Fixture f;
    f.add("nested/deep/notice.txt","notice",textBytes("owned nested notice"));
    const auto initial=f.write(trial,"original.zip");const auto first=f.ref(initial);
    CHECK(store.install(initial,first,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto index=root.getChildFile("installed.json");const auto before=data(index);const auto generation=store.generation();const auto active=store.active();
    const auto originalRecipe=data(release(root,first).getChildFile("rack.json"));CHECK(store.open(first)!=nullptr);
    const auto outside=trial.getChildFile("outside/sentinel.txt");CHECK(outside.getParentDirectory().createDirectory().wasOk());CHECK(outside.replaceWithText("outside unchanged"));const auto sentinel=data(outside);
    f.manifest.ref.version="3.0.0";const auto target=release(root,f.manifest.ref);CHECK(target.getParentDirectory().createDirectory().wasOk());CHECK(target.replaceWithText("target unchanged"));const auto targetBytes=data(target);
    f.manifest.ref.version="2.0.0";const auto incoming=f.write(trial,"incoming.zip");const auto second=f.ref(incoming);CHECK(!release(root,second).exists());
    airack::testing::forceNextStoreRenameFailure(false);CHECK(store.install(incoming,second,RackPackageActivation::makeActive,never)==RackLibraryStatus::storageFailure);
    CHECK(data(index)==before);CHECK(store.generation()==generation);CHECK(store.active()==active);CHECK(store.open(first)!=nullptr);CHECK(data(release(root,first).getChildFile("rack.json"))==originalRecipe);
    CHECK(!release(root,second).exists());CHECK(data(target)==targetBytes);CHECK(data(outside)==sentinel);
    CHECK(root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*").isEmpty());
    // An uncooperative read handle prevents safe reopening for deletion. Leave
    // the owned stage intact and retain the failure instead of claiming cleanup.
    f.manifest.ref.version="4.0.0";const auto blockedZip=f.write(trial,"blocked.zip");const auto blockedRef=f.ref(blockedZip);
    airack::testing::forceNextStoreRenameFailure(true);CHECK(store.install(blockedZip,blockedRef,RackPackageActivation::makeActive,never)==RackLibraryStatus::storageFailure);
    CHECK(airack::testing::storeRenameChildHeld());const auto retained=root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*");
    CHECK(retained.size()==1);if(retained.size()==1)CHECK(data(retained[0].getChildFile("nested/deep/notice.txt"))==textBytes("owned nested notice"));
    airack::testing::releaseStoreRenameChild();CHECK(!release(root,blockedRef).exists());CHECK(data(index)==before);CHECK(store.generation()==generation);CHECK(store.active()==active);CHECK(data(target)==targetBytes);CHECK(data(outside)==sentinel);
}
void nestedStagingCleanup(const juce::File& trial) {
    const auto root=trial.getChildFile("store");RackPackageStore store({root});Fixture f;f.add("nested/deep/notice.txt","notice",textBytes(juce::String::repeatedString("x",180000)));
    const auto zip=f.write(trial);const auto ref=f.ref(zip);CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto previous=data(root.getChildFile("installed.json")),recipe=data(release(root,ref).getChildFile("rack.json"));
    const auto empty=[&]{CHECK(root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*").isEmpty());};empty();
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);empty();CHECK(data(root.getChildFile("installed.json"))==previous);
    auto cancelledFixture=f;cancelledFixture.manifest.ref.version="2.0.0";const auto cancelZip=cancelledFixture.write(trial,"cancel-nested.zip");bool cancelledNested=false;
    CHECK(store.install(cancelZip,cancelledFixture.ref(cancelZip),RackPackageActivation::makeActive,[&]{
        for(const auto& stage:root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"install-*"))if(stage.getChildFile("nested/deep/notice.txt").exists()){cancelledNested=true;return true;}return false;
    })==RackLibraryStatus::cancelled);CHECK(cancelledNested);empty();CHECK(data(root.getChildFile("installed.json"))==previous);
    auto rejected=f;rejected.manifest.ref.version="3.0.0";rejected.manifest.files.back().sha256=juce::String::repeatedString("0",64);const auto rejectZip=rejected.write(trial,"reject-nested.zip");
    CHECK(store.install(rejectZip,rejected.ref(rejectZip),RackPackageActivation::makeActive,never)==RackLibraryStatus::identityConflict);empty();CHECK(data(root.getChildFile("installed.json"))==previous);CHECK(data(release(root,ref).getChildFile("rack.json"))==recipe);
}
void failuresAndSnapshots(const juce::File& trial) {
    const auto root=trial.getChildFile("store");RackPackageStore store({root});Fixture f;const auto zip=f.write(trial);const auto v1=f.ref(zip);
    CHECK(store.install(zip,v1,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);const auto index=root.getChildFile("installed.json");const auto old=data(index);
    f.manifest.ref.version="2.0.0";const auto zip2=f.write(trial,"v2.zip");const auto v2=f.ref(zip2);
    HANDLE blocked=CreateFileW(index.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(blocked!=INVALID_HANDLE_VALUE);
    CHECK(store.install(zip2,v2,RackPackageActivation::makeActive,never)==RackLibraryStatus::storageFailure);CHECK(data(index)==old);CHECK(store.active()==std::vector<RackPackageRef>{v1});
    if(blocked!=INVALID_HANDLE_VALUE)CloseHandle(blocked);CHECK(release(root,v2).isDirectory());CHECK(store.open(v2)==nullptr);
    CHECK(store.install(zip2,v2,RackPackageActivation::preserveActive,never)==RackLibraryStatus::ready);CHECK(store.generation()==2);CHECK(store.active()==std::vector<RackPackageRef>{v1});CHECK(store.open(v2)!=nullptr);
    f.manifest.ref.version="3.0.0";const auto zip3=f.write(trial,"v3.zip");const auto v3=f.ref(zip3);CHECK(release(root,v3).replaceWithText("blocking final path"));const auto before=data(index);
    CHECK(store.install(zip3,v3,RackPackageActivation::makeActive,never)==RackLibraryStatus::storageFailure);CHECK(data(index)==before);
    CHECK(index.replaceWithText("{}"));RackLibraryStatus status=RackLibraryStatus::ready;CHECK(store.active(&status)==std::vector<RackPackageRef>{v1});CHECK(status==RackLibraryStatus::unavailable);CHECK(store.generation()==2);
    CHECK(index.replaceWithData(old.getData(),old.getSize()));CHECK(store.active(&status)==std::vector<RackPackageRef>{v1});CHECK(status==RackLibraryStatus::unavailable);CHECK(store.generation()==2);
    CHECK(index.replaceWithData(before.getData(),before.getSize()));CHECK(store.active(&status)==std::vector<RackPackageRef>{v1});CHECK(status==RackLibraryStatus::ready);
    auto pinned=store.open(v1);CHECK(pinned!=nullptr);const auto image=release(root,v1).getChildFile("screenshot.png");
    CHECK(!MoveFileExW(image.getFullPathName().toWideCharPointer(),image.getSiblingFile("swapped.png").getFullPathName().toWideCharPointer(),0));
    if(pinned){juce::MemoryBlock out;CHECK(pinned->readListed("screenshot.png",out)==RackLibraryStatus::ready);CHECK(out==test::packageFixturePng());}
    pinned.reset();CHECK(MoveFileExW(image.getFullPathName().toWideCharPointer(),image.getSiblingFile("swapped.png").getFullPathName().toWideCharPointer(),0));
    CHECK(image.replaceWithText("different"));CHECK(store.open(v1)==nullptr);CHECK(store.active(&status)==std::vector<RackPackageRef>{v1});
}
struct Child {
    PROCESS_INFORMATION process{};
    Child(const juce::String& args) { STARTUPINFOW start{};start.cb=sizeof(start);const auto command="\""+juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName()+"\" "+args;std::wstring mutableCommand(command.toWideCharPointer());
        CHECK(CreateProcessW(nullptr,mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&process)); }
    bool wait(DWORD expected=0) { if(!process.hProcess)return false;const auto result=WaitForSingleObject(process.hProcess,15000);DWORD exit=1;if(result==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&exit);return result==WAIT_OBJECT_0&&exit==expected; }
    void crash() { CHECK(process.hProcess&&TerminateProcess(process.hProcess,91));CHECK(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0); }
    ~Child() { if(process.hThread)CloseHandle(process.hThread);if(process.hProcess)CloseHandle(process.hProcess); }
};
juce::String quote(const juce::String& s) { return "\""+s+"\""; }
juce::String command(const juce::String& mode,const juce::File& root,const juce::File& zip,const RackPackageRef& ref,const juce::File& signal) {
    return "--child "+mode+" "+quote(root.getFullPathName())+" "+quote(zip.getFullPathName())+" "+ref.id+" "+ref.version+" "+ref.sha256+" "+quote(signal.getFullPathName());
}
bool waitFile(const juce::File& file) { for(int n=0;n<500;++n){if(file.existsAsFile())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false; }
void processes(const juce::File& trial) {
    const auto root=trial.getChildFile("store");Fixture f;const auto zip=f.write(trial);const auto v1=f.ref(zip);f.manifest.ref.version="2.0.0";const auto zip2=f.write(trial,"v2.zip");const auto v2=f.ref(zip2);
    Child a(command("install",root,zip,v1,trial.getChildFile("unused-a"))),b(command("install",root,zip2,v2,trial.getChildFile("unused-b")));CHECK(a.wait());CHECK(b.wait());
    RackPackageStore store({root});CHECK(store.generation()==2);CHECK(store.open(v1)!=nullptr);CHECK(store.open(v2)!=nullptr);
    const auto ready=trial.getChildFile("held");Child held(command("hold",root,zip,v1,ready));CHECK(waitFile(ready));CHECK(store.remove(v1,never)==RackLibraryStatus::busy);CHECK(ready.getSiblingFile("held-release").replaceWithText("release"));CHECK(held.wait());
    const auto crashed=trial.getChildFile("crashed");Child crashedReader(command("hold",root,zip,v1,crashed));CHECK(waitFile(crashed));crashedReader.crash();
    Child c(command("remove",root,zip,v1,trial.getChildFile("unused-c"))),d(command("remove",root,zip2,v2,trial.getChildFile("unused-d")));CHECK(c.wait());CHECK(d.wait());CHECK(store.active().empty());CHECK(store.generation()==4);
    // A killed process holding the writer byte must not strand installation.
    const auto lockReady=trial.getChildFile("writer");Child writer(command("writer",root,zip,v1,lockReady));CHECK(waitFile(lockReady));CHECK(store.install(zip,v1,RackPackageActivation::makeActive,never)==RackLibraryStatus::busy);writer.crash();
    CHECK(store.install(zip,v1,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
}
void removalRecovery(const juce::File& trial) {
    Fixture f;const auto zip=f.write(trial);const auto ref=f.ref(zip);const auto root=trial.getChildFile("rollback");RackPackageStore store({root});
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);const auto index=root.getChildFile("installed.json");const auto original=data(index);
    HANDLE blocked=CreateFileW(index.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(blocked!=INVALID_HANDLE_VALUE);
    CHECK(store.remove(ref,never)==RackLibraryStatus::storageFailure);if(blocked!=INVALID_HANDLE_VALUE)CloseHandle(blocked);
    CHECK(data(index)==original);CHECK(store.open(ref)!=nullptr);CHECK(!root.getChildFile("removal.json").exists());
    bool cancelledAfterRename=false;CHECK(store.remove(ref,[&]{if(root.getChildFile("removal.json").exists()&&!release(root,ref).exists()){cancelledAfterRename=true;return true;}return false;})==RackLibraryStatus::cancelled);
    CHECK(cancelledAfterRename);CHECK(data(index)==original);CHECK(store.open(ref)!=nullptr);CHECK(!root.getChildFile("removal.json").exists());
    const auto sentinel=trial.getChildFile("saved-project-owned-sample.airasset");CHECK(sentinel.replaceWithText("project ownership survives package deletion"));const auto oldSentinel=data(sentinel);
    for(const auto* mode:{"remove-before","remove-after","remove-partial","remove-metadata"}){
        const auto crashRoot=trial.getChildFile(mode);RackPackageStore crashStore({crashRoot});CHECK(crashStore.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
        const auto oldIndex=data(crashRoot.getChildFile("installed.json"));Child process(command(mode,crashRoot,zip,ref,trial.getChildFile(juce::String(mode)+"-exited")));CHECK(process.wait(92));
        CHECK(crashRoot.getChildFile("removal.json").exists());const auto interruptedIndex=data(crashRoot.getChildFile("installed.json"));
        RackLibraryStatus status=RackLibraryStatus::invalidData;const auto active=crashStore.active(&status);CHECK(status==RackLibraryStatus::ready);CHECK(!crashRoot.getChildFile("removal.json").exists());
        if(juce::String(mode)=="remove-before"){CHECK(interruptedIndex==oldIndex);CHECK(active==std::vector<RackPackageRef>{ref});CHECK(crashStore.generation()==1);CHECK(crashStore.open(ref)!=nullptr);}
        else {CHECK(interruptedIndex!=oldIndex);CHECK(active.empty());CHECK(crashStore.generation()==2);CHECK(crashStore.open(ref)==nullptr);CHECK(!release(crashRoot,ref).exists());}
        CHECK(data(crashRoot.getChildFile("installed.json"))==interruptedIndex);CHECK(data(sentinel)==oldSentinel);
    }
    CHECK(root.getChildFile("removal.json").replaceWithText("{\"schema\":1,\"ref\":{\"id\":\"ember-reed\",\"version\":\"1.0.0\",\"sha256\":\""+ref.sha256+"\"},\"generation\":\"1\",\"trash\":\"../outside\"}"));
    RackLibraryStatus status=RackLibraryStatus::ready;CHECK(store.active(&status)==std::vector<RackPackageRef>{ref});CHECK(status==RackLibraryStatus::unavailable);CHECK(data(index)==original);CHECK(data(sentinel)==oldSentinel);
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::unavailable);CHECK(data(index)==original);
}
void cancellableAcquisition(const juce::File& trial) {
    Fixture fixture;const auto zip=fixture.write(trial);const auto ref=fixture.ref(zip);const auto root=trial.getChildFile("store");RackPackageStore store({root});
    CHECK(store.install(zip,ref,RackPackageActivation::makeActive,never)==RackLibraryStatus::ready);
    const auto before=data(root.getChildFile("installed.json"));RackLibraryStatus status=RackLibraryStatus::ready;
    CHECK(!store.open(ref,&status,[]{return true;}));CHECK(status==RackLibraryStatus::cancelled);
    CHECK(store.active(&status,[]{return true;}).empty());CHECK(status==RackLibraryStatus::cancelled);
    store.generation(&status,[]{return true;});CHECK(status==RackLibraryStatus::cancelled);
    unsigned calls=0;CHECK(!store.open(ref,&status,[&]{return ++calls>=12;}));CHECK(calls>=12&&status==RackLibraryStatus::cancelled);
    CHECK(data(root.getChildFile("installed.json"))==before);CHECK(store.open(ref)!=nullptr);
    std::atomic<bool> holding{false},releaseMutex{false};
    std::thread owner([&]{unsigned checkpoints=0;store.generation(nullptr,[&]{if(++checkpoints==2){holding=true;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!releaseMutex&&std::chrono::steady_clock::now()<until)std::this_thread::yield();}return false;});});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!holding&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();CHECK(holding);
    calls=0;CHECK(!store.open(ref,&status,[&]{return ++calls>=4;}));CHECK(status==RackLibraryStatus::cancelled&&holding&&!releaseMutex);releaseMutex=true;owner.join();
    const auto signal=trial.getChildFile("crashed");Child process(command("remove-before",root,zip,ref,signal));CHECK(process.wait(92));
    const auto journal=data(root.getChildFile("removal.json"));CHECK(journal.getSize()>0);calls=0;
    store.active(&status,[&]{return ++calls>=10;});CHECK(status==RackLibraryStatus::cancelled);
    CHECK(data(root.getChildFile("removal.json"))==journal);CHECK(data(root.getChildFile("installed.json"))==before);
    CHECK(store.active(&status)==std::vector<RackPackageRef>{ref});CHECK(status==RackLibraryStatus::ready);CHECK(!root.getChildFile("removal.json").exists());CHECK(store.open(ref)!=nullptr);
}
int child(const juce::StringArray& args) {
    if(args.size()!=9)return 2;const auto mode=args[2];RackPackageStore store({juce::File(args[3])});const juce::File zip(args[4]),signal(args[8]);const RackPackageRef ref{args[5],args[6],args[7]};
    if(mode=="writer"){
        auto lock=juce::File(args[3]).getChildFile("writer.lock");HANDLE h=CreateFileW(lock.getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,0,nullptr);OVERLAPPED ov{};
        if(h==INVALID_HANDLE_VALUE||!LockFileEx(h,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&ov))return 3;signal.replaceWithText("locked");std::this_thread::sleep_for(std::chrono::seconds(12));CloseHandle(h);return 0;
    }
    if(mode=="hold"){auto lease=store.open(ref);if(!lease)return 3;signal.replaceWithText("held");return waitFile(signal.getSiblingFile(signal.getFileName()+"-release"))?0:4;}
    if(mode=="remove-before"||mode=="remove-after"||mode=="remove-partial"||mode=="remove-metadata"){
        const auto generation=store.generation();const juce::File root(args[3]);
        store.remove(ref,[&]{
            const auto doc=juce::JSON::parse(root.getChildFile("installed.json"));const auto current=std::uint64_t(doc["generation"].toString().getLargeIntValue());
            bool partial=false,metadata=false;
            for(const auto& trash:root.getChildFile("staging").findChildFiles(juce::File::findDirectories,false,"remove-*")){
                partial=!trash.getChildFile("rack.json").exists();metadata=!trash.getChildFile("package.json").exists();
            }
            if(root.getChildFile("removal.json").exists()&&!release(root,ref).exists()&&((mode=="remove-before"&&current==generation)||(mode=="remove-after"&&current==generation+1)
                ||(mode=="remove-partial"&&current==generation+1&&partial)||(mode=="remove-metadata"&&current==generation+1&&metadata))){signal.replaceWithText("owned child exited inside removal transaction");ExitProcess(92);}return false;
        });return 7;
    }
    for(int n=0;n<500;++n){auto s=mode=="install"?store.install(zip,ref,RackPackageActivation::makeActive,never):store.remove(ref,never);if(s==RackLibraryStatus::ready)return 0;if(s!=RackLibraryStatus::busy)return 5;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return 6;
}
}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI initialise;juce::StringArray args;for(int i=0;i<argc;++i)args.add(juce::String::fromUTF8(argv[i]));if(args.size()>1&&args[1]=="--child")return child(args);
    if(argc!=2&&argc!=3)return 2;const auto trial=juce::File(args[1]).getChildFile(juce::Uuid().toString());CHECK(trial.createDirectory().wasOk());
    if(argc==3&&args[2]=="--stage-rename-matrix"){stageRenameMatrix(trial);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&args[2]=="--transient-stage-rename"){transientStageRename(trial);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&args[2]=="--failed-rename-cleanup"){failedRenameCleanup(trial);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&args[2]=="--cancel-acquisition"){cancellableAcquisition(trial);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    cancellableAcquisition(trial.getChildFile("cancellation"));
    CHECK(!testing::mcpTestRackLibraryRoot({}));
    CHECK(!testing::mcpTestRackLibraryRoot("relative/rack-root"));
    CHECK(!testing::mcpTestRackLibraryRoot("C:relative"));
    CHECK(!testing::mcpTestRackLibraryRoot("\\root-relative"));
    const auto absolute=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rack-root-test");
    CHECK(testing::mcpTestRackLibraryRoot(absolute.getFullPathName())==absolute.getChildFile("Rack Library"));
    const auto current=RackLibraryPaths::forCurrentUser().root;
#if defined(_DEBUG)
    SetLastError(ERROR_SUCCESS);const auto overrideSize=GetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",nullptr,0);
    const bool overridePresent=overrideSize!=0||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;
    const auto diagnostic=testing::mcpTestRackLibraryRoot(juce::SystemStats::getEnvironmentVariable("AIRACK_MCP_TEST_RACK_ROOT",{}));
    if(overridePresent) CHECK(current==(diagnostic?*diagnostic:juce::File{}));
    else
#endif
    CHECK(current.getFullPathName().replaceCharacter('\\','/').endsWith("AI Rack Synth/Rack Library"));
    lifecycle(trial.getChildFile("lifecycle"));refusals(trial.getChildFile("refusals"));skins(trial.getChildFile("skins"));failuresAndSnapshots(trial.getChildFile("failures"));
    hostileFilesystem(trial.getChildFile("hostile"));receipts(trial.getChildFile("receipts"));processes(trial.getChildFile("processes"));removalRecovery(trial.getChildFile("recovery"));
    stageRenameMatrix(trial.getChildFile("rename-matrix"));revealGeometry(trial.getChildFile("reveal-geometry"));nestedStagingCleanup(trial.getChildFile("nested-cleanup"));failedRenameCleanup(trial.getChildFile("failed-rename-cleanup"));
    std::cout<<checks<<" checks, "<<failures<<" failures; retained "<<trial.getFullPathName()<<'\n';return failures?1:0;
}
