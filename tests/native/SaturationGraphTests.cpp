#include "FrozenRackExamples.h"
#include "PreparedMakerSkins.h"
#include "SaturationGraph.h"
#include "MakerFace.h"
#include <future>
#include "AllControlsPanel.h"
#include "InstrumentProcessor.h"
#include "FxMonitor.h"
#include "RackReconciliation.h"
#include "RackExamples.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace airack {struct FxMonitorTestAccess {
    static FxMonitor::SaturationToken latest(const FxMonitor& monitor,std::uint8_t slot) {
        return {(std::uint64_t{1}<<63)|(monitor.nextSaturationGeneration-1),slot};
    }
};}
namespace airack {struct InstrumentStructuralTestAccess {
    static void detach(InstrumentProcessor& processor){processor.engine.attachFxMonitor(nullptr);}
    static void resetEpoch(InstrumentProcessor& processor){processor.resetGeneration.fetch_add(1);}
};}
namespace airack::studio {struct SaturationGraphTestAccess {
    static void keyQuery(SaturationGraph& graph,bool (*query)(int)){graph.auditionKeyDown=query;}
};}
namespace {
using namespace airack;
using studio::SaturationGraph;
int checks=0,failures=0;bool desktopEnabled=true;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<'\n';}}while(false)
struct Notifications final:juce::AudioProcessorParameter::Listener {
    explicit Notifications(juce::AudioProcessorParameter& p):parameter(p){p.addListener(this);}
    ~Notifications() override{detach();}
    void detach(){if(attached){parameter.removeListener(this);attached=false;}}
    void parameterValueChanged(int,float) override{if(onValue)onValue();}
    void parameterGestureChanged(int,bool start) override{if(start){++starts;if(onStart)onStart();}else ++ends;}
    juce::AudioProcessorParameter& parameter;bool attached=true;int starts=0,ends=0;std::function<void()> onStart,onValue;
};
juce::MouseEvent mouse(juce::Component& c,juce::Point<float> at,bool held=true){
    const auto time=juce::Time::getCurrentTime();return {juce::Desktop::getInstance().getMainMouseSource(),at,
        juce::ModifierKeys(held?juce::ModifierKeys::leftButtonModifier:0),1,0,0,0,0,&c,&c,time,at,time,1,held};
}
struct Rig {
    std::unique_ptr<InstrumentProcessor> p;std::unique_ptr<SaturationGraph> graph;
    bool closing=false;std::uint8_t slot=0;std::size_t low=0,high=0;int frames=0;
    explicit Rig(std::uint8_t s=0):slot(s){
        companion::ClientOptions offline;offline.allowLaunch=false;
        auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
        offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
        p=std::make_unique<InstrumentProcessor>(offline);
        const auto designs=airack::test::frozenFactoryRackDesigns();RackOperation op;op.design=*std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf01");});op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};op.allocation={{0,s}};
        SoundDocument placed;RackDifference diff;CHECK(makeRackCandidate(SoundDocument{},op,SoundEditOrigin::manual,placed,diff)==RackCandidateStatus::ready);install(placed);
        low=saturationParameterStart+s*saturationParametersPerSlot+5;high=low+1;
        studio::house::Host host;host.closing=[this]{return closing;};host.frame=[this]{++frames;return std::make_shared<int>(0);};
        graph=std::make_unique<SaturationGraph>(*p,host);graph->setBounds(20,20,1400,180);if(desktopEnabled)graph->addToDesktop(juce::ComponentPeer::windowIsTemporary);graph->setVisible(true);refresh();
    }
    SoundDocument sound(){SoundDocument d;std::uint64_t rev;CHECK(p->trySoundSnapshot(d,rev));return d;}
    void refresh(){graph->refresh(sound(),slot);}
    void install(const SoundDocument& d){juce::MemoryBlock b;CHECK(encodeSoundState(d,b));CHECK(p->restoreSoundState(b.getData(),int(b.getSize())));}
    void edit(const std::function<void(SoundDocument&)>& fn){auto d=sound();std::uint64_t rev;CHECK(p->trySoundSnapshot(d,rev));const auto before=d;fn(d);if(d!=before)CHECK(p->applySoundEdit(d,SoundEditOrigin::manual,rev));refresh();}
    void physical(SoundDocument& d,std::size_t i,float hz){d.values[i]=p->control(i).convertTo0to1(hz);}
};
void groupedRawCorners(){
    std::cout<<"SECTION all_eight_physical_banks_atomic_raw_gesture_undo\n";
    for(std::uint8_t slot=0;slot<8;++slot){Rig r(slot);auto& g=*r.graph;CHECK(g.parameters()==std::vector<std::size_t>({r.low,r.high}));
        for(float mode:{0.f,1.f}){r.edit([&](auto& d){d.values[r.low-5]=mode;});const auto before=r.sound();
            const float low=mode==0?1800.f:1900.f,high=mode==0?500.f:700.f;
            Notifications a(r.p->control(r.low)),b(r.p->control(r.high));CHECK(g.beginCrossoverGesture());CHECK(g.dragCrossovers(1700,600));CHECK(g.dragCrossovers(low,high));CHECK(g.endCrossoverGesture());
            const auto after=r.sound();CHECK(std::abs(parameterPhysicalValue(after.values,r.low)-low)<.1f);CHECK(std::abs(parameterPhysicalValue(after.values,r.high)-high)<.1f);
            CHECK(a.starts==1&&a.ends==1&&b.starts==1&&b.ends==1);CHECK(r.p->undoSoundEdit());const auto undone=r.sound();
            if(undone!=before){for(std::size_t i=0;i<parameterCount;++i)if(undone.values[i]!=before.values[i])std::cout<<"UNDO_VALUE "<<i<<" "<<before.values[i]<<" -> "<<undone.values[i]<<'\n';std::cout<<"UNDO_OTHER layout="<<(undone.layout==before.layout)<<" macros="<<(undone.macros==before.macros)<<" personalities="<<(!undone.personalities||!before.personalities||*undone.personalities==*before.personalities)<<'\n';}
            CHECK(undone==before);CHECK(r.p->redoSoundEdit());CHECK(r.sound()==after);r.refresh();
            CHECK(g.getDescription().containsIgnoreCase("raw"));CHECK(g.getDescription().containsIgnoreCase("preview"));CHECK(g.getDescription().containsIgnoreCase(mode==0?"inactive":"three"));
        }
    }
}
void refusalsAndContext(){
    std::cout<<"SECTION atomic_refusals_locks_macros_reset_and_identity\n";Rig r;auto& g=*r.graph;
    for(const auto corners:{std::pair(39.f,3500.f),{200.f,16001.f},{std::numeric_limits<float>::quiet_NaN(),800.f},{200.f,std::numeric_limits<float>::infinity()}}){
        const auto before=r.sound();CHECK(g.beginCrossoverGesture());CHECK(!g.dragCrossovers(corners.first,corners.second));CHECK(r.sound()==before);g.endCrossoverGesture();
    }
    for(int lock=0;lock<3;++lock){r.edit([&](auto& d){d.controlLocks[r.low]=lock==0;d.controlLocks[r.high]=lock==1;d.effectLocks[0]=lock==2;});CHECK(!g.beginCrossoverGesture());}
    r.edit([&](auto& d){d.controlLocks[r.low]=d.controlLocks[r.high]=d.effectLocks[0]=false;d.macros[0]={parameterRegistry()[r.low].stateKey,.5f};d.values[1]=.5f;});
    const auto macro=r.sound().macros;CHECK(g.beginCrossoverGesture());CHECK(g.dragCrossovers(700,3000));CHECK(g.endCrossoverGesture());CHECK(r.sound().macros==macro);
    for(int context=0;context<4;++context){r.refresh();CHECK(g.beginCrossoverGesture());auto d=r.sound();
        if(context==0)r.p->control(1).setValueNotifyingHost(.8f);
        else {if(context==1)d.macros[0].amount=-.5f;if(context==2){d.personalities.reset();d.layout.effects[0].kind=EffectKind::drive;}if(context==3)d.values[r.low]=.1f;r.install(d);}
        const auto before=r.sound();CHECK(!g.dragCrossovers(800,4000));CHECK(r.sound()==before);g.endCrossoverGesture();
        if(context==2){d.layout.effects[0].kind=EffectKind::saturation;r.install(d);}r.refresh();
    }
    r.edit([&](auto& d){d.macros[0]={parameterRegistry()[r.low].stateKey,1};d.values[1]=1;r.physical(d,r.low,40);r.physical(d,r.high,500);});
    CHECK(g.getDescription().contains("Raw Low 40 Hz"));CHECK(g.getDescription().contains("Low 400 Hz"));
    CHECK(parameterPhysicalValue(r.sound().values,r.low)==40);
}
void noOpKeepsPreviousUndo(){
    std::cout<<"SECTION net_noop_does_not_hide_previous_undo\n";Rig r;
    const auto original=r.sound();r.edit([&](auto& d){d.values[r.low-5]=1;});const auto before=r.sound();
    CHECK(r.graph->beginCrossoverGesture());CHECK(r.graph->dragCrossovers(700,3000));
    CHECK(r.graph->dragCrossovers(parameterPhysicalValue(before.values,r.low),parameterPhysicalValue(before.values,r.high)));CHECK(r.graph->endCrossoverGesture());
    CHECK(r.sound()==before);CHECK(r.p->undoSoundEdit());CHECK(r.sound()==original);
}
void macroAutomationDuringNotificationCancels(){
    std::cout<<"SECTION macro_context_changed_in_host_notification_ends_group\n";Rig r;
    r.edit([&](auto& d){d.macros[0]={parameterRegistry()[r.low].stateKey,.5f};});
    Notifications low(r.p->control(r.low)),high(r.p->control(r.high));CHECK(r.graph->beginCrossoverGesture());
    low.onValue=[&]{r.p->control(1).setValueNotifyingHost(.75f);};
    CHECK(!r.graph->dragCrossovers(700,3000));const auto after=r.sound();CHECK(after.values[1]==.75f);
    CHECK(!r.graph->dragCrossovers(800,4000));CHECK(r.sound()==after);CHECK(low.starts==low.ends&&high.starts==high.ends);
}
void proposalShowsMacroAwareBoundaryWithoutWriting(){
    std::cout<<"SECTION proposal_macro_preview_outline_preserves_raw_sound\n";Rig r;
    r.edit([&](auto& d){d.values[r.low-5]=1;r.physical(d,r.low,40);r.physical(d,r.high,500);});const auto before=r.sound();
    const auto image=r.graph->createComponentSnapshot(r.graph->getLocalBounds());auto candidate=std::make_shared<SoundDocument>(before);
    candidate->macros[0]={parameterRegistry()[r.low].stateKey,1};candidate->values[1]=1;r.graph->showProposal(candidate);
    const auto proposed=r.graph->createComponentSnapshot(r.graph->getLocalBounds());
    // Candidate raw Low stays40; its macro projects to2000, then the .8*500 clamp gives400.
    const auto x=juce::roundToInt(28+(r.graph->getWidth()-56)*std::log(20.)/std::log(1000.));int different=0;
    for(int y=68;y<104;++y)for(int dx=-2;dx<=2;++dx)if(image.getPixelAt(x+dx,y)!=proposed.getPixelAt(x+dx,y))++different;
    CHECK(different>8);CHECK(r.sound()==before);r.graph->showProposal(nullptr);CHECK(r.sound()==before);
}
void ancestorHideEndsNonfocusedAndReentrantGroups(){
    std::cout<<"SECTION ancestor_hide_and_reentrant_begin_release_nonfocused_groups\n";
    for(bool reentrant:{false,true}){Rig r;r.edit([&](auto& d){d.values[r.low-5]=1;});auto& g=*r.graph;g.removeFromDesktop();
        juce::Component ancestor,other;ancestor.setBounds(20,20,1440,240);other.setBounds(0,0,20,20);other.setWantsKeyboardFocus(true);
        ancestor.addAndMakeVisible(g);ancestor.addAndMakeVisible(other);if(desktopEnabled)ancestor.addToDesktop(juce::ComponentPeer::windowIsTemporary);ancestor.setVisible(true);if(desktopEnabled)other.grabKeyboardFocus();
        CHECK(!g.hasKeyboardFocus(false));Notifications low(r.p->control(r.low)),high(r.p->control(r.high));
        if(reentrant)low.onStart=[&]{ancestor.setVisible(false);};
        CHECK(g.beginCrossoverGesture()==!reentrant);if(!reentrant)ancestor.setVisible(false);
        CHECK(low.starts==1&&low.ends==1&&high.starts==1&&high.ends==1);CHECK(r.p->undoSoundEdit());
        const auto hidden=r.sound();CHECK(!g.dragCrossovers(700,3000));CHECK(r.sound()==hidden);CHECK(low.ends==1&&high.ends==1);
        low.onStart=nullptr;ancestor.setVisible(true);r.refresh();CHECK(g.beginCrossoverGesture());CHECK(g.endCrossoverGesture());CHECK(low.starts==2&&low.ends==2&&high.starts==2&&high.ends==2);
    }
}
void adaptersRateAndLifecycle(){
    std::cout<<"SECTION pointer_keyboard_accessibility_rate_and_lifecycle\n";Rig r;auto& g=*r.graph;
    CHECK(g.getDescription().containsIgnoreCase("unknown effective processing rate"));r.p->prepareToPlay(8000,64);r.refresh();CHECK(g.getDescription().contains("8000"));
    r.edit([&](auto& d){r.physical(d,r.low,2000);r.physical(d,r.high,16000);d.values[r.low+2]=1;d.values[r.low+4]=1;});
    CHECK(g.getDescription().contains("16000"));CHECK(g.getDescription().contains("3600"));CHECK(g.getDescription().containsIgnoreCase("post"));CHECK(g.getDescription().containsIgnoreCase("clamp"));
    r.p->prepareToPlay(48000,64);r.refresh();CHECK(g.getDescription().contains("48000"));
    const auto before=r.sound();CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));CHECK(r.sound()!=before);CHECK(r.p->undoSoundEdit());r.refresh();
    auto* handler=g.getAccessibilityHandler();CHECK(handler);auto* value=handler?handler->getValueInterface():nullptr;CHECK(value);
    if(value){value->setValue(900);CHECK(std::abs(parameterPhysicalValue(r.sound().values,r.low)-900)<.1f);}
    auto current=r.sound();const auto x=28.f+(g.getWidth()-56.f)*float(std::log(parameterPhysicalValue(current.values,r.low)/20)/std::log(1000.));
    g.mouseDown(mouse(g,{x,85}));g.mouseDrag(mouse(g,{x+40,85}));g.mouseUp(mouse(g,{x+40,85},false));CHECK(r.sound()!=current);
    for(int cancel=0;cancel<7;++cancel){r.refresh();Notifications a(r.p->control(r.low)),b(r.p->control(r.high));CHECK(g.beginCrossoverGesture());
        if(cancel==0)g.focusLost(juce::Component::focusChangedDirectly);
        if(cancel==1){g.setVisible(false);g.setVisible(true);}
        if(cancel==2){auto d=r.sound();d.layout.effects[0].use=SlotUse::retired;g.refresh(d,0);}
        if(cancel==3)g.refresh(r.sound(),1);
        if(cancel==4){r.install(r.sound());r.refresh();}
        if(cancel==5){r.closing=true;g.dragCrossovers(900,5000);r.closing=false;}
        if(cancel==6)g.retire();
        const auto unchanged=r.sound();CHECK(!g.dragCrossovers(1000,6000));CHECK(r.sound()==unchanged);CHECK(a.starts==1&&a.ends==1&&b.starts==1&&b.ends==1);
    }
    CHECK(r.frames>0);CHECK(!g.beginCrossoverGesture());CHECK(!g.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
}
void reentrantRetirement(){
    std::cout<<"SECTION reentrant_host_callbacks_end_each_admitted_parameter_once\n";
    for(bool duringBegin:{true,false}){Rig r;Notifications a(r.p->control(r.low)),b(r.p->control(r.high));
        if(duringBegin){a.onStart=[&]{r.graph->retire();};CHECK(!r.graph->beginCrossoverGesture());}
        else {CHECK(r.graph->beginCrossoverGesture());a.onValue=[&]{r.graph->retire();};CHECK(!r.graph->dragCrossovers(900,5000));}
        CHECK(a.starts==a.ends&&b.starts==b.ends);const auto after=r.sound();CHECK(!r.graph->dragCrossovers(1000,6000));CHECK(r.sound()==after);
    }
}
void mountedGlobal(){
    std::cout<<"SECTION mounted_global_numeric_projection_proposal_page_and_bank_lifecycle\n";Rig r(6);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    studio::AllControlsPanel panel(*r.p,*model,{});panel.show({{NativeModule::saturation,6}});panel.refresh(r.sound());panel.addToDesktop(juce::ComponentPeer::windowIsTemporary);panel.setVisible(true);
    std::function<juce::Component*(juce::Component&)> find=[&](auto& c)->juce::Component*{if(c.getComponentID()=="saturation.graph")return &c;for(auto* child:c.getChildren())if(auto* p=find(*child))return p;return nullptr;};
    auto* g=dynamic_cast<SaturationGraph*>(find(panel));CHECK(g);if(!g)return;
    CHECK(g->isShowing());CHECK(g->parameters()==std::vector<std::size_t>({r.low,r.high}));CHECK(panel.parameters().size()==36);CHECK(panel.shownHeight()>444);
    CHECK(panel.knob(r.low)&&std::abs(panel.knob(r.low)->getValue()-parameterPhysicalValue(r.sound().values,r.low))<.1f);
    CHECK(panel.knob(r.high)&&std::abs(panel.knob(r.high)->getValue()-parameterPhysicalValue(r.sound().values,r.high))<.1f);
    CHECK(g->getBottom()<=panel.shownHeight());for(auto* knob:panel.shownKnobs())CHECK(!g->getBounds().intersects(panel.getLocalArea(&knob->entry,knob->entry.getLocalBounds())));
    {Notifications low(r.p->control(r.low)),high(r.p->control(r.high));const auto before=r.sound();CHECK(g->beginCrossoverGesture());panel.setVisible(false);
        CHECK(low.starts==1&&low.ends==1&&high.starts==1&&high.ends==1);CHECK(r.p->beginParameterGesture(0));CHECK(r.p->endParameterGesture(0));CHECK(!g->dragCrossovers(700,3000));CHECK(r.sound()==before);panel.setVisible(true);}
    const auto before=r.sound();auto proposal=std::make_shared<SoundDocument>(before);proposal->values[r.low]=.8f;panel.showProposal(proposal);CHECK(g->getDescription().containsIgnoreCase("proposal"));CHECK(r.sound()==before);
    Notifications a(r.p->control(r.low)),b(r.p->control(r.high));CHECK(g->beginCrossoverGesture());panel.showKey(4);CHECK(!g->isShowing());CHECK(a.starts==a.ends&&b.starts==b.ends);CHECK(panel.shownHeight()==254);
    panel.showKey(0);r.p->prepareToPlay(22050,64);panel.refresh(r.sound());CHECK(g->getDescription().contains("22050"));
    CHECK(g->beginCrossoverGesture());panel.show({{NativeModule::saturation,7}});CHECK(a.starts==a.ends&&b.starts==b.ends);
    auto all=r.sound();const auto designs=airack::test::frozenFactoryRackDesigns();const auto factory=*std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf01");});std::vector<studio::AllControlsPanel::Unit> units;
    for(std::uint8_t b=0;b<8;++b){units.push_back({NativeModule::saturation,b});if(b==6)continue;RackOperation op;op.design=factory;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};op.allocation={{0,b}};SoundDocument next;RackDifference diff;CHECK(makeRackCandidate(all,op,SoundEditOrigin::manual,next,diff)==RackCandidateStatus::ready);all=std::move(next);}
    r.install(all);panel.show(units);panel.refresh(r.sound());
    for(std::uint8_t s=0;s<8;++s){panel.showKey(s*5);auto* bound=dynamic_cast<SaturationGraph*>(find(panel));CHECK(bound);const auto low=saturationParameterStart+s*saturationParametersPerSlot+5;if(bound)CHECK(bound->parameters()==std::vector<std::size_t>({low,low+1}));}
}
void preparedEffectiveRateRefresh(){
    std::cout<<"SECTION prepared_effective_rate_refresh_without_sound_edits\n";Rig r(6);
    r.edit([&](auto& d){d.values[758]=1;d.values[r.low-5]=1;r.physical(d,r.low,2000);r.physical(d,r.high,16000);d.values[r.low+2]=d.values[r.low+4]=1;});
    const auto saved=r.sound();juce::MemoryBlock bytes;CHECK(encodeSoundState(saved,bytes));
    const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    studio::AllControlsPanel panel(*r.p,*model,{});panel.show({{NativeModule::saturation,6}});panel.setSize(1485,panel.shownHeight());
    panel.addToDesktop(juce::ComponentPeer::windowIsTemporary);panel.setVisible(true);
    auto* mounted=dynamic_cast<SaturationGraph*>(panel.findChildWithID("saturation.graph"));CHECK(mounted);if(!mounted)return;
    auto monitor=r.p->fxMonitorHandle();CHECK(monitor);if(!monitor)return;
    const auto refresh=[&]{r.graph->refresh(saved,r.slot);panel.refresh(saved);};
    const auto observe=[&](const char* stage,double rate,float high,const char* preview){
        refresh();CHECK(monitor->sampleRate()==rate);CHECK(r.sound()==saved);
        juce::MemoryBlock current;CHECK(encodeSoundState(r.sound(),current));CHECK(current==bytes);
        for(auto* graph:{r.graph.get(),mounted}){
            auto* accessible=graph->getAccessibilityHandler();CHECK(accessible);
            const auto text=accessible?accessible->getDescription():juce::String();
            CHECK(text.containsIgnoreCase(rate==0?"unknown effective processing rate":"effective processing rate"));
            CHECK(!text.containsIgnoreCase("host rate"));CHECK(text.contains("Raw Low 2000 Hz, High 16000 Hz"));CHECK(text.contains(preview));
            CHECK(text.contains(rate==0?"fallback 48000 Hz":juce::String(rate,0)+" Hz"));CHECK(text.startsWith("SATURATION | "));
            const auto image=graph->createComponentSnapshot(graph->getLocalBounds());
            const auto boundary=[&](float hz){return juce::roundToInt(28+(graph->getWidth()-56)*std::log(hz/20.)/std::log(1000.));};
            CHECK(image.getPixelAt(boundary(high),72)==juce::Colour(0xffceaf7d));
            CHECK(image.getPixelAt(boundary(high==3600?7200.f:3600.f),72)!=juce::Colour(0xffceaf7d));
        }
        CHECK(panel.knob(r.low)&&panel.knob(r.low)->getValue()==2000);CHECK(panel.knob(r.high)&&panel.knob(r.high)->getValue()==16000);
        std::cout<<"RATE "<<stage<<" monitor="<<rate<<" quality="<<int(r.p->effectiveRenderQuality())<<" pending="<<r.p->renderQualityPending()<<" "<<mounted->getDescription()<<'\n';
        const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
        if(root.isNotEmpty()){
            const auto file=juce::File(root).getChildFile(juce::String("effective-rate-")+stage+".png");CHECK(file.getParentDirectory().createDirectory());
            auto stream=file.createOutputStream();CHECK(stream);if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds()),*stream));
        }
    };
    observe("unknown",0,16000,"Low 2000 Hz, High 16000 Hz");
    CHECK(r.p->selectedRenderQuality()==RenderQuality::higher);r.p->prepareToPlay(8000,64);
    CHECK(r.p->effectiveRenderQuality()==RenderQuality::realtime);observe("realtime-8k",8000,3600,"Low 2000 Hz, High 3600 Hz");
    const auto realtime=mounted->getDescription();r.p->setNonRealtime(true);refresh();
    CHECK(r.p->renderQualityPending());CHECK(monitor->sampleRate()==8000);CHECK(mounted->getDescription()==realtime);CHECK(r.graph->getDescription()==realtime);
    r.p->prepareToPlay(8000,64);CHECK(r.p->effectiveRenderQuality()==RenderQuality::higher);CHECK(!r.p->renderQualityPending());
    observe("offline-higher-16k",16000,7200,"Low 2000 Hz, High 7200 Hz");
    const auto higher=mounted->getDescription();r.p->setNonRealtime(false);refresh();
    CHECK(r.p->renderQualityPending());CHECK(monitor->sampleRate()==16000);CHECK(mounted->getDescription()==higher);CHECK(r.graph->getDescription()==higher);
    r.p->prepareToPlay(8000,64);CHECK(r.p->effectiveRenderQuality()==RenderQuality::realtime);
    observe("returned-realtime-8k",8000,3600,"Low 2000 Hz, High 3600 Hz");
}
void mergedDrawerOffscreen(){
    std::cout<<"SECTION merged_drawers_all_banks_no_native_peers\n";
    const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    companion::ClientOptions offline;offline.allowLaunch=false;auto processor=std::make_unique<InstrumentProcessor>(offline);
    for(const auto module:{NativeModule::saturation,NativeModule::spaceReverb,NativeModule::eraReverb})
        for(std::uint8_t slot=0;slot<8;++slot){
            studio::AllControlsPanel panel(*processor,*model,{});panel.show({{module,slot}});panel.setSize(1485,panel.shownHeight());panel.setVisible(true);
            CHECK(panel.getPeer()==nullptr);const auto parameters=panel.parameters();CHECK(parameters.size()==rackUnitParameterCapacity(module));
            if(module==NativeModule::saturation){CHECK(panel.keys().size()==5);for(std::size_t field=0;field<35;++field)CHECK(panel.showParameter(saturationParameterStart+slot*35+field));}
            else if(module==NativeModule::spaceReverb)CHECK(panel.showParameter(spaceReverbParameterStart+slot*spaceReverbParametersPerSlot+95));
            else CHECK(panel.showParameter(eraReverbParameterStart+slot*eraReverbParametersPerSlot+14));
            CHECK(panel.createComponentSnapshot(panel.getLocalBounds()).isValid());CHECK(panel.getPeer()==nullptr);
        }
    std::cout<<"MERGE_DRAWERS offscreen=1 native_peers=0 saturation_banks=8 space_banks=8 era_banks=8\n";
}
void offscreenAuditionCancellation(){
    std::cout<<"SECTION actual_graph_processor_audio_cancellation_matrix_offscreen\n";
    const std::array names{"release","Escape","focus_loss","hide","ancestor_hide","ancestor_delete","editor_close","rebind","retire","token_retire",
        "sound_recall","project_recall","reset_epoch","mode_Full","slot_bypass","rack_move","rack_replace","rack_remove","engine_detach","reprepare","rate_change","release_resources","instance_teardown","repress_during_restore"};
    for(std::size_t event=0;event<names.size();++event){
        Rig r;companion::ClientOptions offline;offline.allowLaunch=false;auto normal=std::make_unique<InstrumentProcessor>(offline);
        const auto install=[&](InstrumentProcessor& p,const SoundDocument& d){juce::MemoryBlock bytes;CHECK(encodeSoundState(d,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));};
        const auto initial=r.sound();install(*normal,initial);
        r.edit([&](auto& d){d.values[r.low-5]=1;d.values[572]=0;d.values[r.low-2]=0;
            for(const auto id:{"output.gain_db","output.protection"}){const auto registry=parameterRegistry();const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return juce::String(p.id)==id;});CHECK(found!=registry.end());if(found!=registry.end())r.physical(d,std::size_t(found-registry.begin()),0);}});
        SoundDocument controlSound;std::uint64_t controlRevision;CHECK(normal->trySoundSnapshot(controlSound,controlRevision));CHECK(normal->applySoundEdit(r.sound(),SoundEditOrigin::manual,controlRevision));
        r.p->prepareToPlay(48000,64);normal->prepareToPlay(48000,64);r.refresh();
        auto monitor=r.p->fxMonitorHandle();SoundDocument captured;std::uint64_t revision,reset;CHECK(r.p->trySoundSnapshot(captured,revision,reset));
        const auto rack=captured.personalities->instances[0].id;juce::MemoryBlock saved;CHECK(encodeSoundState(captured,saved));
        Notifications low(r.p->control(r.low)),high(r.p->control(r.high));
        auto ancestor=std::make_unique<juce::Component>();ancestor->addAndMakeVisible(*r.graph);ancestor->setVisible(true);
        CHECK(r.graph->beginBandAudition(SaturationLane::mid));const auto token=FxMonitorTestAccess::latest(*monitor,r.slot);
        const auto* heldButton=dynamic_cast<juce::Button*>(r.graph->findChildWithID("saturation.audition.2"));CHECK(heldButton);
        const auto staleUiCallback=heldButton?heldButton->onStateChange:std::function<void()>{};CHECK(staleUiCallback);
        juce::AudioBuffer<float> audio(2,64),reference(2,64);juce::MidiBuffer midi;midi.ensureSize(256);
        double energy=0,contrast=0;
        for(int block=0;block<24;++block){midi.clear();if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
            r.p->processBlock(audio,midi);normal->processBlock(reference,midi);
            for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){energy+=std::abs(audio.getSample(ch,n));contrast=std::max(contrast,std::abs(double(audio.getSample(ch,n))-reference.getSample(ch,n)));}}
        CHECK(energy>1e-6);CHECK(contrast>1e-6);CHECK(monitor->auditionTailSeconds()>=2.02);
        const auto editBoth=[&](const std::function<void(SoundDocument&)>& change){r.edit(change);SoundDocument d;std::uint64_t rev;CHECK(normal->trySoundSnapshot(d,rev));CHECK(normal->applySoundEdit(r.sound(),SoundEditOrigin::manual,rev));};
        const auto rackBoth=[&](RackOperationKind kind){RackOperation operation;operation.kind=kind;operation.selected=rack;operation.destination={RackRole::fx,kind==RackOperationKind::move?std::uint8_t(0):masterChain,0};
            if(kind==RackOperationKind::swap){const auto factories=airack::test::frozenFactoryRackDesigns();operation.design=*std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Band Color";});operation.allowSlotReuse=true;operation.allocation={{0,0}};}
            SoundDocument d;std::uint64_t rev;CHECK(r.p->trySoundSnapshot(d,rev));CHECK(r.p->applyRackOperation(operation,rev)==RackCandidateStatus::ready);
            CHECK(normal->trySoundSnapshot(d,rev));CHECK(normal->applyRackOperation(operation,rev)==RackCandidateStatus::ready);r.refresh();};
        int rate=48000;bool stopped=false,teardown=false;
        switch(event){
        case 0:r.graph->mouseUp(mouse(*r.graph,{0,0},false));break;
        case 1:CHECK(r.graph->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));break;
        case 2:r.graph->focusLost(juce::Component::focusChangedDirectly);break;
        case 3:r.graph->setVisible(false);break;
        case 4:ancestor->setVisible(false);break;
        case 5:ancestor.reset();break;
        case 6:r.closing=true;r.refresh();break;
        case 7:r.graph->refresh(r.sound(),1);break;
        case 8:r.graph->retire();break;
        case 9:r.p->retireSaturationAudition(token);break;
        case 10:r.install(captured);install(*normal,captured);r.refresh();stopped=true;break;
        case 11:{ProjectDocument project;std::uint64_t rev;CHECK(r.p->tryProjectSnapshot(project,rev));juce::MemoryBlock bytes;CHECK(encodeProjectState(project,bytes));CHECK(r.p->restoreProjectState(bytes.getData(),int(bytes.getSize())));CHECK(normal->restoreProjectState(bytes.getData(),int(bytes.getSize())));r.refresh();stopped=true;break;}
        case 12:InstrumentStructuralTestAccess::resetEpoch(*r.p);InstrumentStructuralTestAccess::resetEpoch(*normal);r.refresh();stopped=true;break;
        case 13:editBoth([&](auto& d){d.values[r.low-5]=0;});break;
        case 14:editBoth([&](auto& d){d.values[572]=1;});break;
        case 15:rackBoth(RackOperationKind::move);break;
        case 16:rackBoth(RackOperationKind::swap);break;
        case 17:rackBoth(RackOperationKind::remove);break;
        case 18:InstrumentStructuralTestAccess::detach(*r.p);r.refresh();break;
        case 19:r.p->prepareToPlay(rate,64);normal->prepareToPlay(rate,64);r.refresh();stopped=true;break;
        case 20:rate=8000;r.p->prepareToPlay(rate,64);normal->prepareToPlay(rate,64);r.refresh();stopped=true;break;
        case 21:r.p->releaseResources();normal->releaseResources();r.refresh();stopped=true;break;
        case 22:CHECK(low.starts==0&&low.ends==0&&high.starts==0&&high.ends==0);low.detach();high.detach();r.graph.reset();r.p.reset();teardown=true;break;
        case 23:r.graph->endBandAudition();for(int block=0;block<3;++block){r.p->processBlock(audio,midi);normal->processBlock(reference,midi);}CHECK(r.graph->beginBandAudition(SaturationLane::high));r.p->processBlock(audio,midi);normal->processBlock(reference,midi);r.graph->endBandAudition();break;
        }
        if(staleUiCallback)staleUiCallback();CHECK(!monitor->requestSaturationAudition(token,SaturationLane::low));
        if(teardown){CHECK(!monitor->retireSaturationAudition(token));CHECK(monitor->sampleRate()==0);CHECK(monitor->auditionTailSeconds()==0);
            std::cout<<"CANCEL_MATRIX "<<names[event]<<" held_energy="<<energy<<" quiescent_destroyed_owner=1 actual_stale_ui_callback=1 stale_refused=1 post_destruction_render=not_applicable\n";continue;}
        CHECK(!r.p->requestSaturationAudition(token,SaturationLane::high,rack,revision,reset));r.p->retireSaturationAudition(token);
        if(r.graph){r.graph->endBandAudition();r.graph->mouseUp(mouse(*r.graph,{0,0},false));}
        SoundDocument expected=r.sound();juce::MemoryBlock expectedBytes;CHECK(encodeSoundState(expected,expectedBytes));
        if(event==21){r.p->prepareToPlay(rate,64);normal->prepareToPlay(rate,64);}
        const int restoreFrames=int(std::round(.020*rate));bool exactLast=false;double restoredEnergy=0;
        for(int first=0;first<restoreFrames;first+=64){const int count=std::min(64,restoreFrames-first);audio.setSize(2,count,false,false,true);reference.setSize(2,count,false,false,true);
            midi.clear();if(stopped&&first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);r.p->processBlock(audio,midi);normal->processBlock(reference,midi);
            for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n)restoredEnergy+=std::abs(reference.getSample(ch,n));
            if(first+count==restoreFrames)exactLast=audio.getSample(0,count-1)==reference.getSample(0,count-1)&&audio.getSample(1,count-1)==reference.getSample(1,count-1);}
        CHECK(exactLast);CHECK(restoredEnergy>1e-12);CHECK(r.sound()==expected);juce::MemoryBlock after;CHECK(encodeSoundState(r.sound(),after)&&after==expectedBytes);
        audio.setSize(2,64,false,false,true);reference.setSize(2,64,false,false,true);midi.clear();r.p->processBlock(audio,midi);normal->processBlock(reference,midi);
        bool exactFollowing=true;for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)exactFollowing=audio.getSample(ch,n)==reference.getSample(ch,n)&&exactFollowing;CHECK(exactFollowing);
        if(event<10||event>=18){CHECK(expectedBytes==saved);}
        CHECK(low.starts==0&&low.ends==0&&high.starts==0&&high.ends==0);
        const bool undone=r.p->undoSoundEdit(),controlUndone=normal->undoSoundEdit();CHECK(undone==controlUndone);SoundDocument undoneControl;CHECK(normal->trySoundSnapshot(undoneControl,controlRevision));CHECK(r.sound()==undoneControl);
        CHECK(monitor->stats(0).published==0&&monitor->stats(0).capturedFrames==0);CHECK(juce::ComponentPeer::getNumPeers()==0);
        std::cout<<"CANCEL_MATRIX "<<names[event]<<" held_energy="<<energy<<" contrast="<<contrast<<" exact_restore_frames="<<restoreFrames<<" exact_final_frame="<<exactLast<<" actual_stale_ui_callback=1 stale_refused=1 state_undo_gestures_preserved=1 peers=0\n";
    }
}
void offscreenEditorDelivery(){
    std::cout<<"SECTION both_factory_complete_editor_hold_keyboard_accessibility_software_images\n";
    const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    for(const auto& factory:airack::test::frozenFactoryRackDesigns())if(factory.units[0].module==NativeModule::saturation){
        Rig r;RackOperation operation;operation.design=factory;operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
        SoundDocument placed;RackDifference diff;CHECK(makeRackCandidate(SoundDocument{},operation,SoundEditOrigin::manual,placed,diff)==RackCandidateStatus::ready);r.install(placed);r.refresh();
        studio::AllControlsPanel panel(*r.p,*model,{});panel.show({{NativeModule::saturation,0}});panel.setSize(1485,panel.shownHeight());panel.setVisible(true);panel.refresh(r.sound());
        auto* graph=dynamic_cast<SaturationGraph*>(panel.findChildWithID("saturation.graph"));CHECK(graph);if(!graph)continue;
        const auto allParameters=panel.parameters();CHECK(allParameters.size()==36);CHECK(panel.keys().size()==5);CHECK(factory.panel.controls.size()==8);
        for(const auto& focused:factory.panel.controls){const auto index=rackParameterIndex(NativeModule::saturation,0,focused.key);CHECK(index);if(index)CHECK(std::find(allParameters.begin(),allParameters.end(),*index)!=allParameters.end());}
        for(std::size_t field=0;field<35;++field){CHECK(panel.showParameter(saturationParameterStart+field));auto* knob=panel.knob(saturationParameterStart+field);CHECK(knob);if(knob)CHECK(std::abs(knob->getValue()-parameterPhysicalValue(r.sound().values,saturationParameterStart+field))<.01f);}
        CHECK(panel.showParameter(572));CHECK(panel.knob(572));panel.showKey(0);panel.setSize(1485,panel.shownHeight());
        const auto refresh=[&]{r.refresh();panel.refresh(r.sound());panel.setSize(1485,panel.shownHeight());};
        const auto capture=[&](const char* stage){juce::MessageManager::getInstance()->runDispatchLoopUntil(1);juce::Image image(juce::Image::ARGB,panel.getWidth(),panel.getHeight(),true,juce::SoftwareImageType());{juce::Graphics graphics(image);panel.paintEntireComponent(graphics,true);}
            CHECK(image.isValid());CHECK(panel.getPeer()==nullptr&&graph->getPeer()==nullptr);CHECK(graph->getBottom()<=panel.getHeight());
            std::set<juce::uint32> colours;for(int y=0;y<image.getHeight();y+=8)for(int x=0;x<image.getWidth();x+=8)colours.insert(image.getPixelAt(x,y).getARGB());CHECK(colours.size()>32);
            const auto fallback=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",juce::File::getCurrentWorkingDirectory().getFullPathName());
            const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SATURATION_SCREENSHOT_ROOT",juce::File(fallback).getChildFile("saturation-offscreen").getFullPathName());CHECK(root.isNotEmpty());
            if(root.isNotEmpty()){const auto file=juce::File(root).getChildFile(factory.name.removeCharacters(" ")+"-"+stage+".png");CHECK(file.getParentDirectory().createDirectory());auto stream=file.createOutputStream();CHECK(stream);if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(image,*stream));}
            std::cout<<"OFFSCREEN_IMAGE factory="<<factory.name<<" stage="<<stage<<" width="<<image.getWidth()<<" height="<<image.getHeight()<<" peers=0\n";
            return image;
        };
        capture("focused");CHECK(!graph->beginBandAudition(SaturationLane::low));
        r.edit([&](auto& d){d.values[572]=0;d.values[r.low-5]=0;});r.p->prepareToPlay(48000,64);refresh();capture("Full");CHECK(!graph->beginBandAudition(SaturationLane::low));
        r.edit([&](auto& d){d.values[r.low-5]=1;});refresh();const auto normalImage=capture("Three-band");
        auto* low=dynamic_cast<juce::Button*>(graph->findChildWithID("saturation.audition.1"));auto* mid=dynamic_cast<juce::Button*>(graph->findChildWithID("saturation.audition.2"));auto* high=dynamic_cast<juce::Button*>(graph->findChildWithID("saturation.audition.3"));CHECK(low&&mid&&high);if(!low||!mid||!high)continue;
        for(auto* button:{low,mid,high}){CHECK(button->isEnabled());CHECK(button->getWidth()>=64&&button->getHeight()>=24);CHECK(!button->getClickingTogglesState());auto handler=static_cast<juce::Component&>(*button).createAccessibilityHandler();CHECK(handler);if(handler){CHECK(handler->getTitle().containsIgnoreCase("audition"));CHECK(handler->getDescription().contains("Overrides Global Mix"));CHECK(handler->getActions().contains(juce::AccessibilityActionType::press));CHECK(!handler->getActions().contains(juce::AccessibilityActionType::toggle));}}
        const auto saved=r.sound();Notifications a(r.p->control(r.low)),b(r.p->control(r.high));
        CHECK(static_cast<juce::Component&>(*mid).keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));CHECK(mid->isDown());CHECK(graph->getDescription().contains("MID held"));
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        for(int block=0;block<17;++block){r.p->processBlock(audio,midi);midi.clear();}CHECK(audio.getMagnitude(0,64)>1e-9);const auto heldImage=capture("held");
        const auto keyArea=panel.getLocalArea(mid,mid->getLocalBounds());unsigned heldPixels=0;for(int y=keyArea.getY();y<keyArea.getBottom();++y)for(int x=keyArea.getX();x<keyArea.getRight();++x)heldPixels+=normalImage.getPixelAt(x,y)!=heldImage.getPixelAt(x,y);CHECK(heldPixels>30);
        CHECK(static_cast<juce::Component&>(*mid).keyStateChanged(false));CHECK(!mid->isDown());CHECK(r.p->fxMonitorHandle()->auditionTailSeconds()>=2.02);r.p->processBlock(audio,midi);capture("restoring");
        for(int block=0;block<16;++block)r.p->processBlock(audio,midi);CHECK(r.p->fxMonitorHandle()->auditionTailSeconds()==0);
        // Ordinary child button state changes execute the same guarded actions.
        low->setState(juce::Button::buttonDown);CHECK(low->isDown());CHECK(graph->getDescription().contains("LOW held"));low->setState(juce::Button::buttonNormal);CHECK(!low->isDown());
        static_cast<juce::Component&>(*low).mouseDown(mouse(*low,{10,10}));CHECK(low->isDown());static_cast<juce::Component&>(*low).mouseUp(mouse(*low,{10,10},false));CHECK(!low->isDown());
        CHECK(static_cast<juce::Component&>(*high).keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));CHECK(high->isDown());CHECK(static_cast<juce::Component&>(*high).keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));CHECK(!high->isDown());
        auto momentary=static_cast<juce::Component&>(*high).createAccessibilityHandler();CHECK(momentary);if(momentary){CHECK(momentary->getActions().invoke(juce::AccessibilityActionType::press));juce::MessageManager::getInstance()->runDispatchLoopUntil(10);CHECK(high->isDown());CHECK(graph->getDescription().contains("HIGH held"));const auto issued=FxMonitorTestAccess::latest(*r.p->fxMonitorHandle(),0);capture("accessibility-pressed");juce::MessageManager::getInstance()->runDispatchLoopUntil(150);CHECK(!high->isDown());CHECK(!r.p->fxMonitorHandle()->requestSaturationAudition(issued,SaturationLane::high));}
        CHECK(r.sound()==saved);CHECK(a.starts==0&&a.ends==0&&b.starts==0&&b.ends==0);
        // The real numeric and dragged transactions remain one grouped undo.
        auto accessible=graph->createAccessibilityHandler();auto* numeric=accessible?accessible->getValueInterface():nullptr;CHECK(numeric);if(numeric)numeric->setValue(1900);
        refresh();CHECK(std::abs(parameterPhysicalValue(r.sound().values,r.low)-1900)<.1f);CHECK(r.p->undoSoundEdit());refresh();CHECK(r.sound()==saved);
        CHECK(graph->beginCrossoverGesture());CHECK(graph->dragCrossovers(1800,500));CHECK(graph->endCrossoverGesture());refresh();CHECK(r.p->undoSoundEdit());refresh();CHECK(r.sound()==saved);
        r.edit([&](auto& d){r.physical(d,r.low,2000);r.physical(d,r.high,16000);d.macros[0]={parameterRegistry()[r.low].stateKey,.5f};d.values[1]=.5f;});r.p->prepareToPlay(8000,64);refresh();
        CHECK(graph->getDescription().contains("High 3600 Hz"));capture("clamped");
        r.edit([&](auto& d){d.effectLocks[0]=true;});refresh();CHECK(!graph->beginCrossoverGesture());CHECK(graph->beginBandAudition(SaturationLane::low));graph->endBandAudition();
        const auto beforeProposal=r.sound();auto proposal=std::make_shared<SoundDocument>(beforeProposal);proposal->values[r.low]=0;graph->refresh(*proposal,0);CHECK(!graph->beginBandAudition(SaturationLane::mid));CHECK(r.sound()==beforeProposal);refresh();
        CHECK(a.starts==a.ends&&b.starts==b.ends);
        CHECK(juce::ComponentPeer::getNumPeers()==0);
    }
}
void keyboardHeldAudition(){
    std::cout<<"SECTION keyboard_audition_survives_unrelated_keys_and_pointer_crossings\n";
    // Replace only the OS key-state query; component callbacks, tokens and audio stay real.
    static bool spaceDown=false,returnDown=false;
    const auto keyQuery=+[](int key){return key==juce::KeyPress::spaceKey?spaceDown:key==juce::KeyPress::returnKey&&returnDown;};
    for(const auto& factory:airack::test::frozenFactoryRackDesigns())if(factory.units[0].module==NativeModule::saturation)
        for(const auto lane:{SaturationLane::low,SaturationLane::mid,SaturationLane::high})
            for(const auto activation:{juce::KeyPress::spaceKey,juce::KeyPress::returnKey}){
                Rig r;RackOperation operation;operation.design=factory;operation.newInstance=juce::Uuid();operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};
                SoundDocument placed;RackDifference diff;CHECK(makeRackCandidate(SoundDocument{},operation,SoundEditOrigin::manual,placed,diff)==RackCandidateStatus::ready);r.install(placed);r.refresh();
                const auto beforeEdit=r.sound();
                r.edit([&](auto& d){d.values[r.low-5]=1;d.values[572]=0;d.values[r.low-2]=0;
                    for(const auto id:{"output.gain_db","output.protection"}){const auto registry=parameterRegistry();const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return juce::String(p.id)==id;});CHECK(found!=registry.end());if(found!=registry.end())r.physical(d,std::size_t(found-registry.begin()),0);}});
                const auto saved=r.sound();juce::MemoryBlock savedBytes;CHECK(encodeSoundState(saved,savedBytes));
                companion::ClientOptions offline;offline.allowLaunch=false;auto held=std::make_unique<InstrumentProcessor>(offline),normal=std::make_unique<InstrumentProcessor>(offline);
                for(auto* processor:{held.get(),normal.get()})CHECK(processor->restoreSoundState(savedBytes.getData(),int(savedBytes.getSize())));
                r.p->prepareToPlay(48000,64);held->prepareToPlay(48000,64);normal->prepareToPlay(48000,64);r.refresh();
                SaturationGraph heldGraph(*held);heldGraph.setVisible(true);heldGraph.refresh(saved,0);
                studio::SaturationGraphTestAccess::keyQuery(*r.graph,keyQuery);
                auto* button=dynamic_cast<juce::Button*>(r.graph->findChildWithID("saturation.audition."+juce::String(int(lane))));CHECK(button);if(!button)continue;
                auto& component=static_cast<juce::Component&>(*button);const auto staleCallback=button->onStateChange;
                Notifications low(r.p->control(r.low)),high(r.p->control(r.high));auto monitor=r.p->fxMonitorHandle();
                juce::AudioBuffer<float> audio(2,64),heldAudio(2,64),normalAudio(2,64);juce::MidiBuffer midi;midi.ensureSize(256);bool first=true;
                const auto render=[&](int count){audio.setSize(2,count,false,false,true);heldAudio.setSize(2,count,false,false,true);normalAudio.setSize(2,count,false,false,true);midi.clear();if(first){midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);first=false;}r.p->processBlock(audio,midi);held->processBlock(heldAudio,midi);normal->processBlock(normalAudio,midi);};
                const auto hold=[&]{spaceDown=activation==juce::KeyPress::spaceKey;returnDown=activation==juce::KeyPress::returnKey;CHECK(component.keyPressed(juce::KeyPress(activation)));CHECK(button->isDown());CHECK(heldGraph.beginBandAudition(lane));for(int block=0;block<17;++block)render(64);};
                const auto verifyHeld=[&](const char* event,FxMonitor::SaturationToken token){
                    CHECK(button->isDown());CHECK(monitor->requestSaturationAudition(token,lane));CHECK(r.graph->getDescription().contains(button->getButtonText()+" held"));
                    bool exact=true;double energy=0,contrast=0;for(int block=0;block<17;++block){render(64);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){exact=exact&&audio.getSample(ch,n)==heldAudio.getSample(ch,n);energy+=std::abs(heldAudio.getSample(ch,n));contrast=std::max(contrast,std::abs(double(heldAudio.getSample(ch,n))-normalAudio.getSample(ch,n)));}}
                    CHECK(exact);CHECK(energy>1e-12);CHECK(contrast>1e-8);CHECK(r.sound()==saved);
                    std::cout<<"KEYBOARD_HOLD factory="<<factory.name<<" lane="<<int(lane)<<" key="<<activation<<" event="<<event<<" same_token="<<button->isDown()<<" exact_held="<<exact<<" energy="<<energy<<" contrast="<<contrast<<'\n';
                };
                for(int event=0;event<6;++event){hold();const auto token=FxMonitorTestAccess::latest(*monitor,0);
                    if(event==0){CHECK(component.keyStateChanged(true));CHECK(component.keyStateChanged(false));}
                    if(event==1){component.keyPressed(juce::KeyPress('A'));CHECK(component.keyStateChanged(true));CHECK(component.keyStateChanged(false));}
                    if(event==2)component.mouseEnter(mouse(*button,{10,10},false));
                    if(event==3)component.mouseExit(mouse(*button,{-10,-10},false));
                    if(event==4)component.mouseDrag(mouse(*button,{-10,-10}));
                    if(event==5)component.mouseUp(mouse(*button,{10,10},false));
                    const std::array names{"modifier_release","unrelated_key_release","pointer_enter","pointer_exit","pointer_drag","pointer_release"};verifyHeld(names[std::size_t(event)],token);
                }
                for(int cancel=0;cancel<3;++cancel){hold();const auto token=FxMonitorTestAccess::latest(*monitor,0);
                    if(cancel==0){spaceDown=returnDown=true;CHECK(component.keyPressed(juce::KeyPress(activation==juce::KeyPress::spaceKey?juce::KeyPress::returnKey:juce::KeyPress::spaceKey)));if(activation==juce::KeyPress::spaceKey)spaceDown=false;else returnDown=false;CHECK(component.keyStateChanged(false));}
                    if(cancel==1)CHECK(component.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
                    if(cancel==2)component.focusLost(juce::Component::focusChangedDirectly);
                    heldGraph.endBandAudition();CHECK(!button->isDown());if(staleCallback)staleCallback();CHECK(!monitor->requestSaturationAudition(token,lane));
                    bool exactLast=false;for(int firstFrame=0;firstFrame<960;firstFrame+=64){const int count=std::min(64,960-firstFrame);render(count);if(firstFrame+count==960)exactLast=audio.getSample(0,count-1)==normalAudio.getSample(0,count-1)&&audio.getSample(1,count-1)==normalAudio.getSample(1,count-1);}CHECK(exactLast);
                    render(64);bool exactFollowing=true;for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)exactFollowing=exactFollowing&&audio.getSample(ch,n)==normalAudio.getSample(ch,n);CHECK(exactFollowing);
                    spaceDown=returnDown=false;component.keyStateChanged(false);CHECK(!button->isDown());
                    std::cout<<"KEYBOARD_CANCEL factory="<<factory.name<<" lane="<<int(lane)<<" key="<<activation<<" event="<<cancel<<" exact_20ms="<<exactLast<<" exact_following="<<exactFollowing<<" stale_refused=1\n";
                }
                juce::MemoryBlock afterBytes;CHECK(encodeSoundState(r.sound(),afterBytes)&&afterBytes==savedBytes);CHECK(low.starts==0&&low.ends==0&&high.starts==0&&high.ends==0);CHECK(monitor->stats(0).published==0&&monitor->stats(0).capturedFrames==0);
                CHECK(r.p->undoSoundEdit());CHECK(r.sound()==beforeEdit);if(!desktopEnabled)CHECK(juce::ComponentPeer::getNumPeers()==0);
            }
}
}
void preparedArtworkRetiresWithGraph(){
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf01");});CHECK(found!=factories.end());if(found==factories.end())return;
    const auto fixtures=prepareTestMakerSkins();RackInstance rack;rack.design=*found;rack.package=legacyRackPackageRef(rack.design);RackInventoryPtr inventory;
    CHECK(std::async(std::launch::async,[&]{return captureRackInventory(*fixtures->store,std::span(&rack,1),inventory);}).get());
    auto owner=std::async(std::launch::async,[&]{return studio::prepareMakerSkin(rack,inventory.get());}).get();CHECK(owner);if(!owner)return;
    std::weak_ptr<const studio::MakerSkin> weak=owner;auto p=std::make_unique<InstrumentProcessor>();auto graph=std::make_unique<SaturationGraph>(*p,studio::house::Host{},owner);owner.reset();CHECK(!weak.expired());
    graph->setBounds(0,0,1400,180);graph->setVisible(true);CHECK(graph->createComponentSnapshot(graph->getLocalBounds()).isValid());graph->retire();p.reset();CHECK(!weak.expired());graph.reset();CHECK(weak.expired());
}
int main(int argc,char** argv){
    const bool merge=argc==2&&juce::String(argv[1])=="--merge-offscreen";
    const bool release=argc==2&&juce::String(argv[1])=="--offscreen-release";
    if(merge||release||(argc==2&&juce::String(argv[1])=="--transactions-only"))desktopEnabled=false;else if(argc!=1)return 2;
    juce::ScopedJuceInitialiser_GUI gui;preparedArtworkRetiresWithGraph();groupedRawCorners();refusalsAndContext();noOpKeepsPreviousUndo();macroAutomationDuringNotificationCancels();proposalShowsMacroAwareBoundaryWithoutWriting();ancestorHideEndsNonfocusedAndReentrantGroups();
    if(merge)mergedDrawerOffscreen();
    if(release){offscreenAuditionCancellation();offscreenEditorDelivery();mergedDrawerOffscreen();reentrantRetirement();}
    if(release||desktopEnabled)keyboardHeldAudition();
    if(desktopEnabled){adaptersRateAndLifecycle();reentrantRetirement();mountedGlobal();preparedEffectiveRateRefresh();}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
