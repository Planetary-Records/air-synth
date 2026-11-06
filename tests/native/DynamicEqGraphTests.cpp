#include "FrozenRackExamples.h"
#include "DynamicEqGraph.h"
#include "EqOverview.h"
#include "EqOverviewPeers.h"
#include "EqCurveFit.h"
#include "EqReferenceAnalysis.h"
#include "EqMatchPanel.h"
#include <chrono>
#include <thread>
#include "RackStack.h"
#include "AllControlsPanel.h"
#include "InstrumentProcessor.h"
#include "RackLibrary.h"
#include "RackExamples.h"
#include <algorithm>
#include "RackReconciliation.h"
#include "DynamicEqEdits.h"
#include "UserReach.h"
#include "WheelLatch.h"
#include <iostream>
#include <cmath>
#include <bit>
#include <bitset>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace airack;
using studio::DynamicEqGraph;
int checks=0,failures=0;juce::File images;
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool auditHiddenPoll=false;
thread_local std::size_t hiddenAllocations=0,hiddenFrees=0;
int __cdecl hiddenPollHeapHook(int type,void*,std::size_t,int,long,const unsigned char*,int){
 if(auditHiddenPoll){if(type==_HOOK_ALLOC||type==_HOOK_REALLOC)++hiddenAllocations;if(type==_HOOK_FREE)++hiddenFrees;}return 1;
}
#endif
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<'\n';}}while(false)
void snapshotImage(const char*,juce::Component&);
void overlapRequiresTwoAudibleSpectra(){
 std::cout<<"SECTION overview_overlap_requires_two_audible_spectra\n";
 std::array<float,128> a,b;a.fill(-120);b.fill(-120);CHECK(studio::potentialEqOverlap(a,b).none());
 a[40]=-24;b[40]=-30;CHECK(studio::potentialEqOverlap(a,b).test(40));
 b[40]=-72;CHECK(!studio::potentialEqOverlap(a,b).test(40));b[40]=-36;CHECK(studio::potentialEqOverlap(a,b).test(40));
 b[40]=-36.01f;CHECK(!studio::potentialEqOverlap(a,b).test(40));
 b[40]=std::numeric_limits<float>::infinity();CHECK(studio::potentialEqOverlap(a,b).none());
 b[40]=-30;a[40]=std::numeric_limits<float>::quiet_NaN();CHECK(studio::potentialEqOverlap(a,b).none());
}
void overviewSamplingPreservesNarrowPeaksAndNyquist(){
 std::cout<<"SECTION overview_summary_keeps_narrow_peaks_without_aliasing\n";
 auto r=std::make_unique<studio::FxSpectrum::Result>();r->valid=true;r->sampleRate=48000;r->inputDb.fill(-120);r->outputDb.fill(-120);r->outputDb[4096]=-18;
 auto levels=studio::sampleEqSpectrum(*r,true);CHECK(*std::max_element(levels.begin(),levels.end())==-18);CHECK(studio::sampleEqSpectrum(*r,false)[100]==-120);
 r->sampleRate=16000;r->outputDb.fill(-120);r->outputDb.back()=-12;levels=studio::sampleEqSpectrum(*r,true);CHECK(*std::max_element(levels.begin(),levels.end())==-12);CHECK(levels.back()==-120);
 r->valid=false;levels=studio::sampleEqSpectrum(*r,true);CHECK(std::all_of(levels.begin(),levels.end(),[](float value){return value==-120;}));
}
juce::Component* findComponent(juce::Component&,const juce::String&);
bool clickComparisonKey(juce::Button&);
bool bandMenuCommand(DynamicEqGraph&,int);
bool pumpUntil(const std::function<bool()>& ready){const auto until=juce::Time::getMillisecondCounterHiRes()+3000;while(!ready()&&juce::Time::getMillisecondCounterHiRes()<until)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);return ready();}
struct Notifications final:juce::AudioProcessorParameter::Listener {
 explicit Notifications(juce::AudioProcessorParameter& p):parameter(p){p.addListener(this);}
 ~Notifications() override{parameter.removeListener(this);}
 void parameterValueChanged(int,float) override{++values;if(onValue)onValue();}
 void parameterGestureChanged(int,bool start) override{if(start){++starts;if(onStart)onStart();}else{++ends;if(onEnd)onEnd();}}
 std::function<void()> onStart,onValue,onEnd;
 juce::AudioProcessorParameter& parameter;int starts=0,ends=0,values=0;
};
juce::MouseEvent mouse(juce::Component& c,juce::Point<float> at,juce::Point<float> start,bool held=true,int modifiers=0) {
 const auto time=juce::Time::getCurrentTime();return {juce::Desktop::getInstance().getMainMouseSource(),at,
  juce::ModifierKeys((held?juce::ModifierKeys::leftButtonModifier:0)|modifiers),1,0,0,0,0,&c,&c,time,start,time,1,held};
}
struct Rig {
 std::unique_ptr<InstrumentProcessor> processor;
 std::unique_ptr<DynamicEqGraph> graph;
 bool closing=false;
 std::size_t base=dynamicEqParameterStart+7*dynamicEqParametersPerSlot;
 Rig(DynamicEqGraph::Clipboard clipboard={},bool desktop=true,std::optional<companion::ClientOptions> group={}){
  companion::ClientOptions offline;offline.allowLaunch=false;
  auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
  offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=root;
  if(group){offline=*group;root=offline.launch.fixtureRoot;}
  processor=std::make_unique<InstrumentProcessor>(offline);
  const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.name=="Contour Dynamics";});CHECK(found!=designs.end());
  SoundDocument original,placed;RackOperation operation;RackDifference delta;operation.newInstance=juce::Uuid();
  if(found!=designs.end())operation.design=*found;operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,7}};
  CHECK(makeRackCandidate(original,operation,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
  juce::MemoryBlock wire;CHECK(encodeSoundState(placed,wire));CHECK(processor->restoreSoundState(wire.getData(),int(wire.getSize())));
  studio::house::Host host;host.closing=[this]{return closing;};host.change=[this](const auto& edit,const auto&){SoundDocument d;std::uint64_t revision;CHECK(processor->trySoundSnapshot(d,revision));edit(d);CHECK(processor->applySoundEdit(d,SoundEditOrigin::manual,revision));refresh();};
  graph=std::make_unique<DynamicEqGraph>(*processor,host,std::move(clipboard));graph->onEdited=[this]{refresh();};graph->setBounds(40,40,1045,274);if(desktop){graph->addToDesktop(juce::ComponentPeer::windowIsTemporary);graph->setVisible(true);}refresh();
  // The shipped rack starts empty. This fixture activates its first band through
  // the same double-click gesture before response/drag assertions begin.
  CHECK(sound().values[base]==0);const auto plot=graph->plotBounds();const juce::Point<float> at{plot.getX()+plot.getWidth()*float(std::log(50.)/std::log(1000.)),plot.getCentreY()};graph->mouseDoubleClick(mouse(*graph,at,at));CHECK(sound().values[base]==1);
 }
 SoundDocument sound(){SoundDocument d;std::uint64_t revision;CHECK(processor->trySoundSnapshot(d,revision));return d;}
 void refresh(){if(graph)graph->refresh(sound(),7);}
};
void matchCancelDoesNotWriteSound(){
 std::cout<<"SECTION match_cancel_never_writes_sound\n";Rig rig;const auto before=rig.sound();
 studio::EqReference source,reference;source.sampleRate=reference.sampleRate=48000;source.valid.set();reference.valid.set();source.spectrum.db.fill(-30);reference.spectrum.db.fill(-30);
 for(std::size_t i=0;i<129;++i)reference.spectrum.db[i]+=6*std::exp(-.5f*std::pow(float(int(i)-64)/10,2));
 CHECK(rig.graph->previewMatch(source,reference,1));CHECK(rig.sound()==before);rig.graph->cancelMatch();CHECK(!rig.graph->applyMatch());CHECK(rig.sound()==before);
}
void matchPanelRequiresExplicitContinuousCapture(){
 std::cout<<"SECTION match_panel_explicit_capture_and_stream_duration\n";Rig rig({},false);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 studio::EqMatchPanel panel(*rig.processor,*model,rig.graph->analysisHub());panel.bind(*rig.graph);panel.setVisible(true);const auto before=rig.sound();
 CHECK(!panel.sourceReference());CHECK(!panel.preview());CHECK(!panel.selectPeer(GroupUuid{}));CHECK(panel.learn(false,false));CHECK(panel.learning());CHECK(!panel.stopLearning());
 auto monitor=rig.processor->fxMonitorHandle();FxMonitor::ActiveSlots active{};active[7]=true;CHECK(monitor->beginBlock(48000,active));std::uint32_t random=123;
 for(int block=0;block<64;++block){for(int n=0;n<2048;++n){random=random*1664525+1013904223;const double x=(double(random)/4294967296.*2-1)*.2;monitor->capture(7,{x,-x},{x*.5,-x*.5});}rig.graph->analysisHub()->update();panel.updateMonitoring();}
 CHECK(panel.learnedSeconds()>=2&&panel.learnedSeconds()<=10);CHECK(panel.stopLearning());CHECK(panel.sourceReference());CHECK(rig.sound()==before);
 panel.setVisible(false);CHECK(rig.graph->analysisHub()->activeAnalyzers()==0);panel.retire();CHECK(!panel.learn(false,false));
}
void overviewOwnsEightSharedLeasesOnlyWhileVisible(){
 std::cout<<"SECTION overview_eight_rows_shared_consumption_and_retirement\n";
 Rig rig({},false);rig.graph.reset();auto d=rig.sound();std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));
 for(std::uint8_t slot=0;slot<8;++slot){d.layout.effects[slot]={SlotUse::present,EffectKind::dynamicEq,masterChain};d.layout.effectOrder[masterChain][slot]=slot;}d.layout.effectCount[masterChain]=8;
 CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));const auto before=rig.sound();
 auto monitor=rig.processor->fxMonitorHandle();auto hub=std::make_shared<studio::FxAnalysisHub>(monitor);
 juce::Component window;window.setBounds(0,0,1080,820);window.addToDesktop(juce::ComponentPeer::windowIsTemporary);window.setVisible(true);
 studio::EqOverview overview(*rig.processor,*studio::studioSkin(),hub);window.addAndMakeVisible(overview);overview.setBounds(0,0,1080,526);overview.refresh(before);
 CHECK(overview.localCount()==8);CHECK(hub->activeAnalyzers()==8);CHECK(overview.setReferenceSlot(7));CHECK(overview.referenceSlot()==7);CHECK(overview.setComparisonSlot(0));CHECK(!overview.setReferenceSlot(8));
 CHECK(!overview.setGroupEnabled(true));CHECK(overview.peerCount()==0);CHECK(overview.statusText().containsIgnoreCase("off"));
 auto graph=std::make_unique<DynamicEqGraph>(*rig.processor,studio::house::Host{},DynamicEqGraph::Clipboard{},hub);window.addAndMakeVisible(*graph);graph->setBounds(0,530,1045,274);graph->refresh(before,7);graph->updateMonitoring();CHECK(hub->activeAnalyzers()==8);
 FxMonitor::ActiveSlots active;active.fill(true);CHECK(monitor->beginBlock(48000,active));
 for(int n=0;n<studio::FxSpectrum::fftSize;++n)for(std::uint8_t slot=0;slot<8;++slot){const double sample=.1*std::sin(2*juce::MathConstants<double>::pi*1000*n/48000);monitor->capture(slot,{sample,sample},{sample*.5,sample*.5});}
 const auto queued=monitor->stats(7).queued;overview.updateMonitoring();CHECK(monitor->stats(7).queued==queued);hub->update();overview.updateMonitoring();graph->updateMonitoring();CHECK(hub->packetsLastTick()==512);
 for(std::uint8_t slot=0;slot<8;++slot)CHECK(overview.localAvailable(slot));CHECK(graph->spectrum().valid);const auto levels=studio::sampleEqSpectrum(graph->spectrum(),true);CHECK(*std::max_element(levels.begin(),levels.end())>-40);
 CHECK(overview.potentialOverlap()&&overview.potentialOverlap()->any());juce::MouseWheelDetails zoom;zoom.deltaY=1;
 overview.mouseWheelMove(mouse(overview,{overview.plotBounds().getX(),overview.plotBounds().getCentreY()},{},false),zoom);
 CHECK(overview.potentialOverlap()&&overview.potentialOverlap()->none());overview.mouseDoubleClick(mouse(overview,overview.plotBounds().getCentre(),{},false));CHECK(overview.potentialOverlap()&&overview.potentialOverlap()->any());
 const auto windows=graph->spectrum().windows;overview.updateMonitoring();CHECK(graph->spectrum().windows==windows);overview.setVisible(false);CHECK(hub->activeAnalyzers()==1);overview.setVisible(true);CHECK(hub->activeAnalyzers()==8);
 overview.retire();CHECK(hub->activeAnalyzers()==1);overview.refresh(before);overview.updateMonitoring();CHECK(hub->activeAnalyzers()==1);graph->retire();CHECK(hub->activeAnalyzers()==0);CHECK(rig.sound()==before);
}
void retiredOverviewDoesNotBorrowProcessor(){
 std::cout<<"SECTION retired_overview_and_queued_controls_are_inert\n";Rig rig({},false);rig.graph.reset();
 auto d=rig.sound();studio::EqOverview overview(*rig.processor,*studio::studioSkin(),std::make_shared<studio::FxAnalysisHub>(rig.processor->fxMonitorHandle()));overview.setSize(1080,526);overview.refresh(d);
 auto* group=dynamic_cast<juce::Button*>(findComponent(overview,"eq.overview.group"));CHECK(group);if(!group)return;const auto queued=group->onClick;overview.retire();
 CHECK(!group->onClick);CHECK(overview.statusText().containsIgnoreCase("closed"));
 if(!group->onClick&&overview.statusText().containsIgnoreCase("closed")){rig.processor.reset();if(queued)queued();overview.refresh(d);overview.updateMonitoring();CHECK(!overview.potentialOverlap());CHECK(overview.createComponentSnapshot(overview.getLocalBounds()).isValid());}
}
void sharedGraphsDoNotConsumeOrRetainAudition(){
 std::cout<<"SECTION shared_graph_hub_single_consumer_and_view_authority\n";
 Rig rig;const auto before=rig.sound();rig.graph.reset();
 auto monitor=rig.processor->fxMonitorHandle();auto hub=std::make_shared<studio::FxAnalysisHub>(monitor);
 auto graph=std::make_unique<DynamicEqGraph>(*rig.processor,studio::house::Host{},DynamicEqGraph::Clipboard{},hub);
 auto second=std::make_unique<DynamicEqGraph>(*rig.processor,studio::house::Host{},DynamicEqGraph::Clipboard{},hub);
 juce::Component window;window.setBounds(0,0,1080,600);window.addToDesktop(juce::ComponentPeer::windowIsTemporary);window.setVisible(true);
 window.addAndMakeVisible(*graph);window.addAndMakeVisible(*second);graph->setBounds(0,0,1045,274);second->setBounds(0,280,1045,274);
 for(auto* view:{graph.get(),second.get()}){view->refresh(before,7);view->updateMonitoring();CHECK(view->subscription());}
 const auto firstLease=graph->subscription(),secondLease=second->subscription(),overview=hub->acquire(7);CHECK(firstLease&&secondLease&&overview);if(!firstLease||!secondLease||!overview)return;
 CHECK(*firstLease!=*secondLease&&hub->activeAnalyzers()==1);
 FxMonitor::ActiveSlots active{};active[7]=true;CHECK(monitor->beginBlock(48000,active));
 for(int n=0;n<studio::FxSpectrum::fftSize;++n)monitor->capture(7,{.1,-.1},{.05,-.05});
 const auto queued=monitor->stats(7).queued;graph->updateMonitoring();second->updateMonitoring();CHECK(monitor->stats(7).queued==queued);
 hub->update();CHECK(hub->packetsLastTick()==64);CHECK(graph->spectrum().valid&&graph->spectrum().windows==1);CHECK(&graph->spectrum()==&second->spectrum());CHECK(&graph->spectrum()==hub->result(*overview));
 graph->updateMonitoring();second->updateMonitoring();CHECK(graph->spectrum().windows==1);
 CHECK(hub->requestAudition(*firstLease,0));graph->retire();CHECK(!hub->requestAudition(*firstLease,1));CHECK(!hub->requestedAudition(*secondLease));CHECK(second->spectrum().valid&&hub->activeAnalyzers()==1);
 CHECK(hub->requestAudition(*secondLease,0));hub->release(*overview);CHECK(hub->requestedAudition(*secondLease)==0);CHECK(hub->activeAnalyzers()==1);
 const auto retained=hub->acquire(7);CHECK(retained);hub->retire();CHECK(!second->subscription()&&hub->activeAnalyzers()==0);CHECK(!hub->requestAudition(*secondLease,0));
 CHECK(rig.sound()==before);second->retire();rig.processor.reset(); // Queued retired views hold no processor-dependent analyzer.
 hub->update();CHECK(!hub->result(*secondLease));graph.reset();second.reset();
}
void selectionCopiesWithoutChangingAudioState(){
 std::cout<<"SECTION raw_selected_band_copy_and_atomic_paste\n";Rig rig;auto& graph=*rig.graph;auto& p=*rig.processor;
 const auto before=rig.sound();graph.setSelectedBands(std::bitset<8>(1));CHECK(graph.selectedBands().count()==1);
 juce::String text;const bool copied=graph.copySelectedBands(text);CHECK(copied);std::vector<EqBandPatch> bands;
 const bool decoded=decodeEqClipboard(text,bands);CHECK(decoded);CHECK(bands.size()==1);CHECK(rig.sound()==before);
 EqPatch paste;SoundDocument expected;if(decoded&&makeEqPaste(before,7,bands,paste)&&makeEqEdit(before,7,paste,expected)){
  Notifications active(p.control(rig.base+11)),frequency(p.control(rig.base+14));
  CHECK(graph.pasteBands(text));CHECK(rig.sound()==expected);CHECK(active.starts==1&&active.ends==1);CHECK(frequency.starts==1&&frequency.ends==1);
  CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);CHECK(p.redoSoundEdit());CHECK(rig.sound()==expected);
 }
 const auto after=rig.sound();CHECK(!graph.pasteBands("{}"));CHECK(rig.sound()==after);
 graph.setSelectedBands({});juce::String untouched="unchanged";CHECK(!graph.copySelectedBands(untouched));CHECK(untouched=="unchanged");
}
void install(Rig& rig,const SoundDocument& sound){
 juce::MemoryBlock wire;CHECK(encodeSoundState(sound,wire));CHECK(rig.processor->restoreSoundState(wire.getData(),int(wire.getSize())));rig.refresh();
}
std::pair<studio::EqReference,studio::EqReference> matchReferences(){
 studio::EqReference source,reference;source.sampleRate=48000;source.valid.set();source.spectrum.db.fill(-36);source.label="Source fixture";source.captureId="source-fixture";reference=source;reference.label="Broad reference";reference.captureId="reference-fixture";
 for(std::size_t i=0;i<129;++i)reference.spectrum.db[i]+=6*std::exp(-.5f*std::pow(float(int(i)-64)/10,2));return {source,reference};
}
bool waitForMatch(DynamicEqGraph& graph){
 const auto until=juce::Time::getMillisecondCounterHiRes()+8000;while(graph.matchPreview().fitting&&juce::Time::getMillisecondCounterHiRes()<until){graph.updateMonitoring();juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}graph.updateMonitoring();CHECK(!graph.matchPreview().fitting);return graph.matchPreview().canApply;
}
void matchApplyPreservesOccupiedBandsAndMacros(){
 std::cout<<"SECTION match_absolute_target_effective_macros_and_atomic_apply\n";Rig rig({},false);auto before=rig.sound();
 before.macros[0]={parameterRegistry()[rig.base+4].stateKey,.58f};before.macros[1]={parameterRegistry()[dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot+1].stateKey,.65f};
 before.values[1]=.6f;before.values[2]=.8f;before.values[3]=.5f;
 before.controlLocks[rig.base+11+3]=true;before.macros[2]={parameterRegistry()[rig.base+22+4].stateKey,.55f};install(rig,before);
 auto [source,reference]=matchReferences();const auto desired=studio::eqMatchCorrection(source,reference,1);CHECK(desired);if(!desired)return;
 Notifications starts(rig.processor->control(rig.base+33));CHECK(rig.graph->previewMatch(source,reference,1));CHECK(!rig.graph->sketchEnabled());CHECK(!rig.graph->applySketch());CHECK(rig.sound()==before);
 CHECK(waitForMatch(*rig.graph));const auto preview=rig.graph->matchPreview();CHECK(preview.target&&preview.fit&&preview.rmsDb);CHECK(preview.available==5);if(preview.target)for(std::size_t i=0;i<129;++i)CHECK(std::abs(preview.target->db[i]-desired->db[i])<1e-5f);
 CHECK(rig.graph->applyMatch());const auto after=rig.sound();CHECK(after!=before);CHECK(after.macros==before.macros&&after.controlLocks==before.controlLocks&&after.effectLocks==before.effectLocks&&after.layout==before.layout);
 for(std::size_t i=0;i<parameterCount;++i)if(i<rig.base+33||i>=rig.base+88)CHECK(after.values[i]==before.values[i]);
 RoutedPatch a,b;CHECK(makeRoutedPatch(before,a)&&makeRoutedPatch(after,b));double error=0,initial=0;
 for(std::size_t i=0;i<129;++i){const auto hz=20*std::pow(1000.,double(i)/128);const auto x=dynamicEqEnergyResponse(a.effects[7].dynamicEq,48000,hz),y=dynamicEqEnergyResponse(b.effects[7].dynamicEq,48000,hz);CHECK(x&&y);if(x&&y){const auto delta=20*std::log10(*y/ *x);error+=std::pow(delta-desired->db[i],2);initial+=std::pow(desired->db[i],2);}}
 CHECK(error<initial*.5);CHECK(starts.starts==1&&starts.ends==1);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);CHECK(rig.processor->redoSoundEdit());CHECK(rig.sound()==after);CHECK(!rig.graph->applyMatch());
 std::cout<<"MATCH_INDEPENDENT_RMS before="<<std::sqrt(initial/129)<<" after="<<std::sqrt(error/129)<<'\n';
}
void matchRejectsStaleReviewFullAndFlat(){
 std::cout<<"SECTION match_stale_reset_rebind_review_and_capacity\n";Rig rig({},false);const auto before=rig.sound();auto [source,reference]=matchReferences();
 for(int reason=0;reason<5;++reason){install(rig,before);CHECK(rig.graph->previewMatch(source,reference,1));
  if(reason==0)rig.processor->control(0).setValueNotifyingHost(.31f);if(reason==1)install(rig,before);if(reason==2)rig.graph->refresh(before,0);if(reason==3)rig.graph->showProposal(std::make_shared<SoundDocument>(before));if(reason==4)rig.graph->cancelMatch();
  const auto expected=rig.sound();CHECK(!rig.graph->applyMatch());CHECK(rig.sound()==expected);rig.graph->updateMonitoring();CHECK(!rig.graph->matchPreview().canApply);rig.graph->showProposal(nullptr);
 }
 for(int reason=0;reason<3;++reason){auto d=before;if(reason==0)d.effectLocks[7]=true;if(reason==1)for(int band=0;band<8;++band)d.values[rig.base+band*11]=1;if(reason==2)d.values[dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot+1]=0;install(rig,d);CHECK(!rig.graph->previewMatch(source,reference,1));CHECK(rig.sound()==d);}
 install(rig,before);CHECK(rig.graph->previewMatch(source,reference,.5f));CHECK(rig.graph->previewMatch(source,reference,1));CHECK(waitForMatch(*rig.graph));const auto target=rig.graph->matchPreview().target;CHECK(target);rig.graph->cancelMatch();
 CHECK(rig.graph->previewMatch(source,source,1));(void)waitForMatch(*rig.graph);CHECK(!rig.graph->matchPreview().canApply);CHECK(rig.graph->matchPreview().rmsDb&&*rig.graph->matchPreview().rmsDb<.001f);CHECK(!rig.graph->applyMatch());CHECK(rig.sound()==before);
 auto proposal=before;proposal.values[rig.base+4]=.8f;rig.graph->cancelMatch();rig.graph->showProposal(std::make_shared<SoundDocument>(proposal));CHECK(rig.graph->proposed());CHECK(!rig.graph->previewMatch(source,reference,1));CHECK(rig.graph->proposed());CHECK(rig.sound()==before);rig.graph->showProposal(nullptr);
}
void feedMatchCapture(Rig& rig,studio::EqMatchPanel& panel,int blocks){
 auto monitor=rig.processor->fxMonitorHandle();FxMonitor::ActiveSlots active{};active[7]=true;std::uint32_t random=927;
 for(int block=0;block<blocks;++block){CHECK(monitor->beginBlock(48000,active));for(int n=0;n<2048;++n){random=random*1664525+1013904223;const double x=(double(random)/4294967296.*2-1)*.2;monitor->capture(7,{x,-x},{x*.5,-x*.5});}rig.graph->analysisHub()->update();panel.updateMonitoring();}
}
void matchInputAccountsForTheExistingCurve(){
 std::cout<<"SECTION match_input_8dB_to_2dB_moves_toward_reference_and_bypassed_output\n";
 for(bool bypassed:{false,true}){Rig rig({},false);auto before=rig.sound();before.values[rig.base+4]=rig.processor->control(rig.base+4).convertTo0to1(8);
 const auto& registry=parameterRegistry();const auto bypass=std::find_if(registry.begin(),registry.end(),[](const auto& p){return p.id=="effect.8.bypassed";});CHECK(bypass!=registry.end());if(bypass==registry.end())return;before.values[std::size_t(bypass-registry.begin())]=bypassed?1.f:0.f;
 install(rig,before);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 studio::EqMatchPanel panel(*rig.processor,*model,rig.graph->analysisHub());panel.bind(*rig.graph);panel.setVisible(true);CHECK(panel.learn(false,bypassed));feedMatchCapture(rig,panel,64);CHECK(panel.stopLearning());CHECK(panel.sourceReference());if(!panel.sourceReference())return;
 auto reference=*panel.sourceReference();reference.label="Two dB reference";RoutedPatch target;CHECK(makeRoutedPatch(before,target));target.effects[7].dynamicEq.bands[0].gainDb=2;
 for(std::size_t i=0;i<129;++i){const auto response=dynamicEqEnergyResponse(target.effects[7].dynamicEq,48000,20*std::pow(1000.,double(i)/128));CHECK(response);if(response)reference.spectrum.db[i]+=float(20*std::log10(*response));}
 CHECK(panel.setReference(reference));CHECK(panel.preview());CHECK(waitForMatch(*rig.graph));const auto proposed=rig.graph->matchPreview().target;CHECK(proposed);if(proposed)CHECK(proposed->db[72]<-2);
 CHECK(panel.apply());const auto after=rig.sound();CHECK(after.values[rig.base+4]==before.values[rig.base+4]);RoutedPatch old,newPatch;CHECK(makeRoutedPatch(before,old)&&makeRoutedPatch(after,newPatch));double initial=0,matched=0;
 for(std::size_t i=0;i<129;++i){const auto hz=20*std::pow(1000.,double(i)/128);const auto a=dynamicEqEnergyResponse(old.effects[7].dynamicEq,48000,hz),b=dynamicEqEnergyResponse(newPatch.effects[7].dynamicEq,48000,hz),wanted=dynamicEqEnergyResponse(target.effects[7].dynamicEq,48000,hz);CHECK(a&&b&&wanted);if(a&&b&&wanted){initial+=std::pow(20*std::log10(*a/ *wanted),2);matched+=std::pow(20*std::log10(*b/ *wanted),2);}}
 CHECK(matched<initial*.5);CHECK(newPatch.effects[7].bypassed==bypassed);std::cout<<"MATCH_INPUT_RMS bypassed="<<bypassed<<" before="<<std::sqrt(initial/129)<<" after="<<std::sqrt(matched/129)<<'\n';}
}
void matchCaptureLimitsAndFileLifetime(){
 std::cout<<"SECTION match_capture_limit_reset_file_jobs_and_close\n";Rig rig({},false);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 auto panel=std::make_unique<studio::EqMatchPanel>(*rig.processor,*model,rig.graph->analysisHub());panel->bind(*rig.graph);panel->setVisible(true);const auto before=rig.sound();
 CHECK(panel->learn(false,true));feedMatchCapture(rig,*panel,256);CHECK(!panel->learning());CHECK(panel->sourceReference());CHECK(panel->learnedSeconds()>=2&&panel->learnedSeconds()<=10);
 if(!panel->sourceReference())return;auto ref=*panel->sourceReference();for(std::size_t i=0;i<129;++i)ref.spectrum.db[i]+=6*std::exp(-.5f*std::pow(float(int(i)-64)/10,2));ref.label="File reference";CHECK(panel->setReference(ref));
 juce::String encoded;CHECK(panel->exportReference(encoded));CHECK(encoded.getNumBytesAsUTF8()<=16384);CHECK(panel->importReference(encoded));CHECK(rig.sound()==before);
 const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("match-file-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());const auto file=root.getChildFile("reference.eqref");CHECK(file.replaceWithText(encoded));
 CHECK(panel->loadReferenceFile(file));CHECK(pumpUntil([&]{panel->updateMonitoring();return !panel->loadingReference();}));CHECK(panel->reference()&&panel->reference()->label==ref.label);CHECK(rig.sound()==before);
 const auto invalid=root.getChildFile("bad.eqref");CHECK(invalid.replaceWithText("{}"));CHECK(panel->loadReferenceFile(invalid));CHECK(pumpUntil([&]{panel->updateMonitoring();return !panel->loadingReference();}));CHECK(panel->reference()&&panel->reference()->label==ref.label);CHECK(panel->statusText().containsIgnoreCase("could not"));
 CHECK(panel->loadReferenceFile(file));panel->cancelPreview();CHECK(!panel->loadingReference());panel->updateMonitoring();CHECK(rig.sound()==before);
 CHECK(panel->learn(false,false));feedMatchCapture(rig,*panel,16);rig.processor->fxMonitorHandle()->invalidate();panel->updateMonitoring();CHECK(!panel->learning());CHECK(!panel->sourceReference());CHECK(panel->statusText().containsIgnoreCase("interrupted"));
 CHECK(panel->loadReferenceFile(file));panel.reset();CHECK(rig.graph->analysisHub()->activeAnalyzers()==0);CHECK(rig.sound()==before);CHECK(root.deleteRecursively());
}
void matchRejectsFrozenSourceAfterHostRateChange(){
 std::cout<<"SECTION match_frozen_source_invalidated_by_host_prepare_rate\n";Rig rig({},false);const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 rig.processor->prepareToPlay(48000,64);rig.refresh();studio::EqMatchPanel panel(*rig.processor,*model,rig.graph->analysisHub());panel.bind(*rig.graph);panel.setVisible(true);const auto before=rig.sound();
 CHECK(panel.learn(false,false));feedMatchCapture(rig,panel,64);CHECK(panel.stopLearning());const auto source=panel.sourceReference();CHECK(source);if(!source)return;
 auto reference=*source;reference.label="Frozen independent reference";for(std::size_t i=0;i<129;++i)reference.spectrum.db[i]+=6*std::exp(-.5f*std::pow(float(int(i)-64)/10,2));CHECK(panel.setReference(reference));CHECK(panel.preview());CHECK(waitForMatch(*rig.graph));
 rig.processor->prepareToPlay(96000,64);CHECK(rig.sound()==before);CHECK(!panel.preview());CHECK(!panel.sourceReference());CHECK(panel.reference()&&panel.reference()->captureId==reference.captureId);CHECK(!panel.apply());CHECK(rig.sound()==before);CHECK(panel.statusText().containsIgnoreCase("rate"));rig.processor->releaseResources();
}
void detectorDrawerPagesAreComplete()
{
 std::cout<<"SECTION detector_drawer_all_fields_pages_bounds_and_unavailable_readbacks\n";
 Rig rig({},false);CHECK(!rig.graph->isOnDesktop());const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 auto sound=rig.sound();sound.values[rig.base+7*11]=1;sound.values[dynamicEqDetectorParameterStart+7*40+7*5+2]=1;install(rig,sound);
 studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::dynamicEq,7}});panel.refresh(sound);
 CHECK(panel.parameters().size()==181);const auto first=dynamicEqDetectorParameterStart+7*40+7*5;
 for(std::size_t f=0;f<5;++f)CHECK(panel.knob(first+f)!=nullptr);
 CHECK(panel.showParameter(rig.base+7*11+3)&&panel.routesPage()==7&&panel.shownKnobs().size()==12);
 const bool revealed=panel.showParameter(first+4);CHECK(revealed);if(!revealed)return;
 panel.setSize(1485,panel.shownHeight());CHECK(panel.routesPage()==17&&panel.shownHeight()<=240&&panel.shownKnobs().size()==5);
 const auto before=rig.sound();auto* view=panel.knob(first)->getParentComponent();CHECK(view);if(!view)return;
 for(auto* knob:panel.shownKnobs()){CHECK(knob->isVisible());CHECK(view->getLocalBounds().contains(knob->getBounds()));CHECK(view->getLocalBounds().contains(knob->label.getBounds()));CHECK(view->getLocalBounds().contains(knob->entry.getBounds()));}
 const auto shown=panel.shownKnobs();for(std::size_t a=0;a<shown.size();++a)for(std::size_t b=a+1;b<shown.size();++b){CHECK(!shown[a]->getBounds().intersects(shown[b]->getBounds()));CHECK(!shown[a]->labelInk().expanded(2,0).intersects(shown[b]->labelInk().expanded(2,0)));}
 // The compact drawer groups source, matched frequency controls and one readout,
 // with equal mode keys; widening it must preserve those relationships.
 for(int width:{1485,1800}){
  panel.setSize(width,panel.shownHeight());const auto source=shown[2]->getBounds(),low=shown[3]->getBounds(),high=shown[4]->getBounds();
  CHECK(shown[2]->getPresentation()==studio::ProgrammerKnob::Presentation::choice);
  CHECK(low.withPosition(0,0)==high.withPosition(0,0)&&low.getCentreY()==high.getCentreY());
  CHECK(source.getRight()<low.getX()&&low.getRight()<high.getX());
  CHECK(shown[0]->getBounds().withPosition(0,0)==shown[1]->getBounds().withPosition(0,0));
  CHECK(shown[0]->getY()==shown[1]->getY()&&std::abs(shown[0]->getRight()-shown[1]->getX())<=2);
  auto* pageChoice=findComponent(panel,"rack.controls.band");CHECK(pageChoice);
  if(pageChoice)CHECK(pageChoice->getX()>source.getX()&&pageChoice->getRight()<source.getRight()&&pageChoice->getY()<source.getY());
 }
 for(const char* field:{"threshold","attack","release"}){auto* label=dynamic_cast<juce::Label*>(findComponent(*view,juce::String("eq.detector.live.")+field));CHECK(label);if(label){CHECK(label->isVisible()&&label->getText()=="--");CHECK(view->getLocalBounds().reduced(8).contains(label->getBounds()));}}
 auto* status=dynamic_cast<juce::Label*>(findComponent(*view,"eq.detector.status"));CHECK(status);if(status)CHECK(status->getText().containsIgnoreCase("unavailable"));
 CHECK(rig.sound()==before);
 CHECK(panel.showParameter(rig.base+3)&&panel.shownHeight()==292);
}
void detectorPageMetersAndListenAreTransient()
{
 std::cout<<"SECTION detector_page_metadata_without_spectrum_and_transient_trigger_listen\n";
 Rig rig;auto d=rig.sound();const auto detector=dynamicEqDetectorParameterStart+7*40;d.values[detector]=d.values[detector+1]=d.values[detector+2]=1;install(rig,d);
 auto bus=rig.processor->getBusesLayout();bus.inputBuses.set(0,juce::AudioChannelSet::stereo());CHECK(rig.processor->setBusesLayout(bus));rig.processor->prepareToPlay(48000,64);rig.refresh();
 auto& graph=*rig.graph;graph.setSpectrumOn(false);CHECK(!graph.subscription());graph.setDetectorAuditionMode(true);CHECK(graph.detectorAuditionMode());graph.updateMonitoring();CHECK(graph.subscription());
 juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;int frame=0;const auto render=[&]{for(int n=0;n<64;++n,++frame){const auto x=float(.5*std::sin(frame*.03));audio.setSample(0,n,x);audio.setSample(1,n,-x);}rig.processor->processBlock(audio,midi);graph.updateMonitoring();};
 for(int block=0;block<96;++block)render();CHECK(!graph.spectrumOn()&&!graph.spectrum().valid&&graph.spectrum().windows==0);CHECK(graph.spectrum().detectorReadbacksValid&&graph.spectrum().externalAvailable);
 auto* listen=dynamic_cast<juce::Button*>(findComponent(graph,"eq.listen"));CHECK(listen);if(listen){CHECK(listen->isEnabled()&&listen->getButtonText().containsIgnoreCase("trigger"));listen->setState(juce::Button::buttonDown);render();const auto request=rig.processor->fxMonitorHandle()->auditionRequest(7);CHECK(request&&request->detector&&request->band==0);listen->setState(juce::Button::buttonNormal);}
 graph.setDetectorAuditionMode(false);CHECK(!graph.detectorAuditionMode()&&!graph.subscription());CHECK(!graph.spectrum().detectorReadbacksValid);CHECK(rig.sound()==d);
}
void detectorDrawerEditsUseTheSharedTransaction()
{
 std::cout<<"SECTION detector_drawer_manual_undo_locks_and_invalid_free_limits\n";
 Rig rig;const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::dynamicEq,7}});
 const auto first=dynamicEqDetectorParameterStart+7*40;
 for(std::size_t field=0;field<5;++field){
  panel.refresh(rig.sound());CHECK(panel.showParameter(first+field));auto* knob=panel.knob(first+field);CHECK(knob);if(!knob)continue;
  const auto before=rig.sound();const float value=field<2?1.f:field==2?1.f/3:field==3?.2f:.8f;Notifications events(rig.processor->control(first+field));
  knob->move(value);const auto after=rig.sound();CHECK(after.values[first+field]==value);CHECK(events.starts==1&&events.ends==1);CHECK(after!=before);
  CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);CHECK(rig.processor->redoSoundEdit());CHECK(rig.sound()==after);
 }
 auto locked=rig.sound();locked.controlLocks[first]=true;install(rig,locked);panel.refresh(locked);auto* autoThreshold=panel.knob(first);CHECK(autoThreshold);if(autoThreshold)autoThreshold->move(0);CHECK(rig.sound()==locked);
 auto limits=rig.sound();limits.controlLocks[first]=false;limits.values[first+3]=.8f;limits.values[first+4]=.9f;install(rig,limits);panel.refresh(limits);
 auto* low=panel.knob(first+3);CHECK(low);if(low)low->move(1);CHECK(rig.sound()==limits);
 auto bandMode=limits;bandMode.values[first+2]=0;bandMode.values[first+3]=1;install(rig,bandMode);panel.refresh(bandMode);CHECK(panel.showParameter(first+3));
 CHECK(low&&low->isEnabled());if(low)low->move(.2f);CHECK(rig.sound().values[first+3]==.2f);
}
void sketchCancelLeavesTheDocumentUntouched(){
 std::cout<<"SECTION sketch_cancel_is_preview_only\n";
 Rig rig;const auto before=rig.sound();EqCurve target;
 for(std::size_t i=0;i<target.db.size();++i){const double x=(double(i)-70.)/18.;target.db[i]=float(-6.*std::exp(-x*x));}
 rig.graph->setSketchEnabled(true);CHECK(rig.graph->sketchEnabled());
 CHECK(rig.graph->previewSketch(target));CHECK(rig.sound()==before);
 rig.graph->cancelSketch();CHECK(!rig.graph->applySketch());CHECK(rig.sound()==before);
}
EqCurve sketchTargetCurve(float gain=-6.f){
 EqCurve target;for(std::size_t i=0;i<target.db.size();++i){const double x=(double(i)-70.)/18.;target.db[i]=float(gain*std::exp(-x*x));}return target;
}
bool waitForSketch(DynamicEqGraph& graph){
 const auto until=juce::Time::getMillisecondCounterHiRes()+8000;
 while(graph.sketchFitting()&&juce::Time::getMillisecondCounterHiRes()<until){graph.updateMonitoring();juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}
 graph.updateMonitoring();CHECK(!graph.sketchFitting());return graph.sketchPreviewReady();
}
void sketchApplyIsAtomicAndUsesOnlyUnusedBands(){
 std::cout<<"SECTION sketch_async_preview_atomic_apply_and_undo\n";Rig rig;auto& graph=*rig.graph;auto& p=*rig.processor;
 auto original=rig.sound();original.macros[0]={parameterRegistry()[rig.base+3].stateKey,.12f};install(rig,original);
 const auto before=rig.sound();Notifications active(p.control(rig.base+11)),gain(p.control(rig.base+15));
 graph.setSketchEnabled(true);const auto target=sketchTargetCurve();const auto start=juce::Time::getMillisecondCounterHiRes();
 CHECK(graph.previewSketch(target));const auto queuedMs=juce::Time::getMillisecondCounterHiRes()-start;CHECK(rig.sound()==before);
 CHECK(waitForSketch(graph));CHECK(graph.sketchFitError().has_value());if(graph.sketchFitError())CHECK(*graph.sketchFitError()<1.f);
 snapshotImage("sketch-preview.png",graph);
 CHECK(rig.sound()==before);CHECK(graph.sketchTarget()&&graph.sketchTarget()->db==target.db);
 CHECK(graph.applySketch());const auto after=rig.sound();CHECK(after!=before);CHECK(!graph.sketchEnabled());
 CHECK(after.macros==before.macros);CHECK(after.layout==before.layout);CHECK(after.controlLocks==before.controlLocks);CHECK(after.effectLocks==before.effectLocks);
 for(std::size_t i=0;i<parameterCount;++i)if(i<rig.base+11||i>=rig.base+88)CHECK(after.values[i]==before.values[i]);
 CHECK(active.starts==1&&active.ends==1);CHECK(gain.starts==1&&gain.ends==1);
 CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);CHECK(p.redoSoundEdit());CHECK(rig.sound()==after);CHECK(!graph.applySketch());
 std::cout<<"SKETCH_UI queue_ms="<<queuedMs<<'\n';
}
void sketchSkipsLockedAndMacroBoundDestinations(){
 std::cout<<"SECTION sketch_locked_and_macro_bound_unused_bands\n";Rig rig;auto before=rig.sound();
 before.controlLocks[rig.base+11+5]=true;before.macros[0]={parameterRegistry()[rig.base+22+3].stateKey,.15f};install(rig,before);
 rig.graph->setSketchEnabled(true);CHECK(rig.graph->previewSketch(sketchTargetCurve()));CHECK(waitForSketch(*rig.graph));CHECK(rig.graph->applySketch());
 const auto after=rig.sound();for(std::size_t offset=0;offset<33;++offset)CHECK(after.values[rig.base+offset]==before.values[rig.base+offset]);
 CHECK(after.macros==before.macros);CHECK(after.controlLocks==before.controlLocks);CHECK(after.values[rig.base+33]==1);
}
void sketchRefusesFullLockedAndProposalRacks(){
 std::cout<<"SECTION sketch_full_locked_scale_zero_and_ai_review\n";
 for(int reason=0;reason<4;++reason){Rig rig;auto before=rig.sound();
  if(reason==0)before.effectLocks[7]=true;
  if(reason==1)for(std::size_t band=0;band<8;++band)before.values[rig.base+band*11]=1;
  if(reason==2)before.values[dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot+1]=0;
  install(rig,before);
  if(reason==3){auto proposal=std::make_shared<SoundDocument>(before);proposal->values[rig.base+4]=.7f;rig.graph->showProposal(proposal);CHECK(rig.graph->proposed());}
  rig.graph->setSketchEnabled(true);CHECK(!rig.graph->previewSketch(sketchTargetCurve()));CHECK(!rig.graph->applySketch());CHECK(rig.sound()==before);
  if(reason==0)snapshotImage("sketch-locked-refusal.png",*rig.graph);if(reason==1)snapshotImage("sketch-full-bank-refusal.png",*rig.graph);
  if(reason==3)CHECK(rig.graph->proposed());rig.graph->cancelSketch();
 }
}
void sketchRejectsStaleResetRebindAndRetirement(){
 std::cout<<"SECTION sketch_cancelled_or_stale_job_never_applies\n";
 for(int reason=0;reason<7;++reason){Rig rig;auto& graph=*rig.graph;auto& p=*rig.processor;const auto before=rig.sound();
  graph.setSketchEnabled(true);CHECK(graph.previewSketch(sketchTargetCurve()));
  if(reason==0)p.control(0).setValueNotifyingHost(.31f);
  if(reason==1)install(rig,before);
  if(reason==2)graph.refresh(before,0);
  if(reason==3)graph.retire();
  if(reason==4)graph.setVisible(false);
  if(reason==5)graph.showProposal(std::make_shared<SoundDocument>(before));
  if(reason==6)graph.cancelSketch();
  const auto expected=rig.sound();CHECK(!graph.applySketch());CHECK(rig.sound()==expected);
  graph.updateMonitoring();CHECK(!graph.sketchPreviewReady());
 }
}
void sketchReplacementAndRetainedCallbacksAreSafe(){
 std::cout<<"SECTION sketch_latest_job_and_host_callback_retirement\n";
 {Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();graph.setSketchEnabled(true);
  CHECK(graph.previewSketch(sketchTargetCurve(-6)));const auto latest=sketchTargetCurve(5);CHECK(graph.previewSketch(latest));
  CHECK(graph.sketchTarget()&&graph.sketchTarget()->db==latest.db);CHECK(waitForSketch(graph));CHECK(rig.sound()==before);CHECK(graph.applySketch());CHECK(rig.sound()!=before);}
 for(int phase=0;phase<3;++phase){Rig rig;auto* graph=rig.graph.get();const auto before=rig.sound();graph->setSketchEnabled(true);CHECK(graph->previewSketch(sketchTargetCurve()));CHECK(waitForSketch(*graph));
  Notifications event(rig.processor->control(rig.base+11));
  if(phase==0)event.onStart=[&]{rig.graph.reset();};
  if(phase==1)event.onValue=[&]{rig.graph.reset();};
  if(phase==2)event.onEnd=[&]{rig.graph.reset();};
  (void)graph->applySketch();CHECK(!rig.graph);CHECK(event.starts==event.ends);CHECK(validateSoundDocument(rig.sound()));if(phase==0)CHECK(rig.sound()==before);
  event.onStart=nullptr;event.onValue=nullptr;event.onEnd=nullptr;
 }
}
void sketchStrokesAreBoundedDirectionIndependentAndPreviewOnly(){
 std::cout<<"SECTION sketch_stroke_resampling_bounds_direction_and_last_x\n";Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();const auto r=graph.plotBounds();
 const auto point=[&](float x,float gain){return juce::Point<float>{r.getX()+r.getWidth()*x,r.getCentreY()-r.getHeight()*gain/60.f};};
 const std::array<juce::Point<float>,3> points{point(.2f,3),point(.4f,-8),point(.7f,0)};
 const auto stroke=[&](bool backwards){graph.setSketchEnabled(true);const auto first=points[backwards?2:0];graph.mouseDown(mouse(graph,first,first));for(int n=1;n<3;++n)graph.mouseDrag(mouse(graph,points[backwards?2-n:n],first));graph.mouseUp(mouse(graph,points[backwards?0:2],first,false));return graph.sketchTarget();};
 const auto forward=stroke(false);graph.cancelSketch();const auto backward=stroke(true);CHECK(forward&&backward);if(forward&&backward){CHECK(forward->db==backward->db);CHECK(forward->db[0]==0);CHECK(forward->db[128]==0);}
 graph.cancelSketch();graph.setSketchEnabled(true);const auto start=point(.2f,2);graph.mouseDown(mouse(graph,start,start));graph.mouseDrag(mouse(graph,point(.5f,-5),start));graph.mouseDrag(mouse(graph,point(.5f,7),start));graph.mouseDrag(mouse(graph,point(.8f,0),start));graph.mouseUp(mouse(graph,point(.8f,0),start,false));
 const auto duplicate=graph.sketchTarget();CHECK(duplicate.has_value());if(duplicate)CHECK(std::abs(duplicate->db[64]-7.f)<.001f);
 graph.cancelSketch();graph.setSketchEnabled(true);graph.mouseDown(mouse(graph,start,start));
 for(int i=0;i<700;++i)graph.mouseDrag(mouse(graph,point(.05f+.9f*i/699.f,25.f*std::sin(float(i)*.03f)),start));
 CHECK(graph.sketchPointCount()>0&&graph.sketchPointCount()<=256);snapshotImage("sketch-stroke.png",graph);graph.mouseUp(mouse(graph,point(.95f,0),start,false));const auto bounded=graph.sketchTarget();CHECK(bounded.has_value());if(bounded)for(const auto value:bounded->db)CHECK(std::isfinite(value)&&value>=-12.001f&&value<=12.001f);
 CHECK(rig.sound()==before);graph.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));CHECK(!graph.sketchEnabled());CHECK(!graph.applySketch());CHECK(rig.sound()==before);
}
void sketchRateSpectrumBypassAndButtons(){
 std::cout<<"SECTION sketch_rate_change_spectrum_off_bypass_and_real_actions\n";
 {Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();graph.setSketchEnabled(true);CHECK(graph.previewSketch(sketchTargetCurve()));CHECK(waitForSketch(graph));rig.processor->prepareToPlay(96000,64);CHECK(!graph.applySketch());CHECK(rig.sound()==before);}
 {Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();graph.setSpectrumOn(false);CHECK(graph.keyPressed(juce::KeyPress('s')));CHECK(graph.sketchEnabled());CHECK(graph.previewSketch(sketchTargetCurve()));CHECK(waitForSketch(graph));CHECK(!graph.subscription());
  auto* apply=dynamic_cast<juce::Button*>(findComponent(graph,"eq.sketch.apply"));auto* cancel=dynamic_cast<juce::Button*>(findComponent(graph,"eq.sketch.cancel"));CHECK(apply&&cancel);
  int visible=0;for(auto* child:graph.getChildren())if(child->isVisible()&&dynamic_cast<juce::Button*>(child))++visible;CHECK(visible==2);
  if(apply){apply->grabKeyboardFocus();juce::MessageManager::getInstance()->runDispatchLoopUntil(10);CHECK(graph.sketchPreviewReady());CHECK(clickComparisonKey(*apply));CHECK(rig.sound()!=before);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);rig.refresh();}
  graph.setSketchEnabled(true);CHECK(graph.previewSketch(sketchTargetCurve()));if(cancel)CHECK(clickComparisonKey(*cancel));CHECK(!graph.sketchEnabled());CHECK(rig.sound()==before);
 }
 {Rig rig;auto before=rig.sound();
  // Slot bypass belongs to the effect layout's registered bypass control.
  const auto bypass=rackParameterIndex(NativeModule::dynamicEq,7,"bypassed");CHECK(bypass.has_value());if(bypass)before.values[*bypass]=1;install(rig,before);
  rig.graph->setSketchEnabled(true);CHECK(rig.graph->previewSketch(sketchTargetCurve()));CHECK(waitForSketch(*rig.graph));CHECK(rig.graph->applySketch());if(bypass)CHECK(rig.sound().values[*bypass]==1);
 }
 {Rig rig;CHECK(bandMenuCommand(*rig.graph,7));CHECK(rig.graph->sketchEnabled());rig.graph->cancelSketch();}
}
void sketchFlatPoorAndInvalidTargetsStayHonest(){
 std::cout<<"SECTION sketch_flat_and_nonrepresentable_residual\n";Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();graph.setSketchEnabled(true);
 CHECK(graph.previewSketch(EqCurve{}));(void)waitForSketch(graph);CHECK(graph.sketchFitError()&&*graph.sketchFitError()<.001f);CHECK(!graph.sketchPreviewReady());CHECK(!graph.applySketch());CHECK(rig.sound()==before);snapshotImage("sketch-flat.png",graph);
 EqCurve sharp;for(std::size_t i=0;i<sharp.db.size();++i)sharp.db[i]=i%2?12.f:-12.f;CHECK(graph.previewSketch(sharp));(void)waitForSketch(graph);CHECK(graph.sketchFitError()&&*graph.sketchFitError()>1.f);CHECK(rig.sound()==before);snapshotImage("sketch-unrepresentable.png",graph);
 auto invalid=sharp;invalid.db[17]=std::numeric_limits<float>::quiet_NaN();CHECK(!graph.previewSketch(invalid));CHECK(rig.sound()==before);graph.cancelSketch();
}
void sketchKeepsTheReleasePointAndOutsideCutResponse(){
 std::cout<<"SECTION sketch_release_endpoint_and_existing_response_outside_stroke\n";Rig rig;auto before=rig.sound();
 before.values[rig.base+2]=rig.processor->control(rig.base+2).convertTo0to1(float(DynamicEqShape::lowCut));before.values[rig.base+3]=rig.processor->control(rig.base+3).convertTo0to1(180);install(rig,before);
 auto& graph=*rig.graph;const auto r=graph.plotBounds();const auto point=[&](float x,float gain){return juce::Point<float>{r.getX()+r.getWidth()*x,r.getCentreY()-r.getHeight()*gain/60.f};};
 graph.setSketchEnabled(true);const auto start=point(.4f,0);graph.mouseDown(mouse(graph,start,start));graph.mouseDrag(mouse(graph,point(.5f,-5),start));graph.mouseUp(mouse(graph,point(.8f,8),start,false));
 const auto target=graph.sketchTarget();CHECK(target.has_value());if(target){
  auto raw=before;raw.macros={};RoutedPatch patch;CHECK(makeRoutedPatch(raw,patch));const auto response=dynamicEqResponse(patch.effects[7].dynamicEq,48000,20);CHECK(response);
  if(response){const float original=float(20.*std::log10(std::max(std::abs(*response),1e-12)));CHECK(original<-12);CHECK(target->db[0]==original);}
  CHECK(target->db[100]>6.f&&target->db[100]<8.f);
 }
 CHECK(rig.sound()==before);graph.cancelSketch();
}
void actualEditorClosesDuringSketchFit(){
 std::cout<<"SECTION real_editor_closes_with_owned_sketch_job\n";Rig rig;rig.graph.reset();const auto before=rig.sound();
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;
 graph->updateMonitoring();const auto analysis=graph->analysisHub();const auto token=graph->subscription();graph->setSketchEnabled(true);CHECK(graph->previewSketch(sketchTargetCurve()));
 juce::Component::SafePointer<DynamicEqGraph> safe(graph);editor.reset();CHECK(!safe);CHECK(rig.sound()==before);if(token)CHECK(analysis->generation(*token)==0);
 juce::MessageManager::getInstance()->runDispatchLoopUntil(30);CHECK(rig.sound()==before);
}
void sketchFocusAloneKeepsTheExplicitPreviewDecision(){
 std::cout<<"SECTION sketch_focus_alone_does_not_discard_pending_or_ready_preview\n";
 for(const bool ready:{false,true}){Rig rig;const auto before=rig.sound();auto& graph=*rig.graph;graph.setSketchEnabled(true);CHECK(graph.previewSketch(sketchTargetCurve()));
  if(ready)CHECK(waitForSketch(graph));graph.focusLost(juce::Component::focusChangedDirectly);CHECK(graph.sketchEnabled());CHECK(waitForSketch(graph));CHECK(rig.sound()==before);graph.cancelSketch();CHECK(rig.sound()==before);
 }
}
void sketchMailboxNeedsUiConsumptionAfterWorkerCompletion(){
 std::cout<<"SECTION sketch_worker_done_precedes_ui_preview_publication\n";Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();graph.setSketchEnabled(true);CHECK(graph.previewSketch(sketchTargetCurve()));
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(graph.sketchFitting()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 CHECK(!graph.sketchFitting());CHECK(!graph.sketchPreviewReady());CHECK(rig.sound()==before);
 graph.updateMonitoring();CHECK(graph.sketchPreviewReady());CHECK(rig.sound()==before);graph.cancelSketch();
}
void sketchGraphChecks(){sketchCancelLeavesTheDocumentUntouched();sketchApplyIsAtomicAndUsesOnlyUnusedBands();sketchSkipsLockedAndMacroBoundDestinations();sketchRefusesFullLockedAndProposalRacks();sketchRejectsStaleResetRebindAndRetirement();sketchReplacementAndRetainedCallbacksAreSafe();sketchStrokesAreBoundedDirectionIndependentAndPreviewOnly();sketchRateSpectrumBypassAndButtons();sketchFlatPoorAndInvalidTargetsStayHonest();sketchKeepsTheReleasePointAndOutsideCutResponse();actualEditorClosesDuringSketchFit();sketchFocusAloneKeepsTheExplicitPreviewDecision();sketchMailboxNeedsUiConsumptionAfterWorkerCompletion();}
void comparisonRecallIsOneUndo(){
 std::cout<<"SECTION comparison_raw_scope_balanced_gestures_and_one_undo\n";
 Rig rig;auto& graph=*rig.graph;auto& processor=*rig.processor;
 const auto gain=rig.base+4,automatic=dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot,scale=automatic+1;
 const auto before=rig.sound();CHECK(graph.storeComparison(false));CHECK(graph.recallComparison(false));CHECK(rig.sound()==before);
 SoundDocument changed=before;changed.values[gain]=.75f;changed.values[automatic]=1;changed.values[scale]=.75f;
 std::uint64_t revision=0;SoundDocument live;CHECK(processor.trySoundSnapshot(live,revision));CHECK(processor.applySoundEdit(changed,SoundEditOrigin::manual,revision));rig.refresh();
 CHECK(graph.storeComparison(true));
 processor.control(0).setValueNotifyingHost(.23f);rig.refresh();const auto bankB=rig.sound();
 Notifications gainEvents(processor.control(gain)),autoEvents(processor.control(automatic)),scaleEvents(processor.control(scale));
 CHECK(graph.recallComparison(false));const auto bankA=rig.sound();
 const auto reach=unitReach(NativeModule::dynamicEq,7);auto expected=bankB;for(const auto index:reach)expected.values[index]=before.values[index];
 CHECK(bankA==expected);CHECK(bankA.values[0]==.23f);
 CHECK(gainEvents.starts==1&&gainEvents.ends==1);CHECK(autoEvents.starts==1&&autoEvents.ends==1);CHECK(scaleEvents.starts==1&&scaleEvents.ends==1);
 CHECK(processor.undoSoundEdit());CHECK(rig.sound()==bankB);CHECK(processor.redoSoundEdit());CHECK(rig.sound()==bankA);rig.refresh();
 CHECK(graph.recallComparison(true));CHECK(rig.sound()==bankB);
 graph.clearComparison();CHECK(!graph.recallComparison(false));CHECK(!graph.recallComparison(true));
}
void comparisonLocksResetRebindAndVisibility(){
 std::cout<<"SECTION comparison_locks_reset_rebind_macros_and_hidden_retention\n";
 Rig rig;auto& g=*rig.graph;auto& p=*rig.processor;const auto gain=rig.base+4;
 const auto edit=[&](const std::function<void(SoundDocument&)>& change){SoundDocument d;std::uint64_t rev;CHECK(p.trySoundSnapshot(d,rev));change(d);CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,rev));rig.refresh();};
 edit([&](auto& d){d.macros[0]={parameterRegistry()[gain].stateKey,.17f};});const auto a=rig.sound();CHECK(g.storeComparison(false));
 edit([&](auto& d){d.values[gain]=.75f;});CHECK(g.storeComparison(true));
 edit([&](auto& d){d.controlLocks[gain]=true;});const auto locked=rig.sound();Notifications event(p.control(gain));
 CHECK(!g.recallComparison(false));CHECK(rig.sound()==locked);CHECK(event.starts==0&&event.values==0&&event.ends==0);
 edit([&](auto& d){d.controlLocks[gain]=false;});CHECK(g.recallComparison(false));CHECK(rig.sound().values[gain]==a.values[gain]);CHECK(rig.sound().macros==a.macros);
 edit([&](auto& d){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=0;add.effect=EffectKind::drive;CHECK(editRackLayout(d.layout,add,d.layout)==RackEditResult::applied);});
 CHECK(g.recallComparison(true));CHECK(rig.sound().values[gain]==.75f);CHECK(rig.sound().layout.effects[0].kind==EffectKind::drive);
 g.setVisible(false);g.updateMonitoring();g.setVisible(true);rig.refresh();CHECK(g.recallComparison(false));
 CHECK(g.storeComparison(false));juce::MemoryBlock wire;const auto before=rig.sound();CHECK(encodeSoundState(before,wire));CHECK(p.restoreSoundState(wire.getData(),int(wire.getSize())));
 CHECK(!g.recallComparison(false));rig.refresh();CHECK(!g.recallComparison(true));CHECK(rig.sound()==before);
 edit([&](auto& d){CHECK(editRackLayout(d.layout,{RackEditKind::replaceEffect,0,masterChain,0,EffectKind::dynamicEq,true},d.layout)==RackEditResult::applied);});
 CHECK(g.storeComparison(false));g.refresh(rig.sound(),0);CHECK(!g.recallComparison(false));g.refresh(rig.sound(),7);CHECK(!g.recallComparison(false));
}
void comparisonReentrantAutomationAndRetirement(){
 std::cout<<"SECTION comparison_reentrant_host_change_and_graph_retirement\n";
 for(int phase=0;phase<4;++phase){
  Rig rig;auto& p=*rig.processor;const auto gain=rig.base+4;CHECK(rig.graph->storeComparison(false));
  auto d=rig.sound();std::uint64_t rev;CHECK(p.trySoundSnapshot(d,rev));d.values[gain]=.75f;CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,rev));rig.refresh();const auto before=rig.sound();
  Notifications event(p.control(gain));auto* graph=rig.graph.get();
  if(phase==0)event.onStart=[&]{p.control(0).setValueNotifyingHost(.31f);};
  if(phase==1)event.onStart=[&]{rig.graph.reset();};
  if(phase==2)event.onValue=[&]{rig.graph.reset();};
  if(phase==3)event.onEnd=[&]{rig.graph.reset();};
  (void)graph->recallComparison(false);CHECK(event.starts==event.ends);
  if(phase==0){CHECK(rig.sound().values[gain]==before.values[gain]);CHECK(rig.sound().values[0]==.31f);event.onStart=nullptr;rig.refresh();CHECK(rig.graph->recallComparison(false));}
  else {CHECK(!rig.graph);CHECK(validateSoundDocument(rig.sound()));if(phase==1)CHECK(rig.sound()==before);}
  event.onStart=nullptr;event.onValue=nullptr;event.onEnd=nullptr;
  const bool opened=p.beginParameterGesture(0);CHECK(opened);if(opened)CHECK(p.endParameterGesture(0));
 }
}
bool clickComparisonKey(juce::Button& button){
 struct Ack:juce::Button::Listener{bool done=false;void buttonClicked(juce::Button*) override{done=true;}} ack;
 button.addListener(&ack);button.triggerClick();const bool clicked=pumpUntil([&]{return ack.done;});button.removeListener(&ack);return clicked;
}
bool bandMenuCommand(DynamicEqGraph& graph,int downSteps){
 auto* more=dynamic_cast<juce::Button*>(findComponent(graph,"eq.more"));CHECK(more);if(!more)return false;
 more->triggerClick();if(!pumpUntil([]{return juce::Component::getCurrentlyModalComponent()!=nullptr;})){CHECK(false);return false;}
 juce::Component::SafePointer<juce::Component> menu=juce::Component::getCurrentlyModalComponent();CHECK(menu->getName()=="menu");
 const auto selected=graph.selectedBands();graph.focusLost(juce::Component::focusChangedDirectly);CHECK(graph.selectedBands()==selected);
 for(int n=0;n<downSteps&&menu;++n)CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
 if(menu)CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
 const bool closed=pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;});
 // Modal dismissal precedes the separately posted command callback.
 juce::MessageManager::getInstance()->runDispatchLoopUntil(30);return closed;
}
void labelInkClearsTheDisplayRim(){
 std::cout<<"SECTION rendered_axis_ink_clears_all_four_display_edges\n";Rig rig;auto& graph=*rig.graph;
 for(bool mixed:{false,true}){SoundDocument d;std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));
  d.values[dynamicEqPlacementParameterStart+7*8]=mixed?.5f:0;
  if(mixed){for(std::size_t band:{1u,2u}){d.values[rig.base+band*11]=1;d.values[rig.base+band*11+3]=rig.processor->control(rig.base+band*11+3).convertTo0to1(band==1?20.f:20000.f);d.values[rig.base+band*11+4]=rig.processor->control(rig.base+band*11+4).convertTo0to1(band==1?24.f:-24.f);d.values[dynamicEqPlacementParameterStart+7*8+band]=band==1?.5f:1.f;}}
  if(mixed)CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));
  rig.refresh();
 for(bool sketch:{false,true}){graph.setSketchEnabled(sketch);CHECK(graph.sketchEnabled()==sketch);for(int width:{1045,1500})for(float scale:{.7f,1.f,1.17f}){
  graph.setSize(width,274);const auto image=graph.createComponentSnapshot(graph.getLocalBounds(),true,scale);
  std::array<int,4> ink{};
  for(int y=0;y<image.getHeight();++y)for(int x=0;x<image.getWidth();++x){
   const auto p=image.getPixelAt(x,y);if(p.getRed()<=170||int(p.getGreen())-p.getRed()<3||int(p.getBlue())-p.getRed()<5)continue;
   const float localX=(x+.5f)/scale,localY=(y+.5f)/scale;
   if(localY>=8&&localY<266){if(localX>=8&&localX<20)++ink[0];if(localX>=width-20&&localX<width-8)++ink[1];}
   if(localX>=8&&localX<width-8){if(localY>=8&&localY<20)++ink[2];if(localY>=254&&localY<266)++ink[3];}
  }
  std::cout<<"EDGE_INK mixed="<<mixed<<" sketch="<<sketch<<" width="<<width<<" scale="<<scale<<" left="<<ink[0]<<" right="<<ink[1]<<" top="<<ink[2]<<" bottom="<<ink[3]<<'\n';
  for(const auto count:ink)CHECK(count==0);
  if(mixed&&!sketch&&width==1045&&scale==1.f)snapshotImage("placement-boundary-graph.png",graph);
 }}}
}
void focusedToolbarAndBandMenu(){
 std::cout<<"SECTION one_display_strip_and_reachable_band_commands\n";Rig rig;auto& graph=*rig.graph;
 int visibleButtons=0;for(auto* child:graph.getChildren())if(child->isVisible()&&dynamic_cast<juce::Button*>(child))++visibleButtons;
 CHECK(visibleButtons<=8);CHECK(graph.plotBounds().getHeight()>=140);CHECK(graph.getHeight()-graph.plotBounds().getBottom()>=70);
 const auto before=rig.sound();if(!bandMenuCommand(graph,2))return;
 CHECK(rig.sound().values[rig.base+1]==1);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);rig.refresh();
 int selected=0;graph.onBandSelected=[&](std::size_t band){CHECK(band==0);++selected;};
 CHECK(bandMenuCommand(graph,0));CHECK(selected==1);CHECK(rig.sound()==before);graph.onBandSelected=nullptr;
 for(int step:{1,3,4}){
  graph.selectBand(0);
  CHECK(bandMenuCommand(graph,step));const auto edited=rig.sound();
  CHECK(edited.values[rig.base+(step==3?11:0)]==(step==3?1.f:0.f));
  CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);rig.refresh();
 }
 CHECK(bandMenuCommand(graph,5));CHECK(!graph.spectrumOn());CHECK(rig.sound()==before);
 CHECK(bandMenuCommand(graph,5));CHECK(graph.spectrumOn());CHECK(rig.sound()==before);
 // All eight identities, including unused bands, remain keyboard reachable.
 auto* more=dynamic_cast<juce::Button*>(findComponent(graph,"eq.more"));if(!more)return;
 more->triggerClick();CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()!=nullptr;}));
 juce::Component::SafePointer<juce::Component> parentMenu=juce::Component::getCurrentlyModalComponent();
 if(parentMenu){for(int n=0;n<6;++n)CHECK(parentMenu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(parentMenu->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));}
 juce::Component::SafePointer<juce::Component> submenu=juce::Component::getCurrentlyModalComponent();CHECK(submenu&&submenu!=parentMenu);
 if(submenu&&submenu!=parentMenu){CHECK(submenu->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));CHECK(submenu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));}
 CHECK(pumpUntil([&]{return graph.selectedBand()==7;}));CHECK(rig.sound()==before);graph.selectBand(0);
 // A menu opened before a sound recall must not act on the new sound.
 more->triggerClick();CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()!=nullptr;}));
 juce::Component::SafePointer<juce::Component> menu=juce::Component::getCurrentlyModalComponent();install(rig,before);
 if(menu){CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));}
 CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;}));juce::MessageManager::getInstance()->runDispatchLoopUntil(30);CHECK(rig.sound()==before);
 more->triggerClick();CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()!=nullptr;}));menu=juce::Component::getCurrentlyModalComponent();graph.retire();
 if(menu){CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));}
 CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;}));juce::MessageManager::getInstance()->runDispatchLoopUntil(30);CHECK(rig.sound()==before);
}
void comparisonToolbarAndGainBindings(){
 std::cout<<"SECTION comparison_toolbar_copy_switch_registered_gain_and_keyboard\n";
 Rig rig;auto& g=*rig.graph;auto& p=*rig.processor;
 auto* a=dynamic_cast<juce::Button*>(findComponent(g,"eq.compare.a"));auto* b=dynamic_cast<juce::Button*>(findComponent(g,"eq.compare.b"));
 auto* copy=dynamic_cast<juce::Button*>(findComponent(g,"eq.compare.copy"));auto* automatic=dynamic_cast<juce::Button*>(findComponent(g,"eq.auto_gain"));
 auto* scale=dynamic_cast<juce::Slider*>(findComponent(g,"eq.gain_scale"));
 CHECK(a);CHECK(b);CHECK(copy);CHECK(automatic);CHECK(scale);if(!a||!b||!copy||!automatic||!scale)return;
 CHECK(scale->getMinimum()==0&&scale->getMaximum()==2&&scale->getValue()==1);
 CHECK(automatic->getTooltip().containsIgnoreCase("estimate"));CHECK(copy->getTooltip().containsIgnoreCase("transient"));
 for(int width:{1045,1500}){g.setSize(width,274);for(auto* component:g.getChildren())if(component->isVisible()){
  CHECK(g.getLocalBounds().contains(component->getBounds()));CHECK(!g.plotBounds().intersects(component->getBounds().toFloat()));
  for(auto* other:g.getChildren())if(other!=component&&other->isVisible())CHECK(!component->getBounds().intersects(other->getBounds()));
 }}
 const auto original=rig.sound();CHECK(clickComparisonKey(*copy));CHECK(rig.sound()==original);CHECK(clickComparisonKey(*b));
 auto current=rig.sound();std::uint64_t rev;CHECK(p.trySoundSnapshot(current,rev));current.values[rig.base+4]=.75f;CHECK(p.applySoundEdit(current,SoundEditOrigin::manual,rev));rig.refresh();
 CHECK(clickComparisonKey(*a));CHECK(rig.sound().values[rig.base+4]==original.values[rig.base+4]);CHECK(a->getToggleState());
 CHECK(clickComparisonKey(*b));CHECK(rig.sound().values[rig.base+4]==.75f);CHECK(b->getToggleState());
 const auto autoIndex=dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot,scaleIndex=autoIndex+1;
 Notifications autoEvents(p.control(autoIndex)),scaleEvents(p.control(scaleIndex));
 const auto beforeAuto=rig.sound();CHECK(clickComparisonKey(*automatic));CHECK(rig.sound().values[autoIndex]==1);CHECK(autoEvents.starts==1&&autoEvents.ends==1);
 CHECK(p.undoSoundEdit());CHECK(rig.sound()==beforeAuto);CHECK(p.redoSoundEdit());rig.refresh();
 const auto beforeScale=rig.sound();if(scale->onDragStart)scale->onDragStart();scale->setValue(.7,juce::sendNotificationSync);scale->setValue(.5,juce::sendNotificationSync);if(scale->onDragEnd)scale->onDragEnd();
 CHECK(std::abs(rig.sound().values[scaleIndex]-.25f)<1e-6f);CHECK(scaleEvents.starts==1&&scaleEvents.ends==1);CHECK(p.undoSoundEdit());CHECK(rig.sound()==beforeScale);CHECK(p.redoSoundEdit());rig.refresh();
 const auto beforeKey=rig.sound();CHECK(scale->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));CHECK(rig.sound().values[scaleIndex]>beforeKey.values[scaleIndex]);CHECK(p.undoSoundEdit());CHECK(rig.sound()==beforeKey);rig.refresh();
 const auto shown=rig.sound();RoutedPatch patch;CHECK(makeRoutedPatch(shown,patch));const auto response=g.responseAt(1000),expected=dynamicEqResponse(patch.effects[7].dynamicEq,48000,1000);CHECK(response&&expected&&std::abs(*response-*expected)<1e-12);
 auto proposed=std::make_shared<SoundDocument>(shown);proposed->values[scaleIndex]=.9f;g.showProposal(proposed);CHECK(g.proposed());CHECK(g.responseAt(1000,true).has_value());CHECK(rig.sound()==shown);g.showProposal(nullptr);
}
void gainControlCallbacksCanRetireGraph(){
 std::cout<<"SECTION gain_toolbar_callbacks_can_retire_graph_with_balanced_gestures\n";
 for(bool scalar:{false,true})for(int phase=0;phase<3;++phase){
  Rig rig;auto& p=*rig.processor;auto* graph=rig.graph.get();const auto index=dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot+(scalar?1:0);
  std::function<void()> action;
  if(scalar){auto* slider=dynamic_cast<juce::Slider*>(findComponent(*graph,"eq.gain_scale"));CHECK(slider);if(!slider)continue;slider->setValue(.5,juce::dontSendNotification);action=slider->onValueChange;}
  else {auto* button=dynamic_cast<juce::Button*>(findComponent(*graph,"eq.auto_gain"));CHECK(button);if(!button)continue;action=button->onClick;}
  Notifications event(p.control(index));
  if(phase==0)event.onStart=[&]{rig.graph.reset();};if(phase==1)event.onValue=[&]{rig.graph.reset();};if(phase==2)event.onEnd=[&]{rig.graph.reset();};
  CHECK(bool(action));if(action)action();CHECK(!rig.graph);CHECK(event.starts==1&&event.ends==1);CHECK(validateSoundDocument(rig.sound()));
  event.onStart=nullptr;event.onValue=nullptr;event.onEnd=nullptr;const bool opened=p.beginParameterGesture(0);CHECK(opened);if(opened)CHECK(p.endParameterGesture(0));
 }
}
void groupPointerWheelAndMarquee(){
 std::cout<<"SECTION group_shared_boundary_deltas_shape_relevance_marquee_and_one_undo\n";Rig rig;auto& g=*rig.graph;auto& p=*rig.processor;
 // Reuse a heap snapshot so Debug does not reserve a document for every temporary read.
 auto snapshot=std::make_unique<SoundDocument>();
 const auto readSound=[&]() -> const SoundDocument& {std::uint64_t revision;CHECK(p.trySoundSnapshot(*snapshot,revision));return *snapshot;};
 auto d=readSound();
 const auto index=[&](std::size_t band,std::size_t field){return rig.base+band*11+field;};
 const auto put=[&](std::size_t band,std::size_t field,float value){d.values[index(band,field)]=p.control(index(band,field)).convertTo0to1(value);};
 put(0,3,100);put(0,4,-6);put(0,5,1);put(1,0,1);put(1,3,1000);put(1,4,6);put(1,5,3);install(rig,d);
 g.setSelectedBands(std::bitset<8>(1));const auto second=g.nodePosition(1);
 Notifications frequency(p.control(index(0,3))),later(p.control(index(1,3)));
 g.mouseDown(mouse(g,second,second,true,juce::ModifierKeys::shiftModifier));g.mouseUp(mouse(g,second,second,false));
 CHECK(g.selectedBands()==std::bitset<8>(3));CHECK(frequency.starts==0&&later.starts==0);
 const auto before=readSound();const auto start=g.nodePosition(0);const auto upper=start+juce::Point<float>(10000,-10000);
 const auto value=[&](std::size_t band,std::size_t field){return p.control(index(band,field)).convertFrom0to1(readSound().values[index(band,field)]);};
 g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,upper,start));
 CHECK(std::abs(value(1,3)/value(0,3)-10)<.001f);CHECK(std::abs(value(1,3)-20000)<.1f);
 CHECK(std::abs(value(0,4)-12)<.001f);CHECK(std::abs(value(1,4)-24)<.001f);
 const auto upperSound=readSound();g.mouseDrag(mouse(g,upper,start));CHECK(readSound()==upperSound);
 const auto lower=start+juce::Point<float>(-10000,10000);g.mouseDrag(mouse(g,lower,start));g.mouseUp(mouse(g,lower,start,false));
 CHECK(std::abs(value(0,3)-20)<.001f);CHECK(std::abs(value(1,3)/value(0,3)-10)<.001f);
 CHECK(std::abs(value(0,4)+24)<.001f);CHECK(std::abs(value(1,4)+12)<.001f);
 CHECK(frequency.starts==1&&frequency.ends==1);CHECK(later.starts==1&&later.ends==1);
 const auto moved=readSound();CHECK(p.undoSoundEdit());CHECK(readSound()==before);CHECK(p.redoSoundEdit());CHECK(readSound()==moved);
 install(rig,before);g.setSelectedBands(std::bitset<8>(3));juce::MouseWheelDetails wheel;wheel.deltaY=10;
 const auto wheelBefore=readSound();g.mouseWheelMove(mouse(g,g.nodePosition(0),{},false),wheel);
 CHECK(std::abs(value(1,5)-18)<.001f);CHECK(std::abs(value(1,5)/value(0,5)-3)<.001f);
 CHECK(p.undoSoundEdit());CHECK(readSound()==wheelBefore);
 rig.refresh();g.setSelectedBands(std::bitset<8>(3));const auto keysBefore=readSound();CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
 CHECK(std::abs(value(1,3)/value(0,3)-10)<.001f);CHECK(readSound().values[index(0,3)]!=keysBefore.values[index(0,3)]);CHECK(readSound().values[index(1,3)]!=keysBefore.values[index(1,3)]);
 CHECK(p.undoSoundEdit());CHECK(readSound()==keysBefore);rig.refresh();g.setSelectedBands(std::bitset<8>(3));
 const auto bypassBefore=readSound();CHECK(bandMenuCommand(g,2));CHECK(readSound().values[index(0,1)]==1&&readSound().values[index(1,1)]==1);CHECK(p.undoSoundEdit());CHECK(readSound()==bypassBefore);
 // A notch follows frequency/Q; a cut follows frequency but retains gain/Q.
 d=before;put(1,2,5);put(2,0,1);put(2,2,3);put(2,3,5000);put(2,4,5);put(2,5,4);install(rig,d);g.setSelectedBands(std::bitset<8>(7));
 const auto mixed=readSound();const auto at=g.nodePosition(0);g.mouseDown(mouse(g,at,at));g.mouseDrag(mouse(g,at+juce::Point<float>(25,-10),at));g.mouseUp(mouse(g,at,at,false));
 CHECK(readSound().values[index(0,4)]!=mixed.values[index(0,4)]);CHECK(readSound().values[index(1,4)]==mixed.values[index(1,4)]);CHECK(readSound().values[index(2,4)]==mixed.values[index(2,4)]);
 wheel.deltaY=.2f;const auto qBefore=readSound();g.mouseWheelMove(mouse(g,g.nodePosition(0),{},false),wheel);
 CHECK(readSound().values[index(0,5)]!=qBefore.values[index(0,5)]);CHECK(readSound().values[index(1,5)]!=qBefore.values[index(1,5)]);CHECK(readSound().values[index(2,5)]==qBefore.values[index(2,5)]);
 // Marquee selects visible nodes without editing the sound.
 install(rig,before);g.setSelectedBands({});const auto a=g.nodePosition(0),b=g.nodePosition(1);const juce::Point<float> corner{a.x-10,b.y-10},end{b.x+10,a.y+10};
 const auto marqueeBefore=readSound();g.mouseDown(mouse(g,corner,corner));g.mouseDrag(mouse(g,end,corner));g.mouseUp(mouse(g,end,corner,false));
 CHECK(g.selectedBands()==std::bitset<8>(3));CHECK(readSound()==marqueeBefore);
 g.mouseDown(mouse(g,b,b,true,juce::ModifierKeys::shiftModifier));g.mouseUp(mouse(g,b,b,false));CHECK(g.selectedBands()==std::bitset<8>(1));
 // Delete affects both active flags in one history entry.
 g.setSelectedBands(std::bitset<8>(3));const auto deleteBefore=readSound();CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
 CHECK(readSound().values[index(0,0)]==0&&readSound().values[index(1,0)]==0);CHECK(p.undoSoundEdit());CHECK(readSound()==deleteBefore);
}
void groupAtomicLocksResetAndRetirement(){
 std::cout<<"SECTION group_locked_later_field_and_lifetime_end_without_partial_notifications\n";Rig rig;auto& g=*rig.graph;auto& p=*rig.processor;auto d=rig.sound();d.values[rig.base+11]=1;
 const auto later=rig.base+14;d.values[later]=p.control(later).convertTo0to1(3000);d.controlLocks[later]=true;install(rig,d);g.setSelectedBands(std::bitset<8>(3));
 Notifications firstEvents(p.control(rig.base+3)),laterEvents(p.control(later));const auto before=rig.sound();const auto at=g.nodePosition(0);
 g.mouseDown(mouse(g,at,at));g.mouseDrag(mouse(g,at+juce::Point<float>(50,-20),at));g.mouseUp(mouse(g,at,at,false));
 CHECK(rig.sound()==before);CHECK(firstEvents.starts==0&&laterEvents.starts==0);
 d.controlLocks[later]=false;install(rig,d);g.setSelectedBands(std::bitset<8>(3));const auto unlocked=rig.sound();
 laterEvents.onStart=[&]{install(rig,unlocked);};const auto start=g.nodePosition(0);g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,start+juce::Point<float>(50,-20),start));g.mouseUp(mouse(g,start,start,false));
 CHECK(rig.sound()==unlocked);CHECK(firstEvents.starts==firstEvents.ends&&laterEvents.starts==laterEvents.ends);CHECK(g.selectedBands().none());laterEvents.onStart=nullptr;
 g.setSelectedBands(std::bitset<8>(3));g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,start+juce::Point<float>(20,-10),start));
 g.giveAwayKeyboardFocus();g.focusLost(juce::Component::focusChangedDirectly);CHECK(g.selectedBands().none());CHECK(firstEvents.starts==firstEvents.ends&&laterEvents.starts==laterEvents.ends);
 CHECK(p.beginParameterGesture(rig.base+4));CHECK(p.endParameterGesture(rig.base+4));
 g.setSelectedBands(std::bitset<8>(3));g.mouseDown(mouse(g,g.nodePosition(0),g.nodePosition(0)));g.retire();CHECK(g.selectedBands().none());CHECK(firstEvents.starts==firstEvents.ends&&laterEvents.starts==laterEvents.ends);
}
void focusEndCanRetireGraph(){
 std::cout<<"SECTION focus_loss_end_callback_can_destroy_graph\n";Rig rig;auto& g=*rig.graph;Notifications events(rig.processor->control(rig.base+3));
 const auto start=g.nodePosition(0);g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,start+juce::Point<float>(10,-5),start));
 events.onEnd=[&]{rig.graph.reset();};g.focusLost(juce::Component::focusChangedDirectly);CHECK(!rig.graph);CHECK(events.starts==1&&events.ends==1);
}
void crossRackPasteAndClipboardLifetime(){
 std::cout<<"SECTION cross_rack_nonadjacent_paste_raw_macros_capacity_and_reentrant_clipboard\n";Rig source;auto src=source.sound();src.values[source.base+11]=1;src.values[source.base+3]=.2f;src.values[source.base+14]=.7f;src.macros[0]={parameterRegistry()[source.base+4].stateKey,.25f};install(source,src);
 source.graph->setSelectedBands(std::bitset<8>(3));juce::String text;CHECK(source.graph->copySelectedBands(text));
 juce::String localClipboard=text;std::function<void()> onRead;int reads=0,writes=0;
 DynamicEqGraph::Clipboard clipboard;clipboard.read=[&]{++reads;if(onRead)onRead();return localClipboard;};clipboard.write=[&](const auto& value){++writes;localClipboard=value;};Rig target(std::move(clipboard));auto& g=*target.graph;auto& p=*target.processor;
 auto dst=target.sound();dst.values[target.base]=0;dst.values[target.base+11]=1;dst.values[target.base+33]=1;dst.macros[0]={parameterRegistry()[target.base+4].stateKey,.1f};install(target,dst);const auto before=target.sound();
 std::vector<EqBandPatch> decoded;CHECK(decodeEqClipboard(text,decoded));EqPatch patch;SoundDocument expected;const bool prepared=makeEqPaste(before,7,decoded,patch)&&makeEqEdit(before,7,patch,expected);CHECK(prepared);
 Notifications first(p.control(target.base)),later(p.control(target.base+22));if(prepared){CHECK(g.pasteBands(text));CHECK(target.sound()==expected);CHECK(target.sound().macros==before.macros);CHECK(g.selectedBands()==std::bitset<8>(5));CHECK(p.undoSoundEdit());CHECK(target.sound()==before);CHECK(p.redoSoundEdit());CHECK(target.sound()==expected);}
 g.setSelectedBands(std::bitset<8>(1));CHECK(g.keyPressed(juce::KeyPress('C',juce::ModifierKeys::ctrlModifier,0)));CHECK(writes==1&&reads==0);CHECK(target.sound()==expected);
 localClipboard=text;install(target,before);const auto starts=first.starts+later.starts;
 onRead=[&]{install(target,before);};CHECK(g.keyPressed(juce::KeyPress('V',juce::ModifierKeys::ctrlModifier,0)));CHECK(reads==1);CHECK(target.sound()==before);CHECK(first.starts+later.starts==starts);onRead=nullptr;
 // Reset/rebind during clipboard access cannot redirect an old paste to another rack.
 onRead=[&]{auto replaced=before;replaced.personalities.reset();replaced.layout.effects[7].kind=EffectKind::delay;install(target,replaced);};
 CHECK(g.keyPressed(juce::KeyPress('V',juce::ModifierKeys::ctrlModifier,0)));CHECK(!g.boundSlot());CHECK(first.starts+later.starts==starts);onRead=nullptr;
 auto full=before;for(std::size_t band=0;band<8;++band)full.values[target.base+11*band]=1;install(target,full);const auto fullBefore=target.sound();CHECK(!g.pasteBands(text));CHECK(target.sound()==fullBefore);CHECK(g.getDescription().containsIgnoreCase("Paste refused"));CHECK(g.getDescription().containsIgnoreCase("0 available"));
 auto locked=before;locked.controlLocks[target.base+22+5]=true;install(target,locked);const auto lockedBefore=target.sound();const auto count=first.starts+later.starts;CHECK(!g.pasteBands(text));CHECK(target.sound()==lockedBefore);CHECK(first.starts+later.starts==count);CHECK(g.getDescription().containsIgnoreCase("locked"));
}
void popupClipboardCommandsAndStaleMenu(){
 std::cout<<"SECTION actual_popup_keyboard_copy_paste_and_stale_context\n";juce::String text;int reads=0,writes=0;
 DynamicEqGraph::Clipboard clipboard;clipboard.read=[&]{++reads;return text;};clipboard.write=[&](const auto& value){++writes;text=value;};Rig rig(std::move(clipboard));auto& g=*rig.graph;
 g.setSelectedBands(std::bitset<8>(1));const auto before=rig.sound();
 const auto popup=[&](){const auto at=g.plotBounds().getCentre();g.mouseDown(mouse(g,at,at,false,juce::ModifierKeys::rightButtonModifier));auto* menu=juce::Component::getCurrentlyModalComponent();CHECK(menu&&menu->getName()=="menu");return juce::Component::SafePointer<juce::Component>(menu);};
 if(auto menu=popup()){CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));CHECK(pumpUntil([&]{return writes==1;}));}CHECK(writes==1&&reads==0);CHECK(rig.sound()==before);
 std::vector<EqBandPatch> bands;CHECK(decodeEqClipboard(text,bands));EqPatch patch;SoundDocument expected;const bool prepared=makeEqPaste(before,7,bands,patch)&&makeEqEdit(before,7,patch,expected);CHECK(prepared);
 if(auto menu=popup()){CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));CHECK(pumpUntil([&]{return reads==1;}));}CHECK(reads==1);if(prepared)CHECK(rig.sound()==expected);
 g.setSelectedBands(std::bitset<8>(1));const auto current=rig.sound();if(auto menu=popup()){
  install(rig,current);CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;}));
 }CHECK(reads==1);CHECK(rig.sound()==current);
 g.setSelectedBands(std::bitset<8>(1));if(auto menu=popup()){g.retire();CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;}));}CHECK(writes==1&&reads==1);
 juce::PopupMenu::dismissAllActiveMenus();juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
}
void unrefreshedRecallAndPopupDoubleClick(){
 std::cout<<"SECTION identical_recall_before_ui_refresh_and_popup_double_click_are_inert\n";Rig rig;auto& g=*rig.graph;g.setSelectedBands(std::bitset<8>(1));juce::String text;CHECK(g.copySelectedBands(text));const auto before=rig.sound();
 juce::MemoryBlock wire;CHECK(encodeSoundState(before,wire));CHECK(rig.processor->restoreSoundState(wire.getData(),int(wire.getSize())));
 CHECK(!g.pasteBands(text));CHECK(rig.sound()==before);CHECK(g.selectedBands().none());install(rig,before);
 const auto at=g.plotBounds().getCentre();g.mouseDoubleClick(mouse(g,at,at,false,juce::ModifierKeys::rightButtonModifier));CHECK(rig.sound()==before);
}
void responseAndGestures(){
 std::cout<<"SECTION actual_graph_response_drag_host_gestures_and_undo\n";Rig rig;auto& g=*rig.graph;auto& p=*rig.processor;
 CHECK(g.boundSlot()==7);CHECK(!g.plotBounds().isEmpty());CHECK(g.parameters().size()==42);
 CHECK(g.parameters()[40]==dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot);CHECK(g.parameters()[41]==dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot+1);
 const auto response=g.responseAt(1000);CHECK(response&&std::abs(*response-std::complex<double>(1,0))<1e-9);
 const auto before=rig.sound();Notifications f(p.control(rig.base+3)),gain(p.control(rig.base+4));
 const auto start=g.nodePosition(0);const auto end=start+juce::Point<float>(100,-30);
 g.mouseDown(mouse(g,start,start));g.mouseDrag(mouse(g,end,start));g.mouseDrag(mouse(g,end+juce::Point<float>(10,-5),start));g.mouseUp(mouse(g,end,start,false));
 const auto after=rig.sound();CHECK(after.values[rig.base+3]!=before.values[rig.base+3]);CHECK(after.values[rig.base+4]!=before.values[rig.base+4]);
 CHECK(f.starts==1&&f.ends==1&&f.values>=1);CHECK(gain.starts==1&&gain.ends==1&&gain.values>=1);
 CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);CHECK(p.redoSoundEdit());CHECK(rig.sound()==after);rig.refresh();
 {const auto held=rig.sound().values[rig.base+5];juce::MouseWheelDetails latched;latched.deltaY=.1f;studio::noteRackScroll();g.mouseWheelMove(mouse(g,g.nodePosition(0),{},false),latched);CHECK(rig.sound().values[rig.base+5]==held);juce::Thread::sleep(450);}
 const auto oldQ=rig.sound().values[rig.base+5];juce::MouseWheelDetails wheel;wheel.deltaY=.1f;g.mouseWheelMove(mouse(g,g.nodePosition(0),{},false),wheel);CHECK(rig.sound().values[rig.base+5]!=oldQ);
 g.mouseDoubleClick(mouse(g,{240,145},{240,145}));CHECK(rig.sound().values[rig.base+11]==1);CHECK(g.selectedBand()==1);
 g.selectBand(7);CHECK(g.selectedBand()==7);const auto keyboardBefore=rig.sound();CHECK(g.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));CHECK(rig.sound().values[rig.base+7*11+3]!=keyboardBefore.values[rig.base+7*11+3]);
 const auto* model=studio::studioSkin();CHECK(model);if(model){studio::AllControlsPanel panel(p,*model,{});panel.show({{NativeModule::dynamicEq,7}});CHECK(panel.showParameter(rig.base+7*11+4));CHECK(panel.routesPage()==7);CHECK(!panel.showParameter(parameterCount));}
 CHECK(g.getAccessibilityHandler()!=nullptr);
}
void proposalAndLifetime(){
 std::cout<<"SECTION graph_proposal_macros_capture_and_retirement\n";Rig rig;auto& g=*rig.graph;const auto analysis=g.analysisHub();auto d=rig.sound();auto proposal=std::make_shared<SoundDocument>(d);proposal->values[rig.base+4]=.75f;
 g.showProposal(proposal);CHECK(g.proposed());CHECK(rig.sound()==d);const auto expected=dynamicEqResponse([]{DynamicEqSettings s;s.bands[0].active=true;s.bands[0].gainDb=12;return s;}(),48000,1000);
 const auto proposed=g.responseAt(1000,true);CHECK(proposed&&expected&&std::abs(*proposed-*expected)<1e-6);
 g.showProposal(nullptr);CHECK(!g.proposed());
 d.macros[0]={parameterRegistry()[rig.base+4].stateKey,.5f};
 // Macro1's registered value is used by makeRoutedPatch, just as by the audio.
 for(std::size_t i=0;i<parameterCount;++i)if(parameterRegistry()[i].id=="macro.1")d.values[i]=1;
 g.refresh(d,7);RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto actual=g.responseAt(1000),oracle=dynamicEqResponse(patch.effects[7].dynamicEq,48000,1000);CHECK(actual&&oracle&&std::abs(*actual-*oracle)<1e-8);
 g.setSpectrumOn(true);g.updateMonitoring();CHECK(g.spectrumOn());const auto token=g.subscription();CHECK(token);
 g.setSpectrumOn(false);CHECK(!g.subscription());
 juce::Button* listen=nullptr;for(auto* child:g.getChildren())if(child->getComponentID()=="eq.listen")listen=dynamic_cast<juce::Button*>(child);CHECK(listen);
 if(listen){const auto unchanged=rig.sound();listen->setState(juce::Button::buttonDown);const auto held=g.subscription();CHECK(held);if(held)CHECK(analysis->requestedAudition(*held)==g.selectedBand());CHECK(rig.sound()==unchanged);
  listen->setState(juce::Button::buttonNormal);CHECK(!g.subscription());if(held)CHECK(!analysis->requestedAudition(*held));
  // Synthetic release must start with the physical audition keys released too.
  CHECK(pumpUntil([]{return !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey)&&!juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);}));
  CHECK(static_cast<juce::Component*>(listen)->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));const auto keyToken=g.subscription();CHECK(keyToken);if(keyToken)CHECK(analysis->requestedAudition(*keyToken)==g.selectedBand());CHECK(static_cast<juce::Component*>(listen)->keyStateChanged(false));CHECK(!g.subscription());

  g.setSpectrumOn(true);listen->setState(juce::Button::buttonDown);CHECK(listen->getToggleState());rig.processor->fxMonitorHandle()->invalidate();g.updateMonitoring();CHECK(!listen->getToggleState());listen->setState(juce::Button::buttonNormal);g.setSpectrumOn(false);

  listen->setState(juce::Button::buttonDown);CHECK(g.subscription());g.setVisible(false);CHECK(!g.subscription());listen->setState(juce::Button::buttonNormal);g.setVisible(true);
 }

 g.setSpectrumOn(true);g.updateMonitoring();const auto next=g.subscription();CHECK(next);if(next&&token)CHECK(next->id!=token->id);
 const auto slotBypass=rackParameterIndex(NativeModule::dynamicEq,7,"bypassed");CHECK(slotBypass);if(slotBypass){rig.processor->control(*slotBypass).setValueNotifyingHost(1);rig.refresh();CHECK(listen&&!listen->isEnabled());CHECK(g.getDescription().containsIgnoreCase("slot bypassed"));rig.processor->control(*slotBypass).setValueNotifyingHost(0);rig.refresh();}
 g.retire();CHECK(!g.subscription());if(next)CHECK(analysis->generation(*next)==0);
 for(auto* child:g.getChildren())if(auto* button=dynamic_cast<juce::Button*>(child))CHECK(!button->onClick&&!button->onStateChange);
 g.refresh(d,0);CHECK(!g.boundSlot());
}
juce::Component* findComponent(juce::Component& root,const juce::String& id){if(root.getComponentID()==id)return &root;for(auto* child:root.getChildren())if(auto* found=findComponent(*child,id))return found;return nullptr;}
void spectrumOffRate(){
 std::cout<<"SECTION spectrum_off_response_tracks_actual_prepared_rate\n";Rig rig;auto& p=*rig.processor;auto& graph=*rig.graph;graph.setSpectrumOn(false);
 SoundDocument d;std::uint64_t revision;CHECK(p.trySoundSnapshot(d,revision));d.values[rig.base+3]=1;d.values[rig.base+4]=.75f;CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,revision));
 for(const auto rate:{8000.,96000.}){if(rate==96000){p.setNonRealtime(true);p.control(758).setValueNotifyingHost(1);}p.prepareToPlay(rate,64);d=rig.sound();graph.refresh(d,7);graph.updateMonitoring();
  RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));const auto actualRate=rate==96000?192000.:8000.;CHECK(p.fxMonitorHandle()->sampleRate()==actualRate);
  const auto actual=graph.responseAt(rate==8000?3000:17000),expected=dynamicEqResponse(patch.effects[7].dynamicEq,actualRate,rate==8000?3000:17000);CHECK(actual&&expected&&std::abs(*actual-*expected)<1e-7);CHECK(!graph.subscription());
 }
 p.releaseResources();
}
void discreteHostGestures(){
 std::cout<<"SECTION graph_band_toggle_and_remove_host_gestures\n";Rig rig;auto& graph=*rig.graph;const auto before=rig.sound();
 Notifications bypassCalls(rig.processor->control(rig.base+1)),activeCalls(rig.processor->control(rig.base));
 CHECK(bandMenuCommand(graph,2));CHECK(rig.sound().values[rig.base+1]==1);CHECK(bypassCalls.starts==1&&bypassCalls.ends==1&&bypassCalls.values>0);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);
 rig.refresh();CHECK(graph.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));const auto removed=rig.sound();CHECK(removed.values[rig.base]==0);CHECK(removed.values[rig.base+3]==before.values[rig.base+3]);CHECK(activeCalls.starts==1&&activeCalls.ends==1);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);
}
void retiredRowEndsGesture(){
 std::cout<<"SECTION retired_row_ends_held_gesture_before_processor_teardown\n";Rig rig;const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 RackPanelComponent row(*rig.processor,*model);row.setPlace(RackPanelComponent::Place::unit);const auto d=rig.sound();CHECK(d.personalities);if(!d.personalities)return;row.showInstance(d.personalities->instances.front().id);row.refresh(d);
 auto* slider=dynamic_cast<juce::Slider*>(findComponent(row,"rack.focus.0"));CHECK(slider);if(!slider)return;
 Notifications frequency(rig.processor->control(rig.base+3));slider->onDragStart();CHECK(frequency.starts==1);row.retire();CHECK(frequency.ends==1);slider->onDragEnd();CHECK(frequency.ends==1);
 CHECK(rig.processor->beginParameterGesture(rig.base+4));CHECK(rig.processor->endParameterGesture(rig.base+4));
}
void numericRelevance(){
 std::cout<<"SECTION retained_numeric_fields_follow_filter_shape\n";Rig rig;const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::dynamicEq,7}});auto d=rig.sound();panel.refresh(d);
 auto* slope=panel.knob(rig.base+6);auto* gain=panel.knob(rig.base+4);auto* q=panel.knob(rig.base+5);auto* range=panel.knob(rig.base+7);CHECK(slope&&gain&&q&&range);if(!slope||!gain||!q||!range)return;
 CHECK(slope->getAlpha()<.6f&&slope->label.getAlpha()<.6f);CHECK(gain->getAlpha()==1&&q->getAlpha()==1&&range->getAlpha()==1);CHECK(slope->isEnabled());
 d.values[rig.base+2]=rig.processor->control(rig.base+2).convertTo0to1(3);panel.refresh(d);
 CHECK(slope->getAlpha()==1);CHECK(gain->getAlpha()<.6f&&q->getAlpha()<.6f&&range->getAlpha()<.6f);CHECK(gain->isEnabled()&&range->isEnabled());
 CHECK(gain->getDescription().containsIgnoreCase("retained"));
 studio::ProgrammerUnit advanced(*rig.processor,*model,{});advanced.refresh(d);CHECK(advanced.knob(rig.base+4)->getAlpha()<.6f);CHECK(advanced.knob(rig.base+6)->getAlpha()==1);
}
void closingEndsSelection(){
 std::cout<<"SECTION reentrant_closing_during_selection_gesture_end\n";Rig rig;auto& graph=*rig.graph;const auto start=graph.nodePosition(0);Notifications f(rig.processor->control(rig.base+3));
 graph.mouseDown(mouse(graph,start,start));graph.mouseDrag(mouse(graph,start+juce::Point<float>(30,-10),start));
 int callbacks=0;graph.onBandSelected=[&](std::size_t){++callbacks;};f.onEnd=[&]{rig.closing=true;};
 graph.selectBand(1);CHECK(graph.selectedBand()==0);CHECK(callbacks==0);CHECK(f.starts==1&&f.ends==1);
}
void snapshotImage(const char* name,juce::Component& component){
 if(images==juce::File{})return;CHECK(images.createDirectory().wasOk());const auto file=images.getChildFile(name);CHECK(!file.existsAsFile());if(file.exists())return;juce::FileOutputStream out(file);CHECK(out.openedOk());CHECK(juce::PNGImageFormat().writeImageToStream(component.createComponentSnapshot(component.getLocalBounds(),true,1.f),out));
}
void showcase(Rig& rig,juce::AudioProcessorEditor& editor,DynamicEqGraph& graph,RackStack& stack){
 if(images==juce::File{})return;
 // Record the specified empty factory state, then restore the fixture's first
 // added band before configuring the musical demonstration.
 CHECK(rig.processor->undoSoundEdit());juce::MessageManager::getInstance()->runDispatchLoopUntil(60);graph.selectBand(0);stack.closeDrawer();snapshotImage("empty-rack-graph.png",graph);CHECK(rig.processor->redoSoundEdit());
 SoundDocument d;std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));auto& p=*rig.processor;
 for(std::size_t i=0;i<parameterCount;++i)if(parameterRegistry()[i].id=="synth.1.enabled")d.values[i]=1;
 const auto put=[&](int b,int field,float value){const auto index=rig.base+std::size_t(b)*11+field;d.values[index]=p.control(index).convertTo0to1(value);};
 const float frequencies[]{120,700,3200,9000,30},gains[]{-4,3,-3,2,0},ranges[]{-6,-4,4,-3,0};
 for(int b=0;b<5;++b){put(b,0,1);put(b,3,frequencies[b]);put(b,4,gains[b]);put(b,5,b==1?2.f:1.f);put(b,7,ranges[b]);put(b,8,-60);}
 put(3,2,2);put(4,2,3);put(4,6,1);CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,revision));
 juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.closeDrawer();stack.reveal(stack.selected());graph.selectBand(1);stack.closeDrawer();
 p.prepareToPlay(48000,64);juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,57,.8f),0);
 for(int block=0;block<512;++block){p.processBlock(audio,midi);midi.clear();if(block%16==15){graph.analysisHub()->update();stack.refreshAnalysis();}}
 CHECK(graph.spectrum().valid);CHECK(graph.spectrum().windows>0);CHECK(std::any_of(graph.spectrum().movementDb.begin(),graph.spectrum().movementDb.end(),[](float gain){return std::abs(gain)>.1f;}));
 snapshotImage("graph-spectrum.png",graph);snapshotImage("minimum-window.png",editor);
 if(images!=juce::File{}) [&]{
  const int originalWidth=editor.getWidth(),originalHeight=editor.getHeight();
  // Offline rendering above does not pump the editor timer. Synchronize the
  // image fixture before capturing A; a stale live view must still refuse it.
  const auto before=rig.sound();graph.refresh(before,7);graph.clearComparison();CHECK(graph.storeComparison(false));auto altered=before;std::uint64_t rev;SoundDocument current;CHECK(p.trySoundSnapshot(current,rev));
  const auto automatic=dynamicEqGainParameterStart+7*dynamicEqGainParametersPerSlot;altered.values[automatic]=1;altered.values[automatic+1]=.75f;
  CHECK(p.applySoundEdit(altered,SoundEditOrigin::manual,rev));graph.refresh(rig.sound(),7);CHECK(graph.storeComparison(true));CHECK(graph.recallComparison(true));
  for(int block=0;block<64;++block){p.processBlock(audio,midi);if(block%16==15){graph.analysisHub()->update();stack.refreshAnalysis();}}
  snapshotImage("comparison-minimum-editor.png",editor);snapshotImage("comparison-gain-graph.png",graph);
  if(auto* more=dynamic_cast<juce::Button*>(findComponent(graph,"eq.more"))){
   more->triggerClick();CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()!=nullptr;}));
   if(auto* popup=juce::Component::getCurrentlyModalComponent()){snapshotImage("comparison-band-menu.png",*popup);CHECK(popup->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));}
   CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;}));juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  }else CHECK(false);
  editor.setSize(1800,1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());snapshotImage("comparison-expanded-editor.png",editor);
  CHECK(graph.recallComparison(false));CHECK(rig.sound()==before);
  for(int block=0;block<64;++block){p.processBlock(audio,midi);if(block%16==15){graph.analysisHub()->update();stack.refreshAnalysis();}}
  auto locked=before;CHECK(p.trySoundSnapshot(current,rev));locked.controlLocks[rig.base+4]=true;
  CHECK(p.applySoundEdit(locked,SoundEditOrigin::manual,rev));graph.refresh(rig.sound(),7);CHECK(!graph.recallComparison(true));snapshotImage("comparison-locked-editor.png",editor);
  CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);graph.refresh(rig.sound(),7);CHECK(graph.recallComparison(false));graph.clearComparison();
  editor.setSize(originalWidth,originalHeight);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());
 }();
 if(images!=juce::File{}) [&]{
  const auto before=rig.sound();auto raw=before;raw.macros={};RoutedPatch routed;CHECK(makeRoutedPatch(raw,routed));auto desired=routed.effects[7].dynamicEq;
  desired.bands[5]=DynamicEqBand{};desired.bands[5].active=true;desired.bands[5].frequencyHz=1400;desired.bands[5].gainDb=-5;desired.bands[5].q=.7071f;
  EqCurve target;for(std::size_t i=0;i<target.db.size();++i){const auto response=dynamicEqResponse(desired,48000,20.*std::pow(1000.,double(i)/128.));CHECK(response);if(response)target.db[i]=float(20.*std::log10(std::max(std::abs(*response),1e-12)));}
  graph.refresh(before,7);graph.setSketchEnabled(true);CHECK(graph.previewSketch(target));CHECK(waitForSketch(graph));CHECK(rig.sound()==before);
  snapshotImage("sketch-minimum-editor.png",editor);snapshotImage("sketch-preview-graph.png",graph);graph.cancelSketch();
  const int w=editor.getWidth(),h=editor.getHeight();editor.setSize(1800,1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());
  graph.refresh(before,7);graph.setSketchEnabled(true);CHECK(graph.previewSketch(target));CHECK(waitForSketch(graph));snapshotImage("sketch-expanded-editor.png",editor);CHECK(rig.sound()==before);graph.cancelSketch();
  editor.setSize(w,h);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());graph.selectBand(1);
 }();

 if(images!=juce::File{}) [&]{
  const auto before=rig.sound();auto mixed=before;std::uint64_t revision;SoundDocument current;CHECK(p.trySoundSnapshot(current,revision));
  for(std::size_t band=0;band<4;++band)mixed.values[dynamicEqPlacementParameterStart+7*8+band]=float(band+1)/4;
  CHECK(p.applySoundEdit(mixed,SoundEditOrigin::manual,revision));graph.refresh(rig.sound(),7);graph.selectBand(3);stack.closeDrawer();
  for(int block=0;block<128;++block){p.processBlock(audio,midi);if(block%16==15){graph.analysisHub()->update();stack.refreshAnalysis();}}
  const int w=editor.getWidth(),h=editor.getHeight();editor.setSize(1080,680);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());
  snapshotImage("placement-minimum-editor.png",editor);snapshotImage("placement-mixed-graph.png",graph);
  editor.setSize(1800,1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());snapshotImage("placement-expanded-editor.png",editor);
  const char* names[]{"placement-left-graph.png","placement-right-graph.png","placement-mid-graph.png","placement-side-graph.png"};
  for(std::size_t band=0;band<4;++band){graph.selectBand(band);stack.closeDrawer();snapshotImage(names[band],graph);}
  const auto placement=dynamicEqPlacementParameterStart+7*8+3;
  for(const bool minimum:{true,false}){
   editor.setSize(minimum?1080:1800,minimum?680:1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
   graph.selectBand(3);CHECK(stack.controls().showParameter(placement));
   if(auto* knob=stack.controls().knob(placement)){knob->grabKeyboardFocus();juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
    auto& viewport=stack.viewport();const auto label=viewport.getLocalArea(&knob->label,knob->label.getLocalBounds());
    viewport.setViewPosition(viewport.getViewPositionX(),viewport.getViewPositionY()+std::max(0,label.getBottom()+20-viewport.getViewHeight()));
    const auto knobs=stack.controls().shownKnobs();CHECK(knobs.size()==12);
    for(const auto* shown:knobs){CHECK(viewport.getLocalBounds().contains(viewport.getLocalArea(shown,shown->getLocalBounds())));
     CHECK(viewport.getLocalBounds().contains(viewport.getLocalArea(&shown->label,shown->label.getLocalBounds())));CHECK(viewport.getLocalBounds().contains(viewport.getLocalArea(&shown->entry,shown->entry.getLocalBounds())));}}
   else CHECK(false);
   auto* details=dynamic_cast<juce::Button*>(findComponent(editor,"rack.drawer.details"));CHECK(details);
   if(details){auto* footer=details->findParentComponentOfClass<RackPanelComponent>();CHECK(footer);
    if(footer){const auto beforeFooter=rig.sound();CHECK(footer->drawerHeight()<=64);CHECK(std::abs(footer->getHeight()-44)<=1);details->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
     CHECK(footer->drawerHeight()>=400);
     for(const auto* id:{"rack.operation","rack.apply"}){auto* field=findComponent(*footer,id);CHECK(field);if(field){CHECK(field->isVisible());CHECK(footer->getLocalBounds().contains(footer->getLocalArea(field,field->getLocalBounds())));}}
     if(auto* header=findComponent(*footer,"rack.header"))CHECK(!footer->getLocalArea(header,header->getLocalBounds()).intersects(footer->getLocalArea(details,details->getLocalBounds())));
     snapshotImage(minimum?"placement-design-minimum-footer.png":"placement-design-expanded-footer.png",*footer);
     details->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(30);CHECK(footer->drawerHeight()<=64);CHECK(std::abs(footer->getHeight()-44)<=1);CHECK(rig.sound()==beforeFooter);}}
   if(auto* viewport=dynamic_cast<juce::Viewport*>(findComponent(editor,"rack.stack.viewport"))){if(auto* body=viewport->getViewedComponent()){
    const auto graphBounds=body->getLocalArea(&graph,graph.getLocalBounds());viewport->setViewPosition(0,std::max(0,graphBounds.getY()-8));
    for(const auto* shown:stack.controls().shownKnobs())CHECK(viewport->getLocalBounds().contains(viewport->getLocalArea(&shown->entry,shown->entry.getLocalBounds())));}}
   snapshotImage(minimum?"placement-controls-minimum-editor.png":"placement-controls-editor.png",editor);stack.closeDrawer();
  }
  auto raw=mixed;raw.macros={};RoutedPatch patch;CHECK(makeRoutedPatch(raw,patch));auto desired=patch.effects[7].dynamicEq;desired.bands[5]={};desired.bands[5].active=true;desired.bands[5].frequencyHz=1400;desired.bands[5].gainDb=-5;desired.bands[5].q=.7071f;
  EqCurve target;for(std::size_t i=0;i<129;++i){const auto value=dynamicEqEnergyResponse(desired,48000,20.*std::pow(1000.,double(i)/128));CHECK(value);if(value)target.db[i]=float(20.*std::log10(std::max(*value,1.e-12)));}
  graph.setSketchEnabled(true);CHECK(graph.previewSketch(target));CHECK(waitForSketch(graph));CHECK(rig.sound()==mixed);snapshotImage("placement-sketch-energy.png",graph);graph.cancelSketch();
  CHECK(p.trySoundSnapshot(current,revision));CHECK(p.applySoundEdit(before,SoundEditOrigin::manual,revision));graph.refresh(rig.sound(),7);
  for(int block=0;block<64;++block){p.processBlock(audio,midi);if(block%16==15){graph.analysisHub()->update();stack.refreshAnalysis();}}
  editor.setSize(w,h);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());graph.selectBand(1);stack.closeDrawer();
 }();
 if(images!=juce::File{}) [&]{
  graph.setSelectedBands(std::bitset<8>(7));snapshotImage("multi-band-minimum-editor.png",editor);snapshotImage("multi-band-selection-graph.png",graph);
  const auto before=rig.sound();const auto start=graph.nodePosition(0),end=start+juce::Point<float>(graph.plotBounds().getWidth()*2,-10);
  graph.mouseDown(mouse(graph,start,start));graph.mouseDrag(mouse(graph,end,start));graph.mouseUp(mouse(graph,end,start,false));snapshotImage("multi-band-boundary-editor.png",editor);
  CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);graph.refresh(rig.sound(),7);graph.setSelectedBands(std::bitset<8>(7));
  juce::String text;CHECK(graph.copySelectedBands(text));auto full=rig.sound();std::uint64_t edit;SoundDocument live;CHECK(p.trySoundSnapshot(live,edit));
  for(std::size_t n=0;n<8;++n)full.values[rig.base+n*11]=1;CHECK(p.applySoundEdit(full,SoundEditOrigin::manual,edit));graph.refresh(rig.sound(),7);
  const auto fullBefore=rig.sound();CHECK(!graph.pasteBands(text));CHECK(rig.sound()==fullBefore);snapshotImage("multi-band-paste-refusal-editor.png",editor);
  CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);graph.refresh(rig.sound(),7);
 }();
 if(images!=juce::File{}){const auto before=rig.sound();graph.updateMonitoring();graph.analysisHub()->update();stack.refreshAnalysis();graph.setGrabEnabled(true);CHECK(graph.grabEnabled());CHECK(graph.grabPeakCount()>0);snapshotImage("grab-minimum-editor.png",editor);graph.setGrabEnabled(false);CHECK(rig.sound()==before);}

 editor.setSize(1800,1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);stack.reveal(stack.selected());snapshotImage("expanded-rack.png",editor);

 if(images!=juce::File{}){graph.setSelectedBands(std::bitset<8>(7));snapshotImage("multi-band-expanded-editor.png",editor);}
 if(images!=juce::File{}){const auto before=rig.sound();graph.setGrabEnabled(true);CHECK(graph.grabPeakCount()>0);graph.keyPressed(juce::KeyPress(juce::KeyPress::downKey));snapshotImage("grab-expanded-preview-editor.png",editor);graph.setGrabEnabled(false);CHECK(rig.sound()==before);}
 graph.selectBand(1);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);snapshotImage("band-numeric-controls.png",editor);
 auto proposal=std::make_shared<SoundDocument>(rig.sound());proposal->values[rig.base+4]=p.control(rig.base+4).convertTo0to1(4);
 // An AI proposal may add a band as well as move an existing one. Preserve
 // the unmodified document while showing both proposed identities.
 proposal->values[rig.base+7*11]=1;proposal->values[rig.base+7*11+3]=p.control(rig.base+7*11+3).convertTo0to1(5000);proposal->values[rig.base+7*11+4]=p.control(rig.base+7*11+4).convertTo0to1(5);
 const auto beforeProposal=rig.sound();graph.showProposal(proposal);CHECK(graph.proposed());CHECK(rig.sound()==beforeProposal);snapshotImage("graph-proposal.png",graph);graph.showProposal(nullptr);
 if(auto* ai=dynamic_cast<juce::Button*>(findComponent(editor,"ai.toggle"))){ai->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(70);snapshotImage("side-panel-narrow.png",editor);ai->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(70);}else CHECK(false);
 if(auto* unit=stack.unit(stack.selected())){unit->setFolded(true);if(unit->onLayout)unit->onLayout();snapshotImage("folded-rack.png",editor);unit->setFolded(false);if(unit->onLayout)unit->onLayout();stack.reveal(unit->id());}
 p.releaseResources();
}
void spectralActualEditor(){
 std::cout<<"SECTION spectral_actual_editor_bindings_latency_and_geometry\n";
 Rig rig;auto& p=*rig.processor;auto d=rig.sound();const auto mode=dynamicEqSpectralParameterStart+7*25;
 d.values[mode]=1;d.values[mode+1]=1;d.values[rig.base+7]=p.control(rig.base+7).convertTo0to1(-6);
 d.values[rig.base+8]=p.control(rig.base+8).convertTo0to1(-48);install(rig,d);rig.graph.reset();p.prepareToPlay(48000,64);
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;
 RackStack* stack=nullptr;for(auto* c=graph->getParentComponent();c;c=c->getParentComponent())if((stack=dynamic_cast<RackStack*>(c)))break;CHECK(stack);if(!stack)return;
 auto& panel=stack->controls();stack->reveal(stack->selected());graph->selectBand(0);CHECK(bool(graph->onSpectral));if(graph->onSpectral)graph->onSpectral();CHECK(panel.spectralPage());
 graph->setSpectrumOn(true);juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
 for(int block=0;block<240;++block){p.processBlock(audio,midi);midi.clear();graph->updateMonitoring();}
 for(const bool minimum:{true,false}){
  editor->setSize(minimum?1080:1800,minimum?680:1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
  const auto box=stack->body().getLocalArea(graph,graph->getLocalBounds());stack->viewport().setViewPosition(0,std::max(0,box.getY()-8));
  CHECK(panel.shownKnobs().size()==4);CHECK(p.getLatencySamples()==5120);
  const auto* choice=dynamic_cast<juce::ComboBox*>(findComponent(panel,"rack.controls.band"));CHECK(choice&&choice->getText()=="SPECTRAL 1");
  for(auto* knob:panel.shownKnobs())for(auto* item:std::array<juce::Component*,3>{knob,&knob->label,&knob->entry})
   CHECK(stack->viewport().getLocalBounds().contains(stack->viewport().getLocalArea(item,item->getLocalBounds())));
  const auto* page=panel.knob(mode+1)->getParentComponent();CHECK(page);
  if(page)for(const auto* id:{"eq.spectral.eligibility","eq.spectral.effective","eq.spectral.pending"}){
   auto* label=dynamic_cast<juce::Label*>(findComponent(*const_cast<juce::Component*>(page),id));CHECK(label);
   if(label){CHECK(page->getLocalBounds().reduced(8).contains(label->getBounds()));CHECK(juce::GlyphArrangement::getStringWidth(label->getFont(),label->getText())<=label->getWidth()-8);}
  }
  snapshotImage(minimum?"spectral-minimum-editor.png":"spectral-expanded-editor.png",*editor);
 }
 graph->selectBand(7);CHECK(panel.routesPage()==25);graph->selectBand(0);CHECK(panel.routesPage()==18);
 d=rig.sound();d.values[mode]=0;install(rig,d);p.processBlock(audio,midi);graph->updateMonitoring();juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
 CHECK(p.eqPreparationPending(7)&&p.getLatencySamples()==5120);snapshotImage("spectral-pending-editor.png",*editor);
 p.releaseResources();p.prepareToPlay(48000,64);d=rig.sound();d.values[dynamicEqDetectorParameterStart+7*40+2]=1;install(rig,d);
 juce::MessageManager::getInstance()->runDispatchLoopUntil(60);snapshotImage("spectral-unavailable-editor.png",*editor);
 p.releaseResources();
}
void detectorActualEditor(){
 std::cout<<"SECTION detector_actual_editor_meters_navigation_visibility_and_bounds\n";
 Rig rig;auto& p=*rig.processor;const auto first=dynamicEqDetectorParameterStart+7*40;auto d=rig.sound();
 d.values[first]=d.values[first+1]=d.values[first+2]=1;d.values[first+3]=p.control(first+3).convertTo0to1(60);d.values[first+4]=p.control(first+4).convertTo0to1(180);install(rig,d);rig.graph.reset();
 auto buses=p.getBusesLayout();buses.inputBuses.set(0,juce::AudioChannelSet::stereo());CHECK(p.setBusesLayout(buses));p.prepareToPlay(48000,64);
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;
 RackStack* stack=nullptr;for(auto* c=graph->getParentComponent();c;c=c->getParentComponent())if((stack=dynamic_cast<RackStack*>(c)))break;CHECK(stack);if(!stack)return;
 graph->selectBand(0);auto& panel=stack->controls();CHECK(panel.showParameter(first+2));graph->setSpectrumOn(false);graph->updateMonitoring();CHECK(graph->detectorAuditionMode()&&graph->subscription());
 juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;int frame=0;
 const auto render=[&]{for(int block=0;block<96;++block){for(int n=0;n<64;++n,++frame){const float value=float(.5*std::sin(2*juce::MathConstants<double>::pi*100*frame/48000));audio.setSample(0,n,value);audio.setSample(1,n,-.7f*value);}p.processBlock(audio,midi);graph->updateMonitoring();}juce::MessageManager::getInstance()->runDispatchLoopUntil(50);};
 for(bool minimum:{true,false}){
  editor->setSize(minimum?1080:1800,minimum?680:1100);graph->selectBand(0);CHECK(panel.showParameter(first+2));render();
  const auto graphArea=stack->body().getLocalArea(graph,graph->getLocalBounds());stack->viewport().setViewPosition(0,std::max(0,graphArea.getY()-8));
  auto* threshold=dynamic_cast<juce::Label*>(findComponent(panel,"eq.detector.live.threshold"));CHECK(threshold);if(threshold)CHECK(threshold->getText()!="--");
  // Readouts belong to the visible page, including when another detector page preceded it.
  auto* page=panel.knob(first)->getParentComponent();CHECK(page);if(page){auto* live=dynamic_cast<juce::Label*>(findComponent(*page,"eq.detector.live.threshold"));CHECK(live);if(live)CHECK(live->getText()==juce::String(graph->spectrum().detectorReadbacks[0].thresholdDb,1)+" dB");
   for(const char* suffix:{"threshold","attack","release"}){auto* label=dynamic_cast<juce::Label*>(findComponent(*page,juce::String("eq.detector.live.")+suffix));CHECK(label);if(label){CHECK(page->getLocalBounds().reduced(8).contains(label->getBounds()));CHECK(juce::GlyphArrangement::getStringWidth(label->getFont(),label->getText())<=label->getWidth()-8);}}
  }
  CHECK(!graph->spectrum().valid&&graph->spectrum().windows==0);
  for(auto* knob:panel.shownKnobs())for(auto* item:std::array<juce::Component*,3>{knob,&knob->label,&knob->entry})CHECK(stack->viewport().getLocalBounds().contains(stack->viewport().getLocalArea(item,item->getLocalBounds())));
  snapshotImage(minimum?"detector-minimum-editor.png":"detector-expanded-editor.png",*editor);
 }
 auto* choice=dynamic_cast<juce::ComboBox*>(findComponent(panel,"rack.controls.band"));CHECK(choice);
 if(choice){choice->setSelectedId(9,juce::sendNotificationSync);CHECK(!graph->detectorAuditionMode()&&!graph->subscription());choice->setSelectedId(11,juce::sendNotificationSync);CHECK(graph->detectorAuditionMode());graph->selectBand(7);CHECK(panel.routesPage()==17&&choice->getSelectedId()==18);graph->selectBand(0);CHECK(panel.routesPage()==10);}
 p.releaseResources();buses.inputBuses.set(0,juce::AudioChannelSet::disabled());CHECK(p.setBusesLayout(buses));p.prepareToPlay(48000,64);render();
 auto* status=dynamic_cast<juce::Label*>(findComponent(*panel.knob(first)->getParentComponent(),"eq.detector.status"));CHECK(status);if(status)CHECK(status->getText().containsIgnoreCase("unavailable"));auto* listen=dynamic_cast<juce::Button*>(findComponent(*graph,"eq.listen"));CHECK(listen&&!listen->isEnabled());snapshotImage("detector-external-unavailable-editor.png",*editor);
 editor->setVisible(false);graph->updateMonitoring();CHECK(!graph->subscription()&&!graph->spectrum().detectorReadbacksValid);CHECK(rig.sound()==d);p.releaseResources();
}
void overviewInActualEditor(bool desktop=true,EqOverviewPeers* peers=nullptr){
 std::cout<<"SECTION overview_actual_editor_open_close_focus_and_audio_state\n";Rig rig({},desktop,peers?std::optional(peers->options()):std::nullopt);rig.graph.reset();
 auto d=rig.sound();std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));
 for(std::uint8_t slot=0;slot<8;++slot){d.layout.effects[slot]={SlotUse::present,EffectKind::dynamicEq,masterChain};d.layout.effectOrder[masterChain][slot]=slot;}d.layout.effectCount[masterChain]=8;
 for(std::size_t i=0;i<parameterCount;++i)if(parameterRegistry()[i].id=="synth.1.enabled")d.values[i]=1;
 CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));
 if(peers)CHECK(peers->attach(*rig.processor));
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;if(desktop)editor->addToDesktop(0);editor->setVisible(true);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;auto* stack=graph->findParentComponentOfClass<RackStack>();CHECK(stack);if(!stack)return;
 const auto before=rig.sound();const auto scroll=stack->viewport().getViewPosition();auto* more=findComponent(*graph,"eq.more");CHECK(more);if(desktop&&more)more->grabKeyboardFocus();
 CHECK(graph->onOverview);if(!graph->onOverview)return;graph->onOverview();
 auto* overview=dynamic_cast<studio::EqOverview*>(findComponent(*editor,"eq.overview"));CHECK(overview&&overview->isVisible());if(!overview)return;if(desktop)CHECK(overview->isShowing());
 CHECK(overview->referenceSlot()==7);CHECK(!stack->viewport().isVisible());CHECK(overview->localCount()==8);CHECK(overview->statusText().containsIgnoreCase("off"));CHECK(overview->setComparisonSlot(0));
 const auto selection=rig.processor->groupWorkflowView().selection;juce::MemoryBlock saved;rig.processor->getStateInformation(saved);
 if(peers){CHECK(overview->setGroupEnabled(true));CHECK(overview->setPeerSelection(peers->members));CHECK(overview->setComparisonPeer(peers->members[0]));
  CHECK(peers->wait(*rig.processor,[&]{overview->updateMonitoring();for(int i=0;i<4;++i){const auto* row=findComponent(*overview,"eq.overview.peer."+juce::String(i));if(!row||!row->isVisible()||!row->getTitle().contains("EQ8")||row->getTitle().containsIgnoreCase("unavailable"))return false;}return true;}));
  CHECK(overview->peerCount()==4);const std::array<GroupUuid,5> excessive{peers->members[0],peers->members[1],peers->members[2],peers->members[3],rig.processor->groupLinkView().intent->member};CHECK(!overview->setPeerSelection(excessive));
 }
 auto& p=*rig.processor;p.prepareToPlay(48000,512);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,57,.8f),0);
 for(int block=0;block<128;++block){p.processBlock(audio,midi);midi.clear();if(block%16==15){graph->analysisHub()->update();stack->refreshAnalysis();if(peers)peers->pump(p);}}
 // Offline audio rendering above blocks the control/message thread. Restore
 // real client polling and fresh peer windows before taking live-view images.
 if(peers)CHECK(peers->wait(p,[&]{overview->updateMonitoring();for(int i=0;i<4;++i){const auto* row=findComponent(*overview,"eq.overview.peer."+juce::String(i));if(!row||row->getTitle().containsIgnoreCase("unavailable"))return false;}return true;}));
 if(desktop){CHECK(overview->localAvailable(7));CHECK(overview->potentialOverlap()&&overview->potentialOverlap()->any());}else{CHECK(!overview->localAvailable(7));CHECK(!overview->potentialOverlap());}
 for(const auto size:{juce::Point<int>(1080,680),juce::Point<int>(1800,1100)}){editor->setSize(size.x,size.y);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);CHECK(editor->getLocalBounds().contains(editor->getLocalArea(overview,overview->getLocalBounds())));
  for(const auto* id:{"group","send","back","add","input","output"}){auto* key=findComponent(*overview,juce::String("eq.overview.")+id);CHECK(key);if(key)CHECK(overview->getLocalBounds().contains(key->getBounds()));}
  snapshotImage(size.x==1080?"overview-minimum-editor.png":"overview-expanded-editor.png",*editor);}
 juce::MouseWheelDetails zoom;zoom.deltaY=1;overview->mouseWheelMove(mouse(*overview,overview->plotBounds().getCentre(),{},false),zoom);snapshotImage("overview-zoom-editor.png",*editor);overview->mouseDoubleClick(mouse(*overview,overview->plotBounds().getCentre(),{},false));
 if(peers){peers->senders[0]->closeEqSpectrumView(peers->owners[0]);peers->owners[0]=0;
  CHECK(peers->wait(p,[&]{overview->updateMonitoring();const auto* row=findComponent(*overview,"eq.overview.peer.0");return row&&row->getTitle().containsIgnoreCase("unavailable");}));CHECK(!overview->potentialOverlap());CHECK(overview->getDescription().containsIgnoreCase("unavailable"));snapshotImage("overview-unavailable-peer-editor.png",*editor);
  auto reversed=peers->members;std::reverse(reversed.begin(),reversed.end());CHECK(overview->setPeerSelection(reversed));const auto* first=findComponent(*overview,"eq.overview.peer.0");CHECK(first&&first->getTitle().contains("Source 4"));CHECK(peers->providerJobs.load()==0);
 }
 juce::MemoryBlock after;p.getStateInformation(after);CHECK(after==saved);CHECK(p.groupWorkflowView().selection==selection);
 CHECK(overview->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));CHECK(!overview->isVisible());CHECK(stack->viewport().isVisible()&&stack->viewport().getViewPosition()==scroll);if(desktop)CHECK(more&&more->hasKeyboardFocus(false));CHECK(rig.sound()==before);
 graph->onOverview();CHECK(overview->isVisible());const auto hub=graph->analysisHub();editor.reset();CHECK(hub->activeAnalyzers()==0);CHECK(rig.sound()==before);
}
void matchInActualEditor(){
 std::cout<<"SECTION match_actual_editor_capture_preview_fit_apply_and_navigation\n";Rig rig({},false);rig.graph.reset();auto& p=*rig.processor;auto d=rig.sound();std::uint64_t revision;CHECK(p.trySoundSnapshot(d,revision));
 const auto put=[&](const juce::String& key,float value){const auto found=std::find_if(parameterRegistry().begin(),parameterRegistry().end(),[&](const auto& item){return item.id==key;});CHECK(found!=parameterRegistry().end());if(found!=parameterRegistry().end()){const auto index=std::size_t(found-parameterRegistry().begin());d.values[index]=p.control(index).convertTo0to1(value);}};
 put("synth.1.enabled",1);put("synth.1.engine",0);put("synth.1.analog.noise",.5f);put("synth.1.analog.osc.1.level",0);put("synth.1.analog.osc.2.level",0);put("synth.1.analog.filter.cutoff",20000);put("synth.1.analog.amp.sustain",1);CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,revision));p.prepareToPlay(48000,512);
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setSize(1080,680);editor->setVisible(true);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;auto* stack=graph->findParentComponentOfClass<RackStack>();CHECK(stack);if(!stack)return;const auto scroll=stack->viewport().getViewPosition();auto* more=findComponent(*graph,"eq.more");CHECK(more);if(more)more->grabKeyboardFocus();CHECK(graph->onMatch);if(!graph->onMatch)return;graph->onMatch();
 auto* match=dynamic_cast<studio::EqMatchPanel*>(findComponent(*editor,"eq.match"));CHECK(match&&match->isShowing());if(!match)return;CHECK(!stack->viewport().isVisible());const auto before=rig.sound();snapshotImage("match-empty-editor.png",*editor);
 CHECK(match->learn(false,false));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,57,.8f),0);
 for(int block=0;block<416;++block){p.processBlock(audio,midi);midi.clear();if(block%8==7){graph->analysisHub()->update();stack->refreshAnalysis();}}
 CHECK(match->stopLearning());CHECK(match->sourceReference());if(!match->sourceReference())return;CHECK(rig.sound()==before);auto reference=*match->sourceReference();reference.label="Broad reference fixture";reference.captureId="native-editor-reference";
 for(std::size_t i=0;i<129;++i)reference.spectrum.db[i]+=6*std::exp(-.5f*std::pow(float(int(i)-64)/10,2));CHECK(match->setReference(reference));CHECK(match->preview());CHECK(waitForMatch(*graph));match->updateMonitoring();
 for(const auto size:{juce::Point<int>(1080,680),juce::Point<int>(1800,1100)}){editor->setSize(size.x,size.y);juce::MessageManager::getInstance()->runDispatchLoopUntil(40);CHECK(editor->getLocalBounds().contains(editor->getLocalArea(match,match->getLocalBounds())));
  for(const auto* name:{"back","spectra","correction","source","reference","learn","load","amount","cancel","preview","apply"}){const auto* key=findComponent(*match,juce::String("eq.match.")+name);CHECK(key);if(key)CHECK(match->getLocalBounds().contains(key->getBounds()));}
  const auto* cancel=findComponent(*match,"eq.match.cancel");const auto* preview=findComponent(*match,"eq.match.preview");const auto* apply=findComponent(*match,"eq.match.apply");CHECK(cancel&&preview&&apply);if(cancel&&preview&&apply){CHECK(cancel->getWidth()==preview->getWidth()&&preview->getWidth()==apply->getWidth());CHECK(preview->getX()-cancel->getRight()==apply->getX()-preview->getRight());CHECK(apply->isEnabled());}
  snapshotImage(size.x==1080?"match-minimum-editor.png":"match-expanded-editor.png",*editor);
 }
 editor->setSize(1080,680);auto* spectra=dynamic_cast<juce::Button*>(findComponent(*match,"eq.match.spectra"));CHECK(spectra);if(spectra&&spectra->onClick)spectra->onClick();snapshotImage("match-spectra-editor.png",*editor);CHECK(rig.sound()==before);
 CHECK(match->apply());CHECK(!match->isVisible()&&stack->viewport().isVisible());CHECK(stack->viewport().getViewPosition()==scroll);CHECK(rig.sound()!=before);snapshotImage("match-applied-editor.png",*editor);CHECK(p.undoSoundEdit());CHECK(rig.sound()==before);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
 graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph&&graph->onMatch);if(graph&&graph->onMatch)graph->onMatch();match=dynamic_cast<studio::EqMatchPanel*>(findComponent(*editor,"eq.match"));CHECK(match&&match->isVisible());if(!match)return;
 CHECK(match->loadReferenceFile(juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("missing-reference.wav")));CHECK(pumpUntil([&]{match->updateMonitoring();return !match->loadingReference();}));snapshotImage("match-unavailable-editor.png",*editor);
 CHECK(match->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));CHECK(stack->viewport().isVisible());graph->onMatch();const auto hub=graph->analysisHub();CHECK(match->learn(false,false));editor.reset();CHECK(hub->activeAnalyzers()==0);p.releaseResources();CHECK(rig.sound()==before);
}
void matchPeerAuthorityAndFreshness(int manualPollIntervalMs=0){
 std::cout<<"SECTION match_real_peer_admission_freshness_and_scope\n";const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("match-peers-"+juce::Uuid().toString());EqOverviewPeers peers(root);Rig rig({},false,peers.options());CHECK(peers.attach(*rig.processor));const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 peers.manualPollIntervalMs=manualPollIntervalMs;
 studio::EqMatchPanel match(*rig.processor,*model,rig.graph->analysisHub());match.bind(*rig.graph);match.setVisible(true);const auto before=rig.sound();CHECK(match.selectPeer(peers.members[0]));CHECK(peers.wait(*rig.processor,[&]{match.updateMonitoring();return match.reference().has_value();}));
 const auto reference=match.reference();CHECK(reference&&reference->label.contains("Peer")&&reference->captureId.isNotEmpty());juce::String encoded;CHECK(match.exportReference(encoded));CHECK(peers.providerJobs.load()==0);CHECK(rig.sound()==before);
 peers.senders[0]->closeEqSpectrumView(peers.owners[0]);peers.owners[0]=0;CHECK(peers.wait(*rig.processor,[&]{match.updateMonitoring();juce::String text;return !match.exportReference(text);}));CHECK(match.statusText().containsIgnoreCase("unavailable"));CHECK(!match.apply());
 match.setVisible(false);CHECK(rig.graph->analysisHub()->activeAnalyzers()==0);CHECK(rig.sound()==before);match.retire();
}
void overviewWithFourNativePeers(){
 auto root=images==juce::File{}?juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile(juce::Uuid().toString()):images.getChildFile("private-peer-fixture");
 EqOverviewPeers peers(root);overviewInActualEditor(true,&peers);
}
void overviewFourPeerTransport(){
 std::cout<<"SECTION overview_four_real_peer_fft_summaries_headless\n";
 const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile(juce::Uuid().toString());EqOverviewPeers peers(root);Rig rig({},false,peers.options());
 CHECK(peers.attach(*rig.processor));const auto owner=rig.processor->openEqSpectrumView();CHECK(owner&&rig.processor->configureEqSpectrumView(owner,true,7,peers.members));
 CHECK(peers.wait(*rig.processor,[&]{for(const auto& sender:peers.senders)if(!rig.processor->peerEqSpectrum(owner,sender->groupLinkView().runtime))return false;return true;}));
 for(const auto& sender:peers.senders)if(const auto* data=rig.processor->peerEqSpectrum(owner,sender->groupLinkView().runtime)){CHECK(data->slot==7&&data->sampleRate==192000);CHECK(*std::max_element(data->postDb100.begin(),data->postDb100.end())>-6000);}else CHECK(false);
 peers.owners[0]=0;peers.hubs[0]->retire();peers.senders[0]->closeAiSession();CHECK(rig.processor->browseGroups());CHECK(peers.wait(*rig.processor,[&]{return rig.processor->groupWorkflowView().members.size()==4;}));
 CHECK(peers.wait(*rig.processor,[&]{for(std::size_t i=1;i<4;++i)if(!rig.processor->peerEqSpectrum(owner,peers.senders[i]->groupLinkView().runtime))return false;return true;}));
 CHECK(peers.providerJobs.load()==0);rig.processor->closeEqSpectrumView(owner);
}
void actualEditor(){
 std::cout<<"SECTION actual_editor_native_graph_numeric_pages_and_retirement\n";Rig rig;rig.graph.reset();
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;
 CHECK(graph->boundSlot()==7);graph->selectBand(7);
 auto* stack=dynamic_cast<RackStack*>(findComponent(*editor,"rack.stack"));
 // The viewport owns rack.stack; reach its RackStack ancestor instead.
 if(!stack)for(auto* c=graph->getParentComponent();c;c=c->getParentComponent())if((stack=dynamic_cast<RackStack*>(c)))break;
 CHECK(stack);if(stack){CHECK(!stack->drawerRack().isNull());CHECK(stack->controls().routesPage()==7);
  auto* band=dynamic_cast<juce::ComboBox*>(findComponent(stack->controls(),"rack.controls.band"));CHECK(band);
  if(band){const auto before=rig.sound();band->setSelectedId(3,juce::sendNotificationSync);CHECK(graph->selectedBand()==2);CHECK(stack->controls().routesPage()==2);CHECK(rig.sound()==before);
   graph->selectBand(7);CHECK(band->getSelectedId()==8);CHECK(rig.sound()==before);}}
 if(stack)showcase(rig,*editor,*graph,*stack);
 graph->updateMonitoring();const auto analysis=graph->analysisHub();const auto token=graph->subscription();CHECK(token);
 if(stack){const auto id=stack->selected();auto* unit=stack->unit(id);CHECK(unit);if(unit){
   unit->setFolded(true);if(unit->onLayout)unit->onLayout();CHECK(!graph->subscription());if(token)CHECK(analysis->generation(*token)==0);
   unit->setFolded(false);if(unit->onLayout)unit->onLayout();stack->reveal(id);graph->updateMonitoring();CHECK(graph->subscription());
   const auto shown=graph->subscription();const auto savedHeight=stack->body().getHeight();stack->body().setSize(stack->body().getWidth(),savedHeight+3*stack->viewport().getHeight());stack->viewport().setViewPosition(0,stack->body().getHeight());graph->updateMonitoring();CHECK(!graph->subscription());if(shown)CHECK(analysis->generation(*shown)==0);
   stack->body().setSize(stack->body().getWidth(),savedHeight);stack->reveal(id);graph->updateMonitoring();CHECK(graph->subscription());
  }}
 const auto last=graph->subscription();editor.reset();if(last)CHECK(analysis->generation(*last)==0);
}

