#include "../../tests/native/FrozenRackExamples.h"
#include "CompanionClient.h"
#include "AiCatalog.h"
#include "AiListeningBriefs.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "StrictJson.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShellApi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace airack;
using namespace airack::companion;
namespace
{
std::atomic_bool cancelled{false};
// Every Client call stays on main; the console handler only publishes a flag.
BOOL WINAPI consoleControl(DWORD event)
{
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT || event==CTRL_LOGOFF_EVENT || event==CTRL_SHUTDOWN_EVENT){cancelled.store(true,std::memory_order_release);return TRUE;}return FALSE;
}
struct Handle
{
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE h):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields){auto* result=new juce::DynamicObject;for(const auto& field:fields)result->setProperty(field.first,field.second);return result;}
// JUCE's precision is decimal places: 22 preserves raw double metrics in its
// fixed interval and remains within its 48-byte fixed/scientific number buffer.
juce::String json(const juce::var& value)
{
    const auto formatted=juce::JSON::toString(value,true,22).toStdString();std::string result;result.reserve(formatted.size());
    // The pinned JUCE parser counts leading fractional zeros in its 18-digit
    // limit. Move only the decimal point of existing number tokens; JUCE still
    // owns all structural/string encoding, and no numeric rounding occurs here.
    for(std::size_t i=0;i<formatted.size();){const auto begin=i;const auto c=formatted[i];
        if(c=='"'){++i;while(i<formatted.size()){if(formatted[i]=='\\'){i=std::min(i+2,formatted.size());continue;}if(formatted[i++]=='"')break;}result.append(formatted,begin,i-begin);continue;}
        if(c=='-' || (c>='0' && c<='9')){++i;while(i<formatted.size() && std::string_view("0123456789.eE+-").find(formatted[i])!=std::string_view::npos)++i;
            const auto token=formatted.substr(begin,i-begin);const std::size_t sign=token[0]=='-'?1:0;
            if(token.compare(sign,2,"0.")==0 && token.find_first_of("eE")==std::string::npos){auto first=sign+2;while(first<token.size() && token[first]=='0')++first;
                if(first<token.size()){result.append(token,0,sign);result+=token[first];if(first+1<token.size()){result+='.';result.append(token,first+1,std::string::npos);}result+="e-";result+=std::to_string(first-sign-1);continue;}}
            result+=token;continue;}
        result+=c;++i;
    }return juce::String::fromUTF8(result.data(),int(result.size()));
}
void set(juce::var& value,const char* name,const juce::var& field){value.getDynamicObject()->setProperty(name,field);}
WireBytes bytes(const juce::MemoryBlock& data){return {static_cast<const std::byte*>(data.getData()),data.getSize()};}
juce::String safeCode(const juce::String& code){if(code.isEmpty())return {};return code.length()<=64 && code.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789_")?code:juce::String("public_error");}
juce::String display(const juce::String& value){juce::String result;for(auto c:value)if(c>=32 && c!=127)result+=juce::String::charToString(c);return result;}
struct Arguments
{
    enum class Action{invalid,help,status,models,selectModel,signIn,generate};
    Action action=Action::invalid;bool synthetic=false;juce::String model,suffix;juce::File instruction,output,fixture;std::string instructionText;
    const listening::AiBrief* brief=nullptr;
};
const char* actionName(Arguments::Action action)
{
    switch(action){case Arguments::Action::help:return "help";case Arguments::Action::status:return "status";case Arguments::Action::models:return "models";case Arguments::Action::selectModel:return "selectModel";case Arguments::Action::signIn:return "signIn";case Arguments::Action::generate:return "generate";default:return "invalid";}
}
bool absolute(const std::wstring& raw,juce::File& output)
{
    // Reject drive-relative/device namespaces before File normalization.
    if(raw.size()<3 || raw.size()>=32767 || !((raw[0]>=L'A' && raw[0]<=L'Z') || (raw[0]>=L'a' && raw[0]<=L'z')) || raw[1]!=L':' || (raw[2]!=L'\\' && raw[2]!=L'/'))return false;
    std::vector<wchar_t> complete(32768);const auto size=GetFullPathNameW(raw.c_str(),DWORD(complete.size()),complete.data(),nullptr);if(!size || size>=complete.size())return false;
    const std::filesystem::path path(complete.data());for(const auto& part:path.relative_path()){
        const auto component=part.wstring();if(component.empty() || component.back()==L'.' || component.back()==L' ' || component.find_first_of(L":*?\"")!=std::wstring::npos)return false;
        const auto stem=juce::String(component.c_str()).upToFirstOccurrenceOf(".",false,false).toUpperCase();
        if(stem=="CON" || stem=="PRN" || stem=="AUX" || stem=="NUL" || (stem.length()==4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3]>='1' && stem[3]<='9'))return false;
    }output=juce::File(juce::String(complete.data()));return true;
}
bool plainPath(const juce::File& file,bool directory)
{
    const std::filesystem::path path(file.getFullPathName().toWideCharPointer());auto current=path.root_path();for(const auto& part:path.relative_path()){
        current/=part;const auto attributes=GetFileAttributesW(current.c_str());if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT) || (current!=path && !(attributes&FILE_ATTRIBUTE_DIRECTORY)))return false;
    }const auto attributes=GetFileAttributesW(path.c_str());return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT) && bool(attributes&FILE_ATTRIBUTE_DIRECTORY)==directory;
}
juce::File hostRoot(){return juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures");}
bool newOutput(const juce::File& output){const auto root=hostRoot();return plainPath(root,true) && output.getParentDirectory()==root && output!=root && GetFileAttributesW(output.getFullPathName().toWideCharPointer())==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_FILE_NOT_FOUND;}
bool readInstruction(const juce::File& file,std::string& text)
{
    if(!file.isAChildOf(hostRoot()) || !plainPath(hostRoot(),true) || !plainPath(file,false))return false;
    Handle handle(CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION information{};LARGE_INTEGER size{};
    if(handle.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value,&information) || information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY) || !GetFileSizeEx(handle.value,&size) || size.QuadPart<1 || size.QuadPart>8192)return false;
    text.resize(std::size_t(size.QuadPart));DWORD count=0;if(!ReadFile(handle.value,text.data(),DWORD(text.size()),&count,nullptr) || count!=text.size() || text.find('\0')!=std::string::npos)return false;
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0)>0;
}
bool parse(Arguments& parsed)
{
    int count=0;auto** argv=CommandLineToArgvW(GetCommandLineW(),&count);struct Allocation{LPWSTR* value;~Allocation(){if(value)LocalFree(value);}} allocation{argv};if(!argv || count<2)return false;
    if(count==2 && std::wstring_view(argv[1])==L"--help"){parsed.action=Arguments::Action::help;return true;}
    std::map<std::wstring,std::wstring> fields;Arguments::Action action=Arguments::Action::invalid;bool syntaxValid=true,fixtureUnique=true;
    for(int i=1;i<count;++i){const std::wstring key=argv[i];Arguments::Action next=Arguments::Action::invalid;
        if(key==L"--status")next=Arguments::Action::status;else if(key==L"--models")next=Arguments::Action::models;else if(key==L"--select-model")next=Arguments::Action::selectModel;else if(key==L"--sign-in")next=Arguments::Action::signIn;else if(key==L"--generate")next=Arguments::Action::generate;
        if(next!=Arguments::Action::invalid){if(action!=Arguments::Action::invalid)syntaxValid=false;else action=next;if(next==Arguments::Action::selectModel){if(++i>=count){syntaxValid=false;break;}if(!fields.emplace(L"--model",argv[i]).second)syntaxValid=false;}continue;}
        if(key!=L"--model" && key!=L"--instruction-file" && key!=L"--brief" && key!=L"--output" && key!=L"--fixture-root" && key!=L"--pipe-suffix"){syntaxValid=false;continue;}if((key==L"--fixture-root" || key==L"--pipe-suffix") && fields.contains(key))fixtureUnique=false;if(++i>=count){syntaxValid=false;break;}if(!fields.emplace(key,argv[i]).second)syntaxValid=false;
    }
    const bool fixture=fields.contains(L"--fixture-root"),suffix=fields.contains(L"--pipe-suffix");if(!fixtureUnique || fixture!=suffix)return false;
    if(fixture){const auto& s=fields[L"--pipe-suffix"];if(s.empty() || s.size()>32 || !std::all_of(s.begin(),s.end(),[](wchar_t c){return (c>=L'A' && c<=L'Z') || (c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9') || c==L'_' || c==L'-';}) || !absolute(fields[L"--fixture-root"],parsed.fixture) || !parsed.fixture.isAChildOf(hostRoot()) || !plainPath(parsed.fixture,true))return false;parsed.suffix=juce::String(s.c_str());}parsed.synthetic=fixture;if(!syntaxValid || action==Arguments::Action::invalid)return false;
    const bool needsModel=action==Arguments::Action::selectModel || action==Arguments::Action::generate;
    // A generation takes its prompt from exactly one of an instruction file and a named brief.
    const bool briefed=fields.contains(L"--brief"),instructed=fields.contains(L"--instruction-file"),generating=action==Arguments::Action::generate;
    if(fields.contains(L"--model")!=needsModel || (generating?briefed==instructed:briefed || instructed) || fields.contains(L"--output")!=generating)return false;
    if(needsModel){parsed.model=juce::String(fields[L"--model"].c_str());juce::MemoryBlock encoded;if(!encodeAccountCommand({AccountAction::selectModel,parsed.model},encoded))return false;}
    if(briefed){const juce::String id(fields[L"--brief"].c_str());parsed.brief=listening::aiBrief(id.toStdString());if(!parsed.brief)return false;}
    if(generating && (!absolute(fields[L"--output"],parsed.output) || !newOutput(parsed.output) || (instructed && (!absolute(fields[L"--instruction-file"],parsed.instruction) || !readInstruction(parsed.instruction,parsed.instructionText)))))return false;
    parsed.action=action;return true;
}
juce::var peerRecord(PipePeer peer){return object({{"processId",juce::int64(peer.processId)},{"creationTime",juce::String(peer.creationTime)}});}
juce::var emptyHelper(){return object({{"owned",false},{"imagePath",""},{"sha256",""},{"processId",0},{"creationTime","0"},{"peerMatches",false}});}
juce::var replyFor(const Arguments& arguments)
{
    auto reply=object({{"version",1},{"action",actionName(arguments.action)},{"synthetic",arguments.synthetic},{"runtime",juce::Uuid::null().toString()},{"session",juce::Uuid::null().toString()},{"runtimeStatus",int(RuntimeStatus::unavailable)},{"providerStatus",int(rack::oauth::Status::ready)},{"httpStatus",0},{"errorCode",""},{"selectedModel",""},{"modelVerified",false},{"peer",peerRecord({})},{"helper",emptyHelper()}});
    if(arguments.action==Arguments::Action::generate){set(reply,"generationStatus",int(ai::GenerationStatus::invalidRequest));set(reply,"proposalStatus",int(ai::ProposalStatus::malformed));set(reply,"renderStatus",int(ai::RenderStatus::invalidInput));set(reply,"inferenceCalls",0);}else set(reply,"models",juce::Array<juce::var>{});return reply;
}
struct ImageIdentity{juce::File path;juce::String sha256;std::uint64_t size=0,index=0,writeTime=0;DWORD volume=0;bool operator==(const ImageIdentity&) const = default;};
std::uint64_t timeValue(FILETIME time){return (std::uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
bool imageIdentity(const juce::File& file,ImageIdentity& identity)
{
    Handle handle(CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
    if(handle.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value,&info) || info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))return false;
    identity={file,juce::SHA256(file).toHexString(),(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,(std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow,timeValue(info.ftLastWriteTime),info.dwVolumeSerialNumber};return identity.size && identity.sha256.length()==64;
}
juce::var identityRecord(const ImageIdentity& identity){return object({{"imagePath",identity.path.getFullPathName()},{"sha256",identity.sha256},{"bytes",juce::int64(identity.size)},{"fileIndex",juce::String(identity.index)},{"volumeSerial",juce::int64(identity.volume)},{"lastWriteTime",juce::String(identity.writeTime)}});}
bool provenance(const CompanionClient& client,const ImageIdentity& before,juce::var& output)
{
    output=emptyHelper();set(output,"reviewedFileBefore",identityRecord(before));const auto handle=static_cast<HANDLE>(airack::companion::detail::CompanionClientTestAccess::borrowProcessHandle(client));if(!handle)return true;
    set(output,"owned",true);std::vector<wchar_t> image(32768);DWORD size=DWORD(image.size());FILETIME created{},exited{},kernel{},user{};
    if(!QueryFullProcessImageNameW(handle,0,image.data(),&size) || !GetProcessTimes(handle,&created,&exited,&kernel,&user))return false;
    const juce::File loaded(juce::String(image.data(),int(size)));const auto processId=GetProcessId(handle);const auto creationTime=timeValue(created);ImageIdentity after;
    set(output,"imagePath",loaded.getFullPathName());set(output,"processId",juce::int64(processId));set(output,"creationTime",juce::String(creationTime));if(loaded!=before.path || !imageIdentity(loaded,after))return false;
    set(output,"sha256",after.sha256);set(output,"loadedFileAfter",identityRecord(after));const auto peer=client.peer();const bool matches=peer.processId==processId && peer.creationTime==creationTime && processId && creationTime;set(output,"peerMatches",matches);return before==after && matches;
}
bool writeNew(const juce::File& file,const void* data,std::size_t size)
{
    if(!plainPath(file.getParentDirectory(),true) || size>DWORD(-1))return false;Handle handle(CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(handle.value==INVALID_HANDLE_VALUE)return false;DWORD count=0;return WriteFile(handle.value,data,DWORD(size),&count,nullptr)!=FALSE && count==size && FlushFileBuffers(handle.value)!=FALSE;
}
bool exactFile(const juce::File& file,const void* data,std::size_t size){juce::MemoryBlock block;return plainPath(file,false) && file.loadFileAsData(block) && block.getSize()==size && (!size || std::memcmp(block.getData(),data,size)==0);}
bool floatBits(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0);}
struct Exports
{
    juce::File root;Handle hostLease,outputLease,proofLease;bool owned=false;std::map<juce::String,juce::MemoryBlock> captured;
    bool create(const juce::File& output)
    {
        if(!newOutput(output))return false;hostLease.value=CreateFileW(hostRoot().getFullPathName().toWideCharPointer(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(hostLease.value==INVALID_HANDLE_VALUE || !newOutput(output) || !CreateDirectoryW(output.getFullPathName().toWideCharPointer(),nullptr))return false;root=output;owned=true;outputLease.value=CreateFileW(root.getFullPathName().toWideCharPointer(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);return outputLease.value!=INVALID_HANDLE_VALUE && plainPath(root,true);
    }
    bool save(const char* name,const juce::MemoryBlock& data){if(!owned || !writeNew(root.getChildFile(name),data.getData(),data.getSize()))return false;captured.emplace(name,data);return exactFile(root.getChildFile(name),data.getData(),data.getSize());}
    bool text(const char* name,const juce::String& value){return save(name,juce::MemoryBlock(value.toRawUTF8(),value.getNumBytesAsUTF8()));}
    bool readback() const{for(const auto& [name,data]:captured)if(!exactFile(root.getChildFile(name),data.getData(),data.getSize()))return false;return true;}
    juce::var artifacts() const{juce::Array<juce::var> records;for(const auto& [name,data]:captured)records.add(object({{"name",name},{"bytes",juce::int64(data.getSize())},{"sha256",juce::SHA256(data).toHexString()}}));return records;}
    bool proof(const juce::var& value)
    {
        if(!owned || !plainPath(root,true))return false;const auto file=root.getChildFile("proof.json");const auto contents=json(value);
        // Keep the original child handle alive; every update targets that file.
        if(proofLease.value==INVALID_HANDLE_VALUE)proofLease.value=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER beginning{};DWORD count=0;if(proofLease.value==INVALID_HANDLE_VALUE || !SetFilePointerEx(proofLease.value,beginning,nullptr,FILE_BEGIN) || !WriteFile(proofLease.value,contents.toRawUTF8(),DWORD(contents.getNumBytesAsUTF8()),&count,nullptr) || count!=contents.getNumBytesAsUTF8() || !SetEndOfFile(proofLease.value) || !FlushFileBuffers(proofLease.value))return false;
        return exactFile(file,contents.toRawUTF8(),contents.getNumBytesAsUTF8());
    }
};
juce::var catalogMetadata(const ai::GenerationRequest& r,const char* intentCapture)
{
    if(!r.catalog)return {};const auto& c=*r.catalog;const auto& a=*c.authority;
    auto tag=[](const ai::RequestTag& t){return object({{"runtime",t.runtime.toString()},{"requestId",juce::String(std::to_string(t.requestId))},{"editRevision",juce::String(std::to_string(t.editRevision))},{"contextRevision",juce::String(std::to_string(t.contextRevision))}});};
    juce::Array<juce::var> definitions;for(const auto& d:a.definitions)definitions.add(object({{"design",d.design.toDashedString()},{"revision",juce::String(std::to_string(d.revision))},{"contentSha256",d.contentSha256},{"canonicalBytes",juce::int64(d.canonical.getSize())},{"canonicalBase64",d.canonical.toBase64Encoding()}}));
    return object({{"version",int(c.version)},{"phase",c.phase==ai::CatalogPhase::select?"select":"materialize"},{"root",tag(c.root)},{"selectionParent",c.selectionParent?tag(*c.selectionParent):juce::var{}},
        {"snapshot",a.snapshot.toString()},{"descriptorJson",juce::String::fromUTF8(a.descriptorJson.data(),int(a.descriptorJson.size()))},{"definitions",definitions},
        {"baseIntentCapture",intentCapture},{"inferenceCallsUsed",int(c.inferenceCallsUsed)},{"providerStatus",int(c.providerStatus)},{"httpStatus",c.httpStatus}});
}
bool captureCatalog(ai::GenerationRequest& request,std::shared_ptr<const RackCatalogSnapshot>& retained)
{
    std::vector<RackDesign> embedded;
    if(request.source.sound.personalities)for(const auto& i:request.source.sound.personalities->instances)embedded.push_back(i.design);
    auto library=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
    std::erase_if(library,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
    std::erase_if(library,[&](const auto& design){return design.id==tile;});
    if(library.size()!=25)return false; // This probe preserves the explicit historical melodic transport.
    RackCatalogSnapshot snapshot;juce::var descriptor;
    if(!captureRackCatalog(library,embedded,{},snapshot)||!ai::catalogInventoryFor(snapshot,descriptor))return false;
    auto authority=std::make_shared<ai::CatalogAuthority>();authority->root=request.tag;authority->snapshot=snapshot.id;
    authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
    ai::CatalogRequestContext context;context.root=request.tag;context.authority=authority;auto proposed=request;proposed.catalog=std::move(context);juce::MemoryBlock encoded;
    if(!encodeRequest(proposed,encoded))return false;retained=std::make_shared<const RackCatalogSnapshot>(std::move(snapshot));request=std::move(proposed);return true;
}
juce::var phraseRecord(const PatternDocument& p)
{
    juce::Array<juce::var> notes;
    for(std::size_t i=0;i<p.noteCount;++i){const auto& n=p.notes[i];notes.add(object({{"start_beats",n.startBeats},{"duration_beats",n.durationBeats},{"pitch",int(n.pitch)},{"velocity",int(n.velocity)},{"channel",int(n.channel)}}));}
    return object({{"bars",int(p.bars)},{"meter_numerator",int(p.meter.numerator)},{"meter_denominator",int(p.meter.denominator)},{"grid",int(p.grid)},{"swing",p.swing},{"notes",notes}});
}
juce::var requestMetadata(const ai::GenerationRequest& r,const juce::MemoryBlock& source,const juce::String& catalog,const juce::String& schema,const juce::var& brief,const char* intentCapture="request.bin")
{
    auto metadata=object({{"version",1},{"model",r.model},{"brief",brief},{"tag",object({{"runtime",r.tag.runtime.toString()},{"requestId",juce::int64(r.tag.requestId)},{"editRevision",juce::int64(r.tag.editRevision)},{"contextRevision",juce::int64(r.tag.contextRevision)}})},
        {"policy",object({{"scope","sound"},{"allowMacroChanges",false},{"allowSlotReuse",false}})},
        {"context",object({{"role",juce::String(r.role)},{"key",juce::String(r.key)},{"scale",juce::String(r.scale)},{"capturedBpm",r.capturedBpm?juce::var(*r.capturedBpm):juce::var{}},{"capturedPhrase",r.capturedPhrase?phraseRecord(*r.capturedPhrase):juce::var{}}})},
        {"sourceSha256",juce::SHA256(source).toHexString()},{"sourceBytes",juce::int64(source.getSize())},{"catalogSha256",juce::SHA256(catalog.toRawUTF8(),catalog.getNumBytesAsUTF8()).toHexString()},{"schemaSha256",juce::SHA256(schema.toRawUTF8(),schema.getNumBytesAsUTF8()).toHexString()}});
    if(r.catalog)set(metadata,"catalog",catalogMetadata(r,intentCapture));return metadata;
}
bool captureRequest(Exports& output,const ai::GenerationRequest& request,const listening::AiBrief* brief,juce::var& metadata)
{
    juce::MemoryBlock source;juce::var catalog;ProjectDocument decoded;if(!ai::detail::catalogRequestValid(request)||!validateProjectDocument(request.source) || !encodeProjectState(request.source,source) || !decodeProjectState(source.getData(),source.getSize(),decoded) || decoded!=request.source || !ai::catalogFor(request.source,catalog))return false;
    const auto* controls=catalog["controls"].getArray();if(int(catalog["version"])!=1 || int(catalog["limits"]["parameters"])!=int(parameterCount) || !controls || controls->size()!=int(parameterCount))return false;
    if(request.catalog){juce::var inventory;if(!ai::parseStrictJson(request.catalog->authority->descriptorJson,inventory,ai::maximumCatalogDescriptorBytes)||!bool(inventory["complete"]))return false;set(catalog,"rack_catalog",inventory);}
    const auto catalogText=json(catalog),schemaText=json(request.catalog?ai::proposalSchema(request.policy,*request.catalog->authority):ai::proposalSchema(request.policy));metadata=requestMetadata(request,source,catalogText,schemaText,brief?juce::var(brief->id):juce::var{});
    juce::MemoryBlock wire,again;ai::GenerationRequest recalled;if(!encodeRequest(request,wire)||!decodeRequest(bytes(wire),recalled)||!encodeRequest(recalled,again)||again!=wire)return false;
    return output.save("source.airackproject",source) && output.save("request.bin",wire) && output.text("catalog.json",catalogText) && output.text("schema.json",schemaText) && output.save("instruction.txt",juce::MemoryBlock(request.instruction.data(),request.instruction.size())) && output.text("request.json",json(metadata)) && output.readback();
}
bool materializeSelection(Exports& output,const ai::GenerationRequest& initial,const ai::GenerationResult& selection,const RackCatalogSnapshot& retained,ai::GenerationRequest& next,juce::var& metadata)
{
    if(!ai::detail::catalogRequestValid(initial)||!initial.catalog||initial.catalog->phase!=ai::CatalogPhase::select||selection.status!=ai::GenerationStatus::catalogSelectionRequired
        ||!selection.catalog||selection.catalog->root!=initial.catalog->root||selection.catalog->snapshot!=retained.id||retained.id!=initial.catalog->authority->snapshot||initial.tag.requestId==UINT64_MAX)return false;
    juce::MemoryBlock selectionWire;if(!encodeResult(initial,selection,selectionWire))return false;
    auto authority=std::make_shared<ai::CatalogAuthority>(*initial.catalog->authority);authority->definitions.clear();
    for(const auto& s:selection.edits.catalogSelections){if(std::any_of(authority->definitions.begin(),authority->definitions.end(),[&](const auto& d){return d.design==s.design&&d.revision==s.revision&&d.contentSha256==s.contentSha256;}))continue;
        RackDesign design;ai::CatalogDefinition d;d.design=s.design;d.revision=s.revision;d.contentSha256=s.contentSha256;
        if(!resolveCatalogDesign(retained,s.design,s.revision,s.contentSha256,design)||!encodeRackDesign(design,d.canonical,RackValidationMode::instanceRecall)||juce::SHA256(d.canonical).toHexString()!=s.contentSha256)return false;
        authority->definitions.push_back(std::move(d));}
    auto materialized=initial;++materialized.tag.requestId;auto& c=*materialized.catalog;c.phase=ai::CatalogPhase::materialize;c.selectionParent=selection.tag;c.authority=authority;c.baseIntent=selection.edits;
    c.inferenceCallsUsed=static_cast<std::uint8_t>(selection.inferenceCalls);c.providerStatus=selection.providerStatus;c.httpStatus=selection.httpStatus;
    juce::MemoryBlock requestWire,again,source;ai::GenerationRequest recalled;if(!encodeRequest(materialized,requestWire)||!decodeRequest(bytes(requestWire),recalled)||!encodeRequest(recalled,again)||again!=requestWire||!encodeProjectState(initial.source,source))return false;
    const auto catalogText=output.root.getChildFile("catalog.json").loadFileAsString();const auto schemaText=output.root.getChildFile("schema.json").loadFileAsString();const auto record=requestMetadata(materialized,source,catalogText,schemaText,metadata["brief"],"materialize-request.bin");
    if(!output.save("selection.bin",selectionWire)||!output.save("materialize-request.bin",requestWire)||!output.text("materialize-request.json",json(record))||!output.readback())return false;
    set(metadata,"materialize",record);next=std::move(materialized);return true;
}
juce::var measurements(const ai::AudioMeasurements& m)
{
    juce::Array<juce::var> bands;for(auto value:m.bandFractions)bands.add(value);return object({{"finite",m.finite},{"clippedSamples",juce::int64(m.clippedSamples)},{"peak",m.peak},{"rms",m.rms},{"onsetSeconds",m.onsetSeconds?juce::var(*m.onsetSeconds):juce::var{}},{"activityEndSeconds",m.activityEndSeconds},{"decaySeconds",m.decaySeconds},{"bandFractions",bands},{"tailActiveAtEnd",m.tailActiveAtEnd}});
}
bool writeWav(Exports& output,const ai::PreviewAudio& audio)
{
    const auto file=output.root.getChildFile("preview.wav");juce::MemoryBlock physical;
    // Encode within the admitted frame bound, then create the physical file once.
    std::unique_ptr<juce::OutputStream> stream=std::make_unique<juce::MemoryOutputStream>(physical,false);juce::WavAudioFormat format;
    const auto options=juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);auto writer=format.createWriterFor(stream,options);const float* channels[]={audio.left.data(),audio.right.data()};
    if(!writer || !writer->writeFromFloatArrays(channels,2,int(audio.left.size())) || !writer->flush())return false;writer.reset();if(!output.save("preview.wav",physical))return false;auto input=file.createInputStream();if(!input)return false;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(input.release(),true));
    if(!reader || !reader->usesFloatingPointData || reader->bitsPerSample!=32 || reader->numChannels!=2 || reader->sampleRate!=48000 || reader->lengthInSamples!=juce::int64(audio.left.size()))return false;juce::AudioBuffer<float> read(2,int(audio.left.size()));
    if(!reader->read(&read,0,read.getNumSamples(),0,true,true) || std::memcmp(read.getReadPointer(0),audio.left.data(),audio.left.size()*4) || std::memcmp(read.getReadPointer(1),audio.right.data(),audio.right.size()*4))return false;
    return output.readback();
}
bool admittedExports(Exports& output,const ai::GenerationRequest& request,const ai::GenerationResult& result,ai::AudioMeasurements& metrics)
{
    if(!ai::detail::catalogRequestValid(request)||result.tag!=request.tag || result.status!=ai::GenerationStatus::ready || result.proposalStatus!=ai::ProposalStatus::accepted || result.renderStatus!=ai::RenderStatus::ready || result.providerStatus!=rack::oauth::Status::connected || result.httpStatus!=200 || result.inferenceCalls<1 || result.inferenceCalls>3 || !result.candidate || result.errorCode.isNotEmpty())return false;
    ProjectDocument admitted,roundtrip;juce::MemoryBlock candidate,expected,source,encoded;
    const auto status=request.catalog?ai::admitProposal(request.source,request.policy,result.edits,*request.catalog->authority,admitted):ai::admitProposal(request.source,request.policy,result.edits,admitted);
    if(status!=ai::ProposalStatus::accepted || admitted!=*result.candidate || !encodeProjectState(admitted,expected) || !encodeProjectState(*result.candidate,candidate) || candidate!=expected || !decodeProjectState(candidate.getData(),candidate.getSize(),roundtrip) || roundtrip!=admitted || !encodeProjectState(roundtrip,expected) || expected!=candidate || !output.root.getChildFile("source.airackproject").loadFileAsData(source) || !decodeProjectState(source.getData(),source.getSize(),roundtrip) || roundtrip!=request.source || !encodeProjectState(roundtrip,expected) || expected!=source)return false;
    const auto& audio=result.audio;if(audio.sampleRate!=48000 || audio.left.empty() || audio.left.size()!=audio.right.size() || audio.left.size()>ai::maximumPreviewFrames || audio.bodyFrames>ai::maximumPreviewBodyFrames || audio.left.size()!=audio.bodyFrames+ai::previewTailFrames || ai::measurePreview(audio.left,audio.right,audio.bodyFrames,metrics,[]{return cancelled.load(std::memory_order_acquire);})!=ai::RenderStatus::ready || !metrics.finite || metrics.clippedSamples || json(measurements(metrics))!=json(measurements(result.measurements)))return false;
    ai::GenerationResult decoded;if(!encodeResult(request,result,encoded) || !decodeResult(bytes(encoded),request,decoded) || decoded.tag!=request.tag || decoded.candidate!=result.candidate || !floatBits(decoded.audio.left,audio.left) || !floatBits(decoded.audio.right,audio.right))return false;juce::MemoryBlock encodedAgain;if(!encodeResult(request,decoded,encodedAgain) || encodedAgain!=encoded)return false;
    if(!output.save("candidate.airackproject",candidate) || !output.save("result.bin",encoded) || !output.save("preview-left.f32",juce::MemoryBlock(audio.left.data(),audio.left.size()*4)) || !output.save("preview-right.f32",juce::MemoryBlock(audio.right.data(),audio.right.size()*4)) || !writeWav(output,audio) || !output.readback())return false;
    juce::MemoryBlock physical;return output.root.getChildFile("result.bin").loadFileAsData(physical) && decodeResult(bytes(physical),request,decoded) && floatBits(decoded.audio.left,audio.left) && floatBits(decoded.audio.right,audio.right);
}
juce::String runtimeError(RuntimeStatus status){switch(status){case RuntimeStatus::cancelled:return "cancelled";case RuntimeStatus::modelNotConfigured:return "model_not_configured";case RuntimeStatus::invalidMessage:return "invalid_message";case RuntimeStatus::busy:return "companion_busy";case RuntimeStatus::unauthorized:return "companion_unauthorized";default:return "companion_unavailable";}}
struct State
{
    Arguments arguments;juce::var reply=replyFor(arguments),metadata;Exports output;std::optional<ai::PreviewAudio> audio;std::optional<ai::AudioMeasurements> metrics;bool handlerInstalled=false;
    ~State(){if(handlerInstalled)SetConsoleCtrlHandler(consoleControl,FALSE);}
    juce::var proof(bool success,const juce::String& reason)
    {
        auto value=juce::JSON::parse(json(reply));set(value,"success",success);set(value,"reason",reason);set(value,"request",metadata);set(value,"artifacts",output.artifacts());if(audio && metrics){set(value,"sampleRate",audio->sampleRate);set(value,"frames",juce::int64(audio->left.size()));set(value,"bodyFrames",juce::int64(audio->bodyFrames));set(value,"tailFrames",juce::int64(ai::previewTailFrames));set(value,"durationSeconds",double(audio->left.size())/audio->sampleRate);set(value,"phraseTruncated",audio->phraseTruncated);set(value,"measurements",measurements(*metrics));set(value,"auditionContext",object({{"bpm",audio->context.bpm},{"phraseSource",int(audio->context.phraseSource)},{"tempoSource",int(audio->context.tempoSource)},{"noteCount",int(audio->context.phrase.noteCount)}}));}return value;
    }
};
int execute(State& state)
{
    auto& arguments=state.arguments;if(!parse(arguments)){state.reply=replyFor(arguments);set(state.reply,"errorCode","invalid_arguments");return 2;}
    if(arguments.action==Arguments::Action::help){std::cout<<"RackSynthCompanionAccountProbe\n--help\n--status\n--models\n--select-model SLUG\n--sign-in\n--generate --model SLUG --instruction-file ABSOLUTE_FILE --output ABSOLUTE_NEW_CHILD\n--generate --brief A|B --model SLUG --output ABSOLUTE_NEW_CHILD\nAccount/generate: optional complete --fixture-root ABSOLUTE_ROOT --pipe-suffix ASCII_SUFFIX pair (synthetic).\nInstruction and new output are restricted to workspace build/host-fixtures.\n";return 0;}
    state.reply=replyFor(arguments);CompanionClient client;set(state.reply,"runtime",client.runtime().toString());ai::GenerationRequest request;std::shared_ptr<const RackCatalogSnapshot> catalogSnapshot;
    if(arguments.action==Arguments::Action::generate){(void)parameterRegistry();request.tag={client.runtime(),1,0,0};request.policy={ai::RequestScope::sound,false,false};request.model=arguments.model;
        // A brief arrives as the plugin's AI unit sends it: the phrase is the source's pattern, the tempo the audition's.
        if(const auto* b=arguments.brief){request.source=listening::aiBriefSource(*b);request.instruction=b->instruction;request.role=b->role;request.key=b->key;request.scale=b->scale;request.capturedBpm=b->bpm;request.capturedPhrase=request.source.pattern;}
        else{request.source=ProjectDocument{};request.instruction=arguments.instructionText;request.role="lead";request.key="A";request.scale="minor";request.capturedBpm=123;request.capturedPhrase.reset();}
        if(!state.output.create(arguments.output)){set(state.reply,"errorCode","output_creation_failed");return 1;}
        if(!captureCatalog(request,catalogSnapshot)){set(state.reply,"errorCode","catalog_capture_failed");return 1;}
        if(!captureRequest(state.output,request,arguments.brief,state.metadata)||!state.output.proof(state.proof(false,"pending"))){set(state.reply,"errorCode","request_export_failed");return 1;}}
    state.handlerInstalled=SetConsoleCtrlHandler(consoleControl,TRUE)!=FALSE;if(!state.handlerInstalled){set(state.reply,"errorCode","console_handler_failed");return 1;}
    const auto executable=companionExecutable();ImageIdentity reviewed;if(!plainPath(executable,false) || !imageIdentity(executable,reviewed)){set(state.reply,"errorCode","helper_image_unavailable");return 1;}ClientOptions options;options.launch={executable,arguments.suffix,arguments.fixture};
    if(cancelled.load(std::memory_order_acquire)){set(state.reply,"errorCode","cancelled");set(state.reply,"runtimeStatus",int(RuntimeStatus::cancelled));return 130;}
    if(!client.start(options)){const auto status=client.status();set(state.reply,"runtimeStatus",int(status.status));set(state.reply,"errorCode",safeCode(status.errorCode));client.close();return 1;}
    const auto startupEnd=GetTickCount64()+clientStartupTimeoutMs+1000;while(client.status().phase==ClientPhase::connecting && GetTickCount64()<startupEnd && !cancelled.load(std::memory_order_acquire)){client.poll();Sleep(1);}const auto startup=client.status();set(state.reply,"runtimeStatus",int(startup.status));set(state.reply,"session",client.session().toString());set(state.reply,"peer",peerRecord(client.peer()));juce::var helper;
    if(!provenance(client,reviewed,helper) && startup.phase==ClientPhase::ready){set(state.reply,"helper",helper);set(state.reply,"errorCode","helper_identity_changed");client.close();return 1;}set(state.reply,"helper",helper);
    if(cancelled.load(std::memory_order_acquire)){client.close();set(state.reply,"runtimeStatus",int(RuntimeStatus::cancelled));set(state.reply,"errorCode","cancelled");return 130;}if(startup.phase!=ClientPhase::ready){set(state.reply,"errorCode",startup.errorCode.isEmpty()?juce::String("startup_timeout"):safeCode(startup.errorCode));client.close();return 1;}
    const bool generating=arguments.action==Arguments::Action::generate;const AccountAction accountAction=arguments.action==Arguments::Action::models?AccountAction::models:arguments.action==Arguments::Action::selectModel?AccountAction::selectModel:arguments.action==Arguments::Action::signIn?AccountAction::signIn:AccountAction::status;
    if(!(generating?client.generate(request):client.account({accountAction,arguments.action==Arguments::Action::selectModel?arguments.model:juce::String{}}))){set(state.reply,"runtimeStatus",int(client.status().status));set(state.reply,"errorCode","submission_failed");client.close();return 1;}
    const auto operationEnd=GetTickCount64()+clientOperationTimeoutMs+5000;int exit=1;bool consumed=false;
    while(GetTickCount64()<operationEnd){
        if(cancelled.load(std::memory_order_acquire)){if(generating)client.cancelGeneration();else client.cancelAccount();set(state.reply,"runtimeStatus",int(RuntimeStatus::cancelled));set(state.reply,"errorCode","cancelled");exit=130;break;}client.poll();
        if(generating){ClientGenerationReply received;if(client.takeGeneration(received)){consumed=true;if(const auto* control=std::get_if<ControlMessage>(&received)){set(state.reply,"runtimeStatus",int(control->status));set(state.reply,"errorCode",runtimeError(control->status));exit=control->status==RuntimeStatus::cancelled?130:1;}
                else{const auto& result=std::get<ai::GenerationResult>(received);
                    if(result.status==ai::GenerationStatus::catalogSelectionRequired){ai::GenerationRequest materialized;
                        if(!catalogSnapshot||!materializeSelection(state.output,request,result,*catalogSnapshot,materialized,state.metadata)||!client.generate(materialized)){
                            client.close();set(state.reply,"errorCode","catalog_materialize_failed");set(state.reply,"runtimeStatus",int(RuntimeStatus::invalidMessage));break;}
                        request=std::move(materialized);consumed=false;continue;}
                    set(state.reply,"runtimeStatus",int(RuntimeStatus::ready));set(state.reply,"providerStatus",int(result.providerStatus));set(state.reply,"httpStatus",result.httpStatus);set(state.reply,"errorCode",safeCode(result.errorCode));set(state.reply,"generationStatus",int(result.status));set(state.reply,"proposalStatus",int(result.proposalStatus));set(state.reply,"renderStatus",int(result.renderStatus));set(state.reply,"inferenceCalls",int(result.inferenceCalls));if(result.status==ai::GenerationStatus::cancelled || result.providerStatus==rack::oauth::Status::cancelled){exit=130;set(state.reply,"errorCode","cancelled");}else if(result.status==ai::GenerationStatus::ready){ai::AudioMeasurements measured;if(admittedExports(state.output,request,result,measured)){state.audio=result.audio;state.metrics=measured;set(state.reply,"selectedModel",request.model);set(state.reply,"modelVerified",true);set(state.reply,"explanation",result.edits.explanation);set(state.reply,"characteristics",result.edits.characteristics);exit=0;}else set(state.reply,"errorCode",cancelled.load()?"cancelled":"artifact_admission_failed");}}break;}}
        else{AccountReply account;if(client.takeAccount(account)){consumed=true;set(state.reply,"runtimeStatus",int(account.status));set(state.reply,"providerStatus",int(account.providerStatus));set(state.reply,"httpStatus",account.httpStatus);set(state.reply,"errorCode",safeCode(account.errorCode));set(state.reply,"selectedModel",account.selectedModel);set(state.reply,"modelVerified",account.modelVerified);juce::Array<juce::var> models;for(const auto& model:account.models)models.add(object({{"slug",model.slug},{"displayName",display(model.displayName)}}));set(state.reply,"models",models);if(account.status==RuntimeStatus::cancelled || account.providerStatus==rack::oauth::Status::cancelled)exit=130;else if(account.status==RuntimeStatus::ready && (account.providerStatus==rack::oauth::Status::ready || account.providerStatus==rack::oauth::Status::connected))exit=0;break;}}
        const auto status=client.status();if(status.phase==ClientPhase::failed){set(state.reply,"runtimeStatus",int(status.status));set(state.reply,"errorCode",safeCode(status.errorCode));break;}Sleep(1);
    }
    if(!consumed && exit!=130 && state.reply["errorCode"].toString().isEmpty()){if(generating)client.cancelGeneration();else client.cancelAccount();set(state.reply,"errorCode","operation_timeout");}if(cancelled.load(std::memory_order_acquire)){exit=130;set(state.reply,"runtimeStatus",int(RuntimeStatus::cancelled));set(state.reply,"errorCode","cancelled");}
    ImageIdentity after;if(!imageIdentity(executable,after) || after!=reviewed){set(state.reply,"errorCode","helper_identity_changed");exit=1;}else{const auto handle=static_cast<HANDLE>(airack::companion::detail::CompanionClientTestAccess::borrowProcessHandle(client));if(handle && client.status().phase==ClientPhase::ready){juce::var finalHelper;if(!provenance(client,reviewed,finalHelper)){set(state.reply,"errorCode","helper_identity_changed");exit=1;}set(state.reply,"helper",finalHelper);}}client.close();return exit;
}
}
int main()
{
    State state;int exit=1;try{exit=execute(state);}catch(...){set(state.reply,"runtimeStatus",int(RuntimeStatus::unavailable));set(state.reply,"errorCode","native_exception");exit=1;}if(state.arguments.action==Arguments::Action::help)return exit;
    if(state.handlerInstalled && cancelled.load(std::memory_order_acquire)){exit=130;set(state.reply,"runtimeStatus",int(RuntimeStatus::cancelled));set(state.reply,"errorCode","cancelled");}
    if(state.output.owned){try{const auto reason=exit==0?juce::String("ready"):state.reply["errorCode"].toString().isEmpty()?juce::String("operation_failed"):state.reply["errorCode"].toString();if(!state.output.proof(state.proof(exit==0,reason))){set(state.reply,"errorCode","proof_export_failed");exit=1;}}catch(...){set(state.reply,"errorCode","proof_export_failed");exit=1;}}
    if(exit==0 && cancelled.load(std::memory_order_acquire)){exit=130;set(state.reply,"runtimeStatus",int(RuntimeStatus::cancelled));set(state.reply,"errorCode","cancelled");if(state.output.owned)try{state.output.proof(state.proof(false,"cancelled"));}catch(...){}}
    set(state.reply,"success",exit==0);const auto text=json(state.reply);if(text.getNumBytesAsUTF8()>262144){std::cout<<"{\"version\":1,\"success\":false,\"errorCode\":\"public_reply_limit\"}\n";return 1;}std::cout<<text.toStdString()<<'\n';return exit;
}
