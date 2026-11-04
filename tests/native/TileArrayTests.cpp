#include "FrozenRackExamples.h"
#include "PreparedMakerSkins.h"
#include "MakerFace.h"
#include "RackExamples.h"
#include "ParameterRegistry.h"
#include "TileArrayFace.h"
#include "Motion.h"
#include "InstrumentProcessor.h"
#include "UserReach.h"
#include "StudioSkin.h"
#include <bit>
#include <iostream>
#include <set>
#include <typeinfo>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef NDEBUG
namespace airack {
struct RoutedDrumTestAccess {static auto snapshot(const RoutedSynthEngine& e){return e.drumTestSnapshot();}};
struct InstrumentDrumTestAccess {
 static auto image(const InstrumentProcessor& p){return RoutedDrumTestAccess::snapshot(p.engine);}
 static void liveControlLock(InstrumentProcessor& p,std::size_t index,bool value){const std::lock_guard lock(p.restoreMutex);p.metadata.controls[index].store(value);}
 static auto history(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return std::pair(p.editHistory->count,p.editHistory->cursor);}
};
}
#endif
using namespace airack;
namespace {
int checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;}} while(false)
// A native, registry-normalized fixture independent of the new product helper.
RackDesign fixture() {
 RackDesign d;d.id=juce::Uuid("f97641c2-cd64-46a9-a3ab-0cc027a5f8d4");d.name="Tile Array";
 d.intent="Sixteen independent one-shot pads";d.role=RackRole::synth;d.units={{0,NativeModule::drum,{}}};
 auto& u=d.units[0];const auto value=[&](juce::String key,float physical){const auto i=rackParameterIndex(u.module,0,key);CHECK(i.has_value());if(i){const auto& p=parameterRegistry()[*i];u.defaults.push_back({0,key,juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(physical)});}};
 value("enabled",1);value("level",.5f);value("pan",0);
 for(int p=1;p<=16;++p){const auto k="drum.pad."+juce::String(p)+".";value(k+"gain_db",0);value(k+"pan",0);value(k+"tune_semitones",0);value(k+"trim_start",0);value(k+"trim_end",1);value(k+"attack_seconds",0);value(k+"decay_seconds",0);value(k+"midi_note",float(35+p));value(k+"choke_group",0);}
 d.panel.layout=PanelLayout::performanceControls;d.panel.sections={"KIT"};
 d.panel.controls={{"KIT LEVEL",0,0,"level",FocusWidget::verticalSlider,FocusResponse::linear,0,1},{"KIT PAN",0,0,"pan",FocusWidget::rotary,FocusResponse::linear,-1,1}};
 return d;
}
void existingApiRed(){
 const auto d=fixture();CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
 CHECK(studio::makerFaceFor(airack::test::frozenExampleRackDesigns()[2])!=nullptr);
 CHECK(airack::test::frozenMakerSkinFile("thread-press","elements.json").getSize()>0);
 const auto frozen=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("docs/design/racks/2026-10-09-studio-rack/tile-array/skin/elements.json");
 CHECK(frozen.existsAsFile());if(frozen.existsAsFile()){const auto j=juce::JSON::parse(frozen.loadFileAsString());CHECK(j["design_id"].toString()==d.id.toDashedString());}
 CHECK(airack::test::frozenMakerSkinFile("tile-array","elements.json").getSize()>0);
 CHECK(studio::makerFaceFor(d)==nullptr); // The independent custom fixture is not the frozen packaged definition.
}
void frozenHouseContract(){
 std::cout<<"GROUP frozen_house_states_and_integer_contract"<<std::endl;
 const auto root=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("docs/design/racks/2026-10-09-studio-rack/tile-array/skin");
 const auto bytes=[&](const juce::String& path){juce::MemoryBlock out;root.getChildFile(path).loadFileAsData(out);return out;};
 const auto model=studio::house::loadModel(bytes,nullptr);int bound=0,integers=0;
 for(const auto& e:model.elements){if(e.rackUnit>=0)++bound;if(e.key.endsWith(".midi_note")||e.key.endsWith(".choke_group")){++integers;CHECK(e.detents);CHECK(e.range.getStart()==0);CHECK(e.range.getEnd()==(e.key.endsWith(".midi_note")?127:16));CHECK(e.defaultValue==std::floor(e.defaultValue));}for(const auto& [state,asset]:e.stateAssets){CHECK(state.isNotEmpty());CHECK(model.assets.contains(asset));}}
 CHECK(bound==147);CHECK(integers==32);
 auto doc=juce::JSON::parse(bytes("elements.json").toString());auto* elements=doc["elements"].getArray();CHECK(elements!=nullptr);
 if(elements){for(auto& value:*elements)if(value["kind"].toString()=="fader"){auto* states=new juce::DynamicObject;states->setProperty("idle","missing:causal-state");value.getDynamicObject()->setProperty("state_assets",juce::var(states));break;}
  const auto invalid=juce::JSON::toString(doc);bool refused=false;try{(void)studio::house::loadModel([&](const juce::String& path){if(path=="elements.json")return juce::MemoryBlock(invalid.toRawUTF8(),invalid.getNumBytesAsUTF8());return bytes(path);},nullptr);}catch(const std::runtime_error&){refused=true;}CHECK(refused);}
}
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(30);}
juce::MouseEvent event(juce::Component& c,juce::Point<float> at,juce::ModifierKeys mods={},bool dragged=false,std::optional<juce::Point<float>> down={}){const auto now=juce::Time::getCurrentTime();return {juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,juce::MouseInputSource::defaultTiltY,&c,&c,now,down.value_or(at),now,1,dragged};}
int changedPixels(const juce::Image& a,const juce::Image& b,juce::Rectangle<int> area){int changed=0;for(int y=area.getY();y<area.getBottom();++y)for(int x=area.getX();x<area.getRight();++x)changed+=a.getPixelAt(x,y)!=b.getPixelAt(x,y);return changed;}
juce::Component* find(juce::Component& c,const juce::String& id){if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* found=find(*child,id))return found;return nullptr;}
struct FaceRig {
 std::unique_ptr<InstrumentProcessor> processor=std::make_unique<InstrumentProcessor>();
 juce::Uuid rack;RackInstance instance;std::unique_ptr<studio::MakerFace> face;juce::StringArray reports;juce::Component holder;
 explicit FaceRig(unsigned bank=0,bool desktop=true){
  auto& p=*processor;ProjectDocument project;std::uint64_t revision=0;CHECK(p.tryProjectSnapshot(project,revision));const auto design=airack::test::frozenTileArrayRackDesign();
  RackOperation op;op.design=design;op.newInstance=rack;op.allowSlotReuse=true;op.allocation={{0,std::uint8_t(bank)}};op.destination={RackRole::synth,masterChain,0};
  CHECK(p.applyRackOperation(op,revision)==RackCandidateStatus::ready);instance={rack,design,{{0,std::uint8_t(bank)}}};studio::house::Host host;host.report=[this](const juce::String& message){reports.add(message);};
  const auto skin=studio::makerFaceFor(design);CHECK(skin!=nullptr);if(skin)face=studio::MakerFace::forRack(p,skin,instance,host);CHECK(face!=nullptr);if(face){holder.addAndMakeVisible(*face);holder.setSize(2140,1576);if(desktop){holder.addToDesktop(juce::ComponentPeer::windowIsTemporary);holder.setVisible(true);}pump();}
 }
 ~FaceRig(){if(face)face->suspendInputs();processor->releaseResources();}
 studio::house::BoundSlider* slider(const juce::String& id){return face?dynamic_cast<studio::house::BoundSlider*>(find(*face,id)):nullptr;}
};
void recipe_fixed_bindings_and_held_selection(){
 std::cout<<"GROUP recipe_fixed_bindings_and_held_selection"<<std::endl;
 const auto design=airack::test::frozenTileArrayRackDesign(),expected=fixture();CHECK(design.id==expected.id&&design.name==expected.name&&design.role==expected.role&&design.units==expected.units&&design.panel.controls==expected.panel.controls&&design.panel.sections==expected.panel.sections&&design.panel.layout==expected.panel.layout);CHECK(airack::test::frozenExampleRackDesigns().size()==6&&airack::test::frozenFactoryRackDesigns().size()==21&&airack::test::frozenLibraryRackDesigns().size()==28);
 for(unsigned bank=0;bank<4;++bank){FaceRig rig(bank);if(!rig.face)continue;auto& face=*rig.face;CHECK(face.parameters().size()==147);std::set<std::size_t> unique;std::size_t attachments=face.face.sliders.size()+face.revealed().sliders.size();CHECK(attachments==146); // The enabled attachment is a toggle.
  for(auto* view:{&face.face,&face.revealed()})for(const auto& slider:view->sliders){unique.insert(slider->element.paramIndex);const auto key=slider->element.key;const auto index=rackParameterIndex(NativeModule::drum,std::uint8_t(bank),key);CHECK(index&&*index==slider->element.paramIndex);}
  CHECK(unique.size()==146);auto* tile=face.tileArray();CHECK(tile!=nullptr);if(!tile)continue;CHECK(face.units()==4&&face.shownUnits()==4);face.setOpen(true,false);CHECK(face.units()==8&&face.shownUnits()==8);
  auto* first=rig.slider("pad.1.gain_db");auto* last=rig.slider("pad.16.gain_db");CHECK(first&&last);if(!first||!last)continue;
  CHECK(first->isVisible()&&!last->isVisible());const auto immutable=first->element.paramIndex;const auto other=last->param.getValue();const auto revision=rig.processor->control(immutable).getValue();
  first->onDragStart();first->setValue(-12,juce::sendNotificationSync);CHECK(first->gestureHeld()&&face.revealed().hasHeldGesture());CHECK(!tile->selectPad(15,true));CHECK(tile->selectedPad()==0&&first->isVisible()&&!last->isVisible()&&first->element.paramIndex==immutable);first->onDragEnd();CHECK(!first->gestureHeld()&&tile->selectedPad()==15&&!first->isVisible()&&last->isVisible());CHECK(last->param.getValue()==other&&rig.processor->control(immutable).getValue()!=revision);
  CHECK(rig.processor->undoSoundEdit());pump();CHECK(rig.processor->control(immutable).getValue()==revision);
  CHECK(tile->selectPad(0));first->onDragStart();first->setValue(-6,juce::sendNotificationSync);face.suspendInputs();const auto retired=first->param.getValue();first->setValue(-18,juce::sendNotificationSync);CHECK(!first->gestureHeld()&&first->param.getValue()==retired);face.setOpen(false,false);face.setVisible(false);CHECK(!face.isOpen());face.setVisible(true);
  face.setOpen(true,false);tile->selectPad(15);auto* note=rig.slider("pad.16.midi_note");auto* choke=rig.slider("pad.16.choke_group");CHECK(note&&choke);if(note&&choke){CHECK(note->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));CHECK(note->getValue()==52);CHECK(choke->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));CHECK(choke->getValue()==1);CHECK(note->element.paramIndex==*rackParameterIndex(NativeModule::drum,std::uint8_t(bank),"drum.pad.16.midi_note"));}
  auto* attack=rig.slider("pad.16.attack_seconds");CHECK(attack);if(attack){CHECK(attack->keyPressed(juce::KeyPress(juce::KeyPress::homeKey)));for(int step=0;step<5;++step)CHECK(attack->keyPressed(juce::KeyPress(juce::KeyPress::pageUpKey)));CHECK(std::abs(attack->param.convertFrom0to1(attack->param.getValue())-.5f)<1e-5f);CHECK(std::abs(attack->proportion()-.5)<1e-6);}
  SoundDocument d;std::uint64_t edit=0;CHECK(rig.processor->trySoundSnapshot(d,edit));for(const auto index:face.parameters())CHECK(userReach(d).test(index));d.synthLocks[bank]=true;CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,edit));face.refresh(d);pump();const auto before=last->param.getValue();CHECK(last->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(last->param.getValue()==before&&!rig.reports.isEmpty());DrumPadTarget target;DrumCommitError error;CHECK(rig.processor->captureDrumPerformanceTarget(rig.rack,0,15,target,error));CHECK(!rig.processor->captureDrumPadTarget(rig.rack,0,15,target,error)&&error==DrumCommitError::locked);CHECK(tile->triggerPad(15));
 }
}
void drum_locks_deny_numeric_begin_and_live_set(){
 std::cout<<"GROUP drum_locks_deny_numeric_begin_and_live_set"<<std::endl;
 for(unsigned bank=0;bank<4;++bank){FaceRig rig(bank);auto& p=*rig.processor;if(!rig.face)continue;
  const auto field=*rackParameterIndex(NativeModule::drum,std::uint8_t(bank),"drum.pad.16.gain_db");
  SoundDocument before;std::uint64_t revision=0;CHECK(p.trySoundSnapshot(before,revision));
#ifndef NDEBUG
  const auto history=InstrumentDrumTestAccess::history(p);CHECK(p.beginParameterGesture(field));
  // A lock can arrive after the begin callback; the held setter must re-read it.
  InstrumentDrumTestAccess::liveControlLock(p,field,true);CHECK(!p.setParameterGestureValue(field,.25f));CHECK(p.control(field).getValue()==before.values[field]);CHECK(p.endParameterGesture(field));
  CHECK(InstrumentDrumTestAccess::history(p)==history);InstrumentDrumTestAccess::liveControlLock(p,field,false);
#endif
  auto locked=before;locked.synthLocks[bank]=true;CHECK(p.applySoundEdit(locked,SoundEditOrigin::manual,revision));
  SoundDocument current;CHECK(p.trySoundSnapshot(current,revision));const auto kit=current.personalities;
  for(const auto index:rig.face->parameters())CHECK(!p.beginParameterGesture(index));
  CHECK(p.trySoundSnapshot(current,revision));CHECK(current.values==before.values&&current.personalities==kit);
  locked=current;locked.synthLocks[bank]=false;locked.values[*rackParameterIndex(NativeModule::drum,std::uint8_t(bank),"enabled")]=0;
  for(const auto index:rig.face->parameters())locked.controlLocks[index]=true;
  CHECK(p.applySoundEdit(locked,SoundEditOrigin::manual,revision));for(const auto index:rig.face->parameters())CHECK(!p.beginParameterGesture(index));
  DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPerformanceTarget(rig.rack,0,15,target,error));DrumPadReadback readback;CHECK(p.readDrumPad(target,readback,error));
 }
}
void integer_input_methods_continuous_rotor_and_host_marks(){
 std::cout<<"GROUP integer_input_methods_continuous_rotor_and_host_marks"<<std::endl;FaceRig rig;rig.processor->prepareToPlay(48000,512);if(!rig.face)return;auto& face=*rig.face;auto* tile=face.tileArray();if(!tile)return;face.setOpen(true,false);tile->selectPad(15);
 for(const auto* id:{"pad.16.midi_note","pad.16.choke_group"}){auto* slider=rig.slider(id);CHECK(slider);if(!slider)continue;const auto read=[&]{return slider->param.convertFrom0to1(slider->param.getValue());};
  CHECK(slider->keyPressed(juce::KeyPress(juce::KeyPress::homeKey)));CHECK(read()==0);juce::MouseWheelDetails wheel{};wheel.deltaY=.1f;slider->mouseWheelMove(event(*slider,slider->element.pivot-slider->getPosition().toFloat()),wheel);CHECK(read()>0&&read()==std::floor(read()));
  auto* handler=slider->getAccessibilityHandler();CHECK(handler&&handler->getValueInterface());if(handler&&handler->getValueInterface()){handler->getValueInterface()->setValue(7.75);pump();CHECK(read()==8);}
  const auto start=slider->element.pivot-slider->getPosition().toFloat();const auto finish=start.translated(0,-70);slider->mouseDown(event(*slider,start,juce::ModifierKeys::leftButtonModifier));for(int step=1;step<=10;++step)slider->mouseDrag(event(*slider,start+(finish-start)*(float(step)/10),juce::ModifierKeys::leftButtonModifier,true,start));slider->mouseUp(event(*slider,finish,{},true,start));CHECK(read()>8&&read()==std::floor(read())&&!slider->gestureHeld());
  slider->mouseDoubleClick(event(*slider,start));CHECK(read()==slider->element.defaultValue);CHECK(slider->keyPressed(juce::KeyPress(juce::KeyPress::endKey)));CHECK(read()==slider->element.range.getEnd());CHECK(slider->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));CHECK(read()==slider->element.range.getEnd());
 }
 auto* tune=rig.slider("pad.16.tune_semitones");CHECK(tune);if(tune){const auto before=face.createComponentSnapshot(face.getLocalBounds(),true,1.f);tune->setValue(7.125,juce::sendNotificationSync);const auto after=face.createComponentSnapshot(face.getLocalBounds(),true,1.f);const auto at=tune->element.pivot+face.revealed().getPosition().toFloat();CHECK(changedPixels(before,after,juce::Rectangle<int>(72,72).withCentre(at.roundToInt()))>100);CHECK(std::abs(tune->param.convertFrom0to1(tune->param.getValue())-7.125f)<1e-5f);}
 const auto* skin=studio::studioSkin();CHECK(skin);if(!skin)return;face.setHostLook(skin->asset("programmer:lock"),1,skin->house);SoundDocument live;std::uint64_t revision=0;CHECK(rig.processor->trySoundSnapshot(live,revision));face.refresh(live);auto candidate=live;const auto first=*rackParameterIndex(NativeModule::drum,0,"drum.pad.1.gain_db"),last=*rackParameterIndex(NativeModule::drum,0,"drum.pad.16.gain_db");candidate.values[first]=rig.processor->control(first).convertTo0to1(-9);candidate.values[last]=rig.processor->control(last).convertTo0to1(-7);
 face.showProposal(std::make_shared<const SoundDocument>(candidate));CHECK(face.marks().size()==1&&face.marks()[0].element=="pad.16.gain_db");auto locked=live;locked.controlLocks[last]=true;face.refresh(locked);CHECK(face.marks().empty());tile->selectPad(0);CHECK(face.marks().size()==1&&face.marks()[0].element=="pad.1.gain_db");face.showProposal(nullptr);CHECK(face.marks().empty()&&!face.proposesPanel());
}
void exact_pad_accessibility_and_posted_navigation(){
 std::cout<<"GROUP exact_pad_accessibility_and_posted_navigation"<<std::endl;FaceRig rig;auto& p=*rig.processor;if(!rig.face)return;auto& face=*rig.face;auto* tile=face.tileArray();if(!tile)return;
 for(unsigned pad:{0u,15u}){std::vector<float> pcm(4096,pad==0?.004f:-.002f);drums::AssetPtr expected;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,expected,assetError));std::uint64_t ticket=0;DrumCommitError error;CHECK(p.prepareDrumAsset(pcm,1,ticket,error));DrumPadTarget target;CHECK(p.captureDrumPadTarget(rig.rack,0,std::uint8_t(pad),target,error));drums::DrumPad metadata;metadata.asset=expected->id();metadata.name=pad==0?"positive":"negative";CHECK(p.assignPreparedDrumPad(target,ticket,metadata,{},error));}
 SoundDocument sound;std::uint64_t edit=0;CHECK(p.trySoundSnapshot(sound,edit));const auto registry=parameterRegistry();const auto physical=[&](const juce::String& id,float value){for(std::size_t n=0;n<registry.size();++n)if(registry[n].id==id){sound.values[n]=p.control(n).convertTo0to1(value);return;}CHECK(false);};
 physical("global.master_level",1);physical("synth.1.level",1);physical("output.gain_db",0);physical("output.protection",0);physical("synth.1.drum.pad.1.midi_note",36);physical("synth.1.drum.pad.16.midi_note",36);CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,edit));p.prepareToPlay(48000,512);face.refresh(sound);pump();
 auto* pad16=find(face,"face.pad.16");auto* pad1=find(face,"face.pad.1");CHECK(pad16&&pad1);if(!pad16||!pad1)return;
 juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;
 auto stop=[&]{juce::MidiBuffer events;events.addEvent(juce::MidiMessage::midiStop(),0);p.processBlock(audio,events);p.processBlock(audio,midi);};stop();
 face.setOpen(true,false);auto* held=rig.slider("pad.1.gain_db");CHECK(held);if(held)held->onDragStart();
 auto* handler=pad16->getAccessibilityHandler();CHECK(handler&&handler->getActions().invoke(juce::AccessibilityActionType::press));p.processBlock(audio,midi);CHECK(tile->selectedPad()==0);bool onlyNegative=true;for(int n=0;n<512;++n)onlyNegative&=audio.getSample(0,n)<0&&audio.getSample(1,n)<0;CHECK(onlyNegative);
 if(held)held->onDragEnd();CHECK(tile->selectedPad()==15);
