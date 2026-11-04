#include "EqComparison.h"

namespace airack::studio
{
void EqComparison::clear() noexcept { bankA.reset(); bankB.reset(); }
bool EqComparison::store(bool useBankB, std::span<const EqBandPatch> bands, std::span<const EqField> fields)
{
    constexpr auto maximum = rackUnitParameterCapacity(NativeModule::dynamicEq);
    if (bands.size()>dynamicEqBandCount || fields.size()>maximum || (bands.empty()&&fields.empty())) return false;
    auto count=fields.size();
    for (const auto& band : bands) {
        if (band.fields.size()>maximum || count>maximum-band.fields.size()) return false;
        count+=band.fields.size();
    }
    EqPatch candidate {{bands.begin(),bands.end()},{fields.begin(),fields.end()}};
    SoundDocument scratch, checked;
    RackEdit add; add.kind=RackEditKind::addEffect; add.effect=EffectKind::dynamicEq;
    if (editRackLayout(scratch.layout,add,scratch.layout)!=RackEditResult::applied || !makeEqEdit(scratch,0,candidate,checked)) return false;
    (useBankB?bankB:bankA)=std::move(candidate);
    return true;
}
std::optional<EqPatch> EqComparison::recall(bool useBankB) const { return useBankB?bankB:bankA; }
}
