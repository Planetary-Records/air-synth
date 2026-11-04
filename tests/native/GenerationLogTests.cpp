#include "GenerationLog.h"
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
using namespace airack::diagnostics;
namespace
{
int checks=0,failures=0;
void check(bool value,const char* expression,int line){++checks;if(!value){++failures;std::cout<<"FAIL "<<line<<" "<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
juce::File root;
juce::String combined(){juce::String result;for(const auto& f:root.findChildFiles(juce::File::findFiles,false,"*.log*"))result+=f.loadFileAsString();return result;}
void persistence()
{
    Event e; e.runtime=juce::Uuid();e.requestId=73;e.providerStatus=12;e.httpStatus=429;e.errorCode="rate_limit_exceeded";
    e.model="gpt-6.1-sol";e.providerRequestId="req-safe";e.elapsedMs=60234;e.interrupted=true;
    CHECK(write(root,e));
    const auto text=combined();CHECK(text.contains("rate_limit_exceeded"));CHECK(text.contains(e.runtime.toString()));
    const auto first=juce::JSON::parse(root.getChildFile("plugin.log").loadFileAsString().trim());
    CHECK(int(first["request"])==73);CHECK(int(first["http"])==429);CHECK(text.contains("\"pid\":"));CHECK(text.contains("\"utc\":"));
    Event c=e;c.component=Component::companion;c.stage=Stage::provider;CHECK(write(root,c));
    CHECK(root.getChildFile("companion.log").existsAsFile());CHECK(root.getChildFile("plugin.log").existsAsFile());
    const auto p=juce::JSON::parse(root.getChildFile("plugin.log").loadFileAsString().trim());CHECK(p["stage"]==juce::var("terminal"));
    CHECK(p["model"]==juce::var(e.model));CHECK(p["providerRequest"]==juce::var("req-safe"));CHECK(int(p["elapsedMs"])==60234);CHECK(bool(p["interrupted"]));
    CHECK(p["utc"].toString().endsWith("Z"));CHECK(int(p["protocol"])==4);CHECK(p["build"].isString());
    for(const auto& id:juce::StringArray{"123e4567-e89b-42d3-a456-426614174000","req-still-valid","123e4567-e89b-42d3-a456-426614174000-secret","Bearer secret-token","123e4567-e89b-42d3-a456-42661417400x"}){
        Event v;v.providerRequestId=id;CHECK(write(root,v));auto rows=juce::StringArray::fromLines(root.getChildFile("plugin.log").loadFileAsString());rows.removeEmptyStrings();
        const auto last=juce::JSON::parse(rows[rows.size()-1]);CHECK(last["providerRequest"]==juce::var(id=="req-still-valid"||id=="123e4567-e89b-42d3-a456-426614174000"?id:juce::String()));
    }
}
void redaction()
{
    for(const auto& code:juce::StringArray{"Bearer SENSITIVE_TOKEN","sk-SENSITIVE_TOKEN","{\"prompt\":\"SENSITIVE_PROMPT\"}","rate_limit_exceeded\nSENSITIVE_HEADER","SENSITIVE_PROVIDER_BODY"})
    {Event e;e.errorCode=code;CHECK(write(root,e));CHECK(safeErrorCode(code)=="diagnostic_redacted");}
    CHECK(!combined().contains("SENSITIVE"));CHECK(safeErrorCode("sse_missing_completed")=="sse_missing_completed");
    Event e;e.model="sk-SENSITIVE_TOKEN";e.providerRequestId="Bearer SENSITIVE_TOKEN";CHECK(write(root,e));CHECK(!combined().contains("SENSITIVE"));
    CHECK(safeErrorCode({}).isEmpty());
    for(const auto* code:{"proposal_parameter_unknown","proposal_parameter_duplicate","proposal_rack_invalid","proposal_text_invalid","proposal_macro_invalid","proposal_macro_target_unknown",
        "proposal_catalog_selection_invalid","proposal_catalog_parameter_invalid","proposal_catalog_intent_invalid",
        "generation_local_source_unavailable","generation_local_busy","generation_local_budget"})
    {Event value;value.errorCode=code;CHECK(write(root,value));auto lines=juce::StringArray::fromLines(root.getChildFile("plugin.log").loadFileAsString());lines.removeEmptyStrings();CHECK(juce::JSON::parse(lines[lines.size()-1])["error"]==juce::var(code));}
}
void rotation_and_failure()
{
    // Real near-full logs trigger rotation without a long write loop.
    for(const auto name:{"plugin","companion"})
    {
        const auto file=root.getChildFile(juce::String(name)+".log");
        for(int n=0;n<5;++n){CHECK(file.replaceWithText(juce::String::repeatedString("x",int(maximumLogBytes-20))));
            Event e;e.component=juce::String(name)=="plugin"?Component::plugin:Component::companion;CHECK(write(root,e));}
        CHECK(root.getChildFile(juce::String(name)+".log.3").existsAsFile());CHECK(!root.getChildFile(juce::String(name)+".log.4").exists());
    }
    std::int64_t total=0;int files=0;for(const auto& f:root.findChildFiles(juce::File::findFiles,false,"*.log*")){++files;CHECK(f.getSize()<=maximumLogBytes);total+=f.getSize();}
    CHECK(files==8);CHECK(total<=maximumLogBytes*8);
    const auto blocked=root.getChildFile("file-as-directory");CHECK(blocked.replaceWithText("not a directory"));CHECK(!write(blocked,{}));
    // The same exclusive file lock used by another process rejects immediately.
    HANDLE lock=CreateFileW(root.getChildFile("generation.lock").getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(lock!=INVALID_HANDLE_VALUE);CHECK(!write(root,{}));if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);CHECK(write(root,{}));
    std::vector<std::thread> threads;for(int n=0;n<4;++n)threads.emplace_back([]{for(int i=0;i<60;++i)write(root,{});});for(auto& t:threads)t.join();
    CHECK(root.getChildFile("plugin.log").getSize()<=maximumLogBytes);
    bool complete=true;for(const auto& line:juce::StringArray::fromLines(root.getChildFile("plugin.log").loadFileAsString()))if(line.isNotEmpty())complete=complete&&juce::JSON::parse(line).isObject();CHECK(complete);
    juce::ChildProcess child;CHECK(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--hold-lock",root.getFullPathName()},0));
    const auto ready=root.getChildFile("child-ready");const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!ready.existsAsFile()&&child.isRunning()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(ready.existsAsFile());CHECK(!write(root,{}));CHECK(root.getChildFile("child-release").replaceWithText("release"));CHECK(child.waitForProcessToFinish(4000));CHECK(child.getExitCode()==0);CHECK(write(root,{}));
}
}
int main(int argc,char** argv)
{
    std::cout.setf(std::ios::unitbuf);
    if(argc==3&&juce::String(argv[1])=="--hold-lock"){
        const juce::File folder(argv[2]);HANDLE lock=CreateFileW(folder.getChildFile("generation.lock").getFullPathName().toWideCharPointer(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lock==INVALID_HANDLE_VALUE)return 1;if(!folder.getChildFile("child-ready").replaceWithText("ready")){CloseHandle(lock);return 1;}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!folder.getChildFile("child-release").existsAsFile()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CloseHandle(lock);return folder.getChildFile("child-release").existsAsFile()?0:1;
    }
    if(argc!=2)return 2;root=juce::File(argv[1]).getChildFile("log-tests-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());
    persistence();redaction();rotation_and_failure();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
