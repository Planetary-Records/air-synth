#include "RackPanel.h"
#include "InstrumentProcessor.h"
#include "ParameterListenerRetirement.h"
#include "ProgrammerLook.h"
#include "ProgrammerValues.h"
#include "SliderAccessibility.h"
#include "RackExamples.h"
#include "RackOrder.h"
#include "RackReview.h"
#include "UiRetirement.h"
#include "UserReach.h"
#include <algorithm>
namespace airack {
namespace {
constexpr std::pair<int,const char*> operationItems[]{{1,"Create example"},{4,"Swap selected"},{5,"Duplicate selected"},{6,"Detach selected"},{7,"Remove selected"},{8,"Move selected"}};
constexpr const char* designsHeading="Local designs | Apply places";
void identify(juce::Component& c,const juce::String& id,const juce::String& name,int order=0){c.setComponentID(id);c.setName(name);if(order){c.setWantsKeyboardFocus(true);c.setExplicitFocusOrder(order);}}
const RackInstance* instance(const SoundDocument& d,juce::Uuid id){if(d.personalities)for(const auto& i:d.personalities->instances)if(i.id==id)return &i;return nullptr;}
using Look=studio::ProgrammerLook;
// Lines elided to fit keep their whole text as their tooltip.
struct Lines final:juce::Label{juce::String getTooltip()override{return getText();}};
// The PROGRAMMER's first row, moved up under the selection: its top, title baseline, rule, knobs' line and bottom.
constexpr float rowTop=72,titleBaseline=88,ruleY=81,knobLine=163,rowBottom=269;
// The body's height with the form under the row, and without it.
constexpr int formHeight=878,rowHeight=283;
// The AI's marks and the changed panel's outline: white dashes, 6 px on and 4 off.
const juce::Colour markColour(0xf2ffffff);
// A w x h box, rounded to whole pixels, centred across on cx with its top at top.
juce::Rectangle<int> box(float cx,float top,float w,float h){const auto iw=std::round(w),ih=std::round(h);return {int(std::round(cx-iw*.5f)),int(std::round(top)),int(iw),int(ih)};}
}
struct RackPanelComponent::Content final:juce::Component,private juce::Timer,private juce::FocusChangeListener,std::enable_shared_from_this<Content>{
 struct Control;
 // The viewed component: it engraves the dividers and the form's separator, and letters the row's titles and the fields' legends.
 struct Body final:juce::Component{Content& owner;explicit Body(Content& c):owner(c){}void paint(juce::Graphics&)override;void paintOverChildren(juce::Graphics&)override;};
 Look look;
 InstrumentProcessor& processor;juce::Component::SafePointer<RackPanelComponent> panel;
 juce::Viewport viewport;Body body{*this};Lines header,libraryLabel,status;
 studio::KeyFocus<juce::TextButton> advanced{"Advanced"},apply{"Apply"},cancel{"Cancel draft"};
 studio::KeyFocus<juce::TextButton> details{"DESIGN & MAPPINGS"};
 studio::KeyFocus<juce::ComboBox> select,examples,operation,chain,position,controlChoice,resolution;
 studio::KeyFocus<juce::ToggleButton> reuse{"Reuse banks"},moveDependencies{"Move FX to Master"};
 juce::TextEditor allocation,difference,remap;
 std::vector<std::unique_ptr<juce::Component>> sections;
 std::vector<std::shared_ptr<Control>> controls;
 std::vector<juce::Component*> fields;std::vector<studio::Engraving> lines;studio::TitleRow titles;juce::Rectangle<float> face;
 std::shared_ptr<Content> retirement;
 std::shared_ptr<void> editorRetirement;std::function<void()> detachEditorControls;
 SoundDocument shown;std::optional<RackRole> reviewedRole;std::optional<RackInstance> rendered;juce::Uuid selected=juce::Uuid::null();
 std::vector<RackOperation> draft;std::uint64_t expected=0,reset=0;bool first=true,pending=true,shuttingDown=false,formShown=true,relist=false;int frames=0;Place place=Place::all;
 bool detailsOpen=false;
 RackInventoryPtr choiceInventory;std::vector<RackInventoryRow> designs;
 std::optional<RackDesign> localDesign;std::optional<RackPackageRef> localPackage;
 RackLibraryPlacementPreparationPtr preparation;
 // A preview's rack and values; in a rack unit, the proposal's candidate and what it marks.
 std::optional<RackInstance> previewed;std::vector<float> previewValues;
 std::shared_ptr<const SoundDocument> proposal;std::vector<Mark> marked;bool outlined=false;
 std::optional<BareBank> bareBank() const{return place==Place::drawer?bareBankOf(selected):std::nullopt;}
 bool saturationDrawer() const{
  if(place!=Place::drawer)return false;
  if(const auto* i=instance(shown,selected))return std::any_of(i->design.units.begin(),i->design.units.end(),[](const auto& u){return u.module==NativeModule::saturation;});
  if(const auto b=bareBank())return bankModule(shown,b->role,b->bank)==NativeModule::saturation;
  return false;
 }
 bool compactDrawer() const{
  if(place!=Place::drawer)return false;
  if(const auto* i=instance(shown,selected))return std::any_of(i->design.units.begin(),i->design.units.end(),[](const auto& u){return u.module==NativeModule::dynamicEq||u.module==NativeModule::saturation;});
  if(const auto b=bareBank()){const auto module=bankModule(shown,b->role,b->bank);return module==NativeModule::dynamicEq||module==NativeModule::saturation;}
  return false;
 }
 void fillHeader(){
  const auto* i=instance(shown,selected);const auto b=bareBank();
  if(compactDrawer()&&!detailsOpen){
   auto text=i?i->design.name+" | Rev "+juce::String(std::to_string(i->design.revision))+(i->edited?" | Edited":""):bareBankName(shown,*b)+" | No rack";
   const bool locked=i?rackLocked(shown,*i):b->role==RackRole::fx?shown.effectLocks[b->bank]:shown.synthLocks[b->bank];
   text+=locked?" | AI locked":" | Unlocked";header.setText(text,juce::dontSendNotification);
   studio::ProgrammerLook::setRole(header,Look::Text::plain);header.setFont(look.mono.fontForCap(14));header.setColour(juce::Label::textColourId,juce::Colour(0xffb8c1b8));
  }else{
   studio::ProgrammerLook::setRole(header,Look::Text::lines);
   header.setText(i?rackHeaderText(shown,*i):b?bareBankName(shown,*b)+" | no rack\n"+bareBankPlace(shown,*b):"Generic native racks | select or create a personality\nAll physical bank controls remain in Advanced",juce::dontSendNotification);
  }
 }
 Content(InstrumentProcessor& p,const studio::Model& m,RackPanelComponent& c):look(m),processor(p),panel(&c){
  setLookAndFeel(&look);setOpaque(true);
  identify(*this,"rack.panel","Native rack personality and local library");addAndMakeVisible(viewport);identify(viewport,"rack.viewport","Scroll focused controls and design library");viewport.setViewedComponent(&body,false);viewport.setScrollBarsShown(true,false);
  auto add=[&](juce::Component& c,const char* id,const char* title,int order){identify(c,juce::String("rack.")+id,title,order);body.addAndMakeVisible(c);};
  add(details,"drawer.details","Show or hide local design and mapping details",38);Look::setCompactButton(details);
  details.getProperties().set("programmer.disclosure",true);details.setToggleable(true);
  details.onClick=[this]{if(shuttingDown)return;Frame frame(*this);detailsOpen=!detailsOpen;places();resized();};
  for(auto* field:{&allocation,&remap}){field->setFont(look.fieldFont());field->setJustification(juce::Justification::centredLeft);field->setIndents(10,0);}
  difference.setFont(look.fieldFont());difference.setIndents(10,8);
  add(select,"select","Selected committed rack personality",1);select.onChange=[this]{if(shuttingDown)return;const auto n=select.getSelectedId()-2;juce::Uuid id=juce::Uuid::null();if(n>=0&&shown.personalities&&std::size_t(n)<shown.personalities->instances.size())id=shown.personalities->instances[std::size_t(n)].id;show(id);};
  add(advanced,"advanced","Open all retained Advanced controls",11);advanced.onClick=[this]{if(panel&&panel->onAdvanced)panel->onAdvanced();};
  add(header,"header","Personality name, revision, destination, units, bypass and AI locks",0);Look::setRole(header,Look::Text::lines);
  header.setBufferedToImage(true);
  add(libraryLabel,"libraryLabel","Local design library",0);Look::setRole(libraryLabel,Look::Text::title);libraryLabel.setText(designsHeading,juce::dontSendNotification);
  add(examples,"examples","Available local design recipe",20);
  preparation=processor.rackLibraryRuntime()->client->createPlacementPreparation();
  add(operation,"operation","Local rack operation",21);operations();
  add(chain,"destination","Destination including explicit shared Master",22);chain.addItemList({"Synth 1 insert","Synth 2 insert","Synth 3 insert","Synth 4 insert","Shared Master"},1);chain.setSelectedId(5,juce::dontSendNotification);
  add(position,"position","Position in destination (one based)",23);for(int n=0;n<9;++n)position.addItem(juce::String(n+1),n+1);position.setSelectedId(2,juce::dontSendNotification);
  add(allocation,"allocation","Exact allocation: local unit:physical bank, comma separated; blank is automatic",24);allocation.setTextToShowWhenEmpty("Automatic, or 0:2,1:1 (banks start at 1)",juce::Colours::grey);
  add(reuse,"reuse","Explicitly authorize exact physical bank reuse",26);reuse.setTooltip("Reuse exact banks (retains host automation)");
  add(moveDependencies,"moveDependencies","Explicit compound removal: preserve independent FX by moving them to Shared Master",27);moveDependencies.setTooltip("On synth removal, move independent FX personalities to Shared Master");moveDependencies.onClick=[this]{invalidate();};
  add(controlChoice,"control","Focused control to remove or remap",27);controlChoice.addItem("No control resolution",1);controlChoice.setSelectedId(1,juce::dontSendNotification);
  add(resolution,"resolution","Explicit reconciliation choice",28);resolution.addItemList({"Keep mappings","Remove control","Remap control","Detach ownership"},1);resolution.setSelectedId(1,juce::dontSendNotification);
  add(remap,"remap","Local semantic parameter key for remapping",29);remap.setTextToShowWhenEmpty("e.g. analog.filter.cutoff",juce::Colours::grey);
  add(apply,"apply","Review the operation and apply it as one undoable transaction",33);add(cancel,"cancel","Cancel local draft without changing sound",34);
  add(difference,"diff","Sonic, presentation, structural, bank and macro consequences before Apply",36);difference.setMultiLine(true);difference.setReadOnly(true);difference.setScrollbarsShown(true);add(status,"status","Local library operation status",0);Look::setRole(status,Look::Text::lines);
  const std::pair<juce::Component*,const char*> legends[]{{&operation,"OPERATION"},{&examples,"EXAMPLE"},{&chain,"DESTINATION"},{&position,"POSITION"},{&allocation,"BANKS"},{&controlChoice,"CONTROL"},{&resolution,"RESOLUTION"},{&remap,"REMAP KEY"}};
  for(const auto& [field,legend]:legends){Look::setLegend(*field,legend);fields.push_back(field);}
  for(auto* field:{&allocation,&remap})field->onTextChange=[this]{invalidate();};reuse.onClick=[this]{invalidate();};
  for(auto* box:{&examples,&chain,&position,&controlChoice,&resolution,&operation})box->onChange=[this]{invalidate();};
  examples.onChange=[this]{chooseDesign();invalidate();};
  apply.onClick=[this]{commit();};cancel.onClick=[this]{invalidate();showDifference("Draft cancelled; live sound and library preserved");};
  juce::Desktop::getInstance().addFocusChangeListener(this);startTimer(20);
 }
 ~Content()override;
 struct Frame{
  std::shared_ptr<Content> owner;
  detail::ParameterListenerDispatch dispatch;
  explicit Frame(Content& c):owner(c.shared_from_this()),dispatch(c.processor){++owner->frames;}
  ~Frame(){
   if(--owner->frames==0&&owner->shuttingDown){
    owner->detachControls();auto detach=std::move(owner->detachEditorControls);if(detach)detach();
   }
   if(owner->frames==0&&(owner->retirement||owner->editorRetirement)){
    auto retained=std::move(owner->retirement);auto editor=std::move(owner->editorRetirement);
    detail::retireUi(owner->processor,[retained,editor]{});
   }
  }
 };
 void shutdown();void detachControls();
 void destinationInput(){const auto* current=instance(shown,selected);const int kind=operation.getSelectedId();const auto role=reviewedRole.value_or(kind>=5&&current?current->design.role:localDesign?localDesign->role:RackRole::synth);const bool synth=role==RackRole::synth;chain.setEnabled(!synth);chain.setTooltip(synth?"Synth physical bank comes from allocation; position is synth order. Owned inserts follow that synth. The FX chain selector does not apply.":"FX destination: Shared Master or the selected synth insert chain");position.setTooltip(synth?"Synth order position; physical bank and owned inserts are shown by Review":"FX order position in the selected chain");}
 void placementInput(bool changed=false){
  const int kind=operation.getSelectedId();if(kind!=1&&kind!=4){apply.setEnabled(true);return;}
  const auto client=processor.rackLibraryRuntime()->client;const auto view=client->placement(preparation);
  const bool ready=localDesign&&view&&client->placementCurrent(preparation,*view);apply.setEnabled(ready);
  const auto cue=localDesign?"Local package preparing or unavailable":"Local inventory preparing or unavailable";
  if(!ready&&(changed||status.getText().isEmpty()))status.setText(cue,juce::dontSendNotification);
  else if(ready&&(status.getText().startsWith("Local package preparing")||status.getText().startsWith("Local inventory preparing")))status.setText({},juce::dontSendNotification);
 }
 void invalidate(){draft.clear();reviewedRole.reset();destinationInput();placementInput(true);}
 void chooseDesign(){
  localDesign.reset();localPackage.reset();auto client=processor.rackLibraryRuntime()->client;client->cancelPlacement(preparation);
  const auto n=examples.getSelectedId()-1;if(!choiceInventory||!juce::isPositiveAndBelow(n,int(designs.size())))return;
  const auto& row=designs[std::size_t(n)];RackDesign design;std::optional<RackPackageRef> ref;
  if(!resolveInventoryDesign(*choiceInventory,row.metadata.design,row.metadata.revision,row.metadata.canonicalSha256,design,ref)||ref!=row.package)return;
  localDesign=std::move(design);localPackage=ref;std::vector<RackPackageRef> refs;if(ref&&row.installed)refs.push_back(*ref);
  client->requestPlacement(preparation,choiceInventory,refs);
 }
 void refreshChoices(){
  if(place!=Place::all&&place!=Place::drawer)return;
  RackInventoryPtr next;if(!processor.captureAvailableRackInventory(next)||!next||next==choiceInventory)return;
  std::optional<RackInventoryRow> prior;const auto old=examples.getSelectedId()-1;if(juce::isPositiveAndBelow(old,int(designs.size())))prior=designs[std::size_t(old)];
  choiceInventory=std::move(next);designs.assign(choiceInventory->rows().begin(),choiceInventory->rows().end());examples.clear(juce::dontSendNotification);int selectedId=1;
  for(std::size_t n=0;n<designs.size();++n){const auto& row=designs[n];examples.addItem(row.metadata.name+(row.installed?juce::String():" (project recipe)"),int(n+1));
   if(prior&&row.metadata.design==prior->metadata.design&&row.metadata.revision==prior->metadata.revision&&row.metadata.canonicalSha256==prior->metadata.canonicalSha256&&row.package==prior->package)selectedId=int(n+1);
  }
  examples.setSelectedId(designs.empty()?0:selectedId,juce::dontSendNotification);chooseDesign();invalidate();
 }
 void showDifference(const juce::String& text){difference.setText(text,false);places();}
 void operations(){operation.clear(juce::dontSendNotification);if(bareBank())return;for(const auto& [n,text]:operationItems)if(n>1||place==Place::all)operation.addItem(text,n);operation.setSelectedId(place==Place::all?1:4,juce::dontSendNotification);}
 // Under the unit and in the drawer the form shows only for a selected rack; a rack unit's panel shows its row alone,
 // and a generic rack's drawer its header alone.
 void places(){
  const bool unit=place==Place::unit||place==Place::preview,drawer=place==Place::drawer,compact=compactDrawer(),form=!unit&&(place==Place::all||instance(shown,selected)!=nullptr)&&(!compact||detailsOpen);
  for(auto* c:std::initializer_list<juce::Component*>{&examples,&chain,&position,&reuse,&controlChoice,&resolution,&remap,&moveDependencies,&allocation,&libraryLabel})c->setVisible(form);
  for(auto* c:std::initializer_list<juce::Component*>{&operation,&apply,&cancel,&status})c->setVisible(form);
  // In the drawer the difference box shows once it has something to say, and the drawer grows to hold it.
  const bool box=form&&(!drawer||!difference.isEmpty()),boxChanged=box!=difference.isVisible();difference.setVisible(box);
  select.setVisible(!unit&&!drawer);header.setVisible(!unit);advanced.setVisible(!unit&&(!compact||detailsOpen));
  details.setVisible(compact&&!saturationDrawer());details.setToggleState(detailsOpen,juce::dontSendNotification);fillHeader();
  const bool formChanged=form!=formShown;if(formChanged){formShown=form;resized();}
  if((boxChanged||formChanged)&&drawer&&panel&&panel->onHeight)panel->onHeight();
 }
 // A new place takes the first rack at the next update, not here: the editor wires onSelectionChanged after it sets the place.
 // In a rack unit and in the drawer the stack names the parts; the drawer has no row, and Tab reaches ADVANCED after the form.
 void setPlace(Place p){
  if(place==p)return;place=p;detailsOpen=false;relist=true;const bool stacked=p!=Place::all;
  setComponentID(stacked?juce::String():"rack.panel");viewport.setComponentID(stacked?juce::String():"rack.viewport");select.setComponentID(stacked?juce::String():"rack.select");
  viewport.setScrollBarsShown(!stacked,false);viewport.setWantsKeyboardFocus(!stacked);setOpaque(p==Place::all||p==Place::drawer);advanced.setExplicitFocusOrder(p==Place::drawer?36:11);difference.setExplicitFocusOrder(p==Place::drawer?37:36);
  operations();invalidate();difference.clear();status.setText({},juce::dontSendNotification);pending=true;if(!frames)rebuild();places();resized();
 }
 void show(juce::Uuid id){
  if(selected==id&&!first)return;const bool wasBare=bareBank().has_value();selected=id;detailsOpen=false;pending=true;invalidate();if(place==Place::drawer)difference.clear();
  if(bareBank().has_value()!=wasBare)operations();
  int selection=1;if(shown.personalities)for(std::size_t n=0;n<shown.personalities->instances.size();++n)if(shown.personalities->instances[n].id==id)selection=int(n)+2;select.setSelectedId(selection,juce::dontSendNotification);places();if(panel&&panel->onSelectionChanged)panel->onSelectionChanged(id);}
 void globalFocusChanged(juce::Component* c)override{if(shuttingDown||!c||!body.isParentOf(c))return;auto* target=c->getComponentID().startsWith("rack.focus.")?c->getParentComponent():c;const auto r=body.getLocalArea(target,target->getLocalBounds());const int height=viewport.getViewHeight(),top=viewport.getViewPositionY();if(r.getY()<top||r.getBottom()>top+height)viewport.setViewPosition(0,juce::jmax(0,r.getHeight()>height?r.getBottom()-height:r.getCentreY()-height/2));}
 void showDesign(const RackInstance&,std::vector<float>);void remark();
 void update(const SoundDocument& d);
 void rebuild();void paint(juce::Graphics&)override;void paintOverChildren(juce::Graphics&)override;void resized()override;void timerCallback()override;
 void prepare();void commit();
};
// A control's cell: its widget, its name over its value under it. Only the widget takes the mouse, and the value for its
// tooltip, which holds the whole readout.
struct RackPanelComponent::Content::Control final:juce::Component,std::enable_shared_from_this<Control>{
 // Hold the same Content frame across JUCE's continuations after each slider callback.
 struct Slider final:studio::KeyFocus<juce::Slider>{
  Content& owner;explicit Slider(Content& c):owner(c){}
  void mouseDown(const juce::MouseEvent& e)override{Frame frame(owner);juce::Slider::mouseDown(e);}
  void mouseDrag(const juce::MouseEvent& e)override{Frame frame(owner);juce::Slider::mouseDrag(e);}
  void mouseUp(const juce::MouseEvent& e)override{Frame frame(owner);juce::Slider::mouseUp(e);}
  void mouseDoubleClick(const juce::MouseEvent& e)override{Frame frame(owner);juce::Slider::mouseDoubleClick(e);}
  void mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& w)override{Frame frame(owner);studio::KeyFocus<juce::Slider>::mouseWheelMove(e,w);}
  std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler()override{return std::make_unique<detail::SliderAccessibility>(*this,owner.processor,[this]{return std::make_shared<Frame>(owner);});}
 };
 Content& owner;FocusedControl spec;std::size_t index=parameterCount,number;juce::Label label,value;Slider slider;studio::KeyFocus<juce::ComboBox> choice;studio::KeyFocus<juce::ToggleButton> toggle;
 std::unique_ptr<juce::ParameterAttachment> attachment;bool dragging=false,compatible=false;
 Control(Content& c,const RackInstance& i,std::size_t n):owner(c),spec(i.design.panel.controls[n]),number(n),slider(c){
  setInterceptsMouseClicks(false,true);setExplicitFocusOrder(int(n)+3);
  const auto u=std::find_if(i.design.units.begin(),i.design.units.end(),[&](const auto& v){return v.id==spec.unit;});const auto b=std::find_if(i.bindings.begin(),i.bindings.end(),[&](auto v){return v.unit==spec.unit;});if(u!=i.design.units.end()&&b!=i.bindings.end()){auto p=rackParameterIndex(u->module,b->slot,spec.key);if(p)index=*p;}
  label.setText(spec.label,juce::dontSendNotification);identify(label,"rack.label."+juce::String(int(n)),spec.label+" label");Look::setRole(label,Look::Text::name);label.setInterceptsMouseClicks(false,false);addAndMakeVisible(label);addAndMakeVisible(value);identify(value,"rack.value."+juce::String(int(n)),spec.label+" full physical value and mapping reason");Look::setRole(value,Look::Text::value);
  if(spec.widget!=FocusWidget::choice&&spec.widget!=FocusWidget::toggle){slider.setSliderStyle(spec.widget==FocusWidget::verticalSlider?juce::Slider::LinearVertical:juce::Slider::RotaryHorizontalVerticalDrag);slider.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);juce::NormalisableRange<double> range(spec.minimum,spec.maximum);if(spec.response==FocusResponse::logarithmic){range=juce::NormalisableRange<double>(spec.minimum,spec.maximum,[](double low,double high,double p){return low*std::pow(high/low,p);},[](double low,double high,double p){return std::log(p/low)/std::log(high/low);});}slider.setNormalisableRange(range);}
  identify(widget(),"rack.focus."+juce::String(int(n)),spec.label,int(n)+3);addAndMakeVisible(widget());
  if(owner.place==Place::preview){setExplicitFocusOrder(0);widget().setExplicitFocusOrder(0);widget().setWantsKeyboardFocus(false);setInterceptsMouseClicks(false,false);}
  if(index<parameterCount){const auto& d=parameterRegistry()[index];label.setTooltip(d.id+" / "+d.unit);slider.setTooltip(d.id+" / "+d.unit);choice.setTooltip(d.id+" / "+d.unit);toggle.setTooltip(d.id+" / "+d.unit);if(spec.widget==FocusWidget::choice)for(int k=int(spec.minimum);k<=int(spec.maximum);++k)choice.addItem(d.choices[std::size_t(k)],k+1);}
 }
 juce::Component& widget(){if(spec.widget==FocusWidget::choice)return choice;if(spec.widget==FocusWidget::toggle)return toggle;return slider;}
 void attach(){const auto weak=weak_from_this();slider.onDragStart=[weak]{if(auto self=weak.lock())self->begin();};slider.onDragEnd=[weak]{if(auto self=weak.lock())self->end();};slider.onValueChange=[weak]{if(auto self=weak.lock())self->write(float(self->slider.getValue()));};choice.onChange=[weak]{if(auto self=weak.lock())self->write(float(self->choice.getSelectedId()-1));};toggle.onClick=[weak]{if(auto self=weak.lock())self->write(self->toggle.getToggleState()?1.f:0.f);};if(index<parameterCount){attachment=std::make_unique<juce::ParameterAttachment>(owner.processor.control(index),[weak](float v){if(auto self=weak.lock())self->read(v);});attachment->sendInitialUpdate();}}
 ~Control()override{attachment.reset();}
 void begin(){Content::Frame frame(owner);if(!compatible||dragging||owner.shuttingDown)return;dragging=owner.processor.beginParameterGesture(index);if((owner.shuttingDown||!compatible||owner.pending)&&dragging)end();}
 void end(){Content::Frame frame(owner);if(dragging){dragging=false;owner.processor.endParameterGesture(index);}}
 void write(float physical){Content::Frame frame(owner);if(!compatible||owner.shuttingDown||owner.pending)return;const bool discrete=!dragging;if(discrete&&!owner.processor.beginParameterGesture(index))return;const bool ok=!owner.shuttingDown&&compatible&&!owner.pending&&owner.processor.setParameterGestureValue(index,owner.processor.control(index).convertTo0to1(physical));if(discrete)owner.processor.endParameterGesture(index);if(!ok&&index<parameterCount)read(owner.processor.control(index).convertFrom0to1(owner.processor.control(index).getValue()));}
 void read(float physical){
  if(index>=parameterCount){setEnabled(false);value.setText("Unknown mapping | Advanced",juce::dontSendNotification);return;}
  const auto& d=parameterRegistry()[index];const auto* i=instance(owner.shown,owner.selected);const bool active=i&&owner.rendered&&i->bindings==owner.rendered->bindings&&number<i->design.panel.controls.size()&&i->design.panel.controls[number]==spec&&rackControlActive(owner.shown,*i,number);const bool outside=physical<spec.minimum||physical>spec.maximum;
  compatible=active&&!outside;slider.setEnabled(compatible);choice.setEnabled(compatible);toggle.setEnabled(compatible);label.setEnabled(compatible);value.setEnabled(compatible);Look::setLocked(label,owner.shown.controlLocks[index]);
  auto text=studio::formatParameter(index,physical);if(!active)text+=" | retained incompatible mapping | Advanced";else if(outside)text+=" | outside useful range | Advanced";if(owner.shown.controlLocks[index])text+=" | AI locked (manual editable)";
  for(std::size_t n=0;n<owner.shown.macros.size();++n)if(owner.shown.macros[n].targetKey==d.stateKey)text+=" | Macro "+juce::String(int(n+1))+" position "+juce::String(owner.shown.values[n+1],3)+" depth "+juce::String(owner.shown.macros[n].amount,3);
  set(physical,text);
 }
 // In a preview: the value it is shown, lettered as read() letters a compatible control's.
 void show(float normalized){if(index>=parameterCount){value.setText("Unknown mapping",juce::dontSendNotification);return;}const auto physical=owner.processor.control(index).convertFrom0to1(normalized);set(physical,studio::formatParameter(index,physical));}
 void set(float physical,const juce::String& text){const auto& d=parameterRegistry()[index];value.setText(text,juce::dontSendNotification);value.setTooltip(d.id+" / "+d.unit+" / "+text);slider.setValue(physical,juce::dontSendNotification);choice.setSelectedId(int(std::round(physical))+1,juce::dontSendNotification);toggle.setToggleState(physical>=.5f,juce::dontSendNotification);}
};
// Components may survive until the next message turn; their parameter listeners
// must not survive the processor. A callback's frame postpones detachment until
// its begin/write/end notification has returned, preserving the host's dispatch.
void RackPanelComponent::Content::detachControls(){
 const auto keep=shared_from_this();
 if(detail::ParameterListenerDispatch::finishAfterDispatch(processor,[keep]{keep->detachControls();}))return;
 for(auto& c:controls){detail::ParameterListenerDispatch::retire(processor,c->attachment);if(c->dragging){c->dragging=false;processor.endParameterGesture(c->index);}}
}
void RackPanelComponent::Content::shutdown(){
 if(shuttingDown)return;
 shuttingDown=true;stopTimer();juce::Desktop::getInstance().removeFocusChangeListener(this);
 processor.rackLibraryRuntime()->client->cancelPlacement(preparation);
 if(frames)retirement=shared_from_this();else detachControls();
 panel=nullptr;
}
RackPanelComponent::Content::~Content(){stopTimer();juce::Desktop::getInstance().removeFocusChangeListener(this);for(auto& c:controls)if(c->dragging){c->dragging=false;processor.endParameterGesture(c->index);}controls.clear();setLookAndFeel(nullptr);}
void RackPanelComponent::Content::Body::paint(juce::Graphics& g){for(const auto& l:owner.lines)owner.look.drawEngraving(g,l);owner.look.drawTitles(g,owner.titles);for(auto* f:owner.fields)owner.look.drawLegend(g,*f);}
// Each mark where the look draws the control: a fader's cap outlined at the proposed position, a knob's pointer from its
// centre to its skirt at the proposed turn, and a choice box or toggle outlined round it.
void RackPanelComponent::Content::Body::paintOverChildren(juce::Graphics& g){
 for(const auto& m:owner.marked){
  if(m.control>=owner.controls.size())continue;auto& c=*owner.controls[m.control];auto& w=c.widget();const auto r=getLocalArea(&w,w.getLocalBounds()).toFloat();juce::Path path;
  if(c.spec.widget==FocusWidget::choice||c.spec.widget==FocusWidget::toggle)path.addRectangle(r.expanded(3.0f));
  else{
   const auto value=juce::jlimit(c.slider.getMinimum(),c.slider.getMaximum(),double(m.proposed));
   if(c.spec.widget==FocusWidget::verticalSlider)path.addRectangle((owner.look.faderCap(c.slider,float(c.slider.getPositionOfValue(value)))+r.getPosition()).expanded(2.0f));
   else{const auto a=owner.look.knobAngle(float(c.slider.valueToProportionOfLength(value)));path.startNewSubPath(r.getCentre());path.lineTo(r.getCentre()+juce::Point<float>(std::sin(a),-std::cos(a))*owner.look.knobRadius(c.slider));}
  }
  Look::drawDashes(g,path,markColour,6.0f,4.0f);
 }
}
void RackPanelComponent::Content::paintOverChildren(juce::Graphics& g){if(!outlined)return;juce::Path path;path.addRectangle(getLocalBounds().toFloat().reduced(4.0f));Look::drawDashes(g,path,markColour,6.0f,4.0f);}
void RackPanelComponent::Content::showDesign(const RackInstance& i,std::vector<float> values){
 if(previewed&&*previewed==i){if(values==previewValues)return;previewValues=std::move(values);for(auto& c:controls)if(c->number<previewValues.size())c->show(previewValues[c->number]);return;}
 previewed=i;previewValues=std::move(values);pending=true;if(place==Place::preview)rebuild();
}
// A control is marked when the candidate keeps the rack drawn with the same sections, controls and bindings, the control
// is not locked live, and its value differs from the live one; kept with others, the row has the outline instead.
void RackPanelComponent::Content::remark(){
 std::vector<Mark> m;bool outline=false;const auto* kept=place==Place::unit&&proposal&&rendered?instance(*proposal,rendered->id):nullptr;
 if(kept&&kept->design.id==rendered->design.id){
  const auto& a=kept->design.panel;const auto& b=rendered->design.panel;
  if(a.sections!=b.sections||a.controls!=b.controls||kept->bindings!=rendered->bindings)outline=true;
  else for(const auto& c:controls)if(c->index<parameterCount&&!shown.controlLocks[c->index]&&std::abs(proposal->values[c->index]-shown.values[c->index])>1e-6f)m.push_back({c->number,processor.control(c->index).convertFrom0to1(proposal->values[c->index])});
 }
 for(const auto& c:controls){const auto k=std::find_if(m.begin(),m.end(),[&](const Mark& x){return x.control==c->number;});c->widget().setDescription(k==m.end()?juce::String():"AI proposes "+studio::formatParameter(c->index,k->proposed));}
 if(m!=marked||outline!=outlined){marked=std::move(m);outlined=outline;repaint();}
}
void RackPanelComponent::Content::timerCallback(){if(shuttingDown||place==Place::preview)return;refreshChoices();placementInput();SoundDocument d;std::uint64_t edit,token;if(!processor.trySoundSnapshot(d,edit,token))return;if(!first&&token!=reset){invalidate();status.setText("Sound recalled; draft cancelled",juce::dontSendNotification);}reset=token;if(!draft.empty()&&edit!=expected)invalidate();update(d);}
void RackPanelComponent::Content::update(const SoundDocument& d){
 if(shuttingDown||place==Place::preview)return;
 const bool listChanged=first||relist||bool(shown.personalities)!=bool(d.personalities)||(shown.personalities&&d.personalities&&shown.personalities->instances!=d.personalities->instances);
 shown=d;relist=false;if(listChanged){select.clear(juce::dontSendNotification);select.addItem("Generic / no personality",1);int id=2;if(d.personalities)for(const auto& i:d.personalities->instances){select.addItem(i.design.name+(i.edited?" | Edited":""),id);if(i.id==selected)select.setSelectedId(id,juce::dontSendNotification);++id;}if(!instance(d,selected)){if(!selected.isNull()&&!bareBank()){selected=juce::Uuid::null();invalidate();if(panel&&panel->onSelectionChanged)panel->onSelectionChanged(selected);}select.setSelectedId(1,juce::dontSendNotification);}}
 const auto* i=instance(d,selected);if((i&&(!rendered||rendered->design!=i->design||rendered->bindings!=i->bindings))||(!i&&rendered))pending=true;
 if(pending&&frames==0){bool ended=false;for(auto& c:controls)if(c->dragging){c->end();ended=true;}if(!ended&&frames==0)rebuild();}
 for(auto& c:controls)if(c->index<parameterCount)c->read(processor.control(c->index).convertFrom0to1(processor.control(c->index).getValue()));
 first=false;destinationInput();places();remark();
}
void RackPanelComponent::Content::rebuild(){
 if(frames||shuttingDown)return;controls.clear();sections.clear();const bool preview=place==Place::preview;const auto* i=preview?(previewed?&*previewed:nullptr):instance(shown,selected);rendered=i?std::optional<RackInstance>(*i):std::nullopt;pending=false;controlChoice.clear(juce::dontSendNotification);controlChoice.addItem("No control resolution",1);controlChoice.setSelectedId(1,juce::dontSendNotification);
 if(i){for(std::size_t n=0;n<i->design.panel.controls.size();++n)controlChoice.addItem(i->design.panel.controls[n].label,int(n)+2);}
 if(i&&place!=Place::drawer){for(std::size_t n=0;n<i->design.panel.sections.size();++n){auto section=std::make_unique<juce::Component>();identify(*section,"rack.section."+juce::String(int(n)),i->design.panel.sections[n]);section->setInterceptsMouseClicks(false,true);body.addAndMakeVisible(*section);sections.push_back(std::move(section));}
  for(std::size_t n=0;n<i->design.panel.controls.size();++n){auto c=std::make_shared<Control>(*this,*i,n);auto& section=*sections[c->spec.section];if(section.getExplicitFocusOrder()==0)section.setExplicitFocusOrder(int(n)+3);section.addAndMakeVisible(*c);controls.push_back(c);if(!preview)c->attach();else if(n<previewValues.size())c->show(previewValues[n]);}}
 places();resized();remark();
}
void RackPanelComponent::Content::paint(juce::Graphics& g){if(place==Place::all||place==Place::drawer)look.drawFace(g,place==Place::drawer&&!face.isEmpty()?face:getLocalBounds().toFloat());}
// In the panel's own pixels: the band (the selection and the header), the row by the PROGRAMMER's rule with ADVANCED in
// its own column at the right, and the form under an engraved separator. A rack unit's row spans the panel, centred in
// its height; the drawer's header spans the band and the form comes up into the row's place, ADVANCED at the keys' end.
void RackPanelComponent::Content::resized(){
 viewport.setBounds(getLocalBounds());
 const bool unit=place==Place::unit||place==Place::preview,drawer=place==Place::drawer,fitted=unit||drawer;
 // A rack unit's row starts 17 px in, so its first name's box (the pitch and 24) and an emphasised first control fit.
 const int width=fitted?getWidth():juce::jmax(getWidth(),720),fy=drawer?-210:0;const float bx1=float(width-95),left=unit?17.0f:93.0f,right=unit?float(width):std::round(bx1-80.0f);const bool form=operation.isVisible();
 const float dy=unit?std::round((float(getHeight())-(rowBottom-rowTop))*.5f)-rowTop:0.0f,top=rowTop+dy,bottom=rowBottom+dy,knobs=knobLine+dy;
 body.setSize(fitted?getWidth():width-12,fitted?getHeight():form?formHeight:rowHeight);lines.clear();titles={};
 if(compactDrawer()&&!saturationDrawer()){
  const int margin=juce::roundToInt(72.f*std::min(1.f,float(width)/1485.f));
  details.setBounds(width-margin-230,7,230,30);
  if(!detailsOpen){header.setBounds(margin,7,juce::jmax(0,details.getX()-margin-16),30);lines.push_back({{float(margin),0,float(width-2*margin),1},false});return;}
 }
 select.setBounds(93,14,440,44);header.setBounds(juce::Rectangle<float>::leftTopRightBottom(drawer?93.0f:557.0f,12.0f,bx1,66.0f).toNearestInt());
 if(compactDrawer()&&!saturationDrawer())header.setBounds(header.getBounds().withRight(details.getX()-16));
 advanced.setBounds(int(std::round(bx1-82.0f)),drawer?350:int(std::round(knobLine-51.5f)),84,96);
 if(!drawer){
  std::vector<int> counts(sections.size());for(auto& c:controls)++counts[c->spec.section];
  const auto row=studio::layoutRow(look.model.programmer,counts,left,right);
  juce::StringArray names;for(auto& s:sections)names.add(s->getName().toUpperCase());
  titles=studio::layoutTitles(look.model.programmer,look.faces,names,row.dividers,left,right-float(double(look.model.programmer["body"]["layout"]["rule_end"])),titleBaseline+dy,ruleY+dy);
  for(const auto d:row.dividers)lines.push_back({{d,top,1.0f,bottom-top},true});
  if(!unit)lines.push_back({{right,top,1.0f,bottom-top},true});
  // Each section spans its column of the row, widened to hold its cells.
  std::vector<juce::Rectangle<int>> spans;
  for(std::size_t s=0;s<sections.size();++s)spans.push_back(juce::Rectangle<float>::leftTopRightBottom(s==0?left:row.dividers[s-1]+2.0f,top,s+1==sections.size()?right-1.0f:row.dividers[s]-1.0f,bottom).toNearestInt());
  struct Cell{Control* control;juce::Rectangle<int> widget,name,value;};std::vector<Cell> cells;std::vector<std::size_t> next(sections.size());
  for(std::size_t k=0;k<controls.size();++k){
   auto& c=*controls[k];const auto s=c.spec.section;if(s>=sections.size()||next[s]>=row.centres[s].size())continue;
   const float scale=k==0&&rendered&&rendered->design.panel.emphasis?Look::emphasis:1.0f,cx=row.centres[s][next[s]++],pitch=row.pitches[s];
   juce::Rectangle<int> w;
   if(c.spec.widget==FocusWidget::verticalSlider)w=box(cx,knobs-62.0f*scale,44.0f*scale,108.0f*scale);
   else if(c.spec.widget==FocusWidget::toggle)w=box(cx,knobs-51.5f*scale,56.0f*scale,81.0f*scale);
   else if(c.spec.widget==FocusWidget::choice){const auto h=std::round(36.0f*scale);w=box(cx,knobs-h*.5f,100.0f*scale,h);}
   else{const auto size=std::round(103.0f*scale);w=box(cx,knobs-size*.5f,size,size);}
   const float baseline=knobs+65.27f*scale;
   cells.push_back({&c,w,box(cx,baseline-16.0f,pitch+24.0f,22.0f),box(cx,baseline+17.0f-14.0f,pitch-4.0f,18.0f)});
   spans[s]=spans[s].getUnion(w).getUnion(cells.back().name).getUnion(cells.back().value);
  }
  for(std::size_t s=0;s<sections.size();++s)sections[s]->setBounds(spans[s]);
  for(const auto& cell:cells){const auto u=cell.widget.getUnion(cell.name).getUnion(cell.value);const auto at=u.getPosition();cell.control->setBounds(u-spans[cell.control->spec.section].getPosition());cell.control->widget().setBounds(cell.widget-at);cell.control->label.setBounds(cell.name-at);cell.control->value.setBounds(cell.value-at);}
 }
 if(form){
  const auto y=[fy](float at){return at+float(fy);};
  // The drawer's top, where it meets the all-controls panel, engraved as the separator under its header is.
  if(drawer)lines.push_back({juce::Rectangle<float>::leftTopRightBottom(93.0f,0.0f,bx1+1.0f,1.0f),false});
  lines.push_back({juce::Rectangle<float>::leftTopRightBottom(93.0f,y(286.0f),bx1+1.0f,y(287.0f)),false});
  libraryLabel.setBounds(juce::Rectangle<float>::leftTopRightBottom(93.0f,y(300.0f),bx1+1.0f,y(324.0f)).toNearestInt());
  // Four columns of glass fields under their legends.
  const float column=(bx1-165.0f)/4.0f;
  const auto field=[&](juce::Component& c,int col,int line,int span){c.setBounds(juce::Rectangle<float>(93.0f+float(col)*(column+24.0f),y(360.0f)+72.0f*float(line),column*float(span)+24.0f*float(span-1),40.0f).toNearestInt());};
  field(operation,0,0,1);field(examples,1,0,1);field(chain,2,0,1);field(position,3,0,1);
  field(allocation,0,1,1);field(controlChoice,2,1,1);field(resolution,3,1,1);
  field(remap,0,2,1);
  // The actions as sub-page buttons, then the two toggles, 16 px apart: their legends are wider than the PROGRAMMER's.
  juce::Component* buttons[]{&apply,&cancel};
  for(int b=0;b<2;++b)buttons[b]->setBounds(93+100*b,560+fy,84,96);
  const int toggles=cancel.getRight()+40;reuse.setBounds(toggles,560+fy,120,96);moveDependencies.setBounds(toggles+136,560+fy,120,96);
  status.setBounds(juce::Rectangle<float>::leftTopRightBottom(93.0f,y(664.0f),bx1,y(690.0f)).toNearestInt());
  difference.setBounds(juce::Rectangle<float>::leftTopRightBottom(93.0f,y(698.0f),bx1,y(858.0f)).toNearestInt());
 }
 body.repaint();
}
void RackPanelComponent::Content::prepare(){
 if(shuttingDown)return;
 invalidate();
 if(operation.getSelectedId()<=4&&!localDesign){status.setText("Local inventory preparing or unavailable",juce::dontSendNotification);return;}
 auto review=reviewRack(processor,{.operation=operation.getSelectedId(),.localDesign=localDesign,.package=localPackage,.allocation=allocation.getText(),.reuse=reuse.getToggleState(),.chain=std::uint8_t(chain.getSelectedId()-1),.position=std::uint8_t(position.getSelectedId()-1),.selected=selected,.moveDependencies=moveDependencies.getToggleState(),.control=controlChoice.getSelectedId()-2,.resolution=resolution.getSelectedId(),.remap=remap.getText()});
 if(review.role){reviewedRole=review.role;destinationInput();}if(review.difference.isNotEmpty())showDifference(review.difference);if(review.state.isEmpty())return;
 if(!review.draft.empty()){draft=std::move(review.draft);expected=review.expected;reset=review.reset;}status.setText(review.state,juce::dontSendNotification);
}
void RackPanelComponent::Content::commit(){Frame frame(*this);if(shuttingDown)return;prepare();if(draft.empty())return;const RackReview reviewed{.draft=draft,.expected=expected,.reset=reset};invalidate();const auto result=operation.getSelectedId()<=4?applyRackReview(processor,reviewed,preparation,processor.rackLibraryRuntime()->client->placement(preparation)):applyRackReview(processor,reviewed);if(shuttingDown||result.status.isEmpty())return;
 // The drawer leaves the selection to the stack, which may close it.
 if(place!=Place::drawer&&result.placed)show(*result.placed);status.setText(result.status,juce::dontSendNotification);
 if(place==Place::drawer&&panel&&panel->onApplied){const auto applied=panel->onApplied;applied(result.status,result.placed);}
 timerCallback();}
