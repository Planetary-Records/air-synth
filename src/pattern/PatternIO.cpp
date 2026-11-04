#include "PatternIO.h"
#include "PatternTiming.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace airack
{
namespace
{
constexpr std::size_t headerBytes = 24, noteBytes = 19, digestBytes = 32;

bool writeAtomically(const juce::MemoryBlock& data, const juce::File& target)
{
    if (target.isDirectory()) return false;
    juce::TemporaryFile temporary(target);
    {
        auto stream = temporary.getFile().createOutputStream();
        if (!stream || !stream->write(data.getData(), data.getSize())) return false;
        stream->flush();
        if (stream->getStatus().failed()) return false;
    }
    return temporary.overwriteTargetFileWithTemporary();
}
}

bool encodePatternAsset(const PatternDocument& document, juce::MemoryBlock& output)
{
    if (!validatePatternDocument(document)) return false;
    juce::MemoryOutputStream stream;
    stream.write("AIPT", 4);
    stream.writeShort(1);
    stream.writeShort(0);
    stream.writeByte(static_cast<char>(document.bars));
    stream.writeByte(static_cast<char>(document.meter.numerator));
    stream.writeByte(static_cast<char>(document.meter.denominator));
    stream.writeByte(static_cast<char>(document.grid));
    stream.writeDouble(document.swing);
    stream.writeShort(static_cast<short>(document.noteCount));
    stream.writeShort(0);
    for (std::size_t i = 0; i < document.noteCount; ++i)
    {
        const auto& note = document.notes[i];
        stream.writeDouble(note.startBeats);
        stream.writeDouble(note.durationBeats);
        stream.writeByte(static_cast<char>(note.pitch));
        stream.writeByte(static_cast<char>(note.velocity));
        stream.writeByte(static_cast<char>(note.channel));
    }
    const auto digest = juce::SHA256(stream.getData(), stream.getDataSize()).getRawData();
    stream.write(digest.getData(), digest.getSize());
    output = stream.getMemoryBlock();
    return true;
}

bool decodePatternAsset(const void* data, std::size_t size, PatternDocument& output)
{
    if (data == nullptr || size < headerBytes + digestBytes || size > maximumPatternAssetBytes)
        return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (std::memcmp(bytes, "AIPT", 4) != 0) return false;
    juce::MemoryInputStream stream(data, size, false);
    stream.skipNextBytes(4);
    if (stream.readShort() != 1 || stream.readShort() != 0) return false;
    PatternDocument candidate;
    candidate.bars = static_cast<std::uint8_t>(stream.readByte());
    candidate.meter.numerator = static_cast<std::uint8_t>(stream.readByte());
    candidate.meter.denominator = static_cast<std::uint8_t>(stream.readByte());
    candidate.grid = static_cast<PatternGrid>(stream.readByte());
    candidate.swing = stream.readDouble();
    candidate.noteCount = static_cast<std::uint16_t>(stream.readShort());
    if (stream.readShort() != 0 || candidate.noteCount > maximumPatternNotes
        || size != headerBytes + noteBytes * candidate.noteCount + digestBytes) return false;
    const auto digest = juce::SHA256(data, size - digestBytes).getRawData();
    if (std::memcmp(digest.getData(), bytes + size - digestBytes, digestBytes) != 0) return false;
    for (std::size_t i = 0; i < candidate.noteCount; ++i)
    {
        auto& note = candidate.notes[i];
        note.startBeats = stream.readDouble();
        note.durationBeats = stream.readDouble();
        note.pitch = static_cast<std::uint8_t>(stream.readByte());
        note.velocity = static_cast<std::uint8_t>(stream.readByte());
        note.channel = static_cast<std::uint8_t>(stream.readByte());
    }
    if (!validatePatternDocument(candidate)) return false;
    output = candidate;
    return true;
}

bool savePatternAsset(const PatternDocument& document, const juce::File& file)
{
    juce::MemoryBlock encoded;
    return encodePatternAsset(document, encoded) && writeAtomically(encoded, file);
}

bool loadPatternAsset(const juce::File& file, PatternDocument& output)
{
    if (!file.existsAsFile()) return false;
    auto stream = file.createInputStream();
    if (!stream || stream->getTotalLength() < static_cast<juce::int64>(headerBytes + digestBytes)
        || stream->getTotalLength() > static_cast<juce::int64>(maximumPatternAssetBytes)) return false;
    // Bound the read even if another process grows the file after the size check.
    std::array<std::uint8_t, maximumPatternAssetBytes + 1> bytes {};
    const int count = stream->read(bytes.data(), static_cast<int>(bytes.size()));
    if (count < 0 || stream->getStatus().failed() || !stream->isExhausted()) return false;
    return decodePatternAsset(bytes.data(), static_cast<std::size_t>(count), output);
}

bool encodePatternMidi(const PatternDocument& document, juce::MemoryBlock& output)
{
    PatternPlaybackNotes playback;
    if (!makePatternPlaybackNotes(document, playback)) return false;
    const int phraseTicks = static_cast<int>(std::floor(playback.lengthBeats * patternMidiTicksPerQuarter + .5));
    struct Edge { int tick; bool on; std::size_t index; };
    std::array<Edge, maximumPatternNotes * 2> edges {};
    for (std::size_t i = 0; i < playback.noteCount; ++i)
    {
        const auto& note = playback.notes[i];
        const int on = std::clamp(static_cast<int>(std::floor(note.startBeats * patternMidiTicksPerQuarter + .5)), 0, phraseTicks - 1);
        const int off = std::clamp(static_cast<int>(std::floor(note.endBeats * patternMidiTicksPerQuarter + .5)), on + 1, phraseTicks);
        edges[2 * i] = {on, true, i};
        edges[2 * i + 1] = {off, false, i};
    }
    const auto end = edges.begin() + 2 * playback.noteCount;
    std::sort(edges.begin(), end, [](const Edge& a, const Edge& b)
    {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.on != b.on) return !a.on;
        return a.index < b.index;
    });
    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::timeSignatureMetaEvent(document.meter.numerator, document.meter.denominator));
    for (auto edge = edges.begin(); edge != end; ++edge)
    {
        const auto& note = playback.notes[edge->index];
        auto message = edge->on ? juce::MidiMessage::noteOn(note.channel, note.pitch, static_cast<juce::uint8>(note.velocity))
                                : juce::MidiMessage::noteOff(note.channel, note.pitch);
        message.setTimeStamp(edge->tick);
        track.addEvent(message);
    }
    auto endMessage = juce::MidiMessage::endOfTrack();
    endMessage.setTimeStamp(phraseTicks);
    track.addEvent(endMessage);
    // Do not call updateMatchedPairs: it inserts note-offs for overlapping identical keys.
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(patternMidiTicksPerQuarter);
    midi.addTrack(track);
    juce::MemoryOutputStream stream;
    if (!midi.writeTo(stream, 0)) return false;
    output = stream.getMemoryBlock();
    return true;
}

bool exportPatternMidi(const PatternDocument& document, const juce::File& file)
{
    juce::MemoryBlock encoded;
    return encodePatternMidi(document, encoded) && writeAtomically(encoded, file);
}
}
