#include "AiUnit.h"
#include "PostedClick.h"
#include "CompanionLaunch.h"
#include "IsolatedRackLibrary.h"
#include <iostream>
#include <windows.h>
namespace airack {struct InstrumentMcpTestAccess {
    static std::pair<bool,bool> ownedWork(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return {bool(p.mcpState->stage),bool(p.mcpState->preview)};}
};}
namespace {
int checks=0,failures=0;
void check(bool b,const char* s,int line){++checks;if(!b){++failures;std::cout<<"FAIL "<<line<<" "<<s<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
juce::Component* find(juce::Component& c,const juce::String& id){if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* r=find(*child,id))return r;return nullptr;}
template<class T> T* widget(juce::Component& c,const char* id){auto* r=dynamic_cast<T*>(find(c,id));CHECK(r);return r;}
int textButtons(juce::Component& c){int n=dynamic_cast<juce::TextButton*>(&c)!=nullptr;for(auto* child:c.getChildren())n+=textButtons(*child);return n;}
void pump(int ms=30){juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);}
template<class F> bool until(F f,unsigned ms=5000){const auto end=GetTickCount64()+ms;do{if(f())return true;pump(1);}while(GetTickCount64()<end);return f();}
struct Client {
    airack::companion::PipeConnection pipe;juce::Uuid session;std::uint64_t serial=0;
    bool send(airack::companion::MessageKind kind,juce::var value){const auto text=airack::mcp::encodeJson(value);const airack::companion::Frame frame{kind,session,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8())};
        if(pipe.beginWrite(frame)!=airack::companion::PipeStatus::ready)return false;airack::companion::PipeStatus status;return until([&]{status=pipe.pollWrite();return status!=airack::companion::PipeStatus::pending;})&&status==airack::companion::PipeStatus::complete;}
    juce::var receive(){airack::companion::Frame frame;if(pipe.beginRead()!=airack::companion::PipeStatus::ready)return {};airack::companion::PipeStatus status;
        if(!until([&]{status=pipe.pollRead(frame);return status!=airack::companion::PipeStatus::pending;})||status!=airack::companion::PipeStatus::complete)return {};session=frame.session;juce::var value;CHECK(airack::mcp::ipc::parse(frame.payload.getData(),frame.payload.getSize(),airack::mcp::maximumResultBytes,value));return value;}
    bool open(const juce::String& suffix){if(!until([&]{return pipe.connect(suffix,airack::companion::PipeDomain::mcp)==airack::companion::PipeStatus::ready;}))return false;
        if(!send(airack::companion::MessageKind::mcpHello,airack::mcp::ipc::object({{"version",1},{"role","bridge"}})))return false;const auto value=receive();return value["status"].isInt()&&int(value["status"])==0;}
    juce::var call(const char* name,juce::var arguments,airack::mcp::Status expected=airack::mcp::Status::ready){const auto id=++serial;
        CHECK(send(airack::companion::MessageKind::mcpRequest,airack::mcp::ipc::object({{"version",1},{"serial",airack::mcp::ipc::decimal(id)},{"call",airack::mcp::ipc::object({{"name",name},{"arguments",arguments}})}})));
        const auto value=receive();if(value["serial"].toString()!=airack::mcp::ipc::decimal(id)||!value["status"].isInt()||int(value["status"])!=int(expected))
            std::cout<<"MCP unexpected reply name="<<name<<" expected_serial="<<airack::mcp::ipc::decimal(id)<<" expected_status="<<int(expected)<<" actual="<<juce::JSON::toString(value,true)<<std::endl;
        CHECK(value["serial"].toString()==airack::mcp::ipc::decimal(id));CHECK(value["status"].isInt()&&int(value["status"])==int(expected));return value["data"];}
};
// The AI unit with its MCP light and screen, on the desktop as the editor shows it.
struct Unit {
    std::unique_ptr<airack::studio::AiUnit> unit;airack::studio::Target* light=nullptr;airack::studio::Screen* screen=nullptr;
    Unit(airack::InstrumentProcessor& p,const airack::studio::Model& model):unit(std::make_unique<airack::studio::AiUnit>(p,model,airack::studio::defaultStudioServices())){
        unit->addToDesktop(0);unit->setVisible(true);pump();
        light=widget<airack::studio::Target>(*unit,"ai.mcp");screen=widget<airack::studio::Screen>(*unit,"ai.screen");}
    juce::String heading() const{return screen->texts.count("heading")?screen->texts.at("heading").text:juce::String();}
    juce::String button() const{return screen->texts.count("button")?screen->texts.at("button").text:juce::String();}
    // The page's DISABLE ACCESS or ENABLE ACCESS, as a click on it acts.
    bool access(){const auto* hit=screen->hitFor("mcp.access");if(hit==nullptr)return false;screen->act(*hit,false);pump();return true;}
};
juce::File output;
// With --images <dir>: the unit as drawn, at 1x and 2x.
void snap(juce::Component& c,const char* name){
    if(output==juce::File{})return;
    for(const auto scale:{1,2}){juce::FileOutputStream out(output.getChildFile(juce::String(name)+(scale==2?"@2x":"")+".png"));out.setPosition(0);out.truncate();
        CHECK(out.openedOk()&&juce::PNGImageFormat().writeImageToStream(c.createComponentSnapshot(c.getLocalBounds(),true,(float)scale),out));}
}
}
// With --images <dir>: mcp-light.png, the unit with MCP lit; mcp-on.png and mcp-off.png, its page with access on and
// off; mcp-last-edit.png, the page after an agent's edit.
int main(int argc,char** argv){
    juce::ScopedJuceInitialiser_GUI gui;
    if(argc==3&&juce::String(argv[1])=="--images"){output=juce::File(juce::String(argv[2]));CHECK(output.createDirectory().wasOk());}
    using airack::mcp::ipc::object;
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("ui-mcp");CHECK(root.createDirectory().wasOk());const auto suffix=juce::Uuid().toString().removeCharacters("-");
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",root.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",suffix.toWideCharPointer());
    airack::companion::CompanionLaunch service;const auto launched=service.start({juce::File(RACKSYNTH_UI_HELPER),suffix,root,airack::companion::LaunchRole::mcpService});CHECK(launched.status!=airack::companion::LaunchStatus::unavailable);
    auto p=std::make_unique<airack::InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    const bool inventoryReady=airack::testing::readyRackInventory(*p);CHECK(inventoryReady);if(!inventoryReady)return 1;
    const bool ready=until([&]{return p->mcpView().connected;});CHECK(ready);if(!ready)return 1;
    Client client;const bool authenticated=client.open(suffix);CHECK(authenticated);if(!authenticated)return 1;
    const auto before=p->mcpView();
    CHECK(!before.instance.isNull());CHECK(!before.epoch.isNull());
    const auto* model=airack::studio::studioSkin();CHECK(model!=nullptr);if(model==nullptr)return 1;
    const auto target=object({{"instance_id",before.instance.toDashedString()}});auto state=client.call("get_state",target);CHECK(state["snapshot"].isString());if(!state["snapshot"].isString())return 1;
    const auto staged=client.call("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.23}})}}})}}));CHECK(staged["edit_id"].isString());if(!staged["edit_id"].isString())return 1;
    const auto edit=object({{"instance_id",target["instance_id"]},{"edit_id",staged["edit_id"]}});const auto preview=client.call("render_preview",edit);CHECK(preview["job_id"].isString());
    const auto initial=p->control(0).getValue();
    {
        Unit u(*p,*model);if(u.light==nullptr||u.screen==nullptr)return 1;
        // The plate's MCP light, beside LINKED INSTANCES on the unit's bottom row and lit while agents can reach this
        // synth; no button floats over the plate.
        auto* linked=widget<airack::studio::Target>(*u.unit,"ai.linked");
        CHECK(u.light->getTitle()=="MCP"&&u.light->getWantsKeyboardFocus()&&u.light->isShowing()&&textButtons(*u.unit)==0);
        if(linked!=nullptr)CHECK(u.light->element.led.y==linked->element.led.y&&u.light->element.hitRect.getRight()<linked->element.hitRect.getX()&&u.light->element.hitRect.getX()>40.0f);
        CHECK(until([&]{return u.light->litLayer()=="lit"&&u.light->state()=="access on, 1 client";}));
        std::cout<<"MCP light state="<<u.light->state()<<" lit="<<u.light->litLayer()<<std::endl;snap(*u.unit,"mcp-light");
        // A click on it shows the MCP page: the state, the instance and its activity, where the setup is described, and
        // no setup of its own.
        CHECK(clickPosted(*u.light));pump();
        CHECK(u.screen->page=="mcp"&&u.heading()=="MCP ACCESS ON"&&u.button()=="DISABLE ACCESS");snap(*u.unit,"mcp-on");
        const auto text=u.screen->accessibleText();std::cout<<"MCP page: "<<text<<std::endl;
        CHECK(text.contains(before.instance.toDashedString())&&text.contains("1 client")&&text.contains("No agent edit yet"));
        CHECK(text.contains("docs/user-guide/mcp.md")&&!text.contains("mcp_servers")&&!text.contains("--mcp-stdio")&&!text.contains(".exe"));
        // DISABLE ACCESS revokes the work and refuses agents while the committed sound and the manual controls stay;
        // ENABLE ACCESS starts a fresh epoch, so the earlier edit is stale.
        CHECK(u.access());CHECK(!p->mcpView().enabled);const auto disabled=p->mcpView().epoch;
        CHECK(u.heading()=="MCP ACCESS OFF"&&u.button()=="ENABLE ACCESS"&&u.light->litLayer().isEmpty()&&u.light->state()=="access off");snap(*u.unit,"mcp-off");
        client.call("apply_edit",edit,airack::mcp::Status::unauthorized);CHECK(p->control(0).getValue()==initial);p->control(0).setValue(.41f);CHECK(p->control(0).getValue()==.41f);
        CHECK(disabled!=before.epoch);CHECK(u.access());CHECK(p->mcpView().enabled);CHECK(p->mcpView().epoch!=disabled);
        CHECK(u.heading()=="MCP ACCESS ON"&&u.light->litLayer()=="lit");
        client.call("apply_edit",edit,airack::mcp::Status::stale);CHECK(p->control(0).getValue()==.41f);
        // A second click, like Escape, goes back to the page the unit showed.
        CHECK(clickPosted(*u.light));pump();CHECK(u.screen->page!="mcp");
    }
    const auto oldWork=airack::InstrumentMcpTestAccess::ownedWork(*p);
    std::cout<<"MCP post-reenable prior_stage="<<oldWork.first<<" prior_preview="<<oldWork.second<<std::endl;
    const bool retired=until([&]{p->pollMcp();const auto work=airack::InstrumentMcpTestAccess::ownedWork(*p);return !work.first&&!work.second;},30000);
    CHECK(retired);if(!retired)return 1;
    // The unit's going leaves the endpoint and an agent's staged work.
    pump();CHECK(p->mcpView().instance==before.instance);CHECK(p->mcpView().enabled);
    state=client.call("get_state",target);const auto retained=client.call("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.24}})}}})}}));
    const auto retainedEdit=object({{"instance_id",target["instance_id"]},{"edit_id",retained["edit_id"]}});CHECK(retained["edit_id"].isString());
    client.call("get_edit",retainedEdit);
    CHECK(bool(client.call("apply_edit",retainedEdit)["changed"]));CHECK(p->mcpView().lastEdit.contains("global.master_level"));
    CHECK(p->mcpView().clientsAvailable&&p->mcpView().clients==1);
    // The page names the agent's last committed edit, and reads its diff out.
    {
        Unit u(*p,*model);if(u.light==nullptr||u.screen==nullptr)return 1;
        CHECK(clickPosted(*u.light));pump();
        const auto text=u.screen->accessibleText();
        CHECK(u.screen->page=="mcp"&&text.contains("Last agent edit: transaction")&&text.contains("global.master_level"));snap(*u.unit,"mcp-last-edit");
        if(u.screen->onEscape)u.screen->onEscape();
        pump();CHECK(u.screen->page!="mcp");
    }
    CHECK(p->mcpView().instance==before.instance&&p->mcpView().enabled);
    std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;
}
