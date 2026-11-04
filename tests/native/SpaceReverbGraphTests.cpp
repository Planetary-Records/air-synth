#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include <future>
#include <stdexcept>
#include "SpaceReverbGraph.h"
#include "RackStack.h"
#include "AllControlsPanel.h"
#include "InstrumentProcessor.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
using namespace airack;
using Graph=studio::SpaceReverbGraph;
int checks=0,failures=0;juce::File images;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<'\n';}}while(false)
bool near(double a,double b,double tolerance=1.e-5){return std::abs(a-b)<tolerance;}
juce::MouseEvent mouse(juce::Component& c,juce::Point<float> at,juce::Point<float> start,bool held=true){const auto time=juce::Time::getCurrentTime();return {juce::Desktop::getInstance().getMainMouseSource(),at,juce::ModifierKeys(held?juce::ModifierKeys::leftButtonModifier:0),1,0,0,0,0,&c,&c,time,start,time,1,held};}
juce::Component* find(juce::Component& c,const juce::String& id){if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* found=find(*child,id))return found;return nullptr;}
void snapshot(const char* name,juce::Component& component){if(images==juce::File())return;CHECK(images.createDirectory().wasOk());const auto file=images.getChildFile(name);CHECK(!file.exists());if(file.exists())return;const auto image=component.createComponentSnapshot(component.getLocalBounds(),true,1);juce::FileOutputStream out(file);CHECK(out.openedOk());CHECK(juce::PNGImageFormat().writeImageToStream(image,out));}
struct Notifications final:juce::AudioProcessorParameter::Listener{
    juce::AudioProcessorParameter& parameter;int starts=0,ends=0,values=0;std::function<void()> onStart,onValue,onEnd;
    explicit Notifications(juce::AudioProcessorParameter& p):parameter(p){p.addListener(this);}~Notifications()override{parameter.removeListener(this);}
    void parameterValueChanged(int,float)override{++values;if(onValue)onValue();}
    void parameterGestureChanged(int,bool start)override{if(start){++starts;if(onStart)onStart();}else{++ends;if(onEnd)onEnd();}}
};
struct Rig{
    std::shared_ptr<testing::IsolatedRackRuntime> runtime=testing::isolatedRackRuntime();std::unique_ptr<InstrumentProcessor> processor;std::unique_ptr<Graph> graph;juce::Uuid rackId;bool closing=false;
    std::size_t base=spaceReverbParameterStart+7*spaceReverbParametersPerSlot;
    Rig(){
        companion::ClientOptions offline;offline.allowLaunch=false;const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
        processor=std::make_unique<InstrumentProcessor>(offline,runtime);
        const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.name=="Lumen Field";});CHECK(found!=designs.end());
        SoundDocument original,placed;RackOperation operation;RackDifference delta;operation.newInstance=juce::Uuid();rackId=operation.newInstance;if(found!=designs.end())operation.design=*found;operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,7}};
        CHECK(makeRackCandidate(original,operation,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
        for(std::size_t b=0;b<6;++b){placed.values[base+12+b*6]=0;placed.values[base+48+b*8]=0;}
        placed.values[base+1]=processor->control(base+1).convertTo0to1(2);placed.values[base+3]=1;restore(placed);
        studio::house::Host host;host.closing=[this]{return closing;};graph=std::make_unique<Graph>(*processor,host);graph->onEdited=[this]{refresh();};graph->setBounds(40,40,1045,274);graph->addToDesktop(juce::ComponentPeer::windowIsTemporary);graph->setVisible(true);refresh();
    }
    SoundDocument sound(){SoundDocument d;std::uint64_t revision;CHECK(processor->trySoundSnapshot(d,revision));return d;}
    void refresh(){if(graph)graph->refresh(sound(),7);}
    void restore(const SoundDocument& d){juce::MemoryBlock wire;CHECK(encodeSoundState(d,wire));CHECK(processor->restoreSoundState(wire.getData(),int(wire.getSize())));refresh();}
    void physical(SoundDocument& d,std::size_t i,float value){d.values[i]=processor->control(i).convertTo0to1(value);}
};
struct PreparedLumenSkin { juce::Image device{juce::Image::ARGB,1,1,true};studio::MakerSkinPtr owner; };
std::unique_ptr<PreparedLumenSkin> prepareLumenSkin(Rig& rig) try {
    ProjectDocument document;std::uint64_t edit=0;
    if(!rig.processor->tryProjectSnapshot(document,edit)||!document.sound.personalities)throw std::runtime_error("Lumen fixture snapshot");
    const auto& racks=document.sound.personalities->instances;
    const auto found=std::find_if(racks.begin(),racks.end(),[&](const auto& rack){return rack.id==rig.rackId;});
    if(found==racks.end()||found->package)throw std::runtime_error("Lumen fixture instance");
    const auto reference=legacyRackPackageRef(found->design);
    if(!reference||reference->id!="lumen-field")throw std::runtime_error("Lumen fixture identity");
    const auto rack=*found;const auto runtime=rig.runtime;
    return std::async(std::launch::async,[runtime,rack,reference]{
        auto result=std::make_unique<PreparedLumenSkin>();
        if(!result->device.isValid())throw std::runtime_error("Lumen fixture native device");
        const auto workspace=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory();
        const auto archive=workspace.getChildFile("build/rack-library-native/releases/"+reference->id+"-"+reference->version+".airrack");
        const auto installed=runtime->store->install(archive,*reference,RackPackageActivation::makeActive,{});
        if(installed!=RackLibraryStatus::ready)throw std::runtime_error("Lumen package installation status "+std::to_string(int(installed)));
        RackInventoryPtr inventory;RackLibraryStatus status{};
        if(!captureRackInventory(*runtime->store,std::span(&rack,1),inventory,&status)||!inventory||!inventory->openPackage(*reference,&status))
            throw std::runtime_error("Lumen package inventory/lease status "+std::to_string(int(status)));
        result->owner=studio::prepareMakerSkin(rack,inventory.get());
        if(!result->owner)throw std::runtime_error("Lumen package skin preparation");
        return result;
    }).get();
} catch(const std::exception& error) {
    std::cerr<<"LUMEN_FIXTURE_ERROR "<<error.what()<<std::endl;return {};
}
void evaluatorMapping(){
    std::cout<<"SECTION shared_unclamped_decay_mapping_bell_shelf_overlap_limits\n";
    space::DecayCurve c;c.brightness=1;c.decaySeconds=2;c.bands[0].active=true;c.bands[0].multiplier=4;
    CHECK(near(space::evaluate(c,1000,48000)->seconds,8));CHECK(near(*Graph::multiplierForTarget(c,0,1000,8,48000),4));
    for(const auto shape:{space::DecayShape::lowShelf,space::DecayShape::highShelf}){c.bands[0].shape=shape;CHECK(near(space::evaluate(c,1000,48000)->seconds,4));CHECK(near(*Graph::multiplierForTarget(c,0,1000,4,48000),4));}
    c.bands[0].shape=space::DecayShape::bell;c.bands[1]={true,false,space::DecayShape::bell,1000,2,1};
    CHECK(near(*Graph::multiplierForTarget(c,0,1000,8,48000),2));c.bands[0].bypassed=true;const auto retained=c;
    CHECK(near(*Graph::multiplierForTarget(c,0,1000,8,48000),2));CHECK(c==retained);CHECK(near(space::evaluate(c,1000,48000)->seconds,4));
    CHECK(near(*Graph::multiplierForTarget(c,0,1000,400,48000),4));CHECK(near(*Graph::multiplierForTarget(c,0,1000,.001,48000),.25));
    c.bands[0].frequencyHz=20000;const auto point=space::evaluate(c,20000,8000);CHECK(point&&point->frequencyLimited&&point->effectiveHz==3600);
    CHECK(!Graph::multiplierForTarget(c,6,1000,2,48000));CHECK(!Graph::multiplierForTarget(c,0,1000,0,48000));CHECK(!Graph::multiplierForTarget(c,0,20001,2,48000));
    c.bands[0].bypassed=false;c.bands[0].multiplier=4;c.bands[1].frequencyHz=20000;c.bands[1].multiplier=4;c.bands[2]=c.bands[1];const auto clipped=space::evaluate(c,20000,48000);CHECK(clipped&&clipped->decayLimited&&clipped->seconds==40);CHECK(near(*Graph::multiplierForTarget(c,0,20000,32,48000),1));
}
void identitiesAndRetention(){
    std::cout<<"SECTION separate_modes_all_six_fixed_identities_retained_add_delete_bypass\n";Rig rig;auto& g=*rig.graph;auto d=rig.sound();
    for(int mode=0;mode<2;++mode){g.setMode(Graph::Mode(mode));const auto bank=mode?48:12,stride=mode?8:6;
        for(std::size_t n=0;n<6;++n){const auto start=rig.base+bank+n*stride;rig.physical(d,start+3,float(117+n*277));rig.physical(d,start+4,mode?-7.f:1.7f);d.values[start+1]=n==2?1:0;}
    }
    rig.restore(d);for(int mode=0;mode<2;++mode){g.setMode(Graph::Mode(mode));const auto bank=mode?48:12,stride=mode?8:6;
        for(std::size_t n=0;n<6;++n){const auto before=rig.sound();CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));CHECK(g.selectedBand()==n);const auto after=rig.sound();CHECK(after.values[rig.base+bank+n*stride]==1);for(std::size_t i=0;i<parameterCount;++i)if(i!=rig.base+bank+n*stride)CHECK(after.values[i]==before.values[i]);}
        const auto full=rig.sound();CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));CHECK(rig.sound()==full);
        g.selectBand(2);CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));const auto removed=rig.sound();CHECK(removed.values[rig.base+bank+2*stride]==0);CHECK(removed.values[rig.base+bank+2*stride+1]==1);for(std::size_t f=1;f<std::size_t(stride);++f)CHECK(removed.values[rig.base+bank+2*stride+f]==full.values[rig.base+bank+2*stride+f]);
        CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));CHECK(g.selectedBand()==2);CHECK(rig.sound()==full);
        const auto before=rig.sound();g.selectBand(4);CHECK(rig.sound()==before);g.setMode(Graph::Mode(1-mode));CHECK(rig.sound()==before);g.setMode(Graph::Mode(mode));CHECK(g.selectedBand()==4);
    }
}
juce::Point<float> pointAt(Graph& g,double hz,double seconds){const auto r=g.plotBounds();return {r.getX()+r.getWidth()*float(std::log(hz/20)/std::log(1000)),r.getBottom()-r.getHeight()*float(std::log(seconds/.1)/std::log(400))};}
void decayDragAndSelectiveUndo(){
    std::cout<<"SECTION composite_seconds_node_drag_grouped_host_gestures_one_undo\n";
    for(const int shape:{0,1,2}){Rig rig;auto d=rig.sound();d.values[rig.base+12]=1;rig.physical(d,rig.base+14,float(shape));rig.physical(d,rig.base+15,1000);rig.physical(d,rig.base+16,1);rig.restore(d);auto& g=*rig.graph;const auto before=rig.sound();
        Notifications freq(rig.processor->control(rig.base+15)),multiplier(rig.processor->control(rig.base+16));const auto start=g.nodePosition(0),target=pointAt(g,1600,shape==0?8:4);
        g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,(start+target)*.5f,start));g.mouseDrag(mouse(g,target,start));g.mouseUp(mouse(g,target,start,false));
        const auto after=rig.sound();CHECK(near(parameterPhysicalValue(after.values,rig.base+15),1600,.01));CHECK(near(parameterPhysicalValue(after.values,rig.base+16),4,1.e-4));CHECK(freq.starts==1&&freq.ends==1&&multiplier.starts==1&&multiplier.ends==1);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);CHECK(rig.processor->redoSoundEdit());CHECK(rig.sound()==after);
    }
    Rig rig;CHECK(rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));const auto before=rig.sound();const auto start=rig.graph->nodePosition(0);rig.graph->mouseDown(mouse(*rig.graph,start,start));rig.graph->mouseDrag(mouse(*rig.graph,start+juce::Point<float>(15,-10),start));CHECK(rig.sound()!=before);
    rig.processor->control(0).setValueNotifyingHost(.37f);const auto concurrent=rig.sound();rig.graph->mouseDrag(mouse(*rig.graph,start+juce::Point<float>(30,-10),start));rig.graph->mouseUp(mouse(*rig.graph,start,start,false));CHECK(rig.sound()==concurrent);
    auto expected=before;expected.values[0]=concurrent.values[0];CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==expected);
}
void postResponseAndQApplicability(){
    std::cout<<"SECTION wet_eq_shared_response_gain_and_shape_specific_q\n";Rig rig;auto& g=*rig.graph;g.setMode(Graph::Mode::postEq);auto d=rig.sound();const auto post=rig.base+48;d.values[post]=1;rig.physical(d,post+3,2000);rig.physical(d,post+4,7);rig.physical(d,post+5,2);rig.physical(d,post+6,1);rig.restore(d);
    for(int shape=0;shape<5;++shape){d=rig.sound();rig.physical(d,post+2,float(shape));rig.restore(d);RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));space::PreparedPostEq prepared;CHECK(space::preparePostEq(patch.effects[7].spaceReverb,48000,prepared));for(const auto hz:{20.,250.,2000.,8000.,20000.}){const auto wanted=space::postEqResponse(prepared,hz),actual=g.postEqAt(hz);CHECK(wanted&&actual);if(wanted&&actual)CHECK(std::abs(*wanted-*actual)<1.e-12);}
        const auto before=rig.sound();juce::MouseWheelDetails wheel;wheel.deltaY=.2f;const auto at=g.nodePosition(0);g.mouseWheelMove(mouse(g,at,at,false),wheel);const auto after=rig.sound();
        if(shape<3){const auto selectedQ=shape==0?5:6,otherQ=shape==0?6:5;CHECK(after.values[post+selectedQ]>before.values[post+selectedQ]);CHECK(after.values[post+otherQ]==before.values[post+otherQ]);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}else CHECK(after==before);
        if(shape>=3){const auto start=g.nodePosition(0);g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,start+juce::Point<float>(20,-30),start));g.mouseUp(mouse(g,start,start,false));CHECK(rig.sound().values[post+4]==before.values[post+4]);CHECK(rig.sound().values[post+5]==before.values[post+5]);CHECK(rig.sound().values[post+6]==before.values[post+6]);}
    }
}
void staleRecallHideAndRetirement(){
    std::cout<<"SECTION stale_reset_binding_hiding_and_reentrant_host_retirement\n";
    for(int action=0;action<8;++action){Rig rig;auto* g=rig.graph.get();CHECK(g->keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));const auto before=rig.sound();const auto start=g->nodePosition(0);Notifications event(rig.processor->control(rig.base+15));
        if(action==4)event.onStart=[&]{rig.graph.reset();};if(action==5)event.onValue=[&]{rig.graph.reset();};if(action==6)event.onEnd=[&]{rig.graph.reset();};
        g->mouseDown(mouse(*g,start,start));
        if(action==0){juce::MemoryBlock wire;CHECK(encodeSoundState(before,wire));CHECK(rig.processor->restoreSoundState(wire.getData(),int(wire.getSize())));}
        if(action==1)g->refresh(before,0);if(action==2)g->setVisible(false);if(action==3)g->focusLost(juce::Component::focusChangedDirectly);if(action==7)rig.closing=true;
        if(rig.graph){g->mouseDrag(mouse(*g,start+juce::Point<float>(30,-20),start));if(rig.graph)g->mouseUp(mouse(*g,start,start,false));}
        CHECK(event.starts==event.ends);CHECK(validateSoundDocument(rig.sound()));if(action<4||action==4||action==7)CHECK(rig.sound()==before);
        if(action>=4&&action<=6)CHECK(!rig.graph);
        event.onStart=nullptr;event.onValue=nullptr;event.onEnd=nullptr;
    }
}
void locksMarksAndAccessibility(){
    std::cout<<"SECTION ai_lock_marks_numeric_reach_mode_keyboard_and_accessibility\n";Rig rig;auto& g=*rig.graph;const auto before=rig.sound();auto candidate=std::make_shared<SoundDocument>(before);candidate->values[rig.base+16]=.9f;g.showProposal(candidate);CHECK(g.proposed());CHECK(g.decayAt(1000,true));CHECK(g.createAccessibilityHandler()!=nullptr);
    auto locked=before;locked.controlLocks[rig.base+16]=true;rig.restore(locked);candidate=std::make_shared<SoundDocument>(locked);candidate->values[rig.base+16]=.9f;g.showProposal(candidate);CHECK(!g.proposed());
    for(const char mode:{'d','p'}){CHECK(g.keyPressed(juce::KeyPress(mode)));for(char n='1';n<='6';++n){CHECK(g.keyPressed(juce::KeyPress(n)));CHECK(g.selectedBand()==std::size_t(n-'1'));}}CHECK(rig.sound()==locked);
    const auto* model=studio::studioSkin();CHECK(model);if(!model)return;studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::spaceReverb,7}});panel.refresh(locked);
    CHECK(panel.parameters().size()==97);
    for(std::size_t offset=0;offset<96;++offset){CHECK(panel.showParameter(rig.base+offset));CHECK(panel.knob(rig.base+offset));}
    CHECK(panel.knob(rig.base+16)->isLocked());
    // A shelf retains bell Q; a cut retains gain and both Qs while disabling their entries.
    auto d=locked;d.values[rig.base+48]=1;rig.physical(d,rig.base+50,1);rig.restore(d);panel.refresh(d);CHECK(!panel.knob(rig.base+53)->isEnabled());CHECK(panel.knob(rig.base+54)->isEnabled());
    rig.physical(d,rig.base+50,3);rig.restore(d);panel.refresh(d);for(const auto offset:{52,53,54})CHECK(!panel.knob(rig.base+offset)->isEnabled());
    for(int family=0;family<2;++family)for(std::size_t band=0;band<6;++band){const auto frequency=rig.base+(family?48+band*8:12+band*6)+3;CHECK(panel.showParameter(frequency));auto* control=panel.knob(frequency);CHECK(control);if(!control)continue;const auto beforeNumeric=rig.sound();Notifications host(rig.processor->control(frequency));control->move(.6f);CHECK(rig.sound().values[frequency]==.6f);CHECK(host.starts==1&&host.ends==1);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==beforeNumeric);}
}
void gestureEndRequestedDuringStart(){
    std::cout<<"SECTION gesture_end_requested_during_host_start\n";
    Rig rig;const auto index=rig.base+15;studio::house::Host ownerHost;
    studio::ParameterGesture gesture(*rig.processor,index,ownerHost,[](float){});Notifications host(rig.processor->control(index));
    host.onStart=[&]{gesture.end();};const bool began=gesture.begin();host.onStart=nullptr;
    CHECK(!began);CHECK(!gesture.held());CHECK(host.starts==1&&host.ends==1);
    const bool reusable=rig.processor->beginParameterGesture(index);CHECK(reusable);if(reusable)CHECK(rig.processor->endParameterGesture(index));else gesture.end();
    CHECK(host.starts==2&&host.ends==2);
}
void gestureOwnerDeletionDuringNotifications(){
    std::cout<<"SECTION gesture_owner_deleted_during_host_start_and_value\n";
    for(const bool destroyOnStart:{true,false}){
        Rig rig;const auto index=rig.base+15;studio::house::Host ownerHost;
        auto gesture=std::make_unique<studio::ParameterGesture>(*rig.processor,index,ownerHost,[](float){});
        Notifications host(rig.processor->control(index));
        if(destroyOnStart)host.onStart=[&]{gesture->end();gesture.reset();};else host.onValue=[&]{gesture->end();gesture.reset();};
        if(destroyOnStart)gesture->begin();else{CHECK(gesture->begin());gesture->write(1500);}
        CHECK(!gesture);CHECK(host.starts==1&&host.ends==1);
        host.onStart=nullptr;host.onValue=nullptr;
        CHECK(rig.processor->beginParameterGesture(index));CHECK(rig.processor->endParameterGesture(index));
        CHECK(host.starts==2&&host.ends==2);
    }
}
void retainedFieldDisableMayRetireItsOwner(){
    std::cout<<"SECTION disabling_retained_field_closes_gesture_even_when_host_deletes_drawer_or_advanced\n";
    for(bool advanced:{false,true}){
        Rig rig;const auto* model=studio::studioSkin();CHECK(model);if(!model)return;auto d=rig.sound();d.values[rig.base+48]=1;rig.physical(d,rig.base+50,0);rig.restore(d);
        std::unique_ptr<studio::AllControlsPanel> panel;std::unique_ptr<studio::ProgrammerUnit> programmer;
        if(advanced){programmer=std::make_unique<studio::ProgrammerUnit>(*rig.processor,*model,studio::ProgrammerHost{});programmer->refresh(d);}
        else{panel=std::make_unique<studio::AllControlsPanel>(*rig.processor,*model,studio::ProgrammerHost{});panel->show({{NativeModule::spaceReverb,7}});panel->refresh(d);CHECK(panel->showParameter(rig.base+53));}
        auto* q=advanced?programmer->knob(rig.base+53):panel->knob(rig.base+53);CHECK(q&&q->isEnabled());if(!q)continue;
        Notifications host(rig.processor->control(rig.base+53));host.onEnd=[&]{if(advanced)programmer.reset();else panel.reset();};
        const auto at=q->getLocalBounds().toFloat().getCentre();q->mouseDown(mouse(*q,at,at));CHECK(host.starts==1&&host.ends==0);
        rig.processor->control(rig.base+50).setValueNotifyingHost(rig.processor->control(rig.base+50).convertTo0to1(3));const auto changed=rig.sound();
        if(advanced)programmer->refresh(changed);else panel->refresh(changed);
        CHECK(advanced?!programmer:!panel);CHECK(host.starts==1&&host.ends==1);CHECK(rig.sound()==changed);host.onEnd=nullptr;CHECK(rig.processor->beginParameterGesture(rig.base+53));CHECK(rig.processor->endParameterGesture(rig.base+53));CHECK(host.starts==2&&host.ends==2);
    }
}
void rawNodesEffectiveMacroCurveAndStaleSelection(){
    std::cout<<"SECTION raw_retained_nodes_effective_macro_curve_and_stale_numeric_callback\n";Rig rig;auto d=rig.sound();d.values[rig.base+12]=1;rig.physical(d,rig.base+14,0);rig.physical(d,rig.base+15,1000);rig.physical(d,rig.base+16,2);
    d.macros[0]={parameterRegistry()[rig.base+16].stateKey,.25f};const auto catalog=parameterRegistry();const auto macro=std::find_if(catalog.begin(),catalog.end(),[](const auto& p){return p.id=="macro.1";});CHECK(macro!=catalog.end());if(macro==catalog.end())return;d.values[std::size_t(macro-catalog.begin())]=.5f;rig.restore(d);
    RoutedPatch effective;CHECK(makeRoutedPatch(d,effective));const auto wanted=space::evaluate(effective.effects[7].spaceReverb.decay,1000,48000),shown=rig.graph->decayAt(1000);CHECK(wanted&&shown);if(wanted&&shown)CHECK(near(wanted->seconds,shown->seconds,1.e-12));
    auto raw=d;raw.macros={};RoutedPatch retained;CHECK(makeRoutedPatch(raw,retained));const auto point=space::evaluate(retained.effects[7].spaceReverb.decay,1000,48000);CHECK(point);if(point){CHECK(rig.graph->nodePosition(0).getDistanceFrom(pointAt(*rig.graph,1000,point->seconds))<.001f);CHECK(shown&&std::abs(shown->seconds-point->seconds)>.1);}
    int callbacks=0;rig.graph->onBandSelected=[&](Graph::Mode,std::size_t){++callbacks;};rig.graph->selectBand(1);CHECK(callbacks==1);
    juce::MemoryBlock wire;CHECK(encodeSoundState(d,wire));CHECK(rig.processor->restoreSoundState(wire.getData(),int(wire.getSize())));const auto before=rig.sound();rig.graph->selectBand(2);CHECK(callbacks==1);CHECK(rig.sound()==before);rig.refresh();rig.graph->selectBand(3);CHECK(callbacks==2);
}
void hostRateLimitsBothDisplaysWithoutChangingValues(){
    std::cout<<"SECTION logical_host_rate_limit_applies_to_both_curves_and_retains_requested_frequency\n";Rig rig;auto d=rig.sound();d.values[rig.base+12]=d.values[rig.base+48]=1;rig.physical(d,rig.base+15,20000);rig.physical(d,rig.base+51,20000);rig.restore(d);
    // A real JUCE host supplies the public logical-rate metadata before prepareToPlay.
    rig.processor->setRateAndBufferSizeDetails(44100,64);rig.processor->prepareToPlay(44100,64);CHECK(rig.processor->getSampleRate()==44100);
    rig.refresh();const auto decay=rig.graph->decayAt(20000);CHECK(decay&&decay->frequencyLimited&&decay->effectiveHz==19845);
    RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));space::PreparedPostEq post;CHECK(space::preparePostEq(patch.effects[7].spaceReverb,44100,post));CHECK(post.effectiveHz[0]==19845);const auto expected=space::postEqResponse(post,20000),actual=rig.graph->postEqAt(20000);CHECK(expected&&actual);if(expected&&actual)CHECK(std::abs(*expected-*actual)<1.e-12);
    CHECK(rig.sound()==d);CHECK(!rig.graph->postEqAt(20001));CHECK(!rig.graph->postEqAt(0));rig.processor->releaseResources();
}
void actualEditorAndImages(){
    std::cout<<"SECTION actual_native_stack_mount_narrow_fold_numeric_and_images\n";Rig rig;CHECK(rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));snapshot("space-decay.png",*rig.graph);rig.graph->setMode(Graph::Mode::postEq);CHECK(rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::insertKey)));snapshot("space-post-eq.png",*rig.graph);rig.graph->setSize(700,274);snapshot("space-graph-narrow.png",*rig.graph);
    for(auto* child:rig.graph->getChildren())if(child->isVisible())CHECK(rig.graph->getLocalBounds().contains(child->getBounds()));
    rig.graph.reset();auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
    auto* graph=dynamic_cast<Graph*>(find(*editor,"space.reverb.graph"));CHECK(graph);if(!graph)return;auto* stack=dynamic_cast<RackStack*>(find(*editor,"rack.stack"));if(!stack)for(auto* ancestor=graph->getParentComponent();ancestor;ancestor=ancestor->getParentComponent())if((stack=dynamic_cast<RackStack*>(ancestor)))break;CHECK(stack);if(!stack)return;
    CHECK(graph->boundSlot()==std::optional<std::uint8_t>(7));graph->selectBand(3);CHECK(stack->drawerRack()!=juce::Uuid::null());CHECK(stack->controls().knob(rig.base+12+3*6+3));snapshot("space-editor-normal.png",*editor);
    const auto units=stack->units();const auto unit=std::find_if(units.begin(),units.end(),[&](const auto* u){return u->spaceGraph()==graph;});CHECK(unit!=units.end());if(unit!=units.end()){(*unit)->setFolded(true);CHECK(!graph->isShowing());CHECK(stack->drawerRack()==juce::Uuid::null());(*unit)->setFolded(false);if(auto* face=(*unit)->face()){CHECK(!face->isOpen()&&!graph->isShowing());face->setOpen(true,false);}CHECK(graph->isShowing());}
    // Committed main 4f scales the complete stage to fit even beside the AI panel.
    // The RackStack narrow mechanism remains independently reachable by its public size API.
    for(const int width:{900,1800}){
        editor->setSize(width,editor->getHeight());juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        CHECK(!stack->narrow());CHECK(!stack->sideways().isVisible());
        if(width==900)snapshot("space-editor-minimum.png",*editor);
        if(auto* ai=dynamic_cast<juce::Button*>(find(*editor,"ai.toggle"))){
            ai->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(70);
            CHECK(!stack->narrow());CHECK(!stack->sideways().isVisible());
            const auto graphArea=editor->getLocalArea(graph,graph->getLocalBounds());CHECK(graphArea.getX()>=0&&graphArea.getRight()<=editor->getWidth());
            if(width==900)snapshot("space-editor-ai-minimum.png",*editor);else snapshot("space-editor-ai-expanded.png",*editor);
            ai->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(70);
        }else CHECK(false);
    }
    const auto fullStackBounds=stack->getBounds();stack->setSize(1000,stack->getHeight());CHECK(stack->narrow());
    if(unit!=units.end()){stack->select((*unit)->id());stack->reveal((*unit)->id());CHECK(graph->isVisible());}
    snapshot("space-stack-narrow.png",*stack);stack->setBounds(fullStackBounds);CHECK(!stack->narrow());
}
void makerControlEditorRetiresDuringHostNotification(){
    std::cout<<"SECTION lumen_editor_retires_during_host_start_and_value\n";
    for(const bool onStart:{true,false}){
        Rig rig;const auto skin=prepareLumenSkin(rig);CHECK(skin);if(!skin)return;rig.graph.reset();
        auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;
        editor->addToDesktop(0);editor->setVisible(true);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
        auto* graph=dynamic_cast<Graph*>(find(*editor,"space.reverb.graph"));CHECK(graph);if(!graph)return;
        RackStack* stack=nullptr;for(auto* parent=graph->getParentComponent();parent;parent=parent->getParentComponent())if((stack=dynamic_cast<RackStack*>(parent)))break;
        CHECK(stack);if(!stack)return;const auto units=stack->units();const auto placed=std::find_if(units.begin(),units.end(),[&](const auto* u){return u->spaceGraph()==graph;});
        CHECK(placed!=units.end());if(placed==units.end())return;auto* face=(*placed)->face();CHECK(face);if(!face)return;
        std::function<studio::house::BoundSlider*(juce::Component&)> locate=[&](juce::Component& c)->studio::house::BoundSlider*{if(auto* slider=dynamic_cast<studio::house::BoundSlider*>(&c))return slider;for(auto* child:c.getChildren())if(auto* found=locate(*child))return found;return nullptr;};
        auto* slider=locate(*face);CHECK(slider);if(!slider)return;const auto index=slider->element.paramIndex;CHECK(index<parameterCount);if(index>=parameterCount)return;
        juce::Component::SafePointer<studio::house::BoundSlider> deferred(slider);Notifications host(rig.processor->control(index));bool aliveDuring=false;bool closed=false;
        const auto close=[&]{if(closed)return;closed=true;editor.reset();aliveDuring=deferred!=nullptr;};
        if(onStart)host.onStart=close;else host.onValue=close;
        if(onStart){CHECK(bool(slider->onDragStart));if(slider->onDragStart)slider->onDragStart();}
        else slider->setValue(slider->getMinimum()+(slider->getMaximum()-slider->getMinimum())*.75,juce::sendNotificationSync);
        CHECK(closed&&aliveDuring);host.onStart=nullptr;host.onValue=nullptr;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        CHECK(deferred==nullptr);CHECK(host.starts==1&&host.ends==1);CHECK(rig.processor->beginParameterGesture(index));CHECK(rig.processor->endParameterGesture(index));CHECK(host.starts==2&&host.ends==2);
    }
}
void makerViewportRealActionsAndMinimumWindow(){
    std::cout<<"SECTION lumen_live_amber_viewport_real_band_actions_minimum_ai_and_numeric_access\n";Rig rig;const auto skin=prepareLumenSkin(rig);CHECK(skin);if(!skin)return;rig.graph.reset();auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
    auto* graph=dynamic_cast<Graph*>(find(*editor,"space.reverb.graph"));CHECK(graph);if(!graph)return;RackStack* stack=nullptr;for(auto* parent=graph->getParentComponent();parent;parent=parent->getParentComponent())if((stack=dynamic_cast<RackStack*>(parent)))break;CHECK(stack);if(!stack)return;
    const auto units=stack->units();const auto placed=std::find_if(units.begin(),units.end(),[&](const auto* u){return u->spaceGraph()==graph;});CHECK(placed!=units.end());if(placed==units.end())return;auto* face=(*placed)->face();CHECK(face);if(!face)return;
    CHECK(!face->isOpen()&&!graph->isShowing());face->setOpen(true,false);CHECK(graph->isShowing()&&graph->getParentComponent()==&face->revealed()&&graph->boundSlot()==std::optional<std::uint8_t>(7));
    const auto action=[&](const char* id){auto* button=dynamic_cast<juce::Button*>(find(*graph,id));CHECK(button&&button->isShowing()&&button->isEnabled());if(button){button->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(90);}};
    const auto untouched=rig.sound();action("space.add");auto active=rig.sound();CHECK(active.values[rig.base+12]==1&&graph->selectedBand()==0);
    action("space.bypass");const auto bypassed=rig.sound();CHECK(bypassed.values[rig.base+13]==1);for(std::size_t i=0;i<parameterCount;++i)if(i!=rig.base+13)CHECK(bypassed.values[i]==active.values[i]);
    action("space.delete");const auto removed=rig.sound();CHECK(removed.values[rig.base+12]==0);for(std::size_t field=1;field<6;++field)CHECK(removed.values[rig.base+12+field]==bypassed.values[rig.base+12+field]);
    action("space.active");CHECK(rig.sound()==bypassed);action("space.post.eq");CHECK(graph->mode()==Graph::Mode::postEq&&rig.sound()==bypassed);action("space.add");CHECK(rig.sound().values[rig.base+48]==1);
    action("space.controls");CHECK(stack->drawerRack()==(*placed)->id()&&stack->controls().parameters().size()==97);CHECK(stack->controls().knob(rig.base+51));
    action("space.decay");const auto beforeSelection=rig.sound();for(char n='1';n<='6';++n){CHECK(graph->keyPressed(juce::KeyPress(n)));CHECK(graph->selectedBand()==std::size_t(n-'1'));}CHECK(rig.sound()==beforeSelection);
    CHECK(rig.processor->undoSoundEdit());CHECK(rig.processor->redoSoundEdit());CHECK(validateSoundDocument(untouched));
    stack->closeDrawer();
    for(bool ai:{false,true}){
        if(ai){auto* toggle=dynamic_cast<juce::Button*>(find(*editor,"ai.toggle"));CHECK(toggle);if(toggle){toggle->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(90);}}
        graph->grabKeyboardFocus();juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
        const auto viewport=editor->getLocalArea(graph,graph->getLocalBounds());CHECK(viewport.getWidth()>300&&viewport.getHeight()>90&&viewport.getX()>=0&&viewport.getRight()<=editor->getWidth());
        CHECK(stack->viewport().getLocalBounds().contains(stack->viewport().getLocalArea(graph,graph->getLocalBounds())));
        for(auto* child:graph->getChildren())if(child->isVisible()){CHECK(graph->getLocalBounds().contains(child->getBounds()));const auto hit=editor->getLocalArea(child,child->getLocalBounds());CHECK(hit.getWidth()>=18&&hit.getHeight()>=12);CHECK(child->createAccessibilityHandler());}
        snapshot(ai?"lumen-editor-ai-1080.png":"lumen-editor-1080.png",*editor);
    }
    editor->setSize(1800,1000);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);snapshot("lumen-editor-ai-expanded.png",*editor);
}
}
int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);juce::ScopedJuceInitialiser_GUI juce;if(argc==2&&juce::String(argv[1])=="--mapping"){evaluatorMapping();std::cout<<"Space Reverb graph mapping: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--images")images=juce::File(argv[2]);else if(argc!=1)return 2;evaluatorMapping();identitiesAndRetention();decayDragAndSelectiveUndo();postResponseAndQApplicability();staleRecallHideAndRetirement();locksMarksAndAccessibility();gestureEndRequestedDuringStart();gestureOwnerDeletionDuringNotifications();retainedFieldDisableMayRetireItsOwner();rawNodesEffectiveMacroCurveAndStaleSelection();hostRateLimitsBothDisplaysWithoutChangingValues();actualEditorAndImages();makerControlEditorRetiresDuringHostNotification();makerViewportRealActionsAndMinimumWindow();std::cout<<"Space Reverb graph: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