#ifndef NDEBUG
 CHECK(InstrumentDrumTestAccess::image(p).hits[0]==1);
#endif
 stop();pad16->grabKeyboardFocus();pump();CHECK(pad16->hasKeyboardFocus(false));auto* peer=rig.holder.getPeer();CHECK(peer!=nullptr);if(peer){const auto window=static_cast<HWND>(peer->getNativeHandle());CHECK(PostMessageW(window,WM_KEYDOWN,VK_LEFT,1));CHECK(PostMessageW(window,WM_KEYUP,VK_LEFT,1));pump();std::cout<<"POSTED_LEFT selected="<<tile->selectedPad()<<" focus="<<(juce::Component::getCurrentlyFocusedComponent()?juce::Component::getCurrentlyFocusedComponent()->getComponentID():juce::String("none"))<<std::endl;CHECK(tile->selectedPad()==14);p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)==0);}
 const auto now=juce::Time::getCurrentTime();const auto point=pad1->getLocalBounds().getCentre().toFloat();juce::MouseEvent shift(juce::Desktop::getInstance().getMainMouseSource(),point,juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier|juce::ModifierKeys::shiftModifier),juce::MouseInputSource::defaultPressure,juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,juce::MouseInputSource::defaultTiltY,pad1,pad1,now,point,now,1,false);pad1->mouseDown(shift);pad1->mouseUp(shift);CHECK(tile->selectedPad()==0);p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)==0);
 CHECK(pad16->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));CHECK(pad16->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));p.processBlock(audio,midi);