void extensionEditorImages(){
 std::cout<<"SECTION actual_editor_extension_curves_and_compact_drawer\n";Rig rig;rig.graph.reset();auto& p=*rig.processor;
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;
 RackStack* stack=nullptr;for(auto* c=graph->getParentComponent();c;c=c->getParentComponent())if((stack=dynamic_cast<RackStack*>(c)))break;CHECK(stack);if(!stack)return;
 const auto extension=dynamicEqExtensionParameterStart+7*dynamicEqExtensionParametersPerSlot;
 const auto initial=rig.sound();
 for(int shape=1;shape<=4;++shape){
  auto d=initial;std::uint64_t revision;SoundDocument current;CHECK(p.trySoundSnapshot(current,revision));
  d.values[rig.base]=1;d.values[rig.base+3]=p.control(rig.base+3).convertTo0to1(1000);d.values[rig.base+4]=.75f;d.values[rig.base+7]=.25f;
  if(shape<4)d.values[extension]=float(shape)/3;
  else{d.values[rig.base+2]=.6f;d.values[extension+1]=1;}
  CHECK(p.applySoundEdit(d,SoundEditOrigin::manual,revision));
  for(bool minimum:{true,false}){
   editor->setSize(minimum?1080:1800,minimum?680:1100);juce::MessageManager::getInstance()->runDispatchLoopUntil(70);
   graph->refresh(rig.sound(),7);graph->selectBand(0);stack->closeDrawer();stack->reveal(stack->selected());
   const auto stem=juce::String("extension-")+juce::String(shape)+(minimum?"-minimum":"-expanded");
   snapshotImage((stem+"-graph.png").toRawUTF8(),*graph);
   stack->openDrawer(stack->selected());
   CHECK(stack->controls().showParameter(extension+std::size_t(shape==4)));
   juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
   auto* knob=stack->controls().knob(rig.base+(shape==4?6:2));CHECK(knob);if(knob){knob->grabKeyboardFocus();juce::MessageManager::getInstance()->runDispatchLoopUntil(30);}
   auto& viewport=stack->viewport();const auto graphBounds=stack->body().getLocalArea(graph,graph->getLocalBounds());viewport.setViewPosition(0,std::max(0,graphBounds.getY()-8));
   CHECK(stack->controls().shownKnobs().size()==12);
   for(const auto* shown:stack->controls().shownKnobs()){
    CHECK(viewport.getLocalBounds().contains(viewport.getLocalArea(shown,shown->getLocalBounds())));
    CHECK(viewport.getLocalBounds().contains(viewport.getLocalArea(&shown->entry,shown->entry.getLocalBounds())));
    CHECK(viewport.getLocalBounds().contains(viewport.getLocalArea(&shown->label,shown->label.getLocalBounds())));
   }
   snapshotImage((stem+"-editor.png").toRawUTF8(),*editor);
   if(minimum&&shape==3&&knob){
    CHECK(knob->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));auto* menu=juce::Component::getCurrentlyModalComponent();CHECK(menu);
    if(menu){snapshotImage("extension-shape-menu.png",*menu);CHECK(menu->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));}
    CHECK(pumpUntil([]{return juce::Component::getCurrentlyModalComponent()==nullptr;}));
   }
   stack->closeDrawer();
  }
 }
}

