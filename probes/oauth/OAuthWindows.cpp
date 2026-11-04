#include "OAuthProbe.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <sddl.h>
#include <aclapi.h>
#include <cmath>
#include <thread>
#include <condition_variable>
#include <mutex>

namespace rack::oauth::detail
{
using juce::String;
namespace
{
struct Algorithm
{
    BCRYPT_ALG_HANDLE value {};
    explicit Algorithm(const wchar_t* name) { BCryptOpenAlgorithmProvider(&value,name,nullptr,0); }
    ~Algorithm() { if(value) BCryptCloseAlgorithmProvider(value,0); }
};
struct Key { BCRYPT_KEY_HANDLE value {}; ~Key() { if(value) BCryptDestroyKey(value); } };
struct Internet { HINTERNET value {}; ~Internet() { if(value) WinHttpCloseHandle(value); } };
// One unresolved native operation at most. Missing final notification or failed
// close quarantines its handles and buffers until process exit; never reuse them.
const auto nativeAvailable=std::make_shared<std::atomic_bool>(true);
void CALLBACK nativeCallback(HINTERNET,DWORD_PTR context,DWORD status,void* statusInfo,DWORD length)
{
    auto* state=reinterpret_cast<NativeHttpLifecycle*>(context); if(!state) return;
    using Event=NativeHttpLifecycle::Event;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) state->notify(Event::closing);
    else if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) state->notifyRequestError(statusInfo,length);
    else if(status==WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) state->notify(Event::sent);
    else if(status==WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) state->notify(Event::headers);
    else if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE) state->notify(Event::read,length);
}
std::vector<unsigned char> decode(const String& input)
{
    if(input.isEmpty() || input.length()>65536 || !input.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")) return {};
    auto s=input.replaceCharacter('-','+').replaceCharacter('_','/'); while(s.length()%4) s+="=";
    juce::MemoryOutputStream stream; if(!juce::Base64::convertFromBase64(stream,s)) return {};
    auto* p=static_cast<const unsigned char*>(stream.getData());
    return {p,p+stream.getDataSize()};
}
juce::var decodeJson(const String& input)
{
    auto bytes=decode(input); if(bytes.empty()) return {};
    if(!juce::CharPointer_UTF8::isValidString(reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()))) return {};
    return strictJson(String::fromUTF8(reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size())));
}
bool sha256(const void* input, size_t length, unsigned char (&out)[32])
{
    Algorithm alg(BCRYPT_SHA256_ALGORITHM);
    return alg.value && BCryptHash(alg.value,nullptr,0,(PUCHAR)input,static_cast<ULONG>(length),out,32)>=0;
}
bool noReparse(const juce::File& file)
{
    auto path=file;
    for(;;)
    {
        auto attrs=GetFileAttributesW(path.getFullPathName().toWideCharPointer());
        if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        auto parent=path.getParentDirectory(); if(parent==path) break; path=parent;
    }
    return true;
}
struct PrivateSecurity
{
    PSECURITY_DESCRIPTOR descriptor {};
    SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    PrivateSecurity()
    {
        HANDLE token {}; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return;
        DWORD size=0; GetTokenInformation(token,TokenUser,nullptr,0,&size); std::vector<unsigned char> data(size);
        const bool ok=GetTokenInformation(token,TokenUser,data.data(),size,&size)!=0; CloseHandle(token); if(!ok) return;
        LPWSTR sid {}; if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid)) return;
        auto sddl=String("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;")+String(sid)+")"; LocalFree(sid);
        if(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.toWideCharPointer(),SDDL_REVISION_1,&descriptor,nullptr)) attributes.lpSecurityDescriptor=descriptor;
    }
    ~PrivateSecurity() { if(descriptor) LocalFree(descriptor); }
    bool apply(const juce::File& file)
    {
        if(!descriptor) return false;
        return SetFileSecurityW(file.getFullPathName().toWideCharPointer(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor)!=0;
    }
};
}
String base64Url(const void* data,size_t size)
{
    return juce::Base64::toBase64(data,size).replaceCharacter('+','-').replaceCharacter('/','_').removeCharacters("=");
}
NativeHttpLifecycle::NativeHttpLifecycle(std::array<void*,3> h,String b,Close c,std::shared_ptr<std::atomic_bool> a)
    : handles(h),body(std::move(b)),close(std::move(c)),available(std::move(a)) {}
