#include "FrozenRackExamples.h"
#include "InstrumentProcessor.h"
#include "FxMonitor.h"
#include "DspPipeline.h"
#include "SpectralEq.h"
#include "DynamicEqGraph.h"
#include "EqMatchPanel.h"
#include "FxAnalysisClock.h"
#include "EqOverviewPeers.h"
#include "RackExamples.h"
#include "UserReach.h"
#include <algorithm>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <chrono>
#include <thread>
#include <atomic>
#include <numeric>
#include <stdexcept>
#if defined(_WIN32)
#include <Windows.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace airack;
int checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<'\n';}}while(false)
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool counting=false;std::atomic<std::size_t> allocations{0},frees{0};
int heapHook(int operation,void*,std::size_t,int type,long,const unsigned char*,int){if((counting||detail::DspPipeline::renderingOnThisThread())&&type!=_CRT_BLOCK){if(operation==_HOOK_FREE)++frees;else ++allocations;}return 1;}
#endif
constexpr double tone=987.7666025122483;
DynamicEqSettings extreme(bool dynamic=false){
 DynamicEqSettings s;s.outputGainDb=12;
 for(auto& b:s.bands){b.active=true;b.frequencyHz=float(tone);b.q=.25f;b.gainDb=dynamic?0:24;b.rangeDb=dynamic?24:0;b.thresholdDb=-60;b.attackMs=.1f;b.releaseMs=10;}
 return s;
}
RoutedPatch serialPatch(bool protection,bool dynamic=false){
 RoutedPatch patch;patch.outputProtection=protection;patch.outputGainDb=36;
 for(std::uint8_t i=0;i<8;++i){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=i;add.effect=EffectKind::dynamicEq;add.position=i;add.destination=masterChain;CHECK(editRackLayout(patch.layout,add,patch.layout)==RackEditResult::applied);patch.effects[i].dynamicEq=extreme(dynamic);}
 return patch;
}
void downsamplerBound(){
 const auto down=juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod(.08f,-100.f);const auto count=down->getFilterOrder()+1;const auto* taps=down->getRawCoefficients();double full=0,operational=std::abs(double(taps[count/2]));
 for(std::size_t n=0;n<count;++n)full+=std::abs(double(taps[n]));for(std::size_t n=0;n<count/2;n+=2)operational+=2*std::abs(double(taps[n]));
 CHECK(std::abs(full-operational)<1e-6);CHECK(std::isfinite(full)&&full>0);std::cout<<"FIR_BOUND taps="<<count<<" full_l1="<<full<<" operational_l1="<<operational<<" internal_float_limit="<<std::numeric_limits<float>::max()/(4*std::max(1.,operational))<<'\n';
}
void doubleHeadroom(){
 std::cout<<"SECTION serial64_double_core_headroom\n";std::array<DynamicEq,8> cores;const auto settings=extreme();
 for(auto& core:cores)CHECK(core.setSettings(settings)&&core.prepare(48000));
 double peak=0;bool finite=true;
 for(int n=0;n<48000;++n){DynamicEq::Frame x{.0001*std::sin(2*std::numbers::pi*tone*n/48000),0};for(auto& core:cores)x=core.process(x);finite=finite&&std::isfinite(x[0])&&x[1]==0;peak=std::max(peak,std::abs(x[0]));}
 std::cout<<"DOUBLE_SERIAL peak="<<peak<<" float_max="<<std::numeric_limits<float>::max()<<" finite="<<finite<<'\n';CHECK(finite);CHECK(peak>std::numeric_limits<float>::max());
}
void hostFloatHeadroom(){
 std::cout<<"SECTION actual_routed_float64bands_both_qualities_protection_and_recovery\n";
 for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher})for(bool protectedOutput:{false,true})for(bool dynamic:{false,true}){
  RoutedSynthEngine engine;auto patch=serialPatch(protectedOutput,dynamic);CHECK(engine.setPatch(patch));CHECK(engine.prepare(rate,512,quality));engine.reset();
  juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,83,.8f),0);
  std::size_t nonfinite=0;int first=-1;double peak=0;
#if defined(_MSC_VER) && defined(_DEBUG)
  const auto previous=_CrtSetAllocHook(heapHook);allocations=frees=0;
