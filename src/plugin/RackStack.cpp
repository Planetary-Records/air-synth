#include "RackStack.h"
#include "DynamicEqGraph.h"
#include "SpaceReverbGraph.h"
#include "EqOverview.h"
#include "EqMatchPanel.h"
#include "FxAnalysisClock.h"
#include "DotMatrix.h"
#include "EmptyRackSpace.h"
#include "HouseUnit.h"
#include "InstrumentProcessor.h"
#include "RackReview.h"
#include "UiRetirement.h"
#include "SkinControls.h"
#include "UserReach.h"
#include "WheelLatch.h"
#include <algorithm>
#include <cmath>
#include <span>
namespace airack {
namespace {
using Look=studio::ProgrammerLook;
using studio::Pt;
using studio::Rect;
// EIA-310, in millimetres: a unit's height; the holes, 8.75 in from each end of the panel; and the screws' rows from a
// plate's top: a 2U plate's top and bottom holes, a 1U strip's middle hole.
constexpr float unitMm=44.45f,holeIn=8.75f,topRow=6.35f,middleRow=22.225f,bottomRow=82.55f;
// The rails in the fold rail's colours, the seam between units, the selection light with its fringe and halo, and the
// rails' numbers.
const juce::Colour railFill(0xff141517),railEdge(0xff2c2d31),seam(0xff040405),light(0xffdbe8ff),fringe(0xff96afeb),halo(0xff5a7de6),numberInk(0xff8a8c92);
// A placement's dashes and an empty slot's letters; the AI's marks, as the rows draw them.
const juce::Colour slotInk(0xffc8ccd2),markInk(0xf2ffffff);
// The house wordmark's ivory, as the house unit prints it; a carving's recess and the light on its lower edges.
const juce::Colour ivory(0xffeeeae0),recess(0x8c000000),lip(0x26ffffff);
const RackInstance* instance(const SoundDocument& d,juce::Uuid id){if(d.personalities)for(const auto& i:d.personalities->instances)if(i.id==id)return &i;return nullptr;}
bool sameSkin(const RackInstance& a,const RackInstance& b){return a.design==b.design&&a.package==b.package;}
std::span<const RackInstance> skinInstances(const SoundDocument& d){if(d.personalities)return d.personalities->instances;return {};}
// The units a drawer's panel shows: the rack's, or a generic rack's one bank.
std::vector<studio::AllControlsPanel::Unit> drawnUnits(const SoundDocument& d,juce::Uuid id){
 if(const auto* i=instance(d,id))return studio::panelUnits(*i);
 if(const auto b=bareBankOf(id)){
  const auto module=bankModule(d,b->role,b->bank);
  return {{module,b->bank,module==NativeModule::wavetable?d.compatibility.synth[b->bank][1]:modernSynthAlgorithmRevision}};
 }
 return {};
}
float railW(){return (float)studio::HouseUnit::railW;}
// Stage px a design px: the stage's width over the panel and its two rails.
float stackScale(const studio::Model& m){return (float)(int)m.layout["stage"][0]/(2.0f*railW()+m.house.panelWidth);}
// The panel's right edge in stage px, and in a column `width` stage px wide the right rail's inner edge where nearer.
float panelRight(const studio::Model& m){return (railW()+m.house.panelWidth)*stackScale(m);}
float innerRight(const studio::Model& m,int width){return std::min(panelRight(m),(float)width-railW()*stackScale(m));}
// The house strip's left screw: a circle of radius 20 at (75, 99) on its plate.
juce::Image screwImage(const studio::Model& m){
 const auto& plate=m.house.asset("strip:plate").image;juce::Image screw(juce::Image::ARGB,40,40,true);
 juce::Graphics g(screw);juce::Path round;round.addEllipse(0.0f,0.0f,40.0f,40.0f);g.reduceClipRegion(round);g.drawImageAt(plate,-55,-79);
 return screw;
}
// A plate from the left rail to `right` stage px, h stage px tall: the PROGRAMMER's face with its edges, and a screw in
// each ear at the rails' holes, two on a 2U plate and one on a 1U strip.
void drawPlate(juce::Graphics& g,const Look& look,const studio::Model& m,const juce::Image& screw,float right,float h,bool twoUnits){
 const auto k=stackScale(m),mm=m.house.unitHeight/unitMm;const auto plate=Rect::leftTopRightBottom(railW()*k,0.0f,right,h);
 look.drawFace(g,plate);
 g.setColour(juce::Colour(0x14ffffff));g.fillRect(plate.withHeight(1.0f));
 g.setColour(juce::Colour(0xff060708));g.fillRect(plate.withTop(h-2.0f));
 const auto rows=twoUnits?std::vector<float>{topRow,bottomRow}:std::vector<float>{middleRow};
 g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
 for(const auto x:{railW()+holeIn*mm,right/k-holeIn*mm})for(const auto row:rows)
  g.drawImageTransformed(screw,juce::AffineTransform::translation(-(float)screw.getWidth()*.5f,-(float)screw.getHeight()*.5f).scaled(k).translated(x*k,row*mm*k));
}
// The house mark: the wordmark's letters, AIR with its R mirrored, without the AI RACK subtitle, which no plate leaves
// room to read.
juce::Path houseMark(){
 auto svg=juce::parseXML(studio::house::houseSkinFile("ai-rack-wordmark.svg").toString());
 if(svg==nullptr)return {};
 for(auto* group=svg->getChildByName("g");group!=nullptr;){auto* next=group->getNextElementWithTagName("g");if(group->hasAttribute("stroke"))svg->removeChildElement(group,true);group=next;}
 const auto text=svg->toString();const auto letters=juce::Drawable::createFromImageData(text.toRawUTF8(),text.getNumBytesAsUTF8());
 auto path=letters!=nullptr?letters->getOutlineAsPath():juce::Path();path.setUsingNonZeroWinding(false);
 return path;
}
// The mark fitted to `area`: printed ivory, or carved as a recess darker than the face with light on its lower edges.
void drawMark(juce::Graphics& g,const juce::Path& mark,Rect area,bool carved){
 if(mark.isEmpty())return;
 const auto fit=mark.getTransformToScaleToFit(area,true,juce::Justification::centredLeft);
 if(!carved){g.setColour(ivory);g.fillPath(mark,fit);return;}
 g.setColour(lip);g.fillPath(mark,fit.translated(0.0f,1.0f));
 g.setColour(recess);g.fillPath(mark,fit);
}
// A 2U house plate's mark, the width of UP, under the identity block's keys.
const Rect plateMark(96.0f,234.0f,84.0f,22.0f);
// A faceplate's OPERATIONS (the maker faceplates spec, decision 5): a small key in the rails' colours, between the
// selection light and the scroll bar, with the PROGRAMMER's lens at its middle, lit while its drawer is open. Its
// focus ring stays inside the key, which the rail leaves no room around.
struct RailKeyLook final:juce::LookAndFeel_V4{
 explicit RailKeyLook(const studio::Model& m):model(m){}
 void drawButtonBackground(juce::Graphics& g,juce::Button& b,const juce::Colour&,bool hover,bool down) override{
  const auto body=b.getLocalBounds().toFloat().reduced(2.0f);
  g.setGradientFill(juce::ColourGradient(juce::Colour(0xff26272b),0.0f,body.getY(),juce::Colour(0xff17181b),0.0f,body.getBottom(),false));
  g.fillRoundedRectangle(body,3.0f);
  g.setColour(juce::Colour(0x18ffffff));g.fillRect(body.reduced(3.0f,0.0f).withHeight(1.0f));
  g.setColour(seam);g.drawRoundedRectangle(body.expanded(.5f),3.5f,1.0f);
  const Pt lens=body.getCentre();
  studio::drawSprite(g,model.asset("programmer:led"),lens,.6f);
  if(b.getToggleState())studio::drawSprite(g,model.asset("programmer:led.lit"),lens,.6f);
  studio::drawFaceStates(g,body,b.isEnabled(),hover,down,false,railFill);
  if(Look::ringed(b)){g.setColour(studio::accent);g.drawRoundedRectangle(b.getLocalBounds().toFloat().reduced(1.0f),4.0f,1.5f);}
 }
 void drawButtonText(juce::Graphics&,juce::TextButton&,bool,bool) override{}
 const studio::Model& model;
};
}
// The units and the space on the rails: the rails behind them, their numbers and the selection light over them.
struct RackStack::Body final:juce::Component{
 explicit Body(RackStack& s):stack(s){}
 RackStack& stack;
 void paint(juce::Graphics&) override;
 void paintOverChildren(juce::Graphics&) override;
};
// The shared master's divider: a 1U blank plate with the console tape across its middle; a label, not a control.
struct RackStack::Divider final:juce::Component{
 static constexpr const char* text="SHARED MASTER - AFFECTS ALL SYNTHS";
 explicit Divider(const studio::Model& m):model(m),look(m),screw(screwImage(m)),tape(m.house,"unit:tape",{studio::house::TapeArt::widthFor(m.house,text,58.0f),58.0f}){
  setComponentID("rack.divider");setTitle(text);setInterceptsMouseClicks(false,false);setWantsKeyboardFocus(false);
 }
 void paint(juce::Graphics& g) override{
  drawPlate(g,look,model,screw,innerRight(model,getWidth()),(float)getHeight(),false);
  const auto k=stackScale(model);juce::Graphics::ScopedSaveState saved(g);g.addTransform(juce::AffineTransform::scale(k));
  tape.paint(g,{(float)getWidth()*.5f/k,(float)getHeight()/k*.5f},-0.8f,text);
 }
 std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override{return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::label);}
 const studio::Model& model;Look look;juce::Image screw;studio::house::TapeArt tape;
};
// A press anywhere in a rack, on its children too, selects it, and may start a drag.
struct RackStack::Presses final:juce::MouseListener{
 explicit Presses(RackStack& s):stack(s){}
 RackStack& stack;
 // While narrow the press opens the rack and folds the one open before; the view moves with the rack, as far as it can,
 // so its top stays under the pointer.
 void mouseDown(const juce::MouseEvent& e) override{
  for(const auto& r:stack.racks)if(r.get()==e.eventComponent||r->isParentOf(e.eventComponent)){
   const auto top=r->getY()-stack.view.getViewPositionY();stack.choose(r->id(),false);
   if(const auto y=r->getY()-top;y!=stack.view.getViewPositionY())stack.view.setViewPosition(0,y);
   stack.press(*r,e);return;
  }
 }
 void mouseDrag(const juce::MouseEvent& e) override{stack.pull(e);}
 void mouseUp(const juce::MouseEvent& e) override{stack.drop(e);}
};
// A press that may become a drag, and the drag once it starts (the moves spec: Dragging).
struct RackStack::Drag{
 juce::Uuid rack;
 // The press's y in the rack, from its top; the pointer's last y in the stack.
 int grab=0,pointer=0;
 bool started=false,fx=false;
 std::vector<juce::Uuid> lifted;
 std::vector<StackDrop> drops;
 // Each drop line's y in the body as the stack lies without the lifted units; the slot's line; its height in units.
 std::vector<int> lines;
 std::size_t line=0;
 int units=0;
 // The lifted units as drawn, and where the copy was last painted.
 juce::Image copy;
 int drawnAt=0;
 juce::Component::SafePointer<juce::Component> focus;
};
// A placement (the moves spec: Its look): a label, 2U or its preview's faceplate's units, that takes no clicks, focus or
// wheel. An empty slot is light grey dashes round the plate's place, over the rails and the gap, with its letters in the
// middle; a preview is a plate with the design's name, its letters and its row, or the design's faceplate composed at
// its values with its letters on a dark band (the maker faceplates spec, decision 8), dimmed to half in a layer, under
// the same dashes.
struct RackStack::Placement final:juce::Component{
 struct Ghost final:juce::Component{
  Ghost(InstrumentProcessor& p,const studio::Model& m):model(m),look(m),screw(screwImage(m)),mark(houseMark()),row(p,m){
   setLookAndFeel(&look);setInterceptsMouseClicks(false,false);setAlpha(.5f);
   Look::setRole(name,Look::Text::title);Look::setRole(place,Look::Text::lines);row.setPlace(RackPanelComponent::Place::preview);
   for(auto* c:std::initializer_list<juce::Component*>{&name,&place,&row})addAndMakeVisible(c);
  }
  ~Ghost() override{setLookAndFeel(nullptr);}
  void paint(juce::Graphics& g) override{
   if(face.isValid()){
    const auto k=stackScale(model);g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImageTransformed(face,juce::AffineTransform::scale(k).translated(railW()*k,0.0f));
    return;
   }
   drawPlate(g,look,model,screw,innerRight(model,getWidth()),(float)getHeight(),true);look.drawEngraving(g,{Rect(390.0f,39.0f,1.0f,197.0f),true});
   drawMark(g,mark,plateMark,false);
  }
  void resized() override{name.setBounds(96,52,284,26);place.setBounds(96,84,284,20);row.setBounds(400,0,1045,getHeight());}
  const studio::Model& model;Look look;juce::Image screw;juce::Path mark;juce::Label name,place;RackPanelComponent row;
  // The design's faceplate, or none for the house plate.
  juce::Image face;
 };
 Placement(InstrumentProcessor& p,const studio::Model& m):processor(p),model(m),look(m),ghost(p,m){
  setComponentID("rack.placement");setInterceptsMouseClicks(false,false);setWantsKeyboardFocus(false);addChildComponent(ghost);
 }
 // A drag's slot is titled with its letters but not lettered, since the lifted copy passes over it.
 void show(const StackPlacement& s,bool lettered=true,const ParameterValues* parameters=nullptr){
  letters=s.letters;spelled=lettered;setTitle(letters);setDescription(s.preview?s.preview->design.name:juce::String());units=2;
  if(s.preview){
   if(!parameters)processor.requestMakerSkin(*s.preview);
   skin=processor.preparedMakerSkin(*s.preview);
   ghost.face=skin!=nullptr?studio::MakerFace::composeFor(*skin,*s.preview,parameters?*parameters:rackPreviewParameters(s.preview->design)):juce::Image();
   if(ghost.face.isValid())units=skin->units;
   ghost.name.setText(s.preview->design.name.toUpperCase(),juce::dontSendNotification);ghost.place.setText(letters,juce::dontSendNotification);ghost.row.showDesign(*s.preview,s.values);
   for(auto* c:std::initializer_list<juce::Component*>{&ghost.name,&ghost.place,&ghost.row})c->setVisible(!ghost.face.isValid());
  }
  ghost.setVisible(s.preview.has_value());repaint();
 }
 // The ghost to the right rail's inner edge, which cuts a faceplate preview and a house-plate preview's row.
 void resized() override{ghost.setBounds(getLocalBounds().withWidth(juce::roundToInt(innerRight(model,getWidth()))));}
 void paintOverChildren(juce::Graphics& g) override{
  const auto k=stackScale(model);juce::Path edge;edge.addRectangle(Rect::leftTopRightBottom(railW()*k,0.0f,innerRight(model,getWidth()),(float)getHeight()).reduced(6.0f));
  Look::drawDashes(g,edge,slotInk,8.0f,6.0f);
  const bool faced=ghost.isVisible()&&ghost.face.isValid();
  if((ghost.isVisible()&&!faced)||!spelled)return;
  const auto w=(float)getWidth();
  const auto text=studio::setText(look.faces,look.titleStyle,letters,0.0f,std::round((float)getHeight()*.5f+look.titleStyle.cap*.5f),w,0.8f,w*.5f).path;
  if(faced){g.setColour(juce::Colour(0xf00b0c0e));g.fillRoundedRectangle(text.getBounds().expanded(24.0f,12.0f),4.0f);}
  g.setColour(slotInk);g.fillPath(text);
 }
 std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override{return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::label);}
 InstrumentProcessor& processor;const studio::Model& model;Look look;Ghost ghost;juce::String letters;bool spelled=true;
 studio::MakerSkinPtr skin;
 // Its height on the rails.
 int units=2;
};
namespace {
// Where the focus goes in a unit when the drawer under it closes or a unit with the focus retires.
juce::Component& keyOf(RackStackUnit& u){return u.isFolded()?static_cast<juce::Component&>(u.fold):u.operations;}
}
// The plate's parts between the rails: clipped at the right rail's inner edge and taking no press left of the left
// rail's; a sideways wheel off the controls shifts them; and while they are shifted the left rail is drawn over them.
// Neither it nor the track is a control or a group of its own.
struct RackStackUnit::Pane final:juce::Component{
 explicit Pane(RackStackUnit& u):unit(u){setComponentID("rack.pane");setMouseClickGrabsKeyboardFocus(false);setExplicitFocusOrder(2);}
 RackStackUnit& unit;
 bool hitTest(int x,int y) override{return x>=juce::roundToInt(railW()*stackScale(unit.model))&&juce::Component::hitTest(x,y);}
 void mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& w) override{
  const auto delta=std::abs(w.deltaX)>std::abs(w.deltaY)?w.deltaX:e.mods.isShiftDown()?w.deltaY:0.0f;
  if(delta!=0.0f&&unit.across()>0&&unit.onAcross)unit.onAcross(-studio::wheelDistance(delta));
  else juce::Component::mouseWheelMove(e,w);
 }
 void paintOverChildren(juce::Graphics& g) override{
  if(unit.shiftPx<=0)return;
  const auto k=stackScale(unit.model),left=railW()*k,h=(float)getHeight();
  g.setColour(railFill);g.fillRect(Rect(0.0f,0.0f,left,h));g.setColour(railEdge);g.fillRect(Rect(left-2.0f*k,0.0f,2.0f*k,h));
 }
 std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override{return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::ignored);}
};
// The plate and its parts where a full-width unit has them, moved left by the unit's shift.
struct RackStackUnit::Track final:juce::Component{
 explicit Track(RackStackUnit& u):unit(u){setMouseClickGrabsKeyboardFocus(false);}
 RackStackUnit& unit;
 void paint(juce::Graphics& g) override{unit.paintPlate(g);}
 std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override{return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::ignored);}
};
RackStackUnit::RackStackUnit(InstrumentProcessor& p,const studio::Model& m,juce::Uuid id,studio::house::Host h,std::shared_ptr<studio::FxAnalysisHub> hub)
 :fold("rack.fold","Fold rack"),up("UP"),down("DOWN"),operations("OPERATIONS"),row(p,m),model(m),look(m),rack(id),bare(bareBankOf(id)),screw(screwImage(m)),mark(houseMark()),keepTape(m.house,"unit:tape",{150.0f,49.0f}),
  processor(p),host(std::move(h)),analysis(std::move(hub)),railKey(std::make_unique<RailKeyLook>(m)),pane(std::make_unique<Pane>(*this)),track(std::make_unique<Track>(*this)){
 // Tab passes through the unit into the stack's order; a press on the plate selects the rack and leaves the focus.
 setLookAndFeel(&look);setFocusContainerType(FocusContainerType::focusContainer);setMouseClickGrabsKeyboardFocus(false);
 fold.setTransform(juce::AffineTransform::scale(stackScale(m)));
 fold.onToggle=[this]{
  const auto was=isFolded();
  // Pressed while the column folds the unit, or worked from the keyboard then: the stack opens it, with its own fold.
  if(std::exchange(foldPress,false)||columnFold){if(onChosen)onChosen();setFolded(false);}
  else setFolded(!folded);
  if(was==isFolded()&&onLayout)onLayout();
 };
 fold.addMouseListener(this,false);
 up.setComponentID("rack.up");down.setComponentID("rack.down");operations.setComponentID("rack.operations");
 Look::setRole(nameLabel,Look::Text::title);Look::setRole(placeLabel,Look::Text::lines);
 nameLabel.setInterceptsMouseClicks(false,false);placeLabel.setInterceptsMouseClicks(false,false);
 row.setPlace(RackPanelComponent::Place::unit);row.showInstance(id);
 int order=1;for(auto* c:std::initializer_list<juce::Component*>{&fold,&up,&down,&operations,&row})c->setExplicitFocusOrder(order++);
 for(auto* c:std::initializer_list<juce::Component*>{&nameLabel,&placeLabel,&up,&down,&operations,&row})track->addAndMakeVisible(c);
 pane->addAndMakeVisible(*track);addAndMakeVisible(*pane);addAndMakeVisible(fold);showParts();
}
RackStackUnit::~RackStackUnit(){retireGraph();operations.setLookAndFeel(nullptr);setLookAndFeel(nullptr);}
juce::Uuid RackStackUnit::id() const{return rack;}
void RackStackUnit::refresh(const SoundDocument& d){
 // A generic rack stands for a one-unit rack on its bank, for its name, LED and lock.
 RackInstance one;const auto* i=instance(d,rack);
 if(i==nullptr&&bare){one.design.name=bareBankName(d,*bare);one.design.units={{0,bankModule(d,bare->role,bare->bank)}};one.bindings={{0,bare->bank}};i=&one;}
 if(i==nullptr)return;
 designName=i->design.name;const auto upper=designName.toUpperCase();
 const bool renamed=upper!=nameLabel.getText()||!nameTape,wasLit=lit,wasLocked=locked;
 if(renamed){
  nameLabel.setText(upper,juce::dontSendNotification);
  tapeWidth=std::max(180.0f,studio::house::TapeArt::widthFor(model.house,upper,54.0f));nameTape.emplace(model.house,"unit:tape",Pt(tapeWidth,54.0f));
 }
 placeLabel.setText(bare?bareBankPlace(d,*bare):rackPlaceText(d,*i),juce::dontSendNotification);
 setTitle(designName);header=bare?designName+", no rack":rackHeaderText(d,*i);
 fold.setTitle((isFolded()?"Unfold ":"Fold ")+designName);
 up.setTitle("Move "+designName+" up");down.setTitle("Move "+designName+" down");operations.setTitle("Operations on "+designName);
 const bool canUp=rackStep(d,rack,true).has_value(),canDown=rackStep(d,rack,false).has_value();
 if(canUp)up.setEnabled(true);
 if(canDown)down.setEnabled(true);
 if(!canUp&&up.hasKeyboardFocus(false))(canDown?static_cast<juce::Component&>(down):fold).grabKeyboardFocus();
 if(!canDown&&down.hasKeyboardFocus(false))(canUp?static_cast<juce::Component&>(up):fold).grabKeyboardFocus();
 up.setEnabled(canUp);down.setEnabled(canDown);
 lit=rackOn(d,*i);locked=rackLocked(d,*i);
 if(!bare)processor.requestMakerSkin(*i);
 const auto skin=bare?studio::MakerSkinPtr{}:processor.preparedMakerSkin(*i);const auto height=units();
 if(maker&&(&maker->skin!=skin.get()||bound!=i->bindings))dropFace();
 if(!maker&&skin!=nullptr)makeFace(skin,*i);
 if(maker){maker->refresh(d);maker->showProposal(candidate);}
 const auto native=studio::panelUnits(*i);
 const bool dynamic=native.size()==1&&native.front().module==NativeModule::dynamicEq&&!bare;
 const bool space=native.size()==1&&native.front().module==NativeModule::spaceReverb&&!bare;
 if(!space&&spaceGraphView){
  if(spaceGraphView->hasKeyboardFocus(true))fold.grabKeyboardFocus();
  spaceGraphView->retire();if(auto* parent=spaceGraphView->getParentComponent())parent->removeChildComponent(spaceGraphView.get());juce::MessageManager::callAsync([retired=std::shared_ptr<studio::SpaceReverbGraph>(spaceGraphView.release())]{});
 }
 if(space){
  if(!spaceGraphView){spaceGraphView=std::make_unique<studio::SpaceReverbGraph>(processor,host);track->addChildComponent(*spaceGraphView);
   spaceGraphView->onBandSelected=[this](studio::SpaceReverbGraph::Mode mode,std::size_t band){if(onSpaceBandSelected)onSpaceBandSelected(mode==studio::SpaceReverbGraph::Mode::postEq,band);};spaceGraphView->onEdited=[this]{if(onEdited)onEdited();};resized();}
  spaceGraphView->refresh(d,native.front().bank);spaceGraphView->showProposal(candidate);
 }
 if(!dynamic&&graph){
  if(graph->hasKeyboardFocus(true))fold.grabKeyboardFocus();
  graph->retire();track->removeChildComponent(graph.get());detail::retireUi(processor,[retired=std::shared_ptr<studio::DynamicEqGraph>(graph.release())]{});
 }
 if(dynamic){
  if(!graph){graph=std::make_unique<studio::DynamicEqGraph>(processor,host,studio::DynamicEqGraph::Clipboard{},analysis);track->addChildComponent(*graph);
   graph->onBandSelected=[this](std::size_t band){if(onBandSelected)onBandSelected(band);};graph->onEdited=[this]{if(onEdited)onEdited();};graph->onOverview=[this]{if(onOverview)onOverview();};graph->onMatch=[this]{if(onMatch)onMatch();};graph->onSpectral=[this]{if(onSpectral)onSpectral();};resized();}
  graph->refresh(d,native.front().bank);graph->showProposal(candidate);
 }
 if(maker)maker->setRevealParameters(spaceGraphView&&maker->skin.nativeEditor?spaceGraphView->parameters():std::vector<std::size_t>{});
 showParts();
 row.refresh(d);
 if(const auto u=studio::panelUnits(*i);u!=reachUnits){reachUnits=u;reach.clear();for(const auto& v:u)for(const auto p:unitReach(v.module,v.bank,v.revision))reach.push_back(p);}
 const auto onRow=spaceGraphView?spaceGraphView->parameters():graph?graph->parameters():row.parameters(),onFace=maker?maker->parameters():std::vector<std::size_t>{};
 const auto on=[](const std::vector<std::size_t>& v,std::size_t p){return std::find(v.begin(),v.end(),p)!=v.end();};
 drawerOnly.clear();liveBindings=i->bindings;
 for(const auto p:reach)if(!d.controlLocks[p]&&!on(onRow,p)&&!on(onFace,p))drawerOnly.emplace_back(p,d.values[p]);
 rehide();describe();
 if(renamed||lit!=wasLit||locked!=wasLocked)repaint();
 if(units()!=height&&onLayout)onLayout();
}
void RackStackUnit::describe(){
 const auto made=maker?header+"\n"+maker->skin.maker+", "+maker->skin.model:header;
 const bool panel=maker?maker->proposesPanel():row.proposesPanel();
 setDescription((panel?made+"\nAI proposes a new panel":made)+(hidden?"\nAI proposes changes in the drawer":""));
 const bool was=marked;marked=panel||hidden||!(maker?maker->marks().empty():row.marks().empty())||(graph&&graph->proposed())||(spaceGraphView&&spaceGraphView->proposed());
 if(marked!=was)repaint();
}
void RackStackUnit::rehide(){
 bool kept=false;
 if(candidate&&bare){const auto order=rackOrder(*candidate);kept=std::any_of(order.begin(),order.end(),[this](const StackRack& s){return s.id==rack;});}
 else if(candidate){const auto* i=instance(*candidate,rack);kept=i!=nullptr&&i->bindings==liveBindings;}
 const bool now=kept&&std::any_of(drawerOnly.begin(),drawerOnly.end(),[this](const auto& p){return std::abs(candidate->values[p.first]-p.second)>1e-6f;});
 if(now!=hidden){hidden=now;repaint();}
}
void RackStackUnit::makeFace(studio::MakerSkinPtr skin,const RackInstance& i){
 maker=studio::MakerFace::forRack(processor,skin,i,host);if(!maker)return;
 bound=i.bindings;
 maker->setHostLook(model.asset("programmer:lock"),1.0f/stackScale(model),model.house);
 maker->setTransform(juce::AffineTransform::scale(stackScale(model)));
 maker->setExplicitFocusOrder(5);operations.setExplicitFocusOrder(6);operations.setLookAndFeel(railKey.get());
 maker->onOpen=[this]{layoutSpaceGraph();if(onLayout)onLayout();if(maker&&maker->isOpen()&&onOpened)onOpened();};
 maker->onDrumSource=[this](DrumPadTarget target,juce::Component* from){const auto frame=host.frame?host.frame():nullptr;if(host.closing&&host.closing())return;if(onDrumSource)onDrumSource(target,from);};
 maker->onDrumSourceClose=[this]{if(onDrumSourceClose)onDrumSourceClose();};
 maker->onSetup=[this]{if(onSetup)onSetup();};
 if(isFolded())maker->setFolded(true);
 row.showProposal(nullptr);
 // A faceplate's OPERATIONS stays on the right rail as the plate shifts.
 track->addChildComponent(*maker);addAndMakeVisible(operations);showParts();resized();repaint();
}
void RackStackUnit::dropFace(){
 // Protect the active retirement call; only inert controls survive in the message queue.
 const auto frame=host.frame?host.frame():nullptr;
 const bool hadFocus=maker->hasKeyboardFocus(true);
 maker->suspendInputs();maker->retire();
 if(spaceGraphView){spaceGraphView->suspend();track->addChildComponent(*spaceGraphView);}
 if(hadFocus)fold.grabKeyboardFocus();
 maker->onOpen=nullptr;maker->onDrumSource=nullptr;maker->onDrumSourceClose=nullptr;track->removeChildComponent(maker.get());detail::retireUi(processor,[retired=std::shared_ptr<studio::MakerFace>(maker.release())]{});
 bound.clear();operations.setExplicitFocusOrder(4);operations.setLookAndFeel(nullptr);track->addAndMakeVisible(operations);
 row.showProposal(candidate);
 showParts();resized();repaint();
}
void RackStackUnit::showParts(){
 const auto f=isFolded();
 for(auto* c:std::initializer_list<juce::Component*>{&nameLabel,&placeLabel})c->setVisible(!f&&!maker);
 for(auto* c:std::initializer_list<juce::Component*>{&up,&down})c->setVisible(!f&&!maker&&!bare);
 row.setVisible(!f&&!maker&&!bare&&!graph&&!spaceGraphView);
 if(graph)graph->setVisible(!f&&!maker&&!bare);
 layoutSpaceGraph();
 operations.setVisible(!f);
 if(maker)maker->setVisible(!f||maker->hasStrip());
}
void RackStackUnit::layoutSpaceGraph(){
 if(!spaceGraphView)return;
 const auto* editor=maker&&maker->skin.nativeEditor?&*maker->skin.nativeEditor:nullptr;
 auto& parent=editor?static_cast<juce::Component&>(maker->revealed()):static_cast<juce::Component&>(*track);
 const bool visible=!isFolded()&&!bare&&(!maker||(editor&&maker->isOpen()&&!maker->isMoving()));
 if(!visible)spaceGraphView->suspend();
 if(spaceGraphView->getParentComponent()!=&parent)parent.addChildComponent(*spaceGraphView);
 spaceGraphView->setMakerStyle(editor!=nullptr);
 spaceGraphView->setBounds(editor?editor->rect.toNearestInt():juce::Rectangle<int>(400,0,1045,getHeight()));
 spaceGraphView->resized();
 spaceGraphView->setVisible(visible);
}
void RackStackUnit::setFolded(bool f){
 if(f==folded)return;
 const auto was=isFolded();folded=f;showFold(was,false);
 if(was!=isFolded()&&onLayout)onLayout();
}
void RackStackUnit::setColumnFolded(bool f){
 if(f==columnFold)return;
 const auto was=isFolded();columnFold=f;showFold(was,true);
}
void RackStackUnit::showFold(bool was,bool byColumn){
 const auto f=isFolded();
 if(f!=was){
  if(f){
   if(maker)maker->suspendInputs();
   if(spaceGraphView)spaceGraphView->suspend();
   auto* focused=juce::Component::getCurrentlyFocusedComponent();
   if(focused!=nullptr&&focused!=&fold&&isParentOf(focused)){if(byColumn)juce::Component::unfocusAllComponents();else fold.grabKeyboardFocus();}
  }
  if(maker&&maker->hasStrip())maker->setFolded(f);
  else if(f&&maker)maker->setOpen(false,false);
  shiftPx=0;track->setTopLeftPosition(0,0);
 }
 fold.folded=f;fold.setTitle((f?"Unfold ":"Fold ")+designName);fold.repaint();
 showParts();
 repaint();
}
bool RackStackUnit::columnFolded() const{return columnFold;}
bool RackStackUnit::foldedByUser() const{return folded;}
bool RackStackUnit::isFolded() const{return folded||columnFold;}
void RackStackUnit::setShift(int px){
 px=juce::jlimit(0,across(),px);if(px==shiftPx)return;
 shiftPx=px;track->setTopLeftPosition(-shiftPx,0);repaint();
}
int RackStackUnit::shift() const{return shiftPx;}
int RackStackUnit::across() const{return isFolded()||getWidth()<=0?0:std::max(0,juce::roundToInt(panelRight(model))-pane->getWidth());}
juce::Component& RackStackUnit::plate(){return *track;}
int RackStackUnit::units() const{return isFolded()?1:maker?maker->units():2;}
float RackStackUnit::shownUnits() const{return isFolded()?1.0f:maker?maker->shownUnits():2.0f;}
juce::String RackStackUnit::name() const{return nameLabel.getText();}
juce::String RackStackUnit::place() const{return placeLabel.getText();}
bool RackStackUnit::on() const{return lit;}
bool RackStackUnit::kept() const{return locked;}
void RackStackUnit::setProposal(std::shared_ptr<const SoundDocument> d){
 candidate=std::move(d);
 if(maker)maker->showProposal(candidate);else row.showProposal(candidate);
 if(graph)graph->showProposal(candidate);
 if(spaceGraphView)spaceGraphView->showProposal(candidate);
 rehide();describe();
}
bool RackStackUnit::proposed() const{return marked;}
bool RackStackUnit::hiddenProposal() const{return hidden;}
studio::MakerFace* RackStackUnit::face() const{return maker.get();}
studio::DynamicEqGraph* RackStackUnit::eqGraph() const{return graph.get();}
studio::SpaceReverbGraph* RackStackUnit::spaceGraph() const{return spaceGraphView.get();}
void RackStackUnit::retireGraph(){if(maker)maker->suspendInputs();if(graph)graph->retire();if(spaceGraphView)spaceGraphView->retire();}
void RackStackUnit::paintPlate(juce::Graphics& g){
 const auto k=stackScale(model),h=(float)getHeight();const auto f=isFolded();
 if(maker&&(!f||maker->hasStrip()))return;
 const auto right=f?innerRight(model,getWidth()):panelRight(model);
 drawPlate(g,look,model,screw,right,h,!f);
 if(!f){look.drawEngraving(g,{Rect(390.0f,39.0f,1.0f,197.0f),true});drawMark(g,mark,plateMark,false);return;}
 // The fallback strip: the name's tape from 194 design px, and the LED 40 past it, on the strip's middle.
 if(nameTape){juce::Graphics::ScopedSaveState saved(g);g.addTransform(juce::AffineTransform::scale(k));nameTape->paint(g,{194.0f+tapeWidth*.5f,h/k*.5f},-1.5f,nameLabel.getText());}
 const Pt led((194.0f+tapeWidth+40.0f)*k,h*.5f);
 studio::drawSprite(g,model.asset("programmer:led"),led);
 if(lit)studio::drawSprite(g,model.asset("programmer:led.lit"),led);
 // The house's mark carved 85 design px wide, ending as far from the strip's right end as the tape starts from its
 // left (150 design px), where it stays 80 design px clear of the LED; a maker's rack carries none.
 const auto end=right-150.0f*k,start=end-85.0f*k;
 if(!maker&&start>=led.x+80.0f*k)drawMark(g,mark,Rect::leftTopRightBottom(start,h*.5f-11.0f*k,end,h*.5f+11.0f*k),true);
}
void RackStackUnit::paintOverChildren(juce::Graphics& g){
 const auto k=stackScale(model);
 // Folded, the row's marks or outline become dashes round the name's tape; a faceplate's strip outlines its own display.
 if(isFolded()&&marked&&!(maker&&maker->hasStrip())){juce::Path edge;edge.addRectangle(Rect(194.0f*k,(float)getHeight()*.5f-27.0f*k,tapeWidth*k,54.0f*k).expanded(6.0f));Look::drawDashes(g,edge,markInk,6.0f,4.0f);}
 // A change only the drawer shows: dashes 3 px round OPERATIONS while the drawer is closed, under the rail as the plate shifts.
 if(hidden&&operations.isVisible()&&!operations.getToggleState()){
  juce::Graphics::ScopedSaveState saved(g);
  if(operations.getParentComponent()==track.get())g.reduceClipRegion(pane->getBounds());
  juce::Path edge;edge.addRectangle(getLocalArea(&operations,operations.getLocalBounds()).toFloat().expanded(3.0f));Look::drawDashes(g,edge,markInk,6.0f,4.0f);
 }
 if(!locked)return;
 juce::Graphics::ScopedSaveState saved(g);
 if(shiftPx>0)g.reduceClipRegion(pane->getBounds().withLeft(juce::roundToInt(railW()*k)));
 g.addTransform(juce::AffineTransform::scale(k).translated(-(float)shiftPx,0.0f));
 keepTape.paint(g,{112.0f,43.0f},-8.0f,"AI: KEEP",false,juce::Colour(0xffe9c440));
}
void RackStackUnit::resized(){
 const int h=getHeight(),inner=std::max(0,juce::roundToInt(innerRight(model,getWidth())));
 fold.setBounds(0,0,studio::HouseUnit::railW,juce::roundToInt((float)h/stackScale(model)));
 pane->setBounds(0,0,inner,h);
 shiftPx=std::min(shiftPx,across());
 track->setBounds(-shiftPx,0,std::max(getWidth(),(int)model.layout["stage"][0]),h);
 nameLabel.setBounds(96,52,284,26);placeLabel.setBounds(96,84,284,20);
 up.setBounds(96,122,84,96);down.setBounds(196,122,84,96);
 if(!isFolded()){row.setBounds(400,0,1045,h);if(graph)graph->setBounds(400,0,1045,h);}
 layoutSpaceGraph();
 if(!maker){operations.setBounds(296,122,84,96);return;}
 // OPERATIONS beside the unit's top rail number, as the triangle on the left rail, clear of the scroll bar's 8 px.
 maker->setBounds(studio::HouseUnit::railW,0,maker->getWidth(),maker->getHeight());
 const int left=inner+4;
 operations.setBounds(left,4,std::max(0,getWidth()-9-left),44);
}
void RackStackUnit::mouseDown(const juce::MouseEvent& e){if(e.eventComponent==&fold)foldPress=columnFold;}
void RackStackUnit::mouseUp(const juce::MouseEvent& e){if(e.eventComponent==&fold)foldPress=false;}
std::unique_ptr<juce::AccessibilityHandler> RackStackUnit::createAccessibilityHandler(){return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::group);}
RackStack::RackStack(InstrumentProcessor& p,const studio::Model& m,studio::HouseUnit& h,studio::EmptyRackSpace& s,RackPanelComponent& d,studio::house::Host host,std::shared_ptr<studio::FxAnalysisHub> hub)
 :processor(p),model(m),house(h),space(s),drawer(d),unitHost(std::move(host)),privateAnalysisClock(hub?nullptr:std::make_unique<studio::FxAnalysisClock>(p.fxMonitorHandle())),analysis(hub?std::move(hub):privateAnalysisClock->hub),look(m),content(std::make_unique<Body>(*this)),divider(std::make_unique<Divider>(m)),presses(std::make_unique<Presses>(*this)){
 if(privateAnalysisClock)privateAnalysisClock->onUpdated=[this]{refreshAnalysis();};
 house.setTransform(juce::AffineTransform::scale(scale()));house.setTopLeftPosition(0,0);house.onLayout=[this]{layout();};house.setExplicitFocusOrder(1);
 space.setExplicitFocusOrder(1000);
 drawer.setPlace(RackPanelComponent::Place::drawer);drawer.setComponentID("rack.drawer");
 drawer.onApplied=[this](const juce::String& status,std::optional<juce::Uuid> placed){
  if(onEdited)onEdited();
  if(placed)select(*placed);
  if(drawn.isNull()&&onReport)onReport(status);
 };
 studio::ProgrammerHost controlsHost;controlsHost.house=unitHost;controlsHost.refresh=[this]{if(onEdited)onEdited();};
 controlsHost.snapshot=[this](SoundDocument& d,std::uint64_t& edit){if(processor.trySoundSnapshot(d,edit))return true;if(onReport)onReport("Sound is busy; retry the edit");return false;};
 panel=std::make_unique<studio::AllControlsPanel>(p,m,std::move(controlsHost));
 panel->onPageChanged=[this]{updateDetector();};
 panel->onBandSelected=[this](std::size_t slot,std::size_t band){if(auto* u=unit(drawn);u&&u->eqGraph())if(const auto bound=u->eqGraph()->boundSlot();bound&&*bound==slot)u->eqGraph()->selectBand(band);};
 panel->onSpaceBandSelected=[this](std::size_t slot,bool post,std::size_t band){if(auto* u=unit(drawn);u&&u->spaceGraph())if(const auto bound=u->spaceGraph()->boundSlot();bound&&*bound==slot){u->spaceGraph()->setMode(post?studio::SpaceReverbGraph::Mode::postEq:studio::SpaceReverbGraph::Mode::decay);u->spaceGraph()->selectBand(band);}};
 panel->onHeight=[this]{arrange(nullptr);};drawer.onHeight=[this]{arrange(nullptr);};
 panel->onPatchBay=[this](bool open){drawer.setDetailsOpen(open);};
 drawer.onDetailsChanged=[this](bool open){panel->setPatchBayOpen(open);if(!open&&!panel->focusPatchBay())if(auto* u=unit(drawn))u->operations.grabKeyboardFocus();};
 content->setMouseClickGrabsKeyboardFocus(false);
 content->addAndMakeVisible(house);content->addChildComponent(*divider);content->addAndMakeVisible(space);content->addChildComponent(*panel);content->addChildComponent(drawer);
 view.setComponentID("rack.stack");view.setViewedComponent(content.get(),false);view.setScrollBarsShown(false,false,true,false);view.setWantsKeyboardFocus(false);
 view.onMoved=[this]{syncBar();if(dragging())follow();};addAndMakeVisible(view);
 bar.setLookAndFeel(&look);bar.setAutoHide(false);bar.addListener(this);addChildComponent(bar);
 // The focus scrolls the open unit, so its bar takes none, and a press on it leaves the focus where it was.
 across.setComponentID("rack.sideways");across.setLookAndFeel(&look);across.setAutoHide(false);across.setWantsKeyboardFocus(false);
 across.setMouseClickGrabsKeyboardFocus(false);across.addListener(this);content->addChildComponent(across);
 house.onAcross=[this](int px){house.setShift(house.shift()+px);syncAcross();};
 house.onOpened=[this]{keepAcross(house.editor,house.editor.getLocalBounds());};
 overview=std::make_unique<studio::EqOverview>(processor,model,analysis);addChildComponent(*overview);
 overview->onClose=[this]{closeEqOverview();};
 match=std::make_unique<studio::EqMatchPanel>(processor,model,analysis);addChildComponent(*match);
 match->onClose=[this]{closeEqMatch();};
 overview->onNavigate=[this](std::uint8_t slot){
  closeEqOverview();for(const auto& rack:racks){const auto bare=bareBankOf(rack->id());const auto bound=rack->eqGraph()?rack->eqGraph()->boundSlot():std::nullopt;
   if(bound==slot||(bare&&bare->role==RackRole::fx&&bare->bank==slot)){rack->setFolded(false);select(rack->id());layout();reveal(rack->id());keyOf(*rack).grabKeyboardFocus();break;}}
 };
 juce::Desktop::getInstance().addFocusChangeListener(this);
}
RackStack::~RackStack(){
 retireGraphs();
 juce::Desktop::getInstance().removeFocusChangeListener(this);bar.removeListener(this);bar.setLookAndFeel(nullptr);
 across.removeListener(this);across.setLookAndFeel(nullptr);
 house.onLayout=nullptr;house.onAcross=nullptr;house.onOpened=nullptr;drawer.onApplied=nullptr;drawer.onHeight=nullptr;drawer.onDetailsChanged=nullptr;panel->onHeight=nullptr;panel->onPatchBay=nullptr;panel->onPageChanged=nullptr;
}
void RackStack::refresh(const SoundDocument& d){
 RackInventoryPtr inventory;processor.captureAvailableRackInventory(inventory);
 const auto before=skinInstances(shown),after=skinInstances(d);if(!std::equal(before.begin(),before.end(),after.begin(),after.end(),sameSkin))processor.prepareRackSkins({},after);if(placed&&placed->preview)processor.requestMakerSkin(*placed->preview);
 shown=d;auto next=rackOrder(d);
 const bool reordered=!std::equal(next.begin(),next.end(),order.begin(),order.end(),[](const StackRack& a,const StackRack& b){return a.id==b.id&&a.role==b.role&&a.chain==b.chain;});
 order=std::move(next);
 std::vector<std::unique_ptr<RackStackUnit>> kept;
 for(const auto& r:order){
  const auto same=std::find_if(racks.begin(),racks.end(),[&](const auto& u){return u!=nullptr&&u->id()==r.id;});
  if(same!=racks.end()){kept.push_back(std::move(*same));continue;}
  auto made=std::make_unique<RackStackUnit>(processor,model,r.id,unitHost,analysis);const auto id=r.id;
  made->setProposal(proposal);made->onLayout=[this]{settle();};
  made->onOpened=[this,id]{
   auto* u=unit(id);auto* f=u!=nullptr?u->face():nullptr;if(f==nullptr)return;
   const auto opened=f->openedArea();
   const auto area=opened.isEmpty()?f->revealed().getBounds():opened;
   keepInView(content->getLocalArea(f,area));keepAcross(*f,area);
  };
  made->onSetup=[this,id]{openDrawer(id);if(drawn==id){const auto knobs=panel->shownKnobs();if(!knobs.empty())knobs.front()->grabKeyboardFocus();}};
  made->onChosen=[this,id]{choose(id,false);};
  made->onAcross=[this,id](int px){if(auto* u=unit(id)){u->setShift(u->shift()+px);syncAcross();}};
  made->onEdited=[this]{if(onEdited)onEdited();};
  made->onDrumSource=[this](DrumPadTarget target,juce::Component* from){if(onDrumSource)onDrumSource(target,from);};
  made->onDrumSourceClose=[this,id]{if(onDrumSourceClose)onDrumSourceClose(id);};
  made->onOverview=[this,id]{if(auto* rack=unit(id);rack&&rack->eqGraph())if(const auto slot=rack->eqGraph()->boundSlot())openEqOverview(*slot);};
  made->onMatch=[this,id]{if(auto* rack=unit(id);rack&&rack->eqGraph())if(const auto slot=rack->eqGraph()->boundSlot())openEqMatch(*slot);};
  made->onSpectral=[this,id]{openDrawer(id);if(auto* u=unit(id);u&&u->eqGraph())if(const auto slot=u->eqGraph()->boundSlot())panel->showParameter(dynamicEqSpectralParameterStart+std::size_t(*slot)*25+1+u->eqGraph()->selectedBand()*3);};
  made->onBandSelected=[this,id](std::size_t band){openDrawer(id);if(auto* u=unit(id);u&&u->eqGraph()){const auto slot=u->eqGraph()->boundSlot();if(slot)panel->showParameter(panel->spectralPage()?dynamicEqSpectralParameterStart+std::size_t(*slot)*25+1+band*3:panel->detectorPage()?dynamicEqDetectorParameterStart+std::size_t(*slot)*40+band*5+3:dynamicEqParameterStart+std::size_t(*slot)*dynamicEqParametersPerSlot+band*dynamicEqParametersPerBand+3);}};
  made->onSpaceBandSelected=[this,id](bool post,std::size_t band){openDrawer(id);if(auto* u=unit(id);u&&u->spaceGraph()){const auto slot=u->spaceGraph()->boundSlot();if(slot)panel->showParameter(spaceReverbParameterStart+std::size_t(*slot)*spaceReverbParametersPerSlot+(post?48+band*8:12+band*6)+3);}};
  made->operations.onClick=[this,id]{
   // An EQ page owns its full header; the existing rail key exposes the saturation rack's service form there.
   if(drawn==id&&drawer.drawerHeight()==0&&!panel->patchBayVisible())drawer.setDetailsOpen(true);
   else if(drawn==id)closeDrawer();else openDrawer(id);
  };
  made->up.onClick=[this,id]{move(id,true);};made->down.onClick=[this,id]{move(id,false);};
  made->addMouseListener(presses.get(),true);content->addAndMakeVisible(*made);kept.push_back(std::move(made));
 }
 // A retired unit leaves the body now and is deleted on the next turn, so none of its own callbacks runs on it deleted;
 // its row retires now. Its focus goes, rather than to the body's first control, to the selected rack once the selection
 // settles.
 bool lost=false;
 for(auto& gone:racks)if(gone!=nullptr){
  const auto frame=unitHost.frame?unitHost.frame():nullptr;
  if(gone->hasKeyboardFocus(true)){lost=true;juce::Component::unfocusAllComponents();}
  gone->row.retire();if(auto* face=gone->face())face->retire();gone->onSetup=nullptr;gone->retireGraph();gone->onBandSelected=nullptr;gone->onSpaceBandSelected=nullptr;gone->onEdited=nullptr;gone->onOverview=nullptr;gone->onMatch=nullptr;gone->onSpectral=nullptr;gone->onDrumSource=nullptr;gone->onDrumSourceClose=nullptr;
  gone->removeMouseListener(presses.get());gone->onLayout=gone->onOpened=gone->onChosen=nullptr;gone->onAcross=nullptr;
  gone->up.onClick=gone->down.onClick=gone->operations.onClick=nullptr;
  content->removeChildComponent(gone.get());detail::retireUi(processor,[retired=std::shared_ptr<RackStackUnit>(gone.release())]{});
 }
 racks=std::move(kept);
 for(std::size_t i=0,n=0;i<racks.size();++i){
  const auto b=bareBankOf(racks[i]->id());
  racks[i]->setComponentID(b?juce::String(b->role==RackRole::synth?"rack.bank.synth.":"rack.bank.fx.")+juce::String(b->bank+1):"rack.unit."+juce::String((int)n++));
  racks[i]->setExplicitFocusOrder(2+2*(int)i);racks[i]->refresh(d);
 }
 // A sound that moves the racks, or retires the grabbed one, ends the drag with no move.
 if(drag&&(reordered||unit(drag->rack)==nullptr))endDrag();
 if(!drawn.isNull()&&unit(drawn)==nullptr)closeDrawer();
 bool ghostSkinChanged=false;
 if(proposal)for(auto& ghost:proposalGhosts)if(processor.preparedMakerSkin(*ghost.placement.preview)!=ghost.view->skin){
  ghost.view->show(ghost.placement,true,&proposal->values);ghostSkinChanged=true;
 }
 if(reordered||ghostSkinChanged||(placed&&rackSpot(d,placed->destination)!=spot))layout();
 if(unit(chosen)==nullptr){const auto first=std::find_if(order.begin(),order.end(),[](const StackRack& s){return !s.bare;});select(first==order.end()?juce::Uuid::null():first->id);}
 if(!drawn.isNull())panel->show(drawnUnits(d,drawn));
 panel->refresh(d);
 if(overview&&overview->isVisible())overview->refresh(d);
 if(match&&match->isVisible())match->refresh(d);
 updateDetector();
 drawer.refresh(d);
 panel->setPatchBayOpen(!drawer.detailsCollapsed());
 if(auto* u=unit(chosen);lost&&u!=nullptr&&u->isShowing())keyOf(*u).grabKeyboardFocus();
}
void RackStack::setStripOnly(bool s){if(s){endDrag();closeEqOverview();closeEqMatch();}stripOnly=s;layout();}
float RackStack::scale() const{return stackScale(model);}
float RackStack::unitHeight() const{return model.house.unitHeight*scale();}
std::vector<std::pair<int,float>> RackStack::railNumbers() const{
 std::vector<std::pair<int,float>> numbers;
 for(int n=1;n<=stackUnits+spaceUnits;++n)numbers.emplace_back(n,((float)n-.5f)*unitHeight());
 return numbers;
}
std::vector<RackStackUnit*> RackStack::units() const{std::vector<RackStackUnit*> all;for(const auto& r:racks)all.push_back(r.get());return all;}
RackStackUnit* RackStack::unit(juce::Uuid id) const{for(const auto& r:racks)if(r->id()==id)return r.get();return nullptr;}
void RackStack::select(juce::Uuid id){choose(id,true);}
// A press selects without moving the view, so the key under the pointer stays there for the release. While narrow the
// selection opens the rack, and closes the house unit's 3U unit even when the rack was already selected.
void RackStack::choose(juce::Uuid id,bool show){
 if(!id.isNull()&&unit(id)==nullptr)return;
 const bool closing=!id.isNull()&&house.isOpen();
 if(id==chosen&&!closing)return;
 if(id!=chosen){
  chosen=id;
  if(!drawn.isNull()&&drawn!=id)closeDrawer();
  drawer.showInstance(id);bindDrawer();content->repaint();
  if(onSelectionChanged)onSelectionChanged(bareBankOf(id)?juce::Uuid::null():id);
 }
 if(closing)house.setOpen(false);
 if(narrow())layout();
 if(show)reveal(id);
}
juce::Uuid RackStack::selected() const{return chosen;}
void RackStack::openDrawer(juce::Uuid id){
 // A rack the column folds opens as it is selected.
 auto* u=unit(id);if(u==nullptr||u->foldedByUser())return;
 select(id);
 if(drawn==id||u->isFolded())return;
 if(!drawn.isNull())closeDrawer();
 drawn=id;u->operations.setToggleState(true,juce::dontSendNotification);u->repaint();
 panel->show(drawnUnits(shown,id));
 panel->refresh(shown);
 layout();reveal(id);
}
void RackStack::closeDrawer(){
 if(drawn.isNull())return;
 if(auto* u=unit(drawn)){if(auto* graph=u->eqGraph())graph->setDetectorAuditionMode(false);u->operations.setToggleState(false,juce::dontSendNotification);u->repaint();}
 drawn=juce::Uuid::null();
 if(auto* u=unit(chosen);u!=nullptr&&u->isShowing()&&(drawer.hasKeyboardFocus(true)||panel->hasKeyboardFocus(true)))keyOf(*u).grabKeyboardFocus();
 panel->show({});
 layout();
}
void RackStack::updateDetector(){
 auto* u=unit(drawn);auto* graph=u?u->eqGraph():nullptr;if(!graph)return;
 graph->setDetectorAuditionMode(panel->detectorPage()&&panel->isShowing());
 if(const auto slot=graph->boundSlot();slot&&panel->detectorPage()){
  const auto& result=graph->spectrum();const auto token=graph->subscription();const auto monitor=processor.fxMonitorHandle();
  const bool valid=graph->detectorAuditionMode()&&token&&result.detectorReadbacksValid&&result.generation==graph->analysisHub()->generation(*token);
  panel->setDetectorReadbacks(*slot,result.detectorReadbacks,valid,monitor->externalAvailable());
 }
}
juce::Uuid RackStack::drawerRack() const{return drawn;}
studio::AllControlsPanel& RackStack::controls(){return *panel;}
void RackStack::reveal(juce::Uuid id){
 auto* u=unit(id);if(u==nullptr||!u->isVisible())return;
 auto span=u->getBounds();if(id==drawn&&drawer.isVisible())span=span.getUnion(drawer.getBounds());
 keepInView(span);
}
void RackStack::retireGraphs(){if(!skinsRetired){skinsRetired=true;processor.prepareRackSkins({},{});}panel->retireGraph();if(match)match->retire();if(overview)overview->retire();for(auto& rack:racks)rack->retireGraph();if(privateAnalysisClock)privateAnalysisClock->retire();}
void RackStack::refreshAnalysis(){for(auto& rack:racks)if(auto* graph=rack->eqGraph())graph->updateMonitoring();if(overview&&overview->isVisible())overview->updateMonitoring();if(match&&match->isVisible())match->updateMonitoring();updateDetector();}
void RackStack::openEqOverview(std::uint8_t slot){
 const auto frame=unitHost.frame?unitHost.frame():nullptr;if(unitHost.closing&&unitHost.closing())return;
 if(!overview||stripOnly||slot>=8||shown.layout.effects[slot].use!=SlotUse::present||shown.layout.effects[slot].kind!=EffectKind::dynamicEq)return;
 closeEqMatch();
 endDrag();overviewScroll=view.getViewPosition();overviewOpener=juce::Component::getCurrentlyFocusedComponent();
 view.setVisible(false);bar.setVisible(false);for(auto& rack:racks)if(auto* graph=rack->eqGraph())graph->suspend();
 if(unitHost.closing&&unitHost.closing())return;
 overview->setVisible(true);overview->refresh(shown);(void)overview->setReferenceSlot(slot);(void)overview->setSendSlot(slot);overview->toFront(true);
}
void RackStack::closeEqOverview(){
 if(!overview||!overview->isVisible())return;overview->setVisible(false);view.setVisible(true);view.setViewPosition(overviewScroll);syncBar();syncAcross();
 auto focus=overviewOpener;overviewOpener=nullptr;if(focus&&focus->isShowing()&&focus->isEnabled())focus->grabKeyboardFocus();
}
void RackStack::openEqMatch(std::uint8_t slot){
 const auto frame=unitHost.frame?unitHost.frame():nullptr;if(unitHost.closing&&unitHost.closing())return;
 if(!match||stripOnly||slot>=8||shown.layout.effects[slot].use!=SlotUse::present||shown.layout.effects[slot].kind!=EffectKind::dynamicEq)return;
 juce::Component::SafePointer<studio::DynamicEqGraph> graph;for(const auto& rack:racks)if(rack->eqGraph()&&rack->eqGraph()->boundSlot()==slot){graph=rack->eqGraph();break;}if(!graph)return;
 closeEqOverview();closeEqMatch();endDrag();matchScroll=view.getViewPosition();matchOpener=juce::Component::getCurrentlyFocusedComponent();
 view.setVisible(false);bar.setVisible(false);for(auto& rack:racks)if(auto* g=rack->eqGraph())g->suspend();
 if(unitHost.closing&&unitHost.closing())return;
 if(!graph||graph->boundSlot()!=slot){view.setVisible(true);syncBar();return;}
 match->bind(*graph);match->setVisible(true);match->refresh(shown);match->toFront(true);match->grabKeyboardFocus();
}
void RackStack::closeEqMatch(){
 if(!match||!match->isVisible())return;match->setVisible(false);view.setVisible(true);view.setViewPosition(matchScroll);syncBar();syncAcross();
 auto focus=matchOpener;matchOpener=nullptr;if(focus&&focus->isShowing()&&focus->isEnabled())focus->grabKeyboardFocus();
}
bool RackStack::deferEditorDestruction(std::shared_ptr<void> editor){
 bool deferred=false;for(const auto& r:racks)deferred=r->row.deferEditorDestruction(editor)||deferred;
 return deferred;
}
void RackStack::setPlacement(std::optional<StackPlacement> p){
 if(p==placed)return;
 const auto was=slot&&slot->isVisible()?spot:std::nullopt;
 const auto* before=placed&&placed->preview?&*placed->preview:nullptr;const auto* after=p&&p->preview?&*p->preview:nullptr;
 const bool changed=bool(before)!=bool(after)||(before&&after&&!sameSkin(*before,*after));
 placed=std::move(p);
 if(changed){processor.prepareRackSkins({},skinInstances(shown));if(placed&&placed->preview)processor.requestMakerSkin(*placed->preview);}
 if(!placed){if(slot){content->removeChildComponent(slot.get());slot.reset();}layout();return;}
 if(!slot){slot=std::make_unique<Placement>(processor,model);content->addChildComponent(*slot);}
 slot->show(*placed);layout();
 if(slot->isVisible()&&spot!=was)keepInView(slot->getBounds());
}
juce::Component* RackStack::placement() const{
 const auto& showing=dragging()?dragSlot:slot;
 return showing&&showing->isVisible()?showing.get():nullptr;
}
void RackStack::setProposal(const SoundDocument* d){
 if(d==nullptr?proposal==nullptr:proposal!=nullptr&&*proposal==*d)return;
 const bool hadGhosts=!proposalGhosts.empty();
 proposalGhosts.clear();proposalOrder.clear();
 proposal=d!=nullptr?std::make_shared<const SoundDocument>(*d):nullptr;
 for(const auto& r:racks)r->setProposal(proposal);
 panel->showProposal(proposal);
 if(proposal){
  proposalOrder=rackOrder(*proposal);
  for(const auto& entry:proposalOrder){
   if(unit(entry.id)!=nullptr)continue;
   const auto* rack=instance(*proposal,entry.id);if(!rack)continue;
   if(proposalGhosts.empty())processor.prepareRackSkins({},skinInstances(*proposal));
   RackDestination destination{entry.role,entry.role==RackRole::synth?masterChain:entry.chain,0};
   if(entry.role==RackRole::synth)destination.position=std::uint8_t(std::find(proposal->layout.synthOrder.begin(),proposal->layout.synthOrder.begin()+proposal->layout.synthCount,entry.bank)-proposal->layout.synthOrder.begin());
   else{const auto& chain=proposal->layout.effectOrder[entry.chain];destination.position=std::uint8_t(std::find(chain.begin(),chain.begin()+proposal->layout.effectCount[entry.chain],entry.bank)-chain.begin());}
   StackPlacement placement{destination,"PREVIEW: "+rackPlaceText(*proposal,*rack),*rack,rackInstanceValues(*proposal,*rack)};
   auto ghost=std::make_unique<Placement>(processor,model);ghost->setComponentID("rack.proposal."+entry.id.toString());
   ghost->show(placement,true,&proposal->values);content->addAndMakeVisible(*ghost);
   proposalGhosts.push_back({entry.id,std::move(placement),std::move(ghost)});
  }
  // Removed racks remain inspectable; they receive no proposed control marks.
  for(const auto& entry:order)if(std::none_of(proposalOrder.begin(),proposalOrder.end(),[&](const auto& next){return next.id==entry.id;}))proposalOrder.push_back(entry);
 }
 if(hadGhosts&&proposalGhosts.empty())processor.prepareRackSkins({},skinInstances(shown));
 layout();
 if(!proposalGhosts.empty())keepInView(proposalGhosts.front().view->getBounds());
}
std::vector<std::pair<int,std::optional<RackDestination>>> RackStack::dropLines() const{
 std::vector<std::pair<int,std::optional<RackDestination>>> all;
 if(dragging())for(std::size_t k=0;k<drag->drops.size();++k)all.emplace_back(drag->lines[k],drag->drops[k].destination);
 return all;
}
bool RackStack::dragging() const{return drag!=nullptr&&drag->started;}
bool RackStack::keyPressed(const juce::KeyPress& k){
 if(match&&match->isVisible()&&k.isKeyCode(juce::KeyPress::escapeKey)){closeEqMatch();return true;}
 if(overview&&overview->isVisible()&&k.isKeyCode(juce::KeyPress::escapeKey)){closeEqOverview();return true;}
 if(!dragging()||!k.isKeyCode(juce::KeyPress::escapeKey))return false;
 endDrag();return true;
}
// At d px from the view's top or bottom, within 40, the view scrolls ceil(24 * (40 - d) / 40) px a tick that way; the
// view's move opens the slot at the line it brings nearest the copy.
void RackStack::timerCallback(){
 if(!dragging()){stopTimer();return;}
 const int fromTop=drag->pointer,fromFoot=view.getHeight()-drag->pointer;
 const auto speed=[](int d){return (int)std::ceil(24.0*(40-std::max(0,d))/40.0);};
 const int step=fromTop<40?-speed(fromTop):fromFoot<40?speed(fromFoot):0;
 if(step!=0)view.setViewPosition(0,view.getViewPositionY()+step);
}
void RackStack::press(RackStackUnit& u,const juce::MouseEvent& e){
 endDrag();
 if(bareBankOf(u.id()))return;
 for(auto* c=e.eventComponent;c!=nullptr&&c!=&u;c=c->getParentComponent())
  if(dynamic_cast<studio::SpaceReverbGraph*>(c)||dynamic_cast<studio::DynamicEqGraph*>(c)||dynamic_cast<juce::Button*>(c)||dynamic_cast<juce::Slider*>(c)||dynamic_cast<juce::ComboBox*>(c)||dynamic_cast<studio::house::SkinButton*>(c)||dynamic_cast<studio::house::Lever*>(c)||dynamic_cast<studio::house::FoldRail*>(c))return;
 drag=std::make_unique<Drag>();drag->rack=u.id();
 drag->grab=juce::roundToInt(e.getEventRelativeTo(&u).position.y);drag->pointer=getLocalPoint(nullptr,e.getScreenPosition()).y;
}
void RackStack::pull(const juce::MouseEvent& e){
 if(!drag)return;
 drag->pointer=getLocalPoint(nullptr,e.getScreenPosition()).y;
 if(!drag->started){if(e.getEventRelativeTo(this).getDistanceFromDragStart()<=6)return;lift();if(!dragging())return;}
 follow();
}
void RackStack::lift(){
 auto* u=unit(drag->rack);const auto* i=instance(shown,drag->rack);
 drag->lifted=rackGroup(shown,drag->rack);drag->drops=rackDrops(shown,drag->rack);
 if(u==nullptr||i==nullptr||drag->drops.empty()){drag.reset();return;}
 if(!drawn.isNull())closeDrawer();
 const auto top=u->getY();int bottom=top;
 for(const auto id:drag->lifted)if(auto* r=unit(id)){bottom=std::max(bottom,r->getBottom());drag->units+=r->units();}
 drag->copy=juce::Image(juce::Image::ARGB,content->getWidth(),std::max(1,bottom-top),true);
 {juce::Graphics g(drag->copy);for(const auto id:drag->lifted)if(auto* r=unit(id))g.drawImageAt(r->createComponentSnapshot(r->getLocalBounds()),0,r->getY()-top);}
 drag->fx=i->design.role==RackRole::fx;drag->started=true;
 drag->lines.assign(drag->drops.size(),0);arrange(&drag->lines);
 drag->line=drag->drops.size();drag->drawnAt=copyTop();
 drag->focus=juce::Component::getCurrentlyFocusedComponent();
 setWantsKeyboardFocus(true);grabKeyboardFocus();
 if(!dragSlot){dragSlot=std::make_unique<Placement>(processor,model);content->addChildComponent(*dragSlot);}
 startTimer(30);
}
// The slot opens at the line nearest the copy's top (decision 3), so it cannot flicker between two lines.
void RackStack::follow(){
 const auto top=copyTop();std::size_t best=0;
 for(std::size_t k=1;k<drag->lines.size();++k)if(std::abs(top-drag->lines[k])<std::abs(top-drag->lines[best]))best=k;
 if(best!=drag->line){
  drag->line=best;const auto& t=drag->drops[best].destination;
  dragSlot->show(StackPlacement{t.value_or(RackDestination{}),t?rackDestinationText(*t):juce::String(),{},{}},false);
  layout();
 }
 content->repaint(0,std::min(top,drag->drawnAt),content->getWidth(),drag->copy.getHeight()+std::abs(top-drag->drawnAt));drag->drawnAt=top;
}
int RackStack::copyTop() const{return view.getViewPositionY()+drag->pointer-drag->grab;}
void RackStack::drop(const juce::MouseEvent& e){
 if(!drag)return;
 if(!drag->started){drag.reset();return;}
 const auto rack=drag->rack;const auto t=drag->line<drag->drops.size()?drag->drops[drag->line].destination:std::nullopt;
 const bool inside=getLocalBounds().contains(getLocalPoint(nullptr,e.getScreenPosition()));
 endDrag();
 auto* u=unit(rack);if(!inside||!t||u==nullptr)return;
 const auto name=u->getTitle();SoundDocument d;std::uint64_t edit=0;
 if(!processor.trySoundSnapshot(d,edit)){if(onReport)onReport("Sound is busy; retry the edit");return;}
 RackOperation op;op.kind=RackOperationKind::move;op.selected=rack;op.destination=*t;
 const auto status=processor.applyRackOperation(op,edit);
 if(onReport)onReport(rackDropSentence(name,*t,status));
 if(status==RackCandidateStatus::ready&&onEdited)onEdited();
 reveal(rack);
}
void RackStack::endDrag(){
 if(!drag)return;
 const bool started=drag->started;const auto focus=drag->focus;drag.reset();
 if(!started)return;
 stopTimer();
 layout();
 setWantsKeyboardFocus(false);
 if(focus!=nullptr&&focus->isShowing())focus->grabKeyboardFocus();
 else if(hasKeyboardFocus(false))giveAwayKeyboardFocus();
}
juce::Viewport& RackStack::viewport(){return view;}
void RackStack::View::mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& w){
 const auto at=getViewPosition();juce::Viewport::mouseWheelMove(e,w);if(getViewPosition()!=at)studio::noteRackScroll();
}
juce::Component& RackStack::body(){return *content;}
juce::ScrollBar& RackStack::scrollBar(){return bar;}
juce::ScrollBar& RackStack::sideways(){return across;}
bool RackStack::narrow() const{return getWidth()>0&&getWidth()<(int)model.layout["stage"][0];}
void RackStack::openHouse(bool o){house.setOpen(o&&narrow());layout();}
bool RackStack::houseOpen() const{return house.isOpen();}
juce::Component* RackStack::openUnit() const{
 if(!narrow()||stripOnly)return nullptr;
 if(house.isOpen())return &house;
 auto* u=unit(chosen);return u!=nullptr&&!u->isFolded()?u:nullptr;
}
void RackStack::syncAcross(){
 auto* open=openUnit();
 const int left=juce::roundToInt(railW()*scale()),right=content->getWidth()-left;
 int range=0,shown=0,start=0;juce::String name;
 if(open==&house){range=house.unit.getWidth();shown=house.narrowWidth();start=house.shift();name="AI RACK SYNTH";}
 else if(auto* u=dynamic_cast<RackStackUnit*>(open)){range=juce::roundToInt(panelRight(model))-left;shown=range-u->across();start=u->shift();name=u->name();}
 const bool show=open!=nullptr&&open->isVisible()&&range>shown;
 across.setVisible(show);
 if(!show)return;
 const auto foot=content->getLocalArea(open,open->getLocalBounds()).getBottom();
 across.setBounds(left+8,foot-12,right-left-16,8);
 across.setRangeLimits(0.0,(double)range,juce::dontSendNotification);across.setCurrentRange((double)start,(double)shown,juce::dontSendNotification);
 across.setTitle("Scroll "+name+" sideways");across.toFront(false);
}
void RackStack::keepAcross(juce::Component& c,juce::Rectangle<int> area){
 auto* open=openUnit();if(open==nullptr)return;
 // Where the part of the plate in view starts, from `from` and `width` px wide, moved the least that shows x to x + w.
 const auto least=[](int from,int width,int x,int w){return x<from||w>width?x:x+w>from+width?x+w-width:from;};
 if(open==&house){
  if(&c!=&house.unit&&!house.unit.isParentOf(&c))return;
  const auto a=house.unit.getLocalArea(&c,area);
  house.setShift(least(house.shift(),house.narrowWidth(),a.getX(),a.getWidth()));
 }
 else if(auto* u=dynamic_cast<RackStackUnit*>(open)){
  auto& plate=u->plate();if(&c!=&plate&&!plate.isParentOf(&c))return;
  const auto a=plate.getLocalArea(&c,area);const int left=juce::roundToInt(railW()*scale());
  const int width=juce::roundToInt(panelRight(model))-left-u->across();
  u->setShift(least(u->shift()+left,width,a.getX(),a.getWidth())-left);
 }
 syncAcross();
}
void RackStack::resized(){view.setBounds(getLocalBounds());bar.setBounds(getWidth()-8,0,8,getHeight());if(getWidth()>0&&getHeight()>0){for(juce::Component* panel:{static_cast<juce::Component*>(overview.get()),static_cast<juce::Component*>(match.get())})if(panel){panel->setBounds(0,0,1080,526);panel->setTransform(juce::AffineTransform::scale(float(getWidth())/1080,float(getHeight())/526));}}layout();}
void RackStack::settle(){
 if(auto* u=unit(drawn);!drawn.isNull()&&(u==nullptr||u->isFolded()))closeDrawer();
 else layout();
}
void RackStack::move(juce::Uuid id,bool up){
 auto* u=unit(id);if(u==nullptr)return;
 const auto name=u->getTitle();SoundDocument d;std::uint64_t edit=0;
 if(!processor.trySoundSnapshot(d,edit)){if(onReport)onReport("Sound is busy; retry the edit");return;}
 const auto step=rackStep(d,id,up);if(!step)return;
 const auto status=processor.applyRackOperation(*step,edit);
 if(onReport)onReport(rackMoveSentence(name,up,status));
 if(status==RackCandidateStatus::ready&&onEdited)onEdited();
 reveal(id);
}
void RackStack::bindDrawer(){SoundDocument d;std::uint64_t edit=0;if(processor.trySoundSnapshot(d,edit))drawer.refresh(d);}
void RackStack::keepInView(juce::Rectangle<int> area){
 const int top=view.getViewPositionY(),height=view.getViewHeight();
 if(height<=0||(area.getY()>=top&&area.getBottom()<=top+height))return;
 view.setViewPosition(0,area.getHeight()>height||area.getY()<top?area.getY():area.getBottom()-height);
}
void RackStack::syncBar(){
 const int height=content->getHeight(),shown=view.getViewHeight();
 bar.setRangeLimits(0.0,(double)juce::jmax(1,height),juce::dontSendNotification);
 bar.setCurrentRange((double)view.getViewPositionY(),(double)shown,juce::dontSendNotification);
 bar.setVisible(height>shown&&!(overview&&overview->isVisible())&&!(match&&match->isVisible()));
}
// The focus entering a rack selects it; a press's focus leaves the view to the press. Any focus in the body comes into view.
// The focus a window gets back, on the component it left, moves nothing.
void RackStack::globalFocusChanged(juce::Component* c){
 if(c==nullptr)return;
 const bool back=c==focusWas.getComponent();focusWas=c;
 if(back||!content->isParentOf(c))return;
 const bool pressed=juce::Desktop::getInstance().getMainMouseSource().isDragging();
 for(const auto& r:racks)if(r->isParentOf(c)){choose(r->id(),!pressed);break;}
 if(!pressed){keepInView(content->getLocalArea(c,c->getLocalBounds()));keepAcross(*c,c->getLocalBounds());}
}
void RackStack::scrollBarMoved(juce::ScrollBar* b,double start){
 if(b!=&across){view.setViewPosition(0,juce::roundToInt(start));return;}
 if(auto* open=openUnit();open==&house)house.setShift(juce::roundToInt(start));
 else if(auto* u=dynamic_cast<RackStackUnit*>(open))u->setShift(juce::roundToInt(start));
}
// Unit tops at whole multiples of the pitch, rounded: the house unit, the racks with the divider before the first master FX
// rack and the open drawer under its rack, between the rails, in whole units, a moving faceplate's drawn fraction of its
// units until it settles; then the space, at least 1U and the rest of the view. The placement goes before the unit at its spot, after the drawer above it, and under the divider, which it
// brings, when it is on the shared master. While a drag lasts, the lifted units hide, the divider shows for an FX rack,
// and the drag's slot, as tall as they are, takes the placement's place at its line. The strip-only view hides the
// drawer and keeps it open.
void RackStack::layout(){if(fitting)return;fit();arrange(nullptr);}
void RackStack::fit(){
 const juce::ScopedValueSetter<bool> busy(fitting,true);
 const bool thin=narrow();
 const int right=getWidth()-juce::roundToInt(railW()*scale());
 house.setNarrow(thin?(int)std::floor((float)right/scale())-studio::HouseUnit::railW:0);
 auto* focused=juce::Component::getCurrentlyFocusedComponent();
 const bool inside=focused!=nullptr&&content->isParentOf(focused);
 for(const auto& r:racks)r->setColumnFolded(thin&&(house.isOpen()||r->id()!=chosen));
 if(auto* d=unit(drawn);!drawn.isNull()&&(d==nullptr||d->isFolded()))closeDrawer();
 if(!inside||juce::Component::getCurrentlyFocusedComponent()!=nullptr)return;
 if(house.isOpen())house.rail.grabKeyboardFocus();
 else if(auto* u=unit(chosen);u!=nullptr&&u->isShowing())keyOf(*u).grabKeyboardFocus();
}
void RackStack::arrange(std::vector<int>* lines){
 const auto u=unitHeight();const auto px=[u](float n){return juce::roundToInt(n*u);};
 const auto width=getWidth()>0?getWidth():(int)model.layout["stage"][0];
 const int left=juce::roundToInt(railW()*scale()),right=width-left;
 // The panel at the stage's column width and its page's height, scaled down with a narrower column; the form under it.
 const float panelScale=std::min(1.0f,(float)(right-left)/1485.0f);
 const int panelHeight=juce::roundToInt((float)panel->shownHeight()*panelScale);
 // Collapsed controls use measured height; the full form keeps whole rack units.
 const float drawerSize=(float)(panelHeight+drawer.drawerHeight())/u;
 const float drawerUnits=drawer.detailsCollapsed()?drawerSize:std::ceil(drawerSize);
 const bool apply=lines==nullptr,lifting=dragging();
 spot=placed?rackSpot(shown,placed->destination):std::nullopt;
 Placement* opened=nullptr;std::optional<StackSpot> at;int slotUnits=2;
 if(lifting){if(apply&&drag->line<drag->drops.size()){opened=dragSlot.get();at=drag->drops[drag->line].spot;slotUnits=drag->units;}}
 else if(slot){opened=slot.get();at=spot;slotUnits=slot->units;}
 float n=house.showsStrip()?1.0f:3.0f;bool divided=false,slotted=false;
 const auto divide=[&]{if(divided)return;if(apply)divider->setBounds(0,px(n),width,px(n+1)-px(n));n+=1.0f;divided=true;};
 // The line before the unit at j in the order without the lifted units, above or under the divider.
 const auto line=[&](std::size_t j,bool master){
  if(lines)for(std::size_t k=0;k<drag->drops.size();++k)if(drag->drops[k].spot==StackSpot{j,master})(*lines)[k]=px(n);
  if(!opened||!at||slotted||at->before!=j||at->master!=master)return;
  if(master)divide();
  if(apply)opened->setBounds(0,px(n),width,px(n+slotUnits)-px(n));
  n+=slotUnits;slotted=true;
 };
 std::size_t j=0;
 const auto& displayed=proposal&&!lifting&&!placed?proposalOrder:order;
 if(apply)for(auto& ghost:proposalGhosts)ghost.view->setVisible(false);
 for(std::size_t i=0;i<displayed.size();++i){
  const auto& entry=displayed[i];
  if(proposal&&!lifting&&!placed){
   const auto ghost=std::find_if(proposalGhosts.begin(),proposalGhosts.end(),[&](const auto& g){return g.id==entry.id;});
   if(ghost!=proposalGhosts.end()){
    if(entry.role==RackRole::fx&&entry.chain==masterChain)divide();
    const auto units=ghost->view->units;
    if(apply){ghost->view->setBounds(0,px(n),width,px(n+units)-px(n));ghost->view->setVisible(!stripOnly);}
    n+=units;++j;continue;
   }
  }
  auto* current=unit(entry.id);if(!current)continue;auto& r=*current;
  if(lifting&&std::find(drag->lifted.begin(),drag->lifted.end(),r.id())!=drag->lifted.end()){if(apply)r.setVisible(false);continue;}
  line(j,false);
  if(entry.role==RackRole::fx&&entry.chain==masterChain)divide();
  line(j,true);
  if(apply){r.setBounds(0,px(n),width,px(n+r.shownUnits())-px(n));r.setVisible(!stripOnly);}
  n+=r.shownUnits();
  if(r.id()==drawn){
   if(apply){
    const int top=px(n),bottom=px(n+drawerUnits);
    panel->setSize(juce::roundToInt((float)(right-left)/panelScale),panel->shownHeight());
    panel->setTransform(juce::AffineTransform::scale(panelScale).translated((float)left,(float)top));
    drawer.setBounds(left,top+panelHeight,right-left,bottom-top-panelHeight);
    // One face over the panel and the form, so the plate runs on under the panel without a step in tone.
    panel->setFace(Rect(0.0f,0.0f,(float)panel->getWidth(),(float)(bottom-top)/panelScale));
    drawer.setFace(Rect(0.0f,(float)-panelHeight,(float)(right-left),(float)(bottom-top)));
    panel->setExplicitFocusOrder(3+2*(int)i);drawer.setExplicitFocusOrder(3+2*(int)i);
   }
   n+=drawerUnits;
  }
  ++j;
 }
 line(j,false);
 if(lifting&&drag->fx)divide();
 line(j,true);
 if(!apply)return;
 drawer.setVisible(!drawn.isNull()&&!stripOnly);
 panel->setVisible(!drawn.isNull()&&!stripOnly);
 divider->setVisible(divided&&!stripOnly);
 if(slot)slot->setVisible(slotted&&opened==slot.get()&&!stripOnly);
 if(dragSlot)dragSlot->setVisible(slotted&&opened==dragSlot.get()&&!stripOnly);
 const auto top=px(n),h=std::max(px(n+1)-top,view.getHeight()-top);
 space.setRack(scale(),(float)top);space.setBounds(0,top,right,h);space.setVisible(!stripOnly);
 stackUnits=stripOnly?1:(int)std::ceil(n-0.001f);spaceUnits=stripOnly?0:(int)std::floor((float)h/u+0.02f);
 content->setSize(width,stripOnly?px(1):top+h);content->repaint();
 syncBar();syncAcross();
}
void RackStack::Body::paint(juce::Graphics& g){
 const auto k=stack.scale(),left=railW()*k,right=innerRight(stack.model,getWidth()),w=(float)getWidth(),h=(float)getHeight();
 g.fillAll(seam);
 g.setColour(railFill);g.fillRect(Rect(0.0f,0.0f,left,h));g.fillRect(Rect::leftTopRightBottom(right,0.0f,w,h));
 g.setColour(railEdge);g.fillRect(Rect(left-2.0f*k,0.0f,2.0f*k,h));g.fillRect(Rect(right,0.0f,2.0f*k,h));
}
// The selection light beside the selected rack and its open drawer, or while a drag lasts beside its slot; the rails'
// numbers; then the lifted copy at 60%, under the pointer.
void RackStack::Body::paintOverChildren(juce::Graphics& g){
 const auto k=stack.scale(),left=railW()*k,right=innerRight(stack.model,getWidth()),w=(float)getWidth();
 // The line 3 px wide on each rail's inner edge, a fringe on each side of it, and a halo fading 6 px out over the rail and
 // 12 px in over the plate (rack/overview-r2.png). A maker rack's OPERATIONS key on the rail keeps its colours.
 const auto glow=[&](float top,float length){
  juce::Graphics::ScopedSaveState saved(g);
  if(auto* r=stack.unit(stack.chosen);r!=nullptr&&r->operations.isShowing())g.excludeClipRegion(getLocalArea(&r->operations,r->operations.getLocalBounds()));
  const auto fade=[&](float from,float to){
   juce::ColourGradient falling(halo.withAlpha(.75f),from,0.0f,halo.withAlpha(0.0f),to,0.0f,false);
   falling.addColour(.35,halo.withAlpha(.35f));
   g.setGradientFill(falling);
   g.fillRect(Rect(std::min(from,to),top,std::abs(to-from),length));
  };
  fade(left-4.0f,left-10.0f);fade(left+1.0f,left+13.0f);fade(right+4.0f,right+10.0f);fade(right-1.0f,right-13.0f);
  g.setColour(fringe);for(const auto x:{left-4.0f,left,right-1.0f,right+3.0f})g.fillRect(Rect(x,top,1.0f,length));
  g.setColour(light);g.fillRect(Rect(left-3.0f,top,3.0f,length));g.fillRect(Rect(right,top,3.0f,length));
 };
 const bool lifting=stack.dragging();
 if(lifting){if(stack.dragSlot&&stack.dragSlot->isVisible())glow((float)stack.dragSlot->getY(),(float)stack.dragSlot->getHeight());}
 else if(auto* r=stack.unit(stack.chosen);r!=nullptr&&r->isVisible())
  glow((float)r->getY(),(float)((stack.drawn==stack.chosen&&stack.drawer.isVisible()?stack.drawer.getBottom():r->getBottom())-r->getY()));
 g.setColour(numberInk);g.setFont(juce::Font(juce::FontOptions(18.0f*k,juce::Font::bold)));
 for(const auto& [n,y]:stack.railNumbers()){
  g.drawText(juce::String(n),Rect(0.0f,y-12.0f,left-2.0f*k,24.0f),juce::Justification::centred);
  g.drawText(juce::String(n),Rect(right+2.0f*k,y-12.0f,w-right-2.0f*k,24.0f),juce::Justification::centred);
 }
 if(lifting){g.setOpacity(.6f);g.drawImageAt(stack.drag->copy,0,stack.copyTop());}
}
}