std::shared_ptr<NativeHttpLifecycle> NativeHttpLifecycle::create(std::array<void*,3> handles,String body,Close close,std::shared_ptr<std::atomic_bool> available)
{
    auto state=std::shared_ptr<NativeHttpLifecycle>(new NativeHttpLifecycle(handles,std::move(body),std::move(close),{}));
    bool free=true;
    if(!available || !available->compare_exchange_strong(free,false)) return {};
    state->available=std::move(available);
    state->retained=state;
    return state;
}
NativeHttpLifecycle::~NativeHttpLifecycle() = default;
void NativeHttpLifecycle::retireAdmission()
{
    // Relinquish this owner's slot exactly once before publishing completion.
    // Old references/destruction/final notifications cannot reopen a newer slot.
    if(auto slot=std::move(available)) *slot=true;
}
void NativeHttpLifecycle::expectCallbacks() {std::lock_guard lock(mutex);closed=false;}
void NativeHttpLifecycle::notify(Event next,size_t count)
{
    std::shared_ptr<NativeHttpLifecycle> release;
    {
        std::lock_guard lock(mutex);
        if(next==Event::closing) closed=true;
        else if(next==Event::error) failed=true;
        else {event=next;bytes=count;}
        if(closed&&cleanupDone&&closeSucceeded)
        {
            retireAdmission();
            release=std::move(retained);
        }
        changed.notify_all();
    }
}
void NativeHttpLifecycle::reset() {std::lock_guard lock(mutex);event=Event::none;bytes=0;}
size_t NativeHttpLifecycle::bytesRead() {std::lock_guard lock(mutex);return bytes;}
void NativeHttpLifecycle::notifyRequestError(const void* statusInfo,size_t length)
{
    WINHTTP_ASYNC_RESULT result {};
    if(statusInfo&&length>=sizeof(result)) std::memcpy(&result,statusInfo,sizeof(result));
    {std::lock_guard lock(mutex);if(nativeFailure==0)nativeFailure=result.dwError;}
    notify(Event::error);
}
unsigned NativeHttpLifecycle::nativeError() {std::lock_guard lock(mutex);return nativeFailure;}
bool NativeHttpLifecycle::wait(Event expected,const std::atomic_bool& cancel,Deadline deadline)
{
    std::unique_lock lock(mutex);
    while(!failed&&!closed&&event!=expected&&!cancel&&std::chrono::steady_clock::now()<deadline)
        changed.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(20)));
    return !failed&&!closed&&!cancel&&event==expected&&std::chrono::steady_clock::now()<deadline;
}
bool NativeHttpLifecycle::finish(const std::atomic_bool& cancel,Deadline deadline)
{
    bool start=false;
    {std::lock_guard lock(mutex);if(!cleanupStarted) {cleanupStarted=true;start=true;}}
    if(start)
    {
        auto owner=shared_from_this();
        try
        {
            // Close itself is synchronous. This sole cleanup owner can outlive the
            // caller, and must never retry a close-called handle, even on FALSE.
            std::thread([owner]
            {
                bool ok=true;
                for(auto handle:owner->handles)
                    if(handle && !owner->close(handle)) {ok=false;break;}
                std::shared_ptr<NativeHttpLifecycle> release;
                {
                    std::lock_guard lock(owner->mutex);
                    owner->cleanupDone=true;owner->closeSucceeded=ok;
                    if(ok&&owner->closed)
                    {
                        owner->retireAdmission();
                        release=std::move(owner->retained);
                    }
                    owner->changed.notify_all();
                }
            }).detach();
        }
        catch(const std::system_error&)
        {
            // Cannot start cleanup: retain ownership, fail closed, refuse new work.
            std::lock_guard lock(mutex);cleanupDone=true;closeSucceeded=false;
        }
    }
    std::unique_lock lock(mutex);
    while(!(cleanupDone&&(!closeSucceeded||closed))&&!cancel&&std::chrono::steady_clock::now()<deadline)
        changed.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(20)));
    return cleanupDone&&closeSucceeded&&closed&&!cancel&&std::chrono::steady_clock::now()<deadline;
}
String randomUrlToken(size_t count)
{
    if(count>256 || count<16) return {};
    std::vector<unsigned char> bytes(count);
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(count),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return {};
    return base64Url(bytes.data(),bytes.size());
}
String sha256Url(const String& input)
{
    unsigned char digest[32] {}; if(!sha256(input.toRawUTF8(),input.getNumBytesAsUTF8(),digest)) return {};
    return base64Url(digest,32);
}
bool verifyIdentity(const String& jwt,const String& jwks,const String& client,const String& nonce,int64_t now,String& subject)
{
    subject.clear(); if(jwt.length()>65536 || jwks.getNumBytesAsUTF8()>256*1024) return false;
    auto parts=juce::StringArray::fromTokens(jwt,".",""); if(parts.size()!=3) return false;
    auto header=decodeJson(parts[0]), claims=decodeJson(parts[1]), keys=strictJson(jwks);
    if(!header.isObject() || !claims.isObject() || header["alg"]!=juce::var("RS256") || !header["kid"].isString()
        || header.hasProperty("crit") || header.hasProperty("jku") || header.hasProperty("x5u")) return false;
    auto* list=keys["keys"].getArray(); if(!list || list->size()>32) return false;
    juce::var selected;
    for(auto& key:*list) if(key["kid"]==header["kid"])
    {
        if(!selected.isVoid()) return false; selected=key;
    }
    if(selected["kty"]!=juce::var("RSA") || selected["use"]!=juce::var("sig") || selected["alg"]!=juce::var("RS256")
        || !selected["n"].isString() || !selected["e"].isString()) return false;
    auto modulus=decode(selected["n"].toString()), exponent=decode(selected["e"].toString()), signature=decode(parts[2]);
    if(modulus.size()<256 || modulus.size()>1024 || exponent.empty() || exponent.size()>8 || signature.size()!=modulus.size()) return false;
    BCRYPT_RSAKEY_BLOB h { BCRYPT_RSAPUBLIC_MAGIC,static_cast<ULONG>(modulus.size()*8),static_cast<ULONG>(exponent.size()),static_cast<ULONG>(modulus.size()),0,0 };
    std::vector<unsigned char> blob(sizeof(h)+exponent.size()+modulus.size()); std::memcpy(blob.data(),&h,sizeof(h));
    std::memcpy(blob.data()+sizeof(h),exponent.data(),exponent.size()); std::memcpy(blob.data()+sizeof(h)+exponent.size(),modulus.data(),modulus.size());
    Algorithm algorithm(BCRYPT_RSA_ALGORITHM); Key key;
    if(!algorithm.value || BCryptImportKeyPair(algorithm.value,nullptr,BCRYPT_RSAPUBLIC_BLOB,&key.value,blob.data(),static_cast<ULONG>(blob.size()),0)<0) return false;
    auto signedData=parts[0]+"."+parts[1]; unsigned char digest[32] {};
    if(!sha256(signedData.toRawUTF8(),signedData.getNumBytesAsUTF8(),digest)) return false;
    BCRYPT_PKCS1_PADDING_INFO padding {BCRYPT_SHA256_ALGORITHM};
    if(BCryptVerifySignature(key.value,&padding,digest,32,signature.data(),static_cast<ULONG>(signature.size()),BCRYPT_PAD_PKCS1)<0) return false;
    if(claims["iss"]!=juce::var("https://auth.openai.com") || !claims["sub"].isString() || claims["sub"].toString().isEmpty()
        || claims["sub"].toString().length()>512) return false;
    const auto expiry=claims["exp"];
    if(!(expiry.isInt()||expiry.isInt64()||expiry.isDouble()) || !std::isfinite(static_cast<double>(expiry)) || static_cast<double>(expiry)<=static_cast<double>(now)) return false;
    if(claims.hasProperty("nbf"))
    {
        auto nbf=claims["nbf"]; if(!(nbf.isInt()||nbf.isInt64()||nbf.isDouble()) || !std::isfinite(static_cast<double>(nbf)) || static_cast<double>(nbf)>static_cast<double>(now)) return false;
    }
    auto aud=claims["aud"]; bool audience=aud.isString()&&aud.toString()==client;
    if(auto* array=aud.getArray())
    {
        audience=false;
        for(auto& value:*array) { if(!value.isString()) return false; if(value.toString()==client) audience=true; }
        if(array->size()>1 && claims["azp"]!=juce::var(client)) return false;
    }
    if(!audience || (claims.hasProperty("azp")&&claims["azp"]!=juce::var(client))) return false;
    if(nonce.isNotEmpty() && claims["nonce"]!=juce::var(nonce)) return false;
    subject=claims["sub"].toString(); return true;
}
bool nativeBodySizeAllowed(const HttpRequest& request) noexcept
{
    const auto maximum=request.method=="POST" && request.url=="https://api.openai.com/v1/responses"
        ? maximumStructuredRequestBytes : std::size_t(256*1024);
    return request.body.getNumBytesAsUTF8()<=maximum;
}
bool nativeHttpTimeouts(void* handle,const HttpRequest& request,NativeHttpLifecycle::Deadline deadline)
{
    int receive=5000;
    if(request.method=="POST"&&request.url=="https://api.openai.com/v1/responses")
    {
        // Stream gaps share the original request deadline; never start an
        // unbounded wait by passing zero or extend the caller's request budget.
        const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0||request.timeoutMs<=0){SetLastError(ERROR_WINHTTP_TIMEOUT);return false;}
        receive=static_cast<int>(std::min<std::int64_t>(remaining,request.timeoutMs));
    }
    return WinHttpSetTimeouts(handle,5000,5000,5000,receive)!=FALSE;
}
HttpResponse nativeHttp(const HttpRequest& request,const std::atomic_bool& cancel)
{
    const auto failure=[](HttpFailure phase){HttpResponse response;response.nativeError=GetLastError();response.failure=phase;return response;};
    // Fixed trust destinations; no URL supplied by token claims or caller reaches WinHTTP.
    String host,path;
    if(request.url=="https://auth.openai.com/api/accounts/oauth/token") {host="auth.openai.com";path="/api/accounts/oauth/token";}
    else if(request.url=="https://auth.openai.com/.well-known/jwks.json") {host="auth.openai.com";path="/.well-known/jwks.json";}
    else if(request.url=="https://api.openai.com/v1/models") {host="api.openai.com";path="/v1/models";}
    else if(request.url=="https://api.openai.com/v1/responses") {host="api.openai.com";path="/v1/responses";}
    else return {};
    if(cancel || !*nativeAvailable || request.maxBytes>4*1024*1024 || request.maxBytes==0 || request.timeoutMs<1 || request.timeoutMs>60000
        || !nativeBodySizeAllowed(request) || request.bearer.containsAnyOf("\r\n")
        || (request.method!="GET"&&request.method!="POST")) return {};
    Internet session {WinHttpOpen(L"AI Rack Synth OAuth Feasibility/0.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC)};
    if(!session.value) return failure(HttpFailure::open);
    Internet connection {WinHttpConnect(session.value,host.toWideCharPointer(),INTERNET_DEFAULT_HTTPS_PORT,0)};
    if(!connection.value) return failure(HttpFailure::connect);
    Internet handle {WinHttpOpenRequest(connection.value,request.method.toWideCharPointer(),path.toWideCharPointer(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!handle.value) return failure(HttpFailure::request);
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!WinHttpSetOption(handle.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))) return failure(HttpFailure::configuration);
    DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
    if(!WinHttpSetOption(handle.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable))) return failure(HttpFailure::configuration);
    // No SECURITY_FLAG_IGNORE_* options: Windows validates chain, hostname and time.
    String headers="Accept: "+String(path.endsWith("/responses")?"text/event-stream":"application/json")+"\r\n";
    if(request.contentType.isNotEmpty()) headers+="Content-Type: "+request.contentType+"\r\n";
    if(request.bearer.isNotEmpty()) headers+="Authorization: Bearer "+request.bearer+"\r\n";
    if(!WinHttpAddRequestHeaders(handle.value,headers.toWideCharPointer(),static_cast<DWORD>(headers.length()),WINHTTP_ADDREQ_FLAG_ADD)) return failure(HttpFailure::requestHeaders);
    // Transfer the complete hierarchy and callback-visible storage to an independent
    // owner. The cleanup thread starts only after initiating WinHTTP calls return.
    auto state=NativeHttpLifecycle::create({handle.value,connection.value,session.value},request.body,
        [](void* h){return WinHttpCloseHandle(h)!=FALSE;},nativeAvailable);
    if(!state) return {};
    auto raw=handle.value;
    handle.value=nullptr;connection.value=nullptr;session.value=nullptr;
    DWORD_PTR context=reinterpret_cast<DWORD_PTR>(state.get());
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(request.timeoutMs);
    if(!nativeHttpTimeouts(raw,request,deadline)
        || !WinHttpSetOption(raw,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))
        || WinHttpSetStatusCallback(raw,nativeCallback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)
    {auto response=failure(HttpFailure::configuration);state->finish(cancel,deadline);return response;}
    state->expectCallbacks();
    using Event=NativeHttpLifecycle::Event;
    HttpResponse response;
    const auto bodyBytes=static_cast<DWORD>(state->sendBody().getNumBytesAsUTF8());
    const auto waitFailure=[&](HttpFailure phase){return cancel?HttpFailure::cancelled:
        std::chrono::steady_clock::now()>=deadline?HttpFailure::deadline:phase;};
    bool ok=WinHttpSendRequest(raw,WINHTTP_NO_ADDITIONAL_HEADERS,0,(void*)state->sendBody().toRawUTF8(),bodyBytes,bodyBytes,context)!=FALSE;
    if(!ok){response.nativeError=GetLastError();response.failure=HttpFailure::send;}
    else if(!(ok=state->wait(Event::sent,cancel,deadline))) response.failure=waitFailure(HttpFailure::send);
    if(ok)
    {
        state->reset();
        ok=nativeHttpTimeouts(raw,request,deadline)&&WinHttpReceiveResponse(raw,nullptr)!=FALSE;
        if(!ok){response.nativeError=GetLastError();response.failure=waitFailure(HttpFailure::receive);}
        else if(!(ok=state->wait(Event::headers,cancel,deadline))) response.failure=waitFailure(HttpFailure::receive);
    }
    if(ok)
    {
        DWORD status=0,size=sizeof(status);
        ok=WinHttpQueryHeaders(raw,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)!=0;
        if(!ok){response.nativeError=GetLastError();response.failure=HttpFailure::responseHeaders;}
        response.status=static_cast<int>(status);
        wchar_t id[129] {}; size=sizeof(id);
        if(WinHttpQueryHeaders(raw,WINHTTP_QUERY_CUSTOM,L"x-request-id",id,&size,WINHTTP_NO_HEADER_INDEX)) response.requestId=String(id);
    }
    juce::MemoryOutputStream body;
    while(ok)
    {
        state->reset();
        if(!nativeHttpTimeouts(raw,request,deadline)||!WinHttpReadData(raw,state->readBuffer(),8192,nullptr)) {response.nativeError=GetLastError();response.failure=waitFailure(HttpFailure::read);ok=false;break;}
        if(!state->wait(Event::read,cancel,deadline)) {response.failure=waitFailure(HttpFailure::read);ok=false;break;}
        auto bytes=state->bytesRead();
        response.receivedBytes+=bytes;
        if(bytes==0) break;
        if(body.getDataSize()+bytes>request.maxBytes) {response.failure=HttpFailure::bodyLimit;ok=false;break;}
        body.write(state->readBuffer(),bytes);
    }
    const bool cleanup=state->finish(cancel,deadline);
    if(response.nativeError==0)response.nativeError=state->nativeError();
    response.interrupted=!ok || !cleanup || cancel;
    if(response.failure==HttpFailure::none){if(cancel)response.failure=HttpFailure::cancelled;else if(!cleanup)response.failure=HttpFailure::cleanup;}
    if(!response.interrupted && !std::memchr(body.getData(),0,body.getDataSize())
        && juce::CharPointer_UTF8::isValidString(static_cast<const char*>(body.getData()),static_cast<int>(body.getDataSize()))) response.body=body.toUTF8();
    else {if(!response.interrupted)response.failure=HttpFailure::bodyEncoding;response.interrupted=true;}
    return response;
}
Store::Store(juce::File dir):directory(std::move(dir)) {}
Store::~Store() { if(lockHandle) CloseHandle(lockHandle); }
bool Store::lock(const std::atomic_bool& cancel)
{
    if(lockHandle) return true;
    if(cancel || !noReparse(directory) || directory.createDirectory().failed()) return false;
    PrivateSecurity security; if(!security.apply(directory)) return false;
    auto file=directory.getChildFile("session.lock"); if(!noReparse(file)) return false;
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do
    {
        auto h=CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,0,&security.attributes,OPEN_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr);
        if(h!=INVALID_HANDLE_VALUE) {lockHandle=h;return true;}
        if(GetLastError()!=ERROR_SHARING_VIOLATION) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while(!cancel && std::chrono::steady_clock::now()<end);
    return false;
}
bool Store::load(juce::var& value)
{
    value=juce::var(); if(!lockHandle) return false;
    auto file=directory.getChildFile("session.bin"); if(!noReparse(file)) return false;
    if(!file.exists()) return true;
    if(file.getSize()<=0 || file.getSize()>256*1024) return false;
    juce::MemoryBlock encrypted; if(!file.loadFileAsData(encrypted)) return false;
    DATA_BLOB input {static_cast<DWORD>(encrypted.getSize()),static_cast<BYTE*>(encrypted.getData())},output {};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)) return false;
    value=strictJson(String::fromUTF8(reinterpret_cast<char*>(output.pbData),static_cast<int>(output.cbData)));
    SecureZeroMemory(output.pbData,output.cbData); LocalFree(output.pbData); return value.isObject();
}
bool Store::save(const juce::var& value)
{
    if(!lockHandle || !value.isObject()) return false;
    auto text=juce::JSON::toString(value,true); if(text.getNumBytesAsUTF8()>128*1024) return false;
    DATA_BLOB input {static_cast<DWORD>(text.getNumBytesAsUTF8()),(BYTE*)text.toRawUTF8()},output {};
    if(!CryptProtectData(&input,L"AI Rack Synth OAuthProbe",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)) return false;
    auto temporary=directory.getChildFile("session.bin.tmp"), destination=directory.getChildFile("session.bin");
    PrivateSecurity security;
    if(!security.descriptor || !noReparse(temporary) || !noReparse(destination)) {LocalFree(output.pbData);return false;}
    auto h=CreateFileW(temporary.getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,&security.attributes,CREATE_ALWAYS,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(h==INVALID_HANDLE_VALUE) {LocalFree(output.pbData);return false;}
    DWORD written=0; bool ok=WriteFile(h,output.pbData,output.cbData,&written,nullptr)&&written==output.cbData&&FlushFileBuffers(h);
    CloseHandle(h); LocalFree(output.pbData);
    if(ok) ok=security.apply(temporary) && MoveFileExW(temporary.getFullPathName().toWideCharPointer(),destination.getFullPathName().toWideCharPointer(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(!ok) DeleteFileW(temporary.getFullPathName().toWideCharPointer()); return ok;
}
Loopback::Loopback() { socket.createListener(0,"127.0.0.1"); }
Loopback::~Loopback() = default;
int Loopback::port() const { return socket.getBoundPort(); }
String Loopback::receive(const std::atomic_bool& cancel,int timeoutMs)
{
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(juce::jlimit(1,300000,timeoutMs));
    for(int connections=0;connections<12 && !cancel && std::chrono::steady_clock::now()<end;)
    {
        if(socket.waitUntilReady(true,50)!=1) continue;
        std::unique_ptr<juce::StreamingSocket> client(socket.waitForNextConnection()); ++connections;
        if(!client || client->getHostName()!="127.0.0.1") continue;
        String header; auto deadline=std::min(end,std::chrono::steady_clock::now()+std::chrono::seconds(2));
        while(!cancel && header.length()<8192 && std::chrono::steady_clock::now()<deadline && !header.contains("\r\n\r\n"))
        {
            if(client->waitUntilReady(true,20)!=1) continue;
            char bytes[1024]; auto n=client->read(bytes,sizeof(bytes),false); if(n<=0) break;
            if(!juce::CharPointer_ASCII::isValidString(bytes,n)) break;
            header+=String::fromUTF8(bytes,n);
        }
        auto first=header.upToFirstOccurrenceOf("\r\n",false,false);
        auto target=first.fromFirstOccurrenceOf("GET ",false,false).upToLastOccurrenceOf(" HTTP/1.1",false,false);
        bool valid=!cancel && header.length()<8192 && header.contains("\r\n\r\n") && first.startsWith("GET ")
            && first.endsWith(" HTTP/1.1") && target.startsWith("/auth/callback?")
            && header.containsIgnoreCase("\r\nHost: 127.0.0.1:"+String(port())+"\r\n");
        const char* reply=valid?"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nContent-Length: 41\r\n\r\nReturn to AI Rack Synth to check sign-in.\n":"HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        client->write(reply,static_cast<int>(std::strlen(reply)));
        if(valid) return target;
    }
    return {};
}
}
