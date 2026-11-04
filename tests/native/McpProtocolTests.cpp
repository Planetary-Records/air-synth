#include "McpProtocol.h"
#include "StrictJson.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
using namespace airack;
namespace {
int checks=0, failures=0;
void check(bool pass,const char* expression,int line) { ++checks; if(!pass) { ++failures; std::cerr<<line<<" "<<expression<<std::endl; } }
#define CHECK(x) check(bool(x),#x,__LINE__)
juce::var json(const char* s) { return juce::JSON::parse(s); }
void binaryEnvelope(){
    auto value=json(R"({"jsonrpc":"2.0","id":"quote\"\nID","result":{"contents":[{"uri":"air-preview://01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef","mimeType":"audio/wav","blob":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]}})");
    const auto encoded=mcp::encodeJson(value);CHECK(encoded==juce::JSON::toString(value,true));const auto bytes=encoded.getNumBytesAsUTF8();CHECK(mcp::jsonSize(value,bytes)==bytes);CHECK(!mcp::jsonSize(value,bytes-1));CHECK(juce::JSON::parse(encoded)["id"]==value["id"]);
    for(const auto& mutation:{std::pair{"blob",juce::var("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB==")},std::pair{"blob",juce::var("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=+")},std::pair{"blob",juce::var("not base64")},std::pair{"uri",juce::var("C:/file.wav")},std::pair{"mimeType",juce::var("text/plain")},std::pair{"owner",juce::var("injected")}}){auto invalid=value.clone();invalid["result"]["contents"][0].getDynamicObject()->setProperty(mutation.first,mutation.second);CHECK(mcp::encodeJson(invalid).isEmpty());CHECK(!mcp::jsonSize(invalid,mcp::maximumResultBytes));}
    auto args=json(R"({"name":"resources/read","arguments":{}})");mcp::ToolCall call;mcp::RpcError error;CHECK(!mcp::decodeToolCall(args,call,error));
}
void boundedBinaryEncoding(){
    auto fixture=[](std::size_t bytes,bool ipc){
        juce::MemoryOutputStream wav;
        wav.write("RIFF",4);wav.writeInt(int(bytes-8));wav.write("WAVEfmt ",8);wav.writeInt(16);
        wav.writeShort(3);wav.writeShort(2);wav.writeInt(48000);wav.writeInt(48000*8);wav.writeShort(8);wav.writeShort(32);
        wav.write("data",4);wav.writeInt(int(bytes-44));wav.writeRepeatedByte(0,bytes-44);
        auto value=json(ipc?R"({"version":1,"serial":"18446744073709551615","status":0,"data":{"contents":[{"uri":"air-preview://01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef","mimeType":"audio/wav"}]}})":R"({"jsonrpc":"2.0","id":"quote\"\nID","result":{"contents":[{"uri":"air-preview://01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef","mimeType":"audio/wav"}]}})");
        value[ipc?"data":"result"]["contents"][0].getDynamicObject()->setProperty("blob",juce::Base64::toBase64(wav.getData(),wav.getDataSize()));
        return value;
    };
    auto compare=[](const juce::var& value,bool resource,std::size_t cap){
        const auto alias=value;const auto before=juce::JSON::toString(value,true);
        const auto size=mcp::jsonSize(value,cap);const auto old=size?mcp::encodeJson(value):juce::String{};
        const auto result=mcp::encodeBinaryJson(value,cap);
        CHECK(result.resource==resource);
        CHECK(!resource?result.text.isEmpty():result.text==old);
        if(resource&&size&&old.isNotEmpty())CHECK(result.text.getNumBytesAsUTF8()==*size);
        CHECK(juce::JSON::toString(value,true)==before);CHECK(juce::JSON::toString(alias,true)==before);
    };
    for(bool ipc:{false,true}){
        const auto key=ipc?"data":"result";
        for(auto bytes:{std::size_t(44),std::size_t(45),std::size_t(46),std::size_t(44+576000*8),std::size_t(65536+576000*8)}){
            const auto value=fixture(bytes,ipc);const auto encoded=mcp::encodeJson(value);const auto cap=encoded.getNumBytesAsUTF8();
            CHECK(encoded==juce::JSON::toString(value,true));CHECK(cap>0);
            compare(value,true,cap);compare(value,true,cap-1);compare(value,true,cap+1);
        }
        const auto value=fixture(44,ipc);const auto blob=value[key]["contents"][0]["blob"].toString();
        for(const auto& mutation:std::initializer_list<std::pair<const char*,juce::var>>{
            {"blob",blob.substring(0,blob.length()-3)+"B=="},{"blob",blob.substring(0,blob.length()-2)+"=+"},
            {"blob",blob.substring(0,blob.length()-4)+"=AAA"},{"blob",blob.substring(0,blob.length()-4)+"!AAA"},
            {"blob",juce::String::charToString(0x80)+blob.substring(1)},{"blob",juce::Base64::toBase64(std::string(43,'x').data(),43)},
            {"blob",1},{"uri","C:/file.wav"},{"uri",value[key]["contents"][0]["uri"].toString().toUpperCase()},
            {"mimeType","text/plain"},{"mimeType",1},{"owner","injected"}}){
            auto invalid=value.clone();invalid[key]["contents"][0].getDynamicObject()->setProperty(mutation.first,mutation.second);
            CHECK(!mcp::jsonSize(invalid,mcp::maximumResultBytes));compare(invalid,true,mcp::maximumResultBytes);
        }
        auto oversized=fixture(65536+576000*8+1,ipc);CHECK(mcp::encodeJson(oversized).isEmpty());compare(oversized,true,mcp::maximumResultBytes);
        for(const auto& contents:juce::Array<juce::var>{juce::var{},json("{}"),json("[]"),json("[{}]"),json("[{},{}]")}){
            auto invalid=value.clone();invalid[key].getDynamicObject()->setProperty("contents",contents);compare(invalid,true,mcp::maximumResultBytes);
        }
        auto invalid=value.clone();invalid[key].getDynamicObject()->setProperty("extra",true);compare(invalid,true,mcp::maximumResultBytes);
        invalid=value.clone();invalid.getDynamicObject()->setProperty("extra",true);compare(invalid,true,mcp::maximumResultBytes);
        if(ipc){
            for(const auto& mutation:std::initializer_list<std::pair<const char*,juce::var>>{{"status",1},{"status","0"},{"serial","0"},{"serial","18446744073709551616"},{"serial",1},{"version",2},{"version","1"}}){
                invalid=value.clone();invalid.getDynamicObject()->setProperty(mutation.first,mutation.second);compare(invalid,true,mcp::maximumResultBytes);
            }
        }else{
            for(const auto& mutation:std::initializer_list<std::pair<const char*,juce::var>>{{"jsonrpc","1.0"},{"id",juce::var{}},{"id",juce::String::charToString(0x20ac)+"\"\n"}}){
                invalid=value.clone();invalid.getDynamicObject()->setProperty(mutation.first,mutation.second);compare(invalid,true,mcp::maximumResultBytes);
            }
        }
        // A returned encoding is owned bytes, not a retained validation token for the mutable graph.
        auto aliased=value.clone();auto alias=aliased;const auto result=mcp::encodeBinaryJson(aliased,mcp::maximumResultBytes);
        const auto before=mcp::encodeJson(aliased);alias[key]["contents"][0].getDynamicObject()->setProperty("blob","bad");
        CHECK(result.resource&&result.text==before);CHECK(mcp::encodeBinaryJson(aliased,mcp::maximumResultBytes).text.isEmpty());
    }
    for(const auto& generic:juce::Array<juce::var>{juce::var{},json("[]"),json(R"({"version":1,"serial":"1","status":8,"data":{}})"),json(R"({"result":{"status":"ready","text":"quote\"\n"}})"),json(R"({"contents":[]})")}){
        compare(generic,false,mcp::maximumResultBytes);compare(generic,false,0);
    }
}
// Independent pre-R53 alphabet/padding oracle; do not route this through the production encoder.
bool legacyBinaryBlob(const juce::String& blob){
    const auto bytes=blob.getNumBytesAsUTF8();constexpr std::size_t maximumBinary=65536+576000*8;
    if(bytes<4||bytes%4||bytes>4*((maximumBinary+2)/3))return false;
    const auto* data=blob.toRawUTF8();const auto padding=(data[bytes-1]=='='?1u:0u)+(data[bytes-2]=='='?1u:0u);
    const auto digit=[](unsigned char c){if(c>='A'&&c<='Z')return int(c-'A');if(c>='a'&&c<='z')return int(c-'a'+26);if(c>='0'&&c<='9')return int(c-'0'+52);return c=='+'?62:c=='/'?63:-1;};
    for(std::size_t n=0;n<bytes-padding;++n)if(digit(static_cast<unsigned char>(data[n]))<0)return false;
    return !(padding==2&&(digit(data[bytes-3])&15))&&!(padding==1&&(digit(data[bytes-2])&3))
        &&bytes/4*3-padding>=44&&bytes/4*3-padding<=maximumBinary;
}
void alphabetEquivalence(){
    auto rpc=json(R"({"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":"air-preview://01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef","mimeType":"audio/wav"}]}})");
    auto ipc=json(R"({"version":1,"serial":"1","status":0,"data":{"contents":[{"uri":"air-preview://01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef","mimeType":"audio/wav"}]}})");
    std::uint64_t cases=0,accepted=0,digest=14695981039346656037ULL;
    const auto compare=[&](const juce::String& blob,bool useIpc){
        auto& value=useIpc?ipc:rpc;value[useIpc?"data":"result"]["contents"][0].getDynamicObject()->setProperty("blob",blob);
        const bool expected=legacyBinaryBlob(blob);const auto result=mcp::encodeBinaryJson(value,mcp::maximumResultBytes);
        CHECK(result.resource);CHECK(result.text.isNotEmpty()==expected);CHECK(mcp::encodeJson(value)==result.text);
        const auto size=mcp::jsonSize(value,mcp::maximumResultBytes);CHECK(size.has_value()==expected);
        if(expected){++accepted;CHECK(size&&*size==result.text.getNumBytesAsUTF8());CHECK(result.text==juce::JSON::toString(value,true));}
        CHECK(value[useIpc?"data":"result"]["contents"][0]["blob"].toString()==blob);
        digest^=expected?1u:0u;digest*=1099511628211ULL;
        const auto* text=result.text.toRawUTF8();const auto outputBytes=result.text.getNumBytesAsUTF8();for(std::size_t n=0;n<outputBytes;++n){digest^=static_cast<unsigned char>(text[n]);digest*=1099511628211ULL;}
        ++cases;
    };
    // Every short position and U+0000..U+00ff under all three suffix padding states; wrappers alternate.
    for(int padding=0;padding<=2;++padding){
        const auto base=juce::String::repeatedString("A",64-padding)+juce::String::repeatedString("=",padding);
        for(int position=0;position<64;++position)for(int character=0;character<256;++character)
            compare(base.substring(0,position)+juce::String::charToString(juce::juce_wchar(character))+base.substring(position+1),(position+character+padding)%2!=0);
    }
    // Exhaust the final ASCII pair, including misplaced '=', and every low-bit candidate before '=='.
    const auto prefix=juce::String::repeatedString("A",62);
    for(int first=0;first<128;++first)for(int last=0;last<128;++last)
        compare(prefix+juce::String::charToString(juce::juce_wchar(first))+juce::String::charToString(juce::juce_wchar(last)),(first+last)%2!=0);
    for(int character=0;character<128;++character)compare(juce::String::repeatedString("A",61)+juce::String::charToString(juce::juce_wchar(character))+"==",character%2!=0);
    for(auto unicode:{juce::String::charToString(0x80)+juce::String::charToString(0x80),juce::String::charToString(0x20ac),juce::String::charToString(0x1f600)})
        compare(juce::String::repeatedString("A",64-int(unicode.getNumBytesAsUTF8()))+unicode,false);
    // JUCE rejects escaped NUL in JSON but native strings terminate there; preserve both behaviors.
    for(int position:{0,1,43,44,60,64}){
        const auto raw="{\"blob\":\""+juce::String::repeatedString("A",position)+"\\u0000"+juce::String::repeatedString("A",64-position)+"\"}";juce::var parsed;
        CHECK(!ai::parseStrictJson({raw.toRawUTF8(),raw.getNumBytesAsUTF8()},parsed,4096));
        auto native=std::string(65,'A');native[std::size_t(position)]='\0';const auto materialised=juce::String::fromUTF8(native.data(),int(native.size()));
        compare(materialised,false);compare(materialised,true);
    }
    constexpr std::size_t maximumBinary=65536+576000*8;
    for(auto bytes:{std::size_t(43),std::size_t(44),std::size_t(45),std::size_t(46),maximumBinary-2,maximumBinary-1,maximumBinary,maximumBinary+1}){
        const juce::MemoryBlock data(bytes,true);const auto blob=juce::Base64::toBase64(data.getData(),data.getSize());
        compare(blob,false);compare(blob,true);
        if(bytes==maximumBinary)for(auto position:{0,blob.length()/2,blob.length()-3})compare(blob.substring(0,position)+"!"+blob.substring(position+1),true);
    }
    std::cout<<"alphabet_equivalence cases="<<cases<<" accepted="<<accepted<<" fnv1a="<<juce::String::toHexString(juce::int64(digest))<<std::endl;
}
void protocol()
{
    mcp::RpcMessage r; mcp::RpcError e;
    CHECK(mcp::decodeRpc(R"({"jsonrpc":"2.0","id":7,"method":"ping","params":{}})",r,e));
    CHECK(r.id.isInt() || r.id.isInt64()); CHECK(r.method=="ping" && !r.notification);
    CHECK(mcp::decodeRpc(R"({"jsonrpc":"2.0","id":"7","method":"ping"})",r,e));
    CHECK(r.id.isString() && r.id.toString()=="7");
    CHECK(mcp::decodeRpc(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",r,e)); CHECK(r.notification);
    for(auto s : {R"({"jsonrpc":"2.0","id":7,"id":8,"method":"ping"})",
                 R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":1e999}})",
                 R"({"jsonrpc":"2.0","id":null,"method":"ping"})",
                 R"({"jsonrpc":"2.0","id":true,"method":"ping"})",
                 R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})",
                 R"({"jsonrpc":"2.0","id":9007199254740992,"method":"ping"})",
                 R"({"jsonrpc":"1.0","id":1,"method":"ping"})", "[]", "{", "{}",
                 R"({"jsonrpc":"2.0","id":1,"method":"ping","surprise":true})"}) CHECK(!mcp::decodeRpc(s,r,e));
    std::string large(mcp::maximumRequestBytes+1,' '); CHECK(!mcp::decodeRpc(large,r,e));
    CHECK(!mcp::decodeRpc("{",r,e) && e.code==-32700);
    CHECK(!mcp::decodeRpc("[]",r,e) && e.code==-32600);
    CHECK(!mcp::decodeRpc(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[]})",r,e) && e.code==-32602);
    CHECK(!mcp::decodeRpc(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_meta":[]}})",r,e) && e.code==-32602);
    CHECK(mcp::validRpcId(juce::String(std::string(128,'a')))); CHECK(!mcp::validRpcId(juce::String(std::string(129,'a'))));
    CHECK(mcp::validRpcId(juce::int64(9007199254740991LL)));
    for(const auto& [token,expected] : std::initializer_list<std::pair<const char*,juce::int64>>{
        {"0",0},{"-0.0",0},{"1.0",1},{"1e0",1},{"10e-1",1},{"100.00e-2",1},
        {"9007199254740991",9007199254740991LL},{"-9007199254740991",-9007199254740991LL},
        {"9007199254740991.0",9007199254740991LL},{"-9.007199254740991e15",-9007199254740991LL},
        {"90071992547409910e-1",9007199254740991LL},{"-90071992547409910e-1",-9007199254740991LL}}) {
        CHECK(mcp::decodeRpc(std::string("{\"jsonrpc\":\"2.0\",\"id\":")+token+",\"method\":\"ping\"}",r,e));
        CHECK(!r.id.isString() && static_cast<juce::int64>(r.id)==expected);
        CHECK(mcp::decodeRpc(std::string("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":")+token+"}}",r,e));
    }
    for(auto token : {"1.0000000000000001","-1.0000000000000001","900.00000000000000001", "-900.00000000000000001",
        "9007199254740990.9","-9007199254740990.9","9007199254740991.1","-9007199254740991.1",
        "9007199254740991.00000000000000001","-9007199254740991.00000000000000001",
        "9007199254740992","-9007199254740992"}) {
        CHECK(!mcp::decodeRpc(std::string("{\"jsonrpc\":\"2.0\",\"id\":")+token+",\"method\":\"ping\"}",r,e) && e.code==-32600);
        CHECK(!mcp::decodeRpc(std::string("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":")+token+"}}",r,e) && r.notification && e.code==-32602);
    }
    CHECK(!mcp::decodeRpc(R"({"jsonrpc":"2.0","\u0069d":1.0000000000000001,"method":"ping"})",r,e));
    CHECK(mcp::decodeRpc(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_meta":{"id":1.0000000000000001,"requestId":9007199254740991.1}}})",r,e));
    auto defs=mcp::toolDefinitions(); CHECK(defs.isArray() && defs.size()==14);
    CHECK(juce::JSON::toString(defs,true)==juce::JSON::toString(mcp::toolDefinitions(),true));
    if(defs.isArray() && defs.size()==14) {
        CHECK(defs[0]["name"].toString()=="list_instances"); CHECK(defs[1]["name"].toString()=="get_state");
        CHECK(bool(defs[0]["annotations"]["readOnlyHint"])); CHECK(!bool(defs[0]["annotations"]["openWorldHint"]));
    }
    mcp::ToolCall call;
    CHECK(mcp::decodeToolCall(json(R"({"name":"list_instances","arguments":{}})"),call,e)); CHECK(call.tool==mcp::Tool::listInstances);
    CHECK(mcp::decodeToolCall(json(R"({"name":"get_state","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef"}})"),call,e));
    for(auto s : {R"({"name":"get_state","arguments":{}})", R"({"name":"get_state","arguments":{"instance_id":"bad"}})",
        R"({"name":"list_instances","arguments":{"owner":"bad"}})", R"({"name":"prepare_edit","arguments":{}})",
        R"({"name":"list_instances","arguments":{},"extra":1})"}) CHECK(!mcp::decodeToolCall(json(s),call,e) && e.code==-32602);
    const auto instance="01234567-89ab-cdef-0123-456789abcdef";
    for(const auto& def:*defs.getArray()) {
        CHECK(!bool(def["annotations"]["openWorldHint"]));CHECK(!bool(def["inputSchema"]["additionalProperties"]));
        CHECK(!def["description"].toString().contains("not yet implemented"));
    }
    for(auto tool:{mcp::Tool::getCatalog,mcp::Tool::prepareEdit,mcp::Tool::getEdit,mcp::Tool::discardEdit,mcp::Tool::applyEdit,mcp::Tool::undoEdit,mcp::Tool::redoEdit,mcp::Tool::renderPreview,mcp::Tool::getJob,mcp::Tool::cancelJob,mcp::Tool::playPreview,mcp::Tool::stopPreview}) {
        auto args=json(R"({"instance_id":"01234567-89ab-cdef-0123-456789abcdef"})");
        if(tool==mcp::Tool::prepareEdit){args.getDynamicObject()->setProperty("snapshot",instance);args.getDynamicObject()->setProperty("policy",json("{}"));args.getDynamicObject()->setProperty("edits",json(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})"));}
        if(tool==mcp::Tool::getEdit||tool==mcp::Tool::discardEdit||tool==mcp::Tool::applyEdit)args.getDynamicObject()->setProperty("edit_id",instance);
        if(tool==mcp::Tool::renderPreview)args.getDynamicObject()->setProperty("snapshot",instance);
        if(tool==mcp::Tool::getJob||tool==mcp::Tool::cancelJob||tool==mcp::Tool::playPreview||tool==mcp::Tool::stopPreview)args.getDynamicObject()->setProperty("job_id",instance);
        if(tool==mcp::Tool::undoEdit||tool==mcp::Tool::redoEdit){args.getDynamicObject()->setProperty("transaction_id","18446744073709551614");args.getDynamicObject()->setProperty("expected_revision","9007199254740993");}
        juce::var root(new juce::DynamicObject);root.getDynamicObject()->setProperty("name",mcp::toolName(tool));root.getDynamicObject()->setProperty("arguments",args);CHECK(mcp::decodeToolCall(root,call,e));CHECK(call.tool==tool);
        args.getDynamicObject()->setProperty("owner",instance);CHECK(!mcp::decodeToolCall(root,call,e));
    }
    for(auto input:{R"({"name":"undo_edit","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef","transaction_id":"1","expected_revision":1}})",
        R"({"name":"redo_edit","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef","transaction_id":"18446744073709551616","expected_revision":"0"}})",
        R"({"name":"prepare_edit","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef","snapshot":"01234567-89ab-cdef-0123-456789abcdef","policy":{"scope":"all"},"edits":{}}})",
        R"({"name":"get_catalog","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef","kind":"definition"}})",
        R"({"name":"get_catalog","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef","limit":257}})",
        R"({"name":"render_preview","arguments":{"instance_id":"01234567-89ab-cdef-0123-456789abcdef"}})"})CHECK(!mcp::decodeToolCall(json(input),call,e));
    const auto result=mcp::toolResult({mcp::Status::unavailable,{}});
    CHECK(result["structuredContent"]["status"].toString()=="unavailable"); CHECK(bool(result["isError"]));
    CHECK(result["content"].isArray()); if(result["content"].isArray() && result["content"].size()) CHECK(result["content"][0]["type"].toString()=="text");
    CHECK(json(mcp::rpcSuccess(7,json("{}" )).toRawUTF8())["id"].isInt());
    CHECK(json(mcp::rpcSuccess("7",json("{}" )).toRawUTF8())["id"].isString());
    CHECK(int(json(mcp::rpcFailure(7,{-32601,"Unknown method"}).toRawUTF8())["error"]["code"])==-32601);
}
#if JUCE_WINDOWS
// Each fixture owns only the handles returned by its CreateProcessW/CreatePipe.
struct Child {
    HANDLE process=nullptr,input=nullptr,output=nullptr;
    std::string buffered;
    Child(const juce::String& executable,const juce::String& arguments) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE}; HANDLE read=nullptr,write=nullptr;
        CHECK(CreatePipe(&read,&input,&security,4096)); CHECK(CreatePipe(&output,&write,&security,4096));
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0); SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        HANDLE error=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        STARTUPINFOEXW start{}; start.StartupInfo.cb=sizeof(start); start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        start.StartupInfo.hStdInput=read; start.StartupInfo.hStdOutput=write; start.StartupInfo.hStdError=error;
        SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,1,0,&size); std::vector<std::byte> attributes(size);
        start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        CHECK(InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&size));
        HANDLE inherited[]{read,write,error}; CHECK(UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr));
        const auto command="\""+executable+"\" "+arguments; std::wstring text(command.toWideCharPointer()); std::vector<wchar_t> writable(text.begin(),text.end()); writable.push_back(0);
        PROCESS_INFORMATION info{};
        CHECK(CreateProcessW(executable.toWideCharPointer(),writable.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&start.StartupInfo,&info));
        process=info.hProcess; if(info.hThread)CloseHandle(info.hThread); CloseHandle(read);CloseHandle(write);CloseHandle(error);DeleteProcThreadAttributeList(start.lpAttributeList);
    }
    ~Child() { eof(); if(process) { if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0) { CHECK(false); TerminateProcess(process,97); WaitForSingleObject(process,5000); } CloseHandle(process); } if(output)CloseHandle(output); }
    void eof() { if(input) {CloseHandle(input); input=nullptr;} }
    bool send(const std::string& text,unsigned ms=3000) {
        if(!input)return false;
        bool success=false;std::atomic_bool done{false};
        std::thread writer([&] { DWORD n=0;success=WriteFile(input,text.data(),DWORD(text.size()),&n,nullptr) && n==text.size();done=true; });
        const auto end=GetTickCount64()+ms;
        while(!done.load() && GetTickCount64()<end)Sleep(1);
        const bool timedOut=!done.load();
        while(!done.load()) {CancelSynchronousIo(writer.native_handle());Sleep(1);}
        writer.join();return !timedOut && success;
    }
    void consumedInput() {
        DWORD outQuota=0,inQuota=0;CHECK(GetNamedPipeInfo(input,nullptr,&outQuota,&inQuota,nullptr));
        const auto quota=std::max(outQuota,inQuota);CHECK(quota>0 && quota<=65536);
        if(!quota || quota>65536)return;
        constexpr auto paddingLine="{\"jsonrpc\":\"2.0\",\"method\":\"notifications/probe_padding\"}\n";
        std::string padding;
        while(padding.size()<=quota+4096+std::char_traits<char>::length(paddingLine))padding+=paddingLine;
        // Completion forces consumption past earlier input: neither the pipe nor one reader chunk can hold this suffix.
        CHECK(send(padding));
        std::cout<<"input barrier quota="<<quota<<" reader_chunk=4096 padding_bytes="<<padding.size()<<std::endl;
    }
    void blockedOutput() {
        DWORD outQuota=0,inQuota=0,available=0;CHECK(GetNamedPipeInfo(output,nullptr,&outQuota,&inQuota,nullptr));
        const auto quota=std::max(outQuota,inQuota);const auto end=GetTickCount64()+3000;
        while(GetTickCount64()<end) {if(!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr) || available>=quota)break;Sleep(1);}
        CHECK(quota>0 && available>=quota);
        const auto schemaBytes=juce::JSON::toString(mcp::toolDefinitions(),true).getNumBytesAsUTF8();
        CHECK(4*schemaBytes>quota);
        consumedInput();
        std::cout<<"cancel fixture pipe_out="<<outQuota<<" pipe_in="<<inQuota<<" unread="<<available<<" schema_bytes="<<schemaBytes<<std::endl;
    }
    juce::var receive(unsigned ms=3000) {
        const auto end=GetTickCount64()+ms;
        do {
            auto newline=buffered.find('\n'); if(newline!=std::string::npos) { auto line=buffered.substr(0,newline); buffered.erase(0,newline+1); juce::var v; CHECK(ai::parseStrictJson(line,v,mcp::maximumResultBytes)); return v; }
            DWORD available=0; if(!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)) break;
            if(available) { char data[4096];DWORD n=0; if(ReadFile(output,data,std::min<DWORD>(available,sizeof(data)),&n,nullptr))buffered.append(data,n);else break; }
            else Sleep(1);
        } while(GetTickCount64()<end);
        return {};
    }
    bool ended(unsigned ms=3000) { return process && WaitForSingleObject(process,ms)==WAIT_OBJECT_0; }
    DWORD code() { DWORD code=STILL_ACTIVE; if(process)GetExitCodeProcess(process,&code); return code; }
};
void bridge(const char* executable) {
    const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/mcp-native/evidence/task-1/native-"+juce::Uuid().toString());
    const auto args="--mcp-stdio --fixture-root \""+root.getFullPathName()+"\" --pipe-suffix "+root.getFileName().removeCharacters("-").substring(0,32);
    {
        Child c(executable,args);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n"));
        CHECK(int(c.receive()["error"]["code"])==-32600);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":[],\"clientInfo\":{\"name\":\"native\",\"version\":\"1\"}}}\n"));
        CHECK(int(c.receive()["error"]["code"])==-32602);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":\"init\",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2026-07-28\",\"capabilities\":{},\"clientInfo\":{\"name\":\"native\",\"version\":\"1\"}}}\n"));
        auto init=c.receive(); CHECK(init["id"].isString()); CHECK(init["result"]["protocolVersion"].toString()=="2025-11-25");
        CHECK(init["result"]["capabilities"]["tools"].isObject()); CHECK(init["result"]["capabilities"]["resources"].isObject());
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n{\"jsonrpc\":\"2.0\",\"method\":\"unknown\"}\n{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n"));
        auto ping=c.receive(); CHECK(ping["id"].isInt() && int(ping["id"])==7); CHECK(ping["result"].isObject());
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"server/discover\"}\n")); CHECK(int(c.receive()["error"]["code"])==-32601);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}\n")); CHECK(c.receive()["result"]["tools"].size()==14);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\",\"arguments\":{\"instance_id\":\"01234567-89ab-cdef-0123-456789abcdef\"}}}\n"));
        CHECK(c.receive()["result"]["structuredContent"]["status"].toString()=="instance_gone");
        CHECK(c.send("{\n")); CHECK(int(c.receive()["error"]["code"])==-32700);
        CHECK(c.send(std::string(mcp::maximumRequestBytes+1,'x')+"\n")); CHECK(int(c.receive()["error"]["code"])==-32700);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\"}\n")); CHECK(int(c.receive()["id"])==11);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"ping\",\"params\":true}\n"));
        auto invalid=c.receive(); CHECK(int(invalid["id"])==12); CHECK(int(invalid["error"]["code"])==-32602);
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":\"bad-params\",\"method\":\"ping\",\"params\":null}\n"));
        invalid=c.receive(); CHECK(invalid["id"].toString()=="bad-params"); CHECK(int(invalid["error"]["code"])==-32602);
        for(auto token : {"1.0000000000000001","9007199254740991.1","-9007199254740991.1"}) {
            CHECK(c.send(std::string("{\"jsonrpc\":\"2.0\",\"id\":")+token+",\"method\":\"ping\"}\n"));
            invalid=c.receive();CHECK(invalid["id"].isVoid());CHECK(int(invalid["error"]["code"])==-32600);
        }
        for(auto token : {"10e-1","9007199254740991.0","-9.007199254740991e15"}) {
            CHECK(c.send(std::string("{\"jsonrpc\":\"2.0\",\"id\":")+token+",\"method\":\"ping\"}\n"));
            auto reply=c.receive();CHECK(reply["result"].isObject());CHECK(!reply["id"].isString());
        }
        c.eof(); CHECK(c.ended()); CHECK(c.code()==0); CHECK(c.receive(10).isVoid());
    }
    CHECK(!root.getChildFile("OAuthProbe").exists()); CHECK(!root.getChildFile("CompanionPreferences").exists()); CHECK(!root.getChildFile("launch-observation.json").exists());
    // The unread output fills its pipe: EOF must retire its blocked writer.
    {
        Child c(executable,args); CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"native\",\"version\":\"1\"}}}\n")); c.receive();
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"));
        for(int i=0;i<7;++i) CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(i+1)+",\"method\":\"tools/list\"}\n"));
        c.eof(); CHECK(c.ended()); CHECK(c.code()==0);
    }
    for(const auto& invalid : {juce::String("--mcp-stdio --fixture-root relative --pipe-suffix X"),juce::String("--mcp-stdio --instance invalid"),juce::String("--instance 01234567-89ab-cdef-0123-456789abcdef"),juce::String("--mcp-stdio --pipe-suffix X")}) {
        Child c(executable,invalid); c.eof(); CHECK(c.ended());CHECK(c.code()==2);
    }
    {
        Child c(executable,args); CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"native\",\"version\":\"1\"}}}\n")); c.receive();
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"));
        for(int i=1;i<=4;++i) CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(i)+",\"method\":\"tools/list\"}\n"));
        c.blockedOutput();
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":900,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"id\":\"900\",\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":9.00e2}}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":null}}\n{\"jsonrpc\":\"2.0\",\"id\":901,\"method\":\"ping\"}\n"));
        c.consumedInput();
        // Four inventory replies block stdout while leaving room for all four exact-ID probes under the unchanged eight-output cap.
        for(int i=1;i<=4;++i) CHECK(int(c.receive()["id"])==i);
        auto stringId=c.receive();CHECK(stringId["id"].isString() && stringId["id"].toString()=="900");
        auto last=c.receive(); if(int(last["id"])!=901)std::cerr<<"cancel reply="<<juce::JSON::toString(last,true)<<std::endl;
        CHECK(int(last["id"])==901); c.eof();CHECK(c.ended());CHECK(c.code()==0);
    }
    {
        Child c(executable,args); CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"native\",\"version\":\"1\"}}}\n")); c.receive();
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"));
        for(int i=1;i<=4;++i) CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(i)+",\"method\":\"tools/list\"}\n"));
        c.blockedOutput();
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":900,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":900.00000000000000001}}\n{\"jsonrpc\":\"2.0\",\"id\":9007199254740991,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":9007199254740991.1}}\n{\"jsonrpc\":\"2.0\",\"id\":-9007199254740991,\"method\":\"ping\"}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":-9007199254740991.1}}\n{\"jsonrpc\":\"2.0\",\"id\":901,\"method\":\"ping\"}\n"));
        c.consumedInput();
        for(int i=1;i<=4;++i) CHECK(int(c.receive()["id"])==i);
        for(juce::int64 expected : {900LL,9007199254740991LL,-9007199254740991LL,901LL}) {
            auto reply=c.receive(1000);CHECK(!reply["id"].isString() && static_cast<juce::int64>(reply["id"])==expected);
        }
        c.eof();CHECK(c.ended());CHECK(c.code()==0);
    }
    {
        Child c(executable,args); CHECK(c.send("{"));c.eof();CHECK(int(c.receive()["error"]["code"])==-32700);CHECK(c.ended());CHECK(c.code()==0);
    }
    {
        Child c(executable,args); CloseHandle(c.output);c.output=nullptr;
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n"));
        CHECK(c.ended());CHECK(c.code()==1); // Broken stdout cancels this bridge's blocked stdin reader.
    }
    {
        Child c(executable,args+" --instance 01234567-89ab-cdef-0123-456789abcdef");
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"native\",\"version\":\"1\"}}}\n")); c.receive();
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"));
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\",\"arguments\":{\"instance_id\":\"11234567-89ab-cdef-0123-456789abcdef\"}}}\n"));
        CHECK(c.receive()["result"]["structuredContent"]["status"].toString()=="unauthorized");
        CHECK(c.send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\",\"arguments\":{\"instance_id\":\"01234567-89ab-cdef-0123-456789abcdef\"}}}\n"));
        CHECK(c.receive()["result"]["structuredContent"]["status"].toString()=="instance_gone");
        CHECK(c.send(R"({"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"air-preview://11234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef/01234567-89ab-cdef-0123-456789abcdef"}})" "\n"));const auto rejected=c.receive();CHECK(int(rejected["id"])==3);CHECK(int(rejected["error"]["code"])==-32002&&!rejected.hasProperty("result"));
        c.eof();CHECK(c.ended());CHECK(c.code()==0);
    }
}
#endif
}
int main(int argc,char** argv) { if(argc==2&&juce::String(argv[1])=="--alphabet-equivalence"){alphabetEquivalence();std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}binaryEnvelope();boundedBinaryEncoding();alphabetEquivalence();protocol();
#if JUCE_WINDOWS
    if(argc>1) bridge(argv[1]);
#endif
    std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl; return failures?1:0; }