#ifndef NDEBUG
 CHECK(InstrumentDrumTestAccess::image(p).hits[0]==1);
#endif
 stop();pad16->keyStateChanged(false);
 std::cout<<"GROUP pad_focus_transfer_releases_the_old_key_latch"<<std::endl;pad16->grabKeyboardFocus();CHECK(pad16->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));p.processBlock(audio,midi);pad1->grabKeyboardFocus();CHECK(pad1->hasKeyboardFocus(false));pad1->keyStateChanged(false);pad16->grabKeyboardFocus();CHECK(pad16->hasKeyboardFocus(false));stop();CHECK(pad16->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);stop();pad16->keyStateChanged(false);
}
bool png(const juce::File& file,const juce::Image& image){juce::FileOutputStream out(file);return out.openedOk()&&juce::PNGImageFormat().writeImageToStream(image,out);}
void generated_text_scoped_lifecycle(){
 std::cout<<"GROUP generated_text_scoped_lifecycle"<<std::endl;
 const auto& pad=studio::makerFaceFor(airack::test::frozenTileArrayRackDesign())->skin.asset("part:ivory-performance-pad.idle");
 for(int scope=0;scope<32;++scope){studio::house::GeneratedText renderer;CHECK(renderer.available());juce::Image image(juce::Image::ARGB,400,100,true,juce::SoftwareImageType());juce::Graphics g(image);
  for(const auto* family:{"Consolas","Bahnschrift"}){CHECK(renderer.draw(g,"Caf\xc3\xa9 PAD 16",juce::Font(juce::FontOptions(family,29.f,juce::Font::bold)),juce::Colours::white,{2,2,390,90},false));}
  CHECK(renderer.drawSprite(g,pad.image,pad.anchor,{320,70},{.5f,.5f}));
  int visible=0;for(int y=0;y<image.getHeight();++y)for(int x=0;x<image.getWidth();++x)visible+=image.getPixelAt(x,y).getAlpha()!=0;CHECK(visible>100);
 }
}
void real_pad_admission_errors(){
 std::cout<<"GROUP real_pad_admission_errors"<<std::endl;FaceRig rig;auto* tile=rig.face?rig.face->tileArray():nullptr;CHECK(tile);if(!tile)return;auto& p=*rig.processor;p.prepareToPlay(48000,512);
 DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPerformanceTarget(rig.rack,0,0,target,error));int accepted=0;while(accepted<512&&p.enqueueDrumPad(target,100,1))++accepted;CHECK(accepted>0&&accepted<512&&p.drumPerformanceStatus()==DrumPerformanceStatus::queueFull);
 CHECK(!tile->triggerPad(15));CHECK(!rig.reports.isEmpty()&&rig.reports[rig.reports.size()-1].containsIgnoreCase("queue full")&&rig.reports[rig.reports.size()-1].containsIgnoreCase("not queued"));
 SoundDocument sound;std::uint64_t edit=0;CHECK(p.trySoundSnapshot(sound,edit));RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=rig.rack;CHECK(p.applyRackOperation(remove,edit)==RackCandidateStatus::ready);CHECK(!tile->triggerPad(15));CHECK(rig.reports[rig.reports.size()-1].containsIgnoreCase("not queued"));
}
void captureControlledReviewStates(const juce::File& output){
 std::cout<<"GROUP controlled_native_review_states_not_inference"<<std::endl;
 struct CursorRestore {POINT before{};bool saved=GetCursorPos(&before)!=0;~CursorRestore(){if(saved)SetCursorPos(before.x,before.y);}} cursorRestore;
 const auto folder=output.getChildFile("controlled-states");CHECK(!folder.exists());CHECK(folder.createDirectory().wasOk());
 FaceRig rig(0,false);if(!rig.face)return;
 // A physical hover needs an ordinary opaque peer. A temporary transparent
 // popup peer is skipped by Windows hit testing even when snapshots render.
 rig.holder.setOpaque(true);rig.holder.addToDesktop(0);rig.holder.setVisible(true);pump();
 auto& face=*rig.face;auto* tile=face.tileArray();CHECK(tile);if(!tile)return;
 face.face.hideReadouts();face.revealed().hideReadouts();rig.holder.setAlwaysOnTop(true);rig.holder.toFront(true);pump();
 juce::StringArray manifest;manifest.add("Controlled native component-event fixtures; not backend generation or listening acceptance.");
 manifest.add("Hover uses the owned native peer/cursor and checks isMouseOver. Focus sends the public focusGained(Tab) event after real peer focus. Button release is outside the hit area, so it does not invoke Import/open/toggle.");
 manifest.add("Rotors have no alternate hover/pressed/disabled sprite family; those images show their actual existing pointer/focus behavior. No new artwork or product state is invented.");
 const auto shot=[&](const juce::String& name,const juce::String& note,bool settle=true){if(settle)pump();const auto image=face.createComponentSnapshot(face.getLocalBounds(),true,1.f);CHECK(png(folder.getChildFile(name+".png"),image));manifest.add(name+"\t"+note+"\t"+juce::String(image.getWidth())+"x"+juce::String(image.getHeight()));return image;};
 const auto park=[&]{auto* peer=rig.holder.getPeer();CHECK(peer);if(!peer)return;POINT at{2,2};CHECK(ClientToScreen(static_cast<HWND>(peer->getNativeHandle()),&at));CHECK(SetCursorPos(at.x,at.y));CHECK(PostMessageW(static_cast<HWND>(peer->getNativeHandle()),WM_MOUSEMOVE,0,MAKELPARAM(2,2)));pump();};
 const auto hover=[&](juce::Component& c,juce::Point<float> point){
  auto* peer=rig.holder.getPeer();CHECK(peer);if(!peer)return;const auto window=static_cast<HWND>(peer->getNativeHandle());rig.holder.toFront(true);pump();
  const auto at=rig.holder.getLocalPoint(&c,point);const auto scale=peer->getPlatformScaleFactor();const auto dpi=GetDpiForWindow(window);
  POINT client{juce::roundToInt(at.x*scale),juce::roundToInt(at.y*scale)},screen=client;RECT clip{},allowed{},before{};MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
  CHECK(GetClipCursor(&clip));CHECK(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor));CHECK(IntersectRect(&allowed,&clip,&monitor.rcWork));CHECK(ClientToScreen(window,&screen));CHECK(GetWindowRect(window,&before));
  // Keep native component/image geometry exact. Translate only this oversized owned
  // peer so the control is physically reachable within the existing cursor clip.
  const POINT desired{allowed.left+(allowed.right-allowed.left)/2,allowed.top+(allowed.bottom-allowed.top)/2};
  CHECK(SetWindowPos(window,HWND_TOPMOST,before.left+desired.x-screen.x,before.top+desired.y-screen.y,0,0,SWP_NOSIZE|SWP_SHOWWINDOW));pump();screen=client;CHECK(ClientToScreen(window,&screen));
  // Moving the oversized peer can keep the same physical cursor position. Give
  // JUCE an actual new cursor position before the final target, so its cached
  // component under the mouse is recomputed for this moved peer.
  CHECK(SetCursorPos(screen.x+8,screen.y));CHECK(PostMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(client.x+8,client.y)));pump();
  CHECK(SetCursorPos(screen.x,screen.y));CHECK(PostMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(client.x,client.y)));pump();
  POINT actual{};RECT bounds{},clientBounds{};DWORD hitPid=0;CHECK(GetCursorPos(&actual));CHECK(GetWindowRect(window,&bounds));CHECK(GetClientRect(window,&clientBounds));const auto hit=WindowFromPoint(actual);GetWindowThreadProcessId(hit,&hitPid);
  std::cout<<"HOVER_DIAGNOSTIC id="<<c.getComponentID()<<" local="<<at.toString()<<" scale="<<scale<<" client="<<client.x<<","<<client.y<<" screen="<<screen.x<<","<<screen.y<<" actual="<<actual.x<<","<<actual.y<<" window="<<bounds.left<<","<<bounds.top<<","<<bounds.right<<","<<bounds.bottom<<" clientExtent="<<clientBounds.right<<","<<clientBounds.bottom<<" clip="<<clip.left<<","<<clip.top<<","<<clip.right<<","<<clip.bottom<<" dpi="<<GetDpiForWindow(window)<<" visible="<<IsWindowVisible(window)<<" enabled="<<IsWindowEnabled(window)<<" style="<<GetWindowLongPtrW(window,GWL_STYLE)<<" exstyle="<<GetWindowLongPtrW(window,GWL_EXSTYLE)<<" own="<<window<<" hit="<<hit<<" hitPid="<<hitPid<<" selfPid="<<GetCurrentProcessId()<<" foreground="<<GetForegroundWindow()<<" juceMouse="<<juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().toString()<<" cScreen="<<c.getScreenBounds().toString()<<" under="<<(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse()?juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse()->getComponentID():juce::String("none"))<<" at="<<(rig.holder.getComponentAt(at.roundToInt())?rig.holder.getComponentAt(at.roundToInt())->getComponentID():juce::String("none"))<<" over="<<c.isMouseOver(true)<<std::endl;
  CHECK(actual.x==screen.x&&actual.y==screen.y);CHECK(hit==window&&hitPid==GetCurrentProcessId());CHECK(GetDpiForWindow(window)==dpi);CHECK(clientBounds.right==before.right-before.left&&clientBounds.bottom==before.bottom-before.top);CHECK(c.isMouseOver(true));
 };

 struct Control {const char* family;const char* id;};
 for(const auto& spec:std::array<Control,5>{{{"pad","face.pad.16"},{"fader","face.level"},{"rotor","face.pan"},{"toggle","face.enabled"},{"action","face.programmer"}}}){
  face.setOpen(false,false);tile->selectPad(15);auto* c=find(face,spec.id);CHECK(c&&c->isShowing());if(!c)continue;
  auto point=c->getLocalBounds().getCentre().toFloat();auto* slider=dynamic_cast<studio::house::BoundSlider*>(c);auto* fader=dynamic_cast<studio::house::Fader*>(c);
  if(fader)point=juce::Point<float>(fader->element.capCentreX,fader->capY())-fader->getPosition().toFloat();
  else if(slider)point=slider->element.hitCentre()-slider->getPosition().toFloat();
  else if(auto* button=dynamic_cast<studio::house::SkinButton*>(c))point=button->area().getCentre()-button->getPosition().toFloat();
  CHECK(c->hitTest(juce::roundToInt(point.x),juce::roundToInt(point.y)));const auto value=slider?slider->getValue():0.0;
  c->setEnabled(true);juce::Component::unfocusAllComponents();c->mouseExit(event(*c,point));park();
  const auto idle=shot(juce::String(spec.family)+"-idle",juce::String(spec.id)+" actual idle");
  hover(*c,point);const auto over=shot(juce::String(spec.family)+"-hover",juce::String(spec.id)+" actual owned-peer hover");
  if(juce::String(spec.family)!="rotor")CHECK(changedPixels(idle,over,idle.getBounds())>0);
  const auto modifiers=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
  c->mouseDown(event(*c,point,modifiers));if(juce::String(spec.family)=="pad")CHECK(rig.processor->drumPerformanceStatus()==DrumPerformanceStatus::ready);if(slider)c->mouseDrag(event(*c,point.translated(0,-12),modifiers,true,point));
  if(slider)CHECK(slider->gestureHeld());
  (void)shot(juce::String(spec.family)+"-pressed",juce::String(spec.id)+(juce::String(spec.family)=="pad"?" actual mouseDown, exact pad action admitted; empty pad, no sounding claim":" actual mouseDown/slider drag"));
  c->mouseUp(event(*c,{-100,-100},{},true,point));if(juce::String(spec.family)=="pad"){juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;rig.processor->processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)==0);}c->mouseExit(event(*c,point));park();
  if(slider){CHECK(!slider->gestureHeld());slider->setValue(value,juce::sendNotificationSync);}
  c->grabKeyboardFocus();CHECK(c->hasKeyboardFocus(false));c->focusGained(juce::Component::focusChangedByTabKey);
  (void)shot(juce::String(spec.family)+"-focus",juce::String(spec.id)+" real keyboard focus with controlled public Tab-focus event");
  juce::Component::unfocusAllComponents();c->focusLost(juce::Component::focusChangedDirectly);c->setEnabled(false);
  CHECK(!c->isEnabled());(void)shot(juce::String(spec.family)+"-disabled",juce::String(spec.id)+" controlled component-disabled fixture (not service/lock admission)",false);c->setEnabled(true);
 }
 if(auto* enabled=dynamic_cast<studio::house::SkinButton*>(find(face,"face.enabled"))){CHECK(enabled->lit());CHECK(enabled->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));CHECK(!enabled->lit());(void)shot("toggle-off","Real complete parameter gesture, ENABLE off");CHECK(enabled->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));CHECK(enabled->lit());}
 // Use the real C preparation/assignment/readback facade for legal64-byte UTF-8 metadata.
 auto& p=*rig.processor;std::vector<float> pcm(4800,.002f);drums::AssetPtr asset;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,asset,assetError));
 std::uint64_t ticket=0;DrumCommitError error;CHECK(p.prepareDrumAsset(pcm,1,ticket,error));DrumPadTarget target;CHECK(p.captureDrumPadTarget(rig.rack,0,15,target,error));
 std::string name="Caf\xc3\xa9 \xce\xa9 ";name.append(64-name.size(),'X');drums::DrumPad metadata;metadata.asset=asset->id();metadata.name=name;CHECK(metadata.name.size()==64);
 CHECK(p.assignPreparedDrumPad(target,ticket,metadata,{},error));CHECK(p.captureDrumPerformanceTarget(rig.rack,0,15,target,error));DrumPadReadback readback;CHECK(p.readDrumPad(target,readback,error));CHECK(readback.metadata.name==name&&readback.frames==4800&&readback.channels==1);
 SoundDocument live;std::uint64_t revision=0;CHECK(p.trySoundSnapshot(live,revision));face.refresh(live);tile->refresh();tile->selectPad(15);face.setOpen(true,false);park();
 (void)shot("unicode-64-byte-name-open","Real C-assigned copied metadata, exact64 UTF-8 bytes; no PCM escapes into the view");face.setOpen(false,false);(void)shot("unicode-64-byte-name-closed","Same real copied sample name in the closed LCD");
 const auto* skin=studio::studioSkin();CHECK(skin);if(skin){face.setHostLook(skin->asset("programmer:lock"),1,skin->house);
  const auto first=*rackParameterIndex(NativeModule::drum,0,"drum.pad.1.gain_db"),last=*rackParameterIndex(NativeModule::drum,0,"drum.pad.16.gain_db");
  auto locked=live;locked.controlLocks[first]=true;locked.controlLocks[last]=true;CHECK(p.applySoundEdit(locked,SoundEditOrigin::manual,revision));face.refresh(locked);face.setOpen(true,false);(void)shot("host-locks-selected-and-hidden-open","Real control locks with host artwork: selectedpad16 gain and hiddenpad1 gain");face.setOpen(false,false);(void)shot("host-locks-hidden-closed","Real hidden control locks with host-supplied lock artwork on reveal opener");
  CHECK(p.trySoundSnapshot(locked,revision));CHECK(p.applySoundEdit(live,SoundEditOrigin::manual,revision));face.refresh(live);
  auto proposed=live;proposed.values[first]=p.control(first).convertTo0to1(-9);proposed.values[last]=p.control(last).convertTo0to1(-7);face.showProposal(std::make_shared<const SoundDocument>(proposed));
  face.setOpen(true,false);tile->selectPad(15);CHECK(face.marks().size()==1&&face.marks()[0].element=="pad.16.gain_db");(void)shot("synthetic-proposal-pad16-open","Synthetic candidate document fixture, not successful AI inference; pad16 visible mark plus hiddenpad1 reveal marker");
  face.setOpen(false,false);(void)shot("synthetic-proposal-hidden-closed","Synthetic candidate document fixture: closed reveal opener marks hiddenpad changes");face.setOpen(true,false);tile->selectPad(0);CHECK(face.marks().size()==1&&face.marks()[0].element=="pad.1.gain_db");(void)shot("synthetic-proposal-pad1-open","Same synthetic candidate, selecting pad1 switches the visible target without retargeting parameters");face.showProposal(nullptr);
 }
 CHECK(!face.hasStrip());manifest.add("Folded MakerFace is unavailable in frozen Tile artwork (no strip); actual RackStack folded tape/rail is captured by the owned Standalone/connected stack workflow, not fabricated here.");
 CHECK(folder.getChildFile("fixtures.tsv").replaceWithText(manifest.joinIntoString("\n")+"\n"));
}
void capture(const juce::File& output){
 studio::house::GeneratedText textScope;std::cout<<"NATIVE_GDIPLUS_INITIALIZED "<<textScope.available()<<std::endl;CHECK(textScope.available());
 struct Probe final:juce::Component{void paint(juce::Graphics& g)override{std::cout<<"NATIVE_COMPOSITING_CONTEXT "<<typeid(g.getInternalContext()).name()<<" physicalScale="<<g.getInternalContext().getPhysicalPixelScaleFactor()<<std::endl;}};
 for(const auto& spec:std::array<std::pair<const char*,float>,3>{{{"Consolas",29.f},{"Consolas",45.f},{"Bahnschrift",42.f}}}){
  for(const bool em:{false,true}){auto options=juce::FontOptions(spec.first,spec.second,juce::String(spec.first)=="Bahnschrift"?juce::Font::bold:juce::Font::plain);if(em)options=options.withPointHeight(spec.second);juce::Font font(options);
   std::cout<<"FONT_METRICS family="<<spec.first<<" requested="<<spec.second<<" em="<<em<<" height="<<font.getHeight()<<" points="<<font.getHeightInPoints()<<" ascent="<<font.getAscent()<<" descent="<<font.getDescent()<<" d2dHint="<<options.getDirect2DHinting()<<" face="<<font.getTypefaceName()<<" style="<<font.getTypefaceStyle()<<std::endl;}
 }
 CHECK(!output.exists());CHECK(output.createDirectory().wasOk());FaceRig rig;if(!rig.face)return;auto& face=*rig.face;Probe probe;probe.setBounds(0,0,1,1);face.addAndMakeVisible(probe);face.face.hideReadouts();face.revealed().hideReadouts();pump();const auto closed=face.createComponentSnapshot(face.getLocalBounds(),true,1.f);CHECK(closed.getWidth()==2140&&closed.getHeight()==788);CHECK(png(output.getChildFile("tile-array-drawn.png"),closed));
 face.setOpen(true,false);pump();const auto open=face.createComponentSnapshot(face.getLocalBounds(),true,1.f);CHECK(open.getWidth()==2140&&open.getHeight()==1576);CHECK(png(output.getChildFile("tile-array-open-drawn.png"),open));
 if(auto* tile=face.tileArray()){tile->selectPad(15,true);if(auto* tune=rig.slider("pad.16.tune_semitones"))tune->setValue(7,juce::sendNotificationSync);pump();CHECK(png(output.getChildFile("tile-array-pad16-nondefault.png"),face.createComponentSnapshot(face.getLocalBounds(),true,1.f)));}
 SoundDocument sound;std::uint64_t edit=0;CHECK(rig.processor->trySoundSnapshot(sound,edit));sound.synthLocks[0]=true;sound.controlLocks[drumParameterStart]=true;CHECK(rig.processor->applySoundEdit(sound,SoundEditOrigin::manual,edit));face.refresh(sound);CHECK(png(output.getChildFile("tile-array-locked-open.png"),face.createComponentSnapshot(face.getLocalBounds(),true,1.f)));
 captureControlledReviewStates(output);
}
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;const auto preparedSkins=prepareTestMakerSkins();studio::motion::setForTests({},true);if(argc==3&&juce::String(argv[1])=="--capture")capture(juce::File(argv[2]));else if(argc>1&&juce::String(argv[1])=="--desktop")exact_pad_accessibility_and_posted_navigation();else{existingApiRed();frozenHouseContract();recipe_fixed_bindings_and_held_selection();drum_locks_deny_numeric_begin_and_live_set();integer_input_methods_continuous_rotor_and_host_marks();generated_text_scoped_lifecycle();real_pad_admission_errors();}std::cout<<"TileArray checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
