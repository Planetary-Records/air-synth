#include "ProjectDocument.h"
#include "SoundState.h"
#include "PatternIO.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace airack
{
namespace
{
constexpr std::size_t digestBytes = 32, minimumPatternBytes = 56;
static_assert(oldSaturationProject19MinimumBytes==projectStateHeaderBytes+oldSaturationSound20StateSize+minimumPatternBytes+digestBytes);
static_assert(oldSaturationProject21MinimumBytes==projectStateHeaderBytes+oldSaturationSound22StateSize+minimumPatternBytes+digestBytes);
static_assert(oldSaturationProject23MinimumBytes==projectStateHeaderBytes+oldSaturationSound24StateSize+minimumPatternBytes+digestBytes);
static_assert(oldProject28MinimumBytes==projectStateHeaderBytes+oldSound29StateSize+minimumPatternBytes+digestBytes);
static_assert(oldProject18MinimumBytes == projectStateHeaderBytes + oldSound19StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject18MaximumBytes == projectStateHeaderBytes + oldSound19MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject17MinimumBytes == projectStateHeaderBytes + oldSound18StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject17MaximumBytes == projectStateHeaderBytes + oldSound18MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(minimumProjectStateBytes == projectStateHeaderBytes + legacySoundStateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject4MinimumBytes == projectStateHeaderBytes + oldSound5StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject4MaximumBytes == projectStateHeaderBytes + oldSound5MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject5MinimumBytes == projectStateHeaderBytes + oldSound6StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject5MaximumBytes == projectStateHeaderBytes + oldSound6MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject6MinimumBytes == projectStateHeaderBytes + oldSound7StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject6MaximumBytes == projectStateHeaderBytes + oldSound7MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject7MinimumBytes == projectStateHeaderBytes + oldSound8StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject7MaximumBytes == projectStateHeaderBytes + oldSound8MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject8MinimumBytes == projectStateHeaderBytes + oldSound9StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject8MaximumBytes == projectStateHeaderBytes + oldSound9MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject9MinimumBytes == projectStateHeaderBytes + oldSound10StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject9MaximumBytes == projectStateHeaderBytes + oldSound10MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject10MinimumBytes == projectStateHeaderBytes + oldSound11StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject10MaximumBytes == projectStateHeaderBytes + oldSound11MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject12MinimumBytes == projectStateHeaderBytes + oldSound13StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject12MaximumBytes == projectStateHeaderBytes + oldSound13MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject11MinimumBytes == projectStateHeaderBytes + oldSound12StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject11MaximumBytes == projectStateHeaderBytes + oldSound12MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject13MinimumBytes == projectStateHeaderBytes + oldSound14StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject13MaximumBytes == projectStateHeaderBytes + oldSound14MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject14MinimumBytes == projectStateHeaderBytes + oldSound15StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject14MaximumBytes == projectStateHeaderBytes + oldSound15MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject15MinimumBytes == projectStateHeaderBytes + oldSound16StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject15MaximumBytes == projectStateHeaderBytes + oldSound16MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject17MinimumBytes == projectStateHeaderBytes + oldSound18StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject17MaximumBytes == projectStateHeaderBytes + oldSound18MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject19MinimumBytes == projectStateHeaderBytes + oldSound20StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject19MaximumBytes == projectStateHeaderBytes + oldSound20MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject18MinimumBytes == projectStateHeaderBytes + oldSound19StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject18MaximumBytes == projectStateHeaderBytes + oldSound19MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject19MinimumBytes == projectStateHeaderBytes + oldSound20StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject19MaximumBytes == projectStateHeaderBytes + oldSound20MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject20MinimumBytes == projectStateHeaderBytes + oldSound21StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject20MaximumBytes == projectStateHeaderBytes + oldSound21MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject21MinimumBytes == projectStateHeaderBytes + oldSound22StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject21MaximumBytes == projectStateHeaderBytes + oldSound22MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(proRProject20MinimumBytes == projectStateHeaderBytes + proRSound21StateSize + minimumPatternBytes + digestBytes);
static_assert(proRProject20MaximumBytes == projectStateHeaderBytes + proRSound21MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject22MinimumBytes == projectStateHeaderBytes + oldSound23StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject22MaximumBytes == projectStateHeaderBytes + oldSound23MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(proRProject22MinimumBytes == projectStateHeaderBytes + proRSound23StateSize + minimumPatternBytes + digestBytes);
static_assert(proRProject22MaximumBytes == projectStateHeaderBytes + proRSound23MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject23MinimumBytes == projectStateHeaderBytes + oldSound24StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject23MaximumBytes == projectStateHeaderBytes + oldSound24MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(proRProject23MinimumBytes == projectStateHeaderBytes + proRSound24StateSize + minimumPatternBytes + digestBytes);
static_assert(proRProject23MaximumBytes == projectStateHeaderBytes + proRSound24MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject25MinimumBytes == projectStateHeaderBytes + oldSound26StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject25MaximumBytes == projectStateHeaderBytes + oldSound26MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(oldProject26MinimumBytes == projectStateHeaderBytes + oldSound27StateSize + minimumPatternBytes + digestBytes);
static_assert(oldProject26MaximumBytes == projectStateHeaderBytes + oldSound27MaximumBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
static_assert(currentMinimumProjectStateBytes == projectStateHeaderBytes + currentSoundStateSize + minimumPatternBytes + digestBytes);
static_assert(maximumProjectStateBytes == projectStateHeaderBytes + maximumSoundStateBytes + maximumPatternAssetBytes + groupIntentStateBytes + digestBytes);
bool validText(std::string_view text) noexcept
{
    return text.size() <= maximumGroupTextBytes && text.find('\0') == std::string_view::npos
        && (text.empty() || juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size())));
}
bool canonicalText(const GroupText& text) noexcept
{
    return text.length <= maximumGroupTextBytes && validText({text.bytes.data(), text.length})
        && std::all_of(text.bytes.begin() + text.length, text.bytes.end(), [](char c) { return c == 0; });
}
}

GroupUuid::GroupUuid(const juce::Uuid& value) noexcept { *this = value; }
GroupUuid& GroupUuid::operator=(const juce::Uuid& value) noexcept
{ std::copy_n(value.getRawData(), bytes.size(), bytes.begin()); return *this; }
bool GroupUuid::isNull() const noexcept
{ return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; }); }
const std::uint8_t* GroupUuid::getRawData() const noexcept { return bytes.data(); }
bool setGroupText(std::string_view text, GroupText& output) noexcept
{
    if (!validText(text)) return false;
    GroupText candidate;
    if (!text.empty()) std::copy(text.begin(), text.end(), candidate.bytes.begin());
    candidate.length = static_cast<std::uint16_t>(text.size());
    output = candidate;
    return true;
}
std::string_view groupTextView(const GroupText& text) noexcept
{ return canonicalText(text) ? std::string_view(text.bytes.data(), text.length) : std::string_view{}; }
bool validateGroupIntent(const GroupIntent& intent) noexcept
{
    return !intent.group.isNull() && !intent.member.isNull() && intent.name.length != 0
        && canonicalText(intent.name) && canonicalText(intent.role) && canonicalText(intent.key) && canonicalText(intent.scale)
        && (!intent.bpm || (std::isfinite(*intent.bpm) && *intent.bpm >= 20 && *intent.bpm <= 400));
}

bool validateProjectDocument(const ProjectDocument& document) noexcept
{
    return (document.mode == PlaybackMode::externalMidi || document.mode == PlaybackMode::internalPattern)
        && validateSoundDocument(document.sound) && validatePatternDocument(document.pattern)
        && (!document.group || validateGroupIntent(*document.group));
}

bool encodeProjectState(const ProjectDocument& document, juce::MemoryBlock& output)
{
    if (!validateProjectDocument(document)) return false;
    juce::MemoryBlock sound, pattern;
    if (!encodeSoundState(document.sound, sound) || !encodePatternAsset(document.pattern, pattern)) return false;
    juce::MemoryOutputStream stream;
    stream.write("AIPJ", 4);
    stream.writeShort(currentProjectStateVersion);
    stream.writeShort(0);
    stream.writeByte(static_cast<char>(document.mode));
    stream.writeByte(document.group ? 1 : 0);
    stream.writeShort(static_cast<short>(document.group ? groupIntentStateBytes : 0));
    stream.writeInt(static_cast<int>(sound.getSize()));
    stream.writeInt(static_cast<int>(pattern.getSize()));
    stream.write(sound.getData(), sound.getSize());
    stream.write(pattern.getData(), pattern.getSize());
    if (document.group)
    {
        const auto& group = *document.group;
        stream.write(group.group.getRawData(), 16);
        stream.write(group.member.getRawData(), 16);
        for (const auto* text : {&group.name, &group.role, &group.key, &group.scale})
        { stream.writeShort(static_cast<short>(text->length)); stream.write(text->bytes.data(), text->bytes.size()); }
        stream.writeByte(group.bpm ? 1 : 0);
        stream.writeDouble(group.bpm.value_or(0.0));
    }
    const auto digest = juce::SHA256(stream.getData(), stream.getDataSize()).getRawData();
    stream.write(digest.getData(), digest.getSize());
    output = stream.getMemoryBlock();
    return true;
}

bool decodeProjectState(const void* data, std::size_t size, ProjectDocument& output)
{
    if (data == nullptr) return false;
    if (size >= 16 && size <= maximumSoundStateBytes && std::memcmp(data,"ARSI",4)==0)
    {
        ProjectDocument candidate{.sound={SoundAudioState{.values={}}}};
        if (!decodeSoundState(data, size, candidate.sound)) return false;
        output = candidate;
        return true;
    }
    if (size < minimumProjectStateBytes || size > maximumProjectStateBytes) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (std::memcmp(bytes, "AIPJ", 4) != 0) return false;
    juce::MemoryInputStream stream(data, size, false);
    stream.skipNextBytes(4);
    const auto version = stream.readShort();
    if ((version != 1 && version != 2 && version != 3 && version != 4 && version != 5 && version != 6 && version != 7 && version != 8 && version != 9 && version != 10 && version != 11 && version != 12 && version != 13 && version != 14 && version != 15 && version != 16 && version != 17 && version != 18 && version != 19 && version != 20 && version != 21 && version != 22 && version != 23 && version != 24 && version != 25 && version != 26 && version != 27 && version != 28 && version != 29 && version != currentProjectStateVersion) || stream.readShort() != 0) return false;
    if ((version == 4 && (size < oldProject4MinimumBytes || size > oldProject4MaximumBytes))
        || (version == 5 && (size < oldProject5MinimumBytes || size > oldProject5MaximumBytes))
        || (version == 6 && (size < oldProject6MinimumBytes || size > oldProject6MaximumBytes))
        || (version == 7 && (size < oldProject7MinimumBytes || size > oldProject7MaximumBytes))
        || (version == 8 && (size < oldProject8MinimumBytes || size > oldProject8MaximumBytes))
        || (version == 9 && (size < oldProject9MinimumBytes || size > oldProject9MaximumBytes))
        || (version == 10 && (size < oldProject10MinimumBytes || size > oldProject10MaximumBytes))
        || (version == 11 && (size < oldProject11MinimumBytes || size > oldProject11MaximumBytes))
        || (version == 12 && (size < oldProject12MinimumBytes || size > oldProject12MaximumBytes))
        || (version == 13 && (size < oldProject13MinimumBytes || size > oldProject13MaximumBytes))
        || (version == 14 && (size < oldProject14MinimumBytes || size > oldProject14MaximumBytes))
        || (version == 15 && (size < oldProject15MinimumBytes || size > oldProject15MaximumBytes))
        || (version == 16 && (size < oldProject16MinimumBytes || size > oldProject16MaximumBytes))
        || (version == 17 && (size < oldProject17MinimumBytes || size > oldProject17MaximumBytes))
        || (version == 18 && (size < oldProject18MinimumBytes || size > oldProject18MaximumBytes))
        || (version == 19 && (size < oldProject19MinimumBytes || size > oldSaturationProject19MaximumBytes))
        || (version == 20 && (size < oldProject20MinimumBytes || size > proRProject20MaximumBytes))
        || (version == 21 && (size < oldProject21MinimumBytes || size > oldSaturationProject21MaximumBytes))
        || (version == 22 && (size < oldProject22MinimumBytes || size > proRProject22MaximumBytes))
        || (version == 23 && (size < oldProject23MinimumBytes || size > proRProject23MaximumBytes))
        || (version == 24 && (size < spectralProject24MinimumBytes || size > proRProject24MaximumBytes))
        || (version == 25 && (size < oldProject25MinimumBytes || size > oldProject25MaximumBytes))
        || (version == 26 && (size < oldProject26MinimumBytes || size > privateDrumProject26MaximumBytes))
        || (version == 27 && (size < oldProject27MinimumBytes || size > oldProject27MaximumBytes))
        || (version == 28 && (size < oldProject28MinimumBytes || size > oldProject28MaximumBytes))
        || (version == 29 && (size < oldProject29MinimumBytes || size > oldProject29MaximumBytes))
        || (version == currentProjectStateVersion && size < currentMinimumProjectStateBytes)) return false;
    const auto mode = static_cast<PlaybackMode>(stream.readByte());
    const auto presence = stream.readByte();
    const auto groupBytes = stream.readShort();
    if ((mode != PlaybackMode::externalMidi && mode != PlaybackMode::internalPattern)
        || (version == 1 ? presence != 0 || groupBytes != 0 : version==2 ? presence != 1 || groupBytes != groupIntentStateBytes
            : (presence!=0&&presence!=1)||groupBytes!=(presence?groupIntentStateBytes:0))) return false;
    const auto soundBytes = stream.readInt(), patternBytes = stream.readInt();
    // Bound lengths before arithmetic or taking nested pointers. Nested codecs
    // enforce their own exact version/count/semantics in addition to integrity.
    if ((version<3?soundBytes != static_cast<int>(legacySoundStateSize)
            :version==3?soundBytes<=static_cast<int>(legacySoundStateSize)||soundBytes>static_cast<int>(legacyMaximumSoundStateBytes)
            :version==4?soundBytes<static_cast<int>(oldSound5StateSize)||soundBytes>static_cast<int>(oldSound5MaximumBytes)
            :version==5?soundBytes<static_cast<int>(oldSound6StateSize)||soundBytes>static_cast<int>(oldSound6MaximumBytes)
            :version==6?soundBytes<static_cast<int>(oldSound7StateSize)||soundBytes>static_cast<int>(oldSound7MaximumBytes)
            :version==7?soundBytes<static_cast<int>(oldSound8StateSize)||soundBytes>static_cast<int>(oldSound8MaximumBytes)
            :version==8?soundBytes<static_cast<int>(oldSound9StateSize)||soundBytes>static_cast<int>(oldSound9MaximumBytes)
            :version==9?soundBytes<static_cast<int>(oldSound10StateSize)||soundBytes>static_cast<int>(oldSound10MaximumBytes)
            :version==10?soundBytes<static_cast<int>(oldSound11StateSize)||soundBytes>static_cast<int>(oldSound11MaximumBytes)
            :version==11?soundBytes<static_cast<int>(oldSound12StateSize)||soundBytes>static_cast<int>(oldSound12MaximumBytes)
            :version==12?soundBytes<static_cast<int>(oldSound13StateSize)||soundBytes>static_cast<int>(oldSound13MaximumBytes)
            :version==13?soundBytes<static_cast<int>(oldSound14StateSize)||soundBytes>static_cast<int>(oldSound14MaximumBytes)
            :version==14?soundBytes<static_cast<int>(oldSound15StateSize)||soundBytes>static_cast<int>(oldSound15MaximumBytes)
            :version==15?soundBytes<static_cast<int>(oldSound16StateSize)||soundBytes>static_cast<int>(oldSound16MaximumBytes)
            :version==16?soundBytes<static_cast<int>(oldSound17StateSize)||soundBytes>static_cast<int>(oldSound17MaximumBytes)
            :version==17?soundBytes<static_cast<int>(oldSound18StateSize)||soundBytes>static_cast<int>(oldSound18MaximumBytes)
            :version==18?soundBytes<static_cast<int>(oldSound19StateSize)||soundBytes>static_cast<int>(oldSound19MaximumBytes)
            :version==19?soundBytes<static_cast<int>(oldSound20StateSize)||soundBytes>static_cast<int>(oldSaturationSound20MaximumBytes)
            :version==20?soundBytes<static_cast<int>(oldSound21StateSize)||soundBytes>static_cast<int>(proRSound21MaximumBytes)
            :version==21?soundBytes<static_cast<int>(oldSound22StateSize)||soundBytes>static_cast<int>(oldSaturationSound22MaximumBytes)
            :version==22?soundBytes<static_cast<int>(oldSound23StateSize)||soundBytes>static_cast<int>(proRSound23MaximumBytes)
            :version==23?soundBytes<static_cast<int>(oldSound24StateSize)||soundBytes>static_cast<int>(proRSound24MaximumBytes)
            :version==24?soundBytes<static_cast<int>(spectralSound25StateSize)||soundBytes>static_cast<int>(proRSound25MaximumBytes)
            :version==25?soundBytes<static_cast<int>(oldSound26StateSize)||soundBytes>static_cast<int>(oldSound26MaximumBytes)
            :version==26?soundBytes<static_cast<int>(oldSound27StateSize)||soundBytes>static_cast<int>(privateDrumSound27MaximumBytes)
            :version==27?soundBytes<static_cast<int>(oldSound28StateSize)||soundBytes>static_cast<int>(oldSound28MaximumBytes)
            :version==28?soundBytes<static_cast<int>(oldSound29StateSize)||soundBytes>static_cast<int>(oldSound29MaximumBytes)
            :version==29?soundBytes<static_cast<int>(oldSound30StateSize)||soundBytes>static_cast<int>(oldSound30MaximumBytes)
            :soundBytes<static_cast<int>(currentSoundStateSize)||soundBytes>static_cast<int>(maximumSoundStateBytes))
        || patternBytes < static_cast<int>(minimumPatternBytes)
        || patternBytes > static_cast<int>(maximumPatternAssetBytes)
        || size != projectStateHeaderBytes + static_cast<std::size_t>(soundBytes) + static_cast<std::size_t>(patternBytes)
                 + static_cast<std::size_t>(groupBytes) + digestBytes)
        return false;
    const auto digest = juce::SHA256(data, size - digestBytes).getRawData();
    if (std::memcmp(digest.getData(), bytes + size - digestBytes, digestBytes) != 0) return false;
    ProjectDocument candidate{.sound={SoundAudioState{.values={}}}};
    candidate.mode = mode;
    juce::MemoryInputStream soundHeader(bytes+projectStateHeaderBytes,static_cast<std::size_t>(soundBytes),false);
    soundHeader.skipNextBytes(4);
    const auto nestedVersion=soundHeader.readInt(),nestedCount=soundHeader.readInt();
    if(version!=26&&nestedVersion!=(version<3?3:version==3?4:version==4?5:version==5?6:version==6?7:version==7?8:version==8?9:version==9?10:version==10?11:version==11?12:version==12?13:version==13?14:version==14?15:version==15?16:version==16?17:version==17?18:version==18?19:version==19?20:version==20?21:version==21?22:version==22?23:version==23?24:version==24?25:version==25?26:version==26?27:version==27?28:version==28?29:version==29?30:currentSoundStateVersion))return false;
    if(version==19){
        const bool draftEq=nestedCount==static_cast<int>(draftEqSound20ParameterCount);
        const bool saturation=nestedCount==int(oldSaturationSound20ParameterCount);
        if(size<(saturation?oldSaturationProject19MinimumBytes:draftEq?draftEqProject19MinimumBytes:oldProject19MinimumBytes)
            ||size>(saturation?oldSaturationProject19MaximumBytes:draftEq?draftEqProject19MaximumBytes:oldProject19MaximumBytes))return false;
    }
    if(version==20){
        const auto count=nestedCount;
        const bool proR=count==static_cast<int>(proRSound21ParameterCount);
        if((!proR&&count!=static_cast<int>(oldSound21ParameterCount))
            ||size<(proR?proRProject20MinimumBytes:oldProject20MinimumBytes)
            ||size>(proR?proRProject20MaximumBytes:oldProject20MaximumBytes))return false;
    }
    if(version==21){
        const bool saturation=nestedCount==int(oldSaturationSound22ParameterCount);
        if(size<(saturation?oldSaturationProject21MinimumBytes:oldProject21MinimumBytes)
            ||size>(saturation?oldSaturationProject21MaximumBytes:oldProject21MaximumBytes))return false;
    }
    if(version==23&&nestedCount==int(oldSaturationSound24ParameterCount)
        &&(size<oldSaturationProject23MinimumBytes||size>oldSaturationProject23MaximumBytes))return false;
    if(version==28&&nestedCount!=int(oldSound29ParameterCount))return false;
    if(version==22){
        const auto count=nestedCount;
        const bool proR=count==static_cast<int>(proRSound23ParameterCount);
        if((!proR&&count!=static_cast<int>(oldSound23ParameterCount))
            ||size<(proR?proRProject22MinimumBytes:oldProject22MinimumBytes)
            ||size>(proR?proRProject22MaximumBytes:oldProject22MaximumBytes))return false;
    }
    if(version==24){
        const auto count=nestedCount;
        const bool proR=count==static_cast<int>(proRSound25ParameterCount);
        if((!proR&&count!=static_cast<int>(spectralSound25ParameterCount))
            ||size<(proR?proRProject24MinimumBytes:spectralProject24MinimumBytes)
            ||size>(proR?proRProject24MaximumBytes:spectralProject24MaximumBytes))return false;
    }
    if(version==26){
        if(!((nestedVersion==27&&nestedCount==static_cast<int>(oldSound27ParameterCount)
                &&size>=oldProject26MinimumBytes&&size<=oldProject26MaximumBytes
                &&soundBytes>=static_cast<int>(oldSound27StateSize)&&soundBytes<=static_cast<int>(oldSound27MaximumBytes))
            ||(nestedVersion==27&&nestedCount==static_cast<int>(privateDrumSound27ParameterCount)
                &&size>=privateDrumProject26MinimumBytes&&size<=privateDrumProject26MaximumBytes
                &&soundBytes>=static_cast<int>(privateDrumSound27StateSize)&&soundBytes<=static_cast<int>(privateDrumSound27MaximumBytes))))return false;
    }
    if(version==27&&nestedCount!=static_cast<int>(oldSound28ParameterCount))return false;
    if((version==29&&nestedCount!=int(oldSound30ParameterCount))||(version==currentProjectStateVersion&&nestedCount!=int(parameterCount)))return false;
    if (!decodeSoundState(bytes + projectStateHeaderBytes, static_cast<std::size_t>(soundBytes), candidate.sound)
        || (version==3&&(!candidate.sound.personalities||candidate.sound.personalities->instances.empty()))
        || !decodePatternAsset(bytes + projectStateHeaderBytes + static_cast<std::size_t>(soundBytes),
                               static_cast<std::size_t>(patternBytes), candidate.pattern)) return false;
    if (presence == 1)
    {
        stream.setPosition(projectStateHeaderBytes + static_cast<std::size_t>(soundBytes) + static_cast<std::size_t>(patternBytes));
        GroupIntent group;
        stream.read(group.group.bytes.data(), 16);
        stream.read(group.member.bytes.data(), 16);
        for (auto* text : {&group.name, &group.role, &group.key, &group.scale})
        { text->length = static_cast<std::uint16_t>(stream.readShort()); stream.read(text->bytes.data(), static_cast<int>(maximumGroupTextBytes)); }
        const auto tempoPresent = stream.readByte();
        const auto tempoBits = static_cast<std::uint64_t>(stream.readInt64());
        if ((tempoPresent != 0 && tempoPresent != 1) || (tempoPresent == 0 && tempoBits != 0)) return false;
        if (tempoPresent == 1) group.bpm = std::bit_cast<double>(tempoBits);
        candidate.group = group;
    }
    if (!validateProjectDocument(candidate)) return false;
    output = candidate;
    return true;
}
}
