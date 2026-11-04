#include "OAuthProbe.h"
#include "GenerationLog.h"
#include "StrictJson.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace rack::oauth
{
namespace
{
using juce::String;
constexpr auto tokenEndpoint = "https://auth.openai.com/api/accounts/oauth/token";
constexpr auto jwksEndpoint = "https://auth.openai.com/.well-known/jwks.json";
constexpr auto resource = "https://api.openai.com/v1";
constexpr auto scopes = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject;
    for (auto& f : fields) o->setProperty(f.first, f.second);
    return o;
}
String json(const juce::var& v) { return juce::JSON::toString(v, true); }
String string(const juce::var& v) { return v.isString() ? v.toString() : String(); }
bool numeric(const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }
bool hasScope(const juce::var& v, const String& scope)
{
    return v.isString() && juce::StringArray::fromTokens(v.toString(), " ", "").contains(scope);
}
bool permitted(const juce::var& record)
{
    return hasScope(record["scope"], "chatgpt.tokens.use.direct") && hasScope(record["scope"], "resource.invoke");
}
String form(std::initializer_list<std::pair<const char*, String>> fields)
{
    String s;
    for (auto& f : fields) { if (s.isNotEmpty()) s += "&"; s += String(f.first) + "=" + juce::URL::addEscapeChars(f.second, true); }
    return s;
}
bool safeIdentifier(const String& value, int maximum = 256)
{
    return value.isNotEmpty() && value.length() <= maximum
        && value.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:");
}
bool accountRecords(const juce::var& record, std::vector<juce::var>& profiles)
{
    profiles.clear();const auto id=string(record["client_id"]);
    if(record.hasProperty("account_revision")&&(!(record["account_revision"].isInt()||record["account_revision"].isInt64())||static_cast<int64_t>(record["account_revision"])<0))return false;
    if(record.hasProperty("client_id")&&!safeIdentifier(id))return false;
    if(id.isNotEmpty())profiles.push_back(record);
    if(!record.hasProperty("saved_accounts"))return true;
    const auto* saved=record["saved_accounts"].getArray();if(!saved||saved->size()>=int(maximumSavedAccounts))return false;
    if(!saved->isEmpty()&&string(record["host_id"]).isEmpty())return false;
    for(const auto& profile:*saved){const auto key=string(profile["client_id"]);
        if(!profile.isObject()||!safeIdentifier(key)||profile.hasProperty("saved_accounts")||string(profile["host_id"])!=string(record["host_id"]))return false;
        for(const auto& previous:profiles)if(string(previous["client_id"])==key)return false;
        profiles.push_back(profile);
    }
    return profiles.size()<=maximumSavedAccounts;
}
juce::var standaloneAccount(const juce::var& profile,int fallback)
{
    auto copy=profile.clone();auto* o=copy.getDynamicObject();o->removeProperty("saved_accounts");o->removeProperty("account_revision");
    if(!copy["account_number"].isInt()||int(copy["account_number"])<1||int(copy["account_number"])>int(maximumSavedAccounts))o->setProperty("account_number",fallback);
    return copy;
}
Result accountStatus(const juce::var& record,int64_t now)
{
    if(string(record["access_token"]).isEmpty())return {Status::signedOut};
    if(!numeric(record["expires_at"]))return {Status::storageFailure};
    if(now>=static_cast<int64_t>(record["expires_at"]))return {Status::accessExpired};
    return {permitted(record)?Status::connected:Status::inferenceNotGranted};
}
int64_t accountRevision(const juce::var& record)
{return record.hasProperty("account_revision")?static_cast<int64_t>(record["account_revision"]):0;}
bool advanceAccountRevision(const juce::var& current,juce::var& next)
{
    const auto revision=accountRevision(current);if(revision==std::numeric_limits<int64_t>::max())return false;
    next.getDynamicObject()->setProperty("account_revision",juce::var(revision+1));return true;
}
bool activateAccount(const juce::var& current,juce::var& selected)
{
    std::vector<juce::var> profiles;if(!accountRecords(current,profiles))return false;
    juce::Array<juce::var> saved;int n=0;bool replacing=false;
    for(const auto& profile:profiles){++n;if(string(profile["client_id"])==string(selected["client_id"])){
            if(string(profile["subject"]).isNotEmpty()&&string(profile["subject"])!=string(selected["subject"]))return false;
            replacing=true;
        }else saved.add(standaloneAccount(profile,n));}
    if(!replacing&&profiles.size()>=maximumSavedAccounts)return false;
    selected.getDynamicObject()->setProperty("saved_accounts",saved);return advanceAccountRevision(current,selected);
}
bool retainRegistration(juce::var& record,const String& client)
{
    std::vector<juce::var> profiles;if(!accountRecords(record,profiles))return false;
    for(const auto& profile:profiles)if(string(profile["client_id"])==client)return true;
    if(profiles.size()>=maximumSavedAccounts)return false;
    int number=1;for(std::size_t i=0;i<profiles.size();++i)number=std::max(number,int(standaloneAccount(profiles[i],int(i)+1)["account_number"])+1);
    if(profiles.empty()){record.getDynamicObject()->setProperty("client_id",client);record.getDynamicObject()->setProperty("account_number",number);}
    else{
        juce::Array<juce::var> saved;for(std::size_t i=1;i<profiles.size();++i)saved.add(standaloneAccount(profiles[i],int(i)+1));
        saved.add(object({{"host_id",record["host_id"]},{"client_id",client},{"account_number",number}}));
        record.getDynamicObject()->setProperty("saved_accounts",saved);
    }return true;
}
bool schemaEncodingValid(const juce::var& value, int depth = 0)
{
    if(depth>24) return false;
    auto valid=[](const String& text) {
        const auto bytes=text.getNumBytesAsUTF8();
        return bytes<=maximumStructuredSchemaBytes
            && juce::CharPointer_UTF8::isValidString(text.toRawUTF8(),static_cast<int>(bytes));
    };
    if(value.isString()) return valid(value.toString());
    if(auto* o=value.getDynamicObject())
        for(auto& property:o->getProperties())
            if(!valid(property.name.toString()) || !schemaEncodingValid(property.value,depth+1)) return false;
    if(auto* a=value.getArray())
        for(auto& item:*a) if(!schemaEncodingValid(item,depth+1)) return false;
    return true;
}
bool containsRefusal(const juce::var& value)
{
    if(auto* o=value.getDynamicObject())
    {
        if(value["type"]==juce::var("refusal") || value.hasProperty("refusal")) return true;
        for(auto& property:o->getProperties()) if(containsRefusal(property.value)) return true;
    }
    else if(auto* a=value.getArray()) for(auto& item:*a) if(containsRefusal(item)) return true;
    return false;
}
String knownErrorCode(const String& code)
{
    for (const auto* known : { "invalid_grant", "invalid_client", "invalid_scope", "access_denied", "insufficient_quota",
        "rate_limit_exceeded", "server_error", "temporarily_unavailable", "invalid_token", "model_not_found",
        "permission_denied", "refresh_too_early" })
        if (code == known) return known;
    return {};
}
Result errorResult(const HttpResponse& response, Status fallback)
{
    Result r { fallback }; r.httpStatus = response.status;
    if (safeIdentifier(response.requestId, 128)) r.requestId = response.requestId;
    auto data = detail::strictJson(response.body);
    auto code = data["error"].isString() ? string(data["error"]) : string(data["error"]["code"]);
    // Only known machine codes may leave the network boundary.
    r.errorCode = knownErrorCode(code);
    return r;
}
const char* transportFailureName(HttpFailure phase)
{
    switch(phase){
        case HttpFailure::none:return "";case HttpFailure::open:return "transport_open";
        case HttpFailure::configuration:return "transport_configuration";case HttpFailure::connect:return "transport_connect";
        case HttpFailure::request:return "transport_request";case HttpFailure::requestHeaders:return "transport_request_headers";
        case HttpFailure::send:return "transport_send";case HttpFailure::receive:return "transport_receive";
        case HttpFailure::responseHeaders:return "transport_response_headers";case HttpFailure::read:return "transport_read";
        case HttpFailure::bodyLimit:return "transport_body_limit";case HttpFailure::bodyEncoding:return "transport_body_encoding";
        case HttpFailure::cleanup:return "transport_cleanup";case HttpFailure::cancelled:return "transport_cancelled";
        case HttpFailure::deadline:return "transport_deadline";
    }
    return "transport_unknown";
}
}