void editorClosesInHostCallback(){
 std::cout<<"SECTION editor_retirement_during_host_begin_value_and_end\n";
 for(int stage=0;stage<3;++stage){Rig rig;rig.graph.reset();auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)continue;editor->addToDesktop(0);editor->setVisible(true);
  auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)continue;graph->selectBand(0);graph->updateMonitoring();const auto analysis=graph->analysisHub();const auto token=graph->subscription();CHECK(token);
  Notifications frequency(rig.processor->control(rig.base+3)),gain(rig.processor->control(rig.base+4));
  auto close=[&]{editor.reset();};if(stage==0)frequency.onStart=close;if(stage==1)frequency.onValue=close;if(stage==2)frequency.onEnd=close;
  juce::Component::SafePointer<DynamicEqGraph> safe(graph);const auto start=graph->nodePosition(0);
  graph->mouseDown(mouse(*graph,start,start));if(editor&&safe)safe->mouseDrag(mouse(*safe,start+juce::Point<float>(40,-12),start));if(editor&&safe)safe->mouseUp(mouse(*safe,start,start,false));
  CHECK(!editor);CHECK(frequency.starts==1&&frequency.ends==1);if(stage>0)CHECK(gain.starts==1&&gain.ends==1);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(40);CHECK(!safe);if(token)CHECK(analysis->generation(*token)==0);
  CHECK(rig.processor->beginParameterGesture(rig.base+3));CHECK(rig.processor->endParameterGesture(rig.base+3));
 }
}

