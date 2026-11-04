#include "OAuthProbe.h"
#include "AiCatalog.h"
#include "ProposalDocument.h"
#include "GenerationLog.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <iostream>
#include <thread>
#include <cmath>
#include <future>
#include <string>
using namespace rack::oauth;
using juce::String;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::cout << "FAIL " << name << '\n'; } }
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields) {
    auto* o = new juce::DynamicObject; for (auto& f : fields) o->setProperty(f.first, f.second); return o;
}
String json(const juce::var& v) { return juce::JSON::toString(v, true); }
String b64(const void* p, size_t n) { return juce::Base64::toBase64(p,n).replaceCharacter('+','-').replaceCharacter('/','_').removeCharacters("="); }
String b64(const String& s) { return b64(s.toRawUTF8(),s.getNumBytesAsUTF8()); }
String param(const String& url, const String& key) {
    for (auto p : juce::StringArray::fromTokens(url.fromFirstOccurrenceOf("?",false,false),"&", ""))
        if (p.upToFirstOccurrenceOf("=",false,false) == key) return juce::URL::removeEscapeChars(p.fromFirstOccurrenceOf("=",false,false));
    return {};
}
// Test-only private signing key; actual BCrypt signature verification remains production code.
struct SigningKey {
    BCRYPT_ALG_HANDLE alg {}; BCRYPT_KEY_HANDLE key {}; String jwks;
    SigningKey() {
        BCryptOpenAlgorithmProvider(&alg,BCRYPT_RSA_ALGORITHM,nullptr,0);
        BCryptGenerateKeyPair(alg,&key,2048,0); BCryptFinalizeKeyPair(key,0);
        ULONG n=0; BCryptExportKey(key,nullptr,BCRYPT_RSAPUBLIC_BLOB,nullptr,0,&n,0);
        std::vector<unsigned char> blob(n); BCryptExportKey(key,nullptr,BCRYPT_RSAPUBLIC_BLOB,blob.data(),n,&n,0);
        auto* h=reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data()); auto* e=blob.data()+sizeof(*h); auto* modulus=e+h->cbPublicExp;
        auto publicKey=object({{"kty","RSA"},{"use","sig"},{"alg","RS256"},{"kid","test-key"},{"e",b64(e,h->cbPublicExp)},{"n",b64(modulus,h->cbModulus)}});
        jwks=json(object({{"keys",juce::Array<juce::var>{publicKey}}}));
    }
    ~SigningKey() { BCryptDestroyKey(key); BCryptCloseAlgorithmProvider(alg,0); }
    String token(String nonce, int64_t expiry=1700003600, String client="issued-client",String subject="synthetic-user",String issuer="https://auth.openai.com") {
        auto msg=b64("{\"alg\":\"RS256\",\"kid\":\"test-key\"}")+"."+b64(json(object({{"iss",issuer},{"aud",client},{"sub",subject},{"exp",juce::var(expiry)},{"nonce",nonce}})));
        BCRYPT_ALG_HANDLE hashAlg {}; BCryptOpenAlgorithmProvider(&hashAlg,BCRYPT_SHA256_ALGORITHM,nullptr,0);
        unsigned char hash[32] {}; BCryptHash(hashAlg,nullptr,0,(PUCHAR)msg.toRawUTF8(),(ULONG)msg.getNumBytesAsUTF8(),hash,32); BCryptCloseAlgorithmProvider(hashAlg,0);
        BCRYPT_PKCS1_PADDING_INFO padding { BCRYPT_SHA256_ALGORITHM }; ULONG size=0;
        BCryptSignHash(key,&padding,hash,32,nullptr,0,&size,BCRYPT_PAD_PKCS1); std::vector<unsigned char> signature(size);
        BCryptSignHash(key,&padding,hash,32,signature.data(),size,&size,BCRYPT_PAD_PKCS1);
        return msg+"."+b64(signature.data(),signature.size());
    }
};
struct Temp {
    juce::File dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("rack-oauth-test-", "",true);
    Temp() { dir.createDirectory(); }
    ~Temp() { dir.deleteRecursively(); }
};
String event(String type, juce::var fields={}) {
    if (!fields.isObject()) fields=object({}); fields.getDynamicObject()->setProperty("type",type);
    return "event: "+type+"\ndata: "+json(fields)+"\n\n";
}
String completed(String candidate="{\"engine\":\"analog\",\"cutoff_hz\":440}") {
    auto content=object({{"type","output_text"},{"text",candidate}});
    auto message=object({{"type","message"},{"content",juce::Array<juce::var>{content}}});
    auto response=object({{"status","completed"},{"output",juce::Array<juce::var>{message}}});
    return event("response.output_text.delta",object({{"delta",candidate}}))+
        event("response.completed",object({{"response",response}}));
}
struct Fixture {
    Temp temp; SigningKey& key; int64_t now=1700000000; String nonce, subject="synthetic-user", scope="openid resource.invoke chatgpt.tokens.use.direct";
    int tokenStatus=200, calls=0, tokenCalls=0; bool interrupted=false, failSave=false; String tokenError="invalid_grant", stream=completed(), refreshValue="synthetic-refresh-2", client="issued-client",accessValue="synthetic-access",initialRefresh="synthetic-refresh";
    std::function<void()> duringRequest;
    juce::var earliest; std::vector<HttpRequest> requests; String responseRequestId;
    Fixture(SigningKey& k):key(k) {}
    Transport transport() { return [this](const HttpRequest& req,const std::atomic_bool& cancel) -> HttpResponse {
        ++calls; requests.push_back(req); if(duringRequest) duringRequest();
        if(cancel) return {0,{}, {},true};
        if(req.url.endsWith("/oauth/token")) {
            ++tokenCalls;
            if(tokenStatus!=200) return {tokenStatus,json(object({{"error",tokenError},{"error_description","SENSITIVE DO NOT PRINT"}})),"request-123",false};
            auto body=object({{"access_token",accessValue},{"refresh_token",req.body.contains("refresh_token") ? refreshValue : initialRefresh},{"id_token",key.token(nonce,now+3600,client,subject)},{"token_type","Bearer"},{"scope",scope},{"expires_in",3600}});
            if(!earliest.isVoid()) body.getDynamicObject()->setProperty("earliest_refresh_at",earliest);
            if(failSave) temp.dir.getChildFile("session.bin.tmp").createDirectory();
            return {200,json(body),{},false};
        }
        if(req.url.endsWith("jwks.json")) return {200,key.jwks,{},false};
        if(req.url.endsWith("/models")) return {200,"{\"models\":[{\"slug\":\"account-model\",\"display_name\":\"Account Model\",\"visibility\":\"list\"},{\"slug\":\"hidden\",\"visibility\":\"hidden\"}]}",{},false};
        return {200,stream,responseRequestId,interrupted};
    }; }
    std::unique_ptr<OAuthProbe> probe() { return std::make_unique<OAuthProbe>(temp.dir,transport(),[this]{return now;}); }
    String begin(OAuthProbe& p) { auto a=p.beginSignIn(49123); nonce=param(a.browserUrl,"nonce"); return "/auth/callback?state="+param(a.browserUrl,"state")+"&code=synthetic-code&client_id=issued-client"; }
    Result connect(OAuthProbe& p) { return p.acceptCallback(begin(p)); }
};
void parserTests() {
    auto c=detail::parseCandidate("{\"engine\":\"analog\",\"cutoff_hz\":20}"); check(c&&c->cutoffHz==20,"valid candidate lower bound");
    check(detail::parseCandidate("{\"engine\":\"wavetable\",\"cutoff_hz\":20000}").has_value(),"valid candidate upper bound");
    for(auto s:{"{}","{\"engine\":\"fm\",\"cutoff_hz\":440}","{\"engine\":\"analog\",\"cutoff_hz\":19}","{\"engine\":\"analog\",\"cutoff_hz\":20001}","{\"engine\":\"analog\",\"cutoff_hz\":1e999}","{\"engine\":\"analog\",\"cutoff_hz\":true}","{\"engine\":\"analog\",\"cutoff_hz\":\"440\"}","{\"engine\":\"analog\",\"cutoff_hz\":440,\"x\":1}","{\"engine\":\"analog\",\"cutoff_hz\":440,\"cutoff_hz\":1}","{\"engine\":\"analog\",\"cutoff_hz\":0440}","{\"engine\":\"analog\",\"cutoff_hz\":440} garbage"}) check(!detail::parseCandidate(s),"reject invalid candidate");
    check(detail::parseEventStream(completed()).candidate.has_value(),"completed SSE publishes candidate");
    for(auto type:{"response.failed","response.incomplete","error","response.refusal.delta"})
        check(detail::parseEventStream(completed()+event(type)).status==Status::invalidResponse,"terminal failure after text rejects candidate");
    check(detail::parseEventStream(event("response.output_text.delta",object({{"delta","{\"engine\":\"analog\",\"cutoff_hz\":440}"}}))).status==Status::invalidResponse,"interrupted SSE requires completion");
    check(!detail::parseEventStream(completed("{\"engine\":\"analog\",\"cutoff_hz\":0}")).candidate,"completed invalid cutoff rejects");
    auto models=detail::parseModels("{\"models\":[{\"slug\":\"first\",\"display_name\":\"First\",\"visibility\":\"list\"},{\"slug\":\"hidden\",\"visibility\":\"hide\"},{\"slug\":\"second\",\"display_name\":\"Second\",\"visibility\":\"list\"}]}");
    check(models.models.size()==2&&models.models[0].slug=="first"&&models.models[1].slug=="second","catalog filters preserves account order");
    check(detail::parseModels("{\"data\":[]}").status==Status::invalidResponse,"wrong catalog shape rejects");
    check(detail::parseEventStream(completed()+"data: {broken\n\n").status==Status::invalidResponse,"malformed event after complete fails closed");
    check(detail::parseEventStream(event("response.output_item.done",object({{"item",object({{"type","message"},{"content",juce::Array<juce::var>{object({{"type","refusal"},{"refusal","no"}})}}})}}))+completed()).status==Status::invalidResponse,"nested refusal before completion rejects");
    check(detail::parseEventStream(completed().replace("\"completed\"","\"incomplete\"")).status==Status::invalidResponse,"terminal status must be completed");
    check(detail::parseModels("{\"models\":[{\"slug\":\"a\",\"display_name\":\"A\",\"visibility\":\"list\"},{\"slug\":\"a\",\"display_name\":\"B\",\"visibility\":\"list\"}]}").status==Status::invalidResponse,"duplicate model slug rejects");
    check(!detail::parseCandidate("{\"engine\":\"analog\",\"cutoff_hz\":440,\"cutoff_\\u0068z\":441}"),"escaped duplicate JSON key rejects");
    check(detail::sha256Url("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")=="E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM","RFC7636 challenge vector");
}
void identityTests(SigningKey& key) {
    String sub; const auto jwt=key.token("nonce");
    check(detail::verifyIdentity(jwt,key.jwks,"issued-client","nonce",1700000000,sub)&&sub=="synthetic-user","real RS256 valid signature identity");
    for(auto token:{key.token("wrong"),key.token("nonce",1699999999),key.token("nonce",1700003600,"wrong"),key.token("nonce",1700003600,"issued-client","synthetic-user","https://evil.test"),jwt.dropLastCharacters(8)+"AAAAAAAA"})
        check(!detail::verifyIdentity(token,key.jwks,"issued-client","nonce",1700000000,sub),"reject nonce expiry audience issuer signature");
    check(!detail::verifyIdentity(jwt,"{\"keys\":[]}","issued-client","nonce",1700000000,sub),"unknown signing key rejects");
}
void diagnosticTests(SigningKey& key) {
    const std::vector<std::pair<String,String>> rejected {
        {"data: {provider-secret}","sse_unterminated"},
        {"data: {provider-secret}\n\n","sse_invalid_event_json"},
        {"data: {\"provider-secret-key\":\"provider-secret-value\"}\n\n","sse_missing_event_type"},
        {"event: provider-secret\ndata: {\"type\":\"response.completed\"}\n\n","sse_event_type_mismatch"},
        {"event: provider-secret\nevent: another-secret\ndata: {}\n\n","sse_duplicate_event_field"},
        {"provider-secret-line\n\n","sse_invalid_line"},
        {"data: [DONE]\n\n","sse_done_before_completed"},
        {event("error",object({{"message","provider-secret"},{"code","provider-secret-code"}})),"sse_error"},
        {event("response.failed",object({{"response",object({{"error",object({{"code","provider-secret-code"},{"message","provider-secret"}})}})}})),"sse_failed"},
        {event("response.incomplete"),"sse_incomplete"},
        {event("response.refusal.delta",object({{"delta","provider-secret"}})),"sse_refusal"},
        {completed()+event("provider-secret-event"),"sse_event_after_completed"},
        {event("response.output_text.delta",object({{"delta",44}})),"sse_invalid_delta"},
        {event("response.output_text.delta",object({{"delta",String::repeatedString("x",16385)}})),"sse_text_limit"},
        {event("response.completed",object({{"response",object({{"status","provider-secret"},{"output",juce::Array<juce::var>{}}})}})),"sse_terminal_status"},
        {event("response.completed",object({{"response",object({{"status","completed"},{"output","provider-secret"}})}})),"sse_terminal_output"},
        {event("response.completed",object({{"response",object({{"status","completed"},{"output",juce::Array<juce::var>{}}})}})),"sse_empty_output_no_delta_terminal_empty"},
        {event("response.output_text.delta",object({{"delta","provider-secret"}}))+completed(),"sse_text_mismatch"},
        {event("provider-secret-event"),"sse_missing_completed"},
        {completed("{\"provider-secret-key\":\"provider-secret-value\"}"),"candidate_invalid"},
        {String::repeatedString("x",4*1024*1024+1),"sse_size_limit"}
    };
    for(auto& [stream,expected]:rejected) {
        auto result=detail::parseEventStream(stream);
        check(result.status==Status::invalidResponse&&!result.candidate&&result.errorCode==expected,"parser exposes only the fixed rejection reason for its branch");
        check(!result.errorCode.contains("provider-secret")&&result.requestId.isEmpty(),"parser never copies arbitrary provider strings into diagnostics");
    }
    // Only finite structural categories may leave the empty-output boundary.
    for (bool delta : {false,true}) {
        const juce::Array<juce::var> shapes[] {
            {},
            {object({{"type","provider-secret-non-message"},{"provider-secret-key","provider-secret-value"}})},
            {object({{"type","message"},{"content",juce::Array<juce::var>{object({{"type","output_text"},{"text",""}})}}})}
        };
        const char* expected[] = {
            delta ? "candidate_invalid" : "sse_empty_output_no_delta_terminal_empty",
            delta ? "sse_empty_output_delta_present_terminal_non_message" : "sse_empty_output_no_delta_terminal_non_message",
            delta ? "sse_empty_output_delta_present_terminal_message" : "sse_empty_output_no_delta_terminal_message"
        };
        for (int shape=0;shape<3;++shape) {
            auto prefix=event("response.output_text.delta",object({{"delta",delta ? "provider-secret-text" : ""}}));
            auto stream=prefix+event("response.completed",object({{"response",object({{"status","completed"},{"output",shapes[shape]}})}}));
            auto result=detail::parseEventStream(stream);
            check(result.status==Status::invalidResponse&&!result.candidate&&result.errorCode==expected[shape],"empty-output structural diagnostic is finite and preserves rejection");
        }
    }
    auto valid=detail::parseEventStream(completed());
    check(valid.candidate.has_value()&&valid.errorCode.isEmpty(),"valid structured stream has no rejection reason");
    Fixture f(key);auto p=f.probe();check(f.connect(*p).status==Status::connected,"synthetic diagnostic session connected");
    f.responseRequestId="req-safe-123";f.stream="data: {provider-secret}\n\n";
    auto result=p->requestStructuredCandidate("account-model");
    check(result.status==Status::invalidResponse&&result.httpStatus==200&&result.requestId=="req-safe-123"&&result.errorCode=="sse_invalid_event_json","inference parse rejection preserves sanitized HTTP context");
    f.responseRequestId="provider-secret\r\nAuthorization: forbidden";
    result=p->requestStructuredCandidate("account-model");
    check(result.httpStatus==200&&result.requestId.isEmpty()&&result.errorCode=="sse_invalid_event_json","unsafe request ID suppressed while fixed diagnostics retained");
    f.responseRequestId="req-safe-456";f.stream=completed();result=p->requestStructuredCandidate("account-model");
    check(result.candidate.has_value()&&result.httpStatus==200&&result.requestId=="req-safe-456"&&result.errorCode.isEmpty(),"successful inference also preserves safe HTTP context");
}
void directStreamTests() {
    const String candidate="{\"engine\":\"analog\",\"cutoff_hz\":440}";
    auto delta=[&](String text) {return event("response.output_text.delta",object({{"delta",text}}));};
    auto done=[&](juce::var text) {return event("response.output_text.done",object({{"text",text}}));};
    const auto terminal=event("response.completed",object({{"response",object({{"status","completed"},{"output",juce::Array<juce::var>{}}})}}));
    const auto pieces=delta(candidate.substring(0,16))+delta(candidate.substring(16));
    auto result=detail::parseEventStream(pieces+terminal);
    check(result.status==Status::connected&&result.candidate&&result.candidate->engine=="analog"&&result.candidate->cutoffHz==440,"direct stream deltas publish only after completed empty envelope");
    check(detail::parseEventStream(pieces+done(candidate)+terminal).candidate.has_value(),"optional matching final text with empty envelope succeeds");
    check(detail::parseEventStream(done(candidate)+terminal).status==Status::invalidResponse,"done text alone cannot replace required nonempty deltas for empty envelope");
    check(!detail::parseEventStream(pieces).candidate,"deltas without completion never publish");
    check(!detail::parseEventStream(pieces+done(candidate)).candidate,"done without terminal completion never publishes");
    for (auto type:{"error","response.failed","response.incomplete","response.refusal.delta"}) {
        check(!detail::parseEventStream(pieces+event(type)+terminal).candidate,"failure or refusal after deltas rejects before completion");
        check(!detail::parseEventStream(pieces+terminal+event(type)).candidate,"failure or refusal after completion still rejects");
    }
    check(!detail::parseEventStream(pieces+terminal.dropLastCharacters(1)).candidate,"truncated empty-envelope completion rejects");
    check(!detail::parseEventStream(delta("{\"engine\":\"analog\",\"cutoff_hz\":0}")+terminal).candidate,"empty-envelope fallback still enforces candidate range");
    check(!detail::parseEventStream(delta("{\"engine\":\"analog\",\"cutoff_hz\":440,\"cutoff_hz\":441}")+terminal).candidate,"empty-envelope fallback still rejects duplicate candidate keys");
    check(!detail::parseEventStream(delta("{\"engine\":\"fm\",\"cutoff_hz\":440}")+terminal).candidate,"empty-envelope fallback still enforces candidate schema");
    check(!detail::parseEventStream(delta("provider-secret")+completed(candidate)).candidate,"present envelope must still match all accumulated deltas");
    auto mismatch=detail::parseEventStream(pieces+done("provider-secret")+terminal);
    check(!mismatch.candidate&&mismatch.errorCode=="sse_done_text_mismatch","optional final text mismatch rejects with fixed diagnostic");
    check(!detail::parseEventStream(pieces+done(42)+terminal).candidate,"optional final text must be string");
    check(!detail::parseEventStream(pieces+done(candidate)+delta(" ")+terminal).candidate,"deltas after done cannot evade final consistency check");
    check(!detail::parseEventStream(done("provider-secret")+completed(candidate)).candidate,"optional final text also constrains present envelope");
    check(detail::parseEventStream(done(42)+completed(candidate)).errorCode=="sse_invalid_done_text","invalid done type has fixed diagnostic even with present envelope");
}
void accountSwitchTests(SigningKey& key) {
    Fixture f(key);auto p=f.probe();check(f.connect(*p).status==Status::connected,"first account connected");
    const auto returning=p->beginSignIn(49123);check(param(returning.browserUrl,"client_id")=="issued-client","same account keeps its registration");
    const auto other=p->beginSignIn(49124,SignInMode::addAccount);
    check(param(other.browserUrl,"client_id")=="dynamic_agent_client","another account must not reuse the first account registration");
    check(param(other.browserUrl,"agent_name_hint")=="air","new account includes the actual app name");
    check(param(returning.browserUrl,"ext_agent_host_id")==param(other.browserUrl,"ext_agent_host_id"),"account switching preserves host identity");
    check(p->status().status==Status::connected,"pending new sign-in preserves the current account");
    p->cancel();check(p->status().status==Status::cancelled,"cancellation observed");
    auto reopened=f.probe();check(reopened->status().status==Status::connected,"cancelled new sign-in preserves saved credentials");
    const auto finishNew=[&](OAuthProbe& probe){const auto signIn=probe.beginSignIn(49124,SignInMode::addAccount);f.nonce=param(signIn.browserUrl,"nonce");
        return probe.acceptCallback("/auth/callback?state="+param(signIn.browserUrl,"state")+"&code=synthetic-code&client_id="+f.client);};
    f.client="second-client";f.subject="second-user";f.accessValue="second-access";f.initialRefresh="second-refresh";
    f.tokenStatus=500;check(finishNew(*reopened).status==Status::networkFailure,"failed new token exchange is reported");
    std::vector<SavedAccount> accounts;String active;check(reopened->savedAccounts(accounts,active).status==Status::connected&&accounts.size()==2&&active=="issued-client","failed switch preserves first selection and retains unfinished registration");
    f.tokenStatus=200;f.scope="openid";check(finishNew(*reopened).status==Status::inferenceNotGranted,"new account without inference cannot replace working account");
    check(reopened->status().status==Status::connected,"denied permission preserves the current account");
    f.scope="openid resource.invoke chatgpt.tokens.use.direct";check(finishNew(*reopened).status==Status::connected,"second account validates and connects");
    check(reopened->savedAccounts(accounts,active).status==Status::connected&&accounts.size()==2&&active=="second-client","both registrations are saved separately");
    check(reopened->selectAccount("issued-client").status==Status::connected,"first account can be selected again");
    check(reopened->listModels().status==Status::connected&&f.requests.back().bearer=="synthetic-access","first account keeps its own access token");
    check(reopened->selectAccount("second-client").status==Status::connected,"second account can be selected again");
    check(reopened->listModels().status==Status::connected&&f.requests.back().bearer=="second-access","second account keeps its own access token");
    f.now+=3600;auto before=f.requests.size();check(reopened->listModels().status==Status::connected,"second account refresh succeeds");
    check(f.requests.size()>before&&param("?"+f.requests[before].body,"client_id")=="second-client"&&param("?"+f.requests[before].body,"refresh_token")=="second-refresh","refresh uses the matching second client and refresh token");
    check(reopened->selectAccount("unknown-client").status==Status::invalidIdentity,"unknown saved-account selection rejected");
    check(reopened->selectAccount("issued-client").status==Status::accessExpired,"expired saved account needs reauthorization");
    check(reopened->savedAccounts(accounts,active).status==Status::connected&&active=="second-client","expired account does not replace the working account before authorization");
    const auto reauthorize=reopened->beginSignIn(49124,SignInMode::existingAccount,"issued-client");
    check(param(reauthorize.browserUrl,"client_id")=="issued-client"&&!param(reauthorize.browserUrl,"id_token_hint").isEmpty(),"saved reauthorization uses that account registration and identity hint");
    f.client="issued-client";f.subject="synthetic-user";f.accessValue="renewed-first-access";f.nonce=param(reauthorize.browserUrl,"nonce");
    check(reopened->acceptCallback("/auth/callback?state="+param(reauthorize.browserUrl,"state")+"&code=synthetic-code&client_id=issued-client").status==Status::connected,"saved account is activated only after successful reauthorization");
    const auto pending=reopened->beginSignIn(49124,SignInMode::addAccount);f.nonce=param(pending.browserUrl,"nonce");
    check(reopened->selectAccount("second-client").status==Status::connected,"select another still-connected account");
    before=f.requests.size();check(reopened->acceptCallback("/auth/callback?state="+param(pending.browserUrl,"state")+"&code=synthetic-code&client_id=third-client").status==Status::invalidCallback&&f.requests.size()==before,"stale callback cannot replace a newly selected account");
    f.client="third-client";f.subject="third-user";const auto superseded=reopened->beginSignIn(49124,SignInMode::addAccount);f.nonce=param(superseded.browserUrl,"nonce");
    check(reopened->selectAccount("second-client").status==Status::connected,"explicitly keep the same working account while another sign-in is pending");
    before=f.requests.size();check(reopened->acceptCallback("/auth/callback?state="+param(superseded.browserUrl,"state")+"&code=synthetic-code&client_id=third-client").status==Status::invalidCallback&&f.requests.size()==before,"late callback cannot override a newer choice of the same account");
    f.client="issued-client";f.subject="second-user";check(finishNew(*reopened).status==Status::identityMismatch,"new registration cannot rebind an existing client to a different subject");
    {
        Fixture legacy(key);auto original=legacy.probe();check(legacy.connect(*original).status==Status::connected,"legacy account seeded");
        {detail::Store store(legacy.temp.dir);std::atomic_bool stop=false;juce::var record;
            check(store.lock(stop)&&store.load(record),"legacy protected record loaded");
            for(const char* field:{"id_token","account_revision","account_number","saved_accounts"})record.getDynamicObject()->removeProperty(field);
            check(store.save(record),"legacy flat record retained");}
        juce::MemoryBlock beforeBytes,afterBytes;check(legacy.temp.dir.getChildFile("session.bin").loadFileAsData(beforeBytes),"legacy ciphertext retained for read-only check");
        auto migrated=legacy.probe();check(migrated->savedAccounts(accounts,active).status==Status::connected&&accounts.size()==1&&active=="issued-client","old alpha account is recognized without reset");
        check(legacy.temp.dir.getChildFile("session.bin").loadFileAsData(afterBytes)&&beforeBytes==afterBytes,"listing accounts does not rewrite legacy credentials");
        const auto attempt=migrated->beginSignIn(49124,SignInMode::addAccount);legacy.nonce=param(attempt.browserUrl,"nonce");legacy.client="legacy-second";legacy.subject="legacy-second-user";
        check(migrated->acceptCallback("/auth/callback?state="+param(attempt.browserUrl,"state")+"&code=synthetic-code&client_id=legacy-second").status==Status::connected,"new account can be added beside the old alpha registration");
        check(migrated->selectAccount("issued-client").status==Status::connected,"legacy account credentials survive migration");
    }
}
void registrationRecoveryTests(SigningKey& key) {
    for(bool prior:{false,true}){
        Fixture f(key);auto base=f.transport();String challenge,redirect;const String code="one+two/three=four&five?percent%25";
        OAuthProbe p(f.temp.dir,[&](const HttpRequest& req,const std::atomic_bool& cancel){
            if(req.url.endsWith("/oauth/token")&&challenge.isNotEmpty()){
                const auto body="?"+req.body;
                check(req.method=="POST"&&req.contentType=="application/x-www-form-urlencoded"&&req.bearer.isEmpty(),"authorization exchange has public form contract");
                check(param(body,"grant_type")=="authorization_code"&&param(body,"client_id")=="pending-client","exchange uses issued registration");
                check(param(body,"code")==code,"callback code survives query and form encoding exactly");
                check(param(body,"redirect_uri")==redirect&&param(body,"resource")=="https://api.openai.com/v1","exchange preserves exact redirect and resource");
                check(detail::sha256Url(param(body,"code_verifier"))==challenge,"exchange verifier matches browser PKCE challenge");
            }return base(req,cancel);
        },[&]{return f.now;});
        if(prior)check(f.connect(p).status==Status::connected,"prior account connects before registration failure");
        f.client="pending-client";f.subject="pending-user";f.tokenStatus=400;
        auto attempt=p.beginSignIn(49124,SignInMode::addAccount);challenge=param(attempt.browserUrl,"code_challenge");redirect=param(attempt.browserUrl,"redirect_uri");f.nonce=param(attempt.browserUrl,"nonce");
        const auto callback="/auth/callback?state="+param(attempt.browserUrl,"state")+"&code="+juce::URL::addEscapeChars(code,true)+"&client_id=pending-client";
        check(p.acceptCallback(callback).status==Status::tokenFailure,"code exchange failure is preserved");
        std::vector<SavedAccount> accounts;String active;p.savedAccounts(accounts,active);
        check(accounts.size()==(prior?2:1),"issued registration survives failed exchange without replacing old account");
        const auto pending=std::find_if(accounts.begin(),accounts.end(),[](const auto& a){return a.id=="pending-client";});
        check(pending!=accounts.end()&&pending->label.contains("finish sign-in"),"unverified registration is labelled finish sign-in");
        check(active==(prior?"issued-client":"pending-client"),"old selected account is preserved during registration failure");
        check(p.status().status==(prior?Status::connected:Status::signedOut),"registration alone grants no connected identity");
        check(p.selectAccount("pending-client").status==Status::signedOut,"unfinished registration requires authorization");
        attempt=p.beginSignIn(49125,SignInMode::existingAccount,"pending-client");
        check(attempt.result.status==Status::awaitingCallback&&param(attempt.browserUrl,"client_id")=="pending-client","retry reuses issued registration instead of another dynamic client");
        if(attempt.result.status!=Status::awaitingCallback)continue;
        check(param(attempt.browserUrl,"id_token_hint").isEmpty()&&param(attempt.browserUrl,"agent_name_hint").isEmpty(),"unfinished returning registration supplies no unverified identity hint");
        challenge=param(attempt.browserUrl,"code_challenge");redirect=param(attempt.browserUrl,"redirect_uri");f.nonce=param(attempt.browserUrl,"nonce");f.tokenStatus=200;
        check(p.acceptCallback("/auth/callback?state="+param(attempt.browserUrl,"state")+"&code="+juce::URL::addEscapeChars(code,true)).status==Status::connected,"fresh authorization completes the retained registration");
        p.savedAccounts(accounts,active);check(accounts.size()==(prior?2:1)&&active=="pending-client","successful retry selects one registration without duplicating it");
        if(prior)check(p.selectAccount("issued-client").status==Status::connected,"previous account credentials survive successful retry");
    }
}
void flowTests(SigningKey& key) {
    { Fixture f(key); auto p=f.probe(); check(p->status().status==Status::signedOut,"empty store signed out"); auto cb=f.begin(*p); check(!f.nonce.isEmpty(),"fresh nonce generated"); check(p->acceptCallback(cb.replace("state=","state=wrong")).status==Status::invalidCallback&&f.tokenCalls==0,"wrong state never exchanges code"); check(p->acceptCallback(cb).status==Status::invalidCallback,"attempt consumed after invalid state"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); check(p->acceptCallback(cb.upToFirstOccurrenceOf("&code",false,false)+"&error=access_denied").status==Status::denied&&f.tokenCalls==0,"denial no token exchange"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); p->cancel(); check(p->acceptCallback(cb).status==Status::cancelled&&f.tokenCalls==0,"cancel before callback"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); f.now+=301; check(p->acceptCallback(cb).status==Status::expiredAttempt,"attempt expiry enforced"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); check(p->acceptCallback(cb.upToFirstOccurrenceOf("&client_id",false,false)).status==Status::invalidCallback,"dynamic registration requires issued client"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); check(p->acceptCallback(cb.replace("synthetic-code","%ZZ")).status==Status::invalidCallback&&f.tokenCalls==0,"malformed callback percent escape never redeemed"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); check(p->acceptCallback(cb+"&state=duplicate").status==Status::invalidCallback&&f.tokenCalls==0,"duplicate state never redeemed"); }
    { Fixture f(key); auto p=f.probe(); auto first=p->beginSignIn(49123); auto second=p->beginSignIn(49124);
      check(param(first.browserUrl,"agent_name_hint")=="air","initial registration uses the actual app name");
      check(param(first.browserUrl,"state")!=param(second.browserUrl,"state")&&param(first.browserUrl,"nonce")!=param(second.browserUrl,"nonce"),"fresh attempt random state nonce");
      check(param(first.browserUrl,"ext_agent_host_id")==param(second.browserUrl,"ext_agent_host_id")&&param(first.browserUrl,"ext_agent_host_id").startsWith("urn:uuid:"),"stable host id before authorization");
      check(param(first.browserUrl,"redirect_uri")=="http://127.0.0.1:49123/auth/callback"&&param(first.browserUrl,"code_challenge_method")=="S256", "loopback URI and PKCE authorization"); }
    { Fixture f(key); f.tokenStatus=400; auto p=f.probe(); check(f.connect(*p).status==Status::tokenFailure,"token failure handled"); auto a=p->beginSignIn(49123); check(param(a.browserUrl,"client_id")=="issued-client"&&!a.browserUrl.contains("agent_name_hint"),"retain registration after code failure"); }
    { Fixture f(key); f.scope="openid profile"; auto p=f.probe(); check(f.connect(*p).status==Status::inferenceNotGranted,"identity alone not inference permission"); check(p->requestStructuredCandidate("account-model").status==Status::inferenceNotGranted,"missing direct scope gates inference"); }
    { Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); check(p->acceptCallback(cb).status==Status::connected,"valid synthetic OAuth connects"); check(p->acceptCallback(cb).status==Status::invalidCallback,"successful callback replay rejects");
      check(p->listModels().models.size()==1,"authenticated model discovery"); auto result=p->requestStructuredCandidate("account-model"); check(result.candidate&&result.candidate->cutoffHz==440,"structured request completed");
      auto request=juce::JSON::parse(f.requests.empty() ? String() : f.requests.back().body); check(request["store"].isBool()&&!(bool)request["store"]&&(bool)request["stream"]&&request["input"].isArray(),"request disables storage streams ordinary input");
      auto format=request["text"]["format"]; check(format["type"]==juce::var("json_schema")&&(bool)format["strict"]&&!(bool)format["schema"]["additionalProperties"],"strict output schema transported");
      f.interrupted=true; check(p->requestStructuredCandidate("account-model").status==Status::networkFailure,"interrupted body cannot publish candidate"); f.interrupted=false;
      p.reset(); p=f.probe(); check(p->status().status==Status::connected,"restart reconnect DPAPI");
    }
    { Fixture f(key); f.earliest=object({{"unknown","preserve"}}); auto p=f.probe(); check(f.connect(*p).status==Status::connected,"opaque refresh scheduling retained"); int count=f.tokenCalls; f.now+=3599; check(p->listModels().models.size()==1&&f.tokenCalls==count,"no early refresh"); f.now+=1; check(p->listModels().models.size()==1&&f.tokenCalls==count+1,"expired access rotates refresh");
      check(f.requests.size() >= 3 && f.requests[f.requests.size()-3].body.contains("refresh_token=synthetic-refresh"),"refresh request uses saved rotating token");
      f.now+=3600; check(p->listModels().models.size()==1,"second refresh succeeds"); bool rotated=false; for(auto& r:f.requests) if(r.body.contains("refresh_token=synthetic-refresh-2")) rotated=true; check(rotated,"rotated token used next time"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.tokenStatus=503; check(p->listModels().status==Status::networkFailure,"temporary renewal failure"); f.tokenStatus=200; check(p->listModels().models.size()==1,"temporary error preserves renewal"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.tokenStatus=400; check(p->listModels().status==Status::tokenFailure,"terminal refresh error"); check(p->status().status==Status::signedOut,"terminal error clears unusable tokens"); check(param(p->beginSignIn(49123).browserUrl,"client_id")=="issued-client","terminal refresh preserves registration"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.subject="other-user"; check(f.connect(*p).status==Status::identityMismatch,"returning subject cannot replace saved identity"); check(p->status().status==Status::connected,"failed new account preserves credentials"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.scope="openid"; check(f.connect(*p).status==Status::inferenceNotGranted,"returning consent without inference reported"); check(p->status().status==Status::connected,"failed inference consent preserves prior granted connection"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); auto cb=f.begin(*p); check(p->acceptCallback(cb.replace("issued-client","replacement")).status==Status::invalidCallback,"returning client mismatch rejects"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.failSave=true; check(p->listModels().status==Status::storageFailure,"atomic save failure reported"); f.failSave=false; f.temp.dir.getChildFile("session.bin.tmp").deleteRecursively(); f.tokenStatus=200; check(p->listModels().models.size()==1,"save failure preserves old record"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); auto before=f.calls; f.duringRequest=[&]{p->cancel();}; auto result=p->requestStructuredCandidate("account-model"); check(result.status==Status::cancelled&&!result.candidate&&f.calls==before+1,"inflight cancellation never publishes"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.tokenStatus=400; f.tokenError="refresh_too_early"; auto result=p->listModels(); check(result.status==Status::tokenFailure&&result.errorCode=="refresh_too_early"&&result.requestId=="request-123","sanitized scheduling rejection"); f.tokenStatus=200; check(p->listModels().models.size()==1,"scheduling rejection preserves renewal"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.stream=String::repeatedString("x",4*1024*1024+1); check(p->requestStructuredCandidate("account-model").status==Status::networkFailure,"oversized SSE transport rejected"); }
    { Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; check(String(statusName(p->status().status))=="access_expired","status distinguishes expired saved access"); }
    { Fixture f(key); f.earliest=object({{"opaque","unit-unspecified"}}); auto p=f.probe(); f.connect(*p); f.now+=3600; p->listModels(); detail::Store store(f.temp.dir); std::atomic_bool cancel=false; juce::var record; check(store.lock(cancel)&&store.load(record)&&record["earliest_refresh_at"]["opaque"]==juce::var("unit-unspecified"),"opaque earliest refresh value survives rotation"); }
}
void loopbackTests() {
    detail::Loopback listener; check(listener.port()>0,"IPv4 loopback binds available port"); std::atomic_bool cancel=false; String result;
    std::thread server([&]{result=listener.receive(cancel,2000);});
    auto send=[&](String path,String host) { juce::StreamingSocket client; if(!client.connect("127.0.0.1",listener.port(),500)) return; auto request="GET "+path+" HTTP/1.1\r\nHost: "+host+"\r\n\r\n"; client.write(request.toRawUTF8(),(int)request.getNumBytesAsUTF8()); client.waitUntilReady(true,300); char reply[512]; client.read(reply,sizeof(reply),false); };
    send("/favicon.ico","127.0.0.1:"+String(listener.port()));
    send("/auth/callback?state=synthetic","evil.test");
    send("/auth/callback?state=synthetic","127.0.0.1:"+String(listener.port()));
    server.join(); check(result=="/auth/callback?state=synthetic","listener only accepts intended path and host");
    cancel=true; check(listener.receive(cancel,100).isEmpty(),"listener cancellation bounded");
}
// Local HTTP is confined to this fixture. Production nativeHttp keeps its HTTPS
// destination allowlist; only its real timeout configuration/lifecycle are shared.
struct DelayedHttpServer {
    juce::StreamingSocket listener;
    std::mutex mutex;
    std::condition_variable changed;
    bool stop=false;
    std::thread worker;
    explicit DelayedHttpServer(int delayMs) {
        if(!listener.createListener(0,"127.0.0.1")) return;
        worker=std::thread([this,delayMs] {
            while(listener.waitUntilReady(true,100)!=1) {std::lock_guard lock(mutex);if(stop)return;}
            std::unique_ptr<juce::StreamingSocket> client(listener.waitForNextConnection());
            if(!client) return;
            std::string headers;
            while(headers.find("\r\n\r\n")==std::string::npos&&headers.size()<4096) {
                if(client->waitUntilReady(true,100)!=1) {std::lock_guard lock(mutex);if(stop)return;continue;}
                char buffer[512];auto bytes=client->read(buffer,sizeof(buffer),false);if(bytes<=0)return;
                headers.append(buffer,static_cast<size_t>(bytes));
            }
            const std::string first="HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nA";
            if(client->write(first.data(),static_cast<int>(first.size()))!=static_cast<int>(first.size())) return;
            std::unique_lock lock(mutex);
            if(!changed.wait_for(lock,std::chrono::milliseconds(delayMs),[this]{return stop;})) client->write("B",1);
        });
    }
    ~DelayedHttpServer() {
        {std::lock_guard lock(mutex);stop=true;}changed.notify_all();
        if(worker.joinable())worker.join();
    }
};
void CALLBACK timeoutTestCallback(HINTERNET,DWORD_PTR context,DWORD status,void* info,DWORD length) {
    auto* state=reinterpret_cast<detail::NativeHttpLifecycle*>(context);if(!state)return;
    using Event=detail::NativeHttpLifecycle::Event;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)state->notify(Event::closing);
    else if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)state->notifyRequestError(info,length);
    else if(status==WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)state->notify(Event::sent);
    else if(status==WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)state->notify(Event::headers);
    else if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE)state->notify(Event::read,length);
}
struct DelayedReadResult {bool complete=false,retired=false;unsigned nativeError=0;const char* failurePhase="none";std::string body;long long elapsedMs=0;};
DelayedReadResult delayedNativeRead(int budgetMs,int delayMs,bool cancelRead=false) {
    using Life=detail::NativeHttpLifecycle;using Event=Life::Event;
    DelayedHttpServer server(delayMs);DelayedReadResult result;
    auto session=WinHttpOpen(L"Rack Synth timeout test",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
    auto connection=session?WinHttpConnect(session,L"127.0.0.1",static_cast<INTERNET_PORT>(server.listener.getBoundPort()),0):nullptr;
    auto request=connection?WinHttpOpenRequest(connection,L"GET",L"/",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;
    if(!request) {if(connection)WinHttpCloseHandle(connection);if(session)WinHttpCloseHandle(session);return result;}
    auto available=std::make_shared<std::atomic_bool>(true);
    auto state=Life::create({request,connection,session},{},[](void* handle){return WinHttpCloseHandle(handle)!=FALSE;},available);
    std::atomic_bool cancel=false;
    const auto start=std::chrono::steady_clock::now(),deadline=start+std::chrono::milliseconds(budgetMs);
    HttpRequest logicalRequest {"POST","https://api.openai.com/v1/responses",{},{},{},1024,budgetMs};
    DWORD_PTR context=reinterpret_cast<DWORD_PTR>(state.get());
    bool ok=detail::nativeHttpTimeouts(request,logicalRequest,deadline)
        &&WinHttpSetOption(request,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))
        &&WinHttpSetStatusCallback(request,timeoutTestCallback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)!=WINHTTP_INVALID_STATUS_CALLBACK;
    state->expectCallbacks();
    ok=ok&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,context)
        &&state->wait(Event::sent,cancel,deadline);
    state->reset();
    ok=ok&&detail::nativeHttpTimeouts(request,logicalRequest,deadline)&&WinHttpReceiveResponse(request,nullptr)
        &&state->wait(Event::headers,cancel,deadline);
    std::thread canceller;
    if(cancelRead)canceller=std::thread([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));cancel=true;});
    while(ok) {
        state->reset();
        ok=detail::nativeHttpTimeouts(request,logicalRequest,deadline);
        if(!ok){result.nativeError=GetLastError();result.failurePhase="read_configuration";break;}
        ok=WinHttpReadData(request,state->readBuffer(),8192,nullptr)!=FALSE;
        if(!ok){result.nativeError=GetLastError();result.failurePhase="read_initiation";break;}
        ok=state->wait(Event::read,cancel,deadline);
        if(!ok){result.nativeError=state->nativeError();result.failurePhase="read_wait";}
        if(!ok)break;
        const auto bytes=state->bytesRead();if(bytes==0){result.complete=true;break;}
        result.body.append(state->readBuffer(),bytes);
    }
    result.elapsedMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    if(result.nativeError==0)result.nativeError=state->nativeError();
    if(canceller.joinable())canceller.join();
    std::atomic_bool cleanupCancel=false;
    result.retired=state->finish(cleanupCancel,std::chrono::steady_clock::now()+std::chrono::seconds(2))&&*available;
    return result;
}
void nativeReceiveTimeoutTests() {
    auto session=WinHttpOpen(L"Rack Synth timeout options",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    check(session!=nullptr,"native timeout fixture opens real WinHTTP session");if(!session)return;
    const auto query=[&](DWORD option){DWORD value=0,size=sizeof(value);check(WinHttpQueryOption(session,option,&value,&size)!=FALSE,"query applied native timeout option");return value;};
    const auto deadline=[](int ms){return std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);};
    HttpRequest inference {"POST","https://api.openai.com/v1/responses",{},{},{},1024,60000};
    check(detail::nativeHttpTimeouts(session,inference,deadline(60000)),"configure real inference timeout options");
    check(query(WINHTTP_OPTION_RESOLVE_TIMEOUT)==5000&&query(WINHTTP_OPTION_CONNECT_TIMEOUT)==5000
        &&query(WINHTTP_OPTION_SEND_TIMEOUT)==5000,"inference preserves short resolve connect send bounds");
    auto receive=query(WINHTTP_OPTION_RECEIVE_TIMEOUT);
    check(receive>59000&&receive<=60000,"inference receive uses existing total request budget");
    check(detail::nativeHttpTimeouts(session,inference,deadline(250)),"refresh inference receive with remaining budget");
    receive=query(WINHTTP_OPTION_RECEIVE_TIMEOUT);
    check(receive>0&&receive<=250,"inference receive shrinks with original deadline");
    inference.timeoutMs=100;
    check(detail::nativeHttpTimeouts(session,inference,deadline(1000)),"configure shorter request budget");
    check(query(WINHTTP_OPTION_RECEIVE_TIMEOUT)==100,"receive never exceeds request budget even with later deadline");
    check(!detail::nativeHttpTimeouts(session,inference,deadline(-1)),"expired inference cannot start an infinite native wait");
    for(const auto& other:std::vector<HttpRequest>{{"POST","https://auth.openai.com/api/accounts/oauth/token",{},{},{},1024,30000},
        {"GET","https://auth.openai.com/.well-known/jwks.json",{},{},{},1024,30000},
        {"GET","https://api.openai.com/v1/models",{},{},{},1024,30000},
        {"GET","https://api.openai.com/v1/responses",{},{},{},1024,60000}}) {
        check(detail::nativeHttpTimeouts(session,other,deadline(60000)),"configure existing non-inference bounds");
        check(query(WINHTTP_OPTION_RECEIVE_TIMEOUT)==5000,"auth catalog and non-POST receive stay bounded at five seconds");
    }
    WinHttpCloseHandle(session);
    const auto delayed=delayedNativeRead(25000,12200);
    std::cout<<"native delayed read elapsed="<<delayed.elapsedMs<<"ms bytes="<<delayed.body.size()<<" error="<<delayed.nativeError<<" phase="<<delayed.failurePhase<<'\n';
    check(delayed.complete&&delayed.body=="AB"&&delayed.nativeError==0,"real inference read survives a stream gap beyond five seconds");
    check(delayed.elapsedMs>=12000&&delayed.elapsedMs<25000,"delayed read completes inside original total deadline");
    check(delayed.retired,"delayed native request safely retires handles and admission");
    const auto bounded=delayedNativeRead(250,2000);
    check(!bounded.complete&&bounded.elapsedMs<1500,"native stream wait obeys original short total deadline");
    check(bounded.retired,"deadline exit safely retires native ownership");
    const auto cancelled=delayedNativeRead(15000,2000,true);
    check(!cancelled.complete&&cancelled.elapsedMs<1500,"native stream wait remains promptly cancellable");
    check(cancelled.retired,"cancelled native request safely retires ownership");
}
void storageTests() {
    Temp temp; std::atomic_bool cancel=false; auto saved=object({{"secret","SYNTHETIC-SECRET-NOT-PLAINTEXT"}});
    { detail::Store store(temp.dir); check(store.lock(cancel),"store exclusive lock"); check(store.save(saved),"DPAPI atomic protected save"); }
    check(!temp.dir.getChildFile("session.bin").loadFileAsString().contains("SYNTHETIC-SECRET"),"disk never plaintext secret");
    { detail::Store store(temp.dir); juce::var got; check(store.lock(cancel)&&store.load(got)&&got["secret"]==saved["secret"],"DPAPI protected roundtrip"); }
    PSECURITY_DESCRIPTOR descriptor {}; PACL acl {};
    auto path=temp.dir.getChildFile("session.bin").getFullPathName();
    bool privateAcl=GetNamedSecurityInfoW(const_cast<LPWSTR>(path.toWideCharPointer()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)==ERROR_SUCCESS;
    SECURITY_DESCRIPTOR_CONTROL flags {}; DWORD revision=0;
    privateAcl=privateAcl && GetSecurityDescriptorControl(descriptor,&flags,&revision) && (flags&SE_DACL_PROTECTED) && acl && acl->AceCount==2;
    check(privateAcl,"protected file ACL blocks inherited access"); if(descriptor) LocalFree(descriptor);
    {
        detail::Store store(temp.dir); check(store.lock(cancel),"parent process owns refresh lock");
        juce::ChildProcess child;
        check(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--lock-check",temp.dir.getFullPathName()}),"start isolated lock contender");
        check(child.waitForProcessToFinish(8000)&&child.getExitCode()==17,"second process cannot enter refresh critical section");
    }
    {
        juce::ChildProcess child;
        check(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--lock-check",temp.dir.getFullPathName()}),"restart isolated lock contender");
        check(child.waitForProcessToFinish(2000)&&child.getExitCode()==0,"released refresh lock permits another process");
    }
    temp.dir.getChildFile("session.bin").replaceWithText("broken ciphertext");
    { detail::Store store(temp.dir); juce::var got; check(store.lock(cancel)&&!store.load(got),"corrupt protected record fails closed"); }
    check(detail::nativeHttp({"GET","https://evil.test",{},{},{},100,100},cancel).status==0,"native transport refuses untrusted destination offline");
}
void lifecycleTests() {
    using Life=detail::NativeHttpLifecycle; using Event=Life::Event;
    const auto deadline=[] {return std::chrono::steady_clock::now()+std::chrono::milliseconds(40);};
    // A fake native close controls only the OS outcome; ownership, waiting, buffers,
    // completion and callback release below are all the production lifecycle.
    for(bool closeOk:{true,false}) {
        auto available=std::make_shared<std::atomic_bool>(true);
        std::atomic_bool lateStorage=false; std::atomic<int> closes=0;
        auto state=Life::create({(void*)1,(void*)2,(void*)3},"synthetic-send-buffer",[&](void*){++closes;return closeOk;},available);
        state->expectCallbacks(); std::weak_ptr<Life> weak=state;
        std::thread late([weak,&lateStorage] {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if(auto callback=weak.lock()) {
                callback->readBuffer()[0]='L';
                lateStorage=callback->sendBody()=="synthetic-send-buffer";
                callback->notify(Event::read,1);
                callback->notify(Event::closing);
            }
        });
        std::atomic_bool cancel=!closeOk;
        auto started=std::chrono::steady_clock::now();
        check(!state->finish(cancel,deadline()),closeOk?"delayed closing returns incomplete":"failed close returns incomplete");
        auto elapsed=std::chrono::steady_clock::now()-started;
        check(elapsed<std::chrono::milliseconds(150),closeOk?"delayed final callback cannot block caller":"cancel plus failed close cannot block caller");
        state.reset();
        check(!weak.expired()&&!*available,"callback context and send/read storage survive bounded return");
        check(!Life::create({},"",[](void*){return true;},available),"unresolved cleanup refuses further native admission");
        late.join();
        check(lateStorage,"late read and final callback access valid owned storage");
        if(closeOk) check(weak.expired()&&*available,"successful final notification retires owned native lifecycle");
        else {
            check(closes==1,"failed request close does not retry or close parent handles");
            check(!weak.expired()&&!*available,"failed close retains uncertain handles even after callback");
        }
    }
    {
        auto available=std::make_shared<std::atomic_bool>(true);
        std::atomic_bool cancel=false; std::atomic<int> closes=0;
        auto state=Life::create({(void*)1,(void*)2,(void*)3},"synthetic-send",[&](void*){++closes;return true;},available);
        state->expectCallbacks(); std::weak_ptr<Life> weak=state;
        std::thread eventual([weak]{std::this_thread::sleep_for(std::chrono::milliseconds(250));if(auto s=weak.lock())s->notify(Event::closing);});
        auto started=std::chrono::steady_clock::now();
        check(!state->wait(Event::read,cancel,deadline()),"native completion deadline expires");
        cancel=true;
        check(!state->finish(cancel,deadline()),"cancelled cleanup returns interrupted");
        state.reset();
        check(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(150)&&!weak.expired(),"missing notification retains ownership beyond cancelled caller");
        eventual.join();check(*available&&closes==3,"eventual final callback releases successful hierarchy exactly once");
    }
    {
        // No final notification at all: retain a bounded quarantine, no test-only
        // force-free path, no second close, and no new request admitted to this slot.
        auto available=std::make_shared<std::atomic_bool>(true);
        auto closes=std::make_shared<std::atomic<int>>(0);
        auto state=Life::create({(void*)1,(void*)2,(void*)3},"still-owned",[closes](void*){++*closes;return true;},available);
        state->expectCallbacks();std::weak_ptr<Life> weak=state;std::atomic_bool cancel=false;
        check(!state->finish(cancel,deadline()),"absent final notification returns at cleanup deadline");
        state.reset();
        check(!weak.expired()&&!*available&&!Life::create({},"",[](void*){return true;},available),"absent final notification quarantines and blocks subsequent request");
        if(auto late=weak.lock()) {
            late->readBuffer()[0]='Q';late->notify(Event::read,1);
            check(late->sendBody()=="still-owned"&&late->bytesRead()==1,"late non-final callback remains safe in quarantine");
            check(!late->finish(cancel,deadline())&&*closes==3,"repeat finish never reuses close-called handles");
        }
    }
    {
        auto available=std::make_shared<std::atomic_bool>(true);
        auto releaseClose=std::make_shared<std::atomic_bool>(false);
        auto entered=std::make_shared<std::promise<void>>();auto enteredFuture=entered->get_future();
        auto state=Life::create({(void*)1,(void*)2,(void*)3},"blocked-close-send",[releaseClose,entered](void* h){
            if(h==(void*)1){entered->set_value();while(!*releaseClose)std::this_thread::sleep_for(std::chrono::milliseconds(2));}return true;
        },available);
        state->expectCallbacks();std::weak_ptr<Life> weak=state;std::atomic_bool cancel=true;
        auto start=std::chrono::steady_clock::now();
        check(!state->finish(cancel,deadline())&&std::chrono::steady_clock::now()-start<std::chrono::milliseconds(150),"blocked synchronous close cannot block cancelled caller");
        state.reset();check(enteredFuture.wait_for(std::chrono::seconds(1))==std::future_status::ready&&!weak.expired(),"cleanup owner retains native hierarchy during blocked close");
        if(auto callback=weak.lock()) {callback->readBuffer()[0]='B';callback->notify(Event::closing);}
        check(!weak.expired()&&!*available,"final callback cannot retire while cleanup owner still uses hierarchy");
        *releaseClose=true;
        for(int i=0;i<250&&!weak.expired();++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(weak.expired()&&*available,"cleanup completion and final callback jointly retire hierarchy");
    }
    {
        // Close failure itself returns promptly without relying on cancellation,
        // and failure at any level retains every uncertain ancestor.
        for(int failAt:{1,2,3}) {
            auto available=std::make_shared<std::atomic_bool>(true);
            auto closed=std::make_shared<std::vector<void*>>();
            auto route=std::make_shared<std::weak_ptr<Life>>();
            auto state=Life::create({(void*)1,(void*)2,(void*)3},"failed-close-owned",[failAt,closed,route](void* h){
                closed->push_back(h);
                if(h==(void*)1&&failAt!=1)if(auto callback=route->lock())callback->notify(Event::closing);
                return h!=(void*)(intptr_t)failAt;
            },available);
            *route=state;state->expectCallbacks();std::atomic_bool cancel=false;
            auto started=std::chrono::steady_clock::now();
            check(!state->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1))&&std::chrono::steady_clock::now()-started<std::chrono::milliseconds(150),"close failure returns before request deadline without final signal");
            state.reset();
            check(closed->size()==(size_t)failAt&&!route->expired()&&!*available,"failed close retains hierarchy and leaves remaining parents untouched");
        }
    }
    {
        auto available=std::make_shared<std::atomic_bool>(true);std::weak_ptr<Life> weak;std::vector<void*> closed;
        auto state=Life::create({(void*)1,(void*)2,(void*)3},"normal",[&](void* h){closed.push_back(h);if(h==(void*)1)if(auto callback=weak.lock())callback->notify(Event::closing);return true;},available);
        weak=state;state->expectCallbacks();std::atomic_bool cancel=false;
        check(state->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1)),"normal close and inline final callback complete");
        check(*available&&closed==std::vector<void*>{(void*)1,(void*)2,(void*)3},"successful close immediately restores admission with request connection and session closed once");
        state.reset();
    }
}
void admissionRetirementTests() {
    using Life=detail::NativeHttpLifecycle;using Event=Life::Event;
    auto slot=std::make_shared<std::atomic_bool>(true);
    auto route=std::make_shared<std::weak_ptr<Life>>();
    auto old=Life::create({(void*)1,(void*)2,(void*)3},"old-completed",[route](void* h){
        if(h==(void*)1)if(auto callback=route->lock())callback->notify(Event::closing);return true;
    },slot);
    *route=old;old->expectCallbacks();std::atomic_bool cancel=false;
    auto heldOld=old; // Deliberately prevent destruction: no sleeps or polling can hide F1.
    check(old->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1)),"completed lifecycle reports success while referenced");
    check(*slot,"successful finish immediately retires admission before destruction");
    auto newer=Life::create({(void*)4,(void*)5,(void*)6},"new-active",[](void*){return true;},slot);
    check(newer!=nullptr,"immediate next request admitted while completed old reference is held");
    if(!newer)return;
    newer->expectCallbacks();
    check(!*slot,"new request owns admission");
    old->notify(Event::closing);
    check(!*slot,"repeated old final signal cannot release newer admission");
    check(old->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1))&&!*slot,"repeated old finish cannot release newer admission");
    old.reset();heldOld.reset();
    check(!*slot&&!Life::create({},"unexpected-third",[](void*){return true;},slot),"old reference destruction cannot admit a third request over newer owner");
    newer->notify(Event::closing);
    check(newer->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1))&&*slot,"new owner retires its own admission on success");
    auto newest=Life::create({},"third-owner",[](void*){return true;},slot);
    check(newest!=nullptr,"third request admitted while second completed reference is held");
    newer->notify(Event::closing);newer.reset();
    check(!*slot,"second owner cannot reopen third owner admission during retirement");
    if(newest)check(newest->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1))&&*slot,"final admitted owner releases its slot");
}
}
namespace {
// Each group catches a boundary mutation: early publication, character/byte
// confusion, truncated request, pre-auth validation bypass, or auth/cancel reset.
void structuredGroup(const char* name, const std::function<void()>& run) {
    const auto before=checks, failed=failures; run();
    std::cout<<name<<": "<<checks-before<<" checks, "<<failures-failed<<" failures\n";
}
String emptyTerminal() {
    return event("response.completed",object({{"response",object({{"status","completed"},{"output",juce::Array<juce::var>{}}})}}));
}
String terminalText(String text) {
    const auto part=object({{"type","output_text"},{"text",text}});
    const auto message=object({{"type","message"},{"content",juce::Array<juce::var>{part}}});
    return event("response.completed",object({{"response",object({{"status","completed"},{"output",juce::Array<juce::var>{message}}})}}));
}
juce::var proposalWire() {
    return object({{"version",1},{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.25}})}},
        {"rack_edits",juce::Array<juce::var>{}},{"macro_edits",juce::Array<juce::var>{}},{"pattern",{}},
        {"characteristics",String::fromUTF8("warm \xc3\xa9 \xe6\xbc\xa2 \xf0\x9f\x8e\xb5")},
        {"explanation",String::fromUTF8("Soft attack \xe2\x80\x94 preserve the source.")}});
}
void structuredStreamCompletion() {
    const String text="untrusted complete text";
    auto r=detail::parseStructuredEventStream(completed(text));
    check(r.result.status==Status::connected&&r.text==text&&!r.result.candidate,"general completion returns exact untrusted text without toy candidate");
    auto pieces=event("response.output_text.delta",object({{"delta","untrusted "}}))+event("response.output_text.delta",object({{"delta","complete text"}}));
    r=detail::parseStructuredEventStream(pieces+event("response.output_text.done",object({{"text",text}}))+emptyTerminal());
    check(r.result.status==Status::connected&&r.text==text,"matching deltas done and empty completed envelope materialize complete text");
    const auto longCandidate=String::repeatedString(" ",17000)+"{\"engine\":\"analog\",\"cutoff_hz\":440}";
    check(detail::parseStructuredEventStream(completed(longCandidate)).text==longCandidate,"general completed text above feasibility limit succeeds");
    check(detail::parseEventStream(completed(longCandidate)).errorCode=="sse_text_limit","legacy character limit remains enforced");
    const std::vector<std::pair<String,const char*>> bad {
        {pieces+terminalText("provider-secret"),"sse_text_mismatch"},
        {pieces+event("response.output_text.done",object({{"text","provider-secret"}}))+emptyTerminal(),"sse_done_text_mismatch"},
        {event("response.output_text.done",object({{"text","first"}}))+event("response.output_text.done",object({{"text","second"}})),"sse_done_text_mismatch"},
        {event("response.output_text.done",object({{"text",42}})),"sse_invalid_done_text"},
        {event("response.output_text.delta",object({{"delta",42}})),"sse_invalid_delta"},
        {pieces+event("response.refusal.delta",object({{"delta","provider-secret"}})),"sse_refusal"},
        {pieces+event("response.incomplete"),"sse_incomplete"},
        {pieces+event("response.failed"),"sse_failed"},
        {pieces+event("error",object({{"message","provider-secret"}})),"sse_error"},
        {pieces+emptyTerminal().dropLastCharacters(1),"sse_unterminated"},
        {completed(text)+"data: {provider-secret}\n","sse_unterminated"},
        {pieces,"sse_missing_completed"},
        {"data: [DONE]\n\n","sse_done_before_completed"},
        {completed(text)+event("response.created"),"sse_event_after_completed"},
        {emptyTerminal(),"sse_empty_output_no_delta_terminal_empty"},
        {"data: {\"type\":\"response.created\",\"type\":\"response.completed\"}\n\n","sse_invalid_event_json"},
        {String::repeatedString("x",4*1024*1024+1),"sse_size_limit"}
    };
    for(const auto& [stream,reason]:bad) {
        r=detail::parseStructuredEventStream(stream);
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode==reason,"general failed stream returns empty text and literal safe reason");
        check(!r.result.errorCode.contains("provider-secret")&&r.result.requestId.isEmpty(),"general parser does not disclose provider text");
    }
}
void structuredErrorDiagnostics(SigningKey& key) {
    const char* known[] {"invalid_grant","invalid_client","invalid_scope","access_denied","insufficient_quota",
        "rate_limit_exceeded","server_error","temporarily_unavailable","invalid_token","model_not_found",
        "permission_denied","refresh_too_early"};
    const String candidate="{\"engine\":\"analog\",\"cutoff_hz\":440}";
    const auto delta=event("response.output_text.delta",object({{"delta",candidate}}));
    for(const auto* code:known) for(bool afterDelta:{false,true}) {
        const auto stream=(afterDelta?delta:String())+event("error",object({{"code",code},
            {"message","SENSITIVE provider message"},{"param","SENSITIVE provider parameter"}}));
        const auto r=detail::parseStructuredEventStream(stream);
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&!r.result.candidate,"known stream error still withholds text and candidate");
        check(r.result.errorCode==code,"known top-level stream error exposes exact whitelisted literal");
        check(!r.result.errorCode.contains("SENSITIVE")&&r.result.requestId.isEmpty(),"stream classification copies no message parameter or identity");
    }
    const std::vector<juce::var> unknown {juce::var(),true,42,1.5,juce::Array<juce::var>{"insufficient_quota"},
        object({{"code","insufficient_quota"}}),"ordinary_ascii_code","rate_limit_reached","INSUFFICIENT_QUOTA",
        " insufficient_quota","insufficient_quota ","insufficient_quota_extra","SENSITIVE-private-code",
        "insufficient_quota\r\nSENSITIVE",String::fromUTF8("private \xc3\xa9")};
    for(std::size_t index=0;index<=unknown.size();++index) for(bool afterDelta:{false,true}) {
        auto fields=object({{"message","SENSITIVE provider message"},{"param","SENSITIVE provider parameter"},
            {"error",object({{"code","insufficient_quota"}})}});
        if(index<unknown.size()) fields.getDynamicObject()->setProperty("code",unknown[index]);
        const auto r=detail::parseStructuredEventStream((afterDelta?delta:String())+event("error",fields));
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&!r.result.candidate,"unknown stream error still withholds text and candidate");
        check(r.result.errorCode=="sse_error","missing null non-string unknown and private codes keep fixed fallback");
        check(!r.result.errorCode.contains("SENSITIVE")&&r.result.requestId.isEmpty(),"fallback never copies private event fields");
    }
    const auto legacy=detail::parseEventStream(delta+event("error",object({{"code","insufficient_quota"},
        {"message","SENSITIVE provider message"},{"param","SENSITIVE provider parameter"}})));
    check(legacy.status==Status::invalidResponse&&!legacy.candidate,"legacy candidate parser withholds valid deltas on known error");
    check(legacy.errorCode=="insufficient_quota"&&legacy.requestId.isEmpty(),"legacy parser shares exact safe stream classification");
    check(detail::parseStructuredEventStream(event("response.failed",object({{"response",object({{"error",object({{"code","insufficient_quota"}})}})}}))).result.errorCode=="sse_failed","response failed keeps its existing diagnostic despite known nested code");
    Fixture f(key);auto p=f.probe();check(f.connect(*p).status==Status::connected,"stream diagnostic uses authenticated synthetic transport");
    f.stream=delta+event("error",object({{"code","rate_limit_exceeded"},{"message","SENSITIVE"},{"param","SENSITIVE"}}));
    f.responseRequestId="req-stream-safe-123";const auto before=f.calls;
    auto r=p->requestStructured("account-model","valid source prompt",airack::ai::proposalSchema());
    check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&!r.result.candidate&&f.calls==before+1,"known provider stream failure makes one synthetic inference call and withholds output");
    check(r.result.httpStatus==200&&r.result.requestId=="req-stream-safe-123"&&r.result.errorCode=="rate_limit_exceeded","known stream code preserves HTTP status and safe request context");
    f.responseRequestId="unsafe\r\nSENSITIVE";
    r=p->requestStructured("account-model","valid source prompt",airack::ai::proposalSchema());
    check(r.result.status==Status::invalidResponse&&r.result.httpStatus==200&&r.result.requestId.isEmpty()&&r.result.errorCode=="rate_limit_exceeded"&&r.text.isEmpty()&&!r.result.candidate,"known classification never bypasses request ID sanitization");
}
void streamErrorShapes(SigningKey& key) {
    // Breaks caught: arbitrary provider values leaking, shape distinctions lost,
    // diagnostic-only vocabulary changing ordinary classification, or a second
    // parser skipping the legacy framing, limits and early rejection path.
    const String absent="type=error;code=missing;param=missing;error=missing";
    const String nestedMissing="type=error;code=missing;param=missing;error=object;error.code=missing;error.type=missing;error.param=missing";
    const String delta=event("response.output_text.delta",object({{"delta","{\"engine\":\"analog\",\"cutoff_hz\":440}"}}));
    auto diagnostic=[&](const String& stream,const String& expected) {
        const auto safe=detail::streamErrorDiagnostic(stream);
        check(safe==expected,"diagnostic reports hand-derived fixed shapes and literals only");
        check(safe.length()<=320&&safe.containsOnly("abcdefghijklmnopqrstuvwxyz_=.;"),"diagnostic is bounded fixed ASCII without private fingerprints");
        check(!safe.contains("SENSITIVE"),"diagnostic never copies private messages values or keys");
    };
    diagnostic(event("error"),absent);
    diagnostic(event("error",object({{"message","SENSITIVE"},{"SENSITIVE-key","SENSITIVE"}})),absent);
    diagnostic(event("error",object({{"error",object({{"message","SENSITIVE"},{"SENSITIVE-key","SENSITIVE"}})}})),nestedMissing);
    struct Shape { juce::var value; const char* label; };
    const std::vector<Shape> shapes={
        {{},"null"},{false,"non_string"},{42,"non_string"},{.25,"non_string"},
        {object({{"SENSITIVE-key","SENSITIVE"}}),"non_string"},{juce::Array<juce::var>{"SENSITIVE"},"non_string"},
        {"","unrecognized"},{"SENSITIVE","unrecognized"},{"Bearer SENSITIVE","unrecognized"},
        {"req-SENSITIVE","unrecognized"},{"rate_limit_exceeded-SENSITIVE","unrecognized"},
        {" rate_limit_exceeded","unrecognized"},{"INSUFFICIENT_QUOTA","unrecognized"},
        {String::fromUTF8("\xc3\xa9SENSITIVE"),"unrecognized"},{"model.SENSITIVE","unrecognized"}
    };
    for(const auto& shape:shapes) for(auto field:{"code","param"}) {
        auto fields=object({{field,shape.value},{"message","SENSITIVE"}});
        diagnostic(event("error",fields),String("type=error;code=")+(String(field)=="code"?shape.label:"missing")
            +";param="+(String(field)=="param"?shape.label:"missing")+";error=missing");
    }
    for(const auto& shape:shapes) for(auto nestedField:{"code","type","param"}) {
            auto nested=object({{nestedField,shape.value},{"message","SENSITIVE"}});
            diagnostic(event("error",object({{"error",nested}})),String("type=error;code=missing;param=missing;error=object;error.code=")
                +(String(nestedField)=="code"?shape.label:"missing")+";error.type="+(String(nestedField)=="type"?shape.label:"missing")
                +";error.param="+(String(nestedField)=="param"?shape.label:"missing"));
    }
    for(const auto& value:std::vector<juce::var>{juce::var(),false,42,"SENSITIVE",juce::Array<juce::var>{"SENSITIVE"}})
        diagnostic(event("error",object({{"error",value}})),String("type=error;code=missing;param=missing;error=")+(value.isVoid()?"null":"non_object"));
    const std::vector<String> codes={"invalid_grant","invalid_client","invalid_scope","access_denied","insufficient_quota",
        "rate_limit_exceeded","server_error","temporarily_unavailable","invalid_token","model_not_found","permission_denied","refresh_too_early",
        "credit_balance_exhausted","slow_down","organization_spend_limit_exceeded","project_spend_limit_exceeded",
        "organization_usage_limit_exceeded","server_is_overloaded","rate_limit_reached"};
    for(size_t i=0;i<codes.size();++i) for(bool afterDelta:{false,true}) {
        const auto fields=object({{"code",codes[i]},{"param","model"},{"message","SENSITIVE"},
            {"error",object({{"code",codes[i]},{"type","insufficient_quota"},{"param","text.format.schema"},{"message","SENSITIVE"}})}});
        const auto stream=(afterDelta?delta:String())+event("error",fields);
        diagnostic(stream,"type=error;code="+codes[i]+";param=model;error=object;error.code="+codes[i]+";error.type=insufficient_quota;error.param=text.format.schema");
        const auto ordinary=detail::parseEventStream(stream);const auto general=detail::parseStructuredEventStream(stream);
        const auto expected=i<12?codes[i]:String("sse_error");
        check(ordinary.status==Status::invalidResponse&&!ordinary.candidate&&ordinary.errorCode==expected,"nineteen diagnostic codes retain ordinary twelve-code candidate policy and withholding");
        check(general.result.status==Status::invalidResponse&&!general.result.candidate&&general.text.isEmpty()&&general.result.errorCode==expected,"nineteen diagnostic codes retain ordinary structured policy and withholding");
        const auto nestedOnly=event("error",object({{"error",object({{"code",codes[i]}})}}));
        diagnostic(nestedOnly,"type=error;code=missing;param=missing;error=object;error.code="+codes[i]+";error.type=missing;error.param=missing");
        check(detail::parseEventStream(nestedOnly).errorCode=="sse_error","nested diagnostic code never expands ordinary top-level classification");
    }
    for(const auto* type:{"insufficient_quota","rate_limit_error","service_unavailable_error","invalid_request_error"})
        diagnostic(event("error",object({{"error",object({{"type",type}})}})),String("type=error;code=missing;param=missing;error=object;error.code=missing;error.type=")+type+";error.param=missing");
    for(const auto* param:{"model","input","text.format","text.format.schema","service_tier"})
        diagnostic(event("error",object({{"param",param},{"error",object({{"param",param}})}})),String("type=error;code=missing;param=")+param+";error=object;error.code=missing;error.type=missing;error.param="+param);
    // Cross-field recognized strings are still unknown in the wrong vocabulary.
    diagnostic(event("error",object({{"code","model"},{"param","insufficient_quota"},{"error",object({{"code","rate_limit_error"},{"type","model"},{"param","slow_down"}})}})),
        "type=error;code=unrecognized;param=unrecognized;error=object;error.code=unrecognized;error.type=unrecognized;error.param=unrecognized");
    const auto error=event("error");
    for(const auto& stream:std::vector<String>{"",completed(),completed("{\"SENSITIVE\":42}"),delta,
        event("response.failed",object({{"response",object({{"error",object({{"code","insufficient_quota"}})}})}})),
        event("response.incomplete"),event("response.refusal.delta",object({{"delta","SENSITIVE"}}))+error,
        "data: []\n\n"+error,"data: {\n\n"+error,"data: {}\n\n"+error,
        "data: {\"type\":null}\n\n"+error,"data: {\"type\":\"error\",\"code\":null,\"code\":\"slow_down\"}\n\n",
        "event: response.failed\ndata: {\"type\":\"error\"}\n\n","event: error\nevent: error\ndata: {\"type\":\"error\"}\n\n",
        "SENSITIVE-invalid-line\n"+error,"data: [DONE]\n\n"+error,error.dropLastCharacters(1),
        event("response.output_text.delta",object({{"delta",42}}))+error,
        event("response.output_text.delta",object({{"delta",String::repeatedString("x",16385)}}))+error,
        event("response.output_text.done",object({{"text",String::repeatedString("x",16385)}}))+error,
        completed()+event("SENSITIVE-event")+error,
        event("response.completed",object({{"response",object({{"status","failed"},{"output",juce::Array<juce::var>{}}})}}))+error}) diagnostic(stream,{});
    diagnostic(event("response.output_text.delta",object({{"delta",String::repeatedString("x",16384)}}))+error,absent);
    diagnostic(event("response.output_text.delta",object({{"delta",String::repeatedString(String::fromUTF8("\xc3\xa9"),16384)}}))+error,absent);
    diagnostic(error.replace("\n","\r\n"),absent);
    diagnostic(": comment\nid: SENSITIVE\nretry: 100\n"+error,absent);
    diagnostic("data: {\"type\":\"error\",\ndata: \"code\":null}\n\n","type=error;code=null;param=missing;error=missing");
    diagnostic(completed()+error,absent); // Legacy error precedes completed-state rejection.
    diagnostic(error+"data: {\n\n",absent); // Legacy rejects immediately, before later bad event.
    const String exact=":"+String::repeatedString("x",4*1024*1024-int(error.getNumBytesAsUTF8())-2)+"\n"+error;
    check(exact.getNumBytesAsUTF8()==4*1024*1024,"stream-size fixture reaches exact legacy byte maximum");
    diagnostic(exact,absent);diagnostic(":"+exact,{});
    // An offline authenticated wrapper observes one ordinary candidate request;
    // Main's actual native delegation is separately reviewed, never invoked here.
    Fixture f(key);auto base=f.transport();String safe;int observations=0;
    Transport observer=[&](const HttpRequest& request,const std::atomic_bool& cancel) {
        const auto response=base(request,cancel);
        if(request.method=="POST"&&request.url=="https://api.openai.com/v1/responses"&&response.status==200&&!response.interrupted&&!cancel) {
            safe=detail::streamErrorDiagnostic(response.body);++observations;
        }
        return response;
    };
    OAuthProbe probe(f.temp.dir,observer,[&]{return f.now;});
    check(f.connect(probe).status==Status::connected&&observations==0&&safe.isEmpty(),"synthetic token and JWKS requests are outside diagnostic observation");
    f.stream=delta+event("error",object({{"code",juce::var()},{"param","text.format.schema"},{"error",object({{"type","insufficient_quota"}})}}));
    f.responseRequestId="req-shape-safe";const auto before=f.calls;
    const auto result=probe.requestStructuredCandidate("account-model");
    check(f.calls==before+1&&observations==1,"diagnostic wrapper delegates one fixed candidate request without extra inference");
    check(safe=="type=error;code=null;param=text.format.schema;error=object;error.code=missing;error.type=insufficient_quota;error.param=missing","synthetic candidate request observes safe shape independently of Result");
    check(result.status==Status::invalidResponse&&!result.candidate&&result.httpStatus==200&&result.requestId=="req-shape-safe"&&result.errorCode=="sse_error","normal candidate result retains HTTP request identity fallback and withholding");
    const auto wire=detail::strictJson(f.requests.empty()?String():f.requests.back().body);
    const auto* properties=wire["text"]["format"]["schema"]["properties"].getDynamicObject();
    check(wire["model"]==juce::var("account-model")&&wire["text"]["format"]["name"]==juce::var("synth_candidate")
        &&properties&&properties->getProperties().size()==2,"observed request preserves exact fixed two-property candidate schema");
    safe.clear();f.interrupted=true;const auto interrupted=probe.requestStructuredCandidate("account-model");
    check(interrupted.status==Status::networkFailure&&observations==1&&safe.isEmpty(),"interrupted Responses do not produce diagnostic observation");
    f.interrupted=false;f.stream=completed();const auto success=probe.requestStructuredCandidate("account-model");
    check(success.candidate.has_value()&&safe.isEmpty()&&observations==2,"successful normal candidate has no diagnostic");
    for(const auto* code:{"credit_balance_exhausted","slow_down","organization_spend_limit_exceeded","project_spend_limit_exceeded","organization_usage_limit_exceeded","server_is_overloaded","rate_limit_reached"}) {
        Fixture http(key);auto original=http.transport();OAuthProbe normal(http.temp.dir,[&](const HttpRequest& request,const std::atomic_bool& cancel) {
            if(request.url=="https://api.openai.com/v1/responses") return HttpResponse{429,json(object({{"error",object({{"code",code},{"message","SENSITIVE"}})}})),"req-http-safe",false};
            return original(request,cancel);
        },[&]{return http.now;});
        check(http.connect(normal).status==Status::connected,"HTTP diagnostic-only code fixture connects offline");
        const auto rejected=normal.requestStructuredCandidate("account-model");
        check(rejected.status==Status::networkFailure&&rejected.httpStatus==429&&rejected.errorCode.isEmpty()&&!rejected.candidate,"seven diagnostic-only codes do not expand ordinary HTTP classification");
    }
}
void structuredTextUtf8Limits() {
    const String e=String::fromUTF8("\xc3\xa9"), music=String::fromUTF8("\xf0\x9f\x8e\xb5");
    for(const auto& text:std::vector<String>{String::repeatedString("a",262144),String::repeatedString(e,131072),String::repeatedString(music,65536)}) {
        check(text.getNumBytesAsUTF8()==262144,"UTF8 fixture reaches byte boundary without unrelated invalid fields");
        auto r=detail::parseStructuredEventStream(completed(text));
        check(r.result.status==Status::connected&&r.text==text,"ASCII multibyte supplementary completed text at byte maximum accepted");
        r=detail::parseStructuredEventStream(completed(text+"a"));
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode=="sse_text_limit","UTF8 maximum plus one delta rejected");
        r=detail::parseStructuredEventStream(terminalText(text+"a"));
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode=="sse_text_limit","UTF8 terminal-only maximum plus one rejected");
        r=detail::parseStructuredEventStream(event("response.output_text.done",object({{"text",text+"a"}}))+completed("a"));
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode=="sse_text_limit","UTF8 done maximum plus one rejected before consistency checks");
    }
    auto r=detail::parseStructuredEventStream(event("response.output_text.delta",object({{"delta",music}}))+event("response.output_text.delta",object({{"delta",music}}))+emptyTerminal(),8);
    check(r.result.status==Status::connected&&r.text==music+music,"supplementary delta accumulation counts bytes with exact smaller maximum");
    r=detail::parseStructuredEventStream(event("response.output_text.delta",object({{"delta",music}}))+event("response.output_text.delta",object({{"delta",music}}))+emptyTerminal(),7);
    check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode=="sse_text_limit","supplementary accumulated bytes cannot evade smaller maximum");
    for(auto limit:{std::size_t(0),maximumStructuredOutputBytes+1}) {
        r=detail::parseStructuredEventStream(completed("a"),limit);
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode=="sse_text_limit","zero and over-policy explicit limits fail closed");
    }
    const auto text=json(proposalWire()); r=detail::parseStructuredEventStream(completed(text));
    airack::ai::ProposalEdits edits;
    check(r.text==text&&airack::ai::decodeProposal(r.text.toStdString(),edits)&&edits.characteristics==proposalWire()["characteristics"].toString(),"complete real proposal preserves explanatory Unicode through unchanged decoder");
}
void structuredFullCatalogRequest(SigningKey& key) {
    airack::ProjectDocument source; juce::var catalog;
    const bool projectValid = airack::validateProjectDocument(source);
    const bool catalogCreated = projectValid && airack::ai::catalogFor(source,catalog);
    check(projectValid&&catalogCreated,"real valid project creates full native catalog");
    std::cout << "TASK13_OAUTH_CATALOG_PRECONDITION project_valid=" << projectValid
        << " catalog_attempted=" << projectValid << " catalog_created=" << catalogCreated
        << " value_count=" << source.sound.values.size()
        << " compiled_parameter_count=" << airack::parameterCount
        << " controls=" << (catalog["controls"].isArray()?catalog["controls"].getArray()->size():-1)
        << " values_valid=" << airack::validateParameterValues(source.sound.values)
        << " sound_valid=" << airack::validateSoundDocument(source.sound)
        << " layout_valid=" << airack::validateRackLayout(source.sound.layout)
        << " compatibility_valid=" << airack::validateKernelCompatibility(source.sound.compatibility)
        << " pattern_valid=" << airack::validatePatternDocument(source.pattern)
        << " mode=" << int(source.mode) << " group_present=" << source.group.has_value() << '\n';
    airack::RoutedPatch routedDiagnostic;
    const bool routedCreated = airack::makeRoutedPatch(static_cast<const airack::SoundAudioState&>(source.sound),routedDiagnostic);
    const bool rackInstancesValid = airack::validateRackInstances(source.sound);
    bool macrosEmpty = true;
    for(const auto& binding:source.sound.macros)
        macrosEmpty = macrosEmpty && binding.targetKey==0 && binding.amount==0;
    std::cout << "TASK13_OAUTH_SOUND_SPLIT routed_created=" << routedCreated
        << " rack_instances_valid=" << rackInstancesValid
        << " personalities_present=" << bool(source.sound.personalities)
        << " macros_empty=" << macrosEmpty << '\n';
    check(catalog["controls"].isArray()&&catalog["controls"].getArray()->size()==static_cast<int>(airack::parameterCount),"request fixture carries all supported controls");
    const auto fullCatalog=catalog.clone();
    // Repeated column names must not make this transport fixture exceed the production prompt budget.
    juce::Array<juce::var> columns,rows;
    for(const auto* name:{"id","name","kind","unit","minimum","maximum","default","skew","choices","value_normalized","value_physical","locked","slot","module","active"})columns.add(name);
    if(const auto* controls=fullCatalog["controls"].getArray())for(const auto& control:*controls){
        juce::Array<juce::var> row;for(const auto& column:columns)row.add(control[juce::Identifier(column.toString())]);rows.add(row);
    }
    catalog.getDynamicObject()->setProperty("control_columns",columns);
    catalog.getDynamicObject()->setProperty("controls",rows);
    const auto text=json(proposalWire()); const auto schema=airack::ai::proposalSchema();
    const String prefix="Source: requesting instance only; scope sound; preserve pattern and locks.\n";
    const auto compactCatalog=juce::JSON::toString(catalog,true,17);
    const auto prompt=prefix+compactCatalog;
    check(prompt.getNumBytesAsUTF8()<=1024*1024,"complete compact catalog fits unchanged raw prompt limit");
    check(juce::JSON::toString(detail::strictJson(compactCatalog),true,17)==juce::JSON::toString(catalog,true,17),"compact fixture preserves the complete catalog without changing numeric values or controls");
    std::cout<<"OAUTH_FULL_CATALOG_PROMPT pretty_bytes="<<(prefix+juce::JSON::toString(catalog,false)).getNumBytesAsUTF8()<<" compact_bytes="<<prompt.getNumBytesAsUTF8()<<std::endl;
    Fixture f(key); f.stream=completed(text); auto p=f.probe(); check(f.connect(*p).status==Status::connected,"full request uses synthetic authorized session");
    f.responseRequestId="req-full-123";
    const auto r=p->requestStructured("account-model-v2:structured",prompt.toStdString(),schema);
    std::cout<<"OAUTH_FULL_CATALOG_RESULT status="<<int(r.result.status)<<" error="<<r.result.errorCode<<" requests="<<f.requests.size()<<std::endl;
    check(r.result.status==Status::connected&&r.text==text&&!r.result.candidate&&r.result.httpStatus==200&&r.result.requestId=="req-full-123","full request returns completed proposal with safe HTTP context");
    const auto req=f.requests.empty()?HttpRequest{}:f.requests.back(); const auto wire=detail::strictJson(req.body);
    check(req.method=="POST"&&req.url=="https://api.openai.com/v1/responses"&&req.contentType=="application/json"&&req.maxBytes==4*1024*1024&&req.timeoutMs==60000,"general request retains approved native Responses deadline and response limit");
    check(wire["model"]==juce::var("account-model-v2:structured")&&wire["store"].isBool()&&!bool(wire["store"])&&wire["stream"].isBool()&&bool(wire["stream"]),"account model supplied unchanged with store disabled and stream enabled");
    auto* input=wire["input"].getArray();
    check(input&&input->size()==1&&(*input)[0]["role"]==juce::var("user")&&(*input)[0]["content"]==juce::var(prompt),"one ordinary user message contains complete source scope and full host catalog");
    const auto format=wire["text"]["format"];
    check(format["type"]==juce::var("json_schema")&&format["name"]==juce::var("synth_proposal")&&format["strict"].isBool()&&bool(format["strict"])&&json(format["schema"])==json(schema),"exact native proposal schema uses strict synth proposal format");
    check(!wire.hasProperty("tools")&&!wire.hasProperty("request_id")&&!wire.hasProperty("revision"),"generic transport adds no tools or model-generated ownership authority");
    check(req.body.getNumBytesAsUTF8()>256*1024&&detail::nativeBodySizeAllowed(req),"complete real catalog traverses larger native Responses send limit");
    const auto registry=airack::parameterRegistry();
    const auto content=input&&input->size()==1?(*input)[0]["content"].toString():String();
    auto transported=detail::strictJson(content.fromFirstOccurrenceOf("\n",false,false));
    juce::Array<juce::var> restored;
    const auto* transportedColumns=transported["control_columns"].getArray();
    if(const auto* transportedRows=transported["controls"].getArray();transportedRows&&transportedColumns)
        for(const auto& value:*transportedRows)if(const auto* row=value.getArray();row&&row->size()==transportedColumns->size()){
            auto* control=new juce::DynamicObject;for(int n=0;n<row->size();++n)control->setProperty(juce::Identifier((*transportedColumns)[n].toString()),(*row)[n]);restored.add(juce::var(control));
        }
    if(auto* transportedObject=transported.getDynamicObject()){
        transportedObject->removeProperty("control_columns");transportedObject->setProperty("controls",restored);
    }
    check(juce::JSON::toString(transported,true,17)==juce::JSON::toString(fullCatalog,true,17),"column projection preserves every catalog field and exact numeric value through transport");
    const auto* controls=transported["controls"].getArray();
    check(prompt.contains("global.master_level")&&prompt.contains("synth.1.analog.filter.cutoff")&&prompt.contains("output.protection")
        &&controls&&controls->size()==static_cast<int>(registry.size())&&(*controls)[controls->size()-1]["id"]==juce::var(registry.back().id)&&double((*controls)[controls->size()-1]["maximum"])==registry.back().maximum,"current stable and last high-index controls survive complete request");
    airack::ai::ProposalEdits edits; airack::ProjectDocument admitted;
    check(airack::ai::decodeProposal(r.text.toStdString(),edits)&&airack::ai::admitProposal(source,{},edits,admitted)==airack::ai::ProposalStatus::accepted&&admitted.sound.values[0]==.25f&&admitted.pattern==source.pattern,"returned untrusted text only becomes proposal through unchanged transactional native admission");
}
juce::var sizedSchema(std::size_t bytes) {
    auto s=object({{"description",""}}); const auto overhead=json(s).getNumBytesAsUTF8();
    s.getDynamicObject()->setProperty("description",String::repeatedString("s",int(bytes-overhead))); return s;
}
std::size_t requestWireBytes(const String& prompt,const juce::var& schema) {
    return json(object({{"model","account-model"},{"store",false},{"stream",true},
        {"input",juce::Array<juce::var>{object({{"role","user"},{"content",prompt}})}},
        {"text",object({{"format",object({{"type","json_schema"},{"name","synth_proposal"},{"strict",true},{"schema",schema}})}})}})).getNumBytesAsUTF8();
}
juce::var nestedSchema(int arrays) {
    juce::var leaf="encoding only";
    while(arrays-->0) leaf=juce::Array<juce::var>{leaf};
    return object({{"nested",leaf}});
}
void structuredInputAndBodyCaps(SigningKey& key) {
    const auto schema=airack::ai::proposalSchema();
    auto rejected=[&](String model,std::string prompt,juce::var shape,const char* reason) {
        Fixture f(key); auto p=f.probe(); const auto r=p->requestStructured(model,std::string_view(prompt.data(),prompt.size()),shape);
        check(r.result.status==Status::invalidResponse&&r.text.isEmpty()&&r.result.errorCode==reason,"invalid bounded input returns literal safe error");
        check(f.calls==0&&!f.temp.dir.getChildFile("session.bin").exists()&&!f.temp.dir.getChildFile("session.lock").exists(),"invalid input rejected before store creation authentication or network");
    };
    rejected("account-model","",schema,"structured_prompt_invalid");
    rejected("unsafe/model","valid",schema,"structured_model_invalid");
    rejected("account-model",std::string(1024*1024+1,'a'),schema,"structured_prompt_invalid");
    rejected("account-model",std::string("prefix\0SENSITIVE-SUFFIX",23),schema,"structured_prompt_invalid");
    rejected("account-model",std::string("\0suffix",7),schema,"structured_prompt_invalid");
    for(const auto& bytes:std::vector<std::string>{std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xc3",1)})
        rejected("account-model",bytes,schema,"structured_prompt_invalid");
    rejected("account-model","valid",juce::Array<juce::var>{},"structured_schema_invalid");
    rejected("account-model","valid",sizedSchema(65537),"structured_schema_invalid");
    const char invalid[]={'x',char(0xc0),char(0x80)};
    const String invalidString(juce::CharPointer_UTF8(invalid),juce::CharPointer_UTF8(invalid+sizeof(invalid)));
    rejected("account-model","valid",object({{"description",invalidString}}),"structured_schema_invalid");
    rejected("account-model","valid",object({{"nested",juce::Array<juce::var>{object({{"description",invalidString}})}}}),"structured_schema_invalid");
    rejected("account-model","valid",nestedSchema(24),"structured_schema_invalid");
    rejected("account-model",std::string(350000,'\x01'),schema,"structured_request_limit");
    Fixture f(key); f.stream=completed("complete"); auto p=f.probe(); check(f.connect(*p).status==Status::connected,"input positives use same authenticated synthetic client");
    for(const auto& [prompt,shape]:std::vector<std::pair<std::string,juce::var>>{
        {std::string(1024*1024,'a'),schema},{"valid",sizedSchema(65536)},
        {"valid",nestedSchema(23)}}) {
        const auto r=p->requestStructured("account-model",prompt,shape);
        check(r.result.status==Status::connected&&r.text=="complete","exact prompt and serialized schema maximum accepted");
    }
    const auto unicode=String::repeatedString(String::fromUTF8("\xf0\x9f\x8e\xb5"),262144).toStdString();
    auto r=p->requestStructured("account-model",unicode,schema);
    const auto unicodeWire=detail::strictJson(f.requests.empty()?String():f.requests.back().body);
    const auto* unicodeInput=unicodeWire["input"].getArray();
    check(r.result.status==Status::connected&&unicodeInput&&unicodeInput->size()==1&&(*unicodeInput)[0]["content"].toString().toStdString()==unicode,"oneMiB supplementary prompt preserved byte-for-byte");
    const auto overhead=requestWireBytes("",schema), available=2097152-overhead;
    const auto exact=std::string(available/6,'\x01')+std::string(available%6,'x');
    check(exact.size()<=1048576&&requestWireBytes(String::fromUTF8(exact.data(),int(exact.size())),schema)==2097152,"escaping fixture reaches final twoMiB boundary inside raw prompt maximum");
    r=p->requestStructured("account-model",exact,schema);
    check(r.result.status==Status::connected&&!f.requests.empty()&&f.requests.back().body.getNumBytesAsUTF8()==2097152,"final escaped JSON at twoMiB accepted without truncation");
    rejected("account-model",exact+"x",schema,"structured_request_limit");
    const auto base=String::repeatedString("x",262144), large=String::repeatedString("x",2097152);
    for(const auto& [method,url]:std::vector<std::pair<String,String>>{{"POST","https://api.openai.com/v1/responses"},{"GET","https://api.openai.com/v1/responses"},{"POST","https://api.openai.com/v1/models"},{"POST","https://auth.openai.com/api/accounts/oauth/token"},{"post","https://api.openai.com/v1/responses"},{"POST","https://api.openai.com/v1/responses/"}}) {
        const auto limit=method=="POST"&&url=="https://api.openai.com/v1/responses"?large:base;
        check(detail::nativeBodySizeAllowed({method,url,limit}),"exact endpoint method body cap accepted");
        check(!detail::nativeBodySizeAllowed({method,url,limit+"x"}),"one byte beyond exact endpoint method body cap rejected");
        const auto multi=String::repeatedString(String::fromUTF8("\xc3\xa9"),int(limit.getNumBytesAsUTF8()/2));
        check(detail::nativeBodySizeAllowed({method,url,multi})&&!detail::nativeBodySizeAllowed({method,url,multi+"x"}),"native send body cap counts multibyte UTF8 bytes");
    }
}
void structuredAuthCancelErrors(SigningKey& key) {
    const auto schema=airack::ai::proposalSchema();
    auto request=[&](OAuthProbe& p) {return p.requestStructured("account-model","valid source prompt",schema);};
    auto failed=[&](const StructuredResult& r,Status s) {check(r.result.status==s&&r.text.isEmpty()&&!r.result.candidate&&!r.result.errorCode.contains("SENSITIVE"),"auth network and cancel failures never expose completed or sensitive text");};
    {Fixture f(key); auto p=f.probe(); failed(request(*p),Status::signedOut); check(f.calls==0,"signed-out general request makes no HTTP call");}
    {Fixture f(key); auto p=f.probe(); auto cb=f.begin(*p); check(p->acceptCallback(cb.upToFirstOccurrenceOf("&code",false,false)+"&error=access_denied").status==Status::denied,"synthetic browser denial retained"); failed(request(*p),Status::signedOut); check(f.tokenCalls==0,"denial cannot exchange token through general request");}
    {Fixture f(key); f.scope="openid"; auto p=f.probe(); check(f.connect(*p).status==Status::inferenceNotGranted,"structured scope fixture lacks inference permission"); const auto before=f.calls; failed(request(*p),Status::inferenceNotGranted); check(f.calls==before,"missing inference scope prevents provider request");}
    {Fixture f(key); f.stream=completed("complete"); auto p=f.probe(); check(f.connect(*p).status==Status::connected,"renewal structured session connects"); const auto before=f.tokenCalls; f.now+=3599; check(request(*p).text=="complete"&&f.tokenCalls==before,"general request does not refresh early"); f.now+=1; check(request(*p).text=="complete"&&f.tokenCalls==before+1,"expired general request rotates refresh before Responses"); f.now+=3600; check(request(*p).text=="complete"&&f.requests.size()>=3&&f.requests[f.requests.size()-3].body.contains("refresh_token=synthetic-refresh-2"),"next structured renewal uses rotated token");}
    {Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.subject="other-user"; failed(request(*p),Status::identityMismatch); check(!f.requests.empty()&&f.requests.back().url.endsWith("jwks.json"),"changed refresh identity never reaches Responses");}
    {Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.nonce="wrong-nonce"; failed(request(*p),Status::invalidIdentity);}
    {Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.failSave=true; failed(request(*p),Status::storageFailure); check(!f.requests.empty()&&!f.requests.back().url.endsWith("responses"),"failed rotated credential save prevents inference");}
    {Fixture f(key); auto p=f.probe(); f.connect(*p); f.now+=3600; f.tokenStatus=400; f.tokenError="invalid_grant"; auto r=request(*p); failed(r,Status::tokenFailure); check(r.result.httpStatus==400&&r.result.errorCode=="invalid_grant"&&r.result.requestId=="request-123"&&p->status().status==Status::signedOut,"terminal renewal retains sanitized status and clears unusable tokens");}
    {Fixture f(key); auto p=f.probe(); f.connect(*p); f.interrupted=true; failed(request(*p),Status::networkFailure); f.interrupted=false; f.stream=event("response.refusal.delta",object({{"delta","SENSITIVE"}})); f.responseRequestId="unsafe\r\nSENSITIVE"; auto r=request(*p); failed(r,Status::invalidResponse); check(r.result.errorCode=="sse_refusal"&&r.result.httpStatus==200&&r.result.requestId.isEmpty(),"refusal preserves only safe response context");}
    {Fixture f(key); auto base=f.transport(); OAuthProbe p(f.temp.dir,[&](const HttpRequest& req,const std::atomic_bool& cancel) {if(req.url.endsWith("/responses")) return HttpResponse{429,"{\"error\":{\"code\":\"rate_limit_exceeded\",\"message\":\"SENSITIVE\"}}","req-rate-123",false}; return base(req,cancel);},[&]{return f.now;}); f.connect(p); auto r=request(p); failed(r,Status::networkFailure); check(r.result.httpStatus==429&&r.result.errorCode=="rate_limit_exceeded"&&r.result.requestId=="req-rate-123","429 exposes safe known code and request context only");}
    {Fixture f(key); f.stream=completed("complete"); auto p=f.probe(); f.connect(*p); const auto before=f.calls; f.duringRequest=[&]{p->cancel();}; failed(request(*p),Status::cancelled); f.duringRequest={}; failed(request(*p),Status::cancelled); check(f.calls==before+1,"cancelled general request owner remains cancelled without further transport"); p.reset(); p=f.probe(); check(request(*p).text=="complete","fresh synthetic client reconnects after prior owner retired");}
}
void structuredTests(SigningKey& key) {
    structuredGroup("structured_stream_completion",structuredStreamCompletion);
    structuredGroup("structured_error_diagnostics",[&]{structuredErrorDiagnostics(key);});
    structuredGroup("stream_error_shapes",[&]{streamErrorShapes(key);});
    structuredGroup("structured_text_utf8_limits",structuredTextUtf8Limits);
    structuredGroup("structured_full_catalog_request",[&]{structuredFullCatalogRequest(key);});
    structuredGroup("structured_input_and_body_caps",[&]{structuredInputAndBodyCaps(key);});
    structuredGroup("structured_auth_cancel_errors",[&]{structuredAuthCancelErrors(key);});
}
void transportDiagnosticTests(SigningKey& key) {
    using Life=detail::NativeHttpLifecycle;using Event=Life::Event;
    for(int shape:{0,1,2}) {
        auto slot=std::make_shared<std::atomic_bool>(true);auto route=std::make_shared<std::weak_ptr<Life>>();
        auto state=Life::create({(void*)1,(void*)2,(void*)3},"secret-send",[route](void* h){if(h==(void*)1)if(auto s=route->lock())s->notify(Event::closing);return true;},slot);
        *route=state;state->expectCallbacks();std::atomic_bool cancel=false;
        WINHTTP_ASYNC_RESULT error{API_READ_DATA,ERROR_WINHTTP_TIMEOUT};
        state->notifyRequestError(shape==1?nullptr:&error,shape==2?sizeof(error)-1:sizeof(error));
        check(!state->wait(Event::read,cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1)),"request error retains existing failure completion semantics");
        check(state->nativeError()==(shape==0?ERROR_WINHTTP_TIMEOUT:0),"only complete non-null async result yields native error");
        state->reset();state->notify(Event::read,7);
        check(state->nativeError()==(shape==0?ERROR_WINHTTP_TIMEOUT:0),"reset and late read preserve diagnostic error");
        check(state->finish(cancel,std::chrono::steady_clock::now()+std::chrono::seconds(1))&&*slot,"diagnostic error does not change successful cleanup retirement");
        check(state->nativeError()==(shape==0?ERROR_WINHTTP_TIMEOUT:0),"cleanup preserves diagnostic error after admission retirement");
    }
    Fixture f(key);auto base=f.transport();HttpFailure phase=HttpFailure::read;
    OAuthProbe probe(f.temp.dir,[&](const HttpRequest& req,const std::atomic_bool& cancel){
        if(!req.url.endsWith("/responses"))return base(req,cancel);
        HttpResponse response{200,"SENSITIVE-RESPONSE Bearer secret-token", "req-transport-123",true};
        response.nativeError=phase==HttpFailure::read?ERROR_WINHTTP_TIMEOUT:0;
        response.receivedBytes=12345;response.failure=phase;return response;
    },[&]{return f.now;});
    check(f.connect(probe).status==Status::connected,"transport diagnostic fixture connects without native network");
    for(const auto& entry:std::initializer_list<std::pair<HttpFailure,const char*>>{
        {HttpFailure::send,"transport_send"},{HttpFailure::receive,"transport_receive"},
        {HttpFailure::responseHeaders,"transport_response_headers"},{HttpFailure::read,"transport_read"},
        {HttpFailure::bodyLimit,"transport_body_limit"},{HttpFailure::bodyEncoding,"transport_body_encoding"},
        {HttpFailure::cleanup,"transport_cleanup"},{HttpFailure::cancelled,"transport_cancelled"},{HttpFailure::deadline,"transport_deadline"}}) {
        phase=entry.first;const auto result=probe.requestStructured("account-model","secret-user-prompt",airack::ai::proposalSchema());
        check(result.result.status==Status::networkFailure&&result.result.httpStatus==200&&result.text.isEmpty(),"interrupted HTTP200 diagnostic metadata cannot publish text or change failure result");
        const auto log=airack::diagnostics::directory().getChildFile("companion.log").loadFileAsString();
        auto lines=juce::StringArray::fromLines(log);lines.removeEmptyStrings();const auto record=lines.isEmpty()?juce::var():juce::JSON::parse(lines[lines.size()-1]);
        check(record["nativeError"]==juce::var(phase==HttpFailure::read?int(ERROR_WINHTTP_TIMEOUT):0),"provider log records numeric native error even after body dropped");
        check(record["outputBytes"]==juce::var(juce::int64(12345))&&record["interrupted"]==juce::var(true),"provider log retains received byte count for interrupted transport");
        check(record["error"]==juce::var(entry.second),"provider log exposes fixed transport category including zero-native-error causes");
        check(!log.contains("SENSITIVE-RESPONSE")&&!log.contains("secret-token")&&!log.contains("secret-user-prompt")&&!log.contains("secret-send"),"transport diagnostics never expose response body prompt or bearer");
    }
}
}
int main(int argc,char** argv) {
    if(argc==2&&String(argv[1])=="--registration-recovery") {SigningKey key;registrationRecoveryTests(key);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&String(argv[1])=="--account-switch") {SigningKey key;accountSwitchTests(key);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==3&&String(argv[1])=="--lock-check") { detail::Store store{juce::File(argv[2])}; std::atomic_bool cancel=false; return store.lock(cancel)?0:17; }
    if(argc==2&&String(argv[1])=="--transport-diagnostics") {SigningKey key;transportDiagnosticTests(key);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&String(argv[1])=="--receive-timeouts") {nativeReceiveTimeoutTests();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    parserTests(); directStreamTests(); SigningKey key; identityTests(key); diagnosticTests(key); accountSwitchTests(key);registrationRecoveryTests(key);flowTests(key); storageTests(); loopbackTests(); nativeReceiveTimeoutTests(); lifecycleTests(); admissionRetirementTests(); structuredTests(key); structuredGroup("transport_diagnostics",[&]{transportDiagnosticTests(key);}); std::cout<<checks<<" checks, "<<failures<<" failures\n"; return failures?1:0;
}