namespace detail
{
juce::var strictJson(const String& input)
{
    juce::var result;
    if (!airack::ai::parseStrictJson(input.toStdString(), result, 4*1024*1024)) return {};
    return result;
}
std::optional<Candidate> parseCandidate(const String& input)
{
    if (input.getNumBytesAsUTF8()>16384) return {};
    auto v = strictJson(input); auto* o = v.getDynamicObject();
    if (!o || o->getProperties().size()!=2 || !v.hasProperty("engine") || !v.hasProperty("cutoff_hz")) return {};
    auto engine = string(v["engine"]);
    if ((engine!="analog" && engine!="wavetable") || !numeric(v["cutoff_hz"])) return {};
    auto cutoff = static_cast<double>(v["cutoff_hz"]);
    if (!std::isfinite(cutoff) || cutoff < 20 || cutoff > 20000) return {};
    return Candidate { engine, cutoff };
}
Result parseModels(const String& input)
{
    Result r { Status::invalidResponse }; if (input.getNumBytesAsUTF8()>1024*1024) return r;
    auto v = strictJson(input); auto* list=v["models"].getArray(); if (!list || list->size()>1000) return r;
    std::set<String> seen;
    for (auto& m : *list)
    {
        if (!m.isObject() || !m["visibility"].isString()) return { Status::invalidResponse };
        if (string(m["visibility"])!="list") continue;
        auto slug=string(m["slug"]), name=string(m["display_name"]);
        if (!safeIdentifier(slug) || name.isEmpty() || name.length()>256 || name.containsAnyOf("\r\n\t") || !seen.insert(slug).second) return { Status::invalidResponse };
        r.models.push_back({slug,name});
    }
    r.status=Status::connected; return r;
}
namespace
{
String diagnosticField(const juce::var& parent, const char* field,
    std::initializer_list<const char*> literals, bool machineCode = false)
{
    if (!parent.hasProperty(field)) return "missing";
    const auto value=parent[field];
    if (value.isVoid()) return "null";
    if (!value.isString()) return "non_string";
    if (machineCode)
        if (const auto code=knownErrorCode(value.toString()); code.isNotEmpty()) return code;
    for (const auto* literal : literals) if (value==juce::var(literal)) return literal;
    return "unrecognized";
}
String errorDiagnostic(const juce::var& event)
{
    auto code=[](const juce::var& parent) {
        return diagnosticField(parent,"code",{"credit_balance_exhausted","slow_down",
            "organization_spend_limit_exceeded","project_spend_limit_exceeded",
            "organization_usage_limit_exceeded","server_is_overloaded","rate_limit_reached"},true);
    };
    auto param=[](const juce::var& parent) {
        return diagnosticField(parent,"param",{"model","input","text.format","text.format.schema","service_tier"});
    };
    String safe="type=error;code="+code(event)+";param="+param(event)+";error=";
    if (!event.hasProperty("error")) return safe+"missing";
    const auto nested=event["error"];
    if (nested.isVoid()) return safe+"null";
    if (!nested.getDynamicObject()) return safe+"non_object";
    return safe+"object;error.code="+code(nested)+";error.type="
        +diagnosticField(nested,"type",{"insufficient_quota","rate_limit_error","service_unavailable_error","invalid_request_error"})
        +";error.param="+param(nested);
}
StructuredResult completedEventText(const String& input, std::size_t maximumBytes, int maximumCharacters,
    String* diagnostic = nullptr)
{
    StructuredResult bad {{Status::invalidResponse},{}};
    // Every diagnostic below is an internal literal. Never echo provider event
    // types, property names, text, error bodies or parser exception details.
    auto reject=[&](const char* reason) {bad.result.errorCode=reason;return false;};
    auto withinLimit=[&](const String& text) {
        return maximumBytes ? text.getNumBytesAsUTF8()<=maximumBytes : text.length()<=maximumCharacters;
    };
    if (input.getNumBytesAsUTF8()>4*1024*1024) {reject("sse_size_limit");return bad;}
    String stream=input.replace("\r\n","\n");
    if (!stream.endsWith("\n\n")) {reject("sse_unterminated");return bad;}
    String data, eventName, accumulated, finalText; bool completed=false;
    std::optional<String> doneText;
    auto process = [&]() -> bool
    {
        if (data.isEmpty()) return true;
        if (data=="[DONE]") return completed ? true : reject("sse_done_before_completed");
        auto v=strictJson(data); auto type=string(v["type"]);
        if (!v.isObject()) return reject("sse_invalid_event_json");
        if (type.isEmpty()) return reject("sse_missing_event_type");
        if (eventName.isNotEmpty()&&eventName!=type) return reject("sse_event_type_mismatch");
        if (type=="error")
        {
            if (diagnostic) *diagnostic=errorDiagnostic(v);
            const auto code=knownErrorCode(string(v["code"]));
            return reject(code.isEmpty() ? "sse_error" : code.toRawUTF8());
        }
        if (type=="response.failed") return reject("sse_failed");
        if (type=="response.incomplete") return reject("sse_incomplete");
        if (type.contains("refusal") || containsRefusal(v)) return reject("sse_refusal");
        if (completed) return reject("sse_event_after_completed");
        if (type=="response.output_text.delta")
        {
            if (!v["delta"].isString()) return reject("sse_invalid_delta");
            accumulated+=string(v["delta"]);
            return withinLimit(accumulated) ? true : reject("sse_text_limit");
        }
        if (type=="response.output_text.done")
        {
            if (!v["text"].isString()) return reject("sse_invalid_done_text");
            auto text=string(v["text"]);
            if (!withinLimit(text)) return reject("sse_text_limit");
            if (doneText && *doneText!=text) return reject("sse_done_text_mismatch");
            doneText=text;
        }
        if (type=="response.completed")
        {
            auto response=v["response"];
            if (string(response["status"])!="completed") return reject("sse_terminal_status");
            if (!response["output"].isArray()) return reject("sse_terminal_output");
            bool hasMessage=false;
            for (auto& item : *response["output"].getArray())
            {
                if (string(item["type"])!="message") continue;
                hasMessage=true;
                if (!item["content"].isArray()) return reject("sse_message_content");
                for (auto& part : *item["content"].getArray())
                {
                    if (string(part["type"])!="output_text" || !part["text"].isString()) return reject("sse_output_part");
                    finalText+=string(part["text"]);
                    if(maximumBytes && !withinLimit(finalText)) return reject("sse_text_limit");
                }
            }
            // The direct route can finish with an empty output array after text
            // deltas. Completion authorizes validation, never early publication.
            if (response["output"].getArray()->isEmpty()) finalText=accumulated;
            if (finalText.isEmpty())
            {
                const bool delta=accumulated.isNotEmpty();
                if (response["output"].getArray()->isEmpty())
                    return reject("sse_empty_output_no_delta_terminal_empty");
                if (hasMessage)
                    return reject(delta ? "sse_empty_output_delta_present_terminal_message" : "sse_empty_output_no_delta_terminal_message");
                return reject(delta ? "sse_empty_output_delta_present_terminal_non_message" : "sse_empty_output_no_delta_terminal_non_message");
            }
            if (accumulated.isNotEmpty()&&accumulated!=finalText) return reject("sse_text_mismatch");
            if (doneText && *doneText!=finalText) return reject("sse_done_text_mismatch");
            completed=true;
        }
        return true;
    };
    for (auto line : juce::StringArray::fromLines(stream))
    {
        if (line.isEmpty()) { if (!process()) return bad; data.clear(); eventName.clear(); }
        else if (line.startsWith("data:")) { if(data.isNotEmpty()) data+="\n"; data+=line.substring(5).trimStart(); }
        else if (line.startsWith("event:")) { if(eventName.isNotEmpty()) {reject("sse_duplicate_event_field");return bad;} eventName=line.substring(6).trim(); }
        else if (!line.startsWithChar(':') && !line.startsWith("id:") && !line.startsWith("retry:")) {reject("sse_invalid_line");return bad;}
    }
    if (!completed) {reject("sse_missing_completed");return bad;}
    if (!data.isEmpty()) {reject("sse_pending_data");return bad;}
    return {{Status::connected},finalText};
}
}
StructuredResult parseStructuredEventStream(const String& input, std::size_t maximumTextBytes)
{
    if(maximumTextBytes==0 || maximumTextBytes>maximumStructuredOutputBytes)
    {
        StructuredResult bad {{Status::invalidResponse},{}}; bad.result.errorCode="sse_text_limit"; return bad;
    }
    return completedEventText(input,maximumTextBytes,0);
}
String streamErrorDiagnostic(const String& input)
{
    String safe;
    completedEventText(input,0,16384,&safe);
    return safe;
}
Result parseEventStream(const String& input)
{
    auto text=completedEventText(input,0,16384);
    if(text.result.status!=Status::connected) return text.result;
    auto candidate=parseCandidate(text.text);
    if(!candidate) {text.result.status=Status::invalidResponse;text.result.errorCode="candidate_invalid";}
    else text.result.candidate=candidate;
    return text.result;
}
}

