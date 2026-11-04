#pragma once

#include "SoundDocument.h"

namespace airack
{
inline constexpr std::size_t legacySoundStateSize = 7024;
inline constexpr std::size_t legacyMaximumSoundStateBytes = 793944;
// Exact Sound5 geometry is independent of the current appended catalog.
inline constexpr std::size_t oldSound5ParameterCount = 759, oldSound5NumericBytes = 7001;
inline constexpr std::size_t oldSound5CompatibilityBytes = 48, oldSound5StateSize = 7089;
inline constexpr std::size_t oldSound5MaximumBytes = 794005;
// Exact accepted Sound6/EQ geometry remains independently decodable.
inline constexpr std::size_t oldSound6ParameterCount = 831, oldSound6NumericBytes = 7649;
inline constexpr std::size_t oldSound6CompatibilityBytes = 56, oldSound6StateSize = 7745;
inline constexpr std::size_t oldSound6MaximumBytes = 794661;
// Exact accepted Sound7/Filter geometry remains independently decodable.
inline constexpr std::size_t oldSound7ParameterCount = 871, oldSound7NumericBytes = 8009;
inline constexpr std::size_t oldSound7CompatibilityBytes = 64, oldSound7StateSize = 8113;
inline constexpr std::size_t oldSound7MaximumBytes = 795029;
// Exact accepted Sound8/Chorus geometry remains independently decodable.
inline constexpr std::size_t oldSound8ParameterCount = 911, oldSound8NumericBytes = 8369;
inline constexpr std::size_t oldSound8CompatibilityBytes = 72, oldSound8StateSize = 8481;
inline constexpr std::size_t oldSound8MaximumBytes = 795397;
// Exact accepted Sound9/Phaser geometry remains independently decodable.
inline constexpr std::size_t oldSound9ParameterCount = 951, oldSound9NumericBytes = 8729;
inline constexpr std::size_t oldSound9CompatibilityBytes = 80, oldSound9StateSize = 8849;
inline constexpr std::size_t oldSound9MaximumBytes = 795765;
// Exact accepted Sound10/Flanger geometry remains independently decodable.
inline constexpr std::size_t oldSound10ParameterCount = 991, oldSound10NumericBytes = 9089;
inline constexpr std::size_t oldSound10CompatibilityBytes = 88, oldSound10StateSize = 9217;
inline constexpr std::size_t oldSound10MaximumBytes = 796133;
// Exact accepted Sound11/Stereo geometry remains independently decodable.
inline constexpr std::size_t oldSound11ParameterCount = 1023, oldSound11NumericBytes = 9377;
inline constexpr std::size_t oldSound11CompatibilityBytes = 96, oldSound11StateSize = 9513;
inline constexpr std::size_t oldSound11MaximumBytes = 796429;
// Exact accepted Sound12/Dimension geometry remains independently decodable.
inline constexpr std::size_t oldSound12ParameterCount = 1055, oldSound12NumericBytes = 9665;
inline constexpr std::size_t oldSound12CompatibilityBytes = 104, oldSound12StateSize = 9809;
inline constexpr std::size_t oldSound12MaximumBytes = 796725;
// Exact accepted Sound13/FrequencyShift geometry remains independently decodable.
inline constexpr std::size_t oldSound13ParameterCount = 1079, oldSound13NumericBytes = 9881;
inline constexpr std::size_t oldSound13CompatibilityBytes = 112, oldSound13StateSize = 10033;
inline constexpr std::size_t oldSound13MaximumBytes = 796949;
// Exact accepted Sound14/Convolution remains independently decodable.
inline constexpr std::size_t oldSound14ParameterCount = 1111, oldSound14NumericBytes = 10169;
inline constexpr std::size_t oldSound14CompatibilityBytes = 120, oldSound14StateSize = 10329;
inline constexpr std::size_t oldSound14MaximumBytes = 797245;
// Exact accepted Sound15/Frequency Split remains independently decodable.
inline constexpr std::size_t oldSound15ParameterCount = 1175, oldSound15NumericBytes = 10745;
inline constexpr std::size_t oldSound15CompatibilityBytes = 128, oldSound15StateSize = 10913;
inline constexpr std::size_t oldSound15MaximumBytes = 797829;
// Exact accepted Sound16/Mid-Side Split remains independently decodable.
inline constexpr std::size_t oldSound16ParameterCount = 1231, oldSound16NumericBytes = 11249;
inline constexpr std::size_t oldSound16CompatibilityBytes = 136, oldSound16StateSize = 11425;
inline constexpr std::size_t oldSound16MaximumBytes = 798341;
// Exact accepted Sound17/Dynamic EQ geometry is independent of later controls.
inline constexpr std::size_t oldSound17ParameterCount = 1943, oldSound17NumericBytes = 17657;
inline constexpr std::size_t oldSound17CompatibilityBytes = 144, oldSound17StateSize = 17841;
inline constexpr std::size_t oldSound17MaximumBytes = 804757;
inline constexpr std::size_t oldSound18ParameterCount = 1959, oldSound18NumericBytes = 17801;
inline constexpr std::size_t oldSound18CompatibilityBytes = 144, oldSound18StateSize = 17985;
inline constexpr std::size_t oldSound18MaximumBytes = 804901;
inline constexpr std::size_t oldSound19ParameterCount = 2023, oldSound19NumericBytes = 18377;
inline constexpr std::size_t oldSound19CompatibilityBytes = 144, oldSound19StateSize = 18561;
inline constexpr std::size_t oldSound19MaximumBytes = 805477;
// Accepted delay format; its catalog remains a frozen prefix.
inline constexpr std::size_t oldSound20ParameterCount = 2063, oldSound20NumericBytes = 18737;
inline constexpr std::size_t oldSound20CompatibilityBytes = 144, oldSound20StateSize = 18921;
inline constexpr std::size_t oldSound20MaximumBytes = 805837;
// The unmerged EQ draft also used Sound20. Its exact count distinguishes the schemas.
inline constexpr std::size_t draftEqSound20ParameterCount = 2151, draftEqSound20NumericBytes = 19529;
inline constexpr std::size_t draftEqSound20StateSize = 19713, draftEqSound20MaximumBytes = 806629;
// Exact accepted Sound21/EQ shapes geometry, before the delay's stereo keys.
inline constexpr std::size_t oldSound21ParameterCount = 2191, oldSound21NumericBytes = 19889;
inline constexpr std::size_t oldSound21CompatibilityBytes = 144, oldSound21StateSize = 20073;
inline constexpr std::size_t oldSound21MaximumBytes = 806989;
inline constexpr std::size_t oldSound22ParameterCount = 2215, oldSound22NumericBytes = 20105;
inline constexpr std::size_t oldSound22CompatibilityBytes = 144, oldSound22StateSize = 20289;
inline constexpr std::size_t oldSound22MaximumBytes = 807205;
// The earlier Pro-R worktree used Sound21 before the accepted main append.
// Count disambiguates it from accepted Sound21; keys retain their identities.
inline constexpr std::size_t proRSound21ParameterCount = 2831, proRSound21NumericBytes = 25649;
inline constexpr std::size_t proRSound21CompatibilityBytes = 152, proRSound21StateSize = 25841;
inline constexpr std::size_t proRSound21MaximumBytes = 812757;
// Accepted main's detector format and the earlier Pro-R format both used Sound23.
// Their exact counts retain separate key and nested-rack domains.
inline constexpr std::size_t oldSound23ParameterCount = 2535, oldSound23NumericBytes = 22985;
inline constexpr std::size_t oldSound23CompatibilityBytes = 144, oldSound23StateSize = 23169;
inline constexpr std::size_t oldSound23MaximumBytes = 810085;
inline constexpr std::size_t proRSound23ParameterCount = 2983, proRSound23NumericBytes = 27017;
inline constexpr std::size_t proRSound23CompatibilityBytes = 152, proRSound23StateSize = 27209;
inline constexpr std::size_t proRSound23MaximumBytes = 814125;
// Sound24 existed as accepted named detector state and unnamed Pro-R evaluation state.
inline constexpr std::size_t oldSound24StateSize = 23173, oldSound24MaximumBytes = 810249;
inline constexpr std::size_t oldSound24ParameterCount = oldSound23ParameterCount, oldSound24NumericBytes = oldSound23NumericBytes;
inline constexpr std::size_t proRSound24StateSize = 30089, proRSound24MaximumBytes = 817005;
// Sound25 existed independently on released spectral main and the named Pro-R branch.
inline constexpr std::size_t spectralSound25ParameterCount = 2735, spectralSound25NumericBytes = 24785;
inline constexpr std::size_t spectralSound25StateSize = 24973, spectralSound25MaximumBytes = 812049;
inline constexpr std::size_t proRSound25ParameterCount = 3303, proRSound25NumericBytes = 29897;
inline constexpr std::size_t proRSound25StateSize = 30093, proRSound25MaximumBytes = 817169;
// Sound26 is Space Reverb's accepted main format, before the wavetable warmth keys.
inline constexpr std::size_t oldSound26ParameterCount = 3503, oldSound26NumericBytes = 31697;
inline constexpr std::size_t oldSound26CompatibilityBytes = 152, oldSound26StateSize = 31893;
inline constexpr std::size_t oldSound26MaximumBytes = 818969;
// Sound27 is the wavetable warmth format, before Era Reverb's append.
inline constexpr std::size_t oldSound27ParameterCount = 3539, oldSound27NumericBytes = 32021;
inline constexpr std::size_t oldSound27CompatibilityBytes = 152, oldSound27StateSize = 32217;
inline constexpr std::size_t oldSound27MaximumBytes = 819293;
inline constexpr std::size_t privateDrumSound27ParameterCount=4083,privateDrumSound27NumericBytes=36917;
inline constexpr std::size_t privateDrumSound27CompatibilityBytes=152,privateDrumSound27StateSize=37113,privateDrumSound27MaximumBytes=824189;
inline constexpr std::size_t oldSound28ParameterCount=3659,oldSound28NumericBytes=33101;
inline constexpr std::size_t oldSound28CompatibilityBytes=160,oldSound28StateSize=33305,oldSound28MaximumBytes=820381;
// Independently published saturation also used Sound20; its keys and lock order differ.
inline constexpr std::size_t oldSaturationSound20ParameterCount = 2303, oldSaturationSound20NumericBytes = 20897;
inline constexpr std::size_t oldSaturationSound20StateSize = 21089, oldSaturationSound20MaximumBytes = 808005;
// Independently published saturation Sound22 has its own count and lock order.
inline constexpr std::size_t oldSaturationSound22ParameterCount = 2471, oldSaturationSound22NumericBytes = 22409;
inline constexpr std::size_t oldSaturationSound22StateSize = 22601, oldSaturationSound22MaximumBytes = 809517;
// The independent saturation branch wrote unnamed Sound24 with a 280-key suffix.
inline constexpr std::size_t oldSaturationSound24ParameterCount=2815, oldSaturationSound24NumericBytes=25505;
inline constexpr std::size_t oldSaturationSound24StateSize=25697, oldSaturationSound24MaximumBytes=812613;
// Released main Sound29 keeps all 4239 control identities and eighteen effect banks.
inline constexpr std::size_t oldSound29ParameterCount=4239, oldSound29NumericBytes=38321;
inline constexpr std::size_t oldSound29CompatibilityBytes=160, oldSound29StateSize=38525, oldSound29MaximumBytes=825601;
// Frozen Sound30 writer geometry; older readers never acquire the new reference budget.
inline constexpr std::size_t oldSound30ParameterCount=4519, oldSound30NumericBytes=40841, oldSound30CompatibilityBytes=168;
inline constexpr std::size_t oldSound30StateSize=41053, oldSound30MaximumBytes=828129;
inline constexpr std::size_t oldPersonalitySectionBytes=786916;
// Presence plus three uint32 byte lengths and 64/32/64 ASCII bytes per reference.
inline constexpr std::size_t maximumRackPackageRecordBytes=1+3*4+64+32+64;
inline constexpr std::size_t maximumPersonalitySectionBytes=oldPersonalitySectionBytes+12*maximumRackPackageRecordBytes;
inline constexpr int currentSoundStateVersion = 31;
inline constexpr std::size_t currentSoundNumericBytes = 40841;
inline constexpr std::size_t kernelCompatibilitySectionBytes = 168;
inline constexpr std::size_t currentPersonalityLengthOffset = currentSoundNumericBytes + 4 + kernelCompatibilitySectionBytes;
inline constexpr std::size_t currentSoundStateSize = currentPersonalityLengthOffset + 4 + 4 + 32;
inline constexpr std::size_t maximumSoundStateBytes = currentSoundStateSize + maximumPersonalitySectionBytes + maximumSoundNameBytes;
// Control-thread operations. Invalid input leaves the destination unchanged.
bool encodeSoundState(const ParameterValues&, juce::MemoryBlock&);
bool decodeSoundState(const void*, std::size_t, ParameterValues&);
}