void rackRebindReplaceAndRemove(){
 std::cout<<"SECTION editor_graph_rack_move_host_rebind_replace_and_remove\n";Rig rig;rig.graph.reset();auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;graph->selectBand(0);graph->updateMonitoring();const auto analysis=graph->analysisHub();auto token=graph->subscription();CHECK(token);
 const auto first=rig.sound();CHECK(first.personalities&&first.personalities->instances.size()==1);if(!first.personalities)return;const auto id=first.personalities->instances.front().id;
 RackOperation move;move.kind=RackOperationKind::move;move.selected=id;move.destination={RackRole::fx,0,0};SoundDocument current;std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(current,revision));CHECK(rig.processor->applyRackOperation(move,revision)==RackCandidateStatus::ready);
 CHECK(pumpUntil([&]{return !token||analysis->generation(*token)==0;}));graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;CHECK(graph->boundSlot()==7);if(token)CHECK(analysis->generation(*token)==0);graph->updateMonitoring();token=graph->subscription();CHECK(token);
 auto recalled=[&](const RackDesign& design,std::uint8_t slot){
  const auto before=rig.sound();RackDifference difference;RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=id;SoundDocument removed,placed;
  CHECK(makeRackCandidate(before,remove,SoundEditOrigin::manual,removed,difference)==RackCandidateStatus::ready);
  RackOperation place;place.newInstance=id;place.design=design;place.destination={RackRole::fx,masterChain,0};place.allocation={{0,slot}};place.allowSlotReuse=true;
  CHECK(makeRackCandidate(removed,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
  juce::MemoryBlock wire;CHECK(encodeSoundState(placed,wire));CHECK(rig.processor->restoreSoundState(wire.getData(),int(wire.getSize())));
  const bool eq=design.units.size()==1&&design.units.front().module==NativeModule::dynamicEq;
  CHECK(pumpUntil([&]{auto* shown=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));return eq?shown&&shown->boundSlot()==slot:shown==nullptr;}));
 };
 const auto contour=rig.sound().personalities->instances.front().design;recalled(contour,6);graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;CHECK(graph->boundSlot()==6);if(token)CHECK(analysis->generation(*token)==0);graph->selectBand(0);graph->updateMonitoring();token=graph->subscription();CHECK(token);
 const auto designs=airack::test::frozenFactoryRackDesigns();const auto stereo=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.units.size()==1&&d.units.front().module==NativeModule::stereo;});CHECK(stereo!=designs.end());
 juce::Component::SafePointer<DynamicEqGraph> safe(graph);if(stereo!=designs.end())recalled(*stereo,6);CHECK(pumpUntil([&]{return safe==nullptr;}));CHECK(findComponent(*editor,"dynamic.eq.graph")==nullptr);if(token)CHECK(analysis->generation(*token)==0);
 RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=id;CHECK(rig.processor->trySoundSnapshot(current,revision));CHECK(rig.processor->applyRackOperation(remove,revision)==RackCandidateStatus::ready);juce::MessageManager::getInstance()->runDispatchLoopUntil(80);{const auto removed=rig.sound();CHECK(!removed.personalities||removed.personalities->instances.empty());}
}

