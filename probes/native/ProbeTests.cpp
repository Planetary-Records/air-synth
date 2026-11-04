#include "ProbeProcessor.h"
#include "ProbeHostDiagnostics.h"
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <map>
#include <cmath>
#include <iostream>
#include <limits>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace Steinberg;
struct Attributes : Vst::IAttributeList
{
    int refs = 1;
    uint32 lastStringBytes = 0;
    std::map<std::string, std::u16string> strings;
    std::map<std::string, int64> integers;
    tresult PLUGIN_API queryInterface(const TUID, void** out) override { *out = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }
    tresult PLUGIN_API setInt(AttrID, int64) override { return kNotImplemented; }
    tresult PLUGIN_API getInt(AttrID key, int64& value) override { auto i = integers.find(key); if (i == integers.end()) return kResultFalse; value = i->second; return kResultOk; }
    tresult PLUGIN_API setFloat(AttrID, double) override { return kNotImplemented; }
    tresult PLUGIN_API getFloat(AttrID, double&) override { return kResultFalse; }
    tresult PLUGIN_API setString(AttrID, const Vst::TChar*) override { return kNotImplemented; }
    tresult PLUGIN_API getString(AttrID key, Vst::TChar* out, uint32 bytes) override
    {
        lastStringBytes = bytes;
        auto i = strings.find(key); if (i == strings.end()) return kResultFalse;
        const auto count = std::min(i->second.size(), static_cast<size_t>(bytes / 2 - 1));
        std::copy_n(i->second.data(), count, out); out[count] = 0; return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID, const void*, uint32) override { return kNotImplemented; }
    tresult PLUGIN_API getBinary(AttrID, const void*&, uint32&) override { return kResultFalse; }
};
struct HostStream : IBStream, Vst::IStreamAttributes
{
    Attributes attributes;
    bool available = true, nullList = false;
    int refs = 1, reads = 0, writes = 0, seeks = 0;
    int64 cursor = 17;
    const std::string payload = "untouched host bytes";
    tresult PLUGIN_API queryInterface(const TUID iid, void** out) override
    {
        *out = nullptr;
        if (available && std::memcmp(iid, Vst::IStreamAttributes_iid, 16) == 0)
        { *out = static_cast<Vst::IStreamAttributes*>(this); addRef(); return kResultOk; }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }
    tresult PLUGIN_API read(void*, int32, int32*) override { ++reads; return kResultFalse; }
    tresult PLUGIN_API write(void*, int32, int32*) override { ++writes; return kResultFalse; }
    tresult PLUGIN_API seek(int64, int32, int64*) override { ++seeks; return kResultFalse; }
    tresult PLUGIN_API tell(int64* out) override { *out = cursor; return kResultOk; }
    tresult PLUGIN_API getFileName(Vst::String128 out) override { out[0] = u'x'; out[1] = 0; return kResultOk; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return nullList ? nullptr : &attributes; }
};
struct GestureCounter : juce::AudioProcessorParameter::Listener
{
    int values = 0, begins = 0, ends = 0;
    void parameterValueChanged(int, float) override { ++values; }
    void parameterGestureChanged(int, bool start) override { start ? ++begins : ++ends; }
};
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool trackAllocations = false;
thread_local int callbackAllocations = 0;
int __cdecl allocationHook(int kind, void*, size_t, int, long, const unsigned char*, int)
{
    if (trackAllocations && kind == _HOOK_ALLOC)
        ++callbackAllocations;
    return 1;
}
#endif
// Independent fixture for the documented M0 wire format. Reversed records catch
// accidental coupling of saved record order to host parameter indices.
juce::MemoryBlock fixture(bool reversed = false)
{
    juce::MemoryBlock state;
    juce::MemoryOutputStream out(state, false);
    out.writeInt(0x30505253);
    out.writeInt(1);
    const juce::Uuid group("00112233445566778899aabbccddeeff");
    const juce::Uuid member("ffeeddccbbaa99887766554433221100");
    out.write(group.getRawData(), 16);
    out.write(member.getRawData(), 16);
    out.writeInt64(42);
    for (int index = 0; index < 160; ++index)
    {
        const int id = reversed ? 159 - index : index;
        out.writeInt(id);
        out.writeFloat(id == 159 ? 0.75f : id == 128 ? 0.625f : 0.25f);
    }
    out.flush();
    const auto digest = juce::SHA256(state).getRawData();
    out.write(digest.getData(), digest.getSize());
    out.flush();
    return state;
}
void reseal(juce::MemoryBlock& state)
{
    const auto hash = juce::SHA256(state.getData(), state.getSize() - 32).getRawData();
    std::memcpy(static_cast<char*>(state.getData()) + state.getSize() - 32, hash.getData(), 32);
}
void putInt(juce::MemoryBlock& state, size_t offset, int value)
{
    juce::MemoryOutputStream out;
    out.writeInt(value);
    std::memcpy(static_cast<char*>(state.getData()) + offset, out.getData(), 4);
    reseal(state);
}
void putFloat(juce::MemoryBlock& state, size_t offset, float value)
{
    juce::MemoryOutputStream out;
    out.writeFloat(value);
    std::memcpy(static_cast<char*>(state.getData()) + offset, out.getData(), 4);
    reseal(state);
}
struct Transport : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying(playing);
        return p;
    }
};
class ProbeTests final : public juce::UnitTest
{
public:
    ProbeTests() : UnitTest("M0 native probe", "Probe") {}
    void runTest() override
    {
        beginTest("Optional host stream metadata, bounded Unicode and ownership without stream I/O");
        HostStream stream;
        stream.available = false;
        expectEquals(probe::observeState(&stream, false).fields["interface_status"].toString(), juce::String("unavailable"));
        stream.available = true; stream.nullList = true;
        expectEquals(probe::observeState(&stream, true).fields["list_status"].toString(), juce::String("null"));
        stream.nullList = false;
        auto observed = probe::observeState(&stream, false).fields;
        expectEquals(observed["state_type"]["status"].toString(), juce::String("query_failed"));
        stream.attributes.strings["StateType"] = u"Alien";
        stream.attributes.strings["FilePathString"] = u"C:/\u97f3\u697d/\u00e9.als";
        observed = probe::observeState(&stream, false).fields;
        expectEquals(observed["recognized"].toString(), juce::String("unknown"));
        expectEquals(observed["path"]["value"].toString(), juce::String::fromUTF8("C:/\xe9\x9f\xb3\xe6\xa5\xbd/\xc3\xa9.als"));
        stream.attributes.strings["StateType"] = u"Project";
        stream.attributes.strings["FilePathString"] = std::u16string(600, u'z');
        observed = probe::observeState(&stream, true).fields;
        expectEquals(observed["recognized"].toString(), juce::String("Project"));
        expectEquals(observed["path"]["value"].toString().length(), 512);
        expect(static_cast<bool>(observed["path"]["truncated"]));
        expectEquals(observed["direction"].toString(), juce::String("save"));
        expectEquals(static_cast<int>(stream.attributes.lastStringBytes), 1028);
        expectEquals(probe::observeState(nullptr, false).fields["interface_status"].toString(), juce::String("null_stream"));
        for (const auto* kind : { u"Default", u"TrackPreset" })
        {
            stream.attributes.strings["StateType"] = kind;
            expect(probe::observeState(&stream, false).fields["recognized"].toString() != "unknown");
        }
        stream.attributes.strings["StateType"] = u"";
        observed = probe::observeState(&stream, false).fields;
        expectEquals(observed["state_type"]["status"].toString(), juce::String("supplied"));
        expectEquals(observed["recognized"].toString(), juce::String("unknown"));
        stream.attributes.strings["FilePathString"] = std::u16string(511, u'z') + u"\U0001f3b5";
        observed = probe::observeState(&stream, false).fields;
        expectEquals(observed["path"]["value"].toString().length(), 511);
        expect(static_cast<bool>(observed["path"]["truncated"]));
        expectEquals(stream.refs, 1); expectEquals(stream.attributes.refs, 1);
        expectEquals(stream.reads + stream.writes + stream.seeks, 0);
        expectEquals(stream.cursor, int64(17)); expect(stream.payload == "untouched host bytes");
        beginTest("Channel metadata preserves optional query status and raw values");
        stream.attributes.strings["channel uid"] = u"track-\u03b1";
        stream.attributes.integers["channel runtime id"] = 99;
        const auto channel = probe::observeChannel(&stream.attributes).fields;
        expectEquals(channel["uid"]["value"].toString(), juce::String::fromUTF8("track-\xce\xb1"));
        expectEquals(static_cast<int>(channel["runtime_id"]["value"]), 99);
        expectEquals(channel["name"]["status"].toString(), juce::String("query_failed"));
        expectEquals(probe::observeChannel(nullptr).fields["list_status"].toString(), juce::String("null"));

        beginTest("Explicit test groups, runtime identity and native sound membership preservation");
        ProbeProcessor a, b;
        expect(a.runtimeIdentity() != b.runtimeIdentity());
        const auto member = b.identitySnapshot()["member"].toString();
        a.createTestGroup();
        const auto group = a.identitySnapshot()["group"].toString();
        expect(!juce::Uuid(group).isNull());
        b.joinTestGroup(juce::Uuid(group));
        expectEquals(b.identitySnapshot()["group"].toString(), group);
        expectEquals(b.identitySnapshot()["member"].toString(), member);
        b.clearTestGroup();
        expect(juce::Uuid(b.identitySnapshot()["group"].toString()).isNull());
        b.createTestGroup();
        const auto ownGroup = b.identitySnapshot()["group"].toString();
        a.getParameters()[159]->setValueNotifyingHost(0.875f);
        auto native = a.saveNativeSound();
        expectEquals(static_cast<int>(native.getSize()), 1320);
        expect(b.loadNativeSound(native.getData(), static_cast<int>(native.getSize())));
        expectEquals(b.getParameters()[159]->getValue(), 0.875f);
        expectEquals(b.identitySnapshot()["group"].toString(), ownGroup);
        expectEquals(b.identitySnapshot()["member"].toString(), member);
        juce::MemoryBlock hostState; a.getStateInformation(hostState);
        const auto runtimeBefore = b.runtimeIdentity();
        b.setStateInformation(hostState.getData(), static_cast<int>(hostState.getSize()));
        expect(b.runtimeIdentity() == runtimeBefore);
        expectEquals(b.identitySnapshot()["member"].toString(), a.identitySnapshot()["member"].toString());
        b.createTestGroup();

        beginTest("Native sound corrupt fields reject before any state or identity mutation");
        juce::MemoryBlock baseline;
        b.getStateInformation(baseline);
        auto rejectSound = [&](juce::MemoryBlock input)
        {
            expect(!b.loadNativeSound(input.getData(), static_cast<int>(input.getSize())));
            juce::MemoryBlock after; b.getStateInformation(after); expect(after == baseline);
        };
        auto corrupt = native; putInt(corrupt, 0, 0); rejectSound(corrupt);
        corrupt = native; putInt(corrupt, 4, 2); rejectSound(corrupt);
        corrupt = native; putInt(corrupt, 8, -1); rejectSound(corrupt);
        corrupt = native; putInt(corrupt, 8, 160); rejectSound(corrupt);
        corrupt = native; putInt(corrupt, 8 + 159 * 8, 0); rejectSound(corrupt);
        for (const auto value : { -0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
        { corrupt = native; putFloat(corrupt, 12, value); rejectSound(corrupt); }
        corrupt = native; corrupt.setSize(corrupt.getSize() - 1); rejectSound(corrupt);
        corrupt = native; corrupt.append("x", 1); rejectSound(corrupt);
        rejectSound({}); expect(!b.loadNativeSound(nullptr, 1320));

        beginTest("Configure test gestures exactly 129 controls and restores all values");
        GestureCounter gestures;
        for (auto* p : a.getParameters()) p->addListener(&gestures);
        const auto priorSound = a.saveNativeSound();
        a.configureTestControls();
        expectEquals(gestures.begins, 129); expectEquals(gestures.ends, 129);
        expectEquals(gestures.values, 258); expect(a.saveNativeSound() == priorSound);
        for (auto* p : a.getParameters()) p->removeListener(&gestures);
        auto badNative = native;
        if (!badNative.isEmpty()) static_cast<char*>(badNative.getData())[10] ^= 1;
        expect(!b.loadNativeSound(badNative.getData(), static_cast<int>(badNative.getSize())));
        expectEquals(b.getParameters()[159]->getValue(), 0.875f);

        beginTest("One stream observation per call and bounded process trace independent of editor");
        const auto before = static_cast<juce::int64>(probe::ProbeHostDiagnostics::snapshot()["sequence"]);
        probe::recordState(a, &stream, false);
        expectEquals(static_cast<juce::int64>(probe::ProbeHostDiagnostics::snapshot()["sequence"]), before + 1);
        for (int i = 0; i < 300; ++i) probe::ProbeHostDiagnostics::record(a.runtimeIdentity(), "test", {});
        const auto trace = probe::ProbeHostDiagnostics::snapshot();
        expectEquals(trace["records"].size(), 256);
        expect(static_cast<juce::int64>(trace["dropped"]) >= 45);
        expect(static_cast<int>(trace["pid"]) > 0);
        expect(trace["process_start"].toString().isNotEmpty());
        expectEquals(static_cast<juce::int64>(trace["dropped"]), static_cast<juce::int64>(trace["sequence"]) - 256);
        const auto& records = trace["records"];
        expectEquals(static_cast<juce::int64>(records[0]["sequence"]), static_cast<juce::int64>(trace["sequence"]) - 255);
        expectEquals(static_cast<juce::int64>(records[255]["sequence"]), static_cast<juce::int64>(trace["sequence"]));
        juce::Uuid destroyed;
        { ProbeProcessor transient; destroyed = transient.runtimeIdentity(); }
        const auto withDeath = probe::ProbeHostDiagnostics::snapshot()["records"];
        expectEquals(withDeath[withDeath.size() - 1]["kind"].toString(), juce::String("destroy"));
        expectEquals(withDeath[withDeath.size() - 1]["runtime"].toString(), destroyed.toString());

        beginTest("Existing MIDI stop routes, resource release and unrelated-channel handling");
        for (int mode = 0; mode < 5; ++mode)
        {
            ProbeProcessor tone; tone.prepareToPlay(48000, 256);
            juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 0); tone.processBlock(audio, midi);
            expect(audio.getMagnitude(0, 256) > 0.0f);
            midi.clear();
            if (mode == 0) midi.addEvent(juce::MidiMessage::midiStop(), 0);
            if (mode == 1) midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
            if (mode == 2) tone.releaseResources();
            if (mode == 3) midi.addEvent(juce::MidiMessage::allNotesOff(2), 0);
            if (mode == 4) midi.addEvent(juce::MidiMessage::noteOff(2, 69), 0);
            tone.processBlock(audio, midi);
            expect(mode < 3 ? audio.getMagnitude(0, 256) == 0.0f : audio.getMagnitude(0, 256) > 0.0f);
        }

        beginTest("160 fixed automatable IDs, manual editor, and instrument metadata");
        ProbeProcessor processor;
        const auto& params = processor.getParameters();
        expectEquals(params.size(), 160);
        expect(processor.acceptsMidi() && !processor.producesMidi());
        expect(processor.hasEditor());
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        expect(editor != nullptr);
        for (int i = 0; i < params.size(); ++i)
        {
            const auto* p = dynamic_cast<juce::AudioParameterFloat*>(params[i]);
            expect(p != nullptr);
            if (p != nullptr)
            {
                expectEquals(p->paramID, "probe.p" + juce::String(i).paddedLeft('0', 3));
                expectEquals(p->getName(64), p->paramID);
                expect(p->isAutomatable());
            }
        }

        beginTest("Saved high-index values and metadata follow IDs, independent of record order");
        const auto input = fixture(true);
        processor.setStateInformation(input.getData(), static_cast<int>(input.getSize()));
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        expect(saved == fixture(), "Roundtrip must preserve group/member/version/edit count and canonical ID ordering");
        if (params.size() == 160)
        {
            expectEquals(params[128]->getValue(), 0.625f);
            expectEquals(params[159]->getValue(), 0.75f);
            params[159]->setValueNotifyingHost(0.875f);
            processor.getStateInformation(saved);
            juce::MemoryInputStream stream(saved, false);
            stream.setPosition(40);
            expectEquals(stream.readInt64(), juce::int64(43));
            ProbeProcessor restored;
            restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
            juce::MemoryBlock again;
            restored.getStateInformation(again);
            expect(again == saved);
            expectEquals(restored.getParameters()[159]->getValue(), 0.875f);
        }

        beginTest("Every invalid state leaves all prior values and metadata unchanged");
        processor.getStateInformation(saved);
        auto reject = [&](juce::MemoryBlock bad)
        {
            processor.setStateInformation(bad.getData(), static_cast<int>(bad.getSize()));
            juce::MemoryBlock after;
            processor.getStateInformation(after);
            expect(after == saved, "Invalid state mutated live state");
        };
        auto bad = fixture(); putInt(bad, 4, 2); reject(bad);
        bad = fixture(); putInt(bad, 0, 0); reject(bad);
        bad = fixture(); putInt(bad, 48 + 159 * 8, 158); reject(bad);
        bad = fixture(); putInt(bad, 48, 160); reject(bad);
        for (float value : { -0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
        {
            bad = fixture(); putFloat(bad, 48 + 159 * 8 + 4, value); reject(bad);
        }
        bad = fixture(); putInt(bad, 44, -1); reject(bad);
        bad = fixture(); std::memset(static_cast<char*>(bad.getData()) + 24, 0, 16); reseal(bad); reject(bad);
        bad = fixture(); static_cast<char*>(bad.getData())[100] ^= 1; reject(bad);
        bad = fixture(); bad.setSize(bad.getSize() - 1); reject(bad);
        bad = fixture(); bad.append("x", 1); reject(bad);
        reject({});
        processor.setStateInformation(nullptr, 100);
        juce::MemoryBlock afterNull;
        processor.getStateInformation(afterNull);
        expect(afterNull == saved);

        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            beginTest("Sample-accurate finite note on/off, gain, and stop at " + juce::String(rate));
            ProbeProcessor synth;
            synth.prepareToPlay(rate, 256);
            juce::AudioBuffer<float> audio(2, 256);
            juce::MidiBuffer midi;
            synth.processBlock(audio, midi);
            expectEquals(audio.getMagnitude(0, 256), 0.0f);
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 17);
            midi.addEvent(juce::MidiMessage::noteOff(1, 69), 193);
            synth.processBlock(audio, midi);
            for (int i = 0; i < 256; ++i)
            {
                const float expected = i >= 17 && i < 193
                    ? 0.25f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 440.0 * (i - 17) / rate)) : 0.0f;
                expect(std::isfinite(audio.getSample(0, i)));
                expectWithinAbsoluteError(audio.getSample(0, i), expected, 0.00001f);
                expectEquals(audio.getSample(1, i), audio.getSample(0, i));
            }
            midi.clear(); synth.processBlock(audio, midi);
            expectEquals(audio.getMagnitude(0, 256), 0.0f);
            if (synth.getParameters().size() == 160)
                synth.getParameters()[0]->setValueNotifyingHost(0.5f);
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 0);
            synth.processBlock(audio, midi);
            expectWithinAbsoluteError(audio.getMagnitude(0, 256), 0.5f, 0.001f);
            midi.clear(); midi.addEvent(juce::MidiMessage::allSoundOff(1), 31);
            synth.processBlock(audio, midi);
            expect(audio.getMagnitude(0, 31) > 0.0f);
            expectEquals(audio.getMagnitude(31, 225), 0.0f);
            Transport transport;
            synth.setPlayHead(&transport);
            midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
            synth.processBlock(audio, midi);
            expect(audio.getMagnitude(0, 256) > 0.0f);
            transport.playing = false;
            midi.clear(); synth.processBlock(audio, midi);
            expectEquals(audio.getMagnitude(0, 256), 0.0f);
            synth.setPlayHead(nullptr);
        }
#if defined(_MSC_VER) && defined(_DEBUG)
        beginTest("Ignored SysEx does not allocate inside the audio callback");
        ProbeProcessor synth;
        synth.prepareToPlay(48000, 256);
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        std::array<char, 1024> payload {};
        midi.addEvent(juce::MidiMessage::createSysExMessage(payload.data(), static_cast<int>(payload.size())), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 17);
        const auto oldHook = _CrtSetAllocHook(allocationHook);
        callbackAllocations = 0;
        trackAllocations = true;
        synth.processBlock(audio, midi);
        trackAllocations = false;
        _CrtSetAllocHook(oldHook);
        expectEquals(callbackAllocations, 0);
        expect(audio.getMagnitude(17, 239) > 0.0f);
#endif
    }
};
ProbeTests tests;
}
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTestsInCategory("Probe", 1);
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult(i);
        std::cout << result->subcategoryName << ": " << result->passes << " passes, " << result->failures << " failures\n";
        for (int j = 0; j < juce::jmin(3, result->messages.size()); ++j)
            std::cout << "  " << result->messages[j] << '\n';
        failures += runner.getResult(i)->failures;
    }
    std::cout << "Probe failures: " << failures << std::endl;
    return failures == 0 ? 0 : 1;
}
