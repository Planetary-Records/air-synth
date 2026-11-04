#pragma once
#include "SoundState.h"
#include <juce_cryptography/juce_cryptography.h>

namespace airack::test {
// Convert only the bounded personality section of a current state to its
// predecessor representation; the old envelope supplies its own digest.
inline bool legacyPersonalitySection(const juce::MemoryBlock& current,juce::MemoryBlock& result)
{
    const auto* p=static_cast<const unsigned char*>(current.getData());
    const auto size=current.getSize(), start=currentPersonalityLengthOffset;
    const auto u32=[&](std::size_t at){return std::uint32_t(p[at])|(std::uint32_t(p[at+1])<<8)|(std::uint32_t(p[at+2])<<16)|(std::uint32_t(p[at+3])<<24);};
    if(size<start+4+4+32)return false;
    const auto length=std::size_t(u32(start));
    if(length>size-start-4-4-32)return false;
    const auto end=start+4+length;
    const auto nameBytes=std::size_t(u32(end));if(nameBytes>size-end-4-32||end+4+nameBytes+32!=size)return false;
    juce::MemoryBlock section;juce::MemoryOutputStream out(section,false);
    if(length){
        if(length<4||p[start+4]!=2||p[start+5]!=0||p[start+6]<1||p[start+6]>12||p[start+7]!=0)return false;
        out.writeShort(1);out.writeByte(char(p[start+6]));out.writeByte(0);
        auto at=start+8;
        for(unsigned n=0;n<p[start+6];++n){
            if(at>end||end-at<18)return false;
            const auto bindings=unsigned(p[at+17]);if(bindings<1||bindings>9||end-at<18+2*bindings+4)return false;
            const auto header=18+2*bindings;const auto design=std::size_t(u32(at+header));
            if(design<1||design>65536||design>end-at-header-4)return false;
            const auto bytes=header+4+design;out.write(p+at,bytes);at+=bytes;
            if(at>=end||p[at]>1)return false;
            const auto present=p[at++];
            if(present)for(unsigned field=0;field<3;++field){
                if(end-at<4)return false;const auto bytes=std::size_t(u32(at));at+=4;
                if(bytes>std::size_t(field==0?64:field==1?32:64)||bytes>end-at)return false;at+=bytes;
            }
        }
        if(at!=end)return false;
    }
    out.flush();juce::MemoryBlock projected;juce::MemoryOutputStream dest(projected,false);
    dest.writeInt(int(section.getSize()));dest.write(section.getData(),section.getSize());dest.flush();result.swapWith(projected);return true;
}
inline bool oldEnvelopeWithPersonality(const juce::MemoryBlock& old,std::size_t prefix,const SoundDocument& value,juce::MemoryBlock& result)
{
    juce::MemoryBlock current,section;if(prefix>old.getSize()||!encodeSoundState(value,current)||!legacyPersonalitySection(current,section))return false;
    juce::MemoryBlock encoded;juce::MemoryOutputStream out(encoded,false);out.write(old.getData(),prefix);out.write(section.getData(),section.getSize());
    if(old.getSize()<8)return false;const auto* original=static_cast<const unsigned char*>(old.getData());
    const auto version=std::uint32_t(original[4])|(std::uint32_t(original[5])<<8)|(std::uint32_t(original[6])<<16)|(std::uint32_t(original[7])<<24);
    if(version>=25){const auto* bytes=static_cast<const unsigned char*>(current.getData());const auto at=currentPersonalityLengthOffset;
        const auto len=std::size_t(bytes[at])|(std::size_t(bytes[at+1])<<8)|(std::size_t(bytes[at+2])<<16)|(std::size_t(bytes[at+3])<<24);
        const auto name=at+4+len;out.write(bytes+name,current.getSize()-32-name);}
    const auto digest=juce::SHA256(out.getData(),out.getDataSize()).getRawData();out.write(digest.getData(),digest.getSize());out.flush();result.swapWith(encoded);return true;
}
}