void feedGrabSpectrum(Rig& rig,int first=100,int second=300)
{
 auto& graph=*rig.graph;graph.setSpectrumOn(true);graph.updateMonitoring();const auto token=graph.subscription();CHECK(token);if(!token)return;
 FxMonitor::ActiveSlots slots{};slots[7]=true;auto monitor=rig.processor->fxMonitorHandle();CHECK(monitor->beginBlock(48000,slots));
 for(int n=0;n<studio::FxSpectrum::fftSize;++n){const auto x=first==0?0.:.25*std::sin(2*juce::MathConstants<double>::pi*first*n/studio::FxSpectrum::fftSize)+.125*std::sin(2*juce::MathConstants<double>::pi*second*n/studio::FxSpectrum::fftSize);monitor->capture(7,{x,-x},{x*.5,-x*.5});}
 graph.updateMonitoring();CHECK(graph.spectrum().valid);
}
void spectrumGrabInteraction()
{
 std::cout<<"SECTION spectrum_grab_freeze_preview_creation_host_gestures_and_undo\n";Rig rig;auto& graph=*rig.graph;auto& processor=*rig.processor;
 const auto before=rig.sound();graph.setGrabEnabled(true);CHECK(graph.grabEnabled());CHECK(rig.sound()==before);graph.setGrabEnabled(false);CHECK(!graph.grabEnabled());
 feedGrabSpectrum(rig);const auto token=graph.subscription();graph.setGrabEnabled(true);CHECK(graph.grabEnabled());CHECK(graph.grabPeakCount()>=2);CHECK(graph.grabPeakCount()<=8);CHECK(rig.sound()==before);
 if(!graph.grabPeakCount())return;
 snapshotImage("grab-frozen-peaks.png",graph);
 const auto start=graph.grabPeakPosition(0),finish=start+juce::Point<float>(0,30);
 feedGrabSpectrum(rig,400,700);CHECK(graph.grabPeakPosition(0)==start);CHECK(graph.subscription()==token);
 Notifications active(processor.control(rig.base+11)),frequency(processor.control(rig.base+14)),gain(processor.control(rig.base+15)),q(processor.control(rig.base+16));
 graph.mouseDown(mouse(graph,start,start));graph.mouseDrag(mouse(graph,finish,start));CHECK(rig.sound()==before);CHECK(active.starts==0&&frequency.starts==0);
 snapshotImage("grab-preview.png",graph);graph.mouseUp(mouse(graph,finish,start,false));
 const auto after=rig.sound();CHECK(after.values[rig.base+11]==1);CHECK(after.values[rig.base+13]==0);CHECK(after.values[rig.base+15]<.5f);CHECK(after.values[rig.base+18]==.5f);CHECK(!graph.grabEnabled());
 CHECK(std::abs(processor.control(rig.base+14).convertFrom0to1(after.values[rig.base+14])-48000.*100/16384)<1);
 CHECK(active.starts==1&&active.ends==1&&frequency.starts==1&&frequency.ends==1&&gain.starts==1&&gain.ends==1&&q.starts==1&&q.ends==1);
 CHECK(processor.undoSoundEdit());CHECK(rig.sound()==before);CHECK(processor.redoSoundEdit());CHECK(rig.sound()==after);
}
void spectrumGrabCancellation()
{
 std::cout<<"SECTION spectrum_grab_escape_focus_reset_stale_generation_and_rebind\n";
 for(int reason=0;reason<8;++reason){Rig rig;auto& graph=*rig.graph;feedGrabSpectrum(rig);graph.setGrabEnabled(true);CHECK(graph.grabPeakCount()>0);if(!graph.grabPeakCount())continue;
  const auto before=rig.sound();const auto start=graph.grabPeakPosition(0);graph.mouseDown(mouse(graph,start,start));graph.mouseDrag(mouse(graph,start+juce::Point<float>(0,20),start));
  if(reason==0)CHECK(graph.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
  if(reason==1)graph.focusLost(juce::Component::focusChangedDirectly);
  if(reason==2)graph.suspend();
  if(reason==3){juce::MemoryBlock wire;CHECK(encodeSoundState(before,wire));CHECK(rig.processor->restoreSoundState(wire.getData(),int(wire.getSize())));}
  if(reason==4)rig.processor->fxMonitorHandle()->invalidate();
  if(reason==5)graph.refresh(before,0);
  if(reason==6)graph.setSpectrumOn(false);
  if(reason==7){rig.processor->control(0).setValueNotifyingHost(before.values[0]==.2f?.3f:.2f);graph.updateMonitoring();}
  graph.mouseUp(mouse(graph,start+juce::Point<float>(0,20),start,false));const auto after=rig.sound();
  for(std::size_t n=0;n<89;++n)CHECK(after.values[rig.base+n]==before.values[rig.base+n]);CHECK(!graph.grabEnabled());
 }
 Rig rig;feedGrabSpectrum(rig,0,0);rig.graph->setGrabEnabled(true);CHECK(rig.graph->grabPeakCount()==0);snapshotImage("grab-silence.png",*rig.graph);
}
void spectrumGrabLocksAndKeyboard()
{
 std::cout<<"SECTION spectrum_grab_locked_full_macro_destinations_and_keyboard\n";
 for(int reason=0;reason<3;++reason){Rig rig;auto& processor=*rig.processor;SoundDocument d;std::uint64_t revision;CHECK(processor.trySoundSnapshot(d,revision));
  if(reason==0)d.controlLocks[rig.base+16]=true;
  if(reason==1)for(std::size_t n=0;n<8;++n)d.values[rig.base+n*11]=1;
  if(reason==2)d.macros[0]={parameterRegistry()[rig.base+15].stateKey,.5f};
  CHECK(processor.applySoundEdit(d,SoundEditOrigin::manual,revision));rig.refresh();const auto before=rig.sound();feedGrabSpectrum(rig);rig.graph->setGrabEnabled(true);CHECK(rig.graph->grabPeakCount()==0);CHECK(rig.sound()==before);
  if(reason==1)snapshotImage("grab-full-bank.png",*rig.graph);
 }
 Rig rig;feedGrabSpectrum(rig);const auto before=rig.sound();rig.graph->setGrabEnabled(true);CHECK(rig.graph->grabPeakCount()>0);if(!rig.graph->grabPeakCount())return;
 CHECK(rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));CHECK(rig.sound()==before);CHECK(rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));CHECK(rig.sound().values[rig.base+11]==1);CHECK(rig.sound().values[rig.base+15]>.5f);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);
}
void spectrumGrabRetainedAndReentrant()
{
 std::cout<<"SECTION spectrum_grab_neutral_defaults_reentrant_callbacks_and_capture_overflow\n";
 for(int action=0;action<6;++action){Rig rig;auto& processor=*rig.processor;SoundDocument d;std::uint64_t revision;CHECK(processor.trySoundSnapshot(d,revision));
  d.values[rig.base+12]=1;d.values[rig.base+13]=1;d.values[rig.base+18]=processor.control(rig.base+18).convertTo0to1(12);
  CHECK(processor.applySoundEdit(d,SoundEditOrigin::manual,revision));rig.refresh();feedGrabSpectrum(rig);auto* graph=rig.graph.get();graph->setGrabEnabled(true);CHECK(graph->grabPeakCount()>0);if(!graph->grabPeakCount())continue;
  const auto before=rig.sound();const auto start=graph->grabPeakPosition(0),finish=start+juce::Point<float>(0,25);
  Notifications active(processor.control(rig.base+11)),frequency(processor.control(rig.base+14)),gain(processor.control(rig.base+15));
  juce::MemoryBlock wire;CHECK(encodeSoundState(before,wire));
  if(action==1)frequency.onStart=[&]{processor.control(0).setValueNotifyingHost(before.values[0]==.2f?.3f:.2f);};
  if(action==2)frequency.onStart=[&]{CHECK(processor.restoreSoundState(wire.getData(),int(wire.getSize())));};
  if(action==3)frequency.onStart=[&]{rig.graph.reset();};
  graph->mouseDown(mouse(*graph,start,start));graph->mouseDrag(mouse(*graph,finish,start));CHECK(rig.sound()==before);
  if(action==4){auto monitor=processor.fxMonitorHandle();for(std::size_t n=0;n<(FxMonitor::packetCapacity+2)*FxMonitor::packetFrames;++n)monitor->capture(7,{.01,-.01},{.01,-.01});CHECK(monitor->stats(7).dropped>0);}
  if(action==5){auto proposal=std::make_shared<SoundDocument>(before);proposal->values[rig.base+4]=.75f;graph->showProposal(proposal);CHECK(!graph->grabEnabled());}
  graph->mouseUp(mouse(*graph,finish,start,false));const auto after=rig.sound();
  CHECK(active.starts==active.ends&&frequency.starts==frequency.ends&&gain.starts==gain.ends);
  if(action==0||action==3){CHECK(after.values[rig.base+11]==1);CHECK(after.values[rig.base+12]==0);CHECK(after.values[rig.base+13]==0);CHECK(after.values[rig.base+18]==.5f);CHECK(after.values[rig.base+15]<.5f);}
  else for(std::size_t n=0;n<89;++n)CHECK(after.values[rig.base+n]==before.values[rig.base+n]);
  if(action==1)CHECK(after.values[0]!=before.values[0]);if(action==3)CHECK(!rig.graph);
 }
 Rig rig;feedGrabSpectrum(rig);const auto before=rig.sound();rig.graph->setGrabEnabled(true);CHECK(rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));CHECK(rig.sound()==before);
 juce::MouseWheelDetails wheel;wheel.deltaY=.2f;const auto at=rig.graph->grabPeakPosition(0);rig.graph->mouseWheelMove(mouse(*rig.graph,at,at,false),wheel);CHECK(rig.sound()==before);
 rig.graph->setVisible(false);CHECK(!rig.graph->grabEnabled());CHECK(!rig.graph->subscription());
}
void spectrumGrabAudioInvariance(bool sketch=false)
{
 std::cout<<(sketch?"SECTION real_native_render_sketch_worker_preview_cancel_exact_audio_both_qualities\n":"SECTION real_native_render_spectrum_grab_preview_cancel_exact_audio_both_qualities\n");
 for(bool higher:{false,true}){Rig reference,observed;auto source=reference.sound();const auto designs=airack::test::frozenFactoryRackDesigns();
  const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& design){return design.name=="Copper Arc";});CHECK(found!=designs.end());if(found==designs.end())continue;
  RackOperation operation;operation.newInstance=juce::Uuid();operation.design=*found;operation.destination={RackRole::synth,masterChain,0};operation.allocation={{0,0}};operation.allowSlotReuse=true;
  SoundDocument placed;RackDifference difference;CHECK(makeRackCandidate(source,operation,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);placed.values[758]=higher?1.f:0.f;
  juce::MemoryBlock state;CHECK(encodeSoundState(placed,state));
  for(auto* rig:{&reference,&observed}){CHECK(rig->processor->restoreSoundState(state.getData(),int(state.getSize())));rig->refresh();rig->processor->setNonRealtime(true);rig->processor->prepareToPlay(48000,64);}
  reference.graph->setSpectrumOn(false);observed.graph->setSpectrumOn(true);observed.graph->updateMonitoring();
  const auto before=observed.sound();juce::AudioBuffer<float> baseline(2,64),captured(2,64);juce::MidiBuffer midi;bool exact=true;float peak=0;
  for(int block=0;block<850;++block){midi.clear();if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(block==680)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
   reference.processor->processBlock(baseline,midi);observed.processor->processBlock(captured,midi);
   for(int channel=0;channel<2;++channel)for(int frame=0;frame<64;++frame){const auto a=baseline.getSample(channel,frame),b=captured.getSample(channel,frame);exact=exact&&std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);peak=std::max(peak,std::abs(a));}
   if(block%16==15)observed.graph->updateMonitoring();
   if(block==400){if(sketch){observed.graph->setSketchEnabled(true);CHECK(observed.graph->sketchEnabled());CHECK(observed.graph->previewSketch(sketchTargetCurve()));}else{observed.graph->setGrabEnabled(true);CHECK(observed.graph->grabEnabled());CHECK(observed.graph->grabPeakCount()>0);CHECK(observed.graph->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));}CHECK(observed.sound()==before);}
   if(block==600){CHECK(observed.graph->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));CHECK(!observed.graph->grabEnabled());CHECK(!observed.graph->sketchEnabled());CHECK(observed.sound()==before);}
  }
  CHECK(exact);CHECK(peak>.001f);CHECK(observed.sound()==before);reference.processor->releaseResources();observed.processor->releaseResources();
 }
}
void hiddenGraphPollingIsIdle()
{
 std::cout<<"SECTION hidden_graph_polling_has_no_capture_fft_or_heap_work\n";
 Rig rig;rig.graph->setSketchEnabled(true);CHECK(rig.graph->previewSketch(sketchTargetCurve()));rig.graph->setVisible(false);rig.graph->updateMonitoring();CHECK(!rig.graph->sketchEnabled());
 const auto before=rig.processor->fxMonitorHandle()->stats(7);const auto windows=rig.graph->spectrum().windows;
#if defined(_MSC_VER) && defined(_DEBUG)
 hiddenAllocations=hiddenFrees=0;const auto previous=_CrtSetAllocHook(hiddenPollHeapHook);auditHiddenPoll=true;
#endif
 for(int n=0;n<16;++n)rig.graph->updateMonitoring();
#if defined(_MSC_VER) && defined(_DEBUG)
 auditHiddenPoll=false;_CrtSetAllocHook(previous);
 std::cout<<"HIDDEN_POLL allocations="<<hiddenAllocations<<" frees="<<hiddenFrees<<'\n';
 CHECK(hiddenAllocations==0);CHECK(hiddenFrees==0);
#endif
 CHECK(!rig.graph->subscription());CHECK(!rig.graph->grabEnabled());CHECK(rig.graph->spectrum().windows==windows);
 const auto after=rig.processor->fxMonitorHandle()->stats(7);CHECK(after.published==before.published&&after.dropped==before.dropped);
}
void realEditorHiddenPollingIsIdle()
{
 std::cout<<"SECTION actual_editor_host_frame_hidden_polling_has_no_heap_work\n";Rig rig;rig.graph.reset();
 auto editor=std::unique_ptr<juce::AudioProcessorEditor>(rig.processor->createEditor());CHECK(editor);if(!editor)return;editor->addToDesktop(0);editor->setVisible(true);
 auto* graph=dynamic_cast<DynamicEqGraph*>(findComponent(*editor,"dynamic.eq.graph"));CHECK(graph);if(!graph)return;graph->setSketchEnabled(true);CHECK(graph->previewSketch(sketchTargetCurve()));graph->setVisible(false);graph->updateMonitoring();CHECK(!graph->sketchEnabled());
#if defined(_MSC_VER) && defined(_DEBUG)
 hiddenAllocations=hiddenFrees=0;const auto previous=_CrtSetAllocHook(hiddenPollHeapHook);auditHiddenPoll=true;
#endif
 for(int n=0;n<16;++n)graph->updateMonitoring();
#if defined(_MSC_VER) && defined(_DEBUG)
 auditHiddenPoll=false;_CrtSetAllocHook(previous);std::cout<<"EDITOR_HIDDEN_POLL allocations="<<hiddenAllocations<<" frees="<<hiddenFrees<<'\n';CHECK(hiddenAllocations==0);CHECK(hiddenFrees==0);
#endif
 CHECK(!graph->subscription());CHECK(graph->selectedBands().none());
}

