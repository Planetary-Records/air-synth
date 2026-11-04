#include "OwnedFixture.h"
#include "InstrumentProcessor.h"
#include "AllControlsPanel.h"
#include "UserReach.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "CompanionWire.h"
#include "FxMonitor.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <iostream>
#include <set>
#include <algorithm>
#include <bit>
#include <limits>

namespace
{
using namespace airack;
int checks=0,failures=0;
void check(bool good,const char* expression,int line)
{++checks;if(!good){++failures;std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Notifications final : juce::AudioProcessorParameter::Listener
{
    explicit Notifications(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
    ~Notifications() override{parameter.removeListener(this);}
    void parameterValueChanged(int,float) override{++values;if(onValue)onValue();}
    void parameterGestureChanged(int,bool began) override{if(began){++starts;if(onStart)onStart();}else {++ends;if(onEnd)onEnd();}}
    std::function<void()> onStart,onValue,onEnd;
    juce::AudioProcessorParameter& parameter;int starts=0,ends=0,values=0;
};
SoundDocument snapshot(InstrumentProcessor& processor)
{SoundDocument sound;std::uint64_t edit=0;CHECK(processor.trySoundSnapshot(sound,edit));return sound;}
std::size_t find(const juce::String& id)
{const auto registry=parameterRegistry();for(std::size_t i=0;i<registry.size();++i)if(registry[i].id==id)return i;CHECK(false);return parameterCount;}
juce::Component* component(juce::Component& root,const juce::String& id)
{
    if(root.getComponentID()==id)return &root;
    for(auto* child:root.getChildren())if(auto* found=component(*child,id))return found;
    return nullptr;
}
void optionalAuxBusHasAnExplicitDisabledDefault()
{
    std::cout<<"SECTION optional_aux_bus_disabled_default_and_layout_refusals\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("sidechain-bus");
    CHECK(root.createDirectory().wasOk());
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.fixtureRoot=root;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");
    auto processor=std::make_unique<InstrumentProcessor>(offline);
    CHECK(processor->getBusCount(true)==1);CHECK(processor->getBusCount(false)==1);
    if(processor->getBusCount(true)!=1||processor->getBusCount(false)!=1)return;
    const auto disabled=processor->getBusesLayout();
    CHECK(disabled.inputBuses[0].isDisabled());
    CHECK(disabled.outputBuses[0]==juce::AudioChannelSet::stereo());
    CHECK(processor->getTotalNumInputChannels()==0&&processor->isBusesLayoutSupported(disabled));
    const auto* extension=processor->getVST3ClientExtensions();CHECK(extension&&!extension->getPluginHasMainInput());
    auto stereo=disabled;stereo.inputBuses.set(0,juce::AudioChannelSet::stereo());
    CHECK(processor->isBusesLayoutSupported(stereo)&&processor->setBusesLayout(stereo));
    CHECK(processor->getBusesLayout()==stereo&&processor->getTotalNumInputChannels()==2);
    auto mono=stereo;mono.inputBuses.set(0,juce::AudioChannelSet::mono());
    CHECK(!processor->isBusesLayoutSupported(mono)&&!processor->setBusesLayout(mono));
    CHECK(processor->getBusesLayout()==stereo);
    auto output=stereo;output.outputBuses.set(0,juce::AudioChannelSet::mono());CHECK(!processor->isBusesLayoutSupported(output));
    auto missing=stereo;missing.inputBuses.clear();CHECK(!processor->isBusesLayoutSupported(missing));
    CHECK(processor->setBusesLayout(disabled)&&processor->getTotalNumInputChannels()==0);
}
void rawHostParameterSnapshotIsExact()
{
    std::cout<<"SECTION all_raw_host_values_snapshot_bits_and_live_automation\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("raw-host-snapshot");
    CHECK(root.createDirectory().wasOk());
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.fixtureRoot=root;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");
    auto processor=std::make_unique<InstrumentProcessor>(offline);
    auto sound=std::make_unique<SoundDocument>();std::uint64_t edit=0;
    CHECK(processor->trySoundSnapshot(*sound,edit));
    for(float value:{0.f,-0.f,.3712345f,.98123f,1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-.5f,1.5f}){
        for(std::size_t i=0;i<parameterCount;++i)processor->control(i).setValue(value);
        CHECK(processor->trySoundSnapshot(*sound,edit));
        for(std::size_t i=0;i<parameterCount;++i)
            CHECK(std::bit_cast<std::uint32_t>(sound->values[i])==std::bit_cast<std::uint32_t>(processor->control(i).getValue()));
    }
    const auto before=edit;processor->control(0).setValue(.423f);
    CHECK(processor->trySoundSnapshot(*sound,edit)&&edit==before);
    CHECK(std::bit_cast<std::uint32_t>(sound->values[0])==std::bit_cast<std::uint32_t>(processor->control(0).getValue()));
}
void manualRetention()
{
    std::cout<<"SECTION actual_editor_new_control_retention\n";
    std::cout<<"PROCESSOR_FOOTPRINT bytes="<<sizeof(InstrumentProcessor)<<" limit="<<128*1024<<'\n';CHECK(sizeof(InstrumentProcessor)<=128*1024);
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;InstrumentProcessor processor(offline);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());CHECK(editor!=nullptr);if(!editor)return;
    std::size_t retained=0;
    for(std::size_t index=dynamicEqParameterStart;index<parameterCount;++index)if(component(*editor,parameterRegistry()[index].id))++retained;
    std::cout<<"RETAINED dynamic_controls="<<retained<<" expected="<<parameterCount-dynamicEqParameterStart<<'\n';
    CHECK(retained==parameterCount-dynamicEqParameterStart);
    const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    std::array<int,parameterCount> occurrences{};
    const auto count=[&](const auto& rows){for(const auto& row:rows)for(const auto& section:row)for(const auto& knob:section.knobs)
        if(knob.index>=int(dynamicEqParameterStart))++occurrences[std::size_t(knob.index)];};
    for(const auto& page:model->pages)for(const auto& sub:page.subpages){count(sub.rows);for(const auto& route:sub.routes)count(route.rows);}
    for(std::size_t i=dynamicEqParameterStart;i<parameterCount;++i)CHECK(occurrences[i]==1);
    studio::AllControlsPanel panel(processor,*model,{});
    for(std::uint8_t bank=0;bank<8;++bank){
        panel.show({{NativeModule::dynamicEq,bank}});CHECK(panel.parameters()==unitReach(NativeModule::dynamicEq,bank));
        CHECK(panel.keys().size()==1);std::set<std::size_t> visited;
        const auto* first=panel.knob(dynamicEqParameterStart+bank*dynamicEqParametersPerSlot);
        const auto owned=unitReach(NativeModule::dynamicEq,bank);
        for(std::size_t page=0;page<owned.size();++page){
            CHECK(panel.routesPage()==page);
            const auto knobs=panel.shownKnobs();CHECK(!knobs.empty());
            for(auto* knob:knobs){visited.insert(knob->index);if(const auto extra=knob->pairedParameter())visited.insert(*extra);CHECK(!knob->getBounds().isEmpty());
                CHECK(panel.getLocalBounds().contains(panel.getLocalArea(knob,knob->getLocalBounds())));
                CHECK(panel.getLocalBounds().contains(panel.getLocalArea(&knob->label,knob->label.getLocalBounds())));}
            panel.showKey(0);
            if(panel.routesPage()==0)break;
        }
        CHECK(visited==std::set<std::size_t>(owned.begin(),owned.end()));CHECK(panel.routesPage()==0);
        panel.show({{NativeModule::dynamicEq,bank}});CHECK(first==panel.knob(dynamicEqParameterStart+bank*dynamicEqParametersPerSlot));
        const auto gainBase=dynamicEqGainParameterStart+bank*dynamicEqGainParametersPerSlot;
        for(const auto gain:std::array<std::size_t,6>{dynamicEqParameterStart+bank*dynamicEqParametersPerSlot+4,
                dynamicEqParameterStart+bank*dynamicEqParametersPerSlot+7*dynamicEqParametersPerBand+4,gainBase,gainBase+1,
                dynamicEqPlacementParameterStart+bank*8,dynamicEqPlacementParameterStart+bank*8+7}){
            auto* knob=panel.knob(gain);CHECK(knob);if(!knob)continue;
            panel.refresh(snapshot(processor));
            if(gain>=dynamicEqGainParameterStart){CHECK(knob->getAlpha()==1.f);CHECK(knob->label.getAlpha()==1.f);CHECK(knob->getDescription().isEmpty());}
            const auto value=gain==gainBase?1.f:.75f;
            const auto before=snapshot(processor);knob->move(value);const auto after=snapshot(processor);CHECK(after.values[gain]==value);
            CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==before);CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==after);
        }
    }
}
void manualExtensionSelectors()
{
    std::cout<<"SECTION unified_manual_shape_slope_selectors_preserve_raw_state\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    InstrumentProcessor processor(offline);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    studio::AllControlsPanel panel(processor,*model,{});
    for(std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)}){
        panel.show({{NativeModule::dynamicEq,slot}});CHECK(panel.parameters()==unitReach(NativeModule::dynamicEq,slot));
        for(std::size_t band:{std::size_t(0),std::size_t(7)}){
            const auto base=dynamicEqParameterStart+slot*dynamicEqParametersPerSlot+band*11,extension=dynamicEqExtensionParameterStart+slot*16+band*2;
            for(int field=0;field<2;++field){
                processor.control(extension).setValueNotifyingHost(0);processor.control(extension+1).setValueNotifyingHost(0);
                processor.control(base+2).setValueNotifyingHost(field==0?1.f:.6f);processor.control(base+6).setValueNotifyingHost(1);
                panel.refresh(snapshot(processor));
                const auto legacy=base+(field==0?2:6),extra=extension+std::size_t(field);auto* knob=panel.knob(legacy);
                CHECK(knob);CHECK(knob&&panel.knob(extra)==knob);CHECK(panel.showParameter(extra));
                if(!knob)continue;CHECK(knob->steps()==(field==0?8:5));
                CHECK(panel.shownKnobs().size()==12);
                if(panel.knob(extra)!=knob)continue;
                const auto before=snapshot(processor);knob->move(1);const auto after=snapshot(processor);
                CHECK(after.values[extra]==1);CHECK(after.values[legacy]==before.values[legacy]);
                CHECK(knob->valueLine().containsIgnoreCase(field==0?"Flat tilt":"96 dB/oct"));
                CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==before);CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==after);
                // Return to a classic choice in one transaction without disturbing the paired selector's other bank.
                knob->move(field==0?0.:.2);const auto classic=snapshot(processor);CHECK(classic.values[extra]==0&&classic.values[legacy]==0);
                CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==after);CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==classic);
                processor.control(extra).setValueNotifyingHost(1);knob->reread();CHECK(knob->getValue()==(field==0?8:5));
            }
        }
    }
}
void extensionSelectorGuardsAndLifetime()
{
    std::cout<<"SECTION paired_selectors_entry_locks_stale_menu_reset_and_retirement\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    InstrumentProcessor p(offline);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    const auto base=dynamicEqParameterStart+7*dynamicEqParametersPerSlot,extension=dynamicEqExtensionParameterStart+7*16;
    SoundDocument source;RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=7;edit.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(source.layout,edit,source.layout)==RackEditResult::applied);source.values[base]=1;source.values[base+2]=.6f;
    const auto install=[&](const SoundDocument& sound){juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));};
    for(int field=0;field<2;++field){
        const auto legacy=base+(field==0?2:6),extra=extension+std::size_t(field);
        Notifications first(p.control(legacy)),second(p.control(extra));
        auto panel=std::make_unique<studio::AllControlsPanel>(p,*model,studio::ProgrammerHost{});
        panel->show({{NativeModule::dynamicEq,7}});auto* knob=panel->knob(legacy);CHECK(knob);if(!knob)continue;
        const auto reset=[&](const SoundDocument& d){install(d);panel->refresh(snapshot(p));CHECK(panel->showParameter(extra));knob->reread();};
        reset(source);
        knob->openEntry();CHECK(knob->entry.isVisible());knob->entry.setText(field==0?"Flat tilt":"96",false);knob->commitEntry();
        const auto entered=snapshot(p);CHECK(entered.values[extra]==1);CHECK(entered.values[legacy]==source.values[legacy]);
        CHECK(p.undoSoundEdit());CHECK(snapshot(p)==source);CHECK(p.redoSoundEdit());CHECK(snapshot(p)==entered);
        knob->openEntry();knob->entry.setText("Not a filter choice",false);knob->commitEntry();CHECK(snapshot(p)==entered);
        for(int lock=0;lock<3;++lock){
            auto locked=source;if(lock==0)locked.effectLocks[7]=true;else locked.controlLocks[lock==1?legacy:extra]=true;
            reset(locked);const auto starts=first.starts+second.starts;knob->move(1);
            CHECK(snapshot(p)==locked);CHECK(first.starts+second.starts==starts);CHECK(knob->getValue()==(field==0?3:1));
        }
        // The default menu captures its context before a host reset or an unrelated automation write.
        for(bool recall:{false,true}){
            reset(entered);auto menu=knob->menu();CHECK(menu.size()>=2);const auto starts=first.starts+second.starts;
            if(recall)install(entered);else p.control(0).setValueNotifyingHost(.23f);
            const auto live=snapshot(p);menu[1].second();CHECK(snapshot(p)==live);CHECK(first.starts+second.starts==starts);
        }
        panel->addToDesktop(0);panel->setVisible(true);
        for(int stale=0;stale<3;++stale){
            reset(entered);CHECK(knob->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
            auto* menu=juce::Component::getCurrentlyModalComponent();CHECK(menu);if(!menu)continue;
            if(stale==1)install(entered);if(stale==2)p.control(0).setValueNotifyingHost(.23f);
            const auto live=snapshot(p);const auto starts=first.starts+second.starts;
            CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            for(int n=0;n<50&&juce::Component::getCurrentlyModalComponent();++n)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);CHECK(!juce::Component::getCurrentlyModalComponent());
            if(stale){CHECK(snapshot(p)==live);CHECK(first.starts+second.starts==starts);}
            else{CHECK(snapshot(p).values[extra]==(field==0?0.f:.25f));CHECK(snapshot(p).values[legacy]==0);CHECK(p.undoSoundEdit());CHECK(snapshot(p)==entered);}
        }
        juce::PopupMenu::dismissAllActiveMenus();panel->removeFromDesktop();
        for(bool recall:{false,true}){
            reset(source);second.onStart=[&]{if(recall)install(source);else p.control(0).setValueNotifyingHost(.23f);};
            knob->move(1);second.onStart=nullptr;auto expected=source;if(!recall)expected.values[0]=.23f;
            CHECK(snapshot(p)==expected);CHECK(first.starts==first.ends);CHECK(second.starts==second.ends);
        }
        // Static shapes retain saved dynamics while making unavailable fields explicit.
        if(field==0)for(int shape=1;shape<=3;++shape){
            auto d=source;d.values[extension]=float(shape)/3;d.values[base+7]=.25f;reset(d);
            CHECK(!panel->knob(base+7)->isEnabled());CHECK(snapshot(p)==d);
            CHECK(panel->knob(base+4)->isEnabled()==(shape!=1));CHECK(panel->knob(base+5)->isEnabled()==(shape!=3));
        }
        panel.reset();
        for(int stage=0;stage<3;++stage){
            install(source);panel=std::make_unique<studio::AllControlsPanel>(p,*model,studio::ProgrammerHost{});panel->show({{NativeModule::dynamicEq,7}});
            knob=panel->knob(legacy);CHECK(knob);if(!knob)continue;
            const juce::Component::SafePointer<studio::ProgrammerKnob> safe(knob);const auto close=[&]{panel.reset();};
            if(stage==0)second.onStart=close;if(stage==1)second.onValue=close;if(stage==2)second.onEnd=close;
            knob->move(1);second.onStart=nullptr;second.onValue=nullptr;second.onEnd=nullptr;
            CHECK(!panel&&!safe);CHECK(first.starts==first.ends);CHECK(second.starts==second.ends);
            const auto after=snapshot(p);CHECK(after.values[extra]==(stage==0?0:1));CHECK(after.values[legacy]==source.values[legacy]);
            CHECK(p.beginParameterGesture(legacy));CHECK(p.endParameterGesture(legacy));
        }
    }
}
void hostStateAndGestures()
{
    std::cout<<"SECTION actual_host_parameter_state_and_undo\n";
    std::cout<<"FOOTPRINT processor_bytes="<<sizeof(InstrumentProcessor)<<'\n';
    CHECK(sizeof(InstrumentProcessor)<=128*1024);
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("dynamic-eq-host-"+juce::Uuid().toString());
    CHECK(root.createDirectory().wasOk());companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    InstrumentProcessor processor(offline);
    CHECK(dynamicEqExtensionParameterStart==2063); // Frozen predecessor endpoint.
    CHECK(dynamicEqDetectorParameterStart==2215); // Frozen predecessor endpoint.
    CHECK(processor.getParameters().size()==4519);
    for(std::size_t i=0;i<parameterCount;++i){CHECK(processor.control(i).paramID==parameterRegistry()[i].id);CHECK(processor.control(i).getParameterIndex()==int(i));}
    SoundDocument sound;RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=7;edit.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(sound.layout,edit,sound.layout)==RackEditResult::applied);
    const auto active=find("effect.8.dynamic_eq.band.1.active"), gain=find("effect.8.dynamic_eq.band.1.gain_db");
    const auto frequency=find("effect.8.dynamic_eq.band.1.frequency_hz");
    if(active>=parameterCount||gain>=parameterCount||frequency>=parameterCount)return;
    sound.values[active]=1;
    juce::MemoryBlock wire;CHECK(encodeSoundState(sound,wire));CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));
    const auto before=snapshot(processor);
    CHECK(processor.beginParameterGesture(gain));CHECK(processor.setParameterGestureValue(gain,.75f));CHECK(processor.endParameterGesture(gain));
    const auto edited=snapshot(processor);CHECK(edited.values[gain]==.75f);CHECK(edited!=before);
    CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==before);CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==edited);
    processor.control(gain).setValueNotifyingHost(.25f);CHECK(snapshot(processor).values[gain]==.25f);
    auto ringing=snapshot(processor);ringing.values[gain]=1;ringing.values[frequency]=0;
    const auto q=find("effect.8.dynamic_eq.band.1.q");CHECK(q<parameterCount);if(q<parameterCount)ringing.values[q]=1;
    CHECK(encodeSoundState(ringing,wire));CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));
    processor.prepareToPlay(48000,64);
    std::cout<<"TAIL host_seconds="<<processor.getTailLengthSeconds()<<'\n';CHECK(processor.getTailLengthSeconds()>5);
    juce::AudioBuffer<float> buffer(2,64);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
    processor.processBlock(buffer,midi);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)CHECK(std::isfinite(buffer.getSample(ch,n)));
    processor.getStateInformation(wire);InstrumentProcessor recalled(offline);
    CHECK(recalled.restoreProjectState(wire.getData(),int(wire.getSize())));CHECK(snapshot(recalled)==snapshot(processor));
    const auto preset=root.getChildFile("dynamic.airacksound");CHECK(processor.saveSoundPreset(preset));
    processor.control(gain).setValueNotifyingHost(.5f);CHECK(processor.loadSoundPreset(preset));
    auto named=snapshot(recalled);named.name="dynamic";CHECK(snapshot(processor)==named);
    ProjectDocument project;std::uint64_t revision=0;CHECK(processor.tryProjectSnapshot(project,revision));
    juce::MemoryBlock projectBytes;CHECK(encodeProjectState(project,projectBytes));CHECK(recalled.restoreProjectState(projectBytes.getData(),int(projectBytes.getSize())));
    CHECK(snapshot(recalled)==project.sound);processor.releaseResources();
}
void monitoringLifetime()
{
    std::cout<<"SECTION physical_monitor_handle_and_stopped_processor_lifetime\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    auto processor=std::make_unique<InstrumentProcessor>(offline);
    auto sound=snapshot(*processor);RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(sound.layout,add,sound.layout)==RackEditResult::applied);sound.values[dynamicEqParameterStart+7*89]=1;
    juce::MemoryBlock state;CHECK(encodeSoundState(sound,state));CHECK(processor->restoreSoundState(state.getData(),int(state.getSize())));
    const auto handle=processor->fxMonitorHandle();CHECK(handle);if(!handle)return;
    const auto token=handle->subscribe(7);CHECK(token);if(!token)return;
    juce::MemoryBlock unchanged;CHECK(encodeSoundState(snapshot(*processor),unchanged));CHECK(unchanged==state);
    processor->prepareToPlay(48000,64);juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);for(int n=0;n<8;++n){processor->processBlock(audio,midi);midi.clear();}
    FxMonitor::Packet packet;CHECK(handle->read(*token,packet));CHECK(packet.sampleRate==48000);CHECK(processor->fxMonitorHandle()==handle);
    const auto ordinaryTail=processor->getTailLengthSeconds();
    CHECK(handle->requestAudition(*token,0));CHECK(processor->getTailLengthSeconds()>=ordinaryTail+8);
    processor->processBlock(audio,midi);CHECK(handle->requestAudition(*token,std::nullopt));CHECK(processor->getTailLengthSeconds()>=ordinaryTail+8);
    for(int n=0;n<16;++n)processor->processBlock(audio,midi);CHECK(processor->getTailLengthSeconds()==ordinaryTail);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());CHECK(editor);editor.reset();
    CHECK(encodeSoundState(snapshot(*processor),unchanged));CHECK(unchanged==state);
    const auto epoch=handle->generation(*token);processor->releaseResources();CHECK(handle->generation(*token)!=epoch);CHECK(!handle->read(*token,packet));
    const auto stopped=handle->generation(*token);processor.reset();CHECK(handle->generation(*token)!=stopped);CHECK(!handle->read(*token,packet));CHECK(handle->retire(*token));
}
void guardedBandCreation()
{
    std::cout<<"SECTION guarded_eleven_field_creation_and_reentrant_host_changes\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    InstrumentProcessor processor(offline);
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    juce::MemoryBlock wire;CHECK(encodeSoundState(document,wire));CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));
    const auto base=dynamicEqParameterStart+7*dynamicEqParametersPerSlot;
    std::array<ParameterGestureValue,11> edits;
    for(std::size_t n=0;n<edits.size();++n)edits[n]={base+n,processor.control(base+n).getDefaultValue()};
    edits[0].value=1;edits[3].value=processor.control(base+3).convertTo0to1(2000);edits[4].value=.75f;edits[5].value=processor.control(base+5).convertTo0to1(2);
    SoundDocument before;std::uint64_t revision=0,reset=0;CHECK(processor.trySoundSnapshot(before,revision,reset));
    Notifications active(processor.control(base)),frequency(processor.control(base+3)),gain(processor.control(base+4)),q(processor.control(base+5));
    CHECK(processor.beginParameterGesture(base));
    CHECK(processor.setParameterGestureValues(base,edits,before,revision,reset));
    CHECK(processor.endParameterGesture(base));
    auto after=snapshot(processor);CHECK(after.values[base]==1);CHECK(after.values[base+4]==.75f);
    CHECK(active.starts==1&&active.ends==1);CHECK(frequency.starts==1&&frequency.ends==1);CHECK(q.starts==1&&q.ends==1);
    CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==before);CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==after);
    CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));
    CHECK(processor.trySoundSnapshot(before,revision,reset));
    frequency.onStart=[&]{processor.control(0).setValueNotifyingHost(before.values[0]==.2f?.3f:.2f);};
    CHECK(processor.beginParameterGesture(base));
    CHECK(!processor.setParameterGestureValues(base,edits,before,revision,reset));
    CHECK(processor.endParameterGesture(base));
    after=snapshot(processor);CHECK(after.values[base]==0);CHECK(after.values[0]!=before.values[0]);
    for(std::size_t n=0;n<11;++n)CHECK(after.values[base+n]==before.values[base+n]);
    CHECK(active.starts==active.ends&&frequency.starts==frequency.ends&&gain.starts==gain.ends&&q.starts==q.ends);
    frequency.onStart=nullptr;
    CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));CHECK(processor.trySoundSnapshot(before,revision,reset));
    active.onStart=[&]{CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));};
    CHECK(processor.beginParameterGesture(base));CHECK(!processor.setParameterGestureValues(base,edits,before,revision,reset));CHECK(processor.endParameterGesture(base));
    CHECK(snapshot(processor)==before);CHECK(active.starts==active.ends);
}
void guardedWholeEqGroup()
{
    std::cout<<"SECTION guarded_whole_eq_group_atomic_preflight_and_notifications\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    InstrumentProcessor processor(offline);
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    const auto restore=[&](const SoundDocument& d){juce::MemoryBlock bytes;CHECK(encodeSoundState(d,bytes));CHECK(processor.restoreSoundState(bytes.getData(),int(bytes.getSize())));};
    restore(document);
    const auto reach=unitReach(NativeModule::dynamicEq,7);CHECK(reach.size()==rackUnitParameterCapacity(NativeModule::dynamicEq));
    std::vector<ParameterGestureValue> edits;std::vector<std::unique_ptr<Notifications>> events;
    for(const auto index:reach){edits.push_back({index,document.values[index]});events.push_back(std::make_unique<Notifications>(processor.control(index)));}
    const auto leader=reach.front(),last=reach.back();
    edits.front().value=edits.front().value>=.5f?0.f:1.f;
    edits.back().value=.75f;
    SoundDocument before;std::uint64_t revision=0,reset=0;CHECK(processor.trySoundSnapshot(before,revision,reset));
    const bool began=processor.beginParameterGesture(leader,edits,before,revision,reset);CHECK(began);
    if(began){CHECK(snapshot(processor)==before);for(const auto& event:events)CHECK(event->starts==1&&event->ends==0);
        CHECK(processor.setParameterGestureValues(leader,edits,before,revision,reset));CHECK(processor.endParameterGesture(leader));}
    const auto after=snapshot(processor);CHECK(after.values[leader]==edits.front().value);CHECK(after.values[last]==edits.back().value);
    for(const auto& event:events)CHECK(event->starts==1&&event->ends==1);
    if(began){CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==before);CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==after);}
    const auto attempts=[&]{int count=0;for(const auto& event:events)count+=event->starts+event->ends+event->values;return count;};
    const auto refuse=[&](const SoundDocument& expected,std::uint64_t rev,std::uint64_t epoch,const auto& values){
        const auto live=snapshot(processor);const auto calls=attempts();
        const bool opened=processor.beginParameterGesture(leader,values,expected,rev,epoch);CHECK(!opened);
        CHECK(snapshot(processor)==live);CHECK(attempts()==calls);if(opened)CHECK(processor.endParameterGesture(leader));
    };
    auto locked=document;locked.controlLocks[last]=true;restore(locked);CHECK(processor.trySoundSnapshot(before,revision,reset));
    // Even a submitted no-op locked field must refuse before announcing a leader.
    edits.back().value=before.values[last];refuse(before,revision,reset,edits);
    locked=document;locked.effectLocks[7]=true;restore(locked);CHECK(processor.trySoundSnapshot(before,revision,reset));refuse(before,revision,reset,edits);
    restore(document);CHECK(processor.trySoundSnapshot(before,revision,reset));edits.back().value=.75f;
    refuse(before,revision+1,reset,edits);refuse(before,revision,reset+1,edits);
    auto stale=before;stale.values[0]=.23f;refuse(stale,revision,reset,edits);
    auto malformed=edits;malformed.back()=malformed.front();refuse(before,revision,reset,malformed);
    malformed=edits;malformed.back().value=1.01f;refuse(before,revision,reset,malformed);
    malformed=edits;malformed.push_back({0,.2f});refuse(before,revision,reset,malformed);
    malformed=edits;malformed.erase(malformed.begin());refuse(before,revision,reset,malformed);
    // A host callback can automate unrelated raw values without advancing the edit revision.
    events.back()->onStart=[&]{processor.control(0).setValueNotifyingHost(.23f);};
    bool opened=processor.beginParameterGesture(leader,edits,before,revision,reset);CHECK(opened);
    if(opened){CHECK(!processor.setParameterGestureValues(leader,edits,before,revision,reset));CHECK(processor.endParameterGesture(leader));}
    auto live=snapshot(processor);CHECK(live.values[0]==.23f);for(const auto index:reach)CHECK(live.values[index]==before.values[index]);
    events.back()->onStart=nullptr;restore(document);CHECK(processor.trySoundSnapshot(before,revision,reset));
    events.front()->onStart=[&]{restore(document);};
    opened=processor.beginParameterGesture(leader,edits,before,revision,reset);CHECK(opened);
    if(opened){CHECK(!processor.setParameterGestureValues(leader,edits,before,revision,reset));CHECK(processor.endParameterGesture(leader));}
    CHECK(snapshot(processor)==before);for(const auto& event:events)CHECK(event->starts==event->ends);
    events.front()->onStart=nullptr;
    // Historical unguarded batches retain their five-field limit.
    CHECK(processor.beginParameterGesture(leader));CHECK(!processor.setParameterGestureValues(leader,std::span(edits).first(6)));CHECK(processor.endParameterGesture(leader));
}
void guardedGroupUndoKeepsUnrelatedAutomation()
{
    std::cout<<"SECTION guarded_group_history_preserves_unrelated_live_automation\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
    InstrumentProcessor processor(offline);SoundDocument source;
    for(std::uint8_t slot:{6,7}){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind::dynamicEq;CHECK(editRackLayout(source.layout,add,source.layout)==RackEditResult::applied);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(source,bytes));CHECK(processor.restoreSoundState(bytes.getData(),int(bytes.getSize())));
    const auto gain=find("effect.8.dynamic_eq.band.1.gain_db"),frequency=find("effect.8.dynamic_eq.band.1.frequency_hz"),other=find("effect.7.dynamic_eq.band.1.gain_db");
    SoundDocument before;std::uint64_t revision,reset;CHECK(processor.trySoundSnapshot(before,revision,reset));
    const ParameterGestureValue edits[]{{gain,.7f},{frequency,.6f}};
    const bool opened=processor.beginParameterGesture(gain,edits,before,revision,reset);CHECK(opened);if(!opened)return;
    CHECK(processor.setParameterGestureValues(gain,edits,before,revision,reset));
    processor.control(other).setValueNotifyingHost(.8f);CHECK(processor.endParameterGesture(gain));const auto after=snapshot(processor);
    auto expected=before;expected.values[other]=.8f;CHECK(processor.undoSoundEdit());CHECK(snapshot(processor)==expected);
    // Automation after the gesture also remains outside this action's redo.
    processor.control(0).setValueNotifyingHost(.23f);expected=after;expected.values[0]=.23f;
    CHECK(processor.redoSoundEdit());CHECK(snapshot(processor)==expected);
}
void repeatedSoundAndLiveAutomationAudio(bool placements=false)
{
    std::cout<<"SECTION repeated_sound_mapping_host_automation_recall_and_reprepare_exact_audio\n";
    for(bool higher:{false,true}){
        const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
        companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
        auto processor=std::make_unique<InstrumentProcessor>(offline);
        SoundDocument source;source.values[758]=higher?1.f:0.f;
        for(std::uint8_t slot=0;slot<2;++slot){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind::dynamicEq;CHECK(editRackLayout(source.layout,add,source.layout)==RackEditResult::applied);source.values[dynamicEqParameterStart+slot*dynamicEqParametersPerSlot]=1;}
        const auto gain=dynamicEqParameterStart+4,frequency=dynamicEqParameterStart+3;
        source.macros[0]={parameterRegistry()[gain].stateKey,.1f};source.values[1]=.25f;
        if(placements){source.values[find("synth.1.pan")]=.25f;
            source.values[dynamicEqPlacementParameterStart]=.25f;source.values[dynamicEqPlacementParameterStart+8]=1.f;
            source.values[dynamicEqParameterStart+4]=.75f;source.values[dynamicEqParameterStart+dynamicEqParametersPerSlot+4]=.75f;}
        juce::MemoryBlock wire;CHECK(encodeSoundState(source,wire));CHECK(processor->restoreSoundState(wire.getData(),int(wire.getSize())));
        processor->setNonRealtime(higher);processor->prepareToPlay(48000,64);
        auto reference=std::make_unique<RoutedSynthEngine>();RoutedPatch patch;CHECK(makeRoutedPatch(source,patch));CHECK(reference->setPatch(patch));CHECK(reference->prepare(48000,64,higher?RenderQuality::higher:RenderQuality::realtime));
        juce::AudioBuffer<float> actual(2,64),expected(2,64);juce::MidiBuffer midi;bool exact=true;float peak=0;int firstMismatch=-1;
        for(int block=0;block<480;++block){
            if(placements&&block>=32&&block<=160&&block%32==0){
                const auto mode=std::size_t(block/32-1);
                processor->control(dynamicEqPlacementParameterStart).setValueNotifyingHost(float(mode)/4);
                processor->control(dynamicEqPlacementParameterStart+8).setValueNotifyingHost(float(4-mode)/4);
                const auto live=snapshot(*processor);CHECK(live.values[dynamicEqPlacementParameterStart]==float(mode)/4);
                CHECK(live.values[dynamicEqPlacementParameterStart+8]==float(4-mode)/4);
            }
            if(block==128){std::uint64_t beforeRevision=0,afterRevision=0;SoundDocument before,after;CHECK(processor->trySoundSnapshot(before,beforeRevision));processor->control(gain).setValueNotifyingHost(.7f);processor->control(frequency).setValueNotifyingHost(.6f);CHECK(processor->trySoundSnapshot(after,afterRevision));CHECK(beforeRevision==afterRevision);CHECK(after.values[gain]!=before.values[gain]);}
            if(block==192)processor->control(1).setValueNotifyingHost(.8f);
            if(block==256||block==320){auto current=snapshot(*processor);if(block==320)current.macros[0]={parameterRegistry()[dynamicEqParameterStart+dynamicEqParametersPerSlot+4].stateKey,-.2f};CHECK(encodeSoundState(current,wire));CHECK(processor->restoreSoundState(wire.getData(),int(wire.getSize())));}
            if(block==384){processor->prepareToPlay(96000,64);CHECK(reference->prepare(96000,64,higher?RenderQuality::higher:RenderQuality::realtime));}
            if(block==416){processor->prepareToPlay(7999,64);CHECK(!reference->prepare(7999,64,higher?RenderQuality::higher:RenderQuality::realtime));}
            if(block==448)processor->control(dynamicEqParameterStart+1).setValueNotifyingHost(1);
            const auto current=snapshot(*processor);CHECK(makeRoutedPatch(current,patch));CHECK(reference->setPatch(patch));
            // The product installs the recalled patch before its reset epoch.
            if(block==256||block==320)reference->reset();
            midi.clear();if(block==0||block==256||block==320||block==384)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
            processor->processBlock(actual,midi);reference->render(expected,midi);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<64;++frame){const auto a=actual.getSample(ch,frame),b=expected.getSample(ch,frame);const bool same=std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);if(!same&&firstMismatch<0)firstMismatch=block;exact=exact&&same;peak=std::max(peak,std::abs(a));}
        }
        std::cout<<"MAPPING_AUDIO placements="<<placements<<" higher="<<higher<<" exact="<<exact<<" first_mismatch_block="<<firstMismatch<<" peak="<<peak<<'\n';
        CHECK(exact);CHECK(peak>.001f);processor->releaseResources();
    }
}
void externalDetectorProductAudio()
{
    std::cout<<"SECTION external_detector_saved_selectors_aux_copy_disconnect_and_large_callbacks\n";
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    for(bool higher:{false,true})for(int preparedBlock:{64,512})for(double rate:{48000.,96000.}){
        companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
        auto processor=std::make_unique<InstrumentProcessor>(offline);
        SoundDocument source;source.values[758]=higher?1.f:0.f;for(auto& revisions:source.compatibility.synth)revisions.fill(modernSynthAlgorithmRevision);
        for(std::uint8_t slot=0;slot<2;++slot){CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::dynamicEq},source.layout)==RackEditResult::applied);
            const auto band=dynamicEqParameterStart+slot*dynamicEqParametersPerSlot,detector=dynamicEqDetectorParameterStart+slot*dynamicEqDetectorParametersPerSlot;
            source.values[band]=1;source.values[band+7]=.25f;source.values[band+8]=1.f/3;source.values[band+9]=source.values[band+10]=0;
            source.values[detector+1]=1;source.values[detector+2]=1;
            for(const auto [field,hz]:std::array<std::pair<std::size_t,float>,2>{{{3,50.f},{4,500.f}}}){const auto& d=parameterRegistry()[detector+field];source.values[detector+field]=juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(hz);}
            source.values[detector]=float(slot);
        }
        juce::MemoryBlock wire;CHECK(encodeSoundState(source,wire)&&processor->restoreSoundState(wire.getData(),int(wire.getSize())));
        const auto disabled=processor->getBusesLayout();auto enabled=disabled;enabled.inputBuses.set(0,juce::AudioChannelSet::stereo());CHECK(processor->setBusesLayout(enabled));
        processor->setNonRealtime(higher);processor->prepareToPlay(rate,preparedBlock);
        RoutedPatch patch;CHECK(makeRoutedPatch(source,patch));RoutedSynthEngine reference;CHECK(reference.setPatch(patch)&&reference.prepare(rate,preparedBlock,higher?RenderQuality::higher:RenderQuality::realtime));
        CHECK(processor->getLatencySamples()==(higher?21:0));
        const auto monitor=processor->fxMonitorHandle();const auto token=monitor->subscribe(0);CHECK(token.has_value());
        juce::AudioBuffer<float> actual(2,65536),expected(2,65536);juce::MidiBuffer midi,chunkMidi;midi.ensureSize(256);chunkMidi.ensureSize(256);std::array<DynamicEq::Frame,512> external;
        bool exact=true,finite=true;float peak=0,movement=0;int position=0;
        for(int trial=0;trial<11;++trial){
            const bool connected=trial<3||trial>=6;CHECK(processor->setBusesLayout(connected?enabled:disabled));
            if(trial==8){rate=rate==48000?96000.:48000.;processor->prepareToPlay(rate,preparedBlock);CHECK(reference.prepare(rate,preparedBlock,higher?RenderQuality::higher:RenderQuality::realtime));}
            if(trial==9){for(std::size_t slot=0;slot<2;++slot){const auto index=dynamicEqDetectorParameterStart+slot*dynamicEqDetectorParametersPerSlot+2;source.values[index]=1.f/3;processor->control(index).setValueNotifyingHost(source.values[index]);}CHECK(makeRoutedPatch(source,patch)&&reference.setPatch(patch));}
            if(trial==10){processor->releaseResources();processor->prepareToPlay(rate,preparedBlock);reference.reset();}
            const int count=std::array<int,11>{1,17,64,257,1024,65536,33,511,512,4096,1024}[std::size_t(trial)];
            const auto sample=[&](int n,int ch){return float(.5*std::sin(2*juce::MathConstants<double>::pi*100*(position+n)/rate)*(ch?-.7:1));};
            actual.setSize(2,count,false,false,true);expected.setSize(2,count,false,false,true);
            for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)actual.setSample(ch,n,sample(n,ch));
            midi.clear();if(trial==0||trial==8||trial==10)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
            processor->processBlock(actual,midi);
            for(int offset=0;offset<count;offset+=preparedBlock){const int size=std::min(preparedBlock,count-offset);chunkMidi.clear();
                for(const auto event:midi)if(event.samplePosition>=offset&&event.samplePosition<offset+size)chunkMidi.addEvent(event.data,event.numBytes,event.samplePosition-offset);
                for(int n=0;n<size;++n)external[std::size_t(n)]={sample(offset+n,0),sample(offset+n,1)};
                std::array<float*,2> channels{expected.getWritePointer(0,offset),expected.getWritePointer(1,offset)};juce::AudioBuffer<float> view(channels.data(),2,size);
                reference.render(view,chunkMidi,{},connected?std::span<const DynamicEq::Frame>(external).first(std::size_t(size)):std::span<const DynamicEq::Frame>{});
            }
            for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n){const auto x=actual.getSample(ch,n);exact=exact&&std::bit_cast<std::uint32_t>(x)==std::bit_cast<std::uint32_t>(expected.getSample(ch,n));finite=finite&&std::isfinite(x);peak=std::max(peak,std::abs(x));}
            FxMonitor::Packet packet;while(token&&monitor->read(*token,packet))movement=std::min(movement,packet.movementDb[0]);
            CHECK(snapshot(*processor)==source);position+=count;
        }
        std::cout<<"EXTERNAL_PRODUCT higher="<<higher<<" prepared="<<preparedBlock<<" final_rate="<<rate<<" exact="<<exact<<" finite="<<finite<<" peak="<<peak<<" minimum_movement="<<movement<<'\n';
        CHECK(exact&&finite&&peak>.001f&&movement<-1);CHECK(reference.dspWorkerStats().prepared==3);processor->releaseResources();
    }
}
void detectorSegmentsUseTheirRegisteredChoices()
{
    std::cout<<"SECTION detector_four_source_choices_and_undo\n";const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    companion::ClientOptions offline;offline.allowLaunch=false;
    InstrumentProcessor p(offline);SoundDocument source;CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,7,masterChain,0,EffectKind::dynamicEq},source.layout)==RackEditResult::applied);
    studio::AllControlsPanel panel(p,*model,{});panel.show({{NativeModule::dynamicEq,7}});const auto index=dynamicEqDetectorParameterStart+7*40+2;CHECK(panel.showParameter(index));auto* knob=panel.knob(index);CHECK(knob);if(!knob)return;CHECK(knob->steps()==3);
    CHECK(knob->getPresentation()==studio::ProgrammerKnob::Presentation::choice);
    const std::array<int,4> choices{1,3,0,2};
    for(std::size_t n=0;n<4;++n){auto before=source;before.values[index]=float(choices[n]==3?2:choices[n]+1)/3;juce::MemoryBlock bytes;CHECK(encodeSoundState(before,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));panel.refresh(before);knob->reread();Notifications events(p.control(index));
        CHECK(knob->keyPressed(juce::KeyPress(choices[n]==3?juce::KeyPress::upKey:juce::KeyPress::downKey)));
        auto expected=before;expected.values[index]=float(choices[n])/3;CHECK(snapshot(p)==expected);CHECK(events.starts==1&&events.ends==1);CHECK(p.undoSoundEdit());CHECK(snapshot(p)==before);
    }
    panel.addToDesktop(0);panel.setVisible(true);
    CHECK(knob->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    if(auto* menu=juce::Component::getCurrentlyModalComponent()){
        CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        for(int n=0;n<50&&juce::Component::getCurrentlyModalComponent();++n)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }else CHECK(false);
    juce::PopupMenu::dismissAllActiveMenus();panel.removeFromDesktop();
}
void detectorGestureLocksAndReentrantChanges()
{
    std::cout<<"SECTION detector_manual_gestures_all_fields_slot_locks_and_reentrant_lock\n";
    companion::ClientOptions offline;offline.allowLaunch=false;
    InstrumentProcessor p(offline);SoundDocument source;
    for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(source.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::dynamicEq},source.layout)==RackEditResult::applied);
    const auto install=[&](const SoundDocument& d){juce::MemoryBlock bytes;CHECK(encodeSoundState(d,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));};
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t field=0;field<5;++field){const auto index=dynamicEqDetectorParameterStart+slot*40+7*5+field;Notifications events(p.control(index));
        for(bool rack:{false,true}){auto locked=source;if(rack)locked.effectLocks[slot]=true;else locked.controlLocks[index]=true;install(locked);CHECK(!p.beginParameterGesture(index));CHECK(snapshot(p)==locked);CHECK(events.starts==0&&events.ends==0);}
        install(source);CHECK(p.beginParameterGesture(index));CHECK(p.setParameterGestureValue(index,field<2?1.f:field==2?1.f/3:.5f));CHECK(p.endParameterGesture(index));CHECK(events.starts==1&&events.ends==1);CHECK(snapshot(p)!=source);CHECK(p.undoSoundEdit());CHECK(snapshot(p)==source);
    }
    install(source);const auto index=dynamicEqDetectorParameterStart+7*40;Notifications events(p.control(index));
    events.onStart=[&]{auto locked=snapshot(p);locked.effectLocks[7]=true;std::uint64_t revision;SoundDocument current;CHECK(p.trySoundSnapshot(current,revision));CHECK(!p.applySoundEdit(locked,SoundEditOrigin::manual,revision));CHECK(snapshot(p)==current);install(locked);};
    CHECK(p.beginParameterGesture(index));const auto locked=snapshot(p);CHECK(!p.setParameterGestureValue(index,1));CHECK(p.endParameterGesture(index));CHECK(snapshot(p)==locked);CHECK(events.starts==1&&events.ends==1);
}
}
void spectralPreparationLifecycle()
{
    std::cout<<"SECTION spectral_requested_effective_state_and_restart_latency\n";
    companion::ClientOptions offline;offline.allowLaunch=false;
    offline.launch.fixtureRoot=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("spectral-processor");
    offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");
    InstrumentProcessor p(offline);
    SoundDocument sound;std::uint64_t revision;CHECK(p.trySoundSnapshot(sound,revision));
    RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.destination=masterChain;add.effect=EffectKind::dynamicEq;
    CHECK(editRackLayout(sound.layout,add,sound.layout)==RackEditResult::applied);
    const auto mode=dynamicEqSpectralParameterStart+7*dynamicEqSpectralParametersPerSlot;
    sound.values[mode]=1;sound.values[mode+1]=1;
    CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,revision));
    CHECK(!p.control(mode).isAutomatable());CHECK(p.control(mode+1).isAutomatable());
    CHECK(p.eqPreparationPending(7));
    p.prepareToPlay(48000,512);CHECK(p.getLatencySamples()==5120);
    CHECK(p.effectiveEqMode(7)==DynamicEqProcessingMode::spectral&&!p.eqPreparationPending(7));
    juce::MemoryBlock spectralState;p.getStateInformation(spectralState);
    CHECK(p.trySoundSnapshot(sound,revision));sound.values[mode]=0;
    CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,revision));
    juce::AudioBuffer<float> audio(2,139);juce::MidiBuffer midi;p.processBlock(audio,midi);
    CHECK(p.getLatencySamples()==5120&&p.effectiveEqMode(7)==DynamicEqProcessingMode::spectral&&p.eqPreparationPending(7));
    p.releaseResources();p.prepareToPlay(48000,512);
    CHECK(p.getLatencySamples()==0&&p.effectiveEqMode(7)==DynamicEqProcessingMode::ordinary&&!p.eqPreparationPending(7));
    CHECK(p.restoreSoundState(spectralState.getData(),int(spectralState.getSize())));p.processBlock(audio,midi);
    CHECK(p.getLatencySamples()==0&&p.eqPreparationPending(7));
    p.releaseResources();p.prepareToPlay(96000,512);
    CHECK(p.getLatencySamples()==10240&&p.effectiveEqMode(7)==DynamicEqProcessingMode::spectral&&!p.eqPreparationPending(7));
    CHECK(p.trySoundSnapshot(sound,revision));sound.values[758]=1;CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,revision));
    p.releaseResources();p.setNonRealtime(true);p.prepareToPlay(96000,512);
    CHECK(p.getLatencySamples()==10261&&p.effectiveRenderQuality()==RenderQuality::higher);
    CHECK(p.getTailLengthSeconds()>=double(p.getLatencySamples())/96000);
    p.releaseResources();
}
void spectralDrawerBindings()
{
    std::cout<<"SECTION spectral_drawer_all_manual_controls_and_digital_layout\n";
    companion::ClientOptions offline;offline.allowLaunch=false;InstrumentProcessor p(offline);
    const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
    studio::AllControlsPanel panel(p,*model,{});panel.setSize(1485,601);panel.show({{NativeModule::dynamicEq,7}});
    const auto base=dynamicEqSpectralParameterStart+7*dynamicEqSpectralParametersPerSlot;
    CHECK(panel.parameters()==unitReach(NativeModule::dynamicEq,7));
    CHECK(panel.showParameter(base+1+7*3));CHECK(panel.spectralPage());
    const auto knobs=panel.shownKnobs();CHECK(knobs.size()==4);
    for(const auto index:{base,base+22,base+23,base+24})CHECK(panel.knob(index)!=nullptr);
    if(auto* density=panel.knob(base+23))CHECK(density->getPresentation()==studio::ProgrammerKnob::Presentation::horizontal);
}
int main(int argc,char** argv)
{
    if(argc==2&&juce::String(argv[1])=="--extension-selectors"){juce::ScopedJuceInitialiser_GUI gui;manualExtensionSelectors();extensionSelectorGuardsAndLifetime();std::cout<<"EQ extension manual: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    juce::ScopedJuceInitialiser_GUI juce;
    if(argc==2&&juce::String(argv[1])=="--host-state"){hostStateAndGestures();std::cout<<"EQ host state: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--spectral-drawer"){spectralDrawerBindings();std::cout<<"Spectral drawer checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--spectral-lifecycle"){spectralPreparationLifecycle();std::cout<<"Spectral processor checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--detector-segments"){detectorSegmentsUseTheirRegisteredChoices();std::cout<<"EQ detector segments: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--detector-locks"){detectorGestureLocksAndReentrantChanges();std::cout<<"EQ detector locks: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--detector-audio"){externalDetectorProductAudio();std::cout<<"EQ external processor: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--sidechain-bus"){optionalAuxBusHasAnExplicitDisabledDefault();std::cout<<"EQ sidechain bus: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--raw-snapshot"){rawHostParameterSnapshotIsExact();std::cout<<"Raw host snapshot: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--manual"){manualRetention();std::cout<<"EQ placement manual: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--placement"){repeatedSoundAndLiveAutomationAudio(true);std::cout<<"EQ placement processor: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc!=1)return 2;
    spectralPreparationLifecycle();spectralDrawerBindings();detectorSegmentsUseTheirRegisteredChoices();detectorGestureLocksAndReentrantChanges();externalDetectorProductAudio();
    optionalAuxBusHasAnExplicitDisabledDefault();rawHostParameterSnapshotIsExact();hostStateAndGestures();manualRetention();manualExtensionSelectors();extensionSelectorGuardsAndLifetime();monitoringLifetime();guardedBandCreation();guardedWholeEqGroup();guardedGroupUndoKeepsUnrelatedAutomation();repeatedSoundAndLiveAutomationAudio();repeatedSoundAndLiveAutomationAudio(true);std::cout<<"Dynamic EQ host: "<<checks<<" checks, "<<failures<<" failures\n";return failures==0?0:1;
}
