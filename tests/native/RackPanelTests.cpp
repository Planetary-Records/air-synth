#include "FrozenRackExamples.h"
#include "RackPanel.h"
#include "RackStack.h"
#include "RackExamples.h"
#include "RackOrder.h"
#include "RackReview.h"
#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "RackLibrary.h"
#include "AiUnit.h"
#include "MakerFace.h"
#include "ScreenDriver.h"
#include "SoundState.h"
#include "StudioSkin.h"
#include "ProgrammerLook.h"
#include "ProgrammerUnit.h"
#include "ProgrammerValues.h"
#include "PostedClick.h"
#include "PreparedMakerSkins.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <chrono>
#include <iostream>
#include <limits>
#include <cmath>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <Windows.h>
#endif
using namespace airack;
#ifndef NDEBUG
namespace airack {struct InstrumentStructuralTestAccess {static void hook(void (*callback)(int)){InstrumentProcessor::setStructuralTestHook(callback);}};}
#endif
namespace {
int checks=0,failures=0;
std::chrono::steady_clock::time_point observationStart(){static const auto start=std::chrono::steady_clock::now();return start;}
void observation(const char* event,const char* name,int example=-1,int surface=-1,int density=-1){
 const auto start=observationStart();const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
 std::cout<<"RACK_OBSERVATION event="<<event<<" elapsed_ms="<<elapsed<<" name="<<name<<" example="<<example<<" surface="<<surface<<" density="<<density<<std::endl;
}
template<class F>void observeGroup(const char* name,F&& f){observation("begin",name);f();observation("end",name);}
using FocusTick=std::chrono::steady_clock::time_point;
FocusTick focusTick() noexcept {return std::chrono::steady_clock::now();}
struct FocusSpan {
 std::uint64_t count=0,first=0,maximum=0,total=0;bool overflow=false;
 void add(FocusTick a,FocusTick b) noexcept {const auto d=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());
  if(count==std::numeric_limits<std::uint64_t>::max()||total>std::numeric_limits<std::uint64_t>::max()-d){overflow=true;return;}
  if(!count)first=d;maximum=(std::max)(maximum,d);++count;total+=d;}
 void print(const char* name)const{std::cout<<' '<<name<<"_count="<<count<<' '<<name<<"_first_ns="<<first<<' '<<name<<"_max_ns="<<maximum<<' '<<name<<"_total_ns="<<total<<' '<<name<<"_overflow="<<overflow;}
};
struct ThreadCpuSample {std::uint64_t user=0,kernel=0;bool supported=false,valid=false;};
ThreadCpuSample threadCpuSample() noexcept {
#if JUCE_WINDOWS
 FILETIME created{},exited{},kernel{},user{};ThreadCpuSample result;result.supported=true;
 if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)){
  result.user=(std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime;
  result.kernel=(std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime;result.valid=true;}
 return result;
#else
 return {};
#endif
}
struct ThreadCpuSpan {
 std::uint64_t count=0,failed=0,unsupported=0,user100ns=0,kernel100ns=0;bool overflow=false;
 void add(const ThreadCpuSample& a,const ThreadCpuSample& b) noexcept {
  if(!a.supported||!b.supported){if(unsupported==std::numeric_limits<std::uint64_t>::max())overflow=true;else ++unsupported;return;}
  if(!a.valid||!b.valid||b.user<a.user||b.kernel<a.kernel){if(failed==std::numeric_limits<std::uint64_t>::max())overflow=true;else ++failed;return;}
  const auto user=b.user-a.user,kernel=b.kernel-a.kernel;
  if(count==std::numeric_limits<std::uint64_t>::max()||user100ns>std::numeric_limits<std::uint64_t>::max()-user||kernel100ns>std::numeric_limits<std::uint64_t>::max()-kernel){overflow=true;return;}
  ++count;user100ns+=user;kernel100ns+=kernel;
 }
 void print()const{std::cout<<" cpu_count="<<count<<" cpu_failed="<<failed<<" cpu_unsupported="<<unsupported<<" cpu_user_100ns="<<user100ns<<" cpu_kernel_100ns="<<kernel100ns<<" cpu_overflow="<<overflow;}
};
void focusReport(const char* name,const FocusSpan& focus,const FocusSpan& dispatch,const FocusSpan& checks,const FocusSpan& setup,const ThreadCpuSpan& cpu){
 std::cout<<"RACK_FOCUS_SUMMARY name="<<name;focus.print("focus");dispatch.print("pump");checks.print("checks");setup.print("setup");cpu.print();std::cout<<std::endl;
}
void editorReport(FocusTick a,FocusTick b,FocusTick c,FocusTick d,FocusTick e,const ThreadCpuSpan& cpu){
 const auto elapsed=[](FocusTick x,FocusTick y){return std::chrono::duration_cast<std::chrono::nanoseconds>(y-x).count();};
 std::cout<<"RACK_EDITOR_SUMMARY create_ns="<<elapsed(a,b)<<" attach_ns="<<elapsed(b,c)<<" show_ns="<<elapsed(c,d)<<" pump_ns="<<elapsed(d,e);cpu.print();std::cout<<std::endl;
}


#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;} } while(false)
juce::Component* find(juce::Component& c,const juce::String& id){if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* hit=find(*child,id))return hit;return nullptr;}
// A PROGRAMMER knob's press and release on its own mouse handlers, which open and end its gesture.
juce::MouseEvent mouse(juce::Component& c,juce::ModifierKeys mods){const auto t=juce::Time::getCurrentTime();const auto at=c.getLocalBounds().getCentre().toFloat();return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,juce::MouseInputSource::defaultTiltY,&c,&c,t,at,t,1,false);}
void grab(juce::Component& c){c.mouseDown(mouse(c,juce::ModifierKeys::leftButtonModifier));}
void letGo(juce::Component& c){c.mouseUp(mouse(c,{}));}
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(60);}
template<class T>T* widget(juce::Component& c,const juce::String& id){auto* p=dynamic_cast<T*>(find(c,id));CHECK(p);return p;}
void click(juce::Button& b){CHECK(clickPosted(b));pump();}
void click(juce::Component& c,const char* id){if(auto* b=widget<juce::Button>(c,id)){CHECK(b->isEnabled());click(*b);}}
void choice(juce::Component& c,const char* id,int value){if(auto* b=widget<juce::ComboBox>(c,id)){
 if(juce::String(id)=="rack.examples"&&value>=1&&value<=6){const auto wanted=airack::test::frozenExampleRackDesigns()[std::size_t(value-1)].name;const auto end=juce::Time::getMillisecondCounter()+60000;int found=0;
  while(!found&&juce::Time::getMillisecondCounter()<end){for(int n=0;n<b->getNumItems();++n)if(b->getItemText(n)==wanted){found=b->getItemId(n);break;}if(!found)pump();}CHECK(found!=0);value=found;
 }
 b->setSelectedId(value,juce::sendNotificationSync);pump();}}