void placementResponseAndManualRows()
{
 std::cout<<"SECTION placement_graph_energy_component_response_and_native_selector\n";Rig rig;SoundDocument d;std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));const auto placement=dynamicEqPlacementParameterStart+7*8;
 d.values[placement]=1;d.values[rig.base+4]=rig.processor->control(rig.base+4).convertTo0to1(12);CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));rig.refresh();RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));
 const auto& settings=patch.effects[7].dynamicEq;const auto energy=dynamicEqEnergyResponse(settings,48000,1000);const auto displayed=rig.graph->responseAt(1000);CHECK(energy&&displayed);if(energy&&displayed)CHECK(std::abs(std::abs(*displayed)-*energy)<1.e-10);
 const auto matrix=dynamicEqMatrixResponse(settings,48000,1000);CHECK(matrix);if(matrix)for(int mode=1;mode<5;++mode){const auto component=DynamicEqPlacement(mode);const auto actual=rig.graph->componentResponseAt(1000,component);CHECK(actual);
  const auto expected=mode==1?(*matrix)[0]:mode==2?(*matrix)[3]:mode==3?((*matrix)[0]+(*matrix)[1]+(*matrix)[2]+(*matrix)[3])*.5:((*matrix)[0]-(*matrix)[1]-(*matrix)[2]+(*matrix)[3])*.5;if(actual)CHECK(std::abs(*actual-expected)<1.e-10);}
 const auto* model=studio::studioSkin();CHECK(model);if(model){studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::dynamicEq,7}});CHECK(panel.showParameter(placement));auto* knob=panel.knob(placement);CHECK(knob);if(knob){const auto before=rig.sound();knob->move(.75f);CHECK(rig.sound().values[placement]==.75f);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}}
}
void placementControlsUseTheAvailableWidth()
{
 std::cout<<"SECTION eq_band_compact_header_and_uniform_eight_column_body\n";Rig rig;
 const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::dynamicEq,7}});
 for(std::size_t band=0;band<8;++band){CHECK(panel.showParameter(dynamicEqPlacementParameterStart+7*8+band));
  CHECK(panel.shownHeight()<=300);const auto knobs=panel.shownKnobs();CHECK(knobs.size()==12);if(knobs.size()!=12)continue;
  const auto base=rig.base+band*11;float previous=0,pitch=0,baseline=0;
  for(int field=3;field<=10;++field){auto* control=panel.knob(base+std::size_t(field));CHECK(control);if(!control)continue;
   const auto bounds=panel.getLocalArea(control,control->getLocalBounds());const float x=float(bounds.getCentreX()),y=float(bounds.getCentreY());
   if(field==3)baseline=y;else{CHECK(std::abs(y-baseline)<=1);if(field==4)pitch=x-previous;else CHECK(std::abs(x-previous-pitch)<=1);}
   previous=x;CHECK(control->label.getBottom()<bounds.getY());
  }
  for(const auto index:{base,base+1,base+2,dynamicEqPlacementParameterStart+7*8+band}){
   auto* control=panel.knob(index);CHECK(control);if(control)CHECK(panel.getLocalArea(control,control->getLocalBounds()).getBottom()<=70);
  }
  for(const auto* control:knobs){CHECK(panel.getLocalBounds().contains(panel.getLocalArea(control,control->getLocalBounds())));
   CHECK(panel.getLocalBounds().contains(panel.getLocalArea(&control->label,control->label.getLocalBounds())));
   CHECK(panel.getLocalBounds().contains(panel.getLocalArea(&control->entry,control->entry.getLocalBounds())));}
  for(std::size_t a=0;a<knobs.size();++a)for(std::size_t b=a+1;b<knobs.size();++b)
   {CHECK(!panel.getLocalArea(knobs[a],knobs[a]->getLocalBounds()).intersects(panel.getLocalArea(knobs[b],knobs[b]->getLocalBounds())));
    CHECK(!knobs[a]->labelInk().expanded(2,0).intersects(knobs[b]->labelInk().expanded(2,0)));}
 }
}
void compactBandSelectorsEditAndNavigate()
{
 std::cout<<"SECTION compact_eq_clicks_keep_gestures_undo_and_view_navigation\n";Rig rig;
 const auto* model=studio::studioSkin();CHECK(model);if(!model)return;
 studio::AllControlsPanel panel(*rig.processor,*model,{});panel.show({{NativeModule::dynamicEq,7}});CHECK(panel.showParameter(rig.base));
 auto* active=panel.knob(rig.base);CHECK(active);if(active){const auto before=rig.sound();Notifications host(rig.processor->control(rig.base));
  const auto at=active->getLocalBounds().toFloat().getCentre();active->mouseDown(mouse(*active,at,at));active->mouseUp(mouse(*active,at,at,false));
  CHECK(rig.sound().values[rig.base]!=before.values[rig.base]);CHECK(host.starts==1&&host.ends==1&&host.values==1);
  if(rig.sound()!=before){CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  CHECK(active->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));CHECK(rig.sound().values[rig.base]!=before.values[rig.base]);
  if(rig.sound()!=before){CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}}
 const auto placement=dynamicEqPlacementParameterStart+7*8;auto* component=panel.knob(placement);CHECK(component);
 if(component){const auto before=rig.sound();const auto at=juce::Point<float>(float(component->getWidth())*.3f,float(component->getHeight())*.5f);
  component->mouseDown(mouse(*component,at,at));component->mouseUp(mouse(*component,at,at,false));CHECK(rig.sound().values[placement]==.25f);
  if(rig.sound()!=before){CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}}
 auto* band=dynamic_cast<juce::ComboBox*>(findComponent(panel,"rack.controls.band"));CHECK(band);
 int selections=0;std::size_t selectedSlot=99,selectedBand=99;panel.onBandSelected=[&](std::size_t slot,std::size_t chosen){++selections;selectedSlot=slot;selectedBand=chosen;};
 if(band){const auto before=rig.sound();band->setSelectedId(5,juce::sendNotificationSync);CHECK(panel.routesPage()==4);CHECK(rig.sound()==before);
  band->setSelectedId(9,juce::sendNotificationSync);CHECK(panel.knob(rig.base+88)->getParentComponent()->isVisible());
  band->setSelectedId(10,juce::sendNotificationSync);CHECK(panel.knob(dynamicEqGainParameterStart+7*2)->getParentComponent()->isVisible());
  CHECK(selections==1&&selectedSlot==7&&selectedBand==4);
  auto* next=dynamic_cast<juce::Button*>(findComponent(panel,"rack.controls.next"));auto* previous=dynamic_cast<juce::Button*>(findComponent(panel,"rack.controls.previous"));CHECK(next&&previous);
  if(next&&previous){next->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(30);CHECK(panel.routesPage()==10);CHECK(selections==2&&selectedBand==0);
   previous->triggerClick();juce::MessageManager::getInstance()->runDispatchLoopUntil(30);CHECK(panel.routesPage()==9);CHECK(rig.sound()==before);}}

}