#endif
  for(int block=0;block<int(std::ceil(rate/512));++block){
#if defined(_MSC_VER) && defined(_DEBUG)
   counting=true;
#endif
   engine.render(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
   counting=false;
#endif
   midi.clear();for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n){const auto y=audio.getSample(ch,n);if(!std::isfinite(y)){++nonfinite;if(first<0)first=block*512+n;}else peak=std::max(peak,std::abs(double(y)));}
  }
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);
#endif
  std::cout<<"HOST_SERIAL rate="<<rate<<" quality="<<int(quality)<<" protection="<<protectedOutput<<" dynamic="<<dynamic<<" nonfinite="<<nonfinite<<" first="<<first<<" finite_peak="<<peak<<'\n';CHECK(nonfinite==0);if(protectedOutput)CHECK(peak<=std::pow(10.,-1./20)+2e-6);
  // A real Stop/reset must clear overload history before a fresh ordinary note.
  for(auto& effect:patch.effects)effect.dynamicEq={};patch.outputGainDb=-12;CHECK(engine.setPatch(patch));engine.reset();
  midi.addEvent(juce::MidiMessage::midiStop(),0);engine.render(audio,midi);midi.clear();midi.addEvent(juce::MidiMessage::noteOn(1,83,.8f),0);double energy=0;bool recovered=true;
  for(int block=0;block<16;++block){engine.render(audio,midi);midi.clear();for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n){const auto y=audio.getSample(ch,n);recovered=recovered&&std::isfinite(y);energy+=double(y)*y;}}
  CHECK(recovered&&energy>1e-12);std::cout<<"RECOVERY energy="<<energy<<" finite="<<recovered<<'\n';
 }
}
void shelfHeadroom(){
 std::cout<<"SECTION additional_positive_shelf_host_float_headroom\n";
 for(auto shape:{DynamicEqShape::lowShelf,DynamicEqShape::highShelf}){RoutedSynthEngine engine;auto patch=serialPatch(true);for(auto& effect:patch.effects)for(auto& band:effect.dynamicEq.bands){band.shape=shape;band.frequencyHz=shape==DynamicEqShape::lowShelf?20000.f:20.f;}
  CHECK(engine.setPatch(patch)&&engine.prepare(48000,512,RenderQuality::higher));engine.reset();juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,83,.8f),0);bool finite=true;double peak=0;
  for(int b=0;b<8;++b){engine.render(audio,midi);midi.clear();for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n){const auto y=audio.getSample(ch,n);finite=finite&&std::isfinite(y);peak=std::max(peak,std::abs(double(y)));}}
  CHECK(finite&&peak<=std::pow(10.,-1./20)+2e-6);std::cout<<"SHELF shape="<<int(shape)<<" finite="<<finite<<" peak="<<peak<<'\n';
 }
}

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void set(SoundDocument& d,const juce::String& id,float physical){
 const auto& registry=parameterRegistry();const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return p.id==id;});require(found!=registry.end(),"unknown workload field");
 require(physical>=found->minimum&&physical<=found->maximum,"workload value outside range");d.values[std::size_t(found-registry.begin())]=juce::NormalisableRange<float>(found->minimum,found->maximum,0,found->skew).convertTo0to1(physical);
}
void eqValue(SoundDocument& d,std::size_t slot,std::size_t band,std::size_t field,float physical){const auto i=dynamicEqParameterStart+slot*dynamicEqParametersPerSlot+band*11+field;const auto& p=parameterRegistry()[i];require(physical>=p.minimum&&physical<=p.maximum,"EQ workload value outside range");d.values[i]=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(physical);}
SoundDocument capacityScene(){
 SoundDocument d;for(std::uint8_t i=1;i<4;++i)require(editRackLayout(d.layout,{RackEditKind::addSynth,i,masterChain,i},d.layout)==RackEditResult::applied,"add synth");
 for(auto& banks:d.compatibility.synth)banks.fill(modernSynthAlgorithmRevision);
 set(d,"global.voice_mode",0);set(d,"global.master_level",.5f);set(d,"output.gain_db",-12);set(d,"output.protection",1);
 for(int i=1;i<=4;++i){const auto p="synth."+juce::String(i);set(d,p+".enabled",1);set(d,p+".engine",i%2?0.f:1.f);set(d,p+".level",.5f);
  for(const auto* engine:{".analog.",".wavetable."}){const auto b=p+engine;set(d,b+"amp.attack",.005f);set(d,b+"amp.decay",.15f);set(d,b+"amp.sustain",1);set(d,b+"filter.cutoff",4000);set(d,b+"filter.resonance",.25f);}
 }
 for(std::uint8_t slot=0;slot<8;++slot){require(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::dynamicEq},d.layout)==RackEditResult::applied,"add EQ");
  for(std::size_t b=0;b<8;++b){eqValue(d,slot,b,0,1);eqValue(d,slot,b,3,1000);eqValue(d,slot,b,5,1);eqValue(d,slot,b,7,b%2?12.f:-12.f);eqValue(d,slot,b,8,b%2?-43.f:-40.f);eqValue(d,slot,b,9,10);eqValue(d,slot,b,10,100);}
 }
 require(validateSoundDocument(d),"invalid capacity scene");return d;
}
SoundDocument placementCapacityScene(int mode){
 require(mode>=0&&mode<3,"unknown placement workload");auto source=capacityScene();
 // Different engines on opposite sides supply both Mid and Side at full voice count.
 for(int layer=1;layer<=4;++layer)set(source,"synth."+juce::String(layer)+".pan",layer%2?-.75f:.75f);
 for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band)
  source.values[dynamicEqPlacementParameterStart+slot*8+band]=float(mode==0?3:mode==1?4:(slot*8+band)%5)/4;
 require(validateSoundDocument(source),"invalid placement capacity scene");return source;
}
void placementWorkloadAdmission(){
 std::cout<<"SECTION placement_capacity_has_64_active_component_bands_and_stereo_source\n";
 for(int mode=0;mode<3;++mode){const auto sound=placementCapacityScene(mode);RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
  CHECK(patch.synth.layers[0].pan<0&&patch.synth.layers[1].pan>0);
  int active=0;for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto& b=patch.effects[slot].dynamicEq.bands[band];active+=b.active;
   const auto expected=mode==0?DynamicEqPlacement::mid:mode==1?DynamicEqPlacement::side:DynamicEqPlacement((slot*8+band)%5);
   CHECK(b.placement==expected);CHECK(b.rangeDb!=0&&b.active&&!b.bypassed);
  }CHECK(active==64);CHECK(sound.layout==capacityScene().layout);
 }
}
const char* shapeSceneName(int mode){return std::array<const char*,3>{"cuts96-512sections","flat-tilt-384sections","mixed-264sections"}[std::size_t(mode)];}
SoundDocument shapeCapacityScene(int mode){
 require(mode>=0&&mode<3,"unknown shape workload");auto source=capacityScene();
 for(int layer=1;layer<=4;++layer)set(source,"synth."+juce::String(layer)+".pan",layer%2?-.75f:.75f);
 for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){
  const auto extra=dynamicEqExtensionParameterStart+slot*16+band*2;
  eqValue(source,slot,band,5,.7071f);eqValue(source,slot,band,4,band%2?6.f:-6.f);
  if(mode==0){eqValue(source,slot,band,2,4);eqValue(source,slot,band,3,8000);source.values[extra+1]=1;}
  if(mode==1)source.values[extra]=1;
  if(mode==2){
   if(band<3)source.values[extra]=float(band+1)/3;
   else{eqValue(source,slot,band,2,band==4||band==6?4.f:3.f);source.values[extra+1]=float(band==3?1:band==4?2:band==5?3:4)/4;}
   eqValue(source,slot,band,3,band<3?1500.f:band==4||band==6?16000.f:30.f);
   source.values[dynamicEqPlacementParameterStart+slot*8+band]=float((slot*8+band)%5)/4;
  }
 }
 require(validateSoundDocument(source),"invalid shape workload");return source;
}
void shapeWorkloadAdmission(){
 std::cout<<"SECTION current_product_shapes_64_static_bands_512_384_264_sections\n";
 for(int mode=0;mode<3;++mode){const auto source=shapeCapacityScene(mode);RoutedPatch patch;CHECK(makeRoutedPatch(source,patch));
  CHECK(patch.synth.layers[0].pan<0&&patch.synth.layers[1].pan>0);int bands=0,sections=0;
  for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto& b=patch.effects[slot].dynamicEq.bands[band];
   CHECK(b.active&&!b.bypassed&&b.rangeDb!=0);++bands;
   const auto expectedShape=mode==0?DynamicEqShape::highCut:mode==1?DynamicEqShape::flatTilt:
    std::array<DynamicEqShape,8>{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt,DynamicEqShape::lowCut,DynamicEqShape::highCut,DynamicEqShape::lowCut,DynamicEqShape::highCut,DynamicEqShape::lowCut}[band];
   const auto expectedSlope=mode==0?DynamicEqSlope::db96:mode==1||band<3?DynamicEqSlope::db12:
    std::array<DynamicEqSlope,8>{DynamicEqSlope::db12,DynamicEqSlope::db12,DynamicEqSlope::db12,DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96,DynamicEqSlope::db96}[band];
   CHECK(b.shape==expectedShape);CHECK(b.slope==expectedSlope);CHECK(b.placement==(mode==2?DynamicEqPlacement((slot*8+band)%5):DynamicEqPlacement::stereo));
   sections+=mode==0?8:mode==1?6:std::array<int,8>{1,2,6,1,3,4,8,8}[band];
  }
  CHECK(bands==64);CHECK(sections==(mode==0?512:mode==1?384:264));
 }
}
SoundDocument gainCapacityScene(){
 auto source=capacityScene();
 for(std::size_t slot=0;slot<8;++slot){
  set(source,"effect."+juce::String(int(slot+1))+".dynamic_eq.auto_gain",1);
  set(source,"effect."+juce::String(int(slot+1))+".dynamic_eq.gain_scale",1.5f);
  for(std::size_t band=0;band<8;++band)eqValue(source,slot,band,4,band==0?.15625f:band%2?-.125f:.125f);
 }
 require(validateSoundDocument(source),"invalid enabled gain scene");return source;
}
void gainWorkloadAdmission(){
 std::cout<<"SECTION enabled_gain_workload_has_nonzero_static_gains_and_compensation\n";
 const auto sound=gainCapacityScene();RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
 for(const auto& fx:patch.effects){CHECK(fx.dynamicEq.autoGain&&fx.dynamicEq.gainScale!=1);CHECK(std::abs(dynamicEqAutoGainDb(fx.dynamicEq))>.0001f);
  for(const auto& band:fx.dynamicEq.bands)CHECK(band.active&&band.gainDb!=0);
 }
}
SoundDocument sketchCapacityScene(bool wide){
 auto source=capacityScene();
 for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band)
  if(wide?slot==0:band==7)eqValue(source,slot,band,0,0);
 return source;
}
void sketchWorkloadAdmission(){
 std::cout<<"SECTION sketch_workloads_reserve_bands_without_changing_old_scene\n";
 const auto original=capacityScene();
 for(const bool wide:{false,true}){
  const auto source=sketchCapacityScene(wide);CHECK(validateSoundDocument(source));
  RoutedPatch oldPatch,patch;CHECK(makeRoutedPatch(original,oldPatch));CHECK(makeRoutedPatch(source,patch));
  int oldActive=0,active=0,firstFree=0;std::bitset<parameterCount> changed;
  for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){
   oldActive+=oldPatch.effects[slot].dynamicEq.bands[band].active?1:0;active+=patch.effects[slot].dynamicEq.bands[band].active?1:0;
   if(slot==0&&!patch.effects[slot].dynamicEq.bands[band].active)++firstFree;
   if(wide?slot==0:band==7)changed.set(dynamicEqParameterStart+slot*dynamicEqParametersPerSlot+band*dynamicEqParametersPerBand);
  }
  CHECK(oldActive==64);CHECK(active==56);CHECK(firstFree==(wide?8:1));
  CHECK(source.layout==original.layout);CHECK(source.macros==original.macros);CHECK(source.effectLocks==original.effectLocks);CHECK(source.controlLocks==original.controlLocks);
  for(std::size_t i=0;i<parameterCount;++i)CHECK(source.values[i]==(changed[i]?0.f:original.values[i]));
 }
}
SoundDocument matchCapacityScene(bool wide){auto source=sketchCapacityScene(wide);set(source,"synth.1.analog.noise",.8f);set(source,"synth.1.analog.filter.cutoff",20000);return source;}
void matchWorkloadAdmission(){
 std::cout<<"SECTION match_capacity_preserves64voices_and56moving_bands\n";
 for(bool wide:{false,true}){const auto source=matchCapacityScene(wide),original=capacityScene(),reserved=sketchCapacityScene(wide);RoutedPatch patch;CHECK(makeRoutedPatch(source,patch));int active=0,firstFree=0;
  for(std::size_t slot=0;slot<8;++slot)for(const auto& b:patch.effects[slot].dynamicEq.bands){active+=b.active;if(slot==0)firstFree+=!b.active;CHECK(b.rangeDb!=0);}
  CHECK(active==56&&firstFree==(wide?8:1));CHECK(source.layout==original.layout&&source.macros==original.macros&&source.effectLocks==original.effectLocks&&source.controlLocks==original.controlLocks);
  for(std::size_t i=0;i<parameterCount;++i)if(parameterRegistry()[i].id!="synth.1.analog.noise"&&parameterRegistry()[i].id!="synth.1.analog.filter.cutoff")CHECK(source.values[i]==reserved.values[i]);
 }
}
juce::var object(){return juce::var(new juce::DynamicObject);}
void put(juce::var& v,const char* key,const juce::var& value){v.getDynamicObject()->setProperty(key,value);}
juce::File freshDirectory(const char* path){const juce::File directory{juce::String(path)};const auto workspace=juce::File::getCurrentWorkingDirectory();require(juce::File::isAbsolutePath(path)&&directory.isAChildOf(workspace)&&!directory.exists(),"output must be a fresh absolute directory in this worktree");require(directory.createDirectory().wasOk(),"cannot create output");return directory;}
enum class GroupProfile {none,selection,drag};
enum class SketchProfile {none,ready,redraw,stress};
enum class MatchProfile {none,closed,learning,ready,refit,file};
struct MatchCapacity {
 using Clock=std::chrono::steady_clock;
 InstrumentProcessor& processor;studio::FxAnalysisClock& analysis;studio::DynamicEqGraph& graph;MatchProfile mode;
 studio::EqMatchPanel panel;std::optional<studio::FxAnalysisHub::Lease> probe;juce::File file;
 SoundDocument before;int queued=0,completed=0,activityTicks=0;bool outstanding=false,healthy=true;juce::String failure;double maximumQueueMs=0;Clock::time_point next{};
 MatchCapacity(InstrumentProcessor& p,studio::FxAnalysisClock& a,studio::DynamicEqGraph& g,juce::Component& window,const SoundDocument& sound,MatchProfile m,int rate,int block,juce::MidiBuffer& warmMidi,const juce::File& root)
  :processor(p),analysis(a),graph(g),mode(m),panel(p,*studio::studioSkin(),a.hub),file(root.getChildFile("maximum-reference.wav")),before(sound){
  window.addAndMakeVisible(panel);panel.setBounds(0,0,1080,526);panel.bind(graph);panel.toFront(false);probe=analysis.hub->acquire(0);require(bool(probe),"Match capacity observer");
  require(panel.learn(false,false),"Match capacity explicit Input learn");juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(4096);
  for(int k=0;k<16;++k)midi.addEvent(juce::MidiMessage::noteOn(1,48+k,juce::uint8(100)),0);
  const int hop=std::max(1,int(std::ceil(double(rate)/block/30))),count=int(std::ceil(2.6*rate/block));
  for(int k=0;k<count;++k){processor.processBlock(audio,midi);midi.clear();if(k%hop==hop-1){analysis.hub->update();panel.updateMonitoring();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}}
  analysis.hub->update();panel.updateMonitoring();require(panel.stopLearning()&&panel.sourceReference(),"Match capacity continuous source");
  auto reference=*panel.sourceReference();reference.label="Capacity reference from learned Input";RoutedPatch patch;require(makeRoutedPatch(sound,patch),"Match capacity patch");const auto base=patch.effects[0].dynamicEq;auto desired=base;
  std::size_t free=0,ordinal=0;for(const auto& band:base.bands)free+=!band.active;
  for(auto& band:desired.bands)if(!band.active){band={};band.active=true;band.frequencyHz=free==1?700.f:std::array<float,8>{35,80,200,500,1200,3000,7000,15000}[ordinal];band.gainDb=free==1?6.f:(ordinal%2?-9.f:9.f);band.q=free==1?.7071f:2.f;++ordinal;}
  for(std::size_t i=0;i<129;++i){const auto hz=20*std::pow(1000.,double(i)/128);const auto response=dynamicEqEnergyResponse(desired,reference.sampleRate,hz);require(bool(response),"Match capacity reference response");reference.spectrum.db[i]+=float(20*std::log10(std::max(*response,1e-12)));}
  require(panel.setReference(reference),"Match capacity reference snapshot");
  // Every paired case performs this same acquisition; restart exactly64 voices for the normal warm/measured phases.
  warmMidi.clear();warmMidi.addEvent(juce::MidiMessage::allSoundOff(1),0);for(int k=0;k<16;++k)warmMidi.addEvent(juce::MidiMessage::noteOn(1,48+k,juce::uint8(100)),0);
  if(mode==MatchProfile::closed){panel.setVisible(false);analysis.hub->release(*probe);probe.reset();}
 }
 ~MatchCapacity(){panel.retire();if(probe)analysis.hub->release(*probe);}
 const studio::FxSpectrum::Result& spectrum() const {if(probe)if(const auto* r=analysis.hub->result(*probe))return *r;return graph.spectrum();}
 void observe(){panel.updateMonitoring();if(outstanding){const auto fit=graph.matchPreview();const bool done=mode==MatchProfile::file?!panel.loadingReference():!fit.fitting;
   if(done){const bool valid=mode==MatchProfile::file?panel.reference()&&panel.reference()->label==file.getFileName():fit.canApply;if(valid)++completed;else{healthy=false;failure="Completion: "+panel.statusText();}outstanding=false;}}
  if(mode==MatchProfile::file&&panel.loadingReference())++activityTicks;
 }
 void queue(){const auto start=Clock::now();const bool ok=mode==MatchProfile::file?panel.loadReferenceFile(file):panel.preview(queued%2?.9f:1.f);maximumQueueMs=std::max(maximumQueueMs,std::chrono::duration<double,std::milli>(Clock::now()-start).count());if(ok){++queued;outstanding=true;}else{healthy=false;failure="Queue: "+panel.statusText();}}
 void wait(){const auto end=Clock::now()+std::chrono::seconds(30);while(outstanding&&Clock::now()<end){juce::MessageManager::getInstance()->runDispatchLoopUntil(5);observe();}if(outstanding){healthy=false;failure="Completion deadline";}}
 void begin(){if(mode==MatchProfile::learning){healthy=panel.learn(false,false);if(!healthy)failure=panel.statusText();}else if(mode==MatchProfile::ready||mode==MatchProfile::refit){queue();wait();}else if(mode==MatchProfile::file)queue();next=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(mode==MatchProfile::file?1./3:1./30));}
 void tick(){observe();if(!healthy||Clock::now()<next)return;next=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(mode==MatchProfile::file?1./3:1./30));
  if(mode==MatchProfile::refit)queue();else if(mode==MatchProfile::file&&!panel.loadingReference())queue();
 }
 juce::var finish(){observe();wait();SoundDocument after;std::uint64_t revision;const bool exact=processor.trySoundSnapshot(after,revision)&&after==before;
  const auto fit=graph.matchPreview();const bool admitted=mode==MatchProfile::closed||(mode==MatchProfile::learning?panel.learning()&&panel.learnedSeconds()>=1:mode==MatchProfile::file?queued>=1&&completed>=1&&activityTicks>0:fit.canApply&&queued>=(mode==MatchProfile::refit?3:1)&&completed>=(mode==MatchProfile::refit?2:1));
  auto report=object();put(report,"mode",mode==MatchProfile::closed?"closed":mode==MatchProfile::learning?"learning":mode==MatchProfile::ready?"ready":mode==MatchProfile::refit?"repeated_amount_fit_30Hz":"maximum_float64_WAV_analysis");put(report,"queued",queued);put(report,"completed",completed);put(report,"file_loading_ticks",activityTicks);put(report,"maximum_queue_ms",maximumQueueMs);put(report,"learned_seconds",panel.learnedSeconds());put(report,"sound_exact",exact);put(report,"failure",failure);put(report,"admitted",admitted&&exact&&healthy);
  put(report,"status",panel.statusText());put(report,"learning_active",panel.learning());put(report,"source_available",panel.sourceReference().has_value());
  panel.setVisible(false);panel.retire();if(probe){analysis.hub->release(*probe);probe.reset();}return report;
 }
};
struct FftWindowActivity {
 std::uint64_t previous=0,completed=0,resets=0;
 // A coherent-stream reset restarts the spectrum's window counter after dropped packets.
 void observe(std::uint64_t current){if(current<previous){++resets;completed+=current;}else completed+=current-previous;previous=current;}
};
void fftWindowActivityProof(){
 std::cout<<"SECTION measured_fft_activity_survives_coherent_stream_resets\n";
 FftWindowActivity count{5};count.observe(5);CHECK(count.completed==0);count.observe(8);CHECK(count.completed==3);count.observe(2);CHECK(count.completed==5&&count.resets==1);
 count.observe(2);CHECK(count.completed==5);count.observe(0);CHECK(count.completed==5&&count.resets==2);count.observe(1);CHECK(count.completed==6);count.observe(4);CHECK(count.completed==9);
}
juce::var profileCase(const SoundDocument& source,int rate,int block,bool higher,int displays,const juce::File& root,bool bypass=false,bool paced=true,bool grabPreview=false,GroupProfile group=GroupProfile::none,bool gainDrag=false,SketchProfile sketch=SketchProfile::none,bool verifyComponents=false,bool staticExtensions=false,bool externalDetector=false,int overviewMode=0,MatchProfile matchMode=MatchProfile::none,int spectralStages=0,int spectralWindow=4096,bool spreadSpectral=false,bool boundedSpectral=false,bool spectralDiagnostic=false,int routedSpectral=0){
 using Clock=std::chrono::steady_clock;
 const auto slotCount=std::size_t(std::count_if(source.layout.effects.begin(),source.layout.effects.end(),[](const auto& e){return e.use==SlotUse::present&&e.kind==EffectKind::dynamicEq;}));require(slotCount>0,"no profile EQs");
 require(overviewMode>=0&&overviewMode<=2,"unknown overview mode");require(overviewMode==0||(displays==8&&slotCount==8),"overview requires eight original graph owners");
 std::unique_ptr<EqOverviewPeers> peerPublishers;if(overviewMode==2)peerPublishers=std::make_unique<EqOverviewPeers>(root.getChildFile("peers-"+juce::Uuid().toString()));
 companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;if(peerPublishers)offline=peerPublishers->options();auto processor=std::make_unique<InstrumentProcessor>(offline);
 auto d=source;d.values[758]=higher?1.f:0.f;juce::MemoryBlock wire;require(encodeSoundState(d,wire)&&processor->restoreSoundState(wire.getData(),int(wire.getSize())),"profile recall");
 if(peerPublishers)require(peerPublishers->attach(*processor),"overview private peer admission");
 juce::MemoryBlock overviewState;if(overviewMode)processor->getStateInformation(overviewState);const auto aiSelection=processor->groupWorkflowView().selection;
 if(externalDetector){auto buses=processor->getBusesLayout();buses.inputBuses.set(0,juce::AudioChannelSet::stereo());require(processor->setBusesLayout(buses),"profile external input");}
 processor->setNonRealtime(higher);processor->prepareToPlay(rate,block);require(processor->effectiveRenderQuality()==(higher?RenderQuality::higher:RenderQuality::realtime),"profile quality");require(processor->getLatencySamples()==(higher?21:0)+routedSpectral*(rate/48000)*5120,"profile prepared routing latency");
 if(staticExtensions){RoutedPatch admitted;require(makeRoutedPatch(d,admitted),"static extension profile mapping");require(slotCount==8,"static extension eight racks");
  for(const auto& effect:admitted.effects)for(const auto& band:effect.dynamicEq.bands)require(band.active&&!band.bypassed&&band.rangeDb!=0&&band.shape>DynamicEqShape::highShelf,"static extension profile requires64 active static bands with retained range");}
 juce::Component window;window.setBounds(120,60,1180,640);window.addToDesktop(juce::ComponentPeer::windowIsTemporary);window.setVisible(true);
 studio::FxAnalysisClock analysis(processor->fxMonitorHandle());
 std::array<std::unique_ptr<studio::DynamicEqGraph>,8> graphs;for(std::size_t i=0;i<slotCount;++i){auto& graph=graphs[i];graph=std::make_unique<studio::DynamicEqGraph>(*processor,studio::house::Host{},studio::DynamicEqGraph::Clipboard{},analysis.hub);window.addAndMakeVisible(*graph);graph->setBounds(int(i%2)*1045,int(i/2)*274,1045,274);graph->setTransform(juce::AffineTransform::scale(.56f));graph->refresh(d,std::uint8_t(i));graph->setSpectrumOn(true);graph->updateMonitoring();require(bool(graph->subscription()),"warm graph subscription");}
 std::unique_ptr<MatchCapacity> match;const auto spectrum=[&](std::size_t i)->const studio::FxSpectrum::Result&{return match&&i==0?match->spectrum():graphs[i]->spectrum();};
 std::unique_ptr<studio::EqOverview> overview;double overviewMaximumUpdateMs=0;std::size_t overviewUpdates=0;
 if(overviewMode){overview=std::make_unique<studio::EqOverview>(*processor,*studio::studioSkin(),analysis.hub);window.addAndMakeVisible(*overview);overview->setBounds(0,0,1080,526);overview->refresh(d);require(overview->setReferenceSlot(7)&&overview->setComparisonSlot(0),"overview local selection");
  if(peerPublishers){require(overview->setGroupEnabled(true)&&overview->setPeerSelection(peerPublishers->members)&&overview->setComparisonPeer(peerPublishers->members[0]),"overview peer selection");
   require(peerPublishers->wait(*processor,[&]{overview->updateMonitoring();for(int i=0;i<4;++i){auto* row=overview->findChildWithID("eq.overview.peer."+juce::String(i));if(!row||!row->getTitle().contains("EQ8")||row->getTitle().containsIgnoreCase("unavailable"))return false;}return true;}),"overview four fresh native peers");}
 }
 analysis.onUpdated=[&]{for(auto& graph:graphs)if(graph)graph->updateMonitoring();if(match)match->observe();if(overview){const auto begin=Clock::now();overview->updateMonitoring();++overviewUpdates;overviewMaximumUpdateMs=std::max(overviewMaximumUpdateMs,std::chrono::duration<double,std::milli>(Clock::now()-begin).count());}};
 // Test-only finishing stages retain the entire ordinary product workload.
 // Repeat each host frame at the prepared internal factor; this is a conservative
 // CPU feasibility probe, not a production resampler or compensated routing claim.
 require(spectralStages==0||spectralStages==1||spectralStages==8,"spectral stage count");
 std::array<std::unique_ptr<SpectralEq>,8> spectral;std::array<DynamicEqSettings,8> spectralSettings;
 std::vector<DynamicEq::Frame> spectralAudio(std::size_t(spectralStages?block*(higher?2:1):0));
 std::size_t spectralBytes=0;int spectralDelay=0;std::array<float,64> spectralMovement{};
 for(int i=0;i<spectralStages;++i){auto& kernel=spectral[std::size_t(i)];kernel=std::make_unique<SpectralEq>();const int phase=spreadSpectral&&i>0?i*(spectral[0]->transformSize()/4)/spectralStages:0;require(kernel->prepare(rate*(higher?2.:1.),block*(higher?2:1),spectralWindow,phase,boundedSpectral?SpectralEqWork::bounded:SpectralEqWork::immediate),"spectral prepare");
  auto& settings=spectralSettings[std::size_t(i)];for(std::size_t band=0;band<8;++band){auto& b=settings.bands[band];b.active=b.spectralEnabled=true;b.frequencyHz=500;b.q=.25f;b.rangeDb=band%2?6.f:-6.f;b.thresholdDb=float(-60+3.0102999566*std::log2(4096./spectralWindow));b.spectralDensity=1;b.attackMs=10;b.releaseMs=100;}
  require(kernel->setSettings(settings),"spectral settings");spectralBytes+=kernel->preparedBytes();spectralDelay+=kernel->latencySamples()/(higher?2:1);
 }
 std::array<float,64> movement{};std::atomic<int> stage{0};std::atomic<bool> go{false};const int warmBlocks=int(std::ceil(double(rate)/block)),measuredBlocks=int(std::ceil(2.*rate/block));std::vector<double> ns(std::size_t(measuredBlocks),0.);std::vector<unsigned> cpuIds(std::size_t(measuredBlocks),0);double measuredWall=0;bool finite=true;double peak=0,energy=0;double midEnergy=0,sideEnergy=0;std::size_t callbackAllocations=0,callbackFrees=0;std::uint64_t checksum=14695981039346656037ull;RoutedSynthEngine::DspWorkerStats dspWorkers;RoutedSynthEngine::SpectralStats routedStats;
 std::vector<double> ordinaryNs(spectralDiagnostic?std::size_t(measuredBlocks):0);
 juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(4096);for(int k=0;k<16;++k)midi.addEvent(juce::MidiMessage::noteOn(1,48+k,juce::uint8(100)),0);
 if(matchMode!=MatchProfile::none)match=std::make_unique<MatchCapacity>(*processor,analysis,*graphs[0],window,d,matchMode,rate,block,midi,root);
#if defined(_MSC_VER) && defined(_DEBUG)
 const auto previous=_CrtSetAllocHook(heapHook);
#endif
 std::thread render([&]{
#if defined(_MSC_VER) && defined(_DEBUG)
  allocations=frees=0;
#endif
  std::uint64_t triggerFrame=0;
  for(int phase=0;phase<2;++phase){if(phase){stage=1;while(!go.load())std::this_thread::yield();}const auto start=Clock::now();const int count=phase?measuredBlocks:warmBlocks;
   for(int k=0;k<count;++k){
    if(externalDetector)for(int n=0;n<block;++n,++triggerFrame){const double t=double(triggerFrame)/rate;const float value=std::fmod(t,.08)<.01?float(.0085*std::sin(2*std::numbers::pi*1000*t)):0.f;audio.setSample(0,n,value);audio.setSample(1,n,-.8f*value);}
#if defined(_MSC_VER) && defined(_DEBUG)
    counting=true;
#endif
    const auto before=Clock::now();processor->processBlock(audio,midi);
    const auto split=spectralDiagnostic?Clock::now():before;
    if(spectralStages){const int factor=higher?2:1;for(int n=0;n<block;++n)for(int sub=0;sub<factor;++sub)spectralAudio[std::size_t(n*factor+sub)]={audio.getSample(0,n),audio.getSample(1,n)};
     for(int i=0;i<spectralStages;++i){auto& kernel=*spectral[std::size_t(i)];kernel.processBlock(spectralAudio);const auto observed=kernel.dynamicGainsDb();for(std::size_t band=0;band<8;++band)spectralMovement[std::size_t(i)*8+band]=std::max(spectralMovement[std::size_t(i)*8+band],observed[band]);}
     for(int n=0;n<block;++n)for(int ch=0;ch<2;++ch)audio.setSample(ch,n,float(spectralAudio[std::size_t(n*factor)][ch]));
    }
    const auto after=Clock::now();
#if defined(_MSC_VER) && defined(_DEBUG)
    counting=false;
#endif
    midi.clear();if(phase){ns[std::size_t(k)]=std::chrono::duration<double,std::nano>(after-before).count();
    if(spectralDiagnostic)ordinaryNs[std::size_t(k)]=std::chrono::duration<double,std::nano>(split-before).count();
#if defined(_WIN32)
    cpuIds[std::size_t(k)]=GetCurrentProcessorNumber();
#endif
    }
    for(int ch=0;ch<2;++ch)for(int n=0;n<block;++n){const auto value=audio.getSample(ch,n);finite=finite&&std::isfinite(value);peak=std::max(peak,std::abs(double(value)));energy+=double(value)*value;if(phase){const auto bits=std::bit_cast<std::uint32_t>(value);for(int b=0;b<4;++b)checksum=(checksum^((bits>>(b*8))&255))*1099511628211ull;}}
    if(verifyComponents)for(int n=0;n<block;++n){const auto l=double(audio.getSample(0,n)),r=double(audio.getSample(1,n));midEnergy+=(l+r)*(l+r)*.25;sideEnergy+=(l-r)*(l-r)*.25;}
    if(paced)std::this_thread::sleep_until(start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(k+1)*block/rate)));
   }
   if(phase)measuredWall=std::chrono::duration<double>(Clock::now()-start).count();
  }
#if defined(_MSC_VER) && defined(_DEBUG)
  callbackAllocations=allocations;callbackFrees=frees;
#endif
  dspWorkers=processor->dspWorkerStats();routedStats=processor->spectralStats();stage=2;
 });
 auto* manager=juce::MessageManager::getInstance();while(stage.load()==0){manager->runDispatchLoopUntil(5);if(peerPublishers)peerPublishers->pump(*processor);for(std::size_t i=0;i<slotCount;++i)for(std::size_t b=0;b<8;++b)movement[i*8+b]=std::max(movement[i*8+b],std::abs(i<std::size_t(routedSpectral)?spectrum(i).spectralMovementDb[b]:spectrum(i).movementDb[b]));}
 for(std::size_t i=0;i<slotCount;++i){graphs[i]->updateMonitoring();for(std::size_t b=0;b<8;++b)movement[i*8+b]=std::max(movement[i*8+b],std::abs(i<std::size_t(routedSpectral)?spectrum(i).spectralMovementDb[b]:spectrum(i).movementDb[b]));if(int(i)>=displays){graphs[i]->setSpectrumOn(false);graphs[i]->setVisible(false);}}
 bool grabReady=true;
 // Focus loss deliberately cancels Grab. Only one view can own the focused
 // preview; the other displayed graphs keep analyzing their ordinary spectra.
 const int grabViews=grabPreview?std::min(displays,1):0;
 for(int i=0;i<grabViews;++i){auto& graph=*graphs[std::size_t(i)];graph.setGrabEnabled(true);grabReady=graph.grabEnabled()&&graph.grabPeakCount()>0&&graph.keyPressed(juce::KeyPress(juce::KeyPress::upKey))&&grabReady;}
 bool groupReady=true,groupUndo=true,groupRedo=true,groupOnlySelected=true,groupMoved=false;int groupSteps=0;
 std::array<int,8> groupSelectionStart{},groupSelectionEnd{};std::array<bool,parameterCount> groupFields{};
 struct GroupEvents final:juce::AudioProcessorParameter::Listener {
  juce::AudioProcessorParameter& parameter;int begins=0,ends=0,values=0;
  explicit GroupEvents(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
  ~GroupEvents() override{parameter.removeListener(this);}
  void parameterValueChanged(int,float) override{++values;}
  void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
 };
 std::vector<std::unique_ptr<GroupEvents>> groupEvents;juce::Point<float> groupStart;
 const auto groupMouse=[&](juce::Point<float> at,bool held){const auto time=juce::Time::getCurrentTime();return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,juce::ModifierKeys(held?juce::ModifierKeys::leftButtonModifier:0),1,0,0,0,0,graphs[0].get(),graphs[0].get(),time,groupStart,time,1,held);};
 if(group!=GroupProfile::none){
  groupReady=displays==8&&slotCount==8;
  for(int i=0;i<displays;++i){graphs[std::size_t(i)]->setSelectedBands(std::bitset<8>(255));groupSelectionStart[std::size_t(i)]=int(graphs[std::size_t(i)]->selectedBands().count());groupReady=groupReady&&groupSelectionStart[std::size_t(i)]==8;}
  if(group==GroupProfile::drag){
   for(int band=1;band<=8;++band)for(const auto* field:{"frequency_hz","gain_db"}){const auto index=rackParameterIndex(NativeModule::dynamicEq,0,"dynamic_eq.band."+juce::String(band)+"."+field);require(bool(index),"group profile key missing");groupFields[*index]=true;groupEvents.push_back(std::make_unique<GroupEvents>(processor->control(*index)));}
   groupStart=graphs[0]->nodePosition(0);graphs[0]->mouseDown(groupMouse(groupStart,true));
   for(const auto& event:groupEvents)groupReady=groupReady&&event->begins==1&&event->ends==0;
  }
 }
 bool gainReady=!gainDrag,gainUndo=true,gainRedo=true,gainOnlySelected=true,gainMoved=false;int gainSteps=0;
 const auto gainIndex=dynamicEqGainParameterStart+1;juce::Slider* gainSlider=nullptr;double gainBase=0;
 std::unique_ptr<GroupEvents> gainEvents;
 if(gainDrag){
  gainSlider=dynamic_cast<juce::Slider*>(graphs[0]->findChildWithID("eq.gain_scale"));
  gainReady=displays==8&&slotCount==8&&gainSlider&&gainSlider->isEnabled()&&group==GroupProfile::none;
  if(gainReady){gainSlider->grabKeyboardFocus();gainBase=gainSlider->getValue();gainEvents=std::make_unique<GroupEvents>(processor->control(gainIndex));
   if(gainSlider->onDragStart)gainSlider->onDragStart();gainReady=gainEvents->begins==1&&gainEvents->ends==0;
  }
 }
 bool sketchReady=sketch==SketchProfile::none;int sketchQueued=0,sketchCompleted=0,sketchReplaced=0;
 bool sketchOutstanding=false;double sketchQueueMs=0,sketchPureMs=0,sketchRate=0;std::uint32_t sketchEvaluations=0;
 juce::String sketchFailure;std::uint64_t sketchInitialRevision=0,sketchInitialReset=0,sketchFinalRevision=0,sketchFinalReset=0;
 SoundDocument sketchProbe;bool sketchFinalSnapshot=false,sketchInitialReady=false,sketchFinalReady=false,sketchFinalEnabled=false;
 std::array<EqCurve,2> sketchTargets;std::size_t sketchTargetIndex=0;
 const auto waitSketch=[&]{
  const auto deadline=Clock::now()+std::chrono::seconds(15);
  while(graphs[0]->sketchFitting()&&Clock::now()<deadline){manager->runDispatchLoopUntil(5);graphs[0]->updateMonitoring();}
  graphs[0]->updateMonitoring();return graphs[0]->sketchPreviewReady();
 };
 const auto queueSketch=[&](std::size_t target){
  const auto begin=Clock::now();const bool ok=graphs[0]->previewSketch(sketchTargets[target]);
  sketchQueueMs=std::max(sketchQueueMs,std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
  if(ok){++sketchQueued;sketchOutstanding=true;}return ok;
 };
 if(sketch!=SketchProfile::none){
  (void)processor->trySoundSnapshot(sketchProbe,sketchInitialRevision,sketchInitialReset);
  auto raw=d;raw.macros={};RoutedPatch patch;sketchRate=processor->fxMonitorHandle()->sampleRate();
  if(displays==8&&makeRoutedPatch(raw,patch)&&sketchRate>0){
   const auto base=patch.effects[0].dynamicEq;std::bitset<8> available;
   for(std::size_t band=0;band<8;++band)available.set(band,!base.bands[band].active);
   for(std::size_t variant=0;variant<2;++variant){auto desired=base;std::size_t ordinal=0;
    for(std::size_t band=0;band<8;++band)if(available[band]){
     auto& addition=desired.bands[band];addition={};addition.active=true;
     addition.frequencyHz=available.count()==1?700.f:std::array<float,8>{35,80,200,500,1200,3000,7000,15000}[ordinal];
     addition.gainDb=available.count()==1?(variant?6.f:-6.f):(ordinal%2?-9.f:9.f)*(variant?.95f:1.f);addition.q=available.count()==1?.7071f:2.f;++ordinal;
    }
    for(std::size_t i=0;i<129;++i){const auto response=dynamicEqResponse(desired,sketchRate,20.*std::pow(1000.,double(i)/128.));
     sketchTargets[variant].db[i]=response?std::clamp(float(20.*std::log10(std::max(std::abs(*response),1e-12))),-12.f,12.f):0.f;
    }
   }
   const auto begin=Clock::now();const auto expected=fitEqCurve(sketchTargets[0],base,available,sketchRate);
   sketchPureMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();sketchEvaluations=expected.evaluations;
   graphs[0]->setSketchEnabled(true);sketchReady=expected.ready&&expected.evaluations<=4096&&queueSketch(0)&&waitSketch();
   if(sketchReady){++sketchCompleted;sketchOutstanding=false;const auto error=graphs[0]->sketchFitError();sketchReady=error&&std::isfinite(*error)&&std::abs(*error-expected.rmsDb)<.001f;}
   sketchInitialReady=sketchReady;if(!sketchReady)sketchFailure="initial: "+graphs[0]->getDescription();
  }
 }
 if(match)match->begin();
 std::array<FftWindowActivity,8> fftActivity{};for(std::size_t i=0;i<slotCount;++i)fftActivity[i].previous=spectrum(i).windows;
 const auto observeWindows=[&]{for(std::size_t i=0;i<slotCount;++i)fftActivity[i].observe(spectrum(i).windows);};go=true;
 auto nextGroupEdit=Clock::now(),nextGainEdit=Clock::now(),nextSketchEdit=Clock::now();
 while(stage.load()!=2){manager->runDispatchLoopUntil(sketch==SketchProfile::stress?1:5);if(peerPublishers)peerPublishers->pump(*processor);observeWindows();if(group==GroupProfile::drag&&Clock::now()>=nextGroupEdit){
   ++groupSteps;graphs[0]->mouseDrag(groupMouse(groupStart+juce::Point<float>(groupSteps%2?1.f:-1.f,0),true));nextGroupEdit=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1./30));
  }if(gainDrag&&gainSlider&&Clock::now()>=nextGainEdit){
   ++gainSteps;gainSlider->setValue(gainBase+(gainSteps%2?.01:-.01),juce::sendNotificationSync);nextGainEdit=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1./30));
  }if((sketch==SketchProfile::redraw||sketch==SketchProfile::stress)&&sketchReady){
   graphs[0]->updateMonitoring();
   if(sketchOutstanding&&!graphs[0]->sketchFitting()){
    // The worker can finish after the earlier GUI poll. Consume its mailbox
    // after observing completion before checking the published preview.
    graphs[0]->updateMonitoring();
    if(graphs[0]->sketchPreviewReady())++sketchCompleted;else {sketchReady=false;sketchFailure="completion: "+graphs[0]->getDescription();}
    sketchOutstanding=false;
   }
   if(sketchReady&&stage.load()!=2&&Clock::now()>=nextSketchEdit){
    if(sketchOutstanding)++sketchReplaced;
    sketchTargetIndex=1-sketchTargetIndex;sketchReady=queueSketch(sketchTargetIndex)&&sketchReady;if(!sketchReady)sketchFailure="queue: "+graphs[0]->getDescription();
    nextSketchEdit=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sketch==SketchProfile::stress?0.:1./30));
   }
  }if(match)match->tick();}render.join();observeWindows();
 if(sketch!=SketchProfile::none){
  const bool finished=waitSketch();sketchFinalReady=finished;sketchFinalEnabled=graphs[0]->sketchEnabled();if(sketchOutstanding&&finished)++sketchCompleted;
  if(!finished&&sketchFailure.isEmpty())sketchFailure="final: "+graphs[0]->getDescription();
  sketchFinalSnapshot=processor->trySoundSnapshot(sketchProbe,sketchFinalRevision,sketchFinalReset);
  sketchReady=sketchReady&&finished&&(sketch==SketchProfile::ready?(sketchQueued==1&&sketchCompleted==1):(sketchQueued>=3&&sketchCompleted>=2));
 }
 if(group==GroupProfile::drag){graphs[0]->mouseUp(groupMouse(groupStart,false));groupReady=groupReady&&groupSteps>=20;
  for(const auto& event:groupEvents)groupReady=groupReady&&event->begins==1&&event->ends==1&&event->values>=20;}
 if(group!=GroupProfile::none)for(std::size_t i=0;i<slotCount;++i){groupSelectionEnd[i]=int(graphs[i]->selectedBands().count());groupReady=groupReady&&groupSelectionEnd[i]==8;}
 if(gainDrag){if(gainSlider&&gainSlider->onDragEnd)gainSlider->onDragEnd();gainReady=gainReady&&gainSteps>=20&&gainEvents&&gainEvents->begins==1&&gainEvents->ends==1&&gainEvents->values>=20;}
