#include "McpProtocol.h"
#include "StrictJson.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#endif
namespace airack::mcp
{
std::optional<juce::String> ipc::environment(const juce::String& name) {
#if JUCE_WINDOWS
    SetLastError(ERROR_SUCCESS);const auto size=GetEnvironmentVariableW(name.toWideCharPointer(),nullptr,0);
    if(!size)return GetLastError()==ERROR_ENVVAR_NOT_FOUND?std::nullopt:std::optional<juce::String>(juce::String{});
    if(size>32768)return juce::String{};
    std::vector<wchar_t> value(size);SetLastError(ERROR_SUCCESS);
    const auto n=GetEnvironmentVariableW(name.toWideCharPointer(),value.data(),size);
    // A concurrent deletion/growth is present-invalid for this observation, never production fallback.
    if(n>=size||(!n&&GetLastError()!=ERROR_SUCCESS))return juce::String{};
    return juce::String(value.data(),static_cast<int>(n));
#else
    if(const auto* value=std::getenv(name.toRawUTF8()))return juce::String::fromUTF8(value);return std::nullopt;
#endif
}
juce::Uuid newRuntimeId() {
#if JUCE_WINDOWS
    std::array<juce::uint8,16> bytes{};
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("MCP identity unavailable");
    bytes[6]=(bytes[6]&0x0f)|0x40;bytes[8]=(bytes[8]&0x3f)|0x80;return juce::Uuid(bytes.data());
#else
    return juce::Uuid();
#endif
}
namespace {
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{
    auto* o=new juce::DynamicObject; for(const auto& [key,value]:fields) o->setProperty(key,value); return o;
}
bool fields(const juce::var& v,std::initializer_list<const char*> allowed)
{
    auto* o=v.getDynamicObject(); if(!o) return false;
    for(const auto& p:o->getProperties()) {
        bool found=false; for(auto key:allowed) if(p.name.toString()==key) found=true;
        if(!found) return false;
    }
    return true;
}
bool meta(const juce::var& v)
{
    return !v.hasProperty("_meta") || (v["_meta"].getDynamicObject()!=nullptr && juce::JSON::toString(v["_meta"],true).getNumBytesAsUTF8()<=4096);
}
bool unsignedDecimal(const juce::var& value,std::uint64_t& out)
{
    if(!value.isString())return false;const auto text=value.toString();
    if(text.isEmpty()||text.length()>20||!text.containsOnly("0123456789"))return false;
    std::uint64_t n=0;for(auto c:text){const auto d=unsigned(c-'0');if(n>(UINT64_MAX-d)/10)return false;n=n*10+d;}out=n;return true;
}
bool exactNumericId(std::string_view token)
{
    if(token.front()=='-')token.remove_prefix(1);
    const auto exponentAt=token.find_first_of("eE");
    const auto mantissa=token.substr(0,exponentAt);
    const auto point=mantissa.find('.');
    const auto fractional=point==std::string_view::npos?0:mantissa.size()-point-1;
    std::int64_t exponent=0;
    if(exponentAt!=std::string_view::npos) {
        auto digits=token.substr(exponentAt+1);const bool negative=digits.front()=='-';
        if(digits.front()=='-' || digits.front()=='+')digits.remove_prefix(1);
        // A larger exponent cannot be cancelled by any mantissa inside the bounded request.
        for(char c:digits)exponent=std::min<std::int64_t>(maximumRequestBytes+32,exponent*10+(c-'0'));
        if(negative)exponent=-exponent;
    }
    std::string digits;digits.reserve(mantissa.size());
    for(char c:mantissa)if(c!='.')digits.push_back(c);
    const auto first=digits.find_first_not_of('0');if(first==std::string::npos)return true;
    const auto last=digits.find_last_not_of('0');
    const auto scale=exponent-static_cast<std::int64_t>(fractional)+static_cast<std::int64_t>(digits.size()-last-1);
    if(scale<0)return false;
    const auto significant=std::string_view(digits).substr(first,last-first+1);
    constexpr std::string_view maximum="9007199254740991";
    const auto length=significant.size()+static_cast<std::size_t>(scale);
    if(length!=maximum.size())return length<maximum.size();
    for(std::size_t i=0;i<length;++i) {
        const auto digit=i<significant.size()?significant[i]:'0';
        if(digit!=maximum[i])return digit<maximum[i];
    }
    return true;
}
}
bool validRpcId(const juce::var& id)
{
    if(id.isString()) return id.toString().getNumBytesAsUTF8()<=128;
    if(id.isInt() || id.isInt64()) return static_cast<juce::int64>(id)>=-9007199254740991LL && static_cast<juce::int64>(id)<=9007199254740991LL;
    if(id.isDouble()) { const auto n=double(id); return std::isfinite(n) && std::trunc(n)==n && std::abs(n)<=9007199254740991.0; }
    return false;
}
bool decodeRpc(std::string_view bytes,RpcMessage& out,RpcError& error)
{
    out={}; error={-32700,"Invalid JSON"}; juce::var v;
    bool requestNumberValid=true,cancellationNumberValid=true;
    if(!ai::parseStrictJson(bytes,v,maximumRequestBytes,[&](std::span<const juce::String> path,std::string_view token) {
        if(path.size()==1 && path[0]=="id")requestNumberValid=exactNumericId(token);
        if(path.size()==2 && path[0]=="params" && path[1]=="requestId")cancellationNumberValid=exactNumericId(token);
    })) return false;
    error={-32600,"Invalid request"};
    if(!fields(v,{"jsonrpc","id","method","params"}) || !v["jsonrpc"].isString() || v["jsonrpc"].toString()!="2.0"
        || !v["method"].isString() || v["method"].toString().isEmpty() || v["method"].toString().getNumBytesAsUTF8()>256
        || (v.hasProperty("id") && (!requestNumberValid || !validRpcId(v["id"])))) return false;
    out.id=v["id"]; out.notification=!v.hasProperty("id"); out.method=v["method"].toString(); out.params=v["params"];
    if(v.hasProperty("params") && (!v["params"].getDynamicObject() || !meta(v["params"]))) { error={-32602,"Invalid params"}; return false; }
    if(out.method=="notifications/cancelled" && (!cancellationNumberValid || !validRpcId(out.params["requestId"]))) {
        error={-32602,"Invalid cancellation ID"};return false;
    }
    error={}; return true;
}
const char* toolName(Tool tool) noexcept
{
    constexpr const char* names[]={"list_instances","get_state","get_catalog","prepare_edit","get_edit","apply_edit","discard_edit","render_preview","get_job","cancel_job","play_preview","stop_preview","undo_edit","redo_edit"};
    const auto index=static_cast<unsigned>(tool);return index<std::size(names)?names[index]:"";
}
bool decodeToolCall(const juce::var& params,ToolCall& out,RpcError& error)
{
    error={-32602,"Invalid tool arguments"};
    if(!fields(params,{"name","arguments","_meta"}) || !meta(params) || !params["name"].isString()) return false;
    const auto name=params["name"].toString();auto args=params.hasProperty("arguments")?params["arguments"]:object({});
    ToolCall call;call.arguments=args;
    if(name=="list_instances") {if(!fields(args,{}))return false;call.tool=Tool::listInstances;}
    else {
        auto uuid=[](const juce::var& value,juce::Uuid& id){
            if(!value.isString())return false;const auto s=value.toString(),raw=s.removeCharacters("-");
            if(s.length()!=36||s[8]!='-'||s[13]!='-'||s[18]!='-'||s[23]!='-'||raw.length()!=32||!raw.containsOnly("0123456789abcdefABCDEF"))return false;
            id=juce::Uuid(s);return !id.isNull();};
        if(!uuid(args["instance_id"],call.instance))return false;
        if(name=="get_state") {if(!fields(args,{"instance_id"}))return false;call.tool=Tool::getState;}
        else if(name=="get_catalog") {
            if(!fields(args,{"instance_id","snapshot","cursor","kind","limit","design_id","design_revision","content_sha256"}))return false;
            for(auto key:{"snapshot","cursor"})if(args.hasProperty(key)){juce::Uuid id;if(!uuid(args[key],id))return false;}
            if(args.hasProperty("cursor")&&!args.hasProperty("snapshot"))return false;
            const auto kind=args.hasProperty("kind")?args["kind"].toString():juce::String("parameters");
            if(args.hasProperty("kind")&&!args["kind"].isString())return false;
            if(kind!="parameters"&&kind!="racks"&&kind!="definition")return false;
            if(args.hasProperty("limit")&&((!args["limit"].isInt()&&!args["limit"].isInt64())||juce::int64(args["limit"])<1||juce::int64(args["limit"])>(kind=="parameters"?256:1048576)))return false;
            if(kind=="definition") {
                const auto id=args["design_id"].toString(),hash=args["content_sha256"].toString();juce::Uuid design;std::uint64_t revision=0;
                if(args.hasProperty("cursor")||args.hasProperty("limit")||!args["design_id"].isString()||juce::Uuid(id).isNull()
                    ||!((id.length()==32&&id.containsOnly("0123456789abcdefABCDEF"))||uuid(args["design_id"],design))
                    ||!unsignedDecimal(args["design_revision"],revision)
                    ||!args["content_sha256"].isString()||hash.length()!=64||!hash.containsOnly("0123456789abcdef"))return false;
            }else if(args.hasProperty("design_id")||args.hasProperty("design_revision")||args.hasProperty("content_sha256"))return false;
            call.tool=Tool::getCatalog;
        }else if(name=="prepare_edit") {
            juce::Uuid snapshot;
            if(!fields(args,{"instance_id","snapshot","policy","edits"})||!uuid(args["snapshot"],snapshot)
                ||!fields(args["policy"],{"scope","allow_macro_changes","allow_slot_reuse","require_pattern"})
                ||!fields(args["edits"],{"parameter_edits","rack_edits","macro_edits","pattern","reconciliation","catalog_selections","catalog_parameter_edits","name","characteristics","explanation"}))return false;
            const auto policy=args["policy"];
            if(policy.hasProperty("scope")&&(!policy["scope"].isString()||(policy["scope"].toString()!="sound"&&policy["scope"].toString()!="pattern"&&policy["scope"].toString()!="both")))return false;
            for(auto key:{"allow_macro_changes","allow_slot_reuse","require_pattern"})if(policy.hasProperty(key)&&!policy[key].isBool())return false;
            call.tool=Tool::prepareEdit;
        }else if(name=="get_edit"||name=="discard_edit"||name=="apply_edit") {
            juce::Uuid edit;if(!fields(args,{"instance_id","edit_id"})||!uuid(args["edit_id"],edit))return false;
            call.tool=name=="get_edit"?Tool::getEdit:name=="apply_edit"?Tool::applyEdit:Tool::discardEdit;
        }else if(name=="render_preview") {
            juce::Uuid handle;if(!fields(args,{"instance_id","snapshot","edit_id"})||args.hasProperty("snapshot")==args.hasProperty("edit_id")
                ||!uuid(args[args.hasProperty("snapshot")?"snapshot":"edit_id"],handle))return false;call.tool=Tool::renderPreview;
        }else if(name=="get_job"||name=="cancel_job"||name=="play_preview"||name=="stop_preview") {
            juce::Uuid handle;if(!fields(args,{"instance_id","job_id"})||!uuid(args["job_id"],handle))return false;
            call.tool=name=="get_job"?Tool::getJob:name=="cancel_job"?Tool::cancelJob:name=="play_preview"?Tool::playPreview:Tool::stopPreview;
        }else if(name=="undo_edit"||name=="redo_edit") {
            std::uint64_t transaction=0,revision=0;
            if(!fields(args,{"instance_id","transaction_id","expected_revision"})||!unsignedDecimal(args["transaction_id"],transaction)||!transaction||!unsignedDecimal(args["expected_revision"],revision))return false;
            call.tool=name=="undo_edit"?Tool::undoEdit:Tool::redoEdit;
        }else return false;
    }
    out=std::move(call);error={};return true;
}
bool previewResourceTarget(const juce::var& value,juce::Uuid& instance,juce::Uuid& epoch,juce::Uuid& resource){
    if(!value.isString())return false;const auto text=value.toString();if(!text.startsWith("air-preview://")||text.length()!=14+36*3+2)return false;
    const auto parts=juce::StringArray::fromTokens(text.substring(14),"/","");
    auto valid=[](const juce::String& s,juce::Uuid& id){const auto raw=s.removeCharacters("-");if(s.length()!=36||s[8]!='-'||s[13]!='-'||s[18]!='-'||s[23]!='-'||raw.length()!=32||!raw.containsOnly("0123456789abcdef"))return false;id=juce::Uuid(s);return !id.isNull();};
    return parts.size()==3&&valid(parts[0],instance)&&valid(parts[1],epoch)&&valid(parts[2],resource);
}
bool decodeIpcCall(const juce::var& value,ToolCall& out,RpcError& error){
    if(!value.hasProperty("method"))return decodeToolCall(value,out,error);
    error={-32602,"Invalid resource arguments"};if(!fields(value,{"method","arguments"})||!value["method"].isString())return false;
    const auto method=value["method"].toString();const auto args=value["arguments"];ToolCall call;call.arguments=args;
    if(method=="resources/list"){
        if(!fields(args,{"instance_id","cursor"}))return false;call.tool=Tool::resourceList;
        if(args.hasProperty("instance_id")){juce::Uuid epoch,resource;if(!previewResourceTarget("air-preview://"+args["instance_id"].toString()+"/00000000-0000-4000-8000-000000000001/00000000-0000-4000-8000-000000000002",call.instance,epoch,resource))return false;}
        if(args.hasProperty("cursor")&&(!args["cursor"].isString()||args["cursor"].toString().length()>128))return false;
    }else if(method=="resources/read"){
        juce::Uuid epoch,resource;if(!fields(args,{"instance_id","uri"})||!previewResourceTarget(args["uri"],call.instance,epoch,resource)
            ||(args.hasProperty("instance_id")&&args["instance_id"].toString()!=call.instance.toDashedString()))return false;call.tool=Tool::resourceRead;
    }else return false;
    out=std::move(call);error={};return true;
}
juce::var toolDefinitions()
{
    const auto read=object({{"readOnlyHint",true},{"destructiveHint",false},{"idempotentHint",true},{"openWorldHint",false}});
    const auto mutate=object({{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",false},{"openWorldHint",false}});
    const auto apply=object({{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",true},{"openWorldHint",false}});
    const auto stage=object({{"readOnlyHint",false},{"destructiveHint",false},{"idempotentHint",false},{"openWorldHint",false}});
    auto schema=[](juce::var properties,juce::Array<juce::var> required) {
        return object({{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}});
    };
    const auto text=object({{"type","string"}}),uuid=object({{"type","string"},{"format","uuid"}});
    auto policy=schema(object({{"scope",object({{"type","string"},{"enum",juce::Array<juce::var>{"sound","pattern","both"}}})},
        {"allow_macro_changes",object({{"type","boolean"}})},{"allow_slot_reuse",object({{"type","boolean"}})},{"require_pattern",object({{"type","boolean"}})}}),{});
    auto edits=schema(object({{"parameter_edits",object({{"type","array"}})},{"rack_edits",object({{"type","array"}})},
        {"macro_edits",object({{"type","array"}})},{"catalog_selections",object({{"type","array"}})},{"catalog_parameter_edits",object({{"type","array"}})},
        {"pattern",object({{"type",juce::Array<juce::var>{"object","null"}}})},{"reconciliation",object({{"type","object"}})},
        {"name",text},{"characteristics",text},{"explanation",text}}),{});
    edits.getDynamicObject()->setProperty("description","Native control edits. get_catalog returns edits_schema with the complete current native field schemas and limits; no provider envelope fields.");
    const auto editArgs=schema(object({{"instance_id",uuid},{"edit_id",uuid}}),{"instance_id","edit_id"});
    const auto decimal=object({{"type","string"},{"pattern","^[0-9]{1,20}$"},{"description","Exact unsigned 64-bit decimal string."}});
    const auto historyArgs=schema(object({{"instance_id",uuid},{"transaction_id",decimal},{"expected_revision",decimal}}),{"instance_id","transaction_id","expected_revision"});
    const auto jobArgs=schema(object({{"instance_id",uuid},{"job_id",uuid}}),{"instance_id","job_id"});
    return juce::Array<juce::var>{
        object({{"name","list_instances"},{"description","List live AIR instances through the protected native service."},{"inputSchema",schema(object({}),{})},{"annotations",read}}),
        object({{"name","get_state"},{"description","Read committed sound, locks, pattern and current snapshot of the explicitly addressed AIR instance."},{"inputSchema",schema(object({{"instance_id",uuid}}),{"instance_id"})},{"annotations",read}}),
        object({{"name","get_catalog"},{"description","Read native pages and exact edits_schema. Parameter limit defaults to 128, max 256 entries; racks to 65536, max 1048576 bytes. Echo snapshot with cursor. Definition requires design_id, decimal design_revision and content_sha256, no cursor or limit."},
            {"inputSchema",schema(object({{"instance_id",uuid},{"snapshot",uuid},{"cursor",uuid},{"kind",object({{"type","string"},{"enum",juce::Array<juce::var>{"parameters","racks","definition"}}})},
                {"limit",object({{"type","integer"},{"minimum",1},{"maximum",1048576}})},{"design_id",text},{"design_revision",text},{"content_sha256",text}}),{"instance_id"})},{"annotations",read}}),
        object({{"name","prepare_edit"},{"description","Stage native edits against the current snapshot without changing committed sound. One external owner per instance; handle expires after five minutes of inactivity."},
            {"inputSchema",schema(object({{"instance_id",uuid},{"snapshot",uuid},{"policy",policy},{"edits",edits}}),{"instance_id","snapshot","policy","edits"})},{"annotations",stage}}),
        object({{"name","get_edit"},{"description","Read the caller-owned staged edit and reviewed native diff."},{"inputSchema",editArgs},{"annotations",read}}),
        object({{"name","discard_edit"},{"description","Discard the caller-owned staged edit without changing committed sound."},{"inputSchema",editArgs},{"annotations",stage}}),
        object({{"name","apply_edit"},{"description","Commit the caller-owned reviewed native edit to sound once; preview is optional. Replays return the original receipt while retained; unchanged edits have no undo transaction."},{"inputSchema",editArgs},{"annotations",apply}}),
        object({{"name","render_preview"},{"description","Render the current snapshot or caller-owned staged edit offline without changing sound; returns an owned cancellable job."},{"inputSchema",schema(object({{"instance_id",uuid},{"snapshot",uuid},{"edit_id",uuid}}),{"instance_id"})},{"annotations",stage}}),
        object({{"name","get_job"},{"description","Read caller-owned preview status, native measurements and WAV resource URI."},{"inputSchema",jobArgs},{"annotations",read}}),
        object({{"name","cancel_job"},{"description","Cancel and retire the caller-owned preview and resource; committed sound and valid staged edits remain unchanged."},{"inputSchema",jobArgs},{"annotations",stage}}),
        object({{"name","play_preview"},{"description","Audition the caller-owned completed preview through the existing active plugin audio callback."},{"inputSchema",jobArgs},{"annotations",stage}}),
        object({{"name","stop_preview"},{"description","Request safe audition stop through reserved control admission, including while normal commands are busy."},{"inputSchema",jobArgs},{"annotations",stage}}),
        object({{"name","undo_edit"},{"description","Undo this caller's transaction only when it is the current native undo action and expected_revision matches the live revision."},{"inputSchema",historyArgs},{"annotations",mutate}}),
        object({{"name","redo_edit"},{"description","Redo this caller's transaction only when it is the current native redo action and expected_revision matches the live revision."},{"inputSchema",historyArgs},{"annotations",mutate}})};
}
namespace {
struct BinaryEnvelope {juce::String prefix,blob,suffix;bool resource=false,valid=false;};
BinaryEnvelope binaryEnvelope(const juce::var& value){
    BinaryEnvelope envelope;const bool ipc=value.hasProperty("data");const auto body=value[ipc?"data":"result"];
    if(!body.hasProperty("contents"))return envelope;envelope.resource=true;
    if(ipc){std::uint64_t serial=0;if(!fields(value,{"version","serial","status","data"})||!value["version"].isInt()||int(value["version"])!=1
        ||!unsignedDecimal(value["serial"],serial)||!serial||!value["status"].isInt()||int(value["status"])!=int(Status::ready))return envelope;}
    else if(!fields(value,{"jsonrpc","id","result"})||value["jsonrpc"].toString()!="2.0"||!validRpcId(value["id"]))return envelope;
    if(!fields(body,{"contents"}))return envelope;const auto contents=body["contents"];if(!contents.isArray()||contents.size()!=1)return envelope;
    const auto content=contents[0];juce::Uuid instance,epoch,resource;
    if(!fields(content,{"uri","mimeType","blob"})||!previewResourceTarget(content["uri"],instance,epoch,resource)
        ||!content["mimeType"].isString()||content["mimeType"].toString()!="audio/wav"||!content["blob"].isString())return envelope;
    envelope.blob=content["blob"].toString();const auto bytes=envelope.blob.getNumBytesAsUTF8();
    constexpr std::size_t maximumBinary=65536+576000*8;
    if(bytes<4||bytes%4||bytes>4*((maximumBinary+2)/3))return envelope;
    const auto* data=envelope.blob.toRawUTF8();const auto padding=(data[bytes-1]=='='?1u:0u)+(data[bytes-2]=='='?1u:0u);
    const auto digit=[](unsigned char c){if(c>='A'&&c<='Z')return int(c-'A');if(c>='a'&&c<='z')return int(c-'a'+26);if(c>='0'&&c<='9')return int(c-'0'+52);return c=='+'?62:c=='/'?63:-1;};
    if(std::strspn(data,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")!=bytes-padding)return envelope;
    if((padding==2&&(digit(data[bytes-3])&15))||(padding==1&&(digit(data[bytes-2])&3))||bytes/4*3-padding<44||bytes/4*3-padding>maximumBinary)return envelope;
    auto small=value.clone();small[ipc?"data":"result"]["contents"][0].getDynamicObject()->setProperty("blob","");
    const auto encoded=juce::JSON::toString(small,true);const juce::String marker="\"blob\": \"\"";const auto at=encoded.indexOf(marker);
    if(at<0)return envelope;envelope.prefix=encoded.substring(0,at+marker.length()-1);envelope.suffix=encoded.substring(at+marker.length()-1);envelope.valid=true;return envelope;
}
class SizeStream final:public juce::OutputStream {
public:explicit SizeStream(std::size_t maximum):cap(maximum){}
    bool write(const void*,std::size_t n)override{if(n>cap-(std::min)(count,cap)){count=cap+1;return false;}count+=n;return true;}
    juce::int64 getPosition()override{return juce::int64(count);}bool setPosition(juce::int64)override{return false;}void flush()override{}
    std::size_t count=0,cap;
};
}
std::optional<std::size_t> jsonSize(const juce::var& value,std::size_t cap){
    const auto binary=binaryEnvelope(value);if(binary.resource){if(!binary.valid)return {};const auto bytes=binary.prefix.getNumBytesAsUTF8()+binary.blob.getNumBytesAsUTF8()+binary.suffix.getNumBytesAsUTF8();if(bytes>cap)return {};return bytes;}
    SizeStream stream(cap);juce::JSON::writeToStream(stream,value,juce::JSON::FormatOptions{}.withSpacing(juce::JSON::Spacing::singleLine));if(stream.count>cap)return {};return stream.count;
}
juce::String encodeJson(const juce::var& value){
    const auto binary=binaryEnvelope(value);if(!binary.resource)return juce::JSON::toString(value,true);if(!binary.valid)return {};
    const auto bytes=binary.prefix.getNumBytesAsUTF8()+binary.blob.getNumBytesAsUTF8()+binary.suffix.getNumBytesAsUTF8();if(bytes>maximumResultBytes)return {};
    juce::String result;result.preallocateBytes(bytes);result+=binary.prefix;result+=binary.blob;result+=binary.suffix;return result;
}
BinaryJsonEncoding encodeBinaryJson(const juce::var& value,std::size_t cap){
    const auto binary=binaryEnvelope(value);BinaryJsonEncoding result{binary.resource,{}};
    if(!binary.valid)return result;
    const auto limit=(std::min)(cap,maximumResultBytes);std::size_t bytes=0;
    for(const auto* part:{&binary.prefix,&binary.blob,&binary.suffix}){
        const auto size=part->getNumBytesAsUTF8();if(size>limit-bytes)return result;bytes+=size;
    }
    result.text.preallocateBytes(bytes);result.text+=binary.prefix;result.text+=binary.blob;result.text+=binary.suffix;return result;
}
juce::String rpcSuccess(const juce::var& id,const juce::var& result)
{ const auto encoded=encodeJson(object({{"jsonrpc","2.0"},{"id",id},{"result",result}}));return encoded.isNotEmpty()?encoded:rpcFailure(id,{-32603,"Invalid resource response"}); }
juce::String rpcFailure(const juce::var& id,const RpcError& e)
{ return encodeJson(object({{"jsonrpc","2.0"},{"id",id},{"error",object({{"code",e.code},{"message",e.message}})}})); }
juce::var toolResult(const ToolResult& r)
{
    constexpr const char* names[]={"ready","busy","stale","invalid_input","unsupported","unauthorized","unavailable","instance_gone","expired","capacity","host_control_unavailable","audio_unavailable","cancelled","failed"};
    const juce::String status=names[static_cast<unsigned>(r.status)];
    auto data=object({}); if(auto* o=r.data.getDynamicObject()) for(const auto& p:o->getProperties()) data.getDynamicObject()->setProperty(p.name,p.value);
    data.getDynamicObject()->setProperty("status",status);
    return object({{"structuredContent",data},{"content",juce::Array<juce::var>{object({{"type","text"},{"text","AIR: "+status}})}},{"isError",r.status!=Status::ready}});
}
}
