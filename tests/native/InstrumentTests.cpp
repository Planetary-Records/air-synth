#include "FrozenRackExamples.h"
#include "PreparedMakerSkins.h"
#include "IsolatedRackLibrary.h"
#include "InstrumentProcessor.h"
#include "RackLibrary.h"
#include "RackExamples.h"
#include "../../tools/native/RackPersonalityFixtures.h"
#include <juce_dsp/juce_dsp.h>
#include "RackPanel.h"
#include "RackStack.h"
#include "RackReview.h"
#include "MakerFace.h"
#include "ProgrammerUnit.h"
#include "SkinControls.h"
#include "StudioSkin.h"
#include "StudioServices.h"
#include "PostedClick.h"
#include "BlankInstance.h"
#include "OwnedFixture.h"
#include "CompanionWire.h"
#include "RackOrder.h"
#include "UserReach.h"
#include <atomic>
#include <juce_gui_extra/juce_gui_extra.h>
#include <cmath>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <thread>
#include <chrono>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack {
// Explicit historical melodic unit-fixture authority, sealed before normal request construction.
struct InstrumentAiTestAccess {
    static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p,const AiRequestOptions& options,std::optional<double> bpm={})
    try {
        if(options.policy.scope==ai::RequestScope::pattern)return airack::testing::beginAiRequest(p,options,bpm);
        p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
        std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
        if(factory.size()!=26||std::count(factory.begin(),factory.end(),tile)!=1)return {};
        std::erase(factory,tile);if(factory.size()!=25)return {};
        auto captured=std::make_shared<RackCatalogSnapshot>();
        {const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit=0;
            if(!p.tryProjectSnapshotOwned(*source,edit))return {};
            std::vector<RackDesign> embedded;if(source->sound.personalities)for(const auto& rack:source->sound.personalities->instances)embedded.push_back(rack.design);
            if(!captureRackCatalog(factory,embedded,{},*captured))return {};}
        return p.beginAiRequestWithCatalog(options,bpm,std::move(captured));
    }catch(const std::bad_alloc&){return {};}
    static std::unique_ptr<ai::LocalGenerationRequest> currentLocal(InstrumentProcessor& p){
        const std::lock_guard lock(p.restoreMutex);if(!p.aiLocal||!p.aiLocal->current||!p.aiPayloads->request||!p.aiLocalCurrentOwned())return {};
        auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=*p.aiPayloads->request;input->local=*p.aiLocal->current;return input;
    }
};
}

namespace
{

using namespace airack;
int checks = 0, failures = 0, sections = 0;
void check(bool good, const char* what, int line)
{ ++checks; if (!good) { ++failures; std::cout << "FAIL line " << line << ": " << what << '\n'; } }
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
void section(const char* name) { ++sections; std::cout << "SECTION " << name << '\n'; }
struct DiscardPhaseProbe;
DiscardPhaseProbe* discardPhaseProbe=nullptr;
struct DiscardPhaseProbe final:juce::ComponentListener,juce::MessageManager::LifetimeListener
{
    bool stopping=false,globalsStarted=false;
    int watched=0,deleted=0;
    DiscardPhaseProbe(){juce::MessageManager::addLifetimeListener(*this);discardPhaseProbe=this;}
    ~DiscardPhaseProbe() override{discardPhaseProbe=nullptr;juce::MessageManager::removeLifetimeListener(*this);}
    void messageManagerStarting() override{std::cout<<"DISCARD_PHASE manager_starting"<<std::endl;}
    void messageManagerStopping() override{stopping=true;std::cout<<"DISCARD_PHASE manager_stopping_before_globals"<<std::endl;}
    void watch(juce::Component& c){c.addComponentListener(this);++watched;}
    void componentBeingDeleted(juce::Component& c) override{++deleted;CHECK(stopping&&!globalsStarted);std::cout<<"DISCARD_PHASE component_deleting id="<<c.getComponentID()<<" manager_stopping="<<stopping<<std::endl;}
};
// Registered before the editor/skin, so reverse shutdown order deletes those globals before this marker.
struct DiscardGlobalsMarker final:juce::DeletedAtShutdown
{
    ~DiscardGlobalsMarker() override{std::cout<<"DISCARD_PHASE registered_before_editor_marker_deleted"<<std::endl;}
};
// Registered last, immediately before GUI shutdown, so it precedes the skin and every other shutdown global.
struct DiscardGlobalsBoundary final:juce::DeletedAtShutdown
{
    ~DiscardGlobalsBoundary() override{discardPhaseProbe->globalsStarted=true;CHECK(discardPhaseProbe->stopping&&discardPhaseProbe->watched==2&&discardPhaseProbe->deleted==2);std::cout<<"DISCARD_PHASE globals_begin retired_deleted="<<discardPhaseProbe->deleted<<std::endl;}
};
ParameterValues snapshot(InstrumentProcessor& p)
{ ParameterValues v {}; std::uint64_t r = 0; CHECK(p.tryParameterSnapshot(v, r)); return v; }
juce::MemoryBlock state(const ParameterValues& v)
{ juce::MemoryBlock b; CHECK(encodeSoundState(v, b)); return b; }
bool restore(InstrumentProcessor& p, const juce::MemoryBlock& b)
{ return p.restoreSoundState(b.getData(), static_cast<int>(b.getSize())); }
bool decodeHostValues(const juce::MemoryBlock& b, ParameterValues& values) { ProjectDocument d; if(!decodeProjectState(b.getData(),b.getSize(),d))return false; values=d.sound.values; return true; }
float peak(const juce::AudioBuffer<float>& b) { return b.getMagnitude(0, b.getNumSamples()); }
void note(juce::MidiBuffer& midi) { midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0); }
void registry()
{
    section("every_registered_control_is_stable"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; CHECK(blankInstance(p));
    CHECK(p.getParameters().size() == parameterCount);
    // Avoid dereferencing absent controls in the missing-behaviour RED stub.
    if (p.getParameters().size() != parameterCount) return;
    const auto descriptors = parameterRegistry();
    for (std::size_t i = 0; i < parameterCount; ++i)
    {
        auto& c = p.control(i); const auto& d = descriptors[i];
        CHECK(c.paramID == d.id); CHECK(c.getParameterIndex() == static_cast<int>(i));
        CHECK(c.isDiscrete() == (d.kind != ParameterKind::continuous));
        CHECK(c.isBoolean() == (d.kind == ParameterKind::toggle));
        CHECK(c.getLabel() == d.unit); CHECK(c.getValue() == c.getDefaultValue());
        CHECK(std::abs(c.getValueForText(c.getText(c.getDefaultValue(), 0)) - c.getDefaultValue()) < .001f);
    }
    CHECK(p.getParameters()[211] == &p.control(211));
    auto layout = p.getBusesLayout(); CHECK(p.isBusesLayoutSupported(layout));
    layout.outputBuses.set(0, juce::AudioChannelSet::mono()); CHECK(!p.isBusesLayoutSupported(layout));
}
void banksAndInvalid()
{
    section("high_index_and_inactive_banks_recall"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    auto v = defaultParameterValues(); v[211] = .875f; v[29] = .625f; v[121] = .75f;
    auto b = state(v); CHECK(restore(p, b)); CHECK(snapshot(p) == v);
    if (p.getParameters().size() != parameterCount) return;
    p.control(13).setValue(1); CHECK(snapshot(p)[29] == .625f); CHECK(snapshot(p)[211] == .875f);
    juce::MemoryBlock saved; p.getStateInformation(saved); CHECK(saved.getSize() == currentMinimumProjectStateBytes);
    InstrumentProcessor q{airack::testing::isolatedRackRuntime()}; CHECK(restore(q, saved)); CHECK(snapshot(p) == snapshot(q));
    section("invalid_automation_and_recall_preserve_sound");
    for (float f : {-.1f, 1.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    { const auto before = snapshot(p); p.control(211).setValue(f); p.control(28).setValue(f); CHECK(snapshot(p) == before); }
    p.control(28).setValue(.48f); CHECK(std::abs(p.control(28).getValue() - 1.f/3.f) < 1e-6f);
    auto before = snapshot(p); auto bad = saved; static_cast<char*>(bad.getData())[50] ^= 1;
    CHECK(!restore(p, bad)); CHECK(!p.restoreSoundState(nullptr, 1720)); CHECK(!p.restoreSoundState(saved.getData(), -1)); CHECK(snapshot(p) == before);
}
struct Playhead : juce::AudioPlayHead
{
    bool playing = false, available = true; std::optional<double> bpm;
    juce::Optional<PositionInfo> getPosition() const override
    { if (!available) return {}; PositionInfo p; p.setIsPlaying(playing); if (bpm) p.setBpm(*bpm); return p; }
};
void audioAndLifecycle()
{
    section("external_midi_without_playhead"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; CHECK(copperArcInstance(p)); juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
    p.prepareToPlay(48000, 256); note(midi); p.processBlock(audio, midi); CHECK(peak(audio) > 0);
    section("transport_stop_and_state_reset"); Playhead playhead; p.setPlayHead(&playhead);
    playhead.playing = true; midi.clear(); p.processBlock(audio, midi); CHECK(peak(audio) > 0);
    playhead.playing = false; p.processBlock(audio, midi); CHECK(peak(audio) == 0);
    note(midi); p.processBlock(audio, midi); CHECK(peak(audio) > 0); midi.clear(); p.processBlock(audio, midi); CHECK(peak(audio) > 0);
    auto b = state(defaultParameterValues()); CHECK(restore(p, b)); p.processBlock(audio, midi); CHECK(peak(audio) == 0);
    note(midi); p.processBlock(audio, midi); CHECK(peak(audio) > 0); p.control(0).setValue(.4f); midi.clear(); p.processBlock(audio, midi); CHECK(peak(audio) > 0);
    p.releaseResources(); p.processBlock(audio, midi); CHECK(peak(audio) == 0); p.prepareToPlay(48000, 256); p.processBlock(audio, midi); CHECK(peak(audio) == 0);
    note(midi); p.processBlock(audio, midi); midi.clear(); midi.addEvent(juce::MidiMessage::midiStop(), 0); p.processBlock(audio, midi); CHECK(peak(audio) == 0);
    { InstrumentProcessor retained, reference; CHECK(copperArcInstance(retained) && copperArcInstance(reference)); retained.prepareToPlay(48000,256); reference.prepareToPlay(48000,256); juce::AudioBuffer<float> expected(2,256); note(midi); retained.processBlock(audio,midi); reference.processBlock(expected,midi); midi.clear(); retained.prepareToPlay(0,256); retained.processBlock(audio,midi); reference.processBlock(expected,midi); bool equal=true; for(int c=0;c<2;++c)for(int i=0;i<256;++i)equal &= audio.getSample(c,i)==expected.getSample(c,i); CHECK(equal); }
    p.prepareToPlay(48000, 256); playhead.available = false; note(midi); p.processBlock(audio, midi); CHECK(peak(audio) > 0);
    midi.clear(); p.processBlock(audio, midi); CHECK(peak(audio) > 0);
    p.setPlayHead(nullptr);
}
void invalidEditsPreserveAudio()
{
    section("invalid_edits_preserve_actual_audio_and_core_bounds"); InstrumentProcessor p, reference; CHECK(copperArcInstance(p) && copperArcInstance(reference));
    p.prepareToPlay(48000, 256); reference.prepareToPlay(48000, 256);
    juce::AudioBuffer<float> a(2, 256), b(2, 256); juce::MidiBuffer midi;
    note(midi); p.processBlock(a, midi); reference.processBlock(b, midi); midi.clear();
    auto damaged = state(defaultParameterValues()); static_cast<char*>(damaged.getData())[0] ^= 1;
    CHECK(!restore(p, damaged));
    for (std::size_t i = 0; i < parameterCount; ++i) p.control(i).setValue(std::numeric_limits<float>::quiet_NaN());
    p.processBlock(a, midi); reference.processBlock(b, midi);
    bool equal = true;
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 256; ++i) equal &= a.getSample(c, i) == b.getSample(c, i);
    CHECK(equal); CHECK(peak(a) > 0);
    juce::AudioBuffer<float> mono(1, 256), tooLarge(2, 65537);
    p.processBlock(mono, midi); CHECK(peak(mono) == 0); p.processBlock(tooLarge, midi); CHECK(peak(tooLarge) == 0);
    p.processBlock(a, midi); reference.processBlock(b, midi); equal = true;
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 256; ++i) equal &= a.getSample(c, i) == b.getSample(c, i);
    CHECK(equal);
}
void setPhysical(InstrumentProcessor& p, const juce::String& id, float value)
{
    const auto registry = parameterRegistry();
    for (std::size_t i = 0; i < registry.size(); ++i)
        if (registry[i].id == id) { p.control(i).setValue(p.control(i).convertTo0to1(value)); return; }
    CHECK(false);
}
void releasePatch(InstrumentProcessor& p, bool wavetable, float sustain, bool filterOnly = false)
{
    setPhysical(p, "global.master_level", .7f);
    {setPhysical(p,"output.gain_db",0);setPhysical(p,"output.protection",0);}
    setPhysical(p, "synth.1.engine", wavetable ? 1.f : 0.f);
    const juce::String bank = wavetable ? "synth.1.wavetable." : "synth.1.analog.";
    setPhysical(p, bank + "osc.1.level", 1);
    setPhysical(p, bank + "osc.2.level", 0);
    if (wavetable) setPhysical(p, bank + "osc.1.position", .8f);
    setPhysical(p, bank + "filter.cutoff", 900);
    setPhysical(p, bank + "filter.resonance", .2f);
    setPhysical(p, bank + "amp.attack", filterOnly ? 0.f : .06f);
    setPhysical(p, bank + "amp.decay", .12f);
    setPhysical(p, bank + "amp.sustain", filterOnly ? 1.f : sustain);
    setPhysical(p, bank + "amp.release", .12f);
    setPhysical(p, bank + "filter_env.attack", .06f);
    setPhysical(p, bank + "filter_env.decay", .12f);
    setPhysical(p, bank + "filter_env.sustain", sustain);
    setPhysical(p, bank + "filter_env.release", .12f);
    setPhysical(p, bank + "filter_env.octaves", 4);
}
struct ReleaseAudio { std::vector<float> processor, reference; };
void selectKernel(InstrumentProcessor& processor, AlgorithmRevision kernel)
{
    if (kernel == modernSynthAlgorithmRevision) return; // Fresh instances already run it.
    juce::MemoryBlock legacy; CHECK(encodeSoundState(SoundDocument{}, legacy));
    CHECK(processor.restoreSoundState(legacy.getData(), static_cast<int>(legacy.getSize())));
}
SynthPatch enginePatch(InstrumentProcessor& processor, AlgorithmRevision kernel)
{
    SynthPatch patch; CHECK(makeSynthPatch(snapshot(processor), patch));
    for (auto& layer : patch.layers) layer.algorithmRevision = kernel;
    return patch;
}
ReleaseAudio renderEarlyRelease(AlgorithmRevision kernel, bool wavetable, float sustain, int noteOffSample, int block, bool edit, bool filterOnly = false)
{
    InstrumentProcessor processor{airack::testing::isolatedRackRuntime()}; CHECK(copperArcInstance(processor)); selectKernel(processor, kernel);
    releasePatch(processor, wavetable, sustain, filterOnly);
    auto patch = enginePatch(processor, kernel);
    SynthEngine reference; CHECK(reference.setPatch(patch)); CHECK(reference.prepare(48000, 512));
    processor.prepareToPlay(48000, 512);
    const int end = noteOffSample + 5760 + 1024;
    const int editSample = ((noteOffSample + 511) / 512 + 1) * 512;
    ReleaseAudio result {std::vector<float>(end), std::vector<float>(end)};
    for (int start = 0; start < end; start += block)
    {
        if (edit && start == editSample)
        {
            setPhysical(processor, "global.master_level", .55f);
            setPhysical(processor, "synth.1.pan", .25f);
            const juce::String bank = wavetable ? "synth.1.wavetable." : "synth.1.analog.";
            setPhysical(processor, bank + "osc.1.level", .8f);
            setPhysical(processor, bank + "filter.cutoff", 1200);
            patch = enginePatch(processor, kernel);
            CHECK(reference.setPatch(patch)); // Publish only the real edit, never every block.
        }
        const int size = std::min(block, end - start);
        juce::AudioBuffer<float> actual(2, size), expected(2, size); juce::MidiBuffer midi;
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
        if (noteOffSample >= start && noteOffSample < start + size)
            midi.addEvent(juce::MidiMessage::noteOff(1, 69), noteOffSample - start);
        processor.processBlock(actual, midi); reference.render(expected, midi);
        std::copy_n(actual.getReadPointer(0), size, result.processor.data() + start);
        std::copy_n(expected.getReadPointer(0), size, result.reference.data() + start);
    }
    return result;
}
double maxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double result = 0;
    for (std::size_t i = 0; i < a.size(); ++i) result = std::max(result, std::abs(double(a[i]) - b[i]));
    return result;
}
double windowEnergy(const std::vector<float>& a, int start, int count)
{
    double result = 0;
    for (int i = start; i < start + count; ++i) result += double(a[i]) * a[i];
    return result / count;
}
void earlyReleasePublication()
{
    section("early_release_survives_block_publication_and_unrelated_edits");
    for (auto kernel : {legacyAlgorithmRevision, modernSynthAlgorithmRevision})
    for (bool wavetable : {false, true}) for (float sustain : {0.f, .35f})
        for (int noteOffSample : {1237, 4333}) for (bool edit : {false, true})
        {
            const auto small = renderEarlyRelease(kernel, wavetable, sustain, noteOffSample, 64, edit);
            const auto large = renderEarlyRelease(kernel, wavetable, sustain, noteOffSample, 512, edit);
            const auto smallDiff = maxDifference(small.processor, small.reference);
            const auto largeDiff = maxDifference(large.processor, large.reference);
            const auto partitionDiff = maxDifference(small.processor, large.processor);
            CHECK(smallDiff <= 1e-7); CHECK(largeDiff <= 1e-7); CHECK(partitionDiff <= 1e-7);
            // Note-off occurs before sustain. The .12-second release must not be cut
            // short and must settle after .12 seconds, at either partition. A legacy
            // release takes its full time from any level, so it still sounds near .09
            // seconds; a modern release from a lower level ends proportionally sooner.
            const int sounding = kernel == legacyAlgorithmRevision ? 4000 : 1000;
            for (const auto* audio : {&small.processor, &large.processor, &small.reference})
            {
                CHECK(windowEnergy(*audio, noteOffSample + sounding, 256) > 1e-10);
                CHECK(windowEnergy(*audio, noteOffSample + 5888, 256) == 0);
            }
            std::cout << "Release kernel=" << int(kernel) << " engine=" << wavetable << " sustain=" << sustain << " off=" << noteOffSample
                      << " edit=" << edit << " reference64=" << smallDiff << " reference512=" << largeDiff
                      << " partition=" << partitionDiff << '\n';
        }
    section("filter_release_is_preserved_independently_of_amplitude_release");
    for (auto kernel : {legacyAlgorithmRevision, modernSynthAlgorithmRevision})
    for (bool wavetable : {false, true}) for (bool edit : {false, true})
    {
        // Amp is already at sustain1: its repeated slope calculation would be
        // harmless. Only the filter's early release differs in this fixture.
        const auto small = renderEarlyRelease(kernel, wavetable, 0, 1237, 64, edit, true);
        const auto large = renderEarlyRelease(kernel, wavetable, 0, 1237, 512, edit, true);
        CHECK(maxDifference(small.processor, small.reference) <= 1e-7);
        CHECK(maxDifference(large.processor, large.reference) <= 1e-7);
        CHECK(maxDifference(small.processor, large.processor) <= 1e-7);
    }
}
void envelopePreparation()
{
    section("prepare_and_reprepare_recalculate_envelope_rates_in_seconds");
    for (auto kernel : {legacyAlgorithmRevision, modernSynthAlgorithmRevision})
    for (bool reprepare : {false, true}) for (double rate : {44100.0, 96000.0})
    {
        InstrumentProcessor p, constant;
        for (auto* processor : {&p, &constant})
        {
            CHECK(copperArcInstance(*processor));
            selectKernel(*processor, kernel);
            releasePatch(*processor, false, 1);
            setPhysical(*processor, "synth.1.analog.amp.attack", .1f);
            setPhysical(*processor, "synth.1.analog.filter_env.octaves", 0);
            if (reprepare) processor->prepareToPlay(48000, 512);
        }
        setPhysical(constant, "synth.1.analog.amp.attack", 0);
        p.prepareToPlay(rate, 512); constant.prepareToPlay(rate, 512);
        const int end = int(rate * .11);
        std::vector<float> envelope(end), unity(end);
        for (int start = 0; start < end; start += 512)
        {
            const int size = std::min(512, end - start);
            juce::AudioBuffer<float> a(2, size), b(2, size); juce::MidiBuffer midi;
            if (start == 0) note(midi);
            p.processBlock(a, midi); constant.processBlock(b, midi);
            std::copy_n(a.getReadPointer(0), size, envelope.data() + start);
            std::copy_n(b.getReadPointer(0), size, unity.data() + start);
        }
        for (double seconds : {.025, .075})
        {
            const int start = int(rate * seconds);
            double ratio = 0; int used = 0;
            for (int i = start; i < start + 32; ++i) if (std::abs(unity[i]) > 1e-5f)
            { ratio += envelope[i] / unity[i]; ++used; }
            CHECK(used > 0);
            // A legacy attack rises linearly; a modern attack is convex, aimed .3
            // above full level so that it arrives there at the attack time.
            const double expected = kernel == legacyAlgorithmRevision ? seconds / .1 : 1.3 * (1 - std::pow(.3 / 1.3, seconds / .1));
            CHECK(std::abs(ratio / std::max(used, 1) - expected) < .012);
        }
    }
}
void concurrentRecall()
{
    section("concurrent_recall_never_tears"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    auto a = defaultParameterValues(), b = a;
    for (std::size_t i = 0; i < parameterCount; ++i) if (parameterRegistry()[i].kind == ParameterKind::continuous) { a[i] = .25f; b[i] = .75f; }
    const auto sa = state(a), sb = state(b); CHECK(restore(p, sa));
    std::atomic<bool> finished {false}, writerGood {true};
    std::thread writer([&] { for (int i = 0; i < 1000; ++i) if (!restore(p, i % 2 ? sa : sb)) writerGood = false; finished = true; });
    int accepted = 0, rejected = 0; bool whole = true, preserves = true;
    while (!finished)
    {
        auto v = a; std::uint64_t rev = 123;
        if (p.tryParameterSnapshot(v, rev)) { ++accepted; whole &= (v == a || v == b) && (rev % 2 == 0); }
        else { ++rejected; preserves &= v == a && rev == 123; }
    }
    writer.join(); CHECK(writerGood); CHECK(whole); CHECK(preserves); CHECK(accepted + rejected > 0);
    std::cout << "Snapshots accepted=" << accepted << " rejected=" << rejected << '\n';
}
struct SavingListener : juce::AudioProcessorParameter::Listener
{
    InstrumentProcessor& processor;
    std::atomic<bool> good {true};
    std::atomic<int> calls {0}, gestures {0};
    explicit SavingListener(InstrumentProcessor& p) : processor(p) {}
    void parameterValueChanged(int, float) override
    {
        juce::MemoryBlock saved; processor.getStateInformation(saved); ParameterValues values {};
        if (!decodeHostValues(saved, values)) good = false;
        if (values[0] != values[211]) good = false;
        ++calls;
    }
    void parameterGestureChanged(int, bool) override { ++gestures; }
};
void listenerAndRenderConcurrency()
{
    section("recall_notifications_allow_serialization_and_bounded_render"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    auto a = defaultParameterValues(), b = a; a[0] = a[211] = .25f; b[0] = b[211] = .75f;
    auto sa = state(a), sb = state(b); CHECK(restore(p, sa));
    SavingListener listener(p); p.control(0).addListener(&listener); p.prepareToPlay(48000, 64);
    std::atomic<bool> finished {false}, good {true};
    std::thread first([&] { for (int i = 0; i < 200; ++i) if (!restore(p, sa)) good = false; });
    std::thread second([&] { for (int i = 0; i < 200; ++i) if (!restore(p, sb)) good = false; finished = true; });
    juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer midi;
    int renders = 0; double longest = 0;
    do
    {
        note(midi); auto start = std::chrono::steady_clock::now(); p.processBlock(audio, midi);
        longest = std::max(longest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        for (int i = 0; i < 64; ++i) good = good && std::isfinite(audio.getSample(0, i));
        ++renders;
    } while (!finished);
    first.join(); second.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    p.control(0).removeListener(&listener);
    CHECK(good); CHECK(listener.good); CHECK(listener.calls > 0 && listener.calls <= 401); CHECK(listener.gestures == 0);
    CHECK(longest < 1); CHECK(renders > 0);
    std::cout << "Concurrent render blocks=" << renders << " longest_seconds=" << longest
              << " coalesced_serialization_callbacks=" << listener.calls << '\n';
}
juce::Component* findComponent(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren()) if (auto* found = findComponent(*child, id)) return found;
    return nullptr;
}
void dispatch() { juce::MessageManager::getInstance()->runDispatchLoopUntil(30); }
void click(juce::Button& button, int settleMs = 30) { CHECK(clickPosted(button)); juce::MessageManager::getInstance()->runDispatchLoopUntil(settleMs); }
void postedClickBehindSlowMessage()
{
    section("posted_click_behind_slow_message");
    CHECK(clickRunsBehindSlowMessage([](juce::Button& button) { click(button); }));
}
// ADVANCED's PROGRAMMER, and its knob for a parameter.
studio::ProgrammerUnit* programmer(juce::Component& editor) { return dynamic_cast<studio::ProgrammerUnit*>(findComponent(editor,"pages")); }
studio::ProgrammerKnob* knob(juce::Component& editor,const juce::String& id) { return dynamic_cast<studio::ProgrammerKnob*>(findComponent(editor,id)); }
juce::MouseEvent mouse(juce::Component& c,juce::ModifierKeys mods)
{
    const auto t=juce::Time::getCurrentTime();const auto at=c.getLocalBounds().getCentre().toFloat();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,
        juce::MouseInputSource::defaultTiltY,&c,&c,t,at,t,1,false);
}
// A knob's drag on its own mouse handlers: the press opens its gesture and the release ends it.
void grab(juce::Component& c) { c.mouseDown(mouse(c,juce::ModifierKeys::leftButtonModifier)); }
void letGo(juce::Component& c) { c.mouseUp(mouse(c,{})); }
// Runs the knob's menu item with this label; false when the menu has none.
bool choose(studio::ProgrammerKnob& k,const juce::String& label)
{
    for(auto& [text,run]:k.menu())if(text==label){run();return true;}
    return false;
}
template<typename Predicate,typename Description>
bool waitForUi(const char* label,const Predicate& ready,const Description& describe)
{
    // Timer intervals are scheduling requests, not completion barriers. Pump only
    // the real message loop, returning as soon as the required UI state arrives.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!ready()&&std::chrono::steady_clock::now()<deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    if(ready())return true;
    std::cout<<"UI deadline (1000 ms): "<<label<<" "<<describe()<<'\n';
    return false;
}
struct RecallListener : juce::AudioProcessorParameter::Listener
{
    std::function<void()> action;
    bool fired = false;
    int gestures = 0;
    void parameterValueChanged(int, float) override
    { if (!fired) { fired = true; action(); } }
    void parameterGestureChanged(int, bool) override { ++gestures; }
};
void recallNotificationConvergence()
{
    section("nested_and_overlapping_recall_converge_actual_attachments");
    for (bool listenerRegisteredFirst : {false, true})
        for (int mode = 0; mode < 4; ++mode)
        {
            InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
            auto a = defaultParameterValues(), b = a;
            a[0] = .25f; a[211] = .125f;
            b[0] = .75f; b[13] = 1; b[121] = .8f; b[211] = .875f;
            const auto sa = state(a), sb = state(b);
            RecallListener listener;
            if (listenerRegisteredFirst) p.control(0).addListener(&listener);
            std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
            CHECK(editor != nullptr); if (!editor) return;
            if (!listenerRegisteredFirst) p.control(0).addListener(&listener);
            juce::Slider high;
            juce::SliderParameterAttachment highAttachment(p.control(211), high);
            std::thread publication;
            std::atomic<bool> writerGood {false};
            bool callbackGood = false;
            listener.action = [&]
            {
                // Reentrant save must remain valid before and after publication.
                juce::MemoryBlock saved; p.getStateInformation(saved); ParameterValues savedValues {};
                callbackGood = decodeHostValues(saved, savedValues) && savedValues == a;
                if (mode == 1)
                {
                    publication = std::thread([&] { writerGood = restore(p, sb); });
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    bool observed = false;
                    do
                    {
                        ParameterValues values {}; std::uint64_t serial;
                        observed = p.tryParameterSnapshot(values, serial) && values == b;
                        if (!observed) std::this_thread::yield();
                    } while (!observed && std::chrono::steady_clock::now() < deadline);
                    callbackGood &= observed;
                }
                else if (mode == 3)
                {
                    // Independent host automation does not increment recall revision.
                    for (std::size_t index : {std::size_t(0), std::size_t(13), std::size_t(121), std::size_t(211)})
                        p.control(index).setValueNotifyingHost(b[index]);
                    writerGood = true;
                }
                else
                {
                    writerGood = restore(p, sb);
                    // A nested message loop may drain B before A's callback unwinds.
                    if (mode == 2) dispatch();
                }
                p.getStateInformation(saved);
                callbackGood &= decodeHostValues(saved, savedValues) && savedValues == b;
            };
            CHECK(restore(p, sa)); dispatch();
            if (publication.joinable()) publication.join();
            dispatch();
            CHECK(listener.fired); CHECK(callbackGood); CHECK(writerGood); CHECK(listener.gestures == 0);
            CHECK(snapshot(p) == b);
            auto* master = dynamic_cast<juce::Slider*>(findComponent(*editor, "global.master_level"));
            auto* engine = knob(*editor, "synth.1.engine");
            auto* position = dynamic_cast<juce::Slider*>(findComponent(*editor, "synth.1.wavetable.osc.1.position"));
            CHECK(master != nullptr); CHECK(engine != nullptr); CHECK(position != nullptr);
            if (master) CHECK(std::abs(master->getValue() - .75) < 1e-6);
            if (engine) CHECK(engine->getValue() == 1);
            if (position) CHECK(std::abs(position->getValue() - .8) < 1e-6);
            CHECK(std::abs(high.getValue() - 6) < 1e-6);
            std::cout << "Recall case listener_registered_first=" << listenerRegisteredFirst << " mode=" << mode
                      << " parameter=" << p.control(0).getValue() << " editor=" << (master ? master->getValue() : -1) << '\n';
            p.control(0).removeListener(&listener);
        }
}
void pendingRecallDestruction()
{
    section("pending_recall_refresh_cancels_on_message_thread_destruction");
    CHECK(juce::MessageManager::getInstance()->isThisTheMessageThread());
    int callbacks = 0; RecallListener listener; listener.action = [&] { ++callbacks; };
    {
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; p.control(0).addListener(&listener);
        auto values = defaultParameterValues(); values[0] = .75f; const auto saved = state(values);
        CHECK(restore(p, saved)); CHECK(callbacks == 0);
    }
    dispatch(); CHECK(callbacks == 0);
}
void editorCheck(const juce::String& prefix)
{
    section("native_editor_binding_layout_and_snapshot"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor()); CHECK(editor != nullptr); if (!editor) return;
    editor->setVisible(true); dispatch();
    auto* engine = knob(*editor, "synth.1.engine");
    auto* master = knob(*editor, "global.master_level");
    auto* unit = programmer(*editor);
    CHECK(engine != nullptr); CHECK(master != nullptr); CHECK(unit != nullptr);
    for (std::size_t i = 28; i < 51; ++i)
    { auto* c = findComponent(*editor, parameterRegistry()[i].id); CHECK(c != nullptr); if (c) CHECK(!c->getBounds().isEmpty() && c->isVisible()); }
    std::array<juce::Component*,parameterCount> retained{};
    for(std::size_t n=0;n<retained.size();++n){retained[n]=findComponent(*editor,parameterRegistry()[n].id);CHECK(retained[n]);}
    if(auto* lever=dynamic_cast<juce::Button*>(findComponent(*editor,"rack.toggle"))){click(*lever);click(*lever);for(std::size_t n=0;n<retained.size();++n)CHECK(findComponent(*editor,parameterRegistry()[n].id)==retained[n]);if(auto* advanced=dynamic_cast<juce::Button*>(findComponent(*editor,"rack.advanced")))click(*advanced);else dispatch();CHECK(findComponent(*editor,"pages")->isVisible());CHECK(lever->getToggleState());}else CHECK(false);
    p.control(0).setValueNotifyingHost(.8f); dispatch(); if (master) CHECK(std::abs(master->getValue() - .8) < 1e-5);
    if (master) { grab(*master); master->setValue(.3, juce::sendNotificationSync); letGo(*master); CHECK(std::abs(p.control(0).getValue() - .3f) < 1e-6); }
    if (unit) { unit->showPage("synth1"); unit->showSubpage("analog"); }
    auto saveImage = [&](const juce::String& suffix)
    {
        if (prefix.isEmpty()) return;
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true);
        juce::File file(prefix + suffix + ".png"); auto out = file.createOutputStream();
        CHECK(image.isValid() && out != nullptr); if (out) CHECK(juce::PNGImageFormat().writeImageToStream(image, *out));
    };
    saveImage("-analog");
    if (engine) { engine->setValue(1, juce::sendNotificationSync); dispatch(); CHECK(p.control(13).getValue() == 1); }
    for (std::size_t i = 120; i < 143; ++i) CHECK(findComponent(*editor, parameterRegistry()[i].id) != nullptr);
    // The shown sub-page stays when the engine changes; WAVETABLE shows the bank turned on.
    if (unit) { CHECK(unit->subpage() == "analog"); unit->showSubpage("wavetable"); }
    saveImage("-wavetable");
    if (unit) { unit->showPage("synth4"); dispatch(); }
    p.control(25).setValueNotifyingHost(1); dispatch();
    auto* high = dynamic_cast<juce::Slider*>(findComponent(*editor, "synth.4.wavetable.filter_env.octaves")); CHECK(high != nullptr);
    auto v = defaultParameterValues(); v[25] = 1; v[211] = .875f; auto b = state(v); CHECK(restore(p, b)); dispatch();
    high = dynamic_cast<juce::Slider*>(findComponent(*editor, "synth.4.wavetable.filter_env.octaves"));
    CHECK(high != nullptr); if (high) CHECK(std::abs(high->getValue() - 6) < .0001);
}
// A recall that drops Copper Arc retires its rack unit, which is deleted on a later turn; the host may close the editor
// and delete the processor before that turn. Reading the deleted processor crashes or hangs here.
void retiredRackOutlivesProcessor()
{
    section("retired_rack_outlives_editor_and_processor");
    auto p = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor()); CHECK(editor != nullptr); if (!editor) return;
    editor->setVisible(true); dispatch();
    CHECK(restore(*p, state(defaultParameterValues()))); dispatch();
    editor.reset(); p.reset(); dispatch(); dispatch();
}
// Queue retirement without pumping it, then follow the host's editor-before-processor close order.
void queuedRackOutlivesProcessor(bool deliver)
{
    section("queued_rack_retirement_outlives_processor");
    auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(blankInstance(*p));
    auto empty=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*empty,revision));
    CHECK(copperArcInstance(*p));
    auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)return;
    auto* view=findComponent(*editor,"rack.stack");
    auto* stack=view?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr;CHECK(stack);if(!stack)return;
    auto copper=std::make_unique<SoundDocument>();CHECK(p->trySoundSnapshot(*copper,revision));stack->refresh(*copper);
    CHECK(copper->personalities&&!copper->personalities->instances.empty());if(!copper->personalities||copper->personalities->instances.empty())return;
    const auto retired=copper->personalities->instances.front().id;CHECK(stack->unit(retired));
    if(!deliver&&discardPhaseProbe)if(auto* unit=stack->unit(retired)){CHECK(unit->face()==nullptr);discardPhaseProbe->watch(*unit);discardPhaseProbe->watch(unit->row);}
    juce::MemoryBlock wire;CHECK(encodeSoundState(*empty,wire));CHECK(restore(*p,wire));
    stack->refresh(*empty);CHECK(!stack->unit(retired));
    editor.reset();p.reset();
    if(deliver){dispatch();dispatch();}
    std::cout<<"LIFETIME queued retirement completed\n";
}
void callbackEditorOutlivesProcessor(bool deliver)
{
    section("callback_editor_retirement_outlives_processor");
    for(int phase=0;phase<3;++phase){
        std::cout<<"LIFETIME phase="<<phase<<" construct\n";
        auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(blankInstance(*p));
        auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)continue;
        auto* control=knob(*editor,"global.master_level");CHECK(control);if(!control)continue;
        struct Events final:juce::AudioProcessorParameter::Listener{
            std::unique_ptr<juce::AudioProcessorEditor>& editor;int phase,begins=0,ends=0;bool closed=false;
            Events(std::unique_ptr<juce::AudioProcessorEditor>& e,int n):editor(e),phase(n){}
            void close(){if(!closed){closed=true;editor.reset();}}
            void parameterValueChanged(int,float) override{if(phase==1)close();}
            void parameterGestureChanged(int,bool starting) override{if(starting){++begins;if(phase==0)close();}else{++ends;if(phase==2)close();}}
        } events(editor,phase);
        std::cout<<"LIFETIME phase="<<phase<<" edit\n";
        p->control(0).addListener(&events);control->stepBy(-1);p->control(0).removeListener(&events);
        std::cout<<"LIFETIME phase="<<phase<<" edit_returned\n";
        CHECK(!editor);CHECK(events.begins==1&&events.ends==1);
        std::cout<<"LIFETIME phase="<<phase<<" destroy_processor\n";
        p.reset();std::cout<<"LIFETIME phase="<<phase<<" queue\n";if(deliver){dispatch();dispatch();}
        std::cout<<"LIFETIME phase="<<phase<<" completed\n";
    }
}
enum class RetirementOrigin { maker, row, programmer, processor };
void makerCallbackOutlivesProcessor(const std::shared_ptr<PreparedTestMakerSkins>& fixtures,bool deliver,RetirementOrigin origin=RetirementOrigin::maker)
{
    section("maker_callback_retirement_outlives_processor");
    for(int phase=0;phase<3;++phase){
        std::cout<<"LIFETIME phase="<<phase<<" construct\n";
        auto p=std::make_unique<InstrumentProcessor>(preparedTestRackRuntime());CHECK(blankInstance(*p));
        auto empty=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*empty,revision));
        auto sound=std::make_unique<SoundDocument>(rackPersonalityFixtures::fixture(2,0,true,[](bool ok){CHECK(ok);}));
        juce::MemoryBlock wire;CHECK(encodeSoundState(*sound,wire));CHECK(restore(*p,wire));
        auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)continue;
        auto* view=findComponent(*editor,"rack.stack");auto* stack=view?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr;CHECK(stack);if(!stack)continue;
        const auto& thread=sound->personalities->instances.back().design;const auto owner=studio::makerFaceFor(thread);
        const bool exactOwner=owner&&thread==airack::test::frozenExampleRackDesigns()[2]&&std::find(fixtures->owners.begin(),fixtures->owners.end(),owner)!=fixtures->owners.end();
        CHECK(exactOwner);std::cout<<"LIFETIME thread_owner="<<exactOwner<<" phase="<<phase<<'\n';if(!exactOwner)continue;
        stack->refresh(*sound);auto* unit=stack->unit(sound->personalities->instances.back().id);auto* face=unit?unit->face():nullptr;
        CHECK(face!=nullptr);if(!face)continue;CHECK(!face->face.sliders.empty());if(face->face.sliders.empty())continue;
        auto* control=face->face.sliders.front().get();auto index=control->element.paramIndex;
        juce::Slider* rowControl=nullptr;
        if(origin==RetirementOrigin::row){
            const auto fields=unit->row.parameters();CHECK(!fields.empty());if(fields.empty())continue;
            index=fields.front();rowControl=dynamic_cast<juce::Slider*>(findComponent(unit->row,"rack.focus.0"));
            CHECK(rowControl);if(!rowControl)continue;
            const auto mapped=face->parameters();CHECK(std::find(mapped.begin(),mapped.end(),index)!=mapped.end());
        }
        studio::house::Host host;std::unique_ptr<studio::ProgrammerLook> look;std::unique_ptr<studio::ProgrammerKnob> foreign;
        if(origin==RetirementOrigin::programmer){look=std::make_unique<studio::ProgrammerLook>(*studio::studioSkin());foreign=std::make_unique<studio::ProgrammerKnob>(*p,*look,host,index);}
        struct Events final:juce::AudioProcessorParameter::Listener{
            RackStack& stack;const SoundDocument& empty;int phase,begins=0,ends=0;bool retired=false;
            Events(RackStack& s,const SoundDocument& d,int n):stack(s),empty(d),phase(n){}
            void retire(){if(!retired){retired=true;stack.refresh(empty);}}
            void parameterValueChanged(int,float) override{if(phase==1)retire();}
            void parameterGestureChanged(int,bool starting) override{if(starting){++begins;if(phase==0)retire();}else{++ends;if(phase==2)retire();}}
        } events(*stack,*empty,phase);
        p->control(index).addListener(&events);
        if(origin==RetirementOrigin::processor){
            CHECK(p->beginParameterGesture(index));
            const auto before=p->control(index).getValue();CHECK(p->setParameterGestureValue(index,before<.99f?before+.01f:before-.01f));
            CHECK(p->endParameterGesture(index));
        }else if(foreign){
            foreign->stepBy(foreign->getValue()<foreign->getMaximum()?1:-1);
        }else{
            auto* slider=rowControl?rowControl:control;
            if(slider->onDragStart)slider->onDragStart();
            const auto before=p->control(index).getValue();
            if(slider->onValueChange)slider->setValue(rowControl?p->control(index).convertFrom0to1(before<.99f?before+.01f:before-.01f):slider->getValue()+.01,juce::sendNotificationSync);
            if(slider->onDragEnd)slider->onDragEnd();
        }
        p->control(index).removeListener(&events);
        std::cout<<"LIFETIME origin="<<int(origin)<<" phase="<<phase<<" begins="<<events.begins<<" ends="<<events.ends<<'\n';
        CHECK(events.retired);CHECK(events.begins==1&&events.ends==1);
        foreign.reset();
        editor.reset();p.reset();if(deliver){dispatch();dispatch();}
        std::cout<<"LIFETIME phase="<<phase<<" completed\n";
    }
}
void directHeldRetirement()
{
    section("direct_value_notification_retires_idle_held_controls");
    for(const bool row:{false,true}){
        auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(blankInstance(*p));
        auto empty=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*empty,revision));
        auto sound=std::make_unique<SoundDocument>(rackPersonalityFixtures::fixture(2,0,true,[](bool ok){CHECK(ok);}));
        juce::MemoryBlock wire;CHECK(encodeSoundState(*sound,wire));CHECK(restore(*p,wire));
        auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)continue;
        auto* view=findComponent(*editor,"rack.stack");auto* stack=view?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr;CHECK(stack);if(!stack)continue;
        stack->refresh(*sound);auto* unit=stack->unit(sound->personalities->instances.back().id);auto* face=unit?unit->face():nullptr;
        CHECK(face&&!face->face.sliders.empty());if(!face||face->face.sliders.empty())continue;
        juce::Slider* slider=face->face.sliders.front().get();auto index=face->face.sliders.front()->element.paramIndex;
        if(row){const auto fields=unit->row.parameters();CHECK(!fields.empty());if(fields.empty())continue;index=fields.front();slider=dynamic_cast<juce::Slider*>(findComponent(unit->row,"rack.focus.0"));}
        CHECK(slider&&slider->onDragStart);if(!slider||!slider->onDragStart)continue;
        struct Events final:juce::AudioProcessorParameter::Listener{
            RackStack& stack;const SoundDocument& empty;int begins=0,ends=0;bool retired=false;
            Events(RackStack& s,const SoundDocument& d):stack(s),empty(d){}
            void parameterValueChanged(int,float) override{if(!retired){retired=true;stack.refresh(empty);}}
            void parameterGestureChanged(int,bool start) override{++(start?begins:ends);}
        } events(*stack,*empty);
        p->control(index).addListener(&events);slider->onDragStart();CHECK(events.begins==1&&events.ends==0);
        const auto before=p->control(index).getValue();CHECK(p->setParameterGestureValue(index,before<.99f?before+.01f:before-.01f));
        std::cout<<"LIFETIME direct_held_row="<<row<<" begins="<<events.begins<<" ends="<<events.ends<<'\n';
        CHECK(events.retired);CHECK(events.begins==1&&events.ends==1);
        p->control(index).removeListener(&events);CHECK(p->beginParameterGesture(index));CHECK(p->endParameterGesture(index));
        editor.reset();p.reset();
    }
}
void activeRetirementRejectedPost()
{
    section("active_notification_retirement_rejected_post_keeps_owner");
    // Removing the outer input guard must expose premature deletion after its synchronous callback.
    for(const auto input:{"callback","key","legend","house-double-click","row-double-click"}){
    auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(blankInstance(*p));
    auto empty=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*empty,revision));
    const bool legend=juce::String(input)=="legend",row=juce::String(input)=="row-double-click";
    auto sound=std::make_unique<SoundDocument>();
    if(legend){
        RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[3];op.newInstance=juce::Uuid("d83dab6a1c4e48c183c480d9212ca601");
        op.destination={RackRole::fx,masterChain,0};op.allocation={{0,0}};
        RackDifference difference;const auto status=makeRackCandidate(*empty,op,SoundEditOrigin::manual,*sound,difference);
        CHECK(status==RackCandidateStatus::ready);if(status!=RackCandidateStatus::ready)return;
        CHECK(validateSoundDocument(*sound));
    }else *sound=rackPersonalityFixtures::fixture(2,0,true,[](bool ok){CHECK(ok);});
    CHECK(sound->personalities&&!sound->personalities->instances.empty());
    if(!sound->personalities||sound->personalities->instances.empty())return;
    juce::MemoryBlock wire;CHECK(encodeSoundState(*sound,wire));CHECK(restore(*p,wire));
    auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)return;
    auto* view=findComponent(*editor,"rack.stack");auto* stack=view?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr;CHECK(stack);if(!stack)return;
    stack->refresh(*sound);auto* unit=stack->unit(sound->personalities->instances.back().id);auto* face=unit?unit->face():nullptr;
    CHECK(face&&!face->face.sliders.empty());if(!face||face->face.sliders.empty())return;
    auto* house=face->face.sliders.front().get();
    if(legend){
        face->setOpen(true);house=face->revealed().slider<studio::house::Knob>("note");
        CHECK(house&&house->element.legendClick);if(!house)return;
    }
    juce::Slider* slider=house;auto index=house->element.paramIndex;
    if(row){const auto fields=unit->row.parameters();CHECK(!fields.empty());if(fields.empty())return;index=fields.front();slider=dynamic_cast<juce::Slider*>(findComponent(unit->row,"rack.focus.0"));}
    CHECK(slider);if(!slider)return;
    juce::Component::SafePointer<RackStackUnit> retiredUnit(unit);juce::Component::SafePointer<studio::MakerFace> retiredFace(face);
    struct Events final:juce::AudioProcessorParameter::Listener{
        RackStack& stack;const SoundDocument& empty;juce::Component::SafePointer<studio::MakerFace>& face;int begins=0,ends=0;bool retired=false;
        Events(RackStack& s,const SoundDocument& d,juce::Component::SafePointer<studio::MakerFace>& f):stack(s),empty(d),face(f){}
        void parameterValueChanged(int,float) override{}
        void parameterGestureChanged(int,bool start) override{if(start){++begins;if(!retired){retired=true;stack.refresh(empty);CHECK(face!=nullptr);}}else ++ends;}
    } events(*stack,*empty,retiredFace);
    auto* manager=juce::MessageManager::getInstance();manager->stopDispatchLoop();CHECK(manager->hasStopMessageBeenSent());
    bool posted=false;CHECK(!juce::MessageManager::callAsync([&posted]{posted=true;}));CHECK(!posted);
    bool callbackReturned=false;
    if(juce::String(input)!="callback"){
        auto& callback=(juce::String(input)=="key"||legend)?slider->onValueChange:slider->onDragStart;
        const auto original=callback;CHECK(original);
        const auto continuation=[&]{callbackReturned=true;CHECK(retiredUnit!=nullptr&&retiredFace!=nullptr);};
        callback=[original,continuation]{const auto call=original;const auto after=continuation;call();after();};
    }
    p->control(index).addListener(&events);
    if(juce::String(input)=="callback")slider->onDragStart();
    else if(juce::String(input)=="key")CHECK(slider->keyPressed(juce::KeyPress(slider->getValue()<slider->getMaximum()?juce::KeyPress::endKey:juce::KeyPress::homeKey)));
    else if(legend){
        auto* knob=dynamic_cast<studio::house::Knob*>(house);CHECK(knob);if(!knob)return;
        const auto& positions=knob->element.positions;
        const auto position=std::find_if(positions.begin(),positions.end(),[&](const auto& v){return !v.legendRect.isEmpty()&&v.value!=knob->getValue();});
        CHECK(position!=positions.end());if(position==positions.end())return;
        const auto at=position->legendRect.getCentre()-knob->getPosition().toFloat();
        knob->mouseDown(mouse(*knob,juce::ModifierKeys::leftButtonModifier).withNewPosition(at));
        knob->mouseUp(mouse(*knob,{}).withNewPosition(at));
    }else{
        slider->setDoubleClickReturnValue(true,slider->getValue()<slider->getMaximum()?slider->getMaximum():slider->getMinimum());
        slider->mouseDoubleClick(mouse(*slider,{}));
    }
    p->control(index).removeListener(&events);
    std::cout<<"LIFETIME active_rejected_post input="<<input<<" begins="<<events.begins<<" ends="<<events.ends<<'\n';
    CHECK(juce::String(input)=="callback"||callbackReturned);
    CHECK(events.retired);CHECK(events.begins==1&&events.ends==1);CHECK(retiredUnit==nullptr&&retiredFace==nullptr);
    editor.reset();p.reset();CHECK(!posted);
    }
}
void sliderAccessibilityRetirement()
{
    section("accessible_slider_edits_keep_actual_owners_until_complete_setter");
    // Losing the accessibility guard must fail the post-inner-callback owner assertion.
    for(const auto origin:{"house","row","programmer"})for(const bool text:{false,true}){
        auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(blankInstance(*p));
        auto empty=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*empty,revision));
        auto sound=std::make_unique<SoundDocument>(rackPersonalityFixtures::fixture(2,0,true,[](bool ok){CHECK(ok);}));
        juce::MemoryBlock wire;CHECK(encodeSoundState(*sound,wire));CHECK(restore(*p,wire));
        auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)return;
        editor->addToDesktop(0);editor->setVisible(true);
        auto* view=findComponent(*editor,"rack.stack");auto* stack=view?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr;CHECK(stack);if(!stack)return;
        stack->refresh(*sound);auto* unit=stack->unit(sound->personalities->instances.back().id);auto* face=unit?unit->face():nullptr;
        CHECK(face&&!face->face.sliders.empty());if(!face||face->face.sliders.empty())return;
        const bool row=juce::String(origin)=="row",programmerEdit=juce::String(origin)=="programmer";
        juce::Slider* slider=face->face.sliders.front().get();auto index=face->face.sliders.front()->element.paramIndex;
        if(row){const auto fields=unit->row.parameters();CHECK(!fields.empty());if(fields.empty())return;index=fields.front();slider=dynamic_cast<juce::Slider*>(findComponent(unit->row,"rack.focus.0"));}
        if(programmerEdit){slider=knob(*editor,"global.master_level");index=0;}
        CHECK(slider);if(!slider)return;
        CHECK(slider->getWindowHandle()!=nullptr);CHECK(slider->isAccessible());
        if(slider->getWindowHandle()==nullptr||!slider->isAccessible())return;
        auto* handler=slider->getAccessibilityHandler();CHECK(handler);if(!handler)return;
        auto* value=handler->getValueInterface();CHECK(value);if(!value)return;
        CHECK(handler->getRole()==juce::AccessibilityRole::slider);
        CHECK(handler->getTitle()==slider->getTitle());CHECK(handler->getDescription()==slider->getDescription());
        CHECK(handler->getHelp()==slider->getTooltip());CHECK(handler->getCurrentState().isFocusable());
        CHECK(handler->getCurrentState().isFocused()==handler->hasFocus(false));
        for(const auto action:{juce::AccessibilityActionType::press,juce::AccessibilityActionType::toggle,juce::AccessibilityActionType::focus,juce::AccessibilityActionType::showMenu})CHECK(!handler->getActions().contains(action));
        CHECK(!value->isReadOnly());CHECK(value->getCurrentValue()==slider->getValue());
        CHECK(value->getCurrentValueAsString()==slider->getTextFromValue(slider->getValue()));
        const auto range=value->getRange();CHECK(range.isValid());
        CHECK(range.getMinimumValue()==slider->getMinimum());CHECK(range.getMaximumValue()==slider->getMaximum());
        CHECK(range.getInterval()==(slider->getInterval()!=0?slider->getInterval():(slider->getMaximum()-slider->getMinimum())*.01));
        if(programmerEdit){
            CHECK(range.getMinimumValue()==0&&range.getMaximumValue()==1&&range.getInterval()==.01);
            auto* stepped=knob(*editor,"synth.1.engine");CHECK(stepped);if(!stepped)return;
            const auto stepRange=stepped->getAccessibilityHandler()->getValueInterface()->getRange();
            CHECK(stepRange.getMinimumValue()==0&&stepRange.getMaximumValue()==1&&stepRange.getInterval()==1);
        }
        juce::Component::SafePointer<RackStackUnit> retiredUnit(unit);juce::Component::SafePointer<studio::MakerFace> retiredFace(face);
        juce::Component::SafePointer<juce::Slider> retiredSlider(slider);
        struct Events final:juce::AudioProcessorParameter::Listener{
            RackStack& stack;const SoundDocument& empty;std::unique_ptr<juce::AudioProcessorEditor>& editor;bool closeEditor;int begins=0,ends=0;bool retired=false;
            Events(RackStack& s,const SoundDocument& d,std::unique_ptr<juce::AudioProcessorEditor>& e,bool close):stack(s),empty(d),editor(e),closeEditor(close){}
            void parameterValueChanged(int,float) override{}
            void parameterGestureChanged(int,bool start) override{if(start){++begins;if(!retired){retired=true;if(closeEditor)editor.reset();else stack.refresh(empty);}}else ++ends;}
        } events(*stack,*empty,editor,programmerEdit);
        auto target=slider->getValue()<slider->getMaximum()?slider->getMaximum():slider->getMinimum();
        value->setValue(target);CHECK(slider->getValue()==target);
        CHECK(std::abs(p->control(index).getValue()-p->control(index).convertTo0to1(float(target)))<1e-6f);
        if(programmerEdit){
            target=target==slider->getMaximum()?slider->getMinimum():slider->getMaximum();
            value->setValueAsString(slider->getTextFromValue(target));CHECK(slider->getValue()==target);
        }
        // A nonnumeric token proves the string setter retains Slider's conversion hook.
        const auto token=juce::String("accessible endpoint");bool converted=false;
        slider->valueFromTextFunction=[&](const juce::String& typed){converted=true;CHECK(typed==token);CHECK(retiredSlider!=nullptr);return target;};
        target=target==slider->getMaximum()?slider->getMinimum():slider->getMaximum();
        value->setValueAsString(token);CHECK(converted);CHECK(slider->getValue()==target);
        CHECK(std::abs(p->control(index).getValue()-p->control(index).convertTo0to1(float(target)))<1e-6f);
        converted=false;target=target==slider->getMaximum()?slider->getMinimum():slider->getMaximum();
        auto* manager=juce::MessageManager::getInstance();manager->stopDispatchLoop();CHECK(manager->hasStopMessageBeenSent());
        bool posted=false;CHECK(!juce::MessageManager::callAsync([&posted]{posted=true;}));CHECK(!posted);
        bool callbackReturned=false,dragEnded=false;
        auto& callback=programmerEdit?slider->onValueChange:slider->onDragStart;const auto original=callback;CHECK(original);
        const auto continuation=[&]{callbackReturned=true;CHECK(retiredUnit!=nullptr&&retiredFace!=nullptr&&retiredSlider!=nullptr);};
        callback=[original,continuation]{const auto call=original;const auto after=continuation;call();after();};
        const auto originalEnd=slider->onDragEnd;
        slider->onDragEnd=[&,originalEnd]{const auto call=originalEnd;dragEnded=true;CHECK(retiredSlider!=nullptr);if(call)call();};
        p->control(index).addListener(&events);
        if(text)value->setValueAsString(token);else value->setValue(target);
        p->control(index).removeListener(&events);
        std::cout<<"LIFETIME accessible origin="<<origin<<" text="<<text<<" begins="<<events.begins<<" ends="<<events.ends<<'\n';
        CHECK(converted==text);CHECK(callbackReturned);CHECK(events.retired);CHECK(events.begins==1&&events.ends==1);
        // House/row retirement clears the drag callback; Programmer's native drag still ends before deletion.
        CHECK(!programmerEdit||dragEnded);
        CHECK(retiredSlider==nullptr&&retiredUnit==nullptr&&retiredFace==nullptr);
        editor.reset();p.reset();CHECK(!posted);
    }
}
void idleRowGestureOutlivesProcessor(bool deliver)
{
    section("idle_row_gesture_retires_after_notifying_main_frame");
    auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));
    auto sound=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*sound,revision));
    CHECK(sound->personalities&&!sound->personalities->instances.empty());if(!sound->personalities||sound->personalities->instances.empty())return;
    auto editor=std::unique_ptr<juce::AudioProcessorEditor>(p->createEditor());CHECK(editor);if(!editor)return;
    auto* view=findComponent(*editor,"rack.stack");auto* stack=view?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr;CHECK(stack);if(!stack)return;
    stack->refresh(*sound);auto* unit=stack->unit(sound->personalities->instances.front().id);CHECK(unit);if(!unit)return;
    auto* slider=dynamic_cast<juce::Slider*>(findComponent(unit->row,"rack.focus.0"));
    auto* drawer=dynamic_cast<RackPanelComponent*>(findComponent(*editor,"rack.drawer"));CHECK(slider&&drawer);if(!slider||!drawer)return;
    const auto& instance=sound->personalities->instances.front();CHECK(!instance.design.panel.controls.empty());if(instance.design.panel.controls.empty())return;
    const auto& focused=instance.design.panel.controls.front();
    const auto binding=rackUnitBinding(instance,focused.unit);CHECK(binding);if(!binding)return;
    const auto module=std::find_if(instance.design.units.begin(),instance.design.units.end(),[&](const auto& u){return u.id==focused.unit;});
    CHECK(module!=instance.design.units.end());if(module==instance.design.units.end())return;
    const auto field=rackParameterIndex(module->module,binding->slot,focused.key);CHECK(field);if(!field)return;
    struct Events final:juce::AudioProcessorParameter::Listener{
        std::unique_ptr<juce::AudioProcessorEditor>& editor;int begins=0,ends=0;bool closed=false;
        explicit Events(std::unique_ptr<juce::AudioProcessorEditor>& e):editor(e){}
        void parameterValueChanged(int,float) override{if(!closed){closed=true;editor.reset();}}
        void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
    } events(editor);
    p->control(*field).addListener(&events);slider->onDragStart();CHECK(events.begins==1&&events.ends==0);
    auto call=drawer->editorCallbackFrame();const auto before=p->control(*field).getValue();
    CHECK(p->setParameterGestureValue(*field,before<.99f?before+.01f:before-.01f));CHECK(!editor);CHECK(events.ends==0);
    call.reset();CHECK(events.ends==1);p->control(*field).removeListener(&events);
    p.reset();if(deliver){dispatch();dispatch();}std::cout<<"LIFETIME idle held row completed\n";
}
std::size_t idx(const juce::String& id)
{
    const auto r = parameterRegistry();
    for (std::size_t i = 0; i < r.size(); ++i) if (r[i].id == id) return i;
    CHECK(false); return 0;
}
void physical(SoundDocument& d, const juce::String& id, float value)
{
    const auto i = idx(id); const auto& c = parameterRegistry()[i];
    d.values[i] = juce::NormalisableRange<float>(c.minimum,c.maximum,0,c.skew).convertTo0to1(value);
}
SoundDocument document(InstrumentProcessor& p, std::uint64_t* revision = nullptr)
{
    SoundDocument d; std::uint64_t r = 0; CHECK(p.trySoundSnapshot(d,r)); if (revision) *revision = r; return d;
}
std::unique_ptr<SoundDocument> ownedDocument(InstrumentProcessor& p)
{
    auto d=std::make_unique<SoundDocument>();std::uint64_t r=0;CHECK(p.trySoundSnapshot(*d,r));return d;
}
bool documentMatches(InstrumentProcessor& p,const SoundDocument& expected)
{
    return document(p)==expected;
}
juce::String describeMetadata(InstrumentProcessor& p)
{
    SoundDocument d;std::uint64_t revision=0,reset=0;
    if(!p.trySoundSnapshot(d,revision,reset))return "committed snapshot unavailable; ";
    return "committed reset="+juce::String(static_cast<juce::int64>(reset))+" edit="+juce::String(static_cast<juce::int64>(revision))
        +" macro1="+juce::String(d.macros[0].targetKey)+"/"+juce::String(d.macros[0].amount)
        +" effect1 kind/owner="+juce::String(int(d.layout.effects[0].kind))+"/"+juce::String(d.layout.effects[0].chain)
        +" synth4 lock="+juce::String(int(d.synthLocks[3]))+" macro3="+juce::String(d.macros[2].targetKey)+"; ";
}
bool edit(InstrumentProcessor& p, const SoundDocument& d)
{ std::uint64_t r; document(p,&r); return p.applySoundEdit(d,SoundEditOrigin::manual,r); }
juce::MemoryBlock state(const SoundDocument& d)
{ juce::MemoryBlock b; CHECK(encodeSoundState(d,b)); return b; }
SoundDocument fullDocument()
{
    SoundDocument d;
    d.layout.synthUse.fill(SlotUse::present); d.layout.synthOrder = {0,1,2,3}; d.layout.synthCount=4;
    for (int i=0;i<4;++i) { physical(d,"synth."+juce::String(i+1)+".enabled",1); physical(d,"synth."+juce::String(i+1)+".engine",float(i%2)); }
    for (int i=0;i<8;++i) {
        RackLayout next; RackEdit e {RackEditKind::addEffect,noSlot,static_cast<std::uint8_t>(i%5),0,static_cast<EffectKind>(i%4),false};
        CHECK(editRackLayout(d.layout,e,next)==RackEditResult::applied); d.layout=next;
    }
    for(int rack=1;rack<=4;++rack) for(int route=1;route<=16;++route) {
        const auto prefix="synth."+juce::String(rack)+".modulation."+juce::String(route)+".";
        physical(d,prefix+"enabled",1); physical(d,prefix+"source",float(3+route%2)); physical(d,prefix+"target",1); physical(d,prefix+"amount",.001f);
    }
    d.macros[0]={parameterRegistry()[idx("synth.1.level")].stateKey,-.25f}; d.values[1]=.5f;
    d.controlLocks[211]=true; d.synthLocks[2]=true; d.effectLocks[6]=true;
    return d;
}
std::vector<float> renderDocument(const SoundDocument& d, std::optional<double> bpm = {}, int samples=4096)
{
    auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture; CHECK(restore(p,state(d))); Playhead head; head.bpm=bpm; p.setPlayHead(&head); p.prepareToPlay(8000,256);
    std::vector<float> result(static_cast<std::size_t>(samples));
    juce::AudioBuffer<float> b(2,256); juce::MidiBuffer m;
    for (int i=0;i<samples;i+=256) { if(i==0) note(m); else m.clear(); p.processBlock(b,m); std::copy_n(b.getReadPointer(0),256,result.data()+i); }
    p.setPlayHead(nullptr); return result;
}
void menuServiceLifetime()
{
    section("preset_menu_owns_only_its_modal_and_delivers_once");
    auto services=studio::defaultStudioServices();auto target=std::make_unique<juce::Component>();
    target->setBounds(0,0,100,30);target->addToDesktop(0);target->setVisible(true);
    int callbacks=0,selected=-1;
    services.chooseMenu(target.get(),{"One"},[&](int value){++callbacks;selected=value;});
    juce::Component::SafePointer<juce::Component> menu=juce::Component::getCurrentlyModalComponent();CHECK(menu&&menu->isCurrentlyModal());
    juce::Component unrelated;unrelated.enterModalState(false);CHECK(unrelated.isCurrentlyModal());
    target.reset();dispatch();
    CHECK(!menu);CHECK(unrelated.isCurrentlyModal());CHECK(juce::Component::getNumCurrentlyModalComponents()==1);CHECK(callbacks==0&&selected==-1);
    unrelated.exitModalState(0);dispatch();CHECK(juce::Component::getNumCurrentlyModalComponents()==0);
    target=std::make_unique<juce::Component>();target->setBounds(0,0,100,30);target->addToDesktop(0);target->setVisible(true);
    services.chooseMenu(target.get(),{"One"},[&](int value){++callbacks;selected=value;});
    menu=juce::Component::getCurrentlyModalComponent();CHECK(menu&&menu->isCurrentlyModal());if(menu)menu->exitModalState(1);dispatch();
    CHECK(!menu&&callbacks==1&&selected==1);
    services.chooseMenu(target.get(),{"One"},[&](int value){++callbacks;selected=value;});
    CHECK(juce::PopupMenu::dismissAllActiveMenus());dispatch();CHECK(callbacks==2&&selected==0);
    target.reset();dispatch();CHECK(callbacks==2&&juce::Component::getNumCurrentlyModalComponents()==0);
}
void presetMenuCancellation(InstrumentProcessor& p,std::unique_ptr<juce::AudioProcessorEditor>& e)
{
    section("native_chooser_cancellation_and_all_scroll_pages_reachable");
    const auto click=[&]{auto* button=dynamic_cast<juce::Button*>(findComponent(*e,"preset.browse"));CHECK(button);if(button)::click(*button,1);};
    auto chooserOpen=[]{return juce::ModalComponentManager::getInstance()->getNumModalComponents()>0;};
    auto beforeChooserFixture=airack::testing::fixtureValue([&]{return document(p);});const auto& beforeChooser=*beforeChooserFixture;click();
    CHECK(waitForUi("the preset chooser opens",chooserOpen,[]{return juce::String("no modal component");}));
    juce::ModalComponentManager::getInstance()->cancelAllModalComponents();dispatch();dispatch();
    CHECK(documentMatches(p,beforeChooser));
    auto* cancelled=dynamic_cast<juce::Label*>(findComponent(*e,"status"));CHECK(cancelled&&cancelled->getText().containsIgnoreCase("cancelled"));
    click();CHECK(waitForUi("the preset chooser opens again",chooserOpen,[]{return juce::String("no modal component");}));
    const auto closeBegin=std::chrono::steady_clock::now();e.reset();
    dispatch();CHECK(juce::ModalComponentManager::getInstance()->getNumModalComponents()==0);
    CHECK(documentMatches(p,beforeChooser));
    std::cout<<"PRESET_MENU_LIFETIME close_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-closeBegin).count()<<" modal_count="<<juce::ModalComponentManager::getInstance()->getNumModalComponents()<<" dispatch_ms=30\n";
}
void completeEditorCheck(const juce::String& prefix)
{
    const auto editorProgressStart = std::chrono::steady_clock::now();
    const auto editorProgress = [&](const char* stage, std::size_t nextIndex, int cycle = -1) {
        std::cout << "COMPLETE_EDITOR_PROGRESS stage=" << stage
            << " elapsed_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-editorProgressStart).count()
            << " next_index=" << nextIndex << " cycle=" << cycle << " checks=" << checks << " failures=" << failures << std::endl;
    };
    section("complete_native_components_gestures_and_exact_undo");
    editorProgress("processor_create_begin", 0);
    auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture;
    editorProgress("processor_create_end", 0);
    editorProgress("blank_begin", 0);
    CHECK(blankInstance(p));
    editorProgress("blank_end", 0);
    editorProgress("initial_editor_create_begin", 0);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    editorProgress("initial_editor_create_end", 0);
    // Immediate snapshot comparisons use one owned scratch document. Named
    // before/after fixtures below still take independent copies; none of these
    // immediate expressions retains a snapshot reference across another read.
    const auto readDocument = &document;
    auto documentScratch = std::make_unique<SoundDocument>();
    const auto document = [&](InstrumentProcessor& source) -> const SoundDocument& {
        *documentScratch = readDocument(source,nullptr);
        return *documentScratch;
    };
    editorProgress("initial_visibility_dispatch_begin", 0);
    e->setVisible(true); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    editorProgress("initial_visibility_dispatch_end", 0);
    auto click = [&](const juce::String& id) {
        auto* c=dynamic_cast<juce::Button*>(findComponent(*e,id)); CHECK(c!=nullptr);
        if(c)::click(*c,1);
    };
    int present=0;
    for(std::size_t i=0;i<parameterCount;++i)
    {
        if(i%128==0)editorProgress("control_loop_next", i);
        const auto& d=parameterRegistry()[i]; auto* c=findComponent(*e,d.id);
        CHECK(c!=nullptr); if(!c)continue; ++present;
        CHECK(!c->getBounds().isEmpty()); CHECK(c->getName()==d.name);
        // Each control is a knob, whose menu offers its lock first.
        auto* k=dynamic_cast<studio::ProgrammerKnob*>(c); CHECK(k!=nullptr); if(!k)continue;
        CHECK(k->menu().front().first=="Lock");
        if(i>=252&&i<572)continue;
        // Retained wet-EQ fields become editable in their matching shape. Set
        // that fixture before the independent snapshot, then restore it below.
        std::optional<std::pair<std::size_t,float>> retainedPostShape;
        if(i>=spaceReverbParameterStart&&i<spaceReverbParameterStart+8*spaceReverbParametersPerSlot){
            const auto local=(i-spaceReverbParameterStart)%spaceReverbParametersPerSlot,field=local%8;
            if(local>=48&&field>=4&&!k->isEnabled()){
                const auto shapeIndex=i-field+2;auto& shapeParameter=p.control(shapeIndex);
                const auto originalShape=shapeParameter.getValue();
                const auto compatibleShape=shapeParameter.convertTo0to1(field==6?1.f:field==7?3.f:0.f);
                if(originalShape!=compatibleShape){retainedPostShape=std::pair{shapeIndex,originalShape};shapeParameter.setValueNotifyingHost(compatibleShape);}
                CHECK(waitForUi("Space wet-EQ shape makes the target control active",[&]{return k->isEnabled();},
                    [&]{return d.id+" shape="+juce::String(shapeParameter.getValue())+" enabled="+juce::String(int(k->isEnabled()));}));
            }
        }
        auto beforeFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& before = *beforeFixture; auto& parameter=p.control(i);
        RecallListener listener; listener.action=[]{}; parameter.addListener(&listener);
        // A drag is one gesture, as a stepped knob's change of detent is.
        if(k->steps()==0) {
            grab(*k);
            k->setValue(parameter.convertFrom0to1(.75f),juce::sendNotificationSync);
            k->setValue(parameter.convertFrom0to1(.45f),juce::sendNotificationSync);
            letGo(*k);
            CHECK(std::abs(parameter.getValue()-.45f)<1e-5f);
        } else k->setValue(k->getValue()==k->getMinimum()?k->getMinimum()+1:k->getMinimum(),juce::sendNotificationSync);
        CHECK(listener.gestures==2); parameter.removeListener(&listener);
        auto afterFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& after = *afterFixture; CHECK(after!=before);
        CHECK(p.undoSoundEdit()); CHECK(documentMatches(p,before));
        CHECK(p.redoSoundEdit()); CHECK(documentMatches(p,after));
        CHECK(p.undoSoundEdit());
        CHECK(waitForUi("undone knob",[&]{return std::abs(parameter.convertTo0to1(static_cast<float>(k->getValue()))-before.values[i])<1e-5f;},
                        [&]{return juce::String(d.id)+" shows "+juce::String(k->getValue());}));
        if(k->steps()==0) {
            parameter.setValueNotifyingHost(.63f);juce::MessageManager::getInstance()->runDispatchLoopUntil(1);CHECK(std::abs(parameter.convertTo0to1(static_cast<float>(k->getValue()))-.63f)<1e-5f);
            parameter.setValueNotifyingHost(before.values[i]);juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        }
        if(retainedPostShape){
            auto& shapeParameter=p.control(retainedPostShape->first);shapeParameter.setValueNotifyingHost(retainedPostShape->second);
            CHECK(shapeParameter.getValue()==retainedPostShape->second);
        }
    }
    editorProgress("control_loop_complete", parameterCount);
    CHECK(present==(int)parameterCount);
    editorProgress("route_rows_commit_compatible_fields_and_undo_together", parameterCount);
    section("route_rows_commit_compatible_fields_and_undo_together");
    for(int slot=1;slot<=4;++slot)for(int row=1;row<=16;++row) {
        const auto id="synth."+juce::String(slot)+".modulation."+juce::String(row);
        auto* target=knob(*e,id+".target");
        auto* enabled=knob(*e,id+".enabled");
        CHECK(target&&enabled); if(!target||!enabled)continue;
        auto beforeFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& before = *beforeFixture;
        RecallListener targetListener,bankListener;targetListener.action=[]{};bankListener.action=[]{};
        p.control(idx(id+".target")).addListener(&targetListener);p.control(idx(id+".bank")).addListener(&bankListener);
        target->setValue(11,juce::sendNotificationSync);
        CHECK(targetListener.gestures==2);CHECK(bankListener.gestures==2);
        p.control(idx(id+".target")).removeListener(&targetListener);p.control(idx(id+".bank")).removeListener(&bankListener);
        CHECK(validateSoundDocument(document(p))); CHECK(p.control(idx(id+".bank")).getValue()==1);
        CHECK(p.undoSoundEdit()); CHECK(documentMatches(p,before)); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        auto* source=knob(*e,id+".source");
        auto* bank=knob(*e,id+".bank");
        auto* amount=knob(*e,id+".amount");CHECK(source&&bank&&amount);
        if(source&&bank&&amount) {
            source->setValue(5,juce::sendNotificationSync);CHECK(p.control(idx(id+".source")).getValue()==5.f/6);
            bank->setValue(1,juce::sendNotificationSync);CHECK(validateSoundDocument(document(p)));CHECK(p.control(idx(id+".target")).getValue()==14.f/16);
            grab(*amount);amount->setValue(-.5,juce::sendNotificationSync);letGo(*amount);CHECK(p.control(idx(id+".amount")).getValue()==.25f);
            CHECK(p.undoSoundEdit());CHECK(p.undoSoundEdit());CHECK(p.undoSoundEdit());CHECK(documentMatches(p,before));juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        }
        enabled->setValue(1,juce::sendNotificationSync);
        CHECK(validateSoundDocument(document(p))); CHECK(p.control(idx(id+".source")).getValue()>0);
        CHECK(p.control(idx(id+".target")).getValue()>0);
        CHECK(p.undoSoundEdit()); CHECK(documentMatches(p,before)); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    editorProgress("native_layout_explicit_reuse_macros_locks_and_presets", parameterCount);
    section("native_layout_explicit_reuse_macros_locks_and_presets");
    click("synth.2.add"); CHECK(document(p).layout.synthUse[1]==SlotUse::present);
    click("synth.2.up"); CHECK(document(p).layout.synthOrder[0]==1);
    click("synth.2.lock"); CHECK(document(p).synthLocks[1]);
    auto* level=knob(*e,"synth.2.level");
    CHECK(level&&choose(*level,"Lock")); CHECK(document(p).controlLocks[idx("synth.2.level")]);
    if(level)level->setValue(.8,juce::sendNotificationSync);
    CHECK(std::abs(p.control(idx("synth.2.level")).getValue()-.8f)<1e-6f);
    auto retainedFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& retained = *retainedFixture; click("synth.2.remove"); CHECK(document(p).layout.synthUse[1]==SlotUse::retired);
    click("synth.2.add"); CHECK(document(p).layout.synthUse[1]==SlotUse::retired);
    auto* status=dynamic_cast<juce::Label*>(findComponent(*e,"status")); CHECK(status!=nullptr);
    if(status)CHECK(status->getText().containsIgnoreCase("replace"));
    click("synth.2.replace"); CHECK(document(p).layout.synthUse[1]==SlotUse::present);
    CHECK(document(p).values==retained.values); CHECK(document(p).controlLocks==retained.controlLocks);
    click("effect.1.add"); CHECK(document(p).layout.effects[0].use==SlotUse::present);
    auto* kind=dynamic_cast<juce::ComboBox*>(findComponent(*e,"effect.1.kind")); CHECK(kind!=nullptr);
    if(kind)kind->setSelectedId(2,juce::sendNotificationSync);
    p.control(0).setValueNotifyingHost(.53f);dispatch();CHECK(kind&&kind->getSelectedId()==2);
    CHECK(document(p).layout.effects[0].kind==EffectKind::drive);
    click("effect.1.replace"); CHECK(document(p).layout.effects[0].kind==EffectKind::delay);
    auto* owner=dynamic_cast<juce::ComboBox*>(findComponent(*e,"effect.1.owner")); CHECK(owner!=nullptr);
    if(owner)owner->setSelectedId(2,juce::sendNotificationSync);
    p.control(0).setValueNotifyingHost(.54f);dispatch();CHECK(owner&&owner->getSelectedId()==2);
    click("effect.1.move");
    auto* effectStatus=dynamic_cast<juce::Label*>(findComponent(*e,"effect.1.status"));CHECK(effectStatus!=nullptr);
    const auto moveInitialChain=document(p).layout.effects[0].chain;
    const auto moveInitialStatus=effectStatus?effectStatus->getText():juce::String("missing");
    const auto moveWaitStart=std::chrono::steady_clock::now();
    CHECK(waitForUi("effect.1.move completion",[&]{return document(p).layout.effects[0].chain==1&&effectStatus&&effectStatus->getText().contains("Synth 2");},
        [&]{return "chain="+juce::String(document(p).layout.effects[0].chain)+" status="+(effectStatus?effectStatus->getText():juce::String("missing"));}));
    std::cout<<"EFFECT_MOVE_COMPLETION initial_chain="<<int(moveInitialChain)<<" initial_status="<<moveInitialStatus
        <<" final_chain="<<int(document(p).layout.effects[0].chain)<<" final_status="<<(effectStatus?effectStatus->getText():juce::String("missing"))
        <<" wait_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-moveWaitStart).count()<<'\n';
    CHECK(document(p).layout.effects[0].chain==1);
    if(effectStatus){CHECK(effectStatus->getText().contains("Delay"));CHECK(effectStatus->getText().contains("Synth 2"));}
    // FX 1 titles the kinds other than its own at the retained grey. Pressing the shown FX N again steps its pages, so
    // FX 1 is opened from FX 2.
    auto* unit=programmer(*e);CHECK(unit!=nullptr);
    if(unit){
        unit->showPage("effects");unit->showSubpage("fx2");unit->showSubpage("fx1");int drive=-1,delay=-1;
        for(const auto& t:unit->shownTitles()){if(t.text=="DRIVE")drive=t.grey;if(t.text=="DELAY")delay=t.grey;}
        CHECK(drive>=0&&delay>drive);
    }
    click("effect.1.lock"); CHECK(document(p).effectLocks[0]);
    for(int physicalSlot=2;physicalSlot<=8;++physicalSlot) {
        const auto id="effect."+juce::String(physicalSlot);click(id+".add");
        auto* type=dynamic_cast<juce::ComboBox*>(findComponent(*e,id+".kind"));CHECK(type!=nullptr);
        if(type)for(int selected=1;selected<=4;++selected){type->setSelectedId(selected,juce::sendNotificationSync);click(id+".replace");CHECK(static_cast<int>(document(p).layout.effects[static_cast<std::size_t>(physicalSlot-1)].kind)==selected-1);}
        click(id+".up");click(id+".down");click(id+".lock");CHECK(document(p).effectLocks[static_cast<std::size_t>(physicalSlot-1)]);
        auto beforeFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& before = *beforeFixture;click(id+".remove");click(id+".add");CHECK(document(p).layout.effects[static_cast<std::size_t>(physicalSlot-1)].use==SlotUse::retired);
        click(id+".replace");CHECK(document(p).layout.effects[static_cast<std::size_t>(physicalSlot-1)].use==SlotUse::present);CHECK(document(p).values==before.values);
    }
    // A macro's menu opens its assignment editor, whose target and depth apply as they change.
    auto* macro1=knob(*e,"macro.1");CHECK(macro1&&unit);
    if(macro1&&unit) {
        auto& target=unit->assign.target;auto& depth=unit->assign.depth;
        CHECK(choose(*macro1,"Edit assignment"));CHECK(unit->assign.macro==1);
        const auto& cutoff=parameterRegistry()[idx("synth.2.analog.filter.cutoff")];int item=0;
        for(int k=0;k<target.getNumItems();++k)if(target.getItemText(k)==cutoff.name)item=target.getItemId(k);
        CHECK(item!=0);
        auto beforeFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& before = *beforeFixture;
        target.setSelectedId(item,juce::sendNotificationSync);depth.setValue(-40,juce::sendNotificationSync);
        CHECK(document(p).macros[0].targetKey==cutoff.stateKey);
        CHECK(document(p).macros[0].amount==-.4f); CHECK(document(p).values==before.values);
        click("undo");click("undo"); CHECK(documentMatches(p,before)); click("redo");click("redo");
        CHECK(document(p).macros[0].amount==-.4f);
        click("programmer.assign.clear"); CHECK(document(p).macros[0]==MacroBinding{});
        // A recall shows the sound's binding in the open editor, and resets KIND and OWNER to the bank's.
        auto unboundFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& unbound = *unboundFixture;
        target.setSelectedId(2,juce::sendNotificationSync);CHECK(document(p).macros[0]!=MacroBinding{});
        if(kind)kind->setSelectedId(4,juce::sendNotificationSync);if(owner)owner->setSelectedId(5,juce::sendNotificationSync);
        CHECK(restore(p,state(unbound)));
        CHECK(waitForUi("recall resets the editor and the bank's choice: expected target/depth/kind/owner=1/0/2/2",
            [&]{return target.getSelectedId()==1&&depth.getValue()==0&&kind&&kind->getSelectedId()==2&&owner&&owner->getSelectedId()==2;},
            [&]{return describeMetadata(p)+"actual="+juce::String(target.getSelectedId())+"/"+juce::String(depth.getValue())+"/"+juce::String(kind?kind->getSelectedId():-1)+"/"+juce::String(owner?owner->getSelectedId():-1);}));
        CHECK(target.getSelectedId()==1);CHECK(depth.getValue()==0);
        CHECK(kind&&kind->getSelectedId()==2);CHECK(owner&&owner->getSelectedId()==2);
    }
    auto* path=dynamic_cast<juce::TextEditor*>(findComponent(*e,"preset.path")); CHECK(path!=nullptr);
    const auto file=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("editor-preset-"+juce::Uuid().toString()+".airack");
    if(path) {
        path->setText(file.getFullPathName()); auto beforeKnobFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& beforeKnob = *beforeKnobFixture;
        auto* knob=dynamic_cast<juce::Slider*>(findComponent(*e,"global.master_level"));if(knob)knob->setValue(.42,juce::sendNotificationSync);
        auto unnamedFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& unnamed = *unnamedFixture; click("preset.save");
        auto savedFixture = std::make_unique<SoundDocument>(unnamed); auto& saved = *savedFixture;
        saved.name=file.getFileNameWithoutExtension();CHECK(documentMatches(p,saved));
        auto tokenDocumentFixture = std::make_unique<SoundDocument>(); auto& tokenDocument = *tokenDocumentFixture;std::uint64_t editBefore=0,resetBefore=0,editAfter=0,resetAfter=0;
        CHECK(p.trySoundSnapshot(tokenDocument,editBefore,resetBefore));
        p.prepareToPlay(48000,64);juce::AudioBuffer<float> sounding(2,64);juce::MidiBuffer notes;note(notes);p.processBlock(sounding,notes);CHECK(peak(sounding)>0);
        // Macro 1's open editor shows a target and depth not pushed, and the bank a pending KIND and OWNER.
        if(unit){unit->assign.target.setSelectedId(2,juce::dontSendNotification);unit->assign.depth.setValue(-60,juce::dontSendNotification);}
        if(kind)kind->setSelectedId(4,juce::sendNotificationSync);if(owner)owner->setSelectedId(5,juce::sendNotificationSync);
        click("preset.load");CHECK(documentMatches(p,saved));CHECK(status&&!status->getText().containsIgnoreCase("failed"));
        CHECK(p.trySoundSnapshot(tokenDocument,editAfter,resetAfter));CHECK(editAfter>editBefore&&resetAfter>resetBefore);
        notes.clear();p.processBlock(sounding,notes);CHECK(peak(sounding)==0);p.releaseResources();
        CHECK(unit&&unit->assign.target.getSelectedId()==1);CHECK(unit&&unit->assign.depth.getValue()==0);
        CHECK(kind&&kind->getSelectedId()==2);CHECK(owner&&owner->getSelectedId()==2);
        click("undo");CHECK(documentMatches(p,unnamed));click("undo");CHECK(documentMatches(p,beforeKnob));
        click("redo");click("redo");CHECK(documentMatches(p,saved));
        CHECK(file.getSize()==currentSoundStateSize+saved.name.getNumBytesAsUTF8()); click("synth.2.remove"); click("preset.load"); CHECK(documentMatches(p,saved));
        juce::MemoryBlock bytes; CHECK(file.loadFileAsData(bytes));
        path->setText(file.getFullPathName()+".missing"); click("preset.load"); CHECK(documentMatches(p,saved));
        juce::MemoryBlock unchanged; CHECK(file.loadFileAsData(unchanged)); CHECK(bytes==unchanged);
        if(status)CHECK(status->getText().containsIgnoreCase("failed"));
    }
    editorProgress("native_recall_cancels_drag_and_preserves_latest_metadata", parameterCount);
    section("native_recall_cancels_drag_and_preserves_latest_metadata");
    auto* master=knob(*e,"global.master_level");CHECK(master&&unit);
    if(master&&unit) {
        RecallListener listener;listener.action=[]{};p.control(0).addListener(&listener);
        editorProgress("recall_drag_set_begin", parameterCount);
        grab(*master); master->setValue(.2,juce::sendNotificationSync);
        editorProgress("recall_drag_set_end", parameterCount);
        editorProgress("recall_snapshot_edit_begin", parameterCount);
        auto recalledFixture = airack::testing::fixtureValue([&]{return document(p);}); auto& recalled = *recalledFixture;recalled.values[0]=.77f;recalled.macros[2]={parameterRegistry()[0].stateKey,.3f};recalled.synthLocks[3]=true;
        editorProgress("recall_snapshot_edit_end", parameterCount);
        editorProgress("recall_encode_restore_begin", parameterCount);
        CHECK(restore(p,state(recalled)));
        editorProgress("recall_encode_restore_end", parameterCount);
        editorProgress("recall_followup_release_dispatch_begin", parameterCount);
        master->setValue(.4,juce::sendNotificationSync);
        CHECK(std::abs(master->getValue()-.77)<1e-5); letGo(*master);dispatch();
        editorProgress("recall_followup_release_dispatch_end", parameterCount);
        editorProgress("recall_exact_verification_begin", parameterCount);
        CHECK(documentMatches(p,recalled));CHECK(listener.gestures==2);p.control(0).removeListener(&listener);
        editorProgress("recall_exact_verification_end", parameterCount);
        // GLOBAL shows the recalled lock and macro 3's tape, which names its target.
        editorProgress("recall_show_page_metadata_begin", parameterCount);
        unit->showPage("global");
        auto* lock=dynamic_cast<juce::ToggleButton*>(findComponent(*e,"synth.4.lock"));
        auto tape=[&]{for(const auto& t:unit->shownTapes())if(t.index==3u)return t.text;return juce::String();};
        CHECK(waitForUi("recall metadata: expected synth4 lock=1 and macro 3's tape",
            [&]{return lock&&lock->getToggleState()&&tape().isNotEmpty();},
            [&]{return describeMetadata(p)+"actual lock="+juce::String(lock?int(lock->getToggleState()):-1)+" tape="+tape();}));
        CHECK(lock&&lock->getToggleState());CHECK(tape().endsWith(" +30"));
        editorProgress("recall_show_page_metadata_end", parameterCount);
        editorProgress("active_drag_editor_destroy_begin", parameterCount);
        grab(*master);master->setValue(.5,juce::sendNotificationSync);e.reset();
        editorProgress("active_drag_editor_destroy_end", parameterCount);
        editorProgress("post_destroy_processor_gesture_dispatch_begin", parameterCount);
        CHECK(p.beginParameterGesture(0));CHECK(p.endParameterGesture(0));dispatch();
        editorProgress("post_destroy_processor_gesture_dispatch_end", parameterCount);
    }
    for(int n=0;n<3;++n){
        editorProgress("lifecycle_cycle_create_begin", parameterCount, n);
        e.reset(p.createEditor());
        editorProgress("lifecycle_cycle_create_end", parameterCount, n);
        editorProgress("lifecycle_cycle_restore_begin", parameterCount, n);
        CHECK(restore(p,state(document(p))));
        editorProgress("lifecycle_cycle_restore_end", parameterCount, n);
        editorProgress("lifecycle_cycle_destroy_begin", parameterCount, n);
        e.reset();
        editorProgress("lifecycle_cycle_destroy_end", parameterCount, n);
        editorProgress("lifecycle_cycle_dispatch_begin", parameterCount, n);
        dispatch();
        editorProgress("lifecycle_cycle_dispatch_end", parameterCount, n);
    }
    editorProgress("final_editor_create_begin", parameterCount);
    e.reset(p.createEditor());
    editorProgress("final_editor_create_end", parameterCount);
    editorProgress("final_editor_visibility_dispatch_begin", parameterCount);
    e->setVisible(true);dispatch();
    editorProgress("final_editor_visibility_dispatch_end", parameterCount);
    editorProgress("native_chooser_cancellation_and_all_scroll_pages_reachable", parameterCount);
    menuServiceLifetime();
    presetMenuCancellation(p,e);
    e.reset(p.createEditor());e->setVisible(true);dispatch();
    click("rack.toggle");
    // The click is a posted message, which may wait behind others; the walk needs the unit it shows.
    auto* walked=programmer(*e);
    CHECK(waitForUi("the PROGRAMMER after the lever",[&]{return walked!=nullptr&&walked->isVisible();},[&]{return juce::String(walked==nullptr?"no unit":"unit hidden");}));
    // Walk every route of the native model, including every Dynamic EQ band.
    std::set<juce::String> reached;
    auto visit=[&] {
        for(auto* k:walked->shownKnobs()) {
            reached.insert(k->getComponentID());CHECK(e->isParentOf(k));
            for(juce::Component* parent=k;parent;parent=parent->getParentComponent())CHECK(parent->isVisible());
            CHECK(!k->getBounds().isEmpty());
            CHECK(walked->getLocalBounds().contains(walked->getLocalArea(k,k->getLocalBounds())));
        }
    };
    if(walked)for(const auto& page:walked->pageButtons) {
        walked->showPage(page->getComponentID().fromFirstOccurrenceOf("programmer.page.",false,false));
        juce::StringArray subpages;
        for(const auto& b:walked->subpageButtons)if(b->isVisible())subpages.add(b->getComponentID().fromFirstOccurrenceOf("programmer.subpage.",false,false));
        for(const auto& id:subpages) {
            const auto& spec=studio::studioSkin()->page(walked->page());
            const auto sub=std::find_if(spec.subpages.begin(),spec.subpages.end(),[&](const auto& s){return s.id==id;});
            CHECK(sub!=spec.subpages.end());
            const auto count=sub==spec.subpages.end()?1:std::max(1,int(sub->routes.size()));
            for(int n=0;n<count;++n){walked->showSubpage(id);visit();}
        }
        if(subpages.isEmpty())visit();
    }
    CHECK(reached.size()==parameterCount);
    for(std::size_t i=0;i<parameterCount;++i)CHECK(reached.count(parameterRegistry()[i].id)==1);
    if(walked)walked->showPage("global");
    auto tempoDocumentFixture = airack::testing::fixtureValue([&]{return document(p);}); auto& tempoDocument = *tempoDocumentFixture;physical(tempoDocument,"synth.1.lfo.1.synced",1);physical(tempoDocument,"synth.1.modulation.1.enabled",1);physical(tempoDocument,"synth.1.modulation.1.source",3);physical(tempoDocument,"synth.1.modulation.1.target",1);
    CHECK(restore(p,state(tempoDocument)));p.prepareToPlay(48000,64);juce::AudioBuffer<float> block(2,64);juce::MidiBuffer midi;note(midi);p.processBlock(block,midi);
    auto* tempo=dynamic_cast<juce::Label*>(findComponent(*e,"tempo.status"));
    auto describeTempo=[&]{return "processorMissingTempo="+juce::String(int(p.tempoUnavailable()))+" label="+(tempo?tempo->getText():"<missing>");};
    CHECK(p.tempoUnavailable());
    CHECK(waitForUi("missing host tempo label",[&]{return tempo&&tempo->getText().containsIgnoreCase("unavailable");},describeTempo));
    CHECK(tempo&&tempo->getText().containsIgnoreCase("unavailable"));
    Playhead head;head.bpm=120;p.setPlayHead(&head);midi.clear();p.processBlock(block,midi);CHECK(!p.tempoUnavailable());
    CHECK(waitForUi("recovered 120 BPM label",[&]{return tempo&&!tempo->getText().containsIgnoreCase("unavailable");},describeTempo));
    CHECK(tempo&&!tempo->getText().containsIgnoreCase("unavailable"));p.setPlayHead(nullptr);p.releaseResources();
    if(!prefix.isEmpty()) {
        auto* main=programmer(*e);CHECK(main!=nullptr);
        auto capture=[&](const juce::String& suffix){auto image=e->createComponentSnapshot(e->getLocalBounds(),true);auto out=juce::File(prefix+suffix+".png").createOutputStream();CHECK(out&&image.isValid());if(out)CHECK(juce::PNGImageFormat().writeImageToStream(image,*out));};
        capture("-global-min");e->setSize(1440,900);capture("-global-large");e->setSize(1080,680);
        // Each sub-page of each page, ROUTES as its four pages of routes.
        if(main)for(const auto& page:main->pageButtons) {
            const auto id=page->getComponentID().fromFirstOccurrenceOf("programmer.page.",false,false);
            main->showPage(id);dispatch();
            juce::StringArray subpages;
            for(const auto& b:main->subpageButtons)if(b->isVisible())subpages.add(b->getComponentID().fromFirstOccurrenceOf("programmer.subpage.",false,false));
            for(const auto& sub:subpages)for(int n=0;n<(sub=="routes"?4:1);++n) {
                main->showSubpage(sub);dispatch();
                capture("-"+id+"-"+sub+(sub=="routes"?juce::String(main->routesPage()+1):juce::String()));
            }
        }
    }
    editorProgress("function_body_complete", parameterCount);
}
void mainSyncGestureBoundary()
{
    section("main_sync_detector_only_lock_boundary_preserves_saturation_manual_gestures");
    auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;CHECK(blankInstance(p));
    const auto saturation=idx("effect.1.saturation.input_trim_db"),detector=dynamicEqDetectorParameterStart+3;
    auto before=document(p);before.effectLocks[0]=true;before.controlLocks[saturation]=true;before.controlLocks[detector]=true;
    CHECK(restore(p,state(before)));before=document(p);
    struct Events final:juce::AudioProcessorParameter::Listener {int begins=0,ends=0;
        void parameterValueChanged(int,float) override{}
        void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}} events;
    p.control(saturation).addListener(&events);
    const bool opened=p.beginParameterGesture(saturation);CHECK(opened);
    if(opened){CHECK(p.setParameterGestureValue(saturation,.6f));CHECK(p.endParameterGesture(saturation));}
    CHECK(document(p).values[saturation]==.6f);CHECK(events.begins==1&&events.ends==1);
    CHECK(!p.beginParameterGesture(detector));
    if(opened){CHECK(p.undoSoundEdit());CHECK(documentMatches(p,before));}
    p.control(saturation).removeListener(&events);
}
void uiConvergenceDiagnostic(bool fixedWait)
{
    section("focused_real_metadata_and_tempo_timer_delivery");
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};auto initial=document(p);
    RackLayout layout;
    CHECK(editRackLayout(initial.layout,{RackEditKind::addSynth},layout)==RackEditResult::applied);
    initial.layout=layout;
    const auto added=editRackLayout(initial.layout,{RackEditKind::addEffect,noSlot,1,0,EffectKind::delay,false},layout);
    CHECK(added==RackEditResult::applied);if(added!=RackEditResult::applied)return;
    initial.layout=layout;
    physical(initial,"synth.1.lfo.1.synced",1);physical(initial,"synth.1.modulation.1.enabled",1);
    physical(initial,"synth.1.modulation.1.source",3);physical(initial,"synth.1.modulation.1.target",1);
    CHECK(restore(p,state(initial)));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* unit=programmer(*editor);
    auto* macro1=knob(*editor,"macro.1");
    auto* kind=dynamic_cast<juce::ComboBox*>(findComponent(*editor,"effect.1.kind"));
    auto* owner=dynamic_cast<juce::ComboBox*>(findComponent(*editor,"effect.1.owner"));
    auto* lock=dynamic_cast<juce::ToggleButton*>(findComponent(*editor,"synth.4.lock"));
    auto* tempo=dynamic_cast<juce::Label*>(findComponent(*editor,"tempo.status"));
    CHECK(unit&&macro1&&kind&&owner&&lock&&tempo);
    if(!(unit&&macro1&&kind&&owner&&lock&&tempo))return;
    // Macro 1's assignment editor stays open over GLOBAL, which shows macro 3's tape.
    CHECK(choose(*macro1,"Edit assignment"));
    auto* target=&unit->assign.target;auto* depth=&unit->assign.depth;
    auto tape=[&]{for(const auto& t:unit->shownTapes())if(t.index==3u)return t.text;return juce::String();};
    p.prepareToPlay(48000,64);juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;Playhead head;head.bpm=120;
    int misses=0;double longest=0;
    auto observe=[&](int iteration,const char* phase,const auto& ready) {
        const auto start=std::chrono::steady_clock::now();
        bool fixedReady=true;
        if(fixedWait){dispatch();fixedReady=ready();CHECK(fixedReady);if(!fixedReady)++misses;}
        SoundDocument committed;std::uint64_t revision=0,reset=0;CHECK(p.trySoundSnapshot(committed,revision,reset));
        auto values=[&]{std::cout<<" reset="<<reset<<" edit="<<revision<<" docMacro="<<committed.macros[2].targetKey
            <<" docLock="<<committed.synthLocks[3]<<" processorMissingTempo="<<p.tempoUnavailable()
            <<" ui="<<target->getSelectedId()<<','<<depth->getValue()<<','<<kind->getSelectedId()<<','<<owner->getSelectedId()
            <<','<<lock->getToggleState()<<",tape="<<tape()<<" label="<<tempo->getText();};
        if(fixedWait){std::cout<<"UI_DIAG iteration="<<iteration<<" phase="<<phase<<" fixed30Ready="<<fixedReady;values();std::cout<<'\n';}
        CHECK(waitForUi(phase,ready,[&]{
            return "reset="+juce::String(static_cast<juce::int64>(reset))+" edit="+juce::String(static_cast<juce::int64>(revision))
                +" processorMissingTempo="+juce::String(int(p.tempoUnavailable()))+" ui="+juce::String(target->getSelectedId())+","+juce::String(depth->getValue())
                +","+juce::String(kind->getSelectedId())+","+juce::String(owner->getSelectedId())+","+juce::String(int(lock->getToggleState()))+",tape="+tape()+" label="+tempo->getText();}));
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        longest=std::max(longest,elapsed);CHECK(ready());
        if(!fixedWait||!fixedReady){std::cout<<"UI_DIAG phase="<<phase<<" eventualReady="<<ready()<<" elapsed_ms="<<elapsed;values();std::cout<<'\n';}
    };
    for(int n=0;n<40;++n)
    {
        // A target and depth shown but not pushed, and a bank choice still pending.
        target->setSelectedId(2,juce::dontSendNotification);depth->setValue(-70,juce::dontSendNotification);
        kind->setSelectedId(4,juce::sendNotificationSync);owner->setSelectedId(5,juce::sendNotificationSync);
        const auto before=document(p);SoundDocument tokens;std::uint64_t editBefore=0,resetBefore=0,editAfter=0,resetAfter=0;
        CHECK(p.trySoundSnapshot(tokens,editBefore,resetBefore));CHECK(restore(p,state(before)));
        CHECK(p.trySoundSnapshot(tokens,editAfter,resetAfter));CHECK(tokens==before);CHECK(resetAfter>resetBefore&&editAfter>editBefore);
        observe(n,"identical-reset",[&]{return target->getSelectedId()==1&&depth->getValue()==0&&kind->getSelectedId()==2&&owner->getSelectedId()==2;});
        auto recalled=before;recalled.synthLocks[3]=n%2==0;recalled.macros[2]=n%2==0?MacroBinding{parameterRegistry()[0].stateKey,.3f}:MacroBinding{};
        CHECK(restore(p,state(recalled)));CHECK(document(p)==recalled);
        observe(n,"metadata-recall",[&]{return lock->getToggleState()==recalled.synthLocks[3]&&tape().isNotEmpty()==(n%2==0);});
        p.setPlayHead(nullptr);midi.clear();p.processBlock(audio,midi);CHECK(p.tempoUnavailable());
        observe(n,"tempo-unavailable",[&]{return tempo->getText().containsIgnoreCase("unavailable");});
        p.setPlayHead(&head);p.processBlock(audio,midi);CHECK(!p.tempoUnavailable());
        observe(n,"tempo-120",[&]{return !tempo->getText().containsIgnoreCase("unavailable");});
    }
    p.setPlayHead(nullptr);p.releaseResources();
    std::cout<<"UI_DIAG fixed30_misses="<<misses<<" observations=160 longest_observation_ms="<<longest<<'\n';
}
void boundedBatchGestureCheck()
{
    section("bounded_batch_gesture_atomicity_recovery_and_cancellation");
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; CHECK(blankInstance(p)); const auto original=document(p);
    std::array<ParameterGestureValue,5> values {{{252,1},{253,.5f},{254,14.f/16},{255,.5f},{256,.6f}}};
    std::array<RecallListener,5> listeners;
    for(std::size_t i=0;i<5;++i){listeners[i].action=[]{};p.control(252+i).addListener(&listeners[i]);}
    CHECK(p.beginParameterGesture(253));CHECK(p.setParameterGestureValues(253,values));CHECK(p.endParameterGesture(253));
    const auto edited=document(p);CHECK(edited!=original);CHECK(validateSoundDocument(edited));
    for(std::size_t i=0;i<5;++i){CHECK(listeners[i].gestures==2);p.control(252+i).removeListener(&listeners[i]);}
    CHECK(p.undoSoundEdit());CHECK(document(p)==original);CHECK(p.redoSoundEdit());CHECK(document(p)==edited);
    CHECK(p.beginParameterGesture(253));
    auto invalid=values;invalid[4].index=252;CHECK(!p.setParameterGestureValues(253,invalid));
    invalid=values;invalid[4].value=std::numeric_limits<float>::quiet_NaN();CHECK(!p.setParameterGestureValues(253,invalid));
    invalid=values;invalid[4].index=parameterCount;CHECK(!p.setParameterGestureValues(253,invalid));
    CHECK(!p.setParameterGestureValues(253,{}));CHECK(!p.setParameterGestureValues(254,values));
    std::array<ParameterGestureValue,6> tooMany {};CHECK(!p.setParameterGestureValues(253,tooMany));
    CHECK(document(p)==edited);CHECK(p.endParameterGesture(253));CHECK(p.undoSoundEdit());CHECK(document(p)==original);
    struct Nested final : juce::AudioProcessorParameter::Listener {
        std::function<void()> action;int begins=0,ends=0;bool fired=false;
        void parameterValueChanged(int,float) override {}
        void parameterGestureChanged(int,bool starting) override {if(starting){++begins;if(!fired){fired=true;action();}}else ++ends;}
    } nested;
    auto latest=original;latest.values[0]=.87f;latest.macros[3]={parameterRegistry()[0].stateKey,-.3f};
    nested.action=[&]{CHECK(restore(p,state(latest)));dispatch();};p.control(255).addListener(&nested);
    CHECK(p.beginParameterGesture(253));CHECK(!p.setParameterGestureValues(253,values));CHECK(p.endParameterGesture(253));
    CHECK(nested.begins==1&&nested.ends==1);CHECK(document(p)==latest);CHECK(!p.undoSoundEdit());p.control(255).removeListener(&nested);
    CHECK(!p.setParameterGestureValues(253,values));CHECK(document(p)==latest);
    nested.fired=false;nested.begins=0;nested.ends=0;
    nested.action=[&]{p.control(0).setValueNotifyingHost(.39f);p.control(256).setValueNotifyingHost(.72f);};
    p.control(255).addListener(&nested);CHECK(p.beginParameterGesture(253));
    CHECK(p.setParameterGestureValues(253,std::span<const ParameterGestureValue>(values.data(),4)));CHECK(p.endParameterGesture(253));
    CHECK(document(p).values[0]==.39f);CHECK(document(p).values[256]==.72f);CHECK(nested.begins==1&&nested.ends==1);
    p.control(255).removeListener(&nested);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* target=knob(*editor,"synth.1.modulation.1.target");CHECK(target!=nullptr);
    nested.fired=false;nested.begins=0;nested.ends=0;
    latest.values[13]=1;latest.macros[3]={parameterRegistry()[0].stateKey,.6f};latest.synthLocks[2]=true;
    nested.action=[&]{CHECK(restore(p,state(latest)));dispatch();};p.control(255).addListener(&nested);
    if(target)target->setValue(11,juce::sendNotificationSync);
    dispatch();CHECK(document(p)==latest);CHECK(nested.begins==1&&nested.ends==1);
    CHECK(target&&target->getValue()==0);
    auto* status=dynamic_cast<juce::Label*>(findComponent(*editor,"status"));CHECK(status&&status->getText().containsIgnoreCase("failed"));
    // GLOBAL shows macro 4's recalled target on its tape.
    auto* unit=programmer(*editor);juce::String tape;
    if(unit)for(const auto& t:unit->shownTapes())if(t.index==4u)tape=t.text;
    CHECK(tape.endsWith(" +60"));
    auto* lock=dynamic_cast<juce::ToggleButton*>(findComponent(*editor,"synth.3.lock"));CHECK(lock&&lock->getToggleState());
    p.control(255).removeListener(&nested);
}
void enabledWidgetReentrantCoupledGestureCheck()
{
    section("enabled_widget_announces_initially_equal_reentrant_coupled_field");
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; CHECK(blankInstance(p));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    const auto original=document(p);
    auto* enabled=knob(*editor,"synth.1.modulation.1.enabled");
    CHECK(enabled!=nullptr);
    CHECK(original.values[252]==0&&original.values[253]==0&&original.values[254]==0);
    struct Listener final : juce::AudioProcessorParameter::Listener {
        std::function<void()> onBegin;int begins=0,ends=0;
        void parameterValueChanged(int,float) override {}
        void parameterGestureChanged(int,bool starting) override
        {
            if(starting){++begins;if(onBegin)onBegin();}else ++ends;
        }
    } enabledListener,sourceListener,targetListener,bankListener,amountListener;
    enabledListener.onBegin=[&]{p.control(253).setValueNotifyingHost(.5f);};
    targetListener.onBegin=[&]{
        CHECK(enabledListener.begins==1&&enabledListener.ends==0);
        CHECK(p.control(253).getValue()==.5f);
        p.control(253).setValueNotifyingHost(0);
    };
    std::array<Listener*,5> listeners {&enabledListener,&sourceListener,&targetListener,&bankListener,&amountListener};
    for(std::size_t i=0;i<listeners.size();++i)p.control(252+i).addListener(listeners[i]);
    if(enabled)enabled->setValue(1,juce::sendNotificationSync);
    dispatch();
    CHECK(enabledListener.begins==1&&enabledListener.ends==1);
    CHECK(sourceListener.begins==1&&sourceListener.ends==1);
    CHECK(targetListener.begins==1&&targetListener.ends==1);
    CHECK(bankListener.begins==0&&bankListener.ends==0);
    CHECK(amountListener.begins==0&&amountListener.ends==0);
    const auto edited=document(p);auto expected=original;
    expected.values[252]=1;expected.values[253]=.5f;expected.values[254]=1.f/16;
    CHECK(validateSoundDocument(edited));CHECK(edited==expected);
    for(std::size_t i=0;i<listeners.size();++i)p.control(252+i).removeListener(listeners[i]);
    CHECK(p.undoSoundEdit());CHECK(document(p)==original);CHECK(!p.undoSoundEdit());
    CHECK(p.redoSoundEdit());CHECK(document(p)==edited);CHECK(!p.redoSoundEdit());
    std::cout<<"Reentrant Enabled widget gestures Enabled="<<enabledListener.begins<<'/'<<enabledListener.ends
             <<" Source="<<sourceListener.begins<<'/'<<sourceListener.ends
             <<" Target="<<targetListener.begins<<'/'<<targetListener.ends<<'\n';
}
void coherentResetTokenCheck()
{
    section("coherent_reset_token_snapshot_preserves_outputs_on_contention");
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};auto a=document(p),b=a;a.values[0]=.2f;b.values[0]=.8f;b.synthLocks[3]=true;
    const auto sa=state(a),sb=state(b);CHECK(restore(p,sa));
    std::atomic<bool> done=false,writerGood=true;std::thread writer([&]{for(int i=0;i<300;++i)if(!restore(p,i%2==0?sb:sa))writerGood=false;done=true;});
    int rejected=0,accepted=0;bool good=true;
    do {
        SoundDocument d;d.values[0]=.123f;const auto before=d;std::uint64_t edit=777,reset=888;
        if(p.trySoundSnapshot(d,edit,reset)){++accepted;good&=(d==(reset%2==0?b:a))&&edit==reset;}
        else {++rejected;good&=d==before&&edit==777&&reset==888;}
    }while(!done.load());
    writer.join();CHECK(writerGood);CHECK(good);CHECK(accepted>0);
    std::cout<<"Coherent reset snapshots accepted="<<accepted<<" rejected="<<rejected<<'\n';
}
void routedProcessorAndMigration()
{
    section("routedProcessorAndMigration");
    auto baseFixture = airack::testing::fixtureValue([&]{return fullDocument();}); auto& base = *baseFixture; physical(base,"effect.1.drive.gain_db",24); physical(base,"effect.1.drive.mix",1); physical(base,"effect.6.delay.seconds",.013f); physical(base,"effect.6.delay.feedback",.8f); physical(base,"effect.6.delay.mix",.8f); auto actual=renderDocument(base); auto plainFixture = airack::testing::fixtureValue([&]{return base;}); auto& plain = *plainFixture; plain.layout=RackLayout{};
    CHECK(maxDifference(actual,renderDocument(plain))>1e-5);
    auto bypassedFixture = airack::testing::fixtureValue([&]{return base;}); auto& bypassed = *bypassedFixture; for(int i=1;i<=8;++i)physical(bypassed,"effect."+juce::String(i)+".bypassed",1);
    CHECK(maxDifference(actual,renderDocument(bypassed))>1e-6);
    auto reorderedFixture = airack::testing::fixtureValue([&]{return base;}); auto& reordered = *reorderedFixture; std::swap(reordered.layout.effectOrder[0][0],reordered.layout.effectOrder[0][1]);
    CHECK(maxDifference(actual,renderDocument(reordered))>1e-7);
    auto movedFixture = airack::testing::fixtureValue([&]{return base;}); auto& moved = *movedFixture; CHECK(editRackLayout(moved.layout,{RackEditKind::moveEffect,1,masterChain,0},moved.layout)==RackEditResult::applied);
    CHECK(maxDifference(actual,renderDocument(moved))>1e-7);
    auto mutedFixture = airack::testing::fixtureValue([&]{return base;}); auto& muted = *mutedFixture; for(int i=1;i<=4;++i) physical(muted,"synth."+juce::String(i)+".mute",1);
    CHECK(windowEnergy(renderDocument(muted),0,4096)==0);
    auto soloFixture = airack::testing::fixtureValue([&]{return base;}); auto& solo = *soloFixture; physical(solo,"synth.2.solo",1); CHECK(maxDifference(actual,renderDocument(solo))>1e-5);
    auto macroFixture = airack::testing::fixtureValue([&]{return base;}); auto& macro = *macroFixture; macro.values[1]=1; CHECK(maxDifference(actual,renderDocument(macro))>1e-6);
    auto monoFixture = airack::testing::fixtureValue([&]{return base;}); auto& mono = *monoFixture; physical(mono,"global.voice_mode",1); physical(mono,"global.glide_seconds",.2f);
    auto mpFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& mp = *mpFixture; CHECK(restore(mp,state(mono))); mp.prepareToPlay(8000,256);
    auto rpFixture = std::make_unique<RoutedPatch>(); auto& rp = *rpFixture; CHECK(makeRoutedPatch(mono,rp)); RoutedSynthEngine ref; CHECK(ref.setPatch(rp)); CHECK(ref.prepare(8000,256));
    juce::AudioBuffer<float> a(2,256),b(2,256); juce::MidiBuffer m;
    for(int k=0;k<8;++k) { m.clear(); m.addEvent(juce::MidiMessage::noteOn(1,60+k,(juce::uint8)100),13); mp.processBlock(a,m); ref.render(b,m); for(int i=0;i<256;++i) CHECK(a.getSample(0,i)==b.getSample(0,i)); }
    auto lfo=SoundDocument{}; physical(lfo,"synth.1.lfo.1.synced",1); physical(lfo,"synth.1.modulation.1.enabled",1);
    physical(lfo,"synth.1.modulation.1.source",3); physical(lfo,"synth.1.modulation.1.target",1); physical(lfo,"synth.1.modulation.1.amount",.9f);
    CHECK(maxDifference(renderDocument(lfo,120),renderDocument(SoundDocument{},120))>1e-6);
    auto timingFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& timing = *timingFixture; CHECK(restore(timing,state(lfo))); timing.prepareToPlay(8000,256); note(m); timing.processBlock(a,m); CHECK(timing.tempoUnavailable());
    Playhead head; head.bpm=120; timing.setPlayHead(&head); m.clear(); timing.processBlock(a,m); CHECK(!timing.tempoUnavailable()); head.bpm=10; timing.processBlock(a,m); CHECK(timing.tempoUnavailable()); timing.setPlayHead(nullptr);
    // Independently encode exact historical v1 keys; all old physical slots remain usable.
    for (int slot=2;slot<=4;++slot) for (bool enabled:{false,true}) {
        auto vFixture = airack::testing::fixtureValue([&]{return defaultParameterValues();}); auto& v = *vFixture; v[idx("synth.1.enabled")]=0; v[idx("synth."+juce::String(slot)+".enabled")]=enabled?1.f:0.f;
        juce::MemoryBlock legacy; juce::MemoryOutputStream out(legacy,false); out.writeInt(0x49535241); out.writeInt(1); out.writeInt(209); out.writeInt(0);
        for(std::size_t i=0;i<212;++i) if(i<9||i>=12) { out.writeInt(static_cast<int>(parameterRegistry()[i].stateKey)); out.writeFloat(v[i]); }
        out.flush(); auto digest=juce::SHA256(legacy).getRawData(); out.write(digest.getData(),digest.getSize()); out.flush();
        auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture; CHECK(restore(p,legacy)); CHECK(document(p).layout.synthCount==4); p.prepareToPlay(8000,256); note(m); p.processBlock(a,m);
        CHECK((peak(a)>0)==enabled); if(!enabled) { setPhysical(p,"synth."+juce::String(slot)+".enabled",1); for(int k=0;k<4;++k) { note(m); p.processBlock(a,m); } CHECK(peak(a)>0); }
    }
    // Actual free-delay echoes persist after 20s; estimate must include those samples.
    auto delayFixture = std::make_unique<SoundDocument>(); auto& delay = *delayFixture; RackLayout next; CHECK(editRackLayout(delay.layout,{RackEditKind::addEffect,noSlot,masterChain,0,EffectKind::delay,false},next)==RackEditResult::applied); delay.layout=next;
    physical(delay,"effect.1.delay.seconds",4); physical(delay,"effect.1.delay.feedback",.95f); physical(delay,"effect.1.delay.mix",1); physical(delay,"synth.1.analog.amp.release",.01f);
    auto dpFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& dp = *dpFixture; CHECK(restore(dp,state(delay))); dp.prepareToPlay(8000,256);
    float late=0; for(int start=0;start<25*8000;start+=256) { m.clear(); if(start==0) { note(m); m.addEvent(juce::MidiMessage::noteOff(1,60),128); } dp.processBlock(a,m); if(start>=20*8000) late=std::max(late,peak(a)); }
    CHECK(late>1e-5f); CHECK(dp.getTailLengthSeconds()>25); CHECK(dp.getTailLengthSeconds()<2000);
    std::cout<<"Long delay late_peak="<<late<<" estimate_seconds="<<dp.getTailLengthSeconds()<<'\n';
    const auto oneTail=dp.getTailLengthSeconds();
    auto serialDelayFixture = airack::testing::fixtureValue([&]{return delay;}); auto& serialDelay = *serialDelayFixture; CHECK(editRackLayout(serialDelay.layout,{RackEditKind::addEffect,noSlot,0,0,EffectKind::delay,false},serialDelay.layout)==RackEditResult::applied);
    physical(serialDelay,"effect.2.delay.seconds",4); physical(serialDelay,"effect.2.delay.feedback",.95f); physical(serialDelay,"effect.2.delay.mix",1);
    CHECK(restore(dp,state(serialDelay))); m.clear();dp.processBlock(a,m); CHECK(dp.getTailLengthSeconds()>oneTail*1.9);
    CHECK(restore(dp,state(delay))); dp.processBlock(a,m);
    setPhysical(dp,"effect.1.bypassed",1); m.clear(); dp.processBlock(a,m); CHECK(dp.getTailLengthSeconds()<1);
    auto removedFixture = airack::testing::fixtureValue([&]{return delay;}); auto& removed = *removedFixture; removed.layout=RackLayout{}; CHECK(edit(dp,removed)); dp.processBlock(a,m); CHECK(dp.getTailLengthSeconds()<1);
    // Revision 2's sinc reaches up to 8 frames past each repeat's length: 314 passes at 8 kHz.
    auto modernDelayFixture = airack::testing::fixtureValue([&]{return delay;}); auto& modernDelay = *modernDelayFixture; modernDelay.compatibility.effects[0][delayEffectBank]=modernDelayAlgorithmRevision;
    CHECK(restore(dp,state(delay))); m.clear(); dp.processBlock(a,m); const auto legacyTail=dp.getTailLengthSeconds();
    CHECK(restore(dp,state(modernDelay))); m.clear(); dp.processBlock(a,m);
    CHECK(std::abs(dp.getTailLengthSeconds()-legacyTail-314*8/8000.)<1e-6);
    std::cout<<"Delay2 reported tail legacy="<<legacyTail<<" modern="<<dp.getTailLengthSeconds()<<'\n';
    physical(delay,"effect.1.delay.synced",1); CHECK(restore(dp,state(delay))); dp.processBlock(a,m); CHECK(dp.tempoUnavailable()); CHECK(dp.getTailLengthSeconds()>20);
}
// The tape keys add reported tail only away from neutral: per pass one sample
// for the saturation, 1 ms for the wobble and the loop low cut's decay; the
// echo low cut's decay once; and once the glide's lag, 0.5 ln(4 s / TIME).
void delayTapeReportedTail()
{
    section("delay_tape_reported_tail");
    SoundDocument delay;
    CHECK(editRackLayout(delay.layout,{RackEditKind::addEffect,noSlot,masterChain,0,EffectKind::delay,false},delay.layout)==RackEditResult::applied);
    physical(delay,"effect.1.delay.seconds",4);physical(delay,"effect.1.delay.feedback",.95f);physical(delay,"effect.1.delay.mix",1);
    delay.compatibility.effects[0][delayEffectBank]=modernDelayAlgorithmRevision;
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(restore(p,state(delay)));p.prepareToPlay(8000,256);
    juce::AudioBuffer<float> a(2,256);juce::MidiBuffer m;
    const auto tail=[&](const SoundDocument& d,const juce::String& key,float value){
        auto moved=d;physical(moved,"effect.1.delay."+key,value);CHECK(restore(p,state(moved)));m.clear();p.processBlock(a,m);return p.getTailLengthSeconds();};
    const double neutral=tail(delay,"saturation",0),lowCutDecay=16.0127351353/(6.283185307179586*100/1.5537739740300374);
    CHECK(std::abs(tail(delay,"saturation",.3f)-neutral-314/8000.)<1e-6);
    CHECK(std::abs(tail(delay,"wobble",.2f)-neutral-314*.001)<1e-6);
    CHECK(tail(delay,"wobble_rate_hz",5)==neutral);
    CHECK(std::abs(tail(delay,"low_cut_hz",100)-neutral-315*lowCutDecay)<1e-6);
    CHECK(tail(delay,"glide",1)==neutral);
    physical(delay,"effect.1.delay.seconds",.25f);
    const double quarter=tail(delay,"glide",0),glided=tail(delay,"glide",1);
    CHECK(std::abs(glided-quarter-.5*std::log(16.))<1e-6);
    std::cout<<"DELAY_TAPE_REPORTED_TAIL neutral="<<neutral<<" quarter="<<quarter<<" quarter_glide="<<glided<<'\n';
}
void unassignedDrumReportedTail()
{
    section("unassigned_drum_source_tail_ignores_remembered_melodic_release");
    auto documentFixture=std::make_unique<SoundDocument>();auto& d=*documentFixture;
    physical(d,"synth.1.analog.amp.release",10);physical(d,"synth.1.drum_mode",1);
    auto processorFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& processor=*processorFixture;
    CHECK(restore(processor,state(d)));processor.prepareToPlay(8000,256);juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;note(midi);processor.processBlock(audio,midi);
    CHECK(peak(audio)==0);CHECK(processor.getTailLengthSeconds()<1);
    physical(d,"synth.1.drum_mode",0);CHECK(restore(processor,state(d)));note(midi);processor.processBlock(audio,midi);CHECK(processor.getTailLengthSeconds()>9);
    CHECK(editRackLayout(d.layout,{RackEditKind::addSynth},d.layout)==RackEditResult::applied);physical(d,"synth.2.analog.amp.release",.01f);physical(d,"synth.2.enabled",1);physical(d,"synth.2.solo",1);
    CHECK(restore(processor,state(d)));midi.clear();processor.processBlock(audio,midi);CHECK(processor.getTailLengthSeconds()<1);physical(d,"synth.2.solo",0);physical(d,"synth.1.mute",1);CHECK(restore(processor,state(d)));processor.processBlock(audio,midi);CHECK(processor.getTailLengthSeconds()<1);
    physical(d,"synth.1.mute",0);physical(d,"synth.1.enabled",0);CHECK(restore(processor,state(d)));processor.processBlock(audio,midi);CHECK(processor.getTailLengthSeconds()<1);physical(d,"synth.1.enabled",1);CHECK(restore(processor,state(d)));processor.processBlock(audio,midi);CHECK(processor.getTailLengthSeconds()>9);
}
// The stereo keys in the reported tail: the right side's TIME * RIGHT when it is
// the longer, ping-pong's round trips and a note held at 4 s. Each render's
// residual after the note-off plus the reported tail stays under 1e-6 of its peak;
// it runs 5 s past the tail so a repeat up to 4 s apart still lands in the window.
void delayStereoReportedTail()
{
    section("delay_stereo_reported_tail");
    SoundDocument delay;
    CHECK(editRackLayout(delay.layout,{RackEditKind::addEffect,noSlot,masterChain,0,EffectKind::delay,false},delay.layout)==RackEditResult::applied);
    delay.compatibility.effects[0][delayEffectBank]=modernDelayAlgorithmRevision;
    physical(delay,"effect.1.delay.mix",1);physical(delay,"effect.1.delay.filter_hz",20000);physical(delay,"synth.1.analog.amp.release",.01f);
    const auto residual=[&](const SoundDocument& d,std::optional<double> bpm,const char* name){
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(restore(p,state(d)));Playhead head;head.bpm=bpm;p.setPlayHead(&head);p.prepareToPlay(8000,256);
        juce::AudioBuffer<float> a(2,256);juce::MidiBuffer m;note(m);m.addEvent(juce::MidiMessage::noteOff(1,60),128);p.processBlock(a,m);
        const double tail=p.getTailLengthSeconds();const int boundary=128+int(std::ceil(tail*8000));
        double loudest=peak(a),late=0;m.clear();
        for(int start=256;start<boundary+5*8000;start+=256){
            p.processBlock(a,m);
            for(int ch=0;ch<2;++ch)for(int i=0;i<256;++i){const double v=std::abs(double(a.getSample(ch,i)));loudest=std::max(loudest,v);if(start+i>=boundary)late=std::max(late,v);}
        }
        p.setPlayHead(nullptr);
        CHECK(loudest>1e-4&&late<=loudest*1e-6);
        std::cout<<"DELAY_STEREO_REPORTED_TAIL case="<<name<<" tail_seconds="<<tail<<" peak="<<loudest<<" residual="<<late<<'\n';
        return tail;};
    auto right=delay;physical(right,"effect.1.delay.seconds",1);physical(right,"effect.1.delay.feedback",.2f);physical(right,"effect.1.delay.right_ratio",4);
    residual(right,std::nullopt,"right_ratio_4");
    auto bouncing=delay;physical(bouncing,"effect.1.delay.seconds",.01f);physical(bouncing,"effect.1.delay.feedback",.95f);physical(bouncing,"effect.1.delay.ping_pong",1);
    residual(bouncing,std::nullopt,"ping_pong_095");
    auto held=delay;physical(held,"effect.1.delay.synced",1);physical(held,"effect.1.delay.note",21);physical(held,"effect.1.delay.feedback",.2f);
    residual(held,20.0,"dotted_whole_20bpm");
}
void completePublicationAndCallbacks()
{
    section("completePublicationAndCallbacks"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    auto a=fullDocument(), b=a; b.macros[0].amount=.25f; b.controlLocks[100]=true; std::swap(b.layout.synthOrder[0],b.layout.synthOrder[3]); b.values[0]=.8f;
    const auto sa=state(a),sb=state(b); CHECK(restore(p,sa));
    std::atomic<bool> done{false},good{true}; std::thread writer([&]{for(int i=0;i<200;++i) if(!restore(p,i%2?sa:sb)) good=false; done=true;});
    int reads=0; while(!done) { SoundDocument d; std::uint64_t r=0; if(p.trySoundSnapshot(d,r)) { good=good&&(d==a||d==b); ++reads; } }
    writer.join(); CHECK(good); CHECK(reads>0);
    RecallListener listener; bool callbackGood=false; listener.action=[&]{ auto d=document(p); callbackGood=(d==a); CHECK(restore(p,sb)); callbackGood &= document(p)==b; dispatch(); };
    p.control(0).addListener(&listener); CHECK(restore(p,sa)); dispatch(); dispatch(); CHECK(callbackGood); CHECK(document(p)==b); p.control(0).removeListener(&listener);
    // Metadata alone must not restart the held note or a tail.
    InstrumentProcessor x,y; CHECK(blankInstance(x)); CHECK(blankInstance(y)); x.prepareToPlay(8000,256); y.prepareToPlay(8000,256); juce::AudioBuffer<float> ax(2,256),ay(2,256); juce::MidiBuffer m; note(m); x.processBlock(ax,m); y.processBlock(ay,m);
    auto meta=document(x); meta.controlLocks[0]=true; CHECK(edit(x,meta)); m.clear(); x.processBlock(ax,m); y.processBlock(ay,m); for(int i=0;i<256;++i) CHECK(ax.getSample(0,i)==ay.getSample(0,i));
    // An invalid independent automation combination preserves last-good rendering.
    x.control(idx("synth.1.modulation.1.target")).setValue(1); x.processBlock(ax,m); y.processBlock(ay,m);
    for(int i=0;i<256;++i) CHECK(ax.getSample(0,i)==ay.getSample(0,i)); x.control(idx("synth.1.modulation.1.target")).setValue(0);
    std::atomic<bool> finished{false}, editsGood{true};
    std::thread editor([&]{ for(int n=0;n<200;++n) { SoundDocument d;std::uint64_t r; if(!x.trySoundSnapshot(d,r)){editsGood=false;break;} d.controlLocks[1]=!d.controlLocks[1]; if(!x.applySoundEdit(d,SoundEditOrigin::manual,r))editsGood=false; } finished=true; });
    bool continuous=true;int rendered=0;do { x.processBlock(ax,m);y.processBlock(ay,m);for(int i=0;i<256;++i)continuous &= ax.getSample(0,i)==ay.getSample(0,i); ++rendered; }while(!finished);
    editor.join(); CHECK(editsGood); CHECK(continuous); CHECK(rendered>0); CHECK(document(x).controlLocks[1]==false);
    std::cout<<"Contended metadata render blocks="<<rendered<<" last_good_exact="<<continuous<<'\n';
}
struct GestureListener : juce::AudioProcessorParameter::Listener
{
    InstrumentProcessor& p; int begins=0,ends=0,values=0; bool good=true;
    explicit GestureListener(InstrumentProcessor& v):p(v){}
    void parameterValueChanged(int,float value) override { ++values; good &= document(p).values[0]==value; juce::MemoryBlock b; p.getStateInformation(b); good &= b.getSize()==currentMinimumProjectStateBytes; good &= !p.beginParameterGesture(1); }
    void parameterGestureChanged(int,bool begin) override { if(begin)++begins;else ++ends; good &= !p.beginParameterGesture(1); }
};
void nativeTransactionsAndStaleEdits()
{
    section("nativeTransactionsAndStaleEdits"); auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture; CHECK(blankInstance(p)); std::uint64_t r; auto initialFixture = airack::testing::fixtureValue([&]{return document(p,&r);}); auto& initial = *initialFixture;
    auto changedFixture = airack::testing::fixtureValue([&]{return initial;}); auto& changed = *changedFixture; changed.values[0]=.8f; changed.controlLocks[0]=true; CHECK(p.applySoundEdit(changed,SoundEditOrigin::manual,r)); CHECK(!p.applySoundEdit(initial,SoundEditOrigin::manual,r));
    document(p,&r); auto badFixture = airack::testing::fixtureValue([&]{return changed;}); auto& bad = *badFixture; bad.values[0]=.1f; CHECK(!p.applySoundEdit(bad,SoundEditOrigin::aiCandidate,r)); CHECK(document(p)==changed);
    auto sameRevision=r; CHECK(!p.applySoundEdit(changed,SoundEditOrigin::manual,r)); document(p,&r); CHECK(r==sameRevision);
    CHECK(p.applySoundEdit(bad,SoundEditOrigin::manual,r)); CHECK(p.undoSoundEdit()); CHECK(document(p)==changed); CHECK(p.redoSoundEdit()); CHECK(document(p)==bad);
    CHECK(restore(p,state(initial))); document(p,&r); GestureListener listener(p); p.control(0).addListener(&listener);
    CHECK(p.beginParameterGesture(0)); CHECK(!p.beginParameterGesture(0)); CHECK(!p.beginParameterGesture(1)); CHECK(!p.setParameterGestureValue(1,.2f));
    CHECK(p.setParameterGestureValue(0,.7f)); std::uint64_t during; document(p,&during); CHECK(during==r+1); CHECK(!p.applySoundEdit(changed,SoundEditOrigin::manual,r));
    CHECK(p.setParameterGestureValue(0,.8f)); CHECK(!p.setParameterGestureValue(0,NAN)); CHECK(p.endParameterGesture(0)); CHECK(!p.endParameterGesture(0));
    CHECK(listener.begins==1&&listener.ends==1); CHECK(listener.values==2); CHECK(listener.good); CHECK(p.undoSoundEdit()); CHECK(document(p)==initial); CHECK(!p.undoSoundEdit()); CHECK(p.redoSoundEdit()); CHECK(document(p).values[0]==.8f);
    CHECK(restore(p,state(initial))); document(p,&r); CHECK(p.beginParameterGesture(0)); CHECK(p.endParameterGesture(0)); document(p,&during); CHECK(during==r); CHECK(!p.undoSoundEdit());
    CHECK(p.beginParameterGesture(0)); CHECK(p.setParameterGestureValue(0,.7f)); CHECK(p.setParameterGestureValue(0,initial.values[0])); CHECK(p.endParameterGesture(0)); CHECK(!p.undoSoundEdit());
    CHECK(p.beginParameterGesture(0)); CHECK(p.setParameterGestureValue(0,.7f)); CHECK(restore(p,state(changed))); CHECK(!p.setParameterGestureValue(0,.2f)); CHECK(p.endParameterGesture(0)); CHECK(document(p)==changed); CHECK(!p.undoSoundEdit());
    CHECK(p.beginParameterGesture(0)); CHECK(p.setParameterGestureValue(0,.6f)); CHECK(p.endParameterGesture(0)); CHECK(p.undoSoundEdit()); CHECK(document(p)==changed); CHECK(listener.begins==listener.ends); p.control(0).removeListener(&listener);
    document(p,&r); p.control(0).setValue(.4f); document(p,&during); CHECK(during==r); CHECK(document(p).values[0]==.4f);
    auto rackFixture = airack::testing::fixtureValue([&]{return fullDocument();}); auto& rack = *rackFixture; CHECK(restore(p,state(rack))); document(p,&r); CHECK(p.applyRackEdit({RackEditKind::addSynth},r)==RackEditResult::capacityReached);
    CHECK(p.applyRackEdit({RackEditKind::removeSynth,3},r)==RackEditResult::applied); document(p,&r);
    CHECK(p.applyRackEdit({RackEditKind::addSynth},r)==RackEditResult::replacementRequired);
    RackEdit reuse; reuse.slot=3; reuse.allowReuse=true; CHECK(p.applyRackEdit(reuse,r)==RackEditResult::applied); CHECK(document(p).values==rack.values);
    auto protectedRackFixture = airack::testing::fixtureValue([&]{return document(p,&r);}); auto& protectedRack = *protectedRackFixture; protectedRack.controlLocks[idx("synth.1.level")]=true; CHECK(edit(p,protectedRack)); document(p,&r);
    auto removalFixture = airack::testing::fixtureValue([&]{return protectedRack;}); auto& removal = *removalFixture; CHECK(editRackLayout(removal.layout,{RackEditKind::removeSynth,0},removal.layout)==RackEditResult::applied);
    CHECK(!p.applySoundEdit(removal,SoundEditOrigin::aiCandidate,r)); CHECK(document(p)==protectedRack);
    CHECK(p.applySoundEdit(removal,SoundEditOrigin::manual,r)); CHECK(p.undoSoundEdit()); CHECK(document(p)==protectedRack);
    auto protectedEffectFixture = airack::testing::fixtureValue([&]{return protectedRack;}); auto& protectedEffect = *protectedEffectFixture; protectedEffect.controlLocks[idx("effect.1.drive.mix")]=true; CHECK(edit(p,protectedEffect)); document(p,&r);
    auto replacementFixture = airack::testing::fixtureValue([&]{return protectedEffect;}); auto& replacement = *replacementFixture; CHECK(editRackLayout(replacement.layout,{RackEditKind::replaceEffect,0,masterChain,0,EffectKind::delay,true},replacement.layout)==RackEditResult::applied);
    CHECK(!p.applySoundEdit(replacement,SoundEditOrigin::aiCandidate,r)); CHECK(document(p)==protectedEffect);
}
juce::File testWorkspace()
{
    auto directory=juce::File::getCurrentWorkingDirectory();
    for(int i=0;i<8;++i) {
        if(directory.getChildFile("tests/fixtures/m1-analog-pluck.soundstate").existsAsFile())return directory;
        directory=directory.getParentDirectory();
    }
    CHECK(false);return {};
}
void undoLocksAndPresets()
{
    section("undoLocksAndPresets"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; CHECK(blankInstance(p)); const auto initial=document(p); auto d=fullDocument(); d.name="sound"; CHECK(edit(p,d)); CHECK(p.undoSoundEdit()); CHECK(document(p)==initial); CHECK(p.redoSoundEdit()); CHECK(document(p)==d);
    const auto dir=testWorkspace().getChildFile("build/sound-quality-task3-native-test-artifacts/task-3-presets"); CHECK(dir.createDirectory());
    const auto target=dir.getChildFile("sound.arsi"),bad=dir.getChildFile("bad.arsi"); CHECK(p.saveSoundPreset(target)); juce::MemoryBlock original; CHECK(target.loadFileAsData(original)); CHECK(original.getSize()==currentSoundStateSize+5); if(original.getSize()!=currentSoundStateSize+5) return;
    CHECK(edit(p,initial)); CHECK(p.loadSoundPreset(target)); CHECK(document(p)==d); CHECK(p.undoSoundEdit()); CHECK(document(p)==initial); CHECK(p.redoSoundEdit()); CHECK(document(p)==d);
    auto broken=original; static_cast<char*>(broken.getData())[50]^=1; CHECK(bad.replaceWithData(broken.getData(),broken.getSize())); CHECK(!p.loadSoundPreset(bad)); CHECK(document(p)==d);
    broken=original; broken.append("x",1); CHECK(bad.replaceWithData(broken.getData(),broken.getSize())); CHECK(!p.loadSoundPreset(bad)); CHECK(document(p)==d); CHECK(!p.loadSoundPreset(dir));
    CHECK(!p.saveSoundPreset(dir));
    CHECK(target.setReadOnly(true)); CHECK(!p.saveSoundPreset(target)); CHECK(target.setReadOnly(false));
    // Invalid independent host route coordinates cannot overwrite a valid preset.
    p.control(idx("synth.1.modulation.1.target")).setValue(1);
    CHECK(!p.saveSoundPreset(target)); p.control(idx("synth.1.modulation.1.target")).setValue(0);
    juce::MemoryBlock preserved; CHECK(target.loadFileAsData(preserved)); CHECK(preserved==original); CHECK(p.undoSoundEdit()); CHECK(document(p)==initial);
    CHECK(p.loadSoundPreset(testWorkspace().getChildFile("tests/fixtures/m1-analog-pluck.soundstate"))); CHECK(document(p).layout.synthCount==4); CHECK(p.undoSoundEdit()); CHECK(document(p)==initial);
}

void presetNames()
{
    section("presetNames"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; CHECK(blankInstance(p));
    const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
    CHECK(juce::File::isAbsolutePath(root)); if(!juce::File::isAbsolutePath(root)) return;
    const auto folder=juce::File(root).getChildFile("Sounds"); folder.deleteRecursively(); CHECK(p.soundFolder()==folder);
    const auto dir=juce::File(root).getChildFile("preset-names"); dir.deleteRecursively(); CHECK(dir.createDirectory());
    // LOAD takes the stored name, else the file's stem, and the folder follows the preset.
    auto stored=document(p); stored.name="Ethereal Rolling Bass"; auto bytes=state(stored);
    const auto storedFile=dir.getChildFile("Stored.airack"); CHECK(storedFile.replaceWithData(bytes.getData(),bytes.getSize()));
    CHECK(p.loadSoundPreset(storedFile)); CHECK(document(p).name=="Ethereal Rolling Bass" && p.soundFolder()==dir);
    auto plain=stored; plain.name={}; bytes=state(plain);
    const auto plainFile=dir.getChildFile("Glass Keys.airack"); CHECK(plainFile.replaceWithData(bytes.getData(),bytes.getSize()));
    CHECK(p.loadSoundPreset(plainFile)); CHECK(document(p).name=="Glass Keys");
    // SAVE keeps a name that matches the stem ignoring case, otherwise renames the sound in one undoable edit.
    CHECK(p.saveSoundPreset(dir.getChildFile("glass keys.airack"))); CHECK(document(p).name=="Glass Keys");
    CHECK(p.undoSoundEdit()); CHECK(document(p).name=="Ethereal Rolling Bass"); CHECK(p.redoSoundEdit());
    const auto nested=dir.getChildFile("New Folder").getChildFile("Warm Pad.airack");
    CHECK(p.saveSoundPreset(nested)); CHECK(nested.existsAsFile() && document(p).name=="Warm Pad" && p.soundFolder()==nested.getParentDirectory());
    juce::MemoryBlock written; CHECK(nested.loadFileAsData(written)); SoundDocument saved;
    CHECK(decodeSoundState(written.getData(),written.getSize(),saved)); CHECK(saved.name=="Warm Pad");
    CHECK(p.undoSoundEdit()); CHECK(document(p).name=="Glass Keys"); CHECK(p.redoSoundEdit()); CHECK(document(p).name=="Warm Pad");
    dir.deleteRecursively();
}

void invalidAutomationRecoveryHistory()
{
    section("invalidAutomationRecoveryHistory");
    const auto target=idx("synth.1.modulation.1.target");
    for(int mode=0;mode<4;++mode)
    {
        auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture; CHECK(blankInstance(p)); auto initialFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& initial = *initialFixture; auto olderFixture = airack::testing::fixtureValue([&]{return initial;}); auto& older = *olderFixture;older.values[0]=.65f;CHECK(edit(p,older));
        p.control(target).setValue(1); CHECK(!validateSoundDocument(document(p)));
        auto repairedFixture = airack::testing::fixtureValue([&]{return older;}); auto& repaired = *repairedFixture;repaired.values[0]=.8f;
        RecallListener listener;listener.action=[]{};p.control(mode==1?target:0).addListener(&listener);
        if(mode==0) { CHECK(p.beginParameterGesture(0));CHECK(p.setParameterGestureValue(0,.8f));CHECK(p.endParameterGesture(0));CHECK(listener.gestures==2); }
        else if(mode==1) {
            repaired=older;repaired.values[target]=1.f/16;
            CHECK(p.beginParameterGesture(target));CHECK(p.setParameterGestureValue(target,1.f/16));CHECK(p.endParameterGesture(target));CHECK(listener.gestures==2);
        }
        else if(mode==2) CHECK(edit(p,repaired));
        else {
            const auto dir=testWorkspace().getChildFile("build/sound-quality-task3-native-test-artifacts/task-3-recovery-presets");CHECK(dir.createDirectory());
            const auto file=dir.getChildFile("recover.arsi");const auto bytes=state(repaired);CHECK(file.replaceWithData(bytes.getData(),bytes.getSize()));
            CHECK(p.loadSoundPreset(file));repaired.name="recover";
        }
        CHECK(document(p)==repaired);CHECK(validateSoundDocument(document(p)));
        CHECK(p.undoSoundEdit());CHECK(document(p)==older);CHECK(validateSoundDocument(document(p)));
        CHECK(p.undoSoundEdit());CHECK(document(p)==initial);CHECK(!p.undoSoundEdit());
        CHECK(p.redoSoundEdit());CHECK(document(p)==older);CHECK(p.redoSoundEdit());CHECK(document(p)==repaired);CHECK(!p.redoSoundEdit());
        p.control(mode==1?target:0).removeListener(&listener);
    }
    // An invalid automation combination arriving after accepted movement must
    // not erase that movement or its older transaction at gesture end.
    auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture; CHECK(blankInstance(p)); auto initialFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& initial = *initialFixture;auto olderFixture = airack::testing::fixtureValue([&]{return initial;}); auto& older = *olderFixture;older.values[0]=.65f;CHECK(edit(p,older));
    RecallListener listener;listener.action=[]{};p.control(0).addListener(&listener);std::uint64_t before,after;document(p,&before);
    CHECK(p.beginParameterGesture(0));CHECK(p.setParameterGestureValue(0,.8f));auto intendedFixture = airack::testing::fixtureValue([&]{return older;}); auto& intended = *intendedFixture;intended.values[0]=.8f;
    p.control(target).setValue(1);CHECK(!validateSoundDocument(document(p)));CHECK(p.endParameterGesture(0));
    CHECK(listener.gestures==2);CHECK(document(p,&after)==intended);CHECK(after==before+1);
    CHECK(p.undoSoundEdit());CHECK(document(p)==older);CHECK(p.undoSoundEdit());CHECK(document(p)==initial);
    CHECK(p.redoSoundEdit());CHECK(document(p)==older);CHECK(p.redoSoundEdit());CHECK(document(p)==intended);p.control(0).removeListener(&listener);
    // Repairing back to the valid before intent is a real publication but no
    // net-change history entry; the previous transaction remains undoable.
    CHECK(restore(p,state(initial)));CHECK(edit(p,older));p.control(target).setValue(1);
    CHECK(p.beginParameterGesture(target));CHECK(p.setParameterGestureValue(target,0));CHECK(p.endParameterGesture(target));
    CHECK(document(p)==older);CHECK(p.undoSoundEdit());CHECK(document(p)==initial);CHECK(!p.undoSoundEdit());
}
void recoveryEqualsIntentAndBankRepair()
{
    section("recoveryEqualsIntentAndBankRepair");InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));const auto initial=document(p);auto older=initial;older.values[0]=.65f;
    const auto target=idx("synth.1.modulation.1.target"),bank=idx("synth.1.modulation.1.bank");CHECK(edit(p,older));
    p.control(target).setValue(1);CHECK(edit(p,older));CHECK(document(p)==older);
    CHECK(p.undoSoundEdit());CHECK(document(p)==initial);CHECK(!p.undoSoundEdit());CHECK(p.redoSoundEdit());CHECK(document(p)==older);
    p.control(target).setValue(1);auto repaired=older;repaired.values[target]=1;repaired.values[bank]=.5f;
    CHECK(p.beginParameterGesture(bank));CHECK(p.setParameterGestureValue(bank,.5f));CHECK(p.endParameterGesture(bank));CHECK(document(p)==repaired);
    CHECK(p.undoSoundEdit());CHECK(document(p)==older);CHECK(p.undoSoundEdit());CHECK(document(p)==initial);CHECK(p.redoSoundEdit());CHECK(p.redoSoundEdit());CHECK(document(p)==repaired);
    CHECK(restore(p,state(initial)));std::uint64_t before,after;document(p,&before);CHECK(p.beginParameterGesture(0));p.control(target).setValue(1);
    const auto invalid=document(p);CHECK(p.endParameterGesture(0));CHECK(document(p,&after)==invalid);CHECK(after==before);CHECK(!p.undoSoundEdit());
    CHECK(edit(p,initial));CHECK(document(p)==initial);CHECK(!p.undoSoundEdit());
}
void disabledSoloInsertTail()
{
    section("disabledSoloInsertTail");SoundDocument d;
    CHECK(editRackLayout(d.layout,{RackEditKind::addSynth},d.layout)==RackEditResult::applied);
    CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,noSlot,0,0,EffectKind::delay,false},d.layout)==RackEditResult::applied);
    physical(d,"synth.2.enabled",0);physical(d,"synth.2.solo",1);
    physical(d,"effect.1.delay.seconds",4);physical(d,"effect.1.delay.feedback",.95f);physical(d,"effect.1.delay.mix",1);physical(d,"synth.1.analog.amp.release",.01f);
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(restore(p,state(d)));p.prepareToPlay(8000,256);
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;float late=0;
    for(int start=0;start<25*8000;start+=256) {
        midi.clear();if(start==0){note(midi);midi.addEvent(juce::MidiMessage::noteOff(1,60),128);}
        p.processBlock(audio,midi);if(start>=20*8000)late=std::max(late,peak(audio));
    }
    CHECK(late>1e-5f);CHECK(p.getTailLengthSeconds()>25);const auto longTail=p.getTailLengthSeconds();
    std::cout<<"Disabled solo insert late_peak="<<late<<" estimate_seconds="<<longTail<<'\n';
    setPhysical(p,"synth.2.enabled",1);midi.clear();p.processBlock(audio,midi);CHECK(p.getTailLengthSeconds()<1);
    setPhysical(p,"synth.2.solo",0);p.processBlock(audio,midi);CHECK(p.getTailLengthSeconds()>25);
    setPhysical(p,"synth.2.solo",1);setPhysical(p,"synth.2.enabled",0);p.processBlock(audio,midi);CHECK(p.getTailLengthSeconds()>25);
    setPhysical(p,"synth.1.enabled",0);p.processBlock(audio,midi);CHECK(p.getTailLengthSeconds()<1);
    setPhysical(p,"synth.1.enabled",1);p.processBlock(audio,midi);CHECK(p.getTailLengthSeconds()==longTail);
}

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking = false;
thread_local int allocations = 0;
thread_local int audioFrees = 0;
int __cdecl allocationHook(int kind, void*, size_t, int, long, const unsigned char*, int)
{ if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE||kind==_HOOK_REALLOC)++audioFrees;} return 1; }
#endif
void allocationCheck()
{
    section("prepared_process_allocates_zero"); InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; auto full=fullDocument(); CHECK(restore(p,state(full))); p.prepareToPlay(48000,256);
    for (std::size_t i = 0; i < 4; ++i) { p.control(12 + i * 4).setValue(1); p.control(13 + i * 4).setValue(static_cast<float>(i % 2)); }
    juce::AudioBuffer<float> b(2, 256); juce::MidiBuffer midi;
    for (int n = 48; n < 64; ++n) midi.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
    p.processBlock(b, midi); midi.clear();
#if defined(_MSC_VER) && defined(_DEBUG)
    auto previous = _CrtSetAllocHook(allocationHook); tracking = true;
    for (int i = 0; i < 100; ++i)
    { p.control(211).setValue(static_cast<float>(i % 10) / 10); p.processBlock(b, midi); }
    tracking = false;
    midi.addEvent(juce::MidiMessage::allNotesOff(1), 17);
    tracking = true;
    p.processBlock(b, midi); midi.clear();
    for (int i = 0; i < 20; ++i)
    { p.control(0).setValue(.4f + .01f * i); p.processBlock(b, midi); }
    tracking = false;
    for(int i=0;i<20;++i) {
        auto d=full; d.controlLocks[0]=i%2!=0; d.values[1]=float(i%10)/10;
        if(i%2) CHECK(restore(p,state(d))); else CHECK(edit(p,d));
        note(midi); tracking=true; p.processBlock(b,midi); tracking=false; midi.clear();
    }
    _CrtSetAllocHook(previous); CHECK(allocations == 0);
    std::cout << "Audio allocations=" << allocations << '\n';
#else
    CHECK(false); // This acceptance target requires the Windows Debug CRT hook.
#endif
}
void saturationReleaseRouting()
{
    section("saturation_release_all_controls_automation_recall_lifecycle");
    CHECK(juce::ComponentPeer::getNumPeers()==0);
    CHECK(sizeof(InstrumentProcessor)<=128*1024);
    std::cout<<"SATURATION_PROCESSOR sizeof="<<sizeof(InstrumentProcessor)<<" limit="<<128*1024<<'\n';
    for(const auto& factory:airack::test::frozenFactoryRackDesigns())if(factory.units[0].module==NativeModule::saturation){
        SoundDocument source,next;RackOperation op;op.design=factory;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};op.allocation={{0,6}};RackDifference delta;
        CHECK(makeRackCandidate(source,op,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);if(!next.personalities)continue;
        const auto& instance=next.personalities->instances[0];CHECK(!rackOn(next,instance));CHECK(rackHeaderText(next,instance).contains(" (bypassed)"));
        next.values[572+6*23]=0;CHECK(rackOn(next,instance));CHECK(!rackHeaderText(next,instance).contains(" (bypassed)"));
    }
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto processor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*processor;auto d=copperArcSound();physical(d,"output.gain_db",0);physical(d,"output.protection",0);
        d.values[758]=quality==RenderQuality::higher?1.f:0.f;
        for(std::uint8_t slot=0;slot<8;++slot){RackEdit add;add.kind=RackEditKind::addEffect;add.slot=slot;add.effect=EffectKind::saturation;add.destination=masterChain;add.position=slot;
            CHECK(editRackLayout(d.layout,add,d.layout)==RackEditResult::applied);d.values[572+slot*23]=0;}
        CHECK(restore(p,state(d)));const auto registry=parameterRegistry();
        for(std::uint8_t slot=0;slot<8;++slot)for(std::size_t field=0;field<35;++field){const auto i=saturationParameterStart+slot*35+field;const auto& descriptor=registry[i];
            CHECK(p.control(i).isAutomatable()&&p.control(i).paramID==descriptor.id);
            const auto value=descriptor.choices.empty()?float((slot*7+field*3)%61+11)/100.f:float((slot+field)%descriptor.choices.size())/float(descriptor.choices.size()-1);
            p.control(i).setValueNotifyingHost(value);CHECK(p.control(i).getValue()==value);}
        auto automated=document(p);juce::MemoryBlock host;p.getStateInformation(host);auto loadedStorage=std::make_unique<ProjectDocument>();auto& loaded=*loadedStorage;
        CHECK(decodeProjectState(host.getData(),host.getSize(),loaded)&&loaded.sound==automated);
        auto recalledStorage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& recalled=*recalledStorage;CHECK(recalled.restoreProjectState(host.getData(),int(host.getSize())));CHECK(document(recalled)==automated);
        // A real grouped edit owns undo; automation still owns the same stable key.
        const auto drive=saturationParameterStart+13;const auto initial=p.control(drive).getValue();
        CHECK(p.beginParameterGesture(drive));CHECK(p.setParameterGestureValue(drive,.82f));CHECK(p.endParameterGesture(drive));
        CHECK(p.control(drive).getValue()==.82f);CHECK(p.undoSoundEdit()&&p.control(drive).getValue()==initial);CHECK(p.redoSoundEdit()&&p.control(drive).getValue()==.82f);
        auto macro=document(p);macro.macros[0]={registry[drive].stateKey,.25f};macro.values[1]=.4f;macro.controlLocks[drive]=true;macro.effectLocks[3]=true;
        CHECK(edit(p,macro));auto candidate=macro;candidate.values[drive]=.2f;CHECK(!validateAiSoundEdit(macro,candidate));candidate=macro;candidate.values[saturationParameterStart+3*35+34]=.2f;CHECK(!validateAiSoundEdit(macro,candidate));
        std::uint64_t oldRevision=0;const auto stale=document(p,&oldRevision);auto newer=stale;newer.values[saturationParameterStart+2]=.41f;
        CHECK(p.applySoundEdit(newer,SoundEditOrigin::manual,oldRevision));
        CHECK(!p.applySoundEdit(stale,SoundEditOrigin::manual,oldRevision));
        const auto retained=document(p);CHECK(restore(p,state(retained)));CHECK(document(p)==retained);
        p.setNonRealtime(quality==RenderQuality::higher);p.prepareToPlay(48000,64);CHECK(p.effectiveRenderQuality()==quality);const auto latency=p.getLatencySamples();
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(512);note(midi);
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations=audioFrees=0;const auto hook=_CrtSetAllocHook(allocationHook);tracking=true;
#endif
        p.processBlock(audio,midi);midi.clear();
        for(unsigned change=0;change<12;++change){for(std::uint8_t slot=0;slot<8;++slot){const auto base=saturationParameterStart+slot*35;
                p.control(base).setValue(float(change%2));p.control(base+4).setValue(float(change%2));p.control(base+7).setValue(float((change+1)%2));p.control(base+9).setValue(float(change%2));
                for(std::size_t lane=0;lane<4;++lane){p.control(base+11+6*lane).setValue(float((change+lane)%2));p.control(base+12+6*lane).setValue(float((change+lane)%3)*.5f);p.control(base+13+6*lane).setValue(float(change)/12);}}
            p.processBlock(audio,midi);}
#if defined(_MSC_VER) && defined(_DEBUG)
        tracking=false;_CrtSetAllocHook(hook);CHECK(allocations==0&&audioFrees==0);std::cout<<"SATURATION_WRAPPER quality="<<int(quality)<<" allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#endif
        CHECK(p.getLatencySamples()==latency);CHECK(std::isfinite(p.getTailLengthSeconds())&&p.getTailLengthSeconds()>=16);
        note(midi);p.processBlock(audio,midi);midi.clear();midi.addEvent(juce::MidiMessage::midiStop(),0);p.processBlock(audio,midi);midi.clear();
        for(int n=0;n<4;++n)p.processBlock(audio,midi);CHECK(peak(audio)==0);
        Playhead playhead;playhead.playing=true;p.setPlayHead(&playhead);note(midi);p.processBlock(audio,midi);midi.clear();playhead.playing=false;p.processBlock(audio,midi);
        for(int n=0;n<4;++n)p.processBlock(audio,midi);CHECK(peak(audio)==0);p.setPlayHead(nullptr);
        p.releaseResources();p.processBlock(audio,midi);CHECK(peak(audio)==0);p.prepareToPlay(384000,64);CHECK(p.effectiveRenderQuality()==quality);p.processBlock(audio,midi);CHECK(peak(audio)==0);p.releaseResources();
        CHECK(document(p).values[saturationParameterStart+34]==p.control(saturationParameterStart+34).getValue());
    }
    CHECK(juce::ComponentPeer::getNumPeers()==0);
}
void saturationAuditionAuthority()
{
    section("saturation_transient_processor_identity_revision_reset_and_thread_gate");
    for(const auto& factory:airack::test::frozenFactoryRackDesigns())if(factory.units[0].module==NativeModule::saturation){
        auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto source=std::make_unique<SoundDocument>();auto placed=std::make_unique<SoundDocument>();
        RackOperation op;op.design=factory;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,0};op.allocation={{0,6}};RackDifference delta;
        CHECK(makeRackCandidate(*source,op,SoundEditOrigin::manual,*placed,delta)==RackCandidateStatus::ready);
        placed->values[572+6*23]=0;placed->values[saturationParameterStart+6*35]=1;CHECK(restore(*p,state(*placed)));
        SoundDocument captured;std::uint64_t revision,reset;CHECK(p->trySoundSnapshot(captured,revision,reset));const auto rack=captured.personalities->instances[0].id;
        CHECK(!p->acquireSaturationAudition(6,rack,revision,reset));p->prepareToPlay(48000,64);
        CHECK(!p->acquireSaturationAudition(6,juce::Uuid(),revision,reset));CHECK(!p->acquireSaturationAudition(6,rack,revision+1,reset));CHECK(!p->acquireSaturationAudition(6,rack,revision,reset+1));
        CHECK(!p->acquireSaturationAudition(7,rack,revision,reset));
        auto token=p->acquireSaturationAudition(6,rack,revision,reset);CHECK(token);if(!token)continue;
        const auto before=state(captured);CHECK(p->requestSaturationAudition(*token,SaturationLane::low,rack,revision,reset));
        CHECK(!p->requestSaturationAudition(*token,SaturationLane::full,rack,revision,reset));
        p->retireSaturationAudition(*token);CHECK(!p->requestSaturationAudition(*token,SaturationLane::high,rack,revision,reset));
        CHECK(state(document(*p))==before);
        for(int invalidation=0;invalidation<6;++invalidation){CHECK(p->trySoundSnapshot(captured,revision,reset));
            token=p->acquireSaturationAudition(6,rack,revision,reset);CHECK(token);if(!token)continue;
            CHECK(p->requestSaturationAudition(*token,SaturationLane::mid,rack,revision,reset));
            if(invalidation==0){std::atomic<bool> accepted=true;std::thread worker([&]{accepted=p->requestSaturationAudition(*token,SaturationLane::high,rack,revision,reset);});worker.join();CHECK(!accepted.load());}
            if(invalidation==1){auto changed=captured;changed.values[1]=.2f;CHECK(p->applySoundEdit(changed,SoundEditOrigin::manual,revision));}
            if(invalidation==2){p->control(saturationParameterStart+6*35).setValueNotifyingHost(0);CHECK(!p->requestSaturationAudition(*token,SaturationLane::high,rack,revision,reset));p->control(saturationParameterStart+6*35).setValueNotifyingHost(1);}
            if(invalidation==3){p->control(572+6*23).setValueNotifyingHost(1);CHECK(!p->requestSaturationAudition(*token,SaturationLane::high,rack,revision,reset));p->control(572+6*23).setValueNotifyingHost(0);}
            if(invalidation==4)CHECK(restore(*p,state(captured)));
            if(invalidation==5)p->releaseResources();
            CHECK(!p->requestSaturationAudition(*token,SaturationLane::low,rack,revision,reset));
            p->retireSaturationAudition(*token);
        }
        CHECK(p->fxMonitorHandle()->stats(6).published==0);CHECK(juce::ComponentPeer::getNumPeers()==0);
    }
}
std::unique_ptr<SoundDocument> saturationTestSound()
{
    auto d=std::make_unique<SoundDocument>(copperArcSound());d->name="Saturation Target";
    physical(*d,"output.gain_db",0);physical(*d,"output.protection",0);
    CHECK(editRackLayout(d->layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::saturation},d->layout)==RackEditResult::applied);
    const auto factories=airack::test::frozenFactoryRackDesigns();for(const auto& value:factories[19].units[0].defaults){const auto i=rackParameterIndex(NativeModule::saturation,0,value.key);CHECK(i);if(i)d->values[*i]=value.normalized;}
    d->values[572]=0;CHECK(validateSoundDocument(*d));return d;
}
using SaturationAudio=std::array<std::vector<float>,2>;
SaturationAudio saturationStream(InstrumentProcessor& p,int frames=2048,bool startNote=true)
{
    SaturationAudio result;for(auto& channel:result)channel.resize(std::size_t(frames));
    juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(256);bool finite=true;
    for(int first=0;first<frames;first+=64){const auto count=std::min(64,frames-first);audio.setSize(2,count,false,false,true);midi.clear();if(first==0&&startNote)note(midi);p.processBlock(audio,midi);
        for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n){const auto value=audio.getSample(ch,n);finite&=std::isfinite(value);result[std::size_t(ch)][std::size_t(first+n)]=value;}}
    CHECK(finite);return result;
}
double saturationDifference(const SaturationAudio& a,const SaturationAudio& b)
{return std::max(maxDifference(a[0],b[0]),maxDifference(a[1],b[1]));}
double saturationEnergy(const SaturationAudio& audio)
{double energy=0;for(const auto& channel:audio)for(const auto value:channel)energy+=std::abs(value);return energy;}
void saturationRegisteredControlAudio()
{
    section("engaged_registered_saturation_drive_edit_changes_product_audio");
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto source=saturationTestSound();source->values[758]=quality==RenderQuality::higher?1.f:0.f;
        auto reference=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),edited=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
        CHECK(restore(*reference,state(*source))&&restore(*edited,state(*source)));
        const auto drive=rackParameterIndex(NativeModule::saturation,0,"saturation.full.drive_db");CHECK(drive);if(!drive)continue;
        const auto before=airack::testing::fixtureValue([&]{return document(*edited);});
        edited->control(*drive).setValueNotifyingHost(edited->control(*drive).convertTo0to1(30));
        auto expected=std::make_unique<SoundDocument>(*before);expected->values[*drive]=edited->control(*drive).convertTo0to1(30);
        CHECK(document(*edited)==*expected);CHECK(document(*reference)==*source);CHECK(reference->control(572).getValue()==0&&edited->control(572).getValue()==0);
        reference->setNonRealtime(quality==RenderQuality::higher);edited->setNonRealtime(quality==RenderQuality::higher);
        reference->prepareToPlay(48000,64);edited->prepareToPlay(48000,64);
        const auto a=saturationStream(*reference),b=saturationStream(*edited);const auto energy=saturationEnergy(a),difference=saturationDifference(a,b);
        CHECK(energy>1e-6);CHECK(difference>1e-6);
        std::cout<<"SATURATION_REGISTERED_EDIT quality="<<int(quality)<<" id="<<parameterRegistry()[*drive].id<<" reference_energy="<<energy<<" maximum_difference="<<difference<<" both_bypasses_off=1\n";
    }
}
void saturationChargedRecall()
{
    section("charged_saturation_sound_project_and_native_preset_recall_match_fresh_target");
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher})for(const int recall:{0,1,2}){
        auto target=saturationTestSound();target->values[758]=quality==RenderQuality::higher?1.f:0.f;
        auto source=std::make_unique<SoundDocument>(*target);source->name="Charged Saturation";
        physical(*source,"effect.1.saturation.full.drive_db",36);physical(*source,"effect.1.saturation.full.trim_db",9);physical(*source,"effect.1.saturation.pre_low_cut_hz",90);
        auto charged=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),fresh=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());juce::MemoryBlock bytes;
        CHECK(encodeSoundState(*target,bytes));juce::File preset;
        if(recall==1){auto project=std::make_unique<ProjectDocument>();project->sound=*target;CHECK(encodeProjectState(*project,bytes));}
        if(recall==2){const auto directory=testWorkspace().getChildFile("build/saturation-release-mvp-20261026/task2-fix1/native-presets").getChildFile(juce::Uuid().toString());CHECK(directory.createDirectory());
            preset=directory.getChildFile("Saturation Target.airack");CHECK(!preset.exists());CHECK(restore(*charged,state(*target)));CHECK(charged->saveSoundPreset(preset));CHECK(document(*charged)==*target);CHECK(preset.existsAsFile());}
        CHECK(restore(*charged,state(*source))&&restore(*fresh,state(*target)));
        charged->setNonRealtime(quality==RenderQuality::higher);fresh->setNonRealtime(quality==RenderQuality::higher);charged->prepareToPlay(8000,64);fresh->prepareToPlay(8000,64);
        const auto excitation=saturationEnergy(saturationStream(*charged));CHECK(excitation>1e-6);
        CHECK(recall==2?charged->loadSoundPreset(preset):recall==1?charged->restoreProjectState(bytes.getData(),int(bytes.getSize())):charged->restoreSoundState(bytes.getData(),int(bytes.getSize())));
        CHECK(document(*charged)==*target&&document(*fresh)==*target);
        // Recall consumes its reset on the next callback; equal empty input also
        // admits the existing 20 ms transition interval before the target note.
        const auto cleared=saturationStream(*charged,512,false),clean=saturationStream(*fresh,512,false);const auto resetError=saturationDifference(cleared,clean);
        const auto a=saturationStream(*charged),b=saturationStream(*fresh);const auto targetEnergy=saturationEnergy(b),error=saturationDifference(a,b);
        CHECK(saturationEnergy(cleared)==0&&saturationEnergy(clean)==0);CHECK(resetError<1e-6);CHECK(targetEnergy>1e-6);CHECK(error<1e-6);
        CHECK(charged->getLatencySamples()==fresh->getLatencySamples());CHECK(charged->effectiveRenderQuality()==quality);
        std::cout<<"SATURATION_CHARGED_RECALL kind="<<recall<<" quality="<<int(quality)<<" rate=8000 excitation="<<excitation<<" reset_error="<<resetError<<" target_energy="<<targetEnergy<<" maximum_error="<<error<<" latency="<<charged->getLatencySamples()<<" preset="<<preset.getFullPathName()<<'\n';
    }
}
void saturationSameInstanceQuality()
{
    section("same_charged_saturation_instance_realtime_higher_realtime_reprepare");
    auto target=saturationTestSound();auto retained=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(restore(*retained,state(*target)));retained->prepareToPlay(8000,64);
    CHECK(saturationEnergy(saturationStream(*retained))>1e-6);
    for(const auto quality:{RenderQuality::higher,RenderQuality::realtime}){
        retained->control(758).setValue(quality==RenderQuality::higher?1.f:0.f);retained->setNonRealtime(quality==RenderQuality::higher);retained->prepareToPlay(8000,64);
        target->values[758]=quality==RenderQuality::higher?1.f:0.f;auto fresh=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(restore(*fresh,state(*target)));fresh->setNonRealtime(quality==RenderQuality::higher);fresh->prepareToPlay(8000,64);
        const auto empty=saturationStream(*retained,512,false),clean=saturationStream(*fresh,512,false);CHECK(saturationEnergy(empty)==0&&saturationEnergy(clean)==0);
        const auto actual=saturationStream(*retained),expected=saturationStream(*fresh);const auto energy=saturationEnergy(expected),error=saturationDifference(actual,expected);
        CHECK(energy>1e-6&&error<1e-6);CHECK(document(*retained)==*target);CHECK(retained->effectiveRenderQuality()==quality&&fresh->effectiveRenderQuality()==quality);
        const auto latency=fresh->getLatencySamples();CHECK(latency==(quality==RenderQuality::higher?21:0));CHECK(retained->getLatencySamples()==latency);
        std::cout<<"SATURATION_SAME_INSTANCE_QUALITY quality="<<int(quality)<<" rate=8000 target_energy="<<energy<<" maximum_error="<<error<<" latency="<<latency<<'\n';
    }
}
void saturationEmptyMidiTail()
{
    section("saturation_excitation_note_off_then_true_empty_midi_conservative_tail");
    for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        auto sound=saturationTestSound();sound->values[758]=quality==RenderQuality::higher?1.f:0.f;
        physical(*sound,"effect.1.saturation.input_trim_db",24);physical(*sound,"effect.1.saturation.output_trim_db",12);
        physical(*sound,"effect.1.saturation.pre_low_cut_enabled",1);physical(*sound,"effect.1.saturation.pre_low_cut_hz",20);
        physical(*sound,"effect.1.saturation.post_high_cut_enabled",1);physical(*sound,"effect.1.saturation.post_high_cut_hz",1000);
        physical(*sound,"effect.1.saturation.full.drive_db",36);physical(*sound,"effect.1.saturation.full.trim_db",18);
        auto processor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(restore(*processor,state(*sound)));processor->setNonRealtime(quality==RenderQuality::higher);processor->prepareToPlay(8000,64);
        const auto excitation=saturationEnergy(saturationStream(*processor,2048));CHECK(excitation>1e-6);
        const auto tail=processor->getTailLengthSeconds();CHECK(std::isfinite(tail)&&tail>=2);
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOff(1,60),0);processor->processBlock(audio,midi);midi.clear();CHECK(midi.isEmpty());
        const auto declaredFrames=int(std::ceil(tail*8000)),frames=declaredFrames-64,window=160;double finalPeak=0;bool finite=true;
        for(int first=0;first<frames;first+=64){const auto count=std::min(64,frames-first);audio.setSize(2,count,false,false,true);CHECK(midi.isEmpty());processor->processBlock(audio,midi);
            for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n){const auto value=audio.getSample(ch,n);finite&=std::isfinite(value);if(first+n>=frames-window)finalPeak=std::max(finalPeak,std::abs(double(value)));}}
        CHECK(finite);CHECK(finalPeak<=1e-9);CHECK(document(*processor)==*sound);
        std::cout<<"SATURATION_EMPTY_MIDI_TAIL quality="<<int(quality)<<" host_rate=8000 effective_rate="<<(quality==RenderQuality::higher?16000:8000)<<" excitation="<<excitation<<" declared_seconds="<<tail<<" declared_frames="<<declaredFrames<<" noteoff_frames=64 empty_frames="<<frames<<" final20ms_peak="<<finalPeak<<" threshold=1e-9\n";
    }
}
void outputWidgetsAndRecall()
{
    section("output_widgets_locks_macros_ordinary_edits_and_full_recall");
    auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture;CHECK(p.getParameters().size()==parameterCount);if(p.getParameters().size()!=parameterCount)return;
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());e->setVisible(true);dispatch();
    auto* gain=knob(*e,"output.gain_db");
    auto* guard=knob(*e,"output.protection");
    auto* macro1=knob(*e,"macro.1");auto* unit=programmer(*e);
    CHECK(gain&&guard&&macro1&&unit);if(!gain||!guard||!macro1||!unit)return;
    CHECK(std::abs(gain->getValue()-30)<1e-5&&guard->getValue()==1&&guard->valueLine().endsWith(" ON"));
    CHECK(guard->getTooltip().contains("-1 dBFS"));CHECK(p.control(756).getLabel()=="dB");
    // The assignment editor offers the output gain and not the guard, a toggle.
    CHECK(choose(*macro1,"Edit assignment"));
    auto& target=unit->assign.target;auto& depth=unit->assign.depth;
    auto item=[&](std::size_t index){for(int k=0;k<target.getNumItems();++k)if(target.getItemText(k)==parameterRegistry()[index].name)return target.getItemId(k);return 0;};
    CHECK(item(756)!=0&&item(757)==0);
    auto unboundFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& unbound = *unboundFixture;
    target.setSelectedId(item(756),juce::sendNotificationSync);depth.setValue(-25,juce::sendNotificationSync);
    CHECK(document(p).macros[0].targetKey==parameterRegistry()[756].stateKey&&document(p).macros[0].amount==-.25f);
    CHECK(p.undoSoundEdit());CHECK(p.undoSoundEdit());CHECK(document(p)==unbound);CHECK(!p.undoSoundEdit());
    unit->assign.close(false);
    auto initialFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& initial = *initialFixture;std::uint64_t revision,resetBefore,resetAfter;auto snapFixture = std::make_unique<SoundDocument>(); auto& snap = *snapFixture;
    CHECK(p.trySoundSnapshot(snap,revision,resetBefore));
    RecallListener listener;listener.action=[]{};p.control(756).addListener(&listener);
    grab(*gain);gain->setValue(12,juce::sendNotificationSync);gain->setValue(6,juce::sendNotificationSync);letGo(*gain);
    CHECK(listener.gestures==2);p.control(756).removeListener(&listener);
    CHECK(std::abs(parameterPhysicalValue(document(p).values,756)-6)<1e-5);
    CHECK(p.trySoundSnapshot(snap,revision,resetAfter));CHECK(resetBefore==resetAfter);
    CHECK(p.undoSoundEdit());CHECK(document(p)==initial);CHECK(!p.undoSoundEdit());CHECK(p.redoSoundEdit());
    CHECK(choose(*gain,"Lock"));gain->setValue(0,juce::sendNotificationSync);CHECK(std::abs(parameterPhysicalValue(document(p).values,756))<1e-5);
    guard->setValue(0,juce::sendNotificationSync);dispatch();CHECK(document(p).values[757]==0&&guard->valueLine().endsWith(" OFF"));
    auto recalledFixture = airack::testing::fixtureValue([&]{return initial;}); auto& recalled = *recalledFixture;recalled.values[756]=.5f;recalled.values[757]=0;
    grab(*gain);gain->setValue(24,juce::sendNotificationSync);CHECK(restore(p,state(recalled)));gain->setValue(36,juce::sendNotificationSync);letGo(*gain);dispatch();
    CHECK(document(p)==recalled);CHECK(std::abs(gain->getValue()-6)<1e-5&&guard->getValue()==0);CHECK(!p.undoSoundEdit());
    CHECK(p.trySoundSnapshot(snap,revision,resetAfter));CHECK(resetAfter>resetBefore);
    // Recall clears an overload envelope and makes the next note match a fresh instance exactly.
    auto hotFixture = airack::testing::fixtureValue([&]{return fullDocument();}); auto& hot = *hotFixture;physical(hot,"output.gain_db",36);physical(hot,"output.protection",1);
    CHECK(restore(p,state(hot)));p.prepareToPlay(48000,64);juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;
    for(int k=0;k<20;++k){midi.clear();if(k==0)for(int n=48;n<64;++n)midi.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)127),0);p.processBlock(a,midi);}
    CHECK(restore(p,state(initial)));auto freshFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& fresh = *freshFixture;fresh.prepareToPlay(48000,64);
    for(int k=0;k<10;++k){midi.clear();if(k==0)note(midi);p.processBlock(a,midi);fresh.processBlock(b,midi);for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));}
    p.releaseResources();fresh.releaseResources();
    for(const char* fixture:{"m2-analog-pluck.v2.soundstate","m2-compound-mono-modulated-tempo.v2.soundstate","m2-compound-mono-modulated-no-tempo.v2.soundstate"}){
        CHECK(p.loadSoundPreset(testWorkspace().getChildFile("tests/fixtures").getChildFile(fixture)));
        CHECK(std::abs(parameterPhysicalValue(document(p).values,756))<1e-5&&document(p).values[757]==0);
    }
}

}
// BEGIN HOST STATE LOAD ERROR CHECK
#ifndef NDEBUG
namespace airack {
struct InstrumentStructuralTestAccess {
    static std::uint64_t sourceWord(const InstrumentProcessor& p) {return p.structuralGeneration.load();}
    static bool maskedPublish(InstrumentProcessor& p,const ProjectDocument& d,const std::array<bool,parameterCount>& mask) {
        const std::lock_guard lock(p.restoreMutex);return p.publish(d,false,true,false,&mask);
    }
    static bool storedResult(const InstrumentProcessor& p,juce::MemoryBlock& bytes) {
        const std::lock_guard lock(p.restoreMutex);return p.aiPayloads->request&&p.aiPayloads->result&&companion::encodeResult(*p.aiPayloads->request,*p.aiPayloads->result,bytes);
    }
    static bool aiEdit(InstrumentProcessor& p,const ProjectDocument& before,const ProjectDocument& after) {
        const std::lock_guard lock(p.restoreMutex);return p.performAiEdit(before,after);
    }
    static bool prepareWithUnavailableSnapshot(InstrumentProcessor& p,double rate,int block) {
        const auto version=p.revision.load();if(version&1u)return false;
        // Deterministically hold the existing publication seqlock in its
        // writer state, so the real bounded reader must take its fallback.
        p.revision.store(version+1);AudioProjectState ignored;
        std::uint64_t serial=0,edit=0,reset=0,restart=0;
        const bool unavailable=!p.readAudioProject(ignored,serial,edit,reset,restart);
        p.prepareToPlay(rate,block);p.revision.store(version);return unavailable;
    }
    static void hook(void (*callback)(int)) {InstrumentProcessor::setStructuralTestHook(callback);}
    static EngineKind preparedEngine(const InstrumentProcessor& p) {return p.engine.patch().synth.layers[0].engine;}
    static std::array<std::uint64_t,5> authority(const InstrumentProcessor& p) {return {p.aiContextRevision,p.editRevision.load(),p.resetGeneration.load(),p.patternGeneration.load(),p.modeGeneration.load()};}
    static ProjectDocument recovery(const InstrumentProcessor& p) {return *p.recoveryDocument;}
    static std::optional<GroupIntent> savedGroup(const InstrumentProcessor& p) {ProjectDocument d;p.groupMetadata.load(d);return d.group;}
};
}
#endif
namespace
{
void stateLoadErrorCheck(const juce::String& prefix)
{
    section("host_state_rejection_persists_in_editor_without_changing_sound_or_history");
    InstrumentProcessor p, independent;
    auto initial=fullDocument();
    physical(initial,"synth.1.lfo.1.synced",1);
    auto valid=state(initial);
    p.setStateInformation(valid.getData(),static_cast<int>(valid.getSize()));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    editor->setVisible(true);
    auto warning=[&] {return dynamic_cast<juce::Label*>(findComponent(*editor,"state.load.error"));};
    // The warning is drawn on the SESSION display, which lights ERROR while the failure stands.
    auto display=[&] {return dynamic_cast<studio::Display*>(findComponent(*editor,"session.display"));};
    auto expectWarning=[&](bool failed) {
        CHECK(waitForUi("host load warning",[&] {
            auto* label=warning();auto* glass=display();
            return label && glass && (failed ? glass->session.errorLed&&glass->accessibleValue().contains(label->getText())&&label->getText().containsIgnoreCase("could not load")&&label->getText().containsIgnoreCase("sound preserved") : label->getText().isEmpty());
        },[&] {return warning()?warning()->getText():juce::String("missing load error label");}));
    };
    auto capture=[&](const juce::String& suffix) {
        if(prefix.isEmpty())return;
        auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true);
        auto out=juce::File(prefix+suffix+".png").createOutputStream();
        CHECK(out&&image.isValid());if(out)CHECK(juce::PNGImageFormat().writeImageToStream(image,*out));
    };
    auto corrupt=valid;static_cast<char*>(corrupt.getData())[50]^=1;
    auto unsupported=valid;
    static_cast<char*>(unsupported.getData())[4]=99;
    const auto digest=juce::SHA256(unsupported.getData(),unsupported.getSize()-32).getRawData();
    unsupported.copyFrom(digest.getData(),unsupported.getSize()-32,32);
    const std::array<std::pair<const void*,int>,6> rejected {{
        {corrupt.getData(),static_cast<int>(corrupt.getSize())},
        {unsupported.getData(),static_cast<int>(unsupported.getSize())},
        {nullptr,static_cast<int>(valid.getSize())},{valid.getData(),-1},
        {valid.getData(),static_cast<int>(valid.getSize())-1},{valid.getData(),0}}};
    for(const auto& [bytes,size]:rejected)
    {
        p.setStateInformation(valid.getData(),static_cast<int>(valid.getSize()));
        expectWarning(false);
        auto a=initial,b=initial;a.values[0]=.31f;b.values[0]=.62f;
        CHECK(edit(p,a));CHECK(edit(p,b));CHECK(p.undoSoundEdit());CHECK(document(p)==a);
        SoundDocument before;std::uint64_t revision=0,reset=0;
        CHECK(p.trySoundSnapshot(before,revision,reset));
        juce::MemoryBlock savedBefore;p.getStateInformation(savedBefore);
        p.setStateInformation(bytes,size);
        SoundDocument after;std::uint64_t now=0,nowReset=0;
        CHECK(p.trySoundSnapshot(after,now,nowReset));
        CHECK(after==before&&now==revision&&nowReset==reset);
        juce::MemoryBlock savedAfter;p.getStateInformation(savedAfter);CHECK(savedBefore==savedAfter);
        expectWarning(true);
        CHECK(p.redoSoundEdit());CHECK(document(p)==b);
        CHECK(p.undoSoundEdit());CHECK(document(p)==a);
        CHECK(p.undoSoundEdit());CHECK(document(p)==initial);
        CHECK(p.redoSoundEdit());CHECK(document(p)==a);
        expectWarning(true);
    }
    // Failure predating an editor and arriving on a host thread must survive opening/reopening.
    p.setStateInformation(valid.getData(),static_cast<int>(valid.getSize()));expectWarning(false);
    editor.reset();
    std::thread host([&]{p.setStateInformation(corrupt.getData(),static_cast<int>(corrupt.getSize()));});host.join();
    editor.reset(p.createEditor());editor->setVisible(true);expectWarning(true);
    editor.reset();editor.reset(p.createEditor());editor->setVisible(true);expectWarning(true);
    std::unique_ptr<juce::AudioProcessorEditor> other(independent.createEditor());
    auto* otherWarning=dynamic_cast<juce::Label*>(findComponent(*other,"state.load.error"));
    CHECK(otherWarning&&otherWarning->getText().isEmpty());
    // Ordinary operation feedback and unavailable tempo must not replace the persistent failure.
    auto* status=dynamic_cast<juce::Label*>(findComponent(*editor,"status"));
    auto* path=dynamic_cast<juce::TextEditor*>(findComponent(*editor,"preset.path"));
    auto* load=dynamic_cast<juce::Button*>(findComponent(*editor,"preset.load"));
    CHECK(status&&path&&load);
    if(path&&load){path->setText("not-an-absolute-preset");click(*load);}
    CHECK(status&&status->getText().containsIgnoreCase("failed"));expectWarning(true);
    auto* undo=dynamic_cast<juce::Button*>(findComponent(*editor,"undo"));CHECK(undo);
    auto ordinary=document(p);ordinary.values[0]=.37f;CHECK(edit(p,ordinary));
    if(undo)click(*undo);
    CHECK(status&&status->getText().containsIgnoreCase("complete"));expectWarning(true);
    p.prepareToPlay(48000,64);juce::AudioBuffer<float> block(2,64);juce::MidiBuffer midi;note(midi);p.processBlock(block,midi);
    auto* tempo=dynamic_cast<juce::Label*>(findComponent(*editor,"tempo.status"));
    CHECK(waitForUi("independent tempo message",[&]{return tempo&&tempo->getText().containsIgnoreCase("unavailable");},[&]{return tempo?tempo->getText():juce::String("missing tempo label");}));
    expectWarning(true);
    editor->setSize(1080,680);
    if(auto* glass=display()) {
        CHECK(editor->getLocalBounds().contains(editor->getLocalArea(glass,glass->getLocalBounds())));
        for(auto* label:{warning(),status,tempo})CHECK(label&&glass->accessibleValue().contains(label->getText()));
    } else CHECK(false);
    capture("-failed");
    // Valid modern and historical host recalls clear the error and retain full recall semantics.
    auto recalled=initial;recalled.values[0]=.73f;recalled.synthLocks[3]=true;
    const auto modern=state(recalled);
    std::vector<juce::MemoryBlock> accepted {modern};
    for(const auto* fixture:{"m1-analog-pluck.soundstate","m2-analog-pluck.v2.soundstate"}) {
        juce::MemoryBlock bytes;
        CHECK(testWorkspace().getChildFile("tests/fixtures").getChildFile(fixture).loadFileAsData(bytes));
        accepted.push_back(bytes);
    }
    for(const auto& bytes:accepted) {
        auto changed=document(p);changed.values[0]=.29f;CHECK(edit(p,changed));
        p.setStateInformation(nullptr,1);expectWarning(true);
        SoundDocument expected;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),expected));
        SoundDocument before;std::uint64_t revision=0,reset=0;
        CHECK(p.trySoundSnapshot(before,revision,reset));
        std::thread recall([&]{p.setStateInformation(bytes.getData(),static_cast<int>(bytes.getSize()));});recall.join();
        expectWarning(false);
        SoundDocument after;std::uint64_t now=0,nowReset=0;
        CHECK(p.trySoundSnapshot(after,now,nowReset));CHECK(after==expected&&now>revision&&nowReset>reset);
        CHECK(!p.undoSoundEdit());CHECK(!p.redoSoundEdit());
    }
    capture("-recovered");p.releaseResources();
}
}
// END HOST STATE LOAD ERROR CHECK
namespace {
RackDesign personalityDesign(NativeModule module) {
    RackDesign d;d.name="Original";d.role=module<NativeModule::drive?RackRole::synth:RackRole::fx;d.units={{0,module,{}}};d.panel.sections={"Tone"};
    if(d.role==RackRole::synth){d.units[0].algorithmRevision=modernSynthAlgorithmRevision;d.units[0].defaults={{0,"analog.filter.cutoff",.5f}};d.panel.controls={{"Tone",0,0,"analog.filter.cutoff",FocusWidget::rotary,FocusResponse::logarithmic,20,20000}};}
    if(module==NativeModule::drive)d.units[0].algorithmRevision=modernDriveAlgorithmRevision;return d;
}
SoundDocument personalitySnapshot(InstrumentProcessor& p,std::uint64_t& edit) {SoundDocument d;CHECK(p.trySoundSnapshot(d,edit));return d;}
void personalityPaths() {
    section("all_paths_reconcile");InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));std::uint64_t edit;auto d=personalitySnapshot(p,edit);
    RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=personalityDesign(NativeModule::analog);adopt.allocation={{0,0}};
    CHECK(p.applyRackOperation(adopt,edit)==RackCandidateStatus::ready);d=personalitySnapshot(p,edit);CHECK(d.personalities&&d.personalities->instances.size()==1);if(!d.personalities)return;
    const auto original=d;auto value=d;value.values[29]=.75f;CHECK(p.applySoundEdit(value,SoundEditOrigin::manual,edit));d=personalitySnapshot(p,edit);CHECK(*d.personalities==*original.personalities);
    CHECK(p.beginParameterGesture(29));CHECK(p.setParameterGestureValue(29,.6f));CHECK(p.endParameterGesture(29));d=personalitySnapshot(p,edit);CHECK(*d.personalities==*original.personalities);
    p.control(29).setValue(.8f);d=personalitySnapshot(p,edit);CHECK(*d.personalities==*original.personalities);
    section("host_engine_reconciliation");const auto captured=edit;p.control(13).setValue(1);p.control(13).setValue(0);d=personalitySnapshot(p,edit);CHECK(edit>captured);CHECK(!p.applySoundEdit(value,SoundEditOrigin::manual,captured));
    p.control(13).setValue(1);d=personalitySnapshot(p,edit);CHECK(d.personalities->instances[0].design.units[0].module==NativeModule::wavetable);CHECK(!rackControlActive(d,d.personalities->instances[0],0));CHECK(d.personalities->instances[0].design.panel.controls==original.personalities->instances[0].design.panel.controls);
    p.control(13).setValue(0);d=personalitySnapshot(p,edit);CHECK(rackControlActive(d,d.personalities->instances[0],0));
    struct Reenter final:juce::AudioProcessorParameter::Listener {
        InstrumentProcessor& p;bool fired=false;explicit Reenter(InstrumentProcessor& x):p(x){}
        void parameterValueChanged(int,float) override {} void parameterGestureChanged(int,bool begin) override {if(begin&&!fired){fired=true;p.control(13).setValue(1);p.control(0).setValue(.7f);}}
    } listener(p);
    p.control(29).addListener(&listener);CHECK(p.beginParameterGesture(29));CHECK(p.setParameterGestureValue(29,.4f));CHECK(p.endParameterGesture(29));p.control(29).removeListener(&listener);d=personalitySnapshot(p,edit);CHECK(listener.fired);CHECK(d.values[0]==.7f&&d.values[13]==1);CHECK(!rackControlActive(d,d.personalities->instances[0],0));
    section("dependent_fx_survives_synth_removal");RackOperation fx;fx.kind=RackOperationKind::place;fx.newInstance=juce::Uuid();fx.design=personalityDesign(NativeModule::drive);fx.destination={RackRole::fx,0,0};CHECK(p.applyRackOperation(fx,edit)==RackCandidateStatus::ready);d=personalitySnapshot(p,edit);CHECK(p.applyRackEdit({RackEditKind::removeSynth,0},edit)!=RackEditResult::applied);CHECK(personalitySnapshot(p,edit)==d);
    RackOperation move;move.kind=RackOperationKind::move;move.selected=fx.newInstance;move.destination={RackRole::fx,masterChain,0};RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=adopt.newInstance;
    const std::array<RackOperation,2> batch{move,remove};CHECK(p.applyRackOperations(batch,edit)==RackCandidateStatus::ready);auto after=personalitySnapshot(p,edit);CHECK(after.personalities->instances.size()==1&&after.personalities->instances[0].id==fx.newInstance);CHECK(p.undoSoundEdit());CHECK(personalitySnapshot(p,edit)==d);CHECK(p.redoSoundEdit());CHECK(personalitySnapshot(p,edit)==after);
    for(std::size_t i=0;i<parameterCount;++i){CHECK(p.control(i).paramID==parameterRegistry()[i].id);CHECK(p.control(i).getParameterIndex()==int(i));}
}
void personalityAudio() {
    section("fixed_audio_full_personality_capacity");static_assert(std::is_trivially_copyable_v<SoundAudioState>);static_assert(std::is_trivially_copyable_v<AudioProjectState>);
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));std::uint64_t edit;auto d=personalitySnapshot(p,edit);
    for(std::uint8_t s=1;s<4;++s){CHECK(p.applyRackEdit({RackEditKind::addSynth,s,masterChain,s},edit)==RackEditResult::applied);d=personalitySnapshot(p,edit);}
    for(std::uint8_t s=0;s<8;++s){CHECK(p.applyRackEdit({RackEditKind::addEffect,s,masterChain,s},edit)==RackEditResult::applied);d=personalitySnapshot(p,edit);}
    for(std::uint8_t s=0;s<12;++s){RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.design=personalityDesign(s<4?NativeModule::analog:NativeModule::drive);op.destination.role=s<4?RackRole::synth:RackRole::fx;op.allocation={{0,std::uint8_t(s<4?s:s-4)}};CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::ready);d=personalitySnapshot(p,edit);}
    CHECK(d.personalities&&d.personalities->instances.size()==12);p.prepareToPlay(48000,256);juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;note(midi);p.processBlock(audio,midi);midi.clear();
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;audioFrees=0;auto previous=_CrtSetAllocHook(allocationHook);
    for(int i=0;i<8;++i){tracking=true;p.control(13).setValue(float(i%2));tracking=false;d=personalitySnapshot(p,edit);auto next=d;next.values[0]=.51f+.01f*i;CHECK(p.applySoundEdit(next,SoundEditOrigin::manual,edit));CHECK(p.undoSoundEdit());CHECK(p.redoSoundEdit());tracking=true;p.processBlock(audio,midi);tracking=false;}
    _CrtSetAllocHook(previous);CHECK(allocations==0);CHECK(audioFrees==0);std::cout<<"Personality audio allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#endif
    d=personalitySnapshot(p,edit);CHECK(d.personalities->instances.size()==12);InstrumentProcessor reference{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(reference));std::uint64_t r;personalitySnapshot(reference,r);CHECK(reference.applySoundEdit(d,SoundEditOrigin::manual,r));
    p.prepareToPlay(48000,256);reference.prepareToPlay(48000,256);juce::AudioBuffer<float> expected(2,256);note(midi);p.processBlock(audio,midi);reference.processBlock(expected,midi);midi.clear();
    auto invalid=d;auto ownership=std::make_shared<RackPersonalityState>(*d.personalities);ownership->instances[1].bindings=ownership->instances[0].bindings;invalid.personalities=ownership;CHECK(!p.applySoundEdit(invalid,SoundEditOrigin::manual,edit));p.processBlock(audio,midi);reference.processBlock(expected,midi);
    bool same=true;for(int c=0;c<2;++c)for(int n=0;n<256;++n)same&=audio.getSample(c,n)==expected.getSample(c,n);CHECK(same);
}

#ifndef NDEBUG
std::unique_ptr<ai::GenerationResult> sourceAuthorityReady(const ai::GenerationRequest& request) {
    auto result=std::make_unique<ai::GenerationResult>();auto& r=*result;r.tag=request.tag;
    r.status=ai::GenerationStatus::ready;r.proposalStatus=ai::ProposalStatus::accepted;r.renderStatus=ai::RenderStatus::ready;
    r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;r.inferenceCalls=1;
    r.edits.parameters={{"global.master_level",.57f}};r.edits.name="Epoch Fixture";
    if(request.catalog){const auto& c=*request.catalog;r.catalog=ai::CatalogResultBinding{c.root,c.authority->snapshot};
        if(c.phase==ai::CatalogPhase::materialize){r.edits=c.baseIntent;r.inferenceCalls=c.inferenceCallsUsed;r.providerStatus=c.providerStatus;r.httpStatus=c.httpStatus;}}
    r.candidate.emplace();CHECK((request.catalog?ai::admitProposal(request.source,request.policy,r.edits,*request.catalog->authority,*r.candidate):ai::admitProposal(request.source,request.policy,r.edits,*r.candidate))==ai::ProposalStatus::accepted);
    // Synthetic melodic protocol fixture; this is not Drum preview/audio evidence.
    r.audio.bodyFrames=1;const auto frames=ai::previewTailFrames+1;r.audio.left.assign(frames,0);r.audio.right.assign(frames,0);r.audio.left[0]=r.audio.right[0]=.125f;
    r.measurements.peak=.125;r.measurements.rms=.125/std::sqrt(double(frames));r.measurements.onsetSeconds=0;r.measurements.activityEndSeconds=1/ai::previewSampleRate;
    juce::MemoryBlock wire;CHECK(companion::encodeResult(request,r,wire));return result;
}
std::unique_ptr<ai::GenerationResult> sourceAuthoritySelection(const ai::GenerationRequest& root) {
    auto result=std::make_unique<ai::GenerationResult>();const auto design=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;CHECK(encodeRackDesign(design,canonical,RackValidationMode::instanceRecall));
    result->tag=root.tag;result->status=ai::GenerationStatus::catalogSelectionRequired;result->proposalStatus=ai::ProposalStatus::accepted;result->providerStatus=rack::oauth::Status::connected;result->httpStatus=200;result->inferenceCalls=1;result->catalog=ai::CatalogResultBinding{root.catalog->root,root.catalog->authority->snapshot};result->edits.name="Epoch Fixture";result->edits.catalogSelections={{7,design.id,design.revision,juce::SHA256(canonical).toHexString(),{RackRole::synth,masterChain,1}}};
    juce::MemoryBlock wire;CHECK(companion::encodeResult(root,*result,wire));return result;
}
#endif
void sourceSelectorAuthority() {
#ifndef NDEBUG
    section("eight_source_selectors_and_masked_commit_authority");
    auto owner=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*owner;CHECK(blankInstance(p));
    std::array<std::size_t,8> selectors{13,17,21,25,drumModeParameterStart,drumModeParameterStart+1,drumModeParameterStart+2,drumModeParameterStart+3};
    for(std::size_t bit=0;bit<8;++bit){const auto i=selectors[bit],before=InstrumentStructuralTestAccess::sourceWord(p);const auto value=p.control(i).getValue();
        p.control(i).setValue(value);CHECK(InstrumentStructuralTestAccess::sourceWord(p)==before);
        p.control(i).setValue(value==0?.999999f:.000001f);const auto away=InstrumentStructuralTestAccess::sourceWord(p);CHECK((away&255)==((before&255)^(std::uint64_t(1)<<bit)));CHECK((away&~std::uint64_t(255))==(before&~std::uint64_t(255))+256);
        p.control(i).setValue(value);const auto back=InstrumentStructuralTestAccess::sourceWord(p);CHECK((back&255)==(before&255)&&back==before+512);
        auto sound=std::make_unique<SoundDocument>();std::uint64_t edit;CHECK(p.trySoundSnapshot(*sound,edit));CHECK(sound->values[i]==value);
        p.control(i).setValue(1);CHECK(p.control(i).getValue()==1);CHECK(p.trySoundSnapshot(*sound,edit));
        const auto bank=std::uint8_t(bit<4?bit:bit-4);CHECK(bankModule(*sound,RackRole::synth,bank)==(bit<4?NativeModule::wavetable:NativeModule::drum));p.control(i).setValue(0);CHECK(p.trySoundSnapshot(*sound,edit));
    }
    const auto before=InstrumentStructuralTestAccess::sourceWord(p);p.control(39).setValue(.73f);CHECK(InstrumentStructuralTestAccess::sourceWord(p)==before);
    auto d=std::make_unique<ProjectDocument>();std::uint64_t edit;CHECK(p.tryProjectSnapshot(*d,edit));
    for(const int which:{0,1,2}){CHECK(p.tryProjectSnapshot(*d,edit));const auto old=InstrumentStructuralTestAccess::sourceWord(p);const auto expected=which==0?old^16:which==1?old^1:old;
        for(const auto i:selectors)d->sound.values[i]=1-p.control(i).getValue();d->sound.values[0]=.61f;
        std::array<bool,parameterCount> mask{};mask[which==0?drumModeParameterStart:which==1?13:0]=true;
        CHECK(InstrumentStructuralTestAccess::maskedPublish(p,*d,mask));const auto word=InstrumentStructuralTestAccess::sourceWord(p);CHECK((word&255)==(expected&255));CHECK((word&~std::uint64_t(255))==(old&~std::uint64_t(255))+256);
        for(std::size_t bit=0;bit<8;++bit)CHECK(p.control(selectors[bit]).getValue()==float((expected>>bit)&1));
    }
#endif
}
void sourceSelectorCallbacks() {
#ifndef NDEBUG
    section("selector_away_back_inside_begin_and_publish_cas");
    const AiRequestOptions options{"Authority fixture","synthetic-selected_model:1.0",{ai::RequestScope::sound,true,true}};
    for(const auto selector:{std::size_t(13),drumModeParameterStart}){
        auto owner=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*owner;CHECK(blankInstance(p));auto captured=std::make_unique<SoundDocument>();std::uint64_t edit,reset;CHECK(p.trySoundSnapshot(*captured,edit,reset));
        const auto pending=airack::testing::beginAiRequest(p,options);CHECK(pending);const auto word=InstrumentStructuralTestAccess::sourceWord(p);const auto target=p.control(39).getValue(),master=p.control(0).getValue();
        struct Listener final:juce::AudioProcessorParameter::Listener{InstrumentProcessor& p;std::size_t selector;int begins=0,ends=0;Listener(InstrumentProcessor& p,std::size_t s):p(p),selector(s){}void parameterValueChanged(int,float)override{}void parameterGestureChanged(int,bool begin)override{if(begin){++begins;p.control(selector).setValue(1);p.control(selector).setValue(0);}else ++ends;}} listener(p,selector);
        p.control(39).addListener(&listener);const std::array<ParameterGestureValue,1> values{{{39,.26f}}};
        const bool began=p.beginParameterGesture(39,values,*captured,edit,reset);if(began){CHECK(!p.setParameterGestureValues(39,values,*captured,edit,reset));CHECK(p.endParameterGesture(39));}
        p.control(39).removeListener(&listener);CHECK(listener.begins==1&&listener.ends==1);CHECK(p.control(39).getValue()==target&&p.control(0).getValue()==master);
        CHECK((InstrumentStructuralTestAccess::sourceWord(p)&255)==(word&255)&&InstrumentStructuralTestAccess::sourceWord(p)!=word);CHECK(p.aiProposalPhase()==AiProposalPhase::stale);
        for(const int point:{3,2}){CHECK(p.trySoundSnapshot(*captured,edit));const auto recovery=airack::testing::fixtureValue([&]{return InstrumentStructuralTestAccess::recovery(p);});const auto authority=InstrumentStructuralTestAccess::authority(p);auto next=airack::testing::fixtureValue([&]{return *captured;});next->values[39]=.31f;
            static thread_local InstrumentProcessor* hooked;static thread_local std::size_t selected;static thread_local int wanted;hooked=&p;selected=selector;wanted=point;
            InstrumentStructuralTestAccess::hook(+[](int at){if(at==wanted){hooked->control(selected).setValue(1);hooked->control(selected).setValue(0);hooked->control(0).setValue(.83f);}});
            CHECK(p.applySoundEdit(*next,SoundEditOrigin::manual,edit)==(point==2));InstrumentStructuralTestAccess::hook(nullptr);
            if(point==3){CHECK(InstrumentStructuralTestAccess::recovery(p)==*recovery);CHECK(InstrumentStructuralTestAccess::authority(p)==authority);}
            CHECK(p.control(0).getValue()==.83f&&p.control(selector).getValue()==0);CHECK(p.trySoundSnapshot(*captured,edit));CHECK(captured->values[0]==.83f);CHECK(!p.redoSoundEdit());
        }
    }
#endif
}
void sourceAiCaptureAuthority() {
#ifndef NDEBUG
    section("actual_pending_refinement_and_followup_source_authority");
    const AiRequestOptions options{"Authority fixture","synthetic-selected_model:1.0",{ai::RequestScope::sound,true,true}};
    for(const auto selector:{std::size_t(13),drumModeParameterStart})for(const int phase:{0,1,2}){
        auto owner=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*owner;CHECK(blankInstance(p));
        GroupIntent group;group.group.bytes[0]=1;group.member.bytes[0]=2;CHECK(setGroupText("Epoch",group.name));CHECK(p.setGroupIntent(group));
        auto root=InstrumentAiTestAccess::beginHistorical25(p,options);CHECK(root);if(!root)continue;
        if(phase==1){auto ready=sourceAuthorityReady(*root);CHECK(p.receiveAiResult(std::move(*ready)));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed);auto child=p.beginAiRefinement("Refine authority fixture",options.model);CHECK(child);if(!child)continue;CHECK(p.aiWorkflowStatus().refining);}
        if(phase==2){auto selected=sourceAuthoritySelection(*root);CHECK(p.receiveAiResult(std::move(*selected)));CHECK(p.aiCurrentRequest()&&p.aiCurrentRequest()->catalog&&p.aiCurrentRequest()->catalog->phase==ai::CatalogPhase::materialize);}
        if(!p.aiCurrentRequest())continue;auto request=airack::testing::fixtureValue([&]{return *p.aiCurrentRequest();});auto ready=sourceAuthorityReady(*request);
        p.control(selector).setValue(1);p.control(selector).setValue(0);CHECK(p.aiProposalPhase()==AiProposalPhase::stale);CHECK(!p.receiveAiResult(std::move(*ready)));CHECK(!p.aiProposal()&&!p.applyAiProposal()&&!p.startAiPreview());
    }

    for(const bool selection:{false,true}){
        auto owner=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*owner;CHECK(blankInstance(p));auto root=InstrumentAiTestAccess::beginHistorical25(p,options);CHECK(root);if(root){auto ready=selection?sourceAuthoritySelection(*root):sourceAuthorityReady(*root);juce::MemoryBlock before;CHECK(companion::encodeRequest(*p.aiCurrentRequest(),before));
            static thread_local InstrumentProcessor* hooked;hooked=&p;InstrumentStructuralTestAccess::hook(+[](int at){if(at==4){hooked->control(13).setValue(1);hooked->control(13).setValue(0);}});
            CHECK(!p.receiveAiResult(std::move(*ready)));InstrumentStructuralTestAccess::hook(nullptr);juce::MemoryBlock after;CHECK(companion::encodeRequest(*p.aiCurrentRequest(),after)&&after==before);CHECK(p.aiProposalPhase()==AiProposalPhase::stale&&!p.aiProposal());}
    }
    for(const bool refinement:{false,true}){
        auto owner=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*owner;CHECK(blankInstance(p));auto pending=InstrumentAiTestAccess::beginHistorical25(p,options);CHECK(pending);if(!pending)continue;
        if(refinement){auto ready=sourceAuthorityReady(*pending);CHECK(p.receiveAiResult(std::move(*ready)));}
        juce::MemoryBlock requestBefore,resultBefore;CHECK(companion::encodeRequest(*p.aiCurrentRequest(),requestBefore));if(refinement)CHECK(InstrumentStructuralTestAccess::storedResult(p,resultBefore));
        static thread_local InstrumentProcessor* hooked;hooked=&p;InstrumentStructuralTestAccess::hook(+[](int at){if(at==4){hooked->control(drumModeParameterStart).setValue(1);hooked->control(drumModeParameterStart).setValue(0);}});
        CHECK(!(refinement?p.beginAiRefinement("Late child",options.model):InstrumentAiTestAccess::beginHistorical25(p,options)));InstrumentStructuralTestAccess::hook(nullptr);
        juce::MemoryBlock requestAfter,resultAfter;CHECK(p.aiCurrentRequest()&&companion::encodeRequest(*p.aiCurrentRequest(),requestAfter)&&requestAfter==requestBefore);if(refinement)CHECK(InstrumentStructuralTestAccess::storedResult(p,resultAfter)&&resultAfter==resultBefore);
        CHECK(p.aiProposalPhase()==AiProposalPhase::stale&&!p.aiWorkflowStatus().refining);auto sound=std::make_unique<SoundDocument>();std::uint64_t edit;CHECK(p.trySoundSnapshot(*sound,edit));const auto next=InstrumentAiTestAccess::beginHistorical25(p,options);CHECK(next&&next->tag.requestId==pending->tag.requestId+1);
    }
#endif
}
void sourceWholeBankTransactions() {
#ifndef NDEBUG
    section("complete_294_field_source_undo_duplicate_reuse_and_restore");
    auto owner=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*owner;CHECK(blankInstance(p));
    auto before=std::make_unique<ProjectDocument>(),after=std::make_unique<ProjectDocument>(),observed=std::make_unique<ProjectDocument>();std::uint64_t edit;CHECK(p.tryProjectSnapshot(*before,edit));CHECK(before->sound.layout.synthUse[0]==SlotUse::present);
    RackDesign drum;drum.name="Drum";drum.panel.sections={"Drum"};drum.units={{0,NativeModule::drum,{},1}};RackOperation place;place.design=drum;place.newInstance=juce::Uuid();place.allowSlotReuse=true;place.allocation={{0,0}};CHECK(p.applyRackOperation(place,edit)==RackCandidateStatus::ready);CHECK(p.tryProjectSnapshot(*before,edit));

    CHECK(bareBankName(before->sound,{RackRole::synth,0})=="SYNTH 1 DRUM");
    // Current ordinary Drum capture is public and typed; synthetic melodic result fixtures below remain historical.
    for(const auto scope:{ai::RequestScope::sound,ai::RequestScope::pattern,ai::RequestScope::both}){
        const AiRequestOptions request{"Owned Drum capture","synthetic-selected_model:1.0",{scope,true,true}};
        const auto captured=std::make_unique<std::optional<ai::GenerationRequest>>(airack::testing::beginAiRequest(p,request));CHECK(captured->has_value());
        auto local=InstrumentAiTestAccess::currentLocal(p);CHECK(local&&p.aiProposalPhase()==AiProposalPhase::pending);
        if(local){juce::MemoryBlock wire;CHECK(local->request.source==*before&&local->request.policy.scope==scope);CHECK(companion::encodeLocalRequest(*local,wire));
            auto decoded=std::make_unique<ai::LocalGenerationRequest>();CHECK(companion::decodeLocalRequest({static_cast<const std::byte*>(wire.getData()),wire.getSize()},*decoded));CHECK(decoded->local==local->local);CHECK(!companion::encodeRequest(local->request,wire));}
        p.discardAiProposal();p.pollAi();CHECK(!p.aiCurrentRequest());CHECK(p.tryProjectSnapshot(*observed,edit)&&*observed==*before);
    }
    auto varied=airack::testing::fixtureValue([&]{return before->sound;});for(std::size_t f=0;f<9;++f)varied->values[wavetableWarmthParameterStart+f]=f==6?1.f/3:f==0||f==2?2.f/6:.4f;varied->values[drumParameterStart]=.23f;varied->values[drumModeParameterStart]=1;varied->values[13]=1;CHECK(p.applySoundEdit(*varied,SoundEditOrigin::manual,edit));CHECK(p.tryProjectSnapshot(*before,edit));
    RackOperation copy;copy.kind=RackOperationKind::duplicate;copy.selected=place.newInstance;copy.newInstance=juce::Uuid();copy.allocation={{0,3}};copy.destination={RackRole::synth,masterChain,1};CHECK(p.applyRackOperation(copy,edit)==RackCandidateStatus::ready);CHECK(p.tryProjectSnapshot(*after,edit));
    std::size_t fields=0;for(const auto& d:parameterRegistry())if(d.id.startsWith("synth.1.")){const auto key=d.id.substring(8);CHECK(after->sound.values[*rackParameterIndex(NativeModule::drum,3,key)]==before->sound.values[*rackParameterIndex(NativeModule::drum,0,key)]);++fields;}CHECK(fields==294);CHECK(after->sound.compatibility.synth[3]==before->sound.compatibility.synth[0]);
    juce::MemoryBlock bytes;CHECK(encodeProjectState(*after,bytes));CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),*observed)&&*observed==*after);
    auto word=InstrumentStructuralTestAccess::sourceWord(p);CHECK(p.undoSoundEdit());CHECK(p.tryProjectSnapshot(*observed,edit)&&*observed==*before);CHECK(InstrumentStructuralTestAccess::sourceWord(p)!=word);CHECK(p.redoSoundEdit());CHECK(p.tryProjectSnapshot(*observed,edit)&&*observed==*after);
    word=InstrumentStructuralTestAccess::sourceWord(p);CHECK(p.restoreProjectState(bytes.getData(),int(bytes.getSize())));CHECK(InstrumentStructuralTestAccess::sourceWord(p)==word+256);CHECK(p.tryProjectSnapshot(*observed,edit)&&*observed==*after);
    // A real melodic request is invalidated even when a complete remove/reuse ends on the same selectors.
    CHECK(blankInstance(p));CHECK(p.tryProjectSnapshot(*before,edit));RackOperation melodic;melodic.design=personalityDesign(NativeModule::analog);melodic.newInstance=juce::Uuid();melodic.allowSlotReuse=true;melodic.allocation={{0,0}};CHECK(p.applyRackOperation(melodic,edit)==RackCandidateStatus::ready);CHECK(p.tryProjectSnapshot(*before,edit));
    const AiRequestOptions options{"Reuse fixture","synthetic-selected_model:1.0",{ai::RequestScope::sound,true,true}};
    auto request=InstrumentAiTestAccess::beginHistorical25(p,options);CHECK(request);auto ready=sourceAuthorityReady(*request);RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=melodic.newInstance;auto replacement=melodic;replacement.newInstance=juce::Uuid();const std::array<RackOperation,2> compound{remove,replacement};word=InstrumentStructuralTestAccess::sourceWord(p);CHECK(p.applyRackOperations(compound,edit)==RackCandidateStatus::ready);CHECK(p.tryProjectSnapshot(*after,edit));CHECK((InstrumentStructuralTestAccess::sourceWord(p)&255)==(word&255)&&InstrumentStructuralTestAccess::sourceWord(p)==word+256);CHECK(edit>request->tag.editRevision&&p.aiProposalPhase()==AiProposalPhase::stale);CHECK(!p.receiveAiResult(std::move(*ready))&&!p.applyAiProposal());
    CHECK(p.undoSoundEdit());CHECK(p.tryProjectSnapshot(*observed,edit)&&*observed==*before);CHECK(p.redoSoundEdit());CHECK(p.tryProjectSnapshot(*observed,edit)&&*observed==*after);
    request=InstrumentAiTestAccess::beginHistorical25(p,options);CHECK(request);ready=sourceAuthorityReady(*request);CHECK(encodeProjectState(*after,bytes));word=InstrumentStructuralTestAccess::sourceWord(p);CHECK(p.restoreProjectState(bytes.getData(),int(bytes.getSize())));CHECK(InstrumentStructuralTestAccess::sourceWord(p)==word+256);CHECK(p.aiProposalPhase()!=AiProposalPhase::proposed&&p.aiProposalPhase()!=AiProposalPhase::pending);CHECK(!p.receiveAiResult(std::move(*ready))&&!p.applyAiProposal());
#endif
}
void personalityPublicationWindows() {
#ifndef NDEBUG
    section("structural_publication_window_and_interrupted_writer");
    static thread_local InstrumentProcessor* hooked=nullptr;
    static std::atomic<bool> entered=false,released=false;
    for(const int point:{2,3}) {
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));std::uint64_t edit;personalitySnapshot(p,edit);
        RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.design=personalityDesign(NativeModule::analog);op.allocation={{0,0}};
        CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::ready);CHECK(p.beginParameterGesture(29));
        hooked=&p;
        static thread_local int wanted;wanted=point;
        InstrumentStructuralTestAccess::hook(+[](int at){if(at==wanted){hooked->control(13).setValue(1);hooked->control(0).setValue(.73f);}});
        const bool applied=p.setParameterGestureValue(29,.8f);
        InstrumentStructuralTestAccess::hook(nullptr);hooked=nullptr;
        CHECK(applied==(point==2));
        const auto observed=personalitySnapshot(p,edit);
        CHECK(observed.values[13]==1&&observed.values[0]==.73f);
        CHECK(observed.personalities->instances[0].design.units[0].module==NativeModule::wavetable);
        CHECK(!rackControlActive(observed,observed.personalities->instances[0],0));
        CHECK(p.endParameterGesture(29));
    }
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));std::uint64_t edit;personalitySnapshot(p,edit);
    RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.design=personalityDesign(NativeModule::analog);op.allocation={{0,0}};
    CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::ready);const auto before=personalitySnapshot(p,edit);const auto captured=edit;
    entered=false;released=false;
    std::thread writer([&]{InstrumentStructuralTestAccess::hook(+[](int at){if(at==1){entered=true;while(!released.load())std::this_thread::yield();}});p.control(13).setValue(1);InstrumentStructuralTestAccess::hook(nullptr);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!entered.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    CHECK(entered.load());SoundDocument observed;const bool readable=p.trySoundSnapshot(observed,edit);CHECK(readable);
    if(readable&&observed.values[13]==1){CHECK(observed.personalities->instances[0].design.units[0].module==NativeModule::wavetable);CHECK(edit>captured);CHECK(!p.applySoundEdit(before,SoundEditOrigin::manual,captured));}
    released=true;writer.join();observed=personalitySnapshot(p,edit);CHECK(observed.values[13]==1);CHECK(edit>captured);CHECK(observed.personalities->instances[0].design.units[0].module==NativeModule::wavetable);
#endif
}
void personalityDiscreteAndGroupTransactions() {
#ifndef NDEBUG
    section("canonical_engine_roundoff_and_transactional_group_rejection");
    InstrumentProcessor near{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(near));std::uint64_t edit;personalitySnapshot(near,edit);
    RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.design=personalityDesign(NativeModule::analog);op.allocation={{0,0}};
    CHECK(near.applyRackOperation(op,edit)==RackCandidateStatus::ready);auto candidate=personalitySnapshot(near,edit);candidate.values[13]=.5e-6f;
    CHECK(validateSoundDocument(candidate));RoutedPatch expected;CHECK(makeRoutedPatch(candidate,expected));CHECK(expected.synth.layers[0].engine==EngineKind::analog);
    CHECK(near.applySoundEdit(candidate,SoundEditOrigin::manual,edit));CHECK(near.control(13).getValue()==0);
    auto observed=personalitySnapshot(near,edit);CHECK(observed.personalities->instances[0].design.units[0].module==NativeModule::analog);
    near.prepareToPlay(48000,256);CHECK(InstrumentStructuralTestAccess::preparedEngine(near)==EngineKind::analog);
    ProjectDocument roundoff;roundoff.sound.values[13]=.5e-6f;juce::MemoryBlock wire;CHECK(encodeProjectState(roundoff,wire));
    CHECK(near.restoreProjectState(wire.getData(),int(wire.getSize())));CHECK(near.control(13).getValue()==0);near.prepareToPlay(48000,256);CHECK(InstrumentStructuralTestAccess::preparedEngine(near)==EngineKind::analog);

    const auto intent=[](std::uint8_t id){GroupIntent g;g.group.bytes[0]=id;g.member.bytes[0]=id+10;CHECK(setGroupText("Session",g.name));CHECK(setGroupText(id==1?"bass":"pad",g.role));CHECK(setGroupText("A",g.key));CHECK(setGroupText("minor",g.scale));g.bpm=120;CHECK(validateGroupIntent(g));return g;};
    for(const bool recall:{true,false}) {
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};const auto original=intent(1),replacement=intent(2);CHECK(p.setGroupIntent(original));
        const AiRequestOptions options{"A warm editable proposal","synthetic-selected_model:1.0",{ai::RequestScope::both,true,true}};
        CHECK(airack::testing::beginAiRequest(p,options).has_value());
        const auto link=p.groupLinkView();const auto context=p.aiMusicalContext();const auto authority=InstrumentStructuralTestAccess::authority(p);const auto recovery=InstrumentStructuralTestAccess::recovery(p);
        ProjectDocument loaded;loaded.group=replacement;loaded.sound.values[0]=.2f;juce::MemoryBlock raw;CHECK(encodeProjectState(loaded,raw));
        static thread_local InstrumentProcessor* hooked;hooked=&p;
        InstrumentStructuralTestAccess::hook(+[](int at){if(at==3){hooked->control(13).setValue(1);hooked->control(0).setValue(.73f);}});
        const bool accepted=recall?p.restoreProjectState(raw.getData(),int(raw.getSize())):p.setGroupIntent(replacement);
        InstrumentStructuralTestAccess::hook(nullptr);hooked=nullptr;
        CHECK(!accepted);CHECK(p.groupLinkView()==link);CHECK(p.aiMusicalContext()==context);
        CHECK(InstrumentStructuralTestAccess::authority(p)==authority);CHECK(InstrumentStructuralTestAccess::recovery(p)==recovery);CHECK(InstrumentStructuralTestAccess::savedGroup(p)==original);
        CHECK(p.aiProposalPhase()==AiProposalPhase::stale);CHECK(p.control(13).getValue()==1&&p.control(0).getValue()==.73f);
        CHECK(recall?p.restoreProjectState(raw.getData(),int(raw.getSize())):p.setGroupIntent(replacement));
        ProjectDocument committed;CHECK(p.tryProjectSnapshot(committed,edit));CHECK(committed.group==replacement);CHECK(p.groupLinkView().intent==replacement);CHECK(p.aiMusicalContext().role=="pad");
        if(!recall){CHECK(p.setGroupIntent(replacement));CHECK(p.tryProjectSnapshot(committed,edit));CHECK(committed.group==replacement);}
    }
#endif
}
void personalityPolling() {
    section("polling_observes_host_engine_authority_before_snapshot");
    for(const bool returnToStart:{false,true}) {
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));std::uint64_t edit;personalitySnapshot(p,edit);
        RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=personalityDesign(NativeModule::analog);adopt.allocation={{0,0}};
        CHECK(p.applyRackOperation(adopt,edit)==RackCandidateStatus::ready);
        const AiRequestOptions options{"A warm editable proposal","synthetic-selected_model:1.0",{ai::RequestScope::both,true,true}};
        const auto pending=airack::testing::beginAiRequest(p,options);CHECK(pending.has_value());if(!pending)continue;
        p.control(13).setValue(1);if(returnToStart)p.control(13).setValue(0);
        CHECK(p.aiProposalPhase()==AiProposalPhase::stale);
        p.pollAi();
        CHECK(p.aiProposalPhase()==AiProposalPhase::stale); // No intervening control snapshot may reconcile for poll.
        const auto observed=personalitySnapshot(p,edit);CHECK(edit>pending->tag.editRevision);
        CHECK(observed.values[13]==(returnToStart?0.f:1.f));
        CHECK(observed.personalities->instances[0].design.units[0].module==(returnToStart?NativeModule::analog:NativeModule::wavetable));
    }
    InstrumentProcessor valueOnly{airack::testing::isolatedRackRuntime()};
    const AiRequestOptions options{"A warm editable proposal","synthetic-selected_model:1.0",{ai::RequestScope::both,true,true}};
    CHECK(airack::testing::beginAiRequest(valueOnly,options).has_value());valueOnly.control(29).setValue(.8f);valueOnly.pollAi();
    CHECK(valueOnly.aiProposalPhase()==AiProposalPhase::pending);
}
void personalityInvalidEngineRepair() {
    section("host_engine_reconciliation_preserves_invalid_control_editability");InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));std::uint64_t edit;personalitySnapshot(p,edit);
    RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=personalityDesign(NativeModule::analog);adopt.allocation={{0,0}};CHECK(p.applyRackOperation(adopt,edit)==RackCandidateStatus::ready);
    const auto target=idx("synth.1.modulation.1.target");p.control(target).setValue(1);p.control(13).setValue(1);p.control(drumModeParameterStart).setValue(1);
    SoundDocument raw;const bool readable=p.trySoundSnapshot(raw,edit);CHECK(readable);if(!readable)return;
    CHECK(raw.values[target]==1&&raw.values[drumModeParameterStart]==1&&raw.values[13]==1);CHECK(!validateSoundDocument(raw));CHECK(raw.personalities->instances[0].design.units[0].module==NativeModule::drum);
    CHECK(p.beginParameterGesture(0));CHECK(p.setParameterGestureValue(0,.8f));CHECK(p.endParameterGesture(0));auto repaired=personalitySnapshot(p,edit);CHECK(validateSoundDocument(repaired));CHECK(repaired.values[0]==.8f&&repaired.values[13]==1&&repaired.values[drumModeParameterStart]==1);CHECK(!rackControlActive(repaired,repaired.personalities->instances[0],0));
}
}
void rackPersistenceAndCapture()
{
    section("save_requires_active_mappings");companion::ClientOptions options;options.allowLaunch=false;
    options.launch.executable=juce::File::getCurrentWorkingDirectory().getChildFile("task3-no-companion.exe");
    const auto dir=juce::File::getCurrentWorkingDirectory().getChildFile("task3-persistence-"+juce::Uuid().toString());auto pOwner=std::make_unique<InstrumentProcessor>(options, airack::testing::isolatedRackRuntime());auto& p=*pOwner;CHECK(blankInstance(p));std::uint64_t edit;auto dOwned=airack::testing::fixtureValue([&]{return personalitySnapshot(p,edit);});auto& d=*dOwned;CHECK(dir.createDirectory());
    // Keep retained documents and comparison temporaries out of this long Debug frame.
    const auto matches=[&](InstrumentProcessor& processor,const SoundDocument& expected){
        auto actual=airack::testing::fixtureValue([&]{return personalitySnapshot(processor,edit);});return *actual==expected;
    };
    const auto preset=dir.getChildFile("sound.arsi");
    RackOperation adopt;adopt.kind=RackOperationKind::adopt;adopt.newInstance=juce::Uuid();adopt.design=personalityDesign(NativeModule::analog);adopt.allocation={{0,0}};
    CHECK(p.applyRackOperation(adopt,edit)==RackCandidateStatus::ready);CHECK(p.trySoundSnapshot(d,edit));auto beforeFixture = airack::testing::fixtureValue([&]{return d;}); const auto& before = *beforeFixture;
    const auto cutoff=*rackParameterIndex(NativeModule::analog,0,"analog.filter.cutoff");p.control(cutoff).setValue(.73f);p.control(0).setValue(.9f);p.control(1).setValue(.8f);
    auto liveOwned=airack::testing::fixtureValue([&]{return personalitySnapshot(p,edit);});const auto& live=*liveOwned;RackDesign captured;CHECK(p.captureRackDesign(adopt.newInstance,captured));CHECK(captured.id==adopt.design->id&&captured.revision==2);
    CHECK(captured.panel==live.personalities->instances[0].design.panel);CHECK(matches(p,live));CHECK(live.personalities->instances[0].design==before.personalities->instances[0].design);
    CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](auto& x){return x.key=="analog.filter.cutoff"&&x.normalized==.73f;}));
    CHECK(matches(p,live));
    auto sentinel=captured;CHECK(!p.captureRackDesign(juce::Uuid(),sentinel));CHECK(sentinel==captured);
    p.control(13).setValue(1);auto switchedOwned=airack::testing::fixtureValue([&]{return personalitySnapshot(p,edit);});const auto& switched=*switchedOwned;CHECK(!p.captureRackDesign(adopt.newInstance,sentinel));CHECK(sentinel==captured);
    // An explicit mapping removal permits capture; old inactive defaults
    // are excluded rather than silently activating the retained Analog bank.
    auto replacement=switched.personalities->instances[0].design;replacement.panel.controls.clear();
    std::erase_if(replacement.units[0].defaults,[](auto& x){return x.key.startsWith("analog.");});
    for(auto& x:replacement.units[0].defaults)if(x.key=="wavetable.osc.1.position")x.normalized=.44f;
    RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=adopt.newInstance;swap.design=replacement;swap.allowSlotReuse=true;
    CHECK(p.applyRackOperation(swap,edit)==RackCandidateStatus::ready);CHECK(p.trySoundSnapshot(d,edit));CHECK(p.captureRackDesign(adopt.newInstance,sentinel));CHECK(sentinel.units[0].module==NativeModule::wavetable);
    CHECK(d.values[cutoff]==.73f);CHECK(d.values[*rackParameterIndex(NativeModule::wavetable,0,"wavetable.osc.1.position")]==.44f);
    CHECK(p.undoSoundEdit());CHECK(matches(p,switched));CHECK(p.redoSoundEdit());CHECK(matches(p,d));
    for(const auto& x:sentinel.units[0].defaults)CHECK(!x.key.startsWith("analog."));
    section("library_transactions_native_recall_and_undo");
    CHECK(p.saveSoundPreset(preset));d.name="sound";juce::MemoryBlock sound;CHECK(preset.loadFileAsData(sound));CHECK(sound.getSize()>7024);
    auto qOwner=std::make_unique<InstrumentProcessor>(options, airack::testing::isolatedRackRuntime());auto& q=*qOwner;GroupIntent group;group.group=juce::Uuid();group.member=juce::Uuid();CHECK(setGroupText("Destination",group.name));CHECK(q.setGroupIntent(group));
    CHECK(q.loadSoundPreset(preset));auto recalledOwned=airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);});auto& recalled=*recalledOwned;CHECK(recalled==d);CHECK(q.groupLinkView().intent==std::optional<GroupIntent>(group));
    juce::MemoryBlock project;q.getStateInformation(project);auto decodedOwned=std::make_unique<ProjectDocument>();auto& decoded=*decodedOwned;CHECK(decodeProjectState(project.getData(),project.getSize(),decoded));CHECK(decoded.sound==d);CHECK(decoded.group==std::optional<GroupIntent>(group));
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=adopt.newInstance;duplicate.newInstance=juce::Uuid();duplicate.destination={RackRole::synth,masterChain,1};auto previewOwner=std::make_unique<SoundDocument>();auto& preview=*previewOwner;RackDifference diff;
    q.control(*rackParameterIndex(NativeModule::wavetable,0,"wavetable.osc.1.position")).setValue(.64f);
    auto cancelBeforeOwner=airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);});const auto& cancelBefore=*cancelBeforeOwner;CHECK(q.previewRackOperation(duplicate,edit,preview,diff)==RackCandidateStatus::ready);CHECK(matches(q,cancelBefore));
    CHECK(q.applyRackOperation(duplicate,edit)==RackCandidateStatus::ready);auto duplicatedOwner=airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);});auto& duplicated=*duplicatedOwner;CHECK(duplicated.personalities->instances.size()==2);
    CHECK(duplicated.personalities->instances[0].design==duplicated.personalities->instances[1].design);CHECK(duplicated.personalities->instances[0].id!=duplicated.personalities->instances[1].id);
    CHECK(duplicated.values[*rackParameterIndex(NativeModule::wavetable,1,"wavetable.osc.1.position")]==cancelBefore.values[*rackParameterIndex(NativeModule::wavetable,0,"wavetable.osc.1.position")]);
    CHECK(q.undoSoundEdit());CHECK(matches(q,cancelBefore));CHECK(q.redoSoundEdit());CHECK(matches(q,duplicated));
    q.control(*rackParameterIndex(NativeModule::wavetable,1,"wavetable.osc.1.position")).setValue(.87f);auto independentFixture = airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);}); auto& independent = *independentFixture;CHECK(independent.values[*rackParameterIndex(NativeModule::wavetable,0,"wavetable.osc.1.position")]==duplicated.values[*rackParameterIndex(NativeModule::wavetable,0,"wavetable.osc.1.position")]);CHECK(*independent.personalities==*duplicated.personalities);
    RackOperation move;move.kind=RackOperationKind::move;move.selected=duplicate.newInstance;move.destination={RackRole::synth,masterChain,0};CHECK(q.applyRackOperation(move,edit)==RackCandidateStatus::ready);auto movedFixture = airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);}); auto& moved = *movedFixture;CHECK(moved.personalities->instances[1].bindings==independent.personalities->instances[1].bindings);CHECK(moved.values==independent.values);
    CHECK(q.undoSoundEdit());CHECK(matches(q,independent));CHECK(q.redoSoundEdit());CHECK(matches(q,moved));
    RackOperation detach;detach.kind=RackOperationKind::detach;detach.selected=duplicate.newInstance;CHECK(q.applyRackOperation(detach,edit)==RackCandidateStatus::ready);auto detachedFixture = airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);}); auto& detached = *detachedFixture;CHECK(static_cast<const SoundAudioState&>(detached)==static_cast<const SoundAudioState&>(moved));CHECK(detached.personalities->instances.size()==1);
    CHECK(q.undoSoundEdit());CHECK(matches(q,moved));CHECK(q.redoSoundEdit());CHECK(matches(q,detached));
    CHECK(q.loadSoundPreset(preset));CHECK(matches(q,d));CHECK(q.undoSoundEdit());CHECK(matches(q,detached));CHECK(q.redoSoundEdit());CHECK(matches(q,d));
    for(const auto revision:{9223372036854775807ULL,9223372036854775808ULL}){
        auto boundaryFixture = airack::testing::fixtureValue([&]{return d;}); auto& boundary = *boundaryFixture;auto state=std::make_shared<RackPersonalityState>(*boundary.personalities);state->instances[0].design.revision=revision;boundary.personalities=state;
        CHECK(encodeSoundState(boundary,sound));CHECK(q.restoreSoundState(sound.getData(),int(sound.getSize())));auto boundaryBeforeFixture = airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);}); const auto& boundaryBefore = *boundaryBeforeFixture;
        RackDesign fullWidth;CHECK(q.captureRackDesign(adopt.newInstance,fullWidth));CHECK(fullWidth.revision==revision+1&&fullWidth.id==captured.id);
        CHECK(matches(q,boundaryBefore));
    }
    // Processor capture increments revisions, so it refuses a design at UINT64_MAX, which remains legal recall.
    auto maximumOwner=airack::testing::fixtureValue([&]{return d;});auto& maximum=*maximumOwner;auto state=std::make_shared<RackPersonalityState>(*maximum.personalities);state->instances[0].design.revision=std::numeric_limits<std::uint64_t>::max();maximum.personalities=state;
    CHECK(encodeSoundState(maximum,sound));CHECK(q.restoreSoundState(sound.getData(),int(sound.getSize())));auto maximumBeforeOwned=airack::testing::fixtureValue([&]{return personalitySnapshot(q,edit);});const auto& maximumBefore=*maximumBeforeOwned;
    CHECK(!q.captureRackDesign(adopt.newInstance,sentinel));CHECK(matches(q,maximumBefore));
    auto engineEditOwned=airack::testing::fixtureValue([&]{return maximumBefore;});auto& engineEdit=*engineEditOwned;engineEdit.values[13]=0;CHECK(!q.applySoundEdit(engineEdit,SoundEditOrigin::manual,edit));CHECK(matches(q,maximumBefore));
    auto corrupt=sound;static_cast<char*>(corrupt.getData())[corrupt.getSize()-1]^=1;CHECK(!q.restoreSoundState(corrupt.getData(),int(corrupt.getSize())));CHECK(matches(q,maximumBefore));
    const auto badPreset=dir.getChildFile("invalid.arsi");CHECK(badPreset.replaceWithData(corrupt.getData(),corrupt.getSize()));CHECK(!q.loadSoundPreset(badPreset));CHECK(matches(q,maximumBefore));
    corrupt=sound;corrupt.setSize(corrupt.getSize()-1);CHECK(badPreset.replaceWithData(corrupt.getData(),corrupt.getSize()));CHECK(!q.loadSoundPreset(badPreset));CHECK(matches(q,maximumBefore));
    for(const auto size:{std::size_t(793944),std::size_t(793945)}){juce::MemoryBlock oversized(size,true);CHECK(badPreset.replaceWithData(oversized.getData(),oversized.getSize()));CHECK(!q.loadSoundPreset(badPreset));CHECK(matches(q,maximumBefore));}
    CHECK(p.aiWorkflowStatus().connection.phase==companion::ClientPhase::closed);CHECK(q.aiWorkflowStatus().connection.phase==companion::ClientPhase::closed);CHECK(!options.launch.executable.exists());CHECK(!p.aiProposal());CHECK(!q.aiProposal());
    CHECK(dir.deleteRecursively());
}
void rackNativeCapacity()
{
    section("library_transactions_place_full_capacity");companion::ClientOptions options;options.allowLaunch=false;InstrumentProcessor p(options, airack::testing::isolatedRackRuntime());
    SoundDocument empty;empty.layout.synthUse.fill(SlotUse::neverUsed);empty.layout.synthOrder.fill(noSlot);empty.layout.synthCount=0;
    juce::MemoryBlock wire;CHECK(encodeSoundState(empty,wire));CHECK(p.restoreSoundState(wire.getData(),int(wire.getSize())));std::uint64_t edit;personalitySnapshot(p,edit);
    RackOperation op;op.kind=RackOperationKind::place;
    for(std::uint8_t n=0;n<12;++n){op.newInstance=juce::Uuid();op.design=personalityDesign(n<4?NativeModule::analog:NativeModule::drive);op.destination={n<4?RackRole::synth:RackRole::fx,masterChain,std::uint8_t(n<4?n:n-4)};
        CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::ready);const auto d=personalitySnapshot(p,edit);CHECK(d.personalities->instances.size()==n+1);CHECK(d.personalities->instances.back().bindings[0].slot==(n<4?n:n-4));
    }
    const auto full=personalitySnapshot(p,edit);op.newInstance=juce::Uuid();op.destination.position=8;SoundDocument candidate=empty;RackDifference diff;diff.sonic="unchanged";
    CHECK(p.previewRackOperation(op,edit,candidate,diff)==RackCandidateStatus::capacityReached);CHECK(candidate==empty&&diff.sonic=="unchanged");CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::capacityReached);CHECK(personalitySnapshot(p,edit)==full);
    p.getStateInformation(wire);ProjectDocument recalled;CHECK(decodeProjectState(wire.getData(),wire.getSize(),recalled));CHECK(recalled.sound==full);
    CHECK(p.undoSoundEdit());CHECK(personalitySnapshot(p,edit).personalities->instances.size()==11);CHECK(p.redoSoundEdit());CHECK(personalitySnapshot(p,edit)==full);
    p.prepareToPlay(48000,256);juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;note(midi);p.processBlock(audio,midi);CHECK(std::isfinite(peak(audio)));CHECK(peak(audio)>0);CHECK(p.aiWorkflowStatus().connection.phase==companion::ClientPhase::closed);
}
// Rejects audible mapping regressions, incomplete recall and callback metadata ownership.
SoundDocument personalityFixture(int example,int setting,bool master=false)
{return rackPersonalityFixtures::fixture(example,setting,master,[](bool good){CHECK(good);});}
ProjectDocument legacyPersonalityExpectation(const ProjectDocument& original)
{
    // Caller-owned release identities; never derive expected metadata from the decoder's matcher.
    const std::array<RackPackageRef,3> refs{{
        {"ember-reed","1.0.0","14b4f5f907054a35a9d6f95513cc1f5ae57b6ad4e4a383b2975bd17700d8f328"},
        {"glass-current","1.0.0","de4271ed345bad7614652855bcce0d16210f34453a10d3645d957d1b020a8af0"},
        {"thread-press","1.0.0","e6d72527612f86d102ad761a6f60be5417dc8d5f5ac493bbe5a181e0cd47e656"}}};
    auto expected=original;
    CHECK(original.sound.personalities);
    if(!original.sound.personalities)return expected;
    const auto designs=airack::test::frozenExampleRackDesigns();
    auto personalities=std::make_shared<RackPersonalityState>(*original.sound.personalities);
    for(auto& rack:personalities->instances){
        bool known=false;
        for(std::size_t n=0;n<refs.size();++n)if(rack.design==designs[n]){
            CHECK(!rack.package);rack.package=refs[n];known=true;break;
        }
        CHECK(known);
    }
    expected.sound.personalities=std::move(personalities);return expected;
}
juce::AudioBuffer<float> personalitySamples(const SoundDocument& d,int block=512,int frames=0)
{
    companion::ClientOptions offline;offline.allowLaunch=false;InstrumentProcessor p(offline, airack::testing::isolatedRackRuntime());juce::MemoryBlock bytes;
    CHECK(encodeSoundState(d,bytes)&&p.restoreSoundState(bytes.getData(),int(bytes.getSize())));p.prepareToPlay(48000,block);
    if(frames==0)frames=d.personalities->instances.back().design.units.front().module==NativeModule::wavetable?192000:48000;
    juce::AudioBuffer<float> all(2,frames),audio(2,block);juce::MidiBuffer midi;midi.ensureSize(4096);
    for(int offset=0;offset<all.getNumSamples();offset+=block){const int count=std::min(block,all.getNumSamples()-offset);audio.setSize(2,count,false,false,true);audio.clear();midi.clear();
        if(offset==0)midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(100)),0);
        if(offset<=48000&&offset+count>48000)midi.addEvent(juce::MidiMessage::noteOff(1,48),48000-offset);
        p.processBlock(audio,midi);for(int c=0;c<2;++c)all.copyFrom(c,offset,audio,c,0,count);}
    p.releaseResources();return all;
}
double personalityDifference(const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b)
{double result=0;bool finite=true;CHECK(a.getNumSamples()==b.getNumSamples());for(int c=0;c<2;++c)for(int s=0;s<a.getNumSamples();++s){finite=finite&&std::isfinite(a.getSample(c,s))&&std::isfinite(b.getSample(c,s));result=std::max(result,std::abs(double(a.getSample(c,s))-b.getSample(c,s)));}CHECK(finite);return result;}
// Golden values are caller-owned literals from the accepted original recipes.
// 16 float epsilons cover normalized/skew/physical round trips (Air low observed
// error 3.1e-5 Hz), while rejecting meaningful in-range endpoint substitutions.
bool personalityEndpointMatches(const SoundDocument& d,int example,int setting)
{
    constexpr const char* keys[]={"analog.filter.cutoff","reverb.decay_seconds","compressor.threshold_db"};
    constexpr NativeModule modules[]={NativeModule::analog,NativeModule::reverb,NativeModule::compressor};
    constexpr std::uint8_t units[]={0,2,0},slots[]={1,1,1};
    constexpr float low[]={100.f,.3f,-36.f},high[]={10000.f,8.f,0.f};
    const auto& instance=d.personalities->instances.back();
    const auto& focus=instance.design.panel.controls[example==1?3:0];
    const auto binding=std::find_if(instance.bindings.begin(),instance.bindings.end(),[&](auto b){return b.unit==focus.unit;});
    const auto unit=std::find_if(instance.design.units.begin(),instance.design.units.end(),[&](const auto& u){return u.id==focus.unit;});
    if(focus.key!=keys[example]||focus.unit!=units[example]||binding==instance.bindings.end()||unit==instance.design.units.end()
        ||binding->slot!=slots[example]||unit->module!=modules[example])return false;
    const auto index=rackParameterIndex(modules[example],slots[example],keys[example]);
    if(!index)return false;
    const float expected=setting<0?low[example]:high[example];
    const float tolerance=16*std::numeric_limits<float>::epsilon()*std::max(1.f,std::abs(expected));
    return std::abs(parameterPhysicalValue(d.values,*index)-expected)<=tolerance;
}
juce::MemoryBlock legacyPersonalityProjectFixture(const SoundDocument& sound) {
    // Independent frozen v4/v3 projection preserves the original byte digests.
    // Product writers now emit Sound5/Project4 exclusively.
    juce::MemoryBlock modern;CHECK(encodeSoundState(sound,modern));
    juce::MemoryBlock numeric(modern.getData(),6080);
    numeric.append(static_cast<const char*>(modern.getData())+16+8*parameterCount,78+64+758);
    numeric.append(static_cast<const char*>(modern.getData())+currentSoundNumericBytes-12,12);
    CHECK(numeric.getSize()==6992);auto* header=static_cast<unsigned char*>(numeric.getData());header[4]=4;header[8]=246;header[9]=2;
    juce::MemoryOutputStream personalities;personalities.writeShort(1);personalities.writeByte(static_cast<char>(sound.personalities->instances.size()));personalities.writeByte(0);
    for(const auto& instance:sound.personalities->instances){
        juce::var design;CHECK(encodeRackDesignObject(instance.design,design,RackValidationMode::instanceRecall));design.getDynamicObject()->setProperty("version",1);
        for(auto& unit:*design["units"].getArray())unit.getDynamicObject()->removeProperty("algorithm_revision");
        const auto json=juce::JSON::toString(design,true,std::numeric_limits<double>::max_digits10+5).toStdString();
        personalities.write(instance.id.getRawData(),16);personalities.writeByte(instance.edited?1:0);personalities.writeByte(static_cast<char>(instance.bindings.size()));
        for(auto binding:instance.bindings){personalities.writeByte(static_cast<char>(binding.unit));personalities.writeByte(static_cast<char>(binding.slot));}
        personalities.writeInt(static_cast<int>(json.size()));personalities.write(json.data(),json.size());
    }
    juce::MemoryOutputStream legacySound;legacySound.write(numeric.getData(),numeric.getSize());legacySound.writeInt(static_cast<int>(personalities.getDataSize()));legacySound.write(personalities.getData(),personalities.getDataSize());
    const auto soundDigest=juce::SHA256(legacySound.getData(),legacySound.getDataSize()).getRawData();legacySound.write(soundDigest.getData(),32);
    juce::MemoryOutputStream pattern;pattern.write("AIPT",4);pattern.writeShort(1);pattern.writeShort(0);pattern.writeByte(1);pattern.writeByte(4);pattern.writeByte(4);pattern.writeByte(2);pattern.writeDouble(0);pattern.writeShort(0);pattern.writeShort(0);
    const auto patternDigest=juce::SHA256(pattern.getData(),pattern.getDataSize()).getRawData();pattern.write(patternDigest.getData(),32);
    juce::MemoryOutputStream project;project.write("AIPJ",4);project.writeShort(3);project.writeShort(0);project.writeByte(0);project.writeByte(0);project.writeShort(0);project.writeInt(static_cast<int>(legacySound.getDataSize()));project.writeInt(static_cast<int>(pattern.getDataSize()));project.write(legacySound.getData(),legacySound.getDataSize());project.write(pattern.getData(),pattern.getDataSize());
    const auto projectDigest=juce::SHA256(project.getData(),project.getDataSize()).getRawData();project.write(projectDigest.getData(),32);return project.getMemoryBlock();
}
void personality_render_oracles()
{
    section("personality_render_oracles");
    // Caller-owned default project digests pin parity with the retained actual VST3
    // fixtures from source-frozen-v3; these are not generated from the shared helper.
    constexpr const char* fixtureDigests[]={
        "3c897216adc56c9c420f4ae7bc0f2d9eb2c1fe7542de8ad267160525b1612f44",
        "6b55333d7293e2dc0be5aa75973d3de3591d82305cb64c645ba8dc5bdfc6bc05",
        "320206e64387075e58503a720803a83c269fc1a471b25bd98cc534e3069a2d36"};

    for(int example=0;example<3;++example){auto low=personalityFixture(example,-1),high=personalityFixture(example,1);
        CHECK(personalityEndpointMatches(low,example,-1));CHECK(personalityEndpointMatches(high,example,1));
        if(example==2){CHECK(personalityEndpointMatches(personalityFixture(example,-1,true),example,-1));CHECK(personalityEndpointMatches(personalityFixture(example,1,true),example,1));}
        for(const auto& instance:low.personalities->instances)for(const auto& control:instance.design.panel.controls){const auto b=std::find_if(instance.bindings.begin(),instance.bindings.end(),[&](auto v){return v.unit==control.unit;});const auto u=std::find_if(instance.design.units.begin(),instance.design.units.end(),[&](const auto& v){return v.id==control.unit;});const auto i=rackParameterIndex(u->module,b->slot,control.key);CHECK(i);CHECK(control.minimum>=parameterRegistry()[*i].minimum&&control.maximum<=parameterRegistry()[*i].maximum);}
        auto a=personalitySamples(low),b=personalitySamples(high);CHECK(a.getMagnitude(0,a.getNumSamples())<1&&b.getMagnitude(0,b.getNumSamples())<1);CHECK(personalityDifference(a,a)==0&&personalityDifference(b,b)==0);
        const double left=rackPersonalityFixtures::responseMetric(example,a,[](bool good){CHECK(good);});
        const double right=rackPersonalityFixtures::responseMetric(example,b,[](bool good){CHECK(good);});
        CHECK(std::isfinite(left)&&std::isfinite(right));CHECK(left<right);std::cout<<"PERSONALITY_RESPONSE example="<<example<<" low="<<left<<" high="<<right<<'\n';
        auto baseline=personalityFixture(example,0);ProjectDocument parity;parity.sound=baseline;const auto parityBytes=legacyPersonalityProjectFixture(baseline);
        const auto legacyHash=juce::SHA256(parityBytes).toHexString();CHECK(legacyHash==fixtureDigests[example]);
        ProjectDocument legacy;const bool decoded=decodeProjectState(parityBytes.getData(),parityBytes.getSize(),legacy);CHECK(decoded);
        const bool equal=decoded&&legacy==legacyPersonalityExpectation(parity);CHECK(equal);
        std::cout<<"PERSONALITY_LEGACY example="<<example<<" sha256="<<legacyHash<<" decode="<<decoded<<" equality="<<equal<<'\n';
        juce::MemoryBlock bytes;CHECK(encodeSoundState(baseline,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==baseline);
        auto original=personalitySamples(baseline,512,8192);CHECK(personalityDifference(original,personalitySamples(recalled,64,8192))<=1e-6);
        auto metadata=std::make_shared<RackPersonalityState>(*baseline.personalities);metadata->instances.back().design.name="Metadata-only swap";metadata->instances.back().design.revision++;baseline.personalities=metadata;CHECK(validateSoundDocument(baseline));CHECK(personalityDifference(original,personalitySamples(baseline,512,8192))<=1e-6);
    }
}
void personality_callback_ownership()
{
    section("personality_callback_ownership");companion::ClientOptions offline;offline.allowLaunch=false;InstrumentProcessor p(offline, airack::testing::isolatedRackRuntime());
    auto d=fullDocument();for(int n=0;n<12;++n){const bool synth=n<4;const int slot=synth?n:n-4;const auto module=synth?(n%2?NativeModule::wavetable:NativeModule::analog):NativeModule(unsigned(d.layout.effects[std::size_t(slot)].kind)+2);RackDesign design;design.id=juce::Uuid();design.name="Callback capacity";design.role=synth?RackRole::synth:RackRole::fx;design.units={{0,module,{}}};design.panel.sections={"Native"};RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.design=design;op.destination.role=design.role;op.allocation={{0,std::uint8_t(slot)}};SoundDocument next;RackDifference delta;CHECK(makeRackCandidate(d,op,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);d=std::move(next);}CHECK(d.personalities&&d.personalities->instances.size()==12);
    juce::MemoryBlock a,b;CHECK(encodeSoundState(d,a));auto changed=std::make_shared<RackPersonalityState>(*d.personalities);changed->instances.back().design.name="Presentation swap";d.personalities=changed;CHECK(encodeSoundState(d,b));CHECK(p.restoreSoundState(a.getData(),int(a.getSize())));p.prepareToPlay(48000,128);
    juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;midi.ensureSize(4096);note(midi);p.processBlock(audio,midi);midi.clear();
    const auto library=juce::File::getCurrentWorkingDirectory().getChildFile("build/rack-personalities-callback-"+juce::Uuid().toString()+"/fixture.rack");CHECK(library.getParentDirectory().createDirectory().wasOk());CHECK(saveRackDesign(airack::test::frozenExampleRackDesigns()[0],library));
    std::atomic<bool> stop=false,started=false;std::atomic<unsigned> prepared=0;std::thread control([&]{started=true;while(!stop){RackDesign design;juce::MemoryBlock encoded;if(loadRackDesign(library,design)&&encodeRackDesign(design,encoded))++prepared;}});
    while(!started)std::this_thread::yield();
#if defined(_MSC_VER) && defined(_DEBUG)
    for(const auto pair:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}}){
        p.prepareToPlay(pair.first,pair.second);audio.setSize(2,pair.second);note(midi);p.processBlock(audio,midi);midi.clear();
        allocations=0;audioFrees=0;auto old=_CrtSetAllocHook(allocationHook);
        for(int k=0;k<128;++k){const auto& state=k%2?a:b;CHECK(p.restoreSoundState(state.getData(),int(state.getSize())));
            if(k%16==0){std::uint64_t revision;auto type=document(p,&revision);type.layout.effects[0].kind=EffectKind::delay;CHECK(p.applySoundEdit(type,SoundEditOrigin::manual,revision));}
            tracking=true;p.control(17).setValue(float(k%2));p.processBlock(audio,midi);tracking=false;}
        _CrtSetAllocHook(old);CHECK(allocations==0&&audioFrees==0);std::cout<<"PERSONALITY_CALLBACK rate="<<pair.first<<" block="<<pair.second<<" allocations="<<allocations<<" frees="<<audioFrees<<" control-prepared="<<prepared.load()<<'\n';
    }
#else
    CHECK(false); // Callback ownership acceptance requires MSVC Debug CRT.
#endif
    stop=true;control.join();CHECK(prepared>0);p.releaseResources();
}

void personalityPacket(const juce::File& output)
{
    CHECK(!output.exists());if(output.exists())return;CHECK(output.createDirectory().wasOk());
    const auto designs=airack::test::frozenExampleRackDesigns();
    for(int n=0;n<3;++n)for(int setting=-1;setting<=1;++setting)for(int surface=0;surface<2;++surface)for(int density=0;density<2;++density){
        auto d=personalityFixture(n,setting);auto metadata=std::make_shared<RackPersonalityState>(*d.personalities);auto& rack=metadata->instances.back();rack.design.panel.surface=std::uint8_t(surface);rack.design.panel.density=std::uint8_t(density);d.personalities=metadata;
        companion::ClientOptions offline;offline.allowLaunch=false;InstrumentProcessor p(offline, airack::testing::isolatedRackRuntime());juce::MemoryBlock bytes;CHECK(encodeSoundState(d,bytes));CHECK(p.restoreSoundState(bytes.getData(),int(bytes.getSize())));
        RackPanelComponent panel(p,*studio::studioSkin());panel.showInstance(rack.id);panel.addToDesktop(0);panel.setVisible(true);panel.refresh(d);
        const auto stem=designs[n].name.replaceCharacter(' ','-')+"-"+juce::String(setting)+"-surface"+juce::String(surface)+"-density"+juce::String(density);
        auto save=[&](juce::Component& component,const juce::String& name){dispatch();auto image=component.createComponentSnapshot(component.getLocalBounds(),true);auto file=output.getChildFile(name+".png");CHECK(!file.exists());auto stream=file.createOutputStream();CHECK(image.isValid()&&stream);if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(image,*stream));ProjectDocument state;std::uint64_t revision;CHECK(p.tryProjectSnapshot(state,revision));juce::MemoryBlock encoded;CHECK(encodeProjectState(state,encoded));CHECK(output.getChildFile(name+".airackproject").replaceWithData(encoded.getData(),encoded.getSize()));};
        for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(2200,1600)}){panel.setSize(size.x,size.y);save(panel,stem+"-"+juce::String(size.x));}
        if(setting==0&&surface==0&&density==0){const auto focusIndex=n==1?3:0;const auto& focus=rack.design.panel.controls[focusIndex];const auto binding=std::find_if(rack.bindings.begin(),rack.bindings.end(),[&](auto b){return b.unit==focus.unit;});const auto unit=std::find_if(rack.design.units.begin(),rack.design.units.end(),[&](auto u){return u.id==focus.unit;});CHECK(unit!=rack.design.units.end());const auto module=unit->module;const auto i=*rackParameterIndex(module,binding->slot,focus.key);const auto outside=parameterRegistry()[i].maximum>focus.maximum?parameterRegistry()[i].maximum:parameterRegistry()[i].minimum;CHECK(outside<focus.minimum||outside>focus.maximum);p.control(i).setValueNotifyingHost(p.control(i).convertTo0to1(outside));auto actual=document(p);panel.refresh(actual);panel.setSize(1080,680);if(auto* control=findComponent(panel,"rack.focus."+juce::String(focusIndex)))control->grabKeyboardFocus();dispatch();save(panel,stem+"-outside-useful-range");panel.setSize(2200,1600);save(panel,stem+"-outside-useful-range-2200");panel.setSize(1080,680);
            if(n<2){const auto synth=std::find_if(rack.bindings.begin(),rack.bindings.end(),[&](auto b){return b.unit==rack.design.units.front().id;});p.control(13+4*synth->slot).setValueNotifyingHost(n==0?1.f:0.f);actual=document(p);panel.refresh(actual);save(panel,stem+"-disabled-retained");panel.setSize(2200,1600);save(panel,stem+"-disabled-retained-2200");panel.setSize(1080,680);}
            actual=document(p);auto edited=std::make_shared<RackPersonalityState>(*actual.personalities);edited->instances.back().edited=true;actual.personalities=edited;juce::MemoryBlock editedState;CHECK(encodeSoundState(actual,editedState));CHECK(p.restoreSoundState(editedState.getData(),int(editedState.getSize())));actual=document(p);CHECK(actual.personalities&&actual.personalities->instances.back().edited);panel.refresh(actual);save(panel,stem+"-Edited");panel.setSize(2200,1600);save(panel,stem+"-Edited-2200");panel.setSize(1080,680);
            std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);if(auto* lever=dynamic_cast<juce::Button*>(findComponent(*editor,"rack.toggle")))click(*lever);else CHECK(false);
            if(auto* unit=programmer(*editor)){if(n==0){unit->showPage("synth2");unit->showSubpage("analog");}else{/* opened from the next FX N: pressing the shown one again steps its pages */unit->showPage("effects");unit->showSubpage("fx"+juce::String((int(binding->slot)+1)%8+1));unit->showSubpage("fx"+juce::String(int(binding->slot)+1));}}else CHECK(false);
            save(*editor,stem+"-Advanced");editor->setSize(2200,1600);save(*editor,stem+"-Advanced-2200");
        }
    }
    std::cout<<"PERSONALITY_NATIVE_PACKET source-bound native JUCE pixels; loaded HWND pixels not captured\n";
}



void nativeReplacementPreviewUndo() {
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(p));RackOperation op;op.newInstance=juce::Uuid();RackDesign design;design.name="Incumbent";design.role=RackRole::fx;design.units={{0,NativeModule::drive,{{0,"drive.gain_db",.22f}}}};design.panel.sections={"Tone"};op.design=design;op.destination={RackRole::fx,masterChain,0};op.allocation={{0,0}};
    std::uint64_t edit;personalitySnapshot(p,edit);CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::ready);const auto before=personalitySnapshot(p,edit);
    auto replacement=op;replacement.newInstance=juce::Uuid();replacement.choices.remove={op.newInstance};replacement.allowSlotReuse=true;replacement.design->name="Replacement";replacement.design->units[0].defaults[0].normalized=.37f;
    SoundDocument preview;RackDifference delta;CHECK(p.previewRackOperation(replacement,edit,preview,delta)==RackCandidateStatus::ready);CHECK(personalitySnapshot(p,edit)==before);CHECK(preview.personalities&&preview.personalities->instances.size()==1);CHECK(preview.personalities->instances[0].id==replacement.newInstance);
    CHECK(p.applyRackOperation(replacement,edit)==RackCandidateStatus::ready);const auto after=personalitySnapshot(p,edit);CHECK(after==preview);CHECK(p.undoSoundEdit());CHECK(personalitySnapshot(p,edit)==before);CHECK(p.redoSoundEdit());CHECK(personalitySnapshot(p,edit)==after);
}
void wholeFindingTransactions() {
    nativeReplacementPreviewUndo();
#ifndef NDEBUG
    section("whole_finding_gesture_and_group_atomic_rejection");
    static thread_local InstrumentProcessor* hooked=nullptr;
    static thread_local int publications=0;
    const auto reject=+[](int at){if(at==3){++publications;hooked->control(13).setValue(1);}};
    auto pFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*pFixture;CHECK(blankInstance(p));RecallListener listener;listener.action=[]{};p.control(0).addListener(&listener);
    CHECK(p.beginParameterGesture(0));CHECK(p.setParameterGestureValue(0,.8f));
    p.control(idx("synth.1.modulation.1.target")).setValue(1);
    hooked=&p;publications=0;InstrumentStructuralTestAccess::hook(reject);
    CHECK(!p.endParameterGesture(0));InstrumentStructuralTestAccess::hook(nullptr);
    CHECK(publications==1);CHECK(listener.gestures==2);CHECK(p.control(13).getValue()==1);CHECK(!p.undoSoundEdit());
    p.control(idx("synth.1.modulation.1.target")).setValue(0);
    CHECK(p.beginParameterGesture(0));CHECK(p.setParameterGestureValue(0,.7f));CHECK(p.endParameterGesture(0));CHECK(listener.gestures==4);CHECK(p.undoSoundEdit());p.control(0).removeListener(&listener);
    for(const bool leave:{true,false}) {
        auto qFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& q=*qFixture;GroupIntent intent;intent.group=juce::Uuid();intent.member=juce::Uuid();CHECK(setGroupText("Atomic",intent.name));CHECK(setGroupText("bass",intent.role));CHECK(q.setGroupIntent(intent));
        const auto link=q.groupLinkView();const auto context=q.aiMusicalContext();const auto authority=InstrumentStructuralTestAccess::authority(q);const auto recovery=InstrumentStructuralTestAccess::recovery(q);
        auto normalized=intent;normalized.member=juce::Uuid();hooked=&q;publications=0;InstrumentStructuralTestAccess::hook(reject);
        CHECK(!(leave?q.leaveGroupIntent():q.acknowledgeGroupIntent(link.runtime,link.loadRevision,intent,normalized)));
        InstrumentStructuralTestAccess::hook(nullptr);CHECK(publications==1);CHECK(q.groupLinkView()==link);CHECK(q.aiMusicalContext()==context);
        CHECK(InstrumentStructuralTestAccess::authority(q)==authority);CHECK(InstrumentStructuralTestAccess::recovery(q)==recovery);CHECK(InstrumentStructuralTestAccess::savedGroup(q)==intent);CHECK(q.control(13).getValue()==1);
        juce::MemoryBlock bytes;q.getStateInformation(bytes);auto decodedFixture=std::make_unique<ProjectDocument>();auto& decoded=*decodedFixture;CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),decoded));CHECK(decoded.group==intent);
        CHECK(leave?q.leaveGroupIntent():q.acknowledgeGroupIntent(link.runtime,link.loadRevision,intent,normalized));
        CHECK(q.groupLinkView().intent==(leave?std::optional<GroupIntent>{}:std::optional<GroupIntent>{normalized}));
        q.getStateInformation(bytes);CHECK(decodeProjectState(bytes.getData(),bytes.getSize(),decoded));CHECK(decoded.group==q.groupLinkView().intent);
    }
    auto sameFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& same=*sameFixture;GroupIntent intent;intent.group=juce::Uuid();intent.member=juce::Uuid();CHECK(setGroupText("Same",intent.name));CHECK(same.setGroupIntent(intent));const auto link=same.groupLinkView();
    hooked=&same;publications=0;InstrumentStructuralTestAccess::hook(reject);CHECK(same.acknowledgeGroupIntent(link.runtime,link.loadRevision,intent,intent));InstrumentStructuralTestAccess::hook(nullptr);CHECK(publications==0);CHECK(same.groupLinkView().phase==GroupLinkPhase::admitted);
#endif
}
// The studio feedback spec, decision 4: a new instance has no rack and no synth bank, in its document and its host
// parameters, and plays nothing.
void newInstanceEmpty(const juce::String& prefix) {
    section("new_instance_empty: a new instance has no rack and no synth bank and renders silence; copperArcInstance and blankInstance restore the starts before it");
    auto pFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*pFixture;const auto d=document(p);const auto expected=newInstanceSound();
    CHECK(d==expected&&d.layout.synthCount==0&&(!d.personalities||d.personalities->instances.empty()));
    for(std::size_t i=0;i<parameterCount;++i)if(p.control(i).getValue()!=expected.values[i]){CHECK(false);std::cerr<<"  host parameter "<<parameterRegistry()[i].id<<'\n';break;}
    juce::MemoryBlock saved;p.getStateInformation(saved);ProjectDocument stored;
    CHECK(decodeProjectState(saved.getData(),saved.getSize(),stored)&&stored.sound==expected);
    auto recalledFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& recalled=*recalledFixture;recalled.setStateInformation(saved.getData(),int(saved.getSize()));CHECK(document(recalled)==d);
    CHECK(!p.undoSoundEdit());
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;p.prepareToPlay(48000,256);note(midi);
    for(int block=0;block<8;++block){for(int c=0;c<2;++c)juce::FloatVectorOperations::fill(audio.getWritePointer(c),1.0f,256);p.processBlock(audio,midi);CHECK(peak(audio)==0);midi.clear();}
    auto copperFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& copper=*copperFixture;CHECK(copperArcInstance(copper));CHECK(document(copper)==copperArcSound());
    auto blankFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& blank=*blankFixture;CHECK(blankInstance(blank));
    SoundDocument old;old.compatibility=newInstanceKernelCompatibility();CHECK(document(blank)==old);
    if(prefix.isEmpty())return;
    auto freshFixture=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& fresh=*freshFixture;std::unique_ptr<juce::AudioProcessorEditor> e(fresh.createEditor());CHECK(e!=nullptr);if(!e)return;
    e->setVisible(true);e->setSize(1600,1007);dispatch();
    const auto image=e->createComponentSnapshot(e->getLocalBounds(),true);auto out=juce::File(prefix+"-new-instance.png").createOutputStream();
    CHECK(out&&image.isValid());if(out)CHECK(juce::PNGImageFormat().writeImageToStream(image,*out));
}
void freshInstanceKernels() {
    section("fresh_instance_kernels: new instances select modern synth, drive, delay, reverb, compressor, eq, filter, chorus, flanger, dimension and frequency-split kernels; recall keeps saved revisions");
    auto freshFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& fresh = *freshFixture;auto initialFixture = airack::testing::fixtureValue([&]{return document(fresh);}); const auto& initial = *initialFixture;
    const auto modern=[](std::size_t bank){return bank==reverbEffectBank||bank==static_cast<std::size_t>(EffectKind::drive)||bank==delayEffectBank
        ||bank==static_cast<std::size_t>(EffectKind::compressor)||bank==eqEffectBank||bank==filterEffectBank||bank==static_cast<std::size_t>(EffectKind::chorus)
        ||bank==static_cast<std::size_t>(EffectKind::flanger)||bank==static_cast<std::size_t>(EffectKind::dimension)||bank==frequencySplitEffectBank;};
    for(const auto& banks:initial.compatibility.synth)for(auto revision:banks)CHECK(revision==modernSynthAlgorithmRevision);
    for(const auto& banks:initial.compatibility.effects)for(std::size_t bank=0;bank<banks.size();++bank)
        CHECK(banks[bank]==(modern(bank)?AlgorithmRevision{2}:legacyAlgorithmRevision));
    auto patchFixture = std::make_unique<RoutedPatch>(); auto& patch = *patchFixture;CHECK(makeRoutedPatch(initial,patch));
    for(const auto& layer:patch.synth.layers)CHECK(layer.algorithmRevision==modernSynthAlgorithmRevision);
    // A sound has eight effect slots, so the ninth modern kind goes to a second instance.
    constexpr std::array kinds{EffectKind::reverb,EffectKind::delay,EffectKind::drive,EffectKind::compressor,EffectKind::eq,EffectKind::chorus,EffectKind::flanger,EffectKind::dimension,EffectKind::filter};
    int present=0;
    for(std::size_t first=0;first<kinds.size();first+=8){
        auto editedFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& edited = *editedFixture;std::uint64_t revision=0;
        for(std::size_t k=first;k<std::min(kinds.size(),first+8);++k){
            auto beforeFixture = airack::testing::fixtureValue([&]{return document(edited,&revision);}); const auto& before = *beforeFixture;
            CHECK(edited.applyRackEdit({RackEditKind::addEffect,noSlot,masterChain,before.layout.effectCount[masterChain],kinds[k]},revision)==RackEditResult::applied);}
        auto addedFixture = airack::testing::fixtureValue([&]{return document(edited);}); const auto& added = *addedFixture;CHECK(makeRoutedPatch(added,patch));
        for(std::size_t slot=0;slot<added.layout.effects.size();++slot)if(added.layout.effects[slot].use==SlotUse::present){++present;
            CHECK(patch.effects[slot].algorithmRevision==(modern(static_cast<std::size_t>(added.layout.effects[slot].kind))?AlgorithmRevision{2}:legacyAlgorithmRevision));}
    }
    CHECK(present==9);
    juce::MemoryBlock saved;fresh.getStateInformation(saved);auto reopenedFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& reopened = *reopenedFixture;
    reopened.setStateInformation(saved.getData(),static_cast<int>(saved.getSize()));CHECK(document(reopened)==initial);
    auto olderFixture = std::make_unique<ProjectDocument>(); auto& older = *olderFixture;juce::MemoryBlock project;CHECK(encodeProjectState(older,project));auto hostFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& host = *hostFixture;
    host.setStateInformation(project.getData(),static_cast<int>(project.getSize()));CHECK(document(host).compatibility==KernelCompatibility{});
    juce::MemoryBlock preset;CHECK(encodeSoundState(older.sound,preset));auto loadedFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& loaded = *loadedFixture;CHECK(restore(loaded,preset));
    CHECK(document(loaded).compatibility==KernelCompatibility{});
}
void kernelAndModePublication() {
    section("legacy_kernel_and_mode_migration: publication, audio and selective undo");
    auto pFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pFixture; auto qFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& q = *qFixture;CHECK(copperArcInstance(p)&&copperArcInstance(q));auto initialFixture = airack::testing::fixtureValue([&]{return document(p);}); auto& initial = *initialFixture;
    CHECK(initial.layout.synthUse[0]==SlotUse::present&&initial.layout.synthCount==1);
    CHECK(document(q)==initial);p.prepareToPlay(48000,128);q.prepareToPlay(48000,128);
    juce::AudioBuffer<float> a(2,128),b(2,128);juce::MidiBuffer note,other;
    note.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(90)),0);other=note;
    p.processBlock(a,note);q.processBlock(b,other);CHECK(a.getRMSLevel(0,0,128)>0);
    p.control(758).setValue(1);auto selectedFixture = airack::testing::fixtureValue([&]{return document(p);}); const auto& selected = *selectedFixture;CHECK(selected.values[758]==1&&selected.compatibility==newInstanceKernelCompatibility());
    note.clear();other.clear();p.processBlock(a,note);q.processBlock(b,other);
    for(int ch=0;ch<2;++ch)for(int n=0;n<128;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));
    juce::MemoryBlock project;p.getStateInformation(project);auto decodedFixture = std::make_unique<ProjectDocument>(); auto& decoded = *decodedFixture;CHECK(decodeProjectState(project.getData(),project.getSize(),decoded));CHECK(decoded.sound==selected);
    std::uint64_t revision;auto beforeFixture = airack::testing::fixtureValue([&]{return document(p,&revision);}); auto& before = *beforeFixture;auto badFixture = airack::testing::fixtureValue([&]{return before;}); auto& bad = *badFixture;bad.compatibility.effects[7][3]=3;
    CHECK(!p.applySoundEdit(bad,SoundEditOrigin::manual,revision));std::uint64_t unchanged;CHECK(document(p,&unchanged)==before&&unchanged==revision);
    p.releaseResources();q.releaseResources();
#ifndef NDEBUG
    auto aiFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& ai = *aiFixture;auto originalFixture = std::make_unique<ProjectDocument>(); auto& original = *originalFixture;original.sound=document(ai);auto candidateFixture = airack::testing::fixtureValue([&]{return original;}); auto& candidate = *candidateFixture;candidate.sound.values[758]=1;
    CHECK(InstrumentStructuralTestAccess::aiEdit(ai,original,candidate));ai.control(756).setValue(.3f);
    CHECK(ai.undoSoundEdit());auto undoneFixture = airack::testing::fixtureValue([&]{return document(ai);}); auto& undone = *undoneFixture;CHECK(undone.values[758]==0&&undone.values[756]==.3f&&undone.compatibility==newInstanceKernelCompatibility());
    CHECK(ai.redoSoundEdit());auto redoneFixture = airack::testing::fixtureValue([&]{return document(ai);}); auto& redone = *redoneFixture;CHECK(redone.values[758]==1&&redone.values[756]==.3f&&redone.compatibility==newInstanceKernelCompatibility());
    auto rejectedFixture = airack::testing::fixtureValue([&]{return candidate;}); auto& rejected = *rejectedFixture;rejected.sound.compatibility.synth[3][1]=3;CHECK(!InstrumentStructuralTestAccess::aiEdit(ai,candidate,rejected));CHECK(document(ai)==redone);
#endif
}
template<class Processor>
void preparedQualityPolicy()
{
    section("quality_prepared_mode_truth: selected/effective/pending and live held notes");
    if constexpr(requires(Processor& p){p.selectedRenderQuality();p.effectiveRenderQuality();p.renderQualityPending();})
    {
        using Quality=decltype(std::declval<Processor&>().effectiveRenderQuality());
        const auto realtime=static_cast<Quality>(0),higher=static_cast<Quality>(1);
        Processor p,q;CHECK(copperArcInstance(p)&&copperArcInstance(q));p.prepareToPlay(48000,512);q.prepareToPlay(48000,512);
        CHECK(p.selectedRenderQuality()==realtime && p.effectiveRenderQuality()==realtime && p.getLatencySamples()==0);
        juce::AudioBuffer<float> a(2,512),b(2,512);juce::MidiBuffer ma,mb;
        ma.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),17);mb=ma;
        p.processBlock(a,ma);q.processBlock(b,mb);CHECK(a.getRMSLevel(0,0,512)>0);
        p.prepareToPlay(7999,512);p.prepareToPlay(48000,0);
        CHECK(p.effectiveRenderQuality()==realtime && p.getLatencySamples()==0);
        p.control(758).setValue(1);p.setNonRealtime(true);ma.clear();mb.clear();
        CHECK(p.selectedRenderQuality()==higher && p.effectiveRenderQuality()==realtime && p.renderQualityPending());
        p.processBlock(a,ma);q.processBlock(b,mb);
        for(int ch=0;ch<2;++ch)for(int n=0;n<512;++n)CHECK(a.getSample(ch,n)==b.getSample(ch,n));
        CHECK(p.effectiveRenderQuality()==realtime && p.getLatencySamples()==0);
        p.prepareToPlay(48000,512);CHECK(p.effectiveRenderQuality()==higher && !p.renderQualityPending() && p.getLatencySamples()>0);
        p.setNonRealtime(false);CHECK(p.effectiveRenderQuality()==higher && p.renderQualityPending());
        p.prepareToPlay(48000,512);CHECK(p.effectiveRenderQuality()==realtime && p.getLatencySamples()==0);
        p.releaseResources();q.releaseResources();
    }
    else CHECK(false);
}
void qualityWrapperAllocations()
{
    section("quality_higher_wrapper_callback_allocations_frees");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}}){
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};auto full=fullDocument();full.values[758]=1;CHECK(restore(p,state(full)));p.setNonRealtime(true);p.prepareToPlay(geometry.first,geometry.second);
        CHECK(p.effectiveRenderQuality()==RenderQuality::higher&&!p.renderQualityPending());
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;held.ensureSize(4096);released.ensureSize(4096);stop.ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,juce::uint8(100)),n-48);released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations=0;audioFrees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
        p.processBlock(audio,held);tracking=false;CHECK(allocations==0&&audioFrees==0);CHECK(audio.getMagnitude(0,geometry.second)>0);
        std::cout<<"QUALITY_WRAPPER_FIRST rate="<<geometry.first<<" block="<<geometry.second<<" callbacks=1 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
        allocations=0;audioFrees=0;tracking=true;
        for(int n=0;n<96;++n){p.control(0).setValue(.4f+.001f*(n%10));p.processBlock(audio,n==32?released:n==64?stop:empty);}
        tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0&&audioFrees==0);
        std::cout<<"QUALITY_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" mode="<<int(p.effectiveRenderQuality())<<" layers=4 notes=16 effects=8 routes=64 callbacks=96 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
        p.releaseResources();
    }
}
void modernReverbSelectiveAiHistory()
{
    section("modern_reverb_actual_ai_action_selective_undo_and_embedded_defaults");
#ifndef NDEBUG
    for(bool embedded:{false,true})for(int quality:{0,1}){
        SoundDocument legacy;legacy.values[758]=float(quality);legacy.compatibility.effects[7][2]=2;
        RackOperation place;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDesign originalDesign;originalDesign.name="Legacy history";originalDesign.role=RackRole::fx;originalDesign.revision=17;
        originalDesign.units={{0,NativeModule::reverb,{{0,"reverb.mix",.18f},{0,"reverb.decay_seconds",.07f}},1}};
        originalDesign.panel.sections={"Space"};originalDesign.panel.controls={{"Mix",0,0,"reverb.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        place.design=originalDesign;RackDifference difference;SoundDocument placed;
        CHECK(makeRackCandidate(legacy,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);legacy=placed;
        if(!embedded)legacy.personalities.reset();
        const auto mix=idx("effect.1.reverb.mix"),inactiveDrive=idx("effect.8.drive.gain_db");legacy.values[inactiveDrive]=.22f;
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(restore(p,state(legacy)));ProjectDocument before;std::uint64_t revision=0;CHECK(p.tryProjectSnapshot(before,revision));CHECK(before.sound==legacy);
        auto candidate=before;auto modernDesign=originalDesign;modernDesign.id=juce::Uuid();modernDesign.revision=29;modernDesign.name="Modern history";
        modernDesign.units[0].algorithmRevision=2;modernDesign.units[0].defaults={{0,"reverb.mix",.46f},{0,"reverb.decay_seconds",.025f}};
        if(embedded){
            RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=place.newInstance;swap.design=modernDesign;
            swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
            CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        }else{
            auto next=before.sound;next.compatibility.effects[0][2]=2;next.values[mix]=.46f;next.values[idx("effect.1.reverb.decay_seconds")]=.025f;
            CHECK(reconcileRackEdit(before.sound,next,{},SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        }
        CHECK(candidate.sound.compatibility.effects[0][2]==2&&candidate.sound.compatibility.effects[7][2]==2);
        CHECK(InstrumentStructuralTestAccess::aiEdit(p,before,candidate));CHECK(document(p)==candidate.sound);
        p.control(756).setValue(.3f);p.control(inactiveDrive).setValue(.37f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[756]=.3f;expected->sound.values[inactiveDrive]=.37f;}
        CHECK(p.undoSoundEdit());ProjectDocument undone;CHECK(p.tryProjectSnapshot(undone,revision));CHECK(undone==expectedBefore);
        CHECK(p.redoSoundEdit());ProjectDocument redone;CHECK(p.tryProjectSnapshot(redone,revision));CHECK(redone==expectedAfter);
        if(embedded){
            CHECK(redone.sound.personalities&&redone.sound.personalities->instances.size()==1);
            if(redone.sound.personalities&&redone.sound.personalities->instances.size()==1)CHECK(redone.sound.personalities->instances[0].design==modernDesign);
            p.control(mix).setValue(.72f);const auto live=document(p);RackDesign captured;
            CHECK(p.captureRackDesign(place.newInstance,captured));CHECK(document(p)==live);CHECK(captured.units.size()==1);
            if(captured.units.size()==1){CHECK(captured.id==modernDesign.id&&captured.revision==30&&captured.units[0].algorithmRevision==2);
                CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& value){return value.key=="reverb.mix"&&value.normalized==.72f;}));}
            CHECK(live.personalities&&live.personalities->instances.size()==1);
            if(live.personalities&&live.personalities->instances.size()==1)CHECK(live.personalities->instances[0].design==modernDesign);
        }
        CHECK(p.undoSoundEdit());CHECK(document(p)==expectedBefore.sound);
        CHECK(p.redoSoundEdit());CHECK(document(p)==expectedAfter.sound);
        ProjectDocument current;CHECK(p.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][3]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(p,current,invalid));CHECK(document(p)==current.sound);
    }
#endif
}
void modernReverbWrapperCallbacks()
{
    section("modern_reverb_first_and_subsequent_wrapper_callbacks_allocations_frees");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);
        std::array<std::array<std::size_t,5>,8> controls{};
        constexpr const char* keys[]={"reverb.size","reverb.decay_seconds","reverb.damping","reverb.predelay_ms","reverb.mix"};
        for(std::size_t slot=0;slot<8;++slot){full.layout.effects[slot].kind=EffectKind::reverb;full.compatibility.effects[slot][2]=2;
            for(std::size_t control=0;control<5;++control)controls[slot][control]=idx("effect."+juce::String(int(slot)+1)+"."+keys[control]);}
        InstrumentProcessor p{airack::testing::isolatedRackRuntime()};CHECK(restore(p,state(full)));p.setNonRealtime(quality==1);p.prepareToPlay(geometry.first,geometry.second);
        CHECK(p.effectiveRenderQuality()==(quality==0?RenderQuality::realtime:RenderQuality::higher)&&!p.renderQualityPending());
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int note=48;note<64;++note)held.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),note-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        bool bounded=true;const auto inspect=[&]{for(int ch=0;ch<2;++ch)for(int n=0;n<geometry.second;++n){const auto sample=audio.getSample(ch,n);bounded&=std::isfinite(sample)&&std::abs(sample)<=.891252f;}};
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations=0;audioFrees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
        p.processBlock(audio,held);tracking=false;inspect();CHECK(allocations==0&&audioFrees==0);CHECK(peak(audio)>0);
        std::cout<<"MODERN_WRAPPER_FIRST rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" callbacks=1 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
        allocations=0;audioFrees=0;tracking=true;
        for(int block=0;block<96;++block){for(std::size_t control=0;control<5;++control)p.control(controls[std::size_t(block%8)][control]).setValue(.1f+.005f*float((block+int(control))%30));
            p.processBlock(audio,block==32?released:block==64?stop:empty);inspect();}
        tracking=false;CHECK(allocations==0&&audioFrees==0);
        std::cout<<"MODERN_WRAPPER_SUBSEQUENT rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" layers=4 notes=16 effects=8 routes=64 callbacks=96 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
        for(auto algorithm:{AlgorithmRevision(1),AlgorithmRevision(2)}){
            std::uint64_t revision;auto switched=document(p,&revision);for(auto& bank:switched.compatibility.effects)bank[2]=algorithm;
            CHECK(p.applySoundEdit(switched,SoundEditOrigin::manual,revision));allocations=0;audioFrees=0;tracking=true;
            p.processBlock(audio,held);inspect();for(int block=0;block<4;++block){p.processBlock(audio,empty);inspect();}
            tracking=false;CHECK(allocations==0&&audioFrees==0);
            std::cout<<"MODERN_WRAPPER_REVISION rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" algorithm="<<int(algorithm)<<" callbacks=5 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
        }
        _CrtSetAllocHook(old);
#else
        CHECK(false);
#endif
        CHECK(bounded);p.releaseResources();
    }
}
void eqSelectiveHistoryAndWholeDefaults()
{
    section("eq_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842a802");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::eq&&candidate.sound.compatibility.effects[0][eqEffectBank]==modernEqAlgorithmRevision);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.eq.high_shelf.q");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.eq.bell.gain_db")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==modernEqAlgorithmRevision);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="eq.bell.gain_db"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][4]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void eqPreparedWrapperAndTail()
{
    section("eq_prepared_full_wrapper_first_live_allocations_and_pole_tail");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::eq;
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::size_t,8> controls;for(int slot=0;slot<8;++slot)controls[slot]=idx("effect."+juce::String(slot+1)+".eq.bell.gain_db");
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){processor.control(controls[block%8]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"EQ_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind::eq;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    const auto gain=idx("effect.1.eq.bell.gain_db"),frequency=idx("effect.1.eq.bell.frequency_hz"),q=idx("effect.1.eq.bell.q");
    document.values[gain]=1;document.values[frequency]=0;document.values[q]=1;
    InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(document)));processor.prepareToPlay(48000,512);
    // This fixed20Hz/+18dB/Q4 endpoint's independently observed raw pole tail
    // is around11.5s; omitting EQ memory would report only the .15s amp release.
    CHECK(processor.getTailLengthSeconds()>11&&processor.getTailLengthSeconds()<13);
    const double realtime=processor.getTailLengthSeconds();processor.control(758).setValue(1);processor.setNonRealtime(true);
    CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==realtime);
    processor.prepareToPlay(48000,512);CHECK(processor.effectiveRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==21);
    CHECK(processor.getTailLengthSeconds()>11&&processor.getTailLengthSeconds()<13);
    std::cout<<"EQ_REPORTED_TAIL realtime="<<realtime<<" higher="<<processor.getTailLengthSeconds()<<'\n';
    // Revision 2 keeps the prototype's pole, so the same endpoint keeps that tail.
    document.compatibility.effects[0][eqEffectBank]=modernEqAlgorithmRevision;
    InstrumentProcessor modern{airack::testing::isolatedRackRuntime()};CHECK(restore(modern,state(document)));modern.prepareToPlay(48000,512);
    CHECK(modern.getTailLengthSeconds()>11&&modern.getTailLengthSeconds()<13);
    std::cout<<"EQ_REPORTED_TAIL revision=2 realtime="<<modern.getTailLengthSeconds()<<'\n';
}

void filterSelectiveHistoryAndWholeDefaults()
{
    section("filter_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842b802");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::filter&&candidate.sound.compatibility.effects[0][filterEffectBank]==modernFilterAlgorithmRevision);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.filter_fx.resonance");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.filter_fx.cutoff_hz")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==modernFilterAlgorithmRevision);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="filter_fx.cutoff_hz"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][5]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void filterPreparedWrapperAndTail()
{
    section("filter_prepared_full_wrapper_first_live_allocations_and_pole_tail");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::filter;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".filter_fx.";
            physical(full,bank+"mode",float(slot%3));physical(full,bank+"cutoff_hz",float(400+slot*800));
            physical(full,bank+"resonance",float(.7+slot*.5));physical(full,bank+"drive_db",float(2+slot));physical(full,bank+"mix",.8f);}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,5>,8> controls;
        constexpr const char* fields[]{"mode","cutoff_hz","resonance","drive_db","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<5;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".filter_fx."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%5;
            processor.control(controls[block%8][field]).setValue(field==0?float((block/40)%3)*.5f:(block%2?.83f:.17f));
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"FILTER_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind::filter;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    physical(document,"effect.1.filter_fx.cutoff_hz",20);physical(document,"effect.1.filter_fx.resonance",10);
    InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(document)));processor.prepareToPlay(48000,512);
    // This fixed20Hz/Q10 endpoint's independently observed raw pole tail
    // is around10.2s; omitting the dedicated filter memory would report only the .15s amp release.
    CHECK(processor.getTailLengthSeconds()>10&&processor.getTailLengthSeconds()<11);
    const double realtime=processor.getTailLengthSeconds();processor.control(758).setValue(1);processor.setNonRealtime(true);
    CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==realtime);
    processor.prepareToPlay(48000,512);CHECK(processor.effectiveRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==21);
    CHECK(processor.getTailLengthSeconds()>10&&processor.getTailLengthSeconds()<11);
    std::cout<<"FILTER_REPORTED_TAIL realtime="<<realtime<<" higher="<<processor.getTailLengthSeconds()<<'\n';
    // Revision 2 keeps the prototype's pole at four times the rate, so the same endpoint keeps that tail.
    document.compatibility.effects[0][filterEffectBank]=modernFilterAlgorithmRevision;
    InstrumentProcessor modern{airack::testing::isolatedRackRuntime()};CHECK(restore(modern,state(document)));modern.prepareToPlay(48000,512);
    CHECK(modern.getTailLengthSeconds()>10&&modern.getTailLengthSeconds()<11);
    std::cout<<"FILTER_REPORTED_TAIL revision=2 realtime="<<modern.getTailLengthSeconds()<<'\n';
}

void chorusSelectiveHistoryAndWholeDefaults()
{
    section("chorus_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842c802");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::chorus&&candidate.sound.compatibility.effects[0][6]==modernModulatedTapAlgorithmRevision);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.chorus.depth");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.chorus.base_delay_ms")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==modernModulatedTapAlgorithmRevision);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="chorus.base_delay_ms"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][6]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void chorusPreparedWrapperAndTail()
{
    section("chorus_prepared_full_wrapper_first_live_allocations_and_feedback_tail");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::chorus;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".chorus.";
            physical(full,bank+"rate_hz",float(.2+slot*.3));physical(full,bank+"depth",float(.2+slot*.08));
            physical(full,bank+"base_delay_ms",float(14+slot*3));physical(full,bank+"feedback",float(-.3+slot*.08));physical(full,bank+"mix",.8f);}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,5>,8> controls;
        constexpr const char* fields[]{"rate_hz","depth","base_delay_ms","feedback","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<5;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".chorus."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%5;
            processor.control(controls[block%8][field]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"CHORUS_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind::chorus;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    physical(document,"effect.1.chorus.base_delay_ms",40);physical(document,"effect.1.chorus.depth",1);physical(document,"effect.1.chorus.feedback",.8f);
    physical(document,"synth.1.analog.amp.release",.001f);
    InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(document)));processor.prepareToPlay(8000,512);
    // Independent 8k rectangular-bound arithmetic: 402 stored delay frames,
    // 72 feedback repeats plus first pass, one last-output frame per pass,
    // stock .050s support, .001s release and .010s existing transition margin.
    const double realtime=processor.getTailLengthSeconds();CHECK(realtime>3.73&&realtime<3.75);
    processor.control(758).setValue(1);processor.setNonRealtime(true);CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==realtime);
    processor.prepareToPlay(8000,512);const double higher=processor.getTailLengthSeconds();
    CHECK(processor.effectiveRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==21&&higher>3.72&&higher<3.76);
    CHECK(std::abs(higher-(.050+73*803.0/16000+.011+42.0/8000))<2e-6);
    processor.control(758).setValue(0);processor.setNonRealtime(false);CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==higher);
    processor.prepareToPlay(8000,512);CHECK(processor.getTailLengthSeconds()==realtime&&processor.getLatencySamples()==0);
    std::cout<<"CHORUS_REPORTED_TAIL realtime="<<realtime<<" higher="<<higher<<'\n';
}

void frequencyShiftSelectiveHistoryAndWholeDefaults()
{
    section("frequencyShift_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bb01");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::frequencyShift&&candidate.sound.compatibility.effects[0][11]==1);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.frequency_shift.mix");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.frequency_shift.mix")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==1);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="frequency_shift.mix"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][11]=2;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void frequencyShiftPreparedWrapperAndTail()
{
    section("frequencyShift_prepared_full_wrapper_first_live_allocations_and_finite_support");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::frequencyShift;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".frequency_shift.";
            physical(full,bank+"hz",float(slot%2?-7-slot:7+slot));physical(full,bank+"stereo_offset_hz",float(12+slot*4));physical(full,bank+"mix",float(.25+slot*.08));}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,3>,8> controls;
        constexpr const char* fields[]{"hz","stereo_offset_hz","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<3;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".frequency_shift."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%3;
            processor.control(controls[block%8][field]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"FREQUENCY_SHIFT_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    // Prepared wrapper support includes every serial prepared engineering-tail bound.
    for(int quality:{0,1}){SoundDocument d;d.values[758]=float(quality);for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::frequencyShift},d.layout)==RackEditResult::applied);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(d)));processor.setNonRealtime(quality==1);processor.prepareToPlay(8000,512);
        RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));double support=0;for(const auto& effect:patch.effects)support+=detail::frequencyShiftTailSeconds(effect.frequencyShift,quality?16000:8000);
        CHECK(processor.getTailLengthSeconds()>=support&&processor.getLatencySamples()==(quality?21:0));
    }
}

void midSideSplitSelectiveHistoryAndWholeDefaults()
{
    section("midSideSplit_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842be01");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::midSideSplit&&candidate.sound.compatibility.effects[0][14]==1);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.mid_side_split.mix");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.mid_side_split.mix")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==1);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="mid_side_split.mix"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][14]=2;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void midSideSplitPreparedWrapperAndTail()
{
    section("midSideSplit_prepared_full_wrapper_first_live_allocations_and_finite_support");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::midSideSplit;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".mid_side_split.";
            physical(full,bank+"mid_gain_db",-3);physical(full,bank+"mid_mute",0);physical(full,bank+"mid_tone",-.2f);
            physical(full,bank+"side_gain_db",2);physical(full,bank+"side_mute",0);physical(full,bank+"side_tone",.3f);physical(full,bank+"mix",.8f);}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,7>,8> controls;
        constexpr const char* fields[]{"mid_gain_db","mid_mute","mid_tone","side_gain_db","side_mute","side_tone","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<7;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".mid_side_split."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%7;
            processor.control(controls[block%8][field]).setValue(field==1||field==4?float(1-(block/64)%2):block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"MID_SIDE_SPLIT_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 edits=56 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    // Prepared wrapper support includes every serial prepared engineering-tail bound.
    for(int quality:{0,1}){SoundDocument d;d.values[758]=float(quality);for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::midSideSplit},d.layout)==RackEditResult::applied);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(d)));processor.setNonRealtime(quality==1);processor.prepareToPlay(8000,512);
        RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));double support=0;for(std::size_t slot=0;slot<8;++slot)support+=detail::midSideSplitTailSeconds(quality?16000:8000);
        CHECK(processor.getTailLengthSeconds()>=support&&processor.getLatencySamples()==(quality?21:0));
    }
}

void frequencySplitSelectiveHistoryAndWholeDefaults()
{
    section("frequencySplit_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bd02");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::frequencySplit&&candidate.sound.compatibility.effects[0][frequencySplitEffectBank]==modernFrequencySplitAlgorithmRevision);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.frequency_split.mix");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.frequency_split.mix")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==modernFrequencySplitAlgorithmRevision);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="frequency_split.mix"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][13]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void frequencySplitPreparedWrapperAndTail()
{
    section("frequencySplit_prepared_full_wrapper_first_live_allocations_and_finite_support");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::frequencySplit;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".frequency_split.";
            physical(full,bank+"crossover_hz",float(300+131*slot));physical(full,bank+"low_gain_db",-3);physical(full,bank+"low_mute",0);physical(full,bank+"low_tone",-.2f);
            physical(full,bank+"high_gain_db",2);physical(full,bank+"high_mute",0);physical(full,bank+"high_tone",.3f);physical(full,bank+"mix",.8f);}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,8>,8> controls;
        constexpr const char* fields[]{"crossover_hz","low_gain_db","low_mute","low_tone","high_gain_db","high_mute","high_tone","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<8;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".frequency_split."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%8;
            processor.control(controls[block%8][field]).setValue(field==2||field==5?float(1-(block/64)%2):block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"FREQUENCY_SPLIT_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 edits=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    // Prepared wrapper support includes every serial prepared engineering-tail bound.
    for(int quality:{0,1})for(AlgorithmRevision revision:{legacyAlgorithmRevision,modernFrequencySplitAlgorithmRevision}){
        SoundDocument d;d.values[758]=float(quality);for(auto& banks:d.compatibility.effects)banks[frequencySplitEffectBank]=revision;
        for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::frequencySplit},d.layout)==RackEditResult::applied);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(d)));processor.setNonRealtime(quality==1);processor.prepareToPlay(8000,512);
        RoutedPatch patch;CHECK(makeRoutedPatch(d,patch)&&patch.effects[0].algorithmRevision==revision);double support=0;
        for(std::size_t slot=0;slot<8;++slot)support+=detail::frequencySplitTailSeconds(quality?16000:8000,revision);
        CHECK(processor.getTailLengthSeconds()>=support&&processor.getLatencySamples()==(quality?21:0));
    }
}

void convolutionSelectiveHistoryAndWholeDefaults()
{
    section("convolution_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bc01");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::convolution&&candidate.sound.compatibility.effects[0][12]==1);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.convolution.mix");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.convolution.mix")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==1);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="convolution.mix"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][12]=2;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void convolutionPreparedWrapperAndTail()
{
    section("convolution_prepared_full_wrapper_first_live_allocations_and_finite_support");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::convolution;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".convolution.";
            physical(full,bank+"ir",float(slot%2));physical(full,bank+"predelay_ms",float(slot*17));physical(full,bank+"wet_trim_db",float(-6+slot));physical(full,bank+"mix",float(.25+slot*.08));}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,4>,8> controls;
        constexpr const char* fields[]{"ir","predelay_ms","wet_trim_db","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<4;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".convolution."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%4;
            processor.control(controls[block%8][field]).setValue(field==0?float(1-block%2):block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"CONVOLUTION_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    // Prepared wrapper support includes every serial prepared engineering-tail bound.
    for(int quality:{0,1}){SoundDocument d;d.values[758]=float(quality);for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::convolution},d.layout)==RackEditResult::applied);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(d)));processor.setNonRealtime(quality==1);processor.prepareToPlay(8000,512);
        RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));double support=0;for(const auto& effect:patch.effects)support+=detail::convolutionTailSeconds(effect.convolution,quality?16000:8000);
        CHECK(processor.getTailLengthSeconds()>=support&&processor.getLatencySamples()==(quality?21:0));
    }
}

void dimensionSelectiveHistoryAndWholeDefaults()
{
    section("dimension_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842aa02");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::dimension&&candidate.sound.compatibility.effects[0][10]==modernModulatedTapAlgorithmRevision);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.dimension.width");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.dimension.width")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==modernModulatedTapAlgorithmRevision);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="dimension.width"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][10]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void dimensionPreparedWrapperAndTail()
{
    section("dimension_prepared_full_wrapper_first_live_allocations_and_finite_support");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::dimension;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".dimension.";
            physical(full,bank+"depth",float(.2+slot*.1));physical(full,bank+"rate_hz",float(.1+slot*.2));
            physical(full,bank+"width",float(.3+slot*.2));physical(full,bank+"mix",float(.25+slot*.08));}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,4>,8> controls;
        constexpr const char* fields[]{"depth","rate_hz","width","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<4;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".dimension."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%4;
            processor.control(controls[block%8][field]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"DIMENSION_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    // Prepared wrapper support includes every serial finite-history stage.
    for(int quality:{0,1}){SoundDocument d;d.values[758]=float(quality);for(std::uint8_t slot=0;slot<8;++slot)CHECK(editRackLayout(d.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::dimension},d.layout)==RackEditResult::applied);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(d)));processor.setNonRealtime(quality==1);processor.prepareToPlay(8000,512);
        RoutedPatch patch;CHECK(makeRoutedPatch(d,patch));double support=0;for(const auto& effect:patch.effects)support+=detail::dimensionTailSeconds(effect.dimension,quality?16000:8000);
        CHECK(processor.getTailLengthSeconds()>=support&&processor.getLatencySamples()==(quality?21:0));
    }
}

void stereoSelectiveHistoryAndWholeDefaults()
{
    section("stereo_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842f901");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::stereo&&candidate.sound.compatibility.effects[0][9]==1);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.stereo.width");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.stereo.width")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==1);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="stereo.width"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][9]=2;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void stereoPreparedWrapperAndTail()
{
    section("stereo_prepared_full_wrapper_first_live_allocations_and_zero_stage_tail");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto fullFixture = airack::testing::fixtureValue([&]{return fullDocument();}); auto& full = *fullFixture;full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::stereo;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".stereo.";
            physical(full,bank+"gain_db",float(-12+slot*2));physical(full,bank+"pan",float(-.7+slot*.2));
            physical(full,bank+"width",float(.3+slot*.2));physical(full,bank+"channel_mode",float(slot%3));}
        auto processorFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& processor = *processorFixture;CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,4>,8> controls;
        constexpr const char* fields[]{"gain_db","pan","width","channel_mode"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<4;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".stereo."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%4;
            processor.control(controls[block%8][field]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"STEREO_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    // The memoryless utility contributes no audio support. Compare the
    // actual prepared wrapper against the same source and a downstream delay,
    // retaining source transitions and the declared Higher42-frame history.
    auto dryFixture = std::make_unique<SoundDocument>(); auto& dry = *dryFixture;physical(dry,"synth.1.analog.amp.release",.001f);
    auto utilityFixture = airack::testing::fixtureValue([&]{return dry;}); auto& utility = *utilityFixture;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind::stereo;
    CHECK(editRackLayout(utility.layout,add,utility.layout)==RackEditResult::applied);
    auto delayedFixture = airack::testing::fixtureValue([&]{return dry;}); auto& delayed = *delayedFixture; auto serialFixture = airack::testing::fixtureValue([&]{return utility;}); auto& serial = *serialFixture;add.effect=EffectKind::delay;
    CHECK(editRackLayout(delayed.layout,add,delayed.layout)==RackEditResult::applied);
    CHECK(editRackLayout(serial.layout,add,serial.layout)==RackEditResult::applied);
    physical(delayed,"effect.1.delay.seconds",.05f);physical(delayed,"effect.1.delay.feedback",.4f);
    physical(serial,"effect.2.delay.seconds",.05f);physical(serial,"effect.2.delay.feedback",.4f);
    for(int quality:{0,1}){
        for(auto* sound:{&dry,&utility,&delayed,&serial})sound->values[758]=float(quality);
        auto baselineFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& baseline = *baselineFixture; auto placedFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& placed = *placedFixture; auto delayOnlyFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& delayOnly = *delayOnlyFixture; auto withDelayFixture = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& withDelay = *withDelayFixture;
        CHECK(restore(baseline,state(dry))&&restore(placed,state(utility))&&restore(delayOnly,state(delayed))&&restore(withDelay,state(serial)));
        for(auto* processor:{&baseline,&placed,&delayOnly,&withDelay}){processor->setNonRealtime(quality==1);processor->prepareToPlay(8000,512);}
        CHECK(placed.getTailLengthSeconds()==baseline.getTailLengthSeconds());
        CHECK(withDelay.getTailLengthSeconds()==delayOnly.getTailLengthSeconds());
        CHECK(placed.getLatencySamples()==(quality?21:0)&&placed.getTailLengthSeconds()>0);
        std::cout<<"STEREO_REPORTED_TAIL quality="<<quality<<" stage=0 source="<<baseline.getTailLengthSeconds()
                 <<" source_plus_delay="<<withDelay.getTailLengthSeconds()<<'\n';
    }
}

void flangerSelectiveHistoryAndWholeDefaults()
{
    section("flanger_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842e902");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::flanger&&candidate.sound.compatibility.effects[0][8]==modernModulatedTapAlgorithmRevision);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.flanger.depth");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.flanger.base_delay_ms")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==modernModulatedTapAlgorithmRevision);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="flanger.base_delay_ms"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][8]=3;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void flangerPreparedWrapperAndTail()
{
    section("flanger_prepared_full_wrapper_first_live_allocations_and_feedback_tail");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::flanger;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".flanger.";
            physical(full,bank+"rate_hz",float(.2+slot*.3));physical(full,bank+"depth",float(.2+slot*.08));
            physical(full,bank+"base_delay_ms",float(1+slot));physical(full,bank+"feedback",float(-.3+slot*.08));physical(full,bank+"mix",.8f);}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,5>,8> controls;
        constexpr const char* fields[]{"rate_hz","depth","base_delay_ms","feedback","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<5;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".flanger."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%5;
            processor.control(controls[block%8][field]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"FLANGER_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind::flanger;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    physical(document,"effect.1.flanger.base_delay_ms",1);physical(document,"effect.1.flanger.depth",1);physical(document,"effect.1.flanger.feedback",.8f);
    physical(document,"synth.1.analog.amp.release",.001f);
    InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(document)));processor.prepareToPlay(8000,512);
    // Independent global80-repeat linear-delay loop support at the actual
    // float .8 endpoint: .005+80*(ceil(.014*Fs)+3)/Fs. Add the unchanged
    // .001s release/.010s transition and prepared Higher42-frame FIR.
    const double realtime=processor.getTailLengthSeconds();CHECK(std::abs(realtime-1.166)<2e-6);
    processor.control(758).setValue(1);processor.setNonRealtime(true);CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==realtime);
    processor.prepareToPlay(8000,512);const double higher=processor.getTailLengthSeconds();
    CHECK(processor.effectiveRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==21);
    CHECK(std::abs(higher-(1.14+.011+42.0/8000))<2e-6);
    processor.control(758).setValue(0);processor.setNonRealtime(false);CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==higher);
    processor.prepareToPlay(8000,512);CHECK(processor.getTailLengthSeconds()==realtime&&processor.getLatencySamples()==0);
    std::cout<<"FLANGER_REPORTED_TAIL realtime="<<realtime<<" higher="<<higher<<'\n';
}

void phaserSelectiveHistoryAndWholeDefaults()
{
    section("phaser_actual_ai_swap_selective_history_whole_embedded_defaults");
#ifndef NDEBUG
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842d901");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    for(int quality:{0,1}){
        SoundDocument original;original.values[758]=float(quality);original.compatibility.effects[7][2]=2;
        RackDesign legacy;legacy.role=RackRole::fx;legacy.name="Old drive";legacy.units={{0,NativeModule::drive,{{0,"drive.mix",.25f}}}};
        legacy.panel.sections={"Blend"};legacy.panel.controls={{"Mix",0,0,"drive.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
        RackOperation place;place.design=legacy;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
        RackDifference difference;SoundDocument placed;CHECK(makeRackCandidate(original,place,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(placed)));ProjectDocument before;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(before,revision));
        auto candidate=before;RackOperation swap;swap.kind=RackOperationKind::swap;swap.design=*found;swap.selected=place.newInstance;
        swap.destination=place.destination;swap.allocation=place.allocation;swap.allowSlotReuse=true;
        CHECK(makeRackCandidate(before.sound,swap,SoundEditOrigin::aiCandidate,candidate.sound,difference)==RackCandidateStatus::ready);
        CHECK(candidate.sound.layout.effects[0].kind==EffectKind::phaser&&candidate.sound.compatibility.effects[0][7]==1);
        CHECK(InstrumentStructuralTestAccess::aiEdit(processor,before,candidate)&&document(processor)==candidate.sound);
        const auto unrelated=idx("effect.8.phaser.depth");processor.control(unrelated).setValue(.37f);processor.control(756).setValue(.3f);
        auto expectedBefore=before,expectedAfter=candidate;
        for(auto* expected:{&expectedBefore,&expectedAfter}){expected->sound.values[unrelated]=.37f;expected->sound.values[756]=.3f;}
        CHECK(processor.undoSoundEdit()&&document(processor)==expectedBefore.sound);
        CHECK(processor.redoSoundEdit()&&document(processor)==expectedAfter.sound);
        CHECK(document(processor).personalities->instances[0].design==*found);
        processor.control(idx("effect.1.phaser.centre_hz")).setValue(.72f);const auto live=document(processor);RackDesign captured;
        CHECK(processor.captureRackDesign(place.newInstance,captured)&&document(processor)==live);
        CHECK(captured.id==found->id&&captured.revision==found->revision+1&&captured.units[0].algorithmRevision==1);
        CHECK(std::any_of(captured.units[0].defaults.begin(),captured.units[0].defaults.end(),[](const auto& v){return v.key=="phaser.centre_hz"&&v.normalized==.72f;}));
        CHECK(live.personalities->instances[0].design==*found);
        ProjectDocument current;CHECK(processor.tryProjectSnapshot(current,revision));auto invalid=current;invalid.sound.compatibility.effects[7][7]=2;
        CHECK(!InstrumentStructuralTestAccess::aiEdit(processor,current,invalid)&&document(processor)==current.sound);
    }
#endif
}
void phaserPreparedWrapperAndTail()
{
    section("phaser_prepared_full_wrapper_first_live_allocations_and_feedback_tail");
    for(const auto geometry:std::array<std::pair<int,int>,2>{{{48000,128},{96000,512}}})for(int quality:{0,1}){
        auto full=fullDocument();full.values[758]=float(quality);for(auto& effect:full.layout.effects)effect.kind=EffectKind::phaser;
        for(int slot=0;slot<8;++slot){const auto bank="effect."+juce::String(slot+1)+".phaser.";
            physical(full,bank+"rate_hz",float(.2+slot*.3));physical(full,bank+"depth",float(.2+slot*.08));
            physical(full,bank+"centre_hz",float(400+slot*700));physical(full,bank+"feedback",float(-.3+slot*.08));physical(full,bank+"mix",.8f);}
        InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(full)));processor.setNonRealtime(quality==1);processor.prepareToPlay(geometry.first,geometry.second);
        CHECK(processor.effectiveRenderQuality()==static_cast<RenderQuality>(quality)&&processor.getLatencySamples()==(quality?21:0));
        juce::AudioBuffer<float> audio(2,geometry.second);juce::MidiBuffer held,released,stop,empty;
        for(auto* midi:{&held,&released,&stop,&empty})midi->ensureSize(4096);
        for(int n=48;n<64;++n)held.addEvent(juce::MidiMessage::noteOn(1,n,(juce::uint8)100),n-48);
        released.addEvent(juce::MidiMessage::allNotesOff(1),17);stop.addEvent(juce::MidiMessage::midiStop(),31);
        std::array<std::array<std::size_t,5>,8> controls;
        constexpr const char* fields[]{"rate_hz","depth","centre_hz","feedback","mix"};
        for(int slot=0;slot<8;++slot)for(int field=0;field<5;++field)controls[slot][field]=idx("effect."+juce::String(slot+1)+".phaser."+fields[field]);
#if defined(_MSC_VER) && defined(_DEBUG)
        bool finite=true,nonzero=false;auto previous=_CrtSetAllocHook(allocationHook);allocations=0;audioFrees=0;tracking=true;
        processor.processBlock(audio,held);
        for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}
        for(int block=0;block<96;++block){const int field=(block/8)%5;
            processor.control(controls[block%8][field]).setValue(block%2?.83f:.17f);
            processor.processBlock(audio,block==32?released:block==64?stop:empty);
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<geometry.second;++frame){const auto value=audio.getSample(ch,frame);finite&=std::isfinite(value);nonzero|=value!=0;}}
        tracking=false;_CrtSetAllocHook(previous);CHECK(finite&&nonzero&&allocations==0&&audioFrees==0);
        std::cout<<"PHASER_WRAPPER rate="<<geometry.first<<" block="<<geometry.second<<" quality="<<quality<<" first_plus_live=97 layers=4 notes=16 effects=8 routes=64 allocations="<<allocations<<" frees="<<audioFrees<<'\n';
#else
        CHECK(false);
#endif
    }
    SoundDocument document;RackEdit add;add.kind=RackEditKind::addEffect;add.effect=EffectKind::phaser;
    CHECK(editRackLayout(document.layout,add,document.layout)==RackEditResult::applied);
    physical(document,"effect.1.phaser.centre_hz",20);physical(document,"effect.1.phaser.depth",1);physical(document,"effect.1.phaser.feedback",.8f);
    physical(document,"synth.1.analog.amp.release",.001f);
    InstrumentProcessor processor{airack::testing::isolatedRackRuntime()};CHECK(restore(processor,state(document)));processor.prepareToPlay(8000,512);
    // Independent complete six-AP/one-frame loop Cauchy arithmetic at the
    // actual float .8 endpoint: 205893 frames + four-frame hold + .055s,
    // plus .001s release and the existing .010s transition margin.
    const double realtime=processor.getTailLengthSeconds();CHECK(realtime>25.80&&realtime<25.81);
    processor.control(758).setValue(1);processor.setNonRealtime(true);CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==realtime);
    processor.prepareToPlay(8000,512);const double higher=processor.getTailLengthSeconds();
    CHECK(processor.effectiveRenderQuality()==RenderQuality::higher&&processor.getLatencySamples()==21&&higher>26.46&&higher<26.47);
    CHECK(std::abs(higher-(.055+422277.0/16000+.011+42.0/8000))<2e-6);
    processor.control(758).setValue(0);processor.setNonRealtime(false);CHECK(processor.renderQualityPending()&&processor.getTailLengthSeconds()==higher);
    processor.prepareToPlay(8000,512);CHECK(processor.getTailLengthSeconds()==realtime&&processor.getLatencySamples()==0);
    CHECK(higher>realtime+42./8000);
    std::cout<<"PHASER_REPORTED_TAIL realtime="<<realtime<<" higher="<<higher<<'\n';
}

void modernReverbReportedTail()
{
    section("modern_reverb_reported_tail_includes_allpass_ringout_and_quality_fir");
    SoundDocument shortest;CHECK(editRackLayout(shortest.layout,{RackEditKind::addEffect,0,masterChain,0,EffectKind::reverb},shortest.layout)==RackEditResult::applied);
    shortest.compatibility.effects[0][2]=2;physical(shortest,"synth.1.analog.amp.release",.001f);
    physical(shortest,"effect.1.reverb.size",0);physical(shortest,"effect.1.reverb.decay_seconds",.1f);
    physical(shortest,"effect.1.reverb.damping",0);physical(shortest,"effect.1.reverb.predelay_ms",0);physical(shortest,"effect.1.reverb.mix",1);
    InstrumentProcessor realtime,higher,legacy;CHECK(restore(realtime,state(shortest)));
    auto high=shortest;high.values[758]=1;CHECK(restore(higher,state(high)));higher.setNonRealtime(true);
    auto old=shortest;old.compatibility.effects[0][2]=1;CHECK(restore(legacy,state(old)));
    realtime.prepareToPlay(48000,128);higher.prepareToPlay(48000,128);legacy.prepareToPlay(48000,128);
    const auto tail=realtime.getTailLengthSeconds(),highTail=higher.getTailLengthSeconds(),oldTail=legacy.getTailLengthSeconds();
    // Independently bounded8 geometry: .987590483s FDN/filter + .77715s
    // allpass feedback support + .001s release + .01s transition support.
    // Omitting the allpass ringout reports only ~.9986s and fails this bound.
    CHECK(std::isfinite(tail)&&tail>=1.76&&tail<=1.80);CHECK(tail>oldTail+1.3);
    CHECK(higher.getLatencySamples()==21&&higher.effectiveRenderQuality()==RenderQuality::higher);
    CHECK(std::isfinite(highTail)&&highTail>=tail+double(higher.getLatencySamples())/48000);
    // The un-compensated host tail includes all 42 FIR-support frames, not
    // merely its 21-frame group delay or a pending requested quality mode.
    CHECK(std::abs(highTail-tail-2.0*higher.getLatencySamples()/48000)<1e-12);
    higher.getParameters()[758]->setValueNotifyingHost(0);higher.setNonRealtime(false);
    CHECK(higher.renderQualityPending()&&higher.getTailLengthSeconds()==highTail);
    juce::AudioBuffer<float> pending(2,128);pending.clear();juce::MidiBuffer none;higher.processBlock(pending,none);
    CHECK(higher.effectiveRenderQuality()==RenderQuality::higher&&higher.getTailLengthSeconds()==highTail);
    higher.prepareToPlay(48000,128);
    CHECK(higher.effectiveRenderQuality()==RenderQuality::realtime&&higher.getLatencySamples()==0&&higher.getTailLengthSeconds()==tail);
#ifndef NDEBUG
    InstrumentProcessor contended{airack::testing::isolatedRackRuntime()};CHECK(restore(contended,state(shortest)));contended.prepareToPlay(48000,128);
    const auto fallbackBase=contended.getTailLengthSeconds();
    contended.getParameters()[758]->setValueNotifyingHost(1);contended.setNonRealtime(true);
    CHECK(InstrumentStructuralTestAccess::prepareWithUnavailableSnapshot(contended,48000,128));
    CHECK(contended.effectiveRenderQuality()==RenderQuality::higher&&contended.getLatencySamples()==21);
    CHECK(std::abs(contended.getTailLengthSeconds()-fallbackBase-42.0/48000)<1e-12);
    contended.prepareToPlay(48000,128); // Establish the complete higher tail via an available snapshot.
    contended.getParameters()[758]->setValueNotifyingHost(0);
    CHECK(InstrumentStructuralTestAccess::prepareWithUnavailableSnapshot(contended,48000,128));
    CHECK(contended.effectiveRenderQuality()==RenderQuality::realtime&&contended.getLatencySamples()==0);
    CHECK(contended.getTailLengthSeconds()==fallbackBase);contended.releaseResources();
#endif
    auto longest=shortest;physical(longest,"effect.1.reverb.size",1);physical(longest,"effect.1.reverb.decay_seconds",20);
    physical(longest,"effect.1.reverb.damping",1);physical(longest,"effect.1.reverb.predelay_ms",250);
    CHECK(restore(realtime,state(longest)));realtime.prepareToPlay(48000,128);const auto oneTail=realtime.getTailLengthSeconds();
    //1217 recirculations give183.521810s including allpass/predelay/filter.
    CHECK(std::isfinite(oneTail)&&oneTail>=182&&oneTail<=185); //22s is an active observation, not quiescence.
    for(std::uint8_t slot=1;slot<8;++slot){CHECK(editRackLayout(longest.layout,{RackEditKind::addEffect,slot,masterChain,slot,EffectKind::reverb},longest.layout)==RackEditResult::applied);
        longest.compatibility.effects[slot][2]=2;const auto prefix="effect."+juce::String(slot+1)+".";
        physical(longest,prefix+"reverb.size",1);physical(longest,prefix+"reverb.decay_seconds",20);physical(longest,prefix+"reverb.damping",1);
        physical(longest,prefix+"reverb.predelay_ms",250);physical(longest,prefix+"reverb.mix",1);}
    CHECK(restore(realtime,state(longest)));realtime.prepareToPlay(48000,128);const auto serialTail=realtime.getTailLengthSeconds();
    CHECK(std::isfinite(serialTail)&&serialTail>oneTail*7.9);
    std::cout<<"MODERN_TAIL shortest="<<tail<<" legacy="<<oldTail<<" higher="<<highTail<<" longest_one="<<oneTail<<" longest_serial8="<<serialTail<<" engineering_estimate_only=1\n";
    realtime.releaseResources();higher.releaseResources();legacy.releaseResources();
}
int main(int argc, char** argv)
{
    std::cout.setf(std::ios::unitbuf);
    if(argc==2&&juce::String(argv[1])=="--main-sync-retirement-discard"){
        DiscardPhaseProbe probe;
        {
            juce::ScopedJuceInitialiser_GUI gui;
            new DiscardGlobalsMarker;std::cout<<"DISCARD_PHASE marker_registered_before_editor"<<std::endl;
            queuedRackOutlivesProcessor(false);
            new DiscardGlobalsBoundary;
        }
        CHECK(probe.stopping&&probe.globalsStarted&&probe.watched==2&&probe.deleted==2);
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    if(argc==2&&juce::String(argv[1])=="--saturation-release"){
        saturationReleaseRouting();saturationAuditionAuthority();saturationRegisteredControlAudio();saturationChargedRecall();saturationSameInstanceQuality();saturationEmptyMidiTail();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--main-sync-accessibility-retirement"){
        sliderAccessibilityRetirement();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&(juce::String(argv[1])=="--main-sync-row-retirement"||juce::String(argv[1])=="--main-sync-programmer-retirement"||juce::String(argv[1])=="--main-sync-processor-retirement")){
        const auto flag=juce::String(argv[1]);const auto preparedSkins=prepareTestMakerSkins();
        makerCallbackOutlivesProcessor(preparedSkins,false,flag=="--main-sync-row-retirement"?RetirementOrigin::row:flag=="--main-sync-programmer-retirement"?RetirementOrigin::programmer:RetirementOrigin::processor);
        if(flag=="--main-sync-processor-retirement"){directHeldRetirement();activeRetirementRejectedPost();}
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&(juce::String(argv[1])=="--main-sync-idle-row-lifetime"||juce::String(argv[1])=="--main-sync-idle-row-discard")){
        idleRowGestureOutlivesProcessor(juce::String(argv[1])=="--main-sync-idle-row-lifetime");
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&(juce::String(argv[1])=="--main-sync-maker-lifetime"||juce::String(argv[1])=="--main-sync-maker-discard")){
        const auto preparedSkins=prepareTestMakerSkins();makerCallbackOutlivesProcessor(preparedSkins,juce::String(argv[1])=="--main-sync-maker-lifetime");
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&(juce::String(argv[1])=="--main-sync-callback-lifetime"||juce::String(argv[1])=="--main-sync-callback-discard")){
        callbackEditorOutlivesProcessor(juce::String(argv[1])=="--main-sync-callback-lifetime");
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--main-sync-retirement-lifetime"){
        queuedRackOutlivesProcessor(true);
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--main-sync-gesture-boundary"){
        mainSyncGestureBoundary();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--owned-recall-lifecycle") {
        (void)parameterRegistry();modernReverbReportedTail();listenerAndRenderConcurrency();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--owned-persistence") {
        (void)parameterRegistry();rackPersistenceAndCapture();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--source-authority") {
        sourceSelectorAuthority();sourceSelectorCallbacks();sourceAiCaptureAuthority();sourceWholeBankTransactions();personalityInvalidEngineRepair();
        std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--preset-menu-lifetime-check") {
        menuServiceLifetime();
        auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(blankInstance(*p));
        std::unique_ptr<juce::AudioProcessorEditor> e(p->createEditor());e->setVisible(true);dispatch();
        presetMenuCancellation(*p,e);
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--complete-editor-check") {
        std::cout.setf(std::ios::unitbuf);
        const auto focusedEditorStart=std::chrono::steady_clock::now();
        std::cout<<"COMPLETE_EDITOR_FOCUSED begin quality_effect_prelude_excluded=1\n";
        completeEditorCheck(juce::String{});
        std::cout<<"COMPLETE_EDITOR_FOCUSED completed elapsed_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-focusedEditorStart).count()<<"\n";
        std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures==0?0:1;
    }
    if(argc==2&&juce::String(argv[1])=="--output-check") {
        std::cout.setf(std::ios::unitbuf);
        const auto focusedOutputStart=std::chrono::steady_clock::now();
        std::cout<<"OUTPUT_CHECK_FOCUSED begin quality_effect_prelude_excluded=1\n";
        outputWidgetsAndRecall();
        std::cout<<"OUTPUT_CHECK_FOCUSED completed elapsed_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-focusedOutputStart).count()<<"\n";
        std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures==0?0:1;
    }
    preparedQualityPolicy<InstrumentProcessor>();qualityWrapperAllocations();modernReverbSelectiveAiHistory();modernReverbWrapperCallbacks();modernReverbReportedTail();eqSelectiveHistoryAndWholeDefaults();eqPreparedWrapperAndTail();chorusSelectiveHistoryAndWholeDefaults();chorusPreparedWrapperAndTail();phaserSelectiveHistoryAndWholeDefaults();phaserPreparedWrapperAndTail();flangerSelectiveHistoryAndWholeDefaults();flangerPreparedWrapperAndTail();midSideSplitSelectiveHistoryAndWholeDefaults();midSideSplitPreparedWrapperAndTail();frequencySplitSelectiveHistoryAndWholeDefaults();frequencySplitPreparedWrapperAndTail();convolutionSelectiveHistoryAndWholeDefaults();convolutionPreparedWrapperAndTail();frequencyShiftSelectiveHistoryAndWholeDefaults();frequencyShiftPreparedWrapperAndTail();dimensionSelectiveHistoryAndWholeDefaults();dimensionPreparedWrapperAndTail();stereoSelectiveHistoryAndWholeDefaults();stereoPreparedWrapperAndTail();filterSelectiveHistoryAndWholeDefaults();filterPreparedWrapperAndTail();
    std::cout.setf(std::ios::unitbuf);
    if(argc==2&&juce::String(argv[1])=="--whole-findings"){wholeFindingTransactions();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--delay-reported-tail"){delayTapeReportedTail();delayStereoReportedTail();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    juce::String prefix;
    if(argc==3&&juce::String(argv[1])=="--personality-packet"){personalityPacket(juce::File(argv[2]));return failures?1:0;}
    if((argc==2||argc==3)&&juce::String(argv[1])=="--state-load-error-check") {
        stateLoadErrorCheck(argc==3?juce::String(argv[2]):juce::String());
        std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures==0?0:1;
    }
    if(argc==2&&(juce::String(argv[1])=="--ui-convergence-diagnostic"||juce::String(argv[1])=="--ui-convergence-fixed"))
    {
        uiConvergenceDiagnostic(juce::String(argv[1])=="--ui-convergence-fixed");
        std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";
        return failures==0?0:1;
    }
    if (argc == 3 && juce::String(argv[1]) == "--editor-snapshot") prefix = argv[2];
    else if (argc != 1) { std::cerr << "Usage: RackSynthInstrumentTests [--editor-snapshot OUTPUT_PREFIX | --ui-convergence-diagnostic | --ui-convergence-fixed]\n"; return 2; }
    // CTest runs the complete editor check apart (--complete-editor-check) under the desktop lock: its preset chooser is
    // a window on the desktop.
    const bool editorApart=std::getenv("AIRACK_INSTRUMENT_EDITOR_APART")!=nullptr;
    newInstanceEmpty(prefix);freshInstanceKernels();kernelAndModePublication();stateLoadErrorCheck({}); outputWidgetsAndRecall(); if(!editorApart)completeEditorCheck(prefix); boundedBatchGestureCheck(); mainSyncGestureBoundary(); enabledWidgetReentrantCoupledGestureCheck(); coherentResetTokenCheck();
    wholeFindingTransactions();invalidAutomationRecoveryHistory(); recoveryEqualsIntentAndBankRepair(); disabledSoloInsertTail();
    routedProcessorAndMigration(); delayTapeReportedTail(); delayStereoReportedTail(); unassignedDrumReportedTail(); completePublicationAndCallbacks(); nativeTransactionsAndStaleEdits(); undoLocksAndPresets(); presetNames();
    personality_render_oracles(); personality_callback_ownership(); registry(); banksAndInvalid(); concurrentRecall(); editorCheck(prefix); retiredRackOutlivesProcessor(); queuedRackOutlivesProcessor(true); callbackEditorOutlivesProcessor(true); {const auto preparedSkins=prepareTestMakerSkins();makerCallbackOutlivesProcessor(preparedSkins,true);} idleRowGestureOutlivesProcessor(true); recallNotificationConvergence(); pendingRecallDestruction();personalityPaths();personalityAudio();personalityInvalidEngineRepair();personalityPolling();personalityPublicationWindows();personalityDiscreteAndGroupTransactions();sourceSelectorAuthority();sourceSelectorCallbacks();sourceAiCaptureAuthority();sourceWholeBankTransactions();rackPersistenceAndCapture();rackNativeCapacity();postedClickBehindSlowMessage();
    // Stub guards keep RED about behavior, never null dereferences.
    InstrumentProcessor probe{airack::testing::isolatedRackRuntime()};
    if (probe.getParameters().size() == parameterCount) { audioAndLifecycle(); invalidEditsPreserveAudio(); earlyReleasePublication(); envelopePreparation(); listenerAndRenderConcurrency(); allocationCheck(); }
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}


