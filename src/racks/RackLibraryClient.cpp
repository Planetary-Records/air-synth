#include "RackLibraryClient.h"
#include "StrictJson.h"
#include "NativePreviewData.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <windows.h>
#include <winternl.h>
namespace airack {
#if AIRACK_LIBRARY_CACHE_TESTS
namespace {std::mutex cacheTestMutex;std::function<void(const juce::File&,const juce::String&)> cacheTestHook;
std::function<void(std::vector<RackInstance>,std::optional<RackInstance>,std::vector<RackInstance>,bool,bool)> skinTestHook;
std::function<void(std::uint64_t,unsigned,bool,bool,bool)> inventoryTestHook;
std::function<void(unsigned,bool,bool,bool)> thumbnailAcquisitionTestHook;}
void setRackLibraryCacheTestHook(std::function<void(const juce::File&,const juce::String&)> hook){const std::lock_guard lock(cacheTestMutex);cacheTestHook=std::move(hook);}
void setRackLibrarySkinTestHook(std::function<void(std::vector<RackInstance>,std::optional<RackInstance>,std::vector<RackInstance>,bool,bool)> hook){const std::lock_guard lock(cacheTestMutex);skinTestHook=std::move(hook);}
void setRackLibraryInventoryTestHook(std::function<void(std::uint64_t,unsigned,bool,bool,bool)> hook){const std::lock_guard lock(cacheTestMutex);inventoryTestHook=std::move(hook);}
void setRackLibraryThumbnailAcquisitionTestHook(std::function<void(unsigned,bool,bool,bool)> hook){const std::lock_guard lock(cacheTestMutex);thumbnailAcquisitionTestHook=std::move(hook);}
#endif
namespace {
using juce::String;using juce::var;using Status=RackLibraryStatus;
namespace limits=rackPackageLimits;
var object(std::initializer_list<std::pair<juce::Identifier,var>> fields){auto* d=new juce::DynamicObject;for(const auto& [k,v]:fields)d->setProperty(k,v);return d;}
bool closed(const var& value,std::initializer_list<const char*> fields){const auto* d=value.getDynamicObject();return d&&d->getProperties().size()==int(fields.size())&&std::all_of(fields.begin(),fields.end(),[&](auto* f){return d->hasProperty(f);});}
bool quantity(const var& v,std::uint64_t cap,std::uint64_t& out){if(!v.isInt()&&!v.isInt64())return false;const auto n=juce::int64(v);if(n<0||std::uint64_t(n)>cap)return false;out=std::uint64_t(n);return true;}
bool decimal(const var& v,std::uint64_t& out,bool zero=false){if(!v.isString())return false;const auto s=v.toString().toStdString();if(s.empty()||(!zero&&s[0]=='0')||(s.size()>1&&s[0]=='0'))return false;const auto r=std::from_chars(s.data(),s.data()+s.size(),out);return r.ec==std::errc{}&&r.ptr==s.data()+s.size();}
bool headerNumber(const String& value,std::uint64_t& out){const auto text=value.trim();if(text.isEmpty()||text.getNumBytesAsUTF8()>64||!text.containsOnly("0123456789"))return false;std::uint64_t n=0;for(auto c:text){const auto digit=std::uint64_t(c-'0');if(n>(UINT64_MAX-digit)/10)return false;n=n*10+digit;}out=n;return true;}
var number(std::uint64_t n){return String(std::to_string(n));}
bool text(const var& v,std::size_t cap,bool empty=false){if(!v.isString())return false;const auto s=v.toString();if((!empty&&s.trim().isEmpty())||s.getNumBytesAsUTF8()>cap)return false;for(auto c:s)if(c==0||(c<32&&c!='\n'&&c!='\t'))return false;return true;}
std::array<unsigned,3> semver(const String& s){const auto p=juce::StringArray::fromTokens(s,".","");return {unsigned(p[0].getIntValue()),unsigned(p[1].getIntValue()),unsigned(p[2].getIntValue())};}
bool metadata(const var& v,RackMetadata& m){
    if(!closed(v,{"design","revision","canonical_sha256","name","maker","purpose","description","role","tags"})||!v["design"].isString()||!decimal(v["revision"],m.revision)
        ||!v["canonical_sha256"].isString()||!validRackPackageHash(v["canonical_sha256"].toString())||!text(v["name"],limits::nameBytes)||!text(v["maker"],limits::makerBytes)
        ||!text(v["purpose"],limits::purposeBytes)||!text(v["description"],limits::descriptionBytes,true)||!v["role"].isString())return false;
    m.design=juce::Uuid(v["design"].toString());if(m.design.isNull()||m.design.toDashedString()!=v["design"].toString())return false;
    const auto role=v["role"].toString();if(role!="synth"&&role!="fx")return false;m.role=role=="synth"?RackRole::synth:RackRole::fx;
    m.canonicalSha256=v["canonical_sha256"].toString();m.name=v["name"].toString();m.maker=v["maker"].toString();m.purpose=v["purpose"].toString();m.description=v["description"].toString();
    const auto* tags=v["tags"].getArray();if(!tags||tags->size()>int(limits::tagCount))return false;
    for(const auto& t:*tags){if(!t.isString()||!validRackTagId(t.toString())||m.tags.contains(t.toString()))return false;m.tags.add(t.toString());}return true;
}
bool sameMetadata(const RackMetadata& a,const RackMetadata& b){
    auto at=a.tags,bt=b.tags;at.sort(false);bt.sort(false);
    return a.design==b.design&&a.revision==b.revision&&a.canonicalSha256==b.canonicalSha256&&a.name==b.name&&a.maker==b.maker&&a.purpose==b.purpose&&a.description==b.description&&a.role==b.role&&at==bt;
}
bool sameCapabilities(const var& a,const var& b){
    if(a["airMin"].toString()!=b["airMin"].toString()||a["nativeEditor"].toString()!=b["nativeEditor"].toString())return false;
    const auto modules=[](const var& value){std::set<std::pair<int,int>> result;for(const auto& module:*value["modules"].getArray())result.emplace(int(module["module"]),int(module["algorithmRevision"]));return result;};
    return modules(a)==modules(b);
}
bool capabilities(const var& v,bool& compatible){
    if(!closed(v,{"airMin","modules","nativeEditor"})||!v["airMin"].isString()||!validRackPackageVersion(v["airMin"].toString())||!text(v["nativeEditor"],64,true))return false;
    compatible=semver(v["airMin"].toString())<=std::array<unsigned,3>{0,1,0};const auto editor=v["nativeEditor"].toString();
    if(!editor.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789_"))return false;
    if(editor.isNotEmpty()&&editor!="space_reverb")compatible=false;
    const auto* modules=v["modules"].getArray();if(!modules||modules->isEmpty()||modules->size()>9)return false;
    std::set<std::pair<unsigned,unsigned>> seen;bool space=false;
    for(const auto& module:*modules){std::uint64_t m=0,a=0;if(!closed(module,{"module","algorithmRevision"})||!quantity(module["module"],255,m)||!quantity(module["algorithmRevision"],255,a)||!seen.emplace(unsigned(m),unsigned(a)).second)return false;
        compatible&=m<=unsigned(NativeModule::saturation)&&supportsAlgorithm(NativeModule(m),AlgorithmRevision(a));space|=m==unsigned(NativeModule::spaceReverb);}
    return editor!="space_reverb"||space;
}
struct Catalog {std::vector<RackTag> tags;std::vector<RackRemoteRelease> releases;std::map<String,var> raw;};
String identity(const RackPackageRef& r){return r.id+"@"+r.version;}
Status parseCatalog(std::string_view bytes,Catalog& out,const Catalog* prior=nullptr){
    if(bytes.size()>limits::catalogBytes)return Status::byteLimit;var root;if(!ai::parseStrictJson(bytes,root,limits::catalogBytes)||!closed(root,{"schema","tags","racks"})||!root["schema"].isInt()||int(root["schema"])!=1)return Status::invalidData;
    Catalog next;const auto tagJson=juce::JSON::toString(object({{"schema",1},{"tags",root["tags"]}}),true).toStdString();Status status;
    if(!validateRackTagDictionary(tagJson,next.tags,&status))return status;std::set<String> tagIds;for(const auto& t:next.tags)tagIds.insert(t.id);
    const auto* racks=root["racks"].getArray();if(!racks)return Status::invalidData;if(racks->size()>int(limits::installedCount))return Status::countLimit;
    std::set<String> ids;std::set<std::uint64_t> orders;std::map<std::uint64_t,String> assetHashes;
    std::vector<std::pair<std::uint64_t,std::vector<RackRemoteRelease>>> ordered;
    for(const auto& rack:*racks){std::uint64_t order=0;
        if(!closed(rack,{"id","order","releases"})||!rack["id"].isString()||!validRackPackageId(rack["id"].toString())||!ids.insert(rack["id"].toString()).second||!quantity(rack["order"],UINT32_MAX,order)||!orders.insert(order).second)return Status::invalidData;
        const auto* releases=rack["releases"].getArray();if(!releases||releases->isEmpty())return Status::invalidData;if(releases->size()>int(limits::releasesPerRack))return Status::countLimit;
        std::vector<RackRemoteRelease> group;
        for(const auto& release:*releases){RackRemoteRelease r;
            if(!closed(release,{"ref","metadata","capabilities","archive","screenshot"}))return Status::invalidData;
            const auto ref=release["ref"],archive=release["archive"],screen=release["screenshot"];
            if(!closed(ref,{"id","version","sha256"})||!ref["id"].isString()||!ref["version"].isString()||!ref["sha256"].isString())return Status::invalidData;
            r.ref={ref["id"].toString(),ref["version"].toString(),ref["sha256"].toString()};if(!validRackPackageRef(r.ref)||r.ref.id!=rack["id"].toString())return Status::invalidData;
            if(!metadata(release["metadata"],r.metadata)||!capabilities(release["capabilities"],r.compatible))return Status::invalidData;
            for(const auto& tag:r.metadata.tags)if(!tagIds.contains(tag))return Status::invalidData;
            if(!closed(archive,{"assetId","url","bytes","sha256"})||!archive["url"].isString()||!archive["sha256"].isString()
                ||!quantity(archive["assetId"],INT64_MAX,r.archiveAssetId)||!r.archiveAssetId||!quantity(archive["bytes"],limits::archiveBytes,r.archiveBytes)||!r.archiveBytes
                ||archive["sha256"].toString()!=r.ref.sha256||!validRackLibraryUrl(archive["url"].toString()))return Status::invalidData;
            r.archiveUrl=archive["url"].toString();
            if(!closed(screen,{"url","bytes","sha256"})||!screen["url"].isString()||!screen["sha256"].isString()||!quantity(screen["bytes"],limits::pngBytes,r.screenshotBytes)||!r.screenshotBytes||!validRackPackageHash(screen["sha256"].toString())||!validRackLibraryUrl(screen["url"].toString()))return Status::invalidData;
            r.screenshotUrl=screen["url"].toString();r.screenshotSha256=screen["sha256"].toString();if(!r.compatible)r.incompatibility="Requires a newer AIR module or editor";
            const auto key=identity(r.ref);if(!next.raw.emplace(key,release.clone()).second)return Status::identityConflict;
            if(const auto it=assetHashes.find(r.archiveAssetId);it!=assetHashes.end()&&it->second!=r.ref.sha256)return Status::identityConflict;assetHashes[r.archiveAssetId]=r.ref.sha256;
            if(prior){const auto it=prior->raw.find(key);if(it!=prior->raw.end()&&it->second["ref"]["sha256"].toString()!=r.ref.sha256)return Status::identityConflict;}
            group.push_back(std::move(r));
        }
        std::sort(group.begin(),group.end(),[](const auto& a,const auto& b){return semver(a.ref.version)>semver(b.ref.version);});ordered.emplace_back(order,std::move(group));
    }
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.first<b.first;});for(auto& [order,group]:ordered)for(auto& r:group)next.releases.push_back(std::move(r));
    out=std::move(next);return Status::ready;
}
}
namespace {
struct FileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    FileHandle()=default;explicit FileHandle(HANDLE v):value(v){}
    FileHandle(FileHandle&& other)noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    ~FileHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    explicit operator bool()const{return value!=INVALID_HANDLE_VALUE;}
};
struct CacheRoot {
    juce::File root;
    std::vector<FileHandle> directories;
    FileHandle guard;
    static bool directory(HANDLE handle){BY_HANDLE_FILE_INFORMATION info{};return GetFileInformationByHandle(handle,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);}
    static bool matches(HANDLE handle,const juce::File& path){
        std::array<wchar_t,32768> buffer{};const auto length=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!length||length>=buffer.size())return false;
        String actual(buffer.data()),expected=path.getFullPathName().replaceCharacter('/','\\');if(actual.startsWith("\\\\?\\UNC\\"))actual="\\\\"+actual.substring(8);else if(actual.startsWith("\\\\?\\"))actual=actual.substring(4);if(expected.length()==2&&expected[1]==':')expected+="\\";return actual.equalsIgnoreCase(expected);
    }
    static FileHandle child(HANDLE parent,const juce::File& path,const String& name,bool isDirectory){
#if AIRACK_LIBRARY_CACHE_TESTS
        std::function<void(const juce::File&,const String&)> hook;{const std::lock_guard lock(cacheTestMutex);hook=cacheTestHook;}if(hook)hook(path,name);
#else
        (void)path;
#endif
        // A relative single component cannot traverse a replaced ancestor; unsupported native operations fail closed.
        const auto create=reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile"));
        if(!create||name.isEmpty()||name.containsAnyOf("/\\:")||name=="."||name==".."||name.length()>32767)return {};
        const auto wide=std::wstring(name.toWideCharPointer());UNICODE_STRING text{};text.Buffer=const_cast<wchar_t*>(wide.data());text.Length=USHORT(wide.size()*sizeof(wchar_t));text.MaximumLength=text.Length;
        OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=parent;attributes.ObjectName=&text;attributes.Attributes=OBJ_CASE_INSENSITIVE;
        IO_STATUS_BLOCK io{};HANDLE handle=INVALID_HANDLE_VALUE;
        const auto status=create(&handle,SYNCHRONIZE|FILE_READ_ATTRIBUTES|(isDirectory?FILE_LIST_DIRECTORY:0),&attributes,&io,nullptr,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN_IF,
            FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT|(isDirectory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE),nullptr,0);
        if(status<0)return {};return FileHandle(handle);
    }
    bool open(const juce::File& path){
        if(path==juce::File())return false;std::vector<juce::File> chain;auto dir=path;
        for(;;){chain.push_back(dir);auto parent=dir.getParentDirectory();if(parent==dir)break;dir=parent;}
        std::reverse(chain.begin(),chain.end());
        for(const auto& p:chain){
            auto native=p.getFullPathName();if(native.length()==2&&native[1]==':')native+="\\";
            FileHandle h=directories.empty()?FileHandle(CreateFileW(native.toWideCharPointer(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr))
                :child(directories.back().value,p.getParentDirectory(),p.getFileName(),true);
            if(!h||!directory(h.value)||!matches(h.value,p)||(!directories.empty()&&!directory(directories.back().value)))return false;
            directories.push_back(std::move(h));
        }
        auto held=child(directories.back().value,path,".air-cache-guard",false);BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(!held||!GetFileInformationByHandle(held.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1||!GetFileSizeEx(held.value,&size)||size.QuadPart!=0||!matches(held.value,path.getChildFile(".air-cache-guard"))||!directory(directories.back().value))return false;
        guard.value=std::exchange(held.value,INVALID_HANDLE_VALUE);
        root=path;return true;
    }
    bool read(const String& name,std::size_t maximum,juce::MemoryBlock& out){
        FileHandle h(CreateFileW(root.getChildFile(name).getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(!h||!GetFileInformationByHandle(h.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1||!GetFileSizeEx(h.value,&size)||size.QuadPart<0||std::uint64_t(size.QuadPart)>maximum)return false;
        juce::MemoryBlock bytes(std::size_t(size.QuadPart));std::size_t offset=0;while(offset<bytes.getSize()){DWORD got=0;const auto n=DWORD(std::min<std::size_t>(65536,bytes.getSize()-offset));if(!ReadFile(h.value,static_cast<char*>(bytes.getData())+offset,n,&got,nullptr)||!got)return false;offset+=got;}out.swapWith(bytes);return true;
    }
    bool write(const String& name,const void* data,std::size_t bytes){
        const auto temporary=root.getChildFile("write-"+juce::Uuid().toString()+".tmp"),target=root.getChildFile(name);
        bool result=false;{
            FileHandle h(CreateFileW(temporary.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(h){std::size_t offset=0;while(offset<bytes){DWORD wrote=0;const auto n=DWORD(std::min<std::size_t>(65536,bytes-offset));if(!WriteFile(h.value,static_cast<const char*>(data)+offset,n,&wrote,nullptr)||!wrote)break;offset+=wrote;}result=offset==bytes&&FlushFileBuffers(h.value);}
        }
        if(result)result=MoveFileExW(temporary.getFullPathName().toWideCharPointer(),target.getFullPathName().toWideCharPointer(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        if(!result)temporary.deleteFile();return result;
    }
    bool write(const String& name,const String& text){return write(name,text.toRawUTF8(),text.getNumBytesAsUTF8());}
    void touch(const String& name){FileHandle h(CreateFileW(root.getChildFile(name).getFullPathName().toWideCharPointer(),FILE_WRITE_ATTRIBUTES|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};if(!h||!GetFileInformationByHandle(h.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1)return;FILETIME now{};GetSystemTimeAsFileTime(&now);SetFileTime(h.value,nullptr,nullptr,&now);}
    bool remove(const String& name){FileHandle h(CreateFileW(root.getChildFile(name).getFullPathName().toWideCharPointer(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};if(!h||!GetFileInformationByHandle(h.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return false;FILE_DISPOSITION_INFO disposition{TRUE};return SetFileInformationByHandle(h.value,FileDispositionInfo,&disposition,sizeof(disposition))!=0;}
    bool cleanupImageStaging(){WIN32_FIND_DATAW entry{};HANDLE search=FindFirstFileW(root.getChildFile("write-*.tmp").getFullPathName().toWideCharPointer(),&entry);if(search==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;unsigned seen=0;bool ok=true;do{if(++seen>16){ok=false;break;}const String name(entry.cFileName);if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!remove(name))ok=false;}while(FindNextFileW(search,&entry));const auto error=GetLastError();FindClose(search);return ok&&error==ERROR_NO_MORE_FILES;}
};
struct RefreshLease {
    FileHandle file;OVERLAPPED offset{};bool held=false;
    explicit RefreshLease(const juce::File& root):file(CreateFileW(root.getChildFile("refresh.lock").getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)){
        BY_HANDLE_FILE_INFORMATION info{};if(file&&GetFileInformationByHandle(file.value,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&info.nNumberOfLinks==1)held=LockFileEx(file.value,LOCKFILE_FAIL_IMMEDIATELY|LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&offset)!=0;
    }
    ~RefreshLease(){if(held)UnlockFileEx(file.value,0,1,0,&offset);}
};
std::int64_t nowMs(){return juce::Time::currentTimeMillis();}
constexpr std::int64_t freshMs=15*60*1000;
struct CountPage {String url,etag,link;std::map<std::uint64_t,std::uint64_t> assets;};
var pagesJson(const std::vector<CountPage>& pages){juce::Array<var> array;for(const auto& p:pages){juce::Array<var> assets;for(const auto& [id,n]:p.assets)assets.add(object({{"id",number(id)},{"count",number(n)}}));array.add(object({{"url",p.url},{"etag",p.etag},{"link",p.link},{"assets",assets}}));}return array;}
bool nextPage(const String& link,unsigned page,String& next){
    next={};if(link.isEmpty())return true;
    for(const auto& entry:juce::StringArray::fromTokens(link,",","")){
        const auto pieces=juce::StringArray::fromTokens(entry.trim(),";","");if(pieces.size()<2)return false;
        const auto target=pieces[0].trim();if(!target.startsWithChar('<')||!target.endsWithChar('>'))return false;
        const auto url=target.substring(1,target.length()-1);if(!validRackLibraryUrl(url)||!url.startsWith("https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=100&page="))return false;
        bool isNext=false;for(int i=1;i<pieces.size();++i)if(pieces[i].trim()=="rel=\"next\"")isNext=true;
        if(isNext){const auto expected=String(rackLibraryCountsUrl).upToLastOccurrenceOf("=",true,false)+String(page+1);if(next.isNotEmpty()||url!=expected)return false;next=url;}
    }
    return true;
}
Status parseCountPage(std::string_view bytes,const std::set<std::uint64_t>& expected,CountPage& page){
    if(bytes.size()>limits::catalogBytes)return Status::byteLimit;var doc;if(!ai::parseStrictJson(bytes,doc,limits::catalogBytes)||!doc.isArray())return Status::invalidData;
    if(doc.size()>int(limits::countPageReleases))return Status::countLimit;
    for(const auto& release:*doc.getArray()){
        if(!release.isObject()||!release["assets"].isArray())return Status::invalidData;
        for(const auto& asset:*release["assets"].getArray()){
            std::uint64_t id=0,n=0;if(!asset.isObject()||!quantity(asset["id"],INT64_MAX,id)||!id||!quantity(asset["download_count"],INT64_MAX,n))return Status::invalidData;
            if(expected.contains(id)){const auto [it,inserted]=page.assets.emplace(id,n);if(!inserted&&it->second!=n)return Status::invalidData;}
        }
    }
    return Status::ready;
}
}
class RackLibraryInventoryPreparation final {
    friend class RackLibraryClient;
    mutable std::mutex mutex;
    std::vector<RackInstance> embedded;
    std::shared_ptr<const RackLibraryInventoryView> published=std::make_shared<RackLibraryInventoryView>();
    std::atomic<std::uint64_t> revision{0};
    std::uint64_t generation=UINT64_MAX;
    bool active=false,pending=false;
};
class RackLibraryPlacementPreparation final {
    friend class RackLibraryClient;
    mutable std::mutex mutex;
    RackInventoryPtr root;
    std::vector<RackPackageRef> refs;
    std::shared_ptr<const RackLibraryPlacementView> published;
    std::atomic<std::uint64_t> revision{0};
    bool pending=false,preparing=false;
};
class RackLibrarySkinPreparation final {
    friend class RackLibraryClient;
public:
    mutable std::mutex mutex;
    RackInventoryPtr inventory;
    std::vector<RackInstance> requested;
    std::vector<RackInstance> used;
    std::optional<RackInstance> preview;
    std::vector<std::pair<RackInstance,std::shared_ptr<const studio::MakerSkin>>> owners;
    RackLibrarySkinPreparer prepare;
    std::atomic<std::uint64_t> revision{0};
    std::uint64_t generation=0;
    bool pending=false;
    static bool same(const RackInstance& a,const RackInstance& b){return a.design==b.design&&a.package==b.package;}
    void rebuild(){requested=used;if(preview&&std::none_of(used.begin(),used.end(),[&](const auto& rack){return same(rack,*preview);}))requested.push_back(*preview);}
};
struct RackLibraryClient::Impl {
    struct Job {std::uint64_t id=0;enum Kind{refresh,open,install,restore,remove,thumbnail,reveal} kind=refresh;RackPackageRef ref;};
    std::shared_ptr<RackPackageStore> store;RackLibraryTransport transport;
    mutable std::mutex mutex;std::condition_variable_any changed;
    std::shared_ptr<const RackLibraryClientView> published=std::make_shared<RackLibraryClientView>();
    std::optional<Job> pending;std::atomic<std::uint64_t> latest{0},cancelledJob{UINT64_MAX};
    std::vector<std::weak_ptr<RackLibrarySkinPreparation>> skinStates;
    std::vector<std::weak_ptr<RackLibraryInventoryPreparation>> inventoryStates;
    std::vector<std::weak_ptr<RackLibraryPlacementPreparation>> placementStates;
    std::atomic<std::uint64_t> localRevision{0};
    std::chrono::steady_clock::time_point lastObservation{};
    std::size_t inventoryCursor=0,placementCursor=0;unsigned localTurn=0;
    struct ThumbnailRequest {RackPackageRef ref;unsigned edge=512;bool builtIn=false;};
    std::deque<ThumbnailRequest> thumbnailRequests;
    struct ThumbnailEntry {String key;std::shared_ptr<const RackLibraryThumbnail> value;std::uint64_t use=0;std::int64_t checkedAt=0;};
    std::vector<ThumbnailEntry> thumbnails;std::uint64_t thumbnailUse=0;
    std::atomic<bool> stopping{false};
    Catalog catalog;String catalogBody,catalogEtag;std::int64_t catalogTime=0,countTime=0,rateUntil=0;
    std::vector<CountPage> countPages;std::vector<RackDownloadCount> counts;unsigned rateFailures=0;
    std::jthread worker;
    explicit Impl(std::shared_ptr<RackPackageStore> s,RackLibraryTransport t):store(std::move(s)),transport(std::move(t)),worker([this](std::stop_token stop){run(stop);}){}
    ~Impl(){stopping=true;worker.request_stop();changed.notify_all();if(worker.joinable())worker.join();}
    bool cancelled(std::uint64_t id)const{return stopping||latest!=id||cancelledJob==id;}
    bool observeRate(const RackHttpResponse& response){
        const auto previous=rateUntil;const auto failures=rateFailures;const auto now=nowMs();std::int64_t until=0;
        if(response.code==403||response.code==429){
            const auto fallback=std::min<std::int64_t>(3600,60ll<<std::min(rateFailures,6u));rateFailures=std::min(rateFailures+1,7u);until=now+fallback*1000;std::uint64_t seconds=0;
            if(headerNumber(response.retryAfter,seconds)&&seconds<=std::uint64_t((INT64_MAX-now)/1000))until=std::max(until,now+std::int64_t(seconds*1000));
        }
        std::uint64_t remaining=1,reset=0;
        if(headerNumber(response.rateLimitRemaining,remaining)&&remaining==0&&headerNumber(response.rateLimitReset,reset)&&reset<=std::uint64_t(INT64_MAX/1000)&&std::int64_t(reset*1000)>now)until=std::max(until,std::int64_t(reset*1000));
        rateUntil=std::max(rateUntil,until);return rateUntil!=previous||rateFailures!=failures;
    }
    bool saveRate(CacheRoot& cache){return cache.write("rate.json",juce::JSON::toString(object({{"until",number(std::uint64_t(rateUntil))},{"failures",int(rateFailures)}}),true));}
    std::uint64_t submit(Job::Kind kind,const RackPackageRef& ref={}){
        const std::lock_guard lock(mutex);const auto id=++latest;pending=Job{id,kind,ref};
        auto v=std::make_shared<RackLibraryClientView>(*published);v->job=id;v->busy=true;v->bytesDone=v->bytesTotal=0;v->message={};published=std::move(v);changed.notify_all();return id;
    }
    template<class Fn>void publish(std::uint64_t id,Fn&& f){const std::lock_guard lock(mutex);if(published->job!=id||stopping)return;auto v=std::make_shared<RackLibraryClientView>(*published);f(*v);published=std::move(v);}
    void terminal(std::uint64_t id,Status status,bool cached=false){
        const auto generation=store?store->generation(nullptr,[&]{return stopping.load();}):0;
        const auto committed=store?store->committed(nullptr,[&]{return stopping.load();}):std::vector<RackPackageRef>{};
        {const std::lock_guard lock(mutex);for(const auto& weak:skinStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);if(state->generation!=generation){state->inventory.reset();state->generation=generation;++state->revision;state->pending=false;}}}
        publish(id,[&](auto& v){v.busy=false;v.status=status;v.cached=cached;v.offline=status==Status::networkFailure||status==Status::timeout;v.generation=generation;
            v.tags=catalog.tags;v.releases=catalog.releases;v.counts=counts;v.committed=committed;v.countsStale=countTime==0||nowMs()-countTime>=freshMs;v.message=status==Status::ready?String():status==Status::rateLimited?String("GitHub rate limit; try again later"):status==Status::cancelled?String("Cancelled"):String("Library request unavailable");});
        auto expected=id;cancelledJob.compare_exchange_strong(expected,UINT64_MAX);
    }
    Status get(CacheRoot& cache,const RackHttpRequest& request,juce::MemoryBlock& bytes,RackHttpResponse& response,std::uint64_t id){
        if(cancelled(id))return Status::cancelled;loadRate(cache);if(nowMs()<rateUntil)return Status::rateLimited;Status sinkStatus=Status::ready;
        const auto status=transport(request,[&](const void* p,std::size_t n){if(cancelled(id)){sinkStatus=Status::cancelled;return false;}if(n>request.maximumBytes-bytes.getSize()){sinkStatus=Status::byteLimit;return false;}bytes.append(p,n);return true;},[&]{return cancelled(id);},response);
        if(observeRate(response)&&!saveRate(cache))return Status::storageFailure;
        if(cancelled(id))return Status::cancelled;if(sinkStatus!=Status::ready)return sinkStatus;
        return status==Status::ready&&(response.code==403||response.code==429)?Status::rateLimited:status;
    }
    bool loadCache(CacheRoot& cache){
        juce::MemoryBlock bytes;var doc;Catalog next;
        if(!cache.read("catalog.json",limits::catalogBytes*2+4096,bytes)||!ai::parseStrictJson({static_cast<const char*>(bytes.getData()),bytes.getSize()},doc,limits::catalogBytes*2+4096)||!closed(doc,{"schema","body","etag","time"})||!doc["schema"].isInt()||int(doc["schema"])!=1||!doc["body"].isString()||!text(doc["etag"],1024,true))return false;
        std::uint64_t stamp=0;if(!decimal(doc["time"],stamp,true)||stamp>std::uint64_t(nowMs())||parseCatalog(doc["body"].toString().toStdString(),next,catalog.raw.empty()?nullptr:&catalog)!=Status::ready)return false;
        const auto previousCounts=counts;const bool sameBody=catalogBody==doc["body"].toString();
        catalog=std::move(next);catalogBody=doc["body"].toString();catalogEtag=doc["etag"].toString();catalogTime=std::int64_t(stamp);
        if(!sameBody){countTime=0;countPages.clear();}
        std::vector<RackDownloadCount> unknown;std::set<String> ids;for(const auto& r:catalog.releases)if(ids.insert(r.ref.id).second){auto it=std::find_if(previousCounts.begin(),previousCounts.end(),[&](const auto& count){return count.id==r.ref.id;});unknown.push_back({r.ref.id,it==previousCounts.end()?std::optional<std::uint64_t>{}:it->value});}counts=std::move(unknown);
        loadRate(cache);
        if(!cache.read("counts.json",limits::catalogBytes*limits::countPages,bytes)||!ai::parseStrictJson({static_cast<const char*>(bytes.getData()),bytes.getSize()},doc,limits::catalogBytes*limits::countPages)||!doc["schema"].isInt()||!validRackPackageHash(doc["catalogHash"].toString())||!decimal(doc["time"],stamp,true)||stamp>std::uint64_t(nowMs()))return true;
        const bool retained=int(doc["schema"])==2;
        if(retained){
            if(!closed(doc,{"schema","catalogHash","time","pages","totals"})||!doc["totals"].isArray()||doc["totals"].size()>int(limits::installedCount))return true;
            std::map<String,std::uint64_t> known;
            for(const auto& total:*doc["totals"].getArray()){std::uint64_t value=0;if(!closed(total,{"id","value"})||!total["id"].isString()||!validRackPackageId(total["id"].toString())||!decimal(total["value"],value,true)||!known.emplace(total["id"].toString(),value).second)return true;}
            for(auto& count:counts)if(const auto it=known.find(count.id);it!=known.end())count.value=it->second;
        }else if(int(doc["schema"])!=1||!closed(doc,{"schema","catalogHash","time","pages"}))return true;
        if(doc["catalogHash"].toString()!=juce::SHA256(catalogBody.toRawUTF8(),catalogBody.getNumBytesAsUTF8()).toHexString()){countTime=0;countPages.clear();return true;}
        const auto* pages=doc["pages"].getArray();if(!pages||pages->size()>int(limits::countPages))return true;std::vector<CountPage> parsed;
        for(const auto& p:*pages){CountPage page;if(!closed(p,{"url","etag","link","assets"})||!p["url"].isString()||!validRackLibraryUrl(p["url"].toString())||!text(p["etag"],1024,true)||!text(p["link"],32768,true)||!p["assets"].isArray())return true;
            page.url=p["url"].toString();page.etag=p["etag"].toString();page.link=p["link"].toString();
            for(const auto& a:*p["assets"].getArray()){std::uint64_t asset=0,n=0;if(!closed(a,{"id","count"})||!decimal(a["id"],asset)||!decimal(a["count"],n,true)||!page.assets.emplace(asset,n).second)return true;}parsed.push_back(std::move(page));}
        std::vector<RackDownloadCount> totals;if(totalsFrom(parsed,totals)!=Status::ready)return true;countPages=std::move(parsed);counts=std::move(totals);countTime=std::int64_t(stamp);return true;
    }
    void loadRate(CacheRoot& cache){juce::MemoryBlock bytes;var doc;std::uint64_t stamp=0,n=0;if(cache.read("rate.json",4096,bytes)&&ai::parseStrictJson({static_cast<const char*>(bytes.getData()),bytes.getSize()},doc,4096)&&decimal(doc["until"],stamp,true)&&stamp<=INT64_MAX){rateUntil=std::max(rateUntil,std::int64_t(stamp));if(quantity(doc["failures"],7,n))rateFailures=std::max(rateFailures,unsigned(n));}}
    bool saveCounts(CacheRoot& cache,const String& body,std::int64_t stamp,const std::vector<CountPage>& pages,const std::vector<RackDownloadCount>& values){
        juce::Array<var> totals;for(const auto& count:values)if(count.value)totals.add(object({{"id",count.id},{"value",number(*count.value)}}));
        const auto hash=juce::SHA256(body.toRawUTF8(),body.getNumBytesAsUTF8()).toHexString();
        const auto json=juce::JSON::toString(object({{"schema",2},{"catalogHash",hash},{"time",number(std::uint64_t(stamp))},{"pages",pagesJson(pages)},{"totals",totals}}),true);
        return json.getNumBytesAsUTF8()<=limits::catalogBytes*limits::countPages&&cache.write("counts.json",json);
    }
    bool saveCatalog(CacheRoot& cache,const String& body,const String& etag,std::int64_t time){return cache.write("catalog.json",juce::JSON::toString(object({{"schema",1},{"body",body},{"etag",etag},{"time",number(std::uint64_t(time))}}),true));}
    Status totalsFrom(const std::vector<CountPage>& pages,std::vector<RackDownloadCount>& totals){
        std::map<std::uint64_t,std::uint64_t> assets;String expectedUrl=rackLibraryCountsUrl;
        for(std::size_t i=0;i<pages.size();++i){const auto& page=pages[i];if(page.url!=expectedUrl)return Status::invalidData;
            if(!nextPage(page.link,unsigned(i+1),expectedUrl))return Status::invalidData;
            for(const auto& [id,n]:page.assets){const auto [it,inserted]=assets.emplace(id,n);if(!inserted&&it->second!=n)return Status::invalidData;}}
        if(!expectedUrl.isEmpty())return Status::invalidData;std::map<String,std::set<std::uint64_t>> expected;for(const auto& r:catalog.releases)expected[r.ref.id].insert(r.archiveAssetId);
        for(const auto& [id,ids]:expected){std::uint64_t total=0;for(const auto asset:ids){const auto it=assets.find(asset);if(it==assets.end())return Status::invalidData;if(it->second>UINT64_MAX-total)return Status::byteLimit;total+=it->second;}totals.push_back({id,total});}return Status::ready;
    }
    Status scanCounts(CacheRoot& cache,std::uint64_t id){
        std::set<std::uint64_t> expected;for(const auto& r:catalog.releases)expected.insert(r.archiveAssetId);
        String url=rackLibraryCountsUrl;std::vector<CountPage> pages;
        for(unsigned page=1;!url.isEmpty()&&page<=limits::countPages;++page){
            auto old=std::find_if(countPages.begin(),countPages.end(),[&](const auto& p){return p.url==url;});
            RackHttpResponse response;juce::MemoryBlock bytes;const auto result=get(cache,{url,old==countPages.end()?String():old->etag,limits::catalogBytes},bytes,response,id);if(result!=Status::ready)return result;
            CountPage next;next.url=url;
            if(response.code==304){if(old==countPages.end())return Status::invalidData;next=*old;}
            else if(response.code==200){const auto s=parseCountPage({static_cast<const char*>(bytes.getData()),bytes.getSize()},expected,next);if(s!=Status::ready)return s;next.etag=response.etag;next.link=response.link;}
            else return Status::networkFailure;
            if(!nextPage(next.link,page,url))return Status::invalidData;pages.push_back(std::move(next));
        }
        if(!url.isEmpty())return Status::countLimit;std::vector<RackDownloadCount> totals;auto status=totalsFrom(pages,totals);if(status!=Status::ready)return status;if(cancelled(id))return Status::cancelled;
        const auto stamp=nowMs();if(!saveCounts(cache,catalogBody,stamp,pages,totals))return Status::storageFailure;
        countPages=std::move(pages);counts=std::move(totals);countTime=stamp;return Status::ready;
    }
    Status refresh(CacheRoot& cache,const Job& job,bool& fromCache){
        const auto started=nowMs();const auto previous=catalogTime;loadCache(cache);loadRate(cache);
        if(job.kind==Job::open&&catalogTime>0&&countTime>0&&started-catalogTime<freshMs&&started-countTime<freshMs){fromCache=true;return Status::ready;}
        if(nowMs()<rateUntil){fromCache=!catalogBody.isEmpty();return Status::rateLimited;}
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(30);std::unique_ptr<RefreshLease> lease;bool waited=false;
        while(!cancelled(job.id)){lease=std::make_unique<RefreshLease>(cache.root);if(lease->held)break;waited=true;if(std::chrono::steady_clock::now()>=limit)return Status::busy;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        if(cancelled(job.id))return Status::cancelled;
        loadCache(cache);loadRate(cache);if(waited&&((catalogTime>previous&&catalogTime>=started)||(countTime>0&&countTime>=started))){fromCache=true;return Status::ready;}
        if(nowMs()<rateUntil){fromCache=!catalogBody.isEmpty();return Status::rateLimited;}
        RackHttpResponse response;juce::MemoryBlock bytes;auto status=get(cache,{rackLibraryCatalogUrl,catalogEtag,limits::catalogBytes},bytes,response,job.id);
        if(status!=Status::ready){if(status==Status::rateLimited)saveRate(cache);fromCache=!catalogBody.isEmpty();return status;}
        const auto stamp=nowMs();
        if(response.code==304){if(catalogBody.isEmpty())return Status::invalidData;if(!saveCatalog(cache,catalogBody,catalogEtag,stamp))return Status::storageFailure;catalogTime=stamp;fromCache=true;}
        else if(response.code==200){Catalog next;const auto body=String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize()));status=parseCatalog({static_cast<const char*>(bytes.getData()),bytes.getSize()},next,&catalog);if(status!=Status::ready)return status;if(cancelled(job.id))return Status::cancelled;
            const bool changed=body!=catalogBody;
            if(changed&&countTime>0&&!saveCounts(cache,catalogBody,countTime,countPages,counts))return Status::storageFailure;
            if(!saveCatalog(cache,body,response.etag,stamp))return Status::storageFailure;
            const auto previousCounts=counts;catalog=std::move(next);catalogBody=body;catalogEtag=response.etag;catalogTime=stamp;
            if(changed){{const std::lock_guard lock(mutex);thumbnails.clear();}countTime=0;countPages.clear();counts.clear();std::set<String> ids;for(const auto& r:catalog.releases)if(ids.insert(r.ref.id).second){const auto prior=std::find_if(previousCounts.begin(),previousCounts.end(),[&](const auto& count){return count.id==r.ref.id;});counts.push_back({r.ref.id,prior==previousCounts.end()?std::optional<std::uint64_t>{}:prior->value});}}
        }else return Status::networkFailure;
        status=scanCounts(cache,job.id);if(status==Status::rateLimited)saveRate(cache);return status;
    }
    Status download(CacheRoot& cache,const Job& job){
        loadRate(cache);if(nowMs()<rateUntil)return Status::rateLimited;
        const auto found=std::find_if(catalog.releases.begin(),catalog.releases.end(),[&](const auto& r){return r.ref==job.ref;});if(found==catalog.releases.end())return Status::unavailable;if(!found->compatible)return Status::incompatible;const auto release=*found;
        const auto file=cache.root.getChildFile("download-"+juce::Uuid().toString()+".zip");struct Cleanup{juce::File file;~Cleanup(){file.deleteFile();}}cleanup{file};
        publish(job.id,[&](auto& v){v.bytesTotal=release.archiveBytes;v.bytesDone=0;});
        RackHttpResponse response;Status sinkStatus=Status::ready;std::uint64_t total=0;
        {
            FileHandle output(CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(!output)return Status::storageFailure;
            loadRate(cache);if(nowMs()<rateUntil)return Status::rateLimited;
            const auto status=transport({release.archiveUrl,{},release.archiveBytes},[&](const void* data,std::size_t n){
                if(cancelled(job.id)){sinkStatus=Status::cancelled;return false;}if(n>release.archiveBytes-total){sinkStatus=Status::byteLimit;return false;}
                DWORD written=0;if(n>MAXDWORD||!WriteFile(output.value,data,DWORD(n),&written,nullptr)||written!=n){sinkStatus=Status::storageFailure;return false;}total+=n;publish(job.id,[&](auto& v){v.bytesDone=total;});return true;
            },[&]{return cancelled(job.id);},response);
            if(observeRate(response)&&!saveRate(cache))return Status::storageFailure;
            if(cancelled(job.id))return Status::cancelled;if(sinkStatus!=Status::ready)return sinkStatus;if(status!=Status::ready)return status;if(response.code==403||response.code==429)return Status::rateLimited;if(response.code!=200)return Status::networkFailure;
            if(total!=release.archiveBytes)return Status::byteLimit;if(!FlushFileBuffers(output.value))return Status::storageFailure;
        }
        juce::FileInputStream input(file);if(input.failedToOpen())return Status::storageFailure;if(juce::SHA256(input).toHexString()!=release.ref.sha256)return Status::identityConflict;
        input.setPosition(0);juce::ZipFile zip(input);const auto* entry=zip.getEntry("package.json");if(!entry||entry->uncompressedSize<=0||entry->uncompressedSize>juce::int64(limits::manifestBytes))return Status::invalidData;
        auto stream=std::unique_ptr<juce::InputStream>(zip.createStreamForEntry(*entry));juce::MemoryBlock manifestBytes;if(!stream||stream->readIntoMemoryBlock(manifestBytes,entry->uncompressedSize)!=entry->uncompressedSize)return Status::invalidData;
        RackPackageManifest manifest;Status status;if(!decodeRackPackageManifest({static_cast<const char*>(manifestBytes.getData()),manifestBytes.getSize()},manifest,&status))return status;
        const auto actual=juce::JSON::parse(String::fromUTF8(static_cast<const char*>(manifestBytes.getData()),int(manifestBytes.getSize())));const auto& remote=catalog.raw.at(identity(release.ref));
        if(manifest.ref.id!=release.ref.id||manifest.ref.version!=release.ref.version||!sameMetadata(manifest.metadata,release.metadata)||!sameCapabilities(actual["capabilities"],remote["capabilities"]))return Status::identityConflict;
        if(cancelled(job.id))return Status::cancelled;
        return store->install(file,release.ref,job.kind==Job::install?RackPackageActivation::makeActive:RackPackageActivation::preserveActive,[&]{return cancelled(job.id);});
    }
    static String thumbnailKey(const RackPackageRef& ref,unsigned edge,bool builtIn=false){return (builtIn?String("builtin:"):String{})+identity(ref)+":"+ref.sha256+":"+String(edge);}
    void publishThumbnail(const ThumbnailRequest& request,std::shared_ptr<const RackLibraryThumbnail> value){
        const std::lock_guard lock(mutex);const auto key=thumbnailKey(request.ref,request.edge,request.builtIn);
        std::erase_if(thumbnails,[&](const auto& entry){return entry.key==key;});
        thumbnails.push_back({key,std::move(value),++thumbnailUse,nowMs()});
        const auto bytes=[&]{std::uint64_t n=0;for(const auto& entry:thumbnails)n+=std::uint64_t(entry.value->image.getWidth())*entry.value->image.getHeight()*4;return n;};
        while(thumbnails.size()>16||bytes()>16ull*1024*1024){auto oldest=std::min_element(thumbnails.begin(),thumbnails.end(),[](const auto& a,const auto& b){return a.use<b.use;});thumbnails.erase(oldest);}
    }
    bool evictImages(CacheRoot& images,std::uint64_t reserve=0){
        struct Entry{String name;std::uint64_t size=0,time=0;};std::vector<Entry> files;std::uint64_t total=0;
        WIN32_FIND_DATAW entry{};HANDLE search=FindFirstFileW(images.root.getChildFile("*.png").getFullPathName().toWideCharPointer(),&entry);
        if(search!=INVALID_HANDLE_VALUE){unsigned seen=0;bool bounded=true;do{
            if(++seen>256){bounded=false;break;}const String name(entry.cFileName);if((entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!validRackPackageHash(name.dropLastCharacters(4)))continue;
            const auto size=(std::uint64_t(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow,time=(std::uint64_t(entry.ftLastWriteTime.dwHighDateTime)<<32)|entry.ftLastWriteTime.dwLowDateTime;
            if(size>UINT64_MAX-total){bounded=false;break;}total+=size;files.push_back({name,size,time});
        }while(FindNextFileW(search,&entry));const auto error=GetLastError();FindClose(search);if(!bounded||error!=ERROR_NO_MORE_FILES)return false;}
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.time<b.time;});std::size_t count=files.size();
        const auto cap=32ull*1024*1024-reserve;const auto entries=reserve?127u:128u;
        for(const auto& file:files){if(total<=cap&&count<=entries)break;if(images.remove(file.name)){total-=file.size;--count;images.remove(file.name.dropLastCharacters(4)+".json");}}
        return total<=cap&&count<=entries;
    }
    static void decodeThumbnail(const juce::MemoryBlock& bytes,unsigned edge,RackLibraryThumbnail& result){
        std::uint32_t width=0,height=0;
        if(!inspectRackPackagePng({static_cast<const std::byte*>(bytes.getData()),bytes.getSize()},width,height)){result.status=Status::invalidData;return;}
        juce::MemoryInputStream input(bytes,false);auto image=juce::PNGImageFormat().decodeImage(input);
        if(!image.isValid()){result.status=Status::invalidData;return;}
        const auto scale=std::min(1.0,double(edge)/std::max(image.getWidth(),image.getHeight()));
        if(scale<1)image=image.rescaled(std::max(1,int(image.getWidth()*scale)),std::max(1,int(image.getHeight()*scale)),juce::Graphics::highResamplingQuality);
        result.image=std::move(image);result.status=Status::ready;
    }
    static juce::MemoryBlock nativePreviewFile(const String& filename){
        for(int n=0;n<NativePreviewData::namedResourceListSize;++n)
            if(filename==NativePreviewData::originalFilenames[n]){int size=0;const auto* data=NativePreviewData::getNamedResource(NativePreviewData::namedResourceList[n],size);if(data&&size>0&&std::uint64_t(size)<=limits::pngBytes)return {data,std::size_t(size)};}
        return {};
    }
    void prepareBuiltInThumbnail(const ThumbnailRequest& request){
        auto result=std::make_shared<RackLibraryThumbnail>();const auto provenance=nativePreviewFile("provenance.json");var records;
        if(!ai::parseStrictJson({static_cast<const char*>(provenance.getData()),provenance.getSize()},records,65536)||!records.isArray()||records.size()!=17){result->status=Status::invalidData;publishThumbnail(request,result);return;}
        for(const auto& record:*records.getArray())
            if(record["design"].toString()==request.ref.id&&record["revision"].toString()==request.ref.version&&record["canonicalSha256"].toString()==request.ref.sha256){
                const auto bytes=nativePreviewFile(record["png"].toString());
                if(juce::SHA256(bytes).toHexString()!=record["pngSha256"].toString())result->status=Status::identityConflict;
                else decodeThumbnail(bytes,request.edge,*result);break;
            }
        publishThumbnail(request,result);
    }
    void prepareThumbnail(const ThumbnailRequest& request){
        if(request.builtIn){prepareBuiltInThumbnail(request);return;}
#if AIRACK_LIBRARY_CACHE_TESTS
        std::function<void(unsigned,bool,bool,bool)> acquisitionHook;{const std::lock_guard lock(cacheTestMutex);acquisitionHook=thumbnailAcquisitionTestHook;}
        unsigned checkpoints=0;bool installedPublished=false;
        const juce::ScopeGuard acquisitionFinished{[&]{if(acquisitionHook)acquisitionHook(checkpoints,stopping.load(),true,installedPublished);}};
#endif
        const auto acquisitionCancel=[&]{const bool cancelled=stopping.load();
#if AIRACK_LIBRARY_CACHE_TESTS
            if(acquisitionHook)acquisitionHook(++checkpoints,cancelled,false,false);
#endif
            return cancelled;};
        // Installed screenshots belong to this exact release and work without catalog or network authority.
        if(auto lease=store->open(request.ref,nullptr,acquisitionCancel)){
            if(stopping)return;
            auto result=std::make_shared<RackLibraryThumbnail>();juce::MemoryBlock bytes;
            result->status=lease->readListed(lease->manifest().screenshotPath,bytes);
            if(stopping)return;
            if(result->status==Status::ready)decodeThumbnail(bytes,request.edge,*result);
            if(stopping)return;
            publishThumbnail(request,result);
#if AIRACK_LIBRARY_CACHE_TESTS
            installedPublished=true;
#endif
            return;
        }
        if(stopping)return;
        auto result=std::make_shared<RackLibraryThumbnail>();const auto found=std::find_if(catalog.releases.begin(),catalog.releases.end(),[&](const auto& r){return r.ref==request.ref;});
        if(found==catalog.releases.end()){publishThumbnail(request,result);return;}const auto release=*found;const auto id=latest.load();
        CacheRoot cache,images;if(!cache.open(store->paths().root.getChildFile("cache"))||!images.open(cache.root.getChildFile("images"))){result->status=Status::storageFailure;publishThumbnail(request,result);return;}
        RefreshLease lease(cache.root);if(!lease.held){result->status=Status::busy;publishThumbnail(request,result);return;}
        if(!images.cleanupImageStaging()){result->status=Status::storageFailure;publishThumbnail(request,result);return;}
        const auto name=release.screenshotSha256;juce::MemoryBlock bytes,metaBytes;var meta;String etag;std::uint64_t stamp=0;
        const auto valid=[&](const juce::MemoryBlock& data){std::uint32_t w=0,h=0;return data.getSize()==release.screenshotBytes&&juce::SHA256(data).toHexString()==name&&inspectRackPackagePng({static_cast<const std::byte*>(data.getData()),data.getSize()},w,h);};
        bool cached=images.read(name+".png",limits::pngBytes,bytes)&&valid(bytes);
        if(cached&&images.read(name+".json",4096,metaBytes)&&ai::parseStrictJson({static_cast<const char*>(metaBytes.getData()),metaBytes.getSize()},meta,4096)&&text(meta["etag"],1024,true)&&decimal(meta["time"],stamp,true)&&stamp<=std::uint64_t(nowMs()))etag=meta["etag"].toString();
        if(!cached||stamp==0||nowMs()-std::int64_t(stamp)>=freshMs){
            loadRate(cache);if(nowMs()<rateUntil){if(!cached){result->status=Status::rateLimited;publishThumbnail(request,result);return;}}
            else {
                juce::MemoryBlock received;RackHttpResponse response;const auto status=get(cache,{release.screenshotUrl,etag,release.screenshotBytes},received,response,id);
                bool validatedResponse=status==Status::ready&&response.code==304&&cached;
                if(status==Status::rateLimited)saveRate(cache);
                if(status==Status::ready&&response.code==200&&valid(received)){
                    if(cancelled(id))return;if(!evictImages(images,received.getSize())||!images.write(name+".png",received.getData(),received.getSize())){result->status=Status::storageFailure;publishThumbnail(request,result);return;}bytes.swapWith(received);cached=true;etag=response.etag;validatedResponse=true;
                }else if(status!=Status::ready||response.code!=304||!cached){if(!cached){result->status=status==Status::ready?Status::invalidData:status;publishThumbnail(request,result);return;}}
                if(validatedResponse){images.write(name+".json",juce::JSON::toString(object({{"etag",etag},{"time",number(std::uint64_t(nowMs()))}}),true));evictImages(images);}
            }
        }
        images.touch(name+".png");
        if(cancelled(id))return;
        juce::MemoryInputStream input(bytes,false);auto image=juce::PNGImageFormat().decodeImage(input);
        if(image.isValid()){
            const auto scale=std::min(1.0,double(request.edge)/std::max(image.getWidth(),image.getHeight()));
            if(scale<1)image=image.rescaled(std::max(1,int(image.getWidth()*scale)),std::max(1,int(image.getHeight()*scale)),juce::Graphics::highResamplingQuality);
            result->image=std::move(image);result->status=Status::ready;
        }else result->status=Status::invalidData;
        if(!cancelled(id))publishThumbnail(request,result);
    }
    bool hasSkinWork(){std::erase_if(skinStates,[](const auto& state){return state.expired();});for(const auto& weak:skinStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);if(state->pending)return true;}return false;}
    void prepareOneSkin(){
        std::shared_ptr<RackLibrarySkinPreparation> state;{
            const std::lock_guard lock(mutex);for(const auto& weak:skinStates)if(auto candidate=weak.lock()){const std::lock_guard guard(candidate->mutex);if(candidate->pending){state=std::move(candidate);break;}}
        }
        if(!state)return;std::unique_lock lock(state->mutex);const auto serial=state->revision.load();const auto snapshot=state->inventory;const auto work=state->requested;
#if AIRACK_LIBRARY_CACHE_TESTS
        const auto used=state->used;const auto preview=state->preview;
#endif
        state->pending=false;lock.unlock();
#if AIRACK_LIBRARY_CACHE_TESTS
        std::function<void(std::vector<RackInstance>,std::optional<RackInstance>,std::vector<RackInstance>,bool,bool)> hook;{const std::lock_guard guard(cacheTestMutex);hook=skinTestHook;}if(hook)hook(used,preview,work,false,false);
#endif
        const auto cancel=[&]{return stopping||state->revision!=serial;};std::vector<std::pair<RackInstance,std::shared_ptr<const studio::MakerSkin>>> owners;
        try{for(const auto& rack:work){if(cancel())break;owners.emplace_back(rack,state->prepare(rack,snapshot.get(),cancel));}}catch(const std::exception&){}
#if AIRACK_LIBRARY_CACHE_TESTS
        lock.lock();const bool published=!cancel();if(published)state->owners=std::move(owners);lock.unlock();if(hook)hook(used,preview,work,true,published);
#else
        lock.lock();if(!cancel())state->owners=std::move(owners);
#endif
    }
    bool hasInventoryWork(){std::erase_if(inventoryStates,[](const auto& state){return state.expired();});for(const auto& weak:inventoryStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);if(state->pending)return true;}return false;}
    bool hasPlacementWork(){std::erase_if(placementStates,[](const auto& state){return state.expired();});for(const auto& weak:placementStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);if(state->pending)return true;}return false;}
    void observeLocalGeneration(){
        const auto now=std::chrono::steady_clock::now();if(now-lastObservation<std::chrono::milliseconds(250))return;
        std::uint64_t publishedGeneration=0;bool subscribed=false;{const std::lock_guard lock(mutex);publishedGeneration=published->generation;for(const auto& weak:inventoryStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);subscribed|=state->active;}}
        if(!subscribed)return;lastObservation=now;const auto serial=localRevision.load();Status status;
        const auto generation=store->generation(&status,[&]{return stopping||localRevision!=serial;});if(stopping||localRevision!=serial)return;
        const auto committed=status==Status::ready&&publishedGeneration!=generation?store->committed(nullptr,[&]{return stopping||localRevision!=serial;}):std::vector<RackPackageRef>{};
        const std::lock_guard lock(mutex);
        if(status==Status::ready&&published->generation!=generation){auto next=std::make_shared<RackLibraryClientView>(*published);next->generation=generation;next->committed=committed;published=std::move(next);}
        for(const auto& weak:inventoryStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);if(!state->active)continue;
            if(status==Status::ready&&state->generation!=generation){state->generation=generation;state->pending=true;++state->revision;
                auto next=std::make_shared<RackLibraryInventoryView>(*state->published);next->status=Status::busy;next->busy=true;next->revision=state->revision;state->published=std::move(next);
            }else if(status!=Status::ready){auto next=std::make_shared<RackLibraryInventoryView>(*state->published);next->status=status;next->busy=false;state->published=std::move(next);}
        }
        for(const auto& weak:placementStates)if(auto state=weak.lock()){const std::lock_guard guard(state->mutex);if(state->published&&state->published->status==Status::ready&&state->published->storeGeneration!=generation){++state->revision;state->published.reset();state->pending=bool(state->root);}}
    }
    void prepareOneInventory(){
        std::shared_ptr<RackLibraryInventoryPreparation> state;{
            const std::lock_guard lock(mutex);for(std::size_t n=0;n<inventoryStates.size();++n){const auto index=(inventoryCursor+n)%inventoryStates.size();if(auto candidate=inventoryStates[index].lock()){const std::lock_guard guard(candidate->mutex);if(candidate->pending){state=std::move(candidate);inventoryCursor=index+1;break;}}}}
        if(!state)return;std::unique_lock lock(state->mutex);const auto serial=state->revision.load();const auto embedded=state->embedded;state->pending=false;lock.unlock();
        const auto stale=[&]{return stopping||state->revision!=serial;};
#if AIRACK_LIBRARY_CACHE_TESTS
        std::function<void(std::uint64_t,unsigned,bool,bool,bool)> hook;{const std::lock_guard guard(cacheTestMutex);hook=inventoryTestHook;}unsigned checkpoints=0;
        const auto cancel=[&]{const bool cancelled=stale();if(hook)hook(serial,++checkpoints,cancelled,false,false);return cancelled;};
#else
        const auto& cancel=stale;
#endif
        RackInventoryPtr captured;Status status=Status::unavailable;
        try{captureRackInventory(*store,embedded,captured,&status,cancel);}catch(const std::exception&){status=Status::storageFailure;}
        lock.lock();const bool publish=!stale();if(publish){auto next=std::make_shared<RackLibraryInventoryView>(*state->published);next->revision=serial;next->status=status;next->busy=false;
            if(status==Status::ready&&captured){next->storeGeneration=captured->storeGeneration();next->snapshot=std::move(captured);state->generation=next->storeGeneration;}
            state->published=std::move(next);
        }lock.unlock();
#if AIRACK_LIBRARY_CACHE_TESTS
        if(hook)hook(serial,checkpoints,stale(),true,publish);
#endif
    }
    void prepareOnePlacement(){
        std::shared_ptr<RackLibraryPlacementPreparation> state;{
            const std::lock_guard lock(mutex);for(std::size_t n=0;n<placementStates.size();++n){const auto index=(placementCursor+n)%placementStates.size();if(auto candidate=placementStates[index].lock()){const std::lock_guard guard(candidate->mutex);if(candidate->pending){state=std::move(candidate);placementCursor=index+1;break;}}}}
        if(!state)return;std::unique_lock lock(state->mutex);const auto serial=state->revision.load();const auto root=state->root;const auto refs=state->refs;state->pending=false;if(!root){state->published.reset();state->preparing=false;return;}lock.unlock();
        const auto cancel=[&]{return stopping||state->revision!=serial;};auto next=std::make_shared<RackLibraryPlacementView>();next->root=root;next->revision=serial;next->status=Status::ready;next->storeGeneration=store->observedGeneration();
        try{for(const auto& ref:refs){if(cancel())return;auto lease=root->openPackage(ref,&next->status,cancel);if(!lease)break;next->leases.push_back(std::move(lease));}
            if(next->status==Status::ready){next->storeGeneration=store->generation(&next->status,cancel);if(next->status==Status::ready&&next->storeGeneration!=store->observedGeneration())next->status=Status::busy;}
        }catch(const std::exception&){next->status=Status::storageFailure;}
        if(next->status!=Status::ready)next->leases.clear();lock.lock();if(!cancel()){state->published=std::move(next);state->preparing=false;}
    }
    void retireCancelledPlacements(){
        std::vector<std::shared_ptr<const RackLibraryPlacementView>> retired;
        {
            const std::lock_guard lock(mutex);
            retired.reserve(placementStates.size());
            for(const auto& weak:placementStates)if(auto state=weak.lock()){
                const std::lock_guard guard(state->mutex);
                if(!state->root&&state->published&&state->published->revision!=state->revision)
                    retired.push_back(std::move(state->published));
            }
        }
        // Release package-tree handles on this sole worker, outside Client/state locks.
    }
    Status revealLibraryFolder(){
        const auto fixed=RackLibraryPaths::forCurrentUser().root;
        if(!store||!store->paths().root.getFullPathName().equalsIgnoreCase(fixed.getFullPathName()))return Status::unavailable;
        CacheRoot pinned;if(!pinned.open(fixed))return Status::storageFailure;
        wchar_t windowsDirectory[32768]{};const auto length=GetSystemWindowsDirectoryW(windowsDirectory,DWORD(std::size(windowsDirectory)));
        if(!length||length>=std::size(windowsDirectory))return Status::unavailable;
        const std::wstring application=std::wstring(windowsDirectory,length)+L"\\explorer.exe";
        const auto folder=fixed.getFullPathName().toWideCharPointer();
        if(std::wcschr(folder,L'"'))return Status::invalidData;
        auto command=L"\""+application+L"\" \""+std::wstring(folder)+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        if(!CreateProcessW(application.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process))return Status::unavailable;
        CloseHandle(process.hThread);CloseHandle(process.hProcess);return Status::ready;
    }
    void run(std::stop_token stop){
        while(!stop.stop_requested()){
            std::unique_lock lock(mutex);changed.wait_for(lock,stop,std::chrono::milliseconds(250),[&]{return pending.has_value()||hasInventoryWork()||hasPlacementWork()||hasSkinWork()||!thumbnailRequests.empty();});if(stop.stop_requested())break;
            if(!pending){lock.unlock();observeLocalGeneration();lock.lock();if(pending)continue;
                bool selected=false;for(unsigned n=0;n<3;++n){const auto turn=localTurn++%3;if(turn==0&&hasInventoryWork()){lock.unlock();prepareOneInventory();selected=true;break;}if(turn==1&&hasPlacementWork()){lock.unlock();prepareOnePlacement();selected=true;break;}if(turn==2&&hasSkinWork()){lock.unlock();prepareOneSkin();selected=true;break;}}
                if(selected)continue;if(thumbnailRequests.empty())continue;const auto request=thumbnailRequests.front();thumbnailRequests.pop_front();lock.unlock();try{prepareThumbnail(request);}catch(const std::exception&){auto result=std::make_shared<RackLibraryThumbnail>();result->status=Status::storageFailure;publishThumbnail(request,result);}continue;}
            const auto job=*pending;pending.reset();lock.unlock();
            try{
                if(job.kind==Job::reveal){const auto status=revealLibraryFolder();publish(job.id,[&](auto& view){view.busy=false;view.status=status;view.message=status==Status::ready?"Opened Library folder":"Library folder unavailable";});continue;}
                CacheRoot cache;Status status=Status::storageFailure;bool fromCache=false;
                if(store&&store->paths().root!=juce::File()&&cache.open(store->paths().root.getChildFile("cache"))){
                    if(job.kind==Job::refresh||job.kind==Job::open)status=refresh(cache,job,fromCache);
                    else if(job.kind==Job::remove){retireCancelledPlacements();status=store->remove(job.ref,[&]{return cancelled(job.id);});}
                    else {if(catalogBody.isEmpty())loadCache(cache);status=download(cache,job);}
                    if(status==Status::rateLimited)saveRate(cache);
                }
                if(cancelled(job.id))status=Status::cancelled;terminal(job.id,status,fromCache);
            }catch(const std::exception&){terminal(job.id,Status::storageFailure);}
        }
    }
};
RackLibraryClient::RackLibraryClient(std::shared_ptr<RackPackageStore> store,RackLibraryTransport transport):impl(std::make_unique<Impl>(std::move(store),std::move(transport))){}
RackLibraryClient::~RackLibraryClient()=default;
std::uint64_t RackLibraryClient::refresh(){return impl->submit(Impl::Job::refresh);}
std::uint64_t RackLibraryClient::openLibrary(){return impl->submit(Impl::Job::open);}
std::uint64_t RackLibraryClient::revealLibraryFolder(){
    const std::lock_guard lock(impl->mutex);if(impl->published->busy)return 0;
    const auto id=++impl->latest;impl->pending=Impl::Job{id,Impl::Job::reveal,{}};
    auto view=std::make_shared<RackLibraryClientView>(*impl->published);view->job=id;view->busy=true;view->message.clear();impl->published=std::move(view);impl->changed.notify_all();return id;
}
std::uint64_t RackLibraryClient::install(const RackPackageRef& r){return impl->submit(Impl::Job::install,r);}
std::uint64_t RackLibraryClient::restore(const RackPackageRef& r){return impl->submit(Impl::Job::restore,r);}
std::uint64_t RackLibraryClient::remove(const RackPackageRef& r){return impl->submit(Impl::Job::remove,r);}
void RackLibraryClient::cancel(std::uint64_t job){const std::lock_guard lock(impl->mutex);if(impl->published->job==job&&impl->published->busy)impl->cancelledJob=job;impl->changed.notify_all();}
std::shared_ptr<const RackLibraryClientView> RackLibraryClient::view()const{const std::lock_guard lock(impl->mutex);return impl->published;}
void RackLibraryClient::requestThumbnail(const RackPackageRef& ref,unsigned edge){
    if(!validRackPackageRef(ref)||(edge!=512&&edge!=2048))return;const std::lock_guard lock(impl->mutex);const auto key=Impl::thumbnailKey(ref,edge);
    if(std::any_of(impl->thumbnails.begin(),impl->thumbnails.end(),[&](const auto& e){return e.key==key&&nowMs()-e.checkedAt<(e.value->status==Status::ready?freshMs:60000);}))return;
    if(std::any_of(impl->thumbnailRequests.begin(),impl->thumbnailRequests.end(),[&](const auto& r){return r.ref==ref&&r.edge==edge;}))return;
    if(impl->thumbnailRequests.size()>=16)impl->thumbnailRequests.pop_front();impl->thumbnailRequests.push_back({ref,edge});impl->changed.notify_all();
}
std::shared_ptr<const RackLibraryThumbnail> RackLibraryClient::thumbnail(const RackPackageRef& ref,unsigned edge)const{
    const std::lock_guard lock(impl->mutex);const auto key=Impl::thumbnailKey(ref,edge);for(auto& entry:impl->thumbnails)if(entry.key==key){entry.use=++impl->thumbnailUse;return entry.value;}return {};
}
RackLibrarySkinPreparationPtr RackLibraryClient::createSkinPreparation(RackLibrarySkinPreparer prepare){
    if(!prepare)return {};auto state=std::make_shared<RackLibrarySkinPreparation>();state->prepare=std::move(prepare);const std::lock_guard lock(impl->mutex);state->generation=impl->published->generation;std::erase_if(impl->skinStates,[](const auto& weak){return weak.expired();});impl->skinStates.push_back(state);return state;
}
void RackLibraryClient::requestBuiltInThumbnail(const RackMetadata& metadata,unsigned edge){
    if(metadata.design.isNull()||!validRackPackageHash(metadata.canonicalSha256)||(edge!=512&&edge!=2048))return;
    const RackPackageRef identity{metadata.design.toString(),String(metadata.revision),metadata.canonicalSha256};
    const std::lock_guard lock(impl->mutex);const auto key=Impl::thumbnailKey(identity,edge,true);
    if(std::any_of(impl->thumbnails.begin(),impl->thumbnails.end(),[&](const auto& e){return e.key==key;}))return;
    if(std::any_of(impl->thumbnailRequests.begin(),impl->thumbnailRequests.end(),[&](const auto& r){return r.builtIn&&r.ref==identity&&r.edge==edge;}))return;
    if(impl->thumbnailRequests.size()>=16)impl->thumbnailRequests.pop_front();impl->thumbnailRequests.push_back({identity,edge,true});impl->changed.notify_all();
}
std::shared_ptr<const RackLibraryThumbnail> RackLibraryClient::builtInThumbnail(const RackMetadata& metadata,unsigned edge)const{
    const RackPackageRef identity{metadata.design.toString(),String(metadata.revision),metadata.canonicalSha256};
    const std::lock_guard lock(impl->mutex);const auto key=Impl::thumbnailKey(identity,edge,true);
    for(auto& entry:impl->thumbnails)if(entry.key==key){entry.use=++impl->thumbnailUse;return entry.value;}return {};
}
void RackLibraryClient::prepareSkins(const RackLibrarySkinPreparationPtr& state,RackInventoryPtr inventory,std::span<const RackInstance> racks){
    if(!state||racks.size()>12)return;{
        const std::lock_guard control(impl->mutex);
        const std::lock_guard lock(state->mutex);const bool changed=inventory&&inventory!=state->inventory;
        if(!changed&&(inventory||!state->preview)&&racks.size()==state->used.size()&&std::equal(racks.begin(),racks.end(),state->used.begin(),RackLibrarySkinPreparation::same))return;
        if(inventory)state->inventory=std::move(inventory);else state->preview.reset();state->used.assign(racks.begin(),racks.end());state->rebuild();state->pending=true;++state->revision;
    }impl->changed.notify_all();
}
void RackLibraryClient::requestSkin(const RackLibrarySkinPreparationPtr& state,const RackInstance& rack){
    if(!state)return;{const std::lock_guard control(impl->mutex);const std::lock_guard lock(state->mutex);if(std::any_of(state->requested.begin(),state->requested.end(),[&](const auto& r){return RackLibrarySkinPreparation::same(r,rack);}))return;
        state->preview=rack;state->rebuild();state->pending=true;++state->revision;}impl->changed.notify_all();
}
std::shared_ptr<const studio::MakerSkin> RackLibraryClient::preparedSkin(const RackLibrarySkinPreparationPtr& state,const RackInstance& rack)const{
    if(!state)return {};const std::lock_guard lock(state->mutex);for(const auto& [r,owner]:state->owners)if(RackLibrarySkinPreparation::same(r,rack))return owner;return {};
}
RackLibraryInventoryPreparationPtr RackLibraryClient::createInventoryPreparation(){
    auto state=std::make_shared<RackLibraryInventoryPreparation>();const std::lock_guard lock(impl->mutex);std::erase_if(impl->inventoryStates,[](const auto& weak){return weak.expired();});impl->inventoryStates.push_back(state);return state;
}
void RackLibraryClient::requestInventory(const RackLibraryInventoryPreparationPtr& state,std::span<const RackInstance> embedded){
    if(!state||embedded.size()>12)return;{const std::lock_guard lock(state->mutex);
        if(state->active&&embedded.size()==state->embedded.size()&&std::equal(embedded.begin(),embedded.end(),state->embedded.begin(),RackLibrarySkinPreparation::same))return;
        state->embedded.assign(embedded.begin(),embedded.end());state->active=true;state->pending=true;++state->revision;
        auto next=std::make_shared<RackLibraryInventoryView>(*state->published);next->status=Status::busy;next->busy=true;next->revision=state->revision;state->published=std::move(next);
    }++impl->localRevision;impl->changed.notify_all();
}
void RackLibraryClient::retryInventory(const RackLibraryInventoryPreparationPtr& state){
    if(!state)return;{const std::lock_guard lock(state->mutex);if(!state->active||state->pending||state->published->busy||state->published->status==Status::ready)return;
        ++state->revision;state->pending=true;auto next=std::make_shared<RackLibraryInventoryView>(*state->published);next->status=Status::busy;next->busy=true;next->revision=state->revision;state->published=std::move(next);
    }++impl->localRevision;impl->changed.notify_all();
}
void RackLibraryClient::cancelInventory(const RackLibraryInventoryPreparationPtr& state){
    if(!state)return;{const std::lock_guard lock(state->mutex);++state->revision;state->pending=false;state->active=false;state->embedded.clear();state->published=std::make_shared<RackLibraryInventoryView>();}
    ++impl->localRevision;impl->changed.notify_all();
}
std::shared_ptr<const RackLibraryInventoryView> RackLibraryClient::inventory(const RackLibraryInventoryPreparationPtr& state) const{
    if(!state)return {};const std::lock_guard lock(state->mutex);return state->published;
}
RackLibraryPlacementPreparationPtr RackLibraryClient::createPlacementPreparation(){
    auto state=std::make_shared<RackLibraryPlacementPreparation>();const std::lock_guard lock(impl->mutex);std::erase_if(impl->placementStates,[](const auto& weak){return weak.expired();});impl->placementStates.push_back(state);return state;
}
void RackLibraryClient::requestPlacement(const RackLibraryPlacementPreparationPtr& state,RackInventoryPtr root,std::span<const RackPackageRef> refs){
    if(!state||!root||refs.size()>12)return;{const std::lock_guard lock(state->mutex);
        if(root==state->root&&refs.size()==state->refs.size()&&std::equal(refs.begin(),refs.end(),state->refs.begin())
            &&(state->preparing||(state->published&&state->published->status==Status::ready&&state->published->storeGeneration==impl->store->observedGeneration())))return;
        state->root=std::move(root);state->refs.assign(refs.begin(),refs.end());++state->revision;state->pending=true;state->preparing=true;
    }impl->changed.notify_all();
}
void RackLibraryClient::cancelPlacement(const RackLibraryPlacementPreparationPtr& state){
    if(!state)return;const std::lock_guard lock(state->mutex);++state->revision;state->pending=true;state->preparing=true;state->root.reset();state->refs.clear();impl->changed.notify_all();
}
std::shared_ptr<const RackLibraryPlacementView> RackLibraryClient::placement(const RackLibraryPlacementPreparationPtr& state) const{
    if(!state)return {};const std::lock_guard lock(state->mutex);if(state->published&&state->published->revision==state->revision)return state->published;auto pending=std::make_shared<RackLibraryPlacementView>();pending->root=state->root;pending->revision=state->revision;pending->status=state->root?Status::busy:Status::unavailable;return pending;
}
bool RackLibraryClient::placementCurrent(const RackLibraryPlacementPreparationPtr& state,const RackLibraryPlacementView& view) const noexcept{
    return state&&view.status==Status::ready&&state->revision==view.revision&&impl->store->observedGeneration()==view.storeGeneration;
}
std::shared_ptr<RackLibraryRuntime> defaultRackLibraryRuntime(){
    static std::mutex mutex;static std::map<String,std::weak_ptr<RackLibraryRuntime>> registry;const std::lock_guard lock(mutex);
    const auto paths=RackLibraryPaths::forCurrentUser();const auto key=paths.root.getFullPathName().replaceCharacter('\\','/').toLowerCase();if(auto found=registry[key].lock())return found;
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(paths);runtime->client=std::make_shared<RackLibraryClient>(runtime->store,rackLibraryHttpWindows());registry[key]=runtime;return runtime;
}

}
