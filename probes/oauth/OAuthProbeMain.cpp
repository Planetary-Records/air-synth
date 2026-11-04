#include "OAuthProbe.h"
#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <mutex>

namespace
{
std::atomic_bool cancelled=false;
std::mutex probeMutex;
rack::oauth::OAuthProbe* activeProbe {};
BOOL WINAPI control(DWORD type)
{
    if(type!=CTRL_C_EVENT && type!=CTRL_BREAK_EVENT) return FALSE;
    cancelled=true;
    std::lock_guard lock(probeMutex);
    if(activeProbe) activeProbe->cancel();
    return TRUE;
}
int print(const rack::oauth::Result& result, const juce::String& diagnostic = {})
{
    using namespace rack::oauth;
    auto* data=new juce::DynamicObject;
    juce::var output(data);
    data->setProperty("status",statusName(result.status));
    if(result.httpStatus) data->setProperty("http_status",result.httpStatus);
    if(result.errorCode.isNotEmpty()) data->setProperty("error_code",result.errorCode);
    if(result.requestId.isNotEmpty()) data->setProperty("request_id",result.requestId);
    if(result.status==Status::invalidResponse&&!result.candidate&&diagnostic.isNotEmpty())
        data->setProperty("provider_error_diagnostic",diagnostic);
    if(result.candidate)
    {
        auto* candidate=new juce::DynamicObject;
        candidate->setProperty("engine",result.candidate->engine);
        candidate->setProperty("cutoff_hz",result.candidate->cutoffHz);
        data->setProperty("candidate",juce::var(candidate));
    }
    if(!result.models.empty())
    {
        juce::Array<juce::var> models;
        for(auto& m:result.models)
        {
            auto* model=new juce::DynamicObject;
            model->setProperty("slug",m.slug); model->setProperty("display_name",m.displayName);
            models.add(juce::var(model));
        }
        data->setProperty("models",models);
    }
    std::cout<<juce::JSON::toString(output,true)<<'\n';
    return result.status==Status::connected || result.status==Status::signedOut ? 0:1;
}
}
int main(int argc,char** argv)
{
    using namespace rack::oauth;
    const juce::String action=argc>1?argv[1]:"--help";
    if(action=="--help")
    {
        std::cout<<"AI Rack Synth native OAuth feasibility probe\n"
            "  --help                 Show help; no credential storage access\n"
            "  --sign-in              Start loopback, then open default browser for consent\n"
            "  --status               Inspect protected saved-session status (no network)\n"
            "  --models               List models available to the saved account\n"
            "  --request --model SLUG Request one fixed musical JSON candidate\n"
            "  --diagnose --model SLUG Same request with fixed safe error shapes\n"
            "Ctrl+C cancels. Credentials stay in app-owned LocalAppData protected storage.\n"
            "Requests refresh only after saved access expiry; no early-refresh policy.\n";
        return 0;
    }
    if(!((argc==2&&(action=="--sign-in"||action=="--status"||action=="--models"))
        ||(argc==4&&(action=="--request"||action=="--diagnose")&&juce::String(argv[2])=="--model")))
    {
        std::cerr<<"Invalid arguments; use --help.\n"; return 2;
    }
    PWSTR local {};
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))) return print({Status::storageFailure});
    auto directory=juce::File(juce::String(local)).getChildFile("AI Rack Synth/OAuthProbe"); CoTaskMemFree(local);
    // Safe text and transport live through the probe and its cancellation owner.
    juce::String diagnostic;
    Transport diagnosticTransport;
    if(action=="--diagnose") diagnosticTransport=[&diagnostic](const HttpRequest& request,const std::atomic_bool& cancel) {
        const auto response=detail::nativeHttp(request,cancel);
        if(request.method=="POST"&&request.url=="https://api.openai.com/v1/responses"
            &&response.status==200&&!response.interrupted&&!cancel)
            diagnostic=detail::streamErrorDiagnostic(response.body);
        return response;
    };
    OAuthProbe probe(directory,std::move(diagnosticTransport));
    {std::lock_guard lock(probeMutex);activeProbe=&probe;}
    SetConsoleCtrlHandler(control,TRUE);
    Result result;
    if(action=="--status") result=probe.status();
    else if(action=="--models") result=probe.listModels();
    else if(action=="--request"||action=="--diagnose") result=probe.requestStructuredCandidate(argv[3]);
    else
    {
        detail::Loopback listener;
        auto signIn=probe.beginSignIn(listener.port());
        result=signIn.result;
        if(result.status==Status::awaitingCallback)
        {
            // Authorization URL is never printed. It contains state/nonce/challenge,
            // not access tokens, refresh tokens, an ID-token hint or a client secret.
            if(!juce::URL(signIn.browserUrl).launchInDefaultBrowser()) result={Status::networkFailure};
            else
            {
                std::cout<<"Complete consent in your browser. Waiting for local callback; Ctrl+C cancels.\n"<<std::flush;
                auto target=listener.receive(cancelled);
                result=cancelled?Result{Status::cancelled}:target.isEmpty()?Result{Status::expiredAttempt}:probe.acceptCallback(target);
            }
        }
    }
    {std::lock_guard lock(probeMutex);activeProbe=nullptr;}
    SetConsoleCtrlHandler(control,FALSE);
    return print(result,diagnostic);
}