struct OAuthProbe::Impl
{
    juce::File directory; Transport transport; Clock clock; std::atomic_bool cancelled {false};
    struct Attempt { String state, nonce, verifier, redirect, client, host; int64_t created {}; SignInMode mode=SignInMode::existingAccount;String previousClient,previousSubject;int64_t previousRevision=0; };
    std::optional<Attempt> attempt;
    Impl(juce::File dir, Transport t, Clock c) : directory(std::move(dir)), transport(t ? std::move(t) : detail::nativeHttp),
        clock(c ? std::move(c) : [] {return juce::Time::currentTimeMillis()/1000;}) {}
    Result load(detail::Store& store, juce::var& record)
    {
        if (cancelled) return {Status::cancelled};
        if (!store.lock(cancelled)) return {cancelled ? Status::cancelled : Status::busy};
        if (!store.load(record)) return {Status::storageFailure};
        if (record.isVoid()) record=object({});
        std::vector<juce::var> profiles;return {record.isObject()&&accountRecords(record,profiles)?Status::ready:Status::storageFailure};
    }
    HttpResponse http(String method, String url, String body={}, String bearer={}, size_t max=256*1024, int timeout=30000)
    {
        HttpRequest request {method,url,body,bearer,body.isEmpty()?String():String(url==tokenEndpoint?"application/x-www-form-urlencoded":"application/json"),max,timeout};
        if (cancelled) return {0,{}, {},true};
        auto response=transport(request,cancelled);
        if (cancelled || response.body.getNumBytesAsUTF8()>max) return {0,{}, {},true}; return response;
    }
    Result tokens(const HttpResponse& response, juce::var& record, const String& nonce, bool refresh)
    {
        if (cancelled) return {Status::cancelled};
        if (response.interrupted || response.status==0 || response.status>=500 || response.status==429) return errorResult(response,Status::networkFailure);
        if (response.status!=200) return errorResult(response,Status::tokenFailure);
        auto v=detail::strictJson(response.body); auto access=string(v["access_token"]), renewal=string(v["refresh_token"]), id=string(v["id_token"]); auto expires=v["expires_in"];
        if (access.isEmpty() || access.length()>16384 || access.containsAnyOf("\r\n") || renewal.isEmpty() || renewal.length()>16384
            || !numeric(expires) || !std::isfinite(static_cast<double>(expires)) || static_cast<double>(expires)<1
            || static_cast<double>(expires)>86400*31 || string(v["token_type"]).toLowerCase()!="bearer") return {Status::tokenFailure};
        String subject=string(record["subject"]);
        if (!refresh || id.isNotEmpty())
        {
            auto keys=http("GET",jwksEndpoint); if (keys.status!=200 || keys.interrupted) return errorResult(keys,Status::networkFailure);
            String verified, expectedNonce=nonce;
            // Refreshed ID tokens may omit nonce; a present nonce still must match.
            if (refresh)
            {
                auto parts=juce::StringArray::fromTokens(id,".",""); if(parts.size()!=3) return {Status::invalidIdentity};
                juce::MemoryOutputStream decoded; auto payload=parts[1].replaceCharacter('-','+').replaceCharacter('_','/');
                while(payload.length()%4) payload+="=";
                if(!juce::Base64::convertFromBase64(decoded,payload)) return {Status::invalidIdentity};
                if(!detail::strictJson(decoded.toUTF8()).hasProperty("nonce")) expectedNonce.clear();
            }
            if (!detail::verifyIdentity(id,keys.body,string(record["client_id"]),expectedNonce,clock(),verified)) return {Status::invalidIdentity};
            if (subject.isNotEmpty() && subject!=verified) return {Status::identityMismatch}; subject=verified;
            auto parts=juce::StringArray::fromTokens(id,".","");juce::MemoryOutputStream decoded;
            auto payload=parts[1].replaceCharacter('-','+').replaceCharacter('_','/');while(payload.length()%4)payload+="=";
            if(!juce::Base64::convertFromBase64(decoded,payload))return {Status::invalidIdentity};
            const auto claims=detail::strictJson(decoded.toUTF8());const auto email=string(claims["email"]);
            record.getDynamicObject()->setProperty("id_token",id);
            if(email.isNotEmpty()&&email.length()<=254&&!email.containsAnyOf("\r\n"))record.getDynamicObject()->setProperty("email",email);
        }
        if (subject.isEmpty()) return {Status::invalidIdentity};
        if (v.hasProperty("scope") && !v["scope"].isString()) return {Status::tokenFailure};
        auto granted=v.hasProperty("scope")?v["scope"]:record["scope"];
        if (!refresh && !v.hasProperty("scope")) return {Status::tokenFailure};
        auto* o=record.getDynamicObject();
        o->setProperty("subject",subject); o->setProperty("access_token",access); o->setProperty("refresh_token",renewal);
        o->setProperty("scope",granted); o->setProperty("expires_at",juce::var(clock()+static_cast<int64_t>(expires))); o->setProperty("nonce",nonce);
        if(v.hasProperty("earliest_refresh_at")) o->setProperty("earliest_refresh_at",v["earliest_refresh_at"]);
        return {permitted(record)?Status::connected:Status::inferenceNotGranted};
    }
    Result ensureAccess(detail::Store& store, juce::var& record)
    {
        auto loaded=load(store,record); if(loaded.status!=Status::ready) return loaded;
        if(string(record["access_token"]).isEmpty()) return {Status::signedOut};
        if(!permitted(record)) return {Status::inferenceNotGranted};
        if(!numeric(record["expires_at"])) return {Status::storageFailure};
        if(clock()<static_cast<int64_t>(record["expires_at"])) return {Status::connected};
        auto response=http("POST",tokenEndpoint,form({{"grant_type","refresh_token"},{"client_id",string(record["client_id"])},{"refresh_token",string(record["refresh_token"])},{"resource",resource}}));
        auto next=record.clone(); auto result=tokens(response,next,string(record["nonce"]),true);
        if(result.status==Status::connected || result.status==Status::inferenceNotGranted)
        {
            if(cancelled) return {Status::cancelled}; if(!store.save(next)) return {Status::storageFailure}; record=next;
        }
        else if(result.status==Status::tokenFailure && (result.errorCode=="invalid_grant" || result.errorCode=="invalid_client"))
        {
            auto* o=record.getDynamicObject(); o->removeProperty("access_token"); o->removeProperty("refresh_token"); o->removeProperty("expires_at");
            if(!store.save(record)) return {Status::storageFailure};
        }
        return result;
    }
};
OAuthProbe::OAuthProbe(juce::File dir,Transport t,Clock c) : impl(std::make_unique<Impl>(std::move(dir),std::move(t),std::move(c))) {}
OAuthProbe::~OAuthProbe() = default;
void OAuthProbe::cancel() { impl->cancelled=true; }
SignIn OAuthProbe::beginSignIn(int port, SignInMode mode,const String& savedAccount)
{
    impl->cancelled=false; impl->attempt.reset();
    if(port<1 || port>65535) return {{Status::invalidCallback},{}};
    detail::Store store(impl->directory); juce::var record;
    auto result=impl->load(store,record); if(result.status!=Status::ready) return {result,{}};
    std::vector<juce::var> profiles;if(!accountRecords(record,profiles))return {{Status::storageFailure},{}};
    if(mode==SignInMode::addAccount&&profiles.size()>=maximumSavedAccounts)return {{Status::storageFailure,{}, {},0,"account_limit"},{}};
    auto selected=record;
    if(savedAccount.isNotEmpty()){
        if(mode!=SignInMode::existingAccount)return {{Status::invalidIdentity},{}};
        const auto found=std::find_if(profiles.begin(),profiles.end(),[&](const auto& p){return string(p["client_id"])==savedAccount;});
        if(found==profiles.end())return {{Status::invalidIdentity},{}};selected=*found;
    }
    auto host=string(record["host_id"]);
    if(host.isEmpty())
    {
        auto random=detail::randomUrlToken(32); if(random.isEmpty()) return {{Status::storageFailure},{}};
        auto digest=detail::sha256Url(random); juce::MemoryOutputStream bytes;
        auto b64=digest.replaceCharacter('-','+').replaceCharacter('_','/')+"=";
        if(!juce::Base64::convertFromBase64(bytes,b64) || bytes.getDataSize()<16) return {{Status::storageFailure},{}};
        unsigned char uuid[16]; std::memcpy(uuid,bytes.getData(),16); uuid[6]=(uuid[6]&15)|64; uuid[8]=(uuid[8]&63)|128;
        host="urn:uuid:"+juce::Uuid(uuid).toDashedString(); record.getDynamicObject()->setProperty("host_id",host);
        if(!store.save(record)) return {{Status::storageFailure},{}};
    }
    Impl::Attempt a {detail::randomUrlToken(32),detail::randomUrlToken(32),detail::randomUrlToken(32),
        "http://127.0.0.1:"+String(port)+"/auth/callback",mode==SignInMode::addAccount?String():string(selected["client_id"]),host,impl->clock()};
    a.mode=mode;a.previousClient=string(record["client_id"]);a.previousSubject=string(record["subject"]);
    a.previousRevision=accountRevision(record);
    if(a.state.isEmpty()||a.nonce.isEmpty()||a.verifier.isEmpty()) return {{Status::storageFailure},{}};
    auto params=form({{"response_type","code"},{"client_id",a.client.isEmpty()?String("dynamic_agent_client"):a.client},
        {"redirect_uri",a.redirect},{"resource",resource},{"scope",scopes},{"state",a.state},{"nonce",a.nonce},
        {"code_challenge",detail::sha256Url(a.verifier)},{"code_challenge_method","S256"},{"ext_agent_host_id",host}});
    if(a.client.isEmpty()) params+="&agent_name_hint=air";
    else if(string(selected["id_token"]).isNotEmpty())params+="&id_token_hint="+juce::URL::addEscapeChars(string(selected["id_token"]),true);
    impl->attempt=a; return {{Status::awaitingCallback},"https://auth.openai.com/api/accounts/authorize?"+params};
}
Result OAuthProbe::acceptCallback(const String& target)
{
    auto attempt=std::move(impl->attempt); impl->attempt.reset();
    if(impl->cancelled) return {Status::cancelled}; if(!attempt) return {Status::invalidCallback}; auto& a=*attempt;
    if(impl->clock()<a.created || impl->clock()-a.created>300) return {Status::expiredAttempt};
    if(target.length()>8192 || !target.startsWith("/auth/callback?") || target.containsAnyOf("#\r\n ")) return {Status::invalidCallback};
    for(int i=0;i<target.length();++i)
    {
        if(target[i]=='%')
        {
            if(i+2>=target.length() || !String::charToString(target[i+1]).containsOnly("0123456789abcdefABCDEF")
                || !String::charToString(target[i+2]).containsOnly("0123456789abcdefABCDEF")) return {Status::invalidCallback};
            i+=2;
        }
    }
    std::map<String,String> params;
    for(auto part:juce::StringArray::fromTokens(target.fromFirstOccurrenceOf("?",false,false),"&",""))
    {
        if(!part.containsChar('=')) return {Status::invalidCallback};
        auto key=juce::URL::removeEscapeChars(part.upToFirstOccurrenceOf("=",false,false));
        auto value=juce::URL::removeEscapeChars(part.fromFirstOccurrenceOf("=",false,false));
        if(!params.emplace(key,value).second) return {Status::invalidCallback};
    }
    if(params["state"]!=a.state) return {Status::invalidCallback};
    if(params.count("error")) return {params["error"]=="access_denied"?Status::denied:Status::invalidCallback};
    auto client=params["client_id"]; if(client.isEmpty()) client=a.client;
    if(!safeIdentifier(client) || client=="dynamic_agent_client" || (a.client.isNotEmpty()&&a.client!=client)
        || params["code"].isEmpty() || params["code"].length()>4096) return {Status::invalidCallback};
    detail::Store store(impl->directory); juce::var record;
    auto loaded=impl->load(store,record); if(loaded.status!=Status::ready) return loaded;
    if(string(record["host_id"])!=a.host || string(record["client_id"])!=a.previousClient || string(record["subject"])!=a.previousSubject || accountRevision(record)!=a.previousRevision
        || (a.mode==SignInMode::existingAccount&&a.client.isNotEmpty()&&a.client!=client)) return {Status::invalidCallback};
    const bool switching=a.mode==SignInMode::addAccount||a.client!=a.previousClient;
    juce::var selected=record;
    if(a.mode==SignInMode::existingAccount&&switching){std::vector<juce::var> profiles;if(!accountRecords(record,profiles))return {Status::storageFailure};
        const auto found=std::find_if(profiles.begin(),profiles.end(),[&](const auto& p){return string(p["client_id"])==client;});
        if(found==profiles.end())return {Status::invalidCallback};selected=standaloneAccount(*found,1);
    }
    if(!switching){record.getDynamicObject()->setProperty("client_id",client);if(!store.save(record))return {Status::storageFailure};selected=record;}
    // Registration succeeded before code redemption; retain it for a fresh retry without activating an unverified identity.
    else if(!retainRegistration(record,client)||!store.save(record))return {Status::storageFailure};
    auto response=impl->http("POST",tokenEndpoint,form({{"grant_type","authorization_code"},{"code",params["code"]},
        {"client_id",client},{"code_verifier",a.verifier},{"redirect_uri",a.redirect},{"resource",resource}}));
    auto next=a.mode==SignInMode::addAccount?object({{"host_id",a.host},{"client_id",client}}):selected.clone();
    auto result=impl->tokens(response,next,a.nonce,false);
    if(result.status==Status::inferenceNotGranted && permitted(record) && string(record["access_token"]).isNotEmpty()) return result;
    if(result.status==Status::connected || result.status==Status::inferenceNotGranted)
    {
        if(switching){
            if(result.status!=Status::connected)return result;
            std::vector<juce::var> profiles;if(!accountRecords(record,profiles))return {Status::storageFailure};
            if(a.mode==SignInMode::addAccount){int number=1;for(std::size_t i=0;i<profiles.size();++i)number=std::max(number,int(standaloneAccount(profiles[i],int(i)+1)["account_number"])+1);
                for(std::size_t i=0;i<profiles.size();++i)if(string(profiles[i]["client_id"])==client)number=int(standaloneAccount(profiles[i],int(i)+1)["account_number"]);
                next.getDynamicObject()->setProperty("account_number",number);}
            if(!activateAccount(record,next))return {Status::identityMismatch};
        }
        else if(!advanceAccountRevision(record,next))return {Status::storageFailure};
        if(impl->cancelled) return {Status::cancelled}; if(!store.save(next)) return {Status::storageFailure};
    }
    return result;
}
Result OAuthProbe::status()
{
    detail::Store store(impl->directory); juce::var record;
    auto result=impl->load(store,record); if(result.status!=Status::ready) return result;
    return accountStatus(record,impl->clock());
}
Result OAuthProbe::savedAccounts(std::vector<SavedAccount>& out,String& active)
{
    detail::Store store(impl->directory);juce::var record;const auto loaded=impl->load(store,record);if(loaded.status!=Status::ready)return loaded;
    std::vector<juce::var> profiles;if(!accountRecords(record,profiles))return {Status::storageFailure};
    std::vector<SavedAccount> summaries;for(std::size_t i=0;i<profiles.size();++i){const auto profile=standaloneAccount(profiles[i],int(i)+1);
        const auto email=string(profile["email"]).substring(0,80);const auto label="Account "+String(int(profile["account_number"]))+(string(profile["subject"]).isEmpty()?String(" - finish sign-in"):email.isEmpty()?String():" - "+email);
        summaries.push_back({string(profile["client_id"]),label});}
    out=std::move(summaries);active=string(record["client_id"]);return accountStatus(record,impl->clock());
}
Result OAuthProbe::selectAccount(const String& id)
{
    if(!safeIdentifier(id))return {Status::invalidIdentity};
    detail::Store store(impl->directory);juce::var record;const auto loaded=impl->load(store,record);if(loaded.status!=Status::ready)return loaded;
    std::vector<juce::var> profiles;if(!accountRecords(record,profiles))return {Status::storageFailure};
    for(std::size_t i=0;i<profiles.size();++i)if(string(profiles[i]["client_id"])==id){
        auto next=standaloneAccount(profiles[i],int(i)+1);const auto status=accountStatus(next,impl->clock());if(status.status!=Status::connected)return status;
        if(!activateAccount(record,next))return {Status::storageFailure};
        if(impl->cancelled)return {Status::cancelled};if(!store.save(next))return {Status::storageFailure};return accountStatus(next,impl->clock());}
    return {Status::invalidIdentity};
}
Result OAuthProbe::listModels()
{
    detail::Store store(impl->directory); juce::var record;
    auto result=impl->ensureAccess(store,record); if(result.status!=Status::connected) return result;
    auto response=impl->http("GET",String(resource)+"/models",{},string(record["access_token"]),1024*1024);
    if(impl->cancelled) return {Status::cancelled};
    if(response.status!=200 || response.interrupted) return errorResult(response,Status::networkFailure);
    return detail::parseModels(response.body);
}
Result OAuthProbe::requestStructuredCandidate(const String& model)
{
    if(!safeIdentifier(model)) return {Status::invalidResponse};
    detail::Store store(impl->directory); juce::var record;
    auto result=impl->ensureAccess(store,record); if(result.status!=Status::connected) return result;
    auto properties=object({{"engine",object({{"type","string"},{"enum",juce::Array<juce::var>{"analog","wavetable"}}})},{"cutoff_hz",object({{"type","number"}})}});
    auto schema=object({{"type","object"},{"properties",properties},{"required",juce::Array<juce::var>{"engine","cutoff_hz"}},{"additionalProperties",false}});
    auto format=object({{"type","json_schema"},{"name","synth_candidate"},{"strict",true},{"schema",schema}});
    auto message=object({{"role","user"},{"content","Choose a warm, soft synth pad: engine analog or wavetable, cutoff_hz between 20 and 20000. Return one candidate."}});
    auto request=object({{"model",model},{"store",false},{"stream",true},{"input",juce::Array<juce::var>{message}},{"text",object({{"format",format}})}});
    auto response=impl->http("POST",String(resource)+"/responses",json(request),string(record["access_token"]),4*1024*1024,60000);
    if(impl->cancelled) return {Status::cancelled};
    if(response.status!=200 || response.interrupted) return errorResult(response,Status::networkFailure);
    auto parsed=detail::parseEventStream(response.body);
    parsed.httpStatus=response.status;
    if(safeIdentifier(response.requestId,128)) parsed.requestId=response.requestId;
    return parsed;
}
StructuredResult OAuthProbe::requestStructured(const String& model, std::string_view utf8Prompt, const juce::var& schema)
{
    auto invalid=[](const char* reason) {
        StructuredResult bad {{Status::invalidResponse},{}}; bad.result.errorCode=reason; return bad;
    };
    if(!safeIdentifier(model)) return invalid("structured_model_invalid");
    if(utf8Prompt.empty() || utf8Prompt.size()>maximumStructuredPromptBytes
        || utf8Prompt.find('\0')!=std::string_view::npos
        || !juce::CharPointer_UTF8::isValidString(utf8Prompt.data(),static_cast<int>(utf8Prompt.size())))
        return invalid("structured_prompt_invalid");
    if(!schema.getDynamicObject() || !schemaEncodingValid(schema) || json(schema).getNumBytesAsUTF8()>maximumStructuredSchemaBytes)
        return invalid("structured_schema_invalid");
    const auto prompt=String::fromUTF8(utf8Prompt.data(),static_cast<int>(utf8Prompt.size()));
    const auto format=object({{"type","json_schema"},{"name","synth_proposal"},{"strict",true},{"schema",schema}});
    const auto message=object({{"role","user"},{"content",prompt}});
    const auto request=object({{"model",model},{"store",false},{"stream",true},{"input",juce::Array<juce::var>{message}},
        {"text",object({{"format",format}})}});
    const auto body=json(request);
    if(body.getNumBytesAsUTF8()>maximumStructuredRequestBytes) return invalid("structured_request_limit");
    detail::Store store(impl->directory); juce::var record;
    auto result=impl->ensureAccess(store,record); if(result.status!=Status::connected) return {result,{}};
    const auto began=std::chrono::steady_clock::now();
    auto response=impl->http("POST",String(resource)+"/responses",body,string(record["access_token"]),4*1024*1024,60000);
    try{airack::diagnostics::Event event;event.component=airack::diagnostics::Component::companion;event.stage=airack::diagnostics::Stage::provider;
        event.model=model;event.httpStatus=response.status;event.providerRequestId=response.requestId;event.interrupted=response.interrupted;
        event.nativeError=response.nativeError;event.outputBytes=response.receivedBytes;event.errorCode=transportFailureName(response.failure);
        event.elapsedMs=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count());
        airack::diagnostics::write(event);}catch(...){}
    if(impl->cancelled) return {{Status::cancelled},{}};
    if(response.status!=200 || response.interrupted) return {errorResult(response,Status::networkFailure),{}};
    auto parsed=detail::parseStructuredEventStream(response.body);
    parsed.result.httpStatus=response.status;
    if(safeIdentifier(response.requestId,128)) parsed.result.requestId=response.requestId;
    return parsed;
}
const char* statusName(Status s)
{
    switch(s)
    {
        case Status::ready:return "ready"; case Status::signedOut:return "signed_out"; case Status::awaitingCallback:return "awaiting_callback";
        case Status::connected:return "connected"; case Status::denied:return "denied"; case Status::cancelled:return "cancelled";
        case Status::invalidCallback:return "invalid_callback"; case Status::expiredAttempt:return "expired_attempt";
        case Status::tokenFailure:return "token_failure"; case Status::invalidIdentity:return "invalid_identity";
        case Status::identityMismatch:return "identity_mismatch"; case Status::inferenceNotGranted:return "inference_not_granted";
        case Status::networkFailure:return "network_failure"; case Status::invalidResponse:return "invalid_response";
        case Status::accessExpired:return "access_expired"; case Status::storageFailure:return "storage_failure"; case Status::busy:return "busy";
    }
    return "unknown";
}
}