void placementCreationDefaultsAndLocks()
{
 std::cout<<"SECTION placement_new_band_defaults_and_locked_destination_refusal\n";
 for(bool grab:{false,true})for(bool locked:{false,true}){Rig rig;SoundDocument d;std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));const auto band=rig.base+11,placement=dynamicEqPlacementParameterStart+7*8+1;
  d.values[placement]=1;d.values[band+2]=rig.processor->control(band+2).convertTo0to1(2);d.values[band+7]=rig.processor->control(band+7).convertTo0to1(9);d.controlLocks[placement]=locked;
  if(locked)for(std::size_t n=2;n<8;++n)d.values[rig.base+n*11]=1;
  CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));rig.refresh();const auto before=rig.sound();
  if(grab){feedGrabSpectrum(rig);rig.graph->setGrabEnabled(true);if(rig.graph->grabEnabled()&&rig.graph->grabPeakCount())rig.graph->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));}
  else{const auto plot=rig.graph->plotBounds();const auto at=plot.getCentre()+juce::Point<float>(30,-12);rig.graph->mouseDoubleClick(mouse(*rig.graph,at,at));}
  const auto after=rig.sound();if(locked)CHECK(after==before);else{CHECK(after.values[band]==1);CHECK(after.values[placement]==0);CHECK(after.values[band+2]==rig.processor->control(band+2).getDefaultValue());CHECK(after.values[band+7]==rig.processor->control(band+7).getDefaultValue());CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}
 }
}


