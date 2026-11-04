#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "RackLibraryClient.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
namespace airack {RackLibraryTransport rackLibraryHttpWindowsForTest(unsigned);}
using namespace airack;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<line<<": "<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
using Clock=std::chrono::steady_clock;
bool waitUntil(const std::function<bool()>& p){const auto end=Clock::now()+std::chrono::seconds(5);while(Clock::now()<end){if(p())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;}
struct Server {
    SOCKET listener=INVALID_SOCKET;unsigned port=0;std::atomic<bool> stop{false},entered{false};std::atomic<int> requests{0};std::string reply,captured,firstCaptured,afterFirst;bool stalled=false;std::jthread worker;
    explicit Server(std::string response,bool stall=false,std::string next={}):reply(std::move(response)),afterFirst(std::move(next)),stalled(stall){
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(listener==INVALID_SOCKET||bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,8))return;int bytes=sizeof(address);if(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&bytes))return;port=ntohs(address.sin_port);
        worker=std::jthread([this]{while(!stop){fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);timeval interval{0,100000};if(select(0,&readable,nullptr,nullptr,&interval)<=0)continue;const auto client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET)continue;
            std::string request;const auto deadline=Clock::now()+std::chrono::seconds(5);char buffer[4096];while(!stop&&Clock::now()<deadline&&request.find("\r\n\r\n")==std::string::npos){fd_set input;FD_ZERO(&input);FD_SET(client,&input);timeval span{0,100000};if(select(0,&input,nullptr,nullptr,&span)<=0)continue;const auto got=recv(client,buffer,sizeof(buffer),0);if(got<=0)break;request.append(buffer,size_t(got));}
            captured=request;if(requests==0)firstCaptured=request;const auto index=requests++;entered=true;const auto& bytes=index&& !afterFirst.empty()?afterFirst:reply;if(!bytes.empty())send(client,bytes.data(),int(bytes.size()),0);
            while(stalled&&!stop){fd_set input;FD_ZERO(&input);FD_SET(client,&input);timeval span{0,100000};if(select(0,&input,nullptr,nullptr,&span)>0&&recv(client,buffer,sizeof(buffer),0)<=0)break;}
            shutdown(client,SD_BOTH);closesocket(client);
        }});
    }
    ~Server(){stop=true;if(worker.joinable())worker.join();if(listener!=INVALID_SOCKET)closesocket(listener);}
};
void policy(){
    CHECK(validRackLibraryUrl(rackLibraryCatalogUrl));CHECK(validRackLibraryUrl(rackLibraryCountsUrl));CHECK(validRackLibraryUrl("https://github.com/Planetary-Records/air-rack-library/releases/download/v1/rack.zip"));
    CHECK(validRackLibraryUrl("https://release-assets.githubusercontent.com/github-production-release-asset/123/1234-abcd?sig=private",true));
    for(const auto* url:{"http://github.com/Planetary-Records/air-rack-library/releases/download/v/a.zip","https://127.0.0.1/a","https://localhost/a","https://github.com:443/Planetary-Records/air-rack-library/a","https://github.com/Other/repo/a","https://user@github.com/Planetary-Records/air-rack-library/a","https://github.com/Planetary-Records/air-rack-library/releases/download/v/a.zip#part","https://github.com/Planetary-Records/air-rack-library/releases/download/../a.zip","https://github.com/Planetary-Records/air-rack-library/releases/download/%2e%2e/a.zip","https://raw.githubusercontent.com/Planetary-Records/air-rack-library/main/a?token=x","https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=101&page=1","https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=100&page=17","https://release-assets.githubusercontent.com/github-production-release-asset/123/1234-abcd?sig=private"})CHECK(!validRackLibraryUrl(url));
    RackHttpResponse response;CHECK(rackLibraryHttpWindows()({"https://localhost/",{},32},[](auto*,auto){return true;},[]{return false;},response)==RackLibraryStatus::invalidData);
}
void transport(){
    {Server server("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nETag: \"etag\"\r\nConnection: close\r\n\r\nhello");CHECK(server.port!=0);RackHttpResponse response;std::string body;CHECK(rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,"old",5},[&](const void* p,std::size_t n){body.append(static_cast<const char*>(p),n);return true;},[]{return false;},response)==RackLibraryStatus::ready);CHECK(body=="hello");CHECK(response.code==200&&response.etag=="\"etag\"");CHECK(server.requests==1);CHECK(server.captured.find("If-None-Match: old")!=std::string::npos);CHECK(server.captured.find("Cookie:")==std::string::npos&&server.captured.find("Authorization:")==std::string::npos);}
    {Server server("HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");RackHttpResponse response;CHECK(rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,"tag",20},[](auto*,auto){return false;},[]{return false;},response)==RackLibraryStatus::ready);CHECK(response.code==304);}
    {Server server("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7200\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset: 1900000000\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");RackHttpResponse response;CHECK(rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,{},20},[](auto*,auto){return true;},[]{return false;},response)==RackLibraryStatus::rateLimited);CHECK(response.retryAfter=="7200"&&response.rateLimitRemaining=="0"&&response.rateLimitReset=="1900000000");}
    {Server server("HTTP/1.1 200 OK\r\nContent-Length: 30\r\nConnection: close\r\n\r\n");RackHttpResponse response;CHECK(rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,{},20},[](auto*,auto){return true;},[]{return false;},response)==RackLibraryStatus::byteLimit);}
    {Server server("HTTP/1.1 302 Found\r\nLocation: https://github.com/Other/private/a.zip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");RackHttpResponse response;CHECK(rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,{},20},[](auto*,auto){return true;},[]{return false;},response)==RackLibraryStatus::invalidData);CHECK(server.requests==1);}
    {Server server(std::string("HTTP/1.1 302 Found\r\nLocation: ")+rackLibraryCatalogUrl+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");RackHttpResponse response;CHECK(rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,{},20},[](auto*,auto){return true;},[]{return false;},response)==RackLibraryStatus::invalidData);CHECK(server.requests==6);}
    for(const bool bodyStall:{false,true}){Server server(bodyStall?"HTTP/1.1 200 OK\r\nContent-Length: 30\r\nConnection: close\r\n\r\n":"",true);std::atomic<bool> cancel{false};RackLibraryStatus status=RackLibraryStatus::unavailable;RackHttpResponse response;
        std::jthread worker([&]{status=rackLibraryHttpWindowsForTest(server.port)({rackLibraryCatalogUrl,{},100},[](auto*,auto){return true;},[&]{return cancel.load();},response);});CHECK(waitUntil([&]{return server.entered.load();}));const auto start=Clock::now();cancel=true;worker.join();const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();std::cout<<"stalled_"<<(bodyStall?"body":"headers")<<"_drain_ms="<<elapsed<<'\n';CHECK(elapsed<15000);CHECK(status==RackLibraryStatus::cancelled);}
}
void assetRepresentation(){
    const std::string binary="HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\narchive";
    for(const bool redirected:{false,true}){Server server(redirected?"HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/github-production-release-asset/123/1234-abcd?sig=private\r\nContent-Length: 0\r\nConnection: close\r\n\r\n":binary,false,binary);RackHttpResponse response;std::string body;
        CHECK(rackLibraryHttpWindowsForTest(server.port)({"https://api.github.com/repos/Planetary-Records/air-rack-library/releases/assets/123",{},7},[&](const void* data,std::size_t size){body.append(static_cast<const char*>(data),size);return true;},[]{return false;},response)==RackLibraryStatus::ready);CHECK(body=="archive");CHECK(server.requests==(redirected?2:1));CHECK(server.firstCaptured.find("Accept: application/octet-stream\r\n")!=std::string::npos);CHECK(server.firstCaptured.find("Accept: application/vnd.github+json")==std::string::npos);
    }
    Server metadata("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]");RackHttpResponse response;CHECK(rackLibraryHttpWindowsForTest(metadata.port)({rackLibraryCountsUrl,{},2},[](const void*,std::size_t){return true;},[]{return false;},response)==RackLibraryStatus::ready);CHECK(metadata.requests==1);CHECK(metadata.captured.find("Accept: application/vnd.github+json\r\n")!=std::string::npos);
}
void redirectQuota(){
    const auto now=juce::Time::currentTimeMillis()/1000;for(const auto& reset:{std::to_string(now+7200),std::string("18446744073709551615"),std::to_string(now-60)}){const bool exhausted=reset==std::to_string(now+7200);const auto reply=std::string("HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/github-production-release-asset/123/1234-abcd?sig=private\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset: ")+reset+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";Server server(reply,false,"HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\narchive");RackHttpResponse response;std::string body;const auto status=rackLibraryHttpWindowsForTest(server.port)({"https://api.github.com/repos/Planetary-Records/air-rack-library/releases/assets/123",{},7},[&](const void* data,std::size_t size){body.append(static_cast<const char*>(data),size);return true;},[]{return false;},response);
        CHECK(status==(exhausted?RackLibraryStatus::rateLimited:RackLibraryStatus::ready));CHECK(server.requests==(exhausted?1:2));CHECK(body==(exhausted?"":"archive"));if(exhausted){CHECK(response.code==302);CHECK(response.rateLimitRemaining=="0"&&response.rateLimitReset==juce::String(reset));}
    }
}
void unload(const juce::File& root){
    Server server("",true);const auto dll=LoadLibraryW(RACKSYNTH_LIBRARY_HTTP_OWNER_DLL);CHECK(dll!=nullptr);if(!dll)return;
    using Start=void*(*)(const wchar_t*,unsigned);using Stop=void(*)(void*);const auto start=reinterpret_cast<Start>(GetProcAddress(dll,"rackLibraryOwnerStart"));const auto stop=reinterpret_cast<Stop>(GetProcAddress(dll,"rackLibraryOwnerStop"));CHECK(start&&stop);if(!start||!stop){FreeLibrary(dll);return;}
    void* owner=start(root.getFullPathName().toWideCharPointer(),server.port);CHECK(owner!=nullptr);CHECK(waitUntil([&]{return server.entered.load();}));const auto began=Clock::now();if(owner)stop(owner);CHECK(FreeLibrary(dll));const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-began).count();std::cout<<"owner_cancel_join_unload_ms="<<elapsed<<'\n';CHECK(elapsed<15000);CHECK(GetModuleHandleW(RACKSYNTH_LIBRARY_HTTP_OWNER_DLL)==nullptr);
}
}
int main(int argc,char** argv){WSADATA sockets{};if(WSAStartup(MAKEWORD(2,2),&sockets))return 2;
    if(argc==3&&std::string(argv[1])=="--unload")unload(juce::File(argv[2]));
    else {if(argc!=2)return 2;policy();assetRepresentation();redirectQuota();transport();const auto root=juce::File(argv[1]).getChildFile(juce::Uuid().toString());CHECK(root.createDirectory().wasOk());const auto command="\""+juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName()+"\" --unload \""+root.getFullPathName()+"\"";juce::ChildProcess child;CHECK(child.start(command));const bool finished=child.waitForProcessToFinish(20000);CHECK(finished);if(!finished){child.kill();child.waitForProcessToFinish(5000);}std::cout<<child.readAllProcessOutput();CHECK(child.getExitCode()==0);}
    WSACleanup();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
