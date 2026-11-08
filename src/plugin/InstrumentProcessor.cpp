#include "SessionMarker.h"
#include "InstrumentProcessor.h"
#include <juce_cryptography/juce_cryptography.h>
#include "ParameterListenerRetirement.h"
#include "DrumImport.h"
#include <type_traits>
#include "FxMonitor.h"
#include "RackLibrary.h"
#include "RackExamples.h"
#include "InstrumentEditor.h"
#include "MakerFace.h"
#include <condition_variable>
#include <thread>
#include <cmath>
#include "PatternIO.h"
#include <limits>
#include <chrono>
namespace airack
{
namespace
{
// The processors open in this process and their sounds' names. Never held while taking a processor's lock.
struct OpenInstances { std::mutex mutex; std::vector<std::pair<const InstrumentProcessor*,juce::String>> names; };
OpenInstances& openInstances() { static OpenInstances instances; return instances; }
void noteSoundName(const InstrumentProcessor* owner,const juce::String& name)
{
    auto& open=openInstances();const std::lock_guard lock(open.mutex);
    const auto found=std::find_if(open.names.begin(),open.names.end(),[&](const auto& e){return e.first==owner;});
    if(found!=open.names.end())found->second=name;else open.names.emplace_back(owner,name);
}
juce::File defaultSoundFolder()
{
#if defined(_DEBUG)
    // Keeps automated native tests and their subprocesses out of real user data.
    const auto test=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
    if(test.isNotEmpty()&&juce::File::isAbsolutePath(test))return juce::File(test).getChildFile("Sounds");
#endif
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("AI Rack Synth").getChildFile("Sounds");
}
bool gestureFieldLocked(const SoundDocument& sound,std::size_t index)
{
    if(sound.controlLocks[index])return true;
    const auto& id=parameterRegistry()[index].id;
    if(id.startsWith("effect.")&&id[8]=='.'&&id[7]>='1'&&id[7]<='8')return sound.effectLocks[std::size_t(id[7]-'1')];
    if(id.startsWith("synth.")&&id[7]=='.'&&id[6]>='1'&&id[6]<='4')return sound.synthLocks[std::size_t(id[6]-'1')];
    return false;
}
bool drumGestureField(const SoundDocument& sound,std::size_t index)
{
    if(index>=drumParameterStart&&index<drumParameterStart+4*drumParametersPerSlot)
        return sourceModule(sound,static_cast<std::uint8_t>((index-drumParameterStart)/drumParametersPerSlot))==NativeModule::drum;
    for(std::uint8_t bank=0;bank<4;++bank)if(sourceModule(sound,bank)==NativeModule::drum)
        for(const auto* key: {"enabled","level","pan"})if(rackParameterIndex(NativeModule::drum,bank,key)==index)return true;
    return false;
}
std::size_t personalityBytes(const SoundDocument& d)
{
    if(!d.personalities)return 0;std::size_t bytes=sizeof(RackPersonalityState);
    for(const auto& i:d.personalities->instances){bytes+=sizeof(i)+i.bindings.size()*sizeof(UnitBinding)+i.design.name.getNumBytesAsUTF8()+i.design.intent.getNumBytesAsUTF8();
        for(const auto& u:i.design.units){bytes+=sizeof(u)+u.defaults.size()*sizeof(LocalParameter);for(const auto& p:u.defaults)bytes+=p.key.getNumBytesAsUTF8();}
        for(const auto& s:i.design.panel.sections)bytes+=sizeof(s)+s.getNumBytesAsUTF8();
        for(const auto& c:i.design.panel.controls)bytes+=sizeof(c)+c.label.getNumBytesAsUTF8()+c.key.getNumBytesAsUTF8();}
    return bytes;
}
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::uint64_t observedMidiNow() noexcept
{
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return ms<0?0:static_cast<std::uint64_t>(ms);
}

#ifndef NDEBUG
thread_local void (*structuralTestHook)(int)=nullptr;
#endif
void structuralTestPoint(int point) noexcept
{
#ifndef NDEBUG
    if(structuralTestHook)structuralTestHook(point);
#else
    (void)point;
#endif
}
constexpr std::uint64_t sourceSelectorMask=0xffu,sourceEpochStep=0x100u;
std::uint64_t sourceSelectorBit(std::size_t i) noexcept {
    if(i>=13&&i<=25&&(i-13)%4==0)return std::uint64_t(1)<<((i-13)/4);
    if(i>=drumModeParameterStart&&i<drumModeParameterStart+4)return std::uint64_t(1)<<(4+i-drumModeParameterStart);
    return 0;
}
std::uint64_t sourceWord(std::uint64_t old,std::uint64_t bits) noexcept {
    return (bits&sourceSelectorMask)|((old+sourceEpochStep)&~sourceSelectorMask);
}
class InstrumentParameter final : public juce::RangedAudioParameter
{
public:
    explicit InstrumentParameter(const ParameterDescriptor& d,std::atomic<std::uint64_t>* generation=nullptr,std::uint64_t selectorBit=0)
        : RangedAudioParameter(juce::ParameterID(d.id, 1), d.name,
                               juce::AudioProcessorParameterWithIDAttributes().withLabel(d.unit)),
          descriptor(d), range(d.minimum, d.maximum, d.kind == ParameterKind::continuous ? 0.0f : 1.0f, d.skew),
          value(range.convertTo0to1(d.defaultValue)), structuralGeneration(generation),selectorBit(selectorBit) {}
    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }
    float getValue() const override { return structuralGeneration?float((structuralGeneration->load()&selectorBit)!=0):value.load(std::memory_order_seq_cst); }
    void setValue(float v) override
    {
        if (!std::isfinite(v) || v < 0 || v > 1) return;
        if (isDiscrete())
        {
            const auto steps = static_cast<float>(descriptor.choices.size() - 1);
            v = std::round(v * steps) / steps;
        }
        if(structuralGeneration){
            auto old=structuralGeneration->load();
            for(;;){
                const auto bits=v==0?old&~selectorBit:old|selectorBit;
                if(bits==old)return;
                const auto next=sourceWord(old,bits);
                if(structuralGeneration->compare_exchange_weak(old,next)){structuralTestPoint(1);return;}
            }
        }
        value.store(v,std::memory_order_seq_cst);
    }
    float getDefaultValue() const override { return range.convertTo0to1(descriptor.defaultValue); }
    bool isDiscrete() const override { return descriptor.kind != ParameterKind::continuous; }
    bool isBoolean() const override { return descriptor.kind == ParameterKind::toggle; }
    bool isAutomatable() const override { return descriptor.automatable; }
    int getNumSteps() const override { return isDiscrete() ? static_cast<int>(descriptor.choices.size()) : getDefaultNumParameterSteps(); }
    juce::StringArray getAllValueStrings() const override
    { juce::StringArray result; for (const auto& choice : descriptor.choices) result.add(choice); return result; }
    juce::String getText(float v, int length) const override
    {
        if (!std::isfinite(v)) v = getDefaultValue();
        v = juce::jlimit(0.0f, 1.0f, v);
        const auto text = isDiscrete() ? descriptor.choices[static_cast<std::size_t>(std::round(v * (descriptor.choices.size() - 1))) ]
                                       : juce::String(range.convertFrom0to1(v), 6).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        return length > 0 ? text.substring(0, length) : text;
    }
    float getValueForText(const juce::String& text) const override
    {
        for (std::size_t i = 0; i < descriptor.choices.size(); ++i)
            if (text.trim().equalsIgnoreCase(descriptor.choices[i])) return static_cast<float>(i) / (descriptor.choices.size() - 1);
        const auto v = text.getFloatValue();
        if (!std::isfinite(v) || v < range.start || v > range.end) return getValue();
        return convertTo0to1(v);
    }
private:
    const ParameterDescriptor& descriptor;
    const juce::NormalisableRange<float> range;
    std::atomic<float> value;
    std::atomic<std::uint64_t>* structuralGeneration;
    const std::uint64_t selectorBit;
};
}
namespace
{
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint16_t>::is_always_lock_free);
static_assert(std::atomic<PatternPlaybackStatus>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);