RackPanelComponent::RackPanelComponent(InstrumentProcessor& p,const studio::Model& m):content(std::make_shared<Content>(p,m,*this)){setName("Native focused rack personality");addAndMakeVisible(*content);content->timerCallback();}
RackPanelComponent::~RackPanelComponent(){content->shutdown();removeChildComponent(content.get());content.reset();}
void RackPanelComponent::showInstance(juce::Uuid id){content->show(id);}
void RackPanelComponent::showStatus(const juce::String& text){content->status.setText(text,juce::dontSendNotification);if(content->compactDrawer()&&!text.isEmpty()){content->detailsOpen=true;content->places();content->resized();}}
void RackPanelComponent::refresh(const SoundDocument& d){content->update(d);}
void RackPanelComponent::setPlace(Place p){content->setPlace(p);}
RackPanelComponent::Place RackPanelComponent::place() const{return content->place;}
std::shared_ptr<void> RackPanelComponent::editorCallbackFrame(){return std::make_shared<Content::Frame>(*content);}
bool RackPanelComponent::deferEditorDestruction(std::shared_ptr<void> editor,std::function<void()> detach){
 if(!content->frames){content->shutdown();return false;}
 content->editorRetirement=std::move(editor);content->detachEditorControls=std::move(detach);
 content->shutdown();return true;
}
void RackPanelComponent::retire(){content->shutdown();}
void RackPanelComponent::resized(){content->setBounds(getLocalBounds());}
void RackPanelComponent::showDesign(const RackInstance& i,std::vector<float> values){content->showDesign(i,std::move(values));}
void RackPanelComponent::showProposal(std::shared_ptr<const SoundDocument> d){if(content->proposal==d)return;content->proposal=std::move(d);content->remark();}
std::vector<RackPanelComponent::Mark> RackPanelComponent::marks() const{return content->marked;}
bool RackPanelComponent::proposesPanel() const{return content->outlined;}
bool RackPanelComponent::detailsCollapsed() const{return content->compactDrawer()&&!content->detailsOpen;}
int RackPanelComponent::drawerHeight() const{return detailsCollapsed()?(content->saturationDrawer()?0:44):content->difference.isVisible()?668:500;}
void RackPanelComponent::setDetailsOpen(bool open){
 if(!content->compactDrawer()||content->detailsOpen==open)return;
 Content::Frame frame(*content);content->detailsOpen=open;content->places();content->resized();
 if(onDetailsChanged)onDetailsChanged(open);
 if(open&&content->operation.isShowing())content->operation.grabKeyboardFocus();
}
bool RackPanelComponent::keyPressed(const juce::KeyPress& key){
 if(content->saturationDrawer()&&content->detailsOpen&&key.isKeyCode(juce::KeyPress::escapeKey)){setDetailsOpen(false);return true;}return false;
}
void RackPanelComponent::setFace(juce::Rectangle<float> area){if(area!=content->face){content->face=area;content->repaint();}}
std::vector<std::size_t> RackPanelComponent::parameters() const{std::vector<std::size_t> all;if(content->place==Place::unit)for(const auto& c:content->controls)if(c->index<parameterCount)all.push_back(c->index);return all;}
}
