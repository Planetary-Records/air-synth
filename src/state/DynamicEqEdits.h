#pragma once
#include "SoundDocument.h"
#include <cstdint>
#include <span>
#include <vector>

namespace airack
{
struct EqField {
    juce::String key;float normalized=0;
    bool operator==(const EqField&) const=default;
};
struct EqBandPatch {
    std::uint8_t band=0;std::vector<EqField> fields;
    bool operator==(const EqBandPatch&) const=default;
};
struct EqPatch {
    std::vector<EqBandPatch> bands;std::vector<EqField> slotFields;
    bool operator==(const EqPatch&) const=default;
};
// Control-thread values only. Every refusal leaves the destination untouched.
bool captureEqBand(const SoundDocument&,std::uint8_t slot,std::uint8_t band,EqBandPatch&);
bool encodeEqClipboard(std::span<const EqBandPatch>,juce::String&);
bool decodeEqClipboard(const juce::String&,std::vector<EqBandPatch>&);
bool makeEqEdit(const SoundDocument&,std::uint8_t slot,const EqPatch&,SoundDocument&);
bool makeEqPaste(const SoundDocument&,std::uint8_t slot,std::span<const EqBandPatch>,EqPatch&);
}
