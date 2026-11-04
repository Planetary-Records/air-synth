#include "DynamicEqEdits.h"
#include "RackReconciliation.h"
#include "StrictJson.h"
#include "UserReach.h"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <set>

namespace airack
{
namespace
{
constexpr std::size_t maximumBytes=16*1024,maximumBands=8,maximumFields=rackUnitParameterCapacity(NativeModule::dynamicEq);
juce::String bandPrefix(std::uint8_t band){return "dynamic_eq.band."+juce::String(band+1)+".";}
bool validSlot(const SoundDocument& sound,std::uint8_t slot)
{return slot<8&&sound.layout.effects[slot].use==SlotUse::present&&sound.layout.effects[slot].kind==EffectKind::dynamicEq;}
bool validValue(std::size_t index,float value)
{
    if(!std::isfinite(value)||value<0||value>1)return false;
    const auto& descriptor=parameterRegistry()[index];
    if(descriptor.kind==ParameterKind::continuous)return true;
    if(descriptor.choices.size()<2)return false;
    const auto steps=float(descriptor.choices.size()-1);
    return value==std::round(value*steps)/steps;
}
std::size_t bandFieldCount(std::uint8_t band)
{
    const auto prefix="effect.1."+bandPrefix(band);std::size_t count=0;
    for(const auto& descriptor:parameterRegistry())if(descriptor.id.startsWith(prefix))++count;
    return count;
}
bool validBand(const EqBandPatch& band,bool complete)
{
    if(band.band>=maximumBands||band.fields.empty()||band.fields.size()>maximumFields)return false;
    if(complete&&band.fields.size()!=bandFieldCount(band.band))return false;
    const auto prefix=bandPrefix(band.band);std::set<juce::String> keys;
    for(const auto& field:band.fields){
        if(field.key.getNumBytesAsUTF8()>96||!field.key.startsWith(prefix)||!keys.insert(field.key).second)return false;
        const auto index=rackParameterIndex(NativeModule::dynamicEq,0,field.key);
        if(!index||!validValue(*index,field.normalized))return false;
    }
    if(complete){
        auto values=defaultParameterValues();
        for(const auto& field:band.fields)values[*rackParameterIndex(NativeModule::dynamicEq,0,field.key)]=field.normalized;
        if(!validateParameterValues(values))return false;
    }
    return true;
}
bool validBands(std::span<const EqBandPatch> bands)
{
    if(bands.empty()||bands.size()>maximumBands)return false;
    std::bitset<maximumBands> seen;std::size_t fields=0;
    for(const auto& band:bands){
        if(!validBand(band,true)||seen[band.band])return false;
        seen.set(band.band);fields+=band.fields.size();
    }
    return fields<=maximumFields;
}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> properties)
{
    auto* value=new juce::DynamicObject;
    for(const auto& [key,item]:properties)value->setProperty(key,item);
    return juce::var(value);
}
bool members(const juce::var& value,std::initializer_list<const char*> names)
{
    const auto* object=value.getDynamicObject();if(!object||object->getProperties().size()!=int(names.size()))return false;
    for(const auto* name:names)if(!object->hasProperty(name))return false;
    return true;
}
bool integer(const juce::var& value,int maximum,std::uint8_t& out)
{
    if(!value.isInt()&&!value.isInt64())return false;
    const auto number=juce::int64(value);if(number<0||number>maximum)return false;
    out=std::uint8_t(number);return true;
}
}
bool captureEqBand(const SoundDocument& sound,std::uint8_t slot,std::uint8_t band,EqBandPatch& out)
{
    if(!validSlot(sound,slot)||band>=maximumBands||!validateSoundDocument(sound))return false;
    EqBandPatch candidate;candidate.band=band;
    const auto slotPrefix="effect."+juce::String(slot+1)+".",prefix=bandPrefix(band);
    for(const auto index:unitReach(NativeModule::dynamicEq,slot)){
        auto key=parameterRegistry()[index].id.substring(slotPrefix.length());
        if(key.startsWith(prefix))candidate.fields.push_back({std::move(key),sound.values[index]});
    }
    if(!validBand(candidate,true))return false;
    out=std::move(candidate);return true;
}
bool encodeEqClipboard(std::span<const EqBandPatch> bands,juce::String& out)
{
    if(!validBands(bands))return false;
    juce::Array<juce::var> array;
    for(const auto& band:bands){
        juce::Array<juce::var> fields;
        for(const auto& field:band.fields)fields.add(object({{"key",field.key},{"normalized",double(field.normalized)}}));
        array.add(object({{"band",int(band.band)},{"fields",fields}}));
    }
    auto encoded=juce::JSON::toString(object({{"format","airack.eq-bands"},{"version",5},{"bands",array}}),true,17);
    if(encoded.getNumBytesAsUTF8()>maximumBytes)return false;
    out=std::move(encoded);return true;
}
bool decodeEqClipboard(const juce::String& text,std::vector<EqBandPatch>& out)
{
    const auto bytes=text.getNumBytesAsUTF8();
    // Registered keys/format have no NUL. Reject its JSON escape before JUCE
    // string materialization could turn a suffixed token into a known prefix.
    if(bytes==0||bytes>maximumBytes||text.contains("\\u0000"))return false;
    juce::var root;if(!ai::parseStrictJson(std::string_view(text.toRawUTF8(),bytes),root,maximumBytes)
        ||!members(root,{"format","version","bands"})||!root["format"].isString()||root["format"].toString()!="airack.eq-bands")return false;
    std::uint8_t version;if(!integer(root["version"],5,version)||version<1)return false;
    const auto* bands=root["bands"].getArray();if(!bands||bands->isEmpty()||bands->size()>int(maximumBands))return false;
    std::vector<EqBandPatch> decoded;decoded.reserve(std::size_t(bands->size()));
    for(const auto& value:*bands){
        EqBandPatch band;if(!members(value,{"band","fields"})||!integer(value["band"],7,band.band))return false;
        const auto* fields=value["fields"].getArray();if(!fields||fields->isEmpty()||fields->size()>int(maximumFields))return false;
        if(fields->size()!=(version==1?11:version==2?12:version==3?14:version==4?19:22))return false;
        band.fields.reserve(std::size_t(fields->size()));
        for(const auto& item:*fields){
            if(!members(item,{"key","normalized"})||!item["key"].isString())return false;
            const auto key=item["key"].toString();const auto number=item["normalized"];
            if(key.getNumBytesAsUTF8()>96||(!number.isInt()&&!number.isInt64()&&!number.isDouble()))return false;
            const auto normalized=double(number);if(!std::isfinite(normalized)||normalized<0||normalized>1)return false;
            band.fields.push_back({key,float(normalized)});
        }
        // Complete historical bands before the usual ownership/duplicate checks.
        if(version==1)band.fields.push_back({bandPrefix(band.band)+"placement",0});
        if(version<3){
            band.fields.push_back({bandPrefix(band.band)+"shape_extension",0});
            band.fields.push_back({bandPrefix(band.band)+"cut_slope_extension",0});
        }
        if(version<4){
            const char* detectorFields[]{"auto_threshold","adaptive_timing","detector_source","detector_low_hz","detector_high_hz"};
            for(std::size_t i=0;i<5;++i)band.fields.push_back({bandPrefix(band.band)+detectorFields[i],i==4?1.f:0.f});
        }
        if(version<5){
            band.fields.push_back({bandPrefix(band.band)+"spectral_enabled",0});
            band.fields.push_back({bandPrefix(band.band)+"spectral_density",.5f});
            band.fields.push_back({bandPrefix(band.band)+"spectral_tilt_db_oct",.5f});
        }
        decoded.push_back(std::move(band));
    }
    if(!validBands(decoded))return false;
    out=std::move(decoded);return true;
}
bool makeEqEdit(const SoundDocument& sound,std::uint8_t slot,const EqPatch& patch,SoundDocument& out)
{
    if(!validSlot(sound,slot)||sound.effectLocks[slot]||patch.bands.size()>maximumBands
        ||patch.slotFields.size()>maximumFields||!validateSoundDocument(sound))return false;
    std::bitset<maximumBands> bands;std::bitset<parameterCount> fields;std::size_t count=0;auto candidate=sound;
    const auto apply=[&](const EqField& field){
        const auto index=rackParameterIndex(NativeModule::dynamicEq,slot,field.key);
        if(!index||++count>maximumFields||fields[*index]||sound.controlLocks[*index]||!validValue(*index,field.normalized))return false;
        fields.set(*index);candidate.values[*index]=field.normalized;return true;
    };
    for(const auto& band:patch.bands){
        if(!validBand(band,false)||bands[band.band])return false;bands.set(band.band);
        for(const auto& field:band.fields)if(!apply(field))return false;
    }
    for(const auto& field:patch.slotFields){
        if(field.key.getNumBytesAsUTF8()>96||!rackParameterActive(NativeModule::dynamicEq,field.key)
            ||field.key.startsWith("dynamic_eq.band.")||!apply(field))return false;
    }
    SoundDocument reconciled;RackDifference difference;
    if(reconcileRackEdit(sound,candidate,{},SoundEditOrigin::manual,reconciled,difference)!=RackCandidateStatus::ready)return false;
    out=std::move(reconciled);return true;
}
bool makeEqPaste(const SoundDocument& sound,std::uint8_t slot,std::span<const EqBandPatch> copied,EqPatch& out)
{
    if(!validSlot(sound,slot)||!validBands(copied))return false;
    std::array<std::uint8_t,maximumBands> free{};std::size_t freeCount=0;
    for(std::uint8_t band=0;band<maximumBands;++band){
        const auto active=rackParameterIndex(NativeModule::dynamicEq,slot,bandPrefix(band)+"active");
        if(!active)return false;if(sound.values[*active]<.5f)free[freeCount++]=band;
    }
    if(freeCount<copied.size())return false;
    std::array<const EqBandPatch*,maximumBands> sorted{};for(std::size_t n=0;n<copied.size();++n)sorted[n]=&copied[n];
    std::sort(sorted.begin(),sorted.begin()+std::ptrdiff_t(copied.size()),[](auto* a,auto* b){return a->band<b->band;});
    EqPatch candidate;
    for(std::size_t n=0;n<copied.size();++n){
        const auto& source=*sorted[n];EqBandPatch target;target.band=free[n];const auto oldPrefix=bandPrefix(source.band),newPrefix=bandPrefix(target.band);
        target.fields.reserve(source.fields.size());
        for(const auto& field:source.fields)target.fields.push_back({newPrefix+field.key.substring(oldPrefix.length()),field.normalized});
        candidate.bands.push_back(std::move(target));
    }
    SoundDocument checked;if(!makeEqEdit(sound,slot,candidate,checked))return false;
    out=std::move(candidate);return true;
}
}
