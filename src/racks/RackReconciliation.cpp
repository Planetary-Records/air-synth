#include "RackReconciliation.h"
#include "RackLibrary.h"
#include "SoundDocument.h"
#include "UserReach.h"
#include <algorithm>
#include <limits>
#include <set>
#include <type_traits>
namespace airack {
namespace {
using Status=RackCandidateStatus;
const UnitBinding* binding(const RackInstance& i,std::uint8_t id){return rackUnitBinding(i,id);}
NativeModule module(const SoundDocument& d,const RackUnit& u,const UnitBinding& b){return isSourceModule(u.module)?sourceModule(d,b.slot):nativeModuleForEffect(d.layout.effects[b.slot].kind);}
AlgorithmRevision algorithm(const SoundDocument& d,NativeModule m,std::uint8_t slot){return rackAlgorithmRevision(d.compatibility,m,slot);}
bool present(const SoundDocument& d,const RackUnit& u,const UnitBinding& b){return isSourceModule(u.module)?b.slot<4&&d.layout.synthUse[b.slot]==SlotUse::present:b.slot<8&&d.layout.effects[b.slot].use==SlotUse::present;}
bool contains(const std::vector<juce::Uuid>& ids,const juce::Uuid& id){return std::find(ids.begin(),ids.end(),id)!=ids.end();}
const RackInstance* instance(const SoundDocument& d,const juce::Uuid& id){if(!d.personalities)return nullptr;for(const auto& i:d.personalities->instances)if(i.id==id)return &i;return nullptr;}
struct SourceOwner final {
    SlotUse use=SlotUse::neverUsed;
    juce::Uuid rack=juce::Uuid::null();
    std::uint8_t unit=0;
};
SourceOwner sourceOwner(const SoundDocument& d,std::uint8_t bank) noexcept {
    SourceOwner out;out.use=d.layout.synthUse[bank];if(out.use!=SlotUse::present||!d.personalities)return out;
    for(const auto& i:d.personalities->instances)for(const auto& u:i.design.units)if(isSourceModule(u.module))
        if(const auto* b=binding(i,u.id);b&&b->slot==bank){out.rack=i.id;out.unit=u.id;return out;}
    return out;
}
// One real primitive owns one ordinal; the large transactional trace stays on the heap.
struct SourceJournal final {
    SourceAssignmentTrace& trace;
    std::array<SourceOwner,4> current {};
    std::uint16_t operation=0;
    bool ordinalAvailable=true;
    SourceJournal(const SoundDocument& before,SourceAssignmentTrace& output):trace(output) {
        for(std::uint8_t bank=0;bank<4;++bank)current[bank]=sourceOwner(before,bank);
        if(trace.count){const auto last=trace.effects[trace.count-1].operation;ordinalAvailable=last!=UINT16_MAX;operation=std::uint16_t(last+unsigned(ordinalAvailable));}
    }
    bool append(std::uint8_t bank,SourceAssignmentEffectKind kind,SourceOwner after,bool invalidate,
                bool full=false,bool fresh=false,std::uint8_t copyFrom=noSlot) noexcept {
        if(!ordinalAvailable||trace.count>=trace.effects.size())return false;
        SourceAssignmentEffect effect;effect.kind=kind;effect.bank=bank;effect.copyFrom=copyFrom;effect.operation=operation;
        effect.beforeUse=current[bank].use;effect.afterUse=after.use;effect.beforeOwner=current[bank].rack;effect.afterOwner=after.rack;
        effect.beforeUnit=current[bank].unit;effect.afterUnit=after.unit;
        effect.invalidateTarget=invalidate;effect.fullNumericBank=full;effect.freshLogicalOwner=fresh;
        trace.effects[trace.count++]=effect;current[bank]=after;return true;
    }
    bool detach(const RackInstance& i,const SoundDocument& at) noexcept {
        for(const auto& u:i.design.units)if(isSourceModule(u.module))if(const auto* b=binding(i,u.id);b&&current[b->slot].rack==i.id)
            if(!append(b->slot,SourceAssignmentEffectKind::preserve,{at.layout.synthUse[b->slot],juce::Uuid::null(),0},true))return false;
        return true;
    }
};
static_assert(std::is_nothrow_copy_assignable_v<SourceAssignmentTrace>);
std::size_t position(const SoundDocument& d,std::uint8_t chain,std::uint8_t slot){const auto& order=d.layout.effectOrder[chain];return std::find(order.begin(),order.end(),slot)-order.begin();}
bool choicesValid(const SoundDocument& before,const RackReconcileChoices& c){
    std::set<juce::String> ids;
    for(const auto* list:{&c.detach,&c.remove,&c.replace}){if(list->size()>12)return false;for(const auto& id:*list)if(!instance(before,id)||!ids.insert(id.toString()).second)return false;}
    if(c.controls.size()>96)return false;std::set<std::pair<juce::String,unsigned>> controls;
    for(const auto& r:c.controls){const auto* i=instance(before,r.instance);if(!i||r.control>=i->design.panel.controls.size()||!controls.emplace(r.instance.toString(),r.control).second)return false;}
    return true;
}
bool locked(const SoundDocument& d,const RackInstance& i){
    for(const auto& u:i.design.units){const auto* b=binding(i,u.id);if(!b)return true;if(isSourceModule(u.module)?d.synthLocks[b->slot]:d.effectLocks[b->slot])return true;
        const auto prefix=juce::String(isSourceModule(u.module)?"synth.":"effect.")+juce::String(b->slot+1)+".";const auto r=parameterRegistry();for(std::size_t n=0;n<r.size();++n)if(r[n].id.startsWith(prefix)&&d.controlLocks[n])return true;}
    return false;
}
bool locationChanged(const SoundDocument& a,const SoundDocument& b,const RackInstance& i){
    for(const auto& u:i.design.units){const auto* x=binding(i,u.id);if(!x)return true;
        if(isSourceModule(u.module)){if(a.layout.synthUse[x->slot]!=b.layout.synthUse[x->slot]||std::find(a.layout.synthOrder.begin(),a.layout.synthOrder.end(),x->slot)-a.layout.synthOrder.begin()!=std::find(b.layout.synthOrder.begin(),b.layout.synthOrder.end(),x->slot)-b.layout.synthOrder.begin())return true;}
        else {const auto ca=a.layout.effects[x->slot].chain,cb=b.layout.effects[x->slot].chain;if(ca!=cb||position(a,ca,x->slot)!=position(b,cb,x->slot)||a.layout.effects[x->slot].use!=b.layout.effects[x->slot].use)return true;}}
    return false;
}
Status layoutEdit(SoundDocument& d,const RackEdit& e){switch(editRackLayout(d.layout,e,d.layout)){case RackEditResult::applied:return Status::ready;case RackEditResult::capacityReached:return Status::capacityReached;case RackEditResult::replacementRequired:return Status::replacementRequired;default:return Status::invalid;}}
bool independentDependency(const SoundDocument& d,std::uint8_t slot,const juce::Uuid& selected){
    if(!d.personalities)return false;for(const auto& i:d.personalities->instances)if(i.id!=selected&&i.design.role==RackRole::fx)for(const auto& b:i.bindings)if(d.layout.effects[b.slot].use==SlotUse::present&&d.layout.effects[b.slot].chain==slot)return true;return false;
}
Status eraseUnits(SoundDocument& d,const RackInstance& i,SourceJournal* journal=nullptr){
    for(auto it=i.design.units.rbegin();it!=i.design.units.rend();++it){const auto* b=binding(i,it->id);if(!b)return Status::invalid;if(!present(d,*it,*b))continue;
        if(isSourceModule(it->module)&&independentDependency(d,b->slot,i.id))return Status::reconciliationRequired;
        const auto s=layoutEdit(d,{isSourceModule(it->module)?RackEditKind::removeSynth:RackEditKind::removeEffect,b->slot});if(s!=Status::ready)return s;
        if(journal&&isSourceModule(it->module)&&!journal->append(b->slot,SourceAssignmentEffectKind::preserve,
            {d.layout.synthUse[b->slot],juce::Uuid::null(),0},true))return Status::invalid;
    }return Status::ready;
}
Status removeChosenUnits(const SoundDocument& before,SoundDocument& candidate,const RackReconcileChoices& choices,SoundEditOrigin origin,SourceJournal* journal=nullptr){
    // The permission list is unordered: remove explicit FX dependencies first.
    for(const auto role:{RackRole::fx,RackRole::synth})for(const auto& id:choices.remove){const auto* old=instance(before,id);if(old->design.role!=role)continue;
        if(origin==SoundEditOrigin::aiCandidate&&locked(before,*old))return Status::lockedDependency;
        const auto status=eraseUnits(candidate,*old,journal);if(status!=Status::ready)return status;
    }return Status::ready;
}
void append(juce::String& s,const juce::String& text){if(s.isNotEmpty())s+="; ";s+=text;}
juce::String unitName(const RackUnit& u,const UnitBinding& b){return juce::String(isSourceModule(u.module)?"Synth ":"FX ")+juce::String(b.slot+1)+" (local "+juce::String(u.id)+")";}
// A face edit's presentation lines: the name, each control added, removed or changed (a control is its unit and key),
// the kept controls' order, the sections, and the layout and style tokens by their design-file names.
void appendFaceDifference(juce::String& lines,const RackDesign& a,const RackDesign& b){
    if(a.name!=b.name)append(lines,"Name "+a.name+" -> "+b.name);
    const auto& was=a.panel.controls;const auto& now=b.panel.controls;
    const auto at=[](const std::vector<FocusedControl>& v,const FocusedControl& c){return std::find_if(v.begin(),v.end(),[&](const auto& x){return x.unit==c.unit&&x.key==c.key;});};
    std::vector<std::ptrdiff_t> kept;
    for(const auto& c:now){const auto old=at(was,c);
        if(old==was.end()){append(lines,"Add control "+c.label);continue;}
        kept.push_back(old-was.begin());if(!(*old==c))append(lines,"Change control "+(old->label==c.label?c.label:old->label+" -> "+c.label));}
    for(const auto& c:was)if(at(now,c)==now.end())append(lines,"Remove control "+c.label);
    if(!std::is_sorted(kept.begin(),kept.end()))append(lines,"Reorder controls");
    const auto names=[](const std::vector<juce::String>& v){juce::StringArray s;for(const auto& n:v)s.add(n);return s.joinIntoString(", ");};
    if(a.panel.sections!=b.panel.sections)append(lines,"Sections "+names(a.panel.sections)+" -> "+names(b.panel.sections));
    juce::var x,y;if(!encodeRackDesignObject(a,x,RackValidationMode::instanceRecall)||!encodeRackDesignObject(b,y,RackValidationMode::instanceRecall))return;
    for(const char* field:{"layout","surface","accent","frame","emphasis","spacing","density"}){
        const auto p=x["panel"][field].toString(),q=y["panel"][field].toString();
        if(p!=q)append(lines,juce::String(field).substring(0,1).toUpperCase()+juce::String(field).substring(1)+" "+p+" -> "+q);}
}
}
bool rackControlActive(const SoundDocument& d,const RackInstance& i,std::size_t n){
    if(n>=i.design.panel.controls.size())return false;const auto& c=i.design.panel.controls[n];auto u=std::find_if(i.design.units.begin(),i.design.units.end(),[&](auto& u){return u.id==c.unit;});if(u==i.design.units.end())return false;const auto* b=binding(i,u->id);return b&&present(d,*u,*b)&&rackParameterActive(module(d,*u,*b),c.key)&&rackParameterIndex(u->module,b->slot,c.key).has_value();
}
bool validateRackInstances(const SoundDocument& d){
    if(!validateRackLayout(d.layout))return false;if(!d.personalities)return true;if(d.personalities->instances.size()>12)return false;
    std::set<juce::String> ids;std::array<bool,4> synths{};std::array<bool,8> effects{};
    for(const auto& i:d.personalities->instances){if(i.id.isNull()||(i.package&&!validRackPackageRef(*i.package))||!ids.insert(i.id.toString()).second||!validateRackDesign(i.design,RackValidationMode::instanceRecall)||i.bindings.size()!=i.design.units.size())return false;
        std::set<unsigned> locals;std::uint8_t chain=masterChain;std::size_t start=0,n=0;
        for(const auto& u:i.design.units){const auto* b=binding(i,u.id);if(!b||!locals.insert(b->unit).second||!present(d,u,*b)||module(d,u,*b)!=u.module||algorithm(d,u.module,b->slot)!=u.algorithmRevision)return false;
            if(isSourceModule(u.module)){if(synths[b->slot])return false;synths[b->slot]=true;chain=b->slot;start=0;}
            else {if(effects[b->slot])return false;effects[b->slot]=true;const auto c=d.layout.effects[b->slot].chain;if(n==0&&i.design.role==RackRole::fx){chain=c;start=position(d,c,b->slot);}if(c!=chain||position(d,c,b->slot)!=start+n)return false;++n;}}
        for(const auto& b:i.bindings)if(std::none_of(i.design.units.begin(),i.design.units.end(),[&](auto& u){return u.id==b.unit;}))return false;
    }return true;
}
static Status reconcileRackEditImpl(const SoundDocument& before,const SoundDocument& proposed,const RackReconcileChoices& choices,SoundEditOrigin origin,SoundDocument& output,RackDifference& difference,bool removalsStaged,SourceJournal* journal=nullptr){
    try {
        RoutedPatch patch;if(!makeRoutedPatch(static_cast<const SoundAudioState&>(proposed),patch)||!choicesValid(before,choices))return Status::invalid;
        auto candidate=proposed;auto state=std::make_shared<RackPersonalityState>();RackDifference diff;
        if(before.personalities)state->instances=before.personalities->instances;
        if(proposed.personalities)for(const auto& i:proposed.personalities->instances){auto existing=std::find_if(state->instances.begin(),state->instances.end(),[&](auto& a){return a.id==i.id;});if(existing==state->instances.end())state->instances.push_back(i);else if(contains(choices.replace,i.id))*existing=i;else if(i!=*existing)return Status::reconciliationRequired;}
        if(!removalsStaged){const auto status=removeChosenUnits(before,candidate,choices,origin,journal);if(status!=Status::ready)return status;}
        if(journal)for(const auto& i:state->instances)if(contains(choices.detach,i.id))if(!journal->detach(i,candidate))return Status::invalid;
        std::erase_if(state->instances,[&](auto& i){return contains(choices.detach,i.id)||contains(choices.remove,i.id);});
        if(state->instances.size()>12)return Status::capacityReached;
        for(const auto* ids:{&choices.detach,&choices.remove})for(const auto role:{RackRole::fx,RackRole::synth})for(const auto& id:*ids){const auto* old=instance(before,id);if(old->design.role!=role)continue;if(origin==SoundEditOrigin::aiCandidate&&locked(before,*old))return Status::lockedDependency;append(diff.structural,juce::String(ids==&choices.detach?"Detach ":"Remove ")+id.toString());}
        for(auto& i:state->instances){const auto* old=instance(before,i.id);bool changed=false;
            if(old&&contains(choices.replace,i.id)){changed=i.design!=old->design||i.bindings!=old->bindings;}
            std::vector<RackUnit> units;std::vector<UnitBinding> bindings;
            for(auto u:i.design.units){const auto* b=binding(i,u.id);if(!b)return Status::invalid;
                if(!present(candidate,u,*b)){changed=true;append(diff.structural,"Remove "+unitName(u,*b));continue;}
                auto actual=module(candidate,u,*b);if(actual!=u.module){u.module=actual;changed=true;append(diff.structural,"Engine/type "+unitName(u,*b));
                    const auto defaults=defaultParameterValues();const auto registry=parameterRegistry();const auto prefix=juce::String(isSourceModule(u.module)?"synth.":"effect.")+juce::String(b->slot+1)+".";
                    for(std::size_t j=0;j<registry.size();++j)if(registry[j].id.startsWith(prefix)){
                        const auto key=registry[j].id.substring(prefix.length());if(!rackParameterActive(u.module,key))continue;
                        auto existing=std::find_if(u.defaults.begin(),u.defaults.end(),[&](auto& p){return p.key==key;});
                        if(existing==u.defaults.end())u.defaults.push_back({u.id,key,key=="engine"?(u.module==NativeModule::wavetable?1.f:0.f):defaults[j]});
                        else if(key=="engine"&&u.module!=NativeModule::drum)existing->normalized=u.module==NativeModule::wavetable?1.f:0.f;
                    }
                }
                const auto actualRevision=algorithm(candidate,u.module,b->slot);
                if(u.algorithmRevision!=actualRevision){u.algorithmRevision=actualRevision;changed=true;append(diff.structural,"Algorithm "+unitName(u,*b));}
                units.push_back(u);bindings.push_back(*b);
            }
            if(units.empty()||(i.design.role==RackRole::synth&&!isSourceModule(units.front().module)))return Status::reconciliationRequired;
            const std::size_t firstFx=i.design.role==RackRole::synth?1:0;
            if(units.size()>firstFx){
                const auto chain=candidate.layout.effects[bindings[firstFx].slot].chain;
                std::vector<std::size_t> order(units.size());for(std::size_t n=0;n<order.size();++n)order[n]=n;
                std::sort(order.begin()+firstFx,order.end(),[&](auto a,auto b){return position(candidate,chain,bindings[a].slot)<position(candidate,chain,bindings[b].slot);});
                auto sortedUnits=units;auto sortedBindings=bindings;for(std::size_t n=0;n<order.size();++n){sortedUnits[n]=units[order[n]];sortedBindings[n]=bindings[order[n]];}changed|=sortedUnits!=units;units=std::move(sortedUnits);bindings=std::move(sortedBindings);
            }
            auto controls=i.design.panel.controls;std::vector<FocusedControl> retained;
            for(std::size_t n=0;n<controls.size();++n){auto c=controls[n];const auto resolution=std::find_if(choices.controls.begin(),choices.controls.end(),[&](auto& r){return r.instance==i.id&&r.control==n;});
                if(resolution!=choices.controls.end()){changed=true;if(!resolution->replacement){append(diff.presentation,"Remove mapping "+c.label);continue;}c=*resolution->replacement;append(diff.presentation,"Remap "+c.label);}
                if(std::none_of(units.begin(),units.end(),[&](auto& u){return u.id==c.unit;}))return Status::reconciliationRequired;retained.push_back(c);
            }
            i.design.units=std::move(units);i.bindings=std::move(bindings);i.design.panel.controls=std::move(retained);
            if(old&&locationChanged(before,candidate,*old)){changed=true;for(const auto& u:i.design.units){const auto* b=binding(i,u.id);append(diff.structural,"Order/destination "+unitName(u,*b)+(isSourceModule(u.module)?"":" chain "+juce::String(candidate.layout.effects[b->slot].chain)+" position "+juce::String(int(position(candidate,candidate.layout.effects[b->slot].chain,b->slot)))));}}
            if(old&&changed){if(origin==SoundEditOrigin::aiCandidate&&locked(before,*old))return Status::lockedDependency;
                // A supplied replacement is an explicit snapshot swap. Inferred
                // recipe/mapping/destination edits fork the existing snapshot.
                if(!contains(choices.replace,i.id)){if(i.design.revision==std::numeric_limits<std::uint64_t>::max())return Status::invalid;++i.design.revision;i.edited=true;}
                append(diff.structural,"Revision/Edited "+i.id.toString());}
        }
        candidate.personalities=state->instances.empty()?nullptr:state;
        if(!validateRackInstances(candidate))return Status::reconciliationRequired;
        if(origin==SoundEditOrigin::aiCandidate&&!validateAiSoundEdit(before,candidate))return Status::lockedDependency;
        if(candidate.values!=before.values)append(diff.sonic,"Live patch values");
        if(candidate.personalities)for(const auto& i:candidate.personalities->instances){const auto* old=instance(before,i.id);if(old)for(std::size_t n=0;n<i.design.panel.controls.size();++n)if(!rackControlActive(candidate,i,n))append(diff.presentation,"Disabled retained mapping "+i.design.panel.controls[n].label);}
        for(std::size_t n=0;n<8;++n){bool affected=candidate.macros[n]!=before.macros[n];
            for(const auto key:{before.macros[n].targetKey,candidate.macros[n].targetKey})if(const auto target=parameterIndexForKey(key)){
                const auto id=parameterRegistry()[*target].id;
                for(bool isSynth:{true,false})for(std::uint8_t slot=0;slot<(isSynth?4:8);++slot){
                    const auto prefix=juce::String(isSynth?"synth.":"effect.")+juce::String(slot+1)+".";if(!id.startsWith(prefix))continue;
                    if(isSynth)affected|=before.layout.synthUse[slot]!=candidate.layout.synthUse[slot]||before.values[13+4*slot]!=candidate.values[13+4*slot]||before.values[drumModeParameterStart+slot]!=candidate.values[drumModeParameterStart+slot]
                        ||std::find(before.layout.synthOrder.begin(),before.layout.synthOrder.end(),slot)-before.layout.synthOrder.begin()!=std::find(candidate.layout.synthOrder.begin(),candidate.layout.synthOrder.end(),slot)-candidate.layout.synthOrder.begin();
                    else affected|=before.layout.effects[slot]!=candidate.layout.effects[slot]||position(before,before.layout.effects[slot].chain,slot)!=position(candidate,candidate.layout.effects[slot].chain,slot);
                    const auto owner=[&](const SoundDocument& d)->const RackInstance*{if(d.personalities)for(const auto& i:d.personalities->instances)for(const auto& u:i.design.units){const auto* b=binding(i,u.id);if(isSourceModule(u.module)==isSynth&&b&&b->slot==slot)return &i;}return nullptr;};
                    const auto* a=owner(before);const auto* b=owner(candidate);affected|=(a==nullptr)!=(b==nullptr)||(a&&b&&(a->id!=b->id||a->design.id!=b->design.id||a->design.revision!=b->design.revision));
                }
            }
            if(affected)append(diff.automationWarning,"Global macro "+juce::String(n+1)+" target affected");
        }
        output=std::move(candidate);difference=std::move(diff);return Status::ready;
    }catch(...){return Status::invalid;}
}
Status reconcileRackEdit(const SoundDocument& before,const SoundDocument& proposed,const RackReconcileChoices& choices,SoundEditOrigin origin,SoundDocument& output,RackDifference& difference){
    return reconcileRackEditImpl(before,proposed,choices,origin,output,difference,false);
}
static Status makeRackCandidateImpl(const SoundDocument& before,const RackOperation& op,SoundEditOrigin origin,SoundDocument& output,RackDifference& difference,bool capturedNative,SourceAssignmentTrace* trace){
    try {
        std::optional<SourceJournal> journalStorage;if(trace)journalStorage.emplace(before,*trace);auto* journal=journalStorage?&*journalStorage:nullptr;
        if(!validateSoundDocument(before)||(op.package&&!validRackPackageRef(*op.package))||op.kind>RackOperationKind::face||op.destination.role>RackRole::fx||op.destination.chain>masterChain||op.allocation.size()>9||!choicesValid(before,op.choices))return Status::invalid;
        auto candidate=before;auto state=std::make_shared<RackPersonalityState>();if(before.personalities)state->instances=before.personalities->instances;
        auto choices=op.choices;RackDifference diff;const auto* selected=instance(before,op.selected);
        const bool selection=op.kind==RackOperationKind::swap||op.kind==RackOperationKind::duplicate||op.kind==RackOperationKind::detach||op.kind==RackOperationKind::move||op.kind==RackOperationKind::remove||op.kind==RackOperationKind::face;
        if(selection&&!selected)return Status::invalid;if(selected&&origin==SoundEditOrigin::aiCandidate&&locked(before,*selected))return Status::lockedDependency;
        // Every face edit forks the design: the library's design and the sound's never share an ID and revision with
        // different content, which the catalog refuses. A control on another key needs that key active on its bank.
        if(op.kind==RackOperationKind::face){
            if(!op.design||op.design->id!=selected->design.id||op.design->role!=selected->design.role||op.design->units!=selected->design.units
               ||selected->design.revision==std::numeric_limits<std::uint64_t>::max())return Status::invalid;
            auto edited=*selected;edited.design.panel=op.design->panel;edited.design.name=op.design->name;
            if(edited.design==selected->design)return Status::invalid;
            ++edited.design.revision;edited.edited=true;
            if(!validateRackDesign(edited.design,RackValidationMode::instanceRecall))return Status::invalid;
            const auto& old=selected->design.panel.controls;
            for(std::size_t n=0;n<edited.design.panel.controls.size();++n){const auto& c=edited.design.panel.controls[n];
                if(std::none_of(old.begin(),old.end(),[&](const auto& o){return o.unit==c.unit&&o.key==c.key;})&&!rackControlActive(before,edited,n))return Status::invalid;}
            for(auto& i:state->instances)if(i.id==edited.id)i=edited;
            candidate.personalities=state;choices.replace.push_back(edited.id);
            SoundDocument reconciled;RackDifference reconciledDiff;const auto status=reconcileRackEditImpl(before,candidate,choices,origin,reconciled,reconciledDiff,false,journal);if(status!=Status::ready)return status;
            appendFaceDifference(reconciledDiff.presentation,selected->design,edited.design);output=std::move(reconciled);difference=std::move(reconciledDiff);return Status::ready;
        }
        if(op.kind==RackOperationKind::detach){choices.detach.push_back(op.selected);return reconcileRackEditImpl(before,candidate,choices,origin,output,difference,false,journal);}
        if(op.kind==RackOperationKind::remove){choices.remove.push_back(op.selected);return reconcileRackEditImpl(before,candidate,choices,origin,output,difference,false,journal);}
        if(op.kind==RackOperationKind::move){
            if(op.destination.role!=selected->design.role)return Status::invalid;
            const auto& units=selected->design.units;
            if(selected->design.role==RackRole::synth){const auto* b=binding(*selected,units[0].id);auto s=layoutEdit(candidate,{RackEditKind::moveSynth,b->slot,masterChain,op.destination.position});if(s!=Status::ready)return s;}
            else {
                const auto count=selected->bindings.size();const auto oldChain=before.layout.effects[rackUnitBinding(*selected,selected->design.units.front().id)->slot].chain;
                const auto finalCount=candidate.layout.effectCount[op.destination.chain]-(oldChain==op.destination.chain?count:0);
                if(op.destination.position>finalCount)return Status::invalid;
                // Remove/reinsert the whole segment in fixed layout storage.
                for(const auto& u:units){auto s=layoutEdit(candidate,{RackEditKind::removeEffect,binding(*selected,u.id)->slot});if(s!=Status::ready)return s;}
                for(std::size_t n=0;n<units.size();++n){const auto slot=binding(*selected,units[n].id)->slot;auto s=layoutEdit(candidate,{RackEditKind::addEffect,slot,op.destination.chain,std::uint8_t(op.destination.position+n),effectKindForModule(units[n].module),true});if(s!=Status::ready)return s;}
            }
            if(journal&&selected->design.role==RackRole::synth){const auto* b=binding(*selected,units[0].id);
                if(!journal->append(b->slot,SourceAssignmentEffectKind::preserve,sourceOwner(candidate,b->slot),false))return Status::invalid;}
            return reconcileRackEditImpl(before,candidate,choices,origin,output,difference,false,journal);
        }
        RackDesign design;if(op.kind==RackOperationKind::duplicate)design=selected->design;else {if(!op.design)return Status::invalid;design=*op.design;}
        const bool duplicate=op.kind==RackOperationKind::duplicate,adopt=op.kind==RackOperationKind::adopt,swap=op.kind==RackOperationKind::swap;
        if(!validateRackDesign(design,duplicate||capturedNative?RackValidationMode::instanceRecall:RackValidationMode::portableDesign)||design.role!=op.destination.role)return Status::invalid;
        if(!swap&&op.newInstance.isNull())return Status::invalid;if(!swap&&instance(before,op.newInstance))return Status::invalid;
        const auto removalStatus=removeChosenUnits(before,candidate,choices,origin,journal);if(removalStatus!=Status::ready)return removalStatus;
        std::erase_if(state->instances,[&](const auto& i){return contains(choices.remove,i.id);});candidate.personalities=state;
        if(!swap&&state->instances.size()==12)return Status::capacityReached;
        RackInstance placed;placed.id=swap?op.selected:op.newInstance;placed.design=design;placed.edited=duplicate?selected->edited:false;
        placed.package=duplicate?selected->package:op.package;
        auto allocation=op.allocation;
        const auto availableSlot=[&](bool synth,const auto& used)->std::uint8_t{
            for(const auto use:{SlotUse::neverUsed,SlotUse::retired}){
                if(use==SlotUse::retired&&!op.allowSlotReuse)break;
                for(std::uint8_t slot=0;slot<used.size();++slot)if(!used[slot]&&(synth?candidate.layout.synthUse[slot]:candidate.layout.effects[slot].use)==use)return slot;
            }return noSlot;
        };
        if(adopt&&allocation.size()!=design.units.size())return Status::invalid;
        if(swap){if(!op.allowSlotReuse)return Status::replacementRequired;
            // Keep an incumbent synth bank/chain when swapping a synth recipe;
            // independent insert segments are not children of its personality.
            auto erased=*selected;if(selected->design.role==RackRole::synth&&design.role==RackRole::synth)erased.design.units.erase(erased.design.units.begin());
            auto s=eraseUnits(candidate,erased,journal);if(s!=Status::ready)return s;
            if(journal&&!journal->detach(*selected,candidate))return Status::invalid;
            std::erase_if(state->instances,[&](auto& i){return i.id==op.selected;});choices.replace.push_back(op.selected);
            if(allocation.empty()){
                std::array<bool,4> synthSlots{};std::array<bool,8> fxSlots{};
                for(std::size_t n=0;n<design.units.size();++n){const bool isSynth=isSourceModule(design.units[n].module);std::uint8_t slot=noSlot;
                    if(n<selected->design.units.size()&&isSynth==isSourceModule(selected->design.units[n].module))slot=binding(*selected,selected->design.units[n].id)->slot;
                    else slot=isSynth?availableSlot(true,synthSlots):availableSlot(false,fxSlots);
                    if(slot==noSlot)return Status::capacityReached;(isSynth?synthSlots[slot]:fxSlots[slot])=true;allocation.push_back({design.units[n].id,slot});
                }
            }
        }
        if(!allocation.empty()&&allocation.size()!=design.units.size())return Status::invalid;
        std::array<bool,4> usedSynth{};std::array<bool,8> usedFx{};std::uint8_t synthSlot=noSlot;
        for(std::size_t n=0;n<design.units.size();++n){const auto& u=design.units[n];const bool isSynth=isSourceModule(u.module);std::uint8_t slot=noSlot;
            if(!allocation.empty()){auto b=std::find_if(allocation.begin(),allocation.end(),[&](auto b){return b.unit==u.id;});if(b==allocation.end())return Status::invalid;slot=b->slot;}
            else slot=isSynth?availableSlot(true,usedSynth):availableSlot(false,usedFx);
            if(slot==noSlot)return Status::capacityReached;if(slot>=(isSynth?4:8))return Status::invalid;
            auto& seen=isSynth?usedSynth[slot]:usedFx[slot];if(seen)return Status::invalid;seen=true;
            for(const auto& i:state->instances)for(const auto& old:i.design.units){const auto* b=binding(i,old.id);if(isSourceModule(old.module)==isSynth&&b->slot==slot&&(adopt||(!contains(choices.detach,i.id)&&!contains(choices.remove,i.id)&&!contains(choices.replace,i.id))))return op.allowSlotReuse?Status::reconciliationRequired:Status::replacementRequired;}
            const auto use=isSynth?candidate.layout.synthUse[slot]:candidate.layout.effects[slot].use;
            if(adopt){if(use!=SlotUse::present)return Status::invalid;UnitBinding b{u.id,slot};if(module(candidate,u,b)!=u.module||algorithm(candidate,u.module,slot)!=u.algorithmRevision)return Status::invalid;
                if(journal&&isSynth&&!journal->append(slot,SourceAssignmentEffectKind::preserve,{use,placed.id,u.id},true,false,true))return Status::invalid;}
            else {
                if(use!=SlotUse::neverUsed&&!op.allowSlotReuse)return Status::replacementRequired;
                if(use!=SlotUse::neverUsed)append(diff.automationWarning,"Reuse bank "+juce::String(slot+1));
                const bool incumbentSynth=swap&&isSynth&&selected->design.role==RackRole::synth&&slot==binding(*selected,selected->design.units[0].id)->slot;
                if(use==SlotUse::present&&!incumbentSynth){if(isSynth&&independentDependency(candidate,slot,placed.id))return Status::reconciliationRequired;auto s=layoutEdit(candidate,{isSynth?RackEditKind::removeSynth:RackEditKind::removeEffect,slot});if(s!=Status::ready)return s;}
                const auto chain=design.role==RackRole::synth?synthSlot:op.destination.chain;
                auto s=layoutEdit(candidate,{incumbentSynth?RackEditKind::moveSynth:(isSynth?RackEditKind::addSynth:RackEditKind::addEffect),slot,isSynth?masterChain:chain,std::uint8_t(isSynth?op.destination.position:(design.role==RackRole::synth?n-1:op.destination.position+n)),isSynth?EffectKind::drive:effectKindForModule(u.module),op.allowSlotReuse});if(s!=Status::ready)return s;
                const auto defaults=defaultParameterValues();const auto registry=parameterRegistry();const auto prefix=juce::String(isSynth?"synth.":"effect.")+juce::String(slot+1)+".";
                const bool retainedBank=swap&&std::any_of(selected->design.units.begin(),selected->design.units.end(),[&](const auto& old){
                    return isSourceModule(old.module)==isSynth&&binding(*selected,old.id)->slot==slot;
                });
                if(journal&&isSynth){
                    const auto kind=duplicate?SourceAssignmentEffectKind::copy:retainedBank?SourceAssignmentEffectKind::preserve:SourceAssignmentEffectKind::clear;
                    if(!journal->append(slot,kind,{candidate.layout.synthUse[slot],placed.id,u.id},true,
                        duplicate||!retainedBank,!swap,duplicate?binding(*selected,u.id)->slot:noSlot))return Status::invalid;
                }
                const auto active=unitReach(u.module,slot,u.algorithmRevision);
                for(std::size_t j=0;j<registry.size();++j)if(registry[j].id.startsWith(prefix)
                    &&(!retainedBank||std::binary_search(active.begin(),active.end(),j)))candidate.values[j]=defaults[j];
                const auto rememberedEngine=isSynth?candidate.values[13+4*slot]:0.f;
                if(!setRackAlgorithmRevision(candidate.compatibility,u.module,slot,u.algorithmRevision))return Status::invalid;
                for(const auto& p:u.defaults)candidate.values[*rackParameterIndex(u.module,slot,p.key)]=p.normalized;
                if(isSynth){candidate.values[drumModeParameterStart+slot]=u.module==NativeModule::drum?1.f:0.f;
                    candidate.values[13+4*slot]=u.module==NativeModule::drum?rememberedEngine:u.module==NativeModule::wavetable?1.f:0.f;}
                if(duplicate){const auto source=binding(*selected,u.id)->slot;for(std::size_t j=0;j<registry.size();++j)if(registry[j].id.startsWith(prefix)){const auto key=registry[j].id.substring(prefix.length());candidate.values[j]=before.values[*rackParameterIndex(u.module,source,key)];}
                    if(isSynth)candidate.compatibility.synth[slot]=before.compatibility.synth[source];else candidate.compatibility.effects[slot]=before.compatibility.effects[source];}
            }
            if(isSynth)synthSlot=slot;placed.bindings.push_back({u.id,slot});append(diff.structural,"Bind "+unitName(u,{u.id,slot}));
        }
        state->instances.push_back(placed);candidate.personalities=state;
        SoundDocument reconciled;RackDifference reconciledDiff;const auto status=reconcileRackEditImpl(before,candidate,choices,origin,reconciled,reconciledDiff,true,journal);if(status!=Status::ready)return status;
        append(reconciledDiff.structural,diff.structural);append(reconciledDiff.automationWarning,diff.automationWarning);output=std::move(reconciled);difference=std::move(reconciledDiff);return Status::ready;
    }catch(...){return Status::invalid;}
}
Status makeRackCandidate(const SoundDocument& before,const RackOperation& operation,SoundEditOrigin origin,SoundDocument& output,RackDifference& difference,SourceAssignmentTrace* trace){
    try {
        if(trace&&trace->count>trace->effects.size())return Status::invalid;
        auto staged=trace?std::make_unique<SourceAssignmentTrace>(*trace):nullptr;
        const auto status=makeRackCandidateImpl(before,operation,origin,output,difference,false,staged.get());
        if(status==Status::ready&&trace)*trace=*staged;return status;
    }catch(...){return Status::invalid;}
}
Status makeCatalogRackCandidate(const SoundDocument& before,const RackCatalogSnapshot& snapshot,juce::Uuid id,std::uint64_t revision,const juce::String& hash,
    const RackOperation& operation,SoundEditOrigin origin,SoundDocument& output,RackDifference& difference,SourceAssignmentTrace* trace){
    try {
        if(trace&&trace->count>trace->effects.size())return Status::invalid;
        auto staged=trace?std::make_unique<SourceAssignmentTrace>(*trace):nullptr;
        if(operation.design||(operation.kind!=RackOperationKind::place&&operation.kind!=RackOperationKind::adopt&&operation.kind!=RackOperationKind::swap))return Status::invalid;
        RackDesign design;if(!resolveCatalogDesign(snapshot,id,revision,hash,design))return Status::invalid;
        const auto entry=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[&](const auto& e){return e.definition.id==id&&e.definition.revision==revision&&e.contentSha256==hash;});
        if(entry==snapshot.entries.end())return Status::invalid;
        auto captured=operation;captured.design=std::move(design);
        const auto status=makeRackCandidateImpl(before,captured,origin,output,difference,rackCatalogNativeEntry(snapshot,std::size_t(entry-snapshot.entries.begin())),staged.get());
        if(status==Status::ready&&trace)*trace=*staged;return status;
    }catch(...){return Status::invalid;}
}
}
