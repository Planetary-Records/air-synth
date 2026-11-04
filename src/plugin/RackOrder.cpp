#include "RackOrder.h"
#include "RackReview.h"
#include "UserReach.h"
#include <algorithm>
namespace airack {
namespace {
const RackInstance* instance(const SoundDocument& d,juce::Uuid id){if(d.personalities)for(const auto& i:d.personalities->instances)if(i.id==id)return &i;return nullptr;}
// A synth rack's bank, or an FX rack's first effect bank.
std::uint8_t firstBank(const RackInstance& i){return rackUnitBinding(i,i.design.units.front().id)->slot;}
std::uint8_t chainOf(const SoundDocument& d,const RackInstance& i){return i.design.role==RackRole::synth?firstBank(i):d.layout.effects[firstBank(i)].chain;}
int indexIn(const std::uint8_t* order,std::size_t count,std::uint8_t bank){const auto at=std::find(order,order+count,bank);return at==order+count?-1:int(at-order);}
int synthIndex(const SoundDocument& d,const RackInstance& i){return indexIn(d.layout.synthOrder.data(),d.layout.synthCount,firstBank(i));}
int chainIndex(const SoundDocument& d,const RackInstance& i){const auto c=chainOf(d,i);return indexIn(d.layout.effectOrder[c].data(),d.layout.effectCount[c],firstBank(i));}
// The FX racks on a chain, in the chain's order.
std::vector<const RackInstance*> fxOn(const SoundDocument& d,std::uint8_t chain){
 std::vector<std::pair<int,const RackInstance*>> found;for(const auto& i:d.personalities->instances)if(i.design.role==RackRole::fx&&chainOf(d,i)==chain)if(const auto at=chainIndex(d,i);at>=0)found.push_back({at,&i});
 std::sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.first<b.first;});std::vector<const RackInstance*> out;for(const auto& f:found)out.push_back(f.second);return out;
}
// Whether a rack holds the bank: one of its units of the role's kind is bound to it.
bool held(const SoundDocument& d,RackRole role,std::uint8_t bank){
 if(d.personalities)for(const auto& i:d.personalities->instances)for(const auto& u:i.design.units)
  if(isSourceModule(u.module)==(role==RackRole::synth))if(const auto* b=rackUnitBinding(i,u.id);b!=nullptr&&b->slot==bank)return true;
 return false;
}
// A chain's lines in the chain's order: an FX rack at its first bank, a bare line for a bank no rack holds.
std::vector<StackRack> chainLines(const SoundDocument& d,std::uint8_t chain){
 std::vector<StackRack> out;
 for(std::size_t n=0;n<d.layout.effectCount[chain];++n){const auto bank=d.layout.effectOrder[chain][n];bool shown=false;
  if(d.personalities)for(const auto& i:d.personalities->instances)if(i.design.role==RackRole::fx&&firstBank(i)==bank){out.push_back({i.id,RackRole::fx,chain,bank});shown=true;}
  if(!shown&&!held(d,RackRole::fx,bank))out.push_back({bareBankId({RackRole::fx,bank}),RackRole::fx,chain,bank,true});}
 return out;
}
// A line's place in its chain's order, or -1.
int chainAt(const SoundDocument& d,const StackRack& r){return r.chain<d.layout.effectOrder.size()?indexIn(d.layout.effectOrder[r.chain].data(),d.layout.effectCount[r.chain],r.bank):-1;}
// rackOrder's lines, and how many of them close it under the divider.
std::pair<std::vector<StackRack>,std::size_t> walk(const SoundDocument& d){
 std::vector<StackRack> lines;std::vector<juce::Uuid> seen;
 const auto add=[&](const StackRack& r){lines.push_back(r);if(!r.bare)seen.push_back(r.id);};
 for(std::size_t n=0;n<d.layout.synthCount;++n){const auto bank=d.layout.synthOrder[n];bool shown=false;
  if(d.personalities)for(const auto& i:d.personalities->instances)if(i.design.role==RackRole::synth&&firstBank(i)==bank){add({i.id,RackRole::synth,bank,bank});shown=true;}
  if(!shown&&!held(d,RackRole::synth,bank))add({bareBankId({RackRole::synth,bank}),RackRole::synth,bank,bank,true});
  for(const auto& l:chainLines(d,bank))add(l);}
 const auto master=chainLines(d,masterChain);for(const auto& l:master)if(!l.bare)seen.push_back(l.id);
 if(d.personalities)for(const auto& i:d.personalities->instances)if(std::find(seen.begin(),seen.end(),i.id)==seen.end())lines.push_back({i.id,i.design.role,chainOf(d,i),firstBank(i)});
 lines.insert(lines.end(),master.begin(),master.end());return {lines,master.size()};
}
// The fixed bytes of a bare bank's ID.
constexpr std::uint8_t bareStem[14]{0x52,0x41,0x43,0x4b,0x2d,0x42,0x41,0x52,0x45,0x2d,0x42,0x41,0x4e,0x4b};
}
juce::Uuid bareBankId(BareBank b){
 std::uint8_t raw[16];std::copy(std::begin(bareStem),std::end(bareStem),raw);raw[14]=std::uint8_t(b.role);raw[15]=b.bank;return juce::Uuid(raw);
}
std::optional<BareBank> bareBankOf(juce::Uuid id){
 const RackLayout banks;const auto* raw=id.getRawData();
 if(!std::equal(std::begin(bareStem),std::end(bareStem),raw)||raw[14]>std::uint8_t(RackRole::fx))return std::nullopt;
 const auto role=RackRole(raw[14]);
 if(raw[15]>=(role==RackRole::synth?banks.synthUse.size():banks.effects.size()))return std::nullopt;
 return BareBank{role,raw[15]};
}
juce::String bareBankName(const SoundDocument& d,BareBank b){
 const auto n=juce::String(int(b.bank)+1);
 if(b.role==RackRole::synth)return "SYNTH "+n+" "+moduleDisplayName(bankModule(d,b.role,b.bank)).toUpperCase();
 return "FX "+n+" "+effectDisplayName(d.layout.effects[b.bank].kind).toUpperCase();
}
juce::String bareBankPlace(const SoundDocument& d,BareBank b){
 if(b.role==RackRole::synth)return "SYNTH "+juce::String(int(b.bank)+1);
 const auto chain=d.layout.effects[b.bank].chain;
 return chain==masterChain?juce::String("SHARED MASTER"):"SYNTH "+juce::String(int(chain)+1)+" INSERT";
}
std::vector<StackRack> rackOrder(const SoundDocument& d){return walk(d).first;}
std::optional<RackOperation> rackStep(const SoundDocument& d,juce::Uuid rack,bool up){
 const auto* selected=instance(d,rack);if(!selected)return std::nullopt;RackOperation op;op.kind=RackOperationKind::move;op.selected=rack;
 if(selected->design.role==RackRole::synth){
  const auto at=synthIndex(d,*selected);if(at<0)return std::nullopt;
  const auto hasRack=[&](std::uint8_t bank){return std::any_of(d.personalities->instances.begin(),d.personalities->instances.end(),[&](const auto& i){return i.design.role==RackRole::synth&&firstBank(i)==bank;});};
  // Erased and reinserted at its final index: the neighbour's own index, up or down.
  for(int n=up?at-1:at+1;n>=0&&n<int(d.layout.synthCount);n+=up?-1:1)if(hasRack(d.layout.synthOrder[std::size_t(n)])){op.destination={RackRole::synth,masterChain,std::uint8_t(n)};return op;}
  return std::nullopt;
 }
 const auto chain=chainOf(d,*selected);const auto racks=fxOn(d,chain);const auto self=std::find(racks.begin(),racks.end(),selected);if(self==racks.end())return std::nullopt;
 // The position counts the chain without the moved rack's own units.
 if(up){if(self==racks.begin())return std::nullopt;op.destination={RackRole::fx,chain,std::uint8_t(chainIndex(d,**(self-1)))};return op;}
 if(self+1==racks.end())return std::nullopt;const auto* next=*(self+1);
 op.destination={RackRole::fx,chain,std::uint8_t(chainIndex(d,*next)-int(selected->bindings.size())+int(next->bindings.size()))};return op;
}
juce::String rackPlaceText(const SoundDocument& d,const RackInstance& i){
 const auto chain=chainOf(d,i);const juce::String place=i.design.role==RackRole::synth?"SYNTH "+juce::String(chain+1):chain==masterChain?juce::String("SHARED MASTER"):"SYNTH "+juce::String(chain+1)+" INSERT";
 return place+(i.edited?" | EDITED":"");
}
juce::String rackMoveSentence(const juce::String& name,bool up,RackCandidateStatus status){return status==RackCandidateStatus::ready?"Moved "+name+(up?" up":" down"):name+" not moved: "+rackStatusText(status)+"; sound preserved";}
namespace {
// The bank's place in the synth order, or -1.
int synthAt(const SoundDocument& d,std::uint8_t bank){return indexIn(d.layout.synthOrder.data(),d.layout.synthCount,bank);}
std::optional<std::size_t> controlIndex(const RackInstance& i,const FocusedControl& c){
 const auto u=std::find_if(i.design.units.begin(),i.design.units.end(),[&](const auto& v){return v.id==c.unit;});
 const auto b=std::find_if(i.bindings.begin(),i.bindings.end(),[&](auto v){return v.unit==c.unit;});
 if(u==i.design.units.end()||b==i.bindings.end())return std::nullopt;return rackParameterIndex(u->module,b->slot,c.key);
}
}
std::optional<StackSpot> rackSpot(const SoundDocument& d,RackDestination t){
 const auto [order,masterLines]=walk(d);const auto end=order.size()-masterLines;
 if(t.role==RackRole::synth){
  for(std::size_t i=0;i<end;++i)if(synthAt(d,order[i].chain)>=int(t.position))return StackSpot{i,false};
  return StackSpot{end,false};
 }
 if(t.chain==masterChain){
  for(std::size_t i=end;i<order.size();++i)if(chainAt(d,order[i])>=int(t.position))return StackSpot{i,true};
  return StackSpot{order.size(),true};
 }
 const auto at=synthAt(d,t.chain);if(at<0)return std::nullopt;
 std::size_t first=end,last=end;
 for(std::size_t i=0;i<end;++i)if(order[i].chain==t.chain&&synthAt(d,order[i].chain)==at){if(first==end)first=i;last=i+1;}
 // A bank whose group shows nothing is drawn where it sits in the synth order.
 if(first==end){for(std::size_t i=0;i<end;++i)if(synthAt(d,order[i].chain)>at)return StackSpot{i,false};return StackSpot{end,false};}
 for(std::size_t i=first+(order[first].role==RackRole::synth?1:0);i<last;++i)if(chainAt(d,order[i])>=int(t.position))return StackSpot{i,false};
 return StackSpot{last,false};
}
std::vector<juce::Uuid> rackGroup(const SoundDocument& d,juce::Uuid rack){
 const auto* i=instance(d,rack);if(!i)return {};std::vector<juce::Uuid> out{rack};
 if(i->design.role==RackRole::synth)for(const auto& l:chainLines(d,firstBank(*i)))out.push_back(l.id);
 return out;
}
std::vector<StackDrop> rackDrops(const SoundDocument& d,juce::Uuid rack){
 const auto* lifted=instance(d,rack);if(!lifted)return {};
 const auto [full,masterLines]=walk(d);const auto group=rackGroup(d,rack);
 const auto homeAt=std::size_t(std::find_if(full.begin(),full.end(),[&](const auto& r){return r.id==rack;})-full.begin());
 const StackSpot home{homeAt,homeAt>=full.size()-masterLines};
 std::vector<StackRack> rest;for(const auto& r:full)if(std::find(group.begin(),group.end(),r.id)==group.end())rest.push_back(r);
 // The lifted lines are a master rack alone, or a synth rack's group above the divider.
 const std::size_t restMaster=masterLines-(home.master?1:0);
 const auto end=rest.size()-restMaster;
 std::vector<StackDrop> out;
 const auto add=[&](StackSpot s,std::optional<RackDestination> t){out.push_back({s,s==home?std::nullopt:t});};
 const bool synth=lifted->design.role==RackRole::synth;
 // A rack the walk missed lifts with nowhere to go but home.
 const bool walked=synth?synthIndex(d,*lifted)>=0:chainIndex(d,*lifted)>=0&&(chainOf(d,*lifted)==masterChain||synthAt(d,chainOf(d,*lifted))>=0);
 if(walked&&synth){
  const auto a=synthIndex(d,*lifted);
  for(std::size_t i=0;i<=end;++i){
   if(i==end){add({end,false},RackDestination{RackRole::synth,masterChain,std::uint8_t(d.layout.synthCount-1)});continue;}
   if(i>0&&rest[i].chain==rest[i-1].chain)continue;
   const auto g=synthAt(d,rest[i].chain);if(g<0)continue;
   add({i,false},RackDestination{RackRole::synth,masterChain,std::uint8_t(g-(a<g?1:0))});
  }
 }
 else if(walked){
  const auto own=chainOf(d,*lifted);const auto count=int(lifted->bindings.size());const auto ownAt=chainIndex(d,*lifted);
  // Counted as today's move counts: the chain without the lifted rack's own units.
  const auto before=[&](const StackRack& r){const auto at=chainAt(d,r);return RackDestination{RackRole::fx,r.chain,std::uint8_t(at-(r.chain==own&&ownAt<at?count:0))};};
  const auto atEnd=[&](std::uint8_t c){return RackDestination{RackRole::fx,c,std::uint8_t(int(d.layout.effectCount[c])-(c==own?count:0))};};
  for(std::size_t i=0;i<=end;++i){
   const StackRack* above=i>0?&rest[i-1]:nullptr;const StackRack* below=i<end?&rest[i]:nullptr;
   if(above&&below&&above->chain==below->chain)add({i,false},before(*below));
   else if(above)add({i,false},atEnd(above->chain));
   else if(below&&below->role==RackRole::fx)add({i,false},before(*below));
  }
  for(std::size_t k=0;k<=restMaster;++k)add({end+k,true},k<restMaster?before(rest[end+k]):atEnd(masterChain));
 }
 if(std::none_of(out.begin(),out.end(),[&](const auto& l){return l.spot==home;})){
  const auto at=std::find_if(out.begin(),out.end(),[&](const auto& l){return l.spot.before>home.before||(l.spot.before==home.before&&l.spot.master&&!home.master);});
  out.insert(at,{home,std::nullopt});
 }
 return out;
}
juce::String rackDestinationText(RackDestination t){
 const auto position=" POSITION "+juce::String(int(t.position)+1);
 if(t.role==RackRole::synth)return "SYNTH ORDER"+position;
 return (t.chain==masterChain?juce::String("SHARED MASTER"):"SYNTH "+juce::String(int(t.chain)+1)+" INSERT")+position;
}
juce::String rackDropSentence(const juce::String& name,RackDestination t,RackCandidateStatus status){
 if(status!=RackCandidateStatus::ready)return rackMoveSentence(name,false,status);
 const auto position=juce::String(int(t.position)+1);
 if(t.role==RackRole::synth)return "Moved "+name+" to synth order position "+position;
 return "Moved "+name+" to "+(t.chain==masterChain?juce::String("Shared Master"):"Synth "+juce::String(int(t.chain)+1)+" insert")+", position "+position;
}
RackInstance rackPreviewInstance(const RackDesign& design){
 RackInstance i;i.id=juce::Uuid::null();i.design=design;std::uint8_t synths=0,effects=0;
 for(const auto& u:design.units)i.bindings.push_back({u.id,isSourceModule(u.module)?synths++:effects++});
 return i;
}
std::vector<float> rackPreviewValues(const RackDesign& design){
 const auto shown=rackPreviewInstance(design);const auto defaults=defaultParameterValues();std::vector<float> out;
 for(const auto& c:design.panel.controls){
  const auto u=std::find_if(design.units.begin(),design.units.end(),[&](const auto& v){return v.id==c.unit;});
  if(u!=design.units.end())if(const auto own=std::find_if(u->defaults.begin(),u->defaults.end(),[&](const auto& v){return v.key==c.key;});own!=u->defaults.end()){out.push_back(own->normalized);continue;}
  const auto at=controlIndex(shown,c);out.push_back(at?defaults[*at]:0.0f);
 }
 return out;
}
ParameterValues rackPreviewParameters(const RackDesign& design){
 const auto shown=rackPreviewInstance(design);auto out=defaultParameterValues();
 for(const auto& u:design.units)if(const auto* b=rackUnitBinding(shown,u.id)){
  for(const auto& v:u.defaults)if(const auto at=rackParameterIndex(u.module,b->slot,v.key))out[*at]=v.normalized;
  if(isSourceModule(u.module)){out[drumModeParameterStart+b->slot]=u.module==NativeModule::drum?1.f:0.f;out[13+4*b->slot]=u.module==NativeModule::wavetable?1.f:0.f;}
 }
 return out;
}
std::vector<float> rackInstanceValues(const SoundDocument& d,const RackInstance& i){
 std::vector<float> out;
 for(const auto& c:i.design.panel.controls){const auto at=controlIndex(i,c);out.push_back(at?d.values[*at]:0.0f);}
 return out;
}
}