void readyPlacement(juce::Component& panel){auto* button=widget<juce::Button>(panel,"rack.apply");if(!button)return;const auto end=juce::Time::getMillisecondCounter()+60000;while(!button->isEnabled()&&juce::Time::getMillisecondCounter()<end)pump();CHECK(button->isEnabled());}
void text(juce::Component& c,const char* id,const juce::String& value){if(auto* b=widget<juce::TextEditor>(c,id)){b->setText(value,false);if(b->onTextChange)b->onTextChange();}}
SoundDocument sound(InstrumentProcessor& p){SoundDocument d;std::uint64_t e;CHECK(p.trySoundSnapshot(d,e));return d;}
std::uint64_t revision(InstrumentProcessor& p){SoundDocument d;std::uint64_t e=0;CHECK(p.trySoundSnapshot(d,e));return e;}
companion::ClientOptions offline(){companion::ClientOptions o;o.allowLaunch=false;return o;}
// createOutputStream appends to an existing file, so a rerun into the same folder would leave two PNGs in one file.
std::unique_ptr<juce::FileOutputStream> fresh(const juce::File& file){file.deleteFile();return file.createOutputStream();}
// The stack's unit for a placed rack.
RackStackUnit* stackUnit(juce::Component& c,juce::Uuid id){if(auto* u=dynamic_cast<RackStackUnit*>(&c);u!=nullptr&&u->id()==id)return u;for(auto* child:c.getChildren())if(auto* hit=stackUnit(*child,id))return hit;return nullptr;}
// A placed rack's `rack.focus.N` controls stay in its unit's generic pane, hidden under its faceplate, and take no focus;
// each control the unit shows takes it and comes into the stack's view, changing nothing.
void unitFocusInView(InstrumentProcessor& p,juce::Component& unit,juce::Viewport& view,const juce::String& size){
 FocusSpan focusSpan,pumpSpan,checkSpan,setupSpan;ThreadCpuSpan pumpCpu;const auto setupBegin=focusTick();const auto before=sound(p);std::vector<juce::Component::SafePointer<juce::Component>> shown;
 std::function<void(juce::Component&)> collect=[&](juce::Component& c){for(auto* child:c.getChildren()){if(child->isShowing()&&child->getWantsKeyboardFocus())shown.emplace_back(child);collect(*child);}};
 collect(unit);CHECK(!shown.empty());setupSpan.add(setupBegin,focusTick());
 for(auto& c:shown){if(c==nullptr)continue;const auto focusBegin=focusTick();c->grabKeyboardFocus();const auto focusEnd=focusTick();const auto cpuBefore=threadCpuSample();pump();const auto cpuAfter=threadCpuSample();const auto pumpEnd=focusTick();pumpCpu.add(cpuBefore,cpuAfter);focusSpan.add(focusBegin,focusEnd);pumpSpan.add(focusEnd,pumpEnd);if(c!=nullptr&&!c->hasKeyboardFocus(true)){auto* focused=juce::Component::getCurrentlyFocusedComponent();std::cout<<"FOCUS expected="<<c->getComponentID()<<" actual="<<(focused?focused->getComponentID():"none")<<" actual_name="<<(focused?focused->getName():"none")<<" showing="<<c->isShowing()<<" size="<<size<<std::endl;}CHECK(c!=nullptr&&c->hasKeyboardFocus(true));if(c==nullptr){checkSpan.add(pumpEnd,focusTick());continue;}const auto area=view.getLocalArea(c,c->getLocalBounds());CHECK(view.getLocalBounds().contains(area));if(!view.getLocalBounds().contains(area))std::cout<<"  "<<c->getComponentID()<<" at "<<area.toString()<<" outside the stack's view at "<<size<<std::endl;CHECK(sound(p)==before);checkSpan.add(pumpEnd,focusTick());}
 focusReport("unit",focusSpan,pumpSpan,checkSpan,setupSpan,pumpCpu);
}
void three_native_panel_hierarchies(const juce::File& images){
 std::cout<<"GROUP three_native_panel_hierarchies"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();
 CHECK(examples[0].name=="Ember Reed"&&examples[1].name=="Glass Current"&&examples[2].name=="Thread Press"&&examples[3].name=="Oxide Return");
 CHECK(examples[0].units.size()==2&&examples[1].units.size()==3&&examples[2].units.size()==2&&examples[3].units.size()==1);
 for(int n=0;n<4;++n)for(int surface=0;surface<2;++surface)for(int density=0;density<2;++density){
  observation("phase","hierarchy_iteration",n,surface,density);
  InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));auto design=examples[n];design.panel.surface=std::uint8_t(surface);design.panel.density=std::uint8_t(density);
  CHECK(validateRackDesign(design,RackValidationMode::portableDesign));juce::MemoryBlock bytes;CHECK(encodeRackDesign(design,bytes));RackDesign decoded;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),decoded));CHECK(decoded==design);
  RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.destination.role=design.role;op.destination.position=design.role==RackRole::synth?1:0;
  CHECK(p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);auto before=sound(p);CHECK(validateSoundDocument(before));
  CHECK(design.panel.controls.size()<=8&&design.panel.sections.size()<=4);RoutedPatch routed;CHECK(makeRoutedPatch(before,routed));observation("phase","hierarchy_panel_create_begin",n,surface,density);RackPanelComponent panel(p,*studio::studioSkin());panel.showInstance(op.newInstance);panel.setVisible(true);panel.addToDesktop(0);panel.refresh(before);pump();observation("phase","hierarchy_panel_shown",n,surface,density);CHECK(sound(p)==before);
  for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(2200,1600)}){
   observation("phase",size.x==1080?"hierarchy_size_small_begin":"hierarchy_size_large_begin",n,surface,density);panel.setSize(size.x,size.y);pump();CHECK(widget<juce::Label>(panel,"rack.header")->getText().contains(design.name));CHECK(widget<juce::ComboBox>(panel,"rack.select")->getText().contains(design.name));
   auto* advanced=widget<juce::Button>(panel,"rack.advanced");CHECK(!advanced->getBounds().isEmpty());
   for(std::size_t c=0;c<design.panel.controls.size();++c){auto* w=find(panel,"rack.focus."+juce::String(int(c)));CHECK(w&&w->isEnabled()&&!w->getBounds().isEmpty());if(w){CHECK(w->getName().isNotEmpty());CHECK(w->getExplicitFocusOrder()>0);if(auto* slider=dynamic_cast<juce::Slider*>(w)){CHECK(slider->getTooltip().contains(" / "));auto* readout=widget<juce::Label>(panel,"rack.value."+juce::String(int(c)));CHECK(readout->getText().isNotEmpty());}}}
   auto* first=widget<juce::Slider>(panel,"rack.focus.0");if(first)CHECK(first->getSliderStyle()==(n==2?juce::Slider::LinearVertical:juce::Slider::RotaryHorizontalVerticalDrag));
   if(n==0){auto* bloom=widget<juce::Slider>(panel,"rack.focus.2");if(bloom)CHECK(bloom->getSliderStyle()==juce::Slider::LinearVertical);}
   CHECK(find(panel,"rack.section.1"));
   if(n==0)CHECK(first->getWidth()>widget<juce::Slider>(panel,"rack.focus.1")->getWidth());
   if(n==1)CHECK(first->getHeight()>widget<juce::Slider>(panel,"rack.focus.2")->getHeight());
   if(n==2)CHECK(find(panel,"rack.section.0")->getY()==find(panel,"rack.section.1")->getY());
   FocusSpan focusSpan,pumpSpan,checkSpan,setupSpan;ThreadCpuSpan pumpCpu;observation("phase",size.x==1080?"hierarchy_small_focus_begin":"hierarchy_large_focus_begin",n,surface,density);
   for(std::size_t c=0;c<design.panel.controls.size();++c){const auto setupBegin=focusTick();auto* w=find(panel,"rack.focus."+juce::String(int(c)));const auto focusBegin=focusTick();w->grabKeyboardFocus();const auto focusEnd=focusTick();const auto cpuBefore=threadCpuSample();pump();const auto cpuAfter=threadCpuSample();const auto pumpEnd=focusTick();pumpCpu.add(cpuBefore,cpuAfter);auto* viewport=widget<juce::Viewport>(panel,"rack.viewport");CHECK(viewport->getLocalBounds().contains(viewport->getLocalArea(w,w->getLocalBounds()).getCentre()));const auto checkEnd=focusTick();setupSpan.add(setupBegin,focusBegin);focusSpan.add(focusBegin,focusEnd);pumpSpan.add(focusEnd,pumpEnd);checkSpan.add(pumpEnd,checkEnd);}
   focusReport("hierarchy",focusSpan,pumpSpan,checkSpan,setupSpan,pumpCpu);
   observation("phase",size.x==1080?"hierarchy_size_small_focus_done":"hierarchy_size_large_focus_done",n,surface,density);
   if(images!=juce::File{}){auto stem=juce::String(n)+"-"+juce::String(surface)+"-"+juce::String(density)+"-"+juce::String(size.x);auto* viewport=widget<juce::Viewport>(panel,"rack.viewport");viewport->setViewPosition(0,0);pump();auto image=panel.createComponentSnapshot(panel.getLocalBounds(),true);auto stream=fresh(images.getChildFile(stem+".png"));CHECK(image.isValid()&&stream);if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(image,*stream));juce::MemoryBlock state;CHECK(encodeSoundState(before,state));CHECK(images.getChildFile(stem+".soundstate").replaceWithData(state.getData(),state.getSize()));widget<juce::Button>(panel,"rack.cancel")->grabKeyboardFocus();pump();CHECK(viewport->getLocalBounds().contains(viewport->getLocalArea(find(panel,"rack.cancel"),find(panel,"rack.cancel")->getLocalBounds()).getCentre()));auto library=panel.createComponentSnapshot(panel.getLocalBounds(),true);auto tail=fresh(images.getChildFile(stem+"-library.png"));CHECK(tail&&juce::PNGImageFormat().writeImageToStream(library,*tail));}
  }
  // Native focus and snapshots are complete; callback checks keep the component alive offscreen.
  panel.removeFromDesktop();
  if(surface==examples[n].panel.surface&&density==examples[n].panel.density){observation("phase","hierarchy_editor_create_begin",n,surface,density);const auto editorBegin=focusTick();const auto editorCpuBefore=threadCpuSample();std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());const auto editorCpuAfter=threadCpuSample();const auto editorCreated=focusTick();ThreadCpuSpan editorCpu;editorCpu.add(editorCpuBefore,editorCpuAfter);editor->addToDesktop(0);const auto editorAttached=focusTick();editor->setVisible(true);const auto editorVisible=focusTick();pump();const auto editorPumped=focusTick();editorReport(editorBegin,editorCreated,editorAttached,editorVisible,editorPumped,editorCpu);observation("phase","hierarchy_editor_shown",n,surface,density);auto* unit=stackUnit(*editor,op.newInstance);CHECK(unit!=nullptr);if(unit==nullptr)continue;click(unit->operations);for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(2200,1600)}){editor->setSize(size.x,size.y);pump();auto* focus=widget<juce::Component>(*editor,"rack.focus.0");auto* viewport=widget<juce::Viewport>(*editor,"rack.stack");auto* readout=widget<juce::Label>(*editor,"rack.value.0");CHECK(readout->getText().isNotEmpty());auto* label=dynamic_cast<juce::Label*>(focus->getParentComponent()->getChildComponent(0));CHECK(label&&label->getText()==design.panel.controls[0].label);observation("phase",size.x==1080?"hierarchy_editor_small_focus_begin":"hierarchy_editor_large_focus_begin",n,surface,density);unitFocusInView(p,*unit,*viewport,size.toString());observation("phase",size.x==1080?"hierarchy_editor_small_focus_done":"hierarchy_editor_large_focus_done",n,surface,density);if(images!=juce::File{}){auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true);auto stream=fresh(images.getChildFile("editor-"+juce::String(n)+"-"+juce::String(size.x)+".png"));CHECK(stream&&juce::PNGImageFormat().writeImageToStream(image,*stream));}click(*editor,"rack.advanced");CHECK(find(*editor,"pages")->isVisible());/* The protected Advanced editor retains its first758 contract; full host registry admission is exercised in InstrumentTests. */for(std::size_t i=0;i<758;++i)CHECK(find(*editor,parameterRegistry()[i].id));click(*editor,"rack.toggle");}}
  const auto& instance=before.personalities->instances.back();const auto& control=design.panel.controls[0];const auto b=std::find_if(instance.bindings.begin(),instance.bindings.end(),[&](auto v){return v.unit==control.unit;});const auto index=*rackParameterIndex(design.units[0].module,b->slot,control.key);
  auto* slider=widget<juce::Slider>(panel,"rack.focus.0");if(!slider)continue;
  struct Listener:juce::AudioProcessorParameter::Listener{int begin=0,end=0;std::function<void()> action;void parameterValueChanged(int,float)override{}void parameterGestureChanged(int,bool b)override{if(b){++begin;if(action)action();}else ++end;}} listener;
  p.control(index).addListener(&listener);if(slider->onDragStart)slider->onDragStart();slider->setValue((control.minimum+control.maximum)*.5,juce::sendNotificationSync);if(slider->onDragEnd)slider->onDragEnd();CHECK(listener.begin==1&&listener.end==1);
  auto macro=sound(p);macro.macros[0]={parameterRegistry()[index].stateKey,.25f};CHECK(p.applySoundEdit(macro,SoundEditOrigin::manual,revision(p)));p.control(1).setValueNotifyingHost(.6f);panel.refresh(sound(p));pump();CHECK(widget<juce::Label>(panel,"rack.value.0")->getText().contains("Macro 1"));
  auto changed=sound(p);changed.controlLocks[index]=true;CHECK(p.applySoundEdit(changed,SoundEditOrigin::manual,revision(p)));panel.refresh(sound(p));pump();CHECK(slider->isEnabled());
  // Full registered range is independent of focused useful range; opening never clamps the atom.
  const auto& descriptor=parameterRegistry()[index];if(control.maximum<descriptor.maximum){p.control(index).setValueNotifyingHost(1);pump();panel.refresh(sound(p));pump();CHECK(p.control(index).getValue()==1);CHECK(!slider->isEnabled());CHECK(widget<juce::Label>(panel,"rack.value.0")->getText().containsIgnoreCase("outside"));}
  if(n<2){const auto engineIndex=13+4*b->slot;p.control(engineIndex).setValueNotifyingHost(n==0?1.f:0.f);panel.refresh(sound(p));pump();slider=widget<juce::Slider>(panel,"rack.focus.0");CHECK(!slider->isEnabled());p.control(engineIndex).setValueNotifyingHost(n==0?0.f:1.f);p.control(index).setValueNotifyingHost(p.control(index).convertTo0to1((control.minimum+control.maximum)*.5f));panel.refresh(sound(p));pump();CHECK(widget<juce::Slider>(panel,"rack.focus.0")->isEnabled());}
  slider=widget<juce::Slider>(panel,"rack.focus.0");listener.action=[&]{panel.showInstance(juce::Uuid::null());pump();};if(slider->onDragStart)slider->onDragStart();if(slider->onDragEnd)slider->onDragEnd();pump();CHECK(listener.begin==listener.end);p.control(index).removeListener(&listener);observation("phase","hierarchy_body_done",n,surface,density);
 }
}
void native_library_atomic_edges(){
 std::cout<<"GROUP native_library_atomic_edges"<<std::endl;InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));CHECK(p.applyRackEdit({RackEditKind::addEffect,0,0,0,EffectKind::drive},revision(p))==RackEditResult::applied);auto original=sound(p);RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);pump();
 // The engine's adopt, which the drawer no longer offers, makes Ember Reed's rack of the synth and the drive as they are.
 const auto ember=airack::test::frozenExampleRackDesigns()[0];RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.allocation={{0,0},{1,0}};RackDesign captured;CHECK(captureGenericRackDesign(original,ember.role,adopt.allocation,ember.panel,juce::Uuid(),"My rack",ember.intent,captured));adopt.design=captured;
 CHECK(p.applyRackOperation(adopt,revision(p))==RackCandidateStatus::ready);panel.showInstance(adopt.newInstance);pump();choice(panel,"rack.position",1);auto adopted=sound(p);CHECK(adopted.values==original.values&&adopted.personalities->instances.size()==1);
 choice(panel,"rack.operation",8);text(panel,"rack.allocation","");choice(panel,"rack.control",2);choice(panel,"rack.resolution",3);text(panel,"rack.remap","analog.noise");click(panel,"rack.apply");CHECK(sound(p).personalities->instances[0].design.panel.controls[0].key=="analog.noise");
 choice(panel,"rack.control",2);choice(panel,"rack.resolution",2);click(panel,"rack.apply");CHECK(sound(p).personalities->instances[0].design.panel.controls.size()==5);
 choice(panel,"rack.resolution",1);choice(panel,"rack.operation",7);click(panel,"rack.apply");CHECK(!sound(p).personalities||sound(p).personalities->instances.empty());CHECK(sound(p).layout.synthUse[0]==SlotUse::retired);
 // A focused callback must retain its widget/attachment while a host callback deletes the panel and pumps messages.
 InstrumentProcessor q(offline(),preparedTestRackRuntime());CHECK(blankInstance(q));RackOperation op;op.newInstance=juce::Uuid();op.design=airack::test::frozenExampleRackDesigns()[0];op.destination.position=1;CHECK(q.applyRackOperation(op,revision(q))==RackCandidateStatus::ready);auto dying=std::make_unique<RackPanelComponent>(q,*studio::studioSkin());dying->showInstance(op.newInstance);dying->setSize(1080,680);pump();auto* slider=widget<juce::Slider>(*dying,"rack.focus.0");juce::Component::SafePointer<juce::Slider> retained(slider);auto callback=slider->onDragStart;
 struct Listener:juce::AudioProcessorParameter::Listener{int starts=0,ends=0;std::function<void()> action;void parameterValueChanged(int,float)override{}void parameterGestureChanged(int,bool b)override{if(b){++starts;action();}else ++ends;}} listener;
 listener.action=[&]{dying.reset();pump();CHECK(retained!=nullptr);};q.control(62).addListener(&listener);callback();pump();CHECK(listener.starts==1&&listener.ends==1);CHECK(retained==nullptr);q.control(62).removeListener(&listener);
}
void native_library_and_ai_modes(){
 std::cout<<"GROUP native_library_and_ai_modes"<<std::endl;InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);pump();choice(panel,"rack.examples",1);readyPlacement(panel);const auto initial=sound(p);
 CHECK(find(panel,"rack.review")==nullptr&&widget<juce::Button>(panel,"rack.apply")->isEnabled());click(panel,"rack.cancel");CHECK(sound(p)==initial);
 click(panel,"rack.apply");CHECK(widget<juce::TextEditor>(panel,"rack.diff")->getText().containsIgnoreCase("bank"));auto placed=sound(p);CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities)return;
 choice(panel,"rack.operation",5);click(panel,"rack.apply");CHECK(sound(p).personalities->instances.size()==2);
 choice(panel,"rack.operation",8);choice(panel,"rack.position",1);click(panel,"rack.cancel");
 choice(panel,"rack.operation",6);click(panel,"rack.apply");CHECK(sound(p).personalities->instances.size()==1);
 // Signed out, the AI unit shows the sign-in page and GENERATE refuses without starting the companion.
 FakeServices files;studio::AiUnit ai(p,*studio::studioSkin(),files.make());auto* screen=screenOf(ai,"ai.screen");CHECK(screen);if(!screen)return;
 CHECK(screen->page=="signed_out");click(ai,"ai.generate");CHECK(screen->shown.status=="Sign in and connect to generate");CHECK(p.aiWorkflowStatus().connection.phase==companion::ClientPhase::closed&&!p.aiCurrentRequest());
}
void native_library_dependency_transaction(){
 std::cout<<"GROUP native_library_dependency_transaction_permuted"<<std::endl;InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));auto examples=airack::test::frozenExampleRackDesigns();RackOperation synth;synth.design=examples[0];synth.newInstance=juce::Uuid();synth.destination.position=0;synth.allocation={{0,0},{1,7}};synth.allowSlotReuse=true;CHECK(p.applyRackOperation(synth,revision(p))==RackCandidateStatus::ready);
 RackOperation fx;fx.design=examples[2];fx.newInstance=juce::Uuid();fx.destination={RackRole::fx,0,1};fx.allocation={{0,0},{1,6}};CHECK(p.applyRackOperation(fx,revision(p))==RackCandidateStatus::ready);
 auto before=sound(p);auto metadata=std::make_shared<RackPersonalityState>(*before.personalities);for(auto& i:metadata->instances)std::reverse(i.bindings.begin(),i.bindings.end());before.personalities=metadata;CHECK(validateSoundDocument(before));juce::MemoryBlock bytes;CHECK(encodeSoundState(before,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));CHECK(sound(p)==before);
 RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);panel.showInstance(synth.newInstance);pump();CHECK(widget<juce::Label>(panel,"rack.header")->getText().contains("Synth 1;"));choice(panel,"rack.operation",7);click(panel,"rack.apply");CHECK(sound(p)==before);
 auto* move=widget<juce::ToggleButton>(panel,"rack.moveDependencies");if(!move)return;move->setToggleState(true,juce::sendNotificationSync);pump();click(panel,"rack.apply");CHECK(widget<juce::TextEditor>(panel,"rack.diff")->getText().contains("Shared Master"));auto after=sound(p);CHECK(after.personalities&&after.personalities->instances.size()==1);if(after.personalities&&after.personalities->instances.size()==1){CHECK(after.personalities->instances[0].id==fx.newInstance);for(auto b:after.personalities->instances[0].bindings)CHECK(after.layout.effects[b.slot].chain==masterChain);}CHECK(p.undoSoundEdit());CHECK(sound(p)==before);
}

