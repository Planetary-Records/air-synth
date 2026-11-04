#include "ProbeProcessor.h"
#include <cmath>
#include "ProbeHostDiagnostics.h"
#include "ProbeEditor.h"
void ProbeProcessor::configureTestControls()
{
    for (size_t i = 0; i < 129; ++i)
    {
        auto& parameter = *controls[i]; const auto prior = parameter.get();
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(prior < 0.5f ? 0.75f : 0.25f);
        parameter.setValueNotifyingHost(prior);
        parameter.endChangeGesture();
    }
    probe::ProbeHostDiagnostics::record(runtimeId, "configure.p000-p128", identitySnapshot());
}
juce::var ProbeProcessor::identitySnapshot()
{
    const juce::ScopedLock lock(stateLock);
    auto* result = new juce::DynamicObject;
    result->setProperty("runtime", runtimeId.toString());
    result->setProperty("group", groupId.toString());
    result->setProperty("member", memberId.toString());
    return result;
}
void ProbeProcessor::createTestGroup() { joinTestGroup(juce::Uuid()); }
void ProbeProcessor::joinTestGroup(const juce::Uuid& id)
{
    const juce::ScopedLock lock(stateLock);
    groupId = id;
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
    probe::ProbeHostDiagnostics::record(runtimeId, "group.explicit", identitySnapshot());
}
void ProbeProcessor::clearTestGroup() { joinTestGroup(juce::Uuid::null()); }
juce::MemoryBlock ProbeProcessor::saveNativeSound()
{
    const juce::ScopedLock lock(stateLock);
    juce::MemoryBlock data;
    juce::MemoryOutputStream out(data, false);
    out.writeInt(0x31534e50); out.writeInt(1);
    for (int i = 0; i < 160; ++i) { out.writeInt(i); out.writeFloat(controls[static_cast<size_t>(i)]->get()); }
    out.flush();
    const auto hash = juce::SHA256(data).getRawData(); out.write(hash.getData(), hash.getSize()); out.flush();
    return data;
}
bool ProbeProcessor::loadNativeSound(const void* data, int size)
{
    constexpr int payload = 8 + 160 * 8;
    if (data == nullptr || size != payload + 32) return false;
    const auto hash = juce::SHA256(data, payload).getRawData();
    if (std::memcmp(hash.getData(), static_cast<const char*>(data) + payload, 32) != 0) return false;
    juce::MemoryInputStream in(data, static_cast<size_t>(size), false);
    if (in.readInt() != 0x31534e50 || in.readInt() != 1) return false;
    std::array<float, 160> values {};
    std::array<bool, 160> seen {};
    for (int i = 0; i < 160; ++i)
    {
        const auto id = in.readInt(); const auto value = in.readFloat();
        if (id < 0 || id >= 160 || seen[static_cast<size_t>(id)] || !std::isfinite(value) || value < 0 || value > 1) return false;
        seen[static_cast<size_t>(id)] = true; values[static_cast<size_t>(id)] = value;
    }
    const juce::ScopedLock lock(stateLock);
    for (size_t i = 0; i < values.size(); ++i) controls[i]->setValueNotifyingHost(values[i]);
    probe::ProbeHostDiagnostics::record(runtimeId, "native_sound.load", identitySnapshot());
    return true;
}
ProbeProcessor::ProbeProcessor() : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (int i = 0; i < 160; ++i)
    {
        const auto id = "probe.p" + juce::String(i).paddedLeft('0', 3);
        auto* parameter = new juce::AudioParameterFloat(juce::ParameterID { id, 1 }, id, juce::NormalisableRange<float> { 0.0f, 1.0f }, 0.25f);
        controls[static_cast<size_t>(i)] = parameter;
        addParameter(parameter);
        parameter->addListener(this);
    }
    probe::ProbeHostDiagnostics::record(runtimeId, "construct", identitySnapshot());
}
ProbeProcessor::~ProbeProcessor()
{
    probe::ProbeHostDiagnostics::record(runtimeId, "destroy", identitySnapshot());
    for (auto* parameter : controls)
        parameter->removeListener(this);
}
void ProbeProcessor::prepareToPlay(double sampleRate, int)
{
    rate = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 768000.0 ? sampleRate : 44100.0;
    phase = 0.0;
    wasPlaying = false;
    releaseResources();
}
void ProbeProcessor::releaseResources() { activeNote = -1; }
void ProbeProcessor::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
        {
            const bool playing = position->getIsPlaying();
            if (wasPlaying && !playing)
                activeNote = -1;
            wasPlaying = playing;
        }

    const float rawGain = controls[0]->get();
    const float gain = std::isfinite(rawGain) ? juce::jlimit(0.0f, 1.0f, rawGain) : 0.0f;
    int cursor = 0;
    auto renderUntil = [&](int end)
    {
        for (; cursor < end; ++cursor)
            if (activeNote >= 0)
            {
                const auto value = gain * velocity * static_cast<float>(std::sin(phase));
                for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                    audio.setSample(channel, cursor, value);
                phase = std::fmod(phase + phaseStep, juce::MathConstants<double>::twoPi);
            }
    };
    for (const auto metadata : midi)
    {
        renderUntil(juce::jlimit(0, audio.getNumSamples(), metadata.samplePosition));
        // Only short MIDI is relevant to the tone. Copying SysEx would allocate.
        if (metadata.numBytes > 3)
            continue;
        const auto message = metadata.getMessage();
        // ponytail: one most-recent-note reference voice; polyphony is M1 work.
        if (message.isNoteOn())
        {
            activeNote = message.getNoteNumber();
            activeChannel = message.getChannel();
            velocity = message.getFloatVelocity();
            phase = 0.0;
            phaseStep = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz(activeNote) / rate;
        }
        else if (message.isMidiStop()
                 || ((message.isAllNotesOff() || message.isAllSoundOff()) && message.getChannel() == activeChannel)
                 || (message.isNoteOff() && message.getNoteNumber() == activeNote && message.getChannel() == activeChannel))
            activeNote = -1;
    }
    renderUntil(audio.getNumSamples());
    midi.clear();
}
void ProbeProcessor::getStateInformation(juce::MemoryBlock& state)
{
    const juce::ScopedLock lock(stateLock);
    state.reset();
    juce::MemoryOutputStream out(state, false);
    out.writeInt(0x30505253);
    out.writeInt(1);
    out.write(groupId.getRawData(), 16);
    out.write(memberId.getRawData(), 16);
    out.writeInt64(static_cast<juce::int64>(std::min(editCount.load(std::memory_order_relaxed), static_cast<juce::uint64>(std::numeric_limits<juce::int64>::max()))));
    for (int i = 0; i < 160; ++i)
    {
        out.writeInt(i);
        out.writeFloat(controls[static_cast<size_t>(i)]->get());
    }
    out.flush();
    const auto digest = juce::SHA256(state).getRawData();
    out.write(digest.getData(), digest.getSize());
}
void ProbeProcessor::setStateInformation(const void* data, int size)
{
    bool accepted = false;
    const auto before = identitySnapshot();
    const auto record = juce::ScopeGuard([&]
    {
        auto* result = new juce::DynamicObject;
        result->setProperty("accepted", accepted);
        result->setProperty("before", before);
        result->setProperty("after", identitySnapshot());
        probe::ProbeHostDiagnostics::record(runtimeId, "state.load", result);
    });
    constexpr int payloadBytes = 48 + 160 * 8;
    constexpr int stateBytes = payloadBytes + 32;
    if (data == nullptr || size != stateBytes)
        return;
    const auto digest = juce::SHA256(data, payloadBytes).getRawData();
    if (std::memcmp(digest.getData(), static_cast<const char*>(data) + payloadBytes, 32) != 0)
        return;
    juce::MemoryInputStream in(data, static_cast<size_t>(size), false);
    if (in.readInt() != 0x30505253 || in.readInt() != 1)
        return;
    std::array<juce::uint8, 16> group {}, member {};
    in.read(group.data(), 16);
    in.read(member.data(), 16);
    const auto edits = in.readInt64();
    if (edits < 0 || juce::Uuid(member.data()).isNull())
        return;
    std::array<float, 160> values {};
    std::array<bool, 160> seen {};
    for (int i = 0; i < 160; ++i)
    {
        const auto index = in.readInt();
        const auto value = in.readFloat();
        if (index < 0 || index >= 160 || seen[static_cast<size_t>(index)]
            || !std::isfinite(value) || value < 0.0f || value > 1.0f)
            return;
        seen[static_cast<size_t>(index)] = true;
        values[static_cast<size_t>(index)] = value;
    }

    // All bytes and fields have passed validation before publishing anything.
    // This mutex is used only by state save/load, never by processBlock/listeners.
    const juce::ScopedLock lock(stateLock);
    for (size_t i = 0; i < controls.size(); ++i)
        controls[i]->setValueNotifyingHost(values[i]);
    groupId = juce::Uuid(group.data());
    memberId = juce::Uuid(member.data());
    editCount.store(static_cast<juce::uint64>(edits), std::memory_order_relaxed);
    accepted = true;
}
bool ProbeProcessor::isBusesLayoutSupported(const BusesLayout& layout) const { return layout.getMainInputChannelSet().isDisabled() && (layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() || layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono()); }
juce::AudioProcessorEditor* ProbeProcessor::createEditor() { return new ProbeEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ProbeProcessor; }