void extensionGraphControlsAndCreationDefaults()
{
 std::cout<<"SECTION extension_graph_gain_Q_and_fresh_bell_defaults\n";
 for(int shape=1;shape<=3;++shape){Rig rig;rig.graph->setVisible(false);auto d=rig.sound();std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));
  const auto extra=dynamicEqExtensionParameterStart+7*16;d.values[extra]=float(shape)/3;d.values[rig.base+4]=rig.processor->control(rig.base+4).convertTo0to1(12);d.values[rig.base+5]=rig.processor->control(rig.base+5).convertTo0to1(1);
  CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));rig.refresh();const auto node=rig.graph->nodePosition(0);const auto plot=rig.graph->plotBounds();
  if(shape>1){CHECK(std::abs(node.y-plot.getCentreY())>5);
   if(std::abs(node.y-plot.getCentreY())>5){rig.graph->setVisible(true);const auto before=rig.sound();const auto finish=node+juce::Point<float>(0,-12);rig.graph->mouseDown(mouse(*rig.graph,node,node));rig.graph->mouseDrag(mouse(*rig.graph,finish,node));rig.graph->mouseUp(mouse(*rig.graph,finish,node,false));
    const auto after=rig.sound();CHECK(after.values[rig.base+4]>before.values[rig.base+4]);CHECK(after.values[extra]==before.values[extra]);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);rig.refresh();rig.graph->setVisible(false);}}
  const auto before=rig.sound();juce::MouseWheelDetails wheel;wheel.deltaY=.25f;rig.graph->mouseWheelMove(mouse(*rig.graph,rig.graph->nodePosition(0),node,false),wheel);const auto after=rig.sound();
  if(shape<3){CHECK(after.values[rig.base+5]!=before.values[rig.base+5]);if(after!=before){CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}}else CHECK(after==before);
 }
 for(int creation=0;creation<3;++creation)for(bool locked:{false,true}){Rig rig;auto d=rig.sound();std::uint64_t revision;CHECK(rig.processor->trySoundSnapshot(d,revision));const auto extra=dynamicEqExtensionParameterStart+7*16+2,band=rig.base+11;
  d.values[extra]=1;d.values[extra+1]=1;d.controlLocks[extra]=locked;if(locked)for(std::size_t n=2;n<8;++n)d.values[rig.base+n*11]=1;
  CHECK(rig.processor->applySoundEdit(d,SoundEditOrigin::manual,revision));rig.refresh();const auto before=rig.sound();const auto plot=rig.graph->plotBounds();
  if(creation==0){const auto point=plot.getCentre()+juce::Point<float>(30,-12);rig.graph->mouseDoubleClick(mouse(*rig.graph,point,point));}
  if(creation==1){feedGrabSpectrum(rig);rig.graph->setGrabEnabled(true);if(!locked){CHECK(rig.graph->grabPeakCount()>0);if(rig.graph->grabPeakCount()){
   const auto at=rig.graph->grabPeakPosition(0),to=at+juce::Point<float>(0,30);rig.graph->mouseDown(mouse(*rig.graph,at,at));rig.graph->mouseDrag(mouse(*rig.graph,to,at));rig.graph->mouseUp(mouse(*rig.graph,to,at,false));
  }}else CHECK(rig.graph->grabPeakCount()==0);}
  if(creation==2){RoutedPatch patch;CHECK(makeRoutedPatch(before,patch));auto settings=patch.effects[7].dynamicEq;settings.bands[1]={};settings.bands[1].active=true;settings.bands[1].frequencyHz=1500;settings.bands[1].gainDb=-6;
   EqCurve target;for(std::size_t i=0;i<target.db.size();++i){const auto response=dynamicEqResponse(settings,48000,20.*std::pow(1000.,double(i)/128));CHECK(response);if(response)target.db[i]=float(20.*std::log10(std::max(std::abs(*response),1e-12)));}
   rig.graph->setSketchEnabled(true);if(locked){CHECK(!rig.graph->previewSketch(target));}else{CHECK(rig.graph->previewSketch(target));CHECK(waitForSketch(*rig.graph));CHECK(rig.graph->applySketch());}
  }
  const auto after=rig.sound();
  if(locked)CHECK(after==before);else{CHECK(after.values[band]==1&&after.values[extra]==0&&after.values[extra+1]==0);CHECK(rig.processor->undoSoundEdit());CHECK(rig.sound()==before);}
 }
}

}
int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);juce::ScopedJuceInitialiser_GUI juce;if(argc==3&&juce::String(argv[1])=="--spectral-images"){images=juce::File(argv[2]);spectralActualEditor();std::cout<<"Native spectral editor checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--match-rate"){matchRejectsFrozenSourceAfterHostRateChange();std::cout<<"Match rate change: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--match-input"){matchInputAccountsForTheExistingCurve();std::cout<<"Match input compensation: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--match-images"){images=juce::File(argv[2]);matchInActualEditor();std::cout<<"Native Match editor: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--match-peers-stalled-ui"){matchPeerAuthorityAndFreshness(250);std::cout<<"Stalled UI peers: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--match-peers"){matchPeerAuthorityAndFreshness();std::cout<<"Match peers: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--match"){matchCancelDoesNotWriteSound();matchPanelRequiresExplicitContinuousCapture();matchApplyPreservesOccupiedBandsAndMacros();matchRejectsStaleReviewFullAndFlat();matchCaptureLimitsAndFileLifetime();matchInputAccountsForTheExistingCurve();matchRejectsFrozenSourceAfterHostRateChange();std::cout<<"EQ Match: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--overview-sharing"){overviewOwnsEightSharedLeasesOnlyWhileVisible();std::cout<<"Overview sharing and zoom: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--overview-retirement"){retiredOverviewDoesNotBorrowProcessor();std::cout<<"Overview retirement: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--overview-peer-wire"){overviewFourPeerTransport();std::cout<<"Four peer transport: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--overview-peers-images"){images=juce::File(argv[2]);overviewWithFourNativePeers();std::cout<<"Native four-peer overview: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--overview-offscreen"){images=juce::File(argv[2]);overviewInActualEditor(false);std::cout<<"Offscreen layout only: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--overview-pure"){overlapRequiresTwoAudibleSpectra();overviewSamplingPreservesNarrowPeaksAndNyquist();std::cout<<"Overview helpers: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--overview-images"){images=juce::File(argv[2]);overviewInActualEditor();std::cout<<"Overview images: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--overview"){overlapRequiresTwoAudibleSpectra();overviewSamplingPreservesNarrowPeaksAndNyquist();overviewOwnsEightSharedLeasesOnlyWhileVisible();overviewInActualEditor();overviewWithFourNativePeers();std::cout<<"EQ overview: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--analysis-hub"){sharedGraphsDoNotConsumeOrRetainAudition();std::cout<<"EQ shared analysis: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--lifetime"){proposalAndLifetime();std::cout<<"EQ graph lifetime: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--detector-layout"){detectorDrawerPagesAreComplete();std::cout<<"EQ detector layout: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--detectors"){detectorDrawerPagesAreComplete();detectorPageMetersAndListenAreTransient();detectorDrawerEditsUseTheSharedTransaction();detectorActualEditor();std::cout<<"EQ detector drawer: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--detector-images"){images=juce::File(argv[2]);detectorActualEditor();std::cout<<"EQ detector images: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--extension-images"){images=juce::File(argv[2]);extensionEditorImages();std::cout<<"EQ extension images: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--extensions"){extensionGraphControlsAndCreationDefaults();std::cout<<"EQ extension graph: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--placement"){placementResponseAndManualRows();placementControlsUseTheAvailableWidth();compactBandSelectorsEditAndNavigate();placementCreationDefaultsAndLocks();std::cout<<"EQ placement graph: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--sketch"){sketchGraphChecks();std::cout<<"EQ Sketch graph: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--toolbar"){labelInkClearsTheDisplayRim();focusedToolbarAndBandMenu();std::cout<<"Dynamic EQ toolbar: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--editor-images"){images=juce::File(argv[2]);actualEditor();std::cout<<"EQ editor images: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}if(argc==3&&juce::String(argv[1])=="--images")images=juce::File(argv[2]);else if(argc!=1)return 2;spectralActualEditor();matchCancelDoesNotWriteSound();matchPanelRequiresExplicitContinuousCapture();matchApplyPreservesOccupiedBandsAndMacros();matchRejectsStaleReviewFullAndFlat();matchCaptureLimitsAndFileLifetime();matchInputAccountsForTheExistingCurve();matchRejectsFrozenSourceAfterHostRateChange();matchInActualEditor();matchPeerAuthorityAndFreshness();matchPeerAuthorityAndFreshness(250);overlapRequiresTwoAudibleSpectra();overviewSamplingPreservesNarrowPeaksAndNyquist();overviewOwnsEightSharedLeasesOnlyWhileVisible();overviewInActualEditor();overviewWithFourNativePeers();retiredOverviewDoesNotBorrowProcessor();sharedGraphsDoNotConsumeOrRetainAudition();detectorDrawerPagesAreComplete();detectorPageMetersAndListenAreTransient();detectorDrawerEditsUseTheSharedTransaction();detectorActualEditor();extensionGraphControlsAndCreationDefaults();placementResponseAndManualRows();placementControlsUseTheAvailableWidth();compactBandSelectorsEditAndNavigate();placementCreationDefaultsAndLocks();sketchGraphChecks();comparisonRecallIsOneUndo();comparisonLocksResetRebindAndVisibility();comparisonReentrantAutomationAndRetirement();comparisonToolbarAndGainBindings();gainControlCallbacksCanRetireGraph();labelInkClearsTheDisplayRim();focusedToolbarAndBandMenu();selectionCopiesWithoutChangingAudioState();groupPointerWheelAndMarquee();groupAtomicLocksResetAndRetirement();focusEndCanRetireGraph();crossRackPasteAndClipboardLifetime();popupClipboardCommandsAndStaleMenu();unrefreshedRecallAndPopupDoubleClick();responseAndGestures();proposalAndLifetime();spectrumOffRate();discreteHostGestures();retiredRowEndsGesture();numericRelevance();closingEndsSelection();actualEditor();editorClosesInHostCallback();rackRebindReplaceAndRemove();spectrumGrabInteraction();spectrumGrabCancellation();spectrumGrabLocksAndKeyboard();spectrumGrabRetainedAndReentrant();spectrumGrabAudioInvariance();spectrumGrabAudioInvariance(true);hiddenGraphPollingIsIdle();realEditorHiddenPollingIsIdle();std::cout<<"Dynamic EQ graph: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