void focused_failed_repair_retirement() {
#ifndef NDEBUG
 InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackOperation op;op.newInstance=juce::Uuid();op.design=airack::test::frozenExampleRackDesigns()[0];op.destination.position=1;CHECK(p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);
 RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);panel.showInstance(op.newInstance);pump();auto* control=widget<juce::Slider>(panel,"rack.focus.0");if(!control)return;
 const auto index=*rackParameterIndex(NativeModule::analog,1,"analog.filter.cutoff");
 struct Listener:juce::AudioProcessorParameter::Listener {int begins=0,ends=0;void parameterValueChanged(int,float)override{}void parameterGestureChanged(int,bool b)override{if(b)++begins;else ++ends;}} listener;
 p.control(index).addListener(&listener);control->onDragStart();control->setValue(1000,juce::sendNotificationSync);
 const auto target=parameterIndexForKey(parameterRegistry()[*rackParameterIndex(NativeModule::analog,0,"modulation.1.target")].stateKey);CHECK(target);if(!target)return;p.control(*target).setValue(1);
 static thread_local InstrumentProcessor* hooked;hooked=&p;InstrumentStructuralTestAccess::hook(+[](int at){if(at==3)hooked->control(13).setValue(1);});control->onDragEnd();InstrumentStructuralTestAccess::hook(nullptr);
 CHECK(listener.begins==1&&listener.ends==1);CHECK(p.control(13).getValue()==1);p.control(*target).setValue(0);CHECK(p.beginParameterGesture(0));CHECK(p.endParameterGesture(0));p.control(index).removeListener(&listener);panel.refresh(sound(p));pump();
#endif
}
void native_focused_reentry(){
 std::cout<<"GROUP native_focused_reentry"<<std::endl;InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[0];op.newInstance=juce::Uuid();op.destination.position=1;CHECK(p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);auto before=sound(p);RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);panel.showInstance(op.newInstance);pump();auto* slider=widget<juce::Slider>(panel,"rack.focus.0");juce::Component::SafePointer<juce::Slider> old(slider);auto begin=slider->onDragStart;auto recalled=before;auto state=std::make_shared<RackPersonalityState>(*before.personalities);state->instances[0].design.panel.controls[0].key="analog.noise";state->instances[0].design.panel.controls[0].minimum=0;state->instances[0].design.panel.controls[0].maximum=1;state->instances[0].design.panel.controls[0].response=FocusResponse::linear;state->instances[0].design.revision=2;recalled.personalities=state;juce::MemoryBlock bytes;CHECK(encodeSoundState(recalled,bytes));
 struct Listener:juce::AudioProcessorParameter::Listener{int starts=0,ends=0;std::function<void()> action;void parameterValueChanged(int,float)override{}void parameterGestureChanged(int,bool b)override{if(b){++starts;action();}else ++ends;}} listener;
 listener.action=[&]{CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));panel.refresh(sound(p));pump();CHECK(old&&old.getComponent()==find(panel,"rack.focus.0"));CHECK(!old->isEnabled());};p.control(62).addListener(&listener);begin();pump();CHECK(listener.starts==listener.ends);CHECK(old==nullptr);CHECK(sound(p)==recalled);auto* replacement=widget<juce::Slider>(panel,"rack.focus.0");CHECK(replacement->getTooltip().contains("noise"));CHECK(replacement->isEnabled());p.control(62).removeListener(&listener);
 // Destruction during the external end notification also retains the active callback frame.
 auto dying=std::make_unique<RackPanelComponent>(p,*studio::studioSkin());dying->setSize(1080,680);dying->showInstance(op.newInstance);pump();auto* control=widget<juce::Slider>(*dying,"rack.focus.0");juce::Component::SafePointer<juce::Slider> retained(control);struct EndListener:juce::AudioProcessorParameter::Listener{std::function<void()> end;void parameterValueChanged(int,float)override{}void parameterGestureChanged(int,bool b)override{if(!b)end();}} endListener;auto index=*rackParameterIndex(NativeModule::analog,1,"analog.noise");endListener.end=[&]{dying.reset();pump();CHECK(retained);};p.control(index).addListener(&endListener);control->onDragStart();auto end=control->onDragEnd;end();pump();CHECK(!retained);p.control(index).removeListener(&endListener);
 std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->setSize(1080,680);auto* focused=widget<juce::Slider>(*editor,"rack.focus.0");juce::Component::SafePointer<juce::Component> advanced(find(*editor,parameterRegistry()[index].id));CHECK(advanced);juce::Component::SafePointer<juce::Slider> focusedRetained(focused);auto start=focused->onDragStart;listener.starts=listener.ends=0;listener.action=[&]{editor.reset();pump();CHECK(advanced);CHECK(focusedRetained);};p.control(index).addListener(&listener);start();pump();CHECK(!advanced&&!focusedRetained);CHECK(listener.starts==listener.ends);p.control(index).removeListener(&listener);
}
void native_minimum_focus(){
 std::cout<<"GROUP native_minimum_focus"<<std::endl;
 const auto examples=airack::test::frozenExampleRackDesigns();for(int n=0;n<int(examples.size());++n)for(int surface=0;surface<2;++surface)for(int density=0;density<2;++density){
  observation("iteration","native_minimum_focus",n,surface,density);
  InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackOperation op;op.design=examples[n];op.design->panel.surface=std::uint8_t(surface);op.design->panel.density=std::uint8_t(density);op.newInstance=juce::Uuid();op.destination={op.design->role,masterChain,std::uint8_t(op.design->role==RackRole::synth?1:0)};const auto placed=p.applyRackOperation(op,revision(p));CHECK(placed==RackCandidateStatus::ready);if(placed!=RackCandidateStatus::ready)continue;const auto before=sound(p);
  observation("phase","minimum_editor_create_begin",n,surface,density);const auto editorBegin=focusTick();const auto editorCpuBefore=threadCpuSample();std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());const auto editorCpuAfter=threadCpuSample();const auto editorCreated=focusTick();ThreadCpuSpan editorCpu;editorCpu.add(editorCpuBefore,editorCpuAfter);editor->addToDesktop(0);const auto editorAttached=focusTick();editor->setVisible(true);const auto editorVisible=focusTick();pump();const auto editorPumped=focusTick();editorReport(editorBegin,editorCreated,editorAttached,editorVisible,editorPumped,editorCpu);observation("phase","minimum_editor_shown",n,surface,density);
  auto* unit=stackUnit(*editor,op.newInstance);CHECK(unit!=nullptr);if(unit==nullptr)continue;click(unit->operations);
   for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(2200,1600)}){observation("phase",size.x==1080?"minimum_small_begin":"minimum_large_begin",n,surface,density);editor->setSize(size.x,size.y);pump();auto* viewport=widget<juce::Viewport>(*editor,"rack.stack");
   for(std::size_t k=0;k<op.design->panel.controls.size();++k){auto suffix=juce::String(int(k));auto* label=widget<juce::Label>(*editor,"rack.label."+suffix);auto* value=widget<juce::Label>(*editor,"rack.value."+suffix);CHECK(label->getText()==op.design->panel.controls[k].label);CHECK(value->getText().isNotEmpty());}
   observation("phase",size.x==1080?"minimum_small_focus_begin":"minimum_large_focus_begin",n,surface,density);unitFocusInView(p,*unit,*viewport,size.toString());observation("phase",size.x==1080?"minimum_small_focus_done":"minimum_large_focus_done",n,surface,density);CHECK(sound(p)==before);
   widget<juce::Button>(*editor,"rack.advanced")->grabKeyboardFocus();pump();CHECK(viewport->getLocalBounds().contains(viewport->getLocalArea(find(*editor,"rack.advanced"),find(*editor,"rack.advanced")->getLocalBounds())));observation("phase",size.x==1080?"minimum_small_done":"minimum_large_done",n,surface,density);
  }
  observation("phase","minimum_body_done",n,surface,density);
 }
}
void native_advanced_reentry(){
 std::cout<<"GROUP native_advanced_reentry"<<std::endl;
 for(int kind=0;kind<4;++kind)for(int phase=0;phase<3;++phase){const bool route=kind==1;
  InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->setSize(1080,680);pump();
  const std::size_t index=route?255:kind==2?252:kind==3?256:0;auto* registry=find(*editor,parameterRegistry()[index].id);CHECK(registry);juce::Component::SafePointer<juce::Component> retained(registry),other(find(*editor,parameterRegistry()[1].id));
  struct Listener:juce::AudioProcessorParameter::Listener{int starts=0,ends=0,values=0;std::function<void(int)> action;void parameterValueChanged(int,float)override{++values;action(1);}void parameterGestureChanged(int,bool b)override{if(b)++starts;else ++ends;action(b?0:2);}} listener;
  bool closed=false;listener.action=[&](int event){if(event==phase&&!closed){closed=true;editor.reset();pump();CHECK(retained);CHECK(other);}};
  const std::size_t first=kind?252:index,last=kind?257:index+1;for(auto n=first;n<last;++n)p.control(n).addListener(&listener);
  auto* knob=dynamic_cast<studio::ProgrammerKnob*>(registry);CHECK(knob);
  if(knob&&(route||kind==2))knob->setValue(1,juce::sendNotificationSync);
  else if(knob){grab(*knob);if(retained&&phase!=0)knob->setValue(kind==3?.25:-9,juce::sendNotificationSync);if(retained&&phase==2)letGo(*knob);}
  pump();CHECK(closed);CHECK(listener.starts>0&&listener.starts==listener.ends);CHECK(!retained&&!other);for(auto n=first;n<last;++n)p.control(n).removeListener(&listener);
  CHECK(p.beginParameterGesture(0));CHECK(p.endParameterGesture(0));
 }
}
void native_role_destinations(){
 std::cout<<"GROUP native_role_destinations"<<std::endl;
 for(bool exact:{false,true}){
  InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);pump();choice(panel,"rack.examples",1);if(exact)text(panel,"rack.allocation","0:3,1:2");
  RackOperation op;op.newInstance=juce::Uuid();op.design=airack::test::frozenExampleRackDesigns()[0];op.destination.position=1;if(exact)op.allocation={{0,2},{1,1}};SoundDocument candidate;RackDifference delta;CHECK(p.previewRackOperation(op,revision(p),candidate,delta)==RackCandidateStatus::ready);const auto& preview=candidate.personalities->instances.back();const auto bank=preview.bindings.front().slot;
  readyPlacement(panel);click(panel,"rack.apply");auto diff=widget<juce::TextEditor>(panel,"rack.diff")->getText();CHECK(diff.contains("Destination: Synth "+juce::String(bank+1)+"; synth order 2; owned inserts on Synth "+juce::String(bank+1)));CHECK(!widget<juce::ComboBox>(panel,"rack.destination")->isEnabled());CHECK(widget<juce::ComboBox>(panel,"rack.destination")->getTooltip().containsIgnoreCase("synth order"));
  auto placed=sound(p);CHECK(placed.personalities&&!placed.personalities->instances.empty());if(!placed.personalities||placed.personalities->instances.empty())continue;const auto id=placed.personalities->instances.back().id;CHECK(placed.layout==candidate.layout);CHECK(placed.personalities->instances.back().bindings==preview.bindings);panel.showInstance(id);choice(panel,"rack.operation",8);choice(panel,"rack.position",1);text(panel,"rack.allocation","");RackOperation move;move.kind=RackOperationKind::move;move.selected=id;move.destination={RackRole::synth,masterChain,0};CHECK(p.previewRackOperation(move,revision(p),candidate,delta)==RackCandidateStatus::ready);click(panel,"rack.apply");diff=widget<juce::TextEditor>(panel,"rack.diff")->getText();CHECK(diff.contains("Destination: Synth "+juce::String(bank+1)+"; synth order 1; owned inserts on Synth "+juce::String(bank+1)));CHECK(candidate.layout.synthOrder[0]==bank);CHECK(sound(p).layout==candidate.layout);
  choice(panel,"rack.operation",1);choice(panel,"rack.examples",3);CHECK(widget<juce::ComboBox>(panel,"rack.destination")->isEnabled());choice(panel,"rack.destination",5);choice(panel,"rack.position",1);readyPlacement(panel);click(panel,"rack.apply");CHECK(widget<juce::TextEditor>(panel,"rack.diff")->getText().contains("Destination: Shared Master"));
 }
}
void native_library_capacity_and_reset(){
 std::cout<<"GROUP native_library_capacity_and_reset"<<std::endl;InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);pump();choice(panel,"rack.examples",1);for(int n=0;n<3;++n){readyPlacement(panel);click(panel,"rack.apply");}auto full=sound(p);CHECK(full.layout.synthCount==4);readyPlacement(panel);click(panel,"rack.apply");CHECK(widget<juce::TextEditor>(panel,"rack.diff")->getText().contains("Capacity reached"));CHECK(sound(p)==full);CHECK(p.undoSoundEdit());pump();auto before=sound(p);juce::MemoryBlock bytes;CHECK(encodeSoundState(before,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));pump();CHECK(sound(p)==before);CHECK(widget<juce::Label>(panel,"rack.status")->getText().containsIgnoreCase("cancelled"));
}
}
// The standalone panel offers every operation; the drawer only the selected rack's, under its header; a rack unit's panel
// shows the row alone. The drawer and the unit leave the editor's IDs to the stack.
void native_panel_places(){
 std::cout<<"GROUP native_panel_places"<<std::endl;using Place=RackPanelComponent::Place;InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackPanelComponent panel(p,*studio::studioSkin());panel.setSize(1080,680);panel.setVisible(true);panel.addToDesktop(0);pump();
 auto ids=[&]{juce::Array<int> v;if(auto* box=widget<juce::ComboBox>(panel,"rack.operation"))for(int i=0;i<box->getNumItems();++i)v.add(box->getItemId(i));return v;};
 auto showing=[&](const char* id){auto* c=find(panel,id);return c!=nullptr&&c->isShowing();};
 auto heading=[&]{return widget<juce::Label>(panel,"rack.libraryLabel")->getText();};const juce::String designs="Local designs | Apply places";
 CHECK(panel.place()==Place::all&&ids()==(juce::Array<int>{1,4,5,6,7,8})&&showing("rack.apply")&&showing("rack.select")&&heading()==designs);
 panel.setPlace(Place::drawer);pump();
 CHECK(panel.place()==Place::drawer&&ids()==(juce::Array<int>{4,5,6,7,8})&&showing("rack.header")&&showing("rack.advanced")&&!showing("rack.operation")&&heading()==designs);
 CHECK(find(panel,"rack.panel")==nullptr&&find(panel,"rack.viewport")==nullptr&&find(panel,"rack.select")==nullptr);
 panel.setPlace(Place::unit);pump();
 CHECK(!showing("rack.header")&&!showing("rack.advanced")&&!showing("rack.operation")&&!showing("rack.status")&&find(panel,"rack.panel")==nullptr&&find(panel,"rack.select")==nullptr);
 panel.setPlace(Place::all);pump();
 CHECK(find(panel,"rack.panel")!=nullptr&&find(panel,"rack.viewport")!=nullptr&&showing("rack.select")&&ids()==(juce::Array<int>{1,4,5,6,7,8}));
}
// A processor with `design` placed, and a rack panel in `place` showing it at w x h.
struct Shown{InstrumentProcessor p{offline(),preparedTestRackRuntime()};juce::Uuid id;std::unique_ptr<RackPanelComponent> panel;};
std::unique_ptr<Shown> showDesign(const RackDesign& design,RackPanelComponent::Place place,int w,int h){auto s=std::make_unique<Shown>();CHECK(blankInstance(s->p));RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.destination.role=design.role;op.destination.position=design.role==RackRole::synth?1:0;CHECK(s->p.applyRackOperation(op,revision(s->p))==RackCandidateStatus::ready);s->id=op.newInstance;s->panel=std::make_unique<RackPanelComponent>(s->p,*studio::studioSkin());s->panel->setPlace(place);s->panel->showInstance(s->id);s->panel->setSize(w,h);s->panel->refresh(sound(s->p));pump();return s;}
// The parameter a design's control k drives in the last placed rack.
std::size_t controlIndex(InstrumentProcessor& p,const RackDesign& design,std::size_t k){const auto d=sound(p);const auto& i=d.personalities->instances.back();const auto& c=design.panel.controls[k];const auto u=std::find_if(design.units.begin(),design.units.end(),[&](const auto& v){return v.id==c.unit;});const auto b=std::find_if(i.bindings.begin(),i.bindings.end(),[&](auto v){return v.unit==c.unit;});return *rackParameterIndex(u->module,b->slot,c.key);}
double luma(juce::Colour c){return .299*c.getRed()+.587*c.getGreen()+.114*c.getBlue();}
juce::Image snapshot(juce::Component& c){return c.createComponentSnapshot(c.getLocalBounds(),true,1.0f);}
// Count the real header renderer without changing ProgrammerLook or its production label.
struct HeaderPaintLook final:juce::LookAndFeel_V4{
 studio::ProgrammerLook& renderer;int draws=0;bool marker=false;
 explicit HeaderPaintLook(studio::ProgrammerLook& look):renderer(look){}
 void drawLabel(juce::Graphics& g,juce::Label& label)override{++draws;renderer.drawLabel(g,label);if(marker){g.setColour(juce::Colours::red);g.fillRect(0,0,3,3);}}
};
void header_buffering(){
 auto s=showDesign(airack::test::frozenExampleRackDesigns()[3],RackPanelComponent::Place::drawer,1486,668);
 juce::Component holder;holder.setSize(1486,668);holder.addAndMakeVisible(*s->panel);holder.setVisible(true);holder.addToDesktop(0);pump();
 auto* header=widget<juce::Label>(*s->panel,"rack.header");if(!header)return;
 auto* renderer=dynamic_cast<studio::ProgrammerLook*>(&header->getLookAndFeel());CHECK(renderer);if(!renderer)return;
 const auto before=sound(s->p);const auto epoch=revision(s->p);const auto original=header->getText();
 CHECK(original.contains("Oxide Return")&&original.length()>300);
 const bool configured=header->getCachedComponentImage()!=nullptr;CHECK(configured);
 HeaderPaintLook look(*renderer),secondLook(*renderer);secondLook.marker=true;header->setLookAndFeel(&look);
 auto* parent=header->getParentComponent();CHECK(parent);if(!parent){header->setLookAndFeel(nullptr);return;}
 const auto render=[&]{
  juce::Image image(juce::Image::ARGB,header->getWidth(),header->getHeight(),true);juce::Graphics g(image);
  g.fillAll(juce::Colour(0xff1b252e));g.setColour(juce::Colour(0xff72837a));g.fillRect(image.getBounds().withWidth(image.getWidth()/2));
  g.setOrigin(-header->getX(),-header->getY());parent->paintEntireComponent(g,true);return image;
 };
 const auto closePixels=[](const juce::Image& a,const juce::Image& b){
  if(a.getBounds()!=b.getBounds())return false;
  for(int y=0;y<a.getHeight();++y)for(int x=0;x<a.getWidth();++x){const auto p=a.getPixelAt(x,y),q=b.getPixelAt(x,y);
   for(const auto difference:{int(p.getRed())-int(q.getRed()),int(p.getGreen())-int(q.getGreen()),int(p.getBlue())-int(q.getBlue()),int(p.getAlpha())-int(q.getAlpha())})if(std::abs(difference)>1)return false;}
  return true;
 };
 const auto accessible=[&]{auto* a=header->getAccessibilityHandler();CHECK(a);if(a){CHECK(a->getRole()==juce::AccessibilityRole::label);CHECK(a->getTitle()==header->getText());CHECK(a->getHelp()==header->getText());CHECK(a->getValueInterface()&&a->getValueInterface()->getCurrentValueAsString()==header->getText());}CHECK(header->getTooltip()==header->getText()&&!header->getWantsKeyboardFocus()&&!header->isEditable());};
 header->setBufferedToImage(false);look.draws=0;render();render();render();CHECK(look.draws==3);
 header->setBufferedToImage(configured);look.draws=0;const auto first=render();CHECK(look.draws==1);CHECK(closePixels(first,render()));CHECK(look.draws==1);accessible();
 header->setBufferedToImage(true);
 const auto checkCurrent=[&](HeaderPaintLook& active){
  const auto count=active.draws;const auto cached=render();CHECK(active.draws==count+1);CHECK(closePixels(cached,render()));CHECK(active.draws==count+1);
  header->setBufferedToImage(false);const auto reference=render();CHECK(closePixels(cached,reference));header->setBufferedToImage(true);const auto unprimed=active.draws;CHECK(closePixels(cached,render()));CHECK(active.draws==unprimed+1);CHECK(closePixels(cached,render()));CHECK(active.draws==unprimed+1);accessible();
  return cached;
 };
 header->repaint();const auto baseline=checkCurrent(look);
 header->setText("Oxide Return | changed\nA short revised intention.",juce::dontSendNotification);const auto changed=checkCurrent(look);CHECK(!closePixels(baseline,changed));
 header->setSize(header->getWidth()-137,header->getHeight()+17);const auto resized=checkCurrent(look);
 header->setLookAndFeel(&secondLook);const auto changedLook=checkCurrent(secondLook);CHECK(!closePixels(resized,changedLook));
 header->setFont(header->getFont().withHeight(header->getFont().getHeight()+3.0f));checkCurrent(secondLook);
 const auto enabled=render();header->setEnabled(false);const auto disabled=checkCurrent(secondLook);CHECK(!closePixels(enabled,disabled));
 header->setEnabled(true);checkCurrent(secondLook);
 header->setColour(juce::Label::textColourId,juce::Colours::red);checkCurrent(secondLook);
 studio::ProgrammerLook::setRole(*header,studio::ProgrammerLook::Text::plain);header->setText("Oxide Return | Rev 1 | Unlocked",juce::dontSendNotification);const auto plain=checkCurrent(secondLook);
 header->setText("Oxide Return | Rev 2 | AI locked",juce::dontSendNotification);const auto plainText=checkCurrent(secondLook);CHECK(!closePixels(plain,plainText));
 header->setFont(header->getFont().withHeight(header->getFont().getHeight()+5.0f));const auto plainFont=checkCurrent(secondLook);CHECK(!closePixels(plainText,plainFont));
 header->setColour(juce::Label::textColourId,juce::Colours::green);const auto plainColour=checkCurrent(secondLook);CHECK(!closePixels(plainFont,plainColour));
 CHECK(sound(s->p)==before&&revision(s->p)==epoch);
 header->setLookAndFeel(nullptr);
 holder.removeFromDesktop();
 std::cout<<"Header buffering checks="<<checks<<" failures="<<failures<<std::endl;
}
// A label's painting stays inside its own bounds.
bool insideLabel(juce::Component& c){juce::Image img(juce::Image::ARGB,c.getWidth()+200,c.getHeight()+40,true);{juce::Graphics g(img);g.setOrigin({100,20});c.paintEntireComponent(g,true);}
 for(int y=0;y<img.getHeight();++y)for(int x=0;x<img.getWidth();++x)if(img.getPixelAt(x,y).getAlpha()>64&&(x<99||x>100+c.getWidth()||y<19||y>20+c.getHeight()))return false;return true;}
