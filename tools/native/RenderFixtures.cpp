#include "../../tests/native/ComfyHttpFixture.h"
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <future>
#include <set>
#include <charconv>
#include <cstring>
#include "../../src/ai/StrictJson.h"
#include "UserReach.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include "PatternIO.h"
#include "InstrumentProcessor.h"
#include "RackExamples.h"
#include "FactorySounds.h"
#include "../../tests/native/FrozenRackExamples.h"
#include "../../src/dsp/ConvolutionBankData.h"
#include "RackPersonalityFixtures.h"
#include "AiListeningBriefs.h"
#include <juce_dsp/juce_dsp.h>
#include "../../src/drums/DrumBundle.h"
#include <string_view>
#if JUCE_WINDOWS
#include <windows.h>
#include <psapi.h>
#endif
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <vector>
#include <thread>

namespace
{
namespace fs = std::filesystem;
struct Arguments { juce::File plugin, output; bool gain = false, legacyGain = false, personalities = false, soundQuality = false; juce::File baseline; airack::RenderQuality quality=airack::RenderQuality::realtime; bool drumProduct=false, drumPendingUnload=false; juce::File drumState; juce::MemoryBlock drumStateBytes; juce::String drumCase; bool factoryAnalog=false,factoryDigital=false,reverbOnly=false; airack::AlgorithmRevision reverbRevision=1; bool reverbProbesOnly=false,factoryReverb=false,coreFxProbes=false,factoryCoreFx=false,factoryEq=false,factoryFilter=false,factoryChorus=false,factoryPhaser=false,factoryFlanger=false,factoryStereo=false,factoryDimension=false,factoryFrequencyShift=false,factoryConvolution=false,factoryFrequencySplit=false,factoryMidSideSplit=false,wavetableWarmth=false; };
struct Event { int sample; juce::MidiMessage message; };
struct Measures { double peak = 0, rms = 0, tailPeak = 0; };
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
bool isAirInstrument(const juce::PluginDescription& d)
{
    return d.isInstrument&&d.pluginFormatName=="VST3"&&d.manufacturerName=="AI Rack Synth"
        &&(d.name=="air"||d.name=="AI Rack Synth");
}
int identitySelfTest()
{
    juce::PluginDescription d;d.name="air";d.isInstrument=true;d.pluginFormatName="VST3";d.manufacturerName="AI Rack Synth";
    require(isAirInstrument(d),"Current display name rejected");d.name="AI Rack Synth";
    require(isAirInstrument(d),"Legacy display name rejected");d.name="Other instrument";
    require(!isAirInstrument(d),"Foreign name accepted");d.name="air";d.isInstrument=false;
    require(!isAirInstrument(d),"Effect accepted");d.isInstrument=true;d.manufacturerName="Other vendor";
    require(!isAirInstrument(d),"Foreign vendor accepted");d.manufacturerName="AI Rack Synth";d.pluginFormatName="AudioUnit";
    require(!isAirInstrument(d),"Foreign format accepted");std::cout<<"Instrument identity: 6 checks passed\n";return 0;
}

juce::File binaryIn(const juce::File& bundle)
{
    return bundle.getChildFile("Contents/x86_64-win/" + bundle.getFileName());
}

Arguments parseDrumProductArguments(juce::StringArray);
Arguments parseArguments(juce::StringArray args)
{
    if(args.size()>0&&args[0]=="--drum-product")return parseDrumProductArguments(std::move(args));
    const bool personalities=args.size()>0&&args[0]=="--rack-personalities";
    const bool soundQuality=args.size()>0&&args[0]=="--sound-quality";
    const bool legacyGain=args.size()>0&&args[0]=="--compare-legacy-gain";
    const bool gain = args.size() > 0 && (args[0] == "--measure-gain"||legacyGain);
    if (gain||personalities||soundQuality) args.remove(0);
    const bool factoryAnalog=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-analog";
    const bool factoryDigital=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-digital";
    const bool factoryReverb=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-reverb";
    const bool coreFxProbes=soundQuality&&args.size()>0&&args[args.size()-1]=="--core-fx-probes";
    const bool factoryEq=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-eq";
    const bool factoryFilter=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-filter";
    const bool factoryMidSideSplit=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-mid-side-split";
    const bool factoryFrequencySplit=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-frequency-split";
    const bool factoryConvolution=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-convolution";
    const bool factoryFrequencyShift=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-frequency-shift";
    const bool factoryDimension=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-dimension";
    const bool factoryStereo=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-stereo";
    const bool factoryFlanger=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-flanger";
    const bool factoryPhaser=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-phaser";
    const bool factoryChorus=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-chorus";
    const bool factoryCoreFx=soundQuality&&args.size()>0&&args[args.size()-1]=="--factory-core-fx";
    const bool wavetableWarmth=soundQuality&&args.size()>0&&args[args.size()-1]=="--wavetable-warmth";
    const bool reverbProbesOnly=soundQuality&&args.size()>0&&args[args.size()-1]=="--reverb-probes-only";
    const bool reverbOnly=reverbProbesOnly||(soundQuality&&args.size()>0&&args[args.size()-1]=="--reverb-only");
    if(factoryAnalog||factoryDigital||factoryReverb||reverbOnly||coreFxProbes||factoryCoreFx||factoryEq||factoryFilter||factoryChorus||factoryPhaser||factoryFlanger||factoryStereo||factoryDimension||factoryFrequencyShift||factoryConvolution||factoryFrequencySplit||factoryMidSideSplit||wavetableWarmth)args.remove(args.size()-1);
    airack::AlgorithmRevision reverbRevision=1;
    if(reverbOnly&&args.size()>=6&&args[args.size()-2]=="--algorithm") {
        require(args[args.size()-1]=="1"||args[args.size()-1]=="2","Unknown reverb evidence algorithm");
        reverbRevision=args[args.size()-1]=="2"?2:1;
        require(airack::supportsEffectAlgorithmRevision(2,reverbRevision),"Reverb evidence algorithm not implemented");
        args.removeRange(args.size()-2,2);
    }
    auto quality=airack::RenderQuality::realtime;
    if(soundQuality && args.size()==6 && args[4]=="--quality")
    {
        require(args[5]=="realtime"||args[5]=="higher","Unknown render quality");
        quality=args[5]=="higher"?airack::RenderQuality::higher:airack::RenderQuality::realtime;
        args.removeRange(4,2);
    }
    juce::File baseline;
    if(personalities&&args.size()==6&&args[4]=="--baseline") {
        require(juce::File::isAbsolutePath(args[5]),"Baseline manifest must be absolute");baseline=juce::File(args[5]);
        require(baseline.existsAsFile()&&baseline.getSize()>0&&baseline.getSize()<=4*1024*1024,"Baseline manifest missing or outside bounded size");
        args.removeRange(4,2);
    }
    require(args.size() == 4 && args[0] == "--plugin" && args[2] == "--output",
        "Usage: RackSynthRender [--measure-gain | --compare-legacy-gain | --rack-personalities | --sound-quality] --plugin ABSOLUTE_VST3_BUNDLE --output ABSOLUTE_WORKSPACE_DIRECTORY [--baseline ABSOLUTE_FROZEN_TASK1_MANIFEST for --rack-personalities only; --quality realtime|higher and one of --factory-analog|--factory-digital|--factory-reverb|--factory-core-fx|--factory-eq|--factory-frequency-split|--factory-mid-side-split|--core-fx-probes|--reverb-only|--wavetable-warmth for --sound-quality only; --algorithm1|2 immediately before --reverb-only selects its implemented reverb revision]; or --self-test");
    require(juce::File::isAbsolutePath(args[1]) && juce::File::isAbsolutePath(args[3]), "Both paths must be absolute");
    Arguments result {juce::File(args[1]), juce::File(args[3]), gain, legacyGain, personalities, soundQuality};
    result.baseline=baseline;
    result.quality=quality;
    result.factoryAnalog=factoryAnalog;
    result.factoryDigital=factoryDigital;
    result.factoryReverb=factoryReverb;
    result.coreFxProbes=coreFxProbes;
    result.factoryCoreFx=factoryCoreFx;result.factoryEq=factoryEq;result.factoryFilter=factoryFilter;result.factoryChorus=factoryChorus;result.factoryPhaser=factoryPhaser;result.factoryFlanger=factoryFlanger;result.factoryStereo=factoryStereo;result.factoryDimension=factoryDimension;result.factoryFrequencyShift=factoryFrequencyShift;result.factoryConvolution=factoryConvolution;result.factoryFrequencySplit=factoryFrequencySplit;result.factoryMidSideSplit=factoryMidSideSplit;result.wavetableWarmth=wavetableWarmth;
    result.reverbOnly=reverbOnly;
    result.reverbProbesOnly=reverbProbesOnly;
    result.reverbRevision=reverbRevision;
    require(result.plugin.isDirectory() && result.plugin.hasFileExtension("vst3") && binaryIn(result.plugin).existsAsFile(),
        "Plugin must be an existing Windows x64 VST3 bundle");
    // Resolve junctions/symlinks too: lexical prefix checks alone allow workspace escape.
    auto canonical = [](const juce::File& file) { return fs::weakly_canonical(fs::path(file.getFullPathName().toWideCharPointer())); };
    const auto workspace = canonical(juce::File(RACKSYNTH_WORKSPACE));
    if(baseline.existsAsFile()){const auto relative=canonical(baseline).lexically_relative(workspace);require(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="..","Baseline manifest escapes workspace");}
    const auto destination = canonical(result.output);
    const auto relative = destination.lexically_relative(workspace);
    require(!relative.empty() && relative != "." && !relative.is_absolute() && *relative.begin() != "..",
        "Output must be a directory strictly inside this workspace");
    require(!personalities||!result.output.exists(),"Personality output must be absent");
    require(!soundQuality||(!result.output.exists()&&!fs::exists(fs::symlink_status(fs::path(result.output.getFullPathName().toWideCharPointer())))),"Sound-quality output must be absent, including links");
    require(!result.output.exists() || (result.output.isDirectory() && fs::is_empty(destination)), "Output directory must be absent or empty");
    return result;
}

std::vector<Event> phrase(int sampleRate, bool overlapping = false)
{
    require(sampleRate > 0 && sampleRate % 4 == 0, "Unsupported phrase sample rate");
    std::vector<Event> events;
    constexpr int notes[] {48, 52, 55, 60};
    for (int i = 0; i < 4; ++i)
    {
        events.push_back({overlapping?i*sampleRate/8:i*sampleRate/2, juce::MidiMessage::noteOn(1, notes[i], static_cast<juce::uint8>(100))});
        events.push_back({overlapping?(i+4)*sampleRate/8:i*sampleRate/2+sampleRate/4, juce::MidiMessage::noteOff(1, notes[i])});
    }
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.sample<b.sample;});
    return events;
}

Measures measure(const juce::AudioBuffer<float>& audio, int tailStart)
{
    require(audio.getNumChannels() == 2 && audio.getNumSamples() > 0 && tailStart >= 0 && tailStart < audio.getNumSamples(),
        "Measurement requires nonempty stereo audio and a tail window");
    Measures result;
    double squares = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const double sample = audio.getSample(ch, i);
            require(std::isfinite(sample), "Nonfinite render rejected");
            result.peak = std::max(result.peak, std::abs(sample));
            squares += sample * sample;
            if (i >= tailStart) result.tailPeak = std::max(result.tailPeak, std::abs(sample));
        }
    result.rms = std::sqrt(squares / (2.0 * audio.getNumSamples()));
    require(result.rms > 1e-8, "Silent or negligible render rejected");
    require(result.peak < 1, "Clipped render rejected");
    require(result.tailPeak <= 1e-7, "Unsettled final 0.25 second tail rejected");
    return result;
}

double difference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    require(a.getNumChannels() == 2 && a.getNumSamples() > 0 && a.getNumChannels() == b.getNumChannels()
        && a.getNumSamples() == b.getNumSamples(), "Comparison dimensions differ");
    double maximum = 0;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const double left = a.getSample(ch, i), right = b.getSample(ch, i);
            require(std::isfinite(left) && std::isfinite(right), "Nonfinite comparison rejected");
            maximum = std::max(maximum, std::abs(left - right));
        }
    return maximum;
}

struct Interval { int begin, end; };
struct WindowMeasures
{
    double peak = 0, rms = 0, dc = 0;
    juce::int64 samples = 0, excursions = 0;
    std::array<double, 2> channelPeak {}, channelRms {}, channelDc {};
    std::array<juce::int64, 2> channelExcursions {};
};
WindowMeasures measureWindow(const juce::AudioBuffer<float>& audio, const std::vector<Interval>& intervals)
{
    require(audio.getNumChannels() == 2 && audio.getNumSamples() > 0 && !intervals.empty(), "Window requires nonempty stereo audio and intervals");
    int previousEnd = 0, frames = 0;
    for (const auto& interval : intervals)
    {
        require(interval.begin >= previousEnd && interval.end > interval.begin && interval.end <= audio.getNumSamples(), "Invalid or overlapping window intervals");
        previousEnd = interval.end; frames += interval.end - interval.begin;
    }
    WindowMeasures m; m.samples = 2 * static_cast<juce::int64>(frames);
    double totalSquares = 0, totalSum = 0;
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        double squares = 0, sum = 0;
        for (const auto& interval : intervals)
            for (int i = interval.begin; i < interval.end; ++i)
            {
                const double value = audio.getSample(static_cast<int>(ch), i);
                require(std::isfinite(value), "Nonfinite gain measurement rejected");
                m.channelPeak[ch] = std::max(m.channelPeak[ch], std::abs(value));
                squares += value * value; sum += value;
                if (std::abs(value) >= 1) ++m.channelExcursions[ch];
            }
        m.channelRms[ch] = std::sqrt(squares / frames); m.channelDc[ch] = sum / frames;
        m.peak = std::max(m.peak, m.channelPeak[ch]); m.excursions += m.channelExcursions[ch];
        totalSquares += squares; totalSum += sum;
    }
    m.rms = std::sqrt(totalSquares / static_cast<double>(m.samples)); m.dc = totalSum / static_cast<double>(m.samples);
    return m;
}
juce::var dbfs(double amplitude) { return amplitude == 0 ? juce::var() : juce::var(20 * std::log10(amplitude)); }

airack::ParameterValues fixtureValues(bool wavetable)
{
    auto values = airack::defaultParameterValues();
    values[756]=.4f;values[757]=0; // Original fixtures explicitly retain neutral output.
    const auto catalog = airack::parameterRegistry();
    auto set = [&](const juce::String& id, float physical) {
        const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& d) { return d.id == id; });
        require(found != catalog.end(), "Fixture control missing from catalog");
        juce::NormalisableRange<float> range(found->minimum, found->maximum, 0.0f, found->skew);
        values[static_cast<std::size_t>(found - catalog.begin())] = range.convertTo0to1(physical);
    };
    set("synth.1.engine", wavetable ? 1.0f : 0.0f);
    const juce::String bank = wavetable ? "synth.1.wavetable." : "synth.1.analog.";
    set(bank + "amp.attack", .002f);
    set(bank + "amp.decay", .12f);
    set(bank + "amp.sustain", 0);
    set(bank + "amp.release", .1f);
    set(bank + "filter.cutoff", wavetable ? 3500.0f : 1800.0f);
    if (wavetable)
        for (int osc : {1, 2}) set(bank + "osc." + juce::String(osc) + ".position", .8f);
    // Explicit in every active/inactive bank so future catalog defaults cannot introduce noise.
    for (int slot = 1; slot <= 4; ++slot)
        for (const auto* engine : {"analog", "wavetable"})
            set("synth." + juce::String(slot) + "." + engine + ".noise", 0);
    require(airack::validateParameterValues(values), "Invalid fixture values");
    return values;
}

juce::MemoryBlock hostEnvelope(const juce::MemoryBlock& owned)
{
    juce::XmlElement xml("VST3PluginState");
    xml.createNewChildElement("IComponent")->addTextElement(owned.toBase64Encoding());
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(xml, state);
    return state;
}

void verifyReturnedProject(const juce::MemoryBlock&, const airack::ProjectDocument&);
void verifyReturnedState(const juce::MemoryBlock& state, const airack::SoundDocument& requested,
                         std::size_t ownedSize)
{
    require(ownedSize == airack::currentSoundStateSize, "Returned sound expectation must use the current format size");
    airack::ProjectDocument expected; expected.sound = requested;
    verifyReturnedProject(state, expected);
}

// Build-side failure attribution only. No thread priority/affinity or plugin state
// changes: CPU/cycle/residency reads bracket the original processBlock timer.
struct DimensionCostTrace
{
    struct Row { juce::int64 wallNs,cycles,cpu100ns; unsigned beforeCpu,afterCpu; };
    std::vector<Row> rows;
    void prepare(std::size_t count) { rows.reserve(count); }
    template<class Fn> juce::int64 measure(Fn&& call)
    {
        #if JUCE_WINDOWS
        FILETIME creation{},exit{},kernelBefore{},userBefore{},kernelAfter{},userAfter{};
        const auto thread=GetCurrentThread(); ULONG64 cyclesBefore=0,cyclesAfter=0;
        require(GetThreadTimes(thread,&creation,&exit,&kernelBefore,&userBefore)!=0,"Diagnostic initial thread time failed");
        require(QueryThreadCycleTime(thread,&cyclesBefore)!=0,"Diagnostic initial cycle query failed");
        const auto cpuBefore=GetCurrentProcessorNumber();
        const auto begin=std::chrono::steady_clock::now(); call();
        const auto wall=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
        const auto cpuAfter=GetCurrentProcessorNumber();
        require(QueryThreadCycleTime(thread,&cyclesAfter)!=0,"Diagnostic final cycle query failed");
        require(GetThreadTimes(thread,&creation,&exit,&kernelAfter,&userAfter)!=0,"Diagnostic final thread time failed");
        const auto ticks=[](const FILETIME& t){return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
        const auto cpu=ticks(kernelAfter)+ticks(userAfter)-ticks(kernelBefore)-ticks(userBefore);
        rows.push_back({wall,juce::int64(cyclesAfter-cyclesBefore),juce::int64(cpu),cpuBefore,cpuAfter});return wall;
        #else
        require(false,"Dimension cost attribution requires Windows thread accounting");return 0;
        #endif
    }
    juce::var json() const
    {
        auto result=juce::var(new juce::DynamicObject);juce::Array<juce::var> observations;
        for(const auto& row:rows){auto item=juce::var(new juce::DynamicObject);auto* p=item.getDynamicObject();
            p->setProperty("wall_ns",row.wallNs);p->setProperty("thread_cycles",row.cycles);p->setProperty("thread_cpu_100ns",row.cpu100ns);
            p->setProperty("processor_before",int(row.beforeCpu));p->setProperty("processor_after",int(row.afterCpu));observations.add(item);}
        result.getDynamicObject()->setProperty("callbacks",observations);
        result.getDynamicObject()->setProperty("qualification","Unchanged processBlock wall boundary. Thread cycles include bracket-query overhead; GetThreadTimes accounting is coarse and useful only in aggregate. Endpoint processor IDs do not prove no migration within a callback. No clock/frequency, preemption, or CPU-cost claim follows from one counter alone.");
        #if JUCE_WINDOWS
        result.getDynamicObject()->setProperty("process_id",int(GetCurrentProcessId()));result.getDynamicObject()->setProperty("thread_id",int(GetCurrentThreadId()));
        result.getDynamicObject()->setProperty("thread_priority",GetThreadPriority(GetCurrentThread()));result.getDynamicObject()->setProperty("priority_class",int(GetPriorityClass(GetCurrentProcess())));
        #endif
        return result;
    }
};

struct Rendered
{
    juce::AudioBuffer<float> audio;
    juce::MemoryBlock hostState;
    int hostedParameters = 0;
    int latencySamples = 0;
    double tailSeconds = 0;
    std::vector<juce::int64> callbackNs;
};

Rendered render(juce::VST3PluginFormat& format, const juce::PluginDescription& description,
                int sampleRate, int blockSize, const juce::MemoryBlock& state,
                const airack::SoundDocument& document, std::size_t ownedSize, bool tempoPresent = false, int automateSlot = -1,
                const std::vector<Event>* suppliedEvents = nullptr, int suppliedSamples = 0, bool measureCallbacks=false, DimensionCostTrace* diagnostic=nullptr, double tempoBpm=120,
                std::span<const airack::ParameterGestureValue> afterPrepareEdits={},int* restartedLatency=nullptr,bool externalInput=false)
{
    require(airack::validateSoundDocument(document), "Invalid render document");
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(), "Host lifecycle must run on the message thread");
    juce::String error;
    auto instance = format.createInstanceFromDescription(description, sampleRate, blockSize, error);
    require(instance != nullptr, "Actual VST3 instance creation failed");
    require(instance->getTotalNumInputChannels() == 0 && instance->getTotalNumOutputChannels() == 2
        && instance->acceptsMidi(), "Loaded plugin is not the expected stereo MIDI instrument");
    if(externalInput){auto buses=instance->getBusesLayout();buses.inputBuses.set(0,juce::AudioChannelSet::stereo());
        require(instance->setBusesLayout(buses)&&instance->getTotalNumInputChannels()==2,"Hosted spectral sidechain admission failed");}
    instance->setNonRealtime(true);
    instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    Rendered result;
    instance->getStateInformation(result.hostState);
    verifyReturnedState(result.hostState, document, ownedSize);
    result.hostedParameters = instance->getParameters().size();
    // Verify all owned named VST3 identities, including the 209 historical IDs.
    // JUCE adds its own bypass (and may expose MIDI mappings), so total count
    // is reported separately from this exact owned catalog.
    std::array<juce::AudioProcessorParameter*,airack::parameterCount> hosted {};
    std::map<juce::String,juce::AudioProcessorParameter*> namedParameters;
    for(auto* parameter:instance->getParameters())
        if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter))
            require(namedParameters.emplace(named->getParameterID(),parameter).second,"Duplicate hosted identity");
    for(std::size_t n=0;n<hosted.size();++n)
    {
        const auto& d=airack::parameterRegistry()[n];
        const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(d.id,true));
        if(const auto found=namedParameters.find(id);found!=namedParameters.end())hosted[n]=found->second;
        require(hosted[n]!=nullptr,"Missing stable named hosted identity");
        require(hosted[n]->isAutomatable()==d.automatable,"Hosted automation flag changed meaning");
        require(hosted[n]->getName(256)==d.name&&hosted[n]->getLabel()==d.unit,"Hosted named descriptor changed meaning");
        const juce::NormalisableRange<float> range(d.minimum,d.maximum,0,d.skew);
        require(std::abs(hosted[n]->getDefaultValue()-range.convertTo0to1(d.defaultValue))<1e-6f,"Hosted default changed meaning");
    }
    struct Tempo final : juce::AudioPlayHead {
        bool present=false; juce::int64 sample=0; double bpm=120;
        juce::Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setTimeInSamples(sample);if(present)p.setBpm(bpm);return p;}
    } playhead;
    playhead.present=tempoPresent;playhead.bpm=tempoBpm;instance->setPlayHead(&playhead);
    auto expected=document;
    if(automateSlot>=0)
    {
        const auto id="synth."+juce::String(automateSlot+1)+".enabled";
        const auto registry=airack::parameterRegistry();
        const auto it=std::find_if(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;});
        const auto n=static_cast<std::size_t>(it-registry.begin());
        hosted[n]->beginChangeGesture();hosted[n]->setValueNotifyingHost(1);hosted[n]->endChangeGesture();
        expected.values[n]=1;
    }
    instance->prepareToPlay(sampleRate, blockSize);
    result.latencySamples=instance->getLatencySamples();
    result.tailSeconds=instance->getTailLengthSeconds();
    require(std::isfinite(result.tailSeconds)&&result.tailSeconds>=0,"Hosted tail invalid");
    for(const auto& edit:afterPrepareEdits){require(edit.index<hosted.size(),"Post-prepare host edit outside registry");
        hosted[edit.index]->beginChangeGesture();hosted[edit.index]->setValueNotifyingHost(edit.value);hosted[edit.index]->endChangeGesture();expected.values[edit.index]=edit.value;
    }
    const int samples = suppliedEvents != nullptr ? suppliedSamples : sampleRate * 5 / 2;
    require(samples > 0, "Render duration must be positive");
    result.audio.setSize(2, samples); result.audio.clear();
    if(diagnostic){diagnostic->prepare(std::size_t((samples+blockSize-1)/blockSize));result.callbackNs.reserve(std::size_t((samples+blockSize-1)/blockSize));}
    const auto events = suppliedEvents != nullptr ? *suppliedEvents : phrase(sampleRate,document.values[9]>=.5f);
    std::size_t nextEvent = 0;
    juce::AudioBuffer<float> block(2, blockSize);
    juce::MidiBuffer midi;
    for (int offset = 0; offset < samples; offset += blockSize)
    {
        const int count = std::min(blockSize, samples - offset);
        playhead.sample=offset;
        block.setSize(2, count, false, false, true); block.clear(); midi.clear();
        while (nextEvent < events.size() && events[nextEvent].sample < offset + count)
        {
            const auto& event = events[nextEvent++];
            midi.addEvent(event.message, event.sample - offset);
        }
        if(diagnostic)result.callbackNs.push_back(diagnostic->measure([&]{instance->processBlock(block,midi);}));
        else if(measureCallbacks){const auto begin=std::chrono::steady_clock::now();instance->processBlock(block,midi);result.callbackNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());}
        else instance->processBlock(block, midi);
        for (int ch = 0; ch < 2; ++ch) result.audio.copyFrom(ch, offset, block, ch, 0, count);
    }
    require(nextEvent == events.size(), "Phrase was not completely delivered");
    instance->getStateInformation(result.hostState);
    require(instance->getLatencySamples()==result.latencySamples,"Host latency changed while processing");
    verifyReturnedState(result.hostState, expected, ownedSize);
    instance->releaseResources();
    if(restartedLatency){instance->prepareToPlay(sampleRate,blockSize);*restartedLatency=instance->getLatencySamples();instance->releaseResources();}
    instance->setPlayHead(nullptr);
    // Unique instance destruction is serialized on this same message thread; no editor is created.
    return result;
}

std::unique_ptr<juce::FileOutputStream> newStream(const juce::File& file)
{
    require(!file.exists(), "Artifact already exists; refusing overwrite");
    auto stream = file.createOutputStream();
    require(stream != nullptr && stream->openedOk(), "Cannot create artifact stream");
    return stream;
}

void writeBlock(const juce::File& file, const juce::MemoryBlock& block)
{
    auto stream = newStream(file);
    require(stream->write(block.getData(), block.getSize()), "Artifact write failed");
    stream->flush(); require(stream->getStatus().wasOk(), "Artifact flush failed");
}

juce::String hash(const juce::File& file)
{
    require(file.existsAsFile(), "Cannot hash missing artifact");
    return juce::SHA256(file).toHexString();
}

juce::var identity(const juce::File& file)
{
    auto* result = new juce::DynamicObject;
    result->setProperty("path", file.getFullPathName());
    result->setProperty("sha256", hash(file));
    result->setProperty("bytes", file.getSize());
    return juce::var(result);
}

void writeMidiEvents(const juce::File& file, const std::vector<Event>& events,int ppq=480,int tempoMicroseconds=500000)
{
    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::tempoMetaEvent(tempoMicroseconds));
    for (const auto& event : events) // 960 ticks/sec at 480 PPQ, 120 BPM.
    {
        auto message = event.message; message.setTimeStamp(event.sample); track.addEvent(message);
    }
    juce::MidiFile midi; midi.setTicksPerQuarterNote(ppq); midi.addTrack(track);
    auto stream = newStream(file);
    require(midi.writeTo(*stream, 0), "MIDI export failed");
    stream->flush(); require(stream->getStatus().wasOk(), "MIDI flush failed"); stream.reset();
    auto input = file.createInputStream(); juce::MidiFile readback;
    require(input != nullptr && readback.readFrom(*input) && readback.getTimeFormat() == ppq
        && readback.getNumTracks() == 1, "MIDI readback failed");
    const auto* readTrack = readback.getTrack(0);
    int notes = 0, tempo = 0;
    for (int i = 0; i < readTrack->getNumEvents(); ++i)
    {
        const auto& message = readTrack->getEventPointer(i)->message;
        if (message.isTempoMetaEvent()) { require(message.getTempoSecondsPerQuarterNote() == double(tempoMicroseconds)/1000000, "MIDI tempo mismatch"); ++tempo; }
        if (message.isNoteOnOrOff())
        {
            require(static_cast<std::size_t>(notes) < events.size(), "Extra MIDI note event");
            const auto expected = events[static_cast<std::size_t>(notes++)];
            require(message.getTimeStamp() == expected.sample && message.getNoteNumber() == expected.message.getNoteNumber()
                && message.isNoteOn() == expected.message.isNoteOn()
                && (!message.isNoteOn() || message.getVelocity() == expected.message.getVelocity()), "MIDI phrase readback mismatch");
        }
    }
    require(static_cast<std::size_t>(notes) == events.size() && tempo == 1, "Incomplete MIDI phrase");
}

void writeMidi(const juce::File& file, bool overlapping = false) { writeMidiEvents(file, phrase(960, overlapping)); }

void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, int sampleRate)
{
    std::unique_ptr<juce::OutputStream> stream = newStream(file);
    juce::WavAudioFormat format;
    auto options = juce::AudioFormatWriterOptions().withSampleRate(sampleRate).withNumChannels(2).withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = format.createWriterFor(stream, options);
    require(writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()), "Float WAV export failed");
    require(writer->flush(), "WAV flush failed"); writer.reset();
    auto input = file.createInputStream();
    require(input != nullptr, "Cannot reopen WAV");
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(input.release(), true));
    require(reader != nullptr && reader->usesFloatingPointData && reader->bitsPerSample == 32 && reader->numChannels == 2
        && reader->sampleRate == sampleRate && reader->lengthInSamples == audio.getNumSamples(), "WAV format readback mismatch");
    juce::AudioBuffer<float> readback(2, audio.getNumSamples());
    require(reader->read(&readback, 0, readback.getNumSamples(), 0, true, true) && difference(audio, readback) == 0,
        "WAV samples changed during export");
}

int renderSpectralIntegration(const Arguments& args)
{
    require(!args.output.exists(),"Spectral replay output must be fresh");
    require(args.output.createDirectory().wasOk(),"Cannot create spectral replay output");
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> types;format.findAllTypesForFile(types,args.plugin.getFullPathName());
    require(types.size()==1&&isAirInstrument(*types[0]),"Unexpected spectral VST3 identity");
    const auto moduleBefore=hash(binaryIn(args.plugin));
    auto report=juce::var(new juce::DynamicObject);auto* properties=report.getDynamicObject();
    properties->setProperty("module",identity(binaryIn(args.plugin)));properties->setProperty("renderer",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    properties->setProperty("scope","Actual VST3 flat routed reconstruction, opaque state, partition, automation flags, pending mode/restart and per-band host edits. Capacity/listening/ordinary32WAVs remain separate.");
    juce::Array<juce::var> rows;
    const auto encode=[](const airack::SoundDocument& sound){juce::MemoryBlock bytes;require(airack::encodeSoundState(sound,bytes),"Spectral host sound invalid");return hostEnvelope(bytes);};
    const auto exact=[](const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b){
        if(a.getNumSamples()!=b.getNumSamples())return false;
        for(int ch=0;ch<2;++ch)if(std::memcmp(a.getReadPointer(ch),b.getReadPointer(ch),std::size_t(a.getNumSamples())*sizeof(float)))return false;
        return true;
    };
    const auto difference=[](const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b,int delay){
        require(a.getNumSamples()==b.getNumSamples()&&a.getNumSamples()>delay,"Spectral comparison geometry invalid");double error=0;
        for(int ch=0;ch<2;++ch)for(int n=0;n<a.getNumSamples();++n){const auto actual=a.getSample(ch,n);require(std::isfinite(actual),"Nonfinite spectral VST3 output");
            const auto expected=n>=delay?b.getSample(ch,n-delay):0.f;error=std::max(error,std::abs(double(actual)-expected));}
        return error;
    };
    for(const int rate:{48000,96000})for(const auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher})for(const int stages:{1,8}){
        airack::SoundDocument plain;plain.values[0]=.35f;plain.values[14]=.2f;plain.values[28]=plain.values[33]=0;plain.values[30]=plain.values[35]=.5f;
        const auto& gain=airack::parameterRegistry()[756];plain.values[756]=(0-gain.minimum)/(gain.maximum-gain.minimum);plain.values[757]=0;plain.values[758]=quality==airack::RenderQuality::higher?1.f:0.f;
        auto spectral=plain;
        for(int slot=0;slot<stages;++slot){airack::RackEdit edit;edit.kind=airack::RackEditKind::addEffect;edit.slot=std::uint8_t(slot);edit.effect=airack::EffectKind::dynamicEq;edit.destination=airack::masterChain;edit.position=std::uint8_t(slot);
            require(airack::editRackLayout(spectral.layout,edit,spectral.layout)==airack::RackEditResult::applied,"Spectral replay route invalid");
            spectral.values[airack::dynamicEqSpectralParameterStart+std::size_t(slot)*25]=1;
        }
        const int delay=rate/48000*5120*stages,length=delay+8192;
        const std::vector<Event> events{{0,juce::MidiMessage::noteOn(1,69,.8f)}};
        const auto original=render(format,*types[0],rate,64,encode(plain),plain,airack::currentSoundStateSize,false,-1,&events,length);
        const auto flat=render(format,*types[0],rate,64,encode(spectral),spectral,airack::currentSoundStateSize,false,-1,&events,length);
        require(flat.latencySamples==original.latencySamples+delay,"Actual spectral host latency wrong");
        require(flat.tailSeconds>=2.*delay/rate,"Actual spectral host tail omits prepared delay");
        const auto error=difference(flat.audio,original.audio,delay);require(error<=2.e-6,"Actual spectral flat route failed reconstruction");
        const auto restored=render(format,*types[0],rate,512,flat.hostState,spectral,airack::currentSoundStateSize,false,-1,&events,length);
        require(restored.latencySamples==flat.latencySamples&&exact(restored.audio,flat.audio),"Opaque spectral recall/partition changed audio bits");
        auto bypass=spectral;for(int slot=0;slot<stages;++slot)bypass.values[572+std::size_t(slot)*23]=1;
        const auto dry=render(format,*types[0],rate,512,encode(bypass),bypass,airack::currentSoundStateSize,false,-1,&events,length);
        require(dry.latencySamples==flat.latencySamples&&difference(dry.audio,original.audio,delay)==0,"Spectral bypass lost alignment");
        auto row=juce::var(new juce::DynamicObject);auto* p=row.getDynamicObject();p->setProperty("rate",rate);p->setProperty("quality",int(quality));p->setProperty("stages",stages);p->setProperty("latency_samples",flat.latencySamples);p->setProperty("maximum_flat_error",error);p->setProperty("opaque_partition_bit_exact",true);p->setProperty("bypass_exact",true);rows.add(row);
        if(stages==1){
            const auto sidechain=render(format,*types[0],rate,512,encode(spectral),spectral,airack::currentSoundStateSize,false,-1,&events,length,false,nullptr,120.,{},nullptr,true);
            require(sidechain.latencySamples==flat.latencySamples&&exact(sidechain.audio,flat.audio),"Spectral internal detector changed with sidechain bus enabled");
            p->setProperty("sidechain_lifecycle_bit_exact",true);p->setProperty("tail_seconds",flat.tailSeconds);
            const auto mode=airack::dynamicEqSpectralParameterStart;const std::array<airack::ParameterGestureValue,1> modeEdit{{{mode,0}}};int restarted=-1;
            const auto pending=render(format,*types[0],rate,64,encode(spectral),spectral,airack::currentSoundStateSize,false,-1,&events,length,false,nullptr,120.,modeEdit,&restarted);
            require(pending.latencySamples==flat.latencySamples&&restarted==original.latencySamples,"Actual host mode did not latch across restart");
            auto dynamic=spectral;dynamic.values[mode+1]=1;dynamic.values[airack::dynamicEqParameterStart]=1;
            const auto physical=[&](std::size_t index,float value){const auto& d=airack::parameterRegistry()[index];dynamic.values[index]=juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(value);};
            physical(airack::dynamicEqParameterStart+3,440);physical(airack::dynamicEqParameterStart+7,-6);physical(airack::dynamicEqParameterStart+8,-50);
            auto edited=dynamic;edited.values[mode+2]=.75f;edited.values[mode+3]=.25f;
            const std::array<airack::ParameterGestureValue,2> bandEdits{{{mode+2,.75f},{mode+3,.25f}}};
            const auto automated=render(format,*types[0],rate,64,encode(dynamic),dynamic,airack::currentSoundStateSize,false,-1,&events,length,false,nullptr,120.,bandEdits);
            const auto saved=render(format,*types[0],rate,512,encode(edited),edited,airack::currentSoundStateSize,false,-1,&events,length);
            require(exact(automated.audio,saved.audio),"Per-band actual host edits differ from saved audio bits");
            require(difference(automated.audio,flat.audio,0)>1.e-6,"Enabled spectral VST3 case did not alter audible signal");
            p->setProperty("pending_restart_verified",true);p->setProperty("band_host_edits_verified",true);
        }
        std::cout<<"SPECTRAL_VST3 rate="<<rate<<" quality="<<int(quality)<<" stages="<<stages<<" latency="<<flat.latencySamples<<" error="<<error<<'\n';
    }
    require(hash(binaryIn(args.plugin))==moduleBefore,"Spectral VST3 module changed during replay");
    properties->setProperty("rows",rows);properties->setProperty("success",true);
    const auto json=juce::JSON::toString(report);writeBlock(args.output.getChildFile("report.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));
    std::cout<<"SUCCESS: eight spectral VST3 geometry rows, all hosted identities/flags, opaque recall, bypass and pending mode/host edits\n";return 0;
}
int renderSpectralListening(const Arguments& args)
{
    require(!args.output.exists()&&args.output.createDirectory().wasOk(),"Listening output must be fresh");
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> types;format.findAllTypesForFile(types,args.plugin.getFullPathName());
    require(types.size()==1&&isAirInstrument(*types[0]),"Unexpected listening VST3 identity");
    auto report=juce::var(new juce::DynamicObject);auto* properties=report.getDynamicObject();properties->setProperty("module_before",identity(binaryIn(args.plugin)));
    properties->setProperty("scope","Actual VST3 musical listening pairs. Explicit zero oscillator noise in all banks. Fixed output gain, no loudness normalization. Latency trimmed equally; raw renders and complete requested states retained.");juce::Array<juce::var> rows;
    constexpr int rate=48000,frames=rate*4;
    const auto physical=[](airack::SoundDocument& d,const juce::String& id,float value){const auto registry=airack::parameterRegistry();const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return p.id==id;});require(found!=registry.end(),"Unknown spectral listening control");d.values[std::size_t(found-registry.begin())]=juce::NormalisableRange<float>(found->minimum,found->maximum,0,found->skew).convertTo0to1(value);};
    for(int scene=0;scene<3;++scene){airack::SoundDocument plain;plain.values=fixtureValues(false);
        physical(plain,"output.gain_db",30);physical(plain,"global.master_level",.5f);physical(plain,"synth.1.level",.35f);physical(plain,"synth.1.analog.amp.decay",.65f);physical(plain,"synth.1.analog.amp.sustain",.35f);physical(plain,"synth.1.analog.filter.cutoff",6500);
        auto spectral=plain;airack::RackEdit add;add.kind=airack::RackEditKind::addEffect;add.slot=0;add.effect=airack::EffectKind::dynamicEq;add.destination=airack::masterChain;add.position=0;require(airack::editRackLayout(spectral.layout,add,spectral.layout)==airack::RackEditResult::applied,"Listening route invalid");
        const auto prefix=juce::String("effect.1.dynamic_eq.");physical(spectral,prefix+"processing_mode",1);physical(spectral,prefix+"band.1.active",1);physical(spectral,prefix+"band.1.spectral_enabled",1);
        physical(spectral,prefix+"band.1.frequency_hz",scene==0?130.f:700.f);physical(spectral,prefix+"band.1.q",scene==0?5.f:.5f);physical(spectral,prefix+"band.1.range_db",scene==2?9.f:-12.f);physical(spectral,prefix+"band.1.threshold_db",-60);physical(spectral,prefix+"band.1.attack_ms",15);physical(spectral,prefix+"band.1.release_ms",180);physical(spectral,prefix+"band.1.spectral_density",scene==1?.5f:.8f);physical(spectral,prefix+"band.1.spectral_tilt_db_oct",scene==2?3.f:0.f);
        std::vector<Event> events;for(int n=0;n<6;++n){const auto note=(scene==0?48:60)+std::array<int,6>{0,7,3,10,7,0}[std::size_t(n)];const int at=n*rate/2;events.push_back({at,juce::MidiMessage::noteOn(1,note,juce::uint8(90+n%3*10))});events.push_back({at+rate*2/5,juce::MidiMessage::noteOff(1,note)});}
        const juce::String stem=scene==0?"bass-precision":scene==1?"selective-density":"upward-tilt";auto row=juce::var(new juce::DynamicObject);row.getDynamicObject()->setProperty("name",stem);juce::AudioBuffer<float> reference,processed;
        for(bool enabled:{false,true}){const auto& wanted=enabled?spectral:plain;juce::MemoryBlock bytes;require(airack::encodeSoundState(wanted,bytes),"Listening state encode failed");writeBlock(args.output.getChildFile(stem+(enabled?"-spectral.soundstate":"-reference.soundstate")),bytes);
            const auto rendered=render(format,*types[0],rate,512,hostEnvelope(bytes),wanted,airack::currentSoundStateSize,false,-1,&events,frames+5120);const int delay=enabled?5120:0;require(rendered.latencySamples==delay,"Listening latency invalid");
            juce::AudioBuffer<float> trimmed(2,frames);for(int ch=0;ch<2;++ch)trimmed.copyFrom(ch,0,rendered.audio,ch,delay,frames);const auto measures=measureWindow(trimmed,{{0,frames}});require(measures.rms>1.e-5&&measures.peak<.99,"Listening signal silent or clipped");
            writeWav(args.output.getChildFile(stem+(enabled?"-spectral-raw.wav":"-reference-raw.wav")),rendered.audio,rate);writeWav(args.output.getChildFile(stem+(enabled?"-spectral.wav":"-reference.wav")),trimmed,rate);
            auto item=juce::var(new juce::DynamicObject);item.getDynamicObject()->setProperty("peak",measures.peak);item.getDynamicObject()->setProperty("rms",measures.rms);item.getDynamicObject()->setProperty("latency_trimmed_samples",delay);row.getDynamicObject()->setProperty(enabled?"spectral":"reference",item);(enabled?processed:reference)=std::move(trimmed);
        }
        const auto delta=difference(reference,processed);require(delta>1.e-5,"Spectral listening pair has no effect");row.getDynamicObject()->setProperty("maximum_difference",delta);rows.add(row);
        juce::AudioBuffer<float> pair(2,frames*2+rate/2);pair.clear();for(int ch=0;ch<2;++ch){pair.copyFrom(ch,0,reference,ch,0,frames);pair.copyFrom(ch,frames+rate/2,processed,ch,0,frames);}writeWav(args.output.getChildFile(stem+"-reference-then-spectral.wav"),pair,rate);
        std::cout<<"SPECTRAL_LISTEN "<<stem<<" delta="<<delta<<std::endl;
    }
    properties->setProperty("rows",rows);properties->setProperty("module_after",identity(binaryIn(args.plugin)));require(properties->getProperty("module_before")["sha256"]==properties->getProperty("module_after")["sha256"],"Listening module drift");properties->setProperty("success",true);const auto json=juce::JSON::toString(report);writeBlock(args.output.getChildFile("report.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));return 0;
}
int renderFixtures(const Arguments& args)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, args.plugin.getFullPathName());
    require(descriptions.size() == 1 && isAirInstrument(*descriptions[0]), "Bundle must contain the air instrument");
    const auto& description = *descriptions[0];
    // Recheck immediately before creating files, after loading/scanning the actual plugin.
    parseArguments({"--plugin", args.plugin.getFullPathName(), "--output", args.output.getFullPathName()});
    require(args.output.createDirectory().wasOk(), "Cannot create output directory");
    const juce::String marker = "Incomplete until report.json exists with success=true. Artifacts are never overwritten.\n";
    writeBlock(args.output.getChildFile("render-run.txt"), juce::MemoryBlock(marker.toRawUTF8(), marker.getNumBytesAsUTF8()));
    const auto binary = binaryIn(args.plugin);
    const auto originalBinaryHash = hash(binary);
    auto* report = new juce::DynamicObject;
    juce::var reportValue(report);
    report->setProperty("schema", 1);
    report->setProperty("plugin_bundle", args.plugin.getFullPathName());
    report->setProperty("plugin_binary", identity(binary));
    report->setProperty("helper", identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    report->setProperty("plugin_name", description.name);
    report->setProperty("plugin_manufacturer", description.manufacturerName);
    report->setProperty("plugin_unique_id", description.uniqueId);
    report->setProperty("juce_commit", "be29c81492b6151c8ea8d14c840e1311963b3a83");
    report->setProperty("owned_parameter_count", static_cast<int>(airack::parameterCount));
    report->setProperty("normalization_applied", false);
    report->setProperty("bpm", 120); report->setProperty("midi_ppq", 480);
    report->setProperty("duration_seconds", 2.5); report->setProperty("tail_window_start_seconds", 2.25);
    report->setProperty("difference_limit", 1e-6); report->setProperty("tail_peak_limit", 1e-7);
    report->setProperty("limits", "Automated offline behavior only. No listening, musical quality, performance, or universal DAW acceptance. Core uses conservative 1/32 voice scaling; output is quiet and remains unnormalized.");
    const auto midiFile = args.output.getChildFile("four-note-120bpm.mid"); writeMidi(midiFile);
    report->setProperty("midi", identity(midiFile));
    const auto overlappingMidi=args.output.getChildFile("four-overlapping-notes-120bpm.mid");writeMidi(overlappingMidi,true);
    juce::Array<juce::var> fixtures;
    struct Fixture { juce::String name;airack::SoundDocument document;juce::MemoryBlock owned;bool tempo=false;int automateSlot=-1; };
    std::vector<Fixture> cases;
    for(bool wavetable:{false,true})
    {
        Fixture f;f.name=wavetable?"wavetable-stab":"analog-pluck";f.document.values=fixtureValues(wavetable);
        f.document.layout.synthUse.fill(airack::SlotUse::present);f.document.layout.synthOrder={0,1,2,3};f.document.layout.synthCount=4;
        require(airack::encodeSoundState(f.document,f.owned),"M1 fixture encoding failed");cases.push_back(f);
    }
    auto set=[](airack::SoundDocument& d,const juce::String& id,float physical){
        const auto registry=airack::parameterRegistry();const auto it=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return p.id==id;});
        require(it!=registry.end(),"Expanded fixture missing control");d.values[static_cast<std::size_t>(it-registry.begin())]=juce::NormalisableRange<float>(it->minimum,it->maximum,0,it->skew).convertTo0to1(physical);
    };
    for(bool tempo:{false,true})
    {
        Fixture f;f.name=tempo?"compound-mono-modulated-tempo":"compound-mono-modulated-no-tempo";f.tempo=tempo;f.document=cases[0].document;auto& d=f.document;
        d.layout.synthOrder={1,0,3,2};set(d,"synth.2.enabled",1);set(d,"synth.2.engine",1);
        for(int osc:{1,2})set(d,"synth.2.wavetable.osc."+juce::String(osc)+".position",.72f);
        set(d,"synth.2.wavetable.amp.attack",.002f);set(d,"synth.2.wavetable.amp.decay",.12f);set(d,"synth.2.wavetable.amp.sustain",0);set(d,"synth.2.wavetable.amp.release",.1f);
        set(d,"global.voice_mode",1);set(d,"global.glide_seconds",.04f);set(d,"global.legato",1);
        set(d,"synth.1.lfo.1.synced",1);set(d,"synth.1.lfo.1.beats",.25f);
        set(d,"synth.1.modulation.1.enabled",1);set(d,"synth.1.modulation.1.source",3);set(d,"synth.1.modulation.1.target",14);set(d,"synth.1.modulation.1.bank",1);set(d,"synth.1.modulation.1.amount",.2f);
        for(int n=0;n<4;++n){airack::RackEdit edit;edit.kind=airack::RackEditKind::addEffect;edit.slot=static_cast<std::uint8_t>(n);edit.destination=n==0?0:airack::masterChain;edit.position=n==0?0:static_cast<std::uint8_t>(n-1);edit.effect=static_cast<airack::EffectKind>(n);require(airack::editRackLayout(d.layout,edit,d.layout)==airack::RackEditResult::applied,"Expanded fixture effect layout failed");}
        set(d,"effect.1.drive.gain_db",8);set(d,"effect.2.delay.seconds",.015f);set(d,"effect.2.delay.feedback",.05f);set(d,"effect.2.delay.synced",1);set(d,"effect.2.delay.beats",.0625f);set(d,"effect.2.delay.mix",.15f);
        set(d,"effect.3.reverb.decay_seconds",.1f);set(d,"effect.3.reverb.size",.1f);set(d,"effect.3.reverb.mix",.1f);set(d,"effect.4.compressor.release_ms",10);
        const auto registry=airack::parameterRegistry();const auto cutoff=std::find_if(registry.begin(),registry.end(),[](const auto& p){return p.id=="synth.2.wavetable.filter.cutoff";});
        d.macros[0]={cutoff->stateKey,-.2f};d.values[1]=.35f;d.synthLocks[1]=true;d.effectLocks[2]=true;d.controlLocks[static_cast<std::size_t>(cutoff-registry.begin())]=true;
        require(airack::validateSoundDocument(d)&&airack::encodeSoundState(d,f.owned),"Invalid compound fixture");cases.push_back(f);
    }
    // Read the frozen historical files, preserve their 209 record ordering and
    // key identities, change only the requested historical values and digest.
    juce::Array<juce::var> historicalSources;
    for(int slot=1;slot<4;++slot)for(bool automate:{false,true})
    {
        const bool wavetable=slot==2;const auto source=juce::File(RACKSYNTH_WORKSPACE).getChildFile(wavetable?"tests/fixtures/m1-wavetable-stab.soundstate":"tests/fixtures/m1-analog-pluck.soundstate");
        juce::MemoryBlock historical;require(source.loadFileAsData(historical)&&historical.getSize()==1720,"Historical source missing");
        Fixture f;f.name="migrated-slot"+juce::String(slot+1)+(automate?"-later-automation":"-saved-enabled");f.automateSlot=automate?slot:-1;
        require(airack::decodeSoundState(historical.getData(),historical.getSize(),f.document),"Historical source decode failed");
        auto& d=f.document;set(d,"synth.1.enabled",0);set(d,"synth."+juce::String(slot+1)+".enabled",automate?0.f:1.f);set(d,"synth."+juce::String(slot+1)+".engine",wavetable?1.f:0.f);
        const std::size_t bank=wavetable?120:28;for(std::size_t n=0;n<23;++n)d.values[bank+static_cast<std::size_t>(slot)*23+n]=d.values[bank+n];
        juce::MemoryInputStream input(historical,false);juce::MemoryOutputStream output(f.owned,false);
        for(int n=0;n<4;++n)output.writeInt(input.readInt());
        for(int n=0;n<209;++n){const auto key=input.readInt();input.readFloat();const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(key));require(index.has_value(),"Historical key not retained");output.writeInt(key);output.writeFloat(d.values[*index]);}
        output.flush();const auto digest=juce::SHA256(f.owned).getRawData();output.write(digest.getData(),digest.getSize());output.flush();
        airack::SoundDocument checked;require(airack::decodeSoundState(f.owned.getData(),f.owned.getSize(),checked)&&checked==d,"Historical fixture roundtrip failed");
        historicalSources.add(identity(source));cases.push_back(f);
    }
    report->setProperty("historical_sources",historicalSources);report->setProperty("owned_named_ids_verified",static_cast<int>(airack::parameterCount));report->setProperty("historical_named_ids_verified",209);
    report->setProperty("named_id_policy","Pinned JUCE managed named ParamIDs; independent of registry ordinal; wrapper extras counted separately");
    for(const auto& f:cases)
    {
        const auto& name=f.name;const auto& owned=f.owned;
        const auto ownedFile=args.output.getChildFile(name+".soundstate");writeBlock(ownedFile,owned);
        const auto envelope=hostEnvelope(owned);auto recalledDocument=f.document;
        if(f.automateSlot>=0)set(recalledDocument,"synth."+juce::String(f.automateSlot+1)+".enabled",1);
        for (int rate : {44100, 48000, 96000})
        {
            auto baseline = render(format, description, rate, 64, envelope, f.document, airack::currentSoundStateSize, f.tempo, f.automateSlot);
            auto partition = render(format, description, rate, 512, envelope, f.document, airack::currentSoundStateSize, f.tempo, f.automateSlot);
            // Opaque full host state from the completed baseline instance goes into a fresh instance.
            auto recalled = render(format, description, rate, 64, baseline.hostState, recalledDocument, airack::currentSoundStateSize, f.tempo);
            const auto partitionDifference = difference(baseline.audio, partition.audio);
            const auto recallDifference = difference(baseline.audio, recalled.audio);
            require(partitionDifference <= 1e-6 && recallDifference <= 1e-6, "Actual VST3 partition/recall mismatch");
            const auto stem = name + "-" + juce::String(rate);
            const auto hostFile = args.output.getChildFile(stem + ".hoststate"); writeBlock(hostFile, baseline.hostState);
            auto* fixture = new juce::DynamicObject;
            juce::var fixtureValue(fixture);
            fixture->setProperty("name", name); fixture->setProperty("sample_rate", rate);
            fixture->setProperty("host_tempo_present",f.tempo);fixture->setProperty("later_automated_physical_slot",f.automateSlot+1);
            fixture->setProperty("midi",identity(f.document.values[9]>=.5f?overlappingMidi:midiFile));
            fixture->setProperty("owned_state", identity(ownedFile)); fixture->setProperty("full_host_state", identity(hostFile));
            fixture->setProperty("returned_owned_state_verified_all_three_instances", true);
            fixture->setProperty("hosted_parameters", baseline.hostedParameters);
            fixture->setProperty("partition_max_abs_difference", partitionDifference);
            fixture->setProperty("recall_max_abs_difference", recallDifference);
            juce::Array<juce::var> runs;
            int runIndex = 0;
            for (const auto* result : {&baseline, &partition, &recalled})
            {
                const auto metrics = measure(result->audio, rate * 9 / 4);
                const juce::String mode = runIndex == 0 ? "block64" : runIndex == 1 ? "block512" : "recall64";
                const auto wavFile = args.output.getChildFile(stem + "-" + mode + ".wav");
                writeWav(wavFile, result->audio, rate);
                auto* run = new juce::DynamicObject;
                run->setProperty("mode", mode); run->setProperty("block_size", runIndex == 1 ? 512 : 64);
                run->setProperty("wav", identity(wavFile)); run->setProperty("samples", result->audio.getNumSamples());
                run->setProperty("peak", metrics.peak); run->setProperty("rms", metrics.rms);
                run->setProperty("peak_dbfs", 20 * std::log10(metrics.peak)); run->setProperty("rms_dbfs", 20 * std::log10(metrics.rms));
                run->setProperty("tail_peak", metrics.tailPeak); run->setProperty("finite", true);
                runs.add(juce::var(run)); ++runIndex;
            }
            fixture->setProperty("runs", runs); fixtures.add(fixtureValue);
            std::cout << stem << ": partition=" << partitionDifference << ", recall=" << recallDifference << '\n';
        }
    }
    require(hash(binary) == originalBinaryHash, "Plugin binary changed during rendering");
    report->setProperty("fixtures", fixtures); report->setProperty("success", true);
    const auto json = juce::JSON::toString(reportValue, true);
    writeBlock(args.output.getChildFile("report.json"), juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()));
    std::cout << "SUCCESS: 30 fixtures, 90 fresh actual VST3 instances; owned catalog/metadata/migration verified; float WAV/MIDI readbacks exact\n";
    return 0;
}

enum class GainPhrase { sustained, bass, sixteen };
std::vector<Event> gainPhrase(int rate, GainPhrase kind)
{
    require(rate > 0 && rate % 4 == 0, "Unsupported gain phrase sample rate");
    std::vector<Event> events;
    const int notes = kind == GainPhrase::bass ? 4 : kind == GainPhrase::sixteen ? 16 : 1;
    for (int i = 0; i < notes; ++i)
    {
        const int note = kind == GainPhrase::bass ? 36 : kind == GainPhrase::sixteen ? 48 + i : 48;
        const int onset = kind == GainPhrase::bass ? i * rate / 2 : 0;
        const int off = kind == GainPhrase::bass ? onset + rate / 4 : 2 * rate;
        events.push_back({onset, juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100))});
        events.push_back({off, juce::MidiMessage::noteOff(1, note)});
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.sample < b.sample; });
    return events;
}

void setGainValue(airack::SoundDocument& document, const juce::String& id, float value)
{
    const auto catalog = airack::parameterRegistry();
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& p) { return p.id == id; });
    require(found != catalog.end() && std::isfinite(value) && value >= found->minimum && value <= found->maximum, "Invalid named gain fixture value");
    document.values[static_cast<std::size_t>(found - catalog.begin())] = juce::NormalisableRange<float>(found->minimum, found->maximum, 0, found->skew).convertTo0to1(value);
}

struct GainCase
{
    juce::String name, variant = "unchanged";
    airack::SoundDocument document;
    GainPhrase phrase = GainPhrase::sustained;
    juce::File historicalSource;
};
std::vector<GainCase> gainCases(bool legacy=false)
{
    std::vector<GainCase> cases;
    for (bool wave : {false, true})
    {
        GainCase original; original.name = wave ? "original-stab" : "original-pluck";
        original.document.values = fixtureValues(wave); cases.push_back(original);
        if(!legacy){auto calibrated=original;calibrated.name=wave?"calibrated-stab":"calibrated-pluck";
            setGainValue(calibrated.document,"output.gain_db",30);setGainValue(calibrated.document,"output.protection",1);cases.push_back(calibrated);}
        GainCase historical; historical.name = wave ? "historical-stab" : "historical-pluck";
        historical.historicalSource = juce::File(RACKSYNTH_WORKSPACE).getChildFile(wave ? "tests/fixtures/m1-wavetable-stab.soundstate" : "tests/fixtures/m1-analog-pluck.soundstate");
        juce::MemoryBlock bytes;
        require(historical.historicalSource.loadFileAsData(bytes) && bytes.getSize() == 1720
            && airack::decodeSoundState(bytes.getData(), bytes.getSize(), historical.document), "Historical gain source decode failed");
        cases.push_back(historical);
        GainCase defaults; defaults.name = wave ? "default-wavetable" : "default-analog";
        if (wave) setGainValue(defaults.document, "synth.1.engine", static_cast<float>(airack::EngineKind::wavetable));
        cases.push_back(defaults);
    }
    GainCase saw; saw.name = "sustained-saw";
    auto& s = saw.document;
    setGainValue(s, "synth.1.analog.amp.attack", .005f); setGainValue(s, "synth.1.analog.amp.decay", .08f);
    setGainValue(s, "synth.1.analog.amp.sustain", .75f); setGainValue(s, "synth.1.analog.amp.release", .12f);
    setGainValue(s, "synth.1.analog.filter.cutoff", 2800); setGainValue(s, "synth.1.analog.filter.resonance", .25f);
    setGainValue(s, "synth.1.analog.noise", 0); cases.push_back(saw);
    GainCase bass; bass.name = "short-bass"; bass.phrase = GainPhrase::bass; auto& b = bass.document;
    setGainValue(b, "synth.1.analog.osc.1.waveform", static_cast<float>(airack::Waveform::sine));
    setGainValue(b, "synth.1.analog.osc.1.level", .8f);
    setGainValue(b, "synth.1.analog.osc.2.waveform", static_cast<float>(airack::Waveform::saw));
    setGainValue(b, "synth.1.analog.osc.2.level", .2f); setGainValue(b, "synth.1.analog.osc.2.tune", -12);
    setGainValue(b, "synth.1.analog.filter.cutoff", 450); setGainValue(b, "synth.1.analog.filter.resonance", .2f);
    setGainValue(b, "synth.1.analog.amp.attack", .002f); setGainValue(b, "synth.1.analog.amp.decay", .15f);
    setGainValue(b, "synth.1.analog.amp.sustain", .2f); setGainValue(b, "synth.1.analog.amp.release", .08f);
    setGainValue(b, "synth.1.analog.noise", 0); cases.push_back(bass);
    for (int configuration = 0; configuration < 4; ++configuration)
    {
        GainCase full; full.phrase = GainPhrase::sixteen;
        full.name = configuration == 0 ? "full-saw-dry" : configuration == 1 ? "full-wave-dry" : configuration == 2 ? "full-normal-effects" : "full-high-gain-effects";
        auto& d = full.document;
        d.layout.synthUse.fill(airack::SlotUse::present); d.layout.synthOrder = {0, 1, 2, 3}; d.layout.synthCount = 4;
        // All slot defaults share the same banks/common values; only enabled/engine differ.
        for (int slot = 1; slot <= 4; ++slot)
        {
            setGainValue(d, "synth." + juce::String(slot) + ".enabled", 1);
            const bool wave = configuration == 1 || (configuration >= 2 && slot % 2 == 0);
            setGainValue(d, "synth." + juce::String(slot) + ".engine", static_cast<float>(wave ? airack::EngineKind::wavetable : airack::EngineKind::analog));
        }
        setGainValue(d,"output.gain_db",configuration<2?12.f:configuration==2?24.f:30.f);
        if (configuration < 2) { cases.push_back(full); continue; }
        constexpr airack::EffectKind kinds[] {airack::EffectKind::drive, airack::EffectKind::delay, airack::EffectKind::reverb, airack::EffectKind::compressor};
        const bool stress = configuration == 3;
        for (int n = 0; n < 8; ++n)
        {
            airack::RackEdit edit; edit.kind = airack::RackEditKind::addEffect; edit.slot = static_cast<std::uint8_t>(n);
            edit.destination = n < 4 ? static_cast<std::uint8_t>(n) : airack::masterChain;
            edit.position = n < 4 ? 0 : static_cast<std::uint8_t>(n - 4); edit.effect = kinds[n % 4];
            require(airack::editRackLayout(d.layout, edit, d.layout) == airack::RackEditResult::applied, "Gain effect layout failed");
            const auto prefix = "effect." + juce::String(n + 1) + ".";
            switch (edit.effect)
            {
                case airack::EffectKind::drive:
                    setGainValue(d, prefix + "drive.gain_db", stress ? 24.f : 6.f); setGainValue(d, prefix + "drive.mix", stress ? 1.f : .5f); break;
                case airack::EffectKind::delay:
                    setGainValue(d, prefix + "delay.seconds", stress ? .01f : .125f); setGainValue(d, prefix + "delay.feedback", stress ? .95f : .35f);
                    setGainValue(d, prefix + "delay.mix", stress ? 1.f : .25f); setGainValue(d, prefix + "delay.synced", 0); break;
                case airack::EffectKind::reverb:
                    setGainValue(d, prefix + "reverb.size", .5f); setGainValue(d, prefix + "reverb.decay_seconds", stress ? 20.f : .5f);
                    setGainValue(d, prefix + "reverb.damping", .5f); setGainValue(d, prefix + "reverb.predelay_ms", 0);
                    setGainValue(d, prefix + "reverb.mix", stress ? 1.f : .25f); break;
                case airack::EffectKind::compressor:
                    setGainValue(d, prefix + "compressor.threshold_db", -18); setGainValue(d, prefix + "compressor.ratio", stress ? 1.f : 4.f);
                    setGainValue(d, prefix + "compressor.attack_ms", 10); setGainValue(d, prefix + "compressor.release_ms", 100);
                    setGainValue(d, prefix + "compressor.makeup_db", stress ? 24.f : 0.f); setGainValue(d, prefix + "compressor.mix", 1); break;
            }
        }
        for (int variant = 0; variant < 4; ++variant)
        {
            auto matched = full;
            matched.variant = variant == 0 ? "all-bypassed" : variant == 1 ? "inserts-only" : variant == 2 ? "master-only" : "all-active";
            for (int n = 0; n < 8; ++n)
                setGainValue(matched.document, "effect." + juce::String(n + 1) + ".bypassed", (n < 4 ? (variant & 1) : (variant & 2)) ? 0.f : 1.f);
            cases.push_back(matched);
        }
    }
    if(legacy)for(auto& c:cases){setGainValue(c.document,"output.gain_db",0);setGainValue(c.document,"output.protection",0);}
    require(cases.size() == (legacy?18u:20u), "Incomplete gain case set");
    for (const auto& c : cases) require(airack::validateSoundDocument(c.document), "Invalid gain case document");
    return cases;
}

juce::var windowJson(const juce::AudioBuffer<float>& audio, const juce::String& name, const std::vector<Interval>& intervals)
{
    const auto m = measureWindow(audio, intervals);
    auto* object = new juce::DynamicObject; juce::var result(object);
    object->setProperty("name", name); object->setProperty("sample_count", m.samples); object->setProperty("frames_per_channel", m.samples / 2);
    object->setProperty("peak", m.peak); object->setProperty("rms", m.rms); object->setProperty("dc_mean", m.dc);
    object->setProperty("peak_dbfs", dbfs(m.peak)); object->setProperty("rms_dbfs", dbfs(m.rms));
    object->setProperty("zero", m.peak == 0); object->setProperty("finite", true); object->setProperty("samples_abs_ge_1", m.excursions);
    juce::Array<juce::var> bounds, channels;
    for (const auto& interval : intervals) { auto* pair = new juce::DynamicObject; pair->setProperty("begin", interval.begin); pair->setProperty("end", interval.end); bounds.add(juce::var(pair)); }
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        auto* channel = new juce::DynamicObject;
        channel->setProperty("channel", static_cast<int>(ch)); channel->setProperty("sample_count", m.samples / 2);
        channel->setProperty("peak", m.channelPeak[ch]); channel->setProperty("rms", m.channelRms[ch]); channel->setProperty("dc_mean", m.channelDc[ch]);
        channel->setProperty("peak_dbfs", dbfs(m.channelPeak[ch])); channel->setProperty("rms_dbfs", dbfs(m.channelRms[ch]));
        channel->setProperty("samples_abs_ge_1", m.channelExcursions[ch]); channels.add(juce::var(channel));
    }
    object->setProperty("intervals_half_open", bounds); object->setProperty("channels", channels); return result;
}

int compareLegacyGain(const Arguments& args)
{
    const auto workspace=juce::File(RACKSYNTH_WORKSPACE);
    const auto referenceRoot=workspace.getChildFile("build/m2-gain-task1-release-20261004");
    const auto referenceReport=referenceRoot.getChildFile("gain-report.json");
    require(referenceReport.getSize()>0&&referenceReport.getSize()<4*1024*1024,"Legacy report size out of bounds");
    const auto old=juce::JSON::parse(referenceReport.loadFileAsString());
    const auto* records=old["fixtures"].getArray();
    require(bool(old["success"])&&int(old["fresh_instances"])==162&&int(old["bpm"])==120&&int(old["duration_seconds"])==4
        &&records&&records->size()==54,"Legacy report dataset contract changed");
    auto checkedReference=[&](const juce::var& item,const juce::String& filename){
        const auto file=referenceRoot.getChildFile(filename);
        const auto canonical=fs::weakly_canonical(fs::path(file.getFullPathName().toWideCharPointer()));
        const auto root=fs::weakly_canonical(fs::path(workspace.getFullPathName().toWideCharPointer()));
        const auto relative=canonical.lexically_relative(root);
        require(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="..","Legacy reference escapes workspace");
        require(item["path"].toString()==file.getFullPathName()&&juce::int64(item["bytes"])==file.getSize()&&item["sha256"].toString()==hash(file),"Legacy reference identity mismatch");
        return file;
    };
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions,args.plugin.getFullPathName());
    require(descriptions.size()==1&&isAirInstrument(*descriptions[0]),"Unexpected legacy readback product");
    parseArguments({"--compare-legacy-gain","--plugin",args.plugin.getFullPathName(),"--output",args.output.getFullPathName()});
    require(args.output.createDirectory().wasOk(),"Cannot create legacy comparison output");
    const auto binary=binaryIn(args.plugin),helper=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto binaryHash=hash(binary),helperHash=hash(helper);
    auto* report=new juce::DynamicObject;juce::var reportValue(report);juce::Array<juce::var> results;
    report->setProperty("reference_report",identity(referenceReport));report->setProperty("plugin",identity(binary));report->setProperty("helper",identity(helper));
    report->setProperty("historical_binary_identity",old["plugin_binary_before"]);report->setProperty("historical_helper_identity",old["helper_before"]);
    report->setProperty("historical_renderer_source",old["renderer_source"]);report->setProperty("current_renderer_source",identity(workspace.getChildFile("tools/native/RenderFixtures.cpp")));
    // Historical source/helper hashes identify preimage bytes, not overwritten current build paths.
    std::array<bool,54> seen{};
    for(const auto& c:gainCases(true))for(int rate:{44100,48000,96000}){
        int found=-1;
        for(int i=0;i<54;++i){const auto& record=(*records)[i];if(record["name"].toString()==c.name&&record["variant"].toString()==c.variant&&int(record["sample_rate"])==rate){require(found<0,"Duplicate legacy record");found=i;}}
        require(found>=0&&!seen[std::size_t(found)],"Missing legacy case/rate");seen[std::size_t(found)]=true;
        const auto& record=(*records)[found];const auto name=c.name+(c.variant=="unchanged"?juce::String():"-"+c.variant),stem=name+"-"+juce::String(rate);
        const auto stateFile=checkedReference(record["owned_state"],name+".soundstate");require(stateFile.getSize()==7006,"Legacy input must remain v2/7006");
        juce::MemoryBlock owned;require(stateFile.loadFileAsData(owned),"Cannot load frozen v2 state");airack::SoundDocument decoded;
        require(airack::decodeSoundState(owned.getData(),owned.getSize(),decoded)&&decoded==c.document,"Legacy v2 mapping or workload changed");
        const auto events=gainPhrase(rate,c.phrase);const auto* savedEvents=record["events"].getArray();require(savedEvents&&savedEvents->size()==int(events.size()),"Legacy event count changed");
        for(int i=0;i<savedEvents->size();++i){const auto& a=(*savedEvents)[i];const auto& b=events[std::size_t(i)];require(int(a["sample"])==b.sample&&int(a["note"])==b.message.getNoteNumber()&&bool(a["on"])==b.message.isNoteOn()&&int(a["velocity"])==b.message.getVelocity()&&int(a["channel"])==b.message.getChannel(),"Legacy note event changed");}
        const auto envelope=hostEnvelope(owned);
        auto baseline=render(format,*descriptions[0],rate,512,envelope,decoded,airack::currentSoundStateSize,true,-1,&events,4*rate);
        auto partition=render(format,*descriptions[0],rate,64,envelope,decoded,airack::currentSoundStateSize,true,-1,&events,4*rate);
        auto recall=render(format,*descriptions[0],rate,64,baseline.hostState,decoded,airack::currentSoundStateSize,true,-1,&events,4*rate);
        require(difference(baseline.audio,partition.audio)==0&&difference(baseline.audio,recall.audio)==0,"Legacy partition or opaque recall changed");
        const auto* runs=record["runs"].getArray();require(runs&&runs->size()==3,"Legacy mode count changed");int i=0;
        for(const auto* rendered:{&baseline,&partition,&recall}){
            const juce::String mode=i==0?"block512":i==1?"block64":"recall64";const auto& run=(*runs)[i++];require(run["mode"].toString()==mode&&int(run["block_size"])==(i==1?512:64),"Legacy mode changed");
            const auto input=checkedReference(run["wav"],stem+"-"+mode+".wav");juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(input.createInputStream().release(),true));
            require(reader&&reader->usesFloatingPointData&&reader->bitsPerSample==32&&reader->numChannels==2&&reader->sampleRate==rate&&reader->lengthInSamples==4*rate,"Legacy WAV geometry changed");
            juce::AudioBuffer<float> raw(2,4*rate);require(reader->read(&raw,0,4*rate,0,true,true),"Legacy WAV read failed");
            const auto diff=difference(raw,rendered->audio);require(diff==0,"Legacy raw audio changed");
            const auto output=args.output.getChildFile(stem+"-"+mode+".wav");writeWav(output,rendered->audio,rate);
            auto* result=new juce::DynamicObject;result->setProperty("name",c.name);result->setProperty("variant",c.variant);result->setProperty("sample_rate",rate);result->setProperty("mode",mode);
            result->setProperty("reference",identity(input));result->setProperty("wav",identity(output));result->setProperty("maximum_absolute_difference",diff);results.add(juce::var(result));
        }
        std::cout<<stem<<": 3 legacy raw comparisons exact"<<std::endl;
    }
    require(std::all_of(seen.begin(),seen.end(),[](bool x){return x;})&&results.size()==162,"Incomplete legacy comparison");
    require(hash(binary)==binaryHash&&hash(helper)==helperHash,"Legacy comparison binary changed");
    report->setProperty("runs",results);report->setProperty("fresh_instances",162);report->setProperty("success",true);
    report->setProperty("limits","Exact historical electrical recall, including old unrestricted overload. No normalization, listening or realtime claim.");
    const auto json=juce::JSON::toString(reportValue,true);writeBlock(args.output.getChildFile("legacy-gain-report.json"),juce::MemoryBlock(json.toRawUTF8(),json.getNumBytesAsUTF8()));
    std::cout<<"SUCCESS: 162 fresh actual VST3 legacy instances match frozen raw audio exactly\n";return 0;
}

int measureGain(const Arguments& args)
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto helper = juce::File::getSpecialLocation(juce::File::currentExecutableFile), binary = binaryIn(args.plugin);
    const auto helperHash = hash(helper), binaryHash = hash(binary);
    juce::VST3PluginFormat format; juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, args.plugin.getFullPathName());
    require(descriptions.size() == 1 && isAirInstrument(*descriptions[0]), "Bundle must contain the air instrument");
    parseArguments({"--measure-gain", "--plugin", args.plugin.getFullPathName(), "--output", args.output.getFullPathName()});
    require(args.output.createDirectory().wasOk(), "Cannot create gain output directory");
    const juce::String marker = "Incomplete unless gain-report.json exists with success=true. Raw artifacts preserved; no overwrite.\n";
    writeBlock(args.output.getChildFile("gain-run.txt"), juce::MemoryBlock(marker.toRawUTF8(), marker.getNumBytesAsUTF8()));
    auto* report = new juce::DynamicObject; juce::var reportValue(report);
    report->setProperty("schema", 1); report->setProperty("plugin_binary_before", identity(binary)); report->setProperty("helper_before", identity(helper));
    report->setProperty("renderer_source", identity(juce::File(RACKSYNTH_WORKSPACE).getChildFile("tools/native/RenderFixtures.cpp")));
    report->setProperty("juce_commit", "be29c81492b6151c8ea8d14c840e1311963b3a83");
#ifdef NDEBUG
    report->setProperty("configuration", "Release");
#else
    report->setProperty("configuration", "Debug");
#endif
    report->setProperty("msvc_full_version", _MSC_FULL_VER); report->setProperty("cpp_standard", 20);
    report->setProperty("bpm", 120); report->setProperty("midi_ppq", 480); report->setProperty("duration_seconds", 4);
    report->setProperty("owned_named_parameters_verified", static_cast<int>(airack::parameterCount)); report->setProperty("normalization_applied", false);
    report->setProperty("difference_limit", 1e-6);
    report->setProperty("limits", "Raw independent full renders, not simultaneous internal signal taps. Finite clipping excursions and remaining tails are observations. Electrical output calibration only; no listening, deadline benchmark, DAW or beta acceptance. Windows within each union are disjoint; named windows intentionally overlap.");
    juce::Array<juce::var> fixtures;
    for (const auto& c : gainCases())
    {
        const auto name = c.name + (c.variant == "unchanged" ? juce::String() : "-" + c.variant);
        juce::MemoryBlock owned; require(airack::encodeSoundState(c.document, owned) && owned.getSize() == airack::currentSoundStateSize, "Gain state encoding failed");
        const auto ownedFile = args.output.getChildFile(name + ".soundstate"); writeBlock(ownedFile, owned);
        const auto midiFile = args.output.getChildFile(name + ".mid"); writeMidiEvents(midiFile, gainPhrase(960, c.phrase));
        for (int rate : {44100, 48000, 96000})
        {
            const auto events = gainPhrase(rate, c.phrase); const auto envelope = hostEnvelope(owned);
            auto baseline = render(format, *descriptions[0], rate, 512, envelope, c.document, owned.getSize(), true, -1, &events, rate * 4);
            auto partition = render(format, *descriptions[0], rate, 64, envelope, c.document, owned.getSize(), true, -1, &events, rate * 4);
            auto recalled = render(format, *descriptions[0], rate, 64, baseline.hostState, c.document, owned.getSize(), true, -1, &events, rate * 4);
            const auto partitionDifference = difference(baseline.audio, partition.audio), recallDifference = difference(baseline.audio, recalled.audio);
            require(partitionDifference <= 1e-6 && recallDifference <= 1e-6, "Gain partition or recall mismatch");
            const auto stem = name + "-" + juce::String(rate);
            const auto hostFile = args.output.getChildFile(stem + ".hoststate"); writeBlock(hostFile, baseline.hostState);
            auto* fixture = new juce::DynamicObject; juce::var fixtureValue(fixture);
            fixture->setProperty("name", c.name); fixture->setProperty("variant", c.variant); fixture->setProperty("sample_rate", rate);
            fixture->setProperty("owned_state", identity(ownedFile)); fixture->setProperty("midi", identity(midiFile)); fixture->setProperty("full_host_state_from_block512", identity(hostFile));
            if (c.historicalSource.existsAsFile()) fixture->setProperty("historical_source", identity(c.historicalSource));
            fixture->setProperty("returned_owned_state_verified_all_three_instances", true); fixture->setProperty("hosted_parameters", baseline.hostedParameters);
            fixture->setProperty("partition_max_abs_difference", partitionDifference); fixture->setProperty("recall_max_abs_difference", recallDifference);
            juce::Array<juce::var> eventList, runs;
            for (const auto& event : events)
            {
                auto* e = new juce::DynamicObject; e->setProperty("sample", event.sample); e->setProperty("note", event.message.getNoteNumber());
                e->setProperty("on", event.message.isNoteOn()); e->setProperty("velocity", static_cast<int>(event.message.getVelocity())); e->setProperty("channel", event.message.getChannel()); eventList.add(juce::var(e));
            }
            fixture->setProperty("events", eventList);
            fixture->setProperty("output_gain_db",airack::parameterPhysicalValue(c.document.values,756));
            fixture->setProperty("output_protection",c.document.values[757]==1);
            juce::Array<juce::var> settings;
            const auto catalog=airack::parameterRegistry();
            for(std::size_t n=0;n<catalog.size();++n){auto* setting=new juce::DynamicObject;setting->setProperty("id",catalog[n].id);setting->setProperty("normalized",c.document.values[n]);setting->setProperty("physical",airack::parameterPhysicalValue(c.document.values,n));settings.add(juce::var(setting));}
            fixture->setProperty("settings",settings);
            std::vector<Interval> attack, gates;
            if (c.phrase == GainPhrase::bass)
                for (int i = 0; i < 4; ++i) { attack.push_back({i * rate / 2, i * rate / 2 + rate / 10}); gates.push_back({i * rate / 2, i * rate / 2 + rate / 4}); }
            else { attack = {{0, rate / 10}}; gates = {{0, 2 * rate}}; }
            int index = 0;
            for (const auto* r : {&baseline, &partition, &recalled})
            {
                const juce::String mode = index == 0 ? "block512" : index == 1 ? "block64" : "recall64";
                if(c.document.values[757]==1){const auto full=measureWindow(r->audio,{{0,4*rate}});require(full.peak<=std::pow(10.,-.05)+1e-6&&full.excursions==0,"Protected output exceeded sample ceiling");}
                const auto wav = args.output.getChildFile(stem + "-" + mode + ".wav"); writeWav(wav, r->audio, rate);
                auto* run = new juce::DynamicObject; run->setProperty("mode", mode); run->setProperty("block_size", index == 0 ? 512 : 64);
                run->setProperty("wav", identity(wav)); run->setProperty("raw_wav_readback_exact", true);
                juce::Array<juce::var> windows;
                windows.add(windowJson(r->audio, "full", {{0, 4 * rate}})); windows.add(windowJson(r->audio, "attack", attack)); windows.add(windowJson(r->audio, "note-gates", gates));
                if (c.phrase != GainPhrase::bass) windows.add(windowJson(r->audio, "sustain", {{rate / 4, 7 * rate / 4}}));
                windows.add(windowJson(r->audio, "release", {{2 * rate, 7 * rate / 2}})); windows.add(windowJson(r->audio, "observed-tail", {{7 * rate / 2, 4 * rate}}));
                run->setProperty("windows", windows); runs.add(juce::var(run)); ++index;
            }
            fixture->setProperty("runs", runs); fixtures.add(fixtureValue);
            std::cout << stem << ": partition=" << partitionDifference << ", recall=" << recallDifference << std::endl;
        }
    }
    require(hash(binary) == binaryHash && hash(helper) == helperHash, "Gain binary identity changed during measurement");
    report->setProperty("plugin_binary_after", identity(binary)); report->setProperty("helper_after", identity(helper));
    report->setProperty("elapsed_seconds", (juce::Time::getMillisecondCounterHiRes() - start) / 1000);
    report->setProperty("fixtures", fixtures); report->setProperty("fresh_instances", 180); report->setProperty("success", true);
    const auto json = juce::JSON::toString(reportValue, true);
    writeBlock(args.output.getChildFile("gain-report.json"), juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()));
    std::cout << "SUCCESS: 60 gain fixtures, 180 fresh actual VST3 instances; exact raw WAV/MIDI, named state and finite windows verified\n";
    return 0;
}

// M3 project and host-clock helpers. Expected owned size is independent of returned headers.
void verifyReturnedProject(const juce::MemoryBlock& opaque, const airack::ProjectDocument& expected)
{
    juce::MemoryBlock encoded;
    require(airack::encodeProjectState(expected, encoded), "Invalid expected project");
    auto xml = juce::AudioProcessor::getXmlFromBinary(opaque.getData(), static_cast<int>(opaque.getSize()));
    require(xml != nullptr && xml->hasTagName("VST3PluginState"), "Hosted state is not a VST3 state envelope");
    const auto* component = xml->getChildByName("IComponent");
    juce::MemoryBlock returned;
    require(component != nullptr && returned.fromBase64Encoding(component->getAllSubText())
        && returned.getSize() >= encoded.getSize(), "Hosted component state missing or truncated");
    airack::ProjectDocument decoded;
    require(airack::decodeProjectState(returned.getData(), encoded.getSize(), decoded) && decoded == expected,
        "Actual hosted component did not restore the requested complete project");
}
struct PatternClock
{
    int sample = 0;
    double ppq = 0, bpm = 120;
    bool present = true, playing = true, tempo = true, position = true;
    bool looping = false, loopPoints = false;
    double loopStart = .5, loopEnd = 1.5;
    int numerator = 4, denominator = 4;
};
juce::Optional<juce::AudioPlayHead::PositionInfo> patternPosition(const PatternClock& clock, int sample, int rate)
{
    if (!clock.present) return {};
    juce::AudioPlayHead::PositionInfo result;
    result.setTimeInSamples(sample);
    result.setIsPlaying(clock.playing);
    result.setIsLooping(clock.looping);
    if (clock.tempo) result.setBpm(clock.bpm);
    if (clock.position) {
        double ppq = clock.ppq + (clock.playing ? double(sample-clock.sample)*2/rate : 0);
        if (clock.looping && clock.loopPoints && std::isfinite(clock.loopStart) && std::isfinite(clock.loopEnd)
            && clock.loopEnd > clock.loopStart && ppq >= clock.loopEnd)
            ppq = clock.loopStart + std::fmod(ppq-clock.loopStart,clock.loopEnd-clock.loopStart);
        result.setPpqPosition(ppq);
    }
    if (clock.loopPoints) result.setLoopPoints(juce::AudioPlayHead::LoopPoints {clock.loopStart,clock.loopEnd});
    if (clock.numerator > 0) result.setTimeSignature(juce::AudioPlayHead::TimeSignature {clock.numerator,clock.denominator});
    return result;
}
struct WireNote
{
    int tick = 0, pitch = 0, velocity = 0, channel = 1;
    bool on = false;
    bool operator==(const WireNote&) const = default;
};
struct PatternWire { std::vector<WireNote> notes; int end = 0, numerator = 0, denominator = 0, metronome = 0, thirtySeconds = 0; };
PatternWire readPatternWire(const juce::MemoryBlock& bytes)
{
    // Physical wire reader deliberately independent of JUCE's event sorting/matched-pair reader.
    juce::MemoryInputStream input(bytes, false);
    auto u8 = [&] { require(!input.isExhausted(),"Truncated SMF"); return static_cast<unsigned char>(input.readByte()); };
    auto vlq = [&] { int value=0; for(int n=0;n<4;++n) { const auto b=u8(); value=(value<<7)|(b&127); if((b&128)==0)return value; } throw std::runtime_error("Oversized SMF VLQ"); };
    require(input.readIntBigEndian() == 0x4d546864 && input.readIntBigEndian() == 6 && input.readShortBigEndian() == 0
        && input.readShortBigEndian() == 1 && input.readShortBigEndian() == 960,"Expected SMF0 one track at 960PPQ");
    require(input.readIntBigEndian() == 0x4d54726b,"Missing track chunk");
    const auto length=input.readIntBigEndian(); require(length>0 && input.getPosition()+length==static_cast<juce::int64>(bytes.getSize()),"SMF chunk length mismatch");
    PatternWire result; int tick=0,running=0,meters=0; bool ended=false;
    while(!input.isExhausted()) {
        require(!ended,"Bytes after EOT"); tick+=vlq(); int status=u8(); int data=-1;
        if(status<128) { data=status; status=running; } else if(status<240) running=status;
        if(status==255) {
            const int type=u8(),count=vlq();
            if(type==0x58) { require(tick==0 && count==4 && ++meters==1,"Unexpected meter event"); result.numerator=u8(); const auto power=u8(); require(power<8,"Invalid meter denominator"); result.denominator=1<<power; result.metronome=u8(); result.thirtySeconds=u8(); }
            else if(type==0x2f) { require(count==0,"Malformed EOT"); result.end=tick; ended=true; }
            else throw std::runtime_error("Unexpected SMF metadata, including tempo");
        } else {
            require((status&240)==0x80 || (status&240)==0x90,"Unexpected SMF channel message");
            const int pitch=data<0?u8():data,velocity=u8(); require(pitch<128 && velocity<128,"Bad SMF note data");
            result.notes.push_back({tick,pitch,velocity,(status&15)+1,(status&240)==0x90 && velocity>0});
        }
    }
    require(ended && meters==1,"Missing SMF meter or EOT"); return result;
}
bool admitPatternArguments(const juce::StringArray& args)
{
    return args.size()==7 && (args[0]=="--patterns" || args[0]=="--patterns-smoke")
        && args[1]=="--plugin" && args[3]=="--output" && args[5]=="--profile-fixtures";
}

using Json = juce::var;
Json jsonObject() { return Json(new juce::DynamicObject); }
void property(Json& object,const char* key,const Json& value) { object.getDynamicObject()->setProperty(key,value); }
void writePatternJson(const juce::File& file,const Json& value)
{
    const auto text=juce::JSON::toString(value,true);
    writeBlock(file,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8()));
}
int beatSample(double elapsedBeats,int rate) { return int(std::floor(elapsedBeats*rate/2+.5)); }
struct LiteralEdge { double beat; int pitch,velocity=0,channel=1; };
struct ProjectRecall { int sample; airack::ProjectDocument project; };
struct PatternScene
{
    juce::String name;
    airack::ProjectDocument project;
    std::vector<PatternClock> clocks;
    std::vector<Event> expected;
    std::vector<ProjectRecall> recalls;
    int rate=48000,samples=0;
    bool silent=false;
};
std::vector<Event> literalEvents(std::initializer_list<LiteralEdge> edges,int rate)
{
    std::vector<Event> result;
    for(const auto& edge:edges) result.push_back({beatSample(edge.beat,rate),edge.velocity>0
        ?juce::MidiMessage::noteOn(edge.channel,edge.pitch,juce::uint8(edge.velocity))
        :juce::MidiMessage::noteOff(edge.channel,edge.pitch)});
    return result;
}
PatternScene makeScene(const juce::String& name,int rate,std::initializer_list<airack::PatternNote> notes,double stop)
{
    PatternScene result; result.name=name; result.rate=rate; result.samples=beatSample(stop,rate)+rate;
    result.project.sound.values=fixtureValues(false);
    // Sustained, deterministic dry sound makes releases/chases audible, including missing timing.
    setGainValue(result.project.sound,"synth.1.analog.amp.sustain",.6f);
    setGainValue(result.project.sound,"global.master_level",.25f);
    result.project.mode=airack::PlaybackMode::internalPattern;
    result.project.pattern.noteCount=static_cast<std::uint16_t>(notes.size());
    std::copy(notes.begin(),notes.end(),result.project.pattern.notes.begin());
    result.clocks.push_back({});
    PatternClock stopped; stopped.sample=beatSample(stop,rate); stopped.ppq=stop; stopped.playing=false;
    result.clocks.push_back(stopped);
    return result;
}
std::vector<PatternScene> musicalScenes(int rate,bool smoke)
{
    std::vector<PatternScene> scenes;
    auto straight=makeScene("straight",rate,{{0,.25,48,100,1},{.5,.25,52,90,1},{1,.25,55,80,2},{3.75,.25,60,70,16}},4);
    straight.expected=literalEvents({{0,48,100,1},{.25,48,0,1},{.5,52,90,1},{.75,52,0,1},{1,55,80,2},{1.25,55,0,2},{3.75,60,70,16},{4,60,0,16}},rate);
    scenes.push_back(straight);
    auto swing=makeScene("swing",rate,{{0,.25,48,100,1},{.25,.25,52,100,1},{.5,.25,55,100,1},{.75,.25,60,100,1}},4);
    swing.project.pattern.swing=1;
    swing.expected=literalEvents({{0,48,100},{.25,48},{.375,52,100},{.5,55,100},{.625,52},{.75,55},{.875,60,100},{1.125,60}},rate);
    scenes.push_back(swing);
    const std::vector<juce::String> invalidNames=smoke?std::vector<juce::String>{"missing-ppq"}:
        std::vector<juce::String>{"missing-bpm","missing-ppq","absent-position","missing-loop","bpm19","bpm401","bpm-nan","ppq-positive","ppq-negative","loop-equal","loop-inverted","loop-domain","loop-nan"};
    for(const auto& name:invalidNames) {
        auto scene=makeScene(name,rate,{{0,4,48,100,1}},2);
        PatternClock bad; bad.sample=beatSample(.5,rate); bad.ppq=.5;
        if(name=="missing-bpm")bad.tempo=false;
        if(name=="missing-ppq")bad.position=false;
        if(name=="absent-position")bad.present=false;
        if(name=="missing-loop")bad.looping=true;
        if(name=="bpm19")bad.bpm=19;
        if(name=="bpm401")bad.bpm=401;
        if(name=="bpm-nan")bad.bpm=std::numeric_limits<double>::quiet_NaN();
        if(name=="ppq-positive")bad.ppq=16777217;
        if(name=="ppq-negative")bad.ppq=-16777217;
        if(name.startsWith("loop-")) {
            bad.looping=true; bad.loopPoints=true;
            if(name=="loop-equal")bad.loopEnd=bad.loopStart;
            if(name=="loop-inverted")bad.loopStart=2;
            if(name=="loop-domain")bad.loopEnd=16777217;
            if(name=="loop-nan")bad.loopEnd=std::numeric_limits<double>::quiet_NaN();
        }
        PatternClock resume; resume.sample=beatSample(1,rate); resume.ppq=1;
        scene.clocks.insert(scene.clocks.begin()+1,{bad,resume});
        scene.expected=literalEvents({{0,48,100},{.5,48},{1,48,100},{2,48}},rate);
        scenes.push_back(scene);
    }
    if(smoke)return scenes;
    auto same=makeScene("same-key",rate,{{0,1,60,100,1},{.5,1,60,80,1},{1.5,.25,60,70,1}},4);
    same.expected=literalEvents({{0,60,100},{.5,60,80},{1.5,60},{1.5,60,70},{1.75,60}},rate); scenes.push_back(same);
    auto negative=makeScene("negative",rate,{{0,1,48,100,1}},2); negative.clocks[0].ppq=-.25;
    negative.expected=literalEvents({{.25,48,100},{1.25,48}},rate); scenes.push_back(negative);
    auto chase=makeScene("chase",rate,{{0,1,48,100,1}},2); chase.clocks[0].ppq=.5;
    chase.expected=literalEvents({{0,48,100},{.5,48}},rate); scenes.push_back(chase);
    auto seek=makeScene("seek-stop-resume",rate,{{0,2,48,100,1}},4);
    PatternClock jump; jump.sample=beatSample(.5,rate); jump.ppq=1.5;
    auto stop=jump; stop.sample=beatSample(1,rate); stop.ppq=2; stop.playing=false;
    PatternClock resume; resume.sample=beatSample(1.5,rate); resume.ppq=.25;
    seek.clocks.insert(seek.clocks.begin()+1,{jump,stop,resume});
    seek.expected=literalEvents({{0,48,100},{.5,48},{.5,48,100},{1,48},{1.5,48,100},{3.25,48}},rate); scenes.push_back(seek);
    auto loop=makeScene("host-loop",rate,{{0,4,48,100,1}},3);
    loop.clocks[0].ppq=.5; loop.clocks[0].looping=true; loop.clocks[0].loopPoints=true;
    loop.expected=literalEvents({{0,48,100},{1,48},{1,48,100},{2,48},{2,48,100},{3,48}},rate); scenes.push_back(loop);
    auto meter=makeScene("host-meter",rate,{{0,4,48,100,1}},2);
    PatternClock seven; seven.sample=beatSample(.5,rate); seven.ppq=.5; seven.numerator=7; seven.denominator=8;
    meter.clocks.insert(meter.clocks.begin()+1,seven); meter.expected=literalEvents({{0,48,100},{2,48}},rate); scenes.push_back(meter);
    auto recall=makeScene("full-recall-mode",rate,{{0,4,48,100,1}},2);
    auto replacement=recall.project; replacement.pattern.notes[0].pitch=52;
    setGainValue(replacement.sound,"synth.1.analog.filter.cutoff",2400);
    recall.recalls.push_back({beatSample(.5,rate),replacement});
    replacement.mode=airack::PlaybackMode::externalMidi;
    recall.recalls.push_back({beatSample(1,rate),replacement});
    recall.expected=literalEvents({{0,48,100},{.5,52,100},{1,52,100},{1.5,52}},rate); scenes.push_back(recall);
    auto empty=makeScene("empty",rate,{},4); empty.silent=true; scenes.push_back(empty);
    auto dense=makeScene("dense-eight-bar",rate,{},32); dense.project.pattern.bars=8; dense.project.pattern.noteCount=256;
    for(int k=0;k<256;++k) {
        const auto pitch=static_cast<std::uint8_t>(48+k%16);
        dense.project.pattern.notes[std::size_t(k)]={k/8.0,.0625,pitch,100,1};
        dense.expected.push_back({beatSample(k/8.0,rate),juce::MidiMessage::noteOn(1,pitch,juce::uint8(100))});
        dense.expected.push_back({beatSample(k/8.0+.0625,rate),juce::MidiMessage::noteOff(1,pitch)});
    }
    scenes.push_back(dense); return scenes;
}
Json clockJson(const PatternScene& scene)
{
    Json clocks(juce::Array<Json>{});
    for(const auto& clock:scene.clocks) {
        auto item=jsonObject(); property(item,"sample",clock.sample); property(item,"ppq",std::isfinite(clock.ppq)?Json(clock.ppq):Json("NaN"));
        property(item,"bpm",std::isfinite(clock.bpm)?Json(clock.bpm):Json("NaN")); property(item,"present",clock.present); property(item,"playing",clock.playing);
        property(item,"bpm_supplied",clock.tempo); property(item,"ppq_supplied",clock.position); property(item,"looping",clock.looping); property(item,"loop_points_supplied",clock.loopPoints);
        property(item,"loop_start",clock.loopStart); property(item,"loop_end",std::isfinite(clock.loopEnd)?Json(clock.loopEnd):Json("NaN"));
        property(item,"numerator",clock.numerator); property(item,"denominator",clock.denominator); clocks.append(item);
    }
    return clocks;
}
Json eventJson(const std::vector<Event>& events)
{
    Json array(juce::Array<Json>{});
    for(const auto& event:events) {
        auto item=jsonObject(); property(item,"sample",event.sample); property(item,"on",event.message.isNoteOn());
        property(item,"pitch",event.message.getNoteNumber()); property(item,"velocity",int(event.message.getVelocity())); property(item,"channel",event.message.getChannel()); array.append(item);
    }
    return array;
}
Json verifyPatternRegistry(juce::AudioPluginInstance& instance)
{
    Json descriptors(juce::Array<Json>{});
    std::map<juce::String,juce::AudioProcessorParameter*> hosted;
    for(auto* parameter:instance.getParameters())
        if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter))
            require(hosted.emplace(named->getParameterID(),parameter).second,"Duplicate hosted ParamID");
    for(const auto& d:airack::parameterRegistry()) {
        const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(d.id,true));
        auto* found=hosted.contains(id)?hosted.at(id):nullptr;
        require(found!=nullptr && found->getName(256)==d.name && found->getLabel()==d.unit,"Hosted descriptor identity/meaning changed");
        require(found->isAutomatable()==d.automatable,"Hosted automation flag changed");
        const juce::NormalisableRange<float> range(d.minimum,d.maximum,0,d.skew);
        require(std::abs(found->getDefaultValue()-range.convertTo0to1(d.defaultValue))<1e-6f,"Hosted default changed");
        auto entry=jsonObject(); property(entry,"id",d.id); property(entry,"vst3_id",id); property(entry,"name",found->getName(256)); property(entry,"unit",found->getLabel()); property(entry,"default",found->getDefaultValue()); property(entry,"automatable",found->isAutomatable()); descriptors.append(entry);
    }
    require(descriptors.size()==airack::parameterCount,"Expected every current owned control (first758 retain identity)"); return descriptors;
}
struct SceneRendered { Rendered render; juce::MemoryBlock initialOpaque; Json callbacks {juce::Array<Json>{}}; Json registry; };
SceneRendered renderPatternScene(juce::VST3PluginFormat& format,const juce::PluginDescription& description,
    const PatternScene& scene,int blockSize,bool external,bool native,bool suppress,const juce::MemoryBlock* recalled=nullptr)
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(),"Pattern lifecycle requires message thread");
    struct Playhead final:juce::AudioPlayHead { juce::Optional<PositionInfo> current; juce::Optional<PositionInfo> getPosition() const override {return current;} } playhead;
    std::unique_ptr<juce::AudioProcessor> processor;
    if(native) processor=std::make_unique<airack::InstrumentProcessor>();
    else { juce::String error; processor=format.createInstanceFromDescription(description,scene.rate,blockSize,error); }
    require(processor!=nullptr && processor->getTotalNumInputChannels()==0 && processor->getTotalNumOutputChannels()==2 && processor->acceptsMidi(),"Pattern instance creation/identity failed");
    processor->setNonRealtime(true); processor->setPlayHead(&playhead);
    auto expected=scene.project; if(external)expected.mode=airack::PlaybackMode::externalMidi;
    auto restore=[&](const airack::ProjectDocument& document) {
        juce::MemoryBlock owned; require(airack::encodeProjectState(document,owned),"Scene project encoding failed");
        const auto bytes=native?owned:hostEnvelope(owned); processor->setStateInformation(bytes.getData(),int(bytes.getSize()));
    };
    if(recalled)processor->setStateInformation(recalled->getData(),int(recalled->getSize())); else restore(expected);
    SceneRendered result;
    if(!native) result.registry=verifyPatternRegistry(*dynamic_cast<juce::AudioPluginInstance*>(processor.get()));
    auto verify=[&] {
        processor->getStateInformation(result.render.hostState);
        if(native) { airack::ProjectDocument actual; require(airack::decodeProjectState(result.render.hostState.getData(),result.render.hostState.getSize(),actual)&&actual==expected,"Native reference project mismatch"); }
        else verifyReturnedProject(result.render.hostState,expected);
    };
    verify(); result.initialOpaque=result.render.hostState;
    // Invalid full recall must leave the complete accepted project untouched.
    juce::MemoryBlock damaged; require(airack::encodeProjectState(expected,damaged),"Malformed-recall setup failed");
    static_cast<char*>(damaged.getData())[0]^=1;
    const auto rejected=native?damaged:hostEnvelope(damaged);
    processor->setStateInformation(rejected.getData(),int(rejected.getSize())); verify();
    processor->prepareToPlay(scene.rate,blockSize);
    result.render.audio.setSize(2,scene.samples); result.render.audio.clear();
    juce::AudioBuffer<float> block(2,blockSize); juce::MidiBuffer midi; midi.ensureSize(65536);
    std::size_t clockIndex=0,eventIndex=0,recallIndex=0;
    for(int sample=0;sample<scene.samples;) {
        while(clockIndex+1<scene.clocks.size() && scene.clocks[clockIndex+1].sample==sample)++clockIndex;
        while(recallIndex<scene.recalls.size() && scene.recalls[recallIndex].sample==sample) {
            expected=scene.recalls[recallIndex++].project; if(external)expected.mode=airack::PlaybackMode::externalMidi;
            restore(expected); verify();
        }
        int end=std::min(sample+blockSize,scene.samples);
        if(clockIndex+1<scene.clocks.size())end=std::min(end,scene.clocks[clockIndex+1].sample);
        if(recallIndex<scene.recalls.size())end=std::min(end,scene.recalls[recallIndex].sample);
        const int count=end-sample; require(count>0,"Invalid scene boundary order");
        // External references release by the literal MIDI offs. A host stop resets
        // the accepted External engine, which would erase its ordinary release tail.
        const PatternClock continuousReference;
        playhead.current=patternPosition(external?continuousReference:scene.clocks[clockIndex],sample,scene.rate);
        block.setSize(2,count,false,false,true); block.clear(); midi.clear();
        while(eventIndex<scene.expected.size() && scene.expected[eventIndex].sample<end) {
            const auto& event=scene.expected[eventIndex++];
            if(external || expected.mode==airack::PlaybackMode::externalMidi)midi.addEvent(event.message,event.sample-sample);
        }
        if(suppress && expected.mode==airack::PlaybackMode::internalPattern) {
            midi.addEvent(juce::MidiMessage::noteOn(3,99,juce::uint8(127)),0);
            midi.addEvent(juce::MidiMessage::controllerEvent(1,123,0),count-1);
        }
        processor->processBlock(block,midi);
        for(int ch=0;ch<2;++ch)result.render.audio.copyFrom(ch,sample,block,ch,0,count);
        auto callback=jsonObject(); property(callback,"sample",sample); property(callback,"count",count); property(callback,"clock_segment",int(clockIndex));
        property(callback,"clock_role",external?"continuous External literal/native reference":"actual Internal scene clock");
        property(callback,"position_present",playhead.current.hasValue());
        if(playhead.current) {
            const auto& position=*playhead.current;
            property(callback,"time_in_samples",*position.getTimeInSamples()); property(callback,"playing",position.getIsPlaying()); property(callback,"looping",position.getIsLooping());
            const auto bpm=position.getBpm(),ppq=position.getPpqPosition();
            property(callback,"bpm_supplied",bpm.hasValue()); property(callback,"ppq_supplied",ppq.hasValue());
            property(callback,"bpm",bpm?(std::isfinite(*bpm)?Json(*bpm):Json("NaN")):Json());
            property(callback,"ppq",ppq?(std::isfinite(*ppq)?Json(*ppq):Json("NaN")):Json());
        }
        result.callbacks.append(callback);
        sample=end;
    }
    require(eventIndex==scene.expected.size() && recallIndex==scene.recalls.size(),"Undelivered scene events"); verify();
    processor->releaseResources(); processor->setPlayHead(nullptr);
    return result;
}

struct PatternArguments { Arguments host; juce::File fixtures; bool smoke=false; };
PatternArguments parsePatternArguments(const juce::StringArray& args)
{
    require(admitPatternArguments(args),"Usage: --patterns|--patterns-smoke --plugin ABS_VST3 --output FRESH_ABS_WORKSPACE_DIRECTORY --profile-fixtures ABS_WORKSPACE_DIRECTORY");
    PatternArguments result {parseArguments({"--plugin",args[2],"--output",args[4]}),juce::File(args[6]),args[0]=="--patterns-smoke"};
    require(!result.host.output.exists() && !fs::exists(fs::symlink_status(fs::path(result.host.output.getFullPathName().toWideCharPointer()))),"Pattern output must be fresh");
    require(juce::File::isAbsolutePath(args[6]) && result.fixtures.isDirectory(),"Profile fixtures must be an absolute existing directory");
    const auto root=fs::weakly_canonical(fs::path(juce::File(RACKSYNTH_WORKSPACE).getFullPathName().toWideCharPointer()));
    const auto relative=fs::weakly_canonical(fs::path(result.fixtures.getFullPathName().toWideCharPointer())).lexically_relative(root);
    require(!relative.empty() && relative!="." && !relative.is_absolute() && *relative.begin()!="..","Profile fixtures must be inside workspace");
    return result;
}
airack::ProjectDocument loadProfileFixture(const juce::File& directory,bool dense)
{
    const auto file=directory.getChildFile(dense?"dense.aiproject":"typical.aiproject");
    require(file.existsAsFile() && file.getSize()>=juce::int64(airack::minimumProjectStateBytes)
        && file.getSize()<=juce::int64(airack::maximumProjectStateBytes),"Profile fixture size outside strict bounds");
    juce::MemoryBlock bytes; airack::ProjectDocument project;
    require(file.loadFileAsData(bytes) && airack::decodeProjectState(bytes.getData(),bytes.getSize(),project),"Strict profile fixture decode failed");
    require(project.mode==airack::PlaybackMode::internalPattern && project.pattern.bars==1
        && project.pattern.meter==airack::PhraseMeter{4,4} && project.pattern.noteCount==(dense?256:16)
        && project.pattern.swing==(dense?.7:0) && project.pattern.grid==(dense?airack::PatternGrid::sixtyFourth:airack::PatternGrid::sixteenth),"Wrong timing pattern fixture");
    for(int k=0;k<project.pattern.noteCount;++k) {
        const auto& note=project.pattern.notes[std::size_t(k)]; const double start=dense?k/64.0:0;
        require(note==airack::PatternNote{start,dense?std::min(.5,4-start):4,static_cast<std::uint8_t>(48+k%16),100,1},"Wrong literal timing note geometry");
    }
    const auto report=juce::JSON::parse(directory.getChildFile("pattern-profile-report.json"));
    require(report["suite"].toString()=="pattern" && report["configuration"].toString()=="Release" && report["fixtures"].isArray(),"Missing native fixture provenance");
    bool matched=false; const auto fixtureEntries=report["fixtures"];
    for(const auto& entry:*fixtureEntries.getArray())
        if(entry["path"].toString()==file.getFullPathName()) matched=entry["sha256"].toString()==hash(file) && juce::int64(entry["bytes"])==file.getSize();
    require(matched,"Native fixture identity mismatch");
    return project;
}
Measures measurePattern(const PatternScene& scene,const juce::AudioBuffer<float>& audio)
{
    if(!scene.silent)return measure(audio,audio.getNumSamples()-scene.rate/4);
    const auto result=measureWindow(audio,{{0,audio.getNumSamples()}});
    require(result.peak==0,"Empty Internal pattern must stay silent"); return {};
}
Json saveSceneRender(const juce::File& output,const juce::String& stem,const PatternScene& scene,const SceneRendered& rendered)
{
    const auto wav=output.getChildFile(stem+".wav"),opaque=output.getChildFile(stem+".hoststate");
    writeWav(wav,rendered.render.audio,scene.rate); writeBlock(opaque,rendered.render.hostState);
    const auto measures=measurePattern(scene,rendered.render.audio);
    auto item=jsonObject(); property(item,"wav",identity(wav)); property(item,"opaque",identity(opaque));
    property(item,"callbacks",rendered.callbacks); property(item,"peak",measures.peak); property(item,"rms",measures.rms); property(item,"tail_peak",measures.tailPeak);
    property(item,"float_wav_readback_exact",true); return item;
}
std::vector<Event> importedMidiEvents(const juce::File& file,int rate)
{
    auto input=file.createInputStream(); juce::MidiFile midi;
    require(input && midi.readFrom(*input) && midi.getTimeFormat()==960 && midi.getNumTracks()==1,"Standard MIDI import failed");
    std::vector<Event> result; const auto* track=midi.getTrack(0);
    for(int n=0;n<track->getNumEvents();++n) {
        const auto message=track->getEventPointer(n)->message;
        if(message.isNoteOnOrOff())result.push_back({beatSample(message.getTimeStamp()/960,rate),message});
    }
    return result;
}
Json exportMusicalMidi(const juce::File& output,const PatternScene& scene)
{
    const auto file=output.getChildFile(scene.name+"-"+juce::String(scene.rate)+".mid");
    require(airack::exportPatternMidi(scene.project.pattern,file),"Pattern MIDI export failed");
    juce::MemoryBlock bytes; require(file.loadFileAsData(bytes),"MIDI wire readback failed"); const auto wire=readPatternWire(bytes);
    const std::vector<WireNote> swing {{0,48,100,1,true},{240,48,0,1,false},{360,52,100,1,true},{480,55,100,1,true},{600,52,0,1,false},{720,55,0,1,false},{840,60,100,1,true},{1080,60,0,1,false}};
    const std::vector<WireNote> overlap {{0,60,100,1,true},{480,60,80,1,true},{960,60,0,1,false},{1440,60,0,1,false},{1440,60,70,1,true},{1680,60,0,1,false}};
    if(scene.name=="swing")require(wire.notes==swing && wire.end==3840 && wire.numerator==4 && wire.denominator==4,"Literal swing wire mismatch");
    if(scene.name=="same-key")require(wire.notes==overlap && wire.end==3840,"Literal overlap wire mismatch");
    Json events(juce::Array<Json>{}); for(const auto& note:wire.notes) {
        auto event=jsonObject(); property(event,"tick",note.tick); property(event,"on",note.on); property(event,"pitch",note.pitch); property(event,"velocity",note.velocity); property(event,"channel",note.channel); events.append(event);
    }
    auto item=jsonObject(); property(item,"file",identity(file)); property(item,"physical_order",events); property(item,"end_tick",wire.end);
    property(item,"meter_numerator",wire.numerator); property(item,"meter_denominator",wire.denominator);
    property(item,"meter_metronome_clocks",wire.metronome); property(item,"meter_thirty_seconds",wire.thirtySeconds);
    property(item,"overlap_limit","SMF has individual note records; identity-free importer pairing is not the runtime same-key ownership oracle"); return item;
}
Json runMusicalScene(juce::VST3PluginFormat& format,const juce::PluginDescription& description,const juce::File& output,const PatternScene& scene)
{
    const auto stem=scene.name+"-"+juce::String(scene.rate);
    auto report=jsonObject(); property(report,"scene",scene.name); property(report,"sample_rate",scene.rate); property(report,"clocks",clockJson(scene)); property(report,"literal_events",eventJson(scene.expected));
    juce::MemoryBlock project; require(airack::encodeProjectState(scene.project,project),"Scene project encode failed");
    const auto projectFile=output.getChildFile(stem+".aiproject"); writeBlock(projectFile,project); property(report,"project",identity(projectFile));
    const auto baseline=renderPatternScene(format,description,scene,512,false,false,false);
    property(report,"registry",baseline.registry); property(report,"baseline512",saveSceneRender(output,stem+"-internal512",scene,baseline));
    const auto compare=[&](const char* name,const SceneRendered& current) {
        auto item=saveSceneRender(output,stem+"-"+name,scene,current); const auto delta=difference(baseline.render.audio,current.render.audio);
        property(item,"maximum_absolute_difference",delta); property(report,name,item);
        writePatternJson(output.getChildFile(stem+"-"+name+"-scene.json"),report); // fresh partial diagnostic before any failing comparison
        require(delta<=1e-6,"Hosted musical reference/partition/recall mismatch");
    };
    compare("internal64",renderPatternScene(format,description,scene,64,false,false,false));
    compare("external512",renderPatternScene(format,description,scene,512,true,false,false));
    compare("suppressed512",renderPatternScene(format,description,scene,512,false,false,true));
    if(scene.recalls.empty())compare("opaque-recalled512",renderPatternScene(format,description,scene,512,false,false,false,&baseline.initialOpaque));
    if(scene.name=="straight" || scene.name=="full-recall-mode")compare("native-external512",renderPatternScene(format,description,scene,512,true,true,false));
    if(scene.name=="swing" || scene.name=="same-key") {
        property(report,"midi",exportMusicalMidi(output,scene));
        if(scene.name=="swing") {
            auto imported=scene; imported.expected=importedMidiEvents(output.getChildFile(stem+".mid"),scene.rate);
            compare("imported-external512",renderPatternScene(format,description,imported,512,true,false,false));
        }
    }
    // Save once after successful checks. Existing partial artifacts are kept on failures.
    const auto final=output.getChildFile(stem+"-complete.json"); writePatternJson(final,report); return report;
}
Json hostedPatternTrial(juce::VST3PluginFormat& format,const juce::PluginDescription& description,
    const airack::ProjectDocument& project,const juce::String& name,int rate,int blockSize,int trial,bool factoryTiming=false,
    DimensionCostTrace* diagnostic=nullptr,juce::AudioBuffer<float>* captured=nullptr)
{
    struct Playhead final:juce::AudioPlayHead { PositionInfo position; juce::Optional<PositionInfo> getPosition()const override{return position;} } playhead;
    juce::String error; auto instance=format.createInstanceFromDescription(description,rate,blockSize,error);
    require(instance!=nullptr,"Timing VST3 instance creation failed"); instance->setNonRealtime(true); instance->setPlayHead(&playhead);
    juce::MemoryBlock owned; require(airack::encodeProjectState(project,owned),"Timing project encode failed"); const auto envelope=hostEnvelope(owned);
    instance->setStateInformation(envelope.getData(),int(envelope.getSize())); juce::MemoryBlock returned; instance->getStateInformation(returned); verifyReturnedProject(returned,project);
    instance->prepareToPlay(rate,blockSize);
    if(factoryTiming)require(instance->getLatencySamples()==(project.sound.values[758]>=.5f?21:0),"Factory timing effective quality/latency mismatch");
    const int warmup=int(std::ceil(.25*rate/blockSize))*blockSize,frames=int(std::ceil(double(rate)/blockSize))*blockSize;
    const auto count=std::size_t(frames/blockSize); std::vector<juce::int64> raw(count); std::vector<double> peaks(count); std::vector<bool> finite(count,true);
    juce::AudioBuffer<float> audio(2,blockSize); juce::MidiBuffer midi; midi.ensureSize(65536);
    if(diagnostic)diagnostic->prepare(count);if(captured){captured->setSize(2,warmup+frames);captured->clear();}
    playhead.position.setBpm(120); playhead.position.setIsPlaying(true);
    for(int frame=0;frame<warmup+frames;frame+=blockSize) {
        playhead.position.setTimeInSamples(frame); playhead.position.setPpqPosition(double(frame)*2/rate); midi.clear(); audio.clear();
        if(frame<warmup){instance->processBlock(audio,midi);if(captured)for(int channel=0;channel<2;++channel)captured->copyFrom(channel,frame,audio,channel,0,blockSize);continue;}
        const auto n=std::size_t((frame-warmup)/blockSize);
        if(diagnostic)raw[n]=diagnostic->measure([&]{instance->processBlock(audio,midi);});
        else{const auto start=std::chrono::steady_clock::now(); instance->processBlock(audio,midi);
            raw[n]=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();}
        if(captured)for(int channel=0;channel<2;++channel)captured->copyFrom(channel,frame,audio,channel,0,blockSize);
        for(int c=0;c<2;++c)for(int s=0;s<blockSize;++s){const double value=audio.getSample(c,s);finite[n]=finite[n]&&std::isfinite(value);peaks[n]=std::max(peaks[n],std::abs(value));}
    }
    instance->getStateInformation(returned); verifyReturnedProject(returned,project); instance->releaseResources(); instance->setPlayHead(nullptr);
    const double deadline=double(blockSize)*1e9/rate,mean=std::accumulate(raw.begin(),raw.end(),0.0)/count;
    auto sorted=raw; std::sort(sorted.begin(),sorted.end()); const double p99=double(sorted[std::size_t(std::ceil(.99*count))-1]);
    Json durations(juce::Array<Json>{}),observations(juce::Array<Json>{}); bool allFinite=true; double peak=0;
    for(std::size_t n=0;n<count;++n){durations.append(raw[n]);auto item=jsonObject();property(item,"finite",bool(finite[n]));property(item,"peak",peaks[n]);observations.append(item);allFinite=allFinite&&finite[n];peak=std::max(peak,peaks[n]);}
    auto report=jsonObject(); property(report,"pattern",name); property(report,"sample_rate",rate); property(report,"block_size",blockSize); property(report,"trial",trial);
    if(factoryTiming){property(report,"selected_quality",project.sound.values[758]>=.5f?"higher":"realtime");property(report,"host_latency_samples",instance->getLatencySamples());}
    property(report,"warmup_frames",warmup);property(report,"measured_frames",frames);property(report,"callback_ns",durations);property(report,"audio_observations",observations);
    if(diagnostic){require(diagnostic->rows.size()==raw.size(),"Complete steady diagnostic trace required");for(std::size_t n=0;n<raw.size();++n)require(diagnostic->rows[n].wallNs==raw[n],"Steady diagnostic wall trace mismatch");property(report,"thread_accounting",diagnostic->json());}
    property(report,"fixture_sha256",juce::SHA256(owned).toHexString());property(report,"mean_ns",mean);property(report,"p99_ns",p99);property(report,"max_ns",double(sorted.back()));property(report,"deadline_ns",deadline);
    property(report,"deadline_miss_count",int(std::count_if(raw.begin(),raw.end(),[&](auto value){return value>deadline;})));
    property(report,"peak",peak);property(report,"finite",allFinite);property(report,"integrity",allFinite&&peak>0&&peak<1);property(report,"target_met",mean<=.75*deadline&&p99<=deadline); return report;
}
int renderPatterns(const PatternArguments& args)
{
    const auto typical=loadProfileFixture(args.fixtures,false),dense=loadProfileFixture(args.fixtures,true);
    require(typical.sound==dense.sound,"Timing fixtures use different sounds");
    require(args.host.output.createDirectory().wasOk(),"Cannot create fresh pattern output");
    juce::VST3PluginFormat format; juce::OwnedArray<juce::PluginDescription> descriptions;format.findAllTypesForFile(descriptions,args.host.plugin.getFullPathName());
    require(descriptions.size()==1&&descriptions[0]->isInstrument,"Expected exactly one VST3 instrument");
    const auto binary=binaryIn(args.host.plugin),helper=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto binaryHash=hash(binary),helperHash=hash(helper);auto report=jsonObject();property(report,"schema",1);property(report,"suite",args.smoke?"patterns-smoke":"patterns");
    property(report,"plugin_before",identity(binary));property(report,"helper_before",identity(helper));property(report,"renderer_source",identity(juce::File(RACKSYNTH_WORKSPACE).getChildFile("tools/native/RenderFixtures.cpp")));
    property(report,"clock_limit","JUCE adapter represents full absent PositionInfo as stopped/sample0/omitted BPM and PPQ; supplied positions always include timeInSamples; product-native status is not observed");
    property(report,"mode_limit","Portable mode changes use full project recall/reset; ordinary mode-change tails are accepted native processor/editor evidence");
    Json scenes(juce::Array<Json>{}),trials(juce::Array<Json>{});bool integrity=true,target=true;
    for(const auto& scene:musicalScenes(48000,args.smoke)){scenes.append(runMusicalScene(format,*descriptions[0],args.host.output,scene));std::cout<<"musical "<<scene.name<<" passed"<<std::endl;}
    if(!args.smoke) {
        for(int rate:{44100,96000})scenes.append(runMusicalScene(format,*descriptions[0],args.host.output,musicalScenes(rate,true)[0]));
        // Exact v1/v2 fixtures plus current v3 are checked through the historical sound-only entrypoint.
        for(const auto* name:{"m1-analog-pluck.soundstate","m2-analog-pluck.v2.soundstate","current-v3"}) {
            airack::SoundDocument document;juce::MemoryBlock bytes;juce::File input;
            if(juce::String(name)=="current-v3"){document.values=fixtureValues(true);require(airack::encodeSoundState(document,bytes),"Current v3 encode failed");}
            else {input=juce::File(RACKSYNTH_WORKSPACE).getChildFile("tests/fixtures/"+juce::String(name));require(input.loadFileAsData(bytes)&&airack::decodeSoundState(bytes.getData(),bytes.getSize(),document),"Exact legacy decode failed");}
            const auto rendered=render(format,*descriptions[0],48000,512,hostEnvelope(bytes),document,airack::currentSoundStateSize);
            const auto wav=args.host.output.getChildFile(juce::String(name)+".wav");writeWav(wav,rendered.audio,48000);writeBlock(args.host.output.getChildFile(juce::String(name)+".hoststate"),rendered.hostState);
            auto item=jsonObject();property(item,"legacy",name);if(input.existsAsFile())property(item,"input",identity(input));property(item,"wav",identity(wav));property(item,"empty_external_project",true);scenes.append(item);
        }
        require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Actual host timing requires Release");int number=0;
        for(bool isDense:{false,true})for(const auto geometry:std::array<std::pair<int,int>,3>{{{48000,64},{48000,512},{96000,512}}})for(int trial=1;trial<=3;++trial) {
            auto result=hostedPatternTrial(format,*descriptions[0],isDense?dense:typical,isDense?"dense":"typical",geometry.first,geometry.second,trial);
            integrity=integrity&&bool(result["integrity"]);target=target&&bool(result["target_met"]);writePatternJson(args.host.output.getChildFile("host-trial-"+juce::String(++number).paddedLeft('0',2)+".json"),result);trials.append(result);
            std::cout<<number<<"/18 hosted timing target="<<bool(result["target_met"])<<std::endl;
        }
    }
    property(report,"scenes",scenes);property(report,"timing_trials",trials);property(report,"timed_work","Prepared hosted processBlock wrapper only plus steady_clock overhead; clearing/position/scan/state/JSON outside intervals");
    property(report,"plugin_after",identity(binary));property(report,"helper_after",identity(helper));const bool stable=hash(binary)==binaryHash&&hash(helper)==helperHash;
    property(report,"identities_unchanged",stable);property(report,"integrity",integrity&&stable);property(report,"targets_met",target);
    property(report,"scope",args.smoke?"Three 48k musical scenes only; straight partition/opaque recall/suppression, swing/rawSMF/import, missingPPQ; no timing trials":"Current literal musical/state/registry/export fixtures plus18 prepared hosted timing trials; measured cases only, no DAW/listening/universal stability claim");
    writePatternJson(args.host.output.getChildFile("pattern-render-report.json"),report);return integrity&&stable&&target?0:1;
}

juce::MemoryBlock legacyProjectFixture(const airack::ProjectDocument& project)
{
    // Independent frozen first-758 Sound3/4 and Project1/2/3 geometry. Current
    // product writers are checked separately; their bytes cannot prove old hashes.
    juce::MemoryBlock modernSound,modernProject;
    require(airack::encodeSoundState(project.sound,modernSound)&&airack::encodeProjectState(project,modernProject),"Legacy projection current input invalid");
    require(project.sound.values[758]==0&&project.sound.compatibility==airack::KernelCompatibility{},"Legacy projection cannot discard mode/revisions");
    const auto defaults=airack::defaultParameterValues();for(std::size_t i=airack::eqParameterStart;i<airack::parameterCount;++i)require(project.sound.values[i]==defaults[i],"Legacy projection cannot discard EQ values");
    for(const auto& effect:project.sound.layout.effects)require(effect.kind!=airack::EffectKind::eq&&effect.kind!=airack::EffectKind::filter&&effect.kind!=airack::EffectKind::chorus&&effect.kind!=airack::EffectKind::phaser&&effect.kind!=airack::EffectKind::flanger&&effect.kind!=airack::EffectKind::stereo,"Legacy projection cannot discard appended effect kinds");
    juce::MemoryBlock numeric(modernSound.getData(),6080);
    numeric.append(static_cast<const char*>(modernSound.getData())+16+8*airack::parameterCount,78+64+758);
    numeric.append(static_cast<const char*>(modernSound.getData())+airack::currentSoundNumericBytes-12,12);
    require(numeric.getSize()==6992,"Frozen numeric projection size changed");
    auto* header=static_cast<unsigned char*>(numeric.getData());header[4]=project.sound.personalities?4:3;header[8]=246;header[9]=2;
    juce::MemoryOutputStream sound;sound.write(numeric.getData(),numeric.getSize());
    if(project.sound.personalities){
        juce::MemoryOutputStream personalities;personalities.writeShort(1);personalities.writeByte(static_cast<char>(project.sound.personalities->instances.size()));personalities.writeByte(0);
        for(const auto& instance:project.sound.personalities->instances){
            juce::var design;require(airack::encodeRackDesignObject(instance.design,design,airack::RackValidationMode::instanceRecall),"Legacy design projection invalid");design.getDynamicObject()->setProperty("version",1);
            for(auto& unit:*design["units"].getArray())unit.getDynamicObject()->removeProperty("algorithm_revision");
            const auto json=juce::JSON::toString(design,true,std::numeric_limits<double>::max_digits10+5).toStdString();
            personalities.write(instance.id.getRawData(),16);personalities.writeByte(instance.edited?1:0);personalities.writeByte(static_cast<char>(instance.bindings.size()));
            for(auto binding:instance.bindings){personalities.writeByte(static_cast<char>(binding.unit));personalities.writeByte(static_cast<char>(binding.slot));}
            personalities.writeInt(static_cast<int>(json.size()));personalities.write(json.data(),json.size());
        }
        sound.writeInt(static_cast<int>(personalities.getDataSize()));sound.write(personalities.getData(),personalities.getDataSize());
    }
    const auto sd=juce::SHA256(sound.getData(),sound.getDataSize()).getRawData();sound.write(sd.getData(),32);
    juce::MemoryBlock old(modernProject.getData(),20);auto* ph=static_cast<unsigned char*>(old.getData());ph[4]=project.sound.personalities?3:project.group?2:1;
    for(int n=0;n<4;++n)ph[12+n]=static_cast<unsigned char>((sound.getDataSize()>>(8*n))&255);
    old.append(sound.getData(),sound.getDataSize());const auto remaining=modernProject.getSize()-20-modernSound.getSize()-32;
    old.append(static_cast<const char*>(modernProject.getData())+20+modernSound.getSize(),remaining);
    const auto pd=juce::SHA256(old).getRawData();old.append(pd.getData(),32);return old;
}

// BEGIN M3 RENDER SELF TEST -- freeze whole region for RED/GREEN.
int patternRenderSelfTest()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL M3: " << label << '\n'; } };
    auto accepts = [&](auto operation, const char* label) { try { operation(); check(true, label); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; check(false, label); } };
    auto rejects = [&](auto operation, const char* label) { bool caught = false; try { operation(); } catch (const std::exception&) { caught = true; } check(caught, label); };
    airack::ProjectDocument project;
    for (int count : {0, 256})
    {
        project.pattern.noteCount = static_cast<std::uint16_t>(count);
        for (int k = 0; k < count; ++k) project.pattern.notes[std::size_t(k)] = { k / 64.0, std::min(.5, 4 - k / 64.0), static_cast<std::uint8_t>(48 + k % 16), 100, 1 };
        project.mode = count == 0 ? airack::PlaybackMode::externalMidi : airack::PlaybackMode::internalPattern;
        juce::MemoryBlock owned; check(airack::encodeProjectState(project, owned), "project fixture encode");
        check(owned.getSize() == std::size_t(count == 0 ? 41161 : 46025), "independent current canonical/minimum and maximum sizes");
        const auto legacy=legacyProjectFixture(project);airack::ProjectDocument oldDecoded;
        check(legacy.getSize()==std::size_t(count==0?7132:11996)&&airack::decodeProjectState(legacy.getData(),legacy.getSize(),oldDecoded)&&oldDecoded==project,"frozen legacy canonical/minimum and maximum decode unchanged");
        auto component = owned; const unsigned char trailer[] {0xDE, 0xAD, 0xBE, 0xEF, 0x17}; component.append(trailer, sizeof(trailer));
        const auto opaque = hostEnvelope(component); const auto preserved = opaque;
        accepts([&] { verifyReturnedProject(opaque, project); }, "opaque project prefix accepted with distinct trailer");
        check(opaque == preserved, "verifier preserves every opaque byte");
        airack::ProjectDocument decoded; check(!airack::decodeProjectState(component.getData(), component.getSize(), decoded), "strict owned codec still rejects trailer");
        auto wrong = project; wrong.mode = project.mode == airack::PlaybackMode::externalMidi ? airack::PlaybackMode::internalPattern : airack::PlaybackMode::externalMidi;
        rejects([&] { verifyReturnedProject(opaque, wrong); }, "wrong mode rejected");
        wrong = project; wrong.pattern.swing = .5; rejects([&] { verifyReturnedProject(opaque, wrong); }, "wrong pattern rejected");
        wrong = project; wrong.sound.values[0] = .123f; rejects([&] { verifyReturnedProject(opaque, wrong); }, "wrong sound rejected");
        for (const std::size_t offset : {std::size_t(0), std::size_t(12), owned.getSize() - 1}) {
            auto corrupt = component; static_cast<unsigned char*>(corrupt.getData())[offset] ^= 1;
            rejects([&] { verifyReturnedProject(hostEnvelope(corrupt), project); }, "corrupt magic/header/digest rejected");
        }
        auto truncated = owned; truncated.setSize(owned.getSize() - 1);
        rejects([&] { verifyReturnedProject(hostEnvelope(truncated), project); }, "truncated component rejected");
    }
    for (const auto* tag : {"WrongTag", "VST3PluginState"}) {
        juce::XmlElement xml(tag); juce::MemoryBlock bytes; juce::AudioProcessor::copyXmlToBinary(xml, bytes);
        rejects([&] { verifyReturnedProject(bytes, project); }, "missing/wrong XML component rejected");
    }
    juce::XmlElement invalid("VST3PluginState"); invalid.createNewChildElement("IComponent")->addTextElement("not-base64!!!");
    juce::MemoryBlock bad; juce::AudioProcessor::copyXmlToBinary(invalid, bad);
    rejects([&] { verifyReturnedProject(bad, project); }, "invalid base64 rejected");
    PatternClock clock; clock.sample = 12000; clock.ppq = 1.5;
    auto p = patternPosition(clock, 18000, 48000);
    check(p.hasValue() && p->getTimeInSamples() == juce::Optional<juce::int64>(18000) && p->getPpqPosition() == juce::Optional<double>(1.75), "literal seek sample bookkeeping");
    clock.tempo = false; p = patternPosition(clock, 18000, 48000);
    check(p.hasValue() && !p->getBpm().hasValue() && p->getPpqPosition().hasValue() && p->getIsPlaying(), "missing BPM stays optional while playing");
    clock.tempo = true; clock.position = false; p = patternPosition(clock, 18000, 48000);
    check(p.hasValue() && p->getBpm().hasValue() && !p->getPpqPosition().hasValue() && p->getTimeInSamples().hasValue(), "missing PPQ stays optional with sample time");
    clock.position = true; clock.looping = true; clock.loopPoints = true; clock.ppq = .5; clock.sample = 0;
    p = patternPosition(clock, 24000, 48000);
    check(p.hasValue() && p->getPpqPosition() == juce::Optional<double>(.5) && p->getLoopPoints().hasValue(), "literal exact host wrap endpoint");
    clock.loopPoints = false; p = patternPosition(clock, 24000, 48000);
    check(p.hasValue() && p->getIsLooping() && !p->getLoopPoints().hasValue(), "missing loop points not invented");
    clock.present = false; check(!patternPosition(clock, 0, 48000).hasValue(), "full absent clock retained at helper boundary");
    airack::PatternDocument swing; swing.swing = 1; swing.noteCount = 4;
    constexpr std::array<int,4> pitches {48,52,55,60};
    for (int k = 0; k < 4; ++k) swing.notes[std::size_t(k)] = {k * .25, .25, static_cast<std::uint8_t>(pitches[std::size_t(k)]),100,1};
    const std::vector<WireNote> expected {{0,48,100,1,true},{240,48,0,1,false},{360,52,100,1,true},{480,55,100,1,true},{600,52,0,1,false},{720,55,0,1,false},{840,60,100,1,true},{1080,60,0,1,false}};
    accepts([&] { juce::MemoryBlock midi; require(airack::encodePatternMidi(swing,midi),"SMF encode"); const auto wire = readPatternWire(midi); require(wire.notes == expected && wire.end == 3840 && wire.numerator == 4 && wire.denominator == 4,"literal physical swing wire table"); }, "raw SMF swing onset-only table");
    airack::PatternDocument adjacent; adjacent.noteCount = 3;
    adjacent.notes[0] = {0,.25,60,100,1}; adjacent.notes[1] = {.25,.25,60,90,1}; adjacent.notes[2] = {1,.00001,62,70,2};
    const std::vector<WireNote> adjacentExpected {{0,60,100,1,true},{240,60,0,1,false},{240,60,90,1,true},{480,60,0,1,false},{960,62,70,2,true},{961,62,0,2,false}};
    accepts([&] { juce::MemoryBlock midi; require(airack::encodePatternMidi(adjacent,midi),"SMF encode"); require(readPatternWire(midi).notes == adjacentExpected,"literal adjacent and subtick wire table"); }, "physical off-before-on and subtick one-tick minimum");
    check(admitPatternArguments({"--patterns","--plugin","fixture","--output","fixture","--profile-fixtures","fixture"}), "new CLI shape accepted");
    check(admitPatternArguments({"--patterns-smoke","--plugin","fixture","--output","fixture","--profile-fixtures","fixture"}), "bounded parent smoke CLI shape accepted");
    check(!admitPatternArguments({"--patterns","--plugin","fixture","--output","fixture"}), "missing profile fixture argument rejected");
    std::cout << "M3 render helper " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
// END M3 RENDER SELF TEST

using namespace airack;
#define CHECK(x) require(bool(x), #x)
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

#undef CHECK
airack::ProjectDocument personalityComponent(const juce::MemoryBlock& opaque)
{
    const auto xml=juce::AudioProcessor::getXmlFromBinary(opaque.getData(),int(opaque.getSize()));require(xml&&xml->hasTagName("VST3PluginState"),"Personality host envelope missing");
    const auto* child=xml->getChildByName("IComponent");juce::MemoryBlock bytes;require(child&&bytes.fromBase64Encoding(child->getAllSubText()),"Actual IComponent missing");
    require(bytes.getSize()<=airack::maximumProjectStateBytes+65536,"Hosted component exceeds bounded owned/private state");
    std::size_t owned=bytes.getSize();constexpr std::string_view magic="JUCEPrivateData";const auto* data=static_cast<const char*>(bytes.getData());
    if(owned>=magic.size()+16&&std::string_view(data+owned-magic.size(),magic.size())==magic){
        juce::MemoryInputStream length(data+owned-magic.size()-8,8,false);const auto privateSize=length.readInt64();require(privateSize>=0&&std::uint64_t(privateSize)<=owned-magic.size()-16,"Invalid JUCE private suffix length");
        owned-=magic.size()+16+std::size_t(privateSize);for(int n=0;n<8;++n)require(data[owned+std::size_t(n)]==0,"JUCE suffix padding corrupted");
        if(privateSize>0){auto tree=juce::ValueTree::readFromData(data+owned+8,std::size_t(privateSize));require(tree.isValid()&&tree.hasType("JUCEPrivateData"),"JUCE private suffix malformed");}
    }
    airack::ProjectDocument result;require(airack::decodeProjectState(data,owned,result)&&airack::validateProjectDocument(result),"Complete actual owned project invalid");return result;
}
void verifyPersonalityProject(const juce::MemoryBlock& opaque,const airack::ProjectDocument& expected)
{require(personalityComponent(opaque)==expected,"Actual embedded personality project differs");}
void personalityPump()
{
#if JUCE_WINDOWS
    MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
#endif
}
SceneRendered renderPersonality(juce::VST3PluginFormat& format,const juce::PluginDescription& desc,
    const airack::ProjectDocument& wanted,int blockSize,const juce::MemoryBlock* recalled=nullptr,bool lifecycle=false)
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(),"Personality VST3 lifecycle must run on message thread");
    juce::String error;auto instance=format.createInstanceFromDescription(desc,48000,blockSize,error);require(instance!=nullptr,"Actual personality VST3 load failed");
    instance->setNonRealtime(true);juce::MemoryBlock owned;require(airack::encodeProjectState(wanted,owned),"Fixture encoding failed");const auto bytes=recalled?*recalled:hostEnvelope(owned);instance->setStateInformation(bytes.getData(),int(bytes.getSize()));
    SceneRendered result;result.registry=verifyPatternRegistry(*instance);auto verify=[&]{instance->getStateInformation(result.render.hostState);verifyPersonalityProject(result.render.hostState,wanted);};verify();result.initialOpaque=result.render.hostState;
    if(lifecycle){for(int cycle=0;cycle<2;++cycle){std::unique_ptr<juce::AudioProcessorEditor> editor(instance->createEditorIfNeeded());require(editor!=nullptr,"Actual loaded editor missing");editor->addToDesktop(0);editor->setVisible(true);for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(2200,1600)}){editor->setSize(size.x,size.y);personalityPump();require(editor->getWidth()>0&&editor->getHeight()>0,"Actual loaded editor resize failed");verify();}editor.reset();personalityPump();verify();}}
    instance->prepareToPlay(48000,blockSize);result.render.audio.setSize(2,288000);juce::AudioBuffer<float> audio(2,blockSize);juce::MidiBuffer midi;midi.ensureSize(4096);
    for(int offset=0;offset<288000;offset+=blockSize){const auto count=std::min(blockSize,288000-offset);audio.setSize(2,count,false,false,true);audio.clear();midi.clear();if(offset==0)midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(100)),0);if(offset<=48000&&offset+count>48000)midi.addEvent(juce::MidiMessage::noteOff(1,48),48000-offset);instance->processBlock(audio,midi);for(int c=0;c<2;++c)result.render.audio.copyFrom(c,offset,audio,c,0,count);}
    verify();instance->releaseResources();
    if(lifecycle){const auto& descriptor=airack::parameterRegistry()[756];const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));juce::AudioProcessorParameter* target=nullptr;for(auto* p:instance->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(p);named&&named->getParameterID()==id)target=p;require(target,"Hosted automation target missing");target->beginChangeGesture();target->setValueNotifyingHost(.25f);target->endChangeGesture();personalityPump();juce::MemoryBlock changed;instance->getStateInformation(changed);auto expected=wanted;expected.sound.values[756]=.25f;verifyPersonalityProject(changed,expected);instance->setStateInformation(bytes.getData(),int(bytes.getSize()));verify();
        if(wanted.sound.personalities&&wanted.sound.personalities->instances.back().design.role==airack::RackRole::synth){const auto& rack=wanted.sound.personalities->instances.back();const auto slot=airack::rackUnitBinding(rack,rack.design.units.front().id)->slot;const auto engine=airack::parameterRegistry()[13+4*slot];const auto engineId=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(engine.id,true));for(auto* p:instance->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(p);named&&named->getParameterID()==engineId)p->setValueNotifyingHost(rack.design.units.front().module==airack::NativeModule::analog?1.f:0.f);personalityPump();instance->getStateInformation(changed);const auto actual=personalityComponent(changed);require(actual.sound.personalities&&actual.sound.personalities->instances.back().id==rack.id&&!airack::rackControlActive(actual.sound,actual.sound.personalities->instances.back(),0),"Hosted engine automation lost retained disabled mapping");instance->setStateInformation(bytes.getData(),int(bytes.getSize()));verify();}
    }
    return result;
}
// Development-helper-only arbitrary captured-state replay. Existing fixture
// dispatches and their fixed120 timing remain unchanged.
struct CandidateInput
{
    juce::MemoryBlock original;
    airack::ProjectDocument project;
    bool soundOnly=false;
};
CandidateInput decodeCandidateBinary(const juce::MemoryBlock& original)
{
    require(original.getSize()>=4&&original.getSize()<=airack::maximumProjectStateBytes,"Candidate binary extent outside published bounds");
    const auto* bytes=static_cast<const char*>(original.getData());
    const bool sound=std::memcmp(bytes,"ARSI",4)==0,project=std::memcmp(bytes,"AIPJ",4)==0;
    require(sound||project,"Candidate is neither native Sound nor Project binary");
    require(!sound||original.getSize()<=airack::maximumSoundStateBytes,"Sound candidate exceeds its own codec bound");
    CandidateInput input;
    require(airack::decodeProjectState(original.getData(),original.getSize(),input.project)
        &&airack::validateProjectDocument(input.project),"Candidate complete Project validation failed");
    input.original=original;input.soundOnly=sound;return input;
}
CandidateInput readCandidateExtent(juce::InputStream& stream,juce::int64 extent)
{
    require(extent>=4&&extent<=juce::int64(airack::maximumProjectStateBytes),"Candidate input size outside published Project bound");
    require(stream.getTotalLength()==extent,"Candidate input changed before bounded read");
    juce::MemoryBlock bytes{std::size_t(extent)};
    require(stream.read(bytes.getData(),int(extent))==extent,"Candidate actual read truncated");
    char extra=0;require(stream.read(&extra,1)==0&&stream.getTotalLength()==extent,"Candidate input grew during read");
    return decodeCandidateBinary(bytes);
}
CandidateInput readCandidateInput(const juce::File& file)
{
    require(file.existsAsFile(),"Candidate input file missing");const auto extent=file.getSize();
    require(extent>=4&&extent<=juce::int64(airack::maximumProjectStateBytes),"Candidate input size outside published Project bound");
    juce::FileInputStream stream(file);require(stream.openedOk(),"Candidate input cannot be opened");
    return readCandidateExtent(stream,extent);
}
double candidateBpm(const airack::ProjectDocument& project,std::optional<double> requested)
{
    if(requested)require(std::isfinite(*requested)&&*requested>=20&&*requested<=400,"Candidate harness BPM outside supported bounds");
    const auto captured=project.group?project.group->bpm:std::optional<double>{};
    if(captured){require(std::isfinite(*captured)&&*captured>=20&&*captured<=400,"Captured group BPM invalid");
        require(!requested||*requested==*captured,"Harness BPM conflicts with captured group BPM");return *captured;}
    require(requested.has_value(),"Candidate has no saved BPM; supply an explicit controlled harness BPM");return *requested;
}
int candidateBeatFrame(double beats,int rate,double bpm)
{
    require(std::isfinite(beats)&&beats>=0&&rate>0&&std::isfinite(bpm)&&bpm>=20&&bpm<=400,"Invalid candidate beat/sample conversion");
    const auto frame=std::floor(beats*rate*60/bpm+.5);
    require(std::isfinite(frame)&&frame<=std::numeric_limits<int>::max()-4.0*rate,"Candidate frame arithmetic overflow");return int(frame);
}
juce::AudioPlayHead::PositionInfo candidatePosition(const airack::ProjectDocument& project,int sample,int bodyFrames,int rate,double bpm)
{
    const bool external=project.mode==airack::PlaybackMode::externalMidi;
    juce::AudioPlayHead::PositionInfo p;p.setTimeInSamples(sample);p.setBpm(bpm);
    p.setIsPlaying(external||sample<bodyFrames);p.setIsLooping(false);
    p.setPpqPosition(double(external?sample:std::min(sample,bodyFrames))*bpm/(60*rate));
    p.setTimeSignature(juce::AudioPlayHead::TimeSignature{project.pattern.meter.numerator,project.pattern.meter.denominator});return p;
}
airack::ProjectDocument candidateQualityVariant(const airack::ProjectDocument& original,airack::RenderQuality quality)
{
    auto variant=original;variant.sound.values[758]=quality==airack::RenderQuality::higher?1.f:0.f;
    require(airack::validateProjectDocument(variant),"Candidate quality variant invalid");return variant;
}
std::vector<airack::ParameterGestureValue> candidateSaturationAutomation(const airack::ProjectDocument& project)
{
    std::vector<airack::ParameterGestureValue> edits;
    for(std::size_t slot=0;slot<project.sound.layout.effects.size();++slot){const auto& effect=project.sound.layout.effects[slot];
        if(effect.use==airack::SlotUse::present&&effect.kind==airack::EffectKind::saturation)
            for(std::size_t field=0;field<35;++field)for(float value:{0.f,1.f})edits.push_back({airack::saturationParameterStart+slot*35+field,value});}
    return edits;
}
std::vector<std::size_t> candidateDynamicEqAutomation(const airack::ProjectDocument& project)
{
    constexpr auto eqExtensionEndpoint=airack::dynamicEqExtensionParameterStart+8*airack::dynamicEqExtensionParametersPerSlot-1;
    for(const auto& slot:project.sound.layout.effects)
        if(slot.use==airack::SlotUse::present&&slot.kind==airack::EffectKind::dynamicEq)
            return {airack::dynamicEqParameterStart,airack::dynamicEqGainParameterStart-1,airack::dynamicEqPlacementParameterStart-1,airack::delayTapeParameterStart-1,airack::delayStereoParameterStart-1,airack::dynamicEqSpectralParameterStart-1,airack::spaceReverbParameterStart-1};
    return {};
}
float candidateAutomationValue(std::size_t index,float current)
{
    return airack::isContinuousParameter(index)?(current==.25f?.75f:.25f):(current==0?1.f:0.f);
}
struct CandidateEqGroupTrial {std::vector<airack::ParameterGestureValue> edits;airack::ProjectDocument expected;};
CandidateEqGroupTrial candidateEqGroupTrial(const airack::ProjectDocument& project)
{
    CandidateEqGroupTrial trial;trial.expected=project;
    for(std::uint8_t slot=0;slot<8;++slot)if(project.sound.layout.effects[slot].use==airack::SlotUse::present&&project.sound.layout.effects[slot].kind==airack::EffectKind::dynamicEq){
        for(const int band:{1,8})for(const auto* field:{"active","frequency_hz","gain_db","q"}){
            const auto key="dynamic_eq.band."+juce::String(band)+"."+field;
            const auto index=airack::rackParameterIndex(airack::NativeModule::dynamicEq,slot,key);require(bool(index),"Grouped EQ host key missing");
            const auto old=project.sound.values[*index];const float value=juce::String(field)=="active"?(old>=.5f?0.f:1.f):(old==.25f?.75f:.25f);
            trial.edits.push_back({*index,value});trial.expected.sound.values[*index]=value;
        }
        require(airack::validateProjectDocument(trial.expected),"Grouped EQ host expectation invalid");return trial;
    }return trial;
}
struct CandidateEqGainTrial {std::vector<airack::ParameterGestureValue> edits;airack::ProjectDocument expected;};
CandidateEqGainTrial candidateEqGainTrial(const airack::ProjectDocument& project)
{
    CandidateEqGainTrial trial;trial.expected=project;
    for(std::uint8_t slot=0;slot<8;++slot)if(project.sound.layout.effects[slot].use==airack::SlotUse::present&&project.sound.layout.effects[slot].kind==airack::EffectKind::dynamicEq){
        for(const auto* key:{"dynamic_eq.auto_gain","dynamic_eq.gain_scale"}){
            const auto index=airack::rackParameterIndex(airack::NativeModule::dynamicEq,slot,key);require(bool(index),"EQ gain host key missing");
            const auto old=project.sound.values[*index];const float value=juce::String(key)=="dynamic_eq.auto_gain"?(old>=.5f?0.f:1.f):(old==.25f?.75f:.25f);
            trial.edits.push_back({*index,value});trial.expected.sound.values[*index]=value;
        }
        require(airack::validateProjectDocument(trial.expected),"EQ gain host expectation invalid");return trial;
    }return trial;
}
CandidateEqGroupTrial candidateEqPlacementTrial(const airack::ProjectDocument& project,int mode)
{
    require(mode>=0&&mode<5,"Placement host mode outside registered choices");CandidateEqGroupTrial trial;trial.expected=project;
    if(candidateDynamicEqAutomation(project).empty())return trial;
    for(const auto index:{airack::dynamicEqPlacementParameterStart,airack::delayTapeParameterStart-1}){
        const float value=float(mode)/4;trial.edits.push_back({index,value});trial.expected.sound.values[index]=value;
    }
    require(airack::validateProjectDocument(trial.expected),"Placement host expectation invalid");return trial;
}
CandidateEqGroupTrial candidateEqExtensionTrial(const airack::ProjectDocument& project,int shape,int slope,bool legacy=false)
{
    require(shape>=0&&shape<(legacy?6:4)&&slope>=0&&slope<(legacy?2:5),"EQ extension host choice outside fixed domains");
    CandidateEqGroupTrial trial;trial.expected=project;if(candidateDynamicEqAutomation(project).empty())return trial;
    const auto add=[&](std::size_t index,float value){trial.edits.push_back({index,value});trial.expected.sound.values[index]=value;};
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){
        const auto extension=airack::dynamicEqExtensionParameterStart+slot*airack::dynamicEqExtensionParametersPerSlot+band*airack::dynamicEqExtensionParametersPerBand;
        add(extension,legacy?0.f:float(shape)/3);add(extension+1,legacy?0.f:float(slope)/4);
        if(legacy){const auto original=airack::dynamicEqParameterStart+slot*airack::dynamicEqParametersPerSlot+band*airack::dynamicEqParametersPerBand;add(original+2,float(shape)/5);add(original+6,float(slope));}
    }
    require(airack::validateProjectDocument(trial.expected),"EQ extension host expectation invalid");return trial;
}
CandidateEqGroupTrial candidateEqDetectorTrial(const airack::ProjectDocument& project,int source,int flags)
{
    require(source>=0&&source<4&&flags>=0&&flags<4,"Detector host choices outside fixed domains");CandidateEqGroupTrial trial;trial.expected=project;
    if(candidateDynamicEqAutomation(project).empty())return trial;
    const auto add=[&](std::size_t index,float value){trial.edits.push_back({index,value});trial.expected.sound.values[index]=value;};
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto first=airack::dynamicEqDetectorParameterStart+slot*40+band*5;
        // Widen the upper limit before moving the lower one during ordinary host automation.
        add(first+4,.8f);add(first+3,.2f);add(first+2,float(source)/3);add(first,float(flags&1));add(first+1,(flags&2)?1.f:0.f);
    }
    require(airack::validateProjectDocument(trial.expected),"Detector host expectation invalid");return trial;
}
struct CandidateEngineTrial
{
    bool applicable=false;std::size_t index=0;float normalized=0;
    airack::ProjectDocument expected;juce::String reason;
};
CandidateEngineTrial candidateEngineTrial(const airack::ProjectDocument& project)
{
    require(airack::validateProjectDocument(project),"Candidate engine trial requires a complete valid Project");
    CandidateEngineTrial trial;trial.expected=project;std::optional<std::uint8_t> selected;
    // Prefer the first active owned synth in audible order. Ownership is keyed
    // by local-unit binding, independent of instance/unit vector permutation.
    if(project.sound.personalities)for(std::size_t order=0;order<project.sound.layout.synthCount&&!selected;++order){
        const auto slot=project.sound.layout.synthOrder[order];
        for(const auto& rack:project.sound.personalities->instances)for(const auto& unit:rack.design.units)
            if(unit.module==airack::NativeModule::analog||unit.module==airack::NativeModule::wavetable)
                if(const auto* binding=airack::rackUnitBinding(rack,unit.id);binding&&binding->slot==slot)selected=slot;
    }
    if(!selected&&project.sound.layout.synthCount>0)selected=project.sound.layout.synthOrder[0];
    if(!selected){trial.reason="NOT_APPLICABLE_NO_ACTIVE_SYNTH";return trial;}
    trial.index=13+4* *selected;trial.normalized=airack::parameterPhysicalValue(project.sound.values,trial.index)==0?1.f:0.f;
    auto proposed=project.sound;proposed.values[trial.index]=trial.normalized;
    if(project.sound.personalities){airack::SoundDocument reconciled;airack::RackDifference difference;
        require(airack::reconcileRackEdit(project.sound,proposed,{},airack::SoundEditOrigin::manual,reconciled,difference)==airack::RackCandidateStatus::ready,
            "Candidate active-engine automation coverage gap: public reconciliation cannot admit the opposite engine");
        trial.expected.sound=std::move(reconciled);
    }else trial.expected.sound=std::move(proposed);
    require(airack::validateProjectDocument(trial.expected),"Candidate engine automation expectation invalid");
    trial.applicable=true;trial.reason="APPLICABLE_ACTIVE_SYNTH_FULL_PROJECT_RECONCILIATION";return trial;
}
struct CandidateArguments {Arguments host;juce::File input,sidechain;std::optional<double> bpm;};
CandidateArguments parseCandidateArguments(const juce::StringArray& args)
{
    require((args.size()==7||args.size()==9||args.size()==11)&&args[0]=="--candidate"&&args[1]=="--plugin"&&args[3]=="--output"&&args[5]=="--input",
        "Usage: RackSynthRender --candidate --plugin ABS_VST3 --output FRESH_ABS_WORKSPACE_DIRECTORY --input ABS_NATIVE_BINARY [--bpm DECLARED_CONTROLLED_BPM] [--sidechain ABS_STEREO_48K_WAV]");
    CandidateArguments result;result.host=parseArguments({"--plugin",args[2],"--output",args[4]});
    require(juce::File::isAbsolutePath(args[6]),"Candidate input must be absolute");result.input=juce::File(args[6]);
    const auto workspace=fs::weakly_canonical(fs::path(juce::File(RACKSYNTH_WORKSPACE).getFullPathName().toWideCharPointer()));
    const auto input=fs::weakly_canonical(fs::path(result.input.getFullPathName().toWideCharPointer()));const auto relative=input.lexically_relative(workspace);
    require(!relative.empty()&&relative!="."&&!relative.is_absolute()&&*relative.begin()!="..","Candidate input escapes workspace");
    require(!result.host.output.exists()&&!fs::exists(fs::symlink_status(fs::path(result.host.output.getFullPathName().toWideCharPointer()))),"Candidate output must be fresh, including links");
    for(int i=7;i<args.size();i+=2){
        if(args[i]=="--bpm"){
            require(!result.bpm,"Duplicate candidate BPM");std::size_t used=0;const auto text=args[i+1].toStdString();
            const auto value=std::stod(text,&used);require(used==text.size()&&std::isfinite(value)&&value>=20&&value<=400,"Candidate BPM malformed or outside bounds");result.bpm=value;
        }else if(args[i]=="--sidechain"){
            require(result.sidechain==juce::File()&&juce::File::isAbsolutePath(args[i+1]),"Duplicate or relative candidate sidechain");result.sidechain=juce::File(args[i+1]);
            const auto path=fs::weakly_canonical(fs::path(result.sidechain.getFullPathName().toWideCharPointer())).lexically_relative(workspace);
            require(!path.empty()&&path!="."&&!path.is_absolute()&&*path.begin()!="..","Candidate sidechain escapes workspace");
        }else require(false,"Unknown candidate argument");
    }
    return result;
}
juce::AudioBuffer<float> readCandidateSidechain(const juce::File& file)
{
    constexpr juce::int64 maximumFrames=30*48000;
    require(file.existsAsFile()&&file.getSize()>0&&file.getSize()<=64*1024*1024,"Sidechain WAV missing or exceeds file bound");
    juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(),true));
    require(reader&&reader->numChannels==2&&reader->sampleRate==48000&&reader->lengthInSamples>0&&reader->lengthInSamples<=maximumFrames,
        "Sidechain must be stereo 48kHz WAV, 1 sample..30 seconds");
    juce::AudioBuffer<float> audio(2,int(reader->lengthInSamples));
    require(reader->read(&audio,0,audio.getNumSamples(),0,true,true),"Sidechain WAV read failed");
    for(int ch=0;ch<2;++ch)for(int n=0;n<audio.getNumSamples();++n)require(std::isfinite(audio.getSample(ch,n)),"Sidechain WAV contains nonfinite samples");
    return audio;
}
std::vector<Event> aiBriefEvents(const airack::listening::AiBrief& brief,double perBeat)
{
    std::vector<Event> events;
    for(std::size_t n=0;n<brief.phrase.noteCount;++n){const auto& note=brief.phrase.notes[n];
        events.push_back({int(std::llround(note.startBeats*perBeat)),juce::MidiMessage::noteOn(note.channel,note.pitch,juce::uint8(note.velocity))});
        events.push_back({int(std::llround((note.startBeats+note.durationBeats)*perBeat)),juce::MidiMessage::noteOff(note.channel,note.pitch)});}
    // At one sample a release comes first, so that a pitch the next note repeats sounds again.
    std::stable_sort(events.begin(),events.end(),[](const Event& x,const Event& y){return x.sample!=y.sample?x.sample<y.sample:x.message.isNoteOff()&&y.message.isNoteOn();});
    return events;
}
struct AiBriefArguments{Arguments host;juce::File input;airack::RenderQuality quality=airack::RenderQuality::realtime;};
// What an AI brief's answer changed in its source: racks by instance, controls in physical units, macro bindings.
Json aiBriefChanges(const airack::SoundDocument& before,const airack::SoundDocument& after)
{
    auto racks=[](const airack::SoundDocument& d){std::vector<const airack::RackInstance*> list;if(d.personalities)for(const auto& i:d.personalities->instances)list.push_back(&i);return list;};
    auto find=[](const std::vector<const airack::RackInstance*>& list,const juce::Uuid& id)->const airack::RackInstance*{for(const auto* i:list)if(i->id==id)return i;return nullptr;};
    auto describe=[](const airack::RackInstance& i){auto row=jsonObject();property(row,"name",i.design.name);property(row,"role",i.design.role==airack::RackRole::synth?"synth":"fx");
        Json slots(juce::Array<Json>{});for(const auto& b:i.bindings)slots.append(int(b.slot));property(row,"slots",slots);return row;};
    const auto was=racks(before),is=racks(after);Json placed(juce::Array<Json>{}),removed(juce::Array<Json>{}),kept(juce::Array<Json>{});
    for(const auto* i:is)if(!find(was,i->id))placed.append(describe(*i));
    for(const auto* i:was)if(const auto* now=find(is,i->id)){auto row=describe(*now);property(row,"design_changed",!(i->design==now->design));kept.append(row);}else removed.append(describe(*i));
    auto target=[](std::uint32_t key)->Json{const auto index=airack::parameterIndexForKey(key);return index?Json(airack::parameterRegistry()[*index].id):Json();};
    Json controls(juce::Array<Json>{}),macros(juce::Array<Json>{});
    for(std::size_t i=0;i<airack::parameterCount;++i)if(before.values[i]!=after.values[i]){const auto& p=airack::parameterRegistry()[i];auto row=jsonObject();
        property(row,"id",p.id);property(row,"name",p.name);property(row,"unit",p.unit);
        property(row,"before",double(airack::parameterPhysicalValue(before.values,i)));property(row,"after",double(airack::parameterPhysicalValue(after.values,i)));controls.append(row);}
    for(std::size_t i=0;i<after.macros.size();++i)if(!(before.macros[i]==after.macros[i])){auto row=jsonObject();property(row,"index",int(i+1));
        property(row,"before_target",target(before.macros[i].targetKey));property(row,"before_amount",double(before.macros[i].amount));
        property(row,"after_target",target(after.macros[i].targetKey));property(row,"after_amount",double(after.macros[i].amount));macros.append(row);}
    auto out=jsonObject();property(out,"racks_placed",placed);property(out,"racks_removed",removed);property(out,"racks_kept",kept);
    property(out,"layout_changed",!(before.layout==after.layout));property(out,"controls",controls);property(out,"macros_changed",macros);return out;
}
AiBriefArguments parseAiBriefArguments(const juce::StringArray& args)
{
    require(args.size()==9&&args[0]=="--ai-brief"&&args[1]=="--plugin"&&args[3]=="--output"&&args[5]=="--input"&&args[7]=="--quality",
        "Usage: RackSynthRender --ai-brief --plugin ABS_VST3 --output FRESH_ABS_WORKSPACE_DIRECTORY --input ABS_PROBE_OUTPUT --quality realtime|higher");
    require(args[8]=="realtime"||args[8]=="higher","Unknown render quality");
    AiBriefArguments result;result.host=parseArguments({"--plugin",args[2],"--output",args[4]});
    require(!result.host.output.exists()&&!fs::exists(fs::symlink_status(fs::path(result.host.output.getFullPathName().toWideCharPointer()))),"AI brief output must be fresh, including links");
    require(juce::File::isAbsolutePath(args[6]),"AI brief input must be absolute");result.input=juce::File(args[6]);
    const auto workspace=fs::weakly_canonical(fs::path(juce::File(RACKSYNTH_WORKSPACE).getFullPathName().toWideCharPointer()));
    const auto input=fs::weakly_canonical(fs::path(result.input.getFullPathName().toWideCharPointer()));const auto relative=input.lexically_relative(workspace);
    require(!relative.empty()&&relative!="."&&!relative.is_absolute()&&*relative.begin()!="..","AI brief input escapes workspace");
    require(result.input.isDirectory(),"AI brief input must be the probe's output folder");
    result.quality=args[8]=="higher"?airack::RenderQuality::higher:airack::RenderQuality::realtime;return result;
}
SceneRendered renderCandidateState(juce::VST3PluginFormat& format,const juce::PluginDescription& description,
    const airack::ProjectDocument& expected,const juce::MemoryBlock& originalOrVariant,int blockSize,double bpm,bool native,bool lifecycle,
    const juce::MemoryBlock* recalled=nullptr,const juce::File* failureOutput=nullptr,const juce::AudioBuffer<float>* sidechain=nullptr)
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(),"Candidate lifecycle requires message thread");
    constexpr int rate=48000;const int body=candidateBeatFrame(4,rate,bpm),frames=body+4*rate;
    struct Playhead final:juce::AudioPlayHead{PositionInfo current;juce::Optional<PositionInfo> getPosition() const override{return current;}} playhead;
    std::unique_ptr<juce::AudioProcessor> processor;
    if(native)processor=std::make_unique<airack::InstrumentProcessor>();
    else{juce::String error;processor=format.createInstanceFromDescription(description,rate,blockSize,error);}
    require(processor&&processor->getTotalNumInputChannels()==0&&processor->getTotalNumOutputChannels()==2&&processor->acceptsMidi(),"Candidate processor identity unavailable");
    processor->setNonRealtime(true);processor->setPlayHead(&playhead);playhead.current=candidatePosition(expected,0,body,rate,bpm);
    const auto state=native?originalOrVariant:hostEnvelope(originalOrVariant);
    const auto& restoreBytes=recalled?*recalled:state;processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));
    SceneRendered result;
    if(!native){auto* instance=dynamic_cast<juce::AudioPluginInstance*>(processor.get());require(instance,"Loaded candidate is not a plugin instance");result.registry=verifyPatternRegistry(*instance);}
    bool prepared=false;int verificationOrdinal=0;
    auto verify=[&](const airack::ProjectDocument& wanted,const char* stage){++verificationOrdinal;processor->getStateInformation(result.render.hostState);
        if(native){airack::ProjectDocument actual;require(airack::decodeProjectState(result.render.hostState.getData(),result.render.hostState.getSize(),actual)&&airack::validateProjectDocument(actual)&&actual==wanted,"Native complete candidate state differs");}
        else{
            const auto actual=personalityComponent(result.render.hostState);const bool same=actual==wanted;
            if(!same&&failureOutput)try{
                auto summary=jsonObject();property(summary,"status","INCOMPLETE_CANDIDATE_PROJECT_MISMATCH_DIAGNOSTIC");property(summary,"stage",stage);
                property(summary,"verification_ordinal",verificationOrdinal);property(summary,"quality",wanted.sound.values[758]==1?"higher":"realtime");
                property(summary,"block_size",blockSize);property(summary,"loaded_vst3",true);property(summary,"prepared",prepared);
                property(summary,"latency_samples",processor->getLatencySamples());property(summary,"host_non_realtime",processor->isNonRealtime());
                property(summary,"full_project_equal",same);property(summary,"sound_equal",actual.sound==wanted.sound);
                property(summary,"values_equal",actual.sound.values==wanted.sound.values);property(summary,"layout_equal",actual.sound.layout==wanted.sound.layout);
                property(summary,"macros_equal",actual.sound.macros==wanted.sound.macros);property(summary,"control_locks_equal",actual.sound.controlLocks==wanted.sound.controlLocks);
                property(summary,"synth_locks_equal",actual.sound.synthLocks==wanted.sound.synthLocks);property(summary,"effect_locks_equal",actual.sound.effectLocks==wanted.sound.effectLocks);
                property(summary,"compatibility_equal",actual.sound.compatibility==wanted.sound.compatibility);
                const auto actualCount=actual.sound.personalities?actual.sound.personalities->instances.size():0;
                const auto wantedCount=wanted.sound.personalities?wanted.sound.personalities->instances.size():0;
                property(summary,"actual_personality_count",int(actualCount));property(summary,"expected_personality_count",int(wantedCount));
                property(summary,"personalities_equal",actualCount==wantedCount&&(actualCount==0||*actual.sound.personalities==*wanted.sound.personalities));
                property(summary,"pattern_equal",actual.pattern==wanted.pattern);property(summary,"mode_equal",actual.mode==wanted.mode);property(summary,"group_equal",actual.group==wanted.group);
                for(std::size_t i=0;i<airack::parameterCount;++i)if(actual.sound.values[i]!=wanted.sound.values[i]){
                    auto first=jsonObject();property(first,"index",int(i));property(first,"id",airack::parameterRegistry()[i].id);
                    property(first,"actual_float_bits",juce::String::toHexString(juce::int64(std::bit_cast<std::uint32_t>(actual.sound.values[i]))));
                    property(first,"expected_float_bits",juce::String::toHexString(juce::int64(std::bit_cast<std::uint32_t>(wanted.sound.values[i]))));
                    property(summary,"first_parameter_difference",first);break;
                }
                juce::MemoryBlock expectedBytes,actualBytes;require(airack::encodeProjectState(wanted,expectedBytes)&&airack::encodeProjectState(actual,actualBytes),"Candidate diagnostic canonical encode failed");
                const auto opaqueFile=failureOutput->getChildFile("mismatch-returned.hoststate");const auto expectedFile=failureOutput->getChildFile("mismatch-expected.airackproject");const auto actualFile=failureOutput->getChildFile("mismatch-actual.airackproject");
                writeBlock(opaqueFile,result.render.hostState);writeBlock(expectedFile,expectedBytes);writeBlock(actualFile,actualBytes);
                property(summary,"returned_opaque",identity(opaqueFile));property(summary,"expected_project",identity(expectedFile));property(summary,"actual_project",identity(actualFile));
                property(summary,"status","CAPTURED_CANDIDATE_PROJECT_MISMATCH_DIAGNOSTIC");property(summary,"qualification","Failure observation only; original full equality failure and missing runtime postguard remain; not replay acceptance");
                writePatternJson(failureOutput->getChildFile("mismatch-diagnostic.json"),summary);
            }catch(const std::exception&){std::cerr<<"CandidateMismatch diagnostic capture incomplete\n";}catch(...){std::cerr<<"CandidateMismatch diagnostic capture incomplete\n";}
            require(same,"Actual embedded personality project differs");
        }};
    verify(expected,"restore-input");result.initialOpaque=result.render.hostState;
    juce::MemoryBlock damaged;require(airack::encodeProjectState(expected,damaged),"Candidate invalid-recall setup failed");static_cast<char*>(damaged.getData())[0]^=1;
    const auto bad=native?damaged:hostEnvelope(damaged);processor->setStateInformation(bad.getData(),int(bad.getSize()));verify(expected,"invalid-recall");
    if(sidechain){auto layout=processor->getBusesLayout();require(layout.inputBuses.size()==1&&layout.inputBuses[0].isDisabled(),"Candidate optional auxiliary default differs");
        layout.inputBuses.set(0,juce::AudioChannelSet::stereo());require(processor->setBusesLayout(layout)&&processor->getTotalNumInputChannels()==2,"Candidate auxiliary stereo activation failed");}
    processor->prepareToPlay(rate,blockSize);prepared=true;result.render.latencySamples=processor->getLatencySamples();
    const int wantedLatency=expected.sound.values[758]==1?21:0;
    require(result.render.latencySamples==wantedLatency,"Prepared candidate effective latency does not match selected quality");verify(expected,"prepared-latency");
    // Prepare and read latency BEFORE opening any candidate editor. Legacy paths stay unchanged.
    if(lifecycle){require(!native,"Candidate native editor is outside this loaded lifecycle witness");
        const bool saturation=std::any_of(expected.sound.layout.effects.begin(),expected.sound.layout.effects.end(),[](const auto& slot){return slot.use==airack::SlotUse::present&&slot.kind==airack::EffectKind::saturation;});
        for(int cycle=0;cycle<2;++cycle){std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditorIfNeeded());require(editor!=nullptr,"Loaded prepared candidate editor missing");
            if(!saturation){editor->addToDesktop(0);editor->setVisible(true);}
            for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(2200,1600)}){editor->setSize(size.x,size.y);personalityPump();require(editor->getWidth()>0&&editor->getHeight()>0,"Loaded candidate resize failed");
                if(saturation){require(editor->getPeer()==nullptr,"Saturation offline candidate unexpectedly owns a native peer");juce::Image image(juce::Image::ARGB,editor->getWidth(),editor->getHeight(),true);juce::Graphics graphics(image);editor->paintEntireComponent(graphics,true);require(image.isValid(),"Saturation actual offscreen editor paint failed");}
                verify(expected,"editor-resize");}
            editor.reset();personalityPump();verify(expected,"editor-close");require(processor->getLatencySamples()==wantedLatency,"Editor lifecycle changed prepared quality latency");}}
    std::vector<Event> events;
    if(expected.mode==airack::PlaybackMode::externalMidi){events.push_back({0,juce::MidiMessage::noteOn(1,48,juce::uint8(100))});events.push_back({candidateBeatFrame(1,rate,bpm),juce::MidiMessage::noteOff(1,48)});}
    result.render.audio.setSize(2,frames);result.render.audio.clear();juce::AudioBuffer<float> block(2,blockSize);juce::MidiBuffer midi;midi.ensureSize(4096);std::size_t eventIndex=0;
    for(int sample=0;sample<frames;){int end=std::min(sample+blockSize,frames);if(sample<body)end=std::min(end,body);const int count=end-sample;
        playhead.current=candidatePosition(expected,sample,body,rate,bpm);block.setSize(2,count,false,false,true);block.clear();midi.clear();Json delivered(juce::Array<Json>{});
        if(sidechain&&sample<sidechain->getNumSamples())for(int ch=0;ch<2;++ch)block.copyFrom(ch,0,*sidechain,ch,sample,std::min(count,sidechain->getNumSamples()-sample));
        while(eventIndex<events.size()&&events[eventIndex].sample<end){const auto& event=events[eventIndex++];require(event.sample>=sample,"Candidate MIDI schedule regressed");midi.addEvent(event.message,event.sample-sample);
            auto record=jsonObject();property(record,"absolute_sample",event.sample);property(record,"block_offset",event.sample-sample);Json bytes(juce::Array<Json>{});
            for(int n=0;n<event.message.getRawDataSize();++n)bytes.append(int(event.message.getRawData()[n]));property(record,"bytes",bytes);delivered.append(record);}
        processor->processBlock(block,midi);for(int channel=0;channel<2;++channel)result.render.audio.copyFrom(channel,sample,block,channel,0,count);
        auto callback=jsonObject();property(callback,"sample",sample);property(callback,"count",count);property(callback,"bpm",bpm);property(callback,"ppq",*playhead.current.getPpqPosition());property(callback,"playing",playhead.current.getIsPlaying());
        property(callback,"meter_numerator",expected.pattern.meter.numerator);property(callback,"meter_denominator",expected.pattern.meter.denominator);property(callback,"delivered_midi",delivered);result.callbacks.append(callback);sample=end;}
    require(eventIndex==events.size(),"Candidate incoming MIDI incomplete");verify(expected,"render-final");
    // While playing, VST3 host edits reach the processor in the next callback.
    // These post-render lifecycle blocks do not enter the saved musical audio.
    int automationSample=frames;
    auto deliverAutomation=[&](const char* stage){
        playhead.current=candidatePosition(expected,automationSample,body,rate,bpm);
        block.setSize(2,blockSize,false,false,true);block.clear();midi.clear();processor->processBlock(block,midi);
        auto delivered=jsonObject();property(delivered,"kind","post_render_automation_delivery");property(delivered,"stage",stage);
        property(delivered,"sample",automationSample);property(delivered,"count",blockSize);property(delivered,"bpm",bpm);
        property(delivered,"playing",playhead.current.getIsPlaying());property(delivered,"midi_empty",true);property(delivered,"included_in_saved_audio",false);
        result.callbacks.append(delivered);automationSample+=blockSize;
    };
    if(lifecycle&&!candidateSaturationAutomation(expected).empty()){
        struct SaturationEvents final:juce::AudioProcessorParameter::Listener {
            juce::AudioProcessorParameter& parameter;int begins=0,ends=0,values=0;
            explicit SaturationEvents(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
            ~SaturationEvents() override{parameter.removeListener(this);}
            void parameterValueChanged(int,float) override{++values;}
            void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
        };
        const auto target=[&](std::size_t index){const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(airack::parameterRegistry()[index].id,true));juce::AudioProcessorParameter* found=nullptr;
            for(auto* p:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(p);named&&named->getParameterID()==id){require(!found,"Duplicate saturation host ID");found=p;}
            require(found,"Actual saturation hosted parameter missing");return found;};
        const auto editedProject=[&](const airack::ProjectDocument& before,const std::vector<airack::ParameterGestureValue>& edits){auto proposed=std::make_unique<airack::SoundDocument>(before.sound),after=std::make_unique<airack::SoundDocument>(before.sound);for(const auto& edit:edits)proposed->values[edit.index]=edit.value;airack::RackDifference delta;
            const auto status=airack::reconcileRackEdit(before.sound,*proposed,{},airack::SoundEditOrigin::manual,*after,delta);require(status==airack::RackCandidateStatus::ready,"Saturation host expectation rejected");auto wanted=std::make_unique<airack::ProjectDocument>(before);wanted->sound=std::move(*after);require(airack::validateProjectDocument(*wanted),"Saturation host complete expectation invalid");return wanted;};
        Json submitted(juce::Array<Json>{});bool rejectedRecall=false;
        auto edits=candidateSaturationAutomation(expected);
        // Existing shared bypass is deliberately separate from the35 appended fields.
        for(std::size_t slot=0;slot<8;++slot)if(expected.sound.layout.effects[slot].use==airack::SlotUse::present&&expected.sound.layout.effects[slot].kind==airack::EffectKind::saturation)
            for(float value:{0.f,1.f})edits.push_back({572+slot*23,value});
        for(const auto& edit:edits){auto wanted=editedProject(expected,{edit});SaturationEvents events(*target(edit.index));
            events.parameter.beginChangeGesture();events.parameter.setValueNotifyingHost(edit.value);events.parameter.endChangeGesture();personalityPump();deliverAutomation("saturation-field-endpoint");verify(*wanted,"saturation-field-endpoint");
            require(events.begins==1&&events.ends==1&&events.values>=1,"Saturation begin/set/end/listener unbalanced");
            juce::MemoryBlock opaque;processor->getStateInformation(opaque);
            if(!rejectedRecall){processor->setStateInformation(bad.getData(),int(bad.getSize()));deliverAutomation("saturation-rejected-invalid-recall");verify(*wanted,"saturation-invalid-recall-preserves-edited-project");rejectedRecall=true;}
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"saturation-opaque-undo");
            processor->setStateInformation(opaque.getData(),int(opaque.getSize()));personalityPump();verify(*wanted,"saturation-opaque-redo");
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"saturation-restore");require(processor->getLatencySamples()==wantedLatency,"Saturation automation changed quality latency");
            auto row=jsonObject();property(row,"index",int(edit.index));property(row,"parameter_id",airack::parameterRegistry()[edit.index].id);property(row,"host_id",juce::String(juce::VST3ClientExtensions::convertJuceParameterId(airack::parameterRegistry()[edit.index].id,true)));property(row,"normalized",edit.value);property(row,"begins",events.begins);property(row,"ends",events.ends);property(row,"value_callbacks",events.values);property(row,"opaque_sha256",juce::SHA256(opaque).toHexString());submitted.append(row);
        }
        auto witness=jsonObject();property(witness,"kind","candidate_saturation_automation");property(witness,"fields_and_separate_shared_bypass",submitted);property(witness,"all35_two_endpoints_per_present_slot",true);property(witness,"inactive_banks_included",true);property(witness,"full_project_verified",true);property(witness,"opaque_undo_redo",true);property(witness,"invalid_recall_preserves_edited_project",rejectedRecall);property(witness,"editor_delivery","ACTUAL_PLUGIN_CREATED_NO_NATIVE_PEER; paint/resize/close/state only");result.callbacks.append(witness);
        // Play a real saved phrase across structural/style/bypass/compensation/cut edits.
        auto transitionExpected=std::make_unique<airack::ProjectDocument>(expected);processor->releaseResources();processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));processor->prepareToPlay(rate,blockSize);
        constexpr int segment=4096,segments=9;juce::AudioBuffer<float> transitions(2,segment*segments);transitions.clear();Json changes(juce::Array<Json>{});
        for(int sample=0;sample<transitions.getNumSamples();sample+=blockSize){const int step=sample/segment;Json delivery(juce::Array<Json>{});std::vector<std::unique_ptr<SaturationEvents>> transitionEvents;
            if(sample%segment==0&&step>0){std::vector<airack::ParameterGestureValue> change;
                for(std::size_t slot=0;slot<8;++slot)if(expected.sound.layout.effects[slot].use==airack::SlotUse::present&&expected.sound.layout.effects[slot].kind==airack::EffectKind::saturation){const auto start=airack::saturationParameterStart+slot*35;
                    if(step==1)change.push_back({start,1-transitionExpected->sound.values[start]});
                    if(step==2||step==3)for(std::size_t lane=0;lane<4;++lane)change.push_back({start+12+lane*6,step==2?.5f:1.f});
                    if(step==4||step==5)change.push_back({572+slot*23,step==4?1.f:0.f});
                    if(step==6)change.push_back({start+4,1-transitionExpected->sound.values[start+4]});
                    if(step==7)change.push_back({start+7,1-transitionExpected->sound.values[start+7]});
                    if(step==8)change.push_back({start+9,1-transitionExpected->sound.values[start+9]});}
                transitionExpected=editedProject(*transitionExpected,change);personalityPump();
                for(const auto& edit:change){transitionEvents.push_back(std::make_unique<SaturationEvents>(*target(edit.index)));auto& events=*transitionEvents.back();auto field=jsonObject();property(field,"index",int(edit.index));property(field,"requested",edit.value);property(field,"host_before",events.parameter.getValue());events.parameter.beginChangeGesture();events.parameter.setValueNotifyingHost(edit.value);events.parameter.endChangeGesture();property(field,"host_after_set",events.parameter.getValue());delivery.append(field);}
                auto row=jsonObject();property(row,"sample",sample);property(row,"transition",std::array<const char*,8>{"mode","glow","dense","shared-bypass-on","shared-bypass-off","compensation","pre-cut","post-cut"}[std::size_t(step-1)]);changes.append(row);
            }
            const int count=std::min(blockSize,transitions.getNumSamples()-sample);playhead.current=candidatePosition(expected,sample,body,rate,bpm);block.setSize(2,count,false,false,true);block.clear();midi.clear();
            if(expected.mode==airack::PlaybackMode::externalMidi&&sample==0)midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(100)),0);
            processor->processBlock(block,midi);for(int channel=0;channel<2;++channel)transitions.copyFrom(channel,sample,block,channel,0,count);
            if(sample%segment==0){for(std::size_t i=0;i<transitionEvents.size();++i){const auto& events=*transitionEvents[i];require(events.begins==1&&events.ends==1&&events.values>=1,"Saturation rendered transition gestures unbalanced");property(delivery[int(i)],"host_after_callback",events.parameter.getValue());}
                // Playing edits belong to the next callback before queued controller refreshes.
                personalityPump();for(std::size_t i=0;i<transitionEvents.size();++i)property(delivery[int(i)],"host_after_pump",transitionEvents[i]->parameter.getValue());
                if(failureOutput&&step>0){auto diagnostic=jsonObject();property(diagnostic,"sample",sample);property(diagnostic,"fields",delivery);writePatternJson(failureOutput->getChildFile(juce::String("saturation-transition-delivery-")+(expected.sound.values[758]==1?"higher":"realtime")+"-"+juce::String(step)+".json"),diagnostic);}
                verify(*transitionExpected,"saturation-rendered-transition");}
        }
        const auto measured=measureWindow(transitions,{{0,transitions.getNumSamples()}});double changed=0;for(int channel=0;channel<2;++channel)for(int sample=0;sample<transitions.getNumSamples();++sample)changed=std::max(changed,std::abs(double(transitions.getSample(channel,sample))-result.render.audio.getSample(channel,sample)));
        require(measured.rms>1e-8&&changed>1e-6,"Saturation rendered transitions must have nonzero changed audio");
        auto audio=jsonObject();property(audio,"kind","candidate_saturation_rendered_transitions");property(audio,"changes",changes);property(audio,"rms",measured.rms);property(audio,"maximum_difference_from_static",changed);property(audio,"full_project_verified",true);
        if(failureOutput){const auto path=failureOutput->getChildFile(juce::String("saturation-transitions-")+(expected.sound.values[758]==1?"higher":"realtime")+".wav");writeWav(path,transitions,rate);property(audio,"wav",identity(path));}
        processor->reset();processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));deliverAutomation("saturation-final-post-render-context");personalityPump();verify(expected,"saturation-reset-and-full-restore");require(processor->getLatencySamples()==wantedLatency,"Saturation reset changed quality latency");result.callbacks.append(audio);
    }
    if(lifecycle){const auto& gain=airack::parameterRegistry()[756];const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(gain.id,true));juce::AudioProcessorParameter* target=nullptr;
        for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==id){require(!target,"Duplicate candidate gain parameter");target=parameter;}
        require(target,"Candidate output gain automation target missing");const float value=expected.sound.values[756]==.25f?.75f:.25f;auto automated=expected;automated.sound.values[756]=value;
        target->beginChangeGesture();target->setValueNotifyingHost(value);target->endChangeGesture();personalityPump();deliverAutomation("gain-automation");verify(automated,"gain-automation");
        processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"gain-restore");
        auto witness=jsonObject();property(witness,"kind","restored_output_gain_automation");property(witness,"normalized",value);property(witness,"full_project_verified",true);result.callbacks.append(witness);
        const auto engine=candidateEngineTrial(expected);auto engineWitness=jsonObject();property(engineWitness,"kind","candidate_engine_automation");property(engineWitness,"applicability",engine.reason);
        if(engine.applicable){const auto& descriptor=airack::parameterRegistry()[engine.index];const auto engineId=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));juce::AudioProcessorParameter* engineTarget=nullptr;
            for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==engineId){require(!engineTarget,"Duplicate candidate engine parameter");engineTarget=parameter;}
            require(engineTarget,"Candidate engine automation target missing");engineTarget->beginChangeGesture();engineTarget->setValueNotifyingHost(engine.normalized);engineTarget->endChangeGesture();personalityPump();deliverAutomation("engine-automation");verify(engine.expected,"engine-automation");
            require(processor->getLatencySamples()==wantedLatency,"Engine automation changed prepared quality latency");
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));verify(expected,"engine-restore");require(processor->getLatencySamples()==wantedLatency,"Engine restoration changed prepared quality latency");
            property(engineWitness,"parameter_id",descriptor.id);property(engineWitness,"normalized",engine.normalized);property(engineWitness,"full_project_verified",true);property(engineWitness,"original_full_project_restored",true);
        }result.callbacks.append(engineWitness);
        // Retain both original EQ endpoints as new banks append, alongside the
        // current catalog end, including a bank with no inserted effect.
        for(const auto index:candidateDynamicEqAutomation(expected)){
            const auto& descriptor=airack::parameterRegistry()[index];
            const auto hostId=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));juce::AudioProcessorParameter* eqTarget=nullptr;
            for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==hostId){require(!eqTarget,"Duplicate candidate EQ host parameter");eqTarget=parameter;}
            require(eqTarget,"Candidate EQ host automation target missing");
            const float value=candidateAutomationValue(index,expected.sound.values[index]);
            auto proposed=expected.sound;proposed.values[index]=value;auto automated=expected;
            airack::RackDifference difference;require(airack::reconcileRackEdit(expected.sound,proposed,{},airack::SoundEditOrigin::manual,automated.sound,difference)==airack::RackCandidateStatus::ready,"EQ automation expectation rejected");
            eqTarget->beginChangeGesture();eqTarget->setValueNotifyingHost(value);eqTarget->endChangeGesture();personalityPump();deliverAutomation("dynamic-eq-automation");verify(automated,"dynamic-eq-automation");
            require(processor->getLatencySamples()==wantedLatency,"EQ automation changed prepared quality latency");
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"dynamic-eq-restore");
            auto witness=jsonObject();property(witness,"kind","candidate_dynamic_eq_automation");property(witness,"index",int(index));property(witness,"parameter_id",descriptor.id);property(witness,"host_id",hostId);property(witness,"normalized",value);property(witness,"full_project_verified",true);property(witness,"original_full_project_restored",true);result.callbacks.append(witness);
        }
        const auto grouped=candidateEqGroupTrial(expected);
        if(!grouped.edits.empty()){
            struct Events final:juce::AudioProcessorParameter::Listener {
                juce::AudioProcessorParameter& parameter;int begins=0,ends=0,values=0;
                explicit Events(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
                ~Events() override{parameter.removeListener(this);}
                void parameterValueChanged(int,float) override{++values;}
                void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
            };
            std::vector<std::unique_ptr<Events>> fields;Json submitted(juce::Array<Json>{});
            for(const auto& edit:grouped.edits){const auto& descriptor=airack::parameterRegistry()[edit.index];const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));
                juce::AudioProcessorParameter* target=nullptr;for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==id){require(!target,"Duplicate grouped EQ host ID");target=parameter;}
                require(target,"Grouped EQ host target missing");fields.push_back(std::make_unique<Events>(*target));
                auto field=jsonObject();property(field,"index",int(edit.index));property(field,"parameter_id",descriptor.id);property(field,"host_id",id);property(field,"normalized",edit.value);submitted.append(field);
            }
            for(auto& field:fields)field->parameter.beginChangeGesture();
            for(std::size_t n=0;n<fields.size();++n)fields[n]->parameter.setValueNotifyingHost(grouped.edits[n].value);
            for(auto& field:fields)field->parameter.endChangeGesture();
            personalityPump();deliverAutomation("dynamic-eq-group");verify(grouped.expected,"dynamic-eq-group");
            for(const auto& field:fields)require(field->begins==1&&field->ends==1&&field->values>=1,"Grouped EQ host gestures unbalanced");
            juce::MemoryBlock editedOpaque;processor->getStateInformation(editedOpaque);
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"dynamic-eq-group-host-undo");
            processor->setStateInformation(editedOpaque.getData(),int(editedOpaque.getSize()));personalityPump();verify(grouped.expected,"dynamic-eq-group-host-redo");
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"dynamic-eq-group-final-restore");
            require(processor->getLatencySamples()==wantedLatency,"Grouped EQ host state changed quality latency");
            auto witness=jsonObject();property(witness,"kind","candidate_dynamic_eq_group_automation");property(witness,"fields",submitted);property(witness,"balanced_host_gestures",true);property(witness,"full_project_verified",true);property(witness,"host_state_undo_redo",true);property(witness,"manual_graph_history","SEPARATE_CURRENT_SHARED_LIBRARY_NATIVE_GRAPH_TEST");property(witness,"included_in_saved_audio",false);result.callbacks.append(witness);
        }
        const auto gainTrial=candidateEqGainTrial(expected);
        if(!gainTrial.edits.empty()){
            struct GainEvents final:juce::AudioProcessorParameter::Listener {
                juce::AudioProcessorParameter& parameter;int begins=0,ends=0,values=0;
                explicit GainEvents(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
                ~GainEvents() override{parameter.removeListener(this);}
                void parameterValueChanged(int,float) override{++values;}
                void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
            };
            std::vector<std::unique_ptr<GainEvents>> fields;Json submitted(juce::Array<Json>{});
            for(const auto& edit:gainTrial.edits){const auto& descriptor=airack::parameterRegistry()[edit.index];const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));
                juce::AudioProcessorParameter* target=nullptr;for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==id){require(!target,"Duplicate EQ gain host ID");target=parameter;}
                require(target,"EQ gain host target missing");fields.push_back(std::make_unique<GainEvents>(*target));
                auto field=jsonObject();property(field,"index",int(edit.index));property(field,"parameter_id",descriptor.id);property(field,"host_id",id);property(field,"normalized",edit.value);submitted.append(field);
            }
            for(auto& field:fields)field->parameter.beginChangeGesture();
            for(std::size_t n=0;n<fields.size();++n)fields[n]->parameter.setValueNotifyingHost(gainTrial.edits[n].value);
            for(auto& field:fields)field->parameter.endChangeGesture();
            personalityPump();deliverAutomation("dynamic-eq-gain");verify(gainTrial.expected,"dynamic-eq-gain");
            for(const auto& field:fields)require(field->begins==1&&field->ends==1&&field->values>=1,"EQ gain host gestures unbalanced");
            juce::MemoryBlock editedOpaque;processor->getStateInformation(editedOpaque);
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"dynamic-eq-gain-host-undo");
            processor->setStateInformation(editedOpaque.getData(),int(editedOpaque.getSize()));personalityPump();verify(gainTrial.expected,"dynamic-eq-gain-host-redo");
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"dynamic-eq-gain-final-restore");
            require(processor->getLatencySamples()==wantedLatency,"EQ gain host state changed quality latency");
            auto witness=jsonObject();property(witness,"kind","candidate_dynamic_eq_gain_automation");property(witness,"fields",submitted);property(witness,"balanced_host_gestures",true);property(witness,"full_project_verified",true);property(witness,"host_state_undo_redo",true);property(witness,"manual_graph_history","SEPARATE_CURRENT_SHARED_LIBRARY_NATIVE_GRAPH_TEST");property(witness,"included_in_saved_audio",false);result.callbacks.append(witness);
        }
    }
    if(lifecycle)for(int mode=0;mode<5;++mode){const auto trial=candidateEqPlacementTrial(expected,mode);if(trial.edits.empty())break;
        struct PlacementEvents final:juce::AudioProcessorParameter::Listener {
            juce::AudioProcessorParameter& parameter;int begins=0,ends=0,values=0;
            explicit PlacementEvents(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
            ~PlacementEvents() override{parameter.removeListener(this);}
            void parameterValueChanged(int,float) override{++values;}
            void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
        };
        std::vector<std::unique_ptr<PlacementEvents>> fields;Json submitted(juce::Array<Json>{});
        for(const auto& edit:trial.edits){const auto& descriptor=airack::parameterRegistry()[edit.index];const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));juce::AudioProcessorParameter* target=nullptr;
            for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);named&&named->getParameterID()==id){require(!target,"Duplicate placement host ID");target=parameter;}
            require(target,"Placement host target missing");fields.push_back(std::make_unique<PlacementEvents>(*target));
            auto field=jsonObject();property(field,"index",int(edit.index));property(field,"parameter_id",descriptor.id);property(field,"normalized",edit.value);submitted.append(field);
        }
        for(auto& field:fields)field->parameter.beginChangeGesture();
        for(std::size_t n=0;n<fields.size();++n)fields[n]->parameter.setValueNotifyingHost(trial.edits[n].value);
        for(auto& field:fields)field->parameter.endChangeGesture();
        personalityPump();deliverAutomation("dynamic-eq-placement");verify(trial.expected,"dynamic-eq-placement");
        for(const auto& field:fields)require(field->begins==1&&field->ends==1&&field->values>=1,"Placement host gestures unbalanced");
        juce::MemoryBlock editedOpaque;processor->getStateInformation(editedOpaque);
        processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"placement-host-undo");
        processor->setStateInformation(editedOpaque.getData(),int(editedOpaque.getSize()));personalityPump();verify(trial.expected,"placement-host-redo");
        processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"placement-final-restore");
        require(processor->getLatencySamples()==wantedLatency,"Placement host state changed quality latency");
        auto witness=jsonObject();property(witness,"kind","candidate_dynamic_eq_placement_automation");property(witness,"mode",mode);property(witness,"fields",submitted);property(witness,"balanced_host_gestures",true);property(witness,"full_project_verified",true);property(witness,"host_state_undo_redo",true);property(witness,"included_in_saved_audio",false);result.callbacks.append(witness);
    }
    if(lifecycle&&!candidateDynamicEqAutomation(expected).empty()){
        std::map<juce::String,juce::AudioProcessorParameter*> targets;
        for(auto* parameter:processor->getParameters())if(auto* named=dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter))targets.emplace(named->getParameterID(),parameter);
        constexpr int extendedTrials=4*5,historicalTrials=6*2,detectorTrials=4*4;
        for(int n=0;n<extendedTrials+historicalTrials+detectorTrials;++n){
            const bool detector=n>=extendedTrials+historicalTrials,legacy=!detector&&n>=extendedTrials;
            const int shape=detector?0:legacy?(n-extendedTrials)/2:n/5,slope=detector?0:legacy?(n-extendedTrials)%2:n%5;
            const int source=detector?(n-extendedTrials-historicalTrials)/4:0,flags=detector?(n-extendedTrials-historicalTrials)%4:0;
            const auto trial=detector?candidateEqDetectorTrial(expected,source,flags):candidateEqExtensionTrial(expected,shape,slope,legacy);
            struct ChoiceEvents final:juce::AudioProcessorParameter::Listener {
                juce::AudioProcessorParameter& parameter;int begins=0,ends=0,values=0;
                explicit ChoiceEvents(juce::AudioProcessorParameter& p):parameter(p){parameter.addListener(this);}
                ~ChoiceEvents() override{parameter.removeListener(this);}
                void parameterValueChanged(int,float) override{++values;}
                void parameterGestureChanged(int,bool start) override{if(start)++begins;else ++ends;}
            };
            std::vector<std::unique_ptr<ChoiceEvents>> fields;Json submitted(juce::Array<Json>{});
            for(const auto& edit:trial.edits){
                const auto& descriptor=airack::parameterRegistry()[edit.index];const auto id=juce::String(juce::VST3ClientExtensions::convertJuceParameterId(descriptor.id,true));
                const auto found=targets.find(id);require(found!=targets.end(),"EQ extension host target missing");fields.push_back(std::make_unique<ChoiceEvents>(*found->second));
                auto field=jsonObject();property(field,"index",int(edit.index));property(field,"parameter_id",descriptor.id);property(field,"normalized",edit.value);submitted.append(field);
            }
            for(auto& field:fields)field->parameter.beginChangeGesture();
            for(std::size_t n=0;n<fields.size();++n)fields[n]->parameter.setValueNotifyingHost(trial.edits[n].value);
            for(auto& field:fields)field->parameter.endChangeGesture();
            personalityPump();deliverAutomation(detector?"dynamic-eq-detector":"dynamic-eq-shape-slope");verify(trial.expected,detector?"dynamic-eq-detector":"dynamic-eq-shape-slope");
            for(const auto& field:fields)require(field->begins==1&&field->ends==1&&field->values>=1,"EQ extension host gestures unbalanced");
            juce::MemoryBlock editedOpaque;processor->getStateInformation(editedOpaque);
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"extension-host-undo");
            processor->setStateInformation(editedOpaque.getData(),int(editedOpaque.getSize()));personalityPump();verify(trial.expected,"extension-host-redo");
            processor->setStateInformation(restoreBytes.getData(),int(restoreBytes.getSize()));personalityPump();verify(expected,"extension-final-restore");
            require(processor->getLatencySamples()==wantedLatency,"EQ extension host state changed quality latency");
            auto witness=jsonObject();property(witness,"kind",detector?"candidate_dynamic_eq_detector_automation":"candidate_dynamic_eq_shape_slope_automation");
            if(detector){property(witness,"source",source);property(witness,"flags",flags);}else{property(witness,"historical_selectors",legacy);property(witness,"shape_choice",shape);property(witness,"slope_choice",slope);}
            property(witness,"fields",submitted);property(witness,"balanced_host_gestures",true);property(witness,"full_project_verified",true);property(witness,"host_state_undo_redo",true);property(witness,"included_in_saved_audio",false);result.callbacks.append(witness);
        }
    }
    processor->releaseResources();processor->setPlayHead(nullptr);return result;
}
void removeCandidateSaturation(const airack::ProjectDocument& project,airack::ProjectDocument& raw)
{
    for(std::uint8_t slot=0;slot<8;++slot)if(project.sound.layout.effects[slot].use==airack::SlotUse::present&&project.sound.layout.effects[slot].kind==airack::EffectKind::saturation){
        std::optional<juce::Uuid> owner;if(raw.sound.personalities)for(const auto& instance:raw.sound.personalities->instances)for(const auto& unit:instance.design.units)
            if(unit.module==airack::NativeModule::saturation)if(const auto* binding=airack::rackUnitBinding(instance,unit.id);binding&&binding->slot==slot)owner=instance.id;
        if(owner){airack::RackOperation op;op.kind=airack::RackOperationKind::detach;op.selected=*owner;auto detached=std::make_unique<airack::SoundDocument>();airack::RackDifference delta;
            require(airack::makeRackCandidate(raw.sound,op,airack::SoundEditOrigin::manual,*detached,delta)==airack::RackCandidateStatus::ready,"Saturation raw baseline detach rejected");raw.sound=std::move(*detached);}
        airack::RackEdit edit;edit.kind=airack::RackEditKind::removeEffect;edit.slot=slot;require(airack::editRackLayout(raw.sound.layout,edit,raw.sound.layout)==airack::RackEditResult::applied,"Saturation raw baseline slot removal rejected");}
    require(airack::validateProjectDocument(raw),"Saturation raw baseline Project invalid");
}
Json candidateSaturationAudio(juce::VST3PluginFormat& format,const juce::PluginDescription& description,
    const airack::ProjectDocument& project,const juce::File& output,double bpm)
{
    const auto edits=candidateSaturationAutomation(project);if(edits.empty())return Json();
    auto variants=std::make_unique<std::array<airack::ProjectDocument,4>>();variants->fill(project);auto& enabled=(*variants)[0];auto& bypass=(*variants)[1];auto& matched=(*variants)[2];auto& raw=(*variants)[3];
    removeCandidateSaturation(project,raw);
    for(std::uint8_t slot=0;slot<8;++slot)if(project.sound.layout.effects[slot].use==airack::SlotUse::present&&project.sound.layout.effects[slot].kind==airack::EffectKind::saturation){
        enabled.sound.values[572+slot*23]=0;bypass.sound.values[572+slot*23]=1;matched.sound.values[572+slot*23]=0;matched.sound.values[airack::saturationParameterStart+slot*35+3]=0;}
    const juce::String quality=project.sound.values[758]==1?"higher":"realtime";auto witness=jsonObject();property(witness,"kind","candidate_saturation_audio");property(witness,"quality",quality);
    const auto capture=[&](const char* name,const airack::ProjectDocument& wanted){juce::MemoryBlock bytes;require(airack::encodeProjectState(wanted,bytes),"Saturation audio Project encoding rejected");
        const auto rendered=renderCandidateState(format,description,wanted,bytes,512,bpm,false,false,nullptr,&output);
        const auto path=output.getChildFile("saturation-"+quality+"-"+name+".wav");writeWav(path,rendered.render.audio,48000);property(witness,name,identity(path));return rendered;};
    const auto wet=capture("engaged",enabled),dry=capture("raw-bypass",bypass),noSaturation=capture("no-saturation",raw),linear=capture("matched-dry",matched);
    auto driven=std::make_unique<airack::ProjectDocument>(enabled);
    for(std::size_t slot=0;slot<8;++slot)if(project.sound.layout.effects[slot].use==airack::SlotUse::present&&project.sound.layout.effects[slot].kind==airack::EffectKind::saturation)
        for(std::size_t lane=0;lane<4;++lane)driven->sound.values[airack::saturationParameterStart+slot*35+13+lane*6]=1;
    auto half=std::make_unique<airack::ProjectDocument>(*driven),halfRaw=std::make_unique<airack::ProjectDocument>(bypass);const auto registry=airack::parameterRegistry();
    for(int bank=1;bank<=4;++bank){const auto id="synth."+juce::String(bank)+".level";const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& d){return d.id==id;});require(found!=registry.end(),"Saturation homogeneity source-level control missing");const auto index=std::size_t(found-registry.begin());half->sound.values[index]=juce::NormalisableRange<float>(found->minimum,found->maximum,0,found->skew).convertTo0to1(.5f*airack::parameterPhysicalValue(driven->sound.values,index));halfRaw->sound.values[index]=half->sound.values[index];}
    const auto highDrive=capture("nonlinear-drive36",*driven),halfInput=capture("nonlinear-drive36-half-input",*half),halfBypass=capture("raw-bypass-half-input",*halfRaw);double homogeneity=0,rawHomogeneity=0;
    for(int channel=0;channel<2;++channel)for(int sample=0;sample<highDrive.render.audio.getNumSamples();++sample){homogeneity=std::max(homogeneity,std::abs(double(highDrive.render.audio.getSample(channel,sample))-2*double(halfInput.render.audio.getSample(channel,sample))));rawHomogeneity=std::max(rawHomogeneity,std::abs(double(dry.render.audio.getSample(channel,sample))-2*double(halfBypass.render.audio.getSample(channel,sample))));}
    const auto activity=measureWindow(wet.render.audio,{{0,wet.render.audio.getNumSamples()}}),dryActivity=measureWindow(dry.render.audio,{{0,dry.render.audio.getNumSamples()}});
    const auto nonlinear=difference(wet.render.audio,dry.render.audio),rawDifference=difference(dry.render.audio,noSaturation.render.audio);
    property(witness,"engaged_rms",activity.rms);property(witness,"raw_rms",dryActivity.rms);property(witness,"engaged_vs_bypass_maximum_difference",nonlinear);property(witness,"raw_bypass_vs_no_saturation_maximum_difference",rawDifference);
    property(witness,"drive36_half_input_homogeneity_error",homogeneity);property(witness,"nonlinear_stimulus","All four retained lane drives36dB; same authored source with every source-bank level halved, identical phrase/prepare/recall; compare wet(x) against2wet(x/2)");
    property(witness,"raw_bypass_half_input_homogeneity_error",rawHomogeneity);property(witness,"half_input_raw_rms",measureWindow(halfBypass.render.audio,{{0,halfBypass.render.audio.getNumSamples()}}).rms);
    property(witness,"latency_samples",wet.render.latencySamples);property(witness,"no_saturation_latency_samples",noSaturation.render.latencySamples);
    property(witness,"status","MEASURED_BEFORE_ASSERTIONS");writePatternJson(output.getChildFile("saturation-"+quality+"-audio.json"),witness);
    std::cout<<"SaturationAudio quality="<<quality<<" engaged_rms="<<activity.rms<<" raw_rms="<<dryActivity.rms<<" engaged_bypass_difference="<<nonlinear<<" raw_bypass_difference="<<rawDifference<<std::endl;
    require(activity.rms>1e-8&&dryActivity.rms>1e-8,"Saturation engaged/bypass assertion requires nonzero audio");
    require(nonlinear>1e-6,"Saturation engaged output must differ from matched explicit shared bypass");
    require(rawHomogeneity<=1e-6&&measureWindow(halfBypass.render.audio,{{0,halfBypass.render.audio.getNumSamples()}}).rms>1e-8,"Saturation nonlinear witness requires a nonzero linear source/bypass scaling control");
    require(homogeneity>1e-6,"Saturation actual hosted drive must violate linear input homogeneity");
    require(rawDifference==0,"Saturation shared bypass must return raw input exactly");
    require(wet.render.latencySamples==noSaturation.render.latencySamples&&dry.render.latencySamples==noSaturation.render.latencySamples&&linear.render.latencySamples==noSaturation.render.latencySamples,"Saturation changed no-saturation quality baseline latency");
    property(witness,"status","PASS");property(witness,"human_listening","NOT_RUN");writePatternJson(output.getChildFile("saturation-"+quality+"-audio-pass.json"),witness);return witness;
}
int renderCandidateReplay(const CandidateArguments& args)
{
    const auto input=readCandidateInput(args.input);const double bpm=candidateBpm(input.project,args.bpm);
    const auto sidechain=args.sidechain==juce::File()?juce::AudioBuffer<float>{}:readCandidateSidechain(args.sidechain);
    const auto* external=sidechain.getNumSamples()>0?&sidechain:nullptr;
    const auto sidechainBefore=external?identity(args.sidechain):Json();
    const auto moduleBefore=identity(binaryIn(args.host.plugin)),helperBefore=identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile));
    require(args.host.output.createDirectory().wasOk(),"Fresh candidate output creation failed");
    const auto originalFile=args.host.output.getChildFile("original-input.bin");writeBlock(originalFile,input.original);
    juce::MemoryBlock readback;require(originalFile.loadFileAsData(readback)&&readback==input.original,"Saved original candidate bytes differ");
    auto report=jsonObject();property(report,"status","INCOMPLETE");property(report,"input_path",args.input.getFullPathName());property(report,"original_input",identity(originalFile));
    property(report,"original_input_kind",input.soundOnly?"Sound binary; synthesized default external Project":"complete Project binary");property(report,"original_input_sha256",juce::SHA256(input.original).toHexString());
    property(report,"module_before",moduleBefore);property(report,"render_executable_before",helperBefore);property(report,"host_non_realtime_flag",true);property(report,"sample_rate",48000);
    if(external){property(report,"sidechain",sidechainBefore);property(report,"sidechain_frames",sidechain.getNumSamples());property(report,"sidechain_after_eof","connected silent auxiliary");}
    property(report,"bpm",bpm);property(report,"clock_source",input.project.group&&input.project.group->bpm?"captured group BPM":"explicit controlled harness BPM; original audition tempo not inferred");
    property(report,"stimulus",input.project.mode==airack::PlaybackMode::internalPattern?"original saved Internal mode/pattern, controlled four-beat host window then stopped four-second tail":"original saved External mode, controlled MIDI48/channel1/velocity100 at beat0/offbeat1; continuous transport; four-beat body and four-second tail");
    property(report,"original_audition_fidelity","NOT_CLAIMED; bind exported audition phrase/BPM/body/tail/result/preview separately before any such comparison");
    property(report,"empty_or_silent_candidate","finite zero audio is permitted; no mandatory activity or settled-tail assertion");property(report,"candidate_engine_automation","PENDING; selected bound active synth, full public reconciliation and original Project restoration, or explicit synth-free applicability");
    writePatternJson(args.host.output.getChildFile("bindings-incomplete.json"),report);
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> list;format.findAllTypesForFile(list,args.host.plugin.getFullPathName());require(list.size()==1&&isAirInstrument(*list[0]),"Candidate bundle identity differs");
    Json trials(juce::Array<Json>{});const bool selectedHigher=input.project.sound.values[758]==1;
    for(int observation=0;observation<2;++observation){const auto quality=(observation==0?selectedHigher:!selectedHigher)?airack::RenderQuality::higher:airack::RenderQuality::realtime;
        const auto wanted=candidateQualityVariant(input.project,quality);juce::MemoryBlock canonical;require(airack::encodeProjectState(wanted,canonical),"Candidate variant encoding failed");
        const auto saturationAudio=candidateSaturationAudio(format,*list[0],wanted,args.host.output,bpm);
        const auto& state=observation==0?input.original:canonical;const juce::String stem=observation==0?"captured-quality":"other-quality-variant";
        const auto documentFile=args.host.output.getChildFile(stem+".airackproject");writeBlock(documentFile,canonical);
        auto baseline=renderCandidateState(format,*list[0],wanted,state,512,bpm,false,true,nullptr,&args.host.output,external);auto row=jsonObject();property(row,"quality",quality==airack::RenderQuality::higher?"higher":"realtime");property(row,"original_selected_quality",observation==0);
        property(row,"canonical_observation_project",identity(documentFile));property(row,"latency_samples",baseline.render.latencySamples);property(row,"prepared_before_editor",true);property(row,"registry",baseline.registry);
        if(!saturationAudio.isVoid())property(row,"saturation_audio",saturationAudio);
        auto save=[&](const juce::String& suffix,const SceneRendered& rendered){const auto wav=args.host.output.getChildFile(stem+"-"+suffix+".wav"),opaque=args.host.output.getChildFile(stem+"-"+suffix+".state");writeWav(wav,rendered.render.audio,48000);writeBlock(opaque,rendered.render.hostState);
            auto artifact=jsonObject();property(artifact,"wav",identity(wav));property(artifact,"complete_state",identity(opaque));property(artifact,"callbacks",rendered.callbacks);const auto measurements=measureWindow(rendered.render.audio,{{0,rendered.render.audio.getNumSamples()}});property(artifact,"peak",measurements.peak);property(artifact,"rms",measurements.rms);property(artifact,"float_wav_readback_exact",true);return artifact;};
        property(row,"loaded512_lifecycle",save("loaded512",baseline));const auto initialFile=args.host.output.getChildFile(stem+"-initial-opaque.state");writeBlock(initialFile,baseline.initialOpaque);property(row,"initial_opaque",identity(initialFile));
        auto compare=[&](const char* suffix,SceneRendered rendered){const auto delta=difference(baseline.render.audio,rendered.render.audio);property(row,(juce::String(suffix)+"_maximum_difference").toRawUTF8(),delta);
            property(row,suffix,save(suffix,rendered));require(delta<=1e-6,"Candidate partition/opaque/native difference exceeds existing unfitted policy");};
        compare("loaded64",renderCandidateState(format,*list[0],wanted,state,64,bpm,false,false,nullptr,&args.host.output,external));
        compare("opaque512",renderCandidateState(format,*list[0],wanted,state,512,bpm,false,false,&baseline.initialOpaque,&args.host.output,external));
        compare("native512",renderCandidateState(format,*list[0],wanted,state,512,bpm,true,false,nullptr,nullptr,external));trials.append(row);}
    property(report,"observations",trials);property(report,"module_after",identity(binaryIn(args.host.plugin)));property(report,"render_executable_after",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    require(hash(binaryIn(args.host.plugin))==moduleBefore["sha256"].toString()&&hash(juce::File::getSpecialLocation(juce::File::currentExecutableFile))==helperBefore["sha256"].toString(),"Candidate module/helper changed during replay");
    require(juce::SHA256(input.original).toHexString()==report["original_input_sha256"].toString(),"Original captured candidate bytes changed");
    if(external){property(report,"sidechain_after",identity(args.sidechain));require(hash(args.sidechain)==sidechainBefore["sha256"].toString(),"Sidechain file changed during replay");}
    property(report,"candidate_engine_automation","COMPLETED_AS_RECORDED_IN_LOADED512_LIFECYCLE_CALLBACKS; synth-free input is explicitly NOT_APPLICABLE");
    property(report,"status","FINITE_LOADED_TECHNICAL_REPLAY_COMPLETE");property(report,"success",true);property(report,"listening","DEFERRED_BY_USER");property(report,"commercial_parity","UNVERIFIED");
    writePatternJson(args.host.output.getChildFile("report.json"),report);std::cout<<"CandidateReplay complete quality_observations=2 loaded64_512_opaque_native_full_project=PASS lifecycle_prepared=PASS original_audition=NOT_CLAIMED\n";return 0;
}
int probeEqSidechainBuses(const juce::StringArray& args)
{
    require(args.size()==5&&args[0]=="--probe-eq-sidechain-buses"&&args[1]=="--plugin"&&args[3]=="--output",
        "Usage: RackSynthRender --probe-eq-sidechain-buses --plugin ABS_VST3 --output FRESH_ABS_WORKSPACE_DIRECTORY");
    const auto host=parseArguments({"--plugin",args[2],"--output",args[4]});
    require(!host.output.exists()&&!fs::exists(fs::symlink_status(fs::path(host.output.getFullPathName().toWideCharPointer()))),"Bus probe output must be fresh, including links");
    require(host.output.createDirectory().wasOk(),"Cannot create bus probe output");
    auto report=jsonObject();property(report,"success",false);property(report,"scope","ACTUAL_VST3_BUS_TOPOLOGY_ONLY; external-triggered audio and quality alignment remain untested");
    const auto before=identity(binaryIn(host.plugin));property(report,"module_before",before);
    property(report,"render_executable",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> types;format.findAllTypesForFile(types,host.plugin.getFullPathName());
    require(types.size()==1&&isAirInstrument(*types[0]),"Expected actual instrument bundle");
    juce::String error;auto processor=format.createInstanceFromDescription(*types[0],48000,64,error);require(processor!=nullptr,"Cannot instantiate bus-probe instrument");
    const auto* extension=processor->getVST3Client();require(extension!=nullptr,"Actual VST3 component access unavailable");
    auto* component=extension->getIComponentPtr();require(component!=nullptr,"Actual VST3 component missing");
    Steinberg::TUID controller{};require(component->getControllerClassId(controller)==Steinberg::kResultOk,"Controller class ID unavailable");
    char controllerText[33]{};Steinberg::FUID(controller).toString(controllerText);
    const auto preset=extension->getPreset();require(preset.getSize()>=48&&std::memcmp(preset.getData(),"VST3",4)==0,"Actual VST3 preset header unavailable");
    const auto componentId=juce::String::fromUTF8(static_cast<const char*>(preset.getData())+8,32).toUpperCase();
    property(report,"component_class_id",componentId);property(report,"controller_class_id",juce::String(controllerText).toUpperCase());
    property(report,"host_unique_id",types[0]->uniqueId);
    require(componentId=="ABCDEF019182FAEB4172737041727331"&&juce::String(controllerText).equalsIgnoreCase("ABCDEF011234ABCD4172737041727331"),"Historical instrument class IDs changed");
    const auto inputs=component->getBusCount(Steinberg::Vst::kAudio,Steinberg::Vst::kInput);
    const auto outputs=component->getBusCount(Steinberg::Vst::kAudio,Steinberg::Vst::kOutput);
    property(report,"actual_audio_inputs",inputs);property(report,"actual_audio_outputs",outputs);
    auto buses=jsonObject();bool sdkDefaults=inputs==1&&outputs==1;int mainInputs=0;
    for(const auto direction:{Steinberg::Vst::kInput,Steinberg::Vst::kOutput}){
        juce::Array<Json> rows;
        for(int i=0;i<component->getBusCount(Steinberg::Vst::kAudio,direction);++i){
            Steinberg::Vst::BusInfo info{};require(component->getBusInfo(Steinberg::Vst::kAudio,direction,i,info)==Steinberg::kResultOk,"Actual bus enumeration failed");
            auto row=jsonObject();property(row,"index",i);property(row,"channels",info.channelCount);property(row,"type",info.busType==Steinberg::Vst::kAux?"aux":"main");property(row,"default_active",bool(info.flags&Steinberg::Vst::BusInfo::kDefaultActive));rows.add(row);
            const bool expectedAux=direction==Steinberg::Vst::kInput;
            if(expectedAux&&info.busType==Steinberg::Vst::kMain)++mainInputs;
            sdkDefaults&=info.channelCount==2&&info.busType==(expectedAux?Steinberg::Vst::kAux:Steinberg::Vst::kMain)
                &&bool(info.flags&Steinberg::Vst::BusInfo::kDefaultActive)==(direction==Steinberg::Vst::kOutput);
        }
        property(buses,direction==Steinberg::Vst::kInput?"inputs":"outputs",rows);
    }
    property(report,"actual_buses",buses);property(report,"actual_main_audio_inputs",mainInputs);property(report,"sdk_disabled_default_aux_metadata",sdkDefaults);
    property(report,"native_host_input_buses",processor->getBusCount(true));property(report,"native_host_output_buses",processor->getBusCount(false));
    property(report,"status","BUS_METADATA_RECORDED_BEFORE_TOPOLOGY_ASSERTIONS");writePatternJson(host.output.getChildFile("metadata.json"),report);
    require(sdkDefaults&&mainInputs==0&&processor->getBusCount(true)==1&&processor->getBusCount(false)==1,"Auxiliary-only actual bus topology is not available");
    const auto disabled=processor->getBusesLayout();
    require(disabled.inputBuses[0].isDisabled()&&disabled.outputBuses[0]==juce::AudioChannelSet::stereo(),"Default instrument I/O changed");
    auto stereo=disabled;stereo.inputBuses.set(0,juce::AudioChannelSet::stereo());
    require(processor->setBusesLayout(stereo)&&processor->getBusesLayout()==stereo,"Actual auxiliary stereo arrangement rejected");
    auto mono=stereo;mono.inputBuses.set(0,juce::AudioChannelSet::mono());require(!processor->setBusesLayout(mono)&&processor->getBusesLayout()==stereo,"Mono auxiliary input was admitted or changed layout");
    auto output=stereo;output.outputBuses.set(0,juce::AudioChannelSet::mono());require(!processor->setBusesLayout(output)&&processor->getBusesLayout()==stereo,"Mono output was admitted");
    require(processor->setBusesLayout(disabled)&&processor->getBusesLayout()==disabled&&processor->getTotalNumInputChannels()==0,"Auxiliary disconnect failed");
    require(component->getBusCount(Steinberg::Vst::kEvent,Steinberg::Vst::kInput)==1,"Historical MIDI input bus changed");
    property(report,"stereo_aux_enable_disable_and_invalid_layout_refusals",true);property(report,"default_total_input_channels",processor->getTotalNumInputChannels());
    property(report,"module_after",identity(binaryIn(host.plugin)));require(hash(binaryIn(host.plugin))==before["sha256"].toString(),"Module changed during bus probe");
    property(report,"status","ACTUAL_VST3_AUX_BUS_FEASIBILITY_PASS");property(report,"success",true);writePatternJson(host.output.getChildFile("report.json"),report);
    std::cout<<"Actual VST3 sidechain bus: disabled main/optional stereo aux/stereo output and historical class IDs PASS; routing untested\n";return 0;
}
// Candidate helper tests are authored before the implementation draft.
// Neither RED nor GREEN has been compiled or invoked in held preparation.
// Keep independent candidate fixture phases in separate Debug stack frames.
template<class Check>
void candidateAutomationSelfTest(airack::ProjectDocument& eqAutomation, Check& check)
{
    check(candidateDynamicEqAutomation(eqAutomation).empty(),"no EQ fixture has no extra automation probes");
    const auto absentGroup=candidateEqGroupTrial(eqAutomation);
    check(absentGroup.edits.empty()&&absentGroup.expected==eqAutomation,"no EQ group is explicitly inapplicable and preserves the full Project");
    const auto absentGain=candidateEqGainTrial(eqAutomation);
    check(absentGain.edits.empty()&&absentGain.expected==eqAutomation,"no EQ gain trial preserves the complete Project");
    for(const std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)}){
        airack::ProjectDocument input;input.sound.controlLocks[0]=true;input.mode=airack::PlaybackMode::internalPattern;
        input.pattern.noteCount=1;input.pattern.notes[0]={0,.5,64,90,1};
        require(airack::editRackLayout(input.sound.layout,{airack::RackEditKind::addEffect,slot,airack::masterChain,0,airack::EffectKind::dynamicEq},input.sound.layout)==airack::RackEditResult::applied,"gain trial slot fixture");
        const auto first=airack::dynamicEqGainParameterStart+slot*airack::dynamicEqGainParametersPerSlot;
        input.sound.values[first]=slot==0?0.f:1.f;input.sound.values[first+1]=slot==0?.5f:.25f;
        const auto gain=candidateEqGainTrial(input);check(gain.edits.size()==2,"gain trial changes exactly two fields");auto wanted=input;
        std::array<bool,2> seen{};
        for(const auto& edit:gain.edits){check(edit.index==first||edit.index==first+1,"gain trial resolves its actual physical slot");
            if(edit.index!=first&&edit.index!=first+1)continue;const auto offset=edit.index-first;
            check(!seen[offset],"gain trial keys are distinct");seen[offset]=true;
            check(edit.value!=input.sound.values[edit.index]&&edit.value>=0&&edit.value<=1,"gain trial sends changed bounded host values");
            wanted.sound.values[edit.index]=edit.value;}
        check(seen[0]&&seen[1],"gain trial covers both Auto Gain and Scale");
        check(gain.expected==wanted&&airack::validateProjectDocument(gain.expected),"gain trial preserves unrelated sound, locks, pattern and mode");
    }
    require(airack::editRackLayout(eqAutomation.sound.layout,{airack::RackEditKind::addEffect,0,airack::masterChain,0,airack::EffectKind::dynamicEq},eqAutomation.sound.layout)==airack::RackEditResult::applied,"EQ automation fixture");
    check(candidateDynamicEqAutomation(eqAutomation)==std::vector<std::size_t>{1231,1942,1958,2022,2190,2534,2734},"EQ fixture retains every preceding bank endpoint and appends the new catalog endpoint");
    for(const auto index:candidateDynamicEqAutomation(eqAutomation)){
        auto edited=eqAutomation;edited.sound.values[index]=candidateAutomationValue(index,edited.sound.values[index]);
        check(airack::validateProjectDocument(edited),"host endpoint automation respects continuous and discrete parameter domains");
    }
    for(bool legacy:{false,true})for(int shape=0;shape<(legacy?6:4);++shape)for(int slope=0;slope<(legacy?2:5);++slope){
        const auto trial=candidateEqExtensionTrial(eqAutomation,shape,slope,legacy);
        check(trial.edits.size()==(legacy?256:128),"EQ extension host probe covers every registered band in all eight banks");
        auto wanted=eqAutomation;std::array<bool,airack::parameterCount> seen{};
        for(const auto& edit:trial.edits){check(edit.index<airack::parameterCount&&!seen[edit.index],"EQ extension host fields are unique registered targets");if(edit.index>=airack::parameterCount)continue;seen[edit.index]=true;wanted.sound.values[edit.index]=edit.value;}
        auto independent=eqAutomation;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){
            independent.sound.values[2063+slot*16+band*2]=legacy?0.f:float(shape)/3;
            independent.sound.values[2064+slot*16+band*2]=legacy?0.f:float(slope)/4;
            if(legacy){independent.sound.values[1231+slot*89+band*11+2]=float(shape)/5;independent.sound.values[1231+slot*89+band*11+6]=float(slope);}
        }
        check(trial.expected==wanted&&trial.expected==independent&&airack::validateProjectDocument(trial.expected),"All extension and historical6/2host choices preserve every unrelated Project field");
    }
    for(int source=0;source<4;++source)for(int flags=0;flags<4;++flags){
        const auto trial=candidateEqDetectorTrial(eqAutomation,source,flags);check(trial.edits.size()==320,"Detector host trial covers all five controls in all64bands");if(trial.edits.size()!=320)continue;
        auto wanted=eqAutomation;std::array<bool,airack::parameterCount> seen{};
        for(const auto& edit:trial.edits){check(edit.index>=2215&&edit.index<=2534&&!seen[edit.index],"Detector host targets are unique and append-only");if(edit.index>=airack::parameterCount)continue;seen[edit.index]=true;wanted.sound.values[edit.index]=edit.value;}
        auto independent=eqAutomation;for(std::size_t slot=0;slot<8;++slot)for(std::size_t band=0;band<8;++band){const auto first=2215+slot*40+band*5;independent.sound.values[first]=float(flags&1);independent.sound.values[first+1]=(flags&2)?1.f:0.f;independent.sound.values[first+2]=float(source)/3;independent.sound.values[first+3]=.2f;independent.sound.values[first+4]=.8f;}
        check(trial.expected==wanted&&trial.expected==independent&&airack::validateProjectDocument(trial.expected),"Detector source/flags/bounds preserve every unrelated Project field");
    }
    const auto group=candidateEqGroupTrial(eqAutomation);check(group.edits.size()==8,"group trial has exactly eight selected registered controls");
    auto groupExpected=eqAutomation;std::array<bool,airack::parameterCount> groupFields{};
    for(const auto& edit:group.edits){check(edit.index<airack::parameterCount,"group index is registered");if(edit.index>=airack::parameterCount)continue;
        check(!groupFields[edit.index],"group fields are unique");groupFields[edit.index]=true;
        const auto& id=airack::parameterRegistry()[edit.index].id;check(id.startsWith("effect.1.dynamic_eq.band.1.")||id.startsWith("effect.1.dynamic_eq.band.8."),"group only owns the first and eighth band in its actual slot");
        check(edit.value!=eqAutomation.sound.values[edit.index],"every grouped host probe changes its raw control");groupExpected.sound.values[edit.index]=edit.value;}
    check(group.expected==groupExpected&&airack::validateProjectDocument(group.expected),"group preserves every unrelated Project field");
    airack::ProjectDocument lastEq;require(airack::editRackLayout(lastEq.sound.layout,{airack::RackEditKind::addEffect,7,airack::masterChain,0,airack::EffectKind::dynamicEq},lastEq.sound.layout)==airack::RackEditResult::applied,"last EQ group fixture");
    const auto lastGroup=candidateEqGroupTrial(lastEq);check(lastGroup.edits.size()==8,"last physical slot has a complete group");
    for(const auto& edit:lastGroup.edits)check(airack::parameterRegistry()[edit.index].id.startsWith("effect.8.dynamic_eq.band."),"group remaps to actual physical slot instead of first bank");
}
template<class Check>
void candidateDocumentSelfTest(airack::ProjectDocument& eqAutomation, Check& check)
{
    auto accepts=[&](auto operation,const char* name){try{check(operation(),name);}catch(const std::exception&){check(false,name);}};
    auto rejects=[&](auto operation,const char* name){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}check(rejected,name);};
    airack::ProjectDocument project;project.sound=personalityFixture(0,0);
    project.mode=airack::PlaybackMode::internalPattern;project.pattern.noteCount=1;
    project.pattern.notes[0]={0,1,60,83,2};project.sound.controlLocks[0]=true;
    juce::MemoryBlock original;require(airack::encodeProjectState(project,original),"Candidate test fixture invalid");
    accepts([&]{const auto input=decodeCandidateBinary(original);return !input.soundOnly&&input.project==project&&input.original==original;},"complete saved pattern/mode/locks and exact original bytes retained");
    juce::MemoryBlock sound;require(airack::encodeSoundState(project.sound,sound),"Candidate Sound fixture invalid");
    accepts([&]{const auto input=decodeCandidateBinary(sound);return input.soundOnly&&input.project.sound==project.sound&&input.project.pattern==airack::PatternDocument{}&&input.project.mode==airack::PlaybackMode::externalMidi&&input.original==sound;},"Sound input explicitly synthesizes only a default external Project");
    auto corrupt=original;static_cast<char*>(corrupt.getData())[0]^=1;
    rejects([&]{decodeCandidateBinary(corrupt);},"wrong binary kind rejects");
    corrupt=original;static_cast<char*>(corrupt.getData())[corrupt.getSize()-1]^=1;
    rejects([&]{decodeCandidateBinary(corrupt);},"invalid complete Project digest rejects");
    corrupt=original;corrupt.append("x",1);
    rejects([&]{decodeCandidateBinary(corrupt);},"trailing candidate bytes reject");
    juce::MemoryBlock tooLarge(airack::maximumProjectStateBytes+1);
    rejects([&]{decodeCandidateBinary(tooLarge);},"oversized input rejects before codec admission");
    accepts([&]{return candidateBeatFrame(2,48000,75)==76800;},"75 BPM beat2 is sample76800 rather than fixed120 timing");
    accepts([&]{const auto p=candidatePosition(project,36000,115200,48000,100);return p.getBpm()&&*p.getBpm()==100&&p.getPpqPosition()&&*p.getPpqPosition()==1.25;},"100 BPM sample36000 supplies PPQ1.25");
    accepts([&]{const auto p=candidatePosition(project,115200,115200,48000,100);return !p.getIsPlaying()&&p.getPpqPosition()&&*p.getPpqPosition()==4;},"internal controlled four-beat stop holds PPQ4");
    auto external=project;external.mode=airack::PlaybackMode::externalMidi;
    accepts([&]{return candidatePosition(external,150000,115200,48000,100).getIsPlaying();},"external literal note release keeps host transport continuous in tail");
    accepts([&]{return candidateBpm(project,100)==100;},"declared harness BPM used when Project contains none");
    project.group.emplace();project.group->bpm=75;
    accepts([&]{return candidateBpm(project,std::nullopt)==75;},"captured group BPM is retained");
    rejects([&]{candidateBpm(project,100);},"conflicting harness tempo cannot silently replace captured group BPM");
    project.group.reset();rejects([&]{candidateBpm(project,std::nullopt);},"missing candidate and harness tempo is explicit");
    rejects([&]{candidateBpm(project,std::numeric_limits<double>::quiet_NaN());},"nonfinite harness clock rejects");
    accepts([&]{auto higher=candidateQualityVariant(project,airack::RenderQuality::higher);auto expected=project;expected.sound.values[758]=1;return higher==expected&&project.sound.values[758]==0&&original.getSize()>0;},"Higher variant changes only758 without modifying original Project");
    auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/task13-candidate-selftest-"+juce::Uuid().toString());
    require(root.createDirectory().wasOk(),"Candidate selftest temporary directory unavailable");
    const auto inputFile=root.getChildFile("captured.bin");writeBlock(inputFile,original);
    auto replaceInput=[&](const juce::MemoryBlock& bytes){require(inputFile.replaceWithData(bytes.getData(),bytes.getSize()),"Candidate mutable input fixture replacement failed");};
    accepts([&]{const auto input=readCandidateInput(inputFile);return input.original==original&&input.project==project;},"actual bounded source read retains admitted binary bytes");
    rejects([&]{readCandidateInput(root.getChildFile("missing.bin"));},"missing input rejects without loading a bundle");
    const auto opaque=hostEnvelope(original);
    accepts([&]{verifyPersonalityProject(opaque,project);return true;},"complete Project envelope validates saved pattern and mode");
    auto different=project;different.pattern.notes[0].velocity=84;
    rejects([&]{verifyPersonalityProject(opaque,different);},"Sound-equal Project with changed pattern cannot pass full envelope validation");
    // Real argument-only bundle, never loaded by these tests.
    const auto bundle=root.getChildFile("ArgumentFixture.vst3");
    const auto dll=bundle.getChildFile("Contents/x86_64-win/ArgumentFixture.vst3");
    require(dll.getParentDirectory().createDirectory().wasOk()&&dll.replaceWithText("argument-only"),"Candidate CLI fixture unavailable");
    const auto output=root.getChildFile("fresh-output");
    const juce::StringArray args{"--candidate","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--input",inputFile.getFullPathName()};
    accepts([&]{const auto parsed=parseCandidateArguments(args);return parsed.host.output==output&&parsed.input==inputFile&&!parsed.bpm&&!output.exists();},"candidate CLI accepts exact paths without creating output");
    auto tempo=args;tempo.add("--bpm");tempo.add("75");
    accepts([&]{return parseCandidateArguments(tempo).bpm==75;},"candidate CLI admits explicit bounded clock");
    auto sidechain=args;sidechain.add("--sidechain");sidechain.add(root.getChildFile("external.wav").getFullPathName());
    accepts([&]{parseCandidateArguments(sidechain);return !output.exists();},"candidate CLI admits an explicit sidechain without creating output");
    sidechain.add("--bpm");sidechain.add("75");
    accepts([&]{return parseCandidateArguments(sidechain).bpm==75;},"candidate CLI admits sidechain and controlled tempo together");
    auto relativeSidechain=sidechain;relativeSidechain.set(8,"relative.wav");
    rejects([&]{parseCandidateArguments(relativeSidechain);},"relative sidechain path rejects");
    auto outsideSidechain=sidechain;outsideSidechain.set(8,juce::File(RACKSYNTH_WORKSPACE).getParentDirectory().getChildFile("external.wav").getFullPathName());
    rejects([&]{parseCandidateArguments(outsideSidechain);},"sidechain outside canonical workspace rejects");
    auto duplicateSidechain=sidechain;duplicateSidechain.set(9,"--sidechain");duplicateSidechain.set(10,sidechain[8]);
    rejects([&]{parseCandidateArguments(duplicateSidechain);},"duplicate sidechain rejects");
    auto duplicateTempo=tempo;duplicateTempo.add("--bpm");duplicateTempo.add("100");
    rejects([&]{parseCandidateArguments(duplicateTempo);},"duplicate tempo rejects");
    rejects([&]{readCandidateSidechain(root.getChildFile("missing.wav"));},"missing sidechain WAV rejects");
    const auto saveSidechain=[&](const char* name,int channels,int rate,int frames,bool nonfinite=false){
        const auto file=root.getChildFile(name);juce::AudioBuffer<float> audio(channels,frames);audio.clear();
        if(frames>0)audio.setSample(channels-1,frames-1,nonfinite?std::numeric_limits<float>::quiet_NaN():.375f);
        juce::WavAudioFormat wav;std::unique_ptr<juce::OutputStream> stream=newStream(file);
        const auto options=juce::AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(channels).withBitsPerSample(32)
            .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        const auto writer=wav.createWriterFor(stream,options);require(writer&&writer->writeFromAudioSampleBuffer(audio,0,frames)&&writer->flush(),"Sidechain reader fixture export failed");return file;
    };
    const auto validSidechain=saveSidechain("external.wav",2,48000,17);
    accepts([&]{const auto audio=readCandidateSidechain(validSidechain);return audio.getNumChannels()==2&&audio.getNumSamples()==17&&audio.getSample(1,16)==.375f&&audio.getSample(0,16)==0;},"stereo sidechain reads exact independent channels");
    const auto monoSidechain=saveSidechain("mono.wav",1,48000,17);
    rejects([&]{readCandidateSidechain(monoSidechain);},"mono sidechain WAV rejects");
    const auto wrongRateSidechain=saveSidechain("other-rate.wav",2,44100,17);
    rejects([&]{readCandidateSidechain(wrongRateSidechain);},"sidechain rate mismatch rejects instead of resampling silently");
    const auto nonfiniteSidechain=saveSidechain("nonfinite.wav",2,48000,17,true);
    rejects([&]{readCandidateSidechain(nonfiniteSidechain);},"nonfinite sidechain WAV rejects");
    const auto emptySidechain=saveSidechain("empty.wav",2,48000,0);
    rejects([&]{readCandidateSidechain(emptySidechain);},"zero-frame sidechain WAV rejects");
    const auto longSidechain=saveSidechain("too-long.wav",2,48000,30*48000+1);
    rejects([&]{readCandidateSidechain(longSidechain);},"sidechain decoded-frame bound rejects before allocating audio");
    auto changed=args;changed.set(5,"--unknown");rejects([&]{parseCandidateArguments(changed);},"unknown input flag rejects");
    changed=args;changed.remove(6);rejects([&]{parseCandidateArguments(changed);},"missing input value rejects");
    changed=args;changed.set(6,"captured.bin");rejects([&]{parseCandidateArguments(changed);},"relative candidate input rejects");
    changed=args;changed.set(6,juce::File(RACKSYNTH_WORKSPACE).getParentDirectory().getChildFile("candidate-outside.bin").getFullPathName());
    rejects([&]{parseCandidateArguments(changed);},"canonical input outside workspace rejects before bundle load");
    changed=args;changed.set(4,juce::File(RACKSYNTH_WORKSPACE).getParentDirectory().getChildFile("candidate-output-outside").getFullPathName());
    rejects([&]{parseCandidateArguments(changed);},"output outside workspace rejects");
    changed=tempo;changed.set(8,"75tail");rejects([&]{parseCandidateArguments(changed);},"BPM trailing text rejects");
    changed=tempo;changed.set(8,"nan");rejects([&]{parseCandidateArguments(changed);},"nonfinite CLI BPM rejects");
    changed=tempo;changed.set(8,"401");rejects([&]{parseCandidateArguments(changed);},"CLI BPM above published bound rejects");
    require(output.createDirectory().wasOk(),"Candidate stale output setup failed");
    rejects([&]{parseCandidateArguments(args);},"existing empty output rejects");
    require(output.getChildFile("retained.txt").replaceWithText("retained"),"Candidate nonempty output setup failed");
    rejects([&]{parseCandidateArguments(args);},"existing nonempty output rejects without replacing contents");
    // Deterministic real-file changes at the actual preflight/open boundary.
    const auto extent=inputFile.getSize();auto grown=original;grown.append("x",1);replaceInput(grown);
    rejects([&]{juce::FileInputStream stream(inputFile);readCandidateExtent(stream,extent);},"actual input growth after preflight rejects");
    auto shrunk=original;shrunk.setSize(shrunk.getSize()-1);replaceInput(shrunk);
    rejects([&]{juce::FileInputStream stream(inputFile);readCandidateExtent(stream,extent);},"actual input shrink after preflight rejects");
    juce::MemoryInputStream zeroStream(nullptr,0,false);
    rejects([&]{readCandidateExtent(zeroStream,3);},"extent below magic rejects before stream allocation/read");
    rejects([&]{readCandidateExtent(zeroStream,juce::int64(airack::maximumProjectStateBytes)+1);},"Project cap+1 extent rejects before stream allocation/read");
    juce::MemoryBlock maxProject(airack::maximumProjectStateBytes);std::memcpy(maxProject.getData(),"AIPJ",4);replaceInput(maxProject);
    rejects([&]{readCandidateInput(inputFile);},"actual file at maximum bound still requires valid complete codec/digest");
    maxProject.setSize(airack::maximumProjectStateBytes+1);replaceInput(maxProject);
    rejects([&]{readCandidateInput(inputFile);},"actual Project cap+1 file rejects");
    juce::MemoryBlock maxSound(airack::maximumSoundStateBytes+1);std::memcpy(maxSound.getData(),"ARSI",4);
    rejects([&]{decodeCandidateBinary(maxSound);},"Sound cap+1 cannot use the larger Project bound");
    replaceInput(original);
    accepts([&]{const auto trial=candidateEngineTrial(project);if(!trial.applicable||trial.index!=17)return false;
        const auto& before=project.sound.personalities->instances.back();const auto& after=trial.expected.sound.personalities->instances.back();
        const auto* binding=airack::rackUnitBinding(after,after.design.units.front().id);
        return binding&&binding->slot==1&&after.id==before.id&&after.design.units.front().module==airack::NativeModule::wavetable
            &&after.design.revision==before.design.revision+1&&after.edited&&after.design.panel.controls==before.design.panel.controls
            &&trial.expected.pattern==project.pattern&&trial.expected.mode==project.mode&&trial.expected.sound.controlLocks==project.sound.controlLocks;
    },"engine trial uses actual owned slot and retains full pattern/mode/locks/mappings and identity");
    auto noSynth=airack::ProjectDocument{};noSynth.sound.layout.synthUse[0]=airack::SlotUse::retired;noSynth.sound.layout.synthOrder.fill(airack::noSlot);noSynth.sound.layout.synthCount=0;
    accepts([&]{const auto trial=candidateEngineTrial(noSynth);return !trial.applicable&&trial.reason=="NOT_APPLICABLE_NO_ACTIVE_SYNTH";},"synth-free input reports explicit engine applicability");
    // Positive complete Project fixtures use public validators, codecs and
    // ordinary placement admission. They are technical fixtures, not AI output.
    auto grouped=project;grouped.group.emplace();
    grouped.group->group=juce::Uuid("6e731bf8-40ad-4f4e-8277-b30d8e3b1001");
    grouped.group->member=juce::Uuid("6e731bf8-40ad-4f4e-8277-b30d8e3b1002");
    require(airack::setGroupText("Candidate ensemble",grouped.group->name)
        &&airack::setGroupText("layered pad",grouped.group->role)
        &&airack::setGroupText("A",grouped.group->key)&&airack::setGroupText("minor",grouped.group->scale),"Valid grouped candidate text setup failed");
    grouped.group->bpm=75;
    juce::MemoryBlock groupedBytes;require(airack::validateProjectDocument(grouped)&&airack::encodeProjectState(grouped,groupedBytes),"Populated grouped Project admission failed");
    accepts([&]{const auto input=decodeCandidateBinary(groupedBytes);return input.project==grouped&&input.original==groupedBytes&&!input.soundOnly
        &&candidateBpm(input.project,std::nullopt)==75&&candidateBpm(input.project,75)==75;},"valid populated grouped Project retains all group fields, pattern/mode and exact binary");
    const auto groupedFile=root.getChildFile("grouped-candidate.bin");writeBlock(groupedFile,groupedBytes);
    accepts([&]{const auto input=readCandidateInput(groupedFile);const auto position=candidatePosition(input.project,76800,153600,48000,candidateBpm(input.project,std::nullopt));
        return input.original==groupedBytes&&input.project==grouped&&position.getPpqPosition()&&*position.getPpqPosition()==2&&position.getBpm()&&*position.getBpm()==75;},"actual grouped candidate file supplies captured75 BPM and sample76800 PPQ2");
    accepts([&]{verifyPersonalityProject(hostEnvelope(groupedBytes),grouped);return true;},"complete grouped candidate opaque envelope validates group and playback state");
    auto differentGroup=grouped;differentGroup.group->bpm=76;
    rejects([&]{verifyPersonalityProject(hostEnvelope(groupedBytes),differentGroup);},"Sound-equal candidate with changed group tempo cannot pass full Project validation");
    auto full=grouped;full.sound=airack::SoundDocument{};full.sound.values[758]=1;
    const auto factories=airack::test::frozenFactoryRackDesigns();
    auto place=[&](airack::NativeModule module,std::uint8_t slot,std::uint8_t chain,std::uint8_t position,int instanceNo){
        const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& design){return design.units.size()==1&&design.units[0].module==module;});
        require(found!=factories.end(),"Capacity candidate factory module missing");
        airack::RackOperation op;op.design=*found;op.newInstance=juce::Uuid(juce::String("6e731bf840ad4f4e8277b30d8e3b00")+juce::String(instanceNo).paddedLeft('0',2));
        const bool synth=module==airack::NativeModule::analog||module==airack::NativeModule::wavetable;
        op.destination={synth?airack::RackRole::synth:airack::RackRole::fx,chain,position};op.allocation={{found->units[0].id,slot}};op.allowSlotReuse=synth&&slot==0;
        airack::SoundDocument next;airack::RackDifference difference;
        require(airack::makeRackCandidate(full.sound,op,airack::SoundEditOrigin::manual,next,difference)==airack::RackCandidateStatus::ready,"Full-capacity candidate public rack admission failed");full.sound=std::move(next);
    };
    for(std::uint8_t slot=0;slot<full.sound.layout.synthUse.size();++slot)
        place(slot%2==0?airack::NativeModule::analog:airack::NativeModule::wavetable,slot,airack::masterChain,slot,slot);
    constexpr std::array<airack::NativeModule,4> effectModules{airack::NativeModule::drive,airack::NativeModule::delay,airack::NativeModule::reverb,airack::NativeModule::compressor};
    for(std::uint8_t slot=0;slot<full.sound.layout.effects.size();++slot)
        place(effectModules[slot%effectModules.size()],slot,slot<4?slot:airack::masterChain,slot<4?0:std::uint8_t(slot-4),slot+4);
    full.pattern.noteCount=16;for(int n=0;n<16;++n)full.pattern.notes[std::size_t(n)]={0,4,std::uint8_t(48+n),100,1};
    require(airack::validateProjectDocument(full),"Full-capacity layered/grouped/Higher Project fixture invalid");
    juce::MemoryBlock fullBytes;require(airack::encodeProjectState(full,fullBytes),"Full-capacity Project encoding failed");
    accepts([&]{const auto input=decodeCandidateBinary(fullBytes);if(input.project!=full||input.original!=fullBytes||input.soundOnly
        ||full.sound.layout.synthCount!=4||full.sound.personalities->instances.size()!=12||full.pattern.noteCount!=16||full.sound.values[758]!=1)return false;
        for(std::size_t slot=0;slot<4;++slot)if(full.sound.layout.synthUse[slot]!=airack::SlotUse::present||full.sound.layout.synthOrder[slot]!=slot
            ||airack::parameterPhysicalValue(full.sound.values,13+4*slot)!=(slot%2?1:0)||full.sound.layout.effectCount[slot]!=1||full.sound.layout.effectOrder[slot][0]!=slot)return false;
        if(full.sound.layout.effectCount[airack::masterChain]!=4)return false;
        for(std::size_t n=0;n<4;++n)if(full.sound.layout.effectOrder[airack::masterChain][n]!=n+4)return false;
        return candidateBpm(input.project,std::nullopt)==75;},"full-capacity four mixed independent layers/16 notes/eight ordered layer-master FX and Higher/group tempo roundtrip exactly");
    const auto fullFile=root.getChildFile("full-capacity-candidate.bin");writeBlock(fullFile,fullBytes);
    accepts([&]{return readCandidateInput(fullFile).original==fullBytes&&readCandidateInput(fullFile).project==full;},"actual full-capacity candidate binary file retains complete Project");
    accepts([&]{verifyPersonalityProject(hostEnvelope(fullBytes),full);return true;},"full-capacity grouped/layered/Higher Project opaque envelope validates");
    accepts([&]{auto realtime=candidateQualityVariant(full,airack::RenderQuality::realtime);auto expected=full;expected.sound.values[758]=0;
        return realtime==expected&&full.sound.values[758]==1&&candidateBeatFrame(4,48000,candidateBpm(full,std::nullopt))==153600;},"full-capacity variant changes only quality while original Higher and75 BPM clock remain");
    accepts([&]{const auto trial=candidateEngineTrial(full);if(!trial.applicable||trial.index!=13||trial.normalized!=1
        ||trial.expected.group!=full.group||trial.expected.pattern!=full.pattern||trial.expected.mode!=full.mode||trial.expected.sound.layout!=full.sound.layout)return false;
        auto expectedValues=full.sound.values;expectedValues[13]=1;if(trial.expected.sound.values!=expectedValues)return false;
        for(std::size_t n=1;n<full.sound.personalities->instances.size();++n)if(trial.expected.sound.personalities->instances[n]!=full.sound.personalities->instances[n])return false;
        return trial.expected.sound.personalities->instances[0].id==full.sound.personalities->instances[0].id;
    },"capacity engine automation preserves every other layer/FX snapshot, values, group and pattern");
    for(int mode=0;mode<5;++mode){const auto trial=candidateEqPlacementTrial(eqAutomation,mode);
        check(trial.edits.size()==2&&trial.edits.front().index==1959&&trial.edits.back().index==2022,"placement first/last registered host targets");
        auto values=eqAutomation.sound.values;values[1959]=values[2022]=float(mode)/4;auto expected=eqAutomation;expected.sound.values=values;
        check(trial.expected==expected&&airack::validateProjectDocument(trial.expected),"all five placement choices change only expected raw fields");
    }
    std::cout<<"CANDIDATE_FIXTURE technical_grouped="<<groupedFile.getFullPathName()<<" sha256="<<juce::SHA256(groupedBytes).toHexString()<<'\n';
    std::cout<<"CANDIDATE_FIXTURE technical_full_capacity="<<fullFile.getFullPathName()<<" sha256="<<juce::SHA256(fullBytes).toHexString()<<'\n';
}
template<class Check>
void candidateSaturationReferenceSelfTest(Check& check)
{
    const auto factory=airack::test::frozenFactoryRackDesigns();
    const auto find=[&](const char* name)->airack::RackDesign{const auto found=std::find_if(factory.begin(),factory.end(),[&](const auto& d){return d.name==name;});require(found!=factory.end(),"Composite reference factory absent");return *found;};
    auto design=find("Character Drive");design.id=juce::Uuid("0aa6b771-9a22-4bf4-ae20-914777bc0100");design.name="Composite reference regression";
    auto eq=find("Contour Loom").units.front(),second=design.units.front();eq.id=1;second.id=2;
    for(auto& value:eq.defaults)value.unit=eq.id;for(auto& value:second.defaults)value.unit=second.id;
    design.units.push_back(eq);design.units.push_back(second);
    auto documents=std::make_unique<std::array<airack::ProjectDocument,3>>();auto& project=(*documents)[0];auto& expected=(*documents)[1];auto& raw=(*documents)[2];
    project.sound=airack::copperArcSound();airack::RackOperation op;op.kind=airack::RackOperationKind::place;op.design=design;
    op.newInstance=juce::Uuid("0aa6b771-9a22-4bf4-ae20-914777bc0101");op.destination={airack::RackRole::fx,airack::masterChain,0};op.allocation={{0,0},{1,1},{2,7}};
    auto placed=std::make_unique<airack::SoundDocument>();airack::RackDifference delta;
    require(airack::makeRackCandidate(project.sound,op,airack::SoundEditOrigin::manual,*placed,delta)==airack::RackCandidateStatus::ready,"Composite reference rack placement rejected");project.sound=std::move(*placed);
    const auto eqGain=airack::rackParameterIndex(airack::NativeModule::eq,1,"eq.low_shelf.gain_db");require(eqGain.has_value(),"Composite EQ gain absent");
    const auto& descriptor=airack::parameterRegistry()[*eqGain];project.sound.values[*eqGain]=juce::NormalisableRange<float>(descriptor.minimum,descriptor.maximum,0,descriptor.skew).convertTo0to1(12);
    project.sound.values[572]=project.sound.values[572+7*23]=0;project.sound.controlLocks[3]=true;
    project.mode=airack::PlaybackMode::internalPattern;project.pattern.noteCount=1;project.pattern.notes[0]={0,2,48,100,1};
    check(airack::validateProjectDocument(project),"composite two-saturation and audible EQ Project is valid");
    expected=raw=project;auto owners=std::make_shared<airack::RackPersonalityState>(*expected.sound.personalities);
    std::erase_if(owners->instances,[&](const auto& instance){return instance.id==op.newInstance;});expected.sound.personalities=owners;
    expected.sound.layout.effects[0].use=expected.sound.layout.effects[7].use=airack::SlotUse::retired;
    expected.sound.layout.effectOrder[airack::masterChain].fill(airack::noSlot);expected.sound.layout.effectOrder[airack::masterChain][0]=1;expected.sound.layout.effectCount[airack::masterChain]=1;
    bool constructed=true;try{removeCandidateSaturation(project,raw);}catch(const std::exception&){constructed=false;}
    check(constructed,"raw reference removes both saturation units sharing one owner");
    check(raw==expected,"raw reference preserves complete Project, unrelated EQ, values and source ownership");
    check(airack::validateProjectDocument(raw)&&candidateSaturationAutomation(raw).empty(),"raw reference is valid with saturation genuinely absent");
    juce::MemoryBlock bytes;auto recalled=std::make_unique<airack::ProjectDocument>();
    check(airack::encodeProjectState(raw,bytes)&&airack::decodeProjectState(bytes.getData(),bytes.getSize(),*recalled)&&*recalled==expected,"raw composite reference complete Project roundtrip");
}
int candidateReplaySelfTest()
{
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cerr<<"FAIL candidate: "<<name<<'\n';}};
    airack::ProjectDocument eqAutomation;
    check(candidateSaturationAutomation(eqAutomation).empty(),"no saturation has no extra automation probes");
    {auto saturation=eqAutomation;for(const std::uint8_t slot:{std::uint8_t(0),std::uint8_t(7)})require(airack::editRackLayout(saturation.sound.layout,{airack::RackEditKind::addEffect,slot,airack::masterChain,std::uint8_t(saturation.sound.layout.effectCount[airack::masterChain]),airack::EffectKind::saturation},saturation.sound.layout)==airack::RackEditResult::applied,"saturation automation self-test placement");
        const auto edits=candidateSaturationAutomation(saturation);check(edits.size()==140,"all35 fields at two endpoints in both present saturation slots");
        std::map<std::size_t,std::set<float>> observed;for(const auto& edit:edits){auto trial=saturation;trial.sound.values[edit.index]=edit.value;check(airack::validateProjectDocument(trial),"saturation endpoint is valid including inactive banks");observed[edit.index].insert(edit.value);}
        for(const std::size_t slot:{0u,7u})for(std::size_t field=0;field<35;++field)check(observed[airack::saturationParameterStart+slot*35+field].size()==2,"each saturation field has two distinct endpoints");}
    candidateSaturationReferenceSelfTest(check);
    candidateAutomationSelfTest(eqAutomation,check);
    candidateDocumentSelfTest(eqAutomation,check);
    std::cout<<"CandidateReplay self-test checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
int renderRackPersonalities(const Arguments& args)
{
    require(!args.output.exists(),"Personality output must be absent");juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> list;format.findAllTypesForFile(list,args.plugin.getFullPathName());require(list.size()==1&&isAirInstrument(*list[0]),"Wrong personality plugin");require(args.output.createDirectory().wasOk(),"Personality output creation failed");
    auto report=jsonObject();property(report,"scope","Actual VST3 audio/state/host-ID/lifecycle; native panel pixels are separate; human decisions NOT_RUN");property(report,"module_before",identity(binaryIn(args.plugin)));property(report,"render_executable",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));const auto helper=args.plugin.getChildFile("Contents/x86_64-win/AI Rack Synth Companion.exe");property(report,"helper_present",helper.existsAsFile());if(helper.existsAsFile())property(report,"helper_before",identity(helper));
    Json legacyManifest,legacyReport,legacyFiles(juce::Array<Json>{}),legacyComparisons(juce::Array<Json>{});juce::File legacyReportFile;
    const bool replayRequested=args.baseline!=juce::File();
    require(!replayRequested||(args.baseline.existsAsFile()&&args.baseline.getSize()>0&&args.baseline.getSize()<=4*1024*1024),"Requested frozen baseline disappeared or exceeds bounds");
    const auto manifestHash=replayRequested?hash(args.baseline):juce::String();
    auto checkedLegacyFile=[&](const juce::File& file){
        const auto canonical=fs::weakly_canonical(fs::path(file.getFullPathName().toWideCharPointer()));
        const auto root=fs::weakly_canonical(fs::path(juce::File(RACKSYNTH_WORKSPACE).getFullPathName().toWideCharPointer()));const auto relative=canonical.lexically_relative(root);
        require(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="..","Legacy personality input escapes workspace");
        int matches=0;Json entry;for(const auto& candidate:*legacyManifest.getArray())if(juce::File(candidate["path"].toString())==file){++matches;entry=candidate;}
        require(matches==1&&file.existsAsFile()&&juce::int64(entry["bytes"])==file.getSize()&&entry["sha256"].toString().equalsIgnoreCase(hash(file)),"Frozen legacy personality identity missing/mismatched");
        legacyFiles.append(identity(file));return file;
    };
    if(replayRequested){
        legacyManifest=juce::JSON::parse(args.baseline.loadFileAsString());require(legacyManifest.isArray()&&legacyManifest.size()==1218,"Expected complete frozen Task1 output manifest");
        int reports=0;for(const auto& entry:*legacyManifest.getArray())if(entry["path"].toString().replaceCharacter('\\','/').endsWith("/sound-quality-legacy-personalities-20261009-a/report.json")){legacyReportFile=juce::File(entry["path"].toString());++reports;}
        require(reports==1,"Frozen Task1 personality report missing/ambiguous");checkedLegacyFile(legacyReportFile);require(legacyReportFile.getSize()<=4*1024*1024,"Legacy personality report exceeds bound");legacyReport=juce::JSON::parse(legacyReportFile.loadFileAsString());
        require(bool(legacyReport["success"])&&legacyReport["fixtures"].isArray()&&legacyReport["fixtures"].size()==15,"Frozen personality workload contract changed");
        property(report,"legacy_manifest",identity(args.baseline));property(report,"legacy_report",identity(legacyReportFile));
    }
    auto replayLegacy=[&](const juce::String& stem,const airack::ProjectDocument& current,const juce::AudioBuffer<float>& currentAudio,bool duplicate=false){
        if(!replayRequested)return;
        int rows=0;Json row;for(const auto& candidate:*legacyReport["fixtures"].getArray())if(candidate["name"].toString()==stem){++rows;row=candidate;}
        require(rows==1,"Frozen personality workload missing/ambiguous");
        const auto projectFile=checkedLegacyFile(juce::File(row[duplicate?"state":"project"]["path"].toString()));juce::MemoryBlock oldProject;
        require(projectFile.getSize()<=airack::maximumProjectStateBytes&&projectFile.loadFileAsData(oldProject)&&oldProject.getSize()>24,"Frozen project unreadable/bounded");
        const auto* bytes=static_cast<const unsigned char*>(oldProject.getData());require(bytes[4]==3&&bytes[24]==4,"Legacy replay must use captured Project3/Sound4");airack::ProjectDocument wanted;
        require(airack::decodeProjectState(oldProject.getData(),oldProject.getSize(),wanted)&&wanted.sound.values[758]==0&&wanted.sound.compatibility==airack::KernelCompatibility{},"Frozen legacy project migration failed");
        if(!duplicate)require(wanted==legacyPersonalityExpectation(current),"Legacy personality captured intent and exact frozen package refs differ from workload");
        else require(wanted.sound.values==current.sound.values&&wanted.sound.layout==current.sound.layout&&wanted.sound.macros==current.sound.macros,"Legacy duplicate numeric/routing intent changed");
        if(!duplicate){const auto soundFile=checkedLegacyFile(juce::File(row["sound"]["path"].toString()));juce::MemoryBlock oldSound;airack::SoundDocument sound;
            require(soundFile.getSize()>7024&&soundFile.getSize()<=airack::legacyMaximumSoundStateBytes&&soundFile.loadFileAsData(oldSound)&&static_cast<const unsigned char*>(oldSound.getData())[4]==4&&airack::decodeSoundState(oldSound.getData(),oldSound.getSize(),sound)&&sound==wanted.sound,"Frozen Sound4 captured migration changed");}
        const auto opaqueFile=checkedLegacyFile(projectFile.getSiblingFile(stem+".hoststate"));const auto wavFile=checkedLegacyFile(projectFile.getSiblingFile(stem+".wav"));
        juce::MemoryBlock opaque;require(opaqueFile.getSize()<=4*1024*1024&&opaqueFile.loadFileAsData(opaque),"Legacy opaque input unreadable/bounded");require(personalityComponent(opaque)==wanted,"Captured opaque state differs from captured project");
        juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(wavFile.createInputStream().release(),true));
        require(reader&&reader->usesFloatingPointData&&reader->bitsPerSample==32&&reader->numChannels==2&&reader->sampleRate==48000&&reader->lengthInSamples==288000,"Frozen personality WAV geometry changed");juce::AudioBuffer<float> reference(2,288000);require(reader->read(&reference,0,288000,0,true,true)&&measureWindow(reference,{{0,288000}}).rms>0,"Frozen personality reference unreadable/silent");
        auto replay512=renderPersonality(format,*list[0],wanted,512,&opaque);auto replay64=renderPersonality(format,*list[0],wanted,64,&opaque);
        const auto a=difference(reference,replay512.render.audio),b=difference(reference,replay64.render.audio),c=difference(currentAudio,replay512.render.audio);require(a<=1e-6&&b<=1e-6&&c<=1e-6,"Loaded legacy personality audio changed");
        const auto output512=args.output.getChildFile(stem+"-legacy512.wav"),output64=args.output.getChildFile(stem+"-legacy64.wav");writeWav(output512,replay512.render.audio,48000);writeWav(output64,replay64.render.audio,48000);writeBlock(args.output.getChildFile(stem+"-legacy-returned.hoststate"),replay512.render.hostState);
        auto item=jsonObject();property(item,"name",stem);property(item,"input_project",identity(projectFile));property(item,"input_opaque",identity(opaqueFile));property(item,"reference_wav",identity(wavFile));property(item,"replay512",identity(output512));property(item,"replay64",identity(output64));property(item,"legacy512_difference",a);property(item,"legacy64_difference",b);property(item,"current_difference",c);property(item,"gate",1e-6);legacyComparisons.append(item);std::cout<<"LEGACY_PERSONALITY "<<stem<<" captured opaque512="<<a<<" opaque64="<<b<<" current="<<c<<'\n';
    };
    Json fixtures(juce::Array<Json>{});const auto designs=airack::test::frozenExampleRackDesigns();
    for(int n=0;n<3;++n){std::array<double,2> response{};for(int setting=-1;setting<=1;++setting)for(bool master:{false,true}){if(n!=2&&master)continue;auto document=personalityFixture(n,setting,master);if(setting!=0)require(personalityEndpointMatches(document,n,setting),"Independent golden personality endpoint/key/slot mismatch");airack::ProjectDocument project;project.sound=document;
        const auto stem=designs[n].name.replaceCharacter(' ','-')+(master?"-master":"-insert")+(setting<0?"-low":setting>0?"-high":"-default");juce::MemoryBlock owned;require(airack::encodeProjectState(project,owned),"Personality owned encoding failed");writeBlock(args.output.getChildFile(stem+".airackproject"),owned);const auto soundPath=args.output.getChildFile(stem+".soundstate");juce::MemoryBlock sound;require(airack::encodeSoundState(document,sound),"Personality sound encoding failed");writeBlock(soundPath,sound);
        auto baseline=renderPersonality(format,*list[0],project,512,nullptr,setting==0);auto duplicate=renderPersonality(format,*list[0],project,512);auto recall=renderPersonality(format,*list[0],project,64,&baseline.render.hostState);
        replayLegacy(stem,project,baseline.render.audio);
        const auto a=difference(baseline.render.audio,duplicate.render.audio),b=difference(baseline.render.audio,recall.render.audio);require(a<=1e-6&&b<=1e-6,"Personality duplicate/recall/partition difference exceeded");
        const auto measured=measureWindow(baseline.render.audio,{{0,288000}});require(measured.peak<1&&measured.rms>0,"Personality audio clipped/silent");
        writeWav(args.output.getChildFile(stem+".wav"),baseline.render.audio,48000);writeWav(args.output.getChildFile(stem+"-duplicate.wav"),duplicate.render.audio,48000);writeWav(args.output.getChildFile(stem+"-recall64.wav"),recall.render.audio,48000);writeBlock(args.output.getChildFile(stem+".hoststate"),baseline.render.hostState);
        const std::vector<Event> midi{{0,juce::MidiMessage::noteOn(1,48,juce::uint8(100))},{960,juce::MidiMessage::noteOff(1,48)}};writeMidiEvents(args.output.getChildFile(stem+".mid"),midi);
        auto item=jsonObject();property(item,"name",stem);property(item,"project",identity(args.output.getChildFile(stem+".airackproject")));property(item,"opaque",identity(args.output.getChildFile(stem+".hoststate")));property(item,"sound",identity(soundPath));property(item,"wav",identity(args.output.getChildFile(stem+".wav")));property(item,"midi",identity(args.output.getChildFile(stem+".mid")));property(item,"registry",baseline.registry);property(item,"peak",measured.peak);property(item,"rms",measured.rms);property(item,"duplicate_difference",a);property(item,"recall64_difference",b);property(item,"editor_lifecycle_and_automation",setting==0);
        Json settings(juce::Array<Json>{});for(const auto& rack:document.personalities->instances)for(const auto& control:rack.design.panel.controls){const auto bind=std::find_if(rack.bindings.begin(),rack.bindings.end(),[&](auto v){return v.unit==control.unit;});const auto unit=std::find_if(rack.design.units.begin(),rack.design.units.end(),[&](const auto& v){return v.id==control.unit;});const auto index=*airack::rackParameterIndex(unit->module,bind->slot,control.key);auto v=jsonObject();property(v,"label",control.label);property(v,"id",airack::parameterRegistry()[index].id);property(v,"physical",airack::parameterPhysicalValue(document.values,index));property(v,"normalized",document.values[index]);settings.append(v);}property(item,"settings",settings);
        const auto metric=rackPersonalityFixtures::responseMetric(n,baseline.render.audio,[](bool good){require(good,"Shared response measurement has no energy");});property(item,"response_metric",metric);property(item,"metric_window",n==0?"Hann8192 from frame12000, high>=4kHz at48k":n==1?"late squared energy frames96000..192000":"active squared energy frames12000..36000");if(setting!=0&&!master)response[setting<0?0:1]=metric;fixtures.append(item);std::cout<<"PERSONALITY "<<stem<<" peak="<<measured.peak<<" response="<<metric<<" duplicate="<<a<<" recall64="<<b<<'\n';
    }require(response[0]<response[1],"Personality response trend missing");}
    // A real RackOperation duplicate retains the source snapshot/values but gets new physical banks.
    for(int n=0;n<3;++n){auto original=personalityFixture(n,0);const auto selected=original.personalities->instances.back();airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected.id;op.newInstance=juce::Uuid();op.destination={selected.design.role,airack::masterChain,std::uint8_t(selected.design.role==airack::RackRole::synth?2:0)};airack::SoundDocument copied;airack::RackDifference delta;require(airack::makeRackCandidate(original,op,airack::SoundEditOrigin::manual,copied,delta)==airack::RackCandidateStatus::ready,"Real personality duplicate rejected");
        if(selected.design.role==airack::RackRole::synth)copied.values[12+4*airack::rackUnitBinding(selected,selected.design.units.front().id)->slot]=0;
        else for(const auto& binding:selected.bindings){const auto i=airack::rackParameterIndex(selected.design.units[binding.unit].module,binding.slot,"bypassed");require(i.has_value(),"Original duplicate bypass mapping missing");copied.values[*i]=1;}
        // Thread's independent segment moves to shared Master on the clone; original insert is bypassed.
        require(airack::validateSoundDocument(copied),"Real duplicate state invalid");airack::ProjectDocument project;project.sound=copied;auto clone=renderPersonality(format,*list[0],project,512);airack::ProjectDocument source;source.sound=original;auto expected=renderPersonality(format,*list[0],source,512);const auto diff=difference(clone.render.audio,expected.render.audio);require(diff<=1e-6,"Real personality duplicate changed equivalent audio");const auto stem=designs[n].name.replaceCharacter(' ','-')+"-rack-duplicate";replayLegacy(stem,project,clone.render.audio,true);juce::MemoryBlock encoded;require(airack::encodeProjectState(project,encoded),"Real duplicate project encoding failed");writeBlock(args.output.getChildFile(stem+".airackproject"),encoded);writeBlock(args.output.getChildFile(stem+".hoststate"),clone.render.hostState);writeWav(args.output.getChildFile(stem+".wav"),clone.render.audio,48000);auto item=jsonObject();property(item,"name",stem);property(item,"difference",diff);property(item,"state",identity(args.output.getChildFile(stem+".airackproject")));fixtures.append(item);
    }

    // Complete current legacy recall and metadata-only swap/reorder on fresh loaded instances.
    airack::ProjectDocument legacy;legacy.sound.values=fixtureValues(false);auto legacyRender=renderPersonality(format,*list[0],legacy,512,nullptr,true);writeWav(args.output.getChildFile("legacy-generic.wav"),legacyRender.render.audio,48000);
    auto swapped=personalityFixture(0,0);auto state=std::make_shared<airack::RackPersonalityState>(*swapped.personalities);state->instances.back().design.name="Ember Reed Edited presentation";state->instances.back().design.revision++;state->instances.back().edited=true;swapped.personalities=state;airack::ProjectDocument swapProject;swapProject.sound=swapped;auto swap=renderPersonality(format,*list[0],swapProject,512);airack::ProjectDocument base;base.sound=personalityFixture(0,0);auto baseAudio=renderPersonality(format,*list[0],base,512);require(difference(swap.render.audio,baseAudio.render.audio)<=1e-6,"Metadata-only swap changed audio");writeBlock(args.output.getChildFile("metadata-swap.hoststate"),swap.render.hostState);
    airack::RackOperation move;move.kind=airack::RackOperationKind::move;move.selected=swapped.personalities->instances.back().id;move.destination={airack::RackRole::synth,airack::masterChain,0};airack::SoundDocument moved;airack::RackDifference delta;require(airack::makeRackCandidate(swapped,move,airack::SoundEditOrigin::manual,moved,delta)==airack::RackCandidateStatus::ready,"Reorder fixture rejected");swapProject.sound=moved;auto reordered=renderPersonality(format,*list[0],swapProject,512);require(difference(reordered.render.audio,baseAudio.render.audio)<=1e-6,"Silent-bank reorder changed audio");writeBlock(args.output.getChildFile("reordered.hoststate"),reordered.render.hostState);
    if(replayRequested){require(legacyComparisons.size()==15&&hash(args.baseline)==manifestHash,"Incomplete/changed legacy baseline");for(const auto& file:*legacyFiles.getArray())require(hash(juce::File(file["path"].toString()))==file["sha256"].toString(),"Legacy input changed during replay");property(report,"legacy_replay",legacyComparisons);property(report,"legacy_inputs",legacyFiles);}
    property(report,"fixtures",fixtures);property(report,"module_after",identity(binaryIn(args.plugin)));require(hash(binaryIn(args.plugin))==report["module_before"]["sha256"].toString(),"Module changed during render");if(helper.existsAsFile()){property(report,"helper_after",identity(helper));require(hash(helper)==report["helper_before"]["sha256"].toString(),"Helper changed during render");}property(report,"success",true);writePatternJson(args.output.getChildFile("report.json"),report);std::cout<<"SUCCESS rack-personalities actual VST3 complete metadata/759IDs/audio/editor/automation/offline\n";return 0;
}

struct QualityBrief
{
    juce::String id,role,stimulus;
    airack::SoundDocument dry,processed;
    std::vector<int> notes;
    double hold=2,duration=6;
    bool repeated=false,sweep=false,singleSampleNoise=false;
};
void qualityEffect(airack::SoundDocument& d,airack::EffectKind kind)
{
    const auto slot=d.layout.effectCount[airack::masterChain];
    airack::RackEdit edit;edit.kind=airack::RackEditKind::addEffect;edit.slot=slot;edit.destination=airack::masterChain;edit.position=slot;edit.effect=kind;
    require(airack::editRackLayout(d.layout,edit,d.layout)==airack::RackEditResult::applied,"Quality effect placement failed");
}
std::vector<QualityBrief> qualityBriefs()
{
    std::vector<QualityBrief> result;
    const std::array<const char*,5> analog{"rounded-bass","articulate-pluck","resonant-lead","brass-chord","evolving-pad"};
    const std::array<const char*,5> digital{"bright-bass","clean-pluck","dense-lead","moving-chord","airy-pad"};
    for(bool wave:{false,true})for(int n=0;n<5;++n){
        QualityBrief b;b.role=wave?"digital":"analog";b.id=b.role+"-"+(wave?digital:analog)[std::size_t(n)];b.stimulus="MIDI notes, velocity100, channel1";
        b.dry.values=fixtureValues(wave);auto& d=b.dry;const juce::String bank=wave?"synth.1.wavetable.":"synth.1.analog.";
        setGainValue(d,"global.master_level",1);setGainValue(d,"synth.1.level",.7f);setGainValue(d,"output.gain_db",24);setGainValue(d,"output.protection",1);
        setGainValue(d,bank+"amp.attack",n==4?.6f:n==3?.04f:.002f);setGainValue(d,bank+"amp.decay",n==1?.2f:.15f);setGainValue(d,bank+"amp.sustain",n==1?0.f:.7f);setGainValue(d,bank+"amp.release",n==4?1.5f:.15f);
        setGainValue(d,bank+"filter.cutoff",n==0?(wave?4800.f:700.f):n==1?3200.f:n==2?6500.f:n==3?2200.f:4200.f);
        setGainValue(d,bank+"filter.resonance",n==2?.7f:.2f);setGainValue(d,bank+"filter_env.octaves",n==1?2.f:n==3?1.f:0.f);
        if(!wave&&n==0){setGainValue(d,bank+"osc.1.waveform",0);setGainValue(d,bank+"osc.2.level",.15f);setGainValue(d,bank+"osc.2.tune",-12);}
        if(wave)for(int o:{1,2})setGainValue(d,bank+"osc."+juce::String(o)+".position",n==0?.95f:n==1?.25f:n==2?1.f:.6f);
        if(n>=3){setGainValue(d,"synth.1.lfo.1.rate_hz",.25f);setGainValue(d,"synth.1.modulation.1.enabled",1);setGainValue(d,"synth.1.modulation.1.source",float(airack::ModSource::lfo1));setGainValue(d,"synth.1.modulation.1.target",float(wave?airack::ModTarget::osc1Position:airack::ModTarget::cutoff));setGainValue(d,"synth.1.modulation.1.bank",float(wave?airack::ModBank::wavetable:airack::ModBank::analog));setGainValue(d,"synth.1.modulation.1.amount",.15f);}
        b.notes=n==0?std::vector<int>{36}:n==3||n==4?std::vector<int>{48,51,55,58}:std::vector<int>{n==2?72:60};b.repeated=n==1;b.hold=n==1?.2:n==4?3:2;b.duration=n==4?8:6;
        b.processed=d;qualityEffect(b.processed,airack::EffectKind::drive);qualityEffect(b.processed,airack::EffectKind::reverb);
        setGainValue(b.processed,"effect.1.drive.gain_db",3);setGainValue(b.processed,"effect.1.drive.mix",.15f);setGainValue(b.processed,"effect.2.reverb.decay_seconds",1.2f);setGainValue(b.processed,"effect.2.reverb.mix",.12f);
        result.push_back(std::move(b));
    }
    for(bool plate:{false,true})for(bool chord:{false,true})for(bool longTail:{false,true}){
        auto b=result[chord?3:1];b.role="reverb";b.id="reverb-"+juce::String(plate?"plate-intent":"hall-intent")+(chord?"-chord":"-pluck")+(longTail?"-long":"-short");b.duration=longTail?14:6;
        if(chord){airack::RackEdit layer;layer.kind=airack::RackEditKind::addSynth;layer.position=1;require(airack::editRackLayout(b.dry.layout,layer,b.dry.layout)==airack::RackEditResult::applied,"Stereo chord layer failed");setGainValue(b.dry,"synth.1.pan",-.45f);setGainValue(b.dry,"synth.2.enabled",1);setGainValue(b.dry,"synth.2.engine",1);setGainValue(b.dry,"synth.2.pan",.65f);setGainValue(b.dry,"synth.2.level",.35f);setGainValue(b.dry,"synth.2.wavetable.osc.1.position",.8f);setGainValue(b.dry,"synth.2.wavetable.osc.2.tune",7);setGainValue(b.dry,"synth.2.wavetable.amp.attack",.04f);setGainValue(b.dry,"synth.2.wavetable.amp.sustain",.7f);b.stimulus+="; asymmetric analog/digital stereo chord input";}
        b.processed=b.dry;qualityEffect(b.processed,airack::EffectKind::reverb);setGainValue(b.processed,"effect.1.reverb.size",plate?.35f:.9f);setGainValue(b.processed,"effect.1.reverb.decay_seconds",longTail?8.f:.8f);setGainValue(b.processed,"effect.1.reverb.damping",plate?.6f:.3f);setGainValue(b.processed,"effect.1.reverb.predelay_ms",plate?5.f:20.f);setGainValue(b.processed,"effect.1.reverb.mix",.25f);
        b.stimulus+="; hall/plate are comparison intentions; current generic four-delay kernel has no hall/plate algorithm selector";result.push_back(std::move(b));
    }
    for(int n=0;n<5;++n){auto b=result[std::size_t(n==0?1:n==1?0:4)];b.role="compressor";b.id="compressor-"+juce::String(std::array<const char*,5>{"transient","bass","pad","layered-bus","parallel"}[std::size_t(n)]);
        if(n==3){for(std::uint8_t slot=1;slot<3;++slot){airack::RackEdit edit;edit.kind=airack::RackEditKind::addSynth;edit.position=slot;require(airack::editRackLayout(b.dry.layout,edit,b.dry.layout)==airack::RackEditResult::applied,"Bus layer placement failed");setGainValue(b.dry,"synth."+juce::String(slot+1)+".enabled",1);setGainValue(b.dry,"synth."+juce::String(slot+1)+".engine",slot==1?1.f:0.f);setGainValue(b.dry,"synth."+juce::String(slot+1)+".level",.25f);}}
        b.processed=b.dry;qualityEffect(b.processed,airack::EffectKind::compressor);setGainValue(b.processed,"effect.1.compressor.threshold_db",-42);setGainValue(b.processed,"effect.1.compressor.ratio",3);setGainValue(b.processed,"effect.1.compressor.attack_ms",n==0?20.f:10.f);setGainValue(b.processed,"effect.1.compressor.release_ms",150);setGainValue(b.processed,"effect.1.compressor.mix",n==4?.5f:1.f);result.push_back(std::move(b));
    }
    for(bool wave:{false,true}){auto b=result[wave?7:2];b.role="stress";b.id=wave?"digital-pitch-modulation-resonance-drive":"analog-pitch-modulation-resonance-drive";b.notes={24,36,48,60,72,84,96,108};b.sweep=true;b.hold=.15;b.duration=6;
        const juce::String bank=wave?"synth.1.wavetable.":"synth.1.analog.";setGainValue(b.dry,bank+"filter.cutoff",20000);setGainValue(b.dry,bank+"filter.resonance",.95f);setGainValue(b.dry,"synth.1.lfo.1.rate_hz",2);setGainValue(b.dry,"synth.1.modulation.1.enabled",1);setGainValue(b.dry,"synth.1.modulation.1.source",float(airack::ModSource::lfo1));setGainValue(b.dry,"synth.1.modulation.1.target",float(airack::ModTarget::osc1Tune));setGainValue(b.dry,"synth.1.modulation.1.bank",float(wave?airack::ModBank::wavetable:airack::ModBank::analog));setGainValue(b.dry,"synth.1.modulation.1.amount",.5f);
        b.processed=b.dry;qualityEffect(b.processed,airack::EffectKind::drive);setGainValue(b.processed,"effect.1.drive.gain_db",24);setGainValue(b.processed,"effect.1.drive.mix",1);result.push_back(std::move(b));}
    require(result.size()==25,"Quality brief matrix incomplete");for(const auto& b:result)require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid quality brief");return result;
}
std::vector<QualityBrief> reverbAnalysisBriefs(airack::AlgorithmRevision revision=1)
{
    std::vector<QualityBrief> result;
    for(auto brief:qualityBriefs())if(brief.role=="reverb"){
        // Separate linear-analysis fixtures; original protected musical auditions
        // retain their authored gain/protection settings in qualityBriefs().
        for(auto* document:{&brief.dry,&brief.processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        brief.id="analysis-"+brief.id;brief.stimulus+="; output gain0dB/protectionOFF; raw unchanged, no analysis normalization";result.push_back(std::move(brief));
    }
    QualityBrief probe;probe.role="reverb";probe.id="analysis-reverb-filtered-noise-note";probe.notes={60};probe.singleSampleNoise=true;probe.duration=14;
    probe.stimulus="MIDI60 velocity100/channel1, noteOn hostframe0/noteOff hostframe1; instrument noise through actual synth envelope/filter, NOT an effect-input delta; output gain0dB/protectionOFF";
    probe.dry.values=fixtureValues(false);setGainValue(probe.dry,"global.master_level",1);setGainValue(probe.dry,"synth.1.level",.7f);setGainValue(probe.dry,"output.gain_db",0);setGainValue(probe.dry,"output.protection",0);
    const juce::String bank="synth.1.analog.";setGainValue(probe.dry,bank+"osc.1.level",0);setGainValue(probe.dry,bank+"osc.2.level",0);setGainValue(probe.dry,bank+"noise",1);
    setGainValue(probe.dry,bank+"amp.attack",0);setGainValue(probe.dry,bank+"amp.decay",0);setGainValue(probe.dry,bank+"amp.sustain",1);setGainValue(probe.dry,bank+"amp.release",0);
    setGainValue(probe.dry,bank+"filter.cutoff",20000);setGainValue(probe.dry,bank+"filter.resonance",0);setGainValue(probe.dry,bank+"filter_env.octaves",0);setGainValue(probe.dry,"synth.1.modulation.1.enabled",0);
    probe.processed=probe.dry;qualityEffect(probe.processed,airack::EffectKind::reverb);setGainValue(probe.processed,"effect.1.reverb.size",.3f);setGainValue(probe.processed,"effect.1.reverb.decay_seconds",4);setGainValue(probe.processed,"effect.1.reverb.damping",.2f);setGainValue(probe.processed,"effect.1.reverb.predelay_ms",0);setGainValue(probe.processed,"effect.1.reverb.mix",1);
    result.push_back(probe);
    for(const auto& control:std::array<std::pair<const char*,float>,4>{{{"size",.9f},{"decay_seconds",1.f},{"damping",.9f},{"predelay_ms",50.f}}}){
        auto changed=probe;changed.id+="-"+juce::String(control.first);setGainValue(changed.processed,"effect.1.reverb."+juce::String(control.first),control.second);result.push_back(std::move(changed));}
    require(result.size()==13,"Bounded reverb analysis matrix incomplete");
    for(auto& brief:result) {
        if(revision==2)brief.stimulus=brief.stimulus.replace("current generic four-delay kernel has no hall/plate algorithm selector","processed experimental Reverb2 FDN; hall/plate remain comparison intentions, not distinct algorithm modes");
        for(std::size_t slot=0;slot<8;++slot)if(brief.processed.layout.effects[slot].use==airack::SlotUse::present&&brief.processed.layout.effects[slot].kind==airack::EffectKind::reverb)
            brief.processed.compatibility.effects[slot][2]=revision;
        require(airack::validateSoundDocument(brief.dry)&&airack::validateSoundDocument(brief.processed),"Invalid reverb analysis fixture");
    }
    return result;
}
airack::RackDesign qualityFactory(bool digital=false)
{
    const auto module=digital?airack::NativeModule::wavetable:airack::NativeModule::analog;
    const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==juce::Uuid(digital?"91387715-13d3-4c57-a479-12251842b402":"91387715-13d3-4c57-a479-12251842a402");});
    require(found!=designs.end()&&found->revision==1&&found->units.size()==1&&found->units[0].module==module&&found->units[0].algorithmRevision==2,"Expected revised factory on algorithm revision 2");return *found;
}
airack::SoundDocument factorySource(bool digital=false)
{
    airack::SoundDocument original,result;airack::RackOperation op;airack::RackDifference delta;
    op.design=qualityFactory(digital);op.newInstance=juce::Uuid(digital?"91387715-13d3-4c57-a479-12251842b411":"91387715-13d3-4c57-a479-12251842a411");op.destination.position=0;
    // Explicit manual reuse replaces the unowned initial generic bank; four
    // independent factory copies can then occupy the four existing MIDI layers.
    op.allocation={{0,0}};op.allowSlotReuse=true;
    require(airack::makeRackCandidate(original,op,airack::SoundEditOrigin::manual,result,delta)==airack::RackCandidateStatus::ready,"Actual factory placement failed");
    const auto* binding=airack::rackUnitBinding(result.personalities->instances[0],0);require(binding&&binding->slot==0,"Factory semantic unit binding mismatch");
    setGainValue(result,"global.master_level",1);setGainValue(result,"output.gain_db",24);setGainValue(result,"output.protection",1);return result;
}
std::vector<QualityBrief> factoryBriefs(bool digital=false)
{
    const auto original=qualityBriefs();std::vector<QualityBrief> result;
    const auto module=digital?airack::NativeModule::wavetable:airack::NativeModule::analog;const juce::String bank=digital?"wavetable.":"analog.",role=digital?"factory-digital":"factory-analog",name=digital?"Prism Relay":"Copper Arc";
    for(int n=0;n<5;++n){const auto index=std::size_t(n+(digital?5:0));auto b=original[index];b.id="factory-"+b.id;b.role=role;b.dry=factorySource(digital);
        const auto copy=[&](const juce::String& key){const auto parameter=airack::rackParameterIndex(module,0,key);require(parameter.has_value(),"Factory variation key unavailable");b.dry.values[*parameter]=original[index].dry.values[*parameter];};
        for(const auto* key:{"amp.attack","amp.decay","amp.sustain","amp.release","filter.cutoff","filter.resonance","filter_env.octaves"})copy(bank+key);
        if(digital)for(const auto* key:{"osc.1.position","osc.2.position"})copy(bank+key);
        else if(n==0)for(const auto* key:{"osc.1.waveform","osc.2.level","osc.2.tune"})copy(bank+key);
        setGainValue(b.dry,"synth.1.modulation.1.amount",n>=3?.15f:0.f);
        b.processed=b.dry;qualityEffect(b.processed,airack::EffectKind::drive);qualityEffect(b.processed,airack::EffectKind::reverb);
        setGainValue(b.processed,"effect.1.drive.gain_db",3);setGainValue(b.processed,"effect.1.drive.mix",.15f);setGainValue(b.processed,"effect.2.reverb.decay_seconds",1.2f);setGainValue(b.processed,"effect.2.reverb.mix",.12f);
        b.stimulus+="; actual placed "+name+" factory, ordinary same-ID/revision edits; external restrained FX only";result.push_back(std::move(b));
    }
    QualityBrief nominal;nominal.id=role+"-starting-default";nominal.role=role;nominal.dry=factorySource(digital);nominal.processed=nominal.dry;nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched placed "+name+" unit defaults; external restrained FX only";
    qualityEffect(nominal.processed,airack::EffectKind::drive);qualityEffect(nominal.processed,airack::EffectKind::reverb);
    setGainValue(nominal.processed,"effect.1.drive.gain_db",3);setGainValue(nominal.processed,"effect.1.drive.mix",.15f);setGainValue(nominal.processed,"effect.2.reverb.decay_seconds",1.2f);setGainValue(nominal.processed,"effect.2.reverb.mix",.12f);result.push_back(std::move(nominal));
    auto load=result[4];load.id=role+"-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid(juce::String(digital?"91387715-13d3-4c57-a479-12251842b41":"91387715-13d3-4c57-a479-12251842a41")+juce::String(n+1));op.destination.position=std::uint8_t(n);airack::SoundDocument next;airack::RackDifference delta;
        require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Factory four-layer duplicate rejected");load.dry=std::move(next);}
    load.processed=load.dry;
    for(int n=0;n<8;++n){const auto kind=static_cast<airack::EffectKind>(n%4);qualityEffect(load.processed,kind);const auto prefix="effect."+juce::String(n+1)+".";
        setGainValue(load.processed,prefix+"drive.gain_db",3);setGainValue(load.processed,prefix+"drive.mix",.15f);setGainValue(load.processed,prefix+"delay.seconds",.25f);setGainValue(load.processed,prefix+"delay.feedback",.35f);setGainValue(load.processed,prefix+"delay.mix",.15f);setGainValue(load.processed,prefix+"reverb.decay_seconds",8);setGainValue(load.processed,prefix+"reverb.mix",.12f);setGainValue(load.processed,prefix+"compressor.threshold_db",-30);setGainValue(load.processed,prefix+"compressor.ratio",2);setGainValue(load.processed,prefix+"compressor.mix",.5f);}
    load.stimulus="16 simultaneous MIDI48..63 velocity100/channel1, release2s; four actual independent factory copies and eight native master FX, uninterrupted22s";result.push_back(std::move(load));
    for(const auto& b:result)require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid factory quality brief");return result;
}
std::vector<QualityBrief> coreFxProbeBriefs()
{
    auto source=factorySource(false);setGainValue(source,"output.gain_db",0);setGainValue(source,"output.protection",0);
    std::vector<QualityBrief> result;
    for(int threshold=-60;threshold<=0;threshold+=3){
        QualityBrief b;b.id="core-fx-Copper-Arc-threshold-"+juce::String(-threshold);b.role="core-fx-probe";b.dry=source;b.processed=source;b.notes={60};b.hold=4;b.duration=6;
        qualityEffect(b.processed,airack::EffectKind::compressor);setGainValue(b.processed,"effect.1.compressor.threshold_db",float(threshold));
        setGainValue(b.processed,"effect.1.compressor.ratio",3);setGainValue(b.processed,"effect.1.compressor.attack_ms",15);setGainValue(b.processed,"effect.1.compressor.release_ms",150);setGainValue(b.processed,"effect.1.compressor.makeup_db",0);setGainValue(b.processed,"effect.1.compressor.mix",1);
        b.stimulus="Actual untouched Copper Arc/native1, MIDI60 velocity100/channel1, hold4s/capture6s; output0dB/master1/protectionOFF; native1 linked compressor ratio3/15ms/150ms/makeup0/mix1, threshold"+juce::String(threshold)+"dB; fixed settled2-4s window exceeds10x release";
        require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual Copper Arc compressor probe");result.push_back(std::move(b));
    }
    return result;
}

airack::RackDesign reverbFactory()
{
    const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842c401");});
    require(found!=designs.end()&&found->revision==1&&found->role==airack::RackRole::fx&&found->units.size()==1&&found->units[0].module==airack::NativeModule::reverb&&found->units[0].algorithmRevision==2,"Actual Reverb2 factory unavailable");return *found;
}
airack::SoundDocument placeReverbFactory(const airack::SoundDocument& source)
{
    airack::RackOperation op;op.design=reverbFactory();op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c411");op.destination.role=airack::RackRole::fx;op.destination.chain=airack::masterChain;op.destination.position=0;
    airack::SoundDocument result;airack::RackDifference delta;require(airack::makeRackCandidate(source,op,airack::SoundEditOrigin::manual,result,delta)==airack::RackCandidateStatus::ready,"Actual Reverb2 factory placement failed");return result;
}
std::vector<QualityBrief> factoryReverbBriefs()
{
    const auto originals=qualityBriefs();std::vector<QualityBrief> result;
    const auto chord=std::find_if(originals.begin(),originals.end(),[](const auto& b){return b.id=="reverb-hall-intent-chord-long";});require(chord!=originals.end(),"Historical stereo source unavailable");
    auto nominal=*chord;nominal.id="factory-reverb-starting-default";nominal.notes={60,64,67};nominal.hold=2;nominal.duration=14;nominal.repeated=false;nominal.processed=placeReverbFactory(nominal.dry);
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; historical asymmetric two-layer stereo source through untouched Lumen Drift defaults;14s active-tail observation, no additional FX";result.push_back(std::move(nominal));
    for(const auto& original:originals)if(original.role=="reverb"){
        auto b=original;b.id="factory-"+b.id;b.processed=placeReverbFactory(b.dry);const auto& instance=b.processed.personalities->instances[0];const auto* binding=airack::rackUnitBinding(instance,0);require(binding,"Factory reverb binding missing");
        for(const auto* key:{"reverb.size","reverb.decay_seconds","reverb.damping","reverb.predelay_ms","reverb.mix"}){const auto from=airack::rackParameterIndex(airack::NativeModule::reverb,0,key),to=airack::rackParameterIndex(airack::NativeModule::reverb,binding->slot,key);require(from&&to,"Factory role control unavailable");b.processed.values[*to]=original.processed.values[*from];}
        b.stimulus=b.stimulus.replace("current generic four-delay kernel has no hall/plate algorithm selector","actual placed Lumen Drift Reverb2; hall/plate are ordinary control voicings, not distinct algorithm modes");result.push_back(std::move(b));
    }
    auto load=result[0];load.id="factory-reverb-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    while(load.dry.layout.synthCount<4){airack::RackEdit edit;edit.kind=airack::RackEditKind::addSynth;edit.position=load.dry.layout.synthCount;require(airack::editRackLayout(load.dry.layout,edit,load.dry.layout)==airack::RackEditResult::applied,"Factory reverb source layer failed");}
    for(int n=0;n<4;++n){const auto prefix="synth."+juce::String(n+1)+".";setGainValue(load.dry,prefix+"enabled",1);setGainValue(load.dry,prefix+"engine",float(n%2));setGainValue(load.dry,prefix+"level",.35f);setGainValue(load.dry,prefix+"analog.amp.sustain",.7f);setGainValue(load.dry,prefix+"wavetable.amp.sustain",.7f);}
    load.processed=placeReverbFactory(load.dry);const auto selected=load.processed.personalities->instances[0].id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c41"+juce::String(n+1));op.destination.role=airack::RackRole::fx;op.destination.chain=airack::masterChain;op.destination.position=std::uint8_t(n);airack::SoundDocument next;airack::RackDifference delta;
        require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight independent factory reverb placements failed");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four native layers and eight independent untouched Lumen Drift instances; uninterrupted22s active tail, not quiescence";result.push_back(std::move(load));
    require(result.size()==10,"Nominal/eight-role/full-load reverb factory packet incomplete");for(const auto& b:result)require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual factory reverb state");return result;
}
juce::var factoryEvidence(const airack::SoundDocument& document,const airack::RackDesign& design)
{
    juce::var definition;require(airack::encodeRackDesignObject(design,definition,airack::RackValidationMode::portableDesign),"Factory definition encoding failed");juce::MemoryBlock bytes;require(airack::encodeRackDesign(design,bytes),"Factory content identity failed");
    auto result=jsonObject();property(result,"definition",definition);property(result,"definition_sha256",juce::SHA256(bytes).toHexString());juce::Array<juce::var> instances;
    require(document.personalities!=nullptr||design.role==airack::RackRole::fx,"Actual factory snapshot missing");
    if(document.personalities)for(const auto& instance:document.personalities->instances){require(instance.design==design,"Variation silently changed factory identity/content");auto item=jsonObject();property(item,"instance_id",instance.id.toString());juce::Array<juce::var> bindings,tweaks;
        for(const auto& binding:instance.bindings){auto b=jsonObject();property(b,"unit",int(binding.unit));property(b,"slot",int(binding.slot));bindings.add(b);const auto& unit=design.units[0];require(binding.unit==unit.id,"Unexpected factory unit");
            for(const auto& value:unit.defaults){const auto index=airack::rackParameterIndex(unit.module,binding.slot,value.key);require(index.has_value(),"Factory semantic parameter unavailable");if(document.values[*index]!=value.normalized){auto t=jsonObject();property(t,"key",value.key);property(t,"normalized",document.values[*index]);property(t,"physical",airack::parameterPhysicalValue(document.values,*index));tweaks.add(t);}}}
        property(item,"bindings",bindings);property(item,"ordinary_tweaks",tweaks);instances.add(item);}
    property(result,"instances",instances);return result;
}
juce::var factoryEvidence(const airack::SoundDocument& document,bool digital=false){return factoryEvidence(document,qualityFactory(digital));}
airack::RackDesign coreFxFactory(airack::NativeModule module)
{
    const char* id=module==airack::NativeModule::midSideSplit?"91387715-13d3-4c57-a479-12251842be01":module==airack::NativeModule::frequencySplit?"91387715-13d3-4c57-a479-12251842bd02":module==airack::NativeModule::convolution?"91387715-13d3-4c57-a479-12251842bc01":module==airack::NativeModule::frequencyShift?"91387715-13d3-4c57-a479-12251842bb01":module==airack::NativeModule::dimension?"91387715-13d3-4c57-a479-12251842aa02":module==airack::NativeModule::stereo?"91387715-13d3-4c57-a479-12251842f901":module==airack::NativeModule::flanger?"91387715-13d3-4c57-a479-12251842e902":module==airack::NativeModule::phaser?"91387715-13d3-4c57-a479-12251842d901":module==airack::NativeModule::chorus?"91387715-13d3-4c57-a479-12251842c802":module==airack::NativeModule::filter?"91387715-13d3-4c57-a479-12251842b802":module==airack::NativeModule::eq?"91387715-13d3-4c57-a479-12251842a802":module==airack::NativeModule::compressor?"91387715-13d3-4c57-a479-12251842d402":module==airack::NativeModule::drive?"91387715-13d3-4c57-a479-12251842e402":"91387715-13d3-4c57-a479-12251842f402";
    require(module==airack::NativeModule::compressor||module==airack::NativeModule::drive||module==airack::NativeModule::delay||module==airack::NativeModule::eq||module==airack::NativeModule::filter||module==airack::NativeModule::chorus||module==airack::NativeModule::phaser||module==airack::NativeModule::flanger||module==airack::NativeModule::stereo||module==airack::NativeModule::dimension||module==airack::NativeModule::frequencyShift||module==airack::NativeModule::convolution||module==airack::NativeModule::frequencySplit||module==airack::NativeModule::midSideSplit,"Unknown core-FX factory module");
    const auto designs=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==juce::Uuid(id);});
    require(found!=designs.end()&&found->revision==1&&found->role==airack::RackRole::fx&&found->units.size()==1&&found->units[0].module==module&&found->units[0].algorithmRevision==(module==airack::NativeModule::drive?airack::modernDriveAlgorithmRevision:module==airack::NativeModule::delay?airack::modernDelayAlgorithmRevision:module==airack::NativeModule::compressor?airack::modernCompressorAlgorithmRevision:module==airack::NativeModule::eq?airack::modernEqAlgorithmRevision:module==airack::NativeModule::filter?airack::modernFilterAlgorithmRevision:module==airack::NativeModule::frequencySplit?airack::modernFrequencySplitAlgorithmRevision:module==airack::NativeModule::chorus||module==airack::NativeModule::flanger||module==airack::NativeModule::dimension?airack::modernModulatedTapAlgorithmRevision:airack::legacyAlgorithmRevision),"Actual core-FX factory unavailable");return *found;
}
airack::SoundDocument placeCoreFxFactory(const airack::SoundDocument& source,airack::NativeModule module)
{
    airack::RackOperation op;op.design=coreFxFactory(module);const char* id=module==airack::NativeModule::midSideSplit?"91387715-13d3-4c57-a479-12251842be11":module==airack::NativeModule::frequencySplit?"91387715-13d3-4c57-a479-12251842bd11":module==airack::NativeModule::convolution?"91387715-13d3-4c57-a479-12251842bc11":module==airack::NativeModule::frequencyShift?"91387715-13d3-4c57-a479-12251842bb11":module==airack::NativeModule::dimension?"91387715-13d3-4c57-a479-12251842aa11":module==airack::NativeModule::stereo?"91387715-13d3-4c57-a479-12251842f911":module==airack::NativeModule::flanger?"91387715-13d3-4c57-a479-12251842e911":module==airack::NativeModule::phaser?"91387715-13d3-4c57-a479-12251842d911":module==airack::NativeModule::chorus?"91387715-13d3-4c57-a479-12251842c811":module==airack::NativeModule::filter?"91387715-13d3-4c57-a479-12251842b811":module==airack::NativeModule::eq?"91387715-13d3-4c57-a479-12251842a811":module==airack::NativeModule::compressor?"91387715-13d3-4c57-a479-12251842d411":module==airack::NativeModule::drive?"91387715-13d3-4c57-a479-12251842e411":"91387715-13d3-4c57-a479-12251842f411";
    op.newInstance=juce::Uuid(id);op.destination={airack::RackRole::fx,airack::masterChain,source.layout.effectCount[airack::masterChain]};
    airack::SoundDocument result;airack::RackDifference delta;require(airack::makeRackCandidate(source,op,airack::SoundEditOrigin::manual,result,delta)==airack::RackCandidateStatus::ready,"Actual core-FX placement rejected");return result;
}
juce::var coreFxEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,4> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::compressor),coreFxFactory(airack::NativeModule::drive),coreFxFactory(airack::NativeModule::delay)};
    require(document.personalities!=nullptr,"Core-FX carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"Core-FX packet silently changed an embedded definition");
    juce::Array<juce::var> inserts;
    for(std::size_t n=0;n<designs.size();++n){auto filtered=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});filtered.personalities=state;
        const auto evidence=factoryEvidence(filtered,designs[n]);if(n==0)property(result,"carrier",evidence);else inserts.add(evidence);}
    property(result,"inserts",inserts);return result;
}
std::vector<QualityBrief> factoryCoreFxBriefs()
{
    // Keep these large fixture copies off the renderer stack while nested builders run.
    std::vector<QualityBrief> result;const auto carriers=factoryBriefs(false);
    for(auto module:{airack::NativeModule::compressor,airack::NativeModule::drive,airack::NativeModule::delay}){
        auto owned=std::make_unique<QualityBrief>();auto& b=*owned;b.id=module==airack::NativeModule::compressor?"factory-compressor-starting-default":module==airack::NativeModule::drive?"factory-drive-starting-default":"factory-delay-starting-default";
        b.role="factory-core-fx";b.dry=factorySource(false);b.processed=placeCoreFxFactory(b.dry,module);b.notes={60};b.hold=4;
        b.stimulus="MIDI60 velocity100/channel1, hold4s; untouched Copper Arc carrier and untouched actual "+coreFxFactory(module).name+(module==airack::NativeModule::drive?" Drive2":module==airack::NativeModule::compressor?" Compressor2":" Delay2")+" insert; master1/postoutput24dB/protectionON; no other FX";result.push_back(std::move(b));
    }
    for(int n=0;n<5;++n){auto owned=std::make_unique<QualityBrief>(carriers[std::size_t(n==0?1:n==1?0:4)]);auto& b=*owned;b.id="factory-compressor-"+juce::String(std::array<const char*,5>{"transient","bass","pad","layered-bus","parallel"}[std::size_t(n)]);b.role="factory-core-fx";
        if(n==3){const auto selected=b.dry.personalities->instances[0].id;for(int layer=1;layer<3;++layer){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(layer+1));op.destination.position=std::uint8_t(layer);airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(b.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Core-FX bus carrier duplicate rejected");b.dry=std::move(next);}}
        b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::compressor);
        setGainValue(b.processed,"effect.1.compressor.threshold_db",n==3?-48.f:-54.f);setGainValue(b.processed,"effect.1.compressor.attack_ms",n==0?20.f:n==1?8.f:15.f);setGainValue(b.processed,"effect.1.compressor.ratio",n==3?2.f:3.f);setGainValue(b.processed,"effect.1.compressor.mix",n==4?.5f:1.f);
        b.stimulus+="; actual Anchor Press Compressor2, ordinary same-definition threshold/timing/ratio/blend edits; numerical calibration is source-specific, not promised GR for this role";result.push_back(std::move(b));
    }
    for(bool lead:{false,true}){auto owned=std::make_unique<QualityBrief>(carriers[lead?2:0]);auto& b=*owned;b.id=lead?"factory-drive-lead":"factory-drive-bass";b.role="factory-core-fx";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::drive);
        setGainValue(b.processed,"effect.1.drive.gain_db",lead?15.f:9.f);setGainValue(b.processed,"effect.1.drive.tone_hz",lead?9000.f:3500.f);setGainValue(b.processed,"effect.1.drive.mix",lead?.5f:.35f);b.stimulus+="; actual Cinder Coil Drive2 ordinary drive/tone/blend edits";result.push_back(std::move(b));}
    for(int n=0;n<3;++n){auto owned=std::make_unique<QualityBrief>(carriers[n==0?1:n==1?2:4]);auto& b=*owned;b.id="factory-delay-"+juce::String(std::array<const char*,3>{"synced-eighth","short-stereo","filtered-long"}[std::size_t(n)]);b.role="factory-core-fx";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::delay);
        setGainValue(b.processed,"effect.1.delay.synced",n==0?1.f:0.f);setGainValue(b.processed,"effect.1.delay.seconds",n==1?.07f:.45f);setGainValue(b.processed,"effect.1.delay.beats",.5f);setGainValue(b.processed,"effect.1.delay.feedback",n==0?.3f:n==1?.1f:.4f);setGainValue(b.processed,"effect.1.delay.stereo",n==1?.6f:.35f);setGainValue(b.processed,"effect.1.delay.filter_hz",n==2?3000.f:4500.f);setGainValue(b.processed,"effect.1.delay.mix",n==1?.12f:.22f);b.stimulus+="; actual Echo Thread Delay2 ordinary time/feedback/stereo/filter/blend edits; loaded host tempo120BPM";result.push_back(std::move(b));}
    auto ownedLoad=std::make_unique<QualityBrief>(result[0]);auto& load=*ownedLoad;load.id="factory-core-fx-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four core-FX carrier layers rejected");load.dry=std::move(next);}
    load.processed=load.dry;for(auto module:{airack::NativeModule::compressor,airack::NativeModule::drive,airack::NativeModule::delay})load.processed=placeCoreFxFactory(load.processed,module);
    for(int n=3;n<8;++n){const auto original=load.processed.personalities->instances[std::size_t(4+n%3)];airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=original.id;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-1225184217"+juce::String(n+10));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight core-FX factory duplicates rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight actual untouched Anchor Press Compressor2/Cinder Coil Drive2/Echo Thread Delay2 inserts; uninterrupted22s callback/tail observation, not quiescence";result.push_back(std::move(load));
    require(result.size()==14,"Core-FX nominal/role/load packet incomplete");for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual core-FX factory fixture");coreFxEvidence(b.dry);coreFxEvidence(b.processed);}return result;
}
juce::var eqFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::eq)};
    require(document.personalities!=nullptr,"EQ carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"EQ packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto filtered=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});filtered.personalities=state;
        property(result,n==0?"carrier":"eq",factoryEvidence(filtered,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryEqBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-eq-starting-default";nominal.role="factory-eq";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::eq);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Contour Loom EQ2, all three gains0dB; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);const auto carriers=factoryBriefs(false);
    constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    for(int n=0;n<5;++n){auto b=carriers[std::size_t(n)];b.id="factory-eq-"+juce::String(names[n]);b.role="factory-eq";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::eq);
        if(n==0){setGainValue(b.processed,"effect.1.eq.low_shelf.frequency_hz",120);setGainValue(b.processed,"effect.1.eq.low_shelf.gain_db",3);setGainValue(b.processed,"effect.1.eq.bell.frequency_hz",700);setGainValue(b.processed,"effect.1.eq.bell.gain_db",-3);}
        if(n==1){setGainValue(b.processed,"effect.1.eq.low_shelf.frequency_hz",180);setGainValue(b.processed,"effect.1.eq.low_shelf.gain_db",-3);setGainValue(b.processed,"effect.1.eq.bell.frequency_hz",2400);setGainValue(b.processed,"effect.1.eq.bell.gain_db",3);}
        if(n==2){setGainValue(b.processed,"effect.1.eq.bell.frequency_hz",1600);setGainValue(b.processed,"effect.1.eq.bell.gain_db",4);setGainValue(b.processed,"effect.1.eq.bell.q",2);}
        if(n==3){setGainValue(b.processed,"effect.1.eq.high_shelf.frequency_hz",6000);setGainValue(b.processed,"effect.1.eq.high_shelf.gain_db",-4);}
        if(n==4){setGainValue(b.processed,"effect.1.eq.low_shelf.frequency_hz",180);setGainValue(b.processed,"effect.1.eq.low_shelf.gain_db",-2);setGainValue(b.processed,"effect.1.eq.high_shelf.frequency_hz",7000);setGainValue(b.processed,"effect.1.eq.high_shelf.gain_db",3);}
        b.stimulus+="; actual Contour Loom EQ2 ordinary frequency/gain/Q edits; no other FX, output24dB/protectionON";result.push_back(std::move(b));
    }
    auto load=nominal;load.id="factory-eq-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual EQ carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::eq);const auto firstEq=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstEq;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a81"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual EQ factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched transparent Contour Loom EQ2 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"EQ untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual EQ factory fixture");eqFactoryEvidence(b.dry);eqFactoryEvidence(b.processed);}return result;
}

juce::var filterFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::filter)};
    require(document.personalities!=nullptr,"filter carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"filter packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto filtered=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});filtered.personalities=state;
        property(result,n==0?"carrier":"filter",factoryEvidence(filtered,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryFilterBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-filter-starting-default";nominal.role="factory-filter";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::filter);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Veil Current Filter2, LP4000Hz/Q.70710678/drive0dB/mix1; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);const auto carriers=factoryBriefs(false);
    constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    for(int n=0;n<5;++n){auto b=carriers[std::size_t(n)];b.id="factory-filter-"+juce::String(names[n]);b.role="factory-filter";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::filter);
        if(n==0){setGainValue(b.processed,"effect.1.filter_fx.cutoff_hz",1000);setGainValue(b.processed,"effect.1.filter_fx.resonance",.8f);}
        if(n==1){setGainValue(b.processed,"effect.1.filter_fx.mode",1);setGainValue(b.processed,"effect.1.filter_fx.cutoff_hz",1800);setGainValue(b.processed,"effect.1.filter_fx.resonance",1.2f);setGainValue(b.processed,"effect.1.filter_fx.mix",.75f);}
        if(n==2){setGainValue(b.processed,"effect.1.filter_fx.cutoff_hz",3000);setGainValue(b.processed,"effect.1.filter_fx.resonance",2);setGainValue(b.processed,"effect.1.filter_fx.drive_db",6);setGainValue(b.processed,"effect.1.filter_fx.mix",.75f);}
        if(n==3){setGainValue(b.processed,"effect.1.filter_fx.mode",2);setGainValue(b.processed,"effect.1.filter_fx.cutoff_hz",250);setGainValue(b.processed,"effect.1.filter_fx.resonance",.70710678f);}
        if(n==4){setGainValue(b.processed,"effect.1.filter_fx.cutoff_hz",2000);setGainValue(b.processed,"effect.1.filter_fx.resonance",.9f);setGainValue(b.processed,"effect.1.filter_fx.mix",.65f);}
        b.stimulus+="; actual Veil Current Filter2 ordinary mode/cutoff/Q/drive/mix edits; no other FX, output24dB/protectionON";result.push_back(std::move(b));
    }
    auto load=nominal;load.id="factory-filter-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual filter carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::filter);const auto firstFilter=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstFilter;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842b81"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual filter factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Veil Current Filter2 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"filter untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual filter factory fixture");filterFactoryEvidence(b.dry);filterFactoryEvidence(b.processed);}return result;
}

juce::var chorusFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::chorus)};
    require(document.personalities!=nullptr,"chorus carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"chorus packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto chorused=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});chorused.personalities=state;
        property(result,n==0?"carrier":"chorus",factoryEvidence(chorused,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryChorusBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-chorus-starting-default";nominal.role="factory-chorus";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::chorus);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Orbit Ribbon Chorus2, rate.35Hz/depth.25/centre18ms/feedback.1/mix.25; stock shared-LFO dual mono, no stereo widening; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);const auto carriers=factoryBriefs(false);
    constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    for(int n=0;n<5;++n){auto b=carriers[std::size_t(n)];b.id="factory-chorus-"+juce::String(names[n]);b.role="factory-chorus";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::chorus);
        constexpr float settings[5][5]{{.2f,.15f,12,0,.15f},{.8f,.35f,18,.15f,.25f},{1.2f,.5f,20,-.2f,.35f},{.4f,.3f,16,.2f,.3f},{.15f,.7f,30,.3f,.4f}};
        constexpr const char* keys[]{"chorus.rate_hz","chorus.depth","chorus.base_delay_ms","chorus.feedback","chorus.mix"};
        for(int field=0;field<5;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; actual Orbit Ribbon Chorus2 ordinary rate/depth/centre/feedback/mix edits; shared-LFO dual mono, no widening; no other FX, output24dB/protectionON";result.push_back(std::move(b));
    }
    auto load=nominal;load.id="factory-chorus-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual chorus carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::chorus);const auto firstChorus=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstChorus;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c81"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual chorus factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Orbit Ribbon Chorus2 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"chorus untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual chorus factory fixture");chorusFactoryEvidence(b.dry);chorusFactoryEvidence(b.processed);}return result;
}

juce::var phaserFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::phaser)};
    require(document.personalities!=nullptr,"phaser carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"phaser packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto phasered=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});phasered.personalities=state;
        property(result,n==0?"carrier":"phaser",factoryEvidence(phasered,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryPhaserBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-phaser-starting-default";nominal.role="factory-phaser";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::phaser);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Aster Sweep Phaser1, rate.35Hz/depth.6/centre1300Hz/feedback.2/mix.5; stock shared-LFO dual mono, no stereo widening; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);const auto carriers=factoryBriefs(false);
    constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    for(int n=0;n<5;++n){auto b=carriers[std::size_t(n)];b.id="factory-phaser-"+juce::String(names[n]);b.role="factory-phaser";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::phaser);
        constexpr float settings[5][5]{{.15f,.25f,250,.1f,.3f},{.8f,.4f,1000,.15f,.4f},{1.2f,.65f,1600,-.25f,.5f},{.35f,.5f,800,.25f,.45f},{.12f,.8f,1300,.35f,.5f}};
        constexpr const char* keys[]{"phaser.rate_hz","phaser.depth","phaser.centre_hz","phaser.feedback","phaser.mix"};
        for(int field=0;field<5;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; actual Aster Sweep Phaser1 ordinary rate/depth/centre/feedback/mix edits; shared-LFO dual mono, no widening; no other FX, output24dB/protectionON";result.push_back(std::move(b));
    }
    auto load=nominal;load.id="factory-phaser-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual phaser carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::phaser);const auto firstPhaser=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstPhaser;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842d91"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual phaser factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Aster Sweep Phaser1 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"phaser untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual phaser factory fixture");phaserFactoryEvidence(b.dry);phaserFactoryEvidence(b.processed);}return result;
}

juce::var flangerFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::flanger)};
    require(document.personalities!=nullptr,"flanger carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"flanger packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto flangered=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});flangered.personalities=state;
        property(result,n==0?"carrier":"flanger",factoryEvidence(flangered,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryFlangerBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-flanger-starting-default";nominal.role="factory-flanger";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::flanger);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Silver Loop Flanger2, rate.25Hz/depth.6/base2ms/feedback.35/mix.5; prepared windowed-sinc shared-phase dual mono, no stereo widening; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);const auto carriers=factoryBriefs(false);
    constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    for(int n=0;n<5;++n){auto b=carriers[std::size_t(n)];b.id="factory-flanger-"+juce::String(names[n]);b.role="factory-flanger";b.processed=placeCoreFxFactory(b.dry,airack::NativeModule::flanger);
        constexpr float settings[5][5]{{.15f,.4f,1,.55f,.35f},{.8f,.4f,1.25f,-.6f,.4f},{.7f,.8f,2,.6f,.45f},{1.8f,.65f,4,-.6f,.55f},{.12f,.75f,6,-.35f,.6f}};
        constexpr const char* keys[]{"flanger.rate_hz","flanger.depth","flanger.base_delay_ms","flanger.feedback","flanger.mix"};
        for(int field=0;field<5;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; actual Silver Loop Flanger2 ordinary rate/depth/base-delay/feedback/mix edits; shared-LFO dual mono, no widening; no other FX, output24dB/protectionON";result.push_back(std::move(b));
    }
    auto load=nominal;load.id="factory-flanger-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual flanger carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::flanger);const auto firstFlanger=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstFlanger;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842e91"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual flanger factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Silver Loop Flanger2 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"flanger untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual flanger factory fixture");flangerFactoryEvidence(b.dry);flangerFactoryEvidence(b.processed);}return result;
}

juce::var stereoFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::stereo)};
    require(document.personalities!=nullptr,"stereo carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"stereo packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto stereoed=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});stereoed.personalities=state;
        property(result,n==0?"carrier":"stereo",factoryEvidence(stereoed,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryStereoBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-stereo-starting-default";nominal.role="factory-stereo";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::stereo);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Centre Line Stereo1 neutral gain0dB/balance0/width1/Stereo; memoryless existing-side algebra, no mono-generated width; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);constexpr const char* names[]{"narrow","wide","left","mono","swap"};
    constexpr float settings[5][4]{{0,0,.5f,0},{-6,0,2,0},{0,-.5f,1,0},{0,0,1,2},{0,0,1,1}};
    constexpr const char* keys[]{"stereo.gain_db","stereo.pan","stereo.width","stereo.channel_mode"};
    for(int n=0;n<5;++n){auto b=nominal;b.id="factory-stereo-"+juce::String(names[n]);
        if(n<2){setGainValue(b.dry,"synth.1.pan",-.35f);setGainValue(b.processed,"synth.1.pan",-.35f);b.stimulus+="; identical ordinary Copper Arc pan-.35 source edit supplies actual existing side for width comparison";}
        for(int field=0;field<4;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; ordinary same-definition "+juce::String(names[n])+" gain/balance/existing-side-width/channel-mode edits; mode changes do not add audio support";result.push_back(std::move(b));}
    auto load=nominal;load.id="factory-stereo-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual stereo carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::stereo);const auto firstStereo=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstStereo;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842f91"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual stereo factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Centre Line Stereo1 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"stereo untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual stereo factory fixture");stereoFactoryEvidence(b.dry);stereoFactoryEvidence(b.processed);}return result;
}

juce::var dimensionFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::dimension)};
    require(document.personalities!=nullptr,"dimension carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"dimension packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto dimensioned=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});dimensioned.personalities=state;
        property(result,n==0?"carrier":"dimension",factoryEvidence(dimensioned,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryDimensionBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-dimension-starting-default";nominal.role="factory-dimension";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::dimension);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc carrier and Twin Current Dimension2 defaults depth.6/rate.35Hz/generated-side-width1/mix.5; finite mono-history opposed taps generate side and retain the original fold within float tolerance; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    constexpr float settings[5][4]{{.2f,.15f,.6f,.25f},{.35f,.7f,1,.3f},{.6f,1.1f,1.3f,.45f},{.45f,.4f,.9f,.35f},{.85f,.12f,1.5f,.5f}};
    constexpr const char* keys[]{"dimension.depth","dimension.rate_hz","dimension.width","dimension.mix"};
    for(int n=0;n<5;++n){auto b=nominal;b.id="factory-dimension-"+juce::String(names[n]);
        for(int field=0;field<4;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; ordinary same-definition "+juce::String(names[n])+" depth/rate/generated-side-width/mix edits; original fold retained within float tolerance";result.push_back(std::move(b));}
    auto load=nominal;load.id="factory-dimension-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual dimension carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::dimension);const auto firstDimension=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstDimension;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842aa1"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual dimension factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Twin Current Dimension2 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"dimension untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual dimension factory fixture");dimensionFactoryEvidence(b.dry);dimensionFactoryEvidence(b.processed);}return result;
}

juce::var frequencyShiftFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::frequencyShift)};
    require(document.personalities!=nullptr,"frequency-shift carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"frequency-shift packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto frequencyShifted=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});frequencyShifted.personalities=state;
        property(result,n==0?"carrier":"frequency_shift",factoryEvidence(frequencyShifted,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryFrequencyShiftBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-frequency-shift-starting-default";nominal.role="factory-frequency-shift";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::frequencyShift);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc and Sideband Tide FrequencyShift1 defaults+7Hz/offset0Hz/mix.25; translated quadrature carrier, common-D coloration atzeroHz, symmetric offset translations; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);constexpr const char* names[]{"warm-bass","clean-pluck","forward-lead","soft-brass","air-pad"};
    constexpr float settings[5][3]{{-7,0,.25f},{7,0,.5f},{17,0,.5f},{7,37,.4f},{0,0,1}};
    constexpr const char* keys[]{"frequency_shift.hz","frequency_shift.stereo_offset_hz","frequency_shift.mix"};
    for(int n=0;n<5;++n){auto b=nominal;b.id="factory-frequency-shift-"+juce::String(names[n]);
        for(int field=0;field<3;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; ordinary same-definition "+juce::String(names[n])+" Hz/symmetric-stereo-offset/mix edits; shift does not preserve waveform phase or mono fold";result.push_back(std::move(b));}
    auto load=nominal;load.id="factory-frequency-shift-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual frequency-shift carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::frequencyShift);const auto firstFrequencyShift=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstFrequencyShift;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bb1"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual frequency-shift factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Sideband Tide FrequencyShift1 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"frequency-shift untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual frequency-shift factory fixture");frequencyShiftFactoryEvidence(b.dry);frequencyShiftFactoryEvidence(b.processed);}return result;
}

juce::var midSideSplitFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::midSideSplit)};
    require(document.personalities!=nullptr,"midSideSplit carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"midSideSplit packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto midSideSplited=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});midSideSplited.personalities=state;
        property(result,n==0?"carrier":"mid_side_split",factoryEvidence(midSideSplited,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryMidSideSplitBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-mid-side-split-starting-default";nominal.role="factory-mid-side-split";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::midSideSplit);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1 release2s; untouched Copper Arc and Field Balance MidSideSplit1 neutral0dB/unmuted/tone0/mix1, both histories clock/direct original-bit return; master1/output24dB/protectionON/no other FX";
    result.push_back(nominal);constexpr const char* names[]{"centre-focus","side-solo","mono","dark-sides","wider-existing-side"};
    constexpr float settings[5][7]{{0,0,0,-6,0,0,1},{0,1,0,0,0,0,1},{0,0,0,0,1,0,1},{0,0,0,0,0,-.5f,1},{0,0,0,6,0,0,1}};
    constexpr const char* keys[]{"mid_side_split.mid_gain_db","mid_side_split.mid_mute","mid_side_split.mid_tone","mid_side_split.side_gain_db","mid_side_split.side_mute","mid_side_split.side_tone","mid_side_split.mix"};
    for(int n=0;n<5;++n){auto b=nominal;b.id="factory-mid-side-split-"+juce::String(names[n]);setGainValue(b.dry,"synth.1.pan",-.35f);setGainValue(b.processed,"synth.1.pan",-.35f);for(int field=0;field<7;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);b.stimulus="MIDI60/64/67 velocity100/channel1 release2s; matched Copper Arc carrier with ordinary pan-.35 in BOTH dry/processed preserves existing Side; same Field Balance definition with ordinary "+juce::String(names[n])+" gain/mute/tone control edits and mix1; master1/output24dB/protectionON/no other FX, no mono widening";result.push_back(std::move(b));}
    auto load=nominal;load.id="factory-mid-side-split-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual mid side split carrier layers rejected");load.dry=std::move(next);}
    constexpr float pans[]{-.35f,.35f,-.2f,.2f};for(int slot=0;slot<4;++slot)setGainValue(load.dry,"synth."+juce::String(slot+1)+".pan",pans[slot]);
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::midSideSplit);const auto first=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=first;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842be1"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight independent mid side split factory instances rejected");load.processed=std::move(next);}
    for(int slot=0;slot<8;++slot){const std::array<float,7> values{-3,0,-.2f,2,0,.3f,.8f};for(int field=0;field<7;++field)setGainValue(load.processed,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
    load.stimulus="MIDI48..63 velocity100/channel1 release2s; four Copper Arc definitions with ordinary opposing pan-.35,+.35,-.2,+.2 edits/eight independent Field Balance definitions with ordinary Mid-3dB,tone-.2/Side+2dB,tone+.3/mix.8 edits; uninterrupted22s engineering observation, source and HigherFIR support separate";result.push_back(std::move(load));
    require(result.size()==7,"mid side split nominal/five roles/load incomplete");for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual mid side split fixture");midSideSplitFactoryEvidence(b.dry);midSideSplitFactoryEvidence(b.processed);}return result;
}
juce::var frequencySplitFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::frequencySplit)};
    require(document.personalities!=nullptr,"frequencySplit carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"frequencySplit packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto frequencySplited=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});frequencySplited.personalities=state;
        property(result,n==0?"carrier":"frequency_split",factoryEvidence(frequencySplited,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryFrequencySplitBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-frequency-split-starting-default";nominal.role="factory-frequency-split";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::frequencySplit);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1 release2s; untouched Copper Arc and Band Balance FrequencySplit2 neutral1000Hz/0dB/unmuted/tone0/mix1, Linkwitz-Riley bands summing to an allpass at the source level; master1/output24dB/protectionON/no other FX";
    result.push_back(nominal);constexpr const char* names[]{"low-solo","high-solo","warm-low","bright-high","low-cut"};
    constexpr float settings[5][8]{{1000,0,0,0,0,1,0,1},{1000,0,1,0,0,0,0,1},{1000,0,0,-.5f,-3,0,0,1},{1000,0,0,0,3,0,.5f,1},{1000,-12,0,0,0,0,0,1}};
    constexpr const char* keys[]{"frequency_split.crossover_hz","frequency_split.low_gain_db","frequency_split.low_mute","frequency_split.low_tone","frequency_split.high_gain_db","frequency_split.high_mute","frequency_split.high_tone","frequency_split.mix"};
    for(int n=0;n<5;++n){auto b=nominal;b.id="factory-frequency-split-"+juce::String(names[n]);for(int field=0;field<8;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);b.stimulus+="; ordinary same-definition "+juce::String(names[n])+" independent complementary-band gain/mute/tone edits";result.push_back(std::move(b));}
    auto load=nominal;load.id="factory-frequency-split-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual frequency split carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::frequencySplit);const auto first=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=first;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bd1"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight independent frequency split factory instances rejected");load.processed=std::move(next);}
    for(int slot=0;slot<8;++slot){const std::array<float,8> values{300.f+131*slot,-3,0,-.2f,2,0,.3f,.8f};for(int field=0;field<8;++field)setGainValue(load.processed,"effect."+juce::String(slot+1)+"."+keys[field],values[field]);}
    load.stimulus="MIDI48..63 velocity100/channel1 release2s; four untouched Copper Arc layers/eight independent Band Balance definitions with ordinary nonneutral crossover300+131slot/low-3dB,tone-.2/high+2dB,tone+.3/mix.8 edits; uninterrupted22s engineering observation, source and HigherFIR support separate";result.push_back(std::move(load));
    require(result.size()==7,"frequency split nominal/five roles/load incomplete");for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual frequency split fixture");frequencySplitFactoryEvidence(b.dry);frequencySplitFactoryEvidence(b.processed);}return result;
}
juce::var convolutionFactoryEvidence(const airack::SoundDocument& document)
{
    auto result=jsonObject();const std::array<airack::RackDesign,2> designs{qualityFactory(false),coreFxFactory(airack::NativeModule::convolution)};
    require(document.personalities!=nullptr,"convolution carrier snapshot missing");
    for(const auto& instance:document.personalities->instances)require(std::any_of(designs.begin(),designs.end(),[&](const auto& d){return instance.design==d;}),"convolution packet changed an embedded definition");
    for(std::size_t n=0;n<designs.size();++n){auto convolutioned=document;auto state=std::make_shared<airack::RackPersonalityState>(*document.personalities);
        std::erase_if(state->instances,[&](const auto& instance){return instance.design.id!=designs[n].id;});convolutioned.personalities=state;
        property(result,n==0?"carrier":"convolution",factoryEvidence(convolutioned,designs[n]));}
    return result;
}
std::vector<QualityBrief> factoryConvolutionBriefs()
{
    std::vector<QualityBrief> result;QualityBrief nominal;nominal.id="factory-convolution-starting-default";nominal.role="factory-convolution";
    nominal.dry=factorySource(false);nominal.processed=placeCoreFxFactory(nominal.dry,airack::NativeModule::convolution);nominal.notes={60,64,67};
    nominal.stimulus="MIDI60/64/67 velocity100/channel1, release2s; untouched actual Copper Arc and Ember Room Convolution1 defaults ShortRoom/predelay0/trim0dB/mix.25; original canonical48000 planar stereo diagonal IR, SRC+rate gain, TrimOFF NormaliseOFF; master1/output24dB/protectionON, no other FX";
    result.push_back(nominal);constexpr const char* names[]{"unit-reference","room-wet","predelay","wet-trim","room-blend"};
    constexpr float settings[5][4]{{0,0,0,1},{1,0,0,1},{1,80,0,.5f},{1,25,-9,.45f},{1,10,3,.35f}};
    constexpr const char* keys[]{"convolution.ir","convolution.predelay_ms","convolution.wet_trim_db","convolution.mix"};
    for(int n=0;n<5;++n){auto b=nominal;b.id="factory-convolution-"+juce::String(names[n]);
        for(int field=0;field<4;++field)setGainValue(b.processed,"effect.1."+juce::String(keys[field]),settings[n][field]);
        b.stimulus+="; ordinary same-definition "+juce::String(names[n])+" IR/predelay/wet-trim/mix edits; stereo diagonal convolution does not require mono fold preservation";result.push_back(std::move(b));}
    auto load=nominal;load.id="factory-convolution-full-workload-tail";load.role="factory-load";load.notes.clear();for(int n=0;n<16;++n)load.notes.push_back(48+n);load.hold=2;load.duration=22;
    const auto selected=load.dry.personalities->instances[0].id;
    for(int n=1;n<4;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=selected;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a41"+juce::String(n+1));op.destination.position=std::uint8_t(n);
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.dry,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Four actual convolution carrier layers rejected");load.dry=std::move(next);}
    load.processed=placeCoreFxFactory(load.dry,airack::NativeModule::convolution);const auto firstConvolution=load.processed.personalities->instances.back().id;
    for(int n=1;n<8;++n){airack::RackOperation op;op.kind=airack::RackOperationKind::duplicate;op.selected=firstConvolution;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bc1"+juce::String(n+1));op.destination={airack::RackRole::fx,airack::masterChain,std::uint8_t(n)};
        airack::SoundDocument next;airack::RackDifference delta;require(airack::makeRackCandidate(load.processed,op,airack::SoundEditOrigin::manual,next,delta)==airack::RackCandidateStatus::ready,"Eight actual convolution factory instances rejected");load.processed=std::move(next);}
    load.stimulus="MIDI48..63 velocity100/channel1, release2s; four independent untouched Copper Arc layers and eight independent untouched Ember Room Convolution1 inserts; uninterrupted22s active engineering observation, not quiescence";
    result.push_back(std::move(load));require(result.size()==7,"convolution untouched/role/load packet incomplete");
    for(const auto& b:result){require(airack::validateSoundDocument(b.dry)&&airack::validateSoundDocument(b.processed),"Invalid actual convolution factory fixture");convolutionFactoryEvidence(b.dry);convolutionFactoryEvidence(b.processed);}return result;
}

std::vector<Event> qualityEvents(const QualityBrief& b,int rate)
{
    std::vector<Event> events;const int repetitions=b.repeated?4:1;
    for(int repeat=0;repeat<repetitions;++repeat)for(std::size_t n=0;n<b.notes.size();++n){const int start=int((repeat*.5+(b.sweep?n*.25:0))*rate);events.push_back({start,juce::MidiMessage::noteOn(1,b.notes[n],juce::uint8(100))});events.push_back({start+(b.singleSampleNoise?1:int(b.hold*rate)),juce::MidiMessage::noteOff(1,b.notes[n])});}
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& c){return a.sample<c.sample;});return events;
}
juce::AudioBuffer<float> renderFactoryNative(const airack::SoundDocument& document,const std::vector<Event>& events,int rate,int frames,airack::RenderQuality quality,bool tempoPresent=false)
{
    airack::RoutedPatch patch;airack::RoutedSynthEngine engine;require(airack::makeRoutedPatch(document,patch),"Native factory document mapping failed");require(engine.setPatch(patch),"Native factory patch admission failed");require(engine.prepare(rate,512,quality),"Native factory preparation failed");
    juce::AudioBuffer<float> audio(2,frames),block(2,512);juce::MidiBuffer midi;midi.ensureSize(65536);std::size_t next=0;
    for(int offset=0;offset<frames;offset+=512){const int count=std::min(512,frames-offset);block.setSize(2,count,false,false,true);midi.clear();while(next<events.size()&&events[next].sample<offset+count){midi.addEvent(events[next].message,events[next].sample-offset);++next;}engine.render(block,midi,tempoPresent?std::optional<double>{120}:std::nullopt);for(int ch=0;ch<2;++ch)audio.copyFrom(ch,offset,block,ch,0,count);}
    require(next==events.size(),"Native factory MIDI incomplete");return audio;
}
// The wavetable warmth listening pack: Night Dial's starting sound, its registered design's unit defaults placed in
// synth slot 1 on registry defaults, as the LIBRARY places it, and four roles as edits of it.
struct WarmthNote { double start, length; int note; };
struct WarmthRole { juce::String id, description; airack::SoundDocument document; std::vector<WarmthNote> notes; double seconds = 0; };
std::vector<WarmthRole> warmthRoles()
{
    const juce::String s="synth.1.",w=s+"wavetable.";
    const auto night=airack::test::frozenExampleRackDesigns()[5];require(night.name=="Night Dial"&&night.units.size()==1,"Night Dial is not the sixth example design");
    const auto& unit=night.units[0];
    airack::SoundDocument recipe;recipe.compatibility.synth[0][static_cast<std::size_t>(unit.module)]=unit.algorithmRevision;
    for(const auto& p:unit.defaults){const auto index=airack::rackParameterIndex(unit.module,0,p.key);require(index.has_value(),"Night Dial default without a parameter");recipe.values[*index]=p.normalized;}
    const auto route=[&](airack::SoundDocument& d,int n,airack::ModSource source,airack::ModTarget target,float amount){const auto r=s+"modulation."+juce::String(n)+".";
        setGainValue(d,r+"enabled",1);setGainValue(d,r+"source",float(source));setGainValue(d,r+"target",float(target));setGainValue(d,r+"bank",float(airack::ModBank::wavetable));setGainValue(d,r+"amount",amount);};
    const auto chord=[](std::initializer_list<int> notes,double length){std::vector<WarmthNote> r;for(int n:notes)r.push_back({0,length,n});return r;};
    std::vector<WarmthRole> result;
    result.push_back({"pad","The starting sound: a held A minor 9 for 12 s.",recipe,chord({45,57,60,64,67,71},12),16});
    auto swell=recipe;setGainValue(swell,w+"amp.attack",6);setGainValue(swell,w+"amp.attack_curve",-1);
    result.push_back({"swell","A 6 s attack at curve -1: a held F major 7 for 14 s.",swell,chord({41,53,57,60,64},14),18});
    // The recipe's 1.5 s attack and 3 s release would blur a bass line, so the reese also plays them short.
    auto reese=recipe;setGainValue(reese,w+"osc.1.unison",6);setGainValue(reese,w+"osc.1.unison_cents",22);setGainValue(reese,w+"unison_width",.4f);
    setGainValue(reese,w+"filter.type",1);setGainValue(reese,w+"filter.cutoff",600);setGainValue(reese,w+"filter.resonance",.2f);
    setGainValue(reese,w+"osc.2.position",0);setGainValue(reese,w+"amp.attack",.01f);setGainValue(reese,w+"amp.release",.3f);
    result.push_back({"reese","A reese over a clean sub: osc 1 at 7 voices and 22 cents, width 0.4, through the Ladder at 600 Hz; osc 2 one sine voice an octave down. A two-bar line at 120 BPM, attack 10 ms and release 0.3 s.",
        reese,{{0,1,45},{1,.5,45},{1.5,.5,48},{2,1,43},{3,.75,41},{3.75,.25,43}},5});
    auto choir=recipe;setGainValue(choir,w+"filter.type",3);setGainValue(choir,w+"filter.cutoff",700);setGainValue(choir,w+"filter.vowel_size",1.2f);setGainValue(choir,w+"filter.resonance",.6f);
    setGainValue(choir,s+"lfo.2.shape",0);setGainValue(choir,s+"lfo.2.synced",0);setGainValue(choir,s+"lfo.2.rate_hz",.2f);route(choir,3,airack::ModSource::lfo2,airack::ModTarget::cutoff,.15f);
    result.push_back({"vowel-choir","The Vowel filter at 700 Hz, size 1.2 and resonance 0.6, LFO 2 at 0.2 Hz gliding the vowel: a held D minor 7.",choir,chord({50,53,57,60},10),14});
    auto pluck=recipe;setGainValue(pluck,w+"amp.attack",.002f);setGainValue(pluck,w+"amp.attack_curve",1);setGainValue(pluck,w+"amp.decay",.35f);setGainValue(pluck,w+"amp.sustain",0);setGainValue(pluck,w+"amp.release",.4f);setGainValue(pluck,w+"filter.cutoff",2200);
    std::vector<WarmthNote> phrase;const std::array<int,16> steps{69,0,72,76,0,74,72,0,69,0,67,69,0,72,0,76};
    for(int bar=0;bar<2;++bar)for(std::size_t n=0;n<steps.size();++n)if(steps[n]!=0)phrase.push_back({bar*2+double(n)*.125,.1,steps[n]});
    result.push_back({"pluck","A 2 ms attack at curve 1, decay 0.35 s, sustain 0 and release 0.4 s through Classic at 2.2 kHz: two bars of sixteenths at 120 BPM.",pluck,phrase,5.5});
    for(const auto& r:result)require(airack::validateSoundDocument(r.document),"Invalid warmth listening document");
    return result;
}
std::vector<Event> warmthEvents(const WarmthRole& role,int rate)
{
    std::vector<Event> events;
    for(const auto& n:role.notes){const int on=int(std::llround(n.start*rate));events.push_back({on,juce::MidiMessage::noteOn(1,n.note,juce::uint8(100))});events.push_back({on+int(std::llround(n.length*rate)),juce::MidiMessage::noteOff(1,n.note)});}
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.sample<b.sample;});return events;
}
int renderWarmthListening(const Arguments& args,juce::VST3PluginFormat& format,const juce::PluginDescription& description)
{
    constexpr int rate=48000;const bool higher=args.quality==airack::RenderQuality::higher;const auto module=binaryIn(args.plugin);const auto moduleHash=hash(module);
    auto report=jsonObject();property(report,"configuration",RACKSYNTH_CONFIGURATION);property(report,"plugin_before",identity(module));property(report,"sample_rate",rate);property(report,"selected_quality",higher?"higher":"realtime");
    property(report,"levels","Unnormalised: the plugin's output at registry-default master, output gain and protection");property(report,"listening","NOT_RUN");
    juce::String html="<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>Wavetable warmth listening pack</title></head><body>\n<h1>Wavetable warmth listening pack</h1>\n"
        "<p>Night Dial's starting sound and four roles as edits of it, rendered through the built VST3 at 48 kHz. Levels are as the plugin renders them, not normalised.</p>\n";
    juce::Array<juce::var> roles;
    for(const auto& role:warmthRoles()){
        auto document=role.document;document.values[758]=higher?1.f:0.f;const auto events=warmthEvents(role,rate);const int frames=int(role.seconds*rate);
        juce::MemoryBlock owned;require(airack::encodeSoundState(document,owned),"Warmth state encoding failed");const auto state=args.output.getChildFile(role.id+".soundstate");writeBlock(state,owned);
        const auto baseline=render(format,description,rate,512,hostEnvelope(owned),document,airack::currentSoundStateSize,true,-1,&events,frames);
        const auto partition=render(format,description,rate,64,hostEnvelope(owned),document,airack::currentSoundStateSize,true,-1,&events,frames);
        const double pd=difference(baseline.audio,partition.audio),nd=difference(baseline.audio,renderFactoryNative(document,events,rate,frames,args.quality));
        require(pd<=1e-6&&nd<=1e-6,"Warmth role's loaded, partitioned and native renders differ");
        const auto measured=measureWindow(baseline.audio,{{0,frames}});require(measured.rms>1e-6,"Silent warmth role");
        const auto wav=args.output.getChildFile(role.id+".wav");writeWav(wav,baseline.audio,rate);
        auto ticks=events;for(auto& e:ticks)e.sample=int(std::llround(double(e.sample)*960/rate));const auto midi=args.output.getChildFile(role.id+".mid");writeMidiEvents(midi,ticks);
        auto item=jsonObject();property(item,"id",role.id);property(item,"description",role.description);property(item,"wav",identity(wav));property(item,"soundstate",identity(state));property(item,"midi",identity(midi));
        property(item,"frames",frames);property(item,"host_latency_samples",baseline.latencySamples);property(item,"peak",measured.peak);property(item,"rms",measured.rms);property(item,"partition64_difference",pd);property(item,"native_difference",nd);roles.add(item);
        html+="<h2>"+role.id+"</h2>\n<p>"+role.description+"</p>\n<audio controls preload=\"none\" src=\""+role.id+".wav\"></audio>\n<p>Peak "+juce::String(20*std::log10(measured.peak),1)+" dBFS, RMS "+juce::String(20*std::log10(measured.rms),1)+" dBFS. SHA256 "+hash(wav)+".</p>\n";
        std::cout<<"WARMTH_LISTENING "<<role.id<<" peak="<<measured.peak<<" rms="<<measured.rms<<" partition64="<<pd<<" native="<<nd<<'\n'<<std::flush;
    }
    require(hash(module)==moduleHash,"Module changed during render");property(report,"roles",roles);property(report,"plugin_after",identity(module));property(report,"success",true);
    html+="</body></html>\n";writeBlock(args.output.getChildFile("index.html"),juce::MemoryBlock(html.toRawUTF8(),html.getNumBytesAsUTF8()));
    const auto json=juce::JSON::toString(report,false);writeBlock(args.output.getChildFile("report.json"),juce::MemoryBlock(json.toRawUTF8(),json.getNumBytesAsUTF8()));
    std::cout<<"SUCCESS wavetable warmth listening pack: "<<roles.size()<<" roles through the loaded VST3\n";return 0;
}
juce::var qualitySourceIdentities()
{
    juce::Array<juce::var> sources;const juce::File root(RACKSYNTH_WORKSPACE);auto files=root.getChildFile("src").findChildFiles(juce::File::findFiles,true,"*.cpp;*.h");
    files.add(root.getChildFile("CMakeLists.txt"));files.add(root.getChildFile("tools/native/RenderFixtures.cpp"));files.add(root.getChildFile("tests/native/SynthEngineTests.cpp"));files.add(root.getChildFile("tests/native/RoutedSynthEngineTests.cpp"));files.sort();for(const auto& file:files)sources.add(identity(file));return sources;
}
juce::var productionQualityReference(juce::VST3PluginFormat& format,const juce::PluginDescription& description,const Arguments& args,bool filterReference=false)
{
    auto report=juce::var(new juce::DynamicObject);auto put=[](juce::var& o,const char* k,const juce::var& v){o.getDynamicObject()->setProperty(k,v);};
    juce::Array<juce::var> captures;
    for(int amount=0;amount<3;++amount)
    {
        airack::SoundDocument d;const juce::String bank="synth.1.analog.";
        setGainValue(d,"global.master_level",1);setGainValue(d,"synth.1.level",1);setGainValue(d,"output.gain_db",0);setGainValue(d,"output.protection",0);
        setGainValue(d,bank+"osc.1.waveform",float(airack::Waveform::sine));setGainValue(d,bank+"osc.1.level",1);setGainValue(d,bank+"osc.2.level",0);
        setGainValue(d,bank+"osc.1.tune",float(12*std::log2(5859.375/(440*std::exp2((114-69.)/12)))));
        setGainValue(d,bank+"noise",0);setGainValue(d,bank+"amp.attack",0);setGainValue(d,bank+"amp.decay",0);setGainValue(d,bank+"amp.sustain",1);setGainValue(d,bank+"amp.release",0);
        setGainValue(d,bank+"filter.mode",float(airack::FilterMode::highpass));setGainValue(d,bank+"filter.cutoff",20);setGainValue(d,bank+"filter.resonance",0);setGainValue(d,bank+"filter_env.octaves",0);
        if(filterReference&&amount){qualityEffect(d,airack::EffectKind::filter);setGainValue(d,"effect.1.filter_fx.mode",0);setGainValue(d,"effect.1.filter_fx.cutoff_hz",20000);setGainValue(d,"effect.1.filter_fx.resonance",.70710678f);setGainValue(d,"effect.1.filter_fx.drive_db",amount==1?0.f:24.f);setGainValue(d,"effect.1.filter_fx.mix",1);}
        else if(amount){qualityEffect(d,airack::EffectKind::drive);setGainValue(d,"effect.1.drive.gain_db",amount==1?0.f:24.f);setGainValue(d,"effect.1.drive.mix",1);setGainValue(d,"effect.1.drive.tone_hz",20000);}
        airack::RoutedPatch patch;require(airack::makeRoutedPatch(d,patch),"Production reference patch invalid");
        const double frequency=440*std::exp2((114-69.+patch.synth.layers[0].oscillators[0].tune)/12);
        for(int variant=0;variant<5;++variant)
        {
            const int rate=variant==4?48000:48000*(1<<variant);const auto quality=variant==4?airack::RenderQuality::higher:airack::RenderQuality::realtime;
            airack::RoutedSynthEngine engine;require(engine.setPatch(patch)&&engine.prepare(rate,512,quality),"Production reference prepare failed");
            const int frames=rate*3/4;juce::AudioBuffer<float> audio(2,frames),block(2,512);juce::MidiBuffer midi;midi.ensureSize(4096);
            const int noteFrame=17*(rate/48000);const auto start=std::chrono::steady_clock::now();
            for(int offset=0;offset<frames;offset+=512){const int count=std::min(512,frames-offset);block.setSize(2,count,false,false,true);midi.clear();if(noteFrame>=offset&&noteFrame<offset+count)midi.addEvent(juce::MidiMessage::noteOn(1,114,juce::uint8(100)),noteFrame-offset);engine.render(block,midi);for(int ch=0;ch<2;++ch)audio.copyFrom(ch,offset,block,ch,0,count);}
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            const auto id=juce::String(filterReference?"production-filter-":"production-")+juce::String(amount==0?"dry":amount==1?"drive-0db":"drive-24db")+"-"+juce::String(rate)+(variant==4?"-higher":"-realtime");
            const auto file=args.output.getChildFile(id+".wav");writeWav(file,audio,rate);
            auto item=juce::var(new juce::DynamicObject);put(item,"id",id);put(item,"raw",identity(file));put(item,"sample_rate",rate);put(item,"frames",frames);put(item,"quality",int(engine.effectiveQuality()));put(item,"host_latency_samples",engine.latencySamples());put(item,"production_frequency_hz",frequency);put(item,"amount",amount);if(filterReference){put(item,"filter_active",amount!=0);put(item,"filter_mode","lowpass");put(item,"filter_cutoff_hz",20000);put(item,"filter_q",double(patch.effects[0].filter.resonance));put(item,"drive_db",amount==2?24:0);put(item,"output_gain_db",0);put(item,"output_protection",false);put(item,"drive_law","x+a*(tanh(G*x)/G-x), a=d/24, G=10^(d/20); d0 is exact x");}put(item,"note_frame",noteFrame);put(item,"warmup_seconds",.5);put(item,"render_wall_ns",juce::int64(ns));put(item,"whole",windowJson(audio,"whole",{{0,frames}}));
            if(rate==48000 && quality==args.quality)
            {
                d.values[758]=quality==airack::RenderQuality::higher?1.f:0.f;juce::MemoryBlock encoded;require(airack::encodeSoundState(d,encoded),"Production source encode failed");
                const std::vector<Event> events{{17,juce::MidiMessage::noteOn(1,114,juce::uint8(100))}};
                auto loaded=render(format,description,48000,512,hostEnvelope(encoded),d,airack::currentSoundStateSize,false,-1,&events,frames);
                const auto error=difference(audio,loaded.audio);require(error<=1e-6 && loaded.latencySamples==engine.latencySamples(),"Actual loaded quality differs from independently prepared native engine");
                const auto loadedFile=args.output.getChildFile(id+"-loaded.wav");writeWav(loadedFile,loaded.audio,rate);writeBlock(args.output.getChildFile(id+".soundstate"),encoded);put(item,"loaded",identity(loadedFile));put(item,"loaded_latency_samples",loaded.latencySamples);put(item,"loaded_native_max_difference",error);
            }
            captures.add(item);
        }
    }
    put(report,"captures",captures);put(report,"signal",filterReference?"Actual Filter1 stationary sine/LP20000Hz/Q.70710678 with dry/drive0/drive24 controls; master1/output0 physical dB/protectionOFF; note114 velocity100, no modulation/noise; .5s warmup; raw unnormalized samples":"Actual revision1 production sine/filter/Drive; note114 velocity100, no LFO routes/noise/protector;0.5s warmup; raw unnormalized samples");
    put(report,"analysis_contract",filterReference?"Fixed physical-time stationary drive contract79EC7852; centered22kHz Kaiser12 reference at192/384k plus doubled384k FIR; actual-frequency known folds and dry/drive0 controls define measured sensitivity floor. Report TPT passband/phase residual separately, not pure alias energy; no fitted gain/phase/time.":"Compare fixed physical-time captures using centred lowpass references; actual fundamental/odd fold projections, dry/0dB controls and192k/384k convergence define this candidate's floor. Raw residual includes rate-dependent filter/ADAA response and is not pure alias energy.");return report;
}
// A single controlled matrix after the preserved blocking factory-a failure.
// Diagnostic observations cannot satisfy or replace the full factory gates.
int dimensionCostDiagnostic(const Arguments& args,juce::VST3PluginFormat& format,const juce::PluginDescription& description)
{
    require(args.factoryDimension&&args.quality==airack::RenderQuality::realtime,"Dimension diagnostic requires realtime factory selector");
    auto brief=factoryDimensionBriefs().back();const int rate=96000,frames=4*rate;
    const auto events=qualityEvents(brief,rate);require(events.size()==32,"Diagnostic must retain all16 note-on/off pairs");
    auto bypass=brief.processed;
    for(int slot=1;slot<=8;++slot)setGainValue(bypass,"effect."+juce::String(slot)+".bypassed",1);
    require(airack::validateSoundDocument(bypass),"Ordinary eight-insert bypass control rejected");
    auto report=jsonObject();property(report,"scope","Diagnosis only: unchanged explicitMIDI48..63 on0/off2s,4Copper/8Dimension,first callback included,4s window spanning held/release/quiet; two reversed condition orders; normal priority/affinity; no acceptance or budget change");
    property(report,"hypothesis","Determine whether active-insert failure reproduces as intrinsic thread work or primarily wall/cycle/residency variation. Paired ordinary bypass retains all rack instances and source/MIDI work; dry is a separate source control. Coarse CPU accounting and residency endpoints are qualified.");
    property(report,"source_before",qualitySourceIdentities());property(report,"plugin_before",identity(binaryIn(args.plugin)));
    juce::Array<juce::var> trials;
    const std::array<const airack::SoundDocument*,3> documents{&brief.processed,&bypass,&brief.dry};
    const std::array<const char*,3> names{"active","ordinary-bypass","dry"};
    for(int repeat=0;repeat<2;++repeat)for(int step=0;step<3;++step){const int condition=repeat?2-step:step;const auto& document=*documents[std::size_t(condition)];
        juce::MemoryBlock state;require(airack::encodeSoundState(document,state),"Diagnostic state encode failed");
        DimensionCostTrace trace;auto loaded=render(format,description,rate,512,hostEnvelope(state),document,airack::currentSoundStateSize,true,-1,&events,frames,true,&trace);
        require(loaded.latencySamples==0&&trace.rows.size()==750,"Diagnostic quality/count differs");
        auto item=jsonObject();property(item,"repeat",repeat+1);property(item,"condition",names[std::size_t(condition)]);property(item,"state_sha256",juce::SHA256(state).toHexString());property(item,"factory",dimensionFactoryEvidence(document));
        property(item,"sample_rate",rate);property(item,"frames",frames);property(item,"block_frames",512);property(item,"note_count",16);property(item,"note_on_frame",0);property(item,"note_off_frame",2*rate);property(item,"trace",trace.json());
        const auto stem="diagnostic-"+juce::String(repeat+1)+"-"+juce::String(names[std::size_t(condition)]);
        const auto audioFile=args.output.getChildFile(stem+"-raw.wav");writeWav(audioFile,loaded.audio,rate);property(item,"raw",identity(audioFile));
        // Preserve every completed trace/audio before a comparison can reject it.
        const auto partial=juce::JSON::toString(item,false);writeBlock(args.output.getChildFile(stem+"-trace.json"),juce::MemoryBlock(partial.toRawUTF8(),std::size_t(partial.getNumBytesAsUTF8())));
        if(condition!=1){const auto original=args.output.getParentDirectory().getChildFile("factory-realtime-a/factory-dimension-full-workload-tail-"+juce::String(condition==0?"processed":"dry")+"-96000-raw-512.wav");
            juce::WavAudioFormat wav;auto reader=std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(new juce::FileInputStream(original),true));
            require(reader&&reader->sampleRate==rate&&reader->numChannels==2&&reader->lengthInSamples>=frames,"Saved failed-a raw control missing or dimensions differ");
            juce::AudioBuffer<float> prefix(2,frames);require(reader->read(&prefix,0,frames,0,true,true),"Saved raw prefix read failed");
            const double delta=difference(loaded.audio,prefix);std::uint64_t bitDifferences=0;
            for(int channel=0;channel<2;++channel)for(int frame=0;frame<frames;++frame)
                bitDifferences+=std::bit_cast<std::uint32_t>(loaded.audio.getSample(channel,frame))!=std::bit_cast<std::uint32_t>(prefix.getSample(channel,frame));
            property(item,"original_raw",identity(original));property(item,"original_first_four_seconds_difference",delta);property(item,"original_first_four_seconds_bit_differences",juce::int64(bitDifferences));
            auto comparison=jsonObject();property(comparison,"maximum_difference",delta);property(comparison,"bit_differences",juce::int64(bitDifferences));
            const auto checked=juce::JSON::toString(comparison,false);writeBlock(args.output.getChildFile(stem+"-comparison.json"),juce::MemoryBlock(checked.toRawUTF8(),std::size_t(checked.getNumBytesAsUTF8())));
            require(delta==0&&bitDifferences==0,"Diagnostic changed bit-exact failed-a audio prefix");}
        trials.add(item);std::cout<<"DIMENSION_COST_DIAGNOSTIC repeat="<<repeat+1<<" condition="<<names[std::size_t(condition)]<<" callbacks="<<trace.rows.size()<<'\n'<<std::flush;
    }
    property(report,"trials",trials);property(report,"source_after",qualitySourceIdentities());property(report,"plugin_after",identity(binaryIn(args.plugin)));property(report,"completed",true);
    const auto output=juce::JSON::toString(report,false);writeBlock(args.output.getChildFile("dimension-cost-diagnostic.json"),juce::MemoryBlock(output.toRawUTF8(),std::size_t(output.getNumBytesAsUTF8())));return 0;
}

// Diagnosis of the actually failing internal-pattern steady source mode only.
// The normal matrix and its acceptance predicates do not invoke this entry.
int dimensionSteadyCostDiagnostic(const Arguments& args,juce::VST3PluginFormat& format,const juce::PluginDescription& description)
{
    require(args.factoryDimension&&args.quality==airack::RenderQuality::realtime,"Steady diagnostic requires realtime Dimension factory");
    const auto brief=factoryDimensionBriefs().back();
    airack::ProjectDocument active;active.sound=brief.processed;active.sound.values[758]=0;active.mode=airack::PlaybackMode::internalPattern;active.pattern.noteCount=16;
    for(int n=0;n<16;++n)active.pattern.notes[std::size_t(n)]={0,4,std::uint8_t(48+n),100,1};
    auto bypass=active;for(int slot=1;slot<=8;++slot)setGainValue(bypass.sound,"effect."+juce::String(slot)+".bypassed",1);
    auto dry=active;dry.sound=brief.dry;dry.sound.values[758]=0;
    const auto packet=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/sound-quality-task9-dimension-20261010-a");
    const auto originalProject=packet.getChildFile("factory-realtime-b/factory-full-workload.projectstate");
    juce::MemoryBlock expected,encoded;require(originalProject.loadFileAsData(expected)&&airack::encodeProjectState(active,encoded),"Original steady fixture unavailable");
    require(expected==encoded,"Steady diagnostic changed original active project bytes");
    const auto originalReport=juce::JSON::parse(packet.getChildFile("factory-realtime-b/sound-quality-report.json").loadFileAsString());
    require(originalReport["factory_timing_trials"].isArray()&&originalReport["factory_timing_trials"].size()==6,"Original steady observations unavailable");
    const auto original=originalReport["factory_timing_trials"][5];require(int(original["sample_rate"])==96000&&int(original["block_size"])==512&&int(original["trial"])==3,"Expected failed original steady trial");
    auto report=jsonObject();property(report,"scope","Diagnosis only: exact original internal-pattern4Copper/8Dimension/16notes,96k512,.25s warmup,188 measured callbacks,first measured callback retained; active/bypass/dry then reversed; no priority/affinity/workload/budget change");
    property(report,"hypothesis","Determine whether the actually failing steady source mode shows increased thread work or excess wall time; counters cannot reconstruct historical uninstrumented callbacks or satisfy acceptance");
    property(report,"original_project",identity(originalProject));property(report,"original_report",identity(packet.getChildFile("factory-realtime-b/sound-quality-report.json")));
    juce::Array<juce::var> trials;const std::array<const char*,3> names{"active","bypass","dry"};
    for(int order=0;order<2;++order)for(int position=0;position<3;++position){const int condition=order==0?position:2-position;const auto& project=condition==0?active:condition==1?bypass:dry;
        DimensionCostTrace trace;juce::AudioBuffer<float> audio;
        auto item=hostedPatternTrial(format,description,project,"steady-attribution",96000,512,order+1,true,&trace,&audio);
        require(int(item["warmup_frames"])==24064&&int(item["measured_frames"])==96256&&item["callback_ns"].size()==188,"Steady diagnostic changed measured window");
        property(item,"condition",names[std::size_t(condition)]);property(item,"order",order+1);property(item,"position",position+1);
        const auto stem="steady-"+juce::String(order+1)+"-"+juce::String(names[std::size_t(condition)]);
        juce::MemoryBlock state;require(airack::encodeProjectState(project,state),"Steady diagnostic project encode failed");
        const auto stateFile=args.output.getChildFile(stem+".projectstate");writeBlock(stateFile,state);property(item,"project",identity(stateFile));
        const auto audioFile=args.output.getChildFile(stem+"-raw.wav");writeWav(audioFile,audio,96000);property(item,"raw",identity(audioFile));
        // Preserve completed raw/trace before a comparison or integrity rejection.
        auto json=juce::JSON::toString(item,false);writeBlock(args.output.getChildFile(stem+"-trace.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));
        if(condition==0){std::uint64_t differentPeaks=0;require(item["audio_observations"].size()==original["audio_observations"].size(),"Original steady observation count differs");
            for(int n=0;n<188;++n){const auto& current=item["audio_observations"][n];const auto& prior=original["audio_observations"][n];
                differentPeaks+=std::bit_cast<std::uint32_t>(float(double(current["peak"])))!=std::bit_cast<std::uint32_t>(float(double(prior["peak"])));require(bool(current["finite"])&&bool(prior["finite"]),"Steady samples must remain finite");}
            property(item,"original_peak_float_bit_differences",juce::int64(differentPeaks));json=juce::JSON::toString(item,false);writeBlock(args.output.getChildFile(stem+"-comparison.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));
            require(item["fixture_sha256"].toString().equalsIgnoreCase(hash(originalProject))&&differentPeaks==0,"Steady diagnostic changed captured source/peak observations");}
        require(bool(item["integrity"]),"Steady diagnostic audio integrity failed");trials.add(item);
        std::cout<<"STEADY_DIAGNOSTIC "<<names[std::size_t(condition)]<<" order="<<order+1<<" target_observation="<<bool(item["target_met"])<<'\n'<<std::flush;
    }
    property(report,"trials",trials);property(report,"diagnostic_integrity",true);property(report,"acceptance","NOT_ACCEPTANCE; original-a and b required failures retained; no production fix/cause claim");
    const auto json=juce::JSON::toString(report,false);writeBlock(args.output.getChildFile("dimension-steady-cost-diagnostic.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));return 0;
}

int renderSoundQuality(const Arguments& args)
{
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","Sound-quality evidence requires Release");
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> list;format.findAllTypesForFile(list,args.plugin.getFullPathName());require(list.size()==1&&isAirInstrument(*list[0]),"Expected actual instrument bundle");
    parseArguments({"--sound-quality","--plugin",args.plugin.getFullPathName(),"--output",args.output.getFullPathName()});require(args.output.createDirectory().wasOk(),"Fresh quality output creation failed");
    if(args.wavetableWarmth)return renderWarmthListening(args,format,*list[0]);
    auto report=juce::var(new juce::DynamicObject);auto put=[](juce::var& o,const char* key,const juce::var& value){o.getDynamicObject()->setProperty(key,value);};
    const auto before=qualitySourceIdentities();const auto module=binaryIn(args.plugin);const auto moduleBefore=identity(module);put(report,"configuration",RACKSYNTH_CONFIGURATION);put(report,"source_before",before);put(report,"plugin_before",moduleBefore);put(report,"helper",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    const auto diagnostic=juce::SystemStats::getEnvironmentVariable("AIRACK_DIMENSION_COST_DIAGNOSTIC",{});
    const bool fullCostAccounting=diagnostic=="full-factory-sequence";
    if(!diagnostic.isEmpty()){require(args.factoryDimension&&(diagnostic=="external-96k-512"||diagnostic=="internal-96k-512"||fullCostAccounting),"Unknown Dimension cost diagnostic");if(diagnostic=="internal-96k-512")return dimensionSteadyCostDiagnostic(args,format,*list[0]);if(!fullCostAccounting)return dimensionCostDiagnostic(args,format,*list[0]);}
    if(fullCostAccounting)put(report,"cost_revalidation","One preregistered normal40-row factory sequence; original workload, first callback,22s tails and fixed gates unchanged. Qualified current-thread accounting brackets only four tail observations; original-a failure remains UNCLASSIFIED and this is no production-fix claim.");
    const bool higher=args.quality==airack::RenderQuality::higher,factory=args.factoryAnalog||args.factoryDigital||args.factoryReverb||args.factoryCoreFx||args.factoryEq||args.factoryFilter||args.factoryChorus||args.factoryPhaser||args.factoryFlanger||args.factoryStereo||args.factoryDimension||args.factoryFrequencyShift||args.factoryConvolution||args.factoryFrequencySplit||args.factoryMidSideSplit;
    if(args.factoryFilter){const auto production=productionQualityReference(format,*list[0],args,true);put(report,"production_reference",production);const auto referenceJson=juce::JSON::toString(production,false);writeBlock(args.output.getChildFile("production-reference-report.json"),juce::MemoryBlock(referenceJson.toRawUTF8(),std::size_t(referenceJson.getNumBytesAsUTF8())));std::cout<<"FILTER_PRODUCTION_REFERENCE complete\n"<<std::flush;}
    if(!factory&&!args.reverbOnly&&!args.coreFxProbes){const auto production=productionQualityReference(format,*list[0],args);put(report,"production_reference",production);
        const auto referenceJson=juce::JSON::toString(production,false);writeBlock(args.output.getChildFile("production-reference-report.json"),juce::MemoryBlock(referenceJson.toRawUTF8(),std::size_t(referenceJson.getNumBytesAsUTF8())));std::cout<<"QUALITY_PRODUCTION_REFERENCE complete\n"<<std::flush;}
    else if(factory){put(report,"factory_definition",args.factoryMidSideSplit?midSideSplitFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::midSideSplit)):args.factoryFrequencySplit?frequencySplitFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::frequencySplit)):args.factoryConvolution?convolutionFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::convolution)):args.factoryFrequencyShift?frequencyShiftFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::frequencyShift)):args.factoryDimension?dimensionFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::dimension)):args.factoryStereo?stereoFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::stereo)):args.factoryFlanger?flangerFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::flanger)):args.factoryPhaser?phaserFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::phaser)):args.factoryChorus?chorusFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::chorus)):args.factoryFilter?filterFactoryEvidence(placeCoreFxFactory(factorySource(false),airack::NativeModule::filter)):args.factoryEq?eqFactoryEvidence(factorySource(false)):args.factoryCoreFx?coreFxEvidence(factorySource(false)):args.factoryReverb?factoryEvidence(placeReverbFactory(airack::SoundDocument{}),reverbFactory()):factoryEvidence(factorySource(args.factoryDigital),args.factoryDigital));put(report,"raw_dsp_decision",args.factoryMidSideSplit?"Delivered MidSideSplit1 orthonormal matrix/two fixed1k prepared TPT histories/seven current-before-increment5ms ramps; independent gain/gate/tone/mix and direct neutral/Mix0 bits after histories clock; cached16P positive-pole engineering support, no mono widening/hearing claim":args.factoryFrequencySplit?"Delivered FrequencySplit1 correct complementary TPT crossover/two fixed1k branch tilts/independent gain/mute/eight current-before-increment5ms ramps; direct neutral/Mix0 bits after six-history clock; complete prepared negative-pole engineering support, no hearing claim":args.factoryConvolution?"Delivered Convolution1 prepared canonical UnitImpulse/ShortRoom bank, Fixed64 ascending-float mirrored-history immediate head with prepared one-tap Unit normal-finite shortcut/original exceptional-history fallback plus prepared NonUniform512 remainder in64-frame blocks/external64 prefix restoration/zero combined latency; TrimOFF NormaliseOFF/SRC rate gain, three5ms ramps and finite IR/predelay support; no hearing or imported-room claim":args.factoryFrequencyShift?"Delivered FrequencyShift1 exact-AGM prepared24cap double alternating allpasses/Q internal delay/three5ms ramps; common-D zeroHz coloration and current-frame signed rotation with256 resync. Actual native finite-grid measurements qualify image claims; no hearing claim":args.factoryDimension?"Delivered Dimension2 mono windowed-sinc one-write/two-read finite history, generated side and direct dry endpoints; physical4ramps5ms, no hearing claim":args.factoryStereo?"Delivered Stereo1 memoryless gain-amplitude/unity-centre-balance/existing-side width/channel-mode matrix; four5ms ramps, stage tail0, mono width limitation and direct neutral/swap endpoints, no hearing claim":args.factoryFlanger?"Delivered Flanger2 prepared windowed-sinc complete subtraction-feedback loop; shared-phase dual mono, all5 controls5ms, stationary global support with empirical moving checks, no hearing claim":args.factoryPhaser?"Delivered Phaser1 prepared stock JUCE six allpasses with complete subtraction-feedback loop; shared-LFO dual mono, active targets50ms/external centre-mix5ms, stationary tail bound with empirical moving checks, no hearing claim":args.factoryChorus?"Delivered Chorus2 prepared stock-JUCE-law shared-LFO dual mono with windowed-sinc taps; active stock targets50ms, external centre/mix5ms, no widening or sonic claim":args.factoryFilter?"Delivered Filter2 pre-drive, prepared TPT LP/BP/HP and mix at four times the rate through drive revision 2's halfbands, normalized bandpass and zero-drive identity; stationary nonlinear reference measured separately, human listening deferred":args.factoryEq?"Delivered EQ2 three matched biquads following their analog prototypes to Nyquist, with coefficient-space transition; transparent default, human listening deferred":args.factoryReverb?"Delivered Reverb2 eight-line/four-allpass candidate; fixed numerical criteria and full realtime cost passed; human listening deferred, no sonic/brand claim":"Retain revision1; current quality policy already accepted in Task3; no new kernel/sonic claim");}
    if(args.reverbOnly)put(report,"reverb_analysis_contract","Linear analysis copies: output physical gain0dB/protectionOFF, global master1; untouched generic protected auditions retained separately. Known filtered noise-note includes actual envelope/filter duration, not an ideal effect-input impulse. Fixed physical windows and dry-source/control floor precede any candidate threshold.");
    if(args.coreFxProbes){put(report,"core_fx_source",factoryEvidence(coreFxProbeBriefs()[0].dry,false));put(report,"core_fx_analysis_contract","Actual untouched Copper Arc/native1; dry unity output/master and protectionOFF are the actual compressor input. Fixed MIDI60/velocity100/hold4s and settled[2,4)s (>=10x150ms release). Twenty-one threshold points -60..0dB/3dB, ratio3/attack15ms/release150ms/makeup0/mix1; numerical instrument-input calibration, not an ideal static detector or sonic claim.");}
    if(args.factoryConvolution){put(report,"canonical_short_room_sha256",airack::detail::convolutionRoomSha256);put(report,"canonical_unit_impulse_sha256",airack::detail::convolutionUnitSha256);put(report,"convolution_policy","Prepared Fixed64 ascending-float mirrored-history immediate head with prepared one-tap Unit normal-finite shortcut/original exceptional-history fallback plus prepared NonUniform512 remainder in64-frame blocks/external64 prefix restoration/zero combined latency; TrimOFF NormaliseOFF/SRC48000-to-effective+48000/effective gain; diagonal independent histories. Unit atactualrate1frame. Predelay/amplitude/mix5ms, choice structural reset. Finite IR+maximum250ms predelay/ramp/interpolation support separate from source/Higher FIR/22s window. No hearing or imported-room claim.");}
    put(report,"selected_quality",higher?"higher":"realtime");put(report,"host_non_realtime_flag",true);put(report,"quality_evidence","Loaded host latency plus production/raw-engine sample comparison; selection/flag alone is insufficient");put(report,"algorithm_provenance",args.factoryMidSideSplit?"Actual placed Field Balance MidSideSplit1 neutral utility and ordinary roles/load; seven fields/bank14/native1, explicit same dry/processed carrier pan edits retain measurable existing Side":args.factoryFrequencySplit?"Actual placed Band Balance FrequencySplit2 neutral utility and ordinary nonneutral roles/load; eight physical fields/bank13/native2, untouched Copper Arc Synth1":args.factoryConvolution?"Actual placed Ember Room Convolution1; untouched Copper Arc Synth1; four physical fields/bank12/native1. Original canonical ShortRoom and actual-rate UnitImpulse loaded/ready before callback; no external IR import":args.factoryFrequencyShift?"Actual placed Sideband Tide FrequencyShift1, untouched Copper Arc Synth1; actual three physical fields and retained bank11 native1":args.factoryStereo?"Actual placed Centre Line Stereo1; untouched Copper Arc synth1; existing-side algebra, other retained banks explicit":args.factoryFlanger?"Actual placed Silver Loop Flanger2; Copper Arc synth1; shared-phase dual mono, other retained banks explicit":args.factoryPhaser?"Actual placed Aster Sweep Phaser1; Copper Arc synth1; shared-LFO dual mono, other retained banks explicit":args.factoryChorus?"Actual placed Orbit Ribbon Chorus2; Copper Arc synth1; shared-LFO dual mono, other retained banks explicit":args.factoryFilter?"Actual placed Veil Current Filter2; Copper Arc synth1, other retained banks explicit":args.factoryEq?"Actual placed Contour Loom EQ2; Copper Arc synth1, other retained banks explicit":args.factoryReverb?"Actual placed Lumen Drift Reverb2; dry input has no FX, other kernels remain revision1":args.factoryDimension?"Actual placed Twin Current Dimension2; untouched Copper Arc Synth1 on its revision2 voice":args.factoryCoreFx?"Actual placed Compressor2, Drive2 and Delay2 factories, one per brief; Copper Arc Synth1 on its revision2 voice":args.coreFxProbes?"Untouched Copper Arc Synth1 on its revision2 voice; the compressor set directly stays revision1":args.factoryAnalog||args.factoryDigital?juce::String("Actual placed ")+(args.factoryDigital?"Prism Relay":"Copper Arc")+" Synth1 on its revision2 voice; the drive and reverb set directly, and the load's eight master FX, stay revision1":args.reverbOnly&&args.reverbRevision==2?"Experimental Reverb2; other active kernels remain legacy revision1, explicit retained-bank metadata":"Current legacy revision1 kernels, explicit retained-bank revision metadata");put(report,"listening","NOT_RUN");put(report,"brand_comparison","UNVERIFIED");put(report,"output_gain_db",args.reverbOnly||args.coreFxProbes?0:24);put(report,"output_protection",!args.reverbOnly&&!args.coreFxProbes);put(report,"matching","Whole-file stereo RMS, same stimulus duration; separate float copies with common peak-constrained target, not perceptual LUFS equivalence");
    juce::Array<juce::var> fixtures;bool integrity=true;double maximumPartition=0,maximumRecall=0;
    auto briefs=args.factoryMidSideSplit?factoryMidSideSplitBriefs():args.factoryFrequencySplit?factoryFrequencySplitBriefs():args.factoryConvolution?factoryConvolutionBriefs():args.factoryFrequencyShift?factoryFrequencyShiftBriefs():args.factoryDimension?factoryDimensionBriefs():args.factoryStereo?factoryStereoBriefs():args.factoryFlanger?factoryFlangerBriefs():args.factoryPhaser?factoryPhaserBriefs():args.factoryChorus?factoryChorusBriefs():args.factoryFilter?factoryFilterBriefs():args.factoryEq?factoryEqBriefs():args.coreFxProbes?coreFxProbeBriefs():args.factoryCoreFx?factoryCoreFxBriefs():args.factoryReverb?factoryReverbBriefs():factory?factoryBriefs(args.factoryDigital):args.reverbOnly?reverbAnalysisBriefs(args.reverbRevision):qualityBriefs();
    if(args.reverbProbesOnly){std::erase_if(briefs,[](const QualityBrief& brief){return !brief.singleSampleNoise;});require(briefs.size()==5,"Existing mix1/control probe selection incomplete");
        put(report,"reverb_evidence_scope","Only five unchanged14s mix1 noise/control probes; original13 analysis and25 generic brief definitions preserved. Musical/default rows intentionally not captured until candidate cost is viable.");}
    if(args.reverbOnly)put(report,"reverb_algorithm_revision",args.reverbRevision);
    for(const auto& brief:briefs)for(int rate:{48000,44100,96000}){
        if(args.coreFxProbes&&rate!=48000)continue;
        if(brief.role=="factory-load"&&rate==44100)continue;
        std::array<juce::AudioBuffer<float>,2> originals;std::array<WindowMeasures,2> metrics;std::array<juce::var,2> entries;
        for(int variant=0;variant<2;++variant){auto document=variant?brief.processed:brief.dry;document.values[758]=higher?1.f:0.f;const auto name=brief.id+(variant?"-processed":"-dry")+"-"+juce::String(rate);const auto events=qualityEvents(brief,rate);const int frames=int(brief.duration*rate);
            juce::MemoryBlock owned;require(airack::encodeSoundState(document,owned),"Quality state encoding failed");const auto stateFile=args.output.getChildFile(name+".soundstate");writeBlock(stateFile,owned);
            DimensionCostTrace fullTrace;
            auto baseline=render(format,*list[0],rate,512,hostEnvelope(owned),document,airack::currentSoundStateSize,true,-1,&events,frames,brief.role=="factory-load",fullCostAccounting&&brief.role=="factory-load"?&fullTrace:nullptr);
            const auto partition=render(format,*list[0],rate,64,hostEnvelope(owned),document,airack::currentSoundStateSize,true,-1,&events,frames);
            const auto recalled=render(format,*list[0],rate,512,baseline.hostState,document,airack::currentSoundStateSize,true,-1,&events,frames);
            require(baseline.latencySamples==(higher?21:0) && partition.latencySamples==baseline.latencySamples && recalled.latencySamples==baseline.latencySamples,"Loaded quality/latency preparation mismatch");
            double nativeDifference=0;if(factory||args.reverbOnly||args.coreFxProbes){nativeDifference=difference(baseline.audio,renderFactoryNative(document,events,rate,frames,args.quality,args.factoryCoreFx));require(nativeDifference<=1e-6,"Actual selected loaded/native audio mismatch");}
            const double pd=difference(baseline.audio,partition.audio),rd=difference(baseline.audio,recalled.audio);maximumPartition=std::max(maximumPartition,pd);maximumRecall=std::max(maximumRecall,rd);integrity=integrity&&pd<=1e-6&&rd<=1e-6;
            const auto raw=args.output.getChildFile(name+"-raw-512.wav");writeWav(raw,baseline.audio,rate);writeWav(args.output.getChildFile(name+"-raw-64.wav"),partition.audio,rate);writeWav(args.output.getChildFile(name+"-recall-512.wav"),recalled.audio,rate);writeBlock(args.output.getChildFile(name+".vst3state"),baseline.hostState);
            auto tickEvents=events;const int ppq=brief.singleSampleNoise?24000:480,tempo=brief.singleSampleNoise?int(std::llround(double(ppq)*1000000/rate)):500000;
            if(!brief.singleSampleNoise)for(auto& e:tickEvents)e.sample=int(std::llround(double(e.sample)*960/rate));const auto midi=args.output.getChildFile(name+".mid");writeMidiEvents(midi,tickEvents,ppq,tempo);
            metrics[std::size_t(variant)]=measureWindow(baseline.audio,{{0,frames}});require(metrics[std::size_t(variant)].rms>1e-8,"Silent quality render");
            auto item=juce::var(new juce::DynamicObject);put(item,"id",name);put(item,"host_latency_samples",baseline.latencySamples);put(item,"role",brief.role);put(item,"stimulus",brief.stimulus);put(item,"sample_rate",rate);put(item,"frames",frames);put(item,"raw",identity(raw));put(item,"soundstate",identity(stateFile));put(item,"midi",identity(midi));put(item,"hosted_parameters",baseline.hostedParameters);put(item,"partition_difference",pd);put(item,"recall_difference",rd);put(item,"whole",windowJson(baseline.audio,"whole",{{0,frames}}));put(item,"last_second",windowJson(baseline.audio,"last_second",{{frames-rate,frames}}));
            if(factory){put(item,"factory",args.factoryMidSideSplit?midSideSplitFactoryEvidence(document):args.factoryFrequencySplit?frequencySplitFactoryEvidence(document):args.factoryConvolution?convolutionFactoryEvidence(document):args.factoryFrequencyShift?frequencyShiftFactoryEvidence(document):args.factoryDimension?dimensionFactoryEvidence(document):args.factoryStereo?stereoFactoryEvidence(document):args.factoryFlanger?flangerFactoryEvidence(document):args.factoryPhaser?phaserFactoryEvidence(document):args.factoryChorus?chorusFactoryEvidence(document):args.factoryFilter?filterFactoryEvidence(document):args.factoryEq?eqFactoryEvidence(document):args.factoryCoreFx?coreFxEvidence(document):args.factoryReverb?factoryEvidence(document,reverbFactory()):factoryEvidence(document,args.factoryDigital));put(item,"native_difference",nativeDifference);if(args.factoryCoreFx)put(item,"host_tempo_bpm",120);}
            if(args.coreFxProbes){put(item,"core_fx_source",factoryEvidence(document,false));put(item,"native_difference",nativeDifference);put(item,"settled_window_seconds","[2,4)");put(item,"threshold_db",airack::parameterPhysicalValue(brief.processed.values,*airack::rackParameterIndex(airack::NativeModule::compressor,0,"compressor.threshold_db")));}
            if(args.reverbOnly){put(item,"native_difference",nativeDifference);if(brief.singleSampleNoise){auto timebase=juce::var(new juce::DynamicObject);put(timebase,"ticks_per_quarter",ppq);put(timebase,"tempo_microseconds",tempo);put(timebase,"seconds_per_tick",double(tempo)/(1000000.0*ppq));put(timebase,"tick_duration_error_seconds",double(tempo)/(1000000.0*ppq)-1.0/rate);put(timebase,"actual_live_note_on_host_frame",0);put(timebase,"actual_live_note_off_host_frame",1);put(timebase,"provenance","MIDI file readback preserves exact integer hostframe tick positions; playback tempo is nearest integer microsecond and may differ from audio clock. Actual capture uses live native hostframe events.");put(item,"midi_timebase",timebase);}}
            if(brief.role=="factory-load"){const auto& raw=baseline.callbackNs;require(raw.size()==std::size_t((frames+511)/512),"Complete factory tail callback trace required");auto sorted=raw;std::sort(sorted.begin(),sorted.end());
                const double mean=double(std::accumulate(raw.begin(),raw.end(),juce::int64(0)))/raw.size(),p99=double(sorted[std::size_t(std::ceil(.99*raw.size()))-1]),deadline=512.*1e9/rate;int misses=0;juce::Array<juce::var> times;
                for(std::size_t n=0;n<raw.size();++n){times.add(raw[n]);const int count=std::min(512,frames-int(n)*512);if(raw[n]>double(count)*1e9/rate)++misses;}
                auto timing=jsonObject();put(timing,"callback_ns",times);put(timing,"mean_ns",mean);put(timing,"p99_ns",p99);put(timing,"max_ns",double(sorted.back()));put(timing,"deadline_ns",deadline);put(timing,"deadline_miss_count",misses);put(timing,"full_block_frames",512);put(timing,"final_callback_frames",frames%512==0?512:frames%512);put(timing,"target_met",mean<=.75*deadline&&p99<=deadline);put(timing,"scope","Uninterrupted22s loaded processBlock including initial note/release and tail; clock overhead included, scans/copies excluded; final partial-block excursions use actual frames");if(fullCostAccounting){require(fullTrace.rows.size()==raw.size(),"Complete normal factory thread trace required");for(std::size_t n=0;n<raw.size();++n)require(fullTrace.rows[n].wallNs==raw[n],"Normal factory wall/accounting trace mismatch");put(timing,"thread_accounting",fullTrace.json());}put(item,"tail_callback_timing",timing);if(!higher)integrity=integrity&&bool(timing["target_met"]);}
            double mid=0,side=0;for(int i=0;i<frames;++i){const double l=baseline.audio.getSample(0,i),r=baseline.audio.getSample(1,i);mid+=std::pow((l+r)*.5,2);side+=std::pow((l-r)*.5,2);}put(item,"mono_mid_rms",std::sqrt(mid/frames));put(item,"side_rms",std::sqrt(side/frames));if(variant==0&&brief.role=="reverb"&&brief.id.contains("chord"))require(side/frames>1e-12,"Stereo reverb input must have actual side energy");if(variant==0&&args.factoryStereo&&(brief.id.endsWith("narrow")||brief.id.endsWith("wide")))require(side/frames>1e-12,"Width role dry source must contain actual existing side energy");entries[std::size_t(variant)]=item;originals[std::size_t(variant)]=std::move(baseline.audio);
        }
        if(rate==48000){const double target=std::min({.1,.89*metrics[0].rms/metrics[0].peak,.89*metrics[1].rms/metrics[1].peak});
            for(std::size_t v=0;v<2;++v){const float gain=float(target/metrics[v].rms);auto copy=originals[v];copy.applyGain(gain);const auto path=args.output.getChildFile(brief.id+(v?"-processed":"-dry")+"-48000-listen-rms.wav");writeWav(path,copy,rate);put(entries[v],"listening_copy",identity(path));put(entries[v],"listening_gain_linear",double(gain));put(entries[v],"listening_gain_db",20*std::log10(double(gain)));put(entries[v],"listening_target_rms",target);put(entries[v],"listening_interval", "all frames, both channels");put(entries[v],"listening_metrics",windowJson(copy,"whole",{{0,copy.getNumSamples()}}));
                if(brief.role=="reverb"){for(int i=0;i<copy.getNumSamples();++i){const float mono=(copy.getSample(0,i)+copy.getSample(1,i))*.5f;copy.setSample(0,i,mono);copy.setSample(1,i,mono);}const auto mono=args.output.getChildFile(brief.id+(v?"-processed":"-dry")+"-48000-listen-mono.wav");writeWav(mono,copy,rate);put(entries[v],"mono_listening_copy",identity(mono));put(entries[v],"mono_listening_gain_policy","Same gain as stereo copy; no renormalization after M=(L+R)/2 fold");}}}
        for(auto& item:entries){put(item,"selected_quality",higher?"higher":"realtime");fixtures.add(item);}std::cout<<"QUALITY_RENDER "<<brief.id<<' '<<rate<<" raw/partition/recall complete\n"<<std::flush;
    }
    if(factory){airack::ProjectDocument full;full.sound=briefs.back().processed;full.sound.values[758]=higher?1.f:0.f;full.mode=airack::PlaybackMode::internalPattern;full.pattern.noteCount=16;
        for(int n=0;n<16;++n)full.pattern.notes[std::size_t(n)]={0,4,std::uint8_t(48+n),100,1};juce::MemoryBlock project;require(airack::encodeProjectState(full,project),"Full factory timing project encoding failed");writeBlock(args.output.getChildFile("factory-full-workload.projectstate"),project);
        juce::Array<juce::var> timings;bool targets=true;for(int rate:{48000,96000})for(int repeat=1;repeat<=3;++repeat){auto trial=hostedPatternTrial(format,*list[0],full,args.factoryMidSideSplit?"four actual Copper Arc definitions with ordinary opposing pan/16notes/eight independently nonneutral Field Balance MidSideSplit1 inserts":args.factoryFrequencySplit?"four actual Copper Arc layers/16notes/eight independently nonneutral Band Balance FrequencySplit2 inserts":args.factoryConvolution?"four actual Copper Arc layers/16notes/eight untouched Ember Room ShortRoom Convolution1 inserts":args.factoryFrequencyShift?"four actual Copper Arc layers/16notes/eight untouched Sideband Tide FrequencyShift1 inserts":args.factoryStereo?"four actual Copper Arc layers/16notes/eight actual untouched neutral Centre Line Stereo1 inserts":args.factoryFlanger?"four actual Copper Arc layers/16notes/eight actual untouched Silver Loop Flanger2 inserts (shared-phase dual mono)":args.factoryPhaser?"four actual Copper Arc layers/16notes/eight actual untouched Aster Sweep Phaser1 inserts (shared-LFO dual mono)":args.factoryChorus?"four actual Copper Arc layers/16notes/eight actual untouched Orbit Ribbon Chorus2 inserts (shared-LFO dual mono)":args.factoryFilter?"four actual Copper Arc layers/16notes/eight actual untouched Veil Current Filter2 inserts":args.factoryEq?"four actual Copper Arc layers/16notes/eight actual untouched Contour Loom EQ2 inserts":args.factoryCoreFx?"four actual Copper Arc layers/16notes/eight authored native1 compressor-drive-delay instances":args.factoryReverb?"four native layers/16notes/eight actual Reverb2 factory instances":"four actual factory layers/16notes/8FX",rate,rate==48000?128:512,repeat,true);put(trial,"deadline_policy",higher?"Offline cost; realtime target observation retained":"Unchanged realtime mean<=.75/p99<=deadline");targets=targets&&bool(trial["target_met"]);require(bool(trial["integrity"]),"Factory timing audio integrity failed");timings.add(trial);std::cout<<"FACTORY_TIMING "<<rate<<" repeat="<<repeat<<" target="<<bool(trial["target_met"])<<'\n'<<std::flush;}
        put(report,"factory_timing_trials",timings);put(report,"factory_realtime_targets_met",targets);if(!higher)integrity=integrity&&targets;
        put(report,"factory_timing_scope","Prepared actual VST3 processBlock; .25s warmup/approximately1s steady; clock overhead included, metadata/scans excluded. Uninterrupted22s raw tails are separate matrix rows, not inferred long-session timing.");}
    const auto after=qualitySourceIdentities();put(report,"source_after",after);put(report,"plugin_after",identity(module));const bool stable=juce::JSON::toString(before)==juce::JSON::toString(after)&&hash(module)==moduleBefore["sha256"].toString();integrity=integrity&&stable;put(report,"identities_unchanged",stable);put(report,"fixtures",fixtures);put(report,"maximum_partition_difference",maximumPartition);put(report,"maximum_recall_difference",maximumRecall);put(report,"success",integrity);
    const auto json=juce::JSON::toString(report,false);writeBlock(args.output.getChildFile("sound-quality-report.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));std::cout<<(integrity?"SUCCESS":"UNRESOLVED")<<" sound-quality technical rendering; listening NOT_RUN\n";return integrity?0:1;
}
void verifyCapturedTask8LegacyInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK8_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Task8 legacy input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==38,"Expected36 frozen Sound5 and2 Project4 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old input unreadable");
        airack::ProjectDocument decoded;const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();
        const bool project=int(row["project_version"])==4;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project4 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound5 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==5&&old.readInt()==759&&old.readInt()==0,"Expected exact old Sound5 domain");
        for(int record=0;record<759;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();
            require(index&&*index<759&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first759 values changed during migration");++numericChecks;}
        for(std::size_t index=759;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old source did not receive neutral appended EQ defaults");
        old.setPosition(7001);require(old.readInt()==48&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==4&&old.readShort()==0,"Frozen old four-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<4;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old FX bank changed under expanded row stride");++bankChecks;}require(banks[4]==1,"Migrated EQ bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project4 cannot roundtrip current Project5");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound5 cannot roundtrip current Sound6");}
        std::cout<<"EQ_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric759_bit_exact=1 logical_banks_exact=1\n";
    }
    require(sounds==36&&projects==2,"Incomplete actual frozen legacy input validation");
    std::cout<<"EQ_OLD_CAPTURED_INPUTS Sound5="<<sounds<<" Project4="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}


void verifyCapturedFilterOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK8_FILTER_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Filter old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound6 and2 Project5 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old831 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old831 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==5;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project5 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound6 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==6&&old.readInt()==831&&old.readInt()==0,"Expected exact old Sound6 domain");
        for(int record=0;record<831;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<831&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first831 values changed during migration");++numericChecks;}
        for(std::size_t index=831;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old831 source did not receive authored appended filter defaults");
        old.setPosition(7649);require(old.readInt()==56&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==5&&old.readShort()==0,"Frozen old five-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old831 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<5;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old five-bank FX identity changed");++bankChecks;}require(banks[5]==1,"Migrated filter bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project5 cannot roundtrip current Project6");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound6 cannot roundtrip current Sound7");}
        std::cout<<"FILTER_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric831_bit_exact=1 logical_five_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old831 input validation");
    std::cout<<"FILTER_OLD_CAPTURED_INPUTS Sound6="<<sounds<<" Project5="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedChorusOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK8_CHORUS_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Chorus old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound7 and2 Project6 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old871 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old871 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==6;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project6 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound7 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==7&&old.readInt()==871&&old.readInt()==0,"Expected exact old Sound7 domain");
        for(int record=0;record<871;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<871&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first871 values changed during migration");++numericChecks;}
        for(std::size_t index=871;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old871 source did not receive authored appended chorus defaults");
        old.setPosition(8009);require(old.readInt()==64&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==6&&old.readShort()==0,"Frozen old six-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old871 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<6;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old six-bank FX identity changed");++bankChecks;}require(banks[6]==1,"Migrated chorus bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project6 cannot roundtrip current Project7");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound7 cannot roundtrip current Sound8");}
        std::cout<<"CHORUS_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric871_bit_exact=1 logical_six_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old871 input validation");
    std::cout<<"CHORUS_OLD_CAPTURED_INPUTS Sound7="<<sounds<<" Project6="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedPhaserOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK9_PHASER_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Phaser old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound8 and2 Project7 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old911 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old911 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==7;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project7 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound8 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==8&&old.readInt()==911&&old.readInt()==0,"Expected exact old Sound8 domain");
        for(int record=0;record<911;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<911&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first911 values changed during migration");++numericChecks;}
        for(std::size_t index=911;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old911 source did not receive authored appended phaser defaults");
        old.setPosition(8369);require(old.readInt()==72&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==7&&old.readShort()==0,"Frozen old seven-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old911 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<7;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old seven-bank FX identity changed");++bankChecks;}require(banks[7]==1,"Migrated phaser bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project7 cannot roundtrip current Project8");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound8 cannot roundtrip current Sound9");}
        std::cout<<"PHASER_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric911_bit_exact=1 logical_seven_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old911 input validation");
    std::cout<<"PHASER_OLD_CAPTURED_INPUTS Sound8="<<sounds<<" Project7="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedFlangerOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK9_FLANGER_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Flanger old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound9 and2 Project8 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old951 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old951 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==8;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project8 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound9 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==9&&old.readInt()==951&&old.readInt()==0,"Expected exact old Sound9 domain");
        for(int record=0;record<951;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<951&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first951 values changed during migration");++numericChecks;}
        for(std::size_t index=951;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old951 source did not receive authored appended flanger defaults");
        old.setPosition(8729);require(old.readInt()==80&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==8&&old.readShort()==0,"Frozen old eight-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old951 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<8;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old eight-bank FX identity changed");++bankChecks;}require(banks[8]==1,"Migrated flanger bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project8 cannot roundtrip current Project9");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound9 cannot roundtrip current Sound10");}
        std::cout<<"FLANGER_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric951_bit_exact=1 logical_eight_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old951 input validation");
    std::cout<<"FLANGER_OLD_CAPTURED_INPUTS Sound9="<<sounds<<" Project8="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedStereoOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK9_STEREO_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Stereo old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound10 and2 Project9 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old991 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old991 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==9;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project9 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound10 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==10&&old.readInt()==991&&old.readInt()==0,"Expected exact old Sound10 domain");
        for(int record=0;record<991;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<991&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first991 values changed during migration");++numericChecks;}
        for(std::size_t index=991;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old991 source did not receive authored appended stereo defaults");
        old.setPosition(9089);require(old.readInt()==88&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==9&&old.readShort()==0,"Frozen old nine-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old991 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<9;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old nine-bank FX identity changed");++bankChecks;}require(banks[9]==1,"Migrated stereo bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project9 cannot roundtrip current Project10");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound10 cannot roundtrip current Sound11");}
        std::cout<<"STEREO_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric991_bit_exact=1 logical_nine_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old991 input validation");
    std::cout<<"STEREO_OLD_CAPTURED_INPUTS Sound10="<<sounds<<" Project9="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedDimensionOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK9_DIMENSION_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Dimension old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound11 and2 Project10 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old1023 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old1023 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==10;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project10 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound11 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==11&&old.readInt()==1023&&old.readInt()==0,"Expected exact old Sound11 domain");
        for(int record=0;record<1023;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<1023&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first1023 values changed during migration");++numericChecks;}
        for(std::size_t index=1023;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old1023 source did not receive authored appended dimension defaults");
        old.setPosition(9377);require(old.readInt()==96&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==10&&old.readShort()==0,"Frozen old ten-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old1023 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<10;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old ten-bank FX identity changed");++bankChecks;}require(banks[10]==1,"Migrated dimension bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project10 cannot roundtrip current Project11");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound11 cannot roundtrip current Sound12");}
        std::cout<<"DIMENSION_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric1023_bit_exact=1 logical_ten_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old1023 input validation");
    std::cout<<"DIMENSION_OLD_CAPTURED_INPUTS Sound11="<<sounds<<" Project10="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedFrequencyShiftOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK10_FREQUENCY_SHIFT_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"FrequencyShift old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound12 and2 Project11 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old1055 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old1055 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==11;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project11 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound12 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==12&&old.readInt()==1055&&old.readInt()==0,"Expected exact old Sound12 domain");
        for(int record=0;record<1055;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<1055&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first1055 values changed during migration");++numericChecks;}
        for(std::size_t index=1055;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old1023 source did not receive authored appended frequency-shift defaults");
        old.setPosition(9665);require(old.readInt()==104&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==11&&old.readShort()==0,"Frozen old eleven-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old1023 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<11;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old eleven-bank FX identity changed");++bankChecks;}require(banks[11]==1,"Migrated frequency-shift bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project11 cannot roundtrip current Project12");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound12 cannot roundtrip current Sound13");}
        std::cout<<"FREQUENCY_SHIFT_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric1055_bit_exact=1 logical_eleven_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old1055 input validation");
    std::cout<<"FREQUENCY_SHIFT_OLD_CAPTURED_INPUTS Sound12="<<sounds<<" Project11="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

void verifyCapturedMidSideSplitOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK10_MID_SIDE_SPLIT_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Mid side split old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound15 and2 Project14 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old1175 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old1175 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==14;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project14 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound15 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==15&&old.readInt()==1175&&old.readInt()==0,"Expected exact old Sound15 domain");
        for(int record=0;record<1175;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<1175&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first1175 values changed during migration");++numericChecks;}
        for(std::size_t index=1175;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old1175 source did not receive authored appended mid side split defaults");
        old.setPosition(10745);require(old.readInt()==128&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==14&&old.readShort()==0,"Frozen old fourteen-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old1175 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<14;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old fourteen-bank FX identity changed");++bankChecks;}require(banks[14]==1,"Migrated mid side split bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project14 cannot roundtrip current Project15");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound15 cannot roundtrip current Sound16");}
        std::cout<<"MID_SIDE_SPLIT_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric1175_bit_exact=1 logical_fourteen_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old1175 input validation");
    std::cout<<"MID_SIDE_SPLIT_OLD_CAPTURED_INPUTS Sound15="<<sounds<<" Project14="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}
void verifyCapturedFrequencySplitOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK10_FREQUENCY_SPLIT_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Frequency split old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound14 and2 Project13 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old1111 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old1111 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==13;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project13 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound14 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==14&&old.readInt()==1111&&old.readInt()==0,"Expected exact old Sound14 domain");
        for(int record=0;record<1111;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<1111&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first1111 values changed during migration");++numericChecks;}
        for(std::size_t index=1111;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old1111 source did not receive authored appended frequency split defaults");
        old.setPosition(10169);require(old.readInt()==120&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==13&&old.readShort()==0,"Frozen old thirteen-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old1111 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<13;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old thirteen-bank FX identity changed");++bankChecks;}require(banks[13]==1,"Migrated frequency split bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project13 cannot roundtrip current Project14");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound14 cannot roundtrip current Sound15");}
        std::cout<<"FREQUENCY_SPLIT_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric1111_bit_exact=1 logical_thirteen_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old1111 input validation");
    std::cout<<"FREQUENCY_SPLIT_OLD_CAPTURED_INPUTS Sound14="<<sounds<<" Project13="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}
void verifyCapturedConvolutionOldInputs()
{
    const auto path=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK10_CONVOLUTION_OLD_INPUTS",{});if(path.isEmpty())return;
    const juce::File manifest(path);require(manifest.existsAsFile()&&manifest.getSize()<=256*1024,"Convolution old input manifest missing/bounded");
    const auto inputs=juce::JSON::parse(manifest.loadFileAsString());require(inputs.isArray()&&inputs.size()==82,"Expected80 frozen Sound13 and2 Project12 inputs");
    int sounds=0,projects=0,numericChecks=0,bankChecks=0;const auto defaults=airack::defaultParameterValues();
    for(const auto& row:*inputs.getArray()){
        const juce::File file(row["copy"].toString());require(file.existsAsFile()&&file.getSize()==juce::int64(row["bytes"])&&hash(file).equalsIgnoreCase(row["sha256"].toString()),"Frozen old1079 input identity differs");
        juce::MemoryBlock bytes;require(file.loadFileAsData(bytes),"Frozen old1079 input unreadable");airack::ProjectDocument decoded;
        const char* raw=static_cast<const char*>(bytes.getData());std::size_t soundSize=bytes.getSize();const bool project=int(row["project_version"])==12;
        if(project){require(airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded),"Actual captured Project12 migration rejected");juce::MemoryInputStream header(raw,bytes.getSize(),false);header.setPosition(12);soundSize=std::size_t(header.readInt());raw+=20;++projects;}
        else{require(airack::decodeSoundState(bytes.getData(),bytes.getSize(),decoded.sound),"Actual captured Sound13 migration rejected");++sounds;}
        juce::MemoryInputStream old(raw,soundSize,false);require(old.readInt()==0x49535241&&old.readInt()==13&&old.readInt()==1079&&old.readInt()==0,"Expected exact old Sound13 domain");
        for(int record=0;record<1079;++record){const auto index=airack::parameterIndexForKey(static_cast<std::uint32_t>(old.readInt()));const auto value=old.readFloat();require(index&&*index<1079&&std::bit_cast<std::uint32_t>(decoded.sound.values[*index])==std::bit_cast<std::uint32_t>(value),"Frozen first1079 values changed during migration");++numericChecks;}
        for(std::size_t index=1079;index<airack::parameterCount;++index)require(decoded.sound.values[index]==defaults[index],"Old1079 source did not receive authored appended convolution defaults");
        old.setPosition(9881);require(old.readInt()==112&&old.readShort()==1&&old.readByte()==4&&old.readByte()==2&&old.readByte()==8&&old.readByte()==12&&old.readShort()==0,"Frozen old twelve-bank geometry differs");
        for(const auto& banks:decoded.sound.compatibility.synth)for(auto revision:banks){require(static_cast<unsigned char>(old.readByte())==revision,"Old1079 synth revision changed");++bankChecks;}
        for(const auto& banks:decoded.sound.compatibility.effects){for(std::size_t bank=0;bank<12;++bank){require(static_cast<unsigned char>(old.readByte())==banks[bank],"Logical old twelve-bank FX identity changed");++bankChecks;}require(banks[12]==1,"Migrated convolution bank must default to native1");}
        juce::MemoryBlock current;if(project){airack::ProjectDocument recalled;require(airack::encodeProjectState(decoded,current)&&airack::decodeProjectState(current.getData(),current.getSize(),recalled)&&recalled==decoded,"Captured Project12 cannot roundtrip current Project13");}
        else{airack::SoundDocument recalled;require(airack::encodeSoundState(decoded.sound,current)&&airack::decodeSoundState(current.getData(),current.getSize(),recalled)&&recalled==decoded.sound,"Captured Sound13 cannot roundtrip current Sound14");}
        std::cout<<"CONVOLUTION_OLD_INPUT "<<file.getFileName()<<" sha256="<<hash(file)<<" numeric1079_bit_exact=1 logical_twelve_banks_exact=1\n";
    }
    require(sounds==80&&projects==2,"Incomplete actual frozen old1079 input validation");
    std::cout<<"CONVOLUTION_OLD_CAPTURED_INPUTS Sound13="<<sounds<<" Project12="<<projects<<" numeric_bit_checks="<<numericChecks<<" logical_revision_checks="<<bankChecks<<'\n';
}

// Plays an AI brief's notes at its tempo into the brief's source sound (dry) and the AI's returned sound (processed).
int renderAiBrief(const AiBriefArguments& args)
{
    require(juce::String(RACKSYNTH_CONFIGURATION)=="Release","AI brief listening renders require Release");
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> list;format.findAllTypesForFile(list,args.host.plugin.getFullPathName());require(list.size()==1&&isAirInstrument(*list[0]),"Expected actual instrument bundle");
    const auto input=[&](const char* name){const auto file=args.input.getChildFile(name);require(file.existsAsFile(),"AI brief probe output incomplete");return file;};
    const auto requestFile=input("request.json"),proofFile=input("proof.json"),sourceFile=input("source.airackproject"),candidateFile=input("candidate.airackproject");
    const auto request=juce::JSON::parse(requestFile.loadFileAsString());const auto* brief=airack::listening::aiBrief(request["brief"].toString().toStdString());require(brief!=nullptr,"Probe output is not an AI brief's");
    juce::MemoryBlock sourceBytes,candidateBytes;airack::ProjectDocument source,candidate;
    require(sourceFile.loadFileAsData(sourceBytes)&&airack::decodeProjectState(sourceBytes.getData(),sourceBytes.getSize(),source)&&source==airack::listening::aiBriefSource(*brief),"Probe source is not the brief's");
    require(candidateFile.loadFileAsData(candidateBytes)&&airack::decodeProjectState(candidateBytes.getData(),candidateBytes.getSize(),candidate)&&airack::validateProjectDocument(candidate)&&candidate.pattern==source.pattern&&candidate.mode==source.mode,"Candidate is not a sound-scope answer to the brief");
    const auto proof=juce::JSON::parse(proofFile.loadFileAsString());require(bool(proof["success"]),"Probe run did not succeed");
    require(args.host.output.createDirectory().wasOk(),"Fresh AI brief output creation failed");
    const auto module=binaryIn(args.host.plugin);const auto moduleBefore=identity(module);
    const bool higher=args.quality==airack::RenderQuality::higher;const int rate=48000,frames=int(brief->seconds*rate);
    const auto events=aiBriefEvents(*brief,60.0*rate/brief->bpm);const auto ticks=aiBriefEvents(*brief,480);const int tempo=int(std::lround(60e6/brief->bpm));
    const auto stem="brief-"+juce::String(brief->id).toLowerCase();
    std::array<juce::AudioBuffer<float>,2> audio;std::array<WindowMeasures,2> metrics;std::array<Json,2> entries;double maximumPartition=0,maximumRecall=0;bool integrity=true;
    for(std::size_t variant=0;variant<2;++variant){
        auto document=(variant?candidate:source).sound;document.values[758]=higher?1.f:0.f;const auto name=stem+(variant?"-processed":"-dry")+"-48000";
        juce::MemoryBlock owned;require(airack::encodeSoundState(document,owned),"AI brief state encoding failed");const auto stateFile=args.host.output.getChildFile(name+".soundstate");writeBlock(stateFile,owned);
        auto baseline=render(format,*list[0],rate,512,hostEnvelope(owned),document,airack::currentSoundStateSize,true,-1,&events,frames,false,nullptr,brief->bpm);
        const auto partition=render(format,*list[0],rate,64,hostEnvelope(owned),document,airack::currentSoundStateSize,true,-1,&events,frames,false,nullptr,brief->bpm);
        const auto recalled=render(format,*list[0],rate,512,baseline.hostState,document,airack::currentSoundStateSize,true,-1,&events,frames,false,nullptr,brief->bpm);
        require(baseline.latencySamples==(higher?21:0)&&partition.latencySamples==baseline.latencySamples&&recalled.latencySamples==baseline.latencySamples,"Loaded quality/latency preparation mismatch");
        const double pd=difference(baseline.audio,partition.audio),rd=difference(baseline.audio,recalled.audio);maximumPartition=std::max(maximumPartition,pd);maximumRecall=std::max(maximumRecall,rd);integrity=integrity&&pd<=1e-6&&rd<=1e-6;
        const auto raw=args.host.output.getChildFile(name+"-raw-512.wav"),raw64=args.host.output.getChildFile(name+"-raw-64.wav"),recall=args.host.output.getChildFile(name+"-recall-512.wav");
        writeWav(raw,baseline.audio,rate);writeWav(raw64,partition.audio,rate);writeWav(recall,recalled.audio,rate);
        const auto hostState=args.host.output.getChildFile(name+".vst3state");writeBlock(hostState,baseline.hostState);
        const auto midi=args.host.output.getChildFile(name+".mid");writeMidiEvents(midi,ticks,480,tempo);
        metrics[variant]=measureWindow(baseline.audio,{{0,frames}});require(metrics[variant].rms>1e-8,"Silent AI brief render");
        double mid=0,side=0;for(int i=0;i<frames;++i){const double l=baseline.audio.getSample(0,i),r=baseline.audio.getSample(1,i);mid+=std::pow((l+r)*.5,2);side+=std::pow((l-r)*.5,2);}
        auto item=jsonObject();property(item,"id",name);property(item,"variant",variant?"processed":"dry");property(item,"sample_rate",rate);property(item,"frames",frames);
        property(item,"output_gain_db",airack::parameterPhysicalValue(document.values,756));property(item,"output_protection",document.values[757]==1);
        property(item,"raw",identity(raw));property(item,"raw_64",identity(raw64));property(item,"recall",identity(recall));property(item,"soundstate",identity(stateFile));property(item,"vst3state",identity(hostState));property(item,"midi",identity(midi));
        property(item,"host_latency_samples",baseline.latencySamples);property(item,"hosted_parameters",baseline.hostedParameters);property(item,"partition_difference",pd);property(item,"recall_difference",rd);
        property(item,"whole",windowJson(baseline.audio,"whole",{{0,frames}}));property(item,"mono_mid_rms",std::sqrt(mid/frames));property(item,"side_rms",std::sqrt(side/frames));
        entries[variant]=item;audio[variant]=std::move(baseline.audio);
    }
    const double target=std::min({.1,.89*metrics[0].rms/metrics[0].peak,.89*metrics[1].rms/metrics[1].peak});
    for(std::size_t v=0;v<2;++v){const float gain=float(target/metrics[v].rms);auto copy=audio[v];copy.applyGain(gain);const auto name=stem+(v?"-processed":"-dry")+"-48000";
        const auto path=args.host.output.getChildFile(name+"-listen-rms.wav");writeWav(path,copy,rate);property(entries[v],"listening_copy",identity(path));property(entries[v],"listening_gain_db",20*std::log10(double(gain)));property(entries[v],"listening_target_rms",target);
        for(int i=0;i<copy.getNumSamples();++i){const float mono=(copy.getSample(0,i)+copy.getSample(1,i))*.5f;copy.setSample(0,i,mono);copy.setSample(1,i,mono);}
        const auto mono=args.host.output.getChildFile(name+"-listen-mono.wav");writeWav(mono,copy,rate);property(entries[v],"mono_listening_copy",identity(mono));}
    juce::Array<juce::var> notes;for(std::size_t n=0;n<brief->phrase.noteCount;++n){const auto& note=brief->phrase.notes[n];auto row=jsonObject();
        property(row,"start_beats",note.startBeats);property(row,"duration_beats",note.durationBeats);property(row,"pitch",int(note.pitch));property(row,"velocity",int(note.velocity));property(row,"channel",int(note.channel));notes.add(row);}
    auto described=jsonObject();property(described,"id",juce::String(brief->id));property(described,"instruction",juce::String(brief->instruction));property(described,"role",juce::String(brief->role));
    property(described,"key",juce::String(brief->key));property(described,"scale",juce::String(brief->scale));property(described,"bpm",brief->bpm);property(described,"seconds",brief->seconds);property(described,"notes",notes);
    auto answer=jsonObject();for(const auto* key:{"synthetic","selectedModel","inferenceCalls","explanation","characteristics","success"})property(answer,key,proof[key]);
    juce::Array<juce::var> inputs;for(const auto& file:{requestFile,proofFile,sourceFile,candidateFile})inputs.add(identity(file));
    const auto moduleAfter=identity(module);const bool stable=moduleAfter["sha256"].toString()==moduleBefore["sha256"].toString();integrity=integrity&&stable;
    juce::Array<juce::var> fixtures;for(const auto& entry:entries)fixtures.add(entry);
    auto report=jsonObject();property(report,"configuration",RACKSYNTH_CONFIGURATION);property(report,"brief",described);property(report,"quality",higher?"higher":"realtime");property(report,"listening","NOT_RUN");
    property(report,"plugin_before",moduleBefore);property(report,"plugin_after",moduleAfter);property(report,"helper",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    property(report,"input",args.input.getFullPathName());property(report,"inputs",inputs);property(report,"proof",answer);property(report,"changes",aiBriefChanges(source.sound,candidate.sound));property(report,"host_tempo_bpm",brief->bpm);
    property(report,"matching","Whole-file stereo RMS, same stimulus duration; separate float copies with common peak-constrained target, not perceptual LUFS equivalence; mono folds M=(L+R)/2 at the stereo copy's gain");
    property(report,"fixtures",fixtures);property(report,"maximum_partition_difference",maximumPartition);property(report,"maximum_recall_difference",maximumRecall);property(report,"identities_unchanged",stable);property(report,"success",integrity);
    const auto json=juce::JSON::toString(report,false);writeBlock(args.host.output.getChildFile("ai-brief-report.json"),juce::MemoryBlock(json.toRawUTF8(),std::size_t(json.getNumBytesAsUTF8())));
    std::cout<<(integrity?"SUCCESS":"UNRESOLVED")<<" AI brief "<<brief->id<<" rendering; listening NOT_RUN\n";return integrity?0:1;
}
// Actual-bundle Space regression and listening packet. Reuses the existing host and raw writers.
Arguments parseSpaceReverbArguments(juce::StringArray args)
{
    require(args.size()==5&&args[0]=="--space-reverb"&&args[1]=="--plugin"&&args[3]=="--output",
        "Usage: RackSynthRender --space-reverb --plugin ABSOLUTE_VST3_BUNDLE --output FRESH_ABSOLUTE_WORKSPACE_DIRECTORY");
    args.remove(0);auto parsed=parseArguments(args);
    require(!parsed.output.exists()&&!fs::exists(fs::symlink_status(fs::path(parsed.output.getFullPathName().toWideCharPointer()))),
        "Space Reverb output must be absent, including links");
    return parsed;
}

struct SpaceRenderScene
{
    juce::String id,description;
    airack::SoundDocument dry,processed;
    std::vector<int> notes{60};
    std::vector<double> starts{0};
    double hold=.15,seconds=8;
    std::uint8_t slot=airack::noSlot;
};

std::size_t spaceRenderIndex(std::uint8_t slot,const juce::String& key)
{
    const auto index=airack::rackParameterIndex(airack::NativeModule::spaceReverb,slot,"space_reverb."+key);
    require(index.has_value(),"Space Reverb fixture control is not registered");return *index;
}
void setSpaceRenderValue(airack::SoundDocument& document,std::uint8_t slot,const juce::String& key,float physical)
{
    const auto index=spaceRenderIndex(slot,key);const auto& d=airack::parameterRegistry()[index];
    require(std::isfinite(physical)&&physical>=d.minimum&&physical<=d.maximum,"Space Reverb fixture value outside registered range");
    document.values[index]=juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(physical);
}
airack::RackDesign spaceRenderFactory()
{
    const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.name=="Lumen Field";});
    require(found!=designs.end()&&found->revision==1&&found->role==airack::RackRole::fx&&found->units.size()==1
        &&found->units[0].module==airack::NativeModule::spaceReverb&&found->units[0].algorithmRevision==1,
        "Actual original Lumen Field factory is missing or changed");
    return *found;
}
SpaceRenderScene makeSpaceRenderScene(const char* id,const char* description)
{
    SpaceRenderScene scene;scene.id=id;scene.description=description;scene.dry=airack::copperArcSound();
    scene.dry.values=fixtureValues(false);
    setGainValue(scene.dry,"global.master_level",.8f);setGainValue(scene.dry,"synth.1.level",.5f);
    setGainValue(scene.dry,"output.gain_db",0);setGainValue(scene.dry,"output.protection",0);
    setGainValue(scene.dry,"synth.1.analog.filter.cutoff",9000);
    airack::RackOperation operation;operation.design=spaceRenderFactory();
    operation.newInstance=juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f1");
    operation.destination={airack::RackRole::fx,airack::masterChain,0};airack::RackDifference delta;
    require(airack::makeRackCandidate(scene.dry,operation,airack::SoundEditOrigin::manual,scene.processed,delta)==airack::RackCandidateStatus::ready,
        "Ordinary Lumen Field rack placement failed");
    require(scene.processed.personalities!=nullptr,"Lumen Field placement has no retained rack");
    const auto found=std::find_if(scene.processed.personalities->instances.begin(),scene.processed.personalities->instances.end(),
        [&](const auto& instance){return instance.id==operation.newInstance;});
    require(found!=scene.processed.personalities->instances.end(),"Placed Lumen Field instance is missing");
    const auto* binding=airack::rackUnitBinding(*found,found->design.units[0].id);require(binding!=nullptr,"Lumen Field actual binding is missing");scene.slot=binding->slot;
    return scene;
}
std::vector<SpaceRenderScene> spaceRenderScenes()
{
    std::vector<SpaceRenderScene> scenes;
    auto pluck=makeSpaceRenderScene("pluck","Three deterministic analog plucks through the untouched Lumen Field settings");pluck.starts={0,.5,1};pluck.hold=.12;scenes.push_back(pluck);
    auto pad=makeSpaceRenderScene("pad","Sustained deterministic three-note pad through Lumen Field");pad.notes={48,55,60};pad.hold=4;pad.seconds=12;
    for(auto* d:{&pad.dry,&pad.processed}){setGainValue(*d,"synth.1.analog.amp.attack",.12f);setGainValue(*d,"synth.1.analog.amp.decay",.6f);setGainValue(*d,"synth.1.analog.amp.sustain",.65f);setGainValue(*d,"synth.1.analog.amp.release",.8f);}scenes.push_back(pad);
    auto room=makeSpaceRenderScene("short-room","Shorter decay and close early reflections; one original space family");room.starts={0,.35,.7,1.05};room.hold=.1;room.seconds=6;
    for(const auto& [key,value]:std::initializer_list<std::pair<const char*,float>>{{"size",.2f},{"decay_seconds",.65f},{"predelay_ms",8.f},{"distance",.25f},{"character",.1f},{"mix",.32f}})setSpaceRenderValue(room.processed,room.slot,key,value);scenes.push_back(room);
    auto hall=makeSpaceRenderScene("long-hall","Longer original stereo hall with finite captured tail, no tail-bound inference");hall.notes={48,55,60};hall.hold=1.5;hall.seconds=18;
    for(const auto& [key,value]:std::initializer_list<std::pair<const char*,float>>{{"size",.85f},{"decay_seconds",6.f},{"predelay_ms",35.f},{"brightness",.65f},{"distance",.8f},{"mix",.4f}})setSpaceRenderValue(hall.processed,hall.slot,key,value);
    for(auto* d:{&hall.dry,&hall.processed})setGainValue(*d,"synth.1.analog.amp.sustain",.65f);scenes.push_back(hall);
    auto shaped=makeSpaceRenderScene("shaped-decay","Low shelf lengthens low decay; high shelf and mid bell shorten it; separate wet EQ cuts upper mid amplitude");shaped.starts={0,.75,1.5};shaped.hold=.2;shaped.seconds=14;
    for(const auto& [key,value]:std::initializer_list<std::pair<const char*,float>>{{"decay_seconds",4.f},{"brightness",1.f},{"mix",.55f},
        {"decay.1.active",1.f},{"decay.1.bypassed",0.f},{"decay.1.shape",1.f},{"decay.1.freq_hz",250.f},{"decay.1.multiplier",1.7f},{"decay.1.q",.8f},
        {"decay.2.active",1.f},{"decay.2.shape",2.f},{"decay.2.freq_hz",4000.f},{"decay.2.multiplier",.35f},{"decay.2.q",1.f},
        {"decay.3.active",1.f},{"decay.3.shape",0.f},{"decay.3.freq_hz",1200.f},{"decay.3.multiplier",.55f},{"decay.3.q",1.2f},
        {"post.2.active",1.f},{"post.2.shape",0.f},{"post.2.freq_hz",3200.f},{"post.2.gain_db",-4.f},{"post.2.bell_q",1.2f}})setSpaceRenderValue(shaped.processed,shaped.slot,key,value);scenes.push_back(shaped);
    auto duck=makeSpaceRenderScene("ducking","Three sustained chord bursts reduce the final wet branch, with release recovery between bursts");duck.notes={48,55,60};duck.starts={0,2,4};duck.hold=.8;duck.seconds=14;
    for(const auto& [key,value]:std::initializer_list<std::pair<const char*,float>>{{"decay_seconds",4.f},{"predelay_ms",10.f},{"mix",.7f},{"duck_range_db",12.f},{"duck_threshold_db",-42.f},{"duck_attack_ms",3.f},{"duck_release_ms",350.f}})setSpaceRenderValue(duck.processed,duck.slot,key,value);
    for(auto* d:{&duck.dry,&duck.processed}){setGainValue(*d,"synth.1.analog.amp.attack",.02f);setGainValue(*d,"synth.1.analog.amp.sustain",.7f);setGainValue(*d,"synth.1.analog.amp.release",.08f);}scenes.push_back(duck);
    require(scenes.size()==6,"Space Reverb six-scene packet incomplete");
    for(const auto& scene:scenes)require(airack::validateSoundDocument(scene.dry)&&airack::validateSoundDocument(scene.processed),"Invalid retained Space Reverb scene");return scenes;
}
std::vector<Event> spaceRenderEvents(const SpaceRenderScene& scene,int rate,bool technical=false)
{
    const double scale=technical?std::min(1.,1.5/(scene.starts.back()+scene.hold)):1.;
    std::vector<Event> events;for(const auto start:scene.starts)for(const auto pitch:scene.notes){events.push_back({int(std::llround(start*rate*scale)),juce::MidiMessage::noteOn(1,pitch,juce::uint8(100))});events.push_back({int(std::llround((start+scene.hold)*rate*scale)),juce::MidiMessage::noteOff(1,pitch)});}
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.sample<b.sample;});return events;
}
Json spaceRenderAudio(const Rendered& result,int rate,const char* silent="Space Reverb raw render is silent or negligible")
{
    const auto count=result.audio.getNumSamples();const auto m=measureWindow(result.audio,{{0,count}});
    require(m.peak>0&&m.rms>1.e-10,silent);
    auto item=jsonObject();property(item,"whole",windowJson(result.audio,"whole",{{0,count}}));property(item,"last_second",windowJson(result.audio,"last_second",{{count-rate,count}}));
    juce::Array<Json> hashes;for(int ch=0;ch<2;++ch)hashes.add(juce::SHA256(result.audio.getReadPointer(ch),std::size_t(count)*sizeof(float)).toHexString());
    property(item,"channel_float32_sha256",hashes);property(item,"host_latency_samples",result.latencySamples);property(item,"hosted_parameters",result.hostedParameters);return item;
}
Json spaceRenderParameters(const airack::SoundDocument& sound,std::uint8_t slot)
{
    juce::Array<Json> fields;
    for(const auto index:airack::unitReach(airack::NativeModule::spaceReverb,slot)){const auto& d=airack::parameterRegistry()[index];auto item=jsonObject();property(item,"id",d.id);property(item,"state_key",juce::int64(d.stateKey));property(item,"normalized",sound.values[index]);property(item,"physical",airack::parameterPhysicalValue(sound.values,index));fields.add(item);}
    require(fields.size()==97,"Space Reverb actual binding must expose all96 fields and the existing bypass");
    return fields;
}
Json spaceRenderSources()
{
    const auto root=juce::File(RACKSYNTH_WORKSPACE);auto files=root.getChildFile("src").findChildFiles(juce::File::findFiles,true,"*");
    for(int index=files.size();--index>=0;)if(!files[index].hasFileExtension("h")&&!files[index].hasFileExtension("cpp"))files.remove(index);
    files.add(root.getChildFile("CMakeLists.txt"));files.add(root.getChildFile("tools/native/RenderFixtures.cpp"));
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.getFullPathName()<b.getFullPathName();});
    juce::Array<Json> sources;for(const auto& file:files)sources.add(identity(file));return sources;
}
int renderSpaceReverb(const Arguments& args)
{
    // Recheck the same fresh-path policy after plugin discovery, before making any output.
    parseSpaceReverbArguments({"--space-reverb","--plugin",args.plugin.getFullPathName(),"--output",args.output.getFullPathName()});
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;format.findAllTypesForFile(descriptions,args.plugin.getFullPathName());
    require(descriptions.size()==1&&isAirInstrument(*descriptions[0]),"Space Reverb bundle must contain the expected instrument");
    parseSpaceReverbArguments({"--space-reverb","--plugin",args.plugin.getFullPathName(),"--output",args.output.getFullPathName()});
    require(args.output.createDirectory().wasOk(),"Space Reverb output creation failed");
    auto report=jsonObject();juce::Array<Json> rows,listening;
    property(report,"configuration",RACKSYNTH_CONFIGURATION);property(report,"scope","Actual-bundle static audio/host-ID/state/latency regression and raw listening packet; no callback capacity or listening acceptance");
    property(report,"module_before",identity(binaryIn(args.plugin)));property(report,"renderer_before",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));property(report,"sources_before",spaceRenderSources());
    property(report,"initial_state_before_prepare",true);property(report,"static_settings_no_automation",true);property(report,"cpu_capacity_acceptance",false);property(report,"listening_verdict","NOT_RUN");
    property(report,"float_wav_format","stereo IEEE float32, raw product output gain0dB/protectionOFF, no normalization or gain matching");property(report,"listening_quality","realtime, 48000 Hz");
    property(report,"technical_seconds_per_row",2);property(report,"technical_event_policy","Scale the complete scene phrase only when needed so its final noteOff is at1.5s; raw listening phrases retain their original timing and full6–18s captures");
    property(report,"no_space_reference_origin","The explicit Copper Arc/fixtureValues carrier retained before ordinary Lumen Field placement; no Space personality or layout slot is present. New product instances remain empty.");
    property(report,"partition_and_recall_limit",0);property(report,"dry_mix0_limit",0);property(report,"added_host_latency_limit_samples",0);
    const auto design=spaceRenderFactory();juce::var definition;require(airack::encodeRackDesignObject(design,definition,airack::RackValidationMode::portableDesign),"Space factory definition encoding failed");property(report,"factory_definition",definition);
    const auto finish=[&](bool success,const juce::String& error){property(report,"success",success);property(report,"error",error);property(report,"fixtures",rows);property(report,"listening_packet",listening);property(report,"completed_rows",rows.size());property(report,"expected_rows",36);property(report,"fresh_host_instances_per_row",6);property(report,"additional_listening_instances_expected",12);property(report,"completed_listening_pairs",listening.size());writePatternJson(args.output.getChildFile("space-reverb-report.json"),report);};
    try{
        const auto scenes=spaceRenderScenes();
        for(const auto& scene:scenes)for(const int rate:{44100,48000,96000})for(const bool higher:{false,true}){
            const auto stem=scene.id+"-"+(higher?"higher":"realtime")+"-"+juce::String(rate);auto document=scene.processed,dry=scene.dry;
            for(auto* d:{&document,&dry})setGainValue(*d,"output.render_quality",higher?1.f:0.f);
            auto zero=document;setSpaceRenderValue(zero,scene.slot,"mix",0);const auto events=spaceRenderEvents(scene,rate,true);const int frames=2*rate;
            juce::MemoryBlock owned,dryOwned,zeroOwned;require(airack::encodeSoundState(document,owned)&&airack::encodeSoundState(dry,dryOwned)&&airack::encodeSoundState(zero,zeroOwned),"Space scene sound encoding failed");
            const auto envelope=hostEnvelope(owned);
            const auto baseline=render(format,*descriptions[0],rate,64,envelope,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto partition512=render(format,*descriptions[0],rate,512,envelope,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto recalled=render(format,*descriptions[0],rate,64,baseline.hostState,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto partition17=render(format,*descriptions[0],rate,17,envelope,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto reference=render(format,*descriptions[0],rate,64,hostEnvelope(dryOwned),dry,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto mixZero=render(format,*descriptions[0],rate,64,hostEnvelope(zeroOwned),zero,airack::currentSoundStateSize,false,-1,&events,frames);
            auto row=jsonObject();property(row,"id",stem);property(row,"description",scene.description);property(row,"sample_rate",rate);property(row,"quality",higher?"higher":"realtime");property(row,"frames",frames);property(row,"actual_bound_slot",int(scene.slot));property(row,"retained_space_parameters",spaceRenderParameters(document,scene.slot));
            property(row,"fresh64",spaceRenderAudio(baseline,rate));property(row,"fresh512",spaceRenderAudio(partition512,rate));property(row,"fresh17",spaceRenderAudio(partition17,rate));property(row,"third_instance_opaque_recall64",spaceRenderAudio(recalled,rate));property(row,"no_space_dry",spaceRenderAudio(reference,rate));property(row,"placed_mix0",spaceRenderAudio(mixZero,rate));
            const double pd512=difference(baseline.audio,partition512.audio),pd17=difference(baseline.audio,partition17.audio),rd=difference(baseline.audio,recalled.audio),zd=difference(reference.audio,mixZero.audio),wetDifference=difference(reference.audio,baseline.audio);
            property(row,"partition512_max_abs_difference",pd512);property(row,"partition17_max_abs_difference",pd17);property(row,"opaque_recall_max_abs_difference",rd);property(row,"dry_mix0_max_abs_difference",zd);property(row,"processed_dry_max_abs_difference",wetDifference);property(row,"added_latency_samples",baseline.latencySamples-reference.latencySamples);
            const double mix=airack::parameterPhysicalValue(document.values,spaceRenderIndex(scene.slot,"mix"));double wetSquares=0;
            for(int ch=0;ch<2;++ch)for(int sample=0;sample<frames;++sample){const double residual=baseline.audio.getSample(ch,sample)-(1-mix)*reference.audio.getSample(ch,sample);wetSquares+=residual*residual;}
            const double wetResidualRms=std::sqrt(wetSquares/(2.*frames));property(row,"wet_residual_rms",wetResidualRms);property(row,"wet_residual_scope","Numerical residual after subtracting the declared static linear dry contribution; hearing remains a separate verdict");
            const auto stateFile=args.output.getChildFile(stem+".soundstate"),opaqueFile=args.output.getChildFile(stem+".hoststate"),dryFile=args.output.getChildFile(stem+"-dry.soundstate"),zeroFile=args.output.getChildFile(stem+"-mix0.soundstate");
            writeBlock(stateFile,owned);writeBlock(opaqueFile,baseline.hostState);writeBlock(dryFile,dryOwned);writeBlock(zeroFile,zeroOwned);property(row,"initial_sound",identity(stateFile));property(row,"returned_opaque",identity(opaqueFile));property(row,"dry_sound",identity(dryFile));property(row,"mix0_sound",identity(zeroFile));
            const auto sameHashes=[](const Json& a,const Json& b){for(int ch=0;ch<2;++ch)if(a["channel_float32_sha256"][ch].toString()!=b["channel_float32_sha256"][ch].toString())return false;return true;};
            const bool exact=pd512==0&&pd17==0&&rd==0&&zd==0&&sameHashes(row["fresh64"],row["fresh512"])&&sameHashes(row["fresh64"],row["fresh17"])&&sameHashes(row["fresh64"],row["third_instance_opaque_recall64"])&&sameHashes(row["no_space_dry"],row["placed_mix0"]);
            const bool latency=reference.latencySamples==(higher?21:0)&&baseline.latencySamples==reference.latencySamples&&partition512.latencySamples==reference.latencySamples&&partition17.latencySamples==reference.latencySamples&&recalled.latencySamples==reference.latencySamples&&mixZero.latencySamples==reference.latencySamples;
            property(row,"raw_float_bits_exact",exact);property(row,"quality_latency_and_no_extra_pdc",latency);property(row,"wet_residual_rms_minimum",1.e-7);property(row,"success",exact&&latency&&wetDifference>0&&wetResidualRms>1.e-7);rows.add(row);
            std::cout<<"SPACE_RENDER "<<stem<<" partition512="<<pd512<<" partition17="<<pd17<<" opaque="<<rd<<" Mix0="<<zd<<" added_latency="<<baseline.latencySamples-reference.latencySamples<<'\n';
            require(exact,"Space Reverb partition, opaque fresh recall or exact dry endpoint changed");require(latency,"Space Reverb added PDC or selected quality latency changed");require(wetDifference>0&&wetResidualRms>1.e-7,"Space Reverb wet contribution is silent or processed output does not differ from dry");
            if(rate==48000&&!higher){
                const auto fullEvents=spaceRenderEvents(scene,rate);const int fullFrames=int(std::llround(scene.seconds*rate));
                const auto fullWet=render(format,*descriptions[0],rate,64,envelope,document,airack::currentSoundStateSize,false,-1,&fullEvents,fullFrames);
                const auto fullDry=render(format,*descriptions[0],rate,64,hostEnvelope(dryOwned),dry,airack::currentSoundStateSize,false,-1,&fullEvents,fullFrames);
                const auto wetWav=args.output.getChildFile(scene.id+"-processed.wav"),dryWav=args.output.getChildFile(scene.id+"-dry.wav"),midi=args.output.getChildFile(scene.id+".mid");writeWav(wetWav,fullWet.audio,rate);writeWav(dryWav,fullDry.audio,rate);writeMidiEvents(midi,spaceRenderEvents(scene,960));
                auto entry=jsonObject();property(entry,"scene",scene.id);property(entry,"description",scene.description);property(entry,"dry",identity(dryWav));property(entry,"processed",identity(wetWav));property(entry,"midi",identity(midi));property(entry,"source_fixture",stem);property(entry,"frames",fullFrames);property(entry,"dry_audio",spaceRenderAudio(fullDry,rate));property(entry,"processed_audio",spaceRenderAudio(fullWet,rate));listening.add(entry);
            }
        }
        require(rows.size()==36&&listening.size()==6,"Space Reverb packet or regression matrix is incomplete");
        property(report,"module_after",identity(binaryIn(args.plugin)));property(report,"renderer_after",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));property(report,"sources_after",spaceRenderSources());
        require(report["module_before"]["sha256"].toString()==report["module_after"]["sha256"].toString()&&report["renderer_before"]["sha256"].toString()==report["renderer_after"]["sha256"].toString()
            &&juce::JSON::toString(report["sources_before"],false)==juce::JSON::toString(report["sources_after"],false),"Source, renderer or actual bundle changed during Space render");
        finish(true,{});std::cout<<"SUCCESS Space Reverb actual-bundle static regression: 36 two-second rows, 216 fresh instances plus12 listening instances, six dry/processed listening pairs; listening NOT_RUN\n";return 0;
    }catch(const std::exception& error){finish(false,juce::String(error.what()).substring(0,512));throw;}
}

// Actual-bundle Era Reverb regression, four listening demos and the VintageVerb comparison sources.
// The instrument has no audio input, so every stimulus is played by Copper Arc's analog voice.
Arguments parseEraReverbArguments(juce::StringArray args)
{
    require(args.size()==5&&args[0]=="--era-reverb"&&args[1]=="--plugin"&&args[3]=="--output",
        "Usage: RackSynthRender --era-reverb --plugin ABSOLUTE_VST3_BUNDLE --output FRESH_ABSOLUTE_WORKSPACE_DIRECTORY");
    args.remove(0);auto parsed=parseArguments(args);
    require(!parsed.output.exists()&&!fs::exists(fs::symlink_status(fs::path(parsed.output.getFullPathName().toWideCharPointer()))),
        "Era Reverb output must be absent, including links");
    return parsed;
}
struct EraHit { double start=0; int note=60, velocity=100; };
struct EraRenderScene
{
    juce::String id,description;
    airack::SoundDocument dry,processed;
    std::vector<EraHit> hits;
    double hold=.15,seconds=8;
    std::uint8_t slot=airack::noSlot;
    std::array<std::uint8_t,3> gains{airack::noSlot,airack::noSlot,airack::noSlot};
};
std::size_t eraRenderIndex(std::uint8_t slot,const juce::String& key)
{
    const auto index=airack::rackParameterIndex(airack::NativeModule::eraReverb,slot,"era_reverb."+key);
    require(index.has_value(),"Era Reverb fixture control is not registered");return *index;
}
void setEraRenderValue(airack::SoundDocument& document,std::uint8_t slot,const juce::String& key,float physical)
{
    const auto index=eraRenderIndex(slot,key);const auto& d=airack::parameterRegistry()[index];
    require(std::isfinite(physical)&&physical>=d.minimum&&physical<=d.maximum,"Era Reverb fixture value outside registered range");
    document.values[index]=juce::NormalisableRange<float>(d.minimum,d.maximum,0,d.skew).convertTo0to1(physical);
}
// The Small Hours recipe: Space, Dark.
const std::pair<const char*,float> eraRecipe[]{{"voicing",1},{"age",0},{"decay_seconds",8},{"predelay_ms",25},{"size",.85f},{"attack",.6f},
    {"diffusion",1},{"wobble",.4f},{"wobble_rate_hz",.7f},{"grit",.15f},{"bass_multiplier",1.2f},{"damping_hz",5000},{"low_cut_hz",150},
    {"high_cut_hz",9000},{"mix",.3f}};
void setEraVoice(EraRenderScene& scene,std::initializer_list<std::pair<const char*,float>> values)
{
    for(auto* d:{&scene.dry,&scene.processed})for(const auto& [key,value]:values)setGainValue(*d,juce::String("synth.1.analog.")+key,value);
}
// Era Reverb goes directly into the first free FX slot, last on the master chain; no rack is placed.
EraRenderScene makeEraRenderScene(const juce::String& id,const juce::String& description)
{
    EraRenderScene scene;scene.id=id;scene.description=description;scene.dry=airack::copperArcSound();scene.dry.values=fixtureValues(false);
    setGainValue(scene.dry,"global.master_level",.8f);setGainValue(scene.dry,"synth.1.level",.5f);
    setGainValue(scene.dry,"output.gain_db",0);setGainValue(scene.dry,"output.protection",0);
    setGainValue(scene.dry,"synth.1.analog.filter.cutoff",9000);
    scene.processed=scene.dry;auto& layout=scene.processed.layout;
    const auto open=std::find_if(layout.effects.begin(),layout.effects.end(),[](const auto& e){return e.use!=airack::SlotUse::present;});
    require(open!=layout.effects.end(),"Era Reverb scene has no free FX slot");
    airack::RackEdit edit;edit.kind=airack::RackEditKind::addEffect;edit.slot=std::uint8_t(open-layout.effects.begin());
    edit.destination=airack::masterChain;edit.position=layout.effectCount[airack::masterChain];edit.effect=airack::EffectKind::eraReverb;
    require(airack::editRackLayout(layout,edit,layout)==airack::RackEditResult::applied,"Era Reverb slot placement failed");
    scene.slot=edit.slot;for(const auto& [key,value]:eraRecipe)setEraRenderValue(scene.processed,scene.slot,key,value);
    return scene;
}
std::vector<EraRenderScene> eraRenderScenes()
{
    constexpr double sixteenth=60./132/4;
    std::vector<EraRenderScene> scenes;
    auto pad=makeEraRenderScene("pad","Sorrow-style three-note pad held 4 s");pad.hits={{0,48},{0,55},{0,60}};pad.hold=4;pad.seconds=12;
    setEraVoice(pad,{{"amp.attack",.12f},{"amp.decay",.6f},{"amp.sustain",.65f},{"amp.release",.8f}});scenes.push_back(pad);
    auto chop=makeEraRenderScene("chop","Stand-in for a pitched vocal chop: short bright notes in a syncopated 2-step figure at 132 BPM");
    const int steps[]{0,3,6,10,12,16,19,22,26,28};const int pitches[]{67,70,72,74};
    for(std::size_t i=0;i<std::size(steps);++i)chop.hits.push_back({steps[i]*sixteenth,pitches[i%std::size(pitches)]});
    chop.hold=.09;chop.seconds=8;
    setEraVoice(chop,{{"filter.cutoff",2500},{"amp.attack",.002f},{"amp.decay",.12f},{"amp.sustain",.35f},{"amp.release",.08f}});scenes.push_back(chop);
    auto bell=makeEraRenderScene("bell","Bell pluck: three staggered notes, no sustain");bell.hits={{0,72},{.18,79},{.36,84}};bell.hold=.3;bell.seconds=8;
    setEraVoice(bell,{{"osc.1.waveform",1},{"amp.attack",0},{"amp.decay",.4f},{"amp.sustain",0},{"amp.release",.4f}});scenes.push_back(bell);
    auto snare=makeEraRenderScene("snare","2-step snare from the noise source on beats 2 and 4 with a ghost hit, 132 BPM");
    snare.hits={{4*sixteenth,60},{12*sixteenth,60},{20*sixteenth,60},{27*sixteenth,60,40},{28*sixteenth,60}};snare.hold=.05;snare.seconds=8;
    setEraVoice(snare,{{"osc.1.level",0},{"osc.2.level",0},{"noise",1},{"filter.cutoff",6000},{"amp.attack",0},{"amp.decay",.12f},{"amp.sustain",0},{"amp.release",.08f}});
    scenes.push_back(snare);
    require(scenes.size()==4,"Era Reverb four-scene packet incomplete");
    for(const auto& scene:scenes)require(airack::validateSoundDocument(scene.dry)&&airack::validateSoundDocument(scene.processed),"Invalid Era Reverb scene");
    return scenes;
}
std::vector<Event> eraRenderEvents(const EraRenderScene& scene,int rate,bool technical=false)
{
    double last=0;for(const auto& hit:scene.hits)last=std::max(last,hit.start);
    const double scale=technical?std::min(1.,1.5/(last+scene.hold)):1.;
    std::vector<Event> events;
    for(const auto& hit:scene.hits){
        events.push_back({int(std::llround(hit.start*rate*scale)),juce::MidiMessage::noteOn(1,hit.note,juce::uint8(hit.velocity))});
        events.push_back({int(std::llround((hit.start+scene.hold)*rate*scale)),juce::MidiMessage::noteOff(1,hit.note)});
    }
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.sample<b.sample;});return events;
}
Json eraRenderParameters(const airack::SoundDocument& sound,std::uint8_t slot)
{
    juce::Array<Json> fields;
    for(const auto index:airack::unitReach(airack::NativeModule::eraReverb,slot)){const auto& d=airack::parameterRegistry()[index];auto item=jsonObject();property(item,"id",d.id);property(item,"state_key",juce::int64(d.stateKey));property(item,"normalized",sound.values[index]);property(item,"physical",airack::parameterPhysicalValue(sound.values,index));fields.add(item);}
    require(fields.size()==16,"Era Reverb actual binding must expose all 15 fields and the existing bypass");
    return fields;
}
std::size_t eraGainIndex(std::uint8_t slot)
{
    const auto index=airack::rackParameterIndex(airack::NativeModule::stereo,slot,"stereo.gain_db");
    require(index.has_value(),"Era comparison gain stage is not registered");return *index;
}
double eraInputGainDb(const airack::SoundDocument& document,const EraRenderScene& scene)
{
    double db=0;for(const auto slot:scene.gains)db+=airack::parameterPhysicalValue(document.values,eraGainIndex(slot));return db;
}
// Splits a level change evenly over the three Stereo stages, -24 to +12 dB each.
void setEraInputGain(EraRenderScene& scene,double db)
{
    const double each=db/double(scene.gains.size());
    require(std::isfinite(each)&&each>=-24&&each<=12,"Era comparison level is outside the gain stages' range");
    for(auto* d:{&scene.dry,&scene.processed})for(const auto slot:scene.gains){
        const auto index=eraGainIndex(slot);const auto& p=airack::parameterRegistry()[index];
        d->values[index]=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(float(each));
    }
}
// A comparison stimulus: Copper Arc's analog voice with its filter open and no filter envelope. Master is applied after
// the FX chain, so it stays at 1 and three Stereo gain stages ahead of Era set the level: the dry WAV is Era's input.
EraRenderScene eraComparisonSource(const juce::String& name)
{
    auto scene=makeEraRenderScene("source-"+name,"Comparison source "+name);
    setEraVoice(scene,{{"filter.cutoff",20000},{"filter.resonance",0},{"filter_env.octaves",0},{"amp.decay",0}});
    for(auto* d:{&scene.dry,&scene.processed}){setGainValue(*d,"synth.1.level",1);setGainValue(*d,"global.master_level",1);}
    for(auto& stage:scene.gains){
        const auto& effects=scene.processed.layout.effects;
        const auto open=std::find_if(effects.begin(),effects.end(),[](const auto& e){return e.use!=airack::SlotUse::present;});
        require(open!=effects.end(),"Era comparison source has no free FX slot for its gain stage");stage=std::uint8_t(open-effects.begin());
        for(auto* d:{&scene.dry,&scene.processed}){
            auto& layout=d->layout;const auto& order=layout.effectOrder[airack::masterChain];const auto count=layout.effectCount[airack::masterChain];
            airack::RackEdit edit;edit.kind=airack::RackEditKind::addEffect;edit.slot=stage;edit.destination=airack::masterChain;edit.effect=airack::EffectKind::stereo;
            edit.position=std::uint8_t(std::find(order.begin(),order.begin()+count,scene.slot)-order.begin());
            require(airack::editRackLayout(layout,edit,layout)==airack::RackEditResult::applied,"Era comparison gain stage placement failed");
        }
    }
    if(name=="click"||name=="burst"){
        setEraVoice(scene,{{"osc.1.level",0},{"osc.2.level",0},{"noise",1},{"amp.attack",0}});
        if(name=="click")setEraVoice(scene,{{"amp.decay",.003f},{"amp.sustain",0},{"amp.release",.003f}});
        else setEraVoice(scene,{{"amp.sustain",1},{"amp.release",.002f}});
        scene.hits={{0,60}};scene.hold=name=="click"?.003:.05;
    }else if(name=="sine220"||name=="sine220_quiet"||name=="sine1k"){
        setEraVoice(scene,{{"osc.1.waveform",0},{"osc.1.level",1},{"osc.2.level",0},{"noise",0},{"amp.attack",.01f},{"amp.sustain",1},{"amp.release",.01f}});
        if(name=="sine1k")setEraVoice(scene,{{"osc.1.detune",21.31f}});
        scene.hits={{0,name=="sine1k"?83:57}};scene.hold=2;
    }else require(false,"Unknown Era Reverb comparison source");
    for(const auto* d:{&scene.dry,&scene.processed})require(airack::validateSoundDocument(*d),"Invalid Era Reverb comparison source");
    return scene;
}
double eraComparisonPeakTarget(const juce::String& name)
{
    return name=="sine220"?std::pow(10.,-6./20):name=="sine220_quiet"?std::pow(10.,-36./20):name=="sine1k"?std::pow(10.,-12./20):0;
}
void setEraComparisonSettings(airack::SoundDocument& document,std::uint8_t slot,const Json& neutral,const Json& ours)
{
    for(const auto* settings:{&neutral,&ours}){
        const auto* object=settings->getDynamicObject();require(object!=nullptr,"Era Reverb comparison settings must be objects");
        for(const auto& entry:object->getProperties())setEraRenderValue(document,slot,entry.name.toString(),float(double(entry.value)));
    }
}
int renderEraReverb(const Arguments& args)
{
    // Recheck the same fresh-path policy after plugin discovery, before making any output.
    parseEraReverbArguments({"--era-reverb","--plugin",args.plugin.getFullPathName(),"--output",args.output.getFullPathName()});
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;format.findAllTypesForFile(descriptions,args.plugin.getFullPathName());
    require(descriptions.size()==1&&isAirInstrument(*descriptions[0]),"Era Reverb bundle must contain the expected instrument");
    parseEraReverbArguments({"--era-reverb","--plugin",args.plugin.getFullPathName(),"--output",args.output.getFullPathName()});
    const auto rowsFile=juce::File(RACKSYNTH_WORKSPACE).getChildFile("tools/era_reverb_comparison_rows.json");
    const auto comparison=juce::JSON::parse(rowsFile);
    require(comparison["rows"].isArray()&&comparison["neutral"].isObject()&&int(comparison["rate"])==48000&&int(comparison["block"])==512,"Era Reverb comparison rows are missing or malformed");
    require(args.output.createDirectory().wasOk()&&args.output.getChildFile("comparison").createDirectory().wasOk(),"Era Reverb output creation failed");
    auto report=jsonObject();juce::Array<Json> rows,listening,sources,compared;
    property(report,"configuration",RACKSYNTH_CONFIGURATION);property(report,"scope","Actual-bundle static audio/host-ID/state/latency regression, four listening demos and the VintageVerb comparison sources; no callback capacity or listening acceptance");
    property(report,"module_before",identity(binaryIn(args.plugin)));property(report,"renderer_before",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));property(report,"sources_before",spaceRenderSources());
    property(report,"comparison_rows",identity(rowsFile));property(report,"initial_state_before_prepare",true);property(report,"static_settings_no_automation",true);property(report,"cpu_capacity_acceptance",false);property(report,"listening_verdict","DEFERRED_BY_USER");
    property(report,"float_wav_format","stereo IEEE float32, raw product output gain0dB/protectionOFF, no normalization or gain matching");property(report,"listening_quality","realtime, 48000 Hz");
    property(report,"technical_seconds_per_row",2);property(report,"technical_event_policy","Scale the complete scene phrase only when needed so its final noteOff is at1.5s; listening phrases keep their timing and full 8-12 s captures");
    property(report,"placement","Copper Arc/fixtureValues carrier with Era Reverb added directly to the first free FX slot, last on the master chain; no rack");
    auto recipe=jsonObject();for(const auto& [key,value]:eraRecipe)property(recipe,key,value);property(report,"recipe",recipe);
    property(report,"partition_and_recall_limit",0);property(report,"dry_mix0_limit",0);property(report,"added_host_latency_limit_samples",0);
    const auto finish=[&](bool success,const juce::String& error){property(report,"success",success);property(report,"error",error);property(report,"fixtures",rows);property(report,"listening_packet",listening);property(report,"comparison_sources",sources);property(report,"comparison_processed",compared);property(report,"completed_rows",rows.size());property(report,"expected_rows",24);property(report,"completed_listening_pairs",listening.size());writePatternJson(args.output.getChildFile("era-reverb-report.json"),report);};
    try{
        const auto sameHashes=[](const Json& a,const Json& b){for(int ch=0;ch<2;++ch)if(a["channel_float32_sha256"][ch].toString()!=b["channel_float32_sha256"][ch].toString())return false;return true;};
        const auto scenes=eraRenderScenes();
        for(const auto& scene:scenes)for(const int rate:{44100,48000,96000})for(const bool higher:{false,true}){
            const auto stem=scene.id+"-"+(higher?"higher":"realtime")+"-"+juce::String(rate);auto document=scene.processed,dry=scene.dry;
            for(auto* d:{&document,&dry})setGainValue(*d,"output.render_quality",higher?1.f:0.f);
            auto zero=document;setEraRenderValue(zero,scene.slot,"mix",0);const auto events=eraRenderEvents(scene,rate,true);const int frames=2*rate;
            juce::MemoryBlock owned,dryOwned,zeroOwned;require(airack::encodeSoundState(document,owned)&&airack::encodeSoundState(dry,dryOwned)&&airack::encodeSoundState(zero,zeroOwned),"Era scene sound encoding failed");
            const auto envelope=hostEnvelope(owned);
            const auto baseline=render(format,*descriptions[0],rate,64,envelope,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto partition512=render(format,*descriptions[0],rate,512,envelope,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto recalled=render(format,*descriptions[0],rate,64,baseline.hostState,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto partition17=render(format,*descriptions[0],rate,17,envelope,document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto reference=render(format,*descriptions[0],rate,64,hostEnvelope(dryOwned),dry,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto mixZero=render(format,*descriptions[0],rate,64,hostEnvelope(zeroOwned),zero,airack::currentSoundStateSize,false,-1,&events,frames);
            const char* silent="Era Reverb raw render is silent or negligible";
            auto row=jsonObject();property(row,"id",stem);property(row,"description",scene.description);property(row,"sample_rate",rate);property(row,"quality",higher?"higher":"realtime");property(row,"frames",frames);property(row,"slot",int(scene.slot));property(row,"era_parameters",eraRenderParameters(document,scene.slot));
            property(row,"fresh64",spaceRenderAudio(baseline,rate,silent));property(row,"fresh512",spaceRenderAudio(partition512,rate,silent));property(row,"fresh17",spaceRenderAudio(partition17,rate,silent));property(row,"third_instance_opaque_recall64",spaceRenderAudio(recalled,rate,silent));property(row,"no_era_dry",spaceRenderAudio(reference,rate,silent));property(row,"placed_mix0",spaceRenderAudio(mixZero,rate,silent));
            const double pd512=difference(baseline.audio,partition512.audio),pd17=difference(baseline.audio,partition17.audio),rd=difference(baseline.audio,recalled.audio),zd=difference(reference.audio,mixZero.audio),wetDifference=difference(reference.audio,baseline.audio);
            property(row,"partition512_max_abs_difference",pd512);property(row,"partition17_max_abs_difference",pd17);property(row,"opaque_recall_max_abs_difference",rd);property(row,"dry_mix0_max_abs_difference",zd);property(row,"processed_dry_max_abs_difference",wetDifference);property(row,"added_latency_samples",baseline.latencySamples-reference.latencySamples);
            // Era's Mix is equal-power: the dry contribution is cos(pi m / 2).
            const double dryGain=std::cos(std::numbers::pi/2*airack::parameterPhysicalValue(document.values,eraRenderIndex(scene.slot,"mix")));double wetSquares=0;
            for(int ch=0;ch<2;++ch)for(int sample=0;sample<frames;++sample){const double residual=baseline.audio.getSample(ch,sample)-dryGain*reference.audio.getSample(ch,sample);wetSquares+=residual*residual;}
            const double wetResidualRms=std::sqrt(wetSquares/(2.*frames));property(row,"wet_residual_rms",wetResidualRms);property(row,"wet_residual_scope","Numerical residual after subtracting the declared static equal-power dry contribution; hearing remains a separate verdict");
            const auto stateFile=args.output.getChildFile(stem+".soundstate"),opaqueFile=args.output.getChildFile(stem+".hoststate"),dryFile=args.output.getChildFile(stem+"-dry.soundstate"),zeroFile=args.output.getChildFile(stem+"-mix0.soundstate");
            writeBlock(stateFile,owned);writeBlock(opaqueFile,baseline.hostState);writeBlock(dryFile,dryOwned);writeBlock(zeroFile,zeroOwned);property(row,"initial_sound",identity(stateFile));property(row,"returned_opaque",identity(opaqueFile));property(row,"dry_sound",identity(dryFile));property(row,"mix0_sound",identity(zeroFile));
            const bool exact=pd512==0&&pd17==0&&rd==0&&zd==0&&sameHashes(row["fresh64"],row["fresh512"])&&sameHashes(row["fresh64"],row["fresh17"])&&sameHashes(row["fresh64"],row["third_instance_opaque_recall64"])&&sameHashes(row["no_era_dry"],row["placed_mix0"]);
            const bool latency=reference.latencySamples==(higher?21:0)&&baseline.latencySamples==reference.latencySamples&&partition512.latencySamples==reference.latencySamples&&partition17.latencySamples==reference.latencySamples&&recalled.latencySamples==reference.latencySamples&&mixZero.latencySamples==reference.latencySamples;
            property(row,"raw_float_bits_exact",exact);property(row,"quality_latency_and_no_extra_pdc",latency);property(row,"wet_residual_rms_minimum",1.e-7);property(row,"success",exact&&latency&&wetDifference>0&&wetResidualRms>1.e-7);rows.add(row);
            std::cout<<"ERA_RENDER "<<stem<<" partition512="<<pd512<<" partition17="<<pd17<<" opaque="<<rd<<" Mix0="<<zd<<" added_latency="<<baseline.latencySamples-reference.latencySamples<<'\n';
            require(exact,"Era Reverb partition, opaque fresh recall or exact dry endpoint changed");require(latency,"Era Reverb added PDC or selected quality latency changed");require(wetDifference>0&&wetResidualRms>1.e-7,"Era Reverb wet contribution is silent or processed output does not differ from dry");
            if(rate==48000&&!higher){
                const auto fullEvents=eraRenderEvents(scene,rate);const int fullFrames=int(std::llround(scene.seconds*rate));
                const auto fullWet=render(format,*descriptions[0],rate,64,envelope,document,airack::currentSoundStateSize,false,-1,&fullEvents,fullFrames);
                const auto fullDry=render(format,*descriptions[0],rate,64,hostEnvelope(dryOwned),dry,airack::currentSoundStateSize,false,-1,&fullEvents,fullFrames);
                const auto wetWav=args.output.getChildFile(scene.id+"-processed.wav"),dryWav=args.output.getChildFile(scene.id+"-dry.wav"),midi=args.output.getChildFile(scene.id+".mid");writeWav(wetWav,fullWet.audio,rate);writeWav(dryWav,fullDry.audio,rate);writeMidiEvents(midi,eraRenderEvents(scene,960));
                auto entry=jsonObject();property(entry,"scene",scene.id);property(entry,"description",scene.description);property(entry,"dry",identity(dryWav));property(entry,"processed",identity(wetWav));property(entry,"midi",identity(midi));property(entry,"source_fixture",stem);property(entry,"frames",fullFrames);property(entry,"dry_audio",spaceRenderAudio(fullDry,rate,silent));property(entry,"processed_audio",spaceRenderAudio(fullWet,rate,silent));
                // Listening copies carry the product's default Output gain, the same on dry and processed; no normalization.
                const auto registry=airack::parameterRegistry();const auto gain=std::find_if(registry.begin(),registry.end(),[](const auto& p){return p.id=="output.gain_db";});
                const double listenDb=airack::parameterPhysicalValue(airack::defaultParameterValues(),std::size_t(gain-registry.begin()));property(entry,"listen_gain_db",listenDb);
                for(const auto& [suffix,audio]:{std::pair<const char*,const juce::AudioBuffer<float>*>{"dry",&fullDry.audio},{"processed",&fullWet.audio}}){
                    auto copy=*audio;copy.applyGain(float(std::pow(10.,listenDb/20)));require(copy.getMagnitude(0,copy.getNumSamples())<1,"Era listening copy clips at the default Output gain");
                    const auto path=args.output.getChildFile(scene.id+"-"+suffix+"-listen.wav");writeWav(path,copy,rate);property(entry,(juce::String(suffix)+"_listen").toRawUTF8(),identity(path));
                }
                listening.add(entry);
            }
        }
        require(rows.size()==24&&listening.size()==4,"Era Reverb packet or regression matrix is incomplete");

        // Comparison sources: each dry stimulus once, each row processed at Mix and settings from the rows file.
        constexpr int rate=48000,block=512;const auto directory=args.output.getChildFile("comparison");
        require(airack::parameterRegistry()[0].id=="global.master_level","Era comparison calibration expects Master level first");
        std::map<juce::String,EraRenderScene> stimuli;std::map<juce::String,double> longest;
        for(const auto& row:*comparison["rows"].getArray())longest[row["source"].toString()]=std::max(longest[row["source"].toString()],double(row["seconds"]));
        for(const auto& row:*comparison["rows"].getArray()){
            const auto name=row["source"].toString();const double seconds=double(row["seconds"]);const auto id=row["id"].toString();
            require(id.isNotEmpty()&&seconds>=3&&seconds<=30,"Era Reverb comparison row needs an id and 3 to 30 seconds");
            auto found=stimuli.find(name);
            if(found==stimuli.end()){
                auto scene=eraComparisonSource(name);const auto events=eraRenderEvents(scene,rate);const auto target=eraComparisonPeakTarget(name);
                const auto renderDry=[&](const airack::SoundDocument& d){juce::MemoryBlock bytes;require(airack::encodeSoundState(d,bytes),"Era comparison source encoding failed");
                    return render(format,*descriptions[0],rate,block,hostEnvelope(bytes),d,airack::currentSoundStateSize,false,-1,&events,int(rate*scene.hold)+rate);};
                if(target>0){
                    const auto probe=renderDry(scene.dry);const auto peak=measureWindow(probe.audio,{{0,probe.audio.getNumSamples()}}).peak;
                    require(peak>0,"Era comparison sine is silent");setEraInputGain(scene,20*std::log10(target/peak));
                }
                const int frames=int(std::llround(longest[name]*rate));const auto dryRender=render(format,*descriptions[0],rate,block,[&]{juce::MemoryBlock bytes;require(airack::encodeSoundState(scene.dry,bytes),"Era comparison source encoding failed");return hostEnvelope(bytes);}(),scene.dry,airack::currentSoundStateSize,false,-1,&events,frames);
                const auto wav=directory.getChildFile("source-"+name+".wav");writeWav(wav,dryRender.audio,rate);
                auto entry=jsonObject();property(entry,"source",name);property(entry,"description",comparison["sources"][juce::Identifier(name)].toString());property(entry,"wav",identity(wav));property(entry,"audio",spaceRenderAudio(dryRender,rate,"Era comparison source is silent"));
                const auto peak=measureWindow(dryRender.audio,{{0,frames}}).peak;
                require(target==0||std::abs(20*std::log10(peak/target))<=.1,"Era comparison sine missed its calibrated level by more than 0.1 dB");
                property(entry,"master_level",airack::parameterPhysicalValue(scene.dry.values,0));property(entry,"gain_stages_db",eraInputGainDb(scene.dry,scene));property(entry,"peak_target",target);property(entry,"peak",peak);sources.add(entry);
                found=stimuli.emplace(name,scene).first;
            }
            auto document=found->second.processed;setEraComparisonSettings(document,found->second.slot,comparison["neutral"],row["ours"]);
            juce::MemoryBlock owned;require(airack::encodeSoundState(document,owned),"Era comparison row encoding failed");
            const auto events=eraRenderEvents(found->second,rate);const int frames=int(std::llround(seconds*rate));
            const auto processed=render(format,*descriptions[0],rate,block,hostEnvelope(owned),document,airack::currentSoundStateSize,false,-1,&events,frames);
            const auto wav=directory.getChildFile(id+"-processed.wav");writeWav(wav,processed.audio,rate);
            auto entry=jsonObject();property(entry,"id",id);property(entry,"source",name);property(entry,"wav",identity(wav));property(entry,"era_parameters",eraRenderParameters(document,found->second.slot));property(entry,"audio",spaceRenderAudio(processed,rate,"Era comparison row is silent"));compared.add(entry);
            std::cout<<"ERA_COMPARISON "<<id<<" source="<<name<<'\n';
        }
        require(compared.size()==comparison["rows"].size(),"Era Reverb comparison sources incomplete");
        property(report,"module_after",identity(binaryIn(args.plugin)));property(report,"renderer_after",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));property(report,"sources_after",spaceRenderSources());
        require(report["module_before"]["sha256"].toString()==report["module_after"]["sha256"].toString()&&report["renderer_before"]["sha256"].toString()==report["renderer_after"]["sha256"].toString()
            &&juce::JSON::toString(report["sources_before"],false)==juce::JSON::toString(report["sources_after"],false),"Source, renderer or actual bundle changed during Era render");
        finish(true,{});std::cout<<"SUCCESS Era Reverb actual-bundle static regression: 24 two-second rows, 144 fresh instances, four dry/processed listening pairs, "<<compared.size()<<" comparison rows; listening DEFERRED_BY_USER\n";return 0;
    }catch(const std::exception& error){finish(false,juce::String(error.what()).substring(0,512));throw;}
}

namespace drum_product {
using namespace airack::drums;
struct Image { airack::ProjectDocument project; DrumBundlePtr bundle; juce::MemoryBlock owned; };
std::span<const std::uint8_t> bytes(const juce::MemoryBlock& b){return {static_cast<const std::uint8_t*>(b.getData()),b.getSize()};}
Image decodeOwned(const juce::MemoryBlock& raw){
    require(raw.getSize()>0&&raw.getSize()<=maxDrumBundleBytes,"Drum owned bundle size invalid");
    Image result;DrumError error{};require(decodeDrumBundle(bytes(raw),result.bundle,error),"Drum whole-file bundle invalid");
    const auto compact=result.bundle->compactDocument();
    require(airack::decodeProjectState(compact.data(),compact.size(),result.project)&&airack::validateProjectDocument(result.project),"Drum bundle requires complete Project payload");
    bool present=false;for(unsigned bank=0;bank<4;++bank)present|=result.project.sound.layout.synthUse[bank]==airack::SlotUse::present&&airack::sourceModule(result.project.sound,std::uint8_t(bank))==airack::NativeModule::drum;
    require(present,"Drum Project has no present Drum source");result.owned=raw;return result;
}
Image decodeHosted(const juce::MemoryBlock& opaque){
    constexpr std::size_t wrapperLimit=65536;constexpr std::string_view magic="JUCEPrivateData";
    require(opaque.getSize()>0&&opaque.getSize()<=2*(maxDrumBundleBytes+wrapperLimit)+1048576,"Drum opaque envelope exceeds bound");
    const auto xml=juce::AudioProcessor::getXmlFromBinary(opaque.getData(),int(opaque.getSize()));require(xml&&xml->hasTagName("VST3PluginState"),"Drum VST3 envelope missing");
    const auto* child=xml->getChildByName("IComponent");juce::MemoryBlock component;require(child&&component.fromBase64Encoding(child->getAllSubText()),"Drum actual IComponent missing");
    const auto count=component.getSize();const auto* data=static_cast<const char*>(component.getData());
    require(count>=magic.size()+16&&count<=maxDrumBundleBytes+wrapperLimit,"Drum component owned/private size invalid");
    require(std::string_view(data+count-magic.size(),magic.size())==magic,"Drum terminal JUCE private magic missing");
    juce::MemoryInputStream length(data+count-magic.size()-8,8,false);const auto signedPrivate=length.readInt64();
    require(signedPrivate>=0&&std::uint64_t(signedPrivate)<=count-magic.size()-16,"Drum signed private length invalid");
    const auto privateBytes=std::size_t(signedPrivate);require(privateBytes<=wrapperLimit-magic.size()-16,"Drum private wrapper exceeds separate65536 bound");
    const auto owned=count-privateBytes-magic.size()-16;require(owned>0&&owned<=maxDrumBundleBytes,"Drum owned prefix exceeds its independent bound");
    for(std::size_t n=0;n<8;++n)require(data[owned+n]==0,"Drum private separator must be eight zero bytes");
    if(privateBytes){juce::MemoryInputStream privateStream(data+owned+8,privateBytes,false);const auto tree=juce::ValueTree::readFromStream(privateStream);require(tree.isValid()&&tree.hasType("JUCEPrivateData")&&std::uint64_t(privateStream.getPosition())==privateBytes,"Drum private tree type/content/EOF invalid");}
    return decodeOwned(juce::MemoryBlock(data,owned));
}
void plainPath(const juce::File& path){
    for(auto p=fs::path(path.getFullPathName().toWideCharPointer());!p.empty();p=p.parent_path()){
#if JUCE_WINDOWS
        const auto flags=GetFileAttributesW(p.c_str());require(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT)==0,"Drum path contains a reparse point");
#else
        require(!fs::is_symlink(fs::symlink_status(p)),"Drum path contains a link");
#endif
        if(p==p.parent_path())break;
    }
}
// Canonical row IDs preserve the existing four-scene/rate/quality matrix.
const juce::StringArray syntheticSceneIds{"single-pad","sixteen-pad-midi","four-banks-128","pattern-routing-space"};
juce::String caseId(int which,int rate,bool higher,bool savedState){return (savedState?juce::String("real-kept-kit"):syntheticSceneIds[which])+"-"+juce::String(rate)+(higher?"-higher":"-normal");}
juce::StringArray caseIds(bool savedState){juce::StringArray ids;for(int which=0;which<(savedState?1:4);++which)for(int rate:{44100,48000,96000})for(bool higher:{false,true})ids.add(caseId(which,rate,higher,savedState));return ids;}
Image smallFixture(){
    Image out;out.project.sound.values[airack::drumModeParameterStart]=1;
    const std::array<float,4> pcm{.125f,-.25f,.375f,0};AssetPtr asset;AssetError ae{};require(makeSampleAsset(pcm,1,asset,ae),"Drum self-test asset invalid");
    DrumSlots slots{};slots[0][0].asset=asset->id();slots[0][0].name="Independent test pad";DrumDocumentPtr document;DrumError error{};require(makeDrumDocument(slots,document,error),"Drum test document invalid");
    juce::MemoryBlock compact;require(airack::encodeProjectState(out.project,compact),"Drum test Project invalid");const std::array<AssetPtr,1> owners{asset};
    require(makeDrumBundle(document,bytes(compact),owners,out.bundle,error)&&encodeDrumBundle(*out.bundle,out.owned,error),"Drum test bundle invalid");return out;
}
juce::MemoryBlock wrapped(const juce::MemoryBlock& owned,bool tree){
    juce::MemoryBlock component=owned;const std::array<char,8> zeros{};component.append(zeros.data(),zeros.size());
    juce::MemoryBlock privateBytes;if(tree){juce::ValueTree value("JUCEPrivateData");juce::MemoryOutputStream stream(privateBytes,false);value.writeToStream(stream);}
    component.append(privateBytes.getData(),privateBytes.getSize());juce::MemoryOutputStream stream(component,true);stream.writeInt64(juce::int64(privateBytes.getSize()));stream.write("JUCEPrivateData",15);stream.flush();return hostEnvelope(component);
}
}
Arguments parseDrumProductArguments(juce::StringArray args){
    require(args.size()>=5&&args[0]=="--drum-product","Drum product selector missing");args.remove(0);
    juce::File state;bool pending=false;juce::String selectedCase;
    if(args.size()>=6&&args[args.size()-2]=="--drum-product-case"){selectedCase=args[args.size()-1];require(selectedCase.isNotEmpty(),"Drum product case must not be empty");args.removeRange(args.size()-2,2);}
    if(args.size()==5&&args[4]=="--drum-pending-unload"){pending=true;args.remove(4);}
    else if(args.size()==6&&args[4]=="--drum-state"){require(juce::File::isAbsolutePath(args[5]),"Drum state path must be absolute");state=juce::File(args[5]);args.removeRange(4,2);}
    require(args.size()==4&&args[0]=="--plugin"&&args[2]=="--output","Usage: --drum-product --plugin ABS --output FRESH [--drum-state ABS | --drum-pending-unload] [--drum-product-case CANONICAL_ID, incompatible with pending-unload]");
    require(selectedCase.isEmpty()||(!pending&&drum_product::caseIds(state!=juce::File{}).contains(selectedCase)),"Unknown Drum case or incompatible source/pending mode");
    auto result=parseArguments(args);result.drumProduct=true;result.drumPendingUnload=pending;result.drumState=state;result.drumCase=selectedCase;
    drum_product::plainPath(result.output);require(!result.output.exists()&&!fs::exists(fs::symlink_status(fs::path(result.output.getFullPathName().toWideCharPointer()))),"Drum output must be absent");
    if(state!=juce::File{}){
        drum_product::plainPath(state);const auto workspace=fs::weakly_canonical(fs::path(juce::File(RACKSYNTH_WORKSPACE).getFullPathName().toWideCharPointer()));const auto source=fs::weakly_canonical(fs::path(state.getFullPathName().toWideCharPointer()));const auto relative=source.lexically_relative(workspace);
        require(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="..","Drum state escapes workspace");require(state.existsAsFile()&&state.getSize()>0&&std::uint64_t(state.getSize())<=airack::drums::maxDrumBundleBytes,"Drum state file missing or oversized");
        require(state!=result.output&&!result.output.isAChildOf(state)&&!state.isAChildOf(result.output),"Drum source/output alias");
        const auto before=juce::SHA256(state);require(state.loadFileAsData(result.drumStateBytes)&&before==juce::SHA256(result.drumStateBytes)&&before==juce::SHA256(state),"Drum state changed while reading");drum_product::decodeOwned(result.drumStateBytes);
    }
    return result;
}
// The existing host's opted-in owned drum validation.
namespace drum_product {
Json eventRecords(const std::vector<Event>& events){Json rows(juce::Array<Json>{});for(const auto& event:events){auto row=jsonObject();property(row,"sample",event.sample);Json raw(juce::Array<Json>{});for(int n=0;n<event.message.getRawDataSize();++n)raw.append(int(event.message.getRawData()[n]));property(row,"bytes",raw);property(row,"note_on",event.message.isNoteOn());property(row,"note_off",event.message.isNoteOff());rows.append(row);}return rows;}
void verifyImage(const Image& actual,const Image& expected){
    require(actual.project==expected.project,"Drum complete numeric Project differs");
    require(actual.bundle->document().slots()==expected.bundle->document().slots(),"Drum complete64 metadata/assignments differ");
    require(actual.bundle->assets().size()==expected.bundle->assets().size(),"Drum closed asset count differs");
    for(const auto& asset:expected.bundle->assets()){
        const auto* found=actual.bundle->findAsset(asset->id());
        require(found&&found->channels()==asset->channels()&&found->frames()==asset->frames(),"Drum canonical owner geometry differs");
        require(found->samples().size()==asset->samples().size()&&std::memcmp(found->samples().data(),asset->samples().data(),asset->samples().size_bytes())==0,"Drum canonical PCM bits differ");
    }
}
Image reproject(const Image& source,const airack::ProjectDocument& project){
    Image out;out.project=project;juce::MemoryBlock compact;require(airack::encodeProjectState(project,compact),"Drum Project encode failed");
    DrumDocumentPtr document;DrumError error{};require(makeDrumDocument(source.bundle->document().slots(),document,error),"Drum document clone failed");
    require(makeDrumBundle(document,bytes(compact),source.bundle->assets(),out.bundle,error)&&encodeDrumBundle(*out.bundle,out.owned,error),"Drum complete bundle encode failed");return out;
}
airack::SoundDocument tileSound(unsigned banks){
    auto sound=airack::newInstanceSound();
    for(unsigned bank=0;bank<banks;++bank){
        airack::RackOperation operation;operation.design=airack::test::frozenTileArrayRackDesign();
        operation.newInstance=juce::Uuid("daf5a143-7afa-4daa-a581-"+juce::String(bank+1).paddedLeft('0',12));
        operation.destination={airack::RackRole::synth,airack::masterChain,std::uint8_t(bank)};
        operation.allowSlotReuse=true;operation.allocation={{0,std::uint8_t(bank)}};
        airack::SoundDocument next;airack::RackDifference difference;
        require(airack::makeRackCandidate(sound,operation,airack::SoundEditOrigin::manual,next,difference)==airack::RackCandidateStatus::ready,"Drum actual Tile placement failed");sound=std::move(next);
    }
    setGainValue(sound,"global.master_level",1);setGainValue(sound,"output.gain_db",0);setGainValue(sound,"output.protection",0);
    for(unsigned bank=0;bank<banks;++bank){
        const auto prefix="synth."+juce::String(bank+1)+".";setGainValue(sound,prefix+"level",1);setGainValue(sound,prefix+"pan",0);
        for(unsigned pad=0;pad<16;++pad){const auto p=prefix+"drum.pad."+juce::String(pad+1)+".";
            for(const auto& field:std::initializer_list<std::pair<const char*,float>>{{"gain_db",0},{"pan",0},{"tune_semitones",0},{"trim_start",0},{"trim_end",1},{"attack_seconds",0},{"decay_seconds",0},{"midi_note",float(36+pad)},{"choke_group",0}})setGainValue(sound,p+field.first,field.second);
        }
    }
    require(airack::validateSoundDocument(sound),"Neutral Tile sound invalid");return sound;
}
struct Scene {
    juce::String id;Image image;std::vector<Event> events;std::vector<PatternClock> clocks{{}};
    int rate=48000,frames=0;bool dry=true,single=false,fourBanks=false,pattern=false;std::uint8_t spaceSlot=airack::noSlot;
};
Image bundleFor(const airack::ProjectDocument& project,const DrumSlots& slots,std::span<const AssetPtr> owners){
    Image out;out.project=project;DrumDocumentPtr document;DrumError error{};juce::MemoryBlock compact;
    require(makeDrumDocument(slots,document,error)&&airack::encodeProjectState(project,compact),"Drum fixture Project/document invalid");
    require(makeDrumBundle(document,bytes(compact),owners,out.bundle,error)&&encodeDrumBundle(*out.bundle,out.owned,error),"Drum fixture closed bundle invalid");return out;
}
Scene syntheticScene(int which,int rate){
    Scene scene;scene.rate=rate;scene.frames=rate;scene.id=syntheticSceneIds[which];
    auto project=std::make_unique<airack::ProjectDocument>();project->sound=tileSound(which==2?4:1);DrumSlots slots{};std::vector<AssetPtr> owners;
    for(unsigned bank=0;bank<(which==2?4u:1u);++bank)for(unsigned pad=0;pad<(which==0?1u:16u);++pad){
        const unsigned channels=which==0||pad%2==0?1:2;const unsigned frames=which==2?4096:1024;
        std::vector<float> pcm(std::size_t(channels)*frames);
        for(unsigned frame=0;frame<frames;++frame)for(unsigned ch=0;ch<channels;++ch){
            float value=which==0?(frame==0?.125f:frame+1==frames?0.f:.0625f):float(1+bank*16+pad+ch)/65536.f;
            if(which==1&&frame%31==0)value=-value;pcm[std::size_t(frame)*channels+ch]=value;
        }
        AssetPtr asset;AssetError error{};require(makeSampleAsset(pcm,channels,asset,error),"Synthetic canonical sample invalid");owners.push_back(asset);
        slots[bank][pad].asset=asset->id();slots[bank][pad].name="Literal bank "+std::to_string(bank+1)+" pad "+std::to_string(pad+1);
    }
    const auto on=[&](int frame,int note,int velocity=127,int channel=1){scene.events.push_back({frame,juce::MidiMessage::noteOn(channel,note,juce::uint8(velocity))});};
    if(which==0){scene.single=true;on(0,80);on(17,36);scene.events.push_back({31,juce::MidiMessage::noteOff(1,36)});on(rate/10,36,64);on(rate/5,36,1);}
    if(which==1){
        for(int pad=0;pad<16;++pad){const int frame=17+pad*rate/100;on(frame,36+pad,pad%3==0?1:pad%3==1?64:127,pad%2+1);scene.events.push_back({frame+1,juce::MidiMessage::noteOff(pad%2+1,36+pad)});}
        setGainValue(project->sound,"synth.1.drum.pad.16.midi_note",36);setGainValue(project->sound,"synth.1.drum.pad.1.choke_group",1);setGainValue(project->sound,"synth.1.drum.pad.2.choke_group",1);
        on(rate/3,36);on(rate/3+3,37,64);on(rate/3+5,38,127,2);
        scene.events.push_back({rate/3+rate/100,juce::MidiMessage::controllerEvent(1,123,0)});scene.events.push_back({rate/3+rate/100+4,juce::MidiMessage::controllerEvent(2,120,0)});
        on(rate/2,39,100,3);scene.events.push_back({rate/2+17,juce::MidiMessage::midiStop()});on(rate/2+32,40,100,3);
    }
    if(which==2){scene.fourBanks=true;for(int round=0;round<2;++round)for(int pad=0;pad<16;++pad)on(17+round,36+pad,127,round+1);}
    if(which==3){
        scene.dry=false;scene.pattern=true;scene.frames=8*rate;project->mode=airack::PlaybackMode::internalPattern;
        project->pattern.noteCount=4;project->pattern.notes[0]={1.25/rate,.125,36,127,1};project->pattern.notes[1]={.5,.125,38,64,2};project->pattern.notes[2]={1,.125,40,100,1};project->pattern.notes[3]={1.5,.125,42,127,2};
        for(int loop=0;loop<2;++loop)for(int note=0;note<4;++note){const auto& n=project->pattern.notes[std::size_t(note)];const auto start=beatSample(2*loop+n.startBeats,rate);on(start,n.pitch,n.velocity,n.channel);scene.events.push_back({beatSample(2*loop+n.startBeats+n.durationBeats,rate),juce::MidiMessage::noteOff(n.channel,n.pitch)});}
        auto stopped=PatternClock{};stopped.sample=2*rate;stopped.ppq=4;stopped.playing=false;scene.clocks.push_back(stopped);
        // The literal phrase uses a two-beat host loop. Split every wrap exactly in renderScene.
        scene.clocks[0].looping=true;scene.clocks[0].loopPoints=true;scene.clocks[0].loopStart=0;scene.clocks[0].loopEnd=2;
        // A separate low-level melodic bank remains a literal right-channel control.
        {const auto designs=airack::test::frozenFactoryRackDesigns();const auto design=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.name=="Copper Arc";});require(design!=designs.end(),"Drum unrelated Copper Arc fixture missing");
            airack::RackOperation operation;operation.design=*design;operation.newInstance=juce::Uuid("756c7a5f-fd3d-421f-a086-000000000002");operation.destination={airack::RackRole::synth,airack::masterChain,1};operation.allowSlotReuse=true;operation.allocation={{0,1}};
            airack::SoundDocument next;airack::RackDifference difference;require(airack::makeRackCandidate(project->sound,operation,airack::SoundEditOrigin::manual,next,difference)==airack::RackCandidateStatus::ready,"Drum unrelated melodic bank placement failed");project->sound=std::move(next);
            setGainValue(project->sound,"synth.1.pan",-1);setGainValue(project->sound,"synth.2.pan",1);setGainValue(project->sound,"synth.2.level",.001f);setGainValue(project->sound,"synth.2.analog.noise",0);setGainValue(project->sound,"synth.2.analog.amp.release",.01f);
        }
        airack::RackOperation operation;operation.design=spaceRenderFactory();operation.newInstance=juce::Uuid("756c7a5f-fd3d-421f-a086-000000000001");operation.destination={airack::RackRole::fx,airack::masterChain,0};
        airack::SoundDocument wet;airack::RackDifference difference;require(airack::makeRackCandidate(project->sound,operation,airack::SoundEditOrigin::manual,wet,difference)==airack::RackCandidateStatus::ready,"Drum real Lumen Field placement failed");
        const auto found=std::find_if(wet.personalities->instances.begin(),wet.personalities->instances.end(),[&](const auto& r){return r.id==operation.newInstance;});require(found!=wet.personalities->instances.end(),"Drum Space instance missing");
        const auto* binding=airack::rackUnitBinding(*found,found->design.units[0].id);require(binding,"Drum Space binding missing");scene.spaceSlot=binding->slot;
        for(const auto& field:std::initializer_list<std::pair<const char*,float>>{{"decay_seconds",.65f},{"mix",.32f},{"predelay_ms",8},{"size",.2f}})setSpaceRenderValue(wet,scene.spaceSlot,field.first,field.second);
        require(airack::editRackLayout(wet.layout,{airack::RackEditKind::addEffect,1,0,0,airack::EffectKind::stereo},wet.layout)==airack::RackEditResult::applied,"Drum separate bank insert fixture failed");
        require(airack::editRackLayout(wet.layout,{airack::RackEditKind::addEffect,2,airack::masterChain,1,airack::EffectKind::stereo},wet.layout)==airack::RackEditResult::applied,"Drum separate master fixture failed");
        setGainValue(wet,"effect.2.stereo.gain_db",-6);setGainValue(wet,"effect.2.stereo.pan",0);setGainValue(wet,"effect.3.stereo.gain_db",-6);setGainValue(wet,"effect.3.stereo.pan",0);project->sound=std::move(wet);
    }
    std::stable_sort(scene.events.begin(),scene.events.end(),[](const auto& a,const auto& b){return a.sample<b.sample;});scene.image=bundleFor(*project,slots,owners);return scene;
}
Scene externalScene(const Image& image,int rate){
    Scene scene;scene.id="real-kept-kit";scene.image=image;scene.rate=rate;scene.frames=14*rate;scene.dry=std::all_of(image.project.sound.layout.effects.begin(),image.project.sound.layout.effects.end(),[](const auto& effect){return effect.use!=airack::SlotUse::present;});
    require(image.project.mode==airack::PlaybackMode::externalMidi,"Real kit external MIDI mode required");
    unsigned assigned=0;for(unsigned bank=0;bank<4;++bank)for(unsigned pad=0;pad<16;++pad)if(image.bundle->document().slots()[bank][pad].asset){++assigned;const auto index=airack::drumParameterStart+bank*airack::drumParametersPerSlot+pad*airack::drumParametersPerPad+7;const auto note=juce::roundToInt(airack::parameterPhysicalValue(image.project.sound.values,index));const int frame=17+int(assigned-1)*rate/8;scene.events.push_back({frame,juce::MidiMessage::noteOn(1,note,juce::uint8(64+assigned%64))});scene.events.push_back({frame+rate/64,juce::MidiMessage::noteOff(1,note)});}
    require(assigned==16,"Real product kit must retain all16 assigned pads");
    double supportSeconds=0;for(unsigned bank=0;bank<4;++bank)for(unsigned pad=0;pad<16;++pad){const auto& metadata=image.bundle->document().slots()[bank][pad];if(!metadata.asset)continue;const auto* asset=image.bundle->findAsset(*metadata.asset);require(asset,"Real kit owner absent");
        const auto base=airack::drumParameterStart+bank*airack::drumParametersPerSlot+pad*airack::drumParametersPerPad;
        const auto physical=[&](unsigned field){return double(airack::parameterPhysicalValue(image.project.sound.values,base+field));};
        const auto begin=std::min(asset->frames()-1,unsigned(std::floor(physical(3)*asset->frames())));const auto end=std::clamp(unsigned(std::ceil(physical(4)*asset->frames())),begin+1,asset->frames());
        double seconds=double(end-begin)/(48000*std::exp2(physical(2)/12));if(physical(6)>0)seconds=std::min(seconds,physical(5)+physical(6));supportSeconds=std::max(supportSeconds,seconds);
    }
    require(scene.dry,"External product fixture FX tail needs its separately frozen bound");scene.frames=int(std::ceil((2+supportSeconds+1)*rate));return scene;
}
// Pinned JUCE VST3 conversion has no system Stop event. Keep that native control separate.
std::vector<Event> vst3MidiEvents(const std::vector<Event>& events){
    std::vector<Event> result;result.reserve(events.size());
    for(const auto& event:events){
        if(event.message.isMidiStop())continue;
        require(event.message.isNoteOn()||event.message.isNoteOff()||event.message.isController(),"Drum fixture has another unrepresented VST3 MIDI message");
        result.push_back(event);
    }
    return result;
}
Json nativeRawStopEvidence(const juce::AudioBuffer<float>& stopped,const juce::AudioBuffer<float>& continuing,int stopFrame,bool higher){
    const double delta=difference(stopped,continuing);require(stopFrame>=0&&stopFrame<stopped.getNumSamples(),"Drum raw Stop boundary invalid");
    int first=stopped.getNumSamples();
    for(int ch=0;ch<stopped.getNumChannels();++ch){
        require(std::memcmp(stopped.getReadPointer(ch),continuing.getReadPointer(ch),std::size_t(stopFrame)*sizeof(float))==0,"Drum raw Stop changed audio before its literal boundary");
        for(int n=stopFrame;n<stopped.getNumSamples();++n)if(stopped.getSample(ch,n)!=continuing.getSample(ch,n)){first=std::min(first,n);break;}
    }
    require(delta>0&&first<stopped.getNumSamples(),"Drum native raw Stop has no actual audio effect");
    require(higher||first==stopFrame,"Drum Normal raw Stop missed its literal sample boundary");
    auto result=jsonObject();property(result,"literal_stop_frame",stopFrame);property(result,"prefix_bits_exact",true);property(result,"first_differing_frame",first);property(result,"max_abs_effect",delta);return result;
}
SceneRendered renderScene(juce::VST3PluginFormat& format,const juce::PluginDescription& description,const Scene& scene,int prepared,int callback,bool native,const juce::MemoryBlock* opaque=nullptr,bool externalPattern=false){
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(),"Drum host lifecycle requires message thread");
    require(native||callback<=prepared,"Drum VST3 callback exceeds prepared maximum");
    struct Playhead final:juce::AudioPlayHead{juce::Optional<PositionInfo> current;juce::Optional<PositionInfo> getPosition()const override{return current;}} playhead;
    auto expected=std::make_unique<Image>(scene.image);if(externalPattern){auto project=std::make_unique<airack::ProjectDocument>(expected->project);project->mode=airack::PlaybackMode::externalMidi;*expected=reproject(*expected,*project);}
    std::unique_ptr<juce::AudioProcessor> processor;if(native)processor=std::make_unique<airack::InstrumentProcessor>();else{juce::String error;processor=format.createInstanceFromDescription(description,scene.rate,prepared,error);require(processor!=nullptr,error.toRawUTF8());}
    require(processor&&processor->getTotalNumInputChannels()==0&&processor->getTotalNumOutputChannels()==2&&processor->acceptsMidi(),"Drum loaded instrument identity/buses differ");processor->setNonRealtime(true);processor->setPlayHead(&playhead);
    const auto initial=opaque?*opaque:native?expected->owned:hostEnvelope(expected->owned);processor->setStateInformation(initial.getData(),int(initial.getSize()));
    SceneRendered result;if(!native)result.registry=verifyPatternRegistry(*static_cast<juce::AudioPluginInstance*>(processor.get()));
    const auto verify=[&]{processor->getStateInformation(result.render.hostState);verifyImage(native?decodeOwned(result.render.hostState):decodeHosted(result.render.hostState),*expected);};verify();result.initialOpaque=result.render.hostState;
    auto corrupt=expected->owned;static_cast<unsigned char*>(corrupt.getData())[0]^=1;const auto rejected=native?corrupt:hostEnvelope(corrupt);processor->setStateInformation(rejected.getData(),int(rejected.getSize()));verify();
    processor->prepareToPlay(scene.rate,prepared);result.render.latencySamples=processor->getLatencySamples();require(result.render.latencySamples==(expected->project.sound.values[758]==1?21:0),"Drum quality latency differs");verify();
    result.render.audio.setSize(2,scene.frames);result.render.audio.clear();juce::AudioBuffer<float> block(2,callback);juce::MidiBuffer midi;midi.ensureSize(65536);std::size_t event=0,clock=0;
    std::map<int,int> callbackSizes;unsigned submitted=0;
    try{
        for(int first=0;first<scene.frames;){while(clock+1<scene.clocks.size()&&scene.clocks[clock+1].sample==first)++clock;int end=std::min(first+callback,scene.frames);if(clock+1<scene.clocks.size())end=std::min(end,scene.clocks[clock+1].sample);
            if(scene.pattern&&first<2*scene.rate)end=std::min(end,(first/scene.rate+1)*scene.rate);
            const int count=end-first;require(count>0,"Drum callback failed progress");auto position=patternPosition(scene.clocks[clock],first,scene.rate);if(externalPattern){PatternClock running;position=patternPosition(running,first,scene.rate);}playhead.current=position;
            block.setSize(2,count,false,false,true);block.clear();midi.clear();
            while(event<scene.events.size()&&scene.events[event].sample<end){const auto& e=scene.events[event++];require(e.sample>=first,"Drum event schedule regressed");if(externalPattern||expected->project.mode==airack::PlaybackMode::externalMidi){midi.addEvent(e.message,e.sample-first);++submitted;}}
            processor->processBlock(block,midi);
            for(int ch=0;ch<2;++ch)result.render.audio.copyFrom(ch,first,block,ch,0,count);
            ++callbackSizes[count];
            first=end;
        }
        require(event==scene.events.size(),"Drum scheduled events not all consumed");for(const auto [count,calls]:callbackSizes){auto item=jsonObject();property(item,"frames",count);property(item,"calls",calls);result.callbacks.append(item);}auto deliveredRecord=jsonObject();property(deliveredRecord,"scheduled_events",int(event));property(deliveredRecord,"submitted_midi_messages",int(submitted));property(deliveredRecord,"submission_target",native?"Native processor MidiBuffer":"JUCE VST3 host conversion input; plugin receipt is not observed");property(deliveredRecord,"internal_pattern",expected->project.mode==airack::PlaybackMode::internalPattern);result.callbacks.append(deliveredRecord);verify();processor->releaseResources();processor->setPlayHead(nullptr);processor.reset();
    }catch(...){processor->releaseResources();processor->setPlayHead(nullptr);throw;}
    return result;
}
bool exactAudio(const juce::AudioBuffer<float>& a,const juce::AudioBuffer<float>& b){if(a.getNumSamples()!=b.getNumSamples()||a.getNumChannels()!=b.getNumChannels())return false;for(int ch=0;ch<a.getNumChannels();++ch)if(std::memcmp(a.getReadPointer(ch),b.getReadPointer(ch),std::size_t(a.getNumSamples())*sizeof(float))!=0)return false;return true;}
void literalOracle(const Scene& scene,const Rendered& rendered){
    if(scene.rate!=48000||scene.image.project.sound.values[758]!=0)return;
    if(scene.single){
        for(int frame=0;frame<scene.frames;++frame){double expected=0;for(const auto& event:scene.events)if(event.message.isNoteOn()&&event.message.getNoteNumber()==36){const int age=frame-event.sample;if(age>=0&&age<1024)expected+=(age==0?.125:age==1023?0.:.0625)*event.message.getVelocity()/127.;}
            for(int ch=0;ch<2;++ch)require(rendered.audio.getSample(ch,frame)==float(expected),"Independent Drum unity/support/velocity oracle failed");
        }
    }
    if(scene.fourBanks){
        std::array<double,2> expected{};for(unsigned bank=0;bank<4;++bank){std::array<double,2> sum{};for(unsigned pad=0;pad<16;++pad)for(unsigned ch=0;ch<2;++ch)sum[ch]+=2*double(1+bank*16+pad+(pad%2?ch:0))/65536.;for(unsigned ch=0;ch<2;++ch)expected[ch]+=float(sum[ch]);}
        for(int frame=64;frame<512;++frame)for(unsigned ch=0;ch<2;++ch)require(rendered.audio.getSample(int(ch),frame)==float(expected[ch]),"Independent128-hit unity interior sum failed");
    }
}
Json saveRender(const juce::File& output,const juce::String& stem,const SceneRendered& result,int rate,int prepared,int callback,bool native){
    auto record=jsonObject();const auto wav=output.getChildFile(stem+".wav"),state=output.getChildFile(stem+(native?".arbndl":".hoststate"));writeWav(wav,result.render.audio,rate);writeBlock(state,result.render.hostState);
    property(record,"wav",identity(wav));property(record,"returned_state",identity(state));property(record,"rate",rate);property(record,"prepared_maximum",prepared);property(record,"callback_maximum",callback);property(record,"native_reference",native);property(record,"latency",result.render.latencySamples);property(record,"callback_inventory",result.callbacks);property(record,"audio",spaceRenderAudio(result.render,rate));return record;
}
}
int liveDrumHost(const Arguments&);
int renderDrumProduct(const Arguments& args){
    if(args.drumPendingUnload)return liveDrumHost(args);
    juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;format.findAllTypesForFile(descriptions,args.plugin.getFullPathName());
    require(descriptions.size()==1&&isAirInstrument(*descriptions[0]),"Drum actual VST3 identity differs");
    drum_product::plainPath(args.output);require(!args.output.exists(),"Drum output became occupied");require(args.output.createDirectory().wasOk(),"Drum output creation failed");
    auto report=jsonObject();juce::Array<Json> rows;property(report,"scope","Actual VST3 Drum bundle/PCM/state/audio functional matrix; no capacity, warm-generation, listening or final product acceptance");
    property(report,"configuration",RACKSYNTH_CONFIGURATION);property(report,"module_before",identity(binaryIn(args.plugin)));property(report,"renderer_before",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));property(report,"sources_before",spaceRenderSources());
    property(report,"generation_actions",0);property(report,"http_fixture_constructed",false);property(report,"http_submission_callgraph","No generation entrypoint, network client or service launch is reachable from this selector branch; restore/decode/render only. Separate native request-counted recall witnesses remain required.");
    property(report,"normalization",false);property(report,"independent_native_host_limit",1.e-6);property(report,"partition_opaque_dry_limit",0);property(report,"tail_peak_limit",1.e-7);
    property(report,"block_geometry_policy","Actual VST3 setup/callback64/64,17/17,512/512,1024/1024 and opaque64; separate native512/512 versus native512/1024 exact arrays/latency. Historical invalid VST3 setup512/callback1024 remains failed evidence.");
    property(report,"midi_representation_policy","External-MIDI VST3 comparisons use identical note/on/off/CC inputs; internal-pattern scenes retain their separate clock/literal controls. Only raw system Stop0xFC is excluded from those submissions because pinned JUCE cannot represent it; the original full native timeline, exact native512/1024 law and an independent raw-Stop audio-effect control remain separate. Existing actual VST3 transport Stop/seek/restart controls are unchanged.");
    const int expectedRows=args.drumCase.isNotEmpty()?1:args.drumStateBytes.getSize()?6:24;property(report,"selected_case",args.drumCase.isEmpty()?juce::String("all"):args.drumCase);
    if(args.drumStateBytes.getSize())property(report,"input_project",identity(args.drumState));
    unsigned progress=0;bool terminalWritten=false;
    const auto finish=[&](bool success,const juce::String& error,bool terminal){property(report,"success",success);property(report,"error",error);property(report,"rows",rows);property(report,"expected_rows",expectedRows);property(report,"completed_rows",rows.size());const auto name=terminal?juce::String("drum-product-report.json"):"drum-product-progress-"+juce::String(++progress).paddedLeft('0',6)+".json";if(terminal)terminalWritten=true;writePatternJson(args.output.getChildFile(name),report);};
    try{
        for(int which=0;which<(args.drumStateBytes.getSize()?1:4);++which)for(int rate:{44100,48000,96000})for(bool higher:{false,true}){
            if(args.drumCase.isNotEmpty()&&args.drumCase!=drum_product::caseId(which,rate,higher,args.drumStateBytes.getSize()!=0))continue;
            auto scene=std::make_unique<drum_product::Scene>(args.drumStateBytes.getSize()?drum_product::externalScene(drum_product::decodeOwned(args.drumStateBytes),rate):drum_product::syntheticScene(which,rate));
            auto project=std::make_unique<airack::ProjectDocument>(scene->image.project);setGainValue(project->sound,"output.render_quality",higher?1.f:0.f);scene->image=drum_product::reproject(scene->image,*project);
            const auto stem=scene->id+"-"+juce::String(rate)+(higher?"-higher":"-normal");auto row=jsonObject();property(row,"id",stem);property(row,"literal_events",drum_product::eventRecords(scene->events));property(row,"quality",higher?"Higher":"Normal");property(row,"frames",scene->frames);property(row,"source_bundle_sha256",juce::SHA256(scene->image.owned).toHexString());
            auto comparison=std::make_unique<drum_product::Scene>(*scene);comparison->events=drum_product::vst3MidiEvents(scene->events);
            const auto rawStopCount=scene->events.size()-comparison->events.size();require(rawStopCount==(scene->id=="sixteen-pad-midi"?1u:0u),"Drum fixture raw Stop count changed");
            property(row,"vst3_representable_scene_events",drum_product::eventRecords(comparison->events));property(row,"vst3_external_midi_mode",comparison->image.project.mode==airack::PlaybackMode::externalMidi);property(row,"raw_stop_excluded_from_vst3_submission_count",int(rawStopCount));property(row,"native_timeline","Original literal_events including raw Stop; native reference and oversized native reference");
            juce::Array<Json> renders;rows.add(row);finish(false,"In progress; incomplete rows are retained",false);const auto retain=[&](const Json& render){renders.add(render);property(row,"renders",renders);finish(false,"In progress; every completed render is retained",false);};
            auto baseline=drum_product::renderScene(format,*descriptions[0],*comparison,64,64,false);retain(drum_product::saveRender(args.output,stem+"-64",baseline,rate,64,64,false));property(report,"owned_catalog",baseline.registry);
            const auto measured=measure(baseline.render.audio,std::max(0,scene->frames-rate/4));require(measured.peak<1,"Drum fixed level clipped");require(measured.tailPeak<=1.e-7,"Drum final support/tail incomplete");drum_product::literalOracle(*comparison,baseline.render);
            for(const auto geometry:std::array<std::pair<int,int>,3>{{{17,17},{512,512},{1024,1024}}}){
                const auto partition=drum_product::renderScene(format,*descriptions[0],*comparison,geometry.first,geometry.second,false);retain(drum_product::saveRender(args.output,stem+"-"+juce::String(geometry.first)+"-"+juce::String(geometry.second),partition,rate,geometry.first,geometry.second,false));
                require(drum_product::exactAudio(baseline.render.audio,partition.render.audio),"Drum legal VST3 partition bits differ");require(partition.render.latencySamples==baseline.render.latencySamples,"Drum partition latency differs");
            }
            const auto recalled=drum_product::renderScene(format,*descriptions[0],*comparison,64,64,false,&baseline.render.hostState);retain(drum_product::saveRender(args.output,stem+"-opaque64",recalled,rate,64,64,false));require(drum_product::exactAudio(baseline.render.audio,recalled.render.audio),"Drum full opaque64 recall bits differ");
            const auto native=drum_product::renderScene(format,*descriptions[0],*scene,512,512,true);retain(drum_product::saveRender(args.output,stem+"-native",native,rate,512,512,true));
            const auto nativeOversized=drum_product::renderScene(format,*descriptions[0],*scene,512,1024,true);retain(drum_product::saveRender(args.output,stem+"-native-512-1024",nativeOversized,rate,512,1024,true));require(drum_product::exactAudio(native.render.audio,nativeOversized.render.audio),"Drum native prepared512/callback1024 bits differ");require(nativeOversized.render.latencySamples==native.render.latencySamples,"Drum native oversized callback latency differs");property(row,"native_prepared512_callback1024_bits_exact",true);
            double nativeDifference=0;
            if(rawStopCount){
                const auto matched=drum_product::renderScene(format,*descriptions[0],*comparison,512,512,true);retain(drum_product::saveRender(args.output,stem+"-native-vst3-midi",matched,rate,512,512,true));
                const auto stop=std::find_if(scene->events.begin(),scene->events.end(),[](const auto& event){return event.message.isMidiStop();});
                property(row,"native_raw_stop_control",drum_product::nativeRawStopEvidence(native.render.audio,matched.render.audio,stop->sample,higher));
                const auto nativeMeasured=measure(native.render.audio,std::max(0,scene->frames-rate/4));require(nativeMeasured.peak<1&&nativeMeasured.tailPeak<=1.e-7,"Drum original native raw-Stop level/tail law failed");
                nativeDifference=difference(baseline.render.audio,matched.render.audio);
            }else nativeDifference=difference(baseline.render.audio,native.render.audio);
            require(nativeDifference<=1.e-6,"Drum actual VST3 versus matched representable-MIDI native waveform differs");
            property(row,"native_host_reference_render",stem+(rawStopCount?"-native-vst3-midi":"-native"));
            property(row,"native_host_max_abs_difference",nativeDifference);property(row,"peak",measured.peak);property(row,"tail_peak",measured.tailPeak);property(row,"literal_dry_oracle",scene->single||scene->fourBanks?rate==48000&&!higher:false);
            if(scene->pattern){
                const auto external=drum_product::renderScene(format,*descriptions[0],*scene,64,64,false,nullptr,true);retain(drum_product::saveRender(args.output,stem+"-literal-midi",external,rate,64,64,false));require(drum_product::exactAudio(baseline.render.audio,external.render.audio),"Drum internal pattern differs from independent literal MIDI timeline");
                auto zero=std::make_unique<drum_product::Scene>(*scene);auto dryProject=std::make_unique<airack::ProjectDocument>(zero->image.project);setSpaceRenderValue(dryProject->sound,scene->spaceSlot,"mix",0);zero->image=drum_product::reproject(zero->image,*dryProject);
                const auto dry=drum_product::renderScene(format,*descriptions[0],*zero,64,64,false);retain(drum_product::saveRender(args.output,stem+"-space-mix0",dry,rate,64,64,false));
                double square=0;const double mix=airack::parameterPhysicalValue(scene->image.project.sound.values,spaceRenderIndex(scene->spaceSlot,"mix"));for(int ch=0;ch<2;++ch)for(int n=0;n<scene->frames;++n){const double wet=baseline.render.audio.getSample(ch,n)-(1-mix)*dry.render.audio.getSample(ch,n);square+=wet*wet;}
                const double wetRms=std::sqrt(square/(2.*scene->frames));require(wetRms>1.e-7&&difference(baseline.render.audio,dry.render.audio)>0,"Drum actual active Space wet residual absent");require(dry.render.latencySamples==baseline.render.latencySamples,"Drum Space added latency");property(row,"space_wet_residual_rms",wetRms);
                auto unrelated=std::make_unique<drum_product::Scene>(*zero);auto unrelatedProject=std::make_unique<airack::ProjectDocument>(zero->image.project);setGainValue(unrelatedProject->sound,"synth.1.enabled",0);unrelated->image=drum_product::reproject(unrelated->image,*unrelatedProject);
                const auto melodic=drum_product::renderScene(format,*descriptions[0],*unrelated,64,64,false);retain(drum_product::saveRender(args.output,stem+"-unrelated-melodic",melodic,rate,64,64,false));require(std::memcmp(dry.render.audio.getReadPointer(1),melodic.render.audio.getReadPointer(1),std::size_t(scene->frames)*sizeof(float))==0&&melodic.render.audio.getMagnitude(1,0,scene->frames)>1.e-8,"Drum changed the separate nonzero melodic right-channel control");
                juce::Array<Json> clockControls;const auto controls=musicalScenes(rate,false);
                for(const auto& literal:controls)if(literal.name=="missing-ppq"||literal.name=="seek-stop-resume"){
                    auto control=std::make_unique<drum_product::Scene>(*scene);auto controlProject=std::make_unique<airack::ProjectDocument>(scene->image.project);controlProject->pattern=literal.project.pattern;setGainValue(controlProject->sound,"synth.2.enabled",0);control->image=drum_product::reproject(control->image,*controlProject);control->clocks=literal.clocks;control->events=literal.expected;
                    const auto actualClock=drum_product::renderScene(format,*descriptions[0],*control,64,64,false),literalMidi=drum_product::renderScene(format,*descriptions[0],*control,64,64,false,nullptr,true);
                    retain(drum_product::saveRender(args.output,stem+"-"+literal.name,actualClock,rate,64,64,false));retain(drum_product::saveRender(args.output,stem+"-"+literal.name+"-literal",literalMidi,rate,64,64,false));require(drum_product::exactAudio(actualClock.render.audio,literalMidi.render.audio),"Drum missing-clock/seek/Stop/restart differs from independent literal MIDI");auto clockRecord=jsonObject();property(clockRecord,"id",literal.name);property(clockRecord,"events",drum_product::eventRecords(literal.expected));property(clockRecord,"clocks",clockJson(literal));property(clockRecord,"bits_exact",true);clockControls.add(clockRecord);
                }
                property(row,"clock_controls",clockControls);property(row,"unrelated_melodic_right_bits_exact",true);property(row,"fractional_note_start_beats",scene->image.project.pattern.notes[0].startBeats);
            }
            property(row,"renders",renders);property(row,"success",true);finish(false,"Remaining rows incomplete",false);std::cout<<"DRUM_PRODUCT_ROW "<<stem<<" native_difference="<<nativeDifference<<" peak="<<measured.peak<<" tail="<<measured.tailPeak<<std::endl;
        }
        require(rows.size()==expectedRows,"Drum matrix row count incomplete");property(report,"module_after",identity(binaryIn(args.plugin)));property(report,"renderer_after",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));property(report,"sources_after",spaceRenderSources());
        require(report["module_before"]["sha256"].toString()==report["module_after"]["sha256"].toString()&&report["renderer_before"]["sha256"].toString()==report["renderer_after"]["sha256"].toString()&&juce::JSON::toString(report["sources_before"],false)==juce::JSON::toString(report["sources_after"],false),"Drum source/module/renderer changed during matrix");
        if(args.drumStateBytes.getSize())require(juce::SHA256(args.drumState)==juce::SHA256(args.drumStateBytes),"Drum external source changed during matrix");finish(true,{},true);std::cout<<"SUCCESS Drum actual VST3 functional matrix rows="<<rows.size()<<"; final product acceptance remains separate"<<std::endl;return 0;
    }catch(const std::exception& error){if(!terminalWritten)finish(false,error.what(),true);throw;}
}
// Actual owned loaded-editor actions and module lifetime validation.
namespace drum_product {
std::string quote(std::string_view text){return juce::JSON::toString(juce::var(juce::String::fromUTF8(text.data(),int(text.size()))),true).toStdString();}
bool canonicalUuid(std::string_view text){if(text.size()!=36)return false;const juce::Uuid id(juce::String::fromUTF8(text.data(),int(text.size())));return !id.isNull()&&id.toDashedString().toStdString()==text;}
bool decimalValue(std::string_view text,std::uint64_t& value){if(text.empty()||(text.size()>1&&text[0]=='0'))return false;const auto converted=std::from_chars(text.data(),text.data()+text.size(),value);return converted.ec==std::errc()&&converted.ptr==text.data()+text.size()&&value<=std::uint64_t(INT64_MAX);}
// Independent frozen recipe literal. Never use buildComfyAudioSubmit as its own validator.
std::string frozenSubmit(std::string_view attempt,std::string_view client,std::string_view prompt,std::string_view seed,std::string_view prefix){
    return "{\"prompt_id\":"+quote(attempt)+",\"client_id\":"+quote(client)+",\"prompt\":{"+
    R"("1":{"class_type":"CheckpointLoaderSimple","inputs":{"ckpt_name":"stable_audio_3_small_sfx.safetensors"}},"2":{"class_type":"CLIPLoader","inputs":{"clip_name":"t5gemma_b_b_ul2.safetensors","type":"stable_audio","device":"default"}},"3":{"class_type":"CLIPTextEncode","inputs":{"clip":["2",0],"text":)"+quote(prompt)+
    R"(}},"8":{"class_type":"CLIPTextEncode","inputs":{"clip":["2",0],"text":""}},"4":{"class_type":"EmptyLatentAudio","inputs":{"seconds":10.0,"batch_size":1}},"5":{"class_type":"KSampler","inputs":{"model":["1",0],"positive":["3",0],"negative":["8",0],"latent_image":["4",0],"seed":)"+std::string(seed)+
    R"(,"steps":8,"cfg":1.0,"sampler_name":"lcm","scheduler":"simple","denoise":1.0}},"6":{"class_type":"VAEDecodeAudio","inputs":{"samples":["5",0],"vae":["1",2]}},"7":{"class_type":"SaveAudioAdvanced","inputs":{"audio":["6",0],"filename_prefix":)"+quote(prefix)+R"(,"format":"flac"}}}})";
}
struct HostProtocol {
    const std::string prompt="isolated dry single drum hit, hosted product lifecycle fixture";
    std::string flac,runtime,client,attempt,folder,filename;std::uint64_t request=0;
    std::array<std::uint64_t,3> seeds{};std::set<std::string> clients,attempts;unsigned ordinal=0,stage=0,batch=0;
    std::atomic_uint operations{0};std::atomic_bool pending{false},released{false},advanceCancelled{false},gateTimeout{false};
    std::mutex gateMutex;std::condition_variable gateWake;
    void release(){released=true;gateWake.notify_all();}
    ~HostProtocol(){release();}
    comfy_fixture::OwnedHostReply reply(const comfy_fixture::Capture& capture){
        if(!capture.complete||capture.raw.size()>49152||capture.body.size()>16384)return {};
        if(capture.method=="POST"&&capture.target=="/prompt"){
            if(stage!=0){if(!advanceCancelled.exchange(false))return {};stage=0;ordinal=0;++batch;}
            if(ordinal>=3)return {};juce::var decoded;if(!airack::ai::parseStrictJson(capture.body,decoded,16384)||!decoded.isObject())return {};
            const auto freshAttempt=decoded["prompt_id"].toString().toStdString(),freshClient=decoded["client_id"].toString().toStdString();
            if(!canonicalUuid(freshAttempt)||!canonicalUuid(freshClient)||attempts.contains(freshAttempt))return {};
            const auto prefix=decoded["prompt"]["7"]["inputs"]["filename_prefix"].toString().toStdString();
            const auto parts=juce::StringArray::fromTokens(juce::String(prefix),"/","");if(parts.size()!=5||parts[0]!="airack"||!canonicalUuid(parts[1].toStdString())||parts[3].toStdString()!=freshAttempt||!parts[4].startsWith("seed-"))return {};
            std::uint64_t freshRequest=0,seed=0;if(!decimalValue(parts[2].toStdString(),freshRequest)||!freshRequest||!decimalValue(parts[4].substring(5).toStdString(),seed))return {};
            const auto seedText=std::to_string(seed);if(capture.body!=frozenSubmit(freshAttempt,freshClient,prompt,seedText,prefix))return {};
            const auto freshRuntime=parts[1].toStdString();
            if(!ordinal){if(clients.contains(freshClient)||(!runtime.empty()&&(freshRuntime!=runtime||freshRequest<=request)))return {};client=freshClient;runtime=freshRuntime;request=freshRequest;clients.insert(client);}
            else if(freshClient!=client||freshRuntime!=runtime||freshRequest!=request)return {};
            for(unsigned n=0;n<ordinal;++n)if(seeds[n]==seed)return {};
            seeds[ordinal]=seed;attempt=freshAttempt;attempts.insert(attempt);folder="airack/"+runtime+"/"+std::to_string(request)+"/"+attempt;filename="seed-"+seedText+"_00001.flac";stage=1;++operations;
            if(batch){released=false;pending=true;std::unique_lock lock(gateMutex);if(!gateWake.wait_for(lock,std::chrono::milliseconds(1200),[&]{return released.load();}))gateTimeout=true;pending=false;}
            return {true,comfy_fixture::response("{\"prompt_id\":"+quote(attempt)+",\"number\":0,\"node_errors\":{}}")};
        }
        if(capture.method!="GET"||!capture.body.empty())return {};
        if(stage==1&&capture.target=="/history/"+attempt){stage=2;++operations;return {true,comfy_fixture::response("{"+quote(attempt)+":{\"status\":{\"completed\":true,\"status_str\":\"success\"},\"outputs\":{\"7\":{\"audio\":[{\"filename\":"+quote(filename)+",\"subfolder\":"+quote(folder)+",\"type\":\"output\"}]}}}}")};}
        std::string encoded;for(char c:folder)encoded+=c=='/'?"%2F":std::string(1,c);
        if(stage==2&&capture.target=="/view?filename="+filename+"&subfolder="+encoded+"&type=output"){stage=0;++ordinal;++operations;if(ordinal==3){ordinal=0;++batch;}return {true,comfy_fixture::response(flac,"audio/flac")};}
        return {};
    }
};
// Ordinary Windows UI Automation rooted only at this host's actual editor HWND.
// A short-lived MTA client runs separately while the existing host message thread pumps.
// No product pointer, private interface, companion action or second host is exposed.
const wchar_t* actionTitle(const juce::String& id){
    if(id=="face.programmer")return L"PAD FILE  /  PULL";
    if(id=="programmer.generate_cancel")return L"GENERATE";
    if(id=="drum.generation.prompt")return L"One-hit prompt, 1 to 2048 UTF-8 bytes";
    if(id=="drum.generation.generate")return L"Generate three candidates for the captured pad";
    if(id=="drum.generation.candidate.2")return L"PAD 01 candidate 2";
    if(id=="drum.source.start")return L"Source START inclusive, unsigned source frame";
    if(id=="drum.source.end")return L"Source END exclusive, unsigned source frame";
    if(id=="drum.source.gain")return L"Gain multiplier greater than zero and at most sixteen";
    if(id=="drum.source.name")return L"Kept sample name, maximum 64 UTF-8 bytes";
    if(id=="drum.source.prepare")return L"Prepare selected source";
    if(id=="drum.source.audition")return L"Audition prepared source before pad trim voice and effects";
    if(id=="drum.source.keep")return L"Keep captured pad";
    throw std::runtime_error("Unbound hosted accessibility action");
}
struct UiState {bool found=false,enabled=false,offscreen=true;juce::String text,automationId;};
template<class Predicate>void pumpUntil(Predicate predicate,const char* reason,int milliseconds=4000){const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);while(!predicate()&&std::chrono::steady_clock::now()<deadline){personalityPump();juce::Thread::sleep(1);}require(predicate(),reason);}
enum class UiOperation{query,invoke,text,readText};
UiState ownedAction(juce::AudioProcessorEditor& editor,const juce::String& id,UiOperation operation,const juce::String& value={}){
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(),"Owned UIA scheduling requires host message thread");auto* peer=editor.getPeer();require(peer,"Actual host editor peer missing");const auto hwnd=static_cast<HWND>(peer->getNativeHandle());DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);require(pid==GetCurrentProcessId(),"Owned UIA root is foreign");const std::wstring title(actionTitle(id));
    auto future=std::async(std::launch::async,[hwnd,title,operation,value]{
        const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);require(SUCCEEDED(initialized),"Owned UIA MTA initialization failed");struct ComScope{~ComScope(){CoUninitialize();}} scope;
        Microsoft::WRL::ComPtr<IUIAutomation> automation;require(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(automation.GetAddressOf()))),"Owned UIA client creation failed");
        Microsoft::WRL::ComPtr<IUIAutomation2> limits;if(SUCCEEDED(automation.As(&limits))){require(SUCCEEDED(limits->put_ConnectionTimeout(1000))&&SUCCEEDED(limits->put_TransactionTimeout(1000)),"Owned UIA timeout setup failed");}
        std::vector<HWND> windows{hwnd};EnumChildWindows(hwnd,[](HWND child,LPARAM raw)->BOOL{auto& list=*reinterpret_cast<std::vector<HWND>*>(raw);DWORD owner=0;GetWindowThreadProcessId(child,&owner);if(owner==GetCurrentProcessId())list.push_back(child);return list.size()<64;},reinterpret_cast<LPARAM>(&windows));require(windows.size()<64,"Owned UIA child window bound exceeded");
        VARIANT name;VariantInit(&name);name.vt=VT_BSTR;name.bstrVal=SysAllocString(title.c_str());require(name.bstrVal,"Owned UIA title allocation failed");Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;const auto conditionResult=automation->CreatePropertyCondition(UIA_NamePropertyId,name,&condition);VariantClear(&name);require(SUCCEEDED(conditionResult),"Owned UIA name condition failed");
        Microsoft::WRL::ComPtr<IUIAutomationElement> selected;
        for(auto window:windows){Microsoft::WRL::ComPtr<IUIAutomationElement> root;if(FAILED(automation->ElementFromHandle(window,&root)))continue;Microsoft::WRL::ComPtr<IUIAutomationElementArray> matches;if(FAILED(root->FindAll(TreeScope_Descendants,condition.Get(),&matches)))continue;int count=0;require(SUCCEEDED(matches->get_Length(&count))&&count<=32,"Owned UIA match bound exceeded");
            for(int n=0;n<count;++n){Microsoft::WRL::ComPtr<IUIAutomationElement> element;require(SUCCEEDED(matches->GetElement(n,&element)),"Owned UIA element retrieval failed");int owner=0;BOOL offscreen=TRUE;require(SUCCEEDED(element->get_CurrentProcessId(&owner))&&owner==int(GetCurrentProcessId()),"Owned UIA result is foreign");if(FAILED(element->get_CurrentIsOffscreen(&offscreen))||offscreen)continue;if(selected){BOOL same=FALSE;require(SUCCEEDED(automation->CompareElements(selected.Get(),element.Get(),&same))&&same,"Owned UIA action is ambiguous");}else selected=std::move(element);}
        }
        UiState state;if(!selected)return state;state.found=true;BOOL enabled=FALSE;require(SUCCEEDED(selected->get_CurrentIsEnabled(&enabled)),"Owned UIA enabled readback failed");state.enabled=enabled;state.offscreen=false;BSTR automationId=nullptr;if(SUCCEEDED(selected->get_CurrentAutomationId(&automationId))){state.automationId=juce::String(automationId);SysFreeString(automationId);}
        if(operation==UiOperation::query)return state;require(state.enabled,"Actual owned accessible action is disabled");
        if(operation==UiOperation::invoke){Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> invoke;require(SUCCEEDED(selected->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(invoke.GetAddressOf())))&&SUCCEEDED(invoke->Invoke()),"Actual owned UIA Invoke failed");return state;}
        Microsoft::WRL::ComPtr<IUIAutomationTextPattern> text;require(SUCCEEDED(selected->GetCurrentPatternAs(UIA_TextPatternId,IID_PPV_ARGS(text.GetAddressOf()))),"Actual editable TextPattern unavailable");Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;require(SUCCEEDED(text->get_DocumentRange(&range)),"Actual editable document range unavailable");
        if(operation==UiOperation::text){require(!value.isEmpty()&&SUCCEEDED(selected->SetFocus())&&SUCCEEDED(range->Select()),"Actual owned text focus/selection failed");GUITHREADINFO info{};info.cbSize=sizeof(info);DWORD owner=0;const auto thread=GetWindowThreadProcessId(hwnd,&owner);require(owner==GetCurrentProcessId()&&GetGUIThreadInfo(thread,&info)&&info.hwndFocus&&(info.hwndFocus==hwnd||IsChild(hwnd,info.hwndFocus)),"Actual text focus HWND is outside the owned editor");DWORD focusedOwner=0;GetWindowThreadProcessId(info.hwndFocus,&focusedOwner);require(focusedOwner==GetCurrentProcessId(),"Actual text focus is foreign");for(auto character:value)require(PostMessageW(info.hwndFocus,WM_CHAR,WPARAM(character),1)!=0,"Actual owned text delivery failed");}
        BSTR content=nullptr;require(SUCCEEDED(range->GetText(-1,&content)),"Actual editable text readback failed");state.text=juce::String(content);SysFreeString(content);return state;
    });
    // Always join this owned UIA call before editor/module retirement. Abnormal COM
    // stalls remain unresolved failures; no detach, forced termination or live unload.
    while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){personalityPump();juce::Thread::sleep(1);}return future.get();
}
void press(juce::AudioProcessorEditor& editor,const juce::String& id){const auto state=ownedAction(editor,id,UiOperation::invoke);require(state.found,"Actual owned accessible action missing");personalityPump();std::cout<<"DRUM_HOST_UIA invoke="<<id<<" automation_id="<<state.automationId<<std::endl;}
juce::String fieldText(juce::AudioProcessorEditor& editor,const char* id){const auto state=ownedAction(editor,id,UiOperation::readText);require(state.found,"Actual owned text field missing");return state.text;}
void postedText(juce::AudioProcessorEditor& editor,const char* id,const juce::String& value){require(ownedAction(editor,id,UiOperation::text,value).found,"Actual owned editable field missing");pumpUntil([&]{return fieldText(editor,id)==value;},"Actual posted text readback differs");std::cout<<"DRUM_HOST_UIA text="<<id<<" readback="<<value<<std::endl;}
void snapshot(juce::AudioProcessorEditor& editor,const juce::File& output,const char* name){
    auto* peer=editor.getPeer();require(peer,"Actual capture peer unavailable");const auto hwnd=static_cast<HWND>(peer->getNativeHandle());DWORD owner=0;GetWindowThreadProcessId(hwnd,&owner);require(owner==GetCurrentProcessId(),"Actual capture peer is foreign");RECT rect{};require(GetClientRect(hwnd,&rect)!=0,"Actual capture client rectangle unavailable");const int width=rect.right-rect.left,height=rect.bottom-rect.top;require(width>0&&height>0&&std::int64_t(width)*height<=16000000,"Actual capture geometry outside bound");
    BITMAPINFO format{};format.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);format.bmiHeader.biWidth=width;format.bmiHeader.biHeight=-height;format.bmiHeader.biPlanes=1;format.bmiHeader.biBitCount=32;format.bmiHeader.biCompression=BI_RGB;
    struct Bitmap{HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;~Bitmap(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}} bitmap;require(bitmap.dc,"Actual capture DC failed");void* pixels=nullptr;bitmap.bitmap=CreateDIBSection(bitmap.dc,&format,DIB_RGB_COLORS,&pixels,nullptr,0);require(bitmap.bitmap&&pixels,"Actual capture bitmap failed");bitmap.previous=SelectObject(bitmap.dc,bitmap.bitmap);require(PrintWindow(hwnd,bitmap.dc,PW_CLIENTONLY|2)!=0,"Actual native child-window capture failed");
    juce::Image image(juce::Image::ARGB,width,height,false);juce::Image::BitmapData destination(image,juce::Image::BitmapData::writeOnly);const auto* source=static_cast<const std::uint32_t*>(pixels);for(int y=0;y<height;++y){auto* line=reinterpret_cast<std::uint32_t*>(destination.getLinePointer(y));for(int x=0;x<width;++x)line[x]=source[std::size_t(y)*width+x]|0xff000000u;}
    juce::FileOutputStream stream(output.getChildFile(juce::String(name)+".png"));require(stream.openedOk()&&juce::PNGImageFormat().writeImageToStream(image,stream),"Actual native host capture write failed");
}

}
int liveDrumHost(const Arguments& args){
    using namespace drum_product;
    require(args.drumPendingUnload&&args.drumStateBytes.getSize()==0,"Live fixture requires explicit mutually exclusive opt-in");
    plainPath(args.output);require(!args.output.exists(),"Owned host output became occupied");require(args.output.createDirectory().wasOk(),"Owned host output creation failed");
    auto report=jsonObject();property(report,"scope","Real owned loaded VST3 editor Generate/Keep/save/reopen/pending cancel/editor close/instance destruction/ordinary module unload; deterministic fixture only");property(report,"explicit_live_opt_in",true);property(report,"fixed_port",8189);property(report,"listener_seconds_per_phase",6);property(report,"owned_phase_limit",4);property(report,"aggregate_request_limit",64);property(report,"foreign_contact",false);property(report,"module_before",identity(binaryIn(args.plugin)));property(report,"owned_fixture_header",identity(juce::File(RACKSYNTH_WORKSPACE).getChildFile("tests/native/ComfyHttpFixture.h")));
    unsigned progress=0;bool terminalWritten=false;
    const auto finish=[&](bool success,const juce::String& error,bool terminal){property(report,"success",success);property(report,"error",error);const auto name=terminal?juce::String("drum-product-report.json"):"drum-product-progress-"+juce::String(++progress).paddedLeft('0',6)+".json";if(terminal)terminalWritten=true;writePatternJson(args.output.getChildFile(name),report);};finish(false,"Setup incomplete",false);
    HostProtocol protocol;
    try{
#if JUCE_WINDOWS
        struct Winsock{WSADATA data{};Winsock(){require(WSAStartup(MAKEWORD(2,2),&data)==0,"Owned fixture Winsock setup failed");}~Winsock(){WSACleanup();}} winsock;
#else
        throw std::runtime_error("Owned hosted fixture requires Windows");
#endif
        juce::MemoryBlock flac;{
            std::unique_ptr<juce::OutputStream> stream=std::make_unique<juce::MemoryOutputStream>(flac,false);juce::FlacAudioFormat codec;auto writer=codec.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(16));require(writer!=nullptr,"Owned valid FLAC writer failed");
            juce::AudioBuffer<float> pcm(2,442368);for(int n=0;n<442368;++n){pcm.setSample(0,n,.125f);pcm.setSample(1,n,-.0625f);}require(writer->writeFromAudioSampleBuffer(pcm,0,442368),"Owned valid FLAC write failed");writer.reset();
        }
        require(flac.getSize()>42&&flac.getSize()<=16u*1024*1024,"Owned FLAC bound invalid");protocol.flac.assign(static_cast<const char*>(flac.getData()),flac.getSize());writeBlock(args.output.getChildFile("owned-response.flac"),flac);
        auto format=std::make_unique<juce::VST3PluginFormat>();juce::OwnedArray<juce::PluginDescription> descriptions;format->findAllTypesForFile(descriptions,args.plugin.getFullPathName());require(descriptions.size()==1&&isAirInstrument(*descriptions[0]),"Owned actual VST3 identity differs");
        auto original=std::make_unique<Scene>(syntheticScene(0,48000));juce::String loadError;auto processor=format->createInstanceFromDescription(*descriptions[0],48000,512,loadError);require(processor!=nullptr,loadError.toRawUTF8());processor->setNonRealtime(true);
        const auto initial=hostEnvelope(original->image.owned);processor->setStateInformation(initial.getData(),int(initial.getSize()));processor->prepareToPlay(48000,512);require(processor->getLatencySamples()==0,"Owned Normal latency differs");property(report,"owned_catalog",verifyPatternRegistry(*processor));
        juce::MemoryBlock opaque;processor->getStateInformation(opaque);verifyImage(decodeHosted(opaque),original->image);
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        const auto openEditor=[&]{editor.reset(processor->createEditorIfNeeded());require(editor!=nullptr,"Actual owned VST3 editor unavailable");editor->addToDesktop(0);editor->setSize(2200,1600);editor->setVisible(true);personalityPump();
#if JUCE_WINDOWS
            auto* peer=editor->getPeer();require(peer,"Actual owned editor has no peer");DWORD pid=0;GetWindowThreadProcessId(static_cast<HWND>(peer->getNativeHandle()),&pid);require(pid==GetCurrentProcessId(),"Actual editor peer is foreign");property(report,"owned_pid",int(pid));
#endif
            press(*editor,"face.programmer");
        };
        const auto openGenerate=[&]{unsigned opens=0;auto last=std::chrono::steady_clock::time_point{};pumpUntil([&]{const auto prompt=ownedAction(*editor,"drum.generation.prompt",UiOperation::query);if(prompt.found&&prompt.enabled)return true;const auto now=std::chrono::steady_clock::now();if(now-last>=std::chrono::milliseconds(50)&&opens<20){press(*editor,"programmer.generate_cancel");last=now;++opens;}return false;},"Actual owned generation popup stayed busy/draining",1500);std::cout<<"DRUM_HOST_UIA popup_open_actions="<<opens<<" no_submit_retry=1"<<std::endl;postedText(*editor,"drum.generation.prompt",protocol.prompt);};
        openEditor();snapshot(*editor,args.output,"editor-before-generation");openGenerate();
        comfy_fixture::FixtureOptions options;options.fixedProtocol8189=true;options.ownedHostReply=[&](const auto& capture){return protocol.reply(capture);};
        auto fixture=std::make_unique<comfy_fixture::NativeComfyFixture>("","","","",std::move(options));require(fixture->valid()&&fixture->port()==8189,"Exclusive owned8189 setup failed; no foreign contact or fallback permitted");property(report,"exclusive_fixture_owned",true);finish(false,"Owned fixture admitted; workflow incomplete",false);
        press(*editor,"drum.generation.generate");pumpUntil([&]{const auto c=ownedAction(*editor,"drum.generation.candidate.2",UiOperation::query);return protocol.operations.load()==9&&c.found&&c.enabled;},"Actual loaded generation did not complete three bound candidates");
        fixture->join();require(fixture->captures().size()==9&&protocol.operations==9,"Owned initial batch did not actually drain");
        const auto keptSeed=protocol.seeds[1];press(*editor,"drum.generation.candidate.2");pumpUntil([&]{return fieldText(*editor,"drum.source.end")=="442368";},"Actual candidate source metadata did not inspect");
        postedText(*editor,"drum.source.start","100");postedText(*editor,"drum.source.end","4510");postedText(*editor,"drum.source.gain","0.5");postedText(*editor,"drum.source.name","Hosted owned hit");press(*editor,"drum.source.prepare");
        pumpUntil([&]{const auto keep=ownedAction(*editor,"drum.source.keep",UiOperation::query);return keep.found&&keep.enabled;},"Actual loaded explicit source prepare failed");snapshot(*editor,args.output,"editor-prepared-hit");press(*editor,"drum.source.audition");
        juce::AudioBuffer<float> preview(2,512);preview.clear();juce::MidiBuffer empty;processor->processBlock(preview,empty);require(preview.getMagnitude(0,512)>0,"Actual loaded canonical audition silent");press(*editor,"drum.source.keep");
        pumpUntil([&]{return !ownedAction(*editor,"drum.source.keep",UiOperation::query).found;},"Actual loaded Keep did not close its popup");
        processor->getStateInformation(opaque);auto kept=std::make_unique<Image>(decodeHosted(opaque));
        std::vector<float> pcm(4800*2);for(std::size_t n=0;n<4800;++n){pcm[n*2]=.0625f;pcm[n*2+1]=-.03125f;}AssetPtr asset;AssetError assetError{};require(makeSampleAsset(pcm,2,asset,assetError),"Independent kept PCM construction failed");
        DrumSlots slots=original->image.bundle->document().slots();auto& metadata=slots[0][0];metadata.asset=asset->id();metadata.name="Hosted owned hit";SourceProvenance origin;origin.kind=SourceKind::generated;origin.sourceRate=44100;origin.sourceChannels=2;origin.sourceFrames=442368;origin.begin=100;origin.end=4510;origin.gain=.5f;origin.seed=keptSeed;origin.generator="Stable Audio 3 Small SFX";origin.condition="stable-audio3-small-sfx-lcm8-flac-v1;execution=unverified";
        const auto fileHash=juce::SHA256(flac).getRawData(),promptHash=juce::SHA256(protocol.prompt.data(),protocol.prompt.size()).getRawData();std::memcpy(origin.sourceFileId.data(),fileHash.getData(),32);std::memcpy(origin.promptId.data(),promptHash.getData(),32);metadata.source=origin;
        const std::array<AssetPtr,1> owners{asset};auto expected=std::make_unique<Image>(bundleFor(original->image.project,slots,owners));verifyImage(*kept,*expected);
        writeBlock(args.output.getChildFile("actual-kept.hoststate"),opaque);writeBlock(args.output.getChildFile("actual-kept-project.arbndl"),kept->owned);property(report,"actual_kept_seed",juce::int64(keptSeed));property(report,"kept_project",identity(args.output.getChildFile("actual-kept-project.arbndl")));property(report,"independent_canonical_pcm",juce::String(assetIdText(asset->id())));snapshot(*editor,args.output,"editor-after-keep");
        // Processing acknowledges stopped preview storage while its existing control pump drains the source job.
        for(int n=0;n<8;++n){preview.clear();processor->processBlock(preview,empty);personalityPump();}
        openGenerate();const auto readyCaptureStart=std::chrono::steady_clock::now();snapshot(*editor,args.output,"editor-ready-before-pending-close");const auto readyCaptureMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-readyCaptureStart).count();property(report,"ready_before_pending_close_capture_ms",readyCaptureMs);std::cout<<"DRUM_READY_CAPTURE milliseconds="<<readyCaptureMs<<" before_pending_request=1"<<std::endl;
        require(fixture->rearmOwned(),"Owned pending-editor fixture phase refused");press(*editor,"drum.generation.generate");pumpUntil([&]{return protocol.pending.load();},"Actual owned second request did not reach the pending reply gate",1000);
        const auto editorCloseStart=std::chrono::steady_clock::now();editor.reset();personalityPump();const auto editorCloseMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-editorCloseStart).count();property(report,"pending_editor_close_critical_ms",editorCloseMs);std::cout<<"DRUM_PENDING_EDITOR_CLOSE critical_ms="<<editorCloseMs<<" still_pending="<<protocol.pending.load()<<" gate_timeout="<<protocol.gateTimeout.load()<<std::endl;
        require(protocol.pending&&!protocol.gateTimeout,"Pending editor-close gate expired before cancellation");protocol.advanceCancelled=true;protocol.release();pumpUntil([&]{return !protocol.pending.load();},"Owned reply gate did not release",1000);property(report,"actual_pending_editor_close",true);
        fixture->join();require(fixture->captures().size()==10,"Owned pending-editor reply did not actually drain");processor->getStateInformation(kept->owned);verifyImage(decodeHosted(kept->owned),*expected);
        openEditor();openGenerate();require(fixture->rearmOwned(),"Owned pending-instance fixture phase refused");press(*editor,"drum.generation.generate");pumpUntil([&]{return protocol.pending.load();},"Actual owned third request did not reach pending reply gate",1000);
#if JUCE_WINDOWS
        const auto modulePath=binaryIn(args.plugin).getFullPathName();require(GetModuleHandleW(modulePath.toWideCharPointer())!=nullptr,"Actual loaded DLL module identity unavailable");
#endif
        const auto instanceCriticalStart=std::chrono::steady_clock::now();editor.reset();personalityPump();const auto teardownStart=std::chrono::steady_clock::now();processor->releaseResources();processor.reset();format.reset();const auto teardownMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-teardownStart).count();const auto ownerReleaseCriticalMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-instanceCriticalStart).count();property(report,"pending_instance_release_critical_ms",ownerReleaseCriticalMs);property(report,"pending_instance_critical_ms",ownerReleaseCriticalMs);
        property(report,"healthy_teardown_ms",teardownMs);
        require(protocol.pending&&!protocol.gateTimeout,"Owned pending gate expired before actual instance/module drain");
#if JUCE_WINDOWS
        const auto releasedModule=GetModuleHandleW(modulePath.toWideCharPointer());property(report,"ordinary_host_owners_released",!editor&&!processor&&!format);property(report,"module_present_after_release",releasedModule!=nullptr);property(report,"module_after_release_expected_path",modulePath);
        if(releasedModule){std::vector<wchar_t> loadedPath(32768);const auto length=GetModuleFileNameW(releasedModule,loadedPath.data(),DWORD(loadedPath.size()));const auto pathError=length?0:GetLastError();property(report,"module_after_release_actual_path",length?juce::String(loadedPath.data(),int(length)):juce::String{});property(report,"module_after_release_path_error",juce::int64(pathError));}
        std::cout<<"DRUM_PENDING_INSTANCE teardown_ms="<<teardownMs<<" module_present="<<(releasedModule!=nullptr)<<" editor_owner="<<bool(editor)<<" processor_owner="<<bool(processor)<<" format_owner="<<bool(format)<<std::endl;
        const auto unloadPumpStart=std::chrono::steady_clock::now();auto drainedModule=releasedModule;std::uint64_t unloadPumpIterations=0;
        while(drainedModule!=nullptr&&protocol.pending&&!protocol.gateTimeout){personalityPump();std::this_thread::sleep_for(std::chrono::milliseconds(1));++unloadPumpIterations;drainedModule=GetModuleHandleW(modulePath.toWideCharPointer());}
        const auto unloadPumpMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-unloadPumpStart).count();
        property(report,"module_unload_pump_ms",unloadPumpMs);property(report,"module_unload_pump_iterations",juce::int64(unloadPumpIterations));property(report,"module_present_after_unload_pump",drainedModule!=nullptr);property(report,"pending_after_unload_pump",protocol.pending.load());property(report,"gate_timeout_after_unload_pump",protocol.gateTimeout.load());property(report,"pending_instance_critical_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-instanceCriticalStart).count());
        std::cout<<"DRUM_PENDING_MODULE_DRAIN pump_ms="<<unloadPumpMs<<" module_present="<<(drainedModule!=nullptr)<<" still_pending="<<protocol.pending.load()<<" gate_timeout="<<protocol.gateTimeout.load()<<std::endl;
        require(protocol.pending&&!protocol.gateTimeout,"Owned pending gate expired during ordinary module-unload processing");
        require(drainedModule==nullptr,"Actual VST3 module remained loaded after ordinary owners released");
#endif
        property(report,"actual_pending_instance_and_module_unload",true);protocol.release();pumpUntil([&]{return !protocol.pending.load()&&fixture->captures().size()==11;},"Owned pending request did not drain into captured history",1000);require(!fixture->exited(),"Owned listener expired before recall counter control");
        fixture->join();require(!protocol.gateTimeout,"Owned pending gate expired before its reply actually drained");require(fixture->rearmOwned(),"Owned zero-HTTP recall fixture phase refused");
        juce::VST3PluginFormat recallFormat;auto reference=std::make_unique<Scene>();reference->id="actual-hosted-kept";reference->image=*expected;reference->rate=48000;reference->frames=48000;reference->events={{17,juce::MidiMessage::noteOn(1,36,juce::uint8(100))}};
        const auto fresh=renderScene(recallFormat,*descriptions[0],*reference,64,64,false,&opaque);const auto native=renderScene(recallFormat,*descriptions[0],*reference,512,512,true);require(difference(fresh.render.audio,native.render.audio)<=1.e-6,"Actual kept opaque reopen changed the full native musical waveform");property(report,"fresh_opaque_recall",saveRender(args.output,"actual-kept-opaque-reopen",fresh,48000,64,64,false));property(report,"native_kept_reference",saveRender(args.output,"actual-kept-native",native,48000,512,512,true));require(!fixture->exited(),"Owned listener expired during actual recall counter control");fixture->join();
        const auto captures=fixture->captures();require(captures.size()==11&&fixture->requestCount()==11&&protocol.operations==11,"Actual opaque recall initiated HTTP or owned request inventory differs");juce::Array<Json> requests;for(const auto& capture:captures){require(capture.complete&&capture.matched,"Actual owned host accepted a foreign/malformed request");auto row=jsonObject();property(row,"method",juce::String(capture.method));property(row,"route",juce::String(capture.target));property(row,"body_sha256",juce::SHA256(capture.body.data(),capture.body.size()).toHexString());requests.add(row);}property(report,"requests",requests);property(report,"actual_recall_zero_http_with_owned_listener_active",true);fixture.reset();
        property(report,"module_after",identity(binaryIn(args.plugin)));require(report["module_before"]["sha256"].toString()==report["module_after"]["sha256"].toString(),"Actual loaded module changed during owned witness");finish(true,{},true);std::cout<<"SUCCESS real loaded Drum editor Keep/opaque recall and pending editor/instance/module unload; requests=11"<<std::endl;return 0;
    }catch(const std::exception& error){protocol.release();if(!terminalWritten)finish(false,error.what(),true);throw;}
}
int selfTest()
{
    // Independent fixture sections use separate call frames: Debug otherwise
    // reserves all of their large document temporaries at this function entry.
    verifyCapturedTask8LegacyInputs();verifyCapturedFilterOldInputs();verifyCapturedChorusOldInputs();verifyCapturedPhaserOldInputs();verifyCapturedFlangerOldInputs();verifyCapturedStereoOldInputs();verifyCapturedDimensionOldInputs();verifyCapturedFrequencyShiftOldInputs();verifyCapturedConvolutionOldInputs();verifyCapturedFrequencySplitOldInputs();verifyCapturedMidSideSplitOldInputs();
    std::cout << "RENDER_SELFTEST_PHASE captured_input_verification_complete\n";
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    auto rejects = [&](auto operation, const char* name) {
        bool rejected = false;
        try { operation(); } catch (const std::exception&) { rejected = true; }
        check(rejected, name);
    };
    // Caller-owned default project digests pin parity with the retained actual VST3
    // fixtures from source-frozen-v3; these are not generated from the shared helper.
    constexpr const char* fixtureDigests[]={
        "3c897216adc56c9c420f4ae7bc0f2d9eb2c1fe7542de8ad267160525b1612f44",
        "6b55333d7293e2dc0be5aa75973d3de3591d82305cb64c645ba8dc5bdfc6bc05",
        "320206e64387075e58503a720803a83c269fc1a471b25bd98cc534e3069a2d36"};
    for(int example=0;example<3;++example){ [&] {
        for(int setting:{-1,1})check(personalityEndpointMatches(personalityFixture(example,setting),example,setting),"independent golden primary key/slot/endpoint");
        airack::ProjectDocument project;project.sound=personalityFixture(example,0);juce::MemoryBlock bytes;
        const auto legacy=legacyProjectFixture(project);airack::ProjectDocument decoded;
        const auto legacyHash=juce::SHA256(legacy).toHexString();
        check(legacyHash==fixtureDigests[example],"caller legacy fixture SHA matches retained native/hosted baseline");
        const bool legacyDecoded=airack::decodeProjectState(legacy.getData(),legacy.getSize(),decoded);
        check(legacyDecoded,"caller legacy Project3/Sound4 fixture decodes");
        const bool legacyEqual=legacyDecoded&&decoded==legacyPersonalityExpectation(project);
        check(legacyEqual,"caller legacy fixture retains complete state and exact frozen package refs");
        std::cout<<"PERSONALITY_LEGACY example="<<example<<" sha256="<<legacyHash<<" decode="<<legacyDecoded<<" equality="<<legacyEqual<<'\n';
        const bool encoded=airack::encodeProjectState(project,bytes);
        check(encoded,"current Project30 fixture encodes independently of old digest");
        const auto version=bytes.getSize()>=6?juce::ByteOrder::littleEndianShort(static_cast<const unsigned char*>(bytes.getData())+4):0;
        check(airack::currentProjectStateVersion==30&&version==airack::currentProjectStateVersion,"current Project30 fixture complete encoded version");
        const bool currentDecoded=encoded&&airack::decodeProjectState(bytes.getData(),bytes.getSize(),decoded);
        check(currentDecoded,"current Project30 fixture decodes independently of old digest");
        const bool currentEqual=currentDecoded&&decoded==project;
        check(currentEqual,"current Project30 fixture complete roundtrip is independent of old digest");
        std::cout<<"PERSONALITY_CURRENT example="<<example<<" version="<<version<<" encode="<<encoded<<" decode="<<currentDecoded<<" equality="<<currentEqual<<'\n';
    }(); }
    // Real directories test the argument boundary only; these are never loaded as plugins.
    auto root = juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/task-3-self-test-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "self-test directory creation failed");
    auto bundle = root.getChildFile("ArgumentFixture.vst3");
    require(bundle.createDirectory().wasOk(), "argument fixture creation failed");
    auto dll = bundle.getChildFile("Contents/x86_64-win/ArgumentFixture.vst3");
    require(dll.getParentDirectory().createDirectory().wasOk() && dll.replaceWithText("argument-only"), "argument fixture write failed");
    auto output = root.getChildFile("output");
    auto args = juce::StringArray { "--plugin", bundle.getFullPathName(), "--output", output.getFullPathName() };
    [&]{const auto fixture=drum_product::smallFixture();
        try{const auto image=drum_product::decodeHosted(drum_product::wrapped(fixture.owned,false));check(image.project==fixture.project,"Drum hosted verifier accepts exact zero-private JUCE wrapper");}catch(const std::exception&){check(false,"Drum hosted verifier accepts exact zero-private JUCE wrapper");}
        try{const auto image=drum_product::decodeHosted(drum_product::wrapped(fixture.owned,true));check(image.project==fixture.project,"Drum hosted verifier accepts exact valid private tree wrapper");}catch(const std::exception&){check(false,"Drum hosted verifier accepts exact valid private tree wrapper");}
    }();
    // Run large Project fixtures in their own call frame; keep the established stack limit.
    [&]{
        const auto fixture=std::make_unique<drum_product::Image>(drum_product::smallFixture());
        const auto zero=drum_product::wrapped(fixture->owned,false),tree=drum_product::wrapped(fixture->owned,true);
        check(zero==drum_product::wrapped(fixture->owned,false),"Drum opaque original wrapper bytes preserved");
        check(drum_product::decodeOwned(fixture->owned).project==fixture->project,"Drum strict whole raw Project bundle accepts valid original");
        auto trailing=fixture->owned;trailing.append("x",1);rejects([&]{drum_product::decodeOwned(trailing);},"Drum raw Project bundle rejects whole-file trailing byte");
        const auto component=[](const juce::MemoryBlock& opaque){const auto xml=juce::AudioProcessor::getXmlFromBinary(opaque.getData(),int(opaque.getSize()));juce::MemoryBlock result;require(xml&&xml->getChildByName("IComponent")&&result.fromBase64Encoding(xml->getChildByName("IComponent")->getAllSubText()),"Wrapper negative setup invalid");return result;};
        const auto rejected=[&](juce::MemoryBlock raw,const char* name){rejects([&]{drum_product::decodeHosted(hostEnvelope(raw));},name);};
        {auto raw=component(zero);raw.setSize(raw.getSize()-15,true);rejected(raw,"Drum hosted rejects missing terminal private magic");}
        {auto raw=component(zero);static_cast<char*>(raw.getData())[raw.getSize()-1]^=1;rejected(raw,"Drum hosted rejects changed terminal private magic");}
        {auto raw=component(zero);raw.setSize(20,true);rejected(raw,"Drum hosted rejects truncated wrapper");}
        for(const auto length:{juce::int64(-1),juce::int64(INT64_MAX),juce::int64(65536),juce::int64(100)}){auto raw=component(zero);juce::MemoryOutputStream stream(raw,true);stream.setPosition(juce::int64(raw.getSize()-23));stream.writeInt64(length);rejected(raw,"Drum hosted rejects negative/oversized/out-of-range private length");}
        {auto raw=component(zero);static_cast<char*>(raw.getData())[fixture->owned.getSize()]=1;rejected(raw,"Drum hosted rejects nonzero separator");}
        {auto raw=component(tree);static_cast<char*>(raw.getData())[fixture->owned.getSize()+8]=0;rejected(raw,"Drum hosted rejects malformed private tree");}
        const auto withPrivate=[&](const juce::MemoryBlock& privateData){juce::MemoryBlock raw=fixture->owned;const std::array<char,8> zeros{};raw.append(zeros.data(),8);raw.append(privateData.getData(),privateData.getSize());juce::MemoryOutputStream stream(raw,true);stream.writeInt64(juce::int64(privateData.getSize()));stream.write("JUCEPrivateData",15);stream.flush();return raw;};
        {juce::MemoryBlock privateBytes;juce::MemoryOutputStream stream(privateBytes,false);juce::ValueTree wrong("ForeignData");wrong.writeToStream(stream);stream.flush();rejected(withPrivate(privateBytes),"Drum hosted rejects wrong private tree type");}
        {juce::MemoryBlock privateBytes;juce::MemoryOutputStream stream(privateBytes,false);juce::ValueTree good("JUCEPrivateData");good.writeToStream(stream);stream.writeByte(1);stream.flush();rejected(withPrivate(privateBytes),"Drum hosted rejects unconsumed private tree bytes");}
        {juce::MemoryBlock privateBytes(65536,true);rejected(withPrivate(privateBytes),"Drum hosted rejects separate wrapper overhead above65536");}
        {auto raw=component(zero);static_cast<char*>(raw.getData())[0]^=1;rejected(raw,"Drum hosted rejects owned body/header corruption");}
        {auto owned=fixture->owned;owned.append("x",1);rejects([&]{drum_product::decodeHosted(drum_product::wrapped(owned,false));},"Drum hosted rejects owned trailing byte before separator");}
        {auto changed=std::make_unique<drum_product::Image>(*fixture);auto project=std::make_unique<airack::ProjectDocument>(fixture->project);project->sound.values[0]=.25f;*changed=drum_product::reproject(*fixture,*project);rejects([&]{drum_product::verifyImage(*changed,*fixture);},"Drum independent numeric expectation rejects substituted valid Project");}
        {auto changed=std::make_unique<drum_product::Image>(*fixture);auto slots=fixture->bundle->document().slots();slots[0][1]=slots[0][0];slots[0][0]={};*changed=drum_product::bundleFor(fixture->project,slots,fixture->bundle->assets());rejects([&]{drum_product::verifyImage(*changed,*fixture);},"Drum independent64 metadata expectation rejects substituted assignment");}
        {auto slots=fixture->bundle->document().slots();slots[0][0].asset.reset();const std::array<airack::drums::AssetPtr,0> none{};const auto changed=std::make_unique<drum_product::Image>(drum_product::bundleFor(fixture->project,slots,none));rejects([&]{drum_product::verifyImage(*changed,*fixture);},"Drum independent owner expectation rejects missing asset");}
    }();
    [&]{
        auto scene=std::make_unique<drum_product::Scene>(drum_product::syntheticScene(1,48000));
        const auto count=[&](const auto& predicate){return std::count_if(scene->events.begin(),scene->events.end(),[&](const auto& e){return predicate(e.message);});};
        check(scene->events.size()==40&&count([](const auto& m){return m.isNoteOn();})==21&&count([](const auto& m){return m.isNoteOff();})==16&&count([](const auto& m){return m.isController();})==2,"Drum sixteen-pad literal keeps all independent note/off/channel controls");
        check(count([](const auto& m){return m.isMidiStop();})==1&&std::any_of(scene->events.begin(),scene->events.end(),[](const auto& e){return e.sample==24017&&e.message.isMidiStop();}),"Drum original native timeline retains raw Stop at literal frame24017");
        auto comparison=std::make_unique<drum_product::Scene>(*scene);comparison->events=drum_product::vst3MidiEvents(scene->events);
        check(comparison->events.size()==39&&std::count_if(comparison->events.begin(),comparison->events.end(),[](const auto& e){return e.message.isController();})==2
            &&std::any_of(comparison->events.begin(),comparison->events.end(),[](const auto& e){return e.sample==24000&&e.message.isNoteOn()&&e.message.getNoteNumber()==39&&e.message.getChannel()==3;})
            &&std::any_of(comparison->events.begin(),comparison->events.end(),[](const auto& e){return e.sample==24032&&e.message.isNoteOn()&&e.message.getNoteNumber()==40&&e.message.getChannel()==3;}),"Drum VST3 comparison keeps both boundary notes and both channel CC controls");
        rejects([&]{drum_product::vst3MidiEvents({{0,juce::MidiMessage::midiClock()}});},"Drum VST3 fixture rejects another unsupported system message rather than silently dropping it");
        juce::VST3PluginFormat format;juce::PluginDescription unused;
        for(bool higher:{false,true}){
            auto project=std::make_unique<airack::ProjectDocument>(scene->image.project);setGainValue(project->sound,"output.render_quality",higher?1.f:0.f);scene->image=drum_product::reproject(scene->image,*project);comparison->image=scene->image;
            const auto stopped=drum_product::renderScene(format,unused,*scene,512,512,true),continuing=drum_product::renderScene(format,unused,*comparison,512,512,true);
            const auto evidence=drum_product::nativeRawStopEvidence(stopped.render.audio,continuing.render.audio,24017,higher);
            check(bool(evidence["prefix_bits_exact"])&&double(evidence["max_abs_effect"])>0,"Drum real native raw Stop has a causal audio effect with an unchanged prefix in each quality");
            const auto oversized=drum_product::renderScene(format,unused,*scene,512,1024,true);
            check(drum_product::exactAudio(stopped.render.audio,oversized.render.audio)&&stopped.render.latencySamples==oversized.render.latencySamples,"Drum original full native raw-Stop timeline remains exact through the oversized callback path");
        }
    }();
    [&]{
        drum_product::HostProtocol protocol;protocol.flac="fLaC-parser-fixture-only";
        const std::string runtime="72338156-035d-4658-b97e-000000000001",client="72338156-035d-4658-b97e-000000000002";
        const std::array<std::string,3> attempts{{"72338156-035d-4658-b97e-000000000003","72338156-035d-4658-b97e-000000000004","72338156-035d-4658-b97e-000000000005"}};
        const std::array<std::string,3> seeds{{"17","73","9007199254740993"}};
        const auto capture=[](std::string method,std::string target,std::string body={}){comfy_fixture::Capture result;result.method=std::move(method);result.target=std::move(target);result.body=std::move(body);result.complete=true;return result;};
        const auto submission=[&](unsigned n){const auto prefix="airack/"+runtime+"/1/"+attempts[n]+"/seed-"+seeds[n];return capture("POST","/prompt",drum_product::frozenSubmit(attempts[n],client,protocol.prompt,seeds[n],prefix));};
        {auto wrong=submission(0);const auto at=wrong.body.find("\"steps\":8");require(at!=std::string::npos,"Owned graph negative setup invalid");wrong.body.replace(at,9,"\"steps\":9");check(!protocol.reply(wrong).matched&&protocol.operations==0,"Owned host validator rejects altered frozen graph before candidate");}
        {auto wrong=submission(0);wrong.target="/foreign";check(!protocol.reply(wrong).matched&&protocol.operations==0,"Owned host validator rejects forged POST route");}
        {auto wrong=submission(0);const auto at=wrong.body.find(attempts[0]);wrong.body.replace(at,attempts[0].size(),attempts[1]);check(!protocol.reply(wrong).matched&&protocol.operations==0,"Owned host validator rejects forged prompt ID and owned prefix mismatch");}
        {auto wrong=submission(0);const auto at=wrong.body.find("\"seed\":17");wrong.body.replace(at,9,"\"seed\":17.0");check(!protocol.reply(wrong).matched&&protocol.operations==0,"Owned host validator requires native exact decimal seed spelling");}
        for(unsigned n=0;n<3;++n){check(protocol.reply(submission(n)).matched,"Owned host validator accepts full frozen fresh UUID graph");check(!protocol.reply(submission(n)).matched,"Owned host validator rejects repeated/out-of-order submission");
            check(!protocol.reply(capture("GET","/history/"+attempts[(n+1)%3])).matched,"Owned host validator rejects foreign history UUID");
            check(protocol.reply(capture("GET","/history/"+attempts[n])).matched,"Owned host validator binds matching completed history");
            std::string folder="airack/"+runtime+"/1/"+attempts[n],encoded;for(char c:folder)encoded+=c=='/'?"%2F":std::string(1,c);
            const auto view="/view?filename=seed-"+seeds[n]+"_00001.flac&subfolder="+encoded+"&type=output";
            check(!protocol.reply(capture("GET",view+"&extra=1")).matched,"Owned host validator rejects altered view route controls");check(protocol.reply(capture("GET",view)).matched,"Owned host validator binds matching owned FLAC output");
        }
        check(protocol.operations==9&&protocol.batch==1&&protocol.attempts.size()==3&&protocol.clients.size()==1,"Owned host validator completes exactly three sequential native-decimal attempts");
    }();
    // Expected-valid final drum selector checks intentionally precede its parser implementation.
    const auto drumOutput=root.getChildFile("drum-output");
    const auto drumArgs=juce::StringArray{"--drum-product","--plugin",bundle.getFullPathName(),"--output",drumOutput.getFullPathName()};
    try{check(parseArguments(drumArgs).output==drumOutput,"Drum product accepts exact absolute fresh output");}catch(const std::exception&){check(false,"Drum product accepts exact absolute fresh output");}
    {auto pending=drumArgs;pending.add("--drum-pending-unload");try{check(parseArguments(pending).output==drumOutput,"Drum product accepts explicit owned pending-unload condition");}catch(const std::exception&){check(false,"Drum product accepts explicit owned pending-unload condition");}}
    check(!drumOutput.exists(),"Drum argument parsing creates no output or runtime work");
    {
        const auto known=drum_product::caseIds(false);auto unique=known;unique.removeDuplicates(false);
        check(known.size()==24&&unique.size()==24,"Drum synthetic case table contains exactly24 unique rows");
        juce::StringArray admitted;
        for(const auto* scene:{"single-pad","sixteen-pad-midi","four-banks-128","pattern-routing-space"})for(int rate:{44100,48000,96000})for(const auto* quality:{"normal","higher"}){
            const auto id=juce::String(scene)+"-"+juce::String(rate)+"-"+quality;auto selected=drumArgs;selected.add("--drum-product-case");selected.add(id);
            try{const auto parsed=parseArguments(selected);check(parsed.drumCase==id&&parsed.drumStateBytes.getSize()==0&&!parsed.drumPendingUnload,"Drum admits exact synthetic row without changing its source role");admitted.add(id);}catch(const std::exception&){check(false,"Drum admits exact synthetic row without changing its source role");}
        }
        check(admitted==known&&parseArguments(drumArgs).drumCase.isEmpty(),"All24 selected synthetic rows cover the unchanged default matrix exactly once");
        for(const auto* id:{"","single-pad","single-pad-044100-normal","single-pad-44100-Normal","single-pad-192000-normal","single-pad-44100-normal-extra","real-kept-kit-44100-normal"}){auto changed=drumArgs;changed.add("--drum-product-case");changed.add(id);rejects([&]{parseArguments(changed);},"Drum rejects empty/unknown/noncanonical/wrong-source case before work");}
        {auto changed=drumArgs;changed.add("--drum-product-case");rejects([&]{parseArguments(changed);},"Drum rejects missing case value");}
        {auto changed=drumArgs;changed.add("--drum-product-case");changed.add("single-pad-44100-normal");changed.add("--drum-product-case");changed.add("single-pad-44100-normal");rejects([&]{parseArguments(changed);},"Drum rejects duplicate case selectors");}
        for(bool reverse:{false,true}){auto changed=drumArgs;if(!reverse)changed.add("--drum-pending-unload");changed.add("--drum-product-case");changed.add("single-pad-44100-normal");if(reverse)changed.add("--drum-pending-unload");rejects([&]{parseArguments(changed);},"Drum rejects pending-unload and selected case in both orders");}
        for(const auto* other:{"--drum-product","--rack-personalities","--sound-quality","--drum-product-scene","--unknown"}){auto changed=drumArgs;changed.add("--drum-product-case");changed.add("single-pad-44100-normal");changed.add(other);rejects([&]{parseArguments(changed);},"Drum rejects mixed or unknown selector with selected case");}
        {auto changed=args;changed.add("--drum-product-case");changed.add("single-pad-44100-normal");rejects([&]{parseArguments(changed);},"Legacy mode rejects Drum case selector");}
        check(!drumOutput.exists(),"Drum case parser/cardinality checks create no output or runtime work");
    }
    [&]{auto stateFile=root.getChildFile("valid-drum-project.arbndl");const auto fixture=drum_product::smallFixture();require(stateFile.replaceWithData(fixture.owned.getData(),fixture.owned.getSize()),"Drum parser state fixture write failed");
        auto stateArgs=drumArgs;stateArgs.add("--drum-state");stateArgs.add(stateFile.getFullPathName());try{const auto parsed=parseArguments(stateArgs);check(parsed.drumProduct&&!parsed.drumPendingUnload&&parsed.drumState==stateFile&&parsed.drumStateBytes==fixture.owned,"Drum product accepts exact whole native Project state without runtime work");}catch(const std::exception&){check(false,"Drum product accepts exact whole native Project state without runtime work");}
        {const auto known=drum_product::caseIds(true);auto unique=known;unique.removeDuplicates(false);check(known.size()==6&&unique.size()==6,"Drum saved-state table contains exactly6 unique rows");juce::StringArray admitted;
            for(int rate:{44100,48000,96000})for(const auto* quality:{"normal","higher"}){const auto id="real-kept-kit-"+juce::String(rate)+"-"+quality;auto selected=stateArgs;selected.add("--drum-product-case");selected.add(id);try{const auto parsed=parseArguments(selected);check(parsed.drumCase==id&&parsed.drumStateBytes==fixture.owned&&!parsed.drumPendingUnload,"Drum admits exact saved-state row and retains whole native input");admitted.add(id);}catch(const std::exception&){check(false,"Drum admits exact saved-state row and retains whole native input");}}
            check(admitted==known&&parseArguments(stateArgs).drumCase.isEmpty(),"All6 selected saved-state rows cover its unchanged default matrix exactly once");
            auto wrong=stateArgs;wrong.add("--drum-product-case");wrong.add("single-pad-44100-normal");rejects([&]{parseArguments(wrong);},"Drum rejects synthetic case with saved-state source");
            auto reversed=drumArgs;reversed.add("--drum-product-case");reversed.add("real-kept-kit-44100-normal");reversed.add("--drum-state");reversed.add(stateFile.getFullPathName());rejects([&]{parseArguments(reversed);},"Drum requires saved-state before trailing case selector");
        }
        {auto changed=stateArgs;changed.add("--drum-pending-unload");rejects([&]{parseArguments(changed);},"Drum parser rejects saved state and pending-unload combination");}
        {auto changed=stateArgs;changed.set(changed.size()-1,"relative.arbndl");rejects([&]{parseArguments(changed);},"Drum parser rejects relative saved-state input");}
        auto malformed=root.getChildFile("malformed-drum-project.arbndl");require(malformed.replaceWithText("ARBNDL01-invalid"),"Drum malformed parser fixture write failed");{auto changed=stateArgs;changed.set(changed.size()-1,malformed.getFullPathName());rejects([&]{parseArguments(changed);},"Drum parser rejects malformed raw state before output");}
    }();
    for(const auto& option:{"--drum-product","--rack-personalities","--sound-quality","--unknown"}){auto changed=drumArgs;changed.add(option);rejects([&]{parseArguments(changed);},"Drum parser rejects duplicate/mixed/unknown selector");}
    {auto changed=drumArgs;changed.add("--drum-pending-unload");changed.add("--drum-pending-unload");rejects([&]{parseArguments(changed);},"Drum parser rejects duplicate pending condition");}
    {auto changed=args;changed.add("--drum-pending-unload");rejects([&]{parseArguments(changed);},"Legacy parser rejects live condition without Drum selector");}
    {auto changed=drumArgs;changed.set(2,"relative.vst3");rejects([&]{parseArguments(changed);},"Drum parser rejects relative plugin");}
    {auto changed=drumArgs;changed.set(4,"relative-output");rejects([&]{parseArguments(changed);},"Drum parser rejects relative output");}
    require(drumOutput.createDirectory().wasOk(),"Drum existing-output parser setup failed");rejects([&]{parseArguments(drumArgs);},"Drum parser rejects an existing empty output");check(drumOutput.deleteFile(),"Drum parser removes only its empty fixture output");
    // Space's dedicated mode reuses the established path boundary, adding a fresh-only fence.
    const auto spaceOutput=root.getChildFile("space-output");
    const auto spaceArgs=juce::StringArray{"--space-reverb","--plugin",bundle.getFullPathName(),"--output",spaceOutput.getFullPathName()};
    try{check(parseSpaceReverbArguments(spaceArgs).output==spaceOutput,"Space mode accepts exact absolute fresh output");}catch(const std::exception&){check(false,"Space mode accepts exact absolute fresh output");}
    check(!spaceOutput.exists(),"Space argument parsing creates no output");
    {auto changed=spaceArgs;changed.add("--quality");changed.add("higher");rejects([&]{parseSpaceReverbArguments(changed);},"Space rejects unrelated legacy quality selector");}
    {auto changed=spaceArgs;changed.add("--space-reverb");rejects([&]{parseSpaceReverbArguments(changed);},"Space rejects duplicate mode");}
    {auto changed=spaceArgs;changed.set(2,"relative.vst3");rejects([&]{parseSpaceReverbArguments(changed);},"Space rejects relative plugin");}
    {auto changed=spaceArgs;changed.set(4,"relative");rejects([&]{parseSpaceReverbArguments(changed);},"Space rejects relative output");}
    {auto changed=spaceArgs;changed.set(4,juce::File(RACKSYNTH_WORKSPACE).getParentDirectory().getFullPathName());rejects([&]{parseSpaceReverbArguments(changed);},"Space rejects workspace escape");}
    require(spaceOutput.createDirectory().wasOk(),"Space argument test directory creation failed");
    rejects([&]{parseSpaceReverbArguments(spaceArgs);},"Space rejects an existing empty output");
    check(spaceOutput.deleteFile(),"Space test removes only its empty fixture directory");
    for(const auto& scene:spaceRenderScenes()){
        check(scene.dry.layout.synthCount==1&&scene.dry.layout.synthUse[0]==airack::SlotUse::present,
              "Space audio fixtures explicitly contain their synth after empty-instance startup");
        check(scene.processed.layout.synthCount==1&&scene.processed.layout.effects[scene.slot].kind==airack::EffectKind::spaceReverb,
              "Space processed scene retains the same source plus its reverb");
    }
    const auto eraOutput=root.getChildFile("era-output");
    const auto eraArgs=juce::StringArray{"--era-reverb","--plugin",bundle.getFullPathName(),"--output",eraOutput.getFullPathName()};
    try{check(parseEraReverbArguments(eraArgs).output==eraOutput,"Era mode accepts exact absolute fresh output");}catch(const std::exception&){check(false,"Era mode accepts exact absolute fresh output");}
    check(!eraOutput.exists(),"Era argument parsing creates no output");
    {auto changed=eraArgs;changed.add("--era-reverb");rejects([&]{parseEraReverbArguments(changed);},"Era rejects duplicate mode");}
    {auto changed=eraArgs;changed.set(0,"--space-reverb");rejects([&]{parseEraReverbArguments(changed);},"Era rejects another mode");}
    {auto changed=eraArgs;changed.set(2,"relative.vst3");rejects([&]{parseEraReverbArguments(changed);},"Era rejects relative plugin");}
    {auto changed=eraArgs;changed.set(4,"relative");rejects([&]{parseEraReverbArguments(changed);},"Era rejects relative output");}
    require(eraOutput.createDirectory().wasOk(),"Era argument test directory creation failed");
    rejects([&]{parseEraReverbArguments(eraArgs);},"Era rejects an existing empty output");
    check(eraOutput.deleteFile(),"Era test removes only its empty fixture directory");
    [&]{
        for(const auto& scene:eraRenderScenes())
            check(scene.processed.layout.effects[scene.slot].kind==airack::EffectKind::eraReverb&&scene.processed.layout.effects[scene.slot].chain==airack::masterChain
                  &&scene.dry.layout.effects[scene.slot].use!=airack::SlotUse::present,"Era scenes add Era Reverb to a free master slot of the same source");
        const auto comparisonFile=juce::File(RACKSYNTH_WORKSPACE).getChildFile("tools/era_reverb_comparison_rows.json");
        const auto comparison=juce::JSON::parse(comparisonFile);
        if(comparisonFile.existsAsFile()) {
        check(comparison["rows"].isArray()&&comparison["rows"].size()>=40&&comparison["neutral"].isObject(),"Era comparison rows load");
        juce::StringArray ids;bool valid=comparison["rows"].isArray();
        if(valid)for(const auto& row:*comparison["rows"].getArray()){
            try{auto scene=eraComparisonSource(row["source"].toString());setEraComparisonSettings(scene.processed,scene.slot,comparison["neutral"],row["ours"]);
                valid=valid&&airack::validateSoundDocument(scene.processed)&&!ids.contains(row["id"].toString())&&double(row["seconds"])>=3;ids.add(row["id"].toString());}
            catch(const std::exception&){valid=false;}
        }
        check(valid,"every Era comparison row names a known source, registered settings, at least 3 s and a unique id");
        } else std::cout<<"SKIP: two private Era comparison-recipe assertions; research inputs excluded from this source snapshot\n";
        rejects([&]{eraComparisonSource("square");},"Era rejects an unknown comparison source");
        auto source=eraComparisonSource("sine220");const auto& order=source.processed.layout.effectOrder[airack::masterChain];
        bool staged=source.processed.layout.effectCount[airack::masterChain]==4&&order[3]==source.slot&&source.dry.layout.effectCount[airack::masterChain]==3
            &&airack::parameterPhysicalValue(source.dry.values,0)==1&&airack::parameterPhysicalValue(source.processed.values,0)==1;
        for(std::size_t i=0;i<source.gains.size();++i)
            staged=staged&&order[i]==source.gains[i]&&source.dry.layout.effectOrder[airack::masterChain][i]==source.gains[i]
                &&source.processed.layout.effects[source.gains[i]].kind==airack::EffectKind::stereo&&source.dry.layout.effects[source.gains[i]].kind==airack::EffectKind::stereo;
        check(staged,"Era comparison sources reach Era through three Stereo stages with Master at 1");
        setEraInputGain(source,24);check(std::abs(eraInputGainDb(source.dry,source)-24)<1.e-3&&std::abs(eraInputGainDb(source.processed,source)-24)<1.e-3,"Era comparison level splits over the stages");
        setEraInputGain(source,-72);check(std::abs(eraInputGainDb(source.processed,source)+72)<1.e-3,"Era comparison level reaches the stages' -72 dB");
        rejects([&]{setEraInputGain(source,36.5);},"Era comparison level rejects more than the stages' +36 dB");
    }();
    check(parseArguments(args).output == output, "valid absolute workspace output is accepted");
    check(!output.exists(), "argument validation does not create output");
    check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--algorithm","2","--reverb-only"}).reverbRevision==2,"implemented modern reverb evidence selected explicitly");
    check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","higher","--algorithm","1","--reverb-only"}).reverbRevision==1,"legacy reverb evidence remains explicit in higher mode");
    try {check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--algorithm","2","--reverb-probes-only"}).reverbOnly,"existing mix1/control probe selection admitted");}
    catch(const std::exception&) {check(false,"existing mix1/control probe selection admitted");}
    check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","higher","--algorithm","2","--reverb-probes-only"}).reverbProbesOnly,"minimum probe selection is explicit in higher mode");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--reverb-probes-only","--reverb-probes-only"});},"duplicate probe selection rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-analog","--reverb-probes-only"});},"mixed factory/probe selection rejects");
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--reverb-probes-only"});},"probe selection outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--algorithm","3","--reverb-only"});},"unsupported evidence revision rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--algorithm","2","--factory-analog"});},"reverb revision cannot select unrelated factory evidence");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--algorithm","2","--algorithm","2","--reverb-only"});},"duplicate reverb revision flags reject");
    { [&] {
        const auto modern=reverbAnalysisBriefs(2),legacy=reverbAnalysisBriefs(1);
        bool sameDry=true,modernActive=true;
        for(std::size_t n=0;n<modern.size();++n) {
            sameDry&=modern[n].dry==legacy[n].dry;
            airack::RoutedPatch p;require(airack::makeRoutedPatch(modern[n].processed,p),"modern evidence setup invalid");
            for(std::size_t slot=0;slot<8;++slot)if(p.layout.effects[slot].use==airack::SlotUse::present&&p.layout.effects[slot].kind==airack::EffectKind::reverb)
                modernActive&=p.effects[slot].algorithmRevision==2&&p.compatibility.effects[slot][2]==2;
        }
        check(sameDry&&modernActive,"modern reverb evidence preserves dry source and selects actual active bank2");
    }(); }
    const auto baselineArgument=root.getChildFile("argument-only-manifest.json");require(baselineArgument.replaceWithText("[]"),"Baseline argument setup failed");
    check(parseArguments({"--rack-personalities","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--baseline",baselineArgument.getFullPathName()}).baseline==baselineArgument,"personality baseline argument retained explicitly");
    rejects([&]{parseArguments({"--rack-personalities","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--baseline",root.getChildFile("missing.json").getFullPathName()});},"missing requested baseline cannot fall back");
    rejects([&]{parseArguments({"--rack-personalities","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--baseline","relative.json"});},"relative baseline rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--baseline",baselineArgument.getFullPathName()});},"baseline limited to existing personality mode");
    try {const auto selected=parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--core-fx-probes"});check(selected.output==output&&selected.coreFxProbes&&!selected.factoryAnalog&&!selected.factoryDigital&&!selected.factoryReverb&&!selected.reverbOnly,"actual Copper Arc core-FX input calibration admitted in existing sound-quality mode");}
    catch(const std::exception&) {check(false,"actual Copper Arc core-FX input calibration admitted in existing sound-quality mode");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--core-fx-probes"});},"core-FX probe outside sound-quality rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--core-fx-probes","--core-fx-probes"});},"duplicate core-FX probe selector rejected");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--core-fx-probes",reverse?"--core-fx-probes":other});},"mixed core-FX probe selectors rejected in both orders");
    try {check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-core-fx"}).factoryCoreFx,"actual authored core-FX factory packet selector admitted");}
    catch(const std::exception&) {check(false,"actual authored core-FX factory packet selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-core-fx"});},"core-FX factory selector outside sound-quality rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-core-fx","--factory-core-fx"});},"duplicate core-FX factory selectors rejected");
    for(const auto* other:{"--core-fx-probes","--factory-analog","--factory-digital","--factory-reverb","--reverb-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-core-fx",reverse?"--factory-core-fx":other});},"mixed core-FX factory selection rejected in both orders");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-eq"});check(true,"actual Contour Loom EQ factory selector admitted");}
    catch(const std::exception&) {check(false,"actual Contour Loom EQ factory selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-eq"});},"EQ factory selection outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-eq","--factory-eq"});},"duplicate EQ factory selection rejects");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--factory-core-fx","--core-fx-probes","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-eq",reverse?"--factory-eq":other});},"mixed EQ factory selection rejects in both orders");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-filter"});check(true,"actual Veil Current dedicated filter factory selector admitted");}
    catch(const std::exception&) {check(false,"actual Veil Current dedicated filter factory selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-filter"});},"filter factory outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-filter","--factory-filter"});},"duplicate filter factory selectors reject");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--factory-core-fx","--factory-eq","--core-fx-probes","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-filter",reverse?"--factory-filter":other});},"mixed filter factory selectors reject in both orders");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-chorus"});check(true,"actual Orbit Ribbon dedicated chorus factory selector admitted");}
    catch(const std::exception&) {check(false,"actual Orbit Ribbon dedicated chorus factory selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-chorus"});},"chorus factory outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-chorus","--factory-chorus"});},"duplicate chorus factory selectors reject");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--factory-core-fx","--factory-eq","--factory-filter","--core-fx-probes","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-chorus",reverse?"--factory-chorus":other});},"mixed chorus factory selectors reject in both orders");
    { [&] {const auto cases=factoryMidSideSplitBriefs();check(cases.size()==7&&cases.front().processed.personalities->instances.size()==2&&cases.back().processed.personalities->instances.size()==12,"mid side split untouched neutral/five roles/ordinarypan4-carrier8-insert load");
        for(const auto& b:cases){airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"mid side split actual factory mapping");for(std::size_t slot=0;slot<8;++slot)if(patch.layout.effects[slot].use==airack::SlotUse::present)check(patch.layout.effects[slot].kind==airack::EffectKind::midSideSplit&&patch.compatibility.effects[slot][14]==1,"actual active MidSideSplit1 revision");}
        auto nominal=cases.front();nominal.hold=.25;const auto events=qualityEvents(nominal,48000);for(const auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){const auto dry=renderFactoryNative(nominal.dry,events,48000,48000,quality),neutral=renderFactoryNative(nominal.processed,events,48000,48000,quality);check(difference(dry,neutral)==0,"mid side split actual nominal transparent identity");auto zero=cases[1].processed;setGainValue(zero,"effect.1.mid_side_split.mix",0);const auto carrier=renderFactoryNative(cases[1].dry,events,48000,48000,quality);check(difference(carrier,renderFactoryNative(zero,events,48000,48000,quality))==0,"mid side split activeMix0 original carrier identity");check(difference(carrier,renderFactoryNative(cases[2].processed,events,48000,48000,quality))>1e-4,"mid side split actual SideSolo changes existing-Side carrier");double side=0;for(int n=0;n<carrier.getNumSamples();++n)side=std::max(side,std::abs(double(carrier.getSample(0,n))-carrier.getSample(1,n)));check(side>1e-4,"ordinary edited role carrier has measurable Side");const auto mono=renderFactoryNative(cases[3].processed,events,48000,48000,quality);double channelDifference=0;for(int n=0;n<mono.getNumSamples();++n)channelDifference=std::max(channelDifference,std::abs(double(mono.getSample(0,n))-mono.getSample(1,n)));check(channelDifference==0,"actual Side-mute yields mono from fresh history");}}(); }
    { [&] {const auto cases=factoryFrequencySplitBriefs();check(cases.size()==7&&cases.front().processed.personalities->instances.size()==2&&cases.back().processed.personalities->instances.size()==12,"frequency split untouched neutral/five roles/nonneutral4-carrier8-insert load");
        for(const auto& b:cases){airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"frequency split actual factory mapping");for(std::size_t slot=0;slot<8;++slot)if(patch.layout.effects[slot].use==airack::SlotUse::present)check(patch.layout.effects[slot].kind==airack::EffectKind::frequencySplit&&patch.compatibility.effects[slot][airack::frequencySplitEffectBank]==airack::modernFrequencySplitAlgorithmRevision,"actual active FrequencySplit2 revision");}
        auto nominal=cases.front();nominal.hold=.25;const auto events=qualityEvents(nominal,48000);auto solo=cases[1].processed,zero=solo;setGainValue(zero,"effect.1.frequency_split.mix",0);
        for(auto* document:{&nominal.dry,&nominal.processed,&solo,&zero}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        for(const auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){const auto dry=renderFactoryNative(nominal.dry,events,48000,48000,quality),neutral=renderFactoryNative(nominal.processed,events,48000,48000,quality);
            // FrequencySplit2's neutral bands and Mix0 return the allpass sum, so they keep the
            // source's level but not its samples; FrequencySplit1's exact identities stay in Routed.
            const auto unmixed=renderFactoryNative(zero,events,48000,48000,quality);
            const auto level=[&](const juce::AudioBuffer<float>& wet){return 20*std::log10(measureWindow(wet,{{0,48000}}).rms/measureWindow(dry,{{0,48000}}).rms);};
            std::cout<<"FREQUENCY_SPLIT2_NEUTRAL quality="<<int(quality)<<" neutral_level_db="<<level(neutral)<<" mix0_level_db="<<level(unmixed)<<" difference="<<difference(dry,neutral)<<'\n';
            check(measureWindow(dry,{{0,48000}}).rms>1e-7&&std::abs(level(neutral))<=.01&&std::abs(level(unmixed))<=.01,"frequency split actual neutral and activeMix0 keep the source level");
            check(difference(dry,renderFactoryNative(solo,events,48000,48000,quality))>1e-4,"frequency split actual LowSolo changes carrier");}}(); }
    { [&] {const auto cases=factoryConvolutionBriefs();check(cases.size()==7&&cases.front().processed.personalities->instances.size()==2&&cases.back().processed.personalities->instances.size()==12,"convolution untouched nominal/five ordinary roles/four-carrier-eight-insert load");
        for(const auto& b:cases){airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"convolution actual factory native mapping");for(std::size_t slot=0;slot<8;++slot)if(patch.layout.effects[slot].use==airack::SlotUse::present)check(patch.layout.effects[slot].kind==airack::EffectKind::convolution&&patch.compatibility.effects[slot][12]==1,"actual active Convolution1 revision");}
        auto nominal=cases.front();nominal.hold=.25;const auto events=qualityEvents(nominal,48000);for(const auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){auto dry=renderFactoryNative(nominal.dry,events,48000,48000,quality);auto wet=renderFactoryNative(nominal.processed,events,48000,48000,quality);auto zero=nominal.processed;setGainValue(zero,"effect.1.convolution.mix",0);auto identity=renderFactoryNative(zero,events,48000,48000,quality);check(difference(dry,wet)>1e-4&&difference(dry,identity)==0,"convolution actual changed carrier and exact activeMix0");}}(); }
    { [&] {const auto cases=factoryFrequencyShiftBriefs();check(cases.size()==7&&cases.front().processed.personalities->instances.size()==2&&cases.back().processed.personalities->instances.size()==12,"frequency-shift actual untouched nominal/five ordinary roles/four-carrier-eight-insert load");
        for(const auto& b:cases){airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"frequency-shift actual factory native mapping");for(std::size_t slot=0;slot<8;++slot)if(patch.layout.effects[slot].use==airack::SlotUse::present)check(patch.layout.effects[slot].kind==airack::EffectKind::frequencyShift&&patch.compatibility.effects[slot][11]==1,"actual active FrequencyShift1 revision");}
        auto nominal=cases.front();nominal.hold=.25;const auto events=qualityEvents(nominal,48000);for(const auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){auto dry=renderFactoryNative(nominal.dry,events,48000,48000,quality);auto wet=renderFactoryNative(nominal.processed,events,48000,48000,quality);auto zero=nominal.processed;setGainValue(zero,"effect.1.frequency_shift.mix",0);auto identity=renderFactoryNative(zero,events,48000,48000,quality);check(difference(dry,wet)>1e-4&&difference(dry,identity)==0,"frequency-shift actual changed carrier and exact activeMix0");}}(); }
    { [&] {const auto cases=factoryDimensionBriefs();check(cases.size()==7&&cases.front().processed.personalities->instances.size()==2&&cases.back().processed.personalities->instances.size()==12,"Dimension actual nominal/five roles/independent load");
        for(const auto& b:cases){airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"Dimension actual factory native mapping");for(std::size_t slot=0;slot<8;++slot)if(patch.layout.effects[slot].use==airack::SlotUse::present)check(patch.layout.effects[slot].kind==airack::EffectKind::dimension&&patch.compatibility.effects[slot][airack::dimensionEffectBank]==airack::modernModulatedTapAlgorithmRevision,"actual active Dimension2 revision");}
        auto nominal=cases.front();nominal.hold=.25;const auto events=qualityEvents(nominal,48000);auto dry=renderFactoryNative(nominal.dry,events,48000,48000,airack::RenderQuality::realtime);auto wet=renderFactoryNative(nominal.processed,events,48000,48000,airack::RenderQuality::realtime);
        double side=0,fold=0;for(int n=0;n<dry.getNumSamples();++n){side=std::max(side,std::abs(double(wet.getSample(0,n))-wet.getSample(1,n)));fold=std::max(fold,std::abs((double(wet.getSample(0,n))+wet.getSample(1,n))-(double(dry.getSample(0,n))+dry.getSample(1,n))));}
        check(side>1.e-6&&fold<1.e-5,"Actual Twin Current generates side from a mono carrier and preserves its original fold within float tolerance");
        auto bypass=nominal.processed;setGainValue(bypass,"effect.1.dimension.mix",0);const auto neutral=renderFactoryNative(bypass,events,48000,48000,airack::RenderQuality::realtime);check(neutral==dry,"Actual Dimension mix0 is bit-exact dry");
        std::cout<<"DIMENSION_FACTORY side_max="<<side<<" fold_max="<<fold<<" mix0_bit_exact="<<(neutral==dry)<<'\n';}(); }
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-dimension"});check(true,"actual Twin Current Dimension selector");}catch(const std::exception&){check(false,"actual Twin Current Dimension selector");}
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-dimension","--factory-stereo"});},"mixed Dimension selectors reject");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-stereo"});check(true,"actual Centre Line dedicated stereo utility factory selector admitted");}
    catch(const std::exception&) {check(false,"actual Centre Line dedicated stereo utility factory selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-stereo"});},"stereo factory outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-stereo","--factory-stereo"});},"duplicate stereo factory selectors reject");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--factory-core-fx","--factory-eq","--factory-filter","--factory-chorus","--factory-phaser","--factory-flanger","--core-fx-probes","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-stereo",reverse?"--factory-stereo":other});},"mixed stereo factory selectors reject in both orders");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-flanger"});check(true,"actual Silver Loop dedicated flanger factory selector admitted");}
    catch(const std::exception&) {check(false,"actual Silver Loop dedicated flanger factory selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-flanger"});},"flanger factory outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-flanger","--factory-flanger"});},"duplicate flanger factory selectors reject");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--factory-core-fx","--factory-eq","--factory-filter","--factory-chorus","--factory-phaser","--core-fx-probes","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-flanger",reverse?"--factory-flanger":other});},"mixed flanger factory selectors reject in both orders");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-phaser"});check(true,"actual Aster Sweep dedicated phaser factory selector admitted");}
    catch(const std::exception&) {check(false,"actual Aster Sweep dedicated phaser factory selector admitted");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-phaser"});},"phaser factory outside sound-quality rejects");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-phaser","--factory-phaser"});},"duplicate phaser factory selectors reject");
    for(const auto* other:{"--factory-analog","--factory-digital","--factory-reverb","--factory-core-fx","--factory-eq","--factory-filter","--factory-chorus","--core-fx-probes","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-phaser",reverse?"--factory-phaser":other});},"mixed phaser factory selectors reject in both orders");
    [&] {
    const auto stereoCases=factoryStereoBriefs();check(stereoCases.size()==7,"stereo untouched default/five ordinary roles/actual eight-insert workload");
    const auto nominalStereo=stereoFactoryEvidence(stereoCases[0].processed);
    for(const auto& b:stereoCases){ [&] {airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"actual stereo factory maps native controls");
        for(int slot=0;slot<b.processed.layout.effectCount[airack::masterChain];++slot){check(patch.effects[std::size_t(slot)].algorithmRevision==1&&b.processed.compatibility.effects[std::size_t(slot)][9]==1,"actual placed Stereo1 agrees with retained bank9");}}(); }

    check(stereoCases[0].dry==factorySource(false)&&nominalStereo["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"stereo nominal carrier is untouched Copper Arc");
    check(nominalStereo["stereo"]["instances"].size()==1&&nominalStereo["stereo"]["instances"][0]["ordinary_tweaks"].size()==0,"stereo nominal is the complete untouched definition");
    check(stereoCases.back().processed.personalities->instances.size()==12&&stereoCases.back().dry.layout.synthCount==4&&stereoCases.back().processed.layout.effectCount[airack::masterChain]==8,"stereo load has twelve actual instances/four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        auto dry=stereoCases[0].dry,processed=stereoCases[0].processed;for(auto* document:{&dry,&processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        auto brief=stereoCases[0];brief.hold=.25;const auto events=qualityEvents(brief,48000);const auto reference=renderFactoryNative(dry,events,48000,48000,quality),wet=renderFactoryNative(processed,events,48000,48000,quality);
        check(measureWindow(reference,{{0,48000}}).rms>1e-7&&difference(reference,wet)==0,"actual default neutral stereo preserves the nonzero native Copper input in both modes");
        auto changed=processed;setGainValue(changed,"effect.1.stereo.pan",-.5f);check(difference(reference,renderFactoryNative(changed,events,48000,48000,quality))>1e-6,"ordinary unity-centre balance changes the actual nonzero source");
        setGainValue(processed,"effect.1.bypassed",1);check(difference(reference,renderFactoryNative(processed,events,48000,48000,quality))==0,"ordinary stereo bypass preserves native source");
    }(); }
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher})for(int role:{1,2}){ [&] {
        auto b=stereoCases[std::size_t(role)];b.hold=.25;for(auto* document:{&b.dry,&b.processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        const auto events=qualityEvents(b,48000);const auto input=renderFactoryNative(b.dry,events,48000,48000,quality),output=renderFactoryNative(b.processed,events,48000,48000,quality);
        double drySide=0,wetSide=0;for(int frame=0;frame<48000;++frame){const double d=(double(input.getSample(0,frame))-input.getSample(1,frame))*.5,w=(double(output.getSample(0,frame))-output.getSample(1,frame))*.5;drySide+=d*d;wetSide+=w*w;}
        const double coefficient=role==1?.5:2*std::pow(10.0,-6.0/20);
        check(drySide>1e-12&&std::abs(std::sqrt(wetSide/drySide)-coefficient)<2e-6,"actual asymmetric width role source and independent existing-side gain law");
    }(); }
    }();
    [&] {
    const auto flangerCases=factoryFlangerBriefs();check(flangerCases.size()==7,"flanger untouched default/five ordinary roles/actual eight-insert workload");
    const auto nominalFlanger=flangerFactoryEvidence(flangerCases[0].processed);
    for(const auto& b:flangerCases){ [&] {airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"actual flanger factory maps native controls");
        for(int slot=0;slot<b.processed.layout.effectCount[airack::masterChain];++slot){check(patch.effects[std::size_t(slot)].algorithmRevision==airack::modernModulatedTapAlgorithmRevision&&b.processed.compatibility.effects[std::size_t(slot)][airack::flangerEffectBank]==airack::modernModulatedTapAlgorithmRevision,"actual placed Flanger2 agrees with retained bank8");}}(); }

    check(flangerCases[0].dry==factorySource(false)&&nominalFlanger["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"flanger nominal carrier is untouched Copper Arc");
    check(nominalFlanger["flanger"]["instances"].size()==1&&nominalFlanger["flanger"]["instances"][0]["ordinary_tweaks"].size()==0,"flanger nominal is the complete untouched definition");
    check(flangerCases.back().processed.personalities->instances.size()==12&&flangerCases.back().dry.layout.synthCount==4&&flangerCases.back().processed.layout.effectCount[airack::masterChain]==8,"flanger load has twelve actual instances/four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        auto dry=flangerCases[0].dry,processed=flangerCases[0].processed;for(auto* document:{&dry,&processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        auto brief=flangerCases[0];brief.hold=.25;const auto events=qualityEvents(brief,48000);const auto reference=renderFactoryNative(dry,events,48000,48000,quality),wet=renderFactoryNative(processed,events,48000,48000,quality);
        check(measureWindow(reference,{{0,48000}}).rms>1e-7&&difference(reference,wet)>1e-6,"actual default flanger changes the nonzero native carrier");
        auto zero=processed;setGainValue(zero,"effect.1.flanger.mix",0);check(difference(reference,renderFactoryNative(zero,events,48000,48000,quality))==0,"ordinary flanger Mix0 preserves native source");
        setGainValue(processed,"effect.1.bypassed",1);check(difference(reference,renderFactoryNative(processed,events,48000,48000,quality))==0,"ordinary flanger bypass preserves native source");
    }(); }
    }();
    [&] {
    const auto phaserCases=factoryPhaserBriefs();check(phaserCases.size()==7,"phaser untouched default/five ordinary roles/actual eight-insert workload");
    const auto nominalPhaser=phaserFactoryEvidence(phaserCases[0].processed);
    for(const auto& b:phaserCases){ [&] {airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"actual phaser factory maps native controls");
        for(int slot=0;slot<b.processed.layout.effectCount[airack::masterChain];++slot){check(patch.effects[std::size_t(slot)].algorithmRevision==1&&b.processed.compatibility.effects[std::size_t(slot)][7]==1,"actual placed Phaser1 agrees with retained bank7");}}(); }

    check(phaserCases[0].dry==factorySource(false)&&nominalPhaser["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"phaser nominal carrier is untouched Copper Arc");
    check(nominalPhaser["phaser"]["instances"].size()==1&&nominalPhaser["phaser"]["instances"][0]["ordinary_tweaks"].size()==0,"phaser nominal is the complete untouched definition");
    check(phaserCases.back().processed.personalities->instances.size()==12&&phaserCases.back().dry.layout.synthCount==4&&phaserCases.back().processed.layout.effectCount[airack::masterChain]==8,"phaser load has twelve actual instances/four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        auto dry=phaserCases[0].dry,processed=phaserCases[0].processed;for(auto* document:{&dry,&processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        auto brief=phaserCases[0];brief.hold=.25;const auto events=qualityEvents(brief,48000);const auto reference=renderFactoryNative(dry,events,48000,48000,quality),wet=renderFactoryNative(processed,events,48000,48000,quality);
        check(measureWindow(reference,{{0,48000}}).rms>1e-7&&difference(reference,wet)>1e-6,"actual default phaser changes the nonzero native carrier");
        auto zero=processed;setGainValue(zero,"effect.1.phaser.mix",0);check(difference(reference,renderFactoryNative(zero,events,48000,48000,quality))==0,"ordinary phaser Mix0 preserves native source");
        setGainValue(processed,"effect.1.bypassed",1);check(difference(reference,renderFactoryNative(processed,events,48000,48000,quality))==0,"ordinary phaser bypass preserves native source");
    }(); }
    }();
    [&] {
    const auto chorusCases=factoryChorusBriefs();check(chorusCases.size()==7,"chorus untouched default/five ordinary roles/actual eight-insert workload");
    const auto nominalChorus=chorusFactoryEvidence(chorusCases[0].processed);
    for(const auto& b:chorusCases){ [&] {airack::RoutedPatch patch;check(airack::makeRoutedPatch(b.processed,patch),"actual chorus factory maps native controls");
        for(int slot=0;slot<b.processed.layout.effectCount[airack::masterChain];++slot){check(patch.effects[std::size_t(slot)].algorithmRevision==airack::modernModulatedTapAlgorithmRevision&&b.processed.compatibility.effects[std::size_t(slot)][airack::chorusEffectBank]==airack::modernModulatedTapAlgorithmRevision,"actual placed Chorus2 agrees with retained bank6");}}(); }

    check(chorusCases[0].dry==factorySource(false)&&nominalChorus["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"chorus nominal carrier is untouched Copper Arc");
    check(nominalChorus["chorus"]["instances"].size()==1&&nominalChorus["chorus"]["instances"][0]["ordinary_tweaks"].size()==0,"chorus nominal is the complete untouched definition");
    check(chorusCases.back().processed.personalities->instances.size()==12&&chorusCases.back().dry.layout.synthCount==4&&chorusCases.back().processed.layout.effectCount[airack::masterChain]==8,"chorus load has twelve actual instances/four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        auto dry=chorusCases[0].dry,processed=chorusCases[0].processed;for(auto* document:{&dry,&processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        auto brief=chorusCases[0];brief.hold=.25;const auto events=qualityEvents(brief,48000);const auto reference=renderFactoryNative(dry,events,48000,48000,quality),wet=renderFactoryNative(processed,events,48000,48000,quality);
        check(measureWindow(reference,{{0,48000}}).rms>1e-7&&difference(reference,wet)>1e-6,"actual default chorus changes the nonzero native carrier");
        auto zero=processed;setGainValue(zero,"effect.1.chorus.mix",0);check(difference(reference,renderFactoryNative(zero,events,48000,48000,quality))==0,"ordinary chorus Mix0 preserves native source");
        setGainValue(processed,"effect.1.bypassed",1);check(difference(reference,renderFactoryNative(processed,events,48000,48000,quality))==0,"ordinary chorus bypass preserves native source");
    }(); }
    }();
    [&] {
    const auto filterCases=factoryFilterBriefs();check(filterCases.size()==7,"filter untouched default/five ordinary roles/actual eight-insert workload");
    const auto nominalFilter=filterFactoryEvidence(filterCases[0].processed);
    check(filterCases[0].dry==factorySource(false)&&nominalFilter["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"filter nominal carrier is untouched Copper Arc");
    check(nominalFilter["filter"]["instances"].size()==1&&nominalFilter["filter"]["instances"][0]["ordinary_tweaks"].size()==0,"filter nominal is the complete untouched definition");
    {const auto* binding=airack::rackUnitBinding(filterCases[0].processed.personalities->instances.back(),0);check(binding&&filterCases[0].processed.compatibility.effects[binding->slot][airack::filterEffectBank]==airack::modernFilterAlgorithmRevision,"actual placed Veil Current selects the Filter2 bank");}
    check(filterCases.back().processed.personalities->instances.size()==12&&filterCases.back().dry.layout.synthCount==4&&filterCases.back().processed.layout.effectCount[airack::masterChain]==8,"filter load has twelve actual instances/four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        auto dry=filterCases[0].dry,processed=filterCases[0].processed;for(auto* document:{&dry,&processed}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        const auto events=qualityEvents(filterCases[0],48000);const auto reference=renderFactoryNative(dry,events,48000,144000,quality),wet=renderFactoryNative(processed,events,48000,144000,quality);
        check(measureWindow(reference,{{0,144000}}).rms>1e-7&&difference(reference,wet)>1e-6,"actual default filter changes the nonzero native carrier");
        // Filter2 mixes inside its halfbands, so Mix0 keeps the source's level but
        // not its samples; Filter1's exact Mix0 stays in Routed effect_filter_transfer_and_live_clock.
        auto zero=processed;setGainValue(zero,"effect.1.filter_fx.mix",0);const auto unmixed=renderFactoryNative(zero,events,48000,144000,quality);
        const double level=20*std::log10(measureWindow(unmixed,{{0,144000}}).rms/measureWindow(reference,{{0,144000}}).rms);
        std::cout<<"FILTER2_MIX0 quality="<<int(quality)<<" level_db="<<level<<" difference="<<difference(reference,unmixed)<<'\n';
        check(std::abs(level)<=.01,"ordinary Filter2 Mix0 keeps the native source level through its halfbands");
        setGainValue(processed,"effect.1.bypassed",1);check(difference(reference,renderFactoryNative(processed,events,48000,144000,quality))==0,"ordinary filter bypass preserves native source");
    }(); }
    }();
    [&] {
    const auto eqCases=factoryEqBriefs();check(eqCases.size()==7,"EQ untouched default/five ordinary roles/actual eight-insert workload");
    const auto nominalEq=eqFactoryEvidence(eqCases[0].processed);
    check(eqCases[0].dry==factorySource(false)&&nominalEq["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"EQ nominal carrier is untouched actual Copper Arc");
    check(nominalEq["eq"]["instances"].size()==1&&nominalEq["eq"]["instances"][0]["ordinary_tweaks"].size()==0,"EQ nominal insert is the complete untouched factory definition");
    {const auto* binding=airack::rackUnitBinding(eqCases[0].processed.personalities->instances.back(),0);check(binding&&eqCases[0].processed.compatibility.effects[binding->slot][airack::eqEffectBank]==airack::modernEqAlgorithmRevision,"actual placed Contour Loom selects the EQ2 bank");}
    check(eqCases.back().processed.personalities->instances.size()==12&&eqCases.back().dry.layout.synthCount==4&&eqCases.back().processed.layout.effectCount[airack::masterChain]==8,"EQ load has twelve independent actual instances/four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        auto dry=eqCases[0].dry,transparent=eqCases[0].processed;for(auto* document:{&dry,&transparent}){setGainValue(*document,"output.gain_db",0);setGainValue(*document,"output.protection",0);}
        const auto events=qualityEvents(eqCases[0],48000);const auto reference=renderFactoryNative(dry,events,48000,144000,quality),zero=renderFactoryNative(transparent,events,48000,144000,quality);
        check(measureWindow(reference,{{0,144000}}).rms>1e-7&&difference(reference,zero)==0,"actual transparent EQ default is bit-value identical to nonzero native carrier");
        setGainValue(transparent,"effect.1.eq.bell.gain_db",9);setGainValue(transparent,"effect.1.eq.bell.frequency_hz",1000);
        check(difference(reference,renderFactoryNative(transparent,events,48000,144000,quality))>1e-6,"ordinary nonzero EQ edit changes actual placed factory response");
        setGainValue(transparent,"effect.1.bypassed",1);check(difference(reference,renderFactoryNative(transparent,events,48000,144000,quality))==0,"ordinary EQ bypass returns actual carrier");
    }(); }
    }();
    [&] {
    const auto coreFxCases=factoryCoreFxBriefs();check(coreFxCases.size()==14,"three untouched defaults/ten ordinary roles/actual eight-insert workload");
    for(std::size_t n=0;n<3;++n){const auto& b=coreFxCases[n];const auto evidence=coreFxEvidence(b.processed);check(b.dry==factorySource(false),"core-FX nominal carrier is untouched actual Copper Arc");
        check(evidence["carrier"]["instances"][0]["ordinary_tweaks"].size()==0,"core-FX nominal keeps carrier defaults");
        for(int i=0;i<3;++i)check(evidence["inserts"][i]["instances"].size()==(i==int(n)?1:0),"nominal contains exactly its authored selected insert");
        check(evidence["inserts"][int(n)]["instances"][0]["ordinary_tweaks"].size()==0,"nominal insert has no ordinary edits");
        if(n==0){const auto* binding=airack::rackUnitBinding(b.processed.personalities->instances.back(),0);check(binding&&b.processed.compatibility.effects[binding->slot][airack::compressorEffectBank]==airack::modernCompressorAlgorithmRevision,"actual placed Anchor Press selects the Compressor2 bank");}
        if(n==1){const auto* binding=airack::rackUnitBinding(b.processed.personalities->instances.back(),0);check(binding&&b.processed.compatibility.effects[binding->slot][airack::driveEffectBank]==airack::modernDriveAlgorithmRevision,"actual placed Cinder Coil selects the Drive2 bank");}
        if(n==2){const auto* binding=airack::rackUnitBinding(b.processed.personalities->instances.back(),0);check(binding&&b.processed.compatibility.effects[binding->slot][airack::delayEffectBank]==airack::modernDelayAlgorithmRevision,"actual placed Echo Thread selects the Delay2 bank");}}
    check(coreFxCases.back().dry.layout.synthCount==4&&coreFxCases.back().processed.layout.effectCount[airack::masterChain]==8,"authored core-FX workload actually fills four layers/eight inserts");
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher})for(std::size_t n=0;n<3;++n){ [&] {auto b=coreFxCases[n];b.hold=1.5;b.duration=2.5;
        for(auto* d:{&b.dry,&b.processed}){setGainValue(*d,"output.gain_db",0);setGainValue(*d,"output.protection",0);}
        const auto events=qualityEvents(b,48000);const int frames=120000;const auto dry=renderFactoryNative(b.dry,events,48000,frames,quality,true),wet=renderFactoryNative(b.processed,events,48000,frames,quality,true);
        auto bypass=b.processed;setGainValue(bypass,"effect.1.bypassed",1);check(difference(dry,renderFactoryNative(bypass,events,48000,frames,quality,true))<=1e-6,"actual authored bypass preserves source in both modes");
        check(difference(dry,wet)>1e-7,"actual authored insert has a nonzero native response");
        if(n==0){auto unity=b.processed;setGainValue(unity,"effect.1.compressor.ratio",1);check(difference(dry,renderFactoryNative(unity,events,48000,frames,quality,true))<=1e-6,"authored compressor ratio1 is transparent");
            auto makeup=b.processed;setGainValue(makeup,"effect.1.compressor.makeup_db",6);const auto raised=renderFactoryNative(makeup,events,48000,frames,quality,true);const double factor=measureWindow(raised,{{24000,72000}}).rms/measureWindow(wet,{{24000,72000}}).rms;
            std::cout<<"CORE_FX_MAKEUP quality="<<int(quality)<<" factor="<<factor<<'\n';check(std::abs(factor-std::pow(10.,6./20))<1e-5,"authored compressor +6dB makeup measured on settled paired native output");}
        if(n!=1){auto zero=b.processed;setGainValue(zero,n==0?"effect.1.compressor.mix":"effect.1.delay.mix",0);check(difference(dry,renderFactoryNative(zero,events,48000,frames,quality,true))<=1e-6,"authored compressor/delay Mix0 preserves measured native source");}
        // Drive Mix0 retains its existing adjacent-input averaging; old exact
        // dry/wet blend oracle remains in Routed controlRampsAndBoundaryCases.
    }(); }
    const auto calibration=coreFxProbeBriefs();check(calibration.size()==21,"bounded actual input threshold sweep");
    auto calibrationSource=factorySource(false);setGainValue(calibrationSource,"output.gain_db",0);setGainValue(calibrationSource,"output.protection",0);
    for(const auto& b:calibration){check(b.dry==calibrationSource&&b.hold==4&&b.duration==6&&b.notes==std::vector<int>{60},"fixed untouched Copper Arc calibration source and physical settling");
        const auto sourceEvidence=factoryEvidence(b.processed,false);check(sourceEvidence["instances"].size()==1&&sourceEvidence["instances"][0]["ordinary_tweaks"].size()==0,"calibration preserves actual factory authoring without unit tweaks");
        check(b.dry.layout.effectCount[airack::masterChain]==0&&b.processed.layout.effectCount[airack::masterChain]==1&&b.processed.layout.effects[0].kind==airack::EffectKind::compressor&&b.processed.compatibility.effects[0][3]==1,"calibration pairs actual dry input with only native1 compressor");}
    }();
    const auto qualityCases=qualityBriefs();check(qualityCases.size()==25,"all five synth, eight reverb, five compressor and two stress briefs");
    for(const auto& b:qualityCases){const auto events=qualityEvents(b,48000);check(!events.empty()&&events.back().sample<int(b.duration*48000),"quality MIDI fits captured duration");}
    for(const auto& b:qualityCases)if(b.role=="reverb"&&b.id.contains("chord")){ [&] {airack::RoutedPatch p;check(airack::makeRoutedPatch(b.dry,p)&&b.dry.layout.synthCount==2&&p.synth.layers[0].pan<0&&p.synth.layers[1].pan>0&&p.synth.layers[0].engine!=p.synth.layers[1].engine,"reverb chord has distinct stereo input engines");}(); }
    try { check(parseArguments({"--sound-quality", "--plugin", bundle.getFullPathName(), "--output", output.getFullPathName()}).output == output,
                "sound-quality admits fresh absolute output"); }
    catch (const std::exception&) { check(false, "sound-quality admits fresh absolute output"); }
    try { check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","higher"}).quality==airack::RenderQuality::higher,"explicit higher sound-quality job admitted"); }
    catch(const std::exception&) {check(false,"explicit higher sound-quality job admitted");}
    try {check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-analog"}).output==output,"factory analog selection admitted in existing sound-quality mode");}
    catch(const std::exception&) {check(false,"factory analog selection admitted in existing sound-quality mode");}
    try {const auto selected=parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-reverb"});check(selected.output==output&&selected.factoryReverb&&!selected.factoryDigital&&!selected.factoryAnalog,"actual reverb factory selection admitted in existing sound-quality mode");}
    catch(const std::exception&) {check(false,"actual reverb factory selection admitted in existing sound-quality mode");}
    try {const auto selected=parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--factory-digital"});check(selected.output==output&&selected.factoryDigital&&!selected.factoryAnalog,"factory digital selection admitted in existing sound-quality mode");}
    catch(const std::exception&) {check(false,"factory digital selection admitted in existing sound-quality mode");}
    try {const auto selected=parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--wavetable-warmth"});check(selected.output==output&&selected.wavetableWarmth&&!selected.factoryDigital,"wavetable warmth listening selection admitted in sound-quality mode");}
    catch(const std::exception&) {check(false,"wavetable warmth listening selection admitted in sound-quality mode");}
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--wavetable-warmth"});},"wavetable warmth selection outside sound-quality rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--wavetable-warmth","--wavetable-warmth"});},"duplicate wavetable warmth selection rejected");
    for(const auto* other:{"--factory-digital","--factory-analog","--reverb-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--wavetable-warmth",reverse?"--wavetable-warmth":other});},"mixed wavetable warmth selection rejected in both orders");
    { [&] {
        const auto pack=warmthRoles();check(pack.size()==5,"warmth pack holds the pad, swell, reese, vowel choir and pluck");
        std::array<airack::RoutedPatch,5> patches;
        for(std::size_t n=0;n<pack.size()&&n<5;++n){check(airack::validateSoundDocument(pack[n].document)&&airack::makeRoutedPatch(pack[n].document,patches[n])&&!pack[n].notes.empty(),"warmth role is a valid document with notes");
            const auto& l=patches[n].synth.layers[0];check(l.enabled&&l.engine==airack::EngineKind::wavetable&&l.algorithmRevision==airack::modernSynthAlgorithmRevision&&!patches[n].synth.layers[1].enabled,"warmth role plays one revision-2 wavetable layer");}
        if(pack.size()!=5)return;
        const auto approx=[](float a,float b){return std::abs(a-b)<=1e-4f*std::max(1.f,std::abs(b));};
        const auto& pad=patches[0].synth.layers[0];
        check(pad.oscillators[0].unison==5&&approx(pad.oscillators[0].unisonCents,15)&&approx(pad.oscillators[0].position,.45f)&&pad.oscillators[1].unison==1&&approx(pad.oscillators[1].tune,-12)&&approx(pad.oscillators[1].level,.25f),"pad is the recipe's oscillators");
        check(approx(pad.unisonWidth,.7f)&&approx(pad.drift,.35f)&&pad.filterType==airack::FilterType::classic&&approx(pad.cutoffHz,1200)&&approx(pad.resonance,.15f)&&approx(pad.ampEnvelope.attack,1.5f)&&approx(pad.attackCurve,-.5f)&&approx(pad.ampEnvelope.release,3),"pad is the recipe's warmth, filter and swell");
        check(pad.modulation[0].enabled&&pad.modulation[0].source==airack::ModSource::lfo1&&pad.modulation[0].target==airack::ModTarget::osc1Position&&approx(pad.modulation[0].amount,.3f)&&approx(pad.lfos[0].rateHz,.08f)&&pad.modulation[1].source==airack::ModSource::velocity&&pad.modulation[1].target==airack::ModTarget::cutoff,"pad carries the recipe's sweep and velocity routes");
        check(approx(patches[1].synth.layers[0].ampEnvelope.attack,6)&&approx(patches[1].synth.layers[0].attackCurve,-1),"swell is a 6 s attack at curve -1");
        const auto& reese=patches[2].synth.layers[0];
        check(reese.oscillators[0].unison==7&&approx(reese.oscillators[0].unisonCents,22)&&approx(reese.unisonWidth,.4f)&&reese.filterType==airack::FilterType::ladder&&approx(reese.cutoffHz,600)&&approx(reese.resonance,.2f)&&reese.oscillators[1].unison==1&&approx(reese.oscillators[1].position,0)&&approx(reese.oscillators[1].tune,-12),"reese is 7 ladder voices over a clean sub");
        const auto& choir=patches[3].synth.layers[0];
        check(choir.filterType==airack::FilterType::vowel&&approx(choir.cutoffHz,700)&&approx(choir.vowelSize,1.2f)&&approx(choir.resonance,.6f)&&approx(choir.lfos[1].rateHz,.2f)&&choir.modulation[2].enabled&&choir.modulation[2].source==airack::ModSource::lfo2&&choir.modulation[2].target==airack::ModTarget::cutoff&&approx(choir.modulation[2].amount,.15f),"vowel choir glides its vowel with LFO 2");
        const auto& pluck=patches[4].synth.layers[0];
        check(pluck.filterType==airack::FilterType::classic&&approx(pluck.cutoffHz,2200)&&approx(pluck.ampEnvelope.attack,.002f)&&approx(pluck.attackCurve,1)&&approx(pluck.ampEnvelope.decay,.35f)&&approx(pluck.ampEnvelope.sustain,0)&&approx(pluck.ampEnvelope.release,.4f),"pluck is a fast Classic pluck");
        for(const auto& role:pack){const auto events=warmthEvents(role,48000);check(events.size()==2*role.notes.size()&&events.back().sample<int(role.seconds*48000),"every warmth note ends inside its render");}
    }(); }
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-reverb"});},"reverb factory selection outside sound-quality rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-reverb","--factory-reverb"});},"duplicate reverb factory selector rejected");
    for(const auto* other:{"--factory-analog","--factory-digital","--reverb-only","--reverb-probes-only"})for(bool reverse:{false,true})
        rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-reverb",reverse?"--factory-reverb":other});},"mixed reverb factory selector rejected in both orders");
    const auto reverbCases=factoryReverbBriefs();check(reverbCases.size()==10,"actual reverb factory nominal/eight roles/full-load selected");
    const auto nominalReverbEvidence=factoryEvidence(reverbCases[0].processed,reverbFactory());
    check(nominalReverbEvidence["instances"].size()==1&&nominalReverbEvidence["instances"][0]["ordinary_tweaks"].size()==0,"nominal factory reverb carries untouched author defaults");
    check(reverbCases[0].processed==placeReverbFactory(reverbCases[0].dry),"nominal wet source is exactly actual factory placement");
    check(reverbCases.back().dry.layout.synthCount==4&&reverbCases.back().notes.size()==16&&reverbCases.back().processed.personalities->instances.size()==8,"full reverb factory evidence uses independent actual eight placements");
    for(const auto& b:reverbCases){check(!b.dry.personalities,"factory reverb dry source contains no FX design");for(const auto& instance:b.processed.personalities->instances){check(instance.design==reverbFactory(),"role and load edits preserve factory definition");const auto* binding=airack::rackUnitBinding(instance,0);check(binding&&b.processed.compatibility.effects[binding->slot][2]==2,"actual placed factory selects retained Reverb2 bank");}factoryEvidence(b.processed,reverbFactory());}
    const auto probeSource=reverbAnalysisBriefs()[8].dry;auto mixed=placeReverbFactory(probeSource);setGainValue(mixed,"effect.1.reverb.mix",0);
    const auto noiseEvents=qualityEvents(reverbAnalysisBriefs()[8],48000);
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){ [&] {
        const auto dry=renderFactoryNative(probeSource,noiseEvents,48000,48000,quality),zero=renderFactoryNative(mixed,noiseEvents,48000,48000,quality);
        check(difference(dry,zero)==0,"actual factory Mix binding preserves dry endpoint");setGainValue(mixed,"effect.1.reverb.mix",1);
        const auto wet=renderFactoryNative(mixed,noiseEvents,48000,48000,quality);check(measureWindow(wet,{{0,48000}}).rms>1e-10&&difference(dry,wet)>1e-6,"actual factory wet response is finite nonzero and differs from dry");setGainValue(mixed,"effect.1.reverb.mix",0);
    }(); }
    try {check(parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","realtime","--reverb-only"}).output==output,"bounded reverb-only production evidence admitted in existing mode");}
    catch(const std::exception&) {check(false,"bounded reverb-only production evidence admitted in existing mode");}
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--reverb-only","--factory-analog"});},"mixed reverb/factory selectors rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-digital","--reverb-only"});},"reverse mixed factory/reverb selectors rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--reverb-only","--reverb-only"});},"duplicate reverb selector rejected");
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--reverb-only"});},"reverb selector rejected outside existing quality mode");
    { [&] {
        const auto analysis=reverbAnalysisBriefs();check(analysis.size()==13,"reverb evidence bounded to existing eight briefs and five source/control probes");
        for(const auto& brief:analysis){airack::RoutedPatch dry;airack::RoutedPatch wet;check(airack::makeRoutedPatch(brief.dry,dry)&&airack::makeRoutedPatch(brief.processed,wet)&&dry.outputGainDb==0&&!dry.outputProtection&&wet.outputGainDb==0&&!wet.outputProtection,"linear analysis uses explicit physical0dB/protectionOFF");}
        const auto& probe=analysis[8];for(int rate:{44100,48000,96000}){const auto events=qualityEvents(probe,rate);check(events.size()==2&&events[0].sample==0&&events[1].sample==1&&events[1].message.isNoteOff(),"known source releases at actual hostframe1 at each rate");
            const int ppq=24000,tempo=int(std::llround(double(ppq)*1000000/rate));writeMidiEvents(root.getChildFile("sample-tick-noise-"+juce::String(rate)+".mid"),events,ppq,tempo);check(true,"one-frame probe MIDI readback preserves hostframe0/1 ticks with explicit tempo quantization");}
        const auto source=renderFactoryNative(probe.dry,qualityEvents(probe,48000),48000,4096,airack::RenderQuality::realtime);
        check(source.getMagnitude(0,1)>1e-8f&&source.getMagnitude(1,4095)==0,"actual realtime noise source is nonzero frame0 and zero after frame1");
    }(); }
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-analog","--factory-digital"});},"mixed analog/digital selection rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-digital","--factory-analog"});},"mixed digital/analog selection rejected");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-digital","--factory-digital"});},"duplicate digital selection rejected");
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-digital"});},"digital selection outside sound-quality rejected");
    rejects([&]{parseArguments({"--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-analog"});},"factory selection restricted to existing sound-quality mode");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-analog","--factory-analog"});},"duplicate factory selection rejected");
    for(bool digital:{false,true}){ [&] {const auto factoryCases=factoryBriefs(digital);check(factoryCases.size()==7,"untouched default, five ordinary settings and full factory workload");
    const juce::String role=digital?"factory-digital":"factory-analog",bank=digital?"synth.1.wavetable.":"synth.1.analog.";
    const auto nominal=std::find_if(factoryCases.begin(),factoryCases.end(),[&](const auto& b){return b.id==role+"-starting-default";});
    check(nominal!=factoryCases.end()&&nominal->dry==factorySource(digital),"nominal audition uses untouched actual factory placement/defaults");
    if(nominal!=factoryCases.end())for(const auto* d:{&nominal->dry,&nominal->processed}){const auto evidence=factoryEvidence(*d,digital);check(evidence["instances"][0]["ordinary_tweaks"].size()==0,"nominal factory audition has no ordinary unit tweaks");}
    check(factoryCases.back().dry.layout.synthCount==4&&factoryCases.back().processed.layout.effectCount[airack::masterChain]==8&&factoryCases.back().notes.size()==16,"factory full voices/layers/FX use real duplicated designs");
    for(const auto& b:factoryCases){check(b.dry.personalities&&b.dry.personalities->instances[0].design==qualityFactory(digital),"same original factory identity and definition across ordinary settings");factoryEvidence(b.dry,digital);factoryEvidence(b.processed,digital);}
    for(auto quality:{airack::RenderQuality::realtime,airack::RenderQuality::higher}){
        auto low=factorySource(digital),high=low;setGainValue(low,bank+"filter.cutoff",20);setGainValue(high,bank+"filter.cutoff",14000);
        const std::vector<Event> events{{0,juce::MidiMessage::noteOn(1,60,juce::uint8(100))}};const auto dark=renderFactoryNative(low,events,48000,48000,quality),bright=renderFactoryNative(high,events,48000,48000,quality);
        const auto band=[&](const auto& audio){return airack::rackPersonalityFixtures::highBand(audio,[&](bool ok){check(ok,"factory cutoff response carries signal");});};
        const double darkBand=band(dark),brightBand=band(bright);check(brightBand>4*darkBand+.001,"actual factory brightness control changes high-band energy");
        auto slow=high;setGainValue(high,bank+"amp.attack",.001f);setGainValue(slow,bank+"amp.attack",2);const auto fastAttack=renderFactoryNative(high,events,48000,48000,quality),slowAttack=renderFactoryNative(slow,events,48000,48000,quality);
        check(airack::rackPersonalityFixtures::energy(fastAttack,0,2880)>5*airack::rackPersonalityFixtures::energy(slowAttack,0,2880),"actual factory attack binding preserves articulation");
        std::cout<<"FACTORY_RESPONSE digital="<<digital<<" quality="<<int(quality)<<" cutoff_dark="<<darkBand<<" cutoff_bright="<<brightBand<<'\n';
        if(digital){auto sine=factorySource(true);setGainValue(sine,bank+"osc.2.level",0);setGainValue(sine,"synth.1.modulation.1.amount",0);setGainValue(sine,bank+"osc.1.position",0);auto spectral=sine;setGainValue(spectral,bank+"osc.1.position",1);
            const double sineBand=band(renderFactoryNative(sine,events,48000,48000,quality)),spectralBand=band(renderFactoryNative(spectral,events,48000,48000,quality));check(spectralBand>4*sineBand+.001,"actual factory Color binding changes spectral density");std::cout<<"FACTORY_SPECTRUM quality="<<int(quality)<<" sine="<<sineBand<<" spectral="<<spectralBand<<'\n';}}
    }(); }
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--quality","unknown"});},"unknown quality rejected before output creation");
    rejects([&]{parseArguments({"--sound-quality","--rack-personalities","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName()});},"mixed sound-quality mode rejected");
    { [&] {
        airack::ProjectDocument expected;expected.sound=personalityFixture(0,0);juce::MemoryBlock project;require(airack::encodeProjectState(expected,project),"personality selftest setup");
        verifyPersonalityProject(hostEnvelope(project),expected);
        auto drift=expected;auto metadata=std::make_shared<airack::RackPersonalityState>(*drift.sound.personalities);metadata->instances[0].design.name="Incorrect embedded metadata";drift.sound.personalities=metadata;
        rejects([&]{verifyPersonalityProject(hostEnvelope(project),drift);},"fixture metadata drift rejected");
        auto extra=project;extra.append("x",1);rejects([&]{verifyPersonalityProject(hostEnvelope(extra),expected);},"trailing actual owned bytes rejected");
        auto damaged=project;static_cast<char*>(damaged.getData())[24]=3;
        juce::MemoryInputStream header(project.getData(),project.getSize(),false);header.setPosition(12);const auto soundSize=header.readInt();
        auto* data=static_cast<char*>(damaged.getData());auto digest=juce::SHA256(data+20,std::size_t(soundSize)-32).getRawData();std::memcpy(data+20+soundSize-32,digest.getData(),32);digest=juce::SHA256(data,damaged.getSize()-32).getRawData();std::memcpy(data+damaged.getSize()-32,digest.getData(),32);
        rejects([&]{verifyPersonalityProject(hostEnvelope(damaged),expected);},"wrong nested version with valid hashes rejected");
        damaged=project;data=static_cast<char*>(damaged.getData());data[12]^=1;digest=juce::SHA256(data,damaged.getSize()-32).getRawData();std::memcpy(data+damaged.getSize()-32,digest.getData(),32);
        rejects([&]{verifyPersonalityProject(hostEnvelope(damaged),expected);},"wrong nested size with valid outer hash rejected");
        rejects([&]{parseArguments({"--rack-personalities","--measure-gain","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName()});},"mixed personality mode rejected");
    }(); }
    try { check(parseArguments({"--rack-personalities", "--plugin", bundle.getFullPathName(), "--output", output.getFullPathName()}).output == output,
                "personality mode admits fresh absolute output"); }
    catch (const std::exception&) { check(false, "personality mode admits fresh absolute output"); }
    try {
        const auto gain = parseArguments({"--measure-gain", "--plugin", bundle.getFullPathName(), "--output", output.getFullPathName()});
        check(gain.gain && gain.plugin == bundle && gain.output == output, "explicit gain mode accepted");
    } catch (const std::exception&) { check(false, "explicit gain mode accepted"); }
    check(!parseArguments(args).gain, "legacy render mode retained");
    try {check(parseArguments({"--compare-legacy-gain","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName()}).gain,"bounded legacy comparison mode accepted");}
    catch(const std::exception&){check(false,"bounded legacy comparison mode accepted");}
    const auto modern=gainCases();check(modern.size()==20,"modern calibrated and historical case set complete");
    for(const auto& c:modern){
        const bool historical=c.name.startsWith("historical-")||c.name.startsWith("original-");
        const float wanted=historical?0.f:c.name.endsWith("-dry")?12.f:c.name=="full-normal-effects"?24.f:30.f;
        check(std::abs(airack::parameterPhysicalValue(c.document.values,756)-wanted)<1e-5f,"case uses explicit calibrated or neutral gain");
        check((c.document.values[757]==1)==!historical,"case uses explicit calibrated or neutral protection");
    }

    rejects([&] { parseArguments({}); }, "missing arguments rejected");
    rejects([&] { parseArguments({"--plugin", "relative.vst3", "--output", output.getFullPathName()}); }, "relative plugin rejected");
    rejects([&] { parseArguments({"--plugin", bundle.getFullPathName(), "--output", "relative"}); }, "relative output rejected");
    rejects([&] { parseArguments({"--plugin", root.getChildFile("absent.vst3").getFullPathName(), "--output", output.getFullPathName()}); }, "missing plugin rejected");
    rejects([&] { parseArguments({"--plugin", root.getFullPathName(), "--output", output.getFullPathName()}); }, "non-bundle rejected");
    rejects([&] { parseArguments({"--plugin", bundle.getFullPathName(), "--output", root.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("outside-render").getFullPathName()}); }, "outside workspace output rejected");
    auto extra = args; extra.add("--unknown"); rejects([&] { parseArguments(extra); }, "extra argument rejected");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-frequency-shift"});check(true,"singleton frequency-shift factory selector");} catch(const std::exception&) {check(false,"singleton frequency-shift factory selector");}
    for(const auto* other:{"--factory-frequency-shift","--factory-dimension","--factory-stereo","--factory-flanger","--factory-phaser","--factory-chorus","--factory-filter","--factory-eq"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-frequency-shift",reverse?"--factory-frequency-shift":other});},"mixed frequency-shift selector rejects");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-convolution"});check(true,"singleton convolution factory selector");} catch(const std::exception&) {check(false,"singleton convolution factory selector");}
    for(const auto* other:{"--factory-convolution","--factory-frequency-shift","--factory-dimension","--factory-stereo","--factory-flanger","--factory-phaser","--factory-chorus","--factory-filter","--factory-eq"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-convolution",reverse?"--factory-convolution":other});},"mixed convolution selector rejects");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-frequency-split"});check(true,"singleton frequency split factory selector");} catch(const std::exception&) {check(false,"singleton frequency split factory selector");}
    for(const auto* other:{"--factory-frequency-split","--factory-convolution","--factory-frequency-shift","--factory-dimension","--factory-stereo","--factory-flanger","--factory-phaser","--factory-chorus","--factory-filter","--factory-eq"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-frequency-split",reverse?"--factory-frequency-split":other});},"mixed frequency split selector rejects");
    try {parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),"--factory-mid-side-split"});check(true,"singleton mid side split factory selector");} catch(const std::exception&) {check(false,"singleton mid side split factory selector");}
    for(const auto* other:{"--factory-mid-side-split","--factory-frequency-split","--factory-convolution","--factory-frequency-shift","--factory-dimension","--factory-stereo","--factory-flanger","--factory-phaser","--factory-chorus","--factory-filter","--factory-eq"})for(bool reverse:{false,true})rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName(),reverse?other:"--factory-mid-side-split",reverse?"--factory-mid-side-split":other});},"mixed mid side split selector rejects");
    require(output.createDirectory().wasOk(), "empty output creation failed");
    rejects([&]{parseArguments({"--sound-quality","--plugin",bundle.getFullPathName(),"--output",output.getFullPathName()});},"stale empty quality output rejected");
    check(parseArguments(args).output == output, "existing empty output accepted");
    auto sentinel = output.getChildFile("preserve.txt");
    require(sentinel.replaceWithText("preserved"), "sentinel write failed");
    rejects([&] { parseArguments(args); }, "nonempty output rejected");
    check(sentinel.loadFileAsString() == "preserved", "rejected output preserves existing file");
    for (int rate : {44100, 48000, 96000})
    {
        const auto events = phrase(rate);
        check(events.size() == 8, "four note-on/off pairs");
        if (events.size() == 8)
            for (int i = 0; i < 4; ++i)
            {
                constexpr int notes[] {48, 52, 55, 60};
                check(events[2*i].sample == i * rate / 2, "note onset sample");
                check(events[2*i+1].sample == i * rate / 2 + rate / 4, "note duration samples");
                check(events[2*i].message.isNoteOn() && events[2*i].message.getNoteNumber() == notes[i]
                    && events[2*i].message.getVelocity() == 100, "note identity and velocity");
                check(events[2*i+1].message.isNoteOff() && events[2*i+1].message.getNoteNumber() == notes[i], "matching note-off");
            }
    }
    juce::AudioBuffer<float> audio(2, 8); audio.clear();
    rejects([&] { measure(audio, 6); }, "silent render rejected");
    audio.setSample(0, 0, .5f); audio.setSample(1, 0, -.5f);
    auto m = measure(audio, 6);
    check(m.peak == .5 && std::abs(m.rms - std::sqrt(.5/16)) < 1e-12 && m.tailPeak == 0, "unnormalized peak RMS and tail");
    juce::AudioBuffer<float> copy; copy.makeCopyOf(audio);
    check(difference(audio, copy) == 0, "identical samples compare equal");
    copy.setSample(1, 3, .125f);
    check(difference(audio, copy) == .125, "sample mismatch measured");
    audio.setSample(0, 7, .01f); rejects([&] { measure(audio, 6); }, "unsettled tail rejected");
    audio.setSample(0, 7, 0); audio.setSample(0, 0, 1); rejects([&] { measure(audio, 6); }, "clipping rejected");
    audio.setSample(0, 0, std::numeric_limits<float>::quiet_NaN()); rejects([&] { measure(audio, 6); }, "NaN rejected");
    rejects([&] { difference(audio, copy); }, "nonfinite comparison rejected");
    audio.setSample(0, 0, std::numeric_limits<float>::infinity()); rejects([&] { measure(audio, 6); }, "infinity rejected");
    juce::AudioBuffer<float> empty;
    rejects([&] { measure(empty, 0); }, "empty audio rejected");
    rejects([&] { difference(empty, copy); }, "mismatched dimensions rejected");
    // Hand-derived asymmetric samples catch omitted channels, incorrect divisors and interval edges.
    juce::AudioBuffer<float> known(2, 4);
    for (int i = 0; i < 4; ++i) { known.setSample(0, i, .5f); known.setSample(1, i, -1.0f); }
    auto whole = measureWindow(known, {{0, 4}});
    check(whole.samples == 8 && whole.peak == 1 && std::abs(whole.rms - std::sqrt(.625)) < 1e-12, "stereo sample count peak RMS");
    check(whole.dc == -.25 && whole.excursions == 4 && whole.channelExcursions[0] == 0 && whole.channelExcursions[1] == 4, "signed DC and inclusive clipping count");
    check(whole.channelPeak[0] == .5 && whole.channelPeak[1] == 1 && whole.channelRms[0] == .5 && whole.channelRms[1] == 1
        && whole.channelDc[0] == .5 && whole.channelDc[1] == -1, "asymmetric per-channel metrics");
    known.clear(); known.setSample(0, 0, 1); known.setSample(1, 1, -2); known.setSample(0, 2, .5f); known.setSample(1, 3, .25f);
    auto partial = measureWindow(known, {{1, 3}});
    check(partial.samples == 4 && partial.peak == 2 && partial.dc == -.375 && std::abs(partial.rms - std::sqrt(4.25/4)) < 1e-12, "partial half-open window");
    auto united = measureWindow(known, {{0, 1}, {3, 4}});
    check(united.samples == 4 && united.peak == 1 && united.dc == .3125 && std::abs(united.rms - std::sqrt(1.0625/4)) < 1e-12, "disjoint union excludes gaps and includes last sample");
    auto last = measureWindow(known, {{3, 4}});
    check(last.samples == 2 && last.peak == .25 && last.channelPeak[1] == .25, "last stereo sample boundary");
    check(measureWindow(known, {{0, 2}, {2, 4}}).samples == 8, "adjacent intervals accepted without duplication");
    rejects([&] { measureWindow(known, {}); }, "empty interval union rejected");
    rejects([&] { measureWindow(known, {{1, 1}}); }, "zero length interval rejected");
    rejects([&] { measureWindow(known, {{2, 1}}); }, "reversed interval rejected");
    rejects([&] { measureWindow(known, {{0, 3}, {2, 4}}); }, "overlap rejected");
    rejects([&] { measureWindow(known, {{3, 4}, {0, 1}}); }, "out of order intervals rejected");
    rejects([&] { measureWindow(known, {{-1, 2}}); }, "negative boundary rejected");
    rejects([&] { measureWindow(known, {{0, 5}}); }, "outside boundary rejected");
    rejects([&] { measureWindow(empty, {{0, 1}}); }, "empty measurement buffer rejected");
    juce::AudioBuffer<float> mono(1, 4); mono.clear();
    rejects([&] { measureWindow(mono, {{0, 4}}); }, "nonstereo measurement rejected");
    known.clear(); auto silent = measureWindow(known, {{0, 4}});
    check(silent.samples == 8 && silent.peak == 0 && silent.rms == 0 && silent.dc == 0 && silent.excursions == 0, "silence is valid measured data");
    check(dbfs(0).isVoid() && std::abs(static_cast<double>(dbfs(.5)) + 6.020599913279624) < 1e-12, "silence null and finite dBFS");
    known.setSample(1, 3, std::numeric_limits<float>::quiet_NaN());
    rejects([&] { measureWindow(known, {{0, 4}}); }, "measurement NaN rejected");
    known.setSample(1, 3, std::numeric_limits<float>::infinity());
    rejects([&] { measureWindow(known, {{3, 4}}); }, "measurement infinity rejected at last sample");
    // The AI briefs' playback: the brief's notes at its tempo, a released note before a restarted one.
    [&] {
        const auto* a=airack::listening::aiBrief("A");const auto* b=airack::listening::aiBrief("B");check(a&&b&&!airack::listening::aiBrief("C"),"AI briefs A and B only");
        if(a&&b){
            const auto ea=aiBriefEvents(*a,60.0*48000/100);check(ea.size()==16,"brief A plays 8 notes");
            bool order=ea.size()==16;for(int n=0;n<4&&order;++n)order=ea[std::size_t(n)].sample==0&&ea[std::size_t(n)].message.isNoteOn()&&ea[std::size_t(n)].message.getVelocity()==70;
            for(int n=4;n<8&&order;++n)order=ea[std::size_t(n)].sample==230400&&ea[std::size_t(n)].message.isNoteOff();
            for(int n=8;n<12&&order;++n)order=ea[std::size_t(n)].sample==230400&&ea[std::size_t(n)].message.isNoteOn();
            for(int n=12;n<16&&order;++n)order=ea[std::size_t(n)].sample==460800&&ea[std::size_t(n)].message.isNoteOff();
            check(order,"brief A: the first chord's release precedes the second chord at beat 8, 100 BPM");
            const auto eb=aiBriefEvents(*b,60.0*48000/120);check(eb.size()==24,"brief B plays 12 notes");
            bool line=eb.size()==24;for(std::size_t n=0;n<eb.size()&&line;n+=2){const int note=int(n/2),start=(note/6)*96000+(note%6)*12000;
                line=eb[n].sample==start&&eb[n].message.isNoteOn()&&eb[n].message.getVelocity()==(note%2?70:90)&&eb[n+1].sample==start+6000&&eb[n+1].message.isNoteOff();}
            check(line,"brief B: eighth-note line, sixteenth-note lengths, 90/70, repeated at beat 4, 120 BPM");
            const auto midi=aiBriefEvents(*a,480);check(!midi.empty()&&midi.back().sample==16*480,"brief A's MIDI ends at 16 beats of 480 ticks");
            try{writeMidiEvents(root.getChildFile("brief-b.mid"),aiBriefEvents(*b,480),480,500000);check(true,"brief B's MIDI reads back with its 90/70 velocities");}catch(const std::exception&){check(false,"brief B's MIDI reads back with its 90/70 velocities");}
            // What the AI changed, as the listener reads it: racks by name, and each moved control in its own units.
            const auto before=airack::listening::aiBriefSource(*a).sound;const int racks=before.personalities?int(before.personalities->instances.size()):0;
            check(racks>0,"brief A's source carries its rack");
            const auto same=aiBriefChanges(before,before);
            check(same["controls"].size()==0&&same["racks_placed"].size()==0&&same["racks_removed"].size()==0&&same["macros_changed"].size()==0&&same["layout_changed"].isBool()&&!bool(same["layout_changed"]),"an unchanged answer reports no change");
            check(racks>0&&same["racks_kept"].size()==racks&&same["racks_kept"][0]["name"].toString()==before.personalities->instances[0].design.name&&same["racks_kept"][0]["design_changed"].isBool()&&!bool(same["racks_kept"][0]["design_changed"]),"an unchanged answer keeps its racks by name");
            auto louder=before;louder.values[756]=.5f;const auto gain=aiBriefChanges(before,louder);
            check(gain["controls"].size()==1&&gain["controls"][0]["id"].toString()=="output.gain_db"&&std::abs(double(gain["controls"][0]["before"])-24)<1e-3
                &&double(gain["controls"][0]["after"])==double(airack::parameterPhysicalValue(louder.values,756)),"a moved control reports its physical values before and after");
            auto bare=before;bare.personalities=nullptr;
            check(racks>0&&aiBriefChanges(before,bare)["racks_removed"].size()==racks&&aiBriefChanges(before,bare)["racks_placed"].size()==0,"a removed rack is reported");
            check(racks>0&&aiBriefChanges(bare,before)["racks_placed"].size()==racks&&aiBriefChanges(bare,before)["racks_removed"].size()==0,"a placed rack is reported");
            auto bound=before;bound.macros[0].amount=before.macros[0].amount==.5f?.25f:.5f;const auto macro=aiBriefChanges(before,bound);
            check(macro["macros_changed"].size()==1&&int(macro["macros_changed"][0]["index"])==1&&double(macro["macros_changed"][0]["after_amount"])==double(bound.macros[0].amount),"a changed macro binding is reported");
        }
        const auto briefOutput=root.getChildFile("ai-brief-output");
        auto briefArgs=juce::StringArray{"--ai-brief","--plugin",bundle.getFullPathName(),"--output",briefOutput.getFullPathName(),"--input",root.getFullPathName(),"--quality","higher"};
        try{const auto parsed=parseAiBriefArguments(briefArgs);check(parsed.quality==airack::RenderQuality::higher&&parsed.input==root,"AI brief arguments admitted");}catch(const std::exception&){check(false,"AI brief arguments admitted");}
        for(int drop:{8,6}){auto shorter=briefArgs;shorter.removeRange(drop,1);rejects([&]{parseAiBriefArguments(shorter);},"AI brief arguments incomplete");}
        auto unknown=briefArgs;unknown.set(8,"best");rejects([&]{parseAiBriefArguments(unknown);},"AI brief quality unknown");
        auto relative=briefArgs;relative.set(6,"relative");rejects([&]{parseAiBriefArguments(relative);},"AI brief input relative");
        auto outside=briefArgs;outside.set(6,juce::File(RACKSYNTH_WORKSPACE).getParentDirectory().getFullPathName());rejects([&]{parseAiBriefArguments(outside);},"AI brief input outside the workspace");
        check(!briefOutput.exists(),"AI brief parsing creates no output");
    }();
    std::cout << "RackSynthRender self-test: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
}

#include "AlphaSmoke.h"
#include "FactorySoundsRender.h"
int main(int argc, char** argv)
{
    try
    {
        const bool selfTestDiagnostics = argc == 2 && std::string_view(argv[1]) == "--self-test";
        if (selfTestDiagnostics)
        {
            std::cout.setf(std::ios::unitbuf);
            std::cout << "RENDER_SELFTEST_PHASE juce_initialization_begin\n";
        }
        juce::ScopedJuceInitialiser_GUI juce;
        if (selfTestDiagnostics) std::cout << "RENDER_SELFTEST_PHASE juce_initialization_complete\n";
        juce::StringArray args;
        for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
        if (args == juce::StringArray {"--self-test"})
        {
            identitySelfTest();
            alphaSmokeSelfTest();
            std::cout << "RENDER_SELFTEST_PHASE pattern_begin\n";
            const int patterns = patternRenderSelfTest();
            std::cout << "RENDER_SELFTEST_PHASE pattern_complete native_self_test_begin\n";
            const int owned=selfTest();
            std::cout << "RENDER_SELFTEST_PHASE native_self_test_complete candidate_begin\n";
            const int candidate=candidateReplaySelfTest();
            std::cout << "RENDER_SELFTEST_PHASE candidate_complete\n";
            return owned||patterns||candidate;
        }
        if(args==juce::StringArray{"--identity-self-test"})return identitySelfTest();
        if(args==juce::StringArray{"--alpha-smoke-self-test"})return alphaSmokeSelfTest();
        if(args.size()>0&&args[0]=="--alpha-smoke")return renderAlphaSmoke(args);
        if(args.size()>0&&args[0]=="--factory-sounds")return renderFactorySounds(args);
        if(args.size()>0&&args[0]=="--space-reverb")return renderSpaceReverb(parseSpaceReverbArguments(args));
        if(args.size()>0&&args[0]=="--era-reverb")return renderEraReverb(parseEraReverbArguments(args));
        if(args==juce::StringArray{"--candidate-self-test"})return candidateReplaySelfTest();
        if(args.size()>0&&args[0]=="--spectral-listening"){args.remove(0);return renderSpectralListening(parseArguments(args));}
        if(args.size()>0&&args[0]=="--spectral-integration"){args.remove(0);return renderSpectralIntegration(parseArguments(args));}
        if(args.size()>0&&args[0]=="--probe-eq-sidechain-buses")return probeEqSidechainBuses(args);
        if(args.size()>0&&args[0]=="--candidate")return renderCandidateReplay(parseCandidateArguments(args));
        if(args.size()>0&&args[0]=="--ai-brief")return renderAiBrief(parseAiBriefArguments(args));
        if(args.size()>0 && (args[0]=="--patterns" || args[0]=="--patterns-smoke"))return renderPatterns(parsePatternArguments(args));
        const auto parsed = parseArguments(args);
        if(parsed.drumProduct)return renderDrumProduct(parsed);
        if(parsed.personalities)return renderRackPersonalities(parsed);
        if(parsed.soundQuality)return renderSoundQuality(parsed);
        return parsed.legacyGain?compareLegacyGain(parsed):parsed.gain ? measureGain(parsed) : renderFixtures(parsed);
    }
    catch (const std::exception& e) { std::cerr << "RackSynthRender: " << juce::String(e.what()).substring(0, 512) << '\n'; return 1; }
}