// Relative -120 dB amplitude per path, budgeted across up to eight serial stages.
// This finite engineering estimate includes filter memory and conservative
// feedback recirculation; it is not an absolute-silence/gain guarantee. Missing
// sync timing uses the supported 20 BPM lower bound for reporting only.
double tailEstimate(const RoutedPatch& p, std::optional<double> bpm, double effectiveRate=48000,double preparedShiftTail=0,std::array<double,2> preparedSplitTail={},double preparedMidSideTail=0,std::array<double,4> sourceTails={}) noexcept
{
    constexpr double logThreshold = -16.0127351353; // log(1e-6 / 9)
    const auto filterTail = [](double hz) { return -logThreshold / (6.283185307179586 * hz); };
    const auto chainTail = [&](std::size_t chain)
    {
        double result = 0;
        for (std::size_t n=0;n<p.layout.effectCount[chain];++n)
        {
            const auto slot=p.layout.effectOrder[chain][n]; const auto& e=p.effects[slot];
            if(e.bypassed) continue;
            switch(p.layout.effects[slot].kind)
            {
                case EffectKind::saturation:
                    result+=saturationTailSeconds(e.saturation,effectiveRate);
                    break;
                case EffectKind::drive:
                    if(e.drive.mix>0) result+=filterTail(e.drive.toneHz)+.001;
                    if(e.algorithmRevision==modernDriveAlgorithmRevision) result+=detail::drive2OversamplingTailSeconds(effectiveRate);
                    break;
                case EffectKind::delay:
                    if(e.delay.mix>0)
                    {
                        // The period resolves as the engine's does: Beats or a note, held at 4 s;
                        // the right side reads at the period times RIGHT, held in 1 ms to 4 s.
                        const double tempo=bpm.value_or(20);
                        const double period=!e.delay.synced?e.delay.seconds
                            :std::min(4.,e.delay.note==0?60*e.delay.beats/tempo:60*detail::delayNoteBeats[static_cast<std::size_t>(e.delay.note)]/tempo);
                        const double right=std::clamp(period*e.delay.rightRatio,.001,4.);
                        const double repeats=e.delay.feedback>0?std::ceil(logThreshold/std::log(e.delay.feedback)):1;
                        // The tape keys count only away from neutral. Each pass adds the loop low
                        // cut's decay, the wobble's 1 ms and a sample for the saturation; the echo
                        // low cut's decay counts once. After TIME falls from up to 4 s, the glide's
                        // N passes end by N TIME + 0.5 ln(4 s / TIME), from the shorter side's time.
                        const double lowCut=e.delay.lowCutHz>20?filterTail(e.delay.lowCutHz/detail::delayLowCutCornerRatio):0;
                        const double wobble=e.delay.wobble>0?.001:0,saturation=e.delay.saturation>0?1/effectiveRate:0;
                        const double glide=e.delay.glide?.5*std::log(4/std::max(std::min(period,right),.001)):0;
                        const auto pass=[&](double time){return time+filterTail(e.delay.filterHz)+detail::delayReadSupportSeconds(effectiveRate,e.algorithmRevision)
                                                                +lowCut+wobble+saturation;};
                        // Ping-pong decays by REPEATS once per round trip, a left hop then a right
                        // one, and ends one hop past the last.
                        result+=e.delay.pingPong?(repeats+1)*(pass(period)+pass(right))+pass(std::max(period,right))+lowCut+glide
                                                :(repeats+1)*pass(std::max(period,right))+lowCut+glide;
                    }
                    break;
                case EffectKind::reverb:
                    if(e.reverb.mix>0)
                    {
                        if(e.algorithmRevision==2) {
                            const double scale=.5+1.5*e.reverb.size;
                            const double shortest=detail::reverb2DelaySeconds.front()*scale-detail::reverb2ModulationSeconds;
                            const double longest=detail::reverb2DelaySeconds.back()*scale+detail::reverb2ModulationSeconds;
                            const double repeats=std::ceil(logThreshold/(-6.90775527898*shortest/e.reverb.decaySeconds));
                            const double cutoff=20000*std::pow(.025,e.reverb.damping);
                            double allpassSupport=0; for(double time:detail::reverb2AllpassSeconds[1]) allpassSupport+=time;
                            const double allpassRepeats=std::ceil(logThreshold/std::log(detail::reverb2AllpassFeedback));
                            result+=e.reverb.predelayMs*.001+allpassSupport*(.75+.5*e.reverb.size)*(allpassRepeats+1)
                                +(repeats+1)*(longest+filterTail(cutoff));
                            break;
                        }
                        const double scale=.5+1.5*e.reverb.size;
                        const double shortest=.0297*scale, longest=.0437*scale;
                        const double repeats=std::ceil(logThreshold/(-6.90775527898*shortest/e.reverb.decaySeconds));
                        const double cutoff=20000*std::pow(.025,e.reverb.damping);
                        result+=e.reverb.predelayMs*.001+(repeats+1)*(longest+filterTail(cutoff));
                    }
                    break;
                case EffectKind::spaceReverb: break; // Live admitted Space metadata is included by the caller.
                case EffectKind::eraReverb: if(e.eraReverb.mix>0)result+=eraReverbTailSeconds(e.eraReverb);break;
                case EffectKind::compressor: break; // Detector decay cannot generate audio from zero input.
                case EffectKind::dynamicEq: result+=dynamicEqTailSeconds(e.dynamicEq,effectiveRate);break;
                case EffectKind::eq: result+=detail::eqTailSeconds(e.eq,effectiveRate,e.algorithmRevision);break;
                case EffectKind::filter: result+=detail::filterTailSeconds(e.filter,effectiveRate,e.algorithmRevision);break;
                case EffectKind::chorus: result+=detail::chorusTailSeconds(e.chorus,effectiveRate,e.algorithmRevision);break;
                case EffectKind::phaser: result+=detail::phaserTailSeconds(e.phaser,effectiveRate);break;
                case EffectKind::flanger: result+=detail::flangerTailSeconds(e.flanger,effectiveRate,e.algorithmRevision);break;
                case EffectKind::midSideSplit: result+=preparedMidSideTail;break;
                case EffectKind::frequencySplit: result+=preparedSplitTail[e.algorithmRevision==modernFrequencySplitAlgorithmRevision];break;
                case EffectKind::convolution: result+=detail::convolutionTailSeconds(e.convolution,effectiveRate);break;
                case EffectKind::frequencyShift: result+=preparedShiftTail;break;
                case EffectKind::dimension: result+=detail::dimensionTailSeconds(e.dimension,effectiveRate,e.algorithmRevision);break;
                case EffectKind::stereo: break; // Memoryless utility cannot generate audio from zero input.
            }
        }
        return result;
    };
    bool anySolo=false; for(std::size_t i=0;i<4;++i) anySolo |= p.layout.synthUse[i]==SlotUse::present && p.synth.layers[i].enabled && p.solo[i];
    double inserts=0;
    for(std::size_t i=0;i<4;++i)
        if(p.layout.synthUse[i]==SlotUse::present && p.synth.layers[i].enabled && !p.mute[i] && (!anySolo||p.solo[i]))
            inserts=std::max(inserts,std::max(p.drumSource[i]?0.:double(p.synth.layers[i].ampEnvelope.release),sourceTails[i])+chainTail(i));
    return inserts+chainTail(masterChain)+.01; // Includes the accepted structural/continuous ramps.
}
}
void InstrumentProcessor::AtomicMetadata::store(const SoundAudioState& d) noexcept
{
    std::size_t n=0;
    const auto put=[&](auto v){layout[n++].store(static_cast<std::uint8_t>(v),std::memory_order_seq_cst);};
    for(auto v:d.layout.synthUse)put(v); for(auto v:d.layout.synthOrder)put(v); put(d.layout.synthCount);
    for(const auto& e:d.layout.effects){put(e.use);put(e.kind);put(e.chain);}
    for(const auto& order:d.layout.effectOrder)for(auto v:order)put(v);
    for(auto v:d.layout.effectCount)put(v);
    for(std::size_t i=0;i<8;++i){keys[i].store(d.macros[i].targetKey);amounts[i].store(d.macros[i].amount);effects[i].store(d.effectLocks[i]);}
    for(std::size_t i=0;i<parameterCount;++i)controls[i].store(d.controlLocks[i]);
    for(std::size_t i=0;i<4;++i)synths[i].store(d.synthLocks[i]);
    for(std::size_t s=0;s<4;++s)for(std::size_t b=0;b<2;++b)synthRevisions[s][b].store(d.compatibility.synth[s][b]);
    for(std::size_t s=0;s<8;++s)for(std::size_t b=0;b<effectBankCount;++b)effectRevisions[s][b].store(d.compatibility.effects[s][b]);
}
void InstrumentProcessor::AtomicMetadata::load(SoundAudioState& d) const noexcept
{
    std::size_t n=0; const auto get=[&]{return layout[n++].load(std::memory_order_seq_cst);};
    for(auto& v:d.layout.synthUse)v=static_cast<SlotUse>(get()); for(auto& v:d.layout.synthOrder)v=get(); d.layout.synthCount=get();
    for(auto& e:d.layout.effects){e.use=static_cast<SlotUse>(get());e.kind=static_cast<EffectKind>(get());e.chain=get();}
    for(auto& order:d.layout.effectOrder)for(auto& v:order)v=get();
    for(auto& v:d.layout.effectCount)v=get();
    for(std::size_t i=0;i<8;++i){d.macros[i]={keys[i].load(),amounts[i].load()};d.effectLocks[i]=effects[i].load();}
    for(std::size_t i=0;i<parameterCount;++i)d.controlLocks[i]=controls[i].load();
    for(std::size_t i=0;i<4;++i)d.synthLocks[i]=synths[i].load();
    for(std::size_t s=0;s<4;++s)for(std::size_t b=0;b<2;++b)d.compatibility.synth[s][b]=synthRevisions[s][b].load();
    for(std::size_t s=0;s<8;++s)for(std::size_t b=0;b<effectBankCount;++b)d.compatibility.effects[s][b]=effectRevisions[s][b].load();
}
// UndoManager and its actions are owned by the control path, serialized with
// recall by restoreMutex. perform/undo never notify external listeners.
struct InstrumentProcessor::DocumentAction final : juce::UndoableAction
{
    InstrumentProcessor& owner;
    const ProjectDocument before, after;
    bool reset, skipFirst;
    DocumentAction(InstrumentProcessor& p,const ProjectDocument& a,const ProjectDocument& b,bool r,bool skip)
        :owner(p),before(a),after(b),reset(r),skipFirst(skip){}
    bool perform() override
    {
        if(skipFirst){skipFirst=false;return true;}
        return merge(after);
    }
    bool merge(const ProjectDocument& target)
    {
        auto current=target;current.group=owner.recoveryDocument->group;
        if(!validateProjectDocument(current))return false;
        return owner.publish(current,reset);
    }
    bool undo() override {return merge(before);}
    int getSizeInUnits() override {return static_cast<int>(sizeof(*this)+personalityBytes(before.sound)+personalityBytes(after.sound));}
};
struct InstrumentProcessor::AiDocumentAction final : juce::UndoableAction
{
    InstrumentProcessor& owner;
    const ProjectDocument before,after;
    const ChangedProjectFields fields;
    bool skipFirst;
    AiDocumentAction(InstrumentProcessor& p,const ProjectDocument& a,const ProjectDocument& b,bool skip=false)
        :owner(p),before(a),after(b),fields(changedProjectFields(a,b)),skipFirst(skip){}
    bool merge(const ProjectDocument& target)
    {
        ProjectDocument live;std::uint64_t edit;
        if(!owner.tryProjectSnapshotOwned(live,edit)||!owner.mergeChangedProjectFieldsOwned(target,fields,live)
            ||!owner.publish(live,false,true,false,&fields.values)){owner.aiHistoryRejected=true;return false;}
        return true;
    }
    bool perform() override {if(skipFirst){skipFirst=false;return true;}return merge(after);}
    bool undo() override {return merge(before);}
    int getSizeInUnits() override {return static_cast<int>(sizeof(*this)+personalityBytes(before.sound)+personalityBytes(after.sound)+fields.personalities.size()*sizeof(juce::Uuid));}
};
#ifndef NDEBUG
std::array<std::size_t,2> InstrumentProcessor::legacyHistorySizes() noexcept
{return {sizeof(DocumentAction),sizeof(AiDocumentAction)};}
#endif
std::size_t InstrumentProcessor::historyPersonalityBytes(const SoundDocument& sound) {return personalityBytes(sound);}
bool InstrumentProcessor::performHistoryAction(std::unique_ptr<juce::UndoableAction> action)
{
    if(!action)return false;
    const auto charge=action->getSizeInUnits();if(charge<=0)return false;
    auto& history=*editHistory;
    if(history.nextActionId==UINT64_MAX||!action->perform())return false;
    while(history.count>history.cursor){const auto index=--history.count;history.storedUnits-=history.units[index];history.actions[index].reset();history.units[index]=0;history.actionIds[index]=0;}
    const auto removeOldest=[&]{history.storedUnits-=history.units[0];history.actions[0].reset();
        for(std::size_t n=1;n<history.count;++n){history.actions[n-1]=std::move(history.actions[n]);history.units[n-1]=history.units[n];history.actionIds[n-1]=history.actionIds[n];}
        --history.count;history.units[history.count]=0;history.actionIds[history.count]=0;};
    if(history.count==64)removeOldest();
    history.actions[history.count]=std::move(action);history.units[history.count]=static_cast<std::uint32_t>(charge);history.actionIds[history.count]=history.nextActionId++;
    ++history.count;history.storedUnits+=static_cast<std::uint32_t>(charge);
    while(history.storedUnits>30000&&history.count>30)removeOldest();
    history.cursor=history.count;refreshDrumHistoryRootsOwned();drumOwnership->collect();return true;
}
bool InstrumentProcessor::undoHistoryAction()
{
    auto& history=*editHistory;if(history.cursor==0||!history.actions[history.cursor-1]->undo())return false;
    --history.cursor;refreshDrumHistoryRootsOwned();drumOwnership->collect();return true;
}
bool InstrumentProcessor::redoHistoryAction()
{
    auto& history=*editHistory;if(history.cursor==history.count||!history.actions[history.cursor]->perform())return false;
    ++history.cursor;refreshDrumHistoryRootsOwned();drumOwnership->collect();return true;
}
void InstrumentProcessor::clearHistoryOwned() noexcept
{
    auto& history=*editHistory;
    for(auto& action:history.actions)action.reset();history.units={};history.actionIds={};history.count=history.cursor=0;history.storedUnits=0;
    refreshDrumHistoryRootsOwned();drumOwnership->collect();
}
void InstrumentProcessor::prepareRackSkins(RackInventoryPtr snapshot,std::span<const RackInstance> racks) {
    libraryRuntime->client->prepareSkins(makerPreparation,std::move(snapshot),racks);
}
void InstrumentProcessor::requestMakerSkin(const RackInstance& rack) {
    libraryRuntime->client->requestSkin(makerPreparation,rack);
}
studio::MakerSkinPtr InstrumentProcessor::preparedMakerSkin(const RackInstance& rack) const {
    if(auto owner=libraryRuntime->client->preparedSkin(makerPreparation,rack))return owner;
    return rack.package?studio::MakerSkinPtr{}:studio::makerFaceFor(rack.design);
}
InstrumentProcessor::InstrumentProcessor() : InstrumentProcessor(defaultRackLibraryRuntime()) {}
InstrumentProcessor::InstrumentProcessor(std::shared_ptr<RackLibraryRuntime> runtime) : InstrumentProcessor(std::move(runtime),RackPreviewMode{}) {}
InstrumentProcessor::InstrumentProcessor(std::shared_ptr<RackLibraryRuntime> runtime,RackPreviewMode preview) : AudioProcessor(BusesProperties()
    .withInput("External detector",juce::AudioChannelSet::stereo(),false)
    .withOutput("Output",juce::AudioChannelSet::stereo(),true))
{
    // Only a real plug-in or standalone session is tracked; test processors are not.
    if(preview.workspace==juce::File{}&&(wrapperType==wrapperType_VST3||wrapperType==wrapperType_Standalone)){feedback::SessionTracker::instanceCreated();sessionTracked=true;}
    if(preview.workspace!=juce::File{})presetFolder=preview.workspace.getChildFile("Sounds");
    libraryRuntime=runtime&&runtime->store&&runtime->client?std::move(runtime):defaultRackLibraryRuntime();
    makerPreparation=libraryRuntime->client->createSkinPreparation([](const auto& rack,const auto* snapshot,const auto& cancel){return studio::prepareMakerSkin(rack,snapshot,cancel);});
    inventoryPreparation=libraryRuntime->client->createInventoryPreparation();
    placementPreparation=libraryRuntime->client->createPlacementPreparation();
    fxMonitoring=std::make_shared<FxMonitor>();
    engine.attachFxMonitor(fxMonitoring.get());
    performance=std::make_unique<PerformanceMidi>();
    observedMidi=std::make_unique<ObservedMidi>();
    const auto registry = parameterRegistry();
    for (std::size_t i = 0; i < parameterCount; ++i)
    {
        const auto bit=sourceSelectorBit(i);
        auto parameter = std::make_unique<InstrumentParameter>(registry[i],bit?&structuralGeneration:nullptr,bit);
        controls[i] = parameter.get(); addParameter(parameter.release());
    }
    recoveryDocument->sound=newInstanceSound();controlPersonalities=recoveryDocument->sound.personalities;
    // Both source selectors keep their default.
    for(std::size_t i=0;i<parameterCount;++i)if(!sourceSelectorBit(i))controls[i]->setValue(recoveryDocument->sound.values[i]);
    const SoundDocument& initial=recoveryDocument->sound; metadata.store(initial); patternMetadata.store(*recoveryDocument); RoutedPatch patch;
    if(makeRoutedPatch(initial,patch))tailSeconds.store(tailEstimate(patch,{}));
    groupControl=std::make_unique<GroupControl>();
    groupControl->committed=*recoveryDocument;groupControl->processed=groupLinkState.view();
    noteSoundName(this,{});
    for(auto& token:drumTargetTokens)token.store(1);
    drumControlPump=std::make_unique<DrumControlPump>(*this);
    drumImporter=std::make_unique<DrumImport>(*this);
    if(preview.workspace==juce::File{})startMcp();
}
InstrumentProcessor::InstrumentProcessor(const companion::ClientOptions& options) : InstrumentProcessor() { aiClientOptions=options; }
InstrumentProcessor::InstrumentProcessor(const companion::ClientOptions& options,std::shared_ptr<RackLibraryRuntime> runtime) : InstrumentProcessor(std::move(runtime)) {aiClientOptions=options;}
InstrumentProcessor::InstrumentProcessor(const companion::ClientOptions& options,std::shared_ptr<RackLibraryRuntime> runtime,RackPreviewMode preview) : InstrumentProcessor(std::move(runtime),std::move(preview)) {aiClientOptions=options;}
InstrumentProcessor::~InstrumentProcessor()
{
    if(sessionTracked)feedback::SessionTracker::instanceDestroyed();
    closeMcp();
    {const std::lock_guard lock(restoreMutex);retireAiLocalOwned();}
    drumImporter->closeAdmission();
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::beforeMessageCallbackStop);
#endif
    {
        // stopTimer/cancelPendingUpdate do not join callbacks selected by dispatch.
        // Hold no processor lock while excluding dispatch, and release it before worker joins.
        const juce::MessageManagerLock messageLock;
        stopTimer();cancelPendingUpdate();drumControlPump->stopTimer();
#ifndef NDEBUG
        drumTestPoint(DrumTestPoint::beforeCallbackStorageRelease);
#endif
        drumControlPump.reset();
    }
    libraryRuntime->client->prepareSkins(makerPreparation,{},{});
    makerPreparation.reset();
    libraryRuntime->client->cancelInventory(inventoryPreparation);inventoryPreparation.reset();
    libraryRuntime->client->cancelPlacement(placementPreparation);placementPreparation.reset();
    aiClient.cancelGeneration();aiClient.close();drumImporter.reset();
    {const std::lock_guard lock(restoreMutex);renderedPreview.release();releaseManualPreviewChargeOwned();releaseAiPreviewChargeOwned();releaseMcpPreviewChargeOwned();previewOwner=PreviewOwner::none;
        aiPayloads->result.reset();aiRefinementBackup.reset();drainAiLocalOwned(true);}
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::beforeOwnerClear);
#endif
    engine.clearDrumBorrowersQuiescent();drumOwnership->quiescent();
    closeAiSession();
    auto& open=openInstances();const std::lock_guard lock(open.mutex);std::erase_if(open.names,[this](const auto& e){return e.first==this;});
}
DrumImport& InstrumentProcessor::drumImport() noexcept {return *drumImporter;}
juce::RangedAudioParameter& InstrumentProcessor::control(std::size_t i) { return *controls.at(i); }
bool InstrumentProcessor::readAudioProject(AudioProjectState& output,std::uint64_t& serial,std::uint64_t& edit,std::uint64_t& reset,std::uint64_t& restart,std::uint64_t* modeReset,DrumAudioState* drums) const noexcept
{
    for(int attempt=0;attempt<2;++attempt){
#ifndef NDEBUG
        if(drums)drumTestPoint(DrumTestPoint::beforeDrumReadAttempt);
#endif
        const auto before=revision.load();if(before&1u)continue;const auto structural=structuralGeneration.load();
        // Every value is overwritten below; avoid computing catalog defaults.
        AudioProjectState candidate{.sound={.values={}}};
        for(std::size_t i=0;i<parameterCount;++i)candidate.sound.values[i]=controls[i]->getValue();
        metadata.load(candidate.sound);patternMetadata.load(candidate);
        DrumAudioState borrowed;
        if(drums){const auto* publication=drumOwnership->published();
#ifndef NDEBUG
            drumTestPoint(DrumTestPoint::afterBorrowedLoad);
#endif
            if(!publication)continue;
            borrowed.samples=publication->samples;borrowed.identity.generation=publication->generation;
            for(std::size_t n=0;n<64;++n)borrowed.identity.targets[n]=drumTargetTokens[n].load();
            borrowed.source=structural;borrowed.acknowledgedSource=drumAcknowledgedSource.load();
            if(structural!=borrowed.acknowledgedSource)borrowed.identity.targets={};
        }
        const auto e=editRevision.load(),r=resetGeneration.load(),p=patternGeneration.load(),m=modeGeneration.load();
#ifndef NDEBUG
        drumTestPoint(DrumTestPoint::beforeReadValidation);
#endif
        if(before==revision.load()&&structural==structuralGeneration.load()){if(drums)*drums=borrowed;output=candidate;serial=before;edit=e;reset=r;restart=p;if(modeReset)*modeReset=m;return true;}
    }return false;
}
// Lock-held control reader. Metadata ownership never enters readAudioProject.
bool InstrumentProcessor::readProject(ProjectDocument& output,std::uint64_t& serial,std::uint64_t& edit,std::uint64_t& reset,std::uint64_t& restart,std::uint64_t* modeReset,bool includeGroup) const noexcept
{
    try {
        auto& self=*const_cast<InstrumentProcessor*>(this);
        const auto generation=structuralGeneration.load();AudioProjectState numeric{.sound={.values={}}};std::uint64_t seq,e,r,p,m;
        if(!readAudioProject(numeric,seq,e,r,p,&m))return false;
        if(generation!=structuralGeneration.load())return false;
        ProjectDocument candidate{.sound={SoundAudioState{.values={}}}};static_cast<SoundAudioState&>(candidate.sound)=numeric.sound;candidate.sound.personalities=controlPersonalities;
        candidate.sound.name=recoveryDocument->sound.name;
        candidate.pattern=numeric.pattern;candidate.mode=numeric.mode;
        if(generation!=observedStructuralGeneration){
            SoundDocument reconciled{SoundAudioState{.values={}}};RackDifference difference;
            auto intent=candidate.sound;RoutedPatch patch;
            if(controlPersonalities&&!makeRoutedPatch(static_cast<const SoundAudioState&>(intent),patch)){
                // Invalid independent host coordinates remain readable/editable.
                // Reconcile only the observed engines against the last usable
                // intent; never publish repaired numbers over host atomics here.
                intent.values=recoveryDocument->sound.values;
                for(std::size_t i=0;i<parameterCount;++i)if(sourceSelectorBit(i))intent.values[i]=candidate.sound.values[i];
            }
            if(controlPersonalities&&reconcileRackEdit(recoveryDocument->sound,intent,{},SoundEditOrigin::manual,reconciled,difference)!=RackCandidateStatus::ready)return false;
            if(!controlPersonalities)reconciled=candidate.sound;
            if(generation!=structuralGeneration.load())return false;
            if(self.drumTargetEpoch==std::numeric_limits<std::uint64_t>::max())return false;
            self.revision.fetch_add(1);(void)self.invalidateDrumTargetsOwned(~std::uint64_t(0),generation);self.controlPersonalities=reconciled.personalities;self.recoveryDocument->sound=reconciled;
            self.observedStructuralGeneration=generation;self.editRevision.fetch_add(1);self.revision.fetch_add(1);
            reconciled.values=candidate.sound.values;
            candidate.sound=std::move(reconciled);seq=self.revision.load();e=self.editRevision.load();
        }
        if(includeGroup)groupMetadata.load(candidate);
        output=std::move(candidate);serial=seq;edit=e;reset=r;restart=p;if(modeReset)*modeReset=m;return true;
    }catch(...){return false;}
}
bool InstrumentProcessor::readDocument(SoundDocument& output,std::uint64_t& serial,std::uint64_t& edit,std::uint64_t& reset) const noexcept
{
    ProjectDocument d{.sound={SoundAudioState{.values={}}}};std::uint64_t restart;
    if(!readProject(d,serial,edit,reset,restart,nullptr,false))return false;
    output=d.sound;return true;
}bool InstrumentProcessor::trySoundSnapshotOwned(SoundDocument& d,std::uint64_t& edit) const noexcept
{std::uint64_t serial,reset;return readDocument(d,serial,edit,reset);}
bool InstrumentProcessor::trySoundSnapshotOwned(SoundDocument& d,std::uint64_t& edit,std::uint64_t& reset) const noexcept
{std::uint64_t serial;return readDocument(d,serial,edit,reset);}
bool InstrumentProcessor::tryParameterSnapshot(ParameterValues& values,std::uint64_t& serial) const noexcept
{
    AudioProjectState d{.sound={.values={}}};std::uint64_t edit,reset,restart;
    if(!readAudioProject(d,serial,edit,reset,restart))return false;
    values=d.sound.values;return true;
}
#ifndef NDEBUG
void InstrumentProcessor::setStructuralTestHook(void (*hook)(int)) noexcept {structuralTestHook=hook;}
void InstrumentProcessor::sourceCaptureTestPoint() noexcept {structuralTestPoint(4);}
#endif
struct InstrumentProcessor::PreparedControlCommit final
{
    ProjectDocument next;
    juce::String registryName,successPath;
    juce::File folder;
    AiMusicalContext context;
    std::unique_ptr<drums::DrumOwnership::PublicationRecord> publication;
    drums::TargetTokens tokens {};
    std::uint64_t tokenEpoch=0,committedSource=0,nextEdit=0;
    std::size_t publicationSlot=drums::publicationCapacity;
    bool rename=false,path=false,folderChanged=false,modeChanged=false,patternChanged=false,contextChanged=false;
    enum GroupUpdate {none,complete,groupOnly,hostRecall} group=none;
    explicit PreparedControlCommit(const ProjectDocument& document):next(document){}
};
bool InstrumentProcessor::publish(const ProjectDocument& document,bool reset,bool advanceEdit,bool restartPattern,
    const std::array<bool,parameterCount>* valueMask,bool recall) noexcept
{
    return publishDrumCommit(document,reset,advanceEdit,restartPattern,valueMask,recall,{},0,stagedAiPresetPath);
}
bool InstrumentProcessor::publishDrumCommit(const ProjectDocument& document,bool reset,bool advanceEdit,bool restartPattern,
    const std::array<bool,parameterCount>* valueMask,bool recall,
    std::unique_ptr<drums::DrumOwnership::PublicationRecord> publication,std::uint64_t invalidated,
    const juce::String* successPath) noexcept
{
    static_assert(std::is_nothrow_copy_assignable_v<ProjectDocument>);
    static_assert(std::is_nothrow_copy_assignable_v<std::shared_ptr<const RackPersonalityState>>);
    static_assert(std::is_nothrow_move_assignable_v<AiMusicalContext>);
    static_assert(std::is_nothrow_move_assignable_v<juce::File>);
    static_assert(std::is_nothrow_copy_assignable_v<std::optional<GroupIntent>>);
    static_assert(std::is_nothrow_destructible_v<ProjectDocument>);
    static_assert(noexcept(std::declval<juce::String&>().swapWith(std::declval<juce::String&>())));
    std::unique_ptr<PreparedControlCommit> prepared;
    std::unique_lock<std::mutex> nameLock;
    juce::String* registeredName=nullptr;
    auto consumed=recall?structuralGeneration.load():observedStructuralGeneration;
    try {
        drainDrumFeedbackOwned();
        prepared=std::make_unique<PreparedControlCommit>(document);auto& stage=*prepared;
        if(!validateProjectDocument(document)){drumCommitStatus=DrumCommitError::invalid;return false;}
        stage.publication=std::move(publication);
        if(stage.publication){stage.publicationSlot=drumOwnership->freePublicationSlot();
            if(stage.publicationSlot==drums::publicationCapacity){drumCommitStatus=DrumCommitError::publicationFull;return false;}}
        auto bits=consumed&sourceSelectorMask;
        for(std::size_t n=0;n<parameterCount;++n)if(const auto bit=sourceSelectorBit(n);bit&&(!valueMask||(*valueMask)[n]))
            bits=parameterPhysicalValue(document.sound.values,n)==0?bits&~bit:bits|bit;
        stage.committedSource=sourceWord(consumed,bits);
        for(unsigned bank=0;bank<4;++bank)if(drumBankOwner(recoveryDocument->sound,static_cast<std::uint8_t>(bank))!=drumBankOwner(document.sound,static_cast<std::uint8_t>(bank))
            ||sourceModule(recoveryDocument->sound,static_cast<std::uint8_t>(bank))!=sourceModule(document.sound,static_cast<std::uint8_t>(bank)))invalidated|=std::uint64_t(0xffff)<<(16*bank);
        if(recall)invalidated=~std::uint64_t(0);
        if(invalidated&&drumTargetEpoch==std::numeric_limits<std::uint64_t>::max()){drumCommitStatus=DrumCommitError::authorityExhausted;return false;}
        stage.tokenEpoch=drumTargetEpoch+(invalidated?1:0);
        for(std::size_t n=0;n<64;++n)stage.tokens[n]=invalidated&(std::uint64_t(1)<<n)?stage.tokenEpoch:drumTargetTokens[n].load();
        stage.modeChanged=document.mode!=recoveryDocument->mode;stage.patternChanged=document.pattern!=recoveryDocument->pattern;
        stage.nextEdit=editRevision.load()+(advanceEdit?1:0);
        stage.rename=document.sound.name!=recoveryDocument->sound.name;
        if(stage.rename){stage.registryName=document.sound.name;auto& names=openInstances();nameLock=std::unique_lock(names.mutex);
            const auto entry=std::find_if(names.names.begin(),names.names.end(),[&](const auto& value){return value.first==this;});
            if(entry==names.names.end()){drumCommitStatus=DrumCommitError::invalid;return false;}registeredName=&entry->second;}
        if(successPath){stage.successPath=*successPath;stage.path=true;}
        if(stagedPresetFolder){stage.folder=*stagedPresetFolder;stage.folderChanged=true;}
        if(groupControl){
            if(recall)stage.group=PreparedControlCommit::hostRecall;
            else if(!groupBusyOwned()){
                if(groupControl->committed!=document||groupControl->committedEdit!=stage.nextEdit)stage.group=PreparedControlCommit::complete;
            }else if(groupControl->committed.group!=document.group)stage.group=PreparedControlCommit::groupOnly;
        }
        if(recall&&(groupLinkState.view().intent||document.group)){
            stage.contextChanged=true;
            if(document.group){stage.context.role=groupTextView(document.group->role);stage.context.key=groupTextView(document.group->key);
                stage.context.scale=groupTextView(document.group->scale);stage.context.auditionBpm=document.group->bpm;}
        }
    }catch(const std::bad_alloc&){drumCommitStatus=DrumCommitError::allocationFailed;return false;}
    auto& stage=*prepared;
    structuralTestPoint(3);
    revision.fetch_add(1);
    if(!structuralGeneration.compare_exchange_strong(consumed,stage.committedSource)){
        revision.fetch_add(1);drumCommitStatus=DrumCommitError::staleTarget;return false;}
    if(mcpState)mcpState->stage.reset();
    for(std::uint8_t slot=0;slot<8;++slot)fxMonitoring->cancelSaturationAudition(slot);
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::afterSourceCas);
#endif
    if(stage.rename)registeredName->swapWith(stage.registryName);
    *recoveryDocument=stage.next;controlPersonalities=stage.next.sound.personalities;
    for(std::size_t n=0;n<parameterCount;++n)if(!sourceSelectorBit(n)&&(!valueMask||(*valueMask)[n]))controls[n]->setValue(stage.next.sound.values[n]);
    structuralTestPoint(2);
    metadata.store(stage.next.sound);patternMetadata.store(stage.next);groupMetadata.store(stage.next.group);
    if(stage.publication)drumOwnership->install(stage.publicationSlot,std::move(stage.publication));
    for(std::size_t n=0;n<64;++n)drumTargetTokens[n].store(stage.tokens[n]);
    drumTargetEpoch=stage.tokenEpoch;drumAcknowledgedSource.store(stage.committedSource);observedStructuralGeneration=stage.committedSource;
    if(advanceEdit)editRevision.fetch_add(1);
    if(reset)resetGeneration.fetch_add(1);
    if(restartPattern||stage.patternChanged)patternGeneration.fetch_add(1);
    if(stage.modeChanged)modeGeneration.fetch_add(1);
    if(reset||stage.modeChanged)releaseKeyboardNotes();
    if(stage.path)presetPath.swapWith(stage.successPath);
    if(stage.folderChanged)presetFolder=std::move(stage.folder);
    if(stage.contextChanged){aiContext=std::move(stage.context);invalidateAiContextOwned();}
    if(stage.group==PreparedControlCommit::hostRecall){groupLinkState.restore(stage.next.group);
        groupControl->committed=stage.next;groupControl->committedEdit=stage.nextEdit;groupControl->dirty=true;
        groupControl->explicitConnection=false;groupControl->selection={};
        if(gesture->index!=parameterCount)gesture->cancelled=true;loadFailed.store(false,std::memory_order_relaxed);
    }else if(stage.group==PreparedControlCommit::complete){groupControl->committed=stage.next;groupControl->committedEdit=stage.nextEdit;groupControl->dirty=true;}
    else if(stage.group==PreparedControlCommit::groupOnly){groupControl->committed.group=stage.next.group;groupControl->dirty=true;}
    revision.fetch_add(1);drumCommitStatus=DrumCommitError::none;return true;
}
bool InstrumentProcessor::publish(const SoundDocument& d,bool reset,bool advanceEdit) noexcept
{auto project=*recoveryDocument;project.sound=d;return publish(project,reset,advanceEdit);}
bool InstrumentProcessor::performProjectEdit(const ProjectDocument& before,const ProjectDocument& after,bool reset,bool alreadyPublished)
{
    if(!validateProjectDocument(after))return false;
    const bool validBefore=validateProjectDocument(before);
    const auto base=validBefore?before:*recoveryDocument;
    if(!validateProjectDocument(base))return false;
    if(base==after){if(!validBefore&&!alreadyPublished)return publish(after,reset);*recoveryDocument=after;return false;}
    return performHistoryAction(std::make_unique<DocumentAction>(*this,base,after,reset,alreadyPublished));
}
bool InstrumentProcessor::performAiEdit(const ProjectDocument& before,const ProjectDocument& after,bool alreadyPublished)
{
    const auto& candidate=after;
    // Admission already reconciled the complete candidate and explicit choices.
    if(!validateProjectDocument(before)||!validateProjectDocument(candidate))return false;
    if(before==candidate)return true;
    auto action=std::make_unique<AiDocumentAction>(*this,before,candidate,alreadyPublished);
    return performHistoryAction(std::move(action));
}
bool InstrumentProcessor::performEdit(const SoundDocument& before,const SoundDocument& after,bool reset,bool alreadyPublished)
{
    auto a=*recoveryDocument,b=*recoveryDocument;a.sound=before;b.sound=after;
    return performProjectEdit(a,b,reset,alreadyPublished);
}
bool InstrumentProcessor::performSoundNameEdit(const SoundDocument& current,const juce::String& name)
{
    auto before=std::make_unique<ProjectDocument>(*recoveryDocument);before->sound=current;
    auto after=std::make_unique<ProjectDocument>(*before);after->sound.name=name;
    if(!validateProjectDocument(*before)||!validateProjectDocument(*after))return false;
    return performHistoryAction(std::make_unique<DocumentAction>(*this,*before,*after,false,false));
}
bool InstrumentProcessor::applySoundEdit(const SoundDocument& candidate,SoundEditOrigin origin,std::uint64_t expected)
{
    {
        const std::lock_guard lock(restoreMutex); SoundDocument current;std::uint64_t edit;
        if(gesture->index!=parameterCount||!trySoundSnapshotOwned(current,edit)||edit!=expected
            )return false;
        SoundDocument reconciled;RackDifference difference;
        if(reconcileRackEdit(current,candidate,{},origin,reconciled,difference)!=RackCandidateStatus::ready)return false;
        DrumCommitError drumError;
        if(!validateFreshSourceOwnerAdmissionOwned(current,reconciled,nullptr,drumError)||!performEdit(current,reconciled))return false;
    }
    triggerAsyncUpdate();return true;
}
RackEditResult InstrumentProcessor::applyRackEdit(const RackEdit& operation,std::uint64_t expected)
{
    {
        const std::lock_guard lock(restoreMutex);SoundDocument before;std::uint64_t edit;
        if(gesture->index!=parameterCount||!trySoundSnapshotOwned(before,edit)||edit!=expected)return RackEditResult::invalid;
        if(operation.kind==RackEditKind::removeSynth&&before.personalities)
            for(const auto& i:before.personalities->instances)if(i.design.role==RackRole::fx)
                for(const auto& b:i.bindings)if(before.layout.effects[b.slot].chain==operation.slot)return RackEditResult::invalid;
        auto after=validateSoundDocument(before)?before:recoveryDocument->sound;const auto result=editRackLayout(after.layout,operation,after.layout);
        if(result!=RackEditResult::applied)return result;
        SoundDocument reconciled;RackDifference difference;
        if(reconcileRackEdit(before,after,{},SoundEditOrigin::manual,reconciled,difference)!=RackCandidateStatus::ready)return RackEditResult::invalid;
        if(operation.kind==RackEditKind::addSynth||operation.kind==RackEditKind::removeSynth||operation.kind==RackEditKind::moveSynth){
            auto trace=std::make_unique<SourceAssignmentTrace>();
            const auto bank=operation.kind==RackEditKind::addSynth?reconciled.layout.synthOrder[operation.position]:operation.slot;
            if(bank>=4)return RackEditResult::invalid;
            const auto previous=drumBankOwner(before,bank),next=drumBankOwner(reconciled,bank);
            auto& effect=trace->effects[trace->count++];effect.bank=bank;
            effect.beforeUse=previous.use;effect.afterUse=next.use;effect.beforeOwner=previous.rack;effect.afterOwner=next.rack;
            effect.beforeUnit=previous.unit;effect.afterUnit=next.unit;effect.invalidateTarget=operation.kind!=RackEditKind::moveSynth;
            auto a=std::make_unique<ProjectDocument>(*recoveryDocument),b=std::make_unique<ProjectDocument>(*recoveryDocument);a->sound=before;b->sound=reconciled;
            if(!performOwnedRackEdit(*a,*b,*trace,drumCommitStatus))return RackEditResult::invalid;
        }else if(!performEdit(before,reconciled))return RackEditResult::invalid;
    }
    triggerAsyncUpdate();return RackEditResult::applied;
}
bool InstrumentProcessor::beginParameterGesture(std::size_t index)
{
    const detail::ParameterListenerDispatch dispatch(*this);
    {
        const std::lock_guard lock(restoreMutex);std::uint64_t edit;
        if(index>=parameterCount||aiGestureReserved[index]||gesture->index!=parameterCount||gesture->notifying
            ||!trySoundSnapshotOwned(gesture->before,edit))return false;
        if(validateSoundDocument(gesture->before))recoveryDocument->sound=gesture->before;
        else gesture->before=recoveryDocument->sound;
        if(((index>=dynamicEqDetectorParameterStart&&index<dynamicEqSpectralParameterStart)||drumGestureField(gesture->before,index))&&gestureFieldLocked(gesture->before,index))return false;
        gesture->lastValid=gesture->before;
        gesture->index=index;gesture->changed=false;gesture->cancelled=false;gesture->notifying=true;gesture->grouped=false;
        gesture->notified[0]=index;gesture->notifiedCount=1;
    }
    controls[index]->beginChangeGesture();
    {const std::lock_guard lock(restoreMutex);gesture->notifying=false;}
    return true;
}
bool InstrumentProcessor::normalizeGestureValues(std::span<const ParameterGestureValue> edits,std::span<ParameterGestureValue> values) const
{
    if(edits.empty()||edits.size()>values.size())return false;
    for(std::size_t n=0;n<edits.size();++n){
        auto edit=edits[n];
        if(edit.index>=parameterCount||!std::isfinite(edit.value)||edit.value<0||edit.value>1)return false;
        for(std::size_t prior=0;prior<n;++prior)if(values[prior].index==edit.index)return false;
        const auto& control=*controls[edit.index];
        if(control.isDiscrete()){const auto steps=static_cast<float>(control.getNumSteps()-1);edit.value=std::round(edit.value*steps)/steps;}
        values[n]=edit;
    }
    return true;
}
bool InstrumentProcessor::beginParameterGesture(std::size_t index,std::span<const ParameterGestureValue> edits,const SoundDocument& expected,std::uint64_t expectedRevision,std::uint64_t expectedReset)
{
    const detail::ParameterListenerDispatch dispatch(*this);
    std::array<ParameterGestureValue,guardedGestureCapacity> values{};
    if(index>=parameterCount||!normalizeGestureValues(edits,values)
        ||std::none_of(edits.begin(),edits.end(),[&](const auto& edit){return edit.index==index;}))return false;
    {
        const std::lock_guard lock(restoreMutex);SoundDocument live;std::uint64_t edit,reset;
        if(gesture->index!=parameterCount||gesture->notifying||!trySoundSnapshotOwned(live,edit,reset)
            ||edit!=expectedRevision||reset!=expectedReset||live!=expected||!validateSoundDocument(live))return false;
        auto candidate=live;
        for(std::size_t n=0;n<edits.size();++n){const auto& value=values[n];
            if(aiGestureReserved[value.index]||gestureFieldLocked(live,value.index))return false;
            candidate.values[value.index]=value.value;
        }
        SoundDocument reconciled;RackDifference difference;
        if(reconcileRackEdit(live,candidate,{},SoundEditOrigin::manual,reconciled,difference)!=RackCandidateStatus::ready)return false;
        recoveryDocument->sound=live;gesture->before=gesture->lastValid=std::move(live);
        gesture->index=index;gesture->changed=false;gesture->cancelled=false;gesture->notifying=true;gesture->grouped=true;
        gesture->notified[0]=index;gesture->notifiedCount=1;
        for(std::size_t n=0;n<edits.size();++n)if(values[n].index!=index)gesture->notified[gesture->notifiedCount++]=values[n].index;
    }
    // The full group owns its fields before the first external begin callback.
    // A reentrant recall cancels writes; the caller still balances every begin.
    for(std::size_t n=0;n<gesture->notifiedCount;++n)controls[gesture->notified[n]]->beginChangeGesture();
    {const std::lock_guard lock(restoreMutex);gesture->notifying=false;}
    return true;
}
bool InstrumentProcessor::setParameterGestureValue(std::size_t index,float value)
{
    const ParameterGestureValue edit {index,value};
    return setParameterGestureValues(index,std::span<const ParameterGestureValue>(&edit,1));
}
bool InstrumentProcessor::setParameterGestureValues(std::size_t index,std::span<const ParameterGestureValue> edits)
{return setParameterGestureValuesImpl(index,edits,nullptr,0,0);}
bool InstrumentProcessor::setParameterGestureValues(std::size_t index,std::span<const ParameterGestureValue> edits,const SoundDocument& expected,std::uint64_t expectedRevision,std::uint64_t expectedReset)
{return setParameterGestureValuesImpl(index,edits,&expected,expectedRevision,expectedReset);}
bool InstrumentProcessor::setParameterGestureValuesImpl(std::size_t index,std::span<const ParameterGestureValue> edits,const SoundDocument* expected,std::uint64_t expectedRevision,std::uint64_t expectedReset)
{
    const detail::ParameterListenerDispatch dispatch(*this);
    if(edits.size()>(expected?guardedGestureCapacity:5u))return false;
    std::array<ParameterGestureValue,guardedGestureCapacity> values {};
    if(!normalizeGestureValues(edits,values))return false;
    std::array<std::size_t,guardedGestureCapacity> additions {};std::size_t added=0;
    const auto matches=[&](const SoundDocument& live,std::uint64_t edit,std::uint64_t reset){
        return !expected||(edit==expectedRevision&&reset==expectedReset&&live==*expected);
    };
    // Re-read live host fields after begin callbacks. A coupled edit must never
    // replay an earlier full snapshot over unrelated automation or host recall.
    auto candidate=[&](SoundDocument& d){
        const auto before=d;
        for(std::size_t n=0;n<edits.size();++n)if((expected||(values[n].index>=dynamicEqDetectorParameterStart&&values[n].index<dynamicEqSpectralParameterStart)||drumGestureField(d,values[n].index))&&gestureFieldLocked(d,values[n].index))return false;
        for(std::size_t n=0;n<edits.size();++n)d.values[values[n].index]=values[n].value;
        SoundDocument reconciled;RackDifference difference;
        if(reconcileRackEdit(before,d,{},SoundEditOrigin::manual,reconciled,difference)==RackCandidateStatus::ready){d=std::move(reconciled);return true;}
        if(expected)return false;
        d=gesture->lastValid;
        for(std::size_t n=0;n<edits.size();++n)d.values[values[n].index]=values[n].value;
        if(reconcileRackEdit(gesture->lastValid,d,{},SoundEditOrigin::manual,reconciled,difference)!=RackCandidateStatus::ready)return false;
        d=std::move(reconciled);return true;
    };
    {
        const std::lock_guard lock(restoreMutex);SoundDocument live;std::uint64_t edit,reset;
        for(std::size_t n=0;n<edits.size();++n)if(aiGestureReserved[values[n].index])return false;
        if(index>=parameterCount||gesture->index!=index||gesture->notifying||gesture->cancelled||!trySoundSnapshotOwned(live,edit,reset)||!matches(live,edit,reset))return false;
        auto next=live;if(!candidate(next))return false;
        if(next==live)return true;
        for(std::size_t n=0;n<edits.size();++n)
        {
            const auto field=values[n].index;
            const bool known=std::find(gesture->notified.begin(),gesture->notified.begin()+static_cast<std::ptrdiff_t>(gesture->notifiedCount),field)!=gesture->notified.begin()+static_cast<std::ptrdiff_t>(gesture->notifiedCount);
            // Begin callbacks can change even an initially equal submitted field
            // before the final candidate is rebuilt, so announce every such field.
            if(!known)additions[added++]=field;
        }
        if(gesture->notifiedCount+added>(expected?gesture->notified.size():5u))return false;
        for(std::size_t n=0;n<added;++n)gesture->notified[gesture->notifiedCount++]=additions[n];
        gesture->notifying=true;
    }
    for(std::size_t n=0;n<added;++n)controls[additions[n]]->beginChangeGesture();
    {
        const std::lock_guard lock(restoreMutex);SoundDocument live;std::uint64_t edit,reset;
        gesture->notifying=false;
        if(gesture->cancelled||!trySoundSnapshotOwned(live,edit,reset)||!matches(live,edit,reset))return false;
        auto next=live;if(!candidate(next))return false;
        if(next==live)return true;
        std::array<bool,parameterCount> mask{};for(std::size_t n=0;n<parameterCount;++n)mask[n]=next.values[n]!=live.values[n];
        auto project=*recoveryDocument;project.sound=next;if(!publish(project,false,!gesture->changed,false,&mask))return false;gesture->lastValid=next;gesture->changed=true;gesture->notifying=true;
    }
    for(std::size_t n=0;n<edits.size();++n)controls[values[n].index]->sendValueChangedMessageToListeners(values[n].value);
    {const std::lock_guard lock(restoreMutex);gesture->notifying=false;}
    triggerAsyncUpdate();return true;
}
bool InstrumentProcessor::endParameterGesture(std::size_t index)
{
    const detail::ParameterListenerDispatch dispatch(*this);
    bool accepted=true;
    {
        const std::lock_guard lock(restoreMutex);
        if(index>=parameterCount||gesture->index!=index||gesture->notifying)return false;
        SoundDocument after;std::uint64_t edit;
        if(!gesture->cancelled&&gesture->changed&&trySoundSnapshotOwned(after,edit))
        {
            if(!validateSoundDocument(after))
            {
                // End the accepted movement with a valid intent, not a poisoned
                // undo image. Its first movement already advanced editRevision.
                after=gesture->lastValid;accepted=publish(after,false,false);
            }
            if(accepted){
                if(gesture->grouped){
                    // Reuse selective history for this bounded group. Outside
                    // automation belongs to the live sound, not its undo image.
                    auto original=after;
                    for(std::size_t n=0;n<gesture->notifiedCount;++n){const auto field=gesture->notified[n];original.values[field]=gesture->before.values[field];}
                    SoundDocument reconciled;RackDifference difference;
                    accepted=reconcileRackEdit(after,original,{},SoundEditOrigin::manual,reconciled,difference)==RackCandidateStatus::ready;
                    if(accepted){auto beforeProject=*recoveryDocument,afterProject=*recoveryDocument;beforeProject.sound=std::move(reconciled);afterProject.sound=after;accepted=performAiEdit(beforeProject,afterProject,true);}
                }else performEdit(gesture->before,after,false,true);
                if(accepted)recoveryDocument->sound=after;
            }
        }
        gesture->notifying=true;
    }
    for(std::size_t n=0;n<gesture->notifiedCount;++n)controls[gesture->notified[n]]->endChangeGesture();
    {const std::lock_guard lock(restoreMutex);gesture->index=parameterCount;gesture->notifying=false;sealGroupCommitOwned();}
    triggerAsyncUpdate();return accepted;
}
bool InstrumentProcessor::undoSoundEdit()
{
    {const std::lock_guard lock(restoreMutex);aiHistoryRejected=false;if(gesture->index!=parameterCount||!undoHistoryAction()||aiHistoryRejected)return false;}
    triggerAsyncUpdate();return true;
}
bool InstrumentProcessor::redoSoundEdit()
{
    {const std::lock_guard lock(restoreMutex);aiHistoryRejected=false;if(gesture->index!=parameterCount||!redoHistoryAction()||aiHistoryRejected)return false;}
    triggerAsyncUpdate();return true;
}
void InstrumentProcessor::handleAsyncUpdate()
{
    const detail::ParameterListenerDispatch dispatch(*this);
    SoundDocument published{SoundAudioState{.values={}}};std::uint64_t serial,edit,reset;
    {const std::lock_guard lock(restoreMutex);if(!readDocument(published,serial,edit,reset)){triggerAsyncUpdate();return;}}
    for(std::size_t i=0;i<parameterCount;++i)controls[i]->sendValueChangedMessageToListeners(published.values[i]);
    SoundDocument current{SoundAudioState{.values={}}};std::uint64_t now,currentEdit,currentReset;
    {const std::lock_guard lock(restoreMutex);if(!readDocument(current,now,currentEdit,currentReset)||now!=serial||current!=published)triggerAsyncUpdate();}
    if(const auto view=libraryRuntime->client->inventory(inventoryPreparation);view&&view->revision){RackInventoryPtr ignored;captureAvailableRackInventory(ignored);}
}
juce::File InstrumentProcessor::soundFolderOwned() const { return presetFolder!=juce::File()?presetFolder:defaultSoundFolder(); }
juce::File InstrumentProcessor::soundFolder() const { const std::lock_guard lock(restoreMutex);return soundFolderOwned(); }
juce::String InstrumentProcessor::soundPresetPath() const { const std::lock_guard lock(restoreMutex);return presetPath; }
void InstrumentProcessor::setSoundPresetPath(const juce::String& path) { const std::lock_guard lock(restoreMutex);presetPath=path; }
juce::String InstrumentProcessor::soundName() const { const std::lock_guard lock(restoreMutex);return recoveryDocument->sound.name; }
std::vector<juce::String> InstrumentProcessor::soundNamesInUse() const
{
    std::vector<juce::String> names;
    const auto add=[&](const juce::String& name){
        if(name.isNotEmpty()&&name.getNumBytesAsUTF8()<=maximumSoundNameBytes&&names.size()<ai::maximumNamesInUse
           &&std::none_of(names.begin(),names.end(),[&](const auto& n){return n.equalsIgnoreCase(name);}))names.push_back(name);};
    {auto& open=openInstances();const std::lock_guard lock(open.mutex);for(const auto& [owner,name]:open.names)if(owner!=this)add(name);}
    for(const auto& entry:juce::RangedDirectoryIterator(soundFolder(),false,"*.airack",juce::File::findFiles))
    {if(names.size()>=ai::maximumNamesInUse)break;add(entry.getFile().getFileNameWithoutExtension());}
    return names;
}
bool InstrumentProcessor::captureRackDesign(juce::Uuid instanceId,RackDesign& output) const
{
    try {
        const std::lock_guard lock(restoreMutex);SoundDocument sound;std::uint64_t edit;
        if(!trySoundSnapshotOwned(sound,edit)||!validateSoundDocument(sound)||!sound.personalities)return false;
        const auto found=std::find_if(sound.personalities->instances.begin(),sound.personalities->instances.end(),[&](auto& i){return i.id==instanceId;});
        if(found==sound.personalities->instances.end()||found->design.revision==std::numeric_limits<std::uint64_t>::max())return false;
        for(std::size_t n=0;n<found->design.panel.controls.size();++n)if(!rackControlActive(sound,*found,n))return false;
        const auto& design=found->design;auto generic=sound;generic.personalities.reset();RackDesign captured;
        // Bindings may be stored in arbitrary order; recipe order is signal order.
        std::vector<UnitBinding> selected;
        for(const auto& unit:design.units){const auto b=std::find_if(found->bindings.begin(),found->bindings.end(),[&](auto& b){return b.unit==unit.id;});if(b==found->bindings.end())return false;selected.push_back(*b);}
        if(!captureGenericRackDesign(generic,design.role,selected,design.panel,design.id,design.name,design.intent,captured))return false;
        captured.revision=design.revision+1;output=std::move(captured);return true;
    }catch(...){return false;}
}
void InstrumentProcessor::retryAvailableRackInventory(){libraryRuntime->client->retryInventory(inventoryPreparation);}
bool InstrumentProcessor::captureAvailableRackInventory(RackInventoryPtr& output,RackLibraryStatus* status)
{
    try{
        std::shared_ptr<const RackPersonalityState> embedded;
        {const std::lock_guard lock(restoreMutex);embedded=controlPersonalities;}
        libraryRuntime->client->requestInventory(inventoryPreparation,embedded?std::span<const RackInstance>(embedded->instances):std::span<const RackInstance>{});
        const auto view=libraryRuntime->client->inventory(inventoryPreparation);
        if(status)*status=view?view->status:RackLibraryStatus::unavailable;
        if(!view||view->busy||view->status!=RackLibraryStatus::ready||!view->snapshot)return false;
        if(view->storeGeneration!=libraryRuntime->store->observedGeneration()){if(status)*status=RackLibraryStatus::busy;return false;}
        {const std::lock_guard lock(restoreMutex);if(controlPersonalities!=embedded){if(status)*status=RackLibraryStatus::busy;return false;}}
        prepareRackSkins(view->snapshot,embedded?std::span<const RackInstance>(embedded->instances):std::span<const RackInstance>{});
        output=view->snapshot;return true;
    }catch(const std::bad_alloc&){if(status)*status=RackLibraryStatus::byteLimit;return false;}
}
bool InstrumentProcessor::captureAvailableRackCatalog(std::shared_ptr<const RackCatalogSnapshot>& output,RackCatalogStatus* status)
{
    try {
        const std::lock_guard lock(restoreMutex);ProjectDocument source;std::uint64_t edit;
        if(!tryProjectSnapshotOwned(source,edit)){if(status)*status=RackCatalogStatus::unavailable;return false;}
        std::vector<RackDesign> embedded;
        if(source.sound.personalities)for(const auto& instance:source.sound.personalities->instances)embedded.push_back(instance.design);
        RackCatalogSnapshot snapshot;if(!captureRackCatalog(libraryRackDesigns(),embedded,{},snapshot,status))return false;
        auto captured=std::make_shared<const RackCatalogSnapshot>(std::move(snapshot));
        availableRackCatalog=captured;output=std::move(captured);return true;
    }catch(...){if(status)*status=RackCatalogStatus::allocationFailure;return false;}
}
RenderQuality InstrumentProcessor::selectedRenderQuality() const noexcept
{ return controls[758]->getValue()>=.5f ? RenderQuality::higher : RenderQuality::realtime; }
RoutedSynthEngine::SpaceRenderStats InstrumentProcessor::spaceRenderStats() const noexcept {return engine.spaceRenderStats();}
RenderQuality InstrumentProcessor::effectiveRenderQuality() const noexcept
{ return preparedQuality.load(std::memory_order_relaxed); }
bool InstrumentProcessor::renderQualityPending() const noexcept
{
    const auto effective=effectiveRenderQuality();
    return selectedRenderQuality()!=effective || (effective==RenderQuality::higher && !isNonRealtime());
}
DynamicEqProcessingMode InstrumentProcessor::effectiveEqMode(std::size_t slot) const noexcept
{ return slot<8?preparedEqModes[slot].load(std::memory_order_relaxed):DynamicEqProcessingMode::ordinary; }
bool InstrumentProcessor::eqPreparationPending(std::size_t slot) const noexcept
{
    if(slot>=8)return false;
    const auto requested=controls[dynamicEqSpectralParameterStart+slot*dynamicEqSpectralParametersPerSlot]->getValue()>=.5f?DynamicEqProcessingMode::spectral:DynamicEqProcessingMode::ordinary;
    return requested!=effectiveEqMode(slot)||pendingEqRouting.load(std::memory_order_relaxed);
}
void InstrumentProcessor::prepareToPlay(double rate,int size) try
{
    if(!std::isfinite(rate)||rate<8000||rate>384000||size<1||size>65536)return;
    const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();
    if(drumOwnership->lifecycle_==std::numeric_limits<std::uint64_t>::max())return;
    RoutedSynthEngine candidate;
    AudioProjectState d;DrumAudioState drumAudio;std::uint64_t serial,edit,reset,restart,modeReset;RoutedPatch patch;
    const bool read=readAudioProject(d,serial,edit,reset,restart,&modeReset);
    if(!(read?makeRoutedPatch(d.sound,patch):makeRoutedPatch(recoveryDocument->sound,patch)))return;
    if(!read)reset=resetGeneration.load();
    // Control owns this complete publication even after release left the raw
    // bridge null. Do not expose it until every required candidate stage succeeds.
    drumAudio.samples=drumOwnership->current_->borrowed.samples;
    drumAudio.identity.generation=drumOwnership->current_->borrowed.generation;
    for(std::size_t n=0;n<64;++n)drumAudio.identity.targets[n]=drumTargetTokens[n].load();
    drumAudio.acknowledgedSource=drumAcknowledgedSource.load();
    if(structuralGeneration.load()!=drumAudio.acknowledgedSource)drumAudio.identity.targets={};
    BorrowedDrumAssignments assignments;
    for(std::size_t bank=0;bank<4;++bank)for(std::size_t pad=0;pad<16;++pad)assignments[bank][pad]=drumAudio.samples[bank][pad];
    if(!candidate.setPatch(patch,assignments,drumAudio.identity,reset))return;
    const auto selected=read && d.sound.values[758]>=.5f ? RenderQuality::higher : read ? RenderQuality::realtime : selectedRenderQuality();
    const auto quality=isNonRealtime()?selected:RenderQuality::realtime;
    if(!candidate.prepare(rate,std::min(size,512),quality,eqPreparationModesFor(candidate.patch())))return;
    // MIDI capacity is the remaining fallible owning-path resource. Complete
    // it before publishing engine/mode/latency or setting prepared=true.
    performance->prepare();
    detail::FrequencyShiftPreparation shiftPreparation;
    if(!detail::prepareFrequencyShift(rate*(quality==RenderQuality::higher?2:1),shiftPreparation))return;
    const std::array<double,2> splitTail{detail::frequencySplitTailSeconds(rate*(quality==RenderQuality::higher?2:1)),
        detail::frequencySplitTailSeconds(rate*(quality==RenderQuality::higher?2:1),modernFrequencySplitAlgorithmRevision)};
    if(!std::isfinite(splitTail[0])||!std::isfinite(splitTail[1]))return;
    const auto midSideTail=detail::midSideSplitTailSeconds(rate*(quality==RenderQuality::higher?2:1));
    if(!std::isfinite(midSideTail))return;
    preparedFrequencyShiftTail=shiftPreparation.tailSeconds;preparedFrequencySplitTail=splitTail;preparedMidSideSplitTail=midSideTail;
    engine.clearDrumBorrowersQuiescent();drumOwnership->quiescent();
    engine=std::move(candidate);drumOwnership->borrowing_=true;renderDrumIdentity=drumAudio.identity;
    revision.fetch_add(1);drumOwnership->published_.store(&drumOwnership->current_->borrowed);revision.fetch_add(1);
    engine.attachFxMonitor(fxMonitoring.get());preparedQuality.store(quality,std::memory_order_relaxed);
    for(std::size_t slot=0;slot<8;++slot)preparedEqModes[slot].store(engine.effectiveEqMode(slot),std::memory_order_relaxed);
    pendingEqRouting.store(false,std::memory_order_relaxed);
    setLatencySamples(engine.latencySamples());
    // The prepared FIR has finite support twice its host-rate group delay.
    // Plugin tail reporting is uncompensated; previews trim latency separately.
    renderSoundCurrent=read;
    if(read){renderProject=d;renderResetGeneration=reset;renderPatternGeneration=restart;renderModeGeneration=modeReset;}
    tailSeconds.store(engine.spaceTailSeconds()+tailEstimate(engine.patch(),{},rate*(quality==RenderQuality::higher?2:1),preparedFrequencyShiftTail,preparedFrequencySplitTail,preparedMidSideSplitTail,engine.sourceTailSeconds())+2.0*engine.latencySamples()/rate);
    observedMidi->release();hostBpm.store(0,std::memory_order_relaxed);
    observedMidi->prepare(rate);
    mcpAudioValidUntil.store(0,std::memory_order_release);
    renderedPreview.prepare(rate);
    releaseManualPreviewChargeOwned();releaseAiPreviewChargeOwned();releaseMcpPreviewChargeOwned();previewOwner=PreviewOwner::none;
    prepared=true;preparedRate=rate;preparedBlock=std::min(size,512);wasPlaying=false;invalidPatternBlock=false;
    releaseKeyboardNotes();performance->scheduler=PatternScheduler{};
    performance->scheduler.prepare(rate,preparedBlock);performance->scheduler.setPattern(renderProject.pattern);
    missingTempo.store(false);hostMeter.store(0);playbackPosition.store(0);playbackFeedback.store(renderProject.mode==PlaybackMode::externalMidi?PatternPlaybackStatus::externalMidi:PatternPlaybackStatus::stopped);
}
catch(const std::bad_alloc&) { /* Preserve the last prepared engine on allocation failure. */ }
void InstrumentProcessor::releaseResources()
{
    const std::lock_guard lock(restoreMutex);
    performance->beginDrumCallback();if(!performance->drumPresses().empty())performance->reportDrumStatus(DrumPerformanceStatus::staleTarget);performance->consumeDrumCallback();
    engine.clearDrumBorrowersQuiescent();
    revision.fetch_add(1);drumOwnership->published_.store(nullptr);revision.fetch_add(1);
    refreshDrumHistoryRootsOwned();drumOwnership->quiescent();renderDrumIdentity={};
    observedMidi->release();
    mcpAudioValidUntil.store(0,std::memory_order_release);
    renderedPreview.release();releaseManualPreviewChargeOwned();releaseAiPreviewChargeOwned();releaseMcpPreviewChargeOwned();previewOwner=PreviewOwner::none;hostBpm.store(0,std::memory_order_relaxed);
    // Stopping audio cancels audition, while a joined current parent can still Apply later.
    const bool keepLocalProposal=aiLocal&&aiProposalPhase()==AiProposalPhase::proposed&&aiLocalCurrentOwned()
        &&aiLocal->hasParent&&!aiLocal->rendering&&!aiLocal->joining&&aiLocal->receiptSent;
    if(aiLocal&&!keepLocalProposal){retireAiLocalOwned();aiPayloads->request.reset();aiPayloads->result.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiPhase=AiProposalPhase::stale;drainAiLocalOwned();}
    engine.reset();(void)fxMonitoring->publishSampleRate(0);engine.deactivateSpace();prepared=false;renderSoundCurrent=false;wasPlaying=false;missingTempo.store(false);releaseKeyboardNotes();performance->clearOwners();performance->cancelDeferredReset();
    performance->scheduler.release(performance->patternEvents);hostMeter.store(0);playbackPosition.store(0);playbackFeedback.store(PatternPlaybackStatus::unprepared);
}
bool InstrumentProcessor::tempoUnavailable() const noexcept{return missingTempo.load();}
std::shared_ptr<FxMonitor> InstrumentProcessor::fxMonitorHandle() const noexcept{return fxMonitoring;}
bool InstrumentProcessor::saturationAuditionContext(std::uint8_t slot,const juce::Uuid& rack,std::uint64_t expectedRevision,std::uint64_t expectedReset) const noexcept
{
    const auto* messages=juce::MessageManager::getInstanceWithoutCreating();
    if(!messages||!messages->isThisTheMessageThread()||slot>=8||rack.isNull()||fxMonitoring->sampleRate()<=0)return false;
    SoundDocument sound;std::uint64_t currentRevision,currentReset;ParameterValues effective;
    if(!trySoundSnapshot(sound,currentRevision,currentReset)||currentRevision!=expectedRevision||currentReset!=expectedReset
        ||sound.layout.effects[slot].use!=SlotUse::present||sound.layout.effects[slot].kind!=EffectKind::saturation
        ||!sound.personalities||!makeEffectiveParameterValues(sound,effective)
        ||parameterPhysicalValue(effective,saturationParameterStart+slot*saturationParametersPerSlot)!=1
        ||parameterPhysicalValue(effective,572+slot*23)!=0)return false;
    for(const auto& instance:sound.personalities->instances)if(instance.id==rack)
        for(const auto& unit:instance.design.units)if(unit.module==NativeModule::saturation)
            if(const auto* binding=rackUnitBinding(instance,unit.id);binding&&binding->slot==slot)return true;
    return false;
}
std::optional<FxMonitor::SaturationToken> InstrumentProcessor::acquireSaturationAudition(std::uint8_t slot,const juce::Uuid& rack,std::uint64_t revision,std::uint64_t reset) noexcept
{
    if(!saturationAuditionContext(slot,rack,revision,reset))return {};
    return fxMonitoring->acquireSaturationAudition(slot);
}
bool InstrumentProcessor::requestSaturationAudition(FxMonitor::SaturationToken token,std::optional<SaturationLane> lane,const juce::Uuid& rack,std::uint64_t revision,std::uint64_t reset) noexcept
{
    if(!saturationAuditionContext(token.slot,rack,revision,reset)){
        (void)fxMonitoring->retireSaturationAudition(token);return false;
    }
    return fxMonitoring->requestSaturationAudition(token,lane);
}
void InstrumentProcessor::retireSaturationAudition(FxMonitor::SaturationToken token) noexcept
{(void)fxMonitoring->retireSaturationAudition(token);}
void InstrumentProcessor::captureObservedMidi(ObservedMidiSnapshot& out) noexcept { observedMidi->capture(observedMidiNow(),out); }
void InstrumentProcessor::processBlock(juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi)
{
    const auto observedNow=observedMidiNow();
    if(!prepared){observedMidi->beginBlock({}, {},0,renderProject.mode,observedNow);observedMidi->endBlock();audio.clear();hostBpm.store(0,std::memory_order_relaxed);playbackFeedback.store(PatternPlaybackStatus::unprepared);return;}
    const int frames=audio.getNumSamples();
    if(audio.getNumChannels()!=2||frames<1||frames>65536){observedMidi->beginBlock({}, {},0,renderProject.mode,observedNow);observedMidi->endBlock();audio.clear();hostBpm.store(0,std::memory_order_relaxed);playbackFeedback.store(PatternPlaybackStatus::invalidBlock);invalidPatternBlock|=renderProject.mode==PlaybackMode::internalPattern;return;}
    const auto left=reinterpret_cast<std::uintptr_t>(audio.getWritePointer(0)),right=reinterpret_cast<std::uintptr_t>(audio.getWritePointer(1));
    if((left>right?left-right:right-left)<static_cast<std::uintptr_t>(frames)*sizeof(float)){observedMidi->beginBlock({}, {},0,renderProject.mode,observedNow);observedMidi->endBlock();audio.clear();hostBpm.store(0,std::memory_order_relaxed);playbackFeedback.store(PatternPlaybackStatus::invalidBlock);invalidPatternBlock|=renderProject.mode==PlaybackMode::internalPattern;return;}
    mcpAudioValidUntil.store(observedNow+250+static_cast<std::uint64_t>(std::ceil(1000.*frames/preparedRate)),std::memory_order_release);
    HostPatternTiming origin;bool positionAvailable=false;std::optional<double> tempo;std::uint32_t meter=0;std::optional<PhraseMeter> observedMeter;
    if(auto* playhead=getPlayHead())if(const auto position=playhead->getPosition())
    {
        positionAvailable=true;origin.playing=position->getIsPlaying();origin.looping=position->getIsLooping();
        if(auto v=position->getBpm())origin.bpm=*v;
        if(auto v=position->getPpqPosition())origin.ppqPosition=*v;
        if(auto v=position->getTimeInSamples())origin.samplePosition=*v;
        if(auto v=position->getLoopPoints())origin.loop=PatternLoopRange{v->ppqStart,v->ppqEnd};
        if(auto v=position->getTimeSignature())
        {
            observedMeter=PhraseMeter{0,0};
            if(v->numerator>0&&v->numerator<=255&&v->denominator>0&&v->denominator<=255)
            {PhraseMeter m{static_cast<std::uint8_t>(v->numerator),static_cast<std::uint8_t>(v->denominator)};if(validatePhraseMeter(m)){observedMeter=m;meter=(std::uint32_t(m.numerator)<<8)|m.denominator;}}
        }
    }
    hostMeter.store(meter);
    if(origin.bpm&&std::isfinite(*origin.bpm)&&*origin.bpm>=20&&*origin.bpm<=400)tempo=origin.bpm;
    hostBpm.store(tempo.value_or(0),std::memory_order_relaxed);
    // Advancing or wrapping a later chunk must never repair an invalid
    // coordinate supplied for this callback. Leave it for scheduler rejection.
    const bool admittedOrigin = origin.ppqPosition
        && std::isfinite(*origin.ppqPosition)
        && std::abs(*origin.ppqPosition) <= 16777216.;
    performance->beginDrumCallback();
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::afterDrumCutoff);
#endif
    const auto drumPresses=performance->drumPresses();std::array<bool,64> admittedDrums{};
    AudioProjectState d{.sound={.values={}}};DrumAudioState drumAudio;std::uint64_t serial,edit,reset,restart,modeReset;bool modeChanged=false,restartPattern=invalidPatternBlock,replayDeferredReset=false;
    if(readAudioProject(d,serial,edit,reset,restart,&modeReset,&drumAudio))
    {
#ifndef NDEBUG
        drumTestPoint(DrumTestPoint::afterDrumAuthority);
#endif
        for(std::size_t n=0;n<drumPresses.size();++n){const auto& press=drumPresses[n];
            if(performance->drumPressExpired(n)||press.hostOffset>=static_cast<std::uint32_t>(frames)){performance->reportDrumStatus(DrumPerformanceStatus::missedOffset);continue;}
            if(press.bank>=4||press.pad>=16||press.velocity<1||press.velocity>127||press.channel<1||press.channel>16){performance->reportDrumStatus(DrumPerformanceStatus::invalid);continue;}
            if(drumAudio.source!=drumAudio.acknowledgedSource||!press.token||press.token!=drumAudio.identity.targets[press.bank*16+press.pad]
                ||d.sound.layout.synthUse[press.bank]!=SlotUse::present||d.sound.values[drumModeParameterStart+press.bank]<.5f){performance->reportDrumStatus(DrumPerformanceStatus::staleTarget);continue;}
            admittedDrums[n]=true;
        }
        performance->consumeDrumCallback();
        // Always read raw host values. Reuse only a successfully installed,
        // exactly equal sound; editRevision alone does not track automation.
        bool usable=renderSoundCurrent&&d.sound==renderProject.sound&&reset==renderResetGeneration&&drumAudio.identity==renderDrumIdentity;
        if(!usable){RoutedPatch patch;if(makeRoutedPatch(d.sound,patch)){
            for(std::size_t slot=0;slot<8;++slot){auto& eq=patch.effects[slot].dynamicEq;const auto effective=engine.effectiveEqMode(slot);
                if(eq.processingMode!=effective){eq.processingMode=effective;
                    if(!validateDynamicEq(eq))eq=engine.patch().effects[slot].dynamicEq;
                }
            }
            BorrowedDrumAssignments assignments;
            for(std::size_t bank=0;bank<4;++bank)for(std::size_t pad=0;pad<16;++pad)assignments[bank][pad]=drumAudio.samples[bank][pad];
            renderSoundCurrent=engine.setPatch(patch,assignments,drumAudio.identity,reset);usable=renderSoundCurrent;
            pendingEqRouting.store(!renderSoundCurrent,std::memory_order_relaxed);
        }}
        if(!usable&&reset!=renderResetGeneration)performance->deferReset(reset,d.mode==PlaybackMode::externalMidi);
        if(usable){
            modeChanged=modeReset!=renderModeGeneration;
            restartPattern|=restart!=renderPatternGeneration;
            if(reset!=renderResetGeneration){engine.reset();observedMidi->prepare(preparedRate);performance->clearOwners(true);performance->scheduler.release(performance->patternEvents);renderResetGeneration=reset;replayDeferredReset=true;}
            else performance->cancelDeferredReset();
            renderProject=d;renderDrumIdentity=drumAudio.identity;renderPatternGeneration=restart;renderModeGeneration=modeReset;
        }
    }else performance->deferDrumCallback();
    // A mode transition releases with tails even when a stop arrives in the
    // same callback. Only continuous External mode retains legacy stop reset.
    if(positionAvailable){if(!modeChanged&&renderProject.mode==PlaybackMode::externalMidi&&wasPlaying&&!origin.playing){engine.reset();performance->clearOwners();}wasPlaying=origin.playing;}
    tailSeconds.store(engine.spaceTailSeconds()+tailEstimate(engine.patch(),tempo,preparedRate*(engine.effectiveQuality()==RenderQuality::higher?2:1),preparedFrequencyShiftTail,preparedFrequencySplitTail,preparedMidSideSplitTail,engine.sourceTailSeconds())+2.0*engine.latencySamples()/preparedRate);performance->beginCallback();
    if(observedMidi->beginBlock(origin,observedMeter,frames,renderProject.mode,observedNow,positionAvailable))
    {
        std::size_t received=0;
        for(auto event:midi)
        {
            observedMidi->event({event.data,static_cast<std::size_t>(event.numBytes)},event.samplePosition);
            if(++received>maximumObservedMidiEvents)break;
        }
    }
    auto input=midi.cbegin();const auto end=midi.cend();bool suppress=false,suppressDrums=false,anyMissingTempo=false;
    const bool externalAvailable=getBus(true,0)->isEnabled();
    std::array<DynamicEq::Frame,512> externalDetectorScratch;
    for(int offset=0;offset<frames;offset+=preparedBlock)
    {
        const int size=std::min(preparedBlock,frames-offset);performance->beginChunk();
        if(suppressDrums)performance->suppressDrums(false);
        if(offset==0)
        {
            if(modeChanged||restartPattern)performance->requestDrumStop();
            if(modeChanged)performance->releaseAll();
            if(modeChanged||restartPattern){performance->scheduler.release(performance->patternEvents);performance->stagePattern(performance->patternEvents,0);}
            performance->scheduler.setPattern(renderProject.pattern);
            if(replayDeferredReset)performance->stageDeferredReset(renderResetGeneration,renderProject.mode==PlaybackMode::externalMidi);
            performance->stageKeyboard();invalidPatternBlock=false;
        }
        auto timing=origin;
        if(offset>0&&admittedOrigin&&tempo)
        {
            timing.ppqPosition=*origin.ppqPosition+offset* *tempo/(60*preparedRate);
            if(origin.looping&&origin.loop&&std::isfinite(origin.loop->startBeats)&&std::isfinite(origin.loop->endBeats)
                &&std::abs(origin.loop->startBeats)<=16777216.&&std::abs(origin.loop->endBeats)<=16777216.
                &&origin.loop->endBeats>origin.loop->startBeats&&(origin.loop->endBeats-origin.loop->startBeats)*60*preparedRate/ *tempo>=1
                &&std::isfinite(*origin.ppqPosition)&&std::abs(*origin.ppqPosition)<=16777216.&&*timing.ppqPosition>=origin.loop->endBeats)
                timing.ppqPosition=origin.loop->startBeats+std::fmod(*timing.ppqPosition-origin.loop->startBeats,origin.loop->endBeats-origin.loop->startBeats);
        }
        if(origin.samplePosition)
        {
            if(*origin.samplePosition>std::numeric_limits<std::int64_t>::max()-offset-size)timing.ppqPosition=std::numeric_limits<double>::quiet_NaN();
            else timing.samplePosition=*origin.samplePosition+offset;
        }
        if(!suppress)
        {
            auto status=performance->scheduler.processBlock(timing,size,renderProject.mode,performance->patternEvents);
            if(!positionAvailable&&renderProject.mode==PlaybackMode::internalPattern)status=PatternPlaybackStatus::missingTiming;
            if(offset==0&&status==PatternPlaybackStatus::playing&&timing.ppqPosition){const auto beat=*timing.ppqPosition,length=patternLengthBeats(renderProject.pattern);playbackPosition.store(beat-std::floor(beat/length)*length,std::memory_order_relaxed);}
            playbackFeedback.store(status);performance->stagePattern(performance->patternEvents);
            // Scheduler overflow abandons drums atomically, preserving melodic owners and bytes.
            if(status==PatternPlaybackStatus::eventCapacity){performance->suppressDrums(!suppressDrums);suppressDrums=true;}
            else if(performance->patternEvents.drumStopAtStart)performance->requestDrumStop();
        }
        while(input!=end&&(*input).samplePosition<offset+size)
        {
            const auto event=*input;++input;
            if(!suppress&&event.samplePosition>=offset)performance->stageHost(event,event.samplePosition-offset,renderProject.mode!=PlaybackMode::externalMidi);
        }
        if(!suppress&&!suppressDrums)for(std::size_t n=0;n<drumPresses.size();++n)if(admittedDrums[n]&&drumPresses[n].hostOffset>=static_cast<std::uint32_t>(offset)&&drumPresses[n].hostOffset<static_cast<std::uint32_t>(offset+size))
            performance->stageExactDrum(drumPresses[n],static_cast<int>(drumPresses[n].hostOffset)-offset);
        if(!performance->commit()){suppress=true;performance->scheduler.release(performance->patternEvents);}
        // The sole optional aux aliases the stereo output. Copy each bounded
        // chunk before render clears it; earlier chunks cannot touch later input.
        if(externalAvailable)for(int n=0;n<size;++n)externalDetectorScratch[std::size_t(n)]={audio.getSample(0,offset+n),audio.getSample(1,offset+n)};
        std::array<float*,2> channels{audio.getWritePointer(0,offset),audio.getWritePointer(1,offset)};
        juce::AudioBuffer<float> view(channels.data(),2,size);
        if(externalAvailable)engine.render(view,performance->midi(),tempo,std::span<const DynamicEq::Frame>(externalDetectorScratch).first(std::size_t(size)),performance->drumEvents());
        else engine.render(view,performance->midi(),tempo,{},performance->drumEvents());
        if(engine.drumDispatchUnavailable())performance->reportDrumStatus(DrumPerformanceStatus::unavailable);
        anyMissingTempo|=engine.tempoUnavailable();
    }
    missingTempo.store(anyMissingTempo);
    observedMidi->endBlock(!suppress);
    renderedPreview.mix(audio,editRevision.load(std::memory_order_acquire));
    publishDrumFeedback();
}bool InstrumentProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    return l.inputBuses.size()==1&&l.outputBuses.size()==1
        &&(l.inputBuses[0].isDisabled()||l.inputBuses[0]==juce::AudioChannelSet::stereo())
        &&l.outputBuses[0]==juce::AudioChannelSet::stereo();
}
juce::AudioProcessorEditor* InstrumentProcessor::createEditor() { return new InstrumentEditor(*this); }
}



