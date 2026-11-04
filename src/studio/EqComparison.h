#pragma once
#include "DynamicEqEdits.h"
#include <optional>

namespace airack::studio
{
// Control-owned transient raw values. Never serialized or accessed by DSP.
class EqComparison
{
public:
    void clear() noexcept;
    bool store(bool bankB, std::span<const EqBandPatch>, std::span<const EqField> slotFields);
    std::optional<EqPatch> recall(bool bankB) const;
private:
    std::optional<EqPatch> bankA, bankB;
};
}
