#include "FrozenRackExamples.h"
#include "OwnedFixture.h"
#include "CompanionBackend.h"
#include "GenerationLog.h"
#include "LocalPreviewExchange.h"
#include "RoutedSynthEngine.h"
#include <limits>
#include <juce_cryptography/juce_cryptography.h>
#include "CompanionRuntimeServer.h"
#include "AiCatalog.h"
#include "StrictJson.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "UserReach.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dbghelp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace airack;
using namespace airack::companion;
namespace
{
using String=juce::String;
using Status=rack::oauth::Status;
namespace oauth=rack::oauth;
int checks=0,failures=0,groups=0,groupChecks=0,groupFailures=0;
const char* group="";
void check(bool pass,const char* expression,int line)
{++checks;if(!pass){++failures;std::cerr<<group<<':'<<line<<' '<<expression<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void finishGroup(){if(groups)std::cout<<group<<" checks="<<checks-groupChecks<<" failures="<<failures-groupFailures<<std::endl;}
void begin(const char* name){finishGroup();group=name;++groups;groupChecks=checks;groupFailures=failures;std::cout<<name<<std::endl;}
template<class F> bool observe(F f,std::uint64_t ms=3000)
{const auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& f:fields)o->setProperty(f.first,f.second);return o;}
String json(const juce::var& v){return juce::JSON::toString(v,true,17);}
String b64(const void* p,std::size_t n){return juce::Base64::toBase64(p,n).replaceCharacter('+','-').replaceCharacter('/','_').removeCharacters("=");}
String b64(const String& s){return b64(s.toRawUTF8(),s.getNumBytesAsUTF8());}
String query(const String& url,const String& key)
{for(const auto& p:juce::StringArray::fromTokens(url.fromFirstOccurrenceOf("?",false,false),"&",""))if(p.upToFirstOccurrenceOf("=",false,false)==key)return juce::URL::removeEscapeChars(p.fromFirstOccurrenceOf("=",false,false));return {};}
String event(const String& type,juce::var fields={})
{if(!fields.isObject())fields=object({});fields.getDynamicObject()->setProperty("type",type);return "event: "+type+"\ndata: "+json(fields)+"\n\n";}
String proposal()
{return "{\"version\":1,\"parameter_edits\":[{\"id\":\"output.gain_db\",\"normalized\":0.5}],\"rack_edits\":[],\"macro_edits\":[],\"pattern\":null,\"characteristics\":\"warm\",\"explanation\":\"explicit edit\"}";}
String completed(const String& text=proposal())
{
    const auto content=object({{"type","output_text"},{"text",text}});
    const auto message=object({{"type","message"},{"content",juce::Array<juce::var>{content}}});
    const auto response=object({{"status","completed"},{"output",juce::Array<juce::var>{message}}});
    return event("response.output_text.delta",object({{"delta",text}}))+event("response.completed",object({{"response",response}}));
}

// Test-only keys. Production verifies the actual BCrypt signature and native store.
struct SigningKey
{
    BCRYPT_ALG_HANDLE alg{};BCRYPT_KEY_HANDLE key{};String jwks;
    SigningKey()
    {
        if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_RSA_ALGORITHM,nullptr,0)<0 || BCryptGenerateKeyPair(alg,&key,2048,0)<0 || BCryptFinalizeKeyPair(key,0)<0)throw std::runtime_error("native signing fixture");
        ULONG n=0;if(BCryptExportKey(key,nullptr,BCRYPT_RSAPUBLIC_BLOB,nullptr,0,&n,0)<0)throw std::runtime_error("native public key fixture");
        std::vector<unsigned char> blob(n);if(BCryptExportKey(key,nullptr,BCRYPT_RSAPUBLIC_BLOB,blob.data(),n,&n,0)<0)throw std::runtime_error("native public export fixture");
        const auto* h=reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(blob.data());const auto* e=blob.data()+sizeof(*h);
        jwks=json(object({{"keys",juce::Array<juce::var>{object({{"kty","RSA"},{"use","sig"},{"alg","RS256"},{"kid","test-key"},{"e",b64(e,h->cbPublicExp)},{"n",b64(e+h->cbPublicExp,h->cbModulus)}})}}}));
    }
    ~SigningKey(){if(key)BCryptDestroyKey(key);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
    String token(const String& nonce,std::int64_t expiry,const String& client="issued-client",const String& subject="synthetic-user")
    {
        const auto msg=b64("{\"alg\":\"RS256\",\"kid\":\"test-key\"}")+"."+b64(json(object({{"iss","https://auth.openai.com"},{"aud",client},{"sub",subject},{"exp",juce::var(expiry)},{"nonce",nonce}})));
        BCRYPT_ALG_HANDLE hashAlg{};if(BCryptOpenAlgorithmProvider(&hashAlg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("native hash fixture");
        unsigned char hash[32]{};const auto hashed=BCryptHash(hashAlg,nullptr,0,(PUCHAR)msg.toRawUTF8(),(ULONG)msg.getNumBytesAsUTF8(),hash,32);BCryptCloseAlgorithmProvider(hashAlg,0);if(hashed<0)throw std::runtime_error("native hash failed");
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};ULONG n=0;if(BCryptSignHash(key,&padding,hash,32,nullptr,0,&n,BCRYPT_PAD_PKCS1)<0)throw std::runtime_error("native signing length");
        std::vector<unsigned char> signature(n);if(BCryptSignHash(key,&padding,hash,32,signature.data(),n,&n,BCRYPT_PAD_PKCS1)<0)throw std::runtime_error("native signing failed");return msg+"."+b64(signature.data(),n);
    }
};
struct Temp
{
    juce::File root=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile(".superpowers/sdd/2026-10-05-m4-companion-oauth-backend/worker/fixtures");
    juce::File dir=root.getChildFile(juce::Uuid().toString());
    Temp(){if(!juce::File::isAbsolutePath(root.getFullPathName()) || !dir.isAChildOf(root) || dir.createDirectory().failed())throw std::runtime_error("owned fixture root");}
    ~Temp(){if(juce::File::isAbsolutePath(dir.getFullPathName()) && dir.isAChildOf(root) && dir.getParentDirectory()==root)dir.deleteRecursively();}
    juce::File credentials() const{return dir.getChildFile("Credentials");}
    juce::File preferences() const{return dir.getChildFile("Preferences");}
};
enum class BrowserMode {callback,deny,wrongState,badSignature,noCallback,delayedCallback,fail,throwError,expired};
struct Fixture:std::enable_shared_from_this<Fixture>
{
    Temp temp;std::shared_ptr<SigningKey> key;
    std::atomic<std::int64_t> now{1700000000};
    std::atomic_int calls{0},responses{0},tokens{0},clockCalls{0},browserCalls{0},active{0},peak{0};
    std::atomic_bool blocked{false},release{false},callbackOkay{true},priorityGood{true},releaseLate{false};
    String nonce,scope="openid resource.invoke chatgpt.tokens.use.direct",models="{\"models\":[{\"slug\":\"account-model\",\"display_name\":\"Account Model\",\"visibility\":\"list\"},{\"slug\":\"second-model\",\"display_name\":\"Second\",\"visibility\":\"list\"}]}";
    String stream=completed(),tokenError="invalid_grant";int tokenStatus=200,responseStatus=200;
    String client="issued-client",subject="synthetic-user",authorizeClient;
    BrowserMode browserMode=BrowserMode::callback;String blockEndpoint;bool checkPriority=false;
    std::vector<oauth::HttpRequest> requests;std::vector<std::thread> sockets;
    explicit Fixture(std::shared_ptr<SigningKey> k):key(std::move(k)){}
    ~Fixture(){joinSockets();}
    void joinSockets(){for(auto& t:sockets)if(t.joinable())t.join();sockets.clear();}
    oauth::Transport transport()
    {
        return [self=shared_from_this()](const oauth::HttpRequest& req,const std::atomic_bool& cancel)->oauth::HttpResponse
        {
            auto& f=*self;++f.calls;f.requests.push_back(req);const int n=f.active.fetch_add(1)+1;f.peak.store((std::max)(f.peak.load(),n));
            struct Done{std::atomic_int& v;~Done(){--v;}}done{f.active};
            if(f.checkPriority && GetThreadPriority(GetCurrentThread())!=THREAD_PRIORITY_BELOW_NORMAL)f.priorityGood=false;
            if(f.blockEndpoint.isNotEmpty() && req.url.endsWith(f.blockEndpoint))
            {f.blocked=true;const auto end=GetTickCount64()+4000;while(!f.release && !cancel && GetTickCount64()<end)Sleep(1);if(!f.release && !cancel)return {0,{}, {},true};}
            if(cancel)return {0,{}, {},true};
            if(req.url.endsWith("/oauth/token"))
            {
                ++f.tokens;if(f.tokenStatus!=200)return {f.tokenStatus,json(object({{"error",f.tokenError},{"error_description","PRIVATE synthetic description"}})),"123e4567-e89b-42d3-a456-426614174000",false};
                auto jwt=f.key->token(f.nonce,f.now.load()+3600,f.client,f.subject);if(f.browserMode==BrowserMode::badSignature)jwt=jwt.dropLastCharacters(8)+"AAAAAAAA";
                return {200,json(object({{"access_token","synthetic-access"},{"refresh_token","synthetic-refresh"},{"id_token",jwt},{"token_type","Bearer"},{"scope",f.scope},{"expires_in",3600}})),{},false};
            }
            if(req.url.endsWith("jwks.json"))return {200,f.key->jwks,{},false};
            if(req.url.endsWith("/models"))return {200,f.models,{},false};
            if(req.url.endsWith("/responses")){++f.responses;return {f.responseStatus,f.stream,"synthetic-request",false};}
            return {0,{}, {},true};
        };
    }
    oauth::Clock clock(){return [self=shared_from_this()]{++self->clockCalls;return self->now.load();};}
    BrowserOpen browser()
    {
        return [self=shared_from_this()](const String& url)
        {
            auto& f=*self;++f.browserCalls;if(f.browserMode==BrowserMode::throwError)throw std::runtime_error("private browser failure");if(f.browserMode==BrowserMode::fail)return false;
            f.nonce=query(url,"nonce");f.authorizeClient=query(url,"client_id");if(f.browserMode==BrowserMode::noCallback)return true;
            const auto redirect=query(url,"redirect_uri");const int port=redirect.fromFirstOccurrenceOf("127.0.0.1:",false,false).upToFirstOccurrenceOf("/",false,false).getIntValue();
            auto target="/auth/callback?state="+(f.browserMode==BrowserMode::wrongState?String("wrong"):query(url,"state"));
            target+=f.browserMode==BrowserMode::deny?String("&error=access_denied"):String("&code=synthetic-code&client_id=")+f.client;
            if(f.browserMode==BrowserMode::expired)f.now.fetch_add(301);
            const bool delayed=f.browserMode==BrowserMode::delayedCallback;
            f.sockets.emplace_back([self,port,target,delayed]
            {
                // A cancellation's deadline can include this thread: the test releases it once the cancelled job has returned.
                if(delayed)for(const auto until=GetTickCount64()+700;!self->releaseLate && GetTickCount64()<until;)Sleep(1);
                juce::StreamingSocket socket;
                if(port<1 || !socket.connect("127.0.0.1",port,500)){if(!delayed)self->callbackOkay=false;return;}
                const auto header="GET "+target+" HTTP/1.1\r\nHost: 127.0.0.1:"+String(port)+"\r\nConnection: close\r\n\r\n";
                if(socket.waitUntilReady(false,500)!=1 || socket.write(header.toRawUTF8(),static_cast<int>(header.getNumBytesAsUTF8()))!=static_cast<int>(header.getNumBytesAsUTF8())){if(!delayed)self->callbackOkay=false;return;}
                char reply[512]{};const int ready=socket.waitUntilReady(true,2500);const int n=ready==1?socket.read(reply,sizeof(reply),false):0;
                if(!delayed && (n<=0 || !String::fromUTF8(reply,n).startsWith("HTTP/1.1 200 OK")))self->callbackOkay=false;
            });return true;
        };
    }
    BackendOptions options(){return {temp.credentials(),temp.preferences(),transport(),clock(),browser()};}
    bool seed()
    {
        oauth::OAuthProbe p(temp.credentials(),transport(),clock());const auto s=p.beginSignIn(49123);nonce=query(s.browserUrl,"nonce");
        return s.result.status==Status::awaitingCallback && p.acceptCallback("/auth/callback?state="+query(s.browserUrl,"state")+"&code=synthetic-code&client_id=issued-client").status==Status::connected;
    }
};
std::shared_ptr<Fixture> fixture(const std::shared_ptr<SigningKey>& key){return std::make_shared<Fixture>(key);}
std::uint64_t incarnation(){FILETIME a{},b{},c{},d{};return GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d)?(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime:0;}
Frame control(MessageKind k,juce::Uuid session,ai::RequestTag tag)
{Frame f{k,session,{}};if(!encodeControl({tag,RuntimeStatus::ready},f.payload))throw std::runtime_error("control fixture");return f;}
struct Job
{
    RuntimeState state;RuntimeConnectionId id=0;juce::Uuid runtime,session=juce::Uuid::null();RuntimeJob value;
    explicit Job(std::uint32_t capabilities=0)
    {
        const auto now=std::chrono::steady_clock::now();id=state.open({GetCurrentProcessId(),incarnation()},now);
        auto hello=control(MessageKind::hello,juce::Uuid::null(),{runtime,0,0,0});
        if(!encodeControl({{runtime,0,0,0},RuntimeStatus::ready,capabilities},hello.payload)
            ||!id||state.receive(id,hello,now)!=RuntimeStatus::ready)throw std::runtime_error("job registration");
        Frame welcome;if(!state.takeReply(id,welcome))throw std::runtime_error("job welcome");session=welcome.session;state.finishReply(id,true);
    }
    RuntimeJob account(AccountAction action=AccountAction::status,const String& model={})
    {Frame f{MessageKind::account,session,{}};if(!encodeAccountCommand({action,model},f.payload) || state.receive(id,f,std::chrono::steady_clock::now())!=RuntimeStatus::ready || !state.takeJob(value))throw std::runtime_error("account job");return value;}
    RuntimeJob generate(ai::GenerationRequest r)
    {r.tag.runtime=runtime;Frame f{MessageKind::generate,session,{}};if(!encodeRequest(r,f.payload) || state.receive(id,f,std::chrono::steady_clock::now())!=RuntimeStatus::ready || !state.takeJob(value))throw std::runtime_error("generation job");return value;}
    void cancel(CompanionBackend& b){state.disconnect(id);b.cancel(value);}
    void finish(const Frame& answer){CHECK(state.finishJob(value,answer));Frame delivered;CHECK(state.takeReply(id,delivered));state.finishReply(id,true);}
};
ai::GenerationRequest request()
{ai::GenerationRequest r;r.tag={juce::Uuid(),1,7,11};r.model="account-model";r.instruction="Warm rounded sound";r.role="lead";r.key="D";r.scale="minor";r.capturedPhrase=PatternDocument{};r.capturedPhrase->meter={1,4};r.capturedPhrase->noteCount=1;r.capturedPhrase->notes[0]={.25,.125,65,87,2};r.capturedBpm=400;return r;}
AccountReply account(CompanionBackend& b,AccountAction action=AccountAction::status,const String& model={})
{Job j;const auto job=j.account(action,model);const auto f=b.run(job);AccountReply reply;CHECK(f.kind==MessageKind::accountResult && f.session==job.session);CHECK(decodeAccountReply(bytes(f.payload),reply));CHECK(!reply.modelVerified);juce::MemoryBlock framed;CHECK(encodeFrame(f,framed));return reply;}
ai::GenerationResult result(const Frame& f,const RuntimeJob& j)
{ai::GenerationResult r;CHECK(f.kind==MessageKind::result && f.session==j.session);CHECK(decodeResult(bytes(f.payload),std::get<ai::GenerationRequest>(j.input),r));CHECK(r.tag==std::get<ai::GenerationRequest>(j.input).tag);return r;}
void withheld(const ai::GenerationResult& r,ai::GenerationStatus s,unsigned calls)
{CHECK(r.status==s);CHECK(r.inferenceCalls==calls);CHECK(!r.candidate && r.audio.left.empty() && r.audio.right.empty());CHECK(r.edits.parameters.empty() && r.edits.racks.empty() && r.edits.macros.empty() && !r.edits.pattern);}
bool preference(const juce::File& dir,const juce::var& record)
{std::atomic_bool stop=false;oauth::detail::Store store(dir);return store.lock(stop) && store.save(record);}
juce::var preferenceRecord(const juce::File& dir)
{std::atomic_bool stop=false;oauth::detail::Store store(dir);juce::var v;CHECK(store.lock(stop));CHECK(store.load(v));return v;}

void preference_and_model_configuration(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__); // Removing fresh strict preference load or model equality breaks this group.
    auto f=fixture(key);CHECK(f->seed());CompanionBackend b(f->options());
    auto reply=account(b);CHECK(reply.status==RuntimeStatus::ready && reply.providerStatus==Status::connected && reply.selectedModel.isEmpty());
    auto unconfiguredStorage=std::make_unique<Job>();auto& unconfigured=*unconfiguredStorage;auto jStorage=testing::fixtureValue([&]{return unconfigured.generate(request());});auto& j=*jStorage;auto frame=b.run(j);ControlMessage failure;
    CHECK(frame.kind==MessageKind::failure && frame.session==j.session && decodeControl(bytes(frame.payload),failure));CHECK(failure.tag==std::get<ai::GenerationRequest>(j.input).tag && failure.status==RuntimeStatus::modelNotConfigured);CHECK(f->responses==0);
    reply=account(b,AccountAction::selectModel,"account-model");CHECK(reply.status==RuntimeStatus::ready && reply.providerStatus==Status::connected && reply.selectedModel=="account-model" && reply.models.size()==2);
    const auto stored=preferenceRecord(f->temp.preferences());CHECK(stored.getDynamicObject() && stored.getDynamicObject()->getProperties().size()==2);CHECK(stored["version"].isInt() && int(stored["version"])==1 && stored["selectedModel"].toString()=="account-model");
    CompanionBackend reopened(f->options());CHECK(account(reopened).selectedModel=="account-model");
    auto mismatchStorage=testing::fixtureValue([&]{return request();});auto& mismatch=*mismatchStorage;mismatch.model="second-model";auto wrongStorage=std::make_unique<Job>();auto& wrong=*wrongStorage;auto wrongJobStorage=testing::fixtureValue([&]{return wrong.generate(mismatch);});const auto& wrongJob=*wrongJobStorage;frame=reopened.run(wrongJob);
    CHECK(frame.kind==MessageKind::failure && decodeControl(bytes(frame.payload),failure) && failure.status==RuntimeStatus::modelNotConfigured && failure.tag==std::get<ai::GenerationRequest>(wrongJob.input).tag);CHECK(f->responses==0);
    reply=account(b,AccountAction::selectModel,"unauthorized-model");CHECK(reply.status==RuntimeStatus::modelNotConfigured && reply.errorCode=="model_not_authorized" && reply.selectedModel=="account-model");CHECK(preferenceRecord(f->temp.preferences())["selectedModel"].toString()=="account-model");
    CHECK(f->temp.preferences().getChildFile("session.bin.tmp").createDirectory().wasOk());reply=account(b,AccountAction::selectModel,"second-model");CHECK(reply.status==RuntimeStatus::unavailable && reply.providerStatus==Status::storageFailure && reply.selectedModel=="account-model");CHECK(preferenceRecord(f->temp.preferences())["selectedModel"].toString()=="account-model");CHECK(f->temp.preferences().getChildFile("session.bin.tmp").deleteRecursively());
    CHECK(preference(f->temp.preferences(),object({{"version",1},{"selectedModel",""}})));CHECK(account(b).selectedModel.isEmpty());
    const juce::var invalid[] {
        object({{"version",2},{"selectedModel","account-model"}}),object({{"version",true},{"selectedModel","account-model"}}),object({{"version",1.0},{"selectedModel","account-model"}}),
        object({{"selectedModel","account-model"}}),object({{"version",1}}),object({{"version",1},{"selectedModel",42}}),object({{"version",1},{"selectedModel","bad model"}}),
        object({{"version",1},{"selectedModel",String::repeatedString("a",257)}}),object({{"version",1},{"selectedModel",String::fromUTF8("mod\xc3\xa8le")}}),object({{"version",1},{"selectedModel","account-model"},{"extra",true}})
    };
    for(const auto& record:invalid){CHECK(preference(f->temp.preferences(),record));reply=account(b);CHECK(reply.status==RuntimeStatus::unavailable && reply.providerStatus==Status::storageFailure);CHECK(json(preferenceRecord(f->temp.preferences()))==json(record));}
    CHECK(f->temp.preferences().getChildFile("session.bin").replaceWithText("invalid encrypted preference"));reply=account(b);CHECK(reply.status==RuntimeStatus::unavailable && reply.providerStatus==Status::storageFailure);
    CHECK(preference(f->temp.preferences(),object({{"version",1},{"selectedModel","account-model"}})));
    for(int which=0;which<4;++which){auto options=f->options();if(which==0)options.credentialDirectory={};if(which==1)options.preferenceDirectory={};if(which==2)options.preferenceDirectory=options.credentialDirectory;if(which==3)options.preferenceDirectory=options.credentialDirectory.getChildFile("nested");const auto calls=f->calls.load();CompanionBackend invalidDirs(std::move(options));reply=account(invalidDirs);CHECK(reply.status==RuntimeStatus::unavailable && reply.providerStatus==Status::storageFailure);CHECK(f->calls==calls && f->browserCalls==0);}
    auto nestedOptions=f->options();nestedOptions.credentialDirectory=nestedOptions.preferenceDirectory.getChildFile("nested");CompanionBackend reversed(nestedOptions);CHECK(account(reversed).providerStatus==Status::storageFailure);
    {
        std::atomic_bool lockStop=false;oauth::detail::Store held(f->temp.preferences());CHECK(held.lock(lockStop));auto lockedStorage=std::make_unique<Job>();auto& locked=*lockedStorage;auto jobStorage=testing::fixtureValue([&]{return locked.account();});auto& job=*jobStorage;Frame answer;std::atomic_bool done=false;
        std::thread worker([&]{answer=b.run(job);done=true;});Sleep(100);CHECK(!done);const auto cancelledAt=GetTickCount64();locked.cancel(b);CHECK(observe([&]{return done.load();},1500));worker.join();AccountReply cancelled;CHECK(decodeAccountReply(bytes(answer.payload),cancelled) && cancelled.status==RuntimeStatus::cancelled && cancelled.providerStatus==Status::cancelled);CHECK(GetTickCount64()-cancelledAt<1500);CHECK(!locked.state.finishJob(job,answer));
    }
    auto badJobStorage=std::make_unique<RuntimeJob>();auto& badJob=*badJobStorage;badJob.input=request();badJob.session=juce::Uuid();const auto invalidFrame=b.run(badJob);CHECK(invalidFrame.kind==MessageKind::failure && decodeControl(bytes(invalidFrame.payload),failure) && failure.status==RuntimeStatus::invalidMessage);badJob.session=juce::Uuid::null();auto empty=b.run(badJob);juce::MemoryBlock encoded;CHECK(!encodeFrame(empty,encoded));
}

void account_switch(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__);auto f=fixture(key);CHECK(f->seed());CompanionBackend backend(f->options());
    auto reply=account(backend);CHECK(reply.activeAccount=="issued-client"&&reply.accounts.size()==1);
    CHECK(account(backend,AccountAction::selectModel,"account-model").selectedModel=="account-model");
    auto observer=std::make_unique<Job>();auto observed=std::make_unique<RuntimeJob>(observer->account());const auto observedReply=backend.run(*observed);observer->finish(observedReply);
    f->client="second-client";f->subject="second-user";reply=account(backend,AccountAction::addAccount);f->joinSockets();
    CHECK(f->authorizeClient=="dynamic_agent_client");CHECK(reply.providerStatus==Status::connected&&reply.activeAccount=="second-client"&&reply.accounts.size()==2);
    auto stale=std::make_unique<RuntimeJob>(observer->generate(request()));const auto beforeRequests=f->responses.load();const auto blocked=result(backend.run(*stale),*stale);
    CHECK(blocked.status==ai::GenerationStatus::providerFailure&&blocked.errorCode=="account_changed"&&blocked.inferenceCalls==0&&f->responses==beforeRequests);
    const auto before=f->browserCalls.load();reply=account(backend,AccountAction::selectAccount,"issued-client");
    CHECK(reply.providerStatus==Status::connected&&reply.activeAccount=="issued-client"&&f->browserCalls==before);
    f->now+=3600;reply=account(backend,AccountAction::selectAccount,"second-client");f->joinSockets();
    CHECK(f->authorizeClient=="second-client"&&f->browserCalls==before+1);CHECK(reply.providerStatus==Status::connected&&reply.activeAccount=="second-client");
    f->client="third-client";f->subject="third-user";f->browserMode=BrowserMode::deny;reply=account(backend,AccountAction::addAccount);f->joinSockets();
    CHECK(reply.providerStatus==Status::denied&&reply.activeAccount=="second-client"&&reply.accounts.size()==2);
    CHECK(account(backend).providerStatus==Status::connected);
    f->browserMode=BrowserMode::callback;f->tokenStatus=400;reply=account(backend,AccountAction::addAccount);f->joinSockets();
    CHECK(reply.providerStatus==Status::tokenFailure&&reply.httpStatus==400&&reply.errorCode=="invalid_grant");
    CHECK(reply.activeAccount=="second-client"&&reply.accounts.size()==3);
    const auto pending=std::find_if(reply.accounts.begin(),reply.accounts.end(),[](const auto& a){return a.id=="third-client";});
    CHECK(pending!=reply.accounts.end()&&pending->label.contains("finish sign-in"));
    const auto log=diagnostics::directory().getChildFile("companion.log").loadFileAsString();
    const auto event=juce::JSON::parse(log.trimEnd().fromLastOccurrenceOf("\n",false,false));
    CHECK(event["error"]==juce::var("invalid_grant")&&event["providerRequest"]==juce::var("123e4567-e89b-42d3-a456-426614174000"));
    CHECK(!log.contains("PRIVATE synthetic description"));
    f->tokenStatus=200;reply=account(backend,AccountAction::selectAccount,"third-client");f->joinSockets();
    CHECK(f->authorizeClient=="third-client");CHECK(reply.providerStatus==Status::connected&&reply.activeAccount=="third-client"&&reply.accounts.size()==3);
}
void native_signin_and_account(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__); // Bypassing actual callback/signature/refresh or truncating discovery breaks this group.
    auto f=fixture(key);CompanionBackend b(f->options());auto reply=account(b);CHECK(reply.providerStatus==Status::signedOut && reply.status==RuntimeStatus::ready);
    CHECK(account(b,AccountAction::models).providerStatus==Status::signedOut && f->calls==0);
    reply=account(b,AccountAction::signIn);f->joinSockets();CHECK(reply.status==RuntimeStatus::ready && reply.providerStatus==Status::connected && f->callbackOkay && f->browserCalls==1 && f->tokens==1);
    CHECK(account(b).providerStatus==Status::connected);reply=account(b,AccountAction::models);CHECK(reply.models.size()==2 && reply.models[0].slug=="account-model" && reply.models[1].slug=="second-model");
    CHECK(account(b,AccountAction::selectModel,"account-model").selectedModel=="account-model");
    f->now.fetch_add(3601);const int before=f->tokens;CHECK(account(b).providerStatus==Status::accessExpired && f->tokens==before);
    reply=account(b,AccountAction::models);CHECK(reply.providerStatus==Status::connected && reply.selectedModel=="account-model" && f->tokens==before+1);CHECK(account(b).providerStatus==Status::connected);
    f->now.fetch_add(3601);f->tokenStatus=400;reply=account(b,AccountAction::models);CHECK(reply.providerStatus==Status::tokenFailure && reply.errorCode=="invalid_grant" && reply.selectedModel=="account-model");CHECK(account(b).providerStatus==Status::signedOut && account(b).selectedModel=="account-model");
    for(const auto mode:{BrowserMode::deny,BrowserMode::wrongState,BrowserMode::badSignature,BrowserMode::fail,BrowserMode::throwError,BrowserMode::expired})
    {
        auto one=fixture(key);one->browserMode=mode;CompanionBackend native(one->options());const auto r=account(native,AccountAction::signIn);one->joinSockets();
        const auto expected=mode==BrowserMode::deny?Status::denied:mode==BrowserMode::wrongState?Status::invalidCallback:mode==BrowserMode::badSignature?Status::invalidIdentity:mode==BrowserMode::expired?Status::expiredAttempt:Status::networkFailure;
        CHECK(r.providerStatus==expected);CHECK(r.status==((mode==BrowserMode::fail || mode==BrowserMode::throwError)?RuntimeStatus::unavailable:RuntimeStatus::ready));
        if(mode==BrowserMode::fail || mode==BrowserMode::throwError)CHECK(r.errorCode=="browser_open_failed");CHECK(!r.errorCode.contains("PRIVATE") && r.selectedModel.isEmpty());
    }
    auto denied=fixture(key);denied->scope="openid";CompanionBackend restricted(denied->options());CHECK(account(restricted,AccountAction::signIn).providerStatus==Status::inferenceNotGranted);denied->joinSockets();CHECK(account(restricted).providerStatus==Status::inferenceNotGranted);CHECK(account(restricted,AccountAction::models).providerStatus==Status::inferenceNotGranted);
    f->tokenStatus=200;CHECK(f->seed());
    for(int shape=0;shape<2;++shape)
    {
        juce::Array<juce::var> models;const int count=shape==0?129:128;
        for(int i=0;i<count;++i){const auto slug=shape==0?String("model-")+String(i):String::repeatedString("a",240)+String(i);const auto name=shape==0?String("Model"):String::repeatedString(String::fromUTF8("\xf0\x9f\x8e\xb9"),256);models.add(object({{"slug",slug},{"display_name",name},{"visibility","list"}}));}
        f->models=json(object({{"models",models}}));reply=account(b,AccountAction::models);CHECK(reply.providerStatus==Status::invalidResponse && reply.errorCode=="model_list_limit" && reply.models.empty() && reply.selectedModel=="account-model");
    }
    f->models="{\"models\":[{\"slug\":\"bad slug\",\"display_name\":\"Model\",\"visibility\":\"list\"}]}";reply=account(b,AccountAction::models);CHECK(reply.providerStatus==Status::invalidResponse && reply.models.empty() && reply.selectedModel=="account-model");
    f->models="{\"models\":[{\"slug\":\"account-model\",\"display_name\":\"Model\",\"visibility\":\"list\"}]}";CHECK(account(b,AccountAction::models).models.size()==1);
}

