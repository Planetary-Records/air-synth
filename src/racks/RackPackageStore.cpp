#include "RackPackageStore.h"
#include "McpTestRackRoot.h"
#include "StrictJson.h"
#include "HouseSkin.h"
#include "StudioSkinData.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <limits>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <set>
#include <utility>
#include <windows.h>
#include <shlobj.h>

namespace airack {
#if defined(AIRACK_STORE_TESTS)
namespace testing {
thread_local bool failNextStoreRename=false,holdStageChild=false;
thread_local RenameTrial* renameTrial=nullptr;
void setRenameTrial(RenameTrial* trial) noexcept { renameTrial=trial; }
thread_local HANDLE heldStageChild=INVALID_HANDLE_VALUE;
void forceNextStoreRenameFailure(bool holdChild) noexcept { failNextStoreRename=true;holdStageChild=holdChild; }
bool storeRenameChildHeld() noexcept { return heldStageChild!=INVALID_HANDLE_VALUE; }
void releaseStoreRenameChild() noexcept { if(heldStageChild!=INVALID_HANDLE_VALUE)CloseHandle(heldStageChild);heldStageChild=INVALID_HANDLE_VALUE; }
bool consumeStoreRenameFailure() noexcept { return std::exchange(failNextStoreRename,false); }
}
#endif
namespace {
using juce::String;
using Status=RackLibraryStatus;
namespace limits=rackPackageLimits;
void statusTo(Status* out,Status value) { if(out)*out=value; }
bool cancelled(const RackLibraryCancel& cancel) { return cancel&&cancel(); }
bool digest(const String& value) { return value.length()==64&&value.containsOnly("0123456789abcdef"); }
bool validRef(const RackPackageRef& r) { return validRackPackageId(r.id)&&validRackPackageVersion(r.version)&&digest(r.sha256); }
String identity(const RackPackageRef& r) { return r.id+"/"+r.version; }
std::wstring native(const juce::File& f) {
    auto path=f.getFullPathName().replaceCharacter('/','\\');if(path.length()==2&&path[1]==':')path+="\\";
    return L"\\\\?\\"+std::wstring(path.toWideCharPointer());
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE h):value(h) {}
    ~Handle() { reset(); }
    Handle(Handle&& h) noexcept:value(std::exchange(h.value,INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& h) noexcept { if(this!=&h){reset();value=std::exchange(h.value,INVALID_HANDLE_VALUE);}return *this; }
    explicit operator bool() const { return value!=INVALID_HANDLE_VALUE; }
    void reset() { if(*this)CloseHandle(value);value=INVALID_HANDLE_VALUE; }
};
bool regular(HANDLE h,bool directory=false) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(h,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
        &&bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory&&GetFileType(h)==FILE_TYPE_DISK
        &&(directory||info.nNumberOfLinks==1);
}
std::uint64_t length(HANDLE h) { LARGE_INTEGER size{};return GetFileSizeEx(h,&size)&&size.QuadPart>=0?std::uint64_t(size.QuadPart):UINT64_MAX; }
Handle openFile(const juce::File& file,DWORD access=GENERIC_READ,DWORD share=FILE_SHARE_READ,DWORD creation=OPEN_EXISTING) {
    Handle h(CreateFileW(native(file).c_str(),access,share,nullptr,creation,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(h&&!regular(h.value)){h.reset();SetLastError(ERROR_INVALID_DATA);}return h;
}
// Every ancestor stays open without delete sharing; a checked directory cannot be swapped for a junction.
bool directories(const juce::File& file,bool create,std::vector<Handle>& held,bool* missing=nullptr) {
    if(missing)*missing=false;
    const auto path=file.getFullPathName().replaceCharacter('/','\\');
    if(path.length()<3||path[1]!=':'||path[2]!='\\')return false;
    juce::File at(path.substring(0,3));auto parts=juce::StringArray::fromTokens(path.substring(3),"\\","");parts.removeEmptyStrings();
    for(int n=-1;n<parts.size();++n){
        if(n>=0){if(parts[n]=="."||parts[n]=="..")return false;at=at.getChildFile(parts[n]);
            if(create&&!CreateDirectoryW(native(at).c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;}
        Handle h(CreateFileW(native(at).c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h){const auto error=GetLastError();if(missing&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND))*missing=true;return false;}
        if(!regular(h.value,true))return false;held.push_back(std::move(h));
    }
    return true;
}
bool seek(HANDLE h,std::uint64_t offset) { LARGE_INTEGER where;where.QuadPart=static_cast<LONGLONG>(offset);return SetFilePointerEx(h,where,nullptr,FILE_BEGIN); }
bool readBytes(HANDLE h,std::uint64_t cap,juce::MemoryBlock& out,const RackLibraryCancel& cancel={}) {
    if(cancelled(cancel))return false;
    const auto size=length(h);if(size>cap||!seek(h,0))return false;juce::MemoryBlock bytes(static_cast<size_t>(size));
    size_t offset=0;while(offset<bytes.getSize()){if(cancelled(cancel))return false;DWORD count=0;const auto amount=DWORD(std::min<size_t>(131072,bytes.getSize()-offset));
        if(!ReadFile(h,static_cast<char*>(bytes.getData())+offset,amount,&count,nullptr)||count==0)return false;offset+=count;}
    out.swapWith(bytes);return true;
}
bool writeBytes(HANDLE h,const void* bytes,size_t size) {
    size_t offset=0;while(offset<size){DWORD count=0;const auto amount=DWORD(std::min<size_t>(131072,size-offset));
        if(!WriteFile(h,static_cast<const char*>(bytes)+offset,amount,&count,nullptr)||count!=amount)return false;offset+=count;}return true;
}
bool renameHandle(HANDLE h,const juce::File& target,bool replace=false,DWORD* error=nullptr,std::chrono::steady_clock::time_point* failedAt=nullptr) {
    const auto path=native(target);const auto bytes=path.size()*sizeof(wchar_t);
    std::vector<std::byte> buffer(offsetof(FILE_RENAME_INFO,FileName)+bytes+sizeof(wchar_t));
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());info->ReplaceIfExists=replace;info->RootDirectory=nullptr;
    info->FileNameLength=DWORD(bytes);std::memcpy(info->FileName,path.data(),bytes);
    const bool ok=SetFileInformationByHandle(h,FileRenameInfo,info,DWORD(buffer.size()))!=0;
    const DWORD result=ok?ERROR_SUCCESS:GetLastError();if(!ok&&failedAt)*failedAt=std::chrono::steady_clock::now();if(error)*error=result;return ok;
}
bool dispose(Handle& h) {
    if(!h)return true;FILE_DISPOSITION_INFO info{TRUE};const bool result=SetFileInformationByHandle(h.value,FileDispositionInfo,&info,sizeof(info))!=0;h.reset();return result;
}
struct FileLock {
    Handle file;bool acquired=false;OVERLAPPED position{};
    FileLock(const juce::File& path,bool exclusive) {
        file=openFile(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,OPEN_ALWAYS);
        if(file)acquired=LockFileEx(file.value,LOCKFILE_FAIL_IMMEDIATELY|(exclusive?LOCKFILE_EXCLUSIVE_LOCK:0),0,1,0,&position)!=0;
    }
    ~FileLock() { if(acquired)UnlockFileEx(file.value,0,1,0,&position); }
};
struct HeldInput final:juce::InputStream {
    HANDLE handle;std::int64_t size,position=0;bool failed=false;
    explicit HeldInput(HANDLE h):handle(h),size(static_cast<std::int64_t>(length(h))) { setPosition(0); }
    juce::int64 getTotalLength() override { return size; }
    juce::int64 getPosition() override { return position; }
    bool setPosition(juce::int64 p) override { if(p<0||p>size||!seek(handle,std::uint64_t(p)))return false;position=p;return true; }
    bool isExhausted() override { return position>=size; }
    int read(void* out,int count) override { DWORD got=0;if(count<0||!ReadFile(handle,out,DWORD(count),&got,nullptr)){failed=true;return 0;}position+=got;return int(got); }
};
struct Tree {
    juce::File path;std::vector<Handle> ancestors;
    std::map<String,Handle> dirs,files;std::vector<String> dirOrder;bool cleanup=false;
    ~Tree() { if(cleanup){for(auto& [name,h]:files)dispose(h);for(auto it=dirOrder.rbegin();it!=dirOrder.rend();++it)dispose(dirs.at(*it));} }
    bool root(const juce::File& target,bool create,bool deletable=false) {
        path=target;if(!directories(target.getParentDirectory(),create,ancestors))return false;
        if(create&&!CreateDirectoryW(native(target).c_str(),nullptr))return false;
        Handle h(CreateFileW(native(target).c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY|(deletable?DELETE:0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h||!regular(h.value,true))return false;dirs.emplace("",std::move(h));dirOrder.push_back("");cleanup=create;return true;
    }
    bool parent(const String& relative,bool create,bool deletable=false) {
        auto parts=juce::StringArray::fromTokens(relative,"/","");String prefix;
        for(int i=0;i+1<parts.size();++i){prefix+=(prefix.isEmpty()?"":"/")+parts[i];if(dirs.contains(prefix))continue;
            const auto dir=path.getChildFile(prefix);
            if(create&&!CreateDirectoryW(native(dir).c_str(),nullptr))return false;
            Handle h(CreateFileW(native(dir).c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY|((create||deletable)?DELETE:0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!h||!regular(h.value,true))return false;dirs.emplace(prefix,std::move(h));dirOrder.push_back(prefix);}
        return true;
    }
    bool add(const String& name,bool create,bool deletable=false) {
        if(!parent(name,create,deletable))return false;
        auto h=openFile(path.getChildFile(name),GENERIC_READ|(create?GENERIC_WRITE:0)|((create||deletable)?DELETE:0),FILE_SHARE_READ,create?CREATE_NEW:OPEN_EXISTING);
        if(!h)return false;files.emplace(name,std::move(h));return true;
    }
    bool read(const String& name,std::uint64_t cap,juce::MemoryBlock& out,const RackLibraryCancel& cancel={}) const {
        const auto it=files.find(name);return it!=files.end()&&readBytes(it->second.value,cap,out,cancel);
    }
    bool write(const String& name,const void* bytes,size_t size) {
        return add(name,true)&&writeBytes(files.at(name).value,bytes,size)&&FlushFileBuffers(files.at(name).value);
    }
    void closeChildren() {
        files.clear();for(auto it=dirOrder.rbegin();it!=dirOrder.rend();++it)if(it->isNotEmpty())dirs.erase(*it);
        dirOrder.resize(1);
    }
};
struct Snapshot { std::uint64_t generation=0;std::vector<RackPackageRef> releases,active;String hash;bool exists=false; };
juce::var object(std::initializer_list<std::pair<juce::Identifier,juce::var>> fields) {
    auto* obj=new juce::DynamicObject;for(const auto& [key,value]:fields)obj->setProperty(key,value);return obj;
}
bool closed(const juce::var& v,int count) { return v.getDynamicObject()&&v.getDynamicObject()->getProperties().size()==count; }
bool number(const String& text,std::uint64_t& value) {
    const auto s=text.toStdString();if(s.empty()||(s.size()>1&&s[0]=='0'))return false;
    const auto result=std::from_chars(s.data(),s.data()+s.size(),value);return result.ec==std::errc{}&&result.ptr==s.data()+s.size();
}
juce::var refJson(const RackPackageRef& r) { return object({{"id",r.id},{"version",r.version},{"sha256",r.sha256}}); }
bool parseRef(const juce::var& v,RackPackageRef& r) {
    if(!closed(v,3)||!v["id"].isString()||!v["version"].isString()||!v["sha256"].isString())return false;
    r={v["id"].toString(),v["version"].toString(),v["sha256"].toString()};return validRef(r);
}
Status readIndex(const juce::File& root,Snapshot& last,const RackLibraryCancel& cancel={}) {
    if(cancelled(cancel))return Status::cancelled;
    if(root==juce::File())return Status::unavailable;
    std::vector<Handle> held;bool missing=false;
    if(!directories(root,false,held,&missing)){
        if(missing&&!last.exists)return Status::ready;
        return Status::unavailable;
    }
    const auto file=root.getChildFile("installed.json");auto h=openFile(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE);
    if(!h){if(GetLastError()==ERROR_FILE_NOT_FOUND&&!last.exists)return Status::ready;return Status::unavailable;}
    juce::MemoryBlock data;juce::var doc;Snapshot next;
    if(!readBytes(h.value,limits::catalogBytes,data,cancel)||!ai::parseStrictJson({static_cast<const char*>(data.getData()),data.getSize()},doc,limits::catalogBytes)
        ||!closed(doc,4)||!doc["schema"].isInt()||int(doc["schema"])!=1||!doc["generation"].isString()
        ||!number(doc["generation"].toString(),next.generation)||next.generation==0||!doc["releases"].isArray()||!doc["active"].isArray())return Status::unavailable;
    if(doc["active"].size()>int(limits::installedCount)||doc["releases"].size()>int(limits::installedCount*limits::releasesPerRack))return Status::unavailable;
    std::set<String> versions,ids;std::map<String,size_t> counts;
    for(const auto& item:*doc["releases"].getArray()){if(cancelled(cancel))return Status::cancelled;RackPackageRef ref;if(!parseRef(item,ref)||!versions.insert(identity(ref)).second||++counts[ref.id]>limits::releasesPerRack)return Status::unavailable;next.releases.push_back(ref);}
    for(const auto& item:*doc["active"].getArray()){if(cancelled(cancel))return Status::cancelled;RackPackageRef ref;if(!parseRef(item,ref)||!ids.insert(ref.id).second||std::find(next.releases.begin(),next.releases.end(),ref)==next.releases.end())return Status::unavailable;next.active.push_back(ref);}
    next.hash=juce::SHA256(data).toHexString();next.exists=true;
    if(last.exists&&(next.generation<last.generation||(next.generation==last.generation&&next.hash!=last.hash)))return Status::unavailable;
    if(cancelled(cancel))return Status::cancelled;last=std::move(next);return Status::ready;
}
Status commitIndex(const juce::File& root,Snapshot& next) {
    if(next.generation==UINT64_MAX)return Status::storageFailure;
    ++next.generation;juce::Array<juce::var> releases,active;
    for(const auto& ref:next.releases)releases.add(refJson(ref));for(const auto& ref:next.active)active.add(refJson(ref));
    const auto json=juce::JSON::toString(object({{"schema",1},{"generation",String(std::to_string(next.generation))},{"releases",releases},{"active",active}}),true);
    if(json.getNumBytesAsUTF8()>limits::catalogBytes)return Status::byteLimit;
    const auto temporary=root.getChildFile("index-"+juce::Uuid().toString()+".tmp");auto h=openFile(temporary,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,CREATE_NEW);
    if(!h)return Status::storageFailure;
    if(!writeBytes(h.value,json.toRawUTF8(),json.getNumBytesAsUTF8())||!FlushFileBuffers(h.value)||!renameHandle(h.value,root.getChildFile("installed.json"),true)){
        dispose(h);return Status::storageFailure;}
    next.hash=juce::SHA256(json.toRawUTF8(),json.getNumBytesAsUTF8()).toHexString();next.exists=true;return Status::ready;
}
const RackPackageFile* listed(const RackPackageManifest& m,const String& path) {
    const auto it=std::find_if(m.files.begin(),m.files.end(),[&](const auto& f){return f.path==path;});return it==m.files.end()?nullptr:&*it;
}
std::span<const std::byte> span(const juce::MemoryBlock& bytes) { return {static_cast<const std::byte*>(bytes.getData()),bytes.getSize()}; }
String skinPath(const RackPackageManifest& m,const String& relative) {
    const auto parent=m.skinPath.containsChar('/')?m.skinPath.upToLastOccurrenceOf("/",true,false):String();return parent+relative;
}
bool binding(const RackDesign& design,int unit,const String& key) {
    const auto it=std::find_if(design.units.begin(),design.units.end(),[unit](const auto& u){return int(u.id)==unit;});
    return it!=design.units.end()&&rackParameterIndex(it->module,0,key).has_value();
}
// Guard the trusted embedded loader's array and raw bitmap-copy assumptions before admitting package data.
bool skinShape(const juce::var& doc,const RackPackageManifest& m,const Tree& tree,const RackLibraryCancel& cancel) {
    if(!doc.isObject()||!doc["elements"].isArray()||!doc["groups"].isArray()||!doc["assets"].isObject())return false;
    if(!doc["units"].isInt()||int(doc["units"])<=0||int(doc["units"])>64||!doc["reveal"].isObject())return false;
    const auto reveal=doc["reveal"];
    if(reveal.hasProperty("units")&&(!reveal["units"].isInt()||int(reveal["units"])<0))return false;
    std::map<String,std::pair<int,int>> images;std::uint64_t imageBytes=0;
    for(const auto& field:doc["assets"].getDynamicObject()->getProperties()){
        if(cancelled(cancel))return false;
        if(!field.value.isObject())return false;const auto& a=field.value;
        if(a.hasProperty("file")){
            if(!a["file"].isString()||!validRackPackagePath(a["file"].toString()))return false;
            const auto path=skinPath(m,a["file"].toString());const auto* file=listed(m,path);juce::MemoryBlock bytes;std::uint32_t w=0,h=0;
            if(!file||file->kind!="png"||!tree.read(path,limits::pngBytes,bytes,cancel)||!inspectRackPackagePng(span(bytes),w,h))return false;
            const auto count=std::uint64_t(w)*h*4;if(count>limits::decodedSkinBytes-imageBytes)return false;imageBytes+=count;images[field.name.toString()]={int(w),int(h)};
        }
    }
    for(const auto& e:*doc["elements"].getArray()){
        if(!e.isObject()||!e["geometry"].isObject())return false;
        if(e.hasProperty("binding")){const auto b=e["binding"];if(!b.isObject()||!b["rack_unit"].isInt()||int(b["rack_unit"])<0||int(b["rack_unit"])>255||!b["key"].isString()||b.hasProperty("param"))return false;}
    }
    for(const auto& g:*doc["groups"].getArray())if(!g.isObject()||!g["members"].isArray()||!g["focus_order"].isArray())return false;
    if(doc.hasProperty("narrow")){
        const auto n=doc["narrow"];if(!n.isObject()||!n["cuts"].isArray()||!n["fill"].isArray()||n["fill"].size()!=2)return false;
        const auto image=images.find(n["view"].toString()+":plate");if(image==images.end())return false;
        for(const auto* key:{"blend","fill_at","width"})if(!n[key].isInt())return false;
        const auto [width,height]=image->second;const int blend=int(n["blend"]),half=blend/2;int previous=0,reduced=width;
        if(blend<0||blend>width)return false;
        for(const auto& cut:*n["cuts"].getArray()){
            if(!cut.isArray()||cut.size()!=2||!cut[0].isInt()||!cut[1].isInt())return false;const int a=int(cut[0]),b=int(cut[1]);
            if(a<half||a-half<previous||b<=a||b>width-half||a-half+blend>width||b-half+blend>width)return false;previous=b+half;reduced-=b-a;}
        if(reduced<=0||reduced!=int(n["width"])||int(n["fill_at"])<=0||int(n["fill_at"])>=reduced||!n["fill"][0].isInt()||!n["fill"][1].isInt())return false;
        const int a=int(n["fill"][0]),b=int(n["fill"][1]);if(a<0||b<=a||b>width)return false;
        const auto extra=(std::uint64_t(reduced)*2+std::uint64_t(b-a))*height*4;
        if(extra>limits::decodedSkinBytes-imageBytes)return false;
    }
    return true;
}
bool nativeSkinGeometry(const studio::house::Model& model) {
    constexpr auto nativeMax=std::uint64_t(std::numeric_limits<int>::max());
    if(!std::isfinite(model.unitHeight)||model.unitHeight<=0||!std::isfinite(model.panelWidth)||model.panelWidth<=0||model.panelWidth>double(nativeMax))return false;
    using Extent=std::pair<std::uint64_t,std::uint64_t>;
    std::map<String,Extent> views;
    for(const auto& view:{String("face"),model.reveal.view,model.reveal.front,model.reveal.hang,model.strip})if(view.isNotEmpty()){
        const auto& plate=model.asset(view+":plate");const auto width=plate.image.getWidth(),height=plate.image.getHeight();
        const double x=plate.anchor.x,y=plate.anchor.y;
        if(width<=0||height<=0||!std::isfinite(x)||!std::isfinite(y)||x<0||y<0
            ||std::ceil(x)>double(nativeMax-std::uint64_t(width))||std::ceil(y)>double(nativeMax-std::uint64_t(height)))return false;
        // Ceil also bounds the existing renderer's roundToInt before any native conversion.
        views[view]={std::uint64_t(std::ceil(x))+std::uint64_t(width),std::uint64_t(std::ceil(y))+std::uint64_t(height)};
    }
    if(!views.contains("face")||!views.contains(model.reveal.view))return false;
    const auto faceHeight=views.at("face").second,revealHeight=views.at(model.reveal.view).second;
    const auto closedUnits=std::uint64_t(model.units),revealUnits=std::uint64_t(model.reveal.units);
    const double pitch=std::max(double(model.unitHeight),double(faceHeight)/double(closedUnits));
    const double capacity=std::ceil(double(revealHeight)/pitch);
    if(!std::isfinite(capacity)||capacity<0||capacity>double(nativeMax)||revealUnits>std::uint64_t(capacity))return false;
    const auto totalUnits=closedUnits+revealUnits;
    if(totalUnits>nativeMax||faceHeight>nativeMax/totalUnits||(revealUnits>0&&faceHeight>nativeMax/revealUnits))return false;
    const auto extraHeight=faceHeight*revealUnits/closedUnits,doorHeight=faceHeight+extraHeight;
    if(doorHeight>nativeMax)return false;
    std::uint64_t width=0,height=0;
    for(const auto& [name,size]:views){width=std::max(width,size.first);height=std::max(height,size.second);}
    if(model.reveal.kind=="door")height=std::max(height,faceHeight*totalUnits/closedUnits);
    if(model.reveal.kind=="drawer"){
        const auto frontHeight=model.reveal.front.isEmpty()?0:views.at(model.reveal.front).second;
        const auto openedHeight=faceHeight+revealHeight+frontHeight;
        if(openedHeight>nativeMax)return false;height=std::max(height,openedHeight);
    }
    return height>0&&width<=limits::decodedSkinBytes/4/height;
}
Status validateSkin(const RackPackageManifest& m,const RackDesign& design,const Tree& tree,const RackLibraryCancel& cancel) {
    if(cancelled(cancel))return Status::cancelled;
    if(m.skinPath.isEmpty())return Status::ready;
    juce::MemoryBlock bytes;juce::var doc;
    if(!tree.read(m.skinPath,limits::skinBytes,bytes,cancel)||!ai::parseStrictJson({static_cast<const char*>(bytes.getData()),bytes.getSize()},doc,limits::skinBytes)||!skinShape(doc,m,tree,cancel))return Status::invalidData;
    try {
        int fontBytes=0;const auto* font=StudioSkinData::getNamedResource("PermanentMarkerRegular_ttf",fontBytes);
        auto face=font?juce::Typeface::createSystemTypefaceFor(font,size_t(fontBytes)):juce::Typeface::Ptr();
        const auto model=studio::house::loadModel([&](const String& path){juce::MemoryBlock out;const auto resolved=path=="elements.json"?m.skinPath:skinPath(m,path);const auto* f=listed(m,resolved);if(!f||!tree.read(resolved,f->bytes,out,cancel))throw std::runtime_error("unlisted skin file");return out;},face);
        if(model.designId!=design.id||model.units<=0||model.units>64)return Status::invalidData;
        const auto asset=[&](const String& key){return model.asset(key).image.isValid();};
        if(!asset("face:plate")||!asset(model.reveal.view+":plate")
            ||(model.reveal.kind!="recess"&&model.reveal.kind!="drawer"&&model.reveal.kind!="door"))return Status::invalidData;
        for(const auto& view:{model.reveal.front,model.reveal.hang,model.strip})if(view.isNotEmpty()&&!asset(view+":plate"))return Status::invalidData;
        if(!nativeSkinGeometry(model))return Status::invalidData;
        std::set<String> elements,groups;
        for(const auto& e:model.elements){
            if(cancelled(cancel))return Status::cancelled;
            if(e.id.isEmpty()||!elements.insert(e.id).second||!String("rotary selector toggle action fader lever lamp needle still display tape").containsWholeWord(e.kind))return Status::invalidData;
            if(e.view.isEmpty()||!asset(e.view+":plate"))return Status::invalidData;
            if(e.rackUnit>=0&&!binding(design,e.rackUnit,e.key))return Status::invalidData;
            for(const auto& when:{e.shownWhen,e.nativeActiveWhen})if(when.unit>=0&&!binding(design,when.unit,when.key))return Status::invalidData;
            for(const auto& [role,key]:e.layers)if(!asset(key))return Status::invalidData;
            for(const auto& [state,key]:e.stateAssets)if(!asset(key))return Status::invalidData;
            if(e.knurl.isNotEmpty()&&(!asset(e.knurl)||e.knurlFrames<=0||e.knurlFrames>8192||e.knurlTeeth<=0
                ||model.asset(e.knurl).image.getHeight()%e.knurlFrames!=0||model.asset(e.knurl).image.getHeight()<e.knurlFrames))return Status::invalidData;
            for(const auto& position:e.positions)if(position.asset.isNotEmpty()&&!asset(position.asset))return Status::invalidData;
            if((e.kind=="rotary"||e.kind=="selector"||e.kind=="still")&&!asset(e.layer("pointer")))return Status::invalidData;
            if(e.kind=="fader"&&e.stateAssets.empty()&&!asset(e.layer("cap")))return Status::invalidData;
            if(e.kind=="lamp"&&!asset(e.layer("lit")))return Status::invalidData;
            if(e.kind=="needle"&&!asset(e.layer("needle")))return Status::invalidData;
            if(e.kind=="selector"||e.kind=="lever"){
                if(e.positions.empty())return Status::invalidData;
                if(e.kind=="lever")for(const auto& position:e.positions)if(!asset(position.asset))return Status::invalidData;
            }
            if(e.display["bar"].isObject()&&(!asset(e.display["bar"]["lit"].toString())||!asset(e.display["bar"]["unlit"].toString())))return Status::invalidData;
        }
        for(const auto& g:model.groups){if(g.id.isEmpty()||!groups.insert(g.id).second||!asset(g.view+":plate"))return Status::invalidData;for(const auto& id:g.members)if(!elements.contains(id))return Status::invalidData;for(const auto& id:g.focusOrder)if(!elements.contains(id))return Status::invalidData;}
        for(const auto& e:model.elements)if(e.group.isNotEmpty()&&!groups.contains(e.group))return Status::invalidData;
        if(!elements.contains(model.reveal.opener))return Status::invalidData;
        for(const auto& closer:model.reveal.closers)if(!elements.contains(closer))return Status::invalidData;
        if(model.reveal.drawnTouch.isNotEmpty()&&!elements.contains(model.reveal.drawnTouch))return Status::invalidData;
        std::set<String> pages;
        for(const auto& page:model.pages){
            if(page.id.isEmpty()||!pages.insert(page.id).second||!elements.contains(page.button)||page.screens.isEmpty())return Status::invalidData;
            for(const auto& screen:page.screens)if(std::none_of(model.elements.begin(),model.elements.end(),[&](const auto& e){return e.screen==screen;}))return Status::invalidData;
        }
        if(model.nativeFace){
            const auto& n=*model.nativeFace;if(!binding(design,n.statusWhen.unit,n.statusWhen.key)||!binding(design,n.pilotWhen.unit,n.pilotWhen.key))return Status::invalidData;
            for(const auto& b:n.controls)if(!binding(design,b.unit,b.key))return Status::invalidData;
        }
        const auto editor=doc["reveal"]["native_editor"];
        if(doc["reveal"].hasProperty("native_editor")){
            const auto* rect=editor["rect"].getArray();
            if(!editor.isObject()||editor["type"].toString()!="space_reverb"||editor["theme"].toString()!="amber_crt"||!editor["rack_unit"].isInt()||!rect||rect->size()!=4||model.reveal.kind!="drawer"||m.capabilities["nativeEditor"].toString()!="space_reverb")return Status::incompatible;
            for(const auto& value:*rect)if((!value.isInt()&&!value.isDouble())||!std::isfinite(double(value)))return Status::invalidData;
            const auto unit=std::find_if(design.units.begin(),design.units.end(),[&](const auto& u){return int(u.id)==int(editor["rack_unit"])&&u.module==NativeModule::spaceReverb;});
            const auto& plate=model.asset(model.reveal.view+":plate");const juce::Rectangle<float> area{plate.anchor.x,plate.anchor.y,float(plate.image.getWidth()),float(plate.image.getHeight())};
            const juce::Rectangle<float> region{float(double((*rect)[0])),float(double((*rect)[1])),float(double((*rect)[2])),float(double((*rect)[3]))};
            if(unit==design.units.end()||region.isEmpty()||!area.contains(region))return Status::invalidData;
        }
        return Status::ready;
    } catch(const std::exception&) { return Status::invalidData; }
}
Status content(const RackPackageManifest& m,const Tree& tree,std::string& recipe,const RackLibraryCancel& cancel,std::vector<NativeModule>* unitModules=nullptr) {
    std::uint64_t decoded=0;juce::MemoryBlock canonical;
    for(const auto& file:m.files){
        if(cancelled(cancel))return Status::cancelled;juce::MemoryBlock bytes;Status s;
        if(!tree.read(file.path,file.bytes,bytes,cancel))return cancelled(cancel)?Status::cancelled:Status::storageFailure;
        if(!validateRackPackageFileBytes(file,span(bytes),&s,&decoded))return s;
        if(file.kind=="png"){
            const auto image=juce::ImageFileFormat::loadFrom(bytes.getData(),bytes.getSize());
            if(!image.isValid())return Status::invalidData;
        }
        if(file.path==m.designPath)canonical.swapWith(bytes);
    }
    RackDesign design;Status s;const auto text=std::string_view(static_cast<const char*>(canonical.getData()),canonical.getSize());
    if(!validateRackPackageDesign(m,text,design,&s))return s;
    if(cancelled(cancel))return Status::cancelled;s=validateSkin(m,design,tree,cancel);if(s!=Status::ready)return s;
    if(unitModules){unitModules->clear();unitModules->reserve(design.units.size());for(const auto& unit:design.units)unitModules->push_back(unit.module);}
    if(cancelled(cancel))return Status::cancelled;recipe.assign(text);return Status::ready;
}
bool listedTree(const Tree& tree,const RackLibraryCancel& cancel={}) {
    for(const auto& [prefix,handle]:tree.dirs){
        if(cancelled(cancel))return false;
        WIN32_FIND_DATAW entry{};const auto pattern=native(tree.path.getChildFile(prefix))+L"\\*";HANDLE search=FindFirstFileW(pattern.c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE)return false;bool valid=true;
        do {
            if(cancelled(cancel)){valid=false;break;}const String name(entry.cFileName);if(name=="."||name=="..")continue;
            const auto relative=prefix.isEmpty()?name:prefix+"/"+name;
            if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)valid=false;
            else if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!tree.dirs.contains(relative))valid=false;}
            else if(!tree.files.contains(relative))valid=false;
        }while(FindNextFileW(search,&entry));
        const auto error=GetLastError();FindClose(search);if(!valid||error!=ERROR_NO_MORE_FILES)return false;
    }
    return true;
}
Status loadRelease(Tree& tree,const juce::File& directory,const RackPackageRef& ref,RackPackageManifest& manifest,std::string& recipe,bool deletable=false,std::vector<NativeModule>* unitModules=nullptr,const RackLibraryCancel& cancel={}) {
    if(cancelled(cancel))return Status::cancelled;
    if(!tree.root(directory,false,deletable))return Status::unavailable;
    if(!tree.add(".archive-sha256",false,deletable)||!tree.add("package.json",false,deletable))return Status::invalidData;
    juce::MemoryBlock receipt,json;
    if(!tree.read(".archive-sha256",64,receipt,cancel)||receipt.getSize()!=64||receipt.toString()!=ref.sha256)return Status::identityConflict;
    Status s;if(!tree.read("package.json",limits::manifestBytes,json,cancel)||!decodeRackPackageManifest({static_cast<const char*>(json.getData()),json.getSize()},manifest,&s))return Status::invalidData;
    if(manifest.ref.id!=ref.id||manifest.ref.version!=ref.version)return Status::identityConflict;manifest.ref=ref;
    for(const auto& file:manifest.files){if(cancelled(cancel))return Status::cancelled;if(!tree.add(file.path,false,deletable))return Status::invalidData;}
    if(!listedTree(tree,cancel))return cancelled(cancel)?Status::cancelled:Status::invalidData;
    return content(manifest,tree,recipe,cancel,unitModules);
}
Status metadata(juce::ZipFile& zip,RackPackageManifest& m,juce::MemoryBlock& json) {
    if(zip.getNumEntries()<=0)return Status::invalidData;if(zip.getNumEntries()>int(limits::fileCount))return Status::countLimit;
    std::set<String> names;std::uint64_t expanded=0;
    for(int i=0;i<zip.getNumEntries();++i){const auto* e=zip.getEntry(i);if(!e)return Status::invalidData;
        const auto mode=(e->externalFileAttributes>>16)&0170000;
        if(e->isSymbolicLink||(mode!=0&&mode!=0100000)||(e->externalFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))return Status::invalidData;
        if((e->filename!="package.json"&&!validRackPackagePath(e->filename))||!names.insert(e->filename.toLowerCase()).second)return Status::invalidData;
        if(e->uncompressedSize<0||std::uint64_t(e->uncompressedSize)>limits::expandedBytes-expanded)return Status::byteLimit;expanded+=std::uint64_t(e->uncompressedSize);
    }
    const auto* entry=zip.getEntry("package.json");if(!entry||entry->uncompressedSize<=0)return Status::invalidData;if(std::uint64_t(entry->uncompressedSize)>limits::manifestBytes)return Status::byteLimit;
    std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(*entry));if(!stream)return Status::invalidData;
    json.setSize(size_t(entry->uncompressedSize));if(stream->read(json.getData(),int(json.getSize()))!=int(json.getSize()))return Status::invalidData;
    char extra;if(stream->read(&extra,1)!=0)return Status::invalidData;Status s;
    if(!decodeRackPackageManifest({static_cast<const char*>(json.getData()),json.getSize()},m,&s))return s;
    if(m.files.size()+1!=size_t(zip.getNumEntries()))return Status::invalidData;
    for(const auto& file:m.files){const auto* e=zip.getEntry(file.path);if(!e||e->uncompressedSize<0||std::uint64_t(e->uncompressedSize)!=file.bytes)return Status::invalidData;}
    return Status::ready;
}
Status extract(juce::ZipFile& zip,const RackPackageManifest& m,const juce::MemoryBlock& json,Tree& stage,const RackLibraryCancel& cancel) {
    if(!stage.write("package.json",json.getData(),json.getSize()))return Status::storageFailure;
    std::array<char,131072> chunk{};
    for(const auto& file:m.files){
        if(cancelled(cancel))return Status::cancelled;
        if(!stage.add(file.path,true))return Status::storageFailure;
        std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(*zip.getEntry(file.path)));if(!in)return Status::invalidData;
        std::uint64_t written=0;while(written<file.bytes){
            if(cancelled(cancel))return Status::cancelled;const auto count=int(std::min<std::uint64_t>(chunk.size(),file.bytes-written));
            const auto got=in->read(chunk.data(),count);if(got<=0)return Status::invalidData;
            if(!writeBytes(stage.files.at(file.path).value,chunk.data(),size_t(got)))return Status::storageFailure;written+=std::uint64_t(got);
        }
        char extra;if(in->read(&extra,1)!=0)return Status::invalidData;
        if(!FlushFileBuffers(stage.files.at(file.path).value))return Status::storageFailure;
    }
    return Status::ready;
}
juce::File releasePath(const juce::File& root,const RackPackageRef& ref) { return root.getChildFile("packages").getChildFile(ref.id).getChildFile(ref.version); }
juce::File releaseLock(const juce::File& root,const RackPackageRef& ref) { return root.getChildFile("locks").getChildFile(ref.id+"-"+ref.version+".lock"); }
bool finishRemoval(Tree& tree,const RackPackageManifest* manifest,const RackLibraryCancel& cancel={},bool interruptible=false) {
    // Keep recovery metadata until all listed data and empty subdirectories are gone.
    if(manifest)for(const auto& file:manifest->files){
        if(const auto found=tree.files.find(file.path);found!=tree.files.end()&&!dispose(found->second))return false;
        if(cancelled(cancel)&&interruptible)return false;
    }
    for(auto it=tree.dirOrder.rbegin();it!=tree.dirOrder.rend();++it){if(cancelled(cancel)&&interruptible)return false;if(it->isNotEmpty()&&!dispose(tree.dirs.at(*it)))return false;}
    for(const auto* name:{"package.json",".archive-sha256"})if(const auto found=tree.files.find(name);found!=tree.files.end()){
        if(!dispose(found->second))return false;if(cancelled(cancel)&&interruptible)return false;
    }
    tree.cleanup=false;return dispose(tree.dirs.at(""));
}
// A failed publish has already closed the child handles. Reopen only the
// expected tree under the still-pinned stage root, and delete only after the
// receipt, manifest, inventory and contents all validate again. An unavailable
// or changed child leaves the uncommitted stage for later inspection.
void cleanupFailedStage(Tree& stage,const RackPackageManifest& manifest,const juce::MemoryBlock& originalJson,const RackPackageRef& ref,const RackLibraryCancel& cancel) {
    if(cancelled(cancel))return;
    if(!stage.add(".archive-sha256",false,true)||!stage.add("package.json",false,true))return;
    juce::MemoryBlock receipt,json;
    if(!stage.read(".archive-sha256",64,receipt,cancel)||receipt.getSize()!=64||receipt.toString()!=ref.sha256
       ||!stage.read("package.json",limits::manifestBytes,json,cancel)||json!=originalJson)return;
    for(const auto& file:manifest.files)if(cancelled(cancel)||!stage.add(file.path,false,true))return;
    if(!listedTree(stage,cancel))return;
    std::string recipe;if(content(manifest,stage,recipe,cancel)!=Status::ready)return;
    // On disposition failure, leave any remainder uncommitted and report the
    // original storage failure; never delete by an unchecked recursive path.
    (void)finishRemoval(stage,&manifest,cancel,true);
}
Status cleanupCommitted(const juce::File& trash,const RackPackageRef& ref,const RackLibraryCancel& cancel) {
    if(cancelled(cancel))return Status::cancelled;
    Tree tree;if(!tree.root(trash,false,true))return Status::unavailable;
    const auto missing=[] { const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND; };
    const bool hasManifest=tree.add("package.json",false,true);if(!hasManifest&&!missing())return Status::unavailable;
    const bool hasReceipt=tree.add(".archive-sha256",false,true);if(!hasReceipt&&!missing())return Status::unavailable;
    juce::MemoryBlock bytes;
    if(hasReceipt&&(!tree.read(".archive-sha256",64,bytes,cancel)||bytes.getSize()!=64||bytes.toString()!=ref.sha256))return Status::identityConflict;
    if(!hasManifest){
        // A crash after metadata deletion can leave only the receipt or an empty owned directory.
        WIN32_FIND_DATAW entry{};const auto pattern=native(trash)+L"\\*";HANDLE search=FindFirstFileW(pattern.c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE)return Status::unavailable;bool clean=true;
        do {const String name(entry.cFileName);if(name!="."&&name!=".."&&name!=".archive-sha256")clean=false;}while(FindNextFileW(search,&entry));
        const auto error=GetLastError();FindClose(search);if(!clean||error!=ERROR_NO_MORE_FILES)return Status::unavailable;
        return finishRemoval(tree,nullptr,cancel,true)?Status::ready:cancelled(cancel)?Status::cancelled:Status::storageFailure;
    }
    if(!hasReceipt)return Status::unavailable;
    RackPackageManifest manifest;Status s;
    if(!tree.read("package.json",limits::manifestBytes,bytes,cancel)||!decodeRackPackageManifest({static_cast<const char*>(bytes.getData()),bytes.getSize()},manifest,&s)
        ||manifest.ref.id!=ref.id||manifest.ref.version!=ref.version)return Status::invalidData;
    for(const auto& file:manifest.files){
        if(cancelled(cancel))return Status::cancelled;if(!tree.add(file.path,false,true)){if(missing())continue;return Status::unavailable;}
        if(!tree.read(file.path,file.bytes,bytes,cancel)||bytes.getSize()!=file.bytes||juce::SHA256(bytes).toHexString()!=file.sha256)return Status::invalidData;
    }
    if(!listedTree(tree,cancel))return cancelled(cancel)?Status::cancelled:Status::invalidData;
    return finishRemoval(tree,&manifest,cancel,true)?Status::ready:cancelled(cancel)?Status::cancelled:Status::storageFailure;
}
Status writeRemoval(const juce::File& root,const RackPackageRef& ref,std::uint64_t generation,const String& trash) {
    const auto json=juce::JSON::toString(object({{"schema",1},{"ref",refJson(ref)},{"generation",String(std::to_string(generation))},{"trash",trash}}),true);
    auto file=openFile(root.getChildFile("removal.json"),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,CREATE_NEW);
    if(!file)return Status::storageFailure;
    if(!writeBytes(file.value,json.toRawUTF8(),json.getNumBytesAsUTF8())||!FlushFileBuffers(file.value)){dispose(file);return Status::storageFailure;}
    return Status::ready;
}
// The index decides rollback versus cleanup. A journal never supplies a filesystem path.
Status recoverRemoval(const juce::File& root,const Snapshot& snapshot,const RackLibraryCancel& cancel={}) {
    if(cancelled(cancel))return Status::cancelled;
    auto journal=openFile(root.getChildFile("removal.json"),GENERIC_READ|DELETE,FILE_SHARE_READ);
    if(!journal)return GetLastError()==ERROR_FILE_NOT_FOUND?Status::ready:Status::unavailable;
    juce::MemoryBlock bytes;juce::var doc;RackPackageRef ref;std::uint64_t generation=0;
    if(!readBytes(journal.value,limits::manifestBytes,bytes,cancel)||!ai::parseStrictJson({static_cast<const char*>(bytes.getData()),bytes.getSize()},doc,limits::manifestBytes)
        ||!closed(doc,4)||!doc["schema"].isInt()||int(doc["schema"])!=1||!parseRef(doc["ref"],ref)||!doc["generation"].isString()||!number(doc["generation"].toString(),generation)||!doc["trash"].isString())return Status::unavailable;
    const auto trashName=doc["trash"].toString();const auto suffix=trashName.substring(7);
    if(!trashName.startsWith("remove-")||suffix.length()!=32||!suffix.containsOnly("0123456789abcdef"))return Status::unavailable;
    const bool member=std::find(snapshot.releases.begin(),snapshot.releases.end(),ref)!=snapshot.releases.end();
    if(!snapshot.exists||(member?snapshot.generation!=generation:(generation==UINT64_MAX||snapshot.generation!=generation+1)))return Status::unavailable;
    FileLock release(releaseLock(root,ref),true);if(!release.acquired)return release.file?Status::busy:Status::storageFailure;
    const auto original=releasePath(root,ref),trash=root.getChildFile("staging").getChildFile(trashName);
    if(member){
        if(original.exists()==trash.exists())return Status::unavailable;
        Tree tree;RackPackageManifest manifest;std::string recipe;auto s=loadRelease(tree,original.exists()?original:trash,ref,manifest,recipe,!original.exists(),nullptr,cancel);if(s!=Status::ready)return s;
        if(!original.exists()){
            tree.closeChildren();if(!renameHandle(tree.dirs.at("").value,original))return Status::storageFailure;tree.dirs.at("").reset();
            Tree verified;RackPackageManifest restored;std::string restoredRecipe;s=loadRelease(verified,original,ref,restored,restoredRecipe,false,nullptr,cancel);if(s!=Status::ready)return s;
        }
    } else {
        if(original.exists())return Status::unavailable;
        if(trash.exists()){
            const auto s=cleanupCommitted(trash,ref,cancel);if(s!=Status::ready)return s;
        }
    }
    if(cancelled(cancel))return Status::cancelled;return dispose(journal)?Status::ready:Status::storageFailure;
}
Status refreshIndex(const juce::File& root,Snapshot& snapshot,bool writerHeld=false,const RackLibraryCancel& cancel={}) {
    if(cancelled(cancel))return Status::cancelled;auto candidate=snapshot;auto s=readIndex(root,candidate,cancel);if(s!=Status::ready)return s;
    if(writerHeld){s=recoverRemoval(root,candidate,cancel);if(s==Status::ready)snapshot=std::move(candidate);return s;}
    if(!root.getChildFile("removal.json").exists()){snapshot=std::move(candidate);return Status::ready;}
    std::vector<Handle> held;if(!directories(root,false,held))return Status::unavailable;
    FileLock writer(root.getChildFile("writer.lock"),true);if(!writer.acquired)return writer.file?Status::busy:Status::storageFailure;
    s=readIndex(root,candidate,cancel);if(s==Status::ready)s=recoverRemoval(root,candidate,cancel);if(s==Status::ready)snapshot=std::move(candidate);return s;
}
Status selectRelease(Snapshot& next,const RackPackageRef& expected,RackPackageActivation activation,bool& changed) {
    const auto existing=std::find_if(next.releases.begin(),next.releases.end(),[&](const auto& ref){return identity(ref)==identity(expected);});
    if(existing!=next.releases.end()&&*existing!=expected)return Status::identityConflict;
    changed=existing==next.releases.end();
    if(changed){
        if(std::count_if(next.releases.begin(),next.releases.end(),[&](const auto& r){return r.id==expected.id;})>=int(limits::releasesPerRack))return Status::countLimit;
        next.releases.push_back(expected);
    }
    if(activation==RackPackageActivation::makeActive){
        const auto active=std::find_if(next.active.begin(),next.active.end(),[&](const auto& r){return r.id==expected.id;});
        if(active==next.active.end()){if(next.active.size()>=limits::installedCount)return Status::countLimit;next.active.push_back(expected);changed=true;}
        else if(*active!=expected){*active=expected;changed=true;}
    }
    return Status::ready;
}
}
struct RackPackageLease::Impl {
    std::unique_ptr<FileLock> lock;Tree tree;RackPackageManifest manifest;std::string recipe;std::vector<NativeModule> modules;mutable std::mutex mutex;
};
struct RackPackageStore::Impl {
    juce::File root;mutable std::timed_mutex mutex;mutable Snapshot snapshot;mutable std::atomic<std::uint64_t> observed{0};
    explicit Impl(RackLibraryPaths p):root(p.root) {}
    Status refresh(bool writerHeld=false,const RackLibraryCancel& cancel={}){const auto status=refreshIndex(root,snapshot,writerHeld,cancel);if(status==Status::ready)observed=snapshot.generation;return status;}
};
#if defined(_DEBUG) || defined(AIRACK_STORE_TESTS)
namespace {
std::optional<juce::File> mcpTestRoot(const juce::String& value) {
    if (testing::fullyQualifiedMcpTestPath(value))
        return juce::File(value).getChildFile("Rack Library");
    return {};
}
}
#if defined(AIRACK_STORE_TESTS)
namespace testing {
std::optional<juce::File> mcpTestRackLibraryRoot(const juce::String& value) { return mcpTestRoot(value); }
}
#endif
#endif
RackLibraryPaths RackLibraryPaths::forCurrentUser() {
#if defined(_DEBUG)
    SetLastError(ERROR_SUCCESS);
    const auto overrideSize=GetEnvironmentVariableW(L"AIRACK_MCP_TEST_RACK_ROOT",nullptr,0);
    if(overrideSize!=0 || GetLastError()!=ERROR_ENVVAR_NOT_FOUND) {
        if(auto root=mcpTestRoot(juce::SystemStats::getEnvironmentVariable("AIRACK_MCP_TEST_RACK_ROOT",{})))return {*root};
        return {}; // An explicitly supplied malformed override must never reach the user store.
    }
#endif
    PWSTR path=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&path)))return {};
    const juce::File local{String(path)};CoTaskMemFree(path);return {local.getChildFile("AI Rack Synth/Rack Library")};
}
RackPackageLease::RackPackageLease(std::unique_ptr<Impl> value):impl(std::move(value)) {}
RackPackageLease::~RackPackageLease()=default;
const RackPackageManifest& RackPackageLease::manifest() const { return impl->manifest; }
std::string_view RackPackageLease::canonicalRecipe() const { return impl->recipe; }
std::span<const NativeModule> RackPackageLease::unitModules() const { return impl->modules; }
Status RackPackageLease::readListed(const String& path,juce::MemoryBlock& output) const {
    const auto* file=listed(impl->manifest,path);if(!file)return Status::invalidData;
    std::lock_guard guard(impl->mutex);juce::MemoryBlock bytes;Status s;
    if(!impl->tree.read(path,file->bytes,bytes))return Status::storageFailure;
    if(!validateRackPackageFileBytes(*file,span(bytes),&s))return s;output.swapWith(bytes);return Status::ready;
}
RackPackageStore::RackPackageStore(RackLibraryPaths p):impl(std::make_shared<Impl>(std::move(p))) {}
RackPackageStore::RackPackageStore(std::shared_ptr<Impl> value):impl(std::move(value)) {}
RackPackageStore::~RackPackageStore()=default;
Status RackPackageStore::install(const juce::File& archive,const RackPackageRef& expected,RackPackageActivation activation,const RackLibraryCancel& cancel) {
    if(!validRef(expected))return Status::invalidData;if(impl->root==juce::File())return Status::storageFailure;if(cancelled(cancel))return Status::cancelled;
    std::vector<Handle> sourceDirs;if(!directories(archive.getParentDirectory(),false,sourceDirs))return Status::storageFailure;
    auto source=openFile(archive);if(!source)return Status::storageFailure;
    if(length(source.value)>limits::archiveBytes)return Status::byteLimit;
    HeldInput input(source.value);const auto sha=juce::SHA256(input).toHexString();if(input.failed)return Status::storageFailure;
    if(sha!=expected.sha256)return Status::identityConflict;if(cancelled(cancel))return Status::cancelled;
    input.setPosition(0);juce::ZipFile zip(input);RackPackageManifest manifest;juce::MemoryBlock json;auto s=metadata(zip,manifest,json);if(s!=Status::ready)return s;
    if(manifest.ref.id!=expected.id||manifest.ref.version!=expected.version)return Status::identityConflict;manifest.ref=expected;
    Tree stage;if(!stage.root(impl->root.getChildFile("staging").getChildFile("install-"+juce::Uuid().toString()),true,true))return Status::storageFailure;
    s=extract(zip,manifest,json,stage,cancel);if(s!=Status::ready)return s;
    std::string recipe;s=content(manifest,stage,recipe,cancel);if(s!=Status::ready)return s;
    if(!stage.write(".archive-sha256",expected.sha256.toRawUTF8(),64))return Status::storageFailure;
    if(cancelled(cancel))return Status::cancelled;
    std::vector<Handle> finalDirs;if(!directories(releasePath(impl->root,expected).getParentDirectory(),true,finalDirs))return Status::storageFailure;
    std::vector<Handle> lockDirs;if(!directories(impl->root.getChildFile("locks"),true,lockDirs))return Status::storageFailure;
    const auto final=releasePath(impl->root,expected);
    std::unique_ptr<FileLock> release;
    {
        std::lock_guard guard(impl->mutex);FileLock writer(impl->root.getChildFile("writer.lock"),true);if(!writer.acquired)return writer.file?Status::busy:Status::storageFailure;
        s=impl->refresh(true);if(s!=Status::ready)return s;
        auto admitted=impl->snapshot;bool changed=false;s=selectRelease(admitted,expected,activation,changed);if(s!=Status::ready)return s;
        release=std::make_unique<FileLock>(releaseLock(impl->root,expected),false);if(!release->acquired)return release->file?Status::busy:Status::storageFailure;
        if(!final.exists()){
            if(std::find(impl->snapshot.releases.begin(),impl->snapshot.releases.end(),expected)!=impl->snapshot.releases.end())return Status::unavailable;
            stage.cleanup=false;stage.closeChildren();
#if defined(AIRACK_STORE_TESTS)
            const bool oldFailure=testing::consumeStoreRenameFailure();
            if((oldFailure&&std::exchange(testing::holdStageChild,false))||(testing::renameTrial&&testing::renameTrial->holdChild))
                testing::heldStageChild=CreateFileW(native(stage.path.getChildFile("nested/deep/notice.txt")).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
#endif
            std::chrono::steady_clock::time_point failedAt;
            const auto publish=[&](DWORD& error) {
#if defined(AIRACK_STORE_TESTS)
                if(oldFailure){error=ERROR_SHARING_VIOLATION;failedAt=std::chrono::steady_clock::now();return false;}
                if(auto* trial=testing::renameTrial){const int attempt=trial->attempts++;if(attempt<2&&trial->errors[attempt]>=0){error=DWORD(trial->errors[attempt]);failedAt=std::chrono::steady_clock::now();return false;}}
#endif
                return renameHandle(stage.dirs.at("").value,final,false,&error,&failedAt);
            };
            DWORD error=0;bool moved=publish(error);
            if(!moved&&error==ERROR_ACCESS_DENIED){
#if defined(AIRACK_STORE_TESTS)
                if(testing::renameTrial&&testing::renameTrial->event)testing::renameTrial->event(0,final);
#endif
                const auto start=failedAt;
                const auto elapsed=[&] {
#if defined(AIRACK_STORE_TESTS)
                    if(testing::renameTrial&&testing::renameTrial->virtualTime)return std::chrono::milliseconds(testing::renameTrial->elapsedMs);
#endif
                    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start);
                };
                while(!cancelled(cancel)&&elapsed()<std::chrono::milliseconds(50)){
#if defined(AIRACK_STORE_TESTS)
                    if(auto* trial=testing::renameTrial){++trial->waits;if(trial->event)trial->event(1,final);if(trial->virtualTime){trial->elapsedMs+=5;continue;}}
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
#if defined(AIRACK_STORE_TESTS)
                if(testing::renameTrial&&testing::renameTrial->event)testing::renameTrial->event(2,final);
#endif
                if(!cancelled(cancel)&&elapsed()<std::chrono::milliseconds(100))moved=publish(error);
            }
            if(!moved){cleanupFailedStage(stage,manifest,json,expected,cancel);return cancelled(cancel)?Status::cancelled:Status::storageFailure;}
            stage.dirs.at("").reset();
#if defined(AIRACK_STORE_TESTS)
            if(testing::renameTrial&&testing::renameTrial->event)testing::renameTrial->event(3,final);
#endif
        }
    }
    Tree verified;RackPackageManifest old;std::string oldRecipe;s=loadRelease(verified,final,expected,old,oldRecipe);
    if(s!=Status::ready)return final.isDirectory()?s:Status::storageFailure;
    juce::MemoryBlock finalManifest;if(!verified.read("package.json",limits::manifestBytes,finalManifest)||finalManifest!=json)return Status::identityConflict;
    std::lock_guard guard(impl->mutex);FileLock writer(impl->root.getChildFile("writer.lock"),true);if(!writer.acquired)return writer.file?Status::busy:Status::storageFailure;
    s=impl->refresh(true);if(s!=Status::ready)return s;auto next=impl->snapshot;
    bool changed=false;s=selectRelease(next,expected,activation,changed);if(s!=Status::ready)return s;
    if(cancelled(cancel))return Status::cancelled;
    if(!changed)return Status::ready;
    s=commitIndex(impl->root,next);if(s==Status::ready){impl->snapshot=std::move(next);impl->observed=impl->snapshot.generation;}return s;
}
Status RackPackageStore::remove(const RackPackageRef& ref,const RackLibraryCancel& cancel) {
    if(!validRef(ref))return Status::invalidData;if(impl->root==juce::File())return Status::unavailable;if(cancelled(cancel))return Status::cancelled;
    std::vector<Handle> held;std::unique_ptr<FileLock> release;Status s;
    {
        std::lock_guard guard(impl->mutex);bool missing=false;
        if(!directories(impl->root,false,held,&missing))return missing&&!impl->snapshot.exists?Status::ready:Status::unavailable;
        FileLock writer(impl->root.getChildFile("writer.lock"),true);if(!writer.acquired)return writer.file?Status::busy:Status::storageFailure;
        s=impl->refresh(true);if(s!=Status::ready)return s;
        const auto it=std::find_if(impl->snapshot.releases.begin(),impl->snapshot.releases.end(),[&](const auto& r){return identity(r)==identity(ref);});
        if(it==impl->snapshot.releases.end())return Status::ready;if(*it!=ref)return Status::identityConflict;
        release=std::make_unique<FileLock>(releaseLock(impl->root,ref),true);if(!release->acquired)return release->file?Status::busy:Status::storageFailure;
    }
    Tree tree;RackPackageManifest manifest;std::string recipe;s=loadRelease(tree,releasePath(impl->root,ref),ref,manifest,recipe,true);if(s!=Status::ready)return s;
    juce::MemoryBlock originalManifest;if(!tree.read("package.json",limits::manifestBytes,originalManifest))return Status::storageFailure;
    std::vector<Handle> trashDirs;if(!directories(impl->root.getChildFile("staging"),true,trashDirs))return Status::storageFailure;
    const auto trashName="remove-"+juce::Uuid().toString();const auto trash=impl->root.getChildFile("staging").getChildFile(trashName);
    Snapshot next;
    {
        std::lock_guard guard(impl->mutex);FileLock writer(impl->root.getChildFile("writer.lock"),true);if(!writer.acquired)return writer.file?Status::busy:Status::storageFailure;
        s=impl->refresh(true);if(s!=Status::ready)return s;next=impl->snapshot;
        if(std::find(next.releases.begin(),next.releases.end(),ref)==next.releases.end())return Status::unavailable;
        if(cancelled(cancel))return Status::cancelled;
        s=writeRemoval(impl->root,ref,next.generation,trashName);if(s!=Status::ready)return s;
        tree.closeChildren();if(!renameHandle(tree.dirs.at("").value,trash)){
            tree.dirs.at("").reset();auto journal=openFile(impl->root.getChildFile("removal.json"),GENERIC_READ|DELETE);if(journal)dispose(journal);return Status::storageFailure;}
        tree.dirs.at("").reset();
    }
    const bool cancelBeforeCommit=cancelled(cancel);
    Tree verified;RackPackageManifest trashManifest;std::string trashRecipe;
    s=loadRelease(verified,trash,ref,trashManifest,trashRecipe,true);juce::MemoryBlock finalManifest;
    if(s==Status::ready&&(!verified.read("package.json",limits::manifestBytes,finalManifest)||finalManifest!=originalManifest))s=Status::identityConflict;
    std::lock_guard guard(impl->mutex);FileLock writer(impl->root.getChildFile("writer.lock"),true);if(!writer.acquired)return writer.file?Status::busy:Status::storageFailure;
    if(s==Status::ready){s=readIndex(impl->root,impl->snapshot);if(s==Status::ready&&impl->snapshot.generation!=next.generation)s=Status::unavailable;}
    if(s==Status::ready){std::erase(next.releases,ref);std::erase(next.active,ref);s=(cancelBeforeCommit||cancelled(cancel))?Status::cancelled:commitIndex(impl->root,next);}
    if(s!=Status::ready){verified.closeChildren();if(const auto root=verified.dirs.find("");root!=verified.dirs.end())root->second.reset();release.reset();const auto recovered=recoverRemoval(impl->root,impl->snapshot);return recovered==Status::ready?s:recovered;}
    impl->snapshot=std::move(next);
    impl->observed=impl->snapshot.generation;
    // Publication has committed; interruption here is completed from the journal on the next operation.
    cancelled(cancel);
    if(!finishRemoval(verified,&trashManifest,cancel))return Status::storageFailure;
    auto journal=openFile(impl->root.getChildFile("removal.json"),GENERIC_READ|DELETE);return journal&&dispose(journal)?Status::ready:Status::storageFailure;
}
namespace {
bool acquireStore(std::unique_lock<std::timed_mutex>& lock,const RackLibraryCancel& cancel){
    while(!cancelled(cancel))if(lock.try_lock_for(std::chrono::milliseconds(5)))return !cancelled(cancel);
    return false;
}
}
RackPackageLeasePtr RackPackageStore::open(const RackPackageRef& ref,Status* status,const RackLibraryCancel& cancel) const {
    if(cancelled(cancel)){statusTo(status,Status::cancelled);return {};}
    if(!validRef(ref)){statusTo(status,Status::invalidData);return {};}
    auto lease=std::make_unique<RackPackageLease::Impl>();
    const auto failed=[&](Status s)->RackPackageLeasePtr{statusTo(status,cancelled(cancel)?Status::cancelled:s);return {};};
    {
        std::unique_lock guard(impl->mutex,std::defer_lock);if(!acquireStore(guard,cancel))return failed(Status::cancelled);
        auto s=impl->refresh(false,cancel);if(s!=Status::ready)return failed(s);
        std::vector<Handle> dirs;if(!directories(impl->root,false,dirs))return failed(Status::unavailable);
        FileLock writer(impl->root.getChildFile("writer.lock"),true);if(!writer.acquired)return failed(writer.file?Status::busy:Status::storageFailure);
        s=impl->refresh(true,cancel);if(s!=Status::ready)return failed(s);
        impl->observed=impl->snapshot.generation;
        const auto it=std::find_if(impl->snapshot.releases.begin(),impl->snapshot.releases.end(),[&](const auto& r){return identity(r)==identity(ref);});
        if(it==impl->snapshot.releases.end()||*it!=ref)return failed(it==impl->snapshot.releases.end()?Status::unavailable:Status::identityConflict);
        lease->lock=std::make_unique<FileLock>(releaseLock(impl->root,ref),false);if(!lease->lock->acquired)return failed(Status::busy);
    }
    const auto s=loadRelease(lease->tree,releasePath(impl->root,ref),ref,lease->manifest,lease->recipe,false,&lease->modules,cancel);
    if(s!=Status::ready||cancelled(cancel))return failed(s);statusTo(status,Status::ready);return RackPackageLeasePtr(new RackPackageLease(std::move(lease)));
}
std::vector<RackPackageRef> RackPackageStore::active(Status* status,const RackLibraryCancel& cancel) const {
    std::unique_lock guard(impl->mutex,std::defer_lock);if(!acquireStore(guard,cancel)){statusTo(status,Status::cancelled);return {};}
    const auto s=impl->refresh(false,cancel);statusTo(status,cancelled(cancel)?Status::cancelled:s);
    if(s==Status::ready)impl->observed=impl->snapshot.generation;
    return cancelled(cancel)?std::vector<RackPackageRef>{}:impl->snapshot.active;
}
std::uint64_t RackPackageStore::generation(Status* status,const RackLibraryCancel& cancel) const {
    std::unique_lock guard(impl->mutex,std::defer_lock);if(!acquireStore(guard,cancel)){statusTo(status,Status::cancelled);return 0;}
    const auto s=impl->refresh(false,cancel);statusTo(status,cancelled(cancel)?Status::cancelled:s);if(s==Status::ready)impl->observed=impl->snapshot.generation;return impl->snapshot.generation;
}
std::vector<RackPackageRef> RackPackageStore::committed(Status* status,const RackLibraryCancel& cancel) const {
    std::unique_lock guard(impl->mutex,std::defer_lock);if(!acquireStore(guard,cancel)){statusTo(status,Status::cancelled);return {};}
    const auto s=impl->refresh(false,cancel);statusTo(status,cancelled(cancel)?Status::cancelled:s);
    if(s==Status::ready)impl->observed=impl->snapshot.generation;
    return s==Status::ready&&!cancelled(cancel)?impl->snapshot.releases:std::vector<RackPackageRef>{};
}
std::uint64_t RackPackageStore::observedGeneration() const noexcept{return impl->observed.load();}
RackLibraryPaths RackPackageStore::paths() const { return {impl->root}; }
}
