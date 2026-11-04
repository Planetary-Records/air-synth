#include "RackLibraryClient.h"
#include <array>
#include <chrono>
#include <charconv>
#include <windows.h>
#include <winhttp.h>

namespace airack {
namespace {
using Status=RackLibraryStatus;
using Clock=std::chrono::steady_clock;
struct Url { juce::String host,path,target; };
bool parse(const juce::String& input,bool redirect,Url& out){
    if(input.getNumBytesAsUTF8()>16384||!input.startsWith("https://")||input.containsChar('#')||input.containsChar('\\'))return false;
    for(auto c:input)if(c<=32||c>=127)return false;
    const auto tail=input.substring(8);const auto slash=tail.indexOfChar('/');if(slash<=0)return false;
    out.host=tail.substring(0,slash);out.target=tail.substring(slash);out.path=out.target.upToFirstOccurrenceOf("?",false,false);
    if(out.host.containsAnyOf(":@%")||out.path.containsChar('%')||out.path.contains("//"))return false;
    for(const auto& part:juce::StringArray::fromTokens(out.path,"/",""))if(part=="."||part=="..")return false;
    const juce::String repo="/Planetary-Records/air-rack-library/";
    if(out.host=="github.com")return !out.target.containsChar('?')&&out.path.startsWith(repo+"releases/download/")&&out.path.length()>(repo+"releases/download/").length();
    if(out.host=="raw.githubusercontent.com")return !out.target.containsChar('?')&&out.path.startsWith(repo)&&out.path.substring(repo.length()).containsChar('/');
    if(out.host=="api.github.com"){
        const juce::String route="/repos/Planetary-Records/air-rack-library/releases";
        if(out.path==route){
            if(out.target==route)return true;
            const auto prefix=route+"?per_page=100&page=";if(!out.target.startsWith(prefix))return false;
            const auto page=out.target.substring(prefix.length());return page.containsOnly("0123456789")&&page==juce::String(page.getIntValue())&&page.getIntValue()>=1&&page.getIntValue()<=16;
        }
        const auto prefix=route+"/assets/";const auto id=out.path.substring(prefix.length());
        return !out.target.containsChar('?')&&out.path.startsWith(prefix)&&id.isNotEmpty()&&id.containsOnly("0123456789")&&!id.startsWithChar('0');
    }
    // GitHub's signed CDN route has no owner segment; it is admitted only after an approved redirect.
    if(out.host=="release-assets.githubusercontent.com"){
        if(!redirect||!out.path.startsWith("/github-production-release-asset/"))return false;
        const auto parts=juce::StringArray::fromTokens(out.path.substring(33),"/","");
        return parts.size()==2&&parts[0].isNotEmpty()&&parts[0].containsOnly("0123456789")&&parts[1].isNotEmpty()&&parts[1].containsOnly("0123456789abcdefABCDEF-");
    }
    return false;
}
struct HttpHandle {
    HINTERNET value=nullptr;explicit HttpHandle(HINTERNET h):value(h){}~HttpHandle(){if(value)WinHttpCloseHandle(value);}
    HttpHandle(const HttpHandle&)=delete;
};
juce::String header(HINTERNET h,DWORD query,const wchar_t* name=nullptr){
    DWORD bytes=0;WinHttpQueryHeaders(h,query,name,nullptr,&bytes,WINHTTP_NO_HEADER_INDEX);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||bytes>32768)return {};
    std::vector<wchar_t> data(bytes/sizeof(wchar_t)+1);
    return WinHttpQueryHeaders(h,query,name,data.data(),&bytes,WINHTTP_NO_HEADER_INDEX)?juce::String(data.data()):juce::String();
}
bool exhaustedQuota(const RackHttpResponse& response){
    const auto number=[](const juce::String& value,std::uint64_t& out){const auto text=value.trim().toStdString();if(text.empty()||text.size()>64||text.find_first_not_of("0123456789")!=std::string::npos)return false;const auto result=std::from_chars(text.data(),text.data()+text.size(),out);return result.ec==std::errc{}&&result.ptr==text.data()+text.size();};
    std::uint64_t remaining=1,reset=0;return number(response.rateLimitRemaining,remaining)&&remaining==0&&number(response.rateLimitReset,reset)&&reset<=std::uint64_t(INT64_MAX/1000)&&std::int64_t(reset*1000)>juce::Time::currentTimeMillis();
}
Status perform(const RackHttpRequest& input,const RackHttpSink& sink,const RackLibraryCancel& cancelled,RackHttpResponse& response,unsigned testPort=0){
    if(input.maximumBytes==0||input.maximumBytes>rackPackageLimits::archiveBytes||input.etag.getNumBytesAsUTF8()>1024||input.etag.containsAnyOf("\r\n"))return Status::invalidData;
    const bool asset=input.url.startsWith("https://github.com/")||input.url.contains("/releases/assets/");
    const auto deadline=Clock::now()+std::chrono::seconds(asset?300:30);
    const auto check=[&]{if(cancelled&&cancelled())return Status::cancelled;return Clock::now()>=deadline?Status::timeout:Status::ready;};
    auto failure=[&]{const auto status=check();return status!=Status::ready?status:GetLastError()==ERROR_WINHTTP_TIMEOUT?Status::timeout:Status::networkFailure;};
    HttpHandle session(WinHttpOpen(L"AIR Rack Library/0.1",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.value||!WinHttpSetTimeouts(session.value,3000,3000,3000,2000))return failure();
    DWORD retries=1;if(!WinHttpSetOption(session.value,WINHTTP_OPTION_CONNECT_RETRIES,&retries,sizeof(retries)))return failure();
    juce::String url=input.url;response={};
    for(unsigned redirects=0;redirects<=5;++redirects){
        auto state=check();if(state!=Status::ready)return state;Url parsed;if(!parse(url,redirects!=0,parsed))return Status::invalidData;
        const wchar_t* connectHost=parsed.host.toWideCharPointer();INTERNET_PORT port=443;DWORD flags=WINHTTP_FLAG_SECURE;
#ifdef AIRACK_LIBRARY_HTTP_TESTS
        if(testPort){connectHost=L"127.0.0.1";port=INTERNET_PORT(testPort);flags=0;}
#else
        (void)testPort;
#endif
        HttpHandle connection(WinHttpConnect(session.value,connectHost,port,0));if(!connection.value)return failure();
        HttpHandle request(WinHttpOpenRequest(connection.value,L"GET",parsed.target.toWideCharPointer(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags));if(!request.value)return failure();
        DWORD disable=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_AUTHENTICATION|WINHTTP_DISABLE_COOKIES;
        if(!WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)))return failure();
        const auto accept=parsed.host=="api.github.com"&&parsed.path.startsWith("/repos/Planetary-Records/air-rack-library/releases/assets/")?"application/octet-stream":"application/vnd.github+json";
        const auto headers=juce::String("Accept: ")+accept+"\r\nX-GitHub-Api-Version: 2026-03-10\r\n"+(input.etag.isEmpty()?juce::String():"If-None-Match: "+input.etag+"\r\n");
        if(!WinHttpSendRequest(request.value,headers.toWideCharPointer(),DWORD(headers.length()),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.value,nullptr))return failure();
        state=check();if(state!=Status::ready)return state;
        DWORD code=0,codeBytes=sizeof(code);if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&codeBytes,WINHTTP_NO_HEADER_INDEX))return failure();
        response.code=int(code);response.etag=header(request.value,WINHTTP_QUERY_ETAG);response.location=header(request.value,WINHTTP_QUERY_LOCATION);response.link=header(request.value,WINHTTP_QUERY_CUSTOM,L"Link");
        response.retryAfter=header(request.value,WINHTTP_QUERY_CUSTOM,L"Retry-After");
        response.rateLimitRemaining=header(request.value,WINHTTP_QUERY_CUSTOM,L"X-RateLimit-Remaining");
        response.rateLimitReset=header(request.value,WINHTTP_QUERY_CUSTOM,L"X-RateLimit-Reset");
        if(response.etag.getNumBytesAsUTF8()>1024)return Status::invalidData;
        for(auto* value:{&response.retryAfter,&response.rateLimitRemaining,&response.rateLimitReset})if(value->getNumBytesAsUTF8()>64)*value={};
        if(code==301||code==302||code==303||code==307||code==308){if(redirects==5)return Status::invalidData;Url next;if(!parse(response.location,true,next))return Status::invalidData;state=check();if(state!=Status::ready)return state;if(exhaustedQuota(response))return Status::rateLimited;url=response.location;continue;}
        if(code==403||code==429)return Status::rateLimited;
        if(code==304)return Status::ready;
        if(code!=200)return Status::networkFailure;
        const auto length=header(request.value,WINHTTP_QUERY_CONTENT_LENGTH);
        if(length.isNotEmpty()){
            if(!length.containsOnly("0123456789")||length.length()>20)return Status::invalidData;
            std::uint64_t total=0;for(auto c:length){if(total>(UINT64_MAX-std::uint64_t(c-'0'))/10)return Status::byteLimit;total=total*10+std::uint64_t(c-'0');}
            if(total>input.maximumBytes)return Status::byteLimit;
        }
        std::array<char,65536> buffer{};std::uint64_t total=0;
        while(true){state=check();if(state!=Status::ready)return state;DWORD got=0;
            if(!WinHttpReadData(request.value,buffer.data(),DWORD(buffer.size()),&got))return failure();
            state=check();if(state!=Status::ready)return state;if(!got)return Status::ready;
            if(got>input.maximumBytes-total)return Status::byteLimit;total+=got;
            if(!sink(buffer.data(),got)){state=check();return state==Status::ready?Status::byteLimit:state;}
        }
    }
    return Status::invalidData;
}
}
bool validRackLibraryUrl(const juce::String& url,bool redirect){Url parsed;return parse(url,redirect,parsed);}
RackLibraryTransport rackLibraryHttpWindows(){return [](const auto& r,const auto& sink,const auto& cancel,auto& response){return perform(r,sink,cancel,response);};}
#ifdef AIRACK_LIBRARY_HTTP_TESTS
RackLibraryTransport rackLibraryHttpWindowsForTest(unsigned port){return [port](const auto& r,const auto& sink,const auto& cancel,auto& response){return perform(r,sink,cancel,response,port);};}
#endif
}