void exact_job_cancellation(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__); // Resetting sticky cancellation, weak probe ownership or identity-only matching breaks this group.
    auto f=fixture(key);CompanionBackend b(f->options());Job paused;auto job=paused.account(AccountAction::signIn);Frame answer;std::atomic_bool done=false;
    airack::companion::detail::CompanionBackendTestAccess::pauseBeforeSignIn(true);
    std::thread worker([&]{answer=b.run(job);done=true;});const bool observed=observe([]{return airack::companion::detail::CompanionBackendTestAccess::signInPaused();});CHECK(observed);
    const int before=f->clockCalls;paused.cancel(b);airack::companion::detail::CompanionBackendTestAccess::pauseBeforeSignIn(false);CHECK(observe([&]{return done.load();},1500));worker.join();
    AccountReply reply;CHECK(decodeAccountReply(bytes(answer.payload),reply) && reply.status==RuntimeStatus::cancelled && reply.providerStatus==Status::cancelled);CHECK(f->clockCalls>before && f->browserCalls==0);CHECK(!paused.state.finishJob(job,answer));
    b.cancel(job); // Removed context cannot dangle or poison the next fresh job.
    CHECK(account(b,AccountAction::signIn).providerStatus==Status::connected);f->joinSockets();
    f->browserMode=BrowserMode::delayedCallback;Job loop;auto loopJob=loop.account(AccountAction::signIn);done=false;const int browserBefore=f->browserCalls;
    std::thread waiting([&]{answer=b.run(loopJob);done=true;});CHECK(observe([&]{return f->browserCalls>browserBefore;}));const auto cancelledAt=GetTickCount64();loop.cancel(b);CHECK(observe([&]{return done.load();},500));f->releaseLate=true;waiting.join();f->joinSockets();CHECK(GetTickCount64()-cancelledAt<1500);CHECK(decodeAccountReply(bytes(answer.payload),reply) && reply.status==RuntimeStatus::cancelled);
    f->browserMode=BrowserMode::callback;f->blockEndpoint="/models";f->blocked=false;f->release=false;Job next;auto nextJob=next.account(AccountAction::models);done=false;
    std::thread transport([&]{answer=b.run(nextJob);done=true;});const bool blocked=observe([&]{return f->blocked.load();});CHECK(blocked);
    auto wrong=nextJob;wrong.jobId++;b.cancel(wrong);wrong=nextJob;wrong.connectionId++;b.cancel(wrong);wrong=nextJob;wrong.session=juce::Uuid();b.cancel(wrong);Job stranger;wrong=nextJob;wrong.cancellation=stranger.account().cancellation;b.cancel(wrong);b.cancel(job);Sleep(50);CHECK(!done);
    f->release=true;CHECK(observe([&]{return done.load();},1500));transport.join();CHECK(decodeAccountReply(bytes(answer.payload),reply) && reply.status==RuntimeStatus::ready && reply.providerStatus==Status::connected);
    f->blocked=false;f->release=false;Job pending;auto pendingJob=pending.account(AccountAction::models);done=false;
    std::thread blockedTransport([&]{answer=b.run(pendingJob);done=true;});CHECK(observe([&]{return f->blocked.load();}));pending.cancel(b);f->release=true;CHECK(observe([&]{return done.load();},1500));blockedTransport.join();CHECK(decodeAccountReply(bytes(answer.payload),reply) && reply.status==RuntimeStatus::cancelled && reply.models.empty());CHECK(!pending.state.finishJob(pendingJob,answer));
    f->blockEndpoint={};CHECK(account(b).providerStatus==Status::connected);Job pre;auto preJob=pre.account();pre.cancel(b);answer=b.run(preJob);CHECK(decodeAccountReply(bytes(answer.payload),reply) && reply.status==RuntimeStatus::cancelled);CHECK(!pre.state.finishJob(preJob,answer));
}