namespace airack {
void InstrumentProcessor::AtomicPattern::store(const ProjectDocument& d) noexcept {
    const auto& p=d.pattern;numerator.store(p.meter.numerator);denominator.store(p.meter.denominator);bars.store(p.bars);grid.store(static_cast<std::uint8_t>(p.grid));swing.store(p.swing);count.store(p.noteCount);mode.store(static_cast<std::uint8_t>(d.mode));
    for(std::size_t i=0;i<p.noteCount;++i){notes[i].start.store(p.notes[i].startBeats);notes[i].duration.store(p.notes[i].durationBeats);notes[i].pitch.store(p.notes[i].pitch);notes[i].velocity.store(p.notes[i].velocity);notes[i].channel.store(p.notes[i].channel);}
}
void InstrumentProcessor::AtomicPattern::load(AudioProjectState& d) const noexcept {
    auto& p=d.pattern;p={};p.meter={numerator.load(),denominator.load()};p.bars=bars.load();p.grid=static_cast<PatternGrid>(grid.load());p.swing=swing.load();p.noteCount=std::min<std::uint16_t>(count.load(),maximumPatternNotes);d.mode=static_cast<PlaybackMode>(mode.load());
    for(std::size_t i=0;i<p.noteCount;++i)p.notes[i]={notes[i].start.load(),notes[i].duration.load(),notes[i].pitch.load(),notes[i].velocity.load(),notes[i].channel.load()};
}
bool InstrumentProcessor::tryProjectSnapshotOwned(ProjectDocument& d,std::uint64_t& edit) const noexcept {std::uint64_t serial,reset,restart;return readProject(d,serial,edit,reset,restart);}
bool InstrumentProcessor::tryProjectSnapshotOwned(ProjectDocument& d,std::uint64_t& edit,std::uint64_t& reset) const noexcept {std::uint64_t serial,restart;return readProject(d,serial,edit,reset,restart);}
bool InstrumentProcessor::tryProjectSnapshot(ProjectDocument& d,std::uint64_t& edit) const noexcept
{try{const std::lock_guard lock(restoreMutex);return tryProjectSnapshotOwned(d,edit);}catch(...){return false;}}
bool InstrumentProcessor::tryProjectSnapshot(ProjectDocument& d,std::uint64_t& edit,std::uint64_t& reset) const noexcept
{try{const std::lock_guard lock(restoreMutex);return tryProjectSnapshotOwned(d,edit,reset);}catch(...){return false;}}
bool InstrumentProcessor::trySoundSnapshot(SoundDocument& d,std::uint64_t& edit) const noexcept
{try{const std::lock_guard lock(restoreMutex);return trySoundSnapshotOwned(d,edit);}catch(...){return false;}}
bool InstrumentProcessor::trySoundSnapshot(SoundDocument& d,std::uint64_t& edit,std::uint64_t& reset) const noexcept
{try{const std::lock_guard lock(restoreMutex);return trySoundSnapshotOwned(d,edit,reset);}catch(...){return false;}}
RackCandidateStatus InstrumentProcessor::rackOperationsOwned(std::span<const RackOperation> operations,std::uint64_t expected,SoundDocument& output,RackDifference& difference,SourceAssignmentTrace* trace)
{
    if(operations.empty()||operations.size()>12)return RackCandidateStatus::invalid;
    SoundDocument before;std::uint64_t edit;
    if(gesture->index!=parameterCount||!trySoundSnapshotOwned(before,edit)||edit!=expected)return RackCandidateStatus::stale;
    const auto structural=structuralGeneration.load();auto staged=before;RackDifference combined;
    std::unique_ptr<SourceAssignmentTrace> journal;if(trace)journal=std::make_unique<SourceAssignmentTrace>(*trace);
    for(const auto& operation:operations){SoundDocument next;RackDifference diff;const auto status=makeRackCandidate(staged,operation,SoundEditOrigin::manual,next,diff,journal.get());if(status!=RackCandidateStatus::ready)return status;staged=std::move(next);
        const auto append=[](juce::String& a,const juce::String& b){if(b.isNotEmpty()){if(a.isNotEmpty())a+="; ";a+=b;}};
        append(combined.sonic,diff.sonic);append(combined.presentation,diff.presentation);append(combined.structural,diff.structural);append(combined.automationWarning,diff.automationWarning);
    }
    if(structural!=structuralGeneration.load())return RackCandidateStatus::stale;
    output=std::move(staged);difference=std::move(combined);if(trace)*trace=*journal;return RackCandidateStatus::ready;
}
RackCandidateStatus InstrumentProcessor::previewRackOperations(std::span<const RackOperation> operations,std::uint64_t expected,SoundDocument& output,RackDifference& difference)
{try{const std::lock_guard lock(restoreMutex);return rackOperationsOwned(operations,expected,output,difference);}catch(...){return RackCandidateStatus::invalid;}}
RackCandidateStatus InstrumentProcessor::previewRackOperation(const RackOperation& operation,std::uint64_t expected,SoundDocument& output,RackDifference& difference)
{return previewRackOperations(std::span<const RackOperation>(&operation,1),expected,output,difference);}
RackCandidateStatus InstrumentProcessor::applyRackOperations(std::span<const RackOperation> operations,std::uint64_t expected,
    std::optional<std::uint64_t> expectedReset,const RackLibraryPlacementPreparationPtr& preparation,
    const std::shared_ptr<const RackLibraryPlacementView>& authority)
{
    try {
        const bool guarded=expectedReset.has_value()||preparation||authority;
        std::vector<const RackOperation*> embeddedOnly;
        if(guarded){
            if(!expectedReset||!preparation||!authority||!authority->root||operations.empty()
                ||libraryRuntime->client->placement(preparation)!=authority
                ||!libraryRuntime->client->placementCurrent(preparation,*authority))return RackCandidateStatus::stale;
            std::vector<RackPackageRef> used;
            for(const auto& op:operations){
                if((op.kind!=RackOperationKind::place&&op.kind!=RackOperationKind::swap)||!op.design)return RackCandidateStatus::invalid;
                juce::MemoryBlock canonical;
                if(!encodeRackDesign(*op.design,canonical,RackValidationMode::instanceRecall))return RackCandidateStatus::invalid;
                const auto hash=juce::SHA256(canonical).toHexString();RackDesign resolved;std::optional<RackPackageRef> package;
                if(!resolveInventoryDesign(*authority->root,op.design->id,op.design->revision,hash,resolved,package)
                    ||resolved!=*op.design)return RackCandidateStatus::stale;
                if(package!=op.package){if(op.package){embeddedOnly.push_back(&op);continue;}return RackCandidateStatus::stale;}
                if(!op.package)continue;
                const auto lease=std::find_if(authority->leases.begin(),authority->leases.end(),[&](const auto& value){return value&&value->manifest().ref==*op.package;});
                if(lease==authority->leases.end()){embeddedOnly.push_back(&op);continue;}
                const auto bytes=(*lease)->canonicalRecipe();
                if(bytes.size()!=canonical.getSize()||std::memcmp(bytes.data(),canonical.getData(),bytes.size())!=0)return RackCandidateStatus::stale;
                if(std::find(used.begin(),used.end(),*op.package)==used.end())used.push_back(*op.package);
            }
            if(used.size()!=authority->leases.size())return RackCandidateStatus::invalid;
        }
        {const std::lock_guard lock(restoreMutex);SoundDocument next;RackDifference diff;auto trace=std::make_unique<SourceAssignmentTrace>();
            if(guarded){
                SoundDocument live;std::uint64_t edit,reset;
                if(!trySoundSnapshotOwned(live,edit,reset)||edit!=expected||reset!=*expectedReset
                    ||!libraryRuntime->client->placementCurrent(preparation,*authority))return RackCandidateStatus::stale;
                for(const auto* op:embeddedOnly)
                    if(!live.personalities||std::none_of(live.personalities->instances.begin(),live.personalities->instances.end(),[&](const auto& rack){return rack.design==*op->design&&rack.package==op->package;}))return RackCandidateStatus::stale;
            }
            const auto status=rackOperationsOwned(operations,expected,next,diff,trace.get());if(status!=RackCandidateStatus::ready)return status;
            SoundDocument before;std::uint64_t edit;if(!trySoundSnapshotOwned(before,edit)||edit!=expected)return RackCandidateStatus::stale;
            if(guarded&&(resetGeneration.load()!=*expectedReset||!libraryRuntime->client->placementCurrent(preparation,*authority)))return RackCandidateStatus::stale;
            if(trace->count){auto a=std::make_unique<ProjectDocument>(*recoveryDocument),b=std::make_unique<ProjectDocument>(*recoveryDocument);a->sound=before;b->sound=next;
                if(!performOwnedRackEdit(*a,*b,*trace,drumCommitStatus))return RackCandidateStatus::invalid;}
            else if(before!=next&&!performEdit(before,next))return RackCandidateStatus::invalid;}
        triggerAsyncUpdate();return RackCandidateStatus::ready;
    }catch(...){return RackCandidateStatus::invalid;}
}
RackCandidateStatus InstrumentProcessor::applyRackOperation(const RackOperation& operation,std::uint64_t expected)
{return applyRackOperations(std::span<const RackOperation>(&operation,1),expected);}
bool InstrumentProcessor::applyPatternEdit(const PatternDocument& pattern,std::uint64_t expected) {
    if(!validatePatternDocument(pattern))return false;
    {const std::lock_guard lock(restoreMutex);ProjectDocument before;std::uint64_t edit;
    if(gesture->index!=parameterCount||!tryProjectSnapshotOwned(before,edit)||edit!=expected)return false;
    auto after=validateProjectDocument(before)?before:*recoveryDocument;after.pattern=pattern;
    if(before==after)return true;if(!performProjectEdit(before,after))return false;}
    triggerAsyncUpdate();return true;
}
bool InstrumentProcessor::setPlaybackMode(PlaybackMode mode,std::uint64_t expected) {
    if(mode!=PlaybackMode::externalMidi&&mode!=PlaybackMode::internalPattern)return false;
    {const std::lock_guard lock(restoreMutex);ProjectDocument before;std::uint64_t edit;
    if(gesture->index!=parameterCount||!tryProjectSnapshotOwned(before,edit)||edit!=expected)return false;
    auto after=validateProjectDocument(before)?before:*recoveryDocument;after.mode=mode;
    if(before==after)return true;if(!performProjectEdit(before,after))return false;}
    triggerAsyncUpdate();return true;
}
std::optional<PhraseMeter> InstrumentProcessor::currentHostMeter() const noexcept {const auto v=hostMeter.load();if(v==0)return {};return PhraseMeter{static_cast<std::uint8_t>(v>>8),static_cast<std::uint8_t>(v&255)};}
bool InstrumentProcessor::createPattern(std::uint8_t bars,std::uint64_t expected) {PatternDocument d;d.meter=currentHostMeter().value_or(PhraseMeter{});d.bars=bars;return applyPatternEdit(d,expected);}
bool InstrumentProcessor::savePattern(const juce::File& target) {ProjectDocument d;std::uint64_t edit;{const std::lock_guard lock(restoreMutex);if(!tryProjectSnapshotOwned(d,edit))return false;}return savePatternAsset(d.pattern,target);}
bool InstrumentProcessor::exportPattern(const juce::File& target) {ProjectDocument d;std::uint64_t edit;{const std::lock_guard lock(restoreMutex);if(!tryProjectSnapshotOwned(d,edit))return false;}return exportPatternMidi(d.pattern,target);}
bool InstrumentProcessor::loadPattern(const juce::File& file) {
    PatternDocument pattern;if(!loadPatternAsset(file,pattern))return false;
    {const std::lock_guard lock(restoreMutex);ProjectDocument before;std::uint64_t edit;
    if(gesture->index!=parameterCount||!tryProjectSnapshotOwned(before,edit))return false;
    auto after=validateProjectDocument(before)?before:*recoveryDocument;after.pattern=pattern;
    if(before==after){if(!publish(after,false,true,true))return false;}
    else if(!performProjectEdit(before,after))return false;}
    triggerAsyncUpdate();return true;
}
PatternPlaybackStatus InstrumentProcessor::patternPlaybackStatus() const noexcept {return playbackFeedback.load();}
std::optional<double> InstrumentProcessor::patternPlaybackPosition() const noexcept {if(playbackFeedback.load()!=PatternPlaybackStatus::playing)return std::nullopt;return playbackPosition.load(std::memory_order_relaxed);}
PerformanceInputStatus InstrumentProcessor::performanceInputStatus() const noexcept {return performance->status();}
bool InstrumentProcessor::enqueueKeyboardNote(bool on,std::uint8_t pitch,std::uint8_t velocity,std::uint8_t channel) noexcept {return performance->enqueue(on,pitch,velocity,channel);}
void InstrumentProcessor::releaseKeyboardNotes() noexcept {performance->cancelKeyboard();}
}

namespace airack { double InstrumentProcessor::getTailLengthSeconds() const
{return tailSeconds.load(std::memory_order_seq_cst)+fxMonitoring->auditionTailSeconds();} }
