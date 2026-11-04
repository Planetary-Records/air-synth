#include "SoundState.h"
#include "RackExamples.h"
#include "SoundDocument.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace airack
{
namespace
{
constexpr int magic=0x49535241;
constexpr std::size_t v1Size=1720,v2Size=7006;
constexpr std::size_t legacyNumericPayloadBytes=6992;
constexpr std::size_t draftEqMigratedParameterEnd=oldSound20ParameterCount+draftEqSound20ParameterCount-oldSound19ParameterCount;
static_assert(draftEqMigratedParameterEnd==2191);
static_assert(oldSaturationSound20NumericBytes==16+oldSaturationSound20ParameterCount*9+154);
static_assert(oldSaturationSound22NumericBytes==16+oldSaturationSound22ParameterCount*9+154);
static_assert(oldSaturationSound24NumericBytes==16+oldSaturationSound24ParameterCount*9+154);
static_assert(oldSaturationSound24StateSize==oldSaturationSound24NumericBytes+4+152+4+32);
static_assert(oldSound29StateSize==oldSound29NumericBytes+4+160+4+4+32);
static_assert(oldPersonalitySectionBytes==4+12*(16+1+1+18+4+65536));
static_assert(maximumPersonalitySectionBytes==788992);
static_assert(saturationParameterStart==4239 && parameterCount==4519 && dynamicEqSpectralParameterStart==oldSound23ParameterCount && spaceReverbParameterStart==spectralSound25ParameterCount
    && wavetableWarmthParameterStart==oldSound26ParameterCount && eraReverbParameterStart==oldSound27ParameterCount);
static_assert(16+oldSound26ParameterCount*8+78+64+oldSound26ParameterCount+4+8==oldSound26NumericBytes);
static_assert(oldSound26StateSize==oldSound26NumericBytes+4+oldSound26CompatibilityBytes+4+4+32 && oldSound26MaximumBytes==oldSound26StateSize+786916+maximumSoundNameBytes);
static_assert(16+oldSound27ParameterCount*9+78+64+4+8==oldSound27NumericBytes && oldSound27CompatibilityBytes==oldSound26CompatibilityBytes);
static_assert(oldSound27StateSize==oldSound27NumericBytes+4+oldSound27CompatibilityBytes+4+4+32 && oldSound27MaximumBytes==oldSound27StateSize+786916+maximumSoundNameBytes);
static_assert(drumParameterStart-oldSound26ParameterCount==156);
static_assert(drumModeParameterStart+4-drumParameterStart==privateDrumSound27ParameterCount-oldSound26ParameterCount);
static_assert(privateDrumSound27NumericBytes==16+privateDrumSound27ParameterCount*9+78+64+12);
static_assert(privateDrumSound27StateSize==privateDrumSound27NumericBytes+4+152+4+4+32);
static_assert(oldSound28StateSize==oldSound28NumericBytes+4+160+4+4+32);
static_assert(16+oldSound22ParameterCount*9+78+64+4+8==oldSound22NumericBytes);
static_assert(16+oldSound23ParameterCount*9+78+64+4+8==oldSound23NumericBytes);
static_assert(16+proRSound23ParameterCount*9+78+64+4+8==proRSound23NumericBytes);
static_assert(oldSound23StateSize==oldSound23NumericBytes+4+oldSound23CompatibilityBytes+4+32);
static_assert(proRSound23StateSize==proRSound23NumericBytes+4+proRSound23CompatibilityBytes+4+32);
static_assert(maximumSoundStateBytes==830205 && currentSoundStateSize==41053);
static_assert(proRSound21ParameterCount==oldSound20ParameterCount+8*spaceReverbParametersPerSlot);
static_assert(16+proRSound21ParameterCount*9+78+64+4+8==proRSound21NumericBytes);
static_assert(16+parameterCount*8+78+64+parameterCount+4+8==currentSoundNumericBytes);
static_assert(oldSound21ParameterCount==draftEqMigratedParameterEnd && oldSound21MaximumBytes==806989 && oldSound21StateSize==20073);
static_assert(16+oldSound21ParameterCount*8+78+64+oldSound21ParameterCount+4+8==oldSound21NumericBytes);
static_assert(16+oldSound20ParameterCount*8+78+64+oldSound20ParameterCount+4+8==oldSound20NumericBytes);
static_assert(oldSound19ParameterCount==2023 && oldSound19NumericBytes==18377 && oldSound19StateSize==18561 && oldSound19MaximumBytes==805477);
static_assert(oldSound18ParameterCount==1959 && oldSound18NumericBytes==17801 && oldSound18StateSize==17985 && oldSound18MaximumBytes==804901);
static_assert(16+oldSound18ParameterCount*8+78+64+oldSound18ParameterCount+4+8==oldSound18NumericBytes);
static_assert(oldSound17MaximumBytes==804757 && oldSound17StateSize==17841);
static_assert(16+oldSound17ParameterCount*8+78+64+oldSound17ParameterCount+4+8==oldSound17NumericBytes);
static_assert(oldSound5MaximumBytes==794005 && oldSound5StateSize==7089);
static_assert(oldSound6MaximumBytes==794661 && oldSound6StateSize==7745);
static_assert(oldSound7MaximumBytes==795029 && oldSound7StateSize==8113);
static_assert(oldSound8MaximumBytes==795397 && oldSound8StateSize==8481);
static_assert(oldSound9MaximumBytes==795765 && oldSound9StateSize==8849);
static_assert(oldSound10MaximumBytes==796133 && oldSound10StateSize==9217);
static_assert(16+oldSound10ParameterCount*8+78+64+oldSound10ParameterCount+4+8==oldSound10NumericBytes);
static_assert(16+oldSound9ParameterCount*8+78+64+oldSound9ParameterCount+4+8==oldSound9NumericBytes);
static_assert(16+oldSound8ParameterCount*8+78+64+oldSound8ParameterCount+4+8==oldSound8NumericBytes);
static_assert(16+oldSound7ParameterCount*8+78+64+oldSound7ParameterCount+4+8==oldSound7NumericBytes);
static_assert(16+oldSound6ParameterCount*8+78+64+oldSound6ParameterCount+4+8==oldSound6NumericBytes);
static_assert(16+oldSound5ParameterCount*8+78+64+oldSound5ParameterCount+4+8==oldSound5NumericBytes);
static_assert(oldSound11StateSize==9513 && oldSound11MaximumBytes==796429);
static_assert(16+oldSound11ParameterCount*8+78+64+oldSound11ParameterCount+4+8==oldSound11NumericBytes);
static_assert(oldSound12StateSize==9809 && oldSound12MaximumBytes==796725);
static_assert(16+oldSound12ParameterCount*8+78+64+oldSound12ParameterCount+4+8==oldSound12NumericBytes);
static_assert(oldSound13StateSize==10033 && oldSound13MaximumBytes==796949);
static_assert(16+oldSound13ParameterCount*8+78+64+oldSound13ParameterCount+4+8==oldSound13NumericBytes);
static_assert(oldSound14StateSize==10329 && oldSound14MaximumBytes==797245);
static_assert(16+oldSound14ParameterCount*8+78+64+oldSound14ParameterCount+4+8==oldSound14NumericBytes);
static_assert(oldSound15StateSize==10913 && oldSound15MaximumBytes==797829);
static_assert(16+oldSound15ParameterCount*8+78+64+oldSound15ParameterCount+4+8==oldSound15NumericBytes);
static_assert(oldSound16StateSize==11425 && oldSound16MaximumBytes==798341);
static_assert(16+oldSound16ParameterCount*8+78+64+oldSound16ParameterCount+4+8==oldSound16NumericBytes);
static_assert(effectBankCount==19 && kernelCompatibilitySectionBytes==8+4*2+8*effectBankCount && oldSound26CompatibilityBytes==8+4*2+8*17);
static_assert(16+parameterCount*8+78+64+parameterCount+4+8==currentSoundNumericBytes);
static_assert(16+756*8+78+64+756+4+8+32==v2Size && sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
void allSynthsPresent(SoundDocument& d) noexcept
{
    d.layout.synthUse.fill(SlotUse::present);
    d.layout.synthOrder={0,1,2,3};d.layout.synthCount=4;
}
void writeLayout(juce::MemoryOutputStream& s,const RackLayout& l)
{
    for(auto x:l.synthUse)s.writeByte(static_cast<char>(x));
    for(auto x:l.synthOrder)s.writeByte(static_cast<char>(x));
    s.writeByte(static_cast<char>(l.synthCount));
    for(const auto& e:l.effects){s.writeByte(static_cast<char>(e.use));s.writeByte(static_cast<char>(e.kind));s.writeByte(static_cast<char>(e.chain));}
    for(const auto& order:l.effectOrder)for(auto x:order)s.writeByte(static_cast<char>(x));
    for(auto x:l.effectCount)s.writeByte(static_cast<char>(x));
}
void readLayout(juce::MemoryInputStream& s,RackLayout& l)
{
    for(auto& x:l.synthUse)x=static_cast<SlotUse>(s.readByte());
    for(auto& x:l.synthOrder)x=static_cast<std::uint8_t>(s.readByte());
    l.synthCount=static_cast<std::uint8_t>(s.readByte());
    for(auto& e:l.effects){e.use=static_cast<SlotUse>(s.readByte());e.kind=static_cast<EffectKind>(s.readByte());e.chain=static_cast<std::uint8_t>(s.readByte());}
    for(auto& order:l.effectOrder)for(auto& x:order)x=static_cast<std::uint8_t>(s.readByte());
    for(auto& x:l.effectCount)x=static_cast<std::uint8_t>(s.readByte());
}
template<std::size_t N>
bool readLocks(juce::MemoryInputStream& s,std::array<bool,N>& locks,std::size_t count=N)
{
    for(std::size_t i=0;i<count;++i){auto& x=locks[i];const auto b=static_cast<unsigned char>(s.readByte());if(b>1)return false;x=b==1;}
    return true;
}
bool writePersonalities(const RackPersonalityState& state,juce::MemoryBlock& output)
{
    if(state.instances.empty()||state.instances.size()>12)return false;
    juce::MemoryOutputStream s(output,false);s.writeShort(2);s.writeByte(static_cast<char>(state.instances.size()));s.writeByte(0);
    for(const auto& i:state.instances){
        juce::MemoryBlock design;if(!encodeRackDesign(i.design,design,RackValidationMode::instanceRecall))return false;
        if(i.bindings.empty()||i.bindings.size()>9)return false;
        s.write(i.id.getRawData(),16);s.writeByte(i.edited?1:0);s.writeByte(static_cast<char>(i.bindings.size()));
        for(const auto& b:i.bindings){s.writeByte(static_cast<char>(b.unit));s.writeByte(static_cast<char>(b.slot));}
        s.writeInt(static_cast<int>(design.getSize()));s.write(design.getData(),design.getSize());
        s.writeByte(i.package?1:0);
        if(i.package){
            if(!validRackPackageRef(*i.package))return false;
            for(const auto* text:{&i.package->id,&i.package->version,&i.package->sha256}){
                s.writeInt(int(text->getNumBytesAsUTF8()));s.write(text->toRawUTF8(),text->getNumBytesAsUTF8());
            }
        }
    }
    return output.getSize()<=maximumPersonalitySectionBytes;
}
bool oldParameterAllowed(std::size_t index,std::size_t limit,bool draftEq,bool proR=false,bool proR23=false,bool proRFull=false,bool privateDrum27=false,std::size_t saturationPrefix=0) noexcept
{ if(saturationPrefix)return index<saturationPrefix||(index>=saturationParameterStart&&index<saturationParameterStart+8*saturationParametersPerSlot);
  if(privateDrum27)return index < oldSound26ParameterCount || (index >= drumParameterStart && index < drumModeParameterStart+4);
  if(proR||proR23||proRFull)return index<(proR?oldSound20ParameterCount:proR23?oldSound22ParameterCount:oldSound23ParameterCount)||(index>=spaceReverbParameterStart&&index<wavetableWarmthParameterStart);
  return index<limit&&(!draftEq||index<oldSound19ParameterCount
    ||(index>=oldSound20ParameterCount&&index<draftEqMigratedParameterEnd)); }
bool oldDesignDomain(const RackDesign& design,int soundVersion,bool draftEq,bool proR,bool proR23,bool proRFull,bool privateDrum27,std::size_t saturationPrefix)
{
    if(soundVersion==30){
        for(const auto& unit:design.units){
            const auto cap=isSourceModule(unit.module)?293u:unit.module==NativeModule::spaceReverb?97u:unit.module==NativeModule::dynamicEq?181u:unit.module==NativeModule::eraReverb?16u:unit.module==NativeModule::saturation?36u:82u;
            if(unit.module>NativeModule::saturation||unit.defaults.size()>cap)return false;
            for(const auto& value:unit.defaults){const auto p=rackParameterIndex(unit.module,0,value.key);if(!p||*p>=oldSound30ParameterCount)return false;}
        }
        for(const auto& control:design.panel.controls){
            const auto unit=std::find_if(design.units.begin(),design.units.end(),[&](const auto& u){return u.id==control.unit;});
            if(unit==design.units.end())return false;const auto p=rackParameterIndex(unit->module,0,control.key);if(!p||*p>=oldSound30ParameterCount)return false;
        }
        return true;
    }
    // An older sound's signed nested Rack2 data retains its original parameter
    // and module domain even when the current portable codec knows more fields.
    const auto lastModule=saturationPrefix?NativeModule::saturation:soundVersion==29?NativeModule::drum:soundVersion==28?NativeModule::eraReverb:(proR||proR23||proRFull||soundVersion==26||soundVersion==27)?NativeModule::spaceReverb:soundVersion>=17?NativeModule::dynamicEq:soundVersion==16?NativeModule::midSideSplit:soundVersion==15?NativeModule::frequencySplit:soundVersion==14?NativeModule::convolution:soundVersion==13?NativeModule::frequencyShift:soundVersion==12?NativeModule::dimension:soundVersion==11?NativeModule::stereo:soundVersion==10?NativeModule::flanger:soundVersion==9?NativeModule::phaser:soundVersion==8?NativeModule::chorus:soundVersion==7?NativeModule::filter:soundVersion==6?NativeModule::eq:NativeModule::compressor;
    const auto parameterLimit=saturationPrefix?parameterCount:soundVersion==29?oldSound29ParameterCount:privateDrum27?parameterCount:soundVersion==28?oldSound28ParameterCount:(proR||proR23||proRFull)?parameterCount:soundVersion==27?oldSound27ParameterCount:soundVersion==26?oldSound26ParameterCount:soundVersion==25?spectralSound25ParameterCount:soundVersion>=23?oldSound23ParameterCount:soundVersion==22?oldSound22ParameterCount:soundVersion==21?oldSound21ParameterCount:soundVersion==20?(draftEq?draftEqMigratedParameterEnd:oldSound20ParameterCount):soundVersion==19?oldSound19ParameterCount:soundVersion==18?oldSound18ParameterCount:soundVersion==17?oldSound17ParameterCount:soundVersion==16?oldSound16ParameterCount:soundVersion==15?oldSound15ParameterCount:soundVersion==14?oldSound14ParameterCount:soundVersion==13?oldSound13ParameterCount:soundVersion==12?oldSound12ParameterCount:soundVersion==11?oldSound11ParameterCount:soundVersion==10?oldSound10ParameterCount:soundVersion==9?oldSound9ParameterCount:soundVersion==8?oldSound8ParameterCount:soundVersion==7?oldSound7ParameterCount:soundVersion==6?oldSound6ParameterCount:oldSound5ParameterCount;
    const auto defaultLimit=saturationPrefix?std::size_t(saturationPrefix==oldSound19ParameterCount?100:saturationPrefix==oldSound21ParameterCount?116:156):soundVersion==29?std::size_t(293):proR?std::size_t(100):proR23?std::size_t(116):soundVersion>=25&&!proRFull?std::size_t(181):soundVersion>=23?std::size_t(156):soundVersion>=21?std::size_t(116):soundVersion==20?std::size_t(draftEq?116:105):soundVersion==19?std::size_t(100):soundVersion==18?std::size_t(92):soundVersion==17?std::size_t(90):soundVersion==16?std::size_t(82):soundVersion==15?std::size_t(75):soundVersion==14?std::size_t(67):soundVersion==13?std::size_t(63):soundVersion==12?std::size_t(60):soundVersion==11?std::size_t(56):soundVersion==10?std::size_t(52):soundVersion==9?std::size_t(47):soundVersion==8?std::size_t(42):soundVersion==7?std::size_t(37):soundVersion==6?std::size_t(32):std::size_t(23);
    for(const auto& unit:design.units){
        if(unit.module<NativeModule::drive&&(unit.defaults.size()>(soundVersion==29?std::size_t(293):privateDrum27?std::size_t(140):soundVersion>=27?std::size_t(149):std::size_t(140))||(unit.algorithmRevision!=1&&unit.algorithmRevision!=2)))return false;
        if((saturationPrefix&&unit.module>NativeModule::dynamicEq&&unit.module!=NativeModule::saturation)||unit.module>lastModule||(unit.module>=NativeModule::drive&&unit.defaults.size()>defaultLimit))return false;
        for(const auto& value:unit.defaults){const auto index=rackParameterIndex(unit.module,0,value.key);if(!index||!oldParameterAllowed(*index,parameterLimit,draftEq,proR,proR23,proRFull,privateDrum27,saturationPrefix))return false;}
    }
    for(const auto& control:design.panel.controls){
        const auto unit=std::find_if(design.units.begin(),design.units.end(),[&](const auto& candidate){return candidate.id==control.unit;});
        if(unit==design.units.end())return false;
        const auto index=rackParameterIndex(unit->module,0,control.key);if(!index||!oldParameterAllowed(*index,parameterLimit,draftEq,proR,proR23,proRFull,privateDrum27,saturationPrefix))return false;
    }
    return true;
}
bool readPersonalities(juce::MemoryInputStream& s,std::size_t payload,SoundDocument& document,int soundVersion,bool optional=false,bool draftEq=false,bool proR=false,bool proR23=false,bool proR24=false,bool privateDrum27=false,std::size_t saturationPrefix=0)
{
    const auto start=static_cast<std::size_t>(s.getPosition());
    if(start>payload||payload-start<4)return false;
    const auto sectionBytes=static_cast<std::uint32_t>(s.readInt());
    if(optional&&sectionBytes==0)return static_cast<std::size_t>(s.getPosition())==payload;
    if(sectionBytes<4||sectionBytes>(soundVersion>=31?maximumPersonalitySectionBytes:oldPersonalitySectionBytes)||sectionBytes!=payload-start-4)return false;
    if(s.readShort()!=(soundVersion>=31?2:1))return false;
    const auto count=static_cast<unsigned char>(s.readByte());
    if(count<1||count>12||s.readByte()!=0)return false;
    auto state=std::make_shared<RackPersonalityState>();
    for(unsigned n=0;n<count;++n){
        // Bound each fixed header and variable span before reading, allocating,
        // or forming a nested pointer. Counts never size unchecked storage.
        if(payload-static_cast<std::size_t>(s.getPosition())<18)return false;
        std::uint8_t id[16];
        s.read(id,16);
        RackInstance i;
        i.id=juce::Uuid(id);
        const auto edited=static_cast<unsigned char>(s.readByte());
        const auto bindings=static_cast<unsigned char>(s.readByte());
        if(edited>1||bindings<1||bindings>9||payload-static_cast<std::size_t>(s.getPosition())<2*bindings+4)return false;
        i.edited=edited==1;
        for(unsigned b=0;b<bindings;++b){
            const auto local=static_cast<std::uint8_t>(s.readByte());
            const auto slot=static_cast<std::uint8_t>(s.readByte());
            i.bindings.push_back({local,slot});
        }
        const auto designBytes=static_cast<std::uint32_t>(s.readInt());
        if(designBytes<1||designBytes>65536||designBytes>payload-static_cast<std::size_t>(s.getPosition()))return false;
        const auto* design=static_cast<const char*>(s.getData())+s.getPosition();
        if(!decodeRackDesign(design,designBytes,i.design,RackValidationMode::instanceRecall))return false;
        if(soundVersion<currentSoundStateVersion&&!oldDesignDomain(i.design,soundVersion,draftEq,proR,proR23,proR24,privateDrum27,saturationPrefix))return false;
        s.skipNextBytes(designBytes);
        if(soundVersion>=31){
            if(std::size_t(s.getPosition())>=payload)return false;
            const auto present=static_cast<unsigned char>(s.readByte());if(present>1)return false;
            if(present){
                RackPackageRef ref;
                const auto read=[&](juce::String& text,std::size_t maximum){
                    const auto at=std::size_t(s.getPosition());if(at>payload||payload-at<4)return false;
                    const auto bytes=std::uint32_t(s.readInt());if(bytes>maximum||bytes>payload-at-4)return false;
                    const auto* utf8=static_cast<const char*>(s.getData())+at+4;
                    if(!juce::CharPointer_UTF8::isValidString(utf8,int(bytes)))return false;
                    text=juce::String::fromUTF8(utf8,int(bytes));if(text.getNumBytesAsUTF8()!=bytes)return false;s.skipNextBytes(bytes);return true;
                };
                if(!read(ref.id,64)||!read(ref.version,32)||!read(ref.sha256,64)||!validRackPackageRef(ref))return false;
                i.package=std::move(ref);
            }
        }
        state->instances.push_back(std::move(i));
    }
    if(s.getPosition()!=static_cast<juce::int64>(payload))return false;
    document.personalities=std::move(state);
    return true;
}
bool readName(juce::MemoryInputStream& s,std::size_t payload,SoundDocument& document)
{
    const auto at=static_cast<std::size_t>(s.getPosition());
    if(at>payload||payload-at<4)return false;
    const auto bytes=static_cast<std::uint32_t>(s.readInt());
    if(bytes>maximumSoundNameBytes||bytes!=payload-at-4)return false;
    const auto* text=static_cast<const char*>(s.getData())+at+4;
    if(!juce::CharPointer_UTF8::isValidString(text,static_cast<int>(bytes)))return false;
    auto name=juce::String::fromUTF8(text,static_cast<int>(bytes));
    // A NUL would shorten the name; only the exact bytes round-trip.
    if(name.getNumBytesAsUTF8()!=bytes)return false;
    document.name=std::move(name);s.skipNextBytes(bytes);return true;
}
}

bool encodeSoundState(const SoundDocument& document,juce::MemoryBlock& output)
{
    try {
    const auto registry=parameterRegistry();
    if(!validateSoundDocument(document))return false;
    const bool personalities=document.personalities&&!document.personalities->instances.empty();
    juce::MemoryBlock section;if(personalities&&!writePersonalities(*document.personalities,section))return false;
    juce::MemoryBlock encoded;
    {
        juce::MemoryOutputStream s(encoded,false);
        s.writeInt(magic);s.writeInt(currentSoundStateVersion);s.writeInt(static_cast<int>(parameterCount));s.writeInt(0);
        for(std::size_t i=0;i<parameterCount;++i){s.writeInt(std::bit_cast<std::int32_t>(registry[i].stateKey));s.writeFloat(document.values[i]);}
        writeLayout(s,document.layout);
        for(const auto& b:document.macros){s.writeInt(std::bit_cast<std::int32_t>(b.targetKey));s.writeFloat(b.amount);}
        for(bool b:document.controlLocks)s.writeByte(b?1:0);
        for(bool b:document.synthLocks)s.writeByte(b?1:0);
        for(bool b:document.effectLocks)s.writeByte(b?1:0);
        s.writeInt(static_cast<int>(kernelCompatibilitySectionBytes));
        s.writeShort(1);s.writeByte(4);s.writeByte(2);s.writeByte(8);s.writeByte(static_cast<char>(effectBankCount));s.writeShort(0);
        for(const auto& banks:document.compatibility.synth)for(auto revision:banks)s.writeByte(static_cast<char>(revision));
        for(const auto& banks:document.compatibility.effects)for(auto revision:banks)s.writeByte(static_cast<char>(revision));
        s.writeInt(static_cast<int>(section.getSize()));
        if(personalities)s.write(section.getData(),section.getSize());
        const auto name=document.name.toRawUTF8();const auto nameBytes=document.name.getNumBytesAsUTF8();
        s.writeInt(static_cast<int>(nameBytes));s.write(name,nameBytes);
        s.flush();const auto digest=juce::SHA256(encoded).getRawData();s.write(digest.getData(),digest.getSize());
    }
    output.swapWith(encoded);return true;
    }catch(...){return false;}
}
bool decodeSoundState(const void* data,std::size_t size,SoundDocument& output)
{
    try {
    if(data==nullptr||size<16+32||size>maximumSoundStateBytes)return false;
    const auto payload=size-32;
    const auto digest=juce::SHA256(data,payload).getRawData();
    if(std::memcmp(digest.getData(),static_cast<const char*>(data)+payload,32)!=0)return false;
    juce::MemoryInputStream s(data,payload,false);
    if(s.readInt()!=magic)return false;
    const auto version=s.readInt(),count=s.readInt();
    const std::size_t saturationPrefix=version==20&&count==int(oldSaturationSound20ParameterCount)?oldSound19ParameterCount
        :version==22&&count==int(oldSaturationSound22ParameterCount)?oldSound21ParameterCount
        :version==24&&count==int(oldSaturationSound24ParameterCount)?oldSound23ParameterCount:0;
    const auto saturationNumericBytes=saturationPrefix==oldSound19ParameterCount?oldSaturationSound20NumericBytes:saturationPrefix==oldSound21ParameterCount?oldSaturationSound22NumericBytes:oldSaturationSound24NumericBytes;
    const auto saturationMinimumBytes=saturationPrefix==oldSound19ParameterCount?oldSaturationSound20StateSize:saturationPrefix==oldSound21ParameterCount?oldSaturationSound22StateSize:oldSaturationSound24StateSize;
    const auto saturationMaximumBytes=saturationPrefix==oldSound19ParameterCount?oldSaturationSound20MaximumBytes:saturationPrefix==oldSound21ParameterCount?oldSaturationSound22MaximumBytes:oldSaturationSound24MaximumBytes;
    const bool privateDrum27=version==27&&count==static_cast<int>(privateDrumSound27ParameterCount);
    const bool draftEq=version==20&&count==static_cast<int>(draftEqSound20ParameterCount);
    const bool proR=version==21&&count==static_cast<int>(proRSound21ParameterCount);
    const bool proR23=version==23&&count==static_cast<int>(proRSound23ParameterCount);
    const bool proR24=version==24&&count==static_cast<int>(proRSound25ParameterCount);
    const bool proR25=version==25&&count==static_cast<int>(proRSound25ParameterCount);
    const bool spectral25=version==25&&count==static_cast<int>(spectralSound25ParameterCount);
    const bool proRFull=proR24||proR25;
    const bool namedDetector24=version==24&&count==static_cast<int>(oldSound23ParameterCount);
    const bool hasName=version>=25||namedDetector24;
    if(s.readInt()!=0 || !((version==1&&size==v1Size&&count==209)||(version==2&&size==v2Size&&count==756)
        ||(version==3&&size==legacySoundStateSize&&count==758)
        ||(version==4&&size>=legacySoundStateSize+8&&size<=legacyMaximumSoundStateBytes&&count==758)
        ||(version==5&&size>=oldSound5StateSize&&size<=oldSound5MaximumBytes&&count==static_cast<int>(oldSound5ParameterCount))
        ||(version==6&&size>=oldSound6StateSize&&size<=oldSound6MaximumBytes&&count==static_cast<int>(oldSound6ParameterCount))
        ||(version==7&&size>=oldSound7StateSize&&size<=oldSound7MaximumBytes&&count==static_cast<int>(oldSound7ParameterCount))
        ||(version==8&&size>=oldSound8StateSize&&size<=oldSound8MaximumBytes&&count==static_cast<int>(oldSound8ParameterCount))
        ||(version==9&&size>=oldSound9StateSize&&size<=oldSound9MaximumBytes&&count==static_cast<int>(oldSound9ParameterCount))
        ||(version==10&&size>=oldSound10StateSize&&size<=oldSound10MaximumBytes&&count==static_cast<int>(oldSound10ParameterCount))
        ||(version==11&&size>=oldSound11StateSize&&size<=oldSound11MaximumBytes&&count==static_cast<int>(oldSound11ParameterCount))
        ||(version==12&&size>=oldSound12StateSize&&size<=oldSound12MaximumBytes&&count==static_cast<int>(oldSound12ParameterCount))
        ||(version==13&&size>=oldSound13StateSize&&size<=oldSound13MaximumBytes&&count==static_cast<int>(oldSound13ParameterCount))
        ||(version==14&&size>=oldSound14StateSize&&size<=oldSound14MaximumBytes&&count==static_cast<int>(oldSound14ParameterCount))
        ||(version==15&&size>=oldSound15StateSize&&size<=oldSound15MaximumBytes&&count==static_cast<int>(oldSound15ParameterCount))
        ||(version==16&&size>=oldSound16StateSize&&size<=oldSound16MaximumBytes&&count==static_cast<int>(oldSound16ParameterCount))
        ||(version==17&&size>=oldSound17StateSize&&size<=oldSound17MaximumBytes&&count==static_cast<int>(oldSound17ParameterCount))
        ||(version==18&&size>=oldSound18StateSize&&size<=oldSound18MaximumBytes&&count==static_cast<int>(oldSound18ParameterCount))
        ||(version==19&&size>=oldSound19StateSize&&size<=oldSound19MaximumBytes&&count==static_cast<int>(oldSound19ParameterCount))
        ||(version==20&&count==static_cast<int>(oldSound20ParameterCount)&&size>=oldSound20StateSize&&size<=oldSound20MaximumBytes)
        ||(saturationPrefix&&size>=saturationMinimumBytes&&size<=saturationMaximumBytes)
        ||(draftEq&&size>=draftEqSound20StateSize&&size<=draftEqSound20MaximumBytes)
        ||(version==21&&size>=oldSound21StateSize&&size<=oldSound21MaximumBytes&&count==static_cast<int>(oldSound21ParameterCount))
        ||(proR&&size>=proRSound21StateSize&&size<=proRSound21MaximumBytes)
        ||(version==22&&size>=oldSound22StateSize&&size<=oldSound22MaximumBytes&&count==static_cast<int>(oldSound22ParameterCount))
        ||(version==23&&size>=oldSound23StateSize&&size<=oldSound23MaximumBytes&&count==static_cast<int>(oldSound23ParameterCount))
        ||(proR23&&size>=proRSound23StateSize&&size<=proRSound23MaximumBytes)
        ||(namedDetector24&&size>=oldSound24StateSize&&size<=oldSound24MaximumBytes)
        ||(proR24&&size>=proRSound24StateSize&&size<=proRSound24MaximumBytes)
        ||(proR25&&size>=proRSound25StateSize&&size<=proRSound25MaximumBytes)
        ||(spectral25&&size>=spectralSound25StateSize&&size<=spectralSound25MaximumBytes)
        ||(version==26&&size>=oldSound26StateSize&&size<=oldSound26MaximumBytes&&count==static_cast<int>(oldSound26ParameterCount))
        ||(version==27&&size>=oldSound27StateSize&&size<=oldSound27MaximumBytes&&count==static_cast<int>(oldSound27ParameterCount))
        ||(privateDrum27&&size>=privateDrumSound27StateSize&&size<=privateDrumSound27MaximumBytes)
        ||(version==28&&count==static_cast<int>(oldSound28ParameterCount)&&size>=oldSound28StateSize&&size<=oldSound28MaximumBytes)
        ||(version==29&&count==int(oldSound29ParameterCount)&&size>=oldSound29StateSize&&size<=oldSound29MaximumBytes)
        ||(version==30&&size>=oldSound30StateSize&&size<=oldSound30MaximumBytes&&count==int(oldSound30ParameterCount))
        ||(version==currentSoundStateVersion&&size>=currentSoundStateSize&&count==static_cast<int>(parameterCount))))return false;
    const auto registry=parameterRegistry(); // Control-thread initialization before numeric key lookup.
    // Current records contain every unique key. Only historical formats need
    // defaults for parameters appended after their saved catalog.
    SoundDocument document{SoundAudioState{.values={}}};
    if(version!=currentSoundStateVersion)document.values=defaultParameterValues();
    // Legacy sounds retain their original gain, independently of modern defaults.
    if(version<3){document.values[756]=.4f;document.values[757]=0;}
    const auto allowedParameters=saturationPrefix?parameterCount:version==30?oldSound30ParameterCount:version==29?oldSound29ParameterCount:version==1?std::size_t(212):version==2?std::size_t(756):version<5?std::size_t(758):version==5?oldSound5ParameterCount:version==6?oldSound6ParameterCount:version==7?oldSound7ParameterCount:version==8?oldSound8ParameterCount:version==9?oldSound9ParameterCount:version==10?oldSound10ParameterCount:version==11?oldSound11ParameterCount:version==12?oldSound12ParameterCount:version==13?oldSound13ParameterCount:version==14?oldSound14ParameterCount:version==15?oldSound15ParameterCount:version==16?oldSound16ParameterCount:version==17?oldSound17ParameterCount:version==18?oldSound18ParameterCount:version==19?oldSound19ParameterCount:version==20?(draftEq?draftEqMigratedParameterEnd:oldSound20ParameterCount):proR?parameterCount:version==21?oldSound21ParameterCount:version==22?oldSound22ParameterCount:version==23?(proR23?parameterCount:oldSound23ParameterCount):namedDetector24?oldSound23ParameterCount:spectral25?spectralSound25ParameterCount:version==26?oldSound26ParameterCount:privateDrum27?parameterCount:version==27?oldSound27ParameterCount:version==28?oldSound28ParameterCount:parameterCount;
    std::array<bool,parameterCount> seen {};
    for(int record=0;record<count;++record) {
        const auto key=static_cast<std::uint32_t>(s.readInt());
        // Our encoder writes registry order; reordered/historical keys retain
        // the same lookup, duplicate and domain validation below.
        const auto ordinal=static_cast<std::size_t>(record);
        const auto i=ordinal<registry.size()&&registry[ordinal].stateKey==key
            ?std::optional<std::size_t>(ordinal):parameterIndexForKey(key);
        if(!i||seen[*i]||!oldParameterAllowed(*i,allowedParameters,draftEq,proR,proR23,proRFull,privateDrum27,saturationPrefix)||(version==1&&*i>=9&&*i<12))return false;
        seen[*i]=true;document.values[*i]=s.readFloat();
    }
    if(version==1)allSynthsPresent(document);
    else {
        readLayout(s,document.layout);
        if(saturationPrefix){for(auto& effect:document.layout.effects){if(unsigned(effect.kind)>16)return false;if(unsigned(effect.kind)==16)effect.kind=EffectKind::saturation;}}
        if(!saturationPrefix&&version<30)for(const auto& effect:document.layout.effects)if(effect.kind>EffectKind::eraReverb)return false;
        if(!saturationPrefix&&version<26&&!proR&&!proR23&&!proRFull)for(const auto& effect:document.layout.effects)if(effect.kind>EffectKind::dynamicEq)return false;
        if(!saturationPrefix&&version<28)for(const auto& effect:document.layout.effects)if(effect.kind>EffectKind::spaceReverb)return false;
        if(version<17)for(const auto& effect:document.layout.effects)if(effect.kind>=(version==16?EffectKind::dynamicEq:version==15?EffectKind::midSideSplit:version==14?EffectKind::frequencySplit:version==13?EffectKind::convolution:version==12?EffectKind::frequencyShift:version==11?EffectKind::dimension:version==10?EffectKind::stereo:version==9?EffectKind::flanger:version==8?EffectKind::phaser:version==7?EffectKind::chorus:version==6?EffectKind::filter:EffectKind::eq))return false;
        for(auto& b:document.macros){
            b.targetKey=static_cast<std::uint32_t>(s.readInt());b.amount=s.readFloat();
            const auto target=parameterIndexForKey(b.targetKey);
            if(target&&!oldParameterAllowed(*target,allowedParameters,draftEq,proR,proR23,proRFull,privateDrum27,saturationPrefix))return false;
        }
        if(saturationPrefix){
            for(std::size_t saved=0;saved<std::size_t(count);++saved){
                const auto lock=static_cast<unsigned char>(s.readByte());if(lock>1)return false;
                const auto target=saved<saturationPrefix?saved:saturationParameterStart+saved-saturationPrefix;
                document.controlLocks[target]=lock==1;
            }
        }else if(proR||proR23||proRFull){
            const auto prefix=proR?oldSound20ParameterCount:proR23?oldSound22ParameterCount:oldSound23ParameterCount;
            const auto records=proR?proRSound21ParameterCount:proR23?proRSound23ParameterCount:proRSound25ParameterCount;
            for(std::size_t i=0;i<records;++i){
                const auto value=static_cast<unsigned char>(s.readByte());if(value>1)return false;
                const auto target=i<prefix?i:spaceReverbParameterStart+i-prefix;
                document.controlLocks[target]=value==1;
            }
        }else if(draftEq){
            for(std::size_t i=0;i<draftEqSound20ParameterCount;++i){
                const auto value=static_cast<unsigned char>(s.readByte());if(value>1)return false;
                const auto target=i<oldSound19ParameterCount?i:i+(oldSound20ParameterCount-oldSound19ParameterCount);
                document.controlLocks[target]=value==1;
            }
        }else if(privateDrum27){
            for(std::size_t saved=0;saved<privateDrumSound27ParameterCount;++saved){
                const auto b=static_cast<unsigned char>(s.readByte());if(b>1)return false;
                const auto target=saved<oldSound26ParameterCount?saved:saved + (drumParameterStart-oldSound26ParameterCount);
                document.controlLocks[target]=b==1;
            }
        }else if(!readLocks(s,document.controlLocks,allowedParameters))return false;
        if(!readLocks(s,document.synthLocks)||!readLocks(s,document.effectLocks))return false;
    }
    if(version==4&&(s.getPosition()!=legacyNumericPayloadBytes||!readPersonalities(s,payload,document,version)))return false;
    if(version>=5){
        const auto numericBytes=saturationPrefix?saturationNumericBytes:version==30?oldSound30NumericBytes:version==29?oldSound29NumericBytes:version==5?oldSound5NumericBytes:version==6?oldSound6NumericBytes:version==7?oldSound7NumericBytes:version==8?oldSound8NumericBytes:version==9?oldSound9NumericBytes:version==10?oldSound10NumericBytes:version==11?oldSound11NumericBytes:version==12?oldSound12NumericBytes:version==13?oldSound13NumericBytes:version==14?oldSound14NumericBytes:version==15?oldSound15NumericBytes:version==16?oldSound16NumericBytes:version==17?oldSound17NumericBytes:version==18?oldSound18NumericBytes:version==19?oldSound19NumericBytes:version==20?(draftEq?draftEqSound20NumericBytes:oldSound20NumericBytes):proR?proRSound21NumericBytes:version==21?oldSound21NumericBytes:version==22?oldSound22NumericBytes:proR23?proRSound23NumericBytes:version==23||namedDetector24?oldSound23NumericBytes:proRFull?proRSound25NumericBytes:spectral25?spectralSound25NumericBytes:version==26?oldSound26NumericBytes:privateDrum27?privateDrumSound27NumericBytes:version==27?oldSound27NumericBytes:version==28?oldSound28NumericBytes:currentSoundNumericBytes;
        const auto compatibilityBytes=saturationPrefix?std::size_t(152):version==30?oldSound30CompatibilityBytes:version==28||version==29?oldSound29CompatibilityBytes:version==5?oldSound5CompatibilityBytes:version==6?oldSound6CompatibilityBytes:version==7?oldSound7CompatibilityBytes:version==8?oldSound8CompatibilityBytes:version==9?oldSound9CompatibilityBytes:version==10?oldSound10CompatibilityBytes:version==11?oldSound11CompatibilityBytes:version==12?oldSound12CompatibilityBytes:version==13?oldSound13CompatibilityBytes:version==14?oldSound14CompatibilityBytes:version==15?oldSound15CompatibilityBytes:version==16?oldSound16CompatibilityBytes:version==17?oldSound17CompatibilityBytes:version==18?oldSound18CompatibilityBytes:version==19?oldSound19CompatibilityBytes:version==20?oldSound20CompatibilityBytes:proR?proRSound21CompatibilityBytes:version==21?oldSound21CompatibilityBytes:version==22?oldSound22CompatibilityBytes:proR23?proRSound23CompatibilityBytes:version==23||namedDetector24||spectral25?oldSound23CompatibilityBytes:version<28?oldSound26CompatibilityBytes:kernelCompatibilitySectionBytes;
        const auto effectBanks=saturationPrefix?std::size_t(17):version==30?std::size_t(19):version==28||version==29?std::size_t(18):version==5?std::size_t(4):version==6?std::size_t(5):version==7?std::size_t(6):version==8?std::size_t(7):version==9?std::size_t(8):version==10?std::size_t(9):version==11?std::size_t(10):version==12?std::size_t(11):version==13?std::size_t(12):version==14?std::size_t(13):version==15?std::size_t(14):version==16?std::size_t(15):(version<=23&&!proR&&!proR23)||namedDetector24||spectral25?std::size_t(16):version<28?std::size_t(17):effectBankCount;
        if(s.getPosition()!=static_cast<juce::int64>(numericBytes)||s.readInt()!=static_cast<int>(compatibilityBytes)
            ||s.readShort()!=1||s.readByte()!=4||s.readByte()!=2||s.readByte()!=8||s.readByte()!=static_cast<char>(effectBanks)||s.readShort()!=0)return false;
        for(auto& banks:document.compatibility.synth)for(auto& revision:banks)revision=static_cast<AlgorithmRevision>(s.readByte());
        // Copy old four through fourteen-bank logical identities without using the new
        // in-memory stride. New inactive banks retain initialized native1.
        for(auto& banks:document.compatibility.effects)for(std::size_t bank=0;bank<effectBanks;++bank)banks[saturationPrefix&&bank==16?saturationEffectBank:bank]=static_cast<AlgorithmRevision>(s.readByte());
        if(!validateKernelCompatibility(document.compatibility))return false;
        auto personalitiesEnd=payload;
        if(hasName){
            const auto at=static_cast<std::size_t>(s.getPosition());
            if(at>payload||payload-at<8)return false;
            const auto sectionBytes=static_cast<std::uint32_t>(s.readInt());s.setPosition(static_cast<juce::int64>(at));
            if(sectionBytes>(version>=31?maximumPersonalitySectionBytes:oldPersonalitySectionBytes)||sectionBytes>payload-at-8)return false;
            personalitiesEnd=at+4+sectionBytes;
        }
        if(!readPersonalities(s,personalitiesEnd,document,version,true,draftEq,proR,proR23,proRFull,privateDrum27,saturationPrefix)||(hasName&&!readName(s,payload,document)))return false;
    }
    if(s.getPosition()!=static_cast<juce::int64>(payload)||!validateSoundDocument(document))return false;
    if(version<=30&&document.personalities){
        std::shared_ptr<RackPersonalityState> migrated;
        for(std::size_t n=0;n<document.personalities->instances.size();++n){const auto& rack=document.personalities->instances[n];
            if(!rack.package)if(auto ref=legacyRackPackageRef(rack.design)){
                if(!migrated)migrated=std::make_shared<RackPersonalityState>(*document.personalities);
                migrated->instances[n].package=std::move(ref);
            }
        }
        if(migrated)document.personalities=std::move(migrated);
    }
    output=document;return true;
    }catch(...){return false;}
}
bool encodeSoundState(const ParameterValues& values,juce::MemoryBlock& output)
{
    SoundDocument document;document.values=values;allSynthsPresent(document);
    return encodeSoundState(document,output);
}
bool decodeSoundState(const void* data,std::size_t size,ParameterValues& output)
{
    SoundDocument document;if(!decodeSoundState(data,size,document))return false;
    output=document.values;return true;
}
}