// Keep large by-value fixture returns in short helper frames. The production
// worker is tested on the normal Windows stack, without this test's later cases.
RuntimeJob defaultGenerationJob(Job& job){return job.generate(request());}
void readResult(const Frame& frame,const RuntimeJob& job,ai::GenerationResult& out){out=result(frame,job);}
void full_catalog_generation(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__); // Toy preflight, silent model substitution, partial success or a second budget breaks this group.
    auto f=fixture(key);std::cout<<"TASK13_BOUNDARY 39 seed.before"<<std::endl;CHECK(f->seed());std::cout<<"TASK13_BOUNDARY 39 seed.after"<<std::endl;CompanionBackend b(f->options());std::cout<<"TASK13_BOUNDARY 39 model_account.before"<<std::endl;CHECK(account(b,AccountAction::selectModel,"account-model").selectedModel=="account-model");std::cout<<"TASK13_BOUNDARY 39 model_account.after"<<std::endl;
    auto jStorage=std::make_unique<Job>();auto& j=*jStorage;std::cout<<"TASK13_BOUNDARY 39 generation_capture.before"<<std::endl;auto task13Next1136_capturedStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(j)));auto& captured=*task13Next1136_capturedStorage;std::cout<<"TASK13_BOUNDARY 39 generation_capture.after"<<std::endl;const auto& r=std::get<ai::GenerationRequest>(captured.input);std::cout<<"TASK13_BOUNDARY 39 backend_run.before"<<std::endl;const auto frame=b.run(captured);std::cout<<"TASK13_BOUNDARY 39 backend_run.after"<<std::endl;auto generated=result(frame,captured);
    CHECK(generated.status==ai::GenerationStatus::ready && generated.inferenceCalls==1 && generated.candidate && generated.edits.parameters.size()==1);CHECK(generated.measurements.finite && std::isfinite(generated.measurements.rms) && generated.measurements.clippedSamples==0);CHECK(!generated.audio.left.empty() && generated.audio.left.size()==generated.audio.right.size());CHECK(generated.audio.context.phrase==*r.capturedPhrase && generated.audio.context.bpm==400);
    CHECK(f->responses==1);juce::var sent,prompt;bool found=false;
    for(const auto& req:f->requests)if(req.url.endsWith("/responses")){found=true;CHECK(req.method=="POST" && req.contentType=="application/json" && req.timeoutMs==60000);CHECK(ai::parseStrictJson(req.body.toStdString(),sent,2*1024*1024));CHECK(sent["model"].toString()=="account-model" && bool(sent["stream"]) && !bool(sent["store"]));CHECK(sent["text"]["format"]["type"].toString()=="json_schema" && bool(sent["text"]["format"]["strict"]));CHECK(json(sent["text"]["format"]["schema"])==json(ai::proposalSchema()));const auto* input=sent["input"].getArray();CHECK(input && input->size()==1);if(input && input->size()==1)CHECK(ai::parseStrictJson((*input)[0]["content"].toString().toStdString(),prompt,1024*1024));}
    // The rows are the source's reach, ascending.
    const auto reach=userReach(r.source.sound);std::vector<std::size_t> reached;for(std::size_t i=0;i<parameterCount;++i)if(reach[i])reached.push_back(i);
    CHECK(found);const auto* controls=prompt["catalog"]["controls"].getArray();CHECK(controls && controls->size()==int(reached.size()));if(controls && controls->size()==int(reached.size()))for(int n=0;n<int(reached.size());++n){const auto i=reached[std::size_t(n)];CHECK((*controls)[n][0].toString()==parameterRegistry()[i].id);CHECK(float(double((*controls)[n][8]))==r.source.sound.values[i]);}
    CHECK(prompt["context"]["role"].toString()=="lead" && prompt["context"]["key"].toString()=="D" && prompt["context"]["scale"].toString()=="minor" && double(prompt["context"]["bpm"])==400);CHECK(!prompt.hasProperty("runtime") && !prompt.hasProperty("credentials"));
    f->stream=completed("{}");const int callsBefore=f->responses;auto repairStorage=std::make_unique<Job>();auto& repair=*repairStorage;auto task13Next1136_repairJobStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(repair)));auto& repairJob=*task13Next1136_repairJobStorage;readResult(b.run(repairJob),repairJob,generated);withheld(generated,ai::GenerationStatus::budgetExhausted,3);CHECK(f->responses==callsBefore+3);
    for(const auto& stream:std::vector<String>{event("response.refusal.delta",object({{"delta","PRIVATE refusal"}})),event("response.incomplete"),"data: {PRIVATE broken}\n\n"})
    {f->stream=stream;auto failStorage=std::make_unique<Job>();auto& fail=*failStorage;auto task13Next1136_failJobStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(fail)));auto& failJob=*task13Next1136_failJobStorage;readResult(b.run(failJob),failJob,generated);withheld(generated,ai::GenerationStatus::providerFailure,1);CHECK(generated.providerStatus==Status::invalidResponse && !generated.errorCode.contains("PRIVATE"));}
    f->responseStatus=503;f->stream="{\"error\":{\"code\":\"server_error\",\"message\":\"PRIVATE\"}}";auto networkStorage=std::make_unique<Job>();auto& network=*networkStorage;auto task13Next1136_networkJobStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(network)));auto& networkJob=*task13Next1136_networkJobStorage;readResult(b.run(networkJob),networkJob,generated);withheld(generated,ai::GenerationStatus::providerFailure,1);CHECK(generated.providerStatus==Status::networkFailure && generated.httpStatus==503 && generated.errorCode=="server_error");
    f->responseStatus=200;f->stream=completed();f->blockEndpoint="/responses";f->blocked=false;f->release=false;auto cancelledStorage=std::make_unique<Job>();auto& cancelled=*cancelledStorage;auto task13Next1136_cancelJobStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(cancelled)));auto& cancelJob=*task13Next1136_cancelJobStorage;Frame cancellation;std::atomic_bool done=false;std::thread worker([&]{cancellation=b.run(cancelJob);done=true;});CHECK(observe([&]{return f->blocked.load();}));cancelled.cancel(b);f->release=true;CHECK(observe([&]{return done.load();},1500));worker.join();readResult(cancellation,cancelJob,generated);withheld(generated,ai::GenerationStatus::cancelled,1);CHECK(!cancelled.state.finishJob(cancelJob,cancellation));
    f->blockEndpoint={};CHECK(account(b).selectedModel=="account-model" && !account(b).modelVerified);
    auto signedOut=fixture(key);CHECK(preference(signedOut->temp.preferences(),object({{"version",1},{"selectedModel","account-model"}})));CompanionBackend absent(signedOut->options());auto noAuthStorage=std::make_unique<Job>();auto& noAuth=*noAuthStorage;auto task13Next1136_absentJobStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(noAuth)));auto& absentJob=*task13Next1136_absentJobStorage;readResult(absent.run(absentJob),absentJob,generated);withheld(generated,ai::GenerationStatus::providerFailure,1);CHECK(generated.providerStatus==Status::signedOut && signedOut->responses==0 && signedOut->browserCalls==0);
    auto badOptions=f->options();badOptions.preferenceDirectory={};CompanionBackend badStorage(badOptions);auto storageStorage=std::make_unique<Job>();auto& storage=*storageStorage;auto task13Next1136_storageJobStorage=std::unique_ptr<RuntimeJob>(new RuntimeJob(defaultGenerationJob(storage)));auto& storageJob=*task13Next1136_storageJobStorage;readResult(badStorage.run(storageJob),storageJob,generated);withheld(generated,ai::GenerationStatus::providerFailure,0);CHECK(generated.providerStatus==Status::storageFailure);
}
void catalog_materialize_uses_original_inference(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__);auto f=fixture(key);CHECK(f->seed());CompanionBackend b(f->options());
    CHECK(account(b,AccountAction::selectModel,"account-model").selectedModel=="account-model");auto jobStorage=std::make_unique<Job>();auto& job=*jobStorage;
    auto rStorage=airack::testing::fixtureValue([&]{return request();});auto& r=*rStorage;r.tag.runtime=job.runtime;RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(airack::test::frozenFactoryRackDesigns(),{},{},snapshot));
    RackCatalogPage page;CHECK(catalogPage(snapshot,{},0,65536,page));CHECK(page.complete);auto authority=std::make_shared<ai::CatalogAuthority>();
    authority->root=r.tag;authority->snapshot=snapshot.id;authority->descriptorJson=juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).toStdString();
    CHECK(ai::detail::catalogAuthorityValid(*authority));
    r.catalog.emplace();r.catalog->root=r.tag;r.catalog->authority=authority;const auto factory=airack::test::frozenFactoryRackDesigns().front();
    juce::MemoryBlock canonical;CHECK(encodeRackDesign(factory,canonical,RackValidationMode::instanceRecall));const auto hash=juce::SHA256(canonical).toHexString();
    auto wire=juce::JSON::parse(proposal());wire.getDynamicObject()->setProperty("version",2);wire.getDynamicObject()->setProperty("name","Backend Test Sound");
    wire.getDynamicObject()->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>{}},{"remove",juce::Array<juce::var>{}},{"replace",juce::Array<juce::var>{}},{"controls",juce::Array<juce::var>{}}}));
    wire.getDynamicObject()->setProperty("catalog_selections",juce::Array<juce::var>{object({{"local_id",5},{"design",factory.id.toDashedString()},
        {"revision",juce::String(factory.revision)},{"content_sha256",hash},{"destination",object({{"role","synth"},{"chain",4},{"position",0}})}})});
    wire.getDynamicObject()->setProperty("catalog_parameter_edits",juce::Array<juce::var>{});f->stream=completed(json(wire));
    auto firstStorage=airack::testing::fixtureValue([&]{return job.generate(r);});auto& first=*firstStorage;auto reply=b.run(first);auto selectedStorage=airack::testing::fixtureValue([&]{return result(reply,first);});auto& selected=*selectedStorage;
    CHECK(selected.status==ai::GenerationStatus::catalogSelectionRequired && selected.inferenceCalls==1 && selected.edits.catalogSelections.size()==1);
    CHECK(!selected.candidate && selected.audio.left.empty() && selected.audio.right.empty());
    CHECK(selected.catalog==ai::CatalogResultBinding(r.tag,snapshot.id));CHECK(f->responses==1);
    CHECK(job.state.finishJob(first,reply));Frame delivered;CHECK(job.state.takeReply(job.id,delivered));job.state.finishReply(job.id,true);
    auto nextStorage=std::make_unique<ai::GenerationRequest>(r);auto& next=*nextStorage;next.tag.requestId=2;auto& context=*next.catalog;context.phase=ai::CatalogPhase::materialize;context.selectionParent=selected.tag;
    context.baseIntent=selected.edits;context.inferenceCallsUsed=std::uint8_t(selected.inferenceCalls);context.providerStatus=selected.providerStatus;context.httpStatus=selected.httpStatus;
    auto hydrated=std::make_shared<ai::CatalogAuthority>(*authority);hydrated->definitions.push_back({factory.id,factory.revision,hash,canonical});context.authority=hydrated;
    auto secondStorage=airack::testing::fixtureValue([&]{return job.generate(next);});auto& second=*secondStorage;reply=b.run(second);auto readyStorage=airack::testing::fixtureValue([&]{return result(reply,second);});auto& ready=*readyStorage;
    CHECK(ready.status==ai::GenerationStatus::ready && ready.inferenceCalls==1 && ready.candidate && !ready.audio.left.empty());
    CHECK(ready.catalog==selected.catalog && ready.edits.catalogSelections==selected.edits.catalogSelections);CHECK(f->responses==1);
    CHECK(ready.providerStatus==selected.providerStatus && ready.httpStatus==selected.httpStatus);
    CHECK(job.state.finishJob(second,reply));CHECK(job.state.takeReply(job.id,delivered));job.state.finishReply(job.id,true);
}

