#pragma once
#include "McpProtocol.h"
#include <atomic>
#include <optional>
#include <cstdint>
#include "StrictJson.h"
#include <juce_cryptography/juce_cryptography.h>
namespace airack::mcp
{
struct Caller { juce::Uuid owner, epoch; };
enum class CommandControl { none,cancel,ownerDisconnected,epochLost,revoke };
struct Command { std::uint64_t serial=0, deadlineMs=0; Caller caller; ToolCall call;
    CommandControl control=CommandControl::none;std::uint64_t cancelledSerial=0; };
struct Reply { std::uint64_t serial=0; ToolResult result; };
struct ServiceOptions { juce::String pipeSuffix; juce::File fixtureRoot; };
inline bool commandExpired(const Command& command,std::uint64_t now) noexcept {return now>=command.deadlineMs;}
namespace ipc {
inline std::optional<std::size_t> encodedSize(const juce::var& value,std::size_t cap) {return jsonSize(value,cap);}
inline juce::var object(std::initializer_list<std::pair<const char*,juce::var>> values) {
    auto* o=new juce::DynamicObject;for(const auto& [k,v]:values)o->setProperty(k,v);return o;
}
inline bool fields(const juce::var& v,std::initializer_list<const char*> keys) {
    const auto* o=v.getDynamicObject();if(!o)return false;
    for(const auto& p:o->getProperties()){bool found=false;for(auto k:keys)if(p.name.toString()==k)found=true;if(!found)return false;}return true;
}
inline bool uuid(const juce::var& v,juce::Uuid& id) {
    if(!v.isString())return false;const auto s=v.toString(),raw=s.removeCharacters("-");
    if(s.length()!=36||s[8]!='-'||s[13]!='-'||s[18]!='-'||s[23]!='-'||raw.length()!=32||!raw.containsOnly("0123456789abcdefABCDEF"))return false;
    id=juce::Uuid(s);return !id.isNull();
}
inline bool decimal(const juce::var& v,std::uint64_t& out) {
    if(!v.isString())return false;const auto s=v.toString();if(s.isEmpty()||s.length()>20||!s.containsOnly("0123456789"))return false;
    std::uint64_t n=0;for(auto c:s){const auto d=unsigned(c-'0');if(n>(UINT64_MAX-d)/10)return false;n=n*10+d;}out=n;return true;
}
inline juce::String decimal(std::uint64_t n){return juce::String(std::to_string(n));}
inline bool text(const juce::var& v,std::size_t cap=256){return v.isString()&&v.toString().getNumBytesAsUTF8()<=cap;}
inline bool parse(const void* data,std::size_t size,std::size_t cap,juce::var& out) {
    return ai::parseStrictJson({static_cast<const char*>(data),size},out,cap)&&out.getDynamicObject()&&out["version"].isInt()&&int(out["version"])==1;
}
inline juce::var call(const ToolCall& c) {
    if(c.tool==Tool::resourceList||c.tool==Tool::resourceRead)return object({{"method",c.tool==Tool::resourceList?"resources/list":"resources/read"},{"arguments",c.arguments}});
    return object({{"name",toolName(c.tool)},{"arguments",c.arguments}});
}
inline bool result(const juce::var& v,ToolResult& out) {
    if(!v["status"].isInt()||int(v["status"])<0||int(v["status"])>int(Status::failed)||!v["data"].getDynamicObject())return false;
    out={static_cast<Status>(int(v["status"])),v["data"]};return true;
}
inline juce::var reply(const Reply& r){return object({{"version",1},{"serial",decimal(r.serial)},{"status",int(r.result.status)},{"data",r.result.data.getDynamicObject()?r.result.data:object({})}});}
inline bool validSuffix(const juce::String& s){return s.isNotEmpty()&&s.length()<=32&&s.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");}
inline bool safeRoot(const juce::String& s) {
    if(s.length()<4||s.length()>32000||s[1]!=':'||(s[2]!='\\'&&s[2]!='/')||!juce::String::charToString(s[0]).containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))return false;
    return !s.substring(2).containsChar(':')&&!s.containsChar('"')&&!s.containsChar('\n')&&juce::File(s).getParentDirectory()!=juce::File(s);
}
}
inline std::optional<ServiceOptions> serviceOptionsForProcess() {
    const auto root=ipc::environment("AIRACK_MCP_FIXTURE_ROOT");
    const auto suffix=ipc::environment("AIRACK_MCP_PIPE_SUFFIX");
    if(root||suffix){if(!root||!suffix||!ipc::safeRoot(*root)||!ipc::validSuffix(*suffix))return {};return ServiceOptions{*suffix,juce::File(*root)};}
    const auto diagnostic=ipc::environment("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT");
    if(diagnostic){if(!ipc::safeRoot(*diagnostic))return {};const juce::File file(*diagnostic);
        const auto canonical=file.getFullPathName().replaceCharacter('\\','/').toLowerCase();
        return ServiceOptions{juce::SHA256(canonical.toRawUTF8(),canonical.getNumBytesAsUTF8()).toHexString().substring(0,32),file};}
    return ServiceOptions{};
}
int runService(const ServiceOptions&, const std::atomic_bool& stop);
}