// A rack unit's plate behind a unit's panel, as the stack paints it.
struct Plate final:juce::Component{studio::ProgrammerLook look{*studio::studioSkin()};void paint(juce::Graphics& g)override{look.drawFace(g,getLocalBounds().toFloat());}};
// The design's look waits for the makers' skins: two designs that differ only in it draw the same in a rack unit.
void native_panel_programmer(){
 std::cout<<"GROUP native_panel_programmer"<<std::endl;using Place=RackPanelComponent::Place;const auto examples=airack::test::frozenExampleRackDesigns();
 auto a=examples[0],b=examples[0];
 a.panel.surface=1;a.panel.accent=0;a.panel.frame=0;a.panel.spacing=1;a.panel.density=0;a.panel.layout=PanelLayout::instrumentSections;
 b.panel.surface=0;b.panel.accent=2;b.panel.frame=1;b.panel.spacing=0;b.panel.density=1;b.panel.layout=PanelLayout::performanceControls;
 auto x=showDesign(a,Place::unit,1045,274),y=showDesign(b,Place::unit,1045,274);const auto one=snapshot(*x->panel),two=snapshot(*y->panel);
 bool same=one.getBounds()==two.getBounds();for(int py=0;same&&py<one.getHeight();++py)for(int px=0;same&&px<one.getWidth();++px)same=one.getPixelAt(px,py)==two.getPixelAt(px,py);CHECK(same);
}
// A rack unit's row (the rack stage spec: A rack unit): each example's sections in one PROGRAMMER row from 17 px into the
// panel's 1045 px, centred in its 274 px, each control's name and value under it, the dividers engraved between sections and the
// titles where the rule puts them; no face, ADVANCED or form; 8 controls in 4 sections fit; the lock's dot; long text.
void rack_panel_unit(){
 std::cout<<"GROUP rack_panel_unit"<<std::endl;using Place=RackPanelComponent::Place;const auto& skin=*studio::studioSkin();const auto examples=airack::test::frozenExampleRackDesigns();
 const float dy=std::round((274.0f-197.0f)*.5f)-72.0f;
 for(int n=0;n<4;++n){
  observation("iteration","rack_panel_unit",n);
  const auto& design=examples[n];auto s=showDesign(design,Place::unit,1045,274);auto& panel=*s->panel;Plate plate;plate.setSize(1045,274);plate.addAndMakeVisible(panel);
  std::vector<int> counts(design.panel.sections.size());for(const auto& c:design.panel.controls)++counts[c.section];
  const auto row=studio::layoutRow(skin.programmer,counts,17.0f,1045.0f);
  const auto area=[&](juce::Component& c){return panel.getLocalArea(&c,c.getLocalBounds()).toFloat();};
  const auto shown=[&](juce::Component& c){return panel.getLocalBounds().toFloat().contains(area(c));};
  std::vector<std::size_t> next(counts.size());
  for(std::size_t k=0;k<design.panel.controls.size();++k){
   const auto& spec=design.panel.controls[k];const auto suffix=juce::String(int(k));auto* w=find(panel,"rack.focus."+suffix);auto* name=widget<juce::Label>(panel,"rack.label."+suffix);auto* value=widget<juce::Label>(panel,"rack.value."+suffix);
   const bool found=w&&name&&value&&next[spec.section]<row.centres[spec.section].size();CHECK(found);if(!found)continue;
   const float scale=k==0&&design.panel.emphasis?studio::ProgrammerLook::emphasis:1.0f,cx=row.centres[spec.section][next[spec.section]++];
   const float cy=163.0f+dy-(spec.widget==FocusWidget::verticalSlider?8.0f:spec.widget==FocusWidget::toggle?11.0f:0.0f)*scale;
   const auto r=area(*w),l=area(*name),v=area(*value);
   CHECK(std::abs(r.getCentreX()-cx)<=.5f&&std::abs(r.getCentreY()-cy)<=.5f);
   CHECK(std::abs(l.getCentreX()-cx)<=.5f&&std::abs(l.getY()-(163.0f+dy+65.27f*scale-16.0f))<=1.0f&&std::abs(v.getY()+14.0f-(l.getY()+16.0f)-17.0f)<=1.0f);
   if(spec.widget==FocusWidget::rotary)CHECK(r.getWidth()==(scale>1.0f?118.0f:103.0f)&&r.getHeight()==r.getWidth());
   CHECK(shown(*w)&&shown(*name)&&shown(*value));
  }
  for(std::size_t k=0;k<counts.size();++k){auto* section=find(panel,"rack.section."+juce::String(int(k)));CHECK(section&&section->getY()==int(72.0f+dy));}
  CHECK(!find(panel,"rack.advanced")->isVisible()&&!find(panel,"rack.header")->isVisible()&&!find(panel,"rack.operation")->isVisible());
  CHECK(snapshot(panel).getPixelAt(2,2).getAlpha()==0);
  const auto img=snapshot(plate);
  for(const auto d:row.dividers){bool engraved=true;for(int y=100+int(dy);y<=250+int(dy);++y)engraved=engraved&&std::abs(img.getPixelAt(int(d),y).getRed()-201)<=3&&std::abs(img.getPixelAt(int(d)+1,y).getRed()-101)<=3;CHECK(engraved);}
  bool rightDivider=false;for(int x=1020;x<1044;++x)rightDivider=rightDivider||(std::abs(img.getPixelAt(x,160).getRed()-201)<=3&&std::abs(img.getPixelAt(x+1,160).getRed()-101)<=3);CHECK(!rightDivider);
  for(std::size_t k=0;k<counts.size();++k){
   const int from=k==0?0:int(row.dividers[k-1])+2;int first=-1;
   for(int x=from;x<from+80&&first<0;++x)for(int y=74+int(dy);y<=89+int(dy);++y)if(luma(img.getPixelAt(x,y))>150.0){first=x;break;}
   CHECK(first>=0&&std::abs(float(first)-(k==0?24.5f:row.dividers[k-1]+24.5f))<=1.0f);
  }
 }
 observation("subcase","rack_panel_unit_limit");
 // The limit: 8 controls, 2 in each of 4 sections, inside the panel and not overlapping, the first emphasised.
 {
  auto design=examples[1];design.panel.sections={"Motion","Tail","Tone","Space"};
  const auto add=[&](const char* label,int unit,int section,const char* key){const auto& d=parameterRegistry()[*rackParameterIndex(design.units[std::size_t(unit)].module,0,key)];design.panel.controls.push_back({label,std::uint8_t(unit),std::uint8_t(section),key,FocusWidget::rotary,FocusResponse::linear,d.minimum,d.maximum});};
  add("Cutoff",0,2,"wavetable.filter.cutoff");add("Level",0,2,"wavetable.osc.1.level");add("Repeats",1,3,"delay.feedback");add("Room",2,3,"reverb.mix");
  auto s=showDesign(design,Place::unit,1045,274);std::vector<juce::Rectangle<int>> boxes;
  for(int k=0;k<8;++k){auto* w=find(*s->panel,"rack.focus."+juce::String(k));CHECK(w!=nullptr);if(w)boxes.push_back(s->panel->getLocalArea(w,w->getLocalBounds()));}
  bool fits=boxes.size()==8;for(std::size_t a=0;a<boxes.size();++a){fits=fits&&s->panel->getLocalBounds().contains(boxes[a]);for(std::size_t b=a+1;b<boxes.size();++b)fits=fits&&!boxes[a].intersects(boxes[b]);}CHECK(fits);
 }
 observation("subcase","rack_panel_unit_locked");
 // An AI-locked control's name carries the lock's yellow dot left of its ink.
 {
  auto s=showDesign(examples[0],Place::unit,1045,274);auto locked=sound(s->p);locked.controlLocks[controlIndex(s->p,examples[0],0)]=true;CHECK(s->p.applySoundEdit(locked,SoundEditOrigin::manual,revision(s->p)));pump();
  Plate plate;plate.setSize(1045,274);plate.addAndMakeVisible(*s->panel);auto* name=widget<juce::Label>(*s->panel,"rack.label.0");CHECK(name);
  if(name){
   CHECK(studio::ProgrammerLook::locked(*name));
   const auto img=snapshot(plate);const auto box=plate.getLocalArea(name,name->getLocalBounds());int ink=-1;bool yellow=false;
   for(int x=box.getX();x<box.getRight()&&ink<0;++x)for(int y=box.getY();y<box.getBottom();++y){const auto c=img.getPixelAt(x,y);if(luma(c)>150.0&&c.getBlue()>150){ink=x;break;}}
   for(int x=std::max(box.getX(),ink-20);x<ink;++x)for(int y=box.getY();y<box.getBottom();++y){const auto c=img.getPixelAt(x,y);yellow=yellow||(c.getRed()>180&&c.getBlue()<90);}
   CHECK(ink>=0&&yellow);
  }
 }
 observation("subcase","rack_panel_unit_long_text");
 // Long text is squeezed and then elided inside its label, its whole text kept in the label and the tooltip.
 {
  auto design=examples[0];design.panel.controls[1].label="Resonance amount";auto s=showDesign(design,Place::unit,1045,274);
  auto edit=sound(s->p);const auto bite=controlIndex(s->p,design,1);edit.controlLocks[bite]=true;edit.macros[0]={parameterRegistry()[bite].stateKey,.25f};CHECK(s->p.applySoundEdit(edit,SoundEditOrigin::manual,revision(s->p)));pump();
  auto* name=widget<juce::Label>(*s->panel,"rack.label.1");auto* value=widget<juce::Label>(*s->panel,"rack.value.1");
  CHECK(name&&name->getText()=="Resonance amount");
  CHECK(value&&value->getText().contains(" | AI locked")&&value->getText().contains(" | Macro 1")&&value->getTooltip().contains(value->getText()));
  for(auto* id:{"rack.label.1","rack.value.1"}){auto* l=widget<juce::Label>(*s->panel,id);CHECK(l&&insideLabel(*l));}
 }
}
// The drawer (the rack stage spec: The drawer): the selected rack's header lines, today's form moved up under them and
// ADVANCED at the keys' right end, in 500 px, or 668 px once the difference box shows; Tab runs the form, ADVANCED, then
// the difference; an APPLY reports to the stack, which selects what it placed.
void rack_panel_drawer(){
 std::cout<<"GROUP rack_panel_drawer"<<std::endl;using Place=RackPanelComponent::Place;const auto& skin=*studio::studioSkin();const auto examples=airack::test::frozenExampleRackDesigns();
 auto s=showDesign(examples[0],Place::drawer,1486,668);auto& panel=*s->panel;
 // A window's size goes through the display's scale, which at 125% rounds 1486 to 1487; a child keeps its size.
 juce::Component holder;holder.setSize(1486,668);holder.addAndMakeVisible(panel);holder.setVisible(true);holder.addToDesktop(0);pump();
 auto showing=[&](const char* id){auto* c=find(panel,id);return c!=nullptr&&c->isShowing();};
 const auto box=[&](const char* id){auto* c=find(panel,id);return c?c->getBounds():juce::Rectangle<int>();};
 CHECK(showing("rack.operation")&&showing("rack.apply")&&showing("rack.header")&&showing("rack.advanced")&&!showing("rack.diff")&&find(panel,"rack.focus.0")==nullptr&&find(panel,"rack.section.0")==nullptr);
 const auto ember=[&]()->RackInstance{const auto d=sound(s->p);for(const auto& i:d.personalities->instances)if(i.id==s->id)return i;return {};};
 CHECK(widget<juce::Label>(panel,"rack.header")->getText()==rackHeaderText(sound(s->p),ember()));
 if(auto* control=widget<juce::ComboBox>(panel,"rack.control")){juce::StringArray items;for(int k=0;k<control->getNumItems();++k)items.add(control->getItemText(k));CHECK(items==juce::StringArray({"No control resolution","Air","Bite","Bloom","Attack","Release","Grain"}));}
 CHECK(box("rack.header")==juce::Rectangle<int>(93,12,1298,54)&&box("rack.advanced")==juce::Rectangle<int>(1309,350,84,96)&&box("rack.apply").getY()==350&&box("rack.operation").getY()==150&&box("rack.status").getY()==454&&box("rack.diff").getBottom()==648);
 // The difference box shows once APPLY or CANCEL DRAFT writes to it (F14.5); while it is empty and hidden the drawer ends
 // 20 px under the status line. A standalone panel shows it empty.
 int heights=0;panel.onHeight=[&]{++heights;};
 CHECK(panel.drawerHeight()==box("rack.status").getBottom()+20);
 click(panel,"rack.cancel");
 CHECK(showing("rack.diff")&&widget<juce::TextEditor>(panel,"rack.diff")->getText()=="Draft cancelled; live sound and library preserved"&&panel.drawerHeight()==box("rack.diff").getBottom()+20&&heights==1);
 {RackPanelComponent all(s->p,skin);all.setSize(1080,680);juce::Component frame;frame.setSize(1080,680);frame.addAndMakeVisible(all);frame.addToDesktop(0);frame.setVisible(true);pump();
  auto* diff=find(all,"rack.diff");CHECK(diff!=nullptr&&diff->isShowing()&&widget<juce::TextEditor>(all,"rack.diff")->getText().isEmpty());}
 // The shortened toggles keep their names, with today's texts as tooltips; the form's legends; the Tab stops.
 auto* reuse=widget<juce::ToggleButton>(panel,"rack.reuse");CHECK(reuse&&reuse->getButtonText()=="Reuse banks"&&reuse->getTooltip()=="Reuse exact banks (retains host automation)"&&reuse->getName()=="Explicitly authorize exact physical bank reuse");
 auto* move=widget<juce::ToggleButton>(panel,"rack.moveDependencies");CHECK(move&&move->getButtonText()=="Move FX to Master"&&move->getTooltip()=="On synth removal, move independent FX personalities to Shared Master"&&move->getName()=="Explicit compound removal: preserve independent FX by moving them to Shared Master");
 const std::pair<const char*,const char*> legends[]{{"rack.operation","OPERATION"},{"rack.examples","EXAMPLE"},{"rack.destination","DESTINATION"},{"rack.position","POSITION"},{"rack.allocation","BANKS"},{"rack.control","CONTROL"},{"rack.resolution","RESOLUTION"},{"rack.remap","REMAP KEY"}};
 for(const auto& [id,legend]:legends){auto* c=find(panel,id);CHECK(c&&studio::ProgrammerLook::legend(*c)==legend);}
 // The actions' legends, each within its button, stand apart: CANCEL DRAFT is wider than the PROGRAMMER's.
 const juce::StringArray actions{"rack.apply","rack.cancel","rack.reuse","rack.moveDependencies"};
 for(int k=0;k+1<actions.size();++k){auto* a=find(panel,actions[k]);auto* b=find(panel,actions[k+1]);CHECK(a&&b&&a->getY()==b->getY()&&b->getX()-a->getRight()>=12);}
 const std::pair<const char*,int> orders[]{{"rack.examples",20},{"rack.operation",21},{"rack.destination",22},{"rack.position",23},{"rack.allocation",24},{"rack.reuse",26},{"rack.moveDependencies",27},{"rack.control",27},{"rack.resolution",28},{"rack.remap",29},{"rack.apply",33},{"rack.cancel",34},{"rack.advanced",36},{"rack.diff",37}};
 for(const auto& [id,order]:orders){auto* c=find(panel,id);CHECK(c&&c->getExplicitFocusOrder()==order);}
 {juce::KeyboardFocusTraverser traverser;juce::StringArray tabs;juce::Component* c=find(panel,"rack.cancel");
  for(int k=0;k<3&&c!=nullptr;++k){tabs.add(c->getComponentID());c=traverser.getNextComponent(c);}
  CHECK(tabs==juce::StringArray({"rack.cancel","rack.advanced","rack.diff"}));}
 // Long text is squeezed and then elided inside its label.
 panel.showStatus(juce::String::repeatedString("Applied one reviewed rack transaction; ",6));pump();
 for(auto* id:{"rack.header","rack.libraryLabel","rack.status"}){auto* l=widget<juce::Label>(panel,id);CHECK(l&&insideLabel(*l));}
 // A duplicate's APPLY: the stack hears the sentence and the copy; the drawer still shows Ember Reed until told.
 juce::String applied;std::optional<juce::Uuid> placed;panel.onApplied=[&](const juce::String& status,std::optional<juce::Uuid> copy){applied=status;placed=copy;};
 choice(panel,"rack.operation",5);click(panel,"rack.apply");pump();
 CHECK(applied=="Applied one reviewed rack transaction"&&placed&&*placed!=s->id&&sound(s->p).personalities->instances.size()==2);
 CHECK(widget<juce::Label>(panel,"rack.header")->getText()==rackHeaderText(sound(s->p),ember())&&widget<juce::Label>(panel,"rack.status")->getText()==applied);
 // At the face's own size a bare tile is the plate's.
 {
  panel.setSize(1320,516);pump();const auto img=snapshot(panel);const auto& plate=skin.plates.at("advanced");std::array<double,3> sum{};
  for(int y=0;y<32;++y)for(int x=0;x<32;++x){const auto a=img.getPixelAt(16+x,120+y),b=plate.getPixelAt(126+x,340+y);sum[0]+=std::abs(a.getRed()-b.getRed());sum[1]+=std::abs(a.getGreen()-b.getGreen());sum[2]+=std::abs(a.getBlue()-b.getBlue());}
  CHECK(std::max({sum[0],sum[1],sum[2]})/1024.0<=1.0);
 }
}
// Each readout leads with the PROGRAMMER display's text for its parameter: Ember Reed's AIR reads 1.80 kHz.
void rack_panel_values(){
 std::cout<<"GROUP rack_panel_values"<<std::endl;
 for(const auto& design:airack::test::frozenExampleRackDesigns()){
  InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.destination.role=design.role;op.destination.position=design.role==RackRole::synth?1:0;
  CHECK(p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);
  RackPanelComponent panel(p,*studio::studioSkin());panel.setPlace(RackPanelComponent::Place::unit);panel.showInstance(op.newInstance);panel.refresh(sound(p));panel.setSize(1045,274);pump();
  const auto d=sound(p);if(!d.personalities||d.personalities->instances.empty()){CHECK(false);continue;}const auto& i=d.personalities->instances.back();
  for(std::size_t k=0;k<design.panel.controls.size();++k){
   const auto& c=design.panel.controls[k];const auto u=std::find_if(design.units.begin(),design.units.end(),[&](const auto& v){return v.id==c.unit;});const auto b=std::find_if(i.bindings.begin(),i.bindings.end(),[&](auto v){return v.unit==c.unit;});
   const auto index=u!=design.units.end()&&b!=i.bindings.end()?rackParameterIndex(u->module,b->slot,c.key):std::nullopt;
   auto* value=widget<juce::Label>(panel,"rack.value."+juce::String(int(k)));CHECK(index&&value);if(!index||!value)continue;
   auto& parameter=p.control(*index);
   CHECK(value->getText().upToFirstOccurrenceOf(" | ",false,false)==studio::formatParameter(*index,parameter.convertFrom0to1(parameter.getValue())));
  }
  if(design.name=="Ember Reed"){auto* air=widget<juce::Label>(panel,"rack.value.0");CHECK(air&&air->getText()=="1.80 kHz");}
 }
}
// An example rack placed with an explicit allocation (local unit, zero-based physical bank), as the window tests place it.
juce::Uuid placeExample(InstrumentProcessor& p,std::size_t example,RackDestination destination,std::vector<UnitBinding> allocation,bool reuse=false){RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[example];op.newInstance=juce::Uuid();op.destination=destination;op.destination.role=op.design->role;op.allocation=std::move(allocation);op.allowSlotReuse=reuse;CHECK(p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);return op.newInstance;}
// The stack's order, UP and DOWN, and a rack's place, light and lock, from the sound alone (the rack stage spec: The stack;
// UP and DOWN).
void rack_order(){
 std::cout<<"GROUP rack_order"<<std::endl;using Ids=std::vector<juce::Uuid>;
 const auto ids=[](const SoundDocument& d){Ids v;for(const auto& r:rackOrder(d))v.push_back(r.id);return v;};
 const auto step=[](InstrumentProcessor& p,juce::Uuid id,bool up){const auto op=rackStep(sound(p),id,up);return op&&p.applyRackOperation(*op,revision(p))==RackCandidateStatus::ready;};
 const auto rack=[](const SoundDocument& d,juce::Uuid id)->const RackInstance&{for(const auto& i:d.personalities->instances)if(i.id==id)return i;CHECK(false);return d.personalities->instances.front();};
 // The drawn racks: their order and places, the ends, a synth move that changes only the synth order, one undo step,
 // and an AI lock that does not stop it.
 {auto pStorage=std::make_unique<InstrumentProcessor>(offline(),preparedTestRackRuntime());auto& p=*pStorage;CHECK(blankInstance(p));const auto ember=placeExample(p,0,{},{{0,0},{1,0}},true),glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,masterChain,0},{{0,3},{1,4}});
  const auto d=sound(p);const auto order=rackOrder(d);
  CHECK(ids(d)==(Ids{ember,glass,press})&&order.size()==3&&order[0].chain==0&&order[1].chain==1&&order[1].role==RackRole::synth&&order[2].chain==masterChain&&order[2].role==RackRole::fx);
  CHECK(rackPlaceText(d,rack(d,ember))=="SYNTH 1"&&rackPlaceText(d,rack(d,glass))=="SYNTH 2"&&rackPlaceText(d,rack(d,press))=="SHARED MASTER");
  auto state=std::make_shared<RackPersonalityState>(*d.personalities);state->instances[0].edited=true;auto edited=d;edited.personalities=state;CHECK(rackPlaceText(edited,state->instances[0])=="SYNTH 1 | EDITED");
  CHECK(!rackStep(d,ember,true)&&rackStep(d,ember,false)&&rackStep(d,glass,true)&&!rackStep(d,glass,false)&&!rackStep(d,press,true)&&!rackStep(d,press,false));
  CHECK(step(p,glass,true)&&ids(sound(p))==(Ids{glass,ember,press})&&sound(p).layout.synthOrder[0]==1&&sound(p).values==d.values);
  CHECK(p.undoSoundEdit()&&ids(sound(p))==(Ids{ember,glass,press}));
  auto locked=sound(p);locked.synthLocks[1]=true;CHECK(p.applySoundEdit(locked,SoundEditOrigin::manual,revision(p))&&step(p,glass,true)&&ids(sound(p)).front()==glass);
  // The header's rule, shared: on until a bank is disabled or bypassed; locked by a bank's AI lock.
  CHECK(rackOn(d,rack(d,press))&&rackOn(d,rack(d,ember))&&!rackLocked(d,rack(d,press))&&!rackLocked(d,rack(d,glass)));
  auto bypassed=d;bypassed.values[572+23*3]=1.0f;CHECK(!rackOn(bypassed,rack(d,press))&&rackOn(bypassed,rack(d,ember)));
  auto off=d;off.values[12+4*1]=0.0f;CHECK(!rackOn(off,rack(d,glass))&&rackOn(off,rack(d,ember)));
  auto locks=d;locks.effectLocks[4]=true;CHECK(rackLocked(locks,rack(d,press))&&!rackLocked(locks,rack(d,ember)));
  const auto header=rackHeaderText(d,rack(d,ember));CHECK(header.startsWith("Ember Reed | revision ")&&juce::StringArray::fromLines(header).size()==3&&header.contains("Synth 1; synth order 1")&&header.contains("AI rack unlocked"));}
 // An FX rack on Ember Reed's insert chain, after its owned drive: under it as SYNTH 1 INSERT, and it goes with it.
 {auto pStorage=std::make_unique<InstrumentProcessor>(offline(),preparedTestRackRuntime());auto& p=*pStorage;CHECK(blankInstance(p));const auto ember=placeExample(p,0,{},{{0,0},{1,0}},true),glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,0,1},{{0,3},{1,4}});
  const auto d=sound(p);CHECK(ids(d)==(Ids{ember,press,glass})&&rackPlaceText(d,rack(d,press))=="SYNTH 1 INSERT"&&rackOrder(d)[1].chain==0&&!rackStep(d,press,true)&&!rackStep(d,press,false));
  CHECK(step(p,ember,false)&&ids(sound(p))==(Ids{glass,ember,press}));}
 // Two FX racks on the shared master, under synth 1's bare bank: DOWN passes the next and changes the chain's order; UP
 // returns it.
 {auto pStorage=std::make_unique<InstrumentProcessor>(offline(),preparedTestRackRuntime());auto& p=*pStorage;CHECK(blankInstance(p));const auto a=placeExample(p,2,{RackRole::fx,masterChain,0},{{0,3},{1,4}}),b=placeExample(p,2,{RackRole::fx,masterChain,2},{{0,5},{1,6}});
  const auto synth1=bareBankId({RackRole::synth,0});
  const auto d=sound(p);CHECK(ids(d)==(Ids{synth1,a,b})&&!rackStep(d,a,true)&&rackStep(d,a,false)&&rackStep(d,b,true)&&!rackStep(d,b,false));
  CHECK(step(p,a,false)&&ids(sound(p))==(Ids{synth1,b,a})&&sound(p).layout.effectOrder[masterChain][0]==5&&sound(p).layout.effectOrder[masterChain][2]==3);
  CHECK(step(p,a,true)&&ids(sound(p))==(Ids{synth1,a,b})&&sound(p).layout.effectOrder[masterChain][0]==3);}
 // An FX rack on the insert chain of a bank with no synth rack follows that bank's bare entry; no synth rack is above Glass
 // Current, as UP and DOWN pass racks only.
 {auto pStorage=std::make_unique<InstrumentProcessor>(offline(),preparedTestRackRuntime());auto& p=*pStorage;CHECK(blankInstance(p));const auto glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,0,0},{{0,3},{1,4}});
  const auto d=sound(p);CHECK(ids(d)==(Ids{bareBankId({RackRole::synth,0}),press,glass})&&rackPlaceText(d,rack(d,press))=="SYNTH 1 INSERT"&&!rackStep(d,glass,true)&&!rackStep(d,glass,false));}
 // Review Focus 5: Thread Press on Glass Current's insert chain, then bank 2 taken out of the synth order by hand: both
 // still show, at the synth part's end, in instance order.
 {auto pStorage=std::make_unique<InstrumentProcessor>(offline(),preparedTestRackRuntime());auto& p=*pStorage;CHECK(blankInstance(p));const auto ember=placeExample(p,0,{},{{0,0},{1,0}},true),glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,1,2},{{0,3},{1,4}});
  auto d=sound(p);CHECK(ids(d)==(Ids{ember,glass,press})&&rackPlaceText(d,rack(d,press))=="SYNTH 2 INSERT");
  d.layout.synthCount=1;CHECK(ids(d)==(Ids{ember,glass,press})&&!rackStep(d,glass,true)&&!rackStep(d,glass,false));}
 CHECK(rackStatusText(RackCandidateStatus::ready)=="Ready"&&rackStatusText(RackCandidateStatus::stale)=="Stale selection");
 CHECK(rackMoveSentence("Glass Current",true,RackCandidateStatus::ready)=="Moved Glass Current up"&&rackMoveSentence("Glass Current",false,RackCandidateStatus::ready)=="Moved Glass Current down");
 CHECK(rackMoveSentence("Thread Press",false,RackCandidateStatus::stale)=="Thread Press not moved: Stale selection; sound preserved");
}
// Where a placement draws and where a lifted rack can land, from the sound alone (the moves spec: Where a placement goes;
// Where a rack can land); each line written as its spot, "m" under the divider, and its destination's words or home.
void rack_spots(){
 std::cout<<"GROUP rack_spots"<<std::endl;using Ids=std::vector<juce::Uuid>;
 const auto is=[](std::optional<StackSpot> s,std::size_t before,bool master){return s&&s->before==before&&s->master==master;};
 const auto lines=[](const std::vector<StackDrop>& v){juce::StringArray out;for(const auto& l:v)out.add(juce::String((int)l.spot.before)+(l.spot.master?"m":"")+(l.destination?" "+rackDestinationText(*l.destination):juce::String(" home")));return out;};
 // Each destination a line gives, applied as today's move, puts the rack at the line's place in the candidate's order.
 const auto roundTrip=[](const SoundDocument& d,juce::Uuid id){
  for(const auto& l:rackDrops(d,id))if(l.destination){
   RackOperation op;op.kind=RackOperationKind::move;op.selected=id;op.destination=*l.destination;SoundDocument out;RackDifference diff;
   CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);
   const auto order=rackOrder(out);const auto at=std::find_if(order.begin(),order.end(),[&](const auto& r){return r.id==id;});
   CHECK(at!=order.end()&&std::size_t(at-order.begin())==l.spot.before&&(at->chain==masterChain)==l.spot.master);
  }};
 const auto rack=[](const SoundDocument& d,juce::Uuid id)->const RackInstance&{for(const auto& i:d.personalities->instances)if(i.id==id)return i;CHECK(false);return d.personalities->instances.front();};
 // The drawn racks: Ember Reed (bank 1, its drive owned), Glass Current (bank 2, its delay and reverb owned), Thread Press
 // on the shared master.
 {InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));const auto ember=placeExample(p,0,{},{{0,0},{1,0}},true),glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,masterChain,0},{{0,3},{1,4}});
  const auto d=sound(p);CHECK(d.layout.synthCount==2);
  CHECK(is(rackSpot(d,{RackRole::synth,masterChain,0}),0,false)&&is(rackSpot(d,{RackRole::synth,masterChain,1}),1,false)&&is(rackSpot(d,{RackRole::synth,masterChain,2}),2,false)&&is(rackSpot(d,{RackRole::synth,masterChain,8}),2,false));
  CHECK(is(rackSpot(d,{RackRole::fx,0,0}),1,false)&&is(rackSpot(d,{RackRole::fx,0,1}),1,false)&&is(rackSpot(d,{RackRole::fx,1,0}),2,false));
  CHECK(is(rackSpot(d,{RackRole::fx,masterChain,0}),2,true)&&is(rackSpot(d,{RackRole::fx,masterChain,2}),3,true));
  CHECK(!rackSpot(d,{RackRole::fx,2,0})&&!rackSpot(d,{RackRole::fx,3,0}));
  // A bank in the synth order with no rack shows its bare entry at its place, its insert chain after it: bank 3 between
  // the two, then last.
  auto gap=d;gap.layout.synthCount=3;gap.layout.synthOrder={0,2,1};
  CHECK(rackOrder(gap).size()==4&&rackOrder(gap)[1].id==bareBankId({RackRole::synth,2}));
  CHECK(is(rackSpot(gap,{RackRole::fx,2,0}),2,false)&&is(rackSpot(gap,{RackRole::synth,masterChain,1}),1,false)&&is(rackSpot(gap,{RackRole::synth,masterChain,2}),2,false));
  gap.layout.synthOrder={0,1,2};CHECK(is(rackSpot(gap,{RackRole::fx,2,0}),3,false));
  // A rack the walk misses lifts with nowhere to go but home, which no rule gives.
  auto missed=d;missed.layout.synthCount=1;CHECK(lines(rackDrops(missed,glass))==juce::StringArray({"1 home"}));
  CHECK(rackGroup(d,ember)==(Ids{ember})&&rackGroup(d,glass)==(Ids{glass})&&rackGroup(d,press)==(Ids{press})&&rackGroup(d,juce::Uuid()).empty());
  CHECK(lines(rackDrops(d,ember))==juce::StringArray({"0 home","1 SYNTH ORDER POSITION 2"}));
  CHECK(lines(rackDrops(d,glass))==juce::StringArray({"0 SYNTH ORDER POSITION 1","1 home"}));
  CHECK(lines(rackDrops(d,press))==juce::StringArray({"1 SYNTH 1 INSERT POSITION 2","2 SYNTH 2 INSERT POSITION 3","2m home"}));
  roundTrip(d,ember);roundTrip(d,glass);roundTrip(d,press);
  // The values a preview shows: the recipe's, else the parameter's default; a placed rack's: the sound's.
  const auto designs=airack::test::frozenExampleRackDesigns();const auto& design=designs[0];const auto shown=rackPreviewInstance(design);const auto values=rackPreviewValues(design);
  CHECK(shown.design==design&&shown.bindings==(std::vector<UnitBinding>{{0,0},{1,0}})&&values.size()==design.panel.controls.size());
  const auto glassShown=rackPreviewInstance(airack::test::frozenExampleRackDesigns()[1]);CHECK(glassShown.bindings==(std::vector<UnitBinding>{{0,0},{1,0},{2,1}}));
  const auto defaults=defaultParameterValues();bool recipe=false;
  for(std::size_t n=0;n<values.size()&&n<design.panel.controls.size();++n){
   const auto& c=design.panel.controls[n];const auto& u=*std::find_if(design.units.begin(),design.units.end(),[&](const auto& v){return v.id==c.unit;});
   const auto own=std::find_if(u.defaults.begin(),u.defaults.end(),[&](const auto& v){return v.key==c.key;});
   const auto bound=std::find_if(shown.bindings.begin(),shown.bindings.end(),[&](auto b){return b.unit==c.unit;});CHECK(bound!=shown.bindings.end());if(bound==shown.bindings.end())continue;
   const auto at=rackParameterIndex(u.module,bound->slot,c.key);CHECK(at.has_value());if(!at)continue;
   CHECK(values[n]==(own!=u.defaults.end()?own->normalized:defaults[*at]));if(own!=u.defaults.end())recipe=true;
  }
  CHECK(recipe);
  // Every parameter, for a faceplate's preview: each unit's recipe in the preview's banks, the rest at their defaults.
  {const auto& thread=designs[2];const auto banks=rackPreviewInstance(thread);auto expected=defaults;
   for(const auto& u:thread.units)for(const auto& v:u.defaults){const auto at=rackParameterIndex(u.module,rackUnitBinding(banks,u.id)->slot,v.key);CHECK(at.has_value());if(at)expected[*at]=v.normalized;}
   CHECK(rackPreviewParameters(thread)==expected&&expected!=defaults);}
  auto changed=d;const auto& placed=rack(d,ember);const auto& first=placed.design.panel.controls[0];
  const auto firstUnit=*std::find_if(placed.design.units.begin(),placed.design.units.end(),[&](const auto& v){return v.id==first.unit;});
  const auto firstAt=*rackParameterIndex(firstUnit.module,std::find_if(placed.bindings.begin(),placed.bindings.end(),[&](auto b){return b.unit==first.unit;})->slot,first.key);
  changed.values[firstAt]=.125f;const auto live=rackInstanceValues(changed,placed);CHECK(live.size()==placed.design.panel.controls.size()&&!live.empty()&&live[0]==.125f);}
 // Thread Press on Ember Reed's insert chain lifts with it; on its own it can go to Glass Current's chain or the master.
 {InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));const auto ember=placeExample(p,0,{},{{0,0},{1,0}},true),glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,0,1},{{0,3},{1,4}});
  const auto d=sound(p);CHECK(rackGroup(d,ember)==(Ids{ember,press})&&rackGroup(d,glass)==(Ids{glass})&&rackGroup(d,press)==(Ids{press}));
  CHECK(lines(rackDrops(d,ember))==juce::StringArray({"0 home","1 SYNTH ORDER POSITION 2"}));
  CHECK(lines(rackDrops(d,glass))==juce::StringArray({"0 SYNTH ORDER POSITION 1","2 home"}));
  CHECK(lines(rackDrops(d,press))==juce::StringArray({"1 home","2 SYNTH 2 INSERT POSITION 3","2m SHARED MASTER POSITION 1"}));
  CHECK(is(rackSpot(d,{RackRole::fx,0,1}),1,false)&&is(rackSpot(d,{RackRole::fx,0,2}),2,false));
  roundTrip(d,ember);roundTrip(d,glass);roundTrip(d,press);}
 // Two FX racks on the shared master: before the other, or after it, counted without the lifted rack's own units.
 {InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));placeExample(p,0,{},{{0,0},{1,0}},true);placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}});
  const auto a=placeExample(p,2,{RackRole::fx,masterChain,0},{{0,3},{1,4}}),b=placeExample(p,2,{RackRole::fx,masterChain,2},{{0,5},{1,6}});
  const auto d=sound(p);
  CHECK(lines(rackDrops(d,a))==juce::StringArray({"1 SYNTH 1 INSERT POSITION 2","2 SYNTH 2 INSERT POSITION 3","2m home","3m SHARED MASTER POSITION 3"}));
  CHECK(lines(rackDrops(d,b))==juce::StringArray({"1 SYNTH 1 INSERT POSITION 2","2 SYNTH 2 INSERT POSITION 3","2m SHARED MASTER POSITION 1","3m home"}));
  CHECK(is(rackSpot(d,{RackRole::fx,masterChain,1}),3,true)&&is(rackSpot(d,{RackRole::fx,masterChain,2}),3,true)&&is(rackSpot(d,{RackRole::fx,masterChain,4}),4,true));
  roundTrip(d,a);roundTrip(d,b);}
 // An FX rack on a bank with no synth rack, under that bank's bare entry: its home is the line after the entry.
 {InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));const auto glass=placeExample(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}}),press=placeExample(p,2,{RackRole::fx,0,0},{{0,3},{1,4}});
  const auto d=sound(p);CHECK(rackOrder(d).size()==3&&rackOrder(d)[0].id==bareBankId({RackRole::synth,0})&&rackOrder(d)[1].id==press);
  CHECK(lines(rackDrops(d,press))==juce::StringArray({"1 home","2 SYNTH 2 INSERT POSITION 3","2m SHARED MASTER POSITION 1"}));
  CHECK(lines(rackDrops(d,glass))==juce::StringArray({"0 SYNTH ORDER POSITION 1","2 home"}));
  CHECK(is(rackSpot(d,{RackRole::fx,0,0}),1,false)&&is(rackSpot(d,{RackRole::fx,0,2}),2,false)&&is(rackSpot(d,{RackRole::synth,masterChain,0}),0,false));
  roundTrip(d,press);roundTrip(d,glass);}
 CHECK(rackDestinationText({RackRole::fx,masterChain,1})=="SHARED MASTER POSITION 2"&&rackDestinationText({RackRole::fx,0,0})=="SYNTH 1 INSERT POSITION 1"&&rackDestinationText({RackRole::synth,masterChain,2})=="SYNTH ORDER POSITION 3");
 CHECK(rackDropSentence("Ember Reed",{RackRole::synth,masterChain,1},RackCandidateStatus::ready)=="Moved Ember Reed to synth order position 2");
 CHECK(rackDropSentence("Thread Press",{RackRole::fx,masterChain,0},RackCandidateStatus::ready)=="Moved Thread Press to Shared Master, position 1");
 CHECK(rackDropSentence("Thread Press",{RackRole::fx,0,1},RackCandidateStatus::ready)=="Moved Thread Press to Synth 1 insert, position 2");
 CHECK(rackDropSentence("Thread Press",{RackRole::fx,0,1},RackCandidateStatus::stale)=="Thread Press not moved: Stale selection; sound preserved");
 const StackPlacement one{{RackRole::fx,masterChain,1},"NEW RACK SHARED MASTER POSITION 2",{},{}};CHECK(one==one&&!(one==StackPlacement{}));
}
// Bare banks in the stack order (the user-reach spec, decision 3): a present bank no rack holds is a line where a rack on
// it would be, with an ID, name and place from the bank alone.
void rack_bare_banks(){
 std::cout<<"GROUP rack_bare_banks"<<std::endl;using Ids=std::vector<juce::Uuid>;
 const auto ids=[](const SoundDocument& d){Ids v;for(const auto& r:rackOrder(d))v.push_back(r.id);return v;};
 const auto bare=[](RackRole role,int bank){return bareBankId({role,std::uint8_t(bank)});};
 const auto is=[](std::optional<StackSpot> s,std::size_t before,bool master){return s&&s->before==before&&s->master==master;};
 const auto lines=[](const std::vector<StackDrop>& v){juce::StringArray out;for(const auto& l:v)out.add(juce::String((int)l.spot.before)+(l.spot.master?"m":"")+(l.destination?" "+rackDestinationText(*l.destination):juce::String(" home")));return out;};
 for(int b=0;b<4;++b)CHECK(bareBankOf(bare(RackRole::synth,b))==(BareBank{RackRole::synth,std::uint8_t(b)}));
 for(int b=0;b<8;++b)CHECK(bareBankOf(bare(RackRole::fx,b))==(BareBank{RackRole::fx,std::uint8_t(b)}));
 CHECK(!bareBankOf(juce::Uuid())&&!bareBankOf(juce::Uuid::null())&&bare(RackRole::synth,0)!=bare(RackRole::fx,0));
 {const auto order=rackOrder(SoundDocument{});
  CHECK(order.size()==1&&order[0].id==bare(RackRole::synth,0)&&order[0].bare&&order[0].role==RackRole::synth&&order[0].chain==0&&order[0].bank==0);}
 // Names and places, from the bank's engine or kind and its chain.
 {SoundDocument d;CHECK(bareBankName(d,{RackRole::synth,0})=="SYNTH 1 ANALOG"&&bareBankPlace(d,{RackRole::synth,0})=="SYNTH 1");
  d.layout.synthUse[1]=SlotUse::present;d.layout.synthOrder[1]=1;d.layout.synthCount=2;d.values[13+4]=1.0f;
  CHECK(bareBankName(d,{RackRole::synth,1})=="SYNTH 2 WAVETABLE");
  RackLayout out;CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,2,0,0,EffectKind::delay},out)==RackEditResult::applied);d.layout=out;
  CHECK(bareBankName(d,{RackRole::fx,2})=="FX 3 DELAY"&&bareBankPlace(d,{RackRole::fx,2})=="SYNTH 1 INSERT");
  CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,5,masterChain,0,EffectKind::reverb},out)==RackEditResult::applied);d.layout=out;
  CHECK(bareBankPlace(d,{RackRole::fx,5})=="SHARED MASTER");
  const auto order=rackOrder(d);
  CHECK(ids(d)==(Ids{bare(RackRole::synth,0),bare(RackRole::fx,2),bare(RackRole::synth,1),bare(RackRole::fx,5)})&&order[1].chain==0&&order[1].bank==2&&order[3].chain==masterChain);}
 // Review Focus 4: Ember Reed with its owned drive on effect 0, Cinder Coil after it on synth 1's insert chain, then a
 // bare delay after that. Effect 0 has no line; the delay is a line like a rack, counted in spots and drops, and lifts
 // with Ember Reed.
 {InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));const auto ember=placeExample(p,0,{},{{0,0},{1,0}},true);
  RackOperation op;op.design=airack::test::frozenFactoryRackDesigns()[4];op.newInstance=juce::Uuid();op.destination={RackRole::fx,0,1};op.allocation={{0,1}};
  CHECK(op.design->name=="Cinder Coil"&&p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);const auto drive=op.newInstance;
  CHECK(p.applyRackEdit({RackEditKind::addEffect,2,0,2,EffectKind::delay},revision(p))==RackEditResult::applied);
  const auto d=sound(p);const auto delay=bare(RackRole::fx,2);const auto order=rackOrder(d);
  CHECK(ids(d)==(Ids{ember,drive,delay})&&order.size()==3&&order[2].bare&&order[2].role==RackRole::fx&&order[2].chain==0&&order[2].bank==2&&order[1].bank==1&&!order[1].bare);
  CHECK(is(rackSpot(d,{RackRole::fx,0,2}),2,false)&&is(rackSpot(d,{RackRole::fx,0,3}),3,false));
  const auto drops=lines(rackDrops(d,drive));std::cout<<"  Cinder Coil's lines: "<<drops.joinIntoString(", ")<<std::endl;
  CHECK(drops.contains("1 home")&&drops.contains("2 SYNTH 1 INSERT POSITION 3"));
  CHECK(rackGroup(d,ember)==(Ids{ember,drive,delay}));}
}
// Near-white pixels (luma above 0.9) in a rectangle of an image: the marks' and outlines' dashes.
int whiteIn(const juce::Image& img,juce::Rectangle<int> r){int n=0;r=r.getIntersection(img.getBounds());for(int y=r.getY();y<r.getBottom();++y)for(int x=r.getX();x<r.getRight();++x)if(luma(img.getPixelAt(x,y))>.9*255)++n;return n;}
// A design's row with no rack placed (the moves spec: Its look): Ember Reed's at its recipe's values, laid out as a placed
// rack's row, bound to no parameter, and taking no focus or clicks.
void rack_panel_preview(){
 std::cout<<"GROUP rack_panel_preview"<<std::endl;using Place=RackPanelComponent::Place;const auto designs=airack::test::frozenExampleRackDesigns();const auto& design=designs[0];
 auto placed=showDesign(design,Place::unit,1045,274);auto& p=placed->p;
 RackPanelComponent panel(p,*studio::studioSkin());panel.setPlace(Place::preview);panel.setSize(1045,274);pump();
 const auto shown=rackPreviewInstance(design);auto values=rackPreviewValues(design);values[0]=.75f;
 const auto valueText=[&](int k){auto* l=widget<juce::Label>(panel,"rack.value."+juce::String(k));return l?l->getText():juce::String();};
 const auto index0=*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff");const auto air=studio::formatParameter(index0,p.control(index0).convertFrom0to1(.75f));
 const auto before=revision(p);panel.showDesign(shown,values);pump();
 CHECK(panel.place()==Place::preview&&find(panel,"rack.section.0")!=nullptr&&find(panel,"rack.focus."+juce::String((int)design.panel.controls.size()-1))!=nullptr);
 // Each value shows as a placed rack's does, from the values it was shown.
 for(std::size_t k=0;k<design.panel.controls.size();++k){
  const auto& c=design.panel.controls[k];const auto& u=*std::find_if(design.units.begin(),design.units.end(),[&](const auto& v){return v.id==c.unit;});
  const auto* b=rackUnitBinding(shown,c.unit);const auto at=b?rackParameterIndex(u.module,b->slot,c.key):std::nullopt;
  CHECK(at&&valueText(int(k))==studio::formatParameter(*at,p.control(*at).convertFrom0to1(values[k])));
 }
 CHECK(valueText(0)==air&&revision(p)==before);
 // Bound to nothing: the live sound's value does not reach it, and its widgets write nothing.
 p.control(index0).setValueNotifyingHost(.1f);for(int k=0;k<4;++k)pump();CHECK(valueText(0)==air);
 const auto moved=revision(p);if(auto* knob=widget<juce::Slider>(panel,"rack.focus.0")){knob->setValue(knob->getMinimum(),juce::sendNotificationSync);pump();CHECK(revision(p)==moved);}
 // Its row lays out as a rack unit's: the same widgets, names, values and sections at the same places.
 const auto boxes=[](RackPanelComponent& c){std::vector<juce::Rectangle<int>> v;
  for(int k=0;auto* w=find(c,"rack.focus."+juce::String(k));++k){v.push_back(c.getLocalArea(w,w->getLocalBounds()));for(const juce::String id:{"rack.label.","rack.value."})if(auto* l=find(c,id+juce::String(k)))v.push_back(c.getLocalArea(l,l->getLocalBounds()));}
  for(int k=0;auto* s=find(c,"rack.section."+juce::String(k));++k)v.push_back(c.getLocalArea(s,s->getLocalBounds()));return v;};
 CHECK(boxes(panel).size()==design.panel.controls.size()*3+design.panel.sections.size()&&boxes(panel)==boxes(*placed->panel));
 // Inert: no widget takes the focus or the mouse; no face, header, ADVANCED or form, and none of the editor's IDs.
 bool inert=true;for(int k=0;auto* w=find(panel,"rack.focus."+juce::String(k));++k)inert=inert&&!w->getWantsKeyboardFocus()&&panel.getComponentAt(panel.getLocalArea(w,w->getLocalBounds()).getCentre())!=w;CHECK(inert);
 CHECK(snapshot(panel).getPixelAt(2,2).getAlpha()==0&&!find(panel,"rack.advanced")->isVisible()&&!find(panel,"rack.header")->isVisible()&&!find(panel,"rack.operation")->isVisible());
 CHECK(find(panel,"rack.panel")==nullptr&&find(panel,"rack.viewport")==nullptr&&find(panel,"rack.select")==nullptr);
 // Another design takes the row's place.
 panel.showDesign(rackPreviewInstance(designs[2]),rackPreviewValues(designs[2]));pump();
 CHECK(find(panel,"rack.focus.3")!=nullptr&&find(panel,"rack.focus.4")==nullptr&&find(panel,"rack.section.1")!=nullptr&&find(panel,"rack.section.2")==nullptr);
}
// The AI's marks on a placed rack's row (the moves spec: The AI's marks): a knob's pointer, a fader's cap and a choice box's
// outline at the candidate's values; none on a locked control or one whose live value is the candidate's; the row's
// outline for another panel of the same design; nothing for another design or a removed rack.
void rack_panel_marks(const juce::File& images){
 std::cout<<"GROUP rack_panel_marks"<<std::endl;using Place=RackPanelComponent::Place;using Mark=RackPanelComponent::Mark;const auto designs=airack::test::frozenExampleRackDesigns();
 auto design=designs[0];{const auto& d=parameterRegistry()[*rackParameterIndex(NativeModule::analog,0,"analog.filter.mode")];design.panel.controls.push_back({"Mode",0,0,"analog.filter.mode",FocusWidget::choice,FocusResponse::linear,d.minimum,d.maximum});}
 auto s=showDesign(design,Place::unit,1045,274);auto& p=s->p;auto& row=*s->panel;Plate plate;plate.setSize(1045,274);plate.addAndMakeVisible(row);
 const std::size_t air=0,bite=1,bloom=2,attack=3,mode=design.panel.controls.size()-1;
 const auto at=[&](std::size_t k){return controlIndex(p,design,k);};
 const auto physical=[&](std::size_t k,float v){return p.control(at(k)).convertFrom0to1(v);};
 const auto description=[&](std::size_t k){auto* w=find(row,"rack.focus."+juce::String(int(k)));return w?w->getDescription():juce::String("missing");};
 const auto proposes=[&](std::size_t k,float v){return "AI proposes "+studio::formatParameter(at(k),v);};
 // The live sound locks BITE; the candidate moves AIR to its top, BITE, BLOOM to its bottom and MODE.
 auto locked=sound(p);locked.controlLocks[at(bite)]=true;CHECK(p.applySoundEdit(locked,SoundEditOrigin::manual,revision(p)));row.refresh(sound(p));pump();
 auto candidate=std::make_shared<SoundDocument>(sound(p));auto& v=candidate->values;
 v[at(air)]=p.control(at(air)).convertTo0to1(design.panel.controls[air].maximum);v[at(bite)]=v[at(bite)]<.5f?v[at(bite)]+.25f:v[at(bite)]-.25f;
 v[at(bloom)]=p.control(at(bloom)).convertTo0to1(design.panel.controls[bloom].minimum);v[at(mode)]=p.control(at(mode)).convertTo0to1(std::round(physical(mode,v[at(mode)]))<1?2.0f:0.0f);
 const auto bare=snapshot(plate);row.showProposal(candidate);pump();const auto marked=snapshot(plate);
 const std::vector<Mark> expected{{air,physical(air,v[at(air)])},{bloom,physical(bloom,v[at(bloom)])},{mode,physical(mode,v[at(mode)])}};
 CHECK(row.marks()==expected&&!row.proposesPanel());
 CHECK(description(air)==proposes(air,expected[0].proposed)&&description(bloom)==proposes(bloom,expected[1].proposed)&&description(mode)==proposes(mode,expected[2].proposed));
 CHECK(description(bite).isEmpty()&&description(attack).isEmpty()&&studio::ProgrammerLook::locked(*widget<juce::Label>(row,"rack.label.1")));
 // Drawn where the look draws the cap: dashes added at the proposed place and none at the live one.
 studio::ProgrammerLook look(*studio::studioSkin());const auto added=[&](juce::Rectangle<int> r){return whiteIn(marked,r)-whiteIn(bare,r);};
 if(auto* knob=widget<juce::Slider>(row,"rack.focus.0")){
  const auto c=plate.getLocalArea(knob,knob->getLocalBounds()).toFloat().getCentre();const auto r=look.knobRadius(*knob)*.8f;
  const auto point=[&](double value){const auto a=look.knobAngle(float(juce::jlimit(0.0,1.0,knob->valueToProportionOfLength(value))));return (c+juce::Point<float>(std::sin(a),-std::cos(a))*r).toInt();};
  const auto proposed=point(expected[0].proposed),live=point(knob->getValue());
  CHECK(proposed.getDistanceFrom(live)>24&&added(juce::Rectangle<int>(12,12).withCentre(proposed))>=3&&added(juce::Rectangle<int>(12,12).withCentre(live))==0);
 }
 if(auto* fader=widget<juce::Slider>(row,"rack.focus.2")){
  const auto origin=plate.getLocalPoint(fader,juce::Point<int>()).toFloat();
  const auto top=[&](double value){const auto cap=look.faderCap(*fader,float(fader->getPositionOfValue(value)))+origin;return juce::Rectangle<float>(cap.getX(),cap.getY()-5.0f,cap.getWidth(),4.0f).toNearestInt();};
  const auto proposed=top(expected[1].proposed),live=top(fader->getValue());
  CHECK(proposed.getY()-live.getY()>20&&added(proposed)>=8&&added(live)==0);
 }
 if(auto* box=find(row,"rack.focus."+juce::String(int(mode)))){const auto b=plate.getLocalArea(box,box->getLocalBounds());CHECK(added({b.getX(),b.getY()-5,b.getWidth(),4})>=8&&added({b.getX(),b.getBottom()+1,b.getWidth(),4})>=8);}
 if(images!=juce::File{}){auto stream=fresh(images.getChildFile("row-marks.png"));CHECK(stream&&juce::PNGImageFormat().writeImageToStream(marked,*stream));}
 // A control whose live value equals the candidate's has no mark: the candidate's AIR written live.
 auto live=sound(p);live.values[at(air)]=v[at(air)];CHECK(p.applySoundEdit(live,SoundEditOrigin::manual,revision(p)));row.refresh(sound(p));pump();
 CHECK(row.marks()==(std::vector<Mark>{expected[1],expected[2]})&&description(air).isEmpty());
 // Another panel for the same design, as Refine Rack makes: the row's outline round it, and no marks.
 const auto changed=[&](auto edit){auto out=std::make_shared<SoundDocument>(*candidate);auto state=std::make_shared<RackPersonalityState>(*out->personalities);for(auto& i:state->instances)if(i.id==s->id)edit(i);out->personalities=state;return out;};
 row.showProposal(changed([](RackInstance& i){i.design.panel.controls.pop_back();++i.design.revision;}));pump();
 CHECK(row.marks().empty()&&row.proposesPanel()&&description(bloom).isEmpty());
 {const auto outlined=snapshot(plate);CHECK(whiteIn(outlined,{40,2,960,5})-whiteIn(bare,{40,2,960,5})>=200&&whiteIn(outlined,{40,267,960,5})-whiteIn(bare,{40,267,960,5})>=200);}
 // Another design in the rack's place, a removed rack, or no proposal: nothing.
 const auto glass=designs[1];row.showProposal(changed([&](RackInstance& i){i.design=glass;}));pump();CHECK(row.marks().empty()&&!row.proposesPanel());
 auto removed=std::make_shared<SoundDocument>(*candidate);removed->personalities=nullptr;row.showProposal(removed);pump();CHECK(row.marks().empty()&&!row.proposesPanel());
 row.showProposal(candidate);pump();CHECK(row.marks().size()==2);row.showProposal(nullptr);pump();CHECK(row.marks().empty()&&!row.proposesPanel()&&description(bloom).isEmpty());
}
// The factory design of each newer kind, placed on the shared master: its header names its bank by the kind.
void rack_header_kinds(){
 std::cout<<"GROUP rack_header_kinds"<<std::endl;
 const auto factories=airack::test::frozenFactoryRackDesigns();
 const char* const names[]{"EQ","Filter","Chorus","Phaser","Flanger","Stereo","Dimension","Frequency shift","Convolution","Frequency split","Mid/side split"};
 for(std::size_t k=0;k<11;++k){
  InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(blankInstance(p));RackOperation op;op.design=factories[6+k];op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};
  CHECK(p.applyRackOperation(op,revision(p))==RackCandidateStatus::ready);
  const auto d=sound(p);if(!d.personalities||d.personalities->instances.empty()){CHECK(false);continue;}
  const auto header=rackHeaderText(d,d.personalities->instances.back());const auto want=juce::String("Shared Master | ")+names[k]+" bank ";
  if(!header.contains(want))std::cout<<"  "<<header<<std::endl;
  CHECK(header.contains(want));
 }
 header_buffering();
}
void drawer_places_only(){
 std::cout<<"GROUP drawer_places_only"<<std::endl;using Place=RackPanelComponent::Place;
 const auto items=[](RackPanelComponent& panel){std::vector<int> v;if(auto* b=widget<juce::ComboBox>(panel,"rack.operation"))for(int i=0;i<b->getNumItems();++i)v.push_back(b->getItemId(i));return v;};
 InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(copperArcInstance(p));const auto copper=sound(p).personalities->instances.front().id;
 RackPanelComponent panel(p,*studio::studioSkin());panel.setPlace(Place::drawer);panel.showInstance(copper);panel.setSize(1486,668);pump();
 CHECK(items(panel)==std::vector<int>({4,5,6,7,8}));
 for(const auto* gone:{"rack.path","rack.browse","rack.saveDesign"})CHECK(find(panel,gone)==nullptr);
 InstrumentProcessor q(offline(),preparedTestRackRuntime());CHECK(blankInstance(q));
 RackPanelComponent bare(q,*studio::studioSkin());bare.setPlace(Place::drawer);bare.showInstance(bareBankId({RackRole::synth,0}));bare.setSize(1486,668);pump();
 CHECK(items(bare).empty());
}
// The studio feedback spec's decision 7 in the drawer: no REVIEW; APPLY reviews and applies in one press, as one edit,
// and a refusal says the review's sentence and changes nothing.
void drawer_one_press(){
 std::cout<<"GROUP drawer_one_press"<<std::endl;using Place=RackPanelComponent::Place;
 InstrumentProcessor p(offline(),preparedTestRackRuntime());CHECK(copperArcInstance(p));const auto copper=sound(p).personalities->instances.front().id;
 RackPanelComponent panel(p,*studio::studioSkin());panel.setPlace(Place::drawer);panel.showInstance(copper);panel.setSize(1486,668);pump();
 CHECK(find(panel,"rack.review")==nullptr);
 const auto before=sound(p);choice(panel,"rack.operation",5);click(panel,"rack.apply");pump();
 const auto racks=[&]{const auto d=sound(p);return d.personalities?d.personalities->instances.size():std::size_t(0);};
 CHECK(racks()==2);if(racks()==2)CHECK(p.undoSoundEdit()&&sound(p)==before);
 for(std::uint8_t slot=1;slot<4;++slot)CHECK(p.applyRackEdit({RackEditKind::addSynth,slot,masterChain,slot},revision(p))==RackEditResult::applied);
 pump();const auto full=sound(p);choice(panel,"rack.operation",5);click(panel,"rack.apply");pump();
 auto* status=widget<juce::Label>(panel,"rack.status");CHECK(status!=nullptr&&status->getText()=="Capacity reached");CHECK(sound(p)==full);
}
void posted_click_behind_slow_message(){
 std::cout<<"GROUP posted_click_behind_slow_message"<<std::endl;
 CHECK(clickRunsBehindSlowMessage([](juce::Button& b){click(b);}));
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;const auto preparedSkins=prepareTestMakerSkins();if(argc==2&&juce::String(argv[1])=="--header-buffering"){header_buffering();return failures?1:0;}juce::File images;if(argc==3&&juce::String(argv[1])=="--only"&&juce::String(argv[2])=="focus"){observeGroup("native_minimum_focus",[&]{native_minimum_focus();});std::cout<<"RackPanel focus checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}const bool withoutFocus=argc==3&&juce::String(argv[1])=="--only"&&juce::String(argv[2])=="non-focus";if(argc==2){images=juce::File(argv[1]);CHECK(images.createDirectory().wasOk());}observeGroup("three_native_panel_hierarchies",[&]{three_native_panel_hierarchies(images);});observeGroup("native_library_and_ai_modes",[&]{native_library_and_ai_modes();});observeGroup("native_library_atomic_edges",[&]{native_library_atomic_edges();});observeGroup("native_library_dependency_transaction",[&]{native_library_dependency_transaction();});observeGroup("focused_failed_repair_retirement",[&]{focused_failed_repair_retirement();});observeGroup("native_focused_reentry",[&]{native_focused_reentry();});observeGroup("native_library_capacity_and_reset",[&]{native_library_capacity_and_reset();});observeGroup("native_role_destinations",[&]{native_role_destinations();});observeGroup("native_advanced_reentry",[&]{native_advanced_reentry();});if(!withoutFocus)observeGroup("native_minimum_focus",[&]{native_minimum_focus();});observeGroup("native_panel_places",[&]{native_panel_places();});observeGroup("native_panel_programmer",[&]{native_panel_programmer();});observeGroup("rack_panel_unit",[&]{rack_panel_unit();});observeGroup("rack_panel_drawer",[&]{rack_panel_drawer();});observeGroup("rack_panel_values",[&]{rack_panel_values();});observeGroup("rack_order",[&]{rack_order();});observeGroup("rack_spots",[&]{rack_spots();});observeGroup("rack_bare_banks",[&]{rack_bare_banks();});observeGroup("rack_panel_preview",[&]{rack_panel_preview();});observeGroup("rack_panel_marks",[&]{rack_panel_marks(images);});observeGroup("rack_header_kinds",[&]{rack_header_kinds();});observeGroup("drawer_places_only",[&]{drawer_places_only();});observeGroup("drawer_one_press",[&]{drawer_one_press();});observeGroup("posted_click_behind_slow_message",[&]{posted_click_behind_slow_message();});std::cout<<"RackPanel checks="<<checks<<" groups="<<(withoutFocus?23:24)<<" failures="<<failures<<std::endl;return failures?1:0;}