struct Client
{
    PipeConnection pipe;juce::Uuid runtime,session=juce::Uuid::null();bool reading=false;
    bool send(const Frame& f,std::uint64_t ms=1500){if(pipe.beginWrite(f)!=PipeStatus::ready)return false;PipeStatus s=PipeStatus::pending;observe([&]{s=pipe.pollWrite();return s!=PipeStatus::pending;},ms);return s==PipeStatus::complete;}
    std::optional<Frame> receive(std::uint64_t ms=3000){if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return {};reading=true;}Frame f;PipeStatus s=PipeStatus::pending;observe([&]{s=pipe.pollRead(f);return s!=PipeStatus::pending;},ms);if(s!=PipeStatus::pending)reading=false;if(s==PipeStatus::complete)return f;return {};}
    bool open(const String& name){if(!observe([&]{return pipe.connect(name)==PipeStatus::ready;}))return false;if(!send(control(MessageKind::hello,juce::Uuid::null(),{runtime,0,0,0})))return false;const auto f=receive();ControlMessage m;if(!f || f->kind!=MessageKind::welcome || !decodeControl(bytes(f->payload),m) || m.status!=RuntimeStatus::ready || m.tag.runtime!=runtime)return false;session=f->session;return !session.isNull();}
    bool ping(){const auto at=GetTickCount64();if(!send(control(MessageKind::ping,session,{runtime,0,0,0})))return false;const auto f=receive(1500);ControlMessage m;return f && f->kind==MessageKind::pong && f->session==session && decodeControl(bytes(f->payload),m) && m.status==RuntimeStatus::ready && m.tag==ai::RequestTag{runtime,0,0,0} && GetTickCount64()-at<1500;}
    bool account(AccountAction action,const String& model={}){Frame f{MessageKind::account,session,{}};return encodeAccountCommand({action,model},f.payload) && send(f);}
    bool generate(ai::GenerationRequest& r,std::uint64_t id){r.tag.runtime=runtime;r.tag.requestId=id;Frame f{MessageKind::generate,session,{}};return encodeRequest(r,f.payload) && send(f);}
};
void real_server_backend_integration(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__); // Blocking IO with account/provider work or weak callback captures breaks this group.
    auto f=fixture(key);f->checkPriority=true;auto backend=std::make_shared<CompanionBackend>(f->options());std::weak_ptr<CompanionBackend> weak=backend;
    std::atomic_bool stop=false,done=false;std::atomic_int runCalls=0,runActive=0,runPeak=0,cancels=0;String name=juce::Uuid().toString().removeCharacters("-");RuntimeServerResult serverResult;
    auto server=std::make_unique<RuntimeServer>([backend,&runCalls,&runActive,&runPeak](const auto& job){++runCalls;const int n=++runActive;runPeak.store((std::max)(runPeak.load(),n));struct Done{std::atomic_int& v;~Done(){--v;}}done{runActive};return backend->run(job);},[backend,&cancels](const auto& job){++cancels;backend->cancel(job);});
    backend.reset();CHECK(!weak.expired());std::thread io([&]{serverResult=server->run(stop,name);done=true;});
    Client a,b;const bool opened=a.open(name) && b.open(name);CHECK(opened);
    if(opened)
    {
        CHECK(a.account(AccountAction::signIn));auto signIn=a.receive();AccountReply reply;CHECK(signIn && signIn->kind==MessageKind::accountResult && signIn->session==a.session && decodeAccountReply(bytes(signIn->payload),reply) && reply.providerStatus==Status::connected);f->joinSockets();
        CHECK(a.account(AccountAction::selectModel,"account-model"));auto selected=a.receive();CHECK(selected && decodeAccountReply(bytes(selected->payload),reply) && reply.selectedModel=="account-model");
        f->blockEndpoint="/responses";f->release=false;f->blocked=false;auto old=request();CHECK(a.generate(old,1));CHECK(observe([&]{return f->blocked.load();}));CHECK(!weak.expired());CHECK(b.ping());CHECK(b.account(AccountAction::models));CHECK(runActive==1);
        auto replacement=request();CHECK(a.generate(replacement,2));CHECK(observe([&]{return cancels.load()>0;}));f->release=true;
        auto current=a.receive(15000);ai::GenerationResult result;CHECK(current && current->kind==MessageKind::result && current->session==a.session && decodeResult(bytes(current->payload),replacement,result) && result.status==ai::GenerationStatus::ready && result.tag==replacement.tag);auto other=b.receive();CHECK(other && other->kind==MessageKind::accountResult && other->session==b.session && decodeAccountReply(bytes(other->payload),reply) && reply.models.size()==2);CHECK(!a.receive(100));
        f->blockEndpoint="/models";f->release=false;f->blocked=false;const int browserBefore=f->browserCalls;CHECK(a.account(AccountAction::models));CHECK(observe([&]{return f->blocked.load();}));CHECK(b.ping());const auto oldSession=a.session;a.pipe.close();a.reading=false;CHECK(observe([&]{return cancels.load()>1;}));f->release=true;CHECK(a.open(name) && a.session!=oldSession);CHECK(a.account(AccountAction::status));auto reconnect=a.receive();CHECK(reconnect && reconnect->session==a.session && decodeAccountReply(bytes(reconnect->payload),reply) && reply.providerStatus==Status::connected);CHECK(f->browserCalls==browserBefore);
        f->blockEndpoint={};f->browserMode=BrowserMode::delayedCallback;const int before=f->browserCalls;CHECK(a.account(AccountAction::signIn));CHECK(observe([&]{return f->browserCalls>before;}));CHECK(b.ping());a.pipe.close();CHECK(observe([&]{return cancels.load()>2;}));f->joinSockets();CHECK(b.account(AccountAction::status));auto continuing=b.receive();CHECK(continuing && continuing->session==b.session && decodeAccountReply(bytes(continuing->payload),reply) && reply.providerStatus==Status::connected);CHECK(b.ping());
    }
    // All owned transports have a 4s bound; every branch releases before worker drain.
    f->release=true;airack::companion::detail::CompanionBackendTestAccess::pauseBeforeSignIn(false);stop=true;CHECK(observe([&]{return done.load();},15000));io.join();CHECK(serverResult.status==RuntimeStatus::ready && !serverResult.workerRetained);CHECK(runPeak==1 && f->peak<=1 && f->priorityGood);CHECK(weak.expired());server.reset();CHECK(weak.expired());f->joinSockets();airack::companion::detail::PipeTestAccess::reapRetired();CHECK(airack::companion::detail::PipeTestAccess::retainedOwners()==0);
}
struct LocalBackendFixture
{
    std::unique_ptr<Job> owner=std::make_unique<Job>(ownedLocalPreviewCapability);
    ai::LocalGenerationRequest input;
    RuntimeJob job;
    drums::AssetPtr asset;
    explicit LocalBackendFixture(bool linked=false,const std::function<bool(ai::LocalGenerationRequest&,drums::AssetPtr&)>& configure={},bool allPeersOwned=false)
    {
        input.request=request();input.request.tag.runtime=owner->runtime;input.request.source.sound=newInstanceSound();
        input.request.capturedPhrase->notes[0].pitch=36;input.request.capturedPhrase->notes[0].channel=1;
        RackOperation op;op.kind=RackOperationKind::place;op.newInstance=juce::Uuid();op.design=airack::test::frozenTileArrayRackDesign();SoundDocument sound;RackDifference diff;
        CHECK(makeRackCandidate(input.request.source.sound,op,SoundEditOrigin::manual,sound,diff)==RackCandidateStatus::ready);input.request.source.sound=std::move(sound);
        for(const auto& instance:input.request.source.sound.personalities->instances)for(const auto& unit:instance.design.units)
            if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(instance,unit.id))input.local.sourceMap.banks[binding->slot]={instance.id,unit.id,1};
        std::vector<float> pcm(4800);for(std::size_t i=0;i<pcm.size();++i)pcm[i]=float(.1*std::exp(-double(i)/700)*std::sin(double(i)*.1));drums::AssetError error;
        CHECK(drums::makeSampleAsset(pcm,1,asset,error));if(!asset)throw std::runtime_error("local sample fixture");
        input.local.sourceMap.assets.push_back({asset->id(),std::uint8_t(asset->channels()),asset->frames()});
        input.local.sourceMap.pads[0].asset=1;input.local.sourceMap.pads[0].name="Visible \\\"Snare\\\"";input.local.sourceMap.pads[0].metadata.fill(0xde);
        if(configure&&!configure(input,asset))return;
        input.local.root=input.request.tag;input.local.reset=29;CHECK(ai::digestPreviewMap(input.local.sourceMap,input.local.sourceMapDigest));CHECK(ai::validLocalPreviewContext(input));
        GroupSelection selection;
        if(linked){
            const auto ensemble=juce::Uuid();GroupIntent intent;intent.group=ensemble;intent.member=juce::Uuid();intent.bpm=400;
            CHECK(setGroupText("Drum metadata session",intent.name));CHECK(setGroupText("lead",intent.role));CHECK(setGroupText("D",intent.key));CHECK(setGroupText("minor",intent.scale));
            if(configure){CHECK(setGroupText(std::string(128,'\\'),intent.name));CHECK(setGroupText(input.request.role,intent.role));
                CHECK(setGroupText(input.request.key,intent.key));CHECK(setGroupText(input.request.scale,intent.scale));}
            input.request.source.group=intent;
            const auto publish=[&](RuntimeConnectionId id,juce::Uuid session,juce::Uuid runtime,ProjectDocument& project,const ai::PreviewSampleMap* map){
                auto command=std::make_unique<GroupCommand>();command->operationId=1;command->action=GroupAction::admit;command->runtime=runtime;command->loadRevision=1;
                command->publication.emplace();auto& publication=*command->publication;publication.runtime=runtime;publication.loadRevision=1;publication.editRevision=7;publication.project=project;
                Frame frame{map?MessageKind::groupCommandLocal:MessageKind::groupCommand,session,{}};
                CHECK(map?encodeLocalGroupPublication(*command,*map,frame.payload):encodeGroupCommand(*command,frame.payload));
                CHECK(owner->state.receive(id,frame,std::chrono::steady_clock::now())==RuntimeStatus::ready);CHECK(owner->state.takeReply(id,frame));GroupReply reply;
                CHECK(decodeGroupReply(bytes(frame.payload),reply)&&reply.status==GroupReplyStatus::ready&&reply.normalized);
                if(reply.normalized)project.group=reply.normalized;owner->state.finishReply(id,true);
            };
            publish(owner->id,owner->session,owner->runtime,input.request.source,&input.local.sourceMap);
            selection.loadRevision=1;selection.peerCount=4;
            for(unsigned i=0;i<4;++i){const auto runtime=juce::Uuid();const auto now=std::chrono::steady_clock::now();const auto id=owner->state.open({GetCurrentProcessId(),incarnation()},now);
                auto hello=control(MessageKind::hello,juce::Uuid::null(),{runtime,0,0,0});CHECK(encodeControl({{runtime,0,0,0},RuntimeStatus::ready,ownedLocalPreviewCapability},hello.payload));
                CHECK(owner->state.receive(id,hello,now)==RuntimeStatus::ready);Frame welcome;CHECK(owner->state.takeReply(id,welcome));owner->state.finishReply(id,true);
                auto peer=std::make_unique<ProjectDocument>(input.request.source);peer->group->member=juce::Uuid();CHECK(setGroupText("drums",peer->group->role));
                auto map=input.local.sourceMap;
                if(configure){auto role=std::string(128,'"');role[i]='\\';CHECK(setGroupText(role,peer->group->role));}
                else map.pads[0].name="Peer "+std::to_string(i+1);
                publish(id,welcome.session,runtime,*peer,i==3&&!allPeersOwned?nullptr:&map);selection.members[i]=peer->group->member;
            }
        }
        Frame frame{linked?MessageKind::generateGroupLocal:MessageKind::generateLocal,owner->session,{}};
        if(linked){auto command=std::make_unique<ai::LocalGroupGenerationCommand>();command->input=input;command->selection=selection;CHECK(encodeLocalGroupGeneration(*command,frame.payload));}
        else CHECK(encodeLocalRequest(input,frame.payload));
        const auto admitted=owner->state.receive(owner->id,frame,std::chrono::steady_clock::now());CHECK(admitted==RuntimeStatus::ready);
        if(admitted!=RuntimeStatus::ready||!owner->state.takeJob(job))throw std::runtime_error("local backend runtime admission");owner->value=job;
        CHECK(job.localPreview&&job.previewExchange);
    }
};
// Additional owned-local cases; historical melodic61/67/25 cases are untouched.
// The fixture's actual OAuth transport sees only already serialized requests.
bool maximumOwnedBackendInput(ai::LocalGenerationRequest& input,drums::AssetPtr& first,
    std::vector<drums::AssetPtr>& owners,std::vector<String>& hidden,bool hydrated,bool printable,bool simpleFx)
{
    using namespace ai;auto& r=input.request;auto initial=testing::fixtureValue([]{return newInstanceSound();});r.source.sound=std::move(*initial);
    r.instruction=std::string(8192,'"');r.role=std::string(128,'\\');r.key=std::string(128,'"');r.scale=std::string(128,'\\');r.capturedBpm=400;
    r.source.sound.name=String::repeatedString("\\",maximumSoundNameBytes);r.structuralGeneration=UINT64_MAX;
    r.namesInUse.assign(maximumNamesInUse,String::repeatedString("\\",maximumSoundNameBytes));r.policy.allowSlotReuse=true;
    const auto recipe=[&](NativeModule module,unsigned ordinal){
        RackDesign d;d.role=isSourceModule(module)?RackRole::synth:RackRole::fx;d.revision=UINT64_MAX;d.units={{0,module,{}}};
        std::string name(64,printable?'\\':'\x01'),intent(512,printable?'\\':'\x01');for(unsigned bit=0;bit<6;++bit)if(ordinal&(1u<<bit)){name[bit]=printable?'"':'\x02';intent[bit]=printable?'"':'\x02';}
        d.name=String::fromUTF8(name.data(),int(name.size()));d.intent=String::fromUTF8(intent.data(),int(intent.size()));d.panel.sections={d.name,d.name,d.name,d.name};
        const auto prefix=String(isSourceModule(module)?"synth.1.":"effect.1.");
        for(const auto& parameter:parameterRegistry())if(parameter.id.startsWith(prefix)){
            const auto key=parameter.id.substring(prefix.length());const auto at=rackParameterIndex(module,0,key);if(!at||!rackParameterActive(module,key))continue;
            d.units[0].defaults.push_back({0,key,defaultParameterValues()[*at]});
            if(d.panel.controls.size()<8&&parameter.minimum<parameter.maximum)d.panel.controls.push_back({d.name,0,std::uint8_t(d.panel.controls.size()%4),key,
                parameter.kind==ParameterKind::continuous?FocusWidget::rotary:parameter.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice,FocusResponse::linear,parameter.minimum,parameter.maximum});}
        CHECK(validateRackDesign(d,RackValidationMode::portableDesign));return d;
    };
    std::vector<RackDesign> embedded;
    for(std::uint8_t n=0;n<12;++n){RackOperation op;op.kind=RackOperationKind::place;op.allowSlotReuse=true;op.newInstance=juce::Uuid();
        op.design=recipe(n<4?NativeModule::drum:simpleFx?NativeModule::drive:NativeModule::dynamicEq,n);op.allocation={{0,std::uint8_t(n<4?n:n-4)}};
        op.destination={n<4?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(n<4?n:n-4)};
        auto candidate=std::make_unique<SoundDocument>();RackDifference difference;const auto placed=makeRackCandidate(r.source.sound,op,SoundEditOrigin::manual,*candidate,difference);
        CHECK(placed==RackCandidateStatus::ready);if(placed!=RackCandidateStatus::ready)return false;r.source.sound=std::move(*candidate);embedded.push_back(*op.design);}
    r.source.pattern.bars=8;r.source.pattern.noteCount=256;
    for(std::size_t n=0;n<256;++n)r.source.pattern.notes[n]={double(n)/8,.03125,std::uint8_t(n%128),std::uint8_t(1+n%127),std::uint8_t(1+n%16)};
    r.capturedPhrase=r.source.pattern;CHECK(validateProjectDocument(r.source)&&r.source.sound.personalities->instances.size()==12);
    auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
    std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
    CHECK(factory.size()==26&&std::count(factory.begin(),factory.end(),tile)==1
        &&std::count_if(factory.begin(),factory.end(),[&](const auto& d){return d.id==tile.id;})==1);
    for(unsigned n=12;n<41;++n){auto extra=recipe(simpleFx?NativeModule::drive:NativeModule::drum,n);
        // The positive transport case uses historical26 builtins plus41 externals. The hydrated stress
        // also adds the largest known portable9-unit shape, without padding JSON.
        if(hydrated){auto fx=recipe(NativeModule::delay,n);for(std::uint8_t unit=1;unit<=8;++unit){auto add=fx.units[0];add.id=unit;for(auto& value:add.defaults)value.unit=unit;extra.units.push_back(std::move(add));}}
        CHECK(validateRackDesign(extra,RackValidationMode::portableDesign));embedded.push_back(std::move(extra));}
    RackCatalogSnapshot snapshot;const bool captured=captureRackCatalog(factory,embedded,{},snapshot);CHECK(captured&&snapshot.entries.size()==67);if(!captured)return false;
    juce::var inventory;CHECK(catalogInventoryFor(snapshot,CatalogRendering::ownedLocal,inventory));CHECK(inventory["entries"].size()==67&&bool(inventory["complete"]));
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=snapshot.id;authority->rendering=CatalogRendering::ownedLocal;
    authority->descriptorJson=juce::JSON::toString(inventory,true,22).toStdString();r.catalog.emplace();r.catalog->root=r.tag;r.catalog->authority=authority;
    if(hydrated){
        for(const auto& instance:r.source.sound.personalities->instances)r.catalog->baseIntent.reconciliation.remove.push_back(instance.id);
        for(std::size_t n=0;n<12;++n){const auto& design=embedded[n];const auto found=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[&](const auto& entry){return entry.definition.id==design.id;});
            CHECK(found!=snapshot.entries.end());if(found==snapshot.entries.end())return false;CatalogDefinition definition{design.id,design.revision,found->contentSha256,{}};
            CHECK(encodeRackDesign(design,definition.canonical,RackValidationMode::instanceRecall));CHECK(definition.canonical.getSize()<=65536);authority->definitions.push_back(std::move(definition));
            r.catalog->baseIntent.catalogSelections.push_back({std::uint8_t(n),design.id,design.revision,found->contentSha256,{n<4?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(n<4?n:n-4)}});
            for(const auto& value:design.units[0].defaults)r.catalog->baseIntent.catalogParameters.push_back({std::uint8_t(n),value.unit,value.key,value.normalized});}
        auto admitted=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
        CHECK(authority->definitions.size()==12&&r.catalog->baseIntent.catalogSelections.size()==12);
        CHECK(admitProposal(r.source,r.policy,r.catalog->baseIntent,*authority,*admitted,trace.get())==ProposalStatus::accepted);
    }
    if(owners.empty()){
        std::vector<float> pcm(480000*2);drums::AssetError error;
        for(unsigned n=0;n<64;++n){for(std::size_t frame=0;frame<480000;++frame){pcm[frame*2]=float(n+1)/1024.f;pcm[frame*2+1]=-float(n+1)/2048.f;}
            drums::AssetPtr asset;CHECK(drums::makeSampleAsset(pcm,2,asset,error));if(!asset)return false;owners.push_back(std::move(asset));}
        hidden.push_back(juce::Base64::toBase64(pcm.data(),64));}
    input.local={};input.local.root=r.tag;input.local.catalogSnapshot=snapshot.id;input.local.reset=29;input.local.sourceWord=r.structuralGeneration;
    for(const auto& instance:r.source.sound.personalities->instances)for(const auto& unit:instance.design.units)if(isSourceModule(unit.module)){
        const auto* binding=rackUnitBinding(instance,unit.id);CHECK(binding);if(binding)input.local.sourceMap.banks[binding->slot]={instance.id,unit.id,UINT64_MAX};}
    for(const auto& asset:owners)input.local.sourceMap.assets.push_back({asset->id(),std::uint8_t(asset->channels()),asset->frames()});
    std::sort(input.local.sourceMap.assets.begin(),input.local.sourceMap.assets.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    for(unsigned n=0;n<64;++n){auto& pad=input.local.sourceMap.pads[n];pad.name=std::string(64,'\\');for(unsigned bit=0;bit<6;++bit)pad.name[bit]=(n&(1u<<bit))?'"':'\\';
        const auto id=owners[n]->id();const auto found=std::find_if(input.local.sourceMap.assets.begin(),input.local.sourceMap.assets.end(),[&](const auto& ref){return ref.id==id;});pad.asset=std::uint8_t(found-input.local.sourceMap.assets.begin()+1);
        drums::DrumPad metadata;metadata.asset=id;metadata.name=pad.name;drums::SourceProvenance provenance;provenance.kind=drums::SourceKind::generated;provenance.sourceChannels=2;provenance.sourceFrames=480000;provenance.end=480000;
        provenance.generator="PRIVATE_GENERATOR_DO_NOT_UPLOAD";provenance.condition="PRIVATE_GENERATOR_PROMPT_DO_NOT_UPLOAD";provenance.seed=600000000000000000ull+n;
        provenance.sourceFileId[31]=std::uint8_t(n+1);provenance.promptId[30]=std::uint8_t(n+1);metadata.source=provenance;CHECK(digestPreviewPad(metadata,pad.metadata));
        hidden.push_back(drums::assetIdText(id));hidden.push_back(drums::assetIdText(pad.metadata));hidden.push_back(String(std::to_string(provenance.seed)));}
    hidden.push_back("PRIVATE_GENERATOR_DO_NOT_UPLOAD");hidden.push_back("PRIVATE_GENERATOR_PROMPT_DO_NOT_UPLOAD");hidden.push_back("C:/PRIVATE_SOURCE_PATH_DO_NOT_UPLOAD/hit.wav");first=owners.front();
    CHECK(digestPreviewMap(input.local.sourceMap,input.local.sourceMapDigest));CHECK(validLocalPreviewContext(input));CHECK(ai::detail::catalogAuthorityValid(*authority)&&ai::detail::catalogRequestValid(r));
    juce::MemoryBlock encoded;const bool fits=encodeLocalRequest(input,encoded);
    std::cout<<"OWNED_PROVIDER_SHAPE hydrated="<<hydrated<<" printable="<<printable<<" simple_fx="<<simpleFx<<" catalog=67 factory=26 source_instances=12 assets="<<owners.size()<<" descriptor_bytes="<<authority->descriptorJson.size()<<" definitions="<<authority->definitions.size()<<" selections="<<r.catalog->baseIntent.catalogSelections.size()<<" physical_parameters="<<r.catalog->baseIntent.catalogParameters.size()<<" raw_wire_accepted="<<fits<<" raw_wire_bytes="<<encoded.getSize()<<std::endl;
    if(!fits){CHECK(hydrated);return false;} // Explicit unsent wire refusal; not provider coverage.
    CHECK(encoded.getSize()<=maximumGenerationRequestBytes);return true;
}
void checkMaximumSampleProjection(const juce::var& projected,const ai::PreviewSampleMap* expected)
{
    if(!expected){CHECK(projected.isObject()&&projected.getDynamicObject()->getProperties().size()==1&&projected["availability"].toString()=="unavailable");return;}
    CHECK(projected.isObject()&&projected.getDynamicObject()->getProperties().size()==3&&projected["availability"].toString()=="available");
    CHECK(json(projected["pad_fields"])==json(juce::var(juce::Array<juce::var>{"state","displayed_name","channels","frames","duration_seconds"})));CHECK(projected["banks"].size()==4);
    for(unsigned bank=0;bank<4;++bank){CHECK(projected["banks"][int(bank)].size()==16);for(unsigned pad=0;pad<16;++pad){const auto row=projected["banks"][int(bank)][int(pad)];const auto& ref=expected->pads[bank*16+pad];
        CHECK(row.isArray()&&row.size()==5&&row[0].toString()=="assigned"&&row[1].toString().toStdString()==ref.name&&ref.name.size()==64);
        CHECK(int(row[2])==2&&int(row[3])==480000&&double(row[4])==10.);}}
}
void owned_provider_maximum_and_privacy(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__);auto f=fixture(key);CHECK(f->seed());CompanionBackend backend(f->options());CHECK(account(backend,AccountAction::selectModel,"account-model").selectedModel=="account-model");
    // Legitimate provider rejection avoids fake local measurements/render success.
    f->responseStatus=413;f->stream="{\"error\":{\"code\":\"context_length_exceeded\"}}";
    std::vector<drums::AssetPtr> owners;std::vector<String> hidden;unsigned serialized=0;
    // Retain all five extreme DynamicEQ cases. Drive gives a separate full-cardinality
    // positive combination; it makes no claim to maximum simultaneous parameters.
    for(unsigned mode=0;mode<7;++mode){const bool hydrated=mode==4,simpleFx=mode>=5,printable=mode==2||mode==3||simpleFx,allPeers=mode!=1&&mode!=3&&mode!=6;bool wireReady=false;
        auto local=std::make_unique<LocalBackendFixture>(true,[&](auto& input,auto& first){wireReady=maximumOwnedBackendInput(input,first,owners,hidden,hydrated,printable,simpleFx);return wireReady;},allPeers);
        if(!wireReady){CHECK(hydrated&&!local->job.localPreview);std::cout<<"OWNED_PROVIDER_OUTCOME wire_refused_no_provider_coverage=1"<<std::endl;continue;}
        const bool captured=local->job.groupContext&&local->job.localGroupContext&&local->job.localGroupContext->peerCount==4;CHECK(captured);if(!captured)return;
        const auto before=f->responses.load();const auto frame=backend.run(local->job);auto result=std::make_unique<ai::LocalGenerationResult>();
        const bool decoded=frame.kind==MessageKind::resultLocal&&decodeLocalResult(bytes(frame.payload),local->input,*result);CHECK(decoded);if(!decoded)return;CHECK(!result->rendered&&result->numeric.audio.left.empty()&&result->numeric.audio.right.empty());
        const auto sent=f->responses.load()-before;
        if(sent==0){CHECK(!simpleFx);CHECK(result->numeric.errorCode=="generation_prompt_limit"||result->numeric.errorCode=="structured_request_limit");
            CHECK(local->owner->state.finishJob(local->job,frame));Frame delivered;CHECK(local->owner->state.takeReply(local->owner->id,delivered));local->owner->state.finishReply(local->owner->id,true);
            std::cout<<"OWNED_PROVIDER_OUTCOME hydrated="<<hydrated<<" printable="<<printable<<" error="<<result->numeric.errorCode<<" pretransport_refused=1 serialized_body_unobserved=1"<<std::endl;continue;}
        CHECK(simpleFx&&sent==1&&result->numeric.status==ai::GenerationStatus::providerFailure);++serialized;
        const auto found=std::find_if(f->requests.rbegin(),f->requests.rend(),[](const auto& request){return request.url.endsWith("/responses");});CHECK(found!=f->requests.rend());if(found==f->requests.rend())return;
        const auto& request=*found;juce::var envelope,prompt;CHECK(ai::parseStrictJson(request.body.toStdString(),envelope,oauth::maximumStructuredRequestBytes));
        CHECK(envelope["model"].toString()=="account-model"&&bool(envelope["stream"])&&!bool(envelope["store"])&&envelope["input"].size()==1);
        const auto content=envelope["input"][0]["content"].toString();CHECK(ai::parseStrictJson(content.toStdString(),prompt,oauth::maximumStructuredPromptBytes));
        const auto schema=envelope["text"]["format"]["schema"];CHECK(envelope["text"]["format"]["strict"]==juce::var(true));CHECK(json(schema)==json(prompt["proposal_schema"]));
        CHECK(content.getNumBytesAsUTF8()<=1048576&&request.body.getNumBytesAsUTF8()<=2097152&&json(schema).getNumBytesAsUTF8()<=65536);
        CHECK(prompt["catalog"]["rack_catalog"]["entries"].size()==67&&prompt["personalities"].size()==12&&prompt["linked_context"]["peers"].size()==4);
        CHECK(prompt["selected_definitions"].size()==(hydrated?12:0));CHECK(prompt["base_catalog_intent"]["catalog_selections"].size()==(hydrated?12:0));
        CHECK(prompt["base_catalog_intent"]["catalog_parameter_edits"].size()==int(local->input.request.catalog->baseIntent.catalogParameters.size()));
        const auto metadata=prompt["sample_metadata"];checkMaximumSampleProjection(metadata["requester"],&local->input.local.sourceMap);CHECK(metadata["peer_order"].toString()=="linked_context.peers"&&metadata["peers"].size()==4);
        for(unsigned peer=0;peer<4;++peer){const auto& captured=local->job.localGroupContext->peers[peer];CHECK(captured.metadataAvailable==(peer!=3||allPeers));checkMaximumSampleProjection(metadata["peers"][int(peer)],captured.metadataAvailable?&captured.map:nullptr);}
        for(const auto& marker:hidden)CHECK(!content.contains(marker)&&!request.body.contains(marker));
        for(const auto* forbidden:{"metadata_digest","asset_hash","source_path","generator_prompt","generator_seed","provenance","pcm_base64","audio_left","audio_right"})CHECK(!content.contains(forbidden));
        CHECK(local->owner->state.finishJob(local->job,frame));Frame delivered;CHECK(local->owner->state.takeReply(local->owner->id,delivered));local->owner->state.finishReply(local->owner->id,true);
        std::cout<<"OWNED_PROVIDER_ENVELOPE mode="<<mode<<" prompt_bytes="<<content.getNumBytesAsUTF8()<<" escaped_prompt_bytes="<<json(juce::var(content)).getNumBytesAsUTF8()<<" schema_bytes="<<json(schema).getNumBytesAsUTF8()<<" oauth_body_bytes="<<request.body.getNumBytesAsUTF8()<<" body_sha256="<<juce::SHA256(request.body.toRawUTF8(),std::size_t(request.body.getNumBytesAsUTF8())).toHexString()<<std::endl;
    }
    CHECK(serialized==2); // Both full4-map and genuine unavailable-peer positives.
}

