#include "RackReview.h"
#include "InstrumentProcessor.h"
#include "RackExamples.h"
#include <algorithm>
#include <charconv>
namespace airack {
namespace {
const RackInstance* instance(const SoundDocument& d,juce::Uuid id){if(d.personalities)for(const auto& i:d.personalities->instances)if(i.id==id)return &i;return nullptr;}
bool placing(RackOperationKind k){return k==RackOperationKind::place||k==RackOperationKind::duplicate;}
// A synth bank is off when disabled or bypassed; an effect bank when bypassed.
bool bypassed(const SoundDocument& d,NativeModule m,std::uint8_t b){return isSourceModule(m)?d.values[12+4*b]<.5f||d.values[212+2*b]>=.5f:d.values[572+23*b]>=.5f;}
}
juce::String rackDestinationText(const SoundDocument& d,const RackInstance& i){const auto b=rackUnitBinding(i,i.design.units.front().id)->slot;if(i.design.role==RackRole::synth){const auto order=std::find(d.layout.synthOrder.begin(),d.layout.synthOrder.begin()+d.layout.synthCount,b)-d.layout.synthOrder.begin();return "Synth "+juce::String(b+1)+"; synth order "+juce::String(int(order)+1)+(i.design.units.size()>1?"; owned inserts on Synth "+juce::String(b+1):"; no owned inserts");}const auto chain=d.layout.effects[b].chain;return chain==masterChain?"Shared Master":"Synth "+juce::String(chain+1)+" insert";}
juce::String rackStatusText(RackCandidateStatus s){constexpr const char* states[]{"Ready","Invalid operation","Capacity reached","Explicit replacement required","Explicit remove/remap/detach reconciliation required","Locked dependency","Stale selection"};return states[int(s)];}
juce::String rackHeaderText(const SoundDocument& d,const RackInstance& i){
 juce::String units;
 for(const auto& u:i.design.units){const auto b=rackUnitBinding(i,u.id)->slot;units+=(units.isEmpty()?"":" -> ")+moduleDisplayName(u.module)+" bank "+juce::String(b+1);if(bypassed(d,u.module,b))units+=" (bypassed)";}
 return i.design.name+(i.edited?" | Edited":"")+" | revision "+juce::String(std::to_string(i.design.revision))+"\n"+rackDestinationText(d,i)+" | "+units+"\n"+(rackLocked(d,i)?"AI rack locked; compatible manual controls remain editable":"AI rack unlocked")+" | "+i.design.intent;
}
bool rackOn(const SoundDocument& d,const RackInstance& i){return std::none_of(i.design.units.begin(),i.design.units.end(),[&](const auto& u){return bypassed(d,u.module,rackUnitBinding(i,u.id)->slot);});}
bool rackLocked(const SoundDocument& d,const RackInstance& i){return std::any_of(i.design.units.begin(),i.design.units.end(),[&](const auto& u){const auto b=rackUnitBinding(i,u.id)->slot;return isSourceModule(u.module)?d.synthLocks[b]:d.effectLocks[b];});}
bool parseRackAllocation(const juce::String& text,std::vector<UnitBinding>& out){out.clear();if(text.trim().isEmpty())return true;const auto parts=juce::StringArray::fromTokens(text,",","");if(parts.size()>9)return false;for(const auto& p:parts){const auto pair=juce::StringArray::fromTokens(p.trim(),":","");if(pair.size()!=2)return false;unsigned unit=0,slot=0;auto parse=[](const juce::String& s,unsigned& v){auto t=s.trim().toStdString();auto r=std::from_chars(t.data(),t.data()+t.size(),v);return r.ec==std::errc{}&&r.ptr==t.data()+t.size();};if(!parse(pair[0],unit)||!parse(pair[1],slot)||unit>8||slot<1||slot>8)return false;out.push_back({std::uint8_t(unit),std::uint8_t(slot-1)});}return true;}
RackReview reviewRack(InstrumentProcessor& processor,const RackReviewRequest& r){
 RackReview review;SoundDocument d;std::uint64_t e,token;if(!processor.trySoundSnapshot(d,e,token))return review;RackOperation op;op.newInstance=juce::Uuid();op.selected=r.selected;op.allowSlotReuse=r.reuse;op.destination.chain=r.chain;op.destination.position=r.position;
 const int kind=r.operation;
 if(kind<1||kind>8||kind==2||kind==3){review.difference="Choose an operation";return review;}
 op.kind=kind==1?RackOperationKind::place:kind==4?RackOperationKind::swap:kind==5?RackOperationKind::duplicate:kind==6?RackOperationKind::detach:kind==7?RackOperationKind::remove:RackOperationKind::move;
 const auto library=libraryRackDesigns();auto design=kind<=4&&r.localDesign?*r.localDesign:library[std::size_t(juce::jlimit(0,int(library.size())-1,r.example))];const auto* current=instance(d,r.selected);if(kind>=5&&current)design=current->design;
 op.destination.role=design.role;review.role=design.role;
 if(!parseRackAllocation(r.allocation,op.allocation)){review.difference="Invalid exact allocation. Use local unit:physical bank; banks start at 1";return review;}
 if(kind<=4){op.design=design;op.package=r.package;}
 if(current&&r.resolution==4)op.choices.detach.push_back(r.selected);
 const int c=r.control;if(current&&c>=0&&std::size_t(c)<current->design.panel.controls.size()&&(r.resolution==2||r.resolution==3)){
  RackControlResolution resolved{r.selected,std::uint8_t(c),{}};if(r.resolution==3){auto replacement=current->design.panel.controls[std::size_t(c)];replacement.key=r.remap.trim();const auto u=std::find_if(current->design.units.begin(),current->design.units.end(),[&](const auto& x){return x.id==replacement.unit;});const auto index=rackParameterIndex(u->module,0,replacement.key);if(!index){review.difference="Remap key is unknown; live mapping preserved";return review;}const auto& desc=parameterRegistry()[*index];replacement.minimum=desc.minimum;replacement.maximum=desc.maximum;replacement.response=FocusResponse::linear;replacement.widget=desc.kind==ParameterKind::continuous?replacement.widget:desc.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice;resolved.replacement=replacement;}op.choices.controls.push_back(resolved);
 }
 std::vector<RackOperation> operations;
 if(op.kind==RackOperationKind::remove&&current&&current->design.role==RackRole::synth&&r.moveDependencies){
  const auto synthBank=rackUnitBinding(*current,current->design.units.front().id)->slot;int masterPosition=int(d.layout.effectCount[masterChain]);
  for(std::size_t order=0;order<d.layout.effectCount[synthBank];++order){const auto bank=d.layout.effectOrder[synthBank][order];for(const auto& dependent:d.personalities->instances)if(dependent.design.role==RackRole::fx&&rackUnitBinding(dependent,dependent.design.units.front().id)->slot==bank){RackOperation move;move.kind=RackOperationKind::move;move.selected=dependent.id;move.destination={RackRole::fx,masterChain,std::uint8_t(masterPosition)};masterPosition+=int(dependent.bindings.size());operations.push_back(std::move(move));}}
 }
 operations.push_back(op);
 SoundDocument candidate;RackDifference diff;const auto outcome=processor.previewRackOperations(operations,e,candidate,diff);
 const auto* placed=outcome==RackCandidateStatus::ready?instance(candidate,placing(op.kind)?op.newInstance:r.selected):nullptr;const auto placement=placed?rackDestinationText(candidate,*placed):current?"Previous placement: "+rackDestinationText(d,*current):design.role==RackRole::synth?juce::String("Unplaced synth; Review must allocate a physical bank and synth order"):op.destination.chain==masterChain?juce::String("Shared Master"):"Synth "+juce::String(op.destination.chain+1)+" insert";
 juce::String banks="Required units: "+juce::String(int(design.units.size()))+"; allocation: ";if(placed)for(auto b:placed->bindings)banks+="unit "+juce::String(b.unit)+" -> bank "+juce::String(b.slot+1)+"; ";
 review.difference=rackStatusText(outcome)+"\n"+banks+"\nDestination: "+placement+(design.role==RackRole::fx?"; position "+juce::String(op.destination.position+1):juce::String{})+"\nSonic: "+diff.sonic+"\nPresentation: "+diff.presentation+"\nStructural: "+diff.structural+"\n"+diff.automationWarning+"\nRetained host automation envelopes stay attached to physical banks. Global macro targets retain their physical IDs; affected targets are listed above. Shared Master affects every synth. No library write on Apply or Cancel.";
 if(operations.size()>1)review.difference+="\nExplicit compound operation: move "+juce::String(int(operations.size()-1))+" independent FX personalities to Shared Master, then remove selected synth. One transaction and one Undo.";
 if(outcome==RackCandidateStatus::ready){review.draft=std::move(operations);review.expected=e;review.reset=token;}review.state=rackStatusText(outcome);return review;
}
RackApplied applyRackReview(InstrumentProcessor& processor,const RackReview& review){
 RackApplied result;if(review.draft.empty())return result;SoundDocument d;std::uint64_t e,token;if(!processor.trySoundSnapshot(d,e,token)||e!=review.expected||token!=review.reset)return result;
 const auto op=review.draft.back();result.applied=processor.applyRackOperations(review.draft,review.expected)==RackCandidateStatus::ready;result.status=result.applied?"Applied one reviewed rack transaction":"Apply rejected; sound preserved";
 if(result.applied&&placing(op.kind))result.placed=op.newInstance;return result;
}
RackApplied applyRackReview(InstrumentProcessor& processor,const RackReview& review,const RackLibraryPlacementPreparationPtr& preparation,const std::shared_ptr<const RackLibraryPlacementView>& authority){
 RackApplied result;if(review.draft.empty())return result;
 result.applied=processor.applyRackOperations(review.draft,review.expected,review.reset,preparation,authority)==RackCandidateStatus::ready;
 result.status=result.applied?"Applied one reviewed rack transaction":"Add rejected; sound preserved. Refresh the local selection and retry.";
 if(result.applied&&placing(review.draft.back().kind))result.placed=review.draft.back().newInstance;return result;
}
}
