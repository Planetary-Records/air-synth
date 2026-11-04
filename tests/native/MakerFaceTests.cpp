#include "FrozenRackExamples.h"
#include "PreparedMakerSkins.h"
#include "MakerFace.h"
#include "UserReach.h"
#include "AllControlsPanel.h"
#include "SaturationGraph.h"
#include "SoundState.h"
#include "DotMatrix.h"
#include "Motion.h"
#include "HouseControls.h"
#include "HouseSkin.h"
#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "ParameterGesture.h"
#include "RackDesign.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include "StudioSkin.h"
#include "WheelLatch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <tuple>
#if JUCE_WINDOWS
#include <objbase.h>
// Windows RPC's small macro collides with the Small Hours test variables.
#undef small
#endif
using namespace airack;
namespace house=airack::studio::house;
// Windows' own reading of its Animation effects setting, which reduced motion follows.
extern "C" __declspec(dllimport) int __stdcall SystemParametersInfoW(unsigned int,unsigned int,void*,unsigned int);
namespace {
int checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;} } while(false)
const house::Element* element(const house::Model& m,const juce::String& id){for(const auto& e:m.elements)if(e.id==id)return &e;return nullptr;}
const house::Group* group(const house::Model& m,const juce::String& id){for(const auto& g:m.groups)if(g.id==id)return &g;return nullptr;}
// A control's proportion as its slider gives it: logarithmic or linear over the element's range.
double proportion(const house::Element& e,double v){const auto lo=e.range.getStart(),hi=e.range.getEnd();return e.logarithmic?std::log(v/lo)/std::log(hi/lo):(v-lo)/(hi-lo);}
float angleAt(const house::Element& e,double v){return house::pointerAngleFor(e,proportion(e,v));}
float tickAngle(const house::Element& e,double v){for(const auto& [value,angle]:e.ticks)if(value==v)return angle;CHECK(!"no such tick");return 0;}
void maker_face_for(){
 std::cout<<"GROUP maker_face_for"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& thread=examples[2];
 const auto skin=studio::makerFaceFor(thread);CHECK(skin!=nullptr);
 if(skin){CHECK(skin->design==thread.id&&skin->maker=="MARLOW & FINCH"&&skin->model=="PROGRAMME COMPRESSOR TYPE TP-2"&&skin->units==2&&skin->skin.elements.size()==16);}
 // What the faceplate does not draw may differ: the units' values, the intent and the revision.
 {auto d=thread;d.units[0].defaults[0].normalized=.1f;d.units[1].defaults.clear();d.intent="Another intent";d.revision=9;CHECK(studio::makerFaceFor(d)==nullptr);}
 // What it draws may not: its identity, name, role, units and modules, sections and focused controls.
 const auto differs=[&](auto edit){auto d=thread;edit(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](RackDesign& d){d.id=juce::Uuid();}));
 CHECK(differs([](RackDesign& d){d.name="Thread Press II";}));
 CHECK(differs([](RackDesign& d){d.role=RackRole::synth;}));
 CHECK(differs([](RackDesign& d){d.units[1].module=NativeModule::delay;}));
 CHECK(differs([](RackDesign& d){d.units[1].id=2;}));
 CHECK(differs([](RackDesign& d){d.units.pop_back();}));
 CHECK(differs([](RackDesign& d){d.panel.sections[1]="Colour";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].label="Hold";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[3].section=0;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[2].key="compressor.makeup_db";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].widget=FocusWidget::rotary;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].minimum=-48;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[3].maximum=24;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[1].response=FocusResponse::linear;}));
 CHECK(differs([](RackDesign& d){d.panel.controls.pop_back();}));
 {auto other=examples[1];other.id=juce::Uuid();CHECK(studio::makerFaceFor(other)==nullptr);}
 {auto fresh=thread;fresh.id=juce::Uuid();fresh.name="Fresh";CHECK(studio::makerFaceFor(fresh)==nullptr);}
 // The skin's files by their paths in elements.json; another skin's or a missing file is empty.
 CHECK(airack::test::frozenMakerSkinFile("thread-press","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("thread-press","plates/thread-press-face.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("ember-reed","plates/thread-press-face.png").isEmpty()&&airack::test::frozenMakerSkinFile("thread-press","plates/face.png").isEmpty());
}
void maker_skin_loads(){
 std::cout<<"GROUP maker_skin_loads"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& thread=examples[2];
 const auto skin=studio::makerFaceFor(thread);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.elements.size()==16&&m.designId==thread.id&&m.maker=="MARLOW & FINCH"&&m.modelName=="PROGRAMME COMPRESSOR TYPE TP-2"&&m.units==2);
 CHECK(m.panelWidth==2140.0f&&std::abs(m.unitHeight-197.11f)<.01f);
 const auto* face=group(m,"face.face");const auto* setup=group(m,"face.setup");CHECK(face&&setup);
 if(face)CHECK(face->focusOrder==juce::StringArray({"face.grip","face.front","face.blend","face.density","face.in","face.setup"}));
 if(setup)CHECK(setup->focusOrder==juce::StringArray({"face.ratio","face.release","face.makeup","face.shape","face.tone","face.mix","face.drive-in","face.close"}));
 int bound=0,knobs=0,levers=0,lamps=0;
 for(const auto& e:m.elements){
  CHECK(e.id.startsWith("face.")&&(e.view=="face"||e.view=="setup"));
  if(e.kind=="rotary")++knobs;else if(e.kind=="lever")++levers;else if(e.kind=="lamp")++lamps;
  if(e.kind=="rotary"||e.kind=="lever"||e.kind=="lamp"){
   ++bound;CHECK(e.rackUnit==0||e.rackUnit==1);CHECK(e.paramId.isEmpty());
   const auto unit=std::find_if(thread.units.begin(),thread.units.end(),[&](const RackUnit& u){return u.id==e.rackUnit;});CHECK(unit!=thread.units.end());
   if(unit!=thread.units.end()){CHECK(rackParameterActive(unit->module,e.key));CHECK(rackParameterIndex(unit->module,0,e.key).has_value());}
  }
  if(e.kind=="rotary"){CHECK(e.ticks.size()>=2&&e.hasDot);CHECK(e.drawnValue>=e.range.getStart()&&e.drawnValue<=e.range.getEnd());}
  if(e.kind=="lever"){CHECK(e.positions.size()==2&&e.hasDot);CHECK(std::any_of(e.positions.begin(),e.positions.end(),[&](const auto& p){return p.value==e.drawnValue;}));for(const auto& p:e.positions)CHECK(m.assets.count(p.asset)==1&&p.legend.isNotEmpty());}
 }
 CHECK(bound==13&&knobs==9&&levers==3&&lamps==1);
 if(const auto* in=element(m,"face.in"))CHECK(in->key=="bypassed"&&in->positions[0].value==0&&in->positions[0].legend=="IN"&&in->positions[0].asset=="face:in.up"&&in->positions[1].legend=="OUT"&&in->positions[1].asset=="face:in.down");
 if(const auto* lamp=element(m,"face.lamp"))CHECK(lamp->key=="bypassed"&&lamp->rackUnit==0&&lamp->litValue==0&&lamp->lensR>20&&lamp->layer("lit")=="face:lamp.lit");
 if(const auto* shape=element(m,"face.shape"))CHECK(shape->rackUnit==1&&shape->key=="drive.shape"&&shape->choices==juce::StringArray({"Tanh","Soft sign"}));
 if(const auto* ratio=element(m,"face.ratio"))CHECK(ratio->view=="setup"&&!ratio->hitRect.isEmpty()&&ratio->logarithmic&&ratio->range==juce::Range<double>(1.5,20));
 if(const auto* grip=element(m,"face.grip"))CHECK(grip->key=="compressor.threshold_db"&&!grip->logarithmic&&grip->range==juce::Range<double>(-36,0)&&grip->defaultValue==-18&&grip->unit=="dB");
 CHECK(m.asset("face:plate").image.getWidth()==2140&&m.asset("face:plate").image.getHeight()==394&&m.asset("setup:plate").image.isValid());
}
void maker_angles(){
 std::cout<<"GROUP maker_angles"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();
 const auto skin=studio::makerFaceFor(examples[2]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 for(const auto& e:m.elements){
  if(e.kind!="rotary")continue;
  for(std::size_t i=0;i<e.ticks.size();++i){
   CHECK(std::abs(angleAt(e,e.ticks[i].first)-e.ticks[i].second)<.01f);
   if(i>0)CHECK(e.ticks[i].first>e.ticks[i-1].first&&e.ticks[i].second>e.ticks[i-1].second);
  }
  // Pinned at the end ticks, and rising between them.
  CHECK(house::pointerAngleFor(e,-.2)==e.ticks.front().second&&house::pointerAngleFor(e,1.2)==e.ticks.back().second);
  float last=-1000;for(int k=0;k<=20;++k){const auto a=house::pointerAngleFor(e,k/20.0);CHECK(a>=last);last=a;}
 }
 if(const auto* grip=element(m,"face.grip"))CHECK(std::abs(angleAt(*grip,-18)-tickAngle(*grip,-18))<.01f&&angleAt(*grip,-21)>tickAngle(*grip,-24)&&angleAt(*grip,-21)<tickAngle(*grip,-18));
 if(const auto* front=element(m,"face.front")){const auto a=angleAt(*front,15),b=angleAt(*front,std::sqrt(15.0));CHECK(std::abs(a-tickAngle(*front,15))<.01f);CHECK(std::abs(b-.5f*(tickAngle(*front,1)+tickAngle(*front,15)))<.01f);}
 if(const auto* ratio=element(m,"face.ratio")){const auto a=angleAt(*ratio,2.5);CHECK(a>tickAngle(*ratio,2)&&a<tickAngle(*ratio,3));}
}
//------------------------------------------------------------------------------ Ember Reed's skin
const juce::StringArray emberDeck{"face.osc1.wave","face.osc1.tune","face.osc1.fine","face.osc1.width","face.osc1.level","face.osc2.wave","face.osc2.tune","face.osc2.fine","face.osc2.level","face.osc2.noise",
 "face.fenv.mode","face.fenv.amount","face.fenv.attack","face.fenv.sustain","face.fenv.release","face.amp.decay","face.amp.sustain","face.lfo.rate","face.lfo.shape","face.vel-air","face.lfo.sync","face.drive.shape","face.drive.tone","face.drive.mix"};
const ParameterDescriptor* descriptorOf(const RackDesign& d,const house::Element& e){
 const auto unit=std::find_if(d.units.begin(),d.units.end(),[&](const RackUnit& u){return u.id==e.rackUnit;});if(unit==d.units.end())return nullptr;
 const auto index=rackParameterIndex(unit->module,0,e.key);return index?&parameterRegistry()[*index]:nullptr;}
void ember_face_for(){
 std::cout<<"GROUP ember_face_for"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& ember=examples[0];
 const auto skin=studio::makerFaceFor(ember);CHECK(skin!=nullptr);
 if(skin)CHECK(skin->design==ember.id&&skin->maker=="HALLORAN ELECTRONIC MUSIC CO."&&skin->model=="ANALOG VOICE MODEL 74"&&skin->units==3&&skin->skin.elements.size()==32);
 {auto d=ember;d.units[0].defaults[0].normalized=.3f;d.intent="Another intent";d.revision=4;CHECK(studio::makerFaceFor(d)==nullptr);}
 const auto differs=[&](auto edit){auto d=ember;edit(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](RackDesign& d){d.id=juce::Uuid();}));
 CHECK(differs([](RackDesign& d){d.name="Ember Reed II";}));
 // The plate engraves DRIVE where the example's section is Grain; the rule still compares with the example.
 CHECK(differs([](RackDesign& d){d.panel.sections[2]="Drive";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].label="Cutoff";}));
 const auto thread=studio::makerFaceFor(examples[2]);CHECK(thread!=nullptr&&thread!=skin&&thread->maker=="MARLOW & FINCH");
 {auto other=examples[1];other.id=juce::Uuid();CHECK(studio::makerFaceFor(other)==nullptr);}
 CHECK(airack::test::frozenMakerSkinFile("ember-reed","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("ember-reed","plates/ember-reed-deck.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("thread-press","plates/ember-reed-deck.png").isEmpty());
}
void ember_skin_loads(){
 std::cout<<"GROUP ember_skin_loads"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& ember=examples[0];
 const auto skin=studio::makerFaceFor(ember);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.elements.size()==32&&m.designId==ember.id&&m.units==3&&m.panelWidth==2140.0f);
 std::map<juce::String,int> views,kinds;for(const auto& e:m.elements){++views[e.view];++kinds[e.kind];}
 CHECK(views.size()==3&&views["face"]==7&&views["front"]==1&&views["deck"]==24);
 CHECK(kinds["rotary"]==21&&kinds["selector"]==3&&kinds["lever"]==3&&kinds["fader"]==3&&kinds["lamp"]==1&&kinds["action"]==1);
 CHECK(m.reveal.kind=="drawer"&&m.reveal.view=="deck"&&m.reveal.front=="front"&&m.reveal.units==4&&m.reveal.opener=="face.programmer"&&m.reveal.closers.isEmpty());
 CHECK(m.reveal.titles==juce::StringArray({"Open programmer","Close programmer"}));
 if(const auto thread=studio::makerFaceFor(examples[2])){const auto& r=thread->skin.reveal;
  CHECK(r.kind=="recess"&&r.view=="setup"&&r.front.isEmpty()&&r.units==0&&r.opener=="face.setup"&&r.closers==juce::StringArray({"face.close","face.close.right"}));}
 const auto* face=group(m,"face.face");const auto* deck=group(m,"face.deck");const auto* front=group(m,"face.front");CHECK(face&&deck&&front&&m.groups.size()==3);
 if(face)CHECK(face->focusOrder==juce::StringArray({"face.air","face.bite","face.bloom","face.attack","face.release","face.grain"}));
 if(deck)CHECK(deck->focusOrder==emberDeck&&deck->view=="deck");
 if(front)CHECK(front->focusOrder==juce::StringArray({"face.programmer"})&&front->view=="front");
 int bound=0;
 for(const auto& e:m.elements){
  CHECK(e.id.startsWith("face."));
  if(e.view=="deck")CHECK(e.hasDot&&!e.hitRect.isEmpty()&&e.label.isNotEmpty());
  if(e.rackUnit<0)continue;
  ++bound;CHECK(e.paramId.isEmpty());
  const auto unit=std::find_if(ember.units.begin(),ember.units.end(),[&](const RackUnit& u){return u.id==e.rackUnit;});CHECK(unit!=ember.units.end());
  if(unit!=ember.units.end()){CHECK(rackParameterActive(unit->module,e.key));CHECK(rackParameterIndex(unit->module,0,e.key).has_value());}
  if(e.bindingType=="float")CHECK(e.drawnValue>=e.range.getStart()&&e.drawnValue<=e.range.getEnd());
  else if(e.kind!="lamp")CHECK(e.drawnValue>=0&&e.drawnValue<=(e.bindingType=="toggle"?1:e.choices.size()-1)&&e.drawnValue==std::round(e.drawnValue));
 }
 CHECK(bound==31);
 for(const char* id:{"face.air","face.bite","face.grain"})if(const auto* e=element(m,id)){CHECK(e->knurlFrames==24&&m.assets.count(e->knurl)==1&&e->hasDot&&m.assets.count(e->layer("pointer"))==1);}else CHECK(!"missing face knob");
 if(const auto* e=element(m,"face.air"))CHECK(e->knurlTeeth==101&&e->logarithmic&&e->range==juce::Range<double>(100,10000));
 if(const auto* e=element(m,"face.bite"))CHECK(e->knurlTeeth==86);
 if(const auto* e=element(m,"face.grain"))CHECK(e->knurlTeeth==92&&e->rackUnit==1&&e->key=="drive.gain_db");
 if(const auto* e=element(m,"face.lamp"))CHECK(e->key=="enabled"&&e->rackUnit==0&&e->litValue==1&&e->lensR>20&&e->layer("lit")=="face:lamp.lit");
 if(const auto* e=element(m,"face.programmer"))CHECK(e->kind=="action"&&e->view=="front"&&!e->rect.isEmpty()&&e->rackUnit<0);
 for(const auto& e:m.elements)CHECK(e.tapeRect.isEmpty()==(e.id!="face.vel-air"));
 if(const auto* e=element(m,"face.vel-air"))CHECK(e->label=="MOTION VEL TO AIR"&&e->key=="modulation.1.amount");
 if(const auto* e=element(m,"face.osc1.wave"))CHECK(e->label=="OSC 1 WAVE");
 CHECK(m.asset("face:plate").image.getWidth()==2140&&m.asset("face:plate").image.getHeight()==394);
 CHECK(m.asset("front:plate").image.getWidth()==2140&&m.asset("front:plate").image.getHeight()==197);
 CHECK(m.asset("deck:plate").image.getWidth()==2140&&m.asset("deck:plate").image.getHeight()==788);
}
void ember_angles(){
 std::cout<<"GROUP ember_angles"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[0]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 for(const char* id:{"face.air","face.bite","face.grain"}){
  const auto* e=element(m,id);CHECK(e!=nullptr);if(!e)continue;CHECK(e->ticks.size()>=5);
  for(std::size_t i=0;i<e->ticks.size();++i){
   CHECK(std::abs(angleAt(*e,e->ticks[i].first)-e->ticks[i].second)<.01f);
   if(i>0)CHECK(e->ticks[i].first>e->ticks[i-1].first&&e->ticks[i].second>e->ticks[i-1].second);
  }
 }
 if(const auto* air=element(m,"face.air")){const auto a=angleAt(*air,1800);CHECK(a>tickAngle(*air,1000)&&a<tickAngle(*air,3000));}
 for(const char* id:{"face.bloom","face.attack","face.release"}){
  const auto* e=element(m,id);CHECK(e!=nullptr&&e->kind=="fader");if(!e)continue;
  // The printed 0 is below the printed 10, and the travel's ends are the range's.
  CHECK(e->logarithmic&&e->y0>e->y1+150&&e->capCentreX>0&&!e->hitRect.isEmpty()&&e->hasDot&&m.assets.count(e->layer("cap"))==1);
  CHECK(std::abs(house::proportionOf(*e,e->range.getStart()))<1e-9&&std::abs(house::proportionOf(*e,e->range.getEnd())-1)<1e-9);
 }
 if(const auto* bloom=element(m,"face.bloom"))CHECK(bloom->range==juce::Range<double>(.02,2)&&std::abs(house::proportionOf(*bloom,.2)-.5)<1e-9);
 if(const auto* attack=element(m,"face.attack"))CHECK(attack->range==juce::Range<double>(.001,1));
}
void ember_grip_frames(){
 std::cout<<"GROUP ember_grip_frames"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[0]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 for(const char* id:{"face.air","face.bite","face.grain"}){
  const auto* e=element(m,id);CHECK(e!=nullptr&&e->knurlTeeth>0);if(!e||e->knurlTeeth<=0)continue;
  // The teeth advance one frame each 1/24 of a tooth and come round after a whole one, either way.
  const auto tooth=360.0f/(float)e->knurlTeeth;
  for(int k=0;k<24;++k)CHECK(house::gripFrame(*e,e->knurlRest+k*tooth/24)==k);
  CHECK(house::gripFrame(*e,e->knurlRest+tooth)==0&&house::gripFrame(*e,e->knurlRest-tooth/24)==23&&house::gripFrame(*e,e->knurlRest+7*tooth)==0);
 }
 // A rotary without a grip ring has none.
 if(const auto* e=element(m,"face.osc1.tune"))CHECK(e->knurl.isEmpty()&&e->knurlFrames==0);
}
void ember_deck_scales(){
 std::cout<<"GROUP ember_deck_scales"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& ember=examples[0];const auto skin=studio::makerFaceFor(ember);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 int knobs=0;
 for(const auto& e:m.elements){
  if(e.view!="deck"||e.kind!="rotary")continue;
  ++knobs;CHECK(e.registryResponse&&!e.logarithmic&&e.ticks.size()==2);
  const auto* d=descriptorOf(ember,e);CHECK(d!=nullptr);if(!d)continue;
  CHECK(std::abs(e.range.getStart()-d->minimum)<1e-6&&std::abs(e.range.getEnd()-d->maximum)<1e-6&&std::abs(e.skew-d->skew)<1e-6);
  const juce::NormalisableRange<float> range(d->minimum,d->maximum,0.0f,d->skew);
  CHECK(std::abs(house::proportionOf(e,d->defaultValue)-range.convertTo0to1(d->defaultValue))<1e-6);
  // The scale runs from the first tick at the registry's minimum to the last at its maximum.
  CHECK(std::abs(e.ticks.front().first-d->minimum)<1e-6&&std::abs(e.ticks.back().first-d->maximum)<1e-6&&e.ticks.front().second<-100&&e.ticks.back().second>100);
  CHECK(std::abs(house::pointerAngleFor(e,0)-e.ticks.front().second)<1e-4f&&std::abs(house::pointerAngleFor(e,1)-e.ticks.back().second)<1e-4f);
 }
 CHECK(knobs==18);
 if(const auto* decay=element(m,"face.amp.decay"))CHECK(std::abs(decay->skew-.3)<1e-6&&std::abs(house::proportionOf(*decay,.15)-std::pow(.15/20,.3))<1e-6);
 if(const auto* tune=element(m,"face.osc1.tune"))CHECK(std::abs(house::proportionOf(*tune,0)-.5)<1e-9);
}
void ember_selectors(){
 std::cout<<"GROUP ember_selectors"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& ember=examples[0];const auto skin=studio::makerFaceFor(ember);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 const std::map<juce::String,juce::String> means{{"TRI","Triangle"},{"SINE","Sine"},{"SIN","Sine"},{"SAW","Saw"},{"PULSE","Pulse"},{"SQR","Square"},{"LP","Lowpass"},{"BP","Bandpass"},{"HP","Highpass"},
  {"TANH","Tanh"},{"SOFT","Soft sign"},{"FREE","Off"},{"SYNC","On"}};
 const std::map<juce::String,juce::StringArray> printed{{"face.osc1.wave",{"TRI","SINE","SAW","PULSE"}},{"face.osc2.wave",{"TRI","SINE","SAW","PULSE"}},{"face.lfo.shape",{"TRI","SIN","SAW","SQR"}},
  {"face.fenv.mode",{"LP","BP","HP"}},{"face.lfo.sync",{"FREE","SYNC"}},{"face.drive.shape",{"TANH","SOFT"}}};
 for(const auto& [id,legends]:printed){
  const auto* e=element(m,id);CHECK(e!=nullptr);if(!e)continue;const auto* d=descriptorOf(ember,*e);CHECK(d!=nullptr);if(!d)continue;
  CHECK(e->kind==(legends.size()==4?"selector":"lever")&&(int)e->positions.size()==legends.size());
  for(std::size_t i=0;i<e->positions.size()&&(int)i<legends.size();++i){
   const auto& p=e->positions[i];CHECK(p.legend==legends[(int)i]&&!p.legendRect.isEmpty());
   const auto meant=means.find(p.legend);
   CHECK(meant!=means.end()&&p.value>=0&&(std::size_t)p.value<d->choices.size()&&d->choices[(std::size_t)p.value]==meant->second);
   if(e->kind=="selector"&&i>0)CHECK(p.angle>e->positions[i-1].angle);
   if(e->kind=="lever")CHECK(m.assets.count(p.asset)==1);
  }
  if(e->kind=="selector")CHECK(m.assets.count(e->layer("pointer"))==1&&e->faceR>40&&!e->rect.isEmpty());
  if(e->bindingType=="choice"){juce::StringArray names;for(const auto& c:d->choices)names.add(c);CHECK(e->choices==names);}
 }
 if(const auto* sync=element(m,"face.lfo.sync"))CHECK(sync->bindingType=="toggle");
 // The drawing's own positions: OSC 1 at PULSE, OSC 2 at TRI, LFO SHAPE at SIN, and every bat up.
 for(const auto& [id,v]:std::map<juce::String,double>{{"face.osc1.wave",3},{"face.osc2.wave",1},{"face.lfo.shape",0},{"face.fenv.mode",0},{"face.lfo.sync",0},{"face.drive.shape",0}})
  if(const auto* e=element(m,id))CHECK(e->drawnValue==v);
}
//------------------------------------------------------------------------------ Glass Current's skin
// The spec's screens: each item's ID, slot, VFD name, LCD label and binding, in slot order; BEATS shares TIME's slot.
struct GlassItem{const char* id;int slot;const char* vfd;const char* lcd;int unit;const char* key;};
const std::vector<std::pair<const char*,std::vector<GlassItem>>> glassScreens{
 {"osc.1",{{"face.osc1.table",1,"OSC 1 TABLE","TABLE",0,"wavetable.osc.1.table"},{"face.osc1.pos",2,"OSC 1 POSITION","POS",0,"wavetable.osc.1.position"},{"face.osc1.tune",3,"OSC 1 TUNE","TUNE",0,"wavetable.osc.1.tune"},{"face.osc1.fine",4,"OSC 1 FINE","FINE",0,"wavetable.osc.1.detune"}}},
 {"osc.2",{{"face.osc2.table",1,"OSC 2 TABLE","TABLE",0,"wavetable.osc.2.table"},{"face.osc2.pos",2,"OSC 2 POSITION","POS",0,"wavetable.osc.2.position"},{"face.osc2.tune",3,"OSC 2 TUNE","TUNE",0,"wavetable.osc.2.tune"},{"face.osc2.fine",4,"OSC 2 FINE","FINE",0,"wavetable.osc.2.detune"}}},
 {"osc.3",{{"face.osc1.level",1,"OSC 1 LEVEL","LEVEL",0,"wavetable.osc.1.level"},{"face.osc2.level",2,"OSC 2 LEVEL","LEVEL",0,"wavetable.osc.2.level"},{"face.noise",3,"NOISE","NOISE",0,"wavetable.noise"}}},
 {"filter.1",{{"face.filter.cutoff",1,"FILTER CUTOFF","CUTOFF",0,"wavetable.filter.cutoff"},{"face.filter.reso",2,"FILTER RESO","RESO",0,"wavetable.filter.resonance"},{"face.filter.mode",3,"FILTER MODE","MODE",0,"wavetable.filter.mode"}}},
 {"env.1",{{"face.amp.attack",1,"AMP ATTACK","ATK",0,"wavetable.amp.attack"},{"face.amp.decay",2,"AMP DECAY","DEC",0,"wavetable.amp.decay"},{"face.amp.sustain",3,"AMP SUSTAIN","SUS",0,"wavetable.amp.sustain"},{"face.amp.release",4,"AMP RELEASE","REL",0,"wavetable.amp.release"}}},
 {"env.2",{{"face.fenv.attack",1,"FENV ATTACK","ATK",0,"wavetable.filter_env.attack"},{"face.fenv.decay",2,"FENV DECAY","DEC",0,"wavetable.filter_env.decay"},{"face.fenv.sustain",3,"FENV SUSTAIN","SUS",0,"wavetable.filter_env.sustain"},{"face.fenv.release",4,"FENV RELEASE","REL",0,"wavetable.filter_env.release"}}},
 {"env.3",{{"face.fenv.amount",1,"FENV AMOUNT","AMT",0,"wavetable.filter_env.octaves"}}},
 {"lfo.1",{{"face.lfo.rate",1,"LFO RATE","RATE",0,"lfo.1.rate_hz"},{"face.lfo.shape",2,"LFO SHAPE","SHAPE",0,"lfo.1.shape"},{"face.lfo.sync",3,"LFO SYNC","SYNC",0,"lfo.1.synced"},{"face.lfo.beats",4,"LFO BEATS","BEATS",0,"lfo.1.beats"}}},
 {"delay.1",{{"face.delay.time",1,"DELAY TIME","TIME",1,"delay.seconds"},{"face.delay.beats",1,"DELAY TIME","TIME",1,"delay.beats"},{"face.delay.fdbk",2,"DELAY FDBK","FDBK",1,"delay.feedback"},{"face.delay.sync",3,"DELAY SYNC","SYNC",1,"delay.synced"},{"face.delay.mix",4,"DELAY MIX","MIX",1,"delay.mix"}}},
 {"delay.2",{{"face.delay.stereo",1,"DELAY STEREO","STEREO",1,"delay.stereo"},{"face.delay.filter",2,"DELAY FILTER","FILTER",1,"delay.filter_hz"}}},
 {"reverb.1",{{"face.reverb.size",1,"REVERB SIZE","SIZE",2,"reverb.size"},{"face.reverb.decay",2,"REVERB DECAY","DECAY",2,"reverb.decay_seconds"},{"face.reverb.damp",3,"REVERB DAMP","DAMP",2,"reverb.damping"},{"face.reverb.predly",4,"REVERB PREDLY","PREDLY",2,"reverb.predelay_ms"}}},
 {"reverb.2",{{"face.reverb.mix",1,"REVERB MIX","MIX",2,"reverb.mix"}}}};
const std::vector<std::pair<const char*,juce::StringArray>> glassPages{{"osc",{"osc.1","osc.2","osc.3"}},{"filter",{"filter.1"}},{"env",{"env.1","env.2","env.3"}},{"lfo",{"lfo.1"}},{"delay",{"delay.1","delay.2"}},{"reverb",{"reverb.1","reverb.2"}}};
void glass_face_for(){
 std::cout<<"GROUP glass_face_for"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& glass=examples[1];
 const auto skin=studio::makerFaceFor(glass);CHECK(skin!=nullptr);
 if(skin)CHECK(skin->design==glass.id&&skin->maker=="KUROSHIO"&&skin->model=="GLASS CURRENT GC-86 DIGITAL WAVETABLE SYNTHESIZER"&&skin->units==2&&skin->skin.elements.size()==60);
 {auto d=glass;d.units[0].defaults[0].normalized=.3f;d.intent="Another intent";d.revision=4;CHECK(studio::makerFaceFor(d)==nullptr);}
 const auto differs=[&](auto edit){auto d=glass;edit(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](RackDesign& d){d.id=juce::Uuid();}));
 CHECK(differs([](RackDesign& d){d.name="Glass Current II";}));
 CHECK(differs([](RackDesign& d){d.panel.sections[1]="Space";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].label="Sheen";}));
 const auto ember=studio::makerFaceFor(examples[0]);const auto thread=studio::makerFaceFor(examples[2]);
 CHECK(ember&&thread&&ember!=skin&&thread!=skin&&ember->maker=="HALLORAN ELECTRONIC MUSIC CO."&&thread->maker=="MARLOW & FINCH");
 CHECK(airack::test::frozenMakerSkinFile("glass-current","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("glass-current","plates/glass-current-door.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("ember-reed","plates/glass-current-door.png").isEmpty());
}
void glass_skin_loads(){
 std::cout<<"GROUP glass_skin_loads"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& glass=examples[1];
 const auto skin=studio::makerFaceFor(glass);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.elements.size()==60&&m.designId==glass.id&&m.units==2&&m.panelWidth==2140.0f);
 std::map<juce::String,int> views,kinds;for(const auto& e:m.elements){++views[e.view];++kinds[e.view+":"+e.kind];}
 CHECK(views.size()==4&&views["face"]==7&&views["panel"]==50&&views["door"]==1&&views["strip"]==2);
 CHECK(kinds["face:rotary"]==4&&kinds["face:lamp"]==1&&kinds["face:display"]==1&&kinds["face:action"]==1);
 CHECK(kinds["panel:action"]==6&&kinds["panel:display"]==1&&kinds["panel:rotary"]==39&&kinds["panel:still"]==4);
 CHECK(kinds["door:action"]==1&&kinds["strip:lamp"]==1&&kinds["strip:display"]==1);
 const auto& r=m.reveal;
 CHECK(r.kind=="door"&&r.view=="panel"&&r.hang=="door"&&r.front.isEmpty()&&r.units==2&&r.opener=="face.edit"&&r.closers==juce::StringArray({"face.close"}));
 CHECK(r.titles==juce::StringArray({"Open edit door","Close edit door"})&&r.drawnScreen=="delay.1"&&r.drawnTouch=="face.delay.time");
 CHECK(!m.hostReadout&&m.strip=="strip");
 // Thread Press and Ember Reed keep the house readout, and have no strip, pages or hanging view.
 for(std::size_t i:{std::size_t(0),std::size_t(2)})if(const auto other=studio::makerFaceFor(examples[i]))
  CHECK(other->skin.hostReadout&&other->skin.strip.isEmpty()&&other->skin.pages.empty()&&other->skin.reveal.hang.isEmpty()&&other->skin.reveal.drawnScreen.isEmpty());
 // The pages and their screens as the spec's table; each page's button is a panel action with an LED.
 CHECK(m.pages.size()==glassPages.size());
 for(std::size_t i=0;i<m.pages.size()&&i<glassPages.size();++i){
  const auto& p=m.pages[i];CHECK(p.id==glassPages[i].first&&p.button=="face.page."+p.id&&p.screens==glassPages[i].second&&p.label==p.id.toUpperCase());
  if(const auto* b=element(m,p.button))CHECK(b->kind=="action"&&b->view=="panel"&&b->lensR>0&&!b->rect.isEmpty()&&b->hasDot&&b->layer("lit")=="panel:page.lit");else CHECK(!"missing page button");
 }
 int bound=0;
 for(const auto& e:m.elements){
  CHECK(e.id.startsWith("face.")||e.id.startsWith("panel.")||e.id.startsWith("strip."));
  if(e.rackUnit<0)continue;
  ++bound;CHECK(e.paramId.isEmpty());
  const auto unit=std::find_if(glass.units.begin(),glass.units.end(),[&](const RackUnit& u){return u.id==e.rackUnit;});CHECK(unit!=glass.units.end());
  if(unit!=glass.units.end()){CHECK(rackParameterActive(unit->module,e.key));CHECK(rackParameterIndex(unit->module,0,e.key).has_value());}
  if(e.bindingType=="float")CHECK(e.drawnValue>=e.range.getStart()&&e.drawnValue<=e.range.getEnd());
  else if(e.kind!="lamp")CHECK(e.drawnValue>=0&&e.drawnValue<=e.range.getEnd()&&e.drawnValue==std::round(e.drawnValue));
 }
 CHECK(bound==45);
 // The displays, each with its glass and its lines in the window skin's form.
 const auto lines=[](const house::Element& e){juce::StringArray out;if(auto* o=e.display["lines"].getDynamicObject())for(const auto& nv:o->getProperties())out.add(nv.name.toString());return out;};
 if(const auto* e=element(m,"face.vfd")){CHECK(e->kind=="display"&&e->role=="vfd"&&!e->rect.isEmpty()&&lines(*e)==juce::StringArray({"line1","line2"}));
  CHECK(e->display["bar"]["centers"].size()==15&&m.assets.count(e->display["bar"]["lit"].toString())==1&&m.assets.count(e->display["bar"]["unlit"].toString())==1);}else CHECK(!"missing VFD");
 if(const auto* e=element(m,"panel.lcd"))CHECK(e->kind=="display"&&e->role=="lcd"&&!e->rect.isEmpty()&&lines(*e)==juce::StringArray({"slot1","slot2","slot3","slot4"}));else CHECK(!"missing LCD");
 if(const auto* e=element(m,"strip.name"))CHECK(e->kind=="display"&&e->role=="name"&&!e->rect.isEmpty()&&e->display["lines"]["name"]["text"].toString()=="GLASS CURRENT");else CHECK(!"missing name");
 if(const auto* e=element(m,"face.power"))CHECK(e->kind=="lamp"&&e->key=="enabled"&&e->rackUnit==0&&e->litValue==1&&e->lensR>5&&e->layer("lit")=="face:power.lit");else CHECK(!"missing POWER");
 if(const auto* e=element(m,"strip.power"))CHECK(e->kind=="lamp"&&e->key=="enabled"&&e->rackUnit==0&&e->litValue==1&&e->lensR>5&&e->layer("lit")=="strip:power.lit");else CHECK(!"missing strip POWER");
 if(const auto* e=element(m,"face.edit"))CHECK(e->kind=="action"&&e->view=="face"&&e->label=="Open edit"&&!e->hitRect.isEmpty()&&e->hasDot);else CHECK(!"missing EDIT");
 if(const auto* e=element(m,"face.close"))CHECK(e->kind=="action"&&e->view=="door"&&!e->hitRect.isEmpty()&&e->hasDot);else CHECK(!"missing foot notch");
 for(int k=1;k<=4;++k)if(const auto* e=element(m,"panel.still."+juce::String(k)))CHECK(e->slot==k&&e->faceR>20&&e->layer("pointer")=="panel:soft"+juce::String(k)+".rotor");else CHECK(!"missing spare knob");
 // The plates: the face at 2U; the panel over the door's quarter and the door hanging below the face, at their
 // anchors; the strip across the plate at its drawn proportion, centred in 1U.
 const auto& face=m.asset("face:plate");const auto& panel=m.asset("panel:plate");const auto& door=m.asset("door:plate");const auto& strip=m.asset("strip:plate");
 CHECK(face.image.getWidth()==2140&&face.image.getHeight()==394);
 CHECK(panel.anchor.x>1400&&panel.anchor.y==0&&panel.image.getHeight()==394&&panel.anchor.x+(float)panel.image.getWidth()<=2140);
 CHECK(door.anchor.x>1400&&door.anchor.y==394&&door.image.getHeight()<=394&&door.image.hasAlphaChannel());
 CHECK(strip.image.getWidth()==2140&&strip.anchor.x==0&&std::abs(2*strip.anchor.y+(float)strip.image.getHeight()-197)<=1);
 const auto* gf=group(m,"face.face");const auto* gp=group(m,"face.panel");const auto* gd=group(m,"face.door");CHECK(gf&&gp&&gd&&m.groups.size()==4);
 if(gf)CHECK(gf->focusOrder==juce::StringArray({"face.facet","face.drift","face.echo","face.horizon","face.edit"}));
 if(gp)CHECK(gp->view=="panel"&&gp->focusOrder.size()==45&&gp->focusOrder[0]=="face.page.osc"&&gp->focusOrder[6]=="face.osc1.table");
 if(gd)CHECK(gd->focusOrder==juce::StringArray({"face.close"}));
}
void glass_angles(){
 std::cout<<"GROUP glass_angles"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[1]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 const std::map<juce::String,std::tuple<double,double,bool>> knobs{{"face.facet",{0.0,1.0,false}},{"face.drift",{.05,5.0,true}},{"face.echo",{0.0,.5,false}},{"face.horizon",{.3,8.0,true}}};
 for(const auto& [id,spec]:knobs){
  const auto* e=element(m,id);CHECK(e!=nullptr);if(!e)continue;const auto [lo,hi,log]=spec;
  CHECK(e->ticks.size()==2&&std::abs(e->range.getStart()-lo)<1e-9&&std::abs(e->range.getEnd()-hi)<1e-9&&e->logarithmic==log&&e->hasDot&&m.assets.count(e->layer("pointer"))==1);
  CHECK(std::abs(angleAt(*e,lo)-e->ticks.front().second)<.01f&&std::abs(angleAt(*e,hi)-e->ticks.back().second)<.01f);
  CHECK(e->ticks.front().second<-100&&e->ticks.back().second>100);
  float last=-1000;for(int k=0;k<=20;++k){const auto a=house::pointerAngleFor(*e,k/20.0);CHECK(a>last);last=a;}
 }
 if(const auto* drift=element(m,"face.drift"))CHECK(std::abs(house::proportionOf(*drift,.5)-.5)<1e-9);
 if(const auto* horizon=element(m,"face.horizon"))CHECK(std::abs(house::proportionOf(*horizon,std::sqrt(2.4))-.5)<1e-9);
}
void glass_items(){
 std::cout<<"GROUP glass_items"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& glass=examples[1];const auto skin=studio::makerFaceFor(glass);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 const juce::StringArray detents{"face.osc1.table","face.osc2.table","face.filter.mode","face.lfo.shape","face.lfo.sync","face.delay.sync"};
 int items=0;
 for(const auto& [screen,list]:glassScreens)for(const auto& it:list){
  const auto* e=element(m,it.id);CHECK(e!=nullptr);if(!e)continue;++items;
  CHECK(e->view=="panel"&&e->kind=="rotary"&&e->screen==screen&&e->slot==it.slot&&e->vfdName==it.vfd&&e->lcdLabel==it.lcd&&e->rackUnit==it.unit&&e->key==it.key);
  CHECK(e->hasDot&&e->layer("pointer")=="panel:soft"+juce::String(it.slot)+".rotor"&&e->ticks.size()==2);
  CHECK(e->detents==detents.contains(it.id)&&e->registryResponse!=e->detents);
  const auto* d=descriptorOf(glass,*e);CHECK(d!=nullptr);if(!d||!e->detents)continue;
  // One detent per choice; the registry names them.
  const auto n=std::max<std::size_t>(2,d->choices.size());CHECK(e->range==juce::Range<double>(0,(double)n-1));
  if(!d->choices.empty()){juce::StringArray names;for(const auto& c:d->choices)names.add(c);CHECK(e->choices==names);}
 }
 CHECK(items==39);
 for(const auto& e:m.elements)if(e.kind=="rotary"&&e.view=="panel")CHECK(e.screen.isNotEmpty()&&e.slot>=1&&e.slot<=4);
 // TIME shows while DELAY SYNC is off and BEATS while it is on; every other control always.
 if(const auto* t=element(m,"face.delay.time"))CHECK(t->shownWhen.unit==1&&t->shownWhen.key=="delay.synced"&&t->shownWhen.value==0);
 if(const auto* b=element(m,"face.delay.beats"))CHECK(b->shownWhen.unit==1&&b->shownWhen.key=="delay.synced"&&b->shownWhen.value==1);
 for(const auto& e:m.elements)if(e.id!="face.delay.time"&&e.id!="face.delay.beats")CHECK(e.shownWhen.unit<0);
 for(const auto& [id,name]:std::map<juce::String,juce::String>{{"face.facet","FACET"},{"face.drift","DRIFT"},{"face.echo","ECHO"},{"face.horizon","HORIZON"}})
  if(const auto* e=element(m,id))CHECK(e->vfdName==name&&e->screen.isEmpty()&&e->slot==0&&!e->detents);
}
void glass_lines_fit(){
 std::cout<<"GROUP glass_lines_fit"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[1]);const auto* window=studio::studioSkin();CHECK(skin&&window);if(!skin||!window)return;
 const studio::Lettering let(window->face);int lines=0;
 for(const auto& e:skin->skin.elements){
  if(e.kind!="display")continue;
  const auto minHscale=(float)(double)e.display["min_hscale"];CHECK(minHscale>.5f&&minHscale<1);
  if(auto* o=e.display["lines"].getDynamicObject())for(const auto& nv:o->getProperties()){
   // The skin gives the drawn text and its ink; the fit gives the rest.
   const auto drawn=studio::lineFrom(nv.value);CHECK(drawn.text.isNotEmpty()&&drawn.cap==0&&!drawn.ink.isEmpty()&&drawn.align.isNotEmpty());
   const auto fitted=studio::fitLine(let,drawn);CHECK(fitted.cap>10&&fitted.hscale>.5f&&fitted.hscale<2&&fitted.text==drawn.text&&fitted.ink==drawn.ink);
   const auto b=studio::place(let,fitted,drawn.text,minHscale).path.getBounds();
   CHECK(std::abs(b.getX()-drawn.ink.getX())<.5f&&std::abs(b.getY()-drawn.ink.getY())<.5f&&std::abs(b.getRight()-drawn.ink.getRight())<.5f&&std::abs(b.getBottom()-drawn.ink.getBottom())<.5f);
   // Each VFD line holds 25 cells at its cap and the minimum scale: the drawn 16 and room to squeeze.
   if(e.role=="vfd"){const auto cells=(int)std::floor((fitted.maxRight-fitted.penX)/let.advance(fitted.cap,minHscale)+1e-3f);CHECK(cells==25);if(cells!=25)std::cout<<"  "<<nv.name.toString()<<" holds "<<cells<<" cells"<<std::endl;}
   ++lines;
  }
 }
 CHECK(lines==7);
}
//------------------------------------------------------------------------------ Oxide Return's skin
const juce::StringArray oxideFace{"face.time","face.repeats","face.mix","face.saturation","face.wobble","face.low-cut","face.high-cut","face.in","face.tempo"},
                        oxideTempo{"face.sync","face.note","face.ping-pong","face.cross","face.wobble-rate","face.close"};
// NOTE's steps as the registry names them, and as the recess prints them: a triplet by its 3, a dotted note by its dot.
const juce::StringArray noteChoices{"Beats","1/64 triplet","1/64","1/32 triplet","1/64 dotted","1/32","1/16 triplet","1/32 dotted","1/16","1/8 triplet",
                                    "1/16 dotted","1/8","1/4 triplet","1/8 dotted","1/4","1/2 triplet","1/4 dotted","1/2","1/1 triplet","1/2 dotted","1/1","1/1 dotted"},
                        notePrinted{"BEATS","3","1/64","3","","1/32","3","","1/16","3","","1/8","3","","1/4","3","","1/2","3","","1/1",""};
void oxide_face_for(){
 std::cout<<"GROUP oxide_face_for"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& oxide=examples[3];
 const auto skin=studio::makerFaceFor(oxide);CHECK(skin!=nullptr);
 if(skin)CHECK(skin->design==oxide.id&&skin->maker=="ARDENTI"&&skin->model=="OXIDE RETURN TAPE ECHO MODEL AR-62"&&skin->units==3&&skin->skin.elements.size()==17);
 {auto d=oxide;d.units[0].defaults[0].normalized=.6f;d.intent="Another intent";d.revision=3;CHECK(studio::makerFaceFor(d)==nullptr);}
 const auto differs=[&](auto edit){auto d=oxide;edit(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](RackDesign& d){d.id=juce::Uuid();}));
 CHECK(differs([](RackDesign& d){d.name="Oxide Return II";}));
 CHECK(differs([](RackDesign& d){d.role=RackRole::synth;}));
 CHECK(differs([](RackDesign& d){d.units[0].module=NativeModule::compressor;}));
 CHECK(differs([](RackDesign& d){d.units[0].id=1;}));
 CHECK(differs([](RackDesign& d){d.panel.sections[1]="Grit";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].label="Delay";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[1].key="delay.stereo";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].minimum=.001f;}));
 CHECK(differs([](RackDesign& d){d.panel.controls.pop_back();}));
 for(std::size_t i=0;i<3;++i){const auto other=studio::makerFaceFor(examples[i]);CHECK(other!=nullptr&&other!=skin);}
 CHECK(airack::test::frozenMakerSkinFile("oxide-return","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("oxide-return","plates/oxide-return-face.png").getSize()>0
       &&airack::test::frozenMakerSkinFile("oxide-return","plates/oxide-return-tempo.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("thread-press","plates/oxide-return-face.png").isEmpty()&&airack::test::frozenMakerSkinFile("oxide-return","plates/thread-press-face.png").isEmpty());
}
void oxide_skin_loads(){
 std::cout<<"GROUP oxide_skin_loads"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& oxide=examples[3];
 const auto skin=studio::makerFaceFor(oxide);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.elements.size()==17&&m.designId==oxide.id&&m.maker=="ARDENTI"&&m.modelName=="OXIDE RETURN TAPE ECHO MODEL AR-62"&&m.units==3);
 CHECK(m.panelWidth==2140.0f&&std::abs(m.unitHeight-197.11f)<.01f&&m.strip.isEmpty()&&m.pages.empty()&&m.hostReadout);
 std::map<juce::String,int> views,kinds;for(const auto& e:m.elements){++views[e.view];++kinds[e.kind];}
 CHECK(views.size()==2&&views["face"]==11&&views["tempo"]==6);
 CHECK(kinds["rotary"]==10&&kinds["lever"]==3&&kinds["lamp"]==1&&kinds["still"]==1&&kinds["action"]==2);
 CHECK(m.reveal.kind=="recess"&&m.reveal.view=="tempo"&&m.reveal.front.isEmpty()&&m.reveal.units==0&&m.reveal.opener=="face.tempo"&&m.reveal.closers==juce::StringArray({"face.close"}));
 const auto* face=group(m,"face.face");const auto* tempo=group(m,"face.tempo");CHECK(face&&tempo&&m.groups.size()==2);
 if(face)CHECK(face->focusOrder==oxideFace&&face->view=="face");
 if(tempo)CHECK(tempo->focusOrder==oxideTempo&&tempo->view=="tempo");
 int bound=0;
 for(const auto& e:m.elements){
  CHECK(e.id.startsWith("face.")&&e.label.isNotEmpty());
  if(e.rackUnit<0)continue;
  ++bound;CHECK(e.rackUnit==0&&e.paramId.isEmpty()&&rackParameterActive(NativeModule::delay,e.key)&&rackParameterIndex(NativeModule::delay,0,e.key).has_value());
  if(e.kind=="rotary"){
   CHECK(e.ticks.size()>=2&&e.hasDot&&e.knurlFrames>0&&m.assets.count(e.knurl)==1&&m.assets.count(e.layer("pointer"))==1);
   CHECK(e.drawnValue>=e.range.getStart()&&e.drawnValue<=e.range.getEnd());
  }
  if(e.kind=="lever"){
   CHECK(e.positions.size()==2&&e.hasDot&&e.legendClick&&std::any_of(e.positions.begin(),e.positions.end(),[&](const auto& p){return p.value==e.drawnValue;}));
   for(const auto& p:e.positions)CHECK(m.assets.count(p.asset)==1&&p.legend.isNotEmpty()&&!p.legendRect.isEmpty()&&e.rect.contains(p.legendRect));
  }
 }
 CHECK(bound==14);
 // Each grip ring's teeth; TIME's bezel has 8 frames, the knobs 24.
 const std::pair<const char*,int> teeth[]{{"face.time",252},{"face.repeats",73},{"face.mix",67},{"face.saturation",69},{"face.wobble",67},{"face.low-cut",43},
                                          {"face.high-cut",63},{"face.note",87},{"face.cross",54},{"face.wobble-rate",58}};
 for(const auto& [id,n]:teeth)if(const auto* e=element(m,id))CHECK(e->knurlTeeth==n&&e->knurlFrames==(juce::String(id)=="face.time"?8:24));else CHECK(!"missing knob");
 if(const auto* e=element(m,"face.time")){
  CHECK(e->key=="delay.seconds"&&e->logarithmic&&e->range==juce::Range<double>(.02,2)&&e->unit=="s"&&e->defaultValue==.25&&e->layer("pointer")=="face:time.needle");
  // The bezel, which takes the presses, is centred above the needle's hub.
  CHECK(e->hitCentre().getDistanceFrom(e->pivot)>20&&e->hitCentre().y<e->pivot.y&&std::abs(e->hitR-e->faceR)<.01f);
 }
 for(const char* id:{"face.repeats","face.mix","face.saturation","face.wobble","face.cross"})if(const auto* e=element(m,id))CHECK(e->unit=="%"&&!e->logarithmic);else CHECK(!"missing knob");
 for(const char* id:{"face.low-cut","face.high-cut","face.wobble-rate"})if(const auto* e=element(m,id))CHECK(e->unit=="Hz"&&e->logarithmic);else CHECK(!"missing knob");
 for(const auto& e:m.elements)if(e.id!="face.time")CHECK(e.hitCentre()==e.pivot);
 // IN / OUT side by side, the right button first; FREE / SYNC and DUAL / PING PONG stacked, the top one first: Right and
 // Up push the first.
 if(const auto* e=element(m,"face.in"))CHECK(e->key=="bypassed"&&e->positions[0].value==1&&e->positions[0].legend=="OUT"&&e->positions[0].asset=="face:in.out"
                                            &&e->positions[1].value==0&&e->positions[1].legend=="IN"&&e->positions[1].asset=="face:in.in"
                                            &&e->positions[0].legendRect.getX()>e->positions[1].legendRect.getRight());
 if(const auto* e=element(m,"face.sync"))CHECK(e->view=="tempo"&&e->key=="delay.synced"&&e->positions[0].value==0&&e->positions[0].legend=="FREE"&&e->positions[0].asset=="tempo:sync.free"
                                              &&e->positions[1].value==1&&e->positions[1].legend=="SYNC"&&e->positions[1].asset=="tempo:sync.sync"
                                              &&e->positions[0].legendRect.getBottom()<e->positions[1].legendRect.getY()&&!e->hitRect.isEmpty());
 else CHECK(!"missing FREE / SYNC");
 if(const auto* e=element(m,"face.ping-pong"))CHECK(e->view=="tempo"&&e->key=="delay.ping_pong"&&e->label=="DUAL / PING PONG"&&e->defaultValue==0&&e->drawnValue==0
                                                   &&e->positions[0].value==0&&e->positions[0].legend=="DUAL"&&e->positions[0].asset=="tempo:ping-pong.dual"
                                                   &&e->positions[1].value==1&&e->positions[1].legend=="PING PONG"&&e->positions[1].asset=="tempo:ping-pong.ping-pong"
                                                   &&e->positions[0].legendRect.getBottom()<e->positions[1].legendRect.getY()&&!e->hitRect.isEmpty()
                                                   &&e->hitRect.contains(e->rect));
 else CHECK(!"missing DUAL / PING PONG");
 // NOTE: 22 detented steps, the registry's, each at its printed mark with its legend, 3 or dot to press.
 if(const auto* e=element(m,"face.note")){
  CHECK(e->key=="delay.note"&&e->bindingType=="choice"&&e->detents&&e->legendClick&&e->choices==noteChoices&&e->range==juce::Range<double>(0,21)
        &&e->defaultValue==0&&e->drawnValue==13&&e->ticks.size()==22&&e->positions.size()==22);
  for(std::size_t i=0;i<e->positions.size()&&i<e->ticks.size();++i){
   const auto& p=e->positions[i];
   CHECK(p.value==(double)i&&p.legend==notePrinted[(int)i]&&p.asset.isEmpty()&&e->ticks[i].first==(double)i&&std::abs(p.angle-e->ticks[i].second)<.01f);
   CHECK(!p.legendRect.isEmpty()&&e->hitRect.contains(p.legendRect)&&p.legendRect.getCentre().getDistanceFrom(e->pivot)>e->faceR);
   for(std::size_t j=0;j<i;++j)CHECK(!p.legendRect.intersects(e->positions[j].legendRect));
  }
 }
 else CHECK(!"missing NOTE");
 for(const char* id:{"face.cross","face.wobble-rate"})if(const auto* e=element(m,id))CHECK(e->positions.empty()&&!e->detents&&!e->legendClick);
 if(const auto* e=element(m,"face.lamp"))CHECK(e->key=="bypassed"&&e->rackUnit==0&&e->litValue==0&&e->lensR>20&&e->layer("lit")=="face:lamp.lit");
 if(const auto* e=element(m,"face.hub"))CHECK(e->rackUnit<0&&m.assets.count(e->layer("pointer"))==1&&e->pivot.getDistanceFrom(element(m,"face.time")->pivot)<.01f);
 for(const char* id:{"face.tempo","face.close"})if(const auto* e=element(m,id))CHECK(e->kind=="action"&&e->rackUnit<0&&!e->rect.isEmpty());else CHECK(!"missing action");
 for(const char* id:{"face.note","face.cross","face.wobble-rate"})if(const auto* e=element(m,id))CHECK(e->view=="tempo"&&!e->hitRect.isEmpty()&&e->hitRect.contains(e->pivot));
 const auto& plate=m.asset("face:plate");const auto& tempoPlate=m.asset("tempo:plate");
 CHECK(plate.image.getWidth()==2140&&plate.image.getHeight()==591&&tempoPlate.image.isValid());
 if(const auto* e=element(m,"face.tempo"))CHECK(juce::Rectangle<float>(tempoPlate.anchor.x,tempoPlate.anchor.y,(float)tempoPlate.image.getWidth(),(float)tempoPlate.image.getHeight()).contains(e->rect));
}
void oxide_angles(){
 std::cout<<"GROUP oxide_angles"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[3]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 int knobs=0;
 for(const auto& e:m.elements){
  if(e.kind!="rotary")continue;
  ++knobs;
  for(std::size_t i=0;i<e.ticks.size();++i){
   CHECK(std::abs(angleAt(e,e.ticks[i].first)-e.ticks[i].second)<.01f);
   if(i>0)CHECK(e.ticks[i].first>e.ticks[i-1].first&&e.ticks[i].second>e.ticks[i-1].second);
  }
  // The printed scale spans the range, pinning beyond it.
  CHECK(e.ticks.front().first==e.range.getStart()&&e.ticks.back().first==e.range.getEnd());
  CHECK(house::pointerAngleFor(e,-.2)==e.ticks.front().second&&house::pointerAngleFor(e,1.2)==e.ticks.back().second);
 }
 CHECK(knobs==10);
 // TIME's gauge is logarithmic: 300 ms lies between its 200 ms and 500 ms steps at the share of their log distance.
 if(const auto* time=element(m,"face.time")){
  CHECK(time->ticks.size()==7);
  const auto lo=tickAngle(*time,.2),hi=tickAngle(*time,.5);
  CHECK(std::abs(angleAt(*time,.3)-(lo+(hi-lo)*(float)(std::log(1.5)/std::log(2.5))))<.01f);
 }
 if(const auto* low=element(m,"face.low-cut")){const auto a=angleAt(*low,200);CHECK(a>tickAngle(*low,100)&&a<tickAngle(*low,500));}
 if(const auto* repeats=element(m,"face.repeats"))CHECK(std::abs(angleAt(*repeats,.475)-.5f*(tickAngle(*repeats,0)+tickAngle(*repeats,.95)))<.01f);
}
void oxide_grip_frames(){
 std::cout<<"GROUP oxide_grip_frames"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[3]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 for(const auto& e:m.elements){
  if(e.kind!="rotary")continue;
  CHECK(e.knurlTeeth>0&&e.knurlFrames>0);if(e.knurlTeeth<=0||e.knurlFrames<=0)continue;
  // The frames stacked in the strip; each 1/frames of a tooth advances one, and a whole tooth comes round, either way.
  CHECK(m.asset(e.knurl).image.getHeight()%e.knurlFrames==0);
  const auto tooth=360.0f/(float)e.knurlTeeth;const auto n=e.knurlFrames;
  for(int k=0;k<n;++k)CHECK(house::gripFrame(e,e.knurlRest+k*tooth/(float)n)==k);
  CHECK(house::gripFrame(e,e.knurlRest+tooth)==0&&house::gripFrame(e,e.knurlRest-tooth/(float)n)==n-1);
  // Frame 0 is the drawing's: the rest angle is the pointer's at the drawn value.
  CHECK(std::abs(angleAt(e,e.drawnValue)-e.knurlRest)<.25f);
 }
}
//------------------------------------------------------------------------------ the reveals' motion
// Each skin's time; THREAD PRESS's two fasteners and its cover, which lifts; GLASS CURRENT's closed door on the face;
// OXIDE RETURN's tambour, which slides.
void motion_skins(){
 std::cout<<"GROUP motion_skins"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();
 const auto thread=studio::makerFaceFor(examples[2]);const auto ember=studio::makerFaceFor(examples[0]);const auto glass=studio::makerFaceFor(examples[1]);
 CHECK(thread&&ember&&glass);if(!thread||!ember||!glass)return;
 const auto same=[](juce::Rectangle<float> a,juce::Rectangle<float> b){return std::abs(a.getX()-b.getX())<.01f&&std::abs(a.getY()-b.getY())<.01f&&std::abs(a.getWidth()-b.getWidth())<.01f&&std::abs(a.getHeight()-b.getHeight())<.01f;};
 {const auto& m=thread->skin.reveal.motion;CHECK(m.ms==220&&m.turns.size()==2&&m.door.isEmpty());
  const juce::Point<float> want[]={{1675.0f,210.0f},{1964.0f,210.0f}};
  for(std::size_t i=0;i<m.turns.size()&&i<2;++i){const auto& t=m.turns[i];CHECK(t.getCentre().getDistanceFrom(want[i])<3&&std::abs(t.getWidth()-t.getHeight())<.01f&&t.getWidth()>=34&&t.getWidth()<=48);}
  if(const auto* cover=element(thread->skin,"face.setup"))CHECK(same(m.lid,cover->rect));else CHECK(!"missing cover");
  CHECK(m.corner>=15&&m.corner<=30);}
 {const auto& m=ember->skin.reveal.motion;CHECK(m.ms==200&&m.turns.empty()&&m.lid.isEmpty()&&m.door.isEmpty()&&m.corner==0);}
 {const auto& m=glass->skin.reveal.motion;const auto& panel=glass->skin.asset("panel:plate");
  const juce::Rectangle<float> plate(panel.anchor.x,panel.anchor.y,(float)panel.image.getWidth(),(float)panel.image.getHeight());
  CHECK(m.ms==180&&m.turns.empty()&&m.lid.isEmpty()&&plate.contains(m.door)&&m.door.getBottom()>=360&&m.door.getBottom()<=394&&m.door.getWidth()>=480&&m.door.getWidth()<=537);
  if(const auto* edit=element(glass->skin,"face.edit"))CHECK(m.door.contains(edit->rect.getCentre()));else CHECK(!"missing EDIT");}
 for(const auto& s:{thread,ember,glass})CHECK(s->skin.reveal.motion.slide.isOrigin());
 if(const auto oxide=studio::makerFaceFor(examples[3])){const auto& m=oxide->skin.reveal.motion;
  CHECK(m.ms==260&&m.turns.empty()&&m.door.isEmpty()&&m.corner==0&&std::abs(m.slide.x-277.7f)<.01f&&m.slide.y==0);
  // The lid is the tambour; slid all the way, its pull bar lands on the open one, which closes it.
  const auto* tambour=element(oxide->skin,"face.tempo");const auto* close=element(oxide->skin,"face.close");CHECK(tambour&&close);
  if(tambour&&close)CHECK(same(m.lid,tambour->rect)&&std::abs(m.lid.getX()+m.slide.x-close->rect.getX())<1&&m.lid.contains(close->rect.getCentre()));}
 else CHECK(!"no Oxide Return skin");
}
void house_skin_unchanged(){
 std::cout<<"GROUP house_skin_unchanged"<<std::endl;const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->house;
 // The angles pointerAngleFor gave before maker skins, at proportions 0, 0.5 and 1.
 const std::pair<const char*,std::array<float,3>> expected[]={{"unit.pan.2",{-148.4290f,4.3820f,142.9555f}},{"unit.macro.1",{-142.4192f,3.4837f,140.1144f}},{"unit.gain",{-138.5512f,1.9044f,141.4754f}},{"strip.master",{-139.4695f,.3437f,141.0858f}}};
 for(const auto& [id,angles]:expected)if(const auto* e=element(m,id)){CHECK(e->ticks.empty()&&e->rackUnit==-1&&!e->logarithmic&&!e->hasDot);for(int i=0;i<3;++i)CHECK(std::abs(house::pointerAngleFor(*e,i*.5)-angles[(size_t)i])<.01f);}else CHECK(!"missing house knob");
 for(const auto& e:m.elements){CHECK(e.rackUnit==-1&&e.positions.empty());if(e.kind!="action"&&e.kind!="tape"&&e.kind!="toggle"&&e.kind!="fader"&&e.kind!="rotary"&&e.kind!="disclosure")CHECK(!"a house kind changed");}
 CHECK(m.designId.isNull()&&m.maker.isEmpty()&&m.units==0);
}
//------------------------------------------------------------------------------ the faceplate over the real processor
using Pt=juce::Point<float>;
using studio::house::Knob;
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(60);}
companion::ClientOptions offline(){companion::ClientOptions o;o.allowLaunch=false;return o;}
juce::MouseEvent event(juce::Component& c,Pt at,juce::ModifierKeys mods,int clicks,bool dragged,std::optional<Pt> down={}){const auto t=juce::Time::getCurrentTime();return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,juce::MouseInputSource::defaultTiltY,&c,&c,t,down.value_or(at),t,clicks,dragged);}
// The controls sit in views laid out in face pixels from the face's origin.
Pt local(juce::Component& c,Pt facePoint){return facePoint-c.getPosition().toFloat();}
// A press, ten moves and a release on the control's own mouse handlers.
void drag(juce::Component& c,Pt from,Pt to){const auto a=local(c,from),b=local(c,to);const juce::ModifierKeys held(juce::ModifierKeys::leftButtonModifier);c.mouseDown(event(c,a,held,1,false));for(int i=1;i<=10;++i)c.mouseDrag(event(c,a+(b-a)*((float)i/10.0f),held,1,true,a));c.mouseUp(event(c,b,{},1,true,a));}
void clickAt(juce::Component& c,Pt at){const auto a=local(c,at);c.mouseDown(event(c,a,juce::ModifierKeys::leftButtonModifier,1,false));c.mouseUp(event(c,a,{},1,false));}
juce::MouseWheelDetails wheel(float dy){juce::MouseWheelDetails w;w.deltaX=0.0f;w.deltaY=dy;w.isReversed=w.isSmooth=w.isInertial=false;return w;}
struct Counts{int begins=0,ends=0;bool operator==(const Counts&) const=default;};
struct Counter final:juce::AudioProcessorParameter::Listener{Counts counts;void parameterValueChanged(int,float) override{}void parameterGestureChanged(int,bool starting) override{++(starting?counts.begins:counts.ends);}};
int maxDiff(const juce::Image& a,const juce::Image& b){
 if(a.getBounds()!=b.getBounds())return 256;const juce::Image::BitmapData da(a,juce::Image::BitmapData::readOnly),db(b,juce::Image::BitmapData::readOnly);int out=0;
 for(int y=0;y<a.getHeight();++y)for(int x=0;x<a.getWidth();++x){const auto p=da.getPixelColour(x,y),q=db.getPixelColour(x,y);out=std::max({out,std::abs(p.getRed()-q.getRed()),std::abs(p.getGreen()-q.getGreen()),std::abs(p.getBlue()-q.getBlue())});}
 return out;}
juce::StringArray tabOrder(juce::Component& c){juce::StringArray out;for(auto* f:juce::KeyboardFocusTraverser().getAllComponents(&c))out.add(f->getComponentID());return out;}
// Two placed racks, each with its faceplate, the second `below` the first, on the desktop so that focus works.
struct Rig{
 InstrumentProcessor p{offline()};
 Rig(){CHECK(blankInstance(p));}
 bool closing=false;juce::StringArray reported;
 std::map<std::size_t,std::unique_ptr<Counter>> counters;
 juce::Component holder;
 juce::Uuid first,second;
 std::unique_ptr<studio::MakerFace> a,b;
 void show(int below,int height){
  a=make(first);b=make(second);CHECK(a!=nullptr&&b!=nullptr);
  holder.setSize(2140,height);if(a)holder.addAndMakeVisible(*a);if(b){b->setTopLeftPosition(0,below);holder.addAndMakeVisible(*b);}
  holder.addToDesktop(0);holder.setVisible(true);pump();
 }
 ~Rig(){a.reset();b.reset();for(auto& [i,c]:counters)p.control(i).removeListener(c.get());pump();}
 SoundDocument document(){SoundDocument d;std::uint64_t at=0;CHECK(p.trySoundSnapshot(d,at));return d;}
 std::uint64_t revision(){SoundDocument d;std::uint64_t at=0;CHECK(p.trySoundSnapshot(d,at));return at;}
 juce::Uuid place(std::size_t example,RackDestination to,std::vector<UnitBinding> allocation,bool reuse=false){RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[example];op.newInstance=juce::Uuid();op.destination=to;op.allocation=std::move(allocation);op.allowSlotReuse=reuse;const auto status=p.applyRackOperation(op,revision());CHECK(status==RackCandidateStatus::ready);if(status!=RackCandidateStatus::ready)std::cout<<"  placement status "<<(int)status<<std::endl;return op.newInstance;}
 RackInstance instance(juce::Uuid id){const auto d=document();if(d.personalities)for(const auto& i:d.personalities->instances)if(i.id==id)return i;CHECK(!"no such rack");return {};}
 studio::house::Host host(){studio::house::Host h;h.frame=[]{return std::shared_ptr<void>();};h.closing=[this]{return closing;};h.report=[this](const juce::String& s){reported.add(s);};return h;}
 // The skin the faceplates use instead of the design's own.
 studio::MakerSkinPtr fixed;
 std::unique_ptr<studio::MakerFace> make(juce::Uuid id){const auto i=instance(id);const auto skin=fixed?fixed:studio::makerFaceFor(i.design);CHECK(skin!=nullptr);if(!skin)return nullptr;
  try{return studio::MakerFace::forRack(p,skin,i,host());}catch(const std::exception& e){std::cout<<"  the faceplate threw: "<<e.what()<<std::endl;CHECK(!"the faceplate threw");return nullptr;}}
 std::size_t index(NativeModule m,std::uint8_t slot,const char* key){const auto i=rackParameterIndex(m,slot,key);CHECK(i.has_value());return i.value_or(0);}
 Counts gestures(std::size_t i){auto& c=counters[i];if(!c){c=std::make_unique<Counter>();p.control(i).addListener(c.get());}return c->counts;}
 double physical(std::size_t i){auto& c=p.control(i);return c.convertFrom0to1(c.getValue());}
 // What a plug-in wrapper does for host automation: the value and its listeners, no gesture.
 void automate(std::size_t i,double v){auto& c=p.control(i);c.setValueNotifyingHost(c.convertTo0to1((float)v));}
};
// Two Thread Press racks on the shared master: the first's compressor in effect bank 1 and drive in bank 7, the
// second's in banks 2 and 8.
struct FaceRig:Rig{FaceRig(){first=place(2,{RackRole::fx,4,0},{{0,0},{1,6}});second=place(2,{RackRole::fx,4,2},{{0,1},{1,7}});show(400,800);}};
// Two Ember Reeds: the first's analog unit in synth slot 1 and its drive in effect bank 1, the second's in slot 2 and
// bank 2.
struct EmberRig:Rig{EmberRig(){first=place(0,{RackRole::synth,4,0},{{0,0},{1,0}},true);second=place(0,{RackRole::synth,4,1},{{0,1},{1,1}},true);show(1400,2800);}};
constexpr auto comp=NativeModule::compressor,drv=NativeModule::drive,analog=NativeModule::analog;
void face_binds_its_banks(){
 std::cout<<"GROUP face_binds_its_banks"<<std::endl;FaceRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(0)},std::pair{r.b.get(),std::uint8_t(1)}}){
  CHECK(face->getComponentID()=="rack.face"&&face->getWidth()==2140&&face->getHeight()==394&&face->getTitle()=="MARLOW & FINCH, PROGRAMME COMPRESSOR TYPE TP-2");
  const auto drive=std::uint8_t(slot+6);
  const auto knob=[&](studio::house::View& v,const char* id){auto* k=v.slider<Knob>(id);CHECK(k!=nullptr);return k?k->element.paramIndex:~std::size_t();};
  const auto lever=[&](studio::house::View& v,const char* id){auto* l=v.lever(id);CHECK(l!=nullptr);return l?l->element.paramIndex:~std::size_t();};
  CHECK(knob(face->face,"grip")==r.index(comp,slot,"compressor.threshold_db")&&knob(face->face,"front")==r.index(comp,slot,"compressor.attack_ms"));
  CHECK(knob(face->face,"blend")==r.index(comp,slot,"compressor.mix")&&knob(face->face,"density")==r.index(drv,drive,"drive.gain_db"));
  CHECK(lever(face->face,"in")==r.index(comp,slot,"bypassed"));
  {auto* lamp=face->face.lamp("lamp");CHECK(lamp!=nullptr&&lamp->element.paramIndex==r.index(comp,slot,"bypassed"));}
  CHECK(knob(face->revealed(),"ratio")==r.index(comp,slot,"compressor.ratio")&&knob(face->revealed(),"release")==r.index(comp,slot,"compressor.release_ms")&&knob(face->revealed(),"makeup")==r.index(comp,slot,"compressor.makeup_db"));
  CHECK(knob(face->revealed(),"tone")==r.index(drv,drive,"drive.tone_hz")&&knob(face->revealed(),"mix")==r.index(drv,drive,"drive.mix"));
  CHECK(lever(face->revealed(),"shape")==r.index(drv,drive,"drive.shape")&&lever(face->revealed(),"drive-in")==r.index(drv,drive,"bypassed"));
  if(auto* grip=face->face.slider<Knob>("grip"))CHECK(&grip->param==&r.p.control(r.index(comp,slot,"compressor.threshold_db")));
 }
 // A rack whose unit has no bank gets no faceplate.
 {auto broken=r.instance(r.first);broken.bindings.pop_back();CHECK(studio::MakerFace::forRack(r.p,studio::makerFaceFor(broken.design),broken,r.host())==nullptr);}
}
void face_gestures(){
 std::cout<<"GROUP face_gestures"<<std::endl;FaceRig r;if(!r.a)return;
 auto* grip=r.a->face.slider<Knob>("grip");CHECK(grip!=nullptr);if(!grip)return;
 const auto t1=r.index(comp,0,"compressor.threshold_db"),t2=r.index(comp,1,"compressor.threshold_db");
 CHECK(std::abs(r.physical(t1)+18)<1e-3);
 const auto other=r.gestures(t2);auto g0=r.gestures(t1);
 drag(*grip,grip->element.pivot,grip->element.pivot-Pt(0.0f,60.0f));auto g1=r.gestures(t1);
 CHECK(std::abs(r.physical(t1)-(-18+0.2*36))<0.05&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 CHECK(r.gestures(t2)==other&&std::abs(r.physical(t2)+18)<1e-3);
 {const auto held=r.physical(t1);studio::noteRackScroll();grip->mouseWheelMove(event(*grip,local(*grip,grip->element.pivot),{},0,false),wheel(0.1f));CHECK(r.physical(t1)==held);juce::Thread::sleep(450);}
 g0=g1;const auto before=r.physical(t1);grip->mouseWheelMove(event(*grip,local(*grip,grip->element.pivot),{},0,false),wheel(0.1f));g1=r.gestures(t1);
 CHECK(r.physical(t1)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto p0=grip->proportion();grip->keyPressed(juce::KeyPress(juce::KeyPress::upKey));g1=r.gestures(t1);
 CHECK(std::abs(grip->proportion()-p0-0.01)<0.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;grip->mouseDoubleClick(event(*grip,local(*grip,grip->element.pivot),{},2,false));g1=r.gestures(t1);
 CHECK(std::abs(r.physical(t1)+18)<1e-3&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 // FRONT turns logarithmically: half its travel from 1 ms is the geometric middle of 1 and 60 ms.
 if(auto* front=r.a->face.slider<Knob>("front")){const auto f=r.index(comp,0,"compressor.attack_ms");r.automate(f,1.0);drag(*front,front->element.pivot,front->element.pivot-Pt(0.0f,150.0f));CHECK(std::abs(r.physical(f)-std::sqrt(60.0))<0.05);}
 // While another gesture holds the processor, a drag changes nothing, shows the parameter again and says why.
 const auto held=r.index(comp,0,"compressor.ratio");CHECK(r.p.beginParameterGesture(held));
 const auto v=r.physical(t1);const auto pr=grip->proportion();r.reported.clear();
 drag(*grip,grip->element.pivot,grip->element.pivot-Pt(0.0f,60.0f));
 CHECK(r.physical(t1)==v&&std::abs(grip->proportion()-pr)<1e-6&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
void face_pins(){
 std::cout<<"GROUP face_pins"<<std::endl;FaceRig r;if(!r.a)return;
 auto* grip=r.a->face.slider<Knob>("grip");CHECK(grip!=nullptr);if(!grip)return;
 const auto t1=r.index(comp,0,"compressor.threshold_db");const auto g0=r.gestures(t1);
 r.automate(t1,-50.0);
 CHECK(std::abs(r.physical(t1)+50)<1e-3&&std::abs(grip->pointerAngle()-grip->element.ticks.front().second)<0.01f);
 CHECK(grip->readoutText()=="-50.0 dB"&&r.gestures(t1)==g0);
 // The readout says each faceplate unit as printed.
 const auto text=[&](studio::house::View& v,const char* id,std::size_t i,double value){r.automate(i,value);auto* k=v.slider<Knob>(id);return k?k->readoutText():juce::String("missing");};
 CHECK(text(r.a->face,"front",r.index(comp,0,"compressor.attack_ms"),15.0)=="15 ms"&&text(r.a->face,"front",r.index(comp,0,"compressor.attack_ms"),2.5)=="2.5 ms");
 CHECK(text(r.a->face,"blend",r.index(comp,0,"compressor.mix"),0.5)=="50%"&&text(r.a->face,"density",r.index(drv,6,"drive.gain_db"),6.0)=="+6.0 dB");
 CHECK(text(r.a->revealed(),"release",r.index(comp,0,"compressor.release_ms"),1500.0)=="1.50 s"&&text(r.a->revealed(),"tone",r.index(drv,6,"drive.tone_hz"),12000.0)=="12.0 kHz"&&text(r.a->revealed(),"tone",r.index(drv,6,"drive.tone_hz"),450.0)=="450 Hz");
}
void face_levers(){
 std::cout<<"GROUP face_levers"<<std::endl;FaceRig r;if(!r.a||!r.b)return;
 auto* in=r.a->face.lever("in");auto* lamp=r.a->face.lamp("lamp");CHECK(in!=nullptr&&lamp!=nullptr);if(!in||!lamp)return;
 const auto by=r.index(comp,0,"bypassed");
 CHECK(r.physical(by)<0.5&&lamp->lit()&&in->legend()=="IN"&&in->getTitle()=="IN / OUT");
 auto g0=r.gestures(by);clickAt(*in,in->element.rect.getCentre());auto g1=r.gestures(by);
 CHECK(r.physical(by)>0.5&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&!lamp->lit()&&in->legend()=="OUT");
 in->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(r.physical(by)<0.5&&lamp->lit()&&in->legend()=="IN");
 in->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(r.physical(by)>0.5);
 in->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(r.physical(by)<0.5);
 // Down and the wheel towards you move the bat down to OUT, Up and the wheel away back up to IN, stopping at the ends,
 // each one complete gesture.
 {const auto step=[&](auto act,double to,int gestures){const auto g0=r.gestures(by);act();const auto g1=r.gestures(by);return std::abs(r.physical(by)-to)<1e-3&&g1.begins==g0.begins+gestures&&g1.ends==g0.ends+gestures;};
  const auto key=[&](int code){return [&,code]{in->keyPressed(juce::KeyPress(code));};};
  const auto roll=[&](float dy){return [&,dy]{in->mouseWheelMove(event(*in,local(*in,in->element.rect.getCentre()),{},0,false),wheel(dy));};};
  CHECK(step(key(juce::KeyPress::upKey),0.0,0)&&step(key(juce::KeyPress::downKey),1.0,1)&&step(key(juce::KeyPress::downKey),1.0,0)&&step(key(juce::KeyPress::upKey),0.0,1));
  CHECK(step(roll(-0.1f),1.0,1)&&step(roll(-0.1f),1.0,0)&&step(roll(0.1f),0.0,1)&&in->legend()=="IN");}
 // The other rack's lever and lamp are its own.
 CHECK(r.physical(r.index(comp,1,"bypassed"))<0.5&&r.b->face.lamp("lamp")->lit());
 {auto& b=r.p.control(r.index(comp,1,"bypassed"));b.setValueNotifyingHost(1.0f);CHECK(!r.b->face.lamp("lamp")->lit()&&r.b->face.lever("in")->legend()=="OUT"&&lamp->lit());}
 // TANH / SOFT in the recess, by its whole cell.
 r.a->setOpen(true);auto* shape=r.a->revealed().lever("shape");CHECK(shape!=nullptr);if(!shape)return;
 const auto sh=r.index(drv,6,"drive.shape");
 // Thread Press's drive starts at Soft sign.
 CHECK(juce::roundToInt(r.physical(sh))==1&&shape->legend()=="SOFT");
 const auto cell=shape->element.hitRect;clickAt(*shape,{cell.getX()+4.0f,cell.getBottom()-4.0f});CHECK(juce::roundToInt(r.physical(sh))==0&&shape->legend()=="TANH");
 clickAt(*shape,shape->element.rect.getCentre());CHECK(juce::roundToInt(r.physical(sh))==1);
 CHECK(!shape->hitTest(juce::roundToInt(cell.getRight()+4.0f-(float)shape->getX()),juce::roundToInt(cell.getCentreY()-(float)shape->getY())));
 // While another gesture holds the processor, a flip changes nothing and says why.
 const auto held=r.index(comp,0,"compressor.ratio");CHECK(r.p.beginParameterGesture(held));r.reported.clear();
 clickAt(*in,in->element.rect.getCentre());CHECK(r.physical(by)<0.5&&in->legend()=="IN"&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
void face_setup(){
 std::cout<<"GROUP face_setup"<<std::endl;FaceRig r;if(!r.a)return;auto& f=*r.a;
 auto* cover=f.face.button("setup");CHECK(cover!=nullptr&&cover->getTitle()=="Open set-up");if(!cover)return;
 const juce::StringArray closed{"face.grip","face.front","face.blend","face.density","face.in","face.setup"};
 auto open=closed;open.addArray(juce::StringArray{"face.ratio","face.release","face.makeup","face.shape","face.tone","face.mix","face.drive-in","face.close"});
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&tabOrder(f)==closed&&f.front()==nullptr&&f.units()==2);
 const auto bounds=f.getBounds();const auto rev=r.revision();const auto values=r.document().values;int opened=0;f.onOpen=[&]{++opened;};
 clickAt(*cover,cover->element.rect.getCentre());
 CHECK(f.isOpen()&&f.revealed().isVisible()&&f.getBounds()==bounds&&opened==1&&f.units()==2&&cover->getTitle()=="Open set-up");
 CHECK(tabOrder(f)==open);if(tabOrder(f)!=open)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 // Closing with focus inside the recess hands it to SET-UP.
 auto* ratio=f.revealed().slider<Knob>("ratio");auto* close=f.revealed().button("close");auto* right=f.revealed().button("close.right");CHECK(ratio&&close&&right);if(!ratio||!close||!right)return;
 CHECK(close->getTitle()=="Close set-up"&&!right->getWantsKeyboardFocus());
 ratio->grabKeyboardFocus();CHECK(ratio->hasKeyboardFocus(false));
 close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&juce::Component::getCurrentlyFocusedComponent()==cover&&opened==2&&tabOrder(f)==closed);
 // The right fastener closes it too; closing with focus outside leaves focus where it is.
 auto* grip=f.face.slider<Knob>("grip");grip->grabKeyboardFocus();
 cover->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(f.isOpen()&&grip->hasKeyboardFocus(false));
 clickAt(*right,right->element.rect.getCentre());CHECK(!f.isOpen()&&grip->hasKeyboardFocus(false));
 // Opening and closing wrote nothing.
 CHECK(r.revision()==rev&&r.document().values==values&&f.getBounds()==bounds);
 f.setOpen(true);ratio->grabKeyboardFocus();CHECK(f.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
 CHECK(!f.isOpen()&&cover->hasKeyboardFocus(false)&&r.revision()==rev&&r.document().values==values);
 // RATIO is continuous: 2.5 from automation reads exactly and turns between its 2 and 3 steps.
 f.setOpen(true);r.automate(r.index(comp,0,"compressor.ratio"),2.5);
 const auto tick=[&](double v){for(const auto& [value,angle]:ratio->element.ticks)if(value==v)return angle;return 0.0f;};
 CHECK(ratio->readoutText()=="2.50"&&ratio->pointerAngle()>tick(2)&&ratio->pointerAngle()<tick(3));
}
void face_compose(const juce::File& images){
 std::cout<<"GROUP face_compose"<<std::endl;FaceRig r;if(!r.a)return;auto& f=*r.a;
 for(auto* v:{&f.face,&f.revealed()}){for(auto& s:v->sliders)r.automate(s->element.paramIndex,s->element.drawnValue);for(auto& l:v->levers)r.automate(l->element.paramIndex,l->element.drawnValue);}
 pump();ParameterValues values{};for(std::size_t i=0;i<values.size();++i)values[i]=r.p.control(i).getValue();
 f.face.hideReadouts();f.revealed().hideReadouts();
 const auto composed=f.compose(values);const auto live=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(composed.getWidth()==2140&&composed.getHeight()==394);
 {const auto d=maxDiff(composed,live);CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live face by "<<d<<std::endl;}
 if(maxDiff(composed,live)>1){for(const auto& [key,asset]:f.skin.skin.assets)std::cout<<"  image type "<<key<<" "<<(asset.image.isValid()?asset.image.getPixelData()->createType()->getTypeID():-1)<<std::endl;
  int shown=0;for(int y=0;y<composed.getHeight()&&shown<5;++y)for(int x=0;x<composed.getWidth()&&shown<5;++x)if(composed.getPixelAt(x,y)!=live.getPixelAt(x,y)){std::cout<<"  diff "<<x<<","<<y<<" "<<composed.getPixelAt(x,y).toString()<<" "<<live.getPixelAt(x,y).toString()<<std::endl;++shown;}}
 // It composes the closed face whatever the live face shows.
 f.setOpen(true);f.revealed().hideReadouts();const auto opened=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(maxDiff(f.compose(values),composed)==0&&maxDiff(opened,composed)>40);
 // Open, it draws the recess over the face as the live one does.
 {const auto d=maxDiff(f.compose(values,true),opened);CHECK(d<=1);if(d>1)std::cout<<"  the open compose differs from the live face by "<<d<<std::endl;}
 // Other values turn the heads and flip the lever.
 {auto other=values;other[r.index(comp,0,"compressor.threshold_db")]=1.0f;other[r.index(comp,0,"bypassed")]=1.0f;CHECK(maxDiff(f.compose(other),composed)>40);}
 if(images!=juce::File{}){
  for(const auto& [name,img]:{std::pair<const char*,const juce::Image*>{"face-drawn.png",&composed},{"face-open-drawn.png",&opened}}){const auto file=images.getChildFile(name);file.deleteFile();auto out=file.createOutputStream();CHECK(out&&juce::PNGImageFormat().writeImageToStream(*img,*out));}
 }
}
// Of each 1 px step along the path, the share with a near-white pixel within 1 px of it.
double whiteAlong(const juce::Image& img,const juce::Path& p){
 int white=0,n=0;
 for(float t=0.0f;t<p.getLength();t+=1.0f,++n){
  const auto at=p.getPointAlongPath(t).roundToInt();bool hit=false;
  for(int dy=-1;dy<=1&&!hit;++dy)for(int dx=-1;dx<=1&&!hit;++dx){const auto c=img.getPixelAt(at.x+dx,at.y+dy);hit=c.getRed()>230&&c.getGreen()>230&&c.getBlue()>230;}
  white+=hit?1:0;
 }
 return n?(double)white/n:0.0;}
// The share of a path along which `a` differs from `b` within two pixels by more than 48 in a channel: a mark that reads
// on a pale faceplate, where white alone does not.
double changedAlong(const juce::Image& a,const juce::Image& b,const juce::Path& p){
 int changed=0,n=0;
 for(float t=0.0f;t<p.getLength();t+=1.0f,++n){
  const auto at=p.getPointAlongPath(t).roundToInt();bool hit=false;
  for(int dy=-2;dy<=2&&!hit;++dy)for(int dx=-2;dx<=2&&!hit;++dx){const auto x=a.getPixelAt(at.x+dx,at.y+dy),y=b.getPixelAt(at.x+dx,at.y+dy);
   hit=std::abs(x.getRed()-y.getRed())>48||std::abs(x.getGreen()-y.getGreen())>48||std::abs(x.getBlue()-y.getBlue())>48;}
  changed+=hit?1:0;
 }
 return n?(double)changed/n:0.0;}
juce::Path line(Pt a,Pt b){juce::Path p;p.startNewSubPath(a);p.lineTo(b);return p;}
juce::Path outline(juce::Rectangle<float> r){juce::Path p;p.addRectangle(r);return p;}
// The AI's marks (the spec: The AI's marks): a candidate that keeps the rack marks each changed control at its proposed
// value, in focus order and in its description; a set-up control is drawn marked only while the plate is open, and the
// cover is outlined while it is closed; a locked control and an equal value get none; another panel or other banks
// outline the whole face instead, and a candidate without the rack marks nothing.
void face_marks(const juce::File& images){
 std::cout<<"GROUP face_marks"<<std::endl;FaceRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* grip=f.face.slider<Knob>("grip");auto* in=f.face.lever("in");auto* cover=f.face.button("setup");auto* ratio=f.revealed().slider<Knob>("ratio");
 CHECK(grip&&in&&cover&&ratio);if(!grip||!in||!cover||!ratio)return;
 const auto t=r.index(comp,0,"compressor.threshold_db"),by=r.index(comp,0,"bypassed"),ra=r.index(comp,0,"compressor.ratio");
 const auto live=r.document();auto candidate=live;
 const auto set=[&](SoundDocument& d,std::size_t i,double v){d.values[i]=r.p.control(i).convertTo0to1((float)v);};
 set(candidate,t,-24.0);set(candidate,by,1.0);set(candidate,ra,4.0);
 // The host's look at one stage pixel a face pixel.
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(live);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto plain=snapshot();f.setOpen(true);const auto plainOpen=snapshot();f.setOpen(false);
 CHECK(f.marks().empty()&&!f.proposesPanel());
 f.showProposal(std::make_shared<const SoundDocument>(candidate));
 const auto marks=f.marks();CHECK(marks.size()==3&&!f.proposesPanel());
 if(marks.size()==3)CHECK(marks[0].element=="face.grip"&&std::abs(marks[0].proposed+24.0f)<0.01f&&marks[1].element=="face.in"&&marks[1].proposed==1.0f
                          &&marks[2].element=="face.ratio"&&std::abs(marks[2].proposed-4.0f)<0.01f);
 CHECK(grip->getDescription()=="AI proposes -24.0 dB"&&in->getDescription()=="AI proposes OUT"&&ratio->getDescription()=="AI proposes 4.00");
 CHECK(f.face.control("front")->getDescription().isEmpty()&&cover->getDescription()=="AI proposes changes to 1 set-up control");
 // GRIP's dashed pointer at -24 dB's angle, out to its skirt; IN / OUT's outline; the cover's outline, and not RATIO's mark.
 const auto pointer=[](const studio::house::Element& e,double v){const auto a=juce::degreesToRadians(house::pointerAngleFor(e,house::proportionOf(e,v)));
  return line(e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.35f,e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.95f);};
 const auto gripLine=pointer(grip->element,-24.0),ratioLine=pointer(ratio->element,4.0);
 const auto inEdge=outline(in->element.rect.expanded(3.0f)),coverEdge=outline(cover->element.rect.expanded(3.0f));
 const auto marked=snapshot();
 const auto gain=[](const juce::Image& a,const juce::Image& b,const juce::Path& p){return whiteAlong(a,p)-whiteAlong(b,p);};
 CHECK(gain(marked,plain,gripLine)>0.35&&gain(marked,plain,inEdge)>0.35&&gain(marked,plain,coverEdge)>0.35&&std::abs(gain(marked,plain,ratioLine))<0.05);
 // Each mark on its own: nothing joins GRIP's pointer to IN / OUT's outline.
 {const auto& e=grip->element;const auto a=juce::degreesToRadians(house::pointerAngleFor(e,house::proportionOf(e,-24.0)));
  const auto from=e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR,to=in->element.rect.expanded(3.0f).getTopLeft();
  CHECK(gain(marked,plain,line(from+(to-from)*0.2f,from+(to-from)*0.8f))<0.05);}
 std::cout<<"  closed: GRIP "<<gain(marked,plain,gripLine)<<", IN / OUT "<<gain(marked,plain,inEdge)<<", cover "<<gain(marked,plain,coverEdge)<<", RATIO "<<gain(marked,plain,ratioLine)<<std::endl;
 // Open, RATIO's pointer and no cover outline or count.
 f.setOpen(true);const auto markedOpen=snapshot();
 CHECK(gain(markedOpen,plainOpen,ratioLine)>0.35&&gain(markedOpen,plainOpen,coverEdge)<0.05&&cover->getDescription().isEmpty());
 std::cout<<"  open: RATIO "<<gain(markedOpen,plainOpen,ratioLine)<<", cover "<<gain(markedOpen,plainOpen,coverEdge)<<std::endl;
 f.setOpen(false);CHECK(cover->getDescription()=="AI proposes changes to 1 set-up control");
 if(images!=juce::File{})
  for(const auto& [name,img]:{std::pair<const char*,const juce::Image*>{"face-marks.png",&marked},{"face-marks-open.png",&markedOpen}}){const auto file=images.getChildFile(name);file.deleteFile();auto out=file.createOutputStream();CHECK(out&&juce::PNGImageFormat().writeImageToStream(*img,*out));}
 // GRIP locked live: no mark, no description, no dashes.
 {auto locked=live;locked.controlLocks[t]=true;f.refresh(locked);
  CHECK(f.marks().size()==2&&f.marks()[0].element=="face.in"&&grip->getDescription().isEmpty()&&gain(snapshot(),plain,gripLine)<0.05);}
 f.refresh(live);CHECK(f.marks().size()==3);
 // The same values: nothing marked, and the face as it was.
 f.showProposal(std::make_shared<const SoundDocument>(live));
 CHECK(f.marks().empty()&&!f.proposesPanel()&&grip->getDescription().isEmpty()&&cover->getDescription().isEmpty()&&maxDiff(snapshot(),plain)==0);
 // GRIP's control relabelled: the face's outline, inset 4 px, and no marks.
 const auto with=[&](const std::function<void(std::vector<RackInstance>&)>& change){auto d=candidate;auto racks=*d.personalities;change(racks.instances);
  d.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));f.showProposal(std::make_shared<const SoundDocument>(d));};
 with([&](auto& all){for(auto& i:all)if(i.id==r.first)i.design.panel.controls[0].label="Hold";});
 CHECK(f.proposesPanel()&&f.marks().empty()&&grip->getDescription().isEmpty());
 CHECK(gain(snapshot(),plain,outline(f.getLocalBounds().toFloat().reduced(4.0f)))>0.35);
 with([&](auto& all){for(auto& i:all)if(i.id==r.first)i.bindings[1].slot=5;});
 CHECK(f.proposesPanel()&&f.marks().empty());
 with([&](auto& all){all.erase(std::remove_if(all.begin(),all.end(),[&](const RackInstance& i){return i.id==r.first;}),all.end());});
 CHECK(!f.proposesPanel()&&f.marks().empty());
 f.showProposal(nullptr);CHECK(!f.proposesPanel()&&f.marks().empty()&&maxDiff(snapshot(),plain)==0);
}
//------------------------------------------------------------------------------ Ember Reed over the real processor
using studio::house::Fader;
using studio::house::Lever;
void writeImages(const juce::File& images,std::initializer_list<std::pair<const char*,const juce::Image*>> list){
 if(images==juce::File{})return;
 for(const auto& [name,img]:list){const auto file=images.getChildFile(name);file.deleteFile();auto out=file.createOutputStream();CHECK(out&&juce::PNGImageFormat().writeImageToStream(*img,*out));}
}
void ember_binds_its_banks(){
 std::cout<<"GROUP ember_binds_its_banks"<<std::endl;EmberRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(0)},std::pair{r.b.get(),std::uint8_t(1)}}){
  CHECK(face->getComponentID()=="rack.face"&&face->getWidth()==2140&&face->getHeight()==591&&face->units()==3);
  CHECK(face->getTitle()=="HALLORAN ELECTRONIC MUSIC CO., ANALOG VOICE MODEL 74");
  const auto bound=[&](juce::Component* c){if(auto* s=dynamic_cast<studio::house::BoundSlider*>(c))return s->element.paramIndex;if(auto* l=dynamic_cast<Lever*>(c))return l->element.paramIndex;CHECK(!"missing control");return ~std::size_t();};
  const auto id=[&](juce::Component* c){const auto i=bound(c);return i<parameterCount?parameterRegistry()[i].id:juce::String();};
  const auto synth="synth."+juce::String(slot+1)+".";
  CHECK(id(face->face.control("air"))==synth+"analog.filter.cutoff"&&id(face->face.control("bite"))==synth+"analog.filter.resonance");
  CHECK(id(face->face.control("bloom"))==synth+"analog.filter_env.decay"&&id(face->face.control("attack"))==synth+"analog.amp.attack"&&id(face->face.control("release"))==synth+"analog.amp.release");
  CHECK(bound(face->face.control("grain"))==r.index(drv,slot,"drive.gain_db"));
  {auto* lamp=face->face.lamp("lamp");CHECK(lamp!=nullptr&&lamp->element.paramIndex<parameterCount&&parameterRegistry()[lamp->element.paramIndex].id==synth+"enabled");}
  auto& deck=face->revealed();
  CHECK(id(deck.control("lfo.rate"))==synth+"lfo.1.rate_hz"&&id(deck.control("vel-air"))==synth+"modulation.1.amount");
  CHECK(id(deck.control("osc1.wave"))==synth+"analog.osc.1.waveform"&&id(deck.control("fenv.mode"))==synth+"analog.filter.mode");
  CHECK(bound(deck.control("drive.shape"))==r.index(drv,slot,"drive.shape")&&bound(deck.control("drive.mix"))==r.index(drv,slot,"drive.mix"));
  CHECK(face->front()!=nullptr&&face->front()->button("programmer")!=nullptr);
 }
}
void ember_gestures(){
 std::cout<<"GROUP ember_gestures"<<std::endl;EmberRig r;if(!r.a)return;
 auto* air=r.a->face.slider<Knob>("air");auto* bloom=r.a->face.slider<Fader>("bloom");CHECK(air&&bloom);if(!air||!bloom)return;
 const auto c1=r.index(analog,0,"analog.filter.cutoff"),c2=r.index(analog,1,"analog.filter.cutoff");
 r.automate(c1,1000.0);const auto other=r.gestures(c2);const auto v2=r.physical(c2);auto g0=r.gestures(c1);
 // AIR's travel is logarithmic from 100 Hz to 10 kHz: 60 px up from 1 kHz is a fifth of it, to 2.51 kHz.
 drag(*air,air->element.pivot,air->element.pivot-Pt(0.0f,60.0f));auto g1=r.gestures(c1);
 CHECK(std::abs(r.physical(c1)-1000.0*std::pow(10.0,0.4))<5.0&&std::abs(air->proportion()-0.7)<0.005&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 CHECK(r.gestures(c2)==other&&r.physical(c2)==v2);
 // BLOOM's cap follows the pointer.
 {const auto d=r.index(analog,0,"analog.filter_env.decay");r.automate(d,0.2);const auto y=bloom->capY();const auto x=bloom->element.capCentreX;const auto b0=r.gestures(d);
  drag(*bloom,{x,y},{x,y-50.0f});const auto b1=r.gestures(d);
  CHECK(std::abs(bloom->capY()-(y-50.0f))<1.0f&&r.physical(d)>0.2&&b1.begins==b0.begins+1&&b1.ends==b0.ends+1);}
 g0=r.gestures(c1);const auto before=r.physical(c1);air->mouseWheelMove(event(*air,local(*air,air->element.pivot),{},0,false),wheel(0.1f));g1=r.gestures(c1);
 CHECK(r.physical(c1)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto p0=air->proportion();air->keyPressed(juce::KeyPress(juce::KeyPress::upKey));g1=r.gestures(c1);
 CHECK(std::abs(air->proportion()-p0-0.01)<0.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;air->mouseDoubleClick(event(*air,local(*air,air->element.pivot),{},2,false));g1=r.gestures(c1);
 CHECK(std::abs(r.physical(c1)-air->element.defaultValue)<1.0&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 // While another gesture holds the processor, a drag changes nothing, shows the parameter again and says why.
 const auto held=r.index(analog,0,"analog.filter.resonance");CHECK(r.p.beginParameterGesture(held));
 const auto v=r.physical(c1);const auto pr=air->proportion();r.reported.clear();
 drag(*air,air->element.pivot,air->element.pivot+Pt(0.0f,60.0f));
 CHECK(r.physical(c1)==v&&std::abs(air->proportion()-pr)<1e-6&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
void ember_pins(){
 std::cout<<"GROUP ember_pins"<<std::endl;EmberRig r;if(!r.a)return;
 auto* air=r.a->face.slider<Knob>("air");auto* attack=r.a->face.slider<Fader>("attack");auto* grain=r.a->face.slider<Knob>("grain");CHECK(air&&attack&&grain);if(!air||!attack||!grain)return;
 const auto c=r.index(analog,0,"analog.filter.cutoff"),at=r.index(analog,0,"analog.amp.attack"),gr=r.index(drv,0,"drive.gain_db");
 const auto g0=r.gestures(c),a0=r.gestures(at),r0=r.gestures(gr);
 r.automate(c,18000.0);r.automate(at,5.0);
 CHECK(std::abs(r.physical(c)-18000.0)<1.0&&std::abs(air->pointerAngle()-air->element.ticks.back().second)<0.01f&&air->readoutText()=="18.0 kHz");
 CHECK(std::abs(r.physical(at)-5.0)<1e-3&&std::abs(attack->capY()-attack->element.y1)<0.01f&&attack->readoutText()=="5.00 s");
 // GRAIN's grip ring turns with its pointer: pinned past its 12 dB stop it draws as at the stop.
 const auto shot=[&]{return grain->createComponentSnapshot(grain->getLocalBounds(),true,1.0f);};
 r.automate(gr,12.0);const auto stop=shot();r.automate(gr,20.0);const auto pinned=shot();r.automate(gr,6.0);const auto mid=shot();
 CHECK(std::abs(r.physical(gr)-6.0)<1e-3&&maxDiff(pinned,stop)==0&&maxDiff(mid,stop)>40);
 r.automate(gr,20.0);CHECK(std::abs(grain->pointerAngle()-grain->element.ticks.back().second)<0.01f&&grain->readoutText()=="+20.0 dB");
 CHECK(r.gestures(c)==g0&&r.gestures(at)==a0&&r.gestures(gr)==r0);
}
void ember_lamp(){
 std::cout<<"GROUP ember_lamp"<<std::endl;EmberRig r;if(!r.a||!r.b)return;
 auto* lamp=r.a->face.lamp("lamp");auto* other=r.b->face.lamp("lamp");CHECK(lamp&&other);if(!lamp||!other)return;
 const auto en=r.index(analog,0,"enabled");
 {bool self=true,children=true;lamp->getInterceptsMouseClicks(self,children);CHECK(!self&&!children&&!lamp->getWantsKeyboardFocus());}
 CHECK(r.physical(en)>0.5&&lamp->lit()&&other->lit());
 r.automate(en,0.0);CHECK(!lamp->lit()&&other->lit());
 r.automate(en,1.0);CHECK(lamp->lit());
}
void ember_selectors_step(){
 std::cout<<"GROUP ember_selectors_step"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;f.setOpen(true);auto& deck=f.revealed();
 auto* wave=deck.lever("osc1.wave");auto* mode=deck.lever("fenv.mode");auto* sync=deck.lever("lfo.sync");auto* shape=deck.lever("drive.shape");
 CHECK(wave&&mode&&sync&&shape);if(!wave||!mode||!sync||!shape)return;
 // Each step one complete gesture, or none where it stays.
 const auto step=[&](Lever& l,auto act,const char* legend,int gestures){const auto i=l.element.paramIndex;const auto g0=r.gestures(i);act();const auto g1=r.gestures(i);
  const auto ok=l.legend()==legend&&g1.begins==g0.begins+gestures&&g1.ends==g0.ends+gestures;if(!ok)std::cout<<"  "<<l.element.id<<" at "<<l.legend()<<", not "<<legend<<std::endl;return ok;};
 const auto click=[&](Lever& l,Pt at){return [&l,at]{clickAt(l,at);};};
 const auto key=[](Lever& l,int code){return [&l,code]{l.keyPressed(juce::KeyPress(code));};};
 const auto roll=[](Lever& l,float dy){return [&l,dy]{l.mouseWheelMove(event(l,local(l,l.element.pivot),{},0,false),wheel(dy));};};
 const auto w=wave->element.paramIndex;
 CHECK(wave->getTitle()=="OSC 1 WAVE"&&wave->element.positions.size()==4);if(wave->element.positions.size()!=4)return;
 // OSC 1 WAVE: a click off its legends steps TRI, SINE, SAW, PULSE and wraps.
 r.automate(w,1.0);CHECK(wave->legend()=="TRI");
 for(const auto* legend:{"SINE","SAW","PULSE","TRI"})CHECK(step(*wave,click(*wave,wave->element.pivot),legend,1));
 // A click on SAW's legend chooses Saw.
 CHECK(step(*wave,click(*wave,wave->element.positions[2].legendRect.getCentre()),"SAW",1)&&juce::roundToInt(r.physical(w))==2);
 {const auto held=wave->legend();studio::noteRackScroll();roll(*wave,0.1f)();CHECK(wave->legend()==held);juce::Thread::sleep(450);}
 // Down and the wheel towards you turn it back, stopping at TRI; Up and the wheel away on, stopping at PULSE.
 r.automate(w,1.0);
 CHECK(step(*wave,key(*wave,juce::KeyPress::downKey),"TRI",0)&&step(*wave,roll(*wave,-0.1f),"TRI",0));
 CHECK(step(*wave,key(*wave,juce::KeyPress::upKey),"SINE",1)&&step(*wave,roll(*wave,0.1f),"SAW",1)&&step(*wave,key(*wave,juce::KeyPress::rightKey),"PULSE",1));
 CHECK(step(*wave,key(*wave,juce::KeyPress::upKey),"PULSE",0)&&step(*wave,key(*wave,juce::KeyPress::leftKey),"SAW",1));
 CHECK(step(*wave,key(*wave,juce::KeyPress::spaceKey),"PULSE",1)&&step(*wave,key(*wave,juce::KeyPress::returnKey),"TRI",1));
 // MODE steps LP, BP, HP by its bat and wraps.
 r.automate(mode->element.paramIndex,mode->element.positions.front().value);CHECK(mode->legend()=="LP");
 for(const auto* legend:{"BP","HP","LP"})CHECK(step(*mode,click(*mode,mode->element.rect.getCentre()),legend,1));
 CHECK(step(*mode,key(*mode,juce::KeyPress::downKey),"BP",1)&&step(*mode,key(*mode,juce::KeyPress::downKey),"HP",1)&&step(*mode,key(*mode,juce::KeyPress::downKey),"HP",0));
 // LFO FREE / SYNC and DRIVE SHAPE flip.
 const auto flip=[&](Lever& l){const auto from=l.legend(),to=l.element.positions[1].legend==from?l.element.positions[0].legend:l.element.positions[1].legend;
  return step(l,click(l,l.element.rect.getCentre()),to.toRawUTF8(),1)&&step(l,click(l,l.element.rect.getCentre()),from.toRawUTF8(),1);};
 CHECK(sync->element.positions.size()==2&&shape->element.positions.size()==2&&flip(*sync)&&flip(*shape));
}
void ember_drawer(){
 std::cout<<"GROUP ember_drawer"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;
 auto* front=f.front();auto* pull=front?front->button("programmer"):nullptr;CHECK(pull!=nullptr);if(!pull)return;
 CHECK(pull->getTitle()=="Open programmer"&&!f.isOpen()&&!f.revealed().isVisible()&&f.units()==3&&f.getHeight()==591&&front->getY()==394);
 const juce::StringArray closed{"face.air","face.bite","face.bloom","face.attack","face.release","face.grain","face.programmer"};
 juce::StringArray open{"face.air","face.bite","face.bloom","face.attack","face.release","face.grain"};
 if(const auto* g=group(f.skin.skin,"face.deck"))open.addArray(g->focusOrder);
 open.add("face.programmer");CHECK(open.size()==31);
 CHECK(tabOrder(f)==closed);if(tabOrder(f)!=closed)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 const auto rev=r.revision();const auto values=r.document().values;int opened=0;f.onOpen=[&]{++opened;};
 clickAt(*pull,pull->element.rect.getCentre());
 CHECK(f.isOpen()&&f.revealed().isVisible()&&f.units()==7&&f.getWidth()==2140&&f.getHeight()==1379&&opened==1);
 CHECK(f.face.getPosition()==juce::Point<int>()&&f.revealed().getY()==394&&front->getY()==1182&&pull->getTitle()=="Close programmer");
 CHECK(!f.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))&&f.isOpen()&&r.revision()==rev&&r.document().values==values);
 CHECK(tabOrder(f)==open);if(tabOrder(f)!=open)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 // The pull closes it again.
 clickAt(*pull,pull->element.rect.getCentre());
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&f.units()==3&&f.getHeight()==591&&front->getY()==394&&pull->getTitle()=="Open programmer"&&opened==2&&tabOrder(f)==closed);
 // Closing with focus in the deck hands it to the pull; with focus on the face it stays.
 pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(f.isOpen());
 auto* tune=f.revealed().slider<Knob>("osc1.tune");CHECK(tune!=nullptr);if(!tune)return;
 tune->grabKeyboardFocus();CHECK(tune->hasKeyboardFocus(false));
 pull->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(!f.isOpen()&&juce::Component::getCurrentlyFocusedComponent()==pull);
 auto* air=f.face.slider<Knob>("air");air->grabKeyboardFocus();f.setOpen(true);f.setOpen(false);CHECK(air->hasKeyboardFocus(false));
 // Opening and closing wrote nothing.
 CHECK(r.revision()==rev&&r.document().values==values);
 // The deck's readouts as printed.
 f.setOpen(true);
 const auto text=[&](const char* id,const char* key,double value){r.automate(r.index(analog,0,key),value);auto* k=f.revealed().slider<Knob>(id);return k?k->readoutText():juce::String("missing");};
 CHECK(text("osc1.tune","analog.osc.1.tune",7.0)=="+7 st"&&text("osc1.fine","analog.osc.1.detune",-12.0)=="-12 ct"&&text("fenv.amount","analog.filter_env.octaves",2.0)=="+2.0 oct");
 CHECK(text("osc1.tune","analog.osc.1.tune",-12.0)=="-12 st"&&text("fenv.amount","analog.filter_env.octaves",-0.5)=="-0.5 oct");
 // Hz to three figures as the PROGRAMMER gives them, so LFO RATE's slow end does not read 0 Hz.
 CHECK(text("lfo.rate","lfo.1.rate_hz",0.05)=="0.05 Hz"&&text("lfo.rate","lfo.1.rate_hz",3.0)=="3.00 Hz"&&text("lfo.rate","lfo.1.rate_hz",13.226)=="13.2 Hz"
       &&text("lfo.rate","lfo.1.rate_hz",40.0)=="40.0 Hz");
}
void ember_compose(const juce::File& images){
 std::cout<<"GROUP ember_compose"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;
 for(auto* v:{&f.face,&f.revealed()}){for(auto& s:v->sliders)r.automate(s->element.paramIndex,s->element.drawnValue);for(auto& l:v->levers)r.automate(l->element.paramIndex,l->element.drawnValue);}
 pump();ParameterValues values{};for(std::size_t i=0;i<values.size();++i)values[i]=r.p.control(i).getValue();
 f.face.hideReadouts();f.revealed().hideReadouts();
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto closed=f.compose(values);const auto live=snapshot();
 CHECK(closed.getWidth()==2140&&closed.getHeight()==591);
 {const auto d=maxDiff(closed,live);CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live faceplate by "<<d<<std::endl;}
 f.setOpen(true);f.revealed().hideReadouts();
 const auto open=f.compose(values,true);const auto liveOpen=snapshot();
 CHECK(open.getWidth()==2140&&open.getHeight()==1379&&maxDiff(f.compose(values),closed)==0);
 {const auto d=maxDiff(open,liveOpen);CHECK(d<=1);if(d>1)std::cout<<"  the open compose differs from the live faceplate by "<<d<<std::endl;}
 // Other values turn the knobs and their grip rings, slide the caps and step the selectors.
 {auto other=values;other[r.index(analog,0,"analog.filter.cutoff")]=0.9f;CHECK(maxDiff(f.compose(other),closed)>40);}
 {auto other=values;other[r.index(analog,0,"analog.amp.attack")]=0.5f;CHECK(maxDiff(f.compose(other),closed)>40);}
 {auto other=values;other[r.index(analog,0,"analog.osc.1.waveform")]=0.0f;CHECK(maxDiff(f.compose(other,true),open)>40&&maxDiff(f.compose(other),closed)==0);}
 writeImages(images,{{"ember-reed-drawn.png",&closed},{"ember-reed-open-drawn.png",&open}});
}
// The largest channel difference inside `r`, or outside it.
int maxDiffIn(const juce::Image& a,const juce::Image& b,juce::Rectangle<int> r){return maxDiff(a.getClippedImage(r),b.getClippedImage(r));}
int maxDiffOutside(const juce::Image& a,const juce::Image& b,juce::Rectangle<int> r){
 auto x=a.createCopy(),y=b.createCopy();for(auto* i:{&x,&y}){juce::Graphics g(*i);g.setColour(juce::Colours::black);g.fillRect(r);}return maxDiff(x,y);}
// VEL TO AIR stays true: while route 1 is not an enabled route from velocity to the analog bank's cutoff, the open deck
// tapes over its legend with the route's name, and the knob is titled with it.
void ember_vel_air(const juce::File& images){
 std::cout<<"GROUP ember_vel_air"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);
 auto* knob=f.revealed().slider<Knob>("vel-air");CHECK(knob!=nullptr&&!knob->element.tapeRect.isEmpty());if(!knob)return;
 const auto route=[&](const char* field){return r.index(analog,0,(juce::String("modulation.1.")+field).toRawUTF8());};
 const auto choice=[&](const char* field){return juce::roundToInt(r.physical(route(field)));};
 // Ember Reed's own route 1: velocity to the analog bank's cutoff, enabled.
 CHECK(choice("enabled")==1&&choice("source")==5&&choice("target")==14&&choice("bank")==1);
 // On the CPU: a Direct2D snapshot can shift a pixel elsewhere once the tape's images have been drawn.
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
 f.refresh(r.document());const auto plainClosed=snapshot();f.setOpen(true);const auto plain=snapshot();
 CHECK(f.routeTape().isEmpty()&&knob->getTitle()=="MOTION VEL TO AIR");
 const auto tapeRect=(knob->element.tapeRect+f.revealed().getPosition().toFloat()).getSmallestIntegerContainer().expanded(6);
 const auto shown=[&](const char* text,const char* title){f.refresh(r.document());const auto s=snapshot();
  const auto ok=f.routeTape()==text&&knob->getTitle()==title&&maxDiffIn(s,plain,tapeRect)>40&&maxDiffOutside(s,plain,tapeRect)==0;
  if(!ok)std::cout<<"  tape '"<<f.routeTape()<<"', title '"<<knob->getTitle()<<"', inside "<<maxDiffIn(s,plain,tapeRect)<<", outside "<<maxDiffOutside(s,plain,tapeRect)<<std::endl;
  return ok;};
 r.automate(route("source"),3.0);CHECK(shown("LFO 1 TO CUTOFF","LFO 1 to Cutoff"));const auto lfo=snapshot();
 r.automate(route("source"),5.0);r.automate(route("bank"),2.0);CHECK(shown("VELOCITY TO WAVETABLE CUTOFF","Velocity to Wavetable Cutoff"));
 r.automate(route("bank"),1.0);r.automate(route("enabled"),0.0);CHECK(shown("ROUTE 1 OFF","Route 1 off"));const auto off=snapshot();
 writeImages(images,{{"ember-reed-tape.png",&lfo},{"ember-reed-tape-off.png",&off}});
 // Closed, no tape is painted.
 f.setOpen(false);CHECK(maxDiff(snapshot(),plainClosed)==0);f.setOpen(true);
 // The knob still writes route 1's amount.
 {const auto amount=route("amount");const auto g0=r.gestures(amount);const auto before=r.physical(amount);knob->keyPressed(juce::KeyPress(juce::KeyPress::upKey));const auto g1=r.gestures(amount);
  CHECK(knob->element.paramIndex==amount&&r.physical(amount)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);}
 // Velocity to cutoff again: no tape, the printed title.
 r.automate(route("enabled"),1.0);f.refresh(r.document());CHECK(f.routeTape().isEmpty()&&knob->getTitle()=="MOTION VEL TO AIR");
 CHECK(maxDiffIn(snapshot(),plain,tapeRect.withTrimmedTop(tapeRect.getHeight()/2))==0);
}
// The AI's marks over every view: a knob's or selector's dashed pointer, a fader's dashed cap, a lever's dashed
// outline; closed, the deck's marks wait behind the pull's outline and its count; a locked control gets its dot and no
// mark; another panel outlines the whole faceplate.
void ember_marks(const juce::File& images){
 std::cout<<"GROUP ember_marks"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* air=f.face.slider<Knob>("air");auto* attack=f.face.slider<Fader>("attack");auto* wave=f.revealed().lever("osc1.wave");auto* mode=f.revealed().lever("fenv.mode");
 auto* rate=f.revealed().slider<Knob>("lfo.rate");auto* pull=f.front()?f.front()->button("programmer"):nullptr;
 CHECK(air&&attack&&wave&&mode&&rate&&pull);if(!air||!attack||!wave||!mode||!rate||!pull)return;
 const auto cut=air->element.paramIndex,at=attack->element.paramIndex,w=wave->element.paramIndex,m=mode->element.paramIndex,lr=rate->element.paramIndex;
 r.automate(cut,1000.0);r.automate(at,0.005);r.automate(w,1.0);r.automate(m,0.0);r.automate(lr,1.0);
 const auto live=r.document();auto candidate=live;
 const auto set=[&](SoundDocument& d,std::size_t i,double v){d.values[i]=r.p.control(i).convertTo0to1((float)v);};
 set(candidate,cut,2000.0);set(candidate,at,0.05);set(candidate,w,2.0);set(candidate,m,1.0);set(candidate,lr,3.0);
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(live);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto plain=snapshot();f.setOpen(true);const auto plainOpen=snapshot();f.setOpen(false);
 f.showProposal(std::make_shared<const SoundDocument>(candidate));
 {const auto marks=f.marks();juce::StringArray ids;for(const auto& k:marks)ids.add(k.element);
  CHECK(ids==juce::StringArray({"face.air","face.attack","face.osc1.wave","face.fenv.mode","face.lfo.rate"})&&!f.proposesPanel());}
 CHECK(air->getDescription()=="AI proposes 2.0 kHz"&&attack->getDescription()=="AI proposes 50 ms"&&wave->getDescription()=="AI proposes SAW"&&mode->getDescription()=="AI proposes BP");
 CHECK(pull->getDescription()=="AI proposes changes to 3 programmer controls");
 const auto gain=[](const juce::Image& a,const juce::Image& b,const juce::Path& p){return whiteAlong(a,p)-whiteAlong(b,p);};
 const auto pointer=[&](const studio::house::Element& e,float degrees){const auto a=juce::degreesToRadians(degrees);const auto c=e.pivot+f.revealed().getPosition().toFloat()*(e.view=="deck"?1.0f:0.0f);
  return line(c+Pt(std::sin(a),-std::cos(a))*e.faceR*0.35f,c+Pt(std::sin(a),-std::cos(a))*e.faceR*0.95f);};
 const auto airLine=pointer(air->element,house::pointerAngleFor(air->element,house::proportionOf(air->element,2000.0)));
 const auto& cap=f.skin.skin.asset(attack->element.layer("cap"));
 const auto capEdge=outline(juce::Rectangle<float>((float)cap.image.getWidth(),(float)cap.image.getHeight()).withPosition(attack->element.capCentreX-cap.anchor.x,Fader::capYAt(attack->element,house::proportionOf(attack->element,0.05))-cap.anchor.y));
 const auto pullEdge=[&]{return outline(pull->element.rect.expanded(3.0f)+f.front()->getPosition().toFloat());};
 const auto marked=snapshot();const auto closedPull=pullEdge();
 CHECK(gain(marked,plain,airLine)>0.35&&gain(marked,plain,capEdge)>0.35&&gain(marked,plain,closedPull)>0.35);
 std::cout<<"  closed: AIR "<<gain(marked,plain,airLine)<<", ATTACK "<<gain(marked,plain,capEdge)<<", pull "<<gain(marked,plain,closedPull)<<std::endl;
 // Open: WAVE's pointer at SAW's angle and MODE's outline; the pull is not outlined and says nothing.
 f.setOpen(true);const auto markedOpen=snapshot();
 const auto waveLine=pointer(wave->element,wave->element.positions[2].angle);
 const auto modeEdge=outline(mode->element.rect.expanded(3.0f)+f.revealed().getPosition().toFloat());
 CHECK(gain(markedOpen,plainOpen,waveLine)>0.35&&gain(markedOpen,plainOpen,modeEdge)>0.35&&gain(markedOpen,plainOpen,pullEdge())<0.05&&pull->getDescription().isEmpty());
 std::cout<<"  open: WAVE "<<gain(markedOpen,plainOpen,waveLine)<<", MODE "<<gain(markedOpen,plainOpen,modeEdge)<<", pull "<<gain(markedOpen,plainOpen,pullEdge())<<std::endl;
 writeImages(images,{{"ember-reed-marks.png",&marked},{"ember-reed-marks-open.png",&markedOpen}});
 f.setOpen(false);CHECK(pull->getDescription()=="AI proposes changes to 3 programmer controls");
 // AIR and LFO RATE locked live: no marks, AIR's dot, and LFO RATE's only while open.
 {auto locked=live;locked.controlLocks[cut]=locked.controlLocks[lr]=true;f.refresh(locked);
  juce::StringArray ids;for(const auto& k:f.marks())ids.add(k.element);
  CHECK(ids==juce::StringArray({"face.attack","face.osc1.wave","face.fenv.mode"})&&air->getDescription().isEmpty()&&pull->getDescription()=="AI proposes changes to 2 programmer controls");
  const auto around=[](Pt p){return juce::Rectangle<int>(24,24).withCentre(p.roundToInt());};
  const auto closedLocked=snapshot();
  CHECK(gain(closedLocked,plain,airLine)<0.05&&maxDiffIn(closedLocked,marked,around(air->element.dot))>40);
  f.setOpen(true);const auto openLocked=snapshot();const auto rateDot=rate->element.dot+f.revealed().getPosition().toFloat();
  CHECK(maxDiffIn(openLocked,markedOpen,around(rateDot))>40);
  f.setOpen(false);f.refresh(live);}
 // A focused control relabelled: the whole faceplate's outline, closed and open, and no marks.
 {auto d=candidate;auto racks=*d.personalities;for(auto& i:racks.instances)if(i.id==r.first)i.design.panel.controls[0].label="Sky";
  d.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));f.showProposal(std::make_shared<const SoundDocument>(d));}
 CHECK(f.proposesPanel()&&f.marks().empty()&&pull->getDescription().isEmpty());
 CHECK(gain(snapshot(),plain,outline(f.getLocalBounds().toFloat().reduced(4.0f)))>0.35);
 f.setOpen(true);CHECK(gain(snapshot(),plainOpen,outline(f.getLocalBounds().toFloat().reduced(4.0f)))>0.35);
 f.showProposal(nullptr);CHECK(!f.proposesPanel()&&f.marks().empty()&&maxDiff(snapshot(),plainOpen)==0);
}
//------------------------------------------------------------------------------ Glass Current over the real processor
// Two Glass Currents: the first's wavetable unit in synth slot 1, its delay in effect bank 3 and its reverb in bank 4;
// the second's in slot 2 and banks 5 and 6.
struct GlassRig:Rig{GlassRig(){first=place(1,{RackRole::synth,4,0},{{0,0},{1,2},{2,3}},true);second=place(1,{RackRole::synth,4,1},{{0,1},{1,4},{2,5}},true);show(400,800);}};
constexpr auto wt=NativeModule::wavetable,dly=NativeModule::delay,rvb=NativeModule::reverb;
ParameterValues liveValues(Rig& r){ParameterValues v{};for(std::size_t i=0;i<v.size();++i)v[i]=r.p.control(i).getValue();return v;}
void glass_binds_its_banks(){
 std::cout<<"GROUP glass_binds_its_banks"<<std::endl;GlassRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(0)},std::pair{r.b.get(),std::uint8_t(1)}}){
  CHECK(face->getComponentID()=="rack.face"&&face->getWidth()==2140&&face->getHeight()==394&&face->units()==2&&!face->isOpen());
  CHECK(face->getTitle()=="KUROSHIO, GLASS CURRENT GC-86 DIGITAL WAVETABLE SYNTHESIZER");
  const auto knob=[&](const char* id){auto* k=face->face.slider<Knob>(id);CHECK(k!=nullptr);return k?k->element.paramIndex:~std::size_t();};
  const auto delayBank=std::uint8_t(2+2*slot),reverbBank=std::uint8_t(3+2*slot);
  CHECK(knob("facet")==r.index(wt,slot,"wavetable.osc.1.position")&&knob("drift")==r.index(wt,slot,"lfo.1.rate_hz"));
  CHECK(knob("echo")==r.index(dly,delayBank,"delay.mix")&&knob("horizon")==r.index(rvb,reverbBank,"reverb.decay_seconds"));
  CHECK(parameterRegistry()[r.index(wt,slot,"wavetable.osc.1.position")].id=="synth."+juce::String(slot+1)+".wavetable.osc.1.position");
  {auto* lamp=face->face.lamp("power");CHECK(lamp!=nullptr&&lamp->element.paramIndex==r.index(wt,slot,"enabled"));}
  if(auto* facet=face->face.slider<Knob>("facet"))CHECK(&facet->param==&r.p.control(r.index(wt,slot,"wavetable.osc.1.position")));
 }
 // A rack whose reverb has no bank gets no faceplate.
 {auto broken=r.instance(r.first);broken.bindings.pop_back();CHECK(studio::MakerFace::forRack(r.p,studio::makerFaceFor(broken.design),broken,r.host())==nullptr);}
}
void glass_gestures(){
 std::cout<<"GROUP glass_gestures"<<std::endl;GlassRig r;if(!r.a)return;
 auto* facet=r.a->face.slider<Knob>("facet");CHECK(facet!=nullptr);if(!facet)return;
 const auto p1=r.index(wt,0,"wavetable.osc.1.position"),p2=r.index(wt,1,"wavetable.osc.1.position");
 r.automate(p1,.35);const auto other=r.gestures(p2);const auto v2=r.physical(p2);auto g0=r.gestures(p1);
 // FACET turns from 0 to 1 over 300 px: 60 px up is 0.2.
 drag(*facet,facet->element.pivot,facet->element.pivot-Pt(0.0f,60.0f));auto g1=r.gestures(p1);
 CHECK(std::abs(r.physical(p1)-.55)<.005&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 CHECK(r.gestures(p2)==other&&r.physical(p2)==v2);
 g0=g1;const auto before=r.physical(p1);facet->mouseWheelMove(event(*facet,local(*facet,facet->element.pivot),{},0,false),wheel(0.1f));g1=r.gestures(p1);
 CHECK(r.physical(p1)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto q0=facet->proportion();facet->keyPressed(juce::KeyPress(juce::KeyPress::upKey));g1=r.gestures(p1);
 CHECK(std::abs(facet->proportion()-q0-0.01)<0.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;facet->mouseDoubleClick(event(*facet,local(*facet,facet->element.pivot),{},2,false));g1=r.gestures(p1);
 CHECK(std::abs(r.physical(p1)-facet->element.defaultValue)<1e-3&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 // While another gesture holds the processor, a drag changes nothing, shows the parameter again and says why.
 const auto held=r.index(wt,0,"lfo.1.rate_hz");CHECK(r.p.beginParameterGesture(held));
 r.automate(p1,.5);const auto v=r.physical(p1);const auto pr=facet->proportion();r.reported.clear();
 drag(*facet,facet->element.pivot,facet->element.pivot+Pt(0.0f,60.0f));
 CHECK(r.physical(p1)==v&&std::abs(facet->proportion()-pr)<1e-6&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
void glass_pins(){
 std::cout<<"GROUP glass_pins"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;
 auto* drift=f.face.slider<Knob>("drift");CHECK(drift!=nullptr);if(!drift)return;
 const auto d=r.index(wt,0,"lfo.1.rate_hz");const auto g0=r.gestures(d);
 r.automate(d,20.0);
 CHECK(std::abs(r.physical(d)-20.0)<1e-3&&std::abs(drift->pointerAngle()-drift->element.ticks.back().second)<.01f);
 // Automation touches nothing; touching DRIFT shows its true value over a full bar.
 CHECK(f.vfd().line1.startsWith("FACET "));
 drift->grabKeyboardFocus();
 CHECK(f.vfd()==(studio::VfdText{"DRIFT 20.0Hz","",15})&&r.gestures(d)==g0);
 if(f.vfd().line1!="DRIFT 20.0Hz")std::cout<<"  the VFD reads "<<f.vfd().line1<<std::endl;
}
void glass_vfd(){
 std::cout<<"GROUP glass_vfd"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;
 auto* facet=f.face.slider<Knob>("facet");auto* drift=f.face.slider<Knob>("drift");auto* echo=f.face.slider<Knob>("echo");auto* horizon=f.face.slider<Knob>("horizon");
 const auto* vfd=element(f.face.ctx.model,"face.vfd");
 CHECK(facet&&drift&&echo&&horizon&&vfd);if(!facet||!drift||!echo||!horizon||!vfd)return;
 const auto fa=facet->element.paramIndex,dr=drift->element.paramIndex,ec=echo->element.paramIndex,ho=horizon->element.paramIndex;
 // At the brief's values FACET shows first, over the bar with ceil(0.35 x 15) = 6 segments lit as drawn, where
 // round would light 5.
 r.automate(fa,.35);r.automate(dr,.35);r.automate(ec,.15);r.automate(ho,3.0);pump();
 CHECK(f.vfd()==(studio::VfdText{"FACET 0.35","",6}));
 // A face knob's value reads as the VFD writes it.
 {auto* h=horizon->getAccessibilityHandler();const auto* v=h?h->getValueInterface():nullptr;CHECK(v!=nullptr&&v->getCurrentValueAsString()=="3.00s");}
 const auto glass=vfd->rect.getSmallestIntegerContainer();
 const auto shot=[&]{return f.createComponentSnapshot(glass,true,1.0f);};
 const auto atFacet=shot();
 // A drag on ECHO from 15 % to 25 %, a fifth of its travel, shows it with its bar lit to half.
 drag(*echo,echo->element.pivot,echo->element.pivot-Pt(0.0f,60.0f));
 CHECK(std::abs(r.physical(ec)-.25)<.005&&f.vfd()==(studio::VfdText{"ECHO 25%","",8}));
 CHECK(maxDiff(shot(),atFacet)>40&&f.face.readoutBounds().isEmpty());
 // Tab from ECHO to HORIZON.
 echo->grabKeyboardFocus();echo->moveKeyboardFocusToSibling(true);
 CHECK(horizon->hasKeyboardFocus(false)&&f.vfd().line1=="HORIZON 3.00s"&&f.face.readoutBounds().isEmpty());
 // Automation of DRIFT while FACET was last touched leaves FACET, which follows its own automation.
 facet->grabKeyboardFocus();CHECK(f.vfd().line1=="FACET 0.35");
 r.automate(dr,2.0);CHECK(f.vfd().line1=="FACET 0.35");
 r.automate(fa,.5);CHECK(f.vfd()==(studio::VfdText{"FACET 0.50","",8}));
 // A hover touches nothing, and the house readout never shows.
 echo->mouseEnter(event(*echo,local(*echo,echo->element.pivot),{},0,false));
 CHECK(f.vfd().line1=="FACET 0.50"&&f.face.readoutBounds().isEmpty());
 echo->mouseExit(event(*echo,local(*echo,echo->element.pivot),{},0,false));
 // The live VFD draws as compose does.
 pump();CHECK(maxDiff(shot(),f.compose(liveValues(r)).getClippedImage(glass))<=1);
}
void glass_vfd_words(){
 std::cout<<"GROUP glass_vfd_words"<<std::endl;GlassRig r;if(!r.a)return;const auto& m=r.a->face.ctx.model;
 auto values=liveValues(r);
 const auto at=[&](const char* id)->const house::Element&{const auto* e=element(m,id);CHECK(e!=nullptr);return e?*e:m.elements.front();};
 const auto set=[&](const char* id,double v){const auto i=at(id).paramIndex;values[i]=r.p.control(i).convertTo0to1((float)v);};
 // The VFD's lines hold 25 cells each.
 const auto lines=[&](const char* id,int cells1=25,int cells2=25){return studio::vfdLines(m,values,at(id),cells1,cells2);};
 for(const auto& [id,v]:std::vector<std::pair<const char*,double>>{{"face.delay.time",.3},{"face.delay.fdbk",.2},{"face.delay.mix",.15},{"face.delay.sync",0},
                                                                    {"face.osc1.tune",7},{"face.osc1.fine",-12},{"face.fenv.amount",2},{"face.lfo.shape",1}})set(id,v);
 // The drawn line; with no room for it the name drops its first word and keeps the value.
 CHECK(lines("face.delay.time").line1=="DELAY TIME 300ms"&&lines("face.delay.time",16).line1=="DELAY TIME 300ms"&&lines("face.delay.time",15).line1=="TIME 300ms");
 CHECK(lines("face.delay.fdbk").line1=="DELAY FDBK 20%"&&lines("face.osc1.tune").line1=="OSC 1 TUNE +7st"&&lines("face.osc1.fine").line1=="OSC 1 FINE -12ct");
 CHECK(lines("face.fenv.amount").line1=="FENV AMOUNT +2.0oct"&&lines("face.lfo.shape").line1=="LFO SHAPE TRIANGLE"&&lines("face.lfo.shape",15).line1=="SHAPE TRIANGLE");
 CHECK(lines("face.delay.sync").line1=="DELAY SYNC OFF");
 // Line 2: the screen's other shown controls that are not choices, in slot order, as many as fit; SYNC is a choice.
 CHECK(lines("face.delay.time")==(studio::VfdText{"DELAY TIME 300ms","FDBK 20% MIX 15%",-1}));
 CHECK(lines("face.delay.time",25,15).line2=="FDBK 20%"&&lines("face.delay.time",25,7).line2.isEmpty());
 CHECK(lines("face.delay.fdbk").line2=="TIME 300ms MIX 15%"&&lines("face.delay.sync").line2=="TIME 300ms FDBK 20%"&&lines("face.delay.sync",25,27).line2=="TIME 300ms FDBK 20% MIX 15%");
 // With SYNC on, BEATS stands in TIME's slot.
 set("face.delay.sync",1);
 CHECK(lines("face.delay.sync").line1=="DELAY SYNC ON"&&lines("face.delay.fdbk").line2.startsWith("TIME ")&&!lines("face.delay.fdbk").line2.contains("300ms"));
 // A face knob's line 2 is its bar, lit for the first ceil(15 p) segments at its pointer's proportion p, pinned at
 // its stops.
 for(const auto& [v,lit]:std::vector<std::pair<double,int>>{{0.0,0},{.01,1},{.25,8},{.5,15},{.8,15}}){set("face.echo",v);const auto t=lines("face.echo");CHECK(t.line2.isEmpty()&&t.lit==lit);}
}
void glass_lamp(){
 std::cout<<"GROUP glass_lamp"<<std::endl;GlassRig r;if(!r.a||!r.b)return;
 auto* lamp=r.a->face.lamp("power");auto* other=r.b->face.lamp("power");CHECK(lamp&&other);if(!lamp||!other)return;
 const auto en=r.index(wt,0,"enabled");
 {bool self=true,children=true;lamp->getInterceptsMouseClicks(self,children);CHECK(!self&&!children&&!lamp->getWantsKeyboardFocus());}
 CHECK(r.physical(en)>.5&&lamp->lit()&&other->lit());
 r.automate(en,0.0);CHECK(!lamp->lit()&&other->lit());
 r.automate(en,1.0);CHECK(lamp->lit());
}
void glass_compose(const juce::File& images){
 std::cout<<"GROUP glass_compose"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;
 const auto* vfd=element(f.face.ctx.model,"face.vfd");CHECK(vfd!=nullptr);if(!vfd)return;
 for(auto& s:f.face.sliders)r.automate(s->element.paramIndex,s->element.drawnValue);
 pump();const auto values=liveValues(r);f.face.hideReadouts();
 const auto closed=f.compose(values);const auto live=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(closed.getWidth()==2140&&closed.getHeight()==394);
 {const auto d=maxDiff(closed,live);CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live faceplate by "<<d<<std::endl;}
 // At FACET's drawn 0.381 the VFD reads FACET 0.38 over 6 lit segments.
 CHECK(f.vfd()==(studio::VfdText{"FACET 0.38","",6}));
 const auto glass=vfd->rect.getSmallestIntegerContainer();
 // FACET turns its pointer and letters the VFD again; DRIFT turns only its own; POWER unlit darkens its LED.
 {auto other=values;other[r.index(wt,0,"wavetable.osc.1.position")]=.9f;const auto o=f.compose(other);CHECK(maxDiffIn(o,closed,glass)>40&&maxDiffOutside(o,closed,glass)>40);}
 {auto other=values;other[r.index(wt,0,"lfo.1.rate_hz")]=.9f;const auto o=f.compose(other);CHECK(maxDiffIn(o,closed,glass)==0&&maxDiffOutside(o,closed,glass)>40);}
 {auto other=values;other[r.index(wt,0,"enabled")]=0.0f;CHECK(maxDiff(f.compose(other),closed)>40);}
 writeImages(images,{{"glass-current-drawn.png",&closed}});
}
// Presses a page button on the open door.
void choose(studio::MakerFace& f,const char* page){if(auto* b=f.revealed().button(juce::String("page.")+page))clickAt(*b,b->element.rect.getCentre());else CHECK(!"no such page");}
juce::StringArray shownItems(studio::MakerFace& f){juce::StringArray out;for(auto& s:f.revealed().sliders)if(s->isVisible())out.add(s->element.id);return out;}
void glass_door(){
 std::cout<<"GROUP glass_door"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;
 auto* edit=f.face.button("edit");auto* hang=f.hanging();auto* close=hang?hang->button("close"):nullptr;
 CHECK(edit&&hang&&close);if(!edit||!hang||!close)return;
 CHECK(edit->getTitle()=="Open edit door"&&close->getTitle()=="Close edit door");
 const juce::StringArray closed{"face.facet","face.drift","face.echo","face.horizon","face.edit"};
 const juce::StringArray open{"face.facet","face.drift","face.echo","face.horizon","face.page.osc","face.osc1.table","face.osc1.pos","face.osc1.tune","face.osc1.fine","face.close"};
 CHECK(!f.isOpen()&&f.units()==2&&f.getHeight()==394&&!f.revealed().isVisible()&&!hang->isVisible()&&f.openedArea().isEmpty());
 CHECK(tabOrder(f)==closed);if(tabOrder(f)!=closed)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 const auto rev=r.revision();const auto values=r.document().values;int opened=0;f.onOpen=[&]{++opened;};
 // The top notch opens the door below the face, the panel over the face's right quarter; the notch leaves Tab order.
 clickAt(*edit,edit->element.rect.getCentre());
 CHECK(f.isOpen()&&f.units()==4&&f.getWidth()==2140&&f.getHeight()==788&&opened==1&&f.face.getPosition()==juce::Point<int>()&&f.revealed().isVisible()&&hang->isVisible());
 CHECK(!f.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))&&f.isOpen()&&r.revision()==rev&&r.document().values==values);
 {const auto& door=f.face.ctx.model.asset("door:plate");
  CHECK(f.openedArea()==juce::Rectangle<int>(juce::roundToInt(door.anchor.x),juce::roundToInt(door.anchor.y),door.image.getWidth(),door.image.getHeight())&&f.openedArea().getY()==394);}
 CHECK(tabOrder(f)==open);if(tabOrder(f)!=open)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 // The foot notch closes it.
 clickAt(*close,close->element.rect.getCentre());
 CHECK(!f.isOpen()&&f.units()==2&&f.getHeight()==394&&opened==2&&!hang->isVisible()&&f.openedArea().isEmpty()&&tabOrder(f)==closed);
 // From the keyboard: opening moves the focus to the foot notch, closing with the focus in the door to the top notch.
 edit->grabKeyboardFocus();edit->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(f.isOpen()&&juce::Component::getCurrentlyFocusedComponent()==close);
 close->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
 CHECK(!f.isOpen()&&juce::Component::getCurrentlyFocusedComponent()==edit);
 f.setOpen(true);
 if(auto* fine=f.revealed().slider<Knob>("osc1.fine")){fine->grabKeyboardFocus();CHECK(fine->hasKeyboardFocus(false));f.setOpen(false);CHECK(juce::Component::getCurrentlyFocusedComponent()==edit);}
 else CHECK(!"no FINE");
 // Focus on the face stays where it is.
 if(auto* facet=f.face.slider<Knob>("facet")){facet->grabKeyboardFocus();f.setOpen(true);CHECK(facet->hasKeyboardFocus(false));f.setOpen(false);CHECK(facet->hasKeyboardFocus(false));}
 // Opening and closing wrote nothing.
 CHECK(r.revision()==rev&&r.document().values==values);
}
void glass_pages(){
 std::cout<<"GROUP glass_pages"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;auto& panel=f.revealed();
 r.automate(r.index(dly,2,"delay.seconds"),.3);r.automate(r.index(dly,2,"delay.feedback"),.2);r.automate(r.index(dly,2,"delay.mix"),.15);r.automate(r.index(dly,2,"delay.synced"),0.0);
 const auto rev=r.revision();const auto values=r.document().values;
 f.setOpen(true);
 const juce::StringArray pages{"osc","filter","env","lfo","delay","reverb"};
 const auto button=[&](const juce::String& page){return panel.button("page."+page);};
 for(const auto& p:pages)CHECK(button(p)!=nullptr);
 const auto lit=[&]{juce::StringArray out;for(const auto& p:pages)if(auto* b=button(p);b&&b->lit())out.add(p);return out;};
 const auto still=[&](int slot){auto* c=panel.findChildWithID("panel.still."+juce::String(slot));CHECK(c!=nullptr);return c&&c->isVisible();};
 // OSC's first screen, its four items at slots 1 to 4 and no spare showing.
 CHECK(f.page()=="osc"&&f.screen()=="osc.1"&&lit()==juce::StringArray({"osc"}));
 CHECK(shownItems(f)==juce::StringArray({"face.osc1.table","face.osc1.pos","face.osc1.tune","face.osc1.fine"})&&f.lcd()==juce::StringArray({"TABLE","POS","TUNE","FINE"}));
 if(f.lcd()!=juce::StringArray({"TABLE","POS","TUNE","FINE"}))std::cout<<"  the LCD reads "<<f.lcd().joinIntoString("|")<<std::endl;
 CHECK(!still(1)&&!still(2)&&!still(3)&&!still(4));
 // Pressing OSC steps through its screens and wraps; the mix screen's fourth knob is a spare under a blank column.
 auto* osc=button("osc");clickAt(*osc,osc->element.rect.getCentre());
 CHECK(f.screen()=="osc.2"&&shownItems(f)==juce::StringArray({"face.osc2.table","face.osc2.pos","face.osc2.tune","face.osc2.fine"}));
 clickAt(*osc,osc->element.rect.getCentre());
 CHECK(f.screen()=="osc.3"&&shownItems(f)==juce::StringArray({"face.osc1.level","face.osc2.level","face.noise"})&&f.lcd()==juce::StringArray({"LEVEL","LEVEL","NOISE",""}));
 CHECK(!still(1)&&!still(3)&&still(4)&&f.vfd().line1.startsWith("OSC 1 LEVEL "));
 clickAt(*osc,osc->element.rect.getCentre());CHECK(f.page()=="osc"&&f.screen()=="osc.1"&&!still(4));
 // Choosing DELAY shows its first screen and touches TIME.
 auto* delay=button("delay");clickAt(*delay,delay->element.rect.getCentre());
 CHECK(f.page()=="delay"&&f.screen()=="delay.1"&&lit()==juce::StringArray({"delay"}));
 CHECK(shownItems(f)==juce::StringArray({"face.delay.time","face.delay.fdbk","face.delay.sync","face.delay.mix"})&&f.lcd()==juce::StringArray({"TIME","FDBK","SYNC","MIX"}));
 CHECK(f.vfd()==(studio::VfdText{"DELAY TIME 300ms","FDBK 20% MIX 15%",-1}));
 if(f.vfd().line1!="DELAY TIME 300ms")std::cout<<"  the VFD reads "<<f.vfd().line1<<" / "<<f.vfd().line2<<std::endl;
 // The soft knobs are titled with their VFD names, and their values read as the VFD writes them.
 if(auto* time=panel.slider<Knob>("delay.time")){auto* h=time->getAccessibilityHandler();const auto* v=h?h->getValueInterface():nullptr;
  CHECK(time->getTitle()=="DELAY TIME"&&v!=nullptr&&v->getCurrentValueAsString()=="300ms");
  if(v&&v->getCurrentValueAsString()!="300ms")std::cout<<"  TIME's value reads "<<v->getCurrentValueAsString()<<std::endl;}
 else CHECK(!"no TIME");
 // Each page button is a radio button titled with its page, the chosen one with its screen; only it takes focus.
 for(const auto& p:pages)if(auto* b=button(p)){
  auto* h=b->getAccessibilityHandler();const auto chosen=p=="delay";
  CHECK(h!=nullptr&&h->getRole()==juce::AccessibilityRole::radioButton&&h->getCurrentState().isCheckable()&&h->getCurrentState().isChecked()==chosen);
  CHECK(b->getWantsKeyboardFocus()==chosen&&b->getTitle()==(chosen?juce::String("DELAY page, screen 1 of 2"):p.toUpperCase()+" page")&&(h==nullptr||h->getTitle()==b->getTitle()));}
 // The right arrow on the chosen button chooses REVERB and moves the focus to it; the left arrow comes back.
 auto* reverb=button("reverb");
 delay->grabKeyboardFocus();CHECK(delay->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
 CHECK(f.page()=="reverb"&&f.screen()=="reverb.1"&&reverb->hasKeyboardFocus(false)&&!delay->getWantsKeyboardFocus()&&f.vfd().line1.startsWith("REVERB SIZE "));
 CHECK(reverb->keyPressed(juce::KeyPress(juce::KeyPress::leftKey))&&f.page()=="delay"&&f.screen()=="delay.1"&&delay->hasKeyboardFocus(false));
 // Closing and reopening keeps the page and the screen; nothing was written.
 clickAt(*delay,delay->element.rect.getCentre());CHECK(f.screen()=="delay.2"&&delay->getTitle()=="DELAY page, screen 2 of 2");
 f.setOpen(false);f.setOpen(true);CHECK(f.page()=="delay"&&f.screen()=="delay.2"&&lit()==juce::StringArray({"delay"}));
 CHECK(r.revision()==rev&&r.document().values==values);
}
void glass_soft_knobs(){
 std::cout<<"GROUP glass_soft_knobs"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;auto& panel=f.revealed();
 f.setOpen(true);choose(f,"delay");
 auto* fdbk=panel.slider<Knob>("delay.fdbk");CHECK(fdbk!=nullptr&&fdbk->isVisible());if(!fdbk)return;
 // FDBK turns its own rack's delay bank, one gesture a drag, 0 to 0.95 over 300 px.
 const auto fb=r.index(dly,2,"delay.feedback"),otherFb=r.index(dly,4,"delay.feedback");
 CHECK(fdbk->element.paramIndex==fb&&&fdbk->param==&r.p.control(fb));
 r.automate(fb,.2);const auto other=r.gestures(otherFb);const auto ov=r.physical(otherFb);auto g0=r.gestures(fb);
 drag(*fdbk,fdbk->element.pivot,fdbk->element.pivot-Pt(0.0f,60.0f));auto g1=r.gestures(fb);
 CHECK(r.physical(fb)>.25&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&r.gestures(otherFb)==other&&r.physical(otherFb)==ov);
 CHECK(f.vfd().line1.startsWith("DELAY FDBK "));
 // POS and FACET turn one parameter and follow each other; so do REVERB DECAY and HORIZON.
 choose(f,"osc");auto* pos=panel.slider<Knob>("osc1.pos");auto* facet=f.face.slider<Knob>("facet");CHECK(pos&&facet);
 if(pos&&facet){
  CHECK(pos->element.paramIndex==facet->element.paramIndex);
  drag(*facet,facet->element.pivot,facet->element.pivot-Pt(0.0f,60.0f));pump();CHECK(std::abs(pos->getValue()-facet->getValue())<1e-6);
  drag(*pos,pos->element.pivot,pos->element.pivot+Pt(0.0f,30.0f));pump();
  CHECK(std::abs(pos->getValue()-facet->getValue())<1e-6&&std::abs(r.physical(pos->element.paramIndex)-pos->getValue())<1e-4&&f.vfd().line1.startsWith("OSC 1 POSITION "));}
 choose(f,"reverb");auto* decay=panel.slider<Knob>("reverb.decay");auto* horizon=f.face.slider<Knob>("horizon");CHECK(decay&&horizon);
 if(decay&&horizon){CHECK(decay->element.paramIndex==horizon->element.paramIndex);r.automate(decay->element.paramIndex,3.0);
  CHECK(std::abs(decay->getValue()-3.0)<1e-3&&std::abs(horizon->getValue()-3.0)<1e-3);}
}
void glass_detents(){
 std::cout<<"GROUP glass_detents"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;auto& panel=f.revealed();
 f.setOpen(true);choose(f,"filter");
 auto* mode=panel.slider<Knob>("filter.mode");CHECK(mode!=nullptr&&mode->isVisible());if(!mode)return;
 const auto mi=mode->element.paramIndex;
 CHECK(parameterRegistry()[mi].choices.size()==3&&mode->getMinimum()==0&&mode->getMaximum()==2&&mode->getInterval()==1);
 r.automate(mi,0.0);std::vector<float> angles{mode->pointerAngle()};
 // Up steps one mode a press, each one complete gesture, stopping at the last.
 for(int k=1;k<=3;++k){const auto g0=r.gestures(mi);CHECK(mode->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));const auto g1=r.gestures(mi);
  const auto to=std::min(k,2),by=k<=2?1:0;CHECK(juce::roundToInt(r.physical(mi))==to&&g1.begins==g0.begins+by&&g1.ends==g0.ends+by);angles.push_back(mode->pointerAngle());}
 // Its three modes spread over the knob's travel.
 CHECK(angles[1]-angles[0]>100&&angles[2]-angles[1]>100&&std::abs((angles[0]+angles[2])/2-angles[1])<1&&angles[3]==angles[2]);
 CHECK(mode->keyPressed(juce::KeyPress(juce::KeyPress::downKey))&&juce::roundToInt(r.physical(mi))==1);
 // A drag writes only whole modes.
 for(const float dy:{-40.0f,25.0f,70.0f,-150.0f}){drag(*mode,mode->element.pivot,mode->element.pivot+Pt(0.0f,dy));const auto v=r.physical(mi);
  CHECK(std::abs(v-std::round(v))<1e-6&&std::abs(mode->getValue()-std::round(mode->getValue()))<1e-9);}
 // LFO SHAPE names its choice on the VFD; DELAY SYNC flips.
 choose(f,"lfo");
 if(auto* shape=panel.slider<Knob>("lfo.shape")){r.automate(shape->element.paramIndex,1.0);shape->grabKeyboardFocus();CHECK(f.vfd().line1=="LFO SHAPE TRIANGLE");
  if(f.vfd().line1!="LFO SHAPE TRIANGLE")std::cout<<"  the VFD reads "<<f.vfd().line1<<std::endl;}
 else CHECK(!"no SHAPE");
 choose(f,"delay");
 if(auto* sync=panel.slider<Knob>("delay.sync")){const auto si=sync->element.paramIndex;r.automate(si,0.0);
  CHECK(sync->keyPressed(juce::KeyPress(juce::KeyPress::upKey))&&r.physical(si)>.5&&f.vfd().line1=="DELAY SYNC ON");
  CHECK(sync->keyPressed(juce::KeyPress(juce::KeyPress::downKey))&&r.physical(si)<.5&&f.vfd().line1=="DELAY SYNC OFF");}
 else CHECK(!"no SYNC");
}
void glass_beats(){
 std::cout<<"GROUP glass_beats"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;auto& panel=f.revealed();
 const auto si=r.index(dly,2,"delay.synced"),se=r.index(dly,2,"delay.seconds");
 r.automate(si,0.0);r.automate(se,.3);f.setOpen(true);choose(f,"delay");
 auto* time=panel.slider<Knob>("delay.time");auto* beats=panel.slider<Knob>("delay.beats");CHECK(time&&beats);if(!time||!beats)return;
 CHECK(time->element.paramIndex==se&&beats->element.paramIndex==r.index(dly,2,"delay.beats"));
 CHECK(time->isVisible()&&!beats->isVisible()&&f.vfd().line1=="DELAY TIME 300ms");
 // Synced, BEATS stands in TIME's slot under the same LCD word, and the VFD reads the beats.
 r.automate(si,1.0);
 CHECK(!time->isVisible()&&beats->isVisible()&&f.lcd()[0]=="TIME");
 CHECK(f.vfd().line1=="DELAY TIME "+studio::house::compactValue(beats->element,r.physical(beats->element.paramIndex)));
 if(!f.vfd().line1.startsWith("DELAY TIME "))std::cout<<"  the VFD reads "<<f.vfd().line1<<std::endl;
 r.automate(si,0.0);CHECK(time->isVisible()&&!beats->isVisible()&&f.vfd().line1=="DELAY TIME 300ms");
}
void glass_compose_open(const juce::File& images){
 std::cout<<"GROUP glass_compose_open"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;
 f.setOpen(true);choose(f,"delay");
 // The face's knobs last: ECHO keeps its drawn value where MIX shares it.
 for(auto* v:{&f.revealed(),&f.face})for(auto& s:v->sliders)if(s->element.paramIndex!=r.index(dly,2,"delay.synced"))r.automate(s->element.paramIndex,s->element.drawnValue);
 r.automate(r.index(dly,2,"delay.synced"),0.0);
 if(auto* time=f.revealed().slider<Knob>("delay.time"))time->grabKeyboardFocus();
 pump();const auto values=liveValues(r);f.face.hideReadouts();f.revealed().hideReadouts();
 const auto open=f.compose(values,true);const auto live=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(open.getWidth()==2140&&open.getHeight()==788&&f.vfd().line1.startsWith("DELAY TIME "));
 {const auto d=maxDiff(open,live);CHECK(d<=1);if(d>1)std::cout<<"  compose open differs from the live faceplate by "<<d<<std::endl;}
 // Another screen's knob changes nothing on it; a delay knob turns its pointer and letters the VFD again.
 {auto other=values;other[r.index(wt,0,"wavetable.filter.cutoff")]=.9f;CHECK(maxDiff(f.compose(other,true),open)==0);}
 {auto other=values;other[r.index(dly,2,"delay.feedback")]=.9f;CHECK(maxDiff(f.compose(other,true),open)>40);}
 CHECK(f.compose(values).getHeight()==394);
 writeImages(images,{{"glass-current-open-drawn.png",&open}});
}
// The open faceplate's outline, inset 4 px: the face's, stepping down round the door.
juce::Path doorOutline(studio::MakerFace& f){
 const auto top=f.face.getBounds().toFloat().reduced(4.0f),door=f.openedArea().toFloat().reduced(4.0f);juce::Path p;
 p.startNewSubPath(top.getTopLeft());p.lineTo(top.getTopRight());p.lineTo(top.getBottomRight());p.lineTo(door.getRight(),top.getBottom());p.lineTo(door.getBottomRight());
 p.lineTo(door.getBottomLeft());p.lineTo(door.getX(),top.getBottom());p.lineTo(top.getBottomLeft());p.closeSubPath();return p;}
// The AI's marks on Glass Current: a face knob's and a shown soft knob's dashed pointers; closed, the notch outlined and
// counting the edit controls that no face knob shows; open, the buttons of the pages that hold such changes off the
// screen; a locked control's dot, a soft knob's only while its screen shows it; another panel outlines the faceplate.
void glass_marks(const juce::File& images){
 std::cout<<"GROUP glass_marks"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;auto& panel=f.revealed();
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* facet=f.face.slider<Knob>("facet");auto* edit=f.face.button("edit");auto* fdbk=panel.slider<Knob>("delay.fdbk");auto* pos=panel.slider<Knob>("osc1.pos");
 auto* osc=panel.button("page.osc");auto* filter=panel.button("page.filter");auto* delay=panel.button("page.delay");auto* reverb=panel.button("page.reverb");
 CHECK(facet&&edit&&fdbk&&pos&&osc&&filter&&delay&&reverb);if(!facet||!edit||!fdbk||!pos||!osc||!filter||!delay||!reverb)return;
 const auto fa=facet->element.paramIndex,fb=fdbk->element.paramIndex,mo=r.index(wt,0,"wavetable.filter.mode"),sz=r.index(rvb,3,"reverb.size");
 r.automate(fa,.35);r.automate(fb,.2);r.automate(mo,0.0);r.automate(sz,.5);
 const auto live=r.document();auto candidate=live;
 const auto set=[&](SoundDocument& d,std::size_t i,double v){d.values[i]=r.p.control(i).convertTo0to1((float)v);};
 set(candidate,fa,.6);set(candidate,fb,.4);set(candidate,mo,2.0);set(candidate,sz,.8);
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(live);
 // On the CPU: a Direct2D snapshot can shift a pixel elsewhere once the dashes have been drawn.
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
 const auto gain=[](const juce::Image& a,const juce::Image& b,const juce::Path& p){return whiteAlong(a,p)-whiteAlong(b,p);};
 const auto ids=[&]{juce::StringArray out;for(const auto& k:f.marks())out.add(k.element);return out;};
 const auto pointer=[](const studio::house::Element& e,double v){const auto a=juce::degreesToRadians(house::pointerAngleFor(e,house::proportionOf(e,v)));
  return line(e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.35f,e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.95f);};
 const auto edge=[](const studio::house::SkinButton* b){return outline(b->element.rect.expanded(3.0f));};
 const auto around=[](Pt p){return juce::Rectangle<int>(24,24).withCentre(p.roundToInt());};
 f.setOpen(true);choose(f,"delay");const auto plainOpen=snapshot();f.setOpen(false);const auto plain=snapshot();
 f.showProposal(std::make_shared<const SoundDocument>(candidate));
 // Closed on DELAY 1: FACET and FDBK are marked and FACET's pointer drawn; the notch counts FDBK, MODE and SIZE. POS
 // turns FACET's parameter, so it counts no edit control.
 CHECK(ids()==juce::StringArray({"face.facet","face.delay.fdbk"})&&!f.proposesPanel());
 if(ids()!=juce::StringArray({"face.facet","face.delay.fdbk"}))std::cout<<"  marks: "<<ids().joinIntoString(" ")<<std::endl;
 CHECK(edit->getDescription()=="AI proposes changes to 3 edit controls"&&facet->getDescription().startsWith("AI proposes ")&&fdbk->getDescription().startsWith("AI proposes "));
 if(!edit->getDescription().endsWith("3 edit controls"))std::cout<<"  the notch says "<<edit->getDescription()<<std::endl;
 const auto marked=snapshot();
 CHECK(gain(marked,plain,pointer(facet->element,.6))>0.35&&gain(marked,plain,edge(edit))>0.35&&std::abs(gain(marked,plain,pointer(fdbk->element,.4)))<0.05);
 // Open: FDBK's pointer; the FILTER and REVERB buttons outlined for their pages' changes; the notch says nothing.
 f.setOpen(true);const auto markedOpen=snapshot();
 CHECK(gain(markedOpen,plainOpen,pointer(fdbk->element,.4))>0.35&&gain(markedOpen,plainOpen,edge(filter))>0.35&&gain(markedOpen,plainOpen,edge(reverb))>0.35);
 CHECK(std::abs(gain(markedOpen,plainOpen,edge(delay)))<0.05&&std::abs(gain(markedOpen,plainOpen,edge(osc)))<0.05&&edit->getDescription().isEmpty());
 CHECK(filter->getDescription()=="AI proposes changes to 1 control on this page"&&reverb->getDescription()==filter->getDescription()&&delay->getDescription().isEmpty()&&osc->getDescription().isEmpty());
 std::cout<<"  open: FDBK "<<gain(markedOpen,plainOpen,pointer(fdbk->element,.4))<<", FILTER "<<gain(markedOpen,plainOpen,edge(filter))<<", REVERB "<<gain(markedOpen,plainOpen,edge(reverb))<<std::endl;
 writeImages(images,{{"glass-current-marks.png",&marked},{"glass-current-marks-open.png",&markedOpen}});
 // On REVERB 1, SIZE is marked and DELAY's button counts FDBK in its turn.
 choose(f,"reverb");CHECK(ids()==juce::StringArray({"face.facet","face.reverb.size"})&&delay->getDescription()=="AI proposes changes to 1 control on this page"&&reverb->getDescription().isEmpty());
 // A changed position alone marks FACET and POS on OSC 1, and counts no edit control.
 choose(f,"osc");
 {auto d=live;set(d,fa,.6);f.showProposal(std::make_shared<const SoundDocument>(d));
  CHECK(ids()==juce::StringArray({"face.facet","face.osc1.pos"})&&pos->getDescription().startsWith("AI proposes ")&&osc->getDescription().isEmpty());
  f.setOpen(false);CHECK(edit->getDescription().isEmpty()&&std::abs(gain(snapshot(),plain,edge(edit)))<0.05);f.setOpen(true);}
 // FACET and FDBK locked live: neither is marked; FACET shows its dot, and FDBK only while DELAY 1 shows.
 f.showProposal(std::make_shared<const SoundDocument>(candidate));choose(f,"delay");
 {auto locked=live;locked.controlLocks[fa]=locked.controlLocks[fb]=true;
  f.refresh(locked);const auto openLocked=snapshot();CHECK(ids().isEmpty()&&facet->getDescription().isEmpty()&&fdbk->getDescription().isEmpty());
  f.refresh(live);const auto openFree=snapshot();
  CHECK(maxDiffIn(openLocked,openFree,around(fdbk->element.dot))>40&&maxDiffIn(openLocked,openFree,around(facet->element.dot))>40);
  choose(f,"filter");f.refresh(locked);const auto filterLocked=snapshot();f.refresh(live);
  CHECK(maxDiffIn(filterLocked,snapshot(),around(fdbk->element.dot))==0);
  f.setOpen(false);f.refresh(locked);const auto closedLocked=snapshot();CHECK(edit->getDescription()=="AI proposes changes to 2 edit controls");
  f.refresh(live);CHECK(maxDiffIn(closedLocked,snapshot(),around(facet->element.dot))>40&&maxDiffIn(closedLocked,snapshot(),around(fdbk->element.dot))==0);
  f.setOpen(true);choose(f,"delay");}
 // A focused control relabelled: the whole faceplate's outline, open and closed, and no marks.
 {auto d=candidate;auto racks=*d.personalities;for(auto& i:racks.instances)if(i.id==r.first)i.design.panel.controls[0].label="Sky";
  d.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));f.showProposal(std::make_shared<const SoundDocument>(d));}
 CHECK(f.proposesPanel()&&ids().isEmpty()&&filter->getDescription().isEmpty());
 CHECK(gain(snapshot(),plainOpen,doorOutline(f))>0.35);
 f.setOpen(false);CHECK(gain(snapshot(),plain,outline(f.getLocalBounds().toFloat().reduced(4.0f)))>0.35&&edit->getDescription().isEmpty());
 f.setOpen(true);f.showProposal(nullptr);CHECK(!f.proposesPanel()&&ids().isEmpty()&&maxDiff(snapshot(),plainOpen)==0);
}
// Folded, KUROSHIO's strip alone at 1U: its POWER LED live, its display reading GLASS CURRENT and outlined while the
// rack has marks.
void glass_strip(const juce::File& images){
 std::cout<<"GROUP glass_strip"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* strip=f.strip();CHECK(strip!=nullptr&&f.hasStrip());if(!strip)return;
 auto* lamp=strip->lamp("power");const auto* name=element(f.face.ctx.model,"strip.name");CHECK(lamp!=nullptr&&name!=nullptr);if(!lamp||!name)return;
 const auto en=r.index(wt,0,"enabled");
 CHECK(!f.isFolded()&&!strip->isVisible());
 // Folding closes an open door and shows only the strip.
 f.setOpen(true);int opened=0;f.onOpen=[&]{++opened;};
 f.setFolded(true);
 CHECK(f.isFolded()&&!f.isOpen()&&opened==1&&strip->isVisible()&&!f.face.isVisible()&&!f.revealed().isVisible()&&!f.hanging()->isVisible());
 CHECK(f.getWidth()==2140&&f.getHeight()==197&&tabOrder(f).isEmpty());
 // Folded, the door stays shut.
 f.setOpen(true);CHECK(!f.isOpen()&&opened==1);
 CHECK(lamp->lit());r.automate(en,0.0);CHECK(!lamp->lit());r.automate(en,1.0);CHECK(lamp->lit());
 // The live strip draws as composeStrip does, its display lettered over the bare glass.
 pump();const auto values=liveValues(r);const auto composed=f.composeStrip(values);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto live=snapshot();
 CHECK(composed.getWidth()==2140&&composed.getHeight()==197);
 {const auto d=maxDiff(composed,live);CHECK(d<=1);if(d>1)std::cout<<"  composeStrip differs from the live strip by "<<d<<std::endl;}
 {const auto& plate=f.face.ctx.model.asset("strip:plate");const auto glass=name->rect.getSmallestIntegerContainer();
  CHECK(maxDiff(composed.getClippedImage(glass),plate.image.getClippedImage(glass-plate.anchor.roundToInt()))>40);}
 {auto off=values;off[en]=0.0f;CHECK(maxDiff(f.composeStrip(off),composed)>40);}
 // While the rack has marks, the strip's display is outlined, a change on the door's screens too; so is another panel.
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(r.document());
 const auto plain=snapshot();const auto gain=[](const juce::Image& a,const juce::Image& b,const juce::Path& p){return whiteAlong(a,p)-whiteAlong(b,p);};
 const auto edge=outline(name->rect.expanded(3.0f));
 auto candidate=r.document();candidate.values[r.index(dly,2,"delay.feedback")]=.9f;
 f.showProposal(std::make_shared<const SoundDocument>(candidate));CHECK(gain(snapshot(),plain,edge)>0.35);
 {auto d=candidate;auto racks=*d.personalities;for(auto& i:racks.instances)if(i.id==r.first)i.design.panel.controls[0].label="Sky";
  d.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));f.showProposal(std::make_shared<const SoundDocument>(d));}
 CHECK(f.proposesPanel()&&gain(snapshot(),plain,edge)>0.35);
 f.showProposal(nullptr);CHECK(maxDiff(snapshot(),plain)==0);
 // Unfolding shows the closed face.
 f.setFolded(false);
 CHECK(!f.isFolded()&&!f.isOpen()&&f.face.isVisible()&&!strip->isVisible()&&f.getHeight()==394&&tabOrder(f)==juce::StringArray({"face.facet","face.drift","face.echo","face.horizon","face.edit"}));
 writeImages(images,{{"glass-current-strip-drawn.png",&composed}});
}
//------------------------------------------------------------------------------ Oxide Return over the real processor
// Two Oxide Returns on the shared master: the first's delay in effect bank 1, the second's in bank 2.
struct OxideRig:Rig{OxideRig(){first=place(3,{RackRole::fx,4,0},{{0,0}});second=place(3,{RackRole::fx,4,1},{{0,1}});show(600,1200);}};
void oxide_binds_its_banks(){
 std::cout<<"GROUP oxide_binds_its_banks"<<std::endl;OxideRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(0)},std::pair{r.b.get(),std::uint8_t(1)}}){
  CHECK(face->getComponentID()=="rack.face"&&face->getWidth()==2140&&face->getHeight()==591&&face->units()==3);
  CHECK(!face->hasStrip()&&face->front()==nullptr&&face->hanging()==nullptr&&face->getTitle()=="ARDENTI, OXIDE RETURN TAPE ECHO MODEL AR-62");
  const auto knob=[&](studio::house::View& v,const char* id){auto* k=v.slider<Knob>(id);CHECK(k!=nullptr);return k?k->element.paramIndex:~std::size_t();};
  const auto lever=[&](studio::house::View& v,const char* id){auto* l=v.lever(id);CHECK(l!=nullptr);return l?l->element.paramIndex:~std::size_t();};
  const std::pair<const char*,const char*> onFace[]{{"time","delay.seconds"},{"repeats","delay.feedback"},{"mix","delay.mix"},{"saturation","delay.saturation"},
                                                    {"wobble","delay.wobble"},{"low-cut","delay.low_cut_hz"},{"high-cut","delay.filter_hz"}};
  for(const auto& [id,key]:onFace)CHECK(knob(face->face,id)==r.index(dly,slot,key));
  const std::pair<const char*,const char*> inRecess[]{{"note","delay.note"},{"cross","delay.stereo"},{"wobble-rate","delay.wobble_rate_hz"}};
  for(const auto& [id,key]:inRecess)CHECK(knob(face->revealed(),id)==r.index(dly,slot,key));
  CHECK(lever(face->face,"in")==r.index(dly,slot,"bypassed")&&lever(face->revealed(),"sync")==r.index(dly,slot,"delay.synced")
        &&lever(face->revealed(),"ping-pong")==r.index(dly,slot,"delay.ping_pong"));
  {auto* lamp=face->face.lamp("lamp");CHECK(lamp!=nullptr&&lamp->element.paramIndex==r.index(dly,slot,"bypassed")&&lamp->lit());}
  // Thirteen of the unit's parameters: all but its glide, its right time and its beats, which NOTE's first step hands to.
  const auto ps=face->parameters();
  CHECK(ps.size()==13);
  for(const char* key:{"delay.glide","delay.right_ratio","delay.beats"})CHECK(std::find(ps.begin(),ps.end(),r.index(dly,slot,key))==ps.end());
  if(auto* time=face->face.slider<Knob>("time"))CHECK(&time->param==&r.p.control(r.index(dly,slot,"delay.seconds")));
 }
 {auto broken=r.instance(r.first);broken.bindings.pop_back();CHECK(studio::MakerFace::forRack(r.p,studio::makerFaceFor(broken.design),broken,r.host())==nullptr);}
}
// TIME takes its presses on the bezel, centred above the needle's hub: one drag is one gesture on its own rack's
// parameter, the wheel, a key and a double-click one each.
void oxide_gestures(){
 std::cout<<"GROUP oxide_gestures"<<std::endl;OxideRig r;if(!r.a)return;
 auto* time=r.a->face.slider<Knob>("time");CHECK(time!=nullptr);if(!time)return;
 const auto& e=time->element;
 const auto t1=r.index(dly,0,"delay.seconds"),t2=r.index(dly,1,"delay.seconds");
 CHECK(std::abs(r.physical(t1)-.3)<1e-3);
 const auto hits=[&](Pt p){return time->hitTest(juce::roundToInt(p.x-(float)time->getX()),juce::roundToInt(p.y-(float)time->getY()));};
 const auto top=e.hitCentre()-Pt(0.0f,e.hitR-12.0f),under=e.hitCentre()+Pt(0.0f,e.hitR+8.0f);
 CHECK(top.getDistanceFrom(e.pivot)>e.hitR&&under.getDistanceFrom(e.pivot)<e.hitR&&hits(top)&&!hits(under));
 CHECK(time->getBounds().toFloat().contains(juce::Rectangle<float>(2*e.hitR,2*e.hitR).withCentre(e.hitCentre())));
 const auto other=r.gestures(t2);auto g0=r.gestures(t1);
 drag(*time,top,top-Pt(0.0f,60.0f));auto g1=r.gestures(t1);
 // 60 of the 300 px of a full turn: a fifth of TIME's log travel up from 300 ms.
 const auto want=.02*std::pow(100.0,proportion(e,.3)+.2);
 CHECK(std::abs(r.physical(t1)-want)<want*.02&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 CHECK(r.gestures(t2)==other&&std::abs(r.physical(t2)-.3)<1e-3);
 g0=g1;const auto before=r.physical(t1);time->mouseWheelMove(event(*time,local(*time,e.hitCentre()),{},0,false),wheel(0.1f));g1=r.gestures(t1);
 CHECK(r.physical(t1)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto p0=time->proportion();time->keyPressed(juce::KeyPress(juce::KeyPress::upKey));g1=r.gestures(t1);
 CHECK(std::abs(time->proportion()-p0-0.01)<0.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;time->mouseDoubleClick(event(*time,local(*time,e.hitCentre()),{},2,false));g1=r.gestures(t1);
 CHECK(std::abs(r.physical(t1)-.25)<1e-3&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 // The hub turns nothing and takes no press.
 if(auto* hub=r.a->face.findChildWithID("face.hub")){bool self=true,children=true;hub->getInterceptsMouseClicks(self,children);CHECK(hub->isVisible()&&!self&&!hub->getWantsKeyboardFocus());}
 else CHECK(!"no hub");
}
// The readouts say each value as the faceplate prints it; the gauges pin at their printed ends.
void oxide_pins(){
 std::cout<<"GROUP oxide_pins"<<std::endl;OxideRig r;if(!r.a)return;auto& f=*r.a;
 auto* time=f.face.slider<Knob>("time");auto* high=f.face.slider<Knob>("high-cut");CHECK(time&&high);if(!time||!high)return;
 const auto t=r.index(dly,0,"delay.seconds"),h=r.index(dly,0,"delay.filter_hz");const auto g0=r.gestures(t);
 // TIME prints 20 ms to 2 s of the parameter's 1 ms to 4 s; HIGH CUT 1 to 20 kHz of 20 Hz to 20 kHz.
 r.automate(t,3.0);CHECK(std::abs(time->pointerAngle()-time->element.ticks.back().second)<.01f&&time->readoutText()=="3.00 s");
 r.automate(t,.005);CHECK(std::abs(time->pointerAngle()-time->element.ticks.front().second)<.01f&&time->readoutText()=="5 ms");
 r.automate(h,500.0);CHECK(std::abs(high->pointerAngle()-high->element.ticks.front().second)<.01f&&high->readoutText()=="500 Hz");
 CHECK(r.gestures(t)==g0);
 const auto text=[&](studio::house::View& v,const char* id,const char* key,double value){r.automate(r.index(dly,0,key),value);auto* k=v.slider<Knob>(id);return k?k->readoutText():juce::String("missing");};
 CHECK(text(f.face,"time","delay.seconds",.3)=="300 ms"&&text(f.face,"time","delay.seconds",1.25)=="1.25 s");
 CHECK(text(f.face,"repeats","delay.feedback",.38)=="38%"&&text(f.face,"mix","delay.mix",.25)=="25%"&&text(f.face,"saturation","delay.saturation",.3)=="30%"
       &&text(f.face,"wobble","delay.wobble",.2)=="20%");
 CHECK(text(f.face,"low-cut","delay.low_cut_hz",100.0)=="100 Hz"&&text(f.face,"high-cut","delay.filter_hz",5000.0)=="5.0 kHz");
 CHECK(text(f.revealed(),"cross","delay.stereo",.3)=="30%"&&text(f.revealed(),"wobble-rate","delay.wobble_rate_hz",.8)=="0.80 Hz");
 // NOTE says its step by name, its pointer on the step's printed mark.
 for(int i=0;i<22;++i){CHECK(text(f.revealed(),"note","delay.note",i)==noteChoices[i]);
  if(auto* note=f.revealed().slider<Knob>("note"))CHECK(note->element.positions.size()==22&&std::abs(note->pointerAngle()-note->element.positions[(std::size_t)i].angle)<.01f);}
}
// IN / OUT and FREE / SYNC are pairs of push buttons: a press on a button's cap pushes it in and the other comes out, one
// gesture; a press on the one pushed in, or between the caps, does nothing. The lamp is lit while the delay is IN.
void oxide_levers(){
 std::cout<<"GROUP oxide_levers"<<std::endl;OxideRig r;if(!r.a||!r.b)return;
 auto* in=r.a->face.lever("in");auto* lamp=r.a->face.lamp("lamp");CHECK(in!=nullptr&&lamp!=nullptr);if(!in||!lamp)return;
 const auto by=r.index(dly,0,"bypassed");
 const auto out=in->element.positions[0].legendRect,cap=in->element.positions[1].legendRect;
 CHECK(r.physical(by)<0.5&&lamp->lit()&&in->legend()=="IN"&&in->getTitle()=="IN / OUT");
 auto g0=r.gestures(by);clickAt(*in,cap.getCentre());
 CHECK(r.physical(by)<0.5&&r.gestures(by)==g0&&in->legend()=="IN");
 clickAt(*in,out.getCentre());auto g1=r.gestures(by);
 CHECK(r.physical(by)>0.5&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&!lamp->lit()&&in->legend()=="OUT");
 clickAt(*in,out.getCentre());CHECK(r.physical(by)>0.5&&r.gestures(by)==g1);
 clickAt(*in,{(cap.getRight()+out.getX())*0.5f,out.getCentreY()});CHECK(r.physical(by)>0.5&&r.gestures(by)==g1);
 clickAt(*in,cap.getCentre());CHECK(r.physical(by)<0.5&&lamp->lit()&&in->legend()=="IN");
 // Space and Enter push the other; Right or Up the right one, OUT; Left or Down the left one, IN; each one gesture.
 in->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(r.physical(by)>0.5);
 in->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(r.physical(by)<0.5);
 {const auto step=[&](int code,double to,int gestures){const auto g0=r.gestures(by);in->keyPressed(juce::KeyPress(code));const auto g1=r.gestures(by);
   return std::abs(r.physical(by)-to)<1e-3&&g1.begins==g0.begins+gestures&&g1.ends==g0.ends+gestures;};
  CHECK(step(juce::KeyPress::leftKey,0.0,0)&&step(juce::KeyPress::rightKey,1.0,1)&&step(juce::KeyPress::upKey,1.0,0)&&step(juce::KeyPress::downKey,0.0,1)
        &&step(juce::KeyPress::leftKey,0.0,0));}
 // The other rack's are its own.
 CHECK(r.b->face.lamp("lamp")->lit());
 {auto& b=r.p.control(r.index(dly,1,"bypassed"));b.setValueNotifyingHost(1.0f);CHECK(!r.b->face.lamp("lamp")->lit()&&r.b->face.lever("in")->legend()=="OUT"&&lamp->lit());}
 // FREE / SYNC and DUAL / PING PONG in the recess, stacked: Right or Up pushes the top one, Left or Down the bottom one.
 r.a->setOpen(true);auto* sync=r.a->revealed().lever("sync");auto* pp=r.a->revealed().lever("ping-pong");CHECK(sync&&pp);if(!sync||!pp)return;
 const auto sy=r.index(dly,0,"delay.synced"),pi=r.index(dly,0,"delay.ping_pong");
 CHECK(r.physical(sy)<0.5&&sync->legend()=="FREE");
 clickAt(*sync,sync->element.positions[1].legendRect.getCentre());CHECK(r.physical(sy)>0.5&&sync->legend()=="SYNC");
 clickAt(*sync,sync->element.positions[0].legendRect.getCentre());CHECK(r.physical(sy)<0.5&&sync->legend()=="FREE");
 CHECK(r.physical(pi)<0.5&&pp->legend()=="DUAL"&&pp->getTitle()=="DUAL / PING PONG");
 {const auto dual=pp->element.positions[0].legendRect,ping=pp->element.positions[1].legendRect;
  auto g0=r.gestures(pi);clickAt(*pp,dual.getCentre());CHECK(r.physical(pi)<0.5&&r.gestures(pi)==g0);
  clickAt(*pp,ping.getCentre());auto g1=r.gestures(pi);
  CHECK(r.physical(pi)>0.5&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&pp->legend()=="PING PONG"&&r.physical(r.index(dly,1,"delay.ping_pong"))<0.5);
  clickAt(*pp,{dual.getCentreX(),(dual.getBottom()+ping.getY())*0.5f});CHECK(r.physical(pi)>0.5&&r.gestures(pi)==g1);
  clickAt(*pp,dual.getCentre());CHECK(r.physical(pi)<0.5&&pp->legend()=="DUAL");}
 for(auto* l:{sync,pp}){const auto i=l->element.paramIndex;
  const auto step=[&](int code,double to,int gestures){const auto g0=r.gestures(i);l->keyPressed(juce::KeyPress(code));const auto g1=r.gestures(i);
   return std::abs(r.physical(i)-to)<1e-3&&g1.begins==g0.begins+gestures&&g1.ends==g0.ends+gestures;};
  CHECK(step(juce::KeyPress::upKey,0.0,0)&&step(juce::KeyPress::downKey,1.0,1)&&step(juce::KeyPress::leftKey,1.0,0)&&step(juce::KeyPress::rightKey,0.0,1)
        &&step(juce::KeyPress::spaceKey,1.0,1)&&step(juce::KeyPress::returnKey,0.0,1));}
 // While another gesture holds the processor, a press changes nothing and says why.
 const auto held=r.index(dly,0,"delay.mix");CHECK(r.p.beginParameterGesture(held));r.reported.clear();
 clickAt(*in,out.getCentre());CHECK(r.physical(by)<0.5&&in->legend()=="IN"&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
// The pointer over IN / OUT, FREE / SYNC or DUAL / PING PONG lights nothing: a box over a pair of buttons is a highlight
// the faceplate's drawing lacks. A bat lever still lightens under it.
void oxide_levers_no_hover(){
 std::cout<<"GROUP oxide_levers_no_hover"<<std::endl;
 const auto shot=[](juce::Component& c){return c.createComponentSnapshot(c.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
 {OxideRig r;if(!r.a)return;auto& f=*r.a;f.setOpen(true);f.face.hideReadouts();f.revealed().hideReadouts();
  for(auto* l:{f.face.lever("in"),f.revealed().lever("sync"),f.revealed().lever("ping-pong")}){
   CHECK(l!=nullptr&&l->element.legendClick);if(!l)continue;
   const auto plain=shot(f);l->hover=true;l->repaint();const auto d=maxDiff(shot(f),plain);l->hover=false;
   CHECK(d==0);if(d)std::cout<<"  hovering "<<l->element.id<<" changes the face by "<<d<<std::endl;}}
 FaceRig t;if(!t.a)return;auto* bat=t.a->face.lever("in");CHECK(bat!=nullptr&&!bat->element.legendClick);if(!bat)return;
 t.a->face.hideReadouts();const auto plain=shot(*t.a);bat->hover=true;bat->repaint();CHECK(maxDiff(shot(*t.a),plain)>5);bat->hover=false;
}
// NOTE's 22 steps. A press on a step's printed legend, a triplet's 3 or a dotted note's dot, chooses that step where it
// lets go, one gesture, and shows the readout; a press there turns nothing, and a double-click there leaves the step it
// chose. On the knob a drag lands on whole steps, the wheel and the keys step one, and a double-click returns it to
// Beats. Each is the first rack's alone.
void oxide_note(){
 std::cout<<"GROUP oxide_note"<<std::endl;OxideRig r;if(!r.a||!r.b)return;auto& f=*r.a;f.setOpen(true);
 auto* note=f.revealed().slider<Knob>("note");CHECK(note!=nullptr);if(!note)return;
 const auto& e=note->element;const auto ni=r.index(dly,0,"delay.note"),other=r.index(dly,1,"delay.note");
 CHECK(e.paramIndex==ni&&e.choices==noteChoices&&e.legendClick&&note->getMinimum()==0&&note->getMaximum()==21&&note->getInterval()==1);
 {juce::StringArray registry;for(const auto& c:parameterRegistry()[ni].choices)registry.add(c);CHECK(registry==noteChoices);}
 CHECK(e.positions.size()==22);if(e.positions.size()!=22)return;
 // The recipe's eighth.
 CHECK(std::abs(r.physical(ni)-11)<1e-4&&note->readoutText()=="1/8");
 const auto g00=r.gestures(other);
 const auto legend=[&](int i){return e.positions[(std::size_t)i].legendRect.getCentre();};
 const auto is=[&](int i){return std::abs(r.physical(ni)-i)<1e-4&&std::abs(note->getValue()-i)<1e-9&&std::abs(note->pointerAngle()-e.positions[(std::size_t)i].angle)<.01f;};
 const auto once=[&](auto act,int to){const auto g0=r.gestures(ni);act();const auto g1=r.gestures(ni);return is(to)&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1;};
 const auto none=[&](auto act,int stays){const auto g0=r.gestures(ni);act();return is(stays)&&r.gestures(ni)==g0;};
 const auto hits=[&](Pt p){return note->hitTest(juce::roundToInt(p.x-(float)note->getX()),juce::roundToInt(p.y-(float)note->getY()));};
 // Every legend, from the last step back to the first, the readout naming it.
 for(int i=21;i>=0;--i){f.revealed().hideReadouts();
  CHECK(hits(legend(i))&&once([&]{clickAt(*note,legend(i));},i)&&note->readoutText()==noteChoices[i]&&!f.revealed().readoutBounds().isEmpty());}
 // Just off a legend, away from the knob, still chooses it; the step it is on takes no gesture.
 for(int i:{1,4,13,20}){const auto& box=e.positions[(std::size_t)i].legendRect;const auto c=box.getCentre();const auto out=(c-e.pivot)/c.getDistanceFrom(e.pivot);
  auto p=c;while(box.contains(p))p+=out*.25f;p+=out*1.5f;
  CHECK(hits(p)&&once([&]{clickAt(*note,p);},i)&&none([&]{clickAt(*note,p);},i));}
 // A press on a legend dragged and let go on another does nothing; a double-click on one leaves its step.
 {const juce::ModifierKeys held(juce::ModifierKeys::leftButtonModifier);const auto a=local(*note,legend(3)),b=local(*note,legend(5));
  CHECK(none([&]{note->mouseDown(event(*note,a,held,1,false));note->mouseDrag(event(*note,a-Pt(0.0f,80.0f),held,1,true,a));note->mouseUp(event(*note,b,{},1,true,a));},20));
  const auto d=local(*note,legend(14));
  CHECK(once([&]{note->mouseDown(event(*note,d,held,1,false));note->mouseUp(event(*note,d,{},1,false));note->mouseDown(event(*note,d,held,2,false));
                 note->mouseDoubleClick(event(*note,d,{},2,false));note->mouseUp(event(*note,d,{},2,false));},14));}
 // On the knob 43 of the 300 px of a full turn are three steps; the wheel and Up step one, Home and End go to the ends,
 // and a double-click returns to Beats.
 CHECK(once([&]{drag(*note,e.pivot,e.pivot-Pt(0.0f,43.0f));},17));
 CHECK(once([&]{note->mouseWheelMove(event(*note,local(*note,e.pivot),{},0,false),wheel(0.1f));},18));
 CHECK(once([&]{note->keyPressed(juce::KeyPress(juce::KeyPress::upKey));},19)&&once([&]{note->keyPressed(juce::KeyPress(juce::KeyPress::endKey));},21)
       &&none([&]{note->keyPressed(juce::KeyPress(juce::KeyPress::upKey));},21)&&once([&]{note->keyPressed(juce::KeyPress(juce::KeyPress::homeKey));},0));
 CHECK(once([&]{note->keyPressed(juce::KeyPress(juce::KeyPress::endKey));},21)&&once([&]{note->mouseDoubleClick(event(*note,local(*note,e.pivot),{},2,false));},0));
 // While another gesture holds the processor, a legend changes nothing and says why.
 const auto held=r.index(dly,0,"delay.mix");CHECK(r.p.beginParameterGesture(held));r.reported.clear();
 clickAt(*note,legend(8));CHECK(is(0)&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
 CHECK(r.gestures(other)==g00&&std::abs(r.physical(other)-11)<1e-4);
}
// The tambour: its pull bar slides it open over the face's tempo controls, the faceplate keeping its 3U; Tab reaches
// them only while it is open; closing with focus inside hands it to the tambour; neither writes anything.
void oxide_tempo(){
 std::cout<<"GROUP oxide_tempo"<<std::endl;OxideRig r;if(!r.a)return;auto& f=*r.a;
 auto* tambour=f.face.button("tempo");auto* close=f.revealed().button("close");CHECK(tambour!=nullptr&&close!=nullptr);if(!tambour||!close)return;
 CHECK(tambour->getTitle()=="Open tempo and motion"&&close->getTitle()=="Close tempo and motion");
 auto open=oxideFace;open.addArray(oxideTempo);
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&tabOrder(f)==oxideFace&&f.units()==3);
 const auto bounds=f.getBounds();const auto rev=r.revision();const auto values=r.document().values;int opened=0;f.onOpen=[&]{++opened;};
 clickAt(*tambour,tambour->element.rect.getCentre());
 CHECK(f.isOpen()&&f.revealed().isVisible()&&f.getBounds()==bounds&&opened==1&&f.units()==3);
 CHECK(tabOrder(f)==open);if(tabOrder(f)!=open)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 auto* note=f.revealed().slider<Knob>("note");CHECK(note!=nullptr);if(!note)return;
 note->grabKeyboardFocus();CHECK(note->hasKeyboardFocus(false));
 close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&juce::Component::getCurrentlyFocusedComponent()==tambour&&opened==2&&tabOrder(f)==oxideFace);
 // With focus outside, it stays there.
 auto* time=f.face.slider<Knob>("time");time->grabKeyboardFocus();
 tambour->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(f.isOpen()&&time->hasKeyboardFocus(false));
 clickAt(*close,close->element.rect.getCentre());CHECK(!f.isOpen()&&time->hasKeyboardFocus(false));
 CHECK(r.revision()==rev&&r.document().values==values&&f.getBounds()==bounds);
 // NOTE's cell takes its presses.
 f.setOpen(true);
 {const auto cell=note->element.hitRect;CHECK(note->hitTest(juce::roundToInt(cell.getX()+4.0f-(float)note->getX()),juce::roundToInt(cell.getBottom()-4.0f-(float)note->getY())));}
}
void oxide_compose(const juce::File& images){
 std::cout<<"GROUP oxide_compose"<<std::endl;OxideRig r;if(!r.a)return;auto& f=*r.a;
 for(auto* v:{&f.face,&f.revealed()}){for(auto& s:v->sliders)r.automate(s->element.paramIndex,s->element.drawnValue);for(auto& l:v->levers)r.automate(l->element.paramIndex,l->element.drawnValue);}
 pump();const auto values=liveValues(r);
 f.face.hideReadouts();f.revealed().hideReadouts();
 const auto composed=f.compose(values);const auto live=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(composed.getWidth()==2140&&composed.getHeight()==591);
 {const auto d=maxDiff(composed,live);CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live face by "<<d<<std::endl;}
 f.setOpen(true);f.revealed().hideReadouts();const auto opened=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(maxDiff(f.compose(values),composed)==0&&maxDiff(opened,composed)>40);
 {const auto d=maxDiff(f.compose(values,true),opened);CHECK(d<=1);if(d>1)std::cout<<"  the open compose differs from the live face by "<<d<<std::endl;}
 // Other values turn the needle and push OUT in, the lamp dark.
 {auto other=values;const auto t=r.index(dly,0,"delay.seconds"),by=r.index(dly,0,"bypassed");other[t]=r.p.control(t).convertTo0to1(1.0f);other[by]=1.0f;
  const auto o=f.compose(other);const auto& lamp=f.face.lamp("lamp")->element;
  CHECK(maxDiff(o,composed)>40&&maxDiffIn(o,composed,juce::Rectangle<float>(2*lamp.lensR,2*lamp.lensR).withCentre(lamp.led).getSmallestIntegerContainer())>40);}
 writeImages(images,{{"oxide-return-drawn.png",&composed},{"oxide-return-open-drawn.png",&opened}});
}
// The AI's marks: TIME's dashed needle and IN / OUT's outline on the face; NOTE's only while the tambour is open, and the
// tambour outlined while it is closed; a locked control and the same values get none.
void oxide_marks(const juce::File& images){
 std::cout<<"GROUP oxide_marks"<<std::endl;OxideRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* time=f.face.slider<Knob>("time");auto* in=f.face.lever("in");auto* tambour=f.face.button("tempo");auto* note=f.revealed().slider<Knob>("note");
 CHECK(time&&in&&tambour&&note);if(!time||!in||!tambour||!note)return;
 const auto t=r.index(dly,0,"delay.seconds"),by=r.index(dly,0,"bypassed"),no=r.index(dly,0,"delay.note");
 const auto live=r.document();auto candidate=live;
 const auto set=[&](SoundDocument& d,std::size_t i,double v){d.values[i]=r.p.control(i).convertTo0to1((float)v);};
 set(candidate,t,.6);set(candidate,by,1.0);set(candidate,no,14);
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(live);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto plain=snapshot();f.setOpen(true);const auto plainOpen=snapshot();f.setOpen(false);
 f.showProposal(std::make_shared<const SoundDocument>(candidate));
 const auto marks=f.marks();CHECK(marks.size()==3&&!f.proposesPanel());
 if(marks.size()==3)CHECK(marks[0].element=="face.time"&&std::abs(marks[0].proposed-.6f)<.005f&&marks[1].element=="face.in"&&marks[1].proposed==1.0f
                          &&marks[2].element=="face.note"&&std::abs(marks[2].proposed-14.0f)<.005f);
 CHECK(time->getDescription()=="AI proposes 600 ms"&&in->getDescription()=="AI proposes OUT"&&note->getDescription()=="AI proposes 1/4");
 if(note->getDescription()!="AI proposes 1/4")std::cout<<"  NOTE's description: "<<note->getDescription()<<std::endl;
 CHECK(tambour->getDescription()=="AI proposes changes to 1 tempo and motion control");
 const auto pointer=[](const studio::house::Element& e,double v){const auto a=juce::degreesToRadians(house::pointerAngleFor(e,house::proportionOf(e,v)));
  return line(e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.35f,e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.95f);};
 const auto timeLine=pointer(time->element,.6),noteLine=pointer(note->element,14);
 const auto inEdge=outline(in->element.rect.expanded(3.0f)),tambourEdge=outline(tambour->element.rect.expanded(3.0f));
 const auto gain=[](const juce::Image& a,const juce::Image& b,const juce::Path& p){return whiteAlong(a,p)-whiteAlong(b,p);};
 const auto marked=snapshot();
 CHECK(gain(marked,plain,timeLine)>0.35&&gain(marked,plain,inEdge)>0.35&&gain(marked,plain,tambourEdge)>0.35&&std::abs(gain(marked,plain,noteLine))<0.05);
 std::cout<<"  closed: TIME "<<gain(marked,plain,timeLine)<<", IN / OUT "<<gain(marked,plain,inEdge)<<", tambour "<<gain(marked,plain,tambourEdge)<<", NOTE "<<gain(marked,plain,noteLine)<<std::endl;
 f.setOpen(true);const auto markedOpen=snapshot();
 // NOTE's silver cap is near white where its mark crosses it, so the mark must read by its contrast.
 CHECK(changedAlong(markedOpen,plainOpen,noteLine)>0.35&&gain(markedOpen,plainOpen,tambourEdge)<0.05&&tambour->getDescription().isEmpty());
 std::cout<<"  open: NOTE "<<changedAlong(markedOpen,plainOpen,noteLine)<<", tambour "<<gain(markedOpen,plainOpen,tambourEdge)<<std::endl;
 f.setOpen(false);CHECK(tambour->getDescription()=="AI proposes changes to 1 tempo and motion control");
 writeImages(images,{{"oxide-return-marks.png",&marked},{"oxide-return-marks-open.png",&markedOpen}});
 {auto locked=live;locked.controlLocks[t]=true;f.refresh(locked);
  CHECK(f.marks().size()==2&&f.marks()[0].element=="face.in"&&time->getDescription().isEmpty()&&gain(snapshot(),plain,timeLine)<0.05);}
 f.refresh(live);CHECK(f.marks().size()==3);
 f.showProposal(std::make_shared<const SoundDocument>(live));
 CHECK(f.marks().empty()&&time->getDescription().isEmpty()&&tambour->getDescription().isEmpty()&&maxDiff(snapshot(),plain)==0);
 f.showProposal(nullptr);CHECK(f.marks().empty()&&maxDiff(snapshot(),plain)==0);
}
}
//------------------------------------------------------------------------------ the reveals moving
// The motion's sections stand its clock still and step it, with reduced motion off; everywhere else it is on, so
// each reveal jumps as the sections above expect.
// Dispatch due frames after the pump: its deadline may expire inside another message callback.
struct Clock{double t=0;Clock(){studio::motion::setForTests([this]{return t;},false);}~Clock(){studio::motion::setForTests({},true);}
 Clock(const Clock&)=delete;void at(double ms){t=ms;pump();juce::Timer::callPendingTimersSynchronously();}void reduced(bool on){studio::motion::setForTests([this]{return t;},on);}};
void motion_ease(){
 std::cout<<"GROUP motion_ease"<<std::endl;using studio::motion::ease;
 CHECK(ease(0)==0&&ease(1)==1&&ease(.5f)==.5f&&ease(.25f)==.0625f&&ease(-.5f)==0&&ease(1.5f)==1);
 float last=-1;
 for(int k=0;k<=20;++k){const auto t=(float)k/20.0f;CHECK(std::abs(ease(1-t)-(1-ease(t)))<1e-6f);CHECK(ease(t)>last||k==0);last=ease(t);}
}
void motion_overrides(){
 std::cout<<"GROUP motion_overrides"<<std::endl;
 studio::motion::setForTests([]{return 1234.5;},true);CHECK(studio::motion::now()==1234.5&&studio::motion::reduced());
 studio::motion::setForTests({},false);CHECK(!studio::motion::reduced()&&studio::motion::now()!=1234.5);
 // Restored, reduced motion is Windows' Animation effects being off (SPI_GETCLIENTAREAANIMATION).
 studio::motion::setForTests({},std::nullopt);int animations=1;CHECK(SystemParametersInfoW(0x1042,0,&animations,0)!=0);
 CHECK(studio::motion::reduced()==(animations==0));
 // A 5 ms sleep can end under 4 ms, a timer tick early, so the wait watches the clock.
 const auto a=studio::motion::now();const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(5);
 while(std::chrono::steady_clock::now()<until)juce::Thread::sleep(1);CHECK(studio::motion::now()-a>=4);
 studio::motion::setForTests({},true);
}
studio::house::SkinButton* pullOf(studio::MakerFace& f){auto* b=f.front()!=nullptr?f.front()->button("programmer"):nullptr;CHECK(b!=nullptr);return b;}
void press(studio::house::SkinButton& b){clickAt(b,b.element.rect.getCentre());}
// The drawer slides out over 200 ms, its front at the eased fraction of the deck below the face and the deck's foot on
// the front, behind the face; the state is the open one from the press.
void motion_drawer(){
 std::cout<<"GROUP motion_drawer"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;Clock clock;auto* pull=pullOf(f);if(!pull)return;
 int opened=0;f.onOpen=[&]{++opened;};
 const auto drawn=[&](float e){return f.front()->getY()==juce::roundToInt(394+788*e)&&f.revealed().getBottom()==f.front()->getY()&&f.getHeight()==f.front()->getBottom()
                                    &&std::abs(f.shownUnits()-(3+4*e))<1e-5f;};
 press(*pull);
 CHECK(f.isOpen()&&f.units()==7&&f.isMoving()&&f.opening()==0&&f.revealed().isVisible()&&opened==1&&drawn(0));
 CHECK(f.getIndexOfChildComponent(&f.revealed())<f.getIndexOfChildComponent(&f.face));
 for(double t:{50.0,100.0,150.0}){clock.at(t);const auto e=studio::motion::ease((float)(t/200));CHECK(f.isMoving()&&std::abs(f.opening()-e)<1e-6f&&drawn(e));}
 clock.at(200);
 CHECK(!f.isMoving()&&f.opening()==1&&f.getHeight()==1379&&f.revealed().getY()==394&&f.front()->getY()==1182&&opened>=4&&tabOrder(f).size()==31);
 // Closing mirrors it, the deck hidden as it ends.
 clock.at(300);opened=0;press(*pull);
 CHECK(!f.isOpen()&&f.units()==3&&f.isMoving()&&f.opening()==1&&f.revealed().isVisible()&&opened==1&&tabOrder(f).size()==7);
 for(double t:{350.0,400.0,450.0}){clock.at(t);const auto e=1-studio::motion::ease((float)((t-300)/200));CHECK(f.isMoving()&&std::abs(f.opening()-e)<1e-6f&&drawn(e)&&f.revealed().isVisible());}
 clock.at(500);
 CHECK(!f.isMoving()&&f.opening()==0&&!f.revealed().isVisible()&&f.getHeight()==591&&f.front()->getY()==394&&opened>=4);
}
// A press while moving turns it back from where it is drawn; two presses before a frame leave it where it was.
void motion_reverse(){
 std::cout<<"GROUP motion_reverse"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;Clock clock;auto* pull=pullOf(f);if(!pull)return;
 press(*pull);clock.at(50);CHECK(f.isOpen()&&std::abs(f.opening()-.0625f)<1e-6f);
 press(*pull);CHECK(!f.isOpen()&&f.isMoving()&&std::abs(f.opening()-.0625f)<1e-6f);
 clock.at(75);CHECK(f.isMoving()&&std::abs(f.opening()-(1-studio::motion::ease(.875f)))<1e-6f);
 clock.at(100);CHECK(!f.isMoving()&&f.opening()==0&&f.getHeight()==591&&!f.revealed().isVisible());
 press(*pull);press(*pull);CHECK(!f.isOpen()&&f.opening()==0&&f.getHeight()==591);
 const auto diagnose=[&](const char* at){if(f.isMoving()||f.revealed().isVisible()||f.getHeight()!=591)std::cout<<"DIAGNOSTIC motion_reverse "<<at<<" isOpen="<<f.isOpen()<<" isMoving="<<f.isMoving()<<" opening="<<f.opening()<<" revealVisible="<<f.revealed().isVisible()<<" height="<<f.getHeight()<<std::endl;};
 diagnose("immediate");
 CHECK(!f.isMoving()&&!f.revealed().isVisible()&&f.getHeight()==591);
 pump();diagnose("after_pump");CHECK(!f.isMoving()&&!f.revealed().isVisible()&&f.getHeight()==591);
}
// Each press reads reduced motion: on, it jumps; off, it moves.
void motion_reduced(){
 std::cout<<"GROUP motion_reduced"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;Clock clock;auto* pull=pullOf(f);if(!pull)return;
 clock.reduced(true);press(*pull);CHECK(f.isOpen()&&!f.isMoving()&&f.opening()==1&&f.getHeight()==1379&&f.front()->getY()==1182);
 clock.reduced(false);press(*pull);CHECK(!f.isOpen()&&f.isMoving()&&f.opening()==1);
 clock.at(200);CHECK(!f.isMoving()&&f.getHeight()==591);
 clock.reduced(true);press(*pull);CHECK(f.isOpen()&&!f.isMoving()&&f.getHeight()==1379);
}
// The clock read before the press is the start; a frame long after the end settles there.
void motion_clock_bounds(){
 std::cout<<"GROUP motion_clock_bounds"<<std::endl;EmberRig r;if(!r.a)return;auto& f=*r.a;Clock clock;auto* pull=pullOf(f);if(!pull)return;
 clock.t=1000;press(*pull);clock.at(950);CHECK(f.isMoving()&&f.opening()==0&&f.getHeight()==591);
 clock.at(60000);CHECK(!f.isMoving()&&f.opening()==1&&f.getHeight()==1379);
}
// Closing at once while moving, folding while moving, and the faceplate deleted while moving.
void motion_interrupt(){
 std::cout<<"GROUP motion_interrupt"<<std::endl;
 {EmberRig r;if(!r.a)return;auto& f=*r.a;Clock clock;auto* pull=pullOf(f);if(!pull)return;
  press(*pull);clock.at(100);f.setOpen(false,false);
  CHECK(!f.isOpen()&&!f.isMoving()&&f.opening()==0&&f.getHeight()==591&&!f.revealed().isVisible());
  press(*pull);clock.at(150);CHECK(f.isMoving());
  r.a.reset();pump();pump();CHECK(r.b!=nullptr&&r.b->getHeight()==591&&!r.b->isMoving());}
 {GlassRig r;if(!r.a)return;auto& f=*r.a;Clock clock;
  f.setOpen(true);clock.at(90);CHECK(f.isOpen()&&f.isMoving());
  f.setFolded(true);CHECK(f.isFolded()&&!f.isOpen()&&!f.isMoving()&&f.opening()==0&&!f.revealed().isVisible()&&f.getHeight()==197);
  clock.at(400);CHECK(f.isFolded()&&f.getHeight()==197);}
}
bool rectNear(juce::Rectangle<float> a,juce::Rectangle<float> b){return std::abs(a.getX()-b.getX())<1e-2f&&std::abs(a.getY()-b.getY())<1e-2f&&std::abs(a.getWidth()-b.getWidth())<1e-2f&&std::abs(a.getHeight()-b.getHeight())<1e-2f;}
// A component keeps its alpha in 255ths.
constexpr float alphaStep=.5f/255+1e-4f;
// Frames side by side, each its snapshot's `crop`.
juce::Image sheet(const std::vector<juce::Image>& frames,juce::Rectangle<int> crop){
 juce::Image out(juce::Image::ARGB,crop.getWidth()*(int)frames.size(),crop.getHeight(),true);juce::Graphics g(out);int x=0;
 for(const auto& f:frames){g.drawImageAt(f.getClippedImage(crop.getIntersection(f.getBounds())),x,0);x+=crop.getWidth();}
 return out;}
// THREAD PRESS's fasteners turn a quarter while the drawn fraction rises to 0.4; the cover then lifts, growing 4 % about
// its middle as the recess fades in; the faceplate keeps its 2U. Closing runs it backward.
void motion_lid(const juce::File& images){
 std::cout<<"GROUP motion_lid"<<std::endl;FaceRig r;if(!r.a)return;auto& f=*r.a;Clock clock;
 auto* cover=f.face.button("setup");CHECK(cover!=nullptr);if(!cover)return;
 const auto lid=f.skin.skin.reveal.motion.lid;
 const auto drawn=[&](float e){const auto p=std::max(0.0f,(e-.4f)/.6f),s=1+.04f*p;
  return std::abs(f.fastenerTurn()-90*std::min(1.0f,e/.4f))<1e-3f&&std::abs(f.revealed().getAlpha()-p)<alphaStep&&f.getHeight()==394
         &&rectNear(f.movingArea(),lid.withSizeKeepingCentre(lid.getWidth()*s,lid.getHeight()*s));};
 std::vector<juce::Image> frames;const auto frame=[&]{frames.push_back(f.createComponentSnapshot(f.getLocalBounds(),true,1.0f));};
 press(*cover);
 for(double t:{0.0,44.0,88.0,154.0}){clock.at(t);const auto e=studio::motion::ease((float)(t/220));CHECK(f.isMoving()&&drawn(e));frame();}
 clock.at(220);frame();
 CHECK(!f.isMoving()&&f.isOpen()&&f.fastenerTurn()==90&&f.movingArea().isEmpty()&&f.revealed().getAlpha()==1&&f.getHeight()==394);
 {const auto s=sheet(frames,lid.getSmallestIntegerContainer().expanded(50));writeImages(images,{{"thread-press-lid-frames.png",&s}});}
 auto* close=f.revealed().button("close");CHECK(close!=nullptr);if(!close)return;
 clock.at(300);press(*close);
 for(double t:{366.0,454.0}){clock.at(t);const auto e=1-studio::motion::ease((float)((t-300)/220));CHECK(f.isMoving()&&!f.isOpen()&&drawn(e));}
 clock.at(520);CHECK(!f.isMoving()&&f.fastenerTurn()==0&&!f.revealed().isVisible()&&f.revealed().getAlpha()==1&&f.movingArea().isEmpty());
}
// OXIDE RETURN's tambour slides right by the skin's slide at the drawn fraction, cut off at the lid's right edge, over
// the recess, which shows at once where it has gone; nothing else moves and the faceplate keeps its 3U. Closing runs it
// backward.
void motion_tambour(const juce::File& images){
 std::cout<<"GROUP motion_tambour"<<std::endl;OxideRig r;if(!r.a)return;auto& f=*r.a;Clock clock;
 auto* tambour=f.face.button("tempo");CHECK(tambour!=nullptr);if(!tambour)return;
 const auto& m=f.skin.skin.reveal.motion;const auto lid=m.lid;
 const auto& plate=f.face.ctx.model.asset("tempo:plate");
 const auto recess=juce::Rectangle<float>(plate.anchor.x,plate.anchor.y,(float)plate.image.getWidth(),(float)plate.image.getHeight()).getSmallestIntegerContainer().expanded(2);
 const auto slid=[&](float e){return lid.translated(m.slide.x*e,m.slide.y*e).getIntersection(lid);};
 const auto drawn=[&](float e){return f.fastenerTurn()==0&&f.revealed().isVisible()&&f.revealed().getAlpha()==1&&f.getHeight()==591&&rectNear(f.movingArea(),slid(e));};
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 f.face.hideReadouts();const auto closed=snapshot();
 std::vector<juce::Image> frames;
 press(*tambour);
 juce::Image half;
 for(double t:{0.0,52.0,104.0,130.0,156.0,208.0}){clock.at(t);const auto e=studio::motion::ease((float)(t/260));CHECK(f.isMoving()&&f.isOpen()&&drawn(e));frames.push_back(snapshot());if(t==130.0)half=frames.back();}
 clock.at(260);frames.push_back(snapshot());const auto opened=frames.back();
 CHECK(!f.isMoving()&&f.isOpen()&&f.movingArea().isEmpty()&&f.revealed().getAlpha()==1&&f.getHeight()==591);
 // Half-way: the recess where the tambour has gone, the tambour where it is, and the face round them as it was.
 {const auto shift=m.slide.x*.5f;
  const auto gone=lid.withWidth(shift).reduced(6.0f).getSmallestIntegerContainer(),still=lid.withTrimmedLeft(shift).reduced(6.0f).getSmallestIntegerContainer();
  CHECK(maxDiffIn(half,opened,gone)<=1&&maxDiffIn(half,opened,still)>40&&maxDiffOutside(half,closed,recess)<=1);}
 {const auto s=sheet(frames,lid.getSmallestIntegerContainer().expanded(30));writeImages(images,{{"oxide-return-tambour-frames.png",&s}});}
 auto* close=f.revealed().button("close");CHECK(close!=nullptr);if(!close)return;
 clock.at(300);press(*close);
 for(double t:{365.0,430.0,495.0}){clock.at(t);const auto e=1-studio::motion::ease((float)((t-300)/260));CHECK(f.isMoving()&&!f.isOpen()&&drawn(e));}
 clock.at(560);CHECK(!f.isMoving()&&!f.revealed().isVisible()&&f.movingArea().isEmpty()&&maxDiff(snapshot(),closed)<=1);
}
// GLASS CURRENT's door turns 180 degrees about its foot: the closed door foreshortens to its foot by the angle's cosine,
// then the hanging door's inside unfolds below its top; the faceplate grows from 2U to 4U with the drawn fraction and
// the hanging view shows when it settles.
void motion_door(const juce::File& images){
 std::cout<<"GROUP motion_door"<<std::endl;GlassRig r;if(!r.a)return;auto& f=*r.a;Clock clock;
 std::vector<juce::Image> frames;const auto frame=[&]{frames.push_back(f.createComponentSnapshot(f.getLocalBounds(),true,1.0f));};
 auto* edit=f.face.button("edit");auto* hang=f.hanging();auto* close=hang?hang->button("close"):nullptr;CHECK(edit&&hang&&close);if(!edit||!hang||!close)return;
 const auto door=f.skin.skin.reveal.motion.door;const auto& plate=f.face.ctx.model.asset("door:plate");
 const juce::Rectangle<float> inside(plate.anchor.x,plate.anchor.y,(float)plate.image.getWidth(),(float)plate.image.getHeight());
 // From the keyboard, the foot notch has the focus at the press.
 edit->grabKeyboardFocus();edit->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(f.isOpen()&&f.isMoving()&&juce::Component::getCurrentlyFocusedComponent()==close);
 for(double t:{0.0,45.0,79.0,90.0,135.0}){clock.at(t);const auto e=studio::motion::ease((float)(t/180));const auto a=180*e;const auto c=std::cos(juce::degreesToRadians(a));
  const auto swung=a<90?door.withTop(door.getBottom()-door.getHeight()*c):inside.withHeight(-inside.getHeight()*c);
  CHECK(f.isMoving()&&f.getHeight()==juce::roundToInt(394+394*e)&&hang->getAlpha()==0&&f.revealed().getAlpha()==1&&rectNear(f.movingArea(),swung));frame();}
 clock.at(180);frame();
 CHECK(!f.isMoving()&&f.isOpen()&&f.getHeight()==788&&hang->isVisible()&&hang->getAlpha()==1&&f.movingArea().isEmpty()&&f.openedArea()==inside.toNearestInt());
 {const auto s=sheet(frames,{1400,0,740,788});writeImages(images,{{"glass-current-door-frames.png",&s}});}
}
// A SET-UP control's lock dot waits for the recess to settle open.
void motion_marks(){
 std::cout<<"GROUP motion_marks"<<std::endl;FaceRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* cover=f.face.button("setup");auto* ratio=f.revealed().slider<Knob>("ratio");CHECK(cover&&ratio);if(!cover||!ratio)return;
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);
 auto locked=r.document();locked.controlLocks[r.index(comp,0,"compressor.ratio")]=true;f.refresh(locked);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
 const auto dot=juce::Rectangle<int>(24,24).withCentre((ratio->element.dot+f.revealed().getPosition().toFloat()).roundToInt());
 Clock clock;press(*cover);clock.at(200);CHECK(f.isMoving()&&f.opening()>.99f);const auto moving=snapshot();
 clock.at(220);CHECK(!f.isMoving()&&maxDiffIn(moving,snapshot(),dot)>40);
}
void saturation_task2_contract(const juce::File& images){
 std::cout<<"GROUP saturation_task2_contract"<<std::endl;
 const auto factories=airack::test::frozenFactoryRackDesigns();
 CHECK(factories.size()==21);
 const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Character Drive";});
 CHECK(found!=factories.end());if(found==factories.end())return;
 CHECK(found==factories.begin()+19&&found->role==RackRole::fx&&found->units.size()==1);
 CHECK(found->units[0].module==NativeModule::saturation&&found->units[0].defaults.size()==36);
 CHECK(found->revision==2&&!found->intent.containsIgnoreCase("pending")&&found->panel.controls.size()==8);
 const juce::StringArray keys{"saturation.input_trim_db","saturation.full.pre_tilt_db","saturation.full.character","saturation.full.drive_db","saturation.full.mix","saturation.mix","saturation.full.trim_db","saturation.output_trim_db"};
 const double low[]{-24,-6,0,0,0,0,-18,-24},high[]{24,6,2,36,1,1,18,12};
 for(int n=0;n<keys.size();++n){const auto& control=found->panel.controls[std::size_t(n)];CHECK(control.key==keys[n]&&control.unit==0&&control.minimum==low[n]&&control.maximum==high[n]);}
 const auto parsed=house::loadModel([](const juce::String& path){return airack::test::frozenMakerSkinFile("character-drive",path);},nullptr);
 CHECK(parsed.elements.size()==13);
 CHECK((parsed.asset("advanced:plate").slice==std::array<int,4>({8,8,8,8})));
 CHECK((parsed.asset("advanced:section").slice==std::array<int,4>({6,6,6,6})));
 CHECK((parsed.asset("advanced:key").slice==std::array<int,4>({7,7,7,7})));
 const auto& cap=parsed.asset("advanced:knob");const auto& ring=parsed.asset("advanced:knob-ring");
 CHECK(cap.image.getWidth()==104&&cap.anchor==Pt(51.5f,51.5f)&&cap.image.getPixelAt(0,0).getAlpha()==0&&cap.image.getPixelAt(51,51).getAlpha()==255);
 CHECK(ring.image.getWidth()==104&&ring.image.getPixelAt(51,51).getAlpha()==0);

 CHECK(parsed.asset("cap:small").image.getWidth()==88&&parsed.asset("cap:setup-cap").image.getWidth()==64);
 for(const auto* id:{"face.mode","face.comp"}){const auto* e=element(parsed,id);CHECK(e&&e->rect.getWidth()==76&&e->hitR>=36&&e->pivot.y+32<236);}

 const auto malformed=[&](int element,const char* field){auto json=juce::JSON::parse(airack::test::frozenMakerSkinFile("character-drive","elements.json").toString());json["elements"][element]["geometry"].getDynamicObject()->removeProperty(field);const auto text=juce::JSON::toString(json);try{house::loadModel([&](const juce::String& path){return path=="elements.json"?juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8()):airack::test::frozenMakerSkinFile("character-drive",path);},nullptr);return false;}catch(const std::runtime_error&){return true;}};
 CHECK(malformed(0,"tick_center")&&malformed(2,"rect"));
 CHECK(studio::studioSkin());
 const auto skin=studio::makerFaceFor(*found);CHECK(skin);if(!skin)return;
 CHECK(skin->units==2&&skin->skin.reveal.kind=="recess"&&skin->maker=="OXIDE INSTRUMENTS");
 Rig r;for(int n=0;n<2;++n){RackOperation op;op.design=*found;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,std::uint8_t(n)};op.allocation={{0,std::uint8_t(n+6)}};CHECK(r.p.applyRackOperation(op,r.revision())==RackCandidateStatus::ready);(n==0?r.first:r.second)=op.newInstance;}r.show(400,800);if(!r.a||!r.b)return;r.a->refresh(r.document());r.b->refresh(r.document());
 CHECK(r.a->parameters().size()==10&&!r.a->getDescription().contains("DSP PENDING"));
 CHECK(std::abs(r.physical(r.index(NativeModule::saturation,6,"saturation.full.drive_db"))-12)<1e-4&&std::abs(r.physical(r.index(NativeModule::saturation,6,"saturation.full.mix"))-.75)<1e-6);
 auto* character=r.a->face.lever("character");CHECK(character&&character->element.positions.size()==3);if(character){CHECK(character->element.positions[0].angle==-55&&character->element.positions[1].angle==0&&character->element.positions[2].angle==55);CHECK(character->legend()=="GLOW");}
 auto* setup=r.a->face.button("setup");CHECK(setup);if(!setup)return;const auto before=r.document();
 setup->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(r.a->isOpen()&&r.a->revealed().isVisible()&&r.document()==before);
 CHECK(tabOrder(*r.a).contains("face.mode")&&tabOrder(*r.a).contains("face.comp")&&tabOrder(*r.a).contains("face.all-controls"));
 CHECK(r.a->revealed().control("mode")&&r.a->revealed().control("comp")&&r.a->revealed().button("all-controls"));
 r.a->face.hideReadouts();r.a->revealed().hideReadouts();
 const auto openComposed=r.a->compose(before.values,true),openLive=r.a->createComponentSnapshot(r.a->getLocalBounds(),true,1.0f);
 CHECK(maxDiff(openComposed,openLive)<=1);
 if(images!=juce::File{}){writeImages(images,{{"character-drive-open-composed.png",&openComposed},{"character-drive-open-live.png",&openLive}});}
 const auto* model=studio::studioSkin();CHECK(model);if(!model)return;studio::AllControlsPanel panel(r.p,*model,{});panel.show({{NativeModule::saturation,6}});panel.refresh(before);
 studio::SaturationGraph* crossover=nullptr;for(auto* child:panel.getChildren())if(auto* g=dynamic_cast<studio::SaturationGraph*>(child))crossover=g;
 CHECK(crossover&&crossover->isVisible());if(crossover){const auto low=r.index(NativeModule::saturation,6,"saturation.low_crossover_hz");CHECK(crossover->parameters()==std::vector<std::size_t>({low,low+1}));CHECK(crossover->getDescription().startsWith("SATURATION | "));panel.showKey(4);CHECK(!crossover->isVisible()&&panel.shownHeight()==254);panel.showKey(0);}
 CHECK(panel.keys()==std::vector<juce::String>({"GLOBAL","FULL","LOW","MID","HIGH"}));
 for(int n=0;n<5;++n){auto* key=panel.findChildWithID("rack.controls.key."+juce::String(n));CHECK(key&&key->getHeight()==46&&key->getWidth()==112);}
 const auto equipmentShot=panel.createComponentSnapshot(panel.getLocalBounds(),true,1.f);CHECK(equipmentShot.getPixelAt(3,3)==parsed.asset("advanced:plate").image.getPixelAt(3,3));

 CHECK(panel.parameters().size()==36);for(const auto index:panel.parameters()){CHECK(panel.showParameter(index));CHECK(panel.knob(index));}
 const auto mixIndex=r.index(NativeModule::saturation,6,"saturation.mix");auto* mixEntry=panel.knob(mixIndex);CHECK(mixEntry);
 if(mixEntry)for(const auto* text:{"0%","50%","100%"}){const auto unchanged=r.document();mixEntry->openEntry();mixEntry->entry.setText(text,false);mixEntry->commitEntry();const auto expected=juce::String(text).getFloatValue()/100.f;CHECK(std::abs(r.physical(mixIndex)-expected)<1.e-6);if(r.document()!=unchanged){CHECK(r.p.undoSoundEdit()&&r.document()==unchanged);}}


 // The real panel keeps all eight banks and mixed-unit pages visibly selectable within its header.
 const auto reachHeader=[&](std::vector<studio::AllControlsPanel::Unit> units,std::size_t expected){
  panel.show(std::move(units));panel.refresh(before);CHECK(panel.parameters().size()==expected);
  auto* selector=dynamic_cast<juce::ComboBox*>(panel.findChildWithID("rack.controls.unit"));auto* cover=panel.findChildWithID("rack.controls.patchbay");CHECK(selector&&cover);
  if(!selector||!cover)return;CHECK(selector->getNumItems()==(int)panel.keys().size());
  juce::StringArray identity;for(int n=0;n<selector->getNumItems();++n){const auto name=selector->getItemText(n);CHECK(!identity.contains(name));identity.add(name);selector->setSelectedId(n+1,juce::sendNotificationSync);
   CHECK(panel.shownKey()==n&&selector->isVisible());
   for(auto* child:panel.getChildren())if(child->isVisible()&&child->getComponentID().startsWith("rack.controls.key.")){CHECK(panel.getLocalBounds().contains(child->getBounds()));if(cover->isVisible())CHECK(!child->getBounds().intersects(cover->getBounds()));}
  }
  for(const auto i:panel.parameters()){CHECK(panel.showParameter(i));CHECK(panel.knob(i)&&panel.knob(i)->getParentComponent()->isVisible());}
 };
 std::vector<studio::AllControlsPanel::Unit> allSat;for(std::uint8_t bank=0;bank<8;++bank)allSat.push_back({NativeModule::saturation,bank});reachHeader(allSat,8*36);
 reachHeader({{NativeModule::analog,0},{NativeModule::drive,0},{NativeModule::saturation,6}},unitReach(NativeModule::analog,0).size()+unitReach(NativeModule::drive,0).size()+36);
 reachHeader({{NativeModule::dynamicEq,0},{NativeModule::saturation,6}},unitReach(NativeModule::dynamicEq,0).size()+36);
 CHECK(panel.showParameter(dynamicEqParameterStart+3));CHECK(!panel.findChildWithID("rack.controls.patchbay")->isVisible());
 panel.show({{NativeModule::saturation,6}});panel.refresh(before);
 const auto mode=r.index(NativeModule::saturation,6,"saturation.mode"),tilt=r.index(NativeModule::saturation,6,"saturation.full.pre_tilt_db"),lowTilt=r.index(NativeModule::saturation,6,"saturation.low.pre_tilt_db");
 r.automate(mode,1);const auto three=r.document();r.a->refresh(three);r.b->refresh(three);panel.refresh(three);
 CHECK(r.a->getDescription().contains("RETAINED")&&r.b->getDescription().contains("FULL BANK"));
 for(const auto* id:{"pre-tilt","character","drive","lane-mix","wet-trim"}){auto* c=r.a->face.control(id);CHECK(c&&c->isEnabled()&&c->getAlpha()>=.79f&&c->getAlpha()<1&&c->getDescription().containsIgnoreCase("retained"));}
 auto* trim=r.a->face.slider<Knob>("pre-tilt");CHECK(trim);if(trim){trim->keyPressed(juce::KeyPress(juce::KeyPress::upKey));CHECK(r.document().values[tilt]!=three.values[tilt]&&r.document().values[lowTilt]==three.values[lowTilt]);CHECK(r.p.undoSoundEdit()&&r.document()==three);CHECK(r.p.redoSoundEdit());CHECK(r.p.undoSoundEdit());}
 auto* modeControl=r.a->revealed().control("mode");auto* close=r.a->revealed().button("close");CHECK(modeControl&&close);if(modeControl&&close){modeControl->grabKeyboardFocus();CHECK(modeControl->hasKeyboardFocus(false));close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(!r.a->isOpen()&&setup->hasKeyboardFocus(false));}
 auto candidate=std::make_shared<SoundDocument>(three);for(const auto index:panel.parameters())candidate->values[index]=three.values[index]>.5f?0.f:1.f;r.a->showProposal(candidate);CHECK(r.a->marks().size()==10&&setup->getDescription().contains("28 controls"));
 auto locked=three;locked.controlLocks[lowTilt]=true;r.a->refresh(locked);CHECK(setup->getDescription().contains("27 controls"));r.a->showProposal(nullptr);r.a->refresh(three);
 const auto bypass=r.index(NativeModule::saturation,6,"bypassed");auto raw=three.values;raw[bypass]=0;const auto zero=r.a->compose(raw);raw[bypass]=1;CHECK(maxDiff(zero,r.a->compose(raw))==0);
 for(const auto* f:{r.a.get(),r.b.get()})for(const auto& e:f->face.ctx.model.elements)if(e.rackUnit>=0){const auto bank=f==r.a.get()?6:7;CHECK(e.paramIndex==r.index(NativeModule::saturation,std::uint8_t(bank),e.key.toRawUTF8()));if(e.nativeEngraving)CHECK(e.nativeEngraving->baseRadius==0);}
 const auto* output=element(r.a->face.ctx.model,"face.output");CHECK(output&&std::abs(house::pointerAngleFor(*output,house::proportionOf(*output,0))-45)<1e-4);
 const auto saved=r.document();juce::MemoryBlock bytes;CHECK(encodeSoundState(saved,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==saved);

}
void native_layer_image_repeat(const juce::File& images){
 std::cout<<"GROUP native_layer_image_repeat"<<std::endl;
 const auto factories=airack::test::frozenFactoryRackDesigns();
 const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Character Drive";});
 CHECK(found!=factories.end());if(found==factories.end())return;
 const RackInstance saturation{juce::Uuid(),*found,{{0,6}}},thread{juce::Uuid(),airack::test::frozenExampleRackDesigns()[2],{{0,0},{1,6}}};
 const auto satSkin=studio::makerFaceFor(saturation.design);const auto threadSkin=studio::makerFaceFor(thread.design);
 CHECK(satSkin&&threadSkin);if(!satSkin||!threadSkin)return;
 auto values=defaultParameterValues();
 for(const auto* rack:{&saturation,&thread})for(const auto& unit:rack->design.units)for(const auto& field:unit.defaults)
  values[*rackParameterIndex(unit.module,rackUnitBinding(*rack,unit.id)->slot,field.key)]=field.normalized;
 const auto physical=[&](std::size_t index,float value){const auto& d=parameterRegistry()[index];values[index]=juce::NormalisableRange<float>(d.minimum,d.maximum,d.kind==ParameterKind::continuous?0.0f:1.0f,d.skew).convertTo0to1(value);};
 physical(*rackParameterIndex(NativeModule::saturation,6,"saturation.mode"),1.0f);
 for(const auto& e:threadSkin->skin.elements)if(e.rackUnit>=0&&(e.kind=="rotary"||e.kind=="fader"||e.kind=="selector"||e.kind=="lever")){
  const auto unit=std::find_if(thread.design.units.begin(),thread.design.units.end(),[&](const auto& u){return u.id==e.rackUnit;});
  physical(*rackParameterIndex(unit->module,rackUnitBinding(thread,unit->id)->slot,e.key),(float)e.drawnValue);
 }
 // Retain both real Three-band compositions, then all three stills before reading any pixels.
 const auto bypass=*rackParameterIndex(NativeModule::saturation,6,"bypassed");
 values[bypass]=0;const auto zero=studio::MakerFace::composeFor(*satSkin,saturation,values);
 values[bypass]=1;const auto one=studio::MakerFace::composeFor(*satSkin,saturation,values);
 const auto first=studio::MakerFace::composeFor(*threadSkin,thread,values);
 const auto second=studio::MakerFace::composeFor(*threadSkin,thread,values);
 const auto third=studio::MakerFace::composeFor(*threadSkin,thread,values);
 CHECK(first.getWidth()==2140&&first.getHeight()==394);
 CHECK(maxDiff(zero,one)==0);
 const auto repeat=maxDiff(first,second),later=maxDiff(first,third);
 CHECK(repeat==0);CHECK(later==0);
 std::cout<<"  native image repeat="<<repeat<<" third="<<later<<" first="<<first.getPixelAt(1390,216).toString()<<" second="<<second.getPixelAt(1390,216).toString()<<" thirdPixel="<<third.getPixelAt(1390,216).toString()<<std::endl;
 writeImages(images,{{"native-layer-first.png",&first},{"native-layer-second.png",&second},{"native-layer-third.png",&third}});
}
void band_color_face_for(){
 std::cout<<"GROUP band_color_face_for"<<std::endl;
 const auto factories=airack::test::frozenFactoryRackDesigns();
 const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf02");});
 CHECK(found!=factories.end());if(found==factories.end())return;
 CHECK(found->name=="Band Color"&&found->units.size()==1&&found->units[0].defaults.size()==36&&found->panel.controls.size()==8);
 const auto skin=studio::makerFaceFor(*found);CHECK(skin);if(!skin)return;
 CHECK(skin->maker=="VELDRUM AUDIO"&&skin->model=="BAND COLOR"&&skin->units==2);
 {auto d=*found;d.units[0].defaults.clear();d.intent="Edited intent";d.revision=8;CHECK(studio::makerFaceFor(d)==nullptr);}
 const auto differs=[&](auto change){auto d=*found;change(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](auto& d){d.id=juce::Uuid();}));CHECK(differs([](auto& d){d.name="Band Colour";}));
 CHECK(differs([](auto& d){d.role=RackRole::synth;}));CHECK(differs([](auto& d){d.units[0].module=NativeModule::drive;}));
 CHECK(differs([](auto& d){d.units[0].id=1;}));CHECK(differs([](auto& d){d.panel.sections[0]="Changed";}));
 for(std::size_t n=0;n<8;++n){CHECK(differs([&](auto& d){d.panel.controls[n].key="bypassed";}));CHECK(differs([&](auto& d){d.panel.controls[n].label="Changed";}));
  CHECK(differs([&](auto& d){d.panel.controls[n].maximum+=1;}));CHECK(differs([&](auto& d){d.panel.controls[n].response=d.panel.controls[n].response==FocusResponse::linear?FocusResponse::logarithmic:FocusResponse::linear;}));}
 CHECK(airack::test::frozenMakerSkinFile("band-color","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("band-color","plates/band-color-face.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("band-color","parts/thread-press-face-grip-head.png").isEmpty()&&airack::test::frozenMakerSkinFile("thread-press","plates/band-color-face.png").isEmpty());
 const auto& m=skin->skin;CHECK(m.elements.size()==14&&m.units==2&&m.panelWidth==2140&&m.asset("face:plate").image.getBounds()==juce::Rectangle<int>(2140,394));
 CHECK(m.reveal.kind=="recess"&&m.reveal.opener=="face.calibration"&&m.reveal.closers==juce::StringArray({"face.close"}));
 const juce::StringArray keys{"saturation.low_crossover_hz","saturation.high_crossover_hz","saturation.mid.drive_db","saturation.mid.mix","saturation.high.drive_db","saturation.high.mix","saturation.high.pre_tilt_db","saturation.mix","saturation.input_trim_db","saturation.output_trim_db","saturation.mode","saturation.drive_compensation"};
 const double defaults[]{180,3500,0,1,0,1,0,1,0,0,0,1},drawn[]{180,3500,9,.5,6,.5,-1,1,0,0,1,1};
 // Source-bound printed rays: Band Color photograph-calibration-01.json, unchanged numeric scales.
 constexpr float ends[][2]{{-137.28292789077716f,135.78120084158797f},{-137.4909015371206f,135.33997996367884f},
  {-134.07969678709827f,134.19301228472062f},{-136.04313652820738f,135.9937952196361f},
  {-133.21663734283095f,134.34822995933118f},{-134.9435571436383f,135.11138459340233f},
  {-135.96268948323538f,134.0305502594694f},{-137.81974113972473f,137.14649356390055f},
  {-134.70666630764643f,132.77179817087185f},{-134.1042447293975f,132.0782052054999f}};
 int bound=0;
 for(const auto& e:m.elements)if(e.rackUnit>=0){CHECK(e.rackUnit==0&&bound<12);if(bound>=12)break;
  CHECK(e.key==keys[bound]&&e.defaultValue==defaults[bound]&&e.drawnValue==drawn[bound]&&!e.hitRect.isEmpty());
  CHECK(rackParameterActive(NativeModule::saturation,e.key));
  if(e.kind=="rotary"){CHECK(e.ticks.size()==3&&e.hasDot);for(const auto& [v,a]:e.ticks)CHECK(std::abs(angleAt(e,v)-a)<.001f);
   CHECK(house::pointerAngleFor(e,-.1)==ends[bound][0]&&house::pointerAngleFor(e,1.1)==ends[bound][1]);}
  ++bound;
 }
 CHECK(bound==12);
 const auto* output=element(m,"face.output-trim");CHECK(output&&std::abs(angleAt(*output,0)-44.05652095519212f)<.001f&&std::abs(angleAt(*output,-12)+45.02386188710268f)<.001f);
 const auto* closer=element(m,"face.close");CHECK(closer);if(closer)for(const auto* id:{"face.input-trim","face.output-trim","face.mode","face.compensation"}){
  const auto* e=element(m,id);CHECK(e&&e->hitRect.getIntersection(closer->rect).isEmpty());}
}
void band_color_candidate(const juce::File& images){
 std::cout<<"GROUP band_color_candidate"<<std::endl;
 const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Band Color";});
 CHECK(found!=factories.end());if(found==factories.end())return;
 const auto skin=studio::makerFaceFor(*found);CHECK(skin);if(!skin)return;
 Rig r;for(int n=0;n<2;++n){RackOperation op;op.design=*found;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,std::uint8_t(n)};op.allocation={{0,std::uint8_t(n+6)}};
  CHECK(r.p.applyRackOperation(op,r.revision())==RackCandidateStatus::ready);(n==0?r.first:r.second)=op.newInstance;}
 r.show(400,800);if(!r.a||!r.b)return;auto& f=*r.a;
 for(auto* face:{r.a.get(),r.b.get()}){CHECK(face->parameters().size()==12&&face->getHeight()==394&&face->units()==2);
  for(const auto& e:face->face.ctx.model.elements)if(e.rackUnit>=0)CHECK(e.paramIndex==r.index(NativeModule::saturation,face==r.a.get()?6:7,e.key.toRawUTF8()));}
 const auto mid=r.index(NativeModule::saturation,6,"saturation.mid.drive_db"),otherMid=r.index(NativeModule::saturation,7,"saturation.mid.drive_db");
 CHECK(std::abs(r.physical(mid)-9)<1e-5&&std::abs(r.physical(otherMid)-9)<1e-5);
 CHECK(std::abs(r.physical(r.index(NativeModule::saturation,6,"saturation.high.drive_db"))-6)<1e-5&&std::abs(r.physical(r.index(NativeModule::saturation,6,"saturation.high.pre_tilt_db"))+1)<1e-5);
 const auto initial=r.document();f.face.hideReadouts();f.revealed().hideReadouts();
 const auto closed=f.compose(initial.values),closedLive=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(maxDiff(closed,closedLive)<=1);CHECK(maxDiff(closed,f.compose(initial.values))==0);
 f.setOpen(true);f.revealed().hideReadouts();const auto opened=f.compose(initial.values,true),openLive=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(maxDiff(opened,openLive)<=1&&maxDiff(closed,f.compose(initial.values))==0);
 writeImages(images,{{"band-color-drawn.png",&closed},{"band-color-closed-live.png",&closedLive},{"band-color-open-drawn.png",&opened},{"band-color-open-live.png",&openLive}});
 auto state=initial.values;for(const auto& e:f.face.ctx.model.elements)if(e.rackUnit>=0){const auto& d=parameterRegistry()[e.paramIndex];state[e.paramIndex]=r.p.control(e.paramIndex).convertTo0to1(d.defaultValue);}
 const auto registry=f.compose(state,true);writeImages(images,{{"band-color-registered-defaults.png",&registry}});
 for(bool maximum:{false,true}){for(const auto& e:f.face.ctx.model.elements)if(e.rackUnit>=0)state[e.paramIndex]=maximum?1.f:0.f;const auto end=f.compose(state,true);writeImages(images,{{maximum?"band-color-maximum.png":"band-color-minimum.png",&end}});}
 f.setOpen(false);
 auto* cover=f.face.button("calibration");auto* close=f.revealed().button("close");auto* input=f.revealed().slider<Knob>("input-trim");auto* mode=f.revealed().lever("mode");auto* drive=f.face.slider<Knob>("mid-drive");
 CHECK(cover&&close&&input&&mode&&drive);if(!cover||!close||!input||!mode||!drive)return;
 CHECK(tabOrder(f).size()==9&&!tabOrder(f).contains("face.input-trim"));
 const auto beforeCover=r.document();const auto beforeRevision=r.revision();cover->grabKeyboardFocus();cover->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(f.isOpen()&&f.getHeight()==394&&f.units()==2&&tabOrder(f).size()==14&&tabOrder(f).contains("face.close"));
 CHECK(r.document()==beforeCover&&r.revision()==beforeRevision);
 for(const auto* id:{"input-trim","output-trim","mode","compensation"}){auto* c=f.revealed().control(id);CHECK(c);if(!c)continue;
  const auto* e=element(f.face.ctx.model,"face."+juce::String(id));CHECK(e);if(!e)continue;const auto hit=e->hitRect;
  CHECK(c->hitTest(juce::roundToInt(hit.getX()+3-c->getX()),juce::roundToInt(hit.getY()+3-c->getY())));
  CHECK(c->hitTest(juce::roundToInt(hit.getRight()-3-c->getX()),juce::roundToInt(hit.getBottom()-3-c->getY())));}
 input->grabKeyboardFocus();CHECK(input->hasKeyboardFocus(false));close->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
 CHECK(!f.isOpen()&&cover->hasKeyboardFocus(false)&&r.document()==beforeCover&&r.revision()==beforeRevision);
 f.setOpen(true);input->grabKeyboardFocus();CHECK(!f.keyPressed(juce::KeyPress(juce::KeyPress::upKey))&&f.isOpen());CHECK(f.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
 CHECK(!f.isOpen()&&cover->hasKeyboardFocus(false));CHECK(!f.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))&&!f.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
 // This skin has no folded strip; its owning rack closes the recess when folded (StudioWindow coverage).
 CHECK(!f.hasStrip());
 const auto original=r.document();const auto count=r.gestures(mid),otherCount=r.gestures(otherMid);
 drag(*drive,drive->element.pivot,drive->element.pivot-Pt(0,60));const auto afterDrag=r.document();const auto moved=r.gestures(mid);
 CHECK(std::abs(r.physical(mid)-16.2)<.03&&moved.begins==count.begins+1&&moved.ends==count.ends+1&&r.gestures(otherMid)==otherCount&&std::abs(r.physical(otherMid)-9)<1e-5);
 CHECK(r.p.undoSoundEdit()&&r.document()==original);CHECK(r.p.redoSoundEdit()&&r.document()==afterDrag);CHECK(r.p.undoSoundEdit());
 drive->mouseDoubleClick(event(*drive,local(*drive,drive->element.pivot),{},2,false));CHECK(std::abs(r.physical(mid))<1e-6&&drive->element.defaultValue==0);CHECK(r.p.undoSoundEdit()&&r.document()==original);
 const auto modeIndex=r.index(NativeModule::saturation,6,"saturation.mode");r.automate(modeIndex,0);f.refresh(r.document());r.b->refresh(r.document());
 for(const auto* id:{"low-crossover","high-crossover","mid-drive","mid-mix","high-drive","high-mix","high-tilt"}){auto* c=f.face.control(id);CHECK(c&&c->isEnabled()&&c->getAlpha()==1);if(c)CHECK(c->getDescription().containsIgnoreCase("retained"));}
 const auto full=r.document();drive->keyPressed(juce::KeyPress(juce::KeyPress::upKey));CHECK(r.document().values[mid]!=full.values[mid]&&std::abs(r.physical(otherMid)-9)<1e-5);CHECK(r.p.undoSoundEdit()&&r.document()==full);
 f.setOpen(true);const auto modeCount=r.gestures(modeIndex);clickAt(*mode,mode->element.hitRect.getTopLeft()+Pt(4,4));
 CHECK(r.physical(modeIndex)==1&&r.gestures(modeIndex).begins==modeCount.begins+1&&r.gestures(modeIndex).ends==modeCount.ends+1&&r.document().values[mid]==full.values[mid]);
 const auto live=r.document();f.refresh(live);r.b->refresh(live);auto proposal=std::make_shared<SoundDocument>(live);proposal->values[mid]=1;f.showProposal(proposal);CHECK(f.marks().size()==1&&f.marks()[0].element=="face.mid-drive");
 auto locked=live;locked.controlLocks[mid]=true;f.refresh(locked);CHECK(f.marks().empty());f.showProposal(nullptr);f.refresh(live);
 const auto* studio=studio::studioSkin();CHECK(studio);if(studio){studio::AllControlsPanel panel(r.p,*studio,{});panel.show({{NativeModule::saturation,6}});panel.refresh(live);
  CHECK(panel.parameters().size()==36);for(const auto index:panel.parameters())CHECK(panel.showParameter(index)&&panel.knob(index));}
 juce::MemoryBlock bytes;CHECK(encodeSoundState(live,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==live);
 auto missing=r.instance(r.first);missing.bindings.clear();CHECK(studio::MakerFace::forRack(r.p,skin,missing,r.host())==nullptr);
}
// Lumen Field keeps its existing factory identity and recipe; only its maker face changes.
RackDesign lumenDesign(){const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0");});CHECK(found!=designs.end());return found==designs.end()?RackDesign{}:*found;}
const juce::StringArray lumenFace{"face.size","face.decay","face.predelay","face.brightness","face.character","face.distance","face.width","face.mix","face.in"};
const juce::StringArray lumenDeck{"deck.duck-range","deck.duck-threshold","deck.duck-attack","deck.duck-release"};
void lumen_face_for(){
 std::cout<<"GROUP lumen_factory_face_for"<<std::endl;const auto original=lumenDesign();const auto skin=studio::makerFaceFor(original);CHECK(skin!=nullptr);if(!skin)return;
 CHECK(original.name=="Lumen Field"&&skin->design==original.id&&skin->maker=="MONEVIK AKUSTIK"&&skin->model=="Lumen Field"&&skin->units==4);
 CHECK(airack::test::frozenFactoryRackDesigns().size()==21&&airack::test::frozenLibraryRackDesigns().size()==28&&airack::test::frozenExampleRackDesigns().size()==6);
 {auto copy=original;copy.revision+=7;copy.intent="A different intention";copy.units[0].defaults.clear();CHECK(studio::makerFaceFor(copy)==nullptr);}
 const auto differs=[&](auto edit){auto copy=original;edit(copy);return studio::makerFaceFor(copy)==nullptr;};
 CHECK(differs([](auto& d){d.id=juce::Uuid();}));CHECK(differs([](auto& d){d.name="Lumen Field II";}));CHECK(differs([](auto& d){d.role=RackRole::synth;}));
 CHECK(differs([](auto& d){d.units[0].id=1;}));CHECK(differs([](auto& d){d.units[0].module=NativeModule::reverb;}));CHECK(differs([](auto& d){d.units.clear();}));CHECK(differs([](auto& d){d.panel.sections[0]="Room";}));
 for(std::size_t n=0;n<original.panel.controls.size();++n){
  CHECK(differs([&](auto& d){d.panel.controls[n].label+="!";}));CHECK(differs([&](auto& d){d.panel.controls[n].unit=1;}));
  CHECK(differs([&](auto& d){d.panel.controls[n].section=1;}));CHECK(differs([&](auto& d){d.panel.controls[n].key="space_reverb.duck_range_db";}));
  CHECK(differs([&](auto& d){d.panel.controls[n].widget=FocusWidget::verticalSlider;}));CHECK(differs([&](auto& d){d.panel.controls[n].minimum-=1;}));CHECK(differs([&](auto& d){d.panel.controls[n].maximum+=1;}));
  CHECK(differs([&](auto& d){d.panel.controls[n].response=d.panel.controls[n].response==FocusResponse::linear?FocusResponse::logarithmic:FocusResponse::linear;}));
 }
 CHECK(airack::test::frozenMakerSkinFile("lumen-field","elements.json").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("lumen-field","plates/thread-press-face.png").isEmpty()&&airack::test::frozenMakerSkinFile("thread-press","plates/lumen-field-face.png").isEmpty());
}
void lumen_skin_loads(){
 std::cout<<"GROUP lumen_skin_loads"<<std::endl;const auto design=lumenDesign();const auto skin=studio::makerFaceFor(design);CHECK(skin);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.panelWidth==2140&&std::abs(m.unitHeight-197.11f)<.01f&&m.units==4&&m.reveal.kind=="drawer"&&m.reveal.view=="deck"&&m.reveal.front=="front"&&m.reveal.units==3&&m.reveal.opener=="front.voicing");
 CHECK(m.reveal.titles==juce::StringArray({"Open voicing","Close voicing"}));
 const auto focus=[&](const char* view){juce::StringArray ids;for(const auto& g:m.groups)if(g.view==view)ids.addArray(g.focusOrder);return ids;};
 CHECK(focus("face")==lumenFace&&focus("deck")==lumenDeck&&focus("front")==juce::StringArray({"front.voicing"}));
 const auto doc=juce::JSON::parse(airack::test::frozenMakerSkinFile("lumen-field","elements.json").toString());const auto editor=doc["reveal"]["native_editor"];
 CHECK(editor.isObject()&&editor["type"].toString()=="space_reverb"&&int(editor["rack_unit"])==0&&editor["theme"].toString()=="amber_crt");
 CHECK(editor["rect"].isArray()&&editor["rect"].size()==4);
 CHECK(m.asset("face:plate").image.getWidth()==2140&&m.asset("face:plate").image.getHeight()==591&&m.asset("deck:plate").image.getWidth()==2140&&std::abs(m.asset("deck:plate").image.getHeight()-3*m.unitHeight)<1&&m.asset("front:plate").image.getHeight()==197);
 int sliders=0,levers=0,lamps=0;std::vector<juce::String> bound;
 for(const auto& e:m.elements)if(e.rackUnit>=0){
  CHECK(e.rackUnit==0&&rackParameterActive(NativeModule::spaceReverb,e.key)&&rackParameterIndex(NativeModule::spaceReverb,0,e.key).has_value());bound.push_back(e.key);
  if(e.kind=="rotary"||e.kind=="fader"){++sliders;CHECK(e.hasDot&&e.range.getStart()<=e.drawnValue&&e.drawnValue<=e.range.getEnd());}
  if(e.kind=="lever"){++levers;CHECK(e.key=="bypassed"&&e.hasDot&&e.positions.size()==2);}
  if(e.kind=="lamp"){++lamps;CHECK(e.key=="bypassed"&&e.litValue==0);}
 }
 std::sort(bound.begin(),bound.end());bound.erase(std::unique(bound.begin(),bound.end()),bound.end());CHECK(sliders==12&&levers==1&&lamps==1&&bound.size()==13);
 for(std::size_t n=0;n<8;++n){const auto* e=element(m,lumenFace[int(n)]);CHECK(e);if(e){const auto& f=design.panel.controls[n];CHECK(e->rackUnit==f.unit&&e->key==f.key&&std::abs(e->range.getStart()-f.minimum)<1.e-6&&std::abs(e->range.getEnd()-f.maximum)<1.e-6);CHECK(e->kind==(n>=6?"fader":"rotary"));}}
}
void lumen_angles(){
 std::cout<<"GROUP lumen_angles"<<std::endl;const auto skin=studio::makerFaceFor(lumenDesign());CHECK(skin);if(!skin)return;
 for(const auto& e:skin->skin.elements)if(e.kind=="rotary"){
  CHECK(e.ticks.size()>=2);if(e.ticks.empty())continue;for(const auto& tick:e.ticks)CHECK(std::abs(angleAt(e,tick.first)-tick.second)<.01f);
  CHECK(house::pointerAngleFor(e,-.2)==e.ticks.front().second&&house::pointerAngleFor(e,1.2)==e.ticks.back().second);
  float previous=-1000;for(int n=0;n<=100;++n){const auto angle=house::pointerAngleFor(e,n/100.);CHECK(angle>=previous);previous=angle;}
 }
}
struct LumenRig:Rig{
 LumenRig(){const auto placeFactory=[&](std::uint8_t slot,int at){RackOperation op;op.design=lumenDesign();op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,std::uint8_t(at)};op.allocation={{0,slot}};CHECK(p.applyRackOperation(op,revision())==RackCandidateStatus::ready);return op.newInstance;};first=placeFactory(2,0);second=placeFactory(7,1);show(1400,2800);}
};
void lumen_binds_its_banks(){
 std::cout<<"GROUP lumen_two_placements_exact_bindings"<<std::endl;LumenRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(2)},std::pair{r.b.get(),std::uint8_t(7)}}){
  CHECK(face->units()==4&&face->getWidth()==2140&&face->getHeight()==788&&face->front()!=nullptr);
  for(auto* view:{&face->face,&face->revealed()})for(const auto& control:view->sliders){const auto index=r.index(NativeModule::spaceReverb,slot,control->element.key.toRawUTF8());CHECK(control->element.paramIndex==index && &control->param==&r.p.control(index));}
  CHECK(face->face.lever("in")&&face->face.lamp("lamp"));if(auto* lever=face->face.lever("in"))CHECK(lever->element.paramIndex==r.index(NativeModule::spaceReverb,slot,"bypassed"));
  CHECK(face->parameters().size()==13);
 }
 auto broken=r.instance(r.first);broken.bindings.clear();const auto skin=studio::makerFaceFor(broken.design);CHECK(skin&&studio::MakerFace::forRack(r.p,skin,broken,r.host())==nullptr);
}
void lumen_gestures(){
 std::cout<<"GROUP lumen_main_and_trim_gestures_one_undo"<<std::endl;LumenRig r;if(!r.a||!r.b)return;auto& f=*r.a;
 for(bool open:{false,true}){
  f.setOpen(open,false);
  for(auto* view:{&f.face,&f.revealed()})if(view==&f.face||open)for(auto& control:view->sliders){
   const auto index=control->element.paramIndex;const auto before=r.document();const auto other=r.b->parameters();const auto count=r.gestures(index);const auto at=control->getLocalBounds().toFloat().getCentre();
   control->mouseDown(event(*control,at,juce::ModifierKeys::leftButtonModifier,1,false));control->mouseDrag(event(*control,at+Pt(0,-23),juce::ModifierKeys::leftButtonModifier,1,true,at));control->mouseUp(event(*control,at+Pt(0,-23),{},1,true,at));
   const auto after=r.document();CHECK(after.values[index]!=before.values[index]);CHECK(r.gestures(index)==Counts({count.begins+1,count.ends+1}));
   for(const auto parameter:other)CHECK(after.values[parameter]==before.values[parameter]);
   CHECK(r.p.undoSoundEdit()&&r.document()==before);CHECK(r.p.redoSoundEdit()&&r.document()==after);
  }
 }
}
void lumen_reveal(){
 std::cout<<"GROUP lumen_drawer_focus_and_no_sound_edit"<<std::endl;LumenRig r;if(!r.a)return;auto& f=*r.a;auto* pull=f.front()?f.front()->button("voicing"):nullptr;CHECK(pull);if(!pull)return;
 const auto before=r.document();const auto revision=r.revision();const auto closed=tabOrder(f);CHECK(closed==juce::StringArray({"face.size","face.decay","face.predelay","face.brightness","face.character","face.distance","face.width","face.mix","face.in","front.voicing"}));
 clickAt(*pull,pull->element.rect.getCentre());CHECK(f.isOpen()&&f.units()==7&&std::abs(f.getHeight()-7*f.skin.skin.unitHeight)<1&&f.revealed().isVisible()&&pull->getTitle()=="Close voicing");
 const auto opened=tabOrder(f);for(const auto& id:lumenDeck)CHECK(opened.contains(id));
 auto* trim=f.revealed().slider<Knob>("duck-range");CHECK(trim);if(!trim)return;trim->grabKeyboardFocus();CHECK(trim->hasKeyboardFocus(false));
 f.setOpen(false,false);CHECK(!f.isOpen()&&!f.revealed().isVisible()&&f.units()==4&&pull->hasKeyboardFocus(false)&&tabOrder(f)==closed);
 CHECK(!f.hasStrip());
 CHECK(r.revision()==revision&&r.document()==before);
}
void lumen_marks(){
 std::cout<<"GROUP lumen_trim_proposal_opener_and_lock"<<std::endl;LumenRig r;if(!r.a)return;auto& f=*r.a;auto* pull=f.front()?f.front()->button("voicing"):nullptr;CHECK(pull);if(!pull)return;const auto live=r.document();const auto trim=r.index(NativeModule::spaceReverb,2,"space_reverb.duck_range_db");
 auto proposed=std::make_shared<SoundDocument>(live);proposed->values[trim]=.75f;f.refresh(live);f.showProposal(proposed);CHECK(pull->getDescription().contains("1")&&pull->getDescription().contains("voicing"));
 f.setOpen(true,false);CHECK(pull->getDescription().isEmpty());f.setOpen(false,false);
 auto locked=live;locked.controlLocks[trim]=true;f.refresh(locked);CHECK(pull->getDescription().isEmpty());f.showProposal(nullptr);CHECK(f.marks().empty()&&!f.proposesPanel());
}
void lumen_compose(const juce::File& images){
 std::cout<<"GROUP lumen_drawn_closed_open_skin_snapshots"<<std::endl;LumenRig r;if(!r.a)return;auto& f=*r.a;auto values=r.document().values;
 for(const auto& e:f.skin.skin.elements)if(e.rackUnit>=0)if(const auto index=rackParameterIndex(NativeModule::spaceReverb,2,e.key))values[*index]=r.p.control(*index).convertTo0to1(float(e.drawnValue));
 const auto closed=f.compose(values,false),opened=f.compose(values,true);CHECK(closed.getWidth()==2140&&closed.getHeight()==788&&opened.getWidth()==2140&&std::abs(opened.getHeight()-7*f.skin.skin.unitHeight)<1);
 writeImages(images,{{"lumen-field-drawn.png",&closed},{"lumen-field-open-drawn.png",&opened}});
}
//------------------------------------------------------------------------------ Small Hours
const juce::StringArray smallFace{"face.program","face.afterglow","face.depth","face.swell","face.age","face.wow","face.grit","face.mix","face.in","face.adjust"},
                        smallAdjust{"face.pre-delay","face.diffusion","face.bass","face.damping","face.speed","face.low-cut","face.high-cut","face.close"};
constexpr auto erv=NativeModule::eraReverb;
// Each drawn control's key and the value its drawing shows, which is the design's recipe.
const std::vector<std::tuple<const char*,const char*,double>> smallDrawn{
 {"face.program","era_reverb.voicing",1},{"face.afterglow","era_reverb.decay_seconds",8},{"face.needle","era_reverb.decay_seconds",8},{"face.depth","era_reverb.size",.85},
 {"face.swell","era_reverb.attack",.6},{"face.age","era_reverb.age",0},{"face.wow","era_reverb.wobble",.4},{"face.grit","era_reverb.grit",.15},{"face.mix","era_reverb.mix",.3},
 {"face.in","bypassed",0},{"face.lamp","bypassed",0},{"face.pre-delay","era_reverb.predelay_ms",25},{"face.diffusion","era_reverb.diffusion",1},
 {"face.bass","era_reverb.bass_multiplier",1.2},{"face.damping","era_reverb.damping_hz",5000},{"face.speed","era_reverb.wobble_rate_hz",.7},{"face.low-cut","era_reverb.low_cut_hz",150},
 {"face.high-cut","era_reverb.high_cut_hz",9000}};
void small_hours_face_for(){
 std::cout<<"GROUP small_hours_face_for"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& small=examples[4];
 const auto skin=studio::makerFaceFor(small);CHECK(skin!=nullptr);
 if(skin)CHECK(skin->design==small.id&&skin->maker=="NACHTFELD"&&skin->model=="SMALL HOURS"&&skin->units==3&&skin->skin.elements.size()==20&&!skin->nativeEditor);
 {auto d=small;d.units[0].defaults[0].normalized=.6f;d.intent="Another intent";d.revision=3;CHECK(studio::makerFaceFor(d)==nullptr);}
 const auto differs=[&](auto edit){auto d=small;edit(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](RackDesign& d){d.id=juce::Uuid();}));CHECK(differs([](RackDesign& d){d.name="Small Hours II";}));CHECK(differs([](RackDesign& d){d.role=RackRole::synth;}));
 CHECK(differs([](RackDesign& d){d.units[0].module=NativeModule::spaceReverb;}));CHECK(differs([](RackDesign& d){d.units[0].id=1;}));CHECK(differs([](RackDesign& d){d.units.clear();}));
 CHECK(differs([](RackDesign& d){d.panel.sections[1]="Era";}));CHECK(differs([](RackDesign& d){d.panel.controls.pop_back();}));
 for(std::size_t n=0;n<small.panel.controls.size();++n){
  CHECK(differs([&](RackDesign& d){d.panel.controls[n].label+="!";}));CHECK(differs([&](RackDesign& d){d.panel.controls[n].unit=1;}));
  CHECK(differs([&](RackDesign& d){d.panel.controls[n].section=std::uint8_t((d.panel.controls[n].section+1)%3);}));CHECK(differs([&](RackDesign& d){d.panel.controls[n].key="era_reverb.diffusion";}));
  CHECK(differs([&](RackDesign& d){d.panel.controls[n].widget=d.panel.controls[n].widget==FocusWidget::rotary?FocusWidget::verticalSlider:FocusWidget::rotary;}));
  CHECK(differs([&](RackDesign& d){d.panel.controls[n].minimum-=1;}));CHECK(differs([&](RackDesign& d){d.panel.controls[n].maximum+=1;}));
  CHECK(differs([&](RackDesign& d){d.panel.controls[n].response=d.panel.controls[n].response==FocusResponse::linear?FocusResponse::logarithmic:FocusResponse::linear;}));
 }
 for(std::size_t i=0;i<4;++i){const auto other=studio::makerFaceFor(examples[i]);CHECK(other!=nullptr&&other!=skin);}
 CHECK(airack::test::frozenMakerSkinFile("small-hours","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("small-hours","plates/small-hours-face.png").getSize()>0
       &&airack::test::frozenMakerSkinFile("small-hours","plates/small-hours-adjust.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("oxide-return","plates/small-hours-face.png").isEmpty()&&airack::test::frozenMakerSkinFile("small-hours","plates/oxide-return-face.png").isEmpty());
}
void small_hours_skin_loads(){
 std::cout<<"GROUP small_hours_skin_loads"<<std::endl;const auto small=airack::test::frozenExampleRackDesigns()[4];
 const auto skin=studio::makerFaceFor(small);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.elements.size()==20&&m.designId==small.id&&m.maker=="NACHTFELD"&&m.modelName=="SMALL HOURS"&&m.units==3);
 CHECK(m.panelWidth==2140.0f&&std::abs(m.unitHeight-197.11f)<.01f&&m.strip.isEmpty()&&m.pages.empty()&&m.hostReadout);
 std::map<juce::String,int> views,kinds;for(const auto& e:m.elements){++views[e.view];++kinds[e.kind];}
 CHECK(views.size()==2&&views["face"]==12&&views["adjust"]==8);
 CHECK(kinds["rotary"]==13&&kinds["selector"]==1&&kinds["lever"]==2&&kinds["lamp"]==1&&kinds["needle"]==1&&kinds["action"]==2);
 CHECK(m.reveal.kind=="recess"&&m.reveal.view=="adjust"&&m.reveal.front.isEmpty()&&m.reveal.units==0&&m.reveal.opener=="face.adjust"
       &&m.reveal.closers==juce::StringArray({"face.close"})&&m.reveal.titles.isEmpty());
 const auto& motion=m.reveal.motion;
 CHECK(motion.ms==220&&motion.hinge=="top"&&std::abs(motion.openHeight-12.01f)<.01f&&motion.turns.empty()&&motion.slide.isOrigin()&&motion.door.isEmpty());
 const auto* face=group(m,"face.face");const auto* adjust=group(m,"face.adjust");CHECK(face&&adjust&&m.groups.size()==2);
 if(face)CHECK(face->focusOrder==smallFace&&face->view=="face");
 if(adjust)CHECK(adjust->focusOrder==smallAdjust&&adjust->view=="adjust");
 std::vector<juce::String> keys;
 for(const auto& e:m.elements){
  CHECK(e.id.startsWith("face.")&&e.label.isNotEmpty());
  if(e.rackUnit<0){CHECK(e.kind=="action"&&!e.rect.isEmpty());continue;}
  CHECK(e.rackUnit==0&&e.paramId.isEmpty()&&rackParameterActive(erv,e.key));keys.push_back(e.key);
  const auto index=rackParameterIndex(erv,0,e.key);CHECK(index.has_value());if(!index)continue;const auto& d=parameterRegistry()[*index];
  if(e.kind=="rotary"||e.kind=="needle"){
   CHECK(std::abs(e.range.getStart()-d.minimum)<1e-4&&std::abs(e.range.getEnd()-d.maximum)<1e-4&&e.ticks.size()>=2&&m.assets.count(e.layer(e.kind=="needle"?"needle":"pointer"))==1);
   CHECK(e.drawnValue>=e.range.getStart()&&e.drawnValue<=e.range.getEnd());
   if(e.kind=="rotary")CHECK(e.hasDot&&e.hitCentre()==e.pivot);
   if(e.registryResponse)CHECK(std::abs(e.skew-d.skew)<1e-3);
  }
  if(e.kind=="selector"||e.kind=="lever"){
   CHECK(e.hasDot&&std::any_of(e.positions.begin(),e.positions.end(),[&](const auto& p){return p.value==e.drawnValue;}));
   for(const auto& p:e.positions)CHECK(p.legend.isNotEmpty());
   if(e.bindingType=="choice"){juce::StringArray names;for(const auto& c:d.choices)names.add(c);CHECK(e.choices==names&&(int)e.positions.size()==names.size());}
  }
 }
 std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());CHECK(keys.size()==16);
 for(const auto& [id,key,drawn]:smallDrawn)if(const auto* e=element(m,id))CHECK(e->key==key&&(e->kind=="lamp"||std::abs(e->drawnValue-drawn)<1e-4));else CHECK(!"missing control");
 // The design's eight focused controls, in its order: each one's binding, range and response.
 for(std::size_t n=0;n<8;++n)if(const auto* e=element(m,smallFace[(int)n])){
  const auto& c=small.panel.controls[n];const auto choice=c.widget==FocusWidget::choice;
  CHECK(e->rackUnit==c.unit&&e->key==c.key&&e->logarithmic==(c.response==FocusResponse::logarithmic)&&e->kind==(choice?(n==0?"selector":"lever"):"rotary"));
  if(choice)CHECK(c.minimum==0&&e->choices.size()==juce::roundToInt(c.maximum)+1);
  else CHECK(std::abs(e->range.getStart()-c.minimum)<1e-4&&std::abs(e->range.getEnd()-c.maximum)<1e-4);
 }
 // PROGRAM's six stations round its skirt, each with its printed legend in its hit area.
 if(const auto* e=element(m,"face.program")){
  CHECK(e->bindingType=="choice"&&e->positions.size()==6&&e->faceR>40&&m.assets.count(e->layer("pointer"))==1&&!e->hitRect.isEmpty());
  const char* printed[]{"HALL","SPACE","TAPE","PLATE","ROOM","GATE"};
  for(std::size_t i=0;i<e->positions.size()&&i<6;++i){const auto& p=e->positions[i];
   CHECK(p.value==(double)i&&p.legend==printed[i]&&!p.legendRect.isEmpty()&&e->hitRect.contains(p.legendRect));if(i>0)CHECK(p.angle>e->positions[i-1].angle);}
 }
 else CHECK(!"missing PROGRAM");
 // AGE's detents left to right DARK, WARM and CLEAR, their thirds tiling the slot; the right one first, so that Right and
 // Up step towards CLEAR.
 if(const auto* e=element(m,"face.age")){
  CHECK(e->bindingType=="choice"&&e->choices==juce::StringArray({"Dark","Warm","Clear"})&&e->legendClick&&e->positions.size()==3);
  if(e->positions.size()==3){const auto& clear=e->positions[0];const auto& warm=e->positions[1];const auto& dark=e->positions[2];
   CHECK(clear.value==2&&clear.legend=="CLEAR"&&warm.value==1&&warm.legend=="WARM"&&dark.value==0&&dark.legend=="DARK");
   CHECK(std::abs(dark.legendRect.getRight()-warm.legendRect.getX())<.5f&&std::abs(warm.legendRect.getRight()-clear.legendRect.getX())<.5f);
   for(const auto& p:e->positions)CHECK(m.assets.count(p.asset)==1&&e->rect.expanded(.01f).contains(p.legendRect));}
 }
 else CHECK(!"missing AGE");
 // IN / OUT inverted: up is IN, bypassed off; its legends above and below the escutcheon take presses too.
 if(const auto* e=element(m,"face.in"))CHECK(e->bindingType=="toggle"&&!e->legendClick&&e->positions.size()==2&&e->positions[0].value==0&&e->positions[0].legend=="IN"
                                            &&e->positions[0].asset=="face:in.in"&&e->positions[1].value==1&&e->positions[1].legend=="OUT"&&e->positions[1].asset=="face:in.out"
                                            &&e->hitRect.contains(e->rect)&&e->hitRect.getHeight()>e->rect.getHeight());
 else CHECK(!"missing IN / OUT");
 if(const auto* e=element(m,"face.lamp"))CHECK(e->litValue==0&&e->lensR>10&&e->layer("lit")=="face:lamp.lit");else CHECK(!"missing lamp");
 // The needle on the dial's nine printed steps; AFTERGLOW's dimple turns twice over the range.
 if(const auto* e=element(m,"face.needle")){
  CHECK(e->logarithmic&&e->range==juce::Range<double>(.2,70)&&e->ticks.size()==9&&e->y0>0&&!e->hasDot&&m.assets.count(e->layer("needle"))==1);
  const double steps[]{.2,.5,1,2,4,8,16,32,70};
  for(std::size_t i=0;i<e->ticks.size()&&i<9;++i){CHECK(e->ticks[i].first==steps[i]);if(i>0)CHECK(e->ticks[i].second>e->ticks[i-1].second);}
 }
 else CHECK(!"missing needle");
 if(const auto* e=element(m,"face.afterglow"))CHECK(e->logarithmic&&e->unit=="s"&&e->defaultValue==4&&e->ticks.size()==2&&std::abs(e->ticks.back().second-e->ticks.front().second-720.0f)<.01f);
 else CHECK(!"missing AFTERGLOW");
 for(const char* id:{"face.depth","face.swell","face.wow","face.grit","face.mix"})
  if(const auto* e=element(m,id))CHECK(e->unit=="%"&&!e->logarithmic&&!e->registryResponse&&e->ticks.size()==11&&e->layer("pointer")=="face:knob.pointer");else CHECK(!"missing knob");
 // The trims under the lid, each pressed anywhere in its deck column: the registry's skewed travel where it has one.
 for(const auto& id:smallAdjust)if(const auto* e=element(m,id);e&&e->kind=="rotary"){
  const auto skewed=id=="face.pre-delay"||id=="face.bass"||id=="face.damping"||id=="face.high-cut";
  CHECK(e->view=="adjust"&&e->ticks.size()==3&&!e->hitRect.isEmpty()&&e->hitRect.contains(e->pivot)&&e->registryResponse==skewed&&!e->logarithmic&&e->layer("pointer")=="adjust:trim.pointer");}
 const auto& plate=m.asset("face:plate");const auto& deck=m.asset("adjust:plate");
 CHECK(plate.image.getWidth()==2140&&plate.image.getHeight()==591&&deck.image.isValid()&&deck.anchor==Pt(82.0f,427.0f));
 const juce::Rectangle<float> recess(deck.anchor.x,deck.anchor.y,(float)deck.image.getWidth(),(float)deck.image.getHeight());
 if(const auto* e=element(m,"face.adjust"))CHECK(e->view=="face"&&rectNear(e->rect,motion.lid)&&recess.contains(e->rect));else CHECK(!"missing ADJUST");
 // The open strip along the hinge is the closer.
 if(const auto* e=element(m,"face.close"))CHECK(e->view=="adjust"&&e->rect.expanded(.01f).contains(motion.lid.withHeight(motion.openHeight)));else CHECK(!"missing close");
}
void small_hours_angles(){
 std::cout<<"GROUP small_hours_angles"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[4]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 const auto at=[](const house::Element& e,double v){return house::pointerAngleFor(e,house::proportionOf(e,v));};
 int scales=0;
 for(const auto& e:m.elements){
  if(e.kind!="rotary"&&e.kind!="needle")continue;
  ++scales;
  for(std::size_t i=0;i<e.ticks.size();++i){CHECK(std::abs(at(e,e.ticks[i].first)-e.ticks[i].second)<.01f);if(i>0)CHECK(e.ticks[i].first>e.ticks[i-1].first&&e.ticks[i].second>e.ticks[i-1].second);}
  // The printed scale spans the range, pinning beyond it, and rises between.
  CHECK(e.ticks.front().first==e.range.getStart()&&e.ticks.back().first==e.range.getEnd());
  CHECK(house::pointerAngleFor(e,-.2)==e.ticks.front().second&&house::pointerAngleFor(e,1.2)==e.ticks.back().second);
  float last=-1000;for(int k=0;k<=50;++k){const auto a=house::pointerAngleFor(e,k/50.0);CHECK(a>=last);last=a;}
 }
 CHECK(scales==14);
 // AFTERGLOW's dimple stands where the drawing shows 8 s, half-way round at the range's geometric middle; the needle's 3 s
 // lies between its 2 s and 4 s steps at the share of their log distance.
 if(const auto* e=element(m,"face.afterglow"))CHECK(std::abs(at(*e,8)-43.81f)<.05f&&std::abs(at(*e,std::sqrt(.2*70))-.5f*(e->ticks.front().second+e->ticks.back().second))<.01f);
 if(const auto* e=element(m,"face.needle")){const auto lo=tickAngle(*e,2),hi=tickAngle(*e,4);CHECK(std::abs(at(*e,3)-(lo+(hi-lo)*(float)(std::log(1.5)/std::log(2.0))))<.01f);}
 // The fluted knobs print 0 to 100 in tens, linear between them.
 if(const auto* e=element(m,"face.depth"))CHECK(std::abs(at(*e,.85)-.5f*(tickAngle(*e,.8)+tickAngle(*e,.9)))<.01f);
 // A trim's printed centre is the middle of the registry's travel: PRE-DELAY's 100 ms, DAMPING's 6 kHz.
 if(const auto* e=element(m,"face.pre-delay"))CHECK(std::abs(house::proportionOf(*e,100)-.5)<.002&&at(*e,50)>tickAngle(*e,0)&&at(*e,50)<tickAngle(*e,100));
 if(const auto* e=element(m,"face.damping"))CHECK(std::abs(house::proportionOf(*e,6000)-.5)<.002);
}
// Two Small Hours on the shared master: the first's Era Reverb in effect bank 1, the second's in bank 2.
struct SmallRig:Rig{
 SmallRig(){first=place(4,{RackRole::fx,4,0},{{0,0}});second=place(4,{RackRole::fx,4,1},{{0,1}});show(600,1200);}
 std::size_t era(std::uint8_t slot,const char* key){return index(erv,slot,key);}
};
void small_hours_binds_its_banks(){
 std::cout<<"GROUP small_hours_binds_its_banks"<<std::endl;SmallRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(0)},std::pair{r.b.get(),std::uint8_t(1)}}){
  CHECK(face->getComponentID()=="rack.face"&&face->getWidth()==2140&&face->getHeight()==591&&face->units()==3);
  CHECK(!face->hasStrip()&&face->front()==nullptr&&face->hanging()==nullptr&&face->getTitle()=="NACHTFELD, SMALL HOURS");
  const auto knob=[&](studio::house::View& v,const char* id){auto* k=v.slider<Knob>(id);CHECK(k!=nullptr);return k?k->element.paramIndex:~std::size_t();};
  const auto lever=[&](studio::house::View& v,const char* id){auto* l=v.lever(id);CHECK(l!=nullptr);return l?l->element.paramIndex:~std::size_t();};
  const std::pair<const char*,const char*> onFace[]{{"afterglow","era_reverb.decay_seconds"},{"depth","era_reverb.size"},{"swell","era_reverb.attack"},{"wow","era_reverb.wobble"},
                                                    {"grit","era_reverb.grit"},{"mix","era_reverb.mix"}};
  for(const auto& [id,key]:onFace)CHECK(knob(face->face,id)==r.era(slot,key));
  const std::pair<const char*,const char*> underLid[]{{"pre-delay","era_reverb.predelay_ms"},{"diffusion","era_reverb.diffusion"},{"bass","era_reverb.bass_multiplier"},
                                                      {"damping","era_reverb.damping_hz"},{"speed","era_reverb.wobble_rate_hz"},{"low-cut","era_reverb.low_cut_hz"},{"high-cut","era_reverb.high_cut_hz"}};
  for(const auto& [id,key]:underLid)CHECK(knob(face->revealed(),id)==r.era(slot,key));
  CHECK(lever(face->face,"program")==r.era(slot,"era_reverb.voicing")&&lever(face->face,"age")==r.era(slot,"era_reverb.age")&&lever(face->face,"in")==r.era(slot,"bypassed"));
  {auto* needle=face->face.needle("needle");CHECK(needle!=nullptr&&needle->element.paramIndex==r.era(slot,"era_reverb.decay_seconds"));}
  {auto* lamp=face->face.lamp("lamp");CHECK(lamp!=nullptr&&lamp->element.paramIndex==r.era(slot,"bypassed")&&lamp->lit());}
  // All sixteen of the unit's parameters, its own bank's.
  const auto ps=face->parameters();CHECK(ps.size()==16);
  for(const auto p:ps)CHECK(p<parameterCount&&parameterRegistry()[p].id.startsWith("effect."+juce::String(slot+1)+"."));
  if(auto* afterglow=face->face.slider<Knob>("afterglow"))CHECK(&afterglow->param==&r.p.control(r.era(slot,"era_reverb.decay_seconds")));
 }
 {auto broken=r.instance(r.first);broken.bindings.clear();CHECK(studio::MakerFace::forRack(r.p,studio::makerFaceFor(broken.design),broken,r.host())==nullptr);}
}
// AFTERGLOW's flywheel: 60 of the 300 px of a full travel up from 8 s is a fifth of its log travel, one gesture on its
// own rack's parameter; the wheel, a key and a double-click one each. Every knob and trim, the lid open or closed: one
// drag is one gesture on its own parameter alone, one undo step.
void small_hours_gestures(){
 std::cout<<"GROUP small_hours_gestures"<<std::endl;SmallRig r;if(!r.a||!r.b)return;auto& f=*r.a;
 auto* flywheel=f.face.slider<Knob>("afterglow");CHECK(flywheel!=nullptr);if(!flywheel)return;
 const auto& e=flywheel->element;const auto d1=r.era(0,"era_reverb.decay_seconds"),d2=r.era(1,"era_reverb.decay_seconds");
 CHECK(std::abs(r.physical(d1)-8)<1e-3);
 auto g0=r.gestures(d1);const auto other=r.gestures(d2);
 drag(*flywheel,e.pivot,e.pivot-Pt(0.0f,60.0f));auto g1=r.gestures(d1);
 const auto want=.2*std::pow(350.0,proportion(e,8)+.2);
 CHECK(std::abs(r.physical(d1)-want)<want*.02&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&r.gestures(d2)==other&&std::abs(r.physical(d2)-8)<1e-3);
 g0=g1;const auto before=r.physical(d1);flywheel->mouseWheelMove(event(*flywheel,local(*flywheel,e.pivot),{},0,false),wheel(0.1f));g1=r.gestures(d1);
 CHECK(r.physical(d1)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto p0=flywheel->proportion();flywheel->keyPressed(juce::KeyPress(juce::KeyPress::upKey));g1=r.gestures(d1);
 CHECK(std::abs(flywheel->proportion()-p0-0.01)<0.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;flywheel->mouseDoubleClick(event(*flywheel,local(*flywheel,e.pivot),{},2,false));g1=r.gestures(d1);
 CHECK(std::abs(r.physical(d1)-4)<1e-3&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 for(bool open:{false,true}){
  f.setOpen(open,false);
  for(auto* view:{&f.face,&f.revealed()})if(view==&f.face||open)for(auto& control:view->sliders){
   // Down: DIFFUSION's recipe is its maximum.
   const auto index=control->element.paramIndex;const auto doc=r.document();const auto others=r.b->parameters();const auto count=r.gestures(index);const auto at=control->getLocalBounds().toFloat().getCentre();
   control->mouseDown(event(*control,at,juce::ModifierKeys::leftButtonModifier,1,false));control->mouseDrag(event(*control,at+Pt(0,23),juce::ModifierKeys::leftButtonModifier,1,true,at));
   control->mouseUp(event(*control,at+Pt(0,23),{},1,true,at));
   const auto after=r.document();CHECK(after.values[index]!=doc.values[index]);CHECK(r.gestures(index)==Counts({count.begins+1,count.ends+1}));
   for(const auto parameter:others)CHECK(after.values[parameter]==doc.values[parameter]);
   CHECK(r.p.undoSoundEdit()&&r.document()==doc);CHECK(r.p.redoSoundEdit()&&r.document()==after);
  }
 }
}
// The lamp is lit while the reverb is IN, its own rack's; it takes no press or focus.
void small_hours_lamp(){
 std::cout<<"GROUP small_hours_lamp"<<std::endl;SmallRig r;if(!r.a||!r.b)return;
 auto* lamp=r.a->face.lamp("lamp");auto* other=r.b->face.lamp("lamp");auto* in=r.a->face.lever("in");CHECK(lamp&&other&&in);if(!lamp||!other||!in)return;
 const auto by=r.era(0,"bypassed");
 {bool self=true,children=true;lamp->getInterceptsMouseClicks(self,children);CHECK(!self&&!children&&!lamp->getWantsKeyboardFocus());}
 const auto lens=juce::Rectangle<float>(2*lamp->element.lensR,2*lamp->element.lensR).withCentre(lamp->element.led).getSmallestIntegerContainer();
 const auto shot=[&]{return r.a->createComponentSnapshot(r.a->getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
 r.a->face.hideReadouts();
 CHECK(r.physical(by)<.5&&lamp->lit()&&other->lit()&&in->legend()=="IN");const auto lit=shot();
 r.automate(by,1.0);CHECK(!lamp->lit()&&other->lit()&&in->legend()=="OUT"&&maxDiffIn(shot(),lit,lens)>40);
 r.automate(by,0.0);CHECK(lamp->lit()&&in->legend()=="IN"&&maxDiffIn(shot(),lit,lens)==0);
}
// PROGRAM, AGE and IN / OUT. PROGRAM's printed stations each choose their voicing, a press off them steps on, wrapping,
// and the keys and the wheel step, stopping at the ends. AGE's detents and legends throw its paddle, one gesture, and it
// draws no hover. IN / OUT is inverted, up IN; its lamp follows. Each is its own rack's, and a held gesture refuses them.
void small_hours_selector_and_levers(){
 std::cout<<"GROUP small_hours_selector_and_levers"<<std::endl;SmallRig r;if(!r.a||!r.b)return;auto& f=*r.a;
 auto* program=f.face.lever("program");auto* age=f.face.lever("age");auto* in=f.face.lever("in");CHECK(program&&age&&in);if(!program||!age||!in)return;
 const auto step=[&](Lever& l,auto act,const char* legend,int gestures){const auto i=l.element.paramIndex;const auto g0=r.gestures(i);act();const auto g1=r.gestures(i);
  const auto ok=l.legend()==legend&&g1.begins==g0.begins+gestures&&g1.ends==g0.ends+gestures;if(!ok)std::cout<<"  "<<l.element.id<<" at "<<l.legend()<<", not "<<legend<<std::endl;return ok;};
 const auto click=[](Lever& l,Pt at){return [&l,at]{clickAt(l,at);};};
 const auto key=[](Lever& l,int code){return [&l,code]{l.keyPressed(juce::KeyPress(code));};};
 const auto roll=[](Lever& l,float dy){return [&l,dy]{l.mouseWheelMove(event(l,local(l,l.element.pivot),{},0,false),wheel(dy));};};
 const auto pv=program->element.paramIndex;
 CHECK(program->getTitle()=="PROGRAM"&&program->legend()=="SPACE"&&juce::roundToInt(r.physical(pv))==1&&program->element.positions.size()==6);
 if(program->element.positions.size()!=6)return;
 const char* stations[]{"HALL","SPACE","TAPE","PLATE","ROOM","GATE"};
 for(int i:{5,0,3,2,4,1})CHECK(step(*program,click(*program,program->element.positions[(std::size_t)i].legendRect.getCentre()),stations[i],1)&&juce::roundToInt(r.physical(pv))==i);
 CHECK(step(*program,click(*program,program->element.positions[1].legendRect.getCentre()),"SPACE",0));
 r.automate(pv,4.0);
 CHECK(step(*program,click(*program,program->element.pivot),"GATE",1)&&step(*program,click(*program,program->element.pivot),"HALL",1));
 CHECK(step(*program,key(*program,juce::KeyPress::downKey),"HALL",0)&&step(*program,key(*program,juce::KeyPress::leftKey),"HALL",0)&&step(*program,roll(*program,-0.1f),"HALL",0));
 CHECK(step(*program,key(*program,juce::KeyPress::upKey),"SPACE",1)&&step(*program,key(*program,juce::KeyPress::rightKey),"TAPE",1)&&step(*program,roll(*program,0.1f),"PLATE",1));
 r.automate(pv,5.0);
 CHECK(step(*program,key(*program,juce::KeyPress::upKey),"GATE",0)&&step(*program,roll(*program,0.1f),"GATE",0)
       &&step(*program,key(*program,juce::KeyPress::spaceKey),"HALL",1)&&step(*program,key(*program,juce::KeyPress::returnKey),"SPACE",1));
 CHECK(r.b->face.lever("program")->legend()=="SPACE");
 const auto av=age->element.paramIndex;
 CHECK(age->getTitle()=="AGE"&&age->legend()=="DARK"&&juce::roundToInt(r.physical(av))==0&&age->element.positions.size()==3);if(age->element.positions.size()!=3)return;
 const auto third=[&](std::size_t i){return age->element.positions[i].legendRect;};
 CHECK(step(*age,click(*age,third(0).getCentre()),"CLEAR",1)&&juce::roundToInt(r.physical(av))==2);
 CHECK(step(*age,click(*age,{third(1).getCentreX(),third(1).getBottom()-6.0f}),"WARM",1)&&juce::roundToInt(r.physical(av))==1);
 CHECK(step(*age,click(*age,{third(2).getX()+6.0f,third(2).getY()+6.0f}),"DARK",1)&&step(*age,click(*age,third(2).getCentre()),"DARK",0));
 CHECK(step(*age,key(*age,juce::KeyPress::leftKey),"DARK",0)&&step(*age,key(*age,juce::KeyPress::downKey),"DARK",0)&&step(*age,key(*age,juce::KeyPress::rightKey),"WARM",1)
       &&step(*age,key(*age,juce::KeyPress::upKey),"CLEAR",1)&&step(*age,key(*age,juce::KeyPress::rightKey),"CLEAR",0));
 CHECK(step(*age,roll(*age,-0.1f),"WARM",1)&&step(*age,roll(*age,0.1f),"CLEAR",1));
 CHECK(step(*age,key(*age,juce::KeyPress::spaceKey),"WARM",1)&&step(*age,key(*age,juce::KeyPress::returnKey),"DARK",1)&&step(*age,key(*age,juce::KeyPress::spaceKey),"CLEAR",1));
 {f.face.hideReadouts();const auto shot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
  const auto plain=shot();age->hover=true;age->repaint();const auto d=maxDiff(shot(),plain);age->hover=false;age->repaint();CHECK(d==0);}
 const auto by=in->element.paramIndex;auto* lamp=f.face.lamp("lamp");CHECK(lamp!=nullptr);if(!lamp)return;
 CHECK(in->getTitle()=="IN / OUT"&&in->legend()=="IN"&&r.physical(by)<.5&&lamp->lit());
 CHECK(step(*in,click(*in,in->element.rect.getCentre()),"OUT",1)&&r.physical(by)>.5&&!lamp->lit());
 CHECK(step(*in,click(*in,{in->element.hitRect.getCentreX(),in->element.hitRect.getY()+6.0f}),"IN",1)&&lamp->lit());
 CHECK(step(*in,key(*in,juce::KeyPress::upKey),"IN",0)&&step(*in,key(*in,juce::KeyPress::downKey),"OUT",1)&&step(*in,key(*in,juce::KeyPress::downKey),"OUT",0)
       &&step(*in,key(*in,juce::KeyPress::upKey),"IN",1));
 CHECK(step(*in,key(*in,juce::KeyPress::spaceKey),"OUT",1)&&step(*in,key(*in,juce::KeyPress::returnKey),"IN",1));
 CHECK(r.b->face.lever("in")->legend()=="IN"&&r.physical(r.era(1,"bypassed"))<.5&&r.b->face.lever("age")->legend()=="DARK");
 const auto held=r.era(0,"era_reverb.mix");CHECK(r.p.beginParameterGesture(held));r.reported.clear();
 clickAt(*in,in->element.rect.getCentre());clickAt(*age,third(2).getCentre());
 CHECK(in->legend()=="IN"&&age->legend()=="CLEAR"&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
// The lid: ADJUST opens it in place, the faceplate keeping its 3U; Tab reaches the trims and CLOSE only while it is
// open; closing with focus inside hands it to the lid, with focus outside leaves it there; neither writes anything.
void small_hours_lid(){
 std::cout<<"GROUP small_hours_lid"<<std::endl;SmallRig r;if(!r.a)return;auto& f=*r.a;
 auto* lid=f.face.button("adjust");auto* close=f.revealed().button("close");CHECK(lid!=nullptr&&close!=nullptr);if(!lid||!close)return;
 CHECK(lid->getTitle()=="Open adjust lid"&&close->getTitle()=="Close adjust lid");
 auto open=smallFace;open.addArray(smallAdjust);
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&tabOrder(f)==smallFace&&f.units()==3);
 const auto bounds=f.getBounds();const auto rev=r.revision();const auto values=r.document().values;int opened=0;f.onOpen=[&]{++opened;};
 clickAt(*lid,lid->element.rect.getCentre());
 CHECK(f.isOpen()&&f.revealed().isVisible()&&f.getBounds()==bounds&&opened==1&&f.units()==3);
 CHECK(tabOrder(f)==open);if(tabOrder(f)!=open)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 auto* damping=f.revealed().slider<Knob>("damping");CHECK(damping!=nullptr);if(!damping)return;
 damping->grabKeyboardFocus();CHECK(damping->hasKeyboardFocus(false));
 close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&juce::Component::getCurrentlyFocusedComponent()==lid&&opened==2&&tabOrder(f)==smallFace);
 auto* mix=f.face.slider<Knob>("mix");mix->grabKeyboardFocus();
 lid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(f.isOpen()&&mix->hasKeyboardFocus(false));
 clickAt(*close,{close->element.rect.getX()+12.0f,close->element.rect.getBottom()-4.0f});CHECK(!f.isOpen()&&mix->hasKeyboardFocus(false));
 CHECK(r.revision()==rev&&r.document().values==values&&f.getBounds()==bounds);
 // Open, a trim takes presses anywhere in its deck column.
 f.setOpen(true);
 {const auto cell=damping->element.hitRect;CHECK(damping->hitTest(juce::roundToInt(cell.getX()+4.0f-(float)damping->getX()),juce::roundToInt(cell.getBottom()-4.0f-(float)damping->getY())));}
}
// The AI's marks: AFTERGLOW's dashed pointer and IN / OUT's outline on the face; DAMPING's only while the lid is open, and
// the lid outlined while it is closed; a locked control and the same values get none.
void small_hours_marks(const juce::File& images){
 std::cout<<"GROUP small_hours_marks"<<std::endl;SmallRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* flywheel=f.face.slider<Knob>("afterglow");auto* in=f.face.lever("in");auto* lid=f.face.button("adjust");auto* damping=f.revealed().slider<Knob>("damping");
 CHECK(flywheel&&in&&lid&&damping);if(!flywheel||!in||!lid||!damping)return;
 const auto d=r.era(0,"era_reverb.decay_seconds"),by=r.era(0,"bypassed"),dm=r.era(0,"era_reverb.damping_hz");
 const auto live=r.document();auto candidate=live;
 const auto set=[&](SoundDocument& doc,std::size_t i,double v){doc.values[i]=r.p.control(i).convertTo0to1((float)v);};
 set(candidate,d,16.0);set(candidate,by,1.0);set(candidate,dm,3000.0);
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(live);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 f.face.hideReadouts();const auto plain=snapshot();f.setOpen(true);f.revealed().hideReadouts();const auto plainOpen=snapshot();f.setOpen(false);
 f.showProposal(std::make_shared<const SoundDocument>(candidate));
 const auto marks=f.marks();CHECK(marks.size()==3&&!f.proposesPanel());
 if(marks.size()==3)CHECK(marks[0].element=="face.afterglow"&&std::abs(marks[0].proposed-16.0f)<.05f&&marks[1].element=="face.in"&&marks[1].proposed==1.0f
                          &&marks[2].element=="face.damping"&&std::abs(marks[2].proposed-3000.0f)<5.0f);
 CHECK(flywheel->getDescription()=="AI proposes 16.00 s"&&in->getDescription()=="AI proposes OUT"&&damping->getDescription()=="AI proposes 3.0 kHz");
 CHECK(lid->getDescription()=="AI proposes changes to 1 adjust lid control");
 if(lid->getDescription()!="AI proposes changes to 1 adjust lid control")std::cout<<"  the lid's description: "<<lid->getDescription()<<std::endl;
 const auto pointer=[](const studio::house::Element& e,double v){const auto a=juce::degreesToRadians(house::pointerAngleFor(e,house::proportionOf(e,v)));
  return line(e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.35f,e.pivot+Pt(std::sin(a),-std::cos(a))*e.faceR*0.95f);};
 const auto flywheelLine=pointer(flywheel->element,16.0),dampingLine=pointer(damping->element,3000.0);
 const auto inEdge=outline(in->element.rect.expanded(3.0f)),lidEdge=outline(lid->element.rect.expanded(3.0f));
 const auto marked=snapshot();
 CHECK(changedAlong(marked,plain,flywheelLine)>0.35&&changedAlong(marked,plain,inEdge)>0.35&&changedAlong(marked,plain,lidEdge)>0.35&&changedAlong(marked,plain,dampingLine)<0.05);
 std::cout<<"  closed: AFTERGLOW "<<changedAlong(marked,plain,flywheelLine)<<", IN / OUT "<<changedAlong(marked,plain,inEdge)<<", lid "<<changedAlong(marked,plain,lidEdge)
          <<", DAMPING "<<changedAlong(marked,plain,dampingLine)<<std::endl;
 f.setOpen(true);const auto markedOpen=snapshot();
 CHECK(changedAlong(markedOpen,plainOpen,dampingLine)>0.35&&changedAlong(markedOpen,plainOpen,lidEdge)<0.05&&lid->getDescription().isEmpty());
 std::cout<<"  open: DAMPING "<<changedAlong(markedOpen,plainOpen,dampingLine)<<", lid "<<changedAlong(markedOpen,plainOpen,lidEdge)<<std::endl;
 f.setOpen(false);CHECK(lid->getDescription()=="AI proposes changes to 1 adjust lid control");
 writeImages(images,{{"small-hours-marks.png",&marked},{"small-hours-marks-open.png",&markedOpen}});
 {auto locked=live;locked.controlLocks[d]=true;f.refresh(locked);
  CHECK(f.marks().size()==2&&f.marks()[0].element=="face.in"&&flywheel->getDescription().isEmpty()&&changedAlong(snapshot(),plain,flywheelLine)<0.05);}
 f.refresh(live);CHECK(f.marks().size()==3);
 f.showProposal(std::make_shared<const SoundDocument>(live));
 CHECK(f.marks().empty()&&flywheel->getDescription().isEmpty()&&lid->getDescription().isEmpty()&&maxDiff(snapshot(),plain)==0);
 f.showProposal(nullptr);CHECK(f.marks().empty()&&maxDiff(snapshot(),plain)==0);
}
// The faceplate composed at the drawing's values, closed and open, as the live faceplate draws it.
void small_hours_compose(const juce::File& images){
 std::cout<<"GROUP small_hours_compose"<<std::endl;SmallRig r;if(!r.a)return;auto& f=*r.a;
 for(auto* v:{&f.face,&f.revealed()}){for(auto& s:v->sliders)r.automate(s->element.paramIndex,s->element.drawnValue);for(auto& l:v->levers)r.automate(l->element.paramIndex,l->element.drawnValue);}
 pump();const auto values=liveValues(r);
 f.face.hideReadouts();f.revealed().hideReadouts();
 const auto composed=f.compose(values);const auto live=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(composed.getWidth()==2140&&composed.getHeight()==591);
 {const auto d=maxDiff(composed,live);CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live face by "<<d<<std::endl;}
 f.setOpen(true);f.revealed().hideReadouts();const auto opened=f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);
 CHECK(maxDiff(f.compose(values),composed)==0&&maxDiff(opened,composed)>40);
 {const auto d=maxDiff(f.compose(values,true),opened);CHECK(d<=1);if(d>1)std::cout<<"  the open compose differs from the live face by "<<d<<std::endl;}
 // Other values move the needle and push OUT, the lamp dark.
 {auto other=values;const auto d=r.era(0,"era_reverb.decay_seconds"),by=r.era(0,"bypassed");other[d]=r.p.control(d).convertTo0to1(1.0f);other[by]=1.0f;
  const auto o=f.compose(other);const auto& lamp=f.face.lamp("lamp")->element;
  CHECK(maxDiffIn(o,composed,f.face.needle("needle")->getBounds())>40&&maxDiffIn(o,composed,juce::Rectangle<float>(2*lamp.lensR,2*lamp.lensR).withCentre(lamp.led).getSmallestIntegerContainer())>40);}
 writeImages(images,{{"small-hours-drawn.png",&composed},{"small-hours-open-drawn.png",&opened}});
}
// From the keyboard: each face control takes focus under its printed name, the needle and the lamp never; AFTERGLOW's
// Home and End put the needle on the dial's end steps and Page Down turns back a tenth of its travel, one gesture each;
// Space on ADJUST opens the lid with the focus kept, and the trims then step by the keys.
void small_hours_keyboard(){
 std::cout<<"GROUP small_hours_keyboard"<<std::endl;SmallRig r;if(!r.a)return;auto& f=*r.a;const auto& m=f.skin.skin;
 for(const auto& id:smallFace){auto* c=f.face.control(id.fromFirstOccurrenceOf(".",false,false));const auto* e=element(m,id);CHECK(c!=nullptr&&e!=nullptr);if(!c||!e)continue;
  CHECK(c->getWantsKeyboardFocus()&&c->getTitle()==e->label&&c->getComponentID()==id);}
 auto* flywheel=f.face.slider<Knob>("afterglow");auto* needle=f.face.needle("needle");auto* lamp=f.face.lamp("lamp");CHECK(flywheel&&needle&&lamp);if(!flywheel||!needle||!lamp)return;
 CHECK(!needle->getWantsKeyboardFocus()&&!lamp->getWantsKeyboardFocus());
 const auto d=flywheel->element.paramIndex;const auto& ticks=needle->element.ticks;
 const auto key=[&](int code,double to){const auto g0=r.gestures(d);flywheel->keyPressed(juce::KeyPress(code));pump();const auto g1=r.gestures(d);
  return std::abs(r.physical(d)-to)<to*.01&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1;};
 CHECK(key(juce::KeyPress::homeKey,.2)&&std::abs(needle->x()-ticks.front().second)<.5f);
 CHECK(key(juce::KeyPress::endKey,70)&&std::abs(needle->x()-ticks.back().second)<.5f);
 CHECK(key(juce::KeyPress::pageDownKey,.2*std::pow(350.0,.9)));
 auto* lid=f.face.button("adjust");auto* pre=f.revealed().slider<Knob>("pre-delay");CHECK(lid&&pre);if(!lid||!pre)return;
 CHECK(!pre->isShowing());
 lid->grabKeyboardFocus();lid->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
 CHECK(f.isOpen()&&lid->hasKeyboardFocus(false)&&pre->isShowing());
 {const auto i=pre->element.paramIndex;const auto g0=r.gestures(i);const auto p0=pre->proportion();pre->keyPressed(juce::KeyPress(juce::KeyPress::upKey));const auto g1=r.gestures(i);
  CHECK(std::abs(pre->proportion()-p0-.01)<.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);}
}
// The needle reads AFTERGLOW's decay along the dial's printed scale, logarithmic between its steps and pinned at its
// ends, whether automation, the flywheel or an applied AI edit moves it; it takes no press and no focus.
void small_hours_needle(){
 std::cout<<"GROUP small_hours_needle"<<std::endl;SmallRig r;if(!r.a)return;auto& f=*r.a;
 auto* needle=f.face.needle("needle");CHECK(needle!=nullptr);if(!needle)return;
 const auto& e=needle->element;const auto decay=r.era(0,"era_reverb.decay_seconds");
 CHECK(e.paramIndex==decay&&e.logarithmic&&e.range==juce::Range<double>(.2,70)&&e.ticks.size()==9);if(e.ticks.size()!=9)return;
 const auto at=[&](double v){r.automate(decay,v);pump();return needle->x();};
 for(const auto& [value,x]:e.ticks)CHECK(std::abs(at(value)-x)<.5f);
 // 3 s lies between the 2 s and 4 s steps at the share of their log distance.
 {const auto lo=e.ticks[3].second,hi=e.ticks[4].second;CHECK(std::abs(at(3.0)-(lo+(hi-lo)*(float)(std::log(1.5)/std::log(2.0))))<.5f);}
 CHECK(house::pointerAngleFor(e,-.2)==e.ticks.front().second&&house::pointerAngleFor(e,1.2)==e.ticks.back().second);
 {auto d=r.document();d.values[decay]=r.p.control(decay).convertTo0to1(16.f);CHECK(r.p.applySoundEdit(d,SoundEditOrigin::aiCandidate,r.revision()));pump();
  CHECK(std::abs(needle->x()-e.ticks[6].second)<.5f);}
 auto* flywheel=f.face.slider<Knob>("afterglow");CHECK(flywheel!=nullptr);if(!flywheel)return;
 CHECK(flywheel->element.paramIndex==decay);
 {const auto before=at(1.0);flywheel->keyPressed(juce::KeyPress(juce::KeyPress::upKey));pump();CHECK(needle->x()>before+1);}
 // Drawn where it stands: moving it changes nothing but the needle and the flywheel.
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});};
 f.face.hideReadouts();at(.5);const auto lo=snapshot();at(32);const auto hi=snapshot();
 const auto blanked=[&](juce::Image i){i=i.createCopy();juce::Graphics g(i);g.setColour(juce::Colours::black);g.fillRect(needle->getBounds());g.fillRect(flywheel->getBounds());return i;};
 CHECK(maxDiffIn(lo,hi,needle->getBounds())>40&&maxDiff(blanked(lo),blanked(hi))==0);
 {ParameterValues values{};for(std::size_t i=0;i<values.size();++i)values[i]=r.p.control(i).getValue();
  const auto d=maxDiffIn(f.compose(values),f.createComponentSnapshot(f.getLocalBounds(),true,1.0f),needle->getBounds());CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live needle by "<<d<<std::endl;}
 bool self=true,children=true;needle->getInterceptsMouseClicks(self,children);
 CHECK(!self&&!children&&!needle->getWantsKeyboardFocus()&&!tabOrder(f).contains("face.needle"));
}
// SMALL HOURS's lid turns up about its top hinge over 220 ms: its pixels foreshortened toward the hinge by the cosine
// of the angle, from the lid's height to the open strip's, over the recess at full alpha; the faceplate keeps its 3U and
// nothing outside the recess moves. Closing runs it backward.
void small_hours_hinge(const juce::File& images){
 std::cout<<"GROUP small_hours_hinge"<<std::endl;SmallRig r;if(!r.a)return;auto& f=*r.a;Clock clock;
 auto* lid=f.face.button("adjust");CHECK(lid!=nullptr);if(!lid)return;
 const auto& m=f.skin.skin.reveal.motion;const auto area=m.lid;
 CHECK(m.ms==220&&m.hinge=="top"&&std::abs(m.openHeight-12.01f)<.01f&&m.slide.isOrigin()&&m.turns.empty()&&m.door.isEmpty()&&rectNear(area,lid->element.rect));
 const auto turned=std::acos(m.openHeight/area.getHeight());
 const auto hinged=[&](float e){return area.withHeight(area.getHeight()*std::cos(e*turned));};
 const auto drawn=[&](float e){return f.fastenerTurn()==0&&f.revealed().isVisible()&&f.revealed().getAlpha()==1&&f.getHeight()==591&&rectNear(f.movingArea(),hinged(e));};
 const auto& plate=f.face.ctx.model.asset("adjust:plate");
 const auto recess=juce::Rectangle<float>(plate.anchor.x,plate.anchor.y,(float)plate.image.getWidth(),(float)plate.image.getHeight()).getSmallestIntegerContainer().expanded(2);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 f.face.hideReadouts();const auto closed=snapshot();
 std::vector<juce::Image> frames;juce::Image half;
 press(*lid);
 for(double t:{0.0,44.0,88.0,110.0,154.0,198.0}){clock.at(t);const auto e=studio::motion::ease((float)(t/220));CHECK(f.isMoving()&&f.isOpen()&&drawn(e));frames.push_back(snapshot());if(t==110.0)half=frames.back();}
 clock.at(220);frames.push_back(snapshot());const auto opened=frames.back();
 CHECK(!f.isMoving()&&f.isOpen()&&f.movingArea().isEmpty()&&f.revealed().getAlpha()==1&&f.getHeight()==591);
 // Half-way: the recess below the turning lid as it is open, the lid's foreshortened pixels above, the face round the
 // recess as it was.
 {const auto h=hinged(.5f).getHeight();
  const auto below=area.withTrimmedTop(h).reduced(6.0f).getSmallestIntegerContainer(),turning=area.withHeight(h).reduced(4.0f).getSmallestIntegerContainer();
  CHECK(maxDiffIn(half,opened,below)<=1&&maxDiffIn(half,opened,turning)>40&&maxDiffOutside(half,closed,recess)<=1);}
 {const auto s=sheet(frames,area.getSmallestIntegerContainer().expanded(30));writeImages(images,{{"small-hours-lid-frames.png",&s}});}
 auto* close=f.revealed().button("close");CHECK(close!=nullptr);if(!close)return;
 clock.at(300);press(*close);
 for(double t:{365.0,430.0,495.0}){clock.at(t);const auto e=1-studio::motion::ease((float)((t-300)/220));CHECK(f.isMoving()&&!f.isOpen()&&drawn(e));}
 clock.at(520);CHECK(!f.isMoving()&&!f.revealed().isVisible()&&f.movingArea().isEmpty()&&maxDiff(snapshot(),closed)<=1);
}
void hidden_trim_ends_gesture(){
 std::cout<<"GROUP hidden_trim_ends_gesture_and_rejects_late_drag"<<std::endl;FaceRig r;if(!r.a)return;auto& face=*r.a;face.setOpen(true,false);
 auto* trim=face.revealed().slider<Knob>("ratio");CHECK(trim);if(!trim)return;const auto index=trim->element.paramIndex;const auto before=r.gestures(index);
 trim->onDragStart();trim->setValue(trim->proportionOfLengthToValue(.8),juce::sendNotificationSync);const auto held=r.physical(index);
 CHECK(r.gestures(index).begins==before.begins+1&&r.gestures(index).ends==before.ends);
 face.setOpen(false,false);CHECK(r.gestures(index).ends==before.ends+1);
 trim->setValue(trim->proportionOfLengthToValue(.2),juce::sendNotificationSync);CHECK(r.physical(index)==held);trim->onDragEnd();
 face.setOpen(true,false);trim->onDragStart();trim->setValue(trim->proportionOfLengthToValue(.5),juce::sendNotificationSync);trim->onDragEnd();
 CHECK(r.physical(index)!=held&&r.gestures(index).begins==before.begins+2&&r.gestures(index).ends==before.ends+2);
 CHECK(r.p.undoSoundEdit());CHECK(std::abs(r.physical(index)-held)<1.e-5);
}
//------------------------------------------------------------------------------ Night Dial
juce::Image softwareSnapshot(juce::Component& c){return c.createComponentSnapshot(c.getLocalBounds(),true,1.0f,juce::SoftwareImageType{});}
const juce::StringArray nightFace{"face.dial","face.sweep","face.spread","face.wander","face.glow","face.ring","face.swell","face.tail"};
const juce::StringArray nightDeck{"face.osc1.voices","face.osc1.table","face.osc2.table","face.osc2.position","face.osc2.level","face.osc2.tune","face.osc2.voices",
 "face.osc2.spread","face.width","face.filter.type","face.filter.mode","face.filter.size","face.curve","face.fenv.amount","face.fenv.decay","face.lfo.rate","face.noise"};
void night_dial_face_for(){
 std::cout<<"GROUP night_dial_face_for"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& night=examples[5];
 const auto skin=studio::makerFaceFor(night);CHECK(skin!=nullptr);
 if(skin)CHECK(skin->design==night.id&&skin->maker=="ELMSTEAD SIGNAL WORKS"&&skin->model=="NIGHT DIAL HYBRID VOICE NDV-2"&&skin->units==3&&skin->skin.elements.size()==29);
 {auto d=night;d.units[0].defaults[0].normalized=.2f;d.intent="Another intent";d.revision=6;CHECK(studio::makerFaceFor(d)==nullptr);}
 const auto differs=[&](auto edit){auto d=night;edit(d);return studio::makerFaceFor(d)==nullptr;};
 CHECK(differs([](RackDesign& d){d.id=juce::Uuid();}));
 CHECK(differs([](RackDesign& d){d.name="Night Dial II";}));
 CHECK(differs([](RackDesign& d){d.role=RackRole::fx;}));
 CHECK(differs([](RackDesign& d){d.units[0].module=NativeModule::analog;}));
 CHECK(differs([](RackDesign& d){d.units[0].id=1;}));
 CHECK(differs([](RackDesign& d){d.panel.sections[1]="Heat";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[0].label="Tune";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[3].section=2;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[4].key="wavetable.filter.resonance";}));
 CHECK(differs([](RackDesign& d){d.panel.controls[1].widget=FocusWidget::verticalSlider;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[4].minimum=20;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[2].maximum=100;}));
 CHECK(differs([](RackDesign& d){d.panel.controls[6].response=FocusResponse::linear;}));
 CHECK(differs([](RackDesign& d){d.panel.controls.pop_back();}));
 CHECK(airack::test::frozenMakerSkinFile("night-dial","elements.json").getSize()>0&&airack::test::frozenMakerSkinFile("night-dial","plates/night-dial-deck.png").getSize()>0
       &&airack::test::frozenMakerSkinFile("night-dial","parts/night-dial-face-needle.png").getSize()>0);
 CHECK(airack::test::frozenMakerSkinFile("ember-reed","plates/night-dial-deck.png").isEmpty()&&airack::test::frozenMakerSkinFile("night-dial","plates/ember-reed-deck.png").isEmpty());
}
void night_dial_skin_loads(){
 std::cout<<"GROUP night_dial_skin_loads"<<std::endl;const auto examples=airack::test::frozenExampleRackDesigns();const auto& night=examples[5];
 const auto skin=studio::makerFaceFor(night);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 CHECK(m.elements.size()==29&&m.designId==night.id&&m.maker=="ELMSTEAD SIGNAL WORKS"&&m.modelName=="NIGHT DIAL HYBRID VOICE NDV-2"&&m.units==3);
 CHECK(m.panelWidth==2140.0f&&std::abs(m.unitHeight-197.11f)<.01f);
 std::map<juce::String,int> views,kinds;for(const auto& e:m.elements){++views[e.view];++kinds[e.kind];}
 CHECK(views.size()==2&&views["face"]==12&&views["deck"]==17);
 CHECK(kinds["rotary"]==19&&kinds["selector"]==4&&kinds["lever"]==2&&kinds["lamp"]==1&&kinds["needle"]==1&&kinds["tape"]==1&&kinds["action"]==1);
 CHECK(m.reveal.kind=="drawer"&&m.reveal.view=="deck"&&m.reveal.front.isEmpty()&&m.reveal.units==2&&m.reveal.opener=="face.deck"&&m.reveal.closers.isEmpty());
 CHECK(m.reveal.titles==juce::StringArray({"Open deck","Close deck"}));
 const auto* face=group(m,"face.face");const auto* deck=group(m,"face.deck-controls");CHECK(face&&deck&&m.groups.size()==2);
 {auto want=nightFace;want.add("face.deck");if(face)CHECK(face->focusOrder==want&&face->view=="face");}
 if(deck)CHECK(deck->focusOrder==nightDeck&&deck->view=="deck");
 int bound=0;
 for(const auto& e:m.elements){
  CHECK(e.id.startsWith("face."));
  if(e.view=="deck")CHECK(e.hasDot&&e.label.isNotEmpty());
  if(e.rackUnit<0)continue;
  ++bound;CHECK(e.rackUnit==0&&e.paramId.isEmpty());
  CHECK(rackParameterActive(NativeModule::wavetable,e.key)&&rackParameterIndex(NativeModule::wavetable,0,e.key).has_value());
  if(e.kind=="rotary")CHECK(e.hasDot&&e.ticks.size()>=2&&e.drawnValue>=e.range.getStart()&&e.drawnValue<=e.range.getEnd()&&m.assets.count(e.layer("pointer"))==1);
  if(e.kind=="selector"||e.kind=="lever")CHECK(e.hasDot&&!e.positions.empty()&&std::any_of(e.positions.begin(),e.positions.end(),[&](const auto& p){return p.value==e.drawnValue;}));
 }
 CHECK(bound==27);
 for(const auto& id:nightFace)if(const auto* e=element(m,id))CHECK(e->view=="face"&&e->kind=="rotary"&&e->hasDot);else CHECK(!"missing face knob");
 if(const auto* e=element(m,"face.glow"))CHECK(e->logarithmic&&e->range==juce::Range<double>(100,12000)&&e->key=="wavetable.filter.cutoff");
 if(const auto* e=element(m,"face.spread"))CHECK(!e->logarithmic&&e->range==juce::Range<double>(0,50)&&e->key=="wavetable.osc.1.unison_cents");
 if(const auto* e=element(m,"face.sweep"))CHECK(e->key=="modulation.1.amount"&&!e->tapeRect.isEmpty());
 if(const auto* e=element(m,"face.backlight"))CHECK(e->key=="enabled"&&e->litValue==1&&e->layer("lit")=="face:backlight.lit");
 if(const auto* e=element(m,"face.needle"))CHECK(e->key=="wavetable.osc.1.position"&&e->ticks.size()==11&&m.assets.count(e->layer("needle"))==1);
 if(const auto* e=element(m,"face.deck"))CHECK(e->kind=="action"&&e->view=="face"&&!e->rect.isEmpty()&&!e->litRect.isEmpty()&&e->rackUnit<0);
 if(const auto* e=element(m,"face.osc2.spread"))CHECK(e->label=="OSC 2 SPREAD"&&e->key=="wavetable.osc.2.unison_cents");
 for(const auto& e:m.elements)CHECK(e.tapeRect.isEmpty()==(e.id!="face.sweep"));
 CHECK(m.asset("face:plate").image.getWidth()==2140&&m.asset("face:plate").image.getHeight()==591);
 CHECK(m.asset("deck:plate").image.getWidth()==2140&&m.asset("deck:plate").image.getHeight()==394);
}
void night_dial_angles(){
 std::cout<<"GROUP night_dial_angles"<<std::endl;const auto skin=studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[5]);CHECK(skin!=nullptr);if(!skin)return;const auto& m=skin->skin;
 for(const auto& id:nightFace){
  const auto* e=element(m,id);CHECK(e!=nullptr);if(!e)continue;
  CHECK(std::abs(angleAt(*e,e->ticks.front().first)-e->ticks.front().second)<.01f&&std::abs(angleAt(*e,e->ticks.back().first)-e->ticks.back().second)<.01f);
  for(std::size_t i=1;i<e->ticks.size();++i)CHECK(e->ticks[i].first>e->ticks[i-1].first&&e->ticks[i].second>e->ticks[i-1].second);
  CHECK(house::pointerAngleFor(*e,-.2)==e->ticks.front().second&&house::pointerAngleFor(*e,1.2)==e->ticks.back().second);
 }
 // The logarithmic ones through every printed tick, and between them.
 for(const char* id:{"face.glow","face.swell","face.tail"})if(const auto* e=element(m,id)){
  CHECK(e->logarithmic&&e->ticks.size()>=5);
  for(const auto& [value,angle]:e->ticks)CHECK(std::abs(angleAt(*e,value)-angle)<.01f);
 }
 if(const auto* glow=element(m,"face.glow")){const auto a=angleAt(*glow,1200);CHECK(a>tickAngle(*glow,1000)&&a<tickAngle(*glow,2000));}
 if(const auto* swell=element(m,"face.swell")){const auto a=angleAt(*swell,1.5);CHECK(a>tickAngle(*swell,.5)&&a<tickAngle(*swell,2));}
 // The needle's travel is DIAL's: both proportions of the same parameter, the needle rising across the glass.
 if(const auto* needle=element(m,"face.needle"))if(const auto* dial=element(m,"face.dial")){
  CHECK(needle->range==dial->range&&!needle->logarithmic&&!dial->logarithmic);
  float last=-1;for(int k=0;k<=20;++k){const auto x=house::pointerAngleFor(*needle,k/20.0);CHECK(x>last);last=x;}
 }
}
// Two Night Dials: the first's wavetable unit in synth slot 1, the second's in slot 2.
struct NightRig:Rig{
 NightRig(){first=place(5,{RackRole::synth,4,0},{{0,0}},true);second=place(5,{RackRole::synth,4,1},{{0,1}},true);show(1000,2000);}
 std::size_t wavetable(const char* key){return index(NativeModule::wavetable,0,key);}
};
void night_dial_binds_its_banks(){
 std::cout<<"GROUP night_dial_binds_its_banks"<<std::endl;NightRig r;if(!r.a||!r.b)return;
 for(const auto& [face,slot]:{std::pair{r.a.get(),std::uint8_t(0)},std::pair{r.b.get(),std::uint8_t(1)}}){
  CHECK(face->getComponentID()=="rack.face"&&face->getWidth()==2140&&face->getHeight()==591&&face->units()==3&&face->front()==nullptr);
  CHECK(face->getTitle()=="ELMSTEAD SIGNAL WORKS, NIGHT DIAL HYBRID VOICE NDV-2");
  const auto bound=[&](juce::Component* c){if(auto* s=dynamic_cast<studio::house::BoundSlider*>(c))return s->element.paramIndex;if(auto* l=dynamic_cast<studio::house::Lever*>(c))return l->element.paramIndex;CHECK(!"missing control");return ~std::size_t();};
  const auto synth="synth."+juce::String(slot+1)+".";
  const auto is=[&](juce::Component* c,const char* key){const auto i=bound(c);return i<parameterCount&&parameterRegistry()[i].id==synth+key;};
  CHECK(is(face->face.control("dial"),"wavetable.osc.1.position")&&is(face->face.control("sweep"),"modulation.1.amount")&&is(face->face.control("spread"),"wavetable.osc.1.unison_cents"));
  CHECK(is(face->face.control("wander"),"wavetable.drift")&&is(face->face.control("glow"),"wavetable.filter.cutoff")&&is(face->face.control("ring"),"wavetable.filter.resonance"));
  CHECK(is(face->face.control("swell"),"wavetable.amp.attack")&&is(face->face.control("tail"),"wavetable.amp.release"));
  {auto* lamp=face->face.lamp("backlight");CHECK(lamp!=nullptr&&lamp->element.paramIndex<parameterCount&&parameterRegistry()[lamp->element.paramIndex].id==synth+"enabled");}
  {auto* needle=face->face.needle("needle");CHECK(needle!=nullptr&&needle->element.paramIndex==r.index(NativeModule::wavetable,slot,"wavetable.osc.1.position"));}
  auto& deck=face->revealed();
  CHECK(is(deck.control("osc1.voices"),"wavetable.osc.1.unison")&&is(deck.control("osc2.spread"),"wavetable.osc.2.unison_cents")&&is(deck.control("filter.type"),"wavetable.filter.type"));
  CHECK(is(deck.control("osc1.table"),"wavetable.osc.1.table")&&is(deck.control("lfo.rate"),"lfo.1.rate_hz")&&is(deck.control("noise"),"wavetable.noise"));
 }
}
void night_dial_gestures(){
 std::cout<<"GROUP night_dial_gestures"<<std::endl;NightRig r;if(!r.a)return;
 auto* dial=r.a->face.slider<Knob>("dial");auto* glow=r.a->face.slider<Knob>("glow");CHECK(dial&&glow);if(!dial||!glow)return;
 const auto p1=r.wavetable("wavetable.osc.1.position"),p2=r.index(NativeModule::wavetable,1,"wavetable.osc.1.position"),c=r.wavetable("wavetable.filter.cutoff");
 // DIAL's travel is linear over 300 px: 60 px up from 0.45 is 0.65.
 r.automate(p1,.45);const auto other=r.gestures(p2);const auto v2=r.physical(p2);auto g0=r.gestures(p1);
 drag(*dial,dial->element.pivot,dial->element.pivot-Pt(0.0f,60.0f));auto g1=r.gestures(p1);
 CHECK(std::abs(r.physical(p1)-.65)<.005&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&r.gestures(p2)==other&&r.physical(p2)==v2);
 // GLOW's is logarithmic from 100 Hz to 12 kHz: 60 px up is a fifth of it.
 r.automate(c,1000.0);g0=r.gestures(c);const auto p0=proportion(glow->element,1000.0);
 drag(*glow,glow->element.pivot,glow->element.pivot-Pt(0.0f,60.0f));g1=r.gestures(c);
 CHECK(std::abs(proportion(glow->element,r.physical(c))-(p0+.2))<.005&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto before=r.physical(c);glow->mouseWheelMove(event(*glow,local(*glow,glow->element.pivot),{},0,false),wheel(0.1f));g1=r.gestures(c);
 CHECK(r.physical(c)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;const auto q0=glow->proportion();glow->keyPressed(juce::KeyPress(juce::KeyPress::upKey));g1=r.gestures(c);
 CHECK(std::abs(glow->proportion()-q0-0.01)<0.002&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 g0=g1;glow->mouseDoubleClick(event(*glow,local(*glow,glow->element.pivot),{},2,false));g1=r.gestures(c);
 CHECK(std::abs(r.physical(c)-glow->element.defaultValue)<1.0&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
 // While another gesture holds the processor, a drag changes nothing and says why.
 const auto held=r.wavetable("wavetable.filter.resonance");CHECK(r.p.beginParameterGesture(held));
 const auto v=r.physical(c);r.reported.clear();
 drag(*glow,glow->element.pivot,glow->element.pivot+Pt(0.0f,60.0f));
 CHECK(r.physical(c)==v&&r.reported.contains(studio::rejectedEdit));
 CHECK(r.p.endParameterGesture(held));
}
// The dial's backlight is the unit's lamp: lit while it is enabled, each rack's its own, and never pressed or focused.
void night_dial_lamp(){
 std::cout<<"GROUP night_dial_lamp"<<std::endl;NightRig r;if(!r.a||!r.b)return;
 auto* lamp=r.a->face.lamp("backlight");auto* other=r.b->face.lamp("backlight");CHECK(lamp&&other);if(!lamp||!other)return;
 const auto en=r.wavetable("enabled");
 {bool self=true,children=true;lamp->getInterceptsMouseClicks(self,children);CHECK(!self&&!children&&!lamp->getWantsKeyboardFocus());}
 const auto glass=lamp->getBounds();
 CHECK(r.physical(en)>0.5&&lamp->lit()&&other->lit());const auto lit=softwareSnapshot(*r.a);
 r.automate(en,0.0);CHECK(!lamp->lit()&&other->lit());const auto dark=softwareSnapshot(*r.a);
 CHECK(maxDiffIn(lit,dark,glass)>40&&maxDiffOutside(lit,dark,glass)==0);
 r.automate(en,1.0);CHECK(lamp->lit()&&maxDiff(softwareSnapshot(*r.a),lit)==0);
}
// Each step one complete gesture, or none where it stays.
bool stepped(Rig& r,studio::house::Lever& l,const std::function<void()>& act,double value,int gestures){
 const auto i=l.element.paramIndex;const auto g0=r.gestures(i);act();const auto g1=r.gestures(i);
 const auto ok=std::abs(r.physical(i)-value)<1e-3&&g1.begins==g0.begins+gestures&&g1.ends==g0.ends+gestures;
 if(!ok)std::cout<<"  "<<l.element.id<<" at "<<r.physical(i)<<" ("<<l.legend()<<"), not "<<value<<std::endl;return ok;}
// The deck's rotary selectors: a click on each printed legend chooses its detent; a click off them steps on and wraps;
// Up and Down step and stop at the ends; Space and Enter step on and wrap.
void night_dial_selectors_step(){
 std::cout<<"GROUP night_dial_selectors_step"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;f.setOpen(true);auto& deck=f.revealed();
 const std::map<juce::String,juce::StringArray> printed{{"osc1.voices",{"1","2","3","4","5","6","7"}},{"osc2.voices",{"1","2","3","4","5","6","7"}},
  {"filter.type",{"CLEAN","LADDER","CLASSIC","VOWEL"}},{"filter.mode",{"LP","BP","HP"}}};
 for(const auto& [id,legends]:printed){
  auto* s=deck.lever(id);CHECK(s!=nullptr);if(!s)continue;auto& l=*s;const auto& e=l.element;
  CHECK(e.kind=="selector"&&(int)e.positions.size()==legends.size()&&l.getTitle()==e.label);if((int)e.positions.size()!=legends.size())continue;
  r.automate(e.paramIndex,e.positions.back().value);
  for(std::size_t i=0;i<e.positions.size();++i){
   const auto& p=e.positions[i];CHECK(p.legend==legends[(int)i]&&p.value==(double)i);
   CHECK(stepped(r,l,[&]{clickAt(l,p.legendRect.getCentre());},p.value,1)&&l.legend()==p.legend);
   for(std::size_t j=0;j<i;++j)CHECK(!p.legendRect.intersects(e.positions[j].legendRect));
  }
  const auto n=(double)e.positions.size();
  CHECK(stepped(r,l,[&]{clickAt(l,e.pivot);},0,1)&&stepped(r,l,[&]{clickAt(l,e.pivot);},1,1));
  CHECK(stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::downKey));},0,1)&&stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::downKey));},0,0));
  for(double v=1;v<n;++v)CHECK(stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::upKey));},v,1));
  CHECK(stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::upKey));},n-1,0));
  CHECK(stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));},0,1)&&stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));},1,1));
 }
}
// The deck's TABLE levers: SAW above, SQR below; a click on the bat flips it, as on Ember Reed's levers; Up pushes it to
// SAW and Down to SQR, Space and Enter flip it.
void night_dial_levers(){
 std::cout<<"GROUP night_dial_levers"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;f.setOpen(true);
 for(const char* id:{"osc1.table","osc2.table"}){
  auto* lever=f.revealed().lever(id);CHECK(lever!=nullptr);if(!lever)continue;auto& l=*lever;const auto& e=l.element;
  CHECK(e.kind=="lever"&&e.positions.size()==2&&!e.legendClick);if(e.positions.size()!=2)continue;
  CHECK(e.positions[0].legend=="SAW"&&e.positions[1].legend=="SQR"&&e.positions[0].legendRect.getBottom()<e.positions[1].legendRect.getY());
  // The focus ring and the AI's mark frame the rect 3 px out with a 2 px stroke; the legend rects pad their glyphs by 2 px.
  for(const auto& p:e.positions)CHECK(!e.rect.expanded(4.0f).intersects(p.legendRect.reduced(2.0f)));
  CHECK(r.physical(e.paramIndex)<0.5&&l.legend()=="SAW");
  CHECK(stepped(r,l,[&]{clickAt(l,e.rect.getCentre());},1,1)&&l.legend()=="SQR"&&stepped(r,l,[&]{clickAt(l,e.rect.getCentre());},0,1)&&l.legend()=="SAW");
  CHECK(stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::upKey));},0,0)&&stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::downKey));},1,1)
        &&stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::downKey));},1,0)&&stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::upKey));},0,1));
  CHECK(stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));},1,1)&&stepped(r,l,[&]{l.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));},0,1));
 }
 // The other rack's are its own.
 CHECK(r.physical(r.index(NativeModule::wavetable,1,"wavetable.osc.1.table"))<0.5);
}
// The AI's marks: GLOW's dashed pointer on the face; closed, the deck's changes wait behind the pull's outline and its
// count; open, FILTER TYPE's dashed pointer and OSC 1 TABLE's dashed outline; another panel outlines the faceplate.
void night_dial_marks(const juce::File& images){
 std::cout<<"GROUP night_dial_marks"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 auto* glow=f.face.slider<Knob>("glow");auto* type=f.revealed().lever("filter.type");auto* table=f.revealed().lever("osc1.table");
 auto* spread=f.revealed().slider<Knob>("osc2.spread");auto* pull=f.face.button("deck");
 CHECK(glow&&type&&table&&spread&&pull);if(!glow||!type||!table||!spread||!pull)return;
 const auto cut=glow->element.paramIndex,ty=type->element.paramIndex,ta=table->element.paramIndex,sp=spread->element.paramIndex;
 r.automate(cut,1000.0);
 const auto live=r.document();auto candidate=live;
 const auto set=[&](SoundDocument& d,std::size_t i,double v){d.values[i]=r.p.control(i).convertTo0to1((float)v);};
 set(candidate,cut,3000.0);set(candidate,ty,3.0);set(candidate,ta,1.0);set(candidate,sp,40.0);
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);f.refresh(live);
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto plain=snapshot();f.setOpen(true);const auto plainOpen=snapshot();f.setOpen(false);
 f.showProposal(std::make_shared<const SoundDocument>(candidate));
 {juce::StringArray ids;for(const auto& k:f.marks())ids.add(k.element);
  CHECK(ids==juce::StringArray({"face.glow","face.osc1.table","face.osc2.spread","face.filter.type"})&&!f.proposesPanel());
  if(ids.size()!=4)std::cout<<"  marks: "<<ids.joinIntoString(" ")<<std::endl;}
 CHECK(glow->getDescription()=="AI proposes 3.0 kHz"&&type->getDescription()=="AI proposes VOWEL"&&table->getDescription()=="AI proposes SQR");
 CHECK(pull->getDescription()=="AI proposes changes to 3 deck controls");
 const auto gain=[](const juce::Image& a,const juce::Image& b,const juce::Path& p){return whiteAlong(a,p)-whiteAlong(b,p);};
 const auto pointer=[&](const studio::house::Element& e,float degrees){const auto a=juce::degreesToRadians(degrees);const auto c=e.pivot+f.revealed().getPosition().toFloat()*(e.view=="deck"?1.0f:0.0f);
  return line(c+Pt(std::sin(a),-std::cos(a))*e.faceR*0.35f,c+Pt(std::sin(a),-std::cos(a))*e.faceR*0.95f);};
 const auto glowLine=pointer(glow->element,house::pointerAngleFor(glow->element,house::proportionOf(glow->element,3000.0)));
 const auto pullEdge=[&]{return outline(pull->area().expanded(3.0f));};
 const auto marked=snapshot();const auto closedPull=pullEdge();
 CHECK(gain(marked,plain,glowLine)>0.35&&gain(marked,plain,closedPull)>0.35);
 std::cout<<"  closed: GLOW "<<gain(marked,plain,glowLine)<<", pull "<<gain(marked,plain,closedPull)<<std::endl;
 f.setOpen(true);const auto markedOpen=snapshot();
 const auto typeLine=pointer(type->element,type->element.positions[3].angle);
 const auto tableEdge=outline(table->element.rect.expanded(3.0f)+f.revealed().getPosition().toFloat());
 CHECK(gain(markedOpen,plainOpen,typeLine)>0.35&&gain(markedOpen,plainOpen,tableEdge)>0.35&&gain(markedOpen,plainOpen,pullEdge())<0.05&&pull->getDescription().isEmpty());
 std::cout<<"  open: TYPE "<<gain(markedOpen,plainOpen,typeLine)<<", TABLE "<<gain(markedOpen,plainOpen,tableEdge)<<", pull "<<gain(markedOpen,plainOpen,pullEdge())<<std::endl;
 writeImages(images,{{"night-dial-marks.png",&marked},{"night-dial-marks-open.png",&markedOpen}});
 f.setOpen(false);CHECK(pull->getDescription()=="AI proposes changes to 3 deck controls");
 // GLOW locked live: no mark, its dot instead.
 {auto locked=live;locked.controlLocks[cut]=true;f.refresh(locked);
  CHECK(f.marks().size()==3&&glow->getDescription().isEmpty());
  const auto closedLocked=snapshot();
  CHECK(gain(closedLocked,plain,glowLine)<0.05&&maxDiffIn(closedLocked,marked,juce::Rectangle<int>(24,24).withCentre(glow->element.dot.roundToInt()))>40);
  f.refresh(live);}
 // A focused control relabelled: the whole faceplate's outline, closed and open, and no marks.
 {auto d=candidate;auto racks=*d.personalities;for(auto& i:racks.instances)if(i.id==r.first)i.design.panel.controls[0].label="Tune";
  d.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));f.showProposal(std::make_shared<const SoundDocument>(d));}
 CHECK(f.proposesPanel()&&f.marks().empty()&&pull->getDescription().isEmpty());
 CHECK(gain(snapshot(),plain,outline(f.getLocalBounds().toFloat().reduced(4.0f)))>0.35);
 f.setOpen(true);CHECK(gain(snapshot(),plainOpen,outline(f.getLocalBounds().toFloat().reduced(4.0f)))>0.35);
 f.showProposal(nullptr);CHECK(!f.proposesPanel()&&f.marks().empty()&&maxDiff(snapshot(),plainOpen)==0);
}
void night_dial_compose(const juce::File& images){
 std::cout<<"GROUP night_dial_compose"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 for(auto* v:{&f.face,&f.revealed()}){for(auto& s:v->sliders)r.automate(s->element.paramIndex,s->element.drawnValue);for(auto& l:v->levers)r.automate(l->element.paramIndex,l->element.drawnValue);}
 pump();const auto values=liveValues(r);
 f.face.hideReadouts();f.revealed().hideReadouts();
 const auto snapshot=[&]{return f.createComponentSnapshot(f.getLocalBounds(),true,1.0f);};
 const auto closed=f.compose(values);const auto live=snapshot();
 CHECK(closed.getWidth()==2140&&closed.getHeight()==591);
 {const auto d=maxDiff(closed,live);CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live faceplate by "<<d<<std::endl;}
 f.setOpen(true);f.revealed().hideReadouts();
 const auto open=f.compose(values,true);const auto liveOpen=snapshot();
 CHECK(open.getWidth()==2140&&open.getHeight()==985&&maxDiff(f.compose(values),closed)==0);
 // A live snapshot that paints the deck first draws one edge pixel of WANDER's dot 3 levels off; with the deck hidden
 // the face matches the compose exactly, so the face is held to the deck-hidden snapshot and the deck to the open one.
 const juce::Rectangle<int> faceRows{0,0,2140,591},deckRows{0,591,2140,394};
 f.revealed().setVisible(false);const auto liveFace=snapshot();f.revealed().setVisible(true);
 {const auto d=std::max(maxDiffIn(open,liveFace,faceRows),maxDiffIn(open,liveOpen,deckRows));
  CHECK(d<=1);if(d>1)std::cout<<"  the open compose differs from the live faceplate by "<<d<<std::endl;}
 // Other values turn the knobs, move the needle, darken the glass and step the selectors.
 {auto other=values;other[r.wavetable("wavetable.filter.cutoff")]=0.9f;CHECK(maxDiff(f.compose(other),closed)>40);}
 {auto other=values;other[r.wavetable("wavetable.osc.1.position")]=0.9f;CHECK(maxDiff(f.compose(other),closed)>40);}
 {auto other=values;other[r.wavetable("enabled")]=0.0f;CHECK(maxDiff(f.compose(other),closed)>40);}
 {auto other=values;other[r.wavetable("wavetable.filter.type")]=0.0f;CHECK(maxDiff(f.compose(other,true),open)>40&&maxDiff(f.compose(other),closed)==0);}
 auto sqrValues=values;sqrValues[r.wavetable("wavetable.osc.1.table")]=1.0f;sqrValues[r.wavetable("wavetable.osc.2.table")]=1.0f;
 const auto sqr=f.compose(sqrValues,true);CHECK(maxDiff(sqr,open)>40&&maxDiff(f.compose(sqrValues),closed)==0);
 writeImages(images,{{"night-dial-drawn.png",&closed},{"night-dial-open-drawn.png",&open},{"night-dial-open-sqr-drawn.png",&sqr}});
}
// The spec's Tab order and titles: each control titled with its printed label, the deck's with their group; the pull
// "Open deck" or "Close deck", opened by Space or Enter; the needle and the backlight never focused.
void night_dial_keyboard(){
 std::cout<<"GROUP night_dial_keyboard"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 auto* pull=f.face.button("deck");CHECK(pull!=nullptr);if(!pull)return;
 const juce::StringArray titles{"DIAL","SWEEP","SPREAD","WANDER","GLOW","RING","SWELL","TAIL"};
 for(int i=0;i<nightFace.size();++i){auto* c=f.face.control(nightFace[i].fromFirstOccurrenceOf(".",false,false));CHECK(c!=nullptr&&c->getTitle()==titles[i]&&c->getWantsKeyboardFocus());}
 auto closed=nightFace;closed.add("face.deck");CHECK(tabOrder(f)==closed);
 CHECK(pull->getTitle()=="Open deck");
 pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(f.isOpen()&&pull->getTitle()=="Close deck");
 auto opened=nightFace;opened.addArray(nightDeck);opened.add("face.deck");CHECK(tabOrder(f)==opened);
 const juce::StringArray deckTitles{"OSC 1 VOICES","OSC 1 TABLE","OSC 2 TABLE","OSC 2 POSITION","OSC 2 LEVEL","OSC 2 TUNE","OSC 2 VOICES","OSC 2 SPREAD","STEREO WIDTH",
  "FILTER TYPE","FILTER MODE","FILTER SIZE","CONTOUR CURVE","FILTER ENV AMOUNT","FILTER ENV DECAY","MOTION LFO RATE","NOISE LEVEL"};
 for(int i=0;i<nightDeck.size();++i){auto* c=f.revealed().control(nightDeck[i].fromFirstOccurrenceOf(".",false,false));CHECK(c!=nullptr&&c->getTitle()==deckTitles[i]);}
 pull->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(!f.isOpen()&&pull->getTitle()=="Open deck");
 pull->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(f.isOpen());f.setOpen(false);
 auto* needle=f.face.needle("needle");auto* lamp=f.face.lamp("backlight");CHECK(needle&&lamp);
 if(needle&&lamp)CHECK(!needle->getWantsKeyboardFocus()&&!lamp->getWantsKeyboardFocus()&&!tabOrder(f).contains("face.needle")&&!tabOrder(f).contains("face.backlight"));
}
// SWEEP's legend stays true: while route 1 is not an enabled route from LFO 1 to the wavetable bank's Osc 1 position,
// the face tapes over it with the route's name, the printed bank left out, and the knob is titled with it.
void night_dial_sweep_tape(const juce::File& images){
 std::cout<<"GROUP night_dial_sweep_tape"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);
 auto* knob=f.face.slider<Knob>("sweep");CHECK(knob!=nullptr&&!knob->element.tapeRect.isEmpty());if(!knob)return;
 const auto route=[&](const char* field){return r.wavetable((juce::String("modulation.1.")+field).toRawUTF8());};
 const auto choice=[&](const char* field){return juce::roundToInt(r.physical(route(field)));};
 CHECK(choice("enabled")==1&&choice("source")==3&&choice("target")==11&&choice("bank")==2);
 f.refresh(r.document());const auto plain=softwareSnapshot(f);
 CHECK(f.routeTape().isEmpty()&&knob->getTitle()=="SWEEP");if(!f.routeTape().isEmpty())std::cout<<"  tape at the recipe '"<<f.routeTape()<<"'"<<std::endl;
 const auto tapeRect=knob->element.tapeRect.getSmallestIntegerContainer().expanded(6);
 const auto shown=[&](const char* text,const char* title){f.refresh(r.document());const auto s=softwareSnapshot(f);
  const auto ok=f.routeTape()==text&&knob->getTitle()==title&&maxDiffIn(s,plain,tapeRect)>40&&maxDiffOutside(s,plain,tapeRect)==0;
  if(!ok)std::cout<<"  tape '"<<f.routeTape()<<"', title '"<<knob->getTitle()<<"', inside "<<maxDiffIn(s,plain,tapeRect)<<", outside "<<maxDiffOutside(s,plain,tapeRect)<<std::endl;
  return ok;};
 r.automate(route("source"),4.0);CHECK(shown("LFO 2 TO OSC 1 POSITION","LFO 2 to Osc 1 position"));const auto lfo=softwareSnapshot(f);
 r.automate(route("source"),3.0);r.automate(route("bank"),1.0);CHECK(shown("LFO 1 TO ANALOG OSC 1 POSITION","LFO 1 to Analog Osc 1 position"));
 r.automate(route("bank"),2.0);r.automate(route("enabled"),0.0);CHECK(shown("ROUTE 1 OFF","Route 1 off"));
 writeImages(images,{{"night-dial-tape.png",&lfo}});
 // The knob still writes route 1's amount.
 {const auto amount=route("amount");const auto g0=r.gestures(amount);const auto before=r.physical(amount);knob->keyPressed(juce::KeyPress(juce::KeyPress::upKey));const auto g1=r.gestures(amount);
  CHECK(knob->element.paramIndex==amount&&r.physical(amount)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);}
 r.automate(route("enabled"),1.0);f.refresh(r.document());CHECK(f.routeTape().isEmpty()&&knob->getTitle()=="SWEEP");
 CHECK(maxDiffIn(softwareSnapshot(f),plain,tapeRect)==0);
}
// SPREAD's legend is untrue while osc 1 has one voice: the face shows a tape lettered 1 VOICE under it then, which
// takes no press, and the knob still sets the spread.
void night_dial_spread_tape(){
 std::cout<<"GROUP night_dial_spread_tape"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 const auto* skin=studio::studioSkin();CHECK(skin!=nullptr);if(!skin)return;
 f.setHostLook(skin->asset("programmer:lock"),1.0f,skin->house);
 const auto* tape=element(f.skin.skin,"face.spread-tape");CHECK(tape!=nullptr&&tape->text=="1 VOICE");if(!tape)return;
 CHECK(f.face.tape("spread-tape")==nullptr);
 const auto voices=r.wavetable("wavetable.osc.1.unison"),cents=r.wavetable("wavetable.osc.1.unison_cents");
 CHECK(juce::roundToInt(r.physical(voices))==4);
 f.refresh(r.document());const auto five=softwareSnapshot(f);
 const auto area=juce::Rectangle<float>(tape->size.x,tape->size.y).withCentre(tape->centre).getSmallestIntegerContainer().expanded(8);
 r.automate(voices,0.0);f.refresh(r.document());const auto one=softwareSnapshot(f);
 {const auto in=maxDiffIn(one,five,area),out=maxDiffOutside(one,five,area);CHECK(in>40&&out==0);if(in<=40||out!=0)std::cout<<"  one voice: inside "<<in<<", outside "<<out<<std::endl;}
 r.automate(voices,4.0);f.refresh(r.document());CHECK(maxDiff(softwareSnapshot(f),five)==0);
 r.automate(voices,0.0);f.refresh(r.document());
 auto* spread=f.face.slider<Knob>("spread");CHECK(spread!=nullptr&&spread->element.paramIndex==cents);if(!spread)return;
 const auto before=r.physical(cents);const auto g0=r.gestures(cents);
 drag(*spread,spread->element.pivot,spread->element.pivot+Pt(0,-60));
 const auto g1=r.gestures(cents);CHECK(r.physical(cents)>before&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
}
// The needle reads DIAL's parameter along the glass's printed scale, linear between its ticks and pinned at its ends,
// whether automation or an applied AI edit moves it; it takes no press and no focus.
void night_dial_needle(){
 std::cout<<"GROUP night_dial_needle"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 auto* needle=f.face.needle("needle");CHECK(needle!=nullptr);if(!needle)return;
 const auto& e=needle->element;const auto position=r.wavetable("wavetable.osc.1.position");
 CHECK(e.paramIndex==position&&e.ticks.size()==11);if(e.ticks.size()!=11)return;
 const auto at=[&](double v){r.automate(position,v);pump();return needle->x();};
 CHECK(std::abs(at(0.0)-244.63f)<.5f&&std::abs(at(.45)-843.43f)<.5f&&std::abs(at(1.0)-1576.42f)<.5f);
 CHECK(studio::house::pointerAngleFor(e,-.2)==e.ticks.front().second&&studio::house::pointerAngleFor(e,1.2)==e.ticks.back().second);
 {auto d=r.document();d.values[position]=r.p.control(position).convertTo0to1(.8f);CHECK(r.p.applySoundEdit(d,SoundEditOrigin::aiCandidate,r.revision()));pump();
  CHECK(std::abs(needle->x()-1308.93f)<.5f);}
 // Drawn where it stands: moving it changes nothing but the needle and DIAL.
 at(.1);const auto lo=softwareSnapshot(f);at(.9);const auto hi=softwareSnapshot(f);
 auto* dial=f.face.slider<Knob>("dial");CHECK(dial!=nullptr);if(!dial)return;
 const auto blanked=[&](juce::Image i){i=i.createCopy();juce::Graphics g(i);g.setColour(juce::Colours::black);g.fillRect(needle->getBounds());g.fillRect(dial->getBounds());return i;};
 CHECK(maxDiffIn(lo,hi,needle->getBounds())>40&&maxDiff(blanked(lo),blanked(hi))==0);
 {ParameterValues values{};for(std::size_t i=0;i<values.size();++i)values[i]=r.p.control(i).getValue();
  const auto d=maxDiffIn(f.compose(values),f.createComponentSnapshot(f.getLocalBounds(),true,1.0f),needle->getBounds());CHECK(d<=1);if(d>1)std::cout<<"  compose differs from the live needle by "<<d<<std::endl;}
 bool self=true,children=true;needle->getInterceptsMouseClicks(self,children);
 CHECK(!self&&!children&&!needle->getWantsKeyboardFocus()&&!tabOrder(f).contains("face.needle"));
}
// The deck draws out from under the face on the pull, which writes nothing: 3U to 5U, the faceplate growing by the
// deck's 2U, and back. Its controls are in Tab order only while open, after the face's and before the pull; closing
// with focus in the deck hands it to the pull. Open, the pull stands raised and lit, and is pressed there.
void night_dial_drawer(){
 std::cout<<"GROUP night_dial_drawer"<<std::endl;NightRig r;if(!r.a)return;auto& f=*r.a;
 auto* pull=f.face.button("deck");CHECK(pull!=nullptr);if(!pull)return;
 const auto local=[&](Pt facePoint){return (facePoint-pull->getPosition().toFloat()).roundToInt();};
 const auto hits=[&](Pt facePoint){const auto p=local(facePoint);return pull->hitTest(p.x,p.y);};
 const auto rest=pull->element.rect.getCentre(),raised=pull->element.litRect.getCentre();
 CHECK(!pull->element.litRect.isEmpty()&&f.front()==nullptr);
 CHECK(pull->getTitle()=="Open deck"&&!f.isOpen()&&!f.revealed().isVisible()&&f.units()==3&&f.getHeight()==591&&!pull->lit()&&hits(rest)&&!hits(raised));
 const juce::StringArray onFace{"face.dial","face.sweep","face.spread","face.wander","face.glow","face.ring","face.swell","face.tail"};
 auto closed=onFace;closed.add("face.deck");
 auto opened=onFace;if(const auto* g=group(f.skin.skin,"face.deck-controls"))opened.addArray(g->focusOrder);opened.add("face.deck");CHECK(opened.size()==26);
 CHECK(tabOrder(f)==closed);if(tabOrder(f)!=closed)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 const auto rev=r.revision();const auto values=r.document().values;int moved=0;f.onOpen=[&]{++moved;};
 clickAt(*pull,rest);
 CHECK(f.isOpen()&&f.revealed().isVisible()&&f.units()==5&&f.getHeight()==591+394&&moved==1);
 CHECK(f.face.getPosition()==juce::Point<int>()&&f.revealed().getY()==591&&f.openedArea()==juce::Rectangle<int>(0,591,2140,394));
 CHECK(pull->getTitle()=="Close deck"&&pull->lit()&&hits(raised)&&!hits(rest.withY(pull->element.rect.getBottom()-4)));
 CHECK(tabOrder(f)==opened);if(tabOrder(f)!=opened)std::cout<<"  Tab order: "<<tabOrder(f).joinIntoString(" ")<<std::endl;
 clickAt(*pull,raised);
 CHECK(!f.isOpen()&&!f.revealed().isVisible()&&f.units()==3&&f.getHeight()==591&&pull->getTitle()=="Open deck"&&!pull->lit()&&moved==2&&tabOrder(f)==closed);
 // Closing with focus in the deck hands it to the pull; with focus on the face it stays.
 pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));CHECK(f.isOpen());
 auto* tune=f.revealed().slider<Knob>("osc2.tune");CHECK(tune!=nullptr);if(!tune)return;
 tune->grabKeyboardFocus();CHECK(tune->hasKeyboardFocus(false));
 pull->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));CHECK(!f.isOpen()&&juce::Component::getCurrentlyFocusedComponent()==pull);
 auto* dial=f.face.slider<Knob>("dial");dial->grabKeyboardFocus();f.setOpen(true);f.setOpen(false);CHECK(dial->hasKeyboardFocus(false));
 CHECK(r.revision()==rev&&r.document().values==values);
 // Moving, the deck slides from under the face, its foot the faceplate's.
 Clock clock;
 const auto drawn=[&](float e){return f.revealed().getBottom()==juce::roundToInt(591+394*e)&&f.getHeight()==f.revealed().getBottom()&&std::abs(f.shownUnits()-(3+2*e))<1e-5f
                                    &&f.openedArea()==juce::Rectangle<int>(0,591,2140,juce::roundToInt(394*e));};
 press(*pull);CHECK(f.isOpen()&&f.isMoving()&&drawn(0)&&f.getIndexOfChildComponent(&f.revealed())<f.getIndexOfChildComponent(&f.face));
 for(double t:{50.0,100.0,150.0}){clock.at(t);CHECK(f.isMoving()&&drawn(studio::motion::ease((float)(t/200))));}
 clock.at(200);CHECK(!f.isMoving()&&f.getHeight()==985&&f.revealed().getY()==591);
}
void merge_ui_offscreen(){
 std::cout<<"GROUP merge_ui_offscreen"<<std::endl;
 CHECK(juce::ComponentPeer::getNumPeers()==0);
 const auto library=airack::test::frozenLibraryRackDesigns();CHECK(library.size()==28);
 for(const auto* name:{"Ember Reed","Glass Current","Thread Press","Oxide Return","Lumen Field","Small Hours","Night Dial","Tile Array","Character Drive","Band Color"}){
  const auto design=std::find_if(library.begin(),library.end(),[&](const auto& d){return d.name==name;});
  CHECK(design!=library.end());if(design==library.end())continue;
  std::unique_ptr<studio::MakerFace> retired;int closedSources=0;
  {
  Rig r;RackOperation op;op.design=*design;op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.destination={design->role,masterChain,0};
  std::uint8_t synth=0,effect=0;for(const auto& unit:design->units)op.allocation.push_back({unit.id,isSourceModule(unit.module)?synth++:effect++});
  CHECK(r.p.applyRackOperation(op,r.revision())==RackCandidateStatus::ready);r.first=op.newInstance;r.a=r.make(r.first);CHECK(r.a);if(!r.a)continue;
  auto& face=*r.a;const auto d=r.document();face.refresh(d);
  if(design->units[0].module==NativeModule::saturation){
   CHECK(!face.getDescription().contains("DSP PENDING")&&design->revision==2);
   const auto* model=studio::studioSkin();CHECK(model);if(model){studio::AllControlsPanel panel(r.p,*model,{});panel.show({{NativeModule::saturation,0}});panel.refresh(d);
    CHECK(panel.getDescription().isEmpty());const auto by=*rackParameterIndex(NativeModule::saturation,0,"bypassed");const auto* knob=panel.knob(by);CHECK(knob&&knob->getTitle()==parameterRegistry()[by].name);
    studio::SaturationGraph* graph=nullptr;for(auto* child:panel.getChildren())if(auto* g=dynamic_cast<studio::SaturationGraph*>(child))graph=g;CHECK(graph&&graph->getDescription().startsWith("SATURATION | "));}
  }
  CHECK(face.skin.design==design->id&&face.getWidth()==2140);
  const auto closed=face.compose(d.values);CHECK(closed.isValid()&&closed.getWidth()==2140&&closed.getHeight()==face.getHeight());
  face.setOpen(true,false);const auto opened=face.compose(d.values,true);CHECK(opened.isValid()&&opened.getWidth()==2140);
  std::vector<std::pair<house::Needle*,float>> needles;
  for(auto* view:{&face.face,&face.revealed()})for(const auto& needle:view->needles)needles.emplace_back(needle.get(),needle->x());
  face.onDrumSourceClose=[&]{++closedSources;};const bool tile=face.tileArray()!=nullptr;
  face.retire();CHECK(face.tileArray()==nullptr&&!face.onDrumSource&&!face.onDrumSourceClose&&closedSources==1);
  for(const auto& [needle,x]:needles){auto& parameter=r.p.control(needle->element.paramIndex);parameter.setValueNotifyingHost(parameter.getValue()<.5f?1.f:0.f);pump();CHECK(needle->x()==x&&!needle->isEnabled());}
  if(tile){CHECK(!face.face.onGestureEnd&&!face.revealed().onGestureEnd);}
  retired=std::move(r.a);
  }
  retired.reset();CHECK(closedSources==1&&juce::ComponentPeer::getNumPeers()==0);
 }
 native_layer_image_repeat({});band_color_face_for();
 CHECK(juce::ComponentPeer::getNumPeers()==0);
}
int main(int argc,char** argv){
 if(argc==3&&juce::String(argv[1])=="--native-layer-repeat"){
#if JUCE_WINDOWS
  const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))return 2;
#endif
  const juce::File images(argv[2]);CHECK(images.createDirectory().wasOk());
  CHECK(juce::MessageManager::getInstanceWithoutCreating()==nullptr&&juce::ComponentPeer::getNumPeers()==0);
  {
   const auto preparedSkins=prepareTestMakerSkins();
   native_layer_image_repeat(images);
   CHECK(juce::MessageManager::getInstanceWithoutCreating()==nullptr&&juce::ComponentPeer::getNumPeers()==0);
  }
  juce::DeletedAtShutdown::deleteAll();
#if JUCE_WINDOWS
  CoUninitialize();
#endif
  std::cout<<"MakerFace checks="<<checks<<" groups=1 failures="<<failures<<std::endl;return failures?1:0;
 }
 juce::ScopedJuceInitialiser_GUI gui;const auto preparedSkins=prepareTestMakerSkins();juce::File images;
 if(argc==2&&juce::String(argv[1])=="--merge-ui-offscreen"){studio::motion::setForTests({},true);merge_ui_offscreen();std::cout<<"Offscreen merge checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
 if(argc==2&&juce::String(argv[1])=="--hidden-gesture-only"){studio::motion::setForTests({},true);hidden_trim_ends_gesture();std::cout<<"Hidden trim checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
 if(argc==2&&juce::String(argv[1])=="--artwork-only"){studio::motion::setForTests({},true);saturation_task2_contract(images);face_compose(images);std::cout<<"Maker artwork checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
 const bool bandOnly=argc==3&&juce::String(argv[1])=="--band-color";
 if(argc==2||bandOnly){images=juce::File(argv[bandOnly?2:1]);CHECK(images.createDirectory().wasOk());}
 studio::motion::setForTests({},true);
 if(bandOnly){band_color_face_for();band_color_candidate(images);std::cout<<"MakerFace checks="<<checks<<" groups=2 failures="<<failures<<std::endl;return failures?1:0;}
 saturation_task2_contract(images);maker_face_for();maker_skin_loads();maker_angles();house_skin_unchanged();
 ember_face_for();ember_skin_loads();ember_angles();ember_grip_frames();ember_deck_scales();ember_selectors();
 glass_face_for();glass_skin_loads();glass_angles();glass_items();glass_lines_fit();
 oxide_face_for();oxide_skin_loads();oxide_angles();oxide_grip_frames();motion_skins();
 face_binds_its_banks();face_gestures();face_pins();face_levers();face_setup();face_compose(images);face_marks(images);
 ember_binds_its_banks();ember_gestures();ember_pins();ember_lamp();ember_selectors_step();ember_drawer();ember_compose(images);
 ember_vel_air(images);ember_marks(images);
 glass_binds_its_banks();glass_gestures();glass_pins();glass_vfd();glass_vfd_words();glass_lamp();glass_compose(images);
 glass_door();glass_pages();glass_soft_knobs();glass_detents();glass_beats();glass_compose_open(images);glass_marks(images);glass_strip(images);
 oxide_binds_its_banks();oxide_gestures();oxide_pins();oxide_levers();oxide_levers_no_hover();oxide_note();oxide_tempo();oxide_compose(images);oxide_marks(images);
 motion_ease();motion_overrides();motion_drawer();motion_reverse();motion_reduced();motion_clock_bounds();motion_interrupt();
 motion_lid(images);motion_tambour(images);motion_door(images);motion_marks();
 native_layer_image_repeat(images);
 band_color_face_for();band_color_candidate(images);
 lumen_face_for();lumen_skin_loads();lumen_angles();lumen_binds_its_banks();lumen_gestures();lumen_reveal();lumen_marks();lumen_compose(images);
 small_hours_face_for();small_hours_skin_loads();small_hours_angles();small_hours_binds_its_banks();small_hours_gestures();small_hours_lamp();small_hours_selector_and_levers();
 small_hours_lid();small_hours_marks(images);small_hours_compose(images);small_hours_keyboard();small_hours_needle();small_hours_hinge(images);
 night_dial_face_for();night_dial_skin_loads();night_dial_angles();night_dial_binds_its_banks();night_dial_gestures();night_dial_lamp();
 night_dial_selectors_step();night_dial_levers();night_dial_marks(images);night_dial_compose(images);night_dial_keyboard();
 night_dial_sweep_tape(images);night_dial_spread_tape();night_dial_needle();night_dial_drawer();
 hidden_trim_ends_gesture();
 std::cout<<"MakerFace checks="<<checks<<" groups=112 failures="<<failures<<std::endl;return failures?1:0;}
