#include "GenerationLog.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <mutex>

namespace airack::diagnostics
{
namespace
{
struct Handle { HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);} };
const char* stageName(Stage stage)
{
    switch(stage){case Stage::startup:return "startup";case Stage::launch:return "launch";case Stage::connection:return "connection";
        case Stage::account:return "account";case Stage::capture:return "capture";case Stage::backend:return "backend";case Stage::provider:return "provider";
        case Stage::parse:return "parse";case Stage::admission:return "admission";case Stage::render:return "render";case Stage::terminal:return "terminal";}
    return "unknown";
}
bool identifier(const juce::String& value,int maximum)
{return value.isNotEmpty()&&value.length()<=maximum&&value.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:-");}
juce::String safeModel(const juce::String& value)
{return identifier(value,256)&&!value.startsWithIgnoreCase("sk-")&&!value.startsWithIgnoreCase("Bearer")?value:juce::String();}
juce::String safeRequestId(const juce::String& value)
{
    const auto hex=value.removeCharacters("-");
    const bool uuid=value.length()==36&&value[8]=='-'&&value[13]=='-'&&value[18]=='-'&&value[23]=='-'
        &&hex.length()==32&&hex.containsOnly("0123456789abcdefABCDEF");
    return uuid||(identifier(value,128)&&(value.startsWith("req_")||value.startsWith("req-")))?value:juce::String();
}
}
juce::File directory() noexcept
{
    try{
#if defined(_DEBUG)
        // Keeps automated native tests and their subprocesses out of real user data.
        const auto test=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
        if(test.isNotEmpty()&&juce::File::isAbsolutePath(test))return juce::File(test);
#endif
        PWSTR path=nullptr;const auto status=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&path);
        struct Allocation { PWSTR value;~Allocation(){CoTaskMemFree(value);} } allocation{path};
        if(FAILED(status)||!path||!*path)return {};
        return juce::File(juce::String(path)).getChildFile("AI Rack Synth").getChildFile("Logs");
    }catch(...){return {};}
}
juce::String safeErrorCode(const juce::String& code)
{
    if(code.isEmpty())return {};
    // Never admit free-form error messages or bodies, even if they look like slugs.
    for(const auto* known:{
        "invalid_grant","invalid_client","invalid_scope","access_denied","insufficient_quota","rate_limit_exceeded",
        "server_error","temporarily_unavailable","invalid_token","model_not_found","permission_denied","refresh_too_early",
        "structured_model_invalid","structured_prompt_invalid","structured_schema_invalid","structured_request_limit",
        "sse_size_limit","sse_unterminated","sse_done_before_completed","sse_invalid_event_json","sse_missing_event_type",
        "sse_event_type_mismatch","sse_error","sse_failed","sse_incomplete","sse_refusal","sse_event_after_completed",
        "sse_invalid_delta","sse_text_limit","sse_invalid_done_text","sse_done_text_mismatch","sse_terminal_status",
        "sse_terminal_output","sse_message_content","sse_output_part","sse_empty_output_no_delta_terminal_empty",
        "sse_empty_output_delta_present_terminal_message","sse_empty_output_no_delta_terminal_message",
        "sse_empty_output_delta_present_terminal_non_message","sse_empty_output_no_delta_terminal_non_message","sse_text_mismatch",
        "sse_duplicate_event_field","sse_invalid_line","sse_missing_completed","sse_pending_data",
        "generation_provider_failure","generation_provider_exception","generation_prompt_limit","generation_render_failure",
        "generation_repair_limit","generation_allocation_failure","requested_pattern_missing",
        "generation_local_source_unavailable","generation_local_busy","generation_local_budget",
        "backend_response_invalid","backend_invalid_job","backend_result_invalid","backend_directory_invalid","backend_unavailable",
        "preference_storage_failed","model_not_authorized","model_list_limit","loopback_unavailable","browser_open_failed","sign_in_timeout",
        "clock_failure","pipe_unauthorized","invalid_message","pipe_unavailable","invalid_runtime","startup_timeout","request_timeout","group_timeout",
        "launch_executable_missing","launch_options_invalid","launch_failed","launch_identity_invalid","invalid_result","model_not_configured","diagnostic_redacted",
        "proposal_decoded","proposal_strict_json_invalid","proposal_root_fields_invalid","proposal_parameter_edit_invalid","proposal_rack_edit_invalid","proposal_macro_edit_invalid",
        "proposal_pattern_invalid","proposal_reconciliation_invalid","proposal_validity_invalid","proposal_name_invalid",
        "proposal_parameter_unknown","proposal_parameter_duplicate","proposal_rack_invalid","proposal_text_invalid","proposal_macro_invalid","proposal_macro_target_unknown",
        "proposal_catalog_selection_invalid","proposal_catalog_parameter_invalid","proposal_catalog_intent_invalid",
        "transport_open","transport_configuration","transport_connect","transport_request","transport_request_headers","transport_send","transport_receive",
        "transport_response_headers","transport_read","transport_body_limit","transport_body_encoding","transport_cleanup","transport_cancelled","transport_deadline","transport_unknown"})
        if(code==known)return known;
    return "diagnostic_redacted";
}
bool write(const Event& event) noexcept {return write(directory(),event);}
bool write(const juce::File& folder,const Event& event) noexcept
{
    try{
        // JUCE's const File/string conversions can allocate shared path storage.
        // Serialize those before the OS lock too; never wait on a control thread.
        static std::mutex mutex;std::unique_lock gate(mutex,std::try_to_lock);if(!gate.owns_lock())return false;
        if(folder.getFullPathName().isEmpty()||!juce::File::isAbsolutePath(folder.getFullPathName())||folder.createDirectory().failed())return false;
        // A single fixed lock serializes both component rings across all processes.
        // Contention drops an event immediately, keeping message/worker paths responsive.
        Handle lock{CreateFileW(folder.getChildFile("generation.lock").getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(lock.value==INVALID_HANDLE_VALUE)return false;
        auto* object=new juce::DynamicObject;juce::var record(object);
        SYSTEMTIME utc{};GetSystemTime(&utc);
        object->setProperty("utc",juce::String::formatted("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",unsigned(utc.wYear),unsigned(utc.wMonth),unsigned(utc.wDay),unsigned(utc.wHour),unsigned(utc.wMinute),unsigned(utc.wSecond),unsigned(utc.wMilliseconds)));
        object->setProperty("pid",int(GetCurrentProcessId()));object->setProperty("build","generation-diagnostics-20261008");object->setProperty("protocol",4);object->setProperty("format",1);
#if defined(_DEBUG)
        object->setProperty("configuration","Debug");
#else
        object->setProperty("configuration","Release");
#endif
        object->setProperty("component",event.component==Component::plugin?"plugin":"companion");object->setProperty("stage",stageName(event.stage));
        object->setProperty("runtime",event.runtime.toString());object->setProperty("request",static_cast<juce::int64>(event.requestId));
        object->setProperty("generation",event.generationStatus);object->setProperty("provider",event.providerStatus);
        object->setProperty("proposal",event.proposalStatus);object->setProperty("render",event.renderStatus);object->setProperty("http",event.httpStatus);object->setProperty("runtimeStatus",event.runtimeStatus);
        object->setProperty("calls",int(event.calls));object->setProperty("nativeError",static_cast<juce::int64>(event.nativeError));object->setProperty("error",safeErrorCode(event.errorCode));
        object->setProperty("session",event.session.toString());object->setProperty("connection",static_cast<juce::int64>(event.connectionId));object->setProperty("job",static_cast<juce::int64>(event.jobId));
        object->setProperty("elapsedMs",static_cast<juce::int64>((std::min)(event.elapsedMs,std::uint64_t{86400000})));
        object->setProperty("model",safeModel(event.model));object->setProperty("providerRequest",safeRequestId(event.providerRequestId));object->setProperty("interrupted",event.interrupted);
        object->setProperty("outputBytes",static_cast<juce::int64>(event.outputBytes));
        const auto text=juce::JSON::toString(record,true)+"\n";const auto size=text.getNumBytesAsUTF8();if(size>2048)return false;
        const auto file=folder.getChildFile(event.component==Component::plugin?"plugin.log":"companion.log");
        if(file.getSize()+std::int64_t(size)>maximumLogBytes){
            const auto archived=[&](int n){return file.getSiblingFile(file.getFileName()+"."+juce::String(n));};
            if(archived(archivedLogCount).exists()&&!archived(archivedLogCount).deleteFile())return false;
            for(int n=archivedLogCount-1;n>=1;--n)if(archived(n).exists()&&!archived(n).moveFileTo(archived(n+1)))return false;
            if(file.exists()&&!file.moveFileTo(archived(1)))return false;
        }
        Handle output{CreateFileW(file.getFullPathName().toWideCharPointer(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(output.value==INVALID_HANDLE_VALUE)return false;DWORD written=0;
        return WriteFile(output.value,text.toRawUTF8(),DWORD(size),&written,nullptr)&&written==size&&FlushFileBuffers(output.value);
    }catch(...){return false;}
}
}