#if defined(_MSC_VER) && defined(_DEBUG)
 _CrtSetAllocHook(previous);
#endif
 auto result=object();put(result,"cadence",paced?"PACED_HOST_PERIOD":"CONTINUOUS_OFFLINE");put(result,"measured_wall_seconds",measuredWall);put(result,"rate",rate);put(result,"block",block);put(result,"higher",higher);put(result,"displays",displays);put(result,"warm_seconds",double(warmBlocks)*block/rate);put(result,"measured_seconds",double(measuredBlocks)*block/rate);put(result,"callbacks",measuredBlocks);put(result,"finite",finite);put(result,"peak",peak);put(result,"energy",energy);put(result,"checksum",juce::String(checksum));put(result,"callback_allocations",int(callbackAllocations));put(result,"callback_frees",int(callbackFrees));
#if defined(_MSC_VER) && defined(_DEBUG)
 put(result,"allocation_observation","DEBUG_CRT_CALLBACK_AND_PREPARED_DSP_WORKER_JOBS");
#else
 put(result,"allocation_observation","UNAVAILABLE_RELEASE_TIMING_ONLY");
#endif
 const bool pipelineExpected=slotCount>=2&&block*(higher?2:1)>=64;
 put(result,"prepared_dsp_workers",int(dspWorkers.prepared));put(result,"parallel_render_blocks",juce::int64(dspWorkers.blocks));put(result,"parallel_internal_frames",juce::int64(dspWorkers.internalFrames));put(result,"parallel_expected",pipelineExpected);
 const bool workersAdmitted=dspWorkers.prepared==3&&(!pipelineExpected||dspWorkers.blocks>0);put(result,"worker_admission",workersAdmitted);
 auto sorted=ns;std::sort(sorted.begin(),sorted.end());const auto rank=[&](double q){return sorted[std::size_t(std::ceil(q*sorted.size()))-1];};const auto period=double(block)*1e9/rate,mean=std::accumulate(ns.begin(),ns.end(),0.)/ns.size();put(result,"mean_ns",mean);put(result,"p50_ns",rank(.5));put(result,"p99_ns",rank(.99));put(result,"worst_ns",sorted.back());put(result,"period_ns",period);put(result,"overruns",int(std::count_if(ns.begin(),ns.end(),[&](double n){return n>period;})));
 if(spectralDiagnostic){juce::var rows;for(std::size_t i=0;i<ns.size();++i){require(ordinaryNs[i]>=0&&ordinaryNs[i]<=ns[i],"diagnostic callback split bounds");juce::var row;row.append(ns[i]);row.append(ordinaryNs[i]);row.append(ns[i]-ordinaryNs[i]);row.append(int(cpuIds[i]));rows.append(row);}put(result,"diagnostic_callback_split_columns","total_ns,ordinary_ns,spectral_ns,cpu_after_callback");put(result,"diagnostic_callback_split",rows);put(result,"timing_acceptance_eligible",false);}
 juce::var movements,activity;bool moving=true,coverage=true,active=true;int activeBands=0;
 for(std::size_t band=0;band<slotCount*8;++band){const auto v=movement[band];movements.append(v);const bool enabled=d.values[dynamicEqParameterStart+(band/8)*dynamicEqParametersPerSlot+(band%8)*dynamicEqParametersPerBand]>=.5f;const auto ceiling=band/8<std::size_t(routedSpectral)?5.9f:11.9f;if(enabled){++activeBands;moving=moving&&v>.1f&&v<ceiling;}coverage=coverage&&((bypass||!enabled||staticExtensions)?v==0:(v>.1f&&v<ceiling));}
 SoundDocument after;std::uint64_t edit;const bool unchanged=processor->trySoundSnapshot(after,edit)&&after==d;
 if(group==GroupProfile::drag){
  auto unrelated=after;unrelated.values=d.values;groupOnlySelected=unrelated==d;
  for(std::size_t i=0;i<parameterCount;++i)if(after.values[i]!=d.values[i]){groupOnlySelected=groupOnlySelected&&groupFields[i];groupMoved=true;}
  SoundDocument recalled;groupUndo=processor->undoSoundEdit()&&processor->trySoundSnapshot(recalled,edit)&&recalled==d;
  groupRedo=processor->redoSoundEdit()&&processor->trySoundSnapshot(recalled,edit)&&recalled==after;
  groupReady=groupReady&&groupOnlySelected&&groupMoved&&groupUndo&&groupRedo;
 }
 if(gainDrag){
  auto unrelated=after;unrelated.values=d.values;gainOnlySelected=unrelated==d;
  for(std::size_t i=0;i<parameterCount;++i)if(after.values[i]!=d.values[i]){gainOnlySelected=gainOnlySelected&&i==gainIndex;gainMoved=true;}
  SoundDocument recalled;gainUndo=processor->undoSoundEdit()&&processor->trySoundSnapshot(recalled,edit)&&recalled==d;
  gainRedo=processor->redoSoundEdit()&&processor->trySoundSnapshot(recalled,edit)&&recalled==after;
  gainReady=gainReady&&gainOnlySelected&&gainMoved&&gainUndo&&gainRedo;
 }
 for(std::size_t i=0;i<slotCount;++i){auto item=object();const auto stats=processor->fxMonitorHandle()->stats(std::uint8_t(i));const auto windows=fftActivity[i].completed;put(item,"slot",int(i));put(item,"measured_fft_windows",juce::int64(windows));put(item,"fft_counter_resets",juce::int64(fftActivity[i].resets));put(item,"published",juce::int64(stats.published));put(item,"dropped",juce::int64(stats.dropped));const bool subscribed=bool(graphs[i]->subscription())||(match&&i==0&&match->probe.has_value());put(item,"subscribed",subscribed);if(match)put(item,"match_observer",i==0&&match->probe.has_value());activity.append(item);active=active&&(int(i)<displays?(subscribed&&windows>0):!subscribed);if(int(i)<grabViews)grabReady=grabReady&&graphs[i]->grabEnabled()&&graphs[i]->grabPeakCount()>0;if(!match)graphs[i]->retire();}
 put(result,"warm_movement_abs_db",movements);put(result,"activity",activity);put(result,"active_dynamic_bands",activeBands);put(result,"all_active_bands_moving_unsaturated",moving);put(result,"movement_expectation",bypass?"ALL_ZERO_BYPASSED":"ALL_ACTIVE_ABOVE_.1_BELOW_11.9_DB");put(result,"movement_coverage",coverage);put(result,"display_activity",active);put(result,"budget_pass",mean<=.75*period&&rank(.99)<=period);put(result,"integrity",finite&&energy>0&&coverage&&active&&callbackAllocations==0&&callbackFrees==0&&workersAdmitted&&grabReady&&(group==GroupProfile::drag?groupReady:gainDrag?gainReady:unchanged));
 if(staticExtensions){put(result,"active_static_bands",activeBands);put(result,"active_dynamic_bands",0);put(result,"movement_expectation","ALL64_STATIC_ZERO_MOVEMENT_WITH_RETAINED_NONZERO_RANGE");}
 put(result,"spectrum_grab_preview",grabPreview);put(result,"spectrum_grab_views",grabViews);put(result,"spectrum_grab_admitted",grabReady);put(result,"document_unchanged",unchanged);
 put(result,"component_energy_observed",verifyComponents);put(result,"mid_energy",midEnergy);put(result,"side_energy",sideEnergy);
 if(verifyComponents)put(result,"integrity",bool(result["integrity"])&&std::isfinite(midEnergy)&&std::isfinite(sideEnergy)&&midEnergy>1e-10&&sideEnergy>1e-10);
 put(result,"sketch_admitted",sketchReady);put(result,"integrity",bool(result["integrity"])&&sketchReady);
 if(sketch!=SketchProfile::none){put(result,"sketch_queued_jobs",sketchQueued);put(result,"sketch_completed_jobs",sketchCompleted);put(result,"sketch_replaced_pending_jobs",sketchReplaced);put(result,"sketch_requested_redraw_hz",sketch==SketchProfile::redraw?30:0);put(result,"sketch_maximum_queue_ms",sketchQueueMs);put(result,"sketch_reference_fit_ms",sketchPureMs);put(result,"sketch_reference_evaluations",int(sketchEvaluations));put(result,"sketch_fit_internal_rate",sketchRate);}
 if(sketch!=SketchProfile::none){put(result,"sketch_failure",sketchFailure);put(result,"sketch_initial_ready",sketchInitialReady);put(result,"sketch_final_ready",sketchFinalReady);put(result,"sketch_final_enabled",sketchFinalEnabled);put(result,"sketch_initial_revision",juce::int64(sketchInitialRevision));put(result,"sketch_initial_reset",juce::int64(sketchInitialReset));put(result,"sketch_final_revision",juce::int64(sketchFinalRevision));put(result,"sketch_final_reset",juce::int64(sketchFinalReset));put(result,"sketch_final_snapshot",sketchFinalSnapshot);put(result,"sketch_final_snapshot_equal",sketchFinalSnapshot&&sketchProbe==d);}
 if(group!=GroupProfile::none){put(result,"group_mode",group==GroupProfile::drag?"drag":"selection");put(result,"group_admitted",groupReady);put(result,"group_edit_steps",groupSteps);put(result,"group_target_hz",30);put(result,"group_only_selected_values",groupOnlySelected);put(result,"group_sound_changed",groupMoved);put(result,"group_undo_original_exact",groupUndo);put(result,"group_redo_edited_exact",groupRedo);
  juce::var initial,final,events;for(std::size_t n=0;n<slotCount;++n){initial.append(groupSelectionStart[n]);final.append(groupSelectionEnd[n]);}for(const auto& event:groupEvents){auto item=object();put(item,"begins",event->begins);put(item,"ends",event->ends);put(item,"values",event->values);events.append(item);}put(result,"group_selection_initial",initial);put(result,"group_selection_final",final);put(result,"group_host_events",events);put(result,"integrity",bool(result["integrity"])&&groupReady);}
 if(gainDrag){put(result,"gain_drag_admitted",gainReady);put(result,"gain_edit_steps",gainSteps);put(result,"gain_target_hz",30);put(result,"gain_only_scale_changed",gainOnlySelected);put(result,"gain_sound_changed",gainMoved);put(result,"gain_undo_original_exact",gainUndo);put(result,"gain_redo_edited_exact",gainRedo);
  put(result,"gain_host_begins",gainEvents?gainEvents->begins:0);put(result,"gain_host_ends",gainEvents?gainEvents->ends:0);put(result,"gain_host_values",gainEvents?gainEvents->values:0);}
 juce::var intervals;const auto intervalFrames=std::max<std::size_t>(1,std::size_t(std::ceil(.25*rate/block)));
 for(std::size_t first=0;first<ns.size();first+=intervalFrames){const auto end=std::min(ns.size(),first+intervalFrames);std::vector<double> observed(ns.begin()+first,ns.begin()+end);std::sort(observed.begin(),observed.end());auto item=object();put(item,"start_seconds",double(first)*block/rate);put(item,"count",int(observed.size()));put(item,"mean_ns",std::accumulate(observed.begin(),observed.end(),0.)/observed.size());put(item,"p99_ns",observed[std::size_t(std::ceil(.99*observed.size()))-1]);put(item,"overruns",int(std::count_if(observed.begin(),observed.end(),[&](double n){return n>period;})));intervals.append(item);}put(result,"diagnostic_quarter_second_intervals",intervals);
 juce::var byCpu;for(unsigned cpu=0;cpu<256;++cpu){std::vector<double> observed;for(std::size_t n=0;n<ns.size();++n)if(cpuIds[n]==cpu)observed.push_back(ns[n]);if(observed.empty())continue;std::sort(observed.begin(),observed.end());auto item=object();put(item,"logical_cpu",int(cpu));put(item,"count",int(observed.size()));put(item,"mean_ns",std::accumulate(observed.begin(),observed.end(),0.)/observed.size());put(item,"p99_ns",observed[std::size_t(std::ceil(.99*observed.size()))-1]);byCpu.append(item);}put(result,"observed_cpu_after_callback",byCpu);
 if(match){const auto observed=match->finish();put(result,"match",observed);put(result,"integrity",bool(result["integrity"])&&bool(observed["admitted"]));match.reset();for(auto& graph:graphs)if(graph)graph->retire();}
 if(overview){
  bool ready=overview->localCount()==8&&overviewUpdates>0&&analysis.hub->activeAnalyzers()==8&&analysis.hub->packetsLastTick()<=studio::FxAnalysisHub::packetBudgetPerTick;
  for(std::uint8_t slot=0;slot<8;++slot)ready=ready&&overview->localAvailable(slot);
  if(peerPublishers){ready=ready&&overview->peerCount()==4&&peerPublishers->providerJobs.load()==0;for(int i=0;i<4;++i){auto* row=overview->findChildWithID("eq.overview.peer."+juce::String(i));ready=ready&&row&&row->getTitle().contains("EQ8")&&!row->getTitle().containsIgnoreCase("unavailable");}}
  juce::MemoryBlock after;processor->getStateInformation(after);const bool saved=after==overviewState&&processor->groupWorkflowView().selection==aiSelection;
  put(result,"overview_admitted",ready);put(result,"overview_mode",overviewMode==2?"eight_graphs_plus_overview_four_native_peers":"eight_graphs_plus_overview_local");put(result,"overview_updates",int(overviewUpdates));put(result,"overview_maximum_update_ms",overviewMaximumUpdateMs);put(result,"overview_saved_state_and_ai_selection_exact",saved);
  overview->retire();const bool idle=analysis.hub->activeAnalyzers()==0;put(result,"overview_close_capture_idle",idle);put(result,"integrity",bool(result["integrity"])&&ready&&saved&&idle);
 }
 if(spectralStages){put(result,"spectral_bounded_work",boundedSpectral);put(result,"spectral_transform_internal_samples",spectral[0]->transformSize());put(result,"spectral_scheduling_internal_samples",spectral[0]->latencySamples()-spectral[0]->transformSize());put(result,"spectral_reference_window",spectralWindow);put(result,"spectral_spread_hops",spreadSpectral);put(result,"spectral_threshold_db",spectralSettings[0].bands[0].thresholdDb);juce::var phases;for(int i=0;i<spectralStages;++i)phases.append(spreadSpectral?i*(spectral[0]->transformSize()/4)/spectralStages:0);put(result,"spectral_hop_offsets",phases);juce::var values,transforms;bool executed=true,triggered=true;for(int i=0;i<spectralStages;++i){const auto count=spectral[std::size_t(i)]->transformsPerformed();executed=executed&&count>0;transforms.append(juce::int64(count));for(std::size_t band=0;band<8;++band){const auto v=spectralMovement[std::size_t(i)*8+band];values.append(v);triggered=triggered&&v>.1f&&(!spreadSpectral||v<5.9f);}}
  juce::var jobs;bool onTime=true;for(int i=0;i<spectralStages;++i){const auto stats=spectral[std::size_t(i)]->workStats();auto item=object();put(item,"frames_started",juce::int64(stats.framesStarted));put(item,"frames_completed",juce::int64(stats.framesCompleted));put(item,"deadline_misses",juce::int64(stats.deadlineMisses));put(item,"maximum_slice_work",juce::int64(stats.maximumSliceWork));put(item,"maximum_slice_allowance",juce::int64(stats.maximumSliceAllowance));jobs.append(item);if(boundedSpectral)onTime=onTime&&stats.framesCompleted>0&&stats.deadlineMisses==0&&stats.maximumSliceWork>0&&stats.maximumSliceWork<=stats.maximumSliceAllowance;}
  put(result,"spectral_work_stats",jobs);put(result,"spectral_all_jobs_on_time",onTime);put(result,"integrity",bool(result["integrity"])&&onTime);
  put(result,"spectral_stages",spectralStages);put(result,"spectral_storage_bytes",juce::int64(spectralBytes));put(result,"spectral_delay_host_samples",spectralDelay);put(result,"spectral_delay_ms",1000.*spectralDelay/rate);put(result,"spectral_transforms",transforms);put(result,"spectral_maximum_movement_db",values);put(result,"spectral_all_bands_triggered",triggered);put(result,"spectral_actual_transforms",executed);put(result,"spectral_audio_conversion","TEST_ONLY_INTERNAL_RATE_ZERO_ORDER_HOLD;ordinary64bands retained;not compensated product routing");put(result,"integrity",bool(result["integrity"])&&executed&&triggered&&spectralBytes<=64*1024*1024);
 }
 if(routedSpectral){
  const bool onTime=routedStats.framesCompleted>0&&routedStats.deadlineMisses==0&&routedStats.maximumSliceWork>0&&routedStats.maximumSliceWork<=routedStats.maximumSliceAllowance;
  put(result,"routed_spectral_stages",routedSpectral);put(result,"routed_spectral_bands",routedSpectral*8);put(result,"ordinary_dynamic_bands",64-routedSpectral*8);
  put(result,"spectral_all_jobs_on_time",onTime);put(result,"spectral_storage_bytes",juce::int64(routedStats.preparedBytes));put(result,"spectral_frames_started",juce::int64(routedStats.framesStarted));put(result,"spectral_frames_completed",juce::int64(routedStats.framesCompleted));put(result,"spectral_deadline_misses",juce::int64(routedStats.deadlineMisses));put(result,"spectral_maximum_slice_work",juce::int64(routedStats.maximumSliceWork));put(result,"spectral_maximum_slice_allowance",juce::int64(routedStats.maximumSliceAllowance));
  put(result,"host_latency_samples",processor->getLatencySamples());put(result,"integrity",bool(result["integrity"])&&onTime&&routedStats.preparedBytes<=64*1024*1024);
 }
 processor->releaseResources();std::cout<<juce::JSON::toString(result,true)<<'\n';return result;
}
void maximumMatchReferenceWav(const juce::File& file){
 constexpr int rate=192000,frames=30*rate,bytes=frames*16;juce::FileOutputStream stream(file);require(stream.openedOk(),"maximum reference stream");
 require(stream.write("RIFF",4)&&stream.writeInt(36+bytes)&&stream.write("WAVEfmt ",8)&&stream.writeInt(16)&&stream.writeShort(3)&&stream.writeShort(2)&&stream.writeInt(rate)&&stream.writeInt(rate*16)&&stream.writeShort(16)&&stream.writeShort(64)&&stream.write("data",4)&&stream.writeInt(bytes),"maximum reference header");
 std::array<double,8192> scratch;std::uint32_t random=12345;
 for(int frame=0;frame<frames;frame+=4096){const int count=std::min(4096,frames-frame);for(int n=0;n<count;++n){random=random*1664525+1013904223;const auto value=(double(random)/4294967296.*2-1)*.1;scratch[std::size_t(n)*2]=value;scratch[std::size_t(n)*2+1]=-value;}require(stream.write(scratch.data(),std::size_t(count)*16),"maximum reference samples");}
 stream.flush();require(stream.getStatus().wasOk()&&file.getSize()==44+bytes,"maximum reference final size");
}
int profileMatch(const char* path,bool audit=false,bool functional=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"Match allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);maximumMatchReferenceWav(root.getChildFile("maximum-reference.wav"));auto report=object();juce::var cases;bool success=true;
 put(report,"scope","ADDITIONAL_MATCH:64voices/eightEQs/56moving bands; one or eight unused bands in target. Only added sound fields vs reserved Sketch scene: synth1 noise.8/cutoff20k for audible live learning. Every paired case performs identical2.6s acquisition then allSoundOff/64voice restart; unchanged1s warm/2s measured,96k Higher/64 and.75mean/1p99 budgets. Actual native Match workspace: closed,live learn,ready,repeated Amount fit30Hz,maximum30s192k stereo float64 WAV jobs. Eight shared FFT streams retained while visible. Original92 cases remain separate/unchanged.");
 put(report,"allocation_audit_only",audit);put(report,"functional_only",functional);put(report,"timing_admitted",!audit&&!functional);put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"reference_wav_sha256",juce::SHA256(root.getChildFile("maximum-reference.wav")).toHexString());
 for(bool wide:{false,true}){const auto source=matchCapacityScene(wide);juce::MemoryBlock state;require(encodeSoundState(source,state),"Match capacity sound");require(root.getChildFile(wide?"eight-free.racksound":"one-free.racksound").replaceWithData(state.getData(),state.getSize()),"Match capacity save");juce::String baseline;
  for(auto mode:{MatchProfile::closed,MatchProfile::learning,MatchProfile::ready,MatchProfile::refit,MatchProfile::file}){
   if(audit&&mode!=MatchProfile::learning&&mode!=MatchProfile::refit&&mode!=MatchProfile::file)continue;
   auto result=profileCase(source,96000,64,true,mode==MatchProfile::closed?0:8,root,false,true,false,GroupProfile::none,false,SketchProfile::none,false,false,false,0,mode);
   if(mode==MatchProfile::closed)baseline=result["checksum"].toString();const bool same=audit||result["checksum"].toString()==baseline;
   put(result,"free_bands_first_rack",wide?8:1);put(result,"match_audio_exact",audit?juce::var("NOT_OBSERVED_NO_BASELINE_IN_AUDIT"):juce::var(same));put(result,"integrity",bool(result["integrity"])&&same);cases.append(result);put(report,"cases",cases);
   success=success&&bool(result["integrity"])&&(audit||functional||bool(result["budget_pass"]));put(report,"success",success);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"Match capacity report");if(!bool(result["integrity"]))return 1;
  }
 }
 return success?0:1;
}
int profileOverview(const char* path,bool audit=false,bool functional=false){
 const auto root=freshDirectory(path);auto report=object();const auto source=capacityScene();juce::MemoryBlock scene;require(encodeSoundState(source,scene),"overview capacity scene");
 put(report,"scope","ADDITIONAL_OVERVIEW: unchanged original64-voice/64-moving-band scene,48k Normal and96k Higher/64. Per rate:0graphs,8graphs,8graphs plus local overview,8graphs plus overview and4 real admitted native FFT publishers. Peers add analysis/IPC,not four additional synth audio workloads. Same3 normal prepared workers and .75mean/1p99 audio budgets. Original84 cases remain separate and unchanged.");
 put(report,"allocation_audit_only",audit);put(report,"functional_only",functional);put(report,"timing_admitted",!audit&&!functional);put(report,"scene_sha256",juce::SHA256(scene).toHexString());put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());
 juce::var cases;bool success=true;
 for(const bool higher:{false,true}){
  juce::String baseline;
  for(int mode=0;mode<4;++mode){
   auto result=profileCase(source,higher?96000:48000,64,higher,mode==0?0:8,root,false,true,false,GroupProfile::none,false,SketchProfile::none,false,false,false,mode<2?0:mode-1);
   if(mode==0)baseline=result["checksum"].toString();const bool exact=result["checksum"].toString()==baseline;
   put(result,"audio_same_across_overview_and_peers",exact);put(result,"integrity",bool(result["integrity"])&&exact);cases.append(result);success=success&&bool(result["integrity"])&&(audit||functional||bool(result["budget_pass"]));put(report,"cases",cases);put(report,"success",success);
   require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"overview capacity report");
  }
 }
 return success?0:1;
}
enum class StageModulation {none,cutoff,morph};
int profileStages(const char* path,StageModulation mode=StageModulation::none){
 const bool modulated=mode!=StageModulation::none;
 const auto root=freshDirectory(path);const auto source=capacityScene();RoutedPatch patch;
 require(makeRoutedPatch(source,patch),"stage diagnostic patch");juce::MemoryBlock wire;require(encodeSoundState(source,wire),"stage diagnostic state");
 if(modulated)for(auto& layer:patch.synth.layers){layer.lfos[0].rateHz=3;if(mode==StageModulation::morph){layer.oscillators[0].position=.37f;layer.oscillators[1].position=.63f;}for(std::size_t i=0;i<layer.modulation.size();++i)layer.modulation[i]={true,ModSource::lfo1,mode==StageModulation::morph?(i%2?ModTarget::osc1Position:ModTarget::osc2Position):ModTarget::cutoff,mode==StageModulation::morph?ModBank::wavetable:(layer.engine==EngineKind::analog?ModBank::analog:ModBank::wavetable),.005f};}
 auto report=object();put(report,"scope","DIAGNOSTIC_ONLY; same4-synth/16-note source,96k host/64,0 displays; synth raw layers, routed bypass, routed EQ, processor EQ. Different signal taps are not interchangeable audio or capacity acceptance.");
 put(report,"cpu",juce::SystemStats::getCpuModel());put(report,"scene_sha256",juce::SHA256(wire).toHexString());put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"source_sha256",juce::SHA256(juce::File(__FILE__)).toHexString());
 put(report,"modulation_override",mode==StageModulation::morph?"DIRECT_SYNTH_PATCH: base osc positions.37/.63; all64 routes enabled, LFO1 to alternating Wavetable-bank osc1/osc2 positions (inactive in Analog layers), amount.005,3Hz. Original scene hash plus this recipe; Processor omitted.":modulated?"DIRECT_SYNTH_PATCH: all64 routes enabled, LFO1 to matching-bank cutoff, amount.005,3Hz; original scene hash plus this explicit recipe. Processor stage omitted because its document has no override.":"NONE");
 juce::var cases;bool valid=true;
 for(bool higher:{false,true})for(int stage=0;stage<(modulated?3:4);++stage){
  constexpr int rate=96000,block=64,warm=1500,count=3000;const int factor=higher?2:1,internalBlock=block*factor;
  const char* names[]{"synth_raw_layers","routed_eq_bypassed","routed_eq_active","processor_eq_active"};
  auto synth=std::make_unique<SynthEngine>();auto routed=std::make_unique<RoutedSynthEngine>();
  companion::ClientOptions offline;offline.allowLaunch=false;auto processor=std::make_unique<InstrumentProcessor>(offline);
  if(stage==0)require(synth->setPatch(patch.synth)&&synth->prepare(rate*factor,internalBlock),"stage synth prepare");
  else if(stage<3){auto routedPatch=patch;if(stage==1)for(auto& effect:routedPatch.effects)effect.bypassed=true;require(routed->setPatch(routedPatch)&&routed->prepare(rate,block,higher?RenderQuality::higher:RenderQuality::realtime),"stage routed prepare");}
  else{auto d=source;d.values[758]=higher?1.f:0.f;juce::MemoryBlock state;require(encodeSoundState(d,state)&&processor->restoreSoundState(state.getData(),int(state.getSize())),"stage processor recall");processor->setNonRealtime(higher);processor->prepareToPlay(rate,block);require(processor->effectiveRenderQuality()==(higher?RenderQuality::higher:RenderQuality::realtime),"stage processor quality");}
  std::array<juce::AudioBuffer<float>,4> layers;std::array<juce::AudioBuffer<float>*,4> pointers;
  for(std::size_t i=0;i<4;++i){layers[i].setSize(2,internalBlock);pointers[i]=&layers[i];}
  std::vector<float> master(std::size_t(internalBlock),0.f);juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;
  midi.ensureSize(4096);for(int note=48;note<64;++note)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);
  std::vector<double> ns(count);double energy=0;bool finite=true;std::uint64_t checksum=14695981039346656037ull;std::atomic<bool> done=false;
  std::thread render([&]{
   for(int n=-warm;n<count;++n){const auto before=std::chrono::steady_clock::now();
    if(stage==0)synth->renderLayers(pointers,master,midi);else if(stage<3)routed->render(audio,midi);else processor->processBlock(audio,midi);
    const auto after=std::chrono::steady_clock::now();midi.clear();if(n<0)continue;ns[std::size_t(n)]=std::chrono::duration<double,std::nano>(after-before).count();
    const auto observe=[&](const juce::AudioBuffer<float>& buffer){for(int ch=0;ch<2;++ch)for(int sample=0;sample<buffer.getNumSamples();++sample){const auto v=buffer.getSample(ch,sample);finite=finite&&std::isfinite(v);energy+=double(v)*v;const auto bits=std::bit_cast<std::uint32_t>(v);for(int b=0;b<4;++b)checksum=(checksum^((bits>>(8*b))&255))*1099511628211ull;}};
    if(stage==0)for(const auto& layer:layers)observe(layer);else observe(audio);
   }done=true;
  });
  while(!done.load())juce::MessageManager::getInstance()->runDispatchLoopUntil(5);render.join();std::sort(ns.begin(),ns.end());
  auto result=object();put(result,"stage",names[stage]);put(result,"higher",higher);put(result,"host_rate",rate);put(result,"host_block",block);put(result,"internal_rate",rate*factor);put(result,"warm_seconds",1);put(result,"measured_seconds",2);put(result,"cadence","CONTINUOUS_DIAGNOSTIC");put(result,"mean_ns",std::accumulate(ns.begin(),ns.end(),0.)/count);put(result,"p50_ns",ns[std::size_t(std::ceil(.5*count))-1]);put(result,"p99_ns",ns[std::size_t(std::ceil(.99*count))-1]);put(result,"worst_ns",ns.back());put(result,"finite",finite);put(result,"energy",energy);put(result,"checksum",juce::String(checksum));put(result,"host_period_ns",double(block)*1e9/rate);valid=valid&&finite&&energy>0;cases.append(result);put(report,"cases",cases);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"stage report");std::cout<<juce::JSON::toString(result,true)<<'\n';processor->releaseResources();
 }
 return valid?0:1;
}
SoundDocument detectorCapacityScene(int mode){
 require(mode>=0&&mode<4,"unknown detector workload");auto source=capacityScene();
 for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto prefix="effect."+juce::String(int(slot+1))+".dynamic_eq.band."+juce::String(int(band+1))+".";
  set(source,prefix+"detector_source",mode<2?1.f:3.f);set(source,prefix+"detector_low_hz",200);set(source,prefix+"detector_high_hz",4000);
  set(source,prefix+"adaptive_timing",mode>0?1.f:0.f);set(source,prefix+"auto_threshold",mode==3?1.f:0.f);if(mode>=2)eqValue(source,slot,band,8,-48);
 }require(validateSoundDocument(source),"invalid detector capacity scene");return source;
}
void detectorWorkloadAdmission(){
 std::cout<<"SECTION detector_capacity_64_free_triggers_modes_and_manual_bases\n";
 for(int mode=0;mode<4;++mode){const auto d=detectorCapacityScene(mode);RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));int active=0;
  for(const auto& effect:patch.effects)for(const auto& band:effect.dynamicEq.bands){active+=band.active&&!band.bypassed;CHECK(band.shape==DynamicEqShape::bell&&std::abs(band.rangeDb)==12);CHECK(band.detectorSource==(mode<2?DynamicEqDetectorSource::internalFree:DynamicEqDetectorSource::externalFree));CHECK(band.adaptiveTiming==(mode>0)&&band.autoThreshold==(mode==3));CHECK(std::abs(band.detectorLowHz-200)<.01f&&std::abs(band.detectorHighHz-4000)<.01f);CHECK(std::abs(band.attackMs-10)<.001f&&std::abs(band.releaseMs-100)<.001f);}
  CHECK(active==64);CHECK(d.layout==capacityScene().layout);CHECK(d.macros==capacityScene().macros);
 }
}
int profileDetectors(const char* path,bool audit=false,bool functional=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"detector allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);auto report=object();juce::var cases;bool success=true;
 put(report,"scope","ADDITIONAL_DETECTORS:4 synths/16 held notes/64 voices,8 serial EQs/64 moving free detectors. InternalFree/manual,InternalFree/adaptive,ExternalFree/adaptive,ExternalFree/auto+adaptive;48k/64/Realtime and96k/64/Higher,0/8 actual graphs. External1kHz .0085-amplitude10ms pulses every80ms,host input copied before output. Original68 cases remain separate and unchanged. Same .75mean/1p99 budgets and3normal prepared workers.");
 put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"allocation_audit_only",audit);put(report,"functional_only",functional);put(report,"timing_admitted",!audit&&!functional);
 for(int mode=0;mode<4;++mode){const auto source=detectorCapacityScene(mode);juce::MemoryBlock bytes;require(encodeSoundState(source,bytes),"detector capacity encode");
  require(root.getChildFile("detector-mode-"+juce::String(mode)+".soundstate").replaceWithData(bytes.getData(),bytes.getSize()),"detector capacity save");
  for(bool higher:{false,true}){if(audit&&!higher)continue;juce::String baseline;
   for(int displays:{0,8}){if(audit&&displays==0)continue;
    auto result=profileCase(source,higher?96000:48000,64,higher,displays,root,false,true,false,GroupProfile::none,false,SketchProfile::none,false,false,mode>=2);
    put(result,"detector_mode",mode);put(result,"scene_sha256",juce::SHA256(bytes).toHexString());if(displays==0)baseline=result["checksum"].toString();const bool same=audit||result["checksum"].toString()==baseline;
    if(!audit)put(result,"audio_same_across_displays",same);put(result,"integrity",bool(result["integrity"])&&same);cases.append(result);success=success&&bool(result["integrity"])&&(audit||functional||bool(result["budget_pass"]));put(report,"cases",cases);put(report,"success",success);
    require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"detector capacity report");
   }
  }
 }return success?0:1;
}
int profileShapes(const char* path,bool audit=false,bool functional=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"shape allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);auto report=object();juce::var cases;bool success=true;
 put(report,"scope","ADDITIONAL_SHAPES:4 synths/16 held notes/64 voices,eight serial EQs/64 active static bands retaining nonzero dynamic ranges. Maximum512sections of96dB high cuts;384 FlatTilt sections;264 mixed sections using all new shapes/slopes and placements. Same actual processor/3normal workers/real graphs/host cadence and .75mean/1p99 budgets. Original44 cases remain unchanged and separate.");
 put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());
 put(report,"allocation_audit_only",audit);put(report,"functional_only",functional);put(report,"timing_admitted",!audit&&!functional);
 for(int mode=0;mode<3;++mode){const auto source=shapeCapacityScene(mode);juce::MemoryBlock bytes;require(encodeSoundState(source,bytes),"shape workload encode");
  require(root.getChildFile(juce::String(shapeSceneName(mode))+".soundstate").replaceWithData(bytes.getData(),bytes.getSize()),"shape workload save");
  for(int rate:{48000,96000})for(int block:{64,512})for(bool higher:{false,true}){
   if((mode!=0||audit||functional)&&(rate!=96000||block!=64))continue;if(audit&&!higher)continue;juce::String baseline;
   for(int displays:{0,8}){if((audit||functional)&&displays==0)continue;
    auto result=profileCase(source,rate,block,higher,displays,root,false,true,false,GroupProfile::none,false,SketchProfile::none,mode==2,true);
    put(result,"shape_scene",shapeSceneName(mode));put(result,"static_sections",mode==0?512:mode==1?384:264);put(result,"scene_sha256",juce::SHA256(bytes).toHexString());
    if(displays==0)baseline=result["checksum"].toString();const bool same=audit||functional||result["checksum"].toString()==baseline;
    if(!audit&&!functional)put(result,"audio_same_across_displays",same);put(result,"integrity",bool(result["integrity"])&&same);
    cases.append(result);success=success&&bool(result["integrity"])&&(audit||functional||bool(result["budget_pass"]));put(report,"cases",cases);put(report,"success",success);
    require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"shape report save");
   }
  }
 }return success?0:1;
}
int profilePlacement(const char* path,bool audit=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"placement allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);auto report=object();juce::var cases;bool success=true;
 put(report,"scope","ADDITIONAL_PLACEMENT_CASES:96k/64/Higher,4 synths/64 held voices,8 serial EQs/64 moving unsaturated bands; allMid/allSide/mixedStereo-L-R-M-S,0/8 actual graph owners. Alternate analog/wavetable layers panned -.75/+.75 supply real nonzero Mid AND Side. Original38 cases run separately without changes. Same .75mean/1p99 budgets,3 normal prepared workers.");
 put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"allocation_audit_only",audit);
 for(int mode=0;mode<3;++mode){const auto source=placementCapacityScene(mode);const juce::String name=std::array<const char*,3>{"all-mid","all-side","mixed"}[std::size_t(mode)];
  juce::MemoryBlock bytes;require(encodeSoundState(source,bytes),"placement capacity state");require(root.getChildFile(name+".soundstate").replaceWithData(bytes.getData(),bytes.getSize()),"placement capacity save");juce::String baseline;
  for(int displays:{0,8}){if(audit&&displays==0)continue;
   auto result=profileCase(source,96000,64,true,displays,root,false,true,false,GroupProfile::none,false,SketchProfile::none,true);
   put(result,"placement_mode",name);put(result,"scene_sha256",juce::SHA256(bytes).toHexString());
   if(displays==0)baseline=result["checksum"].toString();const bool same=audit||result["checksum"].toString()==baseline;
   if(!audit)put(result,"audio_same_across_displays",same);put(result,"integrity",bool(result["integrity"])&&same);
   cases.append(result);success=success&&bool(result["integrity"])&&(audit||bool(result["budget_pass"]));put(report,"cases",cases);put(report,"success",success);
   require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"placement profile save");
  }
 }return success?0:1;
}
int profile(const char* path,bool bypass=false,bool paced=true,bool audit=false,bool single=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"allocation audit requires a Debug MSVC build");
#endif
const auto root=freshDirectory(path);auto report=object();put(report,"cpu",juce::SystemStats::getCpuModel());put(report,"logical_cpus",juce::SystemStats::getNumCpus());put(report,"os",juce::SystemStats::getOperatingSystemName());put(report,"scope","Actual InstrumentProcessor; scene field and saved state define source/slot count. MIDI48..63 velocity100. Warm movement captured with real graphs; timed owners shown per case. Actual message-thread timers/FFT/painting compete with a separate render thread. Cadence explicitly recorded; no affinity/priority adjustment.");put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"source_sha256_at_start",juce::SHA256(juce::File(__FILE__)).toHexString());
 auto source=capacityScene();if(single){for(std::uint8_t slot=1;slot<4;++slot)require(editRackLayout(source.layout,{RackEditKind::removeSynth,slot},source.layout)==RackEditResult::applied,"single synth scene");for(std::uint8_t slot=1;slot<8;++slot)require(editRackLayout(source.layout,{RackEditKind::removeEffect,slot},source.layout)==RackEditResult::applied,"single EQ scene");for(std::size_t band=0;band<8;++band)eqValue(source,0,band,8,band%2?-53.f:-50.f);}
 put(report,"scene",single?"ONE_SYNTH_16_NOTES_ONE_EQ_8_DYNAMIC_BANDS":"FOUR_SYNTHS_16_NOTES_EIGHT_EQS_64_DYNAMIC_BANDS");if(bypass)for(int slot=1;slot<=8;++slot)set(source,"effect."+juce::String(slot)+".bypassed",1);put(report,"eq_bypassed_diagnostic",bypass);put(report,"allocation_audit_only",audit);juce::MemoryBlock bytes;require(encodeSoundState(source,bytes),"capacity state");
 RoutedPatch tailPatch;require(makeRoutedPatch(source,tailPatch),"tail diagnostic patch");const auto tailStart=std::chrono::steady_clock::now();double tailChecksum=0;for(int trial=0;trial<1000;++trial)for(const auto& effect:tailPatch.effects)tailChecksum+=dynamicEqTailSeconds(effect.dynamicEq,96000);const auto tailNanoseconds=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-tailStart).count()/1000;put(report,"uncached_eight_slot_tail_estimate_ns",tailNanoseconds);put(report,"tail_checksum",tailChecksum);std::cout<<"TAIL_ESTIMATE eight_slots_ns="<<tailNanoseconds<<" checksum="<<tailChecksum<<'\n';require(root.getChildFile("capacity.racksound").replaceWithData(bytes.getData(),bytes.getSize()),"capacity save");put(report,"scene_sha256",juce::SHA256(bytes).toHexString());juce::var cases;bool success=true;
 juce::String baselineChecksum;
 for(int rate:{48000,96000})for(int block:{64,512})for(bool higher:{false,true})for(int displays:{0,1,8}){if(single&&displays==8)continue;if(audit&&!(rate==96000&&block==64&&higher&&displays==8))continue;auto result=profileCase(source,rate,block,higher,displays,root,bypass,paced);if(displays==0||audit)baselineChecksum=result["checksum"].toString();const bool same=result["checksum"].toString()==baselineChecksum;put(result,"audio_same_across_displays",same);put(result,"integrity",bool(result["integrity"])&&same);cases.append(result);put(report,"cases",cases);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"profile report");success=success&&bool(result["integrity"])&&(audit||bool(result["budget_pass"]));if(!bool(result["integrity"]))return 1;}
 return success?0:1;
}
int profileSpectral(const char* path,bool functional=false,int onlyStages=0,int referenceWindow=4096,bool spread=false,bool screening=false,bool bounded=false,bool diagnostic=false){
 require(onlyStages==0||onlyStages==1||onlyStages==8,"spectral stage selector");require(referenceWindow==512||referenceWindow==1024||referenceWindow==2048||referenceWindow==4096,"spectral window");
 const auto root=freshDirectory(path);const auto source=capacityScene();auto report=object();juce::var cases;bool success=true;
 put(report,"scope","FEASIBILITY ONLY: actual64voice/64movingordinaryband InstrumentProcessor with1or8 test-only serial spectral finishing stages at actual internalrate. Prepared internalrate zero-order-hold copy; no product routing/host compensation adoption. All original workloads,budgets,threeworkers unchanged; extra ordinary64bands retained conservatively.");
 put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"functional_only",functional);put(report,"bounded_transform_work",bounded);put(report,"reference_window",referenceWindow);put(report,"spread_hops",spread);put(report,"screening",screening);if(diagnostic)put(report,"diagnostic_only",true);
 juce::MemoryBlock bytes;require(encodeSoundState(source,bytes),"spectral baseline state");put(report,"scene_sha256",juce::SHA256(bytes).toHexString());require(root.getChildFile("capacity.racksound").replaceWithData(bytes.getData(),bytes.getSize()),"spectral state save");
 for(int stages:{1,8}){if(onlyStages&&stages!=onlyStages)continue;for(int rate:{48000,96000})for(int block:{64,512})for(bool higher:{false,true}){juce::String checksum;
  for(int displays:{0,1,8}){if(functional&&!(rate==96000&&block==64&&higher))continue;if(screening&&!(rate==96000&&block==64&&higher&&displays!=1))continue;
   auto result=profileCase(source,rate,block,higher,displays,root,false,!functional,false,GroupProfile::none,false,SketchProfile::none,false,false,false,0,MatchProfile::none,stages,referenceWindow,spread,bounded,diagnostic);
   if(displays==0)checksum=result["checksum"].toString();const bool same=checksum==result["checksum"].toString();put(result,"audio_same_across_displays",same);put(result,"integrity",bool(result["integrity"])&&same);
   cases.append(result);put(report,"cases",cases);success=success&&bool(result["integrity"])&&(functional||bool(result["budget_pass"]));put(report,"success",success);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"spectral report");
  }
 }}return success?0:1;
}
SoundDocument routedSpectralCapacityScene(int stages){
 require(stages==1||stages==8,"routed spectral stage selector");auto source=capacityScene();
 for(int slot=0;slot<stages;++slot){const auto base=dynamicEqSpectralParameterStart+std::size_t(slot)*25;source.values[base]=1;
  for(std::size_t band=0;band<8;++band){source.values[base+1+band*3]=source.values[base+2+band*3]=1;
   eqValue(source,slot,band,3,500);eqValue(source,slot,band,5,.25f);eqValue(source,slot,band,7,band%2?6.f:-6.f);eqValue(source,slot,band,8,-40);
  }
 }
 require(validateSoundDocument(source),"routed spectral scene invalid");return source;
}
int profileRoutedSpectral(const char* path,bool functional=false,int onlyStages=0,bool screen=false){
 const auto root=freshDirectory(path);auto report=object();juce::var cases;bool success=true;
 put(report,"scope","INTEGRATED PRODUCT:64voices/eight routed EQs/64 active bands;1or8 prepared full-resolution spectral stages replace selected ordinary processing. Other ordinary bands remain active. Original ordinary64 and conservative ordinary64+spectral64 feasibility cohorts remain separate, unchanged. Reference4096/hopN/4/D=N+H;3 workers;1s warm/2s measured;.75mean/1p99 budgets.");
 put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"functional_only",functional);put(report,"screening",screen);
 for(int stages:{1,8}){if(onlyStages&&stages!=onlyStages)continue;const auto source=routedSpectralCapacityScene(stages);juce::MemoryBlock bytes;require(encodeSoundState(source,bytes),"routed spectral encode");require(root.getChildFile("capacity-"+juce::String(stages)+".racksound").replaceWithData(bytes.getData(),bytes.getSize()),"routed spectral save");
  for(int rate:{48000,96000})for(int block:{64,512})for(bool higher:{false,true}){juce::String checksum;
   for(int displays:{0,1,8}){if((functional||screen)&&!(rate==96000&&block==64&&higher))continue;if(screen&&displays==1)continue;
    auto result=profileCase(source,rate,block,higher,displays,root,false,!functional,false,GroupProfile::none,false,SketchProfile::none,false,false,false,0,MatchProfile::none,0,4096,false,false,false,stages);
    if(displays==0)checksum=result["checksum"].toString();const bool same=checksum==result["checksum"].toString();put(result,"audio_same_across_displays",same);put(result,"integrity",bool(result["integrity"])&&same);
    cases.append(result);put(report,"cases",cases);success=success&&bool(result["integrity"])&&(functional||bool(result["budget_pass"]));put(report,"success",success);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"routed spectral report");
   }
  }
 }return success?0:1;
}
int profileGrab(const char* path){
 const auto root=freshDirectory(path);auto source=capacityScene();for(std::size_t slot=0;slot<8;++slot)eqValue(source,slot,7,0,0);
 auto report=object();put(report,"scope","ADDITIONAL_SPECTRUM_GRAB_CASE_ONLY: 96k/64/Higher,64 synth voices,eight EQs,56 moving unsaturated bands and one free band per rack,eight real graph owners: one focused frozen/preview and seven live spectra. Original64-band24-case acceptance remains separate and unchanged.");
 juce::MemoryBlock state;require(encodeSoundState(source,state),"grab capacity state");put(report,"scene_sha256",juce::SHA256(state).toHexString());put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());
 const auto baseline=profileCase(source,96000,64,true,0,root);const auto preview=profileCase(source,96000,64,true,8,root,false,true,true);
 put(report,"baseline",baseline);put(report,"preview",preview);const bool same=baseline["checksum"].toString()==preview["checksum"].toString();put(report,"audio_exact",same);
 const bool ok=same&&bool(baseline["integrity"])&&bool(preview["integrity"])&&bool(baseline["budget_pass"])&&bool(preview["budget_pass"]);put(report,"success",ok);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"grab profile save");return ok?0:1;
}
int profileGroups(const char* path,bool audit=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"group allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);const auto source=capacityScene();auto report=object();juce::var cases;
 put(report,"scope","ADDITIONAL_GROUP_EDIT_CASES: original64 moving bands/64 synth voices/eight serial EQs;96k/64/Higher; baseline0graphs,8graphs each selecting8bands,8graphs with one focused8-band live frequency drag at30Hz. Original24 and Grab gates remain unchanged.");
 put(report,"allocation_audit_only",audit);put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());juce::MemoryBlock wire;require(encodeSoundState(source,wire),"group workload state");put(report,"scene_sha256",juce::SHA256(wire).toHexString());
 juce::String baseline;bool success=true;
 for(const auto mode:{GroupProfile::none,GroupProfile::selection,GroupProfile::drag}){
  auto result=profileCase(source,96000,64,true,mode==GroupProfile::none?0:8,root,false,true,false,mode);
  if(mode==GroupProfile::none)baseline=result["checksum"].toString();
  if(mode==GroupProfile::selection){const bool same=result["checksum"].toString()==baseline;put(result,"selection_audio_exact",same);put(result,"integrity",bool(result["integrity"])&&same);}
  cases.append(result);put(report,"cases",cases);success=success&&bool(result["integrity"])&&(audit||bool(result["budget_pass"]));put(report,"success",success);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"group profile save");
  if(!bool(result["integrity"])){std::cerr<<"FAIL group_profile_admission mode="<<int(mode)<<'\n';return 1;}
 }return success?0:1;
}
int profileGain(const char* path,bool audit=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"gain allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);const auto source=gainCapacityScene();RoutedPatch patch;require(makeRoutedPatch(source,patch),"gain workload patch");
 auto report=object();juce::var cases,estimates;bool admitted=true;
 for(const auto& fx:patch.effects){const auto& eq=fx.dynamicEq;const auto compensation=dynamicEqAutoGainDb(eq);estimates.append(compensation);
  admitted=admitted&&eq.autoGain&&eq.gainScale!=1&&std::abs(compensation)>.0001f;
  for(const auto& band:eq.bands)admitted=admitted&&band.active&&band.gainDb!=0;
 }require(admitted,"gain workload must actually enable static scaling and nonzero compensation");
 put(report,"scope","ADDITIONAL_GAIN_CASES: original64 moving dynamic bands/64 synth voices/eight serial EQs,96k/64/Higher; all AutoGain enabled,Scale1.5,nonzero balanced static gains with positive residual. Baseline0graphs,8live graphs,8graphs with one actual Scale slider drag at30Hz. Original29 capacity gates remain unchanged.");
 put(report,"settings_gain_estimates_db",estimates);put(report,"gain_workload_admitted",admitted);put(report,"allocation_audit_only",audit);
 put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());put(report,"source_sha256_at_start",juce::SHA256(juce::File(__FILE__)).toHexString());
 juce::MemoryBlock wire;require(encodeSoundState(source,wire),"gain workload state");put(report,"scene_sha256",juce::SHA256(wire).toHexString());require(root.getChildFile("capacity.racksound").replaceWithData(wire.getData(),wire.getSize()),"gain workload save");
 juce::String baseline;bool success=true;
 for(int mode=0;mode<3;++mode){auto result=profileCase(source,96000,64,true,mode==0?0:8,root,false,true,false,GroupProfile::none,mode==2);
  put(result,"gain_mode",mode==0?"baseline":mode==1?"eight_graphs":"scale_drag");
  if(mode==0)baseline=result["checksum"].toString();
  if(mode==1){const bool same=result["checksum"].toString()==baseline;put(result,"display_audio_exact",same);put(result,"integrity",bool(result["integrity"])&&same);}
  cases.append(result);put(report,"cases",cases);success=success&&bool(result["integrity"])&&(audit||bool(result["budget_pass"]));put(report,"success",success);
  require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"gain profile save");if(!bool(result["integrity"]))return 1;
 }return success?0:1;
}
int profileSketch(const char* path,bool audit=false,bool stress=false){
#if !defined(_MSC_VER) || !defined(_DEBUG)
 require(!audit,"Sketch allocation audit requires Debug MSVC");
#endif
 const auto root=freshDirectory(path);auto report=object();juce::var cases;bool success=true;
 put(report,"scope","ADDITIONAL_SKETCH_CASES: 64 synth voices,eight serial EQs,56 moving active bands. Either one free band per rack or eight free bands in the first rack. Each0graph baseline,8graph ready preview and8graph repeated latest fitting at30Hz. Original32 accepted capacity cases stay unchanged.");
 put(report,"allocation_audit_only",audit);put(report,"helper_sha256",juce::SHA256(juce::File::getSpecialLocation(juce::File::currentExecutableFile)).toHexString());
 put(report,"diagnostic_stress_only",stress);
 if(stress)put(report,"scope","DIAGNOSTIC_ONLY: unthrottled latest-target replacements, two reserved-band scenes, stable sound and explicit refusal diagnostics. Not a capacity timing acceptance; no baseline audio comparison in this invocation.");
 for(const bool wide:{false,true}){const auto source=sketchCapacityScene(wide);juce::MemoryBlock state;require(encodeSoundState(source,state),"Sketch capacity state");
  require(root.getChildFile(wide?"eight-free.racksound":"one-free-per-rack.racksound").replaceWithData(state.getData(),state.getSize()),"Sketch capacity save");juce::String baseline;
  for(const auto mode:{SketchProfile::none,SketchProfile::ready,SketchProfile::redraw}){
   if((audit||stress)&&mode!=SketchProfile::redraw)continue;
   auto result=profileCase(source,96000,64,true,mode==SketchProfile::none?0:8,root,false,true,false,GroupProfile::none,false,stress?SketchProfile::stress:mode);
   if(mode==SketchProfile::none)baseline=result["checksum"].toString();
   const bool same=audit||stress||result["checksum"].toString()==baseline;
   put(result,"free_bands_first_rack",wide?8:1);put(result,"sketch_mode",mode==SketchProfile::none?"baseline":mode==SketchProfile::ready?"ready":"redraw");
   if(!audit&&!stress)put(result,"preview_audio_exact",same);else put(result,"audio_comparison","NOT_OBSERVED_NO_BASELINE_IN_THIS_AUDIT");put(result,"integrity",bool(result["integrity"])&&same);cases.append(result);put(report,"cases",cases);
   success=success&&bool(result["integrity"])&&(audit||stress||bool(result["budget_pass"]));put(report,"success",success);
   require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"Sketch profile save");
   if(!bool(result["integrity"]))return 1;
  }
 }
 return success?0:1;
}
int sketchFixtures(const char* inputPath,const char* outputPath){
 const auto root=freshDirectory(outputPath);const juce::File input{juce::String::fromUTF8(inputPath)};
 require(juce::File::isAbsolutePath(inputPath)&&input.existsAsFile(),"Sketch input project missing");
 juce::MemoryBlock originalBytes;require(input.loadFileAsData(originalBytes),"Sketch input read");ProjectDocument original;
 require(decodeProjectState(originalBytes.getData(),originalBytes.getSize(),original),"Sketch input decode");
 const auto slot=std::size_t(std::find_if(original.sound.layout.effects.begin(),original.sound.layout.effects.end(),[](const auto& effect){return effect.use==SlotUse::present&&effect.kind==EffectKind::dynamicEq;})-original.sound.layout.effects.begin());
 require(slot<8,"Sketch input has no EQ");
 companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
 auto processor=std::make_unique<InstrumentProcessor>(offline);
 juce::MemoryBlock originalSound;require(encodeSoundState(original.sound,originalSound),"Sketch original sound encode");
 processor->setNonRealtime(false);processor->prepareToPlay(48000,64);
 juce::Component window;window.setBounds(100,80,1050,280);window.addToDesktop(juce::ComponentPeer::windowIsTemporary);window.setVisible(true);
 studio::DynamicEqGraph graph(*processor);window.addAndMakeVisible(graph);graph.setBounds(0,0,1045,274);graph.setSpectrumOn(false);
 auto report=object();juce::var files;put(report,"input",input.getFullPathName());put(report,"input_sha256",juce::SHA256(originalBytes).toHexString());
 require(root.getChildFile("reference.airackproject").replaceWithData(originalBytes.getData(),originalBytes.getSize()),"Sketch reference save");
 auto reference=object();put(reference,"name","reference");put(reference,"file",root.getChildFile("reference.airackproject").getFullPathName());put(reference,"sha256",juce::SHA256(originalBytes).toHexString());files.append(reference);
 for(int variant=0;variant<3;++variant){
  require(processor->restoreSoundState(originalSound.getData(),int(originalSound.getSize())),"Sketch fixture recall");graph.refresh(original.sound,std::uint8_t(slot));
  auto raw=original.sound;raw.macros={};RoutedPatch routed;require(makeRoutedPatch(raw,routed),"Sketch raw patch");const auto base=routed.effects[slot].dynamicEq;auto desired=base;
  std::vector<std::size_t> available;for(std::size_t band=0;band<8;++band)if(!base.bands[band].active)available.push_back(band);
  require(available.size()>=2,"Sketch fixture needs two unused bands");
  auto& first=desired.bands[available[0]];first={};first.active=true;first.q=.7071f;
  if(variant==0){first.frequencyHz=700;first.gainDb=-6;}
  if(variant==1){first.frequencyHz=3800;first.gainDb=6;}
  if(variant==2){first.shape=DynamicEqShape::lowShelf;first.frequencyHz=600;first.gainDb=-6;auto& second=desired.bands[available[1]];second={};second.active=true;second.shape=DynamicEqShape::highShelf;second.frequencyHz=5000;second.gainDb=6;second.q=.7071f;}
  const auto rate=processor->fxMonitorHandle()->sampleRate();EqCurve target;
  for(std::size_t i=0;i<129;++i){const auto response=dynamicEqResponse(desired,rate,20.*std::pow(1000.,double(i)/128.));require(bool(response),"Sketch fixture target response");target.db[i]=float(20.*std::log10(std::max(std::abs(*response),1e-12)));}
  graph.setSketchEnabled(true);require(graph.previewSketch(target),"Sketch fixture preview rejected");
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(graph.sketchFitting()&&std::chrono::steady_clock::now()<deadline){juce::MessageManager::getInstance()->runDispatchLoopUntil(5);graph.updateMonitoring();}
  graph.updateMonitoring();require(graph.sketchPreviewReady()&&graph.sketchFitError(),"Sketch fixture fit unavailable");const auto previewError=*graph.sketchFitError();
  SoundDocument before,after;std::uint64_t revision;require(processor->trySoundSnapshot(before,revision)&&before==original.sound,"Sketch preview changed sound");
  require(graph.applySketch()&&processor->trySoundSnapshot(after,revision),"Sketch fixture Apply");
  require(after.macros==before.macros&&after.layout==before.layout&&after.controlLocks==before.controlLocks&&after.effectLocks==before.effectLocks,"Sketch changed unrelated intent");
  std::bitset<parameterCount> allowed;const auto indices=unitReach(NativeModule::dynamicEq,std::uint8_t(slot));
  for(const auto band:available){const auto prefix="effect."+juce::String(int(slot+1))+".dynamic_eq.band."+juce::String(int(band+1))+".";for(const auto index:indices)if(parameterRegistry()[index].id.startsWith(prefix))allowed.set(index);}
  for(std::size_t i=0;i<parameterCount;++i)require(allowed[i]||after.values[i]==before.values[i],"Sketch modified occupied/unrelated field");
  auto appliedRaw=after;appliedRaw.macros={};RoutedPatch applied;require(makeRoutedPatch(appliedRaw,applied),"Sketch applied raw patch");
  double squared=0;int added=0;for(std::size_t band=0;band<8;++band)if(!base.bands[band].active&&applied.effects[slot].dynamicEq.bands[band].active){++added;require(applied.effects[slot].dynamicEq.bands[band].rangeDb==0,"Sketch retained inactive dynamics");}
  for(std::size_t i=0;i<129;++i){const auto response=dynamicEqResponse(applied.effects[slot].dynamicEq,rate,20.*std::pow(1000.,double(i)/128.));require(bool(response),"Sketch applied response");const auto error=target.db[i]-20.*std::log10(std::max(std::abs(*response),1e-12));squared+=error*error;}
  const auto actualError=std::sqrt(squared/129);require(added>0&&actualError<=1&&std::abs(actualError-previewError)<.001,"Sketch applied curve differs from preview");
  SoundDocument recalled;require(processor->undoSoundEdit()&&processor->trySoundSnapshot(recalled,revision)&&recalled==before,"Sketch fixture one undo");
  require(processor->redoSoundEdit()&&processor->trySoundSnapshot(recalled,revision)&&recalled==after,"Sketch fixture redo");
  ProjectDocument project=original;project.sound=after;juce::MemoryBlock bytes;require(encodeProjectState(project,bytes),"Sketch fixture project encode");
  const auto name=juce::String(std::array<const char*,3>{"broad-dip","broad-boost","tilt"}[std::size_t(variant)]);const auto file=root.getChildFile(name+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"Sketch fixture project save");
  auto item=object();put(item,"name",name);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());put(item,"added_bands",added);put(item,"preview_rms_db",previewError);put(item,"applied_rms_db",actualError);put(item,"one_undo_redo_exact",true);files.append(item);
 }
 juce::MemoryBlock afterInput;require(input.loadFileAsData(afterInput)&&afterInput==originalBytes,"Sketch input changed");
 put(report,"fixtures",files);put(report,"source","Same accepted reference project, occupied bands, gain controls, pattern and output in every file. Three fitted static curves applied through the native graph with one exact undo/redo each. Original input preserved; no audio normalization.");put(report,"success",true);
 require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"Sketch fixture report save");graph.retire();processor->releaseResources();return 0;
}
juce::File matchFixtureWave(const juce::File& root,const juce::String& name,const SoundDocument& sound){
 constexpr int rate=48000,frames=6*rate;RoutedPatch patch;require(makeRoutedPatch(sound,patch),"Match fixture patch");RoutedSynthEngine engine;
 require(engine.setPatch(patch)&&engine.prepare(rate,512,RenderQuality::realtime),"Match fixture engine");engine.reset();
 juce::AudioBuffer<float> wave(2,frames),block(2,512);juce::MidiBuffer midi;midi.ensureSize(1024);
 for(int offset=0;offset<frames;offset+=512){const int count=std::min(512,frames-offset);block.setSize(2,count,false,false,true);midi.clear();
  for(int note=0;note<4;++note){const auto pitch=std::array<int,4>{48,55,60,67}[std::size_t(note)];const int on=note*rate/2,off=on+int(.4*rate);
   if(on>=offset&&on<offset+count)midi.addEvent(juce::MidiMessage::noteOn(1,pitch,juce::uint8(100)),on-offset);
   if(off>=offset&&off<offset+count)midi.addEvent(juce::MidiMessage::noteOff(1,pitch),off-offset);}
  engine.render(block,midi);for(int channel=0;channel<2;++channel)wave.copyFrom(channel,offset,block,channel,0,count);
 }
 require(wave.getMagnitude(0,frames)<.85f,"Match measurement fixture must avoid output limiting");
 const auto file=root.getChildFile(name+"-native.wav");std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();require(bool(stream),"Match WAV stream");juce::WavAudioFormat format;
 auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
 require(writer&&writer->writeFromAudioSampleBuffer(wave,0,frames)&&writer->flush(),"Match fixture WAV");writer.reset();return file;
}
int matchFixtures(const char* path){
 const auto root=freshDirectory(path);SoundDocument source;for(auto& bank:source.compatibility.synth)bank.fill(modernSynthAlgorithmRevision);
 require(editRackLayout(source.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::dynamicEq},source.layout)==RackEditResult::applied,"Match fixture EQ");
 set(source,"global.master_level",.7f);set(source,"output.gain_db",24);set(source,"output.protection",1);set(source,"synth.1.enabled",1);set(source,"synth.1.engine",0);
 set(source,"synth.1.analog.noise",.8f);set(source,"synth.1.analog.osc.1.level",.2f);set(source,"synth.1.analog.osc.2.level",.1f);set(source,"synth.1.analog.filter.cutoff",20000);set(source,"synth.1.analog.filter.resonance",0);
 set(source,"synth.1.analog.amp.attack",.005f);set(source,"synth.1.analog.amp.decay",.05f);set(source,"synth.1.analog.amp.sustain",1);set(source,"synth.1.analog.amp.release",.1f);
 eqValue(source,0,0,0,1);eqValue(source,0,0,3,1000);eqValue(source,0,0,4,8);eqValue(source,0,0,5,.7071f);require(validateSoundDocument(source),"Match fixture source");
 auto report=object();juce::var files;const auto save=[&](const juce::String& name,const SoundDocument& sound){
  ProjectDocument project;project.sound=sound;project.mode=PlaybackMode::internalPattern;project.pattern.noteCount=4;
  for(int n=0;n<4;++n)project.pattern.notes[std::size_t(n)]={double(n),.8,std::array<std::uint8_t,4>{48,55,60,67}[std::size_t(n)],100,1};
  juce::MemoryBlock bytes;require(validateProjectDocument(project)&&encodeProjectState(project,bytes),"Match project encode");const auto file=root.getChildFile(name+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"Match project save");
  auto item=object();put(item,"name",name);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());files.append(item);
 };
 save("source",source);std::atomic_bool cancel{false};const auto sourceWave=matchFixtureWave(root,"source",source);const auto measuredSource=studio::analyzeEqReferenceWav(sourceWave,cancel);require(bool(measuredSource),"Match measured source WAV");
 companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.fixtureRoot=root;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");
 auto processor=std::make_unique<InstrumentProcessor>(offline);processor->prepareToPlay(48000,512);studio::DynamicEqGraph graph(*processor);graph.setBounds(0,0,1045,274);graph.setSpectrumOn(false);
 juce::MemoryBlock sourceState;require(encodeSoundState(source,sourceState),"Match source encode");juce::var examples;
 for(int variant=0;variant<3;++variant){
  const juce::String name=std::array<const char*,3>{"milder-boost","broad-boost","tilt"}[std::size_t(variant)];auto desired=source;
  if(variant==0)eqValue(desired,0,0,4,2);
  if(variant==1){eqValue(desired,0,1,0,1);eqValue(desired,0,1,3,3800);eqValue(desired,0,1,4,6);eqValue(desired,0,1,5,.7071f);}
  if(variant==2)for(std::size_t band:{1u,2u}){eqValue(desired,0,band,0,1);eqValue(desired,0,band,2,band==1?1.f:2.f);eqValue(desired,0,band,3,band==1?600.f:5000.f);eqValue(desired,0,band,4,band==1?-6.f:6.f);eqValue(desired,0,band,5,.7071f);}
  save(name+"-reference",desired);const auto wave=matchFixtureWave(root,name+"-reference",desired);const auto reference=studio::analyzeEqReferenceWav(wave,cancel);require(bool(reference),"Match measured reference WAV");
  juce::String encoded;require(studio::encodeEqReference(*reference,encoded)&&root.getChildFile(name+".eqref").replaceWithText(encoded),"Match explicit measured spectrum export");
  require(processor->restoreSoundState(sourceState.getData(),int(sourceState.getSize())),"Match fixture restore");graph.refresh(source,0);require(graph.previewMatch(*measuredSource,*reference,1),"Match measured preview");
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(graph.matchPreview().fitting&&std::chrono::steady_clock::now()<deadline){graph.updateMonitoring();juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}graph.updateMonitoring();const auto preview=graph.matchPreview();require(preview.canApply&&preview.rmsDb,"Match measured fit");
  SoundDocument before,after;std::uint64_t revision;require(processor->trySoundSnapshot(before,revision)&&before==source,"Match preview mutated sound");require(graph.applyMatch()&&processor->trySoundSnapshot(after,revision),"Match fixture Apply");
  require(after.macros==source.macros&&after.layout==source.layout&&after.controlLocks==source.controlLocks&&after.effectLocks==source.effectLocks,"Match unrelated intent");
  std::bitset<parameterCount> allowed;for(const auto index:unitReach(NativeModule::dynamicEq,0))for(int band=2;band<=8;++band)if(parameterRegistry()[index].id.startsWith("effect.1.dynamic_eq.band."+juce::String(band)+"."))allowed.set(index);
  for(std::size_t i=0;i<parameterCount;++i)require(allowed[i]||after.values[i]==source.values[i],"Match changed occupied or unrelated control");
  RoutedPatch a,b,wanted;require(makeRoutedPatch(source,a)&&makeRoutedPatch(after,b)&&makeRoutedPatch(desired,wanted),"Match independent patches");std::array<double,129> differences{},actual{};
  for(std::size_t i=0;i<129;++i){const auto hz=20*std::pow(1000.,double(i)/128);const auto x=dynamicEqEnergyResponse(a.effects[0].dynamicEq,48000,hz),y=dynamicEqEnergyResponse(b.effects[0].dynamicEq,48000,hz),z=dynamicEqEnergyResponse(wanted.effects[0].dynamicEq,48000,hz);require(x&&y&&z,"Match independent response");differences[i]=20*std::log10(*z/ *x);actual[i]=20*std::log10(*y/ *x);}
  auto sorted=differences;std::sort(sorted.begin(),sorted.end());double initial=0,error=0;for(std::size_t i=0;i<129;++i){const auto target=differences[i]-sorted[64];initial+=target*target;error+=std::pow(actual[i]-target,2);}
  const auto beforeRms=std::sqrt(initial/129),afterRms=std::sqrt(error/129);require(error<initial*.5&&afterRms<1,"Match measured reference did not improve independent curve");
  SoundDocument recalled;require(processor->undoSoundEdit()&&processor->trySoundSnapshot(recalled,revision)&&recalled==source,"Match one exact undo");require(processor->redoSoundEdit()&&processor->trySoundSnapshot(recalled,revision)&&recalled==after,"Match exact redo");
  save(name+"-matched",after);const auto matchedWave=matchFixtureWave(root,name+"-matched",after);auto item=object();put(item,"name",name);put(item,"before_rms_db",beforeRms);put(item,"after_rms_db",afterRms);put(item,"preview_rms_db",*preview.rmsDb);put(item,"added_bands",int(preview.added));put(item,"reference_wav_sha256",juce::SHA256(wave).toHexString());put(item,"matched_wav_sha256",juce::SHA256(matchedWave).toHexString());put(item,"one_undo_redo_exact",true);examples.append(item);
 }
 put(report,"files",files);put(report,"examples",examples);put(report,"source_wav_sha256",juce::SHA256(sourceWave).toHexString());put(report,"scope","Three measured WAV references from the same four-note noise/oscillator phrase and fixed output. Full spectra use the production bounded analyzer; native shared Match fitter/Apply preserves the occupied8dB band. Reference/matched projects are for actual VST3 replay. Magnitude matching does not transfer timbre; no per-render normalization.");put(report,"success",true);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"Match fixture report");graph.retire();processor->releaseResources();return 0;
}
int matchMusicalFixtures(const char* inputPath,const char* outputPath){
 const juce::File input{juce::String(inputPath)};const auto root=freshDirectory(outputPath);auto report=object();juce::var files;
 for(const auto* name:{"source","milder-boost-reference","milder-boost-matched","broad-boost-reference","broad-boost-matched","tilt-reference","tilt-matched"}){
  juce::MemoryBlock original;ProjectDocument project;require(input.getChildFile(juce::String(name)+".airackproject").loadFileAsData(original)&&decodeProjectState(original.getData(),original.getSize(),project),"Match musical input project");
  RoutedPatch before,after;require(makeRoutedPatch(project.sound,before),"Match musical original patch");
  set(project.sound,"synth.1.analog.noise",0);set(project.sound,"synth.1.analog.osc.1.level",.7f);set(project.sound,"synth.1.analog.osc.2.level",.3f);set(project.sound,"output.gain_db",18);
  require(makeRoutedPatch(project.sound,after)&&before.effects==after.effects&&before.layout==after.layout,"Musical examples must retain every EQ setting");
  juce::MemoryBlock bytes;require(validateProjectDocument(project)&&encodeProjectState(project,bytes),"Match musical encode");const auto file=root.getChildFile(juce::String(name)+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"Match musical save");
  const auto wave=matchFixtureWave(root,name,project.sound);auto item=object();put(item,"name",name);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());put(item,"native_wav_sha256",juce::SHA256(wave).toHexString());files.append(item);
 }
 put(report,"files",files);put(report,"success",true);put(report,"scope","Listening clarification: every fitted/reference EQ setting is unchanged. All seven projects use noise0,oscillator levels.7/.3 and common output18dB. The matcher was not rerun on this sparse musical source; its measured broadband reference trials remain separate. No per-file normalization.");require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"Match musical report");return 0;
}
int placementFixtures(const char* path){
 const auto root=freshDirectory(path);auto source=placementCapacityScene(2);
 for(std::uint8_t slot:{std::uint8_t(2),std::uint8_t(3)})require(editRackLayout(source.layout,{RackEditKind::removeSynth,slot},source.layout)==RackEditResult::applied,"placement fixture remove synth");
 for(std::uint8_t slot=1;slot<8;++slot)require(editRackLayout(source.layout,{RackEditKind::removeEffect,slot},source.layout)==RackEditResult::applied,"placement fixture remove EQ");
 for(std::size_t band=0;band<8;++band){eqValue(source,0,band,0,band<2?1.f:0.f);eqValue(source,0,band,7,0);source.values[dynamicEqPlacementParameterStart+band]=0;}
 eqValue(source,0,0,2,1);eqValue(source,0,0,3,600);eqValue(source,0,0,4,-6);eqValue(source,0,0,5,.7071f);
 eqValue(source,0,1,2,2);eqValue(source,0,1,3,3000);eqValue(source,0,1,4,6);eqValue(source,0,1,5,.7071f);
 auto report=object();juce::var files;
 for(int mode=0;mode<5;++mode){ProjectDocument project;project.sound=source;project.mode=PlaybackMode::internalPattern;project.pattern.noteCount=4;
  for(int n=0;n<4;++n)project.pattern.notes[std::size_t(n)]={double(n),.8,std::array<std::uint8_t,4>{48,55,60,67}[std::size_t(n)],100,1};
  set(project.sound,"effect.1.bypassed",mode==0?1.f:0.f);for(std::size_t band=0;band<8;++band)project.sound.values[dynamicEqPlacementParameterStart+band]=float(mode)/4;
  require(validateProjectDocument(project),"invalid placement listening fixture");juce::MemoryBlock bytes;require(encodeProjectState(project,bytes),"placement fixture encode");
  const juce::String name=std::array<const char*,5>{"reference","left","right","mid","side"}[std::size_t(mode)];const auto file=root.getChildFile(name+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"placement fixture save");
  auto item=object();put(item,"name",name);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());files.append(item);
 }
 put(report,"fixtures",files);put(report,"source","Same two opposite-panned analog/wavetable layers, four-note phrase, output and lowShelf-6dB@600/highShelf+6dB@3k in all files. Reference bypasses EQ; others change only placement/bypass. No per-render normalization.");put(report,"success",true);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"placement fixture report");return 0;
}
int shapeFixtures(const char* path){
 const auto root=freshDirectory(path);auto source=shapeCapacityScene(2);
 for(std::uint8_t slot:{std::uint8_t(2),std::uint8_t(3)})require(editRackLayout(source.layout,{RackEditKind::removeSynth,slot},source.layout)==RackEditResult::applied,"shape fixture remove synth");
 for(std::uint8_t slot=1;slot<8;++slot)require(editRackLayout(source.layout,{RackEditKind::removeEffect,slot},source.layout)==RackEditResult::applied,"shape fixture remove EQ");
 for(std::size_t band=0;band<8;++band){eqValue(source,0,band,0,band==0?1.f:0.f);eqValue(source,0,band,2,0);eqValue(source,0,band,4,12);eqValue(source,0,band,5,.7071f);
  source.values[dynamicEqExtensionParameterStart+band*2]=0;source.values[dynamicEqExtensionParameterStart+band*2+1]=0;source.values[dynamicEqPlacementParameterStart+band]=0;}
 auto report=object();juce::var files;
 const std::array<const char*,8> names{"reference","band-pass","tilt-shelf","flat-tilt","low-cut-6","high-cut-36","low-cut-48","high-cut-96"};
 for(int mode=0;mode<8;++mode){ProjectDocument project;project.sound=source;project.mode=PlaybackMode::internalPattern;project.pattern.noteCount=4;
  for(int n=0;n<4;++n)project.pattern.notes[std::size_t(n)]={double(n),.8,std::array<std::uint8_t,4>{48,55,60,67}[std::size_t(n)],100,1};
  set(project.sound,"effect.1.bypassed",mode==0?1.f:0.f);eqValue(project.sound,0,0,3,mode<4?1000.f:mode==4||mode==6?400.f:2000.f);
  if(mode>0&&mode<4)project.sound.values[dynamicEqExtensionParameterStart]=float(mode)/3;
  if(mode>=4){eqValue(project.sound,0,0,2,mode==4||mode==6?3.f:4.f);project.sound.values[dynamicEqExtensionParameterStart+1]=float(mode-3)/4;}
  require(validateProjectDocument(project),"invalid shape listening fixture");juce::MemoryBlock bytes;require(encodeProjectState(project,bytes),"shape fixture encode");
  const auto file=root.getChildFile(juce::String(names[std::size_t(mode)])+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"shape fixture save");
  auto item=object();put(item,"name",names[std::size_t(mode)]);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());files.append(item);
 }
 put(report,"fixtures",files);put(report,"source","Same two opposite-panned analog/wavetable layers,four-note phrase,output and one active band. Reference bypasses EQ; examples change only band shape/slope/frequency. Tilt/Flat gain+12dB,Q.7071. Retained dynamic range is ignored by static shapes. No per-render normalization.");
 put(report,"success",true);require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"shape fixture report");return 0;
}
int detectorFixtures(const char* path){
 const auto root=freshDirectory(path);auto source=placementCapacityScene(2);
 for(std::uint8_t slot:{std::uint8_t(2),std::uint8_t(3)})require(editRackLayout(source.layout,{RackEditKind::removeSynth,slot},source.layout)==RackEditResult::applied,"detector fixture remove synth");
 for(std::uint8_t slot=1;slot<8;++slot)require(editRackLayout(source.layout,{RackEditKind::removeEffect,slot},source.layout)==RackEditResult::applied,"detector fixture remove EQ");
 set(source,"output.gain_db",0);
 for(std::size_t band=0;band<8;++band){eqValue(source,0,band,0,band==0?1.f:0.f);eqValue(source,0,band,7,0);source.values[dynamicEqPlacementParameterStart+band]=0;}
 eqValue(source,0,0,2,2);eqValue(source,0,0,3,1400);eqValue(source,0,0,5,.7071f);eqValue(source,0,0,8,-36);eqValue(source,0,0,9,8);eqValue(source,0,0,10,200);
 set(source,"effect.1.dynamic_eq.band.1.detector_low_hz",60);set(source,"effect.1.dynamic_eq.band.1.detector_high_hz",180);
 const std::array<const char*,8> names{"reference","internal-band","internal-free","internal-auto-adaptive","external-band","external-free","external-expansion","external-auto-adaptive"};
 auto report=object();juce::var files;
 for(std::size_t mode=0;mode<names.size();++mode)for(bool higher:{false,true}){
  ProjectDocument project;project.sound=source;project.sound.values[758]=higher?1.f:0.f;project.mode=PlaybackMode::internalPattern;project.pattern.noteCount=4;
  for(int n=0;n<4;++n)project.pattern.notes[std::size_t(n)]={double(n),.8,std::array<std::uint8_t,4>{48,55,60,67}[std::size_t(n)],100,1};
  eqValue(project.sound,0,0,7,mode==0?0.f:mode==6?12.f:-12.f);
  set(project.sound,"effect.1.dynamic_eq.band.1.detector_source",mode<=1?0.f:mode<=3?1.f:mode==4?2.f:3.f);
  if(mode==3||mode==7){set(project.sound,"effect.1.dynamic_eq.band.1.auto_threshold",1);set(project.sound,"effect.1.dynamic_eq.band.1.adaptive_timing",1);}
  require(validateProjectDocument(project),"invalid detector listening fixture");juce::MemoryBlock bytes;require(encodeProjectState(project,bytes),"detector fixture encode");
  const juce::String name=juce::String(names[mode])+(higher?"-higher":"-realtime");const auto file=root.getChildFile(name+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"detector fixture save");
  auto item=object();put(item,"name",name);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());put(item,"quality",higher?"higher":"realtime");files.append(item);
 }
 juce::AudioBuffer<float> trigger(2,6*48000);trigger.clear();
 for(int n=0;n<4*48000;++n){const double t=double(n)/48000,phase=std::fmod(t,.75);const double envelope=phase<.4?std::min({1.,phase/.01,(.4-phase)/.01}):0.;const float value=float(.5*envelope*std::sin(2*std::numbers::pi*100*t));trigger.setSample(0,n,value);trigger.setSample(1,n,-.7f*value);}
 const auto wav=root.getChildFile("bass-trigger-100hz-pulses-then-silence.wav");std::unique_ptr<juce::OutputStream> stream=wav.createOutputStream();require(bool(stream),"detector trigger WAV stream");juce::WavAudioFormat format;
 auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
 require(writer&&writer->writeFromAudioSampleBuffer(trigger,0,trigger.getNumSamples())&&writer->flush(),"detector trigger WAV write");writer.reset();
 put(report,"fixtures",files);put(report,"sidechain",wav.getFullPathName());put(report,"sidechain_sha256",juce::SHA256(wav).toHexString());put(report,"source","Same two panned analog/wavetable layers, four-note phrase, 1400Hz high shelf, output and manual base controls. Variants change only range/source/auto/adaptive and render quality. External stimulus: stereo100Hz pulses for4s followed by2s connected silence;60..180Hz free filter. No normalization. Render external variants both with this WAV and without auxiliary input.");put(report,"success",true);
 require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"detector fixture report");return 0;
}
int fixtures(const char* path,bool gainComparison=false){
 const auto root=freshDirectory(path);const auto designs=airack::test::frozenFactoryRackDesigns();SoundDocument original,placed;RackDifference delta;
 for(const auto* name:{"Copper Arc","Contour Dynamics"}){const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.name==name;});require(found!=designs.end(),"factory design missing");RackOperation op;op.design=*found;op.newInstance=juce::Uuid();op.destination={found->role,masterChain,0};op.allocation={{0,0}};op.allowSlotReuse=true;require(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready,"factory placement");original=placed;}
 set(original,"output.gain_db",12);set(original,"output.protection",1);
 for(std::size_t b=0;b<3;++b){eqValue(original,0,b,0,1);eqValue(original,0,b,3,std::array<float,3>{250,1800,6500}[b]);eqValue(original,0,b,4,std::array<float,3>{-3,2,-2}[b]);eqValue(original,0,b,5,.8f);eqValue(original,0,b,8,-55);eqValue(original,0,b,9,8);eqValue(original,0,b,10,140);}
 eqValue(original,0,7,3,17800);eqValue(original,0,7,7,-5);eqValue(original,0,7,8,-33);eqValue(original,0,7,10,1730);eqValue(original,7,7,3,17777);eqValue(original,7,7,10,1729);
 auto report=object();juce::var files;for(int variant=0;variant<4;++variant){ProjectDocument project;project.sound=original;project.mode=PlaybackMode::internalPattern;project.pattern.noteCount=4;const std::array<std::uint8_t,4> pitches{48,55,60,67},velocities{100,78,110,88};for(int n=0;n<4;++n)project.pattern.notes[std::size_t(n)]={double(n),.8,pitches[std::size_t(n)],velocities[std::size_t(n)],1};
  if(gainComparison){set(project.sound,"effect.1.dynamic_eq.auto_gain",variant==3?1.f:0.f);set(project.sound,"effect.1.dynamic_eq.gain_scale",std::array<float,4>{1,.5f,1.5f,1.5f}[std::size_t(variant)]);}
  else {if(variant==0)set(project.sound,"effect.1.bypassed",1);if(variant>=2)for(std::size_t b=0;b<3;++b)eqValue(project.sound,0,b,7,variant==2?-9.f:9.f);}
  require(validateProjectDocument(project),"invalid listening fixture");juce::MemoryBlock bytes;require(encodeProjectState(project,bytes),"listening fixture encode");
  const auto name=juce::String((gainComparison?std::array<const char*,4>{"reference","scale-half","scale-strong","scale-strong-compensated"}:std::array<const char*,4>{"dry","static","compression","expansion"})[std::size_t(variant)]);
  const auto file=root.getChildFile(name+".airackproject");require(file.replaceWithData(bytes.getData(),bytes.getSize()),"listening fixture save");auto item=object();put(item,"name",name);put(item,"file",file.getFullPathName());put(item,"sha256",juce::SHA256(bytes).toHexString());files.append(item);
 }
 put(report,"fixtures",files);put(report,"source",gainComparison?"Same Copper Arc/Contour Dynamics, raw bands, four-note phrase and +12dB output in every file. Only saved Scale/AutoGain differ:1/off,.5/off,1.5/off,1.5/on. No per-render normalization.":"Unchanged Copper Arc factory definition, Contour Dynamics factory definition, same four-note internal phrase and +12dB output for every pair; no per-render normalization.");require(root.getChildFile("report.json").replaceWithText(juce::JSON::toString(report)),"fixture report save");return 0;
}

}
int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);juce::ScopedJuceInitialiser_GUI juce;try{if(argc==3&&juce::String(argv[1])=="--spectral-routed-functional")return profileRoutedSpectral(argv[2],true);if(argc==3&&juce::String(argv[1])=="--spectral-routed-screen")return profileRoutedSpectral(argv[2],false,0,true);if(argc==4&&juce::String(argv[1])=="--spectral-routed-profile")return profileRoutedSpectral(argv[2],false,std::stoi(argv[3]));if(argc==3&&juce::String(argv[1])=="--spectral-bounded-diagnostic")return profileSpectral(argv[2],false,0,4096,true,true,true,true);if(argc==3&&juce::String(argv[1])=="--spectral-bounded-functional")return profileSpectral(argv[2],true,0,4096,true,false,true);if(argc==3&&juce::String(argv[1])=="--spectral-bounded-screen")return profileSpectral(argv[2],false,0,4096,true,true,true);if(argc==4&&juce::String(argv[1])=="--spectral-bounded-profile")return profileSpectral(argv[2],false,std::stoi(argv[3]),4096,true,false,true);if(argc==4&&juce::String(argv[1])=="--spectral-window-functional")return profileSpectral(argv[2],true,0,std::stoi(argv[3]),true);if(argc==4&&juce::String(argv[1])=="--spectral-window-screen")return profileSpectral(argv[2],false,0,std::stoi(argv[3]),true,true);if(argc==4&&juce::String(argv[1])=="--spectral-window-profile")return profileSpectral(argv[2],false,0,std::stoi(argv[3]),true);if(argc==3&&juce::String(argv[1])=="--profile-spectral")return profileSpectral(argv[2]);if(argc==4&&juce::String(argv[1])=="--profile-spectral-stages")return profileSpectral(argv[2],false,std::stoi(argv[3]));if(argc==3&&juce::String(argv[1])=="--spectral-functional")return profileSpectral(argv[2],true);if(argc==3&&juce::String(argv[1])=="--profile-match")return profileMatch(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-match-audit")return profileMatch(argv[2],true);if(argc==3&&juce::String(argv[1])=="--match-functional")return profileMatch(argv[2],false,true);if(argc==2&&juce::String(argv[1])=="--match-workload-self-test"){matchWorkloadAdmission();std::cout<<"Match workload: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==4&&juce::String(argv[1])=="--match-musical-fixtures")return matchMusicalFixtures(argv[2],argv[3]);if(argc==3&&juce::String(argv[1])=="--match-fixtures")return matchFixtures(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-overview")return profileOverview(argv[2]);if(argc==3&&juce::String(argv[1])=="--overview-functional")return profileOverview(argv[2],false,true);if(argc==3&&juce::String(argv[1])=="--profile-overview-audit")return profileOverview(argv[2],true);if(argc==3&&juce::String(argv[1])=="--profile-detectors")return profileDetectors(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-detectors-audit")return profileDetectors(argv[2],true);if(argc==3&&juce::String(argv[1])=="--detectors-functional")return profileDetectors(argv[2],false,true);if(argc==2&&juce::String(argv[1])=="--detector-workload-self-test"){fftWindowActivityProof();detectorWorkloadAdmission();std::cout<<"Detector workload: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--detector-fixtures")return detectorFixtures(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-shapes")return profileShapes(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-shapes-audit")return profileShapes(argv[2],true);if(argc==3&&juce::String(argv[1])=="--shapes-functional")return profileShapes(argv[2],false,true);if(argc==3&&juce::String(argv[1])=="--shape-fixtures")return shapeFixtures(argv[2]);if(argc==2&&juce::String(argv[1])=="--shapes-workload-self-test"){shapeWorkloadAdmission();std::cout<<"Shape workload: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--profile-placement")return profilePlacement(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-placement-audit")return profilePlacement(argv[2],true);if(argc==3&&juce::String(argv[1])=="--placement-fixtures")return placementFixtures(argv[2]);if(argc==2&&juce::String(argv[1])=="--placement-workload-self-test"){placementWorkloadAdmission();std::cout<<"Placement workload: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--stress-sketch")return profileSketch(argv[2],false,true);if(argc==4&&juce::String(argv[1])=="--sketch-fixtures")return sketchFixtures(argv[2],argv[3]);if(argc==2&&juce::String(argv[1])=="--sketch-workload-self-test"){sketchWorkloadAdmission();std::cout<<"Sketch workload: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--gain-workload-self-test"){gainWorkloadAdmission();std::cout<<"Gain workload: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--profile-sketch-audit")return profileSketch(argv[2],true);if(argc==3&&juce::String(argv[1])=="--profile-sketch")return profileSketch(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-gain-audit")return profileGain(argv[2],true);if(argc==3&&juce::String(argv[1])=="--profile-gain")return profileGain(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-groups-audit")return profileGroups(argv[2],true);if(argc==3&&juce::String(argv[1])=="--profile-groups")return profileGroups(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-grab")return profileGrab(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-stages-morph")return profileStages(argv[2],StageModulation::morph);if(argc==3&&juce::String(argv[1])=="--profile-stages-modulated")return profileStages(argv[2],StageModulation::cutoff);if(argc==3&&juce::String(argv[1])=="--profile-stages")return profileStages(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile")return profile(argv[2]);if(argc==3&&juce::String(argv[1])=="--profile-single")return profile(argv[2],false,true,false,true);if(argc==3&&juce::String(argv[1])=="--profile-continuous")return profile(argv[2],false,false);if(argc==3&&juce::String(argv[1])=="--profile-audit")return profile(argv[2],false,false,true);if(argc==3&&juce::String(argv[1])=="--profile-bypass")return profile(argv[2],true);if(argc==3&&juce::String(argv[1])=="--gain-fixtures")return fixtures(argv[2],true);if(argc==3&&juce::String(argv[1])=="--fixtures")return fixtures(argv[2]);if(argc!=1)return 2;matchWorkloadAdmission();fftWindowActivityProof();detectorWorkloadAdmission();shapeWorkloadAdmission();sketchWorkloadAdmission();gainWorkloadAdmission();downsamplerBound();doubleHeadroom();hostFloatHeadroom();shelfHeadroom();std::cout<<"Dynamic EQ product: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