void owned_local_backend_measures_and_projects(const std::shared_ptr<SigningKey>& key)
{
    begin(__func__);auto f=fixture(key);CHECK(f->seed());CompanionBackend backend(f->options());CHECK(account(backend,AccountAction::selectModel,"account-model").selectedModel=="account-model");
    for(unsigned mode=0;mode<10;++mode){auto local=std::make_unique<LocalBackendFixture>(mode==9);const int before=f->responses;Frame terminal;std::atomic_bool done=false;
        std::thread worker([&]{terminal=backend.run(local->job);done=true;});unsigned renders=0;ai::PreviewReceipt finalReceipt;
        while(!done){auto value=std::make_unique<ai::PreviewEvaluate>();const bool offered=observe([&]{return local->job.previewExchange->takeEvaluation(*value)||done.load();},5000);CHECK(offered);
            if(!offered){backend.cancel(local->job);local->job.previewExchange->cancel();break;}if(done)break;++renders;
            CHECK(value->binding.request==local->input.request.tag&&value->binding.root==local->input.local.root&&value->binding.job==local->job.jobId);
            CHECK(value->binding.providerAttempt==renders&&value->binding.renderOrdinal==renders&&value->binding.ticket!=0);
            if(mode==4){backend.cancel(local->job);continue;}
            auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
            CHECK(ai::admitProposal(local->input.request.source,local->input.request.policy,value->edits,*candidate,trace.get())==ai::ProposalStatus::accepted);
            ai::PreviewSampleMap staged;ai::PreviewDigest digest;CHECK(ai::stagePreviewMap(local->input.local.sourceMap,*trace,staged));CHECK(ai::digestPreviewMap(staged,digest)&&digest==value->binding.stagedMap);
            CHECK(ai::digestPreviewCandidate(*candidate,digest)&&digest==value->binding.candidate);CHECK(ai::samePreviewContext(value->context,ai::previewContextFor(local->input.request,value->edits.pattern)));
            ai::PreviewReceipt receipt;receipt.binding=value->binding;
            if(mode>=1&&mode<=3){receipt.status=ai::RenderStatus::renderFailure;receipt.failure=mode==1?ai::PreviewFailure::missingOwner:mode==2?ai::PreviewFailure::busy:ai::PreviewFailure::budget;}
            else {BorrowedDrumAssignments borrowed;borrowed[0][0]=local->asset.get();auto rendered=ai::renderPreview(*candidate,value->context,borrowed);
                CHECK(rendered.status==ai::RenderStatus::ready&&rendered.measurements.peak>0);
                if(((mode==6||mode==7)&&renders==1)||mode==8){rendered.audio.left[0]=mode==7?std::numeric_limits<float>::quiet_NaN():1.25f;
                    rendered.status=ai::measurePreview(rendered.audio.left,rendered.audio.right,rendered.audio.bodyFrames,rendered.measurements);}
                receipt.status=rendered.status;receipt.measurements=rendered.measurements;receipt.bodyFrames=std::uint32_t(rendered.audio.bodyFrames);receipt.frames=std::uint32_t(rendered.audio.left.size());receipt.phraseTruncated=rendered.audio.phraseTruncated;}
            CHECK(ai::validPreviewReceipt(receipt));if(mode==5){auto foreign=receipt;foreign.binding.ticket++;CHECK(!local->job.previewExchange->acceptReceipt(foreign));}
            CHECK(local->job.previewExchange->acceptReceipt(receipt));finalReceipt=receipt;
        }
        CHECK(observe([&]{return done.load();},2000));if(!done){backend.cancel(local->job);local->job.previewExchange->cancel();}worker.join();
        auto generated=std::make_unique<ai::LocalGenerationResult>();CHECK(terminal.kind==MessageKind::resultLocal&&decodeLocalResult(bytes(terminal.payload),local->input,*generated));
        CHECK(generated->numeric.audio.left.empty()&&generated->numeric.audio.right.empty());
        if(mode==0||mode==5||mode==6||mode==7||mode==9){CHECK(generated->numeric.status==ai::GenerationStatus::ready&&generated->rendered&&ai::samePreviewReceipt(*generated->rendered,finalReceipt));}
        else if(mode==4){CHECK(generated->numeric.status==ai::GenerationStatus::cancelled&&!generated->rendered);}
        else if(mode==8){CHECK(generated->numeric.status==ai::GenerationStatus::budgetExhausted&&!generated->rendered&&renders==3);}
        else {CHECK(generated->numeric.status==ai::GenerationStatus::renderFailure&&!generated->rendered);
            CHECK(generated->numeric.errorCode==(mode==1?"generation_local_source_unavailable":mode==2?"generation_local_busy":"generation_local_budget"));}
        CHECK(generated->numeric.inferenceCalls==renders&&f->responses-before==int(renders));CHECK(renders==(mode==6||mode==7?2u:mode==8?3u:1u));
        for(std::size_t i=f->requests.size();i-->0;)if(f->requests[i].url.endsWith("/responses")){
            const auto& sent=f->requests[i];juce::var envelope,prompt;CHECK(ai::parseStrictJson(sent.body.toStdString(),envelope,oauth::maximumStructuredRequestBytes));
            const auto content=envelope["input"][0]["content"].toString();CHECK(content.getNumBytesAsUTF8()<=oauth::maximumStructuredPromptBytes&&sent.body.getNumBytesAsUTF8()<=oauth::maximumStructuredRequestBytes);
            CHECK(ai::parseStrictJson(content.toStdString(),prompt,oauth::maximumStructuredPromptBytes));const auto metadata=prompt["sample_metadata"];
            CHECK(metadata["requester"]["availability"].toString()=="available"&&metadata["peers"].size()==(mode==9?4:0));
            if(mode==9){for(unsigned peer=0;peer<3;++peer){CHECK(metadata["peers"][int(peer)]["availability"].toString()=="available");
                    CHECK(metadata["peers"][int(peer)]["banks"][0][0][1].toString()=="Peer "+juce::String(peer+1));}
                CHECK(metadata["peers"][3]["availability"].toString()=="unavailable"&&!metadata["peers"][3].hasProperty("banks"));}
            CHECK(metadata["requester"]["banks"][0][0][0].toString()=="assigned");CHECK(metadata["requester"]["banks"][0][0][1].toString()==juce::String(local->input.local.sourceMap.pads[0].name));
            CHECK(int(metadata["requester"]["banks"][0][0][2])==1&&int(metadata["requester"]["banks"][0][0][3])==4800);
            CHECK(metadata["requester"]["banks"][0][1][0].toString()=="empty");
            CHECK(!content.contains(drums::assetIdText(local->asset->id()))&&!content.contains("dededededededededededededededede"));
            for(const auto* forbidden:{"provenance","source_path","generator_prompt","generator_seed","asset_hash","metadata_digest"})CHECK(!content.contains(forbidden));
            break;
        }
    }
}

}
namespace {
// Test-only: a separate default-stack observer walks the faulting CONTEXT.
struct BackendStackTrace final {
    inline static BackendStackTrace* active=nullptr;
    HANDLE signal=CreateEventW(nullptr,FALSE,FALSE,nullptr),done=CreateEventW(nullptr,FALSE,FALSE,nullptr),observer=nullptr,faulting=nullptr;
    void* token=nullptr;CONTEXT context{};DWORD exceptionCode=0;volatile LONG captured=0;std::atomic_bool stopping{false};
    static LONG CALLBACK capture(EXCEPTION_POINTERS* value) noexcept {
        auto* self=active;if(!self||value->ExceptionRecord->ExceptionCode!=EXCEPTION_STACK_OVERFLOW||InterlockedCompareExchange(&self->captured,1,0)!=0)return EXCEPTION_CONTINUE_SEARCH;
        self->context=*value->ContextRecord;self->exceptionCode=value->ExceptionRecord->ExceptionCode;
        if(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&self->faulting,0,FALSE,DUPLICATE_SAME_ACCESS)){SetEvent(self->signal);WaitForSingleObject(self->done,30000);}
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static DWORD WINAPI observe(void* pointer) noexcept {
        auto& self=*static_cast<BackendStackTrace*>(pointer);WaitForSingleObject(self.signal,INFINITE);if(self.stopping)return 0;
        try {
            const auto process=GetCurrentProcess();const auto library=LoadLibraryW(L"DbgHelp.dll");
            if(library){
                const auto initialize=reinterpret_cast<decltype(&SymInitialize)>(GetProcAddress(library,"SymInitialize"));
                const auto symbol=reinterpret_cast<decltype(&SymFromAddr)>(GetProcAddress(library,"SymFromAddr"));
                const auto line=reinterpret_cast<decltype(&SymGetLineFromAddr64)>(GetProcAddress(library,"SymGetLineFromAddr64"));
                const auto walk=reinterpret_cast<decltype(&StackWalk64)>(GetProcAddress(library,"StackWalk64"));
                const auto table=reinterpret_cast<decltype(&SymFunctionTableAccess64)>(GetProcAddress(library,"SymFunctionTableAccess64"));
                const auto base=reinterpret_cast<decltype(&SymGetModuleBase64)>(GetProcAddress(library,"SymGetModuleBase64"));
                const auto cleanup=reinterpret_cast<decltype(&SymCleanup)>(GetProcAddress(library,"SymCleanup"));
                if(initialize)initialize(process,nullptr,TRUE);
                const auto print=[&](DWORD64 at){alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+1024]{};auto* info=reinterpret_cast<SYMBOL_INFO*>(storage);info->SizeOfStruct=sizeof(SYMBOL_INFO);info->MaxNameLen=1023;DWORD64 displacement=0;IMAGEHLP_LINE64 source{};source.SizeOfStruct=sizeof(source);DWORD offset=0;
                    std::cerr<<"BACKEND_STACK_FRAME "<<reinterpret_cast<void*>(at);if(symbol&&symbol(process,at,&displacement,info))std::cerr<<" "<<info->Name<<"+"<<displacement;if(line&&line(process,at,&offset,&source))std::cerr<<" "<<source.FileName<<":"<<source.LineNumber;std::cerr<<std::endl;};
                auto context=self.context;std::cerr<<"BACKEND_ACTUAL_EXCEPTION code="<<std::hex<<self.exceptionCode<<" flags="<<context.ContextFlags<<" rip="<<context.Rip<<" rsp="<<context.Rsp<<" rbp="<<context.Rbp<<std::dec<<std::endl;
                print(context.Rip);STACKFRAME64 frame{};frame.AddrPC={context.Rip,0,AddrModeFlat};frame.AddrStack={context.Rsp,0,AddrModeFlat};frame.AddrFrame={context.Rbp,0,AddrModeFlat};
                if(walk&&table&&base)for(unsigned n=0;n<64&&walk(IMAGE_FILE_MACHINE_AMD64,process,self.faulting,&frame,&context,nullptr,table,base,nullptr)&&frame.AddrPC.Offset;++n)print(frame.AddrPC.Offset);
                if(cleanup)cleanup(process);FreeLibrary(library);
            }
        }catch(...){}
        SetEvent(self.done);return 0;
    }
    BackendStackTrace(){if(signal&&done){observer=CreateThread(nullptr,0,observe,this,0,nullptr);if(observer){active=this;token=AddVectoredExceptionHandler(1,capture);}}}
    ~BackendStackTrace(){if(token)RemoveVectoredExceptionHandler(token);active=nullptr;stopping=true;if(signal)SetEvent(signal);if(observer){WaitForSingleObject(observer,30000);CloseHandle(observer);}if(faulting)CloseHandle(faulting);if(signal)CloseHandle(signal);if(done)CloseHandle(done);}
};
}
int main(int argc,char** argv)
{
    std::unique_ptr<BackendStackTrace> stackTrace;if(argc==2&&(juce::String(argv[1])=="--stack-trace"||juce::String(argv[1])=="--preference-stack-trace"))stackTrace=std::make_unique<BackendStackTrace>();
    try
    {
        const auto key=std::make_shared<SigningKey>();
        if(argc==2&&juce::String(argv[1])=="--account-switch"){account_switch(key);finishGroup();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
        if(argc==2&&juce::String(argv[1])=="--preference-stack-trace"){std::cout<<"BACKEND_FIXTURE_SIZE Job="<<sizeof(Job)<<" RuntimeJob="<<sizeof(RuntimeJob)<<" GenerationRequest="<<sizeof(ai::GenerationRequest)<<std::endl;preference_and_model_configuration(key);finishGroup();std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
        if(argc==2&&juce::String(argv[1])=="--owned-provider-maximum"){owned_provider_maximum_and_privacy(key);finishGroup();std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
        if(argc==2&&juce::String(argv[1])=="--local-preview"){owned_local_backend_measures_and_projects(key);finishGroup();std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
        account_switch(key);owned_local_backend_measures_and_projects(key);owned_provider_maximum_and_privacy(key);preference_and_model_configuration(key);native_signin_and_account(key);exact_job_cancellation(key);full_catalog_generation(key);catalog_materialize_uses_original_inference(key);real_server_backend_integration(key);finishGroup();
    }
    catch(const std::exception&){++failures;std::cerr<<"Native fixture exception (private details suppressed)"<<std::endl;}
    std::cout<<"CompanionBackend checks="<<checks<<" groups="<<groups<<" failures="<<failures<<std::endl;return failures?1:0;
}
